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

/// Distribute a signed volumetric exchange Q (m³/s) onto the 2D cell(s) of a
/// coupling point as a coupling_flux rate (m/s). Sign matches coupling_flux:
/// positive = source INTO the 2D cell, negative = sink OUT of it.
///
/// Triangle-coupled points (vertex_idx < 0) inject into the single cell. Vertex-
/// coupled points spread Q across the vertex stencil weighted by the UPWIND HGL
/// slope from the vertex to each neighbour cell centroid:
///   source (Q>0): downhill cells, w_k = max(0,  (vert_head − head_k)/d_k)
///   sink   (Q<0): uphill   cells, w_k = max(0, −(vert_head − head_k)/d_k)
/// Weights are normalised (Σ = 1) so the injected volume is exactly Q·dt
/// (conservative; totalExchangeFlow() unchanged). On a flat surface (Σw ≈ 0) —
/// or when no cell lies in the upwind direction — fall back to the geometric
/// partition-of-unity weights the head reconstruction uses (vert_stencil_wt).
inline void scatterCouplingFlux(const MeshData& mesh, SurfaceStateData& state,
                                const CouplingPoint& cp, double Q) noexcept {
    // Triangle coupling: single cell, head and flux already co-located.
    if (cp.vertex_idx < 0) {
        int ci = cp.cell_idx;
        double area = mesh.tri_area[ci];
        if (area > 1.0e-30) state.coupling_flux[ci] += Q / area;
        return;
    }

    int v = cp.vertex_idx;
    int start = mesh.vert_stencil_ptr[v];
    int end   = mesh.vert_stencil_ptr[v + 1];
    double hv = state.vert_head[v];
    double vx = mesh.vx[v];
    double vy = mesh.vy[v];
    double sign = (Q >= 0.0) ? 1.0 : -1.0;  // source → downhill, sink → uphill

    // Pass 1: sum the upwind-slope weights over the stencil.
    double wsum = 0.0;
    for (int k = start; k < end; ++k) {
        int kc = mesh.vert_stencil_idx[k];
        double dx = mesh.tri_cx[kc] - vx;
        double dy = mesh.tri_cy[kc] - vy;
        double d = std::sqrt(dx * dx + dy * dy);
        if (d < 1.0e-9) continue;
        double slope = sign * (hv - state.head[kc]) / d;
        if (slope > 0.0) wsum += slope;
    }

    // Pass 2: scatter Q. Upwind weighting when a usable gradient exists,
    // otherwise the geometric partition-of-unity fallback (flat surface).
    if (wsum > 1.0e-30) {
        for (int k = start; k < end; ++k) {
            int kc = mesh.vert_stencil_idx[k];
            double dx = mesh.tri_cx[kc] - vx;
            double dy = mesh.tri_cy[kc] - vy;
            double d = std::sqrt(dx * dx + dy * dy);
            if (d < 1.0e-9) continue;
            double slope = sign * (hv - state.head[kc]) / d;
            if (slope <= 0.0) continue;
            double area = mesh.tri_area[kc];
            if (area > 1.0e-30)
                state.coupling_flux[kc] += (Q * (slope / wsum)) / area;
        }
    } else {
        for (int k = start; k < end; ++k) {
            int kc = mesh.vert_stencil_idx[k];
            double area = mesh.tri_area[kc];
            if (area > 1.0e-30)
                state.coupling_flux[kc] += (Q * mesh.vert_stencil_wt[k]) / area;
        }
    }
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

    // Clear coupling fluxes on the 2D side
    std::fill(state.coupling_flux.begin(), state.coupling_flux.end(), 0.0);

    // Clear the dedicated 1D-side coupling source array for every coupled
    // junction. The signed Q accumulated below is consumed by
    // assembleLateralInflows at the start of step N+1, where it is split
    // into routing_external (positive part) and routing_flooding (negative
    // part) for mass-balance accounting. See review §11.
    //
    // This replaces the earlier hack of routing coupling Q through
    // forcing.node_lat_inflow_value with OVERRIDE+PERSIST, which (a)
    // conflated 2D coupling with user-API forcing and (b) silently dropped
    // the negative (1D→2D) side from the routing continuity equation
    // because the mass-balance accumulator at SWMMEngine.cpp:2152 only
    // sums positive user_lat_flow values.
    for (const auto& cp : cps) {
        if (cp.is_outfall) continue;
        auto ni = static_cast<std::size_t>(cp.node_idx);
        nodes.coupling_inflow[ni] = 0.0;
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

        // 1D node head. The 1D engine always stores heads in feet (US internal
        // units, every project); convert to the 2D solver's SI internal length
        // so dh below is in metres (opts.len_1d_to_2d == 0.3048 always).
        double h_1d = nodes.head[ni] * opts.len_1d_to_2d;

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
        double depth_1d_avail = nodes.depth[ni] * opts.len_1d_to_2d;

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
        double z_top = (nodes.invert_elev[ni] + nodes.full_depth[ni]
                       + nodes.sur_depth[ni]) * opts.len_1d_to_2d;
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
            // Q > 0 means flow from 2D into 1D node (drainage). Node volumes
            // are always ft³ (1D US internal units); convert to m³ so the Q_max
            // cap is in the 2D solver's SI flow units (matching the SI Q above).
            double available = (nodes.full_volume[ni] - nodes.volume[ni])
                               * opts.vol_1d_to_2d;
            if (available > 0.0 && dt > 0.0) {
                double Q_max = available / dt;
                Q = std::min(Q, Q_max);
            } else if (available <= 0.0) {
                // Node full — only allow if 1D head < 2D head (surcharge drain-back)
                if (h_1d >= h_2d) Q = 0.0;
            }

            // Cap drainage at the water actually available in the receiving 2D
            // cell (ci = cp.cell_idx, where the sink is applied below). Without
            // this, the constant per-step sink can pull the cell's H below its
            // bed — the H-formulation floors depth at max(H−z,0), so less water
            // leaves the 2D domain than the 1D node was credited (Q·dt), which
            // shows up as a 2D continuity error. Uses the start-of-step depth,
            // mirroring the 1D node-capacity clamp above. Same clamped Q feeds
            // both sides of the exchange, so the boundary stays conservative.
            if (dt > 0.0) {
                // Sum the wet volume across the cells the sink actually draws
                // from — the whole stencil for vertex coupling, one cell for
                // triangle coupling — matching the distributed sink applied by
                // scatterCouplingFlux below so the cap stays conservative.
                double avail_2d;
                if (cp.vertex_idx >= 0) {
                    int vv = cp.vertex_idx;
                    int s = mesh.vert_stencil_ptr[vv];
                    int e = mesh.vert_stencil_ptr[vv + 1];
                    avail_2d = 0.0;
                    for (int k = s; k < e; ++k) {
                        int kc = mesh.vert_stencil_idx[k];
                        avail_2d += state.depth[kc] * mesh.tri_area[kc];  // m³
                    }
                } else {
                    avail_2d = state.depth[ci] * mesh.tri_area[ci];  // m³
                }
                double Q_max_2d = avail_2d / dt;
                Q = std::min(Q, Q_max_2d);
            }
        }

        // Inject as a dedicated 2D-coupling source on the SWMM node.
        // Multiple coupling points targeting the same node accumulate via
        // the += below. Positive Q = 2D → 1D (drain into pipe);
        // negative Q = 1D → 2D (surcharge spill out of pipe). The signed
        // value is consumed by assembleLateralInflows at the start of the
        // next routing step, which both feeds lat_flow for the DW solver
        // and folds the sign-split volumes into routing_external (positive
        // side) and routing_flooding (negative side, |Q|).
        //
        // Q is in the 2D solver's SI flow units (m³/s); the 1D engine's
        // lateral-inflow sum and continuity accumulators are always in ft³/s
        // (1D US internal units), so convert back here (flow_2d_to_1d ≈ 35.31).
        nodes.coupling_inflow[ni] += Q * opts.flow_2d_to_1d;

        // Record coupling flux back to the 2D cell(s). For vertex coupling the
        // sink/source is distributed across the stencil weighted by the upwind
        // HGL slope (see scatterCouplingFlux); negative Q = drainage out of 2D.
        scatterCouplingFlux(mesh, state, cp, -Q);
    }
}


void updateOutfallBoundaries(const std::vector<CouplingPoint>& cps,
                              const MeshData& mesh,
                              const SurfaceStateData& state,
                              SimulationContext& ctx,
                              const SolverOptions2D& opts) {
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
            // h_2d is SI (metres). The 1D consumer (Outfall::setAllOutfallDepths)
            // always compares against h_standard in feet (1D US internal units),
            // so convert back here (opts.len_2d_to_1d ≈ 3.281 always).
            nodes.outfall_2d_head[ni] = h_2d * opts.len_2d_to_1d;
        } else {
            nodes.outfall_2d_head[ni] = -1.0e30;  // dry — no override
        }
    }
}


void transferOutfallDischarges(const std::vector<CouplingPoint>& cps,
                                const MeshData& mesh,
                                SurfaceStateData& state,
                                const SimulationContext& ctx,
                                const SolverOptions2D& opts,
                                double /*dt*/) {
    auto& nodes = ctx.nodes;

    for (const auto& cp : cps) {
        if (!cp.is_outfall) continue;

        auto ni = static_cast<std::size_t>(cp.node_idx);

        // Net signed 1D→2D exchange at the outfall, in 1D US-internal flow units
        // (ft³/s). nodes.inflow = pipe discharge OUT of the 1D network (onto the
        // 2D surface); nodes.outflow = backflow INTO the network (surface water
        // drawn back through the submerged outfall, driven by the 2D tailwater
        // that updateOutfallBoundaries / setAllOutfallDepths already prescribed
        // as the outfall head BC). So the 2D→1D direction is handled naturally
        // by the head boundary; here we apply the RESULTING 1D flow back to the
        // 2D domain. Convert to the 2D solver's SI flow units (≈ 0.0283).
        double Q_net = (nodes.inflow[ni] - nodes.outflow[ni]) * opts.flow_1d_to_2d;

        // Positive → pipe discharging onto the surface → 2D source.
        // Negative → surface water drawn back into the pipe → 2D sink.
        // Distributed across the stencil (vertex) or single cell (triangle),
        // mirroring the junction path so head-from-many and flux-into-many stay
        // consistent.
        scatterCouplingFlux(mesh, state, cp, Q_net);
    }
}

} // namespace openswmm::twoD
