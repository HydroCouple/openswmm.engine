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

    // Seed the global 2D mass balance. init_storage is the surface volume at
    // the dry initial condition (0 unless a nonzero initial depth is set).
    ctx.mass_balance_2d.active = true;
    ctx.mass_balance_2d.init_storage = totalVolume();
    ctx.mass_balance_2d.final_storage = ctx.mass_balance_2d.init_storage;
    prev_boundary_cum_ = 0.0;
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


void SurfaceRouter2D::advancePostRouting(SimulationContext& ctx, double dt,
                                          double t) {
    if (!active_) return;

    // Subcycling support
    coupling_counter_++;
    if (options_.coupling_interval > 0
        && coupling_counter_ < options_.coupling_interval) {
        return;  // Not yet time to advance 2D
    }
    coupling_counter_ = 0;

    // Save 2D state
    state_.save_state();

    // Compute coupling exchange flows
    computeCouplingExchange(coupling_points_, mesh_, state_, ctx, options_, dt);

    // Transfer outfall discharges into 2D cells
    transferOutfallDischarges(coupling_points_, mesh_, state_, ctx, options_, dt);

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

#ifdef OPENSWMM_HAS_2D
    // Advance CVODE by dt
    double t_target = sim_time_ + dt;
    solver_->advance(sim_time_, t_target);

    // Refresh edge fluxes at the final accepted (head, depth) so the saved
    // fluxes, the per-cell continuity residual, and the reconstructed cell
    // velocities are all consistent with the reported solution. The last
    // in-solver flux evaluation can sit at a Newton/JvP perturbation point,
    // not the accepted state.
    computeEdgeFluxes(mesh_, state_, options_);
#endif

    // Boundary outflow over the step: integrate the refreshed end-of-step
    // boundary edge fluxes (inflow-positive) as −F·dt (outward-positive) into
    // the cumulative tracker the 2D mass balance reads (accumulateMassBalance).
    // First-order in dt (end-of-step flux), consistent with computeCellContinuity.
    {
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

    // Accumulate the global 2D mass-balance terms for this step.
    accumulateMassBalance(ctx, dt);

    // Stiffness-attribution diagnostic row (opt-in via OPENSWMM_2D_DIAG_CSV).
    writeDiagRow(ctx, dt, sim_time_);

    // Clear RESET forcings
    state_.clear_reset_forcings();
}


void SurfaceRouter2D::finalize() {
#ifdef OPENSWMM_HAS_2D
    if (solver_) {
        solver_->finalize();
    }
#endif
    active_ = false;
}


double SurfaceRouter2D::computeCflHint(const SimulationContext& /*ctx*/) const {
    if (!active_) return 1.0e30;

    // Estimate maximum wave speed from maximum depth and minimum cell size
    double max_depth = 0.0;
    double min_dx = 1.0e30;

    int nt = mesh_.n_triangles();
    for (int i = 0; i < nt; ++i) {
        max_depth = std::max(max_depth, state_.depth[i]);

        // Characteristic cell size ~ sqrt(area)
        double dx = std::sqrt(mesh_.tri_area[i]);
        min_dx = std::min(min_dx, dx);
    }

    if (max_depth < options_.dry_depth || min_dx < 1.0e-6) {
        return 1.0e30;  // All dry — no CFL constraint
    }

    // Shallow water wave speed ~ sqrt(g * h)
    double c = std::sqrt(9.80665 * max_depth);
    return min_dx / std::max(c, 1.0e-6);
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
    int nt = mesh_.n_triangles();
    int n_gages = ctx.n_gages();

    if (n_gages <= 0) {
        std::fill(state_.rainfall.begin(), state_.rainfall.end(), 0.0);
        return;
    }

    // Phase 1: use first available gage's current rainfall for all cells
    // Future: natural neighbour interpolation across all gages
    double rain_rate = ctx.gages.rainfall[0];  // User units (in/hr or mm/hr)

    // Convert to m/s
    double rain_m_per_s;
    if (ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units)) == 0) {
        // US customary (CFS/GPM/MGD): in/hr → m/s
        rain_m_per_s = rain_rate * 0.0254 / 3600.0;
    } else {
        // SI (CMS/LPS/MLD): mm/hr → m/s
        rain_m_per_s = rain_rate * 0.001 / 3600.0;
    }

    for (int i = 0; i < nt; ++i) {
        state_.rainfall[i] = rain_m_per_s;
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

    // Coupling and outfall exchange. nodes.coupling_inflow / nodes.outflow are
    // in the 1D engine's flow units (ft³/s for US); convert back to SI (m³/s).
    // coupling_inflow is a per-NODE total (computeCouplingExchange accumulates
    // all coupling points for a node into it), so dedupe by node index to
    // avoid double counting when several points map to one node.
    std::unordered_set<int> seen;
    for (const auto& cp : coupling_points_) {
        if (!seen.insert(cp.node_idx).second) continue;
        auto ni = static_cast<std::size_t>(cp.node_idx);
        if (cp.is_outfall) {
            // Net signed 1D→2D exchange at the outfall — the same quantity
            // transferOutfallDischarges injects into coupling_flux. Positive
            // (inflow) = pipe discharge into 2D (source); negative (outflow) =
            // surface water drawn back into the pipe (withdrawal).
            double q = (ctx.nodes.inflow[ni] - ctx.nodes.outflow[ni])
                       * options_.flow_1d_to_2d;
            if (q > 0.0) mb.outfall_in  += q * dt;
            else         mb.outfall_out += -q * dt;
        } else {
            // Positive = 2D→1D drainage (out of 2D); negative = 1D→2D spill.
            double q = ctx.nodes.coupling_inflow[ni] * options_.flow_1d_to_2d;
            if (q > 0.0) mb.coupling_2d_to_1d_out += q * dt;
            else         mb.coupling_1d_to_2d_in  += -q * dt;
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
                       "total_vol_m3,sum_abs_coupling_m3s,max_node_exch_m3s\n";
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
    // weir-flanking junctions that dominate the late-regime stiffness.
    double max_node_exch = 0.0;
    for (const auto& cp : coupling_points_) {
        const auto ni = static_cast<std::size_t>(cp.node_idx);
        const double q = std::abs(ctx.nodes.coupling_inflow[ni])
                         * options_.flow_1d_to_2d;
        if (q > max_node_exch) max_node_exch = q;
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
                 << max_node_exch << '\n';
    diag_csv_->flush();
}

} // namespace openswmm::twoD
