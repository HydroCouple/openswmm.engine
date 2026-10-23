// SPDX-License-Identifier: Apache-2.0

/**
 * @file      test_richards_column.cpp
 * @brief     Tests for richards column.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>
#include "hydrology/RichardsColumn.hpp"
#include <numeric>
#include <cmath>
using namespace openswmm::richards;
namespace {
/**
 * @brief Build the one-meter soil column and surface storage reference fixture.
 *
 * Soil cells receive theta=0.05+0.4*se, saturated theta 0.45, Ks=1e-5 m/s and fixed retention
 * parameters; surface holds 0.02 m3. No input validation or integration occurs; max internal step
 * is 20 s.
 *
 * @param[in] count Positive soil-layer count fitting allocations, default eight.
 * @param[in] se Intended effective saturation fraction [0,1], default 0.3.
 * @return Owned enabled column with one surface cell and count unit-area soil cells spanning one
 *     meter.
 */
Column column(int count = 8, double se = .3) {
    Column c;
    c.options.enabled = true;
    c.options.max_step = 20;
    c.cells.push_back({.1, 1, 1, .1});
    c.water.push_back(.02);
    for (int i = 0; i < count; ++i) {
        Cell s;
        s.volume = s.dz = 1.0 / count;
        s.area = 1;
        s.z = 1 - (i + .5) / count;
        s.theta_s = .45;
        s.Ks = 1.e-5;
        s.wilting = .1;
        s.material = {.05, 2, 1.6, .5, 1.e-4};
        c.cells.push_back(s);
        c.water.push_back((.05 + .4 * se) * s.volume);
    }
    return c;
}
/**
 * @brief Sum stored water over surface and soil cells.
 *
 * Does not include boundary/ET ledgers or recompute physical consistency.
 *
 * @param[in] c Stable column water vector, unchanged.
 * @return Arithmetic total water m3, zero when empty, without clamping or finite validation.
 */
double total(const Column& c) {
    return std::accumulate(c.water.begin(), c.water.end(), 0.0);
}
}
TEST(RichardsColumn, RetentionStorageInverseIncludingSaturation) {
    Material p{.05, 2, 1.6, .5, 1.e-4};
    for (double h : {-100., -2., -.1, 0., .1, 3.}) {
        EXPECT_NEAR(pressure(p, .45, storage(p, .45, h)), h, 1.e-10 * std::max(1., std::abs(h)));
        EXPECT_LE(theta(p, .45, h), .45);
        EXPECT_GE(conductivity(p, 1.e-5, h), 0);
        EXPECT_LE(conductivity(p, 1.e-5, h), 1.e-5);
    }
}
TEST(RichardsColumn, HeldSurfaceSupplyAndEtAreIntegratedInsideTheOde) {
    auto c = column(4);
    c.water[0] = 0;
    c.inflow = 1e-5;
    c.cells[0].potential_et = 2e-6;
    for (auto& cell : c.cells)
        cell.Ks = 0;
    const double initial = total(c);
    std::vector<Column*> batch{&c};
    auto r = openswmm::richards::advance(batch, 60);
    ASSERT_TRUE(r[0].ok) << r[0].error;
    EXPECT_NEAR(r[0].inflow, .0006, 1e-14);
    EXPECT_GT(r[0].evaporation, 0);
    EXPECT_NEAR(total(c) - initial, r[0].inflow - r[0].evaporation, 1e-10);
    EXPECT_NEAR(r[0].balance, 0, 1e-10);
    double supplied = 0;
    for (const auto& t : r[0].transfers)
        if (t.from == -2) {
            EXPECT_EQ(t.to, 0);
            supplied += t.volume;
        }
    EXPECT_DOUBLE_EQ(supplied, r[0].inflow);
}
TEST(RichardsColumn, SurfaceSupplyConservesWithBothDirectionalBoundaryLimits) {
    for (double head : {-10., 10.}) {
        auto c = column(4, .8);
        c.inflow = 1e-6;
        c.bottom = Bottom::Head;
        c.bottom_head = head;
        c.bottom_K = .001;
        c.bottom_distance = .5;
        c.bottom_area = 40;
        c.bottom_in_limit = c.bottom_out_limit = 1e-6;
        const double initial = total(c);
        std::vector<Column*> batch{&c};
        auto r = openswmm::richards::advance(batch, 20);
        ASSERT_TRUE(r[0].ok) << r[0].error;
        EXPECT_NEAR(r[0].inflow, 2e-5, 1e-14);
        EXPECT_LE(r[0].bottom_inflow, c.bottom_in_limit);
        EXPECT_LE(r[0].bottom_outflow, c.bottom_out_limit);
        EXPECT_GT(r[0].bottom_in_limited + r[0].bottom_out_limited, 0);
        EXPECT_NEAR(total(c) - initial, r[0].inflow - r[0].bottom_volume - r[0].evaporation, 1e-10);
    }
}
TEST(RichardsColumn, ContinuousSupplyIsNotAnInitialWaterPulseAndInvalidRatesRollBack) {
    auto continuous = column(4), pulse = continuous;
    continuous.water[0] = pulse.water[0] = 0;
    continuous.inflow = 1e-4;
    pulse.water[0] += .006;
    std::vector<Column*> a{&continuous}, b{&pulse};
    auto x = openswmm::richards::advance(a, 60), y = openswmm::richards::advance(b, 60);
    ASSERT_TRUE(x[0].ok && y[0].ok);
    EXPECT_NEAR(total(continuous), total(pulse), 1e-10);
    double difference = 0;
    for (std::size_t k = 0; k < continuous.water.size(); ++k)
        difference += std::abs(continuous.water[k] - pulse.water[k]);
    EXPECT_GT(difference, 1e-7);
    for (double rate :
         {-1., std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        auto good = column(), bad = good;
        bad.inflow = rate;
        const auto before = good.water;
        std::vector<Column*> all{&good, &bad};
        auto failed = openswmm::richards::advance(all, 1);
        EXPECT_FALSE(failed[0].ok);
        EXPECT_EQ(good.water, before);
    }
}
TEST(RichardsColumn, SealedInfiltrationConservesAndWetsTopFirst) {
    auto c = column();
    double before = total(c);
    std::vector<Column*> batch{&c};
    auto r = openswmm::richards::advance(batch, 600);
    ASSERT_TRUE(r[0].ok) << r[0].error;
    EXPECT_NEAR(total(c), before, 1.e-9);
    EXPECT_LT(c.water[0], .02);
    EXPECT_GT(c.water[1] / c.cells[1].volume, c.water.back() / c.cells.back().volume);
    EXPECT_LT(std::abs(r[0].balance), 1.e-9);
}
TEST(RichardsColumn, HydrostaticZeroFlowAtFullSaturation) {
    auto c = column();
    c.water[0] = .04;
    for (int i = 1; i < static_cast<int>(c.cells.size()); ++i)
        c.water[i] = c.cells[i].volume * storage(c.cells[i].material, .45, 1.04 - c.cells[i].z);
    const auto before = c.water;
    std::vector<Column*> batch{&c};
    auto r = openswmm::richards::advance(batch, 200);
    ASSERT_TRUE(r[0].ok) << r[0].error;
    for (std::size_t i = 0; i < before.size(); ++i)
        EXPECT_NEAR(c.water[i], before[i], 1.e-12);
}
TEST(RichardsColumn, HeadBoundaryDrivesCapillaryRiseAndBalances) {
    auto c = column();
    c.water[0] = 0;
    c.bottom = Bottom::Head;
    c.bottom_K = 1.e-5;
    c.bottom_distance = .1;
    c.bottom_head = .2;
    double before = total(c);
    std::vector<Column*> batch{&c};
    auto r = openswmm::richards::advance(batch, 300);
    ASSERT_TRUE(r[0].ok) << r[0].error;
    EXPECT_LT(r[0].bottom_volume, 0);
    EXPECT_GT(total(c), before);
    EXPECT_NEAR(total(c) - before, -r[0].bottom_volume, 1.e-9);
}
TEST(RichardsColumn, BatchMatchesIndependentControllersForDifferentWetness) {
    auto a = column(4, .2), b = column(4, .8), x = a, y = b;
    std::vector<Column*> batch{&a, &b}, bx{&x}, by{&y};
    auto r = openswmm::richards::advance(batch, 120), rx = openswmm::richards::advance(bx, 120),
         ry = openswmm::richards::advance(by, 120);
    ASSERT_TRUE(r[0].ok && r[1].ok && rx[0].ok && ry[0].ok);
    for (std::size_t i = 0; i < a.water.size(); ++i) {
        EXPECT_NEAR(a.water[i], x.water[i], 1.e-12);
        EXPECT_NEAR(b.water[i], y.water[i], 1.e-12);
    }
    EXPECT_EQ(r[0].accepted, rx[0].accepted);
    EXPECT_EQ(r[1].accepted, ry[0].accepted);
}
TEST(RichardsColumn, InvalidSaturatedStorageRollsBackWholeBatch) {
    auto a = column(), b = column();
    b.cells[1].material.specific_storage = 0;
    auto before = a.water;
    std::vector<Column*> batch{&a, &b};
    auto r = openswmm::richards::advance(batch, 60);
    EXPECT_FALSE(r[0].ok);
    EXPECT_FALSE(r[1].ok);
    EXPECT_EQ(a.water, before);
}
TEST(RichardsColumn, FreeDrainageAndEvaporationCloseWaterBalance) {
    auto c = column(8, .8);
    c.bottom = Bottom::FreeDrainage;
    c.bottom_K = 1.e-5;
    c.cells[0].potential_et = 1.e-7;
    double before = total(c);
    std::vector<Column*> batch{&c};
    auto r = openswmm::richards::advance(batch, 300);
    ASSERT_TRUE(r[0].ok) << r[0].error;
    EXPECT_GT(r[0].bottom_volume, 0);
    EXPECT_GT(r[0].evaporation, 0);
    EXPECT_NEAR(before - total(c), r[0].bottom_volume + r[0].evaporation, 1.e-9);
}
TEST(RichardsColumn, MaterialBarrierRestrictsDrainageWithoutLosingWater) {
    auto free = column(8, .8), barrier = free;
    free.bottom = barrier.bottom = Bottom::FreeDrainage;
    free.bottom_K = barrier.bottom_K = 1.e-5;
    for (int i = 5; i <= 8; ++i)
        barrier.cells[i].Ks = 1.e-9;
    const double before = total(barrier);
    std::vector<Column*> batch{&free, &barrier};
    const auto reports = openswmm::richards::advance(batch, 600);
    ASSERT_TRUE(reports[0].ok) << reports[0].error;
    ASSERT_TRUE(reports[1].ok) << reports[1].error;
    EXPECT_LT(reports[1].bottom_volume, reports[0].bottom_volume * .01);
    EXPECT_NEAR(total(barrier) + reports[1].bottom_volume, before, 1.e-9);
    for (std::size_t i = 1; i < barrier.water.size(); ++i)
        EXPECT_GT(barrier.water[i], barrier.cells[i].volume * barrier.cells[i].material.theta_r);
}
TEST(RichardsColumn, TighteningTimeToleranceConvergesWettingProfile) {
    auto normal = column(8), tight = normal, reference = normal;
    tight.options.atol = 1.e-9;
    tight.options.rtol = 1.e-7;
    reference.options.atol = 1.e-11;
    reference.options.rtol = 1.e-9;
    std::vector<Column*> batch{&normal, &tight, &reference};
    const auto reports = openswmm::richards::advance(batch, 120);
    for (const auto& report : reports)
        ASSERT_TRUE(report.ok) << report.error;
    double normal_error = 0, tight_error = 0;
    for (std::size_t i = 1; i < normal.water.size(); ++i) {
        normal_error = std::max(normal_error, std::abs(normal.water[i] - reference.water[i]));
        tight_error = std::max(tight_error, std::abs(tight.water[i] - reference.water[i]));
    }
    EXPECT_LT(tight_error, normal_error * .3);
}

TEST(RichardsColumn, LargeHeterogeneousBatchPreservesIndependentSolutionsAndWaterBalance) {
    // Different cell counts form separate vector groups; boundary conditions,
    // materials and wetness vary within each group and retain local controllers.
    std::vector<Column> columns;
    for (int lane = 0; lane < 257; ++lane) {
        auto c = column(4 * (1 + lane % 3), .15 + .1 * (lane % 8));
        c.water[0] = lane % 2 ? 0 : .02;
        c.bottom = static_cast<Bottom>(lane % 3);
        c.bottom_K = 1.e-5;
        c.bottom_head = .2;
        c.bottom_distance = .1;
        c.cells[0].potential_et = 1.e-7;
        if (lane % 4 == 0)
            for (std::size_t i = c.cells.size() / 2; i < c.cells.size(); ++i)
                c.cells[i].Ks = 1.e-8;
        columns.push_back(std::move(c));
    }
    auto independent = columns;
    std::vector<Column*> batch;
    for (auto& c : columns)
        batch.push_back(&c);
    const auto reports = openswmm::richards::advance(batch, 120);
    ASSERT_EQ(reports.size(), columns.size());
    for (std::size_t lane = 0; lane < columns.size(); ++lane) {
        SCOPED_TRACE(lane);
        const double before = total(independent[lane]);
        std::vector<Column*> single{&independent[lane]};
        const auto reference = openswmm::richards::advance(single, 120);
        ASSERT_TRUE(reports[lane].ok) << reports[lane].error;
        ASSERT_TRUE(reference[0].ok) << reference[0].error;
        EXPECT_EQ(columns[lane].water, independent[lane].water);
        EXPECT_EQ(reports[lane].accepted, reference[0].accepted);
        EXPECT_EQ(reports[lane].rejected, reference[0].rejected);
        EXPECT_EQ(reports[lane].rhs, reference[0].rhs);
        EXPECT_EQ(reports[lane].newton, reference[0].newton);
        EXPECT_DOUBLE_EQ(reports[lane].min_step, reference[0].min_step);
        EXPECT_NEAR(total(columns[lane]) + reports[lane].bottom_volume + reports[lane].evaporation,
                    before, 1.e-9);
        EXPECT_LT(std::abs(reports[lane].balance), 1.e-9);
    }
}

TEST(RichardsColumn, InvalidLaneInLargeMixedSizeBatchLeavesEveryColumnUnchanged) {
    std::vector<Column> columns;
    for (int lane = 0; lane < 128; ++lane)
        columns.push_back(column(4 * (1 + lane % 3), .2 + .1 * (lane % 7)));
    columns.back().cells.back().material.specific_storage = 0;
    const auto before = columns;
    std::vector<Column*> batch;
    for (auto& c : columns)
        batch.push_back(&c);
    const auto reports = openswmm::richards::advance(batch, 60);
    ASSERT_EQ(reports.size(), columns.size());
    for (std::size_t lane = 0; lane < columns.size(); ++lane) {
        SCOPED_TRACE(lane);
        EXPECT_FALSE(reports[lane].ok);
        EXPECT_EQ(columns[lane].water, before[lane].water);
    }
}

TEST(RichardsColumn, DirectionalReservationsBindInsideTheOdeAndConserveWithEt) {
    for (bool inward : {false, true}) {
        auto c = column(4, .8);
        c.bottom = Bottom::Head;
        c.bottom_K = 1.e-3;
        c.bottom_distance = .1;
        c.bottom_head = inward ? 5 : -10;
        c.cells[0].potential_et = 1.e-8;
        c.bottom_out_limit = inward ? 0 : 1.e-6;
        c.bottom_in_limit = inward ? 1.e-6 : 0;
        const auto initial = total(c);
        std::vector<Column*> batch{&c};
        auto r = openswmm::richards::advance(batch, 60);
        ASSERT_TRUE(r[0].ok) << r[0].error;
        EXPECT_LE(r[0].bottom_outflow, c.bottom_out_limit);
        EXPECT_LE(r[0].bottom_inflow, c.bottom_in_limit);
        EXPECT_NEAR(inward ? r[0].bottom_inflow : r[0].bottom_outflow, 1.e-6, 1.e-12);
        EXPECT_GT(inward ? r[0].bottom_in_limited : r[0].bottom_out_limited, 0);
        EXPECT_NEAR(initial - total(c), r[0].bottom_outflow - r[0].bottom_inflow + r[0].evaporation,
                    1.e-10);
    }
}
TEST(RichardsColumn, ZeroBoundaryAwardsDoNotSupplyWaterOrDisableRedistributionAndEt) {
    auto c = column(4, .7);
    c.bottom = Bottom::Head;
    c.bottom_head = 100;
    c.bottom_K = 1.e-3;
    c.bottom_out_limit = c.bottom_in_limit = 0;
    c.cells[0].potential_et = 1.e-8;
    const auto before = c.water;
    const auto initial = total(c);
    std::vector<Column*> batch{&c};
    auto r = openswmm::richards::advance(batch, 30);
    ASSERT_TRUE(r[0].ok) << r[0].error;
    EXPECT_DOUBLE_EQ(r[0].bottom_volume, 0);
    EXPECT_GT(r[0].bottom_in_limited, 0);
    EXPECT_GT(r[0].evaporation, 0);
    EXPECT_NE(c.water, before);
    EXPECT_NEAR(total(c) + r[0].evaporation, initial, 1.e-10);
}
TEST(RichardsColumn, ContactAreaAndWholeResistanceCloggingAreAppliedOnce) {
    auto a = column(4, .8);
    a.water[0] = .04;
    for (std::size_t k = 1; k < a.cells.size(); ++k) {
        auto& c = a.cells[k];
        a.water[k] = c.volume * storage(c.material, c.theta_s, 1.04 - c.z);
    }
    a.bottom = Bottom::Head;
    a.bottom_head = 0;
    a.bottom_K = 1.e-5;
    a.bottom_distance = .2;
    auto b = a, c = a;
    b.bottom_factor = .25;
    c.bottom_area = 3;
    std::vector<Column*> batch{&a, &b, &c};
    auto r = openswmm::richards::advance(batch, 1.e-7);
    for (const auto& x : r)
        ASSERT_TRUE(x.ok) << x.error;
    EXPECT_NEAR(r[1].bottom_volume / r[0].bottom_volume, .25, 1.e-6);
    EXPECT_NEAR(r[2].bottom_volume / r[0].bottom_volume, 3, 1.e-6);
    const auto& cell = a.cells.back();
    const double expected = 1.04 / (.5 * cell.dz / cell.Ks + .2 / 1.e-5);
    EXPECT_NEAR(r[0].bottom_volume / 1.e-7, expected, expected * 1.e-6);
}
TEST(RichardsColumn, BoundedMixedBatchesPreserveIndependentClocksAndSolutions) {
    std::vector<Column> all;
    for (int n = 0; n < 257; ++n) {
        auto c = column(4 * (1 + n % 3), .2 + .1 * (n % 7));
        c.bottom = Bottom::Head;
        c.bottom_head = n % 2 ? 5 : -10;
        c.bottom_K = 1.e-3;
        c.bottom_distance = .1;
        c.bottom_out_limit = 1.e-7 * (1 + n % 5);
        c.bottom_in_limit = 1.e-7 * (1 + n % 3);
        all.push_back(c);
    }
    auto independent = all;
    std::vector<Column*> batch;
    for (auto& c : all)
        batch.push_back(&c);
    auto reports = openswmm::richards::advance(batch, 20);
    for (std::size_t n = 0; n < all.size(); ++n) {
        SCOPED_TRACE(n);
        std::vector<Column*> one{&independent[n]};
        auto r = openswmm::richards::advance(one, 20);
        ASSERT_TRUE(reports[n].ok) << reports[n].error;
        ASSERT_TRUE(r[0].ok) << r[0].error;
        EXPECT_EQ(all[n].water, independent[n].water);
        EXPECT_EQ(reports[n].accepted, r[0].accepted);
        EXPECT_EQ(reports[n].rejected, r[0].rejected);
        EXPECT_EQ(reports[n].rhs, r[0].rhs);
        EXPECT_DOUBLE_EQ(reports[n].bottom_outflow, r[0].bottom_outflow);
        EXPECT_DOUBLE_EQ(reports[n].bottom_inflow, r[0].bottom_inflow);
    }
}
TEST(RichardsColumn, InvalidDirectionalLimitOrAliasedColumnRejectsTheWholeBatch) {
    for (double bad : {-1., std::numeric_limits<double>::quiet_NaN(),
                       -std::numeric_limits<double>::infinity()}) {
        auto a = column(), b = a;
        const auto before = a.water;
        b.bottom_in_limit = bad;
        std::vector<Column*> batch{&a, &b};
        auto r = openswmm::richards::advance(batch, 30);
        EXPECT_FALSE(r[0].ok);
        EXPECT_EQ(a.water, before);
    }
    auto a = column();
    const auto before = a.water;
    std::vector<Column*> batch{&a, &a};
    auto r = openswmm::richards::advance(batch, 30);
    EXPECT_FALSE(r[0].ok);
    EXPECT_EQ(a.water, before);
}
TEST(RichardsColumn, BoundedTimeToleranceRefinementConvergesAfterAwardExhaustion) {
    auto a = column(4, .8);
    a.bottom = Bottom::Head;
    a.bottom_head = -10;
    a.bottom_K = 1.e-3;
    a.bottom_distance = .1;
    a.bottom_out_limit = 1.e-5;
    a.bottom_in_limit = 0;
    auto b = a, c = a;
    b.options.atol = 1.e-9;
    b.options.rtol = 1.e-7;
    c.options.atol = 1.e-11;
    c.options.rtol = 1.e-9;
    std::vector<Column*> batch{&a, &b, &c};
    auto r = openswmm::richards::advance(batch, 30);
    for (const auto& x : r)
        ASSERT_TRUE(x.ok) << x.error;
    double loose = 0, tight = 0;
    for (std::size_t k = 0; k < a.water.size(); ++k) {
        loose += std::abs(a.water[k] - c.water[k]);
        tight += std::abs(b.water[k] - c.water[k]);
    }
    EXPECT_LT(tight, loose);
    for (const auto& x : r)
        EXPECT_LE(x.bottom_outflow, 1.e-5);
}
