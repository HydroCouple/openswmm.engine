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
                              const SolverOptions2D& opts,
                              double dt) {
    auto& nodes = ctx.nodes;
    auto& forcing = ctx.forcing;

    // Clear coupling fluxes
    std::fill(state.coupling_flux.begin(), state.coupling_flux.end(), 0.0);

    // Reset the forcing buffer for every 2D-coupled node and re-arm it with
    // OVERRIDE+PERSIST. This runs at end of step N (after 1D routing) so the
    // Q we write here is consumed by applyForcings at start of step N+1.
    // The default ADD+RESET pattern used elsewhere does not work here:
    // clear_reset_entries() (end of step N) sets mode → NONE for any
    // RESET-persisted entry, which silently drops the Q before it is read
    // by step N+1's applyForcings. OVERRIDE+PERSIST keeps mode armed across
    // the step boundary, and the explicit zero below means multiple
    // coupling points targeting the same node accumulate cleanly via the
    // += in the per-cp loop.
    for (const auto& cp : cps) {
        if (cp.is_outfall) continue;
        auto ni = static_cast<std::size_t>(cp.node_idx);
        forcing.node_lat_inflow_mode[ni]    = ForcingMode::OVERRIDE;
        forcing.node_lat_inflow_value[ni]   = 0.0;
        forcing.node_lat_inflow_persist[ni] = ForcingPersist::PERSIST;
    }

    for (const auto& cp : cps) {
        if (cp.is_outfall) continue;  // Outfalls handled separately

        int ci = cp.cell_idx;
        auto ni = static_cast<std::size_t>(cp.node_idx);

        // 2D head and bed at the coupling point
        double h_2d, z_2d;
        if (cp.vertex_idx >= 0) {
            h_2d = state.vert_head[cp.vertex_idx];
            z_2d = mesh.vz[cp.vertex_idx];
        } else {
            h_2d = state.head[ci];
            z_2d = mesh.tri_cz[ci];
        }

        // 1D node head
        double h_1d = nodes.head[ni];

        // Available water on each side, used for the source-side wet/dry
        // ramp below.
        //
        // For vertex-coupled points we use the MAXIMUM depth across the
        // cells in the vertex stencil. Two failure modes to avoid:
        //   (a) `vert_head - vert_z` is spuriously positive on a fully
        //       dry mesh whenever the vertex sits at a local low spot —
        //       the pseudo-Laplacian reconstruction averages neighbour
        //       cell heads, all of which equal their (higher) centroid z,
        //       so the reconstructed head exceeds the vertex z by the
        //       bed-relief amount alone.
        //   (b) `state.depth[first_tri_containing_v]` is spuriously zero
        //       on a wet bowl whenever the vertex sits below all of its
        //       neighbour cells' centroids: the FV grid has no cell at
        //       the bowl bottom, so each touching cell's depth = max(0,
        //       head - centroid_z) stays at 0 even when surrounding
        //       cells hold water. (This is what kills coupling on
        //       parabolic-bowl-with-central-junction inputs.)
        //
        // Max over the vertex stencil resolves both: truly dry → every
        // stencil cell has depth 0 → ramp 0; any wet stencil cell → the
        // vertex is at-or-below that cell's bed → ramp 1.
        double depth_2d_avail;
        if (cp.vertex_idx >= 0) {
            int v = cp.vertex_idx;
            int start = mesh.vert_stencil_ptr[v];
            int end   = mesh.vert_stencil_ptr[v + 1];
            depth_2d_avail = 0.0;
            for (int k = start; k < end; ++k) {
                depth_2d_avail = std::max(depth_2d_avail,
                    state.depth[mesh.vert_stencil_idx[k]]);
            }
        } else {
            depth_2d_avail = state.depth[ci];
        }
        double depth_1d_avail = nodes.depth[ni];

        // Head difference (positive = 2D → 1D)
        double dh = h_2d - h_1d;

        // C1: surcharge envelope top — coupling activates above this elevation.
        // For an uncapped node, sur_depth = 0 and z_top reduces to the rim
        // elevation, preserving the legacy behaviour. For a capped node
        // (sur_depth > 0), z_top is the elevation at which the physical cap
        // (manhole bolt, sealed inlet) yields and water reaches the surface.
        // The effective-area widening from inlet-grate to manhole-opening
        // is also anchored at z_top so both transitions share one threshold.
        // See docs/1D_2D_COUPLING_GATE_REVIEW.md §6 (C1, C2).
        double z_top = nodes.invert_elev[ni] + nodes.full_depth[ni]
                       + nodes.sur_depth[ni];
        double h_max = std::max(h_1d, h_2d);
        double A_eff = effectiveArea(h_max, z_top, nodes.full_depth[ni],
                                      cp.area, cp.area * 2.0);

        // Orifice exchange flow (full 1D–2D gradient, signed/bidirectional;
        // see review §3 R1c).
        double Q = orificeFlow(dh, cp.cd, A_eff);

        // Smoothly ramp Q to zero as the source side dries up. A hard
        // cutoff at opts.dry_depth would introduce a step-discontinuity in
        // ydot that breaks CVODE's BDF corrector. The Hermite ramp matches
        // the one used by the conductance, so both wet/dry transitions
        // share the same C¹ shape.
        auto wetRamp = [&opts](double d) {
            double t = std::min(1.0, std::max(0.0, d / opts.dry_depth));
            return t * t * (3.0 - 2.0 * t);
        };
        Q *= (Q > 0.0) ? wetRamp(depth_2d_avail) : wetRamp(depth_1d_avail);

        // C2: surcharge gate — exchange may only carry flow when either side
        // is above z_top. The ramp is direction-symmetric (multiplies Q
        // regardless of sign) so R1c bidirectionality is preserved: a 2D
        // cell flooded above a still-pressurised capped pipe drains in
        // through the same gate that a surcharging pipe spills out through.
        // The 5 cm transition matches effectiveArea() above so both
        // discontinuity sources widen together. Reduces to a no-op for
        // uncapped nodes (sur_depth = 0) the moment water reaches the rim.
        {
            double surcharge_excess = h_max - z_top;
            double t = std::min(1.0, std::max(0.0,
                                              surcharge_excess / 0.05));
            double capRamp = t * t * (3.0 - 2.0 * t);
            Q *= capRamp;
        }

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

        // Inject as lateral inflow into SWMM node. Accumulate into the
        // forcing buffer that was pre-armed above with OVERRIDE+PERSIST so
        // applyForcings at step N+1 sets user_lat_flow = Σ Q over all
        // coupling points targeting this node.
        // Positive Q = flow from 2D → 1D (positive lateral inflow).
        forcing.node_lat_inflow_value[ni] += Q;

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

    // C4 refactor: this routine no longer writes nodes.head/depth directly.
    // Doing so was a no-op under the DW solver because setAllOutfallDepths
    // re-runs inside every Picard iteration and would overwrite the value.
    // Instead, cache h_2d at the coupling cell into nodes.outfall_2d_head[],
    // and let Outfall::setAllOutfallDepths apply the max(h_standard, h_2d)
    // override on every iteration. See docs/1D_2D_COUPLING_GATE_REVIEW.md §6.
    //
    // The flap-gate decision is also moved into setAllOutfallDepths so it
    // can see the just-computed h_standard for that iteration. To express
    // "flap closed" without coupling the modules, we cache the raw h_2d
    // here; setAllOutfallDepths checks the node's flap-gate flag and the
    // current h_standard to decide whether to apply the override.
    //
    // Dry-mesh guard: when the cell at the coupling point is essentially
    // dry, leave the sentinel value (-1e30) in place so setAllOutfallDepths
    // does not fire the override. The naive check `h_2d > z_inv` in
    // setAllOutfallDepths is true whenever bed_z > z_inv (the common
    // physical case — outfall pipe enters underground beneath the surface
    // mesh), because h_2d = bed_z + depth = bed_z on dry cells. Gating on
    // actual surface depth here keeps the cached value semantically
    // meaningful: "the 2D water surface elevation at this outfall, if any
    // water is present, else absent."
    constexpr double DRY_DEPTH_THRESHOLD = 1.0e-4;  // 0.1 mm
    for (const auto& cp : cps) {
        if (!cp.is_outfall) continue;

        auto ni = static_cast<std::size_t>(cp.node_idx);

        // 2D head and bed elevation at the outfall coupling point
        double h_2d, bed_z;
        if (cp.vertex_idx >= 0) {
            h_2d  = state.vert_head[cp.vertex_idx];
            bed_z = mesh.vz[cp.vertex_idx];
        } else {
            h_2d  = state.head[cp.cell_idx];
            bed_z = mesh.tri_cz[cp.cell_idx];
        }

        double depth_2d = h_2d - bed_z;
        if (depth_2d > DRY_DEPTH_THRESHOLD) {
            nodes.outfall_2d_head[ni] = h_2d;
        } else {
            nodes.outfall_2d_head[ni] = -1.0e30;  // dry — no override
        }
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
