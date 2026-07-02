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

    // Set initial heads from ground elevation
    for (int i = 0; i < mesh_.n_triangles(); ++i) {
        state_.head[i] = mesh_.tri_cz[i];
    }

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
    // macro-window (the held-flux source becomes unstable there). It is correct
    // and conservative, but CURRENTLY SLOW: the orifice is stiff and the
    // diffusion-stencil preconditioner does not yet capture its Jacobian, so
    // CVODE cannot take the large step (see §6 of the plan — a coupling-aware
    // preconditioner is the remaining piece). Kept behind an env flag so the
    // default + the fast held macro-step (COUPLING_INTERVAL) paths are unaffected.
    // Build the non-outfall point list and publish it (+ the 1D node data, frozen
    // during an advance) on the state BEFORE solver_->initialize(), which sizes
    // the augmented state vector (nt cells + one ∫Q dt accumulator per point).
    // Outfall coupling stays on the held path (transferOutfallDischarges).
    if (std::getenv("OPENSWMM_2D_LIVE_COUPLING") != nullptr) {
        node_coupling_points_.clear();
        for (const auto& cp : coupling_points_)
            if (!cp.is_outfall) node_coupling_points_.push_back(cp);
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

#ifdef OPENSWMM_HAS_2D
    // Fold the OPENSWMM_2D_MOMENTUM env override into options_ so it is the single
    // source of truth for both the solver (which RHS split) and the post-advance
    // edge-flux handling below (DW recompute vs. inertial q-projection).
    if (const char* m = std::getenv("OPENSWMM_2D_MOMENTUM")) {
        if (std::strcmp(m, "inertial") == 0) options_.momentum = MomentumType::INERTIAL;
        else if (std::strcmp(m, "dw") == 0)  options_.momentum = MomentumType::DW;
    }

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
    coupling_counter_ = 0;
    sim_time_ = 0.0;
    pending_dt_ = 0.0;

    // COUPLING_INTERVAL > 1 advances the 2D solver over a multi-routing-step
    // macro-window so CVODE can take large adaptive steps (big speedup). This is
    // an EXPLICIT coupling sub-cycle, so the window length is CFL-limited by the
    // 1D↔2D coupling stiffness: small windows stay conservative, but too large a
    // window destabilises the exchange (oscillation → clamped negative cells →
    // mass loss). Always confirm the reported 2D continuity after enabling it.
    if (options_.coupling_interval > 1) {
        std::fprintf(stderr,
            "[openswmm 2D] WARNING: COUPLING_INTERVAL=%d advances the 2D solver "
            "over a %d-step macro-window (experimental). This is an explicit "
            "coupling sub-cycle and is CFL-limited — verify the 2D continuity "
            "error in the report; reduce the interval if it grows.\n",
            options_.coupling_interval, options_.coupling_interval);
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
            std::fprintf(stderr,
                "[openswmm 2D] NOTICE: ACTIVE_SET requires the CVODE "
                "diffusive-wave solver; masking disabled for this run.\n");
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
    // Explicit COUPLING_WINDOW wins; AUTO (−1, the default) defers to an
    // explicit legacy COUPLING_INTERVAL > 1, otherwise the user's declared
    // [OPTIONS] ROUTING_STEP is taken as the acceptable coupling cadence —
    // variable-step collapse of the 1D solver then cannot drag the 2D advance
    // cadence (and its per-advance cost) down with it. Behavior-preserving for
    // healthy models: window == nominal routing step fires every step.
    {
        double win = options_.coupling_window;
        if (const char* env = std::getenv("OPENSWMM_2D_COUPLING_WINDOW")) {
            char* endp = nullptr;
            const double v = std::strtod(env, &endp);
            if (endp != env) win = v;
        }
        if (win < 0.0) {
            win = (options_.coupling_interval > 1)
                      ? 0.0  // legacy step-count gating stays in charge
                      : std::min(ctx.options.routing_step, options_.max_timestep);
        }
        effective_window_ = std::max(0.0, win);
        window_target_    = effective_window_;
        clean_windows_    = 0;
    }
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

    // Macro-step subcycling. Accumulate the routing time elapsed since the last
    // 2D advance, then (when it is time to fire) integrate the 2D solver over
    // the WHOLE accumulated window in a single advance() call. This lets CVODE
    // take large adaptive internal steps instead of being hard-stopped on every
    // ~routing-step boundary — the dominant cost driver for the semi-discrete
    // solver. Two gating policies:
    //   - Time-based (effective_window_ > 0, the default via COUPLING_WINDOW
    //     AUTO = nominal ROUTING_STEP): fire when the accumulated routing time
    //     reaches the window. The window is physical time, so 1D variable-step
    //     collapse cannot shrink the 2D cadence — exactly the regime where a
    //     step-count interval degenerates to advance-per-0.2s.
    //   - Legacy step-count (COUPLING_INTERVAL > 1 with COUPLING_WINDOW unset).
    // The coupling / forcing source terms are held constant across the window,
    // and every downstream term (the solver advance, sim_time_, boundary-flux,
    // continuity, statistics, and the mass-balance ledgers) uses the same
    // accumulated `dt`, so the 1D↔2D exchange stays conservative over the macro
    // step. dt varies under VARIABLE_STEP, so accumulate the actual elapsed time.
    pending_dt_ += routing_dt;
    ++coupling_counter_;
    last_t_ = t;
    if (effective_window_ > 0.0) {
        // Fire when within half a routing step of the window (jitter cannot
        // systematically overshoot: the alternative is overshooting by a whole
        // step). A window equal to the actual routing step fires every step.
        if (pending_dt_ + 0.5 * routing_dt < effective_window_) return;
    } else if (options_.coupling_interval > 1
               && coupling_counter_ < options_.coupling_interval) {
        return;  // Defer the 2D advance; keep accumulating elapsed routing time.
    }
    const double dt = pending_dt_;   // the 2D macro-step
    pending_dt_ = 0.0;
    coupling_counter_ = 0;
    fireAdvanceWindow(ctx, dt, t);
}


void SurfaceRouter2D::fireAdvanceWindow(SimulationContext& ctx, double dt,
                                         double t) {
    // Save 2D state
    state_.save_state();

    // Node coupling. Default (held-flux) path: pre-compute the per-window
    // exchange here and inject it as a held source + 1D lateral inflow. Live
    // macro-step path (state_.node_coupling set, COUPLING_INTERVAL > 1): the
    // orifice is evaluated INSIDE the CVODE RHS against the live 2D head so it
    // self-limits and stays stable over the large window; the conservative
    // ∫Q dt it integrates is booked to the 1D node + 2D ledger AFTER the advance.
    const bool live_coupling = (state_.node_coupling != nullptr);
    if (!live_coupling)
        computeCouplingExchange(coupling_points_, mesh_, state_, ctx, options_, dt);
    else
        // The held path zeroes coupling_flux inside computeCouplingExchange;
        // the live path skips it, and scatterCouplingFlux ACCUMULATES (+=), so
        // without this reset the per-window outfall injection below compounds
        // across windows (and a failed window's flux would leak into the next).
        std::fill(state_.coupling_flux.begin(), state_.coupling_flux.end(), 0.0);

    // Transfer outfall discharges into 2D cells (both paths). Withdrawal is
    // capped at the water available in the receiving cells; the ledger books
    // the applied (clamped) rates from outfall_applied_q_.
    if (transferOutfallDischarges(coupling_points_, mesh_, state_, ctx, options_,
                                  dt, outfall_applied_q_) > 0)
        ++outfall_clamp_windows_;

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
        // The solver will not run, so no exchange can be applied this window.
        // Un-book the held exchanges (mirrors the failure path): a runtime
        // coupling forcing can zero coupling_flux AFTER computeCouplingExchange
        // / transferOutfallDischarges wrote their ledger entries, which would
        // otherwise book an exchange the surface never moved.
        for (const auto& cp : coupling_points_)
            if (!cp.is_outfall)
                ctx.nodes.coupling_volume[
                    static_cast<std::size_t>(cp.node_idx)] = 0.0;
        outfall_applied_q_.clear();
    }

    bool advance_failed = false;

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
            state_.head[i] = mesh_.tri_cz[i] + state_.depth[i];
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
            std::fprintf(stderr,
                "[openswmm 2D] WARNING: wet front outran the active-set halo "
                "twice in one window at t=%.1f s; masking disabled for the "
                "rest of the run (results stay conservative).\n", sim_time_);
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
            std::fprintf(stderr,
                "[openswmm 2D] WARNING: 2D solver advance failed at t=%.1f s "
                "(window %.3f s); the surface is held frozen for this window. "
                "Total occurrences are reported at the end of the run.\n",
                sim_time_, dt);
        }
        solver_->reinitialize(sim_time_);
        if (!live_coupling) {
            for (const auto& cp : coupling_points_)
                if (!cp.is_outfall)
                    ctx.nodes.coupling_volume[
                        static_cast<std::size_t>(cp.node_idx)] = 0.0;
        }
        outfall_applied_q_.clear();

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

    sim_time_ += dt;

    // Per-cell continuity residual (local mass-balance diagnostic). old_depth
    // holds the start-of-step depth saved by save_state() above; depth now
    // holds the end-of-step value.
    computeCellContinuity(mesh_, state_, options_, dt);

    // Cell-centred velocity reconstruction (RT0) from the refreshed fluxes.
    computeFaceVelocity(mesh_, state_, options_);

    // Update statistics
    state_.update_statistics(mesh_.tri_area, dt, options_.num_threads);

    // Accumulate the global 2D mass-balance terms for this step. A failed
    // (frozen) window moved no water — rainfall/evaporation/exchange were not
    // integrated, and the held exchanges were un-booked above — so booking
    // them would inject phantom volume into the ledger.
    if (!advance_failed) accumulateMassBalance(ctx, dt);

    // Refresh the cached CFL hint from the just-accepted state. The 1D engine
    // consults computeCflHint() every routing step, but the 2D state only
    // changes here — computing once per advance replaces an O(n_triangles)
    // scan per routing step with a cached read.
    updateCflHint();

    // Stiffness-attribution diagnostic row (opt-in via OPENSWMM_2D_DIAG_CSV).
    writeDiagRow(ctx, dt, sim_time_);

    // Clear RESET forcings
    state_.clear_reset_forcings();
}


void SurfaceRouter2D::finalize(SimulationContext& ctx) {
    // Flush the partial macro-step window: with time-based or step-count
    // gating, up to one window of routing time can be pending here — dropping
    // it would end the 2D clock (and the exchange booking) short of the
    // simulation end.
    if (active_ && pending_dt_ > 0.0) {
        const double dt = pending_dt_;
        pending_dt_ = 0.0;
        coupling_counter_ = 0;
        fireAdvanceWindow(ctx, dt, last_t_);
    }
#ifdef OPENSWMM_HAS_2D
    if (solver_) {
        solver_->finalize();
    }
#endif
    if (outfall_clamp_windows_ > 0) {
        std::fprintf(stderr,
            "[openswmm 2D] WARNING: outfall withdrawal was capped by the water "
            "available on the 2D surface in %ld advance window(s). The 1D "
            "network may have drawn tailwater the surface could not supply; "
            "check the outfall stage coupling and the 2D continuity block.\n",
            outfall_clamp_windows_);
    }
    if (failed_advance_windows_ > 0) {
        std::fprintf(stderr,
            "[openswmm 2D] WARNING: the 2D solver failed to integrate %ld "
            "advance window(s); the surface was held frozen over them. "
            "Consider raising MAX_CVODE_STEPS or the tolerances.\n",
            failed_advance_windows_);
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
    const int nt = mesh_.n_triangles();
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

    // Coupling and outfall exchange. The junction term reads nodes.coupling_volume
    // (the per-window exchange VOLUME, 1D ft³, just written above); the outfall term
    // reads nodes.inflow/outflow (1D flow units). Both convert to SI for the 2D
    // ledger. coupling_volume is a per-NODE total (computeCouplingExchange / the
    // live booking accumulate all coupling points for a node into it), so dedupe by
    // node index to avoid double counting when several points map to one node.
    std::unordered_set<int> seen;
    for (const auto& cp : coupling_points_) {
        if (!seen.insert(cp.node_idx).second) continue;
        auto ni = static_cast<std::size_t>(cp.node_idx);
        if (cp.is_outfall) {
            // Net signed 1D→2D exchange at the outfall — exactly the quantity
            // transferOutfallDischarges injected into coupling_flux this window
            // (AFTER its withdrawal cap), so the ledger matches what the 2D
            // domain actually received/gave up. Positive = pipe discharge into
            // 2D (source); negative = surface water drawn back (withdrawal).
            auto it = outfall_applied_q_.find(cp.node_idx);
            double q = (it != outfall_applied_q_.end()) ? it->second : 0.0;
            if (q > 0.0) mb.outfall_in  += q * dt;
            else         mb.outfall_out += -q * dt;
        } else {
            // Positive = 2D→1D drainage (out of 2D); negative = 1D→2D spill.
            // coupling_volume is the exchanged VOLUME for this window (1D units,
            // ft³, just written by computeCouplingExchange / the live booking);
            // convert ft³→m³ and book directly — no ·dt (already a volume). This
            // is the exact volume the 1D side will receive next step, so the two
            // ledgers stay matched under VARIABLE_STEP.
            double vol = ctx.nodes.coupling_volume[ni] * options_.vol_1d_to_2d;
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


void SurfaceRouter2D::writeDiagRow(SimulationContext& ctx, double dt, double t) {
    // Resolve the opt-in path once. Empty/unset env var → harness stays off.
    if (!diag_checked_) {
        diag_checked_ = true;
        const char* path = std::getenv("OPENSWMM_2D_DIAG_CSV");
        if (path && path[0]) {
            diag_csv_ = std::make_unique<std::ofstream>(path);
            if (diag_csv_ && diag_csv_->is_open()) {
                (*diag_csv_)
                    << "t_s,dt_s,flag,d_nsteps,d_newton,d_gmres,d_prec_setups,"
                       "d_lin_fails,last_h_s,n_front,n_wet,dn_wet,max_depth_m,"
                       "total_vol_m3,sum_abs_coupling_m3s,max_node_exch_m3s,"
                       "n_quiescent,n_active,halo_trips\n";
            } else {
                diag_csv_.reset();  // open failed — disable cleanly
            }
        }
    }
    if (!diag_csv_) return;

    // Per-cell wet/dry-front metrics from the accepted end-of-step depths.
    const int nt = mesh_.n_triangles();
    const double dry = options_.dry_depth;
    const double front_hi = 3.0 * dry;  // "on the front" band
    int n_front = 0, n_wet = 0;
    double max_depth = 0.0, sum_abs_coup = 0.0;
    for (int i = 0; i < nt; ++i) {
        const double d = state_.depth[i];
        if (d > dry) ++n_wet;
        if (d > 0.0 && d < front_hi) ++n_front;
        if (d > max_depth) max_depth = d;
        sum_abs_coup += std::abs(state_.coupling_flux[i]) * mesh_.tri_area[i];
    }
    const int dn_wet = std::abs(n_wet - diag_prev_nwet_);
    diag_prev_nwet_ = n_wet;

    // Largest single coupled-node exchange magnitude (m³/s) — surfaces the
    // weir-flanking junctions that dominate the late-regime stiffness. The
    // exchange is now carried as a per-window volume (coupling_volume, 1D ft³);
    // divide by dt for the rate this diagnostic reports.
    double max_node_exch = 0.0;
    if (dt > 0.0) {
        for (const auto& cp : coupling_points_) {
            const auto ni = static_cast<std::size_t>(cp.node_idx);
            const double q = std::abs(ctx.nodes.coupling_volume[ni] / dt)
                             * options_.flow_1d_to_2d;
            if (q > max_node_exch) max_node_exch = q;
        }
    }

    // Per-advance integrator deltas (zeros if no 2D solver compiled in).
    long flag = 0, d_nsteps = 0, d_newton = 0, d_gmres = 0,
         d_prec = 0, d_linf = 0;
    double last_h = 0.0;
#ifdef OPENSWMM_HAS_2D
    if (solver_) {
        const SolverAdvanceStats st = solver_->last_advance_stats();
        flag = st.flag; d_nsteps = st.d_nsteps; d_newton = st.d_newton;
        d_gmres = st.d_gmres; d_prec = st.d_prec_setups; d_linf = st.d_lin_fails;
        last_h = st.last_h;
    }
#endif

    (*diag_csv_) << t << ',' << dt << ',' << flag << ','
                 << d_nsteps << ',' << d_newton << ',' << d_gmres << ','
                 << d_prec << ',' << d_linf << ',' << last_h << ','
                 << n_front << ',' << n_wet << ',' << dn_wet << ','
                 << max_depth << ',' << totalVolume() << ',' << sum_abs_coup << ','
                 << max_node_exch << ',' << quiescent_windows_ << ','
                 << (active_set_.enabled ? active_set_.n_active() : -1) << ','
                 << active_set_.halo_trip_count << '\n';
    diag_csv_->flush();
}

} // namespace openswmm::twoD
