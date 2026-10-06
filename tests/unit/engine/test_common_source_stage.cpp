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
struct Model {
    MeshData mesh; SurfaceStateData surface; SolverOptions2D options;
    InertialEdges edges; SubsurfaceConfig config; SubsurfaceSolver gw;
    SimulationContext context; runoff::SourceWaterDriver sources;
    SurfaceOwnershipPreview preview; CommonSourceStage stage;
    ExplicitInertialSolver marcher;
    void setup(bool trench=false,int cells=1,bool reverse=false,bool adjacent=false,bool delayed_rain=false) {
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
        config.options.gw_et="NONE";GwAquiferRow row;row.Ks=.01;row.zs=2;row.hg0=1;
        config.rows.push_back(row);config.node_beds.push_back({0,0,.01,.1,1});
        std::vector<std::string> warnings;
        ASSERT_EQ(gw.initialize(mesh,edges,options,{},1,config,warnings),"");
        context.options.start_date=datetime::encodeDate(2026,10,6);
        context.options.total_duration_ms=86400000;context.options.ignore_snow_melt=true;
        context.subcatches.resize(2);context.gages.resize(1);context.forcing.resize(0,0,2,1,0);
        context.gage_names.try_add("RG");context.tables.tables.resize(1);
        context.gages.ts_index[0]=0;context.gages.interval_sec[0]=30;
        context.tables.tables[0].x={context.options.start_date,datetime::addSeconds(context.options.start_date,30)};
        context.tables.tables[0].y={432,0};
        if(delayed_rain) {
            context.gages.interval_sec[0]=1;
            context.tables.tables[0].x={context.options.start_date,datetime::addSeconds(context.options.start_date,1),datetime::addSeconds(context.options.start_date,2)};
            context.tables.tables[0].y={0,432,0};
        }
        for(int i=0;i<2;++i) {
            context.subcatch_names.try_add("S"+std::to_string(reverse ? 1-i : i));
            context.subcatches.gage[i]=0;context.subcatches.area[i]=1/(43560*ft2);
            context.subcatches.width[i]=1;context.subcatches.n_perv[i]=.1;
            context.subcatches.infil_model[i]=2;context.subcatches.infil_p1[i]=3.5;
            context.subcatches.infil_p2[i]=.5;context.subcatches.infil_p3[i]=.26;
        }
        if(trench) {
            context.lid_controls.names={"IT"};context.lid_controls.lid_type={"IT"};
            context.lid_controls.surface={{3,0,.1,0,0}};
            context.lid_controls.storage={{12,.4,432,0}};
            context.lid_controls.drain={{0,.5,0,0,0,0}};
            context.lid_usage.subcatch_index={0};context.lid_usage.lid_index={0};
            context.lid_usage.number={2};context.lid_usage.area={.25/ft2};context.lid_usage.width={1};
            context.lid_usage.init_sat={50};context.lid_usage.from_imperv={0};context.lid_usage.from_perv={0};
            context.lid_usage.to_perv={0};context.lid_usage.drain_to={""};
        }
        ASSERT_EQ(sources.initialize(context,{0,1},{{0,trench ? .5 : 1},{1,1}}),"");
        preview.mesh_weather_area.assign(cells,4-2./cells);
        for(int sc=0;sc<2;++sc) {
            SurfaceOwnerObject o;o.subcatch=sc;o.reviewed=true;preview.objects.push_back(o);
            for(int i=0;i<cells;++i) preview.shares.push_back({sc,i,1./cells,(trench && sc==0 ? .5 : 1)/cells,0,(trench && sc==0 ? .5 : 0)/cells,0});
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
