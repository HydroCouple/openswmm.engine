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
 * @file ReactionTokens.hpp
 * @brief POD reaction-VM instruction + span types (phase R2, D-L3 flat
 *        pool). Data-layer header: no dependencies, included by both
 *        ReactionData (which owns the pool) and the transport-side
 *        compiler/evaluator (ReactionExpression).
 *
 * @ingroup engine_data
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_DATA_REACTION_TOKENS_HPP
#define OPENSWMM_ENGINE_DATA_REACTION_TOKENS_HPP

#include <cstdint>

namespace openswmm {

/// Hydraulic variables available in reaction expressions (reactions plan
/// §2; engines populate the evaluation environment at R6). Order is the
/// ABI of the evaluator's hydvar array.
enum class RxHydVar : int {
    D = 0,    ///< depth (ft)
    Q,        ///< flow (cfs)
    U,        ///< velocity (ft/s)
    RE,       ///< Reynolds number
    US,       ///< shear velocity (ft/s)
    FF,       ///< Darcy-Weisbach friction factor
    AV,       ///< wetted surface area per volume (1/ft)
    HRT,      ///< hydraulic residence time (s)
    DT,       ///< reaction step (s)
    COUNT_
};

/// One VM instruction. POD, contiguous in the shared pool — no
/// per-expression heap vectors (LARD plan §16 D-L3).
struct RxToken {
    enum Op : uint8_t {
        PUSH_NUM = 0,   ///< value
        PUSH_SPECIES,   ///< idx = local species slot
        PUSH_COEF,      ///< idx = coefficient slot
        PUSH_TERM,      ///< idx = term slot (evaluated earlier in-order)
        PUSH_HYDVAR,    ///< idx = RxHydVar
        ADD, SUB, MUL, DIV, POW, NEG,
        F_EXP, F_LOG, F_LOG10, F_SQRT, F_ABS, F_SGN, F_STEP,
        F_SIN, F_COS, F_TAN,
        F_MIN, F_MAX    ///< binary
    };
    uint8_t  op    = PUSH_NUM;
    int32_t  idx   = 0;
    double   value = 0.0;
};

/// Compiled expression = span into the shared pool. len 0 ⇒ no expression.
struct RxExprSpan {
    int32_t begin = 0;
    int32_t len   = 0;
};

/// Maximum operand-stack depth; the compiler rejects deeper expressions so
/// the evaluator's fixed stack can never overflow.
inline constexpr int kRxMaxStackDepth = 32;

}  // namespace openswmm

#endif  // OPENSWMM_ENGINE_DATA_REACTION_TOKENS_HPP
