// Copyright (c) 2026 Caleb Buahin
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
 * @file test_lard_heat.cpp
 * @brief H7b gates — temperature through the LARD segment store.
 *
 * @details Temperature rides the segments as the SECOND reserved species
 *          row (SpeciesRowLayout::temp_row, after age — H7a made the
 *          identity an index precisely so this row could exist), sourced
 *          from `node_temp_vol_in` (the D-UT10 twin the seven loaders
 *          already fill), state published to `heat_state` (degC). It takes
 *          RWPT dispersion at the same coefficient as a solute — the ARD
 *          engine's deliberate choice for its own temperature row, adopted
 *          2026-08-30 for cross-engine consistency.
 *
 *          What each gate must be able to fail against:
 *          - gate 1 is the cross-engine claim (LEGACY is the control);
 *          - gate 2 is the empty-slab HOLD — unlike age the held value
 *            does not grow, and unlike age the report does NOT mask it
 *            (0 degC is an ordinary temperature; the no-mask call is
 *            documented at the snapshot builder);
 *          - gate 3 records the hotstart contract as it actually is: NO
 *            engine persists temperature (HotStartNodeRecord has an age
 *            field and no temperature field), so a restarted run re-seeds
 *            from INITIAL_STATE. The day the record gains the field, this
 *            gate fails and its replacement asserts the round-trip.
 *          - gate 4 is the dispersion decision's observer: RWPT must MOVE
 *            temperature, or "disperses like ARD" is a comment.
 *
 * @see plans/transport/FINALIZATION_SEQUENCE_2026-08-29.md step 2
 * @see plans/transport/H7A_LARD_ROW_LAYOUT_HANDOFF_2026-08-29.md
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_hotstart.h>

#include "core/SWMMEngine.hpp"

namespace {

openswmm::SWMMEngine& as_cpp_engine(SWMM_Engine engine) {
    return *static_cast<openswmm::SWMMEngine*>(engine);
}

constexpr double kQ     = 5.0;   ///< steady inflow, cfs
constexpr double kTin   = 30.0;  ///< EXTERNAL_INFLOW temperature, degC
constexpr double kTinit = 10.0;  ///< INITIAL_STATE temperature, degC
constexpr int    kC5    = 4;     ///< last conduit of the chain
constexpr int    kC3    = 2;     ///< mid-chain conduit
constexpr double kCin   = 100.0; ///< TSS inflow concentration, mg/L

struct DeckSpec {
    const char* solver = "LAGRANGIAN";  ///< "" = omit (LEGACY default)
    bool rwpt = false;                  ///< DISPERSION RWPT + fixed seed
    int  inflow_stop_h = 0;             ///< 0 = steady for the whole run
    bool bone_dry = false;              ///< no inflow, no initial depth
    bool pollutant_decay = false;       ///< [POLLUTANTS] TSS, k = 1e-3 1/s
    int  end_h = 4;
};

void write_heat_cfg(const char* path) {
    std::ofstream f(path);
    f << "[HEAT_SOURCES]\n"
      << "RAINFALL GLOBAL " << kTinit << "\n"
      << "INITIAL_STATE GLOBAL " << kTinit << "\n"
      << "EXTERNAL_INFLOW GLOBAL " << kTin << "\n\n"
      << "[HEAT_FLUXES]\nSURFACE_EXCHANGE OFF\nRADIATIVE_EXCHANGE OFF\n";
}

void write_deck(const std::string& path, const DeckSpec& s,
                const char* hs_file) {
    std::ofstream f(path);
    f << "[TITLE]\nLARD H7b heat deck\n\n[OPTIONS]\n"
      << "FLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\nHEAT_TRANSPORT ON\n";
    if (s.solver[0] != '\0') f << "QUALITY_SOLVER " << s.solver << "\n";
    if (s.rwpt) f << "DISPERSION RWPT\nRWPT_SEED 42\n";
    f << "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
      << "END_DATE 01/01/2026\nEND_TIME 0" << s.end_h << ":00:00\n"
      << "ROUTING_STEP 5\nREPORT_STEP 00:05:00\n\n";
    (void)hs_file;  // the restart applies via the hotstart API instead
    const char* y0 = s.bone_dry ? "0" : "1.5";
    f << "[JUNCTIONS]\n"
      << "J0 10.0 10 " << y0 << " 0 0\nJ1 9.4  10 " << y0 << " 0 0\n"
      << "J2 8.8  10 " << y0 << " 0 0\nJ3 8.2  10 " << y0 << " 0 0\n"
      << "J4 7.6  10 " << y0 << " 0 0\n\n"
      << "[OUTFALLS]\nOUT 7.0 FREE  NO\n\n"
      << "[CONDUITS]\n"
      << "C1 J0 J1 500 0.013 0 0 0\nC2 J1 J2 500 0.013 0 0 0\n"
      << "C3 J2 J3 500 0.013 0 0 0\nC4 J3 J4 500 0.013 0 0 0\n"
      << "C5 J4 OUT 500 0.013 0 0 0\n\n"
      << "[XSECTIONS]\n"
      << "C1 CIRCULAR 2.0 0 0 0\nC2 CIRCULAR 2.0 0 0 0\n"
      << "C3 CIRCULAR 2.0 0 0 0\nC4 CIRCULAR 2.0 0 0 0\n"
      << "C5 CIRCULAR 2.0 0 0 0\n\n";
    if (s.pollutant_decay)
        f << "[POLLUTANTS]\nTSS MG/L 0 0 0 86.4 NO * 0 0 0\n\n";  // 1e-3 1/s in the 1/day column (KD1)
    if (s.bone_dry) {
        // no [INFLOWS] at all — the network never wets
    } else if (s.inflow_stop_h > 0) {
        f << "[INFLOWS]\nJ0 FLOW inq FLOW 1.0 1.0\n\n[TIMESERIES]\n";
        for (int h = 0; h <= s.end_h; ++h)
            f << "inq 01/01/2026 " << (h < 10 ? "0" : "") << h << ":00 "
              << (h < s.inflow_stop_h ? kQ : 0.0) << "\n";
        f << "\n";
    } else {
        f << "[INFLOWS]\nJ0 FLOW \"\" FLOW 1.0 1.0 " << kQ << "\n";
        if (s.pollutant_decay)
            f << "J0 TSS \"\" CONCEN 1.0 1.0 " << kCin << "\n";
        f << "\n";
    }
    f << "[PROCESS_COMPONENTS]\n"
      << "org.hydrocouple.openswmm.heat config=\"_lh.heat\"\n\n"
      << "[REPORT]\nINPUT NO\n";
}

struct HeatRun {
    bool ok = false;
    double tss_c5_final = -1.0;               ///< outfall-link TSS, mg/L
    std::vector<double> link_temp_final;      ///< [link] degC at end
    std::vector<double> c3_trace;             ///< mid-chain temp per step
    std::vector<std::string> warnings;
};

HeatRun run_deck(const std::string& tag, const DeckSpec& s,
                 const char* save_hs = nullptr,
                 const char* use_hs = nullptr) {
    HeatRun out;
    write_heat_cfg("_lh.heat");
    DeckSpec spec = s;
    write_deck(tag + ".inp", spec, "");
    SWMM_Engine e = swmm_engine_create();
    if (e == nullptr) { ADD_FAILURE() << "create"; return out; }
    bool ok = swmm_engine_open(e, (tag + ".inp").c_str(),
                               (tag + ".rpt").c_str(),
                               (tag + ".out").c_str(), nullptr) == SWMM_OK &&
              swmm_engine_initialize(e) == SWMM_OK;
    // Restart path: apply the saved state through the hotstart API (the
    // test_water_age convention) between initialize and start.
    if (ok && use_hs != nullptr) {
        SWMM_HotStart hs = nullptr;
        ok = swmm_hotstart_open(use_hs, &hs) == SWMM_OK &&
             swmm_hotstart_apply(e, hs) == SWMM_OK;
        if (hs != nullptr) swmm_hotstart_close(hs);
    }
    ok = ok && swmm_engine_start(e, 1) == SWMM_OK;
    if (ok) {
        const auto& ctx = as_cpp_engine(e).context();
        double elapsed = 0.0;
        int guard = 0;
        do {
            if (swmm_engine_step(e, &elapsed) != SWMM_OK) { ok = false; break; }
            if (static_cast<std::size_t>(kC3) <
                ctx.heat_state.link_temp.size())
                out.c3_trace.push_back(ctx.heat_state.link_temp[kC3]);
        } while (elapsed > 0.0 && ++guard < 40000);
        if (ok) {
            swmm_engine_end(e);
            if (ctx.n_pollutants() > 0)
                out.tss_c5_final =
                    ctx.links.conc[static_cast<std::size_t>(kC5) *
                                   static_cast<std::size_t>(
                                       ctx.n_pollutants())];
            out.link_temp_final = ctx.heat_state.link_temp;
            out.warnings = ctx.warnings;
            if (save_hs != nullptr)
                ok = swmm_hotstart_save(e, save_hs) == SWMM_OK;
        }
    }
    if (!ok) ADD_FAILURE() << "run failed for " << tag;
    swmm_engine_destroy(e);
    out.ok = ok;
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Gate 1 — cross-engine: the LEGACY mirror is the control. A 30 degC
// external inflow into a 10 degC network must warm the whole chain to the
// inflow temperature by steady state, and LARD must land where LEGACY
// lands. The band is the X4 cross-engine convention: the two engines share
// loaders and differ only in transport, so at steady state they must agree
// far inside any physical tolerance.
// ---------------------------------------------------------------------------
TEST(LardHeatTest, TemperatureAdvancesAndMatchesTheLegacyControl) {
    DeckSpec lard;
    const HeatRun a = run_deck("_lh_lard", lard);
    DeckSpec legacy;
    legacy.solver = "";
    const HeatRun b = run_deck("_lh_legacy", legacy);
    ASSERT_TRUE(a.ok && b.ok);
    ASSERT_EQ(a.link_temp_final.size(), b.link_temp_final.size());

    // SETUP: the signal moved — LARD is not publishing the seed.
    ASSERT_GT(a.link_temp_final[kC5], kTinit + 5.0)
        << "the outfall link still reads the initial temperature — the "
           "temperature row is not advancing under LARD";

    for (std::size_t l = 0; l < a.link_temp_final.size(); ++l) {
        EXPECT_NEAR(a.link_temp_final[l], b.link_temp_final[l],
                    0.01 * kTin)
            << "link " << l << ": LARD " << a.link_temp_final[l]
            << " degC vs LEGACY " << b.link_temp_final[l]
            << " degC — the engines disagree at steady state";
        EXPECT_NEAR(a.link_temp_final[l], kTin, 0.01 * kTin)
            << "link " << l << " did not reach the inflow temperature";
    }
}

// ---------------------------------------------------------------------------
// Gate 2 — temperature does not decay with the pollutant (the X4 gate-6
// shape, one row over). The decay stage strides the POLLUTANT rows only;
// a stride slip that catches the reserved rows would cool the network
// toward zero while TSS decays — plausible numbers, which is the failure
// shape this program is bitten by most. TSS decaying is the premise
// ASSERT; the temperature holding the inflow value is the claim.
//
// (The first version of this slot asserted the empty-slab HOLD instead.
// That branch has no reachable observer: mid-run, DYNWAVE never lands a
// link's volume on exactly zero, and a bone-dry deck skips the routing —
// and with it the quality stage — entirely, so even a falsifier that
// zeroed the held state could not be seen. The hold is write-nothing by
// construction; recorded here rather than gated vacuously.)
// ---------------------------------------------------------------------------
TEST(LardHeatTest, TemperatureDoesNotDecayWithThePollutant) {
    DeckSpec s;
    s.pollutant_decay = true;   // TSS with k = 1e-4 1/s
    const HeatRun r = run_deck("_lh_decay", s);
    ASSERT_TRUE(r.ok);

    // Premise: the pollutant really decays on this deck.
    ASSERT_GT(r.tss_c5_final, 0.0);
    ASSERT_LT(r.tss_c5_final, 0.5 * kCin)
        << "TSS did not decay — the temperature claim below is vacuous";
    // Claim: the temperature is untouched by the decay stage.
    EXPECT_NEAR(r.link_temp_final[kC5], kTin, 0.01 * kTin)
        << "the outfall temperature is " << r.link_temp_final[kC5]
        << " degC — the decay stride is reaching the temperature row";
}

// ---------------------------------------------------------------------------
// Gate 3 — the hotstart contract AS IT IS. HotStartNodeRecord carries an
// age field and NO temperature field — no engine persists temperature
// (LEGACY and ARD included), so a restarted run re-seeds from
// INITIAL_STATE. This gate pins that shared, documented behaviour so the
// debt stays visible: the day the record gains a temperature field, the
// EXPECT_NEAR below fails and its replacement asserts the round-trip
// instead. (The round that widens the record owns flipping this.)
// ---------------------------------------------------------------------------
TEST(LardHeatTest, HotstartReseedsTemperatureFromInitialState) {
    DeckSpec warm;
    const HeatRun a = run_deck("_lh_hs1", warm, "_lh_hs.bin");
    ASSERT_TRUE(a.ok);
    ASSERT_GT(a.link_temp_final[kC5], kTinit + 5.0);

    // Restart from the saved state, but with a zero-length horizon of new
    // forcing: one hour, steady inflow. The hydraulics restore; the
    // temperature cannot (no field), so init re-seeds INITIAL_STATE.
    DeckSpec resumed;
    resumed.end_h = 1;
    const HeatRun b = run_deck("_lh_hs2", resumed, nullptr, "_lh_hs.bin");
    ASSERT_TRUE(b.ok);

    // First published C3 temperature after restart is the INITIAL_STATE
    // seed (the warm inflow has not reached mid-chain yet), NOT the saved
    // run's final temperature.
    ASSERT_FALSE(b.c3_trace.empty());
    EXPECT_NEAR(b.c3_trace.front(), kTinit, 1.0)
        << "the restarted run's first mid-chain temperature is "
        << b.c3_trace.front() << " degC — either the hotstart began "
           "persisting temperature (update this gate to assert the "
           "round-trip!) or the INITIAL_STATE re-seed broke";
}

// ---------------------------------------------------------------------------
// Gate 4 — the dispersion decision's observer. Temperature takes RWPT
// dispersion at the solute coefficient (the ARD engine's choice, adopted
// 2026-08-30). RWPT physically smears the warm front, so the mid-chain
// trajectory must DIFFER from the RWPT-off run during the transient —
// without this gate, "temperature disperses" is a comment, not a property.
// The steady state is the same either way (dispersion moves heat, it does
// not create it), so the difference is looked for on the rising limb.
// ---------------------------------------------------------------------------
TEST(LardHeatTest, RwptDispersionMovesTemperature) {
    DeckSpec off;
    const HeatRun a = run_deck("_lh_rwoff", off);
    DeckSpec on;
    on.rwpt = true;
    const HeatRun b = run_deck("_lh_rwon", on);
    ASSERT_TRUE(a.ok && b.ok);
    const std::size_t n = std::min(a.c3_trace.size(), b.c3_trace.size());
    ASSERT_GT(n, 100u);

    double max_diff = 0.0;
    for (std::size_t i = 0; i < n; ++i)
        max_diff = std::max(max_diff,
                            std::fabs(a.c3_trace[i] - b.c3_trace[i]));
    EXPECT_GT(max_diff, 0.05)
        << "RWPT on vs off never separates the mid-chain temperature by "
           "more than " << max_diff << " degC — the temperature row is "
           "not participating in dispersion";
    // And both settle at the inflow temperature — dispersion conserves.
    EXPECT_NEAR(a.link_temp_final[kC5], b.link_temp_final[kC5], 0.01 * kTin);
}
