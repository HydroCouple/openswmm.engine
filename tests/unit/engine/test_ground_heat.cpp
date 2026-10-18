// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <cmath>
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_heat.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_gw_transport.h>
#include <openswmm/engine/openswmm_coupling.h>
#include <openswmm/engine/openswmm_hotstart.h>
#include "core/SWMMEngine.hpp"
#include "2d/SurfaceRouter2D.hpp"
#include "2d/subsurface/SubsurfaceSolver.hpp"
#include "2d/heat/ChannelGround.hpp"
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
    const auto& cells=partial.router().subsurface().heat().cells;ASSERT_EQ(cells.size(),2);
    EXPECT_FALSE(cells[0].column.energy.empty());EXPECT_TRUE(cells[1].column.energy.empty());
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
TEST(GroundHeat, RoutedChannelsAndMultipleCellsShareOneFiniteConservativeSoilSolve) {
    const std::string extra="[JUNCTIONS]\nJ2 -.2 4 0 0 0\n[OUTFALLS]\nO2 -.2 FREE NO\n"
        "[CONDUITS]\nC2 J2 O2 8 .013 0 0 0\n[XSECTIONS]\nC2 RECT_OPEN 1 2 0 0 1\n"
        "[COORDINATES]\nJ2 1 7\nO2 9 7\n[DWF]\nJ1 FLOW .01\nJ2 FLOW .01\n[TRANSPORT_OPTIONS]\nTARGET_DX 2\n";
    for(const auto* solver:{"LEGACY","EULERIAN_ARD","LAGRANGIAN"}) {
        Model m(std::string("channel-operator-")+solver,"SIGMA",.5,false,extra,true);ASSERT_NE(m.e,nullptr);
        ASSERT_EQ(swmm_options_set(m.e,"QUALITY_SOLVER",solver),SWMM_OK);
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
        const auto error=contacts.advance(m.router().options(),{},3600,surface);ASSERT_TRUE(error.empty())<<error;
        EXPECT_NEAR(energy(gw.heat())+water_energy(),initial,2e-5);
        EXPECT_EQ(gw.state().hg,hg);EXPECT_EQ(gw.state().hu,hu);EXPECT_EQ(m.router().state().volume,volume);
        EXPECT_GT(sum(contacts.channelToGround()),0);
        EXPECT_NEAR(sum(contacts.channelThermalIncrement())+sum(contacts.channelToGround())-sum(contacts.channelAtmosphere()),0,1e-6);
        double soil_receipt=0;for(const auto& c:gw.heat().cells) {
            soil_receipt+=c.channel_contact_j;for(std::size_t j=0;j<c.column.energy.size();++j) {
                EXPECT_GE(c.column.energy[j]/c.column.capacity[j],-3-1e-10);EXPECT_LE(c.column.energy[j]/c.column.capacity[j],20+1e-10);
            }
        }
        EXPECT_NEAR(soil_receipt,sum(contacts.channelToGround()),1e-6);
        const auto saved=ctx.state_accessors.apply_channel_thermal_increments;
        ctx.state_accessors.apply_channel_thermal_increments=[](const auto&){return std::string("forced binding refusal");};
        const double before_soil=energy(gw.heat()),before_water=water_energy();
        EXPECT_EQ(contacts.advance(m.router().options(),{},60,surface),"forced binding refusal");
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
                EXPECT_GE(t,-10-1e-9);EXPECT_LE(t,10+1e-9);
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
