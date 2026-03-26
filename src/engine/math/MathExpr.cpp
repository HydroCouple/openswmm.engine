/**
 * @file MathExpr.cpp
 * @brief Shunting-yard parser + stack evaluator.
 * @ingroup engine_math
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "MathExpr.hpp"
#include <cmath>
#include <stack>
#include <cctype>
#include <algorithm>
#include <unordered_map>

namespace openswmm {
namespace mathexpr {

// ============================================================================
// Operator precedence
// ============================================================================

static int precedence(TokenType t) {
    switch (t) {
        case TokenType::ADD: case TokenType::SUB: return 1;
        case TokenType::MUL: case TokenType::DIV: return 2;
        case TokenType::POW: return 3;
        case TokenType::NEG: return 4;
        default: return 0;
    }
}

static bool isFunction(TokenType t) {
    return static_cast<int>(t) >= static_cast<int>(TokenType::FUNC_ABS);
}

static bool isOperator(TokenType t) {
    return t == TokenType::ADD || t == TokenType::SUB ||
           t == TokenType::MUL || t == TokenType::DIV ||
           t == TokenType::POW || t == TokenType::NEG;
}

static bool isRightAssoc(TokenType t) {
    return t == TokenType::POW || t == TokenType::NEG;
}

// ============================================================================
// Tokenizer
// ============================================================================

static const std::unordered_map<std::string, TokenType> func_map = {
    {"abs", TokenType::FUNC_ABS}, {"sgn", TokenType::FUNC_SGN},
    {"sqrt", TokenType::FUNC_SQRT}, {"log", TokenType::FUNC_LOG},
    {"exp", TokenType::FUNC_EXP}, {"sin", TokenType::FUNC_SIN},
    {"cos", TokenType::FUNC_COS}, {"tan", TokenType::FUNC_TAN},
    {"asin", TokenType::FUNC_ASIN}, {"acos", TokenType::FUNC_ACOS},
    {"atan", TokenType::FUNC_ATAN}, {"step", TokenType::FUNC_STEP},
    {"min", TokenType::FUNC_MIN}, {"max", TokenType::FUNC_MAX},
};

static std::vector<Token> tokenize(const std::string& s) {
    std::vector<Token> tokens;
    size_t i = 0;
    while (i < s.size()) {
        char c = s[i];
        if (std::isspace(c)) { ++i; continue; }

        if (std::isdigit(c) || c == '.') {
            size_t start = i;
            while (i < s.size() && (std::isdigit(s[i]) || s[i] == '.' || s[i] == 'e' || s[i] == 'E' ||
                   ((s[i] == '+' || s[i] == '-') && i > 0 && (s[i-1] == 'e' || s[i-1] == 'E'))))
                ++i;
            Token t; t.type = TokenType::NUMBER;
            t.value = std::stod(s.substr(start, i - start));
            tokens.push_back(t);
        }
        else if (std::isalpha(c) || c == '_') {
            size_t start = i;
            while (i < s.size() && (std::isalnum(s[i]) || s[i] == '_')) ++i;
            std::string word = s.substr(start, i - start);
            std::string lower = word;
            for (auto& ch : lower) ch = static_cast<char>(std::tolower(ch));

            auto it = func_map.find(lower);
            if (it != func_map.end()) {
                Token t; t.type = it->second;
                tokens.push_back(t);
            } else {
                Token t; t.type = TokenType::VARIABLE;
                t.var_name = word;
                tokens.push_back(t);
            }
        }
        else {
            Token t;
            switch (c) {
                case '+': t.type = TokenType::ADD; break;
                case '-':
                    // Unary minus if at start or after operator/lparen
                    if (tokens.empty() || tokens.back().type == TokenType::LPAREN ||
                        isOperator(tokens.back().type) || tokens.back().type == TokenType::COMMA) {
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
                default: ++i; continue;
            }
            tokens.push_back(t);
            ++i;
        }
    }
    return tokens;
}

// ============================================================================
// Shunting-yard → postfix
// ============================================================================

int parse(const std::string& expr_str, Expression& result) {
    result.postfix.clear();
    result.valid = false;

    auto tokens = tokenize(expr_str);
    std::stack<Token> ops;

    for (const auto& tok : tokens) {
        if (tok.type == TokenType::NUMBER || tok.type == TokenType::VARIABLE) {
            result.postfix.push_back(tok);
        }
        else if (isFunction(tok.type)) {
            ops.push(tok);
        }
        else if (tok.type == TokenType::COMMA) {
            while (!ops.empty() && ops.top().type != TokenType::LPAREN) {
                result.postfix.push_back(ops.top()); ops.pop();
            }
        }
        else if (isOperator(tok.type)) {
            while (!ops.empty() && isOperator(ops.top().type) &&
                   ((isRightAssoc(tok.type) && precedence(tok.type) < precedence(ops.top().type)) ||
                    (!isRightAssoc(tok.type) && precedence(tok.type) <= precedence(ops.top().type)))) {
                result.postfix.push_back(ops.top()); ops.pop();
            }
            ops.push(tok);
        }
        else if (tok.type == TokenType::LPAREN) {
            ops.push(tok);
        }
        else if (tok.type == TokenType::RPAREN) {
            while (!ops.empty() && ops.top().type != TokenType::LPAREN) {
                result.postfix.push_back(ops.top()); ops.pop();
            }
            if (ops.empty()) return -1;  // mismatched parens
            ops.pop();  // pop LPAREN
            if (!ops.empty() && isFunction(ops.top().type)) {
                result.postfix.push_back(ops.top()); ops.pop();
            }
        }
    }

    while (!ops.empty()) {
        if (ops.top().type == TokenType::LPAREN) return -1;
        result.postfix.push_back(ops.top()); ops.pop();
    }

    result.valid = true;
    return 0;
}

// ============================================================================
// Evaluator
// ============================================================================

double evaluate(const Expression& expr,
                const std::function<double(const std::string&)>& var_lookup) {
    std::stack<double> stk;

    for (const auto& tok : expr.postfix) {
        switch (tok.type) {
            case TokenType::NUMBER:
                stk.push(tok.value); break;
            case TokenType::VARIABLE:
                stk.push(var_lookup(tok.var_name)); break;
            case TokenType::NEG: {
                double a = stk.top(); stk.pop(); stk.push(-a); break;
            }
            case TokenType::ADD: case TokenType::SUB:
            case TokenType::MUL: case TokenType::DIV:
            case TokenType::POW: {
                double b = stk.top(); stk.pop();
                double a = stk.top(); stk.pop();
                switch (tok.type) {
                    case TokenType::ADD: stk.push(a+b); break;
                    case TokenType::SUB: stk.push(a-b); break;
                    case TokenType::MUL: stk.push(a*b); break;
                    case TokenType::DIV: stk.push(b!=0?a/b:0); break;
                    case TokenType::POW: stk.push(std::pow(a,b)); break;
                    default: break;
                }
                break;
            }
            case TokenType::FUNC_ABS:  { double a=stk.top(); stk.pop(); stk.push(std::fabs(a)); break; }
            case TokenType::FUNC_SQRT: { double a=stk.top(); stk.pop(); stk.push(a>=0?std::sqrt(a):0); break; }
            case TokenType::FUNC_EXP:  { double a=stk.top(); stk.pop(); stk.push(std::exp(a)); break; }
            case TokenType::FUNC_LOG:  { double a=stk.top(); stk.pop(); stk.push(a>0?std::log(a):0); break; }
            case TokenType::FUNC_SIN:  { double a=stk.top(); stk.pop(); stk.push(std::sin(a)); break; }
            case TokenType::FUNC_COS:  { double a=stk.top(); stk.pop(); stk.push(std::cos(a)); break; }
            case TokenType::FUNC_TAN:  { double a=stk.top(); stk.pop(); stk.push(std::tan(a)); break; }
            case TokenType::FUNC_ASIN: { double a=stk.top(); stk.pop(); stk.push(std::asin(a)); break; }
            case TokenType::FUNC_ACOS: { double a=stk.top(); stk.pop(); stk.push(std::acos(a)); break; }
            case TokenType::FUNC_ATAN: { double a=stk.top(); stk.pop(); stk.push(std::atan(a)); break; }
            case TokenType::FUNC_SGN:  { double a=stk.top(); stk.pop(); stk.push(a>0?1:(a<0?-1:0)); break; }
            case TokenType::FUNC_STEP: { double a=stk.top(); stk.pop(); stk.push(a>=0?1:0); break; }
            case TokenType::FUNC_MIN: {
                double b=stk.top(); stk.pop(); double a=stk.top(); stk.pop();
                stk.push(std::min(a,b)); break;
            }
            case TokenType::FUNC_MAX: {
                double b=stk.top(); stk.pop(); double a=stk.top(); stk.pop();
                stk.push(std::max(a,b)); break;
            }
            default: break;
        }
    }
    return stk.empty() ? 0.0 : stk.top();
}

double evaluate(const Expression& expr, const double* vars, int n_vars) {
    // Use var_idx on each VARIABLE token to index directly into the vars array.
    // Matching legacy mathexpr.c where variables are resolved by index.
    return evaluate(expr, [vars, n_vars, &expr](const std::string& name) -> double {
        // Find the token with this var_name and use its var_idx
        for (const auto& tok : expr.postfix) {
            if (tok.type == TokenType::VARIABLE && tok.var_name == name) {
                if (tok.var_idx >= 0 && tok.var_idx < n_vars)
                    return vars[tok.var_idx];
                break;
            }
        }
        return 0.0;
    });
}

} // namespace mathexpr
} // namespace openswmm
