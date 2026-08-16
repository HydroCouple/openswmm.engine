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
 * @file ReactionExpression.cpp
 * @brief Reaction expression compiler + Tier-1 evaluator — phase R2 body.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "ReactionExpression.hpp"

#include <cctype>
#include <cmath>
#include <cstdlib>

namespace openswmm::transport {

namespace {

// ---------------------------------------------------------------------------
// Lexer
// ---------------------------------------------------------------------------

enum class LexKind {
    NUMBER, IDENT, OP, LPAREN, RPAREN, COMMA, END
};

struct Lexeme {
    LexKind     kind = LexKind::END;
    char        op   = 0;       ///< for OP: + - * / ^
    double      num  = 0.0;
    std::string ident;
    int         col  = 0;       ///< 1-based start column
};

struct Lexer {
    const std::string& s;
    std::size_t i = 0;
    explicit Lexer(const std::string& src) : s(src) {}

    Lexeme next(std::string& err) {
        while (i < s.size() &&
               (s[i] == ' ' || s[i] == '\t')) ++i;
        Lexeme lx;
        lx.col = static_cast<int>(i) + 1;
        if (i >= s.size()) { lx.kind = LexKind::END; return lx; }
        const char c = s[i];
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
            char* end = nullptr;
            lx.num  = std::strtod(s.c_str() + i, &end);
            lx.kind = LexKind::NUMBER;
            i = static_cast<std::size_t>(end - s.c_str());
            return lx;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            std::size_t b = i;
            while (i < s.size() &&
                   (std::isalnum(static_cast<unsigned char>(s[i])) ||
                    s[i] == '_')) ++i;
            lx.kind  = LexKind::IDENT;
            lx.ident = s.substr(b, i - b);
            return lx;
        }
        switch (c) {
            case '+': case '-': case '*': case '/': case '^':
                lx.kind = LexKind::OP; lx.op = c; ++i; return lx;
            case '(': lx.kind = LexKind::LPAREN; ++i; return lx;
            case ')': lx.kind = LexKind::RPAREN; ++i; return lx;
            case ',': lx.kind = LexKind::COMMA;  ++i; return lx;
            default:
                err = std::string("unexpected character '") + c + "'";
                return lx;
        }
    }
};

// ---------------------------------------------------------------------------
// Function table
// ---------------------------------------------------------------------------

struct FuncDef { const char* name; RxToken::Op op; int arity; };

const FuncDef kFuncs[] = {
    {"EXP",   RxToken::F_EXP,   1}, {"LOG",   RxToken::F_LOG,   1},
    {"LOG10", RxToken::F_LOG10, 1}, {"SQRT",  RxToken::F_SQRT,  1},
    {"ABS",   RxToken::F_ABS,   1}, {"SGN",   RxToken::F_SGN,   1},
    {"STEP",  RxToken::F_STEP,  1}, {"SIN",   RxToken::F_SIN,   1},
    {"COS",   RxToken::F_COS,   1}, {"TAN",   RxToken::F_TAN,   1},
    {"MIN",   RxToken::F_MIN,   2}, {"MAX",   RxToken::F_MAX,   2},
    {"POW",   RxToken::POW,     2},
};

std::string upper(std::string t) {
    for (auto& ch : t)
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return t;
}

const FuncDef* findFunc(const std::string& ident) {
    const std::string u = upper(ident);
    for (const auto& f : kFuncs)
        if (u == f.name) return &f;
    return nullptr;
}

/// Hydraulic variable names (case-sensitive match on the documented
/// spellings, then a case-insensitive fallback — "Re" and "RE" both work).
int findHydVar(const std::string& ident) {
    static const char* kNames[] = {"D", "Q", "U", "RE", "US", "FF", "AV",
                                   "HRT", "DT"};
    const std::string u = upper(ident);
    for (int i = 0; i < static_cast<int>(RxHydVar::COUNT_); ++i)
        if (u == kNames[i]) return i;
    return -1;
}

// ---------------------------------------------------------------------------
// Shunting-yard
// ---------------------------------------------------------------------------

struct OpEntry {
    RxToken::Op op;
    int  prec;
    bool right_assoc;
    int  col;
    bool is_func;
    int  arity;        ///< funcs
    bool is_paren;     ///< '(' marker
};

int precOf(char op) {
    switch (op) {
        case '+': case '-': return 1;
        case '*': case '/': return 2;
        case '^':           return 3;
        default:            return 0;
    }
}

RxToken::Op binOf(char op) {
    switch (op) {
        case '+': return RxToken::ADD;
        case '-': return RxToken::SUB;
        case '*': return RxToken::MUL;
        case '/': return RxToken::DIV;
        default:  return RxToken::POW;
    }
}

}  // namespace

std::string compileReactionExpression(const std::string& src,
                                      const RxSymbols& symbols,
                                      std::vector<RxToken>& pool,
                                      RxExprSpan& out,
                                      int& error_col) {
    out = RxExprSpan{static_cast<int32_t>(pool.size()), 0};
    error_col = 0;

    std::vector<RxToken>  output;
    std::vector<OpEntry>  ops;
    Lexer lex(src);

    auto fail = [&](const std::string& msg, int col) {
        error_col = col;
        return msg;
    };

    auto popOpToOutput = [&](const OpEntry& e) {
        RxToken t;
        t.op = e.op;
        output.push_back(t);
    };

    bool expect_operand = true;    // start of expression / after operator
    int  depth_guard    = 0;

    for (;;) {
        std::string lex_err;
        Lexeme lx = lex.next(lex_err);
        if (!lex_err.empty()) return fail(lex_err, lx.col);
        if (lx.kind == LexKind::END) break;

        switch (lx.kind) {
            case LexKind::NUMBER: {
                if (!expect_operand)
                    return fail("unexpected number", lx.col);
                RxToken t;
                t.op = RxToken::PUSH_NUM;
                t.value = lx.num;
                output.push_back(t);
                expect_operand = false;
                break;
            }
            case LexKind::IDENT: {
                if (!expect_operand)
                    return fail("unexpected identifier '" + lx.ident + "'",
                                lx.col);
                if (const FuncDef* f = findFunc(lx.ident)) {
                    // Must be followed by '('.
                    std::string e2;
                    Lexeme la = lex.next(e2);
                    if (!e2.empty()) return fail(e2, la.col);
                    if (la.kind != LexKind::LPAREN)
                        return fail("function '" + lx.ident +
                                    "' needs '('", lx.col);
                    OpEntry fe{};
                    fe.op = f->op; fe.is_func = true; fe.arity = f->arity;
                    fe.col = lx.col;
                    ops.push_back(fe);
                    OpEntry pe{};
                    pe.is_paren = true; pe.col = la.col;
                    ops.push_back(pe);
                    ++depth_guard;
                    expect_operand = true;
                    break;
                }
                RxToken t;
                int idx = -1;
                // Resolution order: species, coefficient, term (forward-only),
                // hydraulic variable.
                if (symbols.species) {
                    for (std::size_t k = 0; k < symbols.species->size(); ++k)
                        if ((*symbols.species)[k] == lx.ident) {
                            idx = static_cast<int>(k);
                            t.op = RxToken::PUSH_SPECIES;
                            break;
                        }
                }
                if (idx < 0 && symbols.coefs) {
                    for (std::size_t k = 0; k < symbols.coefs->size(); ++k)
                        if ((*symbols.coefs)[k] == lx.ident) {
                            idx = static_cast<int>(k);
                            t.op = RxToken::PUSH_COEF;
                            break;
                        }
                }
                if (idx < 0 && symbols.terms) {
                    for (std::size_t k = 0; k < symbols.terms->size(); ++k)
                        if ((*symbols.terms)[k] == lx.ident) {
                            if (static_cast<int>(k) >= symbols.max_term)
                                return fail("term '" + lx.ident +
                                            "' references later term — term "
                                            "references are forward-only",
                                            lx.col);
                            idx = static_cast<int>(k);
                            t.op = RxToken::PUSH_TERM;
                            break;
                        }
                }
                if (idx < 0 && symbols.pollutants) {
                    for (std::size_t k = 0; k < symbols.pollutants->size(); ++k)
                        if ((*symbols.pollutants)[k] == lx.ident) {
                            idx = static_cast<int>(k);
                            t.op = RxToken::PUSH_POLLUT;
                            break;
                        }
                }
                if (idx < 0) {
                    const int hv = findHydVar(lx.ident);
                    if (hv >= 0) { idx = hv; t.op = RxToken::PUSH_HYDVAR; }
                }
                if (idx < 0)
                    return fail("undefined identifier '" + lx.ident + "'",
                                lx.col);
                t.idx = idx;
                output.push_back(t);
                expect_operand = false;
                break;
            }
            case LexKind::OP: {
                if (expect_operand) {
                    if (lx.op == '-') {           // unary minus
                        // D-R8 (decided 2026-08-16): unary minus binds BELOW
                        // '^' — Python/Fortran/MATLAB convention, -2^2 = -4.
                        // Precedence 2 (== '*'): '^' stacks above NEG
                        // (2 < 3 ⇒ no pop), while '*' and '+'/'-' pop it
                        // first — every case then matches sympy, which the
                        // R5 authoring path round-trips through. The R2
                        // validator proved legacy mathexpr.c cannot
                        // arbitrate (returns 0 for both spellings).
                        OpEntry e{};
                        e.op = RxToken::NEG; e.prec = 2; e.right_assoc = true;
                        e.col = lx.col;
                        ops.push_back(e);
                        break;
                    }
                    if (lx.op == '+') break;      // unary plus: no-op
                    return fail(std::string("unexpected operator '") +
                                lx.op + "'", lx.col);
                }
                const int  p  = precOf(lx.op);
                const bool ra = (lx.op == '^');
                while (!ops.empty() && !ops.back().is_paren &&
                       !ops.back().is_func &&
                       (ops.back().prec > p ||
                        (ops.back().prec == p && !ra))) {
                    popOpToOutput(ops.back());
                    ops.pop_back();
                }
                OpEntry e{};
                e.op = binOf(lx.op); e.prec = p; e.right_assoc = ra;
                e.col = lx.col;
                ops.push_back(e);
                expect_operand = true;
                break;
            }
            case LexKind::LPAREN: {
                if (!expect_operand)
                    return fail("unexpected '('", lx.col);
                OpEntry e{};
                e.is_paren = true; e.col = lx.col;
                ops.push_back(e);
                ++depth_guard;
                if (depth_guard > kRxMaxStackDepth)
                    return fail("expression too deep", lx.col);
                break;
            }
            case LexKind::RPAREN: {
                if (expect_operand)
                    return fail("unexpected ')'", lx.col);
                while (!ops.empty() && !ops.back().is_paren) {
                    popOpToOutput(ops.back());
                    ops.pop_back();
                }
                if (ops.empty())
                    return fail("unbalanced ')'", lx.col);
                ops.pop_back();               // the '('
                --depth_guard;
                // Function application?
                if (!ops.empty() && ops.back().is_func) {
                    popOpToOutput(ops.back());
                    ops.pop_back();
                }
                break;
            }
            case LexKind::COMMA: {
                if (expect_operand)
                    return fail("unexpected ','", lx.col);
                while (!ops.empty() && !ops.back().is_paren) {
                    popOpToOutput(ops.back());
                    ops.pop_back();
                }
                if (ops.empty() || ops.size() < 2 ||
                    !ops[ops.size() - 2].is_func)
                    return fail("',' outside a function call", lx.col);
                expect_operand = true;
                break;
            }
            case LexKind::END: break;
        }
    }

    if (expect_operand)
        return fail("expression ends with an operator",
                    static_cast<int>(src.size()) + 1);
    while (!ops.empty()) {
        if (ops.back().is_paren)
            return fail("unbalanced '('", ops.back().col);
        popOpToOutput(ops.back());
        ops.pop_back();
    }
    if (output.empty())
        return fail("empty expression", 1);

    // Verify stack discipline + final depth 1, and bound the depth so the
    // evaluator's fixed stack is provably sufficient.
    int depth = 0, max_depth = 0;
    for (const auto& t : output) {
        switch (t.op) {
            case RxToken::PUSH_NUM: case RxToken::PUSH_SPECIES:
            case RxToken::PUSH_COEF: case RxToken::PUSH_TERM:
            case RxToken::PUSH_HYDVAR: case RxToken::PUSH_POLLUT:
                ++depth; break;
            case RxToken::NEG: case RxToken::F_EXP: case RxToken::F_LOG:
            case RxToken::F_LOG10: case RxToken::F_SQRT: case RxToken::F_ABS:
            case RxToken::F_SGN: case RxToken::F_STEP: case RxToken::F_SIN:
            case RxToken::F_COS: case RxToken::F_TAN:
                if (depth < 1) return fail("malformed expression", 1);
                break;
            default:                                     // binary
                if (depth < 2) return fail("malformed expression", 1);
                --depth; break;
        }
        if (depth > max_depth) max_depth = depth;
    }
    if (depth != 1) return fail("malformed expression", 1);
    if (max_depth > kRxMaxStackDepth)
        return fail("expression too deep", 1);

    out.begin = static_cast<int32_t>(pool.size());
    out.len   = static_cast<int32_t>(output.size());
    pool.insert(pool.end(), output.begin(), output.end());
    return {};
}

// ---------------------------------------------------------------------------
// Tier-1 evaluator — fixed stack, pre-resolved indices, allocation-free.
// ---------------------------------------------------------------------------

double evalReactionExpression(const std::vector<RxToken>& pool,
                              const RxExprSpan& span,
                              const RxEvalEnv& env) noexcept {
    // D-R9: len == 0 is the documented "no expression" encoding — it must
    // be safe regardless of caller discipline (the R2 validator caught the
    // uninitialized-read alternative). Callers may ALSO skip empty spans;
    // this branch is the contract, their skip is the optimization.
    if (span.len <= 0) return 0.0;
    double st[kRxMaxStackDepth];
    int sp = 0;
    const RxToken* t   = pool.data() + span.begin;
    const RxToken* end = t + span.len;
    for (; t != end; ++t) {
        switch (t->op) {
            case RxToken::PUSH_NUM:     st[sp++] = t->value; break;
            case RxToken::PUSH_SPECIES: st[sp++] = env.species[t->idx]; break;
            case RxToken::PUSH_COEF:    st[sp++] = env.coefs[t->idx]; break;
            case RxToken::PUSH_TERM:    st[sp++] = env.terms[t->idx]; break;
            case RxToken::PUSH_HYDVAR:  st[sp++] = env.hydvar[t->idx]; break;
            case RxToken::PUSH_POLLUT:  st[sp++] = env.pollutants[t->idx]; break;
            case RxToken::ADD: --sp; st[sp - 1] += st[sp]; break;
            case RxToken::SUB: --sp; st[sp - 1] -= st[sp]; break;
            case RxToken::MUL: --sp; st[sp - 1] *= st[sp]; break;
            case RxToken::DIV: --sp; st[sp - 1] /= st[sp]; break;
            case RxToken::POW: --sp; st[sp - 1] = std::pow(st[sp - 1], st[sp]); break;
            case RxToken::NEG: st[sp - 1] = -st[sp - 1]; break;
            case RxToken::F_EXP:   st[sp - 1] = std::exp(st[sp - 1]); break;
            case RxToken::F_LOG:   st[sp - 1] = std::log(st[sp - 1]); break;
            case RxToken::F_LOG10: st[sp - 1] = std::log10(st[sp - 1]); break;
            case RxToken::F_SQRT:  st[sp - 1] = std::sqrt(st[sp - 1]); break;
            case RxToken::F_ABS:   st[sp - 1] = std::fabs(st[sp - 1]); break;
            case RxToken::F_SGN:
                st[sp - 1] = (st[sp - 1] > 0.0) ? 1.0
                             : (st[sp - 1] < 0.0) ? -1.0 : 0.0;
                break;
            case RxToken::F_STEP:  st[sp - 1] = (st[sp - 1] > 0.0) ? 1.0 : 0.0; break;
            case RxToken::F_SIN:   st[sp - 1] = std::sin(st[sp - 1]); break;
            case RxToken::F_COS:   st[sp - 1] = std::cos(st[sp - 1]); break;
            case RxToken::F_TAN:   st[sp - 1] = std::tan(st[sp - 1]); break;
            case RxToken::F_MIN:
                --sp; st[sp - 1] = (st[sp - 1] < st[sp]) ? st[sp - 1] : st[sp];
                break;
            case RxToken::F_MAX:
                --sp; st[sp - 1] = (st[sp - 1] > st[sp]) ? st[sp - 1] : st[sp];
                break;
        }
    }
    return st[0];
}

}  // namespace openswmm::transport
