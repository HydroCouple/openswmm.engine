// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include "hydrology/RichardsColumn.hpp"
#include <numeric>
#include <cmath>
using namespace openswmm::richards;
namespace {
Column column(int count = 8, double se = .3) {
    Column c; c.options.enabled = true; c.options.max_step = 20;
    c.cells.push_back({.1, 1, 1, .1}); c.water.push_back(.02);
    for (int i = 0; i < count; ++i) {
        Cell s; s.volume = s.dz = 1.0 / count; s.area = 1; s.z = 1 - (i + .5) / count;
        s.theta_s = .45; s.Ks = 1.e-5; s.wilting = .1; s.material = {.05, 2, 1.6, .5, 1.e-4};
        c.cells.push_back(s); c.water.push_back((.05 + .4 * se) * s.volume);
    }
    return c;
}
double total(const Column& c) { return std::accumulate(c.water.begin(), c.water.end(), 0.0); }
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
TEST(RichardsColumn, SealedInfiltrationConservesAndWetsTopFirst) {
    auto c = column(); double before = total(c); std::vector<Column*> batch{&c};
    auto r = openswmm::richards::advance(batch, 600);
    ASSERT_TRUE(r[0].ok) << r[0].error;
    EXPECT_NEAR(total(c), before, 1.e-9);
    EXPECT_LT(c.water[0], .02);
    EXPECT_GT(c.water[1] / c.cells[1].volume, c.water.back() / c.cells.back().volume);
    EXPECT_LT(std::abs(r[0].balance), 1.e-9);
}
TEST(RichardsColumn, HydrostaticZeroFlowAtFullSaturation) {
    auto c = column(); c.water[0] = .04;
    for (int i = 1; i < static_cast<int>(c.cells.size()); ++i)
        c.water[i] = c.cells[i].volume * storage(c.cells[i].material, .45, 1.04 - c.cells[i].z);
    const auto before = c.water; std::vector<Column*> batch{&c};
    auto r = openswmm::richards::advance(batch, 200);
    ASSERT_TRUE(r[0].ok) << r[0].error;
    for (std::size_t i = 0; i < before.size(); ++i) EXPECT_NEAR(c.water[i], before[i], 1.e-12);
}
TEST(RichardsColumn, HeadBoundaryDrivesCapillaryRiseAndBalances) {
    auto c = column(); c.water[0] = 0; c.bottom = Bottom::Head;
    c.bottom_K = 1.e-5; c.bottom_distance = .1; c.bottom_head = .2;
    double before = total(c); std::vector<Column*> batch{&c};
    auto r = openswmm::richards::advance(batch, 300);
    ASSERT_TRUE(r[0].ok) << r[0].error;
    EXPECT_LT(r[0].bottom_volume, 0);
    EXPECT_GT(total(c), before);
    EXPECT_NEAR(total(c) - before, -r[0].bottom_volume, 1.e-9);
}
TEST(RichardsColumn, BatchMatchesIndependentControllersForDifferentWetness) {
    auto a = column(4, .2), b = column(4, .8), x = a, y = b;
    std::vector<Column*> batch{&a, &b}, bx{&x}, by{&y};
    auto r = openswmm::richards::advance(batch, 120), rx = openswmm::richards::advance(bx, 120), ry = openswmm::richards::advance(by, 120);
    ASSERT_TRUE(r[0].ok && r[1].ok && rx[0].ok && ry[0].ok);
    for (std::size_t i = 0; i < a.water.size(); ++i) {
        EXPECT_NEAR(a.water[i], x.water[i], 1.e-12);
        EXPECT_NEAR(b.water[i], y.water[i], 1.e-12);
    }
    EXPECT_EQ(r[0].accepted, rx[0].accepted); EXPECT_EQ(r[1].accepted, ry[0].accepted);
}
TEST(RichardsColumn, InvalidSaturatedStorageRollsBackWholeBatch) {
    auto a = column(), b = column(); b.cells[1].material.specific_storage = 0;
    auto before = a.water; std::vector<Column*> batch{&a, &b}; auto r = openswmm::richards::advance(batch, 60);
    EXPECT_FALSE(r[0].ok); EXPECT_FALSE(r[1].ok); EXPECT_EQ(a.water, before);
}
TEST(RichardsColumn, FreeDrainageAndEvaporationCloseWaterBalance) {
    auto c = column(8, .8); c.bottom = Bottom::FreeDrainage; c.bottom_K = 1.e-5;
    c.cells[0].potential_et = 1.e-7;
    double before = total(c); std::vector<Column*> batch{&c}; auto r = openswmm::richards::advance(batch, 300);
    ASSERT_TRUE(r[0].ok) << r[0].error;
    EXPECT_GT(r[0].bottom_volume, 0); EXPECT_GT(r[0].evaporation, 0);
    EXPECT_NEAR(before - total(c), r[0].bottom_volume + r[0].evaporation, 1.e-9);
}
TEST(RichardsColumn, MaterialBarrierRestrictsDrainageWithoutLosingWater) {
    auto free = column(8,.8), barrier = free;
    free.bottom = barrier.bottom = Bottom::FreeDrainage;
    free.bottom_K = barrier.bottom_K = 1.e-5;
    for(int i=5;i<=8;++i)barrier.cells[i].Ks=1.e-9;
    const double before=total(barrier);
    std::vector<Column*> batch{&free,&barrier};
    const auto reports=openswmm::richards::advance(batch,600);
    ASSERT_TRUE(reports[0].ok) << reports[0].error;
    ASSERT_TRUE(reports[1].ok) << reports[1].error;
    EXPECT_LT(reports[1].bottom_volume,reports[0].bottom_volume*.01);
    EXPECT_NEAR(total(barrier)+reports[1].bottom_volume,before,1.e-9);
    for(std::size_t i=1;i<barrier.water.size();++i)EXPECT_GT(barrier.water[i],barrier.cells[i].volume*barrier.cells[i].material.theta_r);
}
TEST(RichardsColumn, TighteningTimeToleranceConvergesWettingProfile) {
    auto normal=column(8), tight=normal, reference=normal;
    tight.options.atol=1.e-9; tight.options.rtol=1.e-7;
    reference.options.atol=1.e-11; reference.options.rtol=1.e-9;
    std::vector<Column*> batch{&normal,&tight,&reference};
    const auto reports=openswmm::richards::advance(batch,120);
    for(const auto& report:reports)ASSERT_TRUE(report.ok)<<report.error;
    double normal_error=0,tight_error=0;
    for(std::size_t i=1;i<normal.water.size();++i) {
        normal_error=std::max(normal_error,std::abs(normal.water[i]-reference.water[i]));
        tight_error=std::max(tight_error,std::abs(tight.water[i]-reference.water[i]));
    }
    EXPECT_LT(tight_error,normal_error*.3);
}
