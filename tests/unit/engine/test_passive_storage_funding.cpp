// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include <gtest/gtest.h>
#include "quality/LivePassiveStorageExchange.hpp"
#include "quality/QualityRouting.hpp"
#include "core/SimulationContext.hpp"
#include "2d/coupling/NodeCoupling.hpp"
#include <memory>

namespace {
using namespace openswmm;
using Book = quality::PassiveStorageExchange;
using Phase = Book::Phase;
// Manufactured owner-level observations. Authored engine tests independently
// qualify the real marcher, water booking, quality writer and restart bytes.
struct FundingFixture {
    SimulationContext ctx;
    twoD::SurfaceStateData surface;
    std::vector<twoD::CouplingPoint> points{{0,-1,0,.7,.01,false,false},{1,-1,0,.7,.01,false,false}};
    std::vector<double> published{1};
    std::unique_ptr<quality::LivePassiveStorageExchange> owner;
    explicit FundingFixture(bool preflight=false) {
        ctx.node_names.add("Tank");ctx.pollutant_names.add("Dye");ctx.allocate_objects();
        ctx.options.routing_model=RoutingModel::DYNWAVE;
        ctx.node_subtypes.set_node_type(ctx.nodes,0,NodeType::STORAGE);
        ctx.nodes.volume[0]=ctx.nodes.old_volume[0]=100;ctx.nodes.depth[0]=ctx.nodes.old_depth[0]=1;
        ctx.nodes.conc[0]=ctx.nodes.conc_old[0]=1;
        surface.resize(2,6);surface.transport.resize(1,2,2);surface.transport.n_pollut=1;
        surface.nodes_1d=&ctx.nodes;surface.node_row_conc=&published;surface.node_coupling=&points;
        owner=std::make_unique<quality::LivePassiveStorageExchange>(ctx,surface,points,1,16,
            quality::PassiveStorageArithmetic::RETAIN_EXACT_SLICES,preflight,quality::PassiveStorageLaw::CHANGING_VOLUME,
            quality::PassiveStorageWaterArithmetic::RETAIN_AGGREGATES);
    }
    void begin(Phase phase=Phase::REPORT,double start=0,double end=1000) {
        ctx.elapsed_ms=end;owner->beginWindow(ctx,surface,start,end,phase);
    }
    bool spill(int point,double amount) {
        const double before=surface.transport.cell_mass[point],volume=surface.volume[point];
        if(!owner->admit(point,point,0,-amount,amount,before,volume,0,1,amount))return false;
        surface.transport.cell_mass[point]+=amount;surface.volume[point]+=amount;
        owner->applied(surface.transport.cell_mass[point],surface.volume[point]);return true;
    }
    void confirm(double net,double span) {
        ctx.nodes.coupling_queue[0]+=net;ctx.coupling_delivery_remaining=span;
        owner->observeWaterBooking(ctx);
    }
};
}

TEST(PassiveStorageFunding, SharedNodeOverspendRefusesBeforeSecondCreditAndRetainsGrant) {
    FundingFixture f;ASSERT_NO_THROW(f.begin());
    const auto* grant=f.owner->receipts().fundingAllowance();ASSERT_NE(grant,nullptr);
    ASSERT_EQ(grant->pools.size(),1u);EXPECT_EQ(grant->pools[0].granted.value(),100);
    ASSERT_TRUE(f.spill(0,80));EXPECT_FALSE(f.spill(1,21));
    EXPECT_EQ(f.surface.transport.cell_mass[1],0);EXPECT_EQ(f.surface.volume[1],0);
    EXPECT_EQ(f.owner->legCount(),1u);EXPECT_TRUE(f.owner->leg(0).applied);
    EXPECT_EQ(grant->pools[0].spent.value(),80);EXPECT_EQ(grant->spills.size(),1u);
    EXPECT_TRUE(f.owner->failed());EXPECT_TRUE(f.owner->receipts().receipts().empty());
    EXPECT_EQ(f.owner->attempted().mass_native,21);EXPECT_FALSE(f.owner->attempted().applied);
    EXPECT_THROW(f.owner->receipts().checkpoint(f.ctx),std::exception);
}

TEST(PassiveStorageFunding, DrainsDoNotReplenishThePreCreditAllowance) {
    FundingFixture f;f.surface.volume[0]=f.surface.transport.cell_mass[0]=200;f.begin();
    ASSERT_TRUE(f.owner->admit(0,0,0,200,200,200,200,0,1));
    f.surface.volume[0]=f.surface.transport.cell_mass[0]=0;f.owner->applied(0,0);
    EXPECT_FALSE(f.spill(1,101));EXPECT_EQ(f.surface.transport.cell_mass[1],0);
    EXPECT_EQ(f.owner->receipts().fundingAllowance()->pools[0].spent.value(),0);
}

TEST(PassiveStorageFunding, MaterialAndWaterCommitTogetherThenDuplicateConfirmationRefuses) {
    FundingFixture f;f.begin();ASSERT_TRUE(f.spill(0,10));ASSERT_NO_THROW(f.owner->seal(f.ctx,{-10,0}));
    const auto& book=f.owner->receipts();
    EXPECT_TRUE(book.receipts().empty());EXPECT_TRUE(book.materialSealPrepared());
    ASSERT_NE(book.fundingAllowance(),nullptr);EXPECT_TRUE(book.fundingAllowance()->reconciled);
    EXPECT_THROW(book.checkpoint(f.ctx),std::exception);
    ASSERT_NO_THROW(f.confirm(-10,1));
    ASSERT_EQ(book.receipts().size(),1u);EXPECT_EQ(book.receipts()[0].id,1u);
    EXPECT_EQ(book.receipts()[0].publication.remaining,0); // pre-booking span
    EXPECT_EQ(book.pendingSpillAmount(0).value(),10);EXPECT_FALSE(book.materialSealPrepared());
    EXPECT_EQ(book.fundingAllowance(),nullptr);const auto saved=book.checkpoint(f.ctx);
    EXPECT_THROW(f.owner->observeWaterBooking(f.ctx),std::exception);
    EXPECT_EQ(book.receipts().size(),1u);EXPECT_EQ(book.checkpoint(f.ctx),saved);
}

TEST(PassiveStorageFunding, InvalidOrMissingWaterConfirmationRetainsOldBookAndUnresolvedAuthority) {
    for(int failure=0;failure<3;++failure) {
        SCOPED_TRACE(failure);FundingFixture f;f.begin();ASSERT_TRUE(f.spill(0,10));
        ASSERT_NO_THROW(f.owner->seal(f.ctx,{-10,0}));
        if(failure==0)f.ctx.coupling_delivery_remaining=1; // queue missing
        if(failure==1)f.ctx.nodes.coupling_queue[0]=-10; // span missing
        if(failure==2){f.ctx.nodes.coupling_queue[0]=-10;f.ctx.coupling_delivery_remaining=1;f.ctx.nodes.coupling_volume[0]=1;}
        EXPECT_THROW(f.owner->observeWaterBooking(f.ctx),std::exception);
        const auto& book=f.owner->receipts();EXPECT_TRUE(book.receipts().empty());
        EXPECT_TRUE(book.materialSealPrepared());ASSERT_NE(book.fundingAllowance(),nullptr);
        EXPECT_EQ(book.fundingAllowance()->pools[0].spent.value(),10);
        EXPECT_EQ(book.fundingAllowance()->pools[0].granted.value(),100);
        EXPECT_EQ(f.owner->legCount(),1u);EXPECT_EQ(f.surface.transport.cell_mass[0],10);
        EXPECT_THROW(book.checkpoint(f.ctx),std::exception);
        EXPECT_THROW(f.owner->beginWindow(f.ctx,f.surface,1000,2000,Phase::FINAL),std::exception);
        quality::QualitySolver solver;solver.init(1,0,1);
        EXPECT_THROW(f.owner->receive(f.ctx,solver,1),std::exception);
    }
}

TEST(PassiveStorageFunding, OutstandingGrantBlocksQualityAndAnotherWindow) {
    FundingFixture preflight(true);preflight.begin();
    EXPECT_FALSE(preflight.owner->preflightHistory(preflight.ctx,false));
    EXPECT_FALSE(preflight.owner->failed());ASSERT_NE(preflight.owner->receipts().fundingAllowance(),nullptr);
    FundingFixture f;f.begin();quality::QualitySolver solver;solver.init(1,0,1);
    EXPECT_THROW(solver.execute(f.ctx,1,const_cast<Book&>(f.owner->receipts())),std::exception);
    EXPECT_TRUE(f.owner->receipts().receipts().empty());
    ASSERT_NE(f.owner->receipts().fundingAllowance(),nullptr);
    EXPECT_THROW(f.owner->beginWindow(f.ctx,f.surface,0,1000,Phase::REPORT),std::exception);
}

TEST(PassiveStorageFunding, MissingWindowAndStaleClockRefuseWithoutPublishing) {
    FundingFixture missing;
    EXPECT_THROW(missing.owner->seal(missing.ctx,{0,0}),std::exception);
    EXPECT_EQ(missing.owner->receipts().fundingAllowance(),nullptr);
    EXPECT_TRUE(missing.owner->receipts().receipts().empty());
    FundingFixture stale;stale.begin();ASSERT_TRUE(stale.spill(0,10));stale.owner->seal(stale.ctx,{-10,0});
    stale.ctx.nodes.coupling_queue[0]=-10;stale.ctx.coupling_delivery_remaining=1;stale.ctx.elapsed_ms+=1;
    EXPECT_THROW(stale.owner->observeWaterBooking(stale.ctx),std::exception);
    EXPECT_TRUE(stale.owner->receipts().receipts().empty());
    EXPECT_TRUE(stale.owner->receipts().materialSealPrepared());
    ASSERT_NE(stale.owner->receipts().fundingAllowance(),nullptr);
    FundingFixture forged;forged.begin();ASSERT_TRUE(forged.spill(0,10));
    // A self-consistent smaller proof still cannot replace the operands that
    // consumed funding before the actual ten-unit surface credit.
    auto& leg=const_cast<Book::AppliedLeg&>(forged.owner->leg(0));
    leg.water_si=leg.mass_si=leg.water_native=leg.mass_native=9;
    leg.after=leg.volume_after=leg.withdrawal_si=9;
    EXPECT_THROW(forged.owner->seal(forged.ctx,{-9,0}),std::exception);
    EXPECT_TRUE(forged.owner->receipts().receipts().empty());
    EXPECT_FALSE(forged.owner->receipts().materialSealPrepared());
    ASSERT_NE(forged.owner->receipts().fundingAllowance(),nullptr);
    EXPECT_EQ(forged.owner->receipts().fundingAllowance()->pools[0].spent.value(),10);
    EXPECT_EQ(forged.surface.transport.cell_mass[0],10);
}

TEST(PassiveStorageFunding, EmptyWindowsAdvanceOrdinalWithoutInventingReceiptAndReplay) {
    FundingFixture f;quality::QualitySolver solver;solver.init(1,0,1);
    for(std::uint64_t ordinal=1;ordinal<=2;++ordinal) {
        f.owner->freeze(f.ctx,1);f.owner->observeDrain(f.ctx);
        f.owner->beginWindow(f.ctx,f.surface,1000*(ordinal-1),1000*ordinal,Phase::ADVANCE);
        const auto* grant=f.owner->receipts().fundingAllowance();ASSERT_NE(grant,nullptr);
        EXPECT_EQ(grant->ordinal,ordinal);EXPECT_EQ(grant->receipt,1u);
        ASSERT_NO_THROW(f.owner->seal(f.ctx,{0,0}));
        ASSERT_NO_THROW(f.confirm(0,0));
        EXPECT_EQ(f.owner->receipts().fundingAllowance(),nullptr);
        EXPECT_TRUE(f.owner->receipts().receipts().empty());
        f.owner->receive(f.ctx,solver,1);f.ctx.elapsed_ms=1000*ordinal;
    }
    const auto saved=f.owner->receipts().checkpoint(f.ctx);FundingFixture target;target.ctx.elapsed_ms=2000;
    auto& book=const_cast<Book&>(target.owner->receipts());ASSERT_NO_THROW(book.restore(target.ctx,saved));
    EXPECT_EQ(book.checkpoint(target.ctx),saved);
    target.owner->freeze(target.ctx,1);target.owner->observeDrain(target.ctx);
    target.owner->beginWindow(target.ctx,target.surface,2000,3000,Phase::ADVANCE);ASSERT_NE(book.fundingAllowance(),nullptr);
    EXPECT_EQ(book.fundingAllowance()->ordinal,3u);EXPECT_EQ(book.fundingAllowance()->receipt,1u);
}

TEST(PassiveStorageFunding, PriorSealedSpillRemainsEncumberedBeforeNextQualityDelivery) {
    FundingFixture f;f.begin();ASSERT_TRUE(f.spill(0,80));f.owner->seal(f.ctx,{-80,0});f.confirm(-80,1);
    f.owner->freeze(f.ctx,1);
    f.ctx.nodes.coupling_inflow[0]=f.ctx.nodes.lat_flow[0]=-80;
    f.ctx.nodes.coupling_queue[0]=0;f.ctx.coupling_delivery_remaining=0;f.owner->observeDrain(f.ctx);
    f.ctx.nodes.outflow[0]=80;
    f.owner->beginWindow(f.ctx,f.surface,1000,2000,Phase::ADVANCE);
    const auto* grant=f.owner->receipts().fundingAllowance();ASSERT_NE(grant,nullptr);
    EXPECT_EQ(grant->ordinal,2u);EXPECT_EQ(grant->receipt,2u);
    EXPECT_EQ(grant->pools[0].sealed.value(),80);EXPECT_EQ(grant->pools[0].granted.value(),20);
    EXPECT_FALSE(f.spill(1,21));EXPECT_EQ(f.surface.transport.cell_mass[1],0);
    EXPECT_EQ(f.owner->receipts().receipts().size(),1u);
}
