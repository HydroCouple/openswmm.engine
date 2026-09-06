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
 * @file test_heat_sources_api.cpp
 * @brief Step 3 — the `[HEAT_SOURCES]` C API surface.
 *
 * @details H6a built `openswmm_heat.h` for the flux/radiative/solar/cloud
 *          sections; `[HEAT_SOURCES]` — the per-source inlet temperature
 *          table — was the one section with no API, and it is the one the
 *          G4g editor must round-trip.
 *
 *          **The central claim these gates defend is that the API and the
 *          DECK agree.** Two entry points into one configuration that
 *          disagree about what is legal is how a deck and a GUI come to
 *          describe different models. So the range and scope refusals are
 *          tested against the parser's own answers, not against constants
 *          this file chose.
 *
 *          The save/reopen round-trip gate **exists now** (IO3a). It could
 *          not when this file was written: nothing serialized a component
 *          config back to disk, so the file instead pinned the LOSS and said
 *          the pin must fail when serialization landed. It did, and
 *          `SourceEditsSurviveASaveAndReopen` is what replaced it.
 *
 *          Scratch fixtures use the `_hs_` prefix (collision-checked).
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_heat.h>

namespace {

void write_file(const char* path, const std::string& body) {
    std::ofstream f(path);
    ASSERT_TRUE(f.is_open()) << path;
    f << body;
}

/// Minimal deck: two junctions and an outfall, heat transport on, and a
/// `[PROCESS_COMPONENTS]` row pointing at `heat_cfg` when non-empty.
std::string deck(const char* heat_cfg) {
    std::string s =
        "[TITLE]\nheat sources API\n\n"
        "[OPTIONS]\n"
        "FLOW_UNITS           CFS\nFLOW_ROUTING         DYNWAVE\n"
        "HEAT_TRANSPORT       YES\n"
        "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
        "END_DATE             01/01/2026\nEND_TIME             00:30:00\n"
        "ROUTING_STEP         5\nREPORT_STEP          00:05:00\n\n";
    if (heat_cfg && *heat_cfg) {
        s += std::string("[PROCESS_COMPONENTS]\n"
                         "org.hydrocouple.openswmm.heat config=\"") +
             heat_cfg + "\"\n\n";
    }
    s += "[JUNCTIONS]\nJ0 10.0 10 0.5 0 0\nJ1 9.0 10 0.5 0 0\n\n"
         "[OUTFALLS]\nOUT 7.0 FREE NO\n\n"
         "[CONDUITS]\nC1 J0 J1 400 0.013 0 0 0\nC2 J1 OUT 400 0.013 0 0 0\n\n"
         "[XSECTIONS]\nC1 CIRCULAR 1.5 0 0 0\nC2 CIRCULAR 1.5 0 0 0\n\n"
         "[REPORT]\nINPUT NO\n";
    return s;
}

/// Opened engine on a deck, or nullptr with `why` set.
SWMM_Engine open_deck(const char* tag, const char* heat_cfg,
                      std::string* why) {
    const std::string inp = std::string(tag) + ".inp";
    write_file(inp.c_str(), deck(heat_cfg));
    SWMM_Engine e = swmm_engine_create();
    if (!e) { *why = "engine_create returned null"; return nullptr; }
    const std::string rpt = std::string(tag) + ".rpt";
    const std::string out = std::string(tag) + ".out";
    if (swmm_engine_open(e, inp.c_str(), rpt.c_str(), out.c_str(), nullptr)
        != SWMM_OK) {
        *why = "open failed for " + inp;
        swmm_engine_destroy(e);
        return nullptr;
    }
    return e;
}

}  // namespace

// ---------------------------------------------------------------------------
// The table is readable on a model that configures no heat at all — the MCP's
// first call on most models, and the shape that crashes if a getter assumes a
// parsed list where there is a fixed enum extent.
// ---------------------------------------------------------------------------
TEST(HeatSourcesApiTest, ReadsCleanlyWithNoHeatConfig) {
    std::string why;
    SWMM_Engine e = open_deck("_hs_none", "", &why);
    ASSERT_NE(e, nullptr) << why;

    int n = -1;
    EXPECT_EQ(swmm_heat_source_count(e, &n), SWMM_OK);
    EXPECT_EQ(n, 7) << "the source table is a fixed enum extent";

    int overrides = -1;
    EXPECT_EQ(swmm_heat_node_override_count(e, &overrides), SWMM_OK);
    EXPECT_EQ(overrides, 0);

    for (int s = 0; s < n; ++s) {
        double t = -999.0;
        int    cfgd = -1;
        EXPECT_EQ(swmm_heat_get_source_temp(e, s, &t), SWMM_OK) << s;
        EXPECT_DOUBLE_EQ(t, 20.0) << "source " << s << " should read the default";
        EXPECT_EQ(swmm_heat_get_source_configured(e, s, &cfgd), SWMM_OK);
        EXPECT_EQ(cfgd, 0) << "nothing was configured, so nothing reads as such";
    }
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// The editor's core need: read what the deck said, distinguish an explicit
// row from the default, and write a new value back into the session.
// ---------------------------------------------------------------------------
TEST(HeatSourcesApiTest, ReadsDeckRowsAndDistinguishesTheDefault) {
    write_file("_hs_rw.heat",
               "[HEAT_SOURCES]\n"
               "DWF   GLOBAL 14.5\n"
               "GW    GLOBAL  9.0\n"
               "DWF   NODE J1 11.25\n");
    std::string why;
    SWMM_Engine e = open_deck("_hs_rw", "_hs_rw.heat", &why);
    ASSERT_NE(e, nullptr) << why;

    double t = 0.0;
    int    cfgd = -1;
    EXPECT_EQ(swmm_heat_get_source_temp(e, SWMM_HEAT_SRC_DWF, &t), SWMM_OK);
    EXPECT_DOUBLE_EQ(t, 14.5);
    EXPECT_EQ(swmm_heat_get_source_configured(e, SWMM_HEAT_SRC_DWF, &cfgd),
              SWMM_OK);
    EXPECT_EQ(cfgd, 1);

    // RAINFALL had no row: same 20 °C, but NOT configured. An editor that
    // cannot tell these apart writes rows the user never asked for.
    EXPECT_EQ(swmm_heat_get_source_temp(e, SWMM_HEAT_SRC_RAINFALL, &t),
              SWMM_OK);
    EXPECT_DOUBLE_EQ(t, 20.0);
    EXPECT_EQ(swmm_heat_get_source_configured(e, SWMM_HEAT_SRC_RAINFALL,
                                              &cfgd), SWMM_OK);
    EXPECT_EQ(cfgd, 0);

    // The NODE override the deck carried.
    int n = -1;
    EXPECT_EQ(swmm_heat_node_override_count(e, &n), SWMM_OK);
    ASSERT_EQ(n, 1);
    int src = -1, node = -1;
    double ot = 0.0;
    EXPECT_EQ(swmm_heat_get_node_override(e, 0, &src, &node, &ot), SWMM_OK);
    EXPECT_EQ(src, SWMM_HEAT_SRC_DWF);
    EXPECT_DOUBLE_EQ(ot, 11.25);

    // The resolver, not a re-derivation: DWF at the overridden node takes the
    // override; DWF anywhere else takes the global.
    double eff = 0.0;
    EXPECT_EQ(swmm_heat_get_effective_source_temp(e, SWMM_HEAT_SRC_DWF, node,
                                                  &eff), SWMM_OK);
    EXPECT_DOUBLE_EQ(eff, 11.25);
    const int other = (node == 0) ? 1 : 0;
    EXPECT_EQ(swmm_heat_get_effective_source_temp(e, SWMM_HEAT_SRC_DWF, other,
                                                  &eff), SWMM_OK);
    EXPECT_DOUBLE_EQ(eff, 14.5) << "a node with no override takes the global";

    // Clearing returns the default AND unmarks it, without touching the
    // override rows — they are separate model the caller did not name.
    EXPECT_EQ(swmm_heat_clear_source_temp(e, SWMM_HEAT_SRC_DWF), SWMM_OK);
    EXPECT_EQ(swmm_heat_get_source_temp(e, SWMM_HEAT_SRC_DWF, &t), SWMM_OK);
    EXPECT_DOUBLE_EQ(t, 20.0);
    EXPECT_EQ(swmm_heat_get_source_configured(e, SWMM_HEAT_SRC_DWF, &cfgd),
              SWMM_OK);
    EXPECT_EQ(cfgd, 0);
    EXPECT_EQ(swmm_heat_node_override_count(e, &n), SWMM_OK);
    EXPECT_EQ(n, 1) << "clearing a GLOBAL must not delete NODE rows";

    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// THE central claim: the API refuses exactly what the DECK refuses, and a
// refused call does not mutate.
//
// The range literals in openswmm_heat_impl.cpp are a COPY of the parser's
// file-local kMinTemp/kMaxTemp. This gate is what stops the copy drifting:
// it drives the same out-of-range value through both doors and requires both
// to say no.
// ---------------------------------------------------------------------------
TEST(HeatSourcesApiTest, RefusesWhatTheDeckRefusesAndDoesNotMutate) {
    // Door 1: the deck. 150 °C is outside the parser's [-50, 100].
    write_file("_hs_hot.heat", "[HEAT_SOURCES]\nDWF GLOBAL 150.0\n");
    const std::string inp = "_hs_hot.inp";
    write_file(inp.c_str(), deck("_hs_hot.heat"));
    SWMM_Engine bad = swmm_engine_create();
    ASSERT_NE(bad, nullptr);
    EXPECT_NE(swmm_engine_open(bad, inp.c_str(), "_hs_hot.rpt", "_hs_hot.out",
                               nullptr), SWMM_OK)
        << "the deck accepted 150 degC; the API's range copy is now the one "
           "that is wrong, and this gate is pointing at the wrong door";
    swmm_engine_destroy(bad);

    // Door 2: the API. Same value, same answer.
    std::string why;
    SWMM_Engine e = open_deck("_hs_range", "", &why);
    ASSERT_NE(e, nullptr) << why;

    double before = 0.0;
    ASSERT_EQ(swmm_heat_get_source_temp(e, SWMM_HEAT_SRC_DWF, &before),
              SWMM_OK);

    EXPECT_EQ(swmm_heat_set_source_temp(e, SWMM_HEAT_SRC_DWF, 150.0),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_heat_set_source_temp(e, SWMM_HEAT_SRC_DWF, -80.0),
              SWMM_ERR_BADPARAM);

    double after = 0.0;
    EXPECT_EQ(swmm_heat_get_source_temp(e, SWMM_HEAT_SRC_DWF, &after),
              SWMM_OK);
    EXPECT_DOUBLE_EQ(after, before)
        << "a REFUSED write mutated anyway — an API that errors and writes is "
           "worse than one that only errors";
    int cfgd = -1;
    EXPECT_EQ(swmm_heat_get_source_configured(e, SWMM_HEAT_SRC_DWF, &cfgd),
              SWMM_OK);
    EXPECT_EQ(cfgd, 0) << "a refused write marked the source configured";

    // The boundaries themselves are legal — a range gate that refuses its own
    // endpoints is testing the wrong inequality.
    EXPECT_EQ(swmm_heat_set_source_temp(e, SWMM_HEAT_SRC_DWF, -50.0), SWMM_OK);
    EXPECT_EQ(swmm_heat_set_source_temp(e, SWMM_HEAT_SRC_DWF, 100.0), SWMM_OK);

    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// H1's scope rule, refused through the API exactly as the parser refuses it:
// NODE overrides apply to DWF and EXTERNAL_INFLOW only.
// ---------------------------------------------------------------------------
TEST(HeatSourcesApiTest, NodeOverrideScopeMatchesTheParsersRule) {
    std::string why;
    SWMM_Engine e = open_deck("_hs_scope", "", &why);
    ASSERT_NE(e, nullptr) << why;

    EXPECT_EQ(swmm_heat_set_node_override(e, SWMM_HEAT_SRC_DWF, 0, 12.0),
              SWMM_OK);
    EXPECT_EQ(swmm_heat_set_node_override(e, SWMM_HEAT_SRC_EXTERNAL_INFLOW, 0,
                                          13.0), SWMM_OK);
    // Every other source takes GLOBAL in H1.
    for (const int s : {SWMM_HEAT_SRC_RAINFALL, SWMM_HEAT_SRC_GW,
                        SWMM_HEAT_SRC_RDII, SWMM_HEAT_SRC_IFACE,
                        SWMM_HEAT_SRC_INITIAL_STATE}) {
        EXPECT_EQ(swmm_heat_set_node_override(e, s, 0, 12.0),
                  SWMM_ERR_BADPARAM)
            << "source " << s << " accepted a NODE override it must refuse";
    }
    int n = -1;
    EXPECT_EQ(swmm_heat_node_override_count(e, &n), SWMM_OK);
    EXPECT_EQ(n, 2) << "a refused override was stored anyway";

    // Bad indices are BADINDEX, not BADPARAM — the codes carry meaning.
    EXPECT_EQ(swmm_heat_set_node_override(e, 99, 0, 12.0), SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_heat_set_node_override(e, SWMM_HEAT_SRC_DWF, 9999, 12.0),
              SWMM_ERR_BADINDEX);

    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Setting the same (source, node) twice is an EDIT, not a duplicate. The
// parser refuses a duplicate row because one deck cannot mean two
// temperatures; an editor rewriting a value it just wrote must succeed. Same
// invariant — one row per pair — reached the way each caller means it.
// ---------------------------------------------------------------------------
TEST(HeatSourcesApiTest, RepeatedNodeOverrideUpdatesRatherThanDuplicates) {
    std::string why;
    SWMM_Engine e = open_deck("_hs_dup", "", &why);
    ASSERT_NE(e, nullptr) << why;

    ASSERT_EQ(swmm_heat_set_node_override(e, SWMM_HEAT_SRC_DWF, 1, 10.0),
              SWMM_OK);
    ASSERT_EQ(swmm_heat_set_node_override(e, SWMM_HEAT_SRC_DWF, 1, 17.5),
              SWMM_OK);

    int n = -1;
    EXPECT_EQ(swmm_heat_node_override_count(e, &n), SWMM_OK);
    EXPECT_EQ(n, 1) << "the second write duplicated the row instead of editing";
    double t = 0.0;
    EXPECT_EQ(swmm_heat_get_node_override(e, 0, nullptr, nullptr, &t),
              SWMM_OK);
    EXPECT_DOUBLE_EQ(t, 17.5);

    // Removal shrinks the table and re-indexes.
    EXPECT_EQ(swmm_heat_remove_node_override(e, 0), SWMM_OK);
    EXPECT_EQ(swmm_heat_node_override_count(e, &n), SWMM_OK);
    EXPECT_EQ(n, 0);
    EXPECT_EQ(swmm_heat_remove_node_override(e, 0), SWMM_ERR_BADINDEX);

    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// IO3a FLIPPED THIS GATE, exactly as its previous body predicted.
//
// It used to assert that an API edit was LOST on save — pinning the defect so
// a user would not discover it — and its failure message said: "the edit
// SURVIVED ... replace it with a real round-trip assertion rather than
// relaxing it." The component save hook landed, so that is what this is now.
//
// The old body is not kept as a commented corpse: the claim it carried is the
// negation of the claim below, and two contradictory statements of the same
// fact in one file is how the next reader ends up believing the wrong one.
// ---------------------------------------------------------------------------
TEST(HeatSourcesApiTest, SourceEditsSurviveASaveAndReopen) {
    write_file("_hs_save.heat", "[HEAT_SOURCES]\nDWF GLOBAL 14.5\n");
    std::string why;
    SWMM_Engine e = open_deck("_hs_save", "_hs_save.heat", &why);
    ASSERT_NE(e, nullptr) << why;

    // Edit BOTH row kinds: a GLOBAL that the deck already carried, and a NODE
    // override the deck did not — so the gate covers "changed" and "added",
    // which serialize through different branches of saveHeatConfig.
    ASSERT_EQ(swmm_heat_set_source_temp(e, SWMM_HEAT_SRC_DWF, 31.0), SWMM_OK);
    ASSERT_EQ(swmm_heat_set_node_override(e, SWMM_HEAT_SRC_EXTERNAL_INFLOW, 1,
                                          6.25), SWMM_OK);
    ASSERT_EQ(swmm_model_write(e, "_hs_save_out.inp"), SWMM_OK);
    swmm_engine_destroy(e);

    SWMM_Engine r = swmm_engine_create();
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(swmm_engine_open(r, "_hs_save_out.inp", "_hs_save2.rpt",
                              "_hs_save2.out", nullptr), SWMM_OK);

    double t = 0.0;
    ASSERT_EQ(swmm_heat_get_source_temp(r, SWMM_HEAT_SRC_DWF, &t), SWMM_OK);
    EXPECT_DOUBLE_EQ(t, 31.0)
        << "the GLOBAL edit was lost on save — the component save hook did "
           "not run, or it declined and the carry-alongside copy wrote the "
           "ORIGINAL file back over it";

    int n = -1;
    ASSERT_EQ(swmm_heat_node_override_count(r, &n), SWMM_OK);
    ASSERT_EQ(n, 1) << "the added NODE override did not survive";
    int src = -1, node = -1;
    double ot = 0.0;
    EXPECT_EQ(swmm_heat_get_node_override(r, 0, &src, &node, &ot), SWMM_OK);
    EXPECT_EQ(src, SWMM_HEAT_SRC_EXTERNAL_INFLOW);
    EXPECT_EQ(node, 1);
    EXPECT_DOUBLE_EQ(ot, 6.25);

    // A source the model never configured must NOT have been invented by the
    // save. This is the half a naive serializer gets wrong: writing all seven
    // rows at their defaults makes every saved model look configured.
    int cfgd = -1;
    EXPECT_EQ(swmm_heat_get_source_configured(r, SWMM_HEAT_SRC_RAINFALL,
                                              &cfgd), SWMM_OK);
    EXPECT_EQ(cfgd, 0) << "the save invented a row the model never set";

    swmm_engine_destroy(r);
}

// ---------------------------------------------------------------------------
// Writing twice gives the same file. A serializer that is not idempotent
// makes every save a diff, which destroys review and version control for
// models nobody edited.
// ---------------------------------------------------------------------------
TEST(HeatSourcesApiTest, SaveIsIdempotent) {
    write_file("_hs_idem.heat",
               "[HEAT_SOURCES]\nDWF GLOBAL 14.5\nGW GLOBAL 9.0\n\n"
               "[HEAT_FLUXES]\nRADIATIVE_EXCHANGE ON\n\n"
               "[RADIATIVE_FLUXES]\nSHORTWAVE GLOBAL 317.5\n"
               "ALBEDO GLOBAL 0.12\n\n"
               "[CLOUD_COVER]\nFRACTION GLOBAL 0.4\n");
    std::string why;
    SWMM_Engine e = open_deck("_hs_idem", "_hs_idem.heat", &why);
    ASSERT_NE(e, nullptr) << why;
    // Both saves rewrite the SAME config path (_hs_idem.heat — the writer
    // resolves the relative config= against the destination dir, which is
    // this cwd), so idempotence must be judged by capturing the content
    // BETWEEN the two saves. Slurping once after both compares the file with
    // itself and passes vacuously — the first spelling of this gate did
    // exactly that (caught in the IO3a check round).
    auto slurp = [](const char* p) {
        std::ifstream f(p, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
    };

    ASSERT_EQ(swmm_model_write(e, "_hs_idem_a.inp"), SWMM_OK);
    swmm_engine_destroy(e);
    const std::string first = slurp("_hs_idem.heat");
    ASSERT_FALSE(first.empty()) << "first save wrote nothing";

    SWMM_Engine r = swmm_engine_create();
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(swmm_engine_open(r, "_hs_idem_a.inp", "_hs_idem2.rpt",
                              "_hs_idem2.out", nullptr), SWMM_OK);
    ASSERT_EQ(swmm_model_write(r, "_hs_idem_b.inp"), SWMM_OK);
    swmm_engine_destroy(r);
    const std::string second = slurp("_hs_idem.heat");

    EXPECT_EQ(first, second)
        << "a second save produced a different config file from the first";
}

// ---------------------------------------------------------------------------
// IO3b FLIPPED THIS GATE, exactly as its previous body predicted. Under
// IO3a the renderer could not write H6a's sections, so the save DECLINED on
// such models (preserving them via the copy) and this gate asserted the
// cost: the API edit was lost. The renderer now covers every heat section,
// so both halves must hold at once — the radiative config survives because
// it is RENDERED, and the edit survives with it.
// ---------------------------------------------------------------------------
TEST(HeatSourcesApiTest, H6aSectionsRoundTripWithEditsThroughTheRenderer) {
    write_file("_hs_h6a.heat",
               "[HEAT_SOURCES]\nDWF GLOBAL 14.5\n\n"
               "[HEAT_FLUXES]\nRADIATIVE_EXCHANGE ON\n\n"
               "[RADIATIVE_FLUXES]\nSHORTWAVE GLOBAL 317.5\n");
    std::string why;
    SWMM_Engine e = open_deck("_hs_h6a", "_hs_h6a.heat", &why);
    ASSERT_NE(e, nullptr) << why;

    ASSERT_EQ(swmm_heat_set_source_temp(e, SWMM_HEAT_SRC_DWF, 31.0), SWMM_OK);
    ASSERT_EQ(swmm_model_write(e, "_hs_h6a_out.inp"), SWMM_OK);
    swmm_engine_destroy(e);

    SWMM_Engine r = swmm_engine_create();
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(swmm_engine_open(r, "_hs_h6a_out.inp", "_hs_h6a2.rpt",
                              "_hs_h6a2.out", nullptr), SWMM_OK);

    // The radiative section survived — RENDERED this time, not copied.
    double sw = 0.0;
    ASSERT_EQ(swmm_heat_get_radiative(r, SWMM_HEAT_RAD_SHORTWAVE, &sw),
              SWMM_OK);
    EXPECT_DOUBLE_EQ(sw, 317.5)
        << "[RADIATIVE_FLUXES] was lost — the renderer dropped or mangled "
           "the section it now claims to cover";

    // ...and the edit survives WITH it: the step-3 loss is retired for
    // H6a-configured models. (At IO3b's base this reads 14.5 — the decline
    // path copied the original file over the edit — which is this round's
    // fails-at-base evidence.)
    double t = 0.0;
    ASSERT_EQ(swmm_heat_get_source_temp(r, SWMM_HEAT_SRC_DWF, &t), SWMM_OK);
    EXPECT_DOUBLE_EQ(t, 31.0)
        << "the [HEAT_SOURCES] edit was lost on an H6a-configured model — "
           "the save declined (or the renderer skipped the section)";

    swmm_engine_destroy(r);
}

// ---------------------------------------------------------------------------
// IO3b falsifier-i coverage: every rendered section carries at least one
// asserted field, or "renders it" is untested. One deck configures all five
// sections (both TIMESERIES spellings included), takes an API edit, saves,
// reopens, and every value must come back exactly.
// ---------------------------------------------------------------------------
TEST(HeatSourcesApiTest, EveryHeatSectionRoundTripsFieldByField) {
    write_file("_hs_all.heat",
               "[HEAT_SOURCES]\nGW GLOBAL 9.5\n\n"
               "[HEAT_FLUXES]\nSURFACE_EXCHANGE ON\nRADIATIVE_EXCHANGE ON\n\n"
               "[RADIATIVE_FLUXES]\nSHORTWAVE GLOBAL TIMESERIES sw_ts\n"
               "ALBEDO GLOBAL 0.12\nSKY_VIEW GLOBAL 0.8\n"
               "ATM_LW_REFLECTION GLOBAL 0.05\n\n"
               "[SOLAR_RADIATION]\nLATITUDE GLOBAL 41.7\n"
               "LONGITUDE GLOBAL -111.8\nTIMEZONE GLOBAL -7\n"
               "ELEVATION GLOBAL -430\nOZONE GLOBAL 0.41\n\n"
               "[CLOUD_COVER]\nFRACTION GLOBAL TIMESERIES cloud_ts\n"
               "LW_CLOUD_K GLOBAL 0.3\n");
    // The deck needs the two named series.
    const std::string inp =
        std::string("[TITLE]\nIO3b all-sections round trip\n\n[OPTIONS]\n") +
        "FLOW_UNITS           CFS\nFLOW_ROUTING         DYNWAVE\n" +
        "HEAT_TRANSPORT       YES\n" +
        "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n" +
        "END_DATE             01/01/2026\nEND_TIME             00:30:00\n" +
        "ROUTING_STEP         5\nREPORT_STEP          00:05:00\n\n" +
        // [TIMESERIES] must precede [PROCESS_COMPONENTS]: the heat config's
        // TIMESERIES spellings resolve names during the component apply, so
        // a series declared later reads as missing (the same section-order
        // family as H7b's [INFLOWS]-before-[POLLUTANTS] finding).
        "[TIMESERIES]\nsw_ts 01/01/2026 00:00 700.0\n" +
        "sw_ts 01/02/2026 00:00 700.0\n" +
        "cloud_ts 01/01/2026 00:00 0.5\ncloud_ts 01/02/2026 00:00 0.5\n\n" +
        "[PROCESS_COMPONENTS]\n" +
        "org.hydrocouple.openswmm.heat config=\"_hs_all.heat\"\n\n" +
        "[JUNCTIONS]\nJ0 10.0 10 0.5 0 0\nJ1 9.0 10 0.5 0 0\n\n" +
        "[OUTFALLS]\nOUT 7.0 FREE NO\n\n" +
        "[CONDUITS]\nC1 J0 J1 400 0.013 0 0 0\nC2 J1 OUT 400 0.013 0 0 0\n\n" +
        "[XSECTIONS]\nC1 CIRCULAR 1.5 0 0 0\nC2 CIRCULAR 1.5 0 0 0\n\n" +
        "[REPORT]\nINPUT NO\n";
    write_file("_hs_all.inp", inp);

    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_engine_open(e, "_hs_all.inp", "_hs_all.rpt", "_hs_all.out",
                               nullptr), SWMM_OK);
    ASSERT_EQ(swmm_heat_set_source_temp(e, SWMM_HEAT_SRC_GW, 11.5), SWMM_OK);
    ASSERT_EQ(swmm_model_write(e, "_hs_all_out.inp"), SWMM_OK);
    swmm_engine_destroy(e);

    SWMM_Engine r = swmm_engine_create();
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(swmm_engine_open(r, "_hs_all_out.inp", "_hs_all2.rpt",
                               "_hs_all2.out", nullptr), SWMM_OK);

    double v = 0.0; int iv = -1;
    ASSERT_EQ(swmm_heat_get_source_temp(r, SWMM_HEAT_SRC_GW, &v), SWMM_OK);
    EXPECT_DOUBLE_EQ(v, 11.5);                       // the edit
    EXPECT_EQ(swmm_heat_get_module(r, SWMM_HEAT_SURFACE_EXCHANGE, &iv),
              SWMM_OK);
    EXPECT_EQ(iv, 1);                                // [HEAT_FLUXES]
    EXPECT_EQ(swmm_heat_get_shortwave_mode(r, &iv), SWMM_OK);
    EXPECT_EQ(iv, SWMM_HEAT_SW_TIMESERIES);          // TIMESERIES spelling
    EXPECT_EQ(swmm_heat_get_radiative(r, SWMM_HEAT_RAD_ALBEDO, &v), SWMM_OK);
    EXPECT_DOUBLE_EQ(v, 0.12);                       // [RADIATIVE_FLUXES]
    EXPECT_EQ(swmm_heat_get_radiative(r, SWMM_HEAT_RAD_SKY_VIEW, &v), SWMM_OK);
    EXPECT_DOUBLE_EQ(v, 0.8);
    EXPECT_EQ(swmm_heat_get_radiative(r, SWMM_HEAT_RAD_LW_REFLECTION, &v),
              SWMM_OK);
    EXPECT_DOUBLE_EQ(v, 0.05);
    EXPECT_EQ(swmm_heat_get_solar(r, SWMM_HEAT_SOLAR_LATITUDE, &v), SWMM_OK);
    EXPECT_DOUBLE_EQ(v, 41.7);                       // [SOLAR_RADIATION]
    EXPECT_EQ(swmm_heat_get_solar(r, SWMM_HEAT_SOLAR_LONGITUDE, &v), SWMM_OK);
    EXPECT_DOUBLE_EQ(v, -111.8);
    EXPECT_EQ(swmm_heat_get_solar(r, SWMM_HEAT_SOLAR_TIMEZONE, &v), SWMM_OK);
    EXPECT_DOUBLE_EQ(v, -7.0);
    EXPECT_EQ(swmm_heat_get_solar(r, SWMM_HEAT_SOLAR_ELEVATION, &v), SWMM_OK);
    EXPECT_DOUBLE_EQ(v, -430.0);                     // below sea level kept
    EXPECT_EQ(swmm_heat_get_solar(r, SWMM_HEAT_SOLAR_OZONE, &v), SWMM_OK);
    EXPECT_DOUBLE_EQ(v, 0.41);
    EXPECT_EQ(swmm_heat_get_solar_sited(r, &iv), SWMM_OK);
    EXPECT_EQ(iv, 1);                                // has_* flags survived
    EXPECT_EQ(swmm_heat_get_cloud_configured(r, &iv), SWMM_OK);
    EXPECT_EQ(iv, 1);                                // [CLOUD_COVER]
    EXPECT_EQ(swmm_heat_get_cloud(r, SWMM_HEAT_CLOUD_LW_CLOUD_K, &v), SWMM_OK);
    EXPECT_DOUBLE_EQ(v, 0.3);

    swmm_engine_destroy(r);

    // FILE-level leg: invented DEFAULTS are invisible through the API for
    // the radiative scalars (they carry no per-field configured state), so
    // only the written file can show a serializer that emits every default.
    // EMISS_WATER stays at its 0.97 default in this fixture: its key must
    // not appear in the config the save wrote.
    {
        std::ifstream f("_hs_all.heat", std::ios::binary);
        const std::string written((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());
        ASSERT_FALSE(written.empty());
        EXPECT_EQ(written.find("EMISS_WATER"), std::string::npos)
            << "the save emitted a default the model never set — invented "
               "configuration (lesson 196), invisible through the API and "
               "caught only here";
    }
}

// ---------------------------------------------------------------------------
// The bound-series NAME getters — G4g's recorded gap. The set half existed
// (swmm_heat_set_shortwave_timeseries / _set_cloud_timeseries) with no read
// half, so the heat editor could rebind a series but only display "(keep
// current series)" for the one already bound. Four legs: bound names come
// back exactly, unbound reads "", a rebind through the API is reflected,
// and a one-byte buffer truncates to "" without writing past its end.
// ---------------------------------------------------------------------------
TEST(HeatSourcesApiTest, BoundTimeseriesNamesAreReadable) {
    write_file("_hs_tsn.heat",
               "[HEAT_FLUXES]\nRADIATIVE_EXCHANGE ON\n\n"
               "[RADIATIVE_FLUXES]\nSHORTWAVE GLOBAL TIMESERIES sw_ts\n\n"
               "[CLOUD_COVER]\nFRACTION GLOBAL TIMESERIES cloud_ts\n");
    write_file("_hs_tsn.inp",
               "[TITLE]\nts-name getters\n\n[OPTIONS]\n"
               "FLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\nHEAT_TRANSPORT YES\n"
               "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
               "END_DATE 01/01/2026\nEND_TIME 00:30:00\n"
               "ROUTING_STEP 5\nREPORT_STEP 00:05:00\n\n"
               "[TIMESERIES]\nsw_ts 01/01/2026 00:00 700.0\n"
               "sw_ts 01/02/2026 00:00 700.0\n"
               "cloud_ts 01/01/2026 00:00 0.5\n"
               "cloud_ts 01/02/2026 00:00 0.5\n"
               "alt_ts 01/01/2026 00:00 100.0\n"
               "alt_ts 01/02/2026 00:00 100.0\n\n"
               "[PROCESS_COMPONENTS]\n"
               "org.hydrocouple.openswmm.heat config=\"_hs_tsn.heat\"\n\n"
               "[JUNCTIONS]\nJ0 10.0 10 0.5 0 0\n\n"
               "[OUTFALLS]\nOUT 7.0 FREE NO\n\n"
               "[CONDUITS]\nC1 J0 OUT 400 0.013 0 0 0\n\n"
               "[XSECTIONS]\nC1 CIRCULAR 1.5 0 0 0\n\n"
               "[REPORT]\nINPUT NO\n");

    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_engine_open(e, "_hs_tsn.inp", "_hs_tsn.rpt", "_hs_tsn.out",
                               nullptr), SWMM_OK);

    char buf[64];
    ASSERT_EQ(swmm_heat_get_shortwave_timeseries(e, buf, sizeof buf), SWMM_OK);
    EXPECT_STREQ(buf, "sw_ts") << "the deck bound sw_ts";
    ASSERT_EQ(swmm_heat_get_cloud_timeseries(e, buf, sizeof buf), SWMM_OK);
    EXPECT_STREQ(buf, "cloud_ts") << "the deck bound cloud_ts";

    // A rebind through the set half is visible through the get half.
    ASSERT_EQ(swmm_heat_set_shortwave_timeseries(e, "alt_ts"), SWMM_OK);
    ASSERT_EQ(swmm_heat_get_shortwave_timeseries(e, buf, sizeof buf), SWMM_OK);
    EXPECT_STREQ(buf, "alt_ts");

    // A one-byte buffer must come back as "" — truncated, terminated, and
    // nothing written past the end.
    char tiny[2] = {'x', 'x'};
    ASSERT_EQ(swmm_heat_get_cloud_timeseries(e, tiny, 1), SWMM_OK);
    EXPECT_EQ(tiny[0], '\0');
    EXPECT_EQ(tiny[1], 'x') << "wrote past the end of a 1-byte buffer";

    EXPECT_EQ(swmm_heat_get_shortwave_timeseries(e, nullptr, 8),
              SWMM_ERR_BADPARAM);
    swmm_engine_destroy(e);

    // Unbound reads "": a fresh BUILDING engine has bound nothing.
    SWMM_Engine b = swmm_engine_new();
    ASSERT_NE(b, nullptr);
    buf[0] = 'x';
    ASSERT_EQ(swmm_heat_get_shortwave_timeseries(b, buf, sizeof buf), SWMM_OK);
    EXPECT_STREQ(buf, "") << "no binding must read as the empty string";
    ASSERT_EQ(swmm_heat_get_cloud_timeseries(b, buf, sizeof buf), SWMM_OK);
    EXPECT_STREQ(buf, "");
    swmm_engine_destroy(b);
}
