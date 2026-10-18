// SPDX-License-Identifier: Apache-2.0
// Reviewed 2D thermal plan: domain-scoped authoring and numerical/runtime gates.
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <cmath>
#include <optional>
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
    Model(const std::string& id,const std::string& extra="",const std::string& mesh_section="") {
        fs::create_directories(out);
        const std::string default_mesh=
            "[2D_VERTICES]\n0 0 0\n10 0 0\n10 10 0\n0 10 0\n\n"
            "[2D_TRIANGLES]\n0 1 2 .03 .1 channel:burned\n0 2 3 .03 .1 forest\n\n";
        const std::string& mesh=mesh_section.empty()?default_mesh:mesh_section;
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
            << mesh
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
    LocalHeat(const std::string& id,const std::string& mesh_section=""):model(id,"",mesh_section) {
        if(model.e)openswmm::twoD::buildMeshTopology(*model.ctx().twod_io.mesh);
        options.coupled_heat=true;options.bed_heat_model=1;
        options.heat_surface=0;options.heat_radiative=0;
        options.cell_closure=openswmm::twoD::CellClosure2D::FLAT;
        if(model.e&&model.ctx().twod_io.mesh)
            state.resize(model.ctx().twod_io.mesh->n_cells(),model.ctx().twod_io.mesh->n_vertices());
        state.transport.temp_row=0;
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
TEST(MeshHeat, HyporheicVelocitySpansZeroToStrongFiniteStorageWithClosedEnergy) {
    // Keep the same finite water/bed capacities while varying only the
    // hyporheic conductance. The expected values come from the independent
    // two-capacity solution, so both the zero-velocity limit and a very fast
    // exchange are checked without reusing the production stepper.
    constexpr double water_volume=5,initial_water_c=22,initial_bed_c=-4,dt=3600;
    for(const double velocity:{0.,1e-12,2e-5,.1,1e4}) {
        LocalHeat f("hyporheic-limits-"+std::to_string(velocity));ASSERT_NE(f.model.e,nullptr);
        auto& cfg=f.model.ctx().heat_config.sediment;
        cfg.has_initial_temp=true;cfg.initial_temp=initial_bed_c;cfg.ground_depth=0;
        cfg.hyporheic_velocity=velocity;
        ASSERT_TRUE(f.initialize().empty());

        const double rho_cp=f.model.ctx().options.water_density*f.model.ctx().options.water_specific_heat;
        const double cw=rho_cp*water_volume,cb=f.state.heat.bed_capacity[0];
        const double initial_energy=cw*initial_water_c+cb*initial_bed_c;
        const double conductance=cfg.thermal_diffusivity*cfg.sed_density*cfg.sed_specific_heat*50/cfg.bed_thickness+
                                 rho_cp*velocity*50;
        const double equilibrium=initial_energy/(cw+cb);
        const double difference=(initial_water_c-initial_bed_c)*
            std::exp(-conductance*(1/cw+1/cb)*dt);
        const double expected_water=equilibrium+cb/(cw+cb)*difference;
        const double expected_bed=equilibrium-cw/(cw+cb)*difference;

        const double temperature_volume=f.step(0,initial_water_c*water_volume,water_volume,0,dt);
        EXPECT_NEAR(temperature_volume/water_volume,expected_water,1e-11)<<"v_h="<<velocity;
        EXPECT_NEAR(f.state.heat.bed_energy[0]/cb,expected_bed,1e-11)<<"v_h="<<velocity;
        EXPECT_NEAR(rho_cp*temperature_volume+f.state.heat.bed_energy[0],initial_energy,
                    1e-12*std::max(1.,std::abs(initial_energy)))<<"v_h="<<velocity;
        EXPECT_NEAR(f.state.heat.water_bed_j[0],f.state.heat.bed_energy[0]-cb*initial_bed_c,1e-5)
            <<"v_h="<<velocity;
        EXPECT_NEAR(f.state.heat.water_j[0]+f.state.heat.water_bed_j[0],0,1e-5)
            <<"v_h="<<velocity;
    }
}

TEST(MeshHeat, LocalBedHyporheicIsInvariantUnderUniformSpatialAndTimeRefinement) {
    const std::string refined_mesh=
        "[2D_VERTICES]\n0 0 0\n10 0 0\n10 10 0\n0 10 0\n5 0 0\n\n"
        "[2D_TRIANGLES]\n"
        "0 4 2 .03 .1 channel:burned\n"
        "4 1 2 .03 .1 channel:burned\n"
        "0 2 3 .03 .1 forest\n\n";
    LocalHeat coarse_one_model("uniform-hyporheic-coarse-one"),
              coarse_many_model("uniform-hyporheic-coarse-many"),
              refined_one_model("uniform-hyporheic-refined-one",refined_mesh),
              refined_many_model("uniform-hyporheic-refined-many",refined_mesh);
    for(LocalHeat* f:{&coarse_one_model,&coarse_many_model,&refined_one_model,&refined_many_model}) {
        ASSERT_NE(f->model.e,nullptr);
        auto& sediment=f->model.ctx().heat_config.sediment;
        sediment.has_initial_temp=true;sediment.initial_temp=-5;
        sediment.ground_depth=0;sediment.hyporheic_velocity=.01;
        ASSERT_TRUE(f->initialize().empty());
    }

    struct Result {double water_c=0,bed_c=0,water_j=0,bed_j=0,volume=0;};
    const auto run=[](LocalHeat& f,int steps) {
        Result result;
        const auto& mesh=f.mesh();auto& heat=f.state.heat;
        const double rho_cp=f.model.ctx().options.water_density*f.model.ctx().options.water_specific_heat;
        constexpr double depth=.1,initial_temperature=22,total_seconds=3600;
        double final_mass=0,final_bed_energy=0,bed_capacity=0;
        for(int i=0;i<mesh.n_cells();++i) {
            if(mesh.tri_tag[i]!="channel:burned")continue;
            const double area=mesh.tri_area[i],volume=area*depth;
            const double initial_mass=initial_temperature*volume;
            double mass=initial_mass;
            for(int k=0;k<steps;++k)mass=f.step(i,mass,volume,0,total_seconds/steps);
            result.water_j+=rho_cp*(mass-initial_mass);
            result.bed_j+=heat.bed_energy[i]-heat.initial_bed_energy[i];
            result.volume+=volume;final_mass+=mass;
            final_bed_energy+=heat.bed_energy[i];bed_capacity+=heat.bed_capacity[i];
        }
        result.water_c=final_mass/result.volume;
        result.bed_c=final_bed_energy/bed_capacity;
        return result;
    };

    ASSERT_EQ(coarse_one_model.mesh().n_cells(),2);ASSERT_EQ(refined_many_model.mesh().n_cells(),3);
    const auto coarse_one=run(coarse_one_model,1),coarse_many=run(coarse_many_model,60);
    const auto refined_one=run(refined_one_model,1),refined_many=run(refined_many_model,60);
    for(const auto& result:{coarse_one,coarse_many,refined_one,refined_many}) {
        EXPECT_DOUBLE_EQ(result.volume,5.0);
        EXPECT_NEAR(result.water_j+result.bed_j,0,1e-6);
    }
    EXPECT_NEAR(coarse_one.water_c,coarse_many.water_c,1e-11);
    EXPECT_NEAR(coarse_one.bed_c,coarse_many.bed_c,1e-11);
    EXPECT_NEAR(coarse_one.water_c,refined_one.water_c,1e-11);
    EXPECT_NEAR(coarse_one.bed_c,refined_one.bed_c,1e-11);
    EXPECT_NEAR(coarse_one.water_c,refined_many.water_c,1e-11);
    EXPECT_NEAR(coarse_one.bed_c,refined_many.bed_c,1e-11);
    EXPECT_NEAR(coarse_one.water_j,coarse_many.water_j,1e-5);
    EXPECT_NEAR(coarse_one.bed_j,coarse_many.bed_j,1e-5);
    EXPECT_NEAR(coarse_one.water_j,refined_many.water_j,1e-5);
    EXPECT_NEAR(coarse_one.bed_j,refined_many.bed_j,1e-5);
}

TEST(MeshHeat, ConstantRadiationAndFiniteBedMatchIndependentForcedTwoBodySolution) {
    LocalHeat f("forced-pair");ASSERT_NE(f.model.e,nullptr);
    auto& ctx=f.model.ctx();auto& cfg=ctx.heat_config.sediment;
    cfg.has_initial_temp=true;cfg.initial_temp=-4;cfg.ground_depth=0;cfg.hyporheic_velocity=2e-5;
    auto& radiation=ctx.heat_config.radiative;
    radiation.albedo=.25;radiation.shade_factor=.2;radiation.emiss_water=0;
    radiation.sky_view=0;radiation.emiss_landcover=0;radiation.shortwave_wm2=500;
    f.options.heat_radiative=1;
    ASSERT_TRUE(f.initialize().empty());

    constexpr double water_volume=5,wet_area=50,initial_water_c=22,initial_bed_c=-4,dt=900;
    const double rho_cp=ctx.options.water_density*ctx.options.water_specific_heat;
    const double cw=rho_cp*water_volume,cb=f.state.heat.bed_capacity[0];
    const double conductive=cfg.thermal_diffusivity*cfg.sed_density*cfg.sed_specific_heat*wet_area/cfg.bed_thickness;
    const double hyporheic=rho_cp*cfg.hyporheic_velocity*wet_area;
    const double g=conductive+hyporheic,k=g*(1/cw+1/cb);
    const double incoming_power=500*(1-radiation.albedo)*(1-radiation.shade_factor)*wet_area;
    const double total_capacity=cw+cb;
    const double initial_mean=(cw*initial_water_c+cb*initial_bed_c)/total_capacity;
    const double delta0=initial_water_c-initial_bed_c;
    const double delta=delta0*std::exp(-k*dt)+incoming_power/(cw*k)*(-std::expm1(-k*dt));
    const double expected_water=initial_mean+incoming_power*dt/total_capacity+cb/total_capacity*delta;
    const double expected_bed=initial_mean+incoming_power*dt/total_capacity-cw/total_capacity*delta;

    openswmm::twoD::prepareMeshHeat(ctx,f.state,dt);
    const double mass=f.step(0,initial_water_c*water_volume,water_volume,0,dt);
    EXPECT_NEAR(mass/water_volume,expected_water,1e-10);
    EXPECT_NEAR(f.state.heat.bed_energy[0]/cb,expected_bed,1e-10);
    EXPECT_NEAR(f.state.heat.atmospheric_j[0],incoming_power*dt,1e-6);
    EXPECT_NEAR(f.state.heat.water_j[0]+(f.state.heat.bed_energy[0]-cb*initial_bed_c),
                incoming_power*dt,1e-6);
    EXPECT_NEAR(f.state.heat.water_bed_j[0],f.state.heat.bed_energy[0]-cb*initial_bed_c,1e-6);
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
TEST(MeshHeat, LocalBedTracerMatchesFinitePorewaterResidenceDecayWithoutAddingWater) {
    for(double dt:{1.,60.,900.,7200.}) {
        LocalHeat f("local-tracer-"+std::to_string(dt));ASSERT_NE(f.model.e,nullptr);
        f.state.transport.resize(3,2,0);f.state.transport.temp_row=2;f.state.transport.age_row=1;
        auto& material=f.model.ctx().heat_config.sediment;
        material.mesh_porosity=.4;material.bed_thickness=.3;material.solute_diffusivity=0;material.hyporheic_velocity=2e-5;
        ASSERT_TRUE(f.initialize().empty());auto& h=f.state.heat;
        ASSERT_EQ(h.bed_pore_volume.size(),2);EXPECT_DOUBLE_EQ(h.bed_pore_volume[0],6);
        const double output=openswmm::twoD::applyLocalBedSpecies(f.mesh(),h,0,0,50,5,dt);
        const double mean=50./11,expected=mean+6./11*10*std::exp(-.001*(1./5+1./6)*dt);
        EXPECT_NEAR(output/5,expected,1e-12);EXPECT_NEAR(output+h.bed_carried_mass[0],50,1e-12);
        EXPECT_NEAR(h.bed_exchange_mass[0],50-output,1e-12);EXPECT_DOUBLE_EQ(h.bed_pore_volume[0],6);
        // Ordinary sheet cells have no parameterized recirculation.
        EXPECT_DOUBLE_EQ(openswmm::twoD::applyLocalBedSpecies(f.mesh(),h,1,0,50,5,dt),50);
        EXPECT_DOUBLE_EQ(h.bed_carried_mass[1],0);
    }
}
TEST(MeshHeat, DrySurfaceRetainsLocalBedTracerAndAgeAndCleanRewettingReturnsItsStoredMass) {
    LocalHeat f("local-tracer-rewet");ASSERT_NE(f.model.e,nullptr);
    f.state.transport.resize(3,2,0);f.state.transport.temp_row=2;f.state.transport.age_row=1;
    f.model.ctx().heat_config.sediment.hyporheic_velocity=.001;
    ASSERT_TRUE(f.initialize().empty());auto& h=f.state.heat;
    const double first=openswmm::twoD::applyLocalBedSpecies(f.mesh(),h,0,0,50,5,60);
    ASSERT_GT(h.bed_carried_mass[0],0);const double stored=h.bed_carried_mass[0];
    EXPECT_DOUBLE_EQ(openswmm::twoD::applyLocalBedSpecies(f.mesh(),h,0,0,0,0,60),0);
    EXPECT_DOUBLE_EQ(h.bed_carried_mass[0],stored);
    ASSERT_TRUE(openswmm::twoD::ageLocalBedPorewater(h,60));
    EXPECT_DOUBLE_EQ(h.bed_carried_mass[2],60*h.bed_pore_volume[0]);
    EXPECT_DOUBLE_EQ(h.bed_generated_age[0],60*h.bed_pore_volume[0]);
    const double returned=openswmm::twoD::applyLocalBedSpecies(f.mesh(),h,0,0,0,5,60);
    EXPECT_GT(returned,0);EXPECT_NEAR(returned+h.bed_carried_mass[0]+first,50,1e-12);
}
TEST(MeshHeat, LocalBedTracerAndAgeRunOnBothActualSpeciesKernelsWithSeparateExchangeReceipts) {
    for(int pollutants:{1,8}) {
        std::string extra="[OPTIONS]\nWATER_AGE ON\n[2D_OPTIONS]\nINFILTRATION NO\n"
                          "[POLLUTANTS]\nDye MG/L 0 0 0 0 NO * 0 0\n";
        for(int p=1;p<pollutants;++p)extra+="P"+std::to_string(p)+" MG/L 0 0 0 0 NO * 0 0\n";
        extra+="[2D_INITIAL_QUALITY]\n* Dye 10\n";
        Model m("local-bed-runtime-"+std::to_string(pollutants),extra);ASSERT_NE(m.e,nullptr);
        ASSERT_EQ(swmm_options_set_ext(m.e,"HEAT_MODEL","COUPLED"),SWMM_OK);
        ASSERT_EQ(swmm_options_set_ext(m.e,"BED_HEAT_MODEL","LOCAL_STORAGE"),SWMM_OK);
        ASSERT_EQ(swmm_options_set_ext(m.e,"HEAT_SURFACE_EXCHANGE","NO"),SWMM_OK);
        ASSERT_EQ(swmm_options_set_ext(m.e,"HEAT_RADIATIVE_EXCHANGE","NO"),SWMM_OK);
        ASSERT_EQ(swmm_heat_set_sediment(m.e,SWMM_HEAT_SED_SOLUTE_DIFFUSIVITY,0),SWMM_OK);
        ASSERT_EQ(swmm_heat_set_sediment(m.e,SWMM_HEAT_SED_HYPORHEIC_VELOCITY,.001),SWMM_OK);
        ASSERT_EQ(swmm_engine_initialize(m.e),SWMM_OK);ASSERT_EQ(swmm_engine_start(m.e,0),SWMM_OK);
        auto& router=static_cast<openswmm::SWMMEngine*>(m.e)->surfaceRouter2D();auto& state=router.state();
        auto& tr=state.transport;auto& h=state.heat;ASSERT_EQ(tr.n_pollut,pollutants);
        const double initial=tr.cell_mass[0]+tr.cell_mass[1],water=state.volume[0]+state.volume[1];
        double actual=0;ASSERT_EQ(swmm_coupling_advance_to(m.e,5,&actual),SWMM_OK);
        EXPECT_GT(h.bed_carried_mass[0],0);EXPECT_DOUBLE_EQ(h.bed_carried_mass[1],0);
        EXPECT_NEAR(tr.cell_mass[0]+tr.cell_mass[1]+h.bed_carried_mass[0]+h.bed_carried_mass[1],initial,1e-10);
        EXPECT_NEAR(tr.gained_bed[0]+h.bed_exchange_mass[0]+h.bed_exchange_mass[1],0,1e-12);
        EXPECT_DOUBLE_EQ(tr.gained_external[0],0);EXPECT_DOUBLE_EQ(state.volume[0]+state.volume[1],water);
        const auto ar=std::size_t(tr.age_row)*2;const double pores=h.bed_pore_volume[0]+h.bed_pore_volume[1];
        EXPECT_NEAR(tr.cell_mass[ar]+tr.cell_mass[ar+1]+h.bed_carried_mass[ar]+h.bed_carried_mass[ar+1],5*(water+pores),1e-10);
        EXPECT_NEAR(h.bed_generated_age[0]+h.bed_generated_age[1],5*pores,1e-12);
        ASSERT_EQ(swmm_engine_end(m.e),SWMM_OK);
    }
}
TEST(MeshHeat, InitialBedQualitySeedsLocalPorewaterWithScopedPrecedenceAndAgeUnits) {
    const std::string extra =
        "[POLLUTANTS]\nDye MG/L 0 0 0 0 NO * 0 0 0\n"
        "[OPTIONS]\nWATER_AGE YES\n"
        "[2D_INITIAL_BED_QUALITY]\n"
        "* Dye 2\nTAG forest Dye 5\nCELL 2 Dye 8\n"
        "* __WATER_AGE__ 1\n";
    Model m("local-bed-initial-quality", extra);
    ASSERT_NE(m.e, nullptr);
    ASSERT_EQ(swmm_options_set_ext(m.e, "HEAT_MODEL", "COUPLED"), SWMM_OK);
    ASSERT_EQ(swmm_options_set_ext(m.e, "BED_HEAT_MODEL", "LOCAL_STORAGE"), SWMM_OK);
    ASSERT_EQ(swmm_heat_set_sediment(m.e, SWMM_HEAT_SED_BED_THICKNESS, .3), SWMM_OK);
    ASSERT_EQ(swmm_heat_set_sediment(m.e, SWMM_HEAT_SED_MESH_POROSITY, .4), SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(m.e), SWMM_OK) << swmm_get_last_error_msg(m.e);

    const auto& router = static_cast<openswmm::SWMMEngine*>(m.e)->surfaceRouter2D();
    const auto& state = router.state();
    const auto& heat = state.heat;
    const auto& transport = state.transport;
    const int dye = transport.rowIndex("Dye");
    const int age = transport.age_row;
    ASSERT_GE(dye, 0);
    ASSERT_GE(age, 0);
    ASSERT_EQ(heat.bed_pore_volume.size(), 2);
    for (int cell = 0; cell < 2; ++cell) {
        const auto k = static_cast<std::size_t>(cell);
        const double expected_dye = cell == 0 ? 2.0 : 8.0;
        EXPECT_NEAR(heat.bed_carried_mass[static_cast<std::size_t>(dye) * 2 + k] /
                        heat.bed_pore_volume[k],
                    expected_dye, 1e-12);
        EXPECT_NEAR(heat.bed_carried_mass[static_cast<std::size_t>(age) * 2 + k] /
                        heat.bed_pore_volume[k],
                    3600.0, 1e-9); // age is authored in hours and stored in seconds
    }
    EXPECT_EQ(heat.bed_initial_mass, heat.bed_carried_mass);
    EXPECT_EQ(heat.bed_exchange_mass, std::vector<double>(heat.bed_carried_mass.size(), 0.0));
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

TEST(MeshHeat, FullMarcherAppliesHyporheicExchangeOnlyOnChannelTaggedCells) {
    Model m("marcher-hyporheic");ASSERT_NE(m.e,nullptr);
    ASSERT_EQ(swmm_options_set_ext(m.e,"HEAT_MODEL","COUPLED"),SWMM_OK);
    ASSERT_EQ(swmm_options_set_ext(m.e,"BED_HEAT_MODEL","LOCAL_STORAGE"),SWMM_OK);
    ASSERT_EQ(swmm_options_set_ext(m.e,"HEAT_SURFACE_EXCHANGE","NO"),SWMM_OK);
    ASSERT_EQ(swmm_options_set_ext(m.e,"HEAT_RADIATIVE_EXCHANGE","NO"),SWMM_OK);
    auto& sediment=m.ctx().heat_config.sediment;
    sediment.has_initial_temp=true;sediment.initial_temp=-5;sediment.ground_depth=0;
    sediment.hyporheic_velocity=.01;
    ASSERT_EQ(swmm_engine_initialize(m.e),SWMM_OK);
    ASSERT_EQ(swmm_engine_start(m.e,0),SWMM_OK);
    auto& state=static_cast<openswmm::SWMMEngine*>(m.e)->surfaceRouter2D().state();
    const int row=state.transport.temp_row;ASSERT_GE(row,0);ASSERT_TRUE(state.heat.bed);
    const double rho_cp=m.ctx().options.water_density*m.ctx().options.water_specific_heat;
    const auto initial_volume=state.volume;
    std::vector<double> initial_water(2),initial_bed=state.heat.bed_energy;
    for(int i=0;i<2;++i) {
        const double volume=state.volume[i];ASSERT_GT(volume,0);
        initial_water[i]=state.transport.cell_mass[state.transport.idx(row,i)];
        const double water_c=initial_water[i]/volume;
        const double bed_c=initial_bed[i]/state.heat.bed_capacity[i];
        EXPECT_NE(water_c,bed_c);
    }
    double elapsed=0;ASSERT_EQ(swmm_engine_step(m.e,&elapsed),SWMM_OK);ASSERT_GT(elapsed,0);
    ASSERT_EQ(swmm_engine_end(m.e),SWMM_OK);
    const auto& mesh=m.ctx().twod_io.mesh;
    ASSERT_NE(mesh,nullptr);ASSERT_EQ(mesh->tri_tag[0],"channel:burned");ASSERT_EQ(mesh->tri_tag[1],"forest");
    std::vector<double> water_delta(2);
    for(int i=0;i<2;++i) {
        const double volume=state.volume[i];
        water_delta[i]=rho_cp*(state.transport.cell_mass[state.transport.idx(row,i)]-initial_water[i]);
        EXPECT_NEAR(water_delta[i],state.heat.water_j[i],1e-6);
        EXPECT_NEAR(state.heat.bed_energy[i]-initial_bed[i],state.heat.water_bed_j[i],1e-6);
        EXPECT_NEAR(water_delta[i]+(state.heat.bed_energy[i]-initial_bed[i]),0,1e-6);
        EXPECT_NEAR(state.heat.atmospheric_j[i],0,1e-9);
        EXPECT_NEAR(state.heat.ground_j[i],0,1e-9);
        EXPECT_GT(volume,0);
    }
    EXPECT_NEAR(std::accumulate(state.volume.begin(),state.volume.end(),0.),
                std::accumulate(initial_volume.begin(),initial_volume.end(),0.),1e-12);
    EXPECT_GT(std::abs(water_delta[0]),std::abs(water_delta[1]));
    EXPECT_EQ(state.heat.failures[0],0);
    EXPECT_EQ(state.heat.failures[1],0);
}

TEST(MeshHeat, FullMarcherHyporheicReceiptsAreInvariantAcrossUniformChannelRefinement) {
    const std::string refined_mesh=
        "[2D_VERTICES]\n0 0 0\n10 0 0\n10 10 0\n0 10 0\n5 0 0\n\n"
        "[2D_TRIANGLES]\n"
        "0 4 2 .03 .1 channel:burned\n"
        "4 1 2 .03 .1 channel:burned\n"
        "0 2 3 .03 .1 forest\n\n";
    struct Result {
        double water_temperature=0,bed_temperature=0,water_volume=0;
        double water_j=0,bed_j=0,contact_j=0;
    };
    const auto run=[](const std::string& id,const std::string& mesh_section)->std::optional<Result> {
        Model m(id,"",mesh_section);if(!m.e)return {};
        for(const auto& setting:{std::pair{"HEAT_MODEL","COUPLED"},
                                 std::pair{"BED_HEAT_MODEL","LOCAL_STORAGE"},
                                 std::pair{"HEAT_SURFACE_EXCHANGE","NO"},
                                 std::pair{"HEAT_RADIATIVE_EXCHANGE","NO"}})
            if(swmm_options_set_ext(m.e,setting.first,setting.second)!=SWMM_OK)return {};
        auto& ctx=static_cast<openswmm::SWMMEngine*>(m.e)->context();
        ctx.heat_config.sediment.has_initial_temp=true;
        ctx.heat_config.sediment.initial_temp=-5;
        ctx.heat_config.sediment.ground_depth=0;
        ctx.heat_config.sediment.hyporheic_velocity=.01;
        if(swmm_engine_initialize(m.e)!=SWMM_OK||swmm_engine_start(m.e,0)!=SWMM_OK)return {};
        auto& state=static_cast<openswmm::SWMMEngine*>(m.e)->surfaceRouter2D().state();
        const int row=state.transport.temp_row;
        if(row<0||!state.heat.enabled||!state.heat.bed)return {};
        const double rho_cp=ctx.options.water_density*ctx.options.water_specific_heat;
        double initial_water_j=0,initial_bed_j=0,capacity=0,initial_volume=0;
        for(int i=0;i<ctx.twod_io.mesh->n_cells();++i) {
            if(ctx.twod_io.mesh->tri_tag[i]!="channel:burned")continue;
            const double volume=state.volume[i];if(!(volume>0))return {};
            state.transport.cell_mass[state.transport.idx(row,i)]=22*volume;
            initial_water_j+=rho_cp*22*volume;initial_volume+=volume;
            initial_bed_j+=state.heat.bed_energy[i];capacity+=state.heat.bed_capacity[i];
        }
        if(!(initial_volume>0)||!(capacity>0))return {};
        double elapsed=0;
        if(swmm_engine_step(m.e,&elapsed)!=SWMM_OK||!(elapsed>0)||swmm_engine_end(m.e)!=SWMM_OK)return {};
        Result result;double final_water_j=0,final_bed_j=0,final_capacity=0,final_mass=0;
        for(int i=0;i<ctx.twod_io.mesh->n_cells();++i) {
            if(ctx.twod_io.mesh->tri_tag[i]!="channel:burned")continue;
            const double volume=state.volume[i];
            final_water_j+=rho_cp*state.transport.cell_mass[state.transport.idx(row,i)];
            final_mass+=state.transport.cell_mass[state.transport.idx(row,i)];
            result.water_volume+=volume;
            final_bed_j+=state.heat.bed_energy[i];final_capacity+=state.heat.bed_capacity[i];
            result.contact_j+=state.heat.water_bed_j[i];
        }
        result.water_temperature=final_mass/result.water_volume;
        result.bed_temperature=final_bed_j/final_capacity;
        result.water_j=final_water_j-initial_water_j;
        result.bed_j=final_bed_j-initial_bed_j;
        return result;
    };

    const auto coarse=run("marcher-hyporheic-refine-coarse","");
    const auto refined=run("marcher-hyporheic-refine-fine",refined_mesh);
    ASSERT_TRUE(coarse.has_value());ASSERT_TRUE(refined.has_value());
    EXPECT_NEAR(coarse->water_volume,5.0,1e-12);
    EXPECT_NEAR(refined->water_volume,5.0,1e-12);
    EXPECT_NEAR(coarse->water_j+coarse->bed_j,0,1e-5);
    EXPECT_NEAR(refined->water_j+refined->bed_j,0,1e-5);
    EXPECT_NEAR(coarse->water_j,-coarse->contact_j,1e-5);
    EXPECT_NEAR(refined->water_j,-refined->contact_j,1e-5);
    EXPECT_NEAR(coarse->water_temperature,refined->water_temperature,1e-8);
    EXPECT_NEAR(coarse->bed_temperature,refined->bed_temperature,1e-8);
    EXPECT_NEAR(coarse->water_j,refined->water_j,1e-5);
    EXPECT_NEAR(coarse->bed_j,refined->bed_j,1e-5);
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
TEST(MeshHeat, ChannelGroundSourceUsesExistingOverridesAndPreservesExplicitOffAuthoring) {
    Model m("channel-ground-source");ASSERT_NE(m.e,nullptr);int configured=-1;
    ASSERT_EQ(swmm_heat_ground_source_configured(m.e,&configured),SWMM_OK);EXPECT_EQ(configured,0);
    ASSERT_EQ(swmm_heat_set_sediment(m.e,SWMM_HEAT_SED_GROUND_SOURCE,1),SWMM_OK);
    EXPECT_EQ(swmm_heat_set_sediment(m.e,SWMM_HEAT_SED_GROUND_SOURCE,.5),SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_heat_set_element_override(m.e,SWMM_HEAT_ATTR_GROUND_SOURCE,SWMM_HEAT_SCOPE_CELL2D,"1",1),SWMM_ERR_BADPARAM);
    ASSERT_EQ(swmm_heat_set_element_override(m.e,SWMM_HEAT_ATTR_GROUND_SOURCE,SWMM_HEAT_SCOPE_LINK,"C1",0),SWMM_OK);
    double value=-1;int source=-1;char origin[128];
    ASSERT_EQ(swmm_heat_get_effective_element(m.e,SWMM_HEAT_ATTR_GROUND_SOURCE,SWMM_HEAT_SCOPE_LINK,"C1",&value,&source,origin,sizeof origin),SWMM_OK);
    EXPECT_DOUBLE_EQ(value,0);EXPECT_EQ(source,SWMM_HEAT_SCOPE_LINK);
    ASSERT_EQ(swmm_heat_clear_ground_source(m.e),SWMM_OK);
    ASSERT_EQ(swmm_heat_ground_source_configured(m.e,&configured),SWMM_OK);EXPECT_EQ(configured,0);
    ASSERT_EQ(swmm_heat_set_sediment(m.e,SWMM_HEAT_SED_GROUND_SOURCE,0),SWMM_OK);
    ASSERT_EQ(swmm_process_component_register(m.e,"org.hydrocouple.openswmm.heat","channel-ground.cfg"),SWMM_OK);
    const auto file=out/"_channel-ground-roundtrip.inp";ASSERT_EQ(swmm_model_write(m.e,file.string().c_str()),SWMM_OK);
    auto e=swmm_engine_create();ASSERT_EQ(swmm_engine_open(e,file.string().c_str(),(out/"channel-ground-reopen.rpt").string().c_str(),nullptr,nullptr),SWMM_OK);
    EXPECT_EQ(swmm_heat_ground_source_configured(e,&configured),SWMM_OK);EXPECT_EQ(configured,1);
    EXPECT_EQ(swmm_heat_get_sediment(e,SWMM_HEAT_SED_GROUND_SOURCE,&value),SWMM_OK);EXPECT_DOUBLE_EQ(value,0);
    EXPECT_EQ(swmm_heat_get_effective_element(e,SWMM_HEAT_ATTR_GROUND_SOURCE,SWMM_HEAT_SCOPE_LINK,"C1",&value,&source,origin,sizeof origin),SWMM_OK);
    EXPECT_EQ(source,SWMM_HEAT_SCOPE_LINK);EXPECT_DOUBLE_EQ(value,0);
    swmm_engine_close(e);swmm_engine_destroy(e);
}
TEST(MeshHeat, ChannelThermalBindingsUpdateActualLegacyArdAndLardStoresAtomically) {
    for(const auto* solver:{"LEGACY","EULERIAN_ARD","LAGRANGIAN"}) {
        Model m(std::string("channel-binding-")+solver,"[DWF]\nJ1 FLOW .01\n[TRANSPORT_OPTIONS]\nTARGET_DX 5\n");
        ASSERT_NE(m.e,nullptr);ASSERT_EQ(swmm_options_set(m.e,"QUALITY_SOLVER",solver),SWMM_OK);
        ASSERT_EQ(swmm_engine_initialize(m.e),SWMM_OK);ASSERT_EQ(swmm_engine_start(m.e,0),SWMM_OK);
        double actual=0;ASSERT_EQ(swmm_coupling_advance_to(m.e,5.,&actual),SWMM_OK);
        auto& access=m.ctx().state_accessors;ASSERT_TRUE(bool(access.get_channel_thermal_regions));
        ASSERT_TRUE(bool(access.apply_channel_thermal_increments));const auto before=access.get_channel_thermal_regions();
        ASSERT_FALSE(before.empty())<<solver;
        if(std::string(solver)=="EULERIAN_ARD")EXPECT_GT(before.size(),1);
        else EXPECT_EQ(before.size(),1);
        std::vector<openswmm::ChannelThermalIncrement> increments;
        for(const auto& r:before){
            EXPECT_GT(r.volume,0);EXPECT_LT(r.begin,r.end);EXPECT_GT(r.length,0);EXPECT_GT(r.bed_area,0);EXPECT_GT(r.surface_area,0);
            increments.push_back({r.link,r.region,3});
        }
        ASSERT_TRUE(access.apply_channel_thermal_increments(increments).empty());
        const auto after=access.get_channel_thermal_regions();ASSERT_EQ(after.size(),before.size());
        double water=0,mass=0;
        for(std::size_t k=0;k<after.size();++k) {
            EXPECT_EQ(after[k].region,before[k].region);EXPECT_DOUBLE_EQ(after[k].volume,before[k].volume);
            EXPECT_NEAR(after[k].temperature,before[k].temperature+3,1e-12);
            water+=after[k].volume;mass+=after[k].volume*after[k].temperature;
        }
        EXPECT_NEAR(m.ctx().heat_state.link_temp[0],mass/water,1e-12);
        increments.push_back({0,999999,4});
        EXPECT_FALSE(access.apply_channel_thermal_increments(increments).empty());
        const auto refused=access.get_channel_thermal_regions();ASSERT_EQ(refused.size(),after.size());
        for(std::size_t k=0;k<after.size();++k)EXPECT_DOUBLE_EQ(refused[k].temperature,after[k].temperature);
        ASSERT_EQ(swmm_engine_end(m.e),SWMM_OK);
    }
}
