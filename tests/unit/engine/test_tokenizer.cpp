/**
 * @file test_tokenizer.cpp
 * @brief Unit tests for the multi-delimiter Tokenizer.
 *
 * @details Tests the openswmm::input::Tokenizer class which splits SWMM input
 *          lines on comma, horizontal tab, and one-or-more spaces. This class
 *          replaces the legacy whitespace-only tokenizer in input.c.
 *
 * @see src/engine/input/Tokenizer.hpp
 * @see Legacy reference: src/solver/input.c — getToken() function
 * @ingroup engine_input
 */

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "../../src/engine/input/Tokenizer.hpp"

using ::testing::ElementsAre;
using ::testing::IsEmpty;
using openswmm::input::Tokenizer;

namespace {

// ============================================================================
// strip_comment
// ============================================================================

TEST(TokenizerTest, StripSemicolonComment) {
    auto stripped = Tokenizer::strip_comment("J1 100.0 ; this is a comment");
    EXPECT_EQ(stripped, "J1 100.0 ");
}

TEST(TokenizerTest, StripDoubleSemicolonComment) {
    auto stripped = Tokenizer::strip_comment("J1 100.0 ;; extended comment");
    EXPECT_EQ(stripped, "J1 100.0 ");
}

TEST(TokenizerTest, StripCommentPreservesQuotedSemicolon) {
    // Semicolon inside a quoted string should NOT be treated as a comment
    auto stripped = Tokenizer::strip_comment(R"("val;ue" real_token)");
    EXPECT_EQ(stripped, R"("val;ue" real_token)");
}

TEST(TokenizerTest, StripCommentFullLineComment) {
    auto stripped = Tokenizer::strip_comment(";; entire line is a comment");
    EXPECT_EQ(stripped, "");
}

TEST(TokenizerTest, StripCommentNoComment) {
    auto stripped = Tokenizer::strip_comment("J1 100.0 0.5");
    EXPECT_EQ(stripped, "J1 100.0 0.5");
}

// ============================================================================
// tokenize — whitespace delimiters
// ============================================================================

TEST(TokenizerTest, SpaceDelimitedTokens) {
    auto tokens = Tokenizer::tokenize("J1 100.0 0.5 JUNCTION");
    EXPECT_THAT(tokens, ElementsAre("J1", "100.0", "0.5", "JUNCTION"));
}

TEST(TokenizerTest, MultipleSpacesBetweenTokens) {
    auto tokens = Tokenizer::tokenize("J1   100.0     0.5");
    EXPECT_THAT(tokens, ElementsAre("J1", "100.0", "0.5"));
}

TEST(TokenizerTest, TabDelimitedTokens) {
    auto tokens = Tokenizer::tokenize("J1\t100.0\t0.5\tJUNCTION");
    EXPECT_THAT(tokens, ElementsAre("J1", "100.0", "0.5", "JUNCTION"));
}

TEST(TokenizerTest, LeadingTrailingWhitespaceStripped) {
    auto tokens = Tokenizer::tokenize("   J1   100.0   ");
    EXPECT_THAT(tokens, ElementsAre("J1", "100.0"));
}

// ============================================================================
// tokenize — comma delimiters
// ============================================================================

TEST(TokenizerTest, CommaDelimitedTokens) {
    auto tokens = Tokenizer::tokenize("J1,100.0,0.5,JUNCTION");
    EXPECT_THAT(tokens, ElementsAre("J1", "100.0", "0.5", "JUNCTION"));
}

TEST(TokenizerTest, CommaWithSpaceDelimitedTokens) {
    auto tokens = Tokenizer::tokenize("J1, 100.0, 0.5, JUNCTION");
    EXPECT_THAT(tokens, ElementsAre("J1", "100.0", "0.5", "JUNCTION"));
}

// ============================================================================
// tokenize — mixed delimiters
// ============================================================================

TEST(TokenizerTest, MixedDelimiters) {
    auto tokens = Tokenizer::tokenize("J1, 100.0\t0.5  JUNCTION");
    EXPECT_THAT(tokens, ElementsAre("J1", "100.0", "0.5", "JUNCTION"));
}

// ============================================================================
// tokenize — empty / comment-only lines
// ============================================================================

TEST(TokenizerTest, EmptyLineReturnsNoTokens) {
    auto tokens = Tokenizer::tokenize("");
    EXPECT_THAT(tokens, IsEmpty());
}

TEST(TokenizerTest, WhitespaceOnlyLineReturnsNoTokens) {
    auto tokens = Tokenizer::tokenize("   \t  ");
    EXPECT_THAT(tokens, IsEmpty());
}

TEST(TokenizerTest, CommentOnlyLineReturnsNoTokens) {
    auto tokens = Tokenizer::tokenize(";; entire line is a comment");
    EXPECT_THAT(tokens, IsEmpty());
}

// ============================================================================
// tokenize — quoted strings
// ============================================================================

TEST(TokenizerTest, QuotedStringPreservesSpaces) {
    auto tokens = Tokenizer::tokenize(R"(FLAG_A "My Flag Name" BOOLEAN)");
    EXPECT_THAT(tokens, ElementsAre("FLAG_A", "My Flag Name", "BOOLEAN"));
}

TEST(TokenizerTest, QuotedPathWithColon) {
    // FILE "path/to/rain.csv:EAST_STATION"
    auto tokens = Tokenizer::tokenize(R"(FILE "path/to/rain.csv:EAST_STATION")");
    EXPECT_THAT(tokens, ElementsAre("FILE", "path/to/rain.csv:EAST_STATION"));
}

// ============================================================================
// tokenize_views
// ============================================================================

TEST(TokenizerTest, TokenizeViewsBasic) {
    auto views = Tokenizer::tokenize_views("J1 100.0 0.5");
    ASSERT_EQ(views.size(), 3u);
    EXPECT_EQ(views[0], "J1");
    EXPECT_EQ(views[1], "100.0");
    EXPECT_EQ(views[2], "0.5");
}

// ============================================================================
// trim
// ============================================================================

TEST(TokenizerTest, TrimLeadingSpaces) {
    EXPECT_EQ(Tokenizer::trim("   hello"), "hello");
}

TEST(TokenizerTest, TrimTrailingSpaces) {
    EXPECT_EQ(Tokenizer::trim("hello   "), "hello");
}

TEST(TokenizerTest, TrimBothSides) {
    EXPECT_EQ(Tokenizer::trim("  hello world  "), "hello world");
}

TEST(TokenizerTest, TrimEmpty) {
    EXPECT_EQ(Tokenizer::trim(""), "");
}

// ============================================================================
// to_upper
// ============================================================================

TEST(TokenizerTest, ToUpperMixedCase) {
    EXPECT_EQ(Tokenizer::to_upper("dynwave"), "DYNWAVE");
}

TEST(TokenizerTest, ToUpperAlreadyUpper) {
    EXPECT_EQ(Tokenizer::to_upper("CFS"), "CFS");
}

// ============================================================================
// is_numeric / is_boolean / parse_boolean
// ============================================================================

TEST(TokenizerTest, IsNumericInteger) {
    EXPECT_TRUE(Tokenizer::is_numeric("42"));
}

TEST(TokenizerTest, IsNumericFloat) {
    EXPECT_TRUE(Tokenizer::is_numeric("3.14"));
}

TEST(TokenizerTest, IsNumericNegative) {
    EXPECT_TRUE(Tokenizer::is_numeric("-1.5e-3"));
}

TEST(TokenizerTest, IsNumericText) {
    EXPECT_FALSE(Tokenizer::is_numeric("JUNCTION"));
}

TEST(TokenizerTest, IsBooleanYesNo) {
    EXPECT_TRUE(Tokenizer::is_boolean("YES"));
    EXPECT_TRUE(Tokenizer::is_boolean("NO"));
    EXPECT_TRUE(Tokenizer::is_boolean("yes"));
    EXPECT_TRUE(Tokenizer::is_boolean("no"));
}

TEST(TokenizerTest, IsBooleanTrueFalse) {
    EXPECT_TRUE(Tokenizer::is_boolean("TRUE"));
    EXPECT_TRUE(Tokenizer::is_boolean("FALSE"));
}

TEST(TokenizerTest, IsBooleanOneZero) {
    EXPECT_TRUE(Tokenizer::is_boolean("1"));
    EXPECT_TRUE(Tokenizer::is_boolean("0"));
}

TEST(TokenizerTest, IsBooleanOtherText) {
    EXPECT_FALSE(Tokenizer::is_boolean("MAYBE"));
    EXPECT_FALSE(Tokenizer::is_boolean("2"));
}

TEST(TokenizerTest, ParseBooleanTrue) {
    EXPECT_TRUE(Tokenizer::parse_boolean("YES"));
    EXPECT_TRUE(Tokenizer::parse_boolean("yes"));
    EXPECT_TRUE(Tokenizer::parse_boolean("TRUE"));
    EXPECT_TRUE(Tokenizer::parse_boolean("1"));
}

TEST(TokenizerTest, ParseBooleanFalse) {
    EXPECT_FALSE(Tokenizer::parse_boolean("NO"));
    EXPECT_FALSE(Tokenizer::parse_boolean("FALSE"));
    EXPECT_FALSE(Tokenizer::parse_boolean("0"));
}

} /* anonymous namespace */
