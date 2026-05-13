/**
 * @file test_mathexpr.cpp
 * @brief Unit tests for the MathExpr parser and evaluator.
 *
 * @details Tests:
 *   - Parsing infix to postfix (shunting-yard)
 *   - Evaluation with named variable lookup
 *   - Variable binding (name to index resolution)
 *   - evaluate_fast() (stack-free, indexed variable access)
 *   - All supported operators and functions
 *   - Edge cases: division by zero, negative sqrt, empty expression
 *   - Consistency between evaluate() and evaluate_fast()
 *
 * @see src/engine/math/MathExpr.hpp
 * @ingroup engine_math
 */

#include <gtest/gtest.h>
#include <cmath>
#include <string>

#include "math/MathExpr.hpp"

using namespace openswmm::mathexpr;

// ============================================================================
// Parsing
// ============================================================================

TEST(MathExprParse, SimpleAddition) {
    Expression expr;
    ASSERT_EQ(parse("1 + 2", expr), 0);
    EXPECT_TRUE(expr.valid);
    EXPECT_EQ(expr.postfix.size(), 3u);
}

TEST(MathExprParse, NestedParentheses) {
    Expression expr;
    ASSERT_EQ(parse("(1 + 2) * (3 - 4)", expr), 0);
    EXPECT_TRUE(expr.valid);
}

TEST(MathExprParse, UnaryMinus) {
    Expression expr;
    ASSERT_EQ(parse("-x", expr), 0);
    EXPECT_TRUE(expr.valid);
}

TEST(MathExprParse, FunctionCall) {
    Expression expr;
    ASSERT_EQ(parse("sqrt(x)", expr), 0);
    EXPECT_TRUE(expr.valid);
}

TEST(MathExprParse, MismatchedParens) {
    Expression expr;
    EXPECT_EQ(parse("(1 + 2", expr), -1);
    EXPECT_FALSE(expr.valid);
}

TEST(MathExprParse, EmptyString) {
    Expression expr;
    ASSERT_EQ(parse("", expr), 0);
    EXPECT_TRUE(expr.valid);
    EXPECT_TRUE(expr.postfix.empty());
}

// ============================================================================
// Evaluation with named lookup
// ============================================================================

TEST(MathExprEval, ConstantExpression) {
    Expression expr;
    ASSERT_EQ(parse("3.14", expr), 0);
    double result = evaluate(expr, [](const std::string&) { return 0.0; });
    EXPECT_DOUBLE_EQ(result, 3.14);
}

TEST(MathExprEval, ArithmeticOperators) {
    Expression expr;
    ASSERT_EQ(parse("2 + 3 * 4", expr), 0);
    double result = evaluate(expr, [](const std::string&) { return 0.0; });
    EXPECT_DOUBLE_EQ(result, 14.0);
}

TEST(MathExprEval, ParenthesesOverridePrecedence) {
    Expression expr;
    ASSERT_EQ(parse("(2 + 3) * 4", expr), 0);
    double result = evaluate(expr, [](const std::string&) { return 0.0; });
    EXPECT_DOUBLE_EQ(result, 20.0);
}

TEST(MathExprEval, Power) {
    Expression expr;
    ASSERT_EQ(parse("2 ^ 3", expr), 0);
    double result = evaluate(expr, [](const std::string&) { return 0.0; });
    EXPECT_DOUBLE_EQ(result, 8.0);
}

TEST(MathExprEval, UnaryMinus) {
    Expression expr;
    ASSERT_EQ(parse("-5 + 3", expr), 0);
    double result = evaluate(expr, [](const std::string&) { return 0.0; });
    EXPECT_DOUBLE_EQ(result, -2.0);
}

TEST(MathExprEval, VariableLookup) {
    Expression expr;
    ASSERT_EQ(parse("x * 2 + y", expr), 0);
    double result = evaluate(expr, [](const std::string& name) {
        if (name == "x") return 3.0;
        if (name == "y") return 7.0;
        return 0.0;
    });
    EXPECT_DOUBLE_EQ(result, 13.0);
}

TEST(MathExprEval, DivisionByZero) {
    Expression expr;
    ASSERT_EQ(parse("1 / 0", expr), 0);
    double result = evaluate(expr, [](const std::string&) { return 0.0; });
    EXPECT_DOUBLE_EQ(result, 0.0);
}

// ============================================================================
// Built-in functions
// ============================================================================

TEST(MathExprFunctions, Sqrt) {
    Expression expr;
    ASSERT_EQ(parse("sqrt(16)", expr), 0);
    double result = evaluate(expr, [](const std::string&) { return 0.0; });
    EXPECT_DOUBLE_EQ(result, 4.0);
}

TEST(MathExprFunctions, SqrtNegative) {
    Expression expr;
    ASSERT_EQ(parse("sqrt(-1)", expr), 0);
    double result = evaluate(expr, [](const std::string&) { return 0.0; });
    EXPECT_DOUBLE_EQ(result, 0.0);
}

TEST(MathExprFunctions, Exp) {
    Expression expr;
    ASSERT_EQ(parse("exp(0)", expr), 0);
    double result = evaluate(expr, [](const std::string&) { return 0.0; });
    EXPECT_DOUBLE_EQ(result, 1.0);
}

TEST(MathExprFunctions, Log) {
    Expression expr;
    ASSERT_EQ(parse("log(1)", expr), 0);
    double result = evaluate(expr, [](const std::string&) { return 0.0; });
    EXPECT_DOUBLE_EQ(result, 0.0);
}

TEST(MathExprFunctions, LogNegative) {
    Expression expr;
    ASSERT_EQ(parse("log(-1)", expr), 0);
    double result = evaluate(expr, [](const std::string&) { return 0.0; });
    EXPECT_DOUBLE_EQ(result, 0.0);
}

TEST(MathExprFunctions, Abs) {
    Expression expr;
    ASSERT_EQ(parse("abs(-5.5)", expr), 0);
    double result = evaluate(expr, [](const std::string&) { return 0.0; });
    EXPECT_DOUBLE_EQ(result, 5.5);
}

TEST(MathExprFunctions, Sgn) {
    Expression e1, e2, e3;
    ASSERT_EQ(parse("sgn(-3)", e1), 0);
    EXPECT_DOUBLE_EQ(evaluate(e1, [](const std::string&) { return 0.0; }), -1.0);

    ASSERT_EQ(parse("sgn(7)", e2), 0);
    EXPECT_DOUBLE_EQ(evaluate(e2, [](const std::string&) { return 0.0; }), 1.0);

    ASSERT_EQ(parse("sgn(0)", e3), 0);
    EXPECT_DOUBLE_EQ(evaluate(e3, [](const std::string&) { return 0.0; }), 0.0);
}

TEST(MathExprFunctions, Step) {
    Expression e1, e2;
    ASSERT_EQ(parse("step(-0.5)", e1), 0);
    EXPECT_DOUBLE_EQ(evaluate(e1, [](const std::string&) { return 0.0; }), 0.0);

    ASSERT_EQ(parse("step(0.5)", e2), 0);
    EXPECT_DOUBLE_EQ(evaluate(e2, [](const std::string&) { return 0.0; }), 1.0);
}

TEST(MathExprFunctions, MinMax) {
    Expression e1, e2;
    ASSERT_EQ(parse("min(3, 7)", e1), 0);
    EXPECT_DOUBLE_EQ(evaluate(e1, [](const std::string&) { return 0.0; }), 3.0);

    ASSERT_EQ(parse("max(3, 7)", e2), 0);
    EXPECT_DOUBLE_EQ(evaluate(e2, [](const std::string&) { return 0.0; }), 7.0);
}

TEST(MathExprFunctions, SinCosTan) {
    Expression e1, e2;
    ASSERT_EQ(parse("sin(0)", e1), 0);
    EXPECT_NEAR(evaluate(e1, [](const std::string&) { return 0.0; }), 0.0, 1e-15);

    ASSERT_EQ(parse("cos(0)", e2), 0);
    EXPECT_DOUBLE_EQ(evaluate(e2, [](const std::string&) { return 0.0; }), 1.0);
}

// ============================================================================
// Variable binding + evaluate_fast
// ============================================================================

TEST(MathExprFast, BindVariables) {
    Expression expr;
    ASSERT_EQ(parse("x * 2 + y", expr), 0);

    const char* names[] = {"x", "y"};
    int bound = bind_variables(expr, names, 2);
    EXPECT_EQ(bound, 2);

    for (const auto& tok : expr.postfix) {
        if (tok.type == TokenType::VARIABLE) {
            if (tok.var_name == "x") EXPECT_EQ(tok.var_idx, 0);
            if (tok.var_name == "y") EXPECT_EQ(tok.var_idx, 1);
        }
    }
}

TEST(MathExprFast, BindCaseInsensitive) {
    Expression expr;
    ASSERT_EQ(parse("HGW + hsw", expr), 0);

    const char* names[] = {"HGW", "HSW"};
    int bound = bind_variables(expr, names, 2);
    EXPECT_EQ(bound, 2);
}

TEST(MathExprFast, EvaluateFastSimple) {
    Expression expr;
    ASSERT_EQ(parse("x * 2 + y", expr), 0);

    const char* names[] = {"x", "y"};
    bind_variables(expr, names, 2);

    double vars[] = {3.0, 7.0};
    EXPECT_DOUBLE_EQ(evaluate_fast(expr, vars), 13.0);
}

TEST(MathExprFast, EvaluateFastGWExpression) {
    Expression expr;
    ASSERT_EQ(parse("0.001 * (HGW - HCB) ^ 2", expr), 0);

    const char* gw_vars[] = {
        "HGW", "HSW", "HCB", "HGS", "KS", "K",
        "THETA", "PHI", "FI", "FU", "A"
    };
    bind_variables(expr, gw_vars, 11);

    double vars[11] = {};
    vars[0] = 5.0;  // HGW
    vars[2] = 2.0;  // HCB
    EXPECT_NEAR(evaluate_fast(expr, vars), 0.009, 1e-12);
}

TEST(MathExprFast, EvaluateFastWithFunctions) {
    Expression expr;
    ASSERT_EQ(parse("sqrt(x) + exp(0)", expr), 0);

    const char* names[] = {"x"};
    bind_variables(expr, names, 1);

    double vars[] = {9.0};
    EXPECT_DOUBLE_EQ(evaluate_fast(expr, vars), 4.0);
}

TEST(MathExprFast, EvaluateFastUnboundVariable) {
    Expression expr;
    ASSERT_EQ(parse("x + unknown", expr), 0);

    const char* names[] = {"x"};
    bind_variables(expr, names, 1);

    double vars[] = {5.0};
    EXPECT_DOUBLE_EQ(evaluate_fast(expr, vars), 5.0);
}

TEST(MathExprFast, EvaluateFastInvalid) {
    Expression expr;
    EXPECT_DOUBLE_EQ(evaluate_fast(expr, nullptr), 0.0);
}

TEST(MathExprFast, ConsistencyWithEvaluate) {
    Expression expr;
    ASSERT_EQ(parse("0.5 * (a * b + c) ^ 0.5 - abs(-d)", expr), 0);

    const char* names[] = {"a", "b", "c", "d"};
    bind_variables(expr, names, 4);

    double vars[] = {2.0, 3.0, 10.0, 1.5};

    double r_fast = evaluate_fast(expr, vars);
    double r_std = evaluate(expr, [&](const std::string& name) {
        if (name == "a") return 2.0;
        if (name == "b") return 3.0;
        if (name == "c") return 10.0;
        if (name == "d") return 1.5;
        return 0.0;
    });

    EXPECT_DOUBLE_EQ(r_fast, r_std);
}

// ============================================================================
// Stack depth computation
// ============================================================================

TEST(MathExprStack, DepthSimple) {
    Expression expr;
    ASSERT_EQ(parse("1 + 2", expr), 0);
    EXPECT_EQ(compute_max_stack_depth(expr), 2);
}

TEST(MathExprStack, DepthNested) {
    Expression expr;
    ASSERT_EQ(parse("(a + b) * (c + d)", expr), 0);
    EXPECT_GE(compute_max_stack_depth(expr), 2);
}

TEST(MathExprStack, DepthFunction) {
    Expression expr;
    ASSERT_EQ(parse("min(a, max(b, c))", expr), 0);
    EXPECT_GE(compute_max_stack_depth(expr), 2);
}

// ============================================================================
// Complex GW expressions matching legacy patterns
// ============================================================================

TEST(MathExprGW, DeepFlowExpression) {
    Expression expr;
    ASSERT_EQ(parse("0.0001 * HGW / HGS", expr), 0);

    const char* gw_vars[] = {
        "HGW", "HSW", "HCB", "HGS", "KS", "K",
        "THETA", "PHI", "FI", "FU", "A"
    };
    bind_variables(expr, gw_vars, 11);

    double vars[11] = {};
    vars[0] = 5.0;   // HGW
    vars[3] = 10.0;  // HGS

    EXPECT_NEAR(evaluate_fast(expr, vars), 0.0001 * 5.0 / 10.0, 1e-15);
}

TEST(MathExprGW, LateralFlowWithExpAndStep) {
    Expression expr;
    ASSERT_EQ(parse("0.01 * step(HGW - HCB) * exp(-0.1 * (HGS - HGW))", expr), 0);
    EXPECT_TRUE(expr.valid);

    const char* gw_vars[] = {
        "HGW", "HSW", "HCB", "HGS", "KS", "K",
        "THETA", "PHI", "FI", "FU", "A"
    };
    bind_variables(expr, gw_vars, 11);

    double vars[11] = {};
    vars[0] = 5.0;   // HGW
    vars[2] = 2.0;   // HCB
    vars[3] = 10.0;  // HGS

    double expected = 0.01 * 1.0 * std::exp(-0.1 * (10.0 - 5.0));
    EXPECT_NEAR(evaluate_fast(expr, vars), expected, 1e-12);
}

TEST(MathExprGW, ScientificNotation) {
    Expression expr;
    ASSERT_EQ(parse("1.5e-3 * HGW", expr), 0);

    const char* names[] = {"HGW"};
    bind_variables(expr, names, 1);

    double vars[] = {100.0};
    EXPECT_NEAR(evaluate_fast(expr, vars), 0.15, 1e-15);
}
