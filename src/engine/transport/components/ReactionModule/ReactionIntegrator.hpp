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
 * @file      ReactionIntegrator.hpp
 * @brief     Reaction kinetics integration over one reaction step (phase R3):
 *        EUL / RK5 (Cash–Karp) / ROS2 / BDF2 for RATE species, joint
 *        damped-Newton for EQUIL species, in-order FORMULA evaluation,
 *        rate-unit scaling.
 *
 * @details Semantics (MSX conventions, reactions plan §2/§3):
 *          per step of length dt on one element's local species block:
 *            1. RATE species integrate dφ/dt = expr · unit_factor with the
 *               configured solver. COUPLING FULL integrates the RATE set
 *               as one system; COUPLING NONE integrates each RATE species
 *               with every other species frozen at start-of-step values.
 *            2. EQUIL species solve the joint algebraic system
 *               {expr_s = 0} by damped Newton (FD Jacobian, step-halving
 *               line search, absolute-residual atol convergence, hard iteration cap).
 *            3. FORMULA species evaluate in declaration order (may
 *               reference any current value; declaration-order semantics
 *               are the documented contract).
 *          Terms re-evaluate (in their forward-only order) inside every
 *          right-hand-side evaluation — they are part of f(φ).
 *
 *          Hand-rolled per D-R7 (no CVODE/SUNDIALS): the workload is many
 *          tiny independent systems; BDF2 (D-R7 amendment) covers the
 *          practically stiff range with Newton + the same dense LU ROS2
 *          uses. All scratch lives in a caller-owned workspace sized once —
 *          normal pre-sized numerical work avoids allocation; error-report strings may allocate.
 *          Opt-in interval subcycling also allocates species snapshots.
 *
 * @see plans/transport/MULTISPECIES_REACTIONS_MSX_PLAN.md §3.2 D-R3–D-R9
 * @ingroup engine_transport
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_TRANSPORT_REACTION_INTEGRATOR_HPP
#define OPENSWMM_ENGINE_TRANSPORT_REACTION_INTEGRATOR_HPP

#include <string>
#include <span>
#include <vector>

#include "../../../data/ReactionData.hpp"

namespace openswmm::transport {

/// Outcome of one reaction step. `ok == false` is a HARD failure (Newton
/// divergence, non-finite state) — the caller decides fatality; soft
/// adaptivity (substepping) is internal and only reported.
struct RxStepReport {
    bool ok = true;
    bool interval_limit = false; ///< work/floor limit: caller may retry a smaller interval
    int substeps = 0;            ///< RATE substeps actually taken
    int newton_iters = 0;        ///< EQUIL + implicit-solver Newton iterations
    std::string error;           ///< set when !ok
};
// Several aqueous phase contexts may contribute RATE terms to the same
// physical inventory. Each scale is phase aqueous volume / shared capacity.
// All contexts see the same species state at every integrator evaluation.
// Algebraic phase projections retain their separate existing contract.
struct RxRateContext {
    const std::vector<ReactionExprForm>* forms = nullptr;
    const std::vector<RxExprSpan>* expressions = nullptr;
    double* hydvar = nullptr;
    const double* pollutants = nullptr;
    const double* kinetic_scale = nullptr;
};

/// Caller-owned scratch; size once per (thread × reaction system). The
/// Numerical step scratch is pre-sized; report strings and optional retries may allocate.
class RxWorkspace {
public:
    void init(const ReactionData& rx);
    /**
     * @brief Read whether this workspace has retained species/term allocation.
     * @return n_>0 OR term-vector capacity>0; does not certify current reaction dimension
     * compatibility, compiled laws or complete successful init. No allocation/mutation.
     */
    bool initialized() const noexcept { return n_ > 0 || terms_.capacity() > 0; }

private:
    friend class ReactionIntegrator;
    int n_ = 0;                              ///< n_species
    std::vector<unsigned char> term_needed_; ///< reachable terms for this reactor context
    std::vector<double> terms_;              ///< term value cache
    std::vector<double> y_, y0_, ytmp_, rates_, err_;
    std::vector<double> k1_, k2_, k3_, k4_, k5_, k6_; ///< RK5 stages
    std::vector<double> jac_, lu_;                    ///< dense n×n
    std::vector<int> piv_;
    std::vector<int> rate_idx_, equil_idx_, formula_idx_;
    std::vector<double> res_, res2_, dy_; ///< Newton
    std::vector<double> grp_out_;         ///< staged per-group RATE results

    // FD-Jacobian / LU reuse across substeps. The Jacobian costs `gn` extra
    // RHS evaluations, each of which re-evaluates every term and rate
    // expression — the dominant per-substep cost once a system has more than
    // a species or two. J depends on y (not on h), so it survives a step-size
    // change; the FACTORED matrix I - c*h*J does not, hence the separate
    // scale key.
    bool jac_valid_ = false; ///< jac_ holds a usable Jacobian
    int jac_age_ = 0;        ///< accepted substeps on the current jac_
    bool lu_valid_ = false;  ///< lu_/piv_ factored and reusable
    double lu_scale_ = 0.0;  ///< the c*h that lu_ was factored at
};

class ReactionIntegrator {
public:
    // Explicit reactor context. Expressions and forms refer to the same
    // compiled token pool; groundwater never borrows pipe/tank laws.
    /**
     * @brief Integrate one explicit reactor law set over a local species block.
     * @param[in] rx Stable compiled reaction system, token/spans/dependencies/tolerances already
     * validated.
     * @param[in] dt Positive finite reaction duration s; ordinary step returns successful no-op
     * for uncompiled/zero-species/nonpositive dt, but does not explicitly reject NaN.
     * @param[in,out] species Writable rx.n_species() local values in declared species
     * concentration/area units; full readable state required for referenced expressions. No
     * automatic positivity clamp.
     * @param[in,out] hydvar Writable RxHydVar::COUNT_ tuple, required; DT overwritten with dt
     * for active solve. Other entries held by caller (HRT seconds, TEMP degC, AV declared area/L
     * and groundwater SI fields).
     * @param[in,out] ws Workspace initialized for exact species/term dimensions, exclusive to
     * this solve; scratch/caches overwritten. Distinct concurrent reactors need separate
     * workspaces.
     * @param[in] pollutants Optional stable pollutant concentration tuple, required for
     * referenced tokens.
     * @param[in] coefficients Optional rx.coef_value-sized coefficient override; null uses
     * rx.coef_value. All borrowed inputs remain valid through synchronous call and do not alias
     * species/workspace updates.
     * @param[in] forms Exactly n_species NONE/RATE/EQUIL/FORMULA selectors for this reactor.
     * @param[in] expressions Exactly n_species compiled spans into rx.token_pool, selected with
     * forms.
     * @param[in] kinetic_scale Optional n_species multiplicative RATE weights; single-context
     * values are not generically finite/range checked here. Null means1; must be null for shared
     * contexts.
     * @param[in] contexts Optional synchronous shared RATE contexts. Each has n_species
     * forms/spans, nonnull hydvar and nonnegative finite n_species scales, optional pollutants.
     * Only NONE/RATE forms allowed, context RATE rows must appear in primary RATE mask.
     * Contributions sum on same species state.
     * @return Status with accepted RATE substep and Newton counts, interval_limit for work/floor
     * exhaustion and error text for refusal. No independent concentration/nonnegativity/physical
     * mass qualification beyond solver's finite final-state checks; caller handles
     * acceptance/ledger policy.
     * @note RATE integrates first (FULL joint, NONE freezes other RATE rows at entry), then
     * joint EQUIL Newton using absolute global atol, then FORMULA in declaration order.
     * Reachable terms recompute forward for each evaluation. No hydraulic/global ledger/engine
     * clock advance. Shape/profile errors return before solve; ordinary numerical failure can
     * leave trial/accepted species and DT changed. No rollback or warning emission. Pre-sized
     * numerical path avoids allocation, but report strings or unexpected dependency failures can
     * throw; caller must isolate state for transactional acceptance.
     */
    static RxStepReport step(const ReactionData& rx, const std::vector<ReactionExprForm>& forms,
                             const std::vector<RxExprSpan>& expressions, double dt, double* species,
                             double* hydvar, RxWorkspace& ws, const double* pollutants = nullptr,
                             const double* kinetic_scale = nullptr,
                             const double* coefficients = nullptr,
                             std::span<const RxRateContext> contexts = {});
    // Opt-in transactional retry for an owned interval. Only a work/floor limit
    // permits bisection; invalid laws and Newton failures retain their error.
    // Tolerances are unchanged, and failure restores every supplied species.
    /**
     * @brief Retry an owned reaction interval by bounded bisection while restoring species on
     * returned failure.
     * @param[in] rx Stable compiled reaction system, token/spans/dependencies/tolerances already
     * validated.
     * @param[in] dt Positive finite reaction duration s; ordinary step returns successful no-op
     * for uncompiled/zero-species/nonpositive dt, but does not explicitly reject NaN.
     * @param[in,out] species Writable rx.n_species() local values in declared species
     * concentration/area units; full readable state required for referenced expressions. No
     * automatic positivity clamp.
     * @param[in,out] hydvar Writable RxHydVar::COUNT_ tuple, required; DT overwritten with dt
     * for active solve. Other entries held by caller (HRT seconds, TEMP degC, AV declared area/L
     * and groundwater SI fields).
     * @param[in,out] ws Workspace initialized for exact species/term dimensions, exclusive to
     * this solve; scratch/caches overwritten. Distinct concurrent reactors need separate
     * workspaces.
     * @param[in] pollutants Optional stable pollutant concentration tuple, required for
     * referenced tokens.
     * @param[in] coefficients Optional rx.coef_value-sized coefficient override; null uses
     * rx.coef_value. All borrowed inputs remain valid through synchronous call and do not alias
     * species/workspace updates.
     * @param[in] forms Exactly n_species NONE/RATE/EQUIL/FORMULA selectors for this reactor.
     * @param[in] expressions Exactly n_species compiled spans into rx.token_pool, selected with
     * forms.
     * @param[in] kinetic_scale Optional n_species multiplicative RATE weights; single-context
     * values are not generically finite/range checked here. Null means1; must be null for shared
     * contexts.
     * @param[in] contexts Optional synchronous shared RATE contexts. Each has n_species
     * forms/spans, nonnull hydvar and nonnegative finite n_species scales, optional pollutants.
     * Only NONE/RATE forms allowed, context RATE rows must appear in primary RATE mask.
     * Contributions sum on same species state.
     * @return Report from accepted interval(s), aggregating successful/rejected attempt
     * counters. Only interval_limit permits up to8 nested halvings; shape/law/Newton/finite
     * failures retain their error.
     * @note Saves species at every recursion, restores all n_species values on returned failure
     * and before retry. Does not restore workspace/caches or other caller state; resets
     * primary/context DT to original dt after normal return. Unlike ordinary step,
     * species/hydvar pointers must be valid even for inactive or zero-duration work because
     * snapshots/DT reset still occur. Allocations and exceptions are not caught and can bypass
     * restoration. Tolerances unchanged; no hydraulic/global-ledger/clock update.
     */
    static RxStepReport
    stepSubcycled(const ReactionData& rx, const std::vector<ReactionExprForm>& forms,
                  const std::vector<RxExprSpan>& expressions, double dt, double* species,
                  double* hydvar, RxWorkspace& ws, const double* pollutants = nullptr,
                  const double* kinetic_scale = nullptr, const double* coefficients = nullptr,
                  std::span<const RxRateContext> contexts = {});
    /**
     * @brief Select tank or pipe law arrays then integrate one local species block.
     * @param[in] rx Stable compiled reaction system, token/spans/dependencies/tolerances already
     * validated.
     * @param[in] dt Positive finite reaction duration s; ordinary step returns successful no-op
     * for uncompiled/zero-species/nonpositive dt, but does not explicitly reject NaN.
     * @param[in,out] species Writable rx.n_species() local values in declared species
     * concentration/area units; full readable state required for referenced expressions. No
     * automatic positivity clamp.
     * @param[in,out] hydvar Writable RxHydVar::COUNT_ tuple, required; DT overwritten with dt
     * for active solve. Other entries held by caller (HRT seconds, TEMP degC, AV declared area/L
     * and groundwater SI fields).
     * @param[in,out] ws Workspace initialized for exact species/term dimensions, exclusive to
     * this solve; scratch/caches overwritten. Distinct concurrent reactors need separate
     * workspaces.
     * @param[in] pollutants Optional stable pollutant concentration tuple, required for
     * referenced tokens.
     * @param[in] coefficients Optional rx.coef_value-sized coefficient override; null uses
     * rx.coef_value. All borrowed inputs remain valid through synchronous call and do not alias
     * species/workspace updates.
     * @param[in] tank True uses tank forms/spans, false pipe; no groundwater or other scoped
     * fallback.
     * @return Status with accepted RATE substep and Newton counts, interval_limit for work/floor
     * exhaustion and error text for refusal. No independent concentration/nonnegativity/physical
     * mass qualification beyond solver's finite final-state checks; caller handles
     * acceptance/ledger policy.
     * @note RATE integrates first (FULL joint, NONE freezes other RATE rows at entry), then
     * joint EQUIL Newton using absolute global atol, then FORMULA in declaration order.
     * Reachable terms recompute forward for each evaluation. No hydraulic/global ledger/engine
     * clock advance. Shape/profile errors return before solve; ordinary numerical failure can
     * leave trial/accepted species and DT changed. No rollback or warning emission. Pre-sized
     * numerical path avoids allocation, but report strings or unexpected dependency failures can
     * throw; caller must isolate state for transactional acceptance.
     */
    static RxStepReport step(const ReactionData& rx, bool tank, double dt, double* species,
                             double* hydvar, RxWorkspace& ws, const double* pollutants = nullptr,
                             const double* coefficients = nullptr);
};

} // namespace openswmm::transport

#endif // OPENSWMM_ENGINE_TRANSPORT_REACTION_INTEGRATOR_HPP
