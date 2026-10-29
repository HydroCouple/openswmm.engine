#include <gtest/gtest.h>
#include "core/SimulationContext.hpp"
#include "core/HotStartManager.hpp"
#include "quality/QualityRouting.hpp"
#include "transport/components/ReactionModule/MsxLegacyTransport.hpp"
#include <filesystem>
#include <fstream>
using namespace openswmm;
using quality::FvQualityTransport;
namespace {
struct Held {using type=std::vector<std::vector<double>> FvQualityTransport::*;friend type member(Held);};
template<class Tag,typename Tag::type M>struct Access{friend typename Tag::type member(Tag){return M;}};
template struct Access<Held,&FvQualityTransport::held_>;
SimulationContext context(bool reverse=false,bool families=false) {
 SimulationContext ctx;for(auto id:reverse?std::vector<std::string>{"N1","N0"}:std::vector<std::string>{"N0","N1"})ctx.node_names.add(id);
 ctx.link_names.add("L0");for(auto id:reverse?std::vector<std::string>{"P1","P0"}:std::vector<std::string>{"P0","P1"})ctx.pollutant_names.add(id);
 ctx.allocate_objects();ctx.options.routing_model=RoutingModel::FV;ctx.mass_balance.resize_quality(2);
 if(families){ctx.reactions.species_name={"S0","S1"};ctx.reactions.species_is_wall={0,0};ctx.reactions.species_units={"mg/L","mol/L"};ctx.options.water_age=true;ctx.options.heat_transport=true;}
 return ctx;
}
void bind(SimulationContext&ctx,FvQualityTransport& fv) {
 ctx.state_accessors.has_fv_retained_dry_mass=[&](){return fv.hasRetainedDryMass();};
 ctx.state_accessors.get_fv_dry_state=[&](){return fv.dryRestartState(ctx);};
 ctx.state_accessors.set_fv_dry_state=[&](const std::string&b,bool a){return fv.restoreDryRestart(ctx,b,a);};
 ctx.state_accessors.get_fv_retained_species_mass=[&](){return fv.retainedSpeciesMass(ctx);};
}
std::string path(const char* name) {
#ifdef OPENSWMM_FV_DRY_RESTART_TEST_OUT_DIR
 const std::filesystem::path p=OPENSWMM_FV_DRY_RESTART_TEST_OUT_DIR;
#else
 const auto p=std::filesystem::current_path()/"dry-restart-files";
#endif
 std::filesystem::create_directories(p);return (p/name).string();
}
TEST(FvDryRestart, RemapsAllScalarFamiliesAndElementsByName) {
 auto ctx=context(false,true);FvQualityTransport fv;fv.*member(Held{})={{2,3,5},{7,11,13},{17,19,23},{29,31,37},{41,43,47},{-53,59,-61}};
 const auto blob=fv.dryRestartState(ctx);std::ofstream(path("all-families.txt"))<<blob;
 auto target=context(true,true);FvQualityTransport restored;
 ASSERT_TRUE(restored.restoreDryRestart(target,blob,false).empty());EXPECT_FALSE(restored.hasRetainedDryMass());
 ASSERT_TRUE(restored.restoreDryRestart(target,blob,true).empty());
 const auto h=restored.*member(Held{});ASSERT_EQ(h.size(),6u);EXPECT_EQ(h[0],(std::vector<double>{11,7,13}));EXPECT_EQ(h[1],(std::vector<double>{3,2,5}));EXPECT_EQ(h[2],(std::vector<double>{19,17,23}));EXPECT_EQ(h[5],(std::vector<double>{59,-53,-61}));
 EXPECT_EQ(restored.retainedPollutantMass(target),(std::vector<double>{31,10}));EXPECT_EQ(restored.retainedSpeciesMass(target),(std::vector<double>{59,97}));
}
TEST(FvDryRestart, InvalidDataNeverMutatesInventories) {
 auto ctx=context();FvQualityTransport fv;fv.*member(Held{})={{2,0,0},{0,0,0}};const auto before=fv.dryRestartState(ctx);
 for(const auto& invalid:std::vector<std::string>{"FV_DRY_INVENTORY 2 0", "FV_DRY_INVENTORY 1 1\n\"P\" \"missing\" \"0\" \"N\" \"N0\" 1", "FV_DRY_INVENTORY 1 1\n\"P\" \"P0\" \"0\" \"N\" \"N0\" -1",before+"extra",before.substr(0,before.size()/2)}) {
  EXPECT_FALSE(fv.restoreDryRestart(ctx,invalid,true).empty());EXPECT_EQ(fv.dryRestartState(ctx),before);
 }
 ctx.pollutants.units[0]=MassUnits::UG_PER_L;EXPECT_FALSE(fv.restoreDryRestart(ctx,before,true).empty());ctx.pollutants.units[0]=MassUnits::MG_PER_L;
 ctx.options.routing_model=RoutingModel::DYNWAVE;EXPECT_FALSE(fv.restoreDryRestart(ctx,before,true).empty());
}
TEST(FvDryRestart, BothSaveOverloadsRoundTripOptionalV18AndLegacyFilesClearDryState) {
 for(bool expanded:{false,true}) {
  auto ctx=context();FvQualityTransport fv;fv.*member(Held{})={{2,0,0},{0,0,0}};bind(ctx,fv);
  const auto file=path(expanded?"expanded.hsf":"basic.hsf");
  auto* hs=expanded?HotStartManager::save(ctx,nullptr,nullptr,file):HotStartManager::save(ctx,file);ASSERT_NE(hs,nullptr)<<HotStartManager::last_io_error();EXPECT_EQ(hs->header.version,18u);delete hs;
  hs=HotStartManager::open(file);ASSERT_NE(hs,nullptr)<<HotStartManager::last_io_error();EXPECT_FALSE(hs->fv_dry_state.empty());
  auto target=context(true);FvQualityTransport restored;bind(target,restored);ASSERT_EQ(HotStartManager::apply(*hs,target),0)<<HotStartManager::last_io_error();EXPECT_EQ(restored.retainedPollutantMass(target),(std::vector<double>{0,2}));delete hs;
  HotStartFile empty;EXPECT_EQ(HotStartManager::apply(empty,target),0);EXPECT_FALSE(restored.hasRetainedDryMass());
 }
}
TEST(FvDryRestart, InvalidExtensionIsRejectedBeforeHydraulicMutation) {
 auto ctx=context();FvQualityTransport fv;bind(ctx,fv);ctx.nodes.depth[0]=4;
 HotStartFile hs;HotStartNodeRecord rec;rec.id="N0";rec.depth=100;hs.nodes.push_back(rec);hs.fv_dry_state="broken";
 EXPECT_EQ(HotStartManager::apply(hs,ctx),-1);EXPECT_DOUBLE_EQ(ctx.nodes.depth[0],4);
}
TEST(FvDryRestart, RestoredPollutantInventoryRemobilizesWithoutNewInput) {
 auto ctx=context();FvQualityTransport source;source.*member(Held{})={{2,0,0},{0,0,0}};const auto blob=source.dryRestartState(ctx);
 quality::QualitySolver solver;solver.init(2,1,2);ASSERT_TRUE(solver.restoreFvDryRestart(ctx,blob,true).empty());
 // Reopening the engine budget adds exactly the restored held mass to both
 // opening and current dry storage. Exercise the following accepted step.
 ctx.mass_balance.qual_routing_init[0]=2;ctx.mass_balance.qual_routing_final_dry[0]=2;
 ctx.nodes.ext_inflow[0]=ctx.nodes.lat_flow[0]=1;ctx.nodes.volume[0]=1;
 auto&graph=solver.fvWater();auto&w=graph.water;w.old_volume={0,0,0};w.new_volume={1,0,0};w.source_volume={0,0,0};w.exported=w.seeped=w.evaporated=std::vector<double>(3,0);graph.flooding={0,0,0};graph.ready=true;
 solver.execute(ctx,1);ASSERT_TRUE(solver.fvTransportAccepted());EXPECT_NEAR(ctx.nodes.conc[0],2,1e-12);EXPECT_FALSE(solver.hasFvRetainedDryMass());EXPECT_NEAR(ctx.mass_balance.qual_routing_final_dry[0],0,1e-12);
 EXPECT_NEAR(ctx.nodes.conc[0]*ctx.nodes.volume[0],ctx.mass_balance.qual_routing_init[0],1e-12);
}
TEST(FvDryRestart, SpeciesOpeningLedgerIncludesRestoredHeldMassExactlyOnce) {
 auto ctx=context(false,true);ctx.reactions.configured=true;ctx.reactions.init_global={0,0};FvQualityTransport fv;fv.*member(Held{})=std::vector<std::vector<double>>(6,std::vector<double>(3,0));(fv.*member(Held{}))[2][0]=9;bind(ctx,fv);
 transport::armMsxRoutingLedger(ctx);EXPECT_DOUBLE_EQ(ctx.reactions.msx_ledger.init[0],9);EXPECT_DOUBLE_EQ(ctx.reactions.msx_ledger.final_dry[0],9);
 transport::armMsxRoutingLedger(ctx);EXPECT_DOUBLE_EQ(ctx.reactions.msx_ledger.init[0],9);EXPECT_DOUBLE_EQ(ctx.reactions.msx_ledger.final_dry[0],9);
}
}
#include "core/SWMMEngine.hpp"
namespace {
TEST(FvDryRestart, EngineUseHotstartReopensDryOpeningInventory) {
 auto source=context();FvQualityTransport fv;fv.*member(Held{})={{2,0,0},{0,0,0}};bind(source,fv);
 const auto hsf=path("engine-dry.hsf");auto* saved=HotStartManager::save(source,hsf);ASSERT_NE(saved,nullptr);delete saved;
 const auto inp=path("engine-dry.inp");std::ofstream model(inp);
 model<<"[OPTIONS]\nFLOW_UNITS CFS\nFLOW_ROUTING FV\nSTART_DATE 01/01/2026\nEND_DATE 01/01/2026\nEND_TIME 00:01:00\nROUTING_STEP 1\nREPORT_STEP 00:00:01\nFV_MIN_CELLS 4\nFV_CFL .1875\nFV_LTS NO\n"
      <<"[POLLUTANTS]\nP0 MG/L 0 0 0 0\nP1 MG/L 0 0 0 0\n[JUNCTIONS]\nN0 1 10 0 0 0\n[OUTFALLS]\nN1 0 FREE NO\n[CONDUITS]\nL0 N0 N1 100 .013 0 0 0\n[XSECTIONS]\nL0 CIRCULAR 3 0 0 0 1\n[FILES]\nUSE HOTSTART \""<<hsf<<"\"\n";model.close();
 SWMMEngine engine;ASSERT_EQ(engine.open(inp.c_str(),path("engine-dry.rpt").c_str(),path("engine-dry.out").c_str(),nullptr),0);
 ASSERT_EQ(engine.initialize(),0);const auto&ctx=engine.context();
 EXPECT_DOUBLE_EQ(ctx.mass_balance.qual_routing_init[0],2);EXPECT_DOUBLE_EQ(ctx.mass_balance.qual_routing_final_dry[0],2);
 ASSERT_TRUE(ctx.state_accessors.has_fv_retained_dry_mass);EXPECT_TRUE(ctx.state_accessors.has_fv_retained_dry_mass());
 ASSERT_EQ(engine.start(0),0);double elapsed=0;ASSERT_EQ(engine.step(&elapsed),0)<<engine.last_error_message();EXPECT_DOUBLE_EQ(engine.context().mass_balance.qual_routing_final_dry[0],2);
 engine.end();engine.close();
}
}
namespace {
TEST(FvDryRestart, ClassicRestartUsesConduitVolumeRatherThanCappedDepth) {
 auto source=context();source.links.volume[0]=900;source.links.depth[0]=3;
 FvQualityTransport fv;fv.*member(Held{})={{2,0,0},{0,0,0}};bind(source,fv);
 const auto hsf=path("pressurized-volume.hsf");auto* saved=HotStartManager::save(source,hsf);ASSERT_NE(saved,nullptr);delete saved;
 const auto inp=path("pressurized-volume.inp");std::ofstream model(inp);
 model<<"[OPTIONS]\nFLOW_UNITS CFS\nFLOW_ROUTING FV\nSTART_DATE 01/01/2026\nEND_DATE 01/01/2026\nEND_TIME 00:01:00\nROUTING_STEP 1\nREPORT_STEP 00:00:01\nFV_MIN_CELLS 4\nFV_CFL .1875\nFV_LTS NO\n[POLLUTANTS]\nP0 MG/L 0 0 0 0\nP1 MG/L 0 0 0 0\n[JUNCTIONS]\nN0 0 10 0 0 0\n[OUTFALLS]\nN1 0 FREE NO\n[CONDUITS]\nL0 N0 N1 100 .013 0 0 0\n[XSECTIONS]\nL0 CIRCULAR 3 0 0 0 1\n[FILES]\nUSE HOTSTART \""<<hsf<<"\"\n";model.close();
 SWMMEngine engine;ASSERT_EQ(engine.open(inp.c_str(),path("pressurized-volume.rpt").c_str(),path("pressurized-volume.out").c_str(),nullptr),0);ASSERT_EQ(engine.initialize(),0)<<engine.last_error_message();
 EXPECT_DOUBLE_EQ(engine.context().mass_balance.routing_init_storage,900);
 ASSERT_EQ(engine.start(0),0);double elapsed=0;ASSERT_EQ(engine.step(&elapsed),0)<<engine.last_error_message();
 const auto&ctx=engine.context();EXPECT_GE(ctx.links.volume[0],0);EXPECT_TRUE(std::isfinite(ctx.links.volume[0]));
 // The accepted-water transport validates every control-volume equation,
 // including this initially pressurized conduit, before publishing quality.
 engine.end();engine.close();
}
}
