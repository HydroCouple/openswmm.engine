/**
 * @file Treatment.hpp
 * @brief Treatment expression evaluator for water quality.
 *
 * @details Evaluates user-defined treatment expressions at nodes to modify
 *          pollutant concentrations. Expressions can reference:
 *          - C (current concentration)
 *          - R (removal fraction)
 *          - DT (timestep in seconds)
 *          - HRT (hydraulic residence time in hours)
 *          - Q (flow rate)
 *          - V (volume)
 *          - D (depth)
 *
 *          Treatment expression strings have the form:
 *            "R = <expression>"  — removal equation
 *            "C = <expression>"  — concentration equation
 *
 *          Expressions are parsed at input time into a postfix token list
 *          using the Shunting-yard algorithm. Evaluation is a simple stack
 *          machine — vectorisable across nodes when all nodes use the same
 *          expression (batch eval with different variable values).
 *
 * @note Legacy reference: src/legacy/engine/treatmnt.c, mathexpr.c
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_TREATMENT_HPP
#define OPENSWMM_TREATMENT_HPP

#include <string>
#include <vector>

namespace openswmm {

namespace treatment {

// ============================================================================
// Expression token types
// ============================================================================

enum class TokenType : int {
    NUMBER    = 0,
    VARIABLE  = 1,
    ADD       = 2,
    SUB       = 3,
    MUL       = 4,
    DIV       = 5,
    POW       = 6,
    NEG       = 7,   ///< Unary negation
    FUNC_EXP  = 8,
    FUNC_LOG  = 9,
    FUNC_SQRT = 10,
    FUNC_MIN  = 11,
    FUNC_MAX  = 12,
    FUNC_ABS  = 13,
    FUNC_SGN  = 14,
    FUNC_STEP = 15,  ///< Heaviside step
    LPAREN    = 16,  ///< Left parenthesis (parser internal, never in output)
    RPAREN    = 17,  ///< Right parenthesis (parser internal, never in output)
    COMMA     = 18   ///< Comma separator for min/max (parser internal)
};

enum class TreatVar : int {
    C        = 0,   ///< Current concentration (this pollutant)
    R        = 1,   ///< Removal fraction (this pollutant, unused in expressions)
    DT       = 2,   ///< Timestep (sec)
    HRT      = 3,   ///< Hydraulic residence time (hours, per legacy convention)
    Q        = 4,   ///< Flow rate
    V        = 5,   ///< Volume
    D        = 6,   ///< Depth
    C_POLLUT = 7,   ///< Concentration of another pollutant (uses pollut_ref)
    R_POLLUT = 8    ///< Removal fraction of another pollutant (uses pollut_ref, co-treatment)
};

// ============================================================================
// Expression token
// ============================================================================

struct Token {
    TokenType type = TokenType::NUMBER;
    double    value = 0.0;        ///< For NUMBER tokens
    TreatVar  var = TreatVar::C;  ///< For VARIABLE tokens
    int       pollut_ref = -1;    ///< Pollutant index for C_POLLUT/R_POLLUT
};

// ============================================================================
// Treatment expression (parsed, ready to evaluate)
// ============================================================================

struct TreatExpr {
    std::vector<Token> tokens;   ///< Postfix (RPN) token list
    int pollutant_idx = -1;      ///< Which pollutant this expression applies to
    bool is_removal = false;     ///< True if expression computes R (removal), false if C
};

// ============================================================================
// Functions
// ============================================================================

/**
 * @brief Parse a treatment expression string into postfix tokens.
 *
 * @details The expression string should be of the form "C = ..." or "R = ...".
 *          The parser determines whether this is a concentration or removal
 *          equation from the left-hand side, then parses the right-hand side
 *          (after the '=') into a postfix token sequence using the
 *          Shunting-yard algorithm.
 *
 *          Supported operators: + - * / ^ (standard precedence)
 *          Supported functions: exp, log, sqrt, min, max, abs, sgn, step
 *          Supported variables: C, R, DT, HRT, Q, V, D
 *
 * @param expr_str  Full expression string (e.g., "R = 1.0 - exp(-0.5 * HRT)").
 * @param result    [out] Parsed expression with is_removal set.
 * @returns 0 on success, -1 on parse error.
 */
int parse(const std::string& expr_str, TreatExpr& result);

/**
 * @brief Parse with pollutant name resolution for co-treatment.
 *
 * @param expr_str     Full expression string.
 * @param result       [out] Parsed expression.
 * @param pollut_names Pollutant name → index resolver. Returns -1 if not found.
 * @returns 0 on success, -1 on parse error.
 */
int parse(const std::string& expr_str, TreatExpr& result,
          int (*pollut_lookup)(const std::string& name));

/**
 * @brief Evaluate a treatment expression with given variable values.
 *
 * @param expr   Parsed expression.
 * @param c      Current concentration.
 * @param dt     Timestep (sec).
 * @param hrt    Hydraulic residence time (hours).
 * @param q      Flow rate.
 * @param v      Volume.
 * @param d      Depth.
 * @returns Computed value (concentration or removal fraction).
 */
double evaluate(const TreatExpr& expr, double c, double dt,
                double hrt, double q, double v, double d);

/**
 * @brief Evaluate with co-treatment support (R_pollutant references).
 *
 * @param expr       Parsed expression.
 * @param c          Concentration for this pollutant.
 * @param dt         Timestep (sec).
 * @param hrt        HRT (hours).
 * @param q          Flow rate.
 * @param v          Volume.
 * @param d          Depth.
 * @param cin        Inflow concentrations for all pollutants.
 * @param removal    Removal fractions for all pollutants (-1=not computed).
 * @param n_pollut   Number of pollutants.
 * @returns Computed value.
 */
double evaluate(const TreatExpr& expr, double c, double dt,
                double hrt, double q, double v, double d,
                const double* cin, const double* removal, int n_pollut);

/**
 * @brief Apply treatment at a node for one pollutant.
 *
 * @details For a removal equation (R=): the expression result is clamped to
 *          [0,1] and the output concentration is c_in * (1 - R).
 *          For a concentration equation (C=): the expression result is clamped
 *          to [0, c_in] and the output concentration is the expression result.
 *          This matches the legacy treatmnt.c behavior.
 *
 * @param expr    Treatment expression.
 * @param c_in    Inflow/current concentration (Cin for R=, node conc for C=).
 * @param dt      Timestep (sec).
 * @param hrt     Hydraulic residence time (hours).
 * @param q       Flow.
 * @param v       Volume.
 * @param d       Depth.
 * @returns Treated concentration.
 */
double applyTreatment(const TreatExpr& expr, double c_in, double dt,
                      double hrt, double q, double v, double d);

} // namespace treatment
} // namespace openswmm

#endif // OPENSWMM_TREATMENT_HPP
