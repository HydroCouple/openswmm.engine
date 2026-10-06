// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include "2d/mesh/MeshBuilder.hpp"
#include "2d/solver/InertialEdges.hpp"
#include "2d/subsurface/SubsurfaceSolver.hpp"
#include "2d/subsurface/SigmaColumn.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include "2d/data/SolverOptions2D.hpp"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
using namespace openswmm::twoD;
namespace {
constexpr auto laws={SoilChar::GARDNER,SoilChar::RUSSO,SoilChar::BROOKS_COREY,SoilChar::VAN_GENUCHTEN};
struct Rig {
    MeshData mesh;InertialEdges edges;SurfaceStateData surf;SolverOptions2D opts;
    SubsurfaceConfig cfg;SubsurfaceSolver gw;
    void init(SoilChar law,double table=1,double ks=.01,GwUnitFactors units={}) {
        mesh.resize_vertices(3);mesh.vx={0,4,0};mesh.vy={0,0,2};mesh.vz={10,10,10};
        mesh.resize_triangles(1);mesh.set_triangle(0,0,1,2);mesh.mannings_n[0]=.03;
        buildMeshTopology(mesh);edges.build(mesh);surf.resize(1,mesh.n_edge_slots());
        cfg.options.authored=true;cfg.options.closure=GwClosure::SIGMA;cfg.options.force_closed_form=true;
        cfg.options.soil_char=law;cfg.options.gw_et="BOUNDARY_ET";
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
    for(auto law:laws)for(double node:{-.0005,.0005})for(double table:{1.,1.999999,2.}) {
        Rig r;r.init(law,table);r.gw.state().c_loss[0]=.001;
        for(int k=0;k<20;++k){
            r.gw.state().nacc[0]=node;r.gw.bookLinkSeepage(0,.0004);
            r.gw.bookSurfaceEt(0,1,1e-5,0);r.fire(1,k);r.check();
            EXPECT_NEAR(r.gw.state().qnode_last[0],node,1e-15);
        }
        EXPECT_EQ(r.gw.nodeRefunds(),0);EXPECT_NEAR(r.gw.state().led_node,node*20,1e-13);
        EXPECT_NEAR(r.gw.state().led_link,.0004*20,1e-13);
    }
}
TEST(SigmaConservation, DeepLossCannotSpendWaterNeededForResidualStorage) {
    for(auto law:laws) {
        Rig r;r.init(law);r.gw.state().c_loss[0]=1;
        for(int k=0;k<4;++k){r.fire(10,k*10);r.check();}
        EXPECT_GT(r.gw.state().led_deep,0);EXPECT_NEAR(r.gw.state().hg[0],0,1e-12);
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
