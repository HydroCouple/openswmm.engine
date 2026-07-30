/**
 * @file SurfaceRouter2D.cpp
 * @brief Implementation of the 2D surface routing orchestrator.
 *
 * @see SurfaceRouter2D.hpp
 * @ingroup engine_2d
 */

#include "SurfaceRouter2D.hpp"
#include "mesh/MeshBuilder.hpp"
#include "mesh/VertexReconstruction.hpp"
#include "mesh/VfrClosure.hpp"
#include "solver/SurfaceFluxCalculator.hpp"
#include "solver/ExplicitInertialSolver.hpp"
#ifdef OPENSWMM_HAS_2D
#include "solver/SurfaceSolverFactory.hpp"
#endif
#include "../core/SimulationContext.hpp"
#include "../core/UnitConversion.hpp"
#include "../core/PerfTimers.hpp"

#include <stdexcept>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <unordered_map>
#include <unordered_set>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#if defined(SWMM_USE_OPENMP)
#include <omp.h>
#else
static inline int omp_get_max_threads() { return 1; }
#endif

namespace openswmm::twoD {

namespace {

void refreshOutputGradients(const MeshData& mesh, SurfaceStateData& state,
                            const SolverOptions2D& opts) {
    if (!opts.report_2d) return;
    computeUnlimitedGradients(mesh, state, opts.num_threads);
    computeLimitedGradients(mesh, state, opts.limiter_epsilon,
                            opts.num_threads);
}

// Free surface of cell i at MEAN depth d under the configured closure —
// mirrors the solvers' reconstructFromVolume (FLAT: tri_cz + d; VFR: the
// regularized planar-bed inverse). d == 0 gives the closure's dry head, which
// under VFR is the η(V=0) anchor the solver's head→volume seeding maps back
// to exactly zero volume.
double headFromMeanDepth(const MeshData& mesh, const SolverOptions2D& opts,
                         int i, double d) {
    if (opts.cell_closure == CellClosure2D::VFR) {
        double z1 = mesh.vz[mesh.tri_v0[i]];
        double z2 = mesh.vz[mesh.tri_v1[i]];
        double z3 = mesh.vz[mesh.tri_v2[i]];
        vfrSort3(z1, z2, z3);
        return vfrEtaFromMeanDepth(z1, z2, z3, d, opts.vfr_min_wet_frac);
    }
    return mesh.tri_cz[i] + d;
}

} // namespace

void SurfaceRouter2D::drainPendingRows() {
    if (mesh_.n_triangles() < 1) return;             // nothing to drain into

    // Initialize per-edge boundary condition storage (n_triangles * 3 slots,
    // all initialized to WALL with zero head/slope/cum_flux).
    boundary_.resize(mesh_.n_triangles() * 3);

    // V-E3 — drain any [2D_BOUNDARY_CONDITIONS] rows the parser accumulated
    // into boundary_. Out-of-range rows are silently skipped — defensive
    // against partial INPs.
    {
        const int n_edges = boundary_.size();
        for (const auto& r : pending_bc_rows_) {
            if (r.tri < 0 || r.tri >= mesh_.n_triangles()) continue;
            if (r.edge < 0 || r.edge > 2) continue;
            const int idx = r.tri * 3 + r.edge;
            if (idx < 0 || idx >= n_edges) continue;
            boundary_.edge_bc_type[idx] = static_cast<int8_t>(r.bc_type);
            switch (static_cast<BoundaryType>(r.bc_type)) {
            case BoundaryType::NORMAL_FLOW:
                boundary_.edge_bed_slope[idx] = r.param1;
                break;
            case BoundaryType::SPECIFIED_STAGE:
                if (!r.name.empty()) {
                    boundary_.edge_bc_tseries_name[idx] = r.name;
                    boundary_.edge_bc_tseries[idx]      = -2;  // deferred resolve
                } else {
                    boundary_.edge_bc_head[idx] = r.param1;
                }
                break;
            case BoundaryType::SPECIFIED_FLOW:
                if (!r.name.empty()) {
                    boundary_.edge_bc_flow_tseries_name[idx] = r.name;
                    boundary_.edge_bc_flow_tseries[idx]      = -2;
                } else {
                    boundary_.edge_bc_flow[idx] = r.param1;
                }
                break;
            case BoundaryType::RATING_CURVE:
                boundary_.edge_bc_rating_curve_name[idx] = r.name;
                boundary_.edge_bc_rating_curve[idx]      = -2;
                break;
            case BoundaryType::WALL:
                break;
            }
        }
        // pending_bc_rows_ NOT cleared: retained so InpWriter / GeoPackage can
        // serialize the authored rows (group label, TS-vs-constant choice,
        // authored NORMAL_FLOW slope=0 sentinel are unrecoverable from
        // boundary_). Re-draining is idempotent (resize re-defaults first).
    }

    // §11A — drain [2D_EDGE_CONVEYANCE] rows. Build a one-shot vertex-pair ->
    // list-of-(tri*3+edge_local) slot map, then write each parsed factor into
    // every matching slot (naturally mirroring interior edges).
    {
        struct EdgeKey {
            std::int64_t packed;  ///< (min_v << 32) | max_v
            bool operator==(EdgeKey o) const noexcept { return packed == o.packed; }
        };
        struct EdgeKeyHash {
            std::size_t operator()(EdgeKey k) const noexcept {
                return std::hash<std::int64_t>{}(k.packed);
            }
        };
        auto makeKey = [](int va, int vb) -> EdgeKey {
            const std::int64_t lo = std::min(va, vb);
            const std::int64_t hi = std::max(va, vb);
            return EdgeKey{ (lo << 32) | (hi & 0xFFFFFFFFLL) };
        };

        std::unordered_map<EdgeKey, std::vector<int>, EdgeKeyHash> edge_key_to_slots;
        edge_key_to_slots.reserve(static_cast<std::size_t>(mesh_.n_triangles()) * 3);

        const int nt = mesh_.n_triangles();
        for (int t = 0; t < nt; ++t) {
            const int v[3] = { mesh_.tri_v0[t], mesh_.tri_v1[t], mesh_.tri_v2[t] };
            for (int e = 0; e < 3; ++e) {
                const int va = v[(e + 1) % 3];
                const int vb = v[(e + 2) % 3];
                edge_key_to_slots[makeKey(va, vb)].push_back(t * 3 + e);
            }
        }

        for (const auto& r : pending_edge_conveyance_rows_) {
            if (r.v_from < 0 || r.v_from >= mesh_.n_vertices() ||
                r.v_to   < 0 || r.v_to   >= mesh_.n_vertices()) {
                throw std::runtime_error(
                    "[2D_EDGE_CONVEYANCE]: vertex index out of range ("
                    + std::to_string(r.v_from) + ", " + std::to_string(r.v_to)
                    + "); n_vertices = " + std::to_string(mesh_.n_vertices()));
            }
            auto it = edge_key_to_slots.find(makeKey(r.v_from, r.v_to));
            if (it == edge_key_to_slots.end()) {
                throw std::runtime_error(
                    "[2D_EDGE_CONVEYANCE]: vertices ("
                    + std::to_string(r.v_from) + ", " + std::to_string(r.v_to)
                    + ") do not form a mesh edge");
            }
            for (const int slot : it->second) {
                mesh_.edge_conveyance[slot] = r.conveyance;
            }
        }
    }

    // From here on the drained arrays are the live state (API mutators edit
    // them, not the pending rows) — tell the serialization collectors to read
    // the arrays instead of the now-stale rows.
    options_.pending_rows_drained = true;
}

void SurfaceRouter2D::prepareForEdit() {
    // Make the parsed mesh editable in OPENED (not INITIALIZED) state: size
    // BoundaryData and move the authored BC / conveyance rows into the live
    // arrays so per-edge API edits take effect and serialize on save. Guarded
    // so it never re-drains over edits already made (initialize() re-defaults;
    // this must not).
    if (!options_.pending_rows_drained) drainPendingRows();
}

void SurfaceRouter2D::initialize(SimulationContext& ctx) {
    // Check if 2D sections were parsed (vertices present)
    if (mesh_.n_vertices() < 3 || mesh_.n_triangles() < 1) {
        active_ = false;
        return;
    }

    // IGNORE_2D: the module toggle. The mesh stays parsed/editable, but the
    // solver never activates — the model runs 1D-only.
    if (ctx.options.ignore_2d) {
        std::fprintf(stderr,
                     "[openswmm 2D] IGNORE_2D YES — 2D surface routing "
                     "disabled; running 1D-only.\n");
        active_ = false;
        return;
    }

    // Resolve unit-system conversion factors. The 2D solver runs internally in
    // SI, but the 1D engine ALWAYS computes internally in feet (g=32.2,
    // PHI=1.486) — even for SI/metric FLOW_UNITS, whose metric inputs the 1D
    // reader converts to feet on load and only converts back at the display
    // boundary. So the 1D⇄2D coupling ALWAYS converts feet⇄metres, regardless
    // of FLOW_UNITS. (These factors were previously tied to FLOW_UNITS and
    // collapsed to 1.0 for SI projects, leaving every coupled head/depth off
    // by 3.28× and every exchanged flow/volume off by 35× — corrupting the
    // coupled mass balance. They describe the 1D side, which is always feet.)
    constexpr double ft_to_m = 0.3048;
    options_.len_1d_to_2d  = ft_to_m;
    options_.len_2d_to_1d  = 1.0 / ft_to_m;
    options_.vol_1d_to_2d  = ft_to_m * ft_to_m * ft_to_m;
    options_.flow_1d_to_2d = options_.vol_1d_to_2d;
    options_.flow_2d_to_1d = 1.0 / options_.vol_1d_to_2d;

    // The MESH, by contrast, is authored in the project's display length units
    // (feet for US, metres for SI), so its scaling to the SI solver IS driven
    // by FLOW_UNITS: US → ft→m (0.3048); SI → already metres (no-op).
    const int us = ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));
    const double mesh_to_si = (us == 0) ? ft_to_m : 1.0;

    // Convert mesh geometry from project length units (feet for US) to the SI
    // internal units the 2D solver expects. MUST run BEFORE buildMeshTopology
    // so all derived geometry (areas, edge lengths, centroids, midpoint Z) is
    // computed in SI. No-op for SI projects (factor 1.0). Coupling areas given
    // in the .inp are project-length² and scale by the squared factor.
    //
    // Skipped entirely when the producer declared `;; UNITS: SI (m)` on the
    // mesh file (options_.mesh_units_si == true): the values are already SI
    // and applying the factor a second time would scale the mesh down by
    // 0.3048 on US-FLOW_UNITS projects.  Driven by mesh_to_si (FLOW_UNITS),
    // NOT the coupling factors above, which describe the always-feet 1D side.
    if (!options_.mesh_units_si && !options_.mesh_scaled_to_si &&
        mesh_to_si != 1.0) {
        const double f  = mesh_to_si;
        const double f2 = f * f;
        for (auto& v : mesh_.vx) v *= f;
        for (auto& v : mesh_.vy) v *= f;
        for (auto& v : mesh_.vz) v *= f;
        for (auto& a : mesh_.vert_coupling_area) a *= f2;
        for (auto& a : mesh_.tri_coupling_area)  a *= f2;
        for (auto& r : mesh_.tri_couplings)      r.area *= f2;
        options_.mesh_scaled_to_si = true;
    }

    // Build mesh topology (neighbours, edge geometry, areas)
    buildMeshTopology(mesh_);

    // Validate mesh
    auto err = validateMesh(mesh_);
    if (!err.empty()) {
        throw std::runtime_error("2D mesh validation failed: " + err);
    }

    // Build pseudo-Laplacian vertex reconstruction stencils
    buildVertexStencils(mesh_);

    // Fold the OPENSWMM_2D_RAINFALL_MODE env override into options_ (outside the
    // OPENSWMM_HAS_2D block — rainfall is applied whether or not the CVODE solver
    // is compiled). Mirrors the OPENSWMM_2D_MOMENTUM override below.
    if (const char* rm = std::getenv("OPENSWMM_2D_RAINFALL_MODE")) {
        if (std::strcmp(rm, "system") == 0)
            options_.rainfall_mode = RainfallMode::SYSTEM;
        else if (std::strcmp(rm, "natural") == 0)
            options_.rainfall_mode = RainfallMode::NATURAL_NEIGHBOUR;
        else if (std::strcmp(rm, "none") == 0)
            options_.rainfall_mode = RainfallMode::NONE;
    }

    // Fold the remaining env override into options_ per run (used to be a
    // function-local static cache — process-lifetime, wrong for multi-model /
    // library use). Behavior-neutral when the env var is unset.
    if (const char* s = std::getenv("OPENSWMM_2D_FLUX_DH_EPS")) {
        const double v = std::atof(s);
        if (v >= 0.0) options_.flux_dh_eps = v;
    }

    // Precompute the static rainfall-interpolation weights. Gage POSITIONS are
    // fixed for the run, so the natural-neighbour / IDW weights are built once
    // here and only the per-step gage VALUES vary (see updateRainfall). The
    // [SYMBOLS] gage coords are in project map units; mesh_to_si brings them into
    // the SI mesh frame the centroids now live in. Rebuilds cleanly, so a
    // repeated initialize() is idempotent.
    interp_.build(mesh_.tri_cx, mesh_.tri_cy, ctx.spatial.gage_x, ctx.spatial.gage_y,
                  ctx.n_gages(), mesh_to_si);

    // Resolve deferred coupling node names → indices
    for (int v = 0; v < mesh_.n_vertices(); ++v) {
        auto& name = mesh_.vert_coupled_node_name[v];
        if (name.empty()) continue;

        int node_idx = ctx.node_names.find(name);
        if (node_idx >= 0) {
            mesh_.vert_coupled_node[v] = node_idx;
        } else {
            throw std::runtime_error(
                "2D vertex " + std::to_string(v)
                + " coupled to unknown node '" + name + "'");
        }
    }

    // Cell couplings — the row vector is the source of truth (repeated-row
    // [2D_TRIANGLE_NODE_MAP]; several nodes may share one triangle). Paths
    // that still author only the legacy per-triangle arrays (GeoPackage
    // reader, direct writes) are folded in by synthesising rows first.
    if (mesh_.tri_couplings.empty()) {
        for (int t = 0; t < mesh_.n_triangles(); ++t) {
            const bool named   = !mesh_.tri_coupled_node_name[t].empty();
            const bool indexed = mesh_.tri_coupled_node[t] >= 0;
            if (!named && !indexed) continue;
            MeshData::TriCouplingRow row;
            row.tri       = t;
            row.node      = indexed ? mesh_.tri_coupled_node[t] : -1;
            row.node_name = mesh_.tri_coupled_node_name[t];
            row.cd        = mesh_.tri_coupling_cd[t];
            row.area      = mesh_.tri_coupling_area[t];
            mesh_.tri_couplings.push_back(std::move(row));
        }
    }

    for (auto& row : mesh_.tri_couplings) {
        if (row.node < 0) {
            if (row.node_name.empty()) continue;
            int node_idx = ctx.node_names.find(row.node_name);
            if (node_idx < 0) {
                throw std::runtime_error(
                    "2D triangle " + std::to_string(row.tri)
                    + " coupled to unknown node '" + row.node_name + "'");
            }
            row.node = node_idx;
        }
        // Mirror into the legacy arrays (last row wins) so the existing
        // getter API and the GeoPackage writer keep working for the
        // single-coupling case.
        mesh_.tri_coupled_node[row.tri]  = row.node;
        mesh_.tri_coupling_cd[row.tri]   = row.cd;
        mesh_.tri_coupling_area[row.tri] = row.area;
    }

    // Initialize surface state
    state_.resize(mesh_.n_triangles(), mesh_.n_vertices());

    // Drain pending [2D_BOUNDARY_CONDITIONS] / [2D_EDGE_CONVEYANCE] rows into
    // BoundaryData / mesh edge slots (sizes boundary_, flips the drained flag).
    drainPendingRows();

    // Set initial (dry) heads from ground elevation. FLAT: the bed centroid.
    // VFR: the closure's dry anchor η(V=0) — chosen so the solver's
    // head → volume seeding returns exactly V = 0 for every dry cell.
    for (int i = 0; i < mesh_.n_triangles(); ++i) {
        state_.head[i] = headFromMeanDepth(mesh_, options_, i, 0.0);
    }
    // Seed the vertex heads once from the dry cell heads: the all-vertex pass
    // now runs per accepted window (not per RHS eval), so pre-first-window
    // consumers (output snapshots, first inject) must not read zeros.
    reconstructVertexHeads(mesh_, state_, options_.num_threads);

    // Build coupling point descriptors
    coupling_points_ = buildCouplingPoints(mesh_, ctx);

    // Live in-marcher exchange points: the marcher evaluates the orifice law
    // per 2D substep against live surface heads, so every non-outfall coupling
    // point becomes a SINGLE-CELL point (the lowest-bed incident cell, where
    // water pools — the wet/dry ramp then reflects the real pond, not an
    // incidentally-dry neighbour). Built and published on the state BEFORE
    // solver_->initialize(). Outfall coupling stays on the batch-accumulated
    // injection path.
    {
        node_coupling_points_.clear();
        for (const auto& cp : coupling_points_) {
            if (cp.is_outfall) continue;
            CouplingPoint sc = cp;
            if (cp.vertex_idx >= 0) {
                const int v  = cp.vertex_idx;
                const int s  = mesh_.vert_stencil_ptr[v];
                const int e  = mesh_.vert_stencil_ptr[v + 1];
                int    lo    = cp.cell_idx;
                double zlo   = (lo >= 0) ? mesh_.tri_cz[lo] : 1.0e300;
                for (int k = s; k < e; ++k) {
                    const int t = mesh_.vert_stencil_idx[k];
                    if (mesh_.tri_cz[t] < zlo) { zlo = mesh_.tri_cz[t]; lo = t; }
                }
                sc.cell_idx   = lo;
                sc.vertex_idx = -1;
            }
            node_coupling_points_.push_back(sc);
        }
        if (!node_coupling_points_.empty()) {
            state_.node_coupling = &node_coupling_points_;
            state_.nodes_1d      = &ctx.nodes;
        }
    }

    // Auto-align ponded storage on 2D-coupled junctions with the 2D surface.
    //
    // A coupled junction must be able to surcharge above its crown so the 1D
    // HGL tracks the overlying 2D water surface and the spill/inlet exchange
    // can fire. The dynamic-wave solver only lets a node pond above the crown
    // when it has a non-zero ponded_area, so instead of zeroing it (which
    // pinned the HGL at the crown and disabled junction spill — see
    // docs/1D_2D_COUPLING_GATE_REVIEW.md §6 C3a) we OVERRIDE ponded_area with
    // the footprint of the surrounding 2D cells (median-dual area), and flag
    // the node in ctx.coupled_node so setNodeDepth treats it as pond-capable
    // regardless of the global ALLOW_PONDING option.
    //
    // Outfalls are excluded: they couple through a prescribed tailwater head
    // BC (updateOutfallBoundaries / setAllOutfallDepths), not surface ponding.
    //
    // Tradeoff: the 1D pond and the stencil 2D cells represent the same near-
    // manhole surface, so storage there is double-counted; the median-dual
    // share (Σ incident tri_area / 3) keeps that area minimal, and a real
    // flood spreads onto the broader mesh (cells beyond the stencil), which
    // stays single-counted.
    ctx.coupled_node.assign(static_cast<std::size_t>(ctx.n_nodes()), std::uint8_t{0});

    // 2D cell areas are SI m²; 1D ponded_area is 1D-internal ft² (the engine
    // works in feet for every project). No dedicated area factor exists, so
    // convert by squaring the length factor (len_2d_to_1d² ≈ 10.764).
    const double area_2d_to_1d = options_.len_2d_to_1d * options_.len_2d_to_1d;

    // Mutable: the COUPLING_AREA AUTO derivation below rewrites cp.area for
    // points whose input did not author an explicit area.
    for (auto& cp : coupling_points_) {
        if (cp.is_outfall) continue;
        auto ni = static_cast<std::size_t>(cp.node_idx);

        // First time we touch this node: warn about any overridden user values
        // and reset ponded_area before accumulating the auto footprint. A node
        // mapped to several vertices accumulates each vertex's share (the
        // `else` below just adds for subsequent coupling points).
        if (!ctx.coupled_node[ni]) {
            const auto& nname = ctx.node_names.name_of(cp.node_idx);
            if (ctx.nodes.sur_depth[ni] > 0.0) {
                ctx.warnings.push_back(
                    "WARNING: 2D-coupled node '" + nname
                    + "' has sur_depth > 0 — surcharge gate uses invert + "
                      "full_depth + sur_depth as the spill threshold (z_top).");
            }
            if (ctx.nodes.ponded_area[ni] > 0.0) {
                ctx.warnings.push_back(
                    "WARNING: 2D-coupled node '" + nname
                    + "' has ponded_area > 0 — it is being overridden with the "
                      "surrounding 2D-cell footprint so the 1D HGL stays "
                      "aligned with the 2D surface.");
            }
            ctx.nodes.ponded_area[ni] = 0.0;
            ctx.coupled_node[ni] = std::uint8_t{1};

            // Exchange-area sanity: an exchange area far larger than any
            // conduit the node connects to lets the orifice inject water much
            // faster than the pipe can convey it — the node then fills within
            // a window and spills straight back (drain/spill churn: spiky
            // lateral inflows, large node continuity "errors", and 2D solver
            // grind at the coupling stencils). xsect_a_full is 1D ft²; convert
            // to the SI m² of cp.area. Skipped when no conduit area is
            // resolved yet (a_max == 0) — the guard, not the warning, is load-
            // bearing.
            double a_pipe_max = 0.0;
            for (int l = 0; l < ctx.n_links(); ++l) {
                auto ul = static_cast<std::size_t>(l);
                if (ctx.links.node1[ul] == cp.node_idx
                    || ctx.links.node2[ul] == cp.node_idx)
                    a_pipe_max = std::max(a_pipe_max, ctx.links.xsect_a_full[ul]);
            }
            a_pipe_max *= options_.len_1d_to_2d * options_.len_1d_to_2d; // ft²→m²
            // COUPLING_AREA AUTO: derive the exchange area from the largest
            // connected conduit. Applies to EVERY junction point — the option
            // is the escape hatch for files that baked in a constant default
            // (GUI mappers wrote 2.0 m², 10–300× above the pipes on real
            // models, driving the fill-and-spill churn the warning below
            // describes), and those tokens are indistinguishable from intent.
            // Authored values govern whenever AUTO is not requested.
            if (options_.coupling_area_auto && a_pipe_max > 0.0)
                cp.area = std::clamp(1.25 * a_pipe_max, 0.05, 2.0);
            if (a_pipe_max > 0.0 && cp.area > 10.0 * a_pipe_max) {
                char abuf[320];
                std::snprintf(abuf, sizeof(abuf),
                    "WARNING: 2D-coupled node '%s' has exchange AREA %.3f m² — "
                    "%.0fx the largest connected conduit area (%.4f m²). The "
                    "orifice can inject far more than the pipe can convey; "
                    "expect the node to fill and spill back each window. "
                    "Consider AREA ~ the inlet/barrel area.",
                    nname.c_str(), cp.area, cp.area / a_pipe_max, a_pipe_max);
                ctx.warnings.push_back(abuf);
            }
        }

        // Median-dual footprint of the cells around the coupling point (SI m²).
        double foot_m2 = 0.0;
        if (cp.vertex_idx >= 0) {
            const int s = mesh_.vert_stencil_ptr[cp.vertex_idx];
            const int e = mesh_.vert_stencil_ptr[cp.vertex_idx + 1];
            for (int k = s; k < e; ++k)
                foot_m2 += mesh_.tri_area[mesh_.vert_stencil_idx[k]];
            foot_m2 /= 3.0;  // each triangle contributes ~1/3 of its area per vertex
        } else {
            foot_m2 = mesh_.tri_area[cp.cell_idx];
        }
        ctx.nodes.ponded_area[ni] += foot_m2 * area_2d_to_1d;
    }

    // C3b: vertical-datum-consistency guard for 1D↔2D coupling.
    //
    // computeCouplingExchange forms its driving head as dh = h_2d - h_1d,
    // directly subtracting the 1D node head (invert + depth, converted from the
    // engine's internal feet) from the 2D surface elevation (mesh bed + depth,
    // SI metres). That is only physical when the 2D mesh and the 1D inverts
    // share one vertical datum. If they don't — e.g. a model whose 1D geometry
    // was silently rescaled (a repeated metre↔foot save round-trip inflates the
    // inverts AND MaxDepth together) — the node's ground sits far from the 2D
    // surface, dh is dominated by the datum offset, and the coupling drives a
    // spurious exchange (it can drown an outfall under tens of metres of phantom
    // tailwater and back the 1D network up, wrecking 1D continuity).
    //
    // We can't safely auto-correct (the true offset is unknown), so warn with
    // the numbers. Test the node ground (rim = invert + full_depth) against the
    // 2D mesh's own elevation envelope — the un-rescaled reference. The margin
    // scales with the mesh relief, never with the node depth (the same
    // corruption inflates the depth, so it can't be trusted as a yardstick).
    if (!mesh_.vz.empty()) {
        const double ft_to_m = options_.len_1d_to_2d;   // 0.3048 (1D feet → SI)
        double zmin = mesh_.vz[0], zmax = mesh_.vz[0];
        for (double z : mesh_.vz) { zmin = std::min(zmin, z); zmax = std::max(zmax, z); }
        const double margin = std::max(10.0, 10.0 * (zmax - zmin));   // metres
        for (const auto& cp : coupling_points_) {
            auto ni = static_cast<std::size_t>(cp.node_idx);
            const double bed_z = (cp.vertex_idx >= 0)
                ? mesh_.vz[cp.vertex_idx] : mesh_.tri_cz[cp.cell_idx];
            const double rim_m = (ctx.nodes.invert_elev[ni]
                                  + ctx.nodes.full_depth[ni]) * ft_to_m;
            if (rim_m < zmin - margin || rim_m > zmax + margin) {
                char buf[512];
                std::snprintf(buf, sizeof(buf),
                    "WARNING: 2D-coupled node '%s' is on a different vertical "
                    "datum than the 2D mesh: node ground (invert+MaxDepth) = "
                    "%.2f m, but the mesh spans %.2f..%.2f m (bed = %.2f m at "
                    "the coupling cell) — a ~%.1f m offset. The coupling head "
                    "dh = h_2d - h_1d is dominated by this offset and will "
                    "drive a spurious exchange (it can drown an outfall or back "
                    "the 1D network up). Check that the 1D inverts/MaxDepth and "
                    "the 2D mesh elevations use the same datum and units.",
                    ctx.node_names.name_of(cp.node_idx).c_str(),
                    rim_m, zmin, zmax, bed_z, rim_m - bed_z);
                ctx.warnings.push_back(buf);
            }
        }
    }

    // Resolve the OpenMP thread count for the serial-CPU 2D solver's
    // per-cell / per-vertex loops. Mirror DWSolver::setNumThreads: honour the
    // global THREADS option (0 = all cores), cap at the available threads, and
    // single-thread small meshes where fork/join overhead would dominate (the
    // 2D work unit is the triangle, so gate on n_triangles like dynwave gates
    // on its link count). Resolved here once because the topology is final and
    // it must be set whether or not OPENSWMM_HAS_2D — the post-step diagnostic
    // loops (computeCellContinuity / computeFaceVelocity / update_statistics)
    // read options_.num_threads outside the OPENSWMM_HAS_2D block below.
#if defined(SWMM_USE_OPENMP)
    {
        int max_t = omp_get_max_threads();
        int req   = ctx.options.num_threads;
        int n_thr = (req == 0) ? max_t : std::min(req, max_t);
        if (mesh_.n_triangles() < 4 * n_thr) n_thr = 1;
        options_.num_threads = n_thr;
    }
#else
    options_.num_threads = 1;
#endif

    // Attach the per-edge boundary conditions to the state so the flux kernels
    // (serial computeEdgeFluxes and the Kokkos RHS) apply them at boundary
    // edges instead of walling them. Non-owning: boundary_ outlives state_.
    state_.boundary = &boundary_;

    // Seed output gradients for the initial output/API state. Vertex-head
    // timing is left on the solver RHS path to preserve coupling behaviour.
    refreshOutputGradients(mesh_, state_, options_);

#ifdef OPENSWMM_HAS_2D
    // The explicit marcher IS a local-inertial scheme: it owns state_.edge_flux
    // (prognostic q projection + availability-clamped boundary fluxes), so the
    // router never recomputes edge fluxes post-advance.

    // Outfall batch accumulator and the per-cell withdrawal budget.
    window_outfall_accum_.assign(coupling_points_.size(), 0.0);
    window_avail_budget_.assign(static_cast<std::size_t>(mesh_.n_triangles()), 0.0);

    // Construct the time integrator: the Kokkos marcher plugin when installed
    // and eligible (OPENSWMM_2D_BACKEND policy + small-mesh gate), else the
    // serial ExplicitInertialSolver.
    if (!solver_) {
        solver_ = makeSurfaceSolver(options_, nullptr, mesh_.n_triangles());
    }
    solver_->initialize(mesh_, state_, options_);
#endif

    active_ = true;
    sim_time_ = 0.0;
    pending_dt_ = 0.0;

    resetWindowAccumulators();

    // Seed the global 2D mass balance. init_storage is the surface volume at
    // the dry initial condition (0 unless a nonzero initial depth is set).
    ctx.mass_balance_2d.active = true;
    ctx.mass_balance_2d.init_storage = totalVolume();
    ctx.mass_balance_2d.final_storage = ctx.mass_balance_2d.init_storage;
    prev_boundary_cum_ = 0.0;

    // Seed the render-only vertex free surface so the first snapshot (before
    // any sync batch fires — e.g. a hotstart-restored wet state) already
    // carries a physically consistent field instead of the resize() zeros.
    reconstructVertexRenderDepths(mesh_, state_, options_.dry_depth,
                                  options_.num_threads);
}


void SurfaceRouter2D::step(SimulationContext& ctx, double dt, double t) {
    if (!active_) return;

    // Pre-routing: update outfall boundaries from 2D state
    updateOutfallsPreRouting(ctx);

    // Note: 1D routing happens between pre and post hooks in SWMMEngine

    // Post-routing: coupling exchange and 2D advance
    advancePostRouting(ctx, dt, t);
}


void SurfaceRouter2D::updateOutfallsPreRouting(SimulationContext& ctx) {
    if (!active_) return;
    updateOutfallBoundaries(coupling_points_, mesh_, state_, ctx, options_);
}


void SurfaceRouter2D::coAdvanceStep(SimulationContext& ctx, double dt,
                                    double t) {
    if (dt <= 0.0) return;
    perf::ScopedTimer tm_window(perf::sec_2d_window);
    state_.save_state();

    // Outfall discharge → 2D: accumulate this step's 1D outfall discharge and
    // inject it as a constant-rate source over the subcycle (same ledger as
    // the window path). Junction exchange is LIVE inside the marcher. The
    // clear is targeted: only the cells outfall points scatter into (full
    // O(nt) fills per ~1 s routing step were a measured overhead driver).
    if (accumulateOutfallDischargeStep(coupling_points_, mesh_, state_, ctx,
                                       options_, dt, window_outfall_accum_,
                                       window_avail_budget_,
                                       /*sample_row*/ nullptr) > 0)
        ++outfall_clamp_windows_;
    for (const auto& cp : coupling_points_) {
        if (!cp.is_outfall) continue;
        if (cp.vertex_idx >= 0) {
            const int s = mesh_.vert_stencil_ptr[cp.vertex_idx];
            const int e = mesh_.vert_stencil_ptr[cp.vertex_idx + 1];
            for (int k = s; k < e; ++k)
                state_.coupling_flux[mesh_.vert_stencil_idx[k]] = 0.0;
        } else if (cp.cell_idx >= 0) {
            state_.coupling_flux[cp.cell_idx] = 0.0;
        }
    }
    injectAccumulatedExchange(coupling_points_, mesh_, state_,
                              window_outfall_accum_, dt, +1.0);

    // Rainfall / forcings on a coarse cadence: gage values change at the gage
    // timestep (minutes), and the per-cell interpolation apply is O(nt) — per
    // ~1 s routing step it was a measured overhead driver. Boundary values
    // resolve every step (cheap, perimeter-sized). forcing_dirty bypasses the
    // cadence: the forcing API just changed a prescription (or a one-shot
    // expired in clear_reset_forcings), so apply immediately — that keeps the
    // documented per-step RESET semantics under the batched co-advance.
    co_forcing_elapsed_ += dt;
    if (co_forcing_elapsed_ >= 30.0 || co_forcing_first_ ||
        state_.forcing_dirty) {
        state_.forcing_dirty = false;
        updateRainfall(ctx);
        std::fill(state_.evap_rate.begin(), state_.evap_rate.end(), 0.0);
        for (std::size_t i = 0; i < state_.depth.size(); ++i) {
            if (state_.rainfall_forced[i] == 1)
                state_.rainfall[i] = state_.rainfall_force_val[i];
            else if (state_.rainfall_forced[i] == 2)
                state_.rainfall[i] += state_.rainfall_force_val[i];
            if (state_.evap_forced[i] == 1)
                state_.evap_rate[i] = state_.evap_force_val[i];
            else if (state_.evap_forced[i] == 2)
                state_.evap_rate[i] += state_.evap_force_val[i];
            if (state_.coupling_forced[i] == 1)
                state_.coupling_flux[i] = state_.coupling_force_val[i];
            else if (state_.coupling_forced[i] == 2)
                state_.coupling_flux[i] += state_.coupling_force_val[i];
        }
        co_forcing_elapsed_ = 0.0;
        co_forcing_first_ = false;
    }
    resolveBoundaryValues(ctx, t);

    // OPENSWMM_2D_HEAD_RAMP=1 (experimental, decoupling-viability study):
    // extrapolate each coupled node's 1D head linearly across the batch from
    // its batch-over-batch trend, instead of holding the batch-start head.
    // Serial marcher only (dynamic_cast); plugin backends run held heads.
    static const bool head_ramp = [] {
        const char* e = std::getenv("OPENSWMM_2D_HEAD_RAMP");
        return e && e[0] == '1';
    }();
    if (head_ramp) {
        if (auto* m = dynamic_cast<ExplicitInertialSolver*>(solver_.get())) {
            const std::size_t np = node_coupling_points_.size();
            if (ramp_prev_head_.size() != np) {
                ramp_prev_head_.assign(np, 0.0);
                ramp_prev_span_ = 0.0;
                for (std::size_t k = 0; k < np; ++k)
                    ramp_prev_head_[k] =
                        ctx.nodes.head[static_cast<std::size_t>(
                            node_coupling_points_[k].node_idx)] *
                        options_.len_1d_to_2d;
            }
            std::vector<double> slopes(np, 0.0);
            for (std::size_t k = 0; k < np; ++k) {
                const double h_now =
                    ctx.nodes.head[static_cast<std::size_t>(
                        node_coupling_points_[k].node_idx)] *
                    options_.len_1d_to_2d;
                if (ramp_prev_span_ > 0.0)
                    slopes[k] = (h_now - ramp_prev_head_[k]) / ramp_prev_span_;
                ramp_prev_head_[k] = h_now;
            }
            ramp_prev_span_ = dt;
            m->setExchangeHeadSlopes(std::move(slopes));
        }
    }

    {
        perf::ScopedTimer tm_adv(perf::sec_2d_advance);
        solver_->advance(sim_time_, sim_time_ + dt);  // always reaches target
    }
    sim_time_ += dt;

    // Junction ledger: the marcher integrated ∫Q_k dt (m³, + = 2D→1D drain)
    // per live point at substep cadence. Book the batch volume (1D ft³)
    // through the delivery QUEUE so assembleLateralInflows drains it at a
    // uniform rate over the batch span instead of a single-step pulse —
    // cleared then accumulated so points sharing a node sum correctly.
    const std::vector<double>& exch = solver_->last_coupling_exchange();
    if (!exch.empty()) {
        for (const auto& cp : node_coupling_points_)
            ctx.nodes.coupling_volume[static_cast<std::size_t>(cp.node_idx)] =
                0.0;
        const std::size_t n =
            std::min(exch.size(), node_coupling_points_.size());
        for (std::size_t k = 0; k < n; ++k) {
            const auto ni =
                static_cast<std::size_t>(node_coupling_points_[k].node_idx);
            ctx.nodes.coupling_volume[ni] += exch[k] * options_.flow_2d_to_1d;
        }
    }

    // Boundary ledger: the marcher published the WINDOW-MEAN applied flux in
    // the boundary slots, so −flux·dt recovers the exact ∫F_applied dt.
    {
        const int ne = boundary_.size();
        for (int idx = 0; idx < ne; ++idx)
            if (static_cast<BoundaryType>(boundary_.edge_bc_type[idx]) !=
                BoundaryType::WALL)
                boundary_.edge_bc_cum_flux[idx] +=
                    -state_.edge_flux[idx] * dt;
    }

    // Ledgers every batch (accumulateMassBalance reads coupling_volume as
    // "this batch's exchange", so it MUST run before the queue move below) …
    accumulateMassBalance(ctx, dt);

    // … then hand the batch's junction volumes to the delivery QUEUE so
    // assembleLateralInflows drains them at a uniform rate over the batch
    // span instead of a single-step pulse.
    if (!exch.empty()) {
        bool queued = false;
        for (const auto& cp : node_coupling_points_) {
            const auto ni = static_cast<std::size_t>(cp.node_idx);
            if (ctx.nodes.coupling_volume[ni] != 0.0) {
                ctx.nodes.coupling_queue[ni] += ctx.nodes.coupling_volume[ni];
                ctx.nodes.coupling_volume[ni] = 0.0;
                queued = true;
            }
        }
        if (queued)
            ctx.coupling_delivery_remaining =
                std::max(ctx.coupling_delivery_remaining, dt);
    }
    state_.clear_reset_forcings();
    resetWindowAccumulators();

    // … but the full-mesh OUTPUT refresh (vertex heads, gradients, velocity,
    // render depths, statistics, per-cell continuity) only on a report-scale
    // cadence: six 228k-cell passes per ~1 s routing step were measured at
    // ~90 ms/step — 20× the marcher's own advance cost. Outputs are consumed
    // at REPORT_STEP granularity; exchange/outfall heads use solver-fresh
    // state.head directly, not these derived fields.
    co_refresh_elapsed_ += dt;
    const double refresh_dt =
        std::max(dt, std::min(ctx.options.report_step > 0.0
                                  ? ctx.options.report_step
                                  : 30.0,
                              30.0));
    if (co_refresh_elapsed_ >= refresh_dt) {
        reconstructVertexHeads(mesh_, state_, options_.num_threads);
        refreshOutputGradients(mesh_, state_, options_);
        computeCellContinuity(mesh_, state_, options_, co_refresh_elapsed_);
        computeFaceVelocity(mesh_, state_, options_);
        reconstructVertexRenderDepths(mesh_, state_, options_.dry_depth,
                                      options_.num_threads);
        state_.update_statistics(mesh_.tri_area, co_refresh_elapsed_,
                                 options_.num_threads);
        co_refresh_elapsed_ = 0.0;
    }
}


void SurfaceRouter2D::computeCouplingConductances(
    const SimulationContext& ctx,
    std::vector<std::pair<int, double>>& out) const {
    if (!active_) return;
    // G in SI is m³/s per metre of (2D-frame) 1D head; the 1D consumes ft³/s
    // per ft of its own head: flow_2d_to_1d converts the flow, len_1d_to_2d
    // converts per-metre → per-foot.
    const double to_1d = options_.flow_2d_to_1d * options_.len_1d_to_2d;
    for (const auto& cp : node_coupling_points_) {
        const double g = computeNodeCouplingDQdh1d(cp, mesh_, state_,
                                                   ctx.nodes, options_);
        if (g > 0.0) out.emplace_back(cp.node_idx, g * to_1d);
    }
}


void SurfaceRouter2D::advancePostRouting(SimulationContext& ctx, double routing_dt,
                                          double t) {
    if (!active_) return;

    // Windowless co-advance: live in-marcher exchange, no CFL clamp on the
    // 1D, no failure/carry machinery (the marcher's CFL step is known a
    // priori — it always reaches its target). The 2D advances in SYNC BATCHES
    // of several routing steps: a ~1 s routing step is smaller than an LTS
    // macro cycle, so per-step advances would degenerate every call to the
    // global-dt tail plus an O(nt) settle — measured as the dominant cost.
    // Exchange volumes booked per batch are delivered to the 1D through the
    // queue spread (uniform rate over the batch span).
    pending_dt_ += routing_dt;
    last_t_ = t;
    // OPENSWMM_2D_SYNC_SPAN (seconds): experimental override of the sync-batch
    // span for the decoupling-viability study — bypasses both the MAX_TIMESTEP
    // coupling and the 60 s ceiling. Unset/0 = normal policy.
    static const double env_span = [] {
        const char* e = std::getenv("OPENSWMM_2D_SYNC_SPAN");
        return e ? std::atof(e) : 0.0;
    }();
    const double sync = env_span > 0.0
        ? std::max(env_span, ctx.options.routing_step)
        : std::clamp(options_.max_timestep, ctx.options.routing_step, 60.0);
    if (pending_dt_ + 0.5 * routing_dt >= sync) {
        const double span = pending_dt_;
        pending_dt_ = 0.0;
        coAdvanceStep(ctx, span, t);
    }
}


void SurfaceRouter2D::prepareOneShotForcing(SimulationContext& ctx) {
    if (!active_) return;

    // Flush the partial sync batch so the one-shot forcing applies to future
    // time only; the next batch picks up the new prescription.
    if (pending_dt_ > 0.0) {
        const double span = pending_dt_;
        pending_dt_ = 0.0;
        coAdvanceStep(ctx, span, last_t_);
    }
}


void SurfaceRouter2D::resetWindowAccumulators() {
    std::fill(window_outfall_accum_.begin(), window_outfall_accum_.end(), 0.0);
    // Withdrawal budget = the water each cell actually holds right now (the
    // state the next batch's outfall evaluations will read).
    const int nt = mesh_.n_triangles();
    for (int i = 0; i < nt; ++i)
        window_avail_budget_[static_cast<std::size_t>(i)] =
            std::max(0.0, state_.volume[static_cast<std::size_t>(i)]);
}


void SurfaceRouter2D::finalize(SimulationContext& ctx) {
    // Flush the partial sync batch so the 2D clock (and the exchange booking)
    // ends at the simulation end instead of up to one batch short. The
    // marcher always integrates the whole span — no failure/backlog handling.
    if (active_ && pending_dt_ > 0.0) {
        const double span = pending_dt_;
        pending_dt_ = 0.0;
        coAdvanceStep(ctx, span, last_t_);
    }
#ifdef OPENSWMM_HAS_2D
    if (solver_) {
        // Publish cumulative solver statistics BEFORE finalize() frees the
        // memory the counters live in (the "2D Solver Statistics" report
        // block reads these).
        const auto s = solver_->run_stats();
        auto& mb = ctx.mass_balance_2d;
        mb.solver_nsteps         = s.nsteps;
        mb.solver_nrhs           = s.nrhs;
        mb.solver_last_h         = s.last_h;
        mb.solver_avg_h          = (s.nsteps > 0 && sim_time_ > 0.0)
                                       ? sim_time_ / static_cast<double>(s.nsteps)
                                       : 0.0;
        mb.solver_active_min     = s.active_frac_min;
        mb.solver_active_mean    = s.active_frac_mean;
        mb.solver_active_max     = s.active_frac_max;
        mb.solver_n_tiers        = s.n_tiers;
        for (int k = 0; k < s.n_tiers && k < 8; ++k)
            mb.solver_tier_cells[k] = s.tier_cells[k];
        solver_->finalize();
    }
#endif
    if (outfall_clamp_windows_ > 0) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "WARNING: 2D outfall withdrawal was capped by the water available "
            "on the surface in %ld sync batch(es); check the outfall stage "
            "coupling and the 2D continuity block.", outfall_clamp_windows_);
        ctx.warnings.push_back(buf);
    }
    active_ = false;
}


double SurfaceRouter2D::totalVolume() const {
    // Volume is the integrated state — sum it directly (for a partly-wet cell
    // V ≠ h̄·A_total, so the old depth×area form would be wrong under VFR).
    double vol = 0.0;
    int nt = mesh_.n_triangles();
    for (int i = 0; i < nt; ++i) {
        vol += state_.volume[i];
    }
    return vol;
}


double SurfaceRouter2D::totalExchangeFlow() const {
    double flow = 0.0;
    int nt = mesh_.n_triangles();
    for (int i = 0; i < nt; ++i) {
        flow += state_.coupling_flux[i] * mesh_.tri_area[i];
    }
    return flow;
}


void SurfaceRouter2D::updateRainfall(SimulationContext& ctx) {
    const int n_gages = ctx.n_gages();

    if (n_gages <= 0 || options_.rainfall_mode == RainfallMode::NONE) {
        std::fill(state_.rainfall.begin(), state_.rainfall.end(), 0.0);
        return;
    }

    // Convert every gage's current rainfall (user units, in/hr or mm/hr) to the
    // solver's SI m/s. The conversion is linear, so interpolating the converted
    // values is identical to interpolating then converting.
    const double to_ms =
        (ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units)) == 0)
            ? (0.0254 / 3600.0)   // US: in/hr → m/s
            : (0.001  / 3600.0);  // SI: mm/hr → m/s
    rain_si_.assign(static_cast<std::size_t>(n_gages), 0.0);
    for (int g = 0; g < n_gages; ++g)
        rain_si_[static_cast<std::size_t>(g)] = ctx.gages.rainfall[g] * to_ms;

    if (options_.rainfall_mode == RainfallMode::NATURAL_NEIGHBOUR && interp_.ready()) {
        // Natural-neighbour (Laplace) interpolation inside the gage hull, IDW
        // outside — applied as the precomputed per-cell sparse weighted sum.
        interp_.apply(rain_si_, state_.rainfall);
    } else {
        // SYSTEM mode (or no located gages): uniform = mean of all gages.
        double mean = 0.0;
        for (double r : rain_si_) mean += r;
        mean /= static_cast<double>(n_gages);
        std::fill(state_.rainfall.begin(), state_.rainfall.end(), mean);
    }
}


void SurfaceRouter2D::resolveBoundaryValues(SimulationContext& ctx, double t) {
    const int ne = boundary_.size();
    if (ne == 0) return;

    // One-shot: resolve deferred timeseries / curve names to registry indices.
    // ctx.table_names is only populated post-parse, so this can't happen at
    // parse time; -2 = "name pending", -1 = not found (then treated as constant).
    if (!boundary_names_resolved_) {
        boundary_names_resolved_ = true;
        for (int idx = 0; idx < ne; ++idx) {
            if (boundary_.edge_bc_tseries[idx] == -2)
                boundary_.edge_bc_tseries[idx] =
                    ctx.table_names.find(boundary_.edge_bc_tseries_name[idx]);
            if (boundary_.edge_bc_flow_tseries[idx] == -2)
                boundary_.edge_bc_flow_tseries[idx] =
                    ctx.table_names.find(boundary_.edge_bc_flow_tseries_name[idx]);
            if (boundary_.edge_bc_rating_curve[idx] == -2)
                boundary_.edge_bc_rating_curve[idx] =
                    ctx.table_names.find(boundary_.edge_bc_rating_curve_name[idx]);
        }
    }

    const int n_tables = static_cast<int>(ctx.tables.tables.size());
    for (int idx = 0; idx < ne; ++idx) {
        switch (static_cast<BoundaryType>(boundary_.edge_bc_type[idx])) {
            case BoundaryType::SPECIFIED_STAGE: {
                const int ts = boundary_.edge_bc_tseries[idx];
                if (ts >= 0 && ts < n_tables)
                    boundary_.edge_bc_head[idx] =
                        table_lookup_cursor(ctx.tables.tables[ts], t);
                break;  // else constant: edge_bc_head already holds the value
            }
            case BoundaryType::SPECIFIED_FLOW: {
                const int ts = boundary_.edge_bc_flow_tseries[idx];
                if (ts >= 0 && ts < n_tables)
                    boundary_.edge_bc_flow[idx] =
                        table_lookup_cursor(ctx.tables.tables[ts], t);
                break;
            }
            case BoundaryType::RATING_CURVE: {
                const int cv = boundary_.edge_bc_rating_curve[idx];
                if (cv >= 0 && cv < n_tables) {
                    // Stage = boundary cell water-surface elevation (lagged to
                    // start-of-step). Curve maps stage → outward discharge per
                    // metre of edge (m³/s/m), consistent with SPECIFIED_FLOW.
                    const int i = idx / 3;
                    boundary_.edge_bc_flow[idx] =
                        table_lookupEx(ctx.tables.tables[cv], state_.head[i]);
                }
                break;
            }
            default: break;  // WALL, NORMAL_FLOW — nothing to resolve per step
        }
    }
}


void SurfaceRouter2D::accumulateMassBalance(SimulationContext& ctx, double dt) {
    auto& mb = ctx.mass_balance_2d;
    int nt = mesh_.n_triangles();

    // Rainfall inflow (m³): rainfall is m/s after forcings are applied.
    double rain_vol = 0.0;
    for (int i = 0; i < nt; ++i) {
        rain_vol += state_.rainfall[i] * mesh_.tri_area[i];
    }
    mb.rainfall_in += rain_vol * dt;

    // Evaporation loss (m³): the depth-limited sink assembleRHS integrates,
    // evaluated at the accepted end-of-step depths (exact when cells stay
    // wetter than dry_depth; first-order through dry-out, matching the
    // rainfall term's treatment). Accumulated into the 2D mass-balance struct
    // so the continuity error and reported totals close natively; the 2D
    // state mirror (evap_loss_total) is retained for back-compatibility.
    double evap_vol = 0.0;
    for (int i = 0; i < nt; ++i) {
        evap_vol += evapSink(state_.evap_rate[i], state_.depth[i],
                             options_.dry_depth) * mesh_.tri_area[i];
    }
    mb.evap_out += evap_vol * dt;
    state_.evap_loss_total += evap_vol * dt;

    // Coupling and outfall exchange (m³, SI-native, already capped/clamped —
    // exactly what the 2D domain was asked to move). Outfall sign: + = pipe
    // discharge into 2D (source), − = withdrawal, from the per-batch
    // accumulator. Junction exchange: the marcher booked the batch's per-node
    // total into nodes.coupling_volume (before the queue move) — read it with
    // a per-node dedupe. Sign: + = 2D→1D drain (out of 2D), − = 1D→2D spill.
    std::unordered_set<int> seen;
    for (std::size_t k = 0; k < coupling_points_.size(); ++k) {
        const auto& cp = coupling_points_[k];
        if (cp.is_outfall) {
            const double v = window_outfall_accum_[k];
            if (v > 0.0) mb.outfall_in  += v;
            else         mb.outfall_out += -v;
        } else {
            if (!seen.insert(cp.node_idx).second) continue;
            const double vol = ctx.nodes.coupling_volume[
                static_cast<std::size_t>(cp.node_idx)] * options_.vol_1d_to_2d;
            if (vol > 0.0) mb.coupling_2d_to_1d_out += vol;
            else           mb.coupling_1d_to_2d_in  += -vol;
        }
    }

    // Boundary exchange (outward-positive cumulative, m³). Reads 0 until the
    // non-Wall BC flux integration lands, but the term is wired now.
    double cur_bnd = 0.0;
    for (double f : boundary_.edge_bc_cum_flux) cur_bnd += f;
    double dbnd = cur_bnd - prev_boundary_cum_;
    prev_boundary_cum_ = cur_bnd;
    if (dbnd > 0.0) mb.boundary_out += dbnd;
    else            mb.boundary_in  += -dbnd;

    // Latest storage (m³) — overwrite so the value at simulation end is final.
    mb.final_storage = totalVolume();
}


} // namespace openswmm::twoD
