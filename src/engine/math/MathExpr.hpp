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
 * @file      MathExpr.hpp
 * @brief     Mathematical expression parser and evaluator.
 *
 * @details Parses infix expressions (e.g., "C * exp(-0.1 * DT)") into
 *          postfix (RPN) token lists using the Shunting-yard algorithm.
 *          Evaluation uses a simple stack machine.
 *
 *          Used by:
 *          - Control rules (IF conditions with expressions)
 *          - Treatment functions (pollutant concentration formulas)
 *          - Groundwater custom deep flow expressions
 *
 * @note Legacy reference: src/legacy/engine/mathexpr.c
 * @ingroup engine_math
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_MATHEXPR_HPP
#define OPENSWMM_MATHEXPR_HPP

#include <string>
#include <vector>
#include <functional>

namespace openswmm {
namespace mathexpr {

// ============================================================================
// Token types
// ============================================================================

enum class TokenType : int {
    NUMBER = 0,
    VARIABLE = 1,
    ADD = 2,
    SUB = 3,
    MUL = 4,
    DIV = 5,
    POW = 6,
    NEG = 7,
    LPAREN = 8,
    RPAREN = 9,
    FUNC_ABS = 10,
    FUNC_SGN = 11,
    FUNC_SQRT = 12,
    FUNC_LOG = 13,
    FUNC_EXP = 14,
    FUNC_SIN = 15,
    FUNC_COS = 16,
    FUNC_TAN = 17,
    FUNC_ASIN = 18,
    FUNC_ACOS = 19,
    FUNC_ATAN = 20,
    FUNC_STEP = 21,
    FUNC_MIN = 22,
    FUNC_MAX = 23,
    COMMA = 24,
    // Legacy mathexpr.c functions not in original refactor (Gap 49)
    FUNC_COT = 25,   ///< cot(x) = 1/tan(x)
    FUNC_SINH = 26,  ///< sinh(x)
    FUNC_COSH = 27,  ///< cosh(x)
    FUNC_TANH = 28,  ///< tanh(x)
    FUNC_COTH = 29,  ///< coth(x) = 1/tanh(x)
    FUNC_LOG10 = 30, ///< log10(x)
    FUNC_ACOT = 31   ///< acot(x) = atan(1/x)
};

struct Token {
    TokenType type = TokenType::NUMBER;
    double value = 0.0;   ///< For NUMBER tokens
    int var_idx = -1;     ///< For VARIABLE tokens (index into variable table)
    std::string var_name; ///< For VARIABLE tokens (name for lookup)
};

// ============================================================================
// Compiled expression (postfix token list)
// ============================================================================

struct Expression {
    std::vector<Token> postfix; ///< Postfix (RPN) token sequence
    bool valid = false;
};

// ============================================================================
// Parser (Shunting-yard)
// ============================================================================

/**
 * @brief Tokenize an infix expression and construct its postfix representation.
 * @param[in] expr_str Expression text; built-in function names are
 * case-insensitive and variable names retain their original spelling.
 * @param[out] result Cleared before parsing. On success valid is true;
 * on failure valid is false and postfix may contain a partial expression.
 * @return Zero after conversion, or -1 for unmatched parentheses.
 * @throws std::invalid_argument A numeric token cannot be converted by stod.
 * @throws std::out_of_range A numeric token exceeds stod's representable range.
 * @note Conversion does not validate operand arity or reject every unknown
 * character. A successful return alone does not make arbitrary input safe
 * to evaluate; consumers must ensure a well-formed expression.
 */
int parse(const std::string& expr_str, Expression& result);

/**
 * @brief Return recognized built-in function names in stable token order.
 * @return Borrowed immutable process-lifetime vector of lowercase names:
 * abs, sgn, sqrt, log, exp, sin, cos, tan, asin, acos, atan, step, min, max,
 * cot, sinh, cosh, tanh, coth, log10, acot.
 * @note min and max take two operands; the other functions take one.
 * The caller must not delete or modify the returned storage.
 */
const std::vector<std::string>& function_names();

// ============================================================================
// Variable binding (Tier 1 optimization)
// ============================================================================

/**
 * @brief Bind each variable token to the first matching table entry.
 * @param[in,out] expr Expression whose variable indices are reset to -1
 * before resolution; variable names and postfix order remain unchanged.
 * @param[in] name_table Borrowed array of n_vars non-null, null-terminated
 * names. Each table name is matched as a case-insensitive prefix of the
 * expression's variable name; ordering determines which prefix wins.
 * @param[in] n_vars Nonnegative number of table entries.
 * @return Number of variable token occurrences bound, including repeated
 * uses of the same name. Unmatched tokens retain var_idx == -1.
 * @note Names and the table are not retained. This legacy prefix convention
 * is used by groundwater expressions; it is not an exact-name match.
 */
int bind_variables(Expression& expr, const char* const* name_table, int n_vars);

/**
 * @brief Compute the peak operand-stack occupancy implied by postfix tokens.
 * @param[in] expr Compiled expression; not modified or validated.
 * @return Maximum cumulative stack depth; zero for an empty sequence.
 * @note This counts stack effects only. It does not detect underflow,
 * unsupported fast-evaluator functions, or malformed operand counts.
 */
int compute_max_stack_depth(const Expression& expr);

// ============================================================================
// Evaluator
// ============================================================================

/**
 * @brief Evaluate postfix tokens with a synchronous named-variable callback.
 * @param[in] expr Well-formed postfix expression with sufficient operands
 * for every operation; the valid flag is not checked by this overload.
 * @param[in] var_lookup Nonempty callback returning a value for each
 * variable name, invoked once per occurrence. It is not retained.
 * @return Top stack value, or zero for an empty expression.
 * @details Division by zero, a negative square-root argument, and nonpositive
 * logarithm arguments produce zero. Other math domain and range behavior
 * follows the standard library. Callback and allocation exceptions propagate.
 * @pre Every postfix operation has enough operands; malformed input can
 * access an empty stack. Values use the caller's chosen units.
 */
double evaluate(const Expression& expr,
                const std::function<double(const std::string&)>& var_lookup);

/**
 * @brief Evaluate using variable indices stored in a compiled expression.
 * @param[in] expr Well-formed postfix expression; not modified. The first
 * variable token matching each name supplies the index for that name.
 * @param[in] vars Borrowed array of n_vars values, non-null whenever a bound
 * index in [0, n_vars) is read. No pointer is retained.
 * @param[in] n_vars Nonnegative number of available variable values.
 * @return Expression value; missing, negative, and out-of-range indices
 * contribute zero. Empty expressions evaluate to zero.
 * @pre Operand arity satisfies the named-callback evaluate() contract.
 * @see evaluate(const Expression&, const std::function<double(const std::string&)>&)
 */
double evaluate(const Expression& expr, const double* vars, int n_vars);

/**
 * @brief Evaluate the supported postfix subset with a fixed 32-value stack.
 * @param[in] expr Well-formed expression with bound variable indices and
 * maximum stack depth <= 32; not modified.
 * @param[in] vars Borrowed array large enough for every nonnegative var_idx;
 * non-null whenever such an index is read. Negative indices contribute zero.
 * @return Zero for invalid or empty expressions, otherwise the resulting value.
 * @pre Each operation has sufficient operands. Indices and stack bounds are
 * unchecked. Supported functions end at FUNC_MAX; cot, sinh, cosh, tanh,
 * coth, log10 and acot are not implemented by this fast evaluator.
 * @details Performs no heap allocation and retains no arrays. Division by
 * zero, negative square roots and nonpositive natural logarithms produce
 * zero, as in evaluate(). Other math operations follow standard-library
 * floating-point behavior. compute_max_stack_depth() can check the depth.
 */
double evaluate_fast(const Expression& expr, const double* vars) noexcept;

} // namespace mathexpr
} // namespace openswmm

#endif // OPENSWMM_MATHEXPR_HPP
