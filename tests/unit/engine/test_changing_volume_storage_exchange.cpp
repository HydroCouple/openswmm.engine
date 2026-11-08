// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include <gtest/gtest.h>
#include "core/SWMMEngine.hpp"
#include "core/FileIO.hpp"
#include "core/CheckpointInspection.hpp"
#include "quality/LivePassiveStorageExchange.hpp"
#include <openswmm/engine/openswmm_pollutants.h>
#include <openswmm/engine/openswmm_edit.h>
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
           bool preflight=false,std::size_t capacity=4096,
           quality::PassiveStorageWaterArithmetic water=quality::PassiveStorageWaterArithmetic::EXACT) {
    std::ofstream(evidence(tag+".inp"))<<input;
    ASSERT_EQ(e.open(io::path_utf8(evidence(tag+".inp")).c_str(),io::path_utf8(evidence(tag+".rpt")).c_str(),nullptr),SWMM_OK);
    ASSERT_EQ(e.initialize(),SWMM_OK);ASSERT_EQ(e.start(0),SWMM_OK);
    if(!attach)return;
    auto error=e.attachLiveChangingVolumeExchange(water,capacity,preflight);ASSERT_TRUE(error.empty())<<error;
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
    const bool water=e.livePassiveExchange()->receipts().retainsWaterAggregates();
    const bool slices=e.livePassiveExchange()->receipts().exactSlices();
    EXPECT_EQ(version,slices?(e.livePassiveExchange()->preflightsHistory()?13u:12u):
        e.livePassiveExchange()->preflightsHistory()?(water?11u:9u):(water?10u:8u));
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

namespace {
using WaterArithmetic=quality::PassiveStorageWaterArithmetic;
constexpr auto retainedWater=WaterArithmetic::RETAIN_AGGREGATES;
std::string repeatedWaterModel(bool positive,bool halves) {
    auto input=model(positive,false);
    const auto change=[&](const std::string& from,const std::string& to) {
        const auto at=input.find(from);if(at==std::string::npos)throw std::runtime_error("Missing repeated-water option.");
        input.replace(at,from.size(),to);
    };
    change("MAX_TIMESTEP 0.001953125","MAX_TIMESTEP 0.00048828125");
    if(!halves)change("ROUTING_STEP 0.0009765625","ROUTING_STEP 0.001953125");
    return input;
}
void repeatedWater(bool positive,bool halves) {
    const std::string tag=std::string("water-aggregate-")+(halves?"half-":"whole-")+(positive?"positive":"negative");
    const auto input=repeatedWaterModel(positive,halves);
    SWMMEngine source,ordinary;
    ASSERT_NO_FATAL_FAILURE(start(source,tag+"-source",input,true,true,false,4096,retainedWater));
    ASSERT_NO_FATAL_FAILURE(start(ordinary,tag+"-ordinary",input,false,false));
    std::vector<std::unique_ptr<SWMMEngine>> restores;
    std::ofstream states(evidence(tag+"-states.txt")),rows(evidence(tag+"-rows.txt")),legs(evidence(tag+"-legs.txt")),points(evidence(tag+"-points.txt"));
    states<<std::hexfloat;rows<<std::hexfloat;legs<<std::hexfloat;points<<std::hexfloat;
    int multipart=0;double elapsed=0;
    for(int step=0;step<=(halves?5:3);++step) {
        if(step) {
            ASSERT_EQ(source.step(&elapsed),SWMM_OK)<<source.context().error_message;
            ASSERT_EQ(ordinary.step(&elapsed),SWMM_OK)<<ordinary.context().error_message;
            for(auto& e:restores)ASSERT_EQ(e->step(&elapsed),SWMM_OK)<<e->context().error_message;
        }
        ASSERT_NO_FATAL_FAILURE(sameWater(source,ordinary));
        const auto& book=source.livePassiveExchange()->receipts();const auto& n=source.context().nodes;
        states<<step<<' '<<n.volume[0]<<' '<<n.old_volume[0]<<' '<<n.conc[0]<<' '<<n.conc_old[0];
        for(double part:book.storedRoundoff(0).parts())states<<' '<<part;states<<'\n';
        for(const auto& receipt:book.receipts()) {
            for(const auto& l:receipt.proof.legs)
                legs<<step<<' '<<receipt.id<<' '<<l.point<<' '<<l.drain<<' '<<l.water_native<<' '<<l.mass_native<<' '<<l.water_si<<'\n';
            for(const auto& point:receipt.proof.points)
                points<<step<<' '<<receipt.id<<' '<<point.point<<' '<<point.exchange_si<<' '<<receipt.proof.si_to_native<<'\n';
            for(bool remaining:{false,true})for(const auto& row:remaining?receipt.remaining:receipt.accepted)for(bool drain:{true,false}) {
                const auto& wp=drain?row.drain_water_parts:row.spill_water_parts;
                const auto& mp=drain?row.drain_mass_parts:row.spill_mass_parts;
                if(!wp.empty())++multipart;
                rows<<step<<' '<<receipt.id<<' '<<remaining<<' '<<row.point<<' '<<drain<<' '
                    <<(drain?row.drain_water:row.spill_water)<<' '<<(drain?row.drain_mass:row.spill_mass)<<" |";
                for(double v:wp)rows<<' '<<v;rows<<" |";for(double v:mp)rows<<' '<<v;rows<<'\n';
            }
        }
        if(step==(halves?2:1)) {
            ASSERT_EQ(source.livePassiveExchange()->legCount(),24u);
            ASSERT_EQ(book.receipts().size(),1u);
            for(const auto& row:book.receipts().front().accepted) {
                std::size_t firings=0;for(const auto& l:book.receipts().front().proof.legs)if(l.point==row.point)++firings;
                EXPECT_EQ(firings,4u);
            }
        }
        if(halves && step==3)EXPECT_EQ(book.receipts().front().half_state,1);
        if(step==(halves?4:2))for(const auto& row:book.receipts().front().remaining) {
            EXPECT_TRUE(row.waterAmount(true).zero());EXPECT_TRUE(row.waterAmount(false).zero());
            EXPECT_TRUE(row.materialAmount(true).zero());EXPECT_TRUE(row.materialAmount(false).zero());
        }
        const auto saved=evidence(tag+"-"+std::to_string(step)+".bin"),copy=evidence(tag+"-copy.bin");
        auto error=source.saveCheckpoint(saved.string());ASSERT_TRUE(error.empty())<<error;
        std::ofstream(evidence(tag+"-"+std::to_string(step)+".owner"),std::ios::binary)
            <<source.livePassiveExchange()->checkpoint(source.context(),source.surfaceRouter2D().state());
        for(auto& e:restores) {
            ASSERT_TRUE(e->saveCheckpoint(copy.string()).empty());EXPECT_EQ(read(copy),read(saved));
        }
        auto e=std::make_unique<SWMMEngine>();
        ASSERT_NO_FATAL_FAILURE(start(*e,tag+"-restored-"+std::to_string(step),input,true,true,false,4096,retainedWater));
        ASSERT_TRUE(e->saveCheckpoint(copy.string()).empty());const auto fresh=read(copy);
        CheckpointInspection inspect;error=e->inspectCheckpoint(saved.string(),inspect);ASSERT_TRUE(error.empty())<<error;
        ASSERT_TRUE(e->saveCheckpoint(copy.string()).empty());EXPECT_EQ(read(copy),fresh);
        error=e->loadCheckpoint(saved.string());ASSERT_TRUE(error.empty())<<error;
        ASSERT_TRUE(e->saveCheckpoint(copy.string()).empty());EXPECT_EQ(read(copy),read(saved));
        restores.push_back(std::move(e));
    }
    EXPECT_GT(multipart,0);for(auto& e:restores)EXPECT_EQ(e->close(),SWMM_OK);
    EXPECT_EQ(source.close(),SWMM_OK);EXPECT_EQ(ordinary.close(),SWMM_OK);
}
}
TEST(ChangingVolumeWaterAggregates, AuthoredWholeRepeatedWaterBothSignsRecover) { for(bool positive:{true,false})repeatedWater(positive,false); }
TEST(ChangingVolumeWaterAggregates, AuthoredHalfRepeatedWaterBothSignsRecover) { for(bool positive:{true,false})repeatedWater(positive,true); }
TEST(ChangingVolumeWaterAggregates, EarlierPolicyRetainsItsAppliedJournalAtNativeWaterRefusal) {
    for(bool positive:{true,false}) {
        const auto tag=std::string("water-strict-")+(positive?"positive":"negative");
        SWMMEngine strict,retained;const auto input=repeatedWaterModel(positive,false);
        ASSERT_NO_FATAL_FAILURE(start(strict,tag,input));
        ASSERT_NO_FATAL_FAILURE(start(retained,tag+"-retained",input,true,true,false,4096,retainedWater));
        double elapsed=0;EXPECT_NE(strict.step(&elapsed),SWMM_OK);EXPECT_TRUE(strict.livePassiveExchange()->failed());
        ASSERT_EQ(retained.step(&elapsed),SWMM_OK)<<retained.context().error_message;
        ASSERT_EQ(strict.livePassiveExchange()->legCount(),24u);ASSERT_EQ(retained.livePassiveExchange()->legCount(),24u);
        for(std::size_t k=0;k<24;++k)EXPECT_EQ(strict.livePassiveExchange()->leg(k),retained.livePassiveExchange()->leg(k));
        EXPECT_EQ(strict.surfaceRouter2D().state().volume,retained.surfaceRouter2D().state().volume);
        EXPECT_TRUE(strict.livePassiveExchange()->receipts().receipts().empty());
        EXPECT_EQ(strict.close(),SWMM_OK);EXPECT_EQ(retained.close(),SWMM_OK);
    }
}
TEST(ChangingVolumeWaterAggregates, PolicyIdentityRefusesBeforePublishingAndValidRetrySucceeds) {
    for(bool preflight:{false,true}) {
        const auto tag=std::string("water-policy-")+(preflight?"bounded":"ordinary");
        SWMMEngine earlier,current,target;const auto input=repeatedWaterModel(true,true);
        ASSERT_NO_FATAL_FAILURE(start(earlier,tag+"-earlier",input,true,true,preflight,64));
        ASSERT_NO_FATAL_FAILURE(start(current,tag+"-current",input,true,true,preflight,64,retainedWater));
        ASSERT_NO_FATAL_FAILURE(start(target,tag+"-target",input,true,true,preflight,64,retainedWater));
        const auto a=evidence(tag+"-current.bin"),b=evidence(tag+"-earlier.bin"),copy=evidence(tag+"-copy.bin");
        ASSERT_TRUE(current.saveCheckpoint(a.string()).empty());ASSERT_TRUE(earlier.saveCheckpoint(b.string()).empty());
        const auto ca=read(a),cb=read(b);EXPECT_NE(ca,cb);
        for(auto item:{std::make_pair(&current,b),std::make_pair(&earlier,a)}) {
            CheckpointInspection inspection;inspection.checkpoint_bytes=123;const auto before=inspection;
            EXPECT_FALSE(item.first->inspectCheckpoint(item.second.string(),inspection).empty());EXPECT_EQ(inspection,before);
            EXPECT_FALSE(item.first->loadCheckpoint(item.second.string()).empty());
        }
        ASSERT_TRUE(current.saveCheckpoint(copy.string()).empty());EXPECT_EQ(read(copy),ca);
        ASSERT_TRUE(earlier.saveCheckpoint(copy.string()).empty());EXPECT_EQ(read(copy),cb);
        ASSERT_TRUE(target.loadCheckpoint(a.string()).empty());ASSERT_TRUE(target.saveCheckpoint(copy.string()).empty());EXPECT_EQ(read(copy),ca);
        for(auto* e:{&earlier,&current,&target})EXPECT_EQ(e->close(),SWMM_OK);
    }
}
TEST(ChangingVolumeWaterAggregates, CanonicalWaterAndUnsupportedPolicyGuards) {
    using Row=quality::PassiveStorageExchange::Row;
    NativeAmountSum exact;exact.add(1);exact.add(std::ldexp(1.,-60));Row row;row.setWaterAmount(true,exact);
    EXPECT_EQ(row.drain_water,0);EXPECT_EQ(row.waterAmount(true),exact);EXPECT_THROW(row.waterAmount(true).value(),std::exception);
    for(auto parts:{std::vector<double>{0,1},std::vector<double>{1,1},std::vector<double>{-1,-std::ldexp(1.,-60)},std::vector<double>{1}}) {
        auto invalid=row;invalid.drain_water_parts=parts;EXPECT_THROW(invalid.waterAmount(true),std::exception);
    }
    auto invalid=row;invalid.drain_water=1;EXPECT_THROW(invalid.waterAmount(true),std::exception);
    row.setWaterAmount(true,NativeAmountSum{});EXPECT_TRUE(row.drain_water_parts.empty());EXPECT_TRUE(row.waterAmount(true).zero());
    SWMMEngine e;ASSERT_NO_FATAL_FAILURE(start(e,"water-policy-invalid",model(true,false),false,false));
    const auto& surface=e.surfaceRouter2D().state();const auto& points=*surface.node_coupling;
    EXPECT_THROW(quality::LivePassiveStorageExchange(e.context(),surface,points,1,64,
        quality::PassiveStorageArithmetic::RETAIN_EXACT_HALVES,false,quality::PassiveStorageLaw::FIXED_VOLUME,retainedWater),std::exception);
    EXPECT_FALSE(e.attachLiveChangingVolumeExchange(static_cast<WaterArithmetic>(99),64,false).empty());
    EXPECT_EQ(e.livePassiveExchange(),nullptr);EXPECT_EQ(e.close(),SWMM_OK);
}

namespace {
std::string encodeWaterReceipt(quality::PassiveStorageExchange::Receipt r) {
    Archive a;auto& p=r.publication;std::uint64_t n=r.accepted.size();
    a(r.id,p.phase,p.capture_ms,p.batch_start_ms,p.batch_end_ms,p.eligible_ms,p.remaining,p.generation,n);
    for(std::size_t k=0;k<n;++k) {
        for(auto* row:{&r.accepted[k],&r.remaining[k]})
            a(row->point,row->node,row->pollutant,row->units,row->drain_water,row->spill_water,row->drain_mass,row->spill_mass,
              row->drain_mass_parts,row->spill_mass_parts,row->drain_water_parts,row->spill_water_parts);
        auto& s=r.samples[k];a(s.volume,s.depth,s.concentration);
    }
    a(r.authenticated);auto bytes=a.bytes();if(r.authenticated)bytes+=encodeProof(r.proof);
    Archive tail;tail(r.half_state);return bytes+tail.bytes();
}
void waterHistoryPreflight(bool positive) {
    const auto tag=std::string("water-budget-")+(positive?"positive":"negative");const auto input=repeatedWaterModel(positive,true);
    SWMMEngine source,control;
    ASSERT_NO_FATAL_FAILURE(start(source,tag+"-source",input,true,true,true,64,retainedWater));
    ASSERT_NO_FATAL_FAILURE(start(control,tag+"-control",input,true,true,false,64,retainedWater));
    std::vector<std::unique_ptr<SWMMEngine>> restores;bool refused=false;int accepted=0;double elapsed=0;
    const auto copy=evidence(tag+"-copy.bin");
    for(int k=0;k<12;++k) {
        const auto saved=evidence(tag+"-"+std::to_string(k)+".bin");
        ASSERT_TRUE(source.saveCheckpoint(saved.string()).empty());const auto before=read(saved);
        std::ofstream(evidence(tag+"-"+std::to_string(k)+".owner"),std::ios::binary)
            <<source.livePassiveExchange()->checkpoint(source.context(),source.surfaceRouter2D().state());
        elapsed=12345;
        if(source.step(&elapsed)!=SWMM_OK) {
            refused=true;EXPECT_EQ(elapsed,12345);EXPECT_FALSE(source.livePassiveExchange()->failed());
            EXPECT_EQ(source.context().state,EngineState::RUNNING);EXPECT_EQ(source.context().error_code,SWMM_OK);
            EXPECT_NE(source.livePassiveExchange()->historyPreflightError().find("work exceeds"),std::string::npos);
            ASSERT_TRUE(source.saveCheckpoint(copy.string()).empty());EXPECT_EQ(read(copy),before);
            EXPECT_EQ(livePayload(source),livePayload(control));ASSERT_NO_FATAL_FAILURE(sameWater(source,control));
            auto e=std::make_unique<SWMMEngine>();
            ASSERT_NO_FATAL_FAILURE(start(*e,tag+"-last-restored",input,true,true,true,64,retainedWater));
            ASSERT_TRUE(e->loadCheckpoint(saved.string()).empty());restores.push_back(std::move(e));
            for(auto& target:restores)for(int retry=0;retry<2;++retry) {
                elapsed=12345;EXPECT_NE(target->step(&elapsed),SWMM_OK);EXPECT_EQ(elapsed,12345);
                EXPECT_EQ(target->livePassiveExchange()->historyPreflightError(),source.livePassiveExchange()->historyPreflightError());
                ASSERT_TRUE(target->saveCheckpoint(copy.string()).empty());EXPECT_EQ(read(copy),before);
                EXPECT_FALSE(target->livePassiveExchange()->failed());
            }
            std::ofstream(evidence(tag+"-summary.json"))<<"{\"accepted_steps\":"<<accepted<<",\"restored_owners\":"<<restores.size()<<",\"capacity\":64}\n";
            for(auto* engine:{&source,restores.back().get()})ASSERT_EQ(engine->end(),SWMM_OK)<<engine->context().error_message;
            EXPECT_EQ(livePayload(source),livePayload(*restores.back()));
            std::ofstream(evidence(tag+"-final.owner"),std::ios::binary)
                <<source.livePassiveExchange()->checkpoint(source.context(),source.surfaceRouter2D().state());
            break;
        }
        ++accepted;ASSERT_EQ(control.step(&elapsed),SWMM_OK)<<control.context().error_message;
        EXPECT_EQ(livePayload(source),livePayload(control));ASSERT_NO_FATAL_FAILURE(sameWater(source,control));
        for(auto& e:restores) {ASSERT_EQ(e->step(&elapsed),SWMM_OK);EXPECT_EQ(livePayload(source),livePayload(*e));}
        ASSERT_TRUE(source.saveCheckpoint(copy.string()).empty());const auto after=read(copy);
        auto e=std::make_unique<SWMMEngine>();
        ASSERT_NO_FATAL_FAILURE(start(*e,tag+"-restored-"+std::to_string(k),input,true,true,true,64,retainedWater));
        ASSERT_TRUE(e->loadCheckpoint(copy.string()).empty());ASSERT_TRUE(e->saveCheckpoint(copy.string()).empty());EXPECT_EQ(read(copy),after);
        restores.push_back(std::move(e));
    }
    EXPECT_TRUE(refused);EXPECT_GE(accepted,3);
    for(auto& e:restores)EXPECT_EQ(e->close(),SWMM_OK);EXPECT_EQ(source.close(),SWMM_OK);EXPECT_EQ(control.close(),SWMM_OK);
}
}
TEST(ChangingVolumeWaterAggregates, ProjectedWaterComponentsRefuseBeforeMutationBothSigns) {
    for(bool positive:{true,false})waterHistoryPreflight(positive);
}
TEST(ChangingVolumeWaterAggregates, CorruptedWaterProofAndHalfRemainderRefuseAtomicallyBothSigns) {
    for(bool positive:{true,false}) {
        const auto tag=std::string("water-corrupt-")+(positive?"positive":"negative");const auto input=repeatedWaterModel(positive,true);
        SWMMEngine source,target;
        ASSERT_NO_FATAL_FAILURE(start(source,tag+"-source",input,true,true,false,4096,retainedWater));
        ASSERT_NO_FATAL_FAILURE(start(target,tag+"-target",input,true,true,false,4096,retainedWater));
        double elapsed=0;for(int k=0;k<3;++k)ASSERT_EQ(source.step(&elapsed),SWMM_OK)<<source.context().error_message;
        const auto& receipts=source.livePassiveExchange()->receipts().receipts();ASSERT_EQ(receipts.size(),1u);
        const auto receipt=receipts.front();ASSERT_EQ(receipt.half_state,1);
        std::size_t index=0;bool drain=true;
        for(;index<receipt.accepted.size();++index) {
            if(!receipt.accepted[index].drain_water_parts.empty())break;
            if(!receipt.accepted[index].spill_water_parts.empty()) {drain=false;break;}
        }
        ASSERT_LT(index,receipt.accepted.size());
        const auto good=evidence(tag+"-good.bin"),copy=evidence(tag+"-copy.bin");
        ASSERT_TRUE(source.saveCheckpoint(good.string()).empty());const auto valid=read(good);
        ASSERT_TRUE(target.saveCheckpoint(copy.string()).empty());const auto pristine=read(copy);
        std::vector<quality::PassiveStorageExchange::Receipt> proposals;
        for(int edit=0;edit<8;++edit) {
            auto bad=receipt;auto& row=bad.accepted[index];auto& parts=drain?row.drain_water_parts:row.spill_water_parts;
            if(edit==0)parts[0]=0;
            if(edit==1)(drain?row.drain_water:row.spill_water)=1;
            if(edit==2)parts={1,1};
            if(edit==3)parts={-1,-std::ldexp(1.,-60)};
            if(edit==4)parts[0]=std::numeric_limits<double>::quiet_NaN();
            if(edit==5)parts.assign(65,1);
            if(edit==6)bad.half_state=0;
            if(edit==7)bad.half_state=2;
            proposals.push_back(std::move(bad));
        }
        for(bool remaining:{false,true}) {
            auto bad=receipt;auto& rows=remaining?bad.remaining:bad.accepted;
            std::size_t first=rows.size(),second=rows.size();
            for(std::size_t k=0;k<rows.size();++k)if(!rows[k].waterAmount(true).zero()) {
                if(first==rows.size())first=k;else {second=k;break;}
            }
            ASSERT_LT(second,rows.size());auto a=rows[first].waterAmount(true),b=rows[second].waterAmount(true);
            const auto original_a=a,original_b=b;const double shift=std::ldexp(1.,-60);a.add(shift);b.add(-shift);
            auto difference=original_a;difference.add(original_b);
            for(double part:a.parts())difference.add(-part);
            for(double part:b.parts())difference.add(-part);
            EXPECT_TRUE(difference.zero());
            rows[first].setWaterAmount(true,a);rows[second].setWaterAmount(true,b);proposals.push_back(std::move(bad));
        }
        for(std::size_t k=0;k<proposals.size();++k) {
            const auto invalid=editReceiving(valid,[&](std::string& p){replaceUnique(p,encodeWaterReceipt(receipt),encodeWaterReceipt(proposals[k]));});
            const auto bad=evidence(tag+"-"+std::to_string(k)+".bin");std::ofstream(bad,std::ios::binary)<<invalid;
            CheckpointInspection inspection;inspection.engine_seconds=123;const auto before=inspection;
            auto error=target.inspectCheckpoint(bad.string(),inspection);EXPECT_FALSE(error.empty())<<k;EXPECT_EQ(inspection,before);
            EXPECT_EQ(error.find("checksum"),std::string::npos)<<error;
            EXPECT_FALSE(target.loadCheckpoint(bad.string()).empty())<<k;
            ASSERT_TRUE(target.saveCheckpoint(copy.string()).empty());EXPECT_EQ(read(copy),pristine);
        }
        auto error=target.loadCheckpoint(good.string());ASSERT_TRUE(error.empty())<<error;
        ASSERT_EQ(source.step(&elapsed),SWMM_OK);ASSERT_EQ(target.step(&elapsed),SWMM_OK);
        ASSERT_TRUE(source.saveCheckpoint(good.string()).empty());ASSERT_TRUE(target.saveCheckpoint(copy.string()).empty());EXPECT_EQ(read(copy),read(good));
        EXPECT_EQ(source.close(),SWMM_OK);EXPECT_EQ(target.close(),SWMM_OK);
    }
}
TEST(ChangingVolumeWaterAggregates, WaterOnlyMultipartRowsRemainPendingAndRecover) {
    auto input=repeatedWaterModel(true,false);
    for(auto [from,to]:{std::pair{"NO * 0 0 1","NO * 0 0 0"},std::pair{"* Dye 2","* Dye 0"}}) {
        const auto at=input.find(from);ASSERT_NE(at,std::string::npos);input.replace(at,std::strlen(from),to);
    }
    SWMMEngine source,target;
    ASSERT_NO_FATAL_FAILURE(start(source,"water-only-source",input,true,true,false,4096,retainedWater));
    ASSERT_NO_FATAL_FAILURE(start(target,"water-only-target",input,true,true,false,4096,retainedWater));
    double elapsed=0;ASSERT_EQ(source.step(&elapsed),SWMM_OK)<<source.context().error_message;
    const auto& receipt=source.livePassiveExchange()->receipts().receipts().front();bool multipart=false;
    for(const auto& row:receipt.remaining) {
        EXPECT_TRUE(row.materialAmount(true).zero());EXPECT_TRUE(row.materialAmount(false).zero());
        EXPECT_FALSE(row.waterAmount(true).zero() && row.waterAmount(false).zero());
        multipart|=!row.drain_water_parts.empty() || !row.spill_water_parts.empty();
    }
    EXPECT_TRUE(multipart);
    const auto saved=evidence("water-only.bin"),copy=evidence("water-only-copy.bin");
    ASSERT_TRUE(source.saveCheckpoint(saved.string()).empty());ASSERT_TRUE(target.loadCheckpoint(saved.string()).empty());
    ASSERT_EQ(source.step(&elapsed),SWMM_OK);ASSERT_EQ(target.step(&elapsed),SWMM_OK);
    ASSERT_TRUE(source.saveCheckpoint(saved.string()).empty());ASSERT_TRUE(target.saveCheckpoint(copy.string()).empty());EXPECT_EQ(read(copy),read(saved));
    EXPECT_EQ(source.context().nodes.conc[0],0);EXPECT_EQ(source.close(),SWMM_OK);EXPECT_EQ(target.close(),SWMM_OK);
}

TEST(ChangingVolumeWaterAggregates, ReportAndFinalFlushRecoverWithMultipartHistory) {
    for(bool positive:{true,false})for(bool final:{true,false})for(bool preflight:{false,true}) {
        const auto tag=std::string("water-flush-")+(final?"final-":"report-")+
            (positive?"positive-":"negative-")+(preflight?"bounded":"ordinary");
        const auto input=repeatedWaterModel(positive,true);SWMMEngine source,target;
        ASSERT_NO_FATAL_FAILURE(start(source,tag+"-source",input,true,true,preflight,48,retainedWater));
        ASSERT_NO_FATAL_FAILURE(start(target,tag+"-target",input,true,true,preflight,48,retainedWater));
        double elapsed=0;for(int k=0;k<3;++k)ASSERT_EQ(source.step(&elapsed),SWMM_OK)<<source.context().error_message;
        bool multipart=false;
        for(const auto& r:source.livePassiveExchange()->receipts().receipts())for(const auto& row:r.remaining)
            multipart|=!row.drain_water_parts.empty() || !row.spill_water_parts.empty();
        ASSERT_TRUE(multipart);
        const auto saved=evidence(tag+".bin"),copy=evidence(tag+"-copy.bin");
        if(final) {
            ASSERT_TRUE(source.saveCheckpoint(saved.string()).empty());ASSERT_TRUE(target.loadCheckpoint(saved.string()).empty());
            for(auto* e:{&source,&target})ASSERT_EQ(e->end(),SWMM_OK)<<e->context().error_message;
            EXPECT_EQ(livePayload(source),livePayload(target));
            const auto bytes=source.livePassiveExchange()->checkpoint(source.context(),source.surfaceRouter2D().state());
            SWMMEngine fresh;ASSERT_NO_FATAL_FAILURE(start(fresh,tag+"-fresh",input,true,true,preflight,48,retainedWater));
            auto owner=fresh.livePassiveExchange()->prepareRestore(source.context(),source.surfaceRouter2D().state(),bytes);
            EXPECT_EQ(owner->checkpoint(source.context(),source.surfaceRouter2D().state()),bytes);EXPECT_EQ(fresh.close(),SWMM_OK);
        } else {
            ASSERT_NO_THROW(source.surfaceRouter2D().flushPendingBatch(source.context()));
            ASSERT_TRUE(source.saveCheckpoint(saved.string()).empty());ASSERT_TRUE(target.loadCheckpoint(saved.string()).empty());
            for(auto* e:{&source,&target})ASSERT_EQ(e->step(&elapsed),SWMM_OK)<<e->context().error_message;
            ASSERT_TRUE(source.saveCheckpoint(saved.string()).empty());ASSERT_TRUE(target.saveCheckpoint(copy.string()).empty());
            EXPECT_EQ(read(copy),read(saved));
        }
        EXPECT_EQ(source.close(),SWMM_OK);EXPECT_EQ(target.close(),SWMM_OK);
    }
}

namespace {
std::string slicedModel(bool positive,double drain_width,double spill_width) {
    auto input=repeatedWaterModel(positive,false);
    auto at=input.find("ROUTING_STEP 0.001953125");input.replace(at,strlen("ROUTING_STEP 0.001953125"),"ROUTING_STEP 0.00048828125");
    // Two area-3 triangles retain the original 2 m2 mapped pond footprint.
    for(auto [from,to]:{std::pair{"2 0 -1.5\n","6 0 -1.5\n"},std::pair{"2 -1 -1.5\n","6 -1 -1.5\n"}}) {
        const auto at=input.find(from);input.replace(at,strlen(from),to);
    }
    auto a=input.find("[2D_TRIANGLES]"),b=input.find("[2D_VERTEX_NODE_MAP]");
    std::istringstream triangles(input.substr(a,b-a));std::string line,kept;
    for(int k=0;k<3 && std::getline(triangles,line);++k)kept+=line+"\n";
    input.replace(a,b-a,kept);a=input.find("[2D_VERTEX_NODE_MAP]");b=input.find("[POLLUTANTS]");
    std::ostringstream points;points<<std::setprecision(17)<<"[2D_VERTEX_NODE_MAP]\n0 Tank .7 "<<drain_width<<"\n3 Tank .7 "<<spill_width<<"\n";
    input.replace(a,b-a,points.str());return input;
}
void startSliced(SWMMEngine& e,const std::string& tag,const std::string& input,bool preflight=false,std::size_t capacity=64) {
    ASSERT_NO_FATAL_FAILURE(start(e,tag,input,false,false));
    const auto& n=e.context().nodes;
    const double expected=n.full_volume[0]+(n.depth[0]-n.full_depth[0])*n.ponded_area[0];
    ASSERT_NEAR(n.volume[0],expected,2*std::numeric_limits<double>::epsilon()*std::abs(expected));
    auto error=e.attachLiveChangingVolumeSlicedExchange(capacity,preflight);ASSERT_TRUE(error.empty())<<error;
    error=e.enableCheckpointing();ASSERT_TRUE(error.empty())<<error;
}
}
namespace {
std::pair<double,double> sliceWidths(bool positive) { return positive?std::pair{0.1754150390625,0.1754150390625}:std::pair{0.07489013671875,0.07489013671875}; }
std::string authoredSlices(bool positive) {auto [a,b]=sliceWidths(positive);return slicedModel(positive,a,b);}
std::string basisModel(bool positive,int basis) {
    auto input=authoredSlices(positive);const auto at=input.find("Dye MG/L");
    input.replace(at,strlen("Dye MG/L"),basis==0?"Dye MG/L":basis==1?"Dye UG/L":"Dye #/L");return input;
}
void slicedRun(bool positive,int basis=-1) {
    const auto tag=(basis<0?std::string("slices-"):std::string("basis-")+(basis==0?"mg-":basis==1?"ug-":"counts-"))+(positive?"positive":"negative");
    const auto input=basis<0?authoredSlices(positive):basisModel(positive,basis);
    SWMMEngine source,ordinary;
        ASSERT_NO_FATAL_FAILURE(startSliced(source,tag,input));
    ASSERT_NO_FATAL_FAILURE(start(ordinary,tag+"-ordinary",input,false,false));
    std::vector<std::unique_ptr<SWMMEngine>> restores;
    std::ofstream states(evidence(tag+"-states.txt")),legs(evidence(tag+"-legs.txt")),rows(evidence(tag+"-rows.txt")),points(evidence(tag+"-points.txt"));
    states<<std::hexfloat;legs<<std::hexfloat;rows<<std::hexfloat;points<<std::hexfloat;
    double elapsed=0,span=0,dt=0;
    for(int step=0;step<=8;++step) {
        if(step) {
            span=source.context().coupling_delivery_remaining;const auto before=source.context().elapsed_ms;
            ASSERT_EQ(source.step(&elapsed),SWMM_OK)<<source.context().error_message;
            dt=(source.context().elapsed_ms-before)/1000;
            ASSERT_EQ(ordinary.step(&elapsed),SWMM_OK);
            for(auto& e:restores)ASSERT_EQ(e->step(&elapsed),SWMM_OK)<<e->context().error_message;
        }
        ASSERT_NO_FATAL_FAILURE(sameWater(source,ordinary));
        const auto& book=source.livePassiveExchange()->receipts();const auto& n=source.context().nodes;
        EXPECT_EQ(static_cast<int>(source.context().pollutants.units[0]),basis<0?0:basis);
        states<<step<<' '<<n.volume[0]<<' '<<n.old_volume[0]<<' '<<n.conc[0]<<' '<<n.conc_old[0]<<' '<<span<<' '<<dt;
        for(double v:book.storedRoundoff(0).parts())states<<' '<<v;states<<'\n';
        for(const auto& r:book.receipts()) {
            EXPECT_EQ(r.half_state,0);ASSERT_EQ(r.remaining_water_legs.size(),r.proof.legs.size());
            for(const auto* column:{&r.accepted,&r.remaining})for(const auto& row:*column)EXPECT_EQ(static_cast<int>(row.units),basis<0?0:basis);
            for(std::size_t k=0;k<r.proof.legs.size();++k) {const auto& l=r.proof.legs[k];
                legs<<step<<' '<<r.id<<' '<<k<<' '<<l.point<<' '<<l.drain<<' '<<l.water_native<<' '<<l.mass_native<<' '
                    <<r.remaining_water_legs[k]<<' '<<r.remaining_mass_legs[k]<<' '<<l.water_si<<'\n';
            }
            for(const auto& p:r.proof.points)points<<step<<' '<<r.id<<' '<<p.point<<' '<<p.exchange_si<<' '<<r.proof.si_to_native<<'\n';
            for(bool remaining:{false,true})for(const auto& row:remaining?r.remaining:r.accepted)for(bool drain:{true,false}) {
                rows<<step<<' '<<r.id<<' '<<remaining<<' '<<row.point<<' '<<drain<<' '
                    <<(drain?row.drain_water:row.spill_water)<<' '<<(drain?row.drain_mass:row.spill_mass)<<" |";
                for(double v:drain?row.drain_water_parts:row.spill_water_parts)rows<<' '<<v;
                rows<<" |";for(double v:drain?row.drain_mass_parts:row.spill_mass_parts)rows<<' '<<v;rows<<'\n';
            }
        }
        if(step==4) {ASSERT_EQ(book.receipts().size(),1u);EXPECT_EQ(book.receipts()[0].proof.legs.size(),8u);
            EXPECT_FALSE(book.pendingDrainAmount(0).zero());EXPECT_FALSE(book.pendingSpillAmount(0).zero());}
        if(step==8)for(double v:book.receipts()[0].remaining_water_legs)EXPECT_EQ(v,0);
        const auto saved=evidence(tag+"-"+std::to_string(step)+".bin"),copy=evidence(tag+"-copy.bin");
        auto error=source.saveCheckpoint(saved.string());ASSERT_TRUE(error.empty())<<error;
        std::ofstream(evidence(tag+"-"+std::to_string(step)+".owner"),std::ios::binary)<<source.livePassiveExchange()->checkpoint(source.context(),source.surfaceRouter2D().state());
        for(auto& e:restores) {ASSERT_TRUE(e->saveCheckpoint(copy.string()).empty());EXPECT_EQ(read(copy),read(saved));}
        auto e=std::make_unique<SWMMEngine>();
        ASSERT_NO_FATAL_FAILURE(startSliced(*e,tag+"-restored-"+std::to_string(step),input));
        ASSERT_TRUE(e->saveCheckpoint(copy.string()).empty());const auto pristine=read(copy);
        CheckpointInspection inspection;error=e->inspectCheckpoint(saved.string(),inspection);ASSERT_TRUE(error.empty())<<error;
        ASSERT_TRUE(e->saveCheckpoint(copy.string()).empty());EXPECT_EQ(read(copy),pristine);
        error=e->loadCheckpoint(saved.string());ASSERT_TRUE(error.empty())<<error;
        ASSERT_TRUE(e->saveCheckpoint(copy.string()).empty());EXPECT_EQ(read(copy),read(saved));restores.push_back(std::move(e));
    }
    for(auto& e:restores)EXPECT_EQ(e->close(),SWMM_OK);EXPECT_EQ(source.close(),SWMM_OK);EXPECT_EQ(ordinary.close(),SWMM_OK);
}
std::string encodeSlicedReceipt(const quality::PassiveStorageExchange::Receipt& r) {
    Archive tail;auto water=r.remaining_water_legs,mass=r.remaining_mass_legs;tail(water,mass);return encodeWaterReceipt(r)+tail.bytes();
}
}
TEST(ChangingVolumeExactSlices, AuthoredPositiveSuccessivePartialsRecover) {slicedRun(true);}
TEST(ChangingVolumeExactSlices, AuthoredNegativeSuccessivePartialsRecover) {slicedRun(false);}
TEST(ChangingVolumeExactSlices, UnsupportedPartitionRetainsFundedLegsBeforeMixing) {
    for(bool positive:{true,false}) {
        SWMMEngine e;
        ASSERT_NO_FATAL_FAILURE(startSliced(e,"slice-refusal",slicedModel(positive,.01,.01)));
        double elapsed=0;for(int k=0;k<4;++k)ASSERT_EQ(e.step(&elapsed),SWMM_OK);
        const auto before=e.livePassiveExchange()->receipts().receipts();const auto concentration=e.context().nodes.conc;
        EXPECT_NE(e.step(&elapsed),SWMM_OK);EXPECT_TRUE(e.livePassiveExchange()->failed());
        EXPECT_EQ(e.livePassiveExchange()->receipts().receipts(),before);EXPECT_EQ(e.context().nodes.conc,concentration);
        EXPECT_NE(e.context().error_message.find("partition"),std::string::npos)<<e.context().error_message;EXPECT_EQ(e.close(),SWMM_OK);
    }
}
TEST(ChangingVolumeExactSlices, CorruptedLegRemaindersRefuseAtomicallyBothSigns) {
    for(bool positive:{true,false}) {
        const auto tag=std::string("slice-corrupt-")+(positive?"positive":"negative");const auto input=authoredSlices(positive);
        SWMMEngine source,target;
        ASSERT_NO_FATAL_FAILURE(startSliced(source,tag+"-source",input));
        ASSERT_NO_FATAL_FAILURE(startSliced(target,tag+"-target",input));
        double elapsed=0;for(int k=0;k<6;++k)ASSERT_EQ(source.step(&elapsed),SWMM_OK)<<source.context().error_message;
        const auto receipt=source.livePassiveExchange()->receipts().receipts().front();
        const auto good=evidence(tag+".bin"),copy=evidence(tag+"-copy.bin");ASSERT_TRUE(source.saveCheckpoint(good.string()).empty());const auto valid=read(good);
        ASSERT_TRUE(target.saveCheckpoint(copy.string()).empty());const auto pristine=read(copy);
        for(int edit=0;edit<8;++edit) {
            auto bad=receipt;
            if(edit==0)bad.remaining_water_legs.pop_back();
            if(edit==1)bad.remaining_mass_legs.push_back(0);
            if(edit==2)bad.remaining_water_legs[0]=-1;
            if(edit==3)bad.remaining_mass_legs[0]=std::numeric_limits<double>::quiet_NaN();
            if(edit==4)bad.remaining_water_legs[0]=bad.proof.legs[0].water_native;
            if(edit==5)bad.half_state=1;
            if(edit>=6) {
                auto& column=edit==6?bad.remaining_water_legs:bad.remaining_mass_legs;
                std::size_t second=1;
                while(second<bad.proof.legs.size() && bad.proof.legs[second].point!=bad.proof.legs[0].point)++second;
                ASSERT_LT(second,bad.proof.legs.size());const auto a=column[0],b=column[second];
                const double shift=4*std::max(std::nextafter(a,std::numeric_limits<double>::infinity())-a,
                                              std::nextafter(b,std::numeric_limits<double>::infinity())-b);
                column[0]+=shift;column[second]-=shift;ASSERT_NE(column[0],a);ASSERT_NE(column[second],b);
                NativeAmountSum difference;difference.add(a);difference.add(b);difference.add(-column[0]);difference.add(-column[second]);ASSERT_TRUE(difference.zero());
            }
            const auto invalid=editReceiving(valid,[&](std::string& p){replaceUnique(p,encodeSlicedReceipt(receipt),encodeSlicedReceipt(bad));});
            ASSERT_NE(invalid,valid);
            const auto path=evidence(tag+"-bad-"+std::to_string(edit)+".bin");std::ofstream(path,std::ios::binary)<<invalid;
            CheckpointInspection inspection;inspection.engine_seconds=123;const auto before=inspection;
            auto error=target.inspectCheckpoint(path.string(),inspection);EXPECT_FALSE(error.empty())<<edit;EXPECT_EQ(inspection,before);EXPECT_EQ(error.find("checksum"),std::string::npos)<<error;
            EXPECT_FALSE(target.loadCheckpoint(path.string()).empty())<<edit;ASSERT_TRUE(target.saveCheckpoint(copy.string()).empty());EXPECT_EQ(read(copy),pristine);
        }
        ASSERT_TRUE(target.loadCheckpoint(good.string()).empty());ASSERT_EQ(source.step(&elapsed),SWMM_OK);ASSERT_EQ(target.step(&elapsed),SWMM_OK);
        ASSERT_TRUE(source.saveCheckpoint(good.string()).empty());ASSERT_TRUE(target.saveCheckpoint(copy.string()).empty());EXPECT_EQ(read(copy),read(good));
        EXPECT_EQ(source.close(),SWMM_OK);EXPECT_EQ(target.close(),SWMM_OK);
    }
}
TEST(ChangingVolumeExactSlices, PolicyIsolationAndReportFinalRecovery) {
    {
        SWMMEngine e;ASSERT_NO_FATAL_FAILURE(start(e,"slice-invalid-policy",authoredSlices(true),false,false));
        const auto& state=e.surfaceRouter2D().state();const auto& points=*state.node_coupling;
        EXPECT_THROW(quality::LivePassiveStorageExchange(e.context(),state,points,1,32,
            quality::PassiveStorageArithmetic::RETAIN_EXACT_SLICES,false,quality::PassiveStorageLaw::FIXED_VOLUME,retainedWater),std::exception);
        EXPECT_THROW(quality::LivePassiveStorageExchange(e.context(),state,points,1,32,
            quality::PassiveStorageArithmetic::RETAIN_EXACT_SLICES,false,quality::PassiveStorageLaw::CHANGING_VOLUME,WaterArithmetic::EXACT),std::exception);
        EXPECT_EQ(e.close(),SWMM_OK);
    }
    for(bool positive:{true,false})for(bool preflight:{false,true})for(bool final:{false,true}) {
        const auto tag=std::string("slice-flush-")+(positive?"positive-":"negative-")+(preflight?"bounded-":"ordinary-")+(final?"final":"report");
        const auto input=authoredSlices(positive);SWMMEngine source,target,earlier;
        ASSERT_NO_FATAL_FAILURE(startSliced(source,tag+"-source",input,preflight,32));
        ASSERT_NO_FATAL_FAILURE(startSliced(target,tag+"-target",input,preflight,32));
        ASSERT_NO_FATAL_FAILURE(start(earlier,tag+"-earlier",input,true,true,preflight,32,retainedWater));
        const auto saved=evidence(tag+".bin"),copy=evidence(tag+"-copy.bin");ASSERT_TRUE(source.saveCheckpoint(saved.string()).empty());
        ASSERT_TRUE(earlier.saveCheckpoint(copy.string()).empty());const auto pristine=read(copy);
        EXPECT_FALSE(earlier.loadCheckpoint(saved.string()).empty());ASSERT_TRUE(earlier.saveCheckpoint(copy.string()).empty());EXPECT_EQ(read(copy),pristine);
        EXPECT_FALSE(source.loadCheckpoint(copy.string()).empty());
        double elapsed=0;for(int k=0;k<(final?5:1);++k)ASSERT_EQ(source.step(&elapsed),SWMM_OK)<<source.context().error_message;
        if(!final)ASSERT_NO_THROW(source.surfaceRouter2D().flushPendingBatch(source.context()));
        ASSERT_TRUE(source.saveCheckpoint(saved.string()).empty());ASSERT_TRUE(target.loadCheckpoint(saved.string()).empty());
        if(final) {
            ASSERT_EQ(source.end(),SWMM_OK);ASSERT_EQ(target.end(),SWMM_OK);EXPECT_EQ(livePayload(source),livePayload(target));
            const auto bytes=source.livePassiveExchange()->checkpoint(source.context(),source.surfaceRouter2D().state());SWMMEngine fresh;
            ASSERT_NO_FATAL_FAILURE(startSliced(fresh,tag+"-fresh",input,preflight,32));
            auto owner=fresh.livePassiveExchange()->prepareRestore(source.context(),source.surfaceRouter2D().state(),bytes);
            EXPECT_EQ(owner->checkpoint(source.context(),source.surfaceRouter2D().state()),bytes);EXPECT_EQ(fresh.close(),SWMM_OK);
        } else {
            ASSERT_EQ(source.step(&elapsed),SWMM_OK)<<source.context().error_message;ASSERT_EQ(target.step(&elapsed),SWMM_OK)<<target.context().error_message;
            ASSERT_TRUE(source.saveCheckpoint(saved.string()).empty());ASSERT_TRUE(target.saveCheckpoint(copy.string()).empty());EXPECT_EQ(read(copy),read(saved));
        }
        EXPECT_EQ(source.close(),SWMM_OK);EXPECT_EQ(target.close(),SWMM_OK);EXPECT_EQ(earlier.close(),SWMM_OK);
    }
}
TEST(ChangingVolumeExactSlices, HistoryProjectionRefusesBeforeMutationBothSigns) {
    for(bool positive:{true,false}) {
        const auto tag=std::string("slice-budget-")+(positive?"positive":"negative");const auto input=authoredSlices(positive);SWMMEngine source;
        ASSERT_NO_FATAL_FAILURE(startSliced(source,tag,input,true,32));double elapsed=0;bool refused=false;int accepted=0;
        const auto copy=evidence(tag+"-copy.bin");
        for(int k=0;k<12;++k) {
            const auto saved=evidence(tag+"-"+std::to_string(k)+".bin");ASSERT_TRUE(source.saveCheckpoint(saved.string()).empty());const auto before=read(saved);
            std::ofstream(evidence(tag+"-"+std::to_string(k)+".owner"),std::ios::binary)<<source.livePassiveExchange()->checkpoint(source.context(),source.surfaceRouter2D().state());
            elapsed=12345;
            if(source.step(&elapsed)!=SWMM_OK) {
                ASSERT_FALSE(source.livePassiveExchange()->failed())<<source.context().error_message;EXPECT_EQ(elapsed,12345);refused=true;
                EXPECT_NE(source.livePassiveExchange()->historyPreflightError().find("work exceeds"),std::string::npos);
                ASSERT_TRUE(source.saveCheckpoint(copy.string()).empty());EXPECT_EQ(read(copy),before);
                SWMMEngine target;
        ASSERT_NO_FATAL_FAILURE(startSliced(target,tag+"-restored",input,true,32));ASSERT_TRUE(target.loadCheckpoint(saved.string()).empty());
                for(int retry=0;retry<2;++retry) {elapsed=12345;EXPECT_NE(target.step(&elapsed),SWMM_OK);EXPECT_EQ(elapsed,12345);ASSERT_TRUE(target.saveCheckpoint(copy.string()).empty());EXPECT_EQ(read(copy),before);}
                EXPECT_EQ(target.close(),SWMM_OK);break;
            }
            ++accepted;
        }
        EXPECT_TRUE(refused);EXPECT_GE(accepted,5);std::ofstream(evidence(tag+"-summary.json"))<<"{\"accepted_steps\":"<<accepted<<",\"capacity\":32}\n";EXPECT_EQ(source.close(),SWMM_OK);
    }
}

TEST(ChangingVolumeNativeBases, AuthoredAllBasesBothSignsRecover) {
    for(int basis=0;basis<3;++basis)for(bool positive:{true,false}) {SCOPED_TRACE(basis);
        SCOPED_TRACE(positive);slicedRun(positive,basis);}
}
TEST(ChangingVolumeNativeBases, CrossBasisArchivesRefuseAtomicallyAndValidRetrySucceeds) {
    for(bool positive:{true,false})for(int source_basis=0;source_basis<3;++source_basis)for(int step:{0,4,6,8}) {
        const auto tag=std::string("basis-mismatch-")+(positive?"positive-":"negative-")+std::to_string(source_basis)+"-"+std::to_string(step);
        SWMMEngine source;ASSERT_NO_FATAL_FAILURE(startSliced(source,tag+"-source",basisModel(positive,source_basis)));
        double elapsed=0;for(int k=0;k<step;++k)ASSERT_EQ(source.step(&elapsed),SWMM_OK);
        const auto saved=evidence(tag+".bin"),copy=evidence(tag+"-copy.bin");ASSERT_TRUE(source.saveCheckpoint(saved.string()).empty());
        for(int target_basis=0;target_basis<3;++target_basis)if(target_basis!=source_basis) {
            SWMMEngine target,valid;
            ASSERT_NO_FATAL_FAILURE(startSliced(target,tag+"-target-"+std::to_string(target_basis),basisModel(positive,target_basis)));
            ASSERT_NO_FATAL_FAILURE(startSliced(valid,tag+"-valid-"+std::to_string(target_basis),basisModel(positive,target_basis)));
            ASSERT_TRUE(target.saveCheckpoint(copy.string()).empty());const auto pristine=read(copy);
            CheckpointInspection inspection;inspection.engine_seconds=123;const auto before=inspection;
            auto error=target.inspectCheckpoint(saved.string(),inspection);EXPECT_FALSE(error.empty());EXPECT_EQ(inspection,before);
            EXPECT_FALSE(target.loadCheckpoint(saved.string()).empty());ASSERT_TRUE(target.saveCheckpoint(copy.string()).empty());EXPECT_EQ(read(copy),pristine);
            for(int k=0;k<step;++k)ASSERT_EQ(valid.step(&elapsed),SWMM_OK);
            const auto good=evidence(tag+"-valid.bin");ASSERT_TRUE(valid.saveCheckpoint(good.string()).empty());
            error=target.loadCheckpoint(good.string());ASSERT_TRUE(error.empty())<<error;
            ASSERT_TRUE(target.saveCheckpoint(copy.string()).empty());EXPECT_EQ(read(copy),read(good));
            EXPECT_EQ(target.close(),SWMM_OK);EXPECT_EQ(valid.close(),SWMM_OK);
        }
        EXPECT_EQ(source.close(),SWMM_OK);
    }
}
TEST(ChangingVolumeNativeBases, CrcValidRowBasisChangesRefuseBeforePublication) {
    for(int basis=0;basis<3;++basis) {
        const auto tag="basis-corrupt-"+std::to_string(basis);SWMMEngine source,target;const auto input=basisModel(true,basis);
        ASSERT_NO_FATAL_FAILURE(startSliced(source,tag+"-source",input));
        ASSERT_NO_FATAL_FAILURE(startSliced(target,tag+"-target",input));
        double elapsed=0;for(int k=0;k<6;++k)ASSERT_EQ(source.step(&elapsed),SWMM_OK);
        const auto receipt=source.livePassiveExchange()->receipts().receipts().front();const auto good=evidence(tag+".bin"),copy=evidence(tag+"-copy.bin");
        ASSERT_TRUE(source.saveCheckpoint(good.string()).empty());const auto bytes=read(good);ASSERT_TRUE(target.saveCheckpoint(copy.string()).empty());const auto pristine=read(copy);
        for(bool remaining:{false,true})for(int proposed:{(basis+1)%3,99}) {
            auto bad=receipt;(remaining?bad.remaining:bad.accepted)[0].units=static_cast<MassUnits>(proposed);
            const auto invalid=editReceiving(bytes,[&](std::string& p){replaceUnique(p,encodeSlicedReceipt(receipt),encodeSlicedReceipt(bad));});
            const auto path=evidence(tag+"-"+std::to_string(remaining)+"-"+std::to_string(proposed)+".bin");std::ofstream(path,std::ios::binary)<<invalid;
            CheckpointInspection inspection;inspection.engine_seconds=123;const auto before=inspection;
            auto error=target.inspectCheckpoint(path.string(),inspection);EXPECT_FALSE(error.empty());EXPECT_EQ(inspection,before);EXPECT_EQ(error.find("checksum"),std::string::npos)<<error;
            EXPECT_FALSE(target.loadCheckpoint(path.string()).empty());ASSERT_TRUE(target.saveCheckpoint(copy.string()).empty());EXPECT_EQ(read(copy),pristine);
        }
        ASSERT_TRUE(target.loadCheckpoint(good.string()).empty());ASSERT_TRUE(target.saveCheckpoint(copy.string()).empty());EXPECT_EQ(read(copy),bytes);
        EXPECT_EQ(source.close(),SWMM_OK);EXPECT_EQ(target.close(),SWMM_OK);
    }
}

TEST(ChangingVolumeNativeBases, UnrecognizedAuthoredUnitsRefuseCouplingAndPreserveNativeFallback) {
    for(const char* token:{"COUNTS/L","PPM","KG/M3"}) {
        auto input=authoredSlices(true);const auto at=input.find("Dye MG/L");input.replace(at,strlen("Dye MG/L"),std::string("Dye ")+token);
        SWMMEngine invalid,ordinary;
        ASSERT_NO_FATAL_FAILURE(start(invalid,"basis-unknown",input,false,false));
        ASSERT_NO_FATAL_FAILURE(start(ordinary,"basis-fallback",authoredSlices(true),false,false));
        EXPECT_EQ(invalid.context().pollutants.units[0],MassUnits::MG_PER_L);
        ASSERT_EQ(invalid.context().pollutants.unit_declaration_valid.size(),1u);EXPECT_EQ(invalid.context().pollutants.unit_declaration_valid[0],0);
        const auto error=invalid.attachLiveChangingVolumeSlicedExchange();EXPECT_NE(error.find("unit declaration"),std::string::npos)<<error;
        EXPECT_EQ(invalid.livePassiveExchange(),nullptr);EXPECT_EQ(invalid.context().elapsed_ms,0);
        double elapsed=0;for(int k=0;k<8;++k) {
            ASSERT_EQ(invalid.step(&elapsed),SWMM_OK);ASSERT_EQ(ordinary.step(&elapsed),SWMM_OK);
            ASSERT_NO_FATAL_FAILURE(sameWater(invalid,ordinary));EXPECT_EQ(invalid.context().nodes.conc,ordinary.context().nodes.conc);
            EXPECT_EQ(invalid.surfaceRouter2D().state().transport.cell_mass,ordinary.surfaceRouter2D().state().transport.cell_mass);
        }
        EXPECT_EQ(invalid.close(),SWMM_OK);EXPECT_EQ(ordinary.close(),SWMM_OK);
    }
}
TEST(ChangingVolumeNativeBases, DeclarationValiditySurvivesDeletionAndExplicitCorrection) {
    for(int basis=0;basis<3;++basis)for(bool correct:{false,true}) {
        auto input=authoredSlices(true);const auto at=input.find("[POLLUTANTS]\nDye MG/L");
        input.replace(at,strlen("[POLLUTANTS]\nDye MG/L"),"[POLLUTANTS]\nEarlier MG/L 0 0 0 0 NO * 0 0 0\nDye COUNTS/L");
        const auto tag="basis-repair-"+std::to_string(basis)+"-"+std::to_string(correct);std::ofstream(evidence(tag+".inp"))<<input;
        SWMMEngine e;ASSERT_EQ(e.open(io::path_utf8(evidence(tag+".inp")).c_str(),io::path_utf8(evidence(tag+".rpt")).c_str(),nullptr),SWMM_OK);
        auto handle=static_cast<SWMM_Engine>(&e);ASSERT_EQ(e.context().pollutants.unit_declaration_valid,(std::vector<std::uint8_t>{1,0}));
        ASSERT_EQ(swmm_pollutant_delete(handle,0,nullptr),SWMM_OK);EXPECT_EQ(e.context().pollutants.unit_declaration_valid,(std::vector<std::uint8_t>{0}));
        ASSERT_EQ(swmm_pollutant_add(handle,"Extra",1),SWMM_OK);EXPECT_EQ(e.context().pollutants.unit_declaration_valid,(std::vector<std::uint8_t>{0,1}));
        ASSERT_EQ(swmm_pollutant_add(handle,"Wrapped",256),SWMM_OK);
        EXPECT_EQ(e.context().pollutants.unit_declaration_valid,(std::vector<std::uint8_t>{0,1,0}));
        ASSERT_EQ(swmm_pollutant_delete(handle,1,nullptr),SWMM_OK);
        EXPECT_EQ(e.context().pollutants.unit_declaration_valid,(std::vector<std::uint8_t>{0,0}));
        ASSERT_EQ(swmm_pollutant_delete(handle,1,nullptr),SWMM_OK);
        const auto before=e.context().pollutants.units;EXPECT_EQ(swmm_pollutant_set_units(handle,0,99),SWMM_ERR_BADPARAM);
        EXPECT_EQ(e.context().pollutants.units,before);EXPECT_EQ(e.context().pollutants.unit_declaration_valid,(std::vector<std::uint8_t>{0}));
        if(correct) {ASSERT_EQ(swmm_pollutant_set_units(handle,0,basis),SWMM_OK);EXPECT_EQ(e.context().pollutants.unit_declaration_valid,(std::vector<std::uint8_t>{1}));}
        ASSERT_EQ(e.initialize(),SWMM_OK);ASSERT_EQ(e.start(0),SWMM_OK);auto error=e.attachLiveChangingVolumeSlicedExchange();
        if(correct)EXPECT_TRUE(error.empty())<<error;else EXPECT_NE(error.find("unit declaration"),std::string::npos)<<error;
        EXPECT_EQ(e.close(),SWMM_OK);
    }
}

TEST(PassiveStorageFunding, AuthoredNegativeRReducesAvailablePreCreditInventory) {
    bool witnessed=false;
    for(bool positive:{true,false}) {
        const auto tag=std::string("funding-negative-r-")+(positive?"positive":"negative");SWMMEngine e;
        ASSERT_NO_FATAL_FAILURE(start(e,tag,model(positive,false),false,false));
        ASSERT_TRUE(e.attachLiveChangingVolumeSlicedExchange(64).empty());
        double elapsed=0;for(int k=0;k<3;++k)ASSERT_EQ(e.step(&elapsed),SWMM_OK)<<e.context().error_message;
        auto* owner=const_cast<quality::LivePassiveStorageExchange*>(e.livePassiveExchange());const auto& book=owner->receipts();
        {
            witnessed|=book.storedRoundoff(0).negative();const auto* pool=book.settledPool(0);ASSERT_NE(pool,nullptr);
            NativeAmountSum expected;expected.add(pool->volume*pool->concentration);
            if(book.storedRoundoff(0).negative())expected.add(book.storedRoundoff(0));
            const auto sealed=book.pendingSpillAmount(0);for(double part:sealed.parts())expected.add(-part);
            const auto start=book.receipts().back().publication.batch_end_ms;
            // Owner-level interruption with the actual newly settled sample;
            // the authored REPORT/FINAL test below exercises cache publication.
            auto surface=e.surfaceRouter2D().state();const auto published=e.context().nodes.conc;surface.node_row_conc=&published;
            ASSERT_NO_THROW(owner->beginWindow(e.context(),surface,start,e.context().elapsed_ms,
                quality::PassiveStorageExchange::Phase::REPORT));
            const auto* grant=book.fundingAllowance();ASSERT_NE(grant,nullptr);
            EXPECT_EQ(grant->pools[0].granted,expected);
            NativeAmountSum physical;physical.add(pool->volume*pool->concentration);
            for(double part:sealed.parts())physical.add(-part);
            if(book.storedRoundoff(0).negative())EXPECT_NE(grant->pools[0].granted,physical);
            else EXPECT_EQ(grant->pools[0].granted,physical);
            std::ofstream audit(evidence(tag+".funding"));audit<<std::hexfloat<<pool->volume*pool->concentration;
            for(const auto* sum:{&grant->pools[0].roundoff,&grant->pools[0].sealed,&grant->pools[0].granted}) {
                audit<<" |";for(double part:sum->parts())audit<<' '<<part;
            }
            audit<<'\n';
            EXPECT_FALSE(book.materialSealPrepared());EXPECT_THROW(book.checkpoint(e.context()),std::exception);
            owner->fail("Retained manufactured interruption before native REPORT effects.");
        }
        e.close();
    }
    EXPECT_TRUE(witnessed);
}

TEST(PassiveStorageFunding, ActualPostQualityReportAndFinalUseTheNewSampleAndRecover) {
    for(bool final:{false,true})for(bool positive:{true,false}) {
        const auto tag=std::string("funding-post-quality-")+(final?"final-":"report-")+(positive?"positive":"negative");
        SWMMEngine e,target;const auto input=authoredSlices(positive);
        ASSERT_NO_FATAL_FAILURE(startSliced(e,tag,input));
        ASSERT_NO_FATAL_FAILURE(startSliced(target,tag+"-restore",input));
        double elapsed=0;for(int k=0;k<5;++k)ASSERT_EQ(e.step(&elapsed),SWMM_OK)<<e.context().error_message;
        const auto& book=e.livePassiveExchange()->receipts();ASSERT_FALSE(book.receipts().empty());
        const double preceding=book.receipts().back().proof.legs.back().sample,current=e.context().nodes.conc[0];
        ASSERT_NE(preceding,current);
        const auto before=book.receipts().size();
        if(final)ASSERT_EQ(e.end(),SWMM_OK)<<e.context().error_message;
        else ASSERT_NO_THROW(e.surfaceRouter2D().flushPendingBatch(e.context()));
        ASSERT_EQ(book.receipts().size(),before+1);const auto& receipt=book.receipts().back();
        EXPECT_EQ(receipt.publication.phase,final?quality::PassiveStorageExchange::Phase::FINAL:quality::PassiveStorageExchange::Phase::REPORT);
        EXPECT_EQ(receipt.samples[0].concentration,current);
        bool spill=false;for(const auto& leg:receipt.proof.legs)if(!leg.drain){spill=true;EXPECT_EQ(leg.sample,current);}
        EXPECT_TRUE(spill);EXPECT_NE(book.pendingSpillAmount(0).rounded(),0);
        EXPECT_EQ(book.fundingAllowance(),nullptr);EXPECT_FALSE(book.materialSealPrepared());
        if(final) {
            const auto bytes=e.livePassiveExchange()->checkpoint(e.context(),e.surfaceRouter2D().state());
            auto restored=target.livePassiveExchange()->prepareRestore(e.context(),e.surfaceRouter2D().state(),bytes);
            EXPECT_EQ(restored->checkpoint(e.context(),e.surfaceRouter2D().state()),bytes);
        } else {
            const auto saved=evidence(tag+".bin"),copy=evidence(tag+"-copy.bin");ASSERT_TRUE(e.saveCheckpoint(saved.string()).empty());
            ASSERT_TRUE(target.loadCheckpoint(saved.string()).empty());ASSERT_TRUE(target.saveCheckpoint(copy.string()).empty());
            EXPECT_EQ(read(saved),read(copy));
        }
        EXPECT_EQ(e.close(),SWMM_OK);EXPECT_EQ(target.close(),SWMM_OK);
    }
}
