/** @file test_junction_storage.cpp
 * Physical junction chamber storage: openswmm.engine#10 / USEPA#200.
 * Copyright (c) 2026 Caleb Buahin. Apache-2.0.
 */
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>
#include "core/SWMMEngine.hpp"
#include "hydraulics/Node.hpp"
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/legacy/engine/openswmm_solver.h>

namespace {
const std::filesystem::path work = ISSUE10_OUTPUT_DIR;
std::string deck(double area, double rim = 10, bool pond = false, bool si = false,
                 const char* routing = "DYNWAVE", bool connected = false) {
    std::ostringstream s;
    s << "[OPTIONS]\nFLOW_UNITS " << (si ? "LPS" : "CFS") << "\nFLOW_ROUTING " << routing
      << "\nSTART_DATE 01/01/2026\nEND_DATE 01/01/2026\nEND_TIME 00:10:00\n"
      << "REPORT_STEP 00:00:10\nROUTING_STEP 1\nVARIABLE_STEP 0\nMAX_TRIALS 30\n"
      << "HEAD_TOLERANCE 0.0000001\nMIN_SURFAREA 100\nALLOW_PONDING " << (pond ? "YES" : "NO")
      << "\n[JUNCTIONS]\nJ1 0 " << rim << " 0 0 " << (pond ? 200 : 0) << ' ' << area << '\n';
    if (connected) s << "J2 0 10 4 0 0 " << area << '\n';
    s << "[OUTFALLS]\nO1 0 FREE NO\n";
    if (connected)
        s << "[CONDUITS]\nC1 J1 J2 100 0.013 0 0 0\n[XSECTIONS]\nC1 CIRCULAR 1 0 0 0 1\n";
    s << "[INFLOWS]\nJ1 FLOW \"\" FLOW 1 1 " << (area < 1 ? 0.001 : 1)
      << "\n[REPORT]\nNODES ALL\nLINKS ALL\n";
    return s.str();
}
std::string file(const std::string& name, const std::string& text) {
    std::filesystem::create_directories(work);
    const auto p = (work / (name + ".inp")).string();
    std::ofstream(p) << text;
    return p;
}
struct Totals { double volume, depth, error, inflow, flood, initial, outflow, final; };
Totals run(const std::string& path, bool legacy) {
    double t = 0;
    Totals result{};
    if (legacy) {
        EXPECT_EQ(swmm_open(path.c_str(), (path + ".legacy.rpt").c_str(), ""), 0);
        EXPECT_EQ(swmm_start(0), 0);
        do { EXPECT_EQ(swmm_step(&t), 0); } while (t > 0);
        result.volume = swmm_getValue(swmm_NODE_VOLUME, 0);
        result.depth = swmm_getValue(swmm_NODE_DEPTH, 0);
        EXPECT_EQ(swmm_end(), 0);
        swmm_RoutingTotals a{};
        EXPECT_EQ(swmm_getSystemRoutingTotals(&a), 0);
        result.error=a.pctError; result.inflow=a.exInflow; result.flood=a.flooding;
        result.initial=a.initStorage; result.outflow=a.outflow; result.final=a.finalStorage;
        EXPECT_EQ(swmm_close(), 0);
    } else {
        openswmm::SWMMEngine e;
        EXPECT_EQ(e.open(path.c_str(), (path + ".v6.rpt").c_str(), ""), 0);
        EXPECT_EQ(e.initialize(), 0); EXPECT_EQ(e.start(0), 0);
        do { EXPECT_EQ(e.step(&t), 0); } while (t > 0);
        result.volume=e.context().nodes.volume[0]; result.depth=e.context().nodes.depth[0];
        EXPECT_EQ(e.end(), 0); EXPECT_EQ(e.report(), 0);
        const auto& b=e.context().mass_balance;
        result.error=100*b.routing_error(); result.inflow=b.routing_external;
        result.flood=b.routing_flooding; result.initial=b.routing_init_storage; result.outflow=b.routing_outflow; result.final=b.routing_final_storage;
    }
    return result;
}
TEST(JunctionStorage, AnalyticGeometryIsIndependentOfNumericalFloor) {
    openswmm::SimulationContext ctx;
    ctx.nodes.resize(1); auto& n=ctx.nodes;
    n.storage_area[0]=0.125; n.full_depth[0]=10;
    for (double d : {0., 0.1, 1., 5., 10., 12.}) {
        EXPECT_DOUBLE_EQ(openswmm::node::getVolume(n,0,d), .125*std::min(d,10.));
        EXPECT_DOUBLE_EQ(openswmm::node::getSurfArea(n,0,d), d > 10 ? 0.0 : .125);
        EXPECT_DOUBLE_EQ(openswmm::node::getDepth(n,0,.125*d), std::min(d,10.));
    }
    n.grow_to(3); n.erase_at(1); EXPECT_DOUBLE_EQ(n.storage_area[0],.125);
    EXPECT_EQ(n.storage_area.size(),2u);
}
TEST(JunctionStorage, IsolatedFillFloodAndPondConserveWaterInBothEngines) {
    for (bool pond : {false,true}) for (double rim : {1.,10.}) {
        const auto path=file("chamber-"+std::to_string(rim)+(pond?"-pond":"-flood"),deck(100,rim,pond));
        for (bool legacy : {false,true}) {
            const auto t=run(path,legacy);
            EXPECT_LT(std::abs(t.error),.1);
            EXPECT_NEAR(t.volume + t.flood + t.outflow, t.inflow + t.initial, .001*(t.inflow+t.initial));
            EXPECT_NEAR(t.depth, t.volume > 100*rim ? rim+(t.volume-100*rim)/200 : t.volume/100,1e-6);
        }
    }
    const auto path=file("small-area",deck(.125));
    for (bool legacy : {false,true}) {
        const auto t=run(path,legacy);
        EXPECT_LT(std::abs(t.error),.1);
        EXPECT_NEAR(t.volume,.125*t.depth,1e-9);
        EXPECT_GT(t.depth,4.); // A=100 numerical floor would prevent this rise
    }
}
TEST(JunctionStorage, ConnectedSurchargeRetainsPhysicalVolume) {
    for (bool pond : {false,true}) for (double rim : {1.,10.}) {
        SCOPED_TRACE(std::to_string(rim)+(pond ? " pond" : " flood"));
        auto input=deck(100,rim,pond,false,"DYNWAVE",true);
        // A pressure allowance changes head limits, never chamber capacity.
        if (!pond && rim == 1) {
            const auto pos=input.find("J1 0 1 0 0 0 100");
            input.replace(pos, std::string("J1 0 1 0 0 0 100").size(), "J1 0 1 0 2 0 100");
        }
        const auto path=file("connected-"+std::to_string(rim)+(pond?"-pond":"-flood"),input);
        const auto a=run(path,true), b=run(path,false);
        for (const auto& t : {a,b}) {
            EXPECT_GT(t.depth,1.); // above the pipe crown
            const double expected = t.depth > rim && pond ? 100*rim+200*(t.depth-rim) : 100*std::min(t.depth,rim);
            EXPECT_NEAR(t.volume,expected,1e-6);
            EXPECT_LT(std::abs(t.error),.1);
            EXPECT_NEAR(t.final+t.flood+t.outflow,t.inflow+t.initial,.001*(t.inflow+t.initial));
        }
        EXPECT_NEAR(a.depth,b.depth,1e-5);
    }
}
TEST(JunctionStorage, PublicSetterAndInputRoundTripUseProjectAreaUnits) {
    for (bool si : {false,true}) {
        const auto path=file(si?"setter-si":"setter-us",deck(0,10,false,si));
        SWMM_Engine e=swmm_engine_create(); ASSERT_NE(e,nullptr);
        ASSERT_EQ(swmm_engine_open(e,path.c_str(),nullptr,nullptr,nullptr),0);
        ASSERT_EQ(swmm_node_set_storage_area(e,0,.125),0);
        double area=0; ASSERT_EQ(swmm_node_get_storage_area(e,0,&area),0); EXPECT_DOUBLE_EQ(area,.125);
        EXPECT_NE(swmm_node_set_storage_area(e,0,-1),0);
        EXPECT_NE(swmm_node_set_storage_area(e,0,std::numeric_limits<double>::infinity()),0);
        EXPECT_NE(swmm_node_set_storage_area(e,1,1),0); // outfall
        EXPECT_NE(swmm_model_write_compat(e,(path+".stock.inp").c_str(),SWMM_INP_PROFILE_SWMM5_STOCK),0);
        const auto saved=path+".saved.inp";
        ASSERT_EQ(swmm_model_write(e,saved.c_str()),0);
        ASSERT_EQ(swmm_engine_initialize(e),0);
        EXPECT_NE(swmm_node_set_storage_area(e,0,.25),0); // geometry is locked
        swmm_engine_destroy(e);
        openswmm::SWMMEngine loaded; ASSERT_EQ(loaded.open(saved.c_str(),nullptr,nullptr),0);
        const double factor=si?.3048:1;
        EXPECT_NEAR(loaded.context().nodes.storage_area[0]*factor*factor,.125,1e-15);
        ASSERT_EQ(swmm_open(saved.c_str(),(path+".legacy-api.rpt").c_str(),""),0);
        EXPECT_DOUBLE_EQ(swmm_getValue(swmm_NODE_STORAGE_AREA,0),.125);
        EXPECT_EQ(swmm_setValue(swmm_NODE_STORAGE_AREA,0,.25),0);
        EXPECT_DOUBLE_EQ(swmm_getValue(swmm_NODE_STORAGE_AREA,0),.25);
        EXPECT_EQ(swmm_close(),0);
    }
}
TEST(JunctionStorage, NonConservativeRoutingModesRefusePositiveArea) {
    for (const char* mode : {"KINWAVE","STEADY"}) {
        const auto path=file(mode,deck(100,10,false,false,mode));
        openswmm::SWMMEngine e; ASSERT_EQ(e.open(path.c_str(),nullptr,nullptr),0);
        EXPECT_NE(e.initialize(),0);
        EXPECT_NE(swmm_open(path.c_str(),(path+".legacy.rpt").c_str(),""),0);
        swmm_close();
    }
}
TEST(JunctionStorage, AlternateDynamicWaveClosuresConservePhysicalInventory) {
    for (const char* surcharge : {"EXTRAN","SLOT","TPA"}) {
        for (bool semi : {false,true}) {
            auto text=deck(100,10,false,false,"DYNWAVE",true);
            text += std::string("[OPTIONS]\nSURCHARGE_METHOD ")+surcharge+"\n";
            if (semi) text += "NODE_CONTINUITY SEMI_IMPLICIT\n";
            const auto path=file(std::string("dw-")+surcharge+(semi?"-semi":"-explicit"),text);
            SCOPED_TRACE(path);
            const auto t=run(path,false);
            EXPECT_LT(std::abs(t.error),.1);
            EXPECT_NEAR(t.volume,100*t.depth,1e-6);
            if (!semi && std::string(surcharge)!="TPA") {
                const auto legacy=run(path,true);
                EXPECT_LT(std::abs(legacy.error),.1);
                EXPECT_NEAR(legacy.depth,t.depth,1e-4);
            }
        }
    }
}
TEST(JunctionStorage, UnqualifiedDynamicSlotRefusesPhysicalArea) {
    auto text=deck(100,10,false,false,"DYNWAVE",true);
    text += "[OPTIONS]\nSURCHARGE_METHOD DYNAMIC_SLOT\n";
    const auto path=file("unsupported-dynamic-slot",text);
    openswmm::SWMMEngine e; ASSERT_EQ(e.open(path.c_str(),nullptr,nullptr),0);
    EXPECT_NE(e.initialize(),0);
}
TEST(JunctionStorage, SeepageCannotSpendNumericalChamberWater) {
    auto text=deck(100,10,false,false,"DYNWAVE",true);
    text += "[LOSSES]\nC1 0 0 0 NO 100\n";
    const auto path=file("chamber-seepage",text);
    for (bool legacy : {false,true}) {
        const auto t=run(path,legacy);
        EXPECT_LT(std::abs(t.error),.1);
        EXPECT_NEAR(t.volume,100*t.depth,1e-6);
    }
}
TEST(JunctionStorage, RetainedWaterDilutesTheIncomingTracer) {
    auto text=deck(100);
    const auto pos=text.find("J1 0 10 0 0 0 100");
    text.replace(pos,std::string("J1 0 10 0 0 0 100").size(),"J1 0 10 2 0 0 100");
    text += "[POLLUTANTS]\nP1 MG/L 0 0 0 0 NO * 0 0 0\n[INFLOWS]\nJ1 P1 \"\" CONCEN 1 1 10\n";
    const auto path=file("chamber-quality",text);
    double t=0;
    openswmm::SWMMEngine e; ASSERT_EQ(e.open(path.c_str(),nullptr,nullptr),0);
    ASSERT_EQ(e.initialize(),0); ASSERT_EQ(e.start(0),0);
    do { ASSERT_EQ(e.step(&t),0); } while(t>0);
    EXPECT_NEAR(e.context().nodes.conc[0],7.5,.01);
    ASSERT_EQ(swmm_open(path.c_str(),(path+".legacy.rpt").c_str(),""),0);
    ASSERT_EQ(swmm_start(0),0);
    do { ASSERT_EQ(swmm_step(&t),0); } while(t>0);
    EXPECT_NEAR(swmm_getValueExpanded(swmm_NODE,swmm_NODE_POLLUTANT_CONCENTRATION,0,-1,0),7.5,.01);
    swmm_end(); swmm_close();
}
TEST(JunctionStorage, FiniteVolumeUsesPhysicalChambersAndRefusesPressureAllowance) {
    for (bool pond : {false,true}) for (double rim : {1.,10.}) {
        auto text=deck(100,rim,pond,false,"FV",true);
        text += "[OPTIONS]\nFV_BACKEND CPU\n";
        const auto path=file("fv-"+std::to_string(rim)+(pond?"-pond":"-flood"),text);
        const auto t=run(path,false);
        EXPECT_LT(std::abs(t.error),.1);
        EXPECT_NEAR(t.final+t.flood+t.outflow,t.inflow+t.initial,.001*(t.inflow+t.initial));
        const double expected = t.depth > rim && pond ? 100*rim+200*(t.depth-rim) : 100*std::min(t.depth,rim);
        EXPECT_NEAR(t.volume,expected,1e-5);
    }
    auto text=deck(100,10,false,false,"FV",true);
    const auto pos=text.find("J1 0 10 0 0 0 100");
    text.replace(pos,std::string("J1 0 10 0 0 0 100").size(),"J1 0 10 0 2 0 100");
    const auto path=file("fv-unsupported-pressure",text);
    openswmm::SWMMEngine e; ASSERT_EQ(e.open(path.c_str(),nullptr,nullptr),0);
    EXPECT_NE(e.initialize(),0);
}
TEST(JunctionStorage, IsolatedConstantStorageReferenceHasTheSameGeometryAndResponse) {
    auto chamber=deck(100);
    auto storage=chamber;
    const auto pos=storage.find("[JUNCTIONS]\nJ1 0 10 0 0 0 100");
    storage.replace(pos,std::string("[JUNCTIONS]\nJ1 0 10 0 0 0 100").size(),
                    "[STORAGE]\nJ1 0 10 0 FUNCTIONAL 0 0 100");
    for (bool legacy : {false,true}) {
        const auto a=run(file("reference-chamber",chamber),legacy);
        const auto b=run(file("reference-storage",storage),legacy);
        EXPECT_NEAR(a.volume,b.volume,1e-8);
        EXPECT_NEAR(a.depth,b.depth,1e-8);
        EXPECT_LT(std::abs(b.error),.1);
    }
}
TEST(JunctionStorage, PumpWetWellUsesAuthoredGeometry) {
    auto input=deck(100);
    const auto pos=input.find("J1 0 10 0 0 0 100");
    input.replace(pos,std::string("J1 0 10 0 0 0 100").size(),"J1 0 10 2 0 0 100");
    input += "[PUMPS]\nP1 J1 O1 PC ON 0 0\n[CURVES]\nPC PUMP1 0 0.1\nPC 10000 0.1\n";
    const auto path=file("pump",input);
    for (bool legacy : {false,true}) {
        const auto t=run(path,legacy);
        EXPECT_NEAR(t.initial,200,1e-8);
        EXPECT_NEAR(t.volume,100*t.depth,1e-8);
        EXPECT_GT(t.outflow,50);
        EXPECT_LT(std::abs(t.error),.1);
    }
}
TEST(JunctionStorage, CheckpointRetainsChamberInventoryAndRejectsChangedArea) {
    const auto path=file("checkpoint",deck(100,10,false,false,"DYNWAVE",true));
    openswmm::SWMMEngine a,b,c;
    auto start=[&](openswmm::SWMMEngine& e, double area) {
        ASSERT_EQ(e.open(path.c_str(),nullptr,nullptr),0);
        e.context().nodes.storage_area[0]=area;
        ASSERT_EQ(e.initialize(),0); ASSERT_EQ(e.start(0),0);
        ASSERT_EQ(e.enableCheckpointing(),"");
    };
    start(a,100); start(b,100); start(c,101);
    double t=0;
    for (int k=0;k<20;++k) ASSERT_EQ(a.step(&t),0);
    const auto checkpoint_dir=work/"checkpoint-state";
    std::filesystem::remove_all(checkpoint_dir);
    std::filesystem::create_directories(checkpoint_dir);
    const auto cp=(checkpoint_dir/"chamber.chk").string();
    ASSERT_EQ(a.saveCheckpoint(cp),"");
    ASSERT_EQ(b.loadCheckpoint(cp),"");
    EXPECT_FALSE(c.loadCheckpoint(cp).empty());
    for (int k=0;k<20;++k) { ASSERT_EQ(a.step(&t),0); ASSERT_EQ(b.step(&t),0); }
    EXPECT_EQ(a.context().nodes.volume,b.context().nodes.volume);
    EXPECT_EQ(a.context().nodes.depth,b.context().nodes.depth);
}
TEST(JunctionStorage, ModelContainersRetainAreaInProjectUnits) {
    std::vector<std::pair<std::string,std::string>> formats{{"inp",""}};
#ifdef OPENSWMM_HAS_GEOPACKAGE
    formats.emplace_back("gpkg","org.hydrocouple.openswmm.plugins.geopackage");
#endif
#ifdef OPENSWMM_HAS_HDF5_MODEL
    formats.emplace_back("h5","org.hydrocouple.openswmm.plugins.hdf5");
#endif
    for (bool si : {false,true}) for (const auto& [ext,plugin] : formats) {
        SCOPED_TRACE(ext+(si?" SI":" US"));
        const auto path=file("container-source",deck(.125,10,false,si));
        SWMM_Engine a=swmm_engine_create(), b=swmm_engine_create();
        ASSERT_EQ(swmm_engine_open(a,path.c_str(),nullptr,nullptr,nullptr),0);
        const auto saved=(work/(std::string(si?"si":"us")+"-chamber."+ext)).string();
        std::filesystem::remove(saved);
        const char* id=plugin.empty()?nullptr:plugin.c_str();
        ASSERT_EQ(swmm_model_write_with_plugin(a,saved.c_str(),id),0);
        ASSERT_EQ(swmm_engine_open(b,saved.c_str(),nullptr,nullptr,id),0);
        double area=0; ASSERT_EQ(swmm_node_get_storage_area(b,0,&area),0);
        EXPECT_DOUBLE_EQ(area,.125);
        ASSERT_EQ(swmm_node_get_storage_area(b,1,&area),0); EXPECT_EQ(area,0);
        swmm_engine_destroy(a); swmm_engine_destroy(b);
    }
}
TEST(JunctionStorage, ParsersRejectNonfiniteAndNegativeArea) {
    for (const char* invalid : {"-1","nan","inf","1x"}) {
        auto text=deck(100);
        const auto pos=text.find("J1 0 10 0 0 0 100");
        text.replace(pos,std::string("J1 0 10 0 0 0 100").size(),std::string("J1 0 10 0 0 0 ")+invalid);
        const auto path=file(std::string("invalid-")+invalid,text);
        openswmm::SWMMEngine e; EXPECT_NE(e.open(path.c_str(),nullptr,nullptr),0);
        EXPECT_NE(swmm_open(path.c_str(),(path+".legacy.rpt").c_str(),""),0); swmm_close();
    }
}
} // namespace
