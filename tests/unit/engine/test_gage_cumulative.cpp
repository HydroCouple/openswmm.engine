/**
 * @file test_gage_cumulative.cpp
 * @brief CUMULATIVE rain-gage format — issue #158 regression guard.
 *
 * @details A CUMULATIVE gage records a running depth total; the intensity for
 *          a record is the rise since the previous record spread over the
 *          gage's RECORDING INTERVAL, and it must be applied for the whole of
 *          that interval (legacy gage.c convertRainfall + gage_setState).
 *
 *          Through 6.0.0-alpha.3 the gage was read as a step-function lookup
 *          rather than legacy's state machine, which broke both halves of that
 *          sentence and produced the two symptoms reported in issue #158 on
 *          the deck reproduced here (`cumulative_ramp.inp`, an hourly counter
 *          rising 1 in/hr against a 1:00 interval — a constant 1 in/hr storm):
 *
 *          1. **Spiking.** The intensity was applied for ONE runoff step at
 *             each record instead of across the interval, so the subcatchment
 *             produced a separate rise-and-recess at every table entry instead
 *             of one continuous hydrograph.
 *          2. **Total rainfall short by the interval/step ratio.** Because the
 *             rain lasted one wet step (300 s) out of each 3600 s interval, the
 *             run booked 0.75 in where legacy books 9.00 in — the reported
 *             "factor of 12" (3600/300; the same deck at a 0:15 interval was
 *             short by 3).
 *
 *          Fixed by 704ca917 (the legacy rain-gage state machine). These tests
 *          pin the behaviour analytically AND against the legacy engine, which
 *          is linked into this binary.
 *
 * @see src/engine/hydrology/Gage.cpp (recordRate / setGageState / initGageState)
 * @see Legacy parity: src/legacy/engine/gage.c (convertRainfall, gage_setState)
 * @ingroup engine_hydrology
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_gages.h>
#include <openswmm/engine/openswmm_subcatchments.h>
#include <openswmm/legacy/engine/openswmm_solver.h>

namespace {

/// One runoff step: elapsed time and what the gage and subcatchment read there.
struct Sample {
    double t;       ///< elapsed time at the end of the step (days)
    double rain;    ///< gage intensity, project rain units (in/hr)
    double runoff;  ///< subcatchment runoff, project flow units (cfs)
};

constexpr double kSecPerDay = 86400.0;
constexpr double kHrPerDay  = 24.0;

/// The gage's first record is a zero counter at t=0, so nothing falls until
/// the 01:00 record. Samples at or before that instant are outside the storm.
constexpr double kStormStartSec = 3600.0;

std::string deckPath(const char* name) {
    return std::string("rain_cumulative/") + name + ".inp";
}

/// Run a deck through the NEW engine, sampling gage 0 / subcatchment 0.
std::vector<Sample> runNew(const char* name) {
    std::vector<Sample> out;
    const std::string inp = deckPath(name);
    const std::string rpt = std::string("_cum_") + name + "_new.rpt";
    const std::string bin = std::string("_cum_") + name + "_new.out";

    SWMM_Engine e = swmm_engine_create();
    EXPECT_NE(e, nullptr);
    EXPECT_EQ(swmm_engine_open(e, inp.c_str(), rpt.c_str(), bin.c_str(), nullptr), SWMM_OK);
    EXPECT_EQ(swmm_engine_initialize(e), SWMM_OK);
    EXPECT_EQ(swmm_engine_start(e, 1), SWMM_OK);

    double t = 0.0;
    while (swmm_engine_step(e, &t) == SWMM_OK && t > 0.0) {
        double rain = 0.0, runoff = 0.0;
        EXPECT_EQ(swmm_gage_get_rainfall(e, 0, &rain), SWMM_OK);
        EXPECT_EQ(swmm_subcatch_get_runoff(e, 0, &runoff), SWMM_OK);
        out.push_back({t, rain, runoff});
    }

    EXPECT_EQ(swmm_engine_end(e), SWMM_OK);
    swmm_engine_close(e);
    swmm_engine_destroy(e);
    return out;
}

/// Run the same deck through the LEGACY engine (a process-wide singleton).
std::vector<Sample> runLegacy(const char* name) {
    std::vector<Sample> out;
    const std::string inp = deckPath(name);
    const std::string rpt = std::string("_cum_") + name + "_legacy.rpt";
    const std::string bin = std::string("_cum_") + name + "_legacy.out";

    EXPECT_EQ(swmm_open(inp.c_str(), rpt.c_str(), bin.c_str()), 0);
    EXPECT_EQ(swmm_start(1), 0);

    double t = 0.0;
    while (swmm_step(&t) == 0 && t > 0.0) {
        out.push_back({t,
                       swmm_getValue(swmm_GAGE_RAINFALL, 0),
                       swmm_getValue(swmm_SUBCATCH_RUNOFF, 0)});
    }

    EXPECT_EQ(swmm_end(), 0);
    swmm_close();
    return out;
}

/// Rain depth (in) delivered over the run, integrating intensity over each
/// step. This is the quantity the continuity report calls Total Precipitation.
double integratedDepth(const std::vector<Sample>& s) {
    double depth = 0.0, prev = 0.0;
    for (const Sample& k : s) {
        depth += k.rain * (k.t - prev) * kHrPerDay;
        prev = k.t;
    }
    return depth;
}

} // namespace

// ===========================================================================
// Symptom 1 — the hydrograph must be continuous, not a spike per table entry
// ===========================================================================

TEST(GageCumulative, RainFallsOnEveryStepBetweenRecords) {
    const std::vector<Sample> s = runNew("cumulative_ramp");
    ASSERT_FALSE(s.empty());

    int wet = 0, dry_inside_storm = 0;
    for (const Sample& k : s) {
        if (k.t * kSecPerDay <= kStormStartSec + 0.5) continue;
        if (k.rain > 0.0) ++wet;
        else              ++dry_inside_storm;
    }

    EXPECT_GT(wet, 0) << "the gage never rained at all";
    EXPECT_EQ(dry_inside_storm, 0)
        << "a cumulative record's depth must be spread across every step of "
           "its recording interval; a dry step inside the storm is the issue "
           "#158 spike, where the rain lasted one step per table entry";
}

TEST(GageCumulative, RampHoldsOneInchPerHourThroughout) {
    // The counter rises exactly 1 in per 1:00 interval, so every step inside
    // the storm must read the same 1 in/hr — no per-record transient.
    const std::vector<Sample> s = runNew("cumulative_ramp");
    ASSERT_FALSE(s.empty());

    for (const Sample& k : s) {
        if (k.t * kSecPerDay <= kStormStartSec + 0.5) continue;
        EXPECT_NEAR(k.rain, 1.0, 1e-9) << "at elapsed " << k.t << " d";
    }
}

// ===========================================================================
// Symptom 2 — the interval's whole depth is delivered, not one step of it
// ===========================================================================

TEST(GageCumulative, DeliversTheWholeDepthNotOneStepPerInterval) {
    // 1 in/hr from 01:00 to the 10:00 end of the run. Applying the intensity
    // for one 300 s wet step per 3600 s interval instead booked 0.75 in — the
    // "off by a factor of 12" in issue #158.
    EXPECT_NEAR(integratedDepth(runNew("cumulative_ramp")), 9.0, 1e-6);
}

TEST(GageCumulative, ZeroRiseInTheCounterIsADrySpell) {
    // 0,1,2,2,2,3,4,4,4,5,6 — four records repeat the previous total. Those
    // are gaps, not rain, and the last record's interval starts at the end of
    // the run: 5 in falls, not 6.
    EXPECT_NEAR(integratedDepth(runNew("cumulative_flat")), 5.0, 1e-6);
}

TEST(GageCumulative, ADropInTheCounterRestartsTheAccumulator) {
    // 0,1,2,3,0.5,... — legacy convertRainfall treats a decrease as a reset
    // gauge, and the new value is then the whole interval's depth (0.5 in),
    // not a negative rise. Total 8.5 in.
    EXPECT_NEAR(integratedDepth(runNew("cumulative_reset")), 8.5, 1e-6);
}

// ===========================================================================
// Legacy parity — same steps, same intensity, same hydrograph
// ===========================================================================

class GageCumulativeParity : public ::testing::TestWithParam<const char*> {};

TEST_P(GageCumulativeParity, MatchesLegacyStepForStep) {
    const char* deck = GetParam();
    const std::vector<Sample> mine   = runNew(deck);
    const std::vector<Sample> legacy = runLegacy(deck);

    ASSERT_FALSE(legacy.empty());
    ASSERT_FALSE(mine.empty());

    // The two step loops can differ by the final step alone: legacy swmm_step
    // reports elapsed 0 on the step that reaches the end of the run, where the
    // new engine reports that step's own end time and zeroes on the next call.
    // That is an end-of-run reporting convention, not a gage difference — the
    // step BOUNDARIES below are compared exactly, so a genuinely divergent
    // next-rain-date limit still fails here.
    const std::size_t n = std::min(mine.size(), legacy.size());
    const std::size_t extra =
        std::max(mine.size(), legacy.size()) - n;
    EXPECT_LE(extra, 1u)
        << "the two engines took a different number of runoff steps — the "
           "gage's next-rain-date limit disagrees with legacy "
           "gage_getNextRainDate (new=" << mine.size()
        << " legacy=" << legacy.size() << ")";

    for (std::size_t i = 0; i < n; ++i) {
        EXPECT_NEAR(mine[i].t, legacy[i].t, 1e-12) << "step " << i;
        EXPECT_NEAR(mine[i].rain, legacy[i].rain, 1e-9) << "step " << i;
        EXPECT_NEAR(mine[i].runoff, legacy[i].runoff, 1e-9) << "step " << i;
    }
}

INSTANTIATE_TEST_SUITE_P(Issue158, GageCumulativeParity,
                         ::testing::Values("cumulative_ramp",
                                           "cumulative_flat",
                                           "cumulative_reset"),
                         [](const ::testing::TestParamInfo<const char*>& i) {
                             return std::string(i.param);
                         });
