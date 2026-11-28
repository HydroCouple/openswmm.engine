// SPDX-License-Identifier: Apache-2.0

/**
 * @file      test_constants_legacy_parity.cpp
 * @brief     The engine's legacy-faithful constants equal legacy's, bit for bit.
 *
 * @details Constants.hpp and UnitConversion.hpp carry two families: the
 *          legacy-faithful values the 1D engine computes with (PI, GRAVITY,
 *          the Ucf/Qcf tables, FT3_TO_L, ...) and exact values for the parts
 *          with no legacy counterpart (PI_EXACT, GRAVITY_SI, ...). Bit-identity
 *          with 5.3.0 rests on the first family matching consts.h and swmm5.c
 *          exactly, so every member is compared with EXPECT_EQ, not a tolerance.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>

#include "core/Constants.hpp"
#include "core/SimulationOptions.hpp"
#include "core/UnitConversion.hpp"

#include <cmath>
#include <numbers>

// From legacy_consts_values.c (src/legacy/engine/consts.h macros).
extern "C" {
extern const double legacy_consts_PI;
extern const double legacy_consts_GRAVITY;
extern const double legacy_consts_PHI;
extern const double legacy_consts_TINY;
extern const double legacy_consts_MISSING;
extern const double legacy_consts_FUDGE;
extern const double legacy_consts_FLOW_TOL;
extern const double legacy_consts_SECperDAY;
extern const double legacy_consts_MSECperDAY;
extern const double legacy_consts_MperFT;
extern const double legacy_consts_LperFT3;
extern const double legacy_consts_M3perFT3;
extern const double legacy_consts_MMperINCH;
extern const double legacy_consts_GPMperCFS;
extern const double legacy_consts_MGDperCFS;
extern const double legacy_consts_LPSperCFS;
extern const double legacy_consts_MLDperCFS;
}

namespace {

using namespace openswmm;

TEST(ConstantsLegacyParity, PhysicalAndNumericalConstantsMatchConstsH) {
    EXPECT_EQ(constants::PI, legacy_consts_PI);
    EXPECT_EQ(constants::GRAVITY, legacy_consts_GRAVITY);
    EXPECT_EQ(constants::PHI, legacy_consts_PHI);
    EXPECT_EQ(constants::TINY, legacy_consts_TINY);
    EXPECT_EQ(constants::MISSING, legacy_consts_MISSING);
    EXPECT_EQ(constants::FUDGE, legacy_consts_FUDGE);
    EXPECT_EQ(constants::FLOW_TOL, legacy_consts_FLOW_TOL);
    EXPECT_EQ(constants::SEC_PER_DAY, legacy_consts_SECperDAY);
    EXPECT_EQ(constants::MSEC_PER_DAY, legacy_consts_MSECperDAY);
    EXPECT_EQ(constants::SEC_PER_DAY_INT, 86400);
}

TEST(ConstantsLegacyParity, FixedUnitFactorsMatchConstsH) {
    EXPECT_EQ(ucf::FT_TO_M, legacy_consts_MperFT);
    EXPECT_EQ(ucf::FT3_TO_L, legacy_consts_LperFT3);
    EXPECT_EQ(ucf::FT3_TO_M3, legacy_consts_M3perFT3);
    EXPECT_EQ(ucf::IN_TO_MM, legacy_consts_MMperINCH);
    EXPECT_EQ(ucf::Qcf[1], legacy_consts_GPMperCFS);
    EXPECT_EQ(ucf::Qcf[2], legacy_consts_MGDperCFS);
    EXPECT_EQ(ucf::Qcf[4], legacy_consts_LPSperCFS);
    EXPECT_EQ(ucf::Qcf[5], legacy_consts_MLDperCFS);
}

TEST(ConstantsLegacyParity, UcfAndQcfTablesMatchSwmm5c) {
    // swmm5.c:146 Ucf[10][2] and swmm5.c:164 Qcf[6], copied verbatim: the
    // legacy library does not export them, so the table is the oracle here.
    constexpr double kUcf[10][2] = {
        {43200.0, 1097280.0},    // RAINFALL
        {12.0, 304.8},           // RAINDEPTH
        {1036800.0, 26334720.0}, // EVAPRATE
        {1.0, 0.3048},           // LENGTH
        {2.2956e-5, 0.92903e-5}, // LANDAREA
        {1.0, 0.02832},          // VOLUME
        {1.0, 1.608},            // WINDSPEED
        {1.0, 1.8},              // TEMPERATURE
        {2.203e-6, 1.0e-6},      // MASS
        {43560.0, 3048.0}        // GWFLOW
    };
    constexpr double kQcf[6] = {1.0, 448.831, 0.64632, 0.02832, 28.317, 2.4466};

    for (int q = 0; q < 10; ++q)
        for (int u = 0; u < 2; ++u) {
            EXPECT_EQ(ucf::Ucf[q][u], kUcf[q][u]) << "Ucf[" << q << "][" << u << "]";
            EXPECT_EQ(ucf::Ucf_inv[static_cast<std::size_t>(q)][static_cast<std::size_t>(u)],
                      1.0 / kUcf[q][u])
                << "Ucf_inv[" << q << "][" << u << "]";
        }
    for (int f = 0; f < 6; ++f) {
        EXPECT_EQ(ucf::Qcf[f], kQcf[f]) << "Qcf[" << f << "]";
        EXPECT_EQ(ucf::Qcf_inv[static_cast<std::size_t>(f)], 1.0 / kQcf[f])
            << "Qcf_inv[" << f << "]";
    }

    // The table entries that double as fixed factors are the same object.
    EXPECT_EQ(ucf::Ucf[ucf::LENGTH][1], ucf::FT_TO_M);
    EXPECT_EQ(ucf::Ucf[ucf::GWFLOW][0], ucf::ACRES_TO_FT2);
}

TEST(ConstantsLegacyParity, ReportVolumeFactorsMatchStatsrptAndReportC) {
    for (int fu = 0; fu < 6; ++fu) {
        SimulationOptions opts;
        opts.flow_units = static_cast<FlowUnits>(fu);
        const ucf::DisplayUnits du = ucf::DisplayUnits::from(opts);
        const int us = fu < 3 ? 0 : 1;
        // statsrpt.c:108-112: Vcf = 7.48 / 1.0e6 (US) | 28.317 / 1.0e6 (SI)
        EXPECT_EQ(du.mvol, (us ? 28.317 : 7.48) / 1.0e6) << "flow units " << fu;
        // report.c:433: ucf1 = UCF(LENGTH) * UCF(LANDAREA)
        EXPECT_EQ(du.landvol, ucf::Ucf[ucf::LENGTH][us] * ucf::Ucf[ucf::LANDAREA][us])
            << "flow units " << fu;
    }
}

TEST(ConstantsLegacyParity, ExactFamilyIsDistinctFromTheLegacyFamily) {
    // Full-precision pi moves a storage unit's depth off legacy's bits on the
    // first routing step (StorageGeometry.hpp), so the two must stay apart.
    EXPECT_EQ(constants::PI_EXACT, std::numbers::pi);
    EXPECT_NE(constants::PI, constants::PI_EXACT);
    EXPECT_NEAR(constants::PI, constants::PI_EXACT, 1e-9);

    // Legacy defines SI_GRAVITY 9.81 but never uses it; the 2D solver's gravity
    // is the standard value.
    EXPECT_EQ(constants::GRAVITY_SI, 9.80665);
    EXPECT_NEAR(constants::GRAVITY * ucf::FT_TO_M, constants::GRAVITY_SI, 0.01);

    // The exact volume factors are 0.3048³ and 1000 × that.
    EXPECT_NEAR(ucf::FT3_TO_M3_EXACT, ucf::FT_TO_M * ucf::FT_TO_M * ucf::FT_TO_M, 1e-17);
    EXPECT_NEAR(ucf::FT3_TO_L_EXACT, 1000.0 * ucf::FT3_TO_M3_EXACT, 1e-12);
    EXPECT_NE(ucf::FT3_TO_L, ucf::FT3_TO_L_EXACT);
    EXPECT_NE(ucf::FT3_TO_M3, ucf::FT3_TO_M3_EXACT);
    EXPECT_NE(ucf::FT3_TO_M3, ucf::Ucf[ucf::VOLUME][1]); // 0.028317 vs the table's 0.02832

    // Constants that double as table entries are the same value, not re-typed.
    EXPECT_EQ(constants::IN_PER_FT, ucf::Ucf[ucf::RAINDEPTH][0]);
    EXPECT_EQ(constants::F_PER_C, ucf::Ucf[ucf::TEMPERATURE][1]);
}

} // namespace
