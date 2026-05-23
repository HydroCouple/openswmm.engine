/**
 * @file test_treatment.cpp
 * @brief Unit tests for treatment expression parsing, evaluation, and application.
 *
 * @see src/engine/quality/Treatment.hpp
 * @ingroup engine_quality
 */

#include <gtest/gtest.h>
#include <cmath>
#include <string>

#include "quality/Treatment.hpp"
#include "data/QualityData.hpp"

using namespace openswmm;
using namespace openswmm::treatment;

// ============================================================================
// Expression parsing
// ============================================================================

TEST(TreatParse, RemovalExpression) {
    TreatExpr expr;
    int err = parse("R = 0.5", expr);
    EXPECT_EQ(err, 0);
    EXPECT_TRUE(expr.is_removal);
    EXPECT_FALSE(expr.tokens.empty());
}

TEST(TreatParse, ConcentrationExpression) {
    TreatExpr expr;
    int err = parse("C = 10.0", expr);
    EXPECT_EQ(err, 0);
    EXPECT_FALSE(expr.is_removal);
}

TEST(TreatParse, ExpressionWithVariables) {
    TreatExpr expr;
    int err = parse("R = 1.0 - exp(-0.5 * HRT)", expr);
    EXPECT_EQ(err, 0);
    EXPECT_TRUE(expr.is_removal);
    EXPECT_GT(static_cast<int>(expr.tokens.size()), 3);
}

TEST(TreatParse, ExpressionWithArithmetic) {
    TreatExpr expr;
    int err = parse("C = C * 0.5 + 2.0", expr);
    EXPECT_EQ(err, 0);
    EXPECT_FALSE(expr.is_removal);
}

TEST(TreatParse, EmptyExpressionFails) {
    TreatExpr expr;
    int err = parse("", expr);
    EXPECT_NE(err, 0);
}

TEST(TreatParse, MissingEqualsFails) {
    TreatExpr expr;
    int err = parse("R 0.5", expr);
    EXPECT_NE(err, 0);
}

// ============================================================================
// Expression evaluation
// ============================================================================

TEST(TreatEval, ConstantRemoval) {
    TreatExpr expr;
    ASSERT_EQ(parse("R = 0.75", expr), 0);
    double r = evaluate(expr, 100.0, 300.0, 1.0, 10.0, 500.0, 3.0);
    EXPECT_NEAR(r, 0.75, 1e-10);
}

TEST(TreatEval, ConstantConcentration) {
    TreatExpr expr;
    ASSERT_EQ(parse("C = 5.0", expr), 0);
    double c = evaluate(expr, 100.0, 300.0, 1.0, 10.0, 500.0, 3.0);
    EXPECT_NEAR(c, 5.0, 1e-10);
}

TEST(TreatEval, ConcentrationVariable) {
    TreatExpr expr;
    ASSERT_EQ(parse("C = C * 0.5", expr), 0);
    double c = evaluate(expr, 100.0, 300.0, 1.0, 10.0, 500.0, 3.0);
    EXPECT_NEAR(c, 50.0, 1e-10);
}

TEST(TreatEval, HRTVariable) {
    TreatExpr expr;
    ASSERT_EQ(parse("R = 1.0 - exp(-0.5 * HRT)", expr), 0);

    // HRT = 2.0 hours → R = 1 - exp(-1.0) ≈ 0.6321
    double r = evaluate(expr, 100.0, 300.0, 2.0, 10.0, 500.0, 3.0);
    EXPECT_NEAR(r, 1.0 - std::exp(-1.0), 1e-6);
}

TEST(TreatEval, DTVariable) {
    TreatExpr expr;
    ASSERT_EQ(parse("R = DT / 600.0", expr), 0);
    // DT = 300 → R = 0.5
    double r = evaluate(expr, 100.0, 300.0, 0.0, 0.0, 0.0, 0.0);
    EXPECT_NEAR(r, 0.5, 1e-10);
}

TEST(TreatEval, FlowVariable) {
    TreatExpr expr;
    ASSERT_EQ(parse("C = Q * 2.0", expr), 0);
    double c = evaluate(expr, 100.0, 300.0, 0.0, 5.0, 0.0, 0.0);
    EXPECT_NEAR(c, 10.0, 1e-10);
}

TEST(TreatEval, ExpFunction) {
    TreatExpr expr;
    ASSERT_EQ(parse("R = exp(-1.0)", expr), 0);
    double r = evaluate(expr, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    EXPECT_NEAR(r, std::exp(-1.0), 1e-10);
}

TEST(TreatEval, SqrtFunction) {
    TreatExpr expr;
    ASSERT_EQ(parse("C = sqrt(C)", expr), 0);
    double c = evaluate(expr, 16.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    EXPECT_NEAR(c, 4.0, 1e-10);
}

TEST(TreatEval, StepFunction) {
    TreatExpr expr;
    ASSERT_EQ(parse("R = step(HRT - 1.0)", expr), 0);
    // HRT < 1.0 → step = 0
    double r1 = evaluate(expr, 0.0, 0.0, 0.5, 0.0, 0.0, 0.0);
    EXPECT_NEAR(r1, 0.0, 1e-10);
    // HRT > 1.0 → step = 1
    double r2 = evaluate(expr, 0.0, 0.0, 2.0, 0.0, 0.0, 0.0);
    EXPECT_NEAR(r2, 1.0, 1e-10);
}

// ============================================================================
// Treatment application (clamping behavior)
// ============================================================================

TEST(TreatApply, RemovalClampedToZeroOne) {
    TreatExpr expr;
    ASSERT_EQ(parse("R = 0.8", expr), 0);
    double cOut = applyTreatment(expr, 100.0, 300.0, 1.0, 10.0, 500.0, 3.0);
    // R = 0.8 → cOut = 100 * (1 - 0.8) = 20
    EXPECT_NEAR(cOut, 20.0, 1e-6);
}

TEST(TreatApply, RemovalClampedAtOne) {
    TreatExpr expr;
    ASSERT_EQ(parse("R = 1.5", expr), 0);  // > 1.0 clamped
    double cOut = applyTreatment(expr, 100.0, 300.0, 0.0, 0.0, 0.0, 0.0);
    EXPECT_NEAR(cOut, 0.0, 1e-6)
        << "R > 1 should be clamped to 1 → zero outlet concentration";
}

TEST(TreatApply, ConcentrationClampedToInput) {
    TreatExpr expr;
    ASSERT_EQ(parse("C = 200.0", expr), 0);  // > c_in, should clamp
    double cOut = applyTreatment(expr, 100.0, 300.0, 0.0, 0.0, 0.0, 0.0);
    EXPECT_LE(cOut, 100.0)
        << "C-type treatment should not increase concentration beyond input";
}

TEST(TreatApply, ZeroInputNoTreatment) {
    TreatExpr expr;
    ASSERT_EQ(parse("R = 0.5", expr), 0);
    double cOut = applyTreatment(expr, 0.0, 300.0, 0.0, 0.0, 0.0, 0.0);
    EXPECT_NEAR(cOut, 0.0, 1e-10)
        << "Zero input concentration should produce zero output";
}

TEST(TreatApply, HRTDependentRemoval) {
    TreatExpr expr;
    ASSERT_EQ(parse("R = 1.0 - exp(-0.5 * HRT)", expr), 0);

    // Low HRT → low removal
    double c_low = applyTreatment(expr, 100.0, 300.0, 0.1, 0.0, 0.0, 0.0);
    // High HRT → high removal
    double c_high = applyTreatment(expr, 100.0, 300.0, 10.0, 0.0, 0.0, 0.0);

    EXPECT_GT(c_low, c_high)
        << "Higher HRT should produce more removal (lower outlet conc)";
}

// ============================================================================
// TreatmentData integration
// ============================================================================

TEST(TreatmentData, ResizeSetsCorrectDimensions) {
    TreatmentData td;
    td.resize(5, 3);
    EXPECT_EQ(td.n_nodes, 5);
    EXPECT_EQ(td.n_pollutants, 3);
    EXPECT_EQ(static_cast<int>(td.expressions.size()), 15);
    EXPECT_EQ(static_cast<int>(td.compiled.size()), 15);
    EXPECT_EQ(static_cast<int>(td.has_treatment.size()), 5);
    EXPECT_EQ(static_cast<int>(td.cin.size()), 3);
    EXPECT_EQ(static_cast<int>(td.removal.size()), 3);
}

TEST(TreatmentData, HasAnyDetectsExpression) {
    TreatmentData td;
    td.resize(2, 2);
    EXPECT_FALSE(td.hasAny());

    td.expressions[1] = "R = 0.5";
    EXPECT_TRUE(td.hasAny());
}

TEST(TreatmentData, CompilationProducesTokens) {
    TreatmentData td;
    td.resize(1, 1);
    td.expressions[0] = "R = 0.75";

    TreatExpr te;
    ASSERT_EQ(parse(td.expressions[0], te), 0);
    te.pollutant_idx = 0;
    td.compiled[0] = std::move(te);
    td.has_treatment[0] = true;

    EXPECT_TRUE(td.has_treatment[0]);
    EXPECT_FALSE(td.compiled[0].tokens.empty());
    EXPECT_TRUE(td.compiled[0].is_removal);
}

// ============================================================================
// Co-treatment (R_pollutant references)
// ============================================================================

// Helper: mock pollutant lookup
static int mock_pollut_lookup(const std::string& name) {
    if (name == "TSS") return 0;
    if (name == "Lead") return 1;
    if (name == "TP") return 2;
    return -1;
}

TEST(CoTreatment, ParseR_PollutantReference) {
    TreatExpr expr;
    int err = parse("R = 0.5 * R_TSS", expr, mock_pollut_lookup);
    EXPECT_EQ(err, 0);
    EXPECT_TRUE(expr.is_removal);

    // Should contain an R_POLLUT token with pollut_ref = 0 (TSS)
    bool found = false;
    for (const auto& tok : expr.tokens) {
        if (tok.var == TreatVar::R_POLLUT && tok.pollut_ref == 0)
            found = true;
    }
    EXPECT_TRUE(found) << "Should contain R_POLLUT token referencing TSS (index 0)";
}

TEST(CoTreatment, ParseC_PollutantReference) {
    TreatExpr expr;
    int err = parse("C = C_Lead * 0.5", expr, mock_pollut_lookup);
    EXPECT_EQ(err, 0);
    EXPECT_FALSE(expr.is_removal);

    bool found = false;
    for (const auto& tok : expr.tokens) {
        if (tok.var == TreatVar::C_POLLUT && tok.pollut_ref == 1)
            found = true;
    }
    EXPECT_TRUE(found) << "Should contain C_POLLUT token referencing Lead (index 1)";
}

TEST(CoTreatment, EvaluateR_POLLUT) {
    TreatExpr expr;
    ASSERT_EQ(parse("R = 0.5 * R_TSS", expr, mock_pollut_lookup), 0);

    // Provide removal array: TSS removal = 0.8, Lead removal = -1 (not computed)
    double cin[3]     = {100.0, 50.0, 30.0};
    double removal[3] = {0.8, -1.0, -1.0};

    double result = evaluate(expr, 50.0, 300.0, 1.0, 10.0, 500.0, 3.0,
                             cin, removal, 3);
    // R = 0.5 * 0.8 = 0.4
    EXPECT_NEAR(result, 0.4, 1e-10);
}

TEST(CoTreatment, EvaluateC_POLLUT) {
    TreatExpr expr;
    ASSERT_EQ(parse("C = C_Lead * 0.1", expr, mock_pollut_lookup), 0);

    double cin[3]     = {100.0, 50.0, 30.0};
    double removal[3] = {0.0, 0.0, 0.0};

    // C_Lead = cin[1] = 50.0 → C = 50 * 0.1 = 5.0
    double result = evaluate(expr, 100.0, 300.0, 1.0, 10.0, 500.0, 3.0,
                             cin, removal, 3);
    EXPECT_NEAR(result, 5.0, 1e-10);
}

TEST(CoTreatment, UnknownPollutantFails) {
    TreatExpr expr;
    int err = parse("R = R_Unknown", expr, mock_pollut_lookup);
    EXPECT_NE(err, 0) << "Unknown pollutant reference should fail parse";
}

TEST(CoTreatment, WithoutLookupR_FailsGracefully) {
    TreatExpr expr;
    // Without pollutant lookup, R_TSS should fail (no co-treatment support)
    int err = parse("R = R_TSS", expr);
    EXPECT_NE(err, 0) << "R_TSS without pollutant lookup should fail";
}

// ============================================================================
// Mass balance integration
// ============================================================================

TEST(TreatApply, NegativeRemovalClamped) {
    TreatExpr expr;
    ASSERT_EQ(parse("R = -0.5", expr), 0);
    double cOut = applyTreatment(expr, 100.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    // Negative removal clamped to 0 → no change
    EXPECT_NEAR(cOut, 100.0, 1e-6);
}

TEST(TreatApply, FullRemovalGivesZero) {
    TreatExpr expr;
    ASSERT_EQ(parse("R = 1.0", expr), 0);
    double cOut = applyTreatment(expr, 100.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    EXPECT_NEAR(cOut, 0.0, 1e-10);
}

TEST(TreatApply, ConcentrationZeroSetsToZero) {
    TreatExpr expr;
    ASSERT_EQ(parse("C = 0.0", expr), 0);
    double cOut = applyTreatment(expr, 100.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    EXPECT_NEAR(cOut, 0.0, 1e-10);
}
