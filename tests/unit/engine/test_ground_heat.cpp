// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <cmath>
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_heat.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_gw_transport.h>
#include <openswmm/engine/openswmm_coupling.h>
#include <openswmm/engine/openswmm_forcing.h>
#include <openswmm/engine/openswmm_gw2d.h>
#include <openswmm/engine/openswmm_hotstart.h>
#include "core/CompletedSourceArchive.hpp"
#include "core/SWMMEngine.hpp"
#include "2d/SurfaceRouter2D.hpp"
#include "2d/subsurface/SubsurfaceSolver.hpp"
#include "2d/heat/ChannelGround.hpp"
#include "2d/heat/MeshHeat.hpp"
#include "2d/heat/LidNodeHeat.hpp"
#include "2d/heat/ThermalCheckpoint.hpp"
namespace {
namespace fs=std::filesystem;
const fs::path out=fs::path(OPENSWMM_GROUND_HEAT_OUT)/"ground_heat";
struct Model {
    SWMM_Engine e=nullptr;bool running=false;
    Model(const std::string& name,const std::string& closure="CLOSED_FORM",double hg=.5,bool partial=false,const std::string& extra="",bool mapped_channel=false) {
        fs::create_directories(out);const auto file=out/("_"+name+".inp");
        std::ofstream(file)<<"[OPTIONS]\nFLOW_UNITS CMS\nFLOW_ROUTING DYNWAVE\nHEAT_TRANSPORT YES\n"
            "START_DATE 01/01/2026\nEND_DATE 01/01/2026\nEND_TIME 00:10:00\nROUTING_STEP 5\nREPORT_STEP 00:01:00\n"
            "[JUNCTIONS]\nJ1 -2 4 0 0 0\n[OUTFALLS]\nO1 -2.5 FREE NO\n"
            "[CONDUITS]\nC1 J1 O1 30 .013 0 0 0\n[XSECTIONS]\nC1 RECT_OPEN 1 2 0 0 1\n"
            "[COORDINATES]\nJ1 40 0\nO1 70 0\n"
            "[2D_OPTIONS]\nREPORT_2D NO\nMAX_TIMESTEP 5\nLTS_TIERS 1\nEVAPORATION NO\nINFILTRATION NO\n"
            "[2D_VERTICES]\n0 0 0\n10 0 0\n10 10 0\n0 10 0\n"
            "[2D_TRIANGLES]\n0 1 2 .03 .1 channel:test\n0 2 3 .03 .1 floodplain\n"
            "[2D_AQUIFER_OPTIONS]\nNODE_ENROLMENT ROWS\nGW_ET NONE\nM_LAYERS 4\nCLOSURE "<<closure<<
            "\n[2D_AQUIFER]\n"<<(partial?"CELL 1":"*")<<" 36 1 .45 .1 2 HG0 "<<hg<<
            "\n[GW_TRANSPORT_OPTIONS]\nTHERMAL_MODEL POROUS_EQUILIBRIUM\nCONDUCTION NO\n"
            "DISPERSION NO\nSOIL_ET_HEAT SENSIBLE_ONLY\nSURFACE_THERMAL_BC ADIABATIC\nDEEP_THERMAL_BC ADIABATIC\n"
            "[GW_INITIAL_QUALITY]\n* SAT __TEMPERATURE__ -3\n* UNSAT __TEMPERATURE__ -3\n"<<extra;
        if(mapped_channel) {
            std::ifstream input(file);std::string text((std::istreambuf_iterator<char>(input)),{});input.close();
            const auto replace=[&](const char* before,const char* after){const auto at=text.find(before);if(at!=std::string::npos)text.replace(at,std::string(before).size(),after);};
            replace("J1 -2 4 0 0 0","J1 -.2 4 0 0 0");replace("O1 -2.5 FREE NO","O1 -.2 FREE NO");
            replace("C1 J1 O1 30 .013 0 0 0","C1 J1 O1 8 .013 0 0 0");replace("J1 40 0\nO1 70 0","J1 1 3\nO1 9 3");
            std::ofstream(file)<<text;
        }
        e=swmm_engine_create();
        if(swmm_engine_open(e,file.string().c_str(),(out/(name+".rpt")).string().c_str(),nullptr,nullptr)!=SWMM_OK) {
            swmm_engine_destroy(e);e=nullptr;
        }
    }
    ~Model(){if(e){if(running)swmm_engine_end(e);swmm_engine_close(e);swmm_engine_destroy(e);}}
    auto& router(){return static_cast<openswmm::SWMMEngine*>(e)->surfaceRouter2D();}
    int start(){const int rc=swmm_engine_initialize(e);if(rc!=SWMM_OK)return rc;
        const int started=swmm_engine_start(e,0);running=started==SWMM_OK;return started;}
};
double sum(const std::vector<double>& x){return std::accumulate(x.begin(),x.end(),0.);}
double energy(const openswmm::twoD::GroundHeatState& h){double e=0;for(const auto& c:h.cells)e+=sum(c.column.energy);return e;}
double poreMass(const openswmm::twoD::GroundHeatState& h,int species) {
    double mass=0;
    for(const auto& cell:h.cells)if(cell.porewater.active()) {
        const auto nl=cell.column.theta.size();
        for(std::size_t j=0;j<nl;++j)mass+=cell.porewater.mass[std::size_t(species)*nl+j];
    }
    return mass;
}
int enablePorewater(Model& m) {
    for(const auto& pair:{std::pair{"HEAT_MODEL","COUPLED"},{"BED_HEAT_MODEL","GROUND_COLUMN"},
                         {"HEAT_SURFACE_EXCHANGE","NO"},{"HEAT_RADIATIVE_EXCHANGE","NO"}}) {
        const int rc=swmm_options_set_ext(m.e,pair.first,pair.second);if(rc!=SWMM_OK)return rc;
    }return m.start();
}
const std::string porewater_species="[POLLUTANTS]\nDye MG/L 0 0 0 0 NO * 0 0\n"
    "[OPTIONS]\nWATER_AGE ON\n[GW_INITIAL_QUALITY]\n* SAT Dye 10\n* UNSAT Dye 10\n";
}
TEST(GroundHeat, MeshRichardsLidSharesReciprocalHeatWithItsGroundColumn) {
    const auto file=out/"mesh-lid-ground-heat-refusal.inp";fs::create_directories(out);
    std::ofstream(file)<<"[OPTIONS]\nFLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\nHEAT_TRANSPORT YES\n"
        "START_DATE 01/01/2026\nEND_DATE 01/01/2026\nEND_TIME 00:10:00\nROUTING_STEP 5\nREPORT_STEP 00:01:00\n"
        "[STORAGE]\nS -2 2 1 FUNCTIONAL 0 0 100 0 0\n"
        "[COORDINATES]\nS 1 3\n[2D_OPTIONS]\nREPORT_2D NO\nMAX_TIMESTEP 5\nLTS_TIERS 1\nEVAPORATION NO\nINFILTRATION NO\n"
        "[2D_VERTICES]\n0 0 0\n10 0 0\n10 10 0\n[2D_TRIANGLES]\n0 1 2 .03 .1 floodplain\n"
        "[2D_AQUIFER_OPTIONS]\nNODE_ENROLMENT ROWS\nGW_ET NONE\nM_LAYERS 4\nCLOSURE CLOSED_FORM\n"
        "[2D_AQUIFER]\n* 36 1 .45 .1 2 HG0 .5\n[2D_AQUIFER_NODE]\nS 1 KC 3600 DC .1 AREA 1\n"
        "[GW_TRANSPORT_OPTIONS]\nTHERMAL_MODEL POROUS_EQUILIBRIUM\nCONDUCTION YES\n"
        "SURFACE_THERMAL_BC ADIABATIC\nDEEP_THERMAL_BC ADIABATIC\n"
        "[LID_CONTROLS]\nStack NODE\nStack SURFACE 6 .1\nStack MEDIA 12 .45 .2 .08 2 10 3\n"
        "Stack AGGREGATE 6 .4 100\nStack BOTTOM 0 0\n[LID_NODES]\nS Stack 10 BOTTOM MESH_AQUIFER\n"
        "[LID_RICHARDS]\nStack OPTIONS 4 1e-7 1e-5 30\n"
        "Stack 2 .03 2 1.6 .5 .0001\nStack 3 .03 2 1.6 .5 .0001\n"
        "[GW_INITIAL_QUALITY]\n* SAT __TEMPERATURE__ -3\n* UNSAT __TEMPERATURE__ -3\n";
    SWMM_Engine engine=swmm_engine_create();ASSERT_NE(engine,nullptr);
    ASSERT_EQ(swmm_engine_open(engine,file.string().c_str(),(out/"mesh-lid-ground-heat.rpt").string().c_str(),nullptr,nullptr),SWMM_OK)
        <<swmm_get_last_error_msg(engine);
    for(const auto& pair:{std::pair{"HEAT_MODEL","COUPLED"},{"BED_HEAT_MODEL","GROUND_COLUMN"},
                         {"HEAT_SURFACE_EXCHANGE","NO"},{"HEAT_RADIATIVE_EXCHANGE","NO"}})
        ASSERT_EQ(swmm_options_set_ext(engine,pair.first,pair.second),SWMM_OK);
    ASSERT_EQ(swmm_heat_set_module(engine,SWMM_HEAT_LAYER_CONDUCTION,1),SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(engine),SWMM_OK)<<swmm_get_last_error_msg(engine);
    ASSERT_EQ(swmm_engine_start(engine,0),SWMM_OK)<<swmm_get_last_error_msg(engine);
    auto& ctx=static_cast<openswmm::SWMMEngine*>(engine)->context();
    auto& router=static_cast<openswmm::SWMMEngine*>(engine)->surfaceRouter2D();
    ASSERT_TRUE(router.subsurface().heat().active());
    auto& lid=ctx.node_subtypes.storages.lid_state[ctx.node_subtypes.storage_row(0)];
    ASSERT_TRUE(lid.richards);ASSERT_TRUE(lid.thermal_energy.empty());
    openswmm::twoD::LidNodeThermalGraph initial_lid;
    ASSERT_TRUE(openswmm::twoD::buildLidNodeThermalGraph(lid,0,
        router.subsurface().heat().water_capacity,ctx.heat_config.conduction,
        ctx.heat_config.global_temp[static_cast<int>(openswmm::HeatSource::INITIAL_STATE)],initial_lid).empty());
    const double ground_initial=energy(router.subsurface().heat());
    double lid_initial=0;for(const auto& store:initial_lid.stores)lid_initial+=store.energy;
    const double cp=router.subsurface().heat().water_capacity;
    const auto surfaceEnergy=[&](){double value=0;for(int i=0;i<router.mesh().n_cells();++i)
        if(std::size_t(i)<router.state().heat.reported_temperature.size()&&std::size_t(i)<router.state().volume.size())
            value+=router.state().volume[std::size_t(i)]*cp*router.state().heat.reported_temperature[std::size_t(i)];
        return value;};
    const auto nodeEnergy=[&](){double value=0;for(int n=0;n<ctx.n_nodes();++n)
        if(std::size_t(n)<ctx.heat_state.node_temp.size())
            value+=ctx.nodes.volume[std::size_t(n)]*.028316846592*cp*ctx.heat_state.node_temp[std::size_t(n)];
        return value;};
    const double surface_initial=surfaceEnergy(),node_initial=nodeEnergy();
    const double total_initial=ground_initial+lid_initial+surface_initial+node_initial;
    double actual=0;
    ASSERT_EQ(swmm_coupling_advance_to(engine,5.,&actual),SWMM_OK)<<swmm_get_last_error_msg(engine);
    EXPECT_DOUBLE_EQ(actual,5.);
    ASSERT_FALSE(lid.thermal_energy.empty());
    const auto& ground=router.subsurface().heat().cells[0];
    ASSERT_EQ(ground.failed,0)<<ground.error;
    EXPECT_GT(ground.lid_contact_j,0);
    const auto& aquifer=router.subsurface();
    ASSERT_EQ(aquifer.richardsBedDownCumulative().size(),1u);
    EXPECT_GT(aquifer.richardsBedDownCumulative()[0]+aquifer.richardsBedUpCumulative()[0],0);
    double reported_lid_contact=0,reported_ground_residual=0;
    ASSERT_EQ(swmm_heat_get_ground_values(engine,SWMM_GROUND_HEAT_LID_CONTACT,0,1,&reported_lid_contact),SWMM_OK);
    ASSERT_EQ(swmm_heat_get_ground_values(engine,SWMM_GROUND_HEAT_RESIDUAL,0,1,&reported_ground_residual),SWMM_OK);
    EXPECT_DOUBLE_EQ(reported_lid_contact,ground.lid_contact_j);
    EXPECT_NEAR(reported_ground_residual,0,1e-5);
    const double ground_final=energy(router.subsurface().heat()),lid_final=sum(lid.thermal_energy);
    const double surface_final=surfaceEnergy(),node_final=nodeEnergy();
    const double total_final=ground_final+lid_final+surface_final+node_final;
    EXPECT_NEAR(total_final,total_initial,2e-5)<<"ground delta="<<(ground_final-ground_initial)
        <<" LID delta="<<(lid_final-lid_initial)<<" advective="<<ground.advective_j
        <<" source="<<ground.source_j<<" surface="<<ground.surface_j<<" deep="<<ground.deep_j
        <<" LID contact="<<ground.lid_contact_j<<" water="<<router.state().volume[0]
        <<" node="<<ctx.nodes.volume[0]<<" surface energy delta="<<(surface_final-surface_initial)
        <<" node energy delta="<<(node_final-node_initial);
    ASSERT_EQ(swmm_engine_end(engine),SWMM_OK);
    swmm_engine_close(engine);swmm_engine_destroy(engine);
}
TEST(GroundHeat, DryGroundColumnsAdvanceOnlyTheUnprocessedRouterInterval) {
    Model split("dry-ground-remainder-split"),reference("dry-ground-remainder-reference");
    ASSERT_NE(split.e,nullptr);ASSERT_NE(reference.e,nullptr);
    for(auto* model:{&split,&reference}) {
        ASSERT_EQ(swmm_gw_transport_option_set(model->e,"CONDUCTION","YES"),SWMM_OK);
        ASSERT_EQ(swmm_gw_transport_option_set(model->e,"SURFACE_THERMAL_BC","FIXED 10"),SWMM_OK);
        ASSERT_EQ(enablePorewater(*model),SWMM_OK);
        ASSERT_TRUE(model->router().state().heat.ground_column);
        ASSERT_TRUE(model->router().subsurface().heat().config->options.conduction);
    }
    auto& split_router=split.router();auto& split_state=split_router.state();
    auto& reference_router=reference.router();auto& reference_state=reference_router.state();
    ASSERT_EQ(split_state.volume.size(),2u);ASSERT_EQ(reference_state.volume.size(),2u);
    const int row=split_state.transport.temp_row;ASSERT_GE(row,0);
    for(int cell=0;cell<2;++cell) {
        split_state.volume[cell]=reference_state.volume[cell]=0;
        split_state.transport.cell_mass[split_state.transport.idx(row,cell)]=0;
        reference_state.transport.cell_mass[reference_state.transport.idx(row,cell)]=0;
    }
    auto& split_heat=split_router.subsurface().heat();auto& reference_heat=reference_router.subsurface().heat();
    auto& split_context=static_cast<openswmm::SWMMEngine*>(split.e)->context();
    auto& reference_context=static_cast<openswmm::SWMMEngine*>(reference.e)->context();
    split_heat.prepare();reference_heat.prepare();
    openswmm::twoD::prepareMeshHeat(split_context,split_state,60);
    openswmm::twoD::prepareMeshHeat(reference_context,reference_state,60);
    const double mass=split_state.transport.cell_mass[split_state.transport.idx(row,0)];
    openswmm::twoD::applyMeshHeatCell(split_router.mesh(),split_router.options(),split_state.heat,0,mass,0,0,20);
    openswmm::twoD::advanceDryGroundColumns(split_router.mesh(),split_router.options(),split_state,60);
    const double reference_mass=reference_state.transport.cell_mass[reference_state.transport.idx(row,0)];
    openswmm::twoD::applyMeshHeatCell(reference_router.mesh(),reference_router.options(),reference_state.heat,0,reference_mass,0,0,20);
    openswmm::twoD::applyMeshHeatCell(reference_router.mesh(),reference_router.options(),reference_state.heat,0,reference_mass,0,0,40);
    const double second_mass=reference_state.transport.cell_mass[reference_state.transport.idx(row,1)];
    openswmm::twoD::applyMeshHeatCell(reference_router.mesh(),reference_router.options(),reference_state.heat,1,second_mass,0,0,60);
    for(int cell=0;cell<2;++cell) {
        EXPECT_EQ(split_heat.cells[cell].column.energy,reference_heat.cells[cell].column.energy);
        EXPECT_DOUBLE_EQ(split_state.heat.ground_column_seconds[cell],60);
        EXPECT_DOUBLE_EQ(reference_state.heat.ground_column_seconds[cell],60);
        EXPECT_EQ(split_heat.cells[cell].failed,0)<<split_heat.cells[cell].error;
        EXPECT_EQ(reference_heat.cells[cell].failed,0)<<reference_heat.cells[cell].error;
    }
    EXPECT_NE(split_heat.cells[0].surface_j,0);
}
TEST(GroundHeat, ExplicitGroundPorewaterUsesAcceptedWellDonorsAndBooksItsAgeClock) {
    for(const auto* closure:{"CLOSED_FORM","ENSLAVED","SIGMA"}) {
        SCOPED_TRACE(closure);Model m(std::string("pore-well-")+closure,closure,.5,false,porewater_species);ASSERT_NE(m.e,nullptr);
        ASSERT_EQ(enablePorewater(m),SWMM_OK);auto& gw=m.router().subsurface();auto& tr=gw.transport();
        ASSERT_TRUE(gw.heat().cells[0].porewater.active());const int dye=tr.rowIndex("Dye"),age=tr.age_row,ns=tr.n_species;
        ASSERT_GE(dye,0);ASSERT_GE(age,0);const double initial=poreMass(gw.heat(),dye);
        EXPECT_NEAR(initial,tr.init_mass[dye],1e-10);std::vector<double> concentrations(ns,0),rates(ns,0);
        ASSERT_EQ(swmm_coupling_set_source(m.e,"well",1,0,-.001,0,concentrations.data(),rates.data(),ns,1),SWMM_OK);
        double actual=0;ASSERT_EQ(swmm_coupling_advance_to(m.e,1,&actual),SWMM_OK);
        for(const auto& cell:gw.heat().cells) {
            ASSERT_EQ(cell.failed,0)<<cell.error;const auto nl=cell.column.theta.size();
            for(std::size_t j=0;j<nl;++j) {
                const double water=cell.column.theta[j]*cell.column.area*(cell.column.depth_edges[j+1]-cell.column.depth_edges[j]);
                if(water>0)EXPECT_NEAR(cell.porewater.mass[std::size_t(dye)*nl+j]/water,10,1e-9);
            }
        }
        EXPECT_NEAR(initial-poreMass(gw.heat(),dye),10*gw.state().led_source_out,1e-9);
        EXPECT_NEAR(tr.residual(dye),0,1e-9);EXPECT_GT(tr.generated_age[age],0);EXPECT_NEAR(tr.residual(age),0,1e-9);
        double generated=0;for(const auto& cell:gw.heat().cells)generated+=sum(cell.porewater.generated_age);
        EXPECT_NEAR(generated,tr.generated_age[age],1e-10);
        EXPECT_NEAR(poreMass(gw.heat(),age)+tr.lost_source[age],generated,1e-9);
        int cell=0;SWMM_CouplingReceipt receipt{};std::vector<double> requested(ns),applied(ns);
        ASSERT_EQ(swmm_coupling_get_receipts(m.e,1,"well",&cell,1,&receipt,requested.data(),applied.data(),ns),SWMM_OK);
        EXPECT_NEAR(-applied[dye],tr.lost_source[dye],1e-12);
    }
}
TEST(GroundHeat, GroundPorewaterNodeReversalPreservesGrossMassAndAgeReceipts) {
    Model m("pore-node-reversal","CLOSED_FORM",.5,false,porewater_species+
        "[2D_AQUIFER_NODE]\nJ1 1 KC 3600 DC .1 AREA 1\n");ASSERT_NE(m.e,nullptr);ASSERT_EQ(enablePorewater(m),SWMM_OK);
    auto& gw=m.router().subsurface();auto& tr=gw.transport();const int dye=tr.rowIndex("Dye"),age=tr.age_row;
    openswmm::NodeData nodes;nodes.resize(2);const double head=gw.state().z_bed[0]+gw.state().hg[0],length=m.router().options().len_1d_to_2d;
    nodes.invert_elev[0]=(head+.25)/length;nodes.volume[0]=100;
    std::vector<double> concentrations(2*tr.n_species,0);concentrations[dye]=20;concentrations[age]=100;concentrations[tr.temp_row]=-3;
    const double initial=poreMass(gw.heat(),dye);gw.setNodeRowConc(concentrations.data(),tr.n_species);gw.assignTiers(1,1);
    gw.sampleNodeExchange(&nodes,.5);const double incoming=-gw.nodeExchangeVolumes()[0];ASSERT_GT(incoming,0);
    nodes.invert_elev[0]=(head-.25)/length;gw.sampleNodeExchange(&nodes,.5);EXPECT_NEAR(gw.nodeExchangeVolumes()[0],0,1e-12);
    gw.fireGwCells(0,1,m.router().state(),0);gw.setNodeRowConc(nullptr,0);
    for(const auto& cell:gw.heat().cells)ASSERT_EQ(cell.failed,0)<<cell.error;
    EXPECT_GT(tr.lost_node[dye],0);EXPECT_NEAR(tr.gained_node[dye],incoming*20,1e-12);
    EXPECT_NEAR(poreMass(gw.heat(),dye)-initial,tr.gained_node[dye]-tr.lost_node[dye],1e-9);
    EXPECT_NEAR(gw.nodeExchangeMass()[std::size_t(dye)*2],tr.lost_node[dye],1e-12);
    EXPECT_NEAR(tr.residual(dye),0,1e-9);EXPECT_NEAR(tr.residual(age),0,1e-9);
}
TEST(GroundHeat, AcceptedLateralPorewaterPacketRemainsReservedAcrossAnInterveningFiniteContact) {
    Model m("reserved-lateral-porewater-source","CLOSED_FORM",.5,false,porewater_species+
        "CELL 1 SAT Dye 20\nCELL 1 UNSAT Dye 20\nCELL 2 SAT Dye 2\nCELL 2 UNSAT Dye 2\n"
        "[2D_AQUIFER]\nCELL 1 36 1 .45 .1 2 HG0 .6\nCELL 2 36 1 .45 .1 2 HG0 .5\n");
    Model restored_model("reserved-lateral-porewater-restored","CLOSED_FORM",.5,false,porewater_species+
        "CELL 1 SAT Dye 20\nCELL 1 UNSAT Dye 20\nCELL 2 SAT Dye 2\nCELL 2 UNSAT Dye 2\n"
        "[2D_AQUIFER]\nCELL 1 36 1 .45 .1 2 HG0 .6\nCELL 2 36 1 .45 .1 2 HG0 .5\n");
    ASSERT_NE(m.e,nullptr);ASSERT_NE(restored_model.e,nullptr);
    ASSERT_EQ(enablePorewater(m),SWMM_OK);ASSERT_EQ(enablePorewater(restored_model),SWMM_OK);
    auto& gw=m.router().subsurface();auto& restored=restored_model.router().subsurface();
    auto& tr=gw.transport();const int dye=tr.rowIndex("Dye");ASSERT_GE(dye,0);
    ASSERT_TRUE(gw.heat().cells[0].porewater.active());ASSERT_TRUE(gw.heat().cells[1].porewater.active());
    const double initial=poreMass(gw.heat(),dye);
    gw.state().dt_cell[0]=.1;gw.state().dt_cell[1]=.1;gw.assignTiers(.1,1);
    gw.fireGwFaces(0,.1);
    auto& donor=gw.heat().cells[0].porewater;auto& receiver=gw.heat().cells[1].porewater;
    const auto nl=gw.heat().cells[0].column.theta.size();double queued=0;std::size_t layer=0;
    for(std::size_t j=0;j<nl;++j){const double v=donor.reserved_out[std::size_t(dye)*nl+j];queued+=v;if(v>0)layer=j;}
    ASSERT_GT(queued,0);
    openswmm::detail::CompletedSourceArchive saved;gw.checkpoint(saved,false);
    openswmm::detail::CompletedSourceArchive input(saved.bytes());restored.checkpoint(input,false);input.finish();
    EXPECT_EQ(restored.heat().cells[0].porewater.reserved_out,donor.reserved_out);
    const double contact=std::min(.1*donor.contactAvailableMass(dye,layer),.1*receiver.contactAvailableMass(dye,layer));
    ASSERT_GT(contact,0);
    auto& restored_donor=restored.heat().cells[0].porewater;auto& restored_receiver=restored.heat().cells[1].porewater;
    ASSERT_TRUE(donor.applyContactMassIncrement(dye,layer,-contact));
    ASSERT_TRUE(receiver.applyContactMassIncrement(dye,layer,contact));
    ASSERT_TRUE(restored_donor.applyContactMassIncrement(dye,layer,-contact));
    ASSERT_TRUE(restored_receiver.applyContactMassIncrement(dye,layer,contact));
    gw.fireGwCells(0,.1,m.router().state(),0);restored.fireGwCells(0,.1,restored_model.router().state(),0);
    for(const auto* solver:{&gw,&restored})for(const auto& cell:solver->heat().cells) {
        ASSERT_EQ(cell.failed,0)<<cell.error;
        for(double v:cell.porewater.reserved_out)EXPECT_DOUBLE_EQ(v,0);
    }
    EXPECT_EQ(gw.heat().cells[0].porewater.mass,restored.heat().cells[0].porewater.mass);
    EXPECT_EQ(gw.heat().cells[1].porewater.mass,restored.heat().cells[1].porewater.mass);
    EXPECT_NEAR(poreMass(gw.heat(),dye),initial,1e-9*std::max(1.,initial));
    EXPECT_NEAR(tr.residual(dye),0,1e-8);
}
TEST(GroundHeat, CommittedLinkHeatCannotBeSpentAgainByAnInterveningFiniteWaterContact) {
    Model m("reserved-link-heat","CLOSED_FORM",.5,false,
        "* SAT __TEMPERATURE__ 40\n* UNSAT __TEMPERATURE__ 40\n"
        "[GW_TRANSPORT_PARAMS]\n* 2650 880 10000000 0 0 0 0 0 .065\n");
    Model restored_model("reserved-link-heat-restored","CLOSED_FORM",.5,false,
        "* SAT __TEMPERATURE__ 40\n* UNSAT __TEMPERATURE__ 40\n"
        "[GW_TRANSPORT_PARAMS]\n* 2650 880 10000000 0 0 0 0 0 .065\n");
    ASSERT_NE(m.e,nullptr);ASSERT_NE(restored_model.e,nullptr);
    ASSERT_EQ(swmm_gw_transport_option_set(m.e,"CONDUCTION","YES"),SWMM_OK);
    ASSERT_EQ(swmm_gw_transport_option_set(restored_model.e,"CONDUCTION","YES"),SWMM_OK);
    ASSERT_EQ(enablePorewater(m),SWMM_OK);ASSERT_EQ(enablePorewater(restored_model),SWMM_OK);
    auto& gw=m.router().subsurface();auto& state=m.router().state();
    auto& restored=restored_model.router().subsurface();auto& restored_state=restored_model.router().state();
    const int row=gw.transport().temp_row;const double cp=gw.heat().water_capacity;
    const double draw=std::min(.1,.5*gw.specificYieldOf(0)*gw.state().hg[0]*gw.state().area[0]);
    ASSERT_GT(draw,0);const double before=energy(gw.heat());
    gw.bookLinkSeepage(0,-draw);const double exported=gw.bookLinkGain(0,row,draw)*cp;
    EXPECT_NEAR(exported,draw*40*cp,1e-8);
    const auto& reserved=gw.heat().cells[0];
    for(std::size_t j=0;j<reserved.column.energy.size();++j) {
        EXPECT_NEAR(gw.heat().activeCapacity(0,j),reserved.column.capacity[j]-cp*reserved.reserved_out_water[j],1e-8);
        EXPECT_GT(gw.heat().activeCapacity(0,j),0);
        if(reserved.reserved_out_water[j]>0)EXPECT_NEAR(gw.heat().activeTemperature(0,j),40,1e-9);
    }
    std::vector<double> layer_capacity(reserved.column.energy.size()),layer_temperature(layer_capacity.size());
    ASSERT_EQ(swmm_heat_get_ground_layers(m.e,0,SWMM_GROUND_LAYER_CAPACITY,0,int(layer_capacity.size()),layer_capacity.data()),SWMM_OK);
    ASSERT_EQ(swmm_heat_get_ground_layers(m.e,0,SWMM_GROUND_LAYER_TEMPERATURE,0,int(layer_temperature.size()),layer_temperature.data()),SWMM_OK);
    for(std::size_t j=0;j<layer_capacity.size();++j) {
        EXPECT_DOUBLE_EQ(layer_capacity[j],gw.heat().activeCapacity(0,j));
        EXPECT_DOUBLE_EQ(layer_temperature[j],gw.heat().activeTemperature(0,j));
    }
    double bulk_capacity=0,bulk_temperature=0;
    ASSERT_EQ(swmm_heat_get_ground_values(m.e,SWMM_GROUND_HEAT_CAPACITY,0,1,&bulk_capacity),SWMM_OK);
    ASSERT_EQ(swmm_heat_get_ground_values(m.e,SWMM_GROUND_HEAT_TEMPERATURE,0,1,&bulk_temperature),SWMM_OK);
    EXPECT_NEAR(bulk_capacity,std::accumulate(layer_capacity.begin(),layer_capacity.end(),0.),1e-8);
    EXPECT_NEAR(bulk_temperature,std::accumulate(reserved.column.energy.begin(),reserved.column.energy.end(),0.)/bulk_capacity,1e-12);
    openswmm::detail::CompletedSourceArchive saved;gw.checkpoint(saved,false);
    openswmm::detail::CompletedSourceArchive input(saved.bytes());restored.checkpoint(input,false);input.finish();
    EXPECT_EQ(restored.heat().cells[0].reserved_out_water,reserved.reserved_out_water);
    EXPECT_EQ(restored.heat().cells[0].column.energy,reserved.column.energy);
    // A large but finite bath approaches a controlled 10 degC boundary.
    // This deliberately isolates the real face/link -> contact -> GW-gather
    // ordering without changing any accepted hydraulic transfer.
    constexpr double volume=1e8,seconds=3600;
    const double bath_before=10*volume;
    const double bath_after=openswmm::twoD::applyMeshHeatCell(m.router().mesh(),m.router().options(),state.heat,0,
        bath_before,volume,0,seconds);
    const double restored_bath_after=openswmm::twoD::applyMeshHeatCell(restored_model.router().mesh(),
        restored_model.router().options(),restored_state.heat,0,bath_before,volume,0,seconds);
    ASSERT_EQ(state.heat.failures[0],0)<<gw.heat().cells[0].error;
    ASSERT_EQ(restored_state.heat.failures[0],0)<<restored.heat().cells[0].error;
    EXPECT_DOUBLE_EQ(restored_bath_after,bath_after);
    EXPECT_EQ(restored.heat().cells[0].column.energy,gw.heat().cells[0].column.energy);
    gw.heat().refreshView(0,gw.transport());gw.assignTiers(seconds,1);gw.fireGwCells(0,seconds,state,0);
    restored.heat().refreshView(0,restored.transport());restored.assignTiers(seconds,1);
    restored.fireGwCells(0,seconds,restored_state,0);
    for(const auto* solver:{&gw,&restored})for(const auto& cell:solver->heat().cells) {
        ASSERT_EQ(cell.failed,0)<<cell.error;
        for(std::size_t j=0;j<cell.column.energy.size();++j) {
            const double temperature=cell.column.energy[j]/cell.column.capacity[j];
            EXPECT_GE(temperature,10-1e-8);EXPECT_LE(temperature,40+1e-8);
        }
        for(double capacity:cell.reserved_out_water)EXPECT_DOUBLE_EQ(capacity,0);
    }
    const double residual=energy(gw.heat())-before+cp*(bath_after-bath_before)+exported;
    const double energy_scale=std::abs(before)+std::abs(cp*(bath_after-bath_before))+std::abs(exported);
    EXPECT_LE(std::abs(residual),1e-8*std::max(1.0,energy_scale));
    EXPECT_NEAR(gw.state().continuityResidual(),0,1e-8);
}
TEST(GroundHeat, IsothermalAcceptedInfiltrationPreservesAllThreeClosuresAndSignedTemperature) {
    for(const auto* closure:{"CLOSED_FORM","ENSLAVED","SIGMA"})for(double hg:{0.,.5,1.}) {
        Model m(std::string("infil-")+closure+std::to_string(hg),closure,hg);ASSERT_NE(m.e,nullptr);
        ASSERT_EQ(m.start(),SWMM_OK);
        auto& gw=m.router().subsurface();auto& tr=gw.transport();ASSERT_TRUE(gw.heat().active());
        const int row=tr.temp_row;ASSERT_GE(row,0);const double initial=energy(gw.heat());
        gw.assignTiers(.25,1);double input=0;
        for(int k=0;k<20;++k) {
            constexpr double volume=1e-4;input+=volume;
            gw.state().xacc_from_surface[0]+=volume;tr.xacc_from_surface[tr.idx(row,0)]+=-3*volume;
            gw.fireGwCells(0,.25,m.router().state(),k*.25);
            for(const auto& cell:gw.heat().cells) {
                ASSERT_EQ(cell.failed,0)<<cell.error;
                for(std::size_t j=0;j<cell.column.energy.size();++j)
                    EXPECT_NEAR(cell.column.energy[j]/cell.column.capacity[j],-3,1e-9);
            }
        }
        double pending=0;for(int i=0;i<gw.state().n_cells;++i)pending+=tr.xacc_to_surface[tr.idx(row,i)]*4184000.;
        EXPECT_NEAR(energy(gw.heat())+pending-initial,-3*input*4184000.,1e-5);
    }
}
TEST(GroundHeat, PartialCoverageAndDisabledMasterDoNotInventThermalStores) {
    Model partial("partial","CLOSED_FORM",.5,true);ASSERT_NE(partial.e,nullptr);ASSERT_EQ(partial.start(),SWMM_OK);
    Model partial_restored("partial-restored","CLOSED_FORM",.5,true);
    ASSERT_NE(partial_restored.e,nullptr);ASSERT_EQ(partial_restored.start(),SWMM_OK);
    const auto& cells=partial.router().subsurface().heat().cells;ASSERT_EQ(cells.size(),2);
    EXPECT_FALSE(cells[0].column.energy.empty());EXPECT_TRUE(cells[1].column.energy.empty());
    auto runtime=partial.router().subsurface().heat();
    openswmm::detail::CompletedSourceArchive saved;openswmm::twoD::checkpoint::groundHeat(saved,runtime,false);
    auto restored=partial_restored.router().subsurface().heat();
    openswmm::detail::CompletedSourceArchive input(saved.bytes());
    openswmm::twoD::checkpoint::groundHeat(input,restored,false);input.finish();
    EXPECT_FALSE(restored.cells[0].reserved_out_water.empty());EXPECT_TRUE(restored.cells[1].reserved_out_water.empty());
    Model off("parent-off");ASSERT_NE(off.e,nullptr);
    ASSERT_EQ(swmm_options_set(off.e,"HEAT_TRANSPORT","NO"),SWMM_OK);ASSERT_EQ(off.start(),SWMM_OK);
    EXPECT_FALSE(off.router().subsurface().heat().active());
    char value[128]{};ASSERT_EQ(swmm_gw_transport_option_get(off.e,"THERMAL_MODEL",value,sizeof value),SWMM_OK);
    EXPECT_STREQ(value,"POROUS_EQUILIBRIUM");
}
TEST(GroundHeat, RuntimePowerUsesSoilStorageAndExpiryWithConfiguredWaterCapacity) {
    Model m("power");ASSERT_NE(m.e,nullptr);
    auto& ctx=static_cast<openswmm::SWMMEngine*>(m.e)->context();ctx.options.water_density=997;ctx.options.water_specific_heat=4210;
    ASSERT_EQ(m.start(),SWMM_OK);auto& gw=m.router().subsurface();const double initial=energy(gw.heat());
    const int ns=gw.transport().n_species;std::vector<double> conc(ns,0),rates(ns,0);
    ASSERT_EQ(swmm_coupling_set_source(m.e,"heater",1,0,0,1000,conc.data(),rates.data(),ns,2.),SWMM_OK);
    double actual=0;ASSERT_EQ(swmm_coupling_advance_to(m.e,2.,&actual),SWMM_OK);EXPECT_DOUBLE_EQ(actual,2.);
    for(const auto& c:gw.heat().cells)ASSERT_EQ(c.failed,0)<<c.error;
    EXPECT_NEAR(energy(gw.heat())-initial,2000,1e-5);
    ASSERT_EQ(swmm_coupling_advance_to(m.e,4.,&actual),SWMM_OK);
    EXPECT_NEAR(energy(gw.heat())-initial,2000,1e-5);
}
TEST(GroundHeat, GeothermalJoulesWarmNegativeGroundAndCloseTheCanonicalBudget) {
    Model m("geothermal");ASSERT_NE(m.e,nullptr);
    ASSERT_EQ(swmm_gw_transport_option_set(m.e,"CONDUCTION","YES"),SWMM_OK);
    ASSERT_EQ(swmm_gw_transport_option_set(m.e,"DEEP_THERMAL_BC","GEOTHERMAL_FLUX .065"),SWMM_OK);
    ASSERT_EQ(m.start(),SWMM_OK);auto& gw=m.router().subsurface();const double initial=energy(gw.heat());
    double actual=0;ASSERT_EQ(swmm_coupling_advance_to(m.e,5.,&actual),SWMM_OK);
    double boundary=0;for(const auto& c:gw.heat().cells){ASSERT_EQ(c.failed,0)<<c.error;boundary+=c.deep_j;}
    EXPECT_NEAR(boundary,.065*100*5,1e-8);EXPECT_NEAR(energy(gw.heat())-initial,boundary,1e-5);
}
TEST(GroundHeat, ThermalLayerInitialValuesAreIndependentOfHydraulicSigmaCount) {
    Model m("layers","SIGMA",.5,false,"* LAYER 1 __TEMPERATURE__ -10\n* LAYER 16 __TEMPERATURE__ 5\n");
    ASSERT_NE(m.e,nullptr);ASSERT_EQ(m.start(),SWMM_OK);auto& gw=m.router().subsurface();
    ASSERT_EQ(gw.state().m_layers,4);ASSERT_EQ(gw.heat().cells[0].column.energy.size(),16);
    auto& c=gw.heat().cells[0].column;
    EXPECT_DOUBLE_EQ(c.energy.front()/c.capacity.front(),-10);EXPECT_DOUBLE_EQ(c.energy.back()/c.capacity.back(),5);
    const double initial=energy(gw.heat());
    double actual=0;ASSERT_EQ(swmm_coupling_advance_to(m.e,5.,&actual),SWMM_OK);
    for(const auto& cell:gw.heat().cells)ASSERT_EQ(cell.failed,0)<<cell.error;
    EXPECT_NEAR(energy(gw.heat()),initial,1e-5);
}
TEST(GroundHeat, SoilEtAndDeepExtractionCarryTheirActualSignedSensibleEnergy) {
    for(const auto* closure:{"CLOSED_FORM","ENSLAVED","SIGMA"}) {
        Model m(std::string("et-deep-")+closure,closure);ASSERT_NE(m.e,nullptr);
        ASSERT_EQ(swmm_options_set_ext(m.e,"GW_ET","BOUNDARY_ET"),SWMM_OK);
        ASSERT_EQ(m.start(),SWMM_OK);auto& gw=m.router().subsurface();const double initial=energy(gw.heat());
        gw.assignTiers(.25,1);
        for(auto& loss:gw.state().c_loss)loss=1e-6;
        for(int k=0;k<20;++k) {
            for(int i=0;i<2;++i)gw.bookSurfaceEt(i,.25,1e-6,0);
            gw.fireGwCells(0,.25,m.router().state(),k*.25);
            for(const auto& cell:gw.heat().cells) {
                ASSERT_EQ(cell.failed,0)<<cell.error;
                for(std::size_t j=0;j<cell.column.energy.size();++j)
                    EXPECT_NEAR(cell.column.energy[j]/cell.column.capacity[j],-3,1e-9);
            }
        }
        EXPECT_GT(gw.state().led_et,0);EXPECT_GT(gw.state().led_deep,0);
        EXPECT_NEAR(energy(gw.heat())-initial,3*4184000.*(gw.state().led_et+gw.state().led_deep),1e-5);
    }
}
TEST(GroundHeat, SoilLatentCoolingIsOneSurfaceDebitFromActualSoilEvaporation) {
    Model m("soil-latent");ASSERT_NE(m.e,nullptr);
    ASSERT_EQ(swmm_options_set_ext(m.e,"GW_ET","BOUNDARY_ET"),SWMM_OK);
    ASSERT_EQ(swmm_gw_transport_option_set(m.e,"SOIL_ET_HEAT","LATENT_SURFACE"),SWMM_OK);
    ASSERT_EQ(m.start(),SWMM_OK);auto& gw=m.router().subsurface();const double initial=energy(gw.heat());
    gw.assignTiers(.25,1);gw.bookSurfaceEt(0,.25,1e-6,0);gw.fireGwCells(0,.25,m.router().state(),0);
    double latent=0,advective=0;
    for(const auto& cell:gw.heat().cells){ASSERT_EQ(cell.failed,0)<<cell.error;latent+=cell.latent_j;advective+=cell.advective_j;}
    ASSERT_GT(gw.state().led_et,0);
    EXPECT_NEAR(latent,-1000*gw.state().led_et*(2499e3+2360*3),1e-5);
    EXPECT_NEAR(energy(gw.heat())-initial,latent+advective,1e-5);
}
TEST(GroundHeat, UnqualifiedHotstartRefusesBeforeReplacingAnExistingFile) {
    Model m("hotstart-refusal");ASSERT_NE(m.e,nullptr);ASSERT_EQ(m.start(),SWMM_OK);
    const auto file=out/"must-survive.hsf";std::ofstream(file)<<"unrelated sentinel";
    EXPECT_EQ(swmm_hotstart_save(m.e,file.string().c_str()),SWMM_ERR_HOTSTART);
    std::ifstream input(file);const std::string text((std::istreambuf_iterator<char>(input)),{});
    EXPECT_EQ(text,"unrelated sentinel");
}
TEST(GroundHeat, DisabledAquiferKeepsWaterOnlySurfaceHeatConversion) {
    Model m("aquifer-disabled");ASSERT_NE(m.e,nullptr);
    ASSERT_EQ(swmm_options_set_ext(m.e,"GROUNDWATER","NO"),SWMM_OK);
    ASSERT_EQ(m.start(),SWMM_OK);EXPECT_FALSE(m.router().subsurface().heat().active());
    EXPECT_DOUBLE_EQ(m.router().options().water_heat_capacity,4186000.);
    const auto& t=m.router().state().transport;const int ns=t.n_species,r=t.temp_row;ASSERT_GE(r,0);
    const double initial=t.cell_mass[t.idx(r,0)]+t.cell_mass[t.idx(r,1)];
    std::vector<double> conc(ns,0),rates(ns,0);
    ASSERT_EQ(swmm_coupling_set_source(m.e,"heater",0,0,0,1000,conc.data(),rates.data(),ns,2.),SWMM_OK);
    double actual=0;ASSERT_EQ(swmm_coupling_advance_to(m.e,2.,&actual),SWMM_OK);
    EXPECT_NEAR((t.cell_mass[t.idx(r,0)]+t.cell_mass[t.idx(r,1)]-initial)*4186000.,2000,1e-5);
}
TEST(GroundHeat, SoluteMolecularDiffusivityDoesNotDuplicateThermalConduction) {
    Model m("no-double-diffusion","CLOSED_FORM",.5,false,
        "CELL 1 SAT __TEMPERATURE__ 10\nCELL 1 UNSAT __TEMPERATURE__ 10\n"
        "CELL 2 SAT __TEMPERATURE__ 20\nCELL 2 UNSAT __TEMPERATURE__ 20\n"
        "[GW_TRANSPORT_PARAMS]\n* 2650 880 2 0 0 0 1 0 .065\n");
    ASSERT_NE(m.e,nullptr);ASSERT_EQ(swmm_gw_transport_option_set(m.e,"DISPERSION","YES"),SWMM_OK);
    ASSERT_EQ(m.start(),SWMM_OK);auto& heat=m.router().subsurface().heat();
    const double e0=sum(heat.cells[0].column.energy),e1=sum(heat.cells[1].column.energy);
    double actual=0;ASSERT_EQ(swmm_coupling_advance_to(m.e,5.,&actual),SWMM_OK);
    for(const auto& c:heat.cells)ASSERT_EQ(c.failed,0)<<c.error;
    EXPECT_NEAR(sum(heat.cells[0].column.energy),e0,1e-5);
    EXPECT_NEAR(sum(heat.cells[1].column.energy),e1,1e-5);
}
TEST(GroundHeat, SurfaceContactBorrowsAquiferColumnsAndFillsOnlyUncoveredCells) {
    Model m("shared-soil","SIGMA",.5,true);ASSERT_NE(m.e,nullptr);
    ASSERT_EQ(swmm_options_set_ext(m.e,"HEAT_MODEL","COUPLED"),SWMM_OK);
    ASSERT_EQ(swmm_options_set_ext(m.e,"BED_HEAT_MODEL","GROUND_COLUMN"),SWMM_OK);
    ASSERT_EQ(swmm_options_set_ext(m.e,"HEAT_SURFACE_EXCHANGE","NO"),SWMM_OK);
    ASSERT_EQ(swmm_options_set_ext(m.e,"HEAT_RADIATIVE_EXCHANGE","NO"),SWMM_OK);
    ASSERT_EQ(swmm_gw_transport_option_set(m.e,"CONDUCTION","YES"),SWMM_OK);
    ASSERT_EQ(m.start(),SWMM_OK);auto& state=m.router().state();auto& h=state.heat;auto& gw=m.router().subsurface();
    ASSERT_TRUE(h.ground_column);ASSERT_EQ(h.aquifer_owned.size(),2);
    EXPECT_TRUE(h.aquifer_owned[0]);EXPECT_FALSE(h.aquifer_owned[1]);
    EXPECT_TRUE(h.standalone_ground.cells[0].column.energy.empty());
    EXPECT_TRUE(gw.heat().cells[1].column.energy.empty());
    EXPECT_EQ(&h.groundOwner(0).cells[0],&gw.heat().cells[0]);
    const auto total=[&](){double e=energy(gw.heat())+energy(h.standalone_ground);
        for(int i=0;i<2;++i)e+=state.transport.cell_mass[state.transport.idx(state.transport.temp_row,i)]*4184000.;return e;};
    const double initial=total();double actual=0;
    ASSERT_EQ(swmm_coupling_advance_to(m.e,5.,&actual),SWMM_OK);
    EXPECT_NEAR(total(),initial,2e-5);
    EXPECT_GT(gw.heat().cells[0].water_contact_j,0);
    EXPECT_GT(gw.heat().cells[0].column.energy.front()/gw.heat().cells[0].column.capacity.front(),-3);
    double residual[2];ASSERT_EQ(swmm_heat_get_2d_values(m.e,SWMM_HEAT_2D_THERMAL_RESIDUAL,0,2,residual),SWMM_OK);
    EXPECT_NEAR(residual[0],0,2e-5);EXPECT_NEAR(residual[1],0,2e-5);
}
TEST(GroundHeat, SurfaceWaterBoundaryRequiresAnActualFiniteContact) {
    Model m("missing-contact");ASSERT_NE(m.e,nullptr);
    ASSERT_EQ(swmm_gw_transport_option_set(m.e,"CONDUCTION","YES"),SWMM_OK);
    ASSERT_EQ(swmm_gw_transport_option_set(m.e,"SURFACE_THERMAL_BC","SURFACE_WATER"),SWMM_OK);
    EXPECT_NE(m.start(),SWMM_OK);
}
TEST(GroundHeat, SoilOnlySurfaceReservoirUsesPrescribedSignedTemperatureAndConsumesReturnHeat) {
    Model m("soil-only-donor","SIGMA");ASSERT_NE(m.e,nullptr);
    ASSERT_EQ(swmm_options_set_ext(m.e,"TRANSPORT_TEMPERATURE","NO"),SWMM_OK);
    ASSERT_EQ(swmm_heat_set_source_temp(m.e,0,-7),SWMM_OK);
    ASSERT_EQ(m.start(),SWMM_OK);auto& gw=m.router().subsurface();
    ASSERT_LT(m.router().state().transport.temp_row,0);ASSERT_TRUE(gw.heat().active());
    const double initial=energy(gw.heat());
    SWMM_CouplingForcing forcing{};forcing.struct_size=sizeof forcing;
    forcing.channel=SWMM_COUPLING_INFILTRATION;forcing.cell=0;forcing.mode=1;forcing.rate_m_s=1e-7;forcing.until_seconds=1.;
    SWMM_CouplingFrame frame{};frame.struct_size=sizeof frame;frame.version=SWMM_COUPLING_FRAME_VERSION;
    frame.forcing_count=1;frame.forcings=&forcing;
    ASSERT_EQ(swmm_coupling_apply_frame(m.e,&frame),SWMM_OK);double actual=0;
    ASSERT_EQ(swmm_coupling_advance_to(m.e,1.,&actual),SWMM_OK);
    SWMM_CouplingReceipt receipt{};
    const int ns=m.router().state().transport.n_species;std::vector<double> requested(ns),applied(ns);
    ASSERT_EQ(swmm_coupling_get_infiltration_receipt(m.e,0,&receipt,requested.data(),applied.data(),ns),SWMM_OK);
    EXPECT_GT(receipt.applied_m3,0);EXPECT_NEAR(receipt.applied_heat_j,-7*4184000.*receipt.applied_m3,1e-8);
    // The aquifer may return accepted water; its boundary export ledger is
    // already paired. Incoming heat is exact even without a surface row.
    double adv=0;for(const auto& c:gw.heat().cells){ASSERT_EQ(c.failed,0)<<c.error;adv+=c.advective_j;}
    EXPECT_NEAR(energy(gw.heat())-initial,adv,1e-5);
    gw.state().xacc_to_surface[0]+=1e-4;
    const auto row=gw.transport().idx(gw.transport().temp_row,0);gw.transport().xacc_to_surface[row]+=-3e-4;
    ASSERT_EQ(swmm_coupling_advance_to(m.e,2.,&actual),SWMM_OK);
    EXPECT_DOUBLE_EQ(gw.state().xacc_to_surface[0],0);EXPECT_DOUBLE_EQ(gw.transport().xacc_to_surface[row],0);
    EXPECT_EQ(swmm_options_set_ext(m.e,"TRANSPORT_TEMPERATURE","YES"),SWMM_ERR_LIFECYCLE);
    EXPECT_EQ(swmm_options_set_ext(m.e,"GROUNDWATER","NO"),SWMM_ERR_LIFECYCLE);
}
TEST(GroundHeat, PublicSoilResultsReadCanonicalJoulesAndValidateRangesAtomically) {
    Model m("soil-results","SIGMA",.5,true);ASSERT_NE(m.e,nullptr);
    SWMM_GroundHeatStatus status{};status.struct_size=sizeof status;
    ASSERT_EQ(swmm_heat_get_ground_status(m.e,&status),SWMM_OK);
    EXPECT_TRUE(status.requested);EXPECT_FALSE(status.initialized);EXPECT_EQ(status.aquifer_cells,0);
    double values[2]={123,456};
    EXPECT_EQ(swmm_heat_get_ground_values(m.e,SWMM_GROUND_HEAT_ENERGY,0,2,values),SWMM_ERR_LIFECYCLE);
    EXPECT_EQ(values[0],123);EXPECT_EQ(values[1],456);
    ASSERT_EQ(m.start(),SWMM_OK);ASSERT_EQ(swmm_heat_get_ground_status(m.e,&status),SWMM_OK);
    EXPECT_EQ(status.aquifer_cells,1);EXPECT_EQ(status.standalone_cells,0);EXPECT_EQ(status.thermal_layers,16);
    EXPECT_FALSE(status.conduction_active);
    ASSERT_EQ(swmm_heat_get_ground_values(m.e,SWMM_GROUND_HEAT_ENERGY,0,2,values),SWMM_OK);
    EXPECT_DOUBLE_EQ(values[0],energy(m.router().subsurface().heat()));EXPECT_TRUE(std::isnan(values[1]));
    std::vector<double> layers(16);
    ASSERT_EQ(swmm_heat_get_ground_layers(m.e,0,SWMM_GROUND_LAYER_ENERGY,0,16,layers.data()),SWMM_OK);
    EXPECT_DOUBLE_EQ(sum(layers),values[0]);
    ASSERT_EQ(swmm_heat_get_ground_values(m.e,SWMM_GROUND_HEAT_NORMALIZED_RESIDUAL,0,1,values),SWMM_OK);
    EXPECT_NEAR(values[0],0,1e-12);
    values[0]=123;values[1]=456;
    EXPECT_EQ(swmm_heat_get_ground_layers(m.e,0,SWMM_GROUND_LAYER_ENERGY,15,2,values),SWMM_ERR_BADINDEX);
    EXPECT_EQ(values[0],123);EXPECT_EQ(values[1],456);
    ASSERT_EQ(swmm_engine_end(m.e),SWMM_OK);m.running=false;
    EXPECT_EQ(swmm_heat_get_ground_layers(m.e,0,SWMM_GROUND_LAYER_ENERGY,0,2,values),SWMM_OK);
}
TEST(GroundHeat, SaturatedLateralConductionUsesTheFaceClockAndRetainsCrossTierPendingJoules) {
    Model m("lateral-pending","CLOSED_FORM",1,false,
        "CELL 1 SAT __TEMPERATURE__ 10\nCELL 2 SAT __TEMPERATURE__ 20\n"
        "[GW_TRANSPORT_PARAMS]\n* 2650 880 2000000 0 0 0 0 0 .065\n");
    ASSERT_NE(m.e,nullptr);
    ASSERT_EQ(swmm_gw_transport_option_set(m.e,"CONDUCTION","YES"),SWMM_OK);
    ASSERT_EQ(swmm_gw_transport_option_set(m.e,"LATERAL_CONDUCTION","YES"),SWMM_OK);
    ASSERT_EQ(m.start(),SWMM_OK);auto& gw=m.router().subsurface();const double initial=energy(gw.heat());
    EXPECT_GT(gw.minDtCell(),.8);
    gw.state().dt_cell[0]=.1;gw.state().dt_cell[1]=.8;gw.assignTiers(.1,4);
    for(int k=0;k<80;++k) {
        gw.fireGwFaces(0,.1);gw.fireGwCells(0,.1,m.router().state(),k*.1);
        if(k%8==7)gw.fireGwCells(3,.8,m.router().state(),(k-7)*.1);
        for(const auto& c:gw.heat().cells)ASSERT_EQ(c.failed,0)<<c.error;
        const double pending=gw.pendingHeat(0)+gw.pendingHeat(1);
        EXPECT_NEAR(energy(gw.heat())+pending,initial,2e-5);
        if(k==0){EXPECT_LT(gw.pendingHeat(1),0);EXPECT_DOUBLE_EQ(gw.pendingHeat(0),0);}
        for(const auto& c:gw.heat().cells)for(std::size_t j=0;j<c.column.energy.size();++j) {
            EXPECT_GE(c.column.energy[j]/c.column.capacity[j],10-1e-9);
            EXPECT_LE(c.column.energy[j]/c.column.capacity[j],20+1e-9);
        }
    }
    EXPECT_NEAR(gw.pendingHeat(0)+gw.pendingHeat(1),0,1e-7);
    EXPECT_GT(gw.heat().zoneTemperature(0,true),10);EXPECT_LT(gw.heat().zoneTemperature(1,true),20);
    EXPECT_NEAR(gw.heat().cells[0].lateral_j+gw.heat().cells[1].lateral_j,0,1e-6);
}
TEST(GroundHeat, PendingLateralHeatSurvivesAquiferRetiering) {
    Model m("lateral-retier","CLOSED_FORM",1,false,
        "CELL 1 SAT __TEMPERATURE__ 10\nCELL 2 SAT __TEMPERATURE__ 20\n"
        "[GW_TRANSPORT_PARAMS]\n* 2650 880 2000000 0 0 0 0 0 .065\n");
    ASSERT_NE(m.e,nullptr);
    ASSERT_EQ(swmm_gw_transport_option_set(m.e,"CONDUCTION","YES"),SWMM_OK);
    ASSERT_EQ(swmm_gw_transport_option_set(m.e,"LATERAL_CONDUCTION","YES"),SWMM_OK);
    ASSERT_EQ(m.start(),SWMM_OK);
    auto& gw=m.router().subsurface();
    const double initial=energy(gw.heat());
    gw.state().dt_cell[0]=.1;gw.state().dt_cell[1]=.8;gw.assignTiers(.1,4);
    ASSERT_EQ(gw.state().tier[0],0);ASSERT_EQ(gw.state().tier[1],3);

    gw.fireGwFaces(0,.1);
    const double pending_before=gw.pendingHeat(0)+gw.pendingHeat(1);
    ASSERT_GT(std::abs(gw.pendingHeat(0))+std::abs(gw.pendingHeat(1)),0);
    EXPECT_NEAR(energy(gw.heat())+pending_before,initial,2e-5);

    // Reverse the fast and slow owners while a signed face receipt is queued.
    gw.state().dt_cell[0]=.8;gw.state().dt_cell[1]=.1;gw.assignTiers(.1,4);
    ASSERT_EQ(gw.state().tier[0],3);ASSERT_EQ(gw.state().tier[1],0);
    gw.fireGwCells(0,.1,m.router().state(),0);
    EXPECT_NEAR(energy(gw.heat())+gw.pendingHeat(0)+gw.pendingHeat(1),initial,2e-5);

    for(int k=0;k<8;++k) {
        gw.fireGwFaces(0,.1);
        gw.fireGwCells(0,.1,m.router().state(),k*.1);
        if(k==7)gw.fireGwCells(3,.8,m.router().state(),0);
        for(const auto& cell:gw.heat().cells) {
            ASSERT_EQ(cell.failed,0)<<cell.error;
            for(std::size_t j=0;j<cell.column.energy.size();++j) {
                const double temperature=cell.column.energy[j]/cell.column.capacity[j];
                EXPECT_GE(temperature,10-1e-9);EXPECT_LE(temperature,20+1e-9);
            }
        }
        EXPECT_NEAR(energy(gw.heat())+gw.pendingHeat(0)+gw.pendingHeat(1),initial,2e-5);
    }
    EXPECT_NEAR(gw.pendingHeat(0)+gw.pendingHeat(1),0,1e-7);
    EXPECT_NEAR(gw.heat().cells[0].lateral_j+gw.heat().cells[1].lateral_j,0,1e-6);
}
TEST(GroundHeat, RoutedChannelsAndMultipleCellsShareFiniteConservativeSoilAndSpeciesSolves) {
    const std::string extra="[JUNCTIONS]\nJ2 -.2 4 0 0 0\n[OUTFALLS]\nO2 -.2 FREE NO\n"
        "[CONDUITS]\nC2 J2 O2 8 .013 0 0 0\n[XSECTIONS]\nC2 RECT_OPEN 1 2 0 0 1\n"
        "[COORDINATES]\nJ2 1 7\nO2 9 7\n[DWF]\nJ1 FLOW .01\nJ2 FLOW .01\n[TRANSPORT_OPTIONS]\nTARGET_DX 2\n"+
        porewater_species;
    for(const auto* solver:{"LEGACY","EULERIAN_ARD","LAGRANGIAN"}) {
        Model m(std::string("channel-operator-")+solver,"SIGMA",.5,false,extra,true);ASSERT_NE(m.e,nullptr);
        ASSERT_EQ(swmm_options_set(m.e,"QUALITY_SOLVER",solver),SWMM_OK);
        ASSERT_EQ(swmm_options_set_ext(m.e,"HEAT_MODEL","COUPLED"),SWMM_OK);
        ASSERT_EQ(swmm_options_set_ext(m.e,"BED_HEAT_MODEL","GROUND_COLUMN"),SWMM_OK);
        ASSERT_EQ(swmm_options_set_ext(m.e,"HEAT_SURFACE_EXCHANGE","NO"),SWMM_OK);
        ASSERT_EQ(swmm_options_set_ext(m.e,"HEAT_RADIATIVE_EXCHANGE","NO"),SWMM_OK);
        ASSERT_EQ(swmm_gw_transport_option_set(m.e,"CONDUCTION","YES"),SWMM_OK);
        ASSERT_EQ(m.start(),SWMM_OK);double actual=0;ASSERT_EQ(swmm_coupling_advance_to(m.e,5.,&actual),SWMM_OK);
        auto& ctx=static_cast<openswmm::SWMMEngine*>(m.e)->context();auto& gw=m.router().subsurface();
        // Isolate the finite contact operator. Normal runtime activation is
        // tested separately once its source-clock coordinator is installed.
        ctx.heat_config.sediment_exchange=true;ctx.heat_config.sediment.mesh_ground=true;
        ctx.heat_config.sediment.hyporheic_velocity=2e-5;
        openswmm::twoD::ChannelGroundState contacts;
        ASSERT_TRUE(contacts.initialize(ctx,m.router().mesh(),gw.heat(),gw.transport(),m.router().state(),1.).empty());
        EXPECT_TRUE(contacts.ownsLink(0));EXPECT_TRUE(contacts.ownsLink(1));EXPECT_EQ(contacts.cells().size(),2);
        ASSERT_GT(contacts.contacts().size(),2);
        const auto water_energy=[&](){double result=0;for(const auto& r:ctx.state_accessors.get_channel_thermal_regions())result+=r.volume*r.temperature*4184000.;return result;};
        const double initial=energy(gw.heat())+water_energy();const auto hg=gw.state().hg,hu=gw.state().hu;
        const auto volume=m.router().state().volume;std::vector<double> surface;
        const std::vector<openswmm::twoD::SurfaceThermalSample> samples(m.router().mesh().n_cells());
        const int dye=gw.transport().rowIndex("Dye"),age=gw.transport().age_row;
        ASSERT_GE(dye,0);ASSERT_GE(age,0);
        auto channel_species=ctx.state_accessors.get_channel_species_regions();ASSERT_FALSE(channel_species.empty());
        std::vector<openswmm::ChannelSpeciesIncrement> seed;seed.reserve(channel_species.size());
        for(const auto& region:channel_species) {
            openswmm::ChannelSpeciesIncrement update;update.link=region.link;update.region=region.region;
            update.concentration_delta.assign(region.concentration.size(),0);
            update.concentration_delta[dye]=(region.link==0?100:0)-region.concentration[dye];
            update.concentration_delta[age]=(region.link==0?120:0)-region.concentration[age];
            seed.push_back(std::move(update));
        }
        ASSERT_TRUE(ctx.state_accessors.apply_channel_species_increments(seed).empty());
        channel_species=ctx.state_accessors.get_channel_species_regions();
        const auto channel_mass=[&](int row) {
            double result=0;
            for(const auto& region:channel_species)result+=region.volume*region.concentration[row];
            return result;
        };
        const double dye_initial=poreMass(gw.heat(),dye)+channel_mass(dye);
        const double age_initial=poreMass(gw.heat(),age)+channel_mass(age);
        double dye_ground_receipt_before=0;
        for(const auto& cell:gw.heat().cells)if(cell.porewater.active()) {
            const auto nl=cell.column.theta.size();
            for(std::size_t j=0;j<nl;++j)dye_ground_receipt_before+=cell.porewater.contact_mass[std::size_t(dye)*nl+j];
        }
        const auto species_receipt_before=contacts.channelSpeciesToGround();
        const auto error=contacts.advance(m.router().options(),samples,3600,surface);ASSERT_TRUE(error.empty())<<error;
        channel_species=ctx.state_accessors.get_channel_species_regions();
        EXPECT_NEAR(energy(gw.heat())+water_energy(),initial,2e-5);
        EXPECT_NEAR(poreMass(gw.heat(),dye)+channel_mass(dye),dye_initial,1e-9);
        EXPECT_NEAR(poreMass(gw.heat(),age)+channel_mass(age),age_initial,1e-8);
        EXPECT_EQ(gw.state().hg,hg);EXPECT_EQ(gw.state().hu,hu);EXPECT_EQ(m.router().state().volume,volume);
        EXPECT_GT(sum(contacts.channelToGround()),0);
        EXPECT_NEAR(sum(contacts.channelThermalIncrement())+sum(contacts.channelToGround())-sum(contacts.channelAtmosphere()),0,1e-6);
        double soil_receipt=0;for(const auto& c:gw.heat().cells) {
            soil_receipt+=c.channel_contact_j;for(std::size_t j=0;j<c.column.energy.size();++j) {
                EXPECT_GE(c.column.energy[j]/c.column.capacity[j],-3-1e-10);EXPECT_LE(c.column.energy[j]/c.column.capacity[j],20+1e-10);
            }
        }
        EXPECT_NEAR(soil_receipt,sum(contacts.channelToGround()),1e-6);
        double dye_ground_receipt_after=0;
        for(const auto& cell:gw.heat().cells)if(cell.porewater.active()) {
            const auto nl=cell.column.theta.size();
            for(std::size_t j=0;j<nl;++j)dye_ground_receipt_after+=cell.porewater.contact_mass[std::size_t(dye)*nl+j];
        }
        const auto& species_receipt_after=contacts.channelSpeciesToGround();
        double dye_channel_receipt=0;
        for(int link=0;link<ctx.n_links();++link)
            dye_channel_receipt+=species_receipt_after[std::size_t(dye)*ctx.n_links()+link]-species_receipt_before[std::size_t(dye)*ctx.n_links()+link];
        EXPECT_NEAR(dye_ground_receipt_after-dye_ground_receipt_before,dye_channel_receipt,1e-9);
        EXPECT_GT(species_receipt_after[std::size_t(dye)*ctx.n_links()],0);
        EXPECT_LT(species_receipt_after[std::size_t(dye)*ctx.n_links()+1],0);
        const auto saved=ctx.state_accessors.apply_channel_thermal_increments;
        ctx.state_accessors.apply_channel_thermal_increments=[](const auto&){return std::string("forced binding refusal");};
        const double before_soil=energy(gw.heat()),before_water=water_energy();
        EXPECT_EQ(contacts.advance(m.router().options(),samples,60,surface),"forced binding refusal");
        EXPECT_DOUBLE_EQ(energy(gw.heat()),before_soil);EXPECT_DOUBLE_EQ(water_energy(),before_water);
        ctx.state_accessors.apply_channel_thermal_increments=saved;
    }
}
TEST(GroundHeat, ConfiguredChannelContactsRunOnTheActualSourceClockAndHonorLiveBedOff) {
    for(const auto* solver:{"LEGACY","EULERIAN_ARD","LAGRANGIAN"}) {
        SCOPED_TRACE(solver);
        Model m(std::string("channel-runtime-")+solver,"SIGMA",.5,false,
            "[DWF]\nJ1 FLOW .01\n[TRANSPORT_OPTIONS]\nTARGET_DX 2\n",true);
        ASSERT_NE(m.e,nullptr);ASSERT_EQ(swmm_options_set(m.e,"QUALITY_SOLVER",solver),SWMM_OK);
        ASSERT_EQ(swmm_heat_set_sediment(m.e,SWMM_HEAT_SED_GROUND_SOURCE,1),SWMM_OK);
        ASSERT_EQ(swmm_heat_set_sediment(m.e,SWMM_HEAT_SED_HYPORHEIC_VELOCITY,2e-5),SWMM_OK);
        ASSERT_EQ(swmm_heat_set_module(m.e,SWMM_HEAT_SEDIMENT_EXCHANGE,1),SWMM_OK);
        ASSERT_EQ(swmm_options_set_ext(m.e,"HEAT_MODEL","COUPLED"),SWMM_OK);
        ASSERT_EQ(swmm_options_set_ext(m.e,"BED_HEAT_MODEL","GROUND_COLUMN"),SWMM_OK);
        ASSERT_EQ(swmm_options_set_ext(m.e,"HEAT_SURFACE_EXCHANGE","NO"),SWMM_OK);
        ASSERT_EQ(swmm_options_set_ext(m.e,"HEAT_RADIATIVE_EXCHANGE","NO"),SWMM_OK);
        ASSERT_EQ(swmm_gw_transport_option_set(m.e,"CONDUCTION","YES"),SWMM_OK);
        ASSERT_EQ(m.start(),SWMM_OK);ASSERT_TRUE(m.router().channelGround().active());
        auto& ctx=static_cast<openswmm::SWMMEngine*>(m.e)->context();
        ASSERT_EQ(ctx.heat_state.mesh_contact_link.size(),1);EXPECT_TRUE(ctx.heat_state.mesh_contact_link[0]);
        auto& gw=m.router().subsurface();auto& state=m.router().state();
        const auto surface_energy=[&](){double e=0;for(int i=0;i<2;++i)e+=state.transport.cell_mass[state.transport.idx(state.transport.temp_row,i)]*4184000.;return e;};
        const double initial=surface_energy();double actual=0;
        ASSERT_EQ(swmm_coupling_advance_to(m.e,5.,&actual),SWMM_OK);
        ASSERT_EQ(ctx.heat_state.mesh_contact_link.size(),1);EXPECT_TRUE(ctx.heat_state.mesh_contact_link[0]);
        const auto& contacts=m.router().channelGround();
        EXPECT_GT(sum(contacts.channelToGround()),0);
        EXPECT_NEAR(surface_energy()-initial,sum(state.heat.water_j),2e-5);
        EXPECT_NEAR(sum(state.heat.water_j)+sum(state.heat.water_bed_j)-sum(state.heat.atmospheric_j),0,2e-5);
        EXPECT_NEAR(sum(contacts.channelThermalIncrement())+sum(contacts.channelToGround())-sum(contacts.channelAtmosphere()),0,1e-5);
        double residual[2];ASSERT_EQ(swmm_heat_get_ground_values(m.e,SWMM_GROUND_HEAT_NORMALIZED_RESIDUAL,0,2,residual),SWMM_OK);
        EXPECT_NEAR(residual[0],0,1e-10);EXPECT_NEAR(residual[1],0,1e-10);
        EXPECT_EQ(swmm_heat_set_sediment(m.e,SWMM_HEAT_SED_BED_THICKNESS,.3),SWMM_ERR_LIFECYCLE);
        ASSERT_EQ(swmm_heat_set_module(m.e,SWMM_HEAT_SEDIMENT_EXCHANGE,0),SWMM_OK);
        const double paused=sum(contacts.channelToGround());
        ASSERT_EQ(swmm_coupling_advance_to(m.e,10.,&actual),SWMM_OK);
        EXPECT_DOUBLE_EQ(sum(contacts.channelToGround()),paused);
        ASSERT_EQ(swmm_heat_set_module(m.e,SWMM_HEAT_SEDIMENT_EXCHANGE,1),SWMM_OK);
        ASSERT_EQ(swmm_coupling_advance_to(m.e,15.,&actual),SWMM_OK);
        EXPECT_GT(sum(contacts.channelToGround()),paused);
        for(const auto& cell:gw.heat().cells)ASSERT_EQ(cell.failed,0)<<cell.error;
    }
}
TEST(GroundHeat, RoutedSeepageReversesWhileHyporheicHeatRemainsSeparate) {
    for(const auto* solver:{"LEGACY","EULERIAN_ARD","LAGRANGIAN"}) {
        SCOPED_TRACE(solver);
        Model m(std::string("channel-seepage-reversal-")+solver,"SIGMA",.5,false,
            porewater_species+"[DWF]\nJ1 FLOW .01\n[INFLOWS]\nJ1 FLOW IN1 FLOW 1.0 1.0\n"
            "[TIMESERIES]\nIN1 0:00 0.1\nIN1 1:00 0.1\n[LOSSES]\nC1 0 0 0 NO 100\n"
            "[TRANSPORT_OPTIONS]\nTARGET_DX 2\n",true);
        ASSERT_NE(m.e,nullptr);
        ASSERT_EQ(swmm_options_set(m.e,"QUALITY_SOLVER",solver),SWMM_OK);
        ASSERT_EQ(swmm_gw2d_option_set(m.e,"LINK_SEEPAGE","TWO_WAY"),SWMM_OK);
        ASSERT_EQ(swmm_heat_set_sediment(m.e,SWMM_HEAT_SED_GROUND_SOURCE,1),SWMM_OK);
        ASSERT_EQ(swmm_heat_set_sediment(m.e,SWMM_HEAT_SED_HYPORHEIC_VELOCITY,2e-5),SWMM_OK);
        ASSERT_EQ(swmm_heat_set_module(m.e,SWMM_HEAT_SEDIMENT_EXCHANGE,1),SWMM_OK);
        ASSERT_EQ(swmm_options_set_ext(m.e,"HEAT_MODEL","COUPLED"),SWMM_OK);
        ASSERT_EQ(swmm_options_set_ext(m.e,"BED_HEAT_MODEL","GROUND_COLUMN"),SWMM_OK);
        ASSERT_EQ(swmm_options_set_ext(m.e,"HEAT_SURFACE_EXCHANGE","NO"),SWMM_OK);
        ASSERT_EQ(swmm_options_set_ext(m.e,"HEAT_RADIATIVE_EXCHANGE","NO"),SWMM_OK);
        ASSERT_EQ(swmm_gw_transport_option_set(m.e,"CONDUCTION","NO"),SWMM_OK);
        ASSERT_EQ(m.start(),SWMM_OK);
        auto& ctx=static_cast<openswmm::SWMMEngine*>(m.e)->context();
        auto& router=m.router();auto& gw=router.subsurface();auto& tr=gw.transport();
        ASSERT_TRUE(router.channelGround().active());ASSERT_GE(tr.temp_row,0);
        double actual=0;
        ASSERT_EQ(swmm_coupling_advance_to(m.e,1.,&actual),SWMM_OK)<<swmm_get_last_error_msg(m.e);
        ASSERT_GT(std::abs(ctx.links.flow[0]),1e-6);
        auto thermal=ctx.state_accessors.get_channel_thermal_regions();ASSERT_FALSE(thermal.empty());
        std::vector<openswmm::ChannelThermalIncrement> warm_channel;warm_channel.reserve(thermal.size());
        for(const auto& region:thermal)warm_channel.push_back({region.link,region.region,20-region.temperature});
        ASSERT_TRUE(ctx.state_accessors.apply_channel_thermal_increments(warm_channel).empty());

        struct Terms {double energy=0,advective=0,other=0,channel=0,channel_receipt=0;};
        const auto terms=[&]() {
            Terms result;
            for(const auto& cell:gw.heat().cells) {
                for(double value:cell.column.energy)result.energy+=value;
                result.advective+=cell.advective_j;
                result.channel+=cell.channel_contact_j;
                result.other+=cell.source_j+cell.surface_j+cell.deep_j+cell.latent_j+
                              cell.lateral_j+cell.water_contact_j;
            }
            result.channel_receipt=sum(router.channelGround().channelToGround());
            return result;
        };
        const auto checkEnergy=[&](const Terms& before,const Terms& after) {
            const double d_energy=after.energy-before.energy;
            const double d_receipts=(after.advective-before.advective)+
                (after.other-before.other)+(after.channel-before.channel);
            const double scale=std::max(1.,std::abs(after.advective-before.advective)+
                std::abs(after.other-before.other)+std::abs(after.channel-before.channel));
            EXPECT_LE(std::abs(d_energy-d_receipts),1e-6*scale);
            EXPECT_NEAR(after.channel-before.channel,after.channel_receipt-before.channel_receipt,1e-6*scale);
        };
        const auto before_loss=ctx.mass_balance.routing_seep_loss;
        const auto before_link=gw.state().led_link;
        const auto before_hyporheic=sum(router.channelGround().channelToGround());
        const auto before_positive=terms();
        ASSERT_EQ(swmm_forcing_link_seepage(m.e,0,.001,SWMM_FORCING_OVERRIDE,SWMM_FORCING_PERSIST),SWMM_OK);
        ASSERT_EQ(swmm_coupling_advance_to(m.e,6.,&actual),SWMM_OK)<<swmm_get_last_error_msg(m.e);
        const auto after_positive=terms();
        EXPECT_GT(ctx.mass_balance.routing_seep_loss-before_loss,0);
        EXPECT_GT(gw.state().led_link-before_link,0);
        EXPECT_GT(after_positive.advective-before_positive.advective,0);
        EXPECT_GT(after_positive.channel-before_positive.channel,0);
        checkEnergy(before_positive,after_positive);
        const auto positive_hyporheic=sum(router.channelGround().channelToGround())-before_hyporheic;
        EXPECT_GT(positive_hyporheic,0);

        const auto before_gain=ctx.mass_balance.routing_link_gw_inflow;
        const auto before_reverse_link=gw.state().led_link;
        const auto before_reverse_hyporheic=sum(router.channelGround().channelToGround());
        const auto before_negative=terms();
        ASSERT_EQ(swmm_forcing_link_seepage(m.e,0,-.001,SWMM_FORCING_OVERRIDE,SWMM_FORCING_PERSIST),SWMM_OK);
        ASSERT_EQ(swmm_coupling_advance_to(m.e,11.,&actual),SWMM_OK)<<swmm_get_last_error_msg(m.e);
        const auto after_negative=terms();
        EXPECT_GT(ctx.mass_balance.routing_link_gw_inflow-before_gain,0);
        EXPECT_LT(gw.state().led_link-before_reverse_link,0);
        // The heat receipt is signed sensible energy relative to 0 °C. Exporting
        // subzero groundwater can therefore increase the stored signed energy;
        // the accepted-water direction is asserted by the separate link ledger.
        EXPECT_NE(after_negative.advective,before_negative.advective);
        EXPECT_GT(after_negative.channel-before_negative.channel,0);
        checkEnergy(before_negative,after_negative);
        EXPECT_GT(sum(router.channelGround().channelToGround())-before_reverse_hyporheic,0);
        EXPECT_NEAR(gw.state().continuityResidual(),0,1e-8);
    }
}
TEST(GroundHeat, RoutedMeshContactReplacesThePrescribedBedStoreAcrossNetworkSolvers) {
    for(const auto* solver:{"LEGACY","EULERIAN_ARD","LAGRANGIAN"}) {
        SCOPED_TRACE(solver);
        Model m(std::string("channel-ground-owner-")+solver,"SIGMA",.5,false,
            "[DWF]\nJ1 FLOW .01\n[TRANSPORT_OPTIONS]\nTARGET_DX 2\n",true);
        ASSERT_NE(m.e,nullptr);
        ASSERT_EQ(swmm_options_set(m.e,"QUALITY_SOLVER",solver),SWMM_OK);
        ASSERT_EQ(swmm_heat_set_sediment(m.e,SWMM_HEAT_SED_GROUND_SOURCE,1),SWMM_OK);
        ASSERT_EQ(swmm_heat_set_sediment(m.e,SWMM_HEAT_SED_HYPORHEIC_VELOCITY,2e-5),SWMM_OK);
        ASSERT_EQ(swmm_heat_set_module(m.e,SWMM_HEAT_SEDIMENT_EXCHANGE,1),SWMM_OK);
        ASSERT_EQ(swmm_options_set_ext(m.e,"HEAT_MODEL","COUPLED"),SWMM_OK);
        ASSERT_EQ(swmm_options_set_ext(m.e,"BED_HEAT_MODEL","GROUND_COLUMN"),SWMM_OK);
        ASSERT_EQ(swmm_options_set_ext(m.e,"HEAT_SURFACE_EXCHANGE","NO"),SWMM_OK);
        ASSERT_EQ(swmm_options_set_ext(m.e,"HEAT_RADIATIVE_EXCHANGE","NO"),SWMM_OK);
        ASSERT_EQ(swmm_gw_transport_option_set(m.e,"CONDUCTION","NO"),SWMM_OK);
        ASSERT_EQ(m.start(),SWMM_OK);

        auto& ctx=static_cast<openswmm::SWMMEngine*>(m.e)->context();
        auto& router=m.router();auto& gw=router.subsurface();auto& contact=router.channelGround();
        ASSERT_TRUE(contact.active());ASSERT_EQ(ctx.heat_state.mesh_contact_link.size(),1);
        ASSERT_TRUE(ctx.heat_state.mesh_contact_link[0]);
        double actual=0;
        ASSERT_EQ(swmm_coupling_advance_to(m.e,5.,&actual),SWMM_OK)<<swmm_get_last_error_msg(m.e);

        // Give the superseded prescribed bed store a deliberately different
        // temperature. A mapped mesh-ground contact must spend the aquifer
        // slice only; LEGACY/LARD link beds and ARD cell beds stay dormant.
        ASSERT_TRUE(!ctx.bed_state.link_temp.empty()||!ctx.bed_state.cell_temp.empty());
        std::fill(ctx.bed_state.link_temp.begin(),ctx.bed_state.link_temp.end(),-40.);
        std::fill(ctx.bed_state.cell_temp.begin(),ctx.bed_state.cell_temp.end(),-40.);
        const auto prescribed_link_bed=ctx.bed_state.link_temp;
        const auto prescribed_cell_bed=ctx.bed_state.cell_temp;
        auto regions=ctx.state_accessors.get_channel_thermal_regions();ASSERT_FALSE(regions.empty());
        std::vector<openswmm::ChannelThermalIncrement> warm;warm.reserve(regions.size());
        for(const auto& region:regions)warm.push_back({region.link,region.region,35-region.temperature});
        ASSERT_TRUE(ctx.state_accessors.apply_channel_thermal_increments(warm).empty());

        struct SoilBudget {double energy=0,advective=0,other=0,channel=0;};
        const auto budget=[&]() {
            SoilBudget result;
            for(const auto& cell:gw.heat().cells) {
                result.energy+=sum(cell.column.energy);result.advective+=cell.advective_j;
                result.channel+=cell.channel_contact_j;
                result.other+=cell.source_j+cell.surface_j+cell.deep_j+cell.latent_j+
                              cell.lateral_j+cell.water_contact_j;
            }
            return result;
        };
        const auto soil_before=budget();
        const double contact_before=sum(contact.channelToGround());
        ASSERT_EQ(swmm_coupling_advance_to(m.e,10.,&actual),SWMM_OK)<<swmm_get_last_error_msg(m.e);
        EXPECT_EQ(ctx.bed_state.link_temp,prescribed_link_bed);
        EXPECT_EQ(ctx.bed_state.cell_temp,prescribed_cell_bed);
        const double soil_contact=sum(contact.channelToGround())-contact_before;
        const auto soil_after=budget();
        const double d_energy=soil_after.energy-soil_before.energy;
        const double d_receipts=(soil_after.advective-soil_before.advective)+
            (soil_after.other-soil_before.other)+(soil_after.channel-soil_before.channel);
        const double budget_scale=std::max(1.,std::abs(soil_after.advective-soil_before.advective)+
            std::abs(soil_after.other-soil_before.other)+std::abs(soil_after.channel-soil_before.channel));
        EXPECT_GT(soil_contact,0);
        EXPECT_NEAR(soil_after.channel-soil_before.channel,soil_contact,1e-6*budget_scale);
        EXPECT_LE(std::abs(d_energy-d_receipts),1e-6*budget_scale);
        for(const auto& cell:gw.heat().cells)ASSERT_EQ(cell.failed,0)<<cell.error;

        // The explicit PRESCRIBED mode remains the old finite bed model on a
        // channel without a mesh contact; the ownership switch is per link.
        Model prescribed(std::string("channel-prescribed-owner-")+solver,"SIGMA",.5,false,
            "[DWF]\nJ1 FLOW .01\n[TRANSPORT_OPTIONS]\nTARGET_DX 2\n");
        ASSERT_NE(prescribed.e,nullptr);
        ASSERT_EQ(swmm_options_set(prescribed.e,"QUALITY_SOLVER",solver),SWMM_OK);
        ASSERT_EQ(swmm_heat_set_sediment(prescribed.e,SWMM_HEAT_SED_GROUND_SOURCE,0),SWMM_OK);
        ASSERT_EQ(swmm_heat_set_module(prescribed.e,SWMM_HEAT_SEDIMENT_EXCHANGE,1),SWMM_OK);
        ASSERT_EQ(swmm_options_set_ext(prescribed.e,"HEAT_MODEL","COUPLED"),SWMM_OK);
        ASSERT_EQ(swmm_options_set_ext(prescribed.e,"BED_HEAT_MODEL","GROUND_COLUMN"),SWMM_OK);
        ASSERT_EQ(swmm_options_set_ext(prescribed.e,"HEAT_SURFACE_EXCHANGE","NO"),SWMM_OK);
        ASSERT_EQ(swmm_options_set_ext(prescribed.e,"HEAT_RADIATIVE_EXCHANGE","NO"),SWMM_OK);
        ASSERT_EQ(swmm_gw_transport_option_set(prescribed.e,"CONDUCTION","NO"),SWMM_OK);
        ASSERT_EQ(prescribed.start(),SWMM_OK);
        ASSERT_FALSE(prescribed.router().channelGround().active());
        auto& prescribed_ctx=static_cast<openswmm::SWMMEngine*>(prescribed.e)->context();
        ASSERT_EQ(swmm_coupling_advance_to(prescribed.e,5.,&actual),SWMM_OK);
        ASSERT_TRUE(!prescribed_ctx.bed_state.link_temp.empty()||!prescribed_ctx.bed_state.cell_temp.empty());
        std::fill(prescribed_ctx.bed_state.link_temp.begin(),prescribed_ctx.bed_state.link_temp.end(),-40.);
        std::fill(prescribed_ctx.bed_state.cell_temp.begin(),prescribed_ctx.bed_state.cell_temp.end(),-40.);
        const auto prescribed_link_before=prescribed_ctx.bed_state.link_temp;
        const auto prescribed_cell_before=prescribed_ctx.bed_state.cell_temp;
        auto prescribed_regions=prescribed_ctx.state_accessors.get_channel_thermal_regions();
        ASSERT_FALSE(prescribed_regions.empty());
        std::vector<openswmm::ChannelThermalIncrement> prescribed_warm;prescribed_warm.reserve(prescribed_regions.size());
        for(const auto& region:prescribed_regions)
            prescribed_warm.push_back({region.link,region.region,35-region.temperature});
        ASSERT_TRUE(prescribed_ctx.state_accessors.apply_channel_thermal_increments(prescribed_warm).empty());
        ASSERT_EQ(swmm_coupling_advance_to(prescribed.e,10.,&actual),SWMM_OK);
        EXPECT_TRUE(prescribed_ctx.bed_state.link_temp!=prescribed_link_before||
                    prescribed_ctx.bed_state.cell_temp!=prescribed_cell_before);
    }
}
TEST(GroundHeat, RoutedMeshContactRefusesPartialChannelCoverageInsteadOfFallingBackSilently) {
    Model m("channel-ground-partial-coverage", "SIGMA", .5, false,
        "[DWF]\nJ1 FLOW .01\n[TRANSPORT_OPTIONS]\nTARGET_DX 2\n", true);
    ASSERT_NE(m.e, nullptr);

    auto& ctx=static_cast<openswmm::SWMMEngine*>(m.e)->context();
    const int outlet=ctx.node_names.find("O1");
    ASSERT_GE(outlet,0);
    // The mesh spans x=[0,10], while this open reach now ends at x=12.
    // Partial mesh ownership has no qualified mixed prescribed/mesh fallback.
    ctx.spatial.node_x[outlet]=12.;

    ASSERT_EQ(swmm_heat_set_sediment(m.e,SWMM_HEAT_SED_GROUND_SOURCE,1),SWMM_OK);
    ASSERT_EQ(swmm_heat_set_module(m.e,SWMM_HEAT_SEDIMENT_EXCHANGE,1),SWMM_OK);
    ASSERT_EQ(swmm_options_set_ext(m.e,"HEAT_MODEL","COUPLED"),SWMM_OK);
    ASSERT_EQ(swmm_options_set_ext(m.e,"BED_HEAT_MODEL","GROUND_COLUMN"),SWMM_OK);
    ASSERT_EQ(swmm_options_set_ext(m.e,"HEAT_SURFACE_EXCHANGE","NO"),SWMM_OK);
    ASSERT_EQ(swmm_options_set_ext(m.e,"HEAT_RADIATIVE_EXCHANGE","NO"),SWMM_OK);
    ASSERT_EQ(swmm_gw_transport_option_set(m.e,"CONDUCTION","NO"),SWMM_OK);

    EXPECT_NE(swmm_engine_initialize(m.e),SWMM_OK);
    const char* error=swmm_get_last_error_msg(m.e);
    ASSERT_NE(error,nullptr);
    EXPECT_NE(std::string(error).find("outside the mesh"),std::string::npos)<<error;
    EXPECT_FALSE(m.router().channelGround().active());
    EXPECT_TRUE(ctx.heat_state.mesh_contact_link.empty());
}
TEST(GroundHeat, RoutedChannelPollutantAndAgeExchangeWithSharedPorewaterConservatively) {
    for(const auto* solver:{"LEGACY","EULERIAN_ARD","LAGRANGIAN"}) {
        SCOPED_TRACE(solver);
        Model m(std::string("channel-porewater-")+solver,"SIGMA",.5,false,
            porewater_species+"[DWF]\nJ1 FLOW .01\n[TRANSPORT_OPTIONS]\nTARGET_DX 2\n",true);
        ASSERT_NE(m.e,nullptr);ASSERT_EQ(swmm_options_set(m.e,"QUALITY_SOLVER",solver),SWMM_OK);
        ASSERT_EQ(swmm_heat_set_sediment(m.e,SWMM_HEAT_SED_GROUND_SOURCE,1),SWMM_OK);
        ASSERT_EQ(swmm_heat_set_sediment(m.e,SWMM_HEAT_SED_HYPORHEIC_VELOCITY,2e-5),SWMM_OK);
        ASSERT_EQ(swmm_heat_set_module(m.e,SWMM_HEAT_SEDIMENT_EXCHANGE,1),SWMM_OK);
        ASSERT_EQ(swmm_options_set_ext(m.e,"HEAT_MODEL","COUPLED"),SWMM_OK);
        ASSERT_EQ(swmm_options_set_ext(m.e,"BED_HEAT_MODEL","GROUND_COLUMN"),SWMM_OK);
        ASSERT_EQ(swmm_options_set_ext(m.e,"HEAT_SURFACE_EXCHANGE","NO"),SWMM_OK);
        ASSERT_EQ(swmm_options_set_ext(m.e,"HEAT_RADIATIVE_EXCHANGE","NO"),SWMM_OK);
        ASSERT_EQ(swmm_gw_transport_option_set(m.e,"CONDUCTION","YES"),SWMM_OK);
        ASSERT_EQ(m.start(),SWMM_OK);
        auto& ctx=static_cast<openswmm::SWMMEngine*>(m.e)->context();
        auto& gw=m.router().subsurface();auto& tr=gw.transport();
        const int dye=tr.rowIndex("Dye"),age=tr.age_row;ASSERT_GE(dye,0);ASSERT_GE(age,0);
        double actual=0;ASSERT_EQ(swmm_coupling_advance_to(m.e,5.,&actual),SWMM_OK)<<swmm_get_last_error_msg(m.e);
        auto species=ctx.state_accessors.get_channel_species_regions();ASSERT_FALSE(species.empty());
        std::vector<openswmm::ChannelSpeciesIncrement> seed;seed.reserve(species.size());
        for(const auto& region:species) {
            ASSERT_EQ(region.concentration.size(),std::size_t(tr.n_species));
            openswmm::ChannelSpeciesIncrement update;update.link=region.link;update.region=region.region;
            update.concentration_delta.assign(tr.n_species,0);
            update.concentration_delta[dye]=100-region.concentration[dye];
            update.concentration_delta[age]=120-region.concentration[age];seed.push_back(std::move(update));
        }
        ASSERT_TRUE(ctx.state_accessors.apply_channel_species_increments(seed).empty());
        species=ctx.state_accessors.get_channel_species_regions();
        const auto channelMass=[&](int row){double result=0;for(const auto& r:species)result+=r.volume*r.concentration[row];return result;};
        const auto groundReceipt=[&](int row) {
            double result=0;
            for(const auto& cell:gw.heat().cells)if(cell.porewater.active()) {
                const auto nl=cell.column.theta.size();
                for(std::size_t j=0;j<nl;++j)
                    result+=cell.porewater.contact_mass[std::size_t(row)*nl+j];
            }
            return result;
        };
        const auto channelReceipt=[&](int row) {
            const auto& values=m.router().channelGround().channelSpeciesToGround();
            double result=0;
            for(int link=0;link<ctx.n_links();++link)
                result+=values[std::size_t(row)*ctx.n_links()+link];
            return result;
        };
        const double dye_channel_initial=channelMass(dye),dye_initial=poreMass(gw.heat(),dye)+dye_channel_initial;
        const double age_initial=poreMass(gw.heat(),age)+channelMass(age);
        const double dye_ground_receipt=groundReceipt(dye),age_ground_receipt=groundReceipt(age);
        const double dye_channel_receipt=channelReceipt(dye),age_channel_receipt=channelReceipt(age);
        const auto water=m.router().state().volume,heads=gw.state().hg;
        std::vector<double> returned;
        const auto error=m.router().channelGround().advance(m.router().options(),
            std::vector<openswmm::twoD::SurfaceThermalSample>(m.router().mesh().n_cells()),3600,returned);
        ASSERT_TRUE(error.empty())<<error;
        species=ctx.state_accessors.get_channel_species_regions();
        EXPECT_NEAR(poreMass(gw.heat(),dye)+channelMass(dye),dye_initial,1e-9);
        EXPECT_NEAR(poreMass(gw.heat(),age)+channelMass(age),age_initial,1e-8);
        EXPECT_NE(channelMass(dye),dye_channel_initial);
        EXPECT_NEAR(groundReceipt(dye)-dye_ground_receipt,channelReceipt(dye)-dye_channel_receipt,1e-9);
        EXPECT_NEAR(groundReceipt(age)-age_ground_receipt,channelReceipt(age)-age_channel_receipt,1e-8);
        EXPECT_EQ(m.router().state().volume,water);
        EXPECT_EQ(gw.state().hg,heads);

        const auto thermal_before=ctx.state_accessors.get_channel_thermal_regions();
        std::vector<std::vector<double>> ground_energy_before,porewater_mass_before;
        for(const auto& cell:gw.heat().cells) {
            ground_energy_before.push_back(cell.column.energy);
            porewater_mass_before.push_back(cell.porewater.mass);
        }
        const auto heat_receipt_before=m.router().channelGround().channelToGround();
        const auto species_receipt_before=m.router().channelGround().channelSpeciesToGround();
        const auto channel_species_before=ctx.state_accessors.get_channel_species_regions();
        auto apply_species=ctx.state_accessors.apply_channel_species_increments;
        ctx.state_accessors.apply_channel_species_increments=[](const auto&) {
            return std::string("injected channel species refusal.");
        };
        std::vector<double> rejected_surface_mass;
        const auto rejected=m.router().channelGround().advance(m.router().options(),
            std::vector<openswmm::twoD::SurfaceThermalSample>(m.router().mesh().n_cells()),3600,rejected_surface_mass);
        ctx.state_accessors.apply_channel_species_increments=std::move(apply_species);
        EXPECT_EQ(rejected,"injected channel species refusal.");
        const auto thermal_after=ctx.state_accessors.get_channel_thermal_regions();
        ASSERT_EQ(thermal_after.size(),thermal_before.size());
        for(std::size_t i=0;i<thermal_before.size();++i)
            EXPECT_NEAR(thermal_after[i].temperature,thermal_before[i].temperature,1e-12);
        for(std::size_t i=0;i<gw.heat().cells.size();++i) {
            EXPECT_EQ(gw.heat().cells[i].column.energy,ground_energy_before[i]);
            EXPECT_EQ(gw.heat().cells[i].porewater.mass,porewater_mass_before[i]);
        }
        EXPECT_EQ(m.router().channelGround().channelToGround(),heat_receipt_before);
        EXPECT_EQ(m.router().channelGround().channelSpeciesToGround(),species_receipt_before);
        EXPECT_EQ(ctx.state_accessors.get_channel_species_regions().size(),channel_species_before.size());
        const auto channel_species_after=ctx.state_accessors.get_channel_species_regions();
        for(std::size_t i=0;i<channel_species_before.size();++i)
            EXPECT_EQ(channel_species_after[i].concentration,channel_species_before[i].concentration);

        Model restored(std::string("channel-porewater-restore-")+solver,"SIGMA",.5,false,
            porewater_species+"[DWF]\nJ1 FLOW .01\n",true);
        ASSERT_NE(restored.e,nullptr);
        ASSERT_EQ(swmm_options_set(restored.e,"QUALITY_SOLVER",solver),SWMM_OK);
        ASSERT_EQ(swmm_heat_set_sediment(restored.e,SWMM_HEAT_SED_GROUND_SOURCE,1),SWMM_OK);
        ASSERT_EQ(swmm_heat_set_sediment(restored.e,SWMM_HEAT_SED_HYPORHEIC_VELOCITY,2e-5),SWMM_OK);
        ASSERT_EQ(swmm_heat_set_module(restored.e,SWMM_HEAT_SEDIMENT_EXCHANGE,1),SWMM_OK);
        ASSERT_EQ(swmm_options_set_ext(restored.e,"HEAT_MODEL","COUPLED"),SWMM_OK);
        ASSERT_EQ(swmm_options_set_ext(restored.e,"BED_HEAT_MODEL","GROUND_COLUMN"),SWMM_OK);
        ASSERT_EQ(swmm_options_set_ext(restored.e,"HEAT_SURFACE_EXCHANGE","NO"),SWMM_OK);
        ASSERT_EQ(swmm_options_set_ext(restored.e,"HEAT_RADIATIVE_EXCHANGE","NO"),SWMM_OK);
        ASSERT_EQ(swmm_gw_transport_option_set(restored.e,"CONDUCTION","YES"),SWMM_OK);
        ASSERT_EQ(restored.start(),SWMM_OK);
        openswmm::detail::CompletedSourceArchive channel_state;
        m.router().channelGround().checkpoint(channel_state);
        openswmm::detail::CompletedSourceArchive restored_state(channel_state.bytes());
        restored.router().channelGround().checkpoint(restored_state);
        restored_state.finish();
        EXPECT_EQ(m.router().channelGround().channelSpeciesToGround(),
                 restored.router().channelGround().channelSpeciesToGround());
    }
}
TEST(GroundHeat, ChannelContactsCanRunWithoutAnOverlandTemperatureRow) {
    Model m("channel-soil-only","CLOSED_FORM",.5,false,"[DWF]\nJ1 FLOW .01\n",true);ASSERT_NE(m.e,nullptr);
    ASSERT_EQ(swmm_options_set_ext(m.e,"TRANSPORT_TEMPERATURE","NO"),SWMM_OK);
    ASSERT_EQ(swmm_heat_set_sediment(m.e,SWMM_HEAT_SED_GROUND_SOURCE,1),SWMM_OK);
    ASSERT_EQ(swmm_heat_set_module(m.e,SWMM_HEAT_SEDIMENT_EXCHANGE,1),SWMM_OK);
    ASSERT_EQ(swmm_gw_transport_option_set(m.e,"CONDUCTION","YES"),SWMM_OK);
    ASSERT_EQ(m.start(),SWMM_OK);EXPECT_LT(m.router().state().transport.temp_row,0);
    const double initial=energy(m.router().subsurface().heat());double actual=0;
    ASSERT_EQ(swmm_coupling_advance_to(m.e,5.,&actual),SWMM_OK);
    const auto& contacts=m.router().channelGround();EXPECT_GT(sum(contacts.channelToGround()),0);
    EXPECT_NEAR(energy(m.router().subsurface().heat())-initial,sum(contacts.channelToGround()),1e-5);
    EXPECT_FALSE(m.router().state().heat.enabled);EXPECT_TRUE(m.router().state().heat.bed_energy.empty());
}
TEST(GroundHeat, MappedChannelBedMustFitInsideItsActualAquiferProfile) {
    Model m("channel-bed-depth","CLOSED_FORM",.5,false,"",true);ASSERT_NE(m.e,nullptr);
    ASSERT_EQ(swmm_heat_set_sediment(m.e,SWMM_HEAT_SED_GROUND_SOURCE,1),SWMM_OK);
    ASSERT_EQ(swmm_heat_set_sediment(m.e,SWMM_HEAT_SED_BED_THICKNESS,2),SWMM_OK);
    ASSERT_EQ(swmm_heat_set_module(m.e,SWMM_HEAT_SEDIMENT_EXCHANGE,1),SWMM_OK);
    EXPECT_NE(m.start(),SWMM_OK);
    EXPECT_FALSE(m.router().channelGround().active());
}
TEST(GroundHeat, ChannelContactResultsReportMeasuredTopologyAndReadCanonicalDryBedTemperatures) {
    for(const auto* solver:{"LEGACY","EULERIAN_ARD","LAGRANGIAN"}) {
        SCOPED_TRACE(solver);
        Model m(std::string("channel-results-")+solver,"CLOSED_FORM",.5,false,
            "[JUNCTIONS]\nJ2 -2 4 0 0 0\n[OUTFALLS]\nO2 -2 FREE NO\n"
            "[CONDUITS]\nC2 J2 O2 10 .013 0 0 0\n[XSECTIONS]\nC2 RECT_OPEN 1 2 0 0 1\n"
            "[COORDINATES]\nJ2 40 10\nO2 50 10\n",true);
        ASSERT_NE(m.e,nullptr);ASSERT_EQ(swmm_options_set(m.e,"QUALITY_SOLVER",solver),SWMM_OK);
        ASSERT_EQ(swmm_heat_set_element_override(m.e,SWMM_HEAT_ATTR_GROUND_SOURCE,SWMM_HEAT_SCOPE_LINK,"C1",1),SWMM_OK);
        ASSERT_EQ(swmm_heat_set_sediment(m.e,SWMM_HEAT_SED_BED_THICKNESS,.25),SWMM_OK);
        ASSERT_EQ(swmm_heat_set_module(m.e,SWMM_HEAT_SEDIMENT_EXCHANGE,1),SWMM_OK);
        SWMM_ChannelGroundStatus status{};status.struct_size=sizeof status;
        ASSERT_EQ(swmm_heat_get_channel_ground_status(m.e,&status),SWMM_OK);
        EXPECT_TRUE(status.requested);EXPECT_FALSE(status.initialized);EXPECT_EQ(status.contact_intervals,0);
        double values[2]={123,456};
        EXPECT_EQ(swmm_heat_get_channel_ground_values(m.e,SWMM_CHANNEL_GROUND_BED_TEMPERATURE,0,2,values),SWMM_ERR_LIFECYCLE);
        EXPECT_DOUBLE_EQ(values[0],123);EXPECT_DOUBLE_EQ(values[1],456);
        ASSERT_EQ(m.start(),SWMM_OK);ASSERT_EQ(swmm_heat_get_channel_ground_status(m.e,&status),SWMM_OK);
        EXPECT_TRUE(status.initialized);EXPECT_TRUE(status.exchange_active);EXPECT_EQ(status.mapped_links,1);EXPECT_EQ(status.contact_cells,2);
        ASSERT_GT(status.contact_intervals,0);double chainage=0;
        for(int index=0;index<status.contact_intervals;++index) {
            SWMM_ChannelGroundContact contact{};contact.struct_size=sizeof contact;
            ASSERT_EQ(swmm_heat_get_channel_ground_contact(m.e,index,&contact),SWMM_OK);
            EXPECT_EQ(contact.link,0);EXPECT_GE(contact.cell,0);EXPECT_LT(contact.cell,2);
            EXPECT_GE(contact.layer,0);EXPECT_LT(contact.layer,16);EXPECT_NEAR(contact.invert_m,-.2,1e-12);
            chainage+=(contact.end_fraction-contact.begin_fraction)*contact.share;
        }
        EXPECT_NEAR(chainage,1,1e-12);auto& heat=m.router().subsurface().heat();
        for(int i=0;i<2;++i)for(std::size_t j=0;j<heat.cells[i].column.energy.size();++j)
            heat.cells[i].column.energy[j]=heat.cells[i].column.capacity[j]*(i==0?10:-10);
        const double stored=energy(heat);
        ASSERT_EQ(swmm_heat_get_channel_ground_values(m.e,SWMM_CHANNEL_GROUND_BED_TEMPERATURE,0,2,values),SWMM_OK);
        EXPECT_NEAR(values[0],5,1e-12);EXPECT_TRUE(std::isnan(values[1]));EXPECT_DOUBLE_EQ(energy(heat),stored);
        ASSERT_EQ(swmm_heat_get_channel_ground_values(m.e,SWMM_CHANNEL_GROUND_INTERVAL_COUNT,0,1,values),SWMM_OK);
        EXPECT_DOUBLE_EQ(values[0],status.contact_intervals);
        values[0]=123;values[1]=456;
        EXPECT_EQ(swmm_heat_get_channel_ground_values(m.e,SWMM_CHANNEL_GROUND_BED_TEMPERATURE,1,2,values),SWMM_ERR_BADINDEX);
        EXPECT_DOUBLE_EQ(values[0],123);EXPECT_DOUBLE_EQ(values[1],456);
        SWMM_ChannelGroundContact invalid{};invalid.struct_size=sizeof invalid;invalid.cell=123;
        EXPECT_EQ(swmm_heat_get_channel_ground_contact(m.e,status.contact_intervals,&invalid),SWMM_ERR_BADINDEX);
        EXPECT_EQ(invalid.cell,123);
        ASSERT_EQ(swmm_heat_set_module(m.e,SWMM_HEAT_SEDIMENT_EXCHANGE,0),SWMM_OK);
        ASSERT_EQ(swmm_heat_get_channel_ground_status(m.e,&status),SWMM_OK);
        EXPECT_FALSE(status.exchange_active);EXPECT_EQ(status.mapped_links,1);EXPECT_GT(status.contact_intervals,0);
        ASSERT_EQ(swmm_engine_end(m.e),SWMM_OK);m.running=false;
        ASSERT_EQ(swmm_heat_get_channel_ground_status(m.e,&status),SWMM_OK);EXPECT_FALSE(status.exchange_active);
        EXPECT_EQ(swmm_heat_get_channel_ground_values(m.e,SWMM_CHANNEL_GROUND_BED_TEMPERATURE,0,1,values),SWMM_OK);
        EXPECT_NEAR(values[0],5,1e-12);
    }
}
TEST(GroundHeat, HeterogeneousWellWithdrawalUsesPhysicalLayerDonorsWithoutCreatingNewExtrema) {
    for(const auto* closure:{"CLOSED_FORM","ENSLAVED","SIGMA"})for(const auto temperatures:{std::pair{0.,40.},std::pair{-10.,0.}}) {
        SCOPED_TRACE(closure);const double cold=temperatures.first,warm=temperatures.second;
        const std::string initial="CELL 1 SAT __TEMPERATURE__ "+std::to_string(cold)+
            "\nCELL 1 UNSAT __TEMPERATURE__ "+std::to_string(cold)+
            "\nCELL 1 LAYER 16 __TEMPERATURE__ "+std::to_string(warm)+
            "\nCELL 2 SAT __TEMPERATURE__ "+std::to_string(cold)+
            "\nCELL 2 UNSAT __TEMPERATURE__ "+std::to_string(cold)+"\n";
        Model m(std::string("layer-donor-")+closure+std::to_string(cold),closure,1,false,initial);ASSERT_NE(m.e,nullptr);
        ASSERT_EQ(m.start(),SWMM_OK);auto& gw=m.router().subsurface();const double before=energy(gw.heat());
        const int ns=gw.transport().n_species,row=gw.transport().temp_row;
        std::vector<double> concentrations(ns,0),rates(ns,0);
        ASSERT_EQ(swmm_coupling_set_source(m.e,"well",1,0,-.001,0,concentrations.data(),rates.data(),ns,1.),SWMM_OK);
        double actual=0;ASSERT_EQ(swmm_coupling_advance_to(m.e,1.,&actual),SWMM_OK);
        const auto& cell=gw.heat().cells[0];ASSERT_EQ(cell.failed,0)<<cell.error;
        for(std::size_t j=0;j<cell.column.energy.size();++j) {
            const double t=cell.column.energy[j]/cell.column.capacity[j];
            EXPECT_GE(t,cold-1e-9);EXPECT_LE(t,warm+1e-9);
        }
        const double exported=gw.transport().lost_source[row]*4184000.;
        EXPECT_NEAR(before-energy(gw.heat()),exported,1e-5);
        int c=0;SWMM_CouplingReceipt receipt{};std::vector<double> requested(ns),applied(ns);
        ASSERT_EQ(swmm_coupling_get_receipts(m.e,1,"well",&c,1,&receipt,requested.data(),applied.data(),ns),SWMM_OK);
        EXPECT_NEAR(-applied[row]*4184000.,exported,1e-6);
    }
}
TEST(GroundHeat, ZeroMeanLinkPacketsRetainSignedLayerDonorsAcrossSourceSubintervals) {
    for(const auto* closure:{"CLOSED_FORM","ENSLAVED","SIGMA"}) {
        SCOPED_TRACE(closure);
        std::string initial="* SAT __TEMPERATURE__ -10\n* UNSAT __TEMPERATURE__ -10\n";
        for(int layer=9;layer<=16;++layer)initial+="CELL 1 LAYER "+std::to_string(layer)+" __TEMPERATURE__ 10\n";
        // A zero-flow authored source still divides the cell firing at its
        // forcing knots. Pending layer receipts must follow the same split.
        initial+="[TIMESERIES]\nS 0:00:00 0\nS 0:00:01 0\nS 0:00:02 0\n"
                 "[GW_SOURCES]\nS CELL 1 FLOW S\n";
        Model m(std::string("zero-link-")+closure,closure,2,false,initial);ASSERT_NE(m.e,nullptr);
        ASSERT_EQ(m.start(),SWMM_OK);auto& gw=m.router().subsurface();auto& tr=gw.transport();
        const int row=tr.temp_row;ASSERT_NEAR(tr.sat_mass[tr.idx(row,0)],0,1e-12);
        const double before=energy(gw.heat());constexpr double volume=.01;
        gw.bookLinkSeepage(0,-volume);const double packet=gw.bookLinkGain(0,row,volume);
        EXPECT_NEAR(packet,0,1e-14);gw.assignTiers(2,1);gw.fireGwCells(0,2,m.router().state(),0);
        for(const auto& cell:gw.heat().cells) {
            ASSERT_EQ(cell.failed,0)<<cell.error;
            for(std::size_t j=0;j<cell.column.energy.size();++j) {
                const double t=cell.column.energy[j]/cell.column.capacity[j];
                // Producer-time signed layer receipts are summed in bulk-CV
                // units before being booked back to the fixed thermal grid.
                // Allow a few microdegrees for that conservative roundoff.
                EXPECT_GE(t,-10-1e-5);EXPECT_LE(t,10+1e-5);
            }
        }
        EXPECT_NEAR(energy(gw.heat())-before,-packet*4184000.,1e-5);
        EXPECT_DOUBLE_EQ(tr.lout_mass[tr.idx(row,0)],0);EXPECT_NEAR(tr.lost_link[row],packet,1e-14);
    }
}
TEST(GroundHeat, ZeroMeanLateralPacketsDebitActualLayersAndCloseAcrossTiers) {
    for(const auto* closure:{"CLOSED_FORM","ENSLAVED","SIGMA"}) {
        SCOPED_TRACE(closure);
        std::string initial="* SAT __TEMPERATURE__ -10\n* UNSAT __TEMPERATURE__ -10\n";
        for(int layer=9;layer<=16;++layer)initial+="CELL 1 LAYER "+std::to_string(layer)+" __TEMPERATURE__ 10\n";
        initial+="[2D_AQUIFER]\nCELL 2 36 1 .45 .1 2 HG0 .5\n";
        Model m(std::string("zero-face-")+closure,closure,1,false,initial);ASSERT_NE(m.e,nullptr);
        ASSERT_EQ(m.start(),SWMM_OK);auto& gw=m.router().subsurface();auto& tr=gw.transport();
        ASSERT_NEAR(tr.sat_mass[tr.idx(tr.temp_row,0)],0,1e-12);
        const double before=energy(gw.heat());gw.state().dt_cell[0]=.1;gw.state().dt_cell[1]=.8;gw.assignTiers(.1,4);
        for(int step=0;step<16;++step) {
            gw.fireGwFaces(0,.1);gw.fireGwCells(0,.1,m.router().state(),step*.1);
            if(step%8==7)gw.fireGwCells(3,.8,m.router().state(),(step-7)*.1);
            double pending=0;for(double value:tr.sacc_L)pending+=value;for(double value:tr.sacc_R)pending+=value;
            EXPECT_NEAR(energy(gw.heat())+4184000.*pending,before,2e-5);
            for(const auto& cell:gw.heat().cells) {
                ASSERT_EQ(cell.failed,0)<<cell.error;
                for(std::size_t j=0;j<cell.column.energy.size();++j) {
                    const double t=cell.column.energy[j]/cell.column.capacity[j];
                    EXPECT_GE(t,-10-1e-9);EXPECT_LE(t,10+1e-9);
                }
            }
        }
        EXPECT_NEAR(gw.state().continuityResidual(),0,1e-8);
    }
}
TEST(GroundHeat, NodeFlowReversalKeepsGrossOutgoingHeatWhenAcceptedWaterNetsToZero) {
    Model m("node-gross-heat","CLOSED_FORM",.5,false,
        "* SAT __TEMPERATURE__ 10\n* UNSAT __TEMPERATURE__ 10\n"
        "[2D_AQUIFER_NODE]\nJ1 1 KC 3600 DC .1 AREA 1\n");
    ASSERT_NE(m.e,nullptr);ASSERT_EQ(m.start(),SWMM_OK);auto& gw=m.router().subsurface();
    auto& tr=gw.transport();const int row=tr.temp_row;openswmm::NodeData nodes;nodes.resize(2);
    const double head=gw.state().z_bed[0]+gw.state().hg[0],length=m.router().options().len_1d_to_2d;
    nodes.invert_elev[0]=(head+.25)/length;nodes.depth[0]=0;nodes.volume[0]=100;
    std::vector<double> concentrations(2*tr.n_species,0);concentrations[row]=20;
    gw.setNodeRowConc(concentrations.data(),tr.n_species);gw.assignTiers(1,1);
    const double before=energy(gw.heat());
    gw.sampleNodeExchange(&nodes,.5);const double incoming=-gw.nodeExchangeVolumes()[0];ASSERT_GT(incoming,0);
    nodes.invert_elev[0]=(head-.25)/length;gw.sampleNodeExchange(&nodes,.5);
    ASSERT_NEAR(gw.nodeExchangeVolumes()[0],0,1e-12);
    gw.fireGwCells(0,1,m.router().state(),0);gw.setNodeRowConc(nullptr,0);
    for(const auto& cell:gw.heat().cells)ASSERT_EQ(cell.failed,0)<<cell.error;
    EXPECT_GT(tr.lost_node[row],0);EXPECT_NEAR(tr.gained_node[row],incoming*20,1e-12);
    EXPECT_NEAR(gw.nodeExchangeMass()[std::size_t(row)*2],tr.lost_node[row],1e-12);
    EXPECT_NEAR(energy(gw.heat())-before,4184000.*(tr.gained_node[row]-tr.lost_node[row]),1e-5);
}
