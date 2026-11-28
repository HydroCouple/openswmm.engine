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
 * @file      ReactionExplicitKernel.hpp
 * @brief     Portable kernels of the reaction module: the expression evaluator and the explicit
 *            integrators (Euler, Cash-Karp RK5) for RATE and FORMULA species.
 *
 * @details One implementation serves the host and a device. ReactionIntegrator::step() and
 *          evalReactionExpression() call these kernels on the host; the Kokkos 2D marcher calls
 *          them per cell on a device. The kernels read plain arrays through raw pointers, keep
 *          no state, allocate nothing and report a status code instead of a message, so they
 *          are legal inside a device kernel. Implicit solvers (ROS2, BDF2), EQUIL species and
 *          shared rate contexts stay in ReactionIntegrator.cpp.
 *
 * @par Device builds
 *      Define OPENSWMM_KERNEL_FN (to KOKKOS_INLINE_FUNCTION) before including this header, as
 *      for the other shared kernel headers.
 *
 * @ingroup engine_transport
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_TRANSPORT_REACTION_EXPLICIT_KERNEL_HPP
#define OPENSWMM_ENGINE_TRANSPORT_REACTION_EXPLICIT_KERNEL_HPP

#include <cmath>
#include <limits>

#include "../../../core/Constants.hpp"
#include "../../../data/ReactionTokens.hpp"

#ifndef OPENSWMM_KERNEL_FN
#define OPENSWMM_KERNEL_FN inline
#endif

namespace openswmm::transport::rxk {

inline constexpr int kMaxSubsteps = 100000;    ///< RATE substeps one step may take
inline constexpr double kMinStepFrac = 1.0e-8; ///< substep floor, as a fraction of the step

/// Species forms and solvers as the integers ReactionData's enums hold.
inline constexpr int kFormRate = 1, kFormEquil = 2, kFormFormula = 3;
inline constexpr int kSolverEuler = 0, kSolverRk5 = 1;

/// Largest system the device stage integrates; a larger one runs on the host.
inline constexpr int kDeviceMaxSpecies = 16, kDeviceMaxTerms = 48, kDeviceMaxPollutants = 16;

/// Configured rate units (ReactionRateUnits as an integer) to a per-second multiplier.
inline double rateUnitFactor(int units) {
    switch (units) {
    case 0:
        return 1.0;
    case 1:
        return 1.0 / 60.0;
    case 2:
        return 1.0 / 3600.0;
    default:
        return 1.0 / constants::SEC_PER_DAY;
    }
}

/// Why a step failed. Zero is success; statusMessage() gives the text the host reports.
enum Status : int {
    kOk = 0,
    kSubstepCap,          ///< more than kMaxSubsteps substeps
    kStepFloor,           ///< substep fell below the floor
    kRateAtAcceptedState, ///< non-finite rate at an accepted state
    kProjection,          ///< non-finite state during algebraic projection
    kIntegration,         ///< non-finite state during integration
    kProjectionAfterStep, ///< non-finite state after the final projection
    kStateAfterStep       ///< non-finite state after the step
};

/// Outcome of one explicit step.
struct Result {
    int status = kOk;
    bool interval_limit = false; ///< a work or floor limit: a shorter interval may succeed
    int substeps = 0;            ///< RATE substeps taken
};

/**
 * @brief A compiled reaction system reduced to the arrays the explicit kernels read.
 * @note Every pointer is borrowed and must outlive the call. On a device they point into
 * device memory.
 */
struct Program {
    const RxToken* pool = nullptr;         ///< compiled token pool
    const RxExprSpan* term_expr = nullptr; ///< [n_terms] term expressions, in order
    const unsigned char* needed = nullptr; ///< [n_terms] terms an active expression reaches
    const RxExprSpan* expr = nullptr;      ///< [n_species] species expressions
    const int* rate_idx = nullptr;         ///< [n_rate] RATE species, ascending
    const int* formula_idx = nullptr;      ///< [n_formula] FORMULA species, ascending
    const double* species_atol = nullptr;  ///< [n_species] zero selects atol
    const double* species_rtol = nullptr;  ///< [n_species] zero selects rtol
    const double* kinetic_scale = nullptr; ///< [n_species] rate weights, or null for one
    int n_terms = 0, n_species = 0, n_rate = 0, n_formula = 0;
    int solver = kSolverRk5;
    bool full_coupling = false; ///< project FORMULA species inside every rate evaluation
    double atol = 0.0, rtol = 0.0;
    double factor = 1.0; ///< rate units to per second
};

/// Caller-owned scratch: terms holds n_terms values, every other array n_rate values.
struct Work {
    double* terms = nullptr;
    double *y = nullptr, *ytmp = nullptr, *rates = nullptr, *err = nullptr;
    double *k1 = nullptr, *k2 = nullptr, *k3 = nullptr, *k4 = nullptr, *k5 = nullptr, *k6 = nullptr;
};

/**
 * @brief Evaluate one compiled expression on a fixed stack.
 * @param[in] pool Compiled token pool covering the span.
 * @param[in] span Expression; a non-positive length evaluates to zero.
 * @param[in] species Species values the expression may read.
 * @param[in] coefs Coefficient values.
 * @param[in] terms Term values, evaluated earlier in order.
 * @param[in] hydvar RxHydVar::COUNT_ hydraulic variables.
 * @param[in] pollutants Pollutant concentrations; null when no token reads one.
 * @return The value. Domain errors and overflow give NaN or infinity, not a status.
 * @note The compiler bounds the stack depth by kRxMaxStackDepth, so the stack cannot overflow.
 * Indices are not checked.
 */
OPENSWMM_KERNEL_FN double evalExpr(const RxToken* pool, const RxExprSpan& span,
                                   const double* species, const double* coefs, const double* terms,
                                   const double* hydvar, const double* pollutants) noexcept {
    if (span.len <= 0)
        return 0.0;
    double st[kRxMaxStackDepth];
    int sp = 0;
    const RxToken* t = pool + span.begin;
    const RxToken* end = t + span.len;
    for (; t != end; ++t) {
        switch (t->op) {
        case RxToken::PUSH_NUM:
            st[sp++] = t->value;
            break;
        case RxToken::PUSH_SPECIES:
            st[sp++] = species[t->idx];
            break;
        case RxToken::PUSH_COEF:
            st[sp++] = coefs[t->idx];
            break;
        case RxToken::PUSH_TERM:
            st[sp++] = terms[t->idx];
            break;
        case RxToken::PUSH_HYDVAR:
            st[sp++] = hydvar[t->idx];
            break;
        case RxToken::PUSH_POLLUT:
            st[sp++] = pollutants[t->idx];
            break;
        case RxToken::ADD:
            --sp;
            st[sp - 1] += st[sp];
            break;
        case RxToken::SUB:
            --sp;
            st[sp - 1] -= st[sp];
            break;
        case RxToken::MUL:
            --sp;
            st[sp - 1] *= st[sp];
            break;
        case RxToken::DIV:
            --sp;
            st[sp - 1] /= st[sp];
            break;
        case RxToken::POW:
            --sp;
            st[sp - 1] = std::pow(st[sp - 1], st[sp]);
            break;
        case RxToken::NEG:
            st[sp - 1] = -st[sp - 1];
            break;
        case RxToken::F_EXP:
            st[sp - 1] = std::exp(st[sp - 1]);
            break;
        case RxToken::F_LOG:
            st[sp - 1] = std::log(st[sp - 1]);
            break;
        case RxToken::F_LOG10:
            st[sp - 1] = std::log10(st[sp - 1]);
            break;
        case RxToken::F_SQRT:
            st[sp - 1] = std::sqrt(st[sp - 1]);
            break;
        case RxToken::F_ABS:
            st[sp - 1] = std::fabs(st[sp - 1]);
            break;
        case RxToken::F_SGN:
            st[sp - 1] = (st[sp - 1] > 0.0) ? 1.0 : (st[sp - 1] < 0.0) ? -1.0 : 0.0;
            break;
        case RxToken::F_STEP:
            st[sp - 1] = (st[sp - 1] > 0.0) ? 1.0 : 0.0;
            break;
        case RxToken::F_SIN:
            st[sp - 1] = std::sin(st[sp - 1]);
            break;
        case RxToken::F_COS:
            st[sp - 1] = std::cos(st[sp - 1]);
            break;
        case RxToken::F_TAN:
            st[sp - 1] = std::tan(st[sp - 1]);
            break;
        case RxToken::F_MIN:
            --sp;
            st[sp - 1] = (st[sp - 1] < st[sp]) ? st[sp - 1] : st[sp];
            break;
        case RxToken::F_MAX:
            --sp;
            st[sp - 1] = (st[sp - 1] > st[sp]) ? st[sp - 1] : st[sp];
            break;
        }
    }
    return st[0];
}

/// Mark the terms one expression reads (the host builds Program::needed with it).
inline void markTerms(const RxToken* pool, const RxExprSpan& expression, unsigned char* needed) {
    for (int i = expression.begin; i < expression.begin + expression.len; ++i)
        if (pool[i].op == RxToken::PUSH_TERM)
            needed[pool[i].idx] = 1;
}

/// True when every entry is finite; an empty tuple is finite.
/// @note Not std::isfinite: with the MSVC standard library it is a host-only template, and
/// nvcc then treats the device call as unreachable and prunes everything after it.
OPENSWMM_KERNEL_FN bool finiteAll(const double* v, int n) {
    for (int i = 0; i < n; ++i)
        if (!(std::fabs(v[i]) <= std::numeric_limits<double>::max()))
            return false;
    return true;
}

/// Evaluate the reachable terms, in declaration order, for one trial state.
OPENSWMM_KERNEL_FN void evalTerms(const Program& p, const double* species, const double* hydvar,
                                  const double* pollutants, const double* coefs, double* terms) {
    for (int i = 0; i < p.n_terms; ++i)
        if (p.needed[i])
            terms[i] = evalExpr(p.pool, p.term_expr[i], species, coefs, terms, hydvar, pollutants);
}

/**
 * @brief Assign every FORMULA species from its expression, in declaration order.
 * @return False when the species tuple is not finite afterwards.
 */
OPENSWMM_KERNEL_FN bool projectFormulas(const Program& p, double* species, const double* hydvar,
                                        const double* pollutants, const double* coefs,
                                        double* terms) {
    for (int k = 0; k < p.n_formula; ++k) {
        const int s = p.formula_idx[k];
        evalTerms(p, species, hydvar, pollutants, coefs, terms);
        species[s] = evalExpr(p.pool, p.expr[s], species, coefs, terms, hydvar, pollutants);
    }
    return finiteAll(species, p.n_species);
}

/// Weighted RMS of a local error estimate against the per-species tolerances.
OPENSWMM_KERNEL_FN double errNorm(const Program& p, const double* y, const double* e) {
    if (p.n_rate == 0)
        return 0.0;
    double s = 0.0;
    for (int k = 0; k < p.n_rate; ++k) {
        const int us = p.rate_idx[k];
        const double atol = (p.species_atol[us] > 0.0) ? p.species_atol[us] : p.atol;
        const double rtol = (p.species_rtol[us] > 0.0) ? p.species_rtol[us] : p.rtol;
        const double w = atol + rtol * std::fabs(y[k]);
        const double r = e[k] / w;
        s += r * r;
    }
    return std::sqrt(s / static_cast<double>(p.n_rate));
}

/**
 * @brief Rates of the RATE species at trial state @p y.
 * @param[in] y The RATE species' trial values, packed in rate_idx order.
 * @param[out] out Their rates, per second.
 * @param[in,out] status Receives kProjection when the projected state is not finite and no
 * earlier failure is recorded.
 * @return False when the algebraic projection failed; @p out is then all NaN.
 */
OPENSWMM_KERNEL_FN bool rateRhs(const Program& p, const double* y, double* out, double* species,
                                const double* hydvar, const double* pollutants, const double* coefs,
                                const Work& w, int& status) {
    const int gn = p.n_rate;
    for (int k = 0; k < gn; ++k)
        w.ytmp[k] = y[k];
    for (int k = 0; k < gn; ++k)
        species[p.rate_idx[k]] = w.ytmp[k];
    if (p.full_coupling && !projectFormulas(p, species, hydvar, pollutants, coefs, w.terms)) {
        if (status == kOk)
            status = kProjection;
        for (int k = 0; k < gn; ++k)
            out[k] = std::numeric_limits<double>::quiet_NaN();
        return false;
    }
    evalTerms(p, species, hydvar, pollutants, coefs, w.terms);
    for (int k = 0; k < gn; ++k)
        w.rates[k] =
            p.factor * (p.kinetic_scale ? p.kinetic_scale[p.rate_idx[k]] : 1.) *
            evalExpr(p.pool, p.expr[p.rate_idx[k]], species, coefs, w.terms, hydvar, pollutants);
    for (int k = 0; k < gn; ++k)
        out[k] = w.rates[k];
    return true;
}

/**
 * @brief Advance the RATE species over @p dt with Euler or adaptive Cash-Karp RK5, then publish
 * the FORMULA species.
 * @param[in] p The system. Its solver must be kSolverEuler or kSolverRk5 and it must hold no
 * EQUIL species.
 * @param[in] dt Positive step, seconds. The caller has stored it in hydvar's DT slot.
 * @param[in,out] species [n_species] values; holds trial values when the step fails, so a
 * caller that must keep the old state integrates a copy.
 * @param[in] hydvar RxHydVar::COUNT_ hydraulic variables.
 * @param[in] pollutants Pollutant concentrations, or null when no token reads one.
 * @param[in] coefs Coefficient values.
 * @param[in] w Scratch.
 * @return The status, whether a shorter interval may succeed, and the substeps taken.
 */
OPENSWMM_KERNEL_FN Result explicitStep(const Program& p, double dt, double* species,
                                       const double* hydvar, const double* pollutants,
                                       const double* coefs, const Work& w) {
    Result rep;
    bool ok = true;
    const int gn = p.n_rate;
    if (gn > 0) {
        double t = 0.0;
        double h = dt;
        const double h_min = dt * kMinStepFrac;
        int guard = 0;
        double* y = w.y;
        for (int k = 0; k < gn; ++k)
            y[k] = species[p.rate_idx[k]];

        while (t < dt && ok) {
            if (++guard > kMaxSubsteps) {
                ok = false;
                rep.interval_limit = true;
                rep.status = kSubstepCap;
                break;
            }
            if (h < h_min) {
                ok = false;
                rep.interval_limit = true;
                rep.status = kStepFloor;
                break;
            }
            if (t + h > dt)
                h = dt - t;
            // An invalid law at the accepted state cannot be repaired by a
            // shorter interval; trial-stage excursions use the controller.
            if (!rateRhs(p, y, w.k1, species, hydvar, pollutants, coefs, w, rep.status))
                ok = false;
            if (!finiteAll(w.k1, gn)) {
                ok = false;
                rep.status = kRateAtAcceptedState;
                break;
            }
            bool accepted = false;
            // The step for the NEXT substep: `t += h` below must advance by
            // the step taken, not the one proposed.
            double h_next = h;

            if (p.solver == kSolverEuler) {
                for (int k = 0; k < gn; ++k)
                    y[k] += h * w.k1[k];
                accepted = true;
            } else {
                // Cash-Karp 4(5).
                constexpr double b21 = 0.2, b31 = 3.0 / 40.0, b32 = 9.0 / 40.0, b41 = 0.3,
                                 b42 = -0.9, b43 = 1.2, b51 = -11.0 / 54.0, b52 = 2.5,
                                 b53 = -70.0 / 27.0, b54 = 35.0 / 27.0, b61 = 1631.0 / 55296.0,
                                 b62 = 175.0 / 512.0, b63 = 575.0 / 13824.0,
                                 b64 = 44275.0 / 110592.0, b65 = 253.0 / 4096.0, c1 = 37.0 / 378.0,
                                 c3 = 250.0 / 621.0, c4 = 125.0 / 594.0, c6 = 512.0 / 1771.0,
                                 d1 = 2825.0 / 27648.0, d3 = 18575.0 / 48384.0,
                                 d4 = 13525.0 / 55296.0, d5 = 277.0 / 14336.0, d6 = 0.25;
                for (int k = 0; k < gn; ++k)
                    w.ytmp[k] = y[k] + h * b21 * w.k1[k];
                if (!rateRhs(p, w.ytmp, w.k2, species, hydvar, pollutants, coefs, w, rep.status))
                    ok = false;
                for (int k = 0; k < gn; ++k)
                    w.ytmp[k] = y[k] + h * (b31 * w.k1[k] + b32 * w.k2[k]);
                if (!rateRhs(p, w.ytmp, w.k3, species, hydvar, pollutants, coefs, w, rep.status))
                    ok = false;
                for (int k = 0; k < gn; ++k)
                    w.ytmp[k] = y[k] + h * (b41 * w.k1[k] + b42 * w.k2[k] + b43 * w.k3[k]);
                if (!rateRhs(p, w.ytmp, w.k4, species, hydvar, pollutants, coefs, w, rep.status))
                    ok = false;
                for (int k = 0; k < gn; ++k)
                    w.ytmp[k] =
                        y[k] + h * (b51 * w.k1[k] + b52 * w.k2[k] + b53 * w.k3[k] + b54 * w.k4[k]);
                if (!rateRhs(p, w.ytmp, w.k5, species, hydvar, pollutants, coefs, w, rep.status))
                    ok = false;
                for (int k = 0; k < gn; ++k)
                    w.ytmp[k] = y[k] + h * (b61 * w.k1[k] + b62 * w.k2[k] + b63 * w.k3[k] +
                                            b64 * w.k4[k] + b65 * w.k5[k]);
                if (!rateRhs(p, w.ytmp, w.k6, species, hydvar, pollutants, coefs, w, rep.status))
                    ok = false;
                for (int k = 0; k < gn; ++k) {
                    const double y5 =
                        y[k] + h * (c1 * w.k1[k] + c3 * w.k3[k] + c4 * w.k4[k] + c6 * w.k6[k]);
                    const double y4 = y[k] + h * (d1 * w.k1[k] + d3 * w.k3[k] + d4 * w.k4[k] +
                                                  d5 * w.k5[k] + d6 * w.k6[k]);
                    w.err[k] = y5 - y4;
                    w.ytmp[k] = y5;
                }
                const double en = errNorm(p, w.ytmp, w.err);
                if (en <= 1.0) {
                    for (int k = 0; k < gn; ++k)
                        y[k] = w.ytmp[k];
                    accepted = true;
                    if (en > 1.0e-30) {
                        const double grown = 0.9 * h * std::pow(en, -0.2);
                        h_next = (grown < dt) ? grown : dt; // std::min(dt, grown)
                    }
                } else {
                    const double shrink = 0.9 * std::pow(en, -0.25);
                    h *= (0.1 < shrink) ? shrink : 0.1; // std::max(0.1, shrink)
                }
            }

            if (accepted) {
                t += h; // the step taken, before adopting the proposal
                h = h_next;
                ++rep.substeps;
            } else if (!finiteAll(y, gn)) {
                ok = false;
                rep.status = kIntegration;
            }
        }
        if (ok)
            for (int k = 0; k < gn; ++k)
                species[p.rate_idx[k]] = y[k];
    }
    if (!ok)
        return rep;

    // Always publish the algebraic values of the accepted end-of-step state.
    if (!projectFormulas(p, species, hydvar, pollutants, coefs, w.terms)) {
        if (rep.status == kOk)
            rep.status = kProjectionAfterStep;
        return rep;
    }
    if (!finiteAll(species, p.n_species))
        rep.status = kStateAfterStep;
    return rep;
}

/**
 * @brief The message the host reports for a failed step.
 * @param[in] status A Status other than kOk.
 * @return A static string; empty for kOk.
 */
inline const char* statusMessage(int status) {
    switch (status) {
    case kSubstepCap:
        return "reaction step exceeded the substep cap: the explicit solver is resolving "
               "kinetics too stiff for it. Set [REACTION_OPTIONS] SOLVER to ROS2 or BDF2, or "
               "declare the fast species EQUIL.";
    case kStepFloor:
        return "reaction step size collapsed below the floor (stiffness beyond the solver — see "
               "D-R7 escape hatch)";
    case kRateAtAcceptedState:
        return "non-finite reaction rate at accepted state";
    case kProjection:
        return "non-finite species state during algebraic projection";
    case kIntegration:
        return "non-finite species state during reaction integration";
    case kProjectionAfterStep:
        return "non-finite species state after algebraic projection";
    case kStateAfterStep:
        return "non-finite species state after reaction step";
    default:
        return "";
    }
}

} // namespace openswmm::transport::rxk

#endif // OPENSWMM_ENGINE_TRANSPORT_REACTION_EXPLICIT_KERNEL_HPP
