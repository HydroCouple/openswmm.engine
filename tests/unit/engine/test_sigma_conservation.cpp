// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include "2d/mesh/MeshBuilder.hpp"
#include "2d/solver/InertialEdges.hpp"
#include "2d/subsurface/SubsurfaceSolver.hpp"
#include "2d/subsurface/SigmaColumn.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include "2d/data/SolverOptions2D.hpp"
#include <cmath>
using namespace openswmm::twoD;
namespace {
constexpr auto laws={SoilChar::GARDNER,SoilChar::RUSSO,SoilChar::BROOKS_COREY,SoilChar::VAN_GENUCHTEN};
struct Rig {
    MeshData mesh;InertialEdges edges;SurfaceStateData surf;SolverOptions2D opts;
    SubsurfaceConfig cfg;SubsurfaceSolver gw;
    void init(SoilChar law,double table=1,double ks=.01,GwUnitFactors units={},bool capillary=false,int layers=8) {
        mesh.resize_vertices(3);mesh.vx={0,4,0};mesh.vy={0,0,2};mesh.vz={10,10,10};
        mesh.resize_triangles(1);mesh.set_triangle(0,0,1,2);mesh.mannings_n[0]=.03;
        buildMeshTopology(mesh);edges.build(mesh);surf.resize(1,mesh.n_edge_slots());
        cfg.options.authored=true;cfg.options.closure=GwClosure::SIGMA;cfg.options.force_closed_form=true;
        cfg.options.capillary_diff=capillary;cfg.options.m_layers=layers;cfg.options.soil_char=law;cfg.options.gw_et="BOUNDARY_ET";
        GwAquiferRow row;row.Ks=ks/units.rate;row.zs=2/units.length;row.hg0=table/units.length;
        row.alpha=2/units.inv_len;row.psi_b=.2/units.length;cfg.rows={row};
        cfg.node_beds.push_back({0,0,.01,.1,1});std::vector<std::string> warnings;
        ASSERT_EQ(gw.initialize(mesh,edges,opts,units,1,cfg,warnings),"");
    }
    void fire(double dt,double time=0){gw.assignTiers(dt,1);gw.fireGwCells(0,dt,surf,time);}
    void check()const {
        const auto& s=gw.state();EXPECT_NEAR(s.continuityResidual(),0,2e-11);
        EXPECT_GE(s.hg[0],0);EXPECT_LE(s.hg[0],s.zs[0]);
        for(double th:s.theta_sigma){EXPECT_GE(th,s.theta_r[0]-1e-13);EXPECT_LE(th,s.theta_s[0]+1e-13);}
        EXPECT_NEAR(s.hu[0],sigma::columnStorage(s.theta_sigma.data(),s.m_layers,1,s.zs[0]-s.hg[0]),1e-13);
        EXPECT_NEAR(s.et_potential_cumulative[0],s.et_surface_cumulative[0]+s.et_soil_cumulative[0]+s.et_unused_cumulative[0]+s.et_pending[0],1e-13);
    }
};
}
TEST(SigmaConservation, ClosedHighConductivityCannotCreateWaterOrSaturationExcess) {
    for(auto law:laws)for(double table:{0.,1.,1.999999,2.})for(double dt:{.1,1.,10.}) {
        SCOPED_TRACE(std::to_string(int(law))+"/"+std::to_string(table)+"/"+std::to_string(dt));
        Rig r;r.init(law,table);const double initial=r.gw.state().storage();
        for(int k=0;k<10;++k){r.fire(dt,k*dt);r.check();}
        EXPECT_NEAR(r.gw.state().storage(),initial,2e-11);EXPECT_NEAR(r.gw.state().led_dunne,0,2e-11);
    }
}
TEST(SigmaConservation, ClosedMovingTableAndEtSpendOnlyTheSameInitialWater) {
    for(auto law:laws)for(double dt:{.1,1.,10.}) {
        Rig r;r.init(law);const double initial=r.gw.state().storage();
        for(int k=0;k<10;++k){r.gw.bookSurfaceEt(0,dt,4.0476e-6,0);r.fire(dt,k*dt);r.check();}
        EXPECT_GT(r.gw.state().led_et,0);EXPECT_NEAR(initial-r.gw.state().storage(),r.gw.state().led_et,2e-11);
        EXPECT_NEAR(r.gw.state().led_dunne,0,2e-11);
    }
}
TEST(SigmaConservation, IntakeEtAndNumericalRejectionRemainPaired) {
    for(auto law:laws)for(double table:{1.,1.999999,2.}) {
        Rig r;r.init(law,table);double offered=0;
        for(int k=0;k<30;++k){
            const double dt=k%2 ? 1 : .25;const double input=k%3 ? .0001 : .02;
            offered+=input;r.gw.state().xacc_from_surface[0]+=input;
            r.gw.bookSurfaceEt(0,dt,1e-5,0);r.fire(dt,k);r.check();
        }
        EXPECT_NEAR(r.gw.state().led_infil_in,offered,1e-13);
        EXPECT_NEAR(r.gw.state().xacc_to_surface[0],r.gw.state().led_reject+r.gw.state().led_dunne,1e-13);
    }
}
TEST(SigmaConservation, SignedNodeAndLinkReceiptsShareMovingTableStorage) {
    for(auto law:laws)for(double node:{-.0005,.0005})for(double link:{-.0004,.0004})for(double table:{1.,1.999999,2.}) {
        Rig r;r.init(law,table);r.gw.state().c_loss[0]=.001;
        for(int k=0;k<20;++k){
            r.gw.state().nacc[0]=node;r.gw.bookLinkSeepage(0,link);
            r.gw.bookSurfaceEt(0,1,1e-5,0);r.fire(1,k);r.check();
            EXPECT_NEAR(r.gw.state().qnode_last[0],node,1e-15);
        }
        EXPECT_EQ(r.gw.nodeRefunds(),0);EXPECT_NEAR(r.gw.state().led_node,node*20,1e-13);
        EXPECT_NEAR(r.gw.state().led_link,link*20,1e-13);
    }
}
TEST(SigmaConservation, DeepLossCannotSpendWaterNeededForResidualStorage) {
    for(auto law:laws) {
        Rig r;r.init(law);r.gw.state().c_loss[0]=1;
        bool reached_floor=false;
        for(int k=0;k<4;++k){
            const double requested=r.gw.state().hg[0]/r.gw.state().zs[0]*10*4;
            const double before=r.gw.state().led_deep;r.fire(10,k*10);r.check();
            reached_floor|=r.gw.state().hg[0]<1e-12;
            EXPECT_GE(r.gw.state().qdeep_last[0],0);
            EXPECT_LE(r.gw.state().led_deep-before,requested+1e-13);
            EXPECT_GE(r.gw.state().storage(),4*2*r.gw.state().theta_r[0]-1e-12);
        }
        EXPECT_GT(r.gw.state().led_deep,0);EXPECT_TRUE(reached_floor);
    }
}
TEST(SigmaConservation, UsAndSiInputsGiveTheSamePhysicalMovingTable) {
    for(auto law:laws) {
        Rig si,us;si.init(law);GwUnitFactors units;units.length=.3048;units.inv_len=1/.3048;units.rate=.0254/3600;
        us.init(law,1,.01,units);
        for(int k=0;k<30;++k){
            for(auto* r:{&si,&us}){r->gw.state().xacc_from_surface[0]+=.001;r->gw.bookSurfaceEt(0,1,1e-5,0);r->fire(1,k);r->check();}
            EXPECT_NEAR(si.gw.state().hg[0],us.gw.state().hg[0],1e-12);
            EXPECT_NEAR(si.gw.state().hu[0],us.gw.state().hu[0],1e-12);
        }
    }
}

TEST(SigmaConservation, CapillarySweepAndLayerCountsRetainTheSameWaterBudget) {
    for(auto law:laws)for(int layers:{2,4,8,32})for(double table:{0.,1.,1.999999,2.}) {
        Rig r;r.init(law,table,.01,{},true,layers);
        for(int k=0;k<20;++k){
            r.gw.state().xacc_from_surface[0]+=.001;r.gw.bookSurfaceEt(0,.1,1e-5,0);
            r.gw.bookLinkSeepage(0,.0001);r.fire(.1,k*.1);r.check();
        }
    }
}
TEST(SigmaConservation, SignedWellsAndPairedInertMassCloseAtMovingAndFullTables) {
    for(auto law:laws)for(double table:{1.,1.999999,2.}) {
        Rig r;r.init(law,table);RowLayoutLite rows;rows.n_species=1;rows.n_pollut=1;rows.names={"P"};
        std::vector<std::string> warnings;r.gw.initTransport(rows,nullptr,{},warnings);
        auto& t=r.gw.transport();const auto& w=r.gw.state();
        t.sat_mass[0]=w.hg[0]*w.theta_s[0]*4*3;t.unsat_mass[0]=w.hu[0]*4*2;
        t.init_mass[0]=t.sat_mass[0]+t.unsat_mass[0];
        GwResolvedSource source;source.cells={{0,1}};source.flow.times={0,5,10,15,20};
        source.flow.values={.02,.02,-.01,-.01,.02};
        GwResolvedSourceTerm term;term.row=0;term.signal.constant=4;source.terms={term};
        r.gw.setSources({source});r.gw.state().c_loss[0]=.001;
        for(int k=0;k<80;++k){
            r.gw.state().xacc_from_surface[0]+=.002;t.xacc_from_surface[0]+=.002*5;
            r.gw.bookSurfaceEt(0,.25,1e-5,0);r.fire(.25,k*.25);r.check();
            EXPECT_NEAR(t.residual(0),0,1e-11);EXPECT_GE(t.sat_mass[0],0);EXPECT_GE(t.unsat_mass[0],0);
            EXPECT_NEAR(t.xacc_to_surface[0],t.lost_dunne[0],1e-12);
        }
        EXPECT_GT(t.gained_source[0],0);EXPECT_GT(t.lost_source[0],0);
        EXPECT_DOUBLE_EQ(t.lost_et[0],0);
        if(table>1.99){EXPECT_GT(w.led_dunne,0);EXPECT_GT(t.lost_dunne[0],0);}
    }
}
TEST(SigmaConservation, LateralWaterAndMassRemainConservativeAcrossDifferentTiers) {
    for(auto law:laws)for(int tiers:{1,4}) {
        SCOPED_TRACE(std::to_string(int(law))+"/tiers="+std::to_string(tiers));
        MeshData mesh;mesh.resize_vertices(4);mesh.vx={0,4,0,4};mesh.vy={0,0,2,2};mesh.vz={10,10,10,10};
        mesh.resize_triangles(2);mesh.set_triangle(0,0,1,2);mesh.set_triangle(1,1,3,2);
        mesh.mannings_n={.03,.03};buildMeshTopology(mesh);InertialEdges edges;edges.build(mesh);
        SurfaceStateData surf;surf.resize(2,mesh.n_edge_slots());SolverOptions2D opts;opts.lts_tiers=tiers;
        SubsurfaceConfig cfg;cfg.options.authored=true;cfg.options.force_closed_form=true;
        cfg.options.closure=GwClosure::SIGMA;cfg.options.soil_char=law;
        GwAquiferRow a;a.Ks=.01;a.zs=2;a.hg0=1.5;cfg.rows.push_back(a);
        a.scope=2;a.cell=1;a.hg0=.5;cfg.rows.push_back(a);SubsurfaceSolver gw;std::vector<std::string> warnings;
        ASSERT_EQ(gw.initialize(mesh,edges,opts,{},0,cfg,warnings),"");
        RowLayoutLite rows;rows.n_species=1;rows.n_pollut=1;rows.names={"P"};gw.initTransport(rows,nullptr,{},warnings);
        auto& t=gw.transport();auto& w=gw.state();
        for(int c=0;c<2;++c){t.sat_mass[c]=w.hg[c]*w.theta_s[c]*4*(c+1);t.unsat_mass[c]=w.hu[c]*4*(3-c);}
        t.init_mass[0]=t.ledgeredStorage(0);const double initial=w.storage();
        w.dt_cell[0]=.1;w.dt_cell[1]=.8;gw.assignTiers(.1,tiers);
        if(tiers>1)ASSERT_NE(w.tier[0],w.tier[1]);
        for(int step=1;step<=160;++step){
            for(int tier=0;tier<tiers;++tier)if(step%(1<<tier)==0)gw.fireGwFaces(tier,.1*(1<<tier));
            for(int tier=0;tier<tiers;++tier)if(step%(1<<tier)==0)gw.fireGwCells(tier,.1*(1<<tier),surf);
            // Before the coarse owner fires, its peer's accepted face
            // water/mass is still in flight: include both face sides.
            EXPECT_NEAR(w.ledgeredStorage(),initial,2e-11);EXPECT_NEAR(t.ledgeredStorage(0),t.init_mass[0],2e-11);
            if(step%(1<<(tiers-1))==0){EXPECT_NEAR(w.continuityResidual(),0,2e-11);EXPECT_NEAR(t.residual(0),0,2e-11);}
        }
        gw.settle(surf);EXPECT_NEAR(w.storage(),initial,2e-11);EXPECT_NEAR(w.led_lateral,0,1e-12);
        EXPECT_NEAR(t.residual(0),0,2e-11);EXPECT_GT(std::abs(t.net_lateral[0])+std::abs(t.internal_recharge[0]),0);
    }
}

TEST(SigmaConservation, ForcedMovingProfileConvergesWithTimestepRefinement) {
    for(auto law:laws) {
        const auto trajectory=[&](double dt){
            Rig r;r.init(law,1,1e-4);
            for(int k=0;k<int(16/dt);++k){
                r.gw.state().xacc_from_surface[0]+=.0004*dt;r.gw.bookSurfaceEt(0,dt,1e-5,0);
                r.gw.bookLinkSeepage(0,.0001*dt);r.fire(dt,k*dt);r.check();
            }
            std::vector<double> result=r.gw.state().theta_sigma;
            result.push_back(r.gw.state().hg[0]/2);result.push_back(r.gw.state().hu[0]/2);
            return result;
        };
        const auto reference=trajectory(.00625);double prior=1;
        for(double dt:{.2,.1,.05,.025}){
            const auto result=trajectory(dt);double error=0;
            for(std::size_t j=0;j<result.size();++j)error=std::max(error,std::abs(result[j]-reference[j]));
            EXPECT_LT(error,prior);prior=error;
        }
    }
}

TEST(SigmaConservation, PumpingAtLowTablesCannotOverdrawResidualStorage) {
    for(auto law:laws)for(double table:{0.,.001,1.,2.}) {
        SCOPED_TRACE(std::to_string(int(law))+"/table="+std::to_string(table));
        Rig r;r.init(law,table);GwResolvedSource source;source.cells={{0,1}};source.flow.constant=-1;
        r.gw.setSources({source});
        for(int k=0;k<10;++k){r.fire(1,k);r.check();}
        EXPECT_GE(r.gw.state().storage(),4*2*r.gw.state().theta_r[0]-1e-12);
        EXPECT_LE(r.gw.state().led_source_out,10);
    }
}
