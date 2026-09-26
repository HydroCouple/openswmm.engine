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
 * @file SoilCharacteristic.cpp
 * @brief G-steps 2 and 13 — see the header, especially its @warning about
 *        which `q₀` rates are published closed forms and which are ours.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "SoilCharacteristic.hpp"

#include <algorithm>
#include <cmath>

namespace openswmm::twoD::soil {

namespace {

/// Below this suction the column is at the table and everything is saturated.
constexpr double kPsiEps = 1.0e-12;
/// Floor on the column thickness so `L → 0` (water table at the surface)
/// divides by something finite. A millimetre of column is already the Dunne
/// regime, where the saturated update takes over.
constexpr double kLMin = 1.0e-3;
/// Se is kept strictly inside (0, 1] so `Se^(1/m)` and its inverse stay finite.
constexpr double kSeMin = 1.0e-8;

/// 4-point Gauss–Legendre on [0, 1]: enough for a monotone retention curve,
/// and fixed-order so the cost is a compile-time constant (the plan budgets
/// van Genuchten at ≈2× Gardner, which this meets).
constexpr double kGlX[4] = {0.0694318442029737, 0.3300094782075719,
                            0.6699905217924281, 0.9305681557970263};
constexpr double kGlW[4] = {0.1739274225687269, 0.3260725774312731,
                            0.3260725774312731, 0.1739274225687269};

double clampSe(double se) noexcept {
    return std::clamp(se, kSeMin, 1.0);
}

}  // namespace

// ---------------------------------------------------------------------------
// Retention and conductivity
// ---------------------------------------------------------------------------

double effectiveSaturation(const Params& p, double psi) noexcept {
    if (!(psi > kPsiEps)) return 1.0;
    switch (p.law) {
        case SoilChar::GARDNER:
            // Gardner's exponential retention: Se = e^(−αψ). Not
            // Mualem-consistent (the reason Russo is the default), but it is
            // the form that yields eq. 22.
            return clampSe(std::exp(-p.alpha * psi));
        case SoilChar::RUSSO:
            // Russo (1988) retention consistent with Mualem at L = 0.5:
            // Se = [ (1 + ½αψ)·e^(−½αψ) ]^(2/(2+L))  →  with L = 0.5 the
            // exponent is 4/5. Kr below is then the Mualem integral of it.
            {
                const double a = 0.5 * p.alpha * psi;
                const double base = (1.0 + a) * std::exp(-a);
                const double expo = 2.0 / (2.0 + p.vg_L);
                return clampSe(std::pow(std::max(base, kSeMin), expo));
            }
        case SoilChar::BROOKS_COREY:
            // Se = 1 for ψ ≤ ψ_b, else (ψ_b/ψ)^λ. The branch at the
            // air-entry pressure is the non-smoothness the draft flagged.
            {
                const double pb = std::max(p.psi_b, kPsiEps);
                if (psi <= pb) return 1.0;
                return clampSe(std::pow(pb / psi, p.lambda));
            }
        case SoilChar::VAN_GENUCHTEN:
            {
                const double n = std::max(p.vg_n, 1.0 + 1.0e-6);
                const double m = 1.0 - 1.0 / n;
                const double ap = std::pow(p.alpha * psi, n);
                return clampSe(std::pow(1.0 + ap, -m));
            }
    }
    return 1.0;
}

double waterContent(const Params& p, double psi) noexcept {
    return p.theta_r + effectiveSaturation(p, psi) * (p.theta_s - p.theta_r);
}

double relativeConductivity(const Params& p, double psi) noexcept {
    if (!(psi > kPsiEps)) return 1.0;
    switch (p.law) {
        case SoilChar::GARDNER:
            return std::clamp(std::exp(-p.alpha * psi), 0.0, 1.0);
        case SoilChar::RUSSO:
            {
                // Kr = [ (1 + ½αψ)·e^(−½αψ) ]² — Russo's own form, which is
                // exactly the Mualem integral of his retention curve. This is
                // why he costs the same as Gardner: one exp, no quadrature.
                const double a = 0.5 * p.alpha * psi;
                const double t = (1.0 + a) * std::exp(-a);
                return std::clamp(t * t, 0.0, 1.0);
            }
        case SoilChar::BROOKS_COREY:
            {
                const double pb = std::max(p.psi_b, kPsiEps);
                if (psi <= pb) return 1.0;
                const double e = 2.0 + 3.0 * p.lambda;
                return std::clamp(std::pow(pb / psi, e), 0.0, 1.0);
            }
        case SoilChar::VAN_GENUCHTEN:
            {
                const double n = std::max(p.vg_n, 1.0 + 1.0e-6);
                const double m = 1.0 - 1.0 / n;
                const double se = clampSe(effectiveSaturation(p, psi));
                const double inner = 1.0 - std::pow(1.0 - std::pow(se, 1.0 / m), m);
                return std::clamp(std::pow(se, p.vg_L) * inner * inner, 0.0, 1.0);
            }
    }
    return 1.0;
}

double conductivity(const Params& p, double psi) noexcept {
    return p.Ks * relativeConductivity(p, psi);
}

double suctionAtSaturation(const Params& p, double Se) noexcept {
    const double se = clampSe(Se);
    if (se >= 1.0 - 1.0e-12) return 0.0;
    switch (p.law) {
        case SoilChar::GARDNER:
            return -std::log(se) / std::max(p.alpha, kPsiEps);
        case SoilChar::RUSSO:
            {
                // (1 + a)·e^(−a) = se^((2+L)/2) has no elementary inverse;
                // three Newton steps from the Gardner guess converge to
                // machine precision over the whole range (the function is
                // smooth and monotone in a).
                const double target =
                    std::pow(se, (2.0 + p.vg_L) * 0.5);
                double a = std::max(-std::log(std::max(target, kSeMin)), 0.0);
                for (int it = 0; it < 6; ++it) {
                    const double f  = (1.0 + a) * std::exp(-a) - target;
                    const double df = -a * std::exp(-a);     // d/da
                    if (std::fabs(df) < 1.0e-300) break;
                    const double step = f / df;
                    a -= step;
                    if (a < 0.0) a = 0.0;
                    if (std::fabs(step) < 1.0e-13) break;
                }
                return 2.0 * a / std::max(p.alpha, kPsiEps);
            }
        case SoilChar::BROOKS_COREY:
            return std::max(p.psi_b, kPsiEps) *
                   std::pow(se, -1.0 / std::max(p.lambda, 1.0e-6));
        case SoilChar::VAN_GENUCHTEN:
            {
                const double n = std::max(p.vg_n, 1.0 + 1.0e-6);
                const double m = 1.0 - 1.0 / n;
                const double t = std::pow(se, -1.0 / m) - 1.0;
                return std::pow(std::max(t, 0.0), 1.0 / n) /
                       std::max(p.alpha, kPsiEps);
            }
    }
    return 0.0;
}

double diffusivity(const Params& p, double psi) noexcept {
    // D(θ) = K(ψ)·dψ/dθ. dθ/dψ = (θ_s − θ_r)·dSe/dψ, evaluated by a centred
    // difference in ψ — the analytic derivatives differ per law and per
    // branch, and the column's diffusive term is optional and small (D-N3
    // defaults it OFF), so a second-order difference is the right cost.
    const double dpsi = std::max(1.0e-4, 1.0e-3 * std::fabs(psi));
    const double se_hi = effectiveSaturation(p, psi + dpsi);
    const double se_lo = effectiveSaturation(p, std::max(psi - dpsi, 0.0));
    const double dse   = (se_hi - se_lo) / (psi + dpsi - std::max(psi - dpsi, 0.0));
    const double dtheta_dpsi = dse * (p.theta_s - p.theta_r);
    if (std::fabs(dtheta_dpsi) < 1.0e-12) return 0.0;
    return conductivity(p, psi) / std::fabs(dtheta_dpsi);
}

// ---------------------------------------------------------------------------
// Equilibrium storage and recharge
// ---------------------------------------------------------------------------

/// `∫₀ᴸ Se(ψ) dψ` — the SATURATION integral. Internal, because it is the
/// scale eq. 22 is written on, not a quantity anyone outside this file should
/// hold. `equilibriumStorage` converts it to metres of water, which is what
/// every other `hu` in the engine means.
static double saturationIntegral(const Params& p, double Lc) noexcept {
    switch (p.law) {
        case SoilChar::GARDNER:
            // ∫₀ᴸ e^(−αψ) dψ = (1 − e^(−αL))/α — exactly eq. 39's hᵤ*.
            return (1.0 - std::exp(-p.alpha * Lc)) / std::max(p.alpha, kPsiEps);
        case SoilChar::BROOKS_COREY: {
            // ∫₀ᴸ Se dψ = ψ_b + ∫_{ψ_b}^{L} (ψ_b/ψ)^λ dψ  (L > ψ_b)
            const double pb = std::max(p.psi_b, kPsiEps);
            if (Lc <= pb) return Lc;
            const double lam = std::max(p.lambda, 1.0e-6);
            if (std::fabs(lam - 1.0) < 1.0e-9)
                return pb + pb * std::log(Lc / pb);
            return pb + pb * (std::pow(Lc / pb, 1.0 - lam) - 1.0) / (1.0 - lam);
        }
        case SoilChar::RUSSO:
        case SoilChar::VAN_GENUCHTEN:
        default: {
            double acc = 0.0;   // 4-point Gauss-Legendre on [0, L]
            for (int g = 0; g < 4; ++g)
                acc += kGlW[g] * effectiveSaturation(p, kGlX[g] * Lc);
            return acc * Lc;
        }
    }
}

double equilibriumStorage(const Params& p, double L) noexcept {
    // METRES OF WATER: `∫₀ᴸ θ(ψ) dψ = θ_r·L + (θ_s − θ_r)·∫₀ᴸ Se dψ`.
    //
    // This used to return the saturation integral, because that is the scale
    // Qu & Duffy's eq. 22 measures `hᵤ` on. It made eq. 22 read beautifully
    // and every other line of the engine wrong: `SubsurfaceState::storage()`
    // adds `hu` to `h_g·θ_s`, the closure-A cap is `θ_s·L`, and the ENSLAVED
    // reduction needs `d(hᵤ*)/dL = θ(L)` — all three are water depths. The
    // seeded column came out ABOVE saturation, the cap fired on step 0, and
    // the closure then drained a cell dry and kept going.
    //
    // So the public quantity is water, and eq. 22 converts on entry. Gates
    // 1, 2 and 4 are what this note is for.
    const double Lc = std::max(L, kLMin);
    return p.theta_r * Lc +
           (p.theta_s - p.theta_r) * saturationIntegral(p, Lc);
}

double enslavedStorage(const Params& p, double L) noexcept {
    return equilibriumStorage(p, L);
}

double rechargeQ0(const Params& p, double L, double hu) noexcept {
    const double Lc = std::max(L, kLMin);

    // `hu` arrives as metres of WATER. Eq. 22 is written on the saturation
    // scale, so convert once, here, at the only place that needs it.
    const double span = std::max(p.theta_s - p.theta_r, 1.0e-12);
    const double hu_se = (hu - p.theta_r * Lc) / span;

    if (p.law == SoilChar::GARDNER) {
        // Eq. 22, verbatim. Written in the published form (not the factored
        // one) so a reader can compare it to the paper line for line; the
        // header records that the two are the same expression.
        const double e = std::exp(-p.alpha * Lc);
        const double den = p.alpha * Lc * (1.0 - e);
        if (!(std::fabs(den) > 1.0e-300)) return 0.0;
        const double q = p.Ks * (1.0 - e - p.alpha * hu_se) / den;
        return std::isfinite(q) ? q : 0.0;
    }

    // The generalised relaxation form (see the header's @warning — the rate
    // below is ours, the equilibrium is the law's):
    //     q₀ = C(L)·[hᵤ*(L) − hᵤ],   C(L) = K̄ / (L · hᵤ*(L)/L)  →  K̄/hᵤ*
    // where K̄ is the conductivity at the column's mean suction. Reducing to
    // Gardner: K̄ → Ks·e^(−αL/2) and hᵤ* → (1−e^(−αL))/α gives the same order
    // and the same limits, which is the consistency this rests on.
    // Relaxation on the SATURATION scale, so the rate constant means the
    // same thing it does for Gardner, then reported as a water flux.
    const double hu_eq_se = saturationIntegral(p, Lc);
    if (!(hu_eq_se > 1.0e-12)) return 0.0;
    const double Kbar = conductivity(p, 0.5 * Lc);
    const double C = Kbar / hu_eq_se;
    const double q = C * (hu_eq_se - hu_se);
    return std::isfinite(q) ? q : 0.0;
}

double alphaL(const Params& p, double L) noexcept {
    const double Lc = std::max(L, kLMin);
    switch (p.law) {
        case SoilChar::BROOKS_COREY:
            // 1/ψ_b is Brooks–Corey's inverse length, so αL means the same
            // "column thickness in capillary lengths" across every law and
            // the §2.4 thresholds keep one interpretation.
            return Lc / std::max(p.psi_b, kPsiEps);
        default:
            return p.alpha * Lc;
    }
}

double feddesStress(double psi, double psi_w) noexcept {
    // Smooth (C¹) ramp from full transpiration to zero at the wilting point.
    // Deliberately NOT the legacy `if (infil <= 0)` gate, and deliberately
    // not a hard piecewise linear: a kink here shows up as a kink in the ET
    // ledger that reads like a solver artefact.
    const double pw = std::max(psi_w, 1.0e-6);
    if (psi <= 0.0) return 1.0;
    if (psi >= pw)  return 0.0;
    const double x = psi / pw;                 // 0 wet → 1 wilting
    const double s = 1.0 - x;                  // 1 wet → 0 wilting
    return s * s * (3.0 - 2.0 * s);            // smoothstep
}

}  // namespace openswmm::twoD::soil
