// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include <gtest/gtest.h>
#include "core/SWMMEngine.hpp"
#include "core/FileIO.hpp"
#include "core/CompletedSourceArchive.hpp"
#include "core/CheckpointInspection.hpp"
#include "live_passive_recovery_test_support.hpp"
#include <openswmm/engine/openswmm_callbacks.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <numeric>
#include <sstream>

namespace {
using namespace openswmm;
using namespace live_recovery_test;
constexpr double dt = 1.0 / 1024;
constexpr const char* water_magic = "OpenSWMM paired water surface boundary";
std::filesystem::path evidence(const std::string& name) {
    const char* override = std::getenv("OPENSWMM_COUPLED_WATER_EVIDENCE");
    const std::filesystem::path root = override ? override : COUPLED_WATER_TEST_OUTPUT;
    std::filesystem::create_directories(root);
    return root / name;
}
std::string read(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
// Six disconnected, closed, unit-area triangles share one genuinely ponded
// storage. Asymmetric authored heads produce a nonzero queue with both drain
// and spill legs. No runtime state or scheduling overrides create the halves.
std::string model(bool positive) {
    std::ostringstream s;
    s << std::setprecision(17) <<
        "[OPTIONS]\nFLOW_UNITS CMS\nFLOW_ROUTING DYNWAVE\nSTART_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
        "END_DATE 01/01/2026\nEND_TIME 00:00:01\nROUTING_STEP .0009765625\nVARIABLE_STEP 0\n"
        "REPORT_STEP 00:00:01\nTHREADS 1\nALLOW_PONDING YES\nIGNORE_QUALITY YES\n"
        "[STORAGE]\nTank -10 1 10 FUNCTIONAL 0 0 " << 2*.02832/(.3048*.3048*.3048)
        << ' ' << 2*.02832/(.3048*.3048*.3048) << " 0\n"
        "[2D_OPTIONS]\nINTEGRATOR EXPLICIT\nBACKEND CPU\nLTS_TIERS 1\n"
        "MAX_TIMESTEP .001953125\nCOUPLING_SYNC .001953125\nREPORT_2D NO\n[2D_VERTICES]\n";
    for (int k=0; k<3; ++k) {
        const int x=8*k;
        s << x << " 0 -1.5\n" << x+2 << " 0 -1.5\n" << x << " 1 -1.5\n"
          << x << " -1 -1.5\n" << x << " -2 -1.5\n" << x+2 << " -1 -1.5\n";
    }
    s << "[2D_TRIANGLES]\n";
    for (int k=0; k<6; ++k)
        s << 3*k << ' ' << 3*k+1 << ' ' << 3*k+2 << " .03 "
          << (k%2 ? (positive?1.25:1.2) : (positive?1.8:1.75)) << '\n';
    s << "[2D_VERTEX_NODE_MAP]\n";
    for (int k=0; k<6; ++k) s << 3*k << " Tank .7 .01\n";
    s << "[REPORT]\nINPUT NO\n";
    return s.str();
}
void start(SWMMEngine& e, const std::string& name, const std::string& input, bool checkpoint=true) {
    std::ofstream(evidence(name+".inp")) << input;
    ASSERT_EQ(e.open(io::path_utf8(evidence(name+".inp")).c_str(), io::path_utf8(evidence(name+".rpt")).c_str(), nullptr), SWMM_OK);
    ASSERT_EQ(e.initialize(), SWMM_OK);
    ASSERT_EQ(e.start(0), SWMM_OK);
    if (checkpoint) { const auto error=e.enableCoupledWaterCheckpointing(); ASSERT_TRUE(error.empty()) << error; }
}
void sameState(const SWMMEngine& a, const SWMMEngine& b) {
    const auto& x=a.context(); const auto& y=b.context();
    EXPECT_EQ(x.current_time,y.current_time); EXPECT_EQ(x.elapsed_ms,y.elapsed_ms);
    EXPECT_EQ(x.coupling_delivery_remaining,y.coupling_delivery_remaining);
    const auto& n=x.nodes; const auto& m=y.nodes;
    // The initial authored geometry uses the fixed-pool two-epsilon oracle.
    // Native DW advances depth and volume separately thereafter: trace their
    // accumulated geometric residual instead of imposing a fixed-pool bound.
    if (x.current_time==0)
        for (const auto* nodes : {&n,&m})
            EXPECT_NEAR(nodes->volume[0], nodes->full_volume[0] +
                        (nodes->depth[0]-nodes->full_depth[0])*nodes->ponded_area[0],
                        2*std::numeric_limits<double>::epsilon()*nodes->volume[0]);
#define SAME(field) EXPECT_EQ(n.field,m.field) << #field;
    SAME(volume) SAME(old_volume) SAME(depth) SAME(old_depth) SAME(head)
    SAME(inflow) SAME(outflow) SAME(old_inflow) SAME(old_net_inflow) SAME(lat_flow) SAME(old_lat_flow) SAME(losses)
    SAME(coupling_queue) SAME(coupling_volume) SAME(coupling_inflow) SAME(overflow)
#undef SAME
    const auto& u=a.surfaceRouter2D().state(); const auto& v=b.surfaceRouter2D().state();
    EXPECT_EQ(u.volume,v.volume); EXPECT_EQ(u.depth,v.depth); EXPECT_EQ(u.head,v.head);
    EXPECT_EQ(u.edge_flux,v.edge_flux); EXPECT_EQ(u.old_volume,v.old_volume);
    EXPECT_EQ(a.surfaceRouter2D().couplingExchangeVolumes(),b.surfaceRouter2D().couplingExchangeVolumes());
    EXPECT_EQ(encodeWaterBalance(x.mass_balance_2d),encodeWaterBalance(y.mass_balance_2d));
}
struct Trace {
    std::ofstream out;
    std::vector<std::vector<double>> rows;
    explicit Trace(const std::string& name):out(evidence(name+"-water.txt")) {
        out << "# phase start end span queue volume old_volume depth old_depth old_net inflow outflow coupling_rate coupling_volume lateral surface_volume full_volume si_to_native point0 point1 point2 point3 point4 point5 full_depth pond_area\n" << std::hexfloat;
    }
};
void phase(void* handle, int phase, double begin, double end, void* user) {
    auto& e=*static_cast<SWMMEngine*>(handle); auto& trace=*static_cast<Trace*>(user);
    const auto& c=e.context(); const auto& n=c.nodes; const auto& s=e.surfaceRouter2D().state();
    std::vector<double> row{double(phase),begin,end,c.coupling_delivery_remaining,n.coupling_queue[0],
        n.volume[0],n.old_volume[0],n.depth[0],n.old_depth[0],n.old_net_inflow[0],n.inflow[0],n.outflow[0],
        n.coupling_inflow[0],n.coupling_volume[0],n.lat_flow[0],
        std::accumulate(s.volume.begin(),s.volume.end(),0.0),n.rpt_full_volume[0],e.surfaceRouter2D().options().flow_2d_to_1d};
    const auto& points=e.surfaceRouter2D().couplingExchangeVolumes();
    row.insert(row.end(),points.begin(),points.end());
    row.push_back(n.full_depth[0]); row.push_back(n.ponded_area[0]);
    trace.out << phase;
    for (std::size_t i=1;i<row.size();++i) trace.out << ' ' << row[i];
    trace.out << '\n'; trace.rows.push_back(std::move(row));
}
void recover(bool positive) {
    const std::string tag=positive?"water-positive":"water-negative";
    const auto input=model(positive);
    SWMMEngine source, plain;
    ASSERT_NO_FATAL_FAILURE(start(source,tag+"-source",input));
    ASSERT_NO_FATAL_FAILURE(start(plain,tag+"-plain",input,false));
    Trace native(tag), ordinary(tag+"-plain");
    ASSERT_EQ(source.set_phase_callback(phase,&native),SWMM_OK);
    ASSERT_EQ(plain.set_phase_callback(phase,&ordinary),SWMM_OK);
    std::vector<std::unique_ptr<SWMMEngine>> restored;
    double elapsed=0, first_queue=0, first_volume=source.context().nodes.volume[0];
    ASSERT_NO_FATAL_FAILURE(sameState(source,plain));
    for (int boundary=0;boundary<=12;++boundary) {
        if (boundary) {
            ASSERT_EQ(source.step(&elapsed),SWMM_OK) << source.context().error_message;
            ASSERT_EQ(plain.step(&elapsed),SWMM_OK) << plain.context().error_message;
            ASSERT_NO_FATAL_FAILURE(sameState(source,plain));
            ASSERT_EQ(native.rows,ordinary.rows);
            for (auto& e:restored) {
                ASSERT_EQ(e->step(&elapsed),SWMM_OK) << e->context().error_message;
                ASSERT_NO_FATAL_FAILURE(sameState(source,*e));
            }
        }
        const auto& c=source.context();
        if (boundary==2) {
            first_queue=c.nodes.coupling_queue[0]; ASSERT_NE(first_queue,0);
            EXPECT_EQ(first_queue>0,positive); EXPECT_EQ(c.coupling_delivery_remaining,2*dt);
            const auto& points=source.surfaceRouter2D().couplingExchangeVolumes();
            ASSERT_EQ(points.size(),6u);
            for (int i=0;i<6;++i) EXPECT_EQ(points[i]>0,i%2==0);
        }
        if (boundary==3) {
            EXPECT_EQ(c.coupling_delivery_remaining,dt); EXPECT_EQ(c.nodes.coupling_queue[0],first_queue/2);
            EXPECT_EQ(c.nodes.coupling_inflow[0],first_queue/(2*dt));
            EXPECT_NE(c.nodes.volume[0],first_volume);
            EXPECT_EQ(c.nodes.volume[0],first_volume+first_queue/4);
        }
        const auto saved=evidence(tag+"-boundary-"+std::to_string(boundary)+".bin");
        auto error=source.saveCheckpoint(saved.string()); ASSERT_TRUE(error.empty()) << error;
        const auto copy=evidence(tag+"-copy.bin");
        for (auto& e:restored) {
            error=e->saveCheckpoint(copy.string()); ASSERT_TRUE(error.empty()) << error;
            EXPECT_EQ(read(saved),read(copy));
        }
        // Initial/pending mesh, first posted window, native half, and the
        // following whole-drain/refill include different hydraulic memory.
        if (boundary<=5) {
            auto e=std::make_unique<SWMMEngine>();
            ASSERT_NO_FATAL_FAILURE(start(*e,tag+"-restore-"+std::to_string(boundary),input));
            ASSERT_TRUE(e->saveCheckpoint(copy.string()).empty()); const auto fresh=read(copy);
            CheckpointInspection inspected;
            error=e->inspectCheckpoint(saved.string(),inspected); ASSERT_TRUE(error.empty()) << error;
            EXPECT_EQ(inspected.engine_seconds,source.context().current_time);
            EXPECT_EQ(inspected.completed_source_seconds,(boundary/2)*2*dt);
            ASSERT_TRUE(e->saveCheckpoint(copy.string()).empty()); EXPECT_EQ(read(copy),fresh);
            error=e->loadCheckpoint(saved.string()); ASSERT_TRUE(error.empty()) << error;
            ASSERT_NO_FATAL_FAILURE(sameState(source,*e));
            error=e->saveCheckpoint(copy.string()); ASSERT_TRUE(error.empty()) << error;
            EXPECT_EQ(read(saved),read(copy)); restored.push_back(std::move(e));
        }
    }
    for (auto& e:restored) EXPECT_EQ(e->close(),SWMM_OK);
    EXPECT_EQ(source.close(),SWMM_OK); EXPECT_EQ(plain.close(),SWMM_OK);
}
// Change an actual hydraulic proposal and refresh both inner and outer CRCs.
// Indices below refer to RoutingCheckpoint's NODE_STATE double-vector order.
std::string editNode(std::string bytes, int field, double value) {
    return editSurface(std::move(bytes),[&](std::string& surface) {
        editBlob(surface,"OpenSWMM accepted DW hydraulic boundary",[&](std::string& child) {
            Archive file(child); std::string name,identity,body; std::uint32_t version,sum;
            file(name,version,identity,sum,body); file.finish();
            Archive in(body),prefix;
            std::vector<double> water,reverse,heat,pollutant;
            double time,date,elapsed,old_elapsed,report,evap,infil,recovery,link_span; bool links;
            in(water,reverse,heat,pollutant,time,date,elapsed,old_elapsed,report,evap,infil,recovery,links,link_span);
            prefix(water,reverse,heat,pollutant,time,date,elapsed,old_elapsed,report,evap,infil,recovery,links,link_span);
            auto cursor=prefix.bytes().size();
            for(int row=0;row<=field;++row) {
                std::uint64_t n; std::memcpy(&n,body.data()+cursor,8);cursor+=8;
                if(n>(body.size()-cursor)/sizeof(double)) throw std::runtime_error("Bad hydraulic proposal extent.");
                if(row==field) { if(n!=1) throw std::runtime_error("Expected one storage."); std::memcpy(body.data()+cursor,&value,8); }
                cursor+=n*sizeof(double);
            }
            sum=crc(body); Archive out;out(name,version,identity,sum,body);child=out.bytes();
        });
    },water_magic);
}
}
TEST(CoupledWaterCheckpoint, AuthoredPositiveHalvesAndFlowMemoryRecoverWithoutChangingOrdinarySteps) { recover(true); }
TEST(CoupledWaterCheckpoint, AuthoredNegativeHalvesAndFlowMemoryRecoverWithoutChangingOrdinarySteps) { recover(false); }
namespace {
std::string tailModel(bool positive) {
    auto input=model(positive);
    const auto replace=[&](const std::string& from,const std::string& to) {
        std::size_t at=0;
        while((at=input.find(from,at))!=std::string::npos) {
            input.replace(at,from.size(),to); at+=to.size();
        }
    };
    replace("ROUTING_STEP .0009765625","ROUTING_STEP .125");
    replace("MAX_TIMESTEP .001953125","MAX_TIMESTEP .25");
    replace("COUPLING_SYNC .001953125","COUPLING_SYNC .25");
    replace(" .7 .01"," .7 .1");
    const auto first=input.find("[2D_TRIANGLES]"),last=input.find("[2D_VERTEX_NODE_MAP]");
    std::ostringstream cells;cells<<"[2D_TRIANGLES]\n"<<std::setprecision(17);
    // Authored heads straddle equilibrium. The existing ORIFICE cutoff closes
    // both directions naturally; this test neither sets a cutoff nor mutates
    // heads/queue/clock during execution.
    for(int k=0;k<6;++k)
        cells<<3*k<<' '<<3*k+1<<' '<<3*k+2<<" .03 "
             <<1.5+(k%2?-(positive?1:1.2):(positive?1.2:1))*2e-12<<'\n';
    input.replace(first,last-first,cells.str());return input;
}
void tailRecovery(bool positive) {
    const std::string tag=positive?"water-tail-positive":"water-tail-negative";
    const auto input=tailModel(positive);
    SWMMEngine source,plain;
    ASSERT_NO_FATAL_FAILURE(start(source,tag+"-source",input));
    ASSERT_NO_FATAL_FAILURE(start(plain,tag+"-plain",input,false));
    Trace native(tag),ordinary(tag+"-plain");
    ASSERT_EQ(source.set_phase_callback(phase,&native),SWMM_OK);
    ASSERT_EQ(plain.set_phase_callback(phase,&ordinary),SWMM_OK);
    double elapsed=0, first_queue=0, drained_volume=0, tail_rate=0;
    ASSERT_NO_FATAL_FAILURE(sameState(source,plain));
    std::vector<std::unique_ptr<SWMMEngine>> restored;
    for(int boundary=1;boundary<=7;++boundary) {
        ASSERT_EQ(source.step(&elapsed),SWMM_OK)<<source.context().error_message;
        ASSERT_EQ(plain.step(&elapsed),SWMM_OK)<<plain.context().error_message;
        ASSERT_NO_FATAL_FAILURE(sameState(source,plain)); ASSERT_EQ(native.rows,ordinary.rows);
        for(auto& e:restored) {
            ASSERT_EQ(e->step(&elapsed),SWMM_OK)<<e->context().error_message;
            ASSERT_NO_FATAL_FAILURE(sameState(source,*e));
        }
        const auto& c=source.context();const auto& n=c.nodes;
        EXPECT_EQ(c.current_time,boundary*.125);
        if(boundary==2) {
            first_queue=n.coupling_queue[0];ASSERT_NE(first_queue,0);EXPECT_EQ(first_queue>0,positive);
            const auto& points=source.surfaceRouter2D().couplingExchangeVolumes();ASSERT_EQ(points.size(),6u);
            for(int k=0;k<6;++k)EXPECT_EQ(points[k]>0,k%2==0);
        }
        if(boundary==3) { EXPECT_EQ(n.coupling_queue[0],first_queue/2); EXPECT_EQ(c.coupling_delivery_remaining,.125); }
        if(boundary>=4) {
            EXPECT_EQ(n.coupling_queue[0],0);EXPECT_EQ(c.coupling_delivery_remaining,0);
            for(double value:source.surfaceRouter2D().couplingExchangeVolumes())EXPECT_EQ(value,0);
        }
        if(boundary==4) {
            drained_volume=n.volume[0];tail_rate=n.coupling_inflow[0];ASSERT_NE(tail_rate,0);
            EXPECT_EQ(tail_rate>0,positive);EXPECT_EQ(n.old_net_inflow[0],tail_rate);
        }
        if(boundary==5) {
            EXPECT_EQ(n.coupling_inflow[0],0);EXPECT_EQ(n.old_net_inflow[0],tail_rate);
            EXPECT_EQ(n.volume[0],drained_volume+.5*tail_rate*.125);
            EXPECT_NE(n.volume[0],drained_volume);
        }
        if(boundary>=6) { EXPECT_EQ(n.coupling_inflow[0],0);EXPECT_EQ(n.old_net_inflow[0],0); }
        const auto saved=evidence(tag+"-boundary-"+std::to_string(boundary)+".bin"),copy=evidence(tag+"-copy.bin");
        auto error=source.saveCheckpoint(saved.string());ASSERT_TRUE(error.empty())<<error;
        for(auto& e:restored) {
            ASSERT_TRUE(e->saveCheckpoint(copy.string()).empty());EXPECT_EQ(read(copy),read(saved));
        }
        if(boundary>=3 && boundary<=6) {
            auto e=std::make_unique<SWMMEngine>();
            ASSERT_NO_FATAL_FAILURE(start(*e,tag+"-restore-"+std::to_string(boundary),input));
            ASSERT_TRUE(e->saveCheckpoint(copy.string()).empty());const auto fresh=read(copy);
            CheckpointInspection inspection;
            error=e->inspectCheckpoint(saved.string(),inspection);ASSERT_TRUE(error.empty())<<error;
            EXPECT_EQ(inspection.engine_seconds,c.current_time);
            ASSERT_TRUE(e->saveCheckpoint(copy.string()).empty());EXPECT_EQ(read(copy),fresh);
            error=e->loadCheckpoint(saved.string());ASSERT_TRUE(error.empty())<<error;
            ASSERT_NO_FATAL_FAILURE(sameState(source,*e));
            ASSERT_TRUE(e->saveCheckpoint(copy.string()).empty());EXPECT_EQ(read(copy),read(saved));
            restored.push_back(std::move(e));
        }
    }
    for(auto& e:restored)EXPECT_EQ(e->close(),SWMM_OK);
    EXPECT_EQ(source.close(),SWMM_OK);EXPECT_EQ(plain.close(),SWMM_OK);
}
}
TEST(CoupledWaterCheckpoint, AuthoredPositiveDrainedQueueRetainsAndRecoversHydraulicTail) { tailRecovery(true); }
TEST(CoupledWaterCheckpoint, AuthoredNegativeDrainedQueueRetainsAndRecoversHydraulicTail) { tailRecovery(false); }
TEST(CoupledWaterCheckpoint, FiniteInconsistentFlowHistoryRefusesAcrossBothQueueTails) {
    for(bool positive:{true,false}) {
        const std::string tag=positive?"water-tail-forge-positive":"water-tail-forge-negative";
        SWMMEngine source;
        ASSERT_NO_FATAL_FAILURE(start(source,tag+"-source",tailModel(positive)));
        double elapsed=0;
        for(int boundary=1;boundary<=6;++boundary) {
            ASSERT_EQ(source.step(&elapsed),SWMM_OK)<<source.context().error_message;
            if(boundary<3)continue;
            SWMMEngine target;
            const auto prefix=tag+"-"+std::to_string(boundary);
            ASSERT_NO_FATAL_FAILURE(start(target,prefix+"-target",tailModel(positive)));
            const auto good=evidence(prefix+"-good.bin"),copy=evidence(prefix+"-copy.bin");
            ASSERT_TRUE(source.saveCheckpoint(good.string()).empty());const auto valid=read(good);
            ASSERT_TRUE(target.saveCheckpoint(copy.string()).empty());const auto before=read(copy);
            const auto& n=source.context().nodes;
            const std::pair<int,double> fields[]={{34,n.inflow[0]},{35,n.outflow[0]},
                {37,n.losses[0]},{38,n.old_net_inflow[0]},{43,n.old_lat_flow[0]},{44,n.old_inflow[0]}};
            for(const auto& [field,value]:fields) {
                SCOPED_TRACE(tag+" boundary="+std::to_string(boundary)+" field="+std::to_string(field));
                const auto bad=evidence(prefix+"-bad-"+std::to_string(field)+".bin");
                std::ofstream(bad,std::ios::binary)<<editNode(valid,field,value+1);
                CheckpointInspection inspected;inspected.engine_seconds=123;
                auto error=target.inspectCheckpoint(bad.string(),inspected);
                EXPECT_NE(error.find("inconsistent native flow history"),std::string::npos)<<error;
                EXPECT_EQ(inspected.engine_seconds,123);
                error=target.loadCheckpoint(bad.string());
                EXPECT_NE(error.find("inconsistent native flow history"),std::string::npos)<<error;
                ASSERT_TRUE(target.saveCheckpoint(copy.string()).empty());EXPECT_EQ(read(copy),before);
            }
            auto error=target.loadCheckpoint(good.string());ASSERT_TRUE(error.empty())<<error;
            ASSERT_TRUE(target.saveCheckpoint(copy.string()).empty());EXPECT_EQ(read(copy),valid);
            ASSERT_NO_FATAL_FAILURE(sameState(source,target));
            EXPECT_EQ(target.close(),SWMM_OK);
        }
        EXPECT_EQ(source.close(),SWMM_OK);
    }
}
TEST(CoupledWaterCheckpoint, OptInAndProfileRefusalsLeaveFreshEngineReusable) {
    SWMMEngine e; const auto input=model(true);
    ASSERT_NO_FATAL_FAILURE(start(e,"water-opt-in",input,false));
    EXPECT_FALSE(e.enableCheckpointing().empty());
    auto& c=e.context();
    c.nodes.user_lat_flow[0]=1;
    EXPECT_FALSE(e.enableCoupledWaterCheckpointing().empty()); c.nodes.user_lat_flow[0]=0;
    c.options.water_age=true;
    EXPECT_FALSE(e.enableCoupledWaterCheckpointing().empty()); c.options.water_age=false;
    c.node_subtypes.storages.evap_frac[0]=1;
    EXPECT_FALSE(e.enableCoupledWaterCheckpointing().empty()); c.node_subtypes.storages.evap_frac[0]=0;
    c.control_rules.rule_text.push_back("RULE Unqualified");
    EXPECT_FALSE(e.enableCoupledWaterCheckpointing().empty()); c.control_rules.rule_text.clear();
    const auto error=e.enableCoupledWaterCheckpointing(); ASSERT_TRUE(error.empty()) << error;
    EXPECT_FALSE(e.enableCoupledWaterCheckpointing().empty());
    ASSERT_TRUE(e.saveCheckpoint(evidence("water-opt-in.bin").string()).empty());
    EXPECT_EQ(e.close(),SWMM_OK);
    ASSERT_NO_FATAL_FAILURE(start(e,"water-reopened",input,false));
    EXPECT_FALSE(e.enableCheckpointing().empty());
    ASSERT_TRUE(e.enableCoupledWaterCheckpointing().empty()); EXPECT_EQ(e.close(),SWMM_OK);
}
TEST(CoupledWaterCheckpoint, ChecksumValidInvalidPhysicalProposalsRefuseBeforePublication) {
    SWMMEngine source,target;
    ASSERT_NO_FATAL_FAILURE(start(source,"water-forge-source",model(true)));
    ASSERT_NO_FATAL_FAILURE(start(target,"water-forge-target",model(true)));
    const auto initial=evidence("water-forge-initial.bin"), good=evidence("water-forge-good.bin"), copy=evidence("water-forge-copy.bin");
    ASSERT_TRUE(target.saveCheckpoint(initial.string()).empty()); const auto before=read(initial);
    double elapsed=0; for(int i=0;i<3;++i) ASSERT_EQ(source.step(&elapsed),SWMM_OK);
    ASSERT_TRUE(source.saveCheckpoint(good.string()).empty()); const auto valid=read(good);
    std::vector<std::pair<std::string,std::string>> bad;
    {
        Archive in(valid); std::string magic,identity,body; std::uint32_t version,sum;
        in(magic,version,identity,sum,body); in.finish(); ASSERT_EQ(version,3004u);
        version=3003; Archive out;out(magic,version,identity,sum,body);
        bad.push_back({"other-profile-version",out.bytes()});
    }
    bad.push_back({"undelivered-volume",editNode(valid,12,1)});
    bad.push_back({"external-flow",editNode(valid,4,1)});
    bad.push_back({"nonfinite-memory",editNode(valid,38,std::numeric_limits<double>::infinity())});
    bad.push_back({"negative-volume",editNode(valid,2,-1)});
    // A pending queue cannot be paired with an empty delivery interval.
    bad.push_back({"queue-without-span",editNode(before,13,1)});
    bad.push_back({"water-head-closure",editSurface(valid,[&](std::string& surface) {
        meshVector(surface,"volume",[](char* v,std::size_t n){ addFirstDouble(v,n,1); },"native coupled water mesh state v1");
    },water_magic)});
    auto forged_mb=source.context().mass_balance_2d; forged_mb.coupling_1d_to_2d_in+=1;
    const auto mb=encodeWaterBalance(source.context().mass_balance_2d), changed=encodeWaterBalance(forged_mb);
    bad.push_back({"water-ledger",editSurface(valid,[&](std::string& surface) {
        const auto at=surface.find(mb); if(at==std::string::npos) throw std::runtime_error("Missing water ledger.");
        surface.replace(at,mb.size(),changed);
    },water_magic)});
    for(const auto& [name,bytes]:bad) {
        SCOPED_TRACE(name); const auto path=evidence("water-invalid-"+name+".bin"); std::ofstream(path,std::ios::binary)<<bytes;
        CheckpointInspection inspected; inspected.engine_seconds=123;
        EXPECT_FALSE(target.inspectCheckpoint(path.string(),inspected).empty()); EXPECT_EQ(inspected.engine_seconds,123);
        const auto error=target.loadCheckpoint(path.string()); EXPECT_FALSE(error.empty()) << name;
        ASSERT_TRUE(target.saveCheckpoint(copy.string()).empty()); EXPECT_EQ(read(copy),before);
    }
    const auto error=target.loadCheckpoint(good.string()); ASSERT_TRUE(error.empty()) << error;
    ASSERT_TRUE(target.saveCheckpoint(copy.string()).empty()); EXPECT_EQ(read(copy),valid);
    EXPECT_EQ(source.close(),SWMM_OK); EXPECT_EQ(target.close(),SWMM_OK);
}
TEST(CoupledWaterCheckpoint, MaterialOwnerAndWaterProfileRemainSeparate) {
    auto input=model(true); const auto at=input.find("IGNORE_QUALITY YES");
    ASSERT_NE(at,std::string::npos); input.replace(at,std::strlen("IGNORE_QUALITY YES"),"IGNORE_QUALITY NO");
    input+="[POLLUTANTS]\nDye MG/L 0 0 0 0 NO * 0 0 1\n[2D_INITIAL_QUALITY]\n* Dye 1\n";
    SWMMEngine material,water;
    ASSERT_NO_FATAL_FAILURE(start(material,"water-material",input,false));
    EXPECT_FALSE(material.enableCoupledWaterCheckpointing().empty());
    auto error=material.attachLivePassiveExchange(); ASSERT_TRUE(error.empty()) << error;
    error=material.enableCheckpointing(); ASSERT_TRUE(error.empty()) << error;
    ASSERT_NO_FATAL_FAILURE(start(water,"water-profile",model(true)));
    const auto a=evidence("water-material.bin"),b=evidence("water-profile.bin"),copy=evidence("water-profile-copy.bin");
    ASSERT_TRUE(material.saveCheckpoint(a.string()).empty()); const auto prior=read(a);
    ASSERT_TRUE(water.saveCheckpoint(b.string()).empty()); const auto fresh=read(b);
    EXPECT_FALSE(water.loadCheckpoint(a.string()).empty()); EXPECT_FALSE(material.loadCheckpoint(b.string()).empty());
    ASSERT_TRUE(material.saveCheckpoint(copy.string()).empty()); EXPECT_EQ(read(copy),prior);
    ASSERT_TRUE(water.saveCheckpoint(copy.string()).empty()); EXPECT_EQ(read(copy),fresh);
    EXPECT_EQ(material.close(),SWMM_OK); EXPECT_EQ(water.close(),SWMM_OK);
}
