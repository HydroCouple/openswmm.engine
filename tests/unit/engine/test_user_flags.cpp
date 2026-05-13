/**
 * @file test_user_flags.cpp
 * @brief Unit tests for [USER_FLAGS] + [USER_FLAG_VALUES] parsing and
 *        the UserFlags runtime API (ICM two-section design).
 *
 * Design:
 *   [USER_FLAGS]        — defines flag schema (name, type, description)
 *   [USER_FLAG_VALUES]  — assigns typed values to (ObjectType, ObjectName, FlagName)
 *
 * @see src/engine/core/UserFlags.hpp
 * @see src/engine/input/handlers/UserFlagsHandler.hpp
 * @see src/engine/input/handlers/UserFlagValuesHandler.hpp
 * @ingroup engine_core
 */

#include <gtest/gtest.h>

#include "../../src/engine/input/handlers/UserFlagsHandler.hpp"
#include "../../src/engine/input/handlers/UserFlagValuesHandler.hpp"
#include "../../src/engine/core/SimulationContext.hpp"
#include "../../src/engine/core/UserFlags.hpp"

using openswmm::SimulationContext;
using openswmm::UserFlagType;
using openswmm::UserFlagDef;
using openswmm::UserFlags;

namespace {

static void parse_defs(SimulationContext& ctx, std::vector<std::string> lines) {
    openswmm::input::handle_user_flags(ctx, lines);
}

static void parse_vals(SimulationContext& ctx, std::vector<std::string> lines) {
    openswmm::input::handle_user_flag_values(ctx, lines);
}

// ============================================================================
// [USER_FLAGS] — schema definition parsing
// ============================================================================

TEST(UserFlagsTest, DefineBooleanFlag) {
    SimulationContext ctx;
    parse_defs(ctx, {"INSPECTED  BOOLEAN  \"Has the object been inspected?\""});
    ASSERT_TRUE(ctx.user_flags.is_defined("INSPECTED"));
    const auto& def = ctx.user_flags.get_def("INSPECTED");
    EXPECT_EQ(def.type, UserFlagType::BOOLEAN);
    EXPECT_EQ(def.description, "Has the object been inspected?");
}

TEST(UserFlagsTest, DefineIntegerFlag) {
    SimulationContext ctx;
    parse_defs(ctx, {"PRIORITY  INTEGER  \"Maintenance priority\""});
    ASSERT_TRUE(ctx.user_flags.is_defined("PRIORITY"));
    EXPECT_EQ(ctx.user_flags.get_def("PRIORITY").type, UserFlagType::INTEGER);
}

TEST(UserFlagsTest, DefineRealFlag) {
    SimulationContext ctx;
    parse_defs(ctx, {"ROUGHNESS_ADJ  REAL  \"Site roughness multiplier\""});
    ASSERT_TRUE(ctx.user_flags.is_defined("ROUGHNESS_ADJ"));
    EXPECT_EQ(ctx.user_flags.get_def("ROUGHNESS_ADJ").type, UserFlagType::REAL);
}

TEST(UserFlagsTest, DefineStringFlag) {
    SimulationContext ctx;
    parse_defs(ctx, {"ASSET_ID  STRING  \"External asset management ID\""});
    ASSERT_TRUE(ctx.user_flags.is_defined("ASSET_ID"));
    EXPECT_EQ(ctx.user_flags.get_def("ASSET_ID").type, UserFlagType::STRING);
}

TEST(UserFlagsTest, DefineMultipleFlags) {
    SimulationContext ctx;
    parse_defs(ctx, {
        "INSPECTED     BOOLEAN  \"Field inspected?\"",
        "PRIORITY      INTEGER  \"Maintenance priority\"",
        "ROUGHNESS_ADJ REAL     \"Roughness multiplier\"",
        "ASSET_ID      STRING   \"Asset ID\"",
    });
    EXPECT_EQ(ctx.user_flags.def_count(), 4u);
    EXPECT_TRUE(ctx.user_flags.is_defined("INSPECTED"));
    EXPECT_TRUE(ctx.user_flags.is_defined("PRIORITY"));
    EXPECT_TRUE(ctx.user_flags.is_defined("ROUGHNESS_ADJ"));
    EXPECT_TRUE(ctx.user_flags.is_defined("ASSET_ID"));
}

TEST(UserFlagsTest, DefineFlagNoDescription) {
    SimulationContext ctx;
    parse_defs(ctx, {"ENABLE_LOG  BOOLEAN"});
    ASSERT_TRUE(ctx.user_flags.is_defined("ENABLE_LOG"));
    EXPECT_TRUE(ctx.user_flags.get_def("ENABLE_LOG").description.empty());
}

TEST(UserFlagsTest, DefineFlagCaseInsensitiveName) {
    SimulationContext ctx;
    parse_defs(ctx, {"priority  INTEGER"});
    // Stored uppercase
    EXPECT_TRUE(ctx.user_flags.is_defined("PRIORITY"));
    EXPECT_FALSE(ctx.user_flags.is_defined("priority"));
}

TEST(UserFlagsTest, RedefineOverwrites) {
    SimulationContext ctx;
    parse_defs(ctx, {
        "MY_FLAG  BOOLEAN  \"first\"",
        "MY_FLAG  INTEGER  \"second\"",
    });
    EXPECT_EQ(ctx.user_flags.def_count(), 1u);
    EXPECT_EQ(ctx.user_flags.get_def("MY_FLAG").type, UserFlagType::INTEGER);
    EXPECT_EQ(ctx.user_flags.get_def("MY_FLAG").description, "second");
}

TEST(UserFlagsTest, UnknownTypeSetsWarning) {
    SimulationContext ctx;
    parse_defs(ctx, {"FOO  BADTYPE"});
    EXPECT_NE(ctx.warning_code, 0);
}

// ============================================================================
// [USER_FLAG_VALUES] — per-object value assignment
// ============================================================================

TEST(UserFlagsTest, AssignBooleanValue) {
    SimulationContext ctx;
    parse_defs(ctx, {"INSPECTED  BOOLEAN"});
    parse_vals(ctx, {"NODE  J1  INSPECTED  YES"});
    ASSERT_TRUE(ctx.user_flags.has_value("NODE", "J1", "INSPECTED"));
    const auto& v = ctx.user_flags.get_value("NODE", "J1", "INSPECTED");
    EXPECT_TRUE(std::get<bool>(v));
}

TEST(UserFlagsTest, AssignBooleanValueFalse) {
    SimulationContext ctx;
    parse_defs(ctx, {"INSPECTED  BOOLEAN"});
    parse_vals(ctx, {"SUBCATCHMENT  S_WEST  INSPECTED  NO"});
    const auto& v = ctx.user_flags.get_value("SUBCATCHMENT", "S_WEST", "INSPECTED");
    EXPECT_FALSE(std::get<bool>(v));
}

TEST(UserFlagsTest, AssignIntegerValue) {
    SimulationContext ctx;
    parse_defs(ctx, {"PRIORITY  INTEGER"});
    parse_vals(ctx, {"NODE  J1  PRIORITY  3"});
    const auto& v = ctx.user_flags.get_value("NODE", "J1", "PRIORITY");
    EXPECT_EQ(std::get<int>(v), 3);
}

TEST(UserFlagsTest, AssignNegativeIntegerValue) {
    SimulationContext ctx;
    parse_defs(ctx, {"OFFSET  INTEGER"});
    parse_vals(ctx, {"NODE  OUTFALL_1  OFFSET  -5"});
    const auto& v = ctx.user_flags.get_value("NODE", "OUTFALL_1", "OFFSET");
    EXPECT_EQ(std::get<int>(v), -5);
}

TEST(UserFlagsTest, AssignRealValue) {
    SimulationContext ctx;
    parse_defs(ctx, {"ROUGHNESS_ADJ  REAL"});
    parse_vals(ctx, {"LINK  C_MAIN  ROUGHNESS_ADJ  1.05"});
    const auto& v = ctx.user_flags.get_value("LINK", "C_MAIN", "ROUGHNESS_ADJ");
    EXPECT_DOUBLE_EQ(std::get<double>(v), 1.05);
}

TEST(UserFlagsTest, AssignStringValue) {
    SimulationContext ctx;
    parse_defs(ctx, {"ASSET_ID  STRING"});
    parse_vals(ctx, {"LINK  C_MAIN  ASSET_ID  \"AM-00341\""});
    const auto& v = ctx.user_flags.get_value("LINK", "C_MAIN", "ASSET_ID");
    EXPECT_EQ(std::get<std::string>(v), "AM-00341");
}

TEST(UserFlagsTest, MultipleObjectsSameFlag) {
    SimulationContext ctx;
    parse_defs(ctx, {"PRIORITY  INTEGER"});
    parse_vals(ctx, {
        "NODE  J1  PRIORITY  1",
        "NODE  J2  PRIORITY  3",
        "NODE  J3  PRIORITY  2",
    });
    EXPECT_EQ(std::get<int>(ctx.user_flags.get_value("NODE", "J1", "PRIORITY")), 1);
    EXPECT_EQ(std::get<int>(ctx.user_flags.get_value("NODE", "J2", "PRIORITY")), 3);
    EXPECT_EQ(std::get<int>(ctx.user_flags.get_value("NODE", "J3", "PRIORITY")), 2);
}

TEST(UserFlagsTest, SameObjectMultipleFlags) {
    SimulationContext ctx;
    parse_defs(ctx, {
        "INSPECTED  BOOLEAN",
        "PRIORITY   INTEGER",
        "ASSET_ID   STRING",
    });
    parse_vals(ctx, {
        "NODE  J1  INSPECTED  YES",
        "NODE  J1  PRIORITY   2",
        "NODE  J1  ASSET_ID   \"EX-1234\"",
    });
    EXPECT_TRUE (std::get<bool>(ctx.user_flags.get_value("NODE", "J1", "INSPECTED")));
    EXPECT_EQ   (std::get<int> (ctx.user_flags.get_value("NODE", "J1", "PRIORITY")), 2);
    EXPECT_EQ   (std::get<std::string>(ctx.user_flags.get_value("NODE", "J1", "ASSET_ID")), "EX-1234");
}

TEST(UserFlagsTest, AssignValueOverwrites) {
    SimulationContext ctx;
    parse_defs(ctx, {"PRIORITY  INTEGER"});
    parse_vals(ctx, {
        "NODE  J1  PRIORITY  1",
        "NODE  J1  PRIORITY  9",
    });
    EXPECT_EQ(std::get<int>(ctx.user_flags.get_value("NODE", "J1", "PRIORITY")), 9);
}

TEST(UserFlagsTest, ObjectTypeStoredUppercase) {
    SimulationContext ctx;
    parse_defs(ctx, {"INSPECTED  BOOLEAN"});
    parse_vals(ctx, {"node  J1  INSPECTED  YES"});
    // Stored with uppercase object type
    EXPECT_TRUE(ctx.user_flags.has_value("NODE", "J1", "INSPECTED"));
    EXPECT_FALSE(ctx.user_flags.has_value("node", "J1", "INSPECTED"));
}

TEST(UserFlagsTest, UndefinedFlagValueStoredWithWarning) {
    SimulationContext ctx;
    // No schema definition — value should still be stored as STRING
    parse_vals(ctx, {"NODE  J1  GHOST_FLAG  hello"});
    EXPECT_NE(ctx.warning_code, 0);
    EXPECT_TRUE(ctx.user_flags.has_value("NODE", "J1", "GHOST_FLAG"));
}

// ============================================================================
// UserFlags runtime API (direct class usage)
// ============================================================================

TEST(UserFlagsTest, IsDefinedReturnsFalseForMissing) {
    UserFlags flags;
    EXPECT_FALSE(flags.is_defined("NONEXISTENT"));
}

TEST(UserFlagsTest, GetDefThrowsForMissing) {
    UserFlags flags;
    EXPECT_THROW(flags.get_def("MISSING"), std::out_of_range);
}

TEST(UserFlagsTest, HasValueReturnsFalseForMissing) {
    UserFlags flags;
    EXPECT_FALSE(flags.has_value("NODE", "J1", "FOO"));
}

TEST(UserFlagsTest, GetValueThrowsForMissing) {
    UserFlags flags;
    EXPECT_THROW(flags.get_value("NODE", "J1", "FOO"), std::out_of_range);
}

TEST(UserFlagsTest, TryGetValueReturnsNulloptForMissing) {
    UserFlags flags;
    EXPECT_FALSE(flags.try_get_value("NODE", "J1", "FOO").has_value());
}

TEST(UserFlagsTest, TryGetValueReturnsValueIfPresent) {
    UserFlags flags;
    UserFlagDef def; def.name = "X"; def.type = UserFlagType::INTEGER;
    flags.define(def);
    flags.set("NODE", "J1", "X", 42);
    auto result = flags.try_get_value("NODE", "J1", "X");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(std::get<int>(*result), 42);
}

TEST(UserFlagsTest, ClearRemovesDefsAndValues) {
    UserFlags flags;
    UserFlagDef def; def.name = "A"; def.type = UserFlagType::BOOLEAN;
    flags.define(def);
    flags.set("NODE", "J1", "A", true);
    flags.clear();
    EXPECT_EQ(flags.def_count(), 0u);
    EXPECT_EQ(flags.value_count(), 0u);
    EXPECT_FALSE(flags.is_defined("A"));
    EXPECT_FALSE(flags.has_value("NODE", "J1", "A"));
}

TEST(UserFlagsTest, AllDefsInInsertionOrder) {
    UserFlags flags;
    UserFlagDef a; a.name = "A"; a.type = UserFlagType::BOOLEAN; flags.define(a);
    UserFlagDef b; b.name = "B"; b.type = UserFlagType::INTEGER; flags.define(b);
    UserFlagDef c; c.name = "C"; c.type = UserFlagType::STRING;  flags.define(c);
    ASSERT_EQ(flags.all_defs().size(), 3u);
    EXPECT_EQ(flags.all_defs()[0].name, "A");
    EXPECT_EQ(flags.all_defs()[1].name, "B");
    EXPECT_EQ(flags.all_defs()[2].name, "C");
}

TEST(UserFlagsTest, MakeKeyComposite) {
    // make_key is a raw concatenation — callers are responsible for normalizing case
    EXPECT_EQ(UserFlags::make_key("node", "J1", "inspected"), "node:J1:inspected");
    EXPECT_EQ(UserFlags::make_key("LINK", "C_MAIN", "ASSET_ID"), "LINK:C_MAIN:ASSET_ID");
}

} /* anonymous namespace */
