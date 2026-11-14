#include <gtest/gtest.h>
#include "quality/QualityRouting.hpp"
#include "core/SimulationContext.hpp"
#include <fstream>
#include <filesystem>
using openswmm::quality::QualitySolver;
namespace {
openswmm::SimulationContext makeContext(int np) {
    openswmm::SimulationContext ctx;
    for(int i=0;i<3;++i)ctx.node_names.add("N"+std::to_string(i));
    for(int i=0;i<2;++i)ctx.link_names.add("L"+std::to_string(i));
    for(int p=0;p<np;++p)ctx.pollutant_names.add("P"+std::to_string(p));
    ctx.allocate_objects();
    for(int i=0;i<3;++i)ctx.nodes.depth[i]=1;
    for(int i=0;i<2;++i) {ctx.links.node1[i]=i;ctx.links.node2[i]=i+1;ctx.links.depth[i]=1;}
    return ctx;
}
}
#include "core/HotStartManager.hpp"
using openswmm::HotStartManager;
namespace {
void emptyWater(QualitySolver& solver, int n=5) {
    auto& graph=solver.fvWater();graph={};
    auto& w=graph.water;
    w.old_volume.assign(n,0);w.new_volume.assign(n,0);w.source_volume.assign(n,0);
    w.exported.assign(n,0);w.seeped.assign(n,0);w.evaporated.assign(n,0);
    graph.flooding.assign(n,0);graph.ready=true;
}
TEST(FvConnectedQuality, AcceptedTransfersCloseTheIndependent120UnitBudget) {
    auto ctx=makeContext(1);ctx.mass_balance.resize_quality(1);ctx.options.routing_model=openswmm::RoutingModel::FV;
    ctx.links.type[0]=openswmm::LinkType::CONDUIT;
    ctx.links.volume[0]=ctx.links.old_volume[0]=10;
    ctx.links.conc[0]=ctx.links.conc_old[0]=10;
    ctx.nodes.ext_inflow[0]=1;ctx.nodes.ext_qual_mass[0]=20;ctx.nodes.lat_flow[0]=1;
    ctx.nodes.dwf_inflow[1]=-1;ctx.nodes.lat_flow[1]=-1;
    QualitySolver solver;solver.init(3,2,1);emptyWater(solver);
    auto& w=solver.fvWater().water;w.old_volume[3]=w.new_volume[3]=10;
    w.transfers={{0,3,1},{3,1,1}};w.exported[1]=1;
    solver.execute(ctx,1);
    ASSERT_TRUE(solver.fvTransportAccepted());
    EXPECT_NEAR(ctx.links.conc[0],120./11,1e-12);
    EXPECT_NEAR(10*ctx.links.conc[0]+ctx.mass_balance.qual_routing_outflow[0],120,1e-10);
    EXPECT_NEAR(ctx.nodes.conc[1],ctx.links.conc[0],1e-12);
}
TEST(FvConnectedQuality, GrossCarrierAndExtractionPreserveUniformConcentration) {
    auto ctx=makeContext(1);ctx.mass_balance.resize_quality(1);ctx.options.routing_model=openswmm::RoutingModel::FV;
    ctx.nodes.ext_inflow[0]=2;ctx.nodes.ext_qual_mass[0]=200;
    ctx.nodes.dwf_inflow[0]=-1;ctx.nodes.lat_flow[0]=1;
    QualitySolver solver;solver.init(3,2,1);emptyWater(solver);
    auto& w=solver.fvWater().water;w.transfers={{0,1,10}};w.exported[0]=10;w.exported[1]=10;
    solver.execute(ctx,10);
    ASSERT_TRUE(solver.fvTransportAccepted());
    EXPECT_DOUBLE_EQ(ctx.nodes.qual_vol_in[0],20);
    EXPECT_NEAR(ctx.nodes.conc[0],100,1e-12);EXPECT_NEAR(ctx.nodes.conc[1],100,1e-12);
    EXPECT_NEAR(ctx.mass_balance.qual_routing_outflow[0],2000,1e-10);
}
TEST(FvConnectedQuality, DryMassRemobilizesAndCannotBeSilentlyLostInAHotstart) {
    auto ctx=makeContext(1);ctx.mass_balance.resize_quality(1);ctx.options.routing_model=openswmm::RoutingModel::FV;
    ctx.nodes.ext_qual_mass[0]=2;
    QualitySolver solver;solver.init(3,2,1);emptyWater(solver);solver.execute(ctx,1);
    ASSERT_TRUE(solver.fvTransportAccepted());ASSERT_TRUE(solver.hasFvRetainedDryMass());
    EXPECT_NEAR(ctx.mass_balance.qual_routing_final_dry[0],2,1e-12);
    ctx.state_accessors.has_fv_retained_dry_mass=[&](){return solver.hasFvRetainedDryMass();};
    std::filesystem::create_directories(OPENSWMM_FV_CONSERVATIVE_TEST_OUT_DIR);
    const std::string path=std::string(OPENSWMM_FV_CONSERVATIVE_TEST_OUT_DIR)+"/retained-fv.hsf";{std::ofstream out(path);out<<"sentinel";}
    EXPECT_EQ(HotStartManager::save(ctx,path),nullptr);
    EXPECT_NE(HotStartManager::last_io_error().find("retained FV dry"),std::string::npos);
    {std::ifstream in(path);std::string content;in>>content;EXPECT_EQ(content,"sentinel");}
    ctx.nodes.ext_qual_mass[0]=0;ctx.nodes.ext_inflow[0]=ctx.nodes.lat_flow[0]=1;
    emptyWater(solver);solver.fvWater().water.new_volume[0]=1;
    ctx.nodes.volume[0]=1;solver.execute(ctx,1);
    ASSERT_TRUE(solver.fvTransportAccepted());EXPECT_FALSE(solver.hasFvRetainedDryMass());
    EXPECT_NEAR(ctx.nodes.conc[0],2,1e-12);EXPECT_NEAR(ctx.mass_balance.qual_routing_final_dry[0],0,1e-12);
    auto* hs=HotStartManager::save(ctx,path);ASSERT_NE(hs,nullptr);delete hs;
}
TEST(FvConnectedQuality, LegacyDwGrossCarrierControlRemainsUnchanged) {
    auto ctx=makeContext(1);ctx.mass_balance.resize_quality(1);ctx.options.routing_model=openswmm::RoutingModel::DYNWAVE;
    ctx.nodes.ext_inflow[0]=2;ctx.nodes.ext_qual_mass[0]=200;
    ctx.nodes.dwf_inflow[0]=-1;ctx.nodes.lat_flow[0]=1;
    QualitySolver solver;solver.init(3,2,1);solver.execute(ctx,10);
    EXPECT_DOUBLE_EQ(ctx.nodes.qual_vol_in[0],10);EXPECT_DOUBLE_EQ(ctx.nodes.conc[0],200);
}
}
