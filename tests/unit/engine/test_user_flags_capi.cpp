/**
 * @file test_user_flags_capi.cpp
 * @brief Unit tests for the user-flags C API surface added for the GUI
 *        (Phase 0 of openswmm.gui/docs/USER_FLAGS_UI_PLAN_2026-06-03.md):
 *
 *          swmm_userflag_def_count / swmm_userflag_def_get
 *          swmm_userflag_define    / swmm_userflag_undefine
 *          swmm_userflag_value_get / swmm_userflag_value_set
 *          swmm_userflag_value_clear
 *
 * @details Values use string form symmetric with the INP encoding:
 *          BOOLEAN as YES/NO, INTEGER as %d, REAL as %g, STRING verbatim.
 *          Object types and flag names are case-insensitive (stored
 *          uppercase); object names are case-preserved.
 *
 * @see include/openswmm/engine/openswmm_model.h
 * @see src/engine/core/UserFlags.hpp
 * @see tests/unit/engine/test_user_flags.cpp (INP handler / core container)
 */

#include <gtest/gtest.h>

#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>

namespace {

constexpr int kBool   = 0;
constexpr int kInt    = 1;
constexpr int kReal   = 2;
constexpr int kString = 3;

class UserFlagsCapiTest : public ::testing::Test {
protected:
    SWMM_Engine engine = nullptr;

    void SetUp() override {
        engine = swmm_engine_new();
        ASSERT_NE(engine, nullptr);
    }
    void TearDown() override { if (engine) swmm_engine_destroy(engine); }

    // Convenience: read a per-object value; returns "" when unset.
    std::string getValue(const char* objType, const char* objName,
                         const char* flagName, int* found) {
        char buf[256] = {};
        EXPECT_EQ(swmm_userflag_value_get(engine, objType, objName, flagName,
                                          buf, sizeof(buf), found), SWMM_OK);
        return std::string(buf);
    }
};

// ---------------------------------------------------------------------------
// Schema definitions
// ---------------------------------------------------------------------------

TEST_F(UserFlagsCapiTest, DefineAndEnumerate) {
    EXPECT_EQ(swmm_userflag_define(engine, "INSPECTED", kBool,
                                   "Field inspected?"), SWMM_OK);
    EXPECT_EQ(swmm_userflag_define(engine, "PRIORITY", kInt,
                                   "Maintenance priority"), SWMM_OK);
    EXPECT_EQ(swmm_userflag_define(engine, "ROUGHNESS_ADJ", kReal, nullptr),
              SWMM_OK);
    EXPECT_EQ(swmm_userflag_define(engine, "ASSET_ID", kString, ""), SWMM_OK);

    int count = -1;
    ASSERT_EQ(swmm_userflag_def_count(engine, &count), SWMM_OK);
    ASSERT_EQ(count, 4);

    char name[64] = {};
    char desc[128] = {};
    int  type = -1;

    ASSERT_EQ(swmm_userflag_def_get(engine, 0, name, sizeof(name), &type,
                                    desc, sizeof(desc)), SWMM_OK);
    EXPECT_STREQ(name, "INSPECTED");
    EXPECT_EQ(type, kBool);
    EXPECT_STREQ(desc, "Field inspected?");

    ASSERT_EQ(swmm_userflag_def_get(engine, 2, name, sizeof(name), &type,
                                    desc, sizeof(desc)), SWMM_OK);
    EXPECT_STREQ(name, "ROUGHNESS_ADJ");
    EXPECT_EQ(type, kReal);
    EXPECT_STREQ(desc, "");  // NULL description stored as empty
}

TEST_F(UserFlagsCapiTest, DefineNormalizesNameToUppercase) {
    EXPECT_EQ(swmm_userflag_define(engine, "inspected", kBool, ""), SWMM_OK);
    char name[64] = {};
    ASSERT_EQ(swmm_userflag_def_get(engine, 0, name, sizeof(name), nullptr,
                                    nullptr, 0), SWMM_OK);
    EXPECT_STREQ(name, "INSPECTED");
}

TEST_F(UserFlagsCapiTest, RedefineOverwrites) {
    EXPECT_EQ(swmm_userflag_define(engine, "PRIORITY", kInt, "old"), SWMM_OK);
    EXPECT_EQ(swmm_userflag_define(engine, "PRIORITY", kString, "new"), SWMM_OK);

    int count = -1;
    ASSERT_EQ(swmm_userflag_def_count(engine, &count), SWMM_OK);
    EXPECT_EQ(count, 1);

    int type = -1;
    char desc[64] = {};
    ASSERT_EQ(swmm_userflag_def_get(engine, 0, nullptr, 0, &type,
                                    desc, sizeof(desc)), SWMM_OK);
    EXPECT_EQ(type, kString);
    EXPECT_STREQ(desc, "new");
}

TEST_F(UserFlagsCapiTest, DefineRejectsBadParams) {
    EXPECT_EQ(swmm_userflag_define(engine, nullptr, kBool, ""), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_userflag_define(engine, "", kBool, ""), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_userflag_define(engine, "X", -1, ""), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_userflag_define(engine, "X", 4, ""), SWMM_ERR_BADPARAM);
}

TEST_F(UserFlagsCapiTest, DefGetBadIndex) {
    EXPECT_EQ(swmm_userflag_def_get(engine, 0, nullptr, 0, nullptr, nullptr, 0),
              SWMM_ERR_BADINDEX);
    ASSERT_EQ(swmm_userflag_define(engine, "X", kBool, ""), SWMM_OK);
    EXPECT_EQ(swmm_userflag_def_get(engine, -1, nullptr, 0, nullptr, nullptr, 0),
              SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_userflag_def_get(engine, 1, nullptr, 0, nullptr, nullptr, 0),
              SWMM_ERR_BADINDEX);
}

TEST_F(UserFlagsCapiTest, UndefineRemovesDefinitionAndValues) {
    ASSERT_EQ(swmm_userflag_define(engine, "INSPECTED", kBool, ""), SWMM_OK);
    ASSERT_EQ(swmm_userflag_define(engine, "PRIORITY", kInt, ""), SWMM_OK);
    ASSERT_EQ(swmm_userflag_value_set(engine, "NODE", "J1", "INSPECTED", "YES"),
              SWMM_OK);
    ASSERT_EQ(swmm_userflag_value_set(engine, "NODE", "J1", "PRIORITY", "2"),
              SWMM_OK);

    EXPECT_EQ(swmm_userflag_undefine(engine, "INSPECTED"), SWMM_OK);

    int count = -1;
    ASSERT_EQ(swmm_userflag_def_count(engine, &count), SWMM_OK);
    EXPECT_EQ(count, 1);

    // INSPECTED's value is gone; PRIORITY's survives.
    int found = -1;
    getValue("NODE", "J1", "INSPECTED", &found);
    EXPECT_EQ(found, 0);
    EXPECT_EQ(getValue("NODE", "J1", "PRIORITY", &found), "2");
    EXPECT_EQ(found, 1);

    // Remaining definition still retrievable at its new index.
    char name[64] = {};
    ASSERT_EQ(swmm_userflag_def_get(engine, 0, name, sizeof(name), nullptr,
                                    nullptr, 0), SWMM_OK);
    EXPECT_STREQ(name, "PRIORITY");

    EXPECT_EQ(swmm_userflag_undefine(engine, "NOPE"), SWMM_ERR_BADPARAM);
}

// ---------------------------------------------------------------------------
// Per-object values: round-trips by type
// ---------------------------------------------------------------------------

TEST_F(UserFlagsCapiTest, BooleanValueRoundTrip) {
    ASSERT_EQ(swmm_userflag_define(engine, "INSPECTED", kBool, ""), SWMM_OK);

    int found = -1;
    ASSERT_EQ(swmm_userflag_value_set(engine, "NODE", "J1", "INSPECTED", "YES"),
              SWMM_OK);
    EXPECT_EQ(getValue("NODE", "J1", "INSPECTED", &found), "YES");
    EXPECT_EQ(found, 1);

    // TRUE/1 parse as true; NO/FALSE/0 as false.
    ASSERT_EQ(swmm_userflag_value_set(engine, "NODE", "J1", "INSPECTED", "true"),
              SWMM_OK);
    EXPECT_EQ(getValue("NODE", "J1", "INSPECTED", &found), "YES");
    ASSERT_EQ(swmm_userflag_value_set(engine, "NODE", "J1", "INSPECTED", "NO"),
              SWMM_OK);
    EXPECT_EQ(getValue("NODE", "J1", "INSPECTED", &found), "NO");
}

TEST_F(UserFlagsCapiTest, IntegerValueRoundTrip) {
    ASSERT_EQ(swmm_userflag_define(engine, "PRIORITY", kInt, ""), SWMM_OK);

    int found = -1;
    ASSERT_EQ(swmm_userflag_value_set(engine, "LINK", "C_MAIN", "PRIORITY", "-5"),
              SWMM_OK);
    EXPECT_EQ(getValue("LINK", "C_MAIN", "PRIORITY", &found), "-5");
    EXPECT_EQ(found, 1);

    EXPECT_EQ(swmm_userflag_value_set(engine, "LINK", "C_MAIN", "PRIORITY",
                                      "abc"), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_userflag_value_set(engine, "LINK", "C_MAIN", "PRIORITY",
                                      "1.5"), SWMM_ERR_BADPARAM);
}

TEST_F(UserFlagsCapiTest, RealValueRoundTrip) {
    ASSERT_EQ(swmm_userflag_define(engine, "ROUGHNESS_ADJ", kReal, ""), SWMM_OK);

    int found = -1;
    ASSERT_EQ(swmm_userflag_value_set(engine, "LINK", "C_MAIN",
                                      "ROUGHNESS_ADJ", "1.05"), SWMM_OK);
    EXPECT_EQ(getValue("LINK", "C_MAIN", "ROUGHNESS_ADJ", &found), "1.05");
    EXPECT_EQ(found, 1);

    ASSERT_EQ(swmm_userflag_value_set(engine, "LINK", "C_MAIN",
                                      "ROUGHNESS_ADJ", "1e-6"), SWMM_OK);
    EXPECT_EQ(getValue("LINK", "C_MAIN", "ROUGHNESS_ADJ", &found), "1e-06");

    EXPECT_EQ(swmm_userflag_value_set(engine, "LINK", "C_MAIN",
                                      "ROUGHNESS_ADJ", "abc"),
              SWMM_ERR_BADPARAM);
}

TEST_F(UserFlagsCapiTest, StringValueRoundTrip) {
    ASSERT_EQ(swmm_userflag_define(engine, "ASSET_ID", kString, ""), SWMM_OK);

    int found = -1;
    ASSERT_EQ(swmm_userflag_value_set(engine, "SUBCATCHMENT", "S_WEST",
                                      "ASSET_ID", "AM 00341"), SWMM_OK);
    // Stored verbatim — no quoting at the API boundary.
    EXPECT_EQ(getValue("SUBCATCHMENT", "S_WEST", "ASSET_ID", &found),
              "AM 00341");
    EXPECT_EQ(found, 1);
}

// ---------------------------------------------------------------------------
// Unset semantics, clear, and validation
// ---------------------------------------------------------------------------

TEST_F(UserFlagsCapiTest, UnsetAndClear) {
    ASSERT_EQ(swmm_userflag_define(engine, "INSPECTED", kBool, ""), SWMM_OK);

    int found = -1;
    EXPECT_EQ(getValue("NODE", "J1", "INSPECTED", &found), "");
    EXPECT_EQ(found, 0);

    ASSERT_EQ(swmm_userflag_value_set(engine, "NODE", "J1", "INSPECTED", "YES"),
              SWMM_OK);
    getValue("NODE", "J1", "INSPECTED", &found);
    EXPECT_EQ(found, 1);

    EXPECT_EQ(swmm_userflag_value_clear(engine, "NODE", "J1", "INSPECTED"),
              SWMM_OK);
    getValue("NODE", "J1", "INSPECTED", &found);
    EXPECT_EQ(found, 0);

    // Clearing an unassigned value is idempotent.
    EXPECT_EQ(swmm_userflag_value_clear(engine, "NODE", "J1", "INSPECTED"),
              SWMM_OK);
}

TEST_F(UserFlagsCapiTest, SetRequiresDefinedFlag) {
    EXPECT_EQ(swmm_userflag_value_set(engine, "NODE", "J1", "UNDEFINED", "1"),
              SWMM_ERR_BADPARAM);
}

TEST_F(UserFlagsCapiTest, ModelLevelFlagsAreCaseInsensitive) {
    // Pre-existing MODEL-level accessors now uppercase flag names, matching
    // the [USER_FLAGS] INP handler.
    ASSERT_EQ(swmm_userflag_set_bool(engine, "my_flag", 1), SWMM_OK);
    int bv = -1;
    EXPECT_EQ(swmm_userflag_get_bool(engine, "MY_FLAG", &bv), SWMM_OK);
    EXPECT_EQ(bv, 1);
    EXPECT_EQ(swmm_userflag_get_bool(engine, "My_Flag", &bv), SWMM_OK);
    EXPECT_EQ(bv, 1);

    ASSERT_EQ(swmm_userflag_set_int(engine, "Max_Paths", 4), SWMM_OK);
    int iv = -1;
    EXPECT_EQ(swmm_userflag_get_int(engine, "max_paths", &iv), SWMM_OK);
    EXPECT_EQ(iv, 4);

    ASSERT_EQ(swmm_userflag_set_real(engine, "Tolerance", 1e-6), SWMM_OK);
    double dv = 0.0;
    EXPECT_EQ(swmm_userflag_get_real(engine, "TOLERANCE", &dv), SWMM_OK);
    EXPECT_DOUBLE_EQ(dv, 1e-6);

    // Mixed-case set registers a single uppercase definition (no duplicates).
    ASSERT_EQ(swmm_userflag_set_bool(engine, "MY_FLAG", 0), SWMM_OK);
    int count = -1;
    ASSERT_EQ(swmm_userflag_def_count(engine, &count), SWMM_OK);
    EXPECT_EQ(count, 3);
}

TEST_F(UserFlagsCapiTest, CaseHandling) {
    ASSERT_EQ(swmm_userflag_define(engine, "INSPECTED", kBool, ""), SWMM_OK);
    // Object type and flag name are case-insensitive.
    ASSERT_EQ(swmm_userflag_value_set(engine, "node", "J1", "inspected", "YES"),
              SWMM_OK);
    int found = -1;
    EXPECT_EQ(getValue("NODE", "J1", "INSPECTED", &found), "YES");
    EXPECT_EQ(found, 1);
    // Object name is case-preserved (case-sensitive).
    getValue("NODE", "j1", "INSPECTED", &found);
    EXPECT_EQ(found, 0);
}

} // namespace
