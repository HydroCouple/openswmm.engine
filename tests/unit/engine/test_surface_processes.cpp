// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include <gtest/gtest.h>
#include "hydrology/surface/InfilBank.hpp"
#include "hydrology/surface/DegreeDaySnow.hpp"
#include "hydrology/Runoff.hpp"
#include "core/SimulationContext.hpp"
#include "core/UnitConversion.hpp"
#include "core/DateTime.hpp"
#include "2d/infil/Infil2D.hpp"
#include "2d/data/MeshData.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_subcatchments.h>
#include <openswmm/engine/openswmm_model.h>
#include <filesystem>
#include <array>
#include <cstring>
using namespace openswmm;

namespace {
std::array<double,5> parameters(InfilModel method, bool si) {
    const double scale = si ? 25.4 : 1.0;
    switch (method) {
    case InfilModel::HORTON: case InfilModel::MOD_HORTON:
        return {3*scale, .4*scale, 4, .1, 10*scale};
    case InfilModel::GREEN_AMPT: case InfilModel::MOD_GREEN_AMPT:
        return {4*scale, .5*scale, .3, 0, 0};
    case InfilModel::CURVE_NUM: return {75, 0, .1, 0, 0};
    case InfilModel::CONSTANT: return {.5*scale, 0, 0, 0, 0};
    }
    return {};
}
}

// Exercise the actual runoff and cell drivers on the same ponded-water/rain
// history, rather than comparing two direct calls to the common dispatch.
TEST(SurfaceProcesses, SubcatchmentAndCellAgreeThroughWetDryWetEventsBothUnitSystems) {
    for (auto factors : {surface::InfilBank::Factors{1,1}, surface::InfilBank::Factors{.4,.2}})
    for (bool si : {false, true}) for (int m=0; m<5; ++m) {
        SCOPED_TRACE(::testing::Message() << "SI=" << si << " method=" << m);
        SimulationContext ctx;
        ctx.options.flow_units = si ? FlowUnits::CMS : FlowUnits::CFS;
        ctx.options.ignore_snow_melt = true;
        ctx.subcatch_names.add("S"); ctx.subcatches.resize(1);
        ctx.gage_names.add("G"); ctx.gages.resize(1); ctx.subcatches.gage[0] = 0;
        ctx.subcatches.area[0] = 1; ctx.subcatches.frac_imperv[0] = 0;
        ctx.subcatches.n_perv[0] = .1; ctx.subcatches.slope[0] = 0;
        ctx.subcatches.ds_perv[0] = 1000; // hold water; no runoff loss
        const auto p=parameters(static_cast<InfilModel>(m),si);
        ctx.subcatches.infil_model[0] = m;
        ctx.subcatches.infil_p1[0]=p[0]; ctx.subcatches.infil_p2[0]=p[1];
        ctx.subcatches.infil_p3[0]=p[2]; ctx.subcatches.infil_p4[0]=p[3]; ctx.subcatches.infil_p5[0]=p[4];
        runoff::RunoffSolver runoff; runoff.init(ctx);
        twoD::MeshData mesh; mesh.resize_triangles(1);
        twoD::SurfaceStateData state; state.resize(1,0);
        twoD::Infil2D infil; twoD::Infil2DDefault d; d.tag="*"; d.row.has_method=true;
        d.row.method=static_cast<InfilModel>(m); std::copy(p.begin(),p.end(),d.row.p); infil.defaults().push_back(d);
        std::string error; ASSERT_TRUE(infil.resolve(mesh,ctx.options,error)) << error;
        double cumulative1=0, cumulative2=0;
        for (int k=0;k<180;++k) {
            const double rain = k<40 || k>=120 ? 5.0/43200.0 : 0.0;
            ctx.gages.rainfall[0]=rain*ucf::UCF(ucf::RAINFALL,ctx.options);
            state.rainfall[0]=rain*.3048; state.depth[0]=runoff.soa().depth_perv[0]*.3048;
            infil.updateRates(mesh,state,60,factors);
            runoff.execute(ctx,60,0,factors.infiltration,factors.recovery);
            const double depth1 = runoff.soa().infil_vol[0]/runoff.soa().area[0]*.3048;
            const double depth2 = state.infil_rate[0]*60;
            EXPECT_NEAR(depth1,depth2,1e-12) << "step=" << k;
            cumulative1+=depth1; cumulative2+=depth2;
        }
        EXPECT_GT(cumulative1,0); EXPECT_NEAR(cumulative1,cumulative2,1e-12);
    }
}

TEST(SurfaceProcesses, ConstantCellMatchesPhysicalRateAndSharedSingleElementBothUnits) {
    for (bool si : {false,true}) {
        SimulationOptions options; options.flow_units=si?FlowUnits::CMS:FlowUnits::CFS;
        auto p=parameters(InfilModel::CONSTANT,si);
        surface::InfilBank bank; bank.init(1); bank.setMethod(0,InfilModel::CONSTANT,p.data(),options);
        twoD::MeshData mesh; mesh.resize_triangles(1); twoD::SurfaceStateData s; s.resize(1,0); s.depth[0]=1;
        twoD::Infil2D infil; twoD::Infil2DDefault d; d.tag="*"; d.row.has_method=true;
        d.row.method=InfilModel::CONSTANT; std::copy(p.begin(),p.end(),d.row.p); infil.defaults().push_back(d);
        std::string err; ASSERT_TRUE(infil.resolve(mesh,options,err)); infil.updateRates(mesh,s,60);
        const double f=bank.rate(0,0,0,1,60,{1,1});
        EXPECT_NEAR(f,.5*.0254/3600,1e-15); EXPECT_DOUBLE_EQ(s.infil_rate[0],f);
    }
}

TEST(SurfaceProcesses, ExternalOwnerKeepsPublishedRateAndKernelStateUntouched) {
    twoD::MeshData mesh; mesh.resize_triangles(1); twoD::SurfaceStateData s; s.resize(1,0);
    SimulationOptions opts; twoD::Infil2D infil; twoD::Infil2DDefault d; d.tag="*";
    d.row.has_method=true; d.row.method=InfilModel::GREEN_AMPT; d.row.p[0]=4; d.row.p[1]=1; d.row.p[2]=.3;
    infil.defaults().push_back(d); std::string err; ASSERT_TRUE(infil.resolve(mesh,opts,err));
    int m; double before[6],after[6]; infil.bank().pack(0,m,before);
    infil.bank().setOwner(0,surface::InfilBank::Owner::EXTERNAL);
    s.rainfall[0]=1e-5; s.depth[0]=.1; s.infil_rate[0]=2e-6; infil.updateRates(mesh,s,300);
    EXPECT_DOUBLE_EQ(s.infil_rate[0],2e-6); EXPECT_DOUBLE_EQ(infil.cumulative()[0],0);
    infil.bank().pack(0,m,after); EXPECT_EQ(std::memcmp(before,after,sizeof before),0);
    EXPECT_DOUBLE_EQ(infil.bank().rate(0,1e-5,0,.1,300,{1,1}),0);
}

TEST(SurfaceProcesses, InfiltrationPackRestoresContinuationForEveryStatefulMethod) {
    SimulationOptions opts;
    for (int m=0;m<5;++m) {
        auto p=parameters(static_cast<InfilModel>(m),false);
        surface::InfilBank a,b; a.init(1);b.init(1);a.setMethod(0,static_cast<InfilModel>(m),p.data(),opts);b.setMethod(0,static_cast<InfilModel>(m),p.data(),opts);
        for(int k=0;k<20;++k) a.rate(0,2e-5,0,.01,60,{.7,1.3});
        int model;double state[6];a.pack(0,model,state);b.unpack(0,model,state);
        for(int k=0;k<20;++k) EXPECT_DOUBLE_EQ(a.rate(0,k<10?0:2e-5,0,0,60,{.7,1.3}),b.rate(0,k<10?0:2e-5,0,0,60,{.7,1.3}));
    }
}

TEST(SurfaceProcesses, SnowContractPacksEveryTransientFieldAndBalancesPointElement) {
    surface::DegreeDaySnow a,b; surface::SnowModel& model=a; model.initializeElements(3);b.init(1);
    auto& s=a.state();s.wsnow[0]=.1;s.fw[0]=.01;s.si[0]=.1;s.fwfrac[0]=.05;
    s.dhm[0]=1e-6;s.tbase[0]=32;s.track_age=true;s.age[0]=200;s.precip_age=10;
    const double old=(s.wsnow[0]+s.fw[0])*.3048;
    surface::SnowInputs input;input.dt_s=60;input.Ta_C=5;input.rain_m=.0001;input.snow_m=.0002;
    surface::SnowOutputs output;model.step(0,input,output);
    EXPECT_NEAR(old+input.rain_m+input.snow_m-output.melt_m-output.throughfall_m-output.swe_m,0,1e-15);
    double state[12],copy[12];model.pack(0,state);b.unpack(0,state);b.pack(0,copy);
    EXPECT_EQ(std::memcmp(state,copy,sizeof state),0);
    // Fresh snowfall is mixed at source age; it is not aged before arrival.
    EXPECT_LT(a.state().age[0],260);
}

TEST(SurfaceProcesses, SnowpackAssignmentValidationAndRenameAreAtomic) {
    SWMM_Engine e=swmm_engine_new();ASSERT_TRUE(e);
    ASSERT_EQ(swmm_subcatch_add(e,"S"),0);ASSERT_EQ(swmm_snowpack_add(e,"Pack"),0);
    EXPECT_STREQ(swmm_subcatch_get_snowpack(e,0),"");
    EXPECT_EQ(swmm_subcatch_set_snowpack(e,0,"Pack"),0);EXPECT_STREQ(swmm_subcatch_get_snowpack(e,0),"Pack");
    EXPECT_NE(swmm_subcatch_set_snowpack(e,0,"Missing"),0);EXPECT_STREQ(swmm_subcatch_get_snowpack(e,0),"Pack");
    EXPECT_EQ(swmm_snowpack_rename(e,0,"Renamed"),0);EXPECT_STREQ(swmm_subcatch_get_snowpack(e,0),"Renamed");
    EXPECT_EQ(swmm_subcatch_set_snowpack(e,0,nullptr),0);EXPECT_STREQ(swmm_subcatch_get_snowpack(e,0),"");
    EXPECT_EQ(swmm_subcatch_get_snowpack(e,1),nullptr);swmm_engine_destroy(e);
}

TEST(SurfaceProcesses, ClearingParsedSnowpackSurvivesSaveAndReopenAndRuntimeEditIsRejected) {
    const auto root=std::filesystem::path(OPENSWMM_TEST_SOURCE_DIR).parent_path().parent_path();
    const auto deck=root/"parity/snow/snow_parity.inp";
    const auto output=root/"verification/surface_r1_2026-10-05/snowpack_assignment";
    std::filesystem::create_directories(output);
    SWMM_Engine e=swmm_engine_create(); ASSERT_TRUE(e);
    ASSERT_EQ(swmm_engine_open(e,deck.string().c_str(),(output/"source.rpt").string().c_str(),(output/"source.out").string().c_str(),nullptr),0);
    ASSERT_GT(swmm_snowpack_count(e),0);
    const std::string name=swmm_snowpack_id(e,0);
    ASSERT_EQ(swmm_subcatch_set_snowpack(e,0,name.c_str()),0);
    ASSERT_EQ(swmm_model_write(e,(output/"assigned.inp").string().c_str()),0);
    ASSERT_EQ(swmm_subcatch_set_snowpack(e,0,""),0);
    ASSERT_EQ(swmm_model_write(e,(output/"cleared.inp").string().c_str()),0);
    swmm_engine_destroy(e);
    for (bool assigned : {false,true}) {
        e=swmm_engine_create(); ASSERT_TRUE(e);
        ASSERT_EQ(swmm_engine_open(e,(output/(assigned?"assigned.inp":"cleared.inp")).string().c_str(),(output/"reopen.rpt").string().c_str(),(output/"reopen.out").string().c_str(),nullptr),0);
        EXPECT_STREQ(swmm_subcatch_get_snowpack(e,0),assigned?name.c_str():"");
        ASSERT_EQ(swmm_engine_initialize(e),0);
        EXPECT_NE(swmm_subcatch_set_snowpack(e,0,name.c_str()),0);
        swmm_engine_destroy(e);
    }
}

TEST(SurfaceProcesses, MeshMonthlyFactorsUseEvaluationClockAcrossYearAndMonthBoundary) {
    SimulationContext ctx;
    ctx.options.start_date = datetime::encodeDate(2025,12,31) + datetime::encodeTime(23,59,0);
    ctx.adjust_hydcon[11]=.3;ctx.adjust_hydcon[0]=.7;
    ctx.patterns.factors.push_back(std::vector<double>(12,1));
    ctx.patterns.factors[0][11]=.2;ctx.patterns.factors[0][0]=1.4;
    ctx.climate_state.recovery_pat_index=0;
    // The runoff cache deliberately describes the later interval.
    ctx.climate_state.infil_factor=9;ctx.climate_state.recovery_factor=9;
    auto old=surface::infiltrationFactors(ctx,0);
    auto next=surface::infiltrationFactors(ctx,60);
    EXPECT_DOUBLE_EQ(old.infiltration,.3);EXPECT_DOUBLE_EQ(old.recovery,.2);
    EXPECT_DOUBLE_EQ(next.infiltration,.7);EXPECT_DOUBLE_EQ(next.recovery,1.4);
    ctx.climate_state.recovery_pat_index=-1;
    EXPECT_DOUBLE_EQ(surface::infiltrationFactors(ctx,60).recovery,1);
}

TEST(SurfaceProcesses, ConstantConductivityFactorScalesCapacityOnceInBothUnitSystems) {
    for (bool si : {false,true}) {
        SimulationOptions opts;opts.flow_units=si?FlowUnits::CMS:FlowUnits::CFS;
        auto p=parameters(InfilModel::CONSTANT,si);
        twoD::MeshData mesh;mesh.resize_triangles(1);twoD::SurfaceStateData state;state.resize(1,0);state.depth[0]=1;
        twoD::Infil2D infil;twoD::Infil2DDefault d;d.tag="*";d.row.has_method=true;d.row.method=InfilModel::CONSTANT;
        std::copy(p.begin(),p.end(),d.row.p);infil.defaults().push_back(d);
        std::string error;ASSERT_TRUE(infil.resolve(mesh,opts,error));
        const double base=.5*.0254/3600;
        for (double factor : {.4,.4,1.0,1.0}) {
            infil.updateRates(mesh,state,60,{factor,7});
            EXPECT_NEAR(state.infil_rate[0],base*factor,1e-15);
        }
    }
}

TEST(SurfaceProcesses, SoilRecoveryFactorChangesDryStateAndNextStormCapacityForAllFiveMethods) {
    SimulationOptions opts;
    for (int m=0;m<5;++m) {
        SCOPED_TRACE(m);const auto method=static_cast<InfilModel>(m);auto p=parameters(method,false);
        surface::InfilBank fast,slow;fast.init(1);slow.init(1);fast.setMethod(0,method,p.data(),opts);slow.setMethod(0,method,p.data(),opts);
        for (int k=0;k<10;++k) {
            fast.rate(0,1e-4,0,.01,60,{1,1});slow.rate(0,1e-4,0,.01,60,{1,1});
        }
        // Drain the last available surface film before the dry interval.
        fast.rate(0,0,0,1e-7,60,{1,1});slow.rate(0,0,0,1e-7,60,{1,1});
        for (int k=0;k<60;++k) {fast.rate(0,0,0,0,60,{1,1});slow.rate(0,0,0,0,60,{1,.2});}
        int model;double a[6],b[6];fast.pack(0,model,a);slow.pack(0,model,b);
        EXPECT_NE(std::memcmp(a,b,sizeof a),0);
        const double f=fast.rate(0,1e-4,0,.01,60,{1,1});const double s=slow.rate(0,1e-4,0,.01,60,{1,.2});
        EXPECT_GT(f,s);
    }
}
