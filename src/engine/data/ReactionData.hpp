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
 * @file ReactionData.hpp
 * @brief Multispecies reaction system data (EPANET-MSX conventions) — SoA,
 *        hot/cold split per LARD plan §16 D-L3 / reactions plan D-R3.
 *
 * @details Phase R1 populates this from the reactions component's config
 *          file (`model.rxn`, sections [REACTION_*]). Expression SOURCES
 *          are stored here (cold); the compiled flat RPN token pool arrives
 *          with phase R2 and lives in separate hot arrays so no
 *          std::string enters an integrator loop. Layout notes:
 *          per-species expression slots are dense (size n_species,
 *          form == NONE ⇒ no expression for that species in that scope).
 *
 * @see plans/transport/MULTISPECIES_REACTIONS_MSX_PLAN.md §2–§3
 * @ingroup engine_data
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_DATA_REACTION_DATA_HPP
#define OPENSWMM_ENGINE_DATA_REACTION_DATA_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ReactionTokens.hpp"

namespace openswmm {

enum class ReactionSolverKind : int { EUL = 0, RK5 = 1, ROS2 = 2, BDF2 = 3 };
enum class ReactionCoupling : int { NONE = 0, FULL = 1 };
enum class ReactionRateUnits : int { SEC = 0, MIN = 1, HR = 2, DAY = 3 };
enum class ReactionAreaUnits : int { FT2 = 0, M2 = 1, CM2 = 2 };
enum class ReactionExprForm : int { NONE = 0, RATE = 1, EQUIL = 2, FORMULA = 3 };

struct ReactionData {
    // ---- [REACTION_OPTIONS] ------------------------------------------------
    /**
     * @brief Default integrator.
     *
     * @details RK5, not an implicit solver. Measured substeps for one
     *          routing step, at the shipping tolerances below and at
     *          1e-10/1e-8:
     *
     *          | case                        | RK5   | ROS2   | BDF2   |
     *          |-----------------------------|-------|--------|--------|
     *          | first-order decay, default  |     5 |    134 |    197 |
     *          | stiff (λ ratio 1e6), default|  2682 |    281 |    408 |
     *          | stiff, atol 1e-10           |  2744 |  26938 |  39693 |
     *
     *          RK5 wins the common case by ~27x. It loses the stiff case at
     *          loose tolerance by ~10x but still COMPLETES, with the slow
     *          mode exact. At tight tolerance it beats both implicit solvers
     *          on the stiff problem, because RK5's step is STABILITY-limited
     *          (nearly flat in tolerance) while the Rosenbrock/BDF pairs are
     *          ACCURACY-limited and scale as sqrt(tol).
     *
     *          The cliff: RK5 costs about lambda_fast*dt/3.3 substeps, so
     *          past lambda_fast*dt ~ 3e5 it hits kMaxSubsteps and fails.
     *          That failure is loud and names ROS2/BDF2 as the remedy
     *          (ReactionIntegrator's substep-cap message) — a slow default
     *          that fails legibly beats an implicit default that is slower
     *          everywhere except the stiff-and-loose corner.
     */
    ReactionSolverKind solver      = ReactionSolverKind::RK5;
    ReactionCoupling   coupling    = ReactionCoupling::NONE;
    ReactionRateUnits  rate_units  = ReactionRateUnits::HR;
    ReactionAreaUnits  area_units  = ReactionAreaUnits::FT2;
    double             timestep    = 0.0;   ///< 0 ⇒ follow QUALITY_STEP
    double             atol        = 1.0e-6;
    double             rtol        = 1.0e-4;

    // ---- [REACTION_SPECIES] — index-aligned with the SpeciesRegistry MSX
    //      block (registry index = registry_base + i). ------------------------
    int registry_base = -1;                  ///< first MSX index in the registry
    std::vector<std::string> species_name;   ///< cold
    std::vector<uint8_t>     species_is_wall;///< 0 = BULK, 1 = WALL
    std::vector<std::string> species_units;  ///< cold
    std::vector<double>      species_atol;   ///< hot (0 ⇒ global)
    std::vector<double>      species_rtol;   ///< hot (0 ⇒ global)

    // ---- [REACTION_COEFFICIENTS] -------------------------------------------
    std::vector<std::string> coef_name;      ///< cold
    std::vector<uint8_t>     coef_is_param;  ///< 1 = PARAMETER (overridable), 0 = CONSTANT
    std::vector<double>      coef_value;     ///< hot (global values; per-element
                                             ///< overrides arrive with [REACTION_PARAMETERS])

    // ---- [REACTION_TERMS] --------------------------------------------------
    std::vector<std::string> term_name;      ///< cold
    std::vector<std::string> term_expr_src;  ///< cold (compiled in R2)

    // ---- [REACTION_PIPES] / [REACTION_TANKS] — dense per species -----------
    std::vector<ReactionExprForm> pipe_form;     ///< size n_species
    std::vector<std::string>      pipe_expr_src; ///< size n_species (cold)
    std::vector<ReactionExprForm> tank_form;
    std::vector<std::string>      tank_expr_src;

    // ---- [REACTION_QUALITY] GLOBAL initial values (R1 scope; NODE/LINK
    //      scopes land with R-later phases) ----------------------------------
    std::vector<double> init_global;         ///< size n_species

    // ---- Compiled bytecode (R2 — D-L3 flat pool; spans index token_pool) ----
    std::vector<RxToken>    token_pool;      ///< hot: one contiguous pool
    std::vector<RxExprSpan> term_expr;       ///< per term, in-order evaluation
    std::vector<RxExprSpan> pipe_expr;       ///< per species (len 0 ⇒ none)
    std::vector<RxExprSpan> tank_expr;       ///< per species
    bool compiled = false;                   ///< R2 compile pass succeeded

    // ---- R4: MSX species element state under QUALITY_SOLVER LEGACY --------
    // [element * n_species + s]; sized lazily by the legacy binding. MSX
    // species react per element but are NOT yet transported between elements
    // under LEGACY (R4b) — warned once per run.
    std::vector<double> msx_node_conc;
    std::vector<double> msx_link_conc;
    bool warned_msx_not_transported = false;
    bool warned_react_failure       = false;

    bool configured = false;                 ///< a reactions component applied

    int n_species() const noexcept {
        return static_cast<int>(species_name.size());
    }
    int find_species(std::string_view n) const {
        for (std::size_t i = 0; i < species_name.size(); ++i)
            if (species_name[i] == n) return static_cast<int>(i);
        return -1;
    }
    int find_coef(std::string_view n) const {
        for (std::size_t i = 0; i < coef_name.size(); ++i)
            if (coef_name[i] == n) return static_cast<int>(i);
        return -1;
    }
    int find_term(std::string_view n) const {
        for (std::size_t i = 0; i < term_name.size(); ++i)
            if (term_name[i] == n) return static_cast<int>(i);
        return -1;
    }

    void clear() { *this = ReactionData{}; }
};

}  // namespace openswmm

#endif  // OPENSWMM_ENGINE_DATA_REACTION_DATA_HPP
