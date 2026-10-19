// SPDX-License-Identifier: Apache-2.0
// Reviewed 2D thermal plan: domain-scoped authoring and numerical/runtime gates.
#include <gtest/gtest.h>
#include <array>
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
#include "2d/heat/ThermalCheckpoint.hpp"
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
TEST(MeshHeat, LocalBedPorewaterVolumeIsAvailableWithoutQualityTransport) {
    LocalHeat f("local-bed-pore-volume-no-quality");ASSERT_NE(f.model.e,nullptr);
    auto& cfg=f.model.ctx().heat_config.sediment;
    cfg.mesh_porosity=.25;cfg.bed_thickness=.2;
    ASSERT_TRUE(f.initialize().empty());

    const auto& heat=f.state.heat;
    ASSERT_EQ(f.state.transport.n_species,0);
    ASSERT_EQ(heat.bed_pore_volume.size(),static_cast<std::size_t>(f.mesh().n_cells()));
    EXPECT_TRUE(heat.bed_carried_mass.empty());
    EXPECT_TRUE(heat.bed_initial_mass.empty());
    EXPECT_TRUE(heat.bed_exchange_mass.empty());
    for(std::size_t cell=0;cell<static_cast<std::size_t>(f.mesh().n_cells());++cell)
        EXPECT_DOUBLE_EQ(heat.bed_pore_volume[cell],f.mesh().tri_area[cell]*cfg.bed_thickness*cfg.mesh_porosity);
}
TEST(MeshHeat, LocalBedEnergyTracerAndAgeRoundTripThroughThermalCheckpointState) {
    LocalHeat source("local-bed-thermal-checkpoint-source"),target("local-bed-thermal-checkpoint-target");
    ASSERT_NE(source.model.e,nullptr);ASSERT_NE(target.model.e,nullptr);
    for(auto* f:{&source,&target}) {
        f->state.transport.resize(3,2,0);
        f->state.transport.temp_row=2;
        f->state.transport.age_row=1;
        ASSERT_TRUE(f->initialize().empty());
    }
    auto& original=source.state.heat;
    auto& restored=target.state.heat;
    ASSERT_EQ(original.bed_initial_mass.size(),original.bed_carried_mass.size());
    ASSERT_EQ(original.bed_species,3);
    for(std::size_t i=0;i<original.bed_initial_mass.size();++i) {
        original.bed_initial_mass[i]=restored.bed_initial_mass[i]=10.0+double(i);
        original.bed_carried_mass[i]=20.0+double(i);
        original.bed_exchange_mass[i]=-2.0+double(i);
    }
    original.bed_energy[0]+=125.0;
    original.water_j[0]=200.0;
    original.water_bed_j[0]=-75.0;
    original.bed_generated_age[0]=3600.0*original.bed_pore_volume[0];
    ASSERT_EQ(openswmm::twoD::checkpoint::meshHeatModelIdentity(original),
              openswmm::twoD::checkpoint::meshHeatModelIdentity(restored));

    auto snapshot=original;
    openswmm::twoD::checkpoint::Archive saved;
    openswmm::twoD::checkpoint::meshHeat(saved,snapshot);
    openswmm::twoD::checkpoint::Archive input(saved.bytes());
    openswmm::twoD::checkpoint::meshHeat(input,restored);
    input.finish();

    EXPECT_EQ(restored.bed_energy,original.bed_energy);
    EXPECT_EQ(restored.bed_carried_mass,original.bed_carried_mass);
    EXPECT_EQ(restored.bed_exchange_mass,original.bed_exchange_mass);
    EXPECT_EQ(restored.bed_generated_age,original.bed_generated_age);
    EXPECT_EQ(restored.water_j,original.water_j);
    EXPECT_EQ(restored.water_bed_j,original.water_bed_j);
    EXPECT_EQ(restored.bed_initial_mass,original.bed_initial_mass);
    EXPECT_EQ(openswmm::twoD::checkpoint::meshHeatModelIdentity(original),
              openswmm::twoD::checkpoint::meshHeatModelIdentity(restored));
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
TEST(MeshHeat, RoutedChannelBedMatchesPinnedHtsHeatBalanceTerms) {
    // Reference: HydroCouple/HTSComponent commit
    // 1999b57d2298c7fdf91c0620143b73268f344410,
    // examples/kuparuk_river1/kuparuk_river1.inp, element E1. The row supplies
    // W=12.3 m, L=100 m, depth=0.388054 m, Tbed=Tground=10.86 C,
    // Qmain=0.084005823 m3/s, ground depth=0.117546 m, and alpha=6.10e-7 m2/s.
    // This isolates HTS's channel/bed conduction, main-channel advection, and
    // deep-ground conduction terms in one 2D mesh cell; longitudinal channel
    // transport and atmospheric terms are outside this cell-level comparison.
    constexpr double area=12.3*100.,depth=.388054,ground_depth=.117546;
    constexpr double q_main=.084005823,alpha=6.10e-7;
    constexpr double reference_temperature=10.86,initial_water_temperature=11.86,dt=1800.;
    const std::string reference_mesh=
        "[2D_VERTICES]\n0 0 0\n1230 0 0\n0 2 0\n\n"
        "[2D_TRIANGLES]\n0 1 2 .03 .1 channel:hts-reference\n\n";
    LocalHeat f("hts-channel-reference",reference_mesh);ASSERT_NE(f.model.e,nullptr);
    auto& cfg=f.model.ctx().heat_config.sediment;
    cfg.mesh_porosity=0; // HTS E1's bed capacity uses rho_s*c_s*volume.
    cfg.sed_density=1670;cfg.sed_specific_heat=1807;
    cfg.thermal_diffusivity=alpha;cfg.bed_thickness=depth;cfg.ground_depth=ground_depth;
    cfg.ground_temp=reference_temperature;cfg.has_ground_temp=true;
    cfg.initial_temp=reference_temperature;cfg.has_initial_temp=true;
    cfg.hyporheic_velocity=q_main/area; // maps HTS Qmain to v_hyp*A_bed.
    ASSERT_TRUE(f.initialize().empty());
    ASSERT_EQ(f.mesh().n_cells(),1);
    EXPECT_NEAR(f.mesh().tri_area[0],area,1e-12);

    const double rho=f.model.ctx().options.water_density,cp=f.model.ctx().options.water_specific_heat;
    const double volume=area*depth,rho_cp=rho*cp;
    const double cw=rho_cp*volume;
    const double cb=cfg.sed_density*cfg.sed_specific_heat*volume;
    const double g_wb=alpha*cfg.sed_density*cfg.sed_specific_heat*area/depth+rho_cp*q_main;
    const double g_bg=alpha*cfg.sed_density*cfg.sed_specific_heat*area/ground_depth;
    ASSERT_NEAR(f.state.heat.bed_capacity[0],cb,1e-6);

    // Independently integrate the mapped HTS temperature ODE with RK4.
    // The runtime implementation solves the same finite water/bed pair
    // analytically, so this reference also checks the full accepted 2D step.
    const auto derivative=[&](double tw,double tb) {
        return std::array<double,2>{-g_wb*(tw-tb)/cw,
            (g_wb*(tw-tb)+g_bg*(reference_temperature-tb))/cb};
    };
    double tw=initial_water_temperature,tb=reference_temperature;
    constexpr double h=.25;
    for(double elapsed=0;elapsed<dt;elapsed+=h) {
        const auto k1=derivative(tw,tb);
        const auto k2=derivative(tw+.5*h*k1[0],tb+.5*h*k1[1]);
        const auto k3=derivative(tw+.5*h*k2[0],tb+.5*h*k2[1]);
        const auto k4=derivative(tw+h*k3[0],tb+h*k3[1]);
        tw+=h*(k1[0]+2*k2[0]+2*k3[0]+k4[0])/6;
        tb+=h*(k1[1]+2*k2[1]+2*k3[1]+k4[1])/6;
    }

    const double updated_temperature_volume=f.step(0,initial_water_temperature*volume,volume,0,dt);
    EXPECT_NEAR(updated_temperature_volume/volume,tw,2e-10);
    EXPECT_NEAR(f.state.heat.bed_energy[0]/cb,tb,2e-10);
    const double water_delta=cw*(tw-initial_water_temperature);
    const double bed_delta=cb*(tb-reference_temperature);
    EXPECT_NEAR(f.state.heat.water_j[0],water_delta,1e-3);
    EXPECT_NEAR(f.state.heat.water_bed_j[0],-water_delta,1e-3);
    EXPECT_NEAR(f.state.heat.ground_j[0],bed_delta+water_delta,1e-3);
    EXPECT_NEAR(water_delta+bed_delta-f.state.heat.ground_j[0],0,1e-3);
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

TEST(MeshHeat, HeterogeneousCellHyporheicOverridesMatchIndependentPairsAcrossTimeRefinement) {
    const std::string two_channel_cells=
        "[2D_VERTICES]\n0 0 0\n10 0 0\n10 10 0\n0 10 0\n\n"
        "[2D_TRIANGLES]\n0 1 2 .03 .1 channel:burned\n0 2 3 .03 .1 channel:tributary\n\n";
    LocalHeat coarse("heterogeneous-hyporheic-one",two_channel_cells),
              refined("heterogeneous-hyporheic-many",two_channel_cells);
    constexpr int cellScope=6;
    constexpr double slowVelocity=1.0e-12, fastVelocity=.1;
    for(LocalHeat* f:{&coarse,&refined}) {
        ASSERT_NE(f->model.e,nullptr);
        auto& sediment=f->model.ctx().heat_config.sediment;
        sediment.has_initial_temp=true;sediment.initial_temp=-4;
        sediment.ground_depth=0; // isolate the water/bed hyporheic pair
        ASSERT_EQ(swmm_heat_set_element_override(f->model.e,SWMM_HEAT_ATTR_HYPORHEIC_VELOCITY,
                                                  cellScope,"1",slowVelocity),SWMM_OK);
        ASSERT_EQ(swmm_heat_set_element_override(f->model.e,SWMM_HEAT_ATTR_HYPORHEIC_VELOCITY,
                                                  cellScope,"2",fastVelocity),SWMM_OK);
        ASSERT_TRUE(f->initialize().empty());
        ASSERT_EQ(f->state.heat.hyporheic_eligible.size(),2u);
        EXPECT_EQ(f->state.heat.hyporheic_eligible[0],1);
        EXPECT_EQ(f->state.heat.hyporheic_eligible[1],1);
    }

    constexpr double volume=5,waterTemperature=22,initialBedTemperature=-4,totalSeconds=3600;
    struct Result { double water=0,waterJ=0,bedJ=0,receipt=0; };
    const double rhoCp=coarse.model.ctx().options.water_density*coarse.model.ctx().options.water_specific_heat;
    const auto run=[&](LocalHeat& f,int steps) {
        std::array<Result,2> results{};
        for(int cell=0;cell<2;++cell) {
            double mass=waterTemperature*volume;
            for(int step=0;step<steps;++step)
                mass=f.step(cell,mass,volume,0,totalSeconds/steps);
            auto& heat=f.state.heat;
            results[cell].water=mass/volume;
            results[cell].waterJ=heat.water_j[cell];
            results[cell].bedJ=heat.bed_energy[cell]-heat.initial_bed_energy[cell];
            results[cell].receipt=heat.water_bed_j[cell];
        }
        return results;
    };
    const auto one=run(coarse,1),many=run(refined,60);
    const double velocities[]{slowVelocity,fastVelocity};
    for(int cell=0;cell<2;++cell) {
        const auto& sediment=openswmm::transport::heat::sedimentFor(
            coarse.model.ctx(),openswmm::HeatElement::cell2d(cell));
        const double capacity=coarse.state.heat.bed_capacity[cell];
        const double waterCapacity=rhoCp*volume;
        const double conductance=sediment.thermal_diffusivity*sediment.sed_density*
            sediment.sed_specific_heat*50/sediment.bed_thickness+rhoCp*velocities[cell]*50;
        const double initialEnergy=waterCapacity*waterTemperature+capacity*initialBedTemperature;
        const double equilibrium=initialEnergy/(waterCapacity+capacity);
        const double difference=(waterTemperature-initialBedTemperature)*
            std::exp(-conductance*(1/waterCapacity+1/capacity)*totalSeconds);
        const double expectedWater=equilibrium+capacity/(waterCapacity+capacity)*difference;
        EXPECT_NEAR(one[cell].water,expectedWater,1e-11)<<"cell "<<cell;
        EXPECT_NEAR(many[cell].water,expectedWater,1e-11)<<"cell "<<cell;
        EXPECT_NEAR(one[cell].water,many[cell].water,1e-11)<<"cell "<<cell;
        EXPECT_NEAR(one[cell].waterJ+one[cell].bedJ,0,1e-5)<<"cell "<<cell;
        EXPECT_NEAR(many[cell].waterJ+many[cell].bedJ,0,1e-5)<<"cell "<<cell;
        EXPECT_NEAR(one[cell].waterJ+one[cell].receipt,0,1e-5)<<"cell "<<cell;
        EXPECT_NEAR(many[cell].waterJ+many[cell].receipt,0,1e-5)<<"cell "<<cell;
    }
    EXPECT_GT(one[0].water,one[1].water);
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
TEST(MeshHeat, GroundColumnSurfaceHyporheicTracerAndAgeUseFiniteSharedPorewater) {
    LocalHeat f("ground-column-surface-porewater");ASSERT_NE(f.model.e,nullptr);
    auto& config=f.model.ctx().heat_config.sediment;
    config.bed_thickness=.3;config.solute_diffusivity=0;config.hyporheic_velocity=.001;
    openswmm::twoD::ThermalMaterial material;material.porosity=.4;
    openswmm::twoD::GroundHeatState ground;ground.cells.resize(2);
    for(auto& cell:ground.cells) {
        ASSERT_TRUE(cell.column.initialize(50,{0,.3,1.0},material,{.3,.3},{10,10}).empty());
        cell.saturated_water={.1,.1};
        ASSERT_TRUE(cell.porewater.initialize(cell.column,cell.saturated_water,
            {2,20,0},{148,1480,0},{0,0,0},1,2).empty());
    }
    openswmm::twoD::MeshHeatState heat;
    heat.context=&f.model.ctx();heat.enabled=true;heat.bed=true;heat.ground_column=true;
    heat.bed_species=3;heat.bed_age_row=1;heat.bed_temp_row=2;heat.aquifer_ground=&ground;
    heat.aquifer_owned={1,1};heat.hyporheic_eligible={1,0};heat.channel_footprint={25,0};heat.failures={0,0};

    const double dye_before=ground.cells[0].porewater.mass[0]+ground.cells[0].porewater.mass[1];
    const double dye_surface=openswmm::twoD::applyLocalBedSpecies(f.mesh(),heat,0,0,0,5,60);
    const double age_before=ground.cells[0].porewater.mass[2]+ground.cells[0].porewater.mass[3];
    const double age_surface=openswmm::twoD::applyLocalBedSpecies(f.mesh(),heat,0,1,0,5,60);
    ASSERT_GT(dye_surface,0);ASSERT_GT(age_surface,0);
    const double qdt=.001*25*60,soil_capacity=4.5,water_capacity=5;
    const double attenuation=qdt*soil_capacity/(soil_capacity+qdt);
    const double dye_expected=water_capacity*(attenuation*10)/(water_capacity+attenuation);
    const double age_expected=water_capacity*(attenuation*100)/(water_capacity+attenuation);
    EXPECT_NEAR(dye_surface,dye_expected,1e-11);
    EXPECT_NEAR(age_surface,age_expected,1e-10);
    EXPECT_NEAR(dye_surface+ground.cells[0].porewater.mass[0]+ground.cells[0].porewater.mass[1],dye_before,1e-11);
    EXPECT_NEAR(age_surface+ground.cells[0].porewater.mass[2]+ground.cells[0].porewater.mass[3],age_before,1e-10);
    EXPECT_NEAR(ground.cells[0].porewater.contact_mass[0]+ground.cells[0].porewater.contact_mass[1],
                ground.cells[0].porewater.mass[0]+ground.cells[0].porewater.mass[1]-dye_before,1e-11);
    EXPECT_EQ(ground.cells[1].porewater.mass[0]+ground.cells[1].porewater.mass[1],150);
    EXPECT_EQ(heat.failures,std::vector<int>({0,0}));

    auto protected_ground=ground;
    auto& protected_pore=protected_ground.cells[0].porewater;
    const double protected_mass=protected_pore.mass[0];
    ASSERT_TRUE(protected_pore.reserveOutgoing(0,0,protected_mass));
    auto protected_heat=heat;protected_heat.aquifer_ground=&protected_ground;
    EXPECT_DOUBLE_EQ(openswmm::twoD::applyLocalBedSpecies(f.mesh(),protected_heat,0,0,0,5,60),0);
    EXPECT_DOUBLE_EQ(protected_pore.mass[0],protected_mass);
    EXPECT_DOUBLE_EQ(protected_pore.reserved_out[0],protected_mass);
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

TEST(MeshHeat, FullMarcherHonorsHeterogeneousCellHyporheicRatesAndReciprocalReceipts) {
    const std::string two_channel_cells=
        "[2D_VERTICES]\n0 0 0\n10 0 0\n10 10 0\n0 10 0\n\n"
        "[2D_TRIANGLES]\n0 1 2 .03 .1 channel:burned\n0 2 3 .03 .1 channel:tributary\n\n";
    Model m("marcher-hyporheic-heterogeneous","",two_channel_cells);
    ASSERT_NE(m.e,nullptr);
    for(const auto& setting:{std::pair{"HEAT_MODEL","COUPLED"},
                             std::pair{"BED_HEAT_MODEL","LOCAL_STORAGE"},
                             std::pair{"HEAT_SURFACE_EXCHANGE","NO"},
                             std::pair{"HEAT_RADIATIVE_EXCHANGE","NO"}})
        ASSERT_EQ(swmm_options_set_ext(m.e,setting.first,setting.second),SWMM_OK);
    auto& ctx=m.ctx();
    auto& sediment=ctx.heat_config.sediment;
    sediment.has_initial_temp=true;sediment.initial_temp=-5;sediment.ground_depth=0;
    constexpr int cellScope=6;
    constexpr double slowVelocity=1.0e-12,fastVelocity=.1;
    ASSERT_EQ(swmm_heat_set_element_override(m.e,SWMM_HEAT_ATTR_HYPORHEIC_VELOCITY,
                                              cellScope,"1",slowVelocity),SWMM_OK);
    ASSERT_EQ(swmm_heat_set_element_override(m.e,SWMM_HEAT_ATTR_HYPORHEIC_VELOCITY,
                                              cellScope,"2",fastVelocity),SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(m.e),SWMM_OK);
    ASSERT_EQ(swmm_engine_start(m.e,0),SWMM_OK);
    auto& state=static_cast<openswmm::SWMMEngine*>(m.e)->surfaceRouter2D().state();
    const int row=state.transport.temp_row;
    ASSERT_GE(row,0);ASSERT_TRUE(state.heat.enabled);ASSERT_TRUE(state.heat.bed);
    ASSERT_EQ(state.heat.hyporheic_eligible.size(),2u);
    const double rhoCp=ctx.options.water_density*ctx.options.water_specific_heat;
    std::array<double,2> initial_water_j{},initial_bed_j{};
    for(int cell=0;cell<2;++cell) {
        ASSERT_GT(state.volume[cell],0);
        EXPECT_EQ(state.heat.hyporheic_eligible[cell],1);
        state.transport.cell_mass[state.transport.idx(row,cell)]=22*state.volume[cell];
        initial_water_j[cell]=rhoCp*22*state.volume[cell];
        initial_bed_j[cell]=state.heat.bed_energy[cell];
    }
    double elapsed=0;
    ASSERT_EQ(swmm_engine_step(m.e,&elapsed),SWMM_OK);ASSERT_GT(elapsed,0);
    ASSERT_EQ(swmm_engine_end(m.e),SWMM_OK);

    std::array<double,2> contact_receipt{};
    for(int cell=0;cell<2;++cell) {
        const double final_mass=state.transport.cell_mass[state.transport.idx(row,cell)];
        const double water_delta=rhoCp*final_mass-initial_water_j[cell];
        const double bed_delta=state.heat.bed_energy[cell]-initial_bed_j[cell];
        contact_receipt[cell]=state.heat.water_bed_j[cell];
        EXPECT_NEAR(water_delta+bed_delta,0,1e-5)<<"cell "<<cell;
        EXPECT_NEAR(contact_receipt[cell],bed_delta,1e-5)<<"cell "<<cell;
        EXPECT_GT(contact_receipt[cell],0)<<"cell "<<cell;
        EXPECT_EQ(state.heat.failures[cell],0);
    }
    EXPECT_GT(std::abs(contact_receipt[1]),std::abs(contact_receipt[0]));
}

TEST(MeshHeat, FullMarcherHeterogeneousHyporheicReceiptsSurviveSpatialRefinement) {
    const std::string coarse_mesh=
        "[2D_VERTICES]\n0 0 0\n10 0 0\n10 10 0\n0 10 0\n\n"
        "[2D_TRIANGLES]\n0 1 2 .03 .1 channel:burned\n0 2 3 .03 .1 channel:tributary\n\n";
    const std::string refined_mesh=
        "[2D_VERTICES]\n0 0 0\n10 0 0\n10 10 0\n0 10 0\n"
        "5 0 0\n10 5 0\n5 5 0\n5 10 0\n0 5 0\n\n"
        "[2D_TRIANGLES]\n"
        "0 4 6 .03 .1 channel:burned\n4 1 5 .03 .1 channel:burned\n"
        "6 5 2 .03 .1 channel:burned\n4 5 6 .03 .1 channel:burned\n"
        "0 6 8 .03 .1 channel:tributary\n6 2 7 .03 .1 channel:tributary\n"
        "8 7 3 .03 .1 channel:tributary\n6 7 8 .03 .1 channel:tributary\n\n";
    struct Result {double volume=0,water=0,bed=0,contact=0,water_temperature=0,bed_temperature=0;};
    const auto run=[](const std::string& id,const std::string& mesh,int cells_per_region,int time_steps)
        ->std::optional<std::array<Result,2>> {
        Model m(id,"",mesh);if(!m.e)return {};
        for(const auto& setting:{std::pair{"HEAT_MODEL","COUPLED"},
                                 std::pair{"BED_HEAT_MODEL","LOCAL_STORAGE"},
                                 std::pair{"HEAT_SURFACE_EXCHANGE","NO"},
                                 std::pair{"HEAT_RADIATIVE_EXCHANGE","NO"}})
            if(swmm_options_set_ext(m.e,setting.first,setting.second)!=SWMM_OK)return {};
        auto& ctx=m.ctx();ctx.heat_config.sediment.has_initial_temp=true;
        ctx.heat_config.sediment.initial_temp=-5;ctx.heat_config.sediment.ground_depth=0;
        constexpr int cell_scope=6;constexpr double slow=1e-12,fast=.1;
        for(int cell=0;cell<2*cells_per_region;++cell) {
            const double velocity=cell<cells_per_region?slow:fast;
            if(swmm_heat_set_element_override(m.e,SWMM_HEAT_ATTR_HYPORHEIC_VELOCITY,
                cell_scope,std::to_string(cell+1).c_str(),velocity)!=SWMM_OK)return {};
        }
        if(swmm_engine_initialize(m.e)!=SWMM_OK||swmm_engine_start(m.e,0)!=SWMM_OK)return {};
        auto& state=static_cast<openswmm::SWMMEngine*>(m.e)->surfaceRouter2D().state();
        const auto* mesh_data=ctx.twod_io.mesh;const int row=state.transport.temp_row;
        if(row<0||!state.heat.enabled||!state.heat.bed||!mesh_data||
           mesh_data->n_cells()!=2*cells_per_region||state.heat.hyporheic_eligible.size()!=std::size_t(2*cells_per_region))return {};
        const double rho_cp=ctx.options.water_density*ctx.options.water_specific_heat;
        std::array<Result,2> result{};std::array<double,2> initial_water_j{},initial_bed_j{},bed_capacity{};
        for(int cell=0;cell<2*cells_per_region;++cell) {
            const int region=cell<cells_per_region?0:1;
            if(!(state.volume[cell]>0)||!state.heat.hyporheic_eligible[cell])return {};
            const double water_volume=state.volume[cell];
            state.transport.cell_mass[state.transport.idx(row,cell)]=22*water_volume;
            initial_water_j[region]+=rho_cp*22*water_volume;
            initial_bed_j[region]+=state.heat.bed_energy[cell];
            bed_capacity[region]+=state.heat.bed_capacity[cell];
            result[region].volume+=water_volume;
        }
        for(int step=1;step<=time_steps;++step) {
            double actual=0;const double target=5.*step/time_steps;
            if(swmm_coupling_advance_to(m.e,target,&actual)!=SWMM_OK||actual!=target)return {};
        }
        if(swmm_engine_end(m.e)!=SWMM_OK)return {};
        for(int cell=0;cell<2*cells_per_region;++cell) {
            const int region=cell<cells_per_region?0:1;
            const double mass=state.transport.cell_mass[state.transport.idx(row,cell)];
            result[region].water+=rho_cp*mass-initial_water_j[region]/cells_per_region;
            result[region].bed+=state.heat.bed_energy[cell]-initial_bed_j[region]/cells_per_region;
            result[region].contact+=state.heat.water_bed_j[cell];
            result[region].water_temperature+=mass;
            result[region].bed_temperature+=state.heat.bed_energy[cell];
            EXPECT_EQ(state.heat.failures[cell],0);
        }
        for(int region=0;region<2;++region) {
            result[region].water_temperature/=result[region].volume;
            result[region].bed_temperature/=bed_capacity[region];
        }
        return result;
    };
    const auto coarse=run("marcher-heterogeneous-refine-coarse",coarse_mesh,1,1);
    const auto coarse_fine_time=run("marcher-heterogeneous-refine-coarse-time",coarse_mesh,1,10);
    const auto refined=run("marcher-heterogeneous-refine-fine",refined_mesh,4,1);
    const auto refined_fine_time=run("marcher-heterogeneous-refine-fine-time",refined_mesh,4,10);
    ASSERT_TRUE(coarse.has_value());ASSERT_TRUE(coarse_fine_time.has_value());
    ASSERT_TRUE(refined.has_value());ASSERT_TRUE(refined_fine_time.has_value());
    for(int region=0;region<2;++region) {
        for(const auto* result:{&*coarse,&*coarse_fine_time,&*refined,&*refined_fine_time}) {
            EXPECT_NEAR((*result)[region].volume,5,1e-12);
            EXPECT_NEAR((*result)[region].water+(*result)[region].bed,0,1e-5);
            EXPECT_NEAR((*result)[region].bed,(*result)[region].contact,1e-5);
        }
        for(const auto* result:{&*coarse_fine_time,&*refined,&*refined_fine_time}) {
            EXPECT_NEAR((*coarse)[region].water,(*result)[region].water,1e-5);
            EXPECT_NEAR((*coarse)[region].bed,(*result)[region].bed,1e-5);
            EXPECT_NEAR((*coarse)[region].water_temperature,(*result)[region].water_temperature,1e-9);
            EXPECT_NEAR((*coarse)[region].bed_temperature,(*result)[region].bed_temperature,1e-9);
        }
    }
    EXPECT_GT(std::abs((*coarse)[1].contact),std::abs((*coarse)[0].contact));
    EXPECT_GT(std::abs((*refined)[1].contact),std::abs((*refined)[0].contact));
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
