/**
 * @file test_name_case_parity.cpp
 * @brief Legacy-parity tests for case-insensitive object-name matching and
 *        duplicate-ID rejection (ERR 207).
 *
 * @details Legacy EPA SWMM matches object names case-insensitively
 *          (src/legacy/engine/hash.c UCHAR) and rejects a second definition
 *          of the same name — in any case spelling — with ERROR 207
 *          (input.c addObject), exempting multi-line object classes whose
 *          lines legitimately repeat the name. These tests pin the
 *          refactored engine to that contract.
 *
 * Fixtures live in tests/unit/engine/data/ (WORKING_DIRECTORY of the test)
 * so they are reviewable and committed, not written to a temp dir.
 *
 * @see src/engine/data/NameIndex.hpp, src/engine/input/InputParseUtils.hpp
 * @ingroup engine_input
 */

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_infrastructure.h>
#include <openswmm/engine/openswmm_subcatchments.h>
#include <openswmm/engine/openswmm_tables.h>

namespace {

// Open a model; on success run the full lifecycle so controls compile at
// initialize() and actions land in the .rpt. Captures the .rpt text and the
// initialize() return code.
int open_model(const std::string& inp, const std::string& rpt,
               std::string& rpt_text, int* init_rc = nullptr,
               std::string* init_msg = nullptr) {
    SWMM_Engine e = swmm_engine_create();
    const int rc = swmm_engine_open(e, inp.c_str(), rpt.c_str(), nullptr, nullptr);
    if (rc == SWMM_OK) {
        const int irc = swmm_engine_initialize(e);
        if (init_rc) *init_rc = irc;
        if (init_msg) *init_msg = swmm_get_last_error_msg(e);
        if (irc == SWMM_OK && swmm_engine_start(e, 0) == SWMM_OK) {
            double elapsed = 0.0;
            while (swmm_engine_step(e, &elapsed) == SWMM_OK && elapsed > 0.0) {
            }
            swmm_engine_end(e);
            swmm_engine_report(e);
        }
    }
    swmm_engine_close(e);
    swmm_engine_destroy(e);

    std::ifstream f(rpt);
    std::stringstream ss;
    ss << f.rdbuf();
    rpt_text = ss.str();
    return rc;
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// A deck whose conduit endpoint, orifice endpoint, inflow node and control
// rules reference "ChestnutHillRes_IM" in four case spellings must parse,
// compile its rules and run — legacy accepts it (case-blind hash table).
TEST(NameCaseParity, CaseVariantReferencesResolveAndRun) {
    std::string rpt_text;
    int init_rc = -1;
    const int rc = open_model("namecase_ci_refs.inp", "namecase_ci_refs.rpt",
                              rpt_text, &init_rc);
    EXPECT_EQ(rc, SWMM_OK);
    EXPECT_EQ(init_rc, SWMM_OK);  // [CONTROLS] compiled with variant-case IDs
    EXPECT_TRUE(contains(rpt_text, "Control Actions Taken")) << rpt_text;
}

// [JUNCTIONS] J1 followed by [STORAGE] j1: one node registry, so the second
// definition is a duplicate ID in legacy's case-blind semantics — ERROR 207,
// naming both spellings.
TEST(NameCaseParity, CaseVariantDuplicateAcrossSectionsIs207) {
    std::string rpt_text;
    const int rc = open_model("namecase_dup_cross_section.inp",
                              "namecase_dup_cross_section.rpt", rpt_text);
    EXPECT_EQ(rc, SWMM_ERR_PARSE);
    EXPECT_TRUE(contains(rpt_text, "ERROR 207")) << rpt_text;
    EXPECT_TRUE(contains(rpt_text, "matches existing ID 'J1'")) << rpt_text;
}

// The same junction defined twice in one section is ERROR 207 (legacy
// input.c addObject), not a silent merge.
TEST(NameCaseParity, DuplicateDefinitionWithinSectionIs207) {
    std::string rpt_text;
    const int rc = open_model("namecase_dup_same_section.inp",
                              "namecase_dup_same_section.rpt", rpt_text);
    EXPECT_EQ(rc, SWMM_ERR_PARSE);
    EXPECT_TRUE(contains(rpt_text, "ERROR 207")) << rpt_text;
    EXPECT_TRUE(contains(rpt_text, "duplicate ID name J1")) << rpt_text;
}

// Multi-line object classes are exempt: [TIMESERIES] continuation lines with
// case-variant spellings of the same name merge into ONE series and the deck
// runs (legacy find-first behaviour).
TEST(NameCaseParity, MultiLineContinuationSpellingsMergeWithout207) {
    std::string rpt_text;
    int init_rc = -1;
    SWMM_Engine e = swmm_engine_create();
    const int rc = swmm_engine_open(e, "namecase_multiline_exempt.inp",
                                    "namecase_multiline_exempt.rpt",
                                    nullptr, nullptr);
    EXPECT_EQ(rc, SWMM_OK);
    // StormTS's three case-variant lines must have merged into one table.
    EXPECT_EQ(swmm_table_count(e), 1);
    // DWFpat's case-variant [PATTERNS] lines must have merged into one
    // pattern, and pattern lookup itself is case-insensitive.
    EXPECT_EQ(swmm_pattern_count(e), 1);
    EXPECT_EQ(swmm_pattern_index(e, "dwfPAT"), 0);
    init_rc = swmm_engine_initialize(e);
    EXPECT_EQ(init_rc, SWMM_OK);
    swmm_engine_close(e);
    swmm_engine_destroy(e);
    (void)rpt_text;
}

// A pump curve and an inflow timeseries may share one name — legacy keeps
// CURVE and TSERIES in separate hash tables. Both objects must exist and
// each case-variant reference must resolve to its own kind.
TEST(NameCaseParity, CurveAndTimeseriesMayShareAName) {
    SWMM_Engine e = swmm_engine_create();
    const int rc = swmm_engine_open(e, "namecase_shared_curve_ts.inp",
                                    "namecase_shared_curve_ts.rpt",
                                    nullptr, nullptr);
    EXPECT_EQ(rc, SWMM_OK);
    EXPECT_EQ(swmm_table_count(e), 2);  // one curve + one timeseries
    EXPECT_EQ(swmm_engine_initialize(e), SWMM_OK);
    EXPECT_EQ(swmm_engine_start(e, 0), SWMM_OK);
    double elapsed = 0.0;
    while (swmm_engine_step(e, &elapsed) == SWMM_OK && elapsed > 0.0) {
    }
    EXPECT_EQ(swmm_engine_end(e), SWMM_OK);
    swmm_engine_close(e);
    swmm_engine_destroy(e);
}

// Linear-scan-backed C API lookups (transects here as the representative)
// are case-insensitive too, and their adders reject case-variant duplicates.
TEST(NameCaseParity, TransectLookupIsCaseInsensitive) {
    SWMM_Engine e = swmm_engine_new();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_transect_add(e, "TR1"), SWMM_OK);
    EXPECT_EQ(swmm_transect_index(e, "tr1"), 0);
    EXPECT_EQ(swmm_transect_add(e, "tr1"), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_transect_count(e), 1);
    swmm_engine_destroy(e);
}

// The C API adders that used to call NameIndex::add unguarded must reject a
// case-variant duplicate with SWMM_ERR_BADPARAM, leaving the backing store
// untouched (no throw across the C ABI, no half-added object).
TEST(NameCaseParity, CApiAddersRejectCaseVariantDuplicates) {
    SWMM_Engine e = swmm_engine_new();
    ASSERT_NE(e, nullptr);

    ASSERT_EQ(swmm_lid_add(e, "BC1", 0), SWMM_OK);
    EXPECT_EQ(swmm_lid_add(e, "bc1", 0), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_lid_count(e), 1);

    ASSERT_EQ(swmm_aquifer_add(e, "AQ1"), SWMM_OK);
    EXPECT_EQ(swmm_aquifer_add(e, "aq1"), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_aquifer_count(e), 1);

    ASSERT_EQ(swmm_snowpack_add(e, "SP1"), SWMM_OK);
    EXPECT_EQ(swmm_snowpack_add(e, "sp1"), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_snowpack_count(e), 1);

    swmm_engine_destroy(e);
}

// A genuinely unknown node in a [CONTROLS] premise must be reported by the
// failing ID token, not by quoting the leading NODE keyword.
TEST(NameCaseParity, ControlsParseErrorNamesTheFailingToken) {
    std::string rpt_text;
    int init_rc = -1;
    std::string init_msg;
    const int rc = open_model("namecase_controls_badref.inp",
                              "namecase_controls_badref.rpt", rpt_text,
                              &init_rc, &init_msg);
    EXPECT_EQ(rc, SWMM_OK);           // deck itself is valid
    EXPECT_NE(init_rc, SWMM_OK);      // rule compilation fails
    EXPECT_TRUE(contains(init_msg,
        "'NoSuchNode' is not the name of a defined NODE")) << init_msg;
}
