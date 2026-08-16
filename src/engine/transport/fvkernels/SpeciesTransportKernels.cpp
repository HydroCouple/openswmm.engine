// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file SpeciesTransportKernels.cpp
 * @brief Shared 1D FV species transport kernels — bodies moved VERBATIM from
 *        hydraulics/fv/ExplicitFvSolver.cpp (phase E0; see the header).
 *
 * @details Local reference aliases at the top of each function reproduce the
 *          member names the bodies were written against (`mesh_`, `state_`,
 *          `cell_slope_`, ...) so the moved code is textually identical to
 *          the pre-move solver code — the cheapest possible bitwise-identity
 *          argument. Do not "clean up" the aliases: they are the point.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "SpeciesTransportKernels.hpp"

namespace openswmm::transport::fvkernels {

namespace k = openswmm::fv::kernels;

// ===========================================================================
// Scalar reconstruction — the anti-diffusion layer (plan §3.2)
// ===========================================================================

void reconstructScalars(const SpeciesKernelView& v, double dt) {
    const NetworkMeshData* mesh_  = v.mesh;
    NetworkStateData*      state_ = v.state;
    auto&       cell_slope_   = *v.cell_slope;
    auto&       f_phi_l_      = *v.f_phi_l;
    auto&       f_phi_r_      = *v.f_phi_r;
    const auto& f_sstar_      = *v.f_sstar;
    const auto& cell_u_       = *v.cell_u;
    const auto& active_faces_ = *v.active_faces;

    const int ns = state_->n_species;
    if (ns <= 0) return;

    const int nc = mesh_->n_cells();
    const int nf = mesh_->n_faces();

    for (int s = 0; s < ns; ++s) {
        const auto sbase = static_cast<std::size_t>(s) * static_cast<std::size_t>(nc);
        const auto fbase = static_cast<std::size_t>(s) * static_cast<std::size_t>(nf);

        // ---- per-cell limited slope along the chain ------------------------
        if (v.scalar_scheme == ScalarScheme::UPWIND) {
            std::fill(cell_slope_.begin(), cell_slope_.end(), 0.0);
        } else {
            for (int ch = 0; ch < mesh_->n_chains(); ++ch) {
                const int b = mesh_->chain_ptr[static_cast<std::size_t>(ch)];
                const int e = mesh_->chain_ptr[static_cast<std::size_t>(ch) + 1];
                for (int i = b; i < e; ++i) {
                    const int c = mesh_->chain_cells[static_cast<std::size_t>(i)];
                    const auto uc = static_cast<std::size_t>(c);
                    if (i == b || i == e - 1) { cell_slope_[uc] = 0.0; continue; }
                    const int cm = mesh_->chain_cells[static_cast<std::size_t>(i - 1)];
                    const int cp = mesh_->chain_cells[static_cast<std::size_t>(i + 1)];
                    const auto um = static_cast<std::size_t>(cm);
                    const auto up = static_cast<std::size_t>(cp);
                    const double dm = 0.5 * (mesh_->cell_dx[um] + mesh_->cell_dx[uc]);
                    const double dp = 0.5 * (mesh_->cell_dx[uc] + mesh_->cell_dx[up]);
                    const double gm = (state_->cell_phi[sbase + uc] -
                                       state_->cell_phi[sbase + um]) / dm;
                    const double gp = (state_->cell_phi[sbase + up] -
                                       state_->cell_phi[sbase + uc]) / dp;
                    // Chain-space slope → the cell's OWN axis.
                    cell_slope_[uc] =
                        limitSlope(gm, gp, v.limiter) *
                        static_cast<double>(mesh_->chain_dir[static_cast<std::size_t>(i)]);
                }
            }
        }

        // ---- face values ----------------------------------------------------
        for (const int f : active_faces_) {
            const auto uf = static_cast<std::size_t>(f);
            const int cl = mesh_->face_cl[uf];
            const int cr = mesh_->face_cr[uf];
            // A node ghost presents the interior cell's own value (zero
            // gradient). That keeps a uniform field uniform under inflow, which
            // is the §6.11(a) gate, without inventing a node concentration this
            // solver does not track.
            const int el = (cl >= 0) ? cl : cr;
            const int er = (cr >= 0) ? cr : cl;
            const auto ul = static_cast<std::size_t>(el);
            const auto ur = static_cast<std::size_t>(er);

            double phil = state_->cell_phi[sbase + ul];
            double phir = state_->cell_phi[sbase + ur];
            if (cl >= 0) {
                const double sign = static_cast<double>(mesh_->face_dir_l[uf]);
                phil += sign * cell_slope_[ul] * 0.5 * mesh_->cell_dx[ul];
            }
            if (cr >= 0) {
                const double sign = -static_cast<double>(mesh_->face_dir_r[uf]);
                phir += sign * cell_slope_[ur] * 0.5 * mesh_->cell_dx[ur];
            }

            if (v.scalar_scheme == ScalarScheme::QUICKEST_ULTIMATE) {
                // QUICKEST needs TWO upstream cells. Where the stencil exists
                // it is 3rd order; where it does not — the first cell below a
                // manhole with several inflowing pipes, or a short conduit —
                // it degrades, and the MUSCL value computed above is the
                // fallback. In COARSE mode a sewer spends much of its length in
                // that degraded regime, which is exactly what the §6.11(d)
                // junction-density sweep is there to quantify.
                const bool fwd = (f_sstar_[uf] >= 0.0);
                const int cu = fwd ? cl : cr;         // upwind cell
                const int cd = fwd ? cr : cl;         // downwind cell
                if (cu >= 0 && cd >= 0) {
                    const auto uu = static_cast<std::size_t>(cu);
                    const int chn = mesh_->cell_chain[uu];
                    const int pos = mesh_->cell_chain_pos[uu];
                    const int cb = mesh_->chain_ptr[static_cast<std::size_t>(chn)];
                    const int ce = mesh_->chain_ptr[static_cast<std::size_t>(chn) + 1];
                    // Step one further upstream ALONG THE FLOW, which is the
                    // downwind cell's opposite neighbour in the chain.
                    const int dpos = mesh_->cell_chain_pos[static_cast<std::size_t>(cd)];
                    const int upos = pos + (pos - dpos);   // one further upstream
                    if (upos >= 0 && upos < ce - cb) {
                        const int cuu = mesh_->chain_cells[
                            static_cast<std::size_t>(cb + upos)];
                        const auto uuu = static_cast<std::size_t>(cuu);
                        const double pu  = state_->cell_phi[sbase + uu];
                        const double pd  = state_->cell_phi[sbase + static_cast<std::size_t>(cd)];
                        const double puu = state_->cell_phi[sbase + uuu];
                        const double dx  = mesh_->cell_dx[uu];
                        const double cr_no = std::min(
                            1.0, std::fabs(cell_u_[uu]) * dt / std::max(dx, 1.0e-12));
                        const double q = pd - 2.0 * pu + puu;
                        double pf = 0.5 * (pd + pu) - 0.5 * cr_no * (pd - pu) -
                                    (1.0 - cr_no * cr_no) / 6.0 * q;
                        // ULTIMATE monotonicity limiter (Leonard 1991), applied
                        // in normalized variables.
                        const double den = pd - puu;
                        if (std::fabs(den) > 1.0e-30) {
                            const double un_ = (pu - puu) / den;
                            if (un_ <= 0.0 || un_ >= 1.0) {
                                pf = pu;                       // non-monotone ⇒ upwind
                            } else {
                                double fn = (pf - puu) / den;
                                const double hi = std::min(1.0, un_ / std::max(cr_no, 1.0e-12));
                                fn = std::clamp(fn, un_, hi);
                                pf = puu + fn * den;
                            }
                        } else {
                            pf = pu;
                        }
                        if (fwd) phil = pf; else phir = pf;
                    }
                }
            }

            // Local-extremum clamp — the last line of defence for the "no new
            // extrema, no negative concentrations" contract (§6.11b). Both
            // higher-order reconstructions are TVD for PURE advection on a
            // fixed grid; here the cell areas are evolving underneath them
            // (reflections, wetting, drying), and QUICKEST's normalized-variable
            // limiter measurably loses monotonicity in that regime. Clamping the
            // face value into the bracket its own two cells span costs nothing
            // where the scheme is already monotone and restores the guarantee
            // where it is not.
            if (v.scalar_scheme != ScalarScheme::UPWIND) {
                const double a = state_->cell_phi[sbase + ul];
                const double b = state_->cell_phi[sbase + ur];
                const double lo = std::min(a, b);
                const double hi = std::max(a, b);
                phil = std::clamp(phil, lo, hi);
                phir = std::clamp(phir, lo, hi);
            }

            f_phi_l_[fbase + uf] = phil;
            f_phi_r_[fbase + uf] = phir;
        }

        // ---- flux assembly + Zalesak limiting ------------------------------
        limitSpeciesFluxes(v, s, dt);
    }
}

/**
 * @brief Assemble the face species fluxes and limit them (Zalesak FCT).
 *
 * @details Bracketing the reconstructed FACE value between its two cells is
 *          necessary but NOT sufficient for the discrete maximum principle:
 *          under reversing, unsteady flow with the cell areas evolving
 *          underneath the scalar, a face value legitimately inside its bracket
 *          can still drain more solute than its donor cell holds. Measured:
 *          QUICKEST-ULTIMATE produced −1.3e-3 on a step-function advection case
 *          with wall reflections, which is not acceptable in a water-quality
 *          model.
 *
 *          The fix has to limit the FLUX, not the result. Clipping the updated
 *          concentration would enforce the bound but destroy exact solute
 *          conservation (§6.11c) — the two properties trade off, and Zalesak
 *          (1979) is the construction that keeps both: blend each face's
 *          high-order flux back toward first-order upwind by the largest factor
 *          that no incident cell's bound rejects. The SAME limited flux updates
 *          both incident cells, so conservation is untouched.
 */
void limitSpeciesFluxes(const SpeciesKernelView& v, int species, double dt) {
    const NetworkMeshData* mesh_  = v.mesh;
    NetworkStateData*      state_ = v.state;
    const auto& f_mass_       = *v.f_mass;
    const auto& f_state_l_    = *v.f_state_l;
    const auto& f_state_r_    = *v.f_state_r;
    const auto& f_flux_       = *v.f_flux;
    const auto& f_phi_l_      = *v.f_phi_l;
    const auto& f_phi_r_      = *v.f_phi_r;
    const auto& cell_active_  = *v.cell_active;
    const auto& active_faces_ = *v.active_faces;
    auto&       f_phi_flux_   = *v.f_phi_flux;
    auto&       lo_flux_      = *v.lo_flux;
    auto&       anti_flux_    = *v.anti_flux;
    auto&       td_           = *v.td;
    auto&       anew_         = *v.anew;
    auto&       rplus_        = *v.rplus;
    auto&       rminus_       = *v.rminus;
    const bool  hllc_         = v.hllc;

    /// The flux record with the positivity-scaled mass flux substituted in, so
    /// the species rides on exactly the water the hydrodynamic update moved.
    /// (Formerly ExplicitFvSolver::adjustedFlux.)
    const auto adjustedFlux = [&](int face) {
        k::FaceFlux f = f_flux_[static_cast<std::size_t>(face)];
        f.mass = f_mass_[static_cast<std::size_t>(face)];
        return f;
    };

    const int nc = mesh_->n_cells();
    const int nf = mesh_->n_faces();
    const auto cbase = static_cast<std::size_t>(species) *
                       static_cast<std::size_t>(nc);
    const auto fbase = static_cast<std::size_t>(species) *
                       static_cast<std::size_t>(nf);

    lo_flux_.assign(static_cast<std::size_t>(nf), 0.0);
    anti_flux_.assign(static_cast<std::size_t>(nf), 0.0);

    // Low-order (first-order upwind on the sign of the mass flux) and the
    // antidiffusive remainder.
    for (const int f : active_faces_) {
        const auto uf = static_cast<std::size_t>(f);
        const int cl = mesh_->face_cl[uf];
        const int cr = mesh_->face_cr[uf];
        const auto ul = static_cast<std::size_t>((cl >= 0) ? cl : cr);
        const auto ur = static_cast<std::size_t>((cr >= 0) ? cr : cl);
        const double fm = f_mass_[uf];
        const double lo = fm * ((fm >= 0.0) ? state_->cell_phi[cbase + ul]
                                            : state_->cell_phi[cbase + ur]);
        const double hi = k::speciesFlux(f_state_l_[uf], f_state_r_[uf],
                                         adjustedFlux(f), f_phi_l_[fbase + uf],
                                         f_phi_r_[fbase + uf], hllc_);
        lo_flux_[uf]   = lo;
        anti_flux_[uf] = hi - lo;
    }

    // Transported-diffused state under the low-order flux alone, plus the
    // local bounds it and its neighbours span.
    td_.assign(static_cast<std::size_t>(nc), 0.0);
    anew_.assign(static_cast<std::size_t>(nc), 0.0);
    for (int c = 0; c < nc; ++c) {
        const auto uc = static_cast<std::size_t>(c);
        if (!cell_active_[uc]) { td_[uc] = state_->cell_phi[cbase + uc];
                                 anew_[uc] = state_->cell_a[uc]; continue; }
        const int faces[2]    = {mesh_->cell_face0[uc], mesh_->cell_face1[uc]};
        const int8_t sides[2] = {mesh_->cell_side0[uc], mesh_->cell_side1[uc]};
        double dA = 0.0, dm = 0.0;
        for (int e = 0; e < 2; ++e) {
            const auto uf = static_cast<std::size_t>(faces[e]);
            const double sg = (sides[e] == 0) ? -1.0 : 1.0;
            dA += sg * f_mass_[uf];
            dm += sg * lo_flux_[uf];
        }
        const double inv_dx = 1.0 / mesh_->cell_dx[uc];
        const double a_new = std::max(0.0, state_->cell_a[uc] + dt * dA * inv_dx);
        anew_[uc] = a_new;
        const double m = state_->cell_a[uc] * state_->cell_phi[cbase + uc] +
                         dt * dm * inv_dx;
        td_[uc] = (a_new > k::kDryArea) ? m / a_new : state_->cell_phi[cbase + uc];
    }

    rplus_.assign(static_cast<std::size_t>(nc), 1.0);
    rminus_.assign(static_cast<std::size_t>(nc), 1.0);
    for (int c = 0; c < nc; ++c) {
        const auto uc = static_cast<std::size_t>(c);
        if (!cell_active_[uc]) continue;
        const int faces[2]    = {mesh_->cell_face0[uc], mesh_->cell_face1[uc]};
        const int8_t sides[2] = {mesh_->cell_side0[uc], mesh_->cell_side1[uc]};

        double pmax = std::max(state_->cell_phi[cbase + uc], td_[uc]);
        double pmin = std::min(state_->cell_phi[cbase + uc], td_[uc]);
        double pplus = 0.0, pminus = 0.0;
        for (int e = 0; e < 2; ++e) {
            const auto uf = static_cast<std::size_t>(faces[e]);
            const int cl = mesh_->face_cl[uf];
            const int cr = mesh_->face_cr[uf];
            const int nb = (cl == c) ? cr : cl;
            if (nb >= 0) {
                const auto un = static_cast<std::size_t>(nb);
                pmax = std::max({pmax, state_->cell_phi[cbase + un], td_[un]});
                pmin = std::min({pmin, state_->cell_phi[cbase + un], td_[un]});
            }
            const double sg = (sides[e] == 0) ? -1.0 : 1.0;
            const double a = sg * anti_flux_[uf];
            if (a > 0.0) pplus += a; else pminus -= a;
        }
        const double cap = anew_[uc] * mesh_->cell_dx[uc] / dt;
        if (pplus  > 0.0) rplus_[uc]  = std::min(1.0, (pmax - td_[uc]) * cap / pplus);
        if (pminus > 0.0) rminus_[uc] = std::min(1.0, (td_[uc] - pmin) * cap / pminus);
        rplus_[uc]  = std::max(0.0, rplus_[uc]);
        rminus_[uc] = std::max(0.0, rminus_[uc]);
    }

    for (const int f : active_faces_) {
        const auto uf = static_cast<std::size_t>(f);
        const double a = anti_flux_[uf];
        double coef = 1.0;
        if (a != 0.0) {
            const int cl = mesh_->face_cl[uf];
            const int cr = mesh_->face_cr[uf];
            // The face contributes −a to its LEFT cell and +a to its RIGHT one.
            const double from_r = (cr >= 0)
                ? ((a > 0.0) ? rplus_[static_cast<std::size_t>(cr)]
                             : rminus_[static_cast<std::size_t>(cr)])
                : 1.0;
            const double from_l = (cl >= 0)
                ? ((a > 0.0) ? rminus_[static_cast<std::size_t>(cl)]
                             : rplus_[static_cast<std::size_t>(cl)])
                : 1.0;
            coef = std::min(from_r, from_l);
        }
        f_phi_flux_[fbase + uf] = lo_flux_[uf] + coef * a;
    }
}

// ===========================================================================
// Implicit dispersion (D-FV1)
// ===========================================================================

void dispersionSolve(const SpeciesKernelView& v, double dt) {
    const NetworkMeshData* mesh_  = v.mesh;
    NetworkStateData*      state_ = v.state;

    const int ns = state_->n_species;
    if (ns <= 0 || v.dispersion <= 0.0) return;

    const int nc = mesh_->n_cells();
    static thread_local std::vector<double> aa, bb, ccv, rr, xx;

    // One tridiagonal system per chain (Thomas) — cheap, unconditionally
    // stable, and it removes the Δx²/(2·D_L) explicit constraint entirely,
    // which at fine Δx is MORE restrictive than CFL (plan §3.2, D-FV1). Chains
    // span virtual junctions, so a spliced pair disperses as one conduit.
    for (int ch = 0; ch < mesh_->n_chains(); ++ch) {
        const int b = mesh_->chain_ptr[static_cast<std::size_t>(ch)];
        const int e = mesh_->chain_ptr[static_cast<std::size_t>(ch) + 1];
        const int m = e - b;
        if (m < 2) continue;
        aa.assign(static_cast<std::size_t>(m), 0.0);
        bb.assign(static_cast<std::size_t>(m), 0.0);
        ccv.assign(static_cast<std::size_t>(m), 0.0);
        rr.assign(static_cast<std::size_t>(m), 0.0);
        xx.assign(static_cast<std::size_t>(m), 0.0);

        for (int s = 0; s < ns; ++s) {
            const auto base = static_cast<std::size_t>(s) *
                              static_cast<std::size_t>(nc);
            for (int i = 0; i < m; ++i) {
                const int c = mesh_->chain_cells[static_cast<std::size_t>(b + i)];
                const auto uc = static_cast<std::size_t>(c);
                const double dx = mesh_->cell_dx[uc];
                const double a  = std::max(state_->cell_a[uc], k::kDryArea);
                double lo = 0.0, hi = 0.0;
                if (i > 0) {
                    const int cm = mesh_->chain_cells[static_cast<std::size_t>(b + i - 1)];
                    const double am = std::max(
                        state_->cell_a[static_cast<std::size_t>(cm)], k::kDryArea);
                    lo = v.dispersion * 0.5 * (a + am) * dt / (dx * dx * a);
                }
                if (i < m - 1) {
                    const int cp = mesh_->chain_cells[static_cast<std::size_t>(b + i + 1)];
                    const double ap = std::max(
                        state_->cell_a[static_cast<std::size_t>(cp)], k::kDryArea);
                    hi = v.dispersion * 0.5 * (a + ap) * dt / (dx * dx * a);
                }
                aa[static_cast<std::size_t>(i)]  = -lo;
                ccv[static_cast<std::size_t>(i)] = -hi;
                bb[static_cast<std::size_t>(i)]  = 1.0 + lo + hi;
                rr[static_cast<std::size_t>(i)]  = state_->cell_phi[base + uc];
            }
            for (int i = 1; i < m; ++i) {
                const auto ui = static_cast<std::size_t>(i);
                const double w = aa[ui] / bb[ui - 1];
                bb[ui] -= w * ccv[ui - 1];
                rr[ui] -= w * rr[ui - 1];
            }
            const auto ulast = static_cast<std::size_t>(m - 1);
            xx[ulast] = rr[ulast] / bb[ulast];
            for (int i = m - 2; i >= 0; --i) {
                const auto ui = static_cast<std::size_t>(i);
                xx[ui] = (rr[ui] - ccv[ui] * xx[ui + 1]) / bb[ui];
            }
            for (int i = 0; i < m; ++i) {
                const int c = mesh_->chain_cells[static_cast<std::size_t>(b + i)];
                state_->cell_phi[base + static_cast<std::size_t>(c)] =
                    xx[static_cast<std::size_t>(i)];
            }
        }
    }
}

}  // namespace openswmm::transport::fvkernels
