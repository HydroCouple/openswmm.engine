// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include "core/SWMMEngine.hpp"
#include "core/CompletedSourceArchive.hpp"
#include "core/DateTime.hpp"
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
void step(SWMMEngine& e) {
    double t=-1;auto result=e.step(&t);
    if(result)throw std::runtime_error("step: "+e.context().error_message);
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
