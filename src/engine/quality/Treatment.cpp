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
 * @file      Treatment.cpp
 * @brief     Treatment expression parser (Shunting-yard) and evaluator (stack machine).
 *
 * @details The parser converts infix treatment expressions into postfix (RPN)
 *          token lists. The evaluator is a simple stack machine that processes
 *          the postfix token list with concrete variable values.
 *
 *          Numerically identical to the legacy treatmnt.c + mathexpr.c chain.
 *
 * @ingroup new_engine
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "Treatment.hpp"
#include <cmath>
#include <stack>
#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <functional>
#include <vector>

namespace openswmm {
namespace treatment {

// ============================================================================
// Parser helpers
// ============================================================================

/**
 * @brief Operator precedence (higher binds tighter).
 *
 * @details Return shunting-yard precedence for a treatment arithmetic token.
 * @param[in] t Token kind.
 * @return ADD/SUB1, MUL/DIV2, POW3, unary NEG4, otherwise0. Allocation-free/read-only.
 */
static int precedence(TokenType t) {
    switch (t) {
    case TokenType::ADD:
    case TokenType::SUB:
        return 1;
    case TokenType::MUL:
    case TokenType::DIV:
        return 2;
    case TokenType::POW:
        return 3;
    case TokenType::NEG:
        return 4;
    default:
        return 0;
    }
}

/**
 * @brief True if the token type is a function (unary or binary like min/max).
 *
 * @details Test whether a token lies in the contiguous supported scalar-function enum range.
 * @param[in] t Token kind.
 * @return True for FUNC_EXP through FUNC_STEP inclusive; no operand/arity validation or
 * mutation.
 */
static bool isFunction(TokenType t) {
    int v = static_cast<int>(t);
    return v >= static_cast<int>(TokenType::FUNC_EXP) &&
           v <= static_cast<int>(TokenType::FUNC_STEP);
}

/**
 * @brief True if the token type is a binary or unary arithmetic operator.
 *
 * @details Test whether a token is arithmetic binary operator or unary negation.
 * @param[in] t Token kind.
 * @return True for ADD/SUB/MUL/DIV/POW/NEG, false otherwise; no mutation/allocation.
 */
static bool isOperator(TokenType t) {
    return t == TokenType::ADD || t == TokenType::SUB || t == TokenType::MUL ||
           t == TokenType::DIV || t == TokenType::POW || t == TokenType::NEG;
}

/**
 * @brief Right-associative operators.
 *
 * @details Select right-associative treatment operators.
 * @param[in] t Token kind.
 * @return True for POW or NEG only, false otherwise. Read-only/allocation-free.
 */
static bool isRightAssoc(TokenType t) {
    return t == TokenType::POW || t == TokenType::NEG;
}

// Map of function names (lowercase) to token types.
static const std::unordered_map<std::string, TokenType> func_map = {
    {"exp", TokenType::FUNC_EXP},   {"log", TokenType::FUNC_LOG}, {"ln", TokenType::FUNC_LOG},
    {"sqrt", TokenType::FUNC_SQRT}, {"min", TokenType::FUNC_MIN}, {"max", TokenType::FUNC_MAX},
    {"abs", TokenType::FUNC_ABS},   {"sgn", TokenType::FUNC_SGN}, {"step", TokenType::FUNC_STEP},
};

// Map of variable names (uppercase) to TreatVar.
//
// legacy's process variables are ProcessVarWords = { HRT, DT, FLOW, DEPTH,
// AREA } (keywords.c:103). DEPTH is added here under legacy's own spelling —
// it is how the SWMM manual writes the variable, and an expression using it
// did not fail loudly, it hit the tokenizer's unknown-identifier path and the
// whole treatment was skipped in silence.
//
// All five of legacy's process variables are accepted under legacy's own
// spelling. FLOW was the last hold-out: the validator was contracted to
// REJECT it, which made it the only legacy process variable this engine
// refused, so a deck written the way the SWMM manual documents had its
// treatment silently skipped. `Q` and `D` stay as this engine's own aliases —
// legacy would reject them, but they cost nothing and existing decks may use
// them.
static const std::unordered_map<std::string, TreatVar> var_map = {
    {"C", TreatVar::C},       {"R", TreatVar::R},     {"DT", TreatVar::DT},
    {"HRT", TreatVar::HRT},   {"FLOW", TreatVar::Q},  // legacy pvFLOW
    {"Q", TreatVar::Q},                               // v6 alias
    {"V", TreatVar::V},       {"DEPTH", TreatVar::D}, // legacy pvDEPTH
    {"D", TreatVar::D},                               // v6 alias
    {"AREA", TreatVar::AREA},                         // Gap #16: legacy pvAREA
};

// ============================================================================
// Tokenizer
// ============================================================================

/**
 * @brief Tokenize an expression string into a flat token list (infix order). Variables and
 * functions are recognized by name lookup. If pollut_lookup is non-null, R_xxx and C_xxx are
 * resolved to pollutant refs.
 *
 * @details Tokenize a treatment RHS using built-in variables/functions and optional pollutant
 * lookup.
 * @param[in] s RHS text, no left-hand equation prefix.
 * @param[in] pollut_lookup Optional borrowed callable receiving original-case pollutant name and
 * returning nonnegative zero-based ID or negative absent; invoked synchronously, possibly more
 * than once per bare name. Callback views valid only during call, no pointer retained.
 * @return Owned infix token vector, empty for unknown/unresolved identifier (or empty/only
 * ignored text). Unknown punctuation is silently skipped. Numeric scanning uses std::stod
 * without checking full substring consumption, so this is not a strict malformed-number
 * validator.
 * @note Built-ins byte case-insensitive, ln aliases log, unary '-' recognized after
 * operator/left paren/comma. With lookup, bare names become concentration references; any X_name
 * prefix with second character '_' maps R prefix to removal and others to concentration after
 * lookup. No model mutation by this helper; allocations/std::stod/callback may throw and
 * callback effects are external.
 */
static std::vector<Token>
tokenize(const std::string& s, const std::function<int(const std::string&)>& pollut_lookup = {}) {
    std::vector<Token> tokens;
    size_t i = 0;
    while (i < s.size()) {
        char c = s[i];

        // Skip whitespace
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }

        // Numeric literal (integer, decimal, scientific notation)
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
            size_t start = i;
            while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.' ||
                                    s[i] == 'e' || s[i] == 'E' ||
                                    ((s[i] == '+' || s[i] == '-') && i > 0 &&
                                     (s[i - 1] == 'e' || s[i - 1] == 'E'))))
                ++i;
            Token t;
            t.type = TokenType::NUMBER;
            t.value = std::stod(s.substr(start, i - start));
            tokens.push_back(t);
            continue;
        }

        // Identifier: function name or variable name
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            size_t start = i;
            while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_'))
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
                t.var = vit->second;
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
                    return {}; // unknown pollutant name
                }
            } else if (pollut_lookup && pollut_lookup(word) >= 0) {
                // A BARE pollutant name is that pollutant's concentration —
                // legacy getVariableIndex checks the process variables, then
                // `project_findObject(POLLUT, s)`, and only then the R_ form
                // (treatmnt.c:307-318). This parser knew the R_/C_ forms and
                // not the bare one, so `C = TSS * exp(-0.15*HRT)` — the form
                // the SWMM manual uses — resolved to nothing, the token list
                // came back empty, and the treatment was skipped WITHOUT any
                // error. Twenty corpus decks write their expressions this way.
                Token t;
                t.type = TokenType::VARIABLE;
                t.var = TreatVar::C_POLLUT;
                t.pollut_ref = pollut_lookup(word);
                tokens.push_back(t);
            } else {
                return {}; // unknown identifier
            }
            continue;
        }

        // Single-character tokens
        {
            Token t;
            switch (c) {
            case '+':
                t.type = TokenType::ADD;
                break;
            case '-':
                // Unary minus: at start, after operator, after '(' or ','
                if (tokens.empty() || tokens.back().type == TokenType::LPAREN ||
                    isOperator(tokens.back().type) || tokens.back().type == TokenType::COMMA) {
                    t.type = TokenType::NEG;
                } else {
                    t.type = TokenType::SUB;
                }
                break;
            case '*':
                t.type = TokenType::MUL;
                break;
            case '/':
                t.type = TokenType::DIV;
                break;
            case '^':
                t.type = TokenType::POW;
                break;
            case '(':
                t.type = TokenType::LPAREN;
                break;
            case ')':
                t.type = TokenType::RPAREN;
                break;
            case ',':
                t.type = TokenType::COMMA;
                break;
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

/**
 * @brief Convert infix token list to postfix (RPN) using the Shunting-yard algorithm. Returns 0
 * on success, -1 on error (mismatched parentheses, etc.).
 *
 * @details Convert treatment infix tokens to postfix while checking parentheses/comma placement.
 * @param[in] infix Stable token vector with valid token enum/variable metadata.
 * @param[out] postfix Cleared first, appended progressively; can retain partial output on
 * failure.
 * @return Zero on conversion success, -1 for unpaired parentheses/misplaced comma. Does not
 * validate operand counts, function arity, empty expression or all unknown enum cases;
 * validate() checks stack shape separately. Honors right-associative power/negation and
 * precedence().
 * @note Requires nonaliasing input/output; allocation may throw after partial output. No
 * context, physical native state, table lookup or clock mutation.
 */
static int shuntingYard(const std::vector<Token>& infix, std::vector<Token>& postfix) {
    postfix.clear();
    std::stack<Token> ops;

    for (const auto& tok : infix) {
        if (tok.type == TokenType::NUMBER || tok.type == TokenType::VARIABLE) {
            postfix.push_back(tok);
        } else if (isFunction(tok.type)) {
            ops.push(tok);
        } else if (tok.type == TokenType::COMMA) {
            // Pop operators until left paren (for min/max arguments)
            while (!ops.empty() && ops.top().type != TokenType::LPAREN) {
                postfix.push_back(ops.top());
                ops.pop();
            }
            if (ops.empty())
                return -1; // mismatched comma outside parens
        } else if (isOperator(tok.type)) {
            while (
                !ops.empty() && isOperator(ops.top().type) &&
                ((isRightAssoc(tok.type) && precedence(tok.type) < precedence(ops.top().type)) ||
                 (!isRightAssoc(tok.type) && precedence(tok.type) <= precedence(ops.top().type)))) {
                postfix.push_back(ops.top());
                ops.pop();
            }
            ops.push(tok);
        } else if (tok.type == TokenType::LPAREN) {
            ops.push(tok);
        } else if (tok.type == TokenType::RPAREN) {
            while (!ops.empty() && ops.top().type != TokenType::LPAREN) {
                postfix.push_back(ops.top());
                ops.pop();
            }
            if (ops.empty())
                return -1; // mismatched right paren
            ops.pop();     // discard LPAREN
            // If top of stack is a function, pop it to output
            if (!ops.empty() && isFunction(ops.top().type)) {
                postfix.push_back(ops.top());
                ops.pop();
            }
        }
    }

    // Pop remaining operators
    while (!ops.empty()) {
        if (ops.top().type == TokenType::LPAREN)
            return -1; // mismatched
        postfix.push_back(ops.top());
        ops.pop();
    }

    return 0;
}

// ============================================================================
// Public parse function
// ============================================================================

/**
 * @brief Parse the strict C/R equation prefix and convert built-in-only RHS to postfix.
 * @param[in] expr_str Whole C=... or R=... expression; surrounding LHS whitespace permitted,
 * case ignored.
 * @param[out] result Clears tokens and is_removal first, retains pollutant_idx. Success installs
 * postfix; shunting-yard failure clears tokens, other early failures can retain chosen removal
 * flag.
 * @return Zero on nonempty tokenized/parenthesis-valid RHS, -1 on prefix/empty/unknown
 * identifier or parenthesis error. Does not prove operand stack shape; use validate() for editor
 * syntax checks.
 * @note Unknown punctuation can be skipped and numeric substrings accepted by tokenizer; no
 * independent finite evaluation/physical-unit validation. No pollutant name resolution.
 * Allocation/numeric exceptions propagate; no context or physical/clock changes. Requires
 * exclusive result, nonaliasing input.
 */
int parse(const std::string& expr_str, TreatExpr& result) {
    result.tokens.clear();
    result.is_removal = false;

    if (expr_str.empty())
        return -1;

    // --- Find the '=' sign that separates LHS from RHS
    size_t eq_pos = expr_str.find('=');
    if (eq_pos == std::string::npos)
        return -1;

    // --- Determine equation type from LHS (everything before '=')
    //     LHS should be "R" or "C" (with optional whitespace)
    std::string lhs = expr_str.substr(0, eq_pos);
    // Trim whitespace from LHS
    size_t lhs_start = lhs.find_first_not_of(" \t\r\n");
    size_t lhs_end = lhs.find_last_not_of(" \t\r\n");
    if (lhs_start == std::string::npos)
        return -1;
    lhs = lhs.substr(lhs_start, lhs_end - lhs_start + 1);

    // Convert LHS to uppercase for comparison
    char lhs_char = static_cast<char>(std::toupper(static_cast<unsigned char>(lhs[0])));

    if (lhs.size() == 1 && lhs_char == 'R') {
        result.is_removal = true;
    } else if (lhs.size() == 1 && lhs_char == 'C') {
        result.is_removal = false;
    } else {
        return -1; // LHS must be exactly "R" or "C"
    }

    // --- Extract RHS (everything after '=')
    std::string rhs = expr_str.substr(eq_pos + 1);

    // --- Tokenize the RHS
    auto infix_tokens = tokenize(rhs);
    if (infix_tokens.empty() && !rhs.empty()) {
        // Tokenizer returned empty due to unknown identifier
        return -1;
    }
    if (infix_tokens.empty())
        return -1;

    // --- Convert infix to postfix via Shunting-yard
    if (shuntingYard(infix_tokens, result.tokens) != 0) {
        result.tokens.clear();
        return -1;
    }

    return 0;
}

// ============================================================================
// Validation — diagnostic peer of parse() for editors
// ============================================================================

/**
 * @brief Validate built-in-only treatment grammar with an editor diagnostic and source offset.
 * @param[in] expr_str Whole C=... or R=... expression; byte offsets refer to this complete
 * string.
 * @param[out] msg Cleared first, populated on diagnostic failure.
 * @param[out] col Set -1 first; zero-based byte error offset when attributable, otherwise -1.
 * @return Zero for nonempty strict known-character/identifier, parenthesis and
 * operand-stack-valid expression passing a final basic parse cross-check; -1 for diagnosed
 * failure.
 * @note Unlike parse() rejects unknown characters; co-pollutant references unsupported in this
 * route. Recognizes numeric std::stod conversion errors but does not check full numeric
 * substring consumption or finite/domain evaluation, so success is not a physical/numerical
 * admissibility guarantee. Allocations or final parser numeric exceptions may propagate; no
 * context/physics/clock mutation. Output references must be exclusive and msg must not alias
 * expr_str (cleared before parsing).
 */
int validate(const std::string& expr_str, std::string& msg, int& col) {
    msg.clear();
    col = -1;

    auto fail = [&](std::string m, int c) {
        msg = std::move(m);
        col = c;
        return -1;
    };

    if (expr_str.empty())
        return fail("expression is empty", 0);

    const size_t eq_pos = expr_str.find('=');
    if (eq_pos == std::string::npos)
        return fail("missing '=' — expression must have the form "
                    "\"R = ...\" or \"C = ...\"",
                    0);

    // --- LHS: exactly R or C ------------------------------------------------
    std::string lhs = expr_str.substr(0, eq_pos);
    const size_t lhs_start = lhs.find_first_not_of(" \t\r\n");
    if (lhs_start == std::string::npos)
        return fail("left-hand side must be 'R' (removal) or "
                    "'C' (concentration)",
                    0);
    const size_t lhs_end = lhs.find_last_not_of(" \t\r\n");
    lhs = lhs.substr(lhs_start, lhs_end - lhs_start + 1);
    const char lc = static_cast<char>(std::toupper(static_cast<unsigned char>(lhs[0])));
    if (lhs.size() != 1 || (lc != 'R' && lc != 'C'))
        return fail("left-hand side must be exactly 'R' or 'C', got '" + lhs + "'",
                    static_cast<int>(lhs_start));

    // --- RHS: strict scan with positions ------------------------------------
    // Stricter than tokenize(): unknown characters are an error here rather
    // than silently skipped, so the editor's verdict matches what the
    // engine will actually evaluate.
    const std::string rhs = expr_str.substr(eq_pos + 1);
    const int rhs_off = static_cast<int>(eq_pos) + 1;

    std::vector<Token> tokens;
    size_t i = 0;
    while (i < rhs.size()) {
        const char c = rhs[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }

        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
            const size_t start = i;
            while (i < rhs.size() && (std::isdigit(static_cast<unsigned char>(rhs[i])) ||
                                      rhs[i] == '.' || rhs[i] == 'e' || rhs[i] == 'E' ||
                                      ((rhs[i] == '+' || rhs[i] == '-') && i > 0 &&
                                       (rhs[i - 1] == 'e' || rhs[i - 1] == 'E'))))
                ++i;
            Token t;
            t.type = TokenType::NUMBER;
            try {
                t.value = std::stod(rhs.substr(start, i - start));
            } catch (...) {
                return fail("malformed number '" + rhs.substr(start, i - start) + "'",
                            rhs_off + static_cast<int>(start));
            }
            tokens.push_back(t);
            continue;
        }

        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            const size_t start = i;
            while (i < rhs.size() &&
                   (std::isalnum(static_cast<unsigned char>(rhs[i])) || rhs[i] == '_'))
                ++i;
            const std::string word = rhs.substr(start, i - start);

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

            std::string upper = word;
            for (auto& ch : upper)
                ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            auto vit = var_map.find(upper);
            if (vit != var_map.end()) {
                Token t;
                t.type = TokenType::VARIABLE;
                t.var = vit->second;
                tokens.push_back(t);
                continue;
            }

            if (upper.size() > 2 && upper[1] == '_' && (upper[0] == 'R' || upper[0] == 'C'))
                return fail("co-pollutant reference '" + word + "' is not supported here",
                            rhs_off + static_cast<int>(start));

            return fail("unknown identifier '" + word +
                            "' (variables: C R DT HRT Q V D AREA; "
                            "functions: exp log ln sqrt min max abs sgn step)",
                        rhs_off + static_cast<int>(start));
        }

        Token t;
        switch (c) {
        case '+':
            t.type = TokenType::ADD;
            break;
        case '-':
            t.type = (tokens.empty() || tokens.back().type == TokenType::LPAREN ||
                      isOperator(tokens.back().type) || tokens.back().type == TokenType::COMMA)
                         ? TokenType::NEG
                         : TokenType::SUB;
            break;
        case '*':
            t.type = TokenType::MUL;
            break;
        case '/':
            t.type = TokenType::DIV;
            break;
        case '^':
            t.type = TokenType::POW;
            break;
        case '(':
            t.type = TokenType::LPAREN;
            break;
        case ')':
            t.type = TokenType::RPAREN;
            break;
        case ',':
            t.type = TokenType::COMMA;
            break;
        default:
            return fail(std::string("unexpected character '") + c + "'",
                        rhs_off + static_cast<int>(i));
        }
        tokens.push_back(t);
        ++i;
    }

    if (tokens.empty())
        return fail("missing right-hand side after '='", rhs_off);

    // --- Structure: parens/commas + operator/function arity -----------------
    std::vector<Token> postfix;
    if (shuntingYard(tokens, postfix) != 0)
        return fail("mismatched parentheses or misplaced comma", -1);

    int depth = 0;
    for (const auto& t : postfix) {
        switch (t.type) {
        case TokenType::NUMBER:
        case TokenType::VARIABLE:
            ++depth;
            break;
        case TokenType::NEG:
        case TokenType::FUNC_EXP:
        case TokenType::FUNC_LOG:
        case TokenType::FUNC_SQRT:
        case TokenType::FUNC_ABS:
        case TokenType::FUNC_SGN:
        case TokenType::FUNC_STEP:
            if (depth < 1)
                return fail("function or operator is missing its operand", -1);
            break; // pop 1, push 1
        case TokenType::FUNC_MIN:
        case TokenType::FUNC_MAX:
        case TokenType::ADD:
        case TokenType::SUB:
        case TokenType::MUL:
        case TokenType::DIV:
        case TokenType::POW:
            if (depth < 2)
                return fail("operator or min/max is missing an operand", -1);
            --depth; // pop 2, push 1
            break;
        default:
            return fail("internal: unexpected token in parsed expression", -1);
        }
    }
    if (depth != 1)
        return fail("incomplete expression", -1);

    // --- Drift guard: the production parser must agree ----------------------
    TreatExpr probe;
    if (parse(expr_str, probe) != 0)
        return fail("invalid expression", -1);

    return 0;
}

// ============================================================================
// Evaluator — postfix stack machine
// ============================================================================

/**
 * @brief Evaluate built-in treatment postfix with scalar hydraulic inputs only.
 * @param[in] expr Stable postfix expression; caller supplies valid stack/enum metadata and
 * expected units.
 * @param[in] c Current pollutant concentration in caller's native concentration units.
 * @param[in] dt Accepted interval s.
 * @param[in] hrt Hydraulic residence time hours.
 * @param[in] q Native treatment FLOW/Q input in project display flow units.
 * @param[in] v Caller-supplied volume in its treatment convention; used verbatim (no
 * conversion).
 * @param[in] d Native treatment DEPTH/D in project display length.
 * @param[in] area Node surface area in project display length2, used verbatim.
 * @return Stack top, zero for empty stack or missing operator operands. Extra stack entries are
 * not rejected. R variable always zero; invalid log/sqrt domain and division by zero return0
 * locally, while exp/pow/ordinary arithmetic may yield nonfinite values. No automatic
 * concentration/removal bounds or dimensional validation. Allocates an evaluation stack and may
 * throw; no model mutation.
 * @note Co-pollutant C/R references evaluate0 in this overload. STEP(0)=1 here (>=0), unlike the
 * co-treatment overload's strict-positive STEP. Read-only expression, no clock/physical advance.
 */
double evaluate(const TreatExpr& expr, double c, double dt, double hrt, double q, double v,
                double d, double area) {
    std::stack<double> stk;

    for (const auto& tok : expr.tokens) {
        switch (tok.type) {
        case TokenType::NUMBER:
            stk.push(tok.value);
            break;

        case TokenType::VARIABLE:
            switch (tok.var) {
            case TreatVar::C:
                stk.push(c);
                break;
            case TreatVar::R:
                stk.push(0.0);
                break;
            case TreatVar::DT:
                stk.push(dt);
                break;
            case TreatVar::HRT:
                stk.push(hrt);
                break;
            case TreatVar::Q:
                stk.push(q);
                break;
            case TreatVar::V:
                stk.push(v);
                break;
            case TreatVar::D:
                stk.push(d);
                break;
            case TreatVar::AREA:
                stk.push(area);
                break; // Gap #16
            case TreatVar::C_POLLUT:
                stk.push(0.0);
                break;
            case TreatVar::R_POLLUT:
                stk.push(0.0);
                break;
            }
            break;

        case TokenType::NEG: {
            if (stk.empty())
                return 0.0;
            double a = stk.top();
            stk.pop();
            stk.push(-a);
            break;
        }

        case TokenType::ADD:
        case TokenType::SUB:
        case TokenType::MUL:
        case TokenType::DIV:
        case TokenType::POW: {
            if (stk.size() < 2)
                return 0.0;
            double b = stk.top();
            stk.pop();
            double a = stk.top();
            stk.pop();
            switch (tok.type) {
            case TokenType::ADD:
                stk.push(a + b);
                break;
            case TokenType::SUB:
                stk.push(a - b);
                break;
            case TokenType::MUL:
                stk.push(a * b);
                break;
            case TokenType::DIV:
                stk.push(b != 0.0 ? a / b : 0.0);
                break;
            case TokenType::POW:
                stk.push(std::pow(a, b));
                break;
            default:
                break;
            }
            break;
        }

        case TokenType::FUNC_EXP: {
            if (stk.empty())
                return 0.0;
            double a = stk.top();
            stk.pop();
            stk.push(std::exp(a));
            break;
        }
        case TokenType::FUNC_LOG: {
            if (stk.empty())
                return 0.0;
            double a = stk.top();
            stk.pop();
            stk.push(a > 0.0 ? std::log(a) : 0.0);
            break;
        }
        case TokenType::FUNC_SQRT: {
            if (stk.empty())
                return 0.0;
            double a = stk.top();
            stk.pop();
            stk.push(a >= 0.0 ? std::sqrt(a) : 0.0);
            break;
        }
        case TokenType::FUNC_ABS: {
            if (stk.empty())
                return 0.0;
            double a = stk.top();
            stk.pop();
            stk.push(std::fabs(a));
            break;
        }
        case TokenType::FUNC_SGN: {
            if (stk.empty())
                return 0.0;
            double a = stk.top();
            stk.pop();
            stk.push(a > 0.0 ? 1.0 : (a < 0.0 ? -1.0 : 0.0));
            break;
        }
        case TokenType::FUNC_STEP: {
            if (stk.empty())
                return 0.0;
            double a = stk.top();
            stk.pop();
            stk.push(a >= 0.0 ? 1.0 : 0.0);
            break;
        }
        case TokenType::FUNC_MIN: {
            if (stk.size() < 2)
                return 0.0;
            double b = stk.top();
            stk.pop();
            double a = stk.top();
            stk.pop();
            stk.push(std::min(a, b));
            break;
        }
        case TokenType::FUNC_MAX: {
            if (stk.size() < 2)
                return 0.0;
            double b = stk.top();
            stk.pop();
            double a = stk.top();
            stk.pop();
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

/**
 * @brief Apply scalar-only treatment and clamp its output to removal-only concentration bounds.
 * @param[in] expr Stable parsed expression; is_removal selects removal versus direct
 * concentration.
 * @param[in] c_in Finite nonnegative current/inflow native concentration to bound output.
 * @param[in] dt Accepted duration s.
 * @param[in] hrt Residence time hours.
 * @param[in] q Display-unit treatment flow.
 * @param[in] v Caller treatment volume convention, passed verbatim.
 * @param[in] d Display-unit node depth.
 * @param[in] area Display length2 node area.
 * @return For removal equation c_in*(1-clamp(eval,0,1)); for concentration
 * min(c_in,max(0,eval)).
 * @note Uses scalar evaluate (named pollutant references become0, STEP0=1);
 * domain/malformed-stack fallbacks apply. No context mass/ledger/water/clock mutation.
 * Allocation may throw; caller handles finite input/physical validation and books the accepted
 * treatment mass separately.
 */
double applyTreatment(const TreatExpr& expr, double c_in, double dt, double hrt, double q, double v,
                      double d, double area) {
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

static int parse_impl(const std::string& expr_str, TreatExpr& result,
                      const std::function<int(const std::string&)>& pollut_lookup);

// Names-based overload. The deck-compile path has the pollutant NAME TABLE in
// hand, not a free function, and the function-pointer overload below cannot
// capture it — which is why both production call sites used the lookup-less
// overload and NO deck-authored pollutant reference (bare name or R_xxx) has
// ever resolved.
/**
 * @brief Parse named pollutant/co-treatment references against a stable name table.
 * @param[in] expr_str Callback-enabled equation text; first character C/R, no leading
 * whitespace.
 * @param[out] result Reset and filled through parse_impl; partial output may remain on failure.
 * @param[in] pollut_names Stable native names, case-insensitive first match supplies zero-based
 * ID; lookup returns negative for absent. Borrowed only during call, not retained.
 * @return Zero/-1 from permissive callback parser, which can accept empty RHS and does not
 * validate operand stack shape/full LHS. No physical context/clock change; allocation/numeric
 * errors may throw.
 * @note Bare names and R_name/C_name references accepted; callback tokenizer also accepts other
 * prefix letters before '_'. Caller compiles only qualified expressions and supplies exclusive
 * result.
 */
int parse(const std::string& expr_str, TreatExpr& result,
          const std::vector<std::string>& pollut_names) {
    result.tokens.clear();
    result.pollutant_idx = -1;
    result.is_removal = false;
    auto lookup = [&pollut_names](const std::string& name) -> int {
        for (std::size_t i = 0; i < pollut_names.size(); ++i) {
            if (pollut_names[i].size() != name.size())
                continue;
            bool same = true;
            for (std::size_t k = 0; k < name.size() && same; ++k)
                same = std::toupper(static_cast<unsigned char>(pollut_names[i][k])) ==
                       std::toupper(static_cast<unsigned char>(name[k]));
            if (same)
                return static_cast<int>(i);
        }
        return -1;
    };
    return parse_impl(expr_str, result, lookup);
}

// Shared body: everything below is independent of HOW a pollutant name is
// looked up, so both public overloads funnel through here.
/**
 * @brief Parse callback-enabled treatment with the legacy permissive prefix/RHS rules.
 * @param[in] expr_str Whole expression beginning literally with C/c or R/r; requires '=' after
 * at least one character. Leading whitespace fails; additional LHS text is not validated here.
 * @param[out] result Clears tokens, resets pollutant_idx=-1 and is_removal=false first. May
 * retain partial postfix/removal flag on failure; use staged result for transactional edits.
 * @param[in] pollut_lookup Optional nonempty callable for named species lookup; borrowed during
 * synchronous tokenize only. Negative means absent, no pointer retained.
 * @return Zero on shunting-yard success (including an actually empty RHS), -1 on
 * prefix/token/paren failure. Does not validate operand arity, finite values or numeric
 * substring completeness.
 * @note Callback/numeric/allocation exceptions propagate; external callback effects not rolled
 * back. No native context/physics/calendar mutation, exclusive nonaliasing result/input
 * required.
 */
static int parse_impl(const std::string& expr_str, TreatExpr& result,
                      const std::function<int(const std::string&)>& pollut_lookup) {
    result.tokens.clear();
    result.pollutant_idx = -1;
    result.is_removal = false;

    if (expr_str.empty())
        return -1;

    // Determine treatment type from first non-space character
    size_t eq = expr_str.find('=');
    if (eq == std::string::npos || eq == 0)
        return -1;

    char lhs = static_cast<char>(std::toupper(static_cast<unsigned char>(expr_str[0])));
    if (lhs == 'R')
        result.is_removal = true;
    else if (lhs == 'C')
        result.is_removal = false;
    else
        return -1;

    std::string rhs = expr_str.substr(eq + 1);

    // Tokenize with pollutant lookup
    auto tokens = tokenize(rhs, pollut_lookup);
    if (tokens.empty() && !rhs.empty())
        return -1;

    // Convert infix to postfix via shunting-yard
    if (shuntingYard(tokens, result.tokens) != 0)
        return -1;

    return 0;
}

// ============================================================================
// Extended evaluate with co-treatment support
// ============================================================================

/**
 * @brief Evaluate treatment postfix with optional pollutant concentration/removal views.
 * @param[in] expr Stable postfix expression; caller supplies valid stack/enum metadata and
 * expected units.
 * @param[in] c Current pollutant concentration in caller's native concentration units.
 * @param[in] dt Accepted interval s.
 * @param[in] hrt Hydraulic residence time hours.
 * @param[in] q Native treatment FLOW/Q input in project display flow units.
 * @param[in] v Caller-supplied volume in its treatment convention; used verbatim (no
 * conversion).
 * @param[in] d Native treatment DEPTH/D in project display length.
 * @param[in] area Node surface area in project display length2, used verbatim.
 * @return Stack top, zero for empty stack or missing operator operands. Extra stack entries are
 * not rejected. R variable always zero; invalid log/sqrt domain and division by zero return0
 * locally, while exp/pow/ordinary arithmetic may yield nonfinite values. No automatic
 * concentration/removal bounds or dimensional validation. Allocates an evaluation stack and may
 * throw; no model mutation.
 * @param[in] cin Optional readable n_pollut inflow concentration array, fallback for named C
 * rows.
 * @param[in] removal Optional readable n_pollut removal fractions; negative/not-computed
 * treated0.
 * @param[in] n_pollut Nonnegative readable extent of all provided pollutant arrays; token index
 * outside [0,n_pollut) produces0 without dereference.
 * @param[in] cpollut Optional readable n_pollut current mixed concentration view, preferred over
 * cin for named C rows; null falls back to cin, both null gives0. Arrays borrowed for call only.
 * @note STEP(0)=0 here (strict>0), unlike scalar-only overload. No recursive treatment
 * resolution; caller computes removal dependencies/loop detection. No physical/clock changes,
 * arrays must remain stable, and external mutation concurrent with evaluation is unsupported.
 */
double evaluate(const TreatExpr& expr, double c, double dt, double hrt, double q, double v,
                double d, const double* cin, const double* removal, int n_pollut, double area,
                const double* cpollut) {
    std::stack<double> stk;

    for (const auto& tok : expr.tokens) {
        switch (tok.type) {
        case TokenType::NUMBER:
            stk.push(tok.value);
            break;

        case TokenType::VARIABLE:
            switch (tok.var) {
            case TreatVar::C:
                stk.push(c);
                break;
            case TreatVar::R:
                stk.push(0.0);
                break;
            case TreatVar::DT:
                stk.push(dt);
                break;
            case TreatVar::HRT:
                stk.push(hrt);
                break;
            case TreatVar::Q:
                stk.push(q);
                break;
            case TreatVar::V:
                stk.push(v);
                break;
            case TreatVar::D:
                stk.push(d);
                break;
            case TreatVar::AREA:
                stk.push(area);
                break; // Gap #16
            case TreatVar::C_POLLUT:
                // legacy getVariableValue (treatmnt.c:344-351): a
                // pollutant reference reads Cin[p] when THAT
                // pollutant's treatment here is a removal equation,
                // and the node's current concentration otherwise.
                // `cpollut` is that choice already made by the caller,
                // which is where the per-pollutant treatment types
                // are known; without it we keep the old Cin-only
                // reading.
                if (tok.pollut_ref >= 0 && tok.pollut_ref < n_pollut) {
                    if (cpollut)
                        stk.push(cpollut[tok.pollut_ref]);
                    else if (cin)
                        stk.push(cin[tok.pollut_ref]);
                    else
                        stk.push(0.0);
                } else {
                    stk.push(0.0);
                }
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
            if (stk.empty())
                return 0.0;
            double a = stk.top();
            stk.pop();
            stk.push(-a);
            break;
        }

        case TokenType::ADD:
        case TokenType::SUB:
        case TokenType::MUL:
        case TokenType::DIV:
        case TokenType::POW: {
            if (stk.size() < 2)
                return 0.0;
            double b = stk.top();
            stk.pop();
            double a = stk.top();
            stk.pop();
            switch (tok.type) {
            case TokenType::ADD:
                stk.push(a + b);
                break;
            case TokenType::SUB:
                stk.push(a - b);
                break;
            case TokenType::MUL:
                stk.push(a * b);
                break;
            case TokenType::DIV:
                stk.push(b != 0.0 ? a / b : 0.0);
                break;
            case TokenType::POW:
                stk.push(std::pow(a, b));
                break;
            default:
                break;
            }
            break;
        }

        case TokenType::FUNC_EXP:
        case TokenType::FUNC_LOG:
        case TokenType::FUNC_SQRT:
        case TokenType::FUNC_ABS:
        case TokenType::FUNC_SGN:
        case TokenType::FUNC_STEP: {
            if (stk.empty())
                return 0.0;
            double a = stk.top();
            stk.pop();
            switch (tok.type) {
            case TokenType::FUNC_EXP:
                stk.push(std::exp(a));
                break;
            case TokenType::FUNC_LOG:
                stk.push(a > 0 ? std::log(a) : 0.0);
                break;
            case TokenType::FUNC_SQRT:
                stk.push(a >= 0 ? std::sqrt(a) : 0.0);
                break;
            case TokenType::FUNC_ABS:
                stk.push(std::abs(a));
                break;
            case TokenType::FUNC_SGN:
                stk.push(a > 0 ? 1.0 : (a < 0 ? -1.0 : 0.0));
                break;
            case TokenType::FUNC_STEP:
                stk.push(a > 0 ? 1.0 : 0.0);
                break;
            default:
                break;
            }
            break;
        }

        case TokenType::FUNC_MIN:
        case TokenType::FUNC_MAX: {
            if (stk.size() < 2)
                return 0.0;
            double b = stk.top();
            stk.pop();
            double a = stk.top();
            stk.pop();
            stk.push(tok.type == TokenType::FUNC_MIN ? std::min(a, b) : std::max(a, b));
            break;
        }

        default:
            break;
        }
    }

    return stk.empty() ? 0.0 : stk.top();
}

// Function-pointer overload, kept for callers that already have a free
// function (the unit tests' mock lookup).
/**
 * @brief Parse named treatment references using an optional native name-resolution function.
 * @param[in] expr_str Whole expression passed unchanged to legacy-permissive parse_impl.
 * @param[out] result Reset/fill candidate; partial parse output may remain on diagnostic
 * failure.
 * @param[in] pollut_lookup Optional function pointer returning nonnegative native species ID or
 * -1 for absent; called synchronously with original-case name, must stay callable throughout
 * call.
 * @return Zero/-1 from parse_impl; null lookup permits built-ins only but keeps that overload's
 * permissive prefix/empty-RHS behavior. No pointer retained or context/physical/clock update.
 * @note Numeric/allocation/callback exceptions propagate. Caller handles full
 * syntax/operand/finite qualification and exclusive result, callback side effects remain
 * external.
 */
int parse(const std::string& expr_str, TreatExpr& result,
          int (*pollut_lookup)(const std::string& name)) {
    if (!pollut_lookup)
        return parse_impl(expr_str, result, {});
    return parse_impl(expr_str, result, std::function<int(const std::string&)>(pollut_lookup));
}

} // namespace treatment
} // namespace openswmm
