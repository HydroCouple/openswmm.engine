#include <gtest/gtest.h>
#include "quality/ConservativeMixing.hpp"
#include <limits>
using namespace openswmm::quality;
namespace {
MixingWater pipe(double volume=10, double transfer=1) {
    return {{0,volume,0},{0,volume,0},{transfer,0,0},{0,0,transfer},{0,0,0},{0,0,0},
            {{0,1,transfer},{1,2,transfer}}};
}
TEST(ConservativeMixing, FixesIndependentOneStepConduitCounterexample) {
    ConservativeMixing solver; ASSERT_TRUE(solver.prepare(pipe()));
    MixedScalar out;
    auto r=solver.solve({{0,10,0},{20,0,0},{},true},out);
    ASSERT_TRUE(r.converged);
    EXPECT_NEAR(out.concentration[0],20,1e-12);
    EXPECT_NEAR(out.concentration[1],120.0/11,1e-12);
    EXPECT_NEAR(out.concentration[2],120.0/11,1e-12);
    EXPECT_NEAR(120,out.concentration[1]*10+out.exported_mass[2],1e-10);
    EXPECT_NEAR(out.transfer_mass[1],out.exported_mass[2],1e-12);
}
TEST(ConservativeMixing, UniformFieldAndHighTurnoverAreBounded) {
    ConservativeMixing solver; ASSERT_TRUE(solver.prepare(pipe(.001,1000)));
    MixedScalar out;
    ASSERT_TRUE(solver.solve({{7.25,7.25,7.25},{7250,0,0},{},true},out).converged);
    for (double c:out.concentration) EXPECT_NEAR(c,7.25,1e-10);
    ASSERT_TRUE(solver.solve({{0,10,0},{20000,0,0},{},true},out).converged);
    for (double c:out.concentration) { EXPECT_GE(c,10-1e-10); EXPECT_LE(c,20+1e-10); }
}
TEST(ConservativeMixing, ClosedLoopAndBypassConserve) {
    MixingWater w{{1,1,1,1},{1,1,1,1},{1,0,0,0},{0,0,1,0},{0,0,0,0},{0,0,0,0},
                  {{0,1,1000},{1,2,1000},{2,3,1000},{3,0,1000},{0,2,1}}};
    ConservativeMixing solver; ASSERT_TRUE(solver.prepare(w)); MixedScalar out;
    const auto r=solver.solve({{0,0,0,0},{100,0,0,0},{},true},out);
    ASSERT_TRUE(r.converged); EXPECT_LE(std::abs(r.mass_residual),1e-12);
    double final=out.exported_mass[2];for(double c:out.concentration) { final+=c; EXPECT_GE(c,0); EXPECT_LE(c,100); }
    EXPECT_NEAR(final,100,1e-8);
}
TEST(ConservativeMixing, DryMassIsHeldAndRemobilized) {
    ConservativeMixing solver; MixedScalar out;
    MixingWater dry{{0},{0},{0},{0},{0},{0},{}};
    ASSERT_TRUE(solver.prepare(dry));
    ASSERT_TRUE(solver.solve({{0},{4},{},true},out).converged);
    EXPECT_DOUBLE_EQ(out.concentration[0],0); EXPECT_DOUBLE_EQ(out.held_dry_mass[0],4);
    MixingWater wet{{0},{1},{1},{0},{0},{0},{}};
    ASSERT_TRUE(solver.prepare(wet));
    ASSERT_TRUE(solver.solve({{0},{0},out.held_dry_mass,true},out).converged);
    EXPECT_NEAR(out.concentration[0],4,1e-12);EXPECT_DOUBLE_EQ(out.held_dry_mass[0],0);
}
TEST(ConservativeMixing, EvaporationKeepsSoluteIncludingDryResidue) {
    ConservativeMixing solver;MixedScalar out;
    ASSERT_TRUE(solver.prepare({{1},{.5},{0},{0},{0},{.5},{}}));
    ASSERT_TRUE(solver.solve({{10},{0},{},true},out).converged);EXPECT_NEAR(out.concentration[0],20,1e-12);
    ASSERT_TRUE(solver.prepare({{1,0},{1,0},{1,0},{0,0},{0,0},{0,1},{{0,1,1}}}));
    ASSERT_TRUE(solver.solve({{10,0},{10,0},{},true},out).converged);
    EXPECT_NEAR(out.held_dry_mass[1],10,1e-12);
}
TEST(ConservativeMixing, SeepageAndNegativeTemperatureHaveExplicitMass) {
    ConservativeMixing solver;MixedScalar out;
    ASSERT_TRUE(solver.prepare({{2},{1},{0},{0},{1},{0},{}}));
    ASSERT_TRUE(solver.solve({{-2},{0},{},false},out).converged);
    EXPECT_NEAR(out.concentration[0],-2,1e-12);EXPECT_NEAR(out.seeped_mass[0],-2,1e-12);
}
TEST(ConservativeMixing, InvalidWaterAndScalarCannotPublishPartialState) {
    ConservativeMixing solver;MixedScalar out;out.concentration={123};
    EXPECT_FALSE(solver.prepare({{1},{2},{0},{0},{0},{0},{}}));
    EXPECT_FALSE(solver.solve({{1},{0},{},true},out).converged);EXPECT_EQ(out.concentration[0],123);
    ASSERT_TRUE(solver.prepare(pipe()));
    EXPECT_FALSE(solver.solve({{0,10,0},{std::numeric_limits<double>::quiet_NaN(),0,0},{},true},out).converged);
    EXPECT_EQ(out.concentration[0],123);
}
}
