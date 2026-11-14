// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include <gtest/gtest.h>
#include "core/SWMMEngine.hpp"
#include "core/FileIO.hpp"
#include "core/DateTime.hpp"
#include "io/CheckpointOutputPublication.hpp"
#include "core/SplitFrame.hpp"
#include "quality/PeerPassiveStorageExchange.hpp"
#include "quality/QualityRouting.hpp"
#include "core/CompletedSourceArchive.hpp"
#include "hydraulics/TimestepController.hpp"
#include "live_passive_recovery_test_support.hpp"
#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_split_packet.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <bit>
#include <cmath>
#include <sstream>
#include <functional>
#include "test_material_transport.h"
#ifdef MATERIAL_PACKET_TESTS
#include <nlohmann/json.hpp>
#endif

namespace {
using namespace openswmm;
namespace fs=std::filesystem;
using Book=quality::PassiveStorageExchange;
struct Engine:SWMMEngine {~Engine(){close();}};
std::string read(const fs::path& path){std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
struct FrameOwner {SWMM_SplitFrame value=nullptr;~FrameOwner(){swmm_split_frame_destroy(value);}};
struct MaterialPair {
    Engine reference,network,surface;
    fs::path dir,input;
    int advances=0,reports=0,finishes=0,conductances=0,accepted=0;
    bool drop=false;
    int corrupt_water=0;
    std::vector<double> before_queue;
    double before_span=0,before_surface_in=0,before_surface_out=0;
    std::ofstream phases;
    std::function<void(int,const SplitFrame*)> exchange_observer;
    std::function<void(const SplitFrame&)> request_observer;
    ~MaterialPair(){network.close();surface.close();reference.close();}
    static int exchange(SWMM_Engine e,int phase,SWMM_SplitFrame request,SWMM_SplitFrame* reply,void* user) {
        auto& p=*static_cast<MaterialPair*>(user);const auto& r=*static_cast<SplitFrame*>(request);
        auto& network=*static_cast<SWMMEngine*>(e);
        p.before_queue=network.context().nodes.coupling_queue;p.before_span=network.context().coupling_delivery_remaining;
        p.before_surface_in=network.context().mass_balance_2d.coupling_1d_to_2d_in;
        p.before_surface_out=network.context().mass_balance_2d.coupling_2d_to_1d_out;
        EXPECT_TRUE(r.material);EXPECT_TRUE(r.settled_reports);
        if(p.request_observer)p.request_observer(r);
        if(p.exchange_observer)p.exchange_observer(phase,nullptr);
        const int rc=p.surface.evaluateSplit(phase,request,reply);
        if(rc)return rc;
        if(p.exchange_observer)p.exchange_observer(phase,static_cast<const SplitFrame*>(*reply));
        if(phase==SWMM_SPLIT_ADVANCE)++p.advances;if(phase==SplitExecution::REPORT)++p.reports;
        if(phase==SWMM_SPLIT_FINISH)++p.finishes;if(phase==SWMM_SPLIT_CONDUCTANCE)++p.conductances;
        if(p.phases)p.phases<<phase<<','<<r.sequence<<','<<r.start<<','<<r.dt<<','<<r.report_time_ms<<'\n';
        if(p.corrupt_water && phase==SWMM_SPLIT_ADVANCE && p.surface.peerMaterialProducer()->legCount()>0) {
            auto& value=*static_cast<SplitFrame*>(*reply);
            if(p.corrupt_water==1)value.nodes.coupling_queue[0]+=1;
            else value.remaining+=1;
        }
        if(p.drop && phase==SWMM_SPLIT_ADVANCE && p.surface.peerMaterialProducer()->legCount()>0) {
            swmm_split_frame_destroy(*reply);*reply=nullptr;return SWMM_ERR_PLUGIN;
        }
        return SWMM_OK;
    }
    static void observe(SWMM_Engine,int phase,double,double,void* user) {
        auto& p=*static_cast<MaterialPair*>(user);
        if(phase==SWMM_PHASE_STEP_ACCEPTED) {
            ++p.accepted;EXPECT_FALSE(p.surface.peerMaterialProducer()->awaitingAcknowledgement());
            EXPECT_FALSE(p.network.peerMaterialFunding()->unresolved());
            EXPECT_EQ(p.surface.context().nodes.conc,p.network.context().nodes.conc);
            EXPECT_EQ(p.surface.close(),SWMM_ERR_LIFECYCLE);
            EXPECT_EQ(p.network.close(),SWMM_ERR_LIFECYCLE);
        }
    }
    void start(const std::string& tag,bool positive,int basis=0,bool automatic_report=false,bool bind=true,bool natural_completion=false,MaterialTestTransport* transport=nullptr,bool tail=false,const fs::path& same_input={},bool checkpoint_outputs=false,int mesh_stride=1,bool delayed_report=false,const std::string& output_selection={}) {
        const char* chosen=std::getenv("OPENSWMM_MATERIAL_PAIR_EVIDENCE");
        const std::size_t capacity=tail?6:natural_completion?4096:64;
        dir=fs::path(chosen?chosen:MATERIAL_PAIR_EVIDENCE)/tag;fs::create_directories(dir);input=same_input.empty()?dir/"model.inp":same_input;
        auto text=read(fs::path(MATERIAL_PAIR_DATA)/(std::string(tail?"tail-":"")+(positive?"positive.inp":"negative.inp")));
        ASSERT_FALSE(text.empty());
        if(basis){const auto at=text.find("Dye MG/L");text.replace(at,8,basis==1?"Dye UG/L":"Dye #/L");}
        if(automatic_report){
            const std::string from="REPORT_STEP 00:00:01";const auto at=text.find(from);text.replace(at,from.size(),"REPORT_STEP 00:00:00.00244140625");
            const std::string mesh="REPORT_2D NO";text.replace(text.find(mesh),mesh.size(),"REPORT_2D YES\nREPORT_2D_STEP 0.00244140625");
            if(checkpoint_outputs){
                for(const auto& edit:std::vector<std::pair<std::string,std::string>>{{"REPORT_STEP 00:00:00.00244140625","REPORT_STEP 00:00:00.0009765625"},{"REPORT_2D_STEP 0.00244140625","REPORT_2D_STEP 0.0009765625"}}){const auto at=text.find(edit.first);text.replace(at,edit.first.size(),edit.second);}
            }
            if(mesh_stride>1){const auto from=std::string("REPORT_2D_STEP 0.0009765625");text.replace(text.find(from),from.size(),"REPORT_2D_STEP 0.001953125");}
            if(delayed_report){const auto at=text.find("REPORT_STEP ");text.insert(at,"REPORT_START_TIME 00:00:00.004\n");}
            if(!output_selection.empty())text.insert(text.find("[2D_VERTICES]"),output_selection);
        }
        if(natural_completion) {
            for(const auto& edit:std::vector<std::pair<std::string,std::string>>{
                {"ROUTING_STEP 0.00048828125","ROUTING_STEP 0.25"},
                {"MAX_TIMESTEP 0.00048828125","MAX_TIMESTEP 0.25"},
                {"COUPLING_SYNC 0.001953125","COUPLING_SYNC 2"}}) {
                const auto at=text.find(edit.first);ASSERT_NE(at,std::string::npos);text.replace(at,edit.first.size(),edit.second);
            }
        }
        if (same_input.empty())
            std::ofstream(input, std::ios::binary) << text;
        else
            ASSERT_EQ(read(input), text);
        ASSERT_EQ(reference.open(io::path_utf8(input).c_str(),io::path_utf8((dir/"native.rpt")).c_str(),automatic_report?io::path_utf8((dir/"native.out")).c_str():nullptr),SWMM_OK)<<reference.context().error_message;
        ASSERT_EQ(network.open(io::path_utf8(input).c_str(),checkpoint_outputs?nullptr:io::path_utf8((dir/"network.rpt")).c_str(),automatic_report?io::path_utf8((dir/"network.out")).c_str():nullptr),SWMM_OK);
        ASSERT_EQ(surface.open(io::path_utf8(input).c_str(),checkpoint_outputs?nullptr:io::path_utf8((dir/"surface.rpt")).c_str(),nullptr),SWMM_OK);
        if(delayed_report)for(auto* engine:{&reference,&network,&surface}) {
            // Exercise a subsecond native report gate; the INP calendar-time
            // parser rounds these tiny authored time-of-day values to seconds.
            auto& o=engine->context().options;o.report_start=datetime::addSeconds(o.start_date,.0025);
            ASSERT_GT(o.report_start,o.start_date);
        }
        ASSERT_EQ(network.configureMaterialSplit(SWMM_SPLIT_NETWORK),SWMM_OK)<<network.context().error_message;
        ASSERT_EQ(surface.configureMaterialSplit(SWMM_SPLIT_SURFACE),SWMM_OK)<<surface.context().error_message;
        if(automatic_report) {
            fs::remove(dir/"native.h5");fs::remove(dir/"surface.h5");
            ASSERT_EQ(swmm_2d_set_results_file(&reference,io::path_utf8((dir/"native.h5")).c_str()),SWMM_OK);
            ASSERT_EQ(swmm_2d_set_results_file(&surface,io::path_utf8((dir/"surface.h5")).c_str()),SWMM_OK);
        }
        ASSERT_EQ(surface.initialize(),SWMM_OK);FrameOwner geometry;
        ASSERT_EQ(surface.splitGeometry(&geometry.value),SWMM_OK)<<surface.context().error_message;
        char* encoded=nullptr;EXPECT_NE(swmm_split_packet_encode(&surface,geometry.value,&encoded),SWMM_OK);EXPECT_EQ(encoded,nullptr);
        ASSERT_EQ(network.initialize(),SWMM_OK);ASSERT_EQ(network.applySplitGeometry(geometry.value),SWMM_OK)<<network.context().error_message;
        if(checkpoint_outputs){ASSERT_TRUE(network.prepareOutputCheckpointing().empty());ASSERT_TRUE(surface.prepareSurfaceOutputCheckpointing().empty());}
        if(transport){transport->surface=&surface;ASSERT_EQ(network.setSplitCallback(material_test_exchange,transport),SWMM_OK);}
        else ASSERT_EQ(network.setSplitCallback(exchange,this),SWMM_OK);
        ASSERT_EQ(reference.initialize(),SWMM_OK);
        ASSERT_EQ(reference.start(automatic_report),SWMM_OK);
        ASSERT_TRUE(reference.attachLiveChangingVolumeSlicedExchange(capacity).empty());
        ASSERT_EQ(surface.start(0),SWMM_OK);ASSERT_EQ(network.start(automatic_report),SWMM_OK);
        if(bind)ASSERT_EQ(network.bindMaterialPeer(surface,capacity),SWMM_OK)<<network.context().error_message;
        phases.open(dir/"phases.csv");phases<<std::setprecision(17);
        if(transport)transport->log=io::fopen_path(dir/"packets.jsonl","w");
        if(bind)ASSERT_EQ(swmm_set_phase_callback(&surface,observe,this),SWMM_OK);
    }
    void same(bool closed=false) {
        const auto& a=reference.context();const auto& n=network.context();const auto& s=surface.context();
        EXPECT_EQ(a.elapsed_ms,n.elapsed_ms);EXPECT_EQ(s.elapsed_ms,n.elapsed_ms);EXPECT_EQ(a.current_time,n.current_time);
        EXPECT_EQ(a.next_report_ms,n.next_report_ms);EXPECT_EQ(s.next_report_ms,n.next_report_ms);
#define SAME(field) EXPECT_EQ(a.nodes.field,n.nodes.field)<<#field;
        SAME(volume) SAME(old_volume) SAME(depth) SAME(old_depth) SAME(head) SAME(conc) SAME(conc_old)
        SAME(coupling_queue) SAME(coupling_volume) SAME(coupling_inflow) SAME(old_net_inflow) SAME(old_lat_flow)
#undef SAME
        EXPECT_EQ(n.nodes.coupling_queue,s.nodes.coupling_queue);EXPECT_EQ(n.coupling_delivery_remaining,s.coupling_delivery_remaining);
        EXPECT_EQ(a.coupling_delivery_remaining,n.coupling_delivery_remaining);
        const auto& x=reference.surfaceRouter2D().state();const auto& y=surface.surfaceRouter2D().state();
        EXPECT_EQ(x.volume,y.volume);EXPECT_EQ(x.depth,y.depth);EXPECT_EQ(x.head,y.head);
        EXPECT_EQ(x.transport.cell_mass,y.transport.cell_mass);EXPECT_EQ(x.transport.lost_coupling,y.transport.lost_coupling);
        EXPECT_EQ(x.transport.gained_coupling,y.transport.gained_coupling);
        const auto* owner=reference.livePassiveExchange();const auto* peer=surface.peerMaterialProducer();
        ASSERT_NE(owner,nullptr);ASSERT_NE(peer,nullptr);ASSERT_EQ(owner->legCount(),peer->legCount());
        for(std::size_t k=0;k<owner->legCount();++k)EXPECT_EQ(owner->leg(k),peer->leg(k))<<k;
        const auto& book=network.peerMaterialFunding()->receipts();const auto& expected=owner->receipts();
        EXPECT_EQ(book.receipts(),expected.receipts());EXPECT_EQ(book.storedRoundoff(0),expected.storedRoundoff(0));
        ASSERT_NE(book.settledPool(0),nullptr);EXPECT_EQ(*book.settledPool(0),*expected.settledPool(0));
        for(double value:n.nodes.coupling_qual_queue)EXPECT_EQ(value,0);
        for(double value:s.nodes.coupling_qual_queue)EXPECT_EQ(value,0);
        EXPECT_FALSE(network.peerMaterialFunding()->unresolved());EXPECT_FALSE(peer->awaitingAcknowledgement());
        if(!closed)EXPECT_EQ(n.nodes.conc,s.nodes.conc);
    }
    void steps(int count) {
        double a=0,b=0;
        for(int k=0;k<count;++k) {
            ASSERT_EQ(reference.step(&a),SWMM_OK)<<reference.context().error_message;
            ASSERT_EQ(network.step(&b),SWMM_OK)<<network.context().error_message;
            EXPECT_EQ(a,b);
        ASSERT_NO_FATAL_FAILURE(same());
        }
        EXPECT_EQ(advances,count);EXPECT_EQ(accepted,count);EXPECT_GE(conductances,count);
    }
};

// Owner-only recovery qualification. Numerical observations are supplied by the
// actual native engines; this fixture never restores or marches a second water
// or surface solver. Once installed, its C/old C, receipt remainders and roundoff
// can change only through its own native receiving quality solver.
struct PartialOwnerContinuation {
    using Funding=quality::PeerPassiveStorageFunding;
    using Producer=quality::PeerPassiveStorageProducer;
    using Recovery=quality::PeerPassiveStorageCheckpoint;
    MaterialPair& source;
    SimulationContext network,surface;
    twoD::SurfaceStateData state;
    std::vector<twoD::CouplingPoint> points;
    std::vector<double> published;
    quality::QualitySolver solver;
    std::unique_ptr<Funding> issuer;
    std::unique_ptr<Producer> producer;
    std::string error;
    bool active=false;
    int phase=0,received=0,exchanges=0;
    std::size_t prior_receipts=0,prior_legs=0;
    double dt=0,span=0;
    explicit PartialOwnerContinuation(MaterialPair& p):source(p) {
        auto initial=[](const SimulationContext& a,SimulationContext& b) {
            for(const auto& n:a.node_names.names())b.node_names.add(n);
            for(const auto& n:a.pollutant_names.names())b.pollutant_names.add(n);
            b.allocate_objects();b.options=a.options;b.pollutants=a.pollutants;b.node_subtypes=a.node_subtypes;
        };
        initial(p.network.context(),network);initial(p.surface.context(),surface);
        snapshot(p.network.context(),network);snapshot(p.surface.context(),surface);
        const auto& s=p.surface.surfaceRouter2D().state();points=*s.node_coupling;
        state.resize(s.volume.size(),s.vert_head.size());state.transport.resize(1,s.volume.size(),points.size());
        state.nodes_1d=&surface.nodes;state.node_coupling=&points;state.node_row_conc=&published;
        snapshotSurface();
        std::vector<Book::HydraulicPoint> map;
        for(std::size_t k=0;k<points.size();++k)map.push_back({k,points[k].cell_idx,points[k].node_idx,0});
        const auto id=p.network.peerMaterialFunding()->identity();
        const auto factor=p.surface.surfaceRouter2D().options().flow_2d_to_1d;
        issuer=std::make_unique<Funding>(network,id.issuer,id.recipient,map,factor,64);
        producer=std::make_unique<Producer>(surface,state,issuer->bindRecipient(surface),factor,64);
        solver.init(network.n_nodes(),0,1);
    }
    ~PartialOwnerContinuation() {
        if(active) {
            swmm_set_phase_callback(&source.network,nullptr,nullptr);
            source.exchange_observer={};
        }
    }
    static void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
    static void snapshot(const SimulationContext& a,SimulationContext& b) {
        b.nodes=a.nodes;b.elapsed_ms=a.elapsed_ms;b.old_elapsed_ms=a.old_elapsed_ms;
        b.current_time=a.current_time;b.current_date=a.current_date;b.next_report_ms=a.next_report_ms;
        b.coupling_delivery_remaining=a.coupling_delivery_remaining;b.mass_balance=a.mass_balance;
    }
    void snapshotSurface() {
        const auto& s=source.surface.surfaceRouter2D().state();
        state.volume=s.volume;state.transport.cell_mass=s.transport.cell_mass;published=*s.node_row_conc;
    }
    static void water(const SimulationContext& a,SimulationContext& b) {
        // Only native water inputs. Do not copy quality or the receiver's book.
#define OBSERVE(v) b.nodes.v=a.nodes.v;
        OBSERVE(volume) OBSERVE(old_volume) OBSERVE(depth) OBSERVE(old_depth) OBSERVE(head)
        OBSERVE(inflow) OBSERVE(outflow) OBSERVE(old_net_inflow) OBSERVE(old_lat_flow) OBSERVE(old_inflow)
        OBSERVE(coupling_inflow) OBSERVE(coupling_queue) OBSERVE(lat_flow)
#undef OBSERVE
        b.coupling_delivery_remaining=a.coupling_delivery_remaining;
    }
    static std::string payload(const std::string& bytes) {
        detail::CompletedSourceArchive in(bytes);
        std::string magic,body;std::uint32_t version,endian,width,crc;
        in(magic,version,endian,width,crc,body);in.finish();
        detail::CompletedSourceArchive data(body),prefix;std::uint64_t a,b,c;
        data(a,b,c);prefix(a,b,c);
        return body.substr(prefix.bytes().size()); // exclude only fresh identity provenance
    }
    void restore(const std::string& bytes) {
        const auto& n=source.network.context();const auto& s=source.surface.context();
        auto prepared=Recovery::prepare(issuer,producer,n,s,source.surface.surfaceRouter2D().state(),bytes);
        prepared.validateTarget();
        snapshot(n,network);snapshot(s,surface);snapshotSurface();prepared.install();
        require(issuer->returnedReply()==nullptr,"Restore resurrected a retired reply.");
        require(payload(Recovery::save(*issuer,*producer))==payload(bytes),"Restored owner payload differs.");
        active=true;
        require(swmm_set_phase_callback(&source.network,observe,this)==SWMM_OK,"Phase registration failed.");
        source.exchange_observer=[this](int event,const SplitFrame* returned) noexcept {
            if(!error.empty())return;
            try{exchanged(event,returned);}catch(const std::exception& e){error=e.what();}
        };
    }
    void onPhase(int event,double start,double end) {
        const auto& n=source.network.context();
        if(event==SWMM_PHASE_RUNOFF_PASS_COMPLETE) {
            require(phase==0,"Repeated or incomplete receiving interval.");
            dt=end-start;require(dt>0 && network.elapsed_ms==n.elapsed_ms,"Receiving clock differs.");
            network.save_lat_qual_state();water(n,network);span=network.coupling_delivery_remaining;
            require(network.nodes.conc==n.nodes.conc && network.nodes.conc_old==n.nodes.conc_old,
                "Receiver concentrations differ before continuation.");
            issuer->freeze(dt);phase=1;
        } else if(event==SWMM_PHASE_LID_ADMISSION_PREPARED) {
            require(phase==1,"Missing receiving freeze.");water(n,network);issuer->observeDrain();phase=2;
        } else if(event==SWMM_PHASE_NETWORK_ROUTED) {
            require(phase==2,"Missing observed native lateral assembly.");water(n,network);
            phase=3;
        } else if(event==SWMM_PHASE_INVENTORY_CLOSED) {
            require(phase==4,"Missing native receiving solve.");
            hydraulics::TimestepController::advance(network,dt);
            hydraulics::TimestepController::advance(surface,dt);
            require(network.elapsed_ms==n.elapsed_ms && network.current_time==n.current_time,
                "Continued native clocks differ.");
            surface.nodes.conc=network.nodes.conc;water(source.surface.context(),surface);phase=0;
        }
    }
    static void observe(SWMM_Engine,int event,double start,double end,void* user) noexcept {
        auto& p=*static_cast<PartialOwnerContinuation*>(user);
        if(!p.error.empty())return;
        try{p.onPhase(event,start,end);}catch(const std::exception& e){p.error=e.what();}
    }
    void exchanged(int event,const SplitFrame* returned) {
        if(event!=SWMM_SPLIT_ADVANCE)return;
        if(!returned) {
            prior_receipts=source.network.peerMaterialFunding()->receipts().receipts().size();
            prior_legs=source.surface.peerMaterialProducer()->legCount();return;
        }
        require(phase==3,"ADVANCE lacks its staged native hydraulic observations.");
        require(returned->nodes.coupling_queue==source.before_queue && returned->remaining==source.before_span &&
                returned->nodes.coupling_volume==std::vector<double>(source.before_queue.size(),0) &&
                source.network.peerMaterialFunding()->receipts().receipts().size()==prior_receipts &&
                source.surface.peerMaterialProducer()->legCount()==prior_legs &&
                !source.network.peerMaterialFunding()->unresolved(),"Continuation acquired a new publication/refill.");
        const auto& s=source.surface.surfaceRouter2D().state();
        require(s.volume==state.volume && s.transport.cell_mass==state.transport.cell_mass,
                "Continuation changed the captured surface inventory.");
        // Match native ordering: receive only after the actual ADVANCE has
        // established that this interval contains no grant, proof or refill.
        issuer->receive(solver,dt);++received;++exchanges;phase=4;
    }
    void same() const {
        ASSERT_TRUE(error.empty())<<error;EXPECT_EQ(phase,0);EXPECT_EQ(received,exchanges);
        const auto& a=source.network.context();
        EXPECT_EQ(network.nodes.conc,a.nodes.conc);EXPECT_EQ(network.nodes.conc_old,a.nodes.conc_old);
        EXPECT_EQ(network.nodes.coupling_queue,a.nodes.coupling_queue);
        EXPECT_EQ(network.coupling_delivery_remaining,a.coupling_delivery_remaining);
        const auto& book=source.network.peerMaterialFunding()->receipts();
        EXPECT_EQ(issuer->receipts().receipts(),book.receipts());
        EXPECT_EQ(issuer->receipts().storedRoundoff(0),book.storedRoundoff(0));
        EXPECT_EQ(*issuer->receipts().settledPool(0),*book.settledPool(0));
        EXPECT_EQ(issuer->receipts().pendingDrainAmount(0),book.pendingDrainAmount(0));
        EXPECT_EQ(issuer->receipts().pendingSpillAmount(0),book.pendingSpillAmount(0));
        EXPECT_EQ(payload(Recovery::save(*issuer,*producer)),
            payload(Recovery::save(*source.network.peerMaterialFunding(),*source.surface.peerMaterialProducer())));
    }
    static void trace(const fs::path& path,int step,const SimulationContext& ctx,const Book& book) {
        std::ofstream out(path,std::ios::app);out<<std::setprecision(17);
        const auto& n=ctx.nodes;const auto& r=book.storedRoundoff(0);
        out<<"state "<<step<<' '<<ctx.elapsed_ms<<' '<<n.volume[0]<<' '<<n.old_volume[0]<<' '
            <<n.conc[0]<<' '<<n.conc_old[0]<<' '<<n.coupling_queue[0]<<' '<<ctx.coupling_delivery_remaining<<' '<<r.parts().size();
        for(double part:r.parts())out<<' '<<part;out<<'\n';
        for(const auto& receipt:book.receipts())for(std::size_t k=0;k<receipt.proof.legs.size();++k) {
            const auto& l=receipt.proof.legs[k];
            out<<"leg "<<step<<' '<<receipt.id<<' '<<k<<' '<<l.drain<<' '<<l.water_native<<' '<<l.mass_native<<' '
                <<receipt.remaining_water_legs[k]<<' '<<receipt.remaining_mass_legs[k]<<'\n';
        }
    }
};

void partialOwnerCase(int basis,bool positive,int checkpoint,const std::string& imported={}) {
    const auto tag="owner-partial-"+std::to_string(basis)+(positive?"-positive-":"-negative-")+std::to_string(checkpoint);
    MaterialPair p;ASSERT_NO_FATAL_FAILURE(p.start(tag,positive,basis));
    PartialOwnerContinuation target(p);
    ASSERT_NO_FATAL_FAILURE(p.steps(checkpoint));
    const auto& receipt=p.network.peerMaterialFunding()->receipts().receipts().front();
    ASSERT_EQ(receipt.proof.legs.size(),8u);ASSERT_NE(p.network.context().nodes.coupling_queue[0],0);
    for(std::size_t k=0;k<receipt.proof.legs.size();++k) {
        EXPECT_GT(receipt.remaining_water_legs[k],0);EXPECT_LT(receipt.remaining_water_legs[k],receipt.proof.legs[k].water_native);
        EXPECT_GT(receipt.remaining_mass_legs[k],0);EXPECT_LT(receipt.remaining_mass_legs[k],receipt.proof.legs[k].mass_native);
    }
    const auto bytes=quality::PeerPassiveStorageCheckpoint::save(*p.network.peerMaterialFunding(),*p.surface.peerMaterialProducer());
    std::ofstream(p.dir/"checkpoint.owners",std::ios::binary)<<bytes;
    ASSERT_NO_THROW(target.restore(imported.empty()?bytes:imported));ASSERT_NO_FATAL_FAILURE(target.same());
    const auto original=p.dir/"original.trace",restored=p.dir/"restored.trace";
    std::ofstream(original).close();std::ofstream(restored).close();
    for(int step=checkpoint;step<=7;++step) {
        if(step>checkpoint) {
            double elapsed;ASSERT_EQ(p.reference.step(&elapsed),SWMM_OK)<<p.reference.context().error_message;
            ASSERT_EQ(p.network.step(&elapsed),SWMM_OK)<<p.network.context().error_message;
            ASSERT_NO_FATAL_FAILURE(p.same());
            ASSERT_NO_FATAL_FAILURE(target.same());
        }
        PartialOwnerContinuation::trace(original,step,p.network.context(),p.network.peerMaterialFunding()->receipts());
        PartialOwnerContinuation::trace(restored,step,target.network,target.issuer->receipts());
        std::ofstream(p.dir/("original-"+std::to_string(step)+".owners"),std::ios::binary)
            <<quality::PeerPassiveStorageCheckpoint::save(*p.network.peerMaterialFunding(),*p.surface.peerMaterialProducer());
        std::ofstream(p.dir/("restored-"+std::to_string(step)+".owners"),std::ios::binary)
            <<quality::PeerPassiveStorageCheckpoint::save(*target.issuer,*target.producer);
    }
    EXPECT_EQ(read(original),read(restored));EXPECT_EQ(target.received,7-checkpoint);
    std::ofstream(p.dir/"identity.txt")<<target.issuer->identity().issuer<<' '<<target.issuer->identity().recipient<<' '
        <<target.issuer->identity().session<<'\n';
}
TEST(MaterialOwnerRecovery, AuthoredPartialDeliveryContinuesAllBasesBothSigns) {
    for(int basis=0;basis<3;++basis)for(bool positive:{false,true})for(int checkpoint:{5,6}) {
        SCOPED_TRACE(std::to_string(basis)+"/"+std::to_string(positive)+"/"+std::to_string(checkpoint));
        ASSERT_NO_FATAL_FAILURE(partialOwnerCase(basis,positive,checkpoint));
    }
}
TEST(MaterialOwnerRecovery, FreshProcessPartialChild) {
    const char* which=std::getenv("OPENSWMM_PARTIAL_OWNER_CASE");
    if(!which)GTEST_SKIP()<<"Invoked separately by the fresh-process qualification driver.";
    std::istringstream selected(which);int basis,positive,checkpoint;ASSERT_TRUE(bool(selected>>basis>>positive>>checkpoint));
    ASSERT_GE(basis,0);ASSERT_LT(basis,3);ASSERT_TRUE(checkpoint==5 || checkpoint==6);
    const char* archive=std::getenv("OPENSWMM_PARTIAL_OWNER_ARCHIVE");ASSERT_NE(archive,nullptr);
    const auto bytes=read(archive);ASSERT_FALSE(bytes.empty());
    ASSERT_NO_FATAL_FAILURE(partialOwnerCase(basis,positive,checkpoint,bytes));
}
std::string corruptPartialRemainder(const std::string& bytes,const Book::Receipt& receipt,bool material) {
    using A=detail::CompletedSourceArchive;
    auto crc=[](const std::string& data) {
        std::uint32_t sum=~std::uint32_t(0);
        for(unsigned char c:data){sum^=c;for(int i=0;i<8;++i)sum=(sum>>1)^((sum&1)?0xedb88320u:0u);}
        return ~sum;
    };
    A envelope(bytes);std::string magic,payload;std::uint32_t version,endian,width,checksum;
    envelope(magic,version,endian,width,checksum,payload);envelope.finish();
    A body(payload);std::uint64_t issuer,recipient,session,capacity,count;double factor;
    body(issuer,recipient,session,factor,capacity,count);std::vector<Book::HydraulicPoint> points(count);
    for(auto& p:points)body(p.point,p.cell,p.node,p.exchange_si);std::string book;body(book);body.finish();
    A book_envelope(book);std::string name,book_payload;std::uint32_t book_version,book_endian,book_sum;
    book_envelope(name,book_version,book_endian,book_sum,book_payload);book_envelope.finish();
    auto water=receipt.remaining_water_legs,mass=receipt.remaining_mass_legs;A original;original(water,mass);
    auto& changed=material?mass:water;changed.at(0)=std::nextafter(changed.at(0),0.0);A bad;bad(water,mass);
    const auto at=book_payload.find(original.bytes());
    if(at==std::string::npos || book_payload.rfind(original.bytes())!=at)throw std::runtime_error("Ambiguous partial test columns.");
    book_payload.replace(at,original.bytes().size(),bad.bytes());book_sum=crc(book_payload);
    A book_out;book_out(name,book_version,book_endian,book_sum,book_payload);book=book_out.bytes();
    A body_out;body_out(issuer,recipient,session,factor,capacity,count);
    for(auto p:points)body_out(p.point,p.cell,p.node,p.exchange_si);body_out(book);payload=body_out.bytes();checksum=crc(payload);
    A out;out(magic,version,endian,width,checksum,payload);return out.bytes();
}
TEST(MaterialOwnerRecovery, DamagedPartialQueueSpanOrLegRemainderRefusesAtomicallyAndRetries) {
    using Recovery=quality::PeerPassiveStorageCheckpoint;
    for(int basis=0;basis<3;++basis)for(bool positive:{false,true})for(int checkpoint:{5,6}) {
        SCOPED_TRACE(std::to_string(basis)+"/"+std::to_string(positive)+"/"+std::to_string(checkpoint));
        MaterialPair p;ASSERT_NO_FATAL_FAILURE(p.start("partial-refusal-"+std::to_string(basis)+
            (positive?"-positive-":"-negative-")+std::to_string(checkpoint),positive,basis));
        PartialOwnerContinuation target(p);ASSERT_NO_FATAL_FAILURE(p.steps(checkpoint));
        const auto bytes=Recovery::save(*p.network.peerMaterialFunding(),*p.surface.peerMaterialProducer());
        const auto pristine=Recovery::save(*target.issuer,*target.producer);
        const auto* original_issuer=target.issuer.get();const auto* original_producer=target.producer.get();
        std::ofstream refusals(p.dir/"refusals.txt");
        for(int edit=0;edit<5;++edit) {
            SimulationContext incoming;
            for(const auto& n:p.network.context().node_names.names())incoming.node_names.add(n);
            for(const auto& n:p.network.context().pollutant_names.names())incoming.pollutant_names.add(n);
            incoming.allocate_objects();incoming.options=p.network.context().options;
            incoming.pollutants=p.network.context().pollutants;incoming.node_subtypes=p.network.context().node_subtypes;
            PartialOwnerContinuation::snapshot(p.network.context(),incoming);auto bad=bytes;
            if(edit==0)incoming.nodes.coupling_queue[0]=std::nextafter(incoming.nodes.coupling_queue[0],0.0);
            if(edit==1)incoming.coupling_delivery_remaining=std::nextafter(incoming.coupling_delivery_remaining,INFINITY);
            if(edit==2 || edit==3)bad=corruptPartialRemainder(bytes,p.network.peerMaterialFunding()->receipts().receipts().front(),edit==3);
            if(edit==4)incoming.nodes.conc_old[0]=std::nextafter(incoming.nodes.conc_old[0],0.0);
            bool refused=false;
            try{Recovery::prepare(target.issuer,target.producer,incoming,p.surface.context(),p.surface.surfaceRouter2D().state(),bad);}
            catch(const std::exception& e){refused=true;refusals<<edit<<' '<<e.what()<<'\n';}
            EXPECT_TRUE(refused)<<edit;EXPECT_EQ(original_issuer,target.issuer.get());EXPECT_EQ(original_producer,target.producer.get());
            EXPECT_EQ(pristine,Recovery::save(*target.issuer,*target.producer));
            auto prepared=Recovery::prepare(target.issuer,target.producer,p.network.context(),p.surface.context(),
                p.surface.surfaceRouter2D().state(),bytes);
            EXPECT_NO_THROW(prepared.validateTarget());EXPECT_EQ(pristine,Recovery::save(*target.issuer,*target.producer));
        }
    }
}
TEST(MaterialOwnerRecovery, ActualRefillStopsOwnerOnlyContinuationBeforeItsQualityWrite) {
    for(bool positive:{false,true}) {
        MaterialPair p;ASSERT_NO_FATAL_FAILURE(p.start(positive?"partial-refill-guard-positive":"partial-refill-guard-negative",positive));
        PartialOwnerContinuation target(p);ASSERT_NO_FATAL_FAILURE(p.steps(5));
        ASSERT_NO_THROW(target.restore(quality::PeerPassiveStorageCheckpoint::save(
            *p.network.peerMaterialFunding(),*p.surface.peerMaterialProducer())));
        for(int step=6;step<=7;++step) {
            double elapsed;ASSERT_EQ(p.reference.step(&elapsed),SWMM_OK);ASSERT_EQ(p.network.step(&elapsed),SWMM_OK);
            ASSERT_NO_FATAL_FAILURE(p.same());
            ASSERT_NO_FATAL_FAILURE(target.same());
        }
        const auto before=target.network.nodes.conc;
        const auto receipts=target.issuer->receipts().receipts();const auto roundoff=target.issuer->receipts().storedRoundoff(0);
        double elapsed;ASSERT_EQ(p.reference.step(&elapsed),SWMM_OK);ASSERT_EQ(p.network.step(&elapsed),SWMM_OK);
        ASSERT_NO_FATAL_FAILURE(p.same());
        EXPECT_EQ(target.error,"Continuation acquired a new publication/refill.");
        EXPECT_EQ(target.received,2);EXPECT_EQ(target.exchanges,2);EXPECT_EQ(before,target.network.nodes.conc);
        EXPECT_EQ(receipts,target.issuer->receipts().receipts());EXPECT_EQ(roundoff,target.issuer->receipts().storedRoundoff(0));
        EXPECT_EQ(target.network.elapsed_ms+1000*target.dt,p.network.context().elapsed_ms);
        std::ofstream(p.dir/"guard.txt")<<target.error<<'\n'<<target.received<<' '<<target.exchanges<<'\n';
    }
}
void sameRecoveredPair(const MaterialPair& a,const MaterialPair& b) {
    for(auto roles:{std::pair{&a.network,&b.network},std::pair{&a.surface,&b.surface}}) {
        const auto& x=roles.first->context();const auto& y=roles.second->context();
        EXPECT_EQ(x.elapsed_ms,y.elapsed_ms);EXPECT_EQ(x.current_time,y.current_time);EXPECT_EQ(x.next_report_ms,y.next_report_ms);
#define SAME(f) EXPECT_EQ(x.nodes.f,y.nodes.f)<<#f;
        SAME(volume) SAME(old_volume) SAME(depth) SAME(old_depth) SAME(head) SAME(conc) SAME(conc_old)
        SAME(coupling_queue) SAME(coupling_volume) SAME(coupling_inflow) SAME(old_net_inflow) SAME(old_lat_flow)
#undef SAME
        EXPECT_EQ(x.coupling_delivery_remaining,y.coupling_delivery_remaining);
        EXPECT_EQ(live_recovery_test::encodeWaterBalance(x.mass_balance_2d),live_recovery_test::encodeWaterBalance(y.mass_balance_2d));
    }
    const auto& x=a.surface.surfaceRouter2D().state();const auto& y=b.surface.surfaceRouter2D().state();
    EXPECT_EQ(x.volume,y.volume);EXPECT_EQ(x.depth,y.depth);EXPECT_EQ(x.head,y.head);EXPECT_EQ(x.transport.cell_mass,y.transport.cell_mass);
    EXPECT_EQ(x.transport.lost_coupling,y.transport.lost_coupling);EXPECT_EQ(x.transport.gained_coupling,y.transport.gained_coupling);
    EXPECT_EQ(*x.node_row_conc,*y.node_row_conc);
    const auto& first=a.network.peerMaterialFunding()->receipts();const auto& second=b.network.peerMaterialFunding()->receipts();
    EXPECT_EQ(first.receipts(),second.receipts());EXPECT_EQ(first.storedRoundoff(0),second.storedRoundoff(0));
    ASSERT_EQ(a.surface.peerMaterialProducer()->legCount(),b.surface.peerMaterialProducer()->legCount());
    for(std::size_t k=0;k<a.surface.peerMaterialProducer()->legCount();++k)EXPECT_EQ(a.surface.peerMaterialProducer()->leg(k),b.surface.peerMaterialProducer()->leg(k));
}
std::string normalizePair(std::string bytes) {
    using A=detail::CompletedSourceArchive;using namespace live_recovery_test;
    A in(bytes);std::string magic,model,body;std::uint32_t version,endian,width,sum;
    in(magic,version,endian,width,model,sum,body);in.finish();
    editBlob(body,"OpenSWMM acknowledged network/surface storage owners",[](std::string& child){
        A a(child);std::string name,payload;std::uint32_t v,e,w,c;a(name,v,e,w,c,payload);a.finish();
        if(payload.size()<24)throw std::runtime_error("Missing owner identity provenance.");
        payload.replace(0,24,std::string(24,'\0'));c=crc(payload);A out;out(name,v,e,w,c,payload);child=out.bytes();
    });
    if(body.size()<16)throw std::runtime_error("Missing role identity provenance.");
    body.replace(body.size()-16,16,std::string(16,'\0'));sum=crc(body);A out;out(magic,version,endian,width,model,sum,body);return out.bytes();
}
std::string mutatePair(const std::string& bytes,const std::function<void(std::string&)>& edit) {
    using A=detail::CompletedSourceArchive;A in(bytes);std::string magic,model,body;std::uint32_t version,endian,width,sum;
    in(magic,version,endian,width,model,sum,body);in.finish();edit(body);sum=live_recovery_test::crc(body);
    A out;out(magic,version,endian,width,model,sum,body);return out.bytes();
}
void checkpointMatrixCase(bool positive,int basis,bool tail) {
    const auto tag="checkpoint-"+std::to_string(basis)+(positive?"-positive":"-negative")+(tail?"-tail":"-slices");
    MaterialPair original;ASSERT_NO_FATAL_FAILURE(original.start(tag,positive,basis,false,true,false,nullptr,tail));
    auto error=original.network.enableMaterialPairCheckpoint();ASSERT_TRUE(error.empty())<<error;
    const int last=tail?80:8;std::vector<std::string> states(last+1);
    const std::vector<int> captures=tail?std::vector<int>{0,1,2,3,4,9,16,17}:std::vector<int>{0,1,2,3,4,5,6,7,8};
    std::ofstream refused(original.dir/"save-refusals.txt");
    for(int step=0;step<=last;++step) {
        if(step) {
            double elapsed;ASSERT_EQ(original.reference.step(&elapsed),SWMM_OK);
            ASSERT_EQ(original.network.step(&elapsed),SWMM_OK)<<original.network.context().error_message;
            ASSERT_NO_FATAL_FAILURE(original.same());
        }
        std::string saved="unchanged";error=original.network.materialPairCheckpointBytes(saved);
        if(tail && step>=18) {
            EXPECT_EQ(saved,"unchanged");EXPECT_NE(error.find("staged archive profile"),std::string::npos)<<error;
            refused<<step<<' '<<error<<'\n';
        } else {
            ASSERT_TRUE(error.empty())<<step<<' '<<error;states[step]=std::move(saved);
            std::ofstream(original.dir/("native-"+std::to_string(step)+".checkpoint"),std::ios::binary)<<states[step];
        }
    }
    for(int capture:captures) {
        SCOPED_TRACE(tag+"/"+std::to_string(capture));
        MaterialPair restored;ASSERT_NO_FATAL_FAILURE(restored.start(tag+"-restored-"+std::to_string(capture),positive,basis,false,true,false,nullptr,tail,original.input));
        error=restored.network.enableMaterialPairCheckpoint();ASSERT_TRUE(error.empty())<<error;
        error=restored.network.restoreMaterialPairCheckpoint(states[capture]);ASSERT_TRUE(error.empty())<<error;
        for(int step=capture;step<=last;++step) {
            if(step>capture) {double elapsed;ASSERT_EQ(restored.network.step(&elapsed),SWMM_OK)<<step<<' '<<restored.network.context().error_message;}
            std::string actual="unchanged";error=restored.network.materialPairCheckpointBytes(actual);
            if(states[step].empty()) {EXPECT_EQ(actual,"unchanged");EXPECT_NE(error.find("staged archive profile"),std::string::npos)<<error;}
            else {
                ASSERT_TRUE(error.empty())<<step<<' '<<error;EXPECT_EQ(normalizePair(actual),normalizePair(states[step]))<<step;
                if(step==last)std::ofstream(restored.dir/"final.checkpoint",std::ios::binary)<<actual;
            }
        }
        ASSERT_NO_FATAL_FAILURE(sameRecoveredPair(original,restored));
        if(tail) {
            double elapsed=1;ASSERT_EQ(restored.network.step(&elapsed),SWMM_OK);EXPECT_EQ(elapsed,0);
            EXPECT_FALSE(restored.network.materialPairCheckpointBytes(states[0]).empty());
            ASSERT_EQ(restored.network.end(),SWMM_OK);EXPECT_EQ(restored.finishes,1);
            std::ofstream(restored.dir/"completion.txt")<<restored.network.context().elapsed_ms<<' '<<restored.finishes<<'\n';
        }
    }
    original.phases.flush();
    MaterialPair ordinary;ASSERT_NO_FATAL_FAILURE(ordinary.start(tag+"-ordinary",positive,basis,false,true,false,nullptr,tail,original.input));
    ASSERT_NO_FATAL_FAILURE(ordinary.steps(last));ordinary.phases.flush();
    EXPECT_EQ(read(ordinary.dir/"phases.csv"),read(original.dir/"phases.csv"));
    ASSERT_NO_FATAL_FAILURE(sameRecoveredPair(original,ordinary));
}
TEST(MaterialPairCheckpoint, CompleteSnapshotsAndRefillAllBasesBothSigns) {
    for(int basis=0;basis<3;++basis)for(bool positive:{false,true}) {
        SCOPED_TRACE(std::to_string(basis)+"/"+std::to_string(positive));
        ASSERT_NO_FATAL_FAILURE(checkpointMatrixCase(positive,basis,false));
    }
}
TEST(MaterialPairCheckpoint, PendingTailsEmptyWindowsFullJournalAndCompletion) {
    for(bool positive:{false,true})ASSERT_NO_FATAL_FAILURE(checkpointMatrixCase(positive,0,true));
}
TEST(MaterialPairCheckpoint, DuplicateAndLateAttachmentPreserveExistingState) {
    MaterialPair original;ASSERT_NO_FATAL_FAILURE(original.start("checkpoint-attachment",true));
    ASSERT_TRUE(original.network.enableMaterialPairCheckpoint().empty());std::string before,after;
    ASSERT_TRUE(original.network.materialPairCheckpointBytes(before).empty());
    EXPECT_FALSE(original.network.enableMaterialPairCheckpoint().empty());EXPECT_FALSE(original.surface.enableMaterialPairCheckpoint().empty());
    ASSERT_TRUE(original.network.materialPairCheckpointBytes(after).empty());EXPECT_EQ(before,after);
    ASSERT_NO_FATAL_FAILURE(original.steps(6));ASSERT_TRUE(original.network.materialPairCheckpointBytes(after).empty());
    MaterialPair ordinary;ASSERT_NO_FATAL_FAILURE(ordinary.start("checkpoint-late",true));
    ASSERT_NO_FATAL_FAILURE(ordinary.steps(5));EXPECT_FALSE(ordinary.network.enableMaterialPairCheckpoint().empty());
    for(int k=0;k<3;++k){double elapsed;ASSERT_EQ(ordinary.reference.step(&elapsed),SWMM_OK);ASSERT_EQ(ordinary.network.step(&elapsed),SWMM_OK);}
    ASSERT_NO_FATAL_FAILURE(ordinary.same());
}
TEST(MaterialPairCheckpoint, ChecksumValidSecondRoleDamageIsAtomicAndRetryable) {
    MaterialPair original;ASSERT_NO_FATAL_FAILURE(original.start("checkpoint-corruption",true));
    ASSERT_TRUE(original.network.enableMaterialPairCheckpoint().empty());ASSERT_NO_FATAL_FAILURE(original.steps(6));
    std::string saved;ASSERT_TRUE(original.network.materialPairCheckpointBytes(saved).empty());
    for(int edit=0;edit<9;++edit) {
        SCOPED_TRACE(edit);MaterialPair target;
        ASSERT_NO_FATAL_FAILURE(target.start("checkpoint-corruption-"+std::to_string(edit),true,0,false,true,false,nullptr,false,original.input));
        ASSERT_TRUE(target.network.enableMaterialPairCheckpoint().empty());std::string before,after;
        ASSERT_TRUE(target.network.materialPairCheckpointBytes(before).empty());
        auto bad=mutatePair(saved,[&](std::string& body){
            using namespace live_recovery_test;
            if(edit<=2)editBlob(body,"OpenSWMM split material surface state",[&](std::string& mesh){
                if(edit==0)meshVector(mesh,"cell_mass",[](char* p,std::size_t n){addFirstDouble(p,n,1);},"native material pair mesh v1");
                if(edit==1)meshVector(mesh,"volume",[](char* p,std::size_t n){addFirstDouble(p,n,.125);},"native material pair mesh v1");
                if(edit==2){double c=original.surface.context().nodes.conc_old[0];detail::CompletedSourceArchive a;a(c);
                    const auto at=mesh.size()-sizeof(double);if(mesh.substr(at)!=a.bytes())throw std::runtime_error("Missing publisher test tail.");
                    c=std::nextafter(c,INFINITY);detail::CompletedSourceArchive b;b(c);mesh.replace(at,sizeof(double),b.bytes());}
            });
            if(edit==3 || edit==4) {
                auto mb=original.surface.context().mass_balance_2d;const auto bytes=encodeWaterBalance(mb);
                if(edit==3){mb.init_storage+=1;mb.final_storage+=1;}else mb.coupling_1d_to_2d_in+=1;
                const auto changed=encodeWaterBalance(mb);auto at=body.find(bytes);if(at==std::string::npos)throw std::runtime_error("Missing test balance.");
                do{body.replace(at,bytes.size(),changed);at=body.find(bytes,at+changed.size());}while(at!=std::string::npos);
                if(edit==3)editBlob(body,"OpenSWMM split material surface state",[&](std::string& mesh){
                    const double dz=1/original.surface.surfaceRouter2D().mesh().tri_area[0];
                    meshVector(mesh,"volume",[](char* p,std::size_t n){addFirstDouble(p,n,1);},"native material pair mesh v1");
                    for(const auto* column:{"head","depth"})meshVector(mesh,column,[&](char* p,std::size_t n){addFirstDouble(p,n,dz);},"native material pair mesh v1");
                });
            }
            if(edit==5)body.replace(8,7,"surface");
            if(edit==6)body.replace(body.size()-16,16,std::string(16,'\0'));
            auto change_role=[](std::string& bytes,std::size_t at) {
                detail::CompletedSourceArchive in(bytes.substr(at,16));std::uint64_t first,second;in(first,second);in.finish();
                first=first!=1 && second!=1?1:(first!=2 && second!=2?2:3);
                detail::CompletedSourceArchive out;out(first,second);bytes.replace(at,16,out.bytes());
            };
            if(edit==7)change_role(body,body.size()-16);
            if(edit==8)editBlob(body,"OpenSWMM acknowledged network/surface storage owners",[&](std::string& child){
                detail::CompletedSourceArchive in(child);std::string name,payload;std::uint32_t version,endian,width,sum;
                in(name,version,endian,width,sum,payload);in.finish();change_role(payload,0);sum=crc(payload);
                detail::CompletedSourceArchive out;out(name,version,endian,width,sum,payload);child=out.bytes();
            });
        });
        const auto error=target.network.restoreMaterialPairCheckpoint(bad);EXPECT_FALSE(error.empty());
        std::ofstream(target.dir/"refusal.txt")<<error<<'\n';
        ASSERT_TRUE(target.network.materialPairCheckpointBytes(after).empty());EXPECT_EQ(before,after);
        ASSERT_TRUE(target.network.restoreMaterialPairCheckpoint(saved).empty());ASSERT_NO_FATAL_FAILURE(sameRecoveredPair(original,target));
    }
}
TEST(MaterialPairCheckpoint, RestoredSiblingOuterFrameRefusesBeforeSurfaceEffects) {
    MaterialPair original;ASSERT_NO_FATAL_FAILURE(original.start("checkpoint-sibling-source",true));
    ASSERT_TRUE(original.network.enableMaterialPairCheckpoint().empty());ASSERT_NO_FATAL_FAILURE(original.steps(6));
    std::string saved;ASSERT_TRUE(original.network.materialPairCheckpointBytes(saved).empty());
    MaterialPair a,b;
    ASSERT_NO_FATAL_FAILURE(a.start("checkpoint-sibling-a",true,0,false,true,false,nullptr,false,original.input));
    ASSERT_NO_FATAL_FAILURE(b.start("checkpoint-sibling-b",true,0,false,true,false,nullptr,false,original.input));
    ASSERT_TRUE(a.network.enableMaterialPairCheckpoint().empty());ASSERT_TRUE(b.network.enableMaterialPairCheckpoint().empty());
    ASSERT_TRUE(a.network.restoreMaterialPairCheckpoint(saved).empty());ASSERT_TRUE(b.network.restoreMaterialPairCheckpoint(saved).empty());
    std::unique_ptr<SplitFrame> stale;
    a.request_observer=[&](const SplitFrame& r){if(r.phase==SWMM_SPLIT_CONDUCTANCE && !stale)stale=std::make_unique<SplitFrame>(r);};
    double elapsed;ASSERT_EQ(a.network.step(&elapsed),SWMM_OK);ASSERT_NE(stale,nullptr);
    const auto water=b.surface.surfaceRouter2D().state().volume,mass=b.surface.surfaceRouter2D().state().transport.cell_mass;
    const auto time=b.surface.context().elapsed_ms;int refusals=0;
    b.request_observer=[&](const SplitFrame& current){
        if(current.phase!=SWMM_SPLIT_CONDUCTANCE || refusals)return;
        EXPECT_EQ(current.sequence,stale->sequence);EXPECT_EQ(current.start,stale->start);EXPECT_EQ(current.dt,stale->dt);
        EXPECT_NE(current.sender,stale->sender);EXPECT_NE(current.target,stale->target);
        SWMM_SplitFrame output=nullptr;EXPECT_NE(b.surface.evaluateSplit(current.phase,stale.get(),&output),SWMM_OK);EXPECT_EQ(output,nullptr);++refusals;
    };
    EXPECT_NE(b.network.step(&elapsed),SWMM_OK);EXPECT_EQ(refusals,1);
    EXPECT_EQ(water,b.surface.surfaceRouter2D().state().volume);EXPECT_EQ(mass,b.surface.surfaceRouter2D().state().transport.cell_mass);
    EXPECT_EQ(time,b.surface.context().elapsed_ms);EXPECT_EQ(b.surface.peerMaterialProducer()->legCount(),8u);
}
TEST(MaterialPairCheckpoint, FreshProcessNumericalRestoreChild) {
    const char* directory=std::getenv("OPENSWMM_PAIR_RESTORE_CASE");if(!directory)GTEST_SKIP()<<"Run by fresh executable driver.";
    const char* basis_text=std::getenv("OPENSWMM_PAIR_RESTORE_BASIS");const char* sign=std::getenv("OPENSWMM_PAIR_RESTORE_SIGN");
    const char* capture_text=std::getenv("OPENSWMM_PAIR_RESTORE_CAPTURE");ASSERT_NE(basis_text,nullptr);ASSERT_NE(sign,nullptr);ASSERT_NE(capture_text,nullptr);
    const int basis=std::stoi(basis_text),capture=std::stoi(capture_text);const bool positive=std::string(sign)=="positive";
    ASSERT_GE(capture,0);ASSERT_LE(capture,8);ASSERT_GE(basis,0);ASSERT_LT(basis,3);
    const fs::path parent(directory);MaterialPair target;
    ASSERT_NO_FATAL_FAILURE(target.start("fresh",positive,basis,false,true,false,nullptr,false,parent/"model.inp"));
    ASSERT_TRUE(target.network.enableMaterialPairCheckpoint().empty());
    EXPECT_EQ(target.network.context().elapsed_ms,0);EXPECT_EQ(target.surface.context().elapsed_ms,0);
    EXPECT_EQ(target.advances,0);EXPECT_EQ(target.accepted,0);EXPECT_EQ(target.surface.peerMaterialProducer()->legCount(),0u);
    const auto bytes=read(parent/("native-"+std::to_string(capture)+".checkpoint"));ASSERT_FALSE(bytes.empty());
    const auto error=target.network.restoreMaterialPairCheckpoint(bytes);ASSERT_TRUE(error.empty())<<error;
    // No prefix solver execution: targets were at time zero immediately before restore.
    EXPECT_EQ(target.accepted,0);const auto id=target.network.peerMaterialFunding()->identity();
    std::ofstream(target.dir/"identity.txt")<<id.issuer<<' '<<id.recipient<<' '<<id.session<<'\n';
    for(int step=capture;step<=8;++step) {
        if(step>capture){double elapsed;ASSERT_EQ(target.network.step(&elapsed),SWMM_OK)<<target.network.context().error_message;}
        std::string actual;ASSERT_TRUE(target.network.materialPairCheckpointBytes(actual).empty());
        const auto expected=read(parent/("native-"+std::to_string(step)+".checkpoint"));
        EXPECT_EQ(normalizePair(actual),normalizePair(expected))<<step;
        if(step==8)std::ofstream(target.dir/"final.checkpoint",std::ios::binary)<<actual;
    }
    EXPECT_EQ(target.accepted,8-capture);
}
TEST(MaterialPairCheckpoint, PartialRestoreContinuesThroughActualRefillAndAcknowledgement) {
    MaterialPair original;ASSERT_NO_FATAL_FAILURE(original.start("pair-original",true));
    auto error=original.network.enableMaterialPairCheckpoint();ASSERT_TRUE(error.empty())<<error;
    ASSERT_NO_FATAL_FAILURE(original.steps(5));std::string bytes;
    error=original.network.materialPairCheckpointBytes(bytes);ASSERT_TRUE(error.empty())<<error;
    std::ofstream(original.dir/"pair.checkpoint",std::ios::binary)<<bytes;
    MaterialPair restored;ASSERT_NO_FATAL_FAILURE(restored.start("pair-restored",true,0,false,true,false,nullptr,false,original.input));
    error=restored.network.enableMaterialPairCheckpoint();ASSERT_TRUE(error.empty())<<error;
    error=restored.network.restoreMaterialPairCheckpoint(bytes);ASSERT_TRUE(error.empty())<<error;
    ASSERT_NO_FATAL_FAILURE(sameRecoveredPair(original,restored));
    EXPECT_NE(original.network.peerMaterialFunding()->identity().session,restored.network.peerMaterialFunding()->identity().session);
    for(int step=6;step<=8;++step) {
        SCOPED_TRACE(step);
        double elapsed;ASSERT_EQ(original.reference.step(&elapsed),SWMM_OK);
        ASSERT_EQ(original.network.step(&elapsed),SWMM_OK)<<original.network.context().error_message;
        ASSERT_EQ(restored.network.step(&elapsed),SWMM_OK)<<restored.network.context().error_message;
        ASSERT_NO_FATAL_FAILURE(original.same());
        ASSERT_NO_FATAL_FAILURE(sameRecoveredPair(original,restored));
        std::string saved;error=restored.network.materialPairCheckpointBytes(saved);ASSERT_TRUE(error.empty())<<error;
        std::ofstream(restored.dir/("pair-"+std::to_string(step)+".checkpoint"),std::ios::binary)<<saved;
    }
    ASSERT_EQ(restored.network.peerMaterialFunding()->receipts().receipts().size(),2u);
    EXPECT_FALSE(restored.network.peerMaterialFunding()->unresolved());
    EXPECT_FALSE(restored.surface.peerMaterialProducer()->awaitingAcknowledgement());
}
#ifdef MATERIAL_OUTPUT_CHECKPOINT_TESTS
std::string outputNumerics(std::string bytes) {
    bytes=mutatePair(bytes,[](std::string& body){
        live_recovery_test::editBlob(body,"OpenSWMM material pair output prefixes v1",[](std::string& output){
            detail::CompletedSourceArchive in(output);std::string label,binary,mesh;in(label,binary,mesh);in.finish();
            binary.clear();mesh.clear();detail::CompletedSourceArchive out;out(label,binary,mesh);output=out.bytes();
        });
    });
    return normalizePair(std::move(bytes));
}
void outputRestartCase(bool positive,int basis,int policy) {
    const auto tag="output-"+std::to_string(basis)+(positive?"-positive":"-negative")+"-"+std::to_string(policy);
    const int stride=policy==1?2:1;const bool delayed=policy==2;
    MaterialPair original;ASSERT_NO_FATAL_FAILURE(original.start(tag,positive,basis,true,true,false,nullptr,false,{},true,stride,delayed));
    auto error=original.network.enableMaterialPairCheckpoint(true);ASSERT_TRUE(error.empty())<<error;
    std::vector<std::string> states(13);
    std::ofstream refused(original.dir/"save-refusals.txt");
    for(int step=0;step<=12;++step) {
        if(step) {double elapsed;ASSERT_EQ(original.reference.step(&elapsed),SWMM_OK)<<step<<' '<<original.reference.context().error_message;ASSERT_EQ(original.network.step(&elapsed),SWMM_OK)<<original.network.context().error_message;ASSERT_NO_FATAL_FAILURE(original.same());}
        std::string saved="unchanged";error=original.network.materialPairCheckpointBytes(saved);
        if(!error.empty()){ASSERT_GT(step,8);EXPECT_EQ(saved,"unchanged");ASSERT_NE(error.find("staged archive profile"),std::string::npos)<<error;refused<<step<<' '<<error<<'\n';}
        else{states[step]=std::move(saved);std::ofstream(original.dir/("native-"+std::to_string(step)+".checkpoint"),std::ios::binary)<<states[step];}
    }
    std::vector<fs::path> restored_paths;
    for(int capture:{0,1,2,4,5,6,7,8}) {
        SCOPED_TRACE(tag+"/"+std::to_string(capture));MaterialPair restored;
        ASSERT_NO_FATAL_FAILURE(restored.start(tag+"-restored-"+std::to_string(capture),positive,basis,true,true,false,nullptr,false,original.input,true,stride,delayed));
        error=restored.network.enableMaterialPairCheckpoint(true);ASSERT_TRUE(error.empty())<<error;
        error=restored.network.restoreMaterialPairCheckpoint(states[capture]);ASSERT_TRUE(error.empty())<<error;
        for(int step=capture;step<=12;++step) {
            if(step>capture){double elapsed;ASSERT_EQ(restored.network.step(&elapsed),SWMM_OK)<<restored.network.context().error_message;}
            std::string actual="unchanged";error=restored.network.materialPairCheckpointBytes(actual);
            if(states[step].empty()){EXPECT_EQ(actual,"unchanged");EXPECT_NE(error.find("staged archive profile"),std::string::npos)<<error;}
            else{ASSERT_TRUE(error.empty())<<step<<' '<<error;EXPECT_EQ(outputNumerics(actual),outputNumerics(states[step]))<<step;}
        }
        ASSERT_NO_FATAL_FAILURE(sameRecoveredPair(original,restored));
        ASSERT_EQ(restored.network.end(),SWMM_OK);ASSERT_EQ(restored.network.close(),SWMM_OK);ASSERT_EQ(restored.surface.close(),SWMM_OK);
        restored_paths.push_back(restored.dir);
    }
    ASSERT_EQ(original.reference.end(),SWMM_OK);ASSERT_EQ(original.network.end(),SWMM_OK);
    ASSERT_NO_FATAL_FAILURE(original.same(true));ASSERT_EQ(original.network.close(),SWMM_OK);ASSERT_EQ(original.surface.close(),SWMM_OK);ASSERT_EQ(original.reference.close(),SWMM_OK);
    ASSERT_EQ(read(original.dir/"native.out"),read(original.dir/"network.out"));
    for(const auto& path:restored_paths)EXPECT_EQ(read(path/"network.out"),read(original.dir/"network.out"));
    std::ofstream(original.dir/"completion.txt")<<"12 native steps and end\n";
}
TEST(MaterialPairOutputCheckpoint, PollutantReportsAllBasesBothSignsBeforeAndAfterReports) {
    for(int basis=0;basis<3;++basis)for(bool positive:{false,true})ASSERT_NO_FATAL_FAILURE(outputRestartCase(positive,basis,0));
}
TEST(MaterialPairOutputCheckpoint, IndependentMeshCadenceAndReportStart) {
    for(int policy:{1,2})for(bool positive:{false,true})ASSERT_NO_FATAL_FAILURE(outputRestartCase(positive,0,policy));
}
TEST(MaterialPairOutputCheckpoint, MissingSpeciesSelectionRefusesBeforeAttachmentAndLeavesWritersUsable) {
    int index=0;
    for(const auto& selection:{std::string("REPORT_2D_SPECIES Missing\n"),std::string("REPORT_2D_VARIABLES DEPTH\n")}) {
        MaterialPair pair;ASSERT_NO_FATAL_FAILURE(pair.start("output-selection-"+std::to_string(index++),true,0,true,true,false,nullptr,false,{},true,1,false,selection));
        const auto binary=read(pair.dir/"network.out"),mesh=read(pair.dir/"surface.h5");
        for(int repeat=0;repeat<2;++repeat){const auto error=pair.network.enableMaterialPairCheckpoint(true);EXPECT_FALSE(error.empty());std::ofstream(pair.dir/("refusal-"+std::to_string(repeat)+".txt"))<<error<<'\n';}
        EXPECT_EQ(pair.network.context().elapsed_ms,0);EXPECT_EQ(pair.surface.context().elapsed_ms,0);
        EXPECT_EQ(binary,read(pair.dir/"network.out"));EXPECT_EQ(mesh,read(pair.dir/"surface.h5"));
        ASSERT_NO_FATAL_FAILURE(pair.steps(8));
    }
}
TEST(MaterialPairOutputCheckpoint, EitherPrefixAndPublisherDamagePreserveBothTargetsAndPermitRetry) {
    MaterialPair original;ASSERT_NO_FATAL_FAILURE(original.start("output-damage-source",true,0,true,true,false,nullptr,false,{},true));
    ASSERT_TRUE(original.network.enableMaterialPairCheckpoint(true).empty());std::string early,saved;
    ASSERT_NO_FATAL_FAILURE(original.steps(1));ASSERT_EQ(original.surface.context().nodes.conc,original.surface.context().nodes.conc_old);
    ASSERT_TRUE(original.network.materialPairCheckpointBytes(early).empty());
    for(int step=2;step<=6;++step){double elapsed;ASSERT_EQ(original.reference.step(&elapsed),SWMM_OK);ASSERT_EQ(original.network.step(&elapsed),SWMM_OK);}
    ASSERT_TRUE(original.network.materialPairCheckpointBytes(saved).empty());
    for(int edit=0;edit<4;++edit) {
        MaterialPair target;ASSERT_NO_FATAL_FAILURE(target.start("output-damage-"+std::to_string(edit),true,0,true,true,false,nullptr,false,original.input,true));
        ASSERT_TRUE(target.network.enableMaterialPairCheckpoint(true).empty());std::string before,after;
        ASSERT_TRUE(target.network.materialPairCheckpointBytes(before).empty());
        const auto binary_header=read(target.dir/"network.out"),mesh_header=read(target.dir/"surface.h5");
        auto bad=mutatePair(edit==3?early:saved,[&](std::string& body){
            if(edit==3)live_recovery_test::editBlob(body,"OpenSWMM split material surface state",[&](std::string& mesh){
                detail::CompletedSourceArchive out;auto time=original.network.context().options.routing_step*1000;out(time);mesh.replace(mesh.size()-sizeof(double),sizeof(double),out.bytes());
            });
            else live_recovery_test::editBlob(body,"OpenSWMM material pair output prefixes v1",[&](std::string& output){
                detail::CompletedSourceArchive in(output);std::string label,binary,mesh;in(label,binary,mesh);in.finish();
                if(edit==0)binary[0]^=1;
                if(edit==1) {const auto at=mesh.find(std::string("\211HDF\r\n\032\n",8));if(at==std::string::npos)throw std::runtime_error("Missing HDF5 test prefix");mesh[at]=0;}
                if(edit==2)std::swap(binary,mesh);
                detail::CompletedSourceArchive out;out(label,binary,mesh);output=out.bytes();
            });
        });
        auto error=target.network.restoreMaterialPairCheckpoint(bad);EXPECT_FALSE(error.empty());
        std::ofstream(target.dir/"refusal.txt")<<error<<'\n';ASSERT_TRUE(target.network.materialPairCheckpointBytes(after).empty());
        EXPECT_EQ(before,after);EXPECT_EQ(binary_header,read(target.dir/"network.out"));EXPECT_EQ(mesh_header,read(target.dir/"surface.h5"));
        error=target.network.restoreMaterialPairCheckpoint(saved);ASSERT_TRUE(error.empty())<<error;
        double elapsed;ASSERT_EQ(target.network.step(&elapsed),SWMM_OK);
    }
}
TEST(MaterialPairOutputCheckpoint, SecondRenameFailureRollsBackFreshWritersAndAllowsPairRestore) {
    MaterialPair original;ASSERT_NO_FATAL_FAILURE(original.start("output-publish-source",false,0,true,true,false,nullptr,false,{},true));
    ASSERT_TRUE(original.network.enableMaterialPairCheckpoint(true).empty());ASSERT_NO_FATAL_FAILURE(original.steps(6));std::string saved;
    ASSERT_TRUE(original.network.materialPairCheckpointBytes(saved).empty());
    MaterialPair target;ASSERT_NO_FATAL_FAILURE(target.start("output-publish-target",false,0,true,true,false,nullptr,false,original.input,true));
    ASSERT_TRUE(target.network.enableMaterialPairCheckpoint(true).empty());std::string before,after;
    ASSERT_TRUE(target.network.materialPairCheckpointBytes(before).empty());
    const auto binary=target.dir/"network.out",mesh=target.dir/"surface.h5";
    const auto bh=read(binary),mh=read(mesh);const auto a=target.dir/"first-stage",b=target.dir/"second-stage";
    std::ofstream(a)<<"first published replacement";std::ofstream(b)<<"second published replacement";
    {
        io::CheckpointOutputPublication publication({{a,binary,bh,-1},{b,mesh,mh,-1}});
        fs::remove(b); // Constructor succeeded; first rename succeeds, second fails.
        try{publication.publish();FAIL()<<"Missing second staging file was published";}
        catch(const std::exception& error){EXPECT_NE(std::string(error.what()).find("fresh destinations were restored"),std::string::npos);std::ofstream(target.dir/"rollback.txt")<<error.what()<<'\n';}
    }
    EXPECT_FALSE(fs::exists(a));EXPECT_EQ(bh,read(binary));EXPECT_EQ(mh,read(mesh));
    ASSERT_TRUE(target.network.materialPairCheckpointBytes(after).empty());EXPECT_EQ(before,after);
    const auto error=target.network.restoreMaterialPairCheckpoint(saved);ASSERT_TRUE(error.empty())<<error;
    double elapsed;ASSERT_EQ(target.network.step(&elapsed),SWMM_OK);
}
TEST(MaterialPairOutputCheckpoint, FreshProcessOutputRestoreChild) {
    const char* directory=std::getenv("OPENSWMM_PAIR_OUTPUT_CASE");if(!directory)GTEST_SKIP()<<"Run by fresh executable output driver.";
    const char* basis_text=std::getenv("OPENSWMM_PAIR_RESTORE_BASIS");const char* sign=std::getenv("OPENSWMM_PAIR_RESTORE_SIGN");
    const char* capture_text=std::getenv("OPENSWMM_PAIR_RESTORE_CAPTURE");ASSERT_NE(basis_text,nullptr);ASSERT_NE(sign,nullptr);ASSERT_NE(capture_text,nullptr);
    const int basis=std::stoi(basis_text),capture=std::stoi(capture_text);const bool positive=std::string(sign)=="positive";
    ASSERT_GE(basis,0);ASSERT_LT(basis,3);ASSERT_TRUE(capture==5 || capture==6);
    const fs::path parent(directory);MaterialPair target;
    ASSERT_NO_FATAL_FAILURE(target.start("fresh",positive,basis,true,true,false,nullptr,false,parent/"model.inp",true));
    ASSERT_TRUE(target.network.enableMaterialPairCheckpoint(true).empty());
    ASSERT_EQ(target.network.context().elapsed_ms,0);ASSERT_EQ(target.surface.context().elapsed_ms,0);
    ASSERT_EQ(target.accepted,0);ASSERT_EQ(target.advances,0);ASSERT_EQ(target.surface.peerMaterialProducer()->legCount(),0u);
    auto error=target.network.restoreMaterialPairCheckpoint(read(parent/("native-"+std::to_string(capture)+".checkpoint")));ASSERT_TRUE(error.empty())<<error;
    const auto id=target.network.peerMaterialFunding()->identity();std::ofstream(target.dir/"identity.txt")<<id.issuer<<' '<<id.recipient<<' '<<id.session<<'\n';
    for(int step=capture;step<=12;++step) {
        if(step>capture){double elapsed;ASSERT_EQ(target.network.step(&elapsed),SWMM_OK)<<target.network.context().error_message;}
        std::string actual="unchanged";error=target.network.materialPairCheckpointBytes(actual);
        const auto file=parent/("native-"+std::to_string(step)+".checkpoint");
        if(fs::exists(file)){ASSERT_TRUE(error.empty())<<error;EXPECT_EQ(outputNumerics(actual),outputNumerics(read(file)))<<step;}
        else{EXPECT_EQ(actual,"unchanged");EXPECT_NE(error.find("staged archive profile"),std::string::npos)<<error;}
    }
    EXPECT_EQ(target.accepted,12-capture);ASSERT_EQ(target.network.end(),SWMM_OK);
    ASSERT_EQ(target.network.close(),SWMM_OK);ASSERT_EQ(target.surface.close(),SWMM_OK);
    EXPECT_EQ(read(target.dir/"network.out"),read(parent/"network.out"));
}
#endif
#ifdef MATERIAL_PACKET_TESTS
struct PublicCheckpoint {
    SWMM_MaterialCheckpoint value=nullptr;
    ~PublicCheckpoint(){swmm_split_material_checkpoint_destroy(value);}
};
std::string publicCheckpointBytes(SWMM_MaterialCheckpoint value) {
    const void* data=nullptr;std::size_t size=0;
    if(swmm_split_material_checkpoint_data(value,&data,&size)!=SWMM_OK)throw std::runtime_error(swmm_split_material_checkpoint_last_error());
    return {static_cast<const char*>(data),size};
}
TEST(MaterialPublicRecovery, BytesMatchNativeAndSurviveSourceCloseAndCallerBufferDestruction) {
    for(bool outputs:{false,true}) {
#ifndef MATERIAL_OUTPUT_CHECKPOINT_TESTS
        if(outputs)continue;
#endif
        MaterialPair original;ASSERT_NO_FATAL_FAILURE(original.start(outputs?"public-value-output":"public-value-off",true,0,outputs,true,false,nullptr,false,{},outputs));
        ASSERT_EQ(swmm_split_material_checkpoint_enable(&original.network,outputs),SWMM_OK);ASSERT_NO_FATAL_FAILURE(original.steps(6));
        PublicCheckpoint state,imported;ASSERT_EQ(swmm_split_material_checkpoint_capture(&original.network,&state.value),SWMM_OK);
        const auto bytes=publicCheckpointBytes(state.value);std::string native;
        ASSERT_TRUE(original.network.materialPairCheckpointBytes(native).empty());EXPECT_EQ(bytes,native);
        auto buffer=bytes;ASSERT_EQ(swmm_split_material_checkpoint_from_bytes(buffer.data(),buffer.size(),&imported.value),SWMM_OK);
        buffer.assign(buffer.size(),'\0');buffer.clear();buffer.shrink_to_fit();EXPECT_EQ(publicCheckpointBytes(imported.value),bytes);
        ASSERT_EQ(original.network.end(),SWMM_OK);ASSERT_EQ(original.network.close(),SWMM_OK);ASSERT_EQ(original.surface.close(),SWMM_OK);
        EXPECT_EQ(publicCheckpointBytes(state.value),bytes);EXPECT_EQ(publicCheckpointBytes(imported.value),bytes);
        const void* unchanged=reinterpret_cast<void*>(1);std::size_t size=789;
        EXPECT_EQ(swmm_split_material_checkpoint_data(state.value,nullptr,&size),SWMM_ERR_BADPARAM);EXPECT_EQ(size,789u);
        EXPECT_EQ(swmm_split_material_checkpoint_data(state.value,&unchanged,nullptr),SWMM_ERR_BADPARAM);EXPECT_EQ(unchanged,reinterpret_cast<void*>(1));
    }
}
TEST(MaterialPublicRecovery, OuterEnvelopeAndOutputModeRefuseWithoutChangingFreshPair) {
    MaterialPair original;ASSERT_NO_FATAL_FAILURE(original.start("public-envelope-source",true));
    ASSERT_EQ(swmm_split_material_checkpoint_enable(&original.network,0),SWMM_OK);ASSERT_NO_FATAL_FAILURE(original.steps(6));
    PublicCheckpoint state;ASSERT_EQ(swmm_split_material_checkpoint_capture(&original.network,&state.value),SWMM_OK);const auto bytes=publicCheckpointBytes(state.value);
    for(int edit=0;edit<6;++edit) {
        detail::CompletedSourceArchive in(bytes);std::string magic,model,body;std::uint32_t version,endian,width,sum;in(magic,version,endian,width,model,sum,body);in.finish();
        if(edit==0)magic[0]^=1;if(edit==1)version=99;if(edit==2)endian=0;if(edit==3)width=0;if(edit==4)++sum;if(edit==5)version=2;
        detail::CompletedSourceArchive out;out(magic,version,endian,width,model,sum,body);const auto damaged=out.bytes();
        PublicCheckpoint value;value.value=nullptr;
        const auto rc=swmm_split_material_checkpoint_from_bytes(damaged.data(),damaged.size(),&value.value);
        if(edit<5){EXPECT_EQ(rc,SWMM_ERR_BADPARAM);EXPECT_EQ(value.value,nullptr);continue;}
        ASSERT_EQ(rc,SWMM_OK);MaterialPair target;ASSERT_NO_FATAL_FAILURE(target.start("public-mode-target",true,0,false,true,false,nullptr,false,original.input));
        ASSERT_EQ(swmm_split_material_checkpoint_enable(&target.network,0),SWMM_OK);PublicCheckpoint before,after;
        ASSERT_EQ(swmm_split_material_checkpoint_capture(&target.network,&before.value),SWMM_OK);
        target.network.context().error_message="original diagnostic";
        EXPECT_EQ(swmm_split_material_checkpoint_restore(&target.network,value.value),SWMM_ERR_IO);
        EXPECT_EQ(target.network.context().error_message,"original diagnostic");ASSERT_EQ(swmm_split_material_checkpoint_capture(&target.network,&after.value),SWMM_OK);
        EXPECT_EQ(publicCheckpointBytes(before.value),publicCheckpointBytes(after.value));
        ASSERT_EQ(swmm_split_material_checkpoint_restore(&target.network,state.value),SWMM_OK);ASSERT_NO_FATAL_FAILURE(sameRecoveredPair(original,target));
    }
}
TEST(MaterialPublicRecovery, ExpectedClockIsJoinedBeforeCollectivePublicationAndAllowsRetry) {
    for(bool outputs:{false,true}) {
#ifndef MATERIAL_OUTPUT_CHECKPOINT_TESTS
        if(outputs)continue;
#endif
        MaterialPair original;ASSERT_NO_FATAL_FAILURE(original.start(outputs?"public-clock-source-on":"public-clock-source-off",true,0,outputs,true,false,nullptr,false,{},outputs));
        ASSERT_EQ(swmm_split_material_checkpoint_enable(&original.network,outputs),SWMM_OK);ASSERT_NO_FATAL_FAILURE(original.steps(6));
        PublicCheckpoint state;ASSERT_EQ(swmm_split_material_checkpoint_capture(&original.network,&state.value),SWMM_OK);
        MaterialPair target;ASSERT_NO_FATAL_FAILURE(target.start(outputs?"public-clock-target-on":"public-clock-target-off",true,0,outputs,true,false,nullptr,false,original.input,outputs));
        ASSERT_EQ(swmm_split_material_checkpoint_enable(&target.network,outputs),SWMM_OK);
        PublicCheckpoint before,after;ASSERT_EQ(swmm_split_material_checkpoint_capture(&target.network,&before.value),SWMM_OK);
        const double seconds=original.network.context().elapsed_ms/1000.0;
        for(double bad:{-1.0,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})
            EXPECT_EQ(swmm_split_material_checkpoint_restore_at(&target.network,state.value,bad),SWMM_ERR_BADPARAM);
        EXPECT_EQ(swmm_split_material_checkpoint_restore_at(&target.network,state.value,std::nextafter(seconds,INFINITY)),SWMM_ERR_IO);
        ASSERT_EQ(swmm_split_material_checkpoint_capture(&target.network,&after.value),SWMM_OK);
        EXPECT_EQ(publicCheckpointBytes(before.value),publicCheckpointBytes(after.value));
        ASSERT_EQ(swmm_split_material_checkpoint_restore_at(&target.network,state.value,seconds),SWMM_OK);
        ASSERT_NO_FATAL_FAILURE(sameRecoveredPair(original,target));
    }
}
TEST(MaterialPublicRecovery, InitialCallbackAndLateEnrollmentCannotChangeOrdinaryPair) {
    MaterialPair pair;ASSERT_NO_FATAL_FAILURE(pair.start("public-late",true));int calls=0;
    ASSERT_EQ(swmm_set_step_begin_callback(&pair.network,[](SWMM_Engine engine,double,double,void* user){
        ++*static_cast<int*>(user);SWMM_MaterialCheckpoint untouched=reinterpret_cast<SWMM_MaterialCheckpoint>(1);
        EXPECT_EQ(swmm_split_material_checkpoint_enable(engine,0),SWMM_ERR_LIFECYCLE);
        EXPECT_EQ(swmm_split_material_checkpoint_capture(engine,&untouched),SWMM_ERR_LIFECYCLE);
        EXPECT_EQ(untouched,reinterpret_cast<SWMM_MaterialCheckpoint>(1));
    },&calls),SWMM_OK);
    double elapsed;ASSERT_EQ(pair.reference.step(&elapsed),SWMM_OK);ASSERT_EQ(pair.network.step(&elapsed),SWMM_OK);EXPECT_EQ(calls,1);
    ASSERT_EQ(swmm_set_step_begin_callback(&pair.network,nullptr,nullptr),SWMM_OK);
    EXPECT_EQ(swmm_split_material_checkpoint_enable(&pair.network,0),SWMM_ERR_LIFECYCLE);
    for(int k=1;k<8;++k){ASSERT_EQ(pair.reference.step(&elapsed),SWMM_OK);ASSERT_EQ(pair.network.step(&elapsed),SWMM_OK);ASSERT_NO_FATAL_FAILURE(pair.same());}
}
TEST(MaterialPublicRecovery, TerminalPairRefusalPreservesOriginalEngineDiagnostics) {
    MaterialPair pair;ASSERT_NO_FATAL_FAILURE(pair.start("public-terminal",true));
    ASSERT_EQ(swmm_split_material_checkpoint_enable(&pair.network,0),SWMM_OK);PublicCheckpoint initial;
    ASSERT_EQ(swmm_split_material_checkpoint_capture(&pair.network,&initial.value),SWMM_OK);pair.drop=true;
    double elapsed;for(int k=0;k<3;++k)ASSERT_EQ(pair.network.step(&elapsed),SWMM_OK);
    ASSERT_NE(pair.network.step(&elapsed),SWMM_OK);const auto root=pair.network.context().error_message;
    SWMM_MaterialCheckpoint untouched=reinterpret_cast<SWMM_MaterialCheckpoint>(1);
    EXPECT_EQ(swmm_split_material_checkpoint_capture(&pair.network,&untouched),SWMM_ERR_LIFECYCLE);
    EXPECT_EQ(untouched,reinterpret_cast<SWMM_MaterialCheckpoint>(1));
    EXPECT_EQ(swmm_split_material_checkpoint_restore(&pair.network,initial.value),SWMM_ERR_LIFECYCLE);
    EXPECT_EQ(pair.network.context().error_message,root);
}
TEST(MaterialPublicRecovery, ClosedSurfaceIsRefusedBeforeDereferencingItsOldOwner) {
    MaterialPair pair;ASSERT_NO_FATAL_FAILURE(pair.start("public-closed-peer",true));
    ASSERT_EQ(swmm_split_material_checkpoint_enable(&pair.network,0),SWMM_OK);PublicCheckpoint state;
    ASSERT_EQ(swmm_split_material_checkpoint_capture(&pair.network,&state.value),SWMM_OK);
    const auto bytes=publicCheckpointBytes(state.value);ASSERT_EQ(pair.surface.close(),SWMM_OK);
    SWMM_MaterialCheckpoint untouched=reinterpret_cast<SWMM_MaterialCheckpoint>(1);
    EXPECT_EQ(swmm_split_material_checkpoint_capture(&pair.network,&untouched),SWMM_ERR_LIFECYCLE);
    EXPECT_EQ(untouched,reinterpret_cast<SWMM_MaterialCheckpoint>(1));
    EXPECT_EQ(swmm_split_material_checkpoint_restore(&pair.network,state.value),SWMM_ERR_LIFECYCLE);
    EXPECT_EQ(publicCheckpointBytes(state.value),bytes);
}
#ifdef MATERIAL_OUTPUT_CHECKPOINT_TESTS
TEST(MaterialPublicRecovery, ValidOuterDamagedSecondPrefixIsAtomicAndAllowsValidRetry) {
    MaterialPair original;ASSERT_NO_FATAL_FAILURE(original.start("public-prefix-source",false,0,true,true,false,nullptr,false,{},true));
    ASSERT_EQ(swmm_split_material_checkpoint_enable(&original.network,1),SWMM_OK);ASSERT_NO_FATAL_FAILURE(original.steps(6));
    PublicCheckpoint saved;ASSERT_EQ(swmm_split_material_checkpoint_capture(&original.network,&saved.value),SWMM_OK);
    auto bad=mutatePair(publicCheckpointBytes(saved.value),[](std::string& body){
        live_recovery_test::editBlob(body,"OpenSWMM material pair output prefixes v1",[](std::string& output){
            detail::CompletedSourceArchive in(output);std::string label,binary,mesh;in(label,binary,mesh);in.finish();
            const auto at=mesh.find(std::string("\211HDF\r\n\032\n",8));if(at==mesh.npos)throw std::runtime_error("Missing HDF prefix");mesh[at]=0;
            detail::CompletedSourceArchive out;out(label,binary,mesh);output=out.bytes();
        });
    });
    PublicCheckpoint damaged;ASSERT_EQ(swmm_split_material_checkpoint_from_bytes(bad.data(),bad.size(),&damaged.value),SWMM_OK);
    MaterialPair target;ASSERT_NO_FATAL_FAILURE(target.start("public-prefix-target",false,0,true,true,false,nullptr,false,original.input,true));
    ASSERT_EQ(swmm_split_material_checkpoint_enable(&target.network,1),SWMM_OK);PublicCheckpoint before,after;
    ASSERT_EQ(swmm_split_material_checkpoint_capture(&target.network,&before.value),SWMM_OK);
    const auto binary=read(target.dir/"network.out"),mesh=read(target.dir/"surface.h5");
    EXPECT_EQ(swmm_split_material_checkpoint_restore(&target.network,damaged.value),SWMM_ERR_IO);
    ASSERT_EQ(swmm_split_material_checkpoint_capture(&target.network,&after.value),SWMM_OK);
    EXPECT_EQ(publicCheckpointBytes(before.value),publicCheckpointBytes(after.value));EXPECT_EQ(binary,read(target.dir/"network.out"));EXPECT_EQ(mesh,read(target.dir/"surface.h5"));
    ASSERT_EQ(swmm_split_material_checkpoint_restore(&target.network,saved.value),SWMM_OK);ASSERT_NO_FATAL_FAILURE(sameRecoveredPair(original,target));
}
#endif
#endif
TEST(MaterialEnginePair, AuthoredSuccessivePartialDeliveriesAllBasesBothWaterSigns) {
    for(int basis=0;basis<3;++basis)for(bool positive:{true,false}) {
        MaterialPair p;
        ASSERT_NO_FATAL_FAILURE(p.start("slices-"+std::to_string(basis)+(positive?"-positive":"-negative"),positive,basis));
        ASSERT_NO_FATAL_FAILURE(p.same());
        ASSERT_NO_FATAL_FAILURE(p.steps(8));
        EXPECT_EQ(p.network.context().nodes.coupling_queue[0]>0,positive);
        ASSERT_EQ(p.reference.end(),SWMM_OK);ASSERT_EQ(p.network.end(),SWMM_OK)<<p.network.context().error_message;
        ASSERT_NO_FATAL_FAILURE(p.same(true));EXPECT_EQ(p.finishes,1);
    }
}
#ifdef MATERIAL_OUTPUT_CHECKPOINT_TESTS // the automatic report writes 2D HDF5 results
TEST(MaterialEnginePair, AutomaticReportUsesChangedPostQualitySample) {
    for(bool positive:{true,false}) {
        MaterialPair p;
        ASSERT_NO_FATAL_FAILURE(p.start(positive?"report-positive":"report-negative",positive,0,true));
        ASSERT_NO_FATAL_FAILURE(p.steps(5));EXPECT_EQ(p.reports,1);
        const auto& receipts=p.network.peerMaterialFunding()->receipts().receipts();ASSERT_EQ(receipts.size(),2u);
        EXPECT_EQ(receipts[0].publication.phase,Book::Phase::ADVANCE);EXPECT_EQ(receipts[1].publication.phase,Book::Phase::REPORT);
        EXPECT_NE(receipts[0].samples[0].concentration,receipts[1].samples[0].concentration);
        EXPECT_EQ(receipts[1].samples[0].concentration,p.network.context().nodes.conc[0]);
        ASSERT_EQ(p.reference.end(),SWMM_OK);ASSERT_EQ(p.network.end(),SWMM_OK);
        ASSERT_NO_FATAL_FAILURE(p.same(true));p.reference.close();p.network.close();p.surface.close();
        EXPECT_EQ(read(p.dir/"native.out"),read(p.dir/"network.out"));
    }
}
#endif
TEST(MaterialEnginePair, EarlyFinalLeavesActualMaterialReceiptPending) {
    for(bool positive:{true,false}) {
        MaterialPair p;
        ASSERT_NO_FATAL_FAILURE(p.start(positive?"final-positive":"final-negative",positive));
        ASSERT_NO_FATAL_FAILURE(p.steps(5));const auto before=p.surface.peerMaterialProducer()->legCount();
        ASSERT_EQ(p.reference.end(),SWMM_OK);ASSERT_EQ(p.network.end(),SWMM_OK)<<p.network.context().error_message;
        ASSERT_NO_FATAL_FAILURE(p.same(true));EXPECT_GT(p.surface.peerMaterialProducer()->legCount(),before);
        const auto& book=p.network.peerMaterialFunding()->receipts();ASSERT_FALSE(book.receipts().empty());
        EXPECT_EQ(book.receipts().back().publication.phase,Book::Phase::FINAL);EXPECT_FALSE(book.pendingSpillAmount(0).zero());
        const auto volume=p.surface.surfaceRouter2D().state().volume;
        const auto legs=p.surface.peerMaterialProducer()->legCount();
        EXPECT_EQ(p.surface.end(),SWMM_OK);EXPECT_EQ(p.surface.surfaceRouter2D().state().volume,volume);
        EXPECT_EQ(p.surface.peerMaterialProducer()->legCount(),legs);EXPECT_EQ(p.finishes,1);
    }
}
TEST(MaterialEnginePair, EngineOwnershipCheckpointAndUnboundGuards) {
    MaterialPair p;
        ASSERT_NO_FATAL_FAILURE(p.start("guards",true,0,false,false));
    double elapsed=77;EXPECT_EQ(p.network.step(&elapsed),SWMM_ERR_LIFECYCLE);EXPECT_EQ(elapsed,77);
    EXPECT_EQ(p.surface.step(&elapsed),SWMM_ERR_LIFECYCLE);EXPECT_EQ(p.surface.end(),SWMM_ERR_LIFECYCLE);
    EXPECT_EQ(p.network.bindMaterialPeer(p.network,64),SWMM_ERR_LIFECYCLE);
    ASSERT_EQ(p.network.bindMaterialPeer(p.surface,64),SWMM_OK)<<p.network.context().error_message;
    EXPECT_EQ(p.network.bindMaterialPeer(p.surface,64),SWMM_ERR_LIFECYCLE);
    EXPECT_FALSE(p.network.attachLiveChangingVolumeSlicedExchange(64).empty());
    EXPECT_FALSE(p.surface.attachLiveChangingVolumeSlicedExchange(64).empty());
    EXPECT_FALSE(p.network.enableCheckpointing().empty());EXPECT_FALSE(p.surface.enableCheckpointing().empty());
    const auto elapsed_before=p.network.context().elapsed_ms;EXPECT_EQ(p.surface.close(),SWMM_OK);
    EXPECT_NE(p.network.step(&elapsed),SWMM_OK);EXPECT_EQ(p.network.context().elapsed_ms,elapsed_before);
}
TEST(MaterialEnginePair, NaturalCompletionFinalizesPendingTailExactlyOnce) {
    MaterialPair p;ASSERT_NO_FATAL_FAILURE(p.start("natural-completion",true,0,false,true,true));
    ASSERT_NO_FATAL_FAILURE(p.steps(4));EXPECT_EQ(p.finishes,0);
    EXPECT_EQ(p.surface.peerMaterialProducer()->legCount(),0u);
    double a=1,b=1;
    ASSERT_EQ(p.reference.step(&a),SWMM_OK);ASSERT_EQ(p.network.step(&b),SWMM_OK);
    EXPECT_EQ(a,0);EXPECT_EQ(b,0);EXPECT_EQ(p.finishes,0);EXPECT_EQ(p.advances,4);EXPECT_EQ(p.accepted,4);
    ASSERT_EQ(p.reference.end(),SWMM_OK);ASSERT_EQ(p.network.end(),SWMM_OK)<<p.network.context().error_message;
    ASSERT_NO_FATAL_FAILURE(p.same(true));EXPECT_EQ(p.finishes,1);
    const auto volume=p.surface.surfaceRouter2D().state().volume;
    const auto legs=p.surface.peerMaterialProducer()->legCount();EXPECT_GT(legs,0u);
    EXPECT_EQ(p.network.end(),SWMM_OK);EXPECT_EQ(p.surface.end(),SWMM_OK);
    EXPECT_EQ(p.finishes,1);EXPECT_EQ(p.surface.peerMaterialProducer()->legCount(),legs);
    EXPECT_EQ(p.surface.surfaceRouter2D().state().volume,volume);
    EXPECT_EQ(p.network.close(),SWMM_OK);EXPECT_FALSE(p.surface.peerMaterialProducer()->failed());
}
TEST(MaterialEnginePair, DroppedPhaseAfterNativeSpillRetainsSeparateUnresolvedOwners) {
    MaterialPair p;
        ASSERT_NO_FATAL_FAILURE(p.start("dropped",true));p.drop=true;
    double elapsed=0;for(int k=0;k<3;++k)ASSERT_EQ(p.network.step(&elapsed),SWMM_OK);
    const auto clock=p.network.context().elapsed_ms;EXPECT_NE(p.network.step(&elapsed),SWMM_OK);
    const auto* issuer=p.network.peerMaterialFunding();const auto* producer=p.surface.peerMaterialProducer();
    ASSERT_NE(issuer,nullptr);ASSERT_NE(producer,nullptr);EXPECT_TRUE(issuer->failed());EXPECT_TRUE(issuer->unresolved());
    ASSERT_NE(issuer->receipts().fundingAllowance(),nullptr);const auto& grant=*issuer->receipts().fundingAllowance();
    EXPECT_TRUE(grant.pools[0].spent.zero());EXPECT_FALSE(grant.peer_reported);EXPECT_TRUE(issuer->receipts().receipts().empty());
    bool spill=false;for(std::size_t k=0;k<producer->legCount();++k)spill|=!producer->leg(k).drain && producer->leg(k).applied;
    EXPECT_TRUE(spill);EXPECT_TRUE(producer->awaitingAcknowledgement());EXPECT_EQ(p.accepted,3);
    EXPECT_EQ(p.network.context().elapsed_ms,clock);EXPECT_GT(p.surface.context().elapsed_ms,clock);
    const auto surface_volume=p.surface.surfaceRouter2D().state().volume;
    EXPECT_NE(p.network.end(),SWMM_OK);EXPECT_EQ(p.surface.end(),SWMM_ERR_LIFECYCLE);
    EXPECT_EQ(p.surface.surfaceRouter2D().state().volume,surface_volume);
}
TEST(MaterialEnginePair, FiniteTamperedBookingRefusesBeforePublishingNetworkState) {
    for(int edit:{1,2}) {
        MaterialPair p;ASSERT_NO_FATAL_FAILURE(p.start("tampered-water-"+std::to_string(edit),true));p.corrupt_water=edit;
        double elapsed=0;for(int k=0;k<3;++k)ASSERT_EQ(p.network.step(&elapsed),SWMM_OK);
        EXPECT_NE(p.network.step(&elapsed),SWMM_OK);
        EXPECT_EQ(p.network.context().nodes.coupling_queue,p.before_queue);
        EXPECT_EQ(p.network.context().coupling_delivery_remaining,p.before_span);
        EXPECT_EQ(p.network.context().mass_balance_2d.coupling_1d_to_2d_in,p.before_surface_in);
        EXPECT_EQ(p.network.context().mass_balance_2d.coupling_2d_to_1d_out,p.before_surface_out);
        auto* issuer=p.network.peerMaterialFunding();ASSERT_NE(issuer->returnedReply(),nullptr);
        EXPECT_TRUE(issuer->unresolved());EXPECT_TRUE(issuer->receipts().materialSealPrepared());
        EXPECT_TRUE(issuer->receipts().receipts().empty());EXPECT_NE(issuer->receipts().fundingAllowance(),nullptr);
        EXPECT_TRUE(p.surface.peerMaterialProducer()->awaitingAcknowledgement());EXPECT_EQ(p.accepted,3);
    }
}
TEST(MaterialEnginePair, MasterCallbackFailureDoesNotPublishSurfaceAcceptance) {
    MaterialPair p;ASSERT_NO_FATAL_FAILURE(p.start("callback-failure",true));
    auto fail=[](SWMM_Engine,double,double,void*){throw std::runtime_error("Deliberate master callback failure");};
    ASSERT_EQ(swmm_set_step_end_callback(&p.network,fail,nullptr),SWMM_OK);
    double elapsed=0;EXPECT_NE(p.network.step(&elapsed),SWMM_OK);EXPECT_EQ(p.accepted,0);
    EXPECT_TRUE(p.network.peerMaterialFunding()->failed());
    EXPECT_TRUE(p.surface.peerMaterialProducer()->failed());
    EXPECT_EQ(p.network.context().error_message,"Deliberate master callback failure");
    EXPECT_NE(p.network.end(),SWMM_OK);
    EXPECT_EQ(p.network.context().error_message,"Deliberate master callback failure");
    EXPECT_NE(p.surface.step(&elapsed),SWMM_OK);
}
#ifdef MATERIAL_PACKET_TESTS
TEST(MaterialEnginePair, CPacketsPreserveEveryNativeSliceAllBasesBothSigns) {
    for(int basis=0;basis<3;++basis)for(bool positive:{true,false}) {
        MaterialTestTransport transport{};MaterialPair p;
        ASSERT_NO_FATAL_FAILURE(p.start("packet-slices-"+std::to_string(basis)+(positive?"-positive":"-negative"),positive,basis,false,false,false,&transport));
        ASSERT_EQ(swmm_split_material_set_transport(&p.network,material_test_transport,&transport),SWMM_OK);
        ASSERT_EQ(swmm_split_material_bind(&p.network,&p.surface,64),SWMM_OK);
        ASSERT_EQ(swmm_set_phase_callback(&p.surface,MaterialPair::observe,&p),SWMM_OK);
        for(int k=0;k<8;++k){double a=0,b=0;
            ASSERT_EQ(p.reference.step(&a),SWMM_OK);
            ASSERT_EQ(p.network.step(&b),SWMM_OK)<<p.network.context().error_message<<" / "<<swmm_split_material_last_error();
            EXPECT_EQ(a,b);ASSERT_NO_FATAL_FAILURE(p.same());
        }
        EXPECT_EQ(p.accepted,8);EXPECT_EQ(transport.offers,2);EXPECT_EQ(transport.proofs,2);EXPECT_EQ(transport.acknowledgements,2);
        EXPECT_EQ(transport.reordered,6);
        ASSERT_EQ(p.reference.end(),SWMM_OK);ASSERT_EQ(p.network.end(),SWMM_OK)<<p.network.context().error_message;
        ASSERT_NO_FATAL_FAILURE(p.same(true));p.network.close();p.surface.close();material_test_transport_close(&transport);
    }
}
TEST(MaterialEnginePair, CPacketsPreserveSettledReportAndEarlyFinal) {
    for(bool report:{true,false})for(bool positive:{true,false}) {
        MaterialTestTransport transport{};MaterialPair p;
        ASSERT_NO_FATAL_FAILURE(p.start(std::string("packet-")+(report?"report":"final")+(positive?"-positive":"-negative"),positive,0,report,false,false,&transport));
        ASSERT_EQ(swmm_split_material_set_transport(&p.network,material_test_transport,&transport),SWMM_OK);
        ASSERT_EQ(swmm_split_material_bind(&p.network,&p.surface,64),SWMM_OK);
        for(int k=0;k<5;++k){double elapsed=0;ASSERT_EQ(p.reference.step(&elapsed),SWMM_OK);
            ASSERT_EQ(p.network.step(&elapsed),SWMM_OK)<<p.network.context().error_message;
            ASSERT_NO_FATAL_FAILURE(p.same());
        }
        ASSERT_EQ(p.reference.end(),SWMM_OK);ASSERT_EQ(p.network.end(),SWMM_OK)<<p.network.context().error_message;
        ASSERT_NO_FATAL_FAILURE(p.same(true));EXPECT_EQ(transport.offers,2);EXPECT_EQ(transport.proofs,2);EXPECT_EQ(transport.acknowledgements,2);
        p.reference.close();p.network.close();p.surface.close();material_test_transport_close(&transport);
        if(report)EXPECT_EQ(read(p.dir/"native.out"),read(p.dir/"network.out"));
    }
}
TEST(MaterialEnginePair, CPacketRefusalsRetainStageSpecificOwners) {
    for(int mode=1;mode<=8;++mode) {
        MaterialTestTransport transport{};transport.mode=mode;MaterialPair p;
        ASSERT_NO_FATAL_FAILURE(p.start("packet-refusal-"+std::to_string(mode),true,0,false,false,false,&transport));
        ASSERT_EQ(swmm_split_material_set_transport(&p.network,material_test_transport,&transport),SWMM_OK);
        ASSERT_EQ(swmm_split_material_bind(&p.network,&p.surface,64),SWMM_OK);
        const int target=(mode==3||mode==5)?8:4;
        double elapsed=0;for(int k=0;k<target-1;++k){ASSERT_EQ(p.reference.step(&elapsed),SWMM_OK);
            ASSERT_EQ(p.network.step(&elapsed),SWMM_OK)<<p.network.context().error_message;
            ASSERT_NO_FATAL_FAILURE(p.same());
        }
        const auto volume=p.surface.surfaceRouter2D().state().volume,mass=p.surface.surfaceRouter2D().state().transport.cell_mass;
        const auto legs=p.surface.peerMaterialProducer()->legCount(),receipts=p.network.peerMaterialFunding()->receipts().receipts().size();
        EXPECT_NE(p.network.step(&elapsed),SWMM_OK)<<mode;
        const auto* issuer=p.network.peerMaterialFunding();const auto* producer=p.surface.peerMaterialProducer();
        EXPECT_TRUE(issuer->failed());EXPECT_TRUE(producer->failed());EXPECT_TRUE(issuer->unresolved());
        if(mode==1||mode==2||mode==3||mode==7||mode==8){
            EXPECT_EQ(p.surface.surfaceRouter2D().state().volume,volume);EXPECT_EQ(p.surface.surfaceRouter2D().state().transport.cell_mass,mass);
            EXPECT_EQ(producer->legCount(),legs);
        }else EXPECT_GT(producer->legCount(),legs);
        if(mode==6){EXPECT_EQ(issuer->receipts().receipts().size(),receipts+1);EXPECT_EQ(issuer->receipts().fundingAllowance(),nullptr);EXPECT_TRUE(producer->awaitingAcknowledgement());}
        else {EXPECT_EQ(issuer->receipts().receipts().size(),receipts);ASSERT_NE(issuer->receipts().fundingAllowance(),nullptr);
            EXPECT_TRUE(issuer->receipts().fundingAllowance()->pools[0].spent.zero());EXPECT_FALSE(issuer->receipts().fundingAllowance()->peer_reported);}
        const auto root=p.network.context().error_message;
        EXPECT_NE(p.network.end(),SWMM_OK);EXPECT_EQ(p.network.context().error_message,root);
        const auto after=p.surface.surfaceRouter2D().state().volume;EXPECT_NE(p.network.step(&elapsed),SWMM_OK);
        EXPECT_EQ(p.surface.surfaceRouter2D().state().volume,after);
        p.network.close();p.surface.close();material_test_transport_close(&transport);
    }
}
TEST(MaterialEnginePair, CPacketsAcceptRealEmptyWindowsAfterJournalFills) {
    for(bool positive:{true,false}) {
        MaterialTestTransport transport{};MaterialPair p;
        ASSERT_NO_FATAL_FAILURE(p.start(positive?"packet-empty-positive":"packet-empty-negative",positive,0,false,false,false,&transport,true));
        ASSERT_EQ(swmm_split_material_set_transport(&p.network,material_test_transport,&transport),SWMM_OK);
        ASSERT_EQ(swmm_split_material_bind(&p.network,&p.surface,6),SWMM_OK);
        for(int k=0;k<4;++k){double elapsed=0;ASSERT_EQ(p.reference.step(&elapsed),SWMM_OK);
            ASSERT_EQ(p.network.step(&elapsed),SWMM_OK)<<p.network.context().error_message;
            ASSERT_NO_FATAL_FAILURE(p.same());
        }
        EXPECT_EQ(p.surface.peerMaterialProducer()->legCount(),6u);EXPECT_EQ(transport.offers,2);
        EXPECT_EQ(transport.zero_offers,1);EXPECT_EQ(transport.empty_proofs,1);EXPECT_EQ(transport.acknowledgements,2);
        EXPECT_EQ(transport.last_ordinal,2u);EXPECT_EQ(p.network.peerMaterialFunding()->receipts().receipts().size(),1u);
        ASSERT_EQ(p.reference.end(),SWMM_OK);ASSERT_EQ(p.network.end(),SWMM_OK);
        ASSERT_NO_FATAL_FAILURE(p.same(true));p.network.close();p.surface.close();material_test_transport_close(&transport);
    }
}
TEST(MaterialEnginePair, CPacketGeometryRefusesBeforePublishingTargetOrOutput) {
    Engine network,surface;const auto input=fs::path(MATERIAL_PAIR_DATA)/"positive.inp";
    ASSERT_EQ(network.open(io::path_utf8(input).c_str(),nullptr,nullptr),SWMM_OK);ASSERT_EQ(surface.open(io::path_utf8(input).c_str(),nullptr,nullptr),SWMM_OK);
    ASSERT_EQ(swmm_split_material_configure(&network,1),SWMM_OK);ASSERT_EQ(swmm_split_material_configure(&surface,2),SWMM_OK);
    ASSERT_EQ(surface.initialize(),SWMM_OK);ASSERT_EQ(network.initialize(),SWMM_OK);FrameOwner geometry;
    ASSERT_EQ(surface.splitGeometry(&geometry.value),SWMM_OK);char* encoded=nullptr;
    ASSERT_EQ(swmm_split_material_packet_encode(&surface,geometry.value,&encoded),SWMM_OK)<<swmm_split_material_last_error();
    const auto original=nlohmann::json::parse(encoded);swmm_split_packet_free(encoded);
    const auto coupled=network.context().coupled_node;const auto area=network.context().nodes.ponded_area;
    for(int mutation=0;mutation<8;++mutation){auto j=original;auto& b=j["body"];
        if(mutation==0)b["coupled"]=nlohmann::json::array();if(mutation==1)b["coupled"][0]=0;
        if(mutation==2)b["columns"]["ponded_area"]=nlohmann::json::array();if(mutation==3)b["pond_area_ft2"]={{{"f64","0000000000000000"}}};
        if(mutation==4)b["pond_band_ft"]={{{"f64","0000000000000000"}}};if(mutation==5)b["columns"]["ponded_area"][0]["f64"]="bff0000000000000";
        if(mutation==6)b["point_map"][0]["node"]=1;if(mutation==7)j["basis"]["units"]=1;
        const auto bytes=j.dump();SWMM_SplitFrame out=reinterpret_cast<void*>(std::uintptr_t(1));
        EXPECT_EQ(swmm_split_material_packet_decode(&network,bytes.data(),bytes.size(),&out),SWMM_ERR_BADPARAM)<<mutation;
        EXPECT_EQ(out,reinterpret_cast<void*>(std::uintptr_t(1)));EXPECT_EQ(network.context().coupled_node,coupled);EXPECT_EQ(network.context().nodes.ponded_area,area);
    }
    const auto bytes=original.dump();FrameOwner decoded;ASSERT_EQ(swmm_split_material_packet_decode(&network,bytes.data(),bytes.size(),&decoded.value),SWMM_OK);
    ASSERT_EQ(network.applySplitGeometry(decoded.value),SWMM_OK);EXPECT_EQ(network.context().nodes.ponded_area,surface.context().nodes.ponded_area);
}
TEST(MaterialEnginePair, CPacketValidationIsObservationalAndStrict) {
    MaterialTestTransport transport{};MaterialPair p;
    ASSERT_NO_FATAL_FAILURE(p.start("packet-codec-negatives",true,0,false,false,false,&transport));
    ASSERT_EQ(swmm_split_material_set_transport(&p.network,material_test_transport,&transport),SWMM_OK);
    ASSERT_EQ(swmm_split_material_bind(&p.network,&p.surface,64),SWMM_OK);
    for(int k=0;k<4;++k){double elapsed=0;ASSERT_EQ(p.reference.step(&elapsed),SWMM_OK);ASSERT_EQ(p.network.step(&elapsed),SWMM_OK)<<p.network.context().error_message;}
    ASSERT_NE(transport.response,nullptr);const auto original=nlohmann::json::parse(transport.response);
    for(int mutation=0;mutation<9;++mutation){auto j=original;
        if(mutation==0)j["issuer"]="18446744073709551616";if(mutation==1)j["issuer"]=1;
        if(mutation==2)j["basis"]["si_to_native"]={{"f64","7ff0000000000000"}};
        if(mutation==3)j["basis"]["si_to_native"]=1.0;
        if(mutation==4){auto& parts=j["body"]["view"]["allowance"]["pools"][0]["granted"]["parts"];parts.push_back({{"f64","0000000000000000"}});}
        if(mutation==5)j["body"]["queue_ft3"]=nlohmann::json::array();if(mutation==6)j["extra"]=true;
        if(mutation==7)j["body"]["view"]["head_ft"]=nlohmann::json::array();
        std::string bytes=j.dump();if(mutation==8)bytes.insert(1,"\"stage\":3,");
        char* out=reinterpret_cast<char*>(std::uintptr_t(1));
        EXPECT_EQ(swmm_split_material_packet_copy(&p.network,bytes.data(),bytes.size(),&out),SWMM_ERR_BADPARAM)<<mutation;
        EXPECT_EQ(out,reinterpret_cast<char*>(std::uintptr_t(1)));EXPECT_FALSE(p.network.peerMaterialFunding()->failed());
        ASSERT_NO_FATAL_FAILURE(p.same());
    }
    quality::NativeAmountSum native;
    native.add(1);native.add(std::ldexp(1.,-106));native.add(1-std::ldexp(1.,-53));
    quality::NativeAmountSum rebuilt;for(double v:native.parts())rebuilt.add(v);
    EXPECT_FALSE(native==rebuilt);
    auto ordered=original;auto& parts=ordered["body"]["view"]["allowance"]["pools"][0]["granted"]["parts"];parts=nlohmann::json::array();
    for(double v:native.parts()){std::ostringstream hex;hex<<std::hex<<std::setfill('0')<<std::setw(16)<<std::bit_cast<std::uint64_t>(v);parts.push_back({{"f64",hex.str()}});}
    const auto bytes=ordered.dump();char* copy=nullptr;
    ASSERT_EQ(swmm_split_material_packet_copy(&p.network,bytes.data(),bytes.size(),&copy),SWMM_OK)<<swmm_split_material_last_error();
    EXPECT_EQ(nlohmann::json::parse(copy),ordered);swmm_split_packet_free(copy);
    ASSERT_NO_FATAL_FAILURE(p.same());
    p.network.close();p.surface.close();material_test_transport_close(&transport);
}
#endif
}
