// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include "2d/gw/GwSourceForcing.hpp"
using namespace openswmm::twoD;
TEST(GwSourceForcing, IntegratesFlowConcentrationProductAcrossKnotsAndCadences) {
    GwResolvedSource source;source.flow.times={0,10};source.flow.values={0,2};
    GwResolvedSourceTerm term;term.row=0;term.signal.times={0,5,10};term.signal.values={1,2,3};source.terms={term};
    GwSourcePulse one;one.mass.resize(1);integrateGwSource(source,0,10,1,one);
    EXPECT_NEAR(one.in,10,1e-12);EXPECT_NEAR(one.mass[0],70./3,1e-12);
    GwSourcePulse split;split.mass.resize(1);for(int t=0;t<10;++t)integrateGwSource(source,t,1,1,split);
    EXPECT_NEAR(split.in,one.in,1e-12);EXPECT_NEAR(split.mass[0],one.mass[0],1e-12);
}
TEST(GwSourceForcing, SplitsSignedFlowAndScalesTotalWithoutScalingConcentrationTwice) {
    GwResolvedSource source;source.scale=.5;source.flow.times={0,10};source.flow.values={-2,2};
    GwResolvedSourceTerm term;term.row=0;term.signal.constant=4;source.terms={term};
    GwSourcePulse pulse;pulse.mass.resize(1);integrateGwSource(source,0,10,.25,pulse);
    EXPECT_NEAR(pulse.in,.625,1e-12);EXPECT_NEAR(pulse.out,.625,1e-12);EXPECT_NEAR(pulse.mass[0],2.5,1e-12);
}
TEST(GwSourceForcing, NativeMassRateConvertsToConcentrationTimesCubicMetres) {
    GwResolvedSource source;source.flow.constant=0;source.scale=.5;
    GwResolvedSourceTerm term;term.row=0;term.massRate=true;term.massScale=.001;term.signal.constant=1000;source.terms={term};
    GwSourcePulse pulse;pulse.mass.resize(1);integrateGwSource(source,0,4,.25,pulse);
    EXPECT_EQ(pulse.in,0);EXPECT_EQ(pulse.out,0);EXPECT_NEAR(pulse.mass[0],.5,1e-12);
}
TEST(GwSourceForcing, ResolvedSeriesBackingIsSharedAndIntervalLookupKeepsExactEndpoints) {
    auto series=std::make_shared<GwSourceSeries>();
    series->times={-1000,0,10,1000};series->values={0,0,2,2};
    GwResolvedSource a;a.flow.series=series;
    GwResolvedSource b=a;EXPECT_EQ(a.flow.series.get(),b.flow.series.get());
    GwSourcePulse pulse;integrateGwSource(b,0,10,1,pulse);
    EXPECT_NEAR(pulse.in,10,1e-12);EXPECT_EQ(b.flow.at(-2000),0);EXPECT_EQ(b.flow.at(2000),2);
}
