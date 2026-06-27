/**
 * @file Treatment.cpp
 * @brief Treatment expression parser (Shunting-yard) and evaluator (stack machine).
 *
 * @details The parser converts infix treatment expressions into postfix (RPN)
 *          token lists. The evaluator is a simple stack machine that processes
 *          the postfix token list with concrete variable values.
 *
 *          Numerically identical to the legacy treatmnt.c + mathexpr.c chain.
 *
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "Treatment.hpp"
#include <cmath>
#include <stack>
#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace openswmm {
namespace treatment {

// ============================================================================
// Parser helpers
// ============================================================================

/// Operator precedence (higher binds tighter).
static int precedence(TokenType t) {
    switch (t) {
        case TokenType::ADD: case TokenType::SUB: return 1;
        case TokenType::MUL: case TokenType::DIV: return 2;
        case TokenType::POW: return 3;
        case TokenType::NEG: return 4;
        default: return 0;
    }
}

/// True if the token type is a function (unary or binary like min/max).
static bool isFunction(TokenType t) {
    int v = static_cast<int>(t);
    return v >= static_cast<int>(TokenType::FUNC_EXP) &&
           v <= static_cast<int>(TokenType::FUNC_STEP);
}

/// True if the token type is a binary or unary arithmetic operator.
static bool isOperator(TokenType t) {
    return t == TokenType::ADD || t == TokenType::SUB ||
           t == TokenType::MUL || t == TokenType::DIV ||
           t == TokenType::POW || t == TokenType::NEG;
}

/// Right-associative operators.
static bool isRightAssoc(TokenType t) {
    return t == TokenType::POW || t == TokenType::NEG;
}

// Map of function names (lowercase) to token types.
static const std::unordered_map<std::string, TokenType> func_map = {
    {"exp",  TokenType::FUNC_EXP},
    {"log",  TokenType::FUNC_LOG},
    {"ln",   TokenType::FUNC_LOG},
    {"sqrt", TokenType::FUNC_SQRT},
    {"min",  TokenType::FUNC_MIN},
    {"max",  TokenType::FUNC_MAX},
    {"abs",  TokenType::FUNC_ABS},
    {"sgn",  TokenType::FUNC_SGN},
    {"step", TokenType::FUNC_STEP},
};

// Map of variable names (uppercase) to TreatVar.
static const std::unordered_map<std::string, TreatVar> var_map = {
    {"C",    TreatVar::C},
    {"R",    TreatVar::R},
    {"DT",   TreatVar::DT},
    {"HRT",  TreatVar::HRT},
    {"Q",    TreatVar::Q},
    {"V",    TreatVar::V},
    {"D",    TreatVar::D},
    {"AREA", TreatVar::AREA},  // Gap #16: legacy pvAREA
};

// ============================================================================
// Tokenizer
// ============================================================================

/// Tokenize an expression string into a flat token list (infix order).
/// Variables and functions are recognized by name lookup.
/// If pollut_lookup is non-null, R_xxx and C_xxx are resolved to pollutant refs.
static std::vector<Token> tokenize(const std::string& s,
                                    int (*pollut_lookup)(const std::string&) = nullptr) {
    std::vector<Token> tokens;
    size_t i = 0;
    while (i < s.size()) {
        char c = s[i];

        // Skip whitespace
        if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }

        // Numeric literal (integer, decimal, scientific notation)
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
            size_t start = i;
            while (i < s.size() &&
                   (std::isdigit(static_cast<unsigned char>(s[i])) ||
                    s[i] == '.' || s[i] == 'e' || s[i] == 'E' ||
                    ((s[i] == '+' || s[i] == '-') && i > 0 &&
                     (s[i-1] == 'e' || s[i-1] == 'E'))))
                ++i;
            Token t;
            t.type  = TokenType::NUMBER;
            t.value = std::stod(s.substr(start, i - start));
            tokens.push_back(t);
            continue;
        }

        // Identifier: function name or variable name
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            size_t start = i;
            while (i < s.size() &&
                   (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_'))
                ++i;
            std::string word = s.substr(start, i - start);

            // Check function names (case-insensitive)
            std::string lower = word;
            for (auto& ch : lower)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

            auto fit = func_map.find(lower);
            if (fit != func_map.end()) {
                Token t;
                t.type = fit->second;
                tokens.push_back(t);
                continue;
            }

            // Check variable names (case-insensitive)
            std::string upper = word;
            for (auto& ch : upper)
                ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));

            auto vit = var_map.find(upper);
            if (vit != var_map.end()) {
                Token t;
                t.type = TokenType::VARIABLE;
                t.var  = vit->second;
                tokens.push_back(t);
            } else if (pollut_lookup && upper.size() > 2 && upper[1] == '_') {
                // Co-treatment: R_pollutant or C_pollutant reference
                char prefix = upper[0];
                std::string pollut_name = word.substr(2);
                int pi = pollut_lookup(pollut_name);
                if (pi >= 0) {
                    Token t;
                    t.type = TokenType::VARIABLE;
                    t.var = (prefix == 'R') ? TreatVar::R_POLLUT : TreatVar::C_POLLUT;
                    t.pollut_ref = pi;
                    tokens.push_back(t);
                } else {
                    return {};  // unknown pollutant name
                }
            } else {
                return {};  // unknown identifier
            }
            continue;
        }

        // Single-character tokens
        {
            Token t;
            switch (c) {
                case '+': t.type = TokenType::ADD; break;
                case '-':
                    // Unary minus: at start, after operator, after '(' or ','
                    if (tokens.empty() ||
                        tokens.back().type == TokenType::LPAREN ||
                        isOperator(tokens.back().type) ||
                        tokens.back().type == TokenType::COMMA) {
                        t.type = TokenType::NEG;
                    } else {
                        t.type = TokenType::SUB;
                    }
                    break;
                case '*': t.type = TokenType::MUL; break;
                case '/': t.type = TokenType::DIV; break;
                case '^': t.type = TokenType::POW; break;
                case '(': t.type = TokenType::LPAREN; break;
                case ')': t.type = TokenType::RPAREN; break;
                case ',': t.type = TokenType::COMMA; break;
                default:
                    // Skip unknown characters
                    ++i;
                    continue;
            }
            tokens.push_back(t);
            ++i;
        }
    }
    return tokens;
}

// ============================================================================
// Shunting-yard: infix tokens → postfix tokens
// ============================================================================

/// Convert infix token list to postfix (RPN) using the Shunting-yard algorithm.
/// Returns 0 on success, -1 on error (mismatched parentheses, etc.).
static int shuntingYard(const std::vector<Token>& infix,
                        std::vector<Token>& postfix) {
    postfix.clear();
    std::stack<Token> ops;

    for (const auto& tok : infix) {
        if (tok.type == TokenType::NUMBER || tok.type == TokenType::VARIABLE) {
            postfix.push_back(tok);
        }
        else if (isFunction(tok.type)) {
            ops.push(tok);
        }
        else if (tok.type == TokenType::COMMA) {
            // Pop operators until left paren (for min/max arguments)
            while (!ops.empty() && ops.top().type != TokenType::LPAREN) {
                postfix.push_back(ops.top());
                ops.pop();
            }
            if (ops.empty()) return -1;  // mismatched comma outside parens
        }
        else if (isOperator(tok.type)) {
            while (!ops.empty() && isOperator(ops.top().type) &&
                   ((isRightAssoc(tok.type) &&
                     precedence(tok.type) < precedence(ops.top().type)) ||
                    (!isRightAssoc(tok.type) &&
                     precedence(tok.type) <= precedence(ops.top().type)))) {
                postfix.push_back(ops.top());
                ops.pop();
            }
            ops.push(tok);
        }
        else if (tok.type == TokenType::LPAREN) {
            ops.push(tok);
        }
        else if (tok.type == TokenType::RPAREN) {
            while (!ops.empty() && ops.top().type != TokenType::LPAREN) {
                postfix.push_back(ops.top());
                ops.pop();
            }
            if (ops.empty()) return -1;  // mismatched right paren
            ops.pop();  // discard LPAREN
            // If top of stack is a function, pop it to output
            if (!ops.empty() && isFunction(ops.top().type)) {
                postfix.push_back(ops.top());
                ops.pop();
            }
        }
    }

    // Pop remaining operators
    while (!ops.empty()) {
        if (ops.top().type == TokenType::LPAREN) return -1;  // mismatched
        postfix.push_back(ops.top());
        ops.pop();
    }

    return 0;
}

// ============================================================================
// Public parse function
// ============================================================================

int parse(const std::string& expr_str, TreatExpr& result) {
    result.tokens.clear();
    result.is_removal = false;

    if (expr_str.empty()) return -1;

    // --- Find the '=' sign that separates LHS from RHS
    size_t eq_pos = expr_str.find('=');
    if (eq_pos == std::string::npos) return -1;

    // --- Determine equation type from LHS (everything before '=')
    //     LHS should be "R" or "C" (with optional whitespace)
    std::string lhs = expr_str.substr(0, eq_pos);
    // Trim whitespace from LHS
    size_t lhs_start = lhs.find_first_not_of(" \t\r\n");
    size_t lhs_end   = lhs.find_last_not_of(" \t\r\n");
    if (lhs_start == std::string::npos) return -1;
    lhs = lhs.substr(lhs_start, lhs_end - lhs_start + 1);

    // Convert LHS to uppercase for comparison
    char lhs_char = static_cast<char>(
        std::toupper(static_cast<unsigned char>(lhs[0])));

    if (lhs.size() == 1 && lhs_char == 'R') {
        result.is_removal = true;
    } else if (lhs.size() == 1 && lhs_char == 'C') {
        result.is_removal = false;
    } else {
        return -1;  // LHS must be exactly "R" or "C"
    }

    // --- Extract RHS (everything after '=')
    std::string rhs = expr_str.substr(eq_pos + 1);

    // --- Tokenize the RHS
    auto infix_tokens = tokenize(rhs);
    if (infix_tokens.empty() && !rhs.empty()) {
        // Tokenizer returned empty due to unknown identifier
        return -1;
    }
    if (infix_tokens.empty()) return -1;

    // --- Convert infix to postfix via Shunting-yard
    if (shuntingYard(infix_tokens, result.tokens) != 0) {
        result.tokens.clear();
        return -1;
    }

    return 0;
}

// ============================================================================
// Evaluator — postfix stack machine
// ============================================================================

double evaluate(const TreatExpr& expr, double c, double dt,
                double hrt, double q, double v, double d, double area) {
    std::stack<double> stk;

    for (const auto& tok : expr.tokens) {
        switch (tok.type) {
            case TokenType::NUMBER:
                stk.push(tok.value);
                break;

            case TokenType::VARIABLE:
                switch (tok.var) {
                    case TreatVar::C:        stk.push(c);    break;
                    case TreatVar::R:        stk.push(0.0);  break;
                    case TreatVar::DT:       stk.push(dt);   break;
                    case TreatVar::HRT:      stk.push(hrt);  break;
                    case TreatVar::Q:        stk.push(q);    break;
                    case TreatVar::V:        stk.push(v);    break;
                    case TreatVar::D:        stk.push(d);    break;
                    case TreatVar::AREA:     stk.push(area); break;  // Gap #16
                    case TreatVar::C_POLLUT: stk.push(0.0);  break;
                    case TreatVar::R_POLLUT: stk.push(0.0);  break;
                }
                break;

            case TokenType::NEG: {
                if (stk.empty()) return 0.0;
                double a = stk.top(); stk.pop();
                stk.push(-a);
                break;
            }

            case TokenType::ADD: case TokenType::SUB:
            case TokenType::MUL: case TokenType::DIV:
            case TokenType::POW: {
                if (stk.size() < 2) return 0.0;
                double b = stk.top(); stk.pop();
                double a = stk.top(); stk.pop();
                switch (tok.type) {
                    case TokenType::ADD: stk.push(a + b); break;
                    case TokenType::SUB: stk.push(a - b); break;
                    case TokenType::MUL: stk.push(a * b); break;
                    case TokenType::DIV: stk.push(b != 0.0 ? a / b : 0.0); break;
                    case TokenType::POW: stk.push(std::pow(a, b)); break;
                    default: break;
                }
                break;
            }

            case TokenType::FUNC_EXP: {
                if (stk.empty()) return 0.0;
                double a = stk.top(); stk.pop();
                stk.push(std::exp(a));
                break;
            }
            case TokenType::FUNC_LOG: {
                if (stk.empty()) return 0.0;
                double a = stk.top(); stk.pop();
                stk.push(a > 0.0 ? std::log(a) : 0.0);
                break;
            }
            case TokenType::FUNC_SQRT: {
                if (stk.empty()) return 0.0;
                double a = stk.top(); stk.pop();
                stk.push(a >= 0.0 ? std::sqrt(a) : 0.0);
                break;
            }
            case TokenType::FUNC_ABS: {
                if (stk.empty()) return 0.0;
                double a = stk.top(); stk.pop();
                stk.push(std::fabs(a));
                break;
            }
            case TokenType::FUNC_SGN: {
                if (stk.empty()) return 0.0;
                double a = stk.top(); stk.pop();
                stk.push(a > 0.0 ? 1.0 : (a < 0.0 ? -1.0 : 0.0));
                break;
            }
            case TokenType::FUNC_STEP: {
                if (stk.empty()) return 0.0;
                double a = stk.top(); stk.pop();
                stk.push(a >= 0.0 ? 1.0 : 0.0);
                break;
            }
            case TokenType::FUNC_MIN: {
                if (stk.size() < 2) return 0.0;
                double b = stk.top(); stk.pop();
                double a = stk.top(); stk.pop();
                stk.push(std::min(a, b));
                break;
            }
            case TokenType::FUNC_MAX: {
                if (stk.size() < 2) return 0.0;
                double b = stk.top(); stk.pop();
                double a = stk.top(); stk.pop();
                stk.push(std::max(a, b));
                break;
            }

            // Parser-internal tokens should never appear in postfix output
            case TokenType::LPAREN:
            case TokenType::RPAREN:
            case TokenType::COMMA:
                break;
        }
    }

    return stk.empty() ? 0.0 : stk.top();
}

// ============================================================================
// Apply treatment — matches legacy treatmnt.c getRemoval() + treatmnt_treat()
// ============================================================================

double applyTreatment(const TreatExpr& expr, double c_in, double dt,
                      double hrt, double q, double v, double d, double area) {
    double result = evaluate(expr, c_in, dt, hrt, q, v, d, area);

    // Clamp result to non-negative (legacy: r = MAX(0.0, r))
    result = std::max(0.0, result);

    if (expr.is_removal) {
        // Legacy R= equation: R is clamped to [0,1], then c_out = (1 - R) * c_in
        result = std::min(1.0, result);
        return c_in * (1.0 - result);
    } else {
        // Legacy C= equation: concentration is clamped to [0, c_in]
        // In legacy: r = MIN(c0, r); R[p] = 1.0 - r/c0
        // then later: cOut = (1.0 - R[p]) * Node[j].newQual[p]
        // which simplifies to: cOut = r (the clamped expression result)
        result = std::min(c_in, result);
        return result;
    }
}

// ============================================================================
// Extended parse with pollutant name resolution (co-treatment)
// ============================================================================

int parse(const std::string& expr_str, TreatExpr& result,
          int (*pollut_lookup)(const std::string& name)) {
    result.tokens.clear();
    result.pollutant_idx = -1;
    result.is_removal = false;

    if (expr_str.empty()) return -1;

    // Determine treatment type from first non-space character
    size_t eq = expr_str.find('=');
    if (eq == std::string::npos || eq == 0) return -1;

    char lhs = static_cast<char>(std::toupper(static_cast<unsigned char>(expr_str[0])));
    if (lhs == 'R') result.is_removal = true;
    else if (lhs == 'C') result.is_removal = false;
    else return -1;

    std::string rhs = expr_str.substr(eq + 1);

    // Tokenize with pollutant lookup
    auto tokens = tokenize(rhs, pollut_lookup);
    if (tokens.empty() && !rhs.empty()) return -1;

    // Convert infix to postfix via shunting-yard
    if (shuntingYard(tokens, result.tokens) != 0) return -1;

    return 0;
}

// ============================================================================
// Extended evaluate with co-treatment support
// ============================================================================

double evaluate(const TreatExpr& expr, double c, double dt,
                double hrt, double q, double v, double d,
                const double* cin, const double* removal, int n_pollut,
                double area) {
    std::stack<double> stk;

    for (const auto& tok : expr.tokens) {
        switch (tok.type) {
            case TokenType::NUMBER:
                stk.push(tok.value);
                break;

            case TokenType::VARIABLE:
                switch (tok.var) {
                    case TreatVar::C:     stk.push(c);    break;
                    case TreatVar::R:     stk.push(0.0);  break;
                    case TreatVar::DT:    stk.push(dt);   break;
                    case TreatVar::HRT:   stk.push(hrt);  break;
                    case TreatVar::Q:     stk.push(q);    break;
                    case TreatVar::V:     stk.push(v);    break;
                    case TreatVar::D:     stk.push(d);    break;
                    case TreatVar::AREA:  stk.push(area); break;  // Gap #16
                    case TreatVar::C_POLLUT:
                        if (cin && tok.pollut_ref >= 0 && tok.pollut_ref < n_pollut)
                            stk.push(cin[tok.pollut_ref]);
                        else
                            stk.push(0.0);
                        break;
                    case TreatVar::R_POLLUT:
                        if (removal && tok.pollut_ref >= 0 && tok.pollut_ref < n_pollut)
                            stk.push(std::max(0.0, removal[tok.pollut_ref]));
                        else
                            stk.push(0.0);
                        break;
                }
                break;

            case TokenType::NEG: {
                if (stk.empty()) return 0.0;
                double a = stk.top(); stk.pop();
                stk.push(-a);
                break;
            }

            case TokenType::ADD: case TokenType::SUB:
            case TokenType::MUL: case TokenType::DIV:
            case TokenType::POW: {
                if (stk.size() < 2) return 0.0;
                double b = stk.top(); stk.pop();
                double a = stk.top(); stk.pop();
                switch (tok.type) {
                    case TokenType::ADD: stk.push(a + b); break;
                    case TokenType::SUB: stk.push(a - b); break;
                    case TokenType::MUL: stk.push(a * b); break;
                    case TokenType::DIV: stk.push(b != 0.0 ? a / b : 0.0); break;
                    case TokenType::POW: stk.push(std::pow(a, b)); break;
                    default: break;
                }
                break;
            }

            case TokenType::FUNC_EXP: case TokenType::FUNC_LOG:
            case TokenType::FUNC_SQRT: case TokenType::FUNC_ABS:
            case TokenType::FUNC_SGN: case TokenType::FUNC_STEP: {
                if (stk.empty()) return 0.0;
                double a = stk.top(); stk.pop();
                switch (tok.type) {
                    case TokenType::FUNC_EXP:  stk.push(std::exp(a)); break;
                    case TokenType::FUNC_LOG:  stk.push(a > 0 ? std::log(a) : 0.0); break;
                    case TokenType::FUNC_SQRT: stk.push(a >= 0 ? std::sqrt(a) : 0.0); break;
                    case TokenType::FUNC_ABS:  stk.push(std::abs(a)); break;
                    case TokenType::FUNC_SGN:  stk.push(a > 0 ? 1.0 : (a < 0 ? -1.0 : 0.0)); break;
                    case TokenType::FUNC_STEP: stk.push(a > 0 ? 1.0 : 0.0); break;
                    default: break;
                }
                break;
            }

            case TokenType::FUNC_MIN: case TokenType::FUNC_MAX: {
                if (stk.size() < 2) return 0.0;
                double b = stk.top(); stk.pop();
                double a = stk.top(); stk.pop();
                stk.push(tok.type == TokenType::FUNC_MIN ? std::min(a, b) : std::max(a, b));
                break;
            }

            default: break;
        }
    }

    return stk.empty() ? 0.0 : stk.top();
}

} // namespace treatment
} // namespace openswmm
