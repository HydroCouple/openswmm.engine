// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include "core/SWMMEngine.hpp"
#include "core/CompletedSourceArchive.hpp"
#include "core/DateTime.hpp"
#include "output/OutputReader.hpp"
#include <openswmm/plugin_sdk/IOutputPlugin.hpp>
#ifdef OPENSWMM_HAS_2D
#include "2d/subsurface/CommonSourceStage.hpp"
#include "hydrology/LidNode.hpp"
#include "2d/subsurface/SourceResultHistory.hpp"
#include "2d/subsurface/EngineSourceResultHistory.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include <openswmm/engine/openswmm_coupling.h>
#ifdef OPENSWMM_MESH_OUTPUT_CHECKPOINT_TEST
#include "2d/output/Default2DOutputPlugin.hpp"
#endif
#endif
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <iterator>
#include <future>

using namespace openswmm;
namespace {
class HeldOutput final : public IOutputPlugin {
public:
    std::mutex mutex;
    std::condition_variable changed;
    bool entered=false,released=false;
    int writes=0;
    PluginState state() const noexcept override {return PluginState::PREPARED;}
    int initialize(const std::vector<std::string>&,const IPluginComponentInfo*) override {return 0;}
    int validate(const SimulationContext&) override {return 0;}
    int prepare(const SimulationContext&) override {return 0;}
    int finalize(const SimulationContext&) override {return 0;}
    const char* last_error_message() const noexcept override {return "held output";}
    int update(const SimulationSnapshot&) override {
        std::unique_lock lock(mutex);entered=true;changed.notify_all();
        changed.wait(lock,[&]{return released;});
        return ++writes==2?-17:0;
    }
};
std::filesystem::path root() {
    const auto* env=std::getenv("SWMM_R4_SESSION_OUT");
    auto p=env ? std::filesystem::path(env) : std::filesystem::path(__FILE__).parent_path()/"engine_checkpoint_out";
    std::filesystem::create_directories(p);return p;
}
std::string bytes(const std::filesystem::path& p) {
    std::ifstream in(p,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};
}
void write(const std::filesystem::path& p,const std::string& data) {std::ofstream out(p,std::ios::binary);out<<data;}
void start(SWMMEngine& e,const std::string& name,int lid=0,int schedule=0,bool controls=true,bool checkpoint=true,int surcharge=0) {
    auto p=root()/name;
    std::string deck="[OPTIONS]\nFLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\nSTART_DATE 01/31/2004\nSTART_TIME 23:59:00\nEND_DATE 02/01/2004\nEND_TIME 00:02:00\nROUTING_STEP .7\nMINIMUM_STEP .001\nVARIABLE_STEP .75\nRULE_STEP 00:00:02\nWET_STEP 00:00:10\nDRY_STEP 00:00:20\nREPORT_STEP 00:00:03\nIGNORE_QUALITY YES\nIGNORE_2D YES\nSKIP_STEADY_STATE ";
    deck+=schedule==2 ? "YES\n" : "NO\n";
    deck+="[STORAGE]\nS 0 3 1 FUNCTIONAL 0 0 100 0 .5 6 1 .3\n[OUTFALLS]\nO 0 FIXED .4 NO\n[CONDUITS]\nC S O 30 .013 .75 0 0 0\n[XSECTIONS]\nC CIRCULAR .3 0 0 0\n[EVAPORATION]\nMONTHLY .1 .2 .1 .1 .1 .1 .1 .1 .1 .1 .1 .1\n";
    if(!lid && schedule!=2)deck.replace(deck.find("FUNCTIONAL 0 0 100"),std::string("FUNCTIONAL 0 0 100").size(),"FUNCTIONAL 20 1 100");
    if(lid)deck.replace(deck.find("S 0 3 1 FUNCTIONAL 0 0 100 0 .5 6 1 .3"),std::string("S 0 3 1 FUNCTIONAL 0 0 100 0 .5 6 1 .3").size(),"S 0 2 1 FUNCTIONAL 0 0 100 0 0");
    if(schedule==2 && !lid) {
        deck.replace(deck.find("S 0 3 1"),7,"S 0 3 0");
        deck.replace(deck.find("0 .5 6 1 .3"),10,"0 0");
        deck.replace(deck.find("FIXED .4"),8,"FIXED 0");
        deck.replace(deck.find("MONTHLY .1 .2 .1 .1 .1 .1 .1 .1 .1 .1 .1 .1"),std::string("MONTHLY .1 .2 .1 .1 .1 .1 .1 .1 .1 .1 .1 .1").size(),"CONSTANT 0");
    }
    if(schedule!=2)deck+="[INFLOWS]\nS FLOW Q FLOW 1 1 0\n[TIMESERIES]\nQ 01/31/2004 23:59:00 .05\nQ 01/31/2004 23:59:27 -.02\nQ 02/01/2004 00:00:31 .025\nQ 02/01/2004 00:02:00 .03\n[DWF]\nS FLOW .005\n";
    if(controls)deck+="[CONTROLS]\nRULE CLOSED_RULE\nIF NODE S DEPTH > .2\nTHEN CONDUIT C STATUS = OFF\nELSE CONDUIT C STATUS = ON\nPRIORITY 1\nRULE OPEN_RULE\nIF SIMULATION TIME > 00:01:00\nTHEN CONDUIT C STATUS = ON\nPRIORITY 2\n";
    if(lid) {
        deck+="[LID_CONTROLS]\nStack NODE\nStack SURFACE 6 .1\nStack MEDIA 12 .45 .2 .08 2 10 3\nStack AGGREGATE 6 .4 100\nStack BOTTOM .5 0\n[LID_NODES]\nS Stack 10\n";
        if(lid>=2)deck+="[LID_RICHARDS]\nStack OPTIONS 4 1e-7 1e-5 30\nStack 2 .03 2 1.6 .5 .0001\nStack 3 .03 2 1.6 .5 .0001\n";
    }
    if(lid==3)deck+="[STORAGE]\nS2 0 2 1 FUNCTIONAL 0 0 80 0 0\n[CONDUITS]\nC2 S2 O2 30 .013 .5 0 0 0\n[XSECTIONS]\nC2 CIRCULAR .3 0 0 0\n[LID_NODES]\nS2 Stack 20\n[OUTFALLS]\nO2 0 FIXED 1.25 NO\n";
    write(p.string()+".inp",deck);
    auto error=e.open((p.string()+".inp").c_str(),(p.string()+".rpt").c_str(),nullptr);
    if(error)throw std::runtime_error("open "+name+": "+std::to_string(error)+" "+e.context().error_message+" "+(e.context().errors.empty()?"":e.context().errors.front()));
    if(schedule==1) {
        double date=e.context().options.start_date;
        e.context().events={{datetime::addSeconds(date,15),datetime::addSeconds(date,55)},
                            {datetime::addSeconds(date,85),datetime::addSeconds(date,140)}};
    }
    e.context().options.surcharge_method=surcharge;
    if(name.starts_with("average-"))e.context().options.rpt_averages=true;
    if(e.initialize() || e.start(0))throw std::runtime_error("initialize/start "+name+": "+e.context().error_message+" "+(e.context().errors.empty()?"":e.context().errors.front()));
    if(checkpoint) {auto result=e.enableCheckpointing();if(!result.empty())throw std::runtime_error(result);}
}
// Mixed links exercise the engine-owned callback, sequential shared-well pumps,
// PID history, slow orifice motion and current LID cell-port/backwater heads.
void startStructures(SWMMEngine& e,const std::string& name,int mode=0,int lid=0,int kind=0,bool checkpoint=true,double cd=.65) {
    auto p=root()/name;
    std::string deck="[OPTIONS]\nFLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\nSTART_DATE 01/31/2004\nSTART_TIME 23:59:00\nEND_DATE 02/01/2004\nEND_TIME 00:01:00\nROUTING_STEP .7\nMINIMUM_STEP .001\nVARIABLE_STEP .75\nRULE_STEP 00:00:02\nREPORT_STEP 00:00:03\nIGNORE_QUALITY YES\nIGNORE_2D YES\nSKIP_STEADY_STATE NO\n";
    deck+="[STORAGE]\nS 0 3 1.4 FUNCTIONAL 0 0 100 0 .2 6 1 .3\n[JUNCTIONS]\nJ 0 3 .1 0 0\n[OUTFALLS]\nO 0 FIXED .8 NO\nOR 0 FIXED .8 NO\nOW 0 FIXED .8 NO\nOX 0 FIXED .8 NO\n[CONDUITS]\nC J O 40 .013 0 0 0 0\n[PUMPS]\n";
    deck+=kind==5 ? "P T J " : "P S J ";
    deck+=kind==5 ? "*" : "PC";
    deck+=" ON .9 .3\nP2 S J PC2 ON 1.1 .4\n[ORIFICES]\nR S OR ";deck+=kind%2 ? "SIDE" : "BOTTOM";
    deck+=" .6 "+std::to_string(cd)+" NO .01\n[WEIRS]\nW S OW ";
    const char* kinds[]={"TRANSVERSE","SIDEFLOW","V-NOTCH","TRAPEZOIDAL","ROADWAY"};
    deck+=kinds[kind%5];deck+=" 1.05 3.33 NO 0 0 YES";if(kind%5==4)deck+=" 8 PAVED";deck+="\n";
    deck+="[OUTLETS]\nX S OX .5 ";deck+=kind%2 ? "TABULAR/HEAD XC" : "FUNCTIONAL/DEPTH .04 .5";deck+=" NO\n[XSECTIONS]\nC CIRCULAR .5 0 0 0\nR CIRCULAR .2 0 0 0\nW ";
    deck+=kind%5==2 ? "TRIANGULAR 1 60 0 0" : (kind%5==3 ? "TRAPEZOIDAL 1 .3 1 1" : "RECT_OPEN 1 .3 0 0");
    deck+="\n[CURVES]\nXC RATING 0 0\nXC 3 .1\nPC2 PUMP4 0 0\nPC2 .8 .025\nPC2 3 .08\n";
    if(kind<5) {
        deck+="PC PUMP"+std::to_string(kind+1);
        deck+=kind==0 ? " 0 .01\nPC 50 .04\nPC 300 .08\n" : (kind==2 ? " 0 .08\nPC 1 .04\nPC 3 0\n" : " 0 0\nPC .8 .025\nPC 3 .08\n");
    }
    deck+="[EVAPORATION]\nMONTHLY .1 .2 .1 .1 .1 .1 .1 .1 .1 .1 .1 .1\n[INFLOWS]\nS FLOW Q FLOW 1 1 .02\n[TIMESERIES]\nQ 01/31/2004 23:59:00 .02\nQ 01/31/2004 23:59:35 .09\nQ 02/01/2004 00:00:30 -.01\nQ 02/01/2004 00:01:00 .07\nA 01/31/2004 23:59:00 .2\nA 02/01/2004 00:01:00 .9\n";
    deck+="[CONTROLS]\nRULE P_PID\nIF NODE S DEPTH <> 1.2\nTHEN PUMP P SETTING = PID -.1 .5 .005\nRULE R_PID\nIF NODE S DEPTH <> 1.1\nTHEN ORIFICE R SETTING = PID -.1 .5 .005\nRULE W_TS\nIF NODE S DEPTH > 0\nTHEN WEIR W SETTING = TIMESERIES A\nRULE X_NUM\nIF SIMULATION TIME > 00:00:50\nTHEN OUTLET X SETTING = .4\nELSE OUTLET X SETTING = .8\nRULE R_OFF\nIF SIMULATION TIME > 00:01:10\nAND SIMULATION TIME < 00:01:25\nTHEN ORIFICE R SETTING = 0\nPRIORITY 2\n";
    if(kind==5)deck+="[STORAGE]\nT 0 3 1.4 FUNCTIONAL 0 0 100 0 0\n[INFLOWS]\nT FLOW \"\" FLOW 1 1 .015\n";
    if(lid) {
        deck.replace(deck.find("S 0 3 1.4 FUNCTIONAL 0 0 100 0 .2 6 1 .3"),std::string("S 0 3 1.4 FUNCTIONAL 0 0 100 0 .2 6 1 .3").size(),"S 0 2 1.4 FUNCTIONAL 0 0 100 0 .2");
        deck+="[LID_CONTROLS]\nStack NODE\nStack SURFACE 6 .1\nStack MEDIA 12 .45 .2 .08 2 10 3\nStack AGGREGATE 6 .4 100\nStack BOTTOM .5 0\n[LID_NODES]\nS Stack 25\n";
        if(lid>=2)deck+="[LID_RICHARDS]\nStack OPTIONS 4 1e-7 1e-5 30\nStack 2 .03 2 1.6 .5 .0001\nStack 3 .03 2 1.6 .5 .0001\n";
        if(lid==3)deck+="[STORAGE]\nS2 0 2 1 FUNCTIONAL 0 0 80 0 0\n[CONDUITS]\nC2 S2 O2 30 .013 .5 0 0 0\n[XSECTIONS]\nC2 CIRCULAR .3 0 0 0\n[LID_NODES]\nS2 Stack 20\n[OUTFALLS]\nO2 0 FIXED 1.25 NO\n";
    }
    write(p.string()+".inp",deck);
    if(e.open((p.string()+".inp").c_str(),(p.string()+".rpt").c_str(),nullptr))throw std::runtime_error("structure open: "+e.context().error_message+" "+(e.context().errors.empty()?"":e.context().errors.front()));
    e.context().options.surcharge_method=mode;
    if(kind%2) {auto date=e.context().options.start_date;e.context().events={{datetime::addSeconds(date,5),datetime::addSeconds(date,40)},{datetime::addSeconds(date,60),datetime::addSeconds(date,110)}};}
    if(e.initialize() || e.start(0))throw std::runtime_error("structure start: "+e.context().error_message);
    if(checkpoint) {auto error=e.enableCheckpointing();if(!error.empty())throw std::runtime_error(error);}
}
// Rainfall, upstream run-on, outfall feedback and all eight legacy LID types
// share the actual engine scheduler/structure/node-LID pipeline.
void startRunoff(SWMMEngine& e,const std::string& name,int method=0,int rain=0,int node_lid=0,bool checkpoint=true) {
    SWMMEngine authored;startStructures(authored,name,method%4,node_lid,0,false);
    if(authored.close())throw std::runtime_error("runoff fixture close failed");
    auto p=root()/name;auto deck=bytes(p.string()+".inp");
    deck.replace(deck.find("END_TIME 00:01:00"),std::string("END_TIME 00:01:00").size(),"END_TIME 00:03:00");
    deck+="[OPTIONS]\nIGNORE_SNOWMELT YES\nIGNORE_GROUNDWATER YES\nWET_STEP 00:00:07\nDRY_STEP 00:00:23\n";
    const char* methods[]={"HORTON","MODIFIED_HORTON","GREEN_AMPT","MODIFIED_GREEN_AMPT","CURVE_NUMBER"};
    deck+="INFILTRATION "+std::string(methods[method])+"\n[RAINGAGES]\n";
    const char* formats[]={"INTENSITY","VOLUME","CUMULATIVE"};
    for(const char* g:{"R1","R2","Unused"})deck+=std::string(g)+" "+formats[rain]+" 0:01 1 TIMESERIES Rain\n";
    deck+="[SUBCATCHMENTS]\nA R1 B .05 45 30 1 0\nB R2 S .08 65 40 2 0\nD R1 S .06 35 35 1 0\n[SUBAREAS]\nA .015 .12 .03 .05 35 PERVIOUS 55\nB .02 .13 .02 .04 25 IMPERVIOUS 45\nD .02 .13 .02 .04 25 OUTLET\n[INFILTRATION]\n";
    for(const char* sub:{"A","B","D"})deck+=std::string(sub)+(method<2 ? " 3 .2 4 7 2\n" : (method<4 ? " 3 .25 .3\n" : " 75 .25 7\n"));
    deck+="[LID_CONTROLS]\n";
    for(const char* type:{"BC","RG","GR","IT","PP","RB","RD","VS"}) {
        auto n=std::string("U")+type;deck+=n+" "+type+"\n"+n+" SURFACE 3 .05 .12 1 3\n";
        if(n=="UBC" || n=="URG" || n=="UGR" || n=="UPP")deck+=n+" SOIL 6 .45 .2 .08 1 10 3\n";
        if(n=="UPP")deck+=n+" PAVEMENT 3 .2 0 1 0 0 0\n";
        if(n=="UBC" || n=="UIT" || n=="UPP" || n=="URB")deck+=n+" STORAGE 6 .667 .1 0\n";
        if(n=="UGR")deck+=n+" DRAINMAT 2 .5 .1\n";
        if(n=="UBC" || n=="UIT" || n=="UPP" || n=="URB")deck+=n+" DRAIN .5 .5 0 0\n";
        if(n=="URD")deck+=n+" DRAIN 2 0 0 0\n";
    }
    deck+="[LID_USAGE]\nA UBC 2 20 3 70 30 1 * B\nA URG 1 20 3 50 30 1\nA UGR 1 20 3 50 20 0 * S\nB UIT 1 20 3 70 20 0 * S\nB UPP 1 20 3 70 20 0 * S\nB URB 1 20 3 70 20 0 * A\nD URD 1 20 3 0 30 1\nD UVS 1 20 3 0 30 0\n";
    deck+="[TIMESERIES]\nRain 01/31/2004 23:59:12 ";deck+=rain==0 ? "5" : ".08";
    deck+="\nRain 02/01/2004 00:00:12 ";deck+=rain==2 ? ".14" : (rain==0 ? "3" : ".05");
    deck+="\nRain 02/01/2004 00:01:12 ";deck+=rain==2 ? ".14" : "0";
    deck+="\nRain 02/01/2004 00:02:12 ";deck+=rain==0 ? "4" : ".04";
    deck+="\n[PATTERNS]\nMonth MONTHLY .8 1.2 1 1 1 1 1 1 1 1 1 1\n[ADJUSTMENTS]\nRAIN .9 1.2 1 1 1 1 1 1 1 1 1 1\nINFIL A Month\nN-PERV B Month\nDSTORE D Month\n";
    write(p.string()+".inp",deck);
    if(e.open((p.string()+".inp").c_str(),(p.string()+".rpt").c_str(),nullptr))throw std::runtime_error("runoff open: "+e.context().error_message+" "+(e.context().errors.empty()?"":e.context().errors.front()));
    e.context().gages.scale_factor[1]=1.3;
    e.context().subcatches.rain_scale_factor[1]=.8;
    e.context().node_subtypes.outfalls.route_to[0]=2;
    if(e.initialize() || e.start(0))throw std::runtime_error("runoff start: "+e.context().error_message+" "+(e.context().errors.empty()?"":e.context().errors.front()));
    if(checkpoint) {auto error=e.enableCheckpointing();if(!error.empty())throw std::runtime_error(error);}
}
void step(SWMMEngine& e) {
    double t=-1;auto result=e.step(&t);
    if(result)throw std::runtime_error("step at "+std::to_string(e.context().current_time)+": "+e.context().error_message);
}
void compare(SWMMEngine& a,SWMMEngine& b,const std::string& name) {
    auto pa=root()/(name+"-a.chk"),pb=root()/(name+"-b.chk");
    ASSERT_EQ(a.saveCheckpoint(pa.string()),"");
    ASSERT_EQ(b.saveCheckpoint(pb.string()),"");
    ASSERT_EQ(bytes(pa),bytes(pb));
}
uint32_t crc(const std::string& bytes) {
    uint32_t c=~uint32_t(0);for(unsigned char v:bytes) {c^=v;for(int k=0;k<8;++k)c=(c>>1)^((c&1)?0xedb88320u:0u);}return ~c;
}
}

TEST(EngineCheckpoint, ActualEngineContinuationPreservesClocksControlsExfiltrationAndLedgers) {
    for(int mode=0;mode<4;++mode)for(int lid=0;lid<4;++lid)for(int schedule=0;schedule<3;++schedule) {
        auto name="continuation-"+std::to_string(mode)+"-"+std::to_string(lid)+"-"+std::to_string(schedule);SCOPED_TRACE(name);
        SWMMEngine a,b;
    ASSERT_NO_THROW(start(a,name+"-a",lid,schedule,schedule!=2,true,mode));
        ASSERT_NO_THROW(start(b,name+"-b",lid,schedule,schedule!=2,true,mode));
        const double at=schedule==1 ? 60 : 40;
        while(a.context().current_time<at)ASSERT_NO_THROW(step(a));
        if(schedule!=2)EXPECT_EQ(a.context().links.target_setting[0],0);
        if(schedule==0)EXPECT_GT(a.context().mass_balance.step_ext_withdrawal,0);
        auto p=root()/(name+".chk");ASSERT_EQ(a.saveCheckpoint(p.string()),"");
        ASSERT_EQ(b.loadCheckpoint(p.string()),"");compare(a,b,name);
        int count=0;
        while(a.context().elapsed_ms<a.context().options.totalDurationMs()) {
            ASSERT_NO_THROW(step(a));
            ASSERT_NO_THROW(step(b));
            ASSERT_EQ(a.context().current_time,b.context().current_time);
            if(++count%13==0)compare(a,b,name);
        }
        compare(a,b,name);
        if(lid==0 && schedule==0)EXPECT_GT(a.context().mass_balance.routing_seep_loss,0);
        if(schedule==2 && lid==0)EXPECT_GT(a.context().routing_stats.steady_time,0);
        if(schedule!=2)EXPECT_EQ(a.context().links.target_setting[0],1);
        if(lid>0)EXPECT_EQ(a.context().routing_stats.steady_time,0);
        EXPECT_GT(a.context().routing_stats.report_steps,0);
        ASSERT_EQ(a.end(),0);ASSERT_EQ(b.end(),0);
        EXPECT_EQ(a.context().mass_balance.routing_error(),b.context().mass_balance.routing_error());
        EXPECT_EQ(a.context().routing_stats.computed_avg_iterations(),b.context().routing_stats.computed_avg_iterations());
    }
}

TEST(EngineCheckpoint, InitialAndAveragingBoundariesAndFreshOnlyRestore) {
    SWMMEngine a,b;
    ASSERT_NO_THROW(start(a,"initial-a"));
    ASSERT_NO_THROW(start(b,"initial-b"));
    auto p=root()/"initial.chk";ASSERT_EQ(a.saveCheckpoint(p.string()),"");ASSERT_EQ(b.loadCheckpoint(p.string()),"");
    compare(a,b,"initial");
    ASSERT_NO_THROW(step(a));
    ASSERT_NO_THROW(step(b));compare(a,b,"initial-step");
    EXPECT_FALSE(b.loadCheckpoint(p.string()).empty());
    SWMMEngine c,d;
    ASSERT_NO_THROW(start(c,"average-c",0,0,true,false));
    ASSERT_NO_THROW(start(d,"average-d",0,0,true,false));
    ASSERT_EQ(c.enableCheckpointing(),"");ASSERT_EQ(d.enableCheckpointing(),"");
    for(int i=0;i<27;++i)ASSERT_NO_THROW(step(c));
    auto q=root()/"average.chk";ASSERT_EQ(c.saveCheckpoint(q.string()),"");ASSERT_EQ(d.loadCheckpoint(q.string()),"");
    for(int i=0;i<45;++i) {ASSERT_NO_THROW(step(c));
    ASSERT_NO_THROW(step(d));}
    compare(c,d,"average");
}

TEST(EngineCheckpoint, CorruptionAndModelMismatchLeaveFreshTargetUnchangedWithValidRetry) {
    SWMMEngine a;
    ASSERT_NO_THROW(start(a,"atomic-source"));
    for(int i=0;i<50;++i)ASSERT_NO_THROW(step(a));
    auto good=root()/"atomic-good.chk";ASSERT_EQ(a.saveCheckpoint(good.string()),"");auto valid=bytes(good);
    detail::CompletedSourceArchive input(valid);std::string magic,model,payload;uint32_t version,checksum;
    input(magic,version,model,checksum,payload);input.finish();
    for(int variant=0;variant<13;++variant) {
        auto name="atomic-"+std::to_string(variant);SCOPED_TRACE(name);SWMMEngine b;
    ASSERT_NO_THROW(start(b,name));
        auto before=root()/(name+"-before.chk"),after=root()/(name+"-after.chk"),bad=root()/(name+"-bad.chk");
        ASSERT_EQ(b.saveCheckpoint(before.string()),"");std::string mutant=valid;
        if(variant==0)mutant.resize(mutant.size()/2);
        else if(variant==1)mutant.back()^=1;
        else if(variant==2)mutant+="extra";
        else {
            auto data=payload;auto v=version;
            if(variant==3)v=99;
            if(variant==4) {double nan=std::numeric_limits<double>::quiet_NaN();std::memcpy(data.data(),&nan,sizeof(nan));}
            if(variant==5) {double mismatch=1;std::memcpy(data.data()+sizeof(double),&mismatch,sizeof(mismatch));}
            if(variant==6) {double future=1e15;std::memcpy(data.data()+9*sizeof(double),&future,sizeof(future));}
            if(variant==7)data.resize(data.size()-1);
            if(variant==8)data+="extra";
            if(variant==9)data[7*sizeof(double)+6*sizeof(double)+sizeof(int)]=2; // invalid bool
            if(variant==10) {int event=-1;std::memcpy(data.data()+13*sizeof(double),&event,sizeof(event));}
            if(variant==11) {double future=1e15;std::memcpy(data.data()+12*sizeof(double),&future,sizeof(future));}
            if(variant==12) {
                // Explicit v1 layout through the node-sized zero LID-drain row.
                const auto offset=7*sizeof(double)+6*sizeof(double)+2*sizeof(int)+4*sizeof(bool)+2*sizeof(double)+11*sizeof(double)+sizeof(uint64_t)+a.context().n_nodes()*sizeof(double)+23*sizeof(double);
                double stored;std::memcpy(&stored,data.data()+offset,sizeof(stored));ASSERT_EQ(stored,a.context().mass_balance.routing_final_storage);
                stored+=1;std::memcpy(data.data()+offset,&stored,sizeof(stored));
            }
            auto sum=crc(data);detail::CompletedSourceArchive out;out(magic,v,model,sum,data);mutant=out.bytes();
        }
        write(bad,mutant);EXPECT_FALSE(b.loadCheckpoint(bad.string()).empty());
        ASSERT_EQ(b.saveCheckpoint(after.string()),"");EXPECT_EQ(bytes(before),bytes(after));
        ASSERT_EQ(b.loadCheckpoint(good.string()),"");compare(a,b,name+"-retry");
    }
    SWMMEngine c;
    ASSERT_NO_THROW(start(c,"mismatched",0,1));EXPECT_FALSE(c.loadCheckpoint(good.string()).empty());
}

TEST(EngineCheckpoint, RefusesIncompleteProfilesChangedModelsAndUnrelatedDestinations) {
    SWMMEngine a;
    ASSERT_NO_THROW(start(a,"guards"));
    EXPECT_FALSE(a.enableCheckpointing().empty());
    auto file=root()/"unrelated.txt";write(file,"keep this file");EXPECT_FALSE(a.saveCheckpoint(file.string()).empty());EXPECT_EQ(bytes(file),"keep this file");
    a.context().options.rule_step+=1;auto p=root()/"changed.chk";EXPECT_FALSE(a.saveCheckpoint(p.string()).empty());
    EXPECT_NE(a.step(nullptr),0);EXPECT_FALSE(a.saveCheckpoint(p.string()).empty());
    SWMMEngine b;
    ASSERT_NO_THROW(start(b,"unsupported",0,0,false,false));
    b.context().options.temp_source=2;EXPECT_FALSE(b.enableCheckpointing().empty());
    b.context().options.temp_source=0;b.context().tables[0].is_file_based=true;EXPECT_FALSE(b.enableCheckpointing().empty());
    b.context().tables[0].is_file_based=false;b.context().forcing.climate_evap_mode=ForcingMode::OVERRIDE;EXPECT_FALSE(b.enableCheckpointing().empty());
    b.context().forcing.climate_evap_mode=ForcingMode::NONE;ASSERT_EQ(b.enableCheckpointing(),"");
    ASSERT_EQ(b.close(),0);
    ASSERT_NO_THROW(start(b,"guards-reopened"));
    ASSERT_NO_THROW(step(b));
    EXPECT_EQ(b.saveCheckpoint((root()/"reopened.chk").string()),"");
}

TEST(EngineCheckpoint, StructuralPIDContinuationUsesRealEngineStepsAndCurrentLIDPorts) {
    for(int mode=0;mode<4;++mode)for(int lid=0;lid<4;++lid)for(int kind=0;kind<6;++kind) {
        auto name="structural-"+std::to_string(mode)+"-"+std::to_string(lid)+"-"+std::to_string(kind);SCOPED_TRACE(name);
        SWMMEngine a,b;
        ASSERT_NO_THROW(startStructures(a,name+"-a",mode,lid,kind));
        ASSERT_NO_THROW(startStructures(b,name+"-b",mode,lid,kind));
        while(a.context().current_time<47)ASSERT_NO_THROW(step(a));
        auto p=root()/(name+".chk");ASSERT_EQ(a.saveCheckpoint(p.string()),"");
        ASSERT_EQ(b.loadCheckpoint(p.string()),"");compare(a,b,name);
        int count=0;bool moving=false,flowing=false;
        while(a.context().elapsed_ms<a.context().options.totalDurationMs()) {
            ASSERT_NO_THROW(step(a));
            ASSERT_NO_THROW(step(b));
            for(int j=0;j<a.context().n_links();++j) {
                flowing|=a.context().links.flow[j]!=0;
                moving|=a.context().links.setting[j]!=a.context().links.target_setting[j];
            }
            if(++count%17==0)compare(a,b,name);
        }
        compare(a,b,name);EXPECT_TRUE(flowing);EXPECT_TRUE(moving);
        EXPECT_GT(a.context().links.stat_pump_volume[a.context().link_names.find("P2")],0);
        ASSERT_EQ(a.end(),0);ASSERT_EQ(b.end(),0);
        EXPECT_EQ(a.context().mass_balance.routing_error(),b.context().mass_balance.routing_error());
    }
}

TEST(EngineCheckpoint, StructuralStateCorruptionAndMaterialChangePreserveFreshTargets) {
    SWMMEngine a;
    ASSERT_NO_THROW(startStructures(a,"structural-atomic-source"));
    SWMMEngine initial;
    ASSERT_NO_THROW(startStructures(initial,"structural-initial"));
    auto opening=root()/"structural-initial.chk";ASSERT_EQ(a.saveCheckpoint(opening.string()),"");
    ASSERT_EQ(initial.loadCheckpoint(opening.string()),"");compare(a,initial,"structural-opening");
    for(int i=0;i<71;++i)ASSERT_NO_THROW(step(a));
    auto good=root()/"structural-atomic.chk";ASSERT_EQ(a.saveCheckpoint(good.string()),"");
    detail::CompletedSourceArchive in(bytes(good));std::string magic,model,payload;uint32_t version,sum;
    in(magic,version,model,sum,payload);in.finish();ASSERT_EQ(version,3);
    auto hydraulic=payload.find("OpenSWMM accepted DW hydraulic boundary");ASSERT_NE(hydraulic,std::string::npos);
    // Two fixed PID rows (3 doubles each), one orifice and one weir area precede
    // the length-prefixed nested archive. This fixture keeps those shapes explicit.
    const auto begin=hydraulic-2*sizeof(uint64_t)-(3*sizeof(uint64_t)+8*sizeof(double));
    uint64_t n;std::memcpy(&n,payload.data()+begin,sizeof(n));ASSERT_EQ(n,2);
    double e1;std::memcpy(&e1,payload.data()+begin+sizeof(uint64_t)+sizeof(double),sizeof(e1));EXPECT_NE(e1,0);
    for(int k=0;k<5;++k) {
        SWMMEngine b;auto name="structural-mutant-"+std::to_string(k);SCOPED_TRACE(name);
        ASSERT_NO_THROW(startStructures(b,name));
        auto before=root()/(name+"-before.chk"),after=root()/(name+"-after.chk"),bad=root()/(name+"-bad.chk");
        ASSERT_EQ(b.saveCheckpoint(before.string()),"");auto data=payload;auto v=version;
        if(k==0) {double nan=std::numeric_limits<double>::quiet_NaN();std::memcpy(data.data()+begin+sizeof(uint64_t),&nan,sizeof(nan));}
        if(k==1) {uint64_t wrong=3;std::memcpy(data.data()+begin,&wrong,sizeof(wrong));}
        if(k==2 || k==3) {double negative=-1;auto area=begin+2*sizeof(uint64_t)+6*sizeof(double)+(k==3 ? sizeof(uint64_t)+sizeof(double) : 0);std::memcpy(data.data()+area,&negative,sizeof(negative));}
        if(k==4)v=version-1;
        sum=crc(data);detail::CompletedSourceArchive out;out(magic,v,model,sum,data);write(bad,out.bytes());
        EXPECT_FALSE(b.loadCheckpoint(bad.string()).empty());ASSERT_EQ(b.saveCheckpoint(after.string()),"");EXPECT_EQ(bytes(before),bytes(after));
        ASSERT_EQ(b.loadCheckpoint(good.string()),"");compare(a,b,name+"-retry");
    }
    SWMMEngine c;
    ASSERT_NO_THROW(startStructures(c,"structural-material-change",0,0,0,true,.7));
    EXPECT_FALSE(c.loadCheckpoint(good.string()).empty());
    c.context().link_subtypes.orifices.orate[0]+=1;
    EXPECT_FALSE(c.saveCheckpoint((root()/"structural-changed.chk").string()).empty());
    EXPECT_NE(c.step(nullptr),0);
}


TEST(EngineCheckpoint, RunoffRainfallAndLegacyLIDContinuationPreservePendingWater) {
    for(int method=0;method<5;++method)for(int rain=0;rain<3;++rain)for(int node=0;node<3;++node) {
        auto name="runoff-"+std::to_string(method)+"-"+std::to_string(rain)+"-"+std::to_string(node);SCOPED_TRACE(name);
        SWMMEngine a,b;
        ASSERT_NO_THROW(startRunoff(a,name+"-a",method,rain,node));
        ASSERT_NO_THROW(startRunoff(b,name+"-b",method,rain,node));
        // Initial rain state has a gap before its first record.
        auto initial=root()/(name+"-initial.chk");ASSERT_EQ(a.saveCheckpoint(initial.string()),"");
        ASSERT_EQ(b.loadCheckpoint(initial.string()),"");compare(a,b,name+"-initial");
        while(a.context().current_time<47)ASSERT_NO_THROW(step(a));
        EXPECT_GT(a.context().subcatches.outfall_runon_vol[2],0);
        EXPECT_GT(a.context().mass_balance.runoff_rainfall,0);
        EXPECT_EQ(a.context().gages.co_gage_index[1],0);
        EXPECT_GT(a.context().gages.past_rain_accum[0],0);
        auto saved=root()/(name+".chk");ASSERT_EQ(a.saveCheckpoint(saved.string()),"");
        ASSERT_EQ(b.loadCheckpoint(saved.string()),"");compare(a,b,name);
        bool dry=false,drain=false,return_to_perv=false,runon=false;int count=0;
        while(a.context().elapsed_ms<a.context().options.totalDurationMs()) {
            ASSERT_NO_THROW(step(a));
            ASSERT_NO_THROW(step(b));
            ASSERT_EQ(a.context().current_time,b.context().current_time);
            dry |= a.context().subcatches.rainfall[0]==0;
            for(double v:a.context().subcatches.lid_drain_flow)drain|=v>0;
            for(double v:a.context().subcatches.lid_return_to_perv_cfs)return_to_perv|=v>0;
            for(double v:a.context().subcatches.runon_inflow)runon|=v>0;
            if(++count%37==0)compare(a,b,name);
        }
        compare(a,b,name);EXPECT_TRUE(dry);EXPECT_TRUE(drain);EXPECT_TRUE(return_to_perv);EXPECT_TRUE(runon);
        ASSERT_EQ(a.end(),0);ASSERT_EQ(b.end(),0);
        EXPECT_EQ(a.context().mass_balance.runoff_error(),b.context().mass_balance.runoff_error());
        EXPECT_EQ(a.context().lid_usage.wb_inflow,b.context().lid_usage.wb_inflow);
        EXPECT_EQ(a.context().lid_usage.wb_final_vol,b.context().lid_usage.wb_final_vol);
    }
}

TEST(EngineCheckpoint, RunoffCorruptionAndUnsupportedProfilesPreserveFreshTargets) {
    SWMMEngine a;
    ASSERT_NO_THROW(startRunoff(a,"runoff-atomic-source",2,2,2));
    while(a.context().current_time<47)ASSERT_NO_THROW(step(a));
    auto good=root()/"runoff-atomic.chk";ASSERT_EQ(a.saveCheckpoint(good.string()),"");
    detail::CompletedSourceArchive in(bytes(good));std::string magic,model,payload;uint32_t version,sum;
    in(magic,version,model,sum,payload);in.finish();
    const std::string tag="engine runoff/rainfall boundary v3";auto begin=payload.find(tag);ASSERT_NE(begin,std::string::npos);
    begin+=tag.size();detail::CompletedSourceArchive water_in(payload.substr(begin));std::string water,units;water_in(water,units);
    const auto runoff_at=begin+sizeof(uint64_t),lid_at=runoff_at+water.size()+sizeof(uint64_t);
    const auto sub_at=lid_at+units.size();
    // Walk only the explicit nested layout; mutations retain a valid CRC.
    auto skip=[](const std::string& data,std::size_t& offset,std::size_t width=sizeof(double)) {
        uint64_t n;std::memcpy(&n,data.data()+offset,sizeof(n));offset+=sizeof(n)+n*width;
    };
    std::size_t bank=0;for(int i=0;i<23;++i)skip(water,bank);
    uint64_t n;std::memcpy(&n,water.data()+bank,sizeof(n));ASSERT_EQ(n,3);bank+=sizeof(n)+n*3*10*sizeof(double);
    skip(water,bank);skip(water,bank,8*sizeof(double)); // cumulative, Horton rows
    const auto ga=bank+sizeof(uint64_t);
    for(int k=0;k<9;++k) {
        SWMMEngine b;auto name="runoff-mutant-"+std::to_string(k);SCOPED_TRACE(name);
        ASSERT_NO_THROW(startRunoff(b,name,2,2,2));
        auto before=root()/(name+"-before.chk"),after=root()/(name+"-after.chk"),bad=root()/(name+"-bad.chk");
        ASSERT_EQ(b.saveCheckpoint(before.string()),"");auto data=payload;
        auto put=[&](std::size_t at,double v){std::memcpy(data.data()+at,&v,sizeof(v));};
        if(k==0)put(runoff_at+sizeof(uint64_t),-1);
        if(k==1)put(runoff_at+sizeof(uint64_t),std::numeric_limits<double>::quiet_NaN());
        if(k==2) {uint64_t wrong=4;std::memcpy(data.data()+runoff_at,&wrong,sizeof(wrong));}
        if(k==3)put(runoff_at+ga,123); // GA material
        if(k==4)put(runoff_at+ga+3*sizeof(double),2); // GA moisture deficit
        if(k==5) {std::size_t at=sizeof(uint64_t);for(int i=0;i<6;++i)skip(units,at);skip(units,at,sizeof(int));skip(units,at);skip(units,at);put(lid_at+at+sizeof(uint64_t),-1);}
        if(k==6)put(sub_at+sizeof(uint64_t),-1);
        if(k==7) {
            detail::CompletedSourceArchive cursors;auto cur=a.context().gages.st_cur,next=a.context().gages.st_next;cursors(cur,next);
            auto at=data.find(cursors.bytes(),sub_at);ASSERT_NE(at,std::string::npos);
            int wrong=999;std::memcpy(data.data()+at+sizeof(uint64_t),&wrong,sizeof(wrong));
        }
        if(k==8) {double stored=a.context().mass_balance.runoff_final_store;detail::CompletedSourceArchive value;value(stored);
            auto at=data.find(value.bytes());ASSERT_NE(at,std::string::npos);put(at,stored+1);}
        sum=crc(data);detail::CompletedSourceArchive out;out(magic,version,model,sum,data);write(bad,out.bytes());
        EXPECT_FALSE(b.loadCheckpoint(bad.string()).empty());ASSERT_EQ(b.saveCheckpoint(after.string()),"");EXPECT_EQ(bytes(before),bytes(after));
        ASSERT_EQ(b.loadCheckpoint(good.string()),"");compare(a,b,name+"-retry");
    }
    for(int k=0;k<6;++k) {
        SWMMEngine b;
        ASSERT_NO_THROW(startRunoff(b,"runoff-profile-"+std::to_string(k),0,0,0,false));
        if(k==0)b.context().options.ignore_snow_melt=false;
        if(k==1)b.context().options.ignore_groundwater=false;
        if(k==2)b.context().gages.source[0]=RainSource::FILE_RAIN;
        if(k==3)b.context().gages.api_rainfall[0]=1;
        if(k==4)b.context().lid_usage.rpt_file[0]="detailed.txt";
        if(k==5)b.context().subcatches.gw_aquifer[0]=0;
        EXPECT_FALSE(b.enableCheckpointing().empty());
    }
    SWMMEngine changed;
        ASSERT_NO_THROW(startRunoff(changed,"runoff-model-change",2,2,2));
    changed.context().subcatches.infil_p1[0]+=1;
    EXPECT_FALSE(changed.loadCheckpoint(good.string()).empty());
    EXPECT_FALSE(changed.saveCheckpoint((root()/"runoff-changed.chk").string()).empty());
}


TEST(EngineCheckpoint, RunoffGapDryRecessionAndIgnoredRainRestore) {
    for(int method=0;method<5;++method)for(int phase=0;phase<3;++phase) {
        auto name="runoff-phase-"+std::to_string(method)+"-"+std::to_string(phase);SCOPED_TRACE(name);
        SWMMEngine a,b;
        ASSERT_NO_THROW(startRunoff(a,name+"-a",method,2,2,false));
        ASSERT_NO_THROW(startRunoff(b,name+"-b",method,2,2,false));
        if(phase==2){a.context().options.ignore_rainfall=true;b.context().options.ignore_rainfall=true;}
        ASSERT_EQ(a.enableCheckpointing(),"");ASSERT_EQ(b.enableCheckpointing(),"");
        while(a.context().current_time<(phase==0 ? 5 : 150))ASSERT_NO_THROW(step(a));
        EXPECT_EQ(a.context().subcatches.rainfall[0],0);
        auto p=root()/(name+".chk");ASSERT_EQ(a.saveCheckpoint(p.string()),"");ASSERT_EQ(b.loadCheckpoint(p.string()),"");
        while(a.context().elapsed_ms<a.context().options.totalDurationMs()) {
            ASSERT_NO_THROW(step(a));
            ASSERT_NO_THROW(step(b));
        }
        compare(a,b,name);
        if(phase==2)EXPECT_EQ(a.context().mass_balance.runoff_rainfall,0);
    }
}

#ifdef OPENSWMM_HAS_2D
namespace {
struct SurfaceSession {
    // The qualification source objects outlive the engine's non-owning attachment.
    runoff::SourceWaterDriver sources;
    twoD::CommonSourceStage stage;
    SWMMEngine engine;
    void start(const std::string& name,int lid=0,int mapped=0,int closure=0,int mode=0,bool track=true,bool spatial=false,bool all_sources=false,int outfall_return=-1,bool output=false,int mesh_output=0,bool average=false,const std::string& runtime_identity={}) {
        SWMMEngine authored;startRunoff(authored,name,mode%5,mode%3,lid,false);authored.close();
        auto p=root()/name;auto deck=bytes(p.string()+".inp");
        deck.replace(deck.find("IGNORE_2D YES"),13,"IGNORE_2D NO");
        // Surface advances on every routing step; routing-event skips are outside this profile.
        if(mesh_output) {
            std::filesystem::remove(p.string()+".h5");
            deck+="[2D_OPTIONS]\nOUTPUT_FILE "+p.string()+".h5\nOUTPUT_PRECISION "+(mesh_output==2?std::string("FLOAT32"):std::string("FLOAT64"))+"\nOUTPUT_COMPRESSION "+(mesh_output==2?std::string("4"):std::string("0"))+"\nREPORT_2D_STEP 00:00:06\n";
            if(mesh_output==3)deck+="REPORT_2D_VARIABLES MINIMAL\n";
        if(mesh_output==4)deck+="[OPTIONS]\nREPORT_START_DATE 10/10/2026\nREPORT_START_TIME 00:00:13\n";
        }
        write(p.string()+".inp",deck);
        auto& e=engine;
        if(output)std::filesystem::remove(p.string()+".out");
        if(e.open((p.string()+".inp").c_str(),(p.string()+".rpt").c_str(),output?(p.string()+".out").c_str():nullptr))throw std::runtime_error(e.context().error_message);
        auto& ctx=e.context();ctx.options.num_threads=1;ctx.options.surcharge_method=mode%4;
        ctx.events.clear();ctx.gages.scale_factor[1]=1.3;ctx.node_subtypes.outfalls.route_to[0]=2;
        auto& router=e.surfaceRouter2D();auto& mesh=router.mesh();
        mesh.resize_vertices(4);mesh.resize_triangles(2);
        mesh.vx={0,4,0,4};mesh.vy={0,0,2,2};mesh.vz={.5,.5,.5,.5};
        mesh.set_triangle(0,0,1,2);mesh.set_triangle(1,1,3,2);mesh.mannings_n={.03,.03};
        mesh.tri_init_depth={.04,.003};mesh.tri_coupled_node=mapped==1?std::vector<int>{2,0}:(mapped==2?std::vector<int>{2,2}:(mapped==3?std::vector<int>{ctx.node_names.find("O"),0}:std::vector<int>{0,0}));
        mesh.tri_coupling_cd={.6,.6};mesh.tri_coupling_area={.002,.003};
        auto& options=router.options();options.mesh_units_si=true;options.num_threads=1;
        options.momentum=twoD::Momentum2D::LOCAL_INERTIAL;options.max_timestep=.21;
        options.coupling_sync=3.5;options.evaporation=2;options.rainfall_mode=twoD::RainfallMode::SYSTEM;
        auto& config=router.aquiferConfig();config.options.authored=true;config.options.force_closed_form=true;
        config.options.gw_et="BOUNDARY_ET";config.options.closure=closure==2?twoD::GwClosure::SIGMA:closure?twoD::GwClosure::ENSLAVED:twoD::GwClosure::CLOSED_FORM;
        twoD::GwAquiferRow row;row.Ks=.000001*3600/.0254;row.zs=2/.3048;row.hg0=1/.3048;row.alpha=2*.3048;config.rows.push_back(row);
        ctx.options.rpt_averages=average;
        if(e.initialize())throw std::runtime_error("surface initialize: "+ctx.error_message);
        if(output && mesh_output) {auto error=e.prepareCombinedOutputCheckpointing();if(!error.empty())throw std::runtime_error(error);}
        else if(output) {auto error=e.prepareOutputCheckpointing();if(!error.empty())throw std::runtime_error(error);}
        else if(mesh_output) {auto error=e.prepareSurfaceOutputCheckpointing();if(!error.empty())throw std::runtime_error(error);}
        if(e.start(output?1:0))throw std::runtime_error("surface start: "+ctx.error_message);
        auto& gw=router.subsurface().state();
        for(int i=0;i<2 && closure<2;++i) {
            if(closure)gw.hg[i]=gw.zs[i]-.0001;
            gw.hu[i]=gw.theta_s[i]*(gw.zs[i]-gw.hg[i])-.0001/mesh.tri_area[i];
        }
        if(all_sources)ctx.node_subtypes.outfalls.route_to[0]=-1;
        if(outfall_return>=0)std::fill(ctx.node_subtypes.outfalls.route_to.begin(),ctx.node_subtypes.outfalls.route_to.end(),outfall_return);
        auto error=sources.initialize(ctx,spatial ? (all_sources?std::vector<int>{0,2}:std::vector<int>{0}):std::vector<int>{},{},nullptr,outfall_return>=0);if(!error.empty())throw std::runtime_error(error);
        twoD::SurfaceOwnershipPreview preview;preview.mesh_weather_area=mesh.tri_area;
        if(spatial) {
            constexpr double ft2=.3048*.3048;
            for(const auto& group:sources.clocks().groups())for(int sc:group.subcatches) {
                const auto& r=sources.runoff().soa();const double lid_area=sources.context().subcatches.total_lid_area_ft2[sc]*ft2;
                const double full=r.area[sc]*ft2+lid_area;const double inside=.5;const int cell=sc%2;
                twoD::SurfaceOwnerObject object;object.subcatch=sc;object.reviewed=true;object.declared_area=full;
                object.inside_area=inside;object.outside_area=full-inside;preview.objects.push_back(object);
                twoD::SurfaceOwnerShare share;share.subcatch=sc;share.cell=cell;share.weather_area=inside;
                share.pervious_area=inside*r.area[sc]*(1-r.imperv_pct[sc])*ft2/full;
                share.impervious_area=inside*r.area[sc]*r.imperv_pct[sc]*ft2/full;share.lid_area=inside*lid_area/full;
                preview.shares.push_back(share);preview.mesh_weather_area[cell]-=inside;
            }
        }
        error=stage.initialize(mesh,router.state(),options,router.subsurface(),sources,preview,!spatial,runtime_identity);
        if(!error.empty())throw std::runtime_error(error);
        if(track) {error=spatial?e.enableSpatialSourceCheckpointing(stage):e.enableCheckpointing(stage);if(!error.empty())throw std::runtime_error(error);}
    }
};
}

TEST(EngineCheckpoint, SurfaceSessionRestoresActualEngineRainfallPidLidAndPendingBatches) {
    for(int lid=0;lid<4;++lid)for(int mapped=0;mapped<3;++mapped)for(int closure=0;closure<2;++closure) {
        auto name="surface-session-"+std::to_string(lid)+"-"+std::to_string(mapped)+"-"+std::to_string(closure);SCOPED_TRACE(name);
        SurfaceSession a,b;
        ASSERT_NO_THROW(a.start(name+"-a",lid,mapped,closure,mapped));
        ASSERT_NO_THROW(b.start(name+"-b",lid,mapped,closure,mapped));
        while(a.engine.context().current_time<2.1)ASSERT_NO_THROW(step(a.engine));
        if(closure)a.engine.surfaceRouter2D().subsurface().bookLinkSeepage(0,.01); // Accepted recharge, not a live provider.
        while(a.engine.context().current_time<43.1)ASSERT_NO_THROW(step(a.engine));
        const auto file=root()/(name+".chk");ASSERT_EQ(a.engine.saveCheckpoint(file.string()),"") << "surface stores=" << a.engine.context().mass_balance_2d.init_storage << "," << a.engine.context().mass_balance_2d.final_storage << " rain=" << a.engine.context().mass_balance_2d.rainfall_in << " ET=" << a.engine.context().mass_balance_2d.evap_out << " infil=" << a.engine.context().mass_balance_2d.infil_out << " return=" << a.engine.context().mass_balance_2d.aquifer_in;
        ASSERT_GT(a.stage.intervals(),0);EXPECT_LT(a.stage.completedEnd(),a.engine.context().current_time);
        ASSERT_EQ(b.engine.loadCheckpoint(file.string()),"");compare(a.engine,b.engine,name+"-restored");
        int count=0;
        while(a.engine.context().elapsed_ms<a.engine.context().options.totalDurationMs()) {
            ASSERT_NO_THROW(step(a.engine));
            ASSERT_NO_THROW(step(b.engine));
            ASSERT_EQ(a.engine.context().current_time,b.engine.context().current_time);
            if(++count%17==0)compare(a.engine,b.engine,name+"-continued");
        }
        compare(a.engine,b.engine,name+"-final");
        EXPECT_GT(a.stage.meshRain(),0);EXPECT_GT(a.stage.meshEvaporation(),0);
        EXPECT_GT(a.engine.context().mass_balance.runoff_rainfall,0);
        EXPECT_EQ(a.stage.meshReturn(),b.stage.meshReturn());
        if(closure)EXPECT_GT(a.stage.meshReturn(),0);
    }
}

TEST(EngineCheckpoint, SurfaceAttachmentGuardsAreRetryableAtInitialBoundary) {
    SurfaceSession a,b;
    ASSERT_NO_THROW(a.start("surface-guard-a",2,1,0,0,false));
    ASSERT_NO_THROW(b.start("surface-guard-b",2,1,0,0,false));
    EXPECT_FALSE(a.engine.enableCheckpointing().empty());
    EXPECT_FALSE(a.engine.enableCheckpointing(b.stage).empty());
    auto& ctx=a.engine.context();ctx.events={{ctx.options.start_date,datetime::addSeconds(ctx.options.start_date,50)}};
    EXPECT_FALSE(a.engine.enableCheckpointing(a.stage).empty());ctx.events.clear();
    a.engine.surfaceRouter2D().options().output_file="not-opened.h5";
    EXPECT_FALSE(a.engine.enableCheckpointing(a.stage).empty());
    a.engine.surfaceRouter2D().options().output_file="";
    ASSERT_EQ(a.engine.enableCheckpointing(a.stage),"");
    ASSERT_EQ(b.engine.enableCheckpointing(b.stage),"");compare(a.engine,b.engine,"surface-initial");
    EXPECT_FALSE(a.engine.enableCheckpointing(a.stage).empty());
    ASSERT_NO_THROW(step(a.engine));
}

namespace {
std::pair<std::size_t,std::string> nested(const std::string& payload,const std::string& magic) {
    auto at=payload.find(magic);
    if(at==std::string::npos || at<16)throw std::runtime_error("Missing nested checkpoint record");
    at-=16;uint64_t size;std::memcpy(&size,payload.data()+at,8);
    if(size>payload.size()-at-8)throw std::runtime_error("Invalid nested checkpoint span");
    return {at,payload.substr(at+8,size)};
}
std::string replaceNested(std::string payload,const std::string& magic,const std::string& replacement) {
    auto [at,old]=nested(payload,magic);detail::CompletedSourceArchive record;auto copy=replacement;record(copy);
    payload.replace(at,old.size()+8,record.bytes());return payload;
}
std::string engineSurface(const std::string& bytes) {
    detail::CompletedSourceArchive file(bytes);std::string magic,model,payload;uint32_t version,sum;
    file(magic,version,model,sum,payload);file.finish();return nested(payload,"OpenSWMM joint DW surface/source boundary").second;
}
std::string surfacePayload(const std::string& bytes) {
    detail::CompletedSourceArchive file(bytes);std::string magic,model,payload;uint32_t version,endian,longBytes,sum;
    file(magic,version,endian,longBytes,model,payload,sum);file.finish();return payload;
}
std::string withSurfacePayload(const std::string& bytes,std::string payload) {
    detail::CompletedSourceArchive file(bytes);std::string magic,model,old;uint32_t version,endian,longBytes,sum;
    file(magic,version,endian,longBytes,model,old,sum);file.finish();sum=crc(payload);
    detail::CompletedSourceArchive out;out(magic,version,endian,longBytes,model,payload,sum);return out.bytes();
}
}
TEST(EngineCheckpoint, SurfaceNestedFailuresPreserveEveryComponentAndAllowValidRetry) {
    SurfaceSession a;
        ASSERT_NO_THROW(a.start("surface-atomic-source",2,1));
    while(a.engine.context().current_time<43.1)ASSERT_NO_THROW(step(a.engine));
    auto good=root()/"surface-atomic-good.chk";ASSERT_EQ(a.engine.saveCheckpoint(good.string()),"");
    const double savedStorage=a.engine.context().mass_balance.routing_final_storage;
    auto valid=bytes(good),surface=engineSurface(valid),data=surfacePayload(surface);
    detail::CompletedSourceArchive in(valid);std::string magic,model,payload;uint32_t version,sum;
    in(magic,version,model,sum,payload);in.finish();ASSERT_EQ(version,4);
    a.engine.context().gages.rainfall[0]+=1;
    auto rain=root()/"surface-other-rain.chk";ASSERT_EQ(a.engine.saveCheckpoint(rain.string()),"");
    auto otherRain=engineSurface(bytes(rain));a.engine.context().gages.rainfall[0]-=1;
    for(int i=0;i<4;++i)ASSERT_NO_THROW(step(a.engine));
    auto later=root()/"surface-other-time.chk";ASSERT_EQ(a.engine.saveCheckpoint(later.string()),"");
    auto otherSurface=engineSurface(bytes(later)),otherData=surfacePayload(otherSurface);
    for(int k=0;k<9;++k) {
        SCOPED_TRACE(k);SurfaceSession b;
        ASSERT_NO_THROW(b.start("surface-mutant-"+std::to_string(k),2,1));
        auto before=root()/"surface-atomic-before.chk",after=root()/"surface-atomic-after.chk",bad=root()/"surface-atomic-bad.chk";
        ASSERT_EQ(b.engine.saveCheckpoint(before.string()),"");
        auto changed=surface,body=payload;uint32_t v=version;
        if(k==0)v=3;
        if(k==1)changed=otherSurface; // Individually valid, mismatched engine time.
        if(k==2)changed=otherRain; // Individually valid, mismatched shared gage rainfall.
        if(k>=3 && k<=6) {
            const std::string tag=k%2?"OpenSWMM CPU surface/source boundary":"OpenSWMM accepted DW hydraulic boundary";
            auto replacement=nested(k<=4?data:otherData,tag).second;
            if(k<=4)replacement.back()^=1; // Outer checksums remain valid.
            changed=withSurfacePayload(surface,replaceNested(data,tag,replacement));
        }
        if(k==7) {
            const auto offset=7*sizeof(double)+6*sizeof(double)+2*sizeof(int)+4*sizeof(bool)+2*sizeof(double)+11*sizeof(double)+sizeof(uint64_t)+a.engine.context().n_nodes()*sizeof(double)+23*sizeof(double);
            double stored;std::memcpy(&stored,body.data()+offset,sizeof(stored));
            ASSERT_EQ(stored,savedStorage);stored+=1;std::memcpy(body.data()+offset,&stored,sizeof(stored));
        }
        body=replaceNested(body,"OpenSWMM joint DW surface/source boundary",changed);sum=crc(body);
        detail::CompletedSourceArchive out;out(magic,v,model,sum,body);auto mutant=out.bytes();if(k==8)mutant.resize(mutant.size()/2);
        write(bad,mutant);EXPECT_FALSE(b.engine.loadCheckpoint(bad.string()).empty());
        ASSERT_EQ(b.engine.saveCheckpoint(after.string()),"");EXPECT_EQ(bytes(before),bytes(after));
        EXPECT_EQ(b.stage.completedEnd(),0);EXPECT_EQ(b.stage.intervals(),0);
        ASSERT_EQ(b.engine.loadCheckpoint(good.string()),"");
        // Compare against the saved boundary, before the source was advanced again.
        ASSERT_EQ(b.engine.saveCheckpoint(after.string()),"");EXPECT_EQ(bytes(good),bytes(after));
    }
}
TEST(EngineCheckpoint, SurfaceSessionRejectsComponentOnlyRestoreAndUnsupportedDonorAttachment) {
    twoD::SourceResultHistory history;SurfaceSession a;
    ASSERT_NO_THROW(a.start("surface-component-guard",2,2,0,0,false));
    runoff::SourceWaterDriver donors;ASSERT_EQ(donors.initialize(a.engine.context(),{0}),"");
    twoD::CommonSourceStage rejected;twoD::SurfaceOwnershipPreview preview;auto& r=a.engine.surfaceRouter2D();
    preview.mesh_weather_area=r.mesh().tri_area;
    EXPECT_FALSE(rejected.initialize(r.mesh(),r.state(),r.options(),r.subsurface(),donors,preview,true).empty());
    EXPECT_FALSE(a.engine.enableCheckpointing(rejected).empty());
    ASSERT_EQ(a.engine.enableCheckpointing(a.stage),"");
    auto file=root()/"surface-component-guard.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(file.string()),"") << "surface stores=" << a.engine.context().mass_balance_2d.init_storage << "," << a.engine.context().mass_balance_2d.final_storage << " rain=" << a.engine.context().mass_balance_2d.rainfall_in << " ET=" << a.engine.context().mass_balance_2d.evap_out << " infil=" << a.engine.context().mass_balance_2d.infil_out << " return=" << a.engine.context().mass_balance_2d.aquifer_in;auto original=bytes(file);
    EXPECT_FALSE(r.saveCheckpoint(a.engine.context(),file.string()).empty());
    EXPECT_FALSE(r.loadCheckpoint(a.engine.context(),file.string()).empty());
    EXPECT_EQ(bytes(file),original);
    // Private, unadvanced catchments have no qualified source-result endpoints.
    EXPECT_THROW(r.setSourceResultHistory(history),std::invalid_argument);
    ASSERT_EQ(a.engine.saveCheckpoint(file.string()),"");
    EXPECT_EQ(bytes(file),original);
}
#endif

#ifdef OPENSWMM_HAS_2D
TEST(EngineCheckpoint, SpatialSourcesOwnGraphsDeliverWetWeatherAndContinueVariableEngineSteps) {
    for(int method=0;method<5;++method)for(int rain=0;rain<3;++rain)for(int lid=0;lid<4;++lid) {
        const int mode=method+5*rain;auto name="spatial-"+std::to_string(mode)+"-"+std::to_string(lid);SCOPED_TRACE(name);
        SurfaceSession a,b;
        ASSERT_NO_THROW(a.start(name+"-a",lid,rain%3,method%2,mode,true,true));
        ASSERT_NO_THROW(b.start(name+"-b",lid,rain%3,method%2,mode,true,true));
        const auto* bridge=a.engine.sourceDelivery();ASSERT_NE(bridge,nullptr);
        EXPECT_TRUE(bridge->ownsSubcatch(0));EXPECT_TRUE(bridge->ownsSubcatch(1));EXPECT_FALSE(bridge->ownsSubcatch(2));
        const auto initial=a.engine.lid().waterTrial();
        while(a.engine.context().current_time<2.1)ASSERT_NO_THROW(step(a.engine));
        const auto file=root()/(name+".chk");ASSERT_EQ(a.engine.saveCheckpoint(file.string()),"");
        ASSERT_EQ(b.engine.loadCheckpoint(file.string()),"");compare(a.engine,b.engine,name+"-restored");
        double min_dt=1e9,max_dt=0;int steps=0;bool runoff=false,drain=false,returning=false;
        while(a.engine.context().elapsed_ms<a.engine.context().options.totalDurationMs()) {
            const double before=a.engine.context().current_time;
            ASSERT_NO_THROW(step(a.engine));
            ASSERT_NO_THROW(step(b.engine));
            const double dt=a.engine.context().current_time-before;min_dt=std::min(min_dt,dt);max_dt=std::max(max_dt,dt);
            const auto& c=a.engine.context();
            for(int sc:{0,1}) {
                EXPECT_DOUBLE_EQ(c.subcatches.runoff[sc],0);EXPECT_DOUBLE_EQ(c.subcatches.rainfall[sc],0);
                EXPECT_DOUBLE_EQ(c.subcatches.infil_loss[sc],0);EXPECT_DOUBLE_EQ(c.subcatches.lid_drain_flow[sc],0);
            }
            EXPECT_NEAR(a.sources.balanceResidual(0),0,1e-6);
            for(const auto& d:bridge->delivered()) {runoff|=d.runoff>0;drain|=d.drain>0;}
            for(int sc:{0,1})returning|=a.sources.ledgers()[sc].aquifer_return>0;
            if(++steps%41==0)compare(a.engine,b.engine,name+"-continued");
        }
        EXPECT_LT(min_dt,max_dt);EXPECT_TRUE(runoff);EXPECT_TRUE(drain);
        EXPECT_GT(a.engine.context().subcatches.stat_precip_vol[2],0);
        EXPECT_GT(a.engine.context().mass_balance.runoff_rainfall,0);
        EXPECT_GT(a.engine.context().mass_balance.routing_wet_weather,0);
        EXPECT_EQ(a.sources.deliveries().size(),bridge->capturedCount());
        compare(a.engine,b.engine,name+"-final");
        EXPECT_EQ(a.sources.context().subcatches.runoff,b.sources.context().subcatches.runoff);
        if(method%2)EXPECT_TRUE(returning);
        for(const auto& [t,u]:initial.usageOrder())if(bridge->ownsSubcatch(initial.group(t).subcatch_idx[u])) {
            const auto& g=a.engine.lid().group(t);const auto& original=initial.group(t);
            EXPECT_DOUBLE_EQ(g.surf_depth[u],original.surf_depth[u]);EXPECT_DOUBLE_EQ(g.soil_moist[u],original.soil_moist[u]);
            EXPECT_DOUBLE_EQ(g.stor_depth[u],original.stor_depth[u]);EXPECT_DOUBLE_EQ(g.drain_flow[u],original.drain_flow[u]);
            EXPECT_DOUBLE_EQ(g.wb_inflow[u],original.wb_inflow[u]);EXPECT_DOUBLE_EQ(g.wb_infil[u],original.wb_infil[u]);
        }
        a.engine.end();b.engine.end();
    }
}
TEST(EngineCheckpoint, SpatialAttachmentIsExplicitAndRefusesOwnedOutfallReturnsAndChangedSourceMaterials) {
    SurfaceSession a;
        ASSERT_NO_THROW(a.start("spatial-guard",2,0,0,0,false,true));
    EXPECT_FALSE(a.engine.enableCheckpointing(a.stage).empty());EXPECT_EQ(a.engine.sourceDelivery(),nullptr);
    a.engine.context().node_subtypes.outfalls.route_to[0]=0;
    EXPECT_FALSE(a.engine.enableSpatialSourceCheckpointing(a.stage).empty());EXPECT_EQ(a.engine.sourceDelivery(),nullptr);
    a.engine.context().node_subtypes.outfalls.route_to[0]=2;
    ASSERT_EQ(a.engine.enableSpatialSourceCheckpointing(a.stage),"");
    EXPECT_FALSE(a.engine.enableSpatialSourceCheckpointing(a.stage).empty());
    SurfaceSession b;
        ASSERT_NO_THROW(b.start("spatial-material",2,0,0,0,false,true));
    b.engine.context().subcatches.width[0]+=1;
    EXPECT_FALSE(b.engine.enableSpatialSourceCheckpointing(b.stage).empty());EXPECT_EQ(b.engine.sourceDelivery(),nullptr);
    b.engine.context().subcatches.width[0]-=1;ASSERT_EQ(b.engine.enableSpatialSourceCheckpointing(b.stage),"");
    SurfaceSession c;
        ASSERT_NO_THROW(c.start("spatial-all-sources",2,0,0,0,false,true,true));
    c.engine.context().node_subtypes.outfalls.route_to[0]=2;
    EXPECT_FALSE(c.engine.enableSpatialSourceCheckpointing(c.stage).empty());
    c.engine.context().node_subtypes.outfalls.route_to[0]=-1;
    ASSERT_EQ(c.engine.enableSpatialSourceCheckpointing(c.stage),"");
    auto invalid_lid=c.engine.lid().waterTrial();auto& g=invalid_lid.group(int(lid::LIDType::BIO_CELL));ASSERT_GT(g.count,0);g.surf_void_frac[0]=0;
    runoff::SourceWaterDriver refused;EXPECT_FALSE(refused.initialize(c.engine.context(),{0},{},&invalid_lid).empty());
    ASSERT_EQ(refused.initialize(c.engine.context(),{0}),"");
    ASSERT_NO_THROW(step(c.engine));
    EXPECT_FALSE(c.engine.saveCheckpoint((root()/"spatial-invalid-mode.chk").string()).size());
    ASSERT_EQ(c.engine.close(),0);EXPECT_EQ(c.engine.sourceDelivery(),nullptr);
}
#endif

#ifdef OPENSWMM_HAS_2D
namespace {
void startClosedSpatial(SurfaceSession& m,const std::string& name,int node_lid=0,double cadence=.7,bool returns=false,bool multiple=false,bool output=false,int mesh_output=0,bool average=false) {
    auto path=root()/name;
    std::string deck="[OPTIONS]\nFLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\nSTART_DATE 10/10/2026\nSTART_TIME 00:00:00\nEND_DATE 10/10/2026\nEND_TIME 00:00:30\nROUTING_STEP "+std::to_string(cadence)+"\nVARIABLE_STEP 0\nINFILTRATION GREEN_AMPT\nRULE_STEP 00:00:02\nREPORT_STEP 00:00:03\nWET_STEP 00:00:01\nDRY_STEP 00:00:07\nIGNORE_QUALITY YES\nIGNORE_SNOWMELT YES\nIGNORE_GROUNDWATER YES\nIGNORE_2D NO\n[STORAGE]\nSINK 0 100 0 FUNCTIONAL 0 0 100 0 0\n[OUTFALLS]\nDRAIN 0 FREE NO\n[RAINGAGES]\nR INTENSITY 0:00:05 1 TIMESERIES Rain\nDry INTENSITY 0:00:05 1 TIMESERIES Zero\n[SUBCATCHMENTS]\nA R B .01 45 10 1 0\nB R SINK .02 65 10 1 0\nD Dry SINK .01 50 10 1 0\n[SUBAREAS]\nA .015 .12 .03 .05 35 PERVIOUS 55\nB .02 .13 .02 .04 25 OUTLET\nD .02 .13 .02 .04 25 OUTLET\n[INFILTRATION]\nA 3 .25 .3\nB 3 .25 .3\nD 3 .25 .3\n[LID_CONTROLS]\nI IT\nI SURFACE 3 .05 .12 1 3\nI STORAGE 6 .667 .1 0\nI DRAIN .5 .5 0 0\n[LID_USAGE]\nA I 1 20 3 70 30 0 * DRAIN\n[EVAPORATION]\nCONSTANT 0\n[TIMESERIES]\nRain 10/10/2026 00:00:00 5\nRain 10/10/2026 00:00:05 0\nZero 10/10/2026 00:00:00 0\n";
    if(node_lid) {
        deck.replace(deck.find("SINK 0 100 0"),12,"SINK 0 2 0");
        deck+="[LID_CONTROLS]\nStack NODE\nStack SURFACE 6 .1\nStack MEDIA 12 .45 .2 .08 2 10 3\nStack AGGREGATE 6 .4 100\nStack BOTTOM 0 0\n[LID_NODES]\nSINK Stack 10\n";
        if(node_lid==2)deck+="[LID_RICHARDS]\nStack OPTIONS 4 1e-7 1e-5 30\nStack 2 .03 2 1.6 .5 .0001\nStack 3 .03 2 1.6 .5 .0001\n";
    }
    if(multiple)deck+="[OUTFALLS]\nDRAIN2 0 FREE NO\n[LID_USAGE]\nB I 1 10 3 70 0 0 * DRAIN2\n";
    if(mesh_output) {
        std::filesystem::remove(path.string()+".h5");
        deck+="[2D_OPTIONS]\nOUTPUT_FILE "+path.string()+".h5\nOUTPUT_PRECISION "+(mesh_output==2?std::string("FLOAT32"):std::string("FLOAT64"))+"\nOUTPUT_COMPRESSION "+(mesh_output==2?std::string("4"):std::string("0"))+"\nREPORT_2D_STEP 00:00:06\n";
        if(mesh_output==3)deck+="REPORT_2D_VARIABLES MINIMAL\n";
        if(mesh_output==4)deck+="[OPTIONS]\nREPORT_START_DATE 10/10/2026\nREPORT_START_TIME 00:00:13\n";
    }
    write(path.string()+".inp",deck);auto& e=m.engine;
    if(output)std::filesystem::remove(path.string()+".out");
    if(e.open((path.string()+".inp").c_str(),(path.string()+".rpt").c_str(),output?(path.string()+".out").c_str():nullptr))throw std::runtime_error(e.context().error_message);
    auto& router=e.surfaceRouter2D();auto& mesh=router.mesh();mesh.resize_vertices(4);mesh.resize_triangles(2);
    mesh.vx={0,2,0,2};mesh.vy={0,0,2,2};mesh.vz={0,0,0,0};mesh.set_triangle(0,0,1,2);mesh.set_triangle(1,1,3,2);
    mesh.mannings_n={.03,.03};mesh.tri_init_depth={0,0};mesh.tri_coupled_node={e.context().node_names.find("SINK"),-1};
    // Retain a joint coupling identity while closing hydraulic exchange.
    mesh.tri_coupling_cd={0,0};mesh.tri_coupling_area={1,1};
    auto& options=router.options();options.mesh_units_si=true;options.num_threads=1;options.max_timestep=.21;
    options.coupling_sync=3.5;options.evaporation=2;options.rainfall_mode=twoD::RainfallMode::SYSTEM;
    auto& config=router.aquiferConfig();config.options.authored=true;config.options.force_closed_form=true;
    config.options.closure=twoD::GwClosure::CLOSED_FORM;
    twoD::GwAquiferRow row;row.Ks=.000001*3600/.0254;row.zs=2/.3048;row.hg0=1/.3048;row.alpha=2*.3048;config.rows.push_back(row);
    e.context().options.rpt_averages=average;
    if(e.initialize())throw std::runtime_error(e.context().error_message);
    if(output && mesh_output) {auto error=e.prepareCombinedOutputCheckpointing();if(!error.empty())throw std::runtime_error(error);}
    else if(output) {auto error=e.prepareOutputCheckpointing();if(!error.empty())throw std::runtime_error(error);}
    else if(mesh_output) {auto error=e.prepareSurfaceOutputCheckpointing();if(!error.empty())throw std::runtime_error(error);}
    if(e.start(output?1:0))throw std::runtime_error(e.context().error_message);
    if(returns)std::fill(e.context().node_subtypes.outfalls.route_to.begin(),e.context().node_subtypes.outfalls.route_to.end(),0);
    auto error=m.sources.initialize(e.context(),{0},{},nullptr,returns);if(!error.empty())throw std::runtime_error(error);
    twoD::SurfaceOwnershipPreview preview;preview.mesh_weather_area=mesh.tri_area;constexpr double ft2=.3048*.3048;
    for(int sc:{0,1}) {
        const auto& r=m.sources.runoff().soa();const double lids=m.sources.context().subcatches.total_lid_area_ft2[sc]*ft2;
        const double full=r.area[sc]*ft2+lids,inside=.5;
        twoD::SurfaceOwnerObject object;object.subcatch=sc;object.reviewed=true;object.declared_area=full;
        object.inside_area=inside;object.outside_area=full-inside;preview.objects.push_back(object);
        twoD::SurfaceOwnerShare share;share.subcatch=sc;share.cell=sc;share.weather_area=inside;
        share.pervious_area=inside*r.area[sc]*(1-r.imperv_pct[sc])*ft2/full;
        share.impervious_area=inside*r.area[sc]*r.imperv_pct[sc]*ft2/full;share.lid_area=inside*lids/full;
        preview.shares.push_back(share);preview.mesh_weather_area[sc]-=inside;
    }
    error=m.stage.initialize(mesh,router.state(),options,router.subsurface(),m.sources,preview);
    if(error.empty())error=e.enableSpatialSourceCheckpointing(m.stage);if(!error.empty())throw std::runtime_error(error);
}
struct SpatialBridgeWire {
    uint32_t version=2;uint64_t count=0,digest=0,last_begin=0;double end=0,last_start=0;
    std::vector<double> clocks;std::vector<runoff::QueuedSourceDelivery> pending;
    std::vector<runoff::SourceNodeDelivery> received,delivered,last;
    explicit SpatialBridgeWire(const runoff::SourceWaterDeliveryBridge& b,std::size_t groups) :
        clocks(groups),received(b.received().size()),delivered(received),last(received) {}
    void io(detail::CompletedSourceArchive& a) {
        a(version,count,digest,end);a.fixed(clocks);
        a.sequence(pending,[&](auto& q){auto& d=q.water;a(q.ordinal,q.available,d.source,d.node,d.start,d.end,d.volume,d.drain);});
        a.sequence(received,[&](auto& d){a(d.runoff,d.drain);},true);
        a.sequence(delivered,[&](auto& d){a(d.runoff,d.drain);},true);
        a(last_begin,last_start);a.sequence(last,[&](auto& d){a(d.runoff,d.drain,d.rate);},true);
    }
};
}
TEST(EngineCheckpoint, SpatialReceiptsPreserveVolumesAtEveryClosedReceiverStepAndCadence) {
    for(int lid=0;lid<3;++lid)for(double cadence:{.7,.31,1.}) {
        const auto name="spatial-closed-"+std::to_string(lid)+"-"+std::to_string(cadence);SCOPED_TRACE(name);
        SurfaceSession m;ASSERT_NO_THROW(startClosedSpatial(m,name,lid,cadence));
        const auto& c=m.engine.context();const double initial=c.mass_balance.routing_init_storage;
        const auto initial_inflow=c.nodes.stat_total_inflow_vol;
        double previous=0;int steps=0;bool pending=false;
        while(c.elapsed_ms<c.options.totalDurationMs()) {
            ASSERT_NO_THROW(step(m.engine));const auto* b=m.engine.sourceDelivery();
            double sink=0,outfall=0,total=0,queued=0;
            for(int n=0;n<c.n_nodes();++n) {
                const double v=b->delivered()[n].runoff+b->delivered()[n].drain;total+=v;
                if(!lidnode::active(c,n))EXPECT_NEAR(c.nodes.stat_total_inflow_vol[n]-initial_inflow[n],v,1e-9);
                EXPECT_NEAR(c.nodes.stat_lat_inflow_vol[n],v,1e-9);
                if(c.nodes.type[n]==NodeType::OUTFALL) {
                    outfall+=v;EXPECT_NEAR(c.nodes.stat_total_outflow_vol[n],v,1e-9);
                } else sink+=v;
            }
            for(const auto& q:b->pending())queued+=q.water.volume;pending|=queued>0;
            EXPECT_NEAR(c.mass_balance.routing_final_storage-initial,sink,1e-9);
            EXPECT_NEAR(c.mass_balance.routing_outflow,outfall,1e-9);
            EXPECT_NEAR(c.mass_balance.routing_wet_weather,total,1e-9);
            EXPECT_NEAR(c.mass_balance.routing_report[1],total,1e-9);
            EXPECT_DOUBLE_EQ(c.mass_balance.routing_external,0);
            EXPECT_NEAR(c.mass_balance.runoff_runoff+c.mass_balance.runoff_lid_drain,total+queued,1e-9);
            EXPECT_NEAR(m.sources.balanceResidual(0),0,1e-8);
            EXPECT_GE(total,previous);previous=total;
            if(++steps==11) {
                auto path=root()/(name+".chk");ASSERT_EQ(m.engine.saveCheckpoint(path.string()),"");
                SurfaceSession restored;ASSERT_NO_THROW(startClosedSpatial(restored,name+"-restored",lid,cadence));
                ASSERT_EQ(restored.engine.loadCheckpoint(path.string()),"");compare(m.engine,restored.engine,name+"-opening");
                for(int k=0;k<3;++k) {step(m.engine);step(restored.engine);}compare(m.engine,restored.engine,name+"-continued");
            }
        }
        EXPECT_TRUE(pending);EXPECT_GT(previous,0);m.engine.end();
    }
}
TEST(EngineCheckpoint, SpatialBridgeCorruptionRejectsBeforeEveryComponentInstallation) {
    SurfaceSession a;ASSERT_NO_THROW(a.start("spatial-atomic-source",2,2,1,0,true,true));
    while(a.engine.context().current_time<16 || a.engine.sourceDelivery()->pending().empty())ASSERT_NO_THROW(step(a.engine));
    const auto good=root()/"spatial-atomic.chk";ASSERT_EQ(a.engine.saveCheckpoint(good.string()),"");
    detail::CompletedSourceArchive input(bytes(good));std::string magic,model,payload;uint32_t version,sum;
    input(magic,version,model,sum,payload);input.finish();ASSERT_EQ(version,5);
    auto bridge=*a.engine.sourceDelivery();detail::CompletedSourceArchive saved;bridge.checkpoint(saved,false,a.sources);
    auto water=saved.bytes();detail::CompletedSourceArchive encoded;encoded(water);const auto location=payload.find(encoded.bytes());ASSERT_NE(location,std::string::npos);
    SpatialBridgeWire original(bridge,a.sources.clocks().groups().size());detail::CompletedSourceArchive read(water);original.io(read);read.finish();
    ASSERT_FALSE(original.pending.empty());
    std::array<double,6> totals{};
    for(int sc=0;sc<a.engine.context().n_subcatches();++sc)if(bridge.ownsSubcatch(sc)) {
        const auto& b=a.sources.ledgers()[sc];
        totals[0]+=b.rain;totals[1]+=b.evaporation;totals[2]+=b.infiltration;totals[3]+=b.aquifer_return;
    }
    for(const auto& b:bridge.received()) {totals[4]+=b.runoff;totals[5]+=b.drain;}
    detail::CompletedSourceArchive stored_totals;stored_totals(totals);
    const auto totals_at=payload.find(stored_totals.bytes());ASSERT_NE(totals_at,std::string::npos);
    ASSERT_EQ(payload.find(stored_totals.bytes(),totals_at+1),std::string::npos);
    for(int mutation=0;mutation<22;++mutation) {
        SCOPED_TRACE(mutation);auto w=original;
        switch(mutation) {
        case 0:w.version=1;break;
        case 1:++w.count;break;
        case 2:w.digest^=1;break;
        case 3:w.end+=.1;break;
        case 4:w.clocks[0]+=1;break;
        case 5:w.pending[0].available+=1;break;
        case 6:w.pending[0].water.volume+=1;break;
        case 7:w.pending[0].water.node=99;break;
        case 8:w.delivered[0].runoff+=1;break;
        case 9:w.last_start=w.end;break;
        case 10:w.last_begin=w.count+1;break;
        case 11:w.last[0].rate+=1;break;
        case 12:w.last[0].drain=std::numeric_limits<double>::quiet_NaN();break;
        }
        detail::CompletedSourceArchive changed;w.io(changed);auto badwater=changed.bytes();detail::CompletedSourceArchive newfield;newfield(badwater);
        auto bad=payload;bad.replace(location,encoded.bytes().size(),newfield.bytes());
        if(mutation>=13 && mutation<19) {
            auto inconsistent=totals;inconsistent[mutation-13]+=.1;
            detail::CompletedSourceArchive fields;fields(inconsistent);bad.replace(totals_at,stored_totals.bytes().size(),fields.bytes());
        }
        auto checksum=crc(bad);auto format=mutation==19?uint32_t(4):version;
        detail::CompletedSourceArchive out;out(magic,format,model,checksum,bad);auto invalid=out.bytes();
        if(mutation==20)invalid.pop_back();if(mutation==21)invalid.push_back('x');
        auto path=root()/("spatial-bad-"+std::to_string(mutation)+".chk");write(path,invalid);
        SurfaceSession target;ASSERT_NO_THROW(target.start("spatial-atomic-target-"+std::to_string(mutation),2,2,1,0,true,true));
        const auto before=root()/"spatial-target-before.chk";ASSERT_EQ(target.engine.saveCheckpoint(before.string()),"");auto fresh=bytes(before);
        EXPECT_FALSE(target.engine.loadCheckpoint(path.string()).empty());ASSERT_EQ(target.engine.saveCheckpoint(before.string()),"");EXPECT_EQ(bytes(before),fresh);
        ASSERT_EQ(target.engine.loadCheckpoint(good.string()),"");compare(a.engine,target.engine,"spatial-valid-retry-"+std::to_string(mutation));
    }
}
#endif

#ifdef OPENSWMM_HAS_2D
TEST(EngineCheckpoint, SpatialSourceReturnPreservesLowStorageAtShortCadence) {
    for(bool all:{false,true}) {
        SCOPED_TRACE(all);SurfaceSession m;ASSERT_NO_THROW(m.start("spatial-low-storage-"+std::to_string(all),3,1,1,6,true,true,all));
        const auto end=all?m.engine.context().options.totalDurationMs()/1000:100;
        while(m.engine.context().elapsed_ms<end*1000)ASSERT_NO_THROW(step(m.engine));
        for(int group=0;group<int(m.sources.clocks().groups().size());++group)EXPECT_NEAR(m.sources.balanceResidual(group),0,1e-8);
        if(all) {EXPECT_EQ(m.sources.clocks().groups().size(),2);EXPECT_DOUBLE_EQ(m.engine.context().subcatches.stat_precip_vol[2],0);}
    }
}
#endif

#ifdef OPENSWMM_HAS_2D
TEST(EngineCheckpoint, OwnedOutfallReturnsPreserveEveryAcceptedVolumeAndRunoffBalance) {
    for(int lid=0;lid<3;++lid)for(double cadence:{.31,.7,1.}) {
        const auto name="owned-outfall-closed-"+std::to_string(lid)+"-"+std::to_string(cadence);SCOPED_TRACE(name);
        const bool multiple=lid==2 && cadence==.31;
        SurfaceSession a;ASSERT_NO_THROW(startClosedSpatial(a,name,lid,cadence,true,multiple));
        int steps=0;bool applied=false;
        while(a.engine.context().elapsed_ms<a.engine.context().options.totalDurationMs()) {
            ASSERT_NO_THROW(step(a.engine));const auto& c=a.engine.context();const auto* b=a.engine.sourceDelivery();
            double routed=0;for(int n=0;n<c.n_nodes();++n)if(c.nodes.type[n]==NodeType::OUTFALL)
                routed+=b->delivered()[n].runoff+b->delivered()[n].drain;
            EXPECT_NEAR(a.sources.ledgers()[0].outfall_return,routed,1e-10);
            EXPECT_NEAR(c.mass_balance.routing_outflow,routed,1e-10);
            EXPECT_NEAR(c.mass_balance.runoff_runon,routed,1e-10);
            EXPECT_DOUBLE_EQ(c.subcatches.outfall_runon_vol[0],0);
            EXPECT_DOUBLE_EQ(a.sources.outfallReturnEnd(),c.current_time);
            EXPECT_NEAR(a.sources.balanceResidual(0),0,1e-8);
            const auto& mb=c.mass_balance;
            EXPECT_NEAR(mb.runoff_init_store+mb.runoff_rainfall+mb.runoff_runon-mb.runoff_evap-mb.runoff_infil-
                        mb.runoff_runoff-mb.runoff_lid_drain-mb.runoff_final_store,0,1e-8);
            applied|=a.sources.ledgers()[0].runon>0;
            if(++steps==11) {
                auto file=root()/(name+".chk");ASSERT_EQ(a.engine.saveCheckpoint(file.string()),"");
                SurfaceSession restored;ASSERT_NO_THROW(startClosedSpatial(restored,name+"-restored",lid,cadence,true,multiple));
                ASSERT_EQ(restored.engine.loadCheckpoint(file.string()),"");compare(a.engine,restored.engine,name+"-restored");
                for(int k=0;k<3;++k){step(a.engine);step(restored.engine);}compare(a.engine,restored.engine,name+"-continued");
            }
        }
        EXPECT_GT(a.sources.ledgers()[0].outfall_return,0);EXPECT_TRUE(applied);
        if(multiple) {
            const int second=a.engine.context().node_names.find("DRAIN2");ASSERT_GE(second,0);
            EXPECT_GT(a.engine.sourceDelivery()->delivered()[second].drain,0);
        }
    }
}
TEST(EngineCheckpoint, OwnedOutfallReturnsContinueThroughBackwaterVariableStepsAndMultipleGraphs) {
    for(int lid=0;lid<4;++lid)for(int receiver:{0,2}) {
        const auto name="owned-outfall-network-"+std::to_string(lid)+"-"+std::to_string(receiver);SCOPED_TRACE(name);
        SurfaceSession a,b;const bool all=receiver==2;
        ASSERT_NO_THROW(a.start(name+"-a",lid,0,1,6,true,true,all,receiver));
        ASSERT_NO_THROW(b.start(name+"-b",lid,0,1,6,true,true,all,receiver));
        while(a.engine.context().current_time<5)ASSERT_NO_THROW(step(a.engine));
        auto file=root()/(name+".chk");ASSERT_EQ(a.engine.saveCheckpoint(file.string()),"");
        ASSERT_EQ(b.engine.loadCheckpoint(file.string()),"");compare(a.engine,b.engine,name+"-opening");
        int steps=0;double min_dt=1e9,max_dt=0;
        while(a.engine.context().current_time<35) {
            const auto before=a.engine.context().current_time;
            ASSERT_NO_THROW(step(a.engine));
            ASSERT_NO_THROW(step(b.engine));
            const auto& c=a.engine.context();const double dt=c.current_time-before;min_dt=std::min(min_dt,dt);max_dt=std::max(max_dt,dt);
            EXPECT_DOUBLE_EQ(c.subcatches.outfall_runon_vol[receiver],0);
            EXPECT_NEAR(a.sources.ledgers()[receiver].outfall_return,c.mass_balance.routing_outflow,1e-9);
            double credits=0;for(const auto& l:a.sources.ledgers())credits+=l.outfall_return+l.aquifer_return;
            EXPECT_NEAR(credits,c.mass_balance.runoff_runon,1e-9);
            for(std::size_t g=0;g<a.sources.clocks().groups().size();++g)EXPECT_NEAR(a.sources.balanceResidual(int(g)),0,1e-7);
            if(++steps%23==0)compare(a.engine,b.engine,name+"-during");
        }
        EXPECT_LT(min_dt,max_dt);EXPECT_GT(a.sources.ledgers()[receiver].outfall_return,0);
        if(lid==3)EXPECT_GT(a.engine.context().mass_balance.routing_external,0);
        compare(a.engine,b.engine,name+"-final");
    }
}
TEST(EngineCheckpoint, OwnedOutfallReceiptAttachmentBindsTopologyAndRefusesMappedDuplicateDischarge) {
    SurfaceSession a;ASSERT_NO_THROW(a.start("owned-outfall-topology",2,0,0,0,false,true,false,0));
    a.engine.context().node_subtypes.outfalls.route_to[0]=1;
    EXPECT_FALSE(a.engine.enableSpatialSourceCheckpointing(a.stage).empty());EXPECT_EQ(a.engine.sourceDelivery(),nullptr);
    a.engine.context().node_subtypes.outfalls.route_to[0]=0;ASSERT_EQ(a.engine.enableSpatialSourceCheckpointing(a.stage),"");
    SurfaceSession mapped;ASSERT_NO_THROW(mapped.start("owned-outfall-mapped",2,3,0,0,false,true,false,0));
    EXPECT_NE(mapped.engine.enableSpatialSourceCheckpointing(mapped.stage).find("mapped outfall"),std::string::npos);
    EXPECT_EQ(mapped.engine.sourceDelivery(),nullptr);
}
TEST(EngineCheckpoint, OwnedOutfallReceiptCorruptionPreservesAllComponentsAndAllowsValidRetry) {
    SurfaceSession a;ASSERT_NO_THROW(startClosedSpatial(a,"owned-outfall-atomic",2,.31,true));
    while(a.engine.context().current_time<5)ASSERT_NO_THROW(step(a.engine));
    const auto good=root()/"owned-outfall-atomic.chk";ASSERT_EQ(a.engine.saveCheckpoint(good.string()),"");
    auto valid=bytes(good),surface=engineSurface(valid),surface_data=surfacePayload(surface);
    auto cpu=nested(surface_data,"OpenSWMM CPU surface/source boundary").second;
    detail::CompletedSourceArchive ci(cpu);std::string cpu_magic,cpu_data;uint32_t cpu_v,cpu_endian,cpu_long,cpu_sum;
    ci(cpu_magic,cpu_v,cpu_endian,cpu_long,cpu_data,cpu_sum);ci.finish();
    auto common=nested(cpu_data,"OpenSWMM source boundary").second;
    detail::CompletedSourceArchive si(common);std::string source_magic,source_model,source_data;
    uint32_t source_v,source_endian,source_long,source_double,source_sum;
    si(source_magic,source_v,source_endian,source_long,source_double,source_model,source_data,source_sum);si.finish();
    auto end=a.sources.outfallReturnEnd();std::vector<double> volumes;for(const auto& l:a.sources.ledgers())volumes.push_back(l.outfall_return);
    auto queue=a.sources.pendingOutfallReturns();ASSERT_FALSE(queue.empty());
    const auto tuples=[](detail::CompletedSourceArchive& ar,std::vector<runoff::SourceOutfallReturn>& rows) {
        ar.sequence(rows,[&](auto& q){ar(q.source,q.available,q.volume);});
    };
    ASSERT_GT(volumes[0],0);uint32_t profile=1;detail::CompletedSourceArchive fields;fields(profile,end);
    for(auto v:volumes)fields(v);tuples(fields,queue);
    const auto at=source_data.find(fields.bytes());ASSERT_NE(at,std::string::npos);
    ASSERT_EQ(source_data.find(fields.bytes(),at+1),std::string::npos);
    detail::CompletedSourceArchive ei(valid);std::string engine_magic,engine_model,engine_data;uint32_t engine_v,engine_sum;
    ei(engine_magic,engine_v,engine_model,engine_sum,engine_data);ei.finish();
    for(int mutation=0;mutation<13;++mutation) {
        SCOPED_TRACE(mutation);auto changed_end=end;auto changed_volumes=volumes;auto changed_profile=profile;
        if(mutation==0)changed_profile=2;
        if(mutation==1)changed_end+=1;
        if(mutation==2)changed_end-=.1;
        if(mutation==3)changed_volumes[0]=-1;
        if(mutation==4)changed_volumes[0]=std::numeric_limits<double>::quiet_NaN();
        if(mutation==5)changed_volumes[0]+=.1;
        if(mutation==6)changed_volumes[2]=.1; // unowned catchment
        auto changed_queue=queue;
        if(mutation==7)changed_queue[0].available=end+1;
        if(mutation==8)changed_queue[0].available=0;
        if(mutation==9)changed_queue[0].source=2;
        if(mutation==10)changed_queue[0].volume=std::numeric_limits<double>::quiet_NaN();
        if(mutation==11)changed_queue[0].volume=0;
        if(mutation==12)changed_queue[0].volume+=1;
        detail::CompletedSourceArchive replacement;replacement(changed_profile,changed_end);for(auto v:changed_volumes)replacement(v);tuples(replacement,changed_queue);
        auto changed_source=source_data;changed_source.replace(at,fields.bytes().size(),replacement.bytes());
        auto sum=crc(changed_source);detail::CompletedSourceArchive so;
        so(source_magic,source_v,source_endian,source_long,source_double,source_model,changed_source,sum);
        auto changed_cpu=replaceNested(cpu_data,source_magic,so.bytes());sum=crc(changed_cpu);detail::CompletedSourceArchive co;
        co(cpu_magic,cpu_v,cpu_endian,cpu_long,changed_cpu,sum);
        auto changed_surface=withSurfacePayload(surface,replaceNested(surface_data,cpu_magic,co.bytes()));
        auto changed_engine=replaceNested(engine_data,"OpenSWMM joint DW surface/source boundary",changed_surface);sum=crc(changed_engine);
        detail::CompletedSourceArchive eo;eo(engine_magic,engine_v,engine_model,sum,changed_engine);
        const auto bad=root()/("owned-outfall-bad-"+std::to_string(mutation)+".chk");write(bad,eo.bytes());
        SurfaceSession target;ASSERT_NO_THROW(startClosedSpatial(target,"owned-outfall-target-"+std::to_string(mutation),2,.31,true));
        const auto before=root()/"owned-outfall-target.chk";ASSERT_EQ(target.engine.saveCheckpoint(before.string()),"");auto fresh=bytes(before);
        EXPECT_FALSE(target.engine.loadCheckpoint(bad.string()).empty());ASSERT_EQ(target.engine.saveCheckpoint(before.string()),"");EXPECT_EQ(bytes(before),fresh);
        ASSERT_EQ(target.engine.loadCheckpoint(good.string()),"");compare(a.engine,target.engine,"owned-outfall-retry-"+std::to_string(mutation));
    }
}

namespace {
void sameEngineHistory(const twoD::EngineSourceResultHistory& a,const twoD::EngineSourceResultHistory& b) {
    ASSERT_EQ(a.sourceNames(),b.sourceNames());ASSERT_EQ(a.nodeNames(),b.nodeNames());
    ASSERT_EQ(a.outfallReturnsAvailable(),b.outfallReturnsAvailable());ASSERT_EQ(a.simulationStartDate(),b.simulationStartDate());
    ASSERT_EQ(a.detailedSourceIntervalsAvailable(),b.detailedSourceIntervalsAvailable());
    if(a.sourceIntervals()) {
        const auto& x=*a.sourceIntervals();const auto& y=*b.sourceIntervals();
        ASSERT_EQ(x.sourceNames(),y.sourceNames());ASSERT_EQ(x.pollutantNames(),y.pollutantNames());
        ASSERT_EQ(x.pollutantMassUnits(),y.pollutantMassUnits());ASSERT_EQ(x.outfallReturnsAvailable(),y.outfallReturnsAvailable());
        ASSERT_EQ(x.tables().size(),y.tables().size());
        for(const auto& [name,t]:x.tables()) {
            const auto& next=y.tables().at(name);EXPECT_EQ(t.fields,next.fields)<<name;EXPECT_EQ(t.units,next.units)<<name;EXPECT_EQ(t.columns,next.columns)<<name;
        }
    }
    ASSERT_EQ(a.tables().size(),b.tables().size());
    for(const auto& [name,t]:a.tables()) {
        const auto& next=b.tables().at(name);EXPECT_EQ(t.fields,next.fields)<<name;EXPECT_EQ(t.units,next.units)<<name;EXPECT_EQ(t.columns,next.columns)<<name;
    }
}
}
TEST(EngineCheckpoint, AcceptedSourceHistoryPreservesEveryEngineStepAndExactContinuation) {
    for(int lid=0;lid<3;++lid)for(double cadence:{.31,.7}) {
        const auto name="engine-history-"+std::to_string(lid)+"-"+std::to_string(cadence);SCOPED_TRACE(name);
        twoD::EngineSourceResultHistory history,resumed;SurfaceSession a,control,b;
        const bool multiple=lid==2;
        ASSERT_NO_THROW(startClosedSpatial(a,name,lid,cadence,true,multiple));
        ASSERT_NO_THROW(startClosedSpatial(control,name+"-control",lid,cadence,true,multiple));
        ASSERT_EQ(a.engine.setEngineSourceResultHistory(history),"");
        EXPECT_DOUBLE_EQ(history.tables().at("boundaries").columns[2][0],0);
        EXPECT_TRUE(history.outfallReturnsAvailable());
        bool repeated=false,leading=false,pending=false;int steps=0;bool restored=false;
        while(a.engine.context().elapsed_ms<a.engine.context().options.totalDurationMs()) {
            ASSERT_NO_THROW({step(a.engine);step(control.engine);});++steps;
            const auto& boundaries=history.tables().at("boundaries").columns;
            ASSERT_EQ(boundaries[0].size(),std::size_t(steps+1));
            EXPECT_DOUBLE_EQ(boundaries[2].back(),a.engine.context().current_time);
            EXPECT_DOUBLE_EQ(boundaries[4].back(),a.stage.completedEnd());EXPECT_DOUBLE_EQ(boundaries[5].back(),a.sources.outfallReturnEnd());
            repeated|=boundaries[3].back()==boundaries[3][boundaries[3].size()-2];leading|=boundaries[5].back()>boundaries[4].back();
            pending|=history.tables().at("pending_outfall_return").rows()>0;
            compare(a.engine,control.engine,name+"-physics");
            if(restored) {ASSERT_NO_THROW(step(b.engine));sameEngineHistory(history,resumed);compare(a.engine,b.engine,name+"-continued");}
            if(steps==25) {
                const auto checkpoint=root()/(name+".chk");ASSERT_EQ(a.engine.saveCheckpoint(checkpoint.string()),"");
#ifdef OPENSWMM_HAS_HDF5_MODEL
                const auto file=root()/(name+".h5");ASSERT_EQ(history.save(file.string()),"");bool available=false;
                ASSERT_EQ(twoD::EngineSourceResultHistory::read(file.string(),resumed,available),"");ASSERT_TRUE(available);sameEngineHistory(history,resumed);
                twoD::SourceResultHistory component;available=true;
                EXPECT_EQ(twoD::SourceResultHistory::read(file.string(),component,available),"");EXPECT_FALSE(available);
#else
                resumed=history;
#endif
                ASSERT_NO_THROW(startClosedSpatial(b,name+"-resumed",lid,cadence,true,multiple));
                const auto original=history.tables().at("boundaries").columns;
                EXPECT_FALSE(b.engine.resumeEngineSourceResultHistory(resumed).empty());
                ASSERT_EQ(b.engine.loadCheckpoint(checkpoint.string()),"");ASSERT_EQ(b.engine.resumeEngineSourceResultHistory(resumed),"");
                EXPECT_EQ(history.tables().at("boundaries").columns,original);sameEngineHistory(history,resumed);restored=true;
            }
        }
        EXPECT_TRUE(repeated);EXPECT_TRUE(leading);EXPECT_TRUE(pending);EXPECT_TRUE(restored);
        const auto& credits=history.tables().at("source_water").columns[16];
        EXPECT_GT(*std::max_element(credits.begin(),credits.end()),0);
        const auto last=history.tables().at("boundaries").columns;double elapsed=0;
        ASSERT_EQ(a.engine.step(&elapsed),0);EXPECT_EQ(history.tables().at("boundaries").columns,last);
        ASSERT_EQ(a.engine.end(),0);
#ifdef OPENSWMM_HAS_HDF5_MODEL
        EXPECT_EQ(history.save((root()/(name+"-final.h5")).string()),"");
#endif
    }
}
TEST(EngineCheckpoint, PairedSourceIntervalsPreserveBothClocksWaterAndRestoredContinuation) {
    for(int lid=0;lid<3;++lid)for(double cadence:{.31,.7}) {
        const auto name="paired-history-"+std::to_string(lid)+"-"+std::to_string(cadence);SCOPED_TRACE(name);
        twoD::EngineSourceResultHistory history,resumed;SurfaceSession a,control,b;
        const bool returns=lid!=0,multiple=lid==2;
        ASSERT_NO_THROW(startClosedSpatial(a,name,lid,cadence,returns,multiple));
        ASSERT_NO_THROW(startClosedSpatial(control,name+"-control",lid,cadence,returns,multiple));
        ASSERT_EQ(a.engine.setEngineSourceResultHistory(history,true),"");
        ASSERT_TRUE(history.sourceIntervals());EXPECT_EQ(history.sourceIntervals()->tables().at("boundaries").rows(),1u);
        twoD::SourceResultHistory separate;EXPECT_THROW(a.engine.surfaceRouter2D().setSourceResultHistory(separate),std::invalid_argument);
        bool repeated=false,multiple_intervals=false,earlier_credits=false,restored=false;int steps=0;
        while(steps<45 && a.engine.context().elapsed_ms<a.engine.context().options.totalDurationMs()) {
            ASSERT_NO_THROW({step(a.engine);step(control.engine);});++steps;
            const auto& engine=history.tables().at("boundaries").columns;
            const auto& source=history.sourceIntervals()->tables().at("boundaries").columns;
            ASSERT_EQ(source[0].size(),std::size_t(a.stage.intervals()+1));
            EXPECT_EQ(source[3].back(),a.stage.completedEnd());EXPECT_EQ(source[3].back(),engine[4].back());
            repeated|=engine[3].back()==engine[3][engine[3].size()-2];
            multiple_intervals|=engine[3].back()-engine[3][engine[3].size()-2]>1;
            if(returns)earlier_credits|=history.sourceIntervals()->tables().at("outfall_clock").columns[1].back()<engine[5].back();
            compare(a.engine,control.engine,name+"-physics");
            if(restored) {ASSERT_NO_THROW(step(b.engine));sameEngineHistory(history,resumed);compare(a.engine,b.engine,name+"-continued");}
            if(steps==25) {
                const auto checkpoint=root()/(name+".chk");ASSERT_EQ(a.engine.saveCheckpoint(checkpoint.string()),"");
#ifdef OPENSWMM_HAS_HDF5_MODEL
                const auto file=root()/(name+".h5");ASSERT_EQ(history.save(file.string()),"");bool available=false;
                ASSERT_EQ(twoD::EngineSourceResultHistory::read(file.string(),resumed,available),"");ASSERT_TRUE(available);sameEngineHistory(history,resumed);
                twoD::SourceResultHistory component;available=true;
                EXPECT_EQ(twoD::SourceResultHistory::read(file.string(),component,available),"");EXPECT_FALSE(available);
#else
                resumed=history;
#endif
                ASSERT_NO_THROW(startClosedSpatial(b,name+"-resumed",lid,cadence,returns,multiple));
                const auto prefix=history;EXPECT_FALSE(b.engine.resumeEngineSourceResultHistory(resumed).empty());
                ASSERT_EQ(b.engine.loadCheckpoint(checkpoint.string()),"");ASSERT_EQ(b.engine.resumeEngineSourceResultHistory(resumed),"");
                sameEngineHistory(history,prefix);sameEngineHistory(history,resumed);compare(a.engine,b.engine,name+"-rebind");restored=true;
            }
        }
        EXPECT_TRUE(repeated);EXPECT_TRUE(multiple_intervals);EXPECT_TRUE(restored);EXPECT_EQ(earlier_credits,returns);
        const auto prefix=history;
        ASSERT_EQ(a.engine.end(),0);ASSERT_EQ(control.engine.end(),0);sameEngineHistory(history,prefix);
        EXPECT_EQ(a.stage.intervals(),control.stage.intervals());EXPECT_EQ(a.stage.completedEnd(),control.stage.completedEnd());
        EXPECT_EQ(a.sources.ledgers()[0].outfall_return,control.sources.ledgers()[0].outfall_return);
        EXPECT_EQ(a.engine.surfaceRouter2D().state().depth,control.engine.surfaceRouter2D().state().depth);
#ifdef OPENSWMM_HAS_HDF5_MODEL
        EXPECT_EQ(history.save((root()/(name+"-final.h5")).string()),"");
#endif
    }
}
TEST(EngineCheckpoint, PairedSourceHistoryInspectsEarlierSnapshotsAndOwnershipWithoutRollingBackEngine) {
    SurfaceSession a;ASSERT_NO_THROW(startClosedSpatial(a,"paired-guards",2,.31,true,true));
    twoD::EngineSourceResultHistory history;ASSERT_EQ(a.engine.setEngineSourceResultHistory(history,true),"");
    for(int k=0;k<25;++k)ASSERT_NO_THROW(step(a.engine));
    ASSERT_GT(history.sourceIntervals()->tables().at("boundaries").rows(),1u);
    const auto checkpoint=root()/"paired-guards.chk";ASSERT_EQ(a.engine.saveCheckpoint(checkpoint.string()),"");
    for(int mutation=0;mutation<7;++mutation) {
        SCOPED_TRACE(mutation);auto edited=history;
        auto& source=*const_cast<twoD::SourceResultHistory*>(edited.sourceIntervals());
        auto& t=const_cast<std::map<std::string,twoD::SourceResultTable>&>(source.tables());
        switch(mutation) {
        case 0:t.at("footprints").columns[6][0]+=.001;break; // valid weather area, false static identity
        case 1:t.at("lid_usages").columns[4][0]+=.001;break;
        case 2:t.at("receiver").columns[6].back()+=.001;break; // pending ET invisible to engine water snapshot
        case 3:for(auto& c:t.at("boundaries").columns)c.pop_back();break;
        case 4:t.at("outfall_clock").columns[1].back()=a.sources.outfallReturnEnd();break;
        case 5:const_cast<std::vector<std::string>&>(source.sourceNames())[0]="different";break;
        case 6:t.at("boundaries").columns[3].back()+=.001;break;
        }
        const auto original=edited;SurfaceSession target;
        ASSERT_NO_THROW(startClosedSpatial(target,"paired-target-"+std::to_string(mutation),2,.31,true,true));
        ASSERT_EQ(target.engine.loadCheckpoint(checkpoint.string()),"");
        const auto before=root()/("paired-target-"+std::to_string(mutation)+".chk");ASSERT_EQ(target.engine.saveCheckpoint(before.string()),"");const auto saved=bytes(before);
        EXPECT_FALSE(target.engine.resumeEngineSourceResultHistory(edited).empty());sameEngineHistory(edited,original);
        ASSERT_EQ(target.engine.saveCheckpoint(before.string()),"");EXPECT_EQ(bytes(before),saved);
        auto valid=history;ASSERT_EQ(target.engine.resumeEngineSourceResultHistory(valid),"");
        ASSERT_EQ(target.engine.saveCheckpoint(before.string()),"");EXPECT_EQ(bytes(before),saved);
        ASSERT_NO_THROW(step(target.engine));
    }
}
TEST(EngineCheckpoint, PairedSourceHistorySupportsInitialRebindAndVectorRichardsBackwater) {
    for(bool returns:{false,true}) {
        const auto name=std::string("paired-vector-")+(returns?"returns":"ordinary");
        SurfaceSession a,b,control;twoD::EngineSourceResultHistory history,resumed;
        ASSERT_NO_THROW(a.start(name,3,0,1,6,true,true,true,returns?2:-1));
        ASSERT_NO_THROW(control.start(name+"-control",3,0,1,6,true,true,true,returns?2:-1));
        ASSERT_EQ(a.engine.setEngineSourceResultHistory(history,true),"");resumed=history;
        const auto initial=root()/(name+"-initial.chk");ASSERT_EQ(a.engine.saveCheckpoint(initial.string()),"");
        ASSERT_NO_THROW(b.start(name+"-resumed",3,0,1,6,true,true,true,returns?2:-1));
        ASSERT_EQ(b.engine.loadCheckpoint(initial.string()),"");ASSERT_EQ(b.engine.resumeEngineSourceResultHistory(resumed),"");
        for(int k=0;k<20;++k) {
            ASSERT_NO_THROW({step(a.engine);step(b.engine);step(control.engine);});sameEngineHistory(history,resumed);
            compare(a.engine,b.engine,name+"-continued");compare(a.engine,control.engine,name+"-physics");
        }
#ifdef OPENSWMM_HAS_HDF5_MODEL
        const auto h5=root()/(name+".h5");ASSERT_EQ(history.save(h5.string()),"");twoD::EngineSourceResultHistory read;bool available=false;
        ASSERT_EQ(twoD::EngineSourceResultHistory::read(h5.string(),read,available),"");ASSERT_TRUE(available);sameEngineHistory(history,read);
#endif
    }
}
TEST(EngineCheckpoint, AcceptedSourceHistoryRefusesWrongProfilesRestoreOrderAndFinalProjection) {
    twoD::EngineSourceResultHistory empty,history;SWMMEngine ordinary;
    ASSERT_NO_THROW(start(ordinary,"engine-history-ordinary",0,true,0,false,true));
    EXPECT_FALSE(ordinary.setEngineSourceResultHistory(empty).empty());EXPECT_TRUE(empty.tables().empty());
    SurfaceSession mesh;ASSERT_NO_THROW(mesh.start("engine-history-mesh"));
    EXPECT_FALSE(mesh.engine.setEngineSourceResultHistory(empty).empty());EXPECT_TRUE(empty.tables().empty());
    SurfaceSession a;ASSERT_NO_THROW(startClosedSpatial(a,"engine-history-guards",2,.31,true));
    ASSERT_EQ(a.engine.setEngineSourceResultHistory(history),"");EXPECT_FALSE(a.engine.setEngineSourceResultHistory(empty).empty());
    for(int k=0;k<25;++k)ASSERT_NO_THROW(step(a.engine));
    const auto good=root()/"engine-history-guards.chk";ASSERT_EQ(a.engine.saveCheckpoint(good.string()),"");
    twoD::EngineSourceResultHistory attached;SurfaceSession initial;
    ASSERT_NO_THROW(startClosedSpatial(initial,"engine-history-attached",2,.31,true));
    ASSERT_EQ(initial.engine.setEngineSourceResultHistory(attached),"");
    const auto before=root()/"engine-history-attached.chk";ASSERT_EQ(initial.engine.saveCheckpoint(before.string()),"");auto original=bytes(before);
    EXPECT_FALSE(initial.engine.loadCheckpoint(good.string()).empty());ASSERT_EQ(initial.engine.saveCheckpoint(before.string()),"");EXPECT_EQ(bytes(before),original);
    for(int mutation=0;mutation<10;++mutation) {
        SCOPED_TRACE(mutation);auto edited=history;
        auto& t=const_cast<std::map<std::string,twoD::SourceResultTable>&>(edited.tables());
        switch(mutation) {
        case 0:t.at("boundaries").columns[2].back()+=.001;break;
        case 1:t.at("boundaries").columns[7].back()+=1;break; // semantic but false final projection
        case 2:for(auto& v:t.at("source_water").columns[3])v=.25;break; // valid fraction, false identity
        case 3:t.at("group_water").columns[3].back()+=1;break;
        case 4:t.at("node_delivery").columns[4].back()+=1;break;
        case 5:t.at("boundaries").units[2]="h";break;
        case 6:const_cast<std::vector<std::string>&>(edited.sourceNames())[0]="other catchment";break;
        case 7:t.at("boundaries").columns[3].back()+=1;break;
        case 8:t.at("source_water").columns[16].back()=-1;break;
        case 9:ASSERT_GT(t.at("pending_outfall_return").rows(),0u);t.at("pending_outfall_return").columns[4].back()+=1;break;
        }
        SurfaceSession target;ASSERT_NO_THROW(startClosedSpatial(target,"engine-history-mutant-"+std::to_string(mutation),2,.31,true));
        ASSERT_EQ(target.engine.loadCheckpoint(good.string()),"");
        const auto path=root()/("engine-history-mutant-"+std::to_string(mutation)+".chk");ASSERT_EQ(target.engine.saveCheckpoint(path.string()),"");const auto bytes_before=bytes(path);
        const auto columns=t.at("boundaries").columns;
        EXPECT_FALSE(target.engine.resumeEngineSourceResultHistory(edited).empty());EXPECT_EQ(t.at("boundaries").columns,columns);
        ASSERT_EQ(target.engine.saveCheckpoint(path.string()),"");EXPECT_EQ(bytes(path),bytes_before);
        auto valid=history;ASSERT_EQ(target.engine.resumeEngineSourceResultHistory(valid),"");ASSERT_NO_THROW(step(target.engine));
    }
}
TEST(EngineCheckpoint, AcceptedSourceHistoryKeepsDisabledReturnsUnavailableAndVectorRichardsBackwater) {
    for(bool returns:{false,true}) {
        twoD::EngineSourceResultHistory history,resumed;SurfaceSession a,b;
        const auto name=std::string("engine-history-vector-")+(returns?"returns":"ordinary");
        ASSERT_NO_THROW(a.start(name,3,0,1,6,true,true,true,returns?2:-1));
        ASSERT_EQ(a.engine.setEngineSourceResultHistory(history),"");EXPECT_EQ(history.outfallReturnsAvailable(),returns);
        EXPECT_EQ(history.tables().count("pending_outfall_return"),returns?1u:0u);
        for(int k=0;k<16;++k)ASSERT_NO_THROW(step(a.engine));
        const auto file=root()/(name+".chk");ASSERT_EQ(a.engine.saveCheckpoint(file.string()),"");resumed=history;
        ASSERT_NO_THROW(b.start(name+"-resumed",3,0,1,6,true,true,true,returns?2:-1));
        ASSERT_EQ(b.engine.loadCheckpoint(file.string()),"");ASSERT_EQ(b.engine.resumeEngineSourceResultHistory(resumed),"");
        for(int k=0;k<4;++k){ASSERT_NO_THROW({step(a.engine);step(b.engine);});sameEngineHistory(history,resumed);compare(a.engine,b.engine,name);}
#ifdef OPENSWMM_HAS_HDF5_MODEL
        const auto h5=root()/(name+".h5");ASSERT_EQ(history.save(h5.string()),"");twoD::EngineSourceResultHistory read;bool available=false;
        ASSERT_EQ(twoD::EngineSourceResultHistory::read(h5.string(),read,available),"");ASSERT_TRUE(available);sameEngineHistory(history,read);
#endif
    }
}
#ifdef OPENSWMM_HAS_HDF5_MODEL
TEST(EngineCheckpoint, AcceptedSourceHistoryPublicationAndMalformedReaderPreservePreviousArtifacts) {
    twoD::EngineSourceResultHistory history;SurfaceSession a;
    ASSERT_NO_THROW(startClosedSpatial(a,"engine-history-files",0,.7,true));ASSERT_EQ(a.engine.setEngineSourceResultHistory(history),"");
    for(int k=0;k<12;++k)ASSERT_NO_THROW(step(a.engine));
    const auto good=root()/"engine-history-files.h5";ASSERT_EQ(history.save(good.string()),"");const auto original=bytes(good);
    auto invalid=history;const_cast<std::map<std::string,twoD::SourceResultTable>&>(invalid.tables()).at("boundaries").columns[5].back()=0;
    EXPECT_FALSE(invalid.save(good.string()).empty());EXPECT_EQ(bytes(good),original);
    auto unrelated=root()/"engine-history-unrelated.h5";write(unrelated,"keep unrelated data");
    EXPECT_FALSE(history.save(unrelated.string()).empty());EXPECT_EQ(bytes(unrelated),"keep unrelated data");
    const auto broken=root()/"engine-history-broken.h5";write(broken,original.substr(0,original.size()/2));
    twoD::EngineSourceResultHistory out=history;bool available=true;
    EXPECT_FALSE(twoD::EngineSourceResultHistory::read(broken.string(),out,available).empty());EXPECT_FALSE(available);sameEngineHistory(history,out);
    available=true;EXPECT_FALSE(twoD::EngineSourceResultHistory::read((root()/"missing-engine-history.h5").string(),out,available).empty());EXPECT_FALSE(available);sameEngineHistory(history,out);
    ASSERT_EQ(twoD::EngineSourceResultHistory::read(good.string(),out,available),"");ASSERT_TRUE(available);sameEngineHistory(history,out);
}
#endif
#endif

namespace {
void startBinary(SWMMEngine& e,const std::string& name,int lid=0,bool average=false,bool late=false,int subset=0,bool metric=false,bool qualify=true) {
    SWMMEngine authored;startRunoff(authored,name,2,0,lid,false);authored.close();
    const auto path=root()/name;std::filesystem::remove(path.string()+".out");
    if(metric) {auto deck=bytes(path.string()+".inp");deck.replace(deck.find("FLOW_UNITS CFS"),14,"FLOW_UNITS CMS");write(path.string()+".inp",deck);}
    if(e.open((path.string()+".inp").c_str(),(path.string()+".rpt").c_str(),(path.string()+".out").c_str()))throw std::runtime_error(e.context().error_message);
    auto& ctx=e.context();ctx.options.num_threads=1;ctx.options.rpt_averages=average;
    if(late)ctx.options.report_start=datetime::addSeconds(ctx.options.start_date,13);
    if(subset) {
        std::fill(ctx.links.rpt_flag.begin(),ctx.links.rpt_flag.end(),0);
        std::fill(ctx.nodes.rpt_flag.begin(),ctx.nodes.rpt_flag.end(),0);
        if(subset==1)ctx.nodes.rpt_flag[ctx.node_names.find("S")]=1;
        else std::fill(ctx.subcatches.rpt_flag.begin(),ctx.subcatches.rpt_flag.end(),0);
    }
    if(e.initialize())throw std::runtime_error(ctx.error_message);
    if(qualify) {auto error=e.prepareOutputCheckpointing();if(!error.empty())throw std::runtime_error(error);}
    if(e.start(1))throw std::runtime_error(ctx.error_message);
    if(qualify) {auto error=e.enableCheckpointing();if(!error.empty())throw std::runtime_error(error);}
}
void finishBinary(SWMMEngine& e) {
    while(e.context().elapsed_ms<e.context().options.totalDurationMs())step(e);
    if(e.end())throw std::runtime_error(e.context().error_message);
}
void sameBinary(const std::string& a,const std::string& b,int periods) {
    const auto pa=root()/(a+".out"),pb=root()/(b+".out");
    EXPECT_EQ(bytes(pa),bytes(pb));OutputReader reader;ASSERT_TRUE(reader.open(pa.string().c_str()));
    EXPECT_EQ(reader.period_count(),periods);
}
// The output component is the final length-prefixed field of formats 6/7/8.
// Read its explicit layout, then rehash the outer engine record for mutants.
std::string changeBinary(const std::string& checkpoint,int mutation) {
    detail::CompletedSourceArchive in(checkpoint);std::string magic,model,payload;uint32_t version,sum;
    in(magic,version,model,sum,payload);in.finish();
    const int marker=516114522;const std::string binary(reinterpret_cast<const char*>(&marker),sizeof(marker));
    const auto body=payload.rfind(binary); // last match is the native prefix header (live files have no footer)
    if(body==std::string::npos || body<24)throw std::runtime_error("missing output checkpoint prefix");
    const auto offset=body-24;uint64_t length=0;std::memcpy(&length,payload.data()+offset,8);
    if(offset+8+length!=payload.size())throw std::runtime_error("invalid output checkpoint suffix");
    detail::CompletedSourceArchive wire(payload.substr(offset+8));uint32_t format;int count;std::string prefix;
    wire(format,count,prefix);wire.finish();
    std::array<int,7> native;std::memcpy(native.data(),prefix.data(),sizeof(native));
    const std::size_t row=8+4*(native[3]*(8+native[6])+native[4]*(6+native[6])+native[5]*(5+native[6])+15);
    const auto header=prefix.size()-std::size_t(count)*row;
    if(mutation==0)format=2;
    if(mutation==1)++count;
    if(mutation==2)prefix[0]^=1;
    if(mutation==3)prefix.pop_back();
    if(mutation==4)prefix.push_back('x');
    if(mutation==5) {double date;std::memcpy(&date,prefix.data()+header,8);date+=1;std::memcpy(prefix.data()+header,&date,8);}
    if(mutation==6) {const float value=std::numeric_limits<float>::quiet_NaN();std::memcpy(prefix.data()+header+8,&value,4);}
    if(mutation==7) {int bad=123;std::memcpy(prefix.data()+8,&bad,4);}
    if(mutation==8)--version;
    detail::CompletedSourceArchive changed;changed(format,count,prefix);auto output=changed.bytes();detail::CompletedSourceArchive suffix;suffix(output);
    payload.replace(offset,payload.size()-offset,suffix.bytes());sum=crc(payload);
    detail::CompletedSourceArchive out;out(magic,version,model,sum,payload);return out.bytes();
}
}

TEST(EngineCheckpoint, BinaryOutputContinuationPreservesReportGridAveragesSubsetsAndUnits) {
    for(int lid:{0,1,2,3})for(bool average:{false,true})for(bool late:{false,true}) {
        const auto name="binary-"+std::to_string(lid)+"-"+std::to_string(average)+"-"+std::to_string(late);SCOPED_TRACE(name);
        SWMMEngine a,b,control;const int subset=lid%3;const bool metric=lid==0;
        ASSERT_NO_THROW(startBinary(a,name,lid,average,late,subset,metric));
        ASSERT_NO_THROW(startBinary(control,name+"-control",lid,average,late,subset,metric,false));
        const double cut=lid==0?0:(lid==1?1.4:(lid==2?3.:17.5));
        while(a.context().current_time<cut)ASSERT_NO_THROW(step(a));
        const auto checkpoint=root()/(name+".chk");ASSERT_EQ(a.saveCheckpoint(checkpoint.string()),"");
        ASSERT_NO_THROW(startBinary(b,name+"-resumed",lid,average,late,subset,metric));
        ASSERT_EQ(b.loadCheckpoint(checkpoint.string()),"");compare(a,b,name+"-restored");
        OutputReader live;ASSERT_TRUE(live.openLive((root()/(name+"-resumed.out")).string().c_str()));
        EXPECT_EQ(live.period_count(),std::max(0,int(a.context().next_report_ms/3000)-1-(late?4:0)));
        ASSERT_NO_THROW(finishBinary(a));
    ASSERT_NO_THROW(finishBinary(b));
    ASSERT_NO_THROW(finishBinary(control));
        sameBinary(name,name+"-resumed",late?76:80);sameBinary(name,name+"-control",late?76:80);
    }
}
TEST(EngineCheckpoint, BinaryOutputMutantsPreserveFreshWriterAndAllowMatchingRetry) {
    SWMMEngine a;
    ASSERT_NO_THROW(startBinary(a,"binary-mutant-reference"));
    while(a.context().current_time<18.1)ASSERT_NO_THROW(step(a));
    const auto good=root()/"binary-mutant-reference.chk";ASSERT_EQ(a.saveCheckpoint(good.string()),"");
    for(int mutation=0;mutation<9;++mutation) {
        SCOPED_TRACE(mutation);const auto name="binary-mutant-"+std::to_string(mutation);
        const auto bad=root()/(name+".chk");
    ASSERT_NO_THROW(write(bad,changeBinary(bytes(good),mutation)));
        SWMMEngine b;
    ASSERT_NO_THROW(startBinary(b,name));const auto before=root()/(name+"-before.chk");
        ASSERT_EQ(b.saveCheckpoint(before.string()),"");const auto saved=bytes(before),header=bytes(root()/(name+".out"));
        EXPECT_FALSE(b.loadCheckpoint(bad.string()).empty());ASSERT_EQ(b.saveCheckpoint(before.string()),"");
        EXPECT_EQ(bytes(before),saved);EXPECT_EQ(bytes(root()/(name+".out")),header);
        ASSERT_EQ(b.loadCheckpoint(good.string()),"");compare(a,b,name+"-retry");ASSERT_EQ(b.end(),0);
    }
    ASSERT_EQ(a.end(),0);
}
TEST(EngineCheckpoint, BinaryOutputRestorePublicationFailureLeavesEngineUnchanged) {
    SWMMEngine a,b;
    ASSERT_NO_THROW(startBinary(a,"binary-publication-a"));
    ASSERT_NO_THROW(startBinary(b,"binary-publication-b"));
    while(a.context().current_time<12.1)ASSERT_NO_THROW(step(a));
    const auto good=root()/"binary-publication.chk",before=root()/"binary-publication-before.chk",output=root()/"binary-publication-b.out";
    ASSERT_EQ(a.saveCheckpoint(good.string()),"");ASSERT_EQ(b.saveCheckpoint(before.string()),"");const auto saved=bytes(before),header=bytes(output);
    std::filesystem::permissions(output,std::filesystem::perms::owner_read);
    EXPECT_FALSE(b.loadCheckpoint(good.string()).empty());ASSERT_EQ(b.saveCheckpoint(before.string()),"");EXPECT_EQ(bytes(before),saved);EXPECT_EQ(bytes(output),header);
    std::filesystem::permissions(output,std::filesystem::perms::owner_read|std::filesystem::perms::owner_write);
    ASSERT_EQ(b.loadCheckpoint(good.string()),"");compare(a,b,"binary-publication-retry");ASSERT_EQ(a.end(),0);ASSERT_EQ(b.end(),0);
}
TEST(EngineCheckpoint, BinaryOutputModeRefusesExistingDestinationAndWrongLifecycle) {
    SWMMEngine authored;
    ASSERT_NO_THROW(start(authored,"binary-guard-model",0,0,false,false));ASSERT_EQ(authored.close(),0);
    const auto inp=root()/"binary-guard-model.inp",out=root()/"binary-guard.out";write(out,"saved results");
    SWMMEngine e;ASSERT_EQ(e.open(inp.string().c_str(),nullptr,out.string().c_str()),0);
    EXPECT_FALSE(e.prepareOutputCheckpointing().empty());ASSERT_EQ(e.initialize(),0);
    EXPECT_FALSE(e.prepareOutputCheckpointing().empty());EXPECT_EQ(bytes(out),"saved results");
    std::filesystem::remove(out);
    auto* extra=new HeldOutput;e.plugin_factory().add_output_plugin(extra);
    EXPECT_FALSE(e.prepareOutputCheckpointing().empty());e.plugin_factory().unload_all();ASSERT_EQ(e.close(),0);
    SWMMEngine fresh;ASSERT_EQ(fresh.open(inp.string().c_str(),nullptr,out.string().c_str()),0);ASSERT_EQ(fresh.initialize(),0);
#ifdef OPENSWMM_HAS_2D
    fresh.surfaceRouter2D().options().output_file="unqualified-mesh.h5";
    EXPECT_FALSE(fresh.prepareOutputCheckpointing().empty());fresh.surfaceRouter2D().options().output_file="";
#endif
    ASSERT_EQ(fresh.prepareOutputCheckpointing(),"");
    // An intervening creator is also protected by the exclusive open in start.
    write(out,"new results");EXPECT_NE(fresh.start(1),0);EXPECT_EQ(bytes(out),"new results");ASSERT_EQ(fresh.close(),0);
    SWMMEngine ordinary;
    ASSERT_NO_THROW(startBinary(ordinary,"binary-unprepared",0,false,false,0,false,false));
    EXPECT_FALSE(ordinary.prepareOutputCheckpointing().empty());EXPECT_FALSE(ordinary.enableCheckpointing().empty());ASSERT_EQ(ordinary.end(),0);
    SWMMEngine no_output;
    ASSERT_NO_THROW(start(no_output,"binary-no-output"));EXPECT_FALSE(no_output.prepareOutputCheckpointing().empty());
}

#ifdef OPENSWMM_HAS_2D
TEST(EngineCheckpoint, BinaryOutputContinuesMeshAndPairedSpatialHistories) {
    for(int lid=0;lid<3;++lid)for(double cadence:{.7,.31})for(bool returns:{false,true}) {
        const auto name="binary-spatial-"+std::to_string(lid)+"-"+std::to_string(cadence)+"-"+std::to_string(returns);SCOPED_TRACE(name);
        SurfaceSession a,b,control;twoD::EngineSourceResultHistory history,resumed;
        ASSERT_NO_THROW(startClosedSpatial(a,name,lid,cadence,returns,true,true));
        ASSERT_NO_THROW(startClosedSpatial(control,name+"-control",lid,cadence,returns,true,true));
        ASSERT_EQ(a.engine.setEngineSourceResultHistory(history,true),"");
        while(a.engine.context().current_time<7.1)ASSERT_NO_THROW(step(a.engine));
        const auto checkpoint=root()/(name+".chk");ASSERT_EQ(a.engine.saveCheckpoint(checkpoint.string()),"");resumed=history;
        ASSERT_NO_THROW(startClosedSpatial(b,name+"-resumed",lid,cadence,returns,true,true));
        ASSERT_EQ(b.engine.loadCheckpoint(checkpoint.string()),"");ASSERT_EQ(b.engine.resumeEngineSourceResultHistory(resumed),"");
        compare(a.engine,b.engine,name+"-restored");sameEngineHistory(history,resumed);
        ASSERT_NO_THROW(finishBinary(a.engine));
    ASSERT_NO_THROW(finishBinary(b.engine));
    ASSERT_NO_THROW(finishBinary(control.engine));
        sameBinary(name,name+"-resumed",10);sameBinary(name,name+"-control",10);sameEngineHistory(history,resumed);
    }
    for(bool spatial:{false,true}) {
        const auto name=std::string("binary-mesh-vector-")+(spatial?"spatial":"mesh");SurfaceSession a,b;
        ASSERT_NO_THROW(a.start(name,3,0,1,6,true,spatial,spatial,spatial?2:-1,true));
        while(a.engine.context().current_time<17.1)ASSERT_NO_THROW(step(a.engine));
        const auto checkpoint=root()/(name+".chk");ASSERT_EQ(a.engine.saveCheckpoint(checkpoint.string()),"");
        ASSERT_NO_THROW(b.start(name+"-resumed",3,0,1,6,true,spatial,spatial,spatial?2:-1,true));
        ASSERT_EQ(b.engine.loadCheckpoint(checkpoint.string()),"");compare(a.engine,b.engine,name+"-restored");
        for(int k=0;k<12;++k) {ASSERT_NO_THROW(step(a.engine));
    ASSERT_NO_THROW(step(b.engine));}
        compare(a.engine,b.engine,name+"-continued");ASSERT_EQ(a.engine.end(),0);ASSERT_EQ(b.engine.end(),0);
        EXPECT_EQ(bytes(root()/(name+".out")),bytes(root()/(name+"-resumed.out")));
    }
}
TEST(EngineCheckpoint, BinaryOutputPublicationFailurePreservesAllSpatialComponents) {
    SurfaceSession a,b;
    ASSERT_NO_THROW(startClosedSpatial(a,"binary-spatial-publication-a",2,.31,true,true,true));
    ASSERT_NO_THROW(startClosedSpatial(b,"binary-spatial-publication-b",2,.31,true,true,true));
    while(a.engine.context().current_time<9.1)ASSERT_NO_THROW(step(a.engine));
    const auto good=root()/"binary-spatial-publication.chk",before=root()/"binary-spatial-publication-before.chk",output=root()/"binary-spatial-publication-b.out";
    ASSERT_EQ(a.engine.saveCheckpoint(good.string()),"");ASSERT_EQ(b.engine.saveCheckpoint(before.string()),"");const auto saved=bytes(before),header=bytes(output);
    std::filesystem::permissions(output,std::filesystem::perms::owner_read);
    EXPECT_FALSE(b.engine.loadCheckpoint(good.string()).empty());ASSERT_EQ(b.engine.saveCheckpoint(before.string()),"");
    EXPECT_EQ(bytes(before),saved);EXPECT_EQ(bytes(output),header);EXPECT_EQ(b.stage.intervals(),0);
    std::filesystem::permissions(output,std::filesystem::perms::owner_read|std::filesystem::perms::owner_write);
    ASSERT_EQ(b.engine.loadCheckpoint(good.string()),"");compare(a.engine,b.engine,"binary-spatial-publication-retry");
    ASSERT_EQ(a.engine.end(),0);ASSERT_EQ(b.engine.end(),0);
}
#endif

#ifndef __EMSCRIPTEN__
TEST(EngineCheckpoint, OutputBarrierWaitsForActiveWriteAndQueuedWritesAndReportsFailure) {
    PluginFactory factory;auto* output=new HeldOutput;factory.add_output_plugin(output);
    IOThread io(factory);io.start();io.post(SimulationSnapshot{});
    {
        std::unique_lock lock(output->mutex);
        EXPECT_TRUE(output->changed.wait_for(lock,std::chrono::seconds(5),[&]{return output->entered;}));
    }
    auto barrier=std::async(std::launch::async,[&]{return io.flush();});
    EXPECT_EQ(barrier.wait_for(std::chrono::milliseconds(20)),std::future_status::timeout);
    io.post(SimulationSnapshot{});
    {
        std::lock_guard lock(output->mutex);output->released=true;
    }
    output->changed.notify_all();EXPECT_EQ(barrier.get(),-17);
    EXPECT_EQ(io.tasks_completed(),2);EXPECT_EQ(io.flush(),-17);io.stop();
}
#endif

#ifdef OPENSWMM_HAS_2D
TEST(EngineCheckpoint, BinaryOutputPreservesExistingSourceBatchSchedule) {
    for(double cadence:{.7,.31}) {
        const auto name="binary-source-schedule-"+std::to_string(cadence);SurfaceSession binary,ordinary;
        ASSERT_NO_THROW(startClosedSpatial(binary,name,2,cadence,true,true,true));
        ASSERT_NO_THROW(startClosedSpatial(ordinary,name+"-ordinary",2,cadence,true,true,false));
        while(binary.engine.context().current_time<12.1) {
            ASSERT_NO_THROW(step(binary.engine));
            ASSERT_NO_THROW(step(ordinary.engine));
        }
        const auto a=root()/(name+".chk"),b=root()/(name+"-ordinary.chk");
        ASSERT_EQ(binary.engine.saveCheckpoint(a.string()),"");ASSERT_EQ(ordinary.engine.saveCheckpoint(b.string()),"");
        detail::CompletedSourceArchive in(bytes(a));std::string magic,model,payload;uint32_t version,sum;
        in(magic,version,model,sum,payload);in.finish();ASSERT_EQ(version,8u);
        const int marker=516114522;const std::string native(reinterpret_cast<const char*>(&marker),4);
        const auto body=payload.rfind(native);ASSERT_NE(body,std::string::npos);ASSERT_GE(body,24u);
        payload.resize(body-24);version-=3;sum=crc(payload);detail::CompletedSourceArchive out;out(magic,version,model,sum,payload);
        EXPECT_EQ(out.bytes(),bytes(b));EXPECT_EQ(binary.stage.intervals(),ordinary.stage.intervals());
        EXPECT_EQ(binary.stage.completedEnd(),ordinary.stage.completedEnd());
        ASSERT_EQ(binary.engine.end(),0);ASSERT_EQ(ordinary.engine.end(),0);
    }
}
#endif

#ifdef OPENSWMM_HAS_2D
TEST(EngineCheckpoint, ValidBinaryPrefixIsNotPublishedWhenSpatialCheckpointValidationFails) {
    SurfaceSession a,b;ASSERT_NO_THROW(startClosedSpatial(a,"binary-spatial-invalid-a",2,.31,true,true,true));
    ASSERT_NO_THROW(startClosedSpatial(b,"binary-spatial-invalid-b",2,.31,true,true,true));
    while(a.engine.context().current_time<9.1)ASSERT_NO_THROW(step(a.engine));
    const auto good=root()/"binary-spatial-invalid-good.chk",bad=root()/"binary-spatial-invalid-bad.chk",before=root()/"binary-spatial-invalid-before.chk";
    ASSERT_EQ(a.engine.saveCheckpoint(good.string()),"");
    detail::CompletedSourceArchive in(bytes(good));std::string magic,model,payload;uint32_t version,sum;
    in(magic,version,model,sum,payload);in.finish();
    auto surface=engineSurface(bytes(good));auto body=surfacePayload(surface);
    const std::string cpu_magic="OpenSWMM CPU surface/source boundary";
    auto cpu=nested(body,cpu_magic).second;cpu.back()^=1;
    surface=withSurfacePayload(surface,replaceNested(body,cpu_magic,cpu));
    payload=replaceNested(payload,"OpenSWMM joint DW surface/source boundary",surface);sum=crc(payload);
    detail::CompletedSourceArchive out;out(magic,version,model,sum,payload);write(bad,out.bytes());
    const auto destination=root()/"binary-spatial-invalid-b.out";
    ASSERT_EQ(b.engine.saveCheckpoint(before.string()),"");const auto saved=bytes(before),header=bytes(destination);
    EXPECT_FALSE(b.engine.loadCheckpoint(bad.string()).empty());ASSERT_EQ(b.engine.saveCheckpoint(before.string()),"");
    EXPECT_EQ(bytes(before),saved);EXPECT_EQ(bytes(destination),header);
    for(const auto& file:std::filesystem::directory_iterator(root()))
        EXPECT_FALSE(file.path().filename().string().starts_with("binary-spatial-invalid-b.out.report-part-"));
    ASSERT_EQ(b.engine.loadCheckpoint(good.string()),"");compare(a.engine,b.engine,"binary-spatial-invalid-retry");
    ASSERT_EQ(a.engine.end(),0);ASSERT_EQ(b.engine.end(),0);
}
#endif

#ifdef OPENSWMM_MESH_OUTPUT_CHECKPOINT_TEST
namespace {
struct MeshOutputWire {
    uint32_t version=1; double step=0; hsize_t count=0; double due=-1; bool groundwater=false; std::string image;
    void io(detail::CompletedSourceArchive& a) {a(version,step,count,due,groundwater,image);}
};
std::string meshPhysical(std::string checkpoint) {
    detail::CompletedSourceArchive in(std::move(checkpoint));std::string magic,model,payload;uint32_t version,sum;
    in(magic,version,model,sum,payload);in.finish();
    const auto image=payload.rfind(std::string("\211HDF\r\n\032\n",8));
    if(image==std::string::npos)throw std::runtime_error("Absent mesh checkpoint image");
    const auto begin=image-sizeof(uint64_t)-sizeof(bool)-sizeof(double)-sizeof(hsize_t)-sizeof(double)-sizeof(uint32_t);
    MeshOutputWire wire;detail::CompletedSourceArchive tail(payload.substr(begin));wire.io(tail);tail.finish();
    payload.resize(begin-sizeof(uint64_t));version-=6;sum=crc(payload);detail::CompletedSourceArchive out;
    out(magic,version,model,sum,payload);return out.bytes();
}
void compareMesh(SWMMEngine& a,SWMMEngine& b,const std::string& name) {
    const auto x=root()/(name+"-a.chk"),y=root()/(name+"-b.chk");
    ASSERT_EQ(a.saveCheckpoint(x.string()),"");ASSERT_EQ(b.saveCheckpoint(y.string()),"");
    EXPECT_EQ(meshPhysical(bytes(x)),meshPhysical(bytes(y)));
}
twoD::Default2DOutputPlugin& meshWriter(SWMMEngine& e) {
    for(auto* p:e.plugin_factory().output_plugins())if(auto* w=dynamic_cast<twoD::Default2DOutputPlugin*>(p))return *w;
    throw std::runtime_error("Absent mesh writer");
}
void sameMesh(const std::string& a,const std::string& b,int periods) {
    hid_t x=H5Fopen((root()/(a+".h5")).string().c_str(),H5F_ACC_RDONLY,H5P_DEFAULT);
    hid_t y=H5Fopen((root()/(b+".h5")).string().c_str(),H5F_ACC_RDONLY,H5P_DEFAULT);
    ASSERT_GE(x,0);ASSERT_GE(y,0);H5G_info_t xi{},yi{};
    ASSERT_GE(H5Gget_info(x,&xi),0);ASSERT_GE(H5Gget_info(y,&yi),0);EXPECT_EQ(xi.nlinks,yi.nlinks);
    for(hsize_t k=0;k<xi.nlinks;++k) {
        char name[256];ASSERT_GE(H5Lget_name_by_idx(x,".",H5_INDEX_NAME,H5_ITER_INC,k,name,sizeof(name),H5P_DEFAULT),0);
        ASSERT_GT(H5Lexists(y,name,H5P_DEFAULT),0);H5O_info_t info{};ASSERT_GE(H5Oget_info_by_name(x,name,&info,H5O_INFO_BASIC,H5P_DEFAULT),0);
        if(info.type!=H5O_TYPE_DATASET)continue;
        hid_t xd=H5Dopen2(x,name,H5P_DEFAULT),yd=H5Dopen2(y,name,H5P_DEFAULT);
        hid_t xs=H5Dget_space(xd),ys=H5Dget_space(yd);const auto n=H5Sget_simple_extent_npoints(xs);
        EXPECT_EQ(n,H5Sget_simple_extent_npoints(ys));
        if(std::string(name)=="time")EXPECT_EQ(n,periods);
        std::vector<double> xv(n),yv(n);ASSERT_GE(H5Dread(xd,H5T_NATIVE_DOUBLE,H5S_ALL,H5S_ALL,H5P_DEFAULT,xv.data()),0);
        ASSERT_GE(H5Dread(yd,H5T_NATIVE_DOUBLE,H5S_ALL,H5S_ALL,H5P_DEFAULT,yv.data()),0);EXPECT_EQ(xv,yv)<<name;
        H5Sclose(xs);H5Sclose(ys);H5Dclose(xd);H5Dclose(yd);
    }
    H5Fclose(x);H5Fclose(y);
}
}
TEST(EngineCheckpoint, MeshOutputContinuesCadenceGroundwaterAndCurrentNodeLIDStores) {
    for(int lid=0;lid<3;++lid)for(int format=1;format<5;++format)for(double boundary:{0.,2.1,3.,7.1}) {
        const auto name="mesh-closed-"+std::to_string(lid)+"-"+std::to_string(format)+"-"+std::to_string(boundary);SCOPED_TRACE(name);
        SurfaceSession a,b;
        ASSERT_NO_THROW(startClosedSpatial(a,name,lid,.7,true,true,false,format));
        ASSERT_NO_THROW(startClosedSpatial(b,name+"-resumed",lid,.7,true,true,false,format));
        while(a.engine.context().current_time<boundary)ASSERT_NO_THROW(step(a.engine));
        const auto checkpoint=root()/(name+".chk");ASSERT_EQ(a.engine.saveCheckpoint(checkpoint.string()),"");
        ASSERT_EQ(b.engine.loadCheckpoint(checkpoint.string()),"");compareMesh(a.engine,b.engine,name+"-restored");
        int steps=0;
        while(a.engine.context().elapsed_ms<a.engine.context().options.totalDurationMs()) {
            ASSERT_NO_THROW(step(a.engine));
            ASSERT_NO_THROW(step(b.engine));
            if(++steps%5==0)compareMesh(a.engine,b.engine,name+"-continued");
        }
        compareMesh(a.engine,b.engine,name+"-final");
        ASSERT_EQ(a.engine.end(),0);ASSERT_EQ(b.engine.end(),0);sameMesh(name,name+"-resumed",format==4?3:5);
    }
}
TEST(EngineCheckpoint, MeshOutputContinuesQueuedSourceHistoryAndPairedAcceptedHistory) {
    for(double cadence:{.7,.31}) {
        const auto name="mesh-history-"+std::to_string(cadence);SurfaceSession a,b;
        twoD::EngineSourceResultHistory history,resumed;
        ASSERT_NO_THROW(startClosedSpatial(a,name,2,cadence,true,true,false,2));
        ASSERT_EQ(a.engine.setEngineSourceResultHistory(history,true),"");
        while(a.engine.context().current_time<7.1) {
            meshWriter(a.engine).enqueueSourceHistory(std::make_shared<twoD::SourceResultHistory>(*history.sourceIntervals()));
            ASSERT_NO_THROW(step(a.engine));
        }
        const auto checkpoint=root()/(name+".chk");ASSERT_EQ(a.engine.saveCheckpoint(checkpoint.string()),"");resumed=history;
        ASSERT_NO_THROW(startClosedSpatial(b,name+"-resumed",2,cadence,true,true,false,2));
        ASSERT_EQ(b.engine.loadCheckpoint(checkpoint.string()),"");ASSERT_EQ(b.engine.resumeEngineSourceResultHistory(resumed),"");
        while(a.engine.context().elapsed_ms<a.engine.context().options.totalDurationMs()) {
            meshWriter(a.engine).enqueueSourceHistory(std::make_shared<twoD::SourceResultHistory>(*history.sourceIntervals()));
            meshWriter(b.engine).enqueueSourceHistory(std::make_shared<twoD::SourceResultHistory>(*resumed.sourceIntervals()));
            ASSERT_NO_THROW(step(a.engine));
            ASSERT_NO_THROW(step(b.engine));sameEngineHistory(history,resumed);
            compareMesh(a.engine,b.engine,name+"-continued");
        }
        ASSERT_EQ(a.engine.end(),0);ASSERT_EQ(b.engine.end(),0);sameMesh(name,name+"-resumed",5);
        twoD::SourceResultHistory left,right;bool available=false;
        ASSERT_EQ(twoD::SourceResultHistory::read((root()/(name+".h5")).string(),left,available),"");ASSERT_TRUE(available);
        ASSERT_EQ(twoD::SourceResultHistory::read((root()/(name+"-resumed.h5")).string(),right,available),"");ASSERT_TRUE(available);
        ASSERT_EQ(left.tables().size(),right.tables().size());
        for(const auto& [n,t]:left.tables())EXPECT_EQ(t.columns,right.tables().at(n).columns)<<n;
    }
}

namespace {
std::string changeMesh(const std::string& original,const std::string& name,int kind) {
    detail::CompletedSourceArchive in(original);std::string magic,model,payload;uint32_t version,sum;
    in(magic,version,model,sum,payload);in.finish();
    const auto at=payload.rfind(std::string("\211HDF\r\n\032\n",8));
    const auto begin=at-sizeof(uint64_t)-sizeof(bool)-sizeof(double)-sizeof(hsize_t)-sizeof(double)-sizeof(uint32_t);
    MeshOutputWire wire;detail::CompletedSourceArchive tail(payload.substr(begin));wire.io(tail);tail.finish();
    switch(kind) {
    case 0:++wire.count;break;
    case 1:wire.due+=.01;break;
    case 2:wire.step*=2;break;
    case 3:wire.groundwater=!wire.groundwater;break;
    case 4:++wire.version;break;
    case 5:wire.image.resize(9);break;
    default: {
        const auto file=root()/(name+"-mutant.h5");write(file,wire.image);
        hid_t h5=H5Fopen(file.string().c_str(),H5F_ACC_RDWR,H5P_DEFAULT);
        if(h5<0)throw std::runtime_error("Cannot edit test HDF5 prefix");
        auto change=[&](const char* path,double value,bool add=false) {
            hid_t d=H5Dopen2(h5,path,H5P_DEFAULT),sp=H5Dget_space(d);
            std::vector<double> v(H5Sget_simple_extent_npoints(sp));H5Dread(d,H5T_NATIVE_DOUBLE,H5S_ALL,H5S_ALL,H5P_DEFAULT,v.data());
            v[0]=add?v[0]+value:value;H5Dwrite(d,H5T_NATIVE_DOUBLE,H5S_ALL,H5S_ALL,H5P_DEFAULT,v.data());H5Sclose(sp);H5Dclose(d);
        };
        switch(kind) {
        case 6:change("time",.001,true);break;
        case 7:change("Mesh2_face_depth",std::numeric_limits<double>::quiet_NaN());break;
        case 8:change("Mesh2_node_x",1,true);break;
        case 9:H5Ldelete(h5,"Mesh2_face_head",H5P_DEFAULT);break;
        case 10:{hid_t d=H5Dopen2(h5,"Mesh2_face_depth",H5P_DEFAULT);hsize_t dims[2]={wire.count+1,2};H5Dset_extent(d,dims);H5Dclose(d);break;}
        case 11:H5Adelete(h5,"Conventions");break;
        case 12:{hid_t space=H5Screate(H5S_SCALAR);hid_t d=H5Dcreate2(h5,"unexpected",H5T_NATIVE_DOUBLE,space,H5P_DEFAULT,H5P_DEFAULT,H5P_DEFAULT);H5Dclose(d);H5Sclose(space);break;}
        case 13:{H5Ldelete(h5,"Mesh2_face_depth",H5P_DEFAULT);hsize_t dims[2]={wire.count,2};hid_t space=H5Screate_simple(2,dims,nullptr);hid_t d=H5Dcreate2(h5,"Mesh2_face_depth",H5T_NATIVE_FLOAT,space,H5P_DEFAULT,H5P_DEFAULT,H5P_DEFAULT);H5Dclose(d);H5Sclose(space);break;}
        case 14:H5Ldelete(h5,"Mesh2_face_depth",H5P_DEFAULT);H5Lcreate_external("absent-external-file.h5","depth",h5,"Mesh2_face_depth",H5P_DEFAULT,H5P_DEFAULT);break;
        case 15:{hid_t g=H5Gcreate2(h5,"source_history",H5P_DEFAULT,H5P_DEFAULT,H5P_DEFAULT);H5Gclose(g);break;}
        case 16:{hid_t g=H5Gcreate2(h5,"source_history",H5P_DEFAULT,H5P_DEFAULT,H5P_DEFAULT);H5Lcreate_external("absent-external-file.h5","history",g,"boundaries",H5P_DEFAULT,H5P_DEFAULT);H5Gclose(g);break;}
        case 17:change("Mesh2_face_gw_bed_elev",1,true);break;
        case 18:change("groundwater_ledger",std::numeric_limits<double>::infinity());break;
        }
        H5Fclose(h5);wire.image=bytes(file);break;
    }
    }
    detail::CompletedSourceArchive out_tail;wire.io(out_tail);std::string output=out_tail.bytes();
    detail::CompletedSourceArchive length;length(output);payload.resize(begin-sizeof(uint64_t));payload+=length.bytes();
    sum=crc(payload);detail::CompletedSourceArchive out;out(magic,version,model,sum,payload);return out.bytes();
}
}
TEST(EngineCheckpoint, MeshOutputMalformedPrefixesPreserveFreshEngineAndPermitRetry) {
    SurfaceSession a;ASSERT_NO_THROW(startClosedSpatial(a,"mesh-invalid-source",2,.31,true,true,false,1));
    while(a.engine.context().current_time<9.1)ASSERT_NO_THROW(step(a.engine));
    const auto good=root()/"mesh-invalid-good.chk";ASSERT_EQ(a.engine.saveCheckpoint(good.string()),"");
    for(int kind=0;kind<19;++kind) {
        const auto name="mesh-invalid-"+std::to_string(kind);SCOPED_TRACE(name);SurfaceSession b;
        ASSERT_NO_THROW(startClosedSpatial(b,name,2,.31,true,true,false,1));
        const auto before=root()/(name+"-before.chk"),bad=root()/(name+"-bad.chk"),output=root()/(name+".h5");
        ASSERT_EQ(b.engine.saveCheckpoint(before.string()),"");const auto saved=bytes(before),header=bytes(output);
        write(bad,changeMesh(bytes(good),name,kind));EXPECT_FALSE(b.engine.loadCheckpoint(bad.string()).empty());
        ASSERT_EQ(b.engine.saveCheckpoint(before.string()),"");EXPECT_EQ(bytes(before),saved);EXPECT_EQ(bytes(output),header);
        for(const auto& file:std::filesystem::directory_iterator(root()))EXPECT_FALSE(file.path().filename().string().starts_with(name+".h5.mesh-part-"));
        ASSERT_EQ(b.engine.loadCheckpoint(good.string()),"");compareMesh(a.engine,b.engine,name+"-retry");ASSERT_EQ(b.engine.end(),0);
    }
    ASSERT_EQ(a.engine.end(),0);
}
TEST(EngineCheckpoint, MeshOutputPublicationAndSpatialFailuresPreserveAllTargetComponents) {
    SurfaceSession a;ASSERT_NO_THROW(startClosedSpatial(a,"mesh-atomic-source",2,.31,true,true,false,2));
    while(a.engine.context().current_time<9.1)ASSERT_NO_THROW(step(a.engine));
    const auto good=root()/"mesh-atomic-good.chk";ASSERT_EQ(a.engine.saveCheckpoint(good.string()),"");
    for(bool spatial:{false,true}) {
        const auto name=std::string("mesh-atomic-")+(spatial?"spatial":"publication");SurfaceSession b;
        ASSERT_NO_THROW(startClosedSpatial(b,name,2,.31,true,true,false,2));
        const auto before=root()/(name+"-before.chk"),output=root()/(name+".h5");
        ASSERT_EQ(b.engine.saveCheckpoint(before.string()),"");const auto saved=bytes(before),header=bytes(output);
        auto checkpoint=good;
        if(spatial) {
            detail::CompletedSourceArchive in(bytes(good));std::string magic,model,payload;uint32_t version,sum;in(magic,version,model,sum,payload);in.finish();
            auto surface=engineSurface(bytes(good)),body=surfacePayload(surface);const std::string tag="OpenSWMM CPU surface/source boundary";
            auto cpu=nested(body,tag).second;cpu.back()^=1;surface=withSurfacePayload(surface,replaceNested(body,tag,cpu));
            payload=replaceNested(payload,"OpenSWMM joint DW surface/source boundary",surface);sum=crc(payload);
            detail::CompletedSourceArchive out;out(magic,version,model,sum,payload);checkpoint=root()/(name+"-bad.chk");write(checkpoint,out.bytes());
        } else std::filesystem::permissions(output,std::filesystem::perms::owner_read);
        EXPECT_FALSE(b.engine.loadCheckpoint(checkpoint.string()).empty());
        ASSERT_EQ(b.engine.saveCheckpoint(before.string()),"");EXPECT_EQ(bytes(before),saved);EXPECT_EQ(bytes(output),header);
        std::filesystem::permissions(output,std::filesystem::perms::owner_read|std::filesystem::perms::owner_write);
        for(const auto& file:std::filesystem::directory_iterator(root()))EXPECT_FALSE(file.path().filename().string().starts_with(name+".h5.mesh-part-"));
        ASSERT_EQ(b.engine.loadCheckpoint(good.string()),"");compareMesh(a.engine,b.engine,name+"-retry");ASSERT_EQ(b.engine.end(),0);
    }
    ASSERT_EQ(a.engine.end(),0);
}
TEST(EngineCheckpoint, MeshOutputProtectsExistingDestinationsAndExclusiveStart) {
    SurfaceSession authored;ASSERT_NO_THROW(startClosedSpatial(authored,"mesh-guard",0,.7,false,false,false,1));
    ASSERT_EQ(authored.engine.end(),0);ASSERT_EQ(authored.engine.close(),0);
    const auto inp=root()/"mesh-guard.inp",destination=root()/"mesh-guard.h5";write(destination,"existing results");
    SWMMEngine e;ASSERT_EQ(e.open(inp.string().c_str(),nullptr,nullptr),0);EXPECT_FALSE(e.prepareSurfaceOutputCheckpointing().empty());
    ASSERT_EQ(e.initialize(),0);EXPECT_FALSE(e.prepareSurfaceOutputCheckpointing().empty());EXPECT_EQ(bytes(destination),"existing results");
    std::filesystem::remove(destination);ASSERT_EQ(e.prepareSurfaceOutputCheckpointing(),"");
    write(destination,"intervening creator");EXPECT_NE(e.start(0),0);EXPECT_EQ(bytes(destination),"intervening creator");ASSERT_EQ(e.close(),0);
    SWMMEngine binary;ASSERT_EQ(binary.open(inp.string().c_str(),nullptr,(root()/"mesh-guard.out").string().c_str()),0);
    ASSERT_EQ(binary.initialize(),0);EXPECT_FALSE(binary.prepareSurfaceOutputCheckpointing().empty());EXPECT_FALSE(binary.prepareOutputCheckpointing().empty());
    ASSERT_EQ(binary.close(),0);
}

TEST(EngineCheckpoint, MeshOutputMappedVectorRichardsAndBackwaterContinueExactly) {
    for(bool spatial:{false,true}) {
        const auto name=std::string("mesh-vector-")+(spatial?"spatial":"mesh-owned");SurfaceSession a,b;
        ASSERT_NO_THROW(a.start(name,3,spatial?0:1,1,6,true,spatial,spatial,spatial?2:-1,false,1));
        ASSERT_NO_THROW(b.start(name+"-resumed",3,spatial?0:1,1,6,true,spatial,spatial,spatial?2:-1,false,1));
        while(a.engine.context().current_time<43.1)ASSERT_NO_THROW(step(a.engine));
        const auto checkpoint=root()/(name+".chk");ASSERT_EQ(a.engine.saveCheckpoint(checkpoint.string()),"");
        ASSERT_EQ(b.engine.loadCheckpoint(checkpoint.string()),"");compareMesh(a.engine,b.engine,name+"-restored");
        int steps=0;
        while(a.engine.context().elapsed_ms<a.engine.context().options.totalDurationMs()) {
            ASSERT_NO_THROW(step(a.engine));
            ASSERT_NO_THROW(step(b.engine));
            if(++steps%19==0)compareMesh(a.engine,b.engine,name+"-continued");
        }
        compareMesh(a.engine,b.engine,name+"-final");ASSERT_EQ(a.engine.end(),0);ASSERT_EQ(b.engine.end(),0);
        sameMesh(name,name+"-resumed",40);
    }
}

TEST(EngineCheckpoint, MeshOutputRejectsChangedSourceHistoryOwnershipBeforePublication) {
    SurfaceSession a;twoD::EngineSourceResultHistory history;
    ASSERT_NO_THROW(startClosedSpatial(a,"mesh-source-identity",2,.31,true,true,false,1));
    ASSERT_EQ(a.engine.setEngineSourceResultHistory(history,true),"");
    while(a.engine.context().current_time<7.1) {
        meshWriter(a.engine).enqueueSourceHistory(std::make_shared<twoD::SourceResultHistory>(*history.sourceIntervals()));
        ASSERT_NO_THROW(step(a.engine));
    }
    const auto good=root()/"mesh-source-identity.chk";ASSERT_EQ(a.engine.saveCheckpoint(good.string()),"");
    detail::CompletedSourceArchive in(bytes(good));std::string magic,model,payload;uint32_t version,sum;in(magic,version,model,sum,payload);in.finish();
    const auto at=payload.rfind(std::string("\211HDF\r\n\032\n",8));
    const auto begin=at-sizeof(uint64_t)-sizeof(bool)-sizeof(double)-sizeof(hsize_t)-sizeof(double)-sizeof(uint32_t);
    MeshOutputWire wire;detail::CompletedSourceArchive tail(payload.substr(begin));wire.io(tail);tail.finish();
    const auto image=root()/"mesh-source-identity-mutant.h5";write(image,wire.image);
    hid_t file=H5Fopen(image.string().c_str(),H5F_ACC_RDWR,H5P_DEFAULT);
    ASSERT_GE(file,0);hid_t ds=H5Dopen2(file,"source_history/footprints/area",H5P_DEFAULT);ASSERT_GE(ds,0);
    hid_t space=H5Dget_space(ds);std::vector<double> areas(H5Sget_simple_extent_npoints(space));
    ASSERT_GE(H5Dread(ds,H5T_NATIVE_DOUBLE,H5S_ALL,H5S_ALL,H5P_DEFAULT,areas.data()),0);areas[0]+=.01;
    ASSERT_GE(H5Dwrite(ds,H5T_NATIVE_DOUBLE,H5S_ALL,H5S_ALL,H5P_DEFAULT,areas.data()),0);H5Sclose(space);H5Dclose(ds);H5Fclose(file);
    wire.image=bytes(image);detail::CompletedSourceArchive component;wire.io(component);std::string output=component.bytes();
    detail::CompletedSourceArchive length;length(output);payload.resize(begin-sizeof(uint64_t));payload+=length.bytes();sum=crc(payload);
    detail::CompletedSourceArchive out;out(magic,version,model,sum,payload);const auto bad=root()/"mesh-source-identity-bad.chk";write(bad,out.bytes());
    SurfaceSession b;ASSERT_NO_THROW(startClosedSpatial(b,"mesh-source-identity-target",2,.31,true,true,false,1));
    const auto before=root()/"mesh-source-identity-before.chk",destination=root()/"mesh-source-identity-target.h5";
    ASSERT_EQ(b.engine.saveCheckpoint(before.string()),"");const auto saved=bytes(before),header=bytes(destination);
    EXPECT_FALSE(b.engine.loadCheckpoint(bad.string()).empty());ASSERT_EQ(b.engine.saveCheckpoint(before.string()),"");
    EXPECT_EQ(bytes(before),saved);EXPECT_EQ(bytes(destination),header);ASSERT_EQ(b.engine.loadCheckpoint(good.string()),"");
    ASSERT_EQ(a.engine.end(),0);ASSERT_EQ(b.engine.end(),0);
}

TEST(EngineCheckpoint, CombinedOutputContinuesBothReportClocksAndAveragingWindows) {
    for(int lid=0;lid<3;++lid)for(int format:{1,2,3,4})for(double boundary:{0.,2.1,3.,7.1}) {
        const auto name="combined-closed-"+std::to_string(lid)+"-"+std::to_string(format)+"-"+std::to_string(boundary);SCOPED_TRACE(name);
        SurfaceSession a,b;const bool average=format%2==0;
        ASSERT_NO_THROW(startClosedSpatial(a,name,lid,.7,true,true,true,format,average));
        ASSERT_NO_THROW(startClosedSpatial(b,name+"-resumed",lid,.7,true,true,true,format,average));
        while(a.engine.context().current_time<boundary)ASSERT_NO_THROW(step(a.engine));
        const auto checkpoint=root()/(name+".chk");ASSERT_EQ(a.engine.saveCheckpoint(checkpoint.string()),"");
        ASSERT_EQ(b.engine.loadCheckpoint(checkpoint.string()),"");compareMesh(a.engine,b.engine,name+"-restored");
        int steps=0;
        while(a.engine.context().elapsed_ms<a.engine.context().options.totalDurationMs()) {
            ASSERT_NO_THROW(step(a.engine));
            ASSERT_NO_THROW(step(b.engine));
            if(++steps%7==0)compareMesh(a.engine,b.engine,name+"-continued");
        }
        compareMesh(a.engine,b.engine,name+"-final");ASSERT_EQ(a.engine.end(),0);ASSERT_EQ(b.engine.end(),0);
        sameBinary(name,name+"-resumed",format==4?6:10);sameMesh(name,name+"-resumed",format==4?3:5);
    }
}
TEST(EngineCheckpoint, CombinedOutputContinuesPairedHistoriesAndVectorRichardsBackwater) {
    for(int scenario=0;scenario<4;++scenario) {
        const auto name="combined-paired-"+std::to_string(scenario);SurfaceSession a,b;
        const bool spatial=scenario!=2;const double cadence=scenario==0?.7:.31;
        if(scenario<2) {
            ASSERT_NO_THROW(startClosedSpatial(a,name,2,cadence,true,true,true,2,true));
            ASSERT_NO_THROW(startClosedSpatial(b,name+"-resumed",2,cadence,true,true,true,2,true));
        } else {
            ASSERT_NO_THROW(a.start(name,3,spatial?0:1,1,6,true,spatial,spatial,spatial?2:-1,true,2,true));
            ASSERT_NO_THROW(b.start(name+"-resumed",3,spatial?0:1,1,6,true,spatial,spatial,spatial?2:-1,true,2,true));
        }
        twoD::EngineSourceResultHistory history,resumed;
        if(spatial)ASSERT_EQ(a.engine.setEngineSourceResultHistory(history,true),"");
        while(a.engine.context().current_time<(scenario<2?7.1:43.1)) {
            if(spatial)meshWriter(a.engine).enqueueSourceHistory(std::make_shared<twoD::SourceResultHistory>(*history.sourceIntervals()));
            ASSERT_NO_THROW(step(a.engine));
        }
        const auto checkpoint=root()/(name+".chk");ASSERT_EQ(a.engine.saveCheckpoint(checkpoint.string()),"");
        ASSERT_EQ(b.engine.loadCheckpoint(checkpoint.string()),"");
        if(spatial) {resumed=history;ASSERT_EQ(b.engine.resumeEngineSourceResultHistory(resumed),"");}
        int steps=0;
        while(a.engine.context().elapsed_ms<a.engine.context().options.totalDurationMs()) {
            if(spatial) {
                meshWriter(a.engine).enqueueSourceHistory(std::make_shared<twoD::SourceResultHistory>(*history.sourceIntervals()));
                meshWriter(b.engine).enqueueSourceHistory(std::make_shared<twoD::SourceResultHistory>(*resumed.sourceIntervals()));
            }
            ASSERT_NO_THROW(step(a.engine));
            ASSERT_NO_THROW(step(b.engine));
            if(++steps%19==0) {compareMesh(a.engine,b.engine,name+"-continued");if(spatial)sameEngineHistory(history,resumed);}
        }
        compareMesh(a.engine,b.engine,name+"-final");if(spatial)sameEngineHistory(history,resumed);
        ASSERT_EQ(a.engine.end(),0);ASSERT_EQ(b.engine.end(),0);
        sameBinary(name,name+"-resumed",scenario<2?10:80);sameMesh(name,name+"-resumed",scenario<2?5:40);
        if(spatial) {
            twoD::SourceResultHistory x,y;bool available=false;
            ASSERT_EQ(twoD::SourceResultHistory::read((root()/(name+".h5")).string(),x,available),"");ASSERT_TRUE(available);
            ASSERT_EQ(twoD::SourceResultHistory::read((root()/(name+"-resumed.h5")).string(),y,available),"");ASSERT_TRUE(available);
            for(const auto& [n,t]:x.tables())EXPECT_EQ(t.columns,y.tables().at(n).columns)<<n;
        }
    }
}
namespace {
std::string changeCombinedBinary(const std::string& original,int mutation) {
    // Binary mutation helper expects the binary suffix; temporarily remove and
    // then restore the separately length-prefixed mesh record.
    const auto physical=meshPhysical(original);
    detail::CompletedSourceArchive x(original);std::string magic,model,payload;uint32_t version,sum;
    x(magic,version,model,sum,payload);x.finish();
    detail::CompletedSourceArchive p(physical);std::string m,md,body;uint32_t v,c;p(m,v,md,c,body);p.finish();
    const auto mesh_suffix=payload.substr(body.size());
    detail::CompletedSourceArchive changed(changeBinary(physical,mutation));changed(m,v,md,c,body);changed.finish();
    body+=mesh_suffix;v+=6;c=crc(body);detail::CompletedSourceArchive out;out(m,v,md,c,body);return out.bytes();
}
void noOutputStaging(const std::string& name) {
    for(const auto& f:std::filesystem::directory_iterator(root())) {
        const auto path=f.path().filename().string();
        if(path.starts_with(name+".out.") || path.starts_with(name+".h5."))EXPECT_FALSE(
            path.find("report-part-")!=std::string::npos || path.find("mesh-part-")!=std::string::npos || path.find("checkpoint-backup-")!=std::string::npos)<<path;
    }
}
}
TEST(EngineCheckpoint, CombinedOutputRejectsEitherMalformedPrefixWithoutPublishingTheOther) {
    SurfaceSession a;ASSERT_NO_THROW(startClosedSpatial(a,"combined-invalid-source",2,.31,true,true,true,2,true));
    while(a.engine.context().current_time<9.1)ASSERT_NO_THROW(step(a.engine));
    const auto good=root()/"combined-invalid-good.chk";ASSERT_EQ(a.engine.saveCheckpoint(good.string()),"");
    for(int mutation=0;mutation<28;++mutation) {
        const auto name="combined-invalid-"+std::to_string(mutation);SCOPED_TRACE(name);SurfaceSession b;
        ASSERT_NO_THROW(startClosedSpatial(b,name,2,.31,true,true,true,2,true));
        const auto before=root()/(name+"-before.chk"),bad=root()/(name+"-bad.chk");ASSERT_EQ(b.engine.saveCheckpoint(before.string()),"");
        const auto saved=bytes(before),binary=bytes(root()/(name+".out")),mesh=bytes(root()/(name+".h5"));
        write(bad,mutation<9?changeCombinedBinary(bytes(good),mutation):changeMesh(bytes(good),name,mutation-9));
        EXPECT_FALSE(b.engine.loadCheckpoint(bad.string()).empty());ASSERT_EQ(b.engine.saveCheckpoint(before.string()),"");
        EXPECT_EQ(bytes(before),saved);EXPECT_EQ(bytes(root()/(name+".out")),binary);EXPECT_EQ(bytes(root()/(name+".h5")),mesh);
        noOutputStaging(name);ASSERT_EQ(b.engine.loadCheckpoint(good.string()),"");compareMesh(a.engine,b.engine,name+"-retry");ASSERT_EQ(b.engine.end(),0);
    }
    ASSERT_EQ(a.engine.end(),0);
}
TEST(EngineCheckpoint, CombinedOutputPublicationAndSpatialFailuresPreserveBothWriters) {
    SurfaceSession a;ASSERT_NO_THROW(startClosedSpatial(a,"combined-atomic-source",2,.31,true,true,true,2));
    while(a.engine.context().current_time<9.1)ASSERT_NO_THROW(step(a.engine));
    const auto good=root()/"combined-atomic-good.chk";ASSERT_EQ(a.engine.saveCheckpoint(good.string()),"");
    for(int failure=0;failure<5;++failure) {
        const auto name="combined-atomic-"+std::to_string(failure);SurfaceSession b;
        ASSERT_NO_THROW(startClosedSpatial(b,name,2,.31,true,true,true,2));
        const auto before=root()/(name+"-before.chk"),binary=root()/(name+".out"),mesh=root()/(name+".h5");
        ASSERT_EQ(b.engine.saveCheckpoint(before.string()),"");const auto saved=bytes(before),bh=bytes(binary),mh=bytes(mesh);auto checkpoint=good;
        if(failure<2)std::filesystem::permissions(failure?mesh:binary,std::filesystem::perms::owner_read);
        else if(failure==2) {
            detail::CompletedSourceArchive in(bytes(good));std::string magic,model,payload;uint32_t version,sum;in(magic,version,model,sum,payload);in.finish();
            auto surface=engineSurface(bytes(good)),body=surfacePayload(surface);const std::string tag="OpenSWMM CPU surface/source boundary";
            auto cpu=nested(body,tag).second;cpu.back()^=1;surface=withSurfacePayload(surface,replaceNested(body,tag,cpu));
            payload=replaceNested(payload,"OpenSWMM joint DW surface/source boundary",surface);sum=crc(payload);
            detail::CompletedSourceArchive out;out(magic,version,model,sum,payload);checkpoint=root()/(name+"-bad.chk");write(checkpoint,out.bytes());
        }
        const auto changed=failure==3?binary:mesh,original=root()/(name+"-original-header");
        if(failure>=3) {std::filesystem::rename(changed,original);write(changed,failure==3?bh:mh);}
        EXPECT_FALSE(b.engine.loadCheckpoint(checkpoint.string()).empty());ASSERT_EQ(b.engine.saveCheckpoint(before.string()),"");
        EXPECT_EQ(bytes(before),saved);EXPECT_EQ(bytes(binary),bh);EXPECT_EQ(bytes(mesh),mh);noOutputStaging(name);
        if(failure>=3) {std::filesystem::remove(changed);std::filesystem::rename(original,changed);}
        for(const auto& path:{binary,mesh})std::filesystem::permissions(path,std::filesystem::perms::owner_read|std::filesystem::perms::owner_write);
        ASSERT_EQ(b.engine.loadCheckpoint(good.string()),"");compareMesh(a.engine,b.engine,name+"-retry");ASSERT_EQ(b.engine.end(),0);
    }
    ASSERT_EQ(a.engine.end(),0);
}
TEST(EngineCheckpoint, OutputPublicationRollsBackAnEarlierRenameAndProtectsAliasedPaths) {
    const auto a=root()/"publication-a",b=root()/"publication-b",sa=root()/"publication-a-stage",sb=root()/"publication-b-stage";
    write(a,"header-a");write(b,"header-b");write(sa,"prefix-a");write(sb,"prefix-b");
    {
        io::CheckpointOutputPublication publication({{sa,a,"header-a"},{sb,b,"header-b"}});
        // Simulate loss of the second prepared staging file after preparation.
        std::filesystem::remove(sb);EXPECT_THROW(publication.publish(),std::runtime_error);
        EXPECT_EQ(bytes(a),"header-a");EXPECT_EQ(bytes(b),"header-b");
    }
    write(sa,"prefix-a");write(sb,"prefix-b");
    EXPECT_THROW((io::CheckpointOutputPublication({{sa,a,"header-a"},{sb,a,"header-a"}})),std::runtime_error);
    const auto alias=root()/"publication-hardlink";std::filesystem::remove(alias);std::filesystem::create_hard_link(a,alias);
    EXPECT_THROW((io::CheckpointOutputPublication({{sa,a,"header-a"},{sb,alias,"header-a"}})),std::runtime_error);
    std::filesystem::remove(alias);
    {
        io::CheckpointOutputPublication publication({{sa,a,"header-a"},{sb,b,"header-b"}});publication.publish();
    }
    EXPECT_EQ(bytes(a),"prefix-a");EXPECT_EQ(bytes(b),"prefix-b");
    for(const auto& f:std::filesystem::directory_iterator(root()))EXPECT_EQ(f.path().filename().string().find("publication-a.checkpoint-backup-"),std::string::npos);
}
TEST(EngineCheckpoint, CombinedOutputPreparationProtectsLifecycleAndBothFreshDestinations) {
    SurfaceSession valid;ASSERT_NO_THROW(startClosedSpatial(valid,"combined-guard",2,.7,true,true,true,2));
    EXPECT_FALSE(valid.engine.prepareCombinedOutputCheckpointing().empty());
    SurfaceSession mesh;ASSERT_NO_THROW(startClosedSpatial(mesh,"combined-mesh-only",2,.7,true,true,false,2));
    EXPECT_FALSE(mesh.engine.prepareCombinedOutputCheckpointing().empty());
    SurfaceSession binary;ASSERT_NO_THROW(startClosedSpatial(binary,"combined-binary-only",2,.7,true,true,true));
    EXPECT_FALSE(binary.engine.prepareCombinedOutputCheckpointing().empty());
    const auto geometry=valid.engine.surfaceRouter2D().mesh();
    ASSERT_EQ(valid.engine.end(),0);ASSERT_EQ(valid.engine.close(),0);
    const auto inp=root()/"combined-guard.inp",destination=root()/"combined-guard.out",h5=root()/"combined-guard.h5";
    SWMMEngine e;ASSERT_EQ(e.open(inp.string().c_str(),nullptr,destination.string().c_str()),0);
    e.surfaceRouter2D().mesh()=geometry;
    const auto binary_header=bytes(destination),mesh_header=bytes(h5);
    EXPECT_FALSE(e.prepareCombinedOutputCheckpointing().empty());ASSERT_EQ(e.initialize(),0);
    EXPECT_FALSE(e.prepareCombinedOutputCheckpointing().empty());EXPECT_EQ(bytes(destination),binary_header);EXPECT_EQ(bytes(h5),mesh_header);
    std::filesystem::remove(destination);EXPECT_FALSE(e.prepareCombinedOutputCheckpointing().empty());EXPECT_EQ(bytes(h5),mesh_header);
    std::filesystem::remove(h5);ASSERT_EQ(e.prepareCombinedOutputCheckpointing(),"");
    write(h5,"intervening creator");EXPECT_NE(e.start(1),0);EXPECT_EQ(bytes(h5),"intervening creator");ASSERT_EQ(e.close(),0);
}

#endif


#ifdef OPENSWMM_HAS_2D
namespace {
void startProvider(SurfaceSession& s,const std::string& name,int lid=0,int closure=0,bool output=false,int mesh_output=0,
                   const std::string& identity="mesh water driver revision 1") {
    s.start(name,lid,1,closure,0,true,false,false,-1,output,mesh_output,output,identity);
}
int providerFrame(SWMMEngine& e,double until=9.25,double incoming=.01) {
    const SWMM_CouplingSource sources[]={
        {sizeof(SWMM_CouplingSource),"in",0,0,incoming,0,until,0,nullptr,nullptr},
        {sizeof(SWMM_CouplingSource),"out",0,1,-2.,0,until,0,nullptr,nullptr},
        {sizeof(SWMM_CouplingSource),"in",1,0,.02,0,until,0,nullptr,nullptr},
        {sizeof(SWMM_CouplingSource),"out",1,1,-2.,0,until,0,nullptr,nullptr}};
    const SWMM_CouplingForcing weather[]={
        {sizeof(SWMM_CouplingForcing),1,-1,1,.001,until,0,nullptr},
        {sizeof(SWMM_CouplingForcing),1,0,1,.002,7.75,0,nullptr},
        {sizeof(SWMM_CouplingForcing),2,-1,2,.0001,until,0,nullptr},
        {sizeof(SWMM_CouplingForcing),3,0,1,.02,until,0,nullptr}};
    const SWMM_CouplingFrame frame{sizeof(frame),1,"water",4,sources,0,nullptr,4,weather,0,nullptr};
    return swmm_coupling_apply_frame(reinterpret_cast<SWMM_Engine>(&e),&frame);
}
void until(SWMMEngine& e,double target) {while(e.context().current_time<target-1e-12)step(e);}
std::string changeProviderRecord(const std::string& original,int mutation) {
    using A=detail::CompletedSourceArchive;
    A engine(original);std::string em,model,payload;uint32_t ev,sum;engine(em,ev,model,sum,payload);engine.finish();
    auto surface=engineSurface(original),surface_data=surfacePayload(surface);
    if(mutation==6) {
        surface_data[nested(surface_data,"OpenSWMM CPU surface/source boundary").first-1]=0;
        payload=replaceNested(payload,"OpenSWMM joint DW surface/source boundary",withSurfacePayload(surface,surface_data));
        sum=crc(payload);A result;result(em,ev,model,sum,payload);return result.bytes();
    }
    auto cpu=nested(surface_data,"OpenSWMM CPU surface/source boundary").second;
    A ca(cpu);std::string cm,cp;uint32_t cv,endian,long_bytes;ca(cm,cv,endian,long_bytes,cp,sum);ca.finish();
    auto source=nested(cp,"OpenSWMM source boundary").second;
    A sa(source);std::string sm,source_model,sp;uint32_t sv,se,sl,sd;
    sa(sm,sv,se,sl,sd,source_model,sp,sum);sa.finish();
    auto provider=nested(sp,"OpenSWMM Runtime Water").second;
    A pa(provider);std::string pm,identity,pp;uint32_t pv,pe,pi,pr;std::vector<bool> cells;double time;
    pa(pm,pv,pe,pi,pr,identity,cells,time,pp,sum);pa.finish();
    if(mutation==0)identity+="changed";
    if(mutation==1)time+=.125;
    if(mutation==2)cells[0]=false;
    if(mutation==3)pp.pop_back();
    if(mutation==4)pv++;
    if(mutation>=7) {
        twoD::RuntimeSources s,g;twoD::RuntimeForcings f;
        const auto receipt=[](A& a,twoD::RuntimeReceipt& r){a(r.requested,r.applied,r.water_in,r.water_out,r.last_flow,r.last_dt);};
        const auto fields=[&](A& a) {
            const auto row=[&](auto& r){a(r.id,r.owner,r.cell,r.flow,r.until,r.enabled);receipt(a,r.receipt);};
            a.sequence(s.rows,row);a.sequence(g.rows,row);
            a.sequence(f.rows,[&](auto& r){a(r.channel,r.cell,r.mode,r.rate,r.until,r.enabled,r.owner);receipt(a,r.receipt);});
        };
        A read(pp);fields(read);read.finish();s.rows[0].receipt.water_in+=.001;s.rows[0].receipt.applied+=.001;
        if(mutation==8)for(auto& r:s.rows) {
            r.receipt.water_in=1e308;r.receipt.applied=r.receipt.water_in-r.receipt.water_out;
        }
        A updated;fields(updated);pp=updated.bytes();
    }
    if(mutation==5)provider.pop_back();
    else {sum=crc(pp);A po;po(pm,pv,pe,pi,pr,identity,cells,time,pp,sum);provider=po.bytes();}
    sp=replaceNested(sp,"OpenSWMM Runtime Water",provider);sum=crc(sp);A so;so(sm,sv,se,sl,sd,source_model,sp,sum);
    cp=replaceNested(cp,"OpenSWMM source boundary",so.bytes());sum=crc(cp);A co;co(cm,cv,endian,long_bytes,cp,sum);
    surface_data=replaceNested(surface_data,"OpenSWMM CPU surface/source boundary",co.bytes());
    surface=withSurfacePayload(surface,surface_data);
    payload=replaceNested(payload,"OpenSWMM joint DW surface/source boundary",surface);sum=crc(payload);
    A result;result(em,ev,model,sum,payload);return result.bytes();
}
}

TEST(EngineCheckpoint, RuntimeWaterWholeSessionContinuesStoresAndNodeLidProfiles) {
    for(int lid:{0,1,2})for(int closure:{0,1,2}) {
        const auto name="provider-session-"+std::to_string(lid)+"-"+std::to_string(closure);SCOPED_TRACE(name);
        SurfaceSession a,b;
    ASSERT_NO_THROW(startProvider(a,name+"-a",lid,closure));
    ASSERT_NO_THROW(startProvider(b,name+"-b",lid,closure));
        ASSERT_EQ(providerFrame(a.engine),SWMM_OK);
    ASSERT_NO_THROW(until(a.engine,3.1));
        ASSERT_LE(a.stage.completedEnd(),a.engine.context().current_time);
        auto checkpoint=root()/(name+".chk");ASSERT_EQ(a.engine.saveCheckpoint(checkpoint.string()),"");
        ASSERT_EQ(b.engine.loadCheckpoint(checkpoint.string()),"");EXPECT_TRUE(b.engine.context().runtime_coupling_used);
        compare(a.engine,b.engine,name+"-restored");
        while(a.engine.context().current_time<20.125) {
            ASSERT_NO_THROW(step(a.engine));
    ASSERT_NO_THROW(step(b.engine));
            compare(a.engine,b.engine,name+"-step");
        }
        auto& router=a.engine.surfaceRouter2D();EXPECT_FALSE(router.runtimeActive());
        const auto* in=router.runtimeSources().find("provider/water/in",0);ASSERT_NE(in,nullptr);
        EXPECT_NEAR(in->receipt.requested,.01*9.25,1e-12);EXPECT_GT(in->receipt.water_in,0);
        const auto* gw=router.subsurface().runtimeSources().find("provider/water/in",0);ASSERT_NE(gw,nullptr);
        EXPECT_NEAR(gw->receipt.requested,.02*9.25,1e-12);
        EXPECT_GT(router.state().runtime_forcings.find(3,0)->receipt.requested,0);
        EXPECT_GT(router.runtimeSources().find("provider/water/out",1)->receipt.water_out,0);
        EXPECT_LE(router.runtimeSources().find("provider/water/out",1)->receipt.water_out,2*9.25);
    }
}
TEST(EngineCheckpoint, RuntimeWaterExpiryIsAnExactPhysicalBarrierAndInactiveHistoryRestores) {
    SurfaceSession a,b;
    ASSERT_NO_THROW(startProvider(a,"provider-expiry-a"));
    ASSERT_NO_THROW(startProvider(b,"provider-expiry-b"));
    ASSERT_EQ(providerFrame(a.engine),SWMM_OK);
    ASSERT_NO_THROW(until(a.engine,7.75));
    EXPECT_NEAR(a.engine.context().current_time,7.75,1e-12);EXPECT_NEAR(a.stage.completedEnd(),7.75,1e-12);
    auto& forcing=a.engine.surfaceRouter2D().state().runtime_forcings;
    EXPECT_FALSE(forcing.find(1,0)->enabled);ASSERT_NE(forcing.get(1,0),nullptr);EXPECT_EQ(forcing.get(1,0)->cell,-1);
    ASSERT_NO_THROW(until(a.engine,9.25));EXPECT_NEAR(a.engine.context().current_time,9.25,1e-12);EXPECT_NEAR(a.stage.completedEnd(),9.25,1e-12);
    EXPECT_FALSE(a.engine.surfaceRouter2D().runtimeActive());EXPECT_TRUE(a.engine.context().runtime_coupling_used);
    ASSERT_NO_THROW(step(a.engine));
    ASSERT_LT(a.stage.completedEnd(),a.engine.context().current_time);
    auto checkpoint=root()/"provider-expired.chk";ASSERT_EQ(a.engine.saveCheckpoint(checkpoint.string()),"");
    ASSERT_EQ(b.engine.loadCheckpoint(checkpoint.string()),"");EXPECT_TRUE(b.engine.context().runtime_coupling_used);
    EXPECT_FALSE(b.engine.surfaceRouter2D().runtimeActive());compare(a.engine,b.engine,"provider-expired");
    const auto before=root()/"provider-inactive-before.chk";ASSERT_EQ(b.engine.saveCheckpoint(before.string()),"");
    const auto saved=bytes(before);const double prior=b.stage.completedEnd();
    const SWMM_CouplingBoundary boundary{sizeof(boundary),0,0,0,1,1.,0,nullptr};
    const SWMM_CouplingFrame unsupported{sizeof(unsupported),1,"water",0,nullptr,1,&boundary,0,nullptr,0,nullptr};
    EXPECT_EQ(swmm_coupling_apply_frame(reinterpret_cast<SWMM_Engine>(&b.engine),&unsupported),SWMM_ERR_PLUGIN);
    EXPECT_EQ(b.stage.completedEnd(),prior);ASSERT_EQ(b.engine.saveCheckpoint(before.string()),"");EXPECT_EQ(bytes(before),saved);
    for(auto* engine:{&a.engine,&b.engine})ASSERT_EQ(swmm_coupling_set_source(reinterpret_cast<SWMM_Engine>(engine),
        "future",0,0,.01,0,nullptr,nullptr,0,20.125),SWMM_OK);
    EXPECT_EQ(a.stage.completedEnd(),a.engine.context().current_time);compare(a.engine,b.engine,"provider-future-barrier");
    ASSERT_NO_THROW(step(a.engine));
    ASSERT_NO_THROW(step(b.engine));compare(a.engine,b.engine,"provider-native-fallback");
    const auto& router=b.engine.surfaceRouter2D();EXPECT_EQ(router.state().runtime_forcings.rate(1,0,router.state().rainfall[0]),router.state().rainfall[0]);
}
TEST(EngineCheckpoint, RuntimeWaterFutureFramesPreservePastReceiptsAndRejectBeforeFlushing) {
    SurfaceSession a,b;
    ASSERT_NO_THROW(startProvider(a,"provider-update-a"));
    ASSERT_NO_THROW(startProvider(b,"provider-update-b"));
    ASSERT_EQ(providerFrame(a.engine),SWMM_OK);
    ASSERT_NO_THROW(until(a.engine,4.2));
    auto checkpoint=root()/"provider-before-update.chk";ASSERT_EQ(a.engine.saveCheckpoint(checkpoint.string()),"");ASSERT_EQ(b.engine.loadCheckpoint(checkpoint.string()),"");
    const auto before=bytes(checkpoint);const double source_end=b.stage.completedEnd();
    const SWMM_CouplingBoundary boundary{sizeof(boundary),0,0,0,1,1.,0,nullptr};
    const SWMM_CouplingFrame bad{sizeof(bad),1,"water",0,nullptr,1,&boundary,0,nullptr,0,nullptr};
    EXPECT_EQ(swmm_coupling_apply_frame(reinterpret_cast<SWMM_Engine>(&b.engine),&bad),SWMM_ERR_PLUGIN);
    EXPECT_EQ(b.stage.completedEnd(),source_end);ASSERT_EQ(b.engine.saveCheckpoint(checkpoint.string()),"");EXPECT_EQ(bytes(checkpoint),before);
    const SWMM_CouplingForcing unsupported{sizeof(unsupported),4,0,1,0,0,0,nullptr};
    const SWMM_CouplingFrame quality{sizeof(quality),1,"water",0,nullptr,0,nullptr,1,&unsupported,0,nullptr};
    EXPECT_EQ(swmm_coupling_apply_frame(reinterpret_cast<SWMM_Engine>(&b.engine),&quality),SWMM_ERR_PLUGIN);
    EXPECT_EQ(b.stage.completedEnd(),source_end);ASSERT_EQ(b.engine.saveCheckpoint(checkpoint.string()),"");EXPECT_EQ(bytes(checkpoint),before);
    const double changed_at=a.engine.context().current_time;
    ASSERT_EQ(providerFrame(a.engine,12.5,.03),SWMM_OK);ASSERT_EQ(providerFrame(b.engine,12.5,.03),SWMM_OK);
    EXPECT_EQ(a.stage.completedEnd(),changed_at);compare(a.engine,b.engine,"provider-changed");
    ASSERT_NO_THROW(until(a.engine,12.5));
    ASSERT_NO_THROW(until(b.engine,12.5));compare(a.engine,b.engine,"provider-changed-expired");
    const auto* row=b.engine.surfaceRouter2D().runtimeSources().find("provider/water/in",0);
    EXPECT_NEAR(row->receipt.requested,.01*changed_at+.03*(12.5-changed_at),1e-12);
    EXPECT_NEAR(a.stage.completedEnd(),12.5,1e-12);
}
TEST(EngineCheckpoint, RuntimeWaterPairedInfiltrationReplacesRateAndSharesFiniteWater) {
    SurfaceSession a;
    ASSERT_NO_THROW(startProvider(a,"provider-infiltration"));
    auto& router=a.engine.surfaceRouter2D();
    const double natural=router.subsurface().sourceInfiltrationCapacity(0,router.state().depth[0],.7);
    const SWMM_CouplingForcing rows[]={
        {sizeof(SWMM_CouplingForcing),1,-1,1,0,0,0,nullptr},
        {sizeof(SWMM_CouplingForcing),2,-1,1,1,0,0,nullptr},
        {sizeof(SWMM_CouplingForcing),3,0,1,1,0,0,nullptr},
        {sizeof(SWMM_CouplingForcing),3,1,1,0,0,0,nullptr}};
    const SWMM_CouplingFrame f{sizeof(f),1,"soil",0,nullptr,0,nullptr,4,rows,0,nullptr};
    ASSERT_EQ(swmm_coupling_apply_frame(reinterpret_cast<SWMM_Engine>(&a.engine),&f),SWMM_OK);
    ASSERT_NO_THROW(until(a.engine,3.5));const auto* prescribed=router.state().runtime_forcings.find(3,0);
    EXPECT_GT(prescribed->receipt.requested,natural*router.mesh().tri_area[0]*3.5);
    EXPECT_GT(prescribed->receipt.applied,0);EXPECT_LT(prescribed->receipt.applied,prescribed->receipt.requested);
    EXPECT_EQ(prescribed->receipt.water_out,prescribed->receipt.applied);EXPECT_EQ(router.state().runtime_forcings.find(3,1)->receipt.applied,0);
    double intake=0;for(double v:router.subsurface().state().xacc_from_surface)intake+=v;
    EXPECT_GE(intake,0);for(double v:router.state().volume)EXPECT_GE(v,0);
    EXPECT_GT(a.stage.meshEvaporation(),0);
    ASSERT_EQ(a.engine.saveCheckpoint((root()/"provider-infiltration.chk").string()),"");
}
TEST(EngineCheckpoint, RuntimeWaterProfileRejectsChangedReplayIdentityAndPrivateFootprints) {
    SurfaceSession a,b;
    ASSERT_NO_THROW(startProvider(a,"provider-identity-a"));
    ASSERT_NO_THROW(startProvider(b,"provider-identity-b",0,0,false,0,"driver revision 2"));
    ASSERT_EQ(providerFrame(a.engine),SWMM_OK);
    ASSERT_NO_THROW(until(a.engine,4.2));
    auto good=root()/"provider-identity.chk";ASSERT_EQ(a.engine.saveCheckpoint(good.string()),"");
    auto before=root()/"provider-identity-target.chk";ASSERT_EQ(b.engine.saveCheckpoint(before.string()),"");const auto initial=bytes(before);
    EXPECT_FALSE(b.engine.loadCheckpoint(good.string()).empty());ASSERT_EQ(b.engine.saveCheckpoint(before.string()),"");EXPECT_EQ(bytes(before),initial);
    EXPECT_FALSE(b.engine.context().runtime_coupling_used);
    SurfaceSession private_sources;
    EXPECT_THROW(private_sources.start("provider-private-footprint",0,0,0,0,true,true,false,-1,false,0,false,"driver revision 1"),std::runtime_error);
    SurfaceSession unqualified;
    ASSERT_NO_THROW(unqualified.start("provider-no-profile"));
    const auto end=unqualified.stage.completedEnd();EXPECT_EQ(providerFrame(unqualified.engine),SWMM_ERR_PLUGIN);
    EXPECT_EQ(unqualified.stage.completedEnd(),end);EXPECT_FALSE(unqualified.engine.context().runtime_coupling_used);
}
#ifdef OPENSWMM_MESH_OUTPUT_CHECKPOINT_TEST
TEST(EngineCheckpoint, RuntimeWaterMalformedNestedRecordsPreserveBothOutputsAndAllowRetry) {
    SurfaceSession a;
    ASSERT_NO_THROW(startProvider(a,"provider-atomic-source",2,0,true,2));
    ASSERT_EQ(providerFrame(a.engine),SWMM_OK);
    ASSERT_NO_THROW(until(a.engine,4.2));
    auto good=root()/"provider-atomic-good.chk";ASSERT_EQ(a.engine.saveCheckpoint(good.string()),"");
    for(int mutation=0;mutation<9;++mutation) {
        const auto name="provider-atomic-target-"+std::to_string(mutation);SCOPED_TRACE(name);
        SurfaceSession b;
    ASSERT_NO_THROW(startProvider(b,name,2,0,true,2));
        const auto before=root()/(name+"-before.chk");ASSERT_EQ(b.engine.saveCheckpoint(before.string()),"");
        const auto initial=bytes(before),binary=bytes(root()/(name+".out")),mesh=bytes(root()/(name+".h5"));
        const auto bad=root()/(name+"-bad.chk");write(bad,changeProviderRecord(bytes(good),mutation));
        EXPECT_FALSE(b.engine.loadCheckpoint(bad.string()).empty());EXPECT_FALSE(b.engine.context().runtime_coupling_used);
        ASSERT_EQ(b.engine.saveCheckpoint(before.string()),"");EXPECT_EQ(bytes(before),initial);
        EXPECT_EQ(bytes(root()/(name+".out")),binary);EXPECT_EQ(bytes(root()/(name+".h5")),mesh);
        ASSERT_EQ(b.engine.loadCheckpoint(good.string()),"");compareMesh(a.engine,b.engine,name+"-retry");
    }
}
TEST(EngineCheckpoint, RuntimeWaterCombinedOutputsContinueReportAndExpiryBoundaries) {
    for(int lid:{0,1,2})for(int format:{1,2}) {
        const auto name="provider-combined-"+std::to_string(lid)+"-"+std::to_string(format);SCOPED_TRACE(name);
        SurfaceSession a,b;
    ASSERT_NO_THROW(startProvider(a,name,lid,0,true,format));
    ASSERT_NO_THROW(startProvider(b,name+"-resumed",lid,0,true,format));
        ASSERT_EQ(providerFrame(a.engine),SWMM_OK);
    ASSERT_NO_THROW(until(a.engine,4.2));
        const auto cp=root()/(name+".chk");ASSERT_EQ(a.engine.saveCheckpoint(cp.string()),"");ASSERT_EQ(b.engine.loadCheckpoint(cp.string()),"");
        compareMesh(a.engine,b.engine,name+"-restored");
        while(a.engine.context().elapsed_ms<a.engine.context().options.totalDurationMs()) {
            ASSERT_NO_THROW(step(a.engine));
    ASSERT_NO_THROW(step(b.engine));
        }
        compareMesh(a.engine,b.engine,name+"-final");ASSERT_EQ(a.engine.end(),0);ASSERT_EQ(b.engine.end(),0);
        sameBinary(name,name+"-resumed",80);sameMesh(name,name+"-resumed",40);
    }
}
#endif
#endif
