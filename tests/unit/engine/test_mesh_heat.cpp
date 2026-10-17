// SPDX-License-Identifier: Apache-2.0
// Reviewed 2D thermal plan: domain-scoped authoring and numerical/runtime gates.
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <cmath>
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_heat.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_process_components.h>
#include <openswmm/engine/openswmm_coupling.h>
#include <openswmm/engine/openswmm_gw_transport.h>
#include <openswmm/engine/openswmm_gw2d.h>
#include "core/SWMMEngine.hpp"
#include "2d/SurfaceRouter2D.hpp"
#include "transport/components/HeatFluxModules/HeatOverrides.hpp"
#include "2d/heat/MeshHeat.hpp"
#include "2d/solver/ExplicitInertialSolver.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include <numeric>
namespace {
namespace fs=std::filesystem;
const fs::path out=fs::path(OPENSWMM_MESH_HEAT_OUT)/"mesh_heat";
struct Model {
    SWMM_Engine e=nullptr;
    Model(const std::string& id,const std::string& extra="") {
        fs::create_directories(out);
        // Scratch inputs follow the parity harness convention: an older
        // binary cannot parse fixtures deliberately exercising new syntax.
        const auto file=out/("_"+id+".inp");
        std::ofstream(file) <<
            "[OPTIONS]\nFLOW_UNITS CMS\nFLOW_ROUTING DYNWAVE\n"
            "START_DATE 01/01/2026\nSTART_TIME 00:00:00\nEND_DATE 01/01/2026\nEND_TIME 00:10:00\n"
            "REPORT_STEP 00:01:00\nROUTING_STEP 5\nHEAT_TRANSPORT YES\n\n"
            "[JUNCTIONS]\nJ1 -2 4 0 0 0\n\n[OUTFALLS]\nO1 -2.5 FREE NO\n\n"
            "[CONDUITS]\nC1 J1 O1 30 .013 0 0 0\n\n[XSECTIONS]\nC1 RECT_OPEN 1 2 0 0 1\n\n"
            "[COORDINATES]\nJ1 40 0\nO1 70 0\n\n[TAGS]\nNODE J1 forest\n\n"
            "[2D_OPTIONS]\nREPORT_2D NO\nLTS_TIERS 1\nMAX_TIMESTEP 5\n"
            "EVAPORATION NO\n\n"
            "[2D_VERTICES]\n0 0 0\n10 0 0\n10 10 0\n0 10 0\n\n"
            "[2D_TRIANGLES]\n0 1 2 .03 .1 channel:burned\n0 2 3 .03 .1 forest\n\n"
            << extra;
        e=swmm_engine_create();
        if(swmm_engine_open(e,file.string().c_str(),(out/(id+".rpt")).string().c_str(),
                            (out/(id+".out")).string().c_str(),nullptr)!=SWMM_OK) {
            swmm_engine_destroy(e);e=nullptr;
        }
    }
    ~Model(){if(e){swmm_engine_close(e);swmm_engine_destroy(e);}}
    openswmm::SimulationContext& ctx(){return static_cast<openswmm::SWMMEngine*>(e)->context();}
};
}
TEST(MeshHeat, MeshScopesAreIndependentOfNetworkTagsAndCellWinsOverMeshTag) {
    Model m("scopes");ASSERT_NE(m.e,nullptr);
    // Values 6/7 reserve the effective-value API's existing source codes 4/5.
    constexpr int cell=6,meshTag=7;
    const double global=m.ctx().heat_config.radiative.albedo;
    ASSERT_EQ(swmm_heat_set_element_override(m.e,SWMM_HEAT_ATTR_ALBEDO,meshTag,"forest",.35),SWMM_OK);
    const auto& node=openswmm::transport::heat::radiativeFor(m.ctx(),openswmm::HeatElement::node(0));
    EXPECT_EQ(node.albedo,global);
    EXPECT_EQ(openswmm::transport::heat::radiativeFor(m.ctx(),openswmm::HeatElement::cell2d(0)).albedo,global);
    ASSERT_EQ(swmm_heat_set_element_override(m.e,SWMM_HEAT_ATTR_ALBEDO,cell,"2",.7),SWMM_OK);
    ASSERT_EQ(swmm_heat_set_element_override(m.e,SWMM_HEAT_ATTR_ALBEDO,meshTag,"forest",.2),SWMM_OK);
    EXPECT_EQ(openswmm::transport::heat::radiativeFor(m.ctx(),openswmm::HeatElement::cell2d(1)).albedo,.7);
    double value=0;int source=0;char origin[256]{};
    ASSERT_EQ(swmm_heat_get_effective_element(m.e,SWMM_HEAT_ATTR_ALBEDO,cell,"2",&value,&source,origin,sizeof origin),SWMM_OK);
    EXPECT_EQ(value,.7);EXPECT_EQ(source,cell);EXPECT_STREQ(origin,"2");
    EXPECT_EQ(swmm_heat_set_element_override(m.e,SWMM_HEAT_ATTR_ALBEDO,cell,"0",.5),SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_heat_set_element_override(m.e,SWMM_HEAT_ATTR_ALBEDO,meshTag,"missing",.5),SWMM_ERR_BADPARAM);
}
TEST(MeshHeat, MeshOverridesSurviveNativeSaveAndReopen) {
    Model m("scope-roundtrip");ASSERT_NE(m.e,nullptr);
    ASSERT_EQ(swmm_process_component_register(m.e,"org.hydrocouple.openswmm.heat","mesh-heat.cfg"),SWMM_OK);
    ASSERT_EQ(swmm_heat_set_element_override(m.e,SWMM_HEAT_ATTR_HYPORHEIC_VELOCITY,7,"channel:burned",.00002),SWMM_OK);
    ASSERT_EQ(swmm_heat_set_element_override(m.e,SWMM_HEAT_ATTR_ALBEDO,6,"2",.65),SWMM_OK);
    const auto saved=out/"_scope-roundtrip-native.inp";
    ASSERT_EQ(swmm_model_write(m.e,saved.string().c_str()),SWMM_OK);
    auto e=swmm_engine_create();
    ASSERT_EQ(swmm_engine_open(e,saved.string().c_str(),(out/"scope-reopen.rpt").string().c_str(),nullptr,nullptr),SWMM_OK);
    double value=0;int source=0;char origin[256]{};
    EXPECT_EQ(swmm_heat_get_effective_element(e,SWMM_HEAT_ATTR_ALBEDO,6,"2",&value,&source,origin,sizeof origin),SWMM_OK);
    EXPECT_EQ(value,.65);
    EXPECT_EQ(swmm_heat_get_effective_element(e,SWMM_HEAT_ATTR_HYPORHEIC_VELOCITY,6,"1",&value,&source,origin,sizeof origin),SWMM_OK);
    EXPECT_EQ(value,.00002);EXPECT_EQ(source,7);EXPECT_STREQ(origin,"channel:burned");
    swmm_engine_close(e);swmm_engine_destroy(e);
}

namespace {
struct LocalHeat {
    Model model;
    openswmm::twoD::SurfaceStateData state;
    openswmm::twoD::SolverOptions2D options;
    LocalHeat(const std::string& id):model(id) {
        if(model.e)openswmm::twoD::buildMeshTopology(*model.ctx().twod_io.mesh);
        options.coupled_heat=true;options.bed_heat_model=1;
        options.heat_surface=0;options.heat_radiative=0;
        options.cell_closure=openswmm::twoD::CellClosure2D::FLAT;
        state.resize(2,4);state.transport.temp_row=0;
    }
    const openswmm::twoD::MeshData& mesh(){return *model.ctx().twod_io.mesh;}
    std::string initialize(){return openswmm::twoD::initializeMeshHeat(model.ctx(),mesh(),state,options);}
    double step(int cell,double mass,double volume,double evaporation,double dt){
        return openswmm::twoD::applyMeshHeatCell(mesh(),options,state.heat,cell,mass,volume,evaporation,dt);
    }
};
}
TEST(MeshHeat, ParentsOffKeepAllNewStateEmptyAndRetainChoices) {
    LocalHeat f("parents");ASSERT_NE(f.model.e,nullptr);
    f.options.coupled_heat=false;
    ASSERT_TRUE(f.initialize().empty());EXPECT_FALSE(f.state.heat.enabled);
    EXPECT_TRUE(f.state.heat.bed_energy.empty());EXPECT_TRUE(f.state.heat.atmospheric_j.empty());
    EXPECT_EQ(f.step(0,-50,10,0,60),-50);
    f.options.coupled_heat=true;f.model.ctx().options.heat_transport=false;
    ASSERT_TRUE(f.initialize().empty());EXPECT_FALSE(f.state.heat.enabled);
    EXPECT_EQ(f.options.bed_heat_model,1);
    f.model.ctx().options.heat_transport=true;f.state.transport.temp_row=-1;
    ASSERT_TRUE(f.initialize().empty());EXPECT_FALSE(f.state.heat.enabled);
}
TEST(MeshHeat, NewLocalCapacityContainsSolidAndPorewaterAndSignedInitialEnergy) {
    LocalHeat f("capacity");ASSERT_NE(f.model.e,nullptr);
    auto& cfg=f.model.ctx().heat_config.sediment;
    cfg.mesh_porosity=.4;cfg.sed_density=2600;cfg.sed_specific_heat=800;cfg.bed_thickness=.3;
    cfg.has_initial_temp=true;cfg.initial_temp=-3;
    ASSERT_TRUE(f.initialize().empty());
    // 50 m2 x .3 m x [.6*2600*800 + .4*1000*4184].
    const double expected=50*.3*(.6*2600*800+.4*4184000.);
    EXPECT_DOUBLE_EQ(f.state.heat.bed_capacity[0],expected);
    EXPECT_DOUBLE_EQ(f.state.heat.bed_energy[0],-3*expected);
}
TEST(MeshHeat, IsolatedPairMatchesIndependentAnalyticDecayFromOneSecondToTwoHours) {
    for(const double dt:{1.,60.,900.,7200.}) {
        LocalHeat f("pair"+std::to_string(int(dt)));ASSERT_NE(f.model.e,nullptr);
        auto& cfg=f.model.ctx().heat_config.sediment;
        cfg.has_initial_temp=true;cfg.initial_temp=-4;cfg.ground_depth=0;cfg.hyporheic_velocity=2e-5;
        ASSERT_TRUE(f.initialize().empty());
        const double cw=4184000.*5,cb=f.state.heat.bed_capacity[0],e0=cw*22+cb*(-4);
        const double g=cfg.thermal_diffusivity*cfg.sed_density*cfg.sed_specific_heat*50/cfg.bed_thickness+
                       4184000.*cfg.hyporheic_velocity*50;
        const double equilibrium=e0/(cw+cb),difference=26*std::exp(-g*(1/cw+1/cb)*dt);
        const double expected=equilibrium+cb/(cw+cb)*difference;
        const double mass=f.step(0,22*5,5,0,dt);
        EXPECT_NEAR(mass/5,expected,1e-11);
        const double ef=4184000.*mass+f.state.heat.bed_energy[0];
        EXPECT_NEAR(ef,e0,1e-12*(std::abs(cw*22)+std::abs(cb*(-4))));
        EXPECT_NEAR(f.state.heat.water_j[0]+f.state.heat.water_bed_j[0],0,1e-5);
        EXPECT_NEAR(f.state.heat.bed_energy[0]-f.state.heat.initial_bed_energy[0],f.state.heat.water_bed_j[0],1e-5);
    }
}
TEST(MeshHeat, SheetCellsConductionOnlyUntilExplicitScopeOptInAndZeroStillDisables) {
    for(const double velocity:{0.,1e-12,1e-5,1.}) {
        LocalHeat f("eligibility"+std::to_string(velocity));ASSERT_NE(f.model.e,nullptr);
        auto& cfg=f.model.ctx().heat_config.sediment;
        cfg.has_initial_temp=true;cfg.initial_temp=5;cfg.ground_depth=0;cfg.hyporheic_velocity=velocity;
        ASSERT_TRUE(f.initialize().empty());
        const double burned=f.step(0,100,5,0,60),sheet=f.step(1,100,5,0,60);
        if(velocity>0)EXPECT_LE(burned,sheet);else EXPECT_DOUBLE_EQ(burned,sheet);
        ASSERT_EQ(swmm_heat_set_element_override(f.model.e,SWMM_HEAT_ATTR_HYPORHEIC_VELOCITY,7,"forest",velocity),SWMM_OK);
        ASSERT_TRUE(f.initialize().empty());
        EXPECT_DOUBLE_EQ(f.step(0,100,5,0,60),f.step(1,100,5,0,60));
    }
}
TEST(MeshHeat, DryBedConductsAndRetainsEnergyWhenRewetted) {
    LocalHeat f("dry-rewet");ASSERT_NE(f.model.e,nullptr);
    auto& cfg=f.model.ctx().heat_config.sediment;
    cfg.has_initial_temp=true;cfg.initial_temp=-2;cfg.ground_temp=10;cfg.has_ground_temp=true;
    ASSERT_TRUE(f.initialize().empty());
    const double c=f.state.heat.bed_capacity[0],g=cfg.thermal_diffusivity*cfg.sed_density*cfg.sed_specific_heat*50/cfg.ground_depth;
    EXPECT_EQ(f.step(0,0,0,0,3600),0);
    const double expected=10-12*std::exp(-g*3600/c);
    EXPECT_NEAR(f.state.heat.bed_energy[0]/c,expected,1e-12);
    const double before=f.state.heat.bed_energy[0];
    EXPECT_GT(f.step(0,-25,5,0,60),-25);
    EXPECT_NE(f.state.heat.bed_energy[0],before);
    EXPECT_NEAR(f.state.heat.bed_energy[0]-f.state.heat.initial_bed_energy[0]+f.state.heat.water_j[0],
                f.state.heat.ground_j[0],1e-5);
}
TEST(MeshHeat, ConstantRadiationUsesWettedAreaAndIntegratedJoules) {
    LocalHeat f("radiation");ASSERT_NE(f.model.e,nullptr);
    auto& cfg=f.model.ctx().heat_config.radiative;
    f.options.bed_heat_model=0;f.options.heat_radiative=1;
    cfg.albedo=.25;cfg.shade_factor=.2;cfg.emiss_water=0;cfg.sky_view=0;cfg.emiss_landcover=0;
    cfg.shortwave_wm2=500;
    ASSERT_TRUE(f.initialize().empty());
    openswmm::twoD::prepareMeshHeat(f.model.ctx(),f.state,60);
    const double final=f.step(0,-10,5,0,60),energy=500*.75*.8*50*60;
    EXPECT_NEAR((final+10)*4184000.,energy,1e-6);
    EXPECT_NEAR(f.state.heat.atmospheric_j[0],energy,1e-6);
}
TEST(MeshHeat, LatentUsesAcceptedEvaporationIncludingCompleteDrying) {
    LocalHeat f("latent");ASSERT_NE(f.model.e,nullptr);
    f.options.bed_heat_model=0;f.options.heat_surface=1;
    // Eliminate sensible transfer to isolate the hydrology's latent debit.
    f.model.ctx().options.wind_func_coeff_a=0;f.model.ctx().options.wind_func_coeff_b=0;
    ASSERT_TRUE(f.initialize().empty());
    const double mass=f.step(0,20*5,5,5,60);
    const double expected=-1000*5*(2499e3-2360*20);
    EXPECT_NEAR(f.state.heat.latent_j[0],expected,1e-5);
    EXPECT_NEAR((mass-100)*4184000.,expected,1e-5);
    EXPECT_DOUBLE_EQ(f.state.heat.evaporated_m3[0],5);
    EXPECT_EQ(f.state.heat.failures[0],0);
}
TEST(MeshHeat, OptInValuesAreAuthoredAndOldModelsDoNotGrowNewOptions) {
    Model m("options");ASSERT_NE(m.e,nullptr);
    const auto old=out/"_options-old.inp";
    ASSERT_EQ(swmm_model_write(m.e,old.string().c_str()),SWMM_OK);
    std::ifstream in(old);const std::string text((std::istreambuf_iterator<char>(in)),{});
    EXPECT_EQ(text.find("HEAT_MODEL"),std::string::npos);
    for(const auto* key:{"HEAT_SURFACE_EXCHANGE","HEAT_RADIATIVE_EXCHANGE"})
        ASSERT_EQ(swmm_options_set_ext(m.e,key,"AUTO"),SWMM_OK);
    ASSERT_EQ(swmm_options_set_ext(m.e,"HEAT_MODEL","COUPLED"),SWMM_OK);
    ASSERT_EQ(swmm_options_set_ext(m.e,"BED_HEAT_MODEL","LOCAL_STORAGE"),SWMM_OK);
    ASSERT_EQ(swmm_options_set_ext(m.e,"BED_HEAT_MODEL","bad"),SWMM_ERR_BADPARAM);
    const auto saved=out/"_options-new.inp";
    ASSERT_EQ(swmm_model_write(m.e,saved.string().c_str()),SWMM_OK);
    auto e=swmm_engine_create();
    ASSERT_EQ(swmm_engine_open(e,saved.string().c_str(),(out/"options-new.rpt").string().c_str(),nullptr,nullptr),SWMM_OK);
    const auto& opts=static_cast<openswmm::SWMMEngine*>(e)->surfaceRouter2D().options();
    EXPECT_TRUE(opts.coupled_heat);EXPECT_EQ(opts.bed_heat_model,1);
    EXPECT_TRUE(opts.heat_surface_authored);EXPECT_TRUE(opts.heat_radiative_authored);
    swmm_engine_close(e);swmm_engine_destroy(e);
}
TEST(MeshHeat, FullMarcherAppliesAtmosphereToStagnantWaterAndClosesWaterEnergy) {
    Model m("marcher");ASSERT_NE(m.e,nullptr);
    ASSERT_EQ(swmm_options_set_ext(m.e,"HEAT_MODEL","COUPLED"),SWMM_OK);
    ASSERT_EQ(swmm_options_set_ext(m.e,"HEAT_RADIATIVE_EXCHANGE","YES"),SWMM_OK);
    ASSERT_EQ(swmm_options_set_ext(m.e,"HEAT_SURFACE_EXCHANGE","NO"),SWMM_OK);
    auto& cfg=m.ctx().heat_config.radiative;
    cfg.shortwave_wm2=200;cfg.albedo=0;cfg.shade_factor=0;cfg.sky_view=0;cfg.emiss_water=0;cfg.emiss_landcover=0;
    ASSERT_EQ(swmm_engine_initialize(m.e),SWMM_OK);
    ASSERT_EQ(swmm_engine_start(m.e,0),SWMM_OK);
    auto& state=static_cast<openswmm::SWMMEngine*>(m.e)->surfaceRouter2D().state();
    const auto row=state.transport.temp_row;ASSERT_GE(row,0);ASSERT_TRUE(state.heat.enabled);
    SWMM_Heat2DStatus status{};status.struct_size=sizeof status;
    ASSERT_EQ(swmm_heat_get_2d_status(m.e,&status),SWMM_OK);
    EXPECT_TRUE(status.radiative_active);EXPECT_FALSE(status.bed_active);EXPECT_TRUE(status.experimental);
    const auto initial=state.transport.cell_mass;
    double elapsed=0;ASSERT_EQ(swmm_engine_step(m.e,&elapsed),SWMM_OK);ASSERT_GT(elapsed,0);
    ASSERT_EQ(swmm_engine_end(m.e),SWMM_OK);
    for(int i=0;i<2;++i) {
        const double actual=(state.transport.cell_mass[state.transport.idx(row,i)]-initial[state.transport.idx(row,i)])*4184000.;
        EXPECT_GT(actual,0);EXPECT_NEAR(actual,state.heat.atmospheric_j[i],1e-5);
    }
    double residual[2]={99,99};
    ASSERT_EQ(swmm_heat_get_2d_values(m.e,SWMM_HEAT_2D_THERMAL_RESIDUAL,0,2,residual),SWMM_OK);
    EXPECT_NEAR(residual[0],0,1e-5);EXPECT_NEAR(residual[1],0,1e-5);
    EXPECT_EQ(swmm_heat_get_2d_values(m.e,SWMM_HEAT_2D_BED_ENERGY,0,2,residual),SWMM_ERR_LIFECYCLE);
    EXPECT_EQ(swmm_heat_get_2d_values(m.e,SWMM_HEAT_2D_ATMOSPHERIC_ENERGY,1,2,residual),SWMM_ERR_BADINDEX);
}

TEST(MeshHeat, RuntimeHeatUsesConfiguredCapacityAndExactPowerReceipt) {
    Model m("runtime-capacity");ASSERT_NE(m.e,nullptr);
    ASSERT_EQ(swmm_options_set_ext(m.e,"HEAT_MODEL","COUPLED"),SWMM_OK);
    ASSERT_EQ(swmm_options_set_ext(m.e,"HEAT_SURFACE_EXCHANGE","NO"),SWMM_OK);
    ASSERT_EQ(swmm_options_set_ext(m.e,"HEAT_RADIATIVE_EXCHANGE","NO"),SWMM_OK);
    m.ctx().options.water_density=997;m.ctx().options.water_specific_heat=4210;
    const double cp=997.*4210;
    ASSERT_EQ(swmm_engine_initialize(m.e),SWMM_OK);ASSERT_EQ(swmm_engine_start(m.e,0),SWMM_OK);
    auto& state=static_cast<openswmm::SWMMEngine*>(m.e)->surfaceRouter2D().state();
    const int n=state.transport.n_species,r=state.transport.temp_row;ASSERT_GE(r,0);
    const double initial=std::accumulate(state.transport.cell_mass.begin()+r*2,state.transport.cell_mass.begin()+(r+1)*2,0.);
    std::vector<double> conc(n,0),rates(n,0);
    ASSERT_EQ(swmm_coupling_set_source(m.e,"heater",0,0,0,1000,conc.data(),rates.data(),n,2.),SWMM_OK);
    double actual=0;ASSERT_EQ(swmm_coupling_advance_to(m.e,2.,&actual),SWMM_OK);EXPECT_DOUBLE_EQ(actual,2.);
    const double final=std::accumulate(state.transport.cell_mass.begin()+r*2,state.transport.cell_mass.begin()+(r+1)*2,0.);
    EXPECT_NEAR((final-initial)*cp,2000,1e-5);
    SWMM_CouplingReceipt receipt{};
    ASSERT_EQ(swmm_coupling_get_receipt(m.e,0,"heater",0,&receipt,conc.data(),rates.data(),n),SWMM_OK);
    EXPECT_DOUBLE_EQ(receipt.applied_heat_j,2000);
    ASSERT_EQ(swmm_coupling_advance_to(m.e,4.,&actual),SWMM_OK);
    const double expired=std::accumulate(state.transport.cell_mass.begin()+r*2,state.transport.cell_mass.begin()+(r+1)*2,0.);
    EXPECT_NEAR((expired-initial)*cp,2000,1e-5);
    ASSERT_EQ(swmm_engine_end(m.e),SWMM_OK);
}

TEST(MeshHeat, ForcedEvaporationReceiptsUseExactlyTheHydraulicLossAtDrying) {
    for(double rate:{1e-5,10.}) {
        Model m("accepted-evap"+std::to_string(rate));ASSERT_NE(m.e,nullptr);
        ASSERT_EQ(swmm_options_set_ext(m.e,"EVAPORATION","FORCING"),SWMM_OK);
        ASSERT_EQ(swmm_options_set_ext(m.e,"HEAT_MODEL","COUPLED"),SWMM_OK);
        ASSERT_EQ(swmm_options_set_ext(m.e,"HEAT_SURFACE_EXCHANGE","YES"),SWMM_OK);
        ASSERT_EQ(swmm_options_set_ext(m.e,"HEAT_RADIATIVE_EXCHANGE","NO"),SWMM_OK);
        m.ctx().options.wind_func_coeff_a=0;m.ctx().options.wind_func_coeff_b=0;
        ASSERT_EQ(swmm_engine_initialize(m.e),SWMM_OK);ASSERT_EQ(swmm_engine_start(m.e,0),SWMM_OK);
        auto& state=static_cast<openswmm::SWMMEngine*>(m.e)->surfaceRouter2D().state();
        const double initial_volume=std::accumulate(state.volume.begin(),state.volume.end(),0.);
        ASSERT_EQ(swmm_2d_force_evap_uniform(m.e,rate,SWMM_FORCING_OVERRIDE,SWMM_FORCING_PERSIST),SWMM_OK);
        double actual=0;ASSERT_EQ(swmm_coupling_advance_to(m.e,1.,&actual),SWMM_OK);
        const double final_volume=std::accumulate(state.volume.begin(),state.volume.end(),0.);
        const double thermal_evap=std::accumulate(state.heat.evaporated_m3.begin(),state.heat.evaporated_m3.end(),0.);
        EXPECT_NEAR(thermal_evap,initial_volume-final_volume,1e-12);
        EXPECT_NEAR(thermal_evap,state.evap_loss_total,1e-12);
        EXPECT_GT(thermal_evap,0);
        for(int i=0;i<2;++i) {
            EXPECT_LT(state.heat.latent_j[i],0);
            EXPECT_NEAR(state.heat.latent_j[i],state.heat.atmospheric_j[i],1e-5);
            EXPECT_NEAR(state.heat.water_j[i],state.heat.atmospheric_j[i],1e-5);
            EXPECT_TRUE(std::isfinite(state.transport.cell_mass[state.transport.idx(state.transport.temp_row,i)]));
        }
        if(rate>1)EXPECT_DOUBLE_EQ(final_volume,0);
        ASSERT_EQ(swmm_engine_end(m.e),SWMM_OK);
    }
}
TEST(MeshHeat, AutoInheritsLiveModulesAndFiniteCapacityEditsRequireReinitialization) {
    Model m("live-inputs");ASSERT_NE(m.e,nullptr);
    ASSERT_EQ(swmm_options_set_ext(m.e,"HEAT_MODEL","COUPLED"),SWMM_OK);
    ASSERT_EQ(swmm_options_set_ext(m.e,"BED_HEAT_MODEL","LOCAL_STORAGE"),SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(m.e),SWMM_OK);ASSERT_EQ(swmm_engine_start(m.e,0),SWMM_OK);
    EXPECT_EQ(swmm_options_set_ext(m.e,"BED_HEAT_MODEL","NONE"),SWMM_ERR_LIFECYCLE);
    EXPECT_EQ(swmm_options_set(m.e,"HEAT_TRANSPORT","NO"),SWMM_ERR_LIFECYCLE);
    EXPECT_EQ(swmm_options_set(m.e,"WATER_DENSITY","999"),SWMM_ERR_LIFECYCLE);
    EXPECT_EQ(swmm_heat_set_sediment(m.e,SWMM_HEAT_SED_BED_THICKNESS,.4),SWMM_ERR_LIFECYCLE);
    ASSERT_EQ(swmm_heat_set_module(m.e,SWMM_HEAT_RADIATIVE_EXCHANGE,1),SWMM_OK);
    ASSERT_EQ(swmm_heat_set_radiative(m.e,SWMM_HEAT_RAD_SHORTWAVE,300),SWMM_OK);
    double actual=0;ASSERT_EQ(swmm_coupling_advance_to(m.e,1,&actual),SWMM_OK);
    const auto& h=static_cast<openswmm::SWMMEngine*>(m.e)->surfaceRouter2D().state().heat;
    EXPECT_TRUE(h.radiative);ASSERT_EQ(swmm_engine_end(m.e),SWMM_OK);
}
TEST(MeshHeat, AtomicAndSnapshotRestoreIncludeMeshGroundChoicesAndAuthoringFlags) {
    Model m("atomic-ground");ASSERT_NE(m.e,nullptr);
    auto snapshot=swmm_heat_snapshot_create(m.e);ASSERT_NE(snapshot,nullptr);
    struct Edit {int result=SWMM_ERR_BADPARAM;bool applied=false;} state;
    const auto edit=[](SWMM_Engine e,void* data) {
        auto& s=*static_cast<Edit*>(data);
        s.applied=swmm_options_set_ext(e,"HEAT_MODEL","COUPLED")==SWMM_OK &&
            swmm_options_set_ext(e,"BED_HEAT_MODEL","LOCAL_STORAGE")==SWMM_OK &&
            swmm_options_set_ext(e,"GW_ET","BOTH")==SWMM_OK &&
            swmm_gw_transport_option_set(e,"THERMAL_MODEL","POROUS_EQUILIBRIUM")==SWMM_OK &&
            swmm_heat_set_sediment(e,SWMM_HEAT_SED_MESH_POROSITY,.6)==SWMM_OK;
        return s.applied?s.result:SWMM_ERR_INTERNAL;
    };
    EXPECT_EQ(swmm_hydrothermal_edit_atomic(m.e,edit,&state),SWMM_ERR_BADPARAM);ASSERT_TRUE(state.applied);
    auto& options=static_cast<openswmm::SWMMEngine*>(m.e)->surfaceRouter2D().options();
    EXPECT_FALSE(options.coupled_heat);EXPECT_FALSE(options.heat_model_authored);
    char value[128]{};ASSERT_EQ(swmm_gw_transport_option_get(m.e,"THERMAL_MODEL",value,sizeof value),SWMM_OK);
    EXPECT_STREQ(value,"WATER_ONLY");
    ASSERT_EQ(swmm_gw2d_option_get(m.e,"GW_ET",value,sizeof value),SWMM_OK);EXPECT_STREQ(value,"AUTO");
    double porosity=0;ASSERT_EQ(swmm_heat_get_sediment(m.e,SWMM_HEAT_SED_MESH_POROSITY,&porosity),SWMM_OK);
    EXPECT_DOUBLE_EQ(porosity,.3);
    state.result=SWMM_OK;ASSERT_EQ(swmm_hydrothermal_edit_atomic(m.e,edit,&state),SWMM_OK);
    ASSERT_TRUE(options.coupled_heat);ASSERT_TRUE(options.heat_model_authored);
    EXPECT_EQ(swmm_heat_snapshot_restore(m.e,snapshot),SWMM_OK);swmm_heat_snapshot_destroy(snapshot);
    EXPECT_FALSE(options.coupled_heat);EXPECT_FALSE(options.heat_model_authored);
    ASSERT_EQ(swmm_gw_transport_option_get(m.e,"THERMAL_MODEL",value,sizeof value),SWMM_OK);EXPECT_STREQ(value,"WATER_ONLY");
    ASSERT_EQ(swmm_gw2d_option_get(m.e,"GW_ET",value,sizeof value),SWMM_OK);EXPECT_STREQ(value,"AUTO");
    ASSERT_EQ(swmm_heat_get_sediment(m.e,SWMM_HEAT_SED_MESH_POROSITY,&porosity),SWMM_OK);EXPECT_DOUBLE_EQ(porosity,.3);
}
TEST(MeshHeat, StandaloneGroundColumnHasOneFiniteStoreAndConservativeThroughflow) {
    LocalHeat f("standalone-column");ASSERT_NE(f.model.e,nullptr);f.options.bed_heat_model=2;
    ASSERT_EQ(swmm_gw_transport_option_set(f.model.e,"SURFACE_THERMAL_BC","ADIABATIC"),SWMM_OK);
    ASSERT_EQ(swmm_gw_transport_option_set(f.model.e,"DEEP_THERMAL_BC","ADIABATIC"),SWMM_OK);
    f.model.ctx().heat_config.sediment.initial_temp=-5;f.model.ctx().heat_config.sediment.has_initial_temp=true;
    ASSERT_TRUE(f.initialize().empty());auto& h=f.state.heat;auto& cell=h.standalone_ground.cells[0];
    auto& c=cell.column;const auto total=[&](){return std::accumulate(c.energy.begin(),c.energy.end(),0.);};
    const double initial=total(),water0=20*5*4184000.;const double final=f.step(0,100,5,0,60);
    ASSERT_EQ(h.failures[0],0)<<cell.error;EXPECT_GT(total(),initial);
    EXPECT_NEAR(total()+final*4184000.,initial+water0,1e-5);
    const auto capacity=c.capacity;const double before=total();
    openswmm::twoD::advectStandaloneGround(h,0,1e4,-10);
    EXPECT_EQ(c.capacity,capacity);EXPECT_NEAR(total()-before,cell.advective_j,1e-4);
    for(std::size_t j=0;j<c.energy.size();++j) {
        EXPECT_GE(c.energy[j]/c.capacity[j],-10);EXPECT_LE(c.energy[j]/c.capacity[j],20);
    }
    const double after=total();EXPECT_DOUBLE_EQ(f.step(0,0,0,0,600),0);
    EXPECT_NEAR(total(),after,1e-5);EXPECT_EQ(h.failures[0],0);
    const auto preserved=c.energy;
    openswmm::twoD::advectStandaloneGround(h,0,1e308,20);
    EXPECT_EQ(h.failures[0],1);EXPECT_EQ(c.energy,preserved);
}
TEST(MeshHeat, SavingWaterOnlyGroundOptionsDoesNotAuthorDormantConductionDefaults) {
    Model m("ground-defaults","[GW_TRANSPORT_OPTIONS]\nTRANSPORT_AGE NO\n");ASSERT_NE(m.e,nullptr);
    const auto file=out/"_ground-defaults-written.inp";
    ASSERT_EQ(swmm_model_write(m.e,file.string().c_str()),SWMM_OK);
    const auto read=[&](){std::ifstream input(file);return std::string((std::istreambuf_iterator<char>(input)),{});};
    EXPECT_EQ(read().find("\nCONDUCTION "),std::string::npos);
    EXPECT_EQ(read().find("\nTHERMAL_MIXING "),std::string::npos);
    EXPECT_EQ(read().find("\nTHERMAL_MODEL "),std::string::npos);
    ASSERT_EQ(swmm_gw_transport_option_set(m.e,"CONDUCTION","YES"),SWMM_OK);
    ASSERT_EQ(swmm_model_write(m.e,file.string().c_str()),SWMM_OK);
    EXPECT_NE(read().find("\nCONDUCTION "),std::string::npos);
    EXPECT_EQ(read().find("\nTHERMAL_MODEL "),std::string::npos);
}
