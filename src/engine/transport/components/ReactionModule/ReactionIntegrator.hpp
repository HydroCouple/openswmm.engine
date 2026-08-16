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
 * @file ReactionIntegrator.hpp
 * @brief Reaction kinetics integration over one reaction step (phase R3):
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
 *               line search, atol/rtol convergence, hard iteration cap).
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
 *          the step call is allocation-free (D-R3).
 *
 * @see plans/transport/MULTISPECIES_REACTIONS_MSX_PLAN.md §3.2 D-R3–D-R9
 * @ingroup engine_transport
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_TRANSPORT_REACTION_INTEGRATOR_HPP
#define OPENSWMM_ENGINE_TRANSPORT_REACTION_INTEGRATOR_HPP

#include <string>
#include <vector>

#include "../../../data/ReactionData.hpp"

namespace openswmm::transport {

/// Outcome of one reaction step. `ok == false` is a HARD failure (Newton
/// divergence, non-finite state) — the caller decides fatality; soft
/// adaptivity (substepping) is internal and only reported.
struct RxStepReport {
    bool ok            = true;
    int  substeps      = 0;   ///< RATE substeps actually taken
    int  newton_iters  = 0;   ///< EQUIL + implicit-solver Newton iterations
    std::string error;        ///< set when !ok
};

/// Caller-owned scratch; size once per (thread × reaction system). The
/// step call never allocates.
class RxWorkspace {
public:
    void init(const ReactionData& rx);
    bool initialized() const noexcept { return n_ > 0 || terms_.capacity() > 0; }

private:
    friend class ReactionIntegrator;
    int n_ = 0;                       ///< n_species
    std::vector<double> terms_;       ///< term value cache
    std::vector<double> y_, y0_, ytmp_, rates_, err_;
    std::vector<double> k1_, k2_, k3_, k4_, k5_, k6_;   ///< RK5 stages
    std::vector<double> jac_, lu_;                      ///< dense n×n
    std::vector<int>    piv_;
    std::vector<int>    rate_idx_, equil_idx_, formula_idx_;
    std::vector<double> res_, res2_, dy_;               ///< Newton
    std::vector<double> grp_out_;     ///< staged per-group RATE results

    // FD-Jacobian / LU reuse across substeps. The Jacobian costs `gn` extra
    // RHS evaluations, each of which re-evaluates every term and rate
    // expression — the dominant per-substep cost once a system has more than
    // a species or two. J depends on y (not on h), so it survives a step-size
    // change; the FACTORED matrix I - c*h*J does not, hence the separate
    // scale key.
    bool   jac_valid_ = false;   ///< jac_ holds a usable Jacobian
    int    jac_age_   = 0;       ///< accepted substeps on the current jac_
    bool   lu_valid_  = false;   ///< lu_/piv_ factored and reusable
    double lu_scale_  = 0.0;     ///< the c*h that lu_ was factored at
};

class ReactionIntegrator {
public:
    /**
     * @brief One reaction step on one element's local species block.
     *
     * @param rx      Compiled reaction system (rx.compiled must be true).
     * @param tank    true ⇒ tank-scope expressions, false ⇒ pipe scope.
     * @param dt      Step length (seconds).
     * @param species [in/out] local species values, size rx.n_species().
     * @param hydvar  RxHydVar::COUNT_ entries (DT slot is overwritten with
     *                dt by this call).
     * @param ws      Workspace initialized against `rx`.
     */
    static RxStepReport step(const ReactionData& rx, bool tank, double dt,
                             double* species, double* hydvar, RxWorkspace& ws,
                             const double* pollutants = nullptr);
};

}  // namespace openswmm::transport

#endif  // OPENSWMM_ENGINE_TRANSPORT_REACTION_INTEGRATOR_HPP
