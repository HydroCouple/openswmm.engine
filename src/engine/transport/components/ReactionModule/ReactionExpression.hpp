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
 * @file ReactionExpression.hpp
 * @brief Reaction expression compiler + Tier-1 evaluator (phase R2).
 *
 * @details Shunting-yard compiler from the MSX-convention DSL to a FLAT
 *          token pool (LARD plan §16 D-L3 / reactions plan D-R2 Tier 1):
 *          all compiled expressions share one contiguous
 *          `std::vector<RxToken>`; each expression is a (begin, len) span.
 *          Operands carry PRE-RESOLVED indices (species slot, coefficient
 *          slot, term slot, hydraulic-variable code) so evaluation is a
 *          single allocation-free pass over the span with a fixed-depth
 *          value stack — no name lookups, no std::string, no heap.
 *
 *          Same shunting-yard family as quality/Treatment.cpp (D-R1: one
 *          compiler approach, two front doors); actual TU-level
 *          consolidation of the two VMs is the PROCESS_MODULARIZATION §10
 *          decision and deliberately NOT forced here.
 *
 *          Grammar: numbers; identifiers (species | coefficient | term |
 *          hydraulic variable, resolved in that order); + - * / ^ with
 *          standard precedence, right-assoc ^; unary minus; parentheses;
 *          functions EXP LOG LOG10 SQRT ABS SGN STEP SIN COS TAN (unary),
 *          MIN MAX POW (binary, comma-separated). Term references are
 *          FORWARD-ONLY (a term may reference only earlier terms), so term
 *          evaluation is a simple in-order sweep with no cycle machinery.
 *
 * @see plans/transport/MULTISPECIES_REACTIONS_MSX_PLAN.md §3.2, §5 R2
 * @ingroup engine_transport
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_TRANSPORT_REACTION_EXPRESSION_HPP
#define OPENSWMM_ENGINE_TRANSPORT_REACTION_EXPRESSION_HPP

#include <string>
#include <vector>

#include "../../../data/ReactionTokens.hpp"

namespace openswmm::transport {

using openswmm::RxToken;
using openswmm::RxExprSpan;
using openswmm::RxHydVar;
using openswmm::kRxMaxStackDepth;

/// Name resolution offered to the compiler (pre-resolved at compile time —
/// the evaluator never sees names).
struct RxSymbols {
    const std::vector<std::string>* species = nullptr;  ///< local slots
    const std::vector<std::string>* coefs   = nullptr;
    const std::vector<std::string>* terms   = nullptr;  ///< only [0, max_term) resolvable
    /// R4: pollutant names, resolved AFTER terms and BEFORE hydraulic
    /// variables (a pollutant named "D" shadows the depth variable —
    /// user-defined beats builtin; documented in the DSL reference).
    /// Pollutants are READ-ONLY in expressions.
    const std::vector<std::string>* pollutants = nullptr;
    int max_term = 0;   ///< forward-only rule: terms below this index only
};

/// Evaluation environment: pre-gathered pointers, no lookups.
struct RxEvalEnv {
    const double* species = nullptr;  ///< local species concentrations
    const double* coefs   = nullptr;  ///< coefficient values
    const double* terms   = nullptr;  ///< term values (evaluated in-order)
    const double* hydvar  = nullptr;  ///< RxHydVar::COUNT_ entries
    const double* pollutants = nullptr;  ///< element pollutant conc (R4, read-only)
};

/**
 * @brief Compile `src` into the shared pool.
 * @param error_col [out] 1-based column of the offending token on failure.
 * @return empty on success, else the diagnostic ("undefined identifier
 *         'x'", "expression too deep", "term 'T' references later term", …).
 */
std::string compileReactionExpression(const std::string& src,
                                      const RxSymbols& symbols,
                                      std::vector<RxToken>& pool,
                                      RxExprSpan& out,
                                      int& error_col);

/// Tier-1 evaluation: one pass, fixed-depth stack, allocation-free.
double evalReactionExpression(const std::vector<RxToken>& pool,
                              const RxExprSpan& span,
                              const RxEvalEnv& env) noexcept;

}  // namespace openswmm::transport

#endif  // OPENSWMM_ENGINE_TRANSPORT_REACTION_EXPRESSION_HPP
