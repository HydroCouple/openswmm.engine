/**
 * @file SurfaceTangent.cpp
 * @brief Analytic J·v for the 2D diffusive-wave surface RHS. See header.
 * @ingroup engine_2d
 */

#ifdef OPENSWMM_HAS_2D

#include "SurfaceTangent.hpp"
#include "../mesh/VfrClosure.hpp"
#include "../data/BoundaryData.hpp"
#include "../coupling/NodeCoupling.hpp"  // computeNodeCouplingQ (orifice FD linearization)

#include <cmath>

#if defined(SWMM_USE_OPENMP)
#include <omp.h>
#endif

namespace openswmm::twoD {

namespace {

// --- pure-math helpers, mirroring SurfaceFluxCalculator.cpp exactly. These are
// closed forms; any drift from the flux they linearize is caught immediately by
// test_2d_analytic_jv (analytic-vs-FD parity). ---------------------------------

inline int tri_nbr(const MeshData& mesh, int t, int e) noexcept {
    switch (e) {
        case 0:  return mesh.tri_nbr0[t];
        case 1:  return mesh.tri_nbr1[t];
        default: return mesh.tri_nbr2[t];
    }
}

inline void edgeEndpointZ(const MeshData& mesh, int t, int e,
                          double& z_lo, double& z_hi) noexcept {
    const int v[3] = {mesh.tri_v0[t], mesh.tri_v1[t], mesh.tri_v2[t]};
    const double za = mesh.vz[v[(e + 1) % 3]];
    const double zb = mesh.vz[v[(e + 2) % 3]];
    z_lo = (za < zb) ? za : zb;
    z_hi = (za < zb) ? zb : za;
}

// faceDepthFromEta and its derivative w.r.t. η (SurfaceFluxCalculator Eq. 14).
inline double faceDepth(double eta, double z_lo, double z_hi) noexcept {
    if (eta <= z_lo) return 0.0;
    const double dz = z_hi - z_lo;
    if (dz < 1.0e-9) return eta - z_lo;
    if (eta <= z_hi) { const double t = eta - z_lo; return t * t / (2.0 * dz); }
    return eta - 0.5 * (z_lo + z_hi);
}
inline double faceDepthPrime(double eta, double z_lo, double z_hi) noexcept {
    if (eta <= z_lo) return 0.0;
    const double dz = z_hi - z_lo;
    if (dz < 1.0e-9) return 1.0;
    if (eta <= z_hi) return (eta - z_lo) / dz;
    return 1.0;
}

inline double regSqrt(double x, double eps) noexcept {
    if (eps <= 0.0 || x >= eps) return std::sqrt(x);
    const double inv = 1.0 / std::sqrt(eps);
    return (1.5 * inv) * x - (0.5 * inv / eps) * x * x;
}
// d/dx regSqrt(x), x ≥ 0.
inline double regSqrtPrime(double x, double eps) noexcept {
    if (eps <= 0.0 || x >= eps) return (x > 1.0e-300) ? 0.5 / std::sqrt(x) : 0.0;
    const double inv = 1.0 / std::sqrt(eps);
    return 1.5 * inv - (inv / eps) * x;
}

// d/ddepth evapSink (Hermite ramp below dry_depth); 0 in the flat region.
inline double evapSinkPrime(double rate, double depth, double dry) noexcept {
    if (rate <= 0.0 || depth <= 0.0 || depth >= dry || dry <= 0.0) return 0.0;
    const double t = depth / dry;
    return rate * 6.0 * t * (1.0 - t) / dry;
}

// dη/dV for the active closure (mean-depth chain rule / cell area), evaluated at
// the cell's current free surface η = state.head[i].
inline double dEtaDV(const MeshData& m, const SurfaceStateData& s,
                     const SolverOptions2D& o, int i) noexcept {
    const double A = m.tri_area[i];
    if (A <= 1.0e-30) return 0.0;
    if (o.cell_closure == CellClosure2D::VFR) {
        double z1 = m.vz[m.tri_v0[i]], z2 = m.vz[m.tri_v1[i]], z3 = m.vz[m.tri_v2[i]];
        vfrSort3(z1, z2, z3);
        // depth = V/A ⇒ dη/dV = (dη/dh̄ at η)·(1/A).
        return vfrDEtaDMeanDepth(z1, z2, z3, s.head[i], o.vfr_min_wet_frac) / A;
    }
    return 1.0 / A;   // FLAT: η = z_c + V/A
}

// ∂F_bc/∂V_i for one boundary edge. Mirrors boundaryEdgeFlux in
// SurfaceFluxCalculator.cpp exactly (frozen upwind/gate branch, as FD-Jv).
inline double boundaryTangent(const MeshData& mesh, const SurfaceStateData& state,
                              const SolverOptions2D& opts, int i, int slot,
                              double detaI, bool vfr_face, double dh_eps) noexcept {
    const BoundaryData* b = state.boundary;
    if (!b) return 0.0;
    const double L = mesh.edge_length[slot];
    const double n = mesh.mannings_n[i];
    const double depth = state.depth[i];
    const int e = slot % 3;

    switch (static_cast<BoundaryType>(b->edge_bc_type[slot])) {
        case BoundaryType::NORMAL_FLOW: {
            const double S = b->edge_bed_slope[slot];
            if (S <= 0.0 || depth <= 0.0 || n <= 0.0) return 0.0;
            // F = −(h_out^{5/3}·√S/n)·L. h_out = depth (or faceDepth(η_i)).
            double h_out = depth, dhout_dV = 1.0 / mesh.tri_area[i];
            if (vfr_face) {
                double z_lo, z_hi; edgeEndpointZ(mesh, i, e, z_lo, z_hi);
                h_out = faceDepth(state.head[i], z_lo, z_hi);
                if (h_out <= 0.0) return 0.0;
                dhout_dV = faceDepthPrime(state.head[i], z_lo, z_hi) * detaI;
            }
            const double dF_dhout = -(5.0 / 3.0) * std::cbrt(h_out * h_out)
                                    * std::sqrt(S) / n * L;
            return dF_dhout * dhout_dV;
        }
        case BoundaryType::SPECIFIED_STAGE: {
            if (n <= 0.0) return 0.0;
            const double h_bc = b->edge_bc_head[slot];
            const double dh   = state.head[i] - h_bc;
            const double A    = mesh.tri_area[i];
            const double dx_b = (L > 1.0e-12) ? (2.0 * A) / (3.0 * L) : 0.0;
            if (dx_b <= 1.0e-12) return 0.0;
            // Upwind conveyance depth (h_up depends on V_i only on OUTFLOW).
            double h_up, dhup_dV = 0.0;
            if (vfr_face) {
                double z_lo, z_hi; edgeEndpointZ(mesh, i, e, z_lo, z_hi);
                const double eta_up = (dh > 0.0) ? state.head[i] : h_bc;
                h_up = faceDepth(eta_up, z_lo, z_hi);
                if (dh > 0.0) dhup_dV = faceDepthPrime(eta_up, z_lo, z_hi) * detaI;
            } else {
                h_up = (dh > 0.0) ? depth
                                  : std::max(h_bc - mesh.tri_cz[i], 0.0);
                if (dh > 0.0) dhup_dV = 1.0 / A;
            }
            if (h_up <= 0.0) return 0.0;
            const double C_b  = L / (n * std::sqrt(dx_b));
            const double h53  = h_up * std::cbrt(h_up * h_up);
            const double absd = std::abs(dh);
            const double sign = (dh > 0.0) ? 1.0 : (dh < 0.0 ? -1.0 : 0.0);
            const double Phi  = sign * regSqrt(absd, dh_eps);
            const double dF_dDh  = -C_b * h53 * regSqrtPrime(absd, dh_eps);
            const double dF_dhup = -C_b * (5.0 / 3.0) * std::cbrt(h_up * h_up) * Phi;
            return dF_dDh * detaI + dF_dhup * dhup_dV;   // ∂dh/∂V_i = detaI
        }
        default:
            return 0.0;   // WALL / SPECIFIED_FLOW / RATING_CURVE: constant in y
    }
}

}  // namespace

void buildSurfaceTangents(const MeshData& mesh, SurfaceStateData& state,
                          const SolverOptions2D& opts, SurfaceTangents& tang) {
    const int nt = mesh.n_triangles();
    if (tang.nt != nt) tang.resize(nt);

    const double dh_eps = opts.flux_dh_eps;
    const bool vfr_face = (opts.face_reconstruction == FaceDepth2D::VFR_FACE);

    // Per-cell dη/dV (closure chain rule) reused across a cell's three edges.
    // Cheap; recomputed here so the pass is self-contained.
#pragma omp parallel for schedule(static) num_threads(opts.num_threads)
    for (int i = 0; i < nt; ++i) {
        const double detaI = dEtaDV(mesh, state, opts, i);
        double diag = 0.0;

        // Evaporation-sink diagonal: ∂(A_i·(−evapSink))/∂V_i = −∂evapSink/∂depth
        // (depth = V/A cancels the area). rain/coupling_flux are held constants.
        diag -= evapSinkPrime(state.evap_rate[i], state.depth[i], opts.dry_depth);

        for (int e = 0; e < 3; ++e) {
            const int slot = i * 3 + e;
            const int nbr  = tri_nbr(mesh, i, e);
            tang.dfdvi[slot]   = 0.0;
            tang.dfdvnbr[slot] = 0.0;
            if (nbr < 0) {
                // Boundary edge: diagonal-only tangent (∂F_bc/∂V_i). The
                // y-dependent types (NORMAL_FLOW, SPECIFIED_STAGE) mirror the
                // exact boundaryEdgeFlux formulas; WALL/SPECIFIED_FLOW/RATING
                // are constant in y ⇒ 0.
                tang.dfdvi[slot] = boundaryTangent(mesh, state, opts, i, slot,
                                                   detaI, vfr_face, dh_eps);
                continue;
            }

            const double h_L = state.head[i];
            const double h_R = state.head[nbr];
            const int    up  = (h_L >= h_R) ? i : nbr;
            double depth_up  = state.depth[up];
            if (depth_up <= 0.0) continue;   // dry source ⇒ F ≡ 0, tangent 0

            // §VFR_FACE conveyance depth AT the edge (piecewise-C¹ in η_up).
            double fdp = 1.0;                // d(face depth)/dη_up factor
            if (vfr_face) {
                double z_lo, z_hi;
                edgeEndpointZ(mesh, i, e, z_lo, z_hi);
                const double eta_up = (up == i) ? h_L : h_R;
                depth_up = faceDepth(eta_up, z_lo, z_hi);
                if (depth_up <= 0.0) continue;
                fdp = faceDepthPrime(eta_up, z_lo, z_hi);
            }

            const double dx_x = mesh.tri_cx[i] - mesh.tri_cx[nbr];
            const double dx_y = mesh.tri_cy[i] - mesh.tri_cy[nbr];
            const double dx   = std::sqrt(dx_x * dx_x + dx_y * dx_y);
            if (dx < 1.0e-12) continue;

            const double dh     = h_L - h_R;
            const double abs_dh = std::abs(dh);
            const double sign   = (dh > 0.0) ? 1.0 : (dh < 0.0 ? -1.0 : 0.0);
            const double C      = mesh.edge_length[slot]
                                  / (mesh.mannings_n[up] * std::sqrt(dx))
                                  * mesh.edge_conveyance[slot];

            const double h53 = depth_up * std::cbrt(depth_up * depth_up);   // h^{5/3}
            const double Phi = sign * regSqrt(abs_dh, dh_eps);
            const double dPhi = regSqrtPrime(abs_dh, dh_eps);

            // F = −C·h_up^{5/3}·Φ(Δh). Frozen upwind + gate branch (as FD-Jv).
            const double dF_dDh  = -C * h53 * dPhi;                     // via Δh
            const double dF_dhup = -C * (5.0 / 3.0)
                                   * std::cbrt(depth_up * depth_up) * Phi; // via h_up

            const double detaN = dEtaDV(mesh, state, opts, nbr);
            // ∂Δh/∂η_i = +1, ∂Δh/∂η_nbr = −1.
            double dfi = dF_dDh * detaI;
            double dfn = -dF_dDh * detaN;

            // Upwind-depth path: h_up depends on the UPWIND cell only.
            //   VFR_FACE: h_up = faceDepth(η_up) ⇒ d/dV_up = faceDepth'·dη_up/dV_up
            //   else:     h_up = V_up/A_up (mean) ⇒ d/dV_up = 1/A_up
            const double ddepth_dVup = vfr_face
                ? fdp * ((up == i) ? detaI : detaN)
                : 1.0 / mesh.tri_area[up];
            if (up == i) dfi += dF_dhup * ddepth_dVup;
            else         dfn += dF_dhup * ddepth_dVup;

            tang.dfdvi[slot]   = dfi;
            tang.dfdvnbr[slot] = dfn;
        }
        tang.diag[i] = diag;
    }

    // --- Live single-cell orifice-coupling tangent (Phase 3d) -----------------
    // Each live coupling point is a single-cell (centroid) point: Q_k depends
    // only on the driving cell c's volume (η_c and h̄_c both from V_c). Linearize
    // Q_k about V_c by a local central FD — perturb (head, depth) consistently
    // with a ±dV volume bump (depth = V/A exactly; dη = dEtaDV·dV; the central
    // difference cancels the O(dV²) closure curvature). Assembled once per setup
    // (2 Q evals per point, « cells), so applyTangentJv stays a pure SpMV. The
    // cell-row self term −dQ/dV_c folds into diag[c]; the accumulator rows get
    // dQ/dV_c. Runs serially after the threaded cell loop (no race on diag[]).
    if (state.node_coupling != nullptr && state.nodes_1d != nullptr) {
        const auto& cps   = *state.node_coupling;
        const auto& nodes = *state.nodes_1d;
        const int   nc    = static_cast<int>(cps.size());
        tang.coupling_cell.assign(static_cast<std::size_t>(nc), -1);
        tang.coupling_dQdV.assign(static_cast<std::size_t>(nc), 0.0);
        for (int k = 0; k < nc; ++k) {
            const CouplingPoint& cp = cps[k];
            const int c = cp.cell_idx;
            // Only single-cell centroid points are analytically linearizable
            // here; a vertex-stencil point (legacy live path) is left at −1 and
            // makes analyticJvEligible fall back to FD for the whole system.
            if (cp.vertex_idx >= 0 || c < 0) continue;

            const double A     = mesh.tri_area[c];
            if (A <= 1.0e-30) continue;
            const double detaC = dEtaDV(mesh, state, opts, c);
            const double h0    = state.head[c];
            const double d0    = state.depth[c];
            // Volume step scaled to the cell (never below a small floor so a dry
            // cell still gets a finite, well-conditioned difference).
            const double dV = 1.0e-6 * A * std::max(d0, opts.dry_depth) + 1.0e-12;
            const double dh = detaC * dV;
            const double dd = dV / A;

            state.head[c] = h0 + dh;  state.depth[c] = d0 + dd;
            const double Qp = computeNodeCouplingQ(cp, mesh, state, nodes, opts);
            state.head[c] = h0 - dh;  state.depth[c] = d0 - dd;
            const double Qm = computeNodeCouplingQ(cp, mesh, state, nodes, opts);
            state.head[c] = h0;       state.depth[c] = d0;   // restore

            const double dQdV = (Qp - Qm) / (2.0 * dV);
            tang.coupling_cell[k] = c;
            tang.coupling_dQdV[k] = dQdV;
            // ydot_c += −Q ⇒ ∂(ydot_c)/∂V_c = −dQ/dV_c. Additive if two points
            // share a cell.
            tang.diag[c] -= dQdV;
        }
    } else {
        tang.coupling_cell.clear();
        tang.coupling_dQdV.clear();
    }
}

void applyTangentJv(const MeshData& mesh, const SolverOptions2D& opts,
                    const SurfaceTangents& tang, int nc,
                    const double* v, double* Jv) {
    const int nt = mesh.n_triangles();
#pragma omp parallel for schedule(static) num_threads(opts.num_threads)
    for (int i = 0; i < nt; ++i) {
        double acc = tang.diag[i] * v[i];
        for (int e = 0; e < 3; ++e) {
            const int slot = i * 3 + e;
            acc += tang.dfdvi[slot] * v[i];
            const int nbr = tri_nbr(mesh, i, e);
            if (nbr >= 0) acc += tang.dfdvnbr[slot] * v[nbr];
        }
        Jv[i] = acc;
    }
    // Augmented ∫Q dt accumulator rows. Live single-cell coupling: row k depends
    // only on its driving cell c, dC_k/dt = Q_k ⇒ (J·v)_k = dQ_k/dV_c · v[c].
    // Held path (no coupling tangent stored): the row is y-independent ⇒ zero.
    const int ncc = static_cast<int>(tang.coupling_cell.size());
    for (int k = 0; k < nc; ++k) {
        const int c = (k < ncc) ? tang.coupling_cell[k] : -1;
        Jv[nt + k] = (c >= 0) ? tang.coupling_dQdV[k] * v[c] : 0.0;
    }
}

}  // namespace openswmm::twoD

#endif  // OPENSWMM_HAS_2D
