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
#include "../core/SimulationContext.hpp"

#include <stdexcept>
#include <cmath>
#include <algorithm>
#include <numeric>

namespace openswmm::twoD {

void SurfaceRouter2D::initialize(SimulationContext& ctx) {
    // Check if 2D sections were parsed (vertices present)
    if (mesh_.n_vertices() < 3 || mesh_.n_triangles() < 1) {
        active_ = false;
        return;
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

    // Set initial heads from ground elevation
    for (int i = 0; i < mesh_.n_triangles(); ++i) {
        state_.head[i] = mesh_.tri_cz[i];
    }

    // Build coupling point descriptors
    coupling_points_ = buildCouplingPoints(mesh_, ctx);

    // Suppress ponding for 2D-coupled nodes
    for (const auto& cp : coupling_points_) {
        if (!cp.is_outfall) {
            auto ni = static_cast<std::size_t>(cp.node_idx);
            // Set ponded area to zero — 2D surface handles excess
            ctx.nodes.ponded_area[ni] = 0.0;
        }
    }

#ifdef OPENSWMM_HAS_2D
    // Initialize CVODE solver
    cvode_solver_.initialize(mesh_, state_, options_);
#endif

    active_ = true;
    coupling_counter_ = 0;
    sim_time_ = 0.0;
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
    updateOutfallBoundaries(coupling_points_, mesh_, state_, ctx);
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
    computeCouplingExchange(coupling_points_, mesh_, state_, ctx, dt);

    // Transfer outfall discharges into 2D cells
    transferOutfallDischarges(coupling_points_, mesh_, state_, ctx, dt);

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
#endif

    sim_time_ += dt;

    // Update statistics
    state_.update_statistics(mesh_.tri_area, dt);

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
    if (static_cast<int>(ctx.options.flow_units) < 3) {
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

} // namespace openswmm::twoD
