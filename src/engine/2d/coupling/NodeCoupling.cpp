/**
 * @file NodeCoupling.cpp
 * @brief Implementation of 2D↔1D node coupling via orifice exchange.
 *
 * @see NodeCoupling.hpp
 * @ingroup engine_2d
 */

#include "NodeCoupling.hpp"
#include "../../core/SimulationContext.hpp"

#include <cmath>
#include <algorithm>

namespace openswmm::twoD {

namespace {

constexpr double GRAVITY = 9.80665;  // m/s²

/// Orifice exchange: Q = Cd * A * sign(Δh) * sqrt(2g|Δh|)
inline double orificeFlow(double dh, double cd, double area) noexcept {
    if (std::abs(dh) < 1.0e-12) return 0.0;
    double sign = (dh > 0.0) ? 1.0 : -1.0;
    return sign * cd * area * std::sqrt(2.0 * GRAVITY * std::abs(dh));
}

/// Smooth effective area transition for uncapped surcharge nodes
inline double effectiveArea(double h_max, double z_ground, double full_depth,
                             double A_inlet, double A_manhole) noexcept {
    if (h_max < z_ground) return A_inlet;
    double d_trans = 0.05;  // 5 cm transition
    double frac = std::min((h_max - z_ground) / d_trans, 1.0);
    return A_inlet + frac * (A_manhole - A_inlet);
}

} // anonymous namespace


std::vector<CouplingPoint> buildCouplingPoints(const MeshData& mesh,
                                                const SimulationContext& ctx) {
    std::vector<CouplingPoint> cps;

    // Vertex-to-node couplings
    int nv = mesh.n_vertices();
    for (int v = 0; v < nv; ++v) {
        int node_idx = mesh.vert_coupled_node[v];
        if (node_idx < 0) continue;

        CouplingPoint cp;
        cp.vertex_idx = v;
        cp.node_idx = node_idx;
        cp.cd = mesh.vert_coupling_cd[v];
        cp.area = mesh.vert_coupling_area[v];

        // Find the triangle that contains this vertex (use first one)
        // The coupling flux is distributed to triangles sharing this vertex
        cp.cell_idx = -1;
        int nt = mesh.n_triangles();
        for (int t = 0; t < nt; ++t) {
            if (mesh.tri_v0[t] == v || mesh.tri_v1[t] == v || mesh.tri_v2[t] == v) {
                cp.cell_idx = t;
                break;
            }
        }
        if (cp.cell_idx < 0) continue;

        auto ui = static_cast<std::size_t>(node_idx);
        cp.is_outfall = (ctx.nodes.type[ui] == NodeType::OUTFALL);
        cp.has_flap_gate = cp.is_outfall && ctx.nodes.outfall_has_flap_gate[ui];

        cps.push_back(cp);
    }

    // Triangle-to-node couplings
    int nt = mesh.n_triangles();
    for (int t = 0; t < nt; ++t) {
        int node_idx = mesh.tri_coupled_node[t];
        if (node_idx < 0) continue;

        CouplingPoint cp;
        cp.cell_idx = t;
        cp.vertex_idx = -1;  // centroid coupling
        cp.node_idx = node_idx;
        cp.cd = mesh.tri_coupling_cd[t];
        cp.area = mesh.tri_coupling_area[t];

        auto ui = static_cast<std::size_t>(node_idx);
        cp.is_outfall = (ctx.nodes.type[ui] == NodeType::OUTFALL);
        cp.has_flap_gate = cp.is_outfall && ctx.nodes.outfall_has_flap_gate[ui];

        cps.push_back(cp);
    }

    return cps;
}


void computeCouplingExchange(const std::vector<CouplingPoint>& cps,
                              const MeshData& mesh,
                              SurfaceStateData& state,
                              SimulationContext& ctx,
                              double dt) {
    auto& nodes = ctx.nodes;
    auto& forcing = ctx.forcing;

    // Clear coupling fluxes
    std::fill(state.coupling_flux.begin(), state.coupling_flux.end(), 0.0);

    for (const auto& cp : cps) {
        if (cp.is_outfall) continue;  // Outfalls handled separately

        int ci = cp.cell_idx;
        auto ni = static_cast<std::size_t>(cp.node_idx);

        // 2D head at the coupling point
        double h_2d;
        if (cp.vertex_idx >= 0) {
            h_2d = state.vert_head[cp.vertex_idx];
        } else {
            h_2d = state.head[ci];
        }

        // 1D node head
        double h_1d = nodes.head[ni];

        // Head difference
        double dh = h_2d - h_1d;

        // For uncapped surcharged nodes, use effective area transition
        double z_ground = nodes.invert_elev[ni] + nodes.full_depth[ni];
        double h_max = std::max(h_1d, h_2d);
        double A_eff = effectiveArea(h_max, z_ground, nodes.full_depth[ni],
                                      cp.area, cp.area * 2.0);

        // Orifice exchange flow
        double Q = orificeFlow(dh, cp.cd, A_eff);

        // Throttle return flow (2D → 1D) if node is at capacity
        if (Q > 0.0) {
            // Q > 0 means flow from 2D into 1D node (drainage)
            double available = nodes.full_volume[ni] - nodes.volume[ni];
            if (available > 0.0 && dt > 0.0) {
                double Q_max = available / dt;
                Q = std::min(Q, Q_max);
            } else if (available <= 0.0) {
                // Node full — only allow if 1D head < 2D head (surcharge drain-back)
                if (h_1d >= h_2d) Q = 0.0;
            }
        }

        // Inject as lateral inflow into SWMM node via forcing API
        // Positive Q = flow from 2D → 1D (positive lateral inflow)
        forcing.node_lat_inflow_mode[ni]    = ForcingMode::ADD;
        forcing.node_lat_inflow_value[ni]  += Q;
        forcing.node_lat_inflow_persist[ni] = ForcingPersist::RESET;

        // Record coupling flux back to 2D cell (negative = drainage out of 2D)
        double tri_area = mesh.tri_area[ci];
        if (tri_area > 1.0e-30) {
            state.coupling_flux[ci] += -Q / tri_area;  // m/s sink
        }
    }
}


void updateOutfallBoundaries(const std::vector<CouplingPoint>& cps,
                              const MeshData& mesh,
                              const SurfaceStateData& state,
                              SimulationContext& ctx) {
    auto& nodes = ctx.nodes;

    for (const auto& cp : cps) {
        if (!cp.is_outfall) continue;

        auto ni = static_cast<std::size_t>(cp.node_idx);

        // 2D head at the outfall coupling point
        double h_2d;
        if (cp.vertex_idx >= 0) {
            h_2d = state.vert_head[cp.vertex_idx];
        } else {
            h_2d = state.head[cp.cell_idx];
        }

        // Current outfall head (set by standard outfall logic)
        double h_standard = nodes.head[ni];
        double z_inv = nodes.invert_elev[ni];

        if (cp.has_flap_gate && h_2d > h_standard) {
            // Flap gate closed: don't let 2D raise the outfall boundary
            // Outfall remains at standard boundary condition
            continue;
        }

        // Effective boundary = max(standard, 2D surface head)
        // This ensures 2D flooding raises tailwater but doesn't lower it
        double h_effective = std::max(h_standard, h_2d);
        nodes.depth[ni] = std::max(h_effective - z_inv, 0.0);
        nodes.head[ni]  = z_inv + nodes.depth[ni];
    }
}


void transferOutfallDischarges(const std::vector<CouplingPoint>& cps,
                                const MeshData& mesh,
                                SurfaceStateData& state,
                                const SimulationContext& ctx,
                                double /*dt*/) {
    auto& nodes = ctx.nodes;

    for (const auto& cp : cps) {
        if (!cp.is_outfall) continue;

        auto ni = static_cast<std::size_t>(cp.node_idx);
        int ci = cp.cell_idx;

        // Outfall outflow from 1D solver (computed during routing)
        double Q_outfall = nodes.outflow[ni];

        if (Q_outfall <= 0.0) continue;

        // Inject pipe outflow as source into 2D cell
        double tri_area = mesh.tri_area[ci];
        if (tri_area > 1.0e-30) {
            state.coupling_flux[ci] += Q_outfall / tri_area;  // m/s source
        }
    }
}

} // namespace openswmm::twoD
