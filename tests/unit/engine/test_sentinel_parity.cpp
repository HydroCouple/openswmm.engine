/*
 * test_sentinel_parity.cpp
 *
 * Input sentinels and defaults that legacy EPA SWMM gives a special meaning
 * (`*` placeholders, zero-means-default values, read-time clamps) and the
 * name/number checks legacy applies to the same rows. Every expectation below
 * was confirmed by running the fixture through the legacy engine.
 *
 *   [GROUNDWATER]  Egwt `*` (first character only) = receiving node invert;
 *                  -99 is a LITERAL elevation, not a sentinel; undefined
 *                  subcatchment/aquifer/node = ERROR 209; non-numeric = 211;
 *                  rows may precede the sections they reference; [GWF]
 *                  naming an undefined subcatchment = ERROR 209.
 *   [INFILTRATION] Horton DryTime 0 = TINY (infil.c:349), not "no recovery".
 *   [LID_USAGE]    RptFile `*` = no report file; Number 0 = unit not added.
 *   [DIVIDERS]     DivLink `*` = ERROR 136; undefined link = ERROR 209.
 *   [OUTLETS]      negative crest under LINK_OFFSETS DEPTH clamps to 0.
 *   [TEMPERATURE]  FILE start date (`*` = none) shifts the file days read;
 *                  a day missing one of Tmin/Tmax keeps that one's last value.
 *   [RAINGAGES]    FILE start date skips records dated earlier.
 *
 * Fixtures live in tests/unit/engine/data/ (the test's WORKING_DIRECTORY).
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <openswmm/engine/openswmm_climate.h>
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_forcing.h>
#include <openswmm/engine/openswmm_gages.h>
#include <openswmm/engine/openswmm_infrastructure.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/engine/openswmm_subcatchments.h>

namespace {

constexpr double kMissing = -1.0e10;

std::string read_file(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// Open a deck; on success run it to completion and write the report. Returns
// the open() code and the report text.
int run_model(const std::string& stem, std::string& rpt_text) {
    const std::string inp = stem + ".inp", rpt = stem + ".rpt", out = stem + ".out";
    SWMM_Engine e = swmm_engine_create();
    const int rc = swmm_engine_open(e, inp.c_str(), rpt.c_str(), out.c_str(), nullptr);
    if (rc == SWMM_OK && swmm_engine_initialize(e) == SWMM_OK &&
        swmm_engine_start(e, 1) == SWMM_OK) {
        double elapsed = 0.0;
        while (swmm_engine_step(e, &elapsed) == SWMM_OK && elapsed > 0.0) {
        }
        swmm_engine_end(e);
        swmm_engine_report(e);
    }
    swmm_engine_close(e);
    rpt_text = read_file(rpt);
    return rc;
}

// Open only (no run) for property assertions. Caller closes.
SWMM_Engine open_only(const std::string& stem) {
    SWMM_Engine e = swmm_engine_create();
    const std::string inp = stem + ".inp", rpt = stem + ".rpt", out = stem + ".out";
    EXPECT_EQ(swmm_engine_open(e, inp.c_str(), rpt.c_str(), out.c_str(), nullptr), SWMM_OK)
        << read_file(rpt);
    return e;
}

double egwt_of(SWMM_Engine e, const char* subcatch) {
    const int s = swmm_subcatch_index(e, subcatch);
    EXPECT_GE(s, 0);
    double v[8] = {};
    EXPECT_EQ(swmm_subcatch_get_gw_params(e, s, &v[0], &v[1], &v[2], &v[3],
                                          &v[4], &v[5], &v[6], &v[7]), SWMM_OK);
    return v[7];
}

double runoff_volume(const std::string& stem) {
    SWMM_Engine e = swmm_engine_create();
    const std::string inp = stem + ".inp", rpt = stem + ".rpt", out = stem + ".out";
    double vol = -1.0;
    if (swmm_engine_open(e, inp.c_str(), rpt.c_str(), out.c_str(), nullptr) == SWMM_OK &&
        swmm_engine_initialize(e) == SWMM_OK && swmm_engine_start(e, 0) == SWMM_OK) {
        double elapsed = 0.0;
        while (swmm_engine_step(e, &elapsed) == SWMM_OK && elapsed > 0.0) {
        }
        swmm_subcatch_get_stat_runoff_vol(e, swmm_subcatch_index(e, "S1"), &vol);
        swmm_engine_end(e);
    }
    swmm_engine_close(e);
    return vol;
}

// Run to the first step at or past `days` elapsed and return the air
// temperature there (deg F).
double temperature_at(const std::string& stem, double days) {
    SWMM_Engine e = swmm_engine_create();
    const std::string inp = stem + ".inp", rpt = stem + ".rpt", out = stem + ".out";
    double temp = -1.0e30;
    if (swmm_engine_open(e, inp.c_str(), rpt.c_str(), out.c_str(), nullptr) == SWMM_OK &&
        swmm_engine_initialize(e) == SWMM_OK && swmm_engine_start(e, 0) == SWMM_OK) {
        double elapsed = 0.0;
        while (swmm_engine_step(e, &elapsed) == SWMM_OK && elapsed > 0.0 && elapsed < days) {
        }
        swmm_climate_get_temperature(e, &temp);
        swmm_engine_end(e);
    }
    swmm_engine_close(e);
    return temp;
}

int rain_series_count(const std::string& stem) {
    SWMM_Engine e = open_only(stem);
    int n = -1;
    EXPECT_EQ(swmm_gage_get_rainfall_series_count(e, swmm_gage_index(e, "G1"), &n), SWMM_OK);
    swmm_engine_close(e);
    return n;
}

// Re-write the opened deck and return the text of the written file.
std::string rewritten(const std::string& stem) {
    SWMM_Engine e = open_only(stem);
    const std::string path = stem + "_rewritten.inp";
    EXPECT_EQ(swmm_model_write(e, path.c_str()), SWMM_OK);
    swmm_engine_close(e);
    return read_file(path);
}

}  // namespace

// ---------------------------------------------------------------------------
// [GROUNDWATER]
// ---------------------------------------------------------------------------

TEST(SentinelParityGroundwater, EgwtStarIsMissing) {
    SWMM_Engine e = open_only("sentinel_gw_egwt_star");
    EXPECT_EQ(egwt_of(e, "S1"), kMissing);
    swmm_engine_close(e);
}

// legacy gwater.c:240 tests only the first character: `*tok[m] != '*'`.
TEST(SentinelParityGroundwater, EgwtStarPrefixIsMissing) {
    SWMM_Engine e = open_only("sentinel_gw_egwt_star_prefix");
    EXPECT_EQ(egwt_of(e, "S1"), kMissing);
    swmm_engine_close(e);
}

// -99 is not a sentinel in either engine: it is an elevation of -99.
TEST(SentinelParityGroundwater, EgwtMinus99IsLiteralElevation) {
    SWMM_Engine e = open_only("sentinel_gw_egwt_minus99");
    EXPECT_EQ(egwt_of(e, "S1"), -99.0);
    swmm_engine_close(e);
}

// [GROUNDWATER] ahead of [SUBCATCHMENTS], [AQUIFERS] and [JUNCTIONS]: legacy
// counts every object before reading any row, so the references resolve.
TEST(SentinelParityGroundwater, RowBeforeReferencedSectionsResolves) {
    SWMM_Engine e = open_only("sentinel_gw_forward_refs");
    const int s = swmm_subcatch_index(e, "S1");
    ASSERT_GE(s, 0);
    int aq = -1, nd = -1;
    EXPECT_EQ(swmm_subcatch_get_aquifer(e, s, &aq), SWMM_OK);
    EXPECT_EQ(swmm_subcatch_get_gw_node(e, s, &nd), SWMM_OK);
    EXPECT_GE(aq, 0);
    EXPECT_EQ(nd, swmm_node_index(e, "J1"));
    EXPECT_EQ(egwt_of(e, "S1"), kMissing);
    char expr[64] = {};
    EXPECT_EQ(swmm_subcatch_get_gwf_expression(e, s, SWMM_GWF_LATERAL, expr, sizeof(expr)), SWMM_OK);
    EXPECT_STREQ(expr, "0.001*HGW");
    EXPECT_DOUBLE_EQ([&] {
        double v[8] = {};
        swmm_subcatch_get_gw_params(e, s, &v[0], &v[1], &v[2], &v[3], &v[4],
                                    &v[5], &v[6], &v[7]);
        return v[1];
    }(), 0.1);
    swmm_engine_close(e);
}

TEST(SentinelParityGroundwater, UndefinedAquiferIs209) {
    std::string rpt;
    EXPECT_NE(run_model("sentinel_gw_unknown_aquifer", rpt), SWMM_OK);
    EXPECT_TRUE(contains(rpt, "ERROR 209")) << rpt;
    EXPECT_TRUE(contains(rpt, "NOAQ")) << rpt;
}

TEST(SentinelParityGroundwater, UndefinedNodeIs209) {
    std::string rpt;
    EXPECT_NE(run_model("sentinel_gw_unknown_node", rpt), SWMM_OK);
    EXPECT_TRUE(contains(rpt, "ERROR 209")) << rpt;
    EXPECT_TRUE(contains(rpt, "NONODE")) << rpt;
}

TEST(SentinelParityGroundwater, UndefinedSubcatchmentIs209) {
    std::string rpt;
    EXPECT_NE(run_model("sentinel_gw_unknown_subcatch", rpt), SWMM_OK);
    EXPECT_TRUE(contains(rpt, "ERROR 209")) << rpt;
    EXPECT_TRUE(contains(rpt, "NOSUB")) << rpt;
}

// legacy gwater_readFlowExpression (gwater.c:299).
TEST(SentinelParityGroundwater, GwfUndefinedSubcatchmentIs209) {
    std::string rpt;
    EXPECT_NE(run_model("sentinel_gw_gwf_unknown_subcatch", rpt), SWMM_OK);
    EXPECT_TRUE(contains(rpt, "ERROR 209")) << rpt;
    EXPECT_TRUE(contains(rpt, "NOSUB")) << rpt;
}

TEST(SentinelParityGroundwater, NonNumericFieldIs211) {
    std::string rpt;
    EXPECT_NE(run_model("sentinel_gw_bad_number", rpt), SWMM_OK);
    EXPECT_TRUE(contains(rpt, "ERROR 211")) << rpt;
    EXPECT_TRUE(contains(rpt, "abc")) << rpt;
}

// ---------------------------------------------------------------------------
// [INFILTRATION] Horton DryTime = 0
// ---------------------------------------------------------------------------

// legacy infil.c:349 replaces a zero drying time with TINY (1e-6 days), so the
// capacity recovers almost at once; it does not switch recovery off.
TEST(SentinelParityInfiltration, HortonDryTimeZeroIsTiny) {
    const double v0    = runoff_volume("sentinel_horton_drytime0");
    const double vtiny = runoff_volume("sentinel_horton_drytime_tiny");
    const double v7    = runoff_volume("sentinel_horton_drytime7");
    ASSERT_GT(v7, 0.0);
    EXPECT_EQ(v0, vtiny);
    EXPECT_NE(v0, v7);  // the deck is sensitive to recovery at all
}

// ---------------------------------------------------------------------------
// [LID_USAGE]
// ---------------------------------------------------------------------------

TEST(SentinelParityLid, ReportFileStarWritesNoFile) {
    std::filesystem::remove("*");
    std::string rpt;
    ASSERT_EQ(run_model("sentinel_lid_rptfile_star", rpt), SWMM_OK) << rpt;
    EXPECT_FALSE(std::filesystem::exists("*"));
    std::filesystem::remove("*");
}

TEST(SentinelParityLid, NumberZeroAddsNoUnit) {
    SWMM_Engine e = open_only("sentinel_lid_number0");
    EXPECT_EQ(swmm_lid_usage_count(e), 0);
    swmm_engine_close(e);
}

// ---------------------------------------------------------------------------
// [DIVIDERS]
// ---------------------------------------------------------------------------

TEST(SentinelParityDivider, StarDiversionLinkIs136) {
    std::string rpt;
    EXPECT_NE(run_model("sentinel_divider_link_star", rpt), SWMM_OK);
    EXPECT_TRUE(contains(rpt, "ERROR 136")) << rpt;
    EXPECT_TRUE(contains(rpt, "D1")) << rpt;
}

TEST(SentinelParityDivider, UndefinedDiversionLinkIs209) {
    std::string rpt;
    EXPECT_NE(run_model("sentinel_divider_link_unknown", rpt), SWMM_OK);
    EXPECT_TRUE(contains(rpt, "ERROR 209")) << rpt;
    EXPECT_TRUE(contains(rpt, "C9")) << rpt;
}

// ---------------------------------------------------------------------------
// [OUTLETS]
// ---------------------------------------------------------------------------

// legacy link.c:2594: under DEPTH offsets a negative outlet crest reads as 0.
TEST(SentinelParityOutlet, NegativeCrestClampedUnderDepthOffsets) {
    SWMM_Engine e = open_only("sentinel_outlet_neg_crest");
    const int l = swmm_link_index(e, "OL1");
    ASSERT_GE(l, 0);
    double crest = -1.0;
    EXPECT_EQ(swmm_link_get_crest_height(e, l, &crest), SWMM_OK);
    EXPECT_EQ(crest, 0.0);
    swmm_engine_close(e);
}

// ---------------------------------------------------------------------------
// [TEMPERATURE] FILE
// ---------------------------------------------------------------------------

// The start date is a date (12/01/2019 = OADate 43800), not the 1.0 that
// to_double("12/01/2019") produced.
TEST(SentinelParityClimate, FileStartDateIsParsedAsADate) {
    SWMM_Engine e = open_only("sentinel_climate_startdate");
    double start = 0.0;
    EXPECT_EQ(swmm_climate_get_temp_file_start(e, &start), SWMM_OK);
    EXPECT_EQ(start, 43800.0);
    swmm_engine_close(e);
}

// The file holds 20 F every December day and 80 F every January day. A
// January run told to start the file at 12/01/2019 reads December (legacy
// climate_openFile / updateFileValues); without a date it reads January.
TEST(SentinelParityClimate, FileStartDateShiftsTheDaysRead) {
    EXPECT_EQ(temperature_at("sentinel_climate_startdate", 1.5), 20.0);
    EXPECT_EQ(temperature_at("sentinel_climate_nostart", 1.5), 80.0);
}

// The deck's title also carries the date, so match the FILE line itself.
TEST(SentinelParityClimate, FileStartDateRoundTrips) {
    const std::string text = rewritten("sentinel_climate_startdate");
    EXPECT_TRUE(contains(text, "\"sentinel_climate.dat\" 12/01/2019")) << text;
}

// Jan 1 is 40/40; Jan 2 gives Tmax 60 with Tmin missing. Legacy keeps Tmin at
// 40 and uses the new Tmax, so the Jan 2 afternoon is warmer than 40. Requiring
// both values left the temperature frozen at Jan 1's 40.
TEST(SentinelParityClimate, MissingTminKeepsPreviousTminButUsesNewTmax) {
    const double t = temperature_at("sentinel_climate_missing_tmin", 1.625);  // Jan 2 15:00
    EXPECT_GT(t, 40.5);
    EXPECT_LE(t, 60.0);
}

// ---------------------------------------------------------------------------
// [RAINGAGES] FILE start date
// ---------------------------------------------------------------------------

// sentinel_rain.dat holds three records on Jan 1 and three on Jan 2; a start
// date of 01/02/2020 drops Jan 1's (legacy rain.c readStdLine).
TEST(SentinelParityRainFile, StartDateSkipsEarlierRecords) {
    EXPECT_EQ(rain_series_count("sentinel_raingage_file_nostart"), 6);
    EXPECT_EQ(rain_series_count("sentinel_raingage_file_startdate"), 3);
}

TEST(SentinelParityRainFile, StartDateRoundTrips) {
    const std::string text = rewritten("sentinel_raingage_file_startdate");
    EXPECT_TRUE(contains(text, "STA1 IN 01/02/2020")) << text;
}
