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
 * @file      SigmaColumn.cpp
 * @brief     G-steps 4 and 5 — see the header for the coordinate, the flux forms
 *        and the handover convention.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "SigmaColumn.hpp"
#include "../../core/RunoffProfile.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace openswmm::twoD::sigma {

namespace {

constexpr double kLMin = 1.0e-3; ///< column thickness floor (m)
constexpr double kTiny = 1.0e-300;

/**
 * @brief Address one layer of a caller-owned strided water-content array.
 * @param[in] t Required writable array spanning the requested layer.
 * @param[in] j Nonnegative layer index, within caller-provided extent.
 * @param[in] stride Positive element stride, in doubles.
 * @return Borrowed mutable reference to t[j*stride], invalid when its backing storage changes.
 * @note No bounds/overflow checks, allocation or retention beyond the returned reference.
 */
inline double& at(double* t, int j, int stride) noexcept {
    return t[static_cast<std::size_t>(j) * static_cast<std::size_t>(stride)];
}
/**
 * @brief Address one layer of a caller-owned strided water-content array.
 * @param[in] t Required readable array spanning the requested layer.
 * @param[in] j Nonnegative layer index, within caller-provided extent.
 * @param[in] stride Positive element stride, in doubles.
 * @return Water-content value by copy.
 * @note No bounds/overflow checks, allocation or retention beyond the returned reference.
 */
inline double at(const double* t, int j, int stride) noexcept {
    return t[static_cast<std::size_t>(j) * static_cast<std::size_t>(stride)];
}

/**
 * @brief Cascade layer water above `w_max` down and fill layers below `w_min` from above, the
 *        table supplying what the column cannot (step 6 of the sweep).
 *
 * @param[in,out] w Layer water depths, m; at least m entries.
 * @param[in] m Strictly positive layer count.
 * @param[in] w_max Saturated layer water, m.
 * @param[in] w_min Residual layer water, m.
 * @param[in,out] r Optional receipt; compression[j+1] and support[j+1] are ADDED to, so repeated
 * passes accumulate.
 * @param[out] overflow Water cascaded out of the bottom layer to the table, m.
 * @param[out] draw Water drawn up from the table, m.
 */
void settleLayerBounds(std::vector<double>& w, int m, double w_max, double w_min,
                       ColumnTransfers* r, double& overflow, double& draw) noexcept {
    double carry = 0.0;
    for (int j = 0; j < m; ++j) {
        double wj = w[static_cast<std::size_t>(j)] + carry;
        carry = 0.0;
        if (wj > w_max) {
            carry = wj - w_max;
            wj = w_max;
        }
        w[static_cast<std::size_t>(j)] = wj;
        if (r)
            r->compression[j + 1] += carry;
    }
    overflow = carry;

    static thread_local std::vector<double> before;
    if (r)
        before = w;
    draw = 0.0;
    for (int j = m - 1; j >= 0; --j) {
        double wj = w[static_cast<std::size_t>(j)] - draw;
        draw = 0.0;
        if (wj < w_min) {
            draw = w_min - wj;
            wj = w_min;
        }
        w[static_cast<std::size_t>(j)] = wj;
    }
    if (r) {
        double flux = 0;
        for (int j = 0; j < m; ++j) {
            flux -= w[j] - before[j];
            r->support[j + 1] += flux;
        }
    }
}

/**
 * @brief Suction derivative of a soil function by a centered difference.
 *
 * Used only for the Newton matrix: the residual is evaluated with the exact functions, so the
 * difference error changes how fast the solve converges, never what it converges to.
 */
template <class F> double suctionSlope(F f, double psi) noexcept {
    const double h = 1.0e-6 * std::max(std::fabs(psi), 1.0e-2);
    return (f(psi + h) - f(psi - h)) / (2.0 * h);
}

/// Boundary rates of one S2 solve (m/s): the fixed top flux (accepted infiltration less ET)
/// and the lower bound on the solved table flux.
struct ImplicitForcing {
    double top = 0.0, q0_min = -1.0e300;
};

/// Work of the S2 solves of one step.
struct ImplicitTotals {
    int solves = 0, iterations = 0;
};

constexpr int kNewtonIterations = 40;
constexpr int kLineSearchHalvings = 12;
constexpr int kPolishSteps = 2;
constexpr int kSubstepHalvings = 6;
/// Residual tolerance relative to the water and transfers of each layer (~450 ulps).
constexpr double kResidualTolerance = 1.0e-13;
/// Largest suction the fluxes see (m), ~10 MPa, past any wilting point. Power-law retention
/// reaches its numerical floor only at 1e19 m, where an arithmetic-mean face would carry
/// 1e12 m/s into the layer.
constexpr double kSuctionCap = 1.0e3;
/// Layers thinner than this (m) also try a start from rest (implicitSolve).
constexpr double kThinLayer = 1.0e-3;
/// Below this effective saturation a layer's Newton unknown is its saturation, above it its
/// suction (Forsyth, Wu & Pruess 1995). Suction alone fails to wet a dry layer: retention is
/// flat at its floor, so a suction step there is unbounded.
constexpr double kSwitchSaturation = 0.5;

/**
 * @brief One backward-Euler step of the physical fluxes on fixed layers (S2).
 *
 * Mixed form (Celia, Bouloutas & Zarba 1990), solved by Newton with a line search on the
 * residual norm:
 *
 *     R_j = theta_j dz - w_j - h (P_j - P_{j+1})
 *     P_j     = (K_{j-1} + K_j)/2 (1 + (psi_j - psi_{j-1})/dz)       interior, down-positive
 *     P_m     = max((K_{m-1} + Ks)/2 (1 - psi_{m-1}/(dz/2)), q0_min)  to psi = 0 at the table
 *     P_0     = the fixed top flux (accepted infiltration less ET)
 *
 * The table is a node at psi = 0 with K = Ks, so every face takes the same arithmetic mean; with
 * it the faces are monotone in suction and backward Euler has one root. Every face vanishes on a
 * hydrostatic profile. Each layer's unknown is its effective saturation when dry and its suction when wet
 * (kSwitchSaturation); fluxes see suction capped at kSuctionCap. A layer at zero saturation
 * whose residual still asks for less water is held there (projected Newton). After convergence
 * the new water is taken from the final fluxes, so water is conserved to roundoff whatever the
 * residual; up to two more Newton steps polish the state to roundoff, which keeps the table
 * settlement (a root find over trial table heights) continuous at its own tolerance.
 *
 * @param[in,out] w Layer water, m: start of the step in, end of the step out.
 * @param[in,out] faces Physical face transfers, m, added to (m+1 entries).
 * @param[in,out] totals Solve and iteration counts, added to.
 * @return Whether the residual converged. The step is applied either way.
 */
bool implicitSolve(const soil::Params& p, int m, double dz, double h, const ImplicitForcing& f,
                   std::vector<double>& w, std::vector<double>& faces,
                   ImplicitTotals& totals) noexcept {
    struct Work {
        std::vector<double> sat, psi, trial_sat, trial_psi, ph, k, flux, res, store, dk, chain, up,
            dn, diag, lower, upper, rhs, cp, step;
        std::vector<char> by_suction;
    };
    static thread_local Work s;
    const auto n = static_cast<std::size_t>(m);
    for (auto* v : {&s.sat, &s.psi, &s.trial_sat, &s.trial_psi, &s.ph, &s.k, &s.res, &s.store,
                    &s.dk, &s.chain, &s.diag, &s.lower, &s.upper, &s.rhs, &s.cp, &s.step})
        v->resize(n);
    for (auto* v : {&s.flux, &s.up, &s.dn})
        v->resize(n + 1);
    s.by_suction.resize(n);

    const double span = std::max(p.theta_s - p.theta_r, 1.0e-9);
    const double entry = p.law == SoilChar::BROOKS_COREY ? p.psi_b : 0.0;
    for (int j = 0; j < m; ++j) {
        const double se = std::max((w[j] / dz - p.theta_r) / span, 0.0);
        if (se >= 1.0 - 1.0e-12) {
            // A saturated layer starts at the suction nearest rest it can hold: air entry, or
            // its height above the table when that is less.
            s.sat[j] = 1.0;
            s.psi[j] = std::min(entry, dz * (static_cast<double>(m - j) - 0.5));
        } else {
            s.sat[j] = se;
            s.psi[j] = soil::suctionAtSaturation(p, se);
        }
    }

    bool converged = false;
    // The table face's bound is held on one branch through a Newton pass, chosen from the
    // starting state and flipped (once) if the converged state contradicts it: max(Darcy,
    // q0_min) has its kink exactly at equilibrium when the table has nothing to supply, and
    // switching branch every iterate there cycles.
    bool clamped = false;
    double darcy = 0.0;
    const auto held = [&](const std::vector<double>& sat, int j) {
        return sat[j] <= 0.0 && s.res[j] > 0.0;
    };
    const auto evaluate = [&](const std::vector<double>& sat, const std::vector<double>& psi) {
        for (int j = 0; j < m; ++j) {
            s.ph[j] = std::min(psi[j], kSuctionCap);
            s.k[j] = soil::conductivity(p, s.ph[j]);
        }
        s.flux[0] = f.top;
        for (int j = 1; j < m; ++j)
            s.flux[j] = 0.5 * (s.k[j - 1] + s.k[j]) * (1.0 + (s.ph[j] - s.ph[j - 1]) / dz);
        darcy = 0.5 * (s.k[m - 1] + p.Ks) * (1.0 - 2.0 * s.ph[m - 1] / dz);
        s.flux[n] = clamped ? f.q0_min : darcy;
        for (int j = 0; j < m; ++j)
            s.res[j] = (p.theta_r + span * sat[j]) * dz - w[j] - h * (s.flux[j] - s.flux[j + 1]);
        double sum = 0.0;
        converged = true;
        for (int j = 0; j < m; ++j) {
            if (held(sat, j))
                continue;
            sum += s.res[j] * s.res[j];
            const double scale = (p.theta_r + span * sat[j]) * dz + std::fabs(w[j]) +
                                 h * (std::fabs(s.flux[j]) + std::fabs(s.flux[j + 1]));
            if (!(std::fabs(s.res[j]) <= kResidualTolerance * scale))
                converged = false;
        }
        return sum;
    };
    // Newton step from the state the last evaluate() left; false if the matrix is singular.
    const auto newtonStep = [&]() {
        for (int j = 0; j < m; ++j) {
            const bool free = s.psi[j] < kSuctionCap; // the fluxes see this layer's suction
            s.dk[j] =
                free ? suctionSlope([&](double x) { return soil::conductivity(p, x); }, s.ph[j])
                     : 0.0;
            s.by_suction[j] = s.sat[j] >= kSwitchSaturation;
            if (s.by_suction[j]) {
                s.store[j] =
                    suctionSlope([&](double x) { return soil::waterContent(p, x); }, s.psi[j]) * dz;
                s.chain[j] = free ? 1.0 : 0.0;
            } else {
                // d psi / d Se by a relative difference of the inverse.
                const double se = std::max(s.sat[j], 1.0e-8), d = 1.0e-4 * se;
                s.store[j] = span * dz;
                s.chain[j] = free ? (soil::suctionAtSaturation(p, se + d) -
                                     soil::suctionAtSaturation(p, se - d)) /
                                        (2.0 * d)
                                  : 0.0;
            }
        }
        s.dn[0] = 0.0; // the top flux is fixed
        for (int j = 1; j < m; ++j) {
            const double g = 1.0 + (s.ph[j] - s.ph[j - 1]) / dz;
            const double kf = 0.5 * (s.k[j - 1] + s.k[j]);
            s.up[j] = 0.5 * s.dk[j - 1] * g - kf / dz; // dP_j/dpsi_{j-1}
            s.dn[j] = 0.5 * s.dk[j] * g + kf / dz;     // dP_j/dpsi_j
        }
        const double kb = 0.5 * (s.k[m - 1] + p.Ks);
        s.up[n] =
            clamped ? 0.0 : 0.5 * s.dk[m - 1] * (1.0 - 2.0 * s.ph[m - 1] / dz) - 2.0 * kb / dz;
        for (int j = 0; j < m; ++j) {
            s.diag[j] = s.store[j] - h * (s.dn[j] - s.up[j + 1]) * s.chain[j];
            s.lower[j] = j > 0 ? -h * s.up[j] * s.chain[j - 1] : 0.0;
            s.upper[j] = j < m - 1 ? h * s.dn[j + 1] * s.chain[j + 1] : 0.0;
            s.rhs[j] = -s.res[j];
            if (held(s.sat, j)) {
                s.diag[j] = 1.0;
                s.lower[j] = s.upper[j] = s.rhs[j] = 0.0;
            }
        }
        // Thomas algorithm for J step = -R (held layers do not move).
        double beta = s.diag[0];
        if (!(std::fabs(beta) > kTiny) || !std::isfinite(beta))
            return false;
        s.cp[0] = s.upper[0] / beta;
        s.step[0] = s.rhs[0] / beta;
        for (int j = 1; j < m; ++j) {
            beta = s.diag[j] - s.lower[j] * s.cp[j - 1];
            if (!(std::fabs(beta) > kTiny) || !std::isfinite(beta))
                return false;
            s.cp[j] = s.upper[j] / beta;
            s.step[j] = (s.rhs[j] - s.lower[j] * s.step[j - 1]) / beta;
        }
        for (int j = m - 2; j >= 0; --j)
            s.step[j] -= s.cp[j] * s.step[j + 1];
        for (int j = 0; j < m; ++j)
            if (!std::isfinite(s.step[j]))
                return false;
        return true;
    };
    // The state lambda along the last Newton step, in each layer's own unknown.
    const auto advance = [&](double lambda) {
        for (int j = 0; j < m; ++j) {
            if (s.by_suction[j]) {
                s.trial_psi[j] = s.psi[j] + lambda * s.step[j];
                s.trial_sat[j] = soil::effectiveSaturation(p, s.trial_psi[j]);
            } else {
                const double se = s.sat[j] + lambda * s.step[j];
                if (se >= 1.0) {
                    s.trial_sat[j] = 1.0;
                    s.trial_psi[j] = entry;
                } else {
                    s.trial_sat[j] = std::max(se, 0.0);
                    s.trial_psi[j] = soil::suctionAtSaturation(p, s.trial_sat[j]);
                }
            }
        }
    };

    double merit = evaluate(s.sat, s.psi);
    if (!converged && dz < kThinLayer) {
        // In a column squeezed to a sliver the stored contents carry suction differences far
        // larger than its layers, and the faces would carry them as huge fluxes; the solution
        // there is near rest, so start from rest when that fits better.
        for (int j = 0; j < m; ++j) {
            s.trial_psi[j] = dz * (static_cast<double>(m - j) - 0.5);
            s.trial_sat[j] = soil::effectiveSaturation(p, s.trial_psi[j]);
        }
        const double rest = evaluate(s.trial_sat, s.trial_psi);
        if (rest < merit) {
            s.sat.swap(s.trial_sat);
            s.psi.swap(s.trial_psi);
            merit = rest;
        } else {
            merit = evaluate(s.sat, s.psi);
        }
    }
    if (darcy < f.q0_min) {
        clamped = true;
        merit = evaluate(s.sat, s.psi);
    }
    bool ok = std::isfinite(merit);
    for (int pass = 0; ok && pass < 2; ++pass) {
        int polished = 0;
        for (int it = 0; ok && it < kNewtonIterations; ++it) {
            ++totals.iterations;
            const bool polishing = converged;
            if (polishing && polished++ >= kPolishSteps)
                break;
            if (!newtonStep()) {
                ok = converged;
                break;
            }
            bool moved = false;
            double lambda = 1.0;
            for (int ls = 0; ls <= kLineSearchHalvings; ++ls, lambda *= 0.5) {
                advance(lambda);
                const double next = evaluate(s.trial_sat, s.trial_psi);
                if (next < merit) {
                    s.sat.swap(s.trial_sat);
                    s.psi.swap(s.trial_psi);
                    merit = next;
                    moved = true;
                    OPENSWMM_RUNOFF_PROFILE_COUNT_N(sigma_linesearch_halvings, ls);
                    break;
                }
                if (polishing)
                    break; // at roundoff: only a full step that still helps
            }
            if (!moved) {
                merit = evaluate(s.sat, s.psi); // restore the fluxes of the kept state
                ok = converged;
                break;
            }
        }
        ok = ok && converged;
        const double margin = 1.0e-12 * p.Ks;
        const bool consistent = clamped ? darcy <= f.q0_min + margin : darcy >= f.q0_min - margin;
        if (!ok || consistent)
            break;
        clamped = !clamped; // the other branch, once
        merit = evaluate(s.sat, s.psi);
        ok = pass == 0 && std::isfinite(merit);
    }
    if (ok && !clamped && darcy < f.q0_min - 1.0e-12 * p.Ks)
        ok = false; // the bound would be broken

    for (int j = 0; j <= m; ++j)
        faces[j] += h * s.flux[j];
    for (int j = 0; j < m; ++j)
        w[j] += h * (s.flux[j] - s.flux[j + 1]);
    ++totals.solves;
    return ok;
}

} // namespace

// ---------------------------------------------------------------------------

double columnStorage(const double* theta, int m, int stride, double L) noexcept {
    const double dz = std::max(L, 0.0) / static_cast<double>(m);
    double s = 0.0;
    for (int j = 0; j < m; ++j)
        s += at(theta, j, stride) * dz;
    return s;
}

void seedHydrostatic(const soil::Params& p, double* theta, int m, int stride, double L) noexcept {
    const double Lc = std::max(L, 0.0);
    const double dsig = 1.0 / static_cast<double>(m);
    for (int j = 0; j < m; ++j) {
        // Layer centre: σ = (j + ½)/m, and ψ is the height ABOVE the table,
        // so ψ = L·(1 − σ). Layer 0 is at the surface (largest suction).
        const double sig = (static_cast<double>(j) + 0.5) * dsig;
        const double psi = Lc * (1.0 - sig);
        at(theta, j, stride) = soil::waterContent(p, psi);
    }
}

double columnDtLimit(const soil::Params& p, const double* theta, int m, int stride, double L,
                     double Ldot, double c_col) noexcept {
    const double Lc = std::max(L, kLMin);
    const double dsig = 1.0 / static_cast<double>(m);
    const double dz = Lc * dsig;
    double lim = 1.0e30;
    for (int j = 0; j < m; ++j) {
        const double th = std::clamp(at(theta, j, stride), p.theta_r, p.theta_s);
        // Celerity c(θ) = dK/dθ, by a centred difference through the
        // retention inverse: the analytic form differs per law and per
        // branch, and this runs once per rebuild, not per firing.
        const double span = std::max(p.theta_s - p.theta_r, 1.0e-9);
        const double se = std::clamp((th - p.theta_r) / span, 1.0e-8, 1.0);
        const double dse = 1.0e-3;
        const double se_hi = std::min(se + dse, 1.0);
        const double se_lo = std::max(se - dse, 1.0e-8);
        const double k_hi = soil::conductivity(p, soil::suctionAtSaturation(p, se_hi));
        const double k_lo = soil::conductivity(p, soil::suctionAtSaturation(p, se_lo));
        const double c = std::fabs(k_hi - k_lo) / std::max((se_hi - se_lo) * span, 1.0e-12);

        const double sig = (static_cast<double>(j) + 0.5) * dsig;
        const double grid = std::fabs(sig * Ldot);
        const double denom = c + grid;
        if (denom > kTiny)
            lim = std::min(lim, dz / denom);
    }
    return std::max(c_col, 1.0e-6) * lim;
}

double tableFlux(const soil::Params& p, const double* theta, int m, int stride, double L,
                 bool capillary) noexcept {
    const double span = std::max(p.theta_s - p.theta_r, 1.0e-9);
    const double th_bot = std::clamp(at(theta, m - 1, stride), p.theta_r, p.theta_s);
    // The 1e-6 floor is the one the free-drainage flux has always used.
    double psi = soil::suctionAtSaturation(p, std::clamp((th_bot - p.theta_r) / span, 1.0e-6, 1.0));
    const double k = soil::conductivity(p, psi);
    if (!capillary)
        return k; // kinematic column: free drainage
    // Darcy over the half layer from the bottom layer center to psi = 0 at the table, with the
    // implicit column's face conductivity (the table a node at Ks).
    const double half = std::max(0.5 * std::max(L, 0.0) / static_cast<double>(m), 1.0e-12);
    // A saturated Brooks-Corey layer may hold any suction up to air entry; take the one nearest
    // rest, so a capillary fringe seeded hydrostatic stays put.
    if (p.law == SoilChar::BROOKS_COREY && psi < p.psi_b)
        psi = std::min(p.psi_b, half);
    return 0.5 * (k + p.Ks) * (1.0 - psi / half);
}

// ---------------------------------------------------------------------------

void prepareColumn(const soil::Params& p, const double* theta, int m, int stride,
                   const ColumnStep& st, ColumnPreparation& prepared) noexcept {
    prepared.psi.resize(m);
    prepared.conductivity.resize(m);
    prepared.celerity.resize(m);
    const double dsig = 1.0 / static_cast<double>(m);
    const double dz = std::max(std::max(st.L_old, 0.0), kLMin) * dsig;
    for (int j = 0; j < m; ++j) {
        const double th = std::clamp(at(theta, j, stride), p.theta_r, p.theta_s);
        const double span = std::max(p.theta_s - p.theta_r, 1.0e-9);
        const double se = std::clamp((th - p.theta_r) / span, 1.0e-8, 1.0);
        const double dse = 1.0e-3;
        const double se_hi = std::min(se + dse, 1.0);
        const double se_lo = std::max(se - dse, 1.0e-8);
        const double k_hi = soil::conductivity(p, soil::suctionAtSaturation(p, se_hi));
        const double k_lo = soil::conductivity(p, soil::suctionAtSaturation(p, se_lo));
        prepared.celerity[j] = std::fabs(k_hi - k_lo) / std::max((se_hi - se_lo) * span, 1.0e-12);
        prepared.psi[j] = soil::suctionAtSaturation(p, se);
        prepared.conductivity[j] = soil::conductivity(p, prepared.psi[j]);
    }
    prepared.fixed_velocity = st.forward_euler;
    if (prepared.fixed_velocity) {
        double lim = 1.0e30;
        for (int j = 0; j < m; ++j) {
            const double sig = (static_cast<double>(j) + 0.5) * dsig;
            const double grid = std::fabs(sig * st.grid_rate);
            const double denom = prepared.celerity[j] + grid;
            if (denom > kTiny)
                lim = std::min(lim, dz / denom);
        }
        prepared.dt_limit = std::max(1.0, 1.0e-6) * lim;
    }
}

/**
 * @brief S2: one step of a capillary column.
 *
 * The top boundary is the kinematic column's, fixed before the solve: infiltration accepted up
 * to the top layer's room (the surface's published capacity is already capped by it, so what
 * the surface delivers is stored) and ET at the top layer's old suction, at most its water above
 * residual. The moving layers then carry their old water explicitly (the ALE remap of the
 * explicit sweep: interior faces upwinded on their own sign, the table face handing over its
 * paired content), layer contacts are added and the bound pass settles any layer the remap over-
 * or under-filled. The physical fluxes advance by backward Euler on the new layers
 * (implicitSolve), halving the step inside this call until the solve converges, and a final
 * bound pass absorbs roundoff. Receipts hold remap plus physical transfers per face.
 *
 * @param[in] w Old layer water on the old geometry, m (m entries).
 * @param[in] th_old Old layer contents clamped to [theta_r, theta_s] (m entries).
 * @param[in] psi_top The top layer's old suction, m.
 */
static void advanceImplicitColumn(const soil::Params& p, double* theta, int m, int stride,
                                  ColumnStep& st, double L0, double L1, double Ldot,
                                  const std::vector<double>& w, const std::vector<double>& th_old,
                                  double psi_top) noexcept {
    const double dsig = 1.0 / static_cast<double>(m);
    const double dz0 = L0 * dsig, dz1 = L1 * dsig;
    {
        const double headroom = std::max(p.theta_s - th_old[0], 0.0) * dz0 / st.dt;
        st.f_top = std::min(std::max(st.q_in, 0.0), headroom);
        st.rejected = std::max(st.q_in, 0.0) - st.f_top;
        const double avail = std::max(th_old[0] - p.theta_r, 0.0) * dz0 / st.dt;
        st.et_stress = soil::feddesStress(psi_top, st.wilting_suction);
        st.et_taken = std::min(std::max(st.q_et, 0.0) * st.et_stress, avail);
    }
    const auto n = static_cast<std::size_t>(m);
    static thread_local std::vector<double> grid, w_start, w_end, faces;
    grid.assign(n + 1, 0.0);
    for (int jf = 1; jf < m; ++jf) {
        const double zdot = static_cast<double>(jf) * dsig * Ldot;
        grid[jf] = -(zdot > 0.0 ? th_old[jf] : th_old[jf - 1]) * zdot;
    }
    grid[n] = -(st.handover_theta >= 0.0 ? st.handover_theta : th_old[n - 1]) * Ldot;
    w_start.assign(n, 0.0);
    for (int j = 0; j < m; ++j)
        w_start[j] = w[j] + st.dt * (grid[j] - grid[j + 1]) +
                     (st.layer_inflow ? (*st.layer_inflow)[static_cast<std::size_t>(j)] : 0.0);
    double overflow = 0.0, draw = 0.0;
    settleLayerBounds(w_start, m, p.theta_s * dz1, p.theta_r * dz1, st.transfers, overflow, draw);

    ImplicitForcing forcing;
    forcing.top = st.f_top - st.et_taken;
    forcing.q0_min = st.q0_min;
    ImplicitTotals totals;
    int solves = 0, iterations = 0;
    if (dz1 > 0.0) {
#if defined(OPENSWMM_RUNOFF_PROFILE)
        int prof_level = -1; // the halving level that converged; -1 = none did
#endif
        for (int level = 0; level <= kSubstepHalvings; ++level) {
            const int pieces = 1 << level;
            const double h = st.dt / static_cast<double>(pieces);
            w_end = w_start;
            faces.assign(n + 1, 0.0);
            solves += totals.solves;
            iterations += totals.iterations;
            totals = {};
            bool ok = true;
            for (int k = 0; k < pieces; ++k)
                if (!implicitSolve(p, m, dz1, h, forcing, w_end, faces, totals)) {
                    ok = false;
                    if (level < kSubstepHalvings)
                        break;
                }
            if (ok) {
#if defined(OPENSWMM_RUNOFF_PROFILE)
                prof_level = level;
#endif
                break;
            }
        }
#if defined(OPENSWMM_RUNOFF_PROFILE)
        if (prof_level < 0)
            OPENSWMM_RUNOFF_PROFILE_COUNT(sigma_column_failed);
        else if (prof_level > 0)
            OPENSWMM_RUNOFF_PROFILE_COUNT(sigma_substep_halved);
#endif
    } else {
        // A collapsed column passes its top flux straight to the table: the limit of the solve
        // as its layers vanish.
        w_end = w_start;
        faces.assign(n + 1, std::max(forcing.top, forcing.q0_min) * st.dt);
        faces[0] = forcing.top * st.dt;
        for (int j = 0; j < m; ++j)
            w_end[j] += faces[j] - faces[j + 1];
    }

    double overflow_end = 0.0, draw_end = 0.0;
    settleLayerBounds(w_end, m, p.theta_s * dz1, p.theta_r * dz1, st.transfers, overflow_end,
                      draw_end);
    st.table_flux = faces[n] / st.dt;
    st.f_bot = st.table_flux + grid[n];
    st.overflow_to_sat = (overflow + overflow_end) / st.dt;
    st.deficit_from_sat = (draw + draw_end) / st.dt;
    st.implicit_solves = solves + totals.solves;
    st.newton_iterations = iterations + totals.iterations;
    OPENSWMM_RUNOFF_PROFILE_COUNT_N(sigma_solves, st.implicit_solves);
    OPENSWMM_RUNOFF_PROFILE_COUNT_N(sigma_newton_iter, st.newton_iterations);
    if (st.transfers) {
        auto& r = *st.transfers;
        r.infiltration = st.f_top * st.dt;
        r.evaporation = st.et_taken * st.dt;
        for (int j = 0; j <= m; ++j)
            r.faces[j] = faces[j] + st.dt * grid[j];
        r.new_water = w_end;
    }
    for (int j = 0; j < m; ++j)
        at(theta, j, stride) = dz1 > 0.0 ? w_end[static_cast<std::size_t>(j)] / dz1 : p.theta_s;
    st.theta_bot = at(theta, m - 1, stride);
}

static void advancePreparedColumn(const soil::Params& p, double* theta, int m, int stride,
                                  ColumnStep& st, const ColumnPreparation* prepared) noexcept {
    if (m <= 0 || !(st.dt > 0.0))
        return;

    const double dsig = 1.0 / static_cast<double>(m);
    const double L0 = std::max(st.L_old, 0.0);
    const double L1 = std::max(st.L_new, 0.0);
    const double Ldot = st.forward_euler ? st.grid_rate : (L1 - L0) / st.dt;

    if (!prepared) {
        st.dt_limit = columnDtLimit(p, theta, m, stride, L0, Ldot, 1.0);
    } else if (prepared->fixed_velocity) {
        st.dt_limit = prepared->dt_limit;
    } else {
        // Non-Euler trial heights change Ldot; only the soil terms are invariant.
        const double dz = std::max(L0, kLMin) * dsig;
        double lim = 1.0e30;
        for (int j = 0; j < m; ++j) {
            const double sig = (static_cast<double>(j) + 0.5) * dsig;
            const double grid = std::fabs(sig * Ldot);
            const double denom = prepared->celerity[j] + grid;
            if (denom > kTiny)
                lim = std::min(lim, dz / denom);
        }
        st.dt_limit = std::max(1.0, 1.0e-6) * lim;
    }

    // ---- 1. w_j from the OLD geometry. w is the conserved quantity; θ is a
    //         derived view of it, and it is re-derived from the NEW L at the
    //         end. That ordering is the whole ALE bookkeeping.
    static thread_local std::vector<double> w, w_new, th_old, f;
    w.assign(static_cast<std::size_t>(m), 0.0);
    th_old.assign(static_cast<std::size_t>(m), 0.0);
    f.assign(static_cast<std::size_t>(m) + 1, 0.0);
    const double dz0 = L0 * dsig;
    for (int j = 0; j < m; ++j) {
        th_old[static_cast<std::size_t>(j)] =
            std::clamp(at(theta, j, stride), p.theta_r, p.theta_s);
        w[static_cast<std::size_t>(j)] = th_old[static_cast<std::size_t>(j)] * dz0;
    }

    if (st.transfers) {
        auto& r = *st.transfers;
        r = {};
        r.old_length = L0;
        r.new_length = L1;
        r.contacts = st.layer_inflow ? *st.layer_inflow : std::vector<double>(m, 0);
        r.old_water = w;
        r.faces.resize(m + 1);
        r.compression.assign(m + 1, 0);
        r.support.assign(m + 1, 0);
        r.grid.resize(m + 1);
        for (int j = 0; j <= m; ++j)
            r.grid[j] =
                st.forward_euler ? -double(j) * dsig * Ldot * st.dt : -double(j) * dsig * (L1 - L0);
    }

    auto psiOf = [&](double th) {
        const double span = std::max(p.theta_s - p.theta_r, 1.0e-9);
        const double se = std::clamp((th - p.theta_r) / span, 1.0e-8, 1.0);
        return soil::suctionAtSaturation(p, se);
    };
    if (st.capillary) {
        advanceImplicitColumn(p, theta, m, stride, st, L0, L1, Ldot, w, th_old,
                              prepared ? prepared->psi[0] : psiOf(th_old[0]));
        return;
    }
    st.table_flux = st.q0_phys;

    // ---- 2. Interior faces. f[j] is the face ABOVE layer j (σ = j/m), so
    //         f[0] is the top boundary and f[m] the bottom.
    for (int jf = 1; jf < m; ++jf) {
        const int up = jf - 1; // layer above the face
        const int dn = jf;     // layer below
        const double th_up = th_old[static_cast<std::size_t>(up)];
        const double th_dn = th_old[static_cast<std::size_t>(dn)];
        const double sig_f = static_cast<double>(jf) * dsig;

        // (a) gravity — downward, donor is the layer above.
        double flux = prepared ? prepared->conductivity[up] : soil::conductivity(p, psiOf(th_up));

        // (b) grid motion, upwinded on its own sign. The face moves at
        //     ż = σ_f·L̇; the relative flux is −θ_donor·ż, so L̇ > 0 (faces
        //     sweeping down into the column) takes the layer BELOW.
        const double zdot = sig_f * Ldot;
        const double th_grid = (zdot > 0.0) ? th_dn : th_up;
        flux -= th_grid * zdot;
        f[static_cast<std::size_t>(jf)] = flux;
    }

    // ---- 3. Top boundary: infiltration in, ET out, with acceptance.
    //         The top layer can hold at most (θ_s − θ_top)·dz more water;
    //         anything beyond that is rejected to the surface.
    {
        const double th_top = th_old[0];
        const double headroom = std::max(p.theta_s - th_top, 0.0) * dz0 / st.dt;
        const double accept = std::min(std::max(st.q_in, 0.0), headroom);
        st.f_top = accept;
        st.rejected = std::max(st.q_in, 0.0) - accept;

        // ET is limited by what the top layer holds above residual, and by
        // the Feddes stress at its own suction — no `if (infil <= 0)` gate.
        const double avail = std::max(th_top - p.theta_r, 0.0) * dz0 / st.dt;
        const double psi_top = prepared ? prepared->psi[0] : psiOf(th_top);
        // The reviewed wilting suction is a shared SI model parameter.
        const double psi_w = st.wilting_suction;
        const double stress = soil::feddesStress(psi_top, psi_w);
        st.et_stress = stress;
        st.et_taken = std::min(std::max(st.q_et, 0.0) * stress, avail);
        f[0] = st.f_top - st.et_taken;
    }

    // ---- 4. Bottom boundary: physical Darcy across the table PLUS the
    //         handover swept by the moving boundary (σ = 1, ż = L̇).
    {
        const double th_bot = th_old[static_cast<std::size_t>(m - 1)];
        st.f_bot = st.q0_phys - (st.handover_theta >= 0.0 ? st.handover_theta : th_bot) * Ldot;
        f[static_cast<std::size_t>(m)] = st.f_bot;
    }

    // ---- 5. Positivity share. A layer may not export more than it holds
    //         above residual over this step; when its outgoing faces
    //         over-subscribe it they are scaled together, exactly like the
    //         surface solver's volume share. Applied to interior faces and
    //         the bottom face (the top face is already capped above).
    for (int j = 0; j < m; ++j) {
        const double avail = std::max(w[static_cast<std::size_t>(j)] - p.theta_r * dz0, 0.0);
        double out = 0.0;
        if (f[static_cast<std::size_t>(j)] < 0.0)
            out += -f[static_cast<std::size_t>(j)] * st.dt; // upward out of j
        if (f[static_cast<std::size_t>(j + 1)] > 0.0)
            out += f[static_cast<std::size_t>(j + 1)] * st.dt; // downward out of j
        if (out > avail && out > kTiny) {
            const double beta = avail / out;
            if (f[static_cast<std::size_t>(j)] < 0.0)
                f[static_cast<std::size_t>(j)] *= beta;
            if (f[static_cast<std::size_t>(j + 1)] > 0.0) {
                f[static_cast<std::size_t>(j + 1)] *= beta;
                if (j == m - 1)
                    st.f_bot = f[static_cast<std::size_t>(m)];
            }
        }
    }

    // Positivity sharing may reduce the net top export; report actual ET.
    st.et_taken = std::max(0.0, st.f_top - f[0]);
    if (st.transfers) {
        auto& r = *st.transfers;
        r.infiltration = st.f_top * st.dt;
        r.evaporation = st.et_taken * st.dt;
        for (int j = 0; j <= m; ++j)
            r.faces[j] = f[j] * st.dt;
    }

    // ---- 6. Update w, then re-derive θ from the NEW geometry. This is where
    //         the layers compress: same water, new dz.
    //
    //   A rising table (L shrinking) compresses every layer. A layer whose
    //   water then exceeds θ_s·dz_new is SATURATED, and the surplus belongs
    //   to the saturated zone — it must not be clamped away. Clamping it was
    //   the first version of this function and it lost 3.9e-1 m of water on
    //   the closed-column oscillation gate: the classic σ-grid failure the
    //   plan names in its risks. So the surplus cascades DOWNWARD layer by
    //   layer (each full layer pushes into the one below) and whatever
    //   reaches the bottom is reported as `overflow_to_sat`, which the
    //   solver adds to the saturated zone in this same firing. A deficit
    //   below residual is drawn the same way, reported as
    //   `deficit_from_sat`. With both routed, Σw is conserved exactly and
    //   the gate passes to machine precision.
    const double dz1 = L1 * dsig;
    w_new.assign(static_cast<std::size_t>(m), 0.0);
    for (int j = 0; j < m; ++j) {
        w_new[static_cast<std::size_t>(j)] =
            w[static_cast<std::size_t>(j)] +
            st.dt * (f[static_cast<std::size_t>(j)] - f[static_cast<std::size_t>(j + 1)]) +
            (st.layer_inflow ? (*st.layer_inflow)[static_cast<std::size_t>(j)] : 0.0);
    }

    double overflow = 0.0, draw = 0.0;
    settleLayerBounds(w_new, m, p.theta_s * dz1, p.theta_r * dz1, st.transfers, overflow, draw);
    st.overflow_to_sat = overflow / st.dt; // m/s down across the table
    st.deficit_from_sat = draw / st.dt;    // m/s up across the table
    if (st.transfers)
        st.transfers->new_water = w_new;

    for (int j = 0; j < m; ++j)
        at(theta, j, stride) = dz1 > 0.0 ? w_new[static_cast<std::size_t>(j)] / dz1 : p.theta_s;
    st.theta_bot = at(theta, m - 1, stride);
}

void advanceColumn(const soil::Params& p, double* theta, int m, int stride,
                   ColumnStep& st) noexcept {
    advancePreparedColumn(p, theta, m, stride, st, nullptr);
}

void advanceColumn(const soil::Params& p, double* theta, int m, int stride, ColumnStep& st,
                   const ColumnPreparation& prepared) noexcept {
    advancePreparedColumn(p, theta, m, stride, st, &prepared);
}

} // namespace openswmm::twoD::sigma
