// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include <gtest/gtest.h>
#include "core/SWMMEngine.hpp"
#include "core/CheckpointInspection.hpp"
#include "quality/LivePassiveStorageExchange.hpp"
#include "2d/coupling/NodeCoupling.hpp"
#include "live_passive_recovery_test_support.hpp"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>

namespace {
using namespace openswmm;
using quality::NativeAmountSum;
std::filesystem::path evidence(const std::string& name) {
    const char* override = std::getenv("OPENSWMM_CHANGING_VOLUME_EVIDENCE");
    const std::filesystem::path root = override ? override : CHANGING_VOLUME_TEST_OUTPUT;
    std::filesystem::create_directories(root); return root / name;
}
std::string read(const std::filesystem::path& p) {
    std::ifstream f(p,std::ios::binary); return {std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};
}
// Same authored closed six-point geometry as the water-only qualification.
// The pollutant and receiving law are the only additional physical ownership.
std::string model(bool positive,bool tail) {
    std::ostringstream s; s << std::setprecision(17) <<
        "[OPTIONS]\nFLOW_UNITS CMS\nFLOW_ROUTING DYNWAVE\nSTART_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
        "END_DATE 01/01/2026\nEND_TIME 00:00:01\nROUTING_STEP " << (tail?.125:1./1024) <<
        "\nVARIABLE_STEP 0\nREPORT_STEP 00:00:01\nTHREADS 1\nALLOW_PONDING YES\nIGNORE_QUALITY NO\n"
        "[STORAGE]\nTank -10 1 10 FUNCTIONAL 0 0 " << 2*.02832/(.3048*.3048*.3048)
        << ' ' << 2*.02832/(.3048*.3048*.3048) << " 0\n"
        "[2D_OPTIONS]\nINTEGRATOR EXPLICIT\nBACKEND CPU\nLTS_TIERS 1\nMAX_TIMESTEP " << (tail?.25:1./512)
        << "\nCOUPLING_SYNC " << (tail?.25:1./512) << "\nREPORT_2D NO\n[2D_VERTICES]\n";
    for (int k=0;k<3;++k) {
        const int x=8*k;
        s<<x<<" 0 -1.5\n"<<x+2<<" 0 -1.5\n"<<x<<" 1 -1.5\n"
         <<x<<" -1 -1.5\n"<<x<<" -2 -1.5\n"<<x+2<<" -1 -1.5\n";
    }
    s<<"[2D_TRIANGLES]\n";
    for (int k=0;k<6;++k) {
        const double depth=tail ? 1.5+(k%2?-(positive?1:1.2):(positive?1.2:1))*2e-12 :
            (k%2?(positive?1.25:1.2):(positive?1.8:1.75));
        s<<3*k<<' '<<3*k+1<<' '<<3*k+2<<" .03 "<<depth<<'\n';
    }
    s<<"[2D_VERTEX_NODE_MAP]\n";
    for (int k=0;k<6;++k) s<<3*k<<" Tank .7 "<<(tail?.1:.01)<<'\n';
    s<<"[POLLUTANTS]\nDye MG/L 0 0 0 0 NO * 0 0 "<<(positive?1:2)
     <<"\n[2D_INITIAL_QUALITY]\n* Dye "<<(positive?2:1)<<"\n[REPORT]\nINPUT NO\n";
    return s.str();
}
void start(SWMMEngine& e,const std::string& tag,const std::string& input,bool checkpoint=true,bool attach=true,
           bool preflight=false,std::size_t capacity=4096) {
    std::ofstream(evidence(tag+".inp"))<<input;
    ASSERT_EQ(e.open(evidence(tag+".inp").c_str(),evidence(tag+".rpt").c_str(),nullptr),SWMM_OK);
    ASSERT_EQ(e.initialize(),SWMM_OK);ASSERT_EQ(e.start(0),SWMM_OK);
    if(!attach)return;
    auto error=e.attachLiveChangingVolumeExchange(capacity,preflight);ASSERT_TRUE(error.empty())<<error;
    if(checkpoint) {error=e.enableCheckpointing();ASSERT_TRUE(error.empty())<<error;}
}
void sameWater(const SWMMEngine& a,const SWMMEngine& b) {
    const auto& x=a.context();const auto& y=b.context();
    EXPECT_EQ(x.current_time,y.current_time);EXPECT_EQ(x.coupling_delivery_remaining,y.coupling_delivery_remaining);
#define SAME(f) EXPECT_EQ(x.nodes.f,y.nodes.f)<<#f;
    SAME(volume) SAME(old_volume) SAME(depth) SAME(old_depth) SAME(head) SAME(inflow) SAME(outflow)
    SAME(old_net_inflow) SAME(old_lat_flow) SAME(old_inflow) SAME(coupling_queue) SAME(coupling_inflow) SAME(coupling_volume)
#undef SAME
    const auto& s=a.surfaceRouter2D().state();const auto& t=b.surfaceRouter2D().state();
    EXPECT_EQ(s.volume,t.volume);EXPECT_EQ(s.depth,t.depth);EXPECT_EQ(s.head,t.head);EXPECT_EQ(s.edge_flux,t.edge_flux);
    EXPECT_EQ(a.surfaceRouter2D().couplingExchangeVolumes(),b.surfaceRouter2D().couplingExchangeVolumes());
}
void authored(bool positive,bool tail) {
    const std::string tag=std::string(tail?"tail-":"flow-")+(positive?"positive":"negative");
    SWMMEngine source,ordinary;const auto input=model(positive,tail);
    ASSERT_NO_FATAL_FAILURE(start(source,tag,input));
    ASSERT_NO_FATAL_FAILURE(start(ordinary,tag+"-ordinary",input,false,false));
    const double initial=source.context().nodes.volume[0]*source.context().nodes.conc[0];
    std::vector<std::unique_ptr<SWMMEngine>> restored;
    std::ofstream trace(evidence(tag+"-material.txt"));
    std::ofstream amounts(evidence(tag+"-amounts.txt"));amounts<<std::hexfloat;
    trace<<"# step volume old_volume concentration old_concentration pending_drain pending_spill roundoff_parts...\n"<<std::hexfloat;
    double elapsed=0,prior_volume=0,prior_concentration=0;
    for(int step=0;step<=7;++step) {
        if(step) {
            prior_volume=source.context().nodes.volume[0];prior_concentration=source.context().nodes.conc[0];
            ASSERT_EQ(source.step(&elapsed),SWMM_OK)<<source.context().error_message;
            ASSERT_EQ(ordinary.step(&elapsed),SWMM_OK)<<ordinary.context().error_message;
            for(auto& e:restored) ASSERT_EQ(e->step(&elapsed),SWMM_OK)<<e->context().error_message;
        }
        const auto& n=source.context().nodes;const auto& book=source.livePassiveExchange()->receipts();
        ASSERT_NO_FATAL_FAILURE(sameWater(source,ordinary));
        NativeAmountSum exact=book.storedRoundoff(0);exact.add(n.volume[0]*n.conc[0]);exact.add(-initial);
        for(const auto& r:book.receipts()) {
            for(const auto& row:r.accepted) {
                const auto drain=row.materialAmount(true);
                for(double part:drain.parts())exact.add(-part);
                exact.add(row.materialAmount(false));
            }
            for(const auto& row:r.remaining) {
                exact.add(row.materialAmount(true));
                const auto spill=row.materialAmount(false);
                for(double part:spill.parts())exact.add(-part);
            }
        }
        EXPECT_TRUE(exact.zero())<<step;
        trace<<step<<' '<<n.volume[0]<<' '<<n.old_volume[0]<<' '<<n.conc[0]<<' '<<n.conc_old[0]<<' '
             <<book.pendingDrainAmount(0).rounded()<<' '<<book.pendingSpillAmount(0).rounded();
        for(double part:book.storedRoundoff(0).parts())trace<<' '<<part;
        trace<<'\n';
        for(const auto& r:book.receipts())
            for(bool remaining:{false,true})
                for(const auto& row:remaining?r.remaining:r.accepted)
                    for(bool drain:{true,false}) {
                        const auto value=row.materialAmount(drain);
                        for(double part:value.parts())amounts<<step<<' '<<remaining<<' '<<drain<<' '<<part<<'\n';
                    }
        if(step==2) {
            EXPECT_EQ(n.coupling_queue[0]>0,positive);
            const auto& points=source.surfaceRouter2D().couplingExchangeVolumes();ASSERT_EQ(points.size(),6u);
            for(int k=0;k<6;++k)EXPECT_EQ(points[k]>0,k%2==0);
        }
        if(step==3) {
            ASSERT_FALSE(book.receipts().empty());EXPECT_EQ(book.receipts()[0].half_state,1);
            EXPECT_EQ(n.volume[0]*n.conc[0]>initial,positive);
        }
        if(tail && step==5) {
            EXPECT_EQ(n.coupling_queue[0],0);EXPECT_EQ(n.coupling_inflow[0],0);
            EXPECT_NE(n.volume[0],prior_volume);EXPECT_NE(n.conc[0],prior_concentration);
            EXPECT_EQ(n.conc[0],(prior_concentration*prior_volume)/n.volume[0]);
            EXPECT_TRUE(book.pendingDrainAmount(0).zero());EXPECT_TRUE(book.pendingSpillAmount(0).zero());
        }
        const auto saved=evidence(tag+"-"+std::to_string(step)+".bin"),copy=evidence(tag+"-copy.bin");
        auto error=source.saveCheckpoint(saved.string());ASSERT_TRUE(error.empty())<<error;
        for(auto& e:restored) {
            error=e->saveCheckpoint(copy.string());ASSERT_TRUE(error.empty())<<error;
            EXPECT_EQ(read(saved),read(copy));
        }
        auto e=std::make_unique<SWMMEngine>();ASSERT_NO_FATAL_FAILURE(start(*e,tag+"-restore-"+std::to_string(step),input));
        ASSERT_TRUE(e->saveCheckpoint(copy.string()).empty());const auto fresh=read(copy);
        CheckpointInspection inspection;
        error=e->inspectCheckpoint(saved.string(),inspection);ASSERT_TRUE(error.empty())<<error;
        ASSERT_TRUE(e->saveCheckpoint(copy.string()).empty());EXPECT_EQ(read(copy),fresh);
        error=e->loadCheckpoint(saved.string());ASSERT_TRUE(error.empty())<<error;
        ASSERT_TRUE(e->saveCheckpoint(copy.string()).empty());EXPECT_EQ(read(copy),read(saved));
        restored.push_back(std::move(e));
    }
    for(auto& e:restored)EXPECT_EQ(e->close(),SWMM_OK);
    EXPECT_EQ(source.close(),SWMM_OK);EXPECT_EQ(ordinary.close(),SWMM_OK);
}
}
TEST(ChangingVolumeExchange, AuthoredPositiveMaterialAndWaterRecover) { authored(true,false); }
TEST(ChangingVolumeExchange, AuthoredNegativeMaterialAndWaterRecover) { authored(false,false); }
TEST(ChangingVolumeExchange, AuthoredPositiveEmptyQueueTailRenormalizesInventory) { authored(true,true); }
TEST(ChangingVolumeExchange, AuthoredNegativeEmptyQueueTailRenormalizesInventory) { authored(false,true); }
TEST(ChangingVolumeExchange, SpillAdmissionUsesSettledInventoryBeforeQuality) {
    for(double requested:{100.,150.}) {
        SimulationContext ctx;ctx.node_names.add("Tank");ctx.pollutant_names.add("Dye");ctx.allocate_objects();
        ctx.options.routing_model=RoutingModel::DYNWAVE;
        ctx.node_subtypes.set_node_type(ctx.nodes,0,NodeType::STORAGE);
        ctx.nodes.volume[0]=ctx.nodes.old_volume[0]=100;ctx.nodes.depth[0]=ctx.nodes.old_depth[0]=1;
        ctx.nodes.conc[0]=ctx.nodes.conc_old[0]=1;
        twoD::SurfaceStateData surface;surface.resize(1,3);surface.transport.resize(1,1,1);surface.transport.n_pollut=1;
        std::vector<twoD::CouplingPoint> points{{0,-1,0,.7,.01,false,false}};
        std::vector<double> published{1};surface.nodes_1d=&ctx.nodes;surface.node_row_conc=&published;surface.node_coupling=&points;
        quality::LivePassiveStorageExchange owner(ctx,surface,points,1,8,
            quality::PassiveStorageArithmetic::RETAIN_EXACT_HALVES,false,quality::PassiveStorageLaw::CHANGING_VOLUME);
        owner.freeze(ctx,1);owner.observeDrain(ctx);
        // Manufactured unit-level receiving observation; authored engine tests
        // above supply the physical hydraulic qualification.
        ctx.nodes.volume[0]=200;ctx.nodes.depth[0]=2;
        owner.beginWindow(ctx,surface,0,1000,quality::PassiveStorageExchange::Phase::ADVANCE);
        const bool admitted=owner.admit(0,0,0,-requested,requested,0,0,0,1,requested);
        EXPECT_EQ(admitted,requested==100);
        if(admitted) {
            owner.applied(100,100);
            EXPECT_FALSE(owner.admit(0,0,0,-1,1,100,100,0,1,1));
        }
        EXPECT_TRUE(owner.failed());EXPECT_EQ(ctx.nodes.conc[0],1);EXPECT_EQ(surface.transport.cell_mass[0],0);
        EXPECT_TRUE(owner.receipts().pendingSpillAmount(0).zero());
    }
}
namespace {
using namespace live_recovery_test;
constexpr const char* changing_magic="OpenSWMM paired changing-volume surface boundary";
std::string editReceiving(std::string bytes,const std::function<void(std::string&)>& edit) {
    return editSurface(std::move(bytes),[&](std::string& surface) {
        editBlob(surface,"OpenSWMM live passive owner",[&](std::string& live) {
            editOwner(live,[&](std::string& payload) {
                editBlob(payload,"OpenSWMM changing-volume storage receipt owner",[&](std::string& book) { editOwner(book,edit); });
            });
        });
    },changing_magic);
}
void replaceUnique(std::string& payload,const std::string& from,const std::string& to) {
    const auto at=payload.find(from);
    if(at==std::string::npos || payload.find(from,at+1)!=std::string::npos)throw std::runtime_error("Missing/ambiguous test observation.");
    payload.replace(at,from.size(),to);
}
std::string observedWater(const SimulationContext& ctx,int change=-1) {
    const auto& n=ctx.nodes;Archive a;int field=0;
    for(const auto* values:{&n.volume,&n.old_volume,&n.depth,&n.old_depth,&n.inflow,&n.outflow,
        &n.old_net_inflow,&n.old_lat_flow,&n.old_inflow,&n.coupling_inflow,&n.coupling_queue}) {
        auto copy=*values;if(field++==change)copy[0]+=1;a(copy);
    }
    return a.bytes();
}
std::string emptyPublication(const SWMMEngine& source,int change=-1) {
    using Phase=quality::PassiveStorageExchange::Phase;
    const auto& ctx=source.context();Archive a;
    auto phase=change==0?Phase::SETTLED:Phase::ADVANCE;
    double capture=ctx.elapsed_ms-125,start=ctx.elapsed_ms-250,end=ctx.elapsed_ms,eligible=end,span=0;
    std::uint64_t generation=3,receipt=0;
    double dt=.25,after=change==2?1:0,factor=source.surfaceRouter2D().options().flow_2d_to_1d;
    if(change==1)start=-1;
    a(phase,capture,start,end,eligible,span,generation,receipt,dt,after,factor);
    const auto& points=*source.surfaceRouter2D().state().node_coupling;
    std::uint64_t size=change==3?std::numeric_limits<std::uint64_t>::max():points.size();a(size);
    for(std::size_t k=0;k<points.size();++k) {auto point=std::uint64_t(k);auto cell=points[k].cell_idx,node=points[k].node_idx;double exchange=0;a(point,cell,node,exchange);}
    std::vector<double> queue{0};a(queue,queue);size=1;a(size);
    std::uint64_t parts=change==4?1:0;a(parts);if(parts) {double part=1;a(part);}
    return a.bytes();
}
std::string corruptNative(std::string bytes,int field) {
    return editSurface(std::move(bytes),[&](std::string& surface) {
        editBlob(surface,"OpenSWMM accepted DW hydraulic boundary",[&](std::string& child) {
            Archive file(child);std::string name,identity,body;std::uint32_t version,sum;
            file(name,version,identity,sum,body);file.finish();Archive in(body),prefix;
            std::vector<double> water,reverse,heat,pollutant;
            double time,date,elapsed,old_elapsed,report,evap,infil,recovery,link_span;bool links;
            in(water,reverse,heat,pollutant,time,date,elapsed,old_elapsed,report,evap,infil,recovery,links,link_span);
            prefix(water,reverse,heat,pollutant,time,date,elapsed,old_elapsed,report,evap,infil,recovery,links,link_span);
            auto cursor=prefix.bytes().size();
            for(int k=0;k<=field;++k) {
                std::uint64_t n;std::memcpy(&n,body.data()+cursor,8);cursor+=8;
                if(n>(body.size()-cursor)/8)throw std::runtime_error("Invalid test node extent.");
                if(k==field)addFirstDouble(body.data()+cursor,n,1);cursor+=n*8;
            }
            sum=crc(body);Archive out;out(name,version,identity,sum,body);child=out.bytes();
        });
    },changing_magic);
}
}
TEST(ChangingVolumeExchange, ChecksummedInvalidHistoryRefusesBeforePublicationAndAllowsValidRetry) {
    for(bool positive:{true,false}) {
        const auto tag=std::string("invalid-")+(positive?"positive":"negative");
        SWMMEngine source,target;const auto input=model(positive,true);
        ASSERT_NO_FATAL_FAILURE(start(source,tag+"-source",input));
        ASSERT_NO_FATAL_FAILURE(start(target,tag+"-target",input));
        std::string observed,empty;std::vector<std::string> changed_observations,changed_empty;
        double elapsed=0;
        for(int step=1;step<=7;++step) {
            ASSERT_EQ(source.step(&elapsed),SWMM_OK)<<source.context().error_message;
            if(step==3) { observed=observedWater(source.context());for(int k:{0,1,4,6,7,9,10})changed_observations.push_back(observedWater(source.context(),k)); }
            if(step==4) { empty=emptyPublication(source);for(int k=0;k<5;++k)changed_empty.push_back(emptyPublication(source,k)); }
        }
        const auto good=evidence(tag+"-good.bin"),copy=evidence(tag+"-copy.bin");
        ASSERT_TRUE(source.saveCheckpoint(good.string()).empty());const auto valid=read(good);
        ASSERT_TRUE(target.saveCheckpoint(copy.string()).empty());const auto fresh=read(copy);
        std::vector<std::string> invalid;
        for(const auto& changed:changed_observations)invalid.push_back(editReceiving(valid,[&](auto& p){replaceUnique(p,observed,changed);}));
        for(const auto& changed:changed_empty)invalid.push_back(editReceiving(valid,[&](auto& p){replaceUnique(p,empty,changed);}));
        for(int field:{2,11,13,30,31,38,42})invalid.push_back(corruptNative(valid,field));
        auto proof=source.livePassiveExchange()->receipts().receipts().front().proof;
        auto changed=proof;changed.legs.front().sample+=1;
        invalid.push_back(editReceiving(valid,[&](auto& p){replaceUnique(p,encodeProof(proof),encodeProof(changed));}));
        for(std::size_t k=0;k<invalid.size();++k) {
            SCOPED_TRACE(tag+" proposal="+std::to_string(k));const auto bad=evidence(tag+"-"+std::to_string(k)+".bin");
            std::ofstream(bad,std::ios::binary)<<invalid[k];CheckpointInspection inspection;inspection.engine_seconds=123;
            auto error=target.inspectCheckpoint(bad.string(),inspection);EXPECT_FALSE(error.empty());EXPECT_EQ(inspection.engine_seconds,123);
            EXPECT_EQ(error.find("checksum"),std::string::npos)<<error;
            EXPECT_FALSE(target.loadCheckpoint(bad.string()).empty());ASSERT_TRUE(target.saveCheckpoint(copy.string()).empty());EXPECT_EQ(read(copy),fresh);
        }
        auto error=target.loadCheckpoint(good.string());ASSERT_TRUE(error.empty())<<error;
        ASSERT_TRUE(target.saveCheckpoint(copy.string()).empty());EXPECT_EQ(read(copy),valid);
        EXPECT_EQ(source.close(),SWMM_OK);EXPECT_EQ(target.close(),SWMM_OK);
    }
}
TEST(ChangingVolumeExchange, FixedAndChangingLawsRefuseEachOthersInitialArchives) {
    SWMMEngine changing,fixed;const auto input=model(true,false);
    ASSERT_NO_FATAL_FAILURE(start(changing,"law-changing",input));
    ASSERT_NO_FATAL_FAILURE(start(fixed,"law-fixed",input,false,false));
    ASSERT_TRUE(fixed.attachLivePassiveExchange(quality::PassiveStorageArithmetic::RETAIN_EXACT_HALVES).empty());
    ASSERT_TRUE(fixed.enableCheckpointing().empty());
    const auto a=evidence("law-changing.bin"),b=evidence("law-fixed.bin"),copy=evidence("law-copy.bin");
    ASSERT_TRUE(changing.saveCheckpoint(a.string()).empty());ASSERT_TRUE(fixed.saveCheckpoint(b.string()).empty());
    const auto before_a=read(a),before_b=read(b);
    EXPECT_FALSE(changing.loadCheckpoint(b.string()).empty());EXPECT_FALSE(fixed.loadCheckpoint(a.string()).empty());
    ASSERT_TRUE(changing.saveCheckpoint(copy.string()).empty());EXPECT_EQ(read(copy),before_a);
    ASSERT_TRUE(fixed.saveCheckpoint(copy.string()).empty());EXPECT_EQ(read(copy),before_b);
    EXPECT_EQ(changing.close(),SWMM_OK);EXPECT_EQ(fixed.close(),SWMM_OK);
}
TEST(ChangingVolumeExchange, UncheckpointedExecutionRefusesAnotherSurfaceOwnerBeforeCredit) {
    SWMMEngine e;ASSERT_NO_FATAL_FAILURE(start(e,"profile-refusal",model(true,false),false,false));
    auto& surface=e.surfaceRouter2D().state();ASSERT_FALSE(surface.rainfall.empty());
    surface.rainfall[0]=1;
    EXPECT_FALSE(e.attachLiveChangingVolumeExchange().empty());EXPECT_EQ(e.livePassiveExchange(),nullptr);
    surface.rainfall[0]=0;ASSERT_TRUE(e.attachLiveChangingVolumeExchange().empty());
    const double volume=e.context().nodes.volume[0];
    surface.rainfall[0]=1;double elapsed=0;
    EXPECT_NE(e.step(&elapsed),SWMM_OK);EXPECT_EQ(e.livePassiveExchange()->legCount(),0);
    EXPECT_EQ(e.context().nodes.volume[0],volume);EXPECT_EQ(e.context().nodes.coupling_queue[0],0);
    EXPECT_TRUE(e.livePassiveExchange()->failed());e.close();
}
TEST(ChangingVolumeExchange, ResolvedEqualizationPolicyRefusesAttachment) {
    auto input=model(true,false);const auto at=input.find("BACKEND CPU");ASSERT_NE(at,std::string::npos);
    input.replace(at,std::strlen("BACKEND CPU"),"BACKEND CPU\nCOUPLING_EQUALIZE YES");
    SWMMEngine e;ASSERT_NO_FATAL_FAILURE(start(e,"equalize-refusal",input,false,false));
    const auto* points=e.surfaceRouter2D().state().node_coupling;ASSERT_NE(points,nullptr);
    ASSERT_FALSE(points->empty());EXPECT_TRUE(points->front().equalize);
    EXPECT_FALSE(e.attachLiveChangingVolumeExchange().empty());EXPECT_EQ(e.livePassiveExchange(),nullptr);
    EXPECT_EQ(e.context().current_time,0);EXPECT_EQ(e.close(),SWMM_OK);
}
TEST(ChangingVolumeExchange, ReportPublicationRecoversAndContinuesWithNativeWater) {
    for(bool positive:{true,false}) {
        const auto tag=std::string("report-")+(positive?"positive":"negative");
        SWMMEngine source,target;const auto input=model(positive,true);
        ASSERT_NO_FATAL_FAILURE(start(source,tag+"-source",input));
        ASSERT_NO_FATAL_FAILURE(start(target,tag+"-target",input));
        double elapsed=0;ASSERT_EQ(source.step(&elapsed),SWMM_OK);
        ASSERT_NO_THROW(source.surfaceRouter2D().flushPendingBatch(source.context()));
        const auto& receipts=source.livePassiveExchange()->receipts().receipts();ASSERT_FALSE(receipts.empty());
        EXPECT_EQ(receipts.back().publication.phase,quality::PassiveStorageExchange::Phase::REPORT);
        const auto saved=evidence(tag+".bin"),copy=evidence(tag+"-copy.bin");
        auto error=source.saveCheckpoint(saved.string());ASSERT_TRUE(error.empty())<<error;
        error=target.loadCheckpoint(saved.string());ASSERT_TRUE(error.empty())<<error;
        for(int step=0;step<5;++step) {
            ASSERT_EQ(source.step(&elapsed),SWMM_OK)<<source.context().error_message;
            ASSERT_EQ(target.step(&elapsed),SWMM_OK)<<target.context().error_message;
            ASSERT_TRUE(source.saveCheckpoint(saved.string()).empty());ASSERT_TRUE(target.saveCheckpoint(copy.string()).empty());
            EXPECT_EQ(read(copy),read(saved));
        }
        EXPECT_EQ(source.close(),SWMM_OK);EXPECT_EQ(target.close(),SWMM_OK);
    }
}
TEST(ChangingVolumeExchange, FinalPublicationKeepsItsFundedPendingOwnerReplayable) {
    SWMMEngine source,target;const auto input=model(true,true);
    ASSERT_NO_FATAL_FAILURE(start(source,"final-source",input));
    ASSERT_NO_FATAL_FAILURE(start(target,"final-target",input));
    double elapsed=0;ASSERT_EQ(source.step(&elapsed),SWMM_OK);ASSERT_EQ(source.end(),SWMM_OK);
    const auto* owner=source.livePassiveExchange();ASSERT_NE(owner,nullptr);
    ASSERT_FALSE(owner->receipts().receipts().empty());
    EXPECT_EQ(owner->receipts().receipts().back().publication.phase,quality::PassiveStorageExchange::Phase::FINAL);
    const auto bytes=owner->checkpoint(source.context(),source.surfaceRouter2D().state());
    auto prepared=target.livePassiveExchange()->prepareRestore(source.context(),source.surfaceRouter2D().state(),bytes);
    ASSERT_NE(prepared,nullptr);
    EXPECT_EQ(prepared->checkpoint(source.context(),source.surfaceRouter2D().state()),bytes);
    std::ofstream(evidence("final-owner.bin"),std::ios::binary)<<bytes;
    EXPECT_EQ(source.close(),SWMM_OK);EXPECT_EQ(target.close(),SWMM_OK);
}

namespace {
std::string livePayload(const SWMMEngine& e) {
    const auto bytes=e.livePassiveExchange()->checkpoint(e.context(),e.surfaceRouter2D().state());
    live_recovery_test::Archive in(bytes);
    std::string magic,payload;std::uint32_t version,endian,checksum;
    in(magic,version,endian,checksum,payload);in.finish();
    EXPECT_EQ(version,e.livePassiveExchange()->preflightsHistory()?9u:8u);
    return payload;
}
void boundedHistory(bool positive,bool tail) {
    const std::string tag=std::string("preflight-")+(tail?"tail-":"flow-")+(positive?"positive":"negative");
    const std::size_t capacity=tail?6:64;
    auto input=model(positive,tail);
    const auto end=input.find("END_TIME 00:00:01");ASSERT_NE(end,std::string::npos);
    input.replace(end,std::strlen("END_TIME 00:00:01"),"END_TIME 00:00:10");
    SWMMEngine source,control;
    ASSERT_NO_FATAL_FAILURE(start(source,tag+"-source",input,true,true,true,capacity));
    ASSERT_NO_FATAL_FAILURE(start(control,tag+"-control",input,true,true,false,capacity));
    std::vector<std::unique_ptr<SWMMEngine>> restored;
    bool refused=false;int accepted=0;double elapsed=0;
    const auto copy=evidence(tag+"-copy.bin");
    for(int step=0;step<40;++step) {
        const auto saved=evidence(tag+"-"+std::to_string(step)+".bin");
        auto error=source.saveCheckpoint(saved.string());ASSERT_TRUE(error.empty())<<error;
        const auto bytes=read(saved);
        std::ofstream(evidence(tag+"-"+std::to_string(step)+".owner"),std::ios::binary)
            <<source.livePassiveExchange()->checkpoint(source.context(),source.surfaceRouter2D().state());
        const auto time=source.context().elapsed_ms;
        elapsed=12345;
        if(source.step(&elapsed)!=SWMM_OK) {
            refused=true;EXPECT_EQ(elapsed,12345);EXPECT_EQ(source.context().elapsed_ms,time);
            EXPECT_EQ(source.context().state,EngineState::RUNNING);EXPECT_EQ(source.context().error_code,SWMM_OK);
            EXPECT_FALSE(source.livePassiveExchange()->failed());
            EXPECT_NE(source.livePassiveExchange()->historyPreflightError().find("work exceeds"),std::string::npos);
            EXPECT_EQ(livePayload(source),livePayload(control));ASSERT_NO_FATAL_FAILURE(sameWater(source,control));
            ASSERT_TRUE(source.saveCheckpoint(copy.string()).empty());EXPECT_EQ(read(copy),bytes);
            auto fresh=std::make_unique<SWMMEngine>();
            ASSERT_NO_FATAL_FAILURE(start(*fresh,tag+"-refusal-restored",input,true,true,true,capacity));
            error=fresh->loadCheckpoint(saved.string());ASSERT_TRUE(error.empty())<<error;
            restored.push_back(std::move(fresh));
            for(auto& e:restored) {
                for(int retry=0;retry<2;++retry) {
                    elapsed=12345;EXPECT_NE(e->step(&elapsed),SWMM_OK);EXPECT_EQ(elapsed,12345);
                    EXPECT_EQ(e->livePassiveExchange()->historyPreflightError(),source.livePassiveExchange()->historyPreflightError());
                    EXPECT_FALSE(e->livePassiveExchange()->failed());
                    ASSERT_TRUE(e->saveCheckpoint(copy.string()).empty());EXPECT_EQ(read(copy),bytes);
                }
            }
            elapsed=12345;EXPECT_NE(source.step(&elapsed),SWMM_OK);EXPECT_EQ(elapsed,12345);
            ASSERT_TRUE(source.saveCheckpoint(copy.string()).empty());EXPECT_EQ(read(copy),bytes);
            std::ofstream(evidence(tag+"-summary.json"))<<"{\"accepted_steps\":"<<accepted
                <<",\"capacity\":"<<capacity<<",\"applied_legs\":"<<source.livePassiveExchange()->legCount()
                <<",\"restored_owners\":"<<restored.size()<<"}\n";
            if(tail) {
                EXPECT_EQ(source.livePassiveExchange()->legCount(),capacity);
                EXPECT_TRUE(source.livePassiveExchange()->receipts().pendingDrainAmount(0).zero());
                EXPECT_TRUE(source.livePassiveExchange()->receipts().pendingSpillAmount(0).zero());
                EXPECT_GE(accepted,7);
            } else EXPECT_GE(accepted,3);
            // The preceding accepted step reserved its possible pending flush.
            // FINAL needs no additional quality delivery and must still fit.
            for(auto* e:{&source,restored.back().get()}) {
                ASSERT_EQ(e->end(),SWMM_OK)<<e->context().error_message;
                EXPECT_FALSE(e->livePassiveExchange()->failed());
            }
            EXPECT_EQ(livePayload(source),livePayload(*restored.back()));
            std::ofstream(evidence(tag+"-final.owner"),std::ios::binary)
                <<source.livePassiveExchange()->checkpoint(source.context(),source.surfaceRouter2D().state());
            break;
        }
        ++accepted;
        ASSERT_EQ(control.step(&elapsed),SWMM_OK)<<control.context().error_message;
        ASSERT_NO_FATAL_FAILURE(sameWater(source,control));EXPECT_EQ(livePayload(source),livePayload(control));
        for(auto& e:restored) {
            ASSERT_EQ(e->step(&elapsed),SWMM_OK)<<e->context().error_message;
            EXPECT_EQ(livePayload(source),livePayload(*e));
        }
        ASSERT_TRUE(source.saveCheckpoint(copy.string()).empty());const auto after=read(copy);
        auto e=std::make_unique<SWMMEngine>();
        ASSERT_NO_FATAL_FAILURE(start(*e,tag+"-restore-"+std::to_string(accepted),input,true,true,true,capacity));
        error=e->loadCheckpoint(copy.string());ASSERT_TRUE(error.empty())<<error;
        ASSERT_TRUE(e->saveCheckpoint(copy.string()).empty());EXPECT_EQ(read(copy),after);
        restored.push_back(std::move(e));
    }
    EXPECT_TRUE(refused);
    for(auto& e:restored)EXPECT_EQ(e->close(),SWMM_OK);
    EXPECT_EQ(source.close(),SWMM_OK);EXPECT_EQ(control.close(),SWMM_OK);
}
}
TEST(ChangingVolumeHistoryPreflight, PositiveHistoryRefusesBeforeMutationAndRestores) { boundedHistory(true,false); }
TEST(ChangingVolumeHistoryPreflight, NegativeHistoryRefusesBeforeMutationAndRestores) { boundedHistory(false,false); }
TEST(ChangingVolumeHistoryPreflight, PositiveEmptyWindowsRemainBoundedAfterJournalFills) { boundedHistory(true,true); }
TEST(ChangingVolumeHistoryPreflight, NegativeEmptyWindowsRemainBoundedAfterJournalFills) { boundedHistory(false,true); }

TEST(ChangingVolumeHistoryPreflight, InitialPolicyIdentityAndLargeCapacityRefusal) {
    SWMMEngine bounded,earlier,target;const auto input=model(true,false);
    ASSERT_NO_FATAL_FAILURE(start(bounded,"preflight-large",input,true,true,true));
    ASSERT_NO_FATAL_FAILURE(start(earlier,"preflight-earlier",input));
    ASSERT_NO_FATAL_FAILURE(start(target,"preflight-large-target",input,true,true,true));
    const auto a=evidence("preflight-large.bin"),b=evidence("preflight-earlier.bin"),copy=evidence("preflight-large-copy.bin");
    ASSERT_TRUE(bounded.saveCheckpoint(a.string()).empty());ASSERT_TRUE(earlier.saveCheckpoint(b.string()).empty());
    const auto before=read(a),old=read(b);EXPECT_NE(before,old);EXPECT_EQ(livePayload(bounded),livePayload(earlier));
    for(auto pair:{std::make_pair(&bounded,b),std::make_pair(&earlier,a)}) {
        CheckpointInspection inspection;inspection.checkpoint_bytes=12345;const auto sentinel=inspection;
        EXPECT_FALSE(pair.first->inspectCheckpoint(pair.second.string(),inspection).empty());EXPECT_EQ(inspection,sentinel);
        EXPECT_FALSE(pair.first->loadCheckpoint(pair.second.string()).empty());
    }
    double elapsed=12345;EXPECT_NE(bounded.step(&elapsed),SWMM_OK);EXPECT_EQ(elapsed,12345);
    EXPECT_FALSE(bounded.livePassiveExchange()->failed());EXPECT_EQ(bounded.context().elapsed_ms,0);
    EXPECT_NE(bounded.livePassiveExchange()->historyPreflightError().find("work exceeds"),std::string::npos);
    ASSERT_TRUE(bounded.saveCheckpoint(copy.string()).empty());EXPECT_EQ(read(copy),before);
    ASSERT_TRUE(earlier.saveCheckpoint(copy.string()).empty());EXPECT_EQ(read(copy),old);
    ASSERT_TRUE(target.loadCheckpoint(a.string()).empty());
    elapsed=12345;EXPECT_NE(target.step(&elapsed),SWMM_OK);EXPECT_EQ(elapsed,12345);
    ASSERT_TRUE(target.saveCheckpoint(copy.string()).empty());EXPECT_EQ(read(copy),before);
    for(auto* e:{&bounded,&earlier,&target})EXPECT_EQ(e->close(),SWMM_OK);
}

TEST(ChangingVolumeHistoryPreflight, ReportAndFinalFlushRetainReservedCapacity) {
    for(bool positive:{true,false})for(bool final:{true,false}) {
        const std::string tag=std::string("preflight-")+(final?"final-":"report-")+(positive?"positive":"negative");
        SWMMEngine source,target;const auto input=model(positive,true);
        ASSERT_NO_FATAL_FAILURE(start(source,tag+"-source",input,true,true,true,64));
        ASSERT_NO_FATAL_FAILURE(start(target,tag+"-target",input,true,true,true,64));
        double elapsed=0;ASSERT_EQ(source.step(&elapsed),SWMM_OK);
        const auto saved=evidence(tag+".bin"),copy=evidence(tag+"-copy.bin");
        if(final) {
            ASSERT_TRUE(source.saveCheckpoint(saved.string()).empty());ASSERT_TRUE(target.loadCheckpoint(saved.string()).empty());
            for(auto* e:{&source,&target})ASSERT_EQ(e->end(),SWMM_OK);
            EXPECT_EQ(livePayload(source),livePayload(target));
            const auto bytes=source.livePassiveExchange()->checkpoint(source.context(),source.surfaceRouter2D().state());
            SWMMEngine fresh;ASSERT_NO_FATAL_FAILURE(start(fresh,tag+"-fresh",input,true,true,true,64));
            auto owner=fresh.livePassiveExchange()->prepareRestore(source.context(),source.surfaceRouter2D().state(),bytes);
            EXPECT_EQ(owner->checkpoint(source.context(),source.surfaceRouter2D().state()),bytes);EXPECT_EQ(fresh.close(),SWMM_OK);
        } else {
            ASSERT_NO_THROW(source.surfaceRouter2D().flushPendingBatch(source.context()));
            ASSERT_TRUE(source.saveCheckpoint(saved.string()).empty());ASSERT_TRUE(target.loadCheckpoint(saved.string()).empty());
            for(int k=0;k<5;++k) {
                ASSERT_EQ(source.step(&elapsed),SWMM_OK);ASSERT_EQ(target.step(&elapsed),SWMM_OK);
                ASSERT_TRUE(source.saveCheckpoint(saved.string()).empty());ASSERT_TRUE(target.saveCheckpoint(copy.string()).empty());
                EXPECT_EQ(read(copy),read(saved));
            }
        }
        EXPECT_EQ(source.close(),SWMM_OK);EXPECT_EQ(target.close(),SWMM_OK);
    }
}
TEST(ChangingVolumeHistoryPreflight, CallbacksAreExcludedBeforeAttachmentAndInvocation) {
    SWMMEngine e;ASSERT_NO_FATAL_FAILURE(start(e,"preflight-callback",model(true,false),false,false));
    int calls=0;auto callback=+[](void*,double,double,void* data){++*static_cast<int*>(data);};
    e.set_step_begin_callback(callback,&calls);
    EXPECT_FALSE(e.attachLiveChangingVolumeExchange(64,true).empty());EXPECT_EQ(e.livePassiveExchange(),nullptr);
    e.set_step_begin_callback(nullptr,nullptr);ASSERT_TRUE(e.attachLiveChangingVolumeExchange(64,true).empty());
    e.set_step_begin_callback(callback,&calls);double elapsed=12345;
    EXPECT_NE(e.step(&elapsed),SWMM_OK);EXPECT_EQ(calls,0);EXPECT_EQ(elapsed,12345);
    EXPECT_EQ(e.context().elapsed_ms,0);EXPECT_EQ(e.livePassiveExchange()->legCount(),0);
    EXPECT_EQ(e.close(),SWMM_OK);
}
