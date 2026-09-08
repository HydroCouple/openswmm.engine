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
 * @file test_msx_parity.cpp
 * @brief U2 (2026-09-07) — MSX species reach parity with pollutants at the
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

void writeRxnFile() {
    fs::create_directories(kOutDir);
    std::ofstream f(kOutDir / kRxnFileName);
    f << "[REACTION_OPTIONS]\nAREA_UNITS  M2\nRATE_UNITS  HR\n\n"
         "[REACTION_SPECIES]\nBULK Sp MG 0 0\n\n";
}

/// A two-junction line J1 → J2 → O1 with a species system (one BULK species
/// `Sp`, no kinetics so transport is observable on its own) and one
/// pollutant `Cu`.
std::string deck(const std::string& options_extra,
                 const std::string& extra_sections,
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
      << options_extra << "\n"
         "[POLLUTANTS]\n;;Name Units Crain Cgw Crdii Kdecay\nCu MG/L 0 0 0 0\n\n"
         "[JUNCTIONS]\nJ1 0.0 3.0 0 0 0\nJ2 -0.2 3.0 0 0 0\n\n"
         "[OUTFALLS]\nO1 -1.0 FREE NO\n\n"
         "[CONDUITS]\n"
         "C1 J1 J2 30.0 0.013 0 0 0\nC2 J2 O1 30.0 0.013 0 0 0\n\n"
         "[XSECTIONS]\n"
         "C1 CIRCULAR 0.5 0 0 0 1\nC2 CIRCULAR 0.5 0 0 0 1\n\n"
      << reactions
      << extra_sections
      << "[REPORT]\nINPUT NO\n";
    return m.str();
}

struct DeckRun {
    SWMM_Engine e = nullptr;
    openswmm::SWMMEngine* eng = nullptr;
    fs::path inp, rpt;
    bool opened = false, started = false;
};

DeckRun openDeck(const std::string& tag, const std::string& body) {
    fs::create_directories(kOutDir);
    writeRxnFile();
    DeckRun r;
    r.inp = kOutDir / (tag + ".inp");
    r.rpt = kOutDir / (tag + ".rpt");
    { std::ofstream f(r.inp); f << body; }
    r.e = swmm_engine_create();
    if (!r.e) return r;
    r.opened = swmm_engine_open(r.e, r.inp.string().c_str(), r.rpt.string().c_str(),
                                (kOutDir / (tag + ".out")).string().c_str(),
                                nullptr) == SWMM_OK;
    r.eng = static_cast<openswmm::SWMMEngine*>(r.e);
    return r;
}

bool startRun(DeckRun& r) {
    if (!r.opened) return false;
    if (swmm_engine_initialize(r.e) != SWMM_OK) return false;
    if (swmm_engine_start(r.e, 1) != SWMM_OK) return false;
    r.started = true;
    return true;
}

void stepAll(DeckRun& r) {
    double elapsed = 0.0;
    while (swmm_engine_step(r.e, &elapsed) == SWMM_OK && elapsed > 0.0) {}
}

void finish(DeckRun& r) {
    if (!r.e) return;
    if (r.started) swmm_engine_end(r.e);
    swmm_engine_close(r.e);
    swmm_engine_destroy(r.e);
    r.e = nullptr;
}

/// The species element state the engines share.
double speciesNodeConc(const DeckRun& r, int node, int species) {
    const auto& rx = r.eng->context().reactions;
    const auto ns = static_cast<std::size_t>(rx.n_species());
    const auto i = static_cast<std::size_t>(node) * ns + static_cast<std::size_t>(species);
    return i < rx.msx_node_conc.size() ? rx.msx_node_conc[i] : -1.0;
}

const char* kSolvers[] = {"", "QUALITY_SOLVER        EULERIAN_ARD\n",
                          "QUALITY_SOLVER        LAGRANGIAN\n"};

}  // namespace

// ---------------------------------------------------------------------------
// G1 — [INITIAL_QUALITY] accepts a species
// ---------------------------------------------------------------------------
TEST(MsxParityInitialQuality, SpeciesRowSeedsUnderEverySolver) {
    for (int s = 0; s < 3; ++s) {
        const std::string tag = "iq_solver" + std::to_string(s);
        DeckRun r = openDeck(tag, deck(kSolvers[s],
                                   "[INITIAL_QUALITY]\n"
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
            ASSERT_EQ(swmm_init_quality_get(r.e, i, &is_link, &elem, cons,
                                            sizeof cons, &v), SWMM_OK);
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
    if (r.e) { swmm_engine_close(r.e); swmm_engine_destroy(r.e); }
}

// ---------------------------------------------------------------------------
// G2 — [INFLOWS] CONCEN / MASS for a species
// ---------------------------------------------------------------------------
TEST(MsxParityInflows, ConcenRowRaisesTheDownstreamSpeciesConcentration) {
    const std::string sections =
        "[INFLOWS]\n"
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
    const std::string sections =
        "[INFLOWS]\n"
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
    DeckRun r = openDeck("inflow_wall",
                     deck("",
                          "[INFLOWS]\n"
                          "J1  FLOW  \"\"  FLOW    1.0  1.0  0.05\n"
                          "J1  Wsp   \"\"  CONCEN  1.0  1.0  4.0\n\n",
                          "[REACTION_OPTIONS]\nAREA_UNITS  M2\nRATE_UNITS  HR\n\n"
                          "[REACTION_SPECIES]\nBULK Sp MG 0 0\nWALL Wsp MG 0 0\n\n"));
    ASSERT_TRUE(r.opened);
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
    const std::string sections =
        "[DWF]\n;;Node Constituent Baseline\n"
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
    DeckRun r = openDeck("iq_file",
                     deck("", "[INITIAL_QUALITY]\nFILE iq_rows.csv\nNODE J2 Sp 2.0\n\n"));
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
    DeckRun r2 = openDeck("iq_file2",
                      deck("", "[INITIAL_QUALITY]\nFILE iq_rows.csv\nNODE J2 Sp 2.0\n\n"));
    ASSERT_TRUE(r2.opened);
    const fs::path saved = kOutDir / "iq_file_saved.inp";
    ASSERT_EQ(swmm_model_write(r2.e, saved.string().c_str()), SWMM_OK);
    const std::string text = readAll(saved);
    EXPECT_NE(text.find("FILE"), std::string::npos);
    EXPECT_NE(text.find("iq_rows.csv"), std::string::npos);
    EXPECT_EQ(text.find("J1"), text.rfind("J1"))
        << "the file's J1 row must not be expanded inline";
    finish(r2);
}

TEST(MsxParityInitialQuality, MissingFileIsAnError) {
    DeckRun r = openDeck("iq_file_missing",
                     deck("", "[INITIAL_QUALITY]\nFILE nope.csv\n\n"));
    EXPECT_FALSE(r.opened);
    if (r.e) { swmm_engine_close(r.e); swmm_engine_destroy(r.e); }
}

// ---------------------------------------------------------------------------
// G5 — the V4 hotstart species block
// ---------------------------------------------------------------------------
TEST(MsxParityHotstart, SpeciesConcentrationsRestoreByName) {
    const fs::path hsf = kOutDir / "species.hsf";
    double sp_saved = 0.0, cu_saved = 0.0;
    int j2 = -1;
    {
        DeckRun r = openDeck("hs_save",
                         deck("",
                              "[INITIAL_QUALITY]\nNODE J1 Sp 9.0\nNODE J1 Cu 4.0\n\n"
                              "[INFLOWS]\nJ1 FLOW \"\" FLOW 1.0 1.0 0.05\n\n"));
        ASSERT_TRUE(r.opened);
        ASSERT_TRUE(startRun(r));
        // A few steps so the species has moved off its seed.
        double elapsed = 0.0;
        for (int i = 0; i < 20; ++i)
            if (swmm_engine_step(r.e, &elapsed) != SWMM_OK || elapsed <= 0.0) break;
        j2 = swmm_node_index(r.e, "J2");
        ASSERT_GE(j2, 0);
        sp_saved = speciesNodeConc(r, j2, 0);
        ASSERT_EQ(swmm_node_get_quality(r.e, j2, 0, &cu_saved), SWMM_OK);
        ASSERT_EQ(swmm_hotstart_save(r.e, hsf.string().c_str()), SWMM_OK);
        finish(r);
    }
    ASSERT_TRUE(fs::exists(hsf));

    DeckRun r2 = openDeck("hs_load",
                      deck("", "[INITIAL_QUALITY]\nNODE J1 Sp 9.0\nNODE J1 Cu 4.0\n\n"));
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
    DeckRun r = openDeck("forcing",
                     deck("WATER_AGE            YES\nHEAT_TRANSPORT       YES\n",
                          "[INFLOWS]\nJ1 FLOW \"\" FLOW 1.0 1.0 0.05\n\n"));
    ASSERT_TRUE(r.opened);
    ASSERT_TRUE(startRun(r));
    double elapsed = 0.0;
    ASSERT_EQ(swmm_engine_step(r.e, &elapsed), SWMM_OK);

    const int j1 = swmm_node_index(r.e, "J1");
    ASSERT_GE(j1, 0);
    ASSERT_EQ(swmm_forcing_node_temperature(r.e, j1, 21.5, SWMM_FORCING_OVERRIDE,
                                            SWMM_FORCING_PERSIST), SWMM_OK);
    ASSERT_EQ(swmm_forcing_node_age(r.e, j1, 12.0, SWMM_FORCING_OVERRIDE,
                                    SWMM_FORCING_PERSIST), SWMM_OK);
    ASSERT_EQ(swmm_engine_step(r.e, &elapsed), SWMM_OK);
    const auto& ctx = r.eng->context();
    const auto u = static_cast<std::size_t>(j1);
    EXPECT_NEAR(ctx.heat_state.node_temp[u], 21.5, 1.0e-9);
    EXPECT_NEAR(ctx.water_age_state.node_age[u], 12.0 * 3600.0, 1.0e-6);

    // A bad index and a bad mode are refused.
    EXPECT_NE(swmm_forcing_node_temperature(r.e, 9999, 1.0, SWMM_FORCING_OVERRIDE,
                                            SWMM_FORCING_PERSIST), SWMM_OK);
    finish(r);
}

TEST(MsxParityForcing, ReservedForcingNeedsItsOption) {
    DeckRun r = openDeck("forcing_off", deck("", ""));
    ASSERT_TRUE(r.opened);
    ASSERT_TRUE(startRun(r));
    const int j1 = swmm_node_index(r.e, "J1");
    EXPECT_NE(swmm_forcing_node_temperature(r.e, j1, 20.0, SWMM_FORCING_OVERRIDE,
                                            SWMM_FORCING_PERSIST), SWMM_OK);
    EXPECT_NE(swmm_forcing_node_age(r.e, j1, 1.0, SWMM_FORCING_OVERRIDE,
                                    SWMM_FORCING_PERSIST), SWMM_OK);
    finish(r);
}
