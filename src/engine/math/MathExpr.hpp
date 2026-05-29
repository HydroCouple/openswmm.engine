/**
 * @file MathExpr.hpp
 * @brief Mathematical expression parser and evaluator.
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
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
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
    NUMBER   = 0,
    VARIABLE = 1,
    ADD      = 2,
    SUB      = 3,
    MUL      = 4,
    DIV      = 5,
    POW      = 6,
    NEG      = 7,
    LPAREN   = 8,
    RPAREN   = 9,
    FUNC_ABS  = 10,
    FUNC_SGN  = 11,
    FUNC_SQRT = 12,
    FUNC_LOG  = 13,
    FUNC_EXP  = 14,
    FUNC_SIN  = 15,
    FUNC_COS  = 16,
    FUNC_TAN  = 17,
    FUNC_ASIN = 18,
    FUNC_ACOS = 19,
    FUNC_ATAN = 20,
    FUNC_STEP = 21,
    FUNC_MIN  = 22,
    FUNC_MAX  = 23,
    COMMA     = 24,
    // Legacy mathexpr.c functions not in original refactor (Gap 49)
    FUNC_COT   = 25,  ///< cot(x) = 1/tan(x)
    FUNC_SINH  = 26,  ///< sinh(x)
    FUNC_COSH  = 27,  ///< cosh(x)
    FUNC_TANH  = 28,  ///< tanh(x)
    FUNC_COTH  = 29,  ///< coth(x) = 1/tanh(x)
    FUNC_LOG10 = 30,  ///< log10(x)
    FUNC_ACOT  = 31   ///< acot(x) = atan(1/x)
};

struct Token {
    TokenType type = TokenType::NUMBER;
    double    value = 0.0;      ///< For NUMBER tokens
    int       var_idx = -1;     ///< For VARIABLE tokens (index into variable table)
    std::string var_name;       ///< For VARIABLE tokens (name for lookup)
};

// ============================================================================
// Compiled expression (postfix token list)
// ============================================================================

struct Expression {
    std::vector<Token> postfix;  ///< Postfix (RPN) token sequence
    bool valid = false;
};

// ============================================================================
// Parser (Shunting-yard)
// ============================================================================

/**
 * @brief Parse an infix expression string into a postfix Expression.
 *
 * @param expr_str  Infix expression (e.g., "x * exp(-k * t)").
 * @param result    [out] Compiled postfix expression.
 * @returns 0 on success, -1 on parse error.
 */
int parse(const std::string& expr_str, Expression& result);

// ============================================================================
// Variable binding (Tier 1 optimization)
// ============================================================================

/**
 * @brief Bind variable names in a compiled expression to integer indices.
 *
 * @details Resolves each VARIABLE token's var_name to a var_idx using the
 *          provided name→index table. After binding, evaluate_fast() can
 *          look up variables by index instead of by string comparison.
 *
 * @param expr       Expression to bind (modified in place).
 * @param name_table Array of variable names, indexed 0..n_vars-1.
 * @param n_vars     Number of entries in name_table.
 * @returns Number of variables successfully bound.
 */
int bind_variables(Expression& expr,
                   const char* const* name_table, int n_vars);

/**
 * @brief Compute the maximum stack depth needed to evaluate an expression.
 * @param expr  Compiled expression.
 * @returns Max stack depth (0 if expression is empty).
 */
int compute_max_stack_depth(const Expression& expr);

// ============================================================================
// Evaluator
// ============================================================================

/**
 * @brief Evaluate a compiled expression with named variable lookup.
 *
 * @param expr       Compiled expression.
 * @param var_lookup Function that maps variable name → value.
 * @returns Expression result.
 */
double evaluate(const Expression& expr,
                const std::function<double(const std::string&)>& var_lookup);

/**
 * @brief Evaluate with a flat variable array (by index).
 *
 * @param expr   Compiled expression.
 * @param vars   Variable values indexed by var_idx.
 * @param n_vars Size of vars array.
 * @returns Expression result.
 */
double evaluate(const Expression& expr, const double* vars, int n_vars);

/**
 * @brief Fast evaluate using pre-bound variable indices and stack-free evaluation.
 *
 * @details Uses a fixed-size stack array on the C stack (no heap allocation)
 *          and reads variables via vars[tok.var_idx] (no string comparison).
 *          Variables must be pre-bound via bind_variables().
 *
 * @param expr  Compiled expression with bound variable indices.
 * @param vars  Variable values array. vars[tok.var_idx] is read for each variable.
 * @returns Expression result, or 0.0 if expression is invalid.
 */
double evaluate_fast(const Expression& expr, const double* vars) noexcept;

} // namespace mathexpr
} // namespace openswmm

#endif // OPENSWMM_MATHEXPR_HPP
