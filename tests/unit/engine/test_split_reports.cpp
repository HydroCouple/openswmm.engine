// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
// Private native scheduling prerequisite. Public water packet schemas stay unchanged.
#include <gtest/gtest.h>
#include "core/SWMMEngine.hpp"
#include "core/FileIO.hpp"
#include "core/SplitFrame.hpp"
#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_split_packet.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>

namespace {
using namespace openswmm;
using Frame = SplitFrame;
namespace fs = std::filesystem;
struct Engine : SWMMEngine { ~Engine() { close(); } };
struct OwnedFrame {
    SWMM_SplitFrame value=nullptr;
    ~OwnedFrame(){swmm_split_frame_destroy(value);}
};
std::string bytes(const fs::path& p) {
    std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};
}
std::string model(double route=.31,int report_start=0) {
    std::ostringstream s;s<<std::setprecision(17)<<R"([OPTIONS]
FLOW_UNITS CMS
FLOW_ROUTING DYNWAVE
START_DATE 01/01/2026
START_TIME 00:00:00
END_DATE 01/01/2026
END_TIME 00:00:10
REPORT_START_DATE 01/01/2026
REPORT_START_TIME 00:00:)"<<(report_start<10?"0":"")<<report_start<<R"(
ROUTING_STEP )"<<route<<R"(
REPORT_STEP 00:00:02
IGNORE_QUALITY YES
ALLOW_PONDING YES
MIN_SURFAREA 1
MINIMUM_STEP .001
VARIABLE_STEP 0
[JUNCTIONS]
J 0 1 .1 2 10
[OUTFALLS]
O 0 FIXED 0 NO
[CONDUITS]
C J O 30 .013 0 0
[XSECTIONS]
C CIRCULAR 1 0 0 0 1
[COORDINATES]
J 2 2
O 12 2
[REPORT]
CONTINUITY YES
NODES ALL
LINKS ALL
[2D_OPTIONS]
BACKEND CPU
MAX_TIMESTEP .2
RECONSTRUCTION_ORDER 1
REPORT_2D YES
REPORT_2D_STEP 2
TRANSPORT_POLLUTANTS NO
TRANSPORT_MSX NO
TRANSPORT_AGE NO
TRANSPORT_TEMPERATURE NO
BED_HEAT_MODEL NONE
COUPLING_SYNC .7
[2D_VERTICES]
0 0 1
10 0 1
10 10 1
0 10 1
[2D_TRIANGLES]
0 1 2 .03 .2
0 2 3 .03 .1
[2D_TRIANGLE_NODE_MAP]
0 J .6 .02
)";return s.str();
}
class SplitReports : public ::testing::Test {
protected:
    fs::path dir,input;
    void SetUp() override {
        const char* override=std::getenv("OPENSWMM_SPLIT_REPORT_EVIDENCE");
        dir=fs::path(override?override:SPLIT_REPORT_EVIDENCE)/::testing::UnitTest::GetInstance()->current_test_info()->name();
        fs::create_directories(dir);input=dir/"model.inp";std::ofstream(input)<<model();
    }
    void open(Engine& e,const std::string& name,bool binary=false) {
        ASSERT_EQ(e.open(io::path_utf8(input).c_str(),io::path_utf8((dir/(name+".rpt"))).c_str(),binary?io::path_utf8((dir/(name+".out"))).c_str():nullptr),SWMM_OK);
    }
    void mesh(Engine& e,const std::string& name) {
        fs::remove(dir/(name+".h5"));
        ASSERT_EQ(swmm_2d_set_results_file(&e,io::path_utf8((dir/(name+".h5"))).c_str()),SWMM_OK);
    }
    struct Peer {
        Engine* surface=nullptr;
        Engine* network=nullptr;
        int advances=0,reports=0,finishes=0,outputs=0,report_outputs=0,accepted=0,ends=0;
        std::vector<int> events;
        bool in_report=false,active_bad=false;
        int mutation=0;
        Frame last_report;
        std::ofstream trace;
        ~Peer() { if(network)network->close();if(surface)surface->close(); }
        static void observe(SWMM_Engine,int phase,double,double,void* user) {
            auto& p=*static_cast<Peer*>(user);
            if(phase==SWMM_PHASE_OUTPUTS_PUBLISHED){
                ++p.outputs;if(p.in_report)++p.report_outputs;p.events.push_back(phase);
                if(p.in_report) {
                    SWMM_SplitFrame sentinel=reinterpret_cast<void*>(std::uintptr_t(1));
                    EXPECT_EQ(p.surface->evaluateSplit(SplitExecution::REPORT,&p.last_report,&sentinel),SWMM_ERR_LIFECYCLE);
                    EXPECT_EQ(sentinel,reinterpret_cast<void*>(std::uintptr_t(1)));
                    EXPECT_EQ(p.surface->end(),SWMM_ERR_LIFECYCLE);
                    EXPECT_EQ(p.surface->close(),SWMM_ERR_LIFECYCLE);
                }
            }
            if(phase==SWMM_PHASE_STEP_ACCEPTED){++p.accepted;p.events.push_back(phase);}
        }
        static void ended(SWMM_Engine,double,double,void* user) {
            auto& p=*static_cast<Peer*>(user);++p.ends;p.events.push_back(-1);
        }
        static int exchange(SWMM_Engine network,int phase,SWMM_SplitFrame request,SWMM_SplitFrame* reply,void* user) {
            auto& p=*static_cast<Peer*>(user);auto& n=*static_cast<SWMMEngine*>(network);
            const auto& sent=*static_cast<Frame*>(request);
            EXPECT_TRUE(sent.settled_reports);
            char* text=reinterpret_cast<char*>(std::uintptr_t(1));
            EXPECT_NE(swmm_split_packet_encode(network,request,&text),SWMM_OK);
            EXPECT_EQ(text,reinterpret_cast<char*>(std::uintptr_t(1)));
            if(phase==SplitExecution::REPORT) {
                ++p.reports;EXPECT_EQ(sent.dt,0);EXPECT_EQ(sent.start,n.context().elapsed_ms/1000.);
                EXPECT_EQ(sent.start,p.surface->context().elapsed_ms/1000.);
                EXPECT_EQ(sent.report_time_ms,p.surface->context().next_report_ms);
                p.last_report=sent;
            }
            Frame altered=sent;
            if((phase==SplitExecution::REPORT && p.mutation && p.mutation<=7) || (phase==SWMM_SPLIT_FINISH && p.mutation==8)) {
                if(p.mutation==1)altered.report_time_ms+=1;
                if(p.mutation==2)altered.phase=SWMM_SPLIT_ADVANCE,altered.dt=.31;
                if(p.mutation==3)altered.phase=SWMM_SPLIT_FINISH;
                if(p.mutation==4)altered.settled_reports=false;
                if(p.mutation==5)altered.sender+=1000;
                if(p.mutation==6)altered.start+=.001;
                if(p.mutation==7)altered.sequence+=1;
                if(p.mutation==8)altered.report_time_ms+=1;
                const auto before=p.surface->context().next_report_ms;
                SWMM_SplitFrame sentinel=reinterpret_cast<void*>(std::uintptr_t(1));
                const int rc=p.surface->evaluateSplit(altered.phase,&altered,&sentinel);
                EXPECT_NE(rc,SWMM_OK);EXPECT_EQ(sentinel,reinterpret_cast<void*>(std::uintptr_t(1)));
                EXPECT_EQ(p.surface->context().next_report_ms,before);p.active_bad=true;return rc;
            }
            const auto before=p.surface->context().elapsed_ms,grid=p.surface->context().next_report_ms;
            p.in_report=phase==SplitExecution::REPORT;
            const int rc=p.surface->evaluateSplit(phase,request,reply);p.in_report=false;
            if(rc!=SWMM_OK)return rc;
            if(phase==SWMM_SPLIT_ADVANCE) {
                ++p.advances;EXPECT_EQ(p.surface->context().next_report_ms,grid);
            }
            if(phase==SplitExecution::REPORT) {
                EXPECT_EQ(p.surface->context().elapsed_ms,before);
                EXPECT_GT(p.surface->context().next_report_ms,grid);
            }
            if(phase==SWMM_SPLIT_FINISH){++p.finishes;EXPECT_EQ(p.surface->context().elapsed_ms,before);}
            const auto& got=*static_cast<Frame*>(*reply);
            EXPECT_EQ(got.phase,phase);EXPECT_EQ(got.start,sent.start);
            EXPECT_EQ(got.settled_reports,sent.settled_reports);
            if(p.trace)p.trace<<phase<<','<<sent.sequence<<','<<sent.start<<','<<sent.dt<<','<<sent.report_time_ms<<','
                <<p.surface->context().next_report_ms<<','<<got.remaining<<'\n';
            return SWMM_OK;
        }
    };
    void connect(Engine& network,Engine& surface,Peer& peer,bool writer=false) {
        peer.network=&network;peer.surface=&surface;
        ASSERT_EQ(network.configureSplit(SWMM_SPLIT_NETWORK,true),SWMM_OK);
        ASSERT_EQ(surface.configureSplit(SWMM_SPLIT_SURFACE,true),SWMM_OK);
        if(writer)ASSERT_NO_FATAL_FAILURE(mesh(surface,"surface"));
        ASSERT_EQ(surface.initialize(),SWMM_OK);OwnedFrame geometry;
        ASSERT_EQ(surface.splitGeometry(&geometry.value),SWMM_OK);
        char* text=nullptr;EXPECT_NE(swmm_split_packet_encode(&surface,geometry.value,&text),SWMM_OK);EXPECT_EQ(text,nullptr);
        ASSERT_EQ(network.initialize(),SWMM_OK);ASSERT_EQ(network.applySplitGeometry(geometry.value),SWMM_OK);
        peer.surface=&surface;peer.trace.open(dir/"phases.csv");peer.trace<<std::setprecision(17);
        ASSERT_EQ(network.setSplitCallback(Peer::exchange,&peer),SWMM_OK);
        ASSERT_EQ(swmm_set_phase_callback(&surface,Peer::observe,&peer),SWMM_OK);
        ASSERT_EQ(swmm_set_step_end_callback(&surface,Peer::ended,&peer),SWMM_OK);
    }
    void same(const Engine& reference,const Engine& network,const Engine& surface) {
        const auto& a=reference.context();const auto& b=network.context();const auto& c=surface.context();
        EXPECT_EQ(a.elapsed_ms,b.elapsed_ms);EXPECT_EQ(b.elapsed_ms,c.elapsed_ms);
        EXPECT_EQ(a.next_report_ms,b.next_report_ms);EXPECT_EQ(b.next_report_ms,c.next_report_ms);
        EXPECT_EQ(a.nodes.depth,b.nodes.depth);EXPECT_EQ(a.nodes.volume,b.nodes.volume);
        EXPECT_EQ(a.nodes.coupling_queue,b.nodes.coupling_queue);EXPECT_EQ(b.nodes.coupling_queue,c.nodes.coupling_queue);
        EXPECT_EQ(a.coupling_delivery_remaining,b.coupling_delivery_remaining);EXPECT_EQ(b.coupling_delivery_remaining,c.coupling_delivery_remaining);
        EXPECT_EQ(reference.surfaceRouter2D().state().depth,surface.surfaceRouter2D().state().depth);
        EXPECT_EQ(reference.surfaceRouter2D().state().volume,surface.surfaceRouter2D().state().volume);
    }
    void parity(const std::string& name,bool binary,bool writer,double routing=.31,int report_start=0,int stop=10000) {
        const auto previous=dir;dir/=name;fs::create_directories(dir);input=dir/"model.inp";std::ofstream(input)<<model(routing,report_start);
        Engine reference,network,surface;
        ASSERT_NO_FATAL_FAILURE(open(reference,"native",binary));
        ASSERT_NO_FATAL_FAILURE(open(network,"network",binary));
        ASSERT_NO_FATAL_FAILURE(open(surface,"surface"));
        if(writer)ASSERT_NO_FATAL_FAILURE(mesh(reference,"native"));
        ASSERT_EQ(reference.initialize(),SWMM_OK);Peer peer;
        ASSERT_NO_FATAL_FAILURE(connect(network,surface,peer,writer));
        ASSERT_EQ(reference.start(binary?1:0),SWMM_OK);ASSERT_EQ(surface.start(0),SWMM_OK);ASSERT_EQ(network.start(binary?1:0),SWMM_OK);
        double a=0,b=0;int steps=0;
        do {
            const auto before=reference.context().elapsed_ms;
            ASSERT_EQ(reference.step(&a),SWMM_OK)<<reference.context().error_message;
            ASSERT_EQ(network.step(&b),SWMM_OK)<<network.context().error_message;
            EXPECT_EQ(a,b);ASSERT_NO_FATAL_FAILURE(same(reference,network,surface));
            if(reference.context().elapsed_ms>before)++steps;
        }while(b>0 && steps<stop);
        ASSERT_EQ(reference.end(),SWMM_OK);ASSERT_EQ(network.end(),SWMM_OK)<<network.context().error_message;
        ASSERT_NO_FATAL_FAILURE(same(reference,network,surface));
        EXPECT_EQ(peer.advances,steps);EXPECT_EQ(peer.finishes,1);EXPECT_EQ(peer.outputs,steps);
        EXPECT_EQ(peer.accepted,steps);EXPECT_EQ(peer.ends,steps);ASSERT_EQ(peer.events.size(),3u*steps);
        for(int k=0;k<steps;++k) {
            EXPECT_EQ(peer.events[3*k],SWMM_PHASE_OUTPUTS_PUBLISHED);
            EXPECT_EQ(peer.events[3*k+1],-1);EXPECT_EQ(peer.events[3*k+2],SWMM_PHASE_STEP_ACCEPTED);
        }
        EXPECT_EQ(peer.report_outputs,peer.reports);if(steps>10)EXPECT_GT(peer.reports,1);
        ASSERT_EQ(reference.close(),SWMM_OK);ASSERT_EQ(network.close(),SWMM_OK);ASSERT_EQ(surface.close(),SWMM_OK);
        if(binary)EXPECT_EQ(bytes(dir/"native.out"),bytes(dir/"network.out"));
        std::ofstream(dir/"summary.json")<<"{\"steps\":"<<steps<<",\"reports\":"<<peer.reports<<",\"finishes\":"<<peer.finishes
            <<",\"binary\":"<<(binary?"true":"false")<<",\"mesh\":"<<(writer?"true":"false")<<"}\n";
        dir=previous;
    }
};
TEST_F(SplitReports, NativeWriterCombinationsExactAndOvershotReports) {
    for(bool binary:{false,true})for(bool writer:{false,true})for(double dt:{.5,.31}) {
        const auto tag=std::string(binary?"binary-":"no-binary-")+(writer?"mesh-":"no-mesh-")+(dt==.5?"exact":"overshot");
        ASSERT_NO_FATAL_FAILURE(parity(tag,binary,writer,dt));
    }
}
TEST_F(SplitReports, ReportStartSkipsAndNativeSingleTickCadence) {
    ASSERT_NO_FATAL_FAILURE(parity("skip-early",true,true,.31,6));
    ASSERT_NO_FATAL_FAILURE(parity("coarse",true,true,3.1));
}
TEST_F(SplitReports, EarlyFinishFlushesActualPendingWindowWithoutExtraStep) {
    ASSERT_NO_FATAL_FAILURE(parity("early",true,true,.31,0,9));
}
TEST_F(SplitReports, PendingReportRejectsWrongGridNextAdvanceFinishModePeerClockAndSequence) {
    for(int mutation=1;mutation<=7;++mutation) {
        SCOPED_TRACE(mutation);Engine network,surface;
        ASSERT_NO_FATAL_FAILURE(open(network,"bad-network-"+std::to_string(mutation)));
        ASSERT_NO_FATAL_FAILURE(open(surface,"bad-surface-"+std::to_string(mutation)));
        Peer peer;ASSERT_NO_FATAL_FAILURE(connect(network,surface,peer));peer.mutation=mutation;
        ASSERT_EQ(surface.start(0),SWMM_OK);ASSERT_EQ(network.start(0),SWMM_OK);
        double elapsed=0;int rc=0;for(int k=0;k<10 && rc==SWMM_OK;++k)rc=network.step(&elapsed);
        EXPECT_NE(rc,SWMM_OK);EXPECT_TRUE(peer.active_bad);EXPECT_EQ(peer.reports,1);
        EXPECT_EQ(peer.accepted,peer.advances-1);EXPECT_EQ(peer.ends,peer.advances-1);
        const auto clock=surface.context().elapsed_ms,grid=surface.context().next_report_ms;
        const auto physical=surface.surfaceRouter2D().state().volume;
        EXPECT_EQ(surface.end(),SWMM_ERR_LIFECYCLE);EXPECT_EQ(surface.surfaceRouter2D().state().volume,physical);
        EXPECT_NE(network.step(&elapsed),SWMM_OK);EXPECT_NE(network.end(),SWMM_OK);
        EXPECT_EQ(surface.context().elapsed_ms,clock);EXPECT_EQ(surface.context().next_report_ms,grid);
    }
}
TEST_F(SplitReports, ConsumedReportRefusesReplayWithUnchangedOutputAndClock) {
    Engine network,surface;
    ASSERT_NO_FATAL_FAILURE(open(network,"network"));
    ASSERT_NO_FATAL_FAILURE(open(surface,"surface"));
    Peer peer;ASSERT_NO_FATAL_FAILURE(connect(network,surface,peer));
    ASSERT_EQ(surface.start(0),SWMM_OK);ASSERT_EQ(network.start(0),SWMM_OK);
    double elapsed=0;while(peer.reports==0)ASSERT_EQ(network.step(&elapsed),SWMM_OK);
    const auto clock=surface.context().elapsed_ms,grid=surface.context().next_report_ms;
    SWMM_SplitFrame sentinel=reinterpret_cast<void*>(std::uintptr_t(1));
    EXPECT_NE(surface.evaluateSplit(SplitExecution::REPORT,&peer.last_report,&sentinel),SWMM_OK);
    EXPECT_EQ(sentinel,reinterpret_cast<void*>(std::uintptr_t(1)));EXPECT_EQ(surface.context().elapsed_ms,clock);
    EXPECT_EQ(surface.context().next_report_ms,grid);
}
TEST_F(SplitReports, SettledFinishRejectsStaleReportGridWithoutFlushing) {
    Engine network,surface;
    ASSERT_NO_FATAL_FAILURE(open(network,"network"));
    ASSERT_NO_FATAL_FAILURE(open(surface,"surface"));
    Peer peer;ASSERT_NO_FATAL_FAILURE(connect(network,surface,peer));peer.mutation=8;
    ASSERT_EQ(surface.start(0),SWMM_OK);ASSERT_EQ(network.start(0),SWMM_OK);
    double elapsed=0;for(int k=0;k<8;++k)ASSERT_EQ(network.step(&elapsed),SWMM_OK);
    ASSERT_GT(peer.reports,0);const auto clock=surface.context().elapsed_ms,grid=surface.context().next_report_ms;
    const auto before=surface.surfaceRouter2D().state().volume;
    EXPECT_NE(network.end(),SWMM_OK);EXPECT_TRUE(peer.active_bad);EXPECT_EQ(peer.finishes,0);
    EXPECT_EQ(surface.context().elapsed_ms,clock);EXPECT_EQ(surface.context().next_report_ms,grid);
    EXPECT_EQ(surface.surfaceRouter2D().state().volume,before);
}
TEST_F(SplitReports, GeometryModeMismatchRefusesBeforeApplying) {
    Engine network,surface;
    ASSERT_NO_FATAL_FAILURE(open(network,"network"));
    ASSERT_NO_FATAL_FAILURE(open(surface,"surface"));
    ASSERT_EQ(network.configureSplit(SWMM_SPLIT_NETWORK),SWMM_OK);
    ASSERT_EQ(surface.configureSplit(SWMM_SPLIT_SURFACE,true),SWMM_OK);
    ASSERT_EQ(surface.initialize(),SWMM_OK);OwnedFrame geometry;ASSERT_EQ(surface.splitGeometry(&geometry.value),SWMM_OK);
    ASSERT_EQ(network.initialize(),SWMM_OK);const auto before=network.context().nodes.ponded_area;
    EXPECT_NE(network.applySplitGeometry(geometry.value),SWMM_OK);EXPECT_EQ(network.context().nodes.ponded_area,before);
}
}
