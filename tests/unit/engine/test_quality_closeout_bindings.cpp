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
 * @file test_quality_closeout_bindings.cpp
 * @brief The 2026-09-01 closeout batch: five bindings, one gate each.
 *
 * @details Every gate here was written to FAIL at base:
 *          1. LARD applied NO flux module (transported temperature,
 *             exchanged nothing, silently — no warning either).
 *          2. The bed declined under ARD (warned by name).
 *          3. Treatment declined under LARD (warned by name).
 *          4. MSX state did not advect under LEGACY (warned by name).
 *          5. ARD's non-negativity clamp floored sub-zero temperatures at
 *             0 °C (debt 216, recorded not fixed in the ARD-relax round).
 *
 *          Each asserts the RESULT of the retired limitation, not the
 *          absence of its warning — a deleted warning with unchanged
 *          behaviour is the failure mode these exist to catch.
 *
 *          Scratch fixtures use the `_qcb_` prefix (collision-checked).
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>

#include "core/SWMMEngine.hpp"

namespace {

openswmm::SWMMEngine& as_cpp_engine(SWMM_Engine engine) {
    return *static_cast<openswmm::SWMMEngine*>(engine);
}

void write_file(const std::string& path, const std::string& body) {
    std::ofstream f(path);
    ASSERT_TRUE(f.is_open()) << path;
    f << body;
}

/// Three junctions and an outfall on a circular chain, one steady inflow.
/// `extra_options` lands in [OPTIONS]; `extra_sections` after [XSECTIONS].
std::string deck(const std::string& extra_options,
                 const std::string& extra_sections) {
    return "[TITLE]\ncloseout bindings\n\n[OPTIONS]\n"
           "FLOW_UNITS           CFS\nFLOW_ROUTING         DYNWAVE\n"
           "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
           "END_DATE             01/01/2026\nEND_TIME             02:00:00\n"
           "ROUTING_STEP         5\nREPORT_STEP          00:05:00\n" +
           extra_options +
           "\n[JUNCTIONS]\n"
           "J0 10.0 10 0.5 0 0\nJ1 9.0 10 0.5 0 0\nJ2 8.0 10 0.5 0 0\n\n"
           "[OUTFALLS]\nOUT 6.5 FREE NO\n\n"
           "[CONDUITS]\n"
           "C1 J0 J1 400 0.013 0 0 0\nC2 J1 J2 400 0.013 0 0 0\n"
           "C3 J2 OUT 400 0.013 0 0 0\n\n"
           "[XSECTIONS]\n"
           "C1 CIRCULAR 1.5 0 0 0\nC2 CIRCULAR 1.5 0 0 0\n"
           "C3 CIRCULAR 1.5 0 0 0\n\n" +
           extra_sections +
           "[INFLOWS]\nJ0 FLOW \"\" FLOW 1.0 1.0 3.0\n\n"
           "[REPORT]\nINPUT NO\n";
}

/// Open + run a deck to completion; returns the engine (caller destroys)
/// or nullptr. The context stays readable until destroy.
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
    double elapsed = 0.0;
    int guard = 0;
    do {
        if (swmm_engine_step(e, &elapsed) != SWMM_OK) {
            swmm_engine_destroy(e);
            return nullptr;
        }
    } while (elapsed > 0.0 && ++guard < 40000);
    swmm_engine_end(e);
    return e;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. LARD applies the surface flux modules. Hot air (90 °F) over 5 °C water
//    with SURFACE_EXCHANGE ON under LAGRANGIAN: the chain must WARM. At
//    base the final link temperatures sit exactly at the 5 °C initial state
//    — the modules were never called — which is what this asserts against.
//
//    The LEGACY twin runs as the control: LARD must move in the same
//    direction and land within a band of it, so the gate cannot be
//    satisfied by any warming (e.g. a sign error that heats 10x).
// ---------------------------------------------------------------------------
TEST(QualityCloseoutTest, LardAppliesSurfaceFluxes) {
    const std::string heat_cfg =
        "[HEAT_FLUXES]\nSURFACE_EXCHANGE ON\n\n"
        "[HEAT_SOURCES]\nEXTERNAL_INFLOW GLOBAL 5.0\nINITIAL_STATE GLOBAL 5.0\n";
    write_file("_qcb_flux.heat", heat_cfg);

    // 90 °F air via a [TEMPERATURE] TIMESERIES — the handoff's draft put
    // "TEMPERATURE 90" in [OPTIONS], which is an unknown key (ext_options +
    // warning) and leaves the air at the no-source default of 0 °F, under
    // which BOTH engines cool and the control assert below trips. Checker
    // fix, 2026-09-01; the gate's direction (warming) is unchanged.
    // The 25 mph wind is what makes the flux slope observable at all on a
    // six-minute transit — windless, LEGACY itself warms the outlet by
    // ~0.01 degC and the control assert cannot discriminate (the ARD-relax
    // round's lesson, re-learned here on the first run of this fixture).
    const std::string air =
        "[TEMPERATURE]\nTIMESERIES air_ts\nHUMIDITY 50\n"
        "WINDSPEED MONTHLY 25 25 25 25 25 25 25 25 25 25 25 25\n\n"
        "[TIMESERIES]\nair_ts 01/01/2026 00:00 90.0\n"
        "air_ts 01/02/2026 00:00 90.0\n\n";
    const std::string opts_lard =
        "HEAT_TRANSPORT       YES\nQUALITY_SOLVER       LAGRANGIAN\n";
    const std::string opts_leg =
        "HEAT_TRANSPORT       YES\n";
    const std::string pc = air +
        "[PROCESS_COMPONENTS]\n"
        "org.hydrocouple.openswmm.heat config=\"_qcb_flux.heat\"\n\n";

    // Open channels, or there is nothing to exchange THROUGH: the fixture
    // shipped all-CIRCULAR, whose closed conduits have no free surface (the
    // ClosedConduitCellsDoNotExchange contract) and whose junctions have
    // none either — zero exchange was correct physics on that deck under
    // every engine, and the control assert below caught it on first run.
    auto open_channels = [](std::string d) {
        std::string::size_type at;
        while ((at = d.find("CIRCULAR 1.5 0 0 0")) != std::string::npos)
            d.replace(at, 18, "RECT_OPEN 3.0 4.0 0 0");
        return d;
    };
    SWMM_Engine lard = run("_qcb_flux_lard", open_channels(deck(opts_lard, pc)));
    SWMM_Engine leg  = run("_qcb_flux_leg", open_channels(deck(opts_leg, pc)));
    ASSERT_NE(lard, nullptr);
    ASSERT_NE(leg, nullptr);

    const auto& cl = as_cpp_engine(lard).context();
    const auto& cg = as_cpp_engine(leg).context();
    ASSERT_GT(cl.heat_state.link_temp.size(), 2u);

    const double t_lard = cl.heat_state.link_temp[2];  // C3, chain outlet
    const double t_leg  = cg.heat_state.link_temp[2];
    // The premise must hold for the control, or the deck is not
    // discriminating and this gate would be vacuous.
    ASSERT_GT(t_leg, 5.5) << "the LEGACY control did not warm — deck defect";
    EXPECT_GT(t_lard, 5.5)
        << "LARD applied no surface flux (the pre-batch behaviour)";
    EXPECT_NEAR(t_lard, t_leg, std::max(1.0, 0.15 * (t_leg - 5.0)))
        << "LARD warms, but not where the LEGACY control lands";

    swmm_engine_destroy(lard);
    swmm_engine_destroy(leg);
}

// ---------------------------------------------------------------------------
// 2. The bed binds under ARD. 20 °C water, no surface fluxes, bed ON with a
//    5 °C ground at a shallow ground depth: the chain must COOL toward the
//    ground. At base ARD warned by name and applied nothing, so the final
//    temperature sat at 20 °C exactly.
// ---------------------------------------------------------------------------
TEST(QualityCloseoutTest, BedBindsUnderArd) {
    write_file("_qcb_bed.heat",
               "[HEAT_FLUXES]\nSEDIMENT_EXCHANGE ON\n\n"
               "[SEDIMENT_EXCHANGE]\n"
               "GROUND_TEMPERATURE GLOBAL 5.0\n"
               "GROUND_DEPTH GLOBAL 0.1\n"
               "BED_THICKNESS GLOBAL 0.1\n"
               "THERMAL_DIFFUSIVITY GLOBAL 1.0e-5\n\n"
               "[HEAT_SOURCES]\n"
               "EXTERNAL_INFLOW GLOBAL 20.0\nINITIAL_STATE GLOBAL 20.0\n");
    const std::string opts =
        "HEAT_TRANSPORT       YES\nQUALITY_SOLVER       EULERIAN_ARD\n";
    const std::string pc =
        "[PROCESS_COMPONENTS]\n"
        "org.hydrocouple.openswmm.heat config=\"_qcb_bed.heat\"\n\n";

    SWMM_Engine e = run("_qcb_bed_ard", deck(opts, pc));
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();
    ASSERT_GT(ctx.heat_state.link_temp.size(), 2u);
    EXPECT_LT(ctx.heat_state.link_temp[2], 19.5)
        << "the ARD bed did not exchange (the pre-batch decline)";
    EXPECT_GT(ctx.heat_state.link_temp[2], 4.9)
        << "cooled BELOW the ground temperature — no term can do that";
    // And the engine-gating warning must be gone.
    for (const auto& w : ctx.warnings)
        EXPECT_EQ(w.find("binds to the LEGACY link store"), std::string::npos)
            << w;
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// 3. Treatment runs under LARD. A 50 % TSS removal at J1 must halve what
//    reaches the outfall relative to the untreated twin. At base the
//    warning fired and removal was 0, so the two twins agreed exactly.
// ---------------------------------------------------------------------------
TEST(QualityCloseoutTest, TreatmentRemovesUnderLard) {
    const std::string polls =
        "[POLLUTANTS]\nTSS MG/L 0 0 0 0\n\n";
    const std::string treat =
        "[TREATMENT]\nJ1 TSS R = 0.5\n\n";
    const std::string inflow_tss =
        "[TIMESERIES]\nT_TSS 0:00 100\nT_TSS 2:00 100\n\n";
    const std::string opts = "QUALITY_SOLVER       LAGRANGIAN\n";

    // The inflow row needs the pollutant column; rebuild [INFLOWS].
    auto body = [&](bool treated) {
        std::string d = deck(opts, polls + (treated ? treat : "") +
                                       inflow_tss);
        const std::string mark = "[INFLOWS]\nJ0 FLOW \"\" FLOW 1.0 1.0 3.0\n";
        const auto at = d.find(mark);
        EXPECT_NE(at, std::string::npos);
        d.insert(at + mark.size(), "J0 TSS T_TSS CONCEN 1.0 1.0\n");
        return d;
    };

    SWMM_Engine treated   = run("_qcb_treat_on", body(true));
    SWMM_Engine untreated = run("_qcb_treat_off", body(false));
    ASSERT_NE(treated, nullptr);
    ASSERT_NE(untreated, nullptr);

    const auto& ct = as_cpp_engine(treated).context();
    const auto& cu = as_cpp_engine(untreated).context();
    const auto np = static_cast<std::size_t>(ct.n_pollutants());
    ASSERT_GE(np, 1u);
    const double c3_t = ct.links.conc[2 * np];      // C3 TSS
    const double c3_u = cu.links.conc[2 * np];
    ASSERT_GT(c3_u, 50.0) << "untreated control never loaded — deck defect";
    EXPECT_LT(c3_t, 0.75 * c3_u)
        << "treatment removed nothing under LARD (the pre-batch bypass)";
    for (const auto& w : ct.warnings)
        EXPECT_EQ(w.find("treatment interop is"), std::string::npos) << w;
    swmm_engine_destroy(treated);
    swmm_engine_destroy(untreated);
}

// ---------------------------------------------------------------------------
// 4. MSX state advects under LEGACY. A zero-kinetics RATE species seeded in
//    C1 (and only C1) via [REACTION_QUALITY] must ARRIVE downstream: after
//    two hours of steady flow, C3's species concentration is nonzero. At
//    base the species reacted in place and C3 stayed at exactly 0 forever.
// ---------------------------------------------------------------------------
TEST(QualityCloseoutTest, MsxSpeciesAdvectUnderLegacy) {
    write_file("_qcb_msx.rxn",
               "[REACTION_OPTIONS]\nSOLVER EUL\n\n"
               "[REACTION_SPECIES]\nBULK TRACER MG\n\n"
               "[REACTION_PIPES]\nRATE TRACER 0\n\n"
               "[REACTION_TANKS]\nRATE TRACER 0\n\n"
               "[REACTION_QUALITY]\nLINK C1 TRACER 80.0\n");
    const std::string pc =
        "[PROCESS_COMPONENTS]\n"
        "org.hydrocouple.openswmm.reactions config=\"_qcb_msx.rxn\"\n\n";

    // Twelve minutes, not two hours: the seeded slug transits the chain in
    // ~10 minutes and a 2 h run flushes it out entirely — the first run of
    // this gate read C3 at 2.5e-20 WITH the transport working (instrumented:
    // J1 climbed 0 → 57 mg/L while C1 drained), because it asserted on the
    // post-flush state. A slug gate reads MID-TRANSIT or it reads nothing.
    std::string body = deck("", pc);
    const std::string et = "END_TIME             02:00:00";
    const auto at = body.find(et);
    ASSERT_NE(at, std::string::npos);
    body.replace(at, et.size(), "END_TIME             00:12:00");

    SWMM_Engine e = run("_qcb_msx_leg", body);
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();
    const int nsp = ctx.reactions.n_species();
    ASSERT_EQ(nsp, 1);
    ASSERT_GE(ctx.reactions.msx_link_conc.size(), 3u);
    const double c3 = ctx.reactions.msx_link_conc[2];  // link C3, species 0
    EXPECT_GT(c3, 1.0)
        << "the seeded tracer never arrived downstream — MSX state did not "
           "advect under LEGACY (the pre-batch limitation)";
    // Two hours of 3 cfs through three 400 ft pipes flushes the slug well
    // below its seed value at the seeded link too — advection moves mass
    // OUT, which distinguishes transport from a copy.
    const double c1 = ctx.reactions.msx_link_conc[0];
    EXPECT_LT(c1, 79.0) << "C1 still at its seed — nothing left, so what "
                           "arrived at C3 was not transport";
    for (const auto& w : ctx.warnings)
        EXPECT_EQ(w.find("not yet transported"), std::string::npos) << w;
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// 5. Sub-zero temperature survives ARD transport (debt 216). INITIAL_STATE
//    and inflow at −5 °C, no fluxes: the only physics is advection, and the
//    answer everywhere is −5. At base the store floor clamped the
//    temperature rows to 0 °C, so the outlet read exactly 0.
// ---------------------------------------------------------------------------
TEST(QualityCloseoutTest, SubZeroTemperatureSurvivesArdTransport) {
    write_file("_qcb_subz.heat",
               "[HEAT_SOURCES]\n"
               "EXTERNAL_INFLOW GLOBAL -5.0\nINITIAL_STATE GLOBAL -5.0\n");
    const std::string opts =
        "HEAT_TRANSPORT       YES\nQUALITY_SOLVER       EULERIAN_ARD\n";
    const std::string pc =
        "[PROCESS_COMPONENTS]\n"
        "org.hydrocouple.openswmm.heat config=\"_qcb_subz.heat\"\n\n";

    SWMM_Engine e = run("_qcb_subz_ard", deck(opts, pc));
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();
    ASSERT_GT(ctx.heat_state.link_temp.size(), 2u);
    // Not a band around −5: any clamp pins EXACTLY at 0, and −5 water with
    // no fluxes has nowhere else to go, so the assertion can be tight.
    EXPECT_LT(ctx.heat_state.link_temp[2], -4.5)
        << "sub-zero water arrived at 0 °C — the non-negativity floor is "
           "still capturing the temperature row (debt 216)";
    swmm_engine_destroy(e);
}
