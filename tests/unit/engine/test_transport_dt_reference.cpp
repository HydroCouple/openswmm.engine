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
 * @file test_transport_dt_reference.cpp
 * @brief The timestep-refinement instrument — ONE mechanism, FOUR owed items.
 *
 * @details Four falsifiers accumulated across A4, H5a, D-H5e and H5b that no
 *          single-run gate can observe, because each is a **`dt`-order
 *          error**: the defective and the correct forms converge to the SAME
 *          limit, so any fixed-`dt` assertion sees two numbers that are both
 *          "close enough" and cannot say which is right.
 *
 *          | owed item | the defect |
 *          |---|---|
 *          | A4 falsifier iii  | mix order — read the donor's NEW value, not its old one |
 *          | H5a falsifier vi  | flux applied to the POST-mix volume `v_old + v_in` |
 *          | D-H5e caveat      | one long relaxation step vs many short ones under the real nonlinear flux |
 *          | H5b falsifier ii  | conduction as a SEPARATE pass rather than inside `J(T)` |
 *
 *          H5b's round established that the last of these cannot be
 *          discriminated any other way: conduction's fixed point (uniform)
 *          and the atmospheric one COINCIDE, and both compositions are
 *          first-order consistent with the same ODE, so plain convergence
 *          proves nothing about either.
 *
 * @par What the instrument actually measures, and why it is not "does it
 *      converge"
 *      Every one of these schemes converges. The discriminator is **how fast
 *      relative to the answer's own scale**. Each gate therefore runs the
 *      SAME deck at three timesteps and asserts two things:
 *
 *        1. **Contraction toward a limit** — `|A(h) − A(h/2)|` must exceed
 *           `|A(h/2) − A(h/4)|`. This is reference-free: it is a statement
 *           about the sequence, not about any expected value. The RATIO is
 *           reported but not asserted: a clean first-order scheme on this
 *           4x ladder would give 4, and the measured values are 2.4 to 3.6,
 *           because refining the step also moves the flow solution these
 *           observables ride on. Pinning it would gate the hydraulics.
 *        2. **The coarse answer is already close** — `|A(h) − A(h/4)|` must
 *           be a small fraction of the SPREAD OF THE SOURCES on that deck.
 *           The scale comes from the deck itself (`|init − rain|`), not from
 *           a measured constant, so no golden number is embedded.
 *
 *      A splitting or mix-order defect leaves (1) intact — it is still
 *      first-order — and blows up (2), because its error COEFFICIENT is
 *      several times larger. That asymmetry is the whole instrument.
 *
 * @par The bands, and what each one was measured against
 *      Every band below sits between a MEASURED correct-form error and a
 *      MEASURED defective-form error — the defect being the falsifier the
 *      gate exists to catch. Both numbers, as `|coarse - fine| / spread`:
 *
 *      | gate | observable | correct | defective | band |
 *      |---|---|---|---|---|
 *      | 1  | LID storage age  | 0.000747 | 0.002268 | 0.0012 |
 *      | 2  | subarea temp     | 0.008761 | 0.024273 | 0.014  |
 *      | 3  | node temp        | 0.017842 | NaN      | 0.030  |
 *      | 4a | LID storage temp | 0.000650 | 0.002237 | 0.0011 |
 *      | 4b | LID soil temp    | 0.016085 | 0.037097 | 0.023  |
 *
 *      Gate 3's falsifier does not merely converge more slowly: the explicit
 *      step DIVERGES on this deck and the finiteness leg catches it, so that
 *      band is bounded only from the correct side.
 *
 * @warning **The two LID gates are only instruments because their storage
 *          layer HOLDS WATER.** With the underdrain at `1.0e-3` the layer
 *          settles at 3e-4 ft of a 0.75 ft capacity — the noise floor — its
 *          volume is not even monotone in `dt`, and both gates measure the
 *          LID solver's own timestep sensitivity instead of the transport
 *          scheme's: the separation collapses to 1.00x (gate 1) and 1.5x
 *          (gate 4). `drain_coeff` is therefore part of each gate's
 *          configuration, not deck furniture. Lesson 55 — find the regime
 *          where the term under test dominates, do not widen the band.
 *
 * @note LID decks are in the section's user units since PR #103 made the
 *       solver convert them (re-expressed in the LID fix round, 2026-08-30 —
 *       see test_water_age_lid.cpp). The LID bands were re-pinned then, to
 *       measured floors, not widened — see gate 4.
 *
 * @see plans/transport/HEAT_TRANSPORT_PLAN.md §6.2 D-H5d, §6.3 D-H5e
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>

#include "core/SWMMEngine.hpp"

namespace {

openswmm::SWMMEngine& as_cpp_engine(SWMM_Engine engine) {
    return *static_cast<openswmm::SWMMEngine*>(engine);
}

/// The three refinement levels. WET_STEP is `HH:MM:SS`, so the runoff clock
/// cannot be refined below one second — 60/20/5 is the deepest 4x-per-level
/// ladder the deck format allows. Deeper was not needed: every one of the
/// five call sites separates its falsifier at this depth.
struct Level { int wet_s; int routing_s; };
constexpr Level kCoarse{60, 20};
constexpr Level kMid   {20,  5};
constexpr Level kFine  { 5,  1};

std::string hhmmss(int seconds) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", seconds / 3600,
                  (seconds % 3600) / 60, seconds % 60);
    return buf;
}

std::string series(const char* name, int last_min, double v, int stop_min) {
    std::string s;
    char buf[80];
    for (int m = 0; m <= last_min; m += 5) {
        std::snprintf(buf, sizeof(buf), "%s 01/01/2026 %02d:%02d %.4f\n",
                      name, m / 60, m % 60, m <= stop_min ? v : 0.0);
        s += buf;
    }
    return s + "\n";
}

struct Deck {
    bool   water_age  = false;
    bool   heat       = false;
    bool   lid        = false;
    bool   surface    = false;
    bool   radiative  = false;
    bool   conduction = false;
    double rain_c     = 5.0;
    double init_c     = 30.0;
    double rain_h     = 4.0;
    double init_h     = 0.0;
    double air_f      = 86.0;
    int    end_min    = 30;
    int    rain_stop_min = 30;
    /// Underdrain coefficient. See the file warning: this is what decides
    /// whether the storage layer holds water, and so whether the two LID
    /// gates measure the transport scheme or the LID solver's own step
    /// sensitivity.
    double drain_coeff = 1.24708;
};

/// The spread the answer lives in, taken from the DECK's own sources. Gate
/// bands are fractions of this rather than absolute temperatures, so the
/// same band means the same thing if a deck's forcing changes.
double sourceSpread(const Deck& d, bool heat) {
    // The age configuration is read in HOURS but `subcatch_runoff_age` is
    // published in SECONDS, so the age spread has to be carried into the
    // observable's own unit or the band means something 3600x tighter than
    // it reads.
    return heat ? std::fabs(d.init_c - d.rain_c)
                : std::fabs(d.init_h - d.rain_h) * 3600.0;
}

void write_files(const Deck& d, const Level& lv) {
    {
        std::ofstream f("_dtref.heat");
        f << "[HEAT_SOURCES]\nRAINFALL GLOBAL " << d.rain_c
          << "\nINITIAL_STATE GLOBAL " << d.init_c << "\n\n[HEAT_FLUXES]\n"
          << "SURFACE_EXCHANGE "   << (d.surface   ? "ON" : "OFF") << "\n"
          << "RADIATIVE_EXCHANGE " << (d.radiative ? "ON" : "OFF") << "\n"
          << "LAYER_CONDUCTION "   << (d.conduction? "ON" : "OFF") << "\n";
        if (d.radiative)
            f << "\n[RADIATIVE_FLUXES]\nSHORTWAVE GLOBAL 600\n";
    }
    {
        std::ofstream f("_dtref.age");
        f << "[WATER_AGE_SOURCES]\nRAINFALL GLOBAL " << d.rain_h
          << "\nINITIAL_STATE GLOBAL " << d.init_h << "\n";
    }
    std::ofstream f("_dtref.inp");
    f << "[TITLE]\ndt-refinement reference deck\n\n[OPTIONS]\n"
      << "FLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\nINFILTRATION HORTON\n"
      << (d.water_age ? "WATER_AGE ON\n" : "")
      << (d.heat ? "HEAT_TRANSPORT ON\n" : "")
      << "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
      << "END_DATE 01/01/2026\nEND_TIME " << hhmmss(d.end_min * 60) << "\n"
      << "WET_STEP " << hhmmss(lv.wet_s) << "\n"
      << "DRY_STEP " << hhmmss(lv.wet_s) << "\n"
      << "ROUTING_STEP " << lv.routing_s << "\n"
      << "REPORT_STEP 00:05:00\n\n"
      << "[TEMPERATURE]\nTIMESERIES air_ts\nWINDSPEED MONTHLY";
    for (int m = 0; m < 12; ++m) f << " 20.0";
    f << "\nHUMIDITY";
    for (int m = 0; m < 12; ++m) f << " 20.0";
    f << "\n\n[RAINGAGES]\nRG1 INTENSITY 0:05 1.0 TIMESERIES rain_ts\n\n"
      << "[TIMESERIES]\n"
      << series("rain_ts", d.end_min + 5, 2.0, d.rain_stop_min)
      << series("air_ts", d.end_min + 5, d.air_f, d.end_min + 5)
      << "[SUBCATCHMENTS]\nS1 RG1 J1 5 50 500 0.5 0\n\n"
      << "[SUBAREAS]\nS1 0.01 0.1 0.02 0.02 25 OUTLET\n\n"
      << "[INFILTRATION]\nS1 3.0 0.5 4 7 0\n\n";
    if (d.lid)
        f << "[LID_CONTROLS]\nBC1 BC\n"
          << "BC1 SURFACE  0.6    0.0  0.1  100  5\n"
          << "BC1 SOIL     3.0    0.5  0.2  0.1  0.864  10.0 3.6\n"
          << "BC1 STORAGE  12.0   3.0  0.0  0\n"
          << "BC1 DRAIN    " << d.drain_coeff << " 0.5  0    0\n\n"
          << "[LID_USAGE]\nS1 BC1 1 43560 500 50 100 0\n\n";
    // A small storage node: large enough to be resolved, small enough that
    // the atmospheric flux is a live term rather than a rounding error.
    f << "[JUNCTIONS]\nJ1 10.0 10 0 0 0\n\n"
      << "[STORAGE]\nST1 8.0 4.0 0 FUNCTIONAL 200 0 0\n\n"
      << "[OUTFALLS]\nOUT 6.0 FREE  NO\n\n"
      << "[CONDUITS]\nC1 J1 ST1 400 0.013 0 0 0\n"
      << "C2 ST1 OUT 200 0.013 0 0 0\n\n"
      << "[XSECTIONS]\nC1 CIRCULAR 3.0 0 0 0\nC2 CIRCULAR 3.0 0 0 0\n\n"
      << "[PROCESS_COMPONENTS]\n";
    if (d.heat)      f << "org.hydrocouple.openswmm.heat config=\"_dtref.heat\"\n";
    if (d.water_age) f << "org.hydrocouple.openswmm.waterage config=\"_dtref.age\"\n";
    f << "\n[REPORT]\nINPUT NO\n";
}

/// What one run publishes. Every gate picks one field; they are gathered
/// together so a single run serves whichever gate needs it.
struct Result {
    bool   ok = false;
    double subcatch_temp = 0.0;   ///< H5a — subarea flux/mix ordering
    double storage_temp  = 0.0;   ///< D-H5e — relaxation over a long step
    double lid_storage_temp = 0.0;///< H5b — conduction composition
    double lid_soil_temp = 0.0;
    double lid_storage_age = 0.0; ///< A4  — donor mix order
};

Result runAt(const Deck& d, const Level& lv) {
    write_files(d, lv);
    Result r{};
    SWMM_Engine e = swmm_engine_create();
    if (e == nullptr) return r;
    if (swmm_engine_open(e, "_dtref.inp", "_dtref.rpt", "_dtref.out",
                         nullptr) != SWMM_OK ||
        swmm_engine_initialize(e) != SWMM_OK ||
        swmm_engine_start(e, 1) != SWMM_OK) {
        swmm_engine_destroy(e);
        return r;
    }
    double elapsed = 0.0;
    int guard = 0;
    do {
        if (swmm_engine_step(e, &elapsed) != SWMM_OK) break;
    } while (elapsed > 0.0 && ++guard < 2000000);
    swmm_engine_end(e);

    const auto& ctx = as_cpp_engine(e).context();
    const auto& hs  = ctx.heat_state;
    if (!hs.subcatch_runoff_temp.empty())
        r.subcatch_temp = hs.subcatch_runoff_temp[0];
    // The storage node is index 1 (J1, ST1, OUT).
    if (hs.node_temp.size() > 1) r.storage_temp = hs.node_temp[1];
    const auto& st = ctx.lid_layer_state;
    if (st.active()) {
        r.lid_storage_temp =
            st.value[st.layer_index(0, openswmm::LidLayer::STORAGE,
                                    static_cast<int>(
                                        openswmm::LidSpecies::TEMPERATURE))];
        r.lid_soil_temp =
            st.value[st.layer_index(0, openswmm::LidLayer::SOIL,
                                    static_cast<int>(
                                        openswmm::LidSpecies::TEMPERATURE))];
        r.lid_storage_age =
            st.value[st.layer_index(0, openswmm::LidLayer::STORAGE,
                                    static_cast<int>(
                                        openswmm::LidSpecies::AGE))];
    }
    r.ok = true;
    swmm_engine_destroy(e);
    return r;
}

/**
 * @brief The shared assertion: a contracting sequence, converged by `h`.
 *
 * @param a,b,c    the observable at coarse, mid and fine steps.
 * @param spread   the deck's own source spread — the scale `band` is a
 *                 fraction of.
 * @param band     max `|a − c|` as a fraction of `spread`. **MEASURE THIS.**
 * @param what     name, for the failure message.
 */
void ExpectConverged(double a, double b, double c, double spread,
                     double band, const char* what) {
    const double d1 = std::fabs(a - b);
    const double d2 = std::fabs(b - c);
    const double total = std::fabs(a - c);

    ASSERT_TRUE(std::isfinite(a) && std::isfinite(b) && std::isfinite(c))
        << what << ": non-finite (" << a << ", " << b << ", " << c << ")";
    ASSERT_GT(spread, 1.0e-9)
        << what << ": the deck's sources span nothing, so there is no scale "
                   "to measure convergence against and this gate is vacuous";

    // SETUP — the observable must actually MOVE with the timestep. If it
    // does not, the term under test is not active on this deck and the gate
    // proves nothing (lesson 59, and lesson 96: assert the whole predicate).
    ASSERT_GT(d1 / spread, 1.0e-9)
        << what << ": the answer is identical at coarse and mid steps ("
        << a << " vs " << b << "), so this deck does not exercise a "
           "timestep-dependent term at all";

    // (1) Contraction — reference-free. A first-order scheme on a 4x ladder
    // contracts by roughly 4; anything that does not contract is not
    // converging and no band below is meaningful.
    EXPECT_LT(d2, d1)
        << what << ": refining made the answer move MORE (" << d2
        << " vs " << d1 << "). The sequence is not converging, so the "
           "scheme is not consistent with the ODE it claims";

    // (2) Already close at the coarse step, measured against the deck's own
    // scale. This is the leg a splitting or mix-order defect fails: it stays
    // first-order and so still passes (1), but its error COEFFICIENT is
    // several times larger.
    // The measurement is the point of this file, so it is reported on every
    // run rather than only on failure: re-establishing a band after a change
    // to the schemes should not require editing the gate to see the numbers.
    std::printf("[ dt-ref   ] %-44s coarse %.10g  mid %.10g  fine %.10g  "
                "| ratio %.3g  err/spread %.6g  (band %.6g)\n",
                what, a, b, c, (d2 > 0.0 ? d1 / d2 : 0.0), total / spread,
                band);

    EXPECT_LT(total / spread, band)
        << what << ": |coarse - fine| = " << total << " over a source "
        << "spread of " << spread << " = " << (total / spread)
        << " of scale, against a band of " << band << ". Coarse " << a
        << ", mid " << b << ", fine " << c;
}

}  // namespace

// ---------------------------------------------------------------------------
// Gate 1 — A4 falsifier iii. Mix ORDER in the LID column: the donor's old
//          value, not its new one. A one-step error, so it shows up as a
//          `dt`-proportional shift in the age the column carries.
//
//          The observable is the STORAGE layer's own age, not the
//          subcatchment's runoff age. Two hops from the source, so the
//          ordering error compounds; and the subcatchment's age is mostly
//          the 80% of the area that is not LID at all — the defect moved it
//          by 5 s in 7440, which is a separation of 1.00x.
//
//          No underdrain, so the storage layer fills and stays filled. That
//          is what removes the LID solver's step sensitivity from the
//          comparison and leaves the mix order as the term under test; see
//          the file warning.
// ---------------------------------------------------------------------------
TEST(TransportDtReferenceTest, LidColumnAgeConvergesUnderRefinement) {
    Deck d{};
    d.water_age = true;
    d.lid = true;
    d.drain_coeff = 0.0;
    d.rain_h = 4.0;
    d.init_h = 0.0;

    const auto a = runAt(d, kCoarse);
    const auto b = runAt(d, kMid);
    const auto c = runAt(d, kFine);
    ASSERT_TRUE(a.ok && b.ok && c.ok) << "a refinement level failed to run";

    ExpectConverged(a.lid_storage_age, b.lid_storage_age, c.lid_storage_age,
                    sourceSpread(d, false), 0.0080,
                    "LID storage age (A4 falsifier iii)");
}

// ---------------------------------------------------------------------------
// Gate 2 — H5a falsifier vi. The surface balance applied to the water that
//          was ALREADY there, not to the post-mix volume. Using `v_old +
//          v_in` heats a step's rain before it arrived — an error of order
//          `dt` in the mixing volume.
// ---------------------------------------------------------------------------
TEST(TransportDtReferenceTest, SubareaTemperatureConvergesUnderRefinement) {
    Deck d{};
    d.heat = true;
    d.surface = true;
    d.rain_c = 5.0;
    d.init_c = 30.0;
    d.air_f = 86.0;

    const auto a = runAt(d, kCoarse);
    const auto b = runAt(d, kMid);
    const auto c = runAt(d, kFine);
    ASSERT_TRUE(a.ok && b.ok && c.ok);

    ExpectConverged(a.subcatch_temp, b.subcatch_temp, c.subcatch_temp,
                    sourceSpread(d, true), 0.014,
                    "subarea runoff temperature (H5a falsifier vi)");
}

// ---------------------------------------------------------------------------
// Gate 3 — D-H5e's linearization caveat. `relaxT` linearizes `J` about the
//          CURRENT temperature; over a long step the true `J` moves along
//          the path. Both flux families on, so the composition is exercised
//          as well as the linearization.
//
//          NOT independent of gate 2: this node's water is the subcatchment's
//          runoff, so H5a's post-mix-volume defect reaches it too and trips
//          this gate at 0.0433. A failure here narrows the cause to the
//          relaxation OR the subarea mixing volume, not to one of them.
// ---------------------------------------------------------------------------
TEST(TransportDtReferenceTest, StorageNodeTemperatureConvergesUnderRefinement) {
    Deck d{};
    d.heat = true;
    d.surface = true;
    d.radiative = true;      // both families: the composition D-H5e merged
    d.rain_c = 5.0;
    d.init_c = 30.0;

    const auto a = runAt(d, kCoarse);
    const auto b = runAt(d, kMid);
    const auto c = runAt(d, kFine);
    ASSERT_TRUE(a.ok && b.ok && c.ok);

    ExpectConverged(a.storage_temp, b.storage_temp, c.storage_temp,
                    sourceSpread(d, true), 0.030,
                    "storage node temperature (D-H5e linearization)");
}

// ---------------------------------------------------------------------------
// Gate 4 — H5b falsifier ii. Conduction inside `J(T)` rather than as a
//          separate pass. H5b's round established this is the ONLY way to
//          discriminate: the two compositions share a fixed point and are
//          both first-order consistent, so nothing at a single `dt` can
//          separate them.
// ---------------------------------------------------------------------------
TEST(TransportDtReferenceTest, LidColumnTemperatureConvergesUnderRefinement) {
    Deck d{};
    d.heat = true;
    d.lid = true;
    d.conduction = true;
    d.surface = true;        // BOTH operators live, or there is nothing to
    d.rain_c = 5.0;          // compose and the splitting error is zero
    d.init_c = 30.0;
    // A working underdrain, but slow enough that the storage layer stays
    // two thirds full. Sealing it as gate 1 does leaves the soil leg barely
    // contracting (1.40, and 0.93 on a 60-minute deck); emptying it puts
    // both legs on the noise floor. See the file warning.
    d.drain_coeff = 1.24708;

    const auto a = runAt(d, kCoarse);
    const auto b = runAt(d, kMid);
    const auto c = runAt(d, kFine);
    ASSERT_TRUE(a.ok && b.ok && c.ok);

    // Bands re-pinned 2026-08-30 (LID fix round). The originals were fitted
    // while (a) the solver read the deck's LID parameters unconverted
    // (issue #131 / PR #103 — inches as feet, in/hr as ft/s) and (b) a
    // target-less underdrain recirculated onto its own subcatchment. Both
    // are gone; measured floors on the corrected engine: age 0.00644 at a
    // contraction ratio of 2.77, storage temperature 0.00762 at 1.89, soil
    // 0.0196 at 1.93. The temperature legs contract at ~1.9 on a 4x ladder
    // — closer to first order than the age leg — which is recorded as a
    // finding about the conduction/advection composition, not absorbed here.
    ExpectConverged(a.lid_storage_temp, b.lid_storage_temp,
                    c.lid_storage_temp, sourceSpread(d, true), 0.0095,
                    "LID storage temperature (H5b falsifier ii)");
    ExpectConverged(a.lid_soil_temp, b.lid_soil_temp, c.lid_soil_temp,
                    sourceSpread(d, true), 0.023,
                    "LID soil temperature (H5b falsifier ii)");
}
