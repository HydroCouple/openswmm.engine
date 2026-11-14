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
 * @file      test_msx_parity.cpp
 * @brief     U2 (2026-09-07) — MSX species reach parity with pollutants at the
 *        authoring seams: `[INITIAL_QUALITY]`, `[INFLOWS]` CONCEN/MASS,
 *        `[DWF]`, the `[INITIAL_QUALITY] FILE` sidecar, the V4 hotstart
 *        species block and the reserved-species forcing API.
 *
 * Gates:
 *   - G1 `[INITIAL_QUALITY]` accepts a species name, seeds the element state
 *     under every quality solver, and round-trips through the writer (it
 *     lands in [INITIAL_QUALITY], not in the .rxn's [REACTION_QUALITY]).
 *   - G2 an `[INFLOWS]` CONCEN row for a species raises the downstream
 *     concentration; MASS divides by LperFT3 exactly as a pollutant does.
 *   - G3 a `[DWF]` row for a species rides the node's DWF flow.
 *   - G4 the `FILE` sidecar's rows load, are not re-written inline, and the
 *     FILE line survives a save.
 *   - G5 a hotstart saved mid-run restores both pollutant and species
 *     concentrations by NAME (V4 block).
 *   - G6 `swmm_forcing_node_temperature` / `_age` set and inject.
 *
 * Artefacts under tests/output/msx_parity (CLAUDE.md §4.1).
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_forcing.h>
#include <openswmm/engine/openswmm_hotstart.h>
#include <openswmm/engine/openswmm_initial_quality.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_reactions.h>

#include "core/SWMMEngine.hpp"

namespace fs = std::filesystem;

namespace {

const fs::path kOutDir = fs::path(OPENSWMM_MSX_PARITY_TEST_OUT_DIR) / "msx_parity";

/**
 * @brief Collect an entire fixture file into owned text.
 *
 * Uses ordinary text mode and its platform newline rules. The local stream closes on return; no
 * model state is touched and no borrowed buffer is returned. Default stream exception settings
 * apply; allocation exceptions may propagate.
 *
 * @param[in] p Borrowed input filename used for this synchronous open/read.
 * @return Owned string collected from the stream; empty can mean an empty file or failure to open.
 *     Partial read failures have no separate status.
 */
std::string readAll(const fs::path& p) {
    std::ifstream f(p);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/// The reactions system as an EXTERNAL component config beside the deck.
/// It is not embedded in the .inp because the writer deliberately drops
/// embedded [REACTION_*] sections on save (warn-not-refuse, engine
/// 7d43a1ff), which would make every round-trip deck reload without its
/// species. An external config is the layout D-UT8 documents anyway.
const char* kRxnFileName = "model.rxn";

/**
 * @brief Overwrite the shared inert-species reaction sidecar beneath kOutDir.
 *
 * Creates the artifact directory and truncates model.rxn with one MG bulk species Sp, M2 area units
 * and HR rate units. Does not check open/write status, parse config or create reaction kinetics;
 * filesystem exceptions propagate.
 */
void writeRxnFile() {
    fs::create_directories(kOutDir);
    std::ofstream f(kOutDir / kRxnFileName);
    f << "[REACTION_OPTIONS]\nAREA_UNITS  M2\nRATE_UNITS  HR\n\n"
         "[REACTION_SPECIES]\nBULK Sp MG 0 0\n\n";
}

/**
 * @brief Build the one-species/two-conduit MSX parity fixture.
 *
 * Raw rows are copied without escaping or validation; caller supplies valid line separators. No
 * parser or file IO is involved.
 *
 * @param[in] options_extra Raw rows appended inside OPTIONS.
 * @param[in] extra_sections Raw complete sections appended before REPORT.
 * @param[in] reactions Raw process-component/config section text; default refers to model.rxn.
 * @return Owned CMS deck with fixed geometry and simulation dates.
 */
std::string deck(const std::string& options_extra, const std::string& extra_sections,
                 const std::string& reactions =
                     "[PROCESS_COMPONENTS]\n"
                     "org.hydrocouple.openswmm.reactions  config=\"model.rxn\"\n\n") {
    std::ostringstream m;
    m << "[OPTIONS]\n"
         "FLOW_UNITS           CMS\nFLOW_ROUTING         DYNWAVE\n"
         "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
         "END_DATE             01/01/2026\nEND_TIME             00:30:00\n"
         "REPORT_STEP          00:01:00\nWET_STEP             00:01:00\n"
         "DRY_STEP             00:01:00\nROUTING_STEP         5\n"
         "ALLOW_PONDING        NO\n"
      << options_extra
      << "\n"
         "[POLLUTANTS]\n;;Name Units Crain Cgw Crdii Kdecay\nCu MG/L 0 0 0 0\n\n"
         "[JUNCTIONS]\nJ1 0.0 3.0 0 0 0\nJ2 -0.2 3.0 0 0 0\n\n"
         "[OUTFALLS]\nO1 -1.0 FREE NO\n\n"
         "[CONDUITS]\n"
         "C1 J1 J2 30.0 0.013 0 0 0\nC2 J2 O1 30.0 0.013 0 0 0\n\n"
         "[XSECTIONS]\n"
         "C1 CIRCULAR 0.5 0 0 0 1\nC2 CIRCULAR 0.5 0 0 0 1\n\n"
      << reactions << extra_sections << "[REPORT]\nINPUT NO\n";
    return m.str();
}

struct DeckRun {
    SWMM_Engine e = nullptr;
    openswmm::SWMMEngine* eng = nullptr;
    fs::path inp, rpt;
    bool opened = false, started = false;
};

/**
 * @brief Write and open a MSX parity input for later inspection.
 *
 * Creates kOutDir, overwrites the common model.rxn sidecar, truncates input, and leaves init/start
 * to caller. Stream failures are unchecked; filesystem exceptions propagate. Null e reports
 * allocation failure; every non-null e requires finish.
 *
 * @param[in] tag Artifact stem below kOutDir; matching input/report/result files may be
 *     overwritten.
 * @param[in] body Borrowed full input text, copied verbatim.
 * @return Record owning e even after open rejection; eng borrows e, and opened reports only open
 *     success.
 */
DeckRun openDeck(const std::string& tag, const std::string& body) {
    fs::create_directories(kOutDir);
    writeRxnFile();
    DeckRun r;
    r.inp = kOutDir / (tag + ".inp");
    r.rpt = kOutDir / (tag + ".rpt");
    {
        std::ofstream f(r.inp);
        f << body;
    }
    r.e = swmm_engine_create();
    if (!r.e)
        return r;
    r.opened = swmm_engine_open(r.e, r.inp.string().c_str(), r.rpt.string().c_str(),
                                (kOutDir / (tag + ".out")).string().c_str(), nullptr) == SWMM_OK;
    r.eng = static_cast<openswmm::SWMMEngine*>(r.e);
    return r;
}

/**
 * @brief Initialize and start an opened fixture with saved results.
 *
 * Does not step/end or release the handle. Failed setup can leave partial state requiring finish.
 *
 * @param[in,out] r Caller-owned record; engine is mutated and started is set only after successful
 *     start.
 * @return True on successful open/init/start; false on first rejection.
 */
bool startRun(DeckRun& r) {
    if (!r.opened)
        return false;
    if (swmm_engine_initialize(r.e) != SWMM_OK)
        return false;
    if (swmm_engine_start(r.e, 1) != SWMM_OK)
        return false;
    r.started = true;
    return true;
}

/**
 * @brief Step the started fixture until completion or the first rejected step.
 *
 * Loops while the step status is SWMM_OK and elapsed is positive. Does not assert or return
 * rejection status and has no iteration guard; callers must not infer success from return alone.
 * Does not end or release.
 *
 * @param[in,out] r Caller-owned started record whose engine is advanced in place.
 */
void stepAll(DeckRun& r) {
    double elapsed = 0.0;
    while (swmm_engine_step(r.e, &elapsed) == SWMM_OK && elapsed > 0.0) {
    }
}

/**
 * @brief Release the fixture engine after optional end.
 *
 * When started, calls end; ignores statuses, closes and destroys. eng becomes dangling while
 * opened/started remain unchanged; no artifact deletion occurs.
 *
 * @param[in,out] r Record owning e; null e is a no-op and released e is reset to null.
 */
void finish(DeckRun& r) {
    if (!r.e)
        return;
    if (r.started)
        swmm_engine_end(r.e);
    swmm_engine_close(r.e);
    swmm_engine_destroy(r.e);
    r.e = nullptr;
}

/**
 * @brief Read an MSX node concentration from the fixture flat layout.
 *
 * Uses node*n_species+species and only checks the flattened bound; invalid individual indices can
 * alias another element. Does not mutate state.
 *
 * @param[in] r Borrowed record with non-null eng and readable reaction state.
 * @param[in] node Nonnegative node row in the intended node layout.
 * @param[in] species Nonnegative species row less than n_species for meaningful indexing.
 * @return Native species concentration, or -1 if the computed flattened index is outside the
 *     vector.
 */
double speciesNodeConc(const DeckRun& r, int node, int species) {
    const auto& rx = r.eng->context().reactions;
    const auto ns = static_cast<std::size_t>(rx.n_species());
    const auto i = static_cast<std::size_t>(node) * ns + static_cast<std::size_t>(species);
    return i < rx.msx_node_conc.size() ? rx.msx_node_conc[i] : -1.0;
}

const char* kSolvers[] = {"", "QUALITY_SOLVER        EULERIAN_ARD\n",
                          "QUALITY_SOLVER        LAGRANGIAN\n"};

} // namespace

// ---------------------------------------------------------------------------
// G1 — [INITIAL_QUALITY] accepts a species
// ---------------------------------------------------------------------------
TEST(MsxParityInitialQuality, SpeciesRowSeedsUnderEverySolver) {
    for (int s = 0; s < 3; ++s) {
        const std::string tag = "iq_solver" + std::to_string(s);
        DeckRun r = openDeck(tag, deck(kSolvers[s], "[INITIAL_QUALITY]\n"
                                                    "NODE J1 Sp 7.5\n"
                                                    "NODE J1 Cu 2.5\n\n"));
        ASSERT_TRUE(r.opened) << tag;
        // The row resolved: the C API reports it with its raw name.
        ASSERT_EQ(swmm_init_quality_count(r.e), 2);
        bool found = false;
        for (int i = 0; i < 2; ++i) {
            int is_link = 0, elem = -1;
            char cons[128] = {};
            double v = 0.0;
            ASSERT_EQ(swmm_init_quality_get(r.e, i, &is_link, &elem, cons, sizeof cons, &v),
                      SWMM_OK);
            if (std::string(cons) == "Sp") {
                found = true;
                EXPECT_EQ(is_link, 0);
                EXPECT_DOUBLE_EQ(v, 7.5);
                EXPECT_EQ(swmm_init_quality_is_file(r.e, i), 0);
            }
        }
        EXPECT_TRUE(found) << tag;

        ASSERT_TRUE(startRun(r)) << tag;
        const int j1 = swmm_node_index(r.e, "J1");
        ASSERT_GE(j1, 0);
        EXPECT_NEAR(speciesNodeConc(r, j1, 0), 7.5, 1.0e-9)
            << "the species seed did not reach the element state (" << tag << ")";
        finish(r);
    }
}

TEST(MsxParityInitialQuality, SpeciesRowRoundTripsInInitialQualityNotTheRxn) {
    DeckRun r = openDeck("iq_roundtrip", deck("", "[INITIAL_QUALITY]\nNODE J1 Sp 7.5\n\n"));
    ASSERT_TRUE(r.opened);
    const fs::path saved = kOutDir / "iq_roundtrip_saved.inp";
    ASSERT_EQ(swmm_model_write(r.e, saved.string().c_str()), SWMM_OK);
    const std::string text = readAll(saved);
    EXPECT_NE(text.find("[INITIAL_QUALITY]"), std::string::npos);
    EXPECT_NE(text.find("Sp"), std::string::npos);
    finish(r);

    DeckRun r2 = openDeck("iq_roundtrip2", text);
    ASSERT_TRUE(r2.opened);
    ASSERT_EQ(swmm_init_quality_count(r2.e), 1);
    ASSERT_TRUE(startRun(r2));
    const int j1 = swmm_node_index(r2.e, "J1");
    EXPECT_NEAR(speciesNodeConc(r2, j1, 0), 7.5, 1.0e-9);
    finish(r2);
}

TEST(MsxParityInitialQuality, UnknownConstituentStillFails) {
    DeckRun r = openDeck("iq_unknown", deck("", "[INITIAL_QUALITY]\nNODE J1 Nope 1.0\n\n"));
    EXPECT_FALSE(r.opened) << "an unknown constituent must remain an error";
    if (r.e) {
        swmm_engine_close(r.e);
        swmm_engine_destroy(r.e);
    }
}

// ---------------------------------------------------------------------------
// G2 — [INFLOWS] CONCEN / MASS for a species
// ---------------------------------------------------------------------------
TEST(MsxParityInflows, ConcenRowRaisesTheDownstreamSpeciesConcentration) {
    const std::string sections = "[INFLOWS]\n"
                                 ";;Node Constituent TS Type Mfactor Sfactor Baseline\n"
                                 "J1  FLOW  \"\"  FLOW    1.0  1.0  0.05\n"
                                 "J1  Sp    \"\"  CONCEN  1.0  1.0  4.0\n"
                                 "J1  Cu    \"\"  CONCEN  1.0  1.0  4.0\n\n";
    DeckRun r = openDeck("inflow_concen", deck("", sections));
    ASSERT_TRUE(r.opened);
    ASSERT_TRUE(startRun(r));
    stepAll(r);
    const int j2 = swmm_node_index(r.e, "J2");
    ASSERT_GE(j2, 0);
    double cu = 0.0;
    ASSERT_EQ(swmm_node_get_quality(r.e, j2, 0, &cu), SWMM_OK);
    const double sp = speciesNodeConc(r, j2, 0);
    EXPECT_GT(cu, 0.1) << "the pollutant control did not arrive";
    EXPECT_GT(sp, 0.1) << "the species CONCEN row never reached the node";
    // Same row value on the same water: the two must track each other.
    EXPECT_NEAR(sp, cu, 0.25 * std::max(cu, 1.0e-6));
    finish(r);
}

TEST(MsxParityInflows, MassRowUsesTheSameLperFt3ConventionAsAPollutant) {
    const std::string sections = "[INFLOWS]\n"
                                 "J1  FLOW  \"\"  FLOW  1.0  1.0  0.05\n"
                                 "J1  Sp    \"\"  MASS  1.0  1.0  100.0\n"
                                 "J1  Cu    \"\"  MASS  1.0  1.0  100.0\n\n";
    DeckRun r = openDeck("inflow_mass", deck("", sections));
    ASSERT_TRUE(r.opened);
    ASSERT_TRUE(startRun(r));
    stepAll(r);
    const int j2 = swmm_node_index(r.e, "J2");
    double cu = 0.0;
    ASSERT_EQ(swmm_node_get_quality(r.e, j2, 0, &cu), SWMM_OK);
    const double sp = speciesNodeConc(r, j2, 0);
    EXPECT_GT(cu, 0.0);
    EXPECT_GT(sp, 0.0);
    EXPECT_NEAR(sp, cu, 0.25 * std::max(cu, 1.0e-6))
        << "MASS rows must divide by LperFT3 for a species exactly as for a "
           "pollutant";
    finish(r);
}

TEST(MsxParityInflows, WallSpeciesInflowIsRefusedWithAWarning) {
    DeckRun r =
        openDeck("inflow_wall", deck("",
                                     "[INFLOWS]\n"
                                     "J1  FLOW  \"\"  FLOW    1.0  1.0  0.05\n"
                                     "J1  Wsp   \"\"  CONCEN  1.0  1.0  4.0\n\n",
                                     "[REACTION_OPTIONS]\nAREA_UNITS  M2\nRATE_UNITS  HR\n\n"
                                     "[REACTION_SPECIES]\nBULK Sp MG 0 0\nWALL Wsp MG 0 0\n\n"));
    ASSERT_TRUE(r.opened);
    // [INFLOWS] rows are classified (and warned about) by InflowSolver::init,
    // which runs at initialize, not at open.
    ASSERT_TRUE(startRun(r));
    bool warned = false;
    for (const auto& w : r.eng->context().warnings)
        if (w.find("Wsp") != std::string::npos && w.find("WALL") != std::string::npos)
            warned = true;
    EXPECT_TRUE(warned) << "a WALL-species inflow row must say why it is ignored";
    finish(r);
}

// ---------------------------------------------------------------------------
// G3 — [DWF] for a species
// ---------------------------------------------------------------------------
TEST(MsxParityDwf, SpeciesDwfRowRidesTheNodeDwfFlow) {
    const std::string sections = "[DWF]\n;;Node Constituent Baseline\n"
                                 "J1  FLOW  0.05\n"
                                 "J1  Sp    3.0\n\n";
    DeckRun r = openDeck("dwf_species", deck("", sections));
    ASSERT_TRUE(r.opened);
    ASSERT_TRUE(startRun(r));
    stepAll(r);
    const int j2 = swmm_node_index(r.e, "J2");
    EXPECT_GT(speciesNodeConc(r, j2, 0), 0.1)
        << "the species DWF concentration never reached the node";
    finish(r);
}

// ---------------------------------------------------------------------------
// G4 — the FILE sidecar
// ---------------------------------------------------------------------------
TEST(MsxParityInitialQuality, FileSidecarLoadsAndIsReferencedNotExpanded) {
    fs::create_directories(kOutDir);
    const fs::path csv = kOutDir / "iq_rows.csv";
    {
        std::ofstream f(csv);
        f << "scope,element,constituent,value\n"
             "NODE,J1,Sp,6.0\n"
             "NODE,J2,Cu,1.5\n";
    }
    DeckRun r =
        openDeck("iq_file", deck("", "[INITIAL_QUALITY]\nFILE iq_rows.csv\nNODE J2 Sp 2.0\n\n"));
    ASSERT_TRUE(r.opened);
    EXPECT_EQ(swmm_init_quality_count(r.e), 3) << "2 file rows + 1 inline row";
    char fbuf[512] = {};
    ASSERT_EQ(swmm_init_quality_file_get(r.e, fbuf, sizeof fbuf), SWMM_OK);
    EXPECT_STREQ(fbuf, "iq_rows.csv");
    int fileRows = 0;
    for (int i = 0; i < swmm_init_quality_count(r.e); ++i)
        fileRows += swmm_init_quality_is_file(r.e, i);
    EXPECT_EQ(fileRows, 2);

    ASSERT_TRUE(startRun(r));
    const int j1 = swmm_node_index(r.e, "J1");
    EXPECT_NEAR(speciesNodeConc(r, j1, 0), 6.0, 1.0e-9) << "the file row did not seed";
    finish(r);

    // The writer emits the FILE line and the inline row only.
    DeckRun r2 =
        openDeck("iq_file2", deck("", "[INITIAL_QUALITY]\nFILE iq_rows.csv\nNODE J2 Sp 2.0\n\n"));
    ASSERT_TRUE(r2.opened);
    const fs::path saved = kOutDir / "iq_file_saved.inp";
    ASSERT_EQ(swmm_model_write(r2.e, saved.string().c_str()), SWMM_OK);
    // Scope to [INITIAL_QUALITY]: J1 is also, legitimately, in [JUNCTIONS],
    // [CONDUITS] and [COORDINATES].
    const std::string text = readAll(saved);
    const auto iq = text.find("[INITIAL_QUALITY]");
    ASSERT_NE(iq, std::string::npos);
    const std::string section = text.substr(iq, text.find("\n[", iq) - iq);
    EXPECT_NE(section.find("FILE"), std::string::npos);
    EXPECT_NE(section.find("iq_rows.csv"), std::string::npos);
    EXPECT_EQ(section.find("J1"), std::string::npos)
        << "the file's J1 row must not be expanded inline";
    finish(r2);
}

TEST(MsxParityInitialQuality, MissingFileIsAnError) {
    DeckRun r = openDeck("iq_file_missing", deck("", "[INITIAL_QUALITY]\nFILE nope.csv\n\n"));
    EXPECT_FALSE(r.opened);
    if (r.e) {
        swmm_engine_close(r.e);
        swmm_engine_destroy(r.e);
    }
}

// ---------------------------------------------------------------------------
// G5 — the V4 hotstart species block
// ---------------------------------------------------------------------------
TEST(MsxParityHotstart, SpeciesConcentrationsRestoreByName) {
    const fs::path hsf = kOutDir / "species.hsf";
    double sp_saved = 0.0, cu_saved = 0.0;
    int j2 = -1;
    {
        DeckRun r =
            openDeck("hs_save", deck("", "[INITIAL_QUALITY]\nNODE J1 Sp 9.0\nNODE J1 Cu 4.0\n\n"
                                         "[INFLOWS]\nJ1 FLOW \"\" FLOW 1.0 1.0 0.05\n\n"));
        ASSERT_TRUE(r.opened);
        ASSERT_TRUE(startRun(r));
        // A few steps so the species has moved off its seed.
        double elapsed = 0.0;
        for (int i = 0; i < 20; ++i)
            if (swmm_engine_step(r.e, &elapsed) != SWMM_OK || elapsed <= 0.0)
                break;
        j2 = swmm_node_index(r.e, "J2");
        ASSERT_GE(j2, 0);
        sp_saved = speciesNodeConc(r, j2, 0);
        ASSERT_EQ(swmm_node_get_quality(r.e, j2, 0, &cu_saved), SWMM_OK);
        ASSERT_EQ(swmm_hotstart_save(r.e, hsf.string().c_str()), SWMM_OK);
        finish(r);
    }
    ASSERT_TRUE(fs::exists(hsf));

    DeckRun r2 =
        openDeck("hs_load", deck("", "[INITIAL_QUALITY]\nNODE J1 Sp 9.0\nNODE J1 Cu 4.0\n\n"));
    ASSERT_TRUE(r2.opened);
    ASSERT_EQ(swmm_engine_initialize(r2.e), SWMM_OK);
    SWMM_HotStart hs = nullptr;
    ASSERT_EQ(swmm_hotstart_open(hsf.string().c_str(), &hs), SWMM_OK);
    ASSERT_NE(hs, nullptr);
    ASSERT_EQ(swmm_hotstart_apply(r2.e, hs), SWMM_OK);
    swmm_hotstart_close(hs);
    const int j2b = swmm_node_index(r2.e, "J2");
    ASSERT_EQ(j2b, j2);
    EXPECT_NEAR(speciesNodeConc(r2, j2b, 0), sp_saved, 1.0e-9)
        << "the V4 species block did not restore";
    double cu = 0.0;
    ASSERT_EQ(swmm_node_get_quality(r2.e, j2b, 0, &cu), SWMM_OK);
    EXPECT_NEAR(cu, cu_saved, 1.0e-9);
    swmm_engine_close(r2.e);
    swmm_engine_destroy(r2.e);
    r2.e = nullptr;
}

// ---------------------------------------------------------------------------
// G6 — reserved-species node forcing
// ---------------------------------------------------------------------------
TEST(MsxParityForcing, NodeTemperatureAndAgeForcing) {
    DeckRun r = openDeck("forcing", deck("WATER_AGE            YES\nHEAT_TRANSPORT       YES\n",
                                         "[INFLOWS]\nJ1 FLOW \"\" FLOW 1.0 1.0 0.05\n\n"));
    ASSERT_TRUE(r.opened);
    ASSERT_TRUE(startRun(r));
    double elapsed = 0.0;
    ASSERT_EQ(swmm_engine_step(r.e, &elapsed), SWMM_OK);

    const int j1 = swmm_node_index(r.e, "J1");
    ASSERT_GE(j1, 0);
    ASSERT_EQ(
        swmm_forcing_node_temperature(r.e, j1, 21.5, SWMM_FORCING_OVERRIDE, SWMM_FORCING_PERSIST),
        SWMM_OK);
    ASSERT_EQ(swmm_forcing_node_age(r.e, j1, 12.0, SWMM_FORCING_OVERRIDE, SWMM_FORCING_PERSIST),
              SWMM_OK);
    ASSERT_EQ(swmm_engine_step(r.e, &elapsed), SWMM_OK);
    const auto& ctx = r.eng->context();
    const auto u = static_cast<std::size_t>(j1);
    EXPECT_NEAR(ctx.heat_state.node_temp[u], 21.5, 1.0e-9);
    EXPECT_NEAR(ctx.water_age_state.node_age[u], 12.0 * 3600.0, 1.0e-6);

    // A bad index and a bad mode are refused.
    EXPECT_NE(
        swmm_forcing_node_temperature(r.e, 9999, 1.0, SWMM_FORCING_OVERRIDE, SWMM_FORCING_PERSIST),
        SWMM_OK);
    finish(r);
}

TEST(MsxParityForcing, ReservedForcingNeedsItsOption) {
    DeckRun r = openDeck("forcing_off", deck("", ""));
    ASSERT_TRUE(r.opened);
    ASSERT_TRUE(startRun(r));
    const int j1 = swmm_node_index(r.e, "J1");
    EXPECT_NE(
        swmm_forcing_node_temperature(r.e, j1, 20.0, SWMM_FORCING_OVERRIDE, SWMM_FORCING_PERSIST),
        SWMM_OK);
    EXPECT_NE(swmm_forcing_node_age(r.e, j1, 1.0, SWMM_FORCING_OVERRIDE, SWMM_FORCING_PERSIST),
              SWMM_OK);
    finish(r);
}

// ---------------------------------------------------------------------------
// Review round 1 (2026-10-15): the LEGACY species mirror must mix exactly as
// the pollutant path does, and residence time must exist for tank kinetics.
// ---------------------------------------------------------------------------
namespace {

/**
 * @brief Build the common three-hour CMS review OPTIONS section.
 *
 * Raw rows are copied without escaping or validation; caller supplies valid line separators. No
 * parser or file IO is involved.
 *
 * @param[in] extra Raw rows appended after ALLOW_PONDING.
 * @return Owned section text with a 30 s routing step and five-minute report/wet/dry intervals.
 */
std::string reviewOptions(const std::string& extra) {
    return "[OPTIONS]\n"
           "FLOW_UNITS           CMS\nFLOW_ROUTING         DYNWAVE\n"
           "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
           "END_DATE             01/01/2026\nEND_TIME             03:00:00\n"
           "REPORT_STEP          00:05:00\nWET_STEP             00:05:00\n"
           "DRY_STEP             00:05:00\nROUTING_STEP         30\n"
           "ALLOW_PONDING        NO\n" +
           extra + "\n";
}
const char* kReactions = "[PROCESS_COMPONENTS]\n"
                         "org.hydrocouple.openswmm.reactions  config=\"model.rxn\"\n\n";

} // namespace

// B1: a closed, evaporating storage unit. The pollutant concentrates by the
// storage unit's evaporation factor (legacy findStorageQual); a species with
// the same initial concentration and no kinetics must concentrate with it.
// At base the species mirror had no evaporation factor and stayed at 100.
TEST(MsxParityMixing, EvaporatingStorageConcentratesSpeciesLikeAPollutant) {
    const std::string body =
        reviewOptions("") +
        "[EVAPORATION]\nCONSTANT 250.0\nDRY_ONLY NO\n\n"
        "[POLLUTANTS]\n;;Name Units Crain Cgw Crdii Kdecay\nCu MG/L 0 0 0 0\n\n"
        "[STORAGE]\nST1 0.0 5.0 2.0 FUNCTIONAL 0 0 500 0 1.0\n\n"
        "[OUTFALLS]\nO1 -1.0 FREE NO\n\n"
        "[WEIRS]\nW1 ST1 O1 TRANSVERSE 4.0 1.8 NO 0 0\n\n"
        "[XSECTIONS]\nW1 RECT_OPEN 1.0 2.0 0 0\n\n" +
        std::string(kReactions) +
        "[INITIAL_QUALITY]\nNODE ST1 Cu 100.0\nNODE ST1 Sp 100.0\n\n"
        "[REPORT]\nINPUT NO\n";
    DeckRun r = openDeck("evap_storage", body);
    ASSERT_TRUE(r.opened) << readAll(r.rpt);
    ASSERT_TRUE(startRun(r));
    stepAll(r);
    const auto& ctx = r.eng->context();
    const int st = ctx.node_names.find("ST1");
    ASSERT_GE(st, 0);
    const double cu =
        ctx.nodes.conc[static_cast<std::size_t>(st) * static_cast<std::size_t>(ctx.n_pollutants())];
    const double sp = speciesNodeConc(r, st, 0);
    ASSERT_GT(cu, 100.5) << "the storage did not evaporate; deck defect";
    EXPECT_NEAR(sp, cu, 1.0e-12 * cu)
        << "species " << sp << " vs pollutant " << cu << " in the same evaporating storage";
    finish(r);
}

// B2: legacy forms a node's mixing denominator from the floored NET lateral
// flow, so a withdrawal (a negative DWF row) concentrates the remaining
// inflow. The species must see the same denominator whether or not the deck
// also declares a pollutant. At base the correction ran only when
// [POLLUTANTS] existed: 166.7 mg/L with one, 100 without.
TEST(MsxParityMixing, WithdrawalDenominatorDoesNotDependOnPollutantCount) {
    auto body = [](bool with_pollutant) {
        std::string s = reviewOptions("");
        if (with_pollutant)
            s += "[POLLUTANTS]\n;;Name Units Crain Cgw Crdii Kdecay\nCu MG/L 0 0 0 0\n\n";
        s += "[JUNCTIONS]\nJ1 0.0 3.0 0 0 0\n\n"
             "[OUTFALLS]\nO1 -1.0 FREE NO\n\n"
             "[CONDUITS]\nC1 J1 O1 120.0 0.013 0 0 0\n\n"
             "[XSECTIONS]\nC1 CIRCULAR 1.0 0 0 0 1\n\n";
        s += kReactions;
        s += "[INFLOWS]\nJ1 FLOW QIN FLOW 1.0 1.0\nJ1 Sp CIN CONCEN 1.0 1.0\n";
        if (with_pollutant)
            s += "J1 Cu CIN CONCEN 1.0 1.0\n";
        s += "\n[DWF]\nJ1 FLOW -0.4\n\n"
             "[TIMESERIES]\nQIN 0:00 1.0\nQIN 5:00 1.0\nCIN 0:00 100.0\nCIN 5:00 100.0\n\n"
             "[REPORT]\nINPUT NO\n";
        return s;
    };
    double sp[2] = {0.0, 0.0};
    for (int k = 0; k < 2; ++k) {
        DeckRun r = openDeck(k ? "withdrawal_mixed" : "withdrawal_msx_only", body(k == 1));
        ASSERT_TRUE(r.opened) << readAll(r.rpt);
        ASSERT_TRUE(startRun(r));
        stepAll(r);
        sp[k] = speciesNodeConc(r, r.eng->context().node_names.find("J1"), 0);
        finish(r);
    }
    ASSERT_GT(sp[1], 150.0) << "the mixed deck did not concentrate; deck defect";
    EXPECT_NEAR(sp[0], sp[1], 1.0e-12 * sp[1]) << "MSX-only " << sp[0] << " vs mixed " << sp[1];
}

// A6: residence time at a storage unit WITHOUT a treatment expression. Tank
// kinetics read HRT; at base only treated storage nodes ever updated it, so
// this one read 0 for the whole run under every solver.
TEST(MsxParityMixing, UntreatedStorageTracksResidenceTimeUnderEverySolver) {
    for (int s = 0; s < 3; ++s) {
        const std::string body =
            reviewOptions(kSolvers[s]) +
            "[POLLUTANTS]\n;;Name Units Crain Cgw Crdii Kdecay\nCu MG/L 0 0 0 0\n\n"
            "[JUNCTIONS]\nJ1 2.0 3.0 0 0 0\n\n"
            "[STORAGE]\nST1 0.0 5.0 1.0 FUNCTIONAL 0 0 400 0 0\n\n"
            "[OUTFALLS]\nO1 -1.0 FREE NO\n\n"
            "[CONDUITS]\nC1 J1 ST1 60.0 0.013 0 0 0\n\n"
            "[ORIFICES]\nOR1 ST1 O1 SIDE 0 0.65 NO 0\n\n"
            "[XSECTIONS]\nC1 CIRCULAR 1.0 0 0 0 1\nOR1 CIRCULAR 0.15 0 0 0\n\n" +
            std::string(kReactions) +
            "[INFLOWS]\nJ1 FLOW QIN FLOW 1.0 1.0\n\n"
            "[TIMESERIES]\nQIN 0:00 0.05\nQIN 5:00 0.05\n\n"
            "[REPORT]\nINPUT NO\n";
        DeckRun r = openDeck(std::string("hrt_untreated_") + std::to_string(s), body);
        ASSERT_TRUE(r.opened) << readAll(r.rpt);
        ASSERT_TRUE(startRun(r));
        stepAll(r);
        const auto& ctx = r.eng->context();
        const auto st = static_cast<std::size_t>(ctx.node_names.find("ST1"));
        const double v = ctx.nodes.volume[st];
        const double q = ctx.nodes.inflow[st];
        ASSERT_GT(q, 0.0);
        const double hrt = ctx.nodes.hrt[st];
        // Three hours of near-steady through-flow: the residence-time
        // recursion approaches V/Q (legacy updateHRT's fixed point).
        EXPECT_GT(hrt, 0.5 * v / q)
            << "solver " << s << ": HRT " << hrt << " s, V/Q " << v / q << " s";
        EXPECT_LT(hrt, 1.5 * v / q)
            << "solver " << s << ": HRT " << hrt << " s, V/Q " << v / q << " s";
        finish(r);
    }
}

// A2 (2026-10-15): an explicit solver on stiff kinetics overshoots below zero.
// The negative concentration must not survive (nor be silently zeroed later by
// transport): it is clamped where it is produced, with a warning. At base B
// reached −1.78 here and A+B+C ended at 194 of 100.
TEST(MsxParityReactions, StiffExplicitOvershootIsClampedAndReported) {
    {
        std::ofstream f(kOutDir / "stiff.rxn");
        f << "[REACTION_OPTIONS]\nSOLVER EUL\nCOUPLING NONE\nRATE_UNITS HR\n"
             "[REACTION_SPECIES]\nBULK A MG\nBULK B MG\nBULK C MG\n"
             "[REACTION_COEFFICIENTS]\nCONSTANT k1 1.0\nCONSTANT k2 500.0\n"
             "[REACTION_PIPES]\nRATE A -k1*A\nRATE B k1*A-k2*B\nRATE C k2*B\n"
             "[REACTION_TANKS]\nRATE A -k1*A\nRATE B k1*A-k2*B\nRATE C k2*B\n"
             "[REACTION_QUALITY]\nGLOBAL A 100\n";
    }
    const std::string body =
        reviewOptions("") +
        "[STORAGE]\nST1 0.0 5.0 2.0 FUNCTIONAL 0 0 500 0 0\n\n"
        "[OUTFALLS]\nO1 -1.0 FREE NO\n\n"
        "[WEIRS]\nW1 ST1 O1 TRANSVERSE 4.0 1.8 NO 0 0\n\n"
        "[XSECTIONS]\nW1 RECT_OPEN 1.0 2.0 0 0\n\n"
        "[PROCESS_COMPONENTS]\norg.hydrocouple.openswmm.reactions  config=\"stiff.rxn\"\n\n"
        "[REPORT]\nINPUT NO\n";
    DeckRun r = openDeck("stiff_overshoot", body);
    ASSERT_TRUE(r.opened) << readAll(r.rpt);
    ASSERT_TRUE(startRun(r));
    double elapsed = 0.0, min_b = 1e300;
    const int st = r.eng->context().node_names.find("ST1");
    while (swmm_engine_step(r.e, &elapsed) == SWMM_OK && elapsed > 0.0)
        min_b = std::min(min_b, speciesNodeConc(r, st, 1));
    EXPECT_GE(min_b, 0.0) << "a negative concentration survived the reaction step";
    bool warned = false;
    for (const auto& w : r.eng->context().warnings)
        if (w.find("negative") != std::string::npos)
            warned = true;
    EXPECT_TRUE(warned) << "the clamp was silent";
    finish(r);
}

// B14 (2026-10-15): the species routing ledger. Fed exactly like a pollutant
// (inflow, a withdrawal, conduit seepage, an evaporating storage unit) and
// with no kinetics, the species' ledger rows must equal the pollutant's.
namespace {
/**
 * @brief Build a pollutant/MSX ledger comparison with withdrawal, seepage and evaporation.
 *
 * References but does not create the reaction config. Uses reviewOptions and copies the path
 * without validation; no model is run.
 *
 * @param[in] rxn Reaction config path copied inside double quotes; caller must escape or avoid
 *     input grammar delimiters.
 * @return Owned three-hour CMS deck feeding Cu and Sp equally, with storage, an orifice and a
 *     conduit.
 */
std::string ledgerDeck(const std::string& rxn) {
    return reviewOptions("") +
           "[EVAPORATION]\nCONSTANT 50.0\nDRY_ONLY NO\n\n"
           "[POLLUTANTS]\n;;Name Units Crain Cgw Crdii Kdecay\nCu MG/L 0 0 0 0\n\n"
           "[JUNCTIONS]\nJ1 2.0 3.0 0 0 0\n\n"
           "[STORAGE]\nST1 0.0 5.0 1.0 FUNCTIONAL 0 0 400 0 1.0\n\n"
           "[OUTFALLS]\nO1 -1.0 FREE NO\n\n"
           "[CONDUITS]\nC1 J1 ST1 200.0 0.013 0 0 0\n\n"
           "[ORIFICES]\nOR1 ST1 O1 SIDE 0 0.65 NO 0\n\n"
           "[XSECTIONS]\nC1 CIRCULAR 1.0 0 0 0 1\nOR1 CIRCULAR 0.15 0 0 0\n\n"
           "[LOSSES]\nC1 0 0 0 NO 20\n\n"
           "[PROCESS_COMPONENTS]\norg.hydrocouple.openswmm.reactions  config=\"" +
           rxn +
           "\"\n\n"
           "[INFLOWS]\nJ1 FLOW QIN FLOW 1.0 1.0\nJ1 Cu CIN CONCEN 1.0 1.0\nJ1 Sp CIN CONCEN 1.0 1.0\n\n"
           "[DWF]\nJ1 FLOW -0.01\n\n"
           "[TIMESERIES]\nQIN 0:00 0.08\nQIN 1:00 0.08\nQIN 1:30 0.0\nQIN 5:00 0.0\n"
           "CIN 0:00 100.0\nCIN 5:00 100.0\n\n"
           "[REPORT]\nINPUT NO\n";
}
} // namespace

TEST(MsxParityLedger, SpeciesLedgerMatchesThePollutantLedger) {
    DeckRun r = openDeck("ledger_parity", ledgerDeck("model.rxn"));
    ASSERT_TRUE(r.opened) << readAll(r.rpt);
    ASSERT_TRUE(startRun(r));
    stepAll(r);
    const auto& ctx = r.eng->context();
    const auto& mb = ctx.mass_balance;
    const auto& led = ctx.reactions.msx_ledger;
    ASSERT_TRUE(led.armed);
    const double p_in = mb.qual_routing_dw_in[0] + mb.qual_routing_wet[0] +
                        mb.qual_routing_gw_in[0] + mb.qual_routing_ii_in[0] +
                        mb.qual_routing_ex_in[0];
    ASSERT_GT(p_in, 0.0);
    ASSERT_GT(mb.qual_routing_seep[0], 0.0) << "the conduit did not seep; deck defect";
    auto near = [](double a, double b) {
        return std::fabs(a - b) <= 1e-9 * std::max(1.0, std::fabs(b));
    };
    EXPECT_TRUE(near(led.inflow[0], p_in)) << led.inflow[0] << " vs " << p_in;
    EXPECT_TRUE(near(led.outflow[0], mb.qual_routing_outflow[0]))
        << led.outflow[0] << " vs " << mb.qual_routing_outflow[0];
    EXPECT_TRUE(near(led.flooding[0], mb.qual_routing_flood[0]));
    EXPECT_TRUE(near(led.seepage[0], mb.qual_routing_seep[0]))
        << led.seepage[0] << " vs " << mb.qual_routing_seep[0];
    EXPECT_TRUE(near(led.init[0], mb.qual_routing_init[0]));
    EXPECT_EQ(led.clamped[0], 0.0);
    EXPECT_TRUE(near(led.reacted[0], 0.0)) << "no kinetics, yet reacted " << led.reacted[0];
    // The report prints the block.
    swmm_engine_end(r.e);
    swmm_engine_report(r.e);
    r.started = false;
    finish(r);
    EXPECT_NE(readAll(r.rpt).find("Reaction Species Routing"), std::string::npos);
}

TEST(MsxParityLedger, ReactingSpeciesLedgerCloses) {
    {
        std::ofstream f(kOutDir / "decay.rxn");
        f << "[REACTION_OPTIONS]\nSOLVER RK5\nRATE_UNITS HR\n[REACTION_SPECIES]\nBULK Sp MG\n"
             "[REACTION_PIPES]\nRATE Sp -0.5*Sp\n[REACTION_TANKS]\nRATE Sp -0.5*Sp\n";
    }
    // The pollutant twin decays at the same rate (12 /day = 0.5 /hr) on the
    // pollutant path, so both ledgers carry the CSTR scheme's own error.
    std::string body = ledgerDeck("decay.rxn");
    const std::string cu = "Cu MG/L 0 0 0 0\n";
    body.replace(body.find(cu), cu.size(), "Cu MG/L 0 0 0 12\n");
    DeckRun r = openDeck("ledger_decay", body);
    ASSERT_TRUE(r.opened) << readAll(r.rpt);
    ASSERT_TRUE(startRun(r));
    stepAll(r);
    swmm_engine_end(r.e); // the pollutant's final stored mass is computed at end
    r.started = false;
    const auto& ctx = r.eng->context();
    const auto& led = ctx.reactions.msx_ledger;
    const auto& rx = ctx.reactions;
    double fin = led.final_dry[0];
    for (int j = 0; j < ctx.n_nodes(); ++j)
        fin += rx.msx_node_conc[static_cast<std::size_t>(j)] *
               ctx.nodes.volume[static_cast<std::size_t>(j)];
    for (int j = 0; j < ctx.n_links(); ++j)
        fin += rx.msx_link_conc[static_cast<std::size_t>(j)] *
               ctx.links.volume[static_cast<std::size_t>(j)];
    const double in = led.init[0] + led.inflow[0] + led.clamped[0];
    const double out = led.outflow[0] + led.flooding[0] + led.seepage[0] + led.reacted[0] + fin;
    ASSERT_GT(led.reacted[0], 0.05 * in) << "the decay removed almost nothing; deck defect";
    // The legacy CSTR scheme does not close exactly with decay (−3.5 % here,
    // legacy and v6 pollutant alike: staggered link delivery and backward-
    // Euler mixing). The species ledger must report that same scheme error,
    // not a booking error: within half a percentage point of the pollutant.
    const auto& mb = ctx.mass_balance;
    const double p_in =
        mb.qual_routing_ex_in[0] + mb.qual_routing_dw_in[0] + mb.qual_routing_init[0];
    const double p_out = mb.qual_routing_outflow[0] + mb.qual_routing_flood[0] +
                         mb.qual_routing_seep[0] + mb.qual_routing_reacted[0] +
                         mb.qual_routing_final[0];
    const double err_sp = (in - out) / in, err_p = (p_in - p_out) / p_in;
    EXPECT_NEAR(err_sp, err_p, 0.005)
        << "species error " << err_sp * 100 << " % vs pollutant " << err_p * 100 << " %";
    EXPECT_NEAR(led.reacted[0], mb.qual_routing_reacted[0], 0.01 * mb.qual_routing_reacted[0])
        << "species reacted " << led.reacted[0] << " vs pollutant " << mb.qual_routing_reacted[0];
    finish(r);
}
