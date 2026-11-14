#include <gtest/gtest.h>
#include "fv_test_support.hpp"
using namespace fvtest;
namespace k=openswmm::fv::kernels;
namespace {
TEST(FvScalarDryInventory, ThinCellsRetainUniformConcentrationAndConservedInventory) {
    for(int order:{1,2})for(bool rk2:{false,true}) {
        SCOPED_TRACE(::testing::Message()<<"order="<<order<<",rk2="<<rk2);
        auto ch=makeWalledChannel(circular(3),80,1,[](double){return 0.;},0,100);
        ch.state.resize(80,0,1);
        double water=0,mass=0;
        for(int i=0;i<80;++i) {
            ch.state.cell_a[i]=i<40?k::areaOfDepth(ch.mesh.geom[0],.8):0;
            ch.state.cell_phi[i]=7.25;water+=ch.state.cell_a[i];mass+=7.25*ch.state.cell_a[i];
        }
        FvOptions o;o.order=order;o.cfl=.1875;o.slot_celerity=100;o.time_integration=rk2?TimeIntegration::RK2:TimeIntegration::EULER;
        ExplicitFvSolver solver;solver.initialize(ch.mesh,ch.state,o);FvStepForcing f;
        ASSERT_NEAR(solver.advance(0,1,f),1,1e-12);
        double final_water=0,final_mass=0;
        for(int i=0;i<80;++i) {
            EXPECT_GE(ch.state.cell_a[i],0);EXPECT_TRUE(std::isfinite(ch.state.cell_q[i]));
            if(ch.state.cell_a[i]>k::kDryArea)EXPECT_NEAR(ch.state.cell_phi[i],7.25,1e-10);
            final_water+=ch.state.cell_a[i];final_mass+=ch.state.cell_a[i]*ch.state.cell_phi[i];
        }
        EXPECT_NEAR(final_water,water,1e-11);EXPECT_NEAR(final_mass,mass,1e-10);
    }
}
}
