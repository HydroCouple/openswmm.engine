// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include <gtest/gtest.h>
#include "quality/PeerPassiveStorageExchange.hpp"
#include "quality/LivePassiveStorageExchange.hpp"
#include "quality/QualityRouting.hpp"
#include "core/SimulationContext.hpp"
#include "core/CompletedSourceArchive.hpp"
#ifdef PEER_RECOVERY_PACKET_TESTS
#include "core/MaterialPacket.hpp"
#endif
#include "hydraulics/TimestepController.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include "2d/solver/ExplicitInertialSolver.hpp"
#include "2d/coupling/NodeCoupling.hpp"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <type_traits>
#if defined(__APPLE__) || defined(__linux__)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
using namespace openswmm;
using namespace openswmm::quality;
using Book=PassiveStorageExchange;
using Phase=Book::Phase;
constexpr double dt=1./1024,ft3=.3048*.3048*.3048;
void nativePool(SimulationContext& ctx,double concentration) {
    ctx.node_names.add("Tank");ctx.pollutant_names.add("Dye");ctx.allocate_objects();ctx.mass_balance.resize_quality(1);
    ctx.options.routing_model=RoutingModel::DYNWAVE;ctx.node_subtypes.set_node_type(ctx.nodes,0,NodeType::STORAGE);
    auto& n=ctx.nodes;n.volume[0]=n.old_volume[0]=100;n.depth[0]=n.old_depth[0]=10;
    n.invert_elev[0]=-10;n.head[0]=0;n.full_depth[0]=1;n.full_volume[0]=10;
    n.conc[0]=n.conc_old[0]=concentration;
}
// Existing manufactured closed two-way native marcher fixture. These tests
// qualify one owner-protocol window, not an authored full-engine split run.
struct Surface {
    SimulationContext ctx;twoD::MeshData mesh;twoD::SurfaceStateData state;
    twoD::SolverOptions2D options;twoD::ExplicitInertialSolver solver;
    std::vector<twoD::CouplingPoint> points;
    std::vector<double> published;
    Surface(double node_c=1,double surface_c=2) {
        nativePool(ctx,node_c);mesh.resize_vertices(6);mesh.resize_triangles(2);
        for(int k=0;k<2;++k) {
            const int v=3*k;mesh.vx[v]=4*k;mesh.vy[v]=0;mesh.vx[v+1]=4*k+2;mesh.vy[v+1]=0;
            mesh.vx[v+2]=4*k;mesh.vy[v+2]=1;for(int j=0;j<3;++j)mesh.vz[v+j]=-1;
            mesh.set_triangle(k,v,v+1,v+2);mesh.mannings_n[k]=.03;points.push_back({k,-1,0,.7,.01,false,false});
        }
        twoD::buildMeshTopology(mesh);state.resize(2,6);state.transport.resize(1,2,2);state.transport.cell_runoff_vol.assign(2,0);
        for(int k=0;k<2;++k) {
            state.volume[k]=state.depth[k]=k?.5:1.5;state.head[k]=-1+state.depth[k];state.transport.cell_mass[k]=state.volume[k]*surface_c;
        }
        published={node_c};state.nodes_1d=&ctx.nodes;state.node_row_conc=&published;state.node_coupling=&points;
        options.backend=twoD::Backend2D::CPU;options.lts_tiers=1;options.max_timestep=dt;
        options.len_1d_to_2d=.3048;options.vol_1d_to_2d=ft3;options.flow_2d_to_1d=1/ft3;
        solver.initialize(mesh,state,options);
    }
    std::vector<Book::HydraulicPoint> map()const{return {{0,0,0,0},{1,1,0,0}};}
    void march(PassiveStorageAdmission& admission) {
        state.live_passive_exchange=&admission;
        try{solver.advance(0,dt);state.live_passive_exchange=nullptr;}
        catch(...){state.live_passive_exchange=nullptr;throw;}
    }
};
struct Pair {
    SimulationContext network;
    Surface surface;
    std::unique_ptr<PeerPassiveStorageFunding> issuer;
    std::unique_ptr<PeerPassiveStorageProducer> producer;
    explicit Pair(double node_c=1,double cell_c=2,std::size_t limit=16):surface(node_c,cell_c) {
        nativePool(network,node_c);
        issuer=std::make_unique<PeerPassiveStorageFunding>(network,11,22,surface.map(),1/ft3,limit);
        producer=std::make_unique<PeerPassiveStorageProducer>(surface.ctx,surface.state,issuer->bindRecipient(surface.ctx),1/ft3,limit);
    }
    void open() {
        issuer->freeze(dt);issuer->observeDrain();producer->begin(issuer->issue(Phase::ADVANCE,0,1000*dt));
    }
    std::unique_ptr<PeerFundingReply> march() {
        surface.march(*producer);return producer->seal(surface.solver.last_coupling_exchange());
    }
};
std::filesystem::path evidence(const std::string& name) {
    const char* chosen=std::getenv("OPENSWMM_PEER_FUNDING_EVIDENCE");
    auto p=std::filesystem::path(chosen?chosen:PEER_FUNDING_EVIDENCE)/name;std::filesystem::create_directories(p.parent_path());return p;
}
void transfer(bool positive) {
    const double nc=positive?1:2,sc=positive?2:1;
    Pair p(nc,sc);Surface reference(nc,sc);
    LivePassiveStorageExchange integrated(reference.ctx,reference.state,reference.points,1/ft3,16,
        PassiveStorageArithmetic::RETAIN_EXACT_SLICES,false,PassiveStorageLaw::CHANGING_VOLUME,PassiveStorageWaterArithmetic::RETAIN_AGGREGATES);
    ASSERT_NE(&p.network,&p.surface.ctx);EXPECT_EQ(p.surface.state.nodes_1d,&p.surface.ctx.nodes);
    const double initial=100*nc;
    ASSERT_NO_THROW(p.open());integrated.freeze(reference.ctx,dt);integrated.observeDrain(reference.ctx);
    integrated.beginWindow(reference.ctx,reference.state,0,1000*dt,Phase::ADVANCE);
    std::unique_ptr<PeerFundingReply> reply;ASSERT_NO_THROW(reply=p.march());
    ASSERT_NO_THROW(reference.march(integrated));
    ASSERT_NO_THROW(integrated.seal(reference.ctx,reference.solver.last_coupling_exchange()));
    EXPECT_EQ(p.surface.state.volume,reference.state.volume);EXPECT_EQ(p.surface.state.transport.cell_mass,reference.state.transport.cell_mass);
    ASSERT_EQ(p.producer->legCount(),2u);EXPECT_TRUE(p.producer->leg(0).drain);EXPECT_FALSE(p.producer->leg(1).drain);
    EXPECT_EQ(reply->proof().legs,std::vector<Book::AppliedLeg>({integrated.leg(0),integrated.leg(1)}));
    ASSERT_NE(p.issuer->receipts().fundingAllowance(),nullptr);
    EXPECT_TRUE(p.issuer->receipts().fundingAllowance()->pools[0].spent.zero());
    ASSERT_NO_THROW(p.issuer->accept(std::move(reply)));EXPECT_TRUE(p.issuer->receipts().receipts().empty());
    EXPECT_TRUE(p.issuer->receipts().materialSealPrepared());EXPECT_TRUE(p.issuer->receipts().fundingAllowance()->spills.empty());
    EXPECT_TRUE(p.issuer->receipts().fundingAllowance()->peer_reported);
    // The actual native two-way point totals cancel: no fabricated water queue,
    // hydraulic volume, or clock edits are needed for this bounded witness.
    const auto& water=p.surface.solver.last_coupling_exchange();ASSERT_EQ(water[0]+water[1],0);
    integrated.observeWaterBooking(reference.ctx);
    auto ack=p.issuer->confirmWater();EXPECT_TRUE(p.issuer->unresolved());p.producer->acknowledge(std::move(ack));
    EXPECT_FALSE(p.issuer->unresolved());EXPECT_EQ(p.issuer->receipts().receipts(),integrated.receipts().receipts());
    QualitySolver net_solver,ref_solver;net_solver.init(1,0,1);ref_solver.init(1,0,1);
    p.issuer->receive(net_solver,dt);integrated.receive(reference.ctx,ref_solver,dt); // frozen empty cohort
    hydraulics::TimestepController::advance(p.network,dt);hydraulics::TimestepController::advance(reference.ctx,dt);
    hydraulics::TimestepController::advance(p.surface.ctx,dt); // completed actual producer window
    p.network.save_lat_qual_state();reference.ctx.save_lat_qual_state();
    p.issuer->freeze(dt);p.issuer->observeDrain();integrated.freeze(reference.ctx,dt);integrated.observeDrain(reference.ctx);
    p.issuer->receive(net_solver,dt);integrated.receive(reference.ctx,ref_solver,dt); // sole receiving quality writer
    hydraulics::TimestepController::advance(p.network,dt);hydraulics::TimestepController::advance(reference.ctx,dt);
    EXPECT_EQ(p.network.nodes.conc,reference.ctx.nodes.conc);
    EXPECT_EQ(p.issuer->receipts().storedRoundoff(0),integrated.receipts().storedRoundoff(0));
    EXPECT_EQ(p.issuer->receipts().receipts(),integrated.receipts().receipts());
    EXPECT_TRUE(p.issuer->receipts().pendingDrainAmount(0).zero());EXPECT_TRUE(p.issuer->receipts().pendingSpillAmount(0).zero());
    EXPECT_EQ(p.network.nodes.conc[0]>nc,positive);EXPECT_THROW(p.issuer->receipts().checkpoint(p.network),std::exception);
    const auto& row=p.issuer->receipts().receipts()[0].accepted;
    std::ofstream trace(evidence(positive?"positive.txt":"negative.txt"));trace<<std::hexfloat<<initial<<' '
        <<p.network.nodes.volume[0]*p.network.nodes.conc[0]<<'\n';
    for(const auto* amount:{&p.issuer->receipts().storedRoundoff(0)}){for(double part:amount->parts())trace<<part<<' ';trace<<'\n';}
    for(bool drain:{true,false}){NativeAmountSum sum;for(const auto& r:row)sum.add(r.materialAmount(drain));for(double part:sum.parts())trace<<part<<' ';trace<<'\n';}
    EXPECT_FALSE(p.producer->failed());EXPECT_FALSE(p.issuer->failed());
}
TEST(PeerPassiveFunding, RealBidirectionalNativeWindowAndOneReceivingIntervalBothSigns){transfer(true);transfer(false);}
TEST(PeerPassiveFunding, NativeCapsulesCannotBeCopiedOrBuiltFromDiagnosticViews) {
    static_assert(!std::is_copy_constructible_v<PeerFundingRecipient> && !std::is_copy_assignable_v<PeerFundingRecipient> && !std::is_move_assignable_v<PeerFundingRecipient>);
    static_assert(!std::is_copy_assignable_v<PeerFundingOffer> && !std::is_move_assignable_v<PeerFundingOffer>);
    static_assert(!std::is_copy_assignable_v<PeerFundingReply> && !std::is_move_assignable_v<PeerFundingReply>);
    static_assert(!std::is_copy_assignable_v<PeerFundingAcknowledgement> && !std::is_move_assignable_v<PeerFundingAcknowledgement>);
    static_assert(!std::is_copy_constructible_v<PeerFundingOffer>);
    static_assert(!std::is_copy_constructible_v<PeerFundingReply>);
    static_assert(!std::is_copy_constructible_v<PeerFundingAcknowledgement>);
    static_assert(!std::is_constructible_v<PeerFundingOffer,PeerFundingView>);
    Pair p;p.issuer->freeze(dt);p.issuer->observeDrain();auto offer=p.issuer->issue(Phase::ADVANCE,0,1000*dt);
    auto diagnostic=offer->view();EXPECT_EQ(diagnostic.allowance.peer,p.issuer->identity());
    EXPECT_THROW(p.issuer->issue(Phase::ADVANCE,0,1000*dt),std::exception);
}
TEST(PeerPassiveFunding, SameContextAndSecondRecipientRegistrationRefuseBeforeGrant) {
    SimulationContext ctx;nativePool(ctx,1);Surface surface;
    PeerPassiveStorageFunding issuer(ctx,1,2,surface.map(),1/ft3,16);
    EXPECT_THROW(issuer.bindRecipient(ctx),std::exception);
    auto capability=issuer.bindRecipient(surface.ctx);EXPECT_THROW(issuer.bindRecipient(surface.ctx),std::exception);
    EXPECT_EQ(issuer.receipts().fundingAllowance(),nullptr);
}
TEST(PeerPassiveFunding, WrongSessionOfferIsNotAuthorityForAnotherRecipient) {
    Pair a,b;a.issuer->freeze(dt);a.issuer->observeDrain();auto offer=a.issuer->issue(Phase::ADVANCE,0,1000*dt);
    EXPECT_NE(a.issuer->identity().session,b.issuer->identity().session);
    const auto before=b.surface.state.transport.cell_mass;
    EXPECT_THROW(b.producer->begin(std::move(offer)),std::exception);EXPECT_EQ(before,b.surface.state.transport.cell_mass);
    EXPECT_TRUE(b.producer->failed());EXPECT_TRUE(b.issuer->failed());EXPECT_TRUE(a.issuer->unresolved());
}
TEST(PeerPassiveFunding, CorruptOfferIdentityBasisMapAmountFactorOrClockRefusesBeforeEffects) {
    for(int edit=0;edit<9;++edit) {
        SCOPED_TRACE(edit);Pair p;p.issuer->freeze(dt);p.issuer->observeDrain();auto offer=p.issuer->issue(Phase::ADVANCE,0,1000*dt);
        auto& view=const_cast<PeerFundingView&>(offer->view());
        if(edit==0)++view.allowance.peer.recipient;if(edit==1)++view.allowance.ordinal;
        if(edit==2)view.allowance.units=MassUnits::UG_PER_L;if(edit==3)view.points[0].cell=1;
        if(edit==4)view.allowance.pools[0].granted.add(1);if(edit==5)view.si_to_native+=1;
        if(edit==6)view.allowance.phase=Phase::FINAL;if(edit==7)view.allowance.end_ms+=1;
        if(edit==8)view.allowance.capture_ms+=1;
        const auto before=p.surface.state.transport.cell_mass;EXPECT_THROW(p.producer->begin(std::move(offer)),std::exception);
        EXPECT_EQ(before,p.surface.state.transport.cell_mass);EXPECT_EQ(p.producer->legCount(),0u);
        EXPECT_TRUE(p.issuer->unresolved());EXPECT_TRUE(p.issuer->receipts().fundingAllowance()->pools[0].spent.zero());
    }
}
TEST(PeerPassiveFunding, AdmissionIsBoundToItsActualSurfaceAndOperands) {
    Pair p;p.open();Surface wrong;
    const auto before=wrong.state.volume;EXPECT_THROW(wrong.march(*p.producer),std::exception);EXPECT_EQ(before,wrong.state.volume);
    EXPECT_EQ(p.producer->legCount(),0u);EXPECT_TRUE(p.producer->failed());
    Pair q;q.open();EXPECT_FALSE(q.producer->admit(0,0,0,.001,.001,123,1.5,0,dt));
    EXPECT_EQ(q.producer->legCount(),0u);
}
TEST(PeerPassiveFunding, SharedNodeSpendRefusesBeforeSecondCellCredit) {
    Pair p;p.open();auto spill=[&](int cell,double native) {
        const double water=native*ft3,before=p.surface.state.transport.cell_mass[cell],volume=p.surface.state.volume[cell];
        if(!p.producer->admit(cell,cell,0,-water,water,before,volume,0,dt,water))return false;
        p.surface.state.transport.cell_mass[cell]+=water;p.surface.state.volume[cell]+=water;
        p.producer->applied(p.surface.state.transport.cell_mass[cell],p.surface.state.volume[cell]);return true;
    };
    ASSERT_TRUE(spill(0,80));const auto before=p.surface.state.transport.cell_mass[1];EXPECT_FALSE(spill(1,21));
    EXPECT_EQ(p.surface.state.transport.cell_mass[1],before);EXPECT_EQ(p.producer->legCount(),1u);
    EXPECT_TRUE(p.producer->leg(0).applied);EXPECT_TRUE(p.issuer->receipts().fundingAllowance()->pools[0].spent.zero());
}
TEST(PeerPassiveFunding, LostReplyRetainsFullNetworkGrantAndActualSurfaceJournalSeparately) {
    Pair p;p.open();auto reply=p.march();ASSERT_TRUE(p.producer->leg(1).applied);reply.reset();
    p.issuer->fail("Deliberately dropped reply after native cell write.");
    const auto* grant=p.issuer->receipts().fundingAllowance();ASSERT_NE(grant,nullptr);
    EXPECT_EQ(grant->pools[0].granted.value(),100);EXPECT_TRUE(grant->pools[0].spent.zero());
    EXPECT_FALSE(grant->peer_reported);EXPECT_TRUE(grant->spills.empty());
    EXPECT_GT(p.producer->spend()[0].rounded(),0);EXPECT_TRUE(p.producer->awaitingAcknowledgement());
    EXPECT_TRUE(p.issuer->receipts().receipts().empty());EXPECT_TRUE(p.issuer->unresolved());
    EXPECT_THROW(p.issuer->issue(Phase::ADVANCE,0,1000*dt),std::exception);
    EXPECT_THROW(p.issuer->receipts().checkpoint(p.network),std::exception);EXPECT_EQ(p.producer->legCount(),2u);
}
TEST(PeerPassiveFunding, DuplicateReplyRetainsFirstDiagnosticAndPreparedMaterial) {
    Pair p;p.open();p.issuer->accept(p.march());const auto* first=p.issuer->returnedReply();ASSERT_NE(first,nullptr);
    const auto proof=first->proof();EXPECT_THROW(p.issuer->accept(nullptr),std::exception);
    EXPECT_EQ(p.issuer->returnedReply(),first);EXPECT_EQ(first->proof(),proof);
    EXPECT_TRUE(p.issuer->receipts().materialSealPrepared());EXPECT_TRUE(p.issuer->receipts().receipts().empty());
}
TEST(PeerPassiveFunding, WrongOrCorruptReplyCannotCommitOrReleaseGrant) {
    for(int edit=0;edit<5;++edit) {
        Pair p;p.open();auto reply=p.march();
        if(edit==0)const_cast<PeerFundingView&>(reply->view()).allowance.peer.session+=1;
        if(edit==1)const_cast<Book::LiveProof&>(reply->proof()).legs[1].mass_native+=1;
        if(edit==2)const_cast<std::vector<NativeAmountSum>&>(reply->spend())[0].add(1);
        if(edit==3)const_cast<std::vector<Book::Row>&>(reply->rows())[0].drain_mass+=1;
        if(edit==4)const_cast<Book::LiveProof&>(reply->proof()).points[0].cell+=1;
        EXPECT_THROW(p.issuer->accept(std::move(reply)),std::exception);
        EXPECT_TRUE(p.issuer->receipts().receipts().empty());EXPECT_NE(p.issuer->receipts().fundingAllowance(),nullptr);
        EXPECT_TRUE(p.issuer->failed());EXPECT_EQ(p.producer->legCount(),2u);
    }
}
TEST(PeerPassiveFunding, WrongWaterConfirmationAndMissingAcknowledgementRemainTerminal) {
    Pair bad;bad.open();bad.issuer->accept(bad.march());bad.network.nodes.coupling_queue[0]=1;
    EXPECT_THROW(bad.issuer->confirmWater(),std::exception);EXPECT_TRUE(bad.issuer->receipts().materialSealPrepared());
    EXPECT_TRUE(bad.issuer->receipts().receipts().empty());
    Pair lost;lost.open();lost.issuer->accept(lost.march());auto ack=lost.issuer->confirmWater();ack.reset();
    EXPECT_EQ(lost.issuer->receipts().receipts().size(),1u);EXPECT_TRUE(lost.issuer->unresolved());
    EXPECT_TRUE(lost.producer->awaitingAcknowledgement());lost.issuer->fail("Acknowledgement lost after owner commit.");
    EXPECT_THROW(lost.issuer->freeze(dt),std::exception);
    EXPECT_THROW(lost.issuer->receipts().checkpoint(lost.network),std::exception);
}
TEST(PeerPassiveFunding, EmptyWindowsUseOrdinalAndSealingRequiresCapturedClock) {
    Pair p;p.open();auto reply=p.producer->seal({0,0});p.issuer->accept(std::move(reply));
    auto ack=p.issuer->confirmWater();p.producer->acknowledge(std::move(ack));EXPECT_TRUE(p.issuer->receipts().receipts().empty());
    QualitySolver solver;solver.init(1,0,1);p.issuer->receive(solver,dt);
    hydraulics::TimestepController::advance(p.network,dt);hydraulics::TimestepController::advance(p.surface.ctx,dt);
    p.issuer->freeze(dt);p.issuer->observeDrain();auto next=p.issuer->issue(Phase::ADVANCE,1000*dt,2000*dt);
    EXPECT_EQ(next->view().allowance.ordinal,2u);p.producer->begin(std::move(next));
    hydraulics::TimestepController::advance(p.surface.ctx,dt);
    EXPECT_THROW(p.producer->seal({0,0}),std::exception);EXPECT_TRUE(p.issuer->unresolved());
}
TEST(PeerPassiveFunding, FollowingWindowReceivesRemainingGlobalJournalCapacity) {
    Pair p;p.open();p.issuer->accept(p.march());p.producer->acknowledge(p.issuer->confirmWater());
    QualitySolver solver;solver.init(1,0,1);p.issuer->receive(solver,dt);
    hydraulics::TimestepController::advance(p.network,dt);hydraulics::TimestepController::advance(p.surface.ctx,dt);
    p.network.save_lat_qual_state();p.issuer->freeze(dt);p.issuer->observeDrain();
    auto offer=p.issuer->issue(Phase::ADVANCE,1000*dt,2000*dt);
    EXPECT_EQ(offer->view().allowance.leg_limit,14u);EXPECT_EQ(offer->view().allowance.ordinal,2u);
    ASSERT_NO_THROW(p.producer->begin(std::move(offer)));EXPECT_EQ(p.producer->legCount(),2u);
}

using Recovery=PeerPassiveStorageCheckpoint;
void reenroll(Pair& p,std::size_t limit=16) {
    p.producer.reset();p.issuer.reset();
    p.issuer=std::make_unique<PeerPassiveStorageFunding>(p.network,11,22,p.surface.map(),1/ft3,limit);
    p.producer=std::make_unique<PeerPassiveStorageProducer>(p.surface.ctx,p.surface.state,
        p.issuer->bindRecipient(p.surface.ctx),1/ft3,limit);
}
void acceptedWindow(Pair& p,bool native=true) {
    p.open();p.issuer->accept(native?p.march():p.producer->seal({0,0}));
    p.producer->acknowledge(p.issuer->confirmWater());
    QualitySolver solver;solver.init(1,0,1);p.issuer->receive(solver,dt);
    hydraulics::TimestepController::advance(p.network,dt);
    hydraulics::TimestepController::advance(p.surface.ctx,dt);
    p.surface.ctx.nodes.conc=p.network.nodes.conc;
}
void receiveWindow(Pair& p) {
    p.network.save_lat_qual_state();p.issuer->freeze(dt);p.issuer->observeDrain();
    QualitySolver solver;solver.init(1,0,1);p.issuer->receive(solver,dt);
    hydraulics::TimestepController::advance(p.network,dt);
    hydraulics::TimestepController::advance(p.surface.ctx,dt);
    p.surface.ctx.nodes.conc=p.network.nodes.conc;
}
// This fixture supplies independently captured current numerical observations
// to the owner-only staging contract. It does not implement an engine or CPU
// checkpoint, and the restored fixture never marches its un-restored solver.
void installFixtureObservations(const Pair& source,Pair& target) {
    auto copy=[](const SimulationContext& a,SimulationContext& b) {
        b.nodes=a.nodes;b.elapsed_ms=a.elapsed_ms;b.old_elapsed_ms=a.old_elapsed_ms;
        b.current_time=a.current_time;b.current_date=a.current_date;b.next_report_ms=a.next_report_ms;
        b.coupling_delivery_remaining=a.coupling_delivery_remaining;b.mass_balance=a.mass_balance;
    };
    copy(source.network,target.network);copy(source.surface.ctx,target.surface.ctx);
    target.surface.state.volume=source.surface.state.volume;
    target.surface.state.transport.cell_mass=source.surface.state.transport.cell_mass;
    target.surface.published=source.surface.published;
}
std::string recovered(Pair& source,Pair& target,const std::string& bytes) {
    auto stage=Recovery::prepare(target.issuer,target.producer,source.network,source.surface.ctx,source.surface.state,bytes);
    stage.validateTarget();
    installFixtureObservations(source,target);stage.install();stage.install(); // one shot
    EXPECT_THROW(stage.validateTarget(),std::exception);
    EXPECT_NE(source.issuer->identity().session,target.issuer->identity().session);
    EXPECT_EQ(target.issuer->returnedReply(),nullptr); // no resurrected capability
    EXPECT_FALSE(target.issuer->failed());EXPECT_FALSE(target.producer->failed());
    EXPECT_THROW(target.issuer->receipts().checkpoint(target.network),std::exception);
    EXPECT_EQ(source.issuer->receipts().receipts(),target.issuer->receipts().receipts());
    EXPECT_EQ(source.producer->legCount(),target.producer->legCount());
    EXPECT_EQ(source.producer->attempted(),target.producer->attempted());
    EXPECT_EQ(source.producer->spend(),target.producer->spend());
    for(std::size_t k=0;k<source.producer->legCount();++k)EXPECT_EQ(source.producer->leg(k),target.producer->leg(k));
    EXPECT_EQ(source.issuer->receipts().storedRoundoff(0),target.issuer->receipts().storedRoundoff(0));
    return Recovery::save(*target.issuer,*target.producer);
}
TEST(PeerOwnerRecovery, InitialPendingSettledAndEmptyHistoriesBothSignsAllBases) {
    for(int basis=0;basis<3;++basis)for(bool positive:{false,true})for(int point=0;point<4;++point) {
        SCOPED_TRACE(std::to_string(basis)+"/"+std::to_string(positive)+"/"+std::to_string(point));
        const double nc=positive?1:2,sc=positive?2:1;
        Pair source(nc,sc),target(nc,sc);
        for(auto* p:{&source,&target}) {
            p->network.pollutants.units[0]=p->surface.ctx.pollutants.units[0]=static_cast<MassUnits>(basis);
            // Fixture's initial owners were already enrolled in MG/L.
            reenroll(*p);
        }
        if(point)acceptedWindow(source,point!=3);
        if(point==2)receiveWindow(source);
        const auto bytes=Recovery::save(*source.issuer,*source.producer);
        std::ofstream(evidence("recovery/"+std::to_string(basis)+"-"+std::to_string(positive)+"-"+std::to_string(point)+".owners"),std::ios::binary)<<bytes;
        const auto again=recovered(source,target,bytes);EXPECT_FALSE(again.empty());
        if(point==1) {
            receiveWindow(source);receiveWindow(target);
            EXPECT_EQ(source.network.nodes.conc,target.network.nodes.conc);
            EXPECT_EQ(source.issuer->receipts().receipts(),target.issuer->receipts().receipts());
            EXPECT_EQ(source.issuer->receipts().storedRoundoff(0),target.issuer->receipts().storedRoundoff(0));
        }
        if(point==3) {
            target.network.save_lat_qual_state();target.issuer->freeze(dt);target.issuer->observeDrain();
            auto offer=target.issuer->issue(Phase::ADVANCE,1000*dt,2000*dt);
            EXPECT_EQ(offer->view().allowance.ordinal,2u);EXPECT_EQ(offer->view().allowance.receipt,1u);
            EXPECT_EQ(offer->view().allowance.leg_limit,16u);EXPECT_NO_THROW(target.producer->begin(std::move(offer)));
        }
    }
}
TEST(PeerOwnerRecovery, UnacknowledgedFrozenAndTerminalStatesCannotBeSaved) {
    for(int point=0;point<8;++point) {
        SCOPED_TRACE(point);Pair p;
        if(point==0) {p.issuer->freeze(dt);p.issuer->observeDrain();}
        else if(point==1) {p.issuer->freeze(dt);p.issuer->observeDrain();auto lost=p.issuer->issue(Phase::ADVANCE,0,1000*dt);}
        else {
            p.open();
            if(point>=3) {auto reply=p.march();if(point>=4)p.issuer->accept(std::move(reply));}
            if(point>=5) {auto ack=p.issuer->confirmWater();if(point>=6)p.producer->acknowledge(std::move(ack));}
            if(point==7)p.issuer->fail("Deliberate terminal boundary.");
        }
        const auto count=p.producer->legCount();const auto before=p.surface.state.transport.cell_mass;
        EXPECT_THROW(Recovery::save(*p.issuer,*p.producer),std::exception);
        EXPECT_EQ(count,p.producer->legCount());EXPECT_EQ(before,p.surface.state.transport.cell_mass);
    }
}
TEST(PeerOwnerRecovery, BadArchiveAndIncomingStatePreserveBothTargetsAndPermitRetry) {
    Pair source;acceptedWindow(source);const auto bytes=Recovery::save(*source.issuer,*source.producer);
    for(int edit=0;edit<12;++edit) {
        SCOPED_TRACE(edit);Pair target;const auto before=Recovery::save(*target.issuer,*target.producer);
        const auto* issuer=target.issuer.get();const auto* producer=target.producer.get();auto bad=bytes;
        if(edit==0)bad.clear();if(edit==1)bad.pop_back();if(edit==2)bad.push_back('x');
        if(edit==3)bad[bad.size()/2]^=1;
        if(edit>=4 && edit<8) {
            using A=detail::CompletedSourceArchive;
            A in(bad);std::string magic,payload;std::uint32_t version,endian,word,crc;
            in(magic,version,endian,word,crc,payload);in.finish();
            if(edit==4)++version;if(edit==5)++endian;if(edit==6)++word;if(edit==7)magic="surface/network";
            A out;out(magic,version,endian,word,crc,payload);bad=out.bytes();
        }
        if(edit==8)source.surface.ctx.elapsed_ms+=1;
        if(edit==9)source.surface.points[0].cell_idx=1;
        if(edit==10)source.network.nodes.conc_old[0]+=1;
        if(edit==11)source.surface.ctx.nodes.conc[0]+=1;
        EXPECT_THROW(Recovery::prepare(target.issuer,target.producer,source.network,source.surface.ctx,source.surface.state,bad),std::exception);
        if(edit==8)source.surface.ctx.elapsed_ms-=1;
        if(edit==9)source.surface.points[0].cell_idx=0;
        if(edit==10)source.network.nodes.conc_old[0]-=1;
        if(edit==11)source.surface.ctx.nodes.conc[0]-=1;
        EXPECT_EQ(issuer,target.issuer.get());EXPECT_EQ(producer,target.producer.get());
        EXPECT_EQ(before,Recovery::save(*target.issuer,*target.producer));
        EXPECT_NO_THROW(Recovery::prepare(target.issuer,target.producer,source.network,source.surface.ctx,source.surface.state,bytes));
    }
}
TEST(PeerOwnerRecovery, InitialAnchorsCapacityAndMovedOrReplacedTargetsAreChecked) {
    Pair source;acceptedWindow(source);const auto bytes=Recovery::save(*source.issuer,*source.producer);
    Pair wrong_anchor(2,2),wrong_capacity(1,2,15);
    EXPECT_THROW(Recovery::prepare(wrong_anchor.issuer,wrong_anchor.producer,source.network,source.surface.ctx,source.surface.state,bytes),std::exception);
    EXPECT_THROW(Recovery::prepare(wrong_capacity.issuer,wrong_capacity.producer,source.network,source.surface.ctx,source.surface.state,bytes),std::exception);
    Pair target;
    auto a=Recovery::prepare(target.issuer,target.producer,source.network,source.surface.ctx,source.surface.state,bytes);
    const auto* pinned=target.issuer.get();a.install();EXPECT_EQ(pinned,target.issuer.get()); // no authorization
    auto b=std::move(a);EXPECT_THROW(a.validateTarget(),std::exception);a.install();EXPECT_NO_THROW(b.validateTarget());
    auto old=std::move(target.producer);EXPECT_THROW(b.validateTarget(),std::exception);target.producer=std::move(old);
    b.install();EXPECT_EQ(pinned,target.issuer.get()); // failed preflight revoked authorization
    EXPECT_NO_THROW(b.validateTarget());old=std::move(target.producer);b.install();EXPECT_EQ(pinned,target.issuer.get());
    target.producer=std::move(old);target.issuer->freeze(dt);EXPECT_THROW(b.validateTarget(),std::exception);
}
TEST(PeerOwnerRecovery, FreshTargetNodeSpeciesAndBasisMustMatchIncomingArchive) {
    Pair source;acceptedWindow(source);const auto bytes=Recovery::save(*source.issuer,*source.producer);
    for(int edit=0;edit<4;++edit) {
        Pair target;
        for(auto* ctx:{&target.network,&target.surface.ctx}) {
            if(edit==0)ASSERT_TRUE(ctx->node_names.rename(0,"OtherTank"));
            if(edit==1)ASSERT_TRUE(ctx->pollutant_names.rename(0,"OtherDye"));
            if(edit>=2)ctx->pollutants.units[0]=static_cast<MassUnits>(edit-1);
        }
        reenroll(target);const auto before=Recovery::save(*target.issuer,*target.producer);
        const auto* issuer=target.issuer.get();const auto* producer=target.producer.get();
        EXPECT_THROW(Recovery::prepare(target.issuer,target.producer,source.network,source.surface.ctx,source.surface.state,bytes),std::exception);
        EXPECT_EQ(issuer,target.issuer.get());EXPECT_EQ(producer,target.producer.get());
        EXPECT_EQ(before,Recovery::save(*target.issuer,*target.producer));
    }
}
TEST(PeerOwnerRecovery, EmptyPublicationAfterRealLegsPreservesAttemptAndResetsLatestSpend) {
    Pair source,target;acceptedWindow(source);receiveWindow(source);
    source.surface.published=source.network.nodes.conc;
    source.producer->begin(source.issuer->issue(Phase::REPORT,1000*dt,2000*dt));
    source.issuer->accept(source.producer->seal({0,0}));source.producer->acknowledge(source.issuer->confirmWater());
    ASSERT_EQ(source.producer->legCount(),2u);EXPECT_TRUE(source.producer->spend()[0].zero());
    const auto bytes=Recovery::save(*source.issuer,*source.producer);recovered(source,target,bytes);
    target.surface.published=target.network.nodes.conc;
    // The next empty report proves restored first_ is count, not the last nonempty receipt's start.
    hydraulics::TimestepController::advance(target.network,dt);hydraulics::TimestepController::advance(target.surface.ctx,dt);
    auto offer=target.issuer->issue(Phase::REPORT,2000*dt,3000*dt);
    EXPECT_EQ(offer->view().allowance.ordinal,3u);EXPECT_EQ(offer->view().allowance.leg_limit,14u);
    target.producer->begin(std::move(offer));auto reply=target.producer->seal({0,0});EXPECT_TRUE(reply->proof().legs.empty());
    target.issuer->accept(std::move(reply));target.producer->acknowledge(target.issuer->confirmWater());
    EXPECT_NO_THROW(Recovery::save(*target.issuer,*target.producer));
}
TEST(PeerOwnerRecovery, IndependentRestoresRejectSiblingOfferBeforeAnyCellEffect) {
    Pair source;acceptedWindow(source);const auto bytes=Recovery::save(*source.issuer,*source.producer);
    Pair a,b;recovered(source,a,bytes);recovered(source,b,bytes);
    EXPECT_NE(a.issuer->identity().session,b.issuer->identity().session);
    a.network.save_lat_qual_state();a.issuer->freeze(dt);a.issuer->observeDrain();
    auto offer=a.issuer->issue(Phase::ADVANCE,1000*dt,2000*dt);
    const auto water=b.surface.state.volume,mass=b.surface.state.transport.cell_mass;
    EXPECT_THROW(b.producer->begin(std::move(offer)),std::exception);
    EXPECT_EQ(water,b.surface.state.volume);EXPECT_EQ(mass,b.surface.state.transport.cell_mass);
    EXPECT_EQ(b.producer->legCount(),2u);EXPECT_TRUE(b.producer->failed());
}
TEST(PeerOwnerRecovery, RestoredSiblingReplyAndAcknowledgementCannotRedeemNativeAuthority) {
    Pair source;acceptedWindow(source);const auto bytes=Recovery::save(*source.issuer,*source.producer);
    for(bool ack:{false,true}) {
        Pair a,b;recovered(source,a,bytes);recovered(source,b,bytes);
        for(auto* p:{&a,&b}) {
            p->network.save_lat_qual_state();p->issuer->freeze(dt);p->issuer->observeDrain();
            p->producer->begin(p->issuer->issue(Phase::ADVANCE,1000*dt,2000*dt));
        }
        auto reply_a=a.producer->seal({0,0}),reply_b=b.producer->seal({0,0});
        if(!ack) {
            const auto before=b.issuer->receipts().receipts();
            EXPECT_THROW(b.issuer->accept(std::move(reply_a)),std::exception);
            EXPECT_EQ(before,b.issuer->receipts().receipts());EXPECT_TRUE(b.issuer->unresolved());
        } else {
            a.issuer->accept(std::move(reply_a));b.issuer->accept(std::move(reply_b));
            auto ack_a=a.issuer->confirmWater(),ack_b=b.issuer->confirmWater();
            const auto count=b.producer->legCount();
            EXPECT_THROW(b.producer->acknowledge(std::move(ack_a)),std::exception);
            EXPECT_EQ(count,b.producer->legCount());EXPECT_TRUE(b.producer->awaitingAcknowledgement());
        }
        EXPECT_TRUE(b.issuer->failed());EXPECT_EQ(b.surface.state.transport.cell_mass,source.surface.state.transport.cell_mass);
    }
}
TEST(PeerOwnerRecovery, RechecksummedWrongPayloadsCannotChangeFreshOwners) {
    using A=detail::CompletedSourceArchive;
    Pair source;acceptedWindow(source);const auto bytes=Recovery::save(*source.issuer,*source.producer);
    auto checksum=[](const std::string& b) {
        std::uint32_t value=~std::uint32_t(0);
        for(unsigned char c:b){value^=c;for(int i=0;i<8;++i)value=(value>>1)^((value&1)?0xedb88320u:0u);}
        return ~value;
    };
    for(int edit=0;edit<8;++edit) {
        Pair target;const auto before=Recovery::save(*target.issuer,*target.producer);
        A envelope(bytes);std::string magic,payload;std::uint32_t version,endian,word,crc;
        envelope(magic,version,endian,word,crc,payload);envelope.finish();
        A in(payload);std::uint64_t issuer,recipient,session,capacity,count;double factor;
        in(issuer,recipient,session,factor,capacity,count);std::vector<Book::HydraulicPoint> points(count);
        for(auto& p:points)in(p.point,p.cell,p.node,p.exchange_si);std::string book;in(book);in.finish();
        if(edit==0)session=0;if(edit==1)issuer=recipient;if(edit==2)factor+=1;if(edit==3)--capacity;
        if(edit==4)++points[0].cell;if(edit==5)points[0].exchange_si=1;if(edit==6)++count;
        if(edit==7)book.pop_back();
        A body;body(issuer,recipient,session,factor,capacity,count);
        for(auto p:points)body(p.point,p.cell,p.node,p.exchange_si);body(book);
        payload=body.bytes();crc=checksum(payload);A out;out(magic,version,endian,word,crc,payload);
        EXPECT_THROW(Recovery::prepare(target.issuer,target.producer,source.network,source.surface.ctx,source.surface.state,out.bytes()),std::exception);
        EXPECT_EQ(before,Recovery::save(*target.issuer,*target.producer));
        EXPECT_NO_THROW(Recovery::prepare(target.issuer,target.producer,source.network,source.surface.ctx,source.surface.state,bytes));
    }
}
#if defined(__APPLE__) || defined(__linux__)
TEST(PeerOwnerRecovery, ForkedRecoveryDoesNotReuseInheritedSessionCounter) {
    Pair source,target;acceptedWindow(source);const auto bytes=Recovery::save(*source.issuer,*source.producer);
    std::vector<pid_t> children;
    std::vector<std::filesystem::path> files;
    for(int i=0;i<2;++i) {
        const auto file=evidence("recovery/fork-"+std::to_string(i)+".txt");files.push_back(file);
        const auto child=fork();
        if(child==0) {
            try {
                auto stage=Recovery::prepare(target.issuer,target.producer,source.network,source.surface.ctx,source.surface.state,bytes);
                stage.validateTarget();installFixtureObservations(source,target);stage.install();
                std::ofstream out(file);out<<target.issuer->identity().session<<'\n';out.close();
                _exit(out?0:2);
            }catch(...){_exit(3);}
        }
        EXPECT_GT(child,0);if(child>0)children.push_back(child);
    }
    for(auto child:children){int status=0;ASSERT_EQ(waitpid(child,&status,0),child);EXPECT_TRUE(WIFEXITED(status));EXPECT_EQ(WEXITSTATUS(status),0);}
    ASSERT_EQ(children.size(),2u);std::vector<std::uint64_t> ids;
    for(const auto& file:files){std::ifstream in(file);std::uint64_t id=0;in>>id;ASSERT_GT(id,0u);ids.push_back(id);}
    EXPECT_NE(ids[0],ids[1]);EXPECT_NE(ids[0],source.issuer->identity().session);EXPECT_NE(ids[1],source.issuer->identity().session);
}
#endif
#ifdef PEER_RECOVERY_PACKET_TESTS
TEST(PeerOwnerRecovery, FreshProcessPacketChild) {
    const char* directory=std::getenv("OPENSWMM_PEER_RECOVERY_CHILD");
    if(!directory)GTEST_SKIP()<<"Invoked separately by the fresh-process qualification driver.";
    const auto dir=std::filesystem::path(directory);std::filesystem::create_directories(dir);
    Pair source;acceptedWindow(source);auto bytes=Recovery::save(*source.issuer,*source.producer);
    if(const char* input=std::getenv("OPENSWMM_PEER_RECOVERY_ARCHIVE")) {
        std::ifstream in(input,std::ios::binary);bytes={std::istreambuf_iterator<char>(in),{}};
    }
    std::ofstream(dir/"source.owners",std::ios::binary)<<bytes;
    Pair target;recovered(source,target,bytes);
    const auto id=target.issuer->identity();std::ofstream(dir/"identity.txt")<<id.issuer<<' '<<id.recipient<<' '<<id.session<<'\n';
    target.network.save_lat_qual_state();target.issuer->freeze(dt);target.issuer->observeDrain();
    auto offer=target.issuer->issue(Phase::ADVANCE,1000*dt,2000*dt);const auto view=offer->view();
    auto packet=[&](int k,const std::string& issued) {
        std::ofstream(dir/(std::to_string(k)+".json"))<<issued;
        ASSERT_NO_THROW(matchMaterialPacket(issued,issued.data(),issued.size(),target.network));
        if(const char* sibling=std::getenv("OPENSWMM_PEER_RECOVERY_SIBLING")) {
            std::ifstream in(std::filesystem::path(sibling)/(std::to_string(k)+".json"));
            const std::string stale{std::istreambuf_iterator<char>(in),{}};
            ASSERT_FALSE(stale.empty());
            const auto before=target.surface.state.transport.cell_mass;
            const auto receipts=target.issuer->receipts().receipts();const auto count=target.producer->legCount();
            EXPECT_THROW(matchMaterialPacket(issued,stale.data(),stale.size(),target.network),std::exception);
            EXPECT_EQ(before,target.surface.state.transport.cell_mass);EXPECT_EQ(count,target.producer->legCount());
            EXPECT_EQ(receipts,target.issuer->receipts().receipts());
            EXPECT_NO_THROW(matchMaterialPacket(issued,issued.data(),issued.size(),target.network));
        }
    };
    ASSERT_NO_FATAL_FAILURE(packet(0,materialOfferPacket(view,2,2))); // before credit
    target.producer->begin(std::move(offer));auto reply=target.producer->seal({0,0});
    ASSERT_NO_FATAL_FAILURE(packet(1,materialProofPacket(*reply,2,2,target.network.nodes.coupling_queue,{0},0))); // before commit
    target.issuer->accept(std::move(reply));auto ack=target.issuer->confirmWater();
    ASSERT_NO_FATAL_FAILURE(packet(2,materialAcknowledgementPacket(view,2,2,target.network.nodes.coupling_queue,0)));
    EXPECT_TRUE(target.producer->awaitingAcknowledgement());target.producer->acknowledge(std::move(ack));
}
#endif
}
