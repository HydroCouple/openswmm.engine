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
namespace {
namespace fs=std::filesystem;
const fs::path out=fs::path(OPENSWMM_GROUND_HEAT_OUT)/"ground_heat";
struct Model {
    SWMM_Engine e=nullptr;bool running=false;
    Model(const std::string& name,const std::string& closure="CLOSED_FORM",double hg=.5,bool partial=false,const std::string& extra="") {
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
