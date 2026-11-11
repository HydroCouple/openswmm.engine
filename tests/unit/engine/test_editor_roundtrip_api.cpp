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
 * @file      test_editor_roundtrip_api.cpp
 * @brief     Round-trip coverage for the GUI-editor C API getters/setters.
 *
 * @details The property/editor dialogs need to LOAD an existing definition,
 *          not just write one. These tests assert that each newly added
 *          getter is the exact inverse of its setter (and that the new
 *          pollutant-units setter and aquifer ET-pattern accessors close their
 *          respective round-trips):
 *            - pollutant units      (swmm_pollutant_set/get_units)
 *            - aquifer ET pattern   (swmm_aquifer_set/get_evap_pattern)
 *            - snowpack surfaces     (swmm_snowpack_set/get_{plowable,
 *                                     impervious,pervious,removal} + subcatch)
 *            - inlet params/type    (swmm_inlet_get_params / _get_type)
 *            - LID layers/type      (swmm_lid_get_{surface,soil,storage,
 *                                     drain,type})
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_pollutants.h>
#include <openswmm/engine/openswmm_subcatchments.h>
#include <openswmm/engine/openswmm_infrastructure.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/engine/openswmm_tables.h>
#include <openswmm/engine/openswmm_gages.h>

#include <string>
#include <fstream>
#include <iterator>
#include <limits>

namespace {

// A fresh BUILDING-state engine — the editor config setters are allowed in
// BUILDING / OPENED only.
/**
 * @brief Create an editable BUILDING-state engine for editor API checks.
 *
 * No model objects, initialization or assertion are added; caller closes/destroys ownership.
 * @return Caller-owned swmm_engine_new handle, possibly null after allocation refusal.
 */
SWMM_Engine makeEngine() {
    return swmm_engine_new();
}

/**
 * @brief Close/destroy the sole editor-test engine owner.
 *
 * Ignores close status and does not clear the caller's by-value handle. Does not remove artifact
 * files or return a lifecycle result.
 *
 * @param[in] e Owned live or null handle; all aliases become unusable after return.
 */
void destroy(SWMM_Engine e) {
    swmm_engine_close(e);
    swmm_engine_destroy(e);
}

} // namespace

TEST(EditorDividerRecord, ValidatesBeforeMutationAndKeepsProjectFlowUnits) {
    auto e = makeEngine();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_options_set(e, "FLOW_UNITS", "CMS"), SWMM_OK);
    ASSERT_EQ(swmm_node_add(e, "D", SWMM_NODE_DIVIDER), SWMM_OK);
    ASSERT_EQ(swmm_node_add(e, "N", SWMM_NODE_JUNCTION), SWMM_OK);
    ASSERT_EQ(swmm_link_add(e, "L", SWMM_LINK_CONDUIT), SWMM_OK);
    ASSERT_EQ(swmm_link_set_nodes(e, 0, 0, 1), SWMM_OK);
    ASSERT_EQ(swmm_curve_add(e, "Division", 2), SWMM_OK);
    SWMM_DividerParams value{3, 0, 0, 2.125, 1.5, 1.25}, got{};
    ASSERT_EQ(swmm_node_set_divider_params(e, 0, &value), SWMM_OK);
    ASSERT_EQ(swmm_node_get_divider_params(e, 0, &got), SWMM_OK);
    EXPECT_EQ(got.method, 3);
    EXPECT_EQ(got.diversion_link, 0);
    EXPECT_EQ(got.curve, 0);
    EXPECT_DOUBLE_EQ(got.cutoff_flow, 2.125);
    EXPECT_DOUBLE_EQ(got.weir_height, 1.5);
    EXPECT_DOUBLE_EQ(got.weir_coefficient, 1.25);
    value.weir_height = 0;
    EXPECT_EQ(swmm_node_set_divider_params(e, 0, &value), SWMM_ERR_BADPARAM);
    ASSERT_EQ(swmm_node_get_divider_params(e, 0, &got), SWMM_OK);
    EXPECT_DOUBLE_EQ(got.weir_height, 1.5);
    EXPECT_EQ(swmm_node_get_divider_params(e, 1, &got), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_node_get_divider_params(e, 0, nullptr), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_node_set_divider_params(nullptr, 0, &value), SWMM_ERR_BADHANDLE);
    value = got;
    value.method = 2;
    value.curve = -1;
    EXPECT_EQ(swmm_node_set_divider_params(e, 0, &value), SWMM_ERR_BADPARAM);
    destroy(e);
}

TEST(EditorSubcatchOutlet, TargetsAreExclusiveAndCascadeCyclesAreRejected) {
    auto e = makeEngine();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_node_add(e, "N", SWMM_NODE_JUNCTION), SWMM_OK);
    for (const char* id : {"A", "B", "C"}) ASSERT_EQ(swmm_subcatch_add(e, id), SWMM_OK);
    ASSERT_EQ(swmm_subcatch_set_outlet(e, 0, 0), SWMM_OK);
    ASSERT_EQ(swmm_subcatch_set_outlet_subcatch(e, 0, 1), SWMM_OK);
    int node = -2, sc = -2;
    ASSERT_EQ(swmm_subcatch_get_outlet(e, 0, &node), SWMM_OK);
    EXPECT_EQ(node, -1);
    ASSERT_EQ(swmm_subcatch_set_outlet_subcatch(e, 1, 2), SWMM_OK);
    EXPECT_EQ(swmm_subcatch_set_outlet_subcatch(e, 2, 0), SWMM_ERR_BADPARAM);
    ASSERT_EQ(swmm_subcatch_get_outlet_subcatch(e, 2, &sc), SWMM_OK);
    EXPECT_EQ(sc, -1);
    ASSERT_EQ(swmm_subcatch_set_outlet(e, 0, 0), SWMM_OK);
    ASSERT_EQ(swmm_subcatch_get_outlet_subcatch(e, 0, &sc), SWMM_OK);
    EXPECT_EQ(sc, -1);
    EXPECT_EQ(swmm_subcatch_set_outlet(e, 0, 99), SWMM_ERR_BADPARAM);
    ASSERT_EQ(swmm_subcatch_get_outlet(e, 0, &node), SWMM_OK);
    EXPECT_EQ(node, 0);
    ASSERT_EQ(swmm_node_add(e, "B", SWMM_NODE_JUNCTION), SWMM_OK);
    EXPECT_EQ(swmm_subcatch_set_outlet_subcatch(e, 0, 1), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_subcatch_set_outlet(e, 0, 1), SWMM_ERR_BADPARAM);
    ASSERT_EQ(swmm_subcatch_get_outlet(e, 0, &node), SWMM_OK);
    EXPECT_EQ(node, 0);
    destroy(e);
}

TEST(EditorRainSource, EmptySeriesRestoresAnUnconfiguredAssignment) {
    auto e = makeEngine();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_gage_add(e, "G"), SWMM_OK);
    ASSERT_EQ(swmm_timeseries_add(e, "Rain"), SWMM_OK);
    ASSERT_EQ(swmm_gage_set_timeseries(e, 0, "Rain"), SWMM_OK);
    ASSERT_EQ(swmm_gage_set_timeseries(e, 0, ""), SWMM_OK);
    char id[64]{};
    ASSERT_EQ(swmm_gage_get_timeseries(e, 0, id, sizeof(id)), SWMM_OK);
    EXPECT_STREQ(id, "");
    EXPECT_EQ(swmm_gage_set_timeseries(e, 0, nullptr), SWMM_ERR_BADPARAM);
    destroy(e);
}

TEST(EditorRainSource, SavedFileSourceDoesNotRevertToALatentSeries) {
    auto e = makeEngine();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_gage_add(e, "G"), SWMM_OK);
    ASSERT_EQ(swmm_timeseries_add(e, "Rain"), SWMM_OK);
    ASSERT_EQ(swmm_gage_set_timeseries(e, 0, "Rain"), SWMM_OK);
    ASSERT_EQ(swmm_gage_set_filename(e, 0, "rain.csv", "Station"), SWMM_OK);
    const char* path = "editor_rain_source_roundtrip.inp";
    ASSERT_EQ(swmm_model_write(e, path), SWMM_OK);
    std::ifstream file(path);
    const std::string saved((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    EXPECT_NE(saved.find("FILE "), std::string::npos);
    EXPECT_EQ(saved.find("TIMESERIES Rain"), std::string::npos);
    destroy(e);
}

// ---------------------------------------------------------------------------
// Pollutant units
// ---------------------------------------------------------------------------
TEST(EditorRoundtripApi, PollutantUnitsSetGet) {
    SWMM_Engine e = makeEngine();
    ASSERT_EQ(swmm_pollutant_add(e, "TSS", /*MG/L*/ 0), SWMM_OK);

    int u = -1;
    ASSERT_EQ(swmm_pollutant_get_units(e, 0, &u), SWMM_OK);
    EXPECT_EQ(u, 0);

    ASSERT_EQ(swmm_pollutant_set_units(e, 0, /*#/L*/ 2), SWMM_OK);
    ASSERT_EQ(swmm_pollutant_get_units(e, 0, &u), SWMM_OK);
    EXPECT_EQ(u, 2);

    // Out-of-range code is rejected and leaves the stored value untouched.
    EXPECT_EQ(swmm_pollutant_set_units(e, 0, 5), SWMM_ERR_BADPARAM);
    ASSERT_EQ(swmm_pollutant_get_units(e, 0, &u), SWMM_OK);
    EXPECT_EQ(u, 2);

    EXPECT_EQ(swmm_pollutant_set_units(e, 99, 1), SWMM_ERR_BADINDEX);
    destroy(e);
}

// ---------------------------------------------------------------------------
// Aquifer upper-zone evaporation pattern (the one string column)
// ---------------------------------------------------------------------------
TEST(EditorRoundtripApi, AquiferEvapPatternSetGet) {
    SWMM_Engine e = makeEngine();
    ASSERT_EQ(swmm_aquifer_add(e, "AQ1"), SWMM_OK);

    char buf[64] = {'x'};
    // Defaults to empty.
    ASSERT_EQ(swmm_aquifer_get_evap_pattern(e, 0, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "");

    ASSERT_EQ(swmm_aquifer_set_evap_pattern(e, 0, "ET_MONTHLY"), SWMM_OK);
    ASSERT_EQ(swmm_aquifer_get_evap_pattern(e, 0, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "ET_MONTHLY");

    // Null clears.
    ASSERT_EQ(swmm_aquifer_set_evap_pattern(e, 0, nullptr), SWMM_OK);
    ASSERT_EQ(swmm_aquifer_get_evap_pattern(e, 0, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "");

    // Truncation: NUL-terminated, never overruns.
    ASSERT_EQ(swmm_aquifer_set_evap_pattern(e, 0, "ABCDEFGH"), SWMM_OK);
    char small[4] = {'\0'};
    ASSERT_EQ(swmm_aquifer_get_evap_pattern(e, 0, small, sizeof(small)), SWMM_OK);
    EXPECT_STREQ(small, "ABC");

    EXPECT_EQ(swmm_aquifer_get_evap_pattern(e, 0, buf, 0), SWMM_ERR_BADPARAM);
    destroy(e);
}

// ---------------------------------------------------------------------------
// Snowpack surfaces + removal + destination subcatchment
// ---------------------------------------------------------------------------
TEST(EditorRoundtripApi, SnowpackSurfacesRoundTrip) {
    SWMM_Engine e = makeEngine();
    ASSERT_EQ(swmm_snowpack_add(e, "SP1"), SWMM_OK);

    ASSERT_EQ(swmm_snowpack_set_plowable(e, 0, 0.001, 0.01, 25.0, 0.10, 1.0, 0.5, 0.20), SWMM_OK);
    double cmin, cmax, tbase, fwfrac, sd0, fw0, last;
    cmin = cmax = tbase = fwfrac = sd0 = fw0 = last = -1.0;
    ASSERT_EQ(swmm_snowpack_get_plowable(e, 0, &cmin, &cmax, &tbase, &fwfrac, &sd0, &fw0, &last),
              SWMM_OK);
    EXPECT_DOUBLE_EQ(cmin, 0.001);
    EXPECT_DOUBLE_EQ(cmax, 0.01);
    EXPECT_DOUBLE_EQ(tbase, 25.0);
    EXPECT_DOUBLE_EQ(fwfrac, 0.10);
    EXPECT_DOUBLE_EQ(sd0, 1.0);
    EXPECT_DOUBLE_EQ(fw0, 0.5);
    EXPECT_DOUBLE_EQ(last, 0.20);

    ASSERT_EQ(swmm_snowpack_set_impervious(e, 0, 0.002, 0.02, 26.0, 0.11, 1.1, 0.6, 3.0), SWMM_OK);
    ASSERT_EQ(swmm_snowpack_set_pervious(e, 0, 0.003, 0.03, 27.0, 0.12, 1.2, 0.7, 4.0), SWMM_OK);
    double v[7];
    ASSERT_EQ(swmm_snowpack_get_impervious(e, 0, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]),
              SWMM_OK);
    EXPECT_DOUBLE_EQ(v[0], 0.002);
    EXPECT_DOUBLE_EQ(v[6], 3.0); // IMPERVIOUS last = 100%-cover depth
    ASSERT_EQ(swmm_snowpack_get_pervious(e, 0, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]),
              SWMM_OK);
    EXPECT_DOUBLE_EQ(v[2], 27.0);
    EXPECT_DOUBLE_EQ(v[6], 4.0);

    // Null out-params must be tolerated (read only what you ask for).
    double only_tbase = -1.0;
    ASSERT_EQ(swmm_snowpack_get_plowable(e, 0, nullptr, nullptr, &only_tbase, nullptr, nullptr,
                                         nullptr, nullptr),
              SWMM_OK);
    EXPECT_DOUBLE_EQ(only_tbase, 25.0);

    ASSERT_EQ(swmm_snowpack_set_removal(e, 0, 2.0, 0.1, 0.2, 0.3, 0.4, 0.0), SWMM_OK);
    double dsnow, fout, fimp, fperv, fimelt, fsub;
    ASSERT_EQ(swmm_snowpack_get_removal(e, 0, &dsnow, &fout, &fimp, &fperv, &fimelt, &fsub),
              SWMM_OK);
    EXPECT_DOUBLE_EQ(dsnow, 2.0);
    EXPECT_DOUBLE_EQ(fout, 0.1);
    EXPECT_DOUBLE_EQ(fimp, 0.2);
    EXPECT_DOUBLE_EQ(fperv, 0.3);
    EXPECT_DOUBLE_EQ(fimelt, 0.4);
    EXPECT_DOUBLE_EQ(fsub, 0.0);

    ASSERT_EQ(swmm_snowpack_set_removal_subcatch(e, 0, "S_DUMP"), SWMM_OK);
    char buf[64] = {'\0'};
    ASSERT_EQ(swmm_snowpack_get_removal_subcatch(e, 0, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "S_DUMP");

    EXPECT_EQ(swmm_snowpack_set_plowable(e, 9, 0, 0, 0, 0, 0, 0, 0), SWMM_ERR_BADINDEX);
    destroy(e);
}

// ---------------------------------------------------------------------------
// Inlet params + type
// ---------------------------------------------------------------------------
TEST(EditorRoundtripApi, InletParamsAndTypeRoundTrip) {
    SWMM_Engine e = makeEngine();
    ASSERT_EQ(swmm_inlet_add(e, "IN1", "GRATE"), SWMM_OK);
    ASSERT_EQ(swmm_inlet_set_params(e, 0, /*length*/ 2.0, /*width*/ 1.5, "P-50", /*open_area*/ 0.8,
                                    /*splash_veloc*/ 5.0),
              SWMM_OK);

    double length = 0, width = 0, open_area = 0, splash = 0;
    char grate[32] = {'\0'};
    ASSERT_EQ(
        swmm_inlet_get_params(e, 0, &length, &width, grate, sizeof(grate), &open_area, &splash),
        SWMM_OK);
    EXPECT_DOUBLE_EQ(length, 2.0);
    EXPECT_DOUBLE_EQ(width, 1.5);
    EXPECT_DOUBLE_EQ(open_area, 0.8);
    EXPECT_DOUBLE_EQ(splash, 5.0);
    EXPECT_STREQ(grate, "P-50");

    char type[32] = {'\0'};
    ASSERT_EQ(swmm_inlet_get_type(e, 0, type, sizeof(type)), SWMM_OK);
    EXPECT_STREQ(type, "GRATE");

    // grate_type may be skipped with a null buffer.
    length = 0;
    ASSERT_EQ(swmm_inlet_get_params(e, 0, &length, nullptr, nullptr, 0, nullptr, nullptr), SWMM_OK);
    EXPECT_DOUBLE_EQ(length, 2.0);

    EXPECT_EQ(swmm_inlet_get_type(e, 0, type, 0), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_inlet_get_type(e, 9, type, sizeof(type)), SWMM_ERR_BADINDEX);
    destroy(e);
}

// ---------------------------------------------------------------------------
// LID layers + type
// ---------------------------------------------------------------------------
TEST(EditorRoundtripApi, LidLayersAndTypeRoundTrip) {
    SWMM_Engine e = makeEngine();
    ASSERT_EQ(swmm_lid_add(e, "BC1", /*BIO_CELL*/ 0), SWMM_OK);

    ASSERT_EQ(swmm_lid_set_surface(e, 0, /*storage*/ 2.0, /*roughness*/ 0.1, /*slope*/ 1.0),
              SWMM_OK);
    ASSERT_EQ(swmm_lid_set_soil(e, 0, /*thick*/ 12.0, /*porosity*/ 0.5, /*fc*/ 0.2, /*wp*/ 0.1,
                                /*ksat*/ 0.5, /*kslope*/ 10.0),
              SWMM_OK);
    ASSERT_EQ(swmm_lid_set_storage(e, 0, /*thick*/ 12.0, /*void_frac*/ 0.75, /*ksat*/ 0.5),
              SWMM_OK);
    ASSERT_EQ(swmm_lid_set_drain(e, 0, /*coeff*/ 1.0, /*expon*/ 0.5, /*offset*/ 6.0), SWMM_OK);

    double storage, roughness, slope;
    ASSERT_EQ(swmm_lid_get_surface(e, 0, &storage, &roughness, &slope), SWMM_OK);
    EXPECT_DOUBLE_EQ(storage, 2.0);
    EXPECT_DOUBLE_EQ(roughness, 0.1);
    EXPECT_DOUBLE_EQ(slope, 1.0);

    double thick, poros, fc, wp, ksat, kslope;
    ASSERT_EQ(swmm_lid_get_soil(e, 0, &thick, &poros, &fc, &wp, &ksat, &kslope), SWMM_OK);
    EXPECT_DOUBLE_EQ(thick, 12.0);
    EXPECT_DOUBLE_EQ(poros, 0.5);
    EXPECT_DOUBLE_EQ(fc, 0.2);
    EXPECT_DOUBLE_EQ(wp, 0.1);
    EXPECT_DOUBLE_EQ(ksat, 0.5);
    EXPECT_DOUBLE_EQ(kslope, 10.0);

    double s_thick, void_frac, s_ksat;
    ASSERT_EQ(swmm_lid_get_storage(e, 0, &s_thick, &void_frac, &s_ksat), SWMM_OK);
    EXPECT_DOUBLE_EQ(s_thick, 12.0);
    EXPECT_DOUBLE_EQ(void_frac, 0.75);
    EXPECT_DOUBLE_EQ(s_ksat, 0.5);

    double coeff, expon, offset;
    ASSERT_EQ(swmm_lid_get_drain(e, 0, &coeff, &expon, &offset), SWMM_OK);
    EXPECT_DOUBLE_EQ(coeff, 1.0);
    EXPECT_DOUBLE_EQ(expon, 0.5);
    EXPECT_DOUBLE_EQ(offset, 6.0);

    int type = -1;
    ASSERT_EQ(swmm_lid_get_type(e, 0, &type), SWMM_OK);
    EXPECT_EQ(type, 0); // BIO_CELL

    // A second control with a different type code round-trips its enum too.
    ASSERT_EQ(swmm_lid_add(e, "PP1", /*PERM_PAVEMENT*/ 4), SWMM_OK);
    ASSERT_EQ(swmm_lid_get_type(e, 1, &type), SWMM_OK);
    EXPECT_EQ(type, 4);

    // PAVEMENT (6 vals) round-trips on the permeable-pavement control.
    ASSERT_EQ(swmm_lid_set_pavement(e, 1, /*thick*/ 6.0, /*void_ratio*/ 0.15,
                                    /*frac_imperv*/ 0.0, /*ksat*/ 100.0,
                                    /*clog_factor*/ 0.0, /*regen_days*/ 0.0),
              SWMM_OK);
    double p_thick, p_void, p_fi, p_ksat, p_clog, p_regen;
    ASSERT_EQ(swmm_lid_get_pavement(e, 1, &p_thick, &p_void, &p_fi, &p_ksat, &p_clog, &p_regen),
              SWMM_OK);
    EXPECT_DOUBLE_EQ(p_thick, 6.0);
    EXPECT_DOUBLE_EQ(p_void, 0.15);
    EXPECT_DOUBLE_EQ(p_fi, 0.0);
    EXPECT_DOUBLE_EQ(p_ksat, 100.0);

    // Older setters preserve the degree. A rejected update is atomic.
    ASSERT_EQ(swmm_lid_set_pavement_ex(e, 1, 6, 0.15, 0, 2, 5, 0.125, 0.5), SWMM_OK);
    ASSERT_EQ(swmm_lid_set_pavement(e, 1, 7, 0.15, 0, 2, 5, 0.125), SWMM_OK);
    for (double invalid : {-0.1, 1.1, std::numeric_limits<double>::quiet_NaN()})
        EXPECT_EQ(swmm_lid_set_pavement_ex(e, 1, 8, 0.15, 0, 2, 5, 0.125, invalid),
                  SWMM_ERR_BADPARAM);
    double degree = -1.0;
    ASSERT_EQ(swmm_lid_get_pavement_ex(e, 1, &p_thick, nullptr, nullptr, nullptr, nullptr, nullptr,
                                      &degree), SWMM_OK);
    EXPECT_DOUBLE_EQ(p_thick, 7.0);
    EXPECT_DOUBLE_EQ(degree, 0.5);

    // DRAINMAT (3 vals) round-trips on a green-roof control.
    ASSERT_EQ(swmm_lid_add(e, "GR1", /*GREEN_ROOF*/ 2), SWMM_OK);
    ASSERT_EQ(swmm_lid_set_drainmat(e, 2, /*thick*/ 1.0, /*void_frac*/ 0.5, /*roughness*/ 0.1),
              SWMM_OK);
    double d_thick, d_void, d_rough;
    ASSERT_EQ(swmm_lid_get_drainmat(e, 2, &d_thick, &d_void, &d_rough), SWMM_OK);
    EXPECT_DOUBLE_EQ(d_thick, 1.0);
    EXPECT_DOUBLE_EQ(d_void, 0.5);
    EXPECT_DOUBLE_EQ(d_rough, 0.1);

    // Null out-params tolerated; bad index rejected.
    double only_void = -1.0;
    ASSERT_EQ(swmm_lid_get_pavement(e, 1, nullptr, &only_void, nullptr, nullptr, nullptr, nullptr),
              SWMM_OK);
    EXPECT_DOUBLE_EQ(only_void, 0.15);
    EXPECT_EQ(swmm_lid_get_surface(e, 9, &storage, &roughness, &slope), SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_lid_set_pavement(e, 9, 1, 0.1, 0, 1, 0, 0), SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_lid_set_drainmat(e, 0, -1.0, 0.5, 0.1), SWMM_ERR_BADPARAM);
    destroy(e);
}
