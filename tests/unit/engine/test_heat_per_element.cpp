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
 * @file test_heat_per_element.cpp
 * @brief Plan PE — per-element attributes (PE1/PE2) and per-element climate
 *        through the API (PE4).
 *
 * @details Every gate fails at base, where every attribute is one number for
 *          the whole model and every flux evaluator reads it.
 *
 *          **The load-bearing gate is `PerLinkClimateDoesNotReachSnowmelt`.**
 *          The whole safety argument for PE4 — per-element climate through
 *          the API but never in a deck — rests on the claim that a per-link
 *          push cannot reach hydrology. Nothing else asserts it, and the
 *          falsifier that would break it (writing the value into
 *          `climate_state` instead of resolving at the flux call) is the
 *          "simplification" a future reader is most likely to reach for.
 *
 *          Scratch fixtures use the `_pe_` prefix (collision-checked).
 */

#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_forcing.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_subcatchments.h>

#include "core/SWMMEngine.hpp"
#include "transport/components/HeatFluxModules/HeatOverrides.hpp"

namespace {

openswmm::SWMMEngine& as_cpp(SWMM_Engine e) {
    return *static_cast<openswmm::SWMMEngine*>(e);
}

void write_file(const std::string& p, const std::string& body) {
    std::ofstream f(p);
    ASSERT_TRUE(f.is_open()) << p;
    f << body;
}

/// Two PARALLEL open channels from one source node to one outfall, so the
/// pair differ in nothing the hydraulics can see. Any temperature difference
/// between them is attributable to the attribute under test and to nothing
/// else — the control the shade gate needs.
///
/// `tags` lands as a [TAGS] block; `heat_cfg` names the component file.
std::string deck(const std::string& heat_cfg, const std::string& tags,
                 const std::string& extra_opts = "") {
    return "[TITLE]\nper-element heat\n\n[OPTIONS]\n"
           "FLOW_UNITS           CFS\nFLOW_ROUTING         DYNWAVE\n"
           "HEAT_TRANSPORT       YES\n" + extra_opts +
           "START_DATE           07/01/2026\nSTART_TIME           00:00:00\n"
           "END_DATE             07/01/2026\nEND_TIME             04:00:00\n"
           "ROUTING_STEP         5\nREPORT_STEP          00:05:00\n\n"
           "[TEMPERATURE]\nTIMESERIES air_ts\nHUMIDITY 40\n"
           "WINDSPEED MONTHLY 15 15 15 15 15 15 15 15 15 15 15 15\n\n"
           "[TIMESERIES]\nair_ts 07/01/2026 00:00 95.0\n"
           "air_ts 07/02/2026 00:00 95.0\n\n"
           "[PROCESS_COMPONENTS]\n"
           "org.hydrocouple.openswmm.heat config=\"" + heat_cfg + "\"\n\n"
           "[JUNCTIONS]\nJ0 12.0 10 0.5 0 0\nJA 9.0 10 0.5 0 0\n"
           "JB 9.0 10 0.5 0 0\n\n"
           "[OUTFALLS]\nOUT 6.0 FREE NO\n\n"
           "[CONDUITS]\n"
           "CA J0 JA 500 0.013 0 0 0\nCB J0 JB 500 0.013 0 0 0\n"
           "CAO JA OUT 50 0.013 0 0 0\nCBO JB OUT 50 0.013 0 0 0\n\n"
           "[XSECTIONS]\n"
           "CA RECT_OPEN 3.0 4.0 0 0\nCB RECT_OPEN 3.0 4.0 0 0\n"
           "CAO RECT_OPEN 3.0 4.0 0 0\nCBO RECT_OPEN 3.0 4.0 0 0\n\n"
           "[INFLOWS]\nJ0 FLOW \"\" FLOW 1.0 1.0 4.0\n\n" +
           // [TAGS] goes LAST, where the InpWriter and the EPA GUI both put
           // it: handle_tags resolves names as it parses and does not join
           // the deferred replay, so a tag written above [CONDUITS] is
           // silently dropped — and under D-PE5 that silent drop becomes a
           // fatal "TAG matches nothing" at open. Pre-existing order
           // sensitivity, recorded in the PE check record.
           tags +
           "[REPORT]\nINPUT NO\n";
}

SWMM_Engine run(const std::string& tag, const std::string& body) {
    write_file(tag + ".inp", body);
    SWMM_Engine e = swmm_engine_create();
    if (!e) return nullptr;
    if (swmm_engine_open(e, (tag + ".inp").c_str(), (tag + ".rpt").c_str(),
                         (tag + ".out").c_str(), nullptr) != SWMM_OK ||
        swmm_engine_initialize(e) != SWMM_OK ||
        swmm_engine_start(e, 1) != SWMM_OK) {
        swmm_engine_destroy(e);
        return nullptr;
    }
    double el = 0.0;
    int guard = 0;
    do {
        if (swmm_engine_step(e, &el) != SWMM_OK) {
            swmm_engine_destroy(e);
            return nullptr;
        }
    } while (el > 0.0 && ++guard < 40000);
    swmm_engine_end(e);
    return e;
}

/// Open only — for gates that assert a PARSE outcome rather than a result.
int open_only(const std::string& tag, const std::string& body,
              std::string* first_error) {
    write_file(tag + ".inp", body);
    SWMM_Engine e = swmm_engine_create();
    if (!e) return SWMM_ERR_NOMEM;
    const int rc = swmm_engine_open(e, (tag + ".inp").c_str(),
                                    (tag + ".rpt").c_str(),
                                    (tag + ".out").c_str(), nullptr);
    if (rc != SWMM_OK && first_error) {
        const auto& ctx = as_cpp(e).context();
        if (!ctx.errors.empty()) *first_error = ctx.errors.front();
    }
    swmm_engine_destroy(e);
    return rc;
}

int link_of(const openswmm::SimulationContext& ctx, const char* id) {
    return ctx.link_names.find(id);
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. THE headline. Two identical parallel channels; one fully shaded, one
//    not. Under a 95 °F sun they must reach DIFFERENT temperatures, and the
//    shaded one must be cooler.
//
//    At base every conduit shares one SHADE_FACTOR, so the two channels are
//    identical to the bit — which is exactly what the first assertion below
//    quotes when it fails.
// ---------------------------------------------------------------------------
TEST(HeatPerElementTest, ShadeFactorVariesPerLink) {
    write_file("_pe_shade.heat",
               "[HEAT_FLUXES]\nSURFACE_EXCHANGE ON\nRADIATIVE_EXCHANGE ON\n\n"
               "[RADIATIVE_FLUXES]\n"
               "SHORTWAVE GLOBAL 800.0\n"
               "SHADE_FACTOR GLOBAL 0.0\n"
               "SHADE_FACTOR LINK CB 0.95\n\n"
               "[HEAT_SOURCES]\n"
               "EXTERNAL_INFLOW GLOBAL 12.0\nINITIAL_STATE GLOBAL 12.0\n");

    SWMM_Engine e = run("_pe_shade", deck("_pe_shade.heat", ""));
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp(e).context();
    const int ca = link_of(ctx, "CA"), cb = link_of(ctx, "CB");
    ASSERT_GE(ca, 0);
    ASSERT_GE(cb, 0);

    const double t_sun = ctx.heat_state.link_temp[static_cast<std::size_t>(ca)];
    const double t_shd = ctx.heat_state.link_temp[static_cast<std::size_t>(cb)];

    // Premise: the sunlit channel must actually have warmed, or the deck is
    // not discriminating and everything below would pass vacuously.
    ASSERT_GT(t_sun, 12.5) << "the sunlit control never warmed — deck defect";
    EXPECT_LT(t_shd, t_sun)
        << "shaded and sunlit channels reached the same temperature: the "
           "per-link SHADE_FACTOR did not reach the physics";
    EXPECT_GT(t_sun - t_shd, 0.25)
        << "the difference is present but implausibly small for 95% shade";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// 2. PRECEDENCE — D-PE3's entire content, in one deck. GLOBAL, TAG and LINK
//    all set SHADE_FACTOR; three elements must take three different values.
//
//    FALSIFIER: invert the resolver's two passes and this gate alone fails.
// ---------------------------------------------------------------------------
TEST(HeatPerElementTest, ElementBeatsTagBeatsGlobal) {
    write_file("_pe_prec.heat",
               "[HEAT_FLUXES]\nRADIATIVE_EXCHANGE ON\n\n"
               "[RADIATIVE_FLUXES]\n"
               "SHORTWAVE GLOBAL 800.0\n"
               "SHADE_FACTOR GLOBAL 0.10\n"
               "SHADE_FACTOR TAG  RIPARIAN 0.50\n"
               "SHADE_FACTOR LINK CB       0.90\n");
    // CA and CB are both tagged; only CB is also named at LINK scope. CAO is
    // untagged and unnamed, so it must fall all the way through to GLOBAL.
    const std::string tags =
        "[TAGS]\nLink CA RIPARIAN\nLink CB RIPARIAN\n\n";

    SWMM_Engine e = run("_pe_prec", deck("_pe_prec.heat", tags));
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp(e).context();
    namespace th = openswmm::transport::heat;
    using openswmm::HeatElement;

    const double g = th::radiativeFor(
        ctx, HeatElement::link(link_of(ctx, "CAO"))).shade_factor;
    const double t = th::radiativeFor(
        ctx, HeatElement::link(link_of(ctx, "CA"))).shade_factor;
    const double l = th::radiativeFor(
        ctx, HeatElement::link(link_of(ctx, "CB"))).shade_factor;

    EXPECT_DOUBLE_EQ(g, 0.10) << "untagged, unnamed link did not take GLOBAL";
    EXPECT_DOUBLE_EQ(t, 0.50) << "tagged link did not take TAG";
    EXPECT_DOUBLE_EQ(l, 0.90) << "named link did not take LINK over its TAG";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// 3. Bed attributes per TAG — the user's own examples. A shallow-buried
//    "lateral" and a deep "trunk", identical water, must reach different
//    temperatures through the ground conductance alone.
// ---------------------------------------------------------------------------
TEST(HeatPerElementTest, GroundDepthAndHyporheicVaryPerTag) {
    write_file("_pe_bed.heat",
               "[HEAT_FLUXES]\nSEDIMENT_EXCHANGE ON\n\n"
               "[SEDIMENT_EXCHANGE]\n"
               "GROUND_TEMPERATURE GLOBAL 6.0\n"
               "THERMAL_DIFFUSIVITY GLOBAL 1.0e-5\n"
               "BED_THICKNESS GLOBAL 0.15\n"
               "GROUND_DEPTH GLOBAL 3.0\n"
               "GROUND_DEPTH TAG LATERAL 0.15\n"
               "HYPORHEIC_VELOCITY TAG UNLINED 3.0e-5\n\n"
               "[HEAT_SOURCES]\n"
               "EXTERNAL_INFLOW GLOBAL 22.0\nINITIAL_STATE GLOBAL 22.0\n");
    // A link carries ONE tag, so CB cannot be both LATERAL and UNLINED; the
    // UNLINED row lands on the short outlet conduit CAO instead — a real
    // target for the tag (D-PE5 makes an unmatched tag fatal) that leaves
    // the CA-vs-CB ground-depth comparison untouched.
    const std::string tags =
        "[TAGS]\nLink CB LATERAL\nLink CAO UNLINED\n\n";

    SWMM_Engine e = run("_pe_bed", deck("_pe_bed.heat", tags));
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp(e).context();
    const int ca = link_of(ctx, "CA"), cb = link_of(ctx, "CB");
    const double t_deep = ctx.heat_state.link_temp[static_cast<std::size_t>(ca)];
    const double t_shal = ctx.heat_state.link_temp[static_cast<std::size_t>(cb)];

    ASSERT_LT(t_deep, 21.9) << "neither channel cooled — the bed never ran";
    EXPECT_LT(t_shal, t_deep)
        << "the shallow-buried link did not cool faster: the per-tag "
           "GROUND_DEPTH did not reach bedCouplingFromContact";
    EXPECT_GT(t_shal, 5.9) << "cooled past the ground temperature";

    // And the per-element config itself, so a failure above can be localised
    // to the physics rather than the resolution.
    namespace th = openswmm::transport::heat;
    using openswmm::HeatElement;
    EXPECT_DOUBLE_EQ(th::sedimentFor(ctx, HeatElement::link(cb)).ground_depth,
                     0.15);
    EXPECT_DOUBLE_EQ(th::sedimentFor(ctx, HeatElement::link(ca)).ground_depth,
                     3.0);
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// 4. Refusals (D-PE4, D-PE5) — each names the row.
// ---------------------------------------------------------------------------
TEST(HeatPerElementTest, MalformedOverridesAreRefusedNotIgnored) {
    struct Case {
        const char* tag;
        const char* cfg;
        const char* needle;
    } cases[] = {
        {"_pe_dup",
         "[RADIATIVE_FLUXES]\nSHADE_FACTOR LINK CA 0.2\n"
         "SHADE_FACTOR LINK CA 0.8\n",
         "duplicate"},
        {"_pe_unk",
         "[RADIATIVE_FLUXES]\nSHADE_FACTOR LINK C99 0.5\n",
         "unknown link"},
        {"_pe_tag0",
         "[RADIATIVE_FLUXES]\nSHADE_FACTOR TAG NOBODY 0.5\n",
         "matches no link"},
        {"_pe_range",
         "[RADIATIVE_FLUXES]\nSHADE_FACTOR LINK CA 7.5\n",
         "fraction in [0,1]"},
        {"_pe_nodebed",
         "[SEDIMENT_EXCHANGE]\nGROUND_DEPTH NODE JA 1.0\n",
         "CONDUITS only"},
        {"_pe_swscope",
         "[RADIATIVE_FLUXES]\nSHORTWAVE LINK CA 500.0\n",
         "GLOBAL only"},
    };

    for (const auto& c : cases) {
        write_file(std::string(c.tag) + ".heat", c.cfg);
        std::string err;
        const int rc = open_only(
            c.tag, deck(std::string(c.tag) + ".heat", ""), &err);
        EXPECT_NE(rc, SWMM_OK) << c.tag << ": accepted a row it must refuse";
        EXPECT_NE(err.find(c.needle), std::string::npos)
            << c.tag << ": refused, but the message does not say why. Got: "
            << err;
    }
}

// ---------------------------------------------------------------------------
// 5. Save/reopen round-trip. Per-element rows must survive, at every scope.
//    IO3a lost a whole section this way once (lesson 201).
// ---------------------------------------------------------------------------
TEST(HeatPerElementTest, OverridesSurviveSaveAndReopen) {
    write_file("_pe_rt.heat",
               "[HEAT_FLUXES]\nRADIATIVE_EXCHANGE ON\nSEDIMENT_EXCHANGE ON\n\n"
               "[RADIATIVE_FLUXES]\n"
               "SHADE_FACTOR GLOBAL 0.10\n"
               "SHADE_FACTOR TAG RIPARIAN 0.55\n"
               "SKY_VIEW LINK CB 0.20\n"
               "LANDCOVER_TEMPERATURE TAG RIPARIAN 27.5\n\n"
               "[SEDIMENT_EXCHANGE]\n"
               "GROUND_TEMPERATURE GLOBAL 9.0\n"
               "GROUND_DEPTH LINK CA 1.25\n");
    const std::string tags = "[TAGS]\nLink CA RIPARIAN\nLink CB RIPARIAN\n\n";

    write_file("_pe_rt.inp", deck("_pe_rt.heat", tags));
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_engine_open(e, "_pe_rt.inp", "_pe_rt.rpt", "_pe_rt.out",
                               nullptr), SWMM_OK);
    ASSERT_EQ(swmm_model_write(e, "_pe_rt_gen2.inp"), SWMM_OK);
    swmm_engine_destroy(e);

    SWMM_Engine e2 = swmm_engine_create();
    ASSERT_NE(e2, nullptr);
    ASSERT_EQ(swmm_engine_open(e2, "_pe_rt_gen2.inp", "_pe_rt2.rpt",
                               "_pe_rt2.out", nullptr), SWMM_OK)
        << "the saved model does not reopen — a row was rendered wrong";

    const auto& ctx = as_cpp(e2).context();
    namespace th = openswmm::transport::heat;
    using openswmm::HeatElement;
    const int ca = link_of(ctx, "CA"), cb = link_of(ctx, "CB");
    EXPECT_DOUBLE_EQ(th::radiativeFor(ctx, HeatElement::link(ca)).shade_factor,
                     0.55) << "TAG row lost on save";
    EXPECT_DOUBLE_EQ(th::radiativeFor(ctx, HeatElement::link(cb)).sky_view,
                     0.20) << "LINK row lost on save";
    EXPECT_DOUBLE_EQ(th::sedimentFor(ctx, HeatElement::link(ca)).ground_depth,
                     1.25) << "bed LINK row lost on save";
    EXPECT_DOUBLE_EQ(
        th::radiativeFor(ctx, HeatElement::link(ca)).landcover_temp, 27.5);

    ASSERT_EQ(swmm_model_write(e2, "_pe_rt_gen3.inp"), SWMM_OK);
    swmm_engine_destroy(e2);

    // gen2 == gen3 byte-equality, the `3e87868e` idempotence contract. A
    // renderer that sorted rows differently on the two passes would pass
    // every assertion above and fail here.
    std::ifstream a("_pe_rt_gen2.inp", std::ios::binary);
    std::ifstream b("_pe_rt_gen3.inp", std::ios::binary);
    const std::string sa((std::istreambuf_iterator<char>(a)), {});
    const std::string sb((std::istreambuf_iterator<char>(b)), {});
    EXPECT_EQ(sa, sb) << "the writer is not idempotent over per-element rows";
}

// ---------------------------------------------------------------------------
// 6. LANDCOVER_TEMPERATURE's sentinel. Unset must mean "use air", which is
//    what the pre-PE engine did unconditionally — so an unset model is
//    unchanged, and a set one is not.
// ---------------------------------------------------------------------------
TEST(HeatPerElementTest, LandcoverTemperatureSentinelMeansAir) {
    write_file("_pe_lc_off.heat",
               "[HEAT_FLUXES]\nRADIATIVE_EXCHANGE ON\n\n"
               "[RADIATIVE_FLUXES]\nSHORTWAVE GLOBAL 0.0\nSKY_VIEW GLOBAL 0.3\n\n"
               "[HEAT_SOURCES]\nEXTERNAL_INFLOW GLOBAL 15.0\n"
               "INITIAL_STATE GLOBAL 15.0\n");
    write_file("_pe_lc_on.heat",
               "[HEAT_FLUXES]\nRADIATIVE_EXCHANGE ON\n\n"
               "[RADIATIVE_FLUXES]\nSHORTWAVE GLOBAL 0.0\nSKY_VIEW GLOBAL 0.3\n"
               "LANDCOVER_TEMPERATURE GLOBAL 55.0\n\n"
               "[HEAT_SOURCES]\nEXTERNAL_INFLOW GLOBAL 15.0\n"
               "INITIAL_STATE GLOBAL 15.0\n");

    SWMM_Engine off = run("_pe_lc_off", deck("_pe_lc_off.heat", ""));
    SWMM_Engine on  = run("_pe_lc_on", deck("_pe_lc_on.heat", ""));
    ASSERT_NE(off, nullptr);
    ASSERT_NE(on, nullptr);
    const auto& co = as_cpp(off).context();
    const auto& cn = as_cpp(on).context();
    const int ca = link_of(co, "CA");

    // The sentinel is NaN and must never reach a report.
    EXPECT_TRUE(std::isnan(co.heat_config.radiative.landcover_temp));
    const double t_air_lc = co.heat_state.link_temp[static_cast<std::size_t>(ca)];
    const double t_hot_lc = cn.heat_state.link_temp[static_cast<std::size_t>(ca)];
    EXPECT_TRUE(std::isfinite(t_air_lc));
    // 55 °C land cover radiates far more than 35 °C air, so the water must
    // be warmer with the key set. Equality means the field is parsed and
    // never read.
    EXPECT_GT(t_hot_lc, t_air_lc + 0.05)
        << "LANDCOVER_TEMPERATURE parsed but not used by the longwave term";
    swmm_engine_destroy(off);
    swmm_engine_destroy(on);
}

// ---------------------------------------------------------------------------
// 7. PE4 — per-element climate through the API. `ADD -12 °F` on ONE link;
//    the other must be untouched and match a no-forcing control EXACTLY.
// ---------------------------------------------------------------------------
TEST(HeatPerElementTest, ApiPerElementClimateForcing) {
    write_file("_pe_api.heat",
               "[HEAT_FLUXES]\nSURFACE_EXCHANGE ON\n\n"
               "[HEAT_SOURCES]\nEXTERNAL_INFLOW GLOBAL 15.0\n"
               "INITIAL_STATE GLOBAL 15.0\n");
    const std::string body = deck("_pe_api.heat", "");

    // Control: no forcing at all.
    SWMM_Engine ctrl = run("_pe_api_ctrl", body);
    ASSERT_NE(ctrl, nullptr);
    const double ca_ctrl =
        as_cpp(ctrl).context().heat_state.link_temp[static_cast<std::size_t>(
            link_of(as_cpp(ctrl).context(), "CA"))];
    const double cb_ctrl =
        as_cpp(ctrl).context().heat_state.link_temp[static_cast<std::size_t>(
            link_of(as_cpp(ctrl).context(), "CB"))];
    swmm_engine_destroy(ctrl);

    write_file("_pe_api_f.inp", body);
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_engine_open(e, "_pe_api_f.inp", "_pe_api_f.rpt",
                               "_pe_api_f.out", nullptr), SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(e), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(e, 1), SWMM_OK);
    const auto& ctx = as_cpp(e).context();
    const int cb = link_of(ctx, "CB"), ca = link_of(ctx, "CA");

    // PERSIST so one call covers the run; the RESET contract is gate 8.
    ASSERT_EQ(swmm_forcing_element_climate(
                  e, SWMM_HEAT_ELEM_LINK, cb,
                  SWMM_FORCE_ELEM_AIR_TEMPERATURE, -12.0, SWMM_FORCING_ADD,
                  SWMM_FORCING_PERSIST), SWMM_OK);
    double back = 0.0;
    int mode = -1;
    ASSERT_EQ(swmm_forcing_element_climate_get(
                  e, SWMM_HEAT_ELEM_LINK, cb,
                  SWMM_FORCE_ELEM_AIR_TEMPERATURE, &back, &mode), SWMM_OK);
    EXPECT_EQ(mode, SWMM_FORCING_ADD);
    EXPECT_DOUBLE_EQ(back, -12.0) << "US units must round-trip unconverted";

    double el = 0.0;
    int guard = 0;
    do { ASSERT_EQ(swmm_engine_step(e, &el), SWMM_OK); }
    while (el > 0.0 && ++guard < 40000);
    swmm_engine_end(e);

    const double ca_f = ctx.heat_state.link_temp[static_cast<std::size_t>(ca)];
    const double cb_f = ctx.heat_state.link_temp[static_cast<std::size_t>(cb)];
    EXPECT_DOUBLE_EQ(ca_f, ca_ctrl)
        << "the UNFORCED link moved — the push is not element-scoped";
    EXPECT_LT(cb_f, cb_ctrl - 0.05)
        << "the forced link did not cool: elementAirTempF is not reaching "
           "surfaceFluxOut";

    // SUBCATCH is refused by design (§6.2): a subcatchment air temperature
    // has competing consumers, so there is deliberately no spelling for it.
    EXPECT_EQ(swmm_forcing_element_climate(
                  e, 2 /* SUBCATCH */, 0, SWMM_FORCE_ELEM_AIR_TEMPERATURE,
                  1.0, SWMM_FORCING_ADD, SWMM_FORCING_RESET),
              SWMM_ERR_BADPARAM);
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// 8. RESET semantics — the stale-field guard. Push on ONE step only; the
//    next step must read the global broadcast again.
//
//    This is the gate most likely to be skipped, because everything "works"
//    without it: under a buggy PERSIST-by-default the forced value simply
//    keeps applying and every other gate still passes.
// ---------------------------------------------------------------------------
TEST(HeatPerElementTest, ResetForcingDoesNotPersistToTheNextStep) {
    write_file("_pe_reset.heat",
               "[HEAT_FLUXES]\nSURFACE_EXCHANGE ON\n\n"
               "[HEAT_SOURCES]\nEXTERNAL_INFLOW GLOBAL 15.0\n"
               "INITIAL_STATE GLOBAL 15.0\n");
    write_file("_pe_reset.inp", deck("_pe_reset.heat", ""));
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_engine_open(e, "_pe_reset.inp", "_pe_reset.rpt",
                               "_pe_reset.out", nullptr), SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(e), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(e, 1), SWMM_OK);
    const auto& ctx = as_cpp(e).context();
    const int cb = link_of(ctx, "CB");

    ASSERT_EQ(swmm_forcing_element_climate(
                  e, SWMM_HEAT_ELEM_LINK, cb,
                  SWMM_FORCE_ELEM_AIR_TEMPERATURE, -30.0, SWMM_FORCING_ADD,
                  SWMM_FORCING_RESET), SWMM_OK);
    double el = 0.0;
    ASSERT_EQ(swmm_engine_step(e, &el), SWMM_OK);

    int mode = -1;
    ASSERT_EQ(swmm_forcing_element_climate_get(
                  e, SWMM_HEAT_ELEM_LINK, cb,
                  SWMM_FORCE_ELEM_AIR_TEMPERATURE, nullptr, &mode), SWMM_OK);
    EXPECT_EQ(mode, SWMM_FORCING_NONE)
        << "a RESET forcing survived its step — a driver that stops pushing "
           "would silently keep a stale field that looks like data";
    swmm_engine_end(e);
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// 9. ⚠ THE LOAD-BEARING GATE (§6.2). A per-link air-temperature push must
//    NOT reach snowmelt. The whole reason per-element climate is API-only
//    rather than deck syntax is that ClimateState is shared with hydrology;
//    if the value were written into ClimateState instead of resolved at the
//    flux call, the snowpack above a conduit would inherit that conduit's
//    air temperature.
//
//    FALSIFIER: assign the per-element value into `ctx.climate_state` and
//    this gate fails while gates 7 and 8 still pass.
// ---------------------------------------------------------------------------
TEST(HeatPerElementTest, PerLinkClimateDoesNotReachSnowmelt) {
    write_file("_pe_snow.heat",
               "[HEAT_FLUXES]\nSURFACE_EXCHANGE ON\n\n"
               "[HEAT_SOURCES]\nEXTERNAL_INFLOW GLOBAL 2.0\n"
               "INITIAL_STATE GLOBAL 2.0\n");
    // A subcatchment with a SEEDED, MELTING snowpack, draining to the same
    // network. Air is 95 °F, so the pack is melting; a -80 °F push on a LINK
    // must not slow that down by even a bit.
    //
    // The handoff's draft never attached SP1 to S1 (no 9th [SUBCATCHMENTS]
    // field) and seeded every layer at 0.0 depth — no pack could exist in
    // July at 95 °F, so the gate compared 0.0 with 0.0 and passed for the
    // wrong reason, exactly the vacuity §7 warned about. The pack is now
    // attached and seeded (SD0 = 2.0 in), and the premises below assert
    // that it exists AND is actually melting before the comparison means
    // anything.
    const std::string snow =
        "[SUBCATCHMENTS]\nS1 G1 J0 3.0 50 400 0.5 0 SP1\n\n"
        "[SUBAREAS]\nS1 0.01 0.1 0.05 0.05 25 OUTLET\n\n"
        "[INFILTRATION]\nS1 3.0 0.5 4\n\n"
        "[RAINGAGES]\nG1 INTENSITY 0:05 1.0 TIMESERIES rain_ts\n\n"
        "[SNOWPACKS]\n"
        "SP1 PLOWABLE   0.001 0.001 32.0 1.0 2.0 0.0 0.0\n"
        "SP1 IMPERVIOUS 0.001 0.001 32.0 1.0 2.0 0.0 0.0\n"
        "SP1 PERVIOUS   0.001 0.001 32.0 1.0 2.0 0.0 0.0\n"
        "SP1 REMOVAL    1.0 0.0 0.0 0.0 0.0 0.0\n\n"
        "[TIMESERIES]\nrain_ts 07/01/2026 00:00 0.0\n"
        "rain_ts 07/01/2026 04:00 0.0\n\n";

    auto body = [&](bool /*forced*/) {
        std::string d = deck("_pe_snow.heat", "");
        const std::string mark = "[PROCESS_COMPONENTS]";
        const auto at = d.find(mark);
        EXPECT_NE(at, std::string::npos);
        d.insert(at, snow);
        return d;
    };

    // Control run — stepped by hand so the pack can be read BEFORE and
    // after. Read snow through the PUBLIC API, not a field name — the state
    // layout is not this gate's business and guessing at it is how a test
    // comes to assert against a member that moved.
    write_file("_pe_snow_ctrl.inp", body(false));
    SWMM_Engine ctrl = swmm_engine_create();
    ASSERT_NE(ctrl, nullptr);
    ASSERT_EQ(swmm_engine_open(ctrl, "_pe_snow_ctrl.inp", "_pe_snow_ctrl.rpt",
                               "_pe_snow_ctrl.out", nullptr), SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(ctrl), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(ctrl, 1), SWMM_OK);
    double snow_start = -1.0;
    ASSERT_EQ(swmm_subcatch_get_snow_depth(ctrl, 0, &snow_start), SWMM_OK);
    {
        double el = 0.0;
        int guard = 0;
        do { ASSERT_EQ(swmm_engine_step(ctrl, &el), SWMM_OK); }
        while (el > 0.0 && ++guard < 40000);
    }
    swmm_engine_end(ctrl);
    double snow_ctrl = -1.0;
    ASSERT_EQ(swmm_subcatch_get_snow_depth(ctrl, 0, &snow_ctrl), SWMM_OK);
    swmm_engine_destroy(ctrl);

    // Non-vacuity premises (§7's warning made structural): the pack exists,
    // and it is MELTING but not exhausted — so the final depth is sensitive
    // to the air temperature the melt equation reads. Without these, a gate
    // comparing 0.0 with 0.0 passes under the very falsifier it exists for.
    ASSERT_GT(snow_start, 0.0) << "no pack — SP1 not attached or not seeded";
    ASSERT_GT(snow_ctrl, 0.0) << "pack fully melted — shorten the run";
    ASSERT_LT(snow_ctrl, snow_start)
        << "pack did not melt — the comparison below is insensitive to air "
           "temperature and the gate is vacuous";

    // Forced run: a huge per-LINK air-temperature push.
    write_file("_pe_snow_f.inp", body(true));
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_engine_open(e, "_pe_snow_f.inp", "_pe_snow_f.rpt",
                               "_pe_snow_f.out", nullptr), SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(e), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(e, 1), SWMM_OK);
    const auto& ctx = as_cpp(e).context();
    const int nl = ctx.n_links();
    (void)ctx;
    for (int j = 0; j < nl; ++j)
        ASSERT_EQ(swmm_forcing_element_climate(
                      e, SWMM_HEAT_ELEM_LINK, j,
                      SWMM_FORCE_ELEM_AIR_TEMPERATURE, -80.0,
                      SWMM_FORCING_ADD, SWMM_FORCING_PERSIST), SWMM_OK);
    double el = 0.0;
    int guard = 0;
    do { ASSERT_EQ(swmm_engine_step(e, &el), SWMM_OK); }
    while (el > 0.0 && ++guard < 40000);
    swmm_engine_end(e);

    double snow_forced = -2.0;
    ASSERT_EQ(swmm_subcatch_get_snow_depth(e, 0, &snow_forced), SWMM_OK);
    EXPECT_DOUBLE_EQ(snow_forced, snow_ctrl)
        << "a per-LINK air-temperature push changed the SNOWPACK. The value "
           "is reaching ClimateState instead of being resolved at the flux "
           "call — the one thing PE4's design forbids (plan §6.2).";
    // The check found the snowpack comparison alone is INSULATED from the
    // contamination it exists to catch: the climate broadcast re-asserts the
    // global every hydrology step, healing a stale write before snowmelt
    // reads it (both falsifier-v routes left it green). This observer reads
    // the shared state DIRECTLY: after the run it must hold the broadcast
    // air temperature, never a per-element value. Under either falsifier it
    // reads 15 °F (95 − 80) and fails; the snowpack leg above stays as the
    // consequence-level assertion for orderings where the broadcast no
    // longer heals.
    EXPECT_DOUBLE_EQ(ctx.climate_state.temperature, 95.0)
        << "the shared ClimateState carries a per-element push — PE4's "
           "resolve-at-the-flux-call contract is broken even if the "
           "snowpack has not yet noticed";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// 10. The structural claim of D-PE2: a model with no override rows allocates
//     NOTHING and reads the very same global object. This is what makes the
//     corpus byte-identical by construction rather than by tolerance.
// ---------------------------------------------------------------------------
TEST(HeatPerElementTest, NoOverridesAllocatesNothingAndReadsTheGlobal) {
    write_file("_pe_none.heat",
               "[HEAT_FLUXES]\nRADIATIVE_EXCHANGE ON\n\n"
               "[RADIATIVE_FLUXES]\nSHADE_FACTOR GLOBAL 0.4\n");
    SWMM_Engine e = run("_pe_none", deck("_pe_none.heat", ""));
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp(e).context();
    const auto& ov = ctx.heat_config.overrides;

    EXPECT_TRUE(ov.rows.empty());
    EXPECT_TRUE(ov.rad_link.empty()) << "sized a vector nothing asked for";
    EXPECT_TRUE(ov.rad_node.empty());
    EXPECT_TRUE(ov.sed_link.empty());

    namespace th = openswmm::transport::heat;
    using openswmm::HeatElement;
    // Identity, not equality: the accessor must hand back THE global object.
    EXPECT_EQ(&th::radiativeFor(ctx, HeatElement::link(0)),
              &ctx.heat_config.radiative);
    EXPECT_EQ(&th::sedimentFor(ctx, HeatElement::link(0)),
              &ctx.heat_config.sediment);
    // And a default-constructed token (index -1) must be safe.
    EXPECT_EQ(&th::radiativeFor(ctx, HeatElement{}),
              &ctx.heat_config.radiative);
    swmm_engine_destroy(e);
}
