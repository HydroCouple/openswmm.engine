#include <gtest/gtest.h>
#include "fv_test_support.hpp"
using namespace fvtest;
namespace k=openswmm::fv::kernels;
namespace {
TEST(AcceptedWater, EulerRk2AndActualLtsCloseEveryCellAndPreserveStates) {
    for(int mode=0;mode<3;++mode) {
        auto a=makeWalledChannel(circular(9),80,2,[](double){return 0.;},0,25);
        for(int i=0;i<80;++i) {
            a.mesh.cell_dx[i]=i<40?.25:2.;
            a.state.cell_a[i]=k::areaOfDepth(a.mesh.geom[0],8.95+.15*std::sin(i*.2));
            a.state.cell_q[i]=5*std::sin(i*.3);
        }
        for(int f=0;f<=80;++f)a.mesh.face_dx[f]=f==0?a.mesh.cell_dx[0]/2:(f==80?a.mesh.cell_dx[79]/2:(a.mesh.cell_dx[f-1]+a.mesh.cell_dx[f])/2);
        auto b=a;FvOptions o;o.order=1;o.cfl=.1875;o.slot_celerity=25;o.lts=mode==2;o.time_integration=mode==1?TimeIntegration::RK2:TimeIntegration::EULER;
        ExplicitFvSolver tracked,untracked;tracked.initialize(a.mesh,a.state,o);untracked.initialize(b.mesh,b.state,o);tracked.trackAcceptedWater(true);
        FvStepForcing forcing;
        for(int step=0;step<4;++step) {
            auto initial=a.state.cell_a;
            ASSERT_NEAR(tracked.advance(step*.25,(step+1)*.25,forcing),(step+1)*.25,1e-12);
            ASSERT_NEAR(untracked.advance(step*.25,(step+1)*.25,forcing),(step+1)*.25,1e-12);
            EXPECT_EQ(a.state.cell_a,b.state.cell_a); EXPECT_EQ(a.state.cell_q,b.state.cell_q);
            const auto& r=tracked.acceptedWater();ASSERT_EQ(r.face_forward.size(),81);
            for(int c=0;c<80;++c) {
                double change=r.cell_lateral[c]-r.cell_loss[c];
                for(int e=0;e<2;++e) {
                    int f=e?a.mesh.cell_face1[c]:a.mesh.cell_face0[c];int side=e?a.mesh.cell_side1[c]:a.mesh.cell_side0[c];
                    change+=(side?1.:-1.)*(r.face_forward[f]-r.face_reverse[f]);
                }
                EXPECT_NEAR((a.state.cell_a[c]-initial[c])*a.mesh.cell_dx[c],change,1e-10) << "mode="<<mode<<" cell="<<c;
            }
        }
        if(mode==2)EXPECT_GT(tracked.run_stats().n_macro_cycles,0);
        EXPECT_TRUE(untracked.acceptedWater().face_forward.empty());
    }
}
TEST(AcceptedWater, ReceiptAveragingPreservesGrossDirectionsAndRollbackCopy) {
    openswmm::fv::AcceptedWaterVolumes r;r.resize(1,1,1,1);r.face(0,2);auto base=r;
    r.face(0,4);r.face(0,-6);r.structure(0,-8);r.cell_lateral[0]=10;r.node_deficit[0]=12;
    r.averageDelta(base);
    EXPECT_DOUBLE_EQ(r.face_forward[0],4);EXPECT_DOUBLE_EQ(r.face_reverse[0],3);
    EXPECT_DOUBLE_EQ(r.structure_reverse[0],4);EXPECT_DOUBLE_EQ(r.cell_lateral[0],5);EXPECT_DOUBLE_EQ(r.node_deficit[0],6);
    r=base;EXPECT_DOUBLE_EQ(r.face_forward[0],2);EXPECT_DOUBLE_EQ(r.face_reverse[0],0);
    r.clear();EXPECT_DOUBLE_EQ(r.face_forward[0],0);
}
}
TEST(AcceptedWater, UnavailableExtractionIsRefusedWithoutFutureWaterDebt) {
    for(int mode=0;mode<3;++mode) {
        auto ch=makeNodedChannel(rectOpen(10,10),4,10,[](double){return 0.;},0,25);
        FvOptions o;o.order=1;o.cfl=.1875;o.lts=mode==2;o.time_integration=mode==1?TimeIntegration::RK2:TimeIntegration::EULER;
        ExplicitFvSolver solver;solver.initialize(ch.mesh,ch.state,o);solver.trackAcceptedWater(true);
        double lateral[2]={-1,0},requested[2]={1,0},fixed[2]={std::numeric_limits<double>::quiet_NaN(),0};
        FvStepForcing f;f.node_lateral=lateral;f.node_withdrawal=requested;f.node_fixed_head=fixed;f.n_nodes=2;
        ASSERT_NEAR(solver.advance(0,.25,f),.25,1e-12);
        EXPECT_NEAR(solver.unmetWithdrawalVolumes()[0],.25,1e-12);
        EXPECT_GE(solver.node_continuity_carry()[0],-1e-12);
        lateral[0]=1;requested[0]=0;
        ASSERT_NEAR(solver.advance(.25,.5,f),.5,1e-12);
        double held=0;for(int c=0;c<4;++c)held+=ch.state.cell_a[c]*ch.mesh.cell_dx[c];
        for(double v:ch.state.node_volume)held+=v;
        for(double v:solver.node_continuity_carry())held+=std::min(0.0,v);
        EXPECT_NEAR(held+solver.node_exchange()[1],.25,1e-8);
        EXPECT_NEAR(solver.unmetWithdrawalVolumes()[0],0,1e-12);
    }
}
TEST(AcceptedWater, NodedObservationsAreNumericallyPassive) {
    for(int mode=0;mode<3;++mode) {
        auto a=makeNodedChannel(rectOpen(10,10),8,2,[](double){return 0.;},0,25);
        seedLevel(a,.5);auto b=a;FvOptions o;o.order=1;o.cfl=.1875;o.lts=mode==2;o.time_integration=mode==1?TimeIntegration::RK2:TimeIntegration::EULER;
        ExplicitFvSolver observed,plain;observed.initialize(a.mesh,a.state,o);plain.initialize(b.mesh,b.state,o);observed.trackAcceptedWater(true);
        double lateral[2]={2,0},fixed[2]={std::numeric_limits<double>::quiet_NaN(),.5};FvStepForcing f;f.node_lateral=lateral;f.node_fixed_head=fixed;f.n_nodes=2;
        for(int i=0;i<4;++i) {
            ASSERT_NEAR(observed.advance(i*.125,(i+1)*.125,f),(i+1)*.125,1e-12);
            ASSERT_NEAR(plain.advance(i*.125,(i+1)*.125,f),(i+1)*.125,1e-12);
            EXPECT_EQ(a.state.cell_a,b.state.cell_a);EXPECT_EQ(a.state.cell_q,b.state.cell_q);
            EXPECT_EQ(a.state.node_volume,b.state.node_volume);EXPECT_EQ(a.state.node_head,b.state.node_head);
            EXPECT_EQ(observed.node_continuity_carry(),plain.node_continuity_carry());
        }
    }
}
TEST(AcceptedWater, DummyWindowReceiptUsesTheAcceptedRk2Weight) {
    for(int mode=0;mode<3;++mode) {
        auto ch=makeNodedChannel(rectOpen(10,10),4,2,[](double){return 0.;},0,25);
        ch.mesh.node_kind={kNodeJunction,kNodeJunction,kNodeOutfall};
        ch.mesh.node_invert={0,0,0};ch.mesh.node_full_depth={1e6,1e6,0};ch.mesh.node_ponded_area={0,0,0};
        ch.mesh.node_area={1,1,0};ch.mesh.node_vol_off={-1,-1,-1};ch.mesh.node_vol_dmax={0,0,0};ch.mesh.node_vol_atop={0,0,0};
        ch.mesh.node_face_ptr={0,1,2,2};ch.mesh.node_dummy_drain={0,1,0};
        ch.mesh.struct_link={1};ch.mesh.struct_n1={1};ch.mesh.struct_n2={2};ch.mesh.struct_is_dummy={1};
        ch.state.resize(4,3,0);seedLevel(ch,.5);
        FvOptions o;o.order=1;o.cfl=.1875;o.lts=mode==2;o.time_integration=mode==1?TimeIntegration::RK2:TimeIntegration::EULER;
        ExplicitFvSolver solver;solver.initialize(ch.mesh,ch.state,o);solver.trackAcceptedWater(true);
        double lateral[3]={1,0,0},fixed[3]={std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::quiet_NaN(),0},caps[2]={-1,-1};
        FvStepForcing f;f.node_lateral=lateral;f.node_fixed_head=fixed;f.link_q_cap=caps;f.n_nodes=3;f.n_links=2;
        auto volume=[&](){double v=0;for(int c=0;c<4;++c)v+=ch.state.cell_a[c]*ch.mesh.cell_dx[c];for(double x:ch.state.node_volume)v+=x;for(double x:solver.node_continuity_carry())v+=std::min(0.0,x);return v;};
        for(int step=0;step<3;++step) {
            const double initial=volume();
            ASSERT_NEAR(solver.advance(step*.2,(step+1)*.2,f),(step+1)*.2,1e-12);
            const double exported=solver.acceptedWater().structure_forward[0]-solver.acceptedWater().structure_reverse[0];
            EXPECT_GT(exported,0);
            EXPECT_NEAR(solver.dummy_volume()[0],exported,1e-10) << "mode="<<mode;
            EXPECT_NEAR(initial+.2,volume()+exported,1e-9) << "mode="<<mode;
        }
    }
}
TEST(AcceptedWater, PrescribedStageCanSupplyAConservativeZeroStorageInterface) {
    for(int mode=0;mode<3;++mode) {
        auto ch=makeNodedChannel(rectOpen(10,10),8,2,[](double){return 0.;},0,25);
        seedLevel(ch,.5);FvOptions o;o.order=1;o.cfl=.1875;o.lts=mode==2;o.time_integration=mode==1?TimeIntegration::RK2:TimeIntegration::EULER;
        ExplicitFvSolver solver;solver.initialize(ch.mesh,ch.state,o);solver.trackAcceptedWater(true);solver.requireConservativeNodeBalance(true);
        double fixed[2]={1,.5};FvStepForcing f;f.node_fixed_head=fixed;f.n_nodes=2;
        double initial=0;for(int c=0;c<8;++c)initial+=ch.state.cell_a[c]*ch.mesh.cell_dx[c];
        ASSERT_NEAR(solver.advance(0,.25,f),.25,1e-12);
        EXPECT_LT(solver.node_exchange()[0],0) << "a reservoir must fund its accepted import";
        double final=0;for(int c=0;c<8;++c)final+=ch.state.cell_a[c]*ch.mesh.cell_dx[c];
        EXPECT_NEAR(initial,final+solver.node_exchange()[0]+solver.node_exchange()[1],1e-9);
    }
}
