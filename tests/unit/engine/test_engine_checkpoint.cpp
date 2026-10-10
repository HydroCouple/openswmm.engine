// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include "core/SWMMEngine.hpp"
#include "core/CompletedSourceArchive.hpp"
#include "core/DateTime.hpp"
#ifdef OPENSWMM_HAS_2D
#include "2d/subsurface/CommonSourceStage.hpp"
#include "hydrology/LidNode.hpp"
#include "2d/subsurface/SourceResultHistory.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#endif
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <iterator>

using namespace openswmm;
namespace {
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
    void start(const std::string& name,int lid=0,int mapped=0,int closure=0,int mode=0,bool track=true,bool spatial=false,bool all_sources=false) {
        SWMMEngine authored;startRunoff(authored,name,mode%5,mode%3,lid,false);authored.close();
        auto p=root()/name;auto deck=bytes(p.string()+".inp");
        deck.replace(deck.find("IGNORE_2D YES"),13,"IGNORE_2D NO");
        // Surface advances on every routing step; routing-event skips are outside this profile.
        write(p.string()+".inp",deck);
        auto& e=engine;
        if(e.open((p.string()+".inp").c_str(),(p.string()+".rpt").c_str(),nullptr))throw std::runtime_error(e.context().error_message);
        auto& ctx=e.context();ctx.options.num_threads=1;ctx.options.surcharge_method=mode%4;
        ctx.events.clear();ctx.gages.scale_factor[1]=1.3;ctx.node_subtypes.outfalls.route_to[0]=2;
        auto& router=e.surfaceRouter2D();auto& mesh=router.mesh();
        mesh.resize_vertices(4);mesh.resize_triangles(2);
        mesh.vx={0,4,0,4};mesh.vy={0,0,2,2};mesh.vz={.5,.5,.5,.5};
        mesh.set_triangle(0,0,1,2);mesh.set_triangle(1,1,3,2);mesh.mannings_n={.03,.03};
        mesh.tri_init_depth={.04,.003};mesh.tri_coupled_node=mapped==1?std::vector<int>{2,0}:(mapped==2?std::vector<int>{2,2}:std::vector<int>{0,0});
        mesh.tri_coupling_cd={.6,.6};mesh.tri_coupling_area={.002,.003};
        auto& options=router.options();options.mesh_units_si=true;options.num_threads=1;
        options.momentum=twoD::Momentum2D::LOCAL_INERTIAL;options.max_timestep=.21;
        options.coupling_sync=3.5;options.evaporation=2;options.rainfall_mode=twoD::RainfallMode::SYSTEM;
        auto& config=router.aquiferConfig();config.options.authored=true;config.options.force_closed_form=true;
        config.options.gw_et="BOUNDARY_ET";config.options.closure=closure?twoD::GwClosure::ENSLAVED:twoD::GwClosure::CLOSED_FORM;
        twoD::GwAquiferRow row;row.Ks=.000001*3600/.0254;row.zs=2/.3048;row.hg0=1/.3048;row.alpha=2*.3048;config.rows.push_back(row);
        if(e.initialize() || e.start(0))throw std::runtime_error("surface start: "+ctx.error_message);
        auto& gw=router.subsurface().state();
        for(int i=0;i<2;++i) {
            if(closure)gw.hg[i]=gw.zs[i]-.0001;
            gw.hu[i]=gw.theta_s[i]*(gw.zs[i]-gw.hg[i])-.0001/mesh.tri_area[i];
        }
        if(all_sources)ctx.node_subtypes.outfalls.route_to[0]=-1;
        auto error=sources.initialize(ctx,spatial ? (all_sources?std::vector<int>{0,2}:std::vector<int>{0}):std::vector<int>{});if(!error.empty())throw std::runtime_error(error);
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
        error=stage.initialize(mesh,router.state(),options,router.subsurface(),sources,preview,!spatial);
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
void startClosedSpatial(SurfaceSession& m,const std::string& name,int node_lid=0,double cadence=.7) {
    auto path=root()/name;
    std::string deck="[OPTIONS]\nFLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\nSTART_DATE 10/10/2026\nSTART_TIME 00:00:00\nEND_DATE 10/10/2026\nEND_TIME 00:00:30\nROUTING_STEP "+std::to_string(cadence)+"\nVARIABLE_STEP 0\nINFILTRATION GREEN_AMPT\nRULE_STEP 00:00:02\nREPORT_STEP 00:00:03\nWET_STEP 00:00:01\nDRY_STEP 00:00:07\nIGNORE_QUALITY YES\nIGNORE_SNOWMELT YES\nIGNORE_GROUNDWATER YES\nIGNORE_2D NO\n[STORAGE]\nSINK 0 100 0 FUNCTIONAL 0 0 100 0 0\n[OUTFALLS]\nDRAIN 0 FREE NO\n[RAINGAGES]\nR INTENSITY 0:00:05 1 TIMESERIES Rain\nDry INTENSITY 0:00:05 1 TIMESERIES Zero\n[SUBCATCHMENTS]\nA R B .01 45 10 1 0\nB R SINK .02 65 10 1 0\nD Dry SINK .01 50 10 1 0\n[SUBAREAS]\nA .015 .12 .03 .05 35 PERVIOUS 55\nB .02 .13 .02 .04 25 OUTLET\nD .02 .13 .02 .04 25 OUTLET\n[INFILTRATION]\nA 3 .25 .3\nB 3 .25 .3\nD 3 .25 .3\n[LID_CONTROLS]\nI IT\nI SURFACE 3 .05 .12 1 3\nI STORAGE 6 .667 .1 0\nI DRAIN .5 .5 0 0\n[LID_USAGE]\nA I 1 20 3 70 30 0 * DRAIN\n[EVAPORATION]\nCONSTANT 0\n[TIMESERIES]\nRain 10/10/2026 00:00:00 5\nRain 10/10/2026 00:00:05 0\nZero 10/10/2026 00:00:00 0\n";
    if(node_lid) {
        deck.replace(deck.find("SINK 0 100 0"),12,"SINK 0 2 0");
        deck+="[LID_CONTROLS]\nStack NODE\nStack SURFACE 6 .1\nStack MEDIA 12 .45 .2 .08 2 10 3\nStack AGGREGATE 6 .4 100\nStack BOTTOM 0 0\n[LID_NODES]\nSINK Stack 10\n";
        if(node_lid==2)deck+="[LID_RICHARDS]\nStack OPTIONS 4 1e-7 1e-5 30\nStack 2 .03 2 1.6 .5 .0001\nStack 3 .03 2 1.6 .5 .0001\n";
    }
    write(path.string()+".inp",deck);auto& e=m.engine;
    if(e.open((path.string()+".inp").c_str(),(path.string()+".rpt").c_str(),nullptr))throw std::runtime_error(e.context().error_message);
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
    if(e.initialize() || e.start(0))throw std::runtime_error(e.context().error_message);
    auto error=m.sources.initialize(e.context(),{0});if(!error.empty())throw std::runtime_error(error);
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
