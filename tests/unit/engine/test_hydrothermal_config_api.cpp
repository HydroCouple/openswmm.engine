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
 * @file test_hydrothermal_config_api.cpp
 * @brief Engine-level contracts behind the hydrothermal configuration editors
 *        (closure audit C1, 2026-10-17).
 *
 *   - The pre-run groundwater row of the transport matrix, resolved from the
 *     authored [2D_AQUIFER] before any runtime state exists, agrees with the
 *     row the initialized kernel reports, across MESH/PER_SUBCATCH, aquifer
 *     absent/present, GROUNDWATER AUTO/YES/NO, IGNORE_2D and the
 *     project/groundwater temperature switches. The draft preview of the
 *     saved options agrees with both.
 *   - swmm_hydrothermal_edit_atomic commits on success and restores every
 *     value and provenance flag on refusal, exception or nested use.
 *   - Closure audit C7: DRY_ONLY scope. Lumped subcatchment evaporation stops
 *     while it rains; mesh surface evaporation and 2D-aquifer ET do not read
 *     the flag (owner disposition: retain and document, lumped parity frozen).
 *   - Heat snapshots restore exactly and refuse a foreign engine; the new
 *     dry-policy, module and sediment-provenance setters refuse bad input.
 *
 * Decks land under tests/output/hydrothermal_config_api (CLAUDE.md §4.1).
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_climate.h>
#include <openswmm/engine/openswmm_coupling.h>
#include <openswmm/engine/openswmm_heat.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_process_components.h>
#include <openswmm/engine/openswmm_snow.h>

#include "2d/SurfaceRouter2D.hpp"
#include "2d/subsurface/SubsurfaceData.hpp"
#include "core/SWMMEngine.hpp"

namespace fs = std::filesystem;

namespace {

const fs::path kOutDir = fs::path(OPENSWMM_HYDROTHERMAL_TEST_OUT_DIR) / "hydrothermal_config_api";

struct Combo {
    bool per_subcatch = false;
    bool aquifer = true;
    const char* groundwater = nullptr;   // nullptr = key omitted (AUTO)
    bool ignore_2d = false;
    bool heat = true;
    bool gw_temperature = true;
    bool mesh_temperature = true;

    std::string tag() const {
        std::ostringstream s;
        s << (per_subcatch ? "sub" : "mesh") << '_' << (aquifer ? "aq" : "noaq") << '_'
          << (groundwater ? groundwater : "omit") << (ignore_2d ? "_ig2d" : "")
          << (heat ? "_heat" : "_noheat") << (gw_temperature ? "" : "_gwT0")
          << (mesh_temperature ? "" : "_meshT0");
        return s.str();
    }
};

/// The 10 m two-triangle pan of test_gw_transport_kernel with one pollutant,
/// one subcatchment draining to J1, and the switches of @p c.
std::string deck(const Combo& c) {
    std::ostringstream s;
    s << "[OPTIONS]\n"
         "FLOW_UNITS CMS\nFLOW_ROUTING DYNWAVE\n"
         "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
         "END_DATE 01/01/2026\nEND_TIME 00:20:00\n"
         "REPORT_STEP 00:01:00\nWET_STEP 00:01:00\nDRY_STEP 00:01:00\nROUTING_STEP 5\n"
      << "HEAT_TRANSPORT " << (c.heat ? "YES" : "NO") << '\n'
      << "IGNORE_2D " << (c.ignore_2d ? "YES" : "NO") << "\n\n"
         "[POLLUTANTS]\nTSS MG/L 10.0 0.0 0.0 0.0 NO * 0.0 0.0\n\n"
         "[RAINGAGES]\nRG1 INTENSITY 0:10 1.0 TIMESERIES RAIN\n\n"
         "[TIMESERIES]\nRAIN 0:00 20.0\nRAIN 0:10 0.0\n\n"
         "[SUBCATCHMENTS]\nS1 RG1 J1 0.01 50 100 0.5 0\n\n"
         "[SUBAREAS]\nS1 0.01 0.1 0.05 0.05 25 OUTLET\n\n"
         "[INFILTRATION]\nS1 3.0 0.5 4 7 0\n\n"
         "[JUNCTIONS]\nJ1 -2.0 4.0 0 0 0\n\n"
         "[OUTFALLS]\nO1 -2.5 FREE NO\n\n"
         "[CONDUITS]\nC1 J1 O1 30.0 0.013 0 0 0\n\n"
         "[XSECTIONS]\nC1 CIRCULAR 0.5 0 0 0 1\n\n"
         "[2D_OPTIONS]\nINTEGRATOR EXPLICIT\nLTS_TIERS 1\nMAX_TIMESTEP 5\n"
         "DRY_DEPTH 0.001\nCOUPLING_CD 0.7\nREPORT_2D NO\nRAINFALL_MODE SYSTEM\n"
      << "TRANSPORT_TEMPERATURE " << (c.mesh_temperature ? "YES" : "NO") << '\n';
    if (c.groundwater) s << "GROUNDWATER " << c.groundwater << '\n';
    s << "\n[2D_VERTICES]\n0.0 0.0 0.0\n10.0 0.0 0.0\n10.0 10.0 0.0\n0.0 10.0 0.0\n\n"
         "[2D_TRIANGLES]\n0 1 2 0.03 0.0\n0 2 3 0.03 0.0\n\n"
         "[2D_INFILTRATION_DEFAULTS]\n*  CONSTANT  40.0  -  -  -  -\n\n";
    if (c.aquifer) {
        s << "[2D_AQUIFER_OPTIONS]\nCLOSURE CLOSED_FORM\nNODE_ENROLMENT ROWS\n"
          << "MODE " << (c.per_subcatch ? "PER_SUBCATCH" : "MESH") << "\n\n"
             "[2D_AQUIFER]\n*  36.0  1.0  0.45  0.10  2.0  HG0 0.5\n\n"
             "[GW_TRANSPORT_OPTIONS]\n"
          << "TRANSPORT_TEMPERATURE " << (c.gw_temperature ? "YES" : "NO") << "\n\n";
    }
    s << "[COORDINATES]\nJ1 5.0 5.0\nO1 40.0 40.0\n\n"
         "[POLYGONS]\nS1 0 0\nS1 10 0\nS1 10 10\nS1 0 10\n\n"
         "[REPORT]\nINPUT NO\n";
    return s.str();
}

SWMM_Engine openDeck(const std::string& tag, const std::string& body) {
    fs::create_directories(kOutDir);
    const fs::path inp = kOutDir / (tag + ".inp");
    { std::ofstream f(inp); f << body; }
    SWMM_Engine e = swmm_engine_create();
    if (!e) return nullptr;
    if (swmm_engine_open(e, inp.string().c_str(), (kOutDir / (tag + ".rpt")).string().c_str(),
                         (kOutDir / (tag + ".out")).string().c_str(), nullptr) != SWMM_OK) {
        swmm_engine_destroy(e);
        return nullptr;
    }
    return e;
}

void closeDeck(SWMM_Engine e) {
    swmm_engine_close(e);
    swmm_engine_destroy(e);
}

std::string row(const SWMM_TransportMatrix& m, int domain) {
    std::ostringstream s;
    for (int c = 0; c < SWMM_TRANSPORT_CLASS_COUNT; ++c)
        s << swmm_transport_class_name(c) << '=' << m.cell[domain][c].state << '/'
          << m.cell[domain][c].count << "; ";
    return s.str();
}

std::string reasons(const SWMM_TransportMatrix& m, int domain) {
    std::ostringstream s;
    for (int c = 0; c < SWMM_TRANSPORT_CLASS_COUNT; ++c) s << m.cell[domain][c].reason << " | ";
    return s.str();
}

constexpr int kGroundwater = SWMM_TRANSPORT_DOMAIN_GROUNDWATER;

}  // namespace

// ---------------------------------------------------------------------------
// Pre-run groundwater resolution (audit B3 / C1)
// ---------------------------------------------------------------------------

TEST(HydrothermalConfig, PreRunGroundwaterRowMatchesInitializedKernel) {
    ASSERT_STREQ(swmm_transport_domain_name(kGroundwater), "Groundwater");
    fs::create_directories(kOutDir);
    std::ofstream log(kOutDir / "pre_run_matrix.log");
    int compared = 0, refused = 0;
    for (bool per_subcatch : {false, true})
    for (bool aquifer : {true, false})
    for (const char* gw : {static_cast<const char*>(nullptr), "AUTO", "YES", "NO"})
    for (bool ignore_2d : {false, true})
    for (int t = 0; t < 4; ++t) {
        Combo c;
        c.per_subcatch = per_subcatch; c.aquifer = aquifer; c.groundwater = gw;
        c.ignore_2d = ignore_2d;
        c.heat = t != 1; c.gw_temperature = t != 2; c.mesh_temperature = t != 3;
        if (!aquifer && (per_subcatch || !c.gw_temperature)) continue;   // no aquifer rows to vary
        SCOPED_TRACE(c.tag());
        SWMM_Engine e = openDeck(c.tag(), deck(c));
        ASSERT_NE(e, nullptr) << "deck did not open";

        SWMM_TransportMatrix saved{};
        ASSERT_EQ(swmm_get_transport_matrix(e, &saved), SWMM_OK);
        SWMM_TransportDraft draft{};
        draft.ignore_quality = 0; draft.heat = c.heat; draft.age = 0;
        draft.ignore_2d = c.ignore_2d;
        for (int k = 0; k < SWMM_TRANSPORT_CLASS_COUNT; ++k) draft.surface_class[k] = 1;
        draft.surface_class[SWMM_TRANSPORT_CLASS_TEMPERATURE] = c.mesh_temperature;
        draft.groundwater = !gw || std::strcmp(gw, "AUTO") == 0 ? -1 : std::strcmp(gw, "YES") == 0 ? 1 : 0;
        SWMM_TransportMatrix preview{};
        ASSERT_EQ(swmm_preview_transport_matrix(e, &draft, &preview), SWMM_OK);
        EXPECT_EQ(row(preview, kGroundwater), row(saved, kGroundwater))
            << "draft of the saved options differs from the saved matrix";

        log << c.tag() << std::endl;   // flushed: names the case if initialize aborts
        const int init = swmm_engine_initialize(e);
        log << "\n  pre-run:  " << row(saved, kGroundwater) << reasons(saved, kGroundwater) << '\n';
        if (init != SWMM_OK) {
            // A refused configuration cannot be compared with a runtime row.
            // The pre-run row is a plan; the refusal is recorded for review.
            const char* msg = swmm_get_last_error_msg(e);
            log << "  initialize refused: " << (msg ? msg : "") << '\n';
            ++refused;
            closeDeck(e);
            continue;
        }
        SWMM_TransportMatrix live{};
        ASSERT_EQ(swmm_get_transport_matrix(e, &live), SWMM_OK);
        log << "  runtime:  " << row(live, kGroundwater) << reasons(live, kGroundwater) << '\n';
        EXPECT_EQ(row(saved, kGroundwater), row(live, kGroundwater))
            << "pre-run reasons: " << reasons(saved, kGroundwater)
            << "\nruntime reasons: " << reasons(live, kGroundwater);
        ++compared;
        closeDeck(e);
    }
    log << "compared " << compared << ", refused at initialize " << refused << '\n';
    EXPECT_GT(compared, 20);
}

// ---------------------------------------------------------------------------
// Atomic hydrothermal edits
// ---------------------------------------------------------------------------

namespace {

/// Every value and provenance bit the editors write, as one comparable string.
std::string hydrothermalState(SWMM_Engine e) {
    std::ostringstream s;
    s.precision(17);
    double v = 0; int i = 0; unsigned u = 0; double m[12]; double a[10];
    swmm_climate_get_latitude(e, &v); s << v << '|';
    swmm_climate_get_evap_type(e, &i); s << i << '|';
    swmm_climate_get_evap_monthly(e, m, 12); for (double x : m) s << x << ',';
    swmm_climate_get_adjust_evaporation(e, m, 12); for (double x : m) s << x << ',';
    swmm_climate_get_adjust_temperature(e, m, 12); for (double x : m) s << x << ',';
    swmm_climate_get_adjust_conductivity(e, m, 12); for (double x : m) s << x << ',';
    swmm_climate_get_adc_pervious(e, a, 10); for (double x : a) s << x << ',';
    swmm_climate_get_snow_temp(e, &v); s << v << '|';
    swmm_snow_get_model(e, &i); s << i << '|';
    swmm_heat_get_enabled(e, &i); s << i << '|';
    swmm_heat_get_module(e, SWMM_HEAT_SEDIMENT_EXCHANGE, &i); s << i << '|';
    swmm_heat_get_dry_policy(e, &i); s << i << '|';
    swmm_heat_get_authoring_flags(e, &u); s << u << '|';
    swmm_heat_get_source_temp(e, SWMM_HEAT_SRC_GW, &v); s << v << '|';
    swmm_heat_sediment_temperature_configured(e, SWMM_HEAT_SED_GROUND_TEMPERATURE, &i); s << i << '|';
    swmm_heat_get_sediment(e, SWMM_HEAT_SED_GROUND_TEMPERATURE, &v); s << v << '|';
    s << swmm_process_component_count(e) << '|';
    char path[512] = {}, original[512] = {};
    swmm_file_path_get(e, SWMM_FILE_CLIMATE_TEMP, "", path, sizeof path, original, sizeof original);
    s << path << '|' << original;
    return s.str();
}

/// Writes one value through every setter family the transaction covers.
int writeEverything(SWMM_Engine e) {
    const double ones[12] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    const double half[12] = {.5, .5, .5, .5, .5, .5, .5, .5, .5, .5, .5, .5};
    const double adc[10] = {.1, .2, .3, .4, .5, .6, .7, .8, .9, 1};
    int rc = SWMM_OK;
    const auto ok = [&rc](int r) { if (rc == SWMM_OK) rc = r; };
    ok(swmm_climate_set_latitude(e, 12.25));
    ok(swmm_climate_set_evap_type(e, 1));
    ok(swmm_climate_set_evap_monthly(e, half, 12));
    ok(swmm_climate_set_adjust_evaporation(e, ones, 12));
    ok(swmm_climate_set_adjust_temperature(e, half, 12));
    ok(swmm_climate_set_adjust_conductivity(e, half, 12));
    ok(swmm_climate_set_adc_pervious(e, adc, 10));
    ok(swmm_climate_set_snow_temp(e, 30.5));
    ok(swmm_snow_set_model(e, 1));
    ok(swmm_options_set_ext(e, "HEAT_TRANSPORT", "YES"));
    ok(swmm_heat_set_module(e, SWMM_HEAT_SEDIMENT_EXCHANGE, 1));
    ok(swmm_heat_set_dry_policy(e, 1));
    ok(swmm_heat_set_source_temp(e, SWMM_HEAT_SRC_GW, 17.5));
    ok(swmm_heat_set_sediment(e, SWMM_HEAT_SED_GROUND_TEMPERATURE, 9.5));
    ok(swmm_heat_set_solar(e, SWMM_HEAT_SOLAR_LATITUDE, 0.0));
    ok(swmm_heat_set_solar(e, SWMM_HEAT_SOLAR_LONGITUDE, 0.0));
    ok(swmm_process_component_register(e, "HEAT", "atomic_heat.cfg"));
    ok(swmm_file_path_set(e, SWMM_FILE_CLIMATE_TEMP, "", "climate_atomic.dat"));
    return rc;
}

SWMM_Engine buildingEngine() {
    SWMM_Engine e = swmm_engine_new();
    if (e) swmm_climate_set_latitude(e, 41.5);
    return e;
}

}  // namespace

TEST(HydrothermalConfig, AtomicEditCommitsEverySetterFamily) {
    SWMM_Engine e = buildingEngine();
    ASSERT_NE(e, nullptr);
    const std::string before = hydrothermalState(e);
    ASSERT_EQ(writeEverything(e), SWMM_OK) << "fixture setters must all succeed outside a transaction";
    const std::string direct = hydrothermalState(e);
    swmm_engine_destroy(e);

    e = buildingEngine();
    ASSERT_EQ(hydrothermalState(e), before);
    ASSERT_EQ(swmm_hydrothermal_edit_atomic(e, [](SWMM_Engine x, void*) -> int { return writeEverything(x); },
                                            nullptr), SWMM_OK);
    EXPECT_EQ(hydrothermalState(e), direct);
    swmm_engine_destroy(e);
}

TEST(HydrothermalConfig, AtomicEditRestoresValuesAndProvenanceOnRefusal) {
    SWMM_Engine e = buildingEngine();
    ASSERT_NE(e, nullptr);
    const std::string before = hydrothermalState(e);
    // A late, genuine engine refusal after every family has been written.
    const int rc = swmm_hydrothermal_edit_atomic(e, [](SWMM_Engine x, void*) -> int {
        const int r = writeEverything(x);
        if (r != SWMM_OK) return r;
        return swmm_heat_set_dry_policy(x, 7);
    }, nullptr);
    EXPECT_EQ(rc, SWMM_ERR_BADPARAM);
    EXPECT_EQ(hydrothermalState(e), before);
    int configured = 1;
    ASSERT_EQ(swmm_heat_sediment_temperature_configured(e, SWMM_HEAT_SED_GROUND_TEMPERATURE, &configured), SWMM_OK);
    EXPECT_EQ(configured, 0) << "provenance of the ground temperature leaked";
    unsigned flags = 99;
    ASSERT_EQ(swmm_heat_get_authoring_flags(e, &flags), SWMM_OK);
    EXPECT_EQ(flags & 61u, 0u) << "explicit-site or configured bits leaked: " << flags;
    swmm_engine_destroy(e);
}

TEST(HydrothermalConfig, AtomicEditRestoresOnExceptionAndRefusesNesting) {
    SWMM_Engine e = buildingEngine();
    ASSERT_NE(e, nullptr);
    const std::string before = hydrothermalState(e);
    EXPECT_NE(swmm_hydrothermal_edit_atomic(e, [](SWMM_Engine x, void*) -> int {
        writeEverything(x);
        throw std::runtime_error("editor failure");
    }, nullptr), SWMM_OK);
    EXPECT_EQ(hydrothermalState(e), before);

    struct Nested { int inner = SWMM_OK; } n;
    EXPECT_EQ(swmm_hydrothermal_edit_atomic(e, [](SWMM_Engine x, void* user) -> int {
        auto& s = *static_cast<Nested*>(user);
        s.inner = swmm_hydrothermal_edit_atomic(x, [](SWMM_Engine, void*) -> int { return SWMM_OK; }, nullptr);
        return swmm_climate_set_latitude(x, 33.0);
    }, &n), SWMM_OK);
    EXPECT_EQ(n.inner, SWMM_ERR_LIFECYCLE);
    double lat = 0;
    swmm_climate_get_latitude(e, &lat);
    EXPECT_EQ(lat, 33.0) << "a refused nested call must not abort the outer transaction";
    // The guard is released: a later transaction runs.
    EXPECT_EQ(swmm_hydrothermal_edit_atomic(e, [](SWMM_Engine, void*) -> int { return SWMM_OK; }, nullptr), SWMM_OK);
    swmm_engine_destroy(e);
}

TEST(HydrothermalConfig, AtomicEditRefusesInvalidInputsAndRunningEngines) {
    EXPECT_NE(swmm_hydrothermal_edit_atomic(nullptr, [](SWMM_Engine, void*) -> int { return SWMM_OK; }, nullptr), SWMM_OK);
    SWMM_Engine e = buildingEngine();
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(swmm_hydrothermal_edit_atomic(e, nullptr, nullptr), SWMM_ERR_BADPARAM);
    swmm_engine_destroy(e);

    Combo c;
    e = openDeck("atomic_initialized", deck(c));
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_engine_initialize(e), SWMM_OK);
    bool called = false;
    EXPECT_EQ(swmm_hydrothermal_edit_atomic(e, [](SWMM_Engine, void* user) -> int {
        *static_cast<bool*>(user) = true; return SWMM_OK;
    }, &called), SWMM_ERR_LIFECYCLE);
    EXPECT_FALSE(called) << "the callback ran on an initialized engine";
    closeDeck(e);
}

// ---------------------------------------------------------------------------
// Heat snapshots and the new heat setters
// ---------------------------------------------------------------------------

TEST(HydrothermalConfig, HeatSnapshotRestoresExactlyAndRefusesForeignEngine) {
    SWMM_Engine e = buildingEngine();
    SWMM_Engine other = buildingEngine();
    ASSERT_TRUE(e && other);
    ASSERT_EQ(swmm_heat_set_source_temp(e, SWMM_HEAT_SRC_GW, 0.1 + 0.2), SWMM_OK);
    const std::string before = hydrothermalState(e);
    SWMM_HeatSnapshot snap = swmm_heat_snapshot_create(e);
    ASSERT_NE(snap, nullptr);
    ASSERT_EQ(swmm_heat_set_source_temp(e, SWMM_HEAT_SRC_GW, 25.0), SWMM_OK);
    ASSERT_EQ(swmm_heat_set_module(e, SWMM_HEAT_SEDIMENT_EXCHANGE, 1), SWMM_OK);
    ASSERT_EQ(swmm_heat_set_sediment(e, SWMM_HEAT_SED_GROUND_TEMPERATURE, 4.0), SWMM_OK);
    ASSERT_EQ(swmm_process_component_register(e, "HEAT", "snap_heat.cfg"), SWMM_OK);
    EXPECT_EQ(swmm_heat_snapshot_restore(other, snap), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_heat_snapshot_restore(e, nullptr), SWMM_ERR_BADPARAM);
    ASSERT_EQ(swmm_heat_snapshot_restore(e, snap), SWMM_OK);
    EXPECT_EQ(hydrothermalState(e), before);
    swmm_heat_snapshot_destroy(snap);
    swmm_engine_destroy(other);
    swmm_engine_destroy(e);
}

TEST(HydrothermalConfig, NewHeatSettersRefuseInvalidInput) {
    SWMM_Engine e = buildingEngine();
    ASSERT_NE(e, nullptr);
    const std::string before = hydrothermalState(e);
    int value = 0;
    EXPECT_EQ(swmm_heat_set_dry_policy(e, -1), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_heat_set_dry_policy(e, 3), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_heat_get_dry_policy(e, nullptr), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_heat_set_module(e, 4, 1), SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_heat_get_module(e, 4, &value), SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_heat_sediment_temperature_configured(e, SWMM_HEAT_SED_DENSITY, &value), SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_heat_sediment_temperature_configured(e, SWMM_HEAT_SED_GROUND_TEMPERATURE, nullptr), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_heat_clear_sediment_temperature(e, SWMM_HEAT_SED_DENSITY), SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_heat_get_lid_material(e, nullptr), SWMM_ERR_BADPARAM);
    EXPECT_EQ(hydrothermalState(e), before) << "a refused setter changed the model";

    // Module 3 round-trips independently of the other three.
    ASSERT_EQ(swmm_heat_set_module(e, SWMM_HEAT_SEDIMENT_EXCHANGE, 1), SWMM_OK);
    for (int m : {SWMM_HEAT_SURFACE_EXCHANGE, SWMM_HEAT_RADIATIVE_EXCHANGE, SWMM_HEAT_LAYER_CONDUCTION}) {
        ASSERT_EQ(swmm_heat_get_module(e, m, &value), SWMM_OK);
        EXPECT_EQ(value, 0) << "module " << m;
    }
    ASSERT_EQ(swmm_heat_get_module(e, SWMM_HEAT_SEDIMENT_EXCHANGE, &value), SWMM_OK);
    EXPECT_EQ(value, 1);
    // Clearing an explicit ground temperature restores inheritance.
    ASSERT_EQ(swmm_heat_set_sediment(e, SWMM_HEAT_SED_GROUND_TEMPERATURE, 4.0), SWMM_OK);
    ASSERT_EQ(swmm_heat_sediment_temperature_configured(e, SWMM_HEAT_SED_GROUND_TEMPERATURE, &value), SWMM_OK);
    EXPECT_EQ(value, 1);
    ASSERT_EQ(swmm_heat_clear_sediment_temperature(e, SWMM_HEAT_SED_GROUND_TEMPERATURE), SWMM_OK);
    ASSERT_EQ(swmm_heat_sediment_temperature_configured(e, SWMM_HEAT_SED_GROUND_TEMPERATURE, &value), SWMM_OK);
    EXPECT_EQ(value, 0);
    swmm_engine_destroy(e);
}

TEST(HydrothermalConfig, TransportPreviewRefusesMalformedDrafts) {
    SWMM_Engine e = buildingEngine();
    ASSERT_NE(e, nullptr);
    SWMM_TransportDraft d{};
    SWMM_TransportMatrix m{};
    EXPECT_EQ(swmm_preview_transport_matrix(e, nullptr, &m), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_preview_transport_matrix(e, &d, nullptr), SWMM_ERR_BADPARAM);
    d.heat = 2;
    EXPECT_EQ(swmm_preview_transport_matrix(e, &d, &m), SWMM_ERR_BADPARAM);
    d.heat = 1; d.groundwater = 2;
    EXPECT_EQ(swmm_preview_transport_matrix(e, &d, &m), SWMM_ERR_BADPARAM);
    d.groundwater = -1;
    EXPECT_EQ(swmm_preview_transport_matrix(e, &d, &m), SWMM_OK);
    int heat = 1;
    swmm_heat_get_enabled(e, &heat);
    EXPECT_EQ(heat, 0) << "preview wrote the draft into the model";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// C7 — DRY_ONLY scope per domain
// ---------------------------------------------------------------------------

namespace {

struct EvapVolumes {
    double lumped_ft3=0,mesh_m3=0,aquifer_et_m3=0;
    double potential=0,surface=0,soil=0,unused=0,duration=0;
    bool ok=false;
};

/// Rain falls for the whole run over a ponded mesh with an aquifer whose
/// table is near the ground, and over one subcatchment. Only DRY_ONLY varies.
EvapVolumes runSurface(const std::string& tag, const std::string& evaporation,
                       const std::string& rain);

EvapVolumes runWet(bool dry_only) {
    return runSurface(dry_only ? "dry_only_yes" : "dry_only_no",
                      std::string("[EVAPORATION]\nCONSTANT 200.0\nDRY_ONLY ") + (dry_only ? "YES" : "NO") + "\n\n",
                      "RAIN 0:00 5.0\nRAIN 0:10 5.0\nRAIN 0:20 5.0\nRAIN 0:30 5.0\n");
}

EvapVolumes runSurface(const std::string& tag, const std::string& evaporation,
                       const std::string& rain) {
    std::ostringstream s;
    s << "[OPTIONS]\nFLOW_UNITS CMS\nFLOW_ROUTING DYNWAVE\n"
         "START_DATE 01/01/2026\nSTART_TIME 00:00:00\nEND_DATE 01/01/2026\nEND_TIME 00:20:00\n"
         "REPORT_STEP 00:01:00\nWET_STEP 00:01:00\nDRY_STEP 00:01:00\nROUTING_STEP 5\n\n"
      << evaporation
      << "[RAINGAGES]\nRG1 INTENSITY 0:10 1.0 TIMESERIES RAIN\n\n"
      << "[TIMESERIES]\n" << rain << "\n"
         "[SUBCATCHMENTS]\nS1 RG1 J1 0.01 50 100 0.5 0\n\n"
         "[SUBAREAS]\nS1 0.01 0.1 2.0 2.0 0 OUTLET\n\n"
         "[INFILTRATION]\nS1 3.0 0.5 4 7 0\n\n"
         "[JUNCTIONS]\nJ1 -2.0 4.0 0 0 0\n\n[OUTFALLS]\nO1 -2.5 FREE NO\n\n"
         "[CONDUITS]\nC1 J1 O1 30.0 0.013 0 0 0\n\n[XSECTIONS]\nC1 CIRCULAR 0.5 0 0 0 1\n\n"
         "[2D_OPTIONS]\nINTEGRATOR EXPLICIT\nLTS_TIERS 1\nMAX_TIMESTEP 5\n"
         "DRY_DEPTH 0.001\nCOUPLING_CD 0.7\nREPORT_2D NO\nRAINFALL_MODE SYSTEM\nEVAPORATION CLIMATE\n\n"
         "[2D_VERTICES]\n0.0 0.0 0.0\n10.0 0.0 0.0\n10.0 10.0 0.0\n0.0 10.0 0.0\n\n"
         "[2D_TRIANGLES]\n0 1 2 0.03 0.05\n0 2 3 0.03 0.0\n\n"
         "[2D_INFILTRATION_DEFAULTS]\n*  CONSTANT  1.0  -  -  -  -\n\n"
         "[2D_AQUIFER_OPTIONS]\nCLOSURE CLOSED_FORM\nNODE_ENROLMENT ROWS\nGW_ET BOTH\n\n"
         "[2D_AQUIFER]\n*  36.0  1.0  0.45  0.10  2.0  HG0 0.9\n\n"
         "[COORDINATES]\nJ1 5.0 5.0\nO1 40.0 40.0\n\n"
         "[POLYGONS]\nS1 20 0\nS1 30 0\nS1 30 10\nS1 20 10\n\n"
         "[REPORT]\nINPUT NO\n";
    EvapVolumes v;
    SWMM_Engine e = openDeck(tag, s.str());
    if (!e) return v;
    if (swmm_engine_initialize(e) == SWMM_OK && swmm_engine_start(e, 1) == SWMM_OK) {
        double elapsed = 0.0;
        int status = SWMM_OK;
        while ((status = swmm_engine_step(e, &elapsed)) == SWMM_OK && elapsed > 0.0) {}
        swmm_engine_end(e);
        const auto& ctx = static_cast<openswmm::SWMMEngine*>(e)->context();
        v.lumped_ft3 = ctx.mass_balance.runoff_evap;
        v.mesh_m3 = ctx.mass_balance_2d.evap_out;
        v.duration=ctx.current_time;
        if (ctx.twod_io.aquifer_state) {
            const auto& gw=*ctx.twod_io.aquifer_state;
            v.aquifer_et_m3=gw.led_et;
            const auto sum=[](const auto& values){return std::accumulate(values.begin(),values.end(),0.0);};
            v.potential=sum(gw.et_potential_cumulative);v.surface=sum(gw.et_surface_cumulative);
            v.soil=sum(gw.et_soil_cumulative);v.unused=sum(gw.et_unused_cumulative);
        }
        v.ok = status == SWMM_OK;
    }
    closeDeck(e);
    return v;
}

}  // namespace

TEST(HydrothermalConfig, SurfaceAndSoilEtSpendOneAtmosphericDemand) {
    const auto v=runWet(false);ASSERT_TRUE(v.ok);
    ASSERT_GT(v.surface,0);ASSERT_GT(v.soil,0);
    EXPECT_NEAR(v.potential,v.surface+v.soil+v.unused,1e-10);
    EXPECT_LE(v.mesh_m3+v.aquifer_et_m3,200.0/1000/86400*100*v.duration+1e-10);
}

TEST(HydrothermalConfig, DryOnlyStopsLumpedEvaporationButNotMeshOrAquifer) {
    const EvapVolumes no = runWet(false), yes = runWet(true);
    ASSERT_TRUE(no.ok && yes.ok) << "wet-period decks did not run";
    std::ofstream(kOutDir / "dry_only_scope.log")
        << "domain, DRY_ONLY NO, DRY_ONLY YES\n"
        << "lumped subcatchment evaporation (ft3), " << no.lumped_ft3 << ", " << yes.lumped_ft3 << '\n'
        << "mesh surface evaporation (m3), " << no.mesh_m3 << ", " << yes.mesh_m3 << '\n'
        << "2D aquifer ET (m3), " << no.aquifer_et_m3 << ", " << yes.aquifer_et_m3 << '\n';
    // Lumped: rain falls at every step, so DRY_ONLY suppresses evaporation.
    EXPECT_GT(no.lumped_ft3, 0.0);
    EXPECT_EQ(yes.lumped_ft3, 0.0);
    // Mesh and aquifer: the flag is not read; demand is identical.
    EXPECT_GT(no.mesh_m3, 0.0);
    EXPECT_DOUBLE_EQ(yes.mesh_m3, no.mesh_m3);
    EXPECT_DOUBLE_EQ(yes.aquifer_et_m3, no.aquifer_et_m3);
}

// ---------------------------------------------------------------------------
// C3 — authored configuration reaches the runtime
// ---------------------------------------------------------------------------

TEST(HydrothermalConfig, ZeroBaseEvaporationPlusAdjustmentEqualsTheConstantRate) {
    // [ADJUSTMENTS] EVAP is ADDED to the rate, so CONSTANT 0 + 150 must be the
    // same demand as CONSTANT 150 in every domain that reads project climate.
    const std::string rain = "RAIN 0:00 5.0\nRAIN 0:10 0.0\nRAIN 0:20 0.0\nRAIN 0:30 0.0\n";
    const EvapVolumes adjusted = runSurface("evap_zero_plus_adjustment",
        "[EVAPORATION]\nCONSTANT 0.0\nDRY_ONLY NO\n\n"
        "[ADJUSTMENTS]\nEVAPORATION 150 150 150 150 150 150 150 150 150 150 150 150\n\n", rain);
    const EvapVolumes constant = runSurface("evap_constant",
        "[EVAPORATION]\nCONSTANT 150.0\nDRY_ONLY NO\n\n", rain);
    const EvapVolumes none = runSurface("evap_zero", "[EVAPORATION]\nCONSTANT 0.0\nDRY_ONLY NO\n\n", rain);
    ASSERT_TRUE(adjusted.ok && constant.ok && none.ok);
    std::ofstream(kOutDir / "evaporation_adjustment.log")
        << "domain, CONSTANT 0, CONSTANT 0 + ADJUSTMENTS 150, CONSTANT 150\n"
        << "lumped (ft3), " << none.lumped_ft3 << ", " << adjusted.lumped_ft3 << ", " << constant.lumped_ft3 << '\n'
        << "mesh (m3), " << none.mesh_m3 << ", " << adjusted.mesh_m3 << ", " << constant.mesh_m3 << '\n'
        << "aquifer ET (m3), " << none.aquifer_et_m3 << ", " << adjusted.aquifer_et_m3 << ", " << constant.aquifer_et_m3 << '\n';
    EXPECT_EQ(none.lumped_ft3, 0.0);
    EXPECT_EQ(none.mesh_m3, 0.0);
    EXPECT_EQ(none.aquifer_et_m3, 0.0);
    EXPECT_GT(adjusted.lumped_ft3, 0.0);
    EXPECT_GT(adjusted.mesh_m3, 0.0);
    EXPECT_DOUBLE_EQ(adjusted.lumped_ft3, constant.lumped_ft3);
    EXPECT_DOUBLE_EQ(adjusted.mesh_m3, constant.mesh_m3);
    EXPECT_DOUBLE_EQ(adjusted.aquifer_et_m3, constant.aquifer_et_m3);
}

namespace {

/// Two identical parallel open channels (test_heat_per_element) with no heat
/// configuration of their own: everything thermal is authored through the
/// editors' C API below.
std::string channelDeck() {
    return "[OPTIONS]\nFLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\n"
           "START_DATE 07/01/2026\nSTART_TIME 00:00:00\nEND_DATE 07/01/2026\nEND_TIME 04:00:00\n"
           "ROUTING_STEP 5\nREPORT_STEP 00:05:00\n\n"
           "[TEMPERATURE]\nTIMESERIES air_ts\n\n"
           "[TIMESERIES]\nair_ts 07/01/2026 00:00 95.0\nair_ts 07/02/2026 00:00 95.0\n\n"
           "[JUNCTIONS]\nJ0 12.0 10 0.5 0 0\nJA 9.0 10 0.5 0 0\nJB 9.0 10 0.5 0 0\n\n"
           "[OUTFALLS]\nOUTA 6.0 FREE NO\nOUTB 6.0 FREE NO\n\n"
           "[CONDUITS]\nCA J0 JA 500 0.013 0 0 0\nCB J0 JB 500 0.013 0 0 0\n"
           "CAO JA OUTA 50 0.013 0 0 0\nCBO JB OUTB 50 0.013 0 0 0\n\n"
           "[XSECTIONS]\nCA RECT_OPEN 3.0 4.0 0 0\nCB RECT_OPEN 3.0 4.0 0 0\n"
           "CAO RECT_OPEN 3.0 4.0 0 0\nCBO RECT_OPEN 3.0 4.0 0 0\n\n"
           "[INFLOWS]\nJ0 FLOW \"\" FLOW 1.0 1.0 4.0\n\n[REPORT]\nINPUT NO\n";
}

/// What the heat editor writes for the channel-bed page.
int authorBed(SWMM_Engine e, bool module, double cb_velocity) {
    int rc = SWMM_OK;
    const auto ok = [&rc](int r) { if (rc == SWMM_OK) rc = r; };
    ok(swmm_options_set_ext(e, "HEAT_TRANSPORT", "YES"));
    ok(swmm_process_component_register(e, "org.hydrocouple.openswmm.heat", "c3_bed.heat"));
    ok(swmm_heat_set_source_temp(e, SWMM_HEAT_SRC_EXTERNAL_INFLOW, 22.0));
    ok(swmm_heat_set_source_temp(e, SWMM_HEAT_SRC_INITIAL_STATE, 22.0));
    ok(swmm_heat_set_module(e, SWMM_HEAT_SEDIMENT_EXCHANGE, module ? 1 : 0));
    ok(swmm_heat_set_sediment(e, SWMM_HEAT_SED_GROUND_TEMPERATURE, 6.0));
    ok(swmm_heat_set_sediment(e, SWMM_HEAT_SED_THERMAL_DIFFUSIVITY, 1.0e-5));
    ok(swmm_heat_set_sediment(e, SWMM_HEAT_SED_BED_THICKNESS, 0.15));
    ok(swmm_heat_set_sediment(e, SWMM_HEAT_SED_GROUND_DEPTH, 3.0));
    if (cb_velocity > 0.0)
        ok(swmm_heat_set_element_override(e, SWMM_HEAT_ATTR_HYPORHEIC_VELOCITY, SWMM_HEAT_SCOPE_LINK, "CB", cb_velocity));
    return rc;
}

struct ChannelTemps { double ca = -1, cb = -1; bool ok = false; };

ChannelTemps runChannels(SWMM_Engine e) {
    ChannelTemps t;
    if (swmm_engine_initialize(e) != SWMM_OK || swmm_engine_start(e, 0) != SWMM_OK) return t;
    double elapsed = 0.0;
    int status = SWMM_OK;
    while ((status = swmm_engine_step(e, &elapsed)) == SWMM_OK && elapsed > 0.0) {}
    const auto& ctx = static_cast<openswmm::SWMMEngine*>(e)->context();
    const int ca = ctx.link_names.find("CA"), cb = ctx.link_names.find("CB");
    const auto& temps = ctx.heat_state.link_temp;
    t.ok = status == SWMM_OK && ca >= 0 && cb >= 0 && static_cast<std::size_t>(std::max(ca, cb)) < temps.size();
    if (t.ok) { t.ca = temps[static_cast<std::size_t>(ca)]; t.cb = temps[static_cast<std::size_t>(cb)]; }
    swmm_engine_end(e);
    return t;
}

/// Authors through the API, writes the model, reopens the written file and
/// runs it — the GUI's configure → save → run path (SimulationRunner opens a
/// fresh engine from the saved file). Heat registered on an already-open
/// engine does not bind until the model is reopened.
ChannelTemps authoredSavedRun(const std::string& tag, bool module, double cb_velocity) {
    SWMM_Engine e = openDeck(tag + "_base", channelDeck());
    if (!e) return {};
    const fs::path saved = kOutDir / (tag + "_hydrothermal_saved.inp");
    const bool authored = authorBed(e, module, cb_velocity) == SWMM_OK &&
                          swmm_model_write(e, saved.string().c_str()) == SWMM_OK;
    closeDeck(e);
    if (!authored) return {};
    SWMM_Engine r = swmm_engine_create();
    ChannelTemps t;
    if (swmm_engine_open(r, saved.string().c_str(), (kOutDir / (tag + "_saved.rpt")).string().c_str(),
                         (kOutDir / (tag + "_saved.out")).string().c_str(), nullptr) == SWMM_OK)
        t = runChannels(r);
    closeDeck(r);
    return t;
}

}  // namespace

TEST(HydrothermalConfig, AuthoredChannelBedReachesTheSolverAfterSaveAndReopen) {
    const ChannelTemps off = authoredSavedRun("c3_bed_off", false, 0.0);
    const ChannelTemps on = authoredSavedRun("c3_bed_on", true, 0.0);
    const ChannelTemps hyporheic = authoredSavedRun("c3_bed_hyporheic", true, 3.0e-5);
    ASSERT_TRUE(off.ok && on.ok && hyporheic.ok) << "an authored model did not save, reopen or run";
    std::ofstream(kOutDir / "channel_bed.log")
        << "case, CA (C), CB (C)\nmodule off, " << off.ca << ", " << off.cb
        << "\nconduction only, " << on.ca << ", " << on.cb
        << "\nCB hyporheic 3e-5 m/s, " << hyporheic.ca << ", " << hyporheic.cb << '\n';

    // Off: no ground contact, the warm inflow stays warm and both match.
    EXPECT_DOUBLE_EQ(off.ca, off.cb);
    // On: the authored 6 C ground cools both channels, identically while
    // the hyporheic velocity is zero (conduction only).
    EXPECT_LT(on.ca, off.ca - 0.1);
    EXPECT_GT(on.ca, 6.0);
    EXPECT_DOUBLE_EQ(on.ca, on.cb);
    // The per-link velocity reaches the solver for CB alone. Its direction
    // is not asserted here: it awaits an independent bed solution (P1).
    EXPECT_DOUBLE_EQ(hyporheic.ca, on.ca);
    EXPECT_NE(hyporheic.cb, on.cb);
}

TEST(HydrothermalConfig, HeatOnWithMeshTemperatureOffRunsNetworkTemperatureOnly) {
    for (bool mesh_temperature : {true, false}) {
        Combo c;
        c.aquifer = false;
        c.mesh_temperature = mesh_temperature;
        SCOPED_TRACE(mesh_temperature ? "mesh temperature YES" : "mesh temperature NO");
        SWMM_Engine e = openDeck(std::string("c3_mesh_temperature_") + (mesh_temperature ? "yes" : "no"), deck(c));
        ASSERT_NE(e, nullptr);
        ASSERT_EQ(swmm_engine_initialize(e), SWMM_OK);
        SWMM_TransportMatrix m{};
        ASSERT_EQ(swmm_get_transport_matrix(e, &m), SWMM_OK);
        const auto& router = static_cast<openswmm::SWMMEngine*>(e)->surfaceRouter2D();
        EXPECT_EQ(router.state().transport.temp_row >= 0, mesh_temperature);
        EXPECT_EQ(m.cell[SWMM_TRANSPORT_DOMAIN_SURFACE_2D][SWMM_TRANSPORT_CLASS_TEMPERATURE].state == 0,
                  mesh_temperature)
            << m.cell[SWMM_TRANSPORT_DOMAIN_SURFACE_2D][SWMM_TRANSPORT_CLASS_TEMPERATURE].reason;
        EXPECT_EQ(m.cell[SWMM_TRANSPORT_DOMAIN_NETWORK_1D][SWMM_TRANSPORT_CLASS_TEMPERATURE].state, 0)
            << "network temperature must not follow the mesh switch";
        closeDeck(e);
    }
}

// ---------------------------------------------------------------------------
// C3 — partial 2D-aquifer coverage
// ---------------------------------------------------------------------------

namespace {

/// The two-cell pan with an aquifer row for cell 0 only (no '*' row).
std::string partialAquiferDeck(const std::string& aquifer_rows) {
    return "[OPTIONS]\nFLOW_UNITS CMS\nFLOW_ROUTING DYNWAVE\n"
           "START_DATE 01/01/2026\nSTART_TIME 00:00:00\nEND_DATE 01/01/2026\nEND_TIME 00:20:00\n"
           "REPORT_STEP 00:01:00\nWET_STEP 00:01:00\nDRY_STEP 00:01:00\nROUTING_STEP 5\n\n"
           "[RAINGAGES]\nRG1 INTENSITY 0:10 1.0 TIMESERIES RAIN\n\n"
           "[TIMESERIES]\nRAIN 0:00 20.0\nRAIN 0:10 0.0\n\n"
           "[SUBCATCHMENTS]\nS1 RG1 J1 0.01 50 100 0.5 0\n\n"
           "[SUBAREAS]\nS1 0.01 0.1 0.05 0.05 25 OUTLET\n\n"
           "[INFILTRATION]\nS1 3.0 0.5 4 7 0\n\n"
           "[JUNCTIONS]\nJ1 -2.0 4.0 0 0 0\n\n[OUTFALLS]\nO1 -2.5 FREE NO\n\n"
           "[CONDUITS]\nC1 J1 O1 30.0 0.013 0 0 0\n\n[XSECTIONS]\nC1 CIRCULAR 0.5 0 0 0 1\n\n"
           "[2D_OPTIONS]\nINTEGRATOR EXPLICIT\nLTS_TIERS 1\nMAX_TIMESTEP 5\n"
           "DRY_DEPTH 0.001\nCOUPLING_CD 0.7\nREPORT_2D NO\nRAINFALL_MODE SYSTEM\n\n"
           "[2D_VERTICES]\n0.0 0.0 0.0\n10.0 0.0 0.0\n10.0 10.0 0.0\n0.0 10.0 0.0\n\n"
           "[2D_TRIANGLES]\n0 1 2 0.03 0.0\n0 2 3 0.03 0.0\n\n"
           "[2D_INFILTRATION_DEFAULTS]\n*  CONSTANT  40.0  -  -  -  -\n\n"
           "[2D_AQUIFER_OPTIONS]\nCLOSURE CLOSED_FORM\nNODE_ENROLMENT ROWS\n\n"
           "[2D_AQUIFER]\n" + aquifer_rows + "\n"
           "[COORDINATES]\nJ1 40.0 5.0\nO1 40.0 40.0\n\n"
           "[POLYGONS]\nS1 20 0\nS1 30 0\nS1 30 10\nS1 20 10\n\n[REPORT]\nINPUT NO\n";
}

}  // namespace

TEST(HydrothermalConfig, CellsWithoutAnAquiferRowHaveNoAquifer) {
    SWMM_Engine e = openDeck("partial_aquifer", partialAquiferDeck(
        "CELL 1  36.0  1.0  0.45  0.10  2.0  HG0 0.9\n"));
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_engine_initialize(e), SWMM_OK);
    const auto& ctx = static_cast<openswmm::SWMMEngine*>(e)->context();
    const auto& gw = *ctx.twod_io.aquifer_state;
    ASSERT_EQ(gw.n_cells, 2);
    const double initial = gw.storage();
    // Only the covered cell holds water: 10 x 10 / 2 m2 with its table at
    // 0.9 m of a 1 m column.
    EXPECT_GT(gw.hg[0], 0.0);
    EXPECT_EQ(gw.hg[1], 0.0);
    EXPECT_EQ(gw.hu[1], 0.0);
    ASSERT_EQ(swmm_engine_start(e, 0), SWMM_OK);
    EXPECT_EQ(swmm_coupling_set_source(e,"uncovered-well",SWMM_COUPLING_GROUNDWATER,
        1,.001,0,nullptr,nullptr,0,0),SWMM_ERR_LIFECYCLE);
    double elapsed = 0.0;
    int status = SWMM_OK;
    while ((status = swmm_engine_step(e, &elapsed)) == SWMM_OK && elapsed > 0.0) {}
    ASSERT_EQ(status, SWMM_OK);
    const auto& mb = ctx.mass_balance_2d;
    std::ofstream(kOutDir / "partial_aquifer.log")
        << "initial aquifer storage (m3), " << initial << "\nfinal aquifer storage (m3), " << gw.storage()
        << "\ncell 0 hg/hu (m), " << gw.hg[0] << ", " << gw.hu[0]
        << "\ncell 1 hg/hu (m), " << gw.hg[1] << ", " << gw.hu[1]
        << "\nsurface infiltration leaving the mesh (m3), " << mb.infil_out
        << "\ninfiltration delivered to the mesh aquifer (m3), " << gw.led_infil_in
        << "\naquifer continuity residual (m3), " << gw.continuityResidual() << '\n';
    // The uncovered cell never gains aquifer water: no infiltration into it
    // and no lateral flow across its faces.
    EXPECT_EQ(gw.hg[1], 0.0);
    EXPECT_EQ(gw.hu[1], 0.0);
    // Its rain still infiltrates, through the surface model, and leaves.
    EXPECT_GT(mb.infil_out, 0.0);
    // The covered cell still receives its own infiltration.
    EXPECT_GT(gw.led_infil_in, 0.0); // paired mesh-to-mesh receipt, not the legacy 1D aquifer ledger
    EXPECT_LT(std::fabs(gw.continuityResidual()), 1.0e-9 * std::max(1.0, initial));
    swmm_engine_end(e);
    closeDeck(e);
}

TEST(HydrothermalConfig, AquiferRowsMatchingNoCellAreRefused) {
    SWMM_Engine e = openDeck("aquifer_no_cell", partialAquiferDeck(
        "TAG absent  36.0  1.0  0.45  0.10  2.0  HG0 0.9\n"));
    if (!e) SUCCEED() << "refused at open";
    else {
        EXPECT_NE(swmm_engine_initialize(e), SWMM_OK);
        const char* msg = swmm_get_last_error_msg(e);
        EXPECT_NE(std::string(msg ? msg : "").find("no mesh cell matches"), std::string::npos) << (msg ? msg : "");
        closeDeck(e);
    }
}
