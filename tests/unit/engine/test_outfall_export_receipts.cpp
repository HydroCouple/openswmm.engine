// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_massbalance.h>
#include <openswmm/engine/openswmm_model.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include "core/SWMMEngine.hpp"
namespace {
constexpr double m3perFt3=.028316846592;
const char *deck=R"([OPTIONS]
FLOW_UNITS CFS
FLOW_ROUTING KINWAVE
START_DATE 01/01/2026
START_TIME 00:00
END_DATE 01/01/2026
END_TIME 00:10
REPORT_START_DATE 01/01/2026
REPORT_START_TIME 00:05
ROUTING_STEP 10
REPORT_STEP 00:01
HEAT_TRANSPORT ON
[JUNCTIONS]
J 10 10 0 0 0
[OUTFALLS]
O 9 FREE NO
[CONDUITS]
C J O 10 .013 0 0
[XSECTIONS]
C CIRCULAR 2 0 0 0 1
[DWF]
J FLOW 1
J A 100
J B 100000
[POLLUTANTS]
A MG/L 0 0 0 0 NO * 0 100 100
B UG/L 0 0 0 0 NO * 0 100000 100000
[HEAT_SOURCES]
DWF GLOBAL 20
INITIAL_STATE GLOBAL 20
[HEAT_FLUXES]
SURFACE_EXCHANGE OFF
RADIATIVE_EXCHANGE OFF
[REPORT]
CONTINUITY YES
)";
struct Engine {
    SWMM_Engine value=swmm_engine_create();
    ~Engine(){swmm_engine_destroy(value);}
};
void start(Engine &e,std::string name) {
    const auto dir=std::filesystem::path("outfall_export_receipts");std::filesystem::create_directories(dir);
    const auto inp=dir/(name+".inp");std::ofstream(inp)<<deck;
    ASSERT_EQ(swmm_engine_open(e.value,inp.string().c_str(),(dir/(name+".rpt")).string().c_str(),(dir/(name+".out")).string().c_str(),nullptr),0);
    ASSERT_EQ(swmm_engine_initialize(e.value),0);ASSERT_EQ(swmm_engine_start(e.value,1),0);
}
}
TEST(OutfallExportReceipts, NonuniformExactIntervalsMatchNativeContinuityAndKnownConcentrations) {
    Engine e;start(e,"exact");const int n=swmm_node_index(e.value,"O");
    SWMM_OutfallExportReceipt previous{},current{};double masses[2]={},previousMasses[2]={};
    ASSERT_EQ(swmm_node_get_export_receipt(e.value,n,&previous,masses,2),0);
    EXPECT_DOUBLE_EQ(previous.exported_m3,0);EXPECT_EQ(previous.flags,7u);
    for(double target:{7.,15.,31.,60.,120.}) {
        double actual=0;ASSERT_EQ(swmm_engine_advance_to(e.value,target,&actual),0);EXPECT_DOUBLE_EQ(actual,target);
        ASSERT_EQ(swmm_node_get_export_receipt(e.value,n,&current,masses,2),0);
        double nativeFt3=0;ASSERT_EQ(swmm_get_routing_total(e.value,SWMM_ROUTING_OUTFLOW,&nativeFt3),0);
        EXPECT_NEAR(current.exported_m3,nativeFt3*m3perFt3,1e-12);
        EXPECT_GT(current.exported_m3,previous.exported_m3);
        EXPECT_DOUBLE_EQ(current.reversed_m3,0);
        const auto &mb=static_cast<openswmm::SWMMEngine *>(e.value)->context().mass_balance;
        EXPECT_NEAR(masses[0],mb.qual_routing_outflow[0]*m3perFt3*.001,1e-12);
        EXPECT_NEAR(masses[1],mb.qual_routing_outflow[1]*m3perFt3*.000001,1e-12);
        EXPECT_DOUBLE_EQ(masses[0],masses[1]);
        // Startup outfall water initially has zero concentration. Once the
        // known constant inflow reaches it, its exported mass is C * dV.
        if (target==120.) for(int p=0;p<2;++p)
            EXPECT_NEAR(masses[p]-previousMasses[p],.1*(current.exported_m3-previous.exported_m3),1e-10);
        std::copy(masses,masses+2,previousMasses);
        EXPECT_NEAR(current.exported_heat_j,20*1000*4184*current.exported_m3,1e-3);
        SWMM_OutfallExportReceipt reread{};double again[2]={};
        ASSERT_EQ(swmm_node_get_export_receipt(e.value,n,&reread,again,2),0);
        EXPECT_DOUBLE_EQ(reread.exported_m3,current.exported_m3);EXPECT_DOUBLE_EQ(again[0],masses[0]);
        previous=current;
    }
}
TEST(OutfallExportReceipts, RejectsInvalidBuffersAndRetainedOrInternallyCoupledOutfalls) {
    Engine e;start(e,"ownership");const int n=swmm_node_index(e.value,"O");SWMM_OutfallExportReceipt receipt{};double masses[2];
    EXPECT_NE(swmm_node_get_export_receipt(nullptr,n,&receipt,masses,2),0);
    EXPECT_NE(swmm_node_get_export_receipt(e.value,swmm_node_index(e.value,"J"),&receipt,masses,2),0);
    EXPECT_NE(swmm_node_get_export_receipt(e.value,n,nullptr,masses,2),0);
    EXPECT_NE(swmm_node_get_export_receipt(e.value,n,&receipt,masses,1),0);
    EXPECT_NE(swmm_node_get_export_receipt(e.value,n,&receipt,nullptr,2),0);
    auto &ctx=static_cast<openswmm::SWMMEngine *>(e.value)->context();
    const int row=ctx.node_subtypes.outfall_row(n);
    ctx.node_subtypes.outfalls.route_to[row]=0;
    EXPECT_NE(swmm_node_get_export_receipt(e.value,n,&receipt,masses,2),0);
    ctx.node_subtypes.outfalls.route_to[row]=-1;
    ctx.surface_outfall_link_limit.assign(ctx.n_nodes(),std::numeric_limits<double>::infinity());
    ctx.surface_outfall_link_limit[n]=1;
    EXPECT_NE(swmm_node_get_export_receipt(e.value,n,&receipt,masses,2),0);
}
TEST(OutfallExportReceipts, SeparateEnginesKeepIndependentLedgers) {
    Engine a,b;start(a,"a");start(b,"b");double actual;ASSERT_EQ(swmm_engine_advance_to(a.value,30,&actual),0);
    SWMM_OutfallExportReceipt ar{},br{};double am[2],bm[2];
    ASSERT_EQ(swmm_node_get_export_receipt(a.value,swmm_node_index(a.value,"O"),&ar,am,2),0);
    ASSERT_EQ(swmm_node_get_export_receipt(b.value,swmm_node_index(b.value,"O"),&br,bm,2),0);
    EXPECT_GT(ar.exported_m3,0);EXPECT_DOUBLE_EQ(br.exported_m3,0);
    ASSERT_EQ(swmm_engine_advance_to(b.value,30,&actual),0);
    ASSERT_EQ(swmm_node_get_export_receipt(b.value,swmm_node_index(b.value,"O"),&br,bm,2),0);
    EXPECT_DOUBLE_EQ(ar.exported_m3,br.exported_m3);EXPECT_DOUBLE_EQ(am[0],bm[0]);
}
