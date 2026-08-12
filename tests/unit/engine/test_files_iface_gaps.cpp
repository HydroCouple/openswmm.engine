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
 * @file test_files_iface_gaps.cpp
 * @brief End-to-end tests for the remaining [FILES] interface slots:
 *        USE/SAVE RUNOFF, USE/SAVE RDII, and the RAINFALL warning.
 *
 * @details Verifies plans/FILES_INTERFACE_GAP_CLOSURE_PLAN_2026-07-02.md:
 *          - SAVE RUNOFF writes the binary runoff interface file from the
 *            [FILES] slot (previously only via the C API); USE RUNOFF
 *            replaces the runoff computation with the file (legacy
 *            runoff_readFromFile) and reproduces the SAVE run's wet
 *            weather volume.
 *          - SAVE RDII exports computed RDII flows (legacy "SWMM5-RDII"
 *            binary); USE RDII overrides the internal unit-hydrograph
 *            computation with the file's flows — round trip conserves the
 *            RDII inflow volume. The legacy text format is also accepted.
 *          - USE/SAVE RAINFALL is not implemented: the run proceeds with a
 *            warning instead of silently ignoring the slot.
 *
 *          Working directory is tests/unit/engine/data/ at runtime; all
 *          scratch files use unique names and are removed on teardown.
 *
 * @see Legacy: src/legacy/engine/runoff.c, rdii.c, rain.c
 * @ingroup engine_tests
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_massbalance.h>

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// Model builders
// ---------------------------------------------------------------------------

// Rained-on model: gage + subcatchment draining to J1 → O1. One hour of
// 1 in/hr rain, 5-minute wet step.
std::string runoff_model_inp() {
    return R"([OPTIONS]
FLOW_UNITS           CFS
FLOW_ROUTING         DYNWAVE
INFILTRATION         HORTON
START_DATE           01/01/2026
START_TIME           00:00:00
REPORT_START_DATE    01/01/2026
REPORT_START_TIME    00:00:00
END_DATE             01/01/2026
END_TIME             02:00:00
REPORT_STEP          00:05:00
WET_STEP             00:05:00
DRY_STEP             00:05:00
ROUTING_STEP         0:00:30

[RAINGAGES]
RG1  INTENSITY  0:05  1.0  TIMESERIES  TS1

[SUBCATCHMENTS]
;;Name  Gage  Outlet  Area  PctImperv  Width  Slope  CurbLen
S1  RG1  J1  10.0  100.0  500.0  1.0  0

[SUBAREAS]
;;Subcatch  N-Imperv  N-Perv  S-Imperv  S-Perv  PctZero  RouteTo
S1  0.012  0.1  0.05  0.05  100.0  OUTLET

[INFILTRATION]
;;Subcatch  MaxRate  MinRate  Decay  DryTime  MaxInfil
S1  3.0  0.5  4.0  7.0  0

[JUNCTIONS]
J1  100.0  10.0  0.0  0.0  0.0

[OUTFALLS]
O1  95.0  FREE

[CONDUITS]
C1  J1  O1  400.0  0.013  0  0

[XSECTIONS]
C1  CIRCULAR  1.5  0  0  0  1

[TIMESERIES]
TS1  0:00  1.0
TS1  1:00  0.0
)";
}

// RDII model: same gage/rain, no subcatchment (isolates RDII), one unit
// hydrograph group feeding J1 with 5 acres of sewershed.
std::string rdii_model_inp() {
    return R"([OPTIONS]
FLOW_UNITS           CFS
FLOW_ROUTING         DYNWAVE
START_DATE           01/01/2026
START_TIME           00:00:00
REPORT_START_DATE    01/01/2026
REPORT_START_TIME    00:00:00
END_DATE             01/01/2026
END_TIME             02:00:00
REPORT_STEP          00:05:00
WET_STEP             00:05:00
DRY_STEP             00:05:00
ROUTING_STEP         0:00:30

[RAINGAGES]
RG1  INTENSITY  0:05  1.0  TIMESERIES  TS1

[JUNCTIONS]
J1  100.0  10.0  0.0  0.0  0.0

[OUTFALLS]
O1  95.0  FREE

[CONDUITS]
C1  J1  O1  400.0  0.013  0  0

[XSECTIONS]
C1  CIRCULAR  1.5  0  0  0  1

[HYDROGRAPHS]
UH1  RG1
UH1  ALL  SHORT   0.30  1.0  2.0  0  0  0
UH1  ALL  MEDIUM  0.20  2.0  4.0  0  0  0
UH1  ALL  LONG    0.10  4.0  8.0  0  0  0

[RDII]
;;Node  UnitHydrograph  SewerArea
J1  UH1  5.0

[TIMESERIES]
TS1  0:00  1.0
TS1  1:00  0.0
)";
}

// Legacy-format TEXT RDII file: constant flow at J1, hourly step, covering
// the whole simulation.
std::string rdii_text_file(double flow_cfs) {
    std::ostringstream s;
    s << "SWMM5 RDII Interface File\n"
         "constant test rdii\n"
         "3600 - time step (sec)\n"
         "1    - number of constituents\n"
         "FLOW CFS\n"
         "1    - number of nodes\n"
         "J1\n"
         "Node  Year Mon Day Hr Min Sec Flow\n"
         "J1  2026 01 01 00 00 00 " << flow_cfs << "\n"
         "J1  2026 01 01 01 00 00 " << flow_cfs << "\n"
         "J1  2026 01 01 02 00 00 " << flow_cfs << "\n";
    return s.str();
}

void write_file(const std::string& path, const std::string& content) {
    std::ofstream f(path);
    ASSERT_TRUE(f.is_open()) << "cannot create " << path;
    f << content;
}

class FilesIfaceGapsTest : public ::testing::Test {
protected:
    SWMM_Engine engine_ = nullptr;
    std::vector<std::string> scratch_;

    void TearDown() override {
        destroy_engine();
        for (const auto& p : scratch_) {
            std::error_code ec;
            fs::remove(p, ec);
        }
    }

    void destroy_engine() {
        if (engine_) {
            swmm_engine_close(engine_);
            swmm_engine_destroy(engine_);
            engine_ = nullptr;
        }
    }

    std::string scratch(const std::string& name) {
        auto p = (fs::current_path() / name).string();
        scratch_.push_back(p);
        return p;
    }

    void open_model(const std::string& tag, const std::string& inp_text) {
        const auto inp = scratch("gaps_" + tag + ".inp");
        const auto rpt = scratch("gaps_" + tag + ".rpt");
        const auto out = scratch("gaps_" + tag + ".out");
        write_file(inp, inp_text);

        engine_ = swmm_engine_create();
        ASSERT_NE(engine_, nullptr);
        ASSERT_EQ(swmm_engine_open(engine_, inp.c_str(), rpt.c_str(),
                                   out.c_str(), nullptr), SWMM_OK)
            << "open failed: " << swmm_get_last_error_msg(engine_);
        ASSERT_EQ(swmm_engine_initialize(engine_), SWMM_OK);
    }

    void run_to_completion() {
        double elapsed = 0.0;
        for (;;) {
            int rc = swmm_engine_step(engine_, &elapsed);
            ASSERT_EQ(rc, SWMM_OK) << swmm_get_last_error_msg(engine_);
            if (elapsed <= 0.0) break;
        }
        ASSERT_EQ(swmm_engine_end(engine_), SWMM_OK);
    }

    double routing_total(int component) {
        double v = 0.0;
        EXPECT_EQ(swmm_get_routing_total(engine_, component, &v), SWMM_OK);
        return v;
    }
};

// ---------------------------------------------------------------------------
// RUNOFF: SAVE from the [FILES] slot, then USE reproduces the run.
// ---------------------------------------------------------------------------

TEST_F(FilesIfaceGapsTest, RunoffSaveThenUse_ReproducesWetWeatherVolume) {
    const auto rof = scratch("gaps_runoff.rof");

    // --- SAVE run: normal computation, file auto-written from the slot.
    open_model("rof_save", runoff_model_inp());
    ASSERT_EQ(swmm_files_set(engine_, "RUNOFF_MODE", "SAVE"), SWMM_OK);
    ASSERT_EQ(swmm_files_set(engine_, "RUNOFF_PATH", rof.c_str()), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(engine_, 1), SWMM_OK)
        << swmm_get_last_error_msg(engine_);
    run_to_completion();
    const double wet_save = routing_total(SWMM_ROUTING_WET_WEATHER);
    ASSERT_GT(wet_save, 0.0) << "SAVE run produced no runoff — bad fixture";
    destroy_engine();   // close() finalizes the header step count

    ASSERT_TRUE(fs::exists(rof));
    ASSERT_GT(fs::file_size(rof), 28u);

    // --- USE run: computation replaced by the file.
    open_model("rof_use", runoff_model_inp());
    ASSERT_EQ(swmm_files_set(engine_, "RUNOFF_MODE", "USE"), SWMM_OK);
    ASSERT_EQ(swmm_files_set(engine_, "RUNOFF_PATH", rof.c_str()), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(engine_, 1), SWMM_OK)
        << swmm_get_last_error_msg(engine_);
    run_to_completion();
    const double wet_use = routing_total(SWMM_ROUTING_WET_WEATHER);

    EXPECT_NEAR(wet_use, wet_save, 0.05 * wet_save)
        << "USE RUNOFF did not reproduce the SAVE run's wet weather volume";
}

TEST_F(FilesIfaceGapsTest, UseRunoff_MissingFileFailsStart) {
    open_model("rof_missing", runoff_model_inp());
    ASSERT_EQ(swmm_files_set(engine_, "RUNOFF_MODE", "USE"), SWMM_OK);
    ASSERT_EQ(swmm_files_set(engine_, "RUNOFF_PATH",
                             "no_such_runoff_file.rof"), SWMM_OK);
    EXPECT_NE(swmm_engine_start(engine_, 1), SWMM_OK)
        << "start() must fail when the runoff interface file cannot open";
}

// ---------------------------------------------------------------------------
// RDII: SAVE exports the computed flows; USE overrides the UH computation.
// ---------------------------------------------------------------------------

TEST_F(FilesIfaceGapsTest, RdiiSaveThenUse_ConservesRdiiVolume) {
    const auto rdf = scratch("gaps_rdii.rdf");

    // --- SAVE run: internal UH convolution computes RDII and exports it.
    open_model("rdii_save", rdii_model_inp());
    ASSERT_EQ(swmm_files_set(engine_, "RDII_MODE", "SAVE"), SWMM_OK);
    ASSERT_EQ(swmm_files_set(engine_, "RDII_PATH", rdf.c_str()), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(engine_, 1), SWMM_OK)
        << swmm_get_last_error_msg(engine_);
    run_to_completion();
    const double rdii_save = routing_total(SWMM_ROUTING_RDII);
    ASSERT_GT(rdii_save, 0.0) << "SAVE run produced no RDII — bad fixture";
    destroy_engine();

    // Legacy binary stamp.
    ASSERT_TRUE(fs::exists(rdf));
    {
        std::ifstream f(rdf, std::ios::binary);
        char stamp[10] = {};
        f.read(stamp, 10);
        EXPECT_EQ(std::string(stamp, 10), "SWMM5-RDII");
    }

    // --- USE run: UH computation bypassed; file supplies the flows.
    open_model("rdii_use", rdii_model_inp());
    ASSERT_EQ(swmm_files_set(engine_, "RDII_MODE", "USE"), SWMM_OK);
    ASSERT_EQ(swmm_files_set(engine_, "RDII_PATH", rdf.c_str()), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(engine_, 1), SWMM_OK)
        << swmm_get_last_error_msg(engine_);
    run_to_completion();
    const double rdii_use = routing_total(SWMM_ROUTING_RDII);

    EXPECT_GT(rdii_use, 0.0) << "USE RDII delivered no flow";
    // Step-aligned reads (no interpolation, legacy semantics) introduce a
    // small phase difference vs the internally interpolated computation.
    EXPECT_NEAR(rdii_use, rdii_save, 0.15 * rdii_save);
}

TEST_F(FilesIfaceGapsTest, UseRdii_TextFormatOverridesComputation) {
    const double kFlow = 1.0;   // cfs constant for the 2 h simulation
    const auto rdf = scratch("gaps_rdii_text.txt");
    write_file(rdf, rdii_text_file(kFlow));

    open_model("rdii_text", rdii_model_inp());
    ASSERT_EQ(swmm_files_set(engine_, "RDII_MODE", "USE"), SWMM_OK);
    ASSERT_EQ(swmm_files_set(engine_, "RDII_PATH", rdf.c_str()), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(engine_, 1), SWMM_OK)
        << swmm_get_last_error_msg(engine_);
    run_to_completion();

    // 1 cfs × 7200 s = 7200 ft³ — and NOT the UH-computed volume, proving
    // the file overrides the internal computation.
    const double rdii_vol = routing_total(SWMM_ROUTING_RDII);
    EXPECT_NEAR(rdii_vol, kFlow * 7200.0, 0.10 * kFlow * 7200.0);
}

TEST_F(FilesIfaceGapsTest, UseRdii_MissingFileFailsStart) {
    open_model("rdii_missing", rdii_model_inp());
    ASSERT_EQ(swmm_files_set(engine_, "RDII_MODE", "USE"), SWMM_OK);
    ASSERT_EQ(swmm_files_set(engine_, "RDII_PATH", "no_such.rdf"), SWMM_OK);
    EXPECT_NE(swmm_engine_start(engine_, 1), SWMM_OK);
}

TEST_F(FilesIfaceGapsTest, UseRdii_GarbageFileFailsStart) {
    const auto rdf = scratch("gaps_rdii_bad.rdf");
    write_file(rdf, "definitely not an rdii file\n");
    open_model("rdii_bad", rdii_model_inp());
    ASSERT_EQ(swmm_files_set(engine_, "RDII_MODE", "USE"), SWMM_OK);
    ASSERT_EQ(swmm_files_set(engine_, "RDII_PATH", rdf.c_str()), SWMM_OK);
    EXPECT_NE(swmm_engine_start(engine_, 1), SWMM_OK);
}

// ---------------------------------------------------------------------------
// RAINFALL: unsupported slot warns but does not abort the run.
// ---------------------------------------------------------------------------

TEST_F(FilesIfaceGapsTest, RainfallSlot_WarnsButRuns) {
    open_model("rain_warn", runoff_model_inp());
    ASSERT_EQ(swmm_files_set(engine_, "RAINFALL_MODE", "USE"), SWMM_OK);
    ASSERT_EQ(swmm_files_set(engine_, "RAINFALL_PATH", "collated.rff"),
              SWMM_OK);
    EXPECT_EQ(swmm_engine_start(engine_, 1), SWMM_OK)
        << "unsupported RAINFALL slot must warn, not fail: "
        << swmm_get_last_error_msg(engine_);
    run_to_completion();
    EXPECT_GT(routing_total(SWMM_ROUTING_WET_WEATHER), 0.0)
        << "run must proceed normally (gage data read directly)";
}

} // namespace
