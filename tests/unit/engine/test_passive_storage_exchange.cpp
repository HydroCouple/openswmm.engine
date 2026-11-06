// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include <gtest/gtest.h>
#include "core/SimulationContext.hpp"
#include "quality/QualityRouting.hpp"
#include "quality/PassiveStorageExchange.hpp"
#include "core/CompletedSourceArchive.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <random>

namespace {
using namespace openswmm;
using quality::PassiveStorageExchange;
using quality::QualitySolver;
using Row = PassiveStorageExchange::Row;
constexpr double ft3_to_m3 = 0.028316846592;

SimulationContext closedPool(int nodes = 1, double volume = 100, double concentration = 5) {
    SimulationContext c;
    for (int i = 0; i < nodes; ++i)
        c.node_names.add("Storage" + std::to_string(i));
    c.pollutant_names.add("Dye");
    c.allocate_objects();
    c.mass_balance.resize_quality(1);
    c.options.routing_model = RoutingModel::DYNWAVE;
    for (int i = 0; i < nodes; ++i) {
        c.node_subtypes.set_node_type(c.nodes, i, NodeType::STORAGE);
        c.nodes.volume[i] = c.nodes.old_volume[i] = volume;
        c.nodes.depth[i] = c.nodes.old_depth[i] = 1;
        c.nodes.conc[i] = c.nodes.conc_old[i] = concentration;
    }
    return c;
}
Row row(double drain, double spill, double drain_mass, double spill_mass,
        std::uint64_t point = 0, int node = 0, MassUnits units = MassUnits::MG_PER_L) {
    return {point, node, "Dye", units, drain, spill, drain_mass, spill_mass};
}

// Manufactured accepted hydraulic state: the native caller rolls old quality,
// drains the existing native span, runs quality once, then owns the clock tick.
// No surface/water solver or new delivery clock is implemented by the book.
void nativeStep(SimulationContext& c, QualitySolver& solver, PassiveStorageExchange& book, double dt) {
    c.nodes.conc_old = c.nodes.conc;
    c.coupling_delivery_remaining = std::max(0.0, c.coupling_delivery_remaining - dt);
    const double start = c.elapsed_ms;
    const double remaining = c.coupling_delivery_remaining;
    solver.execute(c, dt, book);
    EXPECT_DOUBLE_EQ(c.elapsed_ms, start);
    EXPECT_DOUBLE_EQ(c.coupling_delivery_remaining, remaining);
    c.old_elapsed_ms = start;
    c.elapsed_ms = start + 1000.0 * dt;
}

// Actual staged surface inventory in its native C*m3 basis. Drain amounts are
// authenticated against this fixture's donor, not against the live marcher.
struct SurfaceDonor {
    twoD::SurfaceStateData state;
    explicit SurfaceDonor(double concentration) {
        state.volume.assign(1, 100 * ft3_to_m3);
        state.transport.resize(1, 1, 1);
        state.transport.cell_mass[0] = concentration * state.volume[0];
    }
    double mass() const { return state.transport.cell_mass[0] / ft3_to_m3; }
    void post(const Row& r) {
        ASSERT_NEAR(r.drain_mass, r.drain_water * state.transport.cell_mass[0] / state.volume[0], 1e-12);
        ASSERT_GE(state.transport.cell_mass[0], r.drain_mass * ft3_to_m3);
        state.transport.cell_mass[0] -= r.drain_mass * ft3_to_m3;
        state.volume[0] -= r.drain_water * ft3_to_m3;
        state.transport.cell_mass[0] += r.spill_mass * ft3_to_m3;
        state.volume[0] += r.spill_water * ft3_to_m3;
    }
};
double combined(const SimulationContext& c, const SurfaceDonor& surface,
                const PassiveStorageExchange& book) {
    return c.nodes.conc[0] * c.nodes.volume[0] + surface.mass() +
           book.pendingDrain(0) - book.pendingSpill(0);
}
} // namespace

TEST(PassiveStorageExchange, BothSignsUseNativeMixingWithZeroNativeDeliverySpan) {
    for (const auto basis : {MassUnits::MG_PER_L, MassUnits::UG_PER_L, MassUnits::COUNTS_PER_L}) {
        for (const double drain_mass : {4., 6.}) {
            SCOPED_TRACE(static_cast<int>(basis));
            SCOPED_TRACE(drain_mass);
            auto c = closedPool(); c.pollutants.units[0] = basis;
            QualitySolver solver; solver.init(1, 0, 1);
            PassiveStorageExchange book;
            SurfaceDonor surface(drain_mass);
            const double initial = 500 + surface.mass();
            const auto receipt = row(1, 1, drain_mass, 5, 0, 0, basis);
            book.reserve(c, 1, {receipt});
            // Reservation encumbers existing node mass; it is not a new store.
            EXPECT_DOUBLE_EQ(c.nodes.conc[0] * c.nodes.volume[0], 500);
            EXPECT_DOUBLE_EQ(book.pendingSpill(0), 5);
            surface.post(receipt);
            EXPECT_NEAR(combined(c, surface, book), initial, 1e-12);
            nativeStep(c, solver, book, 1);
            EXPECT_DOUBLE_EQ(c.nodes.conc[0] * c.nodes.volume[0], drain_mass == 4 ? 499 : 501);
            EXPECT_NEAR(combined(c, surface, book), initial, 1e-12);
            EXPECT_DOUBLE_EQ(book.pendingDrain(0), 0);
            EXPECT_DOUBLE_EQ(book.pendingSpill(0), 0);
            EXPECT_DOUBLE_EQ(c.nodes.coupling_qual_queue[0], 0);
            EXPECT_DOUBLE_EQ(c.nodes.coupling_qual_inflow[0], 0);
            EXPECT_DOUBLE_EQ(c.nodes.qual_mass_in[0], 0);
            EXPECT_DOUBLE_EQ(c.mass_balance.qual_routing_ex_in[0], 0);
            EXPECT_DOUBLE_EQ(c.nodes.volume[0], 100);
            EXPECT_DOUBLE_EQ(c.coupling_delivery_remaining, 0);
            EXPECT_FALSE(book.failed());
        }
    }
}

TEST(PassiveStorageExchange, PartialNativeSpanPreservesGrossPendingAndFinalRemainders) {
    auto c = closedPool(); c.coupling_delivery_remaining = 4;
    QualitySolver solver; solver.init(1, 0, 1);
    PassiveStorageExchange book;
    SurfaceDonor surface(4);
    const auto receipt = row(1, 1, 4, 5);
    book.reserve(c, 1, {receipt}); surface.post(receipt);
    for (double dt : {1., 1.5, 2.}) {
        nativeStep(c, solver, book, dt);
        EXPECT_NEAR(combined(c, surface, book), 900, 1e-12);
    }
    EXPECT_DOUBLE_EQ(c.nodes.conc[0] * c.nodes.volume[0], 499);
    const auto& pending = book.receipts()[0].remaining[0];
    EXPECT_DOUBLE_EQ(pending.drain_water, 0); EXPECT_DOUBLE_EQ(pending.spill_water, 0);
    EXPECT_DOUBLE_EQ(pending.drain_mass, 0); EXPECT_DOUBLE_EQ(pending.spill_mass, 0);
    EXPECT_DOUBLE_EQ(book.receipts()[0].accepted[0].drain_mass, 4);
    EXPECT_DOUBLE_EQ(book.receipts()[0].accepted[0].spill_mass, 5);
}

TEST(PassiveStorageExchange, IncomingDrainCannotFundGrossSpillOrPriorReservations) {
    auto c = closedPool(1, 10, 1); c.coupling_delivery_remaining = 4;
    QualitySolver solver; solver.init(1, 0, 1);
    PassiveStorageExchange book;
    book.reserve(c, 1, {row(8, 8, 0, 8)});
    EXPECT_THROW(book.reserve(c, 2, {row(3, 3, 100, 3, 1)}), std::invalid_argument);
    EXPECT_EQ(book.receipts().size(), 1u); EXPECT_DOUBLE_EQ(book.pendingSpill(0), 8);
    EXPECT_DOUBLE_EQ(c.nodes.conc[0] * c.nodes.volume[0], 10);
    nativeStep(c, solver, book, 2);
    EXPECT_DOUBLE_EQ(c.nodes.conc[0] * c.nodes.volume[0], 6);
    EXPECT_DOUBLE_EQ(book.pendingSpill(0), 4);
    // Current C=.6: 5 ft3 requests 3 units, exceeding free mass 6-4=2.
    EXPECT_THROW(book.reserve(c, 2, {row(5, 5, 100, 3, 1)}), std::invalid_argument);
    EXPECT_DOUBLE_EQ(book.pendingSpill(0), 4); EXPECT_EQ(book.receipts().size(), 1u);
    EXPECT_FALSE(book.failed());
    nativeStep(c, solver, book, 2);
    EXPECT_DOUBLE_EQ(c.nodes.conc[0] * c.nodes.volume[0], 2);
    EXPECT_DOUBLE_EQ(book.pendingSpill(0), 0);
}

TEST(PassiveStorageExchange, SharedPointAdmissionAndPostingAreIndependentOfInputOrder) {
    for (const bool reverse : {false, true}) {
        auto c = closedPool(1, 10, 1);
        QualitySolver solver; solver.init(1, 0, 1);
        PassiveStorageExchange book;
        std::vector<Row> rows{row(4, 0, 3, 0, 1), row(0, 4, 0, 4, 2)};
        if (reverse) std::reverse(rows.begin(), rows.end());
        book.reserve(c, 1, rows);
        EXPECT_EQ(book.receipts()[0].accepted[0].point, 1u);
        EXPECT_DOUBLE_EQ(book.pendingSpill(0), 4);
        nativeStep(c, solver, book, 1);
        EXPECT_DOUBLE_EQ(c.nodes.conc[0] * c.nodes.volume[0], 9);
        EXPECT_DOUBLE_EQ(book.pendingSpill(0), 0);
        auto over = std::vector<Row>{row(5, 5, 100, 4.5, 1), row(6, 6, 100, 5.4, 2)};
        if (reverse) std::reverse(over.begin(), over.end());
        EXPECT_THROW(book.reserve(c, 2, over), std::invalid_argument);
        EXPECT_EQ(book.receipts().size(), 1u);
    }
}

TEST(PassiveStorageExchange, ReserveRefusalIsAtomicAcrossNodesAndMalformedRows) {
    auto c = closedPool(2);
    PassiveStorageExchange book;
    const auto good = row(1, 1, 4, 5);
    std::vector<Row> bad;
    bad.push_back(good); bad.back().pollutant = "Unknown";
    bad.push_back(good); bad.back().units = MassUnits::UG_PER_L;
    bad.push_back(good); bad.back().drain_water = 0;
    bad.push_back(good); bad.back().spill_water = 0;
    bad.push_back(good); bad.back().drain_mass = std::numeric_limits<double>::infinity();
    bad.push_back(good); bad.back().spill_mass = -1;
    bad.push_back(good); bad.back().node = 99;
    bad.push_back(good); bad.back().drain_water = 2;
    for (const auto& r : bad) {
        EXPECT_THROW(book.reserve(c, 1, {r}), std::invalid_argument);
        EXPECT_TRUE(book.receipts().empty()); EXPECT_FALSE(book.failed());
    }
    EXPECT_THROW(book.reserve(c, 1, {good, good}), std::invalid_argument);
    EXPECT_THROW(book.reserve(c, 1, {good, row(101, 101, 1000, 505, 1, 1)}), std::invalid_argument);
    EXPECT_TRUE(book.receipts().empty());
    EXPECT_DOUBLE_EQ(c.nodes.conc[0], 5); EXPECT_DOUBLE_EQ(c.nodes.conc[1], 5);
    book.reserve(c, 1, {good});
    EXPECT_THROW(book.reserve(c, 1, {good}), std::invalid_argument);
    EXPECT_THROW(book.reserve(c, 2, {row(1, 1, 4, 5, 0, 1)}), std::invalid_argument);
    EXPECT_EQ(book.receipts().size(), 1u);
}

TEST(PassiveStorageExchange, GrossBudgetCannotRoundAnUnfundedSumDownIntoAcceptance) {
    for (const bool reverse : {false, true}) {
        auto c = closedPool(1, .3, 1);
        PassiveStorageExchange book;
        std::vector<Row> rows{row(.1, .1, 0, .1, 1), row(.2, .2, 0, .2, 2)};
        if (reverse) std::reverse(rows.begin(), rows.end());
        EXPECT_THROW(book.reserve(c, 1, rows), std::invalid_argument);
        EXPECT_TRUE(book.receipts().empty());
    }
}

TEST(PassiveStorageExchange, TinyWaterResidualCannotHideInRoundedGrossSums) {
    for (bool reverse : {false, true}) {
        auto c = closedPool(1, 10, 1);
        PassiveStorageExchange book;
        std::vector<Row> rows{row(1, 1, 0, 1, 0), row(std::ldexp(1., -55), 0, 0, 0, 1)};
        if (reverse) std::reverse(rows.begin(), rows.end());
        EXPECT_THROW(book.reserve(c, 1, rows), std::invalid_argument);
        EXPECT_TRUE(book.receipts().empty()); EXPECT_DOUBLE_EQ(c.nodes.conc[0], 1);
    }
}

TEST(PassiveStorageExchange, RepresentableSignedAmountSurvivesLargeGrossCancellation) {
    const double large = std::ldexp(1., 53);
    auto c = closedPool(1, large + 4, 1);
    QualitySolver solver; solver.init(1, 0, 1);
    PassiveStorageExchange book;
    book.reserve(c, 1, {row(large, large, large, large, 0), row(1, 1, 0, 1, 1), row(1, 1, 0, 1, 2)});
    EXPECT_DOUBLE_EQ(book.pendingDrain(0), large);
    EXPECT_DOUBLE_EQ(book.pendingSpill(0), large + 2);
    nativeStep(c, solver, book, 1);
    EXPECT_EQ(c.nodes.conc[0] * c.nodes.volume[0], large + 2);
    EXPECT_NE(c.nodes.conc[0], 1);
}

TEST(PassiveStorageExchange, UnrepresentableWholeReceiptsRefuseBeforeSurfaceCredit) {
    const double large = std::ldexp(1., 53);
    auto c = closedPool(1, large + 4, 1);
    PassiveStorageExchange book;
    EXPECT_THROW(book.reserve(c, 1, {row(large, large, large, large, 0), row(1, 1, 0, 1, 1)}),
                 std::invalid_argument);
    EXPECT_TRUE(book.receipts().empty()); EXPECT_FALSE(book.failed());
    EXPECT_DOUBLE_EQ(c.nodes.conc[0], 1);
}

TEST(PassiveStorageExchange, ARepresentableWholeCannotHideSwallowedPartialIncrements) {
    const double large = std::ldexp(1., 53);
    auto c = closedPool(1, large, 1); c.coupling_delivery_remaining = 2;
    QualitySolver solver; solver.init(1, 0, 1);
    PassiveStorageExchange book; book.reserve(c, 1, {row(1, 1, 3, 1)});
    c.coupling_delivery_remaining = 1;
    EXPECT_THROW(solver.execute(c, 1, book), std::invalid_argument);
    EXPECT_TRUE(book.failed()); EXPECT_EQ(c.nodes.conc[0] * c.nodes.volume[0], large);
    EXPECT_DOUBLE_EQ(book.pendingDrain(0), 3); EXPECT_DOUBLE_EQ(book.pendingSpill(0), 1);
    c.elapsed_ms = 1000; c.coupling_delivery_remaining = 0;
    EXPECT_THROW(solver.execute(c, 1, book), std::invalid_argument);
    EXPECT_EQ(c.nodes.conc[0] * c.nodes.volume[0], large);
    EXPECT_DOUBLE_EQ(book.pendingDrain(0), 3);
}

TEST(PassiveStorageExchange, AnInexactGrossPartitionStopsBeforeNativeMixing) {
    auto c = closedPool(1, 100, 0); c.coupling_delivery_remaining = 3;
    QualitySolver solver; solver.init(1, 0, 1);
    PassiveStorageExchange book; book.reserve(c, 1, {row(1, 1, 1, 0)});
    c.coupling_delivery_remaining = 2;
    EXPECT_THROW(solver.execute(c, 1, book), std::invalid_argument);
    EXPECT_TRUE(book.failed()); EXPECT_DOUBLE_EQ(c.nodes.conc[0], 0);
    EXPECT_DOUBLE_EQ(book.pendingDrain(0), 1);
    EXPECT_DOUBLE_EQ(book.receipts()[0].remaining[0].drain_water, 1);
}

TEST(PassiveStorageExchange, NativeConcentrationRoundTripHasAnExplicitMachineBound) {
    auto c = closedPool(1, 11, 1);
    QualitySolver solver; solver.init(1, 0, 1);
    PassiveStorageExchange book; book.reserve(c, 1, {row(1, 1, 5, 1)});
    nativeStep(c, solver, book, 1);
    EXPECT_NEAR(c.nodes.conc[0] * c.nodes.volume[0], 15,
                2 * std::numeric_limits<double>::epsilon() * 15);
    EXPECT_DOUBLE_EQ(book.pendingDrain(0), 0); EXPECT_DOUBLE_EQ(book.pendingSpill(0), 0);
}

TEST(PassiveStorageExchange, PendingReceiptsCannotRebindNativeDeliverySpan) {
    auto c = closedPool(); c.coupling_delivery_remaining = 4;
    QualitySolver solver; solver.init(1, 0, 1);
    PassiveStorageExchange book; book.reserve(c, 1, {row(1, 1, 4, 5)});
    for (double changed : {2., 20.}) {
        c.coupling_delivery_remaining = changed;
        EXPECT_THROW(book.reserve(c, 2, {row(1, 1, 4, 5, 1)}), std::invalid_argument);
        EXPECT_EQ(book.receipts().size(), 1u); EXPECT_DOUBLE_EQ(book.pendingSpill(0), 5);
    }
    c.coupling_delivery_remaining = 4;
    nativeStep(c, solver, book, 4);
    EXPECT_FALSE(book.failed());
}

TEST(PassiveStorageExchange, SettledPoolsRetireButReceiptAndPointHistoryRemain) {
    auto c = closedPool(2);
    QualitySolver solver; solver.init(2, 0, 1);
    PassiveStorageExchange book; book.reserve(c, 1, {row(1, 1, 4, 5)});
    nativeStep(c, solver, book, 1);
    c.elapsed_ms = 5000;
    c.nodes.conc[0] = 2; // no outstanding encumbrance at this retired pool
    book.reserve(c, 2, {row(1, 1, 6, 5, 1, 1)});
    nativeStep(c, solver, book, 1);
    EXPECT_DOUBLE_EQ(c.nodes.conc[1] * c.nodes.volume[1], 501);
    book.reserve(c, 3, {row(1, 1, 3, 2, 0, 0)});
    nativeStep(c, solver, book, 1);
    EXPECT_DOUBLE_EQ(c.nodes.conc[0] * c.nodes.volume[0], 201);
    EXPECT_EQ(book.receipts().size(), 3u);
    EXPECT_THROW(book.reserve(c, 3, {row(1, 1, 3, 2)}), std::invalid_argument);
}

TEST(PassiveStorageExchange, UnsupportedNativeOwnersRefuseBeforeReservation) {
    for (int owner = 0; owner < 20; ++owner) {
        SCOPED_TRACE(owner);
        auto c = closedPool();
        if (owner == 0) c.nodes.type[0] = NodeType::JUNCTION;
        if (owner == 1) c.nodes.depth[0] = 0;
        if (owner == 2) c.nodes.volume[0] = 0;
        if (owner == 3) c.nodes.old_volume[0] = 101;
        if (owner == 4) c.nodes.old_depth[0] = 1.1;
        if (owner == 5) c.pollutants.k_decay[0] = .1;
        if (owner == 6) c.options.water_age = true;
        if (owner == 7) c.options.heat_transport = true;
        if (owner == 8) c.options.routing_model = RoutingModel::FV;
        if (owner == 9) c.options.quality_solver = QualitySolverKind::EULERIAN_ARD;
        if (owner == 10) c.node_subtypes.storages.lid_state[0].cells.resize(1);
        if (owner == 11) c.nodes.coupling_qual_queue[0] = -1;
        if (owner == 12) c.nodes.user_lat_flow[0] = 1;
        if (owner == 13) c.forcing.node_quality_mode.assign(1, ForcingMode::OVERRIDE);
        if (owner == 14) c.surface_outfall_quality.any = true;
        if (owner == 15) c.pollutant_names.add("Second");
        if (owner == 16) c.nodes.coupling_qual_inflow[0] = 1;
        if (owner == 17) {
            c.link_names.add("Port"); c.links.resize(1);
            c.links.node1[0] = c.links.node2[0] = 0;
        }
        if (owner == 18) c.nodes.aquifer2d_bed.assign(1, 1);
        if (owner == 19) c.node_subtypes.storages.evap_loss[0] = .1;
        PassiveStorageExchange book;
        EXPECT_THROW(book.reserve(c, 1, {row(1, 1, 4, 5)}), std::invalid_argument);
        EXPECT_TRUE(book.receipts().empty()); EXPECT_FALSE(book.failed());
    }
}

TEST(PassiveStorageExchange, EveryAffectedPoolIsPreflightedBeforeAnyNativeMixing) {
    auto c = closedPool(2);
    QualitySolver solver; solver.init(2, 0, 1);
    PassiveStorageExchange book;
    book.reserve(c, 1, {row(1, 1, 4, 5, 0, 0), row(1, 1, 6, 5, 1, 1)});
    c.nodes.conc_old[1] = 4;
    EXPECT_THROW(solver.execute(c, 1, book), std::invalid_argument);
    EXPECT_TRUE(book.failed());
    EXPECT_DOUBLE_EQ(c.nodes.conc[0], 5); EXPECT_DOUBLE_EQ(c.nodes.conc[1], 5);
    EXPECT_DOUBLE_EQ(book.pendingSpill(0), 5); EXPECT_DOUBLE_EQ(book.pendingSpill(1), 5);
}

TEST(PassiveStorageExchange, NativeClockAndPoolFailuresStopWithoutReleasingReservations) {
    for (int failure = 0; failure < 5; ++failure) {
        auto c = closedPool(); c.coupling_delivery_remaining = 4;
        QualitySolver solver; solver.init(1, 0, 1);
        PassiveStorageExchange book; book.reserve(c, 1, {row(1, 1, 4, 5)});
        if (failure == 0) c.elapsed_ms = 1000;
        if (failure == 1) c.coupling_delivery_remaining = 4; // expected post-drain 3
        else c.coupling_delivery_remaining = 3;
        if (failure == 2) c.nodes.conc[0] = 4;
        if (failure == 3) c.nodes.user_conc_mass_flux[0] = 1;
        if (failure == 4) c.nodes.conc_old[0] = 4;
        const double concentration = c.nodes.conc[0];
        EXPECT_THROW(solver.execute(c, 1, book), std::invalid_argument);
        EXPECT_TRUE(book.failed());
        EXPECT_DOUBLE_EQ(c.nodes.conc[0], concentration);
        EXPECT_DOUBLE_EQ(book.pendingSpill(0), 5);
        EXPECT_EQ(book.receipts().size(), 1u);
        EXPECT_THROW(solver.execute(c, 1, book), std::invalid_argument);
        EXPECT_THROW(book.reserve(c, 2, {row(1, 1, 4, 5, 1)}), std::invalid_argument);
    }
}

TEST(PassiveStorageExchange, WrongContextChangedBasisAndSuccessfulReplayAreRefused) {
    static_assert(!std::is_copy_constructible_v<PassiveStorageExchange>);
    static_assert(!std::is_move_constructible_v<PassiveStorageExchange>);
    auto c = closedPool(); auto other = closedPool();
    PassiveStorageExchange book; book.reserve(c, 1, {row(1, 1, 4, 5)});
    EXPECT_THROW(book.reserve(other, 2, {row(1, 1, 4, 5)}), std::invalid_argument);
    c.pollutants.units[0] = MassUnits::UG_PER_L;
    EXPECT_THROW(book.reserve(c, 2, {row(1, 1, 4, 5, 1)}), std::invalid_argument);
    c.pollutants.units[0] = MassUnits::MG_PER_L;
    QualitySolver solver; solver.init(1, 0, 1);
    nativeStep(c, solver, book, 1);
    const double concentration = c.nodes.conc[0];
    EXPECT_THROW(solver.execute(c, 1, book), std::invalid_argument);
    EXPECT_DOUBLE_EQ(c.nodes.conc[0], concentration); EXPECT_TRUE(book.failed());
}

TEST(PassiveStorageExchange, FirstFailedAttemptRecordsCurrentNativeInterval) {
    for (bool wrong_basis : {false, true}) {
        auto c = closedPool(); c.coupling_delivery_remaining = 4;
        PassiveStorageExchange book; book.reserve(c, 1, {row(1, 1, 4, 5)});
        QualitySolver solver; solver.init(1, 0, wrong_basis ? 1 : 0);
        c.coupling_delivery_remaining = 3.75;
        if (wrong_basis) { c.pollutants.units[0] = MassUnits::UG_PER_L; c.elapsed_ms = 1234; }
        EXPECT_THROW(solver.execute(c, .25, book), std::invalid_argument);
        EXPECT_DOUBLE_EQ(book.attempt().start_ms, c.elapsed_ms);
        EXPECT_DOUBLE_EQ(book.attempt().dt, .25);
        EXPECT_DOUBLE_EQ(book.attempt().remaining_start, 4);
        const double failed_start = book.attempt().start_ms;
        c.elapsed_ms = 9999;
        EXPECT_THROW(solver.execute(c, 1, book), std::invalid_argument);
        EXPECT_DOUBLE_EQ(book.attempt().start_ms, failed_start);
        EXPECT_DOUBLE_EQ(book.attempt().dt, .25);
    }
}

namespace {
using Phase = PassiveStorageExchange::Phase;
using Archive = openswmm::detail::CompletedSourceArchive;
void freezeAndDrain(SimulationContext& c, PassiveStorageExchange& book, double dt) {
    book.freezeDelivery(c, dt);
    c.coupling_delivery_remaining = std::max(0.0, c.coupling_delivery_remaining - dt);
    book.observeDeliveryDrain(c);
}
void finishNativeStep(SimulationContext& c, QualitySolver& solver, PassiveStorageExchange& book, double dt) {
    const double start = c.elapsed_ms;
    solver.execute(c, dt, book);
    EXPECT_DOUBLE_EQ(c.elapsed_ms, start);
    c.old_elapsed_ms = start; c.elapsed_ms = start + 1000.0 * dt;
}
void archiveArtifact(const std::string& name, const std::string& bytes) {
    const char* chosen = std::getenv("OPENSWMM_PASSIVE_EXCHANGE_EVIDENCE");
    const auto dir = chosen ? std::filesystem::path(chosen) :
        std::filesystem::path(__FILE__).parent_path() / "data/passive_storage_exchange";
    std::filesystem::create_directories(dir);
    std::ofstream out(dir / name, std::ios::binary);
    out.write(bytes.data(), bytes.size());
    ASSERT_TRUE(out.good());
}
std::uint32_t archiveCrc(const std::string& bytes) {
    std::uint32_t value = ~std::uint32_t(0);
    for (unsigned char c : bytes) {
        value ^= c;
        for (int bit = 0; bit < 8; ++bit)
            value = (value >> 1) ^ ((value & 1) ? 0xedb88320u : 0u);
    }
    return ~value;
}
template<class Change> std::string forgedArchive(const std::string& bytes, Change change,
                                                std::uint32_t requested_version = 1) {
    Archive in(bytes);
    std::string name, payload;
    std::uint32_t version, endian, crc;
    in(name, version, endian, crc, payload); in.finish();
    change(payload); version = requested_version; crc = archiveCrc(payload);
    Archive out; out(name, version, endian, crc, payload);
    return out.bytes();
}
template<class T> void replaceLast(std::string& bytes, T before, T after) {
    const std::string pattern(reinterpret_cast<const char*>(&before), sizeof(T));
    const auto at = bytes.rfind(pattern);
    if (at == std::string::npos) throw std::runtime_error("Archive fixture pattern missing");
    std::memcpy(bytes.data() + at, &after, sizeof(T));
}
std::size_t ownerPrefix(const std::string& payload) {
    Archive in(payload);
    std::vector<std::string> nodes, pollutants;
    MassUnits units;
    std::uint64_t id;
    double next, span, time, actual_span, start, dt, remaining;
    in(nodes, pollutants, units, id, next, span, time, actual_span, start, dt, remaining);
    Archive prefix;
    prefix(nodes, pollutants, units, id, next, span, time, actual_span, start, dt, remaining);
    return prefix.bytes().size();
}
} // namespace

TEST(PassiveStorageHandoff, EmptyFrozenCohortLeavesFirstAdvanceReceiptForNextAssembly) {
    auto c = closedPool();
    QualitySolver solver; solver.init(1, 0, 1);
    PassiveStorageExchange book;
    SurfaceDonor surface(4);
    freezeAndDrain(c, book, 1);
    EXPECT_TRUE(book.deliveryFrozen()); EXPECT_TRUE(book.deliveryDrainObserved());
    const auto input = row(1, 1, 4, 5);
    book.reserveNext(c, 1, {input}, 0, 1000); surface.post(input);
    finishNativeStep(c, solver, book, 1);
    EXPECT_EQ(c.nodes.conc[0] * c.nodes.volume[0], 500);
    EXPECT_DOUBLE_EQ(book.pendingDrain(0), 4); EXPECT_DOUBLE_EQ(book.pendingSpill(0), 5);
    EXPECT_FALSE(book.deliveryFrozen());
    EXPECT_NEAR(combined(c, surface, book), 900, 1e-12);
    nativeStep(c, solver, book, 1);
    EXPECT_EQ(c.nodes.conc[0] * c.nodes.volume[0], 499);
    EXPECT_NEAR(combined(c, surface, book), 900, 1e-12);
}

TEST(PassiveStorageHandoff, CurrentQualityConsumesOnlyFrozenIdsAndPreservesEarlierSample) {
    auto c = closedPool();
    QualitySolver solver; solver.init(1, 0, 1);
    PassiveStorageExchange book; book.reserve(c, 1, {row(1, 1, 4, 5)});
    freezeAndDrain(c, book, 1);
    book.reserveNext(c, 2, {row(1, 1, 6, 5, 1)}, 0, 1000);
    finishNativeStep(c, solver, book, 1);
    EXPECT_EQ(c.nodes.conc[0] * c.nodes.volume[0], 499);
    EXPECT_DOUBLE_EQ(book.pendingDrain(0), 6); EXPECT_DOUBLE_EQ(book.pendingSpill(0), 5);
    const auto& receipt = book.receipts()[1];
    EXPECT_EQ(receipt.publication.phase, Phase::ADVANCE);
    EXPECT_DOUBLE_EQ(receipt.publication.capture_ms, 0);
    EXPECT_DOUBLE_EQ(receipt.publication.eligible_ms, 1000);
    EXPECT_DOUBLE_EQ(receipt.samples[0].concentration, 5);
    EXPECT_NE(receipt.samples[0].concentration, c.nodes.conc[0]);
    const auto bytes = book.checkpoint(c); archiveArtifact("overlapping-pending.bin", bytes);
    auto resumed = c; PassiveStorageExchange restored; restored.restore(resumed, bytes);
    EXPECT_EQ(restored.checkpoint(resumed), bytes);
    nativeStep(c, solver, book, 1);
    QualitySolver resumed_solver; resumed_solver.init(1, 0, 1);
    nativeStep(resumed, resumed_solver, restored, 1);
    EXPECT_EQ(c.nodes.conc[0] * c.nodes.volume[0], 500);
    EXPECT_EQ(resumed.nodes.conc, c.nodes.conc);
    EXPECT_EQ(restored.checkpoint(resumed), book.checkpoint(c));
}

TEST(PassiveStorageHandoff, PartialFrozenSpanDoesNotSliceNewlyProducedRows) {
    auto c = closedPool(); c.coupling_delivery_remaining = 4;
    QualitySolver solver; solver.init(1, 0, 1);
    PassiveStorageExchange book; book.reserve(c, 1, {row(1, 1, 4, 5)});
    freezeAndDrain(c, book, 1);
    book.reserveNext(c, 2, {row(1, 1, 6, 5, 1)}, 0, 1000);
    finishNativeStep(c, solver, book, 1);
    EXPECT_NEAR(c.nodes.conc[0] * c.nodes.volume[0], 499.75, 1e-12);
    EXPECT_DOUBLE_EQ(book.receipts()[0].remaining[0].drain_mass, 3);
    EXPECT_DOUBLE_EQ(book.receipts()[1].remaining[0].drain_mass, 6);
    EXPECT_DOUBLE_EQ(book.receipts()[1].remaining[0].spill_mass, 5);
    EXPECT_DOUBLE_EQ(c.coupling_delivery_remaining, 3);
    const auto bytes = book.checkpoint(c);
    auto resumed = c; PassiveStorageExchange restored; restored.restore(resumed, bytes);
    QualitySolver other; other.init(1, 0, 1);
    nativeStep(c, solver, book, 3); nativeStep(resumed, other, restored, 3);
    EXPECT_NEAR(c.nodes.conc[0] * c.nodes.volume[0], 500, 1e-12);
    EXPECT_EQ(resumed.nodes.conc, c.nodes.conc);
    EXPECT_EQ(restored.checkpoint(resumed), book.checkpoint(c));
}

TEST(PassiveStorageHandoff, CoarseBatchProvenanceIsNotReplacedByRoutingDuration) {
    auto c = closedPool(); c.elapsed_ms = 10000;
    QualitySolver solver; solver.init(1, 0, 1);
    PassiveStorageExchange book; freezeAndDrain(c, book, 1);
    book.reserveNext(c, 1, {row(1, 1, 4, 5)}, 0, 11000);
    finishNativeStep(c, solver, book, 1);
    const auto& p = book.receipts()[0].publication;
    EXPECT_DOUBLE_EQ(p.capture_ms, 10000); EXPECT_DOUBLE_EQ(p.batch_start_ms, 0);
    EXPECT_DOUBLE_EQ(p.batch_end_ms, 11000); EXPECT_DOUBLE_EQ(p.eligible_ms, 11000);
    const auto bytes = book.checkpoint(c);
    auto resumed = c; PassiveStorageExchange restored; restored.restore(resumed, bytes);
    EXPECT_EQ(restored.checkpoint(resumed), bytes);
}

TEST(PassiveStorageHandoff, ReportAndFinalKeepActualPostQualitySampleWithoutExtraStep) {
    auto c = closedPool();
    QualitySolver solver; solver.init(1, 0, 1);
    PassiveStorageExchange book; book.reserve(c, 1, {row(1, 1, 4, 5)});
    nativeStep(c, solver, book, 1);
    const double sample = c.nodes.conc[0];
    book.reserveAtBoundary(c, 2, {row(1, 1, sample + 1, sample, 1)}, Phase::REPORT, 0, 1000);
    EXPECT_EQ(book.receipts()[1].publication.phase, Phase::REPORT);
    EXPECT_DOUBLE_EQ(book.receipts()[1].samples[0].concentration, sample);
    nativeStep(c, solver, book, 1);
    book.reserveAtBoundary(c, 3, {row(1, 1, 4, 5, 2)}, Phase::FINAL, 1000, 2000);
    const auto before = c.nodes.conc;
    const auto bytes = book.checkpoint(c); archiveArtifact("final-outstanding.bin", bytes);
    auto resumed = c; PassiveStorageExchange restored; restored.restore(resumed, bytes);
    EXPECT_EQ(c.nodes.conc, before); EXPECT_EQ(resumed.nodes.conc, before);
    EXPECT_DOUBLE_EQ(resumed.elapsed_ms, 2000);
    EXPECT_EQ(restored.receipts().back().publication.phase, Phase::FINAL);
    EXPECT_DOUBLE_EQ(restored.pendingDrain(0), 4); EXPECT_DOUBLE_EQ(restored.pendingSpill(0), 5);
    EXPECT_EQ(restored.checkpoint(resumed), bytes);
}

TEST(PassiveStorageHandoff, NewReservationsDoNotResetObservedSpanOrBorrowCurrentDrain) {
    auto c = closedPool(1, 10, 1); c.coupling_delivery_remaining = 4;
    PassiveStorageExchange book; book.reserve(c, 1, {row(8, 8, 0, 8)});
    freezeAndDrain(c, book, 1);
    c.coupling_delivery_remaining = 20;
    EXPECT_THROW(book.reserveNext(c, 2, {row(1, 1, 100, 1, 1)}, 0, 1000), std::invalid_argument);
    c.coupling_delivery_remaining = 3;
    EXPECT_THROW(book.reserveNext(c, 2, {row(3, 3, 100, 3, 1)}, 0, 1000), std::invalid_argument);
    EXPECT_EQ(book.receipts().size(), 1u); EXPECT_DOUBLE_EQ(book.pendingSpill(0), 8);
    EXPECT_FALSE(book.failed());
}

TEST(PassiveStorageHandoff, PhysicalBatchCannotReplayUnderAnotherReceiptIdOrPhase) {
    auto c = closedPool();
    QualitySolver solver; solver.init(1, 0, 1);
    PassiveStorageExchange book; freezeAndDrain(c, book, 1);
    book.reserveNext(c, 1, {row(1, 1, 4, 5)}, 0, 1000);
    EXPECT_THROW(book.reserveNext(c, 2, {row(1, 1, 4, 5)}, 0, 1000), std::invalid_argument);
    finishNativeStep(c, solver, book, 1);
    EXPECT_THROW(book.reserveAtBoundary(c, 2, {row(1, 1, 4, 5)}, Phase::REPORT, 0, 1000), std::invalid_argument);
    EXPECT_THROW(book.reserveAtBoundary(c, 2, {row(1, 1, 4, 5)}, Phase::FINAL, 500, 1000), std::invalid_argument);
    EXPECT_THROW(book.reserveAtBoundary(c, 2, {row(1, 1, 4, 5, 1)}, Phase::REPORT, 0, 999), std::invalid_argument);
    EXPECT_EQ(book.receipts().size(), 1u);
    // Different points may share a real firing; adjacent batches may reuse a point.
    book.reserveAtBoundary(c, 2, {row(1, 1, 6, 5, 1)}, Phase::REPORT, 0, 1000);
    nativeStep(c, solver, book, 1);
    book.reserveAtBoundary(c, 3, {row(1, 1, 4, 5)}, Phase::REPORT, 1000, 2000);
    EXPECT_EQ(book.receipts().size(), 3u); EXPECT_FALSE(book.failed());
}

TEST(PassiveStorageHandoff, MissingDuplicateAndUnorderedCallbacksStopTheOwner) {
    for (int mode = 0; mode < 4; ++mode) {
        auto c = closedPool(); c.coupling_delivery_remaining = 4;
        QualitySolver solver; solver.init(1, 0, 1);
        PassiveStorageExchange book; book.reserve(c, 1, {row(1, 1, 4, 5)});
        if (mode == 0) EXPECT_THROW(book.observeDeliveryDrain(c), std::invalid_argument);
        if (mode == 1) {
            book.freezeDelivery(c, 1); c.coupling_delivery_remaining = 3;
            EXPECT_THROW(solver.execute(c, 1, book), std::invalid_argument);
        }
        if (mode == 2) {
            freezeAndDrain(c, book, 1);
            EXPECT_THROW(book.observeDeliveryDrain(c), std::invalid_argument);
        }
        if (mode == 3) {
            book.freezeDelivery(c, 1);
            EXPECT_THROW(book.freezeDelivery(c, 1), std::invalid_argument);
        }
        EXPECT_TRUE(book.failed()); EXPECT_DOUBLE_EQ(c.nodes.conc[0], 5);
        EXPECT_DOUBLE_EQ(book.pendingSpill(0), 5);
        EXPECT_THROW(book.checkpoint(c), std::invalid_argument);
    }
}

TEST(PassiveStorageArchive, PendingPartialReceiptResumesToByteIdenticalOwner) {
    auto c = closedPool(); c.coupling_delivery_remaining = 4;
    QualitySolver solver; solver.init(1, 0, 1);
    PassiveStorageExchange book; book.reserve(c, 1, {row(1, 1, 4, 5)});
    nativeStep(c, solver, book, 1);
    const auto bytes = book.checkpoint(c); archiveArtifact("partial-pending.bin", bytes);
    auto resumed = c; PassiveStorageExchange restored; restored.restore(resumed, bytes);
    EXPECT_EQ(restored.checkpoint(resumed), bytes);
    QualitySolver other; other.init(1, 0, 1);
    for (double dt : {1.5, 2.}) {
        nativeStep(c, solver, book, dt); nativeStep(resumed, other, restored, dt);
        EXPECT_EQ(c.nodes.conc, resumed.nodes.conc);
        EXPECT_EQ(book.checkpoint(c), restored.checkpoint(resumed));
    }
    archiveArtifact("partial-final.bin", book.checkpoint(c));
}

TEST(PassiveStorageArchive, EmptyFrozenHistoryPersistsButTransientStateCannotBeSaved) {
    auto c = closedPool(); PassiveStorageExchange book;
    EXPECT_TRUE(book.checkpoint(c).empty());
    book.freezeDelivery(c, 1);
    EXPECT_THROW(book.checkpoint(c), std::invalid_argument);
    book.observeDeliveryDrain(c);
    EXPECT_THROW(book.checkpoint(c), std::invalid_argument);
    QualitySolver solver; solver.init(1, 0, 1);
    finishNativeStep(c, solver, book, 1);
    const auto bytes = book.checkpoint(c);
    auto resumed = c; PassiveStorageExchange restored; restored.restore(resumed, bytes);
    EXPECT_EQ(restored.checkpoint(resumed), bytes);
    EXPECT_THROW(restored.restore(resumed, bytes), std::invalid_argument);
}

TEST(PassiveStorageArchive, RetiredOrdinaryMobilizationDoesNotInvalidateHistoricalReplay) {
    auto c = closedPool(2);
    QualitySolver solver; solver.init(2, 0, 1);
    PassiveStorageExchange book; book.reserve(c, 1, {row(1, 1, 4, 5)});
    nativeStep(c, solver, book, 1);
    c.nodes.user_conc_mass_flux[0] = 1;
    const auto settled = book.checkpoint(c);
    auto fresh = c; PassiveStorageExchange restored; restored.restore(fresh, settled);
    EXPECT_EQ(restored.checkpoint(fresh), settled);
    EXPECT_DOUBLE_EQ(fresh.nodes.user_conc_mass_flux[0], 1);
    book.reserve(c, 2, {row(1, 1, 4, 5, 1, 1)});
    const auto pending = book.checkpoint(c);
    PassiveStorageExchange other; other.restore(fresh, pending);
    EXPECT_EQ(other.checkpoint(fresh), pending);
    EXPECT_DOUBLE_EQ(fresh.nodes.user_conc_mass_flux[0], 1);
    EXPECT_DOUBLE_EQ(other.pendingSpill(1), 5);
}

TEST(PassiveStorageArchive, ReceiptHistoryWorkIsBoundedEvenWithNoDeliveryEvents) {
    constexpr std::uint64_t count = 4096;
    auto c = closedPool(count);
    Archive payload;
    auto nodes = c.node_names.names(), pollutants = c.pollutant_names.names();
    auto units = MassUnits::MG_PER_L;
    std::uint64_t last = count, generation = 0, one = 1, no_deliveries = 0;
    double zero = 0, volume = 100, depth = 1, concentration = 5;
    payload(nodes, pollutants, units, last, zero, zero, zero, zero, zero, zero, zero);
    auto n = count; payload(n);
    for (std::uint64_t i = 0; i < count; ++i) {
        auto id = i + 1; auto phase = Phase::SETTLED;
        payload(id, phase, zero, zero, zero, zero, zero, generation, one);
        auto r = row(1, 1, 5, 5, i, int(i));
        for (int copy = 0; copy < 2; ++copy)
            payload(r.point, r.node, r.pollutant, r.units, r.drain_water, r.spill_water, r.drain_mass, r.spill_mass);
        payload(volume, depth, concentration);
    }
    payload(no_deliveries, n);
    for (int i = 0; i < int(count); ++i) payload(i, volume, depth, concentration);
    std::string name = "OpenSWMM passive storage receipt owner", bytes = payload.bytes();
    std::uint32_t version = 1, endian = 0x01020304, crc = archiveCrc(bytes);
    Archive envelope; envelope(name, version, endian, crc, bytes);
    PassiveStorageExchange target;
    try {
        target.restore(c, envelope.bytes());
        FAIL() << "Expected bounded semantic replay refusal";
    } catch (const std::invalid_argument& e) {
        EXPECT_NE(std::string(e.what()).find("validation work"), std::string::npos);
    }
    EXPECT_TRUE(target.checkpoint(c).empty()); EXPECT_FALSE(target.failed());
}

TEST(PassiveStorageArchive, CorruptionWithValidChecksumRefusesAtomicallyAndAllowsValidRetry) {
    auto c = closedPool(); c.coupling_delivery_remaining = 4;
    QualitySolver solver; solver.init(1, 0, 1);
    PassiveStorageExchange book;
    constexpr std::uint64_t point = 0x123456789abc;
    book.reserve(c, 17, {row(1, 1, 6.125, 5, point)}); nativeStep(c, solver, book, 1);
    const auto good = book.checkpoint(c); archiveArtifact("semantic-reference.bin", good);
    std::vector<std::string> bad{good.substr(0, good.size() - 1), good + "x"};
    auto broken = good; broken.back() ^= 1; bad.push_back(broken);
    bad.push_back(forgedArchive(good, [](auto&) {}, 0));
    bad.push_back(forgedArchive(good, [](auto& p) { replaceLast(p, 4.59375, 4.25); }));
    bad.push_back(forgedArchive(good, [](auto& p) { replaceLast(p, 5., 4.75); }));
    bad.push_back(forgedArchive(good, [](auto& p) { replaceLast(p, std::uint64_t(point), std::uint64_t(point + 1)); }));
    bad.push_back(forgedArchive(good, [](auto& p) { replaceLast(p, 6.125, std::numeric_limits<double>::quiet_NaN()); }));
    bad.push_back(forgedArchive(good, [](auto& p) {
        const auto offset = ownerPrefix(p);
        std::uint64_t too_many = 4097;
        std::memcpy(p.data() + offset, &too_many, sizeof(too_many));
    }));
    bad.push_back(forgedArchive(good, [](auto& p) {
        const auto phase = ownerPrefix(p) + 2 * sizeof(std::uint64_t);
        p[phase] = char(0xff);
    }));
    const auto before = c.nodes.conc;
    for (std::size_t i = 0; i < bad.size(); ++i) {
        SCOPED_TRACE(i); PassiveStorageExchange target;
        EXPECT_THROW(target.restore(c, bad[i]), std::exception);
        EXPECT_TRUE(target.checkpoint(c).empty()); EXPECT_FALSE(target.failed());
        EXPECT_EQ(c.nodes.conc, before); EXPECT_DOUBLE_EQ(c.elapsed_ms, 1000);
        target.restore(c, good); EXPECT_EQ(target.checkpoint(c), good);
    }
}

TEST(PassiveStorageArchive, IncomingNativeIdentityPoolAndClockMustMatchBeforeInstall) {
    auto c = closedPool();
    PassiveStorageExchange book; book.reserve(c, 1, {row(1, 1, 4, 5)});
    const auto bytes = book.checkpoint(c);
    for (int mismatch = 0; mismatch < 5; ++mismatch) {
        auto other = c;
        if (mismatch == 0) other.elapsed_ms = 1;
        if (mismatch == 1) other.coupling_delivery_remaining = 1;
        if (mismatch == 2) other.nodes.conc[0] = 4;
        if (mismatch == 3) other.pollutants.units[0] = MassUnits::UG_PER_L;
        if (mismatch == 4) other.nodes.coupling_qual_queue[0] = -1;
        PassiveStorageExchange target;
        EXPECT_THROW(target.restore(other, bytes), std::exception);
        EXPECT_TRUE(target.checkpoint(other).empty());
        target.restore(c, bytes); EXPECT_EQ(target.checkpoint(c), bytes);
    }
}

namespace {
using Arithmetic = openswmm::quality::PassiveStorageArithmetic;
std::string roundoffLine(double before, const Row& r, const SimulationContext& c, const PassiveStorageExchange& book) {
    std::ostringstream out; out << std::hexfloat << before << ' ' << r.drain_mass << ' ' << r.spill_mass
        << ' ' << c.nodes.volume[0]*c.nodes.conc[0];
    for (double part : book.storedRoundoff(0).parts()) out << ' ' << part;
    return out.str()+"\n";
}
}
TEST(PassiveStorageRoundoff, RepeatedBothSignsAndZeroNetRetainExactReceiverAccounting) {
    for (const double direction : {-1., 1.}) {
        auto c = closedPool(); QualitySolver solver; solver.init(1, 0, 1);
        PassiveStorageExchange book(Arithmetic::RETAIN_ROUNDOFF);
        std::ostringstream audit; audit << "initial " << std::hexfloat << c.nodes.volume[0]*c.nodes.conc[0] << '\n';
        for (std::uint64_t id = 1; id <= 128; ++id) {
            const auto r = row(1, 1, c.nodes.conc[0]+direction*.1, c.nodes.conc[0]);
            const double before = c.nodes.volume[0]*c.nodes.conc[0];
            ASSERT_NO_THROW(book.reserve(c, id, {r}));
            ASSERT_NO_THROW(nativeStep(c, solver, book, 1));
            audit << roundoffLine(before, r, c, book);
            EXPECT_EQ(book.pendingDrain(0), 0); EXPECT_EQ(book.pendingSpill(0), 0);
        }
        ASSERT_FALSE(book.storedRoundoff(0).zero());
        const auto retained = book.storedRoundoff(0); const double concentration = c.nodes.conc[0];
        const auto r = row(1, 1, concentration, concentration);
        ASSERT_NO_THROW(book.reserve(c, 129, {r}));
        ASSERT_NO_THROW(nativeStep(c, solver, book, 1));
        EXPECT_EQ(c.nodes.conc[0], concentration); EXPECT_EQ(book.storedRoundoff(0), retained);
        audit << roundoffLine(c.nodes.volume[0]*concentration, r, c, book);
        archiveArtifact(direction > 0 ? "roundoff-positive.txt" : "roundoff-negative.txt", audit.str());
        const auto bytes = book.checkpoint(c);
        PassiveStorageExchange resumed(Arithmetic::RETAIN_ROUNDOFF);
        ASSERT_NO_THROW(resumed.restore(c, bytes));
        EXPECT_EQ(resumed.checkpoint(c), bytes); EXPECT_EQ(resumed.storedRoundoff(0), retained);
        archiveArtifact(direction > 0 ? "roundoff-positive-retired.bin" : "roundoff-negative-retired.bin", bytes);
    }
}
TEST(PassiveStorageRoundoff, NonzeroRemainderKeepsRetiredPoolBound) {
    auto c = closedPool(); QualitySolver solver; solver.init(1, 0, 1);
    PassiveStorageExchange book(Arithmetic::RETAIN_ROUNDOFF);
    book.reserve(c, 1, {row(1, 1, 5.1, 5)}); nativeStep(c, solver, book, 1);
    ASSERT_FALSE(book.storedRoundoff(0).zero()); ASSERT_EQ(book.pendingSpill(0), 0);
    const auto retained = book.storedRoundoff(0); const auto receipts = book.receipts();
    c.nodes.conc[0] += 1;
    EXPECT_THROW(book.reserve(c, 2, {row(1, 1, c.nodes.conc[0]+.1, c.nodes.conc[0])}), std::invalid_argument);
    EXPECT_EQ(book.storedRoundoff(0), retained); EXPECT_EQ(book.receipts(), receipts);
}
TEST(PassiveStorageRoundoff, NegativeRemainderRestrictsGrossFundingBeforeCredit) {
    auto c = closedPool(); QualitySolver solver; solver.init(1, 0, 1);
    PassiveStorageExchange book(Arithmetic::RETAIN_ROUNDOFF);
    book.reserve(c, 1, {row(1, 1, 5.1, 5)}); nativeStep(c, solver, book, 1);
    ASSERT_TRUE(book.storedRoundoff(0).negative()); const double mass = c.nodes.conc[0]*c.nodes.volume[0];
    EXPECT_FALSE(book.canFund(0, mass, mass));
    const auto receipts = book.receipts(); const auto retained = book.storedRoundoff(0);
    EXPECT_THROW(book.reserve(c, 2, {row(100, 100, mass+100, mass)}), std::invalid_argument);
    EXPECT_EQ(book.receipts(), receipts); EXPECT_EQ(book.storedRoundoff(0), retained);
}
TEST(PassiveStorageRoundoff, PositiveRemainderCannotFundExtraPhysicalSpill) {
    auto c = closedPool(); QualitySolver solver; solver.init(1, 0, 1);
    PassiveStorageExchange book(Arithmetic::RETAIN_ROUNDOFF);
    for (std::uint64_t id = 1; id <= 3; ++id) {
        book.reserve(c, id, {row(1, 1, c.nodes.conc[0]-.1, c.nodes.conc[0])}); nativeStep(c, solver, book, 1);
    }
    ASSERT_FALSE(book.storedRoundoff(0).negative()); ASSERT_FALSE(book.storedRoundoff(0).zero());
    const double water = std::nextafter(100., std::numeric_limits<double>::infinity());
    const double spill = water*c.nodes.conc[0], mass = 100*c.nodes.conc[0];
    auto accountable = book.storedRoundoff(0); accountable.add(mass);
    ASSERT_GT(spill, mass); ASSERT_TRUE(accountable.atLeast(spill));
    EXPECT_FALSE(book.canFund(0, mass, spill));
    EXPECT_THROW(book.reserve(c, 4, {row(water, water, spill+1, spill)}), std::invalid_argument);
}
TEST(PassiveStorageRoundoff, SwallowedPartialAndInexactGrossPartitionRemainTerminal) {
    const double large = std::ldexp(1., 53);
    auto c = closedPool(1, large, 1); c.coupling_delivery_remaining = 2;
    QualitySolver solver; solver.init(1, 0, 1); PassiveStorageExchange book(Arithmetic::RETAIN_ROUNDOFF);
    book.reserve(c, 1, {row(1, 1, 3, 1)}); c.coupling_delivery_remaining = 1;
    EXPECT_THROW(solver.execute(c, 1, book), std::invalid_argument);
    EXPECT_TRUE(book.failed()); EXPECT_TRUE(book.storedRoundoff(0).zero());
    EXPECT_EQ(c.nodes.conc[0]*c.nodes.volume[0], large); EXPECT_EQ(book.pendingDrain(0), 3);
    auto other = closedPool(1, 100, 0); other.coupling_delivery_remaining = 3;
    PassiveStorageExchange partition(Arithmetic::RETAIN_ROUNDOFF);
    partition.reserve(other, 1, {row(1, 1, 1, 0)}); other.coupling_delivery_remaining = 2;
    EXPECT_THROW(solver.execute(other, 1, partition), std::invalid_argument);
    EXPECT_TRUE(partition.failed()); EXPECT_EQ(other.nodes.conc[0], 0);
    auto partial = closedPool(); partial.coupling_delivery_remaining = 2;
    PassiveStorageExchange sliced(Arithmetic::RETAIN_ROUNDOFF);
    sliced.reserve(partial, 1, {row(1, 1, 5.1, 5)});
    std::ostringstream audit; audit << "initial " << std::hexfloat << 500. << '\n';
    for (int i = 0; i < 2; ++i) {
        const double before = partial.nodes.volume[0]*partial.nodes.conc[0];
        ASSERT_NO_THROW(nativeStep(partial, solver, sliced, 1));
        audit << roundoffLine(before, row(.5, .5, 5.1/2, 2.5), partial, sliced);
    }
    EXPECT_EQ(sliced.pendingDrain(0), 0); EXPECT_EQ(sliced.pendingSpill(0), 0);
    archiveArtifact("roundoff-partial.txt", audit.str());
}
TEST(PassiveStorageRoundoff, VersionThreeRoundTripRejectsSemanticRemainderCorruptionAtomically) {
    auto c = closedPool(); QualitySolver solver; solver.init(1, 0, 1);
    PassiveStorageExchange source(Arithmetic::RETAIN_ROUNDOFF);
    source.reserve(c, 1, {row(1, 1, 5.1, 5)}); nativeStep(c, solver, source, 1);
    const auto bytes = source.checkpoint(c); ASSERT_FALSE(source.storedRoundoff(0).zero());
    archiveArtifact("roundoff-v3.bin", bytes);
    const auto n = source.storedRoundoff(0).parts().size();
    for (int mutation = 0; mutation < 7; ++mutation) {
        SCOPED_TRACE(mutation);
        const auto bad = forgedArchive(bytes, [&](std::string& p) {
            const auto parts = p.size()-n*sizeof(double)-sizeof(std::uint64_t);
            if (mutation <= 2) {
                double value; std::memcpy(&value, p.data()+p.size()-sizeof(double), sizeof(double));
                value = mutation == 0 ? value*2 : mutation == 1 ? 0 : std::numeric_limits<double>::infinity();
                std::memcpy(p.data()+p.size()-sizeof(double), &value, sizeof(double));
            } else if (mutation == 3) {
                const std::uint64_t excess = 65; std::memcpy(p.data()+parts, &excess, sizeof(excess));
            } else if (mutation == 4) {
                const int wrong = -1; std::memcpy(p.data()+parts-sizeof(int), &wrong, sizeof(wrong));
            } else if (mutation == 5) {
                const std::uint64_t empty = 0; p.resize(parts-sizeof(int));
                std::memcpy(p.data()+p.size()-sizeof(empty), &empty, sizeof(empty));
            } else {
                const std::uint64_t split = n+1; std::memcpy(p.data()+parts, &split, sizeof(split));
                double part; std::memcpy(&part, p.data()+p.size()-sizeof(part), sizeof(part)); part /= 2;
                std::memcpy(p.data()+p.size()-sizeof(part), &part, sizeof(part));
                p.append(reinterpret_cast<const char*>(&part), sizeof(part));
            }
        }, 3);
        PassiveStorageExchange target(Arithmetic::RETAIN_ROUNDOFF);
        EXPECT_THROW(target.restore(c, bad), std::exception); EXPECT_TRUE(target.checkpoint(c).empty());
        ASSERT_NO_THROW(target.restore(c, bytes)); EXPECT_EQ(target.checkpoint(c), bytes);
        EXPECT_EQ(target.storedRoundoff(0), source.storedRoundoff(0));
    }
    PassiveStorageExchange strict; EXPECT_THROW(strict.restore(c, bytes), std::invalid_argument);
}
TEST(PassiveStorageRoundoff, ArithmeticPolicyPersistsEvenWithZeroCurrentRemainder) {
    auto c = closedPool(); QualitySolver solver; solver.init(1, 0, 1);
    PassiveStorageExchange source(Arithmetic::RETAIN_ROUNDOFF);
    source.reserve(c, 1, {row(1, 1, 5, 5)}); nativeStep(c, solver, source, 1);
    ASSERT_TRUE(source.storedRoundoff(0).zero()); const auto bytes = source.checkpoint(c);
    Archive in(bytes); std::string name, payload; std::uint32_t v,e,crc; in(name,v,e,crc,payload); in.finish();
    EXPECT_EQ(v, 3u); PassiveStorageExchange strict; EXPECT_THROW(strict.restore(c, bytes), std::invalid_argument);
    PassiveStorageExchange resumed(Arithmetic::RETAIN_ROUNDOFF); ASSERT_NO_THROW(resumed.restore(c, bytes));
    EXPECT_EQ(resumed.checkpoint(c), bytes);
    PassiveStorageExchange absent(Arithmetic::RETAIN_ROUNDOFF);
    EXPECT_TRUE(absent.checkpoint(c).empty()); ASSERT_NO_THROW(absent.restore(c, {}));
    EXPECT_EQ(absent.arithmetic(), Arithmetic::RETAIN_ROUNDOFF);
}

namespace {
void aggregateTrace(std::ostream& out, double concentration, double volume,
                    const std::vector<Row>& rows, const SimulationContext& c,
                    const PassiveStorageExchange& book) {
    out << std::hexfloat << concentration << ' ' << volume << ' ' << c.nodes.conc[0] << " |";
    for (const auto& r : rows) out << ' ' << r.drain_mass << ' ' << r.spill_mass;
    out << " |";
    for (double part : book.storedRoundoff(0).parts()) out << ' ' << part;
    out << '\n';
}
}
TEST(PassiveStorageAggregates, ExactNodeGrossAndNetSurviveBothSignsAndRetiredRestart) {
    const double tiny = std::ldexp(1., -55);
    for (const double sign : {-1., 1.}) {
        auto c = closedPool(); QualitySolver solver; solver.init(1, 0, 1);
        PassiveStorageExchange book(Arithmetic::RETAIN_AGGREGATES);
        std::ostringstream audit; audit << "initial " << std::hexfloat << 500. << '\n';
        for (std::uint64_t id = 1; id <= 8; ++id) {
            const double before = c.nodes.conc[0], volume = c.nodes.volume[0];
            const std::vector<Row> rows{row(1, 1, before+sign, before, 0),
                row(tiny, tiny, (before+sign)*tiny, before*tiny, 1)};
            ASSERT_NO_THROW(book.reserve(c, id, rows));
            ASSERT_GT(book.pendingDrainAmount(0).parts().size(), 1u);
            ASSERT_GT(book.pendingSpillAmount(0).parts().size(), 1u);
            EXPECT_THROW(book.pendingDrain(0), std::invalid_argument);
            EXPECT_THROW(book.pendingSpill(0), std::invalid_argument);
            ASSERT_NO_THROW(nativeStep(c, solver, book, 1));
            aggregateTrace(audit, before, volume, rows, c, book);
        }
        ASSERT_FALSE(book.storedRoundoff(0).zero());
        const auto bytes = book.checkpoint(c);
        PassiveStorageExchange recovered(Arithmetic::RETAIN_AGGREGATES);
        ASSERT_NO_THROW(recovered.restore(c, bytes));
        EXPECT_EQ(recovered.checkpoint(c), bytes);
        EXPECT_EQ(recovered.storedRoundoff(0), book.storedRoundoff(0));
        const auto tag = sign > 0 ? "aggregate-positive" : "aggregate-negative";
        archiveArtifact(std::string(tag)+".txt", audit.str());
        archiveArtifact(std::string(tag)+"-v4.bin", bytes);
        PassiveStorageExchange previous(Arithmetic::RETAIN_ROUNDOFF);
        EXPECT_THROW(previous.restore(c, bytes), std::invalid_argument);
    }
}
TEST(PassiveStorageAggregates, ExactGrossOverdraftCannotRoundDownOrBorrowFutureDrain) {
    auto c = closedPool(); PassiveStorageExchange book(Arithmetic::RETAIN_AGGREGATES);
    book.reserve(c, 1, {row(100, 100, 500, 500)});
    const double tiny = std::ldexp(1., -55);
    const auto old = book.receipts();
    EXPECT_THROW(book.reserve(c, 2, {row(tiny, tiny, 2000*tiny, 5*tiny, 1)}), std::invalid_argument);
    EXPECT_EQ(book.receipts(), old); EXPECT_EQ(c.nodes.conc[0], 5);
    quality::NativeAmountSum spill; spill.add(500); spill.add(5*tiny);
    EXPECT_FALSE(book.canFund(0, 500, spill));
    QualitySolver solver; solver.init(1, 0, 1); nativeStep(c, solver, book, 1);
    ASSERT_NO_THROW(book.reserve(c, 2, {row(tiny, tiny, 2000*tiny, 5*tiny, 1)}));
}
TEST(PassiveStorageAggregates, ExactTargetBoundAndSwallowedIncrementsRemainStrict) {
    auto c = closedPool(1, 1, 1); PassiveStorageExchange book(Arithmetic::RETAIN_AGGREGATES);
    // RN(D) loses a significant part of the tiny positive final target after
    // near-complete cancellation. Its error must use RN(M+D), not old M.
    EXPECT_THROW(book.reserve(c, 1, {row(1, 1, 3*std::ldexp(1., -54), 1)}), std::invalid_argument);
    EXPECT_TRUE(book.receipts().empty()); EXPECT_EQ(c.nodes.conc[0], 1);
    auto large = closedPool(1, std::ldexp(1., 53), 1); large.coupling_delivery_remaining = 2;
    QualitySolver solver; solver.init(1, 0, 1); PassiveStorageExchange swallowed(Arithmetic::RETAIN_AGGREGATES);
    swallowed.reserve(large, 1, {row(1, 1, 3, 1)}); large.coupling_delivery_remaining = 1;
    EXPECT_THROW(solver.execute(large, 1, swallowed), std::invalid_argument);
    EXPECT_TRUE(swallowed.failed()); EXPECT_EQ(large.nodes.conc[0], 1);
    auto partial = closedPool(1, 100, 0); partial.coupling_delivery_remaining = 3;
    PassiveStorageExchange partition(Arithmetic::RETAIN_AGGREGATES);
    partition.reserve(partial, 1, {row(1, 1, 1, 0)}); partial.coupling_delivery_remaining = 2;
    EXPECT_THROW(solver.execute(partial, 1, partition), std::invalid_argument);
    EXPECT_TRUE(partition.failed()); EXPECT_EQ(partial.nodes.conc[0], 0);
}
TEST(PassiveStorageAggregates, NodeRemainderCorruptionRefusesBeforeInstallation) {
    auto c = closedPool(); QualitySolver solver; solver.init(1, 0, 1);
    PassiveStorageExchange source(Arithmetic::RETAIN_AGGREGATES);
    const double tiny=std::ldexp(1.,-55);
    source.reserve(c,1,{row(1,1,6,5),row(tiny,tiny,6*tiny,5*tiny,1)});
    nativeStep(c,solver,source,1); const auto bytes=source.checkpoint(c);
    const auto bad=forgedArchive(bytes,[](std::string& p){
        double v;std::memcpy(&v,p.data()+p.size()-sizeof(v),sizeof(v));v*=2;
        std::memcpy(p.data()+p.size()-sizeof(v),&v,sizeof(v));
    },4);
    PassiveStorageExchange target(Arithmetic::RETAIN_AGGREGATES);
    EXPECT_THROW(target.restore(c,bad),std::invalid_argument);
    EXPECT_TRUE(target.checkpoint(c).empty());
    ASSERT_NO_THROW(target.restore(c,bytes));
    EXPECT_EQ(target.checkpoint(c),bytes);
    auto quiet=closedPool();PassiveStorageExchange initial(Arithmetic::RETAIN_AGGREGATES);
    initial.reserve(quiet,1,{row(1,1,5,5)});nativeStep(quiet,solver,initial,1);
    ASSERT_TRUE(initial.storedRoundoff(0).zero());const auto zero=initial.checkpoint(quiet);
    for(const auto policy:{Arithmetic::EXACT,Arithmetic::RETAIN_ROUNDOFF}){
        PassiveStorageExchange incompatible(policy);
        EXPECT_THROW(incompatible.restore(quiet,zero),std::invalid_argument);
        EXPECT_TRUE(incompatible.checkpoint(quiet).empty());
    }
}
TEST(PassiveStorageAggregates, CorrectRoundingOracleTracesIncludeTiesSubnormalsAndOverflow) {
    std::ostringstream out; out << std::hexfloat;
    const auto write=[&](const std::vector<double>& values,bool required=true){
        out << (required ? 1 : 0) << " |";
        for(double x:values)out << ' ' << x;
        out << " | ";
        try {quality::NativeAmountSum sum;for(double x:values)sum.add(x);out << sum.rounded();}
        catch(const std::invalid_argument&){out << "refused";}
        out << '\n';
    };
    std::mt19937_64 rng(41723);
    for(int k=0;k<1024;++k){
        std::vector<double> values;
        for(int j=0;j<24;++j)values.push_back(std::ldexp((rng()%2 ? 1. : -1.)*double(1+rng()%1024)/1024,int(rng()%2065)-1064));
        if(k%3==0){const auto copy=values;for(double x:copy)values.push_back(-x);}
        std::shuffle(values.begin(),values.end(),rng);write(values);
    }
    const double half=std::ldexp(1.,-53),up=std::nextafter(1.,2.);
    write({.1,.2});write({1,half});write({up,half});write({-1,-half});write({-up,-half});
    write({1,-std::ldexp(1.,-54)});write({-1,std::ldexp(1.,-54)});
    const double minimum=std::numeric_limits<double>::min(),tiny=std::numeric_limits<double>::denorm_min();
    write({minimum,-std::nextafter(minimum,1.),2*tiny});write({tiny,tiny,-tiny});
    const double maximum=std::numeric_limits<double>::max();
    write({maximum,-std::ldexp(1.,970)});write({-maximum,std::ldexp(1.,970)});
    write({maximum,std::ldexp(1.,968)},false);write({maximum,maximum},false);
    write({1,-1});write({std::ldexp(1.,53),1,-std::ldexp(1.,53)});write({1,-1,tiny});
    archiveArtifact("aggregate-rounding.txt",out.str());
}

TEST(PassiveStoragePoints, CanonicalSparseAmountAndUnauthenticatedRefusalPreserveOwner) {
    quality::NativeAmountSum amount; amount.add(.1); amount.add(.2);
    Row r = row(1,1,0,5); r.setMaterialAmount(true,amount);
    ASSERT_EQ(r.drain_mass,0); ASSERT_GT(r.drain_mass_parts.size(),1u);
    EXPECT_EQ(r.materialAmount(true),amount);
    auto c=closedPool();
    for(const auto policy:{Arithmetic::EXACT,Arithmetic::RETAIN_ROUNDOFF,Arithmetic::RETAIN_AGGREGATES,Arithmetic::RETAIN_POINT_AGGREGATES}){
        PassiveStorageExchange book(policy);
        EXPECT_THROW(book.reserve(c,1,{r}),std::invalid_argument);
        EXPECT_TRUE(book.receipts().empty()); EXPECT_TRUE(book.checkpoint(c).empty());
        ASSERT_NO_THROW(book.reserve(c,1,{row(1,1,5,5)}));
    }
    auto bad=r;bad.drain_mass=1;EXPECT_THROW(bad.materialAmount(true),std::invalid_argument);
    bad=r;bad.drain_mass_parts={.1};EXPECT_THROW(bad.materialAmount(true),std::invalid_argument);
    bad=r;bad.drain_mass_parts={0,.3};EXPECT_THROW(bad.materialAmount(true),std::invalid_argument);
    bad=r;bad.drain_mass_parts={.3,.1};EXPECT_THROW(bad.materialAmount(true),std::invalid_argument);
    bad=r;for(double& part:bad.drain_mass_parts)part=-part;
    EXPECT_THROW(bad.materialAmount(true),std::invalid_argument);
    bad=r;bad.drain_mass_parts.assign(65,.1);EXPECT_THROW(bad.materialAmount(true),std::invalid_argument);
    r.setMaterialAmount(true,{});EXPECT_EQ(r.drain_mass,0);EXPECT_TRUE(r.drain_mass_parts.empty());
}
TEST(PassiveStoragePoints, RepresentedPartialDeliveryAndPolicyIdentityRemainStrict) {
    auto c=closedPool(); c.coupling_delivery_remaining=2;
    QualitySolver solver;solver.init(1,0,1);PassiveStorageExchange book(Arithmetic::RETAIN_POINT_AGGREGATES);
    book.reserve(c,1,{row(1,1,5.1,5)});
    for(int k=0;k<2;++k)ASSERT_NO_THROW(nativeStep(c,solver,book,1));
    ASSERT_EQ(book.pendingDrain(0),0);const auto bytes=book.checkpoint(c);
    for(const auto policy:{Arithmetic::EXACT,Arithmetic::RETAIN_ROUNDOFF,Arithmetic::RETAIN_AGGREGATES}){
        PassiveStorageExchange old(policy);EXPECT_THROW(old.restore(c,bytes),std::invalid_argument);
        EXPECT_TRUE(old.checkpoint(c).empty());
    }
    PassiveStorageExchange recovered(Arithmetic::RETAIN_POINT_AGGREGATES);
    ASSERT_NO_THROW(recovered.restore(c,bytes));EXPECT_EQ(recovered.checkpoint(c),bytes);
    auto partial=closedPool(1,100,0);partial.coupling_delivery_remaining=3;
    PassiveStorageExchange refused(Arithmetic::RETAIN_POINT_AGGREGATES);
    refused.reserve(partial,1,{row(1,1,1,0)});
    EXPECT_THROW(nativeStep(partial,solver,refused,1),std::invalid_argument);
    EXPECT_TRUE(refused.failed());EXPECT_EQ(partial.nodes.conc[0],0);
}

TEST(PassiveStorageHalves, StagedWholeDeliveryRecoversButPartialRequiresNativeProof) {
    auto c=closedPool();QualitySolver solver;solver.init(1,0,1);
    PassiveStorageExchange whole(Arithmetic::RETAIN_EXACT_HALVES);
    whole.reserve(c,1,{row(1,1,5.1,5)});
    ASSERT_NO_THROW(nativeStep(c,solver,whole,1));
    const auto bytes=whole.checkpoint(c);archiveArtifact("halves-staged-whole-v6.bin",bytes);
    PassiveStorageExchange restored(Arithmetic::RETAIN_EXACT_HALVES);
    ASSERT_NO_THROW(restored.restore(c,bytes));EXPECT_EQ(restored.checkpoint(c),bytes);
    EXPECT_EQ(restored.receipts().front().half_state,0);
    PassiveStorageExchange previous(Arithmetic::RETAIN_POINT_AGGREGATES);
    EXPECT_THROW(previous.restore(c,bytes),std::invalid_argument);
    auto partial=closedPool();partial.coupling_delivery_remaining=2;
    PassiveStorageExchange refused(Arithmetic::RETAIN_EXACT_HALVES);
    refused.reserve(partial,1,{row(1,1,5.1,5)});const auto rows=refused.receipts();
    EXPECT_THROW(nativeStep(partial,solver,refused,1),std::invalid_argument);
    EXPECT_EQ(refused.receipts(),rows);EXPECT_EQ(partial.nodes.conc[0],5);
    EXPECT_TRUE(refused.failed());
}
