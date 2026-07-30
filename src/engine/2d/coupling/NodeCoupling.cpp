/**
 * @file NodeCoupling.cpp
 * @brief Implementation of 2D↔1D node coupling via orifice exchange.
 *
 * @see NodeCoupling.hpp
 * @ingroup engine_2d
 */

#include "NodeCoupling.hpp"
#include "../mesh/VertexReconstruction.hpp"  // vertexHeadAt (on-demand single-vertex head)
#include "../../core/SimulationContext.hpp"

#include <cmath>
#include <algorithm>

namespace openswmm::twoD {

namespace {

constexpr double GRAVITY = 9.80665;  // m/s²

/// Regularization head (m) below which the orifice √-law is replaced by a C¹
/// quadratic. The bare law Q = Cd·A·sign(Δh)·√(2g|Δh|) has dQ/dΔh → ∞ as
/// Δh → 0, which is exactly the weir-equilibrium regime where the 1D and 2D
/// heads hover near-equal — a strong stiffness/oscillation source for the
/// (explicit, step-frozen) exchange. Below H_EPS we use φ(x) that matches √x in
/// value and slope at H_EPS and has a FINITE slope at 0.
constexpr double ORIFICE_H_EPS = 0.02;  // 2 cm

/// √-with-regularized-tail: φ(x)=√x for x≥ε; a C¹ quadratic for x<ε.
inline double orificePhi(double a) noexcept {
    if (a >= ORIFICE_H_EPS) return std::sqrt(a);
    const double inv_sqrt_e = 1.0 / std::sqrt(ORIFICE_H_EPS);
    // φ(x) = (3/(2√ε))x − (1/(2 ε^{3/2}))x² : φ(ε)=√ε, φ'(ε)=1/(2√ε), φ'(0)=3/(2√ε).
    return (1.5 * inv_sqrt_e) * a - (0.5 * inv_sqrt_e / ORIFICE_H_EPS) * a * a;
}

/// Orifice exchange: Q = Cd · A · sign(Δh) · √(2g) · φ(|Δh|), with φ the
/// C¹-regularized square root (bounded sensitivity at Δh → 0).
inline double orificeFlow(double dh, double cd, double area) noexcept {
    const double a = std::abs(dh);
    if (a < 1.0e-12) return 0.0;
    const double sign = (dh > 0.0) ? 1.0 : -1.0;
    return sign * cd * area * std::sqrt(2.0 * GRAVITY) * orificePhi(a);
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
    double hv = vertexHeadAt(mesh, state, v);
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

/// Wet-masked 2D free surface at a coupling vertex: depth-weighted average of
/// the incident WET cells' η (depth ≥ dry_depth), falling back to the vertex
/// ground elevation when every incident cell is dry. Used for the coupling
/// head under CELL_CLOSURE = VFR instead of the pseudo-Laplacian vert_head,
/// which blends DRY-cell heads (= bed elevations) into shoreline vertices and
/// so reads ≈ terrain over a pond — the failure modes documented at the
/// stencil-depth scan below and (for outfalls) in updateOutfallBoundaries.
/// Same weighting rule as reconstructVertexRenderDepths, evaluated on demand
/// for the handful of coupling vertices inside the RHS (O(stencil) each).
inline double wetVertexEta(const MeshData& mesh, const SurfaceStateData& state,
                           int v, double dry_depth) noexcept {
    const int s = mesh.vert_stencil_ptr[v];
    const int e = mesh.vert_stencil_ptr[v + 1];
    double num = 0.0, den = 0.0;
    for (int k = s; k < e; ++k) {
        const int t = mesh.vert_stencil_idx[k];
        const double h = state.depth[t];
        if (!(h >= dry_depth)) continue;            // dry skip (NaN-robust)
        num += h * state.head[t];
        den += h;
    }
    return (den > 0.0) ? num / den : mesh.vz[v];
}

/// Coupling head h_2d at a coupling point: wet-masked vertex η under the VFR
/// closure, the legacy pseudo-Laplacian vert_head under FLAT (bit-identical
/// default), the cell head for triangle-coupled points.
inline double couplingHead2D(const CouplingPoint& cp, const MeshData& mesh,
                             const SurfaceStateData& state,
                             const SolverOptions2D& opts) noexcept {
    if (cp.vertex_idx < 0) return state.head[cp.cell_idx];
    if (opts.cell_closure == CellClosure2D::VFR)
        return wetVertexEta(mesh, state, cp.vertex_idx, opts.dry_depth);
    return vertexHeadAt(mesh, state, cp.vertex_idx);
}

} // anonymous namespace


double computeNodeCouplingQ(const CouplingPoint& cp,
                            const MeshData& mesh,
                            const SurfaceStateData& state,
                            const NodeData& nodes,
                            const SolverOptions2D& opts,
                            const double* provisional_vol_m3,
                            double h1d_offset_m) noexcept {
    const auto ni = static_cast<std::size_t>(cp.node_idx);
    const int  ci = cp.cell_idx;

    // Provisional-state helpers (per-step decoupled path only; nullptr on the
    // live-RHS path, where CVODE's own state is already current). When given,
    // provisional_vol_m3[i] is the volume cell i still holds after the
    // exchange already committed this window, so depth/head are re-derived
    // from it under the FLAT closure (h = V/A, η = z_c + h). The VFR inverse
    // is deliberately not applied here: this is a self-limiting correction on
    // an explicit sub-step, and the flat form is monotone in the same
    // direction — the exact closure still governs the solver's own state.
    const auto provDepth = [&](int i) noexcept {
        const double a = mesh.tri_area[i];
        const double v = std::max(0.0, provisional_vol_m3[i]);
        return (a > 1.0e-30) ? v / a : 0.0;
    };

    // Live 2D head at the coupling point (reconstructed this RHS call).
    // VFR closure: wet-masked vertex η (dry terrain can't fake a surface);
    // FLAT: the legacy pseudo-Laplacian vert_head, bit-identical.
    double h_2d;
    if (provisional_vol_m3 == nullptr) {
        h_2d = couplingHead2D(cp, mesh, state, opts);
    } else if (cp.vertex_idx < 0) {
        h_2d = mesh.tri_cz[ci] + provDepth(ci);
    } else {
        // Wet-depth-weighted η over the stencil, on provisional depths —
        // mirrors wetVertexEta so a drying stencil lowers the driving head
        // instead of holding the window-start surface.
        const int v = cp.vertex_idx;
        const int s = mesh.vert_stencil_ptr[v];
        const int e = mesh.vert_stencil_ptr[v + 1];
        double num = 0.0, den = 0.0;
        for (int k = s; k < e; ++k) {
            const int t = mesh.vert_stencil_idx[k];
            const double d = provDepth(t);
            if (!(d >= opts.dry_depth)) continue;
            num += d * (mesh.tri_cz[t] + d);
            den += d;
        }
        h_2d = (den > 0.0) ? num / den : mesh.vz[v];
    }
    // 1D node head — frozen across the 2D advance window. h1d_offset_m is the
    // experimental head-ramp extrapolation (OPENSWMM_2D_HEAD_RAMP): the batch
    // trend slope × time-in-batch, in the 2D metre frame.
    const double h_1d = nodes.head[ni] * opts.len_1d_to_2d + h1d_offset_m;

    // Live max-over-stencil source depth: the wet/dry self-limiter below uses it
    // so the drain ramps to zero as the cell dries (no overshoot / negativity).
    double depth_2d_avail;
    if (cp.vertex_idx >= 0) {
        const int v = cp.vertex_idx;
        const int s = mesh.vert_stencil_ptr[v];
        const int e = mesh.vert_stencil_ptr[v + 1];
        depth_2d_avail = 0.0;
        for (int k = s; k < e; ++k) {
            const int t = mesh.vert_stencil_idx[k];
            depth_2d_avail = std::max(depth_2d_avail,
                (provisional_vol_m3 == nullptr) ? state.depth[t] : provDepth(t));
        }
    } else {
        depth_2d_avail = (provisional_vol_m3 == nullptr) ? state.depth[ci]
                                                         : provDepth(ci);
    }
    const double depth_1d_avail = nodes.depth[ni] * opts.len_1d_to_2d;

    // Capped-pipe gate at the crown (= surcharge / slot-engagement threshold).
    const double crown = (nodes.invert_elev[ni] + nodes.full_depth[ni])
                         * opts.len_1d_to_2d;
    const double z_top = crown;
    const double h_max = std::max(h_1d, h_2d);
    const double A_eff = effectiveArea(h_max, crown, nodes.full_depth[ni],
                                       cp.area, cp.area * 2.0);

    double Q = orificeFlow(h_2d - h_1d, cp.cd, A_eff);

    // C¹ Hermite gate over a 5 cm band above the crown.
    constexpr double CAP_BAND = 0.05;
    const double ct = std::min(1.0, std::max(0.0, (h_max - z_top) / CAP_BAND));
    Q *= ct * ct * (3.0 - 2.0 * ct);

    // Source-side wet/dry self-limit on the LIVE depth — this is what makes the
    // continuous coupling stable inside the implicit solve (Q → 0 as the source
    // empties), replacing the held-flux avail/dt cap of the retired computeCouplingExchange.
    auto wetRamp = [&opts](double d) {
        const double t = std::min(1.0, std::max(0.0, d / opts.dry_depth));
        return t * t * (3.0 - 2.0 * t);
    };
    Q *= (Q > 0.0) ? wetRamp(depth_2d_avail) : wetRamp(depth_1d_avail);
    return Q;
}


double computeNodeCouplingDQdh1d(const CouplingPoint& cp,
                                 const MeshData& mesh,
                                 const SurfaceStateData& state,
                                 const NodeData& nodes,
                                 const SolverOptions2D& opts) noexcept {
    // Mirror of computeNodeCouplingQ with Q = Cd·A_eff·sign(Δh)·√(2g)·φ(|Δh|),
    // Δh = h_2d − h_1d: the head sensitivity is G = −∂Q/∂h_1d =
    // Cd·A_eff·√(2g)·φ′(|Δh|) · (gate) · (ramp) ≥ 0. Gate/ramp factors are
    // treated as constants (each ∈ [0,1]); their derivatives are deliberately
    // dropped so the sign guarantee — a pure damping term on the 1D node
    // continuity denominator — can never be violated.
    const auto ni = static_cast<std::size_t>(cp.node_idx);
    const double h_2d = couplingHead2D(cp, mesh, state, opts);
    const double h_1d = nodes.head[ni] * opts.len_1d_to_2d;
    const double a = std::abs(h_2d - h_1d);

    const double crown =
        (nodes.invert_elev[ni] + nodes.full_depth[ni]) * opts.len_1d_to_2d;
    const double h_max = std::max(h_1d, h_2d);
    const double A_eff = effectiveArea(h_max, crown, nodes.full_depth[ni],
                                       cp.area, cp.area * 2.0);

    // φ′ of the C¹-regularized root: 1/(2√a) beyond ε, linear-finite below.
    double phi_p;
    if (a >= ORIFICE_H_EPS) {
        phi_p = 0.5 / std::sqrt(a);
    } else {
        const double inv_sqrt_e = 1.0 / std::sqrt(ORIFICE_H_EPS);
        phi_p = 1.5 * inv_sqrt_e - (inv_sqrt_e / ORIFICE_H_EPS) * a;
    }
    double G = cp.cd * A_eff * std::sqrt(2.0 * GRAVITY) * phi_p;

    constexpr double CAP_BAND = 0.05;
    const double ct = std::min(1.0, std::max(0.0, (h_max - crown) / CAP_BAND));
    G *= ct * ct * (3.0 - 2.0 * ct);

    auto wetRamp = [&opts](double d) {
        const double t = std::min(1.0, std::max(0.0, d / opts.dry_depth));
        return t * t * (3.0 - 2.0 * t);
    };
    const double depth_1d = nodes.depth[ni] * opts.len_1d_to_2d;
    const double depth_2d =
        (cp.cell_idx >= 0) ? state.depth[cp.cell_idx] : 0.0;
    G *= wetRamp(std::max(depth_1d, depth_2d));
    return (G > 0.0) ? G : 0.0;
}


void scatterCouplingToYdot(const MeshData& mesh, const SurfaceStateData& state,
                           const CouplingPoint& cp, double Q,
                           double* ydot) noexcept {
    if (cp.vertex_idx < 0) { ydot[cp.cell_idx] += Q; return; }

    const int v = cp.vertex_idx;
    const int start = mesh.vert_stencil_ptr[v];
    const int end   = mesh.vert_stencil_ptr[v + 1];
    const double hv = vertexHeadAt(mesh, state, v);
    const double vx = mesh.vx[v];
    const double vy = mesh.vy[v];
    const double sign = (Q >= 0.0) ? 1.0 : -1.0;  // source → downhill, sink → uphill

    double wsum = 0.0;
    for (int k = start; k < end; ++k) {
        const int kc = mesh.vert_stencil_idx[k];
        const double dx = mesh.tri_cx[kc] - vx;
        const double dy = mesh.tri_cy[kc] - vy;
        const double d  = std::sqrt(dx * dx + dy * dy);
        if (d < 1.0e-9) continue;
        const double slope = sign * (hv - state.head[kc]) / d;
        if (slope > 0.0) wsum += slope;
    }

    if (wsum > 1.0e-30) {
        for (int k = start; k < end; ++k) {
            const int kc = mesh.vert_stencil_idx[k];
            const double dx = mesh.tri_cx[kc] - vx;
            const double dy = mesh.tri_cy[kc] - vy;
            const double d  = std::sqrt(dx * dx + dy * dy);
            if (d < 1.0e-9) continue;
            const double slope = sign * (hv - state.head[kc]) / d;
            if (slope <= 0.0) continue;
            ydot[kc] += Q * (slope / wsum);
        }
    } else {
        for (int k = start; k < end; ++k) {
            const int kc = mesh.vert_stencil_idx[k];
            ydot[kc] += Q * mesh.vert_stencil_wt[k];
        }
    }
}


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
        cp.area_authored =
            v < static_cast<int>(mesh.vert_coupling_area_set.size()) &&
            mesh.vert_coupling_area_set[static_cast<std::size_t>(v)] != 0;

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
        const int ofr = ctx.node_subtypes.outfall_row(node_idx);
        cp.has_flap_gate = cp.is_outfall && ofr >= 0 &&
                           ctx.node_subtypes.outfalls.has_flap_gate[static_cast<std::size_t>(ofr)];

        cps.push_back(cp);
    }

    // Triangle-to-node couplings — one CouplingPoint per authored row.
    // mesh.tri_couplings is the source of truth (repeated-row form: several
    // nodes may couple to the same triangle); legacy-array-only paths were
    // synthesised into rows during resolve (SurfaceRouter2D).
    for (const auto& row : mesh.tri_couplings) {
        const int node_idx = row.node;
        if (node_idx < 0) continue;
        if (row.tri < 0 || row.tri >= mesh.n_triangles()) continue;

        CouplingPoint cp;
        cp.cell_idx = row.tri;
        cp.vertex_idx = -1;  // centroid coupling
        cp.node_idx = node_idx;
        cp.cd = row.cd;
        cp.area = row.area;
        cp.area_authored = row.area_set;

        auto ui = static_cast<std::size_t>(node_idx);
        cp.is_outfall = (ctx.nodes.type[ui] == NodeType::OUTFALL);
        const int ofr = ctx.node_subtypes.outfall_row(node_idx);
        cp.has_flap_gate = cp.is_outfall && ofr >= 0 &&
                           ctx.node_subtypes.outfalls.has_flap_gate[static_cast<std::size_t>(ofr)];

        cps.push_back(cp);
    }

    return cps;
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
    // Wet/dry gating: the tailwater override must engage only when the 2D
    // cell is genuinely wet, otherwise the outfall reverts to its legacy
    // free-discharge condition. The naive check `h_2d > z_inv` in
    // setAllOutfallDepths is true whenever bed_z > z_inv (the common
    // physical case — outfall pipe enters underground beneath the surface
    // mesh), because h_2d = bed_z + depth ≈ bed_z on near-dry cells. A
    // residual film thinner than the 2D solver's own dry_depth (which the
    // surface solver itself treats as immovable) would otherwise pin the
    // outfall stage at bed_z and deadlock the pipe.
    //
    // So we cache two things: the raw 2D surface head h_2d (always), and a
    // Hermite wet/dry ramp factor in [0,1]. setAllOutfallDepths blends the
    // prescribed stage from the free condition (ramp=0, dry) up to the full
    // tailwater (ramp=1, wet), keeping the transition C¹ so the outfall cell
    // does not chatter as its own discharge re-wets it.
    //
    // The ramp is keyed on the surface depth IN EXCESS of opts.dry_depth, the
    // same threshold the 2D solver uses to call a cell dry (and below which it
    // freezes the water as immovable). A draining cell therefore comes to rest
    // at a film at/just below dry_depth, so the ramp must read ZERO there —
    // hence (depth_2d - dry_depth)/dry_depth, full tailwater only once the cell
    // holds more than ~2·dry_depth of real water. A ramp keyed on depth_2d
    // alone would read ≈1 at that resting film and reinstate the deadlock.
    auto wetRamp = [&opts](double d) {
        double t = std::min(1.0, std::max(0.0,
                                (d - opts.dry_depth) / opts.dry_depth));
        return t * t * (3.0 - 2.0 * t);  // smoothstep, C¹ continuous
    };
    for (const auto& cp : cps) {
        if (!cp.is_outfall) continue;

        // 2D stage and wetness at the outfall coupling point.
        //
        // For vertex-coupled outfalls, do NOT derive these from the
        // pseudo-Laplacian vert_head: it averages neighbour CELL heads and
        // ignores the vertex's own z, so a vertex carved below its neighbours
        // (outfall vertices are commonly written at the pipe INVERT, metres
        // below the surrounding terrain) reads vert_head ≈ terrain on a bone-
        // dry mesh → phantom depth equal to the bed relief → every outfall
        // pinned at a terrain-level tailwater from the first step (choked /
        // reversed discharge, 1D routing-step collapse). This is failure mode
        // (a) documented for the junction path above; mirror its guard here:
        // wetness AND stage come from the actual water in the vertex stencil.
        // The deepest stencil cell is where outfall water genuinely pools, and
        // its head is a real water surface — dry cells (head = own bed) can
        // never fake a tailwater.
        double h_2d, depth_2d;
        if (cp.vertex_idx >= 0) {
            const int v = cp.vertex_idx;
            const int s = mesh.vert_stencil_ptr[v];
            const int e = mesh.vert_stencil_ptr[v + 1];
            int deepest = -1;
            depth_2d = 0.0;
            for (int k = s; k < e; ++k) {
                const int c = mesh.vert_stencil_idx[k];
                if (state.depth[c] > depth_2d) { depth_2d = state.depth[c]; deepest = c; }
            }
            // Fully dry stencil: ramp below reads 0, so the cached stage is
            // never applied; the vertex bed keeps the value physically sane.
            h_2d = (deepest >= 0) ? state.head[deepest] : mesh.vz[v];
        } else {
            h_2d     = state.head[cp.cell_idx];
            depth_2d = state.depth[cp.cell_idx];  // == head - tri_cz by reconstruction
        }
        // h_2d is SI (metres). The 1D consumer (Outfall::setAllOutfallDepths)
        // always compares against h_standard in feet (1D US internal units),
        // so convert back here (opts.len_2d_to_1d ≈ 3.281 always). Cache into the
        // outfall side-table (Phase 4 Stage B). The wet/dry decision now lives in
        // ramp_2d, so the head is cached unconditionally.
        const int orow = ctx.node_subtypes.outfall_row(cp.node_idx);
        if (orow >= 0) {
            auto ur = static_cast<std::size_t>(orow);
            ctx.node_subtypes.outfalls.head_2d[ur] = h_2d * opts.len_2d_to_1d;
            ctx.node_subtypes.outfalls.ramp_2d[ur] = wetRamp(depth_2d);
        }
    }
}




namespace {

/// Remaining withdrawal budget (m³) visible to a coupling point: the whole
/// vertex stencil for vertex coupling, the single cell for centroid coupling.
inline double budgetAvail(const MeshData& mesh, const CouplingPoint& cp,
                          const std::vector<double>& cell_budget_m3) noexcept {
    if (cp.vertex_idx < 0)
        return std::max(0.0, cell_budget_m3[static_cast<std::size_t>(cp.cell_idx)]);
    double avail = 0.0;
    const int v = cp.vertex_idx;
    for (int k = mesh.vert_stencil_ptr[v]; k < mesh.vert_stencil_ptr[v + 1]; ++k)
        avail += std::max(0.0,
            cell_budget_m3[static_cast<std::size_t>(mesh.vert_stencil_idx[k])]);
    return avail;
}

/// Draw `vol` (m³, ≥ 0) down from the budget cells a coupling point taps.
/// Cells are floored at 0 with the undrawn share spilling to the remaining
/// cells (two greedy passes): budgetAvail sums max(0, ·), so an unclamped
/// weighted subtraction that pushes one cell negative while another stays
/// positive would let repeated draws exceed the true pool — the budget must
/// consume exactly `vol` from the non-negative mass or it is not a cap.
inline void budgetDraw(const MeshData& mesh, const CouplingPoint& cp,
                       double vol, std::vector<double>& cell_budget_m3) noexcept {
    if (vol <= 0.0) return;
    if (cp.vertex_idx < 0) {
        auto& b = cell_budget_m3[static_cast<std::size_t>(cp.cell_idx)];
        b = std::max(0.0, b - vol);
        return;
    }
    const int v = cp.vertex_idx;
    const int s = mesh.vert_stencil_ptr[v];
    const int e = mesh.vert_stencil_ptr[v + 1];
    // Pass 1: weighted draw, clamped per cell; collect the undrawn remainder.
    double remainder = 0.0;
    for (int k = s; k < e; ++k) {
        auto& b = cell_budget_m3[static_cast<std::size_t>(mesh.vert_stencil_idx[k])];
        const double want = vol * mesh.vert_stencil_wt[k];
        const double take = std::min(want, std::max(0.0, b));
        b -= take;
        remainder += want - take;
    }
    // Pass 2: greedy spill of the remainder into cells with budget left.
    for (int k = s; k < e && remainder > 0.0; ++k) {
        auto& b = cell_budget_m3[static_cast<std::size_t>(mesh.vert_stencil_idx[k])];
        const double take = std::min(remainder, std::max(0.0, b));
        b -= take;
        remainder -= take;
    }
    // Any residual means the caller drew more than budgetAvail reported —
    // impossible when the per-step cap uses budgetAvail, so drop it.
}

/// Credit `vol` (m³, ≥ 0) of water ADDED to the cells a coupling point feeds
/// (1D→2D spill, outfall discharge). Keeps the budget a faithful provisional
/// volume so the next sub-step's driving head sees the water this window has
/// already committed to putting on the surface, not just what it took off.
inline void budgetCredit(const MeshData& mesh, const CouplingPoint& cp,
                         double vol, std::vector<double>& cell_budget_m3) noexcept {
    if (vol <= 0.0) return;
    if (cp.vertex_idx < 0) {
        cell_budget_m3[static_cast<std::size_t>(cp.cell_idx)] += vol;
        return;
    }
    const int v = cp.vertex_idx;
    for (int k = mesh.vert_stencil_ptr[v]; k < mesh.vert_stencil_ptr[v + 1]; ++k)
        cell_budget_m3[static_cast<std::size_t>(mesh.vert_stencil_idx[k])] +=
            vol * mesh.vert_stencil_wt[k];
}

} // anonymous namespace


int accumulateOutfallDischargeStep(const std::vector<CouplingPoint>& cps,
                                   const MeshData& mesh,
                                   const SurfaceStateData& state,
                                   const SimulationContext& ctx,
                                   const SolverOptions2D& opts,
                                   double dt,
                                   std::vector<double>& accum_m3,
                                   std::vector<double>& cell_budget_m3,
                                   double* sample_row) {
    (void)state;
    if (dt <= 0.0) return 0;
    const auto& nodes = ctx.nodes;
    int clamped = 0;

    for (std::size_t k = 0; k < cps.size(); ++k) {
        const auto& cp = cps[k];
        if (!cp.is_outfall) continue;

        const auto ni = static_cast<std::size_t>(cp.node_idx);

        // LIVE net signed 1D→2D exchange this routing step (see
        // the retired transferOutfallDischarges for the sign/BC discussion).
        double Q_net = (nodes.inflow[ni] - nodes.outflow[ni]) * opts.flow_1d_to_2d;

        // Withdrawal capped by the shared frozen-2D budget (window-cumulative).
        if (Q_net < 0.0) {
            const double Q_min = -budgetAvail(mesh, cp, cell_budget_m3) / dt;
            if (Q_net < Q_min) { Q_net = Q_min; ++clamped; }
        }
        if (Q_net == 0.0) continue;

        const double vol_m3 = Q_net * dt;             // m³ this routing step
        if (Q_net < 0.0) budgetDraw(mesh, cp, -vol_m3, cell_budget_m3);
        else             budgetCredit(mesh, cp, vol_m3, cell_budget_m3);

        accum_m3[k] += vol_m3;

        // Series sample: Q_net is already net-2D-source-positive.
        if (sample_row) sample_row[k] = Q_net;
    }
    return clamped;
}


void injectAccumulatedExchange(const std::vector<CouplingPoint>& cps,
                               const MeshData& mesh,
                               SurfaceStateData& state,
                               const std::vector<double>& accum_m3,
                               double window_dt,
                               double sign) {
    if (window_dt <= 0.0) return;
    for (std::size_t k = 0; k < cps.size(); ++k) {
        const double vol = accum_m3[k];
        if (vol == 0.0) continue;
        scatterCouplingFlux(mesh, state, cps[k], sign * vol / window_dt);
    }
}

} // namespace openswmm::twoD
