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
#include "../core/SimulationContext.hpp"
#include "../core/UnitConversion.hpp"

#include <stdexcept>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

namespace openswmm::twoD {

void SurfaceRouter2D::initialize(SimulationContext& ctx) {
    // Check if 2D sections were parsed (vertices present)
    if (mesh_.n_vertices() < 3 || mesh_.n_triangles() < 1) {
        active_ = false;
        return;
    }

    // Resolve unit-system conversion factors. The 2D solver runs internally in
    // SI, but the 1D engine computes in feet for US flow units. We convert at
    // the coupling boundary (NodeCoupling) and the mesh (below) so the SI
    // solver stays pure. SI projects yield factor 1.0 (no-op).
    {
        const int us = ucf::getUnitSystem(
            static_cast<int>(ctx.options.flow_units));
        const double ft_to_m = (us == 0) ? 0.3048 : 1.0;
        options_.len_1d_to_2d  = ft_to_m;
        options_.len_2d_to_1d  = 1.0 / ft_to_m;
        options_.vol_1d_to_2d  = ft_to_m * ft_to_m * ft_to_m;
        options_.flow_1d_to_2d = options_.vol_1d_to_2d;
        options_.flow_2d_to_1d = 1.0 / options_.vol_1d_to_2d;
    }

    // Convert mesh geometry from project length units (feet for US) to the SI
    // internal units the 2D solver expects. MUST run BEFORE buildMeshTopology
    // so all derived geometry (areas, edge lengths, centroids, midpoint Z) is
    // computed in SI. No-op for SI projects (factor 1.0). Coupling areas given
    // in the .inp are project-length² and scale by the squared factor.
    //
    // Skipped entirely when the producer declared `;; UNITS: SI (m)` on the
    // mesh file (options_.mesh_units_si == true): the values are already SI
    // and applying the factor a second time would scale the mesh down by
    // 0.3048 on US-FLOW_UNITS projects.  Coupling-side factors
    // (len_1d_to_2d, vol_1d_to_2d, flow_*) remain driven by FLOW_UNITS
    // because they describe the 1D side of the boundary, not the mesh.
    if (!options_.mesh_units_si && options_.len_1d_to_2d != 1.0) {
        const double f  = options_.len_1d_to_2d;
        const double f2 = f * f;
        for (auto& v : mesh_.vx) v *= f;
        for (auto& v : mesh_.vy) v *= f;
        for (auto& v : mesh_.vz) v *= f;
        for (auto& a : mesh_.vert_coupling_area) a *= f2;
        for (auto& a : mesh_.tri_coupling_area)  a *= f2;
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

    // Initialize per-edge boundary condition storage (n_triangles * 3 slots,
    // all initialized to WALL with zero head/slope/cum_flux).
    boundary_.resize(mesh_.n_triangles() * 3);

    // V-E3 — drain any [2D_BOUNDARY_CONDITIONS] rows the parser
    // accumulated into boundary_. Out-of-range (tri/edge beyond mesh
    // size) rows are silently skipped — defensive against partial INPs.
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
        pending_bc_rows_.clear();
    }

    // §11A — drain [2D_EDGE_CONVEYANCE] rows.
    //
    // Build a one-shot vertex-pair → list-of-(tri*3+edge_local) slot map by
    // walking the mesh once.  Each parsed (FROM, TO) row hashes into the
    // map (with vertices sorted so the key is direction-symmetric) and
    // writes the conveyance factor into every matching slot — naturally
    // mirroring across interior edges (Q3 silent partner mirror) and
    // gracefully handling the single-slot case for boundary edges.
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
                // Local edge e is opposite vertex e, connecting v[(e+1)%3] and v[(e+2)%3].
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
            // Q3 — silent partner mirroring: write the same factor into
            // every slot the edge-key resolves to (1 slot for boundary
            // edges, 2 for interior).  Duplicate parse-row last-write-wins
            // falls out naturally because we just overwrite.
            for (const int slot : it->second) {
                mesh_.edge_conveyance[slot] = r.conveyance;
            }
        }
        pending_edge_conveyance_rows_.clear();
    }

    // Set initial heads from ground elevation
    for (int i = 0; i < mesh_.n_triangles(); ++i) {
        state_.head[i] = mesh_.tri_cz[i];
    }

    // Build coupling point descriptors
    coupling_points_ = buildCouplingPoints(mesh_, ctx);

    // Suppress ponding for 2D-coupled nodes. The 2D surface owns the
    // surface storage, so any legacy ponded_area would double-count.
    //
    // C3a: warn (don't fail) when the user-supplied INP marks a coupled
    // node with sur_depth > 0 or ponded_area > 0. The engine still proceeds
    // — the surcharge gate in computeCouplingExchange honours sur_depth
    // (C1+C2), and ponded_area is zeroed below — but the user should know
    // that membership in [2D_VERTEX_NODE_MAP] / [2D_TRIANGLE_NODE_MAP] is
    // the unambiguous "uncapped" flag and the other two attributes have
    // their meaning narrowed to "physical surcharge cap" only.
    // See docs/1D_2D_COUPLING_GATE_REVIEW.md §6 (C3a).
    for (const auto& cp : coupling_points_) {
        if (cp.is_outfall) continue;
        auto ni = static_cast<std::size_t>(cp.node_idx);
        const auto& nname = ctx.node_names.name_of(cp.node_idx);
        if (ctx.nodes.sur_depth[ni] > 0.0) {
            ctx.warnings.push_back(
                "WARNING: 2D-coupled node '" + nname
                + "' has sur_depth > 0 — surcharge gate uses invert + "
                  "full_depth + sur_depth as the spill threshold "
                  "(z_top). Below z_top the orifice ramp is closed in "
                  "both directions; above z_top it opens.");
        }
        if (ctx.nodes.ponded_area[ni] > 0.0) {
            ctx.warnings.push_back(
                "WARNING: 2D-coupled node '" + nname
                + "' has ponded_area > 0 — the 2D mesh owns surface "
                  "storage for coupled nodes; ponded_area is being "
                  "zeroed.");
        }
        // Set ponded area to zero — 2D surface handles excess
        ctx.nodes.ponded_area[ni] = 0.0;
    }

#ifdef OPENSWMM_HAS_2D
    // Initialize CVODE solver
    cvode_solver_.initialize(mesh_, state_, options_);
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

    // Apply 2D forcings
    for (std::size_t i = 0; i < state_.depth.size(); ++i) {
        // Rainfall forcing
        if (state_.rainfall_forced[i] == 1) {
            state_.rainfall[i] = state_.rainfall_force_val[i];
        } else if (state_.rainfall_forced[i] == 2) {
            state_.rainfall[i] += state_.rainfall_force_val[i];
        }
        // Coupling forcing
        if (state_.coupling_forced[i] == 1) {
            state_.coupling_flux[i] = state_.coupling_force_val[i];
        } else if (state_.coupling_forced[i] == 2) {
            state_.coupling_flux[i] += state_.coupling_force_val[i];
        }
    }

#ifdef OPENSWMM_HAS_2D
    // Advance CVODE by dt
    double t_target = sim_time_ + dt;
    cvode_solver_.advance(sim_time_, t_target);

    // Refresh edge fluxes at the final accepted (head, depth) so the saved
    // fluxes, the per-cell continuity residual, and the reconstructed cell
    // velocities are all consistent with the reported solution. The last
    // in-solver flux evaluation can sit at a Newton/JvP perturbation point,
    // not the accepted state.
    computeEdgeFluxes(mesh_, state_, options_);
#endif

    sim_time_ += dt;

    // Per-cell continuity residual (local mass-balance diagnostic). old_depth
    // holds the start-of-step depth saved by save_state() above; depth now
    // holds the end-of-step value.
    computeCellContinuity(mesh_, state_, dt);

    // Cell-centred velocity reconstruction (RT0) from the refreshed fluxes.
    computeFaceVelocity(mesh_, state_, options_);

    // Update statistics
    state_.update_statistics(mesh_.tri_area, dt);

    // Accumulate the global 2D mass-balance terms for this step.
    accumulateMassBalance(ctx, dt);

    // Clear RESET forcings
    state_.clear_reset_forcings();
}


void SurfaceRouter2D::finalize() {
#ifdef OPENSWMM_HAS_2D
    cvode_solver_.finalize();
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
    double vol = 0.0;
    int nt = mesh_.n_triangles();
    for (int i = 0; i < nt; ++i) {
        vol += state_.depth[i] * mesh_.tri_area[i];
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


void SurfaceRouter2D::accumulateMassBalance(SimulationContext& ctx, double dt) {
    auto& mb = ctx.mass_balance_2d;
    int nt = mesh_.n_triangles();

    // Rainfall inflow (m³): rainfall is m/s after forcings are applied.
    double rain_vol = 0.0;
    for (int i = 0; i < nt; ++i) {
        rain_vol += state_.rainfall[i] * mesh_.tri_area[i];
    }
    mb.rainfall_in += rain_vol * dt;

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
            // 1D outfall discharge injected into the 2D cell as a source.
            double q = ctx.nodes.outflow[ni] * options_.flow_1d_to_2d;
            if (q > 0.0) mb.outfall_in += q * dt;
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

} // namespace openswmm::twoD
