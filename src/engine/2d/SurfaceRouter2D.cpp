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
#include "solver/ActiveSetBuilder.hpp"
#include "solver/SurfaceFluxCalculator.hpp"
#ifdef OPENSWMM_HAS_2D
#include "solver/SurfaceSolverFactory.hpp"
#endif
#include "../core/SimulationContext.hpp"
#include "../core/UnitConversion.hpp"

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

    for (int t = 0; t < mesh_.n_triangles(); ++t) {
        auto& name = mesh_.tri_coupled_node_name[t];
        if (name.empty()) continue;

        int node_idx = ctx.node_names.find(name);
        if (node_idx >= 0) {
            mesh_.tri_coupled_node[t] = node_idx;
        } else {
            throw std::runtime_error(
                "2D triangle " + std::to_string(t)
                + " coupled to unknown node '" + name + "'");
        }
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

    // Cells whose state participates in the explicit 1D↔2D exchange — the
    // coupling-point stencils (vertex coupling) or single cells (triangle
    // coupling). Only THESE constrain the routing step through the CFL hint:
    // the 2D interior is integrated fully implicitly by CVODE and imposes no
    // step limit of its own. (The previous whole-mesh scan let any wet cell —
    // e.g. rain-on-mesh ponding kilometres from the network — pin the 1D
    // routing step for the entire run.)
    cfl_cells_.clear();
    {
        std::unordered_set<int> seen_cells;
        for (const auto& cp : coupling_points_) {
            if (cp.vertex_idx >= 0) {
                const int v = cp.vertex_idx;
                for (int k = mesh_.vert_stencil_ptr[v];
                     k < mesh_.vert_stencil_ptr[v + 1]; ++k)
                    seen_cells.insert(mesh_.vert_stencil_idx[k]);
            } else if (cp.cell_idx >= 0) {
                seen_cells.insert(cp.cell_idx);
            }
        }
        cfl_cells_.assign(seen_cells.begin(), seen_cells.end());
        std::sort(cfl_cells_.begin(), cfl_cells_.end());
    }

    // Live (implicit) coupling path — OPT-IN via OPENSWMM_2D_LIVE_COUPLING. The
    // orifice exchange is evaluated inside the CVODE RHS against the live 2D head
    // so it self-limits and integrates stably/conservatively over a large
    // macro-window (the held-flux source becomes unstable there). Phase 3d makes
    // each point SINGLE-CELL (centroid) so the orifice ∂Q/∂V is a clean diagonal
    // term the analytic J·v now assembles (SurfaceTangent) — the live path uses
    // analytic J·v like the held path instead of the finite-difference fallback
    // that was its ~1.26× penalty. Kept behind an env flag so the default + the
    // fast held macro-step (COUPLING_INTERVAL) paths are unaffected while the
    // single-cell exchange distribution is validated. Build the non-outfall point
    // list and publish it (+ the 1D node data, frozen during an advance) on the
    // state BEFORE solver_->initialize(), which sizes the augmented state vector
    // (nt cells + one ∫Q dt accumulator per point). Outfall coupling stays on the
    // held path (transferOutfallDischarges).
    if (std::getenv("OPENSWMM_2D_LIVE_COUPLING") != nullptr) {
        node_coupling_points_.clear();
        for (const auto& cp : coupling_points_) {
            if (cp.is_outfall) continue;
            // Single-cell coupling (Phase 3d): map each point to ONE driving cell
            // so the orifice exchange depends on a single cell volume — the form
            // whose Jacobian is a clean diagonal term (analytic J·v on the live
            // path). A vertex point becomes a centroid point (vertex_idx = −1) at
            // the LOWEST-bed incident cell (where water pools, so the wet/dry ramp
            // reflects the real pond, not an incidentally-dry neighbour).
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

    for (const auto& cp : coupling_points_) {
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
    // Fold the OPENSWMM_2D_MOMENTUM env override into options_ so it is the single
    // source of truth for both the solver (which RHS split) and the post-advance
    // edge-flux handling below (DW recompute vs. inertial q-projection).
    if (const char* m = std::getenv("OPENSWMM_2D_MOMENTUM")) {
        if (std::strcmp(m, "inertial") == 0) options_.momentum = MomentumType::INERTIAL;
        else if (std::strcmp(m, "dw") == 0)  options_.momentum = MomentumType::DW;
    }

    // Decoupled-timestep coupling state: per-point window accumulators and the
    // per-cell withdrawal budget. The exchange is booked to the 1D every routing
    // step and injected into the 2D as the held MEAN-rate coupling_flux source
    // when the window fires — conservative and y-independent. (The RHS-side
    // interpolated deviation was deleted in Phase 3; it was the only term that
    // blocked the analytic Jacobian on coupled models and it required a
    // trapezoid-vs-BDF quadrature reconciliation to stay conservative.)
    window_exchange_accum_.assign(coupling_points_.size(), 0.0);
    window_outfall_accum_.assign(coupling_points_.size(), 0.0);
    window_avail_budget_.assign(static_cast<std::size_t>(mesh_.n_triangles()), 0.0);
    window_had_outfall_clamp_ = false;

    // Construct the time integrator. The backend (serial CPU vs. a runtime-
    // loaded GPU plugin) is resolved by makeSurfaceSolver from the
    // OPENSWMM_2D_BACKEND policy; absent/unusable plugins fall back to the
    // serial CPU solver. See docs/2D_GPU_PORTABLE_CVODE_STRATEGY.md §4.2.
    if (!solver_) {
        solver_ = makeSurfaceSolver(options_, nullptr, mesh_.n_triangles());
    }
    solver_->initialize(mesh_, state_, options_);
#endif

    active_ = true;
    sim_time_ = 0.0;
    pending_dt_ = 0.0;
    force_next_window_ = false;

    resetWindowAccumulators();

    // COUPLING_INTERVAL > 1 advances the 2D solver over a multi-routing-step
    // macro-window so CVODE can take large adaptive steps (big speedup). This is
    // an EXPLICIT coupling sub-cycle, so the window length is CFL-limited by the
    // 1D↔2D coupling stiffness: small windows stay conservative, but too large a
    // window destabilises the exchange (oscillation → clamped negative cells →
    // mass loss). Always confirm the reported 2D continuity after enabling it.
    if (options_.coupling_interval > 1) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "WARNING: 2D COUPLING_INTERVAL=%d advances the solver over a "
            "%d-step macro-window (experimental, CFL-limited) — verify the 2D "
            "continuity error; reduce the interval if it grows.",
            options_.coupling_interval, options_.coupling_interval);
        ctx.warnings.push_back(buf);
    }

    // Seed the global 2D mass balance. init_storage is the surface volume at
    // the dry initial condition (0 unless a nonzero initial depth is set).
    ctx.mass_balance_2d.active = true;
    ctx.mass_balance_2d.init_storage = totalVolume();
    ctx.mass_balance_2d.final_storage = ctx.mass_balance_2d.init_storage;
    prev_boundary_cum_ = 0.0;

    // Seed the cached CFL hint from the initial state (nonzero if the model
    // starts with water on the mesh); refreshed after every 2D advance.
    updateCflHint();

    // Dry-cell active-set masking (opt-in). Resolve the option (env overrides
    // the INP key), restrict it to the validated integrator/momentum pair, and
    // run the one-time seed pass BEFORE enabling the mask: the seed fills
    // every frozen cell/vertex with its exact dry value (terrain gradients,
    // bed-level vertex heads, zero fluxes), which the masked stages then rely
    // on when active cells read frozen neighbours.
    {
        bool want = options_.active_set;
        if (const char* env = std::getenv("OPENSWMM_2D_ACTIVE_SET"))
            want = (env[0] == '1' || env[0] == 'y' || env[0] == 'Y');
        if (const char* env = std::getenv("OPENSWMM_2D_ACTIVE_SET_HALO")) {
            char* endp = nullptr;
            const long h = std::strtol(env, &endp, 10);
            if (endp != env && h >= 1) options_.active_set_halo = static_cast<int>(h);
        }
        active_set_.halo_rings    = std::max(1, options_.active_set_halo);
        // The wet/seed threshold must sit ABOVE the solver's per-cell error
        // floor (abs_tolerance is a depth): implicit solves splash tolerance-
        // level films across the whole active set, and a threshold below that
        // noise reads them as "wet" — false breach trips and an active set
        // that can only grow. One order of margin over the tolerance.
        active_set_.wet_depth_eps = std::max(1.0e-3 * options_.dry_depth,
                                             10.0 * options_.abs_tolerance);
        if (const char* env = std::getenv("OPENSWMM_2D_ACTIVE_SET_EPS")) {
            char* endp = nullptr;
            const double v = std::strtod(env, &endp);
            if (endp != env && v >= 0.0) active_set_.wet_depth_eps = v;
        }

        const bool supported = (options_.integrator == IntegratorType::CVODE)
                            && (options_.momentum   == MomentumType::DW);
        if (want && !supported) {
            ctx.warnings.push_back(
                "WARNING: 2D ACTIVE_SET requires the CVODE diffusive-wave "
                "solver; masking disabled for this run.");
            want = false;
        }
        active_set_.enabled = false;   // seed pass must take the full loops
        if (want) {
            active_set_.resize(mesh_.n_triangles(), mesh_.n_vertices());
            state_.active_set = &active_set_;
            seedInactiveState(mesh_, state_, options_);
            active_set_.enabled = true;
        } else {
            state_.active_set = nullptr;
        }
    }

    // Resolve the effective 2D advance-window policy (time-based macro-step).
    // Explicit COUPLING_WINDOW wins; AUTO (−1, the default) maps a legacy
    // COUPLING_INTERVAL N > 1 to the TIME window N × nominal ROUTING_STEP
    // (2026-07 decoupling plan — step-count gating is retired: under 1D
    // variable-step collapse an N-step interval degenerated to an advance per
    // MINIMUM_STEP, exactly the cadence collapse the time window exists to
    // prevent, while the time mapping preserves the model author's intended
    // cadence in physical time). AUTO alone takes the declared ROUTING_STEP as
    // the acceptable coupling cadence. Behavior-preserving for healthy models:
    // window == nominal routing step fires every step.
    {
        double win = options_.coupling_window;
        if (const char* env = std::getenv("OPENSWMM_2D_COUPLING_WINDOW")) {
            char* endp = nullptr;
            const double v = std::strtod(env, &endp);
            if (endp != env) win = v;
        }
        if (win < 0.0) {
            win = (options_.coupling_interval > 1)
                      ? options_.coupling_interval * ctx.options.routing_step
                      : std::min(ctx.options.routing_step, options_.max_timestep);
        }
        effective_window_ = std::max(0.0, win);
        window_target_    = effective_window_;
        clean_windows_    = 0;
    }

    // Seed the render-only vertex free surface so the first snapshot (before
    // any advance window fires — e.g. a hotstart-restored wet state) already
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


void SurfaceRouter2D::advancePostRouting(SimulationContext& ctx, double routing_dt,
                                          double t) {
    if (!active_) return;

    // ── Per-step decoupled exchange (2026-07 plan) ──────────────────────────
    // The 1D and 2D domains proceed on their own clocks: the exchange is
    // evaluated EVERY routing step against the live 1D heads and the frozen 2D
    // state, booked to the 1D node for next-step consumption, and accumulated
    // as a volume for the 2D window injection. The window fire below no longer
    // samples heads at all — it injects exactly what accumulated.
    // Junctions skip this on the live-RHS path (exchange evaluated inside the
    // solver); outfalls accumulate per-step on both paths.
    if (!state_.node_coupling)
        computeCouplingExchangeStep(coupling_points_, mesh_, state_, ctx,
                                    options_, routing_dt,
                                    window_exchange_accum_,
                                    window_avail_budget_,
                                    /*sample_row*/ nullptr);
    if (accumulateOutfallDischargeStep(coupling_points_, mesh_, state_, ctx,
                                       options_, routing_dt,
                                       window_outfall_accum_,
                                       window_avail_budget_,
                                       /*sample_row*/ nullptr) > 0)
        window_had_outfall_clamp_ = true;

    // Macro-step subcycling. Accumulate the routing time elapsed since the last
    // 2D advance, then (when it is time to fire) integrate the 2D solver over
    // the WHOLE accumulated window in a single advance() call. This lets CVODE
    // take large adaptive internal steps instead of being hard-stopped on every
    // ~routing-step boundary — the dominant cost driver for the semi-discrete
    // solver. Gating is TIME-based only (effective_window_, resolved at
    // initialize(); legacy COUPLING_INTERVAL now maps to a time window there):
    // the window is physical time, so 1D variable-step collapse cannot shrink
    // the 2D cadence — exactly the regime where the old step-count interval
    // degenerated to an advance per MINIMUM_STEP. Every downstream term (the
    // solver advance, sim_time_, boundary-flux, continuity, statistics, and
    // the mass-balance ledgers) uses the same accumulated `dt`, so the 1D↔2D
    // exchange stays conservative over the macro step. dt varies under
    // VARIABLE_STEP, so accumulate the actual elapsed time.
    pending_dt_ += routing_dt;
    last_t_ = t;
    const bool force_window = force_next_window_;
    if (!force_window && effective_window_ > 0.0) {
        // Fire when within half a routing step of the window (jitter cannot
        // systematically overshoot: the alternative is overshooting by a whole
        // step). A window equal to the actual routing step fires every step.
        if (pending_dt_ + 0.5 * routing_dt < effective_window_) return;
    }
    force_next_window_ = false;
    const double dt = pending_dt_;   // the 2D macro-step
    pending_dt_ = 0.0;
    fireAdvanceWindow(ctx, dt, t);
}


void SurfaceRouter2D::prepareOneShotForcing(SimulationContext& ctx) {
    if (!active_) return;

    if (pending_dt_ > 0.0) {
        const double dt = pending_dt_;
        pending_dt_ = 0.0;
        force_next_window_ = false;
        fireAdvanceWindow(ctx, dt, last_t_);
    }
    force_next_window_ = true;
}


void SurfaceRouter2D::resetWindowAccumulators() {
    std::fill(window_exchange_accum_.begin(), window_exchange_accum_.end(), 0.0);
    std::fill(window_outfall_accum_.begin(), window_outfall_accum_.end(), 0.0);
    // Withdrawal budget = the water each cell actually holds right now (the
    // state the next window's per-step evaluations will read as frozen).
    const int nt = mesh_.n_triangles();
    for (int i = 0; i < nt; ++i)
        window_avail_budget_[static_cast<std::size_t>(i)] =
            std::max(0.0, state_.volume[static_cast<std::size_t>(i)]);
}


void SurfaceRouter2D::fireAdvanceWindow(SimulationContext& ctx, double dt,
                                         double t) {
    // Save 2D state
    state_.save_state();

    // Decoupled-timestep coupling (2026-07 plan): the exchange was evaluated
    // and booked to the 1D EVERY routing step (advancePostRouting); here the
    // accumulated volumes are delivered to the 2D as the window source. The
    // MEAN rate per point goes into the held coupling_flux (the conservative
    // baseline: active-set seeding, quiescence, and every backend read it);
    // the sampled per-step series is finalized below into a zero-mean
    // deviation the CVODE RHS interpolates in time. Live macro-step path
    // (state_.node_coupling set): the junction accumulators are identically
    // zero (the orifice is evaluated inside the RHS); outfall accumulators
    // apply on both paths.
    const bool live_coupling = (state_.node_coupling != nullptr);
    std::fill(state_.coupling_flux.begin(), state_.coupling_flux.end(), 0.0);
    // Junction accum is drain-positive (2D sink) → sign −1; outfall accum is
    // discharge-positive (2D source) → sign +1.
    injectAccumulatedExchange(coupling_points_, mesh_, state_,
                              window_exchange_accum_, dt, -1.0);
    injectAccumulatedExchange(coupling_points_, mesh_, state_,
                              window_outfall_accum_, dt, +1.0);
    if (window_had_outfall_clamp_) {
        ++outfall_clamp_windows_;
        window_had_outfall_clamp_ = false;
    }

    // Update rainfall from system gages
    updateRainfall(ctx);

    // Evaporation demand: default 0 until the 1D ClimateState.evap_rate
    // broadcast is wired in (owned by the climate-coupling work stream).
    // Runtime forcing (below) is the only source today.
    std::fill(state_.evap_rate.begin(), state_.evap_rate.end(), 0.0);

    // Apply 2D forcings
    for (std::size_t i = 0; i < state_.depth.size(); ++i) {
        // Rainfall forcing
        if (state_.rainfall_forced[i] == 1) {
            state_.rainfall[i] = state_.rainfall_force_val[i];
        } else if (state_.rainfall_forced[i] == 2) {
            state_.rainfall[i] += state_.rainfall_force_val[i];
        }
        // Evaporation forcing
        if (state_.evap_forced[i] == 1) {
            state_.evap_rate[i] = state_.evap_force_val[i];
        } else if (state_.evap_forced[i] == 2) {
            state_.evap_rate[i] += state_.evap_force_val[i];
        }
        // Coupling forcing
        if (state_.coupling_forced[i] == 1) {
            state_.coupling_flux[i] = state_.coupling_force_val[i];
        } else if (state_.coupling_forced[i] == 2) {
            state_.coupling_flux[i] += state_.coupling_force_val[i];
        }
    }

    // Resolve time-varying / rating-curve boundary driving values for this step
    // (host-side; the kernels read the resolved edge_bc_head / edge_bc_flow).
    // No-op for WALL / NORMAL_FLOW and for constant SPECIFIED_* edges.
    resolveBoundaryValues(ctx, t);

    // Quiescence short-circuit: a window over a mesh holding NO water, with no
    // source that could wet it, cannot change the 2D state — skip the solver
    // advance entirely. Dry weather dominates continuous simulations, so this
    // caps the 2D cost at the (cheap) checks below whenever the surface is
    // genuinely idle. The checks are conservative: any water volume, any
    // nonzero rainfall/coupling source (runtime forcings were folded into
    // these arrays above), any boundary type that can push water in, or the
    // live-coupling path (whose exchange is evaluated inside the RHS, not in
    // coupling_flux) disables the skip.
    bool quiescent = !live_coupling;
    if (quiescent) {
        const int ntq = mesh_.n_triangles();
        for (int i = 0; i < ntq; ++i) {
            if (state_.volume[i] > 0.0 || state_.rainfall[i] != 0.0
                || state_.coupling_flux[i] != 0.0) { quiescent = false; break; }
        }
    }
    if (quiescent) {
        const int ne = boundary_.size();
        for (int idx = 0; idx < ne; ++idx) {
            const auto bt = static_cast<BoundaryType>(boundary_.edge_bc_type[idx]);
            if (bt != BoundaryType::WALL && bt != BoundaryType::NORMAL_FLOW) {
                quiescent = false;
                break;
            }
        }
    }
    if (quiescent) {
        ++quiescent_windows_;
        // No un-booking needed here (unlike the old window-sampled path): a
        // quiescent window implies every per-step exchange in it was zero —
        // any nonzero accumulator would have left coupling_flux nonzero above
        // and disabled the skip — so the 1D consumed nothing all window.
    }

    bool advance_failed = false;

    // TEMP overdraw diagnostics (OPENSWMM_2D_DEBUG_COUPLE=1).
    static const bool dbg_couple =
        (std::getenv("OPENSWMM_2D_DEBUG_COUPLE") != nullptr);
    const double dbg_vol_before = dbg_couple ? totalVolume() : 0.0;
    double dbg_drain_booked = 0.0, dbg_spill_booked = 0.0, dbg_outf_booked = 0.0;
    if (dbg_couple) {
        for (std::size_t k = 0; k < coupling_points_.size(); ++k) {
            const double j = window_exchange_accum_[k];
            if (j > 0) dbg_drain_booked += j; else dbg_spill_booked += -j;
            dbg_outf_booked += window_outfall_accum_[k];
        }
    }

#ifdef OPENSWMM_HAS_2D
    if (!quiescent) {
    // Rebuild the dry-cell active-set mask for this window. All wetting
    // mechanisms are final here (held exchange, outfall transfer, rainfall,
    // forcings, resolved BCs), so the seed set is complete; the halo gives
    // the front room to move within the window (breach-checked below).
    if (active_set_.enabled)
        rebuildActiveSet(mesh_, state_, &boundary_,
                         &coupling_points_, options_, active_set_,
                         live_coupling);

    // Advance CVODE by dt
    double t_target = sim_time_ + dt;
    double t_reached = solver_->advance(sim_time_, t_target);

    // Breach check: if the wet front crossed the WHOLE halo within this one
    // window (an outer-ring cell got wet), the wall guard has locally walled
    // the front — conservative but wrong at the edge. Discard the window,
    // restore the start-of-window state, widen the halo, and redo once. A
    // second breach keeps the (mass-safe, walled) result and disables masking
    // for the rest of the run.
    if (active_set_.enabled && (dt > 0.0) && t_reached > sim_time_
        && activeSetBreached(mesh_, state_, active_set_)) {
        ++active_set_.halo_trip_count;
        state_.reset_state();
        const int ntr = mesh_.n_triangles();
        for (int i = 0; i < ntr; ++i)
            state_.head[i] = headFromMeanDepth(mesh_, options_, i,
                                               state_.depth[i]);
        active_set_.halo_rings = std::min(2 * active_set_.halo_rings, 16);
        rebuildActiveSet(mesh_, state_, &boundary_,
                         &coupling_points_, options_, active_set_,
                         live_coupling);
        solver_->reinitialize(sim_time_);
        t_reached = solver_->advance(sim_time_, t_target);
        if (t_reached > sim_time_
            && activeSetBreached(mesh_, state_, active_set_)) {
            active_set_.enabled = false;
            state_.active_set   = nullptr;
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                "WARNING: 2D wet front outran the active-set halo twice in one "
                "window at t=%.1f s; masking disabled for the rest of the run "
                "(results stay conservative).", sim_time_);
            ctx.warnings.push_back(buf);
        }
    }

    // Advance failure (e.g. MAX_CVODE_STEPS exhausted): the solver left the 2D
    // state unchanged, but its internal integrator clock may sit anywhere in the
    // window. Previously this was silently ignored — time moved on while the
    // exchanges the 1D side already consumed were still booked to a surface
    // that never integrated them, desynchronising the ledgers. Instead: freeze
    // the 2D domain for this window (state is already the window-start state),
    // resync the integrator to it, and un-book the held exchanges so neither
    // domain receives water the other never moved.
    advance_failed = (dt > 0.0) && !(t_reached > sim_time_);
    if (advance_failed) {
        ++failed_advance_windows_;
        if (failed_advance_windows_ == 1) {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                "WARNING: 2D solver advance failed at t=%.1f s (window %.3f s); "
                "the surface is held frozen for this window. Total occurrences "
                "are reported at the end of the run.", sim_time_, dt);
            ctx.warnings.push_back(buf);
        }
        // Volume-exact resync: reinitialize() would reseed from the clamped
        // head reconstruction and zero any negative-volume debt, creating
        // water on every failed window (the dominant residual leak in the
        // failed-window regime).
        solver_->resyncFromVolumes(sim_time_);
        // The 1D already consumed its side of the exchange per routing step,
        // but the frozen surface never moved the matching water. Redeliver the
        // window's accumulated junction volumes NEGATED through the delivery
        // queue: the 1D gives back what the 2D never gave up (and vice versa
        // for spills), so the combined system stays mass-conservative.
        // Spread over a window-length of routing steps to avoid a pulse.
        // (Outfall accumulators: the 1D's outfall discharge is part of its
        // normal outflow accounting, not lateral inflow — matching the old
        // failure handling, the undelivered surface source is simply not
        // booked to the 2D ledger.)
        if (!live_coupling) {
            bool requeued = false;
            for (std::size_t k = 0; k < coupling_points_.size(); ++k) {
                const auto& cp = coupling_points_[k];
                if (cp.is_outfall || window_exchange_accum_[k] == 0.0) continue;
                ctx.nodes.coupling_queue[static_cast<std::size_t>(cp.node_idx)] +=
                    -window_exchange_accum_[k] * options_.flow_2d_to_1d;
                requeued = true;
            }
            if (requeued)
                ctx.coupling_delivery_remaining =
                    std::max(ctx.coupling_delivery_remaining, dt);
        }

        // Stability guard: halve the macro window so the retry pressure eases
        // (a window below the routing step degenerates to fire-every-step);
        // recover ×2 toward the resolved target after a run of clean windows
        // (below).
        if (effective_window_ > 0.0) {
            effective_window_ = std::max(1.0e-3, 0.5 * effective_window_);
            clean_windows_ = 0;
        }
    } else if (effective_window_ > 0.0 && effective_window_ < window_target_
               && ++clean_windows_ >= 20) {
        // Guard recovery: 20 consecutive clean windows → double back toward
        // the configured/AUTO target.
        effective_window_ = std::min(window_target_, 2.0 * effective_window_);
        clean_windows_ = 0;
    }

    // Live-path booking: the solver integrated ∫Q_k dt (m³, +drain/−spill) per
    // node-coupling point over the window. Store the exchange VOLUME (in 1D flow
    // units, ft³) into coupling_volume so the 1D engine consumes exactly that
    // volume over the next step (assembleLateralInflows divides by that step's dt)
    // and accumulateMassBalance() below books the identical volume into the 2D
    // ledger — conservative under VARIABLE_STEP. Cleared then accumulated so
    // multiple points sharing a node sum correctly.
    if (live_coupling && dt > 0.0 && !advance_failed) {
        const std::vector<double>& exch = solver_->last_coupling_exchange();
        for (const auto& cp : node_coupling_points_)
            ctx.nodes.coupling_volume[static_cast<std::size_t>(cp.node_idx)] = 0.0;
        const std::size_t n = std::min(exch.size(), node_coupling_points_.size());
        for (std::size_t k = 0; k < n; ++k) {
            const auto ni = static_cast<std::size_t>(node_coupling_points_[k].node_idx);
            ctx.nodes.coupling_volume[ni] += exch[k] * options_.flow_2d_to_1d;
        }
    }

    // (The held-path deviation reconciliation was deleted in Phase 3 with the
    // deviation itself: the mean-rate held source carries the full window
    // exchange with no in-solver quadrature to reconcile.)

    // Refresh the all-vertex pseudo-Laplacian heads once per accepted window
    // (Phase 1 hoist: this pass used to run inside every RHS/Jv evaluation).
    // Every between-window consumer — output plugins, held-exchange
    // evaluation, outfall boundary updates, next window's inject weights —
    // reads exactly the values the in-RHS pass would have left behind, since
    // the cell heads are frozen between windows.
    reconstructVertexHeads(mesh_, state_, options_.num_threads);

    // Refresh accepted-state output gradients once per window for snapshots
    // and API reads instead of inside every CVODE nonlinear/Jv evaluation.
    refreshOutputGradients(mesh_, state_, options_);

    // Refresh edge fluxes at the final accepted (head, depth) so the saved
    // fluxes, the per-cell continuity residual, and the reconstructed cell
    // velocities are all consistent with the reported solution. The last
    // in-solver flux evaluation can sit at a Newton/JvP perturbation point,
    // not the accepted state. In INERTIAL mode the edge flux is the prognostic
    // discharge q (already projected into state_.edge_flux by the solver's
    // advance), NOT the DW formula — so skip the DW recompute there.
    if (options_.momentum != MomentumType::INERTIAL)
        computeEdgeFluxes(mesh_, state_, options_);
    }  // !quiescent
#endif

    // Boundary outflow over the step: integrate the refreshed end-of-step
    // boundary edge fluxes (inflow-positive) as −F·dt (outward-positive) into
    // the cumulative tracker the 2D mass balance reads (accumulateMassBalance).
    // First-order in dt (end-of-step flux), consistent with computeCellContinuity.
    // Skipped for a failed (frozen) or quiescent (skipped) window: the state
    // did not change, so no volume actually crossed the boundary — integrating
    // a stale nonzero flux from the last fired window would book phantom
    // outflow.
    if (!advance_failed && !quiescent) {
        const int ne = boundary_.size();
        for (int idx = 0; idx < ne; ++idx) {
            if (static_cast<BoundaryType>(boundary_.edge_bc_type[idx])
                != BoundaryType::WALL) {
                boundary_.edge_bc_cum_flux[idx] += -state_.edge_flux[idx] * dt;
            }
        }
    }

    if (dbg_couple) {
        double eps_sum = 0.0;
        if (!live_coupling && !advance_failed) {
            const auto& eps = solver_->last_coupling_exchange();
            for (double e : eps) eps_sum += e;
        }
        const double vol_after = totalVolume();
        double rain_vol = 0.0;
        const int ntq = mesh_.n_triangles();
        for (int i = 0; i < ntq; ++i)
            rain_vol += state_.rainfall[static_cast<std::size_t>(i)]
                        * mesh_.tri_area[static_cast<std::size_t>(i)] * dt;
        const double net_booked_2d_out =
            dbg_drain_booked - dbg_spill_booked - dbg_outf_booked;
        // Conservation: dVol == rain_in − net_coupling_out (no evap/boundary here).
        const double expect_dVol = rain_vol - net_booked_2d_out;
        std::fprintf(stderr,
            "[couple] t=%.1f dt=%.2f fail=%d quiet=%d | dVol=%.3f rain=%.3f "
            "drain=%.3f spill=%.3f outf=%.3f net_out=%.3f eps=%.4f | "
            "expect_dVol=%.3f RESID=%.3f\n",
            t, dt, int(advance_failed), int(quiescent),
            vol_after - dbg_vol_before, rain_vol,
            dbg_drain_booked, dbg_spill_booked, dbg_outf_booked,
            net_booked_2d_out, eps_sum, expect_dVol,
            (vol_after - dbg_vol_before) - expect_dVol);
    }

    sim_time_ += dt;

    // Per-cell continuity residual (local mass-balance diagnostic). old_depth
    // holds the start-of-step depth saved by save_state() above; depth now
    // holds the end-of-step value.
    computeCellContinuity(mesh_, state_, options_, dt);

    // Cell-centred velocity reconstruction (RT0) from the refreshed fluxes.
    computeFaceVelocity(mesh_, state_, options_);

    // Render/output-only vertex free surface (signed depths) from the accepted
    // end-of-window depths. Wet-masked so dry-cell bed elevations never lift
    // the rendered water surface (solver vert_head keeps its own semantics).
    reconstructVertexRenderDepths(mesh_, state_, options_.dry_depth,
                                  options_.num_threads);

    // Update statistics
    state_.update_statistics(mesh_.tri_area, dt, options_.num_threads);

    // Accumulate the global 2D mass-balance terms for this step. A failed
    // (frozen) window moved no water — rainfall/evaporation/exchange were not
    // integrated, and the accumulated junction volumes were re-queued back to
    // the 1D above — so booking them would inject phantom volume.
    if (!advance_failed) accumulateMassBalance(ctx, dt);

    // LIVE path only: hand the window's junction-exchange volumes (booked as
    // window totals by the live-RHS block above) to the 1D side as a delivery
    // QUEUE — assembleLateralInflows drains it at the uniform rate
    // queue / coupling_delivery_remaining over the following routing steps, so
    // the multi-step window total does not land as a single-step pulse. MUST
    // run AFTER accumulateMassBalance (which reads coupling_volume as "this
    // window's exchange"). The default per-step path books coupling_volume in
    // step-sized increments consumed directly at the next routing step — its
    // volumes never route through this queue.
    if (live_coupling) {
        bool queued = false;
        for (const auto& cp : coupling_points_) {
            if (cp.is_outfall) continue;
            auto ni = static_cast<std::size_t>(cp.node_idx);
            if (ctx.nodes.coupling_volume[ni] != 0.0) {
                ctx.nodes.coupling_queue[ni] += ctx.nodes.coupling_volume[ni];
                ctx.nodes.coupling_volume[ni] = 0.0;
                queued = true;
            }
        }
        if (queued) ctx.coupling_delivery_remaining = dt;
    }

    // Refresh the cached CFL hint from the just-accepted state. The 1D engine
    // consults computeCflHint() every routing step, but the 2D state only
    // changes here — computing once per advance replaces an O(n_triangles)
    // scan per routing step with a cached read.
    updateCflHint();

    // Clear RESET forcings
    state_.clear_reset_forcings();

    // Re-seed the withdrawal budget from the just-accepted state and zero the
    // window accumulators + series for the next window (all outcomes: the
    // failed path re-queued its volumes above; the quiescent path had none).
    resetWindowAccumulators();
}


void SurfaceRouter2D::finalize(SimulationContext& ctx) {
    // Flush the partial macro-step window: with time-based or step-count
    // gating, up to one window of routing time can be pending here — dropping
    // it would end the 2D clock (and the exchange booking) short of the
    // simulation end.
    if (active_ && pending_dt_ > 0.0) {
        const double dt = pending_dt_;
        pending_dt_ = 0.0;
        force_next_window_ = false;
        fireAdvanceWindow(ctx, dt, last_t_);
    }
#ifdef OPENSWMM_HAS_2D
    if (solver_) {
        // Publish cumulative integrator statistics BEFORE finalize() frees the
        // solver memory the counters live in (the "2D Solver Statistics" report
        // block reads these — the throughput numbers each reformulation phase
        // is measured against).
        const auto s = solver_->run_stats();
        auto& mb = ctx.mass_balance_2d;
        mb.solver_nsteps         = s.nsteps;
        mb.solver_nrhs           = s.nrhs;
        mb.solver_nrhs_ls        = s.nrhs_ls;
        mb.solver_nni            = s.nni;
        mb.solver_nli            = s.nli;
        mb.solver_nsetups        = s.nsetups;
        mb.solver_netfails       = s.netfails;
        mb.solver_nncfails       = s.nncfails;
        mb.solver_failed_windows = failed_advance_windows_;
        mb.solver_last_h         = s.last_h;
        mb.solver_avg_h          = (s.nsteps > 0 && sim_time_ > 0.0)
                                       ? sim_time_ / static_cast<double>(s.nsteps)
                                       : 0.0;
        solver_->finalize();
    }
#endif
    if (outfall_clamp_windows_ > 0) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "WARNING: 2D outfall withdrawal was capped by the water available "
            "on the surface in %ld advance window(s); check the outfall stage "
            "coupling and the 2D continuity block.", outfall_clamp_windows_);
        ctx.warnings.push_back(buf);
    }
    if (failed_advance_windows_ > 0) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "WARNING: the 2D solver failed to integrate %ld advance window(s); "
            "the surface was held frozen over them. Consider raising "
            "MAX_CVODE_STEPS or the tolerances.", failed_advance_windows_);
        ctx.warnings.push_back(buf);
    }
    active_ = false;
}


double SurfaceRouter2D::computeCflHint(const SimulationContext& /*ctx*/) const {
    if (!active_) return 1.0e30;
    return cfl_hint_;
}


void SurfaceRouter2D::updateCflHint() {
    // Per-cell celerity constraint dt_i = sqrt(A_i) / sqrt(g·h_i), evaluated
    // ONLY over the coupling-stencil cells (cfl_cells_): the explicit 1D↔2D
    // exchange is what the hint stabilises — the 2D interior is fully implicit
    // and imposes no routing-step limit. Pairing each cell's own depth with
    // its own size is the physical constraint; the previous global-max-depth ×
    // global-min-cell-size pairing over the whole mesh let one wet cell
    // anywhere pin the 1D routing step for the entire run. Dry cells impose
    // nothing.
    double hint = 1.0e30;
    for (int i : cfl_cells_) {
        const double h = state_.depth[i];
        if (h < options_.dry_depth) continue;
        const double dx = std::sqrt(mesh_.tri_area[i]);
        if (dx < 1.0e-6) continue;
        const double c = std::sqrt(9.80665 * h);
        hint = std::min(hint, dx / std::max(c, 1.0e-6));
    }
    cfl_hint_ = hint;
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

    // Coupling and outfall exchange, from the per-window accumulators (m³,
    // SI-native, already capped/clamped — exactly what the 2D domain was asked
    // to move). Junction sign: + = 2D→1D drain (out of 2D), − = 1D→2D spill.
    // Outfall sign: + = pipe discharge into 2D (source), − = withdrawal. On
    // the live macro-step path the junction exchange is instead booked as a
    // per-node window total in nodes.coupling_volume (written by the live
    // booking block above) — read it with a per-node dedupe, matching the
    // per-node aggregation of that path.
    const bool live = (state_.node_coupling != nullptr);
    std::unordered_set<int> seen;
    for (std::size_t k = 0; k < coupling_points_.size(); ++k) {
        const auto& cp = coupling_points_[k];
        if (cp.is_outfall) {
            const double v = window_outfall_accum_[k];
            if (v > 0.0) mb.outfall_in  += v;
            else         mb.outfall_out += -v;
        } else if (!live) {
            const double v = window_exchange_accum_[k];
            if (v > 0.0) mb.coupling_2d_to_1d_out += v;
            else         mb.coupling_1d_to_2d_in  += -v;
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
