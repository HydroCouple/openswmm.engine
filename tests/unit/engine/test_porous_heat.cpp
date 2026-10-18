// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include "2d/heat/PorousHeat.hpp"
#include "2d/heat/PorewaterTransport.hpp"
#include "2d/heat/GroundPorewater.hpp"
#include "2d/subsurface/SubsurfaceData.hpp"
#include "2d/gw/GwTransportSections.hpp"
#include <cmath>
#include <numeric>
using namespace openswmm::twoD;
namespace {
constexpr double pi=3.14159265358979323846;
std::vector<double> depths(int n,double length){std::vector<double> d(n+1);for(int i=0;i<=n;++i)d[i]=length*i/n;return d;}
double sum(const std::vector<double>& x){return std::accumulate(x.begin(),x.end(),0.);}
}
TEST(PorousHeat, MaterialPhaseCapacityAndMixingAreIndependentCalculations) {
    ThermalMaterial m;m.porosity=.4;
    EXPECT_DOUBLE_EQ(m.volumetricCapacity(0),.6*2650*880);
    EXPECT_DOUBLE_EQ(m.volumetricCapacity(.2),.6*2650*880+.2*4184000.);
    EXPECT_DOUBLE_EQ(m.volumetricCapacity(.4),.6*2650*880+.4*4184000.);
    EXPECT_DOUBLE_EQ(m.conductivity(.2),.6*2+.2*.606+.2*.025);
    m.geometric=true;
    EXPECT_NEAR(m.conductivity(.2),std::pow(2,.6)*std::pow(.606,.2)*std::pow(.025,.2),1e-15);
    m.air_conductivity=0;EXPECT_FALSE(m.validate().empty());
}
TEST(PorousHeat, FixedGridMoistureChangePreservesEnergyAndGrains) {
    PorousHeatColumn c;ThermalMaterial m;
    ASSERT_TRUE(c.initialize(50,{0,.25,1},m,{.1,.4},{-4,15}).empty());
    const auto energy=c.energy;const auto geometry=c.depth_edges;
    ASSERT_TRUE(c.setMoisture({.4,0}).empty());EXPECT_EQ(c.energy,energy);EXPECT_EQ(c.depth_edges,geometry);
    EXPECT_DOUBLE_EQ(c.capacity[1],50*.75*.6*2650*880);
    const auto cap=c.capacity;EXPECT_FALSE(c.setMoisture({.5,0}).empty());
    EXPECT_EQ(c.capacity,cap);EXPECT_EQ(c.energy,energy);
}
TEST(PorousHeat, BackwardEulerFinitePairConservesAndConvergesToAnalyticalDecay) {
    double previous=1e100;
    for(int steps:{1,4,16,64,256}) {
        std::vector<ThermalStore> s={{2e6,-2e6},{3e6,60e6}};
        const std::vector<ThermalContact> contacts={{0,1,1000}};
        for(int k=0;k<steps;++k) {
            auto r=solveThermalContacts(s,contacts,{},600./steps);ASSERT_TRUE(r.error.empty())<<r.error;
            EXPECT_NEAR(sum(r.energy),58e6,1e-7);
            EXPECT_NEAR(r.energy[0]-s[0].energy,-r.contact_j[0],1e-7);
            for(int i=0;i<2;++i)s[i].energy=r.energy[i];
        }
        const double difference=-21*std::exp(-1000*(1/2e6+1/3e6)*600);
        const double expected=58e6/5e6+3./5*difference;
        const double error=std::abs(s[0].energy/s[0].capacity-expected);
        EXPECT_LT(error,previous);previous=error;
    }
    EXPECT_LT(previous,.01);
}
TEST(PorousHeat, UniformDryInsulatedColumnAndArbitraryContactGraphDoNotDrift) {
    PorousHeatColumn c;ThermalMaterial m;
    ASSERT_TRUE(c.initialize(10,depths(20,2),m,std::vector<double>(20,0),std::vector<double>(20,-7)).empty());
    auto contacts=c.contacts();contacts.push_back({0,19,3});contacts.push_back({4,12,8});
    const auto original=c.energy;
    for(int k=0;k<10;++k) {
        const auto r=solveThermalContacts(c.stores(),contacts,{},7200);ASSERT_TRUE(r.error.empty())<<r.error;
        for(int i=0;i<20;++i)EXPECT_NEAR(r.temperature[i],-7,1e-11);
        c.energy=r.energy;
    }
    EXPECT_NEAR(sum(c.energy),sum(original),1e-6);
}
TEST(PorousHeat, SlabSineModeRefinesAgainstAnalyticalDiffusion) {
    double previous=1;
    for(int n:{10,20,40}) {
        ThermalMaterial m;m.porosity=0;m.grain_conductivity=2;m.grain_density=2000;m.grain_specific_heat=1000;
        std::vector<double> init(n);for(int i=0;i<n;++i)init[i]=std::sin(pi*(i+.5)/n);
        PorousHeatColumn c;ASSERT_TRUE(c.initialize(1,depths(n,1),m,std::vector<double>(n,0),init).empty());
        const double duration=20000,dt=duration/(n*n),kappa=1e-6;
        const auto edges=c.contacts();
        const std::vector<ThermalBoundary> bc={{0,2/(.5/n),0,0},{n-1,2/(.5/n),0,0}};
        for(int k=0;k<n*n;++k){auto r=solveThermalContacts(c.stores(),edges,bc,dt);ASSERT_TRUE(r.error.empty())<<r.error;c.energy=r.energy;}
        double error=0;
        for(int i=0;i<n;++i)error=std::max(error,std::abs(c.energy[i]/c.capacity[i]-init[i]*std::exp(-kappa*pi*pi*duration)));
        EXPECT_LT(error,previous);previous=error;
    }
    EXPECT_LT(previous,.01);
}
TEST(PorousHeat, GeothermalFluxAndMaterialInterfaceUseSignedReciprocalJoules) {
    const std::vector<ThermalStore> stores={{1e6,-1e6},{2e6,8e6},{3e6,30e6}};
    // Independent series resistance: A=2, half-widths .1/.2, k=1/4.
    const double g=2/(.1/1+.2/4);
    const auto r=solveThermalContacts(stores,{{0,1,g},{1,2,5}},{{2,0,0,.065*2}},7200);
    ASSERT_TRUE(r.error.empty())<<r.error;
    EXPECT_NEAR(sum(r.energy)-37e6,.065*2*7200,1e-6);
    EXPECT_NEAR(r.boundary_j[0],.065*2*7200,1e-12);
    EXPECT_NEAR(r.contact_j[0],g*(r.temperature[0]-r.temperature[1])*7200,1e-5);
    EXPECT_NEAR(r.energy[0]-stores[0].energy,-r.contact_j[0],1e-7);
}
TEST(PorousHeat, InvalidInputsAreRefusedWithoutPublishingPartialEnergy) {
    const auto r=solveThermalContacts({{0,1}}, {},{},60);
    EXPECT_FALSE(r.error.empty());EXPECT_TRUE(r.energy.empty());
    const auto b=solveThermalContacts({{1,1}},{{0,2,1}},{},60);
    EXPECT_FALSE(b.error.empty());EXPECT_TRUE(b.energy.empty());
}
TEST(PorousHeat, DailySurfaceSignalHasAnalyticalAttenuationAndPhaseAtDepth) {
    constexpr int n=100,steps=1440;
    constexpr double period=86400,dt=60,amplitude=10,kappa=1e-6;
    const double omega=2*pi/period,delta=std::sqrt(2*kappa/omega);
    ThermalMaterial m;m.porosity=0;m.grain_density=2000;m.grain_specific_heat=1000;m.grain_conductivity=2;
    std::vector<double> initial(n);
    for(int j=0;j<n;++j){const double z=(j+.5)/n;initial[j]=-amplitude*std::exp(-z/delta)*std::sin(z/delta);}
    PorousHeatColumn c;ASSERT_TRUE(c.initialize(1,depths(n,1),m,std::vector<double>(n,0),initial).empty());
    double sine[2]{},cosine[2]{};const int sample[2]={15,32};
    const auto contacts=c.contacts();
    for(int k=1;k<=3*steps;++k) {
        const double t=k*dt;
        std::vector<ThermalBoundary> bc={{0,400,amplitude*std::sin(omega*t),0},
            {n-1,400,amplitude*std::exp(-1/delta)*std::sin(omega*t-1/delta),0}};
        auto r=solveThermalContacts(c.stores(),contacts,bc,dt);ASSERT_TRUE(r.error.empty())<<r.error;
        c.energy=r.energy;
        if(k>2*steps)for(int a=0;a<2;++a){sine[a]+=2*r.temperature[sample[a]]*std::sin(omega*t)/steps;
                                             cosine[a]+=2*r.temperature[sample[a]]*std::cos(omega*t)/steps;}
    }
    for(int a=0;a<2;++a) {
        const double z=(sample[a]+.5)/n,expected=amplitude*std::exp(-z/delta);
        EXPECT_NEAR(std::hypot(sine[a],cosine[a]),expected,.03*expected);
        EXPECT_NEAR(std::atan2(-cosine[a],sine[a]),z/delta,.02*2*pi);
    }
}
TEST(PorousHeat, MultipleFiniteOwnersShareTheGroundWithoutDuplicatingItsCapacity) {
    const std::vector<ThermalStore> stores={{4e6,80e6},{1e6,-1e6},{2e6,16e6},{5e6,50e6}};
    const auto r=solveThermalContacts(stores,{{0,2,100},{1,2,200},{0,3,50},{2,3,400}}, {},7200);
    ASSERT_TRUE(r.error.empty())<<r.error;
    EXPECT_NEAR(sum(r.energy),145e6,1e-6);
    for(double t:r.temperature){EXPECT_GE(t,-1);EXPECT_LE(t,20);}
    EXPECT_NEAR(r.energy[0]-stores[0].energy,-r.contact_j[0]-r.contact_j[2],1e-7);
    EXPECT_NEAR(r.energy[2]-stores[2].energy,r.contact_j[0]+r.contact_j[1]-r.contact_j[3],1e-7);
}
TEST(PorousHeat, AcceptedInfiltrationExtractionAndRedistributionPreserveIsothermalState) {
    ThermalMaterial m;
    for(double temperature:{-3.,20.}) {
        PorousHeatColumn c;ASSERT_TRUE(c.initialize(50,{0,.5,1},m,{.1,.3},{temperature,temperature}).empty());
        const double initial=sum(c.energy);
        // Add 2.5 m3 at the top; fixed layer theta changes also imply a
        // physical 1.25 m3 displacement into the lower thermal layer.
        const double heat=2.5*4184000.*temperature;
        auto r=advectPorousColumn(c,{.15,.35},{2.5,0},{heat,0});
        ASSERT_TRUE(r.error.empty())<<r.error;EXPECT_NEAR(sum(c.energy)-initial,heat,1e-6);
        EXPECT_NEAR(r.vertical_water[0],1.25,1e-12);
        for(int j=0;j<2;++j)EXPECT_NEAR(c.energy[j]/c.capacity[j],temperature,1e-11);
        r=advectPorousColumn(c,{.1,.3},{-1.25,-1.25},{-heat/2,-heat/2});
        ASSERT_TRUE(r.error.empty())<<r.error;EXPECT_NEAR(sum(c.energy),initial,1e-6);
        for(int j=0;j<2;++j)EXPECT_NEAR(c.energy[j]/c.capacity[j],temperature,1e-11);
        // Pure internal water redistribution has no external energy.
        r=advectPorousColumn(c,{.2,.2},{0,0},{0,0});ASSERT_TRUE(r.error.empty())<<r.error;
        EXPECT_NEAR(sum(c.energy),initial,1e-6);
        for(int j=0;j<2;++j)EXPECT_NEAR(c.energy[j]/c.capacity[j],temperature,1e-11);
    }
}
TEST(PorousHeat, UnpairedWaterIsRefusedAtomically) {
    PorousHeatColumn c;ThermalMaterial m;
    ASSERT_TRUE(c.initialize(1,{0,.5,1},m,{.2,.2},{0,20}).empty());
    const auto energy=c.energy,theta=c.theta,capacity=c.capacity;
    const auto r=advectPorousColumn(c,{.3,.3},{0,0},{0,0});
    EXPECT_FALSE(r.error.empty());EXPECT_EQ(c.energy,energy);EXPECT_EQ(c.theta,theta);EXPECT_EQ(c.capacity,capacity);
}
TEST(PorousHeat, ImplicitExternalWithdrawalsUseLayerTemperaturesAndPreserveExtrema) {
    ThermalMaterial m;PorousHeatColumn c;
    ASSERT_TRUE(c.initialize(1,{0,.5,1},m,{.4,.4},{0,40}).empty());const double initial=sum(c.energy);
    const auto solved=advectPorousColumn(c,{.38,.38},{-.01,-.01},{0,0},{.01,.01});
    ASSERT_TRUE(solved.error.empty())<<solved.error;
    EXPECT_NEAR(c.energy[0]/c.capacity[0],0,1e-12);EXPECT_NEAR(c.energy[1]/c.capacity[1],40,1e-12);
    EXPECT_NEAR(sum(c.energy)-initial,-.01*4184000.*40,1e-6);
    EXPECT_NEAR(solved.external_j,-sum(solved.withdrawn_j),1e-8);
    const auto energy=c.energy,theta=c.theta;
    EXPECT_FALSE(advectPorousColumn(c,{.38,.38},{0,0},{0,0},{-.01,0}).error.empty());
    EXPECT_EQ(c.energy,energy);EXPECT_EQ(c.theta,theta);
}
TEST(PorousHeat, LargeBoundaryWithdrawalsAndInternalRedistributionKeepSignedDonorsConservative) {
    ThermalMaterial m;PorousHeatColumn c;
    ASSERT_TRUE(c.initialize(1,{0,.1,.5,1},m,{.4,.4,.4},{-10,5,40}).empty());
    const double initial=sum(c.energy);
    // More than the top layer's initial .04 m3 is withdrawn. Accepted
    // redistribution supplies it from below; the implicit donor must remain
    // finite and bounded without restricting or inventing hydraulic water.
    const auto solved=advectPorousColumn(c,{.2,.3,.3},{-.08,0,-.03},{0,0,0},{.08,0,.03});
    ASSERT_TRUE(solved.error.empty())<<solved.error;
    for(std::size_t j=0;j<c.energy.size();++j) {
        EXPECT_GE(c.energy[j]/c.capacity[j],-10-1e-12);EXPECT_LE(c.energy[j]/c.capacity[j],40+1e-12);
    }
    EXPECT_NEAR(sum(c.energy)-initial,-sum(solved.withdrawn_j),1e-7);
    EXPECT_GT(solved.temperature.front(),-10);EXPECT_NEAR(solved.temperature.back(),40,1e-12);
}
TEST(PorewaterTransport, WithdrawingWaterAtLocalConcentrationRetainsTheSorptionStore) {
    const auto out=advectPorewater({.2,.3},{.1,.2},{.4,.5},{1.2,8},{-.1,-.1},{0,0},{.1,.1});
    ASSERT_TRUE(out.error.empty())<<out.error;
    EXPECT_NEAR(out.concentration[0],2,1e-12);EXPECT_NEAR(out.concentration[1],10,1e-12);
    EXPECT_NEAR(out.mass[0],1,1e-12);EXPECT_NEAR(out.mass[1],7,1e-12);
    EXPECT_NEAR(sum(out.mass)+sum(out.withdrawn_mass),9.2,1e-12);
}
TEST(PorewaterTransport, EvaporationRetainsDryResidueAndRewettingDissolvesItWithoutCreatingMass) {
    const auto dry=advectPorewater({.1},{0},{0},{3},{-.1},{0},{0});
    ASSERT_TRUE(dry.error.empty())<<dry.error;ASSERT_EQ(dry.mass.size(),1);EXPECT_DOUBLE_EQ(dry.mass[0],3);
    EXPECT_DOUBLE_EQ(dry.concentration[0],0);EXPECT_DOUBLE_EQ(dry.withdrawn_mass[0],0);
    const auto wet=advectPorewater({0},{.2},{0},dry.mass,{.2},{1},{0});
    ASSERT_TRUE(wet.error.empty())<<wet.error;EXPECT_NEAR(wet.mass[0],4,1e-12);EXPECT_NEAR(wet.concentration[0],20,1e-12);
}
TEST(PorewaterTransport, ZeroFinalWaterThroughflowExportsTheFiniteIncomingPacket) {
    const auto out=advectPorewater({0,0},{0,0},{0,0},{0,0},{1,-1},{7,0},{0,1});
    ASSERT_TRUE(out.error.empty())<<out.error;
    EXPECT_NEAR(out.concentration[0],7,1e-12);EXPECT_NEAR(out.concentration[1],7,1e-12);
    EXPECT_NEAR(sum(out.mass),0,1e-12);EXPECT_NEAR(sum(out.withdrawn_mass),7,1e-12);
    ASSERT_EQ(out.vertical_mass.size(),1);EXPECT_NEAR(out.vertical_mass[0],7,1e-12);
}
TEST(PorewaterTransport, WaterCanEvaporateAfterThroughflowAndLeaveItsSoluteInADryLayer) {
    const auto out=advectPorewater({0,0},{0,0},{0,0},{0,0},{1,-1},{7,0},{0,0});
    ASSERT_TRUE(out.error.empty())<<out.error;
    EXPECT_NEAR(out.mass[0],0,1e-12);EXPECT_NEAR(out.mass[1],7,1e-12);EXPECT_NEAR(out.external_mass,7,1e-12);
}
TEST(PorewaterTransport, AContinuousCleanWaterFlushConvergesToTheIndependentResidenceDecay) {
    double previous=1;
    for(int steps:{1,10,100,1000}) {
        std::vector<double> mass{10};const double dt=1./steps;
        for(int k=0;k<steps;++k) {
            const auto out=advectPorewater({1},{1},{0},mass,{0},{0},{dt});
            ASSERT_TRUE(out.error.empty())<<out.error;mass=out.mass;
        }
        const double error=std::abs(mass[0]/10-std::exp(-1.));
        EXPECT_LT(error,previous);previous=error;
        if(steps==1000)EXPECT_LT(error,.0002);
    }
}
TEST(PorewaterTransport, LargeRedistributionAndExtractionRemainNonnegativeAndConservative) {
    const auto out=advectPorewater({.04,.16,.2},{.02,.12,.15},{0,.1,.2},{.04,1.6,40},
                                  {-.08,0,-.03},{0,0,0},{.08,0,.03});
    ASSERT_TRUE(out.error.empty())<<out.error;
    for(double value:out.mass)EXPECT_GE(value,0);
    for(double value:out.concentration){EXPECT_GE(value,0);EXPECT_LE(value,100+1e-12);}
    EXPECT_NEAR(sum(out.mass)+sum(out.withdrawn_mass),41.64,1e-11);
}
TEST(PorewaterTransport, InvalidOrUnpairedTransfersPublishNoPartialSolution) {
    for(const auto out:{advectPorewater({.1},{.2},{0},{1},{0},{0},{0}),
                        advectPorewater({.1},{.1},{0},{1},{0},{0},{-1}),
                        advectPorewater({.1},{.1},{0},{1},{0},{-2},{0})}) {
        EXPECT_FALSE(out.error.empty());EXPECT_TRUE(out.mass.empty());EXPECT_TRUE(out.concentration.empty());
    }
}
TEST(PorewaterTransport, MixedDryWetAndSorbingColumnsPreserveMassAndConcentrationExtrema) {
    unsigned seed=1234567;
    const auto unit=[&](){seed=1664525u*seed+1013904223u;return double(seed%10000)/10000.;};
    for(int trial=0;trial<128;++trial) {
        const std::size_t n=1+trial%16;SCOPED_TRACE(trial);
        std::vector<double> old(n),next(n),retention(n),mass(n),water(n),input(n),withdrawal(n),face(n+1,0);
        for(std::size_t j=1;j<n;++j)face[j]=2*unit()-1;
        double initial=0;
        for(std::size_t j=0;j<n;++j) {
            old[j]=(trial+int(j))%3?unit():0;next[j]=(trial+int(j))%4?unit():0;
            retention[j]=(trial+int(j))%2?unit():0;
            mass[j]=20*unit()*(old[j]+retention[j]);initial+=mass[j];
            water[j]=next[j]-old[j]+face[j+1]-face[j];
            input[j]=std::max(0.,water[j])*20*unit();withdrawal[j]=std::max(0.,-water[j]);
        }
        const auto out=advectPorewater(old,next,retention,mass,water,input,withdrawal);
        ASSERT_TRUE(out.error.empty())<<out.error;
        for(double value:out.mass)EXPECT_GE(value,0);
        for(double value:out.concentration){EXPECT_GE(value,0);EXPECT_LE(value,20+1e-10);}
        EXPECT_NEAR(sum(out.mass)+sum(out.withdrawn_mass),initial+sum(input),1e-10*std::max(1.,initial+sum(input)));
    }
}
TEST(GroundPorewater, InitialPartitionPreservesExistingTotalMassWithoutInventingSorbedMass) {
    ThermalMaterial material;material.porosity=.4;material.grain_density=2600;PorousHeatColumn soil;
    ASSERT_TRUE(soil.initialize(1,{0,.5,1},material,{.1,.4},{-3,-3}).empty());
    GroundPorewaterColumn pore;
    ASSERT_TRUE(pore.initialize(soil,{0,.2},{8,-.6},{1,-.15},{.001,0},-1,1).empty());
    ASSERT_EQ(pore.mass.size(),4);EXPECT_NEAR(pore.mass[0],1,1e-12);EXPECT_NEAR(pore.mass[1],8,1e-12);
    EXPECT_DOUBLE_EQ(pore.mass[2],0);EXPECT_DOUBLE_EQ(pore.mass[3],0); // no second heat store
    EXPECT_NEAR(pore.retention[0],.78,1e-12);EXPECT_NEAR(sum(pore.initial_mass),9,1e-12);
    const auto zones=pore.zoneMasses(soil,{0,.2});EXPECT_NEAR(zones.first[0],8,1e-12);EXPECT_NEAR(zones.second[0],1,1e-12);
}
TEST(GroundPorewater, ALayerCrossingTheTableHasOneMassAndConservesBothDerivedZoneViews) {
    ThermalMaterial material;PorousHeatColumn soil;
    ASSERT_TRUE(soil.initialize(1,{0,1},material,{.3},{10}).empty());
    GroundPorewaterColumn pore;ASSERT_TRUE(pore.initialize(soil,{.2},{4},{2},{.001},-1,-1).empty());
    ASSERT_EQ(pore.mass.size(),1);EXPECT_NEAR(pore.mass[0],6,1e-12);
    const auto zones=pore.zoneMasses(soil,{.2});EXPECT_GT(zones.first[0],0);EXPECT_GT(zones.second[0],0);
    EXPECT_NEAR(zones.first[0]+zones.second[0],6,1e-12);
    const auto before=pore.mass;ASSERT_TRUE(soil.setMoisture({.2}).empty());
    const auto shifted=pore.zoneMasses(soil,{.1});EXPECT_NEAR(shifted.first[0]+shifted.second[0],6,1e-12);
    EXPECT_EQ(pore.mass,before);
}
TEST(GroundPorewater, AgeNeverSorbsAndGrowsOnlyByActualPorewaterOnItsOwnerClock) {
    ThermalMaterial material;PorousHeatColumn soil;
    ASSERT_TRUE(soil.initialize(2,{0,.5,1},material,{.1,.4},{0,0}).empty());
    GroundPorewaterColumn pore;ASSERT_TRUE(pore.initialize(soil,{0,.4},{10},{2},{100},0,-1).empty());
    EXPECT_DOUBLE_EQ(sum(pore.retention),0);const double before=sum(pore.mass);
    ASSERT_TRUE(pore.generateAge(soil,17));EXPECT_NEAR(sum(pore.mass)-before,17*.5,1e-12);
    EXPECT_NEAR(sum(pore.generated_age),17*.5,1e-12);
}
TEST(GroundPorewater, DryResidueRemainsInTheDerivedPhysicalZoneAndInvalidSeedsAreAtomic) {
    ThermalMaterial material;PorousHeatColumn soil;
    ASSERT_TRUE(soil.initialize(1,{0,1},material,{0},{0}).empty());
    GroundPorewaterColumn pore;ASSERT_TRUE(pore.initialize(soil,{0},{0},{0},{0},-1,-1).empty());pore.mass[0]=3;
    const auto zones=pore.zoneMasses(soil,{0});EXPECT_DOUBLE_EQ(zones.first[0],0);EXPECT_DOUBLE_EQ(zones.second[0],3);
    const auto before=pore.mass;
    EXPECT_FALSE(pore.initialize(soil,{0},{0},{1},{0},-1,-1).empty());EXPECT_EQ(pore.mass,before);
    EXPECT_FALSE(pore.initialize(soil,{0},{0},{0},{-1},-1,-1).empty());EXPECT_EQ(pore.mass,before);
}
TEST(GroundPorewater, ReservedExportCannotBeSpentAgainByAFiniteContact) {
    ThermalMaterial material;PorousHeatColumn soil;
    ASSERT_TRUE(soil.initialize(1,{0,1},material,{.4},{10}).empty());
    GroundPorewaterColumn pore;ASSERT_TRUE(pore.initialize(soil,{.4},{10},{0},{0},-1,-1).empty());
    ASSERT_TRUE(pore.reserveOutgoing(0,0,4));
    EXPECT_DOUBLE_EQ(pore.contactAvailableMass(0,0),6);
    EXPECT_FALSE(pore.applyContactMassIncrement(0,0,-7));
    EXPECT_DOUBLE_EQ(pore.mass[0],10);EXPECT_DOUBLE_EQ(pore.reserved_out[0],4);
    ASSERT_TRUE(pore.applyContactMassIncrement(0,0,-3));
    EXPECT_DOUBLE_EQ(pore.mass[0],7);EXPECT_DOUBLE_EQ(pore.contactAvailableMass(0,0),3);
    // The cell's existing gather removes the queued four-unit packet from
    // canonical mass, then releases exactly the matching reservation.
    pore.mass[0]-=4;
    ASSERT_TRUE(pore.settleOutgoingReservation(0,0,4));
    EXPECT_DOUBLE_EQ(pore.mass[0],3);EXPECT_DOUBLE_EQ(pore.contactAvailableMass(0,0),3);
    EXPECT_DOUBLE_EQ(pore.reserved_out[0],0);
}
TEST(PorousHeat, ClosureMoistureProjectionConservesWaterAcrossMovingAndCollapsedLayers) {
    for(auto closure:{GwClosure::CLOSED_FORM,GwClosure::ENSLAVED,GwClosure::SIGMA})
        for(double hg:{0.,.1,.75,1.,1.999999,2.}) {
            SubsurfaceState s;s.resize(1,4);s.zs[0]=2;s.area[0]=10;s.theta_s[0]=.4;
            s.closure[0]=static_cast<int8_t>(closure);s.hg[0]=hg;s.hu[0]=(2-hg)*.25;
            s.theta_sigma={.1,.2,.3,.4};
            std::vector<double> theta,sat;
            ASSERT_TRUE(projectGroundMoisture(s,0,hg,depths(13,2),theta,&sat).empty());
            double water=0;for(double v:theta)water+=v*(2./13)*10;
            EXPECT_NEAR(water,10*((2-hg)*.25+hg*.4),1e-12);
            EXPECT_NEAR(sum(sat),10*hg*.4,1e-12);
            for(double v:theta){EXPECT_GE(v,0);EXPECT_LE(v,.4);}
        }
}
TEST(PorousHeat, ModelSelectionIsExplicitAndNewDefaultsStayUnauthored) {
    GwTransportOptions o;
    EXPECT_EQ(o.thermal_model,"WATER_ONLY");EXPECT_FALSE(o.lateral_conduction);
    EXPECT_TRUE(o.conduction);EXPECT_TRUE(o.thermal_authored.empty());
    ASSERT_TRUE(parseGwTransportOptionsLine({"THERMAL_MODEL","POROUS_EQUILIBRIUM"},o).empty());
    EXPECT_EQ(o.thermal_model,"POROUS_EQUILIBRIUM");EXPECT_EQ(o.thermal_authored.size(),1);
    ASSERT_TRUE(parseGwTransportOptionsLine({"THERMAL_LAYERS","32"},o).empty());EXPECT_EQ(o.thermal_layers,32);
    EXPECT_FALSE(parseGwTransportOptionsLine({"THERMAL_LAYERS","1e30"},o).empty());EXPECT_EQ(o.thermal_layers,32);
    EXPECT_FALSE(parseGwTransportOptionsLine({"AIR_THERMAL_CONDUCTIVITY","0"},o).empty());
    ASSERT_TRUE(parseGwTransportOptionsLine({"SURFACE_THERMAL_BC","HEAT_FLUX","-10"},o).empty());
    EXPECT_EQ(o.surface_thermal_bc,"HEAT_FLUX");EXPECT_EQ(o.surface_thermal_arg,"-10");
    ASSERT_TRUE(parseGwTransportOptionsLine({"DEEP_THERMAL_BC","ADIABATIC"},o).empty());
    EXPECT_EQ(o.deep_thermal_bc,"ADIABATIC");EXPECT_TRUE(o.deep_thermal_arg.empty());
}
