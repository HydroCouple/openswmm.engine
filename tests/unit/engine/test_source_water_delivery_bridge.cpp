// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include "hydrology/SourceWaterDeliveryBridge.hpp"
#include "core/CompletedSourceArchive.hpp"
#include "core/SimulationContext.hpp"
#include "core/SWMMEngine.hpp"
#include "core/DateTime.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <type_traits>

using namespace openswmm;
using namespace openswmm::runoff;
using detail::CompletedSourceArchive;
namespace {
constexpr double ft2 = .3048*.3048;
SimulationContext model(bool lid = true) {
    SimulationContext c;
    c.options.start_date = datetime::encodeDate(2026,10,10);
    c.options.total_duration_ms = 60000; c.options.ignore_quality = true;
    c.options.ignore_snow_melt = true; c.options.ignore_groundwater = true;
    c.nodes.resize(2); c.node_names.try_add("SINK"); c.node_names.try_add("DRAIN");
    c.subcatches.resize(4); c.gages.resize(1); c.forcing.resize(2,0,4,1,0);
    c.gage_names.try_add("RG"); c.gages.ts_index[0] = 0; c.gages.interval_sec[0] = 30;
    c.tables.tables.resize(1);
    c.tables.tables[0].x = {c.options.start_date, datetime::addSeconds(c.options.start_date,30)};
    c.tables.tables[0].y = {432,0};
    for (int sc = 0; sc < 4; ++sc) {
        c.subcatch_names.try_add("S"+std::to_string(sc)); c.subcatches.gage[sc] = 0;
        c.subcatches.area[sc] = 1/(43560*ft2); c.subcatches.width[sc] = 1;
        c.subcatches.frac_imperv[sc] = 1; c.subcatches.pct_zero[sc] = 100;
        c.subcatches.n_imperv[sc] = c.subcatches.n_perv[sc] = 0;
        c.subcatches.infil_model[sc] = 2; c.subcatches.infil_p1[sc] = 3.5;
        c.subcatches.infil_p2[sc] = .5; c.subcatches.infil_p3[sc] = .26;
        c.subcatches.outlet_node[sc] = sc == 2 ? 1 : 0;
    }
    c.subcatches.outlet_node[0] = -1; c.subcatches.outlet_subcatch[0] = 1;
    if (lid) {
        c.lid_controls.names = {"TRENCH"}; c.lid_controls.lid_type = {"IT"};
        c.lid_controls.surface = {{3,0,.1,0,0}}; c.lid_controls.storage = {{12,.4,0,0}};
        c.lid_controls.drain = {{43.2,.5,0,0,0,0}};
        c.lid_usage.subcatch_index = {0}; c.lid_usage.lid_index = {0};
        c.lid_usage.number = {2}; c.lid_usage.area = {.25/ft2}; c.lid_usage.width = {1};
        c.lid_usage.init_sat = {50}; c.lid_usage.from_imperv = {100}; c.lid_usage.from_perv = {0};
        c.lid_usage.to_perv = {0}; c.lid_usage.drain_to = {"DRAIN"};
    }
    return c;
}
struct Pair {
    SourceWaterDriver source;
    SourceWaterDeliveryBridge bridge;
    explicit Pair(bool lid = true, std::vector<int> seeds = {0,2}) {
        auto c = model(lid);
        auto err = source.initialize(c,seeds,{{0,lid ? .5 : 1.},{1,1},{2,1},{3,1}});
        if (!err.empty()) throw std::runtime_error(err);
        err = bridge.initialize(source); if (!err.empty()) throw std::runtime_error(err);
    }
};
void advance(SourceWaterDriver& source, int group, double end) {
    RunoffSolver::InfiltrationBoundary sealed = [](int,double,double,double& rate) { rate=0;return true; };
    auto err = source.stage(group,end,end,&sealed);
    if (!err.empty()) throw std::runtime_error(err);
    err = source.commit(); if (!err.empty()) throw std::runtime_error(err);
    EXPECT_NEAR(source.balanceResidual(group),0,1e-9);
}
void receive(SourceWaterDeliveryBridge& b, double end) {
    auto err = b.stage(b.receiverEnd(),end); if (!err.empty()) throw std::runtime_error(err);
    err = b.commit(); if (!err.empty()) throw std::runtime_error(err);
}
double volume(const std::vector<SourceNodeDelivery>& v) {
    double sum = 0; for (auto& d : v) sum += d.runoff+d.drain; return sum;
}
std::string observed(const SourceWaterDeliveryBridge& b) {
    CompletedSourceArchive a; auto end=b.receiverEnd(); auto count=b.capturedCount(); a(end,count);
    auto pending=b.pending();a.sequence(pending,[&](auto& q) {
        auto& d=q.water;a(q.ordinal,q.available,d.source,d.node,d.start,d.end,d.volume,d.drain,d.mass);
    });
    auto received=b.received(), delivered=b.delivered();
    for (auto& d : received) a(d.runoff,d.drain,d.rate);
    for (auto& d : delivered) a(d.runoff,d.drain,d.rate);
    return a.bytes();
}
std::string identity(Pair& p) {
    CompletedSourceArchive a;p.source.checkpoint(a,true);p.bridge.checkpoint(a,true,p.source);return a.bytes();
}
std::string checkpoint(Pair& p) {
    // As in the existing component envelopes, bind runtime bytes to model
    // identity before privately restoring any component with compatible shapes.
    CompletedSourceArchive a;auto model=identity(p);a(model);
    p.source.checkpoint(a,false);p.bridge.checkpoint(a,false,p.source);return a.bytes();
}
void restore(Pair& p, const std::string& bytes) {
    CompletedSourceArchive a(bytes);std::string model;a(model);
    if (model!=identity(p))throw std::runtime_error("Paired source delivery model differs.");
    auto source=p.source.waterTrial(); auto bridge=p.bridge;
    source.checkpoint(a,false);bridge.checkpoint(a,false,source);a.finish();
    static_assert(std::is_nothrow_move_assignable_v<SourceWaterDriver>);
    static_assert(std::is_nothrow_move_assignable_v<SourceWaterDeliveryBridge>);
    p.source=std::move(source);p.bridge=std::move(bridge);
}
std::filesystem::path root() {
    const auto* env=std::getenv("SWMM_R4_DELIVERY_OUT");
    auto p=env ? std::filesystem::path(env) : std::filesystem::path(__FILE__).parent_path()/"source_delivery_out";
    std::filesystem::create_directories(p); return p;
}
void write(const std::filesystem::path& path,const std::string& bytes) { std::ofstream out(path,std::ios::binary);out<<bytes; }
}

TEST(SourceWaterDeliveryBridge, OwnershipIncludesConnectedRunoffAndDrainGraphsOnly) {
    Pair p;
    EXPECT_EQ(p.bridge.sourceGroups(),(std::vector<int>{0,0,1,-1}));
    EXPECT_TRUE(p.bridge.ownsSubcatch(0)); EXPECT_TRUE(p.bridge.ownsSubcatch(1));
    EXPECT_TRUE(p.bridge.ownsSubcatch(2)); EXPECT_FALSE(p.bridge.ownsSubcatch(3));
    EXPECT_FALSE(p.bridge.ownsSubcatch(-1)); EXPECT_FALSE(p.bridge.ownsSubcatch(4));
    EXPECT_EQ(p.bridge.validateReceiver(p.source.context()),"");
    auto c=model(); c.node_names.clear(); c.node_names.try_add("DRAIN"); c.node_names.try_add("SINK");
    EXPECT_FALSE(p.bridge.validateReceiver(c).empty());
    c.nodes.resize(3); EXPECT_FALSE(p.bridge.validateReceiver(c).empty());
    c=model();c.lid_usage.drain_to[0]="S2"; SourceWaterDriver d;
    ASSERT_EQ(d.initialize(c,{1},{{0,.5},{1,1},{2,1},{3,1}}),"");
    SourceWaterDeliveryBridge b; ASSERT_EQ(b.initialize(d),"");
    EXPECT_EQ(b.sourceGroups(),(std::vector<int>{0,0,0,-1}));
}
TEST(SourceWaterDeliveryBridge, CompletedVolumesArePastOnlyAndDeliveredOnceWithCancelRetry) {
    Pair p; receive(p.bridge,1);
    advance(p.source,0,1); advance(p.source,1,1);
    ASSERT_EQ(p.bridge.capture(p.source,1),"");
    ASSERT_FALSE(p.bridge.pending().empty()); const auto before=observed(p.bridge);
    EXPECT_GT(volume(p.bridge.received()),0); EXPECT_DOUBLE_EQ(volume(p.bridge.delivered()),0);
    ASSERT_EQ(p.bridge.capture(p.source,1),""); EXPECT_EQ(observed(p.bridge),before);
    ASSERT_EQ(p.bridge.stage(1,1.25),"");const auto receipts=p.bridge.receipts();
    EXPECT_GT(receipts[0].runoff,0); EXPECT_GT(receipts[1].drain,0);
    for (auto& d : receipts) EXPECT_DOUBLE_EQ(d.rate,(d.runoff+d.drain)/.25);
    EXPECT_EQ(observed(p.bridge),before);
    EXPECT_FALSE(p.bridge.capture(p.source,1).empty()); EXPECT_FALSE(p.bridge.stage(1,2).empty());
    EXPECT_THROW(checkpoint(p),std::runtime_error); p.bridge.cancel(); EXPECT_EQ(observed(p.bridge),before);
    ASSERT_EQ(p.bridge.stage(1,2),""); EXPECT_DOUBLE_EQ(volume(p.bridge.receipts()),volume(receipts));
    ASSERT_EQ(p.bridge.commit(),""); EXPECT_TRUE(p.bridge.pending().empty());
    EXPECT_DOUBLE_EQ(volume(p.bridge.delivered()),volume(p.bridge.received()));
    EXPECT_FALSE(p.bridge.commit().empty()); ASSERT_EQ(p.bridge.stage(2,3),"");
    EXPECT_DOUBLE_EQ(volume(p.bridge.receipts()),0); ASSERT_EQ(p.bridge.commit(),"");
    EXPECT_DOUBLE_EQ(p.source.ledgers()[3].rain,0);
}
TEST(SourceWaterDeliveryBridge, SourceAndReceiverClocksMayDifferWithoutPrematurePublication) {
    Pair p; receive(p.bridge,1); const auto before=observed(p.bridge);
    advance(p.source,0,2); EXPECT_FALSE(p.bridge.capture(p.source,1).empty());
    EXPECT_EQ(observed(p.bridge),before); receive(p.bridge,2);
    ASSERT_EQ(p.bridge.capture(p.source,2),"");
    EXPECT_EQ(p.source.clocks().groups()[1].completed_end,0);
    for (auto& q : p.bridge.pending()) { EXPECT_LE(q.water.end,q.available);EXPECT_EQ(q.available,2); }
    receive(p.bridge,2.5); advance(p.source,1,2.25);
    ASSERT_EQ(p.bridge.capture(p.source,2.5),"");
    for (auto& q : p.bridge.pending()) { EXPECT_DOUBLE_EQ(q.water.end,2.25);EXPECT_EQ(q.available,2.5); }
    const auto accepted=observed(p.bridge);
    ASSERT_EQ(p.source.stage(0,3,3),"");
    EXPECT_FALSE(p.bridge.capture(p.source,2.5).empty()); EXPECT_EQ(observed(p.bridge),accepted);
    p.source.cancel(); ASSERT_EQ(p.bridge.capture(p.source,2.5),"");
    for (auto interval : {std::pair{-1.,3.},std::pair{2.,3.},std::pair{2.5,2.5},
                          std::pair{2.5,std::numeric_limits<double>::infinity()}}) {
        EXPECT_FALSE(p.bridge.stage(interval.first,interval.second).empty());
        EXPECT_EQ(observed(p.bridge),accepted);
    }
}
TEST(SourceWaterDeliveryBridge, OwnershipAndDestinationChangesFailBeforeAnyReceiptIsInstalled) {
    Pair p; receive(p.bridge,1); const auto before=observed(p.bridge);
    Pair other(true,{3}); advance(other.source,0,1);
    EXPECT_FALSE(p.bridge.capture(other.source,1).empty()); EXPECT_EQ(observed(p.bridge),before);
    auto c=model();c.subcatches.width[0]=2; SourceWaterDriver changed;
    ASSERT_EQ(changed.initialize(c,{0,2},{{0,.5},{1,1},{2,1},{3,1}}),"");advance(changed,0,1);
    EXPECT_FALSE(p.bridge.capture(changed,1).empty()); EXPECT_EQ(observed(p.bridge),before);
    c=model();c.subcatches.outlet_node[1]=-1; SourceWaterDriver unresolved;
    ASSERT_EQ(unresolved.initialize(c,{0,2},{{0,.5},{1,1},{2,1},{3,1}}),"");
    SourceWaterDeliveryBridge b; EXPECT_FALSE(b.initialize(unresolved).empty());
    EXPECT_EQ(b.initialize(p.source),"");EXPECT_FALSE(b.initialize(p.source).empty());
    advance(p.source,0,1);SourceWaterDeliveryBridge late; EXPECT_FALSE(late.initialize(p.source).empty());
    c=model();c.pollutant_names.try_add("Tracer");c.pollutants.resize_pollutants(1);SourceWaterDriver quality;
    ASSERT_EQ(quality.initialize(c,{0},{{0,.5},{1,1},{2,1},{3,1}}),"");
    SourceWaterDeliveryBridge wet; EXPECT_FALSE(wet.initialize(quality).empty());
}
TEST(SourceWaterDeliveryBridge, VariableCadencesAndRestartPreserveSourceAndDeliveryAccounting) {
    for (bool lid : {false,true}) {
        Pair a(lid), b(lid); receive(a.bridge,1);
        advance(a.source,0,1); advance(a.source,1,.75); ASSERT_EQ(a.bridge.capture(a.source,1),"");
        auto saved=checkpoint(a); write(root()/(lid ? "lid-pending.bin" : "runoff-pending.bin"),saved);
        restore(b,saved); EXPECT_EQ(checkpoint(a),checkpoint(b));
        double last0=1,last1=.75;
        for (double end : {1.25,2.,2.75,5.,7.,8.25,12.,19.,30.,37.,40.}) {
            for (auto* p : {&a,&b}) {
                ASSERT_EQ(p->bridge.stage(p->bridge.receiverEnd(),end),"");
                const auto receipts=p->bridge.receipts();
                EXPECT_NEAR(volume(p->bridge.delivered())+volume(receipts),volume(p->bridge.received()),1e-10);
                ASSERT_EQ(p->bridge.commit(),"");
                if (end>last0) advance(p->source,0,end);
                const double end1=end-.125; if (end1>last1) advance(p->source,1,end1);
                ASSERT_EQ(p->bridge.capture(p->source,end),"");
                double pending=0,outlet=0;
                for (auto& q : p->bridge.pending()) pending+=q.water.volume;
                for (auto& ledger : p->source.ledgers()) outlet+=ledger.outlet;
                EXPECT_NEAR(outlet,volume(p->bridge.delivered())+pending,1e-10);
            }
            last0=end;last1=end-.125;EXPECT_EQ(checkpoint(a),checkpoint(b));
        }
        receive(a.bridge,41);receive(b.bridge,41);EXPECT_EQ(checkpoint(a),checkpoint(b));
        EXPECT_DOUBLE_EQ(volume(a.bridge.delivered()),volume(a.bridge.received()));
    }
}

namespace {
struct Wire {
    uint32_t version=2;uint64_t count=0,digest=0;double end=0;
    uint64_t last_begin=0;double last_start=0;std::vector<SourceNodeDelivery> last=std::vector<SourceNodeDelivery>(2);
    std::vector<double> clocks={0,0};std::vector<QueuedSourceDelivery> pending;
    std::vector<SourceNodeDelivery> received=std::vector<SourceNodeDelivery>(2),delivered=received;
    void io(CompletedSourceArchive& a) {
        a(version,count,digest,end);a.fixed(clocks);
        a.sequence(pending,[&](auto& q){auto& d=q.water;a(q.ordinal,q.available,d.source,d.node,d.start,d.end,d.volume,d.drain);});
        a.sequence(received,[&](auto& d){a(d.runoff,d.drain);},true);
        a.sequence(delivered,[&](auto& d){a(d.runoff,d.drain);},true);
        a(last_begin,last_start);a.sequence(last,[&](auto& d){a(d.runoff,d.drain,d.rate);},true);
    }
};
}
TEST(SourceWaterDeliveryBridge, CorruptPairedRestorePreservesBothTargetsAndPermitsValidRetry) {
    Pair a; receive(a.bridge,2);advance(a.source,0,2);advance(a.source,1,1.5);
    ASSERT_EQ(a.bridge.capture(a.source,2),"");const auto valid=checkpoint(a);
    CompletedSourceArchive sa;auto fingerprint=identity(a);sa(fingerprint);a.source.checkpoint(sa,false);
    const auto source_bytes=sa.bytes();Wire original;CompletedSourceArchive read(valid.substr(source_bytes.size()));
    original.io(read);read.finish();ASSERT_GT(original.pending.size(),1);
    for (int mutation=0;mutation<23;++mutation) {
        SCOPED_TRACE(mutation);auto w=original;
        switch (mutation) {
        case 0:w.version=1;break;
        case 1:++w.count;break;
        case 2:w.digest^=1;break;
        case 3:w.end=-1;break;
        case 4:w.clocks[0]=3;break;
        case 5:w.pending[0].ordinal=1;break;
        case 6:w.pending[0].available=1;break;
        case 7:w.pending[0].water.node=99;break;
        case 8:w.pending[0].water.volume+=1;break;
        case 9:w.pending[0].water.start=std::numeric_limits<double>::quiet_NaN();break;
        case 10:w.pending[0].water.drain=!w.pending[0].water.drain;break;
        case 11:w.received[0].runoff+=1;break;
        case 12:w.delivered[0].drain=-1;break;
        case 13:w.pending[1].ordinal=w.pending[0].ordinal;break;
        case 14:w.delivered[0].runoff=std::numeric_limits<double>::infinity();break;
        case 15:w.received[0].drain=std::numeric_limits<double>::quiet_NaN();break;
        case 16:w.clocks[1]=std::numeric_limits<double>::quiet_NaN();break;
        case 17:w.last_begin=w.count+1;break;
        case 18:w.last_start=w.end;break;
        case 19:w.last[0].runoff+=1;break;
        case 20:w.last[0].rate=1;break;
        case 21:w.last[0].drain=-1;break;
        case 22:w.last_start=std::numeric_limits<double>::quiet_NaN();break;
        }
        CompletedSourceArchive out;w.io(out);Pair target;const auto before=checkpoint(target);
        EXPECT_THROW(restore(target,source_bytes+out.bytes()),std::runtime_error);
        EXPECT_EQ(checkpoint(target),before);restore(target,valid);EXPECT_EQ(checkpoint(target),valid);
    }
    for (auto bad : {valid.substr(0,valid.size()-1),valid+"trailing"}) {
        Pair target;const auto before=checkpoint(target);EXPECT_THROW(restore(target,bad),std::runtime_error);
        EXPECT_EQ(checkpoint(target),before);restore(target,valid);EXPECT_EQ(checkpoint(target),valid);
    }
    Pair mismatch(false);const auto before=checkpoint(mismatch);
    EXPECT_THROW(restore(mismatch,valid),std::runtime_error);EXPECT_EQ(checkpoint(mismatch),before);
    // Authored material and node-name changes with identical runtime shapes
    // must also fail; component field counts alone cannot establish identity.
    for (int change=0;change<2;++change) {
        auto c=model();if (change==0)c.subcatches.width[0]=2;
        else {c.node_names.clear();c.node_names.try_add("DRAIN");c.node_names.try_add("SINK");}
        Pair changed;changed.source=SourceWaterDriver{};changed.bridge=SourceWaterDeliveryBridge{};
        ASSERT_EQ(changed.source.initialize(c,{0,2},{{0,.5},{1,1},{2,1},{3,1}}),"");
        ASSERT_EQ(changed.bridge.initialize(changed.source),"");const auto initial=checkpoint(changed);
        EXPECT_THROW(restore(changed,valid),std::runtime_error);EXPECT_EQ(checkpoint(changed),initial);
    }
}
TEST(SourceWaterDeliveryBridge, CheckpointRefusesUnpublishedCompletedWaterAndChangedPrefix) {
    Pair p; const auto initial_source=p.source.waterTrial();receive(p.bridge,1);advance(p.source,0,1);
    EXPECT_THROW(checkpoint(p),std::runtime_error);ASSERT_EQ(p.bridge.capture(p.source,1),"");
    auto saved=checkpoint(p);auto reset=initial_source.waterTrial();CompletedSourceArchive a(saved);
    std::string fingerprint;a(fingerprint);
    reset.checkpoint(a,false);auto b=p.bridge;
    EXPECT_FALSE(b.capture(initial_source,1).empty()); EXPECT_EQ(observed(b),observed(p.bridge));
    ASSERT_EQ(b.capture(reset,1),"");
    Pair prefix;receive(prefix.bridge,1);advance(prefix.source,0,.75);
    EXPECT_FALSE(b.capture(prefix.source,1).empty());EXPECT_EQ(observed(b),observed(p.bridge));
}

TEST(SourceWaterDeliveryBridge, DryAndEmptySourceProfilesStillPublishTheirClockBoundary) {
    for (auto seeds : {std::vector<int>{0,2},std::vector<int>{}}) {
        auto c=model(false);c.tables.tables[0].y={0,0};
        SourceWaterDriver source;ASSERT_EQ(source.initialize(c,seeds,{{0,1},{1,1},{2,1},{3,1}}),"");
        SourceWaterDeliveryBridge bridge;ASSERT_EQ(bridge.initialize(source),"");receive(bridge,1);
        for (int g=0;g<int(source.clocks().groups().size());++g) advance(source,g,1);
        EXPECT_TRUE(source.deliveries().empty());
        if (!seeds.empty()) {
            CompletedSourceArchive a;EXPECT_THROW(bridge.checkpoint(a,false,source),std::runtime_error);
        }
        ASSERT_EQ(bridge.capture(source,1),"");CompletedSourceArchive a;bridge.checkpoint(a,false,source);
        auto b=bridge;CompletedSourceArchive read(a.bytes());b.checkpoint(read,false,source);read.finish();
        EXPECT_EQ(observed(b),observed(bridge));ASSERT_EQ(bridge.stage(1,2),"");
        EXPECT_DOUBLE_EQ(volume(bridge.receipts()),0);ASSERT_EQ(bridge.commit(),"");
    }
}

TEST(SourceWaterDeliveryBridge, QualifiedReceiptRatesDriveActualEngineStorageWithoutReplayingWater) {
    Pair p(false);SWMMEngine engine;const auto path=root()/"receiving-engine.inp";
    write(path,"[OPTIONS]\nFLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\nSTART_DATE 10/10/2026\nSTART_TIME 00:00:00\nEND_DATE 10/10/2026\nEND_TIME 00:00:30\nROUTING_STEP 1\nVARIABLE_STEP 0\nREPORT_STEP 00:00:01\nWET_STEP 00:00:01\nDRY_STEP 00:00:05\nIGNORE_QUALITY YES\nIGNORE_2D YES\n[STORAGE]\nSINK 0 100 0 FUNCTIONAL 0 0 100 0 0\n[OUTFALLS]\nDRAIN 0 FREE NO\n");
    ASSERT_EQ(engine.open(path.string().c_str(),(path.string()+".rpt").c_str(),nullptr),0);
    ASSERT_EQ(engine.initialize(),0)<<engine.context().error_message;
    ASSERT_EQ(engine.start(0),0)<<engine.context().error_message;
    ASSERT_EQ(p.bridge.validateReceiver(engine.context()),"");
    double elapsed=0;
    for (int step=0;step<10;++step) {
        ASSERT_EQ(p.bridge.stage(step,step+1),"");
        const double sink_rate=p.bridge.receipts()[0].rate;
        for (int n=0;n<2;++n) engine.context().nodes.user_lat_flow[n]=p.bridge.receipts()[n].rate;
        ASSERT_EQ(engine.step(&elapsed),0);ASSERT_EQ(engine.context().current_time,step+1);
        ASSERT_EQ(p.bridge.commit(),"");
        // DW averages successive endpoint rates. With fixed one-second steps,
        // half the current sink receipt remains in that interpolation history.
        EXPECT_NEAR(engine.context().nodes.volume[0]+.5*sink_rate,
                    p.bridge.delivered()[0].runoff+p.bridge.delivered()[0].drain,1e-9);
        EXPECT_NEAR(engine.context().mass_balance.routing_outflow,p.bridge.delivered()[1].runoff+p.bridge.delivered()[1].drain,1e-9);
        if (step<5) {
            advance(p.source,0,step+1);advance(p.source,1,step+1);
            ASSERT_EQ(p.bridge.capture(p.source,step+1),"");
        }
    }
    EXPECT_GT(volume(p.bridge.delivered()),0); EXPECT_TRUE(p.bridge.pending().empty());
    EXPECT_NEAR(engine.context().nodes.volume[0]+engine.context().mass_balance.routing_outflow,
                volume(p.bridge.delivered()),1e-9);
    EXPECT_NEAR(engine.context().mass_balance.routing_forcing_inflow,volume(p.bridge.delivered()),1e-9);
    // Explicit qualification injection uses the existing forcing path. The live
    // engine does not attach spatial ownership or book these as wet weather yet.
    engine.end();engine.close();
}
