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
 * @file ReactionIntegrator.cpp
 * @brief Reaction kinetics integration — phase R3 body.
 *
 * @details Solver notes:
 *          - EUL: single explicit Euler step over dt (MSX EUL semantics).
 *          - RK5: Cash–Karp embedded 4(5) with adaptive substepping
 *            controlled by the per-species atol/rtol weighted RMS norm.
 *          - ROS2: 2-stage L-stable Rosenbrock (γ = 1 − 1/√2) with FD
 *            Jacobian, adaptive by step-doubling comparison.
 *          - BDF2: fixed-order BDF2 with Newton (+ dense LU), backward-
 *            Euler startup step, adaptive by halving on Newton failure —
 *            the D-R7-amendment stiff workhorse.
 *          Substep floor guards against pathological kinetics; hitting it
 *          is a HARD failure (report.error), never a silent inaccuracy.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "ReactionIntegrator.hpp"

#include <algorithm>
#include <cmath>

#include "ReactionExpression.hpp"

namespace openswmm::transport {

namespace {

constexpr int    kMaxSubsteps    = 100000;
constexpr int    kNewtonMax      = 50;
constexpr double kMinStepFrac    = 1.0e-8;   ///< dt_sub floor = dt * frac
constexpr double kFdEps          = 1.0e-7;

double unitFactor(ReactionRateUnits u) {
    switch (u) {
        case ReactionRateUnits::SEC: return 1.0;
        case ReactionRateUnits::MIN: return 1.0 / 60.0;
        case ReactionRateUnits::HR:  return 1.0 / 3600.0;
        default:                     return 1.0 / 86400.0;
    }
}

/// Evaluate all terms (forward-only order) for the current species state.
void evalTerms(const ReactionData& rx, const double* species,
               const double* hydvar, std::vector<double>& terms) {
    RxEvalEnv env{species, rx.coef_value.data(), terms.data(), hydvar};
    for (std::size_t i = 0; i < rx.term_expr.size(); ++i)
        terms[i] = evalReactionExpression(rx.token_pool, rx.term_expr[i], env);
}

/// Rates (1/s) for the RATE subset listed in `idx`, at state `species`.
void evalRates(const ReactionData& rx, bool tank, const double* species,
               const double* hydvar, std::vector<double>& terms,
               const std::vector<int>& idx, double factor, double* out) {
    evalTerms(rx, species, hydvar, terms);
    RxEvalEnv env{species, rx.coef_value.data(), terms.data(), hydvar};
    const auto& spans = tank ? rx.tank_expr : rx.pipe_expr;
    for (std::size_t k = 0; k < idx.size(); ++k)
        out[k] = factor *
                 evalReactionExpression(rx.token_pool,
                                        spans[static_cast<std::size_t>(idx[k])],
                                        env);
}

/// Weighted RMS error norm against per-species atol/rtol. `idx`/`count`
/// describe the ACTIVE GROUP (group-local y/e views) — under COUPLING NONE
/// the group is a single species, and iterating the full RATE set here
/// would read past the group-local arrays.
double errNorm(const ReactionData& rx, const int* idx, std::size_t count,
               const double* y, const double* e) {
    if (count == 0) return 0.0;
    double s = 0.0;
    for (std::size_t k = 0; k < count; ++k) {
        const auto us = static_cast<std::size_t>(idx[k]);
        const double atol = (rx.species_atol[us] > 0.0) ? rx.species_atol[us]
                                                        : rx.atol;
        const double rtol = (rx.species_rtol[us] > 0.0) ? rx.species_rtol[us]
                                                        : rx.rtol;
        const double w = atol + rtol * std::fabs(y[k]);
        const double r = e[k] / w;
        s += r * r;
    }
    return std::sqrt(s / static_cast<double>(count));
}

bool finiteAll(const double* v, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i)
        if (!std::isfinite(v[i])) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Dense LU (partial pivoting) on n×n scratch — n is small (species count).
// ---------------------------------------------------------------------------

bool luFactor(std::vector<double>& a, std::vector<int>& piv, int n) {
    for (int i = 0; i < n; ++i) piv[static_cast<std::size_t>(i)] = i;
    for (int c = 0; c < n; ++c) {
        int p = c;
        double best = std::fabs(a[static_cast<std::size_t>(c * n + c)]);
        for (int r = c + 1; r < n; ++r) {
            const double v = std::fabs(a[static_cast<std::size_t>(r * n + c)]);
            if (v > best) { best = v; p = r; }
        }
        if (best < 1.0e-300) return false;
        if (p != c) {
            for (int k = 0; k < n; ++k)
                std::swap(a[static_cast<std::size_t>(c * n + k)],
                          a[static_cast<std::size_t>(p * n + k)]);
            std::swap(piv[static_cast<std::size_t>(c)],
                      piv[static_cast<std::size_t>(p)]);
        }
        const double d = a[static_cast<std::size_t>(c * n + c)];
        for (int r = c + 1; r < n; ++r) {
            const double m = a[static_cast<std::size_t>(r * n + c)] / d;
            a[static_cast<std::size_t>(r * n + c)] = m;
            for (int k = c + 1; k < n; ++k)
                a[static_cast<std::size_t>(r * n + k)] -=
                    m * a[static_cast<std::size_t>(c * n + k)];
        }
    }
    return true;
}

void luSolve(const std::vector<double>& a, const std::vector<int>& piv,
             int n, const double* b, double* x) {
    for (int i = 0; i < n; ++i)
        x[i] = b[piv[static_cast<std::size_t>(i)]];
    for (int i = 0; i < n; ++i)
        for (int k = 0; k < i; ++k)
            x[i] -= a[static_cast<std::size_t>(i * n + k)] * x[k];
    for (int i = n - 1; i >= 0; --i) {
        for (int k = i + 1; k < n; ++k)
            x[i] -= a[static_cast<std::size_t>(i * n + k)] * x[k];
        x[i] /= a[static_cast<std::size_t>(i * n + i)];
    }
}

}  // namespace

// ===========================================================================
// Workspace
// ===========================================================================

void RxWorkspace::init(const ReactionData& rx) {
    n_ = rx.n_species();
    const auto un = static_cast<std::size_t>(n_);
    terms_.assign(rx.term_expr.size(), 0.0);
    y_.assign(un, 0.0);   y0_.assign(un, 0.0);  ytmp_.assign(un, 0.0);
    rates_.assign(un, 0.0); err_.assign(un, 0.0);
    k1_.assign(un, 0.0); k2_.assign(un, 0.0); k3_.assign(un, 0.0);
    k4_.assign(un, 0.0); k5_.assign(un, 0.0); k6_.assign(un, 0.0);
    jac_.assign(un * un, 0.0); lu_.assign(un * un, 0.0);
    piv_.assign(un, 0);
    res_.assign(un, 0.0); res2_.assign(un, 0.0); dy_.assign(un, 0.0);
    grp_out_.assign(un, 0.0);

    rate_idx_.clear(); equil_idx_.clear(); formula_idx_.clear();
    // Scope-independent classification is not possible (pipe vs tank forms
    // may differ per species); classified per call in step(). Vectors are
    // reserved here so step() never allocates.
    rate_idx_.reserve(un); equil_idx_.reserve(un); formula_idx_.reserve(un);
}

// ===========================================================================
// step
// ===========================================================================

RxStepReport ReactionIntegrator::step(const ReactionData& rx, bool tank,
                                      double dt, double* species,
                                      double* hydvar, RxWorkspace& ws) {
    RxStepReport rep;
    const int n = rx.n_species();
    if (!rx.compiled || n == 0 || dt <= 0.0) return rep;

    hydvar[static_cast<int>(RxHydVar::DT)] = dt;
    const double factor = unitFactor(rx.rate_units);
    const auto& forms = tank ? rx.tank_form : rx.pipe_form;

    // Classify species for this scope (no allocation — reserved in init).
    ws.rate_idx_.clear(); ws.equil_idx_.clear(); ws.formula_idx_.clear();
    for (int s = 0; s < n; ++s) {
        switch (forms[static_cast<std::size_t>(s)]) {
            case ReactionExprForm::RATE:    ws.rate_idx_.push_back(s); break;
            case ReactionExprForm::EQUIL:   ws.equil_idx_.push_back(s); break;
            case ReactionExprForm::FORMULA: ws.formula_idx_.push_back(s); break;
            default: break;
        }
    }

    // ---- 1. RATE integration -------------------------------------------
    const auto& ridx = ws.rate_idx_;
    const std::size_t nr = ridx.size();
    if (nr > 0) {
        const bool coupled = (rx.coupling == ReactionCoupling::FULL) || nr == 1;
        // COUPLING NONE with nr > 1: integrate one species at a time with
        // the others frozen (MSX semantics). Realized by running the same
        // machinery once per species with a 1-entry index view — but to
        // stay allocation-free we freeze OTHER rate species inside f by
        // restoring them after each evaluation; start-of-step snapshot:
        for (int s2 = 0; s2 < n; ++s2)
            ws.y0_[static_cast<std::size_t>(s2)] = species[s2];
        auto freeze_others = [&](std::size_t active) {
            if (coupled) return;
            for (std::size_t k = 0; k < nr; ++k)
                if (k != active)
                    species[ridx[k]] = ws.y0_[static_cast<std::size_t>(ridx[k])];
        };
        // Each group's result is STAGED here, not written straight back into
        // `species`: under COUPLING NONE the next group's freeze_others()
        // restores every other species from the start-of-step snapshot, which
        // would otherwise wipe the group just finished (leaving all but the
        // last species unintegrated). Published once, after every group.
        for (std::size_t k = 0; k < nr; ++k)
            ws.grp_out_[static_cast<std::size_t>(ridx[k])] =
                ws.y0_[static_cast<std::size_t>(ridx[k])];

        const std::size_t groups = coupled ? 1 : nr;
        for (std::size_t g = 0; g < groups && rep.ok; ++g) {
            // Active index set for this group.
            const std::size_t gb = coupled ? 0 : g;
            const std::size_t gn = coupled ? nr : 1;

            double t = 0.0;
            double h = dt;
            double h_prev = 0.0;          ///< last ACCEPTED step (BDF2 history)
            const double h_min = dt * kMinStepFrac;
            int guard = 0;

            auto fg = [&](const double* y, double* out) {
                // y/out are the group view; map through the full machinery.
                for (std::size_t k = 0; k < gn; ++k)
                    ws.ytmp_[k] = y[k];
                for (std::size_t k = 0; k < gn; ++k)
                    species[ridx[gb + k]] = ws.ytmp_[k];
                freeze_others(coupled ? static_cast<std::size_t>(-1) : gb);
                evalRates(rx, tank, species, hydvar, ws.terms_, ridx, factor,
                          ws.rates_.data());
                for (std::size_t k = 0; k < gn; ++k)
                    out[k] = ws.rates_[gb + k];
            };

            // Current group state.
            for (std::size_t k = 0; k < gn; ++k)
                ws.y_[k] = species[ridx[gb + k]];

            while (t < dt && rep.ok) {
                if (++guard > kMaxSubsteps) {
                    rep.ok = false;
                    rep.error = "reaction step exceeded the substep cap";
                    break;
                }
                if (h < h_min) {
                    rep.ok = false;
                    rep.error = "reaction step size collapsed below the "
                                "floor (stiffness beyond the solver — see "
                                "D-R7 escape hatch)";
                    break;
                }
                if (t + h > dt) h = dt - t;
                bool accepted = false;
                // Step size for the NEXT substep. The controllers must NOT
                // write `h` on acceptance: `t += h` below has to advance by
                // the step actually taken, not by the one being proposed.
                double h_next = h;

                switch (rx.solver) {
                    case ReactionSolverKind::EUL: {
                        fg(ws.y_.data(), ws.k1_.data());
                        for (std::size_t k = 0; k < gn; ++k)
                            ws.y_[k] += h * ws.k1_[k];
                        accepted = true;
                        break;
                    }
                    case ReactionSolverKind::RK5: {
                        // Cash–Karp 4(5).
                        static constexpr double
                          b21=0.2,
                          b31=3.0/40.0,  b32=9.0/40.0,
                          b41=0.3,       b42=-0.9,      b43=1.2,
                          b51=-11.0/54.0,b52=2.5,       b53=-70.0/27.0, b54=35.0/27.0,
                          b61=1631.0/55296.0, b62=175.0/512.0, b63=575.0/13824.0,
                          b64=44275.0/110592.0, b65=253.0/4096.0,
                          c1=37.0/378.0, c3=250.0/621.0, c4=125.0/594.0, c6=512.0/1771.0,
                          d1=2825.0/27648.0, d3=18575.0/48384.0, d4=13525.0/55296.0,
                          d5=277.0/14336.0,  d6=0.25;
                        double* y = ws.y_.data();
                        fg(y, ws.k1_.data());
                        for (std::size_t k = 0; k < gn; ++k)
                            ws.ytmp_[k] = y[k] + h * b21 * ws.k1_[k];
                        fg(ws.ytmp_.data(), ws.k2_.data());
                        for (std::size_t k = 0; k < gn; ++k)
                            ws.ytmp_[k] = y[k] + h * (b31*ws.k1_[k] + b32*ws.k2_[k]);
                        fg(ws.ytmp_.data(), ws.k3_.data());
                        for (std::size_t k = 0; k < gn; ++k)
                            ws.ytmp_[k] = y[k] + h * (b41*ws.k1_[k] + b42*ws.k2_[k] + b43*ws.k3_[k]);
                        fg(ws.ytmp_.data(), ws.k4_.data());
                        for (std::size_t k = 0; k < gn; ++k)
                            ws.ytmp_[k] = y[k] + h * (b51*ws.k1_[k] + b52*ws.k2_[k] + b53*ws.k3_[k] + b54*ws.k4_[k]);
                        fg(ws.ytmp_.data(), ws.k5_.data());
                        for (std::size_t k = 0; k < gn; ++k)
                            ws.ytmp_[k] = y[k] + h * (b61*ws.k1_[k] + b62*ws.k2_[k] + b63*ws.k3_[k] + b64*ws.k4_[k] + b65*ws.k5_[k]);
                        fg(ws.ytmp_.data(), ws.k6_.data());
                        for (std::size_t k = 0; k < gn; ++k) {
                            const double y5 = y[k] + h * (c1*ws.k1_[k] + c3*ws.k3_[k] + c4*ws.k4_[k] + c6*ws.k6_[k]);
                            const double y4 = y[k] + h * (d1*ws.k1_[k] + d3*ws.k3_[k] + d4*ws.k4_[k] + d5*ws.k5_[k] + d6*ws.k6_[k]);
                            ws.err_[k]  = y5 - y4;
                            ws.ytmp_[k] = y5;
                        }
                        const double en = errNorm(rx, ridx.data() + gb, gn,
                                                  ws.ytmp_.data(), ws.err_.data());
                        if (en <= 1.0) {
                            for (std::size_t k = 0; k < gn; ++k)
                                y[k] = ws.ytmp_[k];
                            accepted = true;
                            if (en > 1.0e-30)
                                h_next = std::min(dt, 0.9 * h *
                                                  std::pow(en, -0.2));
                        } else {
                            h *= std::max(0.1, 0.9 * std::pow(en, -0.25));
                        }
                        break;
                    }
                    case ReactionSolverKind::ROS2:
                    case ReactionSolverKind::BDF2: {
                        // Shared implicit machinery: FD Jacobian at y.
                        const int gni = static_cast<int>(gn);
                        double* y = ws.y_.data();
                        fg(y, ws.k1_.data());               // f(y)
                        for (int c = 0; c < gni; ++c) {
                            const double save = y[c];
                            const double dy = kFdEps * std::max(1.0, std::fabs(save));
                            y[c] = save + dy;
                            fg(y, ws.k2_.data());
                            y[c] = save;
                            for (int r = 0; r < gni; ++r)
                                ws.jac_[static_cast<std::size_t>(r * gni + c)] =
                                    (ws.k2_[static_cast<std::size_t>(r)] -
                                     ws.k1_[static_cast<std::size_t>(r)]) / dy;
                        }

                        if (rx.solver == ReactionSolverKind::ROS2) {
                            // 2-stage L-stable Rosenbrock (Verwer, Spee, Blom
                            // & Hundsdorfer), γ = 1 − 1/√2:
                            //   (I − γhJ) k1 = f(y)
                            //   (I − γhJ) k2 = f(y + h k1) − 2 k1
                            //   y⁺ = y + (3/2) h k1 + (1/2) h k2
                            // The weights must be 3/2 and 1/2: as h → 0,
                            // k1 → f and k2 → −f, so any other pair fails
                            // consistency. (1 − 1/(2γ), 1/(2γ)) = (−0.707,
                            // +1.707) gives y⁺ → y − 2.414 h f — the R3
                            // validator's step-size collapse.
                            const double g2 = 1.0 - 1.0 / std::sqrt(2.0);
                            for (int r = 0; r < gni; ++r)
                                for (int c = 0; c < gni; ++c)
                                    ws.lu_[static_cast<std::size_t>(r * gni + c)] =
                                        ((r == c) ? 1.0 : 0.0) -
                                        g2 * h *
                                        ws.jac_[static_cast<std::size_t>(r * gni + c)];
                            if (!luFactor(ws.lu_, ws.piv_, gni)) {
                                h *= 0.5; break;
                            }
                            luSolve(ws.lu_, ws.piv_, gni, ws.k1_.data(),
                                    ws.k3_.data());                 // k1 solved
                            for (std::size_t k = 0; k < gn; ++k)
                                ws.ytmp_[k] = y[k] + h * ws.k3_[k];
                            fg(ws.ytmp_.data(), ws.k4_.data());
                            for (std::size_t k = 0; k < gn; ++k)
                                ws.res_[k] = ws.k4_[k] - 2.0 * ws.k3_[k];
                            luSolve(ws.lu_, ws.piv_, gni, ws.res_.data(),
                                    ws.k5_.data());                 // k2 solved
                            constexpr double w1 = 1.5, w2 = 0.5;
                            for (std::size_t k = 0; k < gn; ++k) {
                                // Embedded first-order companion y + h k1;
                                // the difference is (h/2)(k1 + k2), which
                                // vanishes as h → 0 because k2 → −k1. The
                                // (k2 − k1) form tends to −2 h f instead and
                                // never falls below tolerance — the R3
                                // validator's 99,991-substep run.
                                ws.err_[k]  = 0.5 * h * (ws.k3_[k] + ws.k5_[k]);
                                ws.ytmp_[k] = y[k] + h * (w1 * ws.k3_[k] +
                                                          w2 * ws.k5_[k]);
                            }
                            const double en = errNorm(rx, ridx.data() + gb, gn,
                                                      ws.ytmp_.data(), ws.err_.data());
                            if (en <= 1.0) {
                                for (std::size_t k = 0; k < gn; ++k)
                                    y[k] = ws.ytmp_[k];
                                accepted = true;
                                if (en > 1.0e-30)
                                    h_next = std::min(dt, 0.9 * h *
                                                      std::pow(en, -1.0 / 3.0));
                            } else {
                                h *= std::max(0.1,
                                              0.9 * std::pow(en, -0.5));
                            }
                        } else {
                            // Newton-solved implicit step. BE starts the
                            // step; once a history point exists we use
                            // VARIABLE-step BDF2 with r = h/h_prev:
                            //   y⁺ = a0 y − a1 y_prev + β h f(y⁺),
                            //   a0 = (1+r)²/(1+2r), a1 = r²/(1+2r),
                            //   β  = (1+r)/(1+2r)
                            // (r = 1 recovers the familiar 4/3, 1/3, 2/3).
                            // The fixed-step coefficients were wrong to use
                            // here because the controller below changes h
                            // between substeps. History y_prev is ws.res2_.
                            const bool have_hist = (t > 0.0 && h_prev > 0.0);
                            const double r = have_hist ? (h / h_prev) : 0.0;
                            const double a0 = have_hist
                                ? (1.0 + r) * (1.0 + r) / (1.0 + 2.0 * r) : 1.0;
                            const double a1 = have_hist
                                ? (r * r) / (1.0 + 2.0 * r) : 0.0;
                            for (std::size_t k = 0; k < gn; ++k)
                                ws.ytmp_[k] = y[k];         // Newton seed
                            const double beta = have_hist
                                ? (1.0 + r) / (1.0 + 2.0 * r) : 1.0;
                            bool newton_ok = false;
                            for (int it = 0; it < kNewtonMax; ++it) {
                                ++rep.newton_iters;
                                fg(ws.ytmp_.data(), ws.k2_.data());
                                for (std::size_t k = 0; k < gn; ++k) {
                                    const double rhs = have_hist
                                        ? (a0 * y[k] - a1 * ws.res2_[k])
                                        : y[k];
                                    ws.res_[k] = ws.ytmp_[k] - rhs -
                                                 beta * h * ws.k2_[k];
                                }
                                double rn = 0.0;
                                for (std::size_t k = 0; k < gn; ++k)
                                    rn = std::max(rn, std::fabs(ws.res_[k]));
                                if (rn < rx.atol) { newton_ok = true; break; }
                                for (int r = 0; r < static_cast<int>(gn); ++r)
                                    for (int c = 0; c < static_cast<int>(gn); ++c)
                                        ws.lu_[static_cast<std::size_t>(
                                            r * static_cast<int>(gn) + c)] =
                                            ((r == c) ? 1.0 : 0.0) -
                                            beta * h *
                                            ws.jac_[static_cast<std::size_t>(
                                                r * static_cast<int>(gn) + c)];
                                if (!luFactor(ws.lu_, ws.piv_,
                                              static_cast<int>(gn)))
                                    break;
                                luSolve(ws.lu_, ws.piv_, static_cast<int>(gn),
                                        ws.res_.data(), ws.dy_.data());
                                for (std::size_t k = 0; k < gn; ++k)
                                    ws.ytmp_[k] -= ws.dy_[k];
                            }
                            if (newton_ok &&
                                finiteAll(ws.ytmp_.data(), gn)) {
                                // Error control. Without it BDF2 took ONE
                                // step of the whole dt (pure backward Euler)
                                // and was 248% off at k·dt = 2.5 — the R3
                                // validator's finding. The embedded
                                // companion is the explicit predictor
                                // y + h f(y); f(y) is ws.k1_, already
                                // evaluated for the Jacobian, so the
                                // estimate is free. Implicit − explicit is
                                // O(h²): a first-order pair, hence the
                                // -1/2 growth exponent.
                                for (std::size_t k = 0; k < gn; ++k)
                                    ws.err_[k] = ws.ytmp_[k] -
                                                 (y[k] + h * ws.k1_[k]);
                                const double en =
                                    errNorm(rx, ridx.data() + gb, gn,
                                            ws.ytmp_.data(), ws.err_.data());
                                if (en <= 1.0) {
                                    for (std::size_t k = 0; k < gn; ++k) {
                                        ws.res2_[k] = y[k];     // history
                                        y[k]        = ws.ytmp_[k];
                                    }
                                    accepted = true;
                                    if (en > 1.0e-30)
                                        h_next = std::min(dt, 0.9 * h *
                                                          std::pow(en, -0.5));
                                } else {
                                    h *= std::max(0.1,
                                                  0.9 * std::pow(en, -0.5));
                                }
                            } else {
                                h *= 0.5;
                            }
                        }
                        break;
                    }
                }

                if (accepted) {
                    t     += h;          // the step TAKEN, before adopting…
                    h_prev = h;          // …the controller's proposal
                    h      = h_next;
                    ++rep.substeps;
                    if (!finiteAll(ws.y_.data(), gn)) {
                        rep.ok = false;
                        rep.error = "non-finite species state during "
                                    "reaction integration";
                    }
                }
            }
            // Stage the group result; see the grp_out_ note above.
            if (rep.ok)
                for (std::size_t k = 0; k < gn; ++k)
                    ws.grp_out_[static_cast<std::size_t>(ridx[gb + k])] =
                        ws.y_[k];
        }
        // Publish every group at once, after the last freeze_others().
        if (rep.ok)
            for (std::size_t k = 0; k < nr; ++k)
                species[ridx[k]] =
                    ws.grp_out_[static_cast<std::size_t>(ridx[k])];
    }
    if (!rep.ok) return rep;

    // ---- 2. EQUIL: joint damped Newton on {expr_s = 0} ------------------
    const auto& eidx = ws.equil_idx_;
    const int ne = static_cast<int>(eidx.size());
    if (ne > 0) {
        const auto& spans = tank ? rx.tank_expr : rx.pipe_expr;
        auto residual = [&](double* out) {
            evalTerms(rx, species, hydvar, ws.terms_);
            RxEvalEnv env{species, rx.coef_value.data(), ws.terms_.data(),
                          hydvar};
            for (int k = 0; k < ne; ++k)
                out[k] = evalReactionExpression(
                    rx.token_pool,
                    spans[static_cast<std::size_t>(eidx[static_cast<std::size_t>(k)])],
                    env);
        };
        bool converged = false;
        for (int it = 0; it < kNewtonMax; ++it) {
            ++rep.newton_iters;
            residual(ws.res_.data());
            double rn = 0.0;
            for (int k = 0; k < ne; ++k)
                rn = std::max(rn, std::fabs(ws.res_[static_cast<std::size_t>(k)]));
            if (rn < rx.atol) { converged = true; break; }
            // FD Jacobian wrt the EQUIL species.
            for (int c = 0; c < ne; ++c) {
                const int sc = eidx[static_cast<std::size_t>(c)];
                const double save = species[sc];
                const double dy = kFdEps * std::max(1.0, std::fabs(save));
                species[sc] = save + dy;
                residual(ws.res2_.data());
                species[sc] = save;
                for (int r = 0; r < ne; ++r)
                    ws.jac_[static_cast<std::size_t>(r * ne + c)] =
                        (ws.res2_[static_cast<std::size_t>(r)] -
                         ws.res_[static_cast<std::size_t>(r)]) / dy;
            }
            for (std::size_t k = 0; k < static_cast<std::size_t>(ne * ne); ++k)
                ws.lu_[k] = ws.jac_[k];
            if (!luFactor(ws.lu_, ws.piv_, ne)) break;
            luSolve(ws.lu_, ws.piv_, ne, ws.res_.data(), ws.dy_.data());
            // Damped line search (halving) on the max-residual norm.
            double lambda = 1.0;
            for (int ls = 0; ls < 8; ++ls) {
                for (int k = 0; k < ne; ++k)
                    species[eidx[static_cast<std::size_t>(k)]] -=
                        lambda * ws.dy_[static_cast<std::size_t>(k)];
                residual(ws.res2_.data());
                double rn2 = 0.0;
                for (int k = 0; k < ne; ++k)
                    rn2 = std::max(rn2,
                                   std::fabs(ws.res2_[static_cast<std::size_t>(k)]));
                if (rn2 < rn) break;
                for (int k = 0; k < ne; ++k)          // undo, halve
                    species[eidx[static_cast<std::size_t>(k)]] +=
                        lambda * ws.dy_[static_cast<std::size_t>(k)];
                lambda *= 0.5;
            }
        }
        if (!converged) {
            rep.ok = false;
            rep.error = "EQUIL Newton failed to converge";
            return rep;
        }
    }

    // ---- 3. FORMULA: declaration order, current values ------------------
    if (!ws.formula_idx_.empty()) {
        const auto& spans = tank ? rx.tank_expr : rx.pipe_expr;
        for (const int s : ws.formula_idx_) {
            evalTerms(rx, species, hydvar, ws.terms_);
            RxEvalEnv env{species, rx.coef_value.data(), ws.terms_.data(),
                          hydvar};
            species[s] = evalReactionExpression(
                rx.token_pool, spans[static_cast<std::size_t>(s)], env);
        }
    }

    if (!finiteAll(species, static_cast<std::size_t>(n))) {
        rep.ok = false;
        rep.error = "non-finite species state after reaction step";
    }
    return rep;
}

}  // namespace openswmm::transport
