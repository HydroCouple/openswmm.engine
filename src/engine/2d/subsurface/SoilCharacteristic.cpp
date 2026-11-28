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
 * @file      SoilCharacteristic.cpp
 * @brief     G-steps 2 and 13 — see the header, especially its @warning about
 *        which `q₀` rates are published closed forms and which are ours.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "SoilCharacteristic.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace openswmm::twoD::soil {

namespace {

/// Below this suction the column is at the table and everything is saturated.
constexpr double kPsiEps = 1.0e-12;
/// Floor on the column thickness so `L → 0` (water table at the surface)
/// divides by something finite. A millimetre of column is already the Dunne
/// regime, where the saturated update takes over.
constexpr double kLMin = kRechargeLengthFloor;
/// Se is kept strictly inside (0, 1] so `Se^(1/m)` and its inverse stay finite.
constexpr double kSeMin = 1.0e-8;

/// 4-point Gauss–Legendre on [0, 1]: enough for a monotone retention curve,
/// and fixed-order so the cost is a compile-time constant (the plan budgets
/// van Genuchten at ≈2× Gardner, which this meets).
constexpr double kGlX[4] = {0.0694318442029737, 0.3300094782075719, 0.6699905217924281,
                            0.9305681557970263};
constexpr double kGlW[4] = {0.1739274225687269, 0.3260725774312731, 0.3260725774312731,
                            0.1739274225687269};

/**
 * @brief Apply the numerical effective-saturation floor and upper bound.
 * @param[in] se Finite dimensionless candidate effective saturation.
 * @return std::clamp(se,1e-8,1); NaN is not explicitly sanitized. Pure scalar helper, no
 * mutation/allocation.
 */
double clampSe(double se) noexcept {
    return std::clamp(se, kSeMin, 1.0);
}

} // namespace

// ---------------------------------------------------------------------------
// Retention and conductivity
// ---------------------------------------------------------------------------

double effectiveSaturation(const Params& p, double psi) noexcept {
    if (!(psi > kPsiEps))
        return 1.0;
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
            if (psi <= pb)
                return 1.0;
            return clampSe(std::pow(pb / psi, p.lambda));
        }
    case SoilChar::VAN_GENUCHTEN: {
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
    if (!(psi > kPsiEps))
        return 1.0;
    switch (p.law) {
    case SoilChar::GARDNER:
        return std::clamp(std::exp(-p.alpha * psi), 0.0, 1.0);
    case SoilChar::RUSSO: {
        // Kr = [ (1 + ½αψ)·e^(−½αψ) ]² — Russo's own form, which is
        // exactly the Mualem integral of his retention curve. This is
        // why he costs the same as Gardner: one exp, no quadrature.
        const double a = 0.5 * p.alpha * psi;
        const double t = (1.0 + a) * std::exp(-a);
        return std::clamp(t * t, 0.0, 1.0);
    }
    case SoilChar::BROOKS_COREY: {
        const double pb = std::max(p.psi_b, kPsiEps);
        if (psi <= pb)
            return 1.0;
        const double e = 2.0 + 3.0 * p.lambda;
        return std::clamp(std::pow(pb / psi, e), 0.0, 1.0);
    }
    case SoilChar::VAN_GENUCHTEN: {
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

Curve curve(const Params& p, double psi) noexcept {
    Curve c;
    curveBatch(p, &psi, 1, &c);
    return c;
}

namespace {
/// Elements the batches process in one pass: a σ column has 8 layers by default, M_LAYERS
/// authors more; longer arrays go round again.
constexpr int kChunk = 64;
} // namespace

void curveBatch(const Params& p, const double* psi, int n, Curve* out) noexcept {
    for (int start = 0; start < n; start += kChunk) {
        const int m = std::min(kChunk, n - start);
        const double* x = psi + start;
        Curve* c = out + start;
        // Saturated (ψ ≤ 1e-12, or not a number): the default curve, unit values and zero
        // slopes. Those elements take a benign argument through the transcendental passes
        // below and are left at the default by the fill loops.
        bool wet[kChunk];
        double xs[kChunk];
        for (int i = 0; i < m; ++i) {
            c[i] = Curve{};
            wet[i] = !(x[i] > kPsiEps);
            xs[i] = wet[i] ? 1.0 : x[i];
        }
        switch (p.law) {
        case SoilChar::GARDNER: {
            // Se = Kr = e^(−αψ): one exponential carries both values and both slopes.
            double e[kChunk];
            for (int i = 0; i < m; ++i)
                e[i] = std::exp(-p.alpha * xs[i]);
            for (int i = 0; i < m; ++i) {
                if (wet[i])
                    continue;
                c[i].Se = clampSe(e[i]);
                c[i].dSe = e[i] > kSeMin ? -p.alpha * e[i] : 0.0;
                c[i].Kr = std::clamp(e[i], 0.0, 1.0);
                c[i].dKr = -p.alpha * e[i];
            }
            break;
        }
        case SoilChar::RUSSO: {
            // t = (1 + a)e^(−a), a = ½αψ: Se = t^(2/(2+L)), Kr = t², dt/dψ = −½α·a·e^(−a).
            const double expo = 2.0 / (2.0 + p.vg_L);
            double a[kChunk], e[kChunk], se[kChunk];
            for (int i = 0; i < m; ++i) {
                a[i] = 0.5 * p.alpha * xs[i];
                e[i] = std::exp(-a[i]);
            }
            for (int i = 0; i < m; ++i) {
                const double t = (1.0 + a[i]) * e[i];
                se[i] = std::pow(std::max(t, kSeMin), expo);
            }
            for (int i = 0; i < m; ++i) {
                if (wet[i])
                    continue;
                const double t = (1.0 + a[i]) * e[i];
                const double dt = -0.5 * p.alpha * a[i] * e[i];
                c[i].Se = clampSe(se[i]);
                c[i].dSe =
                    (t > kSeMin && se[i] > kSeMin && se[i] < 1.0) ? expo * se[i] / t * dt : 0.0;
                c[i].Kr = std::clamp(t * t, 0.0, 1.0);
                c[i].dKr = t * t < 1.0 ? 2.0 * t * dt : 0.0;
            }
            break;
        }
        case SoilChar::BROOKS_COREY: {
            const double pb = std::max(p.psi_b, kPsiEps);
            const double e = 2.0 + 3.0 * p.lambda;
            double se[kChunk], kr[kChunk];
            for (int i = 0; i < m; ++i)
                se[i] = std::pow(xs[i] > pb ? pb / xs[i] : 1.0, p.lambda);
            for (int i = 0; i < m; ++i)
                kr[i] = std::pow(xs[i] > pb ? pb / xs[i] : 1.0, e);
            for (int i = 0; i < m; ++i) {
                if (wet[i] || xs[i] < pb)
                    continue;
                if (xs[i] == pb) {
                    // The air-entry kink: a saturated layer sits exactly here (its start
                    // suction) and can only dry, so the slopes are the dry side's one-sided
                    // derivatives. The flat side's zeros leave a Newton with no storage term to
                    // move the layer.
                    c[i].dSe = -p.lambda / pb;
                    c[i].dKr = -(2.0 + 3.0 * p.lambda) / pb;
                    continue;
                }
                c[i].Se = clampSe(se[i]);
                c[i].dSe = se[i] > kSeMin ? -p.lambda * se[i] / xs[i] : 0.0;
                c[i].Kr = std::clamp(kr[i], 0.0, 1.0);
                c[i].dKr = kr[i] < 1.0 ? -e * kr[i] / xs[i] : 0.0;
            }
            break;
        }
        case SoilChar::VAN_GENUCHTEN: {
            const double n_ = std::max(p.vg_n, 1.0 + 1.0e-6);
            const double mm = 1.0 - 1.0 / n_;
            double ap[kChunk], se_raw[kChunk], g[kChunk], gm[kChunk], seL[kChunk];
            for (int i = 0; i < m; ++i)
                ap[i] = std::pow(p.alpha * xs[i], n_);
            for (int i = 0; i < m; ++i)
                se_raw[i] = std::pow(1.0 + ap[i], -mm);
            // Kr = Se^L (1 − (1 − Se^(1/m))^m)², with Se^(1/m) = 1/u off the floor, so
            // 1 − Se^(1/m) = ap/u and d(ap/u)/dψ = dap/u².
            for (int i = 0; i < m; ++i) {
                const bool open = se_raw[i] > kSeMin;
                g[i] = open ? ap[i] / (1.0 + ap[i]) : 1.0 - std::pow(clampSe(se_raw[i]), 1.0 / mm);
            }
            for (int i = 0; i < m; ++i)
                gm[i] = std::pow(std::max(g[i], 0.0), mm);
            for (int i = 0; i < m; ++i)
                seL[i] = std::pow(clampSe(se_raw[i]), p.vg_L);
            for (int i = 0; i < m; ++i) {
                if (wet[i])
                    continue;
                const double u = 1.0 + ap[i];
                const double se = clampSe(se_raw[i]);
                const bool open = se_raw[i] > kSeMin;  // not on the floor: the identities hold
                const double dap = n_ * ap[i] / xs[i]; // d(αψ)^n/dψ
                c[i].Se = se;
                c[i].dSe = open ? -mm * se_raw[i] / u * dap : 0.0;
                const double inner = 1.0 - gm[i];
                const double kr = seL[i] * inner * inner;
                c[i].Kr = std::clamp(kr, 0.0, 1.0);
                if (open && kr < 1.0 && g[i] > 0.0) {
                    const double dinner = -mm * gm[i] / g[i] * dap / (u * u);
                    const double dseL = p.vg_L * seL[i] / se * c[i].dSe;
                    c[i].dKr = dseL * inner * inner + 2.0 * seL[i] * inner * dinner;
                    // For n < 2 the exact slope is unbounded at ψ → 0 (Kr ≈ 1 − 2(αψ)^(n−1));
                    // the chord to saturation bounds it there. Elsewhere the curve is convex
                    // and the chord is shallower than the slope, so the bound stays inactive;
                    // for n ≥ 2 the slope is finite everywhere and the curve starts concave,
                    // where the chord would wrongly replace it.
                    if (n_ < 2.0)
                        c[i].dKr = std::max(c[i].dKr, (c[i].Kr - 1.0) / xs[i]);
                }
            }
            break;
        }
        }
    }
}

double suctionAtSaturation(const Params& p, double Se) noexcept {
    double psi;
    suctionAtSaturationBatch(p, &Se, 1, &psi);
    return psi;
}

void suctionAtSaturationBatch(const Params& p, const double* Se, int n, double* psi) noexcept {
    const double alpha = std::max(p.alpha, kPsiEps);
    for (int start = 0; start < n; start += kChunk) {
        const int m = std::min(kChunk, n - start);
        const double* x = Se + start;
        double* out = psi + start;
        // Saturated elements (Se within 1e-12 of 1) return 0 and take no part in the passes.
        bool todo[kChunk];
        double se[kChunk];
        for (int i = 0; i < m; ++i) {
            se[i] = clampSe(x[i]);
            todo[i] = se[i] < 1.0 - 1.0e-12;
            out[i] = 0.0;
        }
        switch (p.law) {
        case SoilChar::GARDNER:
            for (int i = 0; i < m; ++i)
                if (todo[i])
                    out[i] = -std::log(se[i]) / alpha;
            break;
        case SoilChar::RUSSO: {
            // (1 + a)·e^(−a) = t = se^((2+L)/2) has no elementary inverse. Its logarithm,
            //     g(a) = ln(1 + a) − a + ℓ = 0,   ℓ = −ln t = −½(2 + L)·ln se,
            // is monotone and concave on a ≥ 0, so Halley's method converges from either side
            // and cubically: from a start within a few percent two steps reach roundoff, one
            // log1p each. The start is the series inverse of ℓ = a²/2 − a³/3 + … near saturation
            // (a ≈ s + s²/3, s = √(2ℓ)) and the fixed point of a = ℓ + ln(1 + a) when dry (ℓ
            // beyond 2.5, where the series is 7 % high and the log form 6 % low). Before
            // 2026-11-26 this was Newton on the exponential form from √(2(1 − t)) or
            // ℓ + ln(1 + ℓ): a pow, a log, a log1p and three to five exp per inverse — the single
            // largest cost of the σ column once its Newton matrix no longer needed the soil
            // functions. The steps run interleaved across the elements, so each pass's log1p
            // calls are independent and pipeline.
            double ell[kChunk], a[kChunk];
            bool active[kChunk];
            for (int i = 0; i < m; ++i) {
                ell[i] = todo[i] ? -0.5 * (2.0 + p.vg_L) * std::log(se[i]) : 0.0;
                active[i] = todo[i];
            }
            for (int i = 0; i < m; ++i) {
                if (!todo[i])
                    continue;
                if (ell[i] < 2.5) {
                    const double s = std::sqrt(2.0 * ell[i]);
                    a[i] = s + s * s / 3.0;
                } else {
                    a[i] = ell[i] + std::log1p(ell[i]);
                }
            }
            for (int it = 0; it < 8; ++it) {
                bool any = false;
                for (int i = 0; i < m; ++i) {
                    if (!active[i])
                        continue;
                    const double g = std::log1p(a[i]) - a[i] + ell[i];
                    const double g1 = -a[i] / (1.0 + a[i]);
                    const double g2 = -1.0 / ((1.0 + a[i]) * (1.0 + a[i]));
                    const double denom = 2.0 * g1 * g1 - g * g2;
                    if (!(std::fabs(denom) > 1.0e-300)) {
                        active[i] = false;
                        continue;
                    }
                    const double step = 2.0 * g * g1 / denom;
                    a[i] -= step;
                    if (a[i] < 0.0)
                        a[i] = 0.0;
                    // The error after a Halley step of relative size δ is O(δ³): a step of
                    // 1e-5 leaves roundoff, so the next evaluation would change nothing.
                    if (std::fabs(step) <= 1.0e-5 * a[i])
                        active[i] = false;
                    else
                        any = true;
                }
                if (!any)
                    break;
            }
            for (int i = 0; i < m; ++i)
                if (todo[i])
                    out[i] = 2.0 * a[i] / alpha;
            break;
        }
        case SoilChar::BROOKS_COREY: {
            const double pb = std::max(p.psi_b, kPsiEps);
            const double inv_lambda = -1.0 / std::max(p.lambda, 1.0e-6);
            for (int i = 0; i < m; ++i)
                if (todo[i])
                    out[i] = pb * std::pow(se[i], inv_lambda);
            break;
        }
        case SoilChar::VAN_GENUCHTEN: {
            const double n_ = std::max(p.vg_n, 1.0 + 1.0e-6);
            const double mm = 1.0 - 1.0 / n_;
            double t[kChunk];
            for (int i = 0; i < m; ++i)
                t[i] = todo[i] ? std::pow(se[i], -1.0 / mm) - 1.0 : 0.0;
            for (int i = 0; i < m; ++i)
                if (todo[i])
                    out[i] = std::pow(std::max(t[i], 0.0), 1.0 / n_) / alpha;
            break;
        }
        }
    }
}

double diffusivity(const Params& p, double psi) noexcept {
    // D(θ) = K(ψ)·dψ/dθ. dθ/dψ = (θ_s − θ_r)·dSe/dψ, evaluated by a centred
    // difference in ψ — the analytic derivatives differ per law and per
    // branch, and the column's diffusive term is optional and small (D-N3
    // defaults it OFF), so a second-order difference is the right cost.
    const double dpsi = std::max(1.0e-4, 1.0e-3 * std::fabs(psi));
    const double se_hi = effectiveSaturation(p, psi + dpsi);
    const double se_lo = effectiveSaturation(p, std::max(psi - dpsi, 0.0));
    const double dse = (se_hi - se_lo) / (psi + dpsi - std::max(psi - dpsi, 0.0));
    const double dtheta_dpsi = dse * (p.theta_s - p.theta_r);
    if (std::fabs(dtheta_dpsi) < 1.0e-12)
        return 0.0;
    return conductivity(p, psi) / std::fabs(dtheta_dpsi);
}

// ---------------------------------------------------------------------------
// Equilibrium storage and recharge
// ---------------------------------------------------------------------------

/**
 * @brief `∫₀ᴸ Se(ψ) dψ` — the SATURATION integral. Internal, because it is the scale eq. 22 is
 * written on, not a quantity anyone outside this file should hold. `equilibriumStorage` converts
 * it to metres of water, which is what every other `hu` in the engine means.
 *
 * @param[in] p Borrowed validated soil parameters in SI units.
 * @param[in] Lc Nonnegative finite integration thickness, m.
 * @return Integral of dimensionless effective saturation over suction, in m;
 * Gardner/Brooks-Corey use closed forms, other/default cases use four-point Gauss-Legendre.
 * @note This is not total metres of water until residual water and porosity span are applied. No
 * allocation/input mutation; no quadrature error estimate.
 */
static double saturationIntegral(const Params& p, double Lc) noexcept {
    switch (p.law) {
    case SoilChar::GARDNER:
        // ∫₀ᴸ e^(−αψ) dψ = (1 − e^(−αL))/α — exactly eq. 39's hᵤ*.
        return (1.0 - std::exp(-p.alpha * Lc)) / std::max(p.alpha, kPsiEps);
    case SoilChar::BROOKS_COREY: {
        // ∫₀ᴸ Se dψ = ψ_b + ∫_{ψ_b}^{L} (ψ_b/ψ)^λ dψ  (L > ψ_b)
        const double pb = std::max(p.psi_b, kPsiEps);
        if (Lc <= pb)
            return Lc;
        const double lam = std::max(p.lambda, 1.0e-6);
        if (std::fabs(lam - 1.0) < 1.0e-9)
            return pb + pb * std::log(Lc / pb);
        return pb + pb * (std::pow(Lc / pb, 1.0 - lam) - 1.0) / (1.0 - lam);
    }
    case SoilChar::RUSSO:
    case SoilChar::VAN_GENUCHTEN:
    default: {
        double acc = 0.0; // 4-point Gauss-Legendre on [0, L]
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
    const double Lc = std::max(L, 0.0);
    return p.theta_r * Lc + (p.theta_s - p.theta_r) * saturationIntegral(p, Lc);
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
        // Eq. 22's form, with the drainage sign: a column holding more than
        // hydrostatic storage drains down (+), a drier one draws water up.
        // The header records the factored identity.
        const double e = std::exp(-p.alpha * Lc);
        const double den = p.alpha * Lc * (1.0 - e);
        if (!(std::fabs(den) > 1.0e-300))
            return 0.0;
        const double q = p.Ks * (p.alpha * hu_se - (1.0 - e)) / den;
        return std::isfinite(q) ? q : 0.0;
    }

    // The generalised relaxation form (see the header's @warning — the rate
    // below is ours, the equilibrium is the law's):
    //     q₀ = C(L)·[hᵤ − hᵤ*(L)],   C(L) = K̄ / (L · hᵤ*(L)/L)  →  K̄/hᵤ*
    // where K̄ is the conductivity at the column's mean suction. Reducing to
    // Gardner: K̄ → Ks·e^(−αL/2) and hᵤ* → (1−e^(−αL))/α gives the same order
    // and the same limits, which is the consistency this rests on.
    // Relaxation on the SATURATION scale, so the rate constant means the
    // same thing it does for Gardner, then reported as a water flux.
    const double hu_eq_se = saturationIntegral(p, Lc);
    if (!(hu_eq_se > 1.0e-12))
        return 0.0;
    const double Kbar = conductivity(p, 0.5 * Lc);
    const double C = Kbar / hu_eq_se;
    const double q = C * (hu_se - hu_eq_se);
    return std::isfinite(q) ? q : 0.0;
}

double rechargeQ0AtLength(const Params& p, double L, double hu) noexcept {
    if (!(L > 0))
        return 0.;
    if (L >= kLMin)
        return rechargeQ0(p, L, hu);
    const double span = std::max(p.theta_s - p.theta_r, 1e-12);
    const double equilibrium =
        p.law == SoilChar::GARDNER ? -std::expm1(-p.alpha * L) / p.alpha : saturationIntegral(p, L);
    // Same rate floor as the legacy law, but never invent the water or
    // residual storage belonging to the missing part of a millimetre.
    const double rate = p.law == SoilChar::GARDNER
                            ? p.Ks / (kLMin * -std::expm1(-p.alpha * kLMin))
                            : conductivity(p, .5 * kLMin) / saturationIntegral(p, kLMin);
    const double disequilibrium = (hu - p.theta_r * L - span * equilibrium) / span;
    const double q = rate * disequilibrium;
    return std::isfinite(q) ? q : 0.;
}

double rechargeRate(const Params& p, double L) noexcept {
    if (!(L > 0.0))
        return 0.;
    const double Lc = std::max(L, kLMin);
    const double span = std::max(p.theta_s - p.theta_r, 1e-12);
    // The same rate coefficient rechargeQ0() and rechargeQ0AtLength() apply to the
    // saturation-scale disequilibrium, divided by the scale: dq₀/dhᵤ.
    const double rate = p.law == SoilChar::GARDNER
                            ? p.Ks / (Lc * -std::expm1(-p.alpha * Lc))
                            : conductivity(p, .5 * Lc) / saturationIntegral(p, Lc);
    const double C = rate / span;
    return std::isfinite(C) && C > 0. ? C : 0.;
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
    if (psi <= 0.0)
        return 1.0;
    if (psi >= pw)
        return 0.0;
    const double x = psi / pw;      // 0 wet → 1 wilting
    const double s = 1.0 - x;       // 1 wet → 0 wilting
    return s * s * (3.0 - 2.0 * s); // smoothstep
}

} // namespace openswmm::twoD::soil
