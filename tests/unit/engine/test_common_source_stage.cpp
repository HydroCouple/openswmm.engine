// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include "2d/subsurface/CommonSourceStage.hpp"
#include "2d/subsurface/SubsurfaceSolver.hpp"
#include "2d/solver/ExplicitInertialSolver.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include "core/SimulationContext.hpp"
#include "core/DateTime.hpp"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>

using namespace openswmm;
using namespace openswmm::twoD;
namespace {
constexpr double ft2=.3048*.3048,ft3=ft2*.3048;
struct AtmosphericInput {
    std::array<double,2> pet{0,0},impervious{0,0}; // ft/s, fraction of non-LID area
    std::array<double,2> rain_override{-1,-1}; // ft/s; negative retains gage records
    double rain=432,lid_area=.5,lid_bottom=432,lid_saturation=50,ks=.01;
    bool barrel=false,covered=false;
    std::string et="NONE";
    GwClosure closure=GwClosure::CLOSED_FORM;SoilChar law=SoilChar::GARDNER;
};
struct Model {
    MeshData mesh; SurfaceStateData surface; SolverOptions2D options;
    InertialEdges edges; SubsurfaceConfig config; SubsurfaceSolver gw;
    SimulationContext context; runoff::SourceWaterDriver sources;
    SurfaceOwnershipPreview preview; CommonSourceStage stage;
    ExplicitInertialSolver marcher;
    void setup(bool trench=false,int cells=1,bool reverse=false,bool adjacent=false,bool delayed_rain=false,
               const AtmosphericInput& a={}) {
        mesh.resize_vertices(3*cells);mesh.resize_triangles(cells);
        for(int i=0;i<cells;++i) {
            mesh.vx[3*i]=6*i;mesh.vx[3*i+1]=6*i+4;mesh.vx[3*i+2]=6*i;
            mesh.vy[3*i]=0;mesh.vy[3*i+1]=0;mesh.vy[3*i+2]=2;
            for(int j=0;j<3;++j)mesh.vz[3*i+j]=10;
            mesh.set_triangle(i,3*i,3*i+1,3*i+2);mesh.mannings_n[i]=.03;
        }
        if(adjacent) {
            mesh.vx[3]=4;mesh.vy[3]=2;mesh.set_triangle(1,1,3,2);
        }
        buildMeshTopology(mesh);edges.build(mesh);surface.resize(cells,mesh.n_edge_slots());
        options.num_threads=1;options.lts_tiers=3;
        config.options.authored=true;config.options.force_closed_form=true;
        config.options.gw_et=a.et;config.options.closure=a.closure;config.options.soil_char=a.law;
        GwAquiferRow row;row.Ks=a.ks;row.zs=2;row.hg0=1;
        config.rows.push_back(row);config.node_beds.push_back({0,0,.01,.1,1});
        std::vector<std::string> warnings;
        ASSERT_EQ(gw.initialize(mesh,edges,options,{},1,config,warnings),"");
        context.options.start_date=datetime::encodeDate(2026,10,6);
        context.options.total_duration_ms=86400000;context.options.ignore_snow_melt=true;
        context.subcatches.resize(2);context.gages.resize(1);context.forcing.resize(0,0,2,1,0);
        context.gage_names.try_add("RG");context.tables.tables.resize(1);
        context.gages.ts_index[0]=0;context.gages.interval_sec[0]=30;
        context.tables.tables[0].x={context.options.start_date,datetime::addSeconds(context.options.start_date,30)};
        context.tables.tables[0].y={a.rain,0};
        if(delayed_rain) {
            context.gages.interval_sec[0]=1;
            context.tables.tables[0].x={context.options.start_date,datetime::addSeconds(context.options.start_date,1),datetime::addSeconds(context.options.start_date,2)};
            context.tables.tables[0].y={0,432,0};
        }
        for(int i=0;i<2;++i) {
            context.subcatch_names.try_add("S"+std::to_string(reverse ? 1-i : i));
            context.subcatches.gage[i]=0;context.subcatches.area[i]=1/(43560*ft2);
            context.subcatches.width[i]=1;context.subcatches.n_perv[i]=.1;
            context.subcatches.n_imperv[i]=.1;context.subcatches.frac_imperv[i]=a.impervious[i];
            context.forcing.subcatch_evap_mode[i]=ForcingMode::OVERRIDE;
            context.forcing.subcatch_evap_value[i]=a.pet[i];
            context.forcing.subcatch_evap_persist[i]=ForcingPersist::PERSIST;
            if(a.rain_override[i]>=0) {
                context.forcing.subcatch_rainfall_mode[i]=ForcingMode::OVERRIDE;
                context.forcing.subcatch_rainfall_value[i]=a.rain_override[i];
                context.forcing.subcatch_rainfall_persist[i]=ForcingPersist::PERSIST;
            }
            context.subcatches.infil_model[i]=2;context.subcatches.infil_p1[i]=3.5;
            context.subcatches.infil_p2[i]=.5;context.subcatches.infil_p3[i]=.26;
        }
        if(trench) {
            context.lid_controls.names={"IT"};context.lid_controls.lid_type={a.barrel ? "RB" : "IT"};
            context.lid_controls.surface={{3,0,.1,0,0}};
            context.lid_controls.storage={{12,.4,a.lid_bottom,0}};
            context.lid_controls.drain={{0,.5,0,0,0,0}};
            context.lid_usage.subcatch_index={0};context.lid_usage.lid_index={0};
            context.lid_usage.number={2};context.lid_usage.area={a.lid_area/2/ft2};context.lid_usage.width={1};
            context.lid_usage.init_sat={a.lid_saturation};context.lid_usage.from_imperv={0};context.lid_usage.from_perv={0};
            context.lid_usage.to_perv={0};context.lid_usage.drain_to={""};
        }
        lid::LIDSolver initial;const lid::LIDSolver* snapshot=nullptr;
        if(a.covered) {
            initial.init(context);initial.group(static_cast<int>(lid::LIDType::RAIN_BARREL)).stor_covered[0]=1;snapshot=&initial;
        }
        ASSERT_EQ(sources.initialize(context,{0,1},{{0,trench ? 1-a.lid_area : 1},{1,1}},snapshot),"");
        preview.mesh_weather_area.assign(cells,4-2./cells);
        for(int sc=0;sc<2;++sc) {
            SurfaceOwnerObject o;o.subcatch=sc;o.reviewed=true;preview.objects.push_back(o);
            const double non_lid=trench && sc==0 ? 1-a.lid_area : 1;
            for(int i=0;i<cells;++i) preview.shares.push_back({sc,i,1./cells,non_lid*(1-a.impervious[sc])/cells,
                non_lid*a.impervious[sc]/cells,(trench && sc==0 ? a.lid_area : 0)/cells,0});
        }
        for(int i=0;i<cells;++i) {surface.volume[i]=.08;surface.depth[i]=.02;surface.head[i]=10.02;}
    }
    void room(double volume,int cell=0) {
        const double L=gw.state().zs[cell]-gw.state().hg[cell];
        gw.state().hu[cell]=gw.state().theta_s[cell]*L-volume/mesh.tri_area[cell];
    }
    void attach(bool march=false) {
        ASSERT_EQ(stage.initialize(mesh,surface,options,gw,sources,preview),"");
        if(march) {marcher.initialize(mesh,surface,options);marcher.setSubsurface(&gw);marcher.setCommonSourceStage(stage);}
    }
    double receipts(SurfaceDonorKind kind) const {
        double v=0;for(const auto& r:stage.receipts()) if(r.request.kind==kind)v+=r.volume;return v;
    }
};
void etBalance(const Model& m) {
    const auto& s=m.gw.state();
    for(int i=0;i<s.n_cells;++i)
        EXPECT_NEAR(s.et_potential_cumulative[i],s.et_surface_cumulative[i]+s.et_soil_cumulative[i]+
            s.et_unused_cumulative[i]+s.et_pending[i],1e-13);
    for(const auto& r:m.stage.etReceipts())
        EXPECT_NEAR(r.potential,r.evaporation+r.soil_demand+r.unused,1e-14);
}
}

TEST(CommonSourceStage, MeshAndTwoIndependentSourcesShareScarceReceiverBeforeDebits) {
    Model m;m.setup();m.room(.001);m.attach();
    const double capacity=m.gw.sourceInfiltrationCapacity(0,.02,1)*2;
    const double initial=m.surface.volume[0];ASSERT_EQ(m.stage.advance(0,1),"");
    const double mesh=m.receipts(SurfaceDonorKind::MESH),source=m.receipts(SurfaceDonorKind::NON_LID);
    EXPECT_GT(mesh,0);EXPECT_GT(source,0);EXPECT_NEAR(mesh+source,.001,1e-12);
    EXPECT_NEAR(m.gw.state().xacc_from_surface[0],mesh+source,1e-15);
    EXPECT_NEAR(m.surface.volume[0],initial-mesh,1e-15);
    ASSERT_EQ(m.sources.clocks().groups().size(),2u);
    for(int sc=0;sc<2;++sc) {
        EXPECT_DOUBLE_EQ(m.sources.clocks().groups()[sc].completed_end,1);
        EXPECT_NEAR(m.sources.balanceResidual(sc),0,1e-10);
        EXPECT_GT(m.sources.runoff().soa().depth_perv[sc],0);
        EXPECT_DOUBLE_EQ(m.context.subcatches.stat_evap_vol[sc],0);
    }
    EXPECT_NEAR(m.sources.ledgers()[0].infiltration*ft3,source/2,1e-15);
    EXPECT_NEAR(m.sources.ledgers()[1].infiltration*ft3,source/2,1e-15);
    // Shared intake is proportional to the three bounded candidate demands.
    EXPECT_NEAR(mesh/.001,capacity/(capacity+2*.01*.3048),1e-10);
    const auto dir=std::filesystem::path(OPENSWMM_R4_STAGE_OUT);std::filesystem::create_directories(dir);
    std::ofstream f(dir/"common-stage.json");f<<std::setprecision(17)
        <<"{\"interval_end\":1,\"receiver_headroom\":0.001,\"mesh_receipt\":"<<mesh
        <<",\"non_lid_receipts\":"<<source<<",\"receiver_booking\":"<<m.gw.state().xacc_from_surface[0]
        <<",\"source_groups\":2,\"runtime_activated\":false}\n";
}

TEST(CommonSourceStage, UnusedIntervalAwardsReturnToReceiverWithoutExtraDonorWithdrawal) {
    Model m;m.setup(false,1,false,false,true);m.room(.001);m.attach();
    ASSERT_EQ(m.stage.advance(0,2),"");
    // Rain arrives only in the second forcing segment. A uniform interval
    // ceiling cannot consume the award while the first segment is dry.
    const double source=m.receipts(SurfaceDonorKind::NON_LID);
    const double total=source+m.receipts(SurfaceDonorKind::MESH);
    EXPECT_GT(source,0);EXPECT_LT(total,.001);
    EXPECT_NEAR(m.gw.state().xacc_from_surface[0],total,1e-15);
    EXPECT_GT(m.gw.infiltrationHeadroom(0),0);
    EXPECT_NEAR((m.sources.ledgers()[0].infiltration+m.sources.ledgers()[1].infiltration)*ft3,source,1e-15);
    EXPECT_NEAR(m.sources.balanceResidual(0),0,1e-10);EXPECT_NEAR(m.sources.balanceResidual(1),0,1e-10);
}

TEST(CommonSourceStage, SaturatedReceiverKeepsDeniedWaterInMeshAndBothSources) {
    Model m;m.setup();m.room(0);m.attach();ASSERT_EQ(m.stage.advance(0,1),"");
    EXPECT_DOUBLE_EQ(m.gw.state().xacc_from_surface[0],0);EXPECT_DOUBLE_EQ(m.surface.volume[0],.08);
    for(int sc=0;sc<2;++sc) {
        EXPECT_DOUBLE_EQ(m.sources.ledgers()[sc].infiltration,0);
        EXPECT_NEAR(m.sources.runoff().soa().depth_perv[sc],.01,1e-10);
        EXPECT_NEAR(m.sources.balanceResidual(sc),0,1e-10);
    }
}

TEST(CommonSourceStage, StableNamesGiveTheSameAwardsAfterSourceOrderChanges) {
    std::map<std::string,double> prior;
    for(bool reverse:{false,true}) {
        Model m;m.setup(false,1,reverse);m.room(.001);m.attach();ASSERT_EQ(m.stage.advance(0,1),"");
        std::map<std::string,double> now;for(const auto& r:m.stage.receipts())now[r.request.source]+=r.volume;
        if(!reverse)prior=now;else EXPECT_EQ(prior,now);
    }
}

TEST(CommonSourceStage, NativeLidBottomAndNonLidCompeteWithMeshAndRemainPaired) {
    Model m;m.setup(true);m.room(.001);m.attach();const double initial=m.sources.lids().storedVolume();
    ASSERT_EQ(m.stage.advance(0,1),"");
    const double bottom=m.receipts(SurfaceDonorKind::LID_BOTTOM);EXPECT_GT(bottom,0);
    const int t=static_cast<int>(lid::LIDType::INFIL_TRENCH);const auto& g=m.sources.lids().group(t);
    EXPECT_NEAR(g.wb_infil[0]*g.area[0]*ft3,bottom,1e-15);
    EXPECT_GT(m.sources.lids().storedVolume(),initial);
    EXPECT_NEAR(m.receipts(SurfaceDonorKind::MESH)+m.receipts(SurfaceDonorKind::NON_LID)+bottom,.001,1e-12);
    EXPECT_NEAR(m.sources.balanceResidual(0),0,1e-10);EXPECT_NEAR(m.sources.balanceResidual(1),0,1e-10);
}

TEST(CommonSourceStage, DeniedCellShareNeverMigratesToAnotherReceiver) {
    Model m;m.setup(false,2);m.room(0,0);m.room(.1,1);m.attach();
    const double c0=m.gw.sourceInfiltrationCapacity(0,0,1),c1=m.gw.sourceInfiltrationCapacity(1,0,1);
    ASSERT_EQ(m.stage.advance(0,1),"");
    EXPECT_DOUBLE_EQ(m.gw.state().xacc_from_surface[0],0);
    for(const auto& r:m.stage.receipts()) {
        if(r.request.cell==0)EXPECT_DOUBLE_EQ(r.volume,0);
        EXPECT_LE(r.volume,r.request.candidate);
    }
    const double received=m.receipts(SurfaceDonorKind::NON_LID);
    EXPECT_NEAR(received,2*.01*.3048*c1/(c0+c1),1e-10);
    EXPECT_GT(m.sources.runoff().soa().depth_perv[0],0);
}

TEST(CommonSourceStage, PendingNodeArrivalReducesHeadroomAndFutureExtractionDoesNotAddIt) {
    for(double pending:{-.0004,.0004}) {
        Model m;m.setup();m.room(.001);ASSERT_EQ(m.gw.state().nacc.size(),1u);
        m.gw.state().nacc[0]=pending;m.attach();ASSERT_EQ(m.stage.advance(0,1),"");
        EXPECT_NEAR(m.gw.state().xacc_from_surface[0],pending<0 ? .0006 : .001,1e-12);
        EXPECT_DOUBLE_EQ(m.gw.state().nacc[0],pending);
    }
}

TEST(CommonSourceStage, MeshWeatherUsesOnlyRemainingAreaAndKeepsExistingEvaporationGate) {
    Model m;m.setup();m.room(0);m.surface.rainfall[0]=.001;m.surface.evap_rate[0]=.0001;m.attach();
    ASSERT_EQ(m.stage.advance(0,1),"");EXPECT_NEAR(m.stage.meshRain(),.002,1e-15);
    EXPECT_NEAR(m.stage.meshEvaporation(),.0002,1e-15);
    EXPECT_NEAR(m.surface.volume[0],.08+.002-.0002,1e-15);
    EXPECT_NEAR(m.surface.evap_loss_total,.0002,1e-15);
    EXPECT_NEAR(m.sources.ledgers()[0].rain*ft3,.003048,1e-10);
    EXPECT_NEAR(m.sources.ledgers()[1].rain*ft3,.003048,1e-10);
}

TEST(CommonSourceStage, ProfileFailureDoesNotInstallWaterOrAdvanceAnyClock) {
    Model m;m.setup();m.attach();m.surface.runtime_sources.rows.push_back({});
    EXPECT_FALSE(m.stage.advance(0,1).empty());EXPECT_DOUBLE_EQ(m.stage.completedEnd(),0);
    EXPECT_DOUBLE_EQ(m.sources.ledgers()[0].rain,0);EXPECT_DOUBLE_EQ(m.surface.volume[0],.08);
    EXPECT_DOUBLE_EQ(m.gw.state().xacc_from_surface[0],0);
    m.surface.runtime_sources.rows.clear();ASSERT_EQ(m.stage.advance(0,1),"");
    EXPECT_FALSE(m.stage.advance(0,1).empty());EXPECT_DOUBLE_EQ(m.stage.completedEnd(),1);
}

TEST(CommonSourceStage, UnsupportedOrIncompleteReviewedGroupsCannotAttach) {
    Model m;m.setup();m.preview.objects[1].reviewed=false;
    EXPECT_FALSE(m.stage.initialize(m.mesh,m.surface,m.options,m.gw,m.sources,m.preview).empty());
    m.preview.objects[1].reviewed=true;m.preview.objects[0].outside_area=.01;
    EXPECT_FALSE(m.stage.initialize(m.mesh,m.surface,m.options,m.gw,m.sources,m.preview).empty());
    m.preview.objects[0].outside_area=0;m.preview.mesh_weather_area[0]=3;
    EXPECT_FALSE(m.stage.initialize(m.mesh,m.surface,m.options,m.gw,m.sources,m.preview).empty());
    m.preview.mesh_weather_area[0]=2;m.attach();
}

TEST(CommonSourceStage, ActualMarcherSourcePhaseAdvancesDryPinnedSourcesAndTailTogether) {
    Model m;m.setup();m.surface.volume[0]=0;m.surface.depth[0]=0;m.surface.head[0]=10;
    m.surface.rainfall[0]=.001;m.attach(true);
    for(double end:{.3,1.,2.125,4.,4.000000000001}) {
        const double start=m.stage.completedEnd();EXPECT_DOUBLE_EQ(m.marcher.advance(start,end),end);
        EXPECT_DOUBLE_EQ(m.stage.completedEnd(),end);EXPECT_DOUBLE_EQ(m.marcher.completedSourceTime(),end);
        for(const auto& group:m.sources.clocks().groups())EXPECT_DOUBLE_EQ(group.completed_end,end);
        EXPECT_NEAR(m.sources.balanceResidual(0),0,1e-10);EXPECT_NEAR(m.sources.balanceResidual(1),0,1e-10);
        EXPECT_NEAR(m.gw.state().continuityResidual(),0,1e-10);
    }
    EXPECT_GT(m.stage.intervals(),4);EXPECT_NEAR(m.stage.meshRain(),.001*2*m.stage.completedEnd(),1e-14);
    EXPECT_GT(m.gw.state().led_infil_in,0);EXPECT_DOUBLE_EQ(m.gw.state().xacc_from_surface[0],0);
    m.marcher.finalize();
}

TEST(CommonSourceStage, RegistrationRequiresMatchingInitialFirstOrderMesh) {
    Model m;m.setup();m.attach();m.marcher.initialize(m.mesh,m.surface,m.options);
    EXPECT_THROW(m.marcher.setCommonSourceStage(m.stage),std::invalid_argument);
    m.marcher.setSubsurface(&m.gw);EXPECT_NO_THROW(m.marcher.setCommonSourceStage(m.stage));
    EXPECT_THROW(m.marcher.setCommonSourceStage(m.stage),std::invalid_argument);
    EXPECT_THROW(m.marcher.reinitialize(0),std::runtime_error);
    EXPECT_THROW(m.marcher.resyncFromVolumes(0),std::runtime_error);
    m.marcher.finalize();
    Model second;second.setup();second.options.momentum=Momentum2D::FULL_SWE;
    second.options.lts_tiers=1;second.options.reconstruction_order=2;second.attach();
    second.marcher.initialize(second.mesh,second.surface,second.options);second.marcher.setSubsurface(&second.gw);
    EXPECT_THROW(second.marcher.setCommonSourceStage(second.stage),std::invalid_argument);second.marcher.finalize();
}

TEST(CommonSourceStage, IncomingFaceWaterLandsBeforeDryMeshDonorRequestsAreGathered) {
    Model m;m.setup(false,2,false,true);m.surface.volume[1]=0;m.surface.depth[1]=0;m.surface.head[1]=10;
    m.attach(true);ASSERT_DOUBLE_EQ(m.marcher.advance(0,.1),.1);
    double into_dry=0,total=0;
    for(const auto& r:m.stage.receipts()) if(r.request.kind==SurfaceDonorKind::MESH) {
        total+=r.volume;if(r.request.cell==1)into_dry+=r.volume;
    }
    EXPECT_GT(into_dry,0);
    EXPECT_NEAR(m.surface.volume[0]+m.surface.volume[1]+total,.08,1e-13);
    EXPECT_NEAR(m.gw.state().continuityResidual(),0,1e-10);m.marcher.finalize();
}

TEST(CommonSourceStage, DryOwnerDemandUsesReviewedAreasAndOneSoilStressForQualifiedClosuresAndAllLaws) {
    for(auto cl:{GwClosure::CLOSED_FORM,GwClosure::ENSLAVED,GwClosure::SIGMA})
    for(auto law:{SoilChar::GARDNER,SoilChar::RUSSO,SoilChar::BROOKS_COREY,SoilChar::VAN_GENUCHTEN}) {
        AtmosphericInput a;a.et="BOUNDARY_ET";a.closure=cl;a.law=law;a.rain=0;a.pet={1e-5,3e-5};
        a.ks=0; // Isolate atmospheric extraction from recharge/table movement.
        Model m;m.setup(false,1,false,false,false,a);m.surface.volume[0]=0;m.surface.depth[0]=0;
        m.surface.evap_rate[0]=2e-6;m.attach();ASSERT_EQ(m.stage.advance(0,10),"");
        auto& s=m.gw.state();const double independent=10*((1e-5+3e-5)*.3048+2*2e-6);
        EXPECT_NEAR(s.et_potential_cumulative[0],independent,1e-15);
        EXPECT_NEAR(s.et_pending[0],independent,1e-15);EXPECT_DOUBLE_EQ(s.et_surface_cumulative[0],0);etBalance(m);
        double area=0;for(const auto& r:m.stage.etReceipts()){area+=r.area;EXPECT_DOUBLE_EQ(r.end,10);}
        EXPECT_NEAR(area,4,1e-15);const double initial=s.storage();m.gw.assignTiers(10,1);m.gw.fireGwCells(0,10,m.surface,10);
        EXPECT_NEAR(s.et_soil_cumulative[0],independent*s.et_stress[0],1e-12);
        EXPECT_NEAR(initial-s.storage(),s.led_et,1e-12);EXPECT_NEAR(s.continuityResidual(),0,1e-12);
        EXPECT_DOUBLE_EQ(s.et_refresh[0],10);etBalance(m);
    }
}

TEST(CommonSourceStage, WetImperviousEvaporationCannotConsumeDryPerviousDemand) {
    AtmosphericInput a;a.et="BOUNDARY_ET";a.pet={.0001,.0001};a.impervious={0,1};a.rain_override[0]=0;
    Model m;m.setup(false,1,false,false,false,a);m.surface.volume[0]=0;m.surface.depth[0]=0;m.attach();
    ASSERT_EQ(m.stage.advance(0,1),"");ASSERT_EQ(m.stage.advance(1,2),"");
    const auto& s=m.gw.state();EXPECT_NEAR(s.et_pending[0],.0001*.3048*2,1e-15);
    EXPECT_NEAR(s.et_surface_cumulative[0],.0001*.3048,1e-15);
    EXPECT_NEAR(s.et_unused_cumulative[0],.0001*.3048,1e-15);
    for(const auto& r:m.stage.etReceipts()) if(r.owner==SurfaceEtOwner::IMPERVIOUS){
        EXPECT_GT(r.evaporation,0);EXPECT_DOUBLE_EQ(r.soil_demand,0);
    }
    EXPECT_NEAR(m.sources.runoff().soa().actual_perv_evap_vol[1],0,1e-15);etBalance(m);
}
TEST(CommonSourceStage, MixedSourceDoesNotUseLegacyImperviousCarryInAsPerviousEvaporation) {
    AtmosphericInput a;a.pet={.0001,0};a.impervious={.4,0};a.rain_override[1]=0;
    Model m;m.setup(false,1,false,false,false,a);m.surface.volume[0]=0;m.surface.depth[0]=0;m.attach();
    ASSERT_EQ(m.stage.advance(0,1),"");ASSERT_EQ(m.stage.advance(1,2),"");
    EXPECT_GT(m.sources.runoff().soa().perv_evap_vol[0],0);
    EXPECT_NEAR(m.sources.runoff().soa().actual_perv_evap_vol[0],0,1e-15);
    double potential=0,pervious=0,impervious=0;
    for(const auto& r:m.stage.etReceipts())if(r.source==0){
        potential+=r.potential;
        if(r.owner==SurfaceEtOwner::PERVIOUS){pervious+=r.soil_demand;EXPECT_NEAR(r.evaporation,0,1e-15);}
        if(r.owner==SurfaceEtOwner::IMPERVIOUS)impervious+=r.evaporation;
    }
    EXPECT_NEAR(potential,.0001*.3048,1e-15);EXPECT_NEAR(pervious,potential*.6,1e-15);
    EXPECT_NEAR(impervious,potential*.4,1e-15);etBalance(m);
}

TEST(CommonSourceStage, OpenAndSealedLidStoresPayTheirOwnDemandBeforeSoil) {
    for(double bottom:{0.,432.})for(double saturation:{0.,.005,50.}) {
        AtmosphericInput a;a.rain=0;a.pet={.0001,0};a.lid_bottom=bottom;a.lid_saturation=saturation;
        Model m;m.setup(true,1,false,false,false,a);m.surface.volume[0]=0;m.surface.depth[0]=0;m.room(0);m.attach();
        const double stored=m.sources.lids().storedVolume()*ft3;ASSERT_EQ(m.stage.advance(0,1),"");
        double lid_evap=0,lid_soil=0,lid_unused=0;
        for(const auto& r:m.stage.etReceipts()) if(r.owner==SurfaceEtOwner::LID){
            EXPECT_NEAR(r.area,.5,1e-15);lid_evap+=r.evaporation;lid_soil+=r.soil_demand;lid_unused+=r.unused;
        }
        const double expected=.0001*.3048*.5;
        if(saturation>0){
            EXPECT_NEAR(lid_evap,std::min(expected,stored),1e-15);
            EXPECT_NEAR(lid_soil+lid_unused,expected-lid_evap,1e-15);
            if(bottom>0)EXPECT_DOUBLE_EQ(lid_unused,0);else EXPECT_DOUBLE_EQ(lid_soil,0);
        }
        else if(bottom>0){EXPECT_DOUBLE_EQ(lid_evap,0);EXPECT_NEAR(lid_soil,expected,1e-15);EXPECT_DOUBLE_EQ(lid_unused,0);}
        else{EXPECT_DOUBLE_EQ(lid_evap,0);EXPECT_DOUBLE_EQ(lid_soil,0);EXPECT_NEAR(lid_unused,expected,1e-15);}
        const auto& g=m.sources.lids().group(static_cast<int>(lid::LIDType::INFIL_TRENCH));
        EXPECT_NEAR(g.wb_evap[0]*g.area[0]*ft3,lid_evap,1e-15);
        EXPECT_NEAR(m.sources.balanceResidual(0),0,1e-10);etBalance(m);
    }
}

TEST(CommonSourceStage, FullLidFootprintDoesNotAddAnExtraNonLidAtmosphericBudget) {
    AtmosphericInput a;a.rain=0;a.pet={.0001,0};a.lid_area=1;a.lid_saturation=0;a.lid_bottom=0;
    Model m;m.setup(true,1,false,false,false,a);m.surface.volume[0]=0;m.surface.depth[0]=0;m.attach();
    ASSERT_EQ(m.stage.advance(0,1),"");EXPECT_NEAR(m.gw.state().et_potential_cumulative[0],.0001*.3048,1e-15);
    EXPECT_NEAR(m.gw.state().et_unused_cumulative[0],.0001*.3048,1e-15);
    EXPECT_DOUBLE_EQ(m.gw.state().et_pending[0],0);etBalance(m);
}

TEST(CommonSourceStage, CoveredAndUncoveredBarrelRemaindersExpireOnTheirOwnFootprint) {
    for(bool covered:{false,true}) {
        AtmosphericInput a;a.rain=0;a.pet={.0001,0};a.barrel=true;a.covered=covered;a.lid_bottom=0;
        Model m;m.setup(true,1,false,false,false,a);m.surface.volume[0]=0;m.surface.depth[0]=0;m.attach();
        ASSERT_EQ(m.stage.advance(0,1),"");
        EXPECT_NEAR(m.gw.state().et_pending[0],.0001*.3048*.5,1e-15);
        EXPECT_NEAR(m.gw.state().et_unused_cumulative[0],.0001*.3048*.5,1e-15);
        for(const auto& r:m.stage.etReceipts()) if(r.owner==SurfaceEtOwner::LID){
            EXPECT_DOUBLE_EQ(r.soil_demand,0);EXPECT_DOUBLE_EQ(r.evaporation,0);EXPECT_GT(r.unused,0);
        }
        EXPECT_NEAR(m.sources.balanceResidual(0),0,1e-10);etBalance(m);
    }
}

TEST(CommonSourceStage, FailedIntervalCannotInstallAnyAtmosphericReceiptOrFutureDemand) {
    AtmosphericInput a;a.rain=0;a.pet={.0001,.0002};Model m;m.setup(false,1,false,false,false,a);m.attach();
    m.surface.runtime_sources.rows.push_back({});EXPECT_FALSE(m.stage.advance(0,1).empty());
    EXPECT_TRUE(m.stage.etReceipts().empty());EXPECT_DOUBLE_EQ(m.gw.state().et_potential_cumulative[0],0);
    for(const auto& r:m.sources.atmosphere())EXPECT_DOUBLE_EQ(r.potential,0);
    m.surface.runtime_sources.rows.clear();ASSERT_EQ(m.stage.advance(0,1),"");
    const auto receipts=m.stage.etReceipts();const double past=m.gw.state().et_pending[0];
    EXPECT_FALSE(m.stage.advance(1,100000).empty());EXPECT_DOUBLE_EQ(m.gw.state().et_pending[0],past);
    ASSERT_EQ(m.stage.etReceipts().size(),receipts.size());
    for(std::size_t j=0;j<receipts.size();++j)EXPECT_DOUBLE_EQ(m.stage.etReceipts()[j].potential,receipts[j].potential);
    etBalance(m);
}

TEST(CommonSourceStage, OwnerSpecificDemandKeepsCellProvenanceAcrossSourceOrderAndCadence) {
    std::map<std::string,double> prior;
    for(bool reverse:{false,true}) {
        AtmosphericInput a;a.rain=0;a.pet=reverse ? std::array<double,2>{3e-5,1e-5} : std::array<double,2>{1e-5,3e-5};
        Model m;m.setup(false,2,reverse,false,false,a);for(int i=0;i<2;++i){m.surface.volume[i]=0;m.surface.depth[i]=0;}
        m.attach();for(double end:{.1,1.,2.125,4.})ASSERT_EQ(m.stage.advance(m.stage.completedEnd(),end),"");
        std::map<std::string,double> now;
        for(const auto& r:m.sources.atmosphere())now[m.sources.context().subcatch_names.name_of(r.source)]+=r.potential;
        if(!reverse)prior=now;else EXPECT_EQ(prior,now);
        for(const auto& r:m.stage.etReceipts()){
            EXPECT_EQ(r.start,2.125);EXPECT_EQ(r.end,4);if(r.source>=0)EXPECT_NEAR(r.area,.5,1e-15);
        }
        for(int i=0;i<2;++i)EXPECT_NEAR(m.gw.state().et_pending[i],4*4e-5*.3048*.5,1e-15);
        m.gw.assignTiers(4,1);m.gw.fireGwCells(0,4,m.surface,4);etBalance(m);
        for(int i=0;i<2;++i){EXPECT_DOUBLE_EQ(m.gw.state().et_pending[i],0);EXPECT_DOUBLE_EQ(m.gw.state().et_soil_cumulative[i],0);}
    }
}

TEST(CommonSourceStage, MarcherConsumesPastSoilDemandWithoutDuplicatingMeshOrSourceEvaporation) {
    AtmosphericInput a;a.rain=0;a.pet={1e-5,3e-5};a.et="BOUNDARY_ET";
    Model m;m.setup(false,1,false,false,false,a);m.surface.volume[0]=0;m.surface.depth[0]=0;
    m.surface.evap_rate[0]=2e-6;m.attach(true);
    for(double end:{.3,1.,2.125,4.}){
        const auto start=m.stage.completedEnd();EXPECT_DOUBLE_EQ(m.marcher.advance(start,end),end);etBalance(m);
        EXPECT_NEAR(m.gw.state().et_potential_cumulative[0],end*(4e-5*.3048+4e-6),1e-15);
        EXPECT_NEAR(m.gw.state().continuityResidual(),0,1e-11);
    }
    EXPECT_GT(m.gw.state().et_soil_cumulative[0],0);EXPECT_DOUBLE_EQ(m.gw.state().et_pending[0],0);
    EXPECT_DOUBLE_EQ(m.surface.evap_loss_total,0);m.marcher.finalize();
}
TEST(CommonSourceStage, ExistingEtModesConsumeOnlyEligibleCompletedDemand) {
    for(const std::string mode:{"NONE","BOUNDARY_ET","CAPILLARY_RISE","BOTH"}) {
        AtmosphericInput a;a.rain=0;a.pet={.0001,0};a.impervious={.4,0};a.et=mode;a.ks=0;
        Model m;m.setup(false,1,false,false,false,a);m.surface.volume[0]=0;m.surface.depth[0]=0;m.attach();
        ASSERT_EQ(m.stage.advance(0,1),"");m.gw.assignTiers(1,1);m.gw.fireGwCells(0,1,m.surface,1);
        if(mode=="BOUNDARY_ET"||mode=="BOTH")EXPECT_GT(m.gw.state().led_et,0);
        else {EXPECT_DOUBLE_EQ(m.gw.state().led_et,0);EXPECT_NEAR(m.gw.state().et_unused_cumulative[0],.0001*.3048,1e-15);}
        EXPECT_DOUBLE_EQ(m.gw.state().et_pending[0],0);etBalance(m);
    }
}
TEST(CommonSourceStage, ComponentAreasMustCloseEvenWhenTotalWeatherCoverageCloses) {
    AtmosphericInput a;a.impervious={.4,0};Model m;m.setup(false,1,false,false,false,a);
    m.preview.shares[0].impervious_area=.5;
    EXPECT_FALSE(m.stage.initialize(m.mesh,m.surface,m.options,m.gw,m.sources,m.preview).empty());
    EXPECT_DOUBLE_EQ(m.gw.state().et_potential_cumulative[0],0);
    m.preview.shares[0].impervious_area=.4;m.attach();
    Model split;split.setup(false,2,false,false,false,a);
    // Component totals remain correct, but one cell would borrow its peer's area.
    split.preview.shares[0].impervious_area+=.1;split.preview.shares[1].impervious_area-=.1;
    EXPECT_FALSE(split.stage.initialize(split.mesh,split.surface,split.options,split.gw,split.sources,split.preview).empty());
}
TEST(CommonSourceStage, QualifiedSigmaMarchesCompletedOwnerEtWithMovingTable) {
    for(auto law:{SoilChar::GARDNER,SoilChar::RUSSO,SoilChar::BROOKS_COREY,SoilChar::VAN_GENUCHTEN}) {
        AtmosphericInput a;a.closure=GwClosure::SIGMA;a.law=law;a.rain=0;
        a.et="BOUNDARY_ET";a.pet={1e-5,3e-5};a.ks=.01;
        Model m;m.setup(false,1,false,false,false,a);
        m.surface.volume[0]=0;m.surface.depth[0]=0;m.surface.head[0]=10;
        m.surface.evap_rate[0]=2e-6;const double initial=m.gw.state().storage();
        m.attach(true);
        for(double end:{.3,1.,2.125,4.,10.,20.}) {
            const double start=m.stage.completedEnd();EXPECT_DOUBLE_EQ(m.marcher.advance(start,end),end);
            etBalance(m);EXPECT_NEAR(m.gw.state().continuityResidual(),0,2e-11);
            EXPECT_NEAR(initial-m.gw.state().storage(),m.gw.state().led_et,2e-11);
            EXPECT_NEAR(m.gw.state().led_dunne,0,2e-11);EXPECT_EQ(m.gw.nodeRefunds(),0);
            EXPECT_NEAR(m.gw.state().et_potential_cumulative[0],end*(4e-5*.3048+4e-6),1e-14);
        }
        EXPECT_GT(m.gw.state().et_soil_cumulative[0],0);EXPECT_DOUBLE_EQ(m.surface.evap_loss_total,0);
        m.marcher.finalize();
    }
}
