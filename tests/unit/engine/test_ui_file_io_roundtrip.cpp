// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_gages.h>
#include <openswmm/engine/openswmm_pollutants.h>
#include <openswmm/engine/openswmm_subcatchments.h>
#include <openswmm/engine/openswmm_snow.h>
#include <openswmm/engine/openswmm_infrastructure.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/engine/openswmm_tables.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {
struct Engine {
    SWMM_Engine value = swmm_engine_create();
    ~Engine() { swmm_engine_close(value); swmm_engine_destroy(value); }
    operator SWMM_Engine() const { return value; }
};
std::string option(SWMM_Engine e, const char* key, bool ext = false) {
    char value[65536]{};
    const int rc = ext ? swmm_options_get_ext(e, key, value, sizeof value)
                       : swmm_options_get(e, key, value, sizeof value);
    EXPECT_EQ(rc, 0) << key;
    return value;
}
struct Format { std::string extension, plugin; };
std::vector<Format> formats() {
    std::vector<Format> result{{"inp", ""}};
#ifdef OPENSWMM_HAS_GEOPACKAGE
    result.push_back({"gpkg", "org.hydrocouple.openswmm.plugins.geopackage"});
#endif
#ifdef OPENSWMM_HAS_HDF5_MODEL
    result.push_back({"h5", "org.hydrocouple.openswmm.plugins.hdf5"});
#endif
    return result;
}
const std::map<std::string, std::string> values{
    {"VARIABLE_STEP", "0.755"}, {"ROUTING_STEP", "0.0005"}, {"RULE_STEP", "37"},
    {"FLOW_ROUTING", "FV"}, {"FV_CELL_LENGTH", "17.25"}, {"FV_MIN_CELLS", "7"},
    {"FV_CFL", "0.63"}, {"FV_ORDER", "2"}, {"FV_LIMITER", "SUPERBEE"},
    {"FV_TIME_INTEGRATION", "RK2"}, {"FV_SLOT_CELERITY", "122.5"},
    {"FV_BACKEND", "CPU"}, {"FV_MIN_PARALLEL_CELLS", "1234"}, {"FV_LTS", "NO"},
    {"FV_LTS_MAX_TIERS", "3"}, {"FV_CFL_CENSUS_INTERVAL", "4"},
    {"FV_STRUCTURE_COUPLING", "ROUTING_STEP"}, {"FV_PRESSURIZED_IMPLICIT", "YES"},
    {"QUALITY_SOLVER", "LAGRANGIAN"}, {"QUALITY_STEP", "0.25"},
    {"MAX_SEGMENTS_PER_LINK", "73"}, {"DISPERSION", "RWPT"}, {"RWPT_SEED", "123"},
    {"WATER_AGE", "ON"}, {"HEAT_TRANSPORT", "ON"}, {"OUTFALL_BACKFLOW_QUALITY", "ZERO"},
    {"SKIP_STEADY_STATE", "YES"}, {"WATER_DENSITY", "997"},
    {"WATER_SPECIFIC_HEAT_CAPACITY", "4200"}, {"WIND_FUNC_COEFF_A", "11"},
    {"WIND_FUNC_COEFF_B", "13"}, {"PRESSURE_RATIO", "0.9"},
    {"REPORT_START_TIME", "00:00:30"}, {"SURCHARGE_METHOD", "EXTRAN"}, {"DPS_CELERITY", "42"}, {"DPS_ALPHA", "0.2"},
    {"DPS_DECAY_TIME", "17"}, {"TPA_CELERITY", "777"}
};

TEST(UiFileIO, ConfiguredFieldsSurviveEveryModelFormatAndRepeatedSave) {
    const auto root = std::filesystem::absolute("ui_file_io_roundtrip");
    std::filesystem::create_directories(root);
    std::ifstream fixture("precip_scaling/snow_scaled.inp");
    ASSERT_TRUE(fixture.good());
    const std::string base{std::istreambuf_iterator<char>(fixture), {}};
    const auto source = root / "source.inp";
    std::ofstream(source) << base << R"(
[TITLE]
Retained native model notes
[TIMESERIES]
FRAC 0:00:00.0005 0.000012345
FRAC 0:00:00.1234567 9.87654321012345
[POLLUTANTS]
P1 MG/L 0.000012345 2 3 0.04 NO P2 0.5 5 6
P2 MG/L 0 0 0 0 NO * 0 0 0
[AQUIFERS]
AQ1 0.5 0.15 0.3 5 10 15 0.35 0 0.002 0 10 0.3
[GROUNDWATER]
S1 AQ1 J1 12 0.001 1 0 0 0 0 0
[GWF]
S1 LATERAL 0.001*(HGW-10)
[WEIRS]
W1 J1 O1 TRANSVERSE 1.2 3.33 NO 1 0 NO
[OUTLETS]
D1 J1 O1 1.2 FUNCTIONAL/HEAD 2 0.5 NO
[XSECTIONS]
W1 RECT_OPEN 1 2 0 0
[TRANSECTS]
;Retained transect description
NC .03 .04 .05
X1 T1 4 2 8 1.25 8.75 1 1 0
GR 3 0 0 2 0 8 3 10
[PATTERNS]
PAT MONTHLY 2 2 2 2 2 2 2 2 2 2 2 2
[ADJUSTMENTS]
N-PERV S1 PAT
)";
    for (const auto& format : formats()) {
        SCOPED_TRACE(format.extension);
        Engine before;
        ASSERT_EQ(swmm_engine_open(before, source.string().c_str(), "", "", nullptr), 0);
        for (const auto& [key, value] : values)
            ASSERT_EQ(swmm_options_set(before, key.c_str(), value.c_str()), 0) << key;
        ASSERT_EQ(swmm_gage_set_scale_factor(before, 0, 10), 0);
        ASSERT_EQ(swmm_pollutant_set_mwt(before, 0, 58.44), 0);
        ASSERT_EQ(swmm_snow_set_step(before, 73), 0);
        ASSERT_EQ(swmm_link_set_crest_height(before, 1, 0.6), 0);
        ASSERT_EQ(swmm_link_set_crest_height(before, 2, 0.9), 0);
        ASSERT_EQ(swmm_link_set_offset_up(before, 1, 0.7), 0);
        EXPECT_NE(swmm_link_set_offset_dn(before, 1, 0.8), 0);
        std::map<std::string, std::string> expected;
        for (const auto& [key, _] : values) expected[key] = option(before, key.c_str());
        const auto target = root / ("saved." + format.extension);
        const char* plugin = format.plugin.empty() ? nullptr : format.plugin.c_str();
        std::filesystem::remove(target);
        for (int save = 0; save < 2; ++save) {
            if (save) {
                ASSERT_EQ(swmm_options_set(before, "VARIABLE_STEP", "0.625"), 0);
                expected["VARIABLE_STEP"] = option(before, "VARIABLE_STEP");
            }
            ASSERT_EQ(swmm_model_write_with_plugin(before, target.string().c_str(), plugin), 0);
            Engine after;
            ASSERT_EQ(swmm_engine_open(after, target.string().c_str(), "", "", plugin), 0);
            for (const auto& [key, value] : expected) EXPECT_EQ(option(after, key.c_str()), value) << key;
            double value = 0, fraction = 0;
            ASSERT_EQ(swmm_gage_get_scale_factor(after, 0, &value), 0); EXPECT_DOUBLE_EQ(value, 10);
            ASSERT_EQ(swmm_pollutant_get_mwt(after, 0, &value), 0); EXPECT_DOUBLE_EQ(value, 58.44);
            ASSERT_EQ(swmm_pollutant_get_rain_conc(after, 0, &value), 0); EXPECT_DOUBLE_EQ(value, .000012345);
            ASSERT_EQ(swmm_snow_get_step(after, &value), 0); EXPECT_DOUBLE_EQ(value, 73);
            EXPECT_EQ(swmm_aquifer_count(after), 1);
            for (int parameter = 0; parameter < 12; ++parameter) {
                double original = 0;
                ASSERT_EQ(swmm_aquifer_get_param(before, 0, parameter, &original), 0);
                ASSERT_EQ(swmm_aquifer_get_param(after, 0, parameter, &value), 0);
                EXPECT_NEAR(value, original, std::max(1e-15, std::abs(original)*1e-14));
            }
            const int originalTable = swmm_table_index(before, "FRAC");
            const int restoredTable = swmm_table_index(after, "FRAC");
            ASSERT_GE(originalTable, 0); ASSERT_GE(restoredTable, 0);
            for (int point = 0; point < 2; ++point) {
                double x, y, restoredX, restoredY;
                ASSERT_EQ(swmm_table_get_point(before, originalTable, point, &x, &y), 0);
                ASSERT_EQ(swmm_table_get_point(after, restoredTable, point, &restoredX, &restoredY), 0);
                EXPECT_DOUBLE_EQ(restoredX, x); EXPECT_DOUBLE_EQ(restoredY, y);
            }
            EXPECT_STREQ(swmm_subcatch_get_snowpack(after, 0), "SP1");
            int co = -2;
            ASSERT_EQ(swmm_pollutant_get_co_pollutant(after, 0, &co, &fraction), 0);
            EXPECT_EQ(co, 1); EXPECT_DOUBLE_EQ(fraction, .5);
            ASSERT_EQ(swmm_pollutant_get_co_pollutant(after, 1, &co, nullptr), 0); EXPECT_EQ(co, -1);
            ASSERT_EQ(swmm_link_get_offset_up(after, 1, &value), 0); EXPECT_DOUBLE_EQ(value, .7);
            ASSERT_EQ(swmm_link_get_offset_up(after, 2, &value), 0); EXPECT_DOUBLE_EQ(value, .9);
            ASSERT_EQ(swmm_link_get_crest_height(after, 2, &value), 0); EXPECT_DOUBLE_EQ(value, .9);
            double right;
            ASSERT_EQ(swmm_transect_get_encroachment_stations(after, 0, &value, &right), 0);
            EXPECT_DOUBLE_EQ(value, 1.25); EXPECT_DOUBLE_EQ(right, 8.75);
            char comment[1024]{};
            ASSERT_EQ(swmm_transect_get_comments(after, 0, comment, sizeof comment), 0);
            EXPECT_STREQ(comment, "Retained transect description");
        }
    }
}

TEST(UiFileIO, Draft2DOptionsPersistWithoutMesh) {
#ifdef OPENSWMM_HAS_2D
    const auto root = std::filesystem::absolute("ui_file_io_roundtrip");
    std::filesystem::create_directories(root);
    for (const auto& format : formats()) {
        SCOPED_TRACE(format.extension);
        Engine before;
        ASSERT_EQ(swmm_engine_open(before, "precip_scaling/snow_scaled.inp", "", "", nullptr), 0);
        ASSERT_EQ(swmm_options_set_ext(before, "MAX_TIMESTEP", "1.2345"), 0);
        const auto target = root / ("draft." + format.extension);
        std::filesystem::remove(target);
        const char* plugin = format.plugin.empty() ? nullptr : format.plugin.c_str();
        ASSERT_EQ(swmm_model_write_with_plugin(before, target.string().c_str(), plugin), 0);
        Engine after;
        ASSERT_EQ(swmm_engine_open(after, target.string().c_str(), "", "", plugin), 0);
        EXPECT_EQ(option(after, "MAX_TIMESTEP", true), option(before, "MAX_TIMESTEP", true));
    }
#else
    GTEST_SKIP() << "2D support is disabled";
#endif
}

TEST(UiFileIO, SelectedGeoPackageWritersReceiveDestinationsAndRunIndependently) {
#ifdef OPENSWMM_HAS_GEOPACKAGE
    const auto root = std::filesystem::absolute("ui_file_io_roundtrip");
    std::filesystem::create_directories(root);
    for (const bool reportOnly : {true, false}) {
        SCOPED_TRACE(reportOnly);
        const std::string stem = reportOnly ? "report_only" : "single_container";
        const auto source = root / (stem + ".inp"), container = root / (stem + ".gpkg");
        std::filesystem::remove(container);
        {
            Engine e;
            ASSERT_EQ(swmm_engine_open(e, "precip_scaling/snow_scaled.inp", "", "", nullptr), 0);
            ASSERT_EQ(swmm_options_set(e, "END_TIME", "00:01:00"), 0);
            ASSERT_EQ(swmm_options_set(e, "REPORT_STEP", "00:00:30"), 0);
            ASSERT_EQ(swmm_options_set(e, "IGNORE_2D", "YES"), 0);
            ASSERT_EQ(swmm_options_set(e, "IGNORE_SNOWMELT", "YES"), 0);
            ASSERT_EQ(swmm_options_set(e, "RPT_NODES", "ALL"), 0);
            ASSERT_EQ(swmm_options_set(e, "RPT_LINKS", "ALL"), 0);
            ASSERT_EQ(swmm_options_set(e, "RPT_SUBCATCHMENTS", "ALL"), 0);
            ASSERT_EQ(swmm_options_set_ext(e, "IO_REPORT_WRITER", "org.hydrocouple.openswmm.plugins.geopackage"), 0);
            ASSERT_EQ(swmm_options_set_ext(e, "IO_REPORT_PATH", container.string().c_str()), 0);
            if (!reportOnly) {
                ASSERT_EQ(swmm_options_set_ext(e, "IO_OUTPUT_WRITER", "org.hydrocouple.openswmm.plugins.geopackage"), 0);
                ASSERT_EQ(swmm_options_set_ext(e, "IO_OUTPUT_PATH", container.string().c_str()), 0);
                ASSERT_EQ(swmm_options_set_ext(e, "IO_SINGLE_CONTAINER", "YES"), 0);
            }
            ASSERT_EQ(swmm_model_write(e, source.string().c_str()), 0);
        }
        Engine e;
        ASSERT_EQ(swmm_engine_open(e, source.string().c_str(), (root/(stem+".rpt")).string().c_str(),
                                   (root/(stem+".out")).string().c_str(), nullptr), 0);
        ASSERT_EQ(swmm_engine_initialize(e), 0) << swmm_get_last_error_msg(e);
        ASSERT_EQ(swmm_engine_start(e, 1), 0) << swmm_get_last_error_msg(e);
        double elapsed = 0;
        int steps = 0;
        do {
            ASSERT_EQ(swmm_engine_step(e, &elapsed), 0) << swmm_get_last_error_msg(e);
            ASSERT_LT(++steps, 10000);
        } while (elapsed > 0);
        ASSERT_EQ(swmm_engine_end(e), 0);
        ASSERT_EQ(swmm_engine_report(e), 0);
        ASSERT_EQ(swmm_engine_close(e), 0);
        ASSERT_TRUE(std::filesystem::exists(container));
        EXPECT_GT(std::filesystem::file_size(container), 10000u);
        Engine saved;
        ASSERT_EQ(swmm_engine_open(saved, container.string().c_str(), "", "",
            "org.hydrocouple.openswmm.plugins.geopackage"), 0);
        EXPECT_EQ(swmm_aquifer_count(saved), 0);
        EXPECT_STREQ(swmm_subcatch_get_snowpack(saved, 0), "SP1");
    }
#else
    GTEST_SKIP() << "GeoPackage support is disabled";
#endif
}

}
