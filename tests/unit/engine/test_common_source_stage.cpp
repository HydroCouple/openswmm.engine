// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include "2d/subsurface/CommonSourceStage.hpp"
#include "2d/subsurface/SubsurfaceSolver.hpp"
#include "2d/solver/ExplicitInertialSolver.hpp"
#include "2d/SurfaceRouter2D.hpp"
#include "hydraulics/TimestepController.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include "core/SimulationContext.hpp"
#include "core/DateTime.hpp"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <map>
#include <limits>
#include <cstdlib>
#include "core/CompletedSourceArchive.hpp"
#include "2d/subsurface/SourceResultHistory.hpp"
#ifdef OPENSWMM_HAS_HDF5_MODEL
#include "io/hdf5/Hdf5ModelWriter.hpp"
#endif

using namespace openswmm;
using namespace openswmm::twoD;
namespace {
constexpr double ft2=.3048*.3048,ft3=ft2*.3048;
struct AtmosphericInput {
    std::array<double,2> pet{0,0},impervious{0,0}; // ft/s, fraction of non-LID area
    std::array<double,2> rain_override{-1,-1}; // ft/s; negative retains gage records
    std::array<double,2> inside{1,1};
    double rain=432,lid_area=.5,lid_bottom=432,lid_saturation=50,ks=.01;
    bool barrel=false,covered=false,connected=false;
    std::string lid_type="IT";
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
            context.lid_controls.names={"IT"};context.lid_controls.lid_type={a.barrel ? "RB" : a.lid_type};
            context.lid_controls.surface={{3,0,.1,0,0}};
            if(a.lid_type=="VS")context.lid_controls.surface={{12,.1,.1,1,1}};
            context.lid_controls.storage={{12,.4,a.lid_bottom,0}};
            context.lid_controls.drain={{0,.5,0,0,0,0}};
            if(a.lid_type=="BC" || a.lid_type=="RG" || a.lid_type=="GR" || a.lid_type=="PP"){
                context.lid_controls.soil={{12,.45,.3,.1,43.2,4,3.5}};
                context.lid_controls.pavement={{6,.2,0,43.2,0,0}};
                context.lid_controls.drainmat={{3,.5,.1}};
            }
            context.lid_usage.subcatch_index={0};context.lid_usage.lid_index={0};
            context.lid_usage.number={2};context.lid_usage.area={a.lid_area/2/ft2};context.lid_usage.width={1};
            context.lid_usage.init_sat={a.lid_saturation};context.lid_usage.from_imperv={0};context.lid_usage.from_perv={0};
            context.lid_usage.to_perv={0};context.lid_usage.drain_to={""};
        }
        if(a.connected)context.subcatches.outlet_subcatch[0]=1;
        lid::LIDSolver initial;const lid::LIDSolver* snapshot=nullptr;
        if(a.covered) {
            initial.init(context);initial.group(static_cast<int>(lid::LIDType::RAIN_BARREL)).stor_covered[0]=1;snapshot=&initial;
        }
        ASSERT_EQ(sources.initialize(context,{0,1},{{0,trench ? 1-a.lid_area : 1},{1,1}},snapshot),"");
        preview.mesh_weather_area.assign(cells,4-(a.inside[0]+a.inside[1])/cells);
        for(int sc=0;sc<2;++sc) {
            SurfaceOwnerObject o;o.subcatch=sc;o.reviewed=a.inside[sc]>0;o.declared_area=1;
            o.inside_area=a.inside[sc];o.outside_area=1-a.inside[sc];preview.objects.push_back(o);
            const double non_lid=trench && sc==0 ? 1-a.lid_area : 1;
            for(int i=0;i<cells;++i) preview.shares.push_back({sc,i,a.inside[sc]/cells,a.inside[sc]*non_lid*(1-a.impervious[sc])/cells,
                a.inside[sc]*non_lid*a.impervious[sc]/cells,a.inside[sc]*(trench && sc==0 ? a.lid_area : 0)/cells,0});
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
void pollutantSources(Model& m,int cells=1,double inside_fraction=1) {
    m.setup(true,cells);
    const double full=2*cells/inside_fraction;
    m.context.subcatches.area.assign(2,full/(43560*ft2));
    m.context.lid_usage.subcatch_index={0,1};m.context.lid_usage.lid_index={0,0};
    m.context.lid_usage.number={2,2};m.context.lid_usage.area={full/2/ft2,full/2/ft2};
    m.context.lid_usage.width={1,1};m.context.lid_usage.init_sat={80,20};
    m.context.lid_usage.from_imperv={0,0};m.context.lid_usage.from_perv={0,0};
    m.context.lid_usage.to_perv={0,0};m.context.lid_usage.drain_to={"",""};
    m.context.pollutant_names.try_add("Dissolved");m.context.pollutant_names.try_add("Counts");
    m.context.pollutants.resize_pollutants(2);m.context.pollutants.c_rain={7,23};
    m.context.pollutants.units[1]=MassUnits::COUNTS_PER_L;m.context.options.ignore_quality=false;
    m.sources=runoff::SourceWaterDriver{};
    ASSERT_EQ(m.sources.initialize(m.context,{0,1},{{0,0},{1,0}}),"");
    m.preview={};m.preview.mesh_weather_area.assign(cells,0);
    for(int sc=0;sc<2;++sc) {
        SurfaceOwnerObject o;o.subcatch=sc;o.reviewed=true;o.declared_area=full;
        o.inside_area=2*cells;o.outside_area=full-o.inside_area;m.preview.objects.push_back(o);
        for(int i=0;i<cells;++i)m.preview.shares.push_back({sc,i,2,0,0,2,0});
    }
    RowLayoutLite rows;rows.n_species=2;rows.n_pollut=2;rows.names={"Dissolved","Counts"};
    std::vector<std::string> warnings;m.gw.initTransport(rows,nullptr,{},warnings);
    m.surface.transport.resize(2,cells,0);m.surface.transport.row_names=rows.names;
    m.surface.transport.cell_mass.assign(2*cells,19); // unrelated mesh pool must stay intact
}
void mixedPollutants(Model& m,int cells=1,const AtmosphericInput& a={},bool adjacent=false) {
    m.setup(true,cells,false,adjacent,false,a);
    m.context.pollutant_names.try_add("Dissolved");m.context.pollutant_names.try_add("Counts");
    m.context.pollutants.resize_pollutants(2);m.context.pollutants.c_rain={7,23};
    m.context.pollutants.units[1]=MassUnits::COUNTS_PER_L;m.context.options.ignore_quality=false;
    m.sources=runoff::SourceWaterDriver{};
    ASSERT_EQ(m.sources.initialize(m.context,{0,1},{{0,1-a.lid_area},{1,1}}),"");
    RowLayoutLite rows;rows.n_species=2;rows.n_pollut=2;rows.names={"Dissolved","Counts"};
    std::vector<std::string> warnings;m.gw.initTransport(rows,nullptr,{},warnings);
    auto& tr=m.surface.transport;tr.resize(2,cells,0);tr.row_names=rows.names;
    tr.cell_mass.assign(2*cells,19);tr.rain_conc={3,11};
    m.surface.rainfall.assign(cells,.001);
}
}

TEST(CommonSourceStage, CompletedLidPollutantsPairWithBoundedReceiverExactlyOnce) {
    Model m;pollutantSources(m);m.room(.001);m.attach();
    const auto mesh_mass=m.surface.transport.cell_mass;
    ASSERT_EQ(m.stage.advance(0,1),"");
    std::array<double,2> mass{};double water=0;
    for(const auto& r:m.stage.receipts()) {
        EXPECT_EQ(r.request.kind,SurfaceDonorKind::LID_BOTTOM);ASSERT_EQ(r.mass.size(),2u);
        water+=r.volume;for(int p=0;p<2;++p)mass[p]+=r.mass[p];
    }
    EXPECT_GT(water,0);EXPECT_LE(water,.001+1e-12);EXPECT_GT(mass[0],0);
    EXPECT_LT(mass[0]/water,7); // initially clean bottom store dilutes rain
    EXPECT_EQ(m.surface.transport.cell_mass,mesh_mass);
    EXPECT_DOUBLE_EQ(m.surface.transport.lost_infiltration[0],0);
    for(int p=0;p<2;++p) {
        EXPECT_NEAR(m.gw.transport().xacc_from_surface[m.gw.transport().idx(p,0)],mass[p],1e-15);
        double withdrawal=0;for(const auto& b:m.sources.pollutants())withdrawal+=b.spatial_infiltration[p];
        EXPECT_NEAR(withdrawal,mass[p],1e-15);
        for(int group=0;group<2;++group)EXPECT_NEAR(m.sources.pollutantBalanceResidual(group,p),0,1e-12);
    }
    EXPECT_FALSE(m.stage.advance(0,1).empty());
    EXPECT_NEAR(m.gw.transport().xacc_from_surface[0],mass[0],1e-15);
}

TEST(CommonSourceStage, MeshAndLidPollutantsShareReviewedWaterAndMass) {
    Model m;pollutantSources(m);
    for(auto& o:m.preview.objects){o.inside_area=1;o.outside_area=1;}
    for(auto& s:m.preview.shares){s.weather_area=1;s.lid_area=1;}
    m.preview.mesh_weather_area[0]=2;m.surface.rainfall[0]=.001;
    m.surface.transport.rain_conc={3,11};m.room(.001);m.attach();
    ASSERT_EQ(m.stage.advance(0,1),"");EXPECT_GT(m.receipts(SurfaceDonorKind::MESH),0);
    EXPECT_GT(m.receipts(SurfaceDonorKind::LID_BOTTOM),0);
    EXPECT_NEAR(m.surface.transport.gained_rainfall[0],.002*3,1e-15);
}

TEST(CommonSourceStage, AllThreeDonorKindsUseTheirOwnMixtureAndActualReceipt) {
    AtmosphericInput a;a.inside={.5,.25};Model m;mixedPollutants(m,1,a);m.room(.001);m.attach();
    ASSERT_EQ(m.stage.advance(0,1),"");
    std::array<double,2> received{},mesh{},nonlid{},lid{};
    for(const auto& r:m.stage.receipts()) {
        ASSERT_EQ(r.mass.size(),2u);EXPECT_LE(r.volume,r.request.candidate+1e-15);
        for(int p=0;p<2;++p) {
            received[p]+=r.mass[p];
            if(r.request.kind==SurfaceDonorKind::MESH) {
                mesh[p]+=r.mass[p];
                const double rain=.001*m.preview.mesh_weather_area[0];
                EXPECT_NEAR(r.mass[p],r.volume*(19+rain*m.surface.transport.rain_conc[p])/(.08+rain),1e-13);
            } else if(r.request.kind==SurfaceDonorKind::NON_LID) {
                nonlid[p]+=r.mass[p];EXPECT_NEAR(r.mass[p],r.volume*m.context.pollutants.c_rain[p],1e-14);
            } else {
                lid[p]+=r.mass[p];EXPECT_LT(r.mass[p],r.volume*m.context.pollutants.c_rain[p]);
            }
        }
    }
    EXPECT_GT(mesh[0],0);EXPECT_GT(nonlid[0],0);EXPECT_GT(lid[0],0);
    for(int p=0;p<2;++p) {
        double source=0;for(const auto& b:m.sources.pollutants())source+=b.spatial_infiltration[p];
        EXPECT_NEAR(source,nonlid[p]+lid[p],1e-14);
        const auto& tr=m.surface.transport;
        EXPECT_NEAR(tr.cell_mass[p]+mesh[p],19+tr.gained_rainfall[p],1e-13);
        EXPECT_NEAR(tr.lost_infiltration[p],mesh[p],1e-13);
        EXPECT_NEAR(m.gw.transport().xacc_from_surface[p],received[p],1e-13);
        for(int group=0;group<2;++group)EXPECT_NEAR(m.sources.pollutantBalanceResidual(group,p),0,1e-12);
    }
}

TEST(CommonSourceStage, MeshEvaporationUsesPreEvaporationMixtureAndDryMassRewets) {
    Model wet;mixedPollutants(wet);wet.surface.evap_rate[0]=.0001;wet.room(.001);wet.attach();
    ASSERT_EQ(wet.stage.advance(0,1),"");EXPECT_GT(wet.surface.evap_loss_total,0);
    for(const auto& r:wet.stage.receipts())if(r.request.kind==SurfaceDonorKind::MESH)
        EXPECT_NEAR(r.mass[0],r.volume*(19+.002*3)/(.08+.002),1e-13);
    AtmosphericInput a;a.ks=0;Model m;mixedPollutants(m,1,a);m.surface.rainfall[0]=0;m.surface.evap_rate[0]=1;
    m.room(0);m.attach();ASSERT_EQ(m.stage.advance(0,1),"");
    EXPECT_DOUBLE_EQ(m.surface.volume[0],0);EXPECT_EQ(m.surface.transport.cell_mass,(std::vector<double>{19,19}));
    m.surface.rainfall[0]=.001;m.surface.evap_rate[0]=.0001;m.gw.state().Ks[0]=.01;m.room(.001);
    ASSERT_EQ(m.stage.advance(1,2),"");
    double withdrawn=0;for(const auto& r:m.stage.receipts())if(r.request.kind==SurfaceDonorKind::MESH) {
        withdrawn+=r.mass[0];EXPECT_NEAR(r.mass[0],r.volume*(19+.002*3)/.002,1e-12);
    }
    EXPECT_GT(withdrawn,0);EXPECT_NEAR(m.surface.transport.cell_mass[0]+withdrawn,19+.006,1e-12);
    EXPECT_DOUBLE_EQ(m.surface.evap_loss_total,.08); // first rewet step starts at dry depth
}

TEST(CommonSourceStage, MixedQualityKeepsUnequalAwardsAndMassInTheirCells) {
    AtmosphericInput a;a.inside={.5,.25};Model m;mixedPollutants(m,2,a);
    m.room(0,0);m.room(.001,1);m.surface.transport.cell_mass={5,19,17,31};m.attach();
    const auto old=m.surface.transport.cell_mass;ASSERT_EQ(m.stage.advance(0,1),"");
    std::array<double,2> received{};
    for(const auto& r:m.stage.receipts()) {
        if(r.request.cell==0){EXPECT_DOUBLE_EQ(r.volume,0);for(double mass:r.mass)EXPECT_DOUBLE_EQ(mass,0);}
        else for(int p=0;p<2;++p)received[p]+=r.mass[p];
    }
    for(int p=0;p<2;++p) {
        const auto& tr=m.surface.transport;
        EXPECT_NEAR(tr.cell_mass[tr.idx(p,0)],old[tr.idx(p,0)]+.001*m.preview.mesh_weather_area[0]*tr.rain_conc[p],1e-14);
        EXPECT_DOUBLE_EQ(m.gw.transport().xacc_from_surface[m.gw.transport().idx(p,0)],0);
        EXPECT_NEAR(m.gw.transport().xacc_from_surface[m.gw.transport().idx(p,1)],received[p],1e-13);
        EXPECT_GT(received[p],0);
    }
}

TEST(CommonSourceStage, InvalidMeshMassAndRainDefinitionsRollbackAndRetry) {
    for(int mode=0;mode<7;++mode) {
        Model m;mixedPollutants(m);m.attach();
        if(mode==0)m.surface.transport.cell_mass[0]=-1;
        if(mode==1)m.surface.transport.cell_mass[0]=std::numeric_limits<double>::quiet_NaN();
        if(mode==2)m.surface.transport.rain_conc[0]=-1;
        if(mode==3)m.surface.transport.rain_conc[0]=std::numeric_limits<double>::infinity();
        if(mode==4)m.surface.transport.cell_mass.pop_back();
        if(mode==5)m.surface.transport.rain_conc.pop_back();
        if(mode==6)m.surface.transport.lost_infiltration.pop_back();
        const double water=m.surface.volume[0];EXPECT_FALSE(m.stage.advance(0,1).empty());
        EXPECT_DOUBLE_EQ(m.stage.completedEnd(),0);EXPECT_TRUE(m.stage.receipts().empty());
        EXPECT_DOUBLE_EQ(m.surface.volume[0],water);EXPECT_DOUBLE_EQ(m.gw.state().xacc_from_surface[0],0);
        for(const auto& b:m.sources.pollutants())EXPECT_DOUBLE_EQ(b.rain[0],0);
        m.surface.transport.cell_mass={19,19};m.surface.transport.rain_conc={3,11};
        m.surface.transport.lost_infiltration={0,0};ASSERT_EQ(m.stage.advance(0,1),"");
    }
}

TEST(CommonSourceStage, MixedQualityConservesThroughRealMarcherAndGroundwaterFirings) {
    AtmosphericInput a;a.inside={.5,.25};Model m;mixedPollutants(m,1,a);m.attach(true);
    for(double end:{.3,1.,2.125,4.}) {
        const double start=m.stage.completedEnd();ASSERT_DOUBLE_EQ(m.marcher.advance(start,end),end);
        for(int p=0;p<2;++p) {
            const auto& tr=m.surface.transport;const auto& gt=m.gw.transport();
            double source=0;for(const auto& b:m.sources.pollutants())source+=b.spatial_infiltration[p];
            EXPECT_NEAR(gt.gained_infil[p],source+tr.lost_infiltration[p],1e-13);
            EXPECT_NEAR(tr.cell_mass[p]+tr.lost_infiltration[p],19+tr.gained_rainfall[p],1e-13);
            EXPECT_NEAR(tr.gained_rainfall[p],.001*3.25*end*tr.rain_conc[p],1e-14);
            EXPECT_NEAR(gt.residual(p),0,1e-12);EXPECT_DOUBLE_EQ(gt.xacc_from_surface[p],0);
            for(int group=0;group<2;++group)EXPECT_NEAR(m.sources.pollutantBalanceResidual(group,p),0,1e-12);
        }
        EXPECT_NEAR(m.gw.state().continuityResidual(),0,2e-11);
    }
    m.marcher.finalize();
}

TEST(CommonSourceStage, DeniedLidRechargeRetainsBothWaterAndPollutants) {
    Model m;pollutantSources(m);m.room(0);m.attach();ASSERT_EQ(m.stage.advance(0,1),"");
    for(const auto& r:m.stage.receipts()) {EXPECT_DOUBLE_EQ(r.volume,0);for(double a:r.mass)EXPECT_DOUBLE_EQ(a,0);}
    EXPECT_DOUBLE_EQ(m.gw.transport().xacc_from_surface[0],0);
    for(const auto& b:m.sources.pollutants()) {
        EXPECT_DOUBLE_EQ(b.spatial_infiltration[0],0);EXPECT_GT(b.layers[0][3],0);
    }
    for(int group=0;group<2;++group)EXPECT_NEAR(m.sources.pollutantBalanceResidual(group,0),0,1e-12);
}

TEST(CommonSourceStage, PartialOutsideLidBottomKeepsSeparateMassAndDeniedCellShare) {
    Model m;pollutantSources(m,2,.5);m.room(0,0);m.room(.01,1);m.attach();ASSERT_EQ(m.stage.advance(0,1),"");
    std::array<double,2> received{};
    for(const auto& r:m.stage.receipts()) {
        if(r.request.cell==0) {EXPECT_DOUBLE_EQ(r.volume,0);for(double a:r.mass)EXPECT_DOUBLE_EQ(a,0);}
        else for(int p=0;p<2;++p)received[p]+=r.mass[p];
    }
    EXPECT_DOUBLE_EQ(m.gw.transport().xacc_from_surface[0],0);
    EXPECT_GT(received[0],0);
    for(int p=0;p<2;++p) {
        double spatial=0,outside=0;
        for(const auto& b:m.sources.pollutants()){spatial+=b.spatial_infiltration[p];outside+=b.outside_infiltration[p];}
        EXPECT_NEAR(spatial,received[p],1e-15);EXPECT_GT(outside,0);
        EXPECT_NEAR(m.gw.transport().xacc_from_surface[m.gw.transport().idx(p,1)],received[p],1e-15);
        for(int group=0;group<2;++group)EXPECT_NEAR(m.sources.pollutantBalanceResidual(group,p),0,1e-12);
    }
}

TEST(CommonSourceStage, PollutantLayoutAndMeshProfileFailuresCannotPartiallyAttach) {
    for(int mode=0;mode<4;++mode) {
        Model m;pollutantSources(m);
        if(mode==0)std::swap(m.gw.transport().row_names[0],m.gw.transport().row_names[1]);
        if(mode==1)m.surface.transport.temp_row=1;
        if(mode==2)m.preview.mesh_weather_area[0]=.1;
        if(mode==3)m.surface.transport.n_pollut=1;
        EXPECT_FALSE(m.stage.initialize(m.mesh,m.surface,m.options,m.gw,m.sources,m.preview).empty());
        EXPECT_EQ(m.sources.spatialFractions(),(std::vector<double>{1,1}));
        EXPECT_DOUBLE_EQ(m.sources.pollutants()[0].rain[0],0);EXPECT_DOUBLE_EQ(m.gw.state().xacc_from_surface[0],0);
    }
}

TEST(CommonSourceStage, PollutantProfileFailureCanRetryWithoutWaterOrMassMutation) {
    Model m;pollutantSources(m);m.room(.001);m.attach();m.surface.coupling_flux[0]=.01;
    const auto old=m.surface.transport.cell_mass;
    EXPECT_FALSE(m.stage.advance(0,1).empty());EXPECT_DOUBLE_EQ(m.stage.completedEnd(),0);
    EXPECT_DOUBLE_EQ(m.sources.pollutants()[0].rain[0],0);EXPECT_DOUBLE_EQ(m.gw.transport().xacc_from_surface[0],0);
    EXPECT_EQ(m.surface.transport.cell_mass,old);m.surface.coupling_flux[0]=0;
    m.gw.transport().row_names[0]="Wrong";EXPECT_FALSE(m.stage.advance(0,1).empty());
    m.gw.transport().row_names[0]="Dissolved";ASSERT_EQ(m.stage.advance(0,1),"");
    EXPECT_GT(m.gw.transport().xacc_from_surface[0],0);
}

TEST(CommonSourceStage, PollutantsConserveThroughPendingAndActualGroundwaterFirings) {
    Model m;pollutantSources(m,1,.5);m.attach();
    std::array<double,2> delivered{};
    for(double end:{.1,.3,1.,2.125,4.}) {
        const double start=m.stage.completedEnd();ASSERT_EQ(m.stage.advance(start,end),"");
        for(const auto& r:m.stage.receipts())for(int p=0;p<2;++p)delivered[p]+=r.mass[p];
        for(int p=0;p<2;++p) {
            EXPECT_NEAR(m.gw.transport().gained_infil[p]+m.gw.transport().xacc_from_surface[p],delivered[p],1e-14);
            EXPECT_NEAR(m.gw.transport().residual(p),0,1e-12);
        }
        m.gw.assignTiers(end-start,1);m.gw.fireGwCells(0,end-start,m.surface,start);
        for(int p=0;p<2;++p) {
            EXPECT_DOUBLE_EQ(m.gw.transport().xacc_from_surface[p],0);
            EXPECT_NEAR(m.gw.transport().gained_infil[p],delivered[p],1e-14);
            EXPECT_NEAR(m.gw.transport().residual(p),0,1e-12);
            double rain=0,stores=0,external=0,spatial=0;
            for(const auto& b:m.sources.pollutants()) {
                rain+=b.rain[p];spatial+=b.spatial_infiltration[p];
                external+=b.outlet[p]+b.removed[p]+b.outside_infiltration[p];
                for(double mass:b.layers[p])stores+=mass;
            }
            EXPECT_NEAR(spatial,delivered[p],1e-14);
            const auto& tr=m.gw.transport();
            EXPECT_NEAR(rain,stores+external+tr.ledgeredStorage(p)+tr.lost_deep[p]+tr.lost_node[p]+
                tr.lost_link[p]+tr.lost_dunne[p]+tr.lost_et[p]+tr.lost_reaction[p],1e-12);
        }
        EXPECT_NEAR(m.gw.state().continuityResidual(),0,1e-12);
    }
}

TEST(CommonSourceStage, ActualMarcherPairsLidPollutantsWithoutInventingMeshRainMass) {
    Model m;pollutantSources(m);m.surface.rainfall[0]=.005;m.attach(true);
    const auto mesh_mass=m.surface.transport.cell_mass;
    for(double end:{.3,1.,2.125,4.}) {
        const double start=m.stage.completedEnd();EXPECT_DOUBLE_EQ(m.marcher.advance(start,end),end);
        EXPECT_DOUBLE_EQ(m.stage.completedEnd(),end);EXPECT_DOUBLE_EQ(m.marcher.completedSourceTime(),end);
        EXPECT_EQ(m.surface.transport.cell_mass,mesh_mass);
        for(int p=0;p<2;++p) {
            EXPECT_DOUBLE_EQ(m.surface.transport.gained_rainfall[p],0);
            EXPECT_DOUBLE_EQ(m.surface.transport.lost_infiltration[p],0);
            EXPECT_DOUBLE_EQ(m.gw.transport().xacc_from_surface[p],0);
            double spatial=0;for(const auto& b:m.sources.pollutants())spatial+=b.spatial_infiltration[p];
            EXPECT_NEAR(m.gw.transport().gained_infil[p],spatial,1e-13);
            EXPECT_NEAR(m.gw.transport().residual(p),0,1e-12);
            for(int group=0;group<2;++group)EXPECT_NEAR(m.sources.pollutantBalanceResidual(group,p),0,1e-12);
        }
    }
    EXPECT_GT(m.gw.transport().gained_infil[0],0);m.marcher.finalize();
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

TEST(CommonSourceStage, PartialInsideSourcesKeepOutsideLossAndWeatherSeparate) {
    AtmosphericInput a;a.inside={.5,.25};a.ks=1e-5;a.et="BOUNDARY_ET";a.pet={1e-5,3e-5};
    Model m;m.setup(true,1,false,false,false,a);
    ASSERT_EQ(m.stage.initialize(m.mesh,m.surface,m.options,m.gw,m.sources,m.preview),"");
    EXPECT_DOUBLE_EQ(m.preview.mesh_weather_area[0],3.25);
    for(double end:{.1,.3,1.,2.125,4.}) {
        const double start=m.stage.completedEnd();
        ASSERT_EQ(m.stage.advance(start,end),"");
        for(int sc=0;sc<2;++sc)EXPECT_NEAR(m.sources.balanceResidual(m.sources.clocks().groupForSubcatch(sc)),0,1e-10);
        double spatial=0;for(const auto& r:m.stage.receipts())if(r.request.kind!=SurfaceDonorKind::MESH)spatial+=r.volume;
        EXPECT_GE(spatial,0);m.gw.assignTiers(end-start,1);m.gw.fireGwCells(0,end-start,m.surface,start);etBalance(m);
        EXPECT_NEAR(m.gw.state().continuityResidual(),0,2e-11);
    }
}
TEST(CommonSourceStage, ConnectedOutsideOnlyPeerKeepsItsNativePath) {
    AtmosphericInput a;a.inside={1,0};a.ks=1e-5;a.connected=true;Model m;m.setup(false,1,false,false,false,a);
    ASSERT_EQ(m.stage.initialize(m.mesh,m.surface,m.options,m.gw,m.sources,m.preview),"");
    ASSERT_EQ(m.sources.clocks().groups().size(),1u);
    ASSERT_EQ(m.stage.advance(0,1),"");EXPECT_GT(m.sources.ledgers()[1].infiltration,0);
    for(const auto& r:m.stage.receipts())EXPECT_NE(r.request.source,"S1");
}

TEST(CommonSourceStage, GeneralizedNativeAndSealedLidFootprintsSpendOnlyTheirInsideAwards) {
    for(const std::string type:{"BC","RG","GR","IT","PP","RB","VS","RD"})
    for(double inside:{.25,.5,1.})for(bool sealed:{false,true}) {
        SCOPED_TRACE(type+"/"+std::to_string(inside)+"/"+std::to_string(sealed));
        AtmosphericInput a;a.inside={inside,1};a.lid_type=type;a.lid_bottom=sealed?0:432;
        a.ks=1e-5;a.et="BOUNDARY_ET";a.pet={1e-5,3e-5};
        Model m;m.setup(true,1,false,false,false,a);
        ASSERT_EQ(m.stage.initialize(m.mesh,m.surface,m.options,m.gw,m.sources,m.preview),"");
        const bool native=type=="VS" || (type!="GR" && type!="RB" && type!="RD" && !sealed);
        double before_spatial=0;for(double end:{.1,.3,1.,2.125,4.}) {
            const double start=m.stage.completedEnd();ASSERT_EQ(m.stage.advance(start,end),"");
            double paid=0;for(const auto& r:m.stage.receipts())if(r.request.kind!=SurfaceDonorKind::MESH){
                paid+=r.volume;if(r.request.kind==SurfaceDonorKind::LID_BOTTOM){EXPECT_TRUE(native);EXPECT_LE(r.request.area,.5*inside);}
            }
            double spatial=0;for(const auto& b:m.sources.ledgers()) {
                EXPECT_NEAR(b.infiltration,b.spatial_infiltration+b.outside_infiltration,1e-11);
                spatial+=b.spatial_infiltration*ft3;
            }
            EXPECT_NEAR(paid,spatial-before_spatial,1e-12);before_spatial=spatial;etBalance(m);
            double lid_soil=0,lid_weather=0;for(const auto& r:m.stage.etReceipts())if(r.owner==SurfaceEtOwner::LID){
                lid_soil+=r.soil_demand;lid_weather+=r.area;
                if(r.cell<0){EXPECT_DOUBLE_EQ(r.soil_demand,0);EXPECT_NEAR(r.area,.5*(1-inside),1e-14);}
            }
            EXPECT_NEAR(lid_weather,.5,1e-14);if(!native)EXPECT_DOUBLE_EQ(lid_soil,0);
            m.gw.assignTiers(end-start,1);m.gw.fireGwCells(0,end-start,m.surface,start);
            EXPECT_NEAR(m.gw.state().continuityResidual(),0,2e-11);
            EXPECT_EQ(m.gw.nodeRefunds(),0);for(int g=0;g<int(m.sources.clocks().groups().size());++g)EXPECT_NEAR(m.sources.balanceResidual(g),0,1e-10);
        }
        if(inside<1 && native)EXPECT_GT(m.sources.ledgers()[0].outside_infiltration,0);
    }
}

TEST(CommonSourceStage, PartialAreasKeepScarceAwardsInTheirCellsAcrossSourceOrder) {
    std::map<std::string,double> prior;
    for(bool reverse:{false,true}) {
        AtmosphericInput a;a.inside=reverse?std::array<double,2>{.25,.5}:std::array<double,2>{.5,.25};
        a.ks=1e-5;Model m;m.setup(false,2,reverse,false,false,a);m.room(0,0);m.room(.001,1);m.attach();
        ASSERT_EQ(m.stage.advance(0,1),"");std::map<std::string,double> now;
        for(const auto& r:m.stage.receipts()){
            if(r.request.cell==0)EXPECT_DOUBLE_EQ(r.volume,0);
            EXPECT_LE(r.volume,r.request.candidate+1e-15);now[r.request.source]+=r.volume;
        }
        EXPECT_DOUBLE_EQ(m.gw.state().xacc_from_surface[0],0);EXPECT_LE(m.gw.state().xacc_from_surface[1],.001+1e-14);
        for(const auto& b:m.sources.ledgers()){
            EXPECT_GT(b.outside_infiltration,0);EXPECT_GT(b.spatial_infiltration,0);
            EXPECT_NEAR(b.infiltration,b.spatial_infiltration+b.outside_infiltration,1e-12);
        }
        if(!reverse)prior=now;else for(const auto& [name,volume]:now)EXPECT_NEAR(volume,prior[name],1e-13);
        for(int g=0;g<int(m.sources.clocks().groups().size());++g)EXPECT_NEAR(m.sources.balanceResidual(g),0,1e-10);
    }
    AtmosphericInput a;a.inside={.5,.25};Model invalid;invalid.setup(false,1,false,false,false,a);
    invalid.preview.shares[0].pervious_area-=.1;invalid.preview.shares[0].impervious_area+=.1;
    EXPECT_FALSE(invalid.stage.initialize(invalid.mesh,invalid.surface,invalid.options,invalid.gw,invalid.sources,invalid.preview).empty());
    EXPECT_EQ(invalid.sources.spatialFractions(),(std::vector<double>{1,1}));
    EXPECT_DOUBLE_EQ(invalid.sources.ledgers()[0].rain,0);EXPECT_DOUBLE_EQ(invalid.gw.state().xacc_from_surface[0],0);
    Model area_mismatch;area_mismatch.setup(false,1,false,false,false,a);
    area_mismatch.preview.objects[0].inside_area+=.1;
    EXPECT_FALSE(area_mismatch.stage.initialize(area_mismatch.mesh,area_mismatch.surface,area_mismatch.options,
        area_mismatch.gw,area_mismatch.sources,area_mismatch.preview).empty());
    EXPECT_EQ(area_mismatch.sources.spatialFractions(),(std::vector<double>{1,1}));
}

TEST(CommonSourceStage, NumericalBackstopKeepsOwnedReturnsOutOfGenericMeshAccumulator) {
    Model m;mixedPollutants(m);m.room(.005);m.attach();
    ASSERT_EQ(m.stage.advance(0,1),"");
    ASSERT_EQ(m.stage.advance(1,2),"");
    m.gw.state().lacc[0]=4;
    m.gw.fireGwCells(0,2,m.surface,0);
    EXPECT_GT(m.gw.state().led_reject,0);
    EXPECT_DOUBLE_EQ(m.gw.state().xacc_to_surface[0],0);
    EXPECT_DOUBLE_EQ(m.gw.transport().xacc_to_surface[0],0);
    EXPECT_EQ(m.stage.advance(2,3),"");
}
TEST(CommonSourceStage, PhysicalDunneUsesReviewedSourceOwnership) {
    Model m;pollutantSources(m);m.attach();
    ASSERT_EQ(m.stage.advance(0,1),"");
    m.gw.state().lacc[0]=10;
    m.gw.fireGwCells(0,1,m.surface,0);
    EXPECT_GT(m.gw.state().led_dunne,0);
    EXPECT_DOUBLE_EQ(m.gw.state().xacc_to_surface[0],0);
    EXPECT_EQ(m.stage.advance(1,2),"");
}

TEST(CommonSourceStage, PendingIntervalsRefundOriginalMixturesAndRetryExactlyOnce) {
    Model m;mixedPollutants(m);m.room(.005);m.attach();
    std::vector<SurfaceIntakeReceipt> original;
    for(int t=0;t<2;++t) {
        ASSERT_EQ(m.stage.advance(t,t+1),"");
        for(const auto& r:m.stage.receipts())if(r.volume>0)original.push_back(r);
    }
    ASSERT_EQ(m.gw.pendingSourceReceipts(0).size(),original.size());
    auto& tr=m.gw.transport();tr.unsat_mass={5,17};tr.init_mass={5,17};
    m.gw.state().lacc[0]=4;m.gw.fireGwCells(0,2,m.surface,0);
    EXPECT_TRUE(m.gw.pendingSourceReceipts(0).empty());
    const auto pending=m.gw.sourceReturns();ASSERT_FALSE(pending.empty());
    double refund=0,excess=0,physical=0;int matched=0;
    for(const auto& r:pending) {
        EXPECT_LE(r.available_at,2);
        if(r.kind==SurfaceReturnKind::NUMERICAL_REFUND) {
            ++matched;refund+=r.volume;
            auto donor=std::find_if(original.begin(),original.end(),[&](const auto& x){
                return x.start==r.intake.start && x.request.kind==r.intake.request.kind &&
                    x.request.source==r.intake.request.source && x.request.unit==r.intake.request.unit;
            });
            ASSERT_NE(donor,original.end());
            EXPECT_EQ(r.intake.end,donor->end);EXPECT_EQ(r.intake.mass,donor->mass);
            for(int p=0;p<2;++p)EXPECT_NEAR(r.mass[p],donor->mass[p]*r.volume/donor->volume,1e-14);
        } else if(r.kind==SurfaceReturnKind::COLUMN_EXCESS)excess+=r.volume;
        else physical+=r.volume;
    }
    EXPECT_EQ(matched,int(original.size()));
    EXPECT_NEAR(refund+excess,m.gw.state().led_reject,1e-13);
    EXPECT_NEAR(physical,m.gw.state().led_dunne,1e-13);
    for(int p=0;p<2;++p)EXPECT_NEAR(tr.residual(p),0,1e-12);
    const double mesh_volume=m.surface.volume[0];m.surface.volume[0]=-1;
    EXPECT_FALSE(m.stage.advance(2,3).empty());EXPECT_EQ(m.stage.completedEnd(),2);
    EXPECT_EQ(m.gw.sourceReturns().size(),pending.size());
    for(const auto& b:m.sources.ledgers())EXPECT_DOUBLE_EQ(b.aquifer_return,0);
    m.surface.volume[0]=mesh_volume;ASSERT_EQ(m.stage.advance(2,3),"");
    EXPECT_TRUE(m.gw.sourceReturns().empty());EXPECT_EQ(m.stage.returns().size(),pending.size());
    double credited=m.stage.meshReturn();for(const auto& b:m.sources.ledgers())credited+=b.aquifer_return*ft3;
    EXPECT_NEAR(credited,refund+excess+physical,1e-12);
    const auto source0=m.sources.ledgers()[0].aquifer_return;
    ASSERT_EQ(m.stage.advance(3,4),"");EXPECT_TRUE(m.stage.returns().empty());
    EXPECT_DOUBLE_EQ(m.sources.ledgers()[0].aquifer_return,source0);
    for(int group=0;group<2;++group) {
        EXPECT_NEAR(m.sources.balanceResidual(group),0,1e-9);
        for(int p=0;p<2;++p)EXPECT_NEAR(m.sources.pollutantBalanceResidual(group,p),0,1e-10);
    }
}
TEST(CommonSourceStage, ReturnOwnershipUsesPhysicalAreasIncludingImperviousPools) {
    AtmosphericInput a;a.impervious={.6,.4};a.inside={.75,.25};
    Model m;mixedPollutants(m,1,a);m.attach();
    m.gw.state().lacc[0]=10;m.gw.transport().sat_mass={3,11};m.gw.transport().init_mass={3,11};
    m.gw.fireGwCells(0,1,m.surface,0);
    double volume=0;std::array<double,2> mass{};
    for(const auto& r:m.gw.sourceReturns()){volume+=r.volume;for(int p=0;p<2;++p)mass[p]+=r.mass[p];}
    ASSERT_GT(volume,0);ASSERT_EQ(m.stage.advance(0,1),"");ASSERT_EQ(m.stage.advance(1,2),"");
    EXPECT_NEAR(m.stage.meshReturn(),volume*.75,1e-12); // 3 m2 of the 4 m2 cell
    for(int sc=0;sc<2;++sc) {
        EXPECT_NEAR(m.sources.ledgers()[sc].aquifer_return*ft3,volume*a.inside[sc]/4,1e-12);
        for(int p=0;p<2;++p) {
            double returned=0;for(const auto& b:m.sources.pollutants())if(b.source==sc)returned+=b.aquifer_return[p];
            EXPECT_NEAR(returned,mass[p]*a.inside[sc]/4,1e-13);
            EXPECT_NEAR(m.sources.pollutantBalanceResidual(sc,p),0,1e-10);
        }
    }
    for(int p=0;p<2;++p)EXPECT_NEAR(m.surface.transport.gained_exfiltration[p],mass[p]*.75,1e-13);
}
TEST(CommonSourceStage, FutureIntakesFailBeforeGatherAndReturnsNeverLookAhead) {
    Model m;mixedPollutants(m);m.attach();ASSERT_EQ(m.stage.advance(0,1),"");
    const auto volume=m.gw.state().xacc_from_surface[0];const auto mass=m.gw.transport().xacc_from_surface;
    EXPECT_THROW(m.gw.fireGwCells(0,.5,m.surface,0),std::runtime_error);
    EXPECT_DOUBLE_EQ(m.gw.state().xacc_from_surface[0],volume);EXPECT_EQ(m.gw.transport().xacc_from_surface,mass);
    EXPECT_FALSE(m.gw.pendingSourceReceipts(0).empty());
    m.gw.state().lacc[0]=10;m.gw.fireGwCells(0,2,m.surface,0);
    ASSERT_FALSE(m.gw.sourceReturns().empty());const auto count=m.gw.sourceReturns().size();
    ASSERT_EQ(m.stage.advance(1,2),"");EXPECT_TRUE(m.stage.returns().empty());
    EXPECT_EQ(m.gw.sourceReturns().size(),count);
    ASSERT_EQ(m.stage.advance(2,3),"");EXPECT_TRUE(m.gw.sourceReturns().empty());
}
TEST(CommonSourceStage, OwnedCellRejectsUnpairedInputsBeforeCellMutation) {
    Model m;mixedPollutants(m);m.attach();
    m.gw.bookInfiltrationFromSurface(0,.001);const auto water=m.gw.storage();
    EXPECT_THROW(m.gw.fireGwCells(0,1,m.surface,0),std::runtime_error);
    EXPECT_DOUBLE_EQ(m.gw.storage(),water);EXPECT_DOUBLE_EQ(m.gw.state().xacc_from_surface[0],.001);
}
TEST(CommonSourceStage, ForcingKnotsPreserveTupleFractionsAndCompletedAvailability) {
    Model m;mixedPollutants(m);m.room(.005);m.attach();ASSERT_EQ(m.stage.advance(0,2),"");
    const auto original=m.gw.pendingSourceReceipts(0);
    GwResolvedSource forcing;forcing.cells={{0,1}};forcing.flow.times={0,.5,1,2};forcing.flow.values={0,0,0,0};
    m.gw.setSources({forcing});m.gw.state().lacc[0]=4;m.gw.fireGwCells(0,2,m.surface,0);
    EXPECT_TRUE(m.gw.pendingSourceReceipts(0).empty());
    int refunds=0;double volume=0;std::array<double,2> mass{};
    for(const auto& r:m.gw.sourceReturns())if(r.kind==SurfaceReturnKind::NUMERICAL_REFUND) {
        ++refunds;EXPECT_EQ(r.available_at,2);volume+=r.volume;
        for(int p=0;p<2;++p){mass[p]+=r.mass[p];EXPECT_NEAR(r.mass[p],r.intake.mass[p]*r.volume/r.intake.volume,1e-14);}
    }
    EXPECT_GT(refunds,int(original.size()));double incoming=0;std::array<double,2> input_mass{};
    for(const auto& r:original){incoming+=r.volume;for(int p=0;p<2;++p)input_mass[p]+=r.mass[p];}
    EXPECT_LE(volume,incoming+1e-13);for(int p=0;p<2;++p)EXPECT_LE(mass[p],input_mass[p]+1e-13);
    ASSERT_EQ(m.stage.advance(2,3),"");EXPECT_TRUE(m.gw.sourceReturns().empty());
}
TEST(CommonSourceStage, WaterOnlyReturnsRemainLocalAcrossCellsAndClosures) {
    for(auto closure:{GwClosure::CLOSED_FORM,GwClosure::ENSLAVED,GwClosure::SIGMA}) {
        AtmosphericInput a;a.closure=closure;Model m;m.setup(true,2,false,false,false,a);m.attach();
        const double initial=m.gw.storage();ASSERT_EQ(m.stage.advance(0,1),"");
        m.gw.state().lacc[0]=10;m.gw.fireGwCells(0,1,m.surface,0);
        double returned=0,mesh_return=0;
        for(const auto& r:m.gw.sourceReturns()) {
            EXPECT_EQ(r.intake.request.cell,0);returned+=r.volume;EXPECT_TRUE(r.mass.empty());
            mesh_return+=r.kind==SurfaceReturnKind::NUMERICAL_REFUND ?
                (r.intake.request.kind==SurfaceDonorKind::MESH ? r.volume : 0) : r.volume*.75;
        }
        EXPECT_GT(returned,0);EXPECT_NEAR(m.gw.storage()+returned+m.gw.state().led_deep,initial+10+m.gw.state().led_infil_in,1e-9);
        const auto before=m.surface.volume;ASSERT_EQ(m.stage.advance(1,2),"");
        EXPECT_NEAR(m.stage.meshReturn(),mesh_return,1e-12); // refunds use their donor, emergence uses area
        EXPECT_LE(m.surface.volume[1],before[1]+1e-12); // no emergence migrates to cell 1
        for(int group=0;group<2;++group)EXPECT_NEAR(m.sources.balanceResidual(group),0,1e-9);
    }
}

TEST(CommonSourceStage, ColumnExcessCannotInventAnIntakeDonor) {
    Model m;mixedPollutants(m);m.attach();
    // Exercise the rejection backstop with an already oversaturated bulk
    // store: no intake exists to refund, so all excess needs a local owner.
    m.gw.state().hu[0]=m.gw.state().theta_s[0]*1.1;
    m.gw.transport().unsat_mass={5,17};m.gw.transport().init_mass={5,17};
    const double initial=m.gw.storage();m.gw.fireGwCells(0,1,m.surface,0);
    double excess=0;for(const auto& r:m.gw.sourceReturns()) {
        EXPECT_NE(r.kind,SurfaceReturnKind::NUMERICAL_REFUND);
        if(r.kind==SurfaceReturnKind::COLUMN_EXCESS)excess+=r.volume;
    }
    EXPECT_GT(excess,0);EXPECT_NEAR(excess,m.gw.state().led_reject,1e-13);
    EXPECT_NEAR(m.gw.storage()+excess+m.gw.state().led_dunne+m.gw.state().led_deep,initial,1e-10);
    for(int p=0;p<2;++p)EXPECT_NEAR(m.gw.transport().residual(p),0,1e-12);
    ASSERT_EQ(m.stage.advance(0,1),"");EXPECT_TRUE(m.stage.returns().empty());
    ASSERT_EQ(m.stage.advance(1,2),"");EXPECT_TRUE(m.gw.sourceReturns().empty());
}

TEST(CommonSourceStage, PartialRejectionPreservesOriginalConcentrationAndAcceptedMass) {
    bool partial=false;
    for(double rise:{.001,.01,.1,.5,.7,.75,.78,.79,.8,1.,4.}) {
        Model m;mixedPollutants(m);m.room(.005);m.attach();ASSERT_EQ(m.stage.advance(0,1),"");
        double incoming=0;for(const auto& r:m.gw.pendingSourceReceipts(0))incoming+=r.volume;
        m.gw.state().lacc[0]=rise;m.gw.fireGwCells(0,1,m.surface,0);
        double refund=0;for(const auto& r:m.gw.sourceReturns())if(r.kind==SurfaceReturnKind::NUMERICAL_REFUND) {
            refund+=r.volume;
            for(int p=0;p<2;++p)EXPECT_NEAR(r.mass[p],r.intake.mass[p]*r.volume/r.intake.volume,1e-14);
        }
        if(refund>1e-12 && refund<incoming-1e-12)partial=true;
        for(int p=0;p<2;++p)EXPECT_NEAR(m.gw.transport().residual(p),0,1e-12);
    }
    EXPECT_TRUE(partial);
}
TEST(CommonSourceStage, CombinedWaterAndPollutantBalanceIncludesPendingAndReturnedTuples) {
    for(auto closure:{GwClosure::CLOSED_FORM,GwClosure::ENSLAVED,GwClosure::SIGMA}) {
    SCOPED_TRACE(int(closure));AtmosphericInput a;a.closure=closure;
    Model m;mixedPollutants(m,1,a);m.attach();const double initial_water=m.gw.storage()+m.surface.volume[0]+m.sources.lids().storedVolume()*ft3;
    const auto totals=[&]() {
        std::array<double,3> t{};t[0]=m.gw.storage()+m.surface.volume[0];
        const auto& r=m.sources.runoff().soa();
        for(int sc=0;sc<2;++sc) {
            t[0]+=r.area[sc]*(r.depth_imperv0[sc]*r.frac_imperv0[sc]+r.depth_imperv1[sc]*r.frac_imperv1[sc]+r.depth_perv[sc]*(1-r.imperv_pct[sc]))*ft3;
            t[0]+=m.sources.pendingVolume(sc)*ft3;
        }
        t[0]+=m.sources.lids().storedVolume()*ft3+m.gw.state().xacc_from_surface[0];
        for(int p=0;p<2;++p) {
            t[p+1]=m.surface.transport.cell_mass[p]+m.gw.transport().sat_mass[p]+m.gw.transport().unsat_mass[p]+m.gw.transport().xacc_from_surface[p];
            for(const auto& b:m.sources.pollutants())for(double mass:b.layers[p])t[p+1]+=mass;
            for(int group=0;group<2;++group)t[p+1]+=m.sources.pendingPollutantMass(group,p);
        }
        for(const auto& q:m.gw.sourceReturns()){t[0]+=q.volume;for(int p=0;p<2;++p)t[p+1]+=q.mass[p];}
        return t;
    };
    const auto balance=[&]() {
        auto current=totals();double inputs=initial_water+m.stage.meshRain()+m.gw.state().led_link;
        double outputs=m.stage.meshEvaporation()+m.gw.state().led_et+m.gw.state().led_deep;
        for(const auto& b:m.sources.ledgers()){inputs+=b.rain*ft3;outputs+=(b.evaporation+b.outside_infiltration+b.outlet)*ft3;}
        EXPECT_NEAR(current[0]+outputs,inputs,1e-9);
        for(int p=0;p<2;++p) {
            double rain=m.surface.transport.gained_rainfall[p],out=m.gw.transport().lost_deep[p]+m.gw.transport().lost_reaction[p];
            for(const auto& b:m.sources.pollutants()){rain+=b.rain[p];out+=b.outside_infiltration[p]+b.outlet[p]+b.removed[p];}
            EXPECT_NEAR(current[p+1]+out,19+rain,1e-9);
        }
    };
    ASSERT_EQ(m.stage.advance(0,1),"");balance();
    ASSERT_EQ(m.stage.advance(1,2),"");balance();
    m.gw.state().lacc[0]=10;m.gw.fireGwCells(0,2,m.surface,0);balance();
    ASSERT_EQ(m.stage.advance(2,3),"");balance();
    m.gw.fireGwCells(0,1,m.surface,2);balance();
    ASSERT_EQ(m.stage.advance(3,4),"");balance();
    }
}

TEST(CommonSourceStage, ReceiptSpeciesChangesFailBeforeGather) {
    Model m;mixedPollutants(m);m.attach();ASSERT_EQ(m.stage.advance(0,1),"");
    const auto water=m.gw.state().xacc_from_surface[0],storage=m.gw.storage();
    const auto mass=m.gw.transport().xacc_from_surface;
    std::reverse(m.gw.transport().row_names.begin(),m.gw.transport().row_names.end());
    EXPECT_THROW(m.gw.fireGwCells(0,1,m.surface,0),std::runtime_error);
    EXPECT_DOUBLE_EQ(m.gw.storage(),storage);EXPECT_DOUBLE_EQ(m.gw.state().xacc_from_surface[0],water);
    EXPECT_EQ(m.gw.transport().xacc_from_surface,mass);
    std::reverse(m.gw.transport().row_names.begin(),m.gw.transport().row_names.end());
    m.gw.fireGwCells(0,1,m.surface,0);EXPECT_TRUE(m.gw.pendingSourceReceipts(0).empty());
}
TEST(CommonSourceStage, DuplicateReceiverBindingLeavesFreshSourcePartitionRetryable) {
    Model m;m.setup();m.attach();
    runoff::SourceWaterDriver fresh;ASSERT_EQ(fresh.initialize(m.context,{0,1},{{0,1},{1,1}}),"");
    CommonSourceStage duplicate;EXPECT_FALSE(duplicate.initialize(m.mesh,m.surface,m.options,m.gw,fresh,m.preview).empty());
    EXPECT_EQ(fresh.configureSpatialCoverage({1,1}),"");
}

TEST(CommonSourceStage, FailedSourceTrialLeavesReadyReturnTuplesAndCreditsUntouched) {
    AtmosphericInput a;a.rain=0;a.impervious={.8,0};Model m;mixedPollutants(m,1,a);
    m.context.subcatches.n_imperv[0]=0;m.context.lid_usage.from_imperv[0]=120;
    m.sources=runoff::SourceWaterDriver{};ASSERT_EQ(m.sources.initialize(m.context,{0,1},{{0,.5},{1,1}}),"");
    m.surface.rainfall[0]=0;m.attach();ASSERT_EQ(m.stage.advance(0,1),"");
    m.gw.state().lacc[0]=10;m.gw.fireGwCells(0,1,m.surface,0);
    const auto returns=m.gw.sourceReturns();ASSERT_FALSE(returns.empty());const auto opening=m.surface.volume;
    for(int attempt=0;attempt<2;++attempt) {
        EXPECT_NE(m.stage.advance(1,2).find("capture exceeds"),std::string::npos);
        EXPECT_EQ(m.stage.completedEnd(),1);EXPECT_EQ(m.surface.volume,opening);
        ASSERT_EQ(m.gw.sourceReturns().size(),returns.size());
        for(std::size_t i=0;i<returns.size();++i) {
            EXPECT_DOUBLE_EQ(m.gw.sourceReturns()[i].volume,returns[i].volume);
            EXPECT_EQ(m.gw.sourceReturns()[i].mass,returns[i].mass);
        }
        for(const auto& b:m.sources.ledgers())EXPECT_DOUBLE_EQ(b.aquifer_return,0);
    }
}

namespace {
std::filesystem::path restartPath(const std::string& name) {
    const char* out=std::getenv("SWMM_R4_RESTART_OUT");
    const auto dir=out?std::filesystem::path(out):std::filesystem::current_path()/"source-restart-evidence";
    std::filesystem::create_directories(dir);return dir/(name+".source-checkpoint");
}
std::string restartBytes(const std::filesystem::path& path) {
    std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};
}
void checkpointEqual(Model& a,Model& b,const std::string& name) {
    auto pa=restartPath(name+"-continuous"),pb=restartPath(name+"-resumed");
    ASSERT_EQ(a.stage.saveCheckpoint(pa.string()),"");ASSERT_EQ(b.stage.saveCheckpoint(pb.string()),"");
    EXPECT_EQ(restartBytes(pa),restartBytes(pb));
}
void advanceRestart(Model& m,double start,double end,bool fire=true) {
    ASSERT_EQ(m.stage.advance(start,end),"");
    if(fire)m.gw.fireGwCells(0,end-start,m.surface,start);
}
}
TEST(CommonSourceStage, DiskRestartPreservesPendingIntakeMassAndEtWithoutFutureGather) {
    AtmosphericInput a;a.pet={.0001,.0002};a.et="BOUNDARY_ET";a.inside={.75,.25};a.impervious={.2,.4};
    Model continuous;mixedPollutants(continuous,2,a);continuous.room(.001,0);continuous.attach();
    ASSERT_EQ(continuous.stage.advance(0,1),"");ASSERT_GT(continuous.gw.state().xacc_from_surface[0],0);
    ASSERT_FALSE(continuous.gw.pendingSourceReceipts(0).empty());
    const auto file=restartPath("pending-intake-et");ASSERT_EQ(continuous.stage.saveCheckpoint(file.string()),"");
    Model resumed;mixedPollutants(resumed,2,a);resumed.room(.001,0);resumed.attach();
    ASSERT_EQ(resumed.stage.loadCheckpoint(file.string()),"");checkpointEqual(continuous,resumed,"pending-initial");
    const auto before=resumed.gw.state().xacc_from_surface;
    EXPECT_THROW(resumed.gw.fireGwCells(0,.5,resumed.surface,0),std::runtime_error);
    EXPECT_THROW(resumed.gw.fireGwCells(0,1,resumed.surface,std::numeric_limits<double>::quiet_NaN()),std::runtime_error);
    EXPECT_EQ(before,resumed.gw.state().xacc_from_surface);EXPECT_FALSE(resumed.stage.advance(0,1).empty());
    resumed.gw.fireGwCells(0,1,resumed.surface,0);continuous.gw.fireGwCells(0,1,continuous.surface,0);
    for(const auto [start,end]:{std::pair{1.,1.25},std::pair{1.25,2.},std::pair{2.,4.}}) {
        advanceRestart(continuous,start,end);advanceRestart(resumed,start,end);checkpointEqual(continuous,resumed,"pending-"+std::to_string(end));
    }
    etBalance(continuous);etBalance(resumed);
    for(int g=0;g<2;++g){EXPECT_NEAR(resumed.sources.balanceResidual(g),0,1e-9);
        for(int p=0;p<2;++p)EXPECT_NEAR(resumed.sources.pollutantBalanceResidual(g,p),0,1e-10);}
}
TEST(CommonSourceStage, DiskRestartAdoptsRefundAndPhysicalMassExactlyOnceAcrossClosures) {
    for(auto closure:{GwClosure::CLOSED_FORM,GwClosure::ENSLAVED,GwClosure::SIGMA}) {
        SCOPED_TRACE(int(closure));AtmosphericInput a;a.closure=closure;
        Model continuous;mixedPollutants(continuous,1,a);continuous.room(.005);continuous.attach();
        ASSERT_EQ(continuous.stage.advance(0,1),"");continuous.gw.state().lacc[0]=10;
        continuous.gw.fireGwCells(0,1,continuous.surface,0);ASSERT_FALSE(continuous.gw.sourceReturns().empty());
        const auto file=restartPath("ready-returns-"+std::to_string(int(closure)));
        ASSERT_EQ(continuous.stage.saveCheckpoint(file.string()),"");
        Model resumed;mixedPollutants(resumed,1,a);resumed.room(.005);resumed.attach();ASSERT_EQ(resumed.stage.loadCheckpoint(file.string()),"");
        checkpointEqual(continuous,resumed,"returns-initial-"+std::to_string(int(closure)));
        const auto water=resumed.gw.storage();const auto mass=resumed.gw.transport().sat_mass;
        EXPECT_THROW(resumed.gw.fireGwCells(0,1,resumed.surface,0),std::runtime_error);
        EXPECT_DOUBLE_EQ(resumed.gw.storage(),water);EXPECT_EQ(resumed.gw.transport().sat_mass,mass);
        advanceRestart(continuous,1,2,false);advanceRestart(resumed,1,2,false);
        EXPECT_TRUE(resumed.gw.sourceReturns().empty());ASSERT_FALSE(resumed.stage.returns().empty());
        const auto credit=resumed.stage.meshReturn();advanceRestart(continuous,2,3,false);advanceRestart(resumed,2,3,false);
        EXPECT_TRUE(resumed.stage.returns().empty());EXPECT_DOUBLE_EQ(resumed.stage.meshReturn(),credit);
        checkpointEqual(continuous,resumed,"returns-final-"+std::to_string(int(closure)));
    }
}
TEST(CommonSourceStage, RestartMatchesNativeLidHistoriesAndConnectedPartialFootprints) {
    for(const auto& type:{"BC","RG","PP","GR","IT","VS","RB"})for(int method:{0,1,2,3,4}) {
        SCOPED_TRACE(std::string(type)+":"+std::to_string(method));AtmosphericInput a;a.lid_type=type;a.connected=true;a.inside={.5,.25};
        a.barrel=std::string(type)=="RB";a.pet={.00001,.00003};a.impervious={.4,.3};
        const auto configure=[&](Model& m){m.setup(true,1,false,false,false,a);
            m.context.subcatches.infil_model.assign(2,method);
            m.context.subcatches.infil_p1.assign(2,method<=1?3.5:method==2?3.5:75);
            m.context.subcatches.infil_p2.assign(2,.5);m.context.subcatches.infil_p3.assign(2,method<=1?4:method==2?.26:7);
            m.sources=runoff::SourceWaterDriver{};
            ASSERT_EQ(m.sources.initialize(m.context,{0,1},{{0,.5},{1,1}}),"");m.attach();};
        Model continuous;configure(continuous);advanceRestart(continuous,0,.5);advanceRestart(continuous,.5,2);
        const auto file=restartPath(std::string("native-")+type+"-"+std::to_string(method));ASSERT_EQ(continuous.stage.saveCheckpoint(file.string()),"");
        Model resumed;configure(resumed);ASSERT_EQ(resumed.stage.loadCheckpoint(file.string()),"");
        for(const auto [start,end]:{std::pair{2.,3.25},std::pair{3.25,6.}}){advanceRestart(continuous,start,end);advanceRestart(resumed,start,end);}
        checkpointEqual(continuous,resumed,std::string("native-final-")+type+"-"+std::to_string(method));
    }
}
TEST(CommonSourceStage, RestartPreservesForcingKnotsAndMonthBoundaryWithoutReplay) {
    const auto configure=[](Model& m){m.setup(true);
        m.context.options.start_date=datetime::encodeDate(2026,10,31)+datetime::encodeTime(23,59,58);
        const double t=m.context.options.start_date;m.context.gages.interval_sec[0]=1;
        m.context.tables.tables[0].x={t,datetime::addSeconds(t,1),datetime::addSeconds(t,2),datetime::addSeconds(t,3)};
        m.context.tables.tables[0].y={432,0,864,0};m.context.adjust_rain[10]=.25;
        m.sources=runoff::SourceWaterDriver{};ASSERT_EQ(m.sources.initialize(m.context,{0,1},{{0,.5},{1,1}}),"");
        GwResolvedSource q;q.cells={{0,1}};q.flow.times={0,.5,1.5,2.5,5};q.flow.values={0,.001,0,.002,0};m.gw.setSources({q});m.attach();};
    Model continuous;configure(continuous);advanceRestart(continuous,0,2);
    const auto file=restartPath("forcing-month");ASSERT_EQ(continuous.stage.saveCheckpoint(file.string()),"");
    Model resumed;configure(resumed);ASSERT_EQ(resumed.stage.loadCheckpoint(file.string()),"");
    advanceRestart(continuous,2,3.25);advanceRestart(resumed,2,3.25);advanceRestart(continuous,3.25,5);advanceRestart(resumed,3.25,5);
    checkpointEqual(continuous,resumed,"forcing-month-final");
}
TEST(CommonSourceStage, CheckpointRefusesOutstandingTrialsFutureFiringsAndChangedModels) {
    Model m;mixedPollutants(m);m.attach();const auto file=restartPath("boundary-refusals");
    ASSERT_EQ(m.stage.saveCheckpoint(file.string()),"");const auto original=restartBytes(file);
    runoff::SourceWaterDriver::BottomCeiling ceiling=[](int,int,double,double){return 0.;};
    runoff::RunoffSolver::InfiltrationBoundary boundary=[](int,double,double,double& rate){rate=0;return true;};
    ASSERT_EQ(m.sources.stage(0,1,1,&boundary,&ceiling),"");EXPECT_FALSE(m.stage.saveCheckpoint(file.string()).empty());
    EXPECT_EQ(restartBytes(file),original);m.sources.cancel();
    m.gw.fireGwCells(0,1,m.surface,0);EXPECT_FALSE(m.stage.saveCheckpoint(file.string()).empty());
    EXPECT_EQ(restartBytes(file),original);ASSERT_EQ(m.stage.advance(0,1),"");ASSERT_EQ(m.stage.saveCheckpoint(file.string()),"");
    const auto accepted=restartBytes(file);const double meshVolume=m.surface.volume[0];m.surface.volume[0]=-1;
    EXPECT_FALSE(m.stage.saveCheckpoint(file.string()).empty());EXPECT_EQ(restartBytes(file),accepted);
    m.surface.volume[0]=meshVolume;
    const auto blocked=restartPath("publication-directory");std::filesystem::create_directories(blocked);
    {std::ofstream existing(blocked/"existing.txt");existing<<"preserve this";}
    EXPECT_FALSE(m.stage.saveCheckpoint(blocked.string()).empty());
    EXPECT_TRUE(std::filesystem::is_directory(blocked));EXPECT_EQ(restartBytes(blocked/"existing.txt"),"preserve this");
    EXPECT_FALSE(m.stage.saveCheckpoint((restartPath("missing")/"no-parent").string()).empty());
    m.gw.state().Ks[0]*=2;
    EXPECT_FALSE(m.stage.saveCheckpoint(file.string()).empty());EXPECT_EQ(restartBytes(file),accepted);
}
TEST(CommonSourceStage, CheckpointModelMismatchAndCorruptFilesLeaveFreshTargetUnchanged) {
    Model original;mixedPollutants(original);original.attach();advanceRestart(original,0,1);
    const auto file=restartPath("atomic-source");ASSERT_EQ(original.stage.saveCheckpoint(file.string()),"");const auto bytes=restartBytes(file);
    for(int change=0;change<6;++change) {
        Model target;AtmosphericInput a;if(change==0)a.rain=433;if(change==1)a.inside={.5,1};if(change==2)a.lid_bottom=431;
        mixedPollutants(target,1,a);if(change==3)target.gw.state().alpha[0]*=2;if(change==4)target.mesh.vz[0]+=.1;
        if(change==5){target.context.forcing.subcatch_evap_value[0]=.001;target.sources=runoff::SourceWaterDriver{};
            ASSERT_EQ(target.sources.initialize(target.context,{0,1},{{0,.5},{1,1}}),"");}
        target.attach();const auto before=restartPath("mismatch-before-"+std::to_string(change));ASSERT_EQ(target.stage.saveCheckpoint(before.string()),"");
        EXPECT_FALSE(target.stage.loadCheckpoint(file.string()).empty());const auto after=restartPath("mismatch-after-"+std::to_string(change));
        ASSERT_EQ(target.stage.saveCheckpoint(after.string()),"");EXPECT_EQ(restartBytes(before),restartBytes(after));
    }
    for(int corruption=0;corruption<5;++corruption) {
        auto bad=bytes;if(corruption==0)bad.resize(bad.size()/2);if(corruption==1)bad.back()^=1;
        if(corruption==2)bad[8]^=1;if(corruption==3)bad.append("extra");if(corruption==4)bad.assign(8,'\xff');
        const auto path=restartPath("corrupt-"+std::to_string(corruption));{std::ofstream out(path,std::ios::binary);out.write(bad.data(),bad.size());}
        Model target;mixedPollutants(target);target.attach();const auto before=restartPath("corrupt-before");ASSERT_EQ(target.stage.saveCheckpoint(before.string()),"");
        EXPECT_FALSE(target.stage.loadCheckpoint(path.string()).empty());const auto after=restartPath("corrupt-after");ASSERT_EQ(target.stage.saveCheckpoint(after.string()),"");
        EXPECT_EQ(restartBytes(before),restartBytes(after));ASSERT_EQ(target.stage.loadCheckpoint(file.string()),"");
        EXPECT_FALSE(target.stage.loadCheckpoint(file.string()).empty()); // accepted intervals cannot be overwritten/replayed
    }
}

TEST(CommonSourceStage, RestartKeepsPrivatePairedReturnsCapturedRoutingAndNativeHistories) {
    const auto configure=[](Model& m){AtmosphericInput a;a.connected=true;a.impervious={.4,.3};a.inside={.5,.25};
        a.lid_bottom=0;mixedPollutants(m,1,a);m.context.subcatches.n_perv.assign(2,0);m.context.subcatches.n_imperv.assign(2,0);m.context.lid_usage.from_imperv[0]=60;m.context.lid_usage.from_perv[0]=20;
        m.context.lid_usage.to_perv[0]=1;m.context.lid_usage.drain_to[0]="S1";m.context.lid_controls.drain[0][0]=43.2;
        m.sources=runoff::SourceWaterDriver{};ASSERT_EQ(m.sources.initialize(m.context,{0,1},{{0,.5},{1,1}}),"");m.attach();};
    Model continuous;configure(continuous);advanceRestart(continuous,0,20);
    runoff::SourceSurfaceReturn a;a.source=0;a.pool=2;a.volume=.1;a.mass={2,7};
    auto b=a;b.type=int(lid::LIDType::INFIL_TRENCH);b.unit=0;
    ASSERT_EQ(continuous.sources.creditSurfaceReturns(20,{a,b}),"");ASSERT_GE(continuous.sources.pendingVolume(0),.2);
    EXPECT_GT(continuous.sources.ledgers()[0].captured,0);

    const auto file=restartPath("private-paired-routing");ASSERT_EQ(continuous.stage.saveCheckpoint(file.string()),"");
    Model resumed;configure(resumed);ASSERT_EQ(resumed.stage.loadCheckpoint(file.string()),"");
    EXPECT_DOUBLE_EQ(resumed.sources.pendingVolume(0),continuous.sources.pendingVolume(0));
    for(const auto [start,end]:{std::pair{20.,30.},std::pair{30.,35.},std::pair{35.,50.}}){advanceRestart(continuous,start,end);advanceRestart(resumed,start,end);}
    checkpointEqual(continuous,resumed,"private-paired-routing-final");
    EXPECT_GT(resumed.sources.ledgers()[1].runon,0);
    for(int p=0;p<2;++p)EXPECT_NEAR(resumed.sources.pollutantBalanceResidual(0,p),0,1e-9);
}
TEST(CommonSourceStage, RestartPreservesUnequalGroundwaterCadencesAndLateralNodeLinkQueues) {
    const auto configure=[](Model& m){m.setup(true,2,false,true);m.gw.state().dt_cell={1,2};m.gw.assignTiers(1,2);m.attach();};
    Model continuous;configure(continuous);
    ASSERT_EQ(continuous.stage.advance(0,1),"");continuous.gw.fireGwFaces(0,1);continuous.gw.fireGwCells(0,1,continuous.surface,0);
    // The coarser owner has not gathered its completed source tuple yet.
    ASSERT_FALSE(continuous.gw.pendingSourceReceipts(1).empty());
    continuous.gw.state().lacc[1]=.001;continuous.gw.state().nacc[0]=.0001;
    const auto file=restartPath("unequal-cadence");ASSERT_EQ(continuous.stage.saveCheckpoint(file.string()),"");
    Model resumed;configure(resumed);ASSERT_EQ(resumed.stage.loadCheckpoint(file.string()),"");
    for(Model* m:{&continuous,&resumed}) {
        ASSERT_EQ(m->stage.advance(1,2),"");m->gw.fireGwFaces(0,1);m->gw.fireGwCells(0,1,m->surface,1);m->gw.fireGwCells(1,2,m->surface,0);
        ASSERT_EQ(m->stage.advance(2,3),"");m->gw.fireGwFaces(0,1);m->gw.fireGwCells(0,1,m->surface,2);
        ASSERT_EQ(m->stage.advance(3,4),"");m->gw.fireGwFaces(0,1);m->gw.fireGwCells(0,1,m->surface,3);m->gw.fireGwCells(1,2,m->surface,2);
    }
    checkpointEqual(continuous,resumed,"unequal-cadence-final");
}
namespace {
uint32_t restartCrc(const std::string& bytes) {
    uint32_t crc=~uint32_t(0);for(unsigned char c:bytes){crc^=c;for(int k=0;k<8;++k)crc=(crc>>1)^((crc&1)?0xedb88320u:0u);}return ~crc;
}
}
TEST(CommonSourceStage, SemanticallyInvalidCheckpointWithValidChecksumIsRejectedAtomically) {
    Model original;mixedPollutants(original);original.attach();advanceRestart(original,0,1);
    const auto file=restartPath("semantic-original");ASSERT_EQ(original.stage.saveCheckpoint(file.string()),"");
    for(int change=0;change<5;++change) {
        detail::CompletedSourceArchive reader(restartBytes(file));std::string magic,model,payload;uint32_t version=0,endian=0,longBytes=0,doubleBytes=0,crc=0;
        reader(magic,version,endian,longBytes,doubleBytes,model,payload,crc);reader.finish();
        if(change==0){double future=2;std::memcpy(payload.data(),&future,sizeof(future));}
        if(change==1){double invalid=std::numeric_limits<double>::quiet_NaN();std::memcpy(payload.data(),&invalid,sizeof(invalid));}
        if(change==2){version=999;}
        if(change==3){uint64_t invalid=UINT64_MAX;const auto receiptCount=sizeof(double)+sizeof(long)+3*sizeof(double);std::memcpy(payload.data()+receiptCount,&invalid,sizeof(invalid));}
        if(change==4) {
            // Metadata prefix locates the first ET receipt without relying on struct padding.
            detail::CompletedSourceArchive prefix;auto end=original.stage.completedEnd();auto intervals=original.stage.intervals();
            auto rain=original.stage.meshRain(),evap=original.stage.meshEvaporation(),back=original.stage.meshReturn();
            prefix(end,intervals,rain,evap,back);auto receipts=original.stage.receipts();
            prefix.sequence(receipts,[&](auto& r){auto& q=r.request;prefix(q.kind,q.source,q.unit,q.cell,q.area,q.pond,q.water_ceiling,q.candidate,
                r.start,r.end,r.volume,r.mass);});
            const auto offset=prefix.bytes().size()+sizeof(uint64_t);auto invalid=SurfaceEtOwner::LID;
            std::memcpy(payload.data()+offset,&invalid,sizeof(invalid)); // mesh tuple cannot identify a LID usage
        }
        crc=restartCrc(payload);detail::CompletedSourceArchive writer;writer(magic,version,endian,longBytes,doubleBytes,model,payload,crc);
        const auto bad=restartPath("semantic-invalid-"+std::to_string(change));{std::ofstream out(bad,std::ios::binary);out.write(writer.bytes().data(),writer.bytes().size());}
        Model target;mixedPollutants(target);target.attach();const auto before=restartPath("semantic-before");ASSERT_EQ(target.stage.saveCheckpoint(before.string()),"");
        EXPECT_FALSE(target.stage.loadCheckpoint(bad.string()).empty());const auto after=restartPath("semantic-after");ASSERT_EQ(target.stage.saveCheckpoint(after.string()),"");
        EXPECT_EQ(restartBytes(before),restartBytes(after));ASSERT_EQ(target.stage.loadCheckpoint(file.string()),"");
    }
}

namespace {
std::filesystem::path sourceResultPath(const std::string& name) {
    const char* out=std::getenv("SWMM_R4_RESULTS_OUT");
    const auto dir=out?std::filesystem::path(out):std::filesystem::current_path()/"source-result-evidence";
    std::filesystem::create_directories(dir);return dir/(name+".h5");
}
void resultEqual(const SourceResultHistory& a,const SourceResultHistory& b) {
    EXPECT_EQ(a.sourceNames(),b.sourceNames());EXPECT_EQ(a.pollutantNames(),b.pollutantNames());
    EXPECT_EQ(a.pollutantMassUnits(),b.pollutantMassUnits());ASSERT_EQ(a.tables().size(),b.tables().size());
    for(const auto& [name,t]:a.tables()) {
        const auto& u=b.tables().at(name);EXPECT_EQ(t.fields,u.fields);EXPECT_EQ(t.units,u.units);EXPECT_EQ(t.columns,u.columns)<<name;
    }
}
}
TEST(CommonSourceStage, SourceHistoryKeepsAcceptedIntervalsEtAndSourceBalances) {
    AtmosphericInput a;a.inside={.75,.25};a.impervious={.2,.4};a.pet={.0001,.0002};a.et="BOUNDARY_ET";
    Model m;mixedPollutants(m,2,a);m.attach();SourceResultHistory history;
    const auto before=restartPath("history-readonly-before"),after=restartPath("history-readonly-after");
    ASSERT_EQ(m.stage.saveCheckpoint(before.string()),"");ASSERT_EQ(history.append(m.stage),"");
    ASSERT_EQ(m.stage.saveCheckpoint(after.string()),"");EXPECT_EQ(restartBytes(before),restartBytes(after));
    for(const auto [start,end]:{std::pair{0.,.5},std::pair{.5,2.},std::pair{2.,35.}}) {
        advanceRestart(m,start,end);ASSERT_EQ(history.append(m.stage),"");etBalance(m);
    }
    const auto& b=history.tables().at("boundaries");
    EXPECT_EQ(b.columns[2],(std::vector<double>{0,0,.5,2}));EXPECT_EQ(b.columns[3],(std::vector<double>{0,.5,2,35}));
    const auto& water=history.tables().at("source_water").columns;
    for(std::size_t r=0;r<water[0].size();++r)EXPECT_NEAR(water[7][r],water[8][r]+water[9][r],1e-13);
    const auto& residual=history.tables().at("group_water").columns[3];for(double v:residual)EXPECT_NEAR(v,0,1e-12);
    for(double v:history.tables().at("group_mass").columns[4])EXPECT_NEAR(v,0,1e-10);
    EXPECT_FALSE(history.tables().at("et").columns[0].empty());EXPECT_EQ(history.pollutantMassUnits(),(std::vector<std::string>{"mg/L*m3","count/L*m3"}));
    EXPECT_EQ(history.tables().at("receiver").columns[3].front(),-1); // initially unpublished refresh
    const auto& footprint=history.tables().at("footprints").columns;
    std::array<double,2> cell_area{};double outside=0;
    for(std::size_t r=0;r<footprint[0].size();++r) {
        if(footprint[5][r]<0)outside+=footprint[6][r];else cell_area[int(footprint[5][r])]+=footprint[6][r];
    }
    EXPECT_NEAR(outside,1,1e-12);for(int cell=0;cell<2;++cell)EXPECT_NEAR(cell_area[cell],m.mesh.tri_area[cell],1e-12);
}
TEST(CommonSourceStage, SourceHistoryRetainsPendingIntakesAtSlowerReceiverCadence) {
    Model m;mixedPollutants(m);m.room(.005);m.attach();SourceResultHistory history;
    ASSERT_EQ(history.append(m.stage),"");ASSERT_EQ(m.stage.advance(0,.5),"");ASSERT_EQ(history.append(m.stage),"");
    EXPECT_EQ(history.tables().at("receiver").columns[2].back(),0);
    const auto count=history.tables().at("pending_intake").rows();ASSERT_GT(count,0);
    ASSERT_EQ(m.stage.advance(.5,2),"");ASSERT_EQ(history.append(m.stage),"");
    EXPECT_GT(history.tables().at("pending_intake").rows(),count);
    double volume=0;for(const auto& r:m.gw.pendingSourceReceipts(0))volume+=r.volume;
    const auto& t=history.tables().at("pending_intake").columns;double saved=0;
    for(std::size_t r=0;r<t[0].size();++r)if(t[0][r]==2)saved+=t[12][r];EXPECT_NEAR(saved,volume,1e-14);
    m.gw.fireGwCells(0,2,m.surface,0);ASSERT_EQ(m.stage.advance(2,3),"");ASSERT_EQ(history.append(m.stage),"");
    EXPECT_EQ(history.tables().at("receiver").columns[2].back(),2);
}
TEST(CommonSourceStage, SourceHistoryDistinguishesRefundAvailabilityAndAdoption) {
    Model m;mixedPollutants(m);m.room(.005);m.attach();SourceResultHistory history;
    ASSERT_EQ(history.append(m.stage),"");ASSERT_EQ(m.stage.advance(0,1),"");
    m.gw.state().lacc[0]=4;m.gw.fireGwCells(0,1,m.surface,0);ASSERT_FALSE(m.gw.sourceReturns().empty());
    ASSERT_EQ(history.append(m.stage),"");ASSERT_GT(history.tables().at("pending_return").rows(),0);
    ASSERT_EQ(m.stage.advance(1,2),"");ASSERT_EQ(history.append(m.stage),"");
    const auto& t=history.tables().at("return").columns;int refunds=0,physical=0;
    for(std::size_t r=0;r<t[0].size();++r) {
        EXPECT_EQ(t[4][r],1);EXPECT_EQ(t[5][r],1);
        if(t[2][r]==0){++refunds;EXPECT_GE(t[6][r],0);EXPECT_GT(t[11][r],0);}
        else{++physical;EXPECT_EQ(t[7][r],-1);}
    }
    EXPECT_GT(refunds,0);EXPECT_GT(physical,0);
    const auto count=t[0].size();ASSERT_EQ(m.stage.advance(2,3),"");ASSERT_EQ(history.append(m.stage),"");
    EXPECT_EQ(history.tables().at("return").rows(),count); // no replay of adopted credits
}
TEST(CommonSourceStage, SourceHistoryRefusesSkippedDuplicateTrialAndChangedModelsAtomically) {
    Model m;mixedPollutants(m);m.attach();SourceResultHistory history;ASSERT_EQ(history.append(m.stage),"");
    const auto initial=history;EXPECT_FALSE(history.append(m.stage).empty());resultEqual(history,initial);
    runoff::SourceWaterDriver::BottomCeiling ceiling=[](int,int,double,double){return 0.;};
    runoff::RunoffSolver::InfiltrationBoundary boundary=[](int,double,double,double& rate){rate=0;return true;};
    ASSERT_EQ(m.sources.stage(0,.5,.5,&boundary,&ceiling),"");EXPECT_FALSE(history.append(m.stage).empty());resultEqual(history,initial);m.sources.cancel();
    Model other;mixedPollutants(other);other.attach();EXPECT_FALSE(history.append(other.stage).empty());resultEqual(history,initial);
    m.mesh.mannings_n[0]*=2;EXPECT_FALSE(history.append(m.stage).empty());resultEqual(history,initial);m.mesh.mannings_n[0]/=2;
    ASSERT_EQ(m.stage.advance(0,.5),"");ASSERT_EQ(m.stage.advance(.5,1),"");EXPECT_FALSE(history.append(m.stage).empty());resultEqual(history,initial);
    SourceResultHistory late;EXPECT_FALSE(late.append(m.stage).empty());EXPECT_TRUE(late.tables().empty());
}
#ifdef OPENSWMM_HAS_HDF5_MODEL
TEST(CommonSourceStage, SourceHistoryHdf5RoundTripPreservesAllColumnsAndNativeUnits) {
    Model m;mixedPollutants(m);m.attach();SourceResultHistory history;ASSERT_EQ(history.append(m.stage),"");
    advanceRestart(m,0,.75);ASSERT_EQ(history.append(m.stage),"");advanceRestart(m,.75,2);ASSERT_EQ(history.append(m.stage),"");
    const auto path=sourceResultPath("round-trip");ASSERT_EQ(history.save(path.string()),"");
    SourceResultHistory restored;bool available=false;ASSERT_EQ(SourceResultHistory::read(path.string(),restored,available),"");
    ASSERT_TRUE(available);resultEqual(history,restored);EXPECT_FALSE(restored.append(m.stage).empty());
    ASSERT_EQ(restored.save(sourceResultPath("copied-history").string()),"");
}
TEST(CommonSourceStage, SourceHistoryHdf5ReportsAbsentInsteadOfZeroAndProtectsModelFiles) {
    Model m;m.setup();m.attach();SourceResultHistory history;ASSERT_EQ(history.append(m.stage),"");const auto original=history;
    const auto path=sourceResultPath("older-model");ASSERT_EQ(h5io::write_model(path.string(),m.context),0);
    const auto bytes=restartBytes(path);bool available=true;
    ASSERT_EQ(SourceResultHistory::read(path.string(),history,available),"");EXPECT_FALSE(available);resultEqual(history,original);
    EXPECT_FALSE(history.save(path.string()).empty());EXPECT_EQ(restartBytes(path),bytes);
}
TEST(CommonSourceStage, SourceHistoryHdf5FailedPublicationPreservesExistingArtifact) {
    Model m;mixedPollutants(m);m.attach();SourceResultHistory history;ASSERT_EQ(history.append(m.stage),"");
    advanceRestart(m,0,1);ASSERT_EQ(history.append(m.stage),"");const auto path=sourceResultPath("publication");
    ASSERT_EQ(history.save(path.string()),"");const auto bytes=restartBytes(path);
    auto invalid=history;auto& tables=const_cast<std::map<std::string,SourceResultTable>&>(invalid.tables());
    tables.at("source_water").columns[5].pop_back();EXPECT_FALSE(invalid.save(path.string()).empty());EXPECT_EQ(restartBytes(path),bytes);
    EXPECT_FALSE(history.save((sourceResultPath("missing-parent")/"history.h5").string()).empty());
    SourceResultHistory restored;bool available=false;ASSERT_EQ(SourceResultHistory::read(path.string(),restored,available),"");resultEqual(history,restored);
}
TEST(CommonSourceStage, SourceHistoryHdf5RejectsTruncationAndUnknownUnitsWithoutPartialLoad) {
    Model m;mixedPollutants(m);m.attach();SourceResultHistory history;ASSERT_EQ(history.append(m.stage),"");
    advanceRestart(m,0,1);ASSERT_EQ(history.append(m.stage),"");const auto valid=sourceResultPath("corruption-original");ASSERT_EQ(history.save(valid.string()),"");
    const auto original=history;const auto bytes=restartBytes(valid);
    for(int mode=0;mode<3;++mode) {
        auto corrupt=bytes;
        if(mode==0)corrupt.resize(bytes.size()/2);
        if(mode==1){const auto at=corrupt.find("mg/L*m3");ASSERT_NE(at,std::string::npos);corrupt.replace(at,7,"kg/L*m3");}
        if(mode==2){const auto at=corrupt.find("accepted source intervals");ASSERT_NE(at,std::string::npos);corrupt[at]='X';}
        const auto path=sourceResultPath("corrupt-"+std::to_string(mode));{std::ofstream out(path,std::ios::binary);out.write(corrupt.data(),corrupt.size());}
        bool available=true;EXPECT_FALSE(SourceResultHistory::read(path.string(),history,available).empty())<<mode;
        EXPECT_FALSE(available);resultEqual(history,original);
    }
}
TEST(CommonSourceStage, SourceHistoryHdf5RejectsIncompleteEndpointsAndInvalidTransferSemantics) {
    Model m;mixedPollutants(m);m.room(.005);m.attach();SourceResultHistory history;ASSERT_EQ(history.append(m.stage),"");
    ASSERT_EQ(m.stage.advance(0,1),"");m.gw.state().lacc[0]=4;m.gw.fireGwCells(0,1,m.surface,0);ASSERT_EQ(history.append(m.stage),"");
    ASSERT_EQ(m.stage.advance(1,2),"");ASSERT_EQ(history.append(m.stage),"");
    ASSERT_EQ(history.save(sourceResultPath("semantic-reference").string()),"");
    for(int mode=0;mode<6;++mode) {
        auto invalid=history;auto& t=const_cast<std::map<std::string,SourceResultTable>&>(invalid.tables());
        if(mode==0)for(auto& c:t.at("source_water").columns)c.pop_back();
        if(mode==1)t.at("receiver").columns[2].back()=100;
        if(mode==2)t.at("return").columns[5].back()=0;
        if(mode==3){const auto& kinds=t.at("return").columns[2];const auto at=std::find(kinds.begin(),kinds.end(),0);ASSERT_NE(at,kinds.end());
            t.at("return_mass").columns[4][std::size_t(at-kinds.begin())*2]+=1;}
        if(mode==4)t.at("et").columns[9].back()+=1;
        if(mode==5)t.at("intake_mass").columns[2].back()=99;
        EXPECT_FALSE(invalid.save(sourceResultPath("invalid-semantics-"+std::to_string(mode)).string()).empty())<<mode;
    }
}
#endif

TEST(CommonSourceStage, MarcherHistoryCapturesEveryAcceptedIntervalAfterReceiverFirings) {
    AtmosphericInput a;a.pet={.0001,.0002};a.et="BOUNDARY_ET";Model m;mixedPollutants(m,2,a);m.attach(true);
    SourceResultHistory history;m.marcher.setSourceResultHistory(history);
    EXPECT_THROW(m.marcher.setSourceResultHistory(history),std::invalid_argument);
    for(const auto [start,end]:{std::pair{0.,.7},std::pair{.7,4.25},std::pair{4.25,7.}})
        EXPECT_DOUBLE_EQ(m.marcher.advance(start,end),end);
    const auto& b=history.tables().at("boundaries").columns;
    EXPECT_EQ(b[0].size(),std::size_t(m.stage.intervals()+1));EXPECT_DOUBLE_EQ(b[3].back(),7);
    const auto& receiver=history.tables().at("receiver").columns;
    for(std::size_t r=0;r<receiver[0].size();++r)EXPECT_LE(receiver[2][r],b[3][std::size_t(receiver[0][r])]);
    EXPECT_GT(receiver[2].back(),0);etBalance(m);
    const auto original=history;m.marcher.finalize();resultEqual(history,original);
}
TEST(CommonSourceStage, MarcherHistoryRegistrationRefusesMissingStageAndLateCapture) {
    Model no_stage;no_stage.setup();no_stage.marcher.initialize(no_stage.mesh,no_stage.surface,no_stage.options);
    SourceResultHistory history;EXPECT_THROW(no_stage.marcher.setSourceResultHistory(history),std::invalid_argument);EXPECT_TRUE(history.tables().empty());
    Model late;mixedPollutants(late);late.attach(true);late.marcher.advance(0,1);
    EXPECT_THROW(late.marcher.setSourceResultHistory(history),std::invalid_argument);EXPECT_TRUE(history.tables().empty());
}
#ifdef OPENSWMM_HAS_HDF5_MODEL
TEST(CommonSourceStage, SourceHistoryContinuesOnlyFromExactRestoredCheckpointAndPrefix) {
    AtmosphericInput a;a.pet={.0001,.0002};a.et="BOUNDARY_ET";Model continuous;mixedPollutants(continuous,2,a);continuous.attach();
    SourceResultHistory history;ASSERT_EQ(history.append(continuous.stage),"");advanceRestart(continuous,0,1,false);ASSERT_EQ(history.append(continuous.stage),"");
    const auto checkpoint=restartPath("history-continuation"),file=sourceResultPath("history-prefix");
    ASSERT_EQ(continuous.stage.saveCheckpoint(checkpoint.string()),"");ASSERT_EQ(history.save(file.string()),"");
    Model resumed;mixedPollutants(resumed,2,a);resumed.attach();ASSERT_EQ(resumed.stage.loadCheckpoint(checkpoint.string()),"");
    SourceResultHistory prefix;bool available=false;ASSERT_EQ(SourceResultHistory::read(file.string(),prefix,available),"");ASSERT_TRUE(available);
    ASSERT_EQ(prefix.resume(resumed.stage),"");resultEqual(history,prefix);EXPECT_FALSE(prefix.append(resumed.stage).empty());
    for(const auto [start,end]:{std::pair{1.,2.},std::pair{2.,4.25}}) {
        advanceRestart(continuous,start,end);advanceRestart(resumed,start,end);
        ASSERT_EQ(history.append(continuous.stage),"");ASSERT_EQ(prefix.append(resumed.stage),"");resultEqual(history,prefix);
    }
    checkpointEqual(continuous,resumed,"history-continuation-final");ASSERT_EQ(prefix.save(sourceResultPath("history-continued").string()),"");
}
TEST(CommonSourceStage, SourceHistoryContinuationRefusesWrongStateClockAndProjectionAtomically) {
    Model original;mixedPollutants(original);original.attach();SourceResultHistory history;ASSERT_EQ(history.append(original.stage),"");
    advanceRestart(original,0,1);ASSERT_EQ(history.append(original.stage),"");const auto checkpoint=restartPath("history-mismatch");ASSERT_EQ(original.stage.saveCheckpoint(checkpoint.string()),"");
    Model target;mixedPollutants(target);target.attach();const auto before=history;EXPECT_FALSE(history.resume(target.stage).empty());resultEqual(history,before);
    ASSERT_EQ(target.stage.loadCheckpoint(checkpoint.string()),"");target.surface.volume[0]+=.001;
    EXPECT_FALSE(history.resume(target.stage).empty());resultEqual(history,before);target.surface.volume[0]-=.001;
    auto edited=history;auto& tables=const_cast<std::map<std::string,SourceResultTable>&>(edited.tables());
    tables.at("source_water").columns[12].back()+=.001; // diagnostic capture total; balance unchanged
    const auto changed=edited;EXPECT_FALSE(edited.resume(target.stage).empty());resultEqual(edited,changed);
    advanceRestart(target,1,2);EXPECT_FALSE(history.resume(target.stage).empty());resultEqual(history,before);
}
TEST(CommonSourceStage, InitialSourceHistoryCanRebindWithoutAddingAnInterval) {
    Model original;mixedPollutants(original);original.attach();SourceResultHistory history;ASSERT_EQ(history.append(original.stage),"");
    Model other;mixedPollutants(other);other.attach();ASSERT_EQ(history.resume(other.stage),"");EXPECT_EQ(history.tables().at("boundaries").rows(),1);
    advanceRestart(other,0,1);ASSERT_EQ(history.append(other.stage),"");EXPECT_EQ(history.tables().at("boundaries").rows(),2);
}
#endif

#ifdef OPENSWMM_HAS_HDF5_MODEL
TEST(CommonSourceStage, MarcherSourceHistoryMatchesCheckpointAfterCompletedAdvancePublication) {
    Model original;original.setup(false,2,false,true);original.surface.volume[1]=.01;
    original.surface.depth[1]=.0025;original.surface.head[1]=10.0025;original.attach(true);SourceResultHistory history;
    original.marcher.setSourceResultHistory(history);ASSERT_DOUBLE_EQ(original.marcher.advance(0,3.25),3.25);
    const auto file=restartPath("marcher-history-boundary");ASSERT_EQ(original.stage.saveCheckpoint(file.string()),"");
    Model restored;restored.setup(false,2,false,true);restored.surface.volume[1]=.01;
    restored.surface.depth[1]=.0025;restored.surface.head[1]=10.0025;restored.attach();ASSERT_EQ(restored.stage.loadCheckpoint(file.string()),"");
    ASSERT_EQ(history.resume(restored.stage),"");
}
#endif

namespace {
void marcherRestartModel(Model& m,Momentum2D mode=Momentum2D::LOCAL_INERTIAL,GwClosure closure=GwClosure::CLOSED_FORM) {
    AtmosphericInput a;a.et="ENSLAVED";a.pet={1e-5,2e-5};a.connected=true;a.inside={.7,.4};
    a.closure=closure;
    mixedPollutants(m,2,a,true);m.options.momentum=mode;m.options.max_timestep=.3;m.options.cfl_number=.5;
    m.options.theta=.8;m.options.front_rebuild=1;
    m.surface.volume[1]=.01;m.surface.depth[1]=.0025;m.surface.head[1]=10.0025;
    m.surface.evap_rate={1e-5,2e-5};m.room(.03,0);m.attach(true);
}
void marcherEqual(const Model& a,const Model& b,const std::string& name) {
    const auto x=restartPath("cpu-a-"+name),y=restartPath("cpu-b-"+name);
    ASSERT_EQ(a.marcher.saveCheckpoint(x.string()),"");ASSERT_EQ(b.marcher.saveCheckpoint(y.string()),"");
    EXPECT_EQ(restartBytes(x),restartBytes(y));
    EXPECT_EQ(a.surface.volume,b.surface.volume);EXPECT_EQ(a.surface.edge_flux,b.surface.edge_flux);
    EXPECT_EQ(a.gw.transport().sat_mass,b.gw.transport().sat_mass);
    EXPECT_DOUBLE_EQ(a.marcher.completedSourceTime(),b.marcher.completedSourceTime());
}
}
TEST(CommonSourceStage, CpuCheckpointContinuesMomentumScheduleAndPendingTuplesExactly) {
    for(const auto closure:{GwClosure::CLOSED_FORM,GwClosure::SIGMA})
    for(const auto mode:{Momentum2D::LOCAL_INERTIAL,Momentum2D::FULL_SWE,Momentum2D::DIFFUSIVE_WAVE}) {
        SCOPED_TRACE(std::to_string(int(closure))+":"+std::to_string(int(mode)));
        Model continuous;marcherRestartModel(continuous,mode,closure);
        ASSERT_DOUBLE_EQ(continuous.marcher.advance(0,1.25),1.25);
        ASSERT_DOUBLE_EQ(continuous.marcher.advance(1.25,3.25),3.25);
        if(closure==GwClosure::CLOSED_FORM)
            ASSERT_TRUE(std::any_of(continuous.surface.edge_flux.begin(),continuous.surface.edge_flux.end(),[](double q){return q!=0;}));
        const auto file=restartPath("cpu-mode-"+std::to_string(int(mode)));
        ASSERT_EQ(continuous.marcher.saveCheckpoint(file.string()),"");
        EXPECT_GT(*std::max_element(continuous.gw.state().tier.begin(),continuous.gw.state().tier.end()),0);
        Model resumed;marcherRestartModel(resumed,mode,closure);ASSERT_EQ(resumed.marcher.loadCheckpoint(file.string()),"");
        marcherEqual(continuous,resumed,"restored-"+std::to_string(int(mode)));
        for(const auto [start,end]:{std::pair{3.25,8.},std::pair{8.,9.125},std::pair{9.125,31.5},std::pair{31.5,36.}}) {
            ASSERT_DOUBLE_EQ(continuous.marcher.advance(start,end),end);ASSERT_DOUBLE_EQ(resumed.marcher.advance(start,end),end);
            marcherEqual(continuous,resumed,std::to_string(int(mode))+"-"+std::to_string(end));
            etBalance(resumed);
        }
        EXPECT_THROW(resumed.marcher.reinitialize(36),std::runtime_error);
        EXPECT_THROW(resumed.marcher.resyncFromVolumes(36),std::runtime_error);
    }
}
TEST(CommonSourceStage, CpuCheckpointInvalidClocksCannotAlterAcceptedState) {
    Model m;marcherRestartModel(m);ASSERT_DOUBLE_EQ(m.marcher.advance(0,1.25),1.25);
    const auto file=restartPath("cpu-clock");ASSERT_EQ(m.marcher.saveCheckpoint(file.string()),"");const auto before=restartBytes(file);
    for(const auto [start,end]:{std::pair{0.,2.},std::pair{1.5,2.},std::pair{1.25,1.},
         std::pair{1.25,std::numeric_limits<double>::quiet_NaN()},std::pair{std::numeric_limits<double>::quiet_NaN(),2.}}) {
        EXPECT_THROW(m.marcher.advance(start,end),std::invalid_argument);
        ASSERT_EQ(m.marcher.saveCheckpoint(file.string()),"");EXPECT_EQ(restartBytes(file),before);
    }
    EXPECT_DOUBLE_EQ(m.marcher.advance(1.25,1.25),1.25);
    ASSERT_EQ(m.marcher.saveCheckpoint(file.string()),"");EXPECT_EQ(restartBytes(file),before);
}
TEST(CommonSourceStage, CpuCheckpointCorruptionAndModelMismatchRestoreAtomically) {
    Model original;marcherRestartModel(original);ASSERT_DOUBLE_EQ(original.marcher.advance(0,1.25),1.25);
    const auto file=restartPath("cpu-corrupt-original");ASSERT_EQ(original.marcher.saveCheckpoint(file.string()),"");const auto saved=restartBytes(file);
    for(int change=0;change<5;++change) {
        Model target;marcherRestartModel(target);const auto before=restartPath("cpu-target-before");
        ASSERT_EQ(target.marcher.saveCheckpoint(before.string()),"");const auto initial=restartBytes(before);
        auto bytes=saved;
        if(change==0)bytes.resize(bytes.size()-1);
        if(change==1)bytes.back()^=1;
        if(change==2)bytes+="junk";
        if(change==3)bytes[8]^=1;
        if(change==4)target.options.max_timestep=.2;
        const auto bad=restartPath("cpu-corrupt-"+std::to_string(change));std::ofstream(bad,std::ios::binary).write(bytes.data(),bytes.size());
        EXPECT_FALSE(target.marcher.loadCheckpoint(bad.string()).empty());
        if(change==4)target.options.max_timestep=.3;
        ASSERT_EQ(target.marcher.saveCheckpoint(before.string()),"");EXPECT_EQ(restartBytes(before),initial);
        ASSERT_EQ(target.marcher.loadCheckpoint(file.string()),"");
        EXPECT_FALSE(target.marcher.loadCheckpoint(file.string()).empty());
        marcherEqual(original,target,"atomic-"+std::to_string(change));
    }
}
TEST(CommonSourceStage, CpuCheckpointRefusesUnsupportedOrFailedBoundariesAndProtectsFiles) {
    Model bare;bare.setup();bare.marcher.initialize(bare.mesh,bare.surface,bare.options);
    const auto file=restartPath("cpu-profile");EXPECT_FALSE(bare.marcher.saveCheckpoint(file.string()).empty());
    Model m;marcherRestartModel(m);ASSERT_EQ(m.marcher.saveCheckpoint(file.string()),"");const auto original=restartBytes(file);
    NodeData nodes;m.surface.nodes_1d=&nodes;EXPECT_FALSE(m.marcher.saveCheckpoint(file.string()).empty());
    EXPECT_EQ(restartBytes(file),original);m.surface.nodes_1d=nullptr;
    const auto unrelated=restartPath("cpu-unrelated");std::ofstream(unrelated)<<"user model input";
    EXPECT_FALSE(m.marcher.saveCheckpoint(unrelated.string()).empty());EXPECT_EQ(restartBytes(unrelated),"user model input");
    runoff::SourceWaterDriver::BottomCeiling ceiling=[](int,int,double,double){return 0.;};
    runoff::RunoffSolver::InfiltrationBoundary boundary=[](int,double,double,double& rate){rate=0;return true;};
    ASSERT_EQ(m.sources.stage(0,1,1,&boundary,&ceiling),"");
    EXPECT_THROW(m.marcher.advance(0,1),std::runtime_error);m.sources.cancel();
    EXPECT_FALSE(m.marcher.saveCheckpoint(file.string()).empty());EXPECT_EQ(restartBytes(file),original);
}
#ifdef OPENSWMM_HAS_HDF5_MODEL
TEST(CommonSourceStage, CpuCheckpointResumesSavedHistoryAtMatchingBoundaryWithoutReplay) {
    Model continuous;marcherRestartModel(continuous);SourceResultHistory history;continuous.marcher.setSourceResultHistory(history);
    ASSERT_DOUBLE_EQ(continuous.marcher.advance(0,3.25),3.25);
    const auto file=restartPath("cpu-history"),result=sourceResultPath("cpu-history");
    ASSERT_EQ(continuous.marcher.saveCheckpoint(file.string()),"");ASSERT_EQ(history.save(result.string()),"");
    Model resumed;marcherRestartModel(resumed);ASSERT_EQ(resumed.marcher.loadCheckpoint(file.string()),"");
    SourceResultHistory prefix;bool available=false;ASSERT_EQ(SourceResultHistory::read(result.string(),prefix,available),"");ASSERT_TRUE(available);
    resumed.marcher.resumeSourceResultHistory(prefix);resultEqual(history,prefix);
    EXPECT_THROW(resumed.marcher.resumeSourceResultHistory(prefix),std::invalid_argument);
    ASSERT_DOUBLE_EQ(continuous.marcher.advance(3.25,6.125),6.125);ASSERT_DOUBLE_EQ(resumed.marcher.advance(3.25,6.125),6.125);
    marcherEqual(continuous,resumed,"history-final");resultEqual(history,prefix);
}
#endif

TEST(CommonSourceStage, CpuCheckpointValidChecksumCannotHideBadSchedulesOrClocks) {
    Model original;marcherRestartModel(original);ASSERT_DOUBLE_EQ(original.marcher.advance(0,1.25),1.25);
    const auto file=restartPath("cpu-semantic");ASSERT_EQ(original.marcher.saveCheckpoint(file.string()),"");
    for(int change=0;change<7;++change) {
        detail::CompletedSourceArchive reader(restartBytes(file));std::string magic,payload;
        uint32_t version=0,endian=0,longBytes=0,crc=0;reader(magic,version,endian,longBytes,payload,crc);reader.finish();
        const auto count=[&](size_t at){uint64_t n=0;std::memcpy(&n,payload.data()+at,sizeof(n));return n;};
        size_t offset=sizeof(uint64_t)+count(0);std::array<size_t,20> fields{};
        for(int field=0;field<20;++field) {
            fields[field]=offset;const auto n=count(offset);offset+=sizeof(uint64_t);
            if(field<18)offset+=n*((field>=12 && field<=16)?sizeof(uint8_t):sizeof(double));
            else for(uint64_t k=0;k<n;++k){const auto size=count(offset);offset+=sizeof(uint64_t)+size*sizeof(int);}
        }
        const auto cells=offset;offset+=sizeof(uint64_t)+count(offset)*sizeof(int);
        offset+=sizeof(uint64_t)+count(offset)*sizeof(int); // active faces; dt0 then completed clocks
        if(change==0){double nan=std::numeric_limits<double>::quiet_NaN();std::memcpy(payload.data()+fields[0]+8,&nan,sizeof(nan));}
        if(change==1)payload[fields[13]+8]=char(255); // cell tier outside ladder
        if(change==2){int bad=999;std::memcpy(payload.data()+cells+8,&bad,sizeof(bad));}
        if(change==3){double future=2;std::memcpy(payload.data()+offset+8,&future,sizeof(future));std::memcpy(payload.data()+offset+16,&future,sizeof(future));}
        if(change==4){uint64_t oversized=UINT64_MAX;std::memcpy(payload.data()+fields[0],&oversized,sizeof(oversized));}
        if(change==5){double pending=.1;std::memcpy(payload.data()+fields[3]+8,&pending,sizeof(pending));}
        if(change==6)version=999;
        crc=restartCrc(payload);detail::CompletedSourceArchive writer;writer(magic,version,endian,longBytes,payload,crc);
        const auto bad=restartPath("cpu-semantic-"+std::to_string(change));std::ofstream(bad,std::ios::binary).write(writer.bytes().data(),writer.bytes().size());
        Model target;marcherRestartModel(target);const auto before=restartPath("cpu-semantic-before");ASSERT_EQ(target.marcher.saveCheckpoint(before.string()),"");
        const auto initial=restartBytes(before);EXPECT_FALSE(target.marcher.loadCheckpoint(bad.string()).empty());
        ASSERT_EQ(target.marcher.saveCheckpoint(before.string()),"");EXPECT_EQ(restartBytes(before),initial);
        ASSERT_EQ(target.marcher.loadCheckpoint(file.string()),"");marcherEqual(original,target,"semantic-"+std::to_string(change));
    }
}
TEST(CommonSourceStage, CpuInitialCheckpointPreservesFreshScheduleAndStartsOnce) {
    Model original;marcherRestartModel(original);const auto file=restartPath("cpu-initial");ASSERT_EQ(original.marcher.saveCheckpoint(file.string()),"");
    Model target;marcherRestartModel(target);ASSERT_EQ(target.marcher.loadCheckpoint(file.string()),"");marcherEqual(original,target,"initial");
    ASSERT_DOUBLE_EQ(original.marcher.advance(0,1.25),1.25);ASSERT_DOUBLE_EQ(target.marcher.advance(0,1.25),1.25);marcherEqual(original,target,"initial-continued");
}

namespace {
struct RouterSourceModel {
    Model source;
    SurfaceRouter2D router;
    void setup(Momentum2D momentum=Momentum2D::LOCAL_INERTIAL,GwClosure closure=GwClosure::CLOSED_FORM,bool quality=false,double report=2.3) {
        AtmosphericInput a;a.inside={.75,.25};a.pet={.0001,.0002};a.et="BOUNDARY_ET";a.closure=closure;
        if(quality) mixedPollutants(source,2,a,true);else source.setup(true,2,false,true,false,a);
        auto& ctx=source.context;ctx.options.num_threads=1;ctx.options.routing_step=.7;ctx.options.wet_step=1.4;ctx.options.report_step=report;
        ctx.current_date=ctx.options.start_date;ctx.next_report_ms=1000*report;ctx.gages.rainfall[0]=432;
        router.mesh()=source.mesh;router.mesh().tri_init_depth={.02,.003};
        router.options()=source.options;router.options().mesh_units_si=true;router.options().momentum=momentum;
        router.options().max_timestep=.21;router.options().coupling_sync=3.5;router.options().evaporation=2;
        router.options().rainfall_mode=RainfallMode::SYSTEM;ctx.climate_state.evap_rate=.0001;
        router.aquiferConfig()=source.config;router.aquiferConfig().node_beds.clear();
        auto& row=router.aquiferConfig().rows[0];row.Ks=a.ks*3600/.0254;row.zs=2/.3048;row.hg0=1/.3048;row.alpha=2*.3048;
        ctx.twod_io.mesh=&router.mesh();ctx.twod_io.options=&router.options();
        ctx.twod_io.gw=&router.gwTransport();ctx.twod_io.aquifer_state=&router.subsurface().state();
        ASSERT_NO_THROW(router.initialize(ctx));
        if(quality) router.state().transport.cell_mass.assign(4,19);
        auto& gw=router.subsurface().state();
        for(int i=0;i<2;++i)gw.hu[i]=gw.theta_s[i]*(gw.zs[i]-gw.hg[i])-.0001/router.mesh().tri_area[i];
        ASSERT_EQ(source.stage.initialize(router.mesh(),router.state(),router.options(),router.subsurface(),source.sources,source.preview),"");
        ASSERT_NO_THROW(router.setCommonSourceStage(ctx,source.stage));
        router.bookGageRates(ctx,0);
    }
    void tick(double dt=.7) {
        auto& ctx=source.context;
        try {router.step(ctx,dt,ctx.current_time);}
        catch(const std::runtime_error& e) {std::ostringstream message;message<<std::setprecision(17)<<e.what()<<" boundary="<<source.stage.completedEnd()<<" infil="<<router.subsurface().state().infil_refresh[0]<<" et="<<router.subsurface().state().et_refresh[0];throw std::runtime_error(message.str());}
        hydraulics::TimestepController::advance(ctx,dt);
    }
    void weather(double rate) {
        auto& ctx=source.context;ctx.gages.rainfall[0]=rate;router.bookGageRates(ctx,ctx.current_time);
    }
};
void routerEqual(RouterSourceModel& a,RouterSourceModel& b,const std::string& label) {
    const auto pa=restartPath(label+"-router-continuous"),pb=restartPath(label+"-router-resumed");
    ASSERT_EQ(a.router.saveCheckpoint(a.source.context,pa.string()),"");
    ASSERT_EQ(b.router.saveCheckpoint(b.source.context,pb.string()),"");
    EXPECT_EQ(restartBytes(pa),restartBytes(pb));
    EXPECT_NEAR(a.source.context.mass_balance_2d.error(),0,1e-12);
    EXPECT_NEAR(b.source.context.mass_balance_2d.error(),0,1e-12);
}
}
TEST(CommonSourceStage, RouterCheckpointPreservesPartialBatchWeatherReportFramesAndLedgers) {
    for(auto momentum:{Momentum2D::LOCAL_INERTIAL,Momentum2D::DIFFUSIVE_WAVE,Momentum2D::FULL_SWE})
    for(auto closure:{GwClosure::CLOSED_FORM,GwClosure::SIGMA}) {
        RouterSourceModel continuous;continuous.setup(momentum,closure,true);
        for(int i=0;i<6;++i) {if(i==2)continuous.weather(216);
            ASSERT_NO_THROW(continuous.tick());}
        EXPECT_LT(continuous.source.stage.completedEnd(),continuous.source.context.current_time);
        EXPECT_GT(continuous.router.reportWeight(2300),0);EXPECT_LT(continuous.router.reportWeight(2300),1);
        EXPECT_DOUBLE_EQ(continuous.source.context.mass_balance_2d.rainfall_in,continuous.source.stage.meshRain());
        EXPECT_DOUBLE_EQ(continuous.source.context.mass_balance_2d.aquifer_in,continuous.source.stage.meshReturn());
        for(double v:continuous.router.couplingCumulative())EXPECT_DOUBLE_EQ(v,0);
        const auto file=restartPath("router-batch-"+std::to_string(int(momentum))+"-"+std::to_string(int(closure)));
        ASSERT_EQ(continuous.router.saveCheckpoint(continuous.source.context,file.string()),"");
        RouterSourceModel resumed;resumed.setup(momentum,closure,true);
        ASSERT_EQ(resumed.router.loadCheckpoint(resumed.source.context,file.string()),"");
        routerEqual(continuous,resumed,"router-initial");
        EXPECT_EQ(continuous.router.reportOldDepth(),resumed.router.reportOldDepth());
        EXPECT_EQ(continuous.router.reportOldHead(),resumed.router.reportOldHead());
        EXPECT_EQ(continuous.router.reportRainCumulative(2300),resumed.router.reportRainCumulative(2300));
        for(int i=6;i<15;++i) {
            if(i==8) {continuous.weather(0);resumed.weather(0);}
            if(i==11) {continuous.source.context.next_report_ms=9200;resumed.source.context.next_report_ms=9200;}
            ASSERT_NO_THROW(continuous.tick());
            ASSERT_NO_THROW(resumed.tick());
        }
        continuous.router.flushPendingBatch(continuous.source.context);resumed.router.flushPendingBatch(resumed.source.context);
        routerEqual(continuous,resumed,"router-final");
        EXPECT_FALSE(resumed.router.loadCheckpoint(resumed.source.context,file.string()).empty());
    }
}
TEST(CommonSourceStage, RouterCheckpointRejectsCorruptionModelChangesAndUnsupportedProfilesWithoutMutation) {
    RouterSourceModel continuous;continuous.setup();for(int i=0;i<6;++i)continuous.tick();
    const auto file=restartPath("router-corrupt");ASSERT_EQ(continuous.router.saveCheckpoint(continuous.source.context,file.string()),"");
    const auto original=restartBytes(file);
    for(int variant=0;variant<5;++variant) {
        auto bytes=original;if(variant==0)bytes.resize(bytes.size()/2);if(variant==1)bytes.back()^=1;if(variant==2)bytes+='x';
        const auto bad=restartPath("router-corrupt-"+std::to_string(variant));std::ofstream(bad,std::ios::binary).write(bytes.data(),bytes.size());
        RouterSourceModel fresh;fresh.setup(Momentum2D::LOCAL_INERTIAL,GwClosure::CLOSED_FORM,false,variant==3?3:2.3);
        if(variant==4)fresh.router.options().max_timestep=.3;
        const auto before=restartPath("router-before-"+std::to_string(variant)),after=restartPath("router-after-"+std::to_string(variant));
        if(variant<4)ASSERT_EQ(fresh.router.saveCheckpoint(fresh.source.context,before.string()),"");
        const auto volume=fresh.router.state().volume;const auto time=fresh.source.context.current_time;
        EXPECT_FALSE(fresh.router.loadCheckpoint(fresh.source.context,bad.string()).empty());
        EXPECT_EQ(fresh.router.state().volume,volume);EXPECT_DOUBLE_EQ(fresh.source.context.current_time,time);
        if(variant<4) {ASSERT_EQ(fresh.router.saveCheckpoint(fresh.source.context,after.string()),"");EXPECT_EQ(restartBytes(before),restartBytes(after));}
    }
    const auto unrelated=restartPath("router-unrelated");std::ofstream(unrelated)<<"user result file";
    EXPECT_FALSE(continuous.router.saveCheckpoint(continuous.source.context,unrelated.string()).empty());EXPECT_EQ(restartBytes(unrelated),"user result file");
    const auto before=continuous.router.state().volume;
    for(auto pair:{std::pair{0.,4.2},std::pair{.7,0.},std::pair{.7,std::numeric_limits<double>::infinity()}})
        EXPECT_THROW(continuous.router.advancePostRouting(continuous.source.context,pair.first,pair.second),std::invalid_argument);
    EXPECT_EQ(continuous.router.state().volume,before);
    EXPECT_EQ(continuous.router.saveCheckpoint(continuous.source.context,file.string()),"");EXPECT_EQ(restartBytes(file),original);
    continuous.router.state().coupling_forced[0]=1;
    EXPECT_FALSE(continuous.router.saveCheckpoint(continuous.source.context,file.string()).empty());EXPECT_EQ(restartBytes(file),original);
}
TEST(CommonSourceStage, RouterCheckpointSupportsInitialAndExplicitTailFlushAndOnDemandRender) {
    RouterSourceModel initial;initial.setup();const auto file=restartPath("router-zero");
    ASSERT_EQ(initial.router.saveCheckpoint(initial.source.context,file.string()),"");
    RouterSourceModel fresh;fresh.setup();ASSERT_EQ(fresh.router.loadCheckpoint(fresh.source.context,file.string()),"");routerEqual(initial,fresh,"router-zero");
    for(int i=0;i<7;++i) {initial.tick(.1);fresh.tick(.1);}
    initial.router.flushPendingBatch(initial.source.context);fresh.router.flushPendingBatch(fresh.source.context);
    initial.router.refreshRenderFieldsIfStale();fresh.router.refreshRenderFieldsIfStale();routerEqual(initial,fresh,"router-render");
    initial.router.finalize(initial.source.context);fresh.router.finalize(fresh.source.context);
    EXPECT_NO_THROW(initial.router.refreshRenderFieldsIfStale());
    EXPECT_FALSE(initial.router.saveCheckpoint(initial.source.context,file.string()).empty());
}
#ifdef OPENSWMM_HAS_HDF5_MODEL
TEST(CommonSourceStage, RouterCheckpointResumesSavedSourceHistoryAfterPostprocessingAndRenderWithoutReplay) {
    RouterSourceModel continuous;continuous.setup(Momentum2D::LOCAL_INERTIAL,GwClosure::CLOSED_FORM,true,.8);
    SourceResultHistory history;continuous.router.setSourceResultHistory(history);
    for(int i=0;i<6;++i)continuous.tick();continuous.router.refreshRenderFieldsIfStale();
    const auto file=restartPath("router-history"),result=sourceResultPath("router-history");
    ASSERT_EQ(continuous.router.saveCheckpoint(continuous.source.context,file.string()),"");ASSERT_EQ(history.save(result.string()),"");
    RouterSourceModel resumed;resumed.setup(Momentum2D::LOCAL_INERTIAL,GwClosure::CLOSED_FORM,true,.8);
    ASSERT_EQ(resumed.router.loadCheckpoint(resumed.source.context,file.string()),"");
    SourceResultHistory prefix;bool available=false;ASSERT_EQ(SourceResultHistory::read(result.string(),prefix,available),"");ASSERT_TRUE(available);
    ASSERT_NO_THROW(resumed.router.resumeSourceResultHistory(prefix));resultEqual(history,prefix);
    for(int i=6;i<11;++i) {continuous.tick();resumed.tick();}
    continuous.router.flushPendingBatch(continuous.source.context);resumed.router.flushPendingBatch(resumed.source.context);
    routerEqual(continuous,resumed,"router-history");resultEqual(history,prefix);
}
#endif

TEST(CommonSourceStage, RouterLedgerUsesActualWeatherFootprintAndReturnsWithoutInventingNodeSpill) {
    RouterSourceModel m;m.setup(Momentum2D::LOCAL_INERTIAL,GwClosure::CLOSED_FORM,true);ASSERT_FALSE(HasFatalFailure());
    for(int i=0;i<5;++i) {if(i==2)m.weather(216);m.tick();}
    const double rain=7*.0254/3600*(432*1.4+216*2.1);
    EXPECT_NEAR(m.source.context.mass_balance_2d.rainfall_in,rain,1e-14);
    double per_cell=0;for(double v:m.router.rainCumulative())per_cell+=v;EXPECT_NEAR(per_cell,rain,1e-14);
    const double report=7*.0254/3600*(432*1.4+216*.9);
    double per_report=0;for(double v:m.router.reportRainCumulative(2300))per_report+=v;EXPECT_NEAR(per_report,report,1e-14);
    // Controlled groundwater rise exercises real Dunne return ownership.
    m.router.subsurface().bookLinkSeepage(0,10);
    for(int i=0;i<50;++i)m.tick();m.router.flushPendingBatch(m.source.context);
    EXPECT_GT(m.source.stage.meshReturn(),0);EXPECT_DOUBLE_EQ(m.source.context.mass_balance_2d.aquifer_in,m.source.stage.meshReturn());
    for(double v:m.router.couplingCumulative())EXPECT_DOUBLE_EQ(v,0);
    EXPECT_NEAR(m.source.context.mass_balance_2d.error(),0,1e-12);
}
TEST(CommonSourceStage, RouterCheckpointValidChecksumStillRejectsSemanticClocksShapesAndCrossComponentLedgers) {
    RouterSourceModel original;original.setup();for(int i=0;i<6;++i)original.tick();
    const auto file=restartPath("router-semantic");ASSERT_EQ(original.router.saveCheckpoint(original.source.context,file.string()),"");
    for(int change=0;change<9;++change) {
        detail::CompletedSourceArchive reader(restartBytes(file));std::string magic,model,payload;
        uint32_t version=0,endian=0,longBytes=0,crc=0;reader(magic,version,endian,longBytes,model,payload,crc);reader.finish();
        const auto count=[&](size_t at){uint64_t n=0;std::memcpy(&n,payload.data()+at,sizeof(n));return n;};
        const std::array<int,28> types{8,0,0,0,8,8,1,8,1,8,8,0,0,8,8,1,8,1,1,0,0,0,0,0,8,0,0,8};
        std::array<size_t,28> fields{};size_t offset=0;
        for(int i=0;i<28;++i) {fields[i]=offset;offset+=types[i]?types[i]:8+count(offset)*sizeof(double);}
        const auto clocks=offset;offset+=6*sizeof(double);offset+=8+count(offset)*sizeof(double);const auto ledger=offset;
        const auto put=[&](size_t at,double value){std::memcpy(payload.data()+at,&value,sizeof(value));};
        if(change==0)put(fields[0],std::numeric_limits<double>::quiet_NaN());
        if(change==1)put(fields[10],-1);
        if(change==2)payload[fields[6]]=char(2);
        if(change==3){uint64_t huge=UINT64_MAX;std::memcpy(payload.data()+fields[3],&huge,sizeof(huge));}
        if(change==4)put(fields[13],9000);
        if(change==5)put(clocks+2*sizeof(double),-1);
        if(change==6) { // Router clocks close internally but disagree with the embedded CPU/source clock.
            put(fields[9],original.source.stage.completedEnd()+.5);
            put(fields[10],original.source.context.current_time-original.source.stage.completedEnd()-.5);
            put(fields[14],1000*(original.source.stage.completedEnd()+.5));
        }
        if(change==7)put(ledger+sizeof(double),original.source.context.mass_balance_2d.final_storage+.001);
        if(change==8) { // Router rainfall totals agree with each other, but not with source receipts.
            put(fields[3]+8,original.router.rainCumulative()[0]+.001);
            put(ledger+4*sizeof(double),original.source.context.mass_balance_2d.rainfall_in+.001);
        }
        crc=restartCrc(payload);detail::CompletedSourceArchive writer;writer(magic,version,endian,longBytes,model,payload,crc);
        const auto bad=restartPath("router-semantic-"+std::to_string(change));std::ofstream(bad,std::ios::binary).write(writer.bytes().data(),writer.bytes().size());
        RouterSourceModel target;target.setup();const auto before=restartPath("router-semantic-before");
        ASSERT_EQ(target.router.saveCheckpoint(target.source.context,before.string()),"");const auto initial=restartBytes(before);
        EXPECT_FALSE(target.router.loadCheckpoint(target.source.context,bad.string()).empty());
        ASSERT_EQ(target.router.saveCheckpoint(target.source.context,before.string()),"");EXPECT_EQ(restartBytes(before),initial);
        ASSERT_EQ(target.router.loadCheckpoint(target.source.context,file.string()),"");routerEqual(original,target,"router-semantic-retry");
    }
}
TEST(CommonSourceStage, RouterFailedSourceTrialCannotPublishOrContinuePartialRouterState) {
    RouterSourceModel m;m.setup();const auto file=restartPath("router-failed");ASSERT_EQ(m.router.saveCheckpoint(m.source.context,file.string()),"");
    const auto initial=restartBytes(file);
    runoff::SourceWaterDriver::BottomCeiling ceiling=[](int,int,double,double){return 0.;};
    runoff::RunoffSolver::InfiltrationBoundary boundary=[](int,double,double,double& rate){rate=0;return true;};
    ASSERT_EQ(m.source.sources.stage(0,1,1,&boundary,&ceiling),"");
    EXPECT_NO_THROW(m.router.flushPendingBatch(m.source.context));
    for(int i=0;i<4;++i)m.tick();
    EXPECT_THROW(m.tick(),std::runtime_error);m.source.sources.cancel();
    EXPECT_FALSE(m.router.saveCheckpoint(m.source.context,file.string()).empty());EXPECT_EQ(restartBytes(file),initial);
    EXPECT_THROW(m.tick(),std::invalid_argument);
}
