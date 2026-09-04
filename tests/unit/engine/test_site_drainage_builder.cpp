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
 * @file test_site_drainage_builder.cpp
 * @brief End-to-end model-builder parity test for the site drainage model.
 *
 * @details Companion to @ref test_site_drainage_model.cpp, which *loads*
 *          @c data/site_drainage_model.inp.  This test instead *builds* the
 *          same model entirely through the programmatic builder API
 *          (@c swmm_engine_new → @c *_add / @c *_set_* → @c swmm_finalize_model),
 *          runs it, writes it back out to an @c .inp, and confirms results
 *          parity.  It exercises:
 *            - 7 subcatchments with Horton infiltration + OUTLET routing
 *            - 11 junctions + 1 free outfall
 *            - 11 conduits (trapezoidal + circular sections, tags)
 *            - 1 rain gage + a 2-yr design-storm time series (239 points)
 *            - TSS pollutant + 4 land uses (buildup / washoff / coverages)
 *            - Dynamic-wave routing, 5-second routing step, 30-hour run
 *
 *          Three checks:
 *            1. @c BuildFinalizeCounts    — builder produces the right object
 *               counts and properties.
 *            2. @c WriteReopenRoundTrip   — @c swmm_model_write round-trips the
 *               built model with bit-tight result parity.
 *            3. @c ParityWithReferenceInp — built-model results match a fresh
 *               run of @c site_drainage_model.inp (peaks + continuity).
 *
 * @note Working directory is @c tests/unit/engine/data/ (set by CMakeLists),
 *       so the reference @c .inp is opened by its relative name and temp
 *       artifacts go to @c std::filesystem::temp_directory_path().
 *
 * @note Known builder-API gaps — several [SUBCATCHMENTS]/[SUBAREAS] columns have
 *       no setter, so the built model falls back to engine defaults for them.
 *       None affect the hydraulic peaks / continuity these tests assert for THIS
 *       model, but they would matter for other inputs:
 *         - CurbLen        (curb_length, default 0) — only feeds CURB-normalized
 *           pollutant buildup MASS; pure water-quality, no hydraulic effect here.
 *         - PctZero        (frac_imperv_no_store, default 0 vs the file's 25) —
 *           fraction of impervious area with no depression storage.  Shifts the
 *           earliest impervious runoff; negligible vs. this design-storm peak
 *           (ParityWithReferenceInp passes at 1e-3), but a real fidelity gap.
 *         - RouteTo/PctRouted (subarea_routing default OUTLET / pct_routed
 *           default 0) — match this model (all OUTLET), but a model using
 *           IMPERV/PERV inter-subarea routing could not be reproduced.
 *         - SnowPack assignment — swmm_snowpack_add exists but no
 *           swmm_subcatch_set_snowpack to attach one (unused in this model).
 *       Also: spatial sections ([MAP]/[COORDINATES]/[VERTICES]/[Polygons]) have
 *       no builder setters (display-only, no simulation effect).
 *       Tracked as follow-ups:
 *       swmm_subcatch_set_curb_length / _pct_zero / _subarea_routing /
 *       _pct_routed / _snowpack.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @license  Apache-2.0
 * @ingroup  engine_tests
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/engine/openswmm_subcatchments.h>
#include <openswmm/engine/openswmm_gages.h>
#include <openswmm/engine/openswmm_tables.h>
#include <openswmm/engine/openswmm_pollutants.h>
#include <openswmm/engine/openswmm_quality.h>
#include <openswmm/engine/openswmm_massbalance.h>

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// Enum codes (mirrors header docs; named constants exist for node/link/gage,
// integer literals used where the header documents the code inline).
// ---------------------------------------------------------------------------
constexpr int XSECT_CIRCULAR    = 0;   // geom1 = diameter
constexpr int XSECT_TRAPEZOIDAL = 4;   // geom1=depth, geom2=base, geom3/4=slopes
constexpr int OUTFALL_FREE      = 0;   // swmm_node_set_outfall_type
constexpr int POLLUT_UNITS_MGL  = 0;   // MG/L

constexpr int BUILDUP_NONE      = 0;
constexpr int BUILDUP_EXP       = 2;
constexpr int WASHOFF_EXP       = 1;
constexpr int WASHOFF_RC        = 2;
constexpr int NORMALIZER_AREA   = 0;
constexpr int NORMALIZER_CURB   = 1;

// ---------------------------------------------------------------------------
// RAII temp file (pattern from test_hotstart.cpp).
// ---------------------------------------------------------------------------
class TempFile {
public:
    explicit TempFile(const std::string& suffix) {
        path_ = (fs::temp_directory_path() /
                 ("openswmm_builder_" +
                  std::to_string(reinterpret_cast<std::uintptr_t>(this)) +
                  suffix)).string();
    }
    ~TempFile() { std::error_code ec; fs::remove(path_, ec); }
    const std::string& path() const { return path_; }
private:
    std::string path_;
};

// ---------------------------------------------------------------------------
// Captured run results, keyed by object id so the programmatic build and the
// reference run compare regardless of internal ordering.
// ---------------------------------------------------------------------------
struct Results {
    std::map<std::string, double> link_peak_flow;   // max |flow|
    std::map<std::string, double> node_peak_depth;   // max depth
    std::map<std::string, double> subcatch_runoff;   // peak runoff
    double runoff_cont_err  = 0.0;
    double routing_cont_err = 0.0;
};

// ---------------------------------------------------------------------------
// 2-yr design-storm time series — transcribed verbatim from the
// [TIMESERIES] block of site_drainage_model.inp.  First column is elapsed
// minutes (clock "H:MM"); converted to decimal days at insert time.
// ---------------------------------------------------------------------------
struct TsPoint { int minutes; double value; };
const std::vector<TsPoint> kStorm = {
    {6, 0}, {12, 0.00284}, {18, 0.00284}, {24, 0.00284}, {30, 0.003124},
    {36, 0.00284}, {42, 0.003124}, {48, 0.00284}, {54, 0.003124}, {60, 0.003124},
    {66, 0.003124}, {72, 0.003124}, {78, 0.003124}, {84, 0.003124}, {90, 0.003408},
    {96, 0.003124}, {102, 0.003408}, {108, 0.003124}, {114, 0.003408}, {120, 0.003408},
    {126, 0.003408}, {132, 0.003408}, {138, 0.003408}, {144, 0.003692}, {150, 0.003408},
    {156, 0.003408}, {162, 0.003692}, {168, 0.003408}, {174, 0.003692}, {180, 0.003692},
    {186, 0.003692}, {192, 0.003692}, {198, 0.003692}, {204, 0.003692}, {210, 0.003976},
    {216, 0.003692}, {222, 0.003976}, {228, 0.003976}, {234, 0.003692}, {240, 0.003976},
    {246, 0.003976}, {252, 0.003976}, {258, 0.003976}, {264, 0.00426}, {270, 0.00426},
    {276, 0.00426}, {282, 0.00426}, {288, 0.00426}, {294, 0.00426}, {300, 0.004544},
    {306, 0.004544}, {312, 0.004544}, {318, 0.004544}, {324, 0.004828}, {330, 0.004828},
    {336, 0.004544}, {342, 0.005112}, {348, 0.004828}, {354, 0.004828}, {360, 0.005112},
    {366, 0.005112}, {372, 0.005112}, {378, 0.005112}, {384, 0.005396}, {390, 0.005396},
    {396, 0.005112}, {402, 0.00568}, {408, 0.005396}, {414, 0.005396}, {420, 0.00568},
    {426, 0.00568}, {432, 0.00568}, {438, 0.00568}, {444, 0.005964}, {450, 0.005964},
    {456, 0.005964}, {462, 0.005964}, {468, 0.005964}, {474, 0.005964}, {480, 0.006248},
    {486, 0.006248}, {492, 0.006248}, {498, 0.006816}, {504, 0.006816}, {510, 0.007384},
    {516, 0.007384}, {522, 0.007952}, {528, 0.008236}, {534, 0.008236}, {540, 0.00852},
    {546, 0.009088}, {552, 0.009088}, {558, 0.009088}, {564, 0.009088}, {570, 0.009088},
    {576, 0.009088}, {582, 0.009372}, {588, 0.009656}, {594, 0.010224}, {600, 0.010792},
    {606, 0.011076}, {612, 0.011644}, {618, 0.012496}, {624, 0.013064}, {630, 0.013632},
    {636, 0.014484}, {642, 0.015336}, {648, 0.016472}, {654, 0.017608}, {660, 0.018744},
    {666, 0.01988}, {672, 0.021868}, {678, 0.024424}, {684, 0.027264}, {690, 0.030104},
    {696, 0.03266}, {702, 0.067592}, {708, 0.135184}, {714, 0.216976}, {720, 0.389364},
    {726, 0.270084}, {732, 0.05396}, {738, 0.047144}, {744, 0.040896}, {750, 0.034648},
    {756, 0.027832}, {762, 0.023856}, {768, 0.02272}, {774, 0.021016}, {780, 0.019312},
    {786, 0.018176}, {792, 0.01704}, {798, 0.015904}, {804, 0.015336}, {810, 0.014768},
    {816, 0.013632}, {822, 0.013064}, {828, 0.012496}, {834, 0.011928}, {840, 0.01136},
    {846, 0.010792}, {852, 0.010508}, {858, 0.010224}, {864, 0.00994}, {870, 0.009656},
    {876, 0.009656}, {882, 0.009372}, {888, 0.009372}, {894, 0.009088}, {900, 0.008804},
    {906, 0.00852}, {912, 0.00852}, {918, 0.008236}, {924, 0.007952}, {930, 0.007668},
    {936, 0.007668}, {942, 0.007384}, {948, 0.007384}, {954, 0.0071}, {960, 0.006816},
    {966, 0.006532}, {972, 0.006532}, {978, 0.006248}, {984, 0.006532}, {990, 0.006248},
    {996, 0.006248}, {1002, 0.006248}, {1008, 0.005964}, {1014, 0.005964}, {1020, 0.005964},
    {1026, 0.005964}, {1032, 0.00568}, {1038, 0.00568}, {1044, 0.00568}, {1050, 0.005396},
    {1056, 0.00568}, {1062, 0.005396}, {1068, 0.005396}, {1074, 0.005112}, {1080, 0.005396},
    {1086, 0.005112}, {1092, 0.005112}, {1098, 0.004828}, {1104, 0.005112}, {1110, 0.004828},
    {1116, 0.004828}, {1122, 0.004544}, {1128, 0.004828}, {1134, 0.004544}, {1140, 0.004544},
    {1146, 0.00426}, {1152, 0.004544}, {1158, 0.00426}, {1164, 0.00426}, {1170, 0.00426},
    {1176, 0.003976}, {1182, 0.003976}, {1188, 0.003976}, {1194, 0.003692}, {1200, 0.003976},
    {1206, 0.003692}, {1212, 0.003692}, {1218, 0.003692}, {1224, 0.003692}, {1230, 0.003692},
    {1236, 0.003408}, {1242, 0.003692}, {1248, 0.003692}, {1254, 0.003408}, {1260, 0.003692},
    {1266, 0.003408}, {1272, 0.003692}, {1278, 0.003408}, {1284, 0.003692}, {1290, 0.003408},
    {1296, 0.003408}, {1302, 0.003692}, {1308, 0.003408}, {1314, 0.003408}, {1320, 0.003408},
    {1326, 0.003408}, {1332, 0.003408}, {1338, 0.003408}, {1344, 0.003408}, {1350, 0.003408},
    {1356, 0.003124}, {1362, 0.003408}, {1368, 0.003408}, {1374, 0.003124}, {1380, 0.003408},
    {1386, 0.003124}, {1392, 0.003408}, {1398, 0.003124}, {1404, 0.003408}, {1410, 0.003124},
    {1416, 0.003124}, {1422, 0.003408}, {1428, 0.003124}, {1434, 0.003124},
};

// ---------------------------------------------------------------------------
// Builder — construct the site drainage model in `e` (a fresh swmm_engine_new()
// handle) and finalize it.  Mirrors site_drainage_model.inp exactly.
// ---------------------------------------------------------------------------
void build_site_drainage_model(SWMM_Engine e) {
    // --- [OPTIONS] — swmm_options_set accepts the exact INP option strings. ---
    auto opt = [&](const char* k, const char* v) {
        ASSERT_EQ(swmm_options_set(e, k, v), SWMM_OK) << "option " << k;
    };
    opt("FLOW_UNITS",          "CFS");
    opt("INFILTRATION",        "HORTON");
    opt("FLOW_ROUTING",        "DYNWAVE");
    opt("LINK_OFFSETS",        "DEPTH");
    opt("MIN_SLOPE",           "0");
    opt("ALLOW_PONDING",       "NO");
    opt("SKIP_STEADY_STATE",   "NO");
    opt("START_DATE",          "01/01/1998");
    opt("START_TIME",          "00:00:00");
    opt("REPORT_START_DATE",   "01/01/1998");
    opt("REPORT_START_TIME",   "00:00:00");
    opt("END_DATE",            "01/02/1998");
    opt("END_TIME",            "06:00:00");
    opt("SWEEP_START",         "01/01");
    opt("SWEEP_END",           "12/31");
    opt("DRY_DAYS",            "5");
    opt("REPORT_STEP",         "00:05:00");
    opt("WET_STEP",            "00:01:00");
    opt("DRY_STEP",            "00:05:00");
    opt("ROUTING_STEP",        "0:00:05");
    opt("RULE_STEP",           "00:00:00");
    opt("INERTIAL_DAMPING",    "PARTIAL");
    opt("NORMAL_FLOW_LIMITED", "SLOPE");
    opt("FORCE_MAIN_EQUATION", "H-W");
    opt("VARIABLE_STEP",       "0.75");
    opt("LENGTHENING_STEP",    "0");
    opt("MIN_SURFAREA",        "12.566");
    opt("MAX_TRIALS",          "8");
    opt("HEAD_TOLERANCE",      "0.005");
    opt("SYS_FLOW_TOL",        "5");
    opt("LAT_FLOW_TOL",        "5");
    opt("THREADS",             "1");
    opt("MINIMUM_STEP",        "0.5");

    // --- [TIMESERIES] 2-yr ---
    ASSERT_EQ(swmm_timeseries_add(e, "2-yr"), SWMM_OK);
    const int ts = swmm_table_index(e, "2-yr");
    ASSERT_GE(ts, 0);
    int n_storm = 0;
    for (const auto& p : kStorm) {
        ASSERT_EQ(swmm_table_add_point(e, ts, p.minutes / 1440.0, p.value), SWMM_OK);
        ++n_storm;
    }
    // The points above are ELAPSED times (days from the simulation start),
    // the time-only [TIMESERIES] form. Declare that explicitly so the
    // resolve pass anchors them at START_DATE — the engine no longer
    // guesses relative-ness from x-value magnitude (the old x[0] < 366
    // heuristic corrupted mixed relative+dated series).
    ASSERT_EQ(swmm_timeseries_set_relative_info(e, ts, n_storm, 0.0), SWMM_OK);

    // --- [RAINGAGES] RainGage : VOLUME, 0:05 interval, scale 1.0 ---
    ASSERT_EQ(swmm_gage_add(e, "RainGage"), SWMM_OK);
    const int rg = swmm_gage_index(e, "RainGage");
    ASSERT_GE(rg, 0);
    ASSERT_EQ(swmm_gage_set_rain_type(e, rg, SWMM_RAIN_VOLUME), SWMM_OK);
    ASSERT_EQ(swmm_gage_set_rain_interval(e, rg, 300.0), SWMM_OK);   // 0:05
    ASSERT_EQ(swmm_gage_set_data_source(e, rg, SWMM_GAGE_TIMESERIES), SWMM_OK);
    ASSERT_EQ(swmm_gage_set_timeseries(e, rg, "2-yr"), SWMM_OK);
    ASSERT_EQ(swmm_gage_set_scale_factor(e, rg, 1.0), SWMM_OK);

    // --- [POLLUTANTS] TSS (MG/L, all zero) ---
    ASSERT_EQ(swmm_pollutant_add(e, "TSS", POLLUT_UNITS_MGL), SWMM_OK);
    const int tss = swmm_pollutant_index(e, "TSS");
    ASSERT_GE(tss, 0);

    // --- [LANDUSES] ---
    const char* lunames[] = {"Residential_1", "Residential_2",
                             "Commercial", "Undeveloped"};
    for (const char* ln : lunames) ASSERT_EQ(swmm_landuse_add(e, ln), SWMM_OK);
    const int luR1 = swmm_landuse_index(e, "Residential_1");
    const int luR2 = swmm_landuse_index(e, "Residential_2");
    const int luCm = swmm_landuse_index(e, "Commercial");
    const int luUn = swmm_landuse_index(e, "Undeveloped");
    ASSERT_GE(luR1, 0); ASSERT_GE(luR2, 0); ASSERT_GE(luCm, 0); ASSERT_GE(luUn, 0);

    // --- [BUILDUP] func/c1/c2/c3/normalizer ---
    ASSERT_EQ(swmm_buildup_set(e, luR1, tss, BUILDUP_EXP, 0.11, 0.5, 0.0, NORMALIZER_CURB), SWMM_OK);
    ASSERT_EQ(swmm_buildup_set(e, luR2, tss, BUILDUP_EXP, 0.13, 0.5, 0.0, NORMALIZER_CURB), SWMM_OK);
    ASSERT_EQ(swmm_buildup_set(e, luCm, tss, BUILDUP_EXP, 0.15, 0.2, 0.0, NORMALIZER_CURB), SWMM_OK);
    ASSERT_EQ(swmm_buildup_set(e, luUn, tss, BUILDUP_NONE, 0.0, 0.0, 0.0, NORMALIZER_AREA), SWMM_OK);

    // --- [WASHOFF] func/coeff/expon/sweepRmvl/bmpRmvl ---
    ASSERT_EQ(swmm_washoff_set(e, luR1, tss, WASHOFF_EXP, 2.0, 1.8, 0.0, 0.0), SWMM_OK);
    ASSERT_EQ(swmm_washoff_set(e, luR2, tss, WASHOFF_EXP, 4.0, 2.2, 0.0, 0.0), SWMM_OK);
    ASSERT_EQ(swmm_washoff_set(e, luCm, tss, WASHOFF_EXP, 4.0, 2.2, 0.0, 0.0), SWMM_OK);
    ASSERT_EQ(swmm_washoff_set(e, luUn, tss, WASHOFF_RC,  500.0, 2.0, 0.0, 0.0), SWMM_OK);

    // --- [JUNCTIONS] (invert elevation; depths/ponding default 0) ---
    struct J { const char* id; double elev; };
    const J junctions[] = {
        {"J1", 4973}, {"J2", 4969}, {"J3", 4973}, {"J4", 4971},
        {"J5", 4969.8}, {"J6", 4969}, {"J7", 4971.5}, {"J8", 4966.5},
        {"J9", 4964.8}, {"J10", 4963.8}, {"J11", 4963},
    };
    for (const auto& j : junctions) {
        ASSERT_EQ(swmm_node_add(e, j.id, SWMM_NODE_JUNCTION), SWMM_OK);
        ASSERT_EQ(swmm_node_set_invert_elev(e, swmm_node_index(e, j.id), j.elev), SWMM_OK);
    }

    // --- [OUTFALLS] O1 : FREE, no gate ---
    ASSERT_EQ(swmm_node_add(e, "O1", SWMM_NODE_OUTFALL), SWMM_OK);
    const int o1 = swmm_node_index(e, "O1");
    ASSERT_GE(o1, 0);
    ASSERT_EQ(swmm_node_set_invert_elev(e, o1, 4962.0), SWMM_OK);
    ASSERT_EQ(swmm_node_set_outfall_type(e, o1, OUTFALL_FREE), SWMM_OK);
    ASSERT_EQ(swmm_node_set_outfall_flap_gate(e, o1, 0), SWMM_OK);

    // --- [CONDUITS] + [XSECTIONS] + [TAGS] ---
    struct C {
        const char* id; const char* from; const char* to;
        double length; double rough; double in_off; double out_off;
        int shape; double g1, g2, g3, g4;
        const char* tag;
    };
    const C conduits[] = {
        {"C1",  "J1",  "J5",  185, 0.05,  0, 0, XSECT_TRAPEZOIDAL, 3, 5, 5, 5,        "Swale"},
        {"C2",  "J2",  "J11", 526, 0.016, 0, 4, XSECT_TRAPEZOIDAL, 1, 0, 0.0001, 25,  "Gutter"},
        {"C3",  "J3",  "J4",  109, 0.016, 0, 0, XSECT_CIRCULAR,    2.25, 0, 0, 0,     "Culvert"},
        {"C4",  "J4",  "J5",  133, 0.05,  0, 0, XSECT_TRAPEZOIDAL, 3, 5, 5, 5,        "Swale"},
        {"C5",  "J5",  "J6",  207, 0.05,  0, 0, XSECT_TRAPEZOIDAL, 3, 5, 5, 5,        "Swale"},
        {"C6",  "J7",  "J6",  140, 0.05,  0, 0, XSECT_TRAPEZOIDAL, 3, 5, 5, 5,        "Swale"},
        {"C7",  "J6",  "J8",  95,  0.016, 0, 0, XSECT_CIRCULAR,    3.5, 0, 0, 0,      "Culvert"},
        {"C8",  "J8",  "J9",  166, 0.05,  0, 0, XSECT_TRAPEZOIDAL, 3, 5, 5, 5,        "Swale"},
        {"C9",  "J9",  "J10", 320, 0.05,  0, 0, XSECT_TRAPEZOIDAL, 3, 5, 5, 5,        "Swale"},
        {"C10", "J10", "J11", 145, 0.05,  0, 0, XSECT_TRAPEZOIDAL, 3, 5, 5, 5,        "Swale"},
        {"C11", "J11", "O1",  89,  0.016, 0, 0, XSECT_CIRCULAR,    4.75, 0, 0, 0,     "Culvert"},
    };
    for (const auto& c : conduits) {
        ASSERT_EQ(swmm_link_add(e, c.id, SWMM_LINK_CONDUIT), SWMM_OK);
        const int li = swmm_link_index(e, c.id);
        ASSERT_GE(li, 0);
        ASSERT_EQ(swmm_link_set_nodes(e, li, swmm_node_index(e, c.from),
                                            swmm_node_index(e, c.to)), SWMM_OK);
        ASSERT_EQ(swmm_link_set_length(e, li, c.length), SWMM_OK);
        ASSERT_EQ(swmm_link_set_roughness(e, li, c.rough), SWMM_OK);
        ASSERT_EQ(swmm_link_set_offset_up(e, li, c.in_off), SWMM_OK);
        ASSERT_EQ(swmm_link_set_offset_dn(e, li, c.out_off), SWMM_OK);
        ASSERT_EQ(swmm_link_set_initial_flow(e, li, 0.0), SWMM_OK);
        ASSERT_EQ(swmm_link_set_max_flow(e, li, 0.0), SWMM_OK);
        ASSERT_EQ(swmm_link_set_xsect(e, li, c.shape, c.g1, c.g2, c.g3, c.g4), SWMM_OK);
        ASSERT_EQ(swmm_link_set_tag(e, li, c.tag), SWMM_OK);
    }

    // --- [SUBCATCHMENTS] + [SUBAREAS] + [INFILTRATION] ---
    struct S {
        const char* id; const char* outlet;
        double area; double imperv; double width; double slope;
    };
    const S subs[] = {
        {"S1", "J1",  4.55, 56.8, 1587, 2.0},
        {"S2", "J2",  4.74, 63.0, 1653, 2.0},
        {"S3", "J3",  3.74, 39.5, 1456, 3.1},
        {"S4", "J7",  6.79, 49.9, 2331, 3.1},
        {"S5", "J10", 4.79, 87.7, 1670, 2.0},
        {"S6", "J11", 1.98, 95.0, 690,  2.0},
        {"S7", "J10", 2.33, 0.0,  907,  3.1},
    };
    for (const auto& s : subs) {
        ASSERT_EQ(swmm_subcatch_add(e, s.id), SWMM_OK);
        const int si = swmm_subcatch_index(e, s.id);
        ASSERT_GE(si, 0);
        ASSERT_EQ(swmm_subcatch_set_gage(e, si, swmm_gage_index(e, "RainGage")), SWMM_OK);
        ASSERT_EQ(swmm_subcatch_set_outlet(e, si, swmm_node_index(e, s.outlet)), SWMM_OK);
        ASSERT_EQ(swmm_subcatch_set_area(e, si, s.area), SWMM_OK);
        ASSERT_EQ(swmm_subcatch_set_imperv_pct(e, si, s.imperv), SWMM_OK);
        ASSERT_EQ(swmm_subcatch_set_width(e, si, s.width), SWMM_OK);
        ASSERT_EQ(swmm_subcatch_set_slope(e, si, s.slope), SWMM_OK);
        // [SUBAREAS] — identical for all 7 (PctZero 25, RouteTo OUTLET = default).
        ASSERT_EQ(swmm_subcatch_set_n_imperv(e, si, 0.015), SWMM_OK);
        ASSERT_EQ(swmm_subcatch_set_n_perv(e, si, 0.24), SWMM_OK);
        ASSERT_EQ(swmm_subcatch_set_ds_imperv(e, si, 0.06), SWMM_OK);
        ASSERT_EQ(swmm_subcatch_set_ds_perv(e, si, 0.3), SWMM_OK);
        // [INFILTRATION] Horton — identical for all 7.
        ASSERT_EQ(swmm_subcatch_set_infil_horton(e, si, 4.5, 0.2, 6.5, 7.0), SWMM_OK);
    }

    // --- [COVERAGES] (percent, matching the INP/handler convention) ---
    auto cover = [&](const char* sc, int lu, double pct) {
        ASSERT_EQ(swmm_subcatch_set_coverage(e, swmm_subcatch_index(e, sc), lu, pct), SWMM_OK);
    };
    cover("S1", luR1, 100);
    cover("S2", luR1, 27);  cover("S2", luR2, 73);
    cover("S3", luR1, 27);  cover("S3", luR2, 32);
    cover("S4", luR1, 9);   cover("S4", luR2, 30);  cover("S4", luCm, 26);
    cover("S5", luCm, 98);
    cover("S6", luCm, 100);

    // --- Validate + finalize (BUILDING → INITIALIZED). ---
    ASSERT_EQ(swmm_validate_model(e), SWMM_OK)
        << "validate failed: " << swmm_get_last_error_msg(e);
    ASSERT_EQ(swmm_finalize_model(e), SWMM_OK)
        << "finalize failed: " << swmm_get_last_error_msg(e);
}

// ---------------------------------------------------------------------------
// Run an INITIALIZED engine to completion, capturing peaks + continuity.
// (Both a finalized build and an opened+initialized .inp are INITIALIZED.)
// ---------------------------------------------------------------------------
Results run_and_collect(SWMM_Engine e) {
    Results r;

    const int nn = swmm_node_count(e);
    const int nl = swmm_link_count(e);
    const int ns = swmm_subcatch_count(e);

    // Seed maps with object ids so every object appears even if it stays at 0.
    for (int i = 0; i < nl; ++i) r.link_peak_flow[swmm_link_id(e, i)]   = 0.0;
    for (int i = 0; i < nn; ++i) r.node_peak_depth[swmm_node_id(e, i)]  = 0.0;
    for (int i = 0; i < ns; ++i) r.subcatch_runoff[swmm_subcatch_id(e, i)] = 0.0;

    EXPECT_EQ(swmm_engine_start(e, /*save_results=*/0), SWMM_OK)
        << "start failed: " << swmm_get_last_error_msg(e);

    double elapsed = 0.0;
    int steps = 0;
    do {
        const int rc = swmm_engine_step(e, &elapsed);
        if (rc != SWMM_OK) {
            ADD_FAILURE() << "step " << steps << " failed: "
                          << swmm_get_last_error_msg(e);
            break;
        }
        for (int i = 0; i < nl; ++i) {
            double f = 0.0;
            swmm_link_get_flow(e, i, &f);
            double& peak = r.link_peak_flow[swmm_link_id(e, i)];
            if (std::fabs(f) > peak) peak = std::fabs(f);
        }
        for (int i = 0; i < nn; ++i) {
            double d = 0.0;
            swmm_node_get_depth(e, i, &d);
            double& peak = r.node_peak_depth[swmm_node_id(e, i)];
            if (d > peak) peak = d;
        }
        for (int i = 0; i < ns; ++i) {
            double ro = 0.0;
            swmm_subcatch_get_runoff(e, i, &ro);
            double& peak = r.subcatch_runoff[swmm_subcatch_id(e, i)];
            if (ro > peak) peak = ro;
        }
        if (++steps >= 500000) {
            ADD_FAILURE() << "simulation did not terminate";
            break;
        }
    } while (elapsed > 0.0);

    EXPECT_EQ(swmm_engine_end(e), SWMM_OK);
    EXPECT_EQ(swmm_get_runoff_continuity_error(e, &r.runoff_cont_err), SWMM_OK);
    EXPECT_EQ(swmm_get_routing_continuity_error(e, &r.routing_cont_err), SWMM_OK);
    return r;
}

// ---------------------------------------------------------------------------
// Compare two keyed result sets: tol = max(abs, rel*|ref|), ref = `b`.
// ---------------------------------------------------------------------------
void expect_close(const std::map<std::string, double>& a,
                  const std::map<std::string, double>& b,
                  const char* what, double rel, double abs_tol) {
    ASSERT_EQ(a.size(), b.size()) << what << ": object-count mismatch";
    for (const auto& [id, av] : a) {
        const auto it = b.find(id);
        ASSERT_NE(it, b.end()) << what << ": id '" << id << "' missing in reference";
        const double bv = it->second;
        const double tol = std::max(abs_tol, rel * std::fabs(bv));
        EXPECT_LE(std::fabs(av - bv), tol)
            << what << " mismatch for '" << id << "': built=" << av
            << " ref=" << bv;
    }
}

void expect_results_close(const Results& a, const Results& b,
                          double rel, double abs_tol) {
    expect_close(a.link_peak_flow,  b.link_peak_flow,  "link peak flow",  rel, abs_tol);
    expect_close(a.node_peak_depth, b.node_peak_depth, "node peak depth", rel, abs_tol);
    expect_close(a.subcatch_runoff, b.subcatch_runoff, "subcatch runoff", rel, abs_tol);
    EXPECT_LE(std::fabs(a.runoff_cont_err  - b.runoff_cont_err),  std::max(abs_tol, rel))
        << "runoff continuity: built=" << a.runoff_cont_err
        << " ref=" << b.runoff_cont_err;
    EXPECT_LE(std::fabs(a.routing_cont_err - b.routing_cont_err), std::max(abs_tol, rel))
        << "routing continuity: built=" << a.routing_cont_err
        << " ref=" << b.routing_cont_err;
}

// ---------------------------------------------------------------------------
// Fixture — owns engine handles and cleans them up.
// ---------------------------------------------------------------------------
class SiteDrainageBuilderTest : public ::testing::Test {
protected:
    std::vector<SWMM_Engine> engines_;

    SWMM_Engine make_built() {
        SWMM_Engine e = swmm_engine_new();
        EXPECT_NE(e, nullptr);
        engines_.push_back(e);
        build_site_drainage_model(e);
        return e;
    }

    SWMM_Engine make_from_inp(const char* inp, const char* rpt, const char* out) {
        SWMM_Engine e = swmm_engine_create();
        EXPECT_NE(e, nullptr);
        engines_.push_back(e);
        EXPECT_EQ(swmm_engine_open(e, inp, rpt, out, nullptr), SWMM_OK)
            << "open failed: " << swmm_get_last_error_msg(e);
        EXPECT_EQ(swmm_engine_initialize(e), SWMM_OK);
        return e;
    }

    void TearDown() override {
        for (SWMM_Engine e : engines_) {
            if (e) { swmm_engine_close(e); swmm_engine_destroy(e); }
        }
        engines_.clear();
    }
};

// ---------------------------------------------------------------------------
// 1. The builder produces the right object counts + key properties.
// ---------------------------------------------------------------------------
TEST_F(SiteDrainageBuilderTest, BuildFinalizeCounts) {
    SWMM_Engine e = make_built();

    EXPECT_EQ(swmm_node_count(e), 12);      // 11 junctions + 1 outfall
    EXPECT_EQ(swmm_link_count(e), 11);
    EXPECT_EQ(swmm_subcatch_count(e), 7);
    EXPECT_EQ(swmm_gage_count(e), 1);
    EXPECT_EQ(swmm_pollutant_count(e), 1);
    EXPECT_EQ(swmm_landuse_count(e), 4);

    double v = 0.0;
    EXPECT_EQ(swmm_node_get_invert_elev(e, swmm_node_index(e, "J1"), &v), SWMM_OK);
    EXPECT_NEAR(v, 4973.0, 1e-9);
    EXPECT_EQ(swmm_link_get_length(e, swmm_link_index(e, "C1"), &v), SWMM_OK);
    EXPECT_NEAR(v, 185.0, 1e-9);
    EXPECT_EQ(swmm_link_get_roughness(e, swmm_link_index(e, "C11"), &v), SWMM_OK);
    EXPECT_NEAR(v, 0.016, 1e-12);
    EXPECT_EQ(swmm_subcatch_get_area(e, swmm_subcatch_index(e, "S4"), &v), SWMM_OK);
    EXPECT_NEAR(v, 6.79, 1e-9);
}

// ---------------------------------------------------------------------------
// 2. swmm_model_write round-trips the built model with tight result parity.
// ---------------------------------------------------------------------------
TEST_F(SiteDrainageBuilderTest, WriteReopenRoundTrip) {
    TempFile inp(".inp"), rpt(".rpt"), out(".out");

    SWMM_Engine built = make_built();
    ASSERT_EQ(swmm_model_write(built, inp.path().c_str()), SWMM_OK)
        << "model_write failed: " << swmm_get_last_error_msg(built);
    ASSERT_TRUE(fs::exists(inp.path()));

    const Results a = run_and_collect(built);

    SWMM_Engine reopened = make_from_inp(inp.path().c_str(),
                                         rpt.path().c_str(), out.path().c_str());
    const Results b = run_and_collect(reopened);

    // Same construction → effectively exact; allow only FP noise.
    expect_results_close(a, b, /*rel=*/1e-6, /*abs=*/1e-6);
}

// ---------------------------------------------------------------------------
// 3. Built-model results match a fresh run of the reference .inp.
// ---------------------------------------------------------------------------
TEST_F(SiteDrainageBuilderTest, ParityWithReferenceInp) {
    TempFile rpt(".rpt"), out(".out");

    SWMM_Engine built = make_built();
    const Results a = run_and_collect(built);

    SWMM_Engine reference = make_from_inp("site_drainage_model.inp",
                                          rpt.path().c_str(), out.path().c_str());
    const Results c = run_and_collect(reference);

    // Hydraulic peaks + continuity, regression-suite tolerance.
    expect_results_close(a, c, /*rel=*/1e-3, /*abs=*/1e-3);
}

}  // namespace
