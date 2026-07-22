/**
 * @file SurfaceTangent.cpp
 * @brief Analytic J·v for the 2D diffusive-wave surface RHS. See header.
 * @ingroup engine_2d
 */

#ifdef OPENSWMM_HAS_2D

#include "SurfaceTangent.hpp"
#include "../mesh/VfrClosure.hpp"

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
            if (nbr < 0) continue;   // boundary: solver stays on FD-Jv (see .hpp)

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
    // Augmented accumulator rows are y-independent on the held path (time-only
    // deviation forcing) ⇒ their J·v row is exactly zero.
    for (int k = 0; k < nc; ++k) Jv[nt + k] = 0.0;
}

}  // namespace openswmm::twoD

#endif  // OPENSWMM_HAS_2D
