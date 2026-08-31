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
 * @file test_heat_solar_radiation.cpp
 * @brief Phase H6a gates — solar position, Bird clear-sky, cloud, parser.
 *
 * @par What each class of gate is worth — read before trusting a green run
 *      These gates are NOT all equally strong, and the difference is the
 *      whole reason D-H6a-4 was amended away from NREL SPA.
 *
 *      - **ASTRONOMICAL gates** (1, 2) assert facts that exist outside any
 *        implementation: declination is +23.44° at the June solstice and
 *        ~0° at the equinoxes; the hour angle is 0 at solar noon; air mass
 *        is 1 at the zenith and 2 at 60°. These are the strong ones. A
 *        transcription error in the Fourier coefficients moves them.
 *
 *      - **CROSS-IMPLEMENTATION gates** (3) compare this module's
 *        declination against the engine's OWN legacy fit in
 *        `Climate.cpp:180`. Two independent formulations, one tree. Note
 *        the band is deliberately LOOSE and asymmetric — see the gate.
 *
 *      - **INVARIANT gates** (4, 6, 7) assert structure rather than values:
 *        monotonicity, exact identity at C = 0, darkness below the horizon.
 *        They cannot be satisfied by a plausible-but-wrong constant.
 *
 *      - **TRANSCRIPTION gates** (5) carry Bird numbers computed from the
 *        same formulas this module implements, by a separate
 *        implementation. VERIFIED 2026-08-31 (step 3 validation round):
 *        every Bird coefficient was checked term-by-term against pvlib's
 *        NREL-faithful `clearsky.bird` — ozone exponent −0.3034, Ba = 0.85,
 *        pressure-corrected air mass on Rayleigh/gases and uncorrected on
 *        ozone/water/aerosol all match. (One knowingly kept truncation:
 *        the broadband AOD weight 0.2758, where pvlib carries 0.27583 —
 *        NREL's own Excel uses 0.2758; ~3e-5 relative.) Gate 5 may be read
 *        as a verified reference pin, not merely a self-consistency check.
 *
 * @see plans/transport/HEAT_TRANSPORT_PLAN.md §2.5, §6 H6a, §6.4
 * @see plans/transport/H6A_VALIDATION_HANDOFF_2026-08-30.md
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_heat.h>

#include "core/SWMMEngine.hpp"
#include "hydrology/Climate.hpp"
#include "transport/components/HeatFluxModules/RadiativeExchange.hpp"
#include "transport/components/HeatFluxModules/SolarRadiation.hpp"

namespace sr = openswmm::transport::heat;

namespace {

openswmm::SWMMEngine& as_cpp_engine(SWMM_Engine engine) {
    return *static_cast<openswmm::SWMMEngine*>(engine);
}

/// Day numbers of the astronomical cardinal points in a non-leap year.
constexpr int kMarEquinox = 80;   ///< ~Mar 21
constexpr int kJunSolstice = 172; ///< ~Jun 21
constexpr int kSepEquinox = 266;  ///< ~Sep 23
constexpr int kDecSolstice = 355; ///< ~Dec 21

void write_deck(const char* path, const std::string& pc_lines) {
    std::ofstream f(path);
    f << "[TITLE]\nH6a solar radiation gate deck\n\n[OPTIONS]\n"
      << "FLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\nHEAT_TRANSPORT ON\n"
      << "START_DATE 06/21/2026\nSTART_TIME 12:00:00\n"
      << "END_DATE 06/21/2026\nEND_TIME 12:05:00\n"
      << "ROUTING_STEP 10\nREPORT_STEP 00:05:00\n\n"
      << "[TEMPERATURE]\nTIMESERIES air_ts\nHUMIDITY 50.0\n\n"
      << "[TIMESERIES]\nair_ts 06/21/2026 00:00 50.0\n"
      << "air_ts 06/22/2026 00:00 50.0\n"
      << "sw_ts  06/21/2026 00:00 700.0\n"
      << "sw_ts  06/22/2026 00:00 700.0\n"
      << "cloud_ts 06/21/2026 00:00 0.5\n"
      << "cloud_ts 06/22/2026 00:00 0.5\n\n"
      << "[STORAGE]\nS1 10.0 12 6.0 FUNCTIONAL 0 0 5000\n\n"
      << "[JUNCTIONS]\nJ1 9.0 10 1.0 0 0\n\n"
      << "[OUTFALLS]\nOUT 8.0 FREE  NO\n\n"
      << "[CONDUITS]\nC1 S1 J1 500 0.013 0 0 0\nC2 J1 OUT 500 0.013 0 0 0\n\n"
      << "[XSECTIONS]\nC1 CIRCULAR 3.0 0 0 0\nC2 CIRCULAR 3.0 0 0 0\n\n";
    if (!pc_lines.empty())
        f << "[PROCESS_COMPONENTS]\n" << pc_lines << "\n\n";
    f << "[REPORT]\nINPUT NO\n";
}

void write_file(const char* path, const std::string& body) {
    std::ofstream f(path);
    f << body;
}

/// Open only — these gates are about CONFIGURATION being accepted or
/// refused, so nothing needs to run.
int open_only(const char* inp, const char* rpt, const char* out,
              SWMM_Engine* held) {
    SWMM_Engine e = swmm_engine_create();
    if (e == nullptr) return SWMM_ERR_BADHANDLE;
    const int rc = swmm_engine_open(e, inp, rpt, out, nullptr);
    if (held != nullptr && rc == SWMM_OK) { *held = e; return rc; }
    swmm_engine_destroy(e);
    return rc;
}

}  // namespace

// ---------------------------------------------------------------------------
// Gate 1 — ASTRONOMY. Declination at the cardinal points.
//
// These four values are properties of the Earth's orbit, not of any
// implementation, which is what makes this the strongest gate in the file:
// nothing about how the series is written can satisfy it by accident.
// ---------------------------------------------------------------------------
TEST(HeatSolarRadiationTest, DeclinationMatchesTheCardinalPoints) {
    // Obliquity of the ecliptic — the June solstice declination IS 23.44°.
    EXPECT_NEAR(sr::solarPosition(kJunSolstice, 12.0, 0.0, 0.0, 0.0)
                    .declination_deg,
                23.44, 0.05);
    EXPECT_NEAR(sr::solarPosition(kDecSolstice, 12.0, 0.0, 0.0, 0.0)
                    .declination_deg,
                -23.44, 0.10);
    // Equinoxes: the sun crosses the celestial equator, declination 0.
    EXPECT_NEAR(sr::solarPosition(kMarEquinox, 12.0, 0.0, 0.0, 0.0)
                    .declination_deg,
                0.0, 0.40);
    EXPECT_NEAR(sr::solarPosition(kSepEquinox, 12.0, 0.0, 0.0, 0.0)
                    .declination_deg,
                0.0, 0.40);

    // Earth is near APHELION at the June solstice, so the irradiance
    // correction is BELOW one — and near perihelion in January, above it.
    // Getting this backwards is a sign error that a symmetric test on
    // |E0 - 1| would not catch.
    EXPECT_LT(sr::solarPosition(kJunSolstice, 12.0, 0, 0, 0).eccentricity, 1.0);
    EXPECT_GT(sr::solarPosition(1, 12.0, 0, 0, 0).eccentricity, 1.0);
}

// ---------------------------------------------------------------------------
// Gate 2 — ASTRONOMY. Geometry at solar noon and through a day.
// ---------------------------------------------------------------------------
TEST(HeatSolarRadiationTest, GeometryAtSolarNoonAndThroughTheDay) {
    // At longitude 0, timezone 0, local noon is solar noon to within the
    // equation of time (|EoT| <= ~16 min => |hour angle| <= ~4 deg).
    const auto noon = sr::solarPosition(kJunSolstice, 12.0, 40.0, 0.0, 0.0);
    EXPECT_LT(std::fabs(noon.hour_angle_deg), 4.0);

    // At 40 N on the June solstice the noon zenith is |lat - decl|.
    EXPECT_NEAR(noon.zenith_deg, std::fabs(40.0 - 23.44), 0.5);

    // Midnight at the same site: sun well below the horizon, and the cosine
    // CLAMPED to 0 rather than negative — the contract every Bird term
    // depends on.
    const auto midnight = sr::solarPosition(kJunSolstice, 0.0, 40.0, 0.0, 0.0);
    EXPECT_GT(midnight.zenith_deg, 90.0);
    EXPECT_DOUBLE_EQ(midnight.cos_zenith, 0.0);

    // The longitude/timezone pair must CANCEL for a site at its zone
    // meridian. -105 deg is the exact meridian of UTC-7, so noon there is
    // solar noon to within the equation of time. This is the arithmetic
    // that goes wrong when a sign is flipped, and a flipped sign shows up
    // here as a ~14 h error, not a subtle one.
    const auto mtn = sr::solarPosition(kJunSolstice, 12.0, 40.0, -105.0, -7.0);
    EXPECT_LT(std::fabs(mtn.hour_angle_deg), 4.0);
}

// ---------------------------------------------------------------------------
// Gate 3 — CROSS-IMPLEMENTATION. This module vs the engine's legacy fit.
//
// `Climate.cpp:180` carries an INDEPENDENT declination: a single-cosine fit,
// `0.40928 cos(0.017202 (172 - doy))`, used by the snowmelt/temperature
// interpolation. Two formulations in one tree.
//
// The band is loose and the reason matters: a one-term fit is exact by
// construction at the solstices it was fitted to and WORST at the equinoxes,
// where declination moves fastest. Measured divergence peaks at ~1.5 deg
// near the September equinox — and there the THREE-term series is the
// correct one (true declination on Sep 23 is near 0; the legacy fit says
// -1.08). So this gate asserts tight agreement only where the legacy fit is
// trustworthy, and merely bounded disagreement elsewhere. Tightening it
// would be pinning this module to a known-worse reference.
// ---------------------------------------------------------------------------
TEST(HeatSolarRadiationTest, DeclinationAgreesWithTheLegacyFitWhereItIsGood) {
    const auto legacy_decl = [](int doy) {
        return 0.40928 * std::cos(0.017202 * (172.0 - doy)) * 180.0 / 3.14159265358979323846;
    };

    // At the solstices the legacy fit is at its best: agree tightly.
    for (const int doy : {kJunSolstice, kDecSolstice}) {
        EXPECT_NEAR(sr::solarPosition(doy, 12.0, 0, 0, 0).declination_deg,
                    legacy_decl(doy), 0.10)
            << "solstice doy " << doy;
    }

    // Everywhere else: bounded, and never wilder than the known 1.5 deg.
    for (int doy = 1; doy <= 365; ++doy) {
        EXPECT_LT(std::fabs(sr::solarPosition(doy, 12.0, 0, 0, 0)
                                .declination_deg -
                            legacy_decl(doy)),
                  2.0)
            << "doy " << doy;
    }

    // And at the September equinox the two genuinely disagree, with THIS
    // module the closer to truth. Asserted so that a future "fix" that
    // makes them agree there is caught: it would be a regression.
    EXPECT_LT(std::fabs(sr::solarPosition(kSepEquinox, 12.0, 0, 0, 0)
                            .declination_deg),
              std::fabs(legacy_decl(kSepEquinox)));
}

// ---------------------------------------------------------------------------
// Gate 4 — INVARIANTS of air mass and the clear-sky curve.
// ---------------------------------------------------------------------------
TEST(HeatSolarRadiationTest, AirMassAndClearSkyInvariants) {
    // Kasten–Young: 1 at the zenith, 2 at 60 deg. Textbook values.
    EXPECT_NEAR(sr::airMass(0.0),  1.0, 1.0e-3);
    EXPECT_NEAR(sr::airMass(60.0), 2.0, 1.0e-2);
    // At and below the horizon there is no path — 0, not a NaN and not a
    // negative. Every Bird term is written to collapse on this.
    EXPECT_DOUBLE_EQ(sr::airMass(90.0), 0.0);
    EXPECT_DOUBLE_EQ(sr::airMass(95.0), 0.0);

    // Standard atmosphere at sea level.
    EXPECT_NEAR(sr::pressureFromElevation(0.0), 1013.25, 1.0e-6);
    // Pressure falls with height, and 1400 m (Salt Lake City) is ~856 mb.
    EXPECT_NEAR(sr::pressureFromElevation(1400.0), 856.0, 2.0);

    openswmm::SolarConfig cfg{};  // paper's standard atmosphere

    // GHI is strictly decreasing in zenith angle across the daylit range,
    // and exactly zero once the sun sets. A constant, a sign error, or a
    // mis-set air-mass term all break one of these.
    double prev = 1.0e9;
    for (const double z : {0.0, 15.0, 30.0, 45.0, 60.0, 75.0, 85.0}) {
        openswmm::SolarConfig c = cfg;
        auto pos = sr::solarPosition(kJunSolstice, 12.0, 0, 0, 0);
        pos.zenith_deg = z;
        pos.cos_zenith = std::cos(z * 3.14159265358979323846 / 180.0);
        pos.eccentricity = 1.0;
        const double ghi = sr::birdClearSkyGHI(pos, 1013.25, c);
        EXPECT_GT(ghi, 0.0) << "z=" << z;
        EXPECT_LT(ghi, prev) << "not monotone at z=" << z;
        prev = ghi;
    }

    auto night = sr::solarPosition(kJunSolstice, 12.0, 0, 0, 0);
    night.cos_zenith = 0.0;
    night.zenith_deg = 95.0;
    EXPECT_DOUBLE_EQ(sr::birdClearSkyGHI(night, 1013.25, cfg), 0.0);

    // A clean, dry atmosphere must transmit MORE than a hazy humid one, and
    // still less than the extraterrestrial constant. The upper bound is the
    // one that catches a transmittance written as its own reciprocal.
    openswmm::SolarConfig clean = cfg;
    clean.aod380 = 0.0; clean.aod500 = 0.0;
    clean.precip_water_cm = 0.0; clean.ozone_cm = 0.0;
    auto zenith = sr::solarPosition(kJunSolstice, 12.0, 0, 0, 0);
    zenith.zenith_deg = 0.0; zenith.cos_zenith = 1.0; zenith.eccentricity = 1.0;
    const double ghi_clean = sr::birdClearSkyGHI(zenith, 1013.25, clean);
    const double ghi_std   = sr::birdClearSkyGHI(zenith, 1013.25, cfg);
    EXPECT_GT(ghi_clean, ghi_std);
    EXPECT_LT(ghi_clean, sr::kSolarConstant);

    // Thinner air transmits more: the same sun at altitude beats sea level.
    EXPECT_GT(sr::birdClearSkyGHI(zenith, sr::pressureFromElevation(2000.0), cfg),
              ghi_std);
}

// ---------------------------------------------------------------------------
// Gate 5 — TRANSCRIPTION PIN, coefficients since verified.
//
// The number below was computed by a SEPARATE implementation of the same
// formulas this module carries. The coefficients themselves were verified
// term-by-term against pvlib's NREL-faithful bird() in the step 3
// validation round (2026-08-31) — see the file header — so this now pins a
// checked transcription rather than a recalled one.
//
// It pins the composed result, so any later edit to a single transmittance
// term moves it.
// ---------------------------------------------------------------------------
TEST(HeatSolarRadiationTest, BirdStandardAtmosphereRegressionPin) {
    openswmm::SolarConfig cfg{};
    auto pos = sr::solarPosition(kJunSolstice, 12.0, 0, 0, 0);
    pos.zenith_deg = 0.0; pos.cos_zenith = 1.0; pos.eccentricity = 1.0;
    // Sea level, sun overhead, paper's standard atmosphere.
    EXPECT_NEAR(sr::birdClearSkyGHI(pos, 1013.25, cfg), 1056.09, 0.5);

    pos.zenith_deg = 60.0; pos.cos_zenith = 0.5;
    EXPECT_NEAR(sr::birdClearSkyGHI(pos, 1013.25, cfg), 476.79, 0.5);
}

// ---------------------------------------------------------------------------
// Gate 6 — THE H3 BASELINE. C = 0 must be EXACT, not approximate.
//
// Plan §2.5 makes this a verify criterion in its own right, because the
// cloud correction reaches into the RHE-gated Brunt term. `EXPECT_DOUBLE_EQ`
// rather than `EXPECT_NEAR` is the whole point: "close enough" here would
// let the H3 parity baseline drift out from under every later gate.
// ---------------------------------------------------------------------------
TEST(HeatSolarRadiationTest, ClearSkyReducesToH3Exactly) {
    // The factor itself is a literal 1.0 at and below zero cloud.
    EXPECT_DOUBLE_EQ(sr::cloudLongwaveFactor(0.0, 0.17), 1.0);
    EXPECT_DOUBLE_EQ(sr::cloudLongwaveFactor(-0.5, 0.17), 1.0);
    EXPECT_DOUBLE_EQ(sr::cloudShortwaveFactor(0.0, 0.75, 3.4), 1.0);

    // And the H3 entry points are bit-identical with it applied.
    const double e_a = 0.6159414661;
    EXPECT_DOUBLE_EQ(
        openswmm::transport::heat::atmosphericEmissivity(e_a, 0.5),
        openswmm::transport::heat::atmosphericEmissivity(
            e_a, 0.5, sr::cloudLongwaveFactor(0.0, 0.17)));

    EXPECT_DOUBLE_EQ(
        openswmm::transport::heat::atmosphericLongwave(10.0, 50.0, 0.5, 0.03, 1.0),
        openswmm::transport::heat::atmosphericLongwave(10.0, 50.0, 0.5, 0.03, 1.0,
                                                       1.0));

    // The negative sentinel on netRadiativeFluxOut means "use the config
    // constant", so the four-argument H3 call and an explicit pass of the
    // same constant must agree exactly.
    openswmm::RadiativeConfig rc{};
    rc.shortwave_wm2 = 250.0;
    EXPECT_DOUBLE_EQ(
        openswmm::transport::heat::netRadiativeFluxOut(20.0, 10.0, 50.0, rc),
        openswmm::transport::heat::netRadiativeFluxOut(20.0, 10.0, 50.0, rc,
                                                       250.0, 1.0));

    // An UNRESOLVED cache must fall back on the configured constant, not
    // read as a resolved 0. `radiativeFluxOut` passes `shortwave_now`
    // POSITIONALLY, so the sentinel is only reachable if that field's
    // default is itself negative — which is why it is -1.0 and not 0.0.
    // With a 0.0 default this assertion fails and the shortwave term
    // silently vanishes from any call landing before the step's
    // updateSolarForcing.
    EXPECT_LT(openswmm::HeatState{}.shortwave_now, 0.0);
    EXPECT_DOUBLE_EQ(
        openswmm::transport::heat::netRadiativeFluxOut(
            20.0, 10.0, 50.0, rc, openswmm::HeatState{}.shortwave_now, 1.0),
        openswmm::transport::heat::netRadiativeFluxOut(20.0, 10.0, 50.0, rc));

    // ...and `resize()` must restore that sentinel, not leave a previous
    // run's forcing behind. `clear()` gets it via whole-struct assignment;
    // `resize()` is the one that had to be told.
    openswmm::HeatState hs;
    hs.shortwave_now = 812.0;
    hs.resize(2, 1, 20.0);
    EXPECT_LT(hs.shortwave_now, 0.0);
    EXPECT_DOUBLE_EQ(hs.cloud_now, 0.0);
}

// ---------------------------------------------------------------------------
// Gate 6b — the SAME identity, through the CONTEXT path.
//
// Gate 6 works at the free-function level, where the identity is nearly
// trivial. The binding is where it can actually break: `radiativeFluxOut`
// reads a cache rather than the config, so "clear sky reduces to H3" is a
// claim about `updateSolarForcing` copying the constant through UNCHANGED.
// ---------------------------------------------------------------------------
TEST(HeatSolarRadiationTest, ConstantModeCopiesTheConfiguredValueExactly) {
    write_file("_h6a_const.heat",
               "[HEAT_SOURCES]\nINITIAL_STATE GLOBAL 20.0\n\n"
               "[HEAT_FLUXES]\nRADIATIVE_EXCHANGE ON\n\n"
               "[RADIATIVE_FLUXES]\nSHORTWAVE GLOBAL 317.5\n");
    write_deck("_h6a_const.inp",
               "org.hydrocouple.openswmm.heat config=\"_h6a_const.heat\"");

    SWMM_Engine e = nullptr;
    ASSERT_EQ(open_only("_h6a_const.inp", "_h6a_const.rpt", "_h6a_const.out",
                        &e),
              SWMM_OK);
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_engine_initialize(e), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(e, 1), SWMM_OK);
    double elapsed = 0.0;
    ASSERT_EQ(swmm_engine_step(e, &elapsed), SWMM_OK);

    auto& ctx = as_cpp_engine(e).context();
    // Bit-identical, not close: no cloud configured means the shortwave
    // factor is a literal 1.0 and the multiply is exact.
    EXPECT_DOUBLE_EQ(ctx.heat_state.shortwave_now,
                     ctx.heat_config.radiative.shortwave_wm2);
    EXPECT_DOUBLE_EQ(ctx.heat_state.shortwave_now, 317.5);
    EXPECT_DOUBLE_EQ(ctx.heat_state.cloud_now, 0.0);

    swmm_engine_end(e);
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 7 — cloud does what D-H6a-2 says: BOTH directions, right sign.
// ---------------------------------------------------------------------------
TEST(HeatSolarRadiationTest, CloudDarkensShortwaveAndWarmsLongwave) {
    // Overcast removes exactly k of the shortwave.
    EXPECT_NEAR(sr::cloudShortwaveFactor(1.0, 0.75, 3.4), 0.25, 1.0e-12);
    // Monotone in between.
    EXPECT_GT(sr::cloudShortwaveFactor(0.3, 0.75, 3.4),
              sr::cloudShortwaveFactor(0.7, 0.75, 3.4));
    // An over-unity k blocks completely; it does not invert.
    EXPECT_DOUBLE_EQ(sr::cloudShortwaveFactor(1.0, 2.0, 1.0), 0.0);
    // A fraction above 1 is treated as overcast, not extrapolated.
    EXPECT_DOUBLE_EQ(sr::cloudShortwaveFactor(5.0, 0.75, 3.4),
                     sr::cloudShortwaveFactor(1.0, 0.75, 3.4));

    // Longwave goes the OTHER way — clouds re-radiate. A sign error here is
    // the physical error D-H6a-2 exists to prevent, and it would make an
    // overcast night cool faster rather than slower.
    EXPECT_GT(sr::cloudLongwaveFactor(1.0, 0.17), 1.0);
    EXPECT_NEAR(sr::cloudLongwaveFactor(1.0, 0.17), 1.17, 1.0e-12);

    // Composed: overcast must RAISE incoming atmospheric longwave.
    const double clear = openswmm::transport::heat::atmosphericLongwave(
        10.0, 50.0, 0.5, 0.03, 1.0, sr::cloudLongwaveFactor(0.0, 0.17));
    const double overcast = openswmm::transport::heat::atmosphericLongwave(
        10.0, 50.0, 0.5, 0.03, 1.0, sr::cloudLongwaveFactor(1.0, 0.17));
    EXPECT_GT(overcast, clear);
}

// ---------------------------------------------------------------------------
// Gate 8 — D-H6a-3. The three spellings are MUTUALLY EXCLUSIVE.
// ---------------------------------------------------------------------------
TEST(HeatSolarRadiationTest, TwoShortwaveSpellingsAreRefused) {
    write_file("_h6a_dup.heat",
               "[HEAT_SOURCES]\nINITIAL_STATE GLOBAL 20.0\n\n"
               "[HEAT_FLUXES]\nRADIATIVE_EXCHANGE ON\n\n"
               "[RADIATIVE_FLUXES]\n"
               "SHORTWAVE GLOBAL 250.0\n"
               "SHORTWAVE GLOBAL COMPUTED\n");
    write_deck("_h6a_dup.inp",
               "org.hydrocouple.openswmm.heat config=\"_h6a_dup.heat\"");

    // A precedence ladder would have ACCEPTED this deck and silently run
    // one of the two. Refusing is the decision.
    EXPECT_NE(open_only("_h6a_dup.inp", "_h6a_dup.rpt", "_h6a_dup.out", nullptr),
              SWMM_OK);
}

// ---------------------------------------------------------------------------
// Gate 9 — plan §2.5 trap 1. COMPUTED without coordinates is an ERROR.
//
// The failure this prevents is not a crash: it is a model that runs and
// reports equatorial noon. Nothing downstream could catch that.
// ---------------------------------------------------------------------------
TEST(HeatSolarRadiationTest, ComputedWithoutCoordinatesIsRefused) {
    write_file("_h6a_nolat.heat",
               "[HEAT_SOURCES]\nINITIAL_STATE GLOBAL 20.0\n\n"
               "[HEAT_FLUXES]\nRADIATIVE_EXCHANGE ON\n\n"
               "[RADIATIVE_FLUXES]\nSHORTWAVE GLOBAL COMPUTED\n");
    write_deck("_h6a_nolat.inp",
               "org.hydrocouple.openswmm.heat config=\"_h6a_nolat.heat\"");
    EXPECT_NE(open_only("_h6a_nolat.inp", "_h6a_nolat.rpt", "_h6a_nolat.out",
                        nullptr),
              SWMM_OK);

    // Latitude alone is still not sited — longitude is equally required.
    write_file("_h6a_latonly.heat",
               "[HEAT_SOURCES]\nINITIAL_STATE GLOBAL 20.0\n\n"
               "[HEAT_FLUXES]\nRADIATIVE_EXCHANGE ON\n\n"
               "[RADIATIVE_FLUXES]\nSHORTWAVE GLOBAL COMPUTED\n\n"
               "[SOLAR_RADIATION]\nLATITUDE GLOBAL 41.7\n");
    write_deck("_h6a_latonly.inp",
               "org.hydrocouple.openswmm.heat config=\"_h6a_latonly.heat\"");
    EXPECT_NE(open_only("_h6a_latonly.inp", "_h6a_latonly.rpt",
                        "_h6a_latonly.out", nullptr),
              SWMM_OK);

    // Both present: accepted.
    write_file("_h6a_sited.heat",
               "[HEAT_SOURCES]\nINITIAL_STATE GLOBAL 20.0\n\n"
               "[HEAT_FLUXES]\nRADIATIVE_EXCHANGE ON\n\n"
               "[RADIATIVE_FLUXES]\nSHORTWAVE GLOBAL COMPUTED\n\n"
               "[SOLAR_RADIATION]\nLATITUDE GLOBAL 41.7\n"
               "LONGITUDE GLOBAL -111.8\nTIMEZONE GLOBAL -7.0\n");
    write_deck("_h6a_sited.inp",
               "org.hydrocouple.openswmm.heat config=\"_h6a_sited.heat\"");
    SWMM_Engine e = nullptr;
    ASSERT_EQ(open_only("_h6a_sited.inp", "_h6a_sited.rpt", "_h6a_sited.out",
                        &e),
              SWMM_OK);
    ASSERT_NE(e, nullptr);

    int sited = 0;
    EXPECT_EQ(swmm_heat_get_solar_sited(e, &sited), SWMM_OK);
    EXPECT_EQ(sited, 1);
    int mode = -1;
    EXPECT_EQ(swmm_heat_get_shortwave_mode(e, &mode), SWMM_OK);
    EXPECT_EQ(mode, SWMM_HEAT_SW_COMPUTED);
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 10 — the C API refuses exactly what the parser refuses.
//
// Two entry points into one configuration that disagree about what is legal
// is how a deck and a GUI come to describe different models.
// ---------------------------------------------------------------------------
TEST(HeatSolarRadiationTest, ApiEnforcesTheSameRulesAsTheParser) {
    write_file("_h6a_api.heat",
               "[HEAT_SOURCES]\nINITIAL_STATE GLOBAL 20.0\n\n"
               "[HEAT_FLUXES]\nRADIATIVE_EXCHANGE ON\n\n"
               "[RADIATIVE_FLUXES]\nSHORTWAVE GLOBAL 250.0\n");
    write_deck("_h6a_api.inp", "org.hydrocouple.openswmm.heat config=\"_h6a_api.heat\"");
    SWMM_Engine e = nullptr;
    ASSERT_EQ(open_only("_h6a_api.inp", "_h6a_api.rpt", "_h6a_api.out", &e),
              SWMM_OK);
    ASSERT_NE(e, nullptr);

    // COMPUTED is refused until the site is set — the same rule, same reason.
    EXPECT_EQ(swmm_heat_set_shortwave_mode(e, SWMM_HEAT_SW_COMPUTED),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_heat_set_solar(e, SWMM_HEAT_SOLAR_LATITUDE, 41.7), SWMM_OK);
    EXPECT_EQ(swmm_heat_set_shortwave_mode(e, SWMM_HEAT_SW_COMPUTED),
              SWMM_ERR_BADPARAM);  // longitude still missing
    EXPECT_EQ(swmm_heat_set_solar(e, SWMM_HEAT_SOLAR_LONGITUDE, -111.8),
              SWMM_OK);
    EXPECT_EQ(swmm_heat_set_shortwave_mode(e, SWMM_HEAT_SW_COMPUTED), SWMM_OK);

    // Out-of-range values are REFUSED, not clamped.
    EXPECT_EQ(swmm_heat_set_solar(e, SWMM_HEAT_SOLAR_LATITUDE, 100.0),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_heat_set_cloud(e, SWMM_HEAT_CLOUD_FRACTION, 75.0),
              SWMM_ERR_BADPARAM);   // 75% typed as a fraction
    EXPECT_EQ(swmm_heat_set_radiative(e, SWMM_HEAT_RAD_ALBEDO, 97.0),
              SWMM_ERR_BADPARAM);

    // ...and a refused write must not have happened. This is the half-apply
    // check: an API that errors AND mutates is worse than one that errors.
    double lat = 0.0;
    EXPECT_EQ(swmm_heat_get_solar(e, SWMM_HEAT_SOLAR_LATITUDE, &lat), SWMM_OK);
    EXPECT_DOUBLE_EQ(lat, 41.7);

    // A shortwave constant is meaningless outside CONSTANT mode, and the
    // refusal must likewise leave the stored value alone.
    double sw_before = 0.0;
    EXPECT_EQ(swmm_heat_get_radiative(e, SWMM_HEAT_RAD_SHORTWAVE, &sw_before),
              SWMM_OK);
    EXPECT_EQ(swmm_heat_set_radiative(e, SWMM_HEAT_RAD_SHORTWAVE, 900.0),
              SWMM_ERR_BADPARAM);
    double sw_after = 0.0;
    EXPECT_EQ(swmm_heat_get_radiative(e, SWMM_HEAT_RAD_SHORTWAVE, &sw_after),
              SWMM_OK);
    EXPECT_DOUBLE_EQ(sw_before, sw_after);

    // Clearing cloud restores the documented defaults wholesale, so "clear"
    // means "no section" rather than "the section with the last k typed".
    EXPECT_EQ(swmm_heat_set_cloud(e, SWMM_HEAT_CLOUD_LW_CLOUD_K, 0.9), SWMM_OK);
    EXPECT_EQ(swmm_heat_clear_cloud(e), SWMM_OK);
    int configured = 1;
    EXPECT_EQ(swmm_heat_get_cloud_configured(e, &configured), SWMM_OK);
    EXPECT_EQ(configured, 0);
    double k = 0.0;
    EXPECT_EQ(swmm_heat_get_cloud(e, SWMM_HEAT_CLOUD_LW_CLOUD_K, &k), SWMM_OK);
    EXPECT_DOUBLE_EQ(k, 0.17);

    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 11 — the TIMESERIES spelling actually reaches the flux.
//
// A mode that parses but never reaches a number is the silent bypass
// lessons 10/20 name; this gate is what makes the wiring observable.
// ---------------------------------------------------------------------------
TEST(HeatSolarRadiationTest, ShortwaveTimeseriesReachesTheResolvedForcing) {
    write_file("_h6a_ts.heat",
               "[HEAT_SOURCES]\nINITIAL_STATE GLOBAL 20.0\n\n"
               "[HEAT_FLUXES]\nRADIATIVE_EXCHANGE ON\n\n"
               "[RADIATIVE_FLUXES]\nSHORTWAVE GLOBAL TIMESERIES sw_ts\n");
    write_deck("_h6a_ts.inp", "org.hydrocouple.openswmm.heat config=\"_h6a_ts.heat\"");

    SWMM_Engine e = nullptr;
    ASSERT_EQ(open_only("_h6a_ts.inp", "_h6a_ts.rpt", "_h6a_ts.out", &e),
              SWMM_OK);
    ASSERT_NE(e, nullptr);

    int mode = -1;
    EXPECT_EQ(swmm_heat_get_shortwave_mode(e, &mode), SWMM_OK);
    EXPECT_EQ(mode, SWMM_HEAT_SW_TIMESERIES);

    ASSERT_EQ(swmm_engine_initialize(e), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(e, 1), SWMM_OK);
    double elapsed = 0.0;
    ASSERT_EQ(swmm_engine_step(e, &elapsed), SWMM_OK);

    // The series is a flat 700 W/m2 across the run window.
    double wm2 = -1.0;
    EXPECT_EQ(swmm_heat_get_current_shortwave(e, &wm2), SWMM_OK);
    EXPECT_NEAR(wm2, 700.0, 1.0e-6);

    swmm_engine_end(e);
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 12 — an unnamed timeseries is refused, not silently zeroed.
// ---------------------------------------------------------------------------
TEST(HeatSolarRadiationTest, MissingTimeseriesIsRefused) {
    write_file("_h6a_missing.heat",
               "[HEAT_SOURCES]\nINITIAL_STATE GLOBAL 20.0\n\n"
               "[HEAT_FLUXES]\nRADIATIVE_EXCHANGE ON\n\n"
               "[RADIATIVE_FLUXES]\nSHORTWAVE GLOBAL TIMESERIES no_such_ts\n");
    write_deck("_h6a_missing.inp",
               "org.hydrocouple.openswmm.heat config=\"_h6a_missing.heat\"");
    EXPECT_NE(open_only("_h6a_missing.inp", "_h6a_missing.rpt",
                        "_h6a_missing.out", nullptr),
              SWMM_OK);
}

// ---------------------------------------------------------------------------
// Gate 13 — NaN does not walk through the range guards.
//
// `std::strtod` accepts "nan", and every range test in the parser is
// `v < lo || v > hi` — BOTH false for NaN. Without an explicit finiteness
// check a NaN latitude is stored, no error is raised, and the model runs
// producing NaN irradiance. Found by review, not by a failing gate.
// ---------------------------------------------------------------------------
TEST(HeatSolarRadiationTest, NonFiniteValuesAreRefused) {
    for (const char* bad : {"nan", "inf", "-inf"}) {
        const std::string body =
            std::string("[HEAT_SOURCES]\nINITIAL_STATE GLOBAL 20.0\n\n"
                        "[HEAT_FLUXES]\nRADIATIVE_EXCHANGE ON\n\n"
                        "[RADIATIVE_FLUXES]\nSHORTWAVE GLOBAL COMPUTED\n\n"
                        "[SOLAR_RADIATION]\nLATITUDE GLOBAL ") + bad +
            "\nLONGITUDE GLOBAL -111.8\n";
        write_file("_h6a_nan.heat", body);
        write_deck("_h6a_nan.inp", "org.hydrocouple.openswmm.heat config=\"_h6a_nan.heat\"");
        EXPECT_NE(open_only("_h6a_nan.inp", "_h6a_nan.rpt", "_h6a_nan.out",
                            nullptr),
                  SWMM_OK)
            << "accepted LATITUDE " << bad;
    }

    // The PRE-EXISTING [RADIATIVE_FLUXES] ladder had the same hole through
    // its reject-form frac() guard (`v < 0 || v > 1`, both false for NaN),
    // while the C API's frac_ok refuses NaN — a deck/API disagreement in
    // exactly the contract gate 10 defends. Closed in the step 3 validation
    // round; this leg is what keeps it closed.
    for (const char* bad : {"nan", "inf"}) {
        const std::string body =
            std::string("[HEAT_SOURCES]\nINITIAL_STATE GLOBAL 20.0\n\n"
                        "[HEAT_FLUXES]\nRADIATIVE_EXCHANGE ON\n\n"
                        "[RADIATIVE_FLUXES]\nALBEDO GLOBAL ") + bad + "\n";
        write_file("_h6a_nan.heat", body);
        write_deck("_h6a_nan.inp", "org.hydrocouple.openswmm.heat config=\"_h6a_nan.heat\"");
        EXPECT_NE(open_only("_h6a_nan.inp", "_h6a_nan.rpt", "_h6a_nan.out",
                            nullptr),
                  SWMM_OK)
            << "accepted ALBEDO " << bad;
    }
}

// ---------------------------------------------------------------------------
// Gate 13b — the validation handoff §5's adversarial decks, each aimed at a
// specific silent-success (added in the step 3 validation round).
// ---------------------------------------------------------------------------
TEST(HeatSolarRadiationTest, AdversarialDecksRefuseOrWarnAsSpecified) {
    // FRACTION 75 typed for 75% must ERROR, not blacken the sky.
    write_file("_h6a_adv.heat",
               "[HEAT_FLUXES]\nRADIATIVE_EXCHANGE ON\n\n"
               "[RADIATIVE_FLUXES]\nSHORTWAVE GLOBAL 250.0\n\n"
               "[CLOUD_COVER]\nFRACTION GLOBAL 75\n");
    write_deck("_h6a_adv.inp", "org.hydrocouple.openswmm.heat config=\"_h6a_adv.heat\"");
    EXPECT_NE(open_only("_h6a_adv.inp", "_h6a_adv.rpt", "_h6a_adv.out",
                        nullptr), SWMM_OK) << "FRACTION 75 accepted";

    // LATITUDE 100 must ERROR, not saturate to the pole.
    write_file("_h6a_adv.heat",
               "[HEAT_FLUXES]\nRADIATIVE_EXCHANGE ON\n\n"
               "[RADIATIVE_FLUXES]\nSHORTWAVE GLOBAL COMPUTED\n\n"
               "[SOLAR_RADIATION]\nLATITUDE GLOBAL 100\n"
               "LONGITUDE GLOBAL -111.8\n");
    write_deck("_h6a_adv.inp", "org.hydrocouple.openswmm.heat config=\"_h6a_adv.heat\"");
    EXPECT_NE(open_only("_h6a_adv.inp", "_h6a_adv.rpt", "_h6a_adv.out",
                        nullptr), SWMM_OK) << "LATITUDE 100 accepted";

    // A measured series with [CLOUD_COVER] on top double-counts: WARN, and
    // still run — a user may be scaling a clear-sky-corrected record.
    write_file("_h6a_adv.heat",
               "[HEAT_FLUXES]\nRADIATIVE_EXCHANGE ON\n\n"
               "[RADIATIVE_FLUXES]\nSHORTWAVE GLOBAL TIMESERIES sw_ts\n\n"
               "[CLOUD_COVER]\nFRACTION GLOBAL 0.4\n");
    write_deck("_h6a_adv.inp", "org.hydrocouple.openswmm.heat config=\"_h6a_adv.heat\"");
    {
        SWMM_Engine e = nullptr;
        ASSERT_EQ(open_only("_h6a_adv.inp", "_h6a_adv.rpt", "_h6a_adv.out",
                            &e), SWMM_OK);
        bool warned = false;
        for (const auto& w : as_cpp_engine(e).context().warnings)
            if (w.find("attenuates them twice") != std::string::npos)
                warned = true;
        EXPECT_TRUE(warned) << "no double-counting warning";
        swmm_engine_destroy(e);
    }

    // [SOLAR_RADIATION] with the mode not COMPUTED: coordinates are unused —
    // WARN, and still run.
    write_file("_h6a_adv.heat",
               "[HEAT_FLUXES]\nRADIATIVE_EXCHANGE ON\n\n"
               "[RADIATIVE_FLUXES]\nSHORTWAVE GLOBAL 250.0\n\n"
               "[SOLAR_RADIATION]\nLATITUDE GLOBAL 41.7\n"
               "LONGITUDE GLOBAL -111.8\n");
    write_deck("_h6a_adv.inp", "org.hydrocouple.openswmm.heat config=\"_h6a_adv.heat\"");
    {
        SWMM_Engine e = nullptr;
        ASSERT_EQ(open_only("_h6a_adv.inp", "_h6a_adv.rpt", "_h6a_adv.out",
                            &e), SWMM_OK);
        bool warned = false;
        for (const auto& w : as_cpp_engine(e).context().warnings)
            if (w.find("coordinates are") != std::string::npos ||
                w.find("SHORTWAVE is not COMPUTED") != std::string::npos)
                warned = true;
        EXPECT_TRUE(warned) << "no unused-coordinates warning";
        swmm_engine_destroy(e);
    }

    // A cloud SERIES carrying 1.7 arrives mid-run with no user to ask:
    // clamped to [0,1] at runtime, run continues. (A config FRACTION of the
    // same value is refused at parse — the first leg above.)
    write_file("_h6a_adv.heat",
               "[HEAT_FLUXES]\nRADIATIVE_EXCHANGE ON\n\n"
               "[RADIATIVE_FLUXES]\nSHORTWAVE GLOBAL 250.0\n\n"
               "[CLOUD_COVER]\nFRACTION GLOBAL TIMESERIES cloud_hot\n");
    {
        // The deck writer only carries cloud_ts at 0.5; add a hot series.
        std::ofstream f("_h6a_adv.inp");
        f << "[TITLE]\nH6a adversarial cloud series\n\n[OPTIONS]\n"
          << "FLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\nHEAT_TRANSPORT ON\n"
          << "START_DATE 06/21/2026\nSTART_TIME 12:00:00\n"
          << "END_DATE 06/21/2026\nEND_TIME 12:05:00\n"
          << "ROUTING_STEP 10\nREPORT_STEP 00:05:00\n\n"
          << "[TEMPERATURE]\nTIMESERIES air_ts\nHUMIDITY 50.0\n\n"
          << "[TIMESERIES]\nair_ts 06/21/2026 00:00 50.0\n"
          << "air_ts 06/22/2026 00:00 50.0\n"
          << "cloud_hot 06/21/2026 00:00 1.7\n"
          << "cloud_hot 06/22/2026 00:00 1.7\n\n"
          << "[JUNCTIONS]\nJ1 9.0 10 1.0 0 0\n\n[OUTFALLS]\nOUT 8.0 FREE NO\n\n"
          << "[CONDUITS]\nC2 J1 OUT 500 0.013 0 0 0\n\n"
          << "[XSECTIONS]\nC2 CIRCULAR 3.0 0 0 0\n\n"
          << "[PROCESS_COMPONENTS]\norg.hydrocouple.openswmm.heat "
             "config=\"_h6a_adv.heat\"\n\n[REPORT]\nINPUT NO\n";
    }
    {
        SWMM_Engine e = nullptr;
        ASSERT_EQ(open_only("_h6a_adv.inp", "_h6a_adv.rpt", "_h6a_adv.out",
                            &e), SWMM_OK);
        ASSERT_EQ(swmm_engine_initialize(e), SWMM_OK);
        ASSERT_EQ(swmm_engine_start(e, 1), SWMM_OK);
        double elapsed = 0.0;
        ASSERT_EQ(swmm_engine_step(e, &elapsed), SWMM_OK);
        double c = -1.0;
        EXPECT_EQ(swmm_heat_get_current_cloud(e, &c), SWMM_OK);
        EXPECT_DOUBLE_EQ(c, 1.0) << "series value 1.7 not clamped to 1";
        swmm_engine_end(e);
        swmm_engine_destroy(e);
    }
}

// ---------------------------------------------------------------------------
// Gate 14 — a below-sea-level site keeps its elevation.
//
// `elevation_m` used to carry a `< 0` "use the climate value" sentinel
// while the parser deliberately admitted down to -500 m. Every Dead-Sea or
// Salton-Sea deck silently got the climate elevation and the wrong
// pressure. The flag replaced the sentinel; this is what says so.
// ---------------------------------------------------------------------------
TEST(HeatSolarRadiationTest, BelowSeaLevelElevationIsKept) {
    write_file("_h6a_below.heat",
               "[HEAT_SOURCES]\nINITIAL_STATE GLOBAL 20.0\n\n"
               "[HEAT_FLUXES]\nRADIATIVE_EXCHANGE ON\n\n"
               "[RADIATIVE_FLUXES]\nSHORTWAVE GLOBAL COMPUTED\n\n"
               "[SOLAR_RADIATION]\nLATITUDE GLOBAL 31.5\n"
               "LONGITUDE GLOBAL 35.5\nTIMEZONE GLOBAL 2.0\n"
               "ELEVATION GLOBAL -430.0\n");
    write_deck("_h6a_below.inp",
               "org.hydrocouple.openswmm.heat config=\"_h6a_below.heat\"");
    SWMM_Engine e = nullptr;
    ASSERT_EQ(open_only("_h6a_below.inp", "_h6a_below.rpt", "_h6a_below.out",
                        &e),
              SWMM_OK);
    ASSERT_NE(e, nullptr);

    double elev = 0.0;
    EXPECT_EQ(swmm_heat_get_solar(e, SWMM_HEAT_SOLAR_ELEVATION, &elev), SWMM_OK);
    EXPECT_DOUBLE_EQ(elev, -430.0);
    // Below sea level is HIGHER pressure than at sea level — the direction
    // that would have been lost by falling back on the climate elevation.
    EXPECT_GT(sr::pressureFromElevation(-430.0), 1013.25);
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 15 — a [CLOUD_COVER] holding only coefficients is not silently inert.
//
// It used to be: coefficient rows did not set `configured`, so
// `updateSolarForcing` skipped the whole cloud read and the section did
// nothing, with no error and no warning.
// ---------------------------------------------------------------------------
TEST(HeatSolarRadiationTest, CloudCoefficientsWithoutFractionWarn) {
    write_file("_h6a_coeff.heat",
               "[HEAT_SOURCES]\nINITIAL_STATE GLOBAL 20.0\n\n"
               "[HEAT_FLUXES]\nRADIATIVE_EXCHANGE ON\n\n"
               "[RADIATIVE_FLUXES]\nSHORTWAVE GLOBAL 250.0\n\n"
               "[CLOUD_COVER]\nLW_CLOUD_K GLOBAL 0.30\n");
    write_deck("_h6a_coeff.inp",
               "org.hydrocouple.openswmm.heat config=\"_h6a_coeff.heat\"");
    SWMM_Engine e = nullptr;
    ASSERT_EQ(open_only("_h6a_coeff.inp", "_h6a_coeff.rpt", "_h6a_coeff.out",
                        &e),
              SWMM_OK);
    ASSERT_NE(e, nullptr);

    // The coefficient was stored and the section is marked configured...
    double k = 0.0;
    EXPECT_EQ(swmm_heat_get_cloud(e, SWMM_HEAT_CLOUD_LW_CLOUD_K, &k), SWMM_OK);
    EXPECT_DOUBLE_EQ(k, 0.30);
    int configured = 0;
    EXPECT_EQ(swmm_heat_get_cloud_configured(e, &configured), SWMM_OK);
    EXPECT_EQ(configured, 1);

    // ...and the user was told it does nothing without a FRACTION.
    const auto& warnings = as_cpp_engine(e).context().warnings;
    bool found = false;
    for (const auto& w : warnings)
        if (w.find("CLOUD_COVER") != std::string::npos &&
            w.find("FRACTION") != std::string::npos)
            found = true;
    EXPECT_TRUE(found) << "no warning about coefficients without FRACTION";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 16 — warnings do not outlive a rejected configuration.
//
// Warnings used to go straight to `ctx.warnings` while an error reset the
// config, so on a lenient open the user was warned about a configuration
// that never took effect — and a malformed COMPUTED row produced the advice
// "Set SHORTWAVE GLOBAL COMPUTED" to someone who had written exactly that.
// ---------------------------------------------------------------------------
TEST(HeatSolarRadiationTest, WarningsDoNotSurviveARejectedConfig) {
    write_file("_h6a_warnerr.heat",
               "[HEAT_SOURCES]\nINITIAL_STATE GLOBAL 20.0\n\n"
               "[HEAT_FLUXES]\nRADIATIVE_EXCHANGE ON\n\n"
               // Malformed: trailing token. Errors, and leaves sw_mode at
               // CONSTANT — which is what used to trigger the misleading
               // "[SOLAR_RADIATION] ... is not COMPUTED" warning.
               "[RADIATIVE_FLUXES]\nSHORTWAVE GLOBAL COMPUTED EXTRA\n\n"
               "[SOLAR_RADIATION]\nLATITUDE GLOBAL 41.7\n"
               "LONGITUDE GLOBAL -111.8\n");
    write_deck("_h6a_warnerr.inp",
               "org.hydrocouple.openswmm.heat config=\"_h6a_warnerr.heat\"");

    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    const int rc = swmm_engine_open(e, "_h6a_warnerr.inp", "_h6a_warnerr.rpt",
                                    "_h6a_warnerr.out", nullptr);
    // However the open resolves, no warning may claim the coordinates went
    // unused — the config they belong to was discarded.
    for (const auto& w : as_cpp_engine(e).context().warnings) {
        EXPECT_EQ(w.find("[SOLAR_RADIATION] is configured but"),
                  std::string::npos)
            << "warning survived a discarded config: " << w;
    }
    (void)rc;
    swmm_engine_destroy(e);
}
