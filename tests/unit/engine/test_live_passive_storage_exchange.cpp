// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include <gtest/gtest.h>
#include "core/SWMMEngine.hpp"
#include "quality/LivePassiveStorageExchange.hpp"
#include "quality/QualityRouting.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include "2d/solver/ExplicitInertialSolver.hpp"
#include <openswmm/engine/openswmm_hotstart.h>
#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_forcing.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include "core/CompletedSourceArchive.hpp"
#include "core/CheckpointInspection.hpp"
#include <cstring>
#include <functional>
#include <iomanip>
#include <limits>
#include <cstdlib>
#include <sstream>
#include "live_passive_recovery_test_support.hpp"

namespace {
using namespace openswmm;
using namespace openswmm::twoD;
using quality::LivePassiveStorageExchange;
using Phase = quality::PassiveStorageExchange::Phase;
constexpr double dt = 1.0 / 1024;
constexpr double ft3 = .3048 * .3048 * .3048;

// Manufactured, disconnected surface pools with genuine native ponded donor
// inventory above full_volume. The real marcher owns all ORIFICE/water writes.
// This is deliberately distinct from an authored whole-engine witness.
struct Marcher {
    SimulationContext ctx;
    MeshData mesh;
    SurfaceStateData state;
    SolverOptions2D options;
    ExplicitInertialSolver solver;
    std::vector<CouplingPoint> points;
    std::vector<double> published;
    std::unique_ptr<LivePassiveStorageExchange> owner;
    explicit Marcher(double node_c = 1, double surface_c = 1, std::size_t capacity = 4096,
                     quality::PassiveStorageArithmetic arithmetic = quality::PassiveStorageArithmetic::EXACT, bool preflight = false, int pairs = 1, double initial_span = 0) {
        ctx.node_names.add("Tank"); ctx.pollutant_names.add("Dye"); ctx.allocate_objects();
        ctx.mass_balance.resize_quality(1);
        ctx.coupling_delivery_remaining = initial_span;
        ctx.options.routing_model = RoutingModel::DYNWAVE;
        ctx.node_subtypes.set_node_type(ctx.nodes, 0, NodeType::STORAGE);
        ctx.nodes.volume[0] = ctx.nodes.old_volume[0] = 100;
        ctx.nodes.depth[0] = ctx.nodes.old_depth[0] = 10;
        ctx.nodes.invert_elev[0] = -10; ctx.nodes.head[0] = 0;
        ctx.nodes.full_depth[0] = 1; ctx.nodes.full_volume[0] = 10;
        ctx.nodes.conc[0] = ctx.nodes.conc_old[0] = node_c;
        mesh.resize_vertices(6*pairs); mesh.resize_triangles(2*pairs);
        for (int k = 0; k < 2*pairs; ++k) {
            const int v = 3 * k;
            mesh.vx[v] = 4 * k; mesh.vy[v] = 0;
            mesh.vx[v+1] = 4 * k + 2; mesh.vy[v+1] = 0;
            mesh.vx[v+2] = 4 * k; mesh.vy[v+2] = 1;
            for (int j = 0; j < 3; ++j) mesh.vz[v+j] = -1;
            mesh.set_triangle(k, v, v+1, v+2); mesh.mannings_n[k] = .03;
            points.push_back({k, -1, 0, .7, .01, false, false});
        }
        buildMeshTopology(mesh);
        state.resize(2*pairs, 6*pairs); state.transport.resize(1, 2*pairs, 2*pairs);
        state.transport.cell_runoff_vol.assign(2*pairs, 0);
        for (int k = 0; k < 2*pairs; ++k) {
            state.volume[k] = state.depth[k] = k%2 == 0 ? 1.5 : .5;
            state.head[k] = -1 + state.depth[k];
            state.transport.cell_mass[k] = state.volume[k] * surface_c;
        }
        published = {node_c}; state.nodes_1d = &ctx.nodes;
        state.node_row_conc = &published; state.node_coupling = &points;
        options.backend = Backend2D::CPU; options.lts_tiers = 1; options.max_timestep = dt;
        options.len_1d_to_2d = .3048; options.vol_1d_to_2d = ft3;
        options.flow_2d_to_1d = 1 / ft3;
        solver.initialize(mesh, state, options);
        owner = std::make_unique<LivePassiveStorageExchange>(ctx, state, points, options.flow_2d_to_1d, capacity, arithmetic, preflight);
    }
    void march(Phase phase = Phase::ADVANCE) {
        if (phase == Phase::ADVANCE) { owner->freeze(ctx, dt); owner->observeDrain(ctx); }
        else ctx.elapsed_ms = 1000 * dt;
        owner->beginWindow(ctx, state, 0, 1000 * dt, phase);
        state.live_passive_exchange = owner.get();
        try {
            solver.advance(0, dt);
            owner->seal(ctx, solver.last_coupling_exchange());
            state.live_passive_exchange = nullptr;
        } catch (...) {
            state.live_passive_exchange = nullptr;
            owner->fail("Manufactured native advance failed.");
            throw;
        }
    }
};

std::filesystem::path evidence(const std::string& name) {
    const char* chosen = std::getenv("OPENSWMM_LIVE_EXCHANGE_EVIDENCE");
    auto p = std::filesystem::path(chosen ? chosen : LIVE_EXCHANGE_TEST_OUTPUT) / name;
    std::filesystem::create_directories(p.parent_path()); return p;
}
std::string deck(double surface_depth = .5, double sync = 0) {
    return "[OPTIONS]\nFLOW_UNITS CMS\nFLOW_ROUTING DYNWAVE\nSTART_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
        "END_DATE 01/01/2026\nEND_TIME 00:00:10\nROUTING_STEP 1\nVARIABLE_STEP 0\nREPORT_STEP 00:00:01\nTHREADS 1\n"
        "[POLLUTANTS]\nDye MG/L 0 0 0 0\n[STORAGE]\nTank 0 1 1 FUNCTIONAL 0 0 100 0 0\n"
        "[2D_OPTIONS]\nINTEGRATOR EXPLICIT\nBACKEND CPU\nLTS_TIERS 1\nMAX_TIMESTEP 1\nCOUPLING_SYNC " + std::to_string(sync) +
        "\nREPORT_2D NO\n[2D_VERTICES]\n0 0 1\n2 0 1\n0 1 1\n[2D_TRIANGLES]\n0 1 2 .03 " + std::to_string(surface_depth) +
        "\n[2D_VERTEX_NODE_MAP]\n0 Tank .7 .01\n[2D_INITIAL_QUALITY]\n* Dye 1\n[REPORT]\nINPUT NO\n";
}
void start(SWMMEngine& e, const std::string& name, double depth = .5, double sync = 0, bool binary = false) {
    const auto p = evidence(name);
    std::ofstream(p.string()+".inp") << deck(depth, sync);
    ASSERT_EQ(e.open((p.string()+".inp").c_str(), (p.string()+".rpt").c_str(), binary ? (p.string()+".out").c_str() : nullptr), SWMM_OK) << e.context().error_message;
    ASSERT_EQ(e.initialize(), SWMM_OK) << e.context().error_message;
    ASSERT_EQ(e.start(binary ? 1 : 0), SWMM_OK) << e.context().error_message;
    ASSERT_TRUE(e.surfaceRouter2D().isActive());
}
}

TEST(LivePassiveStorageExchange, NativeMarcherAdmitsBothGrossDirectionsAndSealsNextCohort) {
    Marcher f;
    const double initial = 100 + (f.state.transport.cell_mass[0] + f.state.transport.cell_mass[1]) / ft3;
    ASSERT_GT(f.ctx.nodes.volume[0], f.ctx.nodes.full_volume[0]);
    ASSERT_NO_THROW(f.march());
    ASSERT_EQ(f.owner->legCount(), 2u);
    EXPECT_TRUE(f.owner->leg(0).drain); EXPECT_FALSE(f.owner->leg(1).drain);
    for (int k = 0; k < 2; ++k) {
        const auto& l = f.owner->leg(k);
        EXPECT_TRUE(l.applied); EXPECT_GT(l.water_si, 0); EXPECT_GT(l.mass_si, 0);
        EXPECT_EQ(l.mass_native, l.mass_si * f.options.flow_2d_to_1d); EXPECT_EQ(l.sample, 1);
    }
    EXPECT_EQ(f.solver.last_coupling_exchange()[0], -f.solver.last_coupling_exchange()[1]);
    EXPECT_EQ(f.ctx.nodes.coupling_qual_queue[0], 0);
    ASSERT_EQ(f.owner->receipts().receipts().size(), 1u);
    EXPECT_EQ(f.owner->receipts().receipts()[0].publication.phase, Phase::ADVANCE);
    quality::QualitySolver quality; quality.init(1, 0, 1);
    f.owner->receive(f.ctx, quality, dt); // frozen empty current cohort
    EXPECT_GT(f.owner->receipts().pendingSpill(0), 0);
    f.ctx.elapsed_ms += 1000 * dt;
    f.owner->freeze(f.ctx, dt); f.owner->observeDrain(f.ctx); f.owner->receive(f.ctx, quality, dt);
    EXPECT_EQ(f.owner->receipts().pendingSpill(0), 0);
    EXPECT_NEAR(f.ctx.nodes.volume[0] * f.ctx.nodes.conc[0] +
        (f.state.transport.cell_mass[0] + f.state.transport.cell_mass[1]) / ft3, initial, 1e-12);
}

TEST(LivePassiveStorageExchange, NativeZeroConcentrationStillAuthenticatesBothWaterLegs) {
    Marcher f(0, 0); ASSERT_NO_THROW(f.march());
    ASSERT_EQ(f.owner->legCount(), 2u);
    for (int k = 0; k < 2; ++k) { EXPECT_GT(f.owner->leg(k).water_si, 0); EXPECT_EQ(f.owner->leg(k).mass_si, 0); }
    EXPECT_EQ(f.owner->receipts().receipts().size(), 1u);
}

TEST(LivePassiveStorageExchange, ActualSpillCannotBorrowPendingOrCurrentDrain) {
    Marcher f;
    f.owner->reserveInitial(f.ctx, {{99, 0, "Dye", MassUnits::MG_PER_L, 100, 100, 100, 100}});
    const double v = f.state.volume[1], m = f.state.transport.cell_mass[1];
    EXPECT_THROW(f.march(), std::exception);
    ASSERT_TRUE(f.owner->failed()); ASSERT_EQ(f.owner->legCount(), 1u);
    EXPECT_TRUE(f.owner->leg(0).drain); EXPECT_TRUE(f.owner->leg(0).applied);
    EXPECT_LT(f.state.volume[0], 1.5); EXPECT_GT(f.owner->attempted().mass_si, 0);
    EXPECT_EQ(f.state.volume[1], v); EXPECT_EQ(f.state.transport.cell_mass[1], m);
    EXPECT_EQ(f.state.coupling_applied[1], 0); EXPECT_EQ(f.state.transport.exch_spill[1], 0);
    EXPECT_EQ(f.state.transport.cell_runoff_vol[1], 0); EXPECT_EQ(f.state.transport.gained_coupling[0], 0);
    EXPECT_EQ(f.solver.last_coupling_exchange()[1], 0);
    EXPECT_EQ(f.owner->receipts().pendingSpill(0), 100);
    EXPECT_EQ(f.state.live_passive_exchange, nullptr);
}

TEST(LivePassiveStorageExchange, NativeCapacityRefusalPrecedesAllSpillEffects) {
    Marcher f(1, 1, 1);
    EXPECT_THROW(f.march(), std::exception);
    EXPECT_TRUE(f.owner->failed()); ASSERT_EQ(f.owner->legCount(), 1u);
    EXPECT_TRUE(f.owner->leg(0).applied);
    EXPECT_EQ(f.state.volume[1], .5); EXPECT_EQ(f.state.transport.cell_mass[1], .5);
    EXPECT_EQ(f.state.coupling_applied[1], 0); EXPECT_EQ(f.state.transport.exch_spill[1], 0);
    const auto volume = f.state.volume, mass = f.state.transport.cell_mass;
    f.state.live_passive_exchange = f.owner.get();
    EXPECT_THROW(f.solver.advance(0, dt), std::exception);
    f.state.live_passive_exchange = nullptr;
    EXPECT_EQ(f.state.volume, volume); EXPECT_EQ(f.state.transport.cell_mass, mass);
    EXPECT_EQ(f.owner->legCount(), 1u);
}

TEST(LivePassiveStorageExchange, UnsealableActualDrainRetainsAppliedLegAndNoLegacyFallback) {
    Marcher f; f.points[1].cd = 0;
    EXPECT_THROW(f.march(), std::exception);
    ASSERT_EQ(f.owner->legCount(), 1u); EXPECT_TRUE(f.owner->leg(0).applied);
    EXPECT_TRUE(f.owner->windowOpen()); EXPECT_TRUE(f.owner->failed());
    EXPECT_TRUE(f.owner->receipts().receipts().empty()); EXPECT_EQ(f.ctx.nodes.coupling_qual_queue[0], 0);
    EXPECT_THROW(f.owner->beginWindow(f.ctx, f.state, 0, 1000 * dt, Phase::ADVANCE), std::exception);
}

TEST(LivePassiveStorageExchange, RK2RefusesBeforeAnyNativeLegOrTrial) {
    Marcher f; f.options.reconstruction_order = 2;
    const auto volume = f.state.volume, mass = f.state.transport.cell_mass;
    EXPECT_THROW(f.march(), std::exception);
    EXPECT_EQ(f.state.volume, volume); EXPECT_EQ(f.state.transport.cell_mass, mass);
    EXPECT_EQ(f.owner->legCount(), 0u); EXPECT_TRUE(f.owner->failed());
}

TEST(LivePassiveStorageExchange, ActualReportAndFinalFiringSamplesAreRetainedWithoutExtraReceive) {
    for (auto phase : {Phase::REPORT, Phase::FINAL}) {
        Marcher f; ASSERT_NO_THROW(f.march(phase));
        ASSERT_EQ(f.owner->receipts().receipts().size(), 1u);
        const auto& r = f.owner->receipts().receipts()[0];
        EXPECT_EQ(r.publication.phase, phase); EXPECT_EQ(r.publication.capture_ms, 1000 * dt);
        EXPECT_EQ(r.samples[0].concentration, 1); EXPECT_GT(f.owner->receipts().pendingSpill(0), 0);
    }
}

TEST(LivePassiveStorageExchange, SourceRepresentationLossIsTerminalBeforeCredit) {
    Marcher f; f.owner->beginWindow(f.ctx, f.state, 0, 1000 * dt, Phase::REPORT);
    EXPECT_FALSE(f.owner->admit(1, 1, 0, -1e-100, 1e-100, 1e100, .5, 0, dt));
    EXPECT_TRUE(f.owner->failed()); EXPECT_EQ(f.owner->legCount(), 0u);
    EXPECT_EQ(f.state.transport.cell_mass[1], .5);
}

TEST(LivePassiveStorageExchange, CanonicalEngineFailureCannotRetryOnStepOrEnd) {
    SWMMEngine e; ASSERT_NO_FATAL_FAILURE(start(e, "canonical-drain"));
    ASSERT_TRUE(e.attachLivePassiveExchange().empty());
    ASSERT_NE(e.livePassiveExchange(), nullptr);
    double elapsed = 0;
    int rc = SWMM_OK;
    for (int steps = 0; steps < 100 && rc == SWMM_OK && !e.livePassiveExchange()->failed(); ++steps)
        rc = e.step(&elapsed);
    EXPECT_NE(rc, SWMM_OK);
    const auto* owner = e.livePassiveExchange(); ASSERT_TRUE(owner->failed());
    ASSERT_GT(owner->legCount(), 0u); EXPECT_TRUE(owner->leg(0).applied);
    const auto count = owner->legCount(); const auto water = e.surfaceRouter2D().state().volume;
    const auto error = e.context().error_message;
    EXPECT_NE(e.step(&elapsed), SWMM_OK); EXPECT_NE(e.end(), SWMM_OK);
    EXPECT_EQ(owner->legCount(), count); EXPECT_EQ(e.surfaceRouter2D().state().volume, water);
    EXPECT_EQ(e.surfaceRouter2D().state().live_passive_exchange, nullptr);
    EXPECT_EQ(e.context().error_message, error); EXPECT_EQ(e.close(), SWMM_OK);
}

TEST(LivePassiveStorageExchange, CanonicalEmptyWindowsKeepNativeExecutionAndRefuseUnpairedHotstart) {
    SWMMEngine e; ASSERT_NO_FATAL_FAILURE(start(e, "canonical-empty", 0));
    ASSERT_TRUE(e.attachLivePassiveExchange().empty());
    ASSERT_TRUE(e.enableCheckpointing().empty());
    const auto p = evidence("must-not-save.hsf");
    EXPECT_NE(swmm_hotstart_save(&e, p.c_str()), SWMM_OK); EXPECT_FALSE(std::filesystem::exists(p));
    double elapsed = 0; ASSERT_EQ(e.step(&elapsed), SWMM_OK) << e.context().error_message;
    EXPECT_EQ(e.livePassiveExchange()->legCount(), 0u);
    EXPECT_FALSE(e.livePassiveExchange()->failed()); EXPECT_EQ(e.end(), SWMM_OK);
    EXPECT_EQ(e.close(), SWMM_OK);
}

TEST(LivePassiveStorageExchange, CanonicalAttachmentRefusesExistingCheckpointOrRuntimeProfile) {
    SWMMEngine e;
    const auto p = evidence("existing-checkpoint");
    const auto model = deck(0);
    std::ofstream(p.string()+".inp") << model.substr(0, model.find("[2D_OPTIONS]"))
        << "[OUTFALLS]\nOut -1 FREE NO\n[CONDUITS]\nC Tank Out 30 .013 0 0 0\n[XSECTIONS]\nC CIRCULAR .3 0 0 0 1\n";
    ASSERT_EQ(e.open((p.string()+".inp").c_str(), (p.string()+".rpt").c_str(), nullptr), SWMM_OK);
    ASSERT_EQ(e.initialize(), SWMM_OK) << e.context().error_message; ASSERT_EQ(e.start(0), SWMM_OK);
    const auto error = e.enableCheckpointing(); ASSERT_TRUE(error.empty()) << error;
    EXPECT_FALSE(e.attachLivePassiveExchange().empty()); EXPECT_EQ(e.livePassiveExchange(), nullptr);
    EXPECT_EQ(e.close(), SWMM_OK);
    SWMMEngine second; ASSERT_NO_FATAL_FAILURE(start(second, "unsupported-order", 0));
    second.surfaceRouter2D().options().reconstruction_order = 2;
    EXPECT_FALSE(second.attachLivePassiveExchange().empty()); EXPECT_EQ(second.livePassiveExchange(), nullptr);
    EXPECT_EQ(second.close(), SWMM_OK);
}

TEST(LivePassiveStorageExchange, PriorBoundDonorChangeRefusesBeforeNativeCredit) {
    Marcher f;
    f.owner->reserveInitial(f.ctx, {{99, 0, "Dye", MassUnits::MG_PER_L, 1, 1, 1, 1}});
    f.ctx.nodes.conc[0] = f.ctx.nodes.conc_old[0] = f.published[0] = 2;
    const auto volume = f.state.volume, mass = f.state.transport.cell_mass;
    EXPECT_THROW(f.march(), std::exception);
    EXPECT_TRUE(f.owner->failed()); EXPECT_EQ(f.owner->legCount(), 0u);
    EXPECT_EQ(f.state.volume, volume); EXPECT_EQ(f.state.transport.cell_mass, mass);
    EXPECT_EQ(f.owner->receipts().pendingSpill(0), 1);
}

TEST(LivePassiveStorageExchange, CapacityBoundsPrecedeAllocation) {
    Marcher f;
    EXPECT_THROW(LivePassiveStorageExchange(f.ctx, f.state, f.points, 1 / ft3, std::size_t(-1)), std::invalid_argument);
    EXPECT_THROW(LivePassiveStorageExchange(f.ctx, f.state, f.points, 1 / ft3, 0), std::invalid_argument);
}

TEST(LivePassiveStorageExchange, ActualSISpillSampleCannotBeFabricated) {
    Marcher f;
    f.owner->beginWindow(f.ctx, f.state, 0, 1000 * dt, Phase::REPORT);
    EXPECT_FALSE(f.owner->admit(1, 1, 0, -.01, .005, .5, .5, 0, dt));
    EXPECT_TRUE(f.owner->failed()); EXPECT_EQ(f.owner->legCount(), 0u);
    EXPECT_EQ(f.state.volume[1], .5); EXPECT_EQ(f.state.transport.cell_mass[1], .5);
}

TEST(LivePassiveStorageExchange, ActualSILegsPreserveConversionRoundoffThroughSealedNativeDelivery) {
    // Arithmetic/API witness only: the live hooks use the same native SI products.
    // The authenticated archive keeps the actual native product.
    Marcher f(.3, .3);
    f.state.transport.cell_mass[0] = .3; f.state.volume[0] = 1;
    f.ctx.elapsed_ms = 1000 * dt;
    f.owner->beginWindow(f.ctx, f.state, 0, 1000 * dt, Phase::FINAL);
    const double water = .1, actual = water * .3;
    ASSERT_NE(actual * f.options.flow_2d_to_1d, (water * f.options.flow_2d_to_1d) * .3);
    ASSERT_TRUE(f.owner->admit(0, 0, 0, water, actual, f.state.transport.cell_mass[0], f.state.volume[0], 0, dt));
    f.state.transport.cell_mass[0] -= actual; f.state.volume[0] -= water; f.owner->applied(f.state.transport.cell_mass[0], f.state.volume[0]);
    ASSERT_TRUE(f.owner->admit(1, 1, 0, -water, actual, f.state.transport.cell_mass[1], f.state.volume[1], 0, dt, water));
    f.state.transport.cell_mass[1] += actual; f.state.volume[1] += water; f.owner->applied(f.state.transport.cell_mass[1], f.state.volume[1]);
    ASSERT_NO_THROW(f.owner->seal(f.ctx, {water, -water}));
    EXPECT_FALSE(f.owner->failed()); ASSERT_EQ(f.owner->legCount(), 2u);
    EXPECT_TRUE(f.owner->leg(1).applied); EXPECT_EQ(f.owner->leg(1).mass_native, actual * f.options.flow_2d_to_1d);
    ASSERT_EQ(f.owner->receipts().receipts().size(), 1u);
    EXPECT_TRUE(f.owner->receipts().receipts()[0].authenticated);
    quality::PassiveStorageExchange staged;
    EXPECT_THROW(staged.reserveAtBoundary(f.ctx, 1, f.owner->receipts().receipts()[0].accepted,
        Phase::FINAL, 0, 1000 * dt), std::invalid_argument);
}

TEST(LivePassiveStorageExchange, EventsAndHeadOverridesRefuseAtAttachmentAndBeforeNextStep) {
    SWMMEngine e; ASSERT_NO_FATAL_FAILURE(start(e, "event-profile", 0));
    e.context().events.push_back({e.context().options.start_date, e.context().options.end_date});
    EXPECT_FALSE(e.attachLivePassiveExchange().empty()); EXPECT_EQ(e.livePassiveExchange(), nullptr);
    e.context().events.clear();
    e.context().forcing.node_head_boundary_mode[0] = ForcingMode::OVERRIDE;
    EXPECT_FALSE(e.attachLivePassiveExchange().empty()); EXPECT_EQ(e.livePassiveExchange(), nullptr);
    e.context().forcing.node_head_boundary_mode[0] = ForcingMode::NONE;
    ASSERT_TRUE(e.attachLivePassiveExchange().empty());
    const auto water = e.surfaceRouter2D().state().volume;
    const auto clock = e.context().elapsed_ms;
    e.context().events.push_back({e.context().options.start_date, e.context().options.end_date});
    double elapsed = 0; EXPECT_NE(e.step(&elapsed), SWMM_OK);
    EXPECT_TRUE(e.livePassiveExchange()->failed()); EXPECT_EQ(e.livePassiveExchange()->legCount(), 0u);
    EXPECT_EQ(e.context().elapsed_ms, clock); EXPECT_EQ(e.surfaceRouter2D().state().volume, water);
    EXPECT_EQ(e.close(), SWMM_OK);
}

TEST(LivePassiveStorageExchange, CanonicalFinalFlushSamplesActualNativeStateAndCannotRetryFailure) {
    SWMMEngine e; ASSERT_NO_FATAL_FAILURE(start(e, "canonical-final", .5, 100));
    ASSERT_TRUE(e.attachLivePassiveExchange().empty());
    double elapsed = 0; ASSERT_EQ(e.step(&elapsed), SWMM_OK) << e.context().error_message;
    ASSERT_EQ(e.livePassiveExchange()->legCount(), 0u);
    EXPECT_NE(e.end(), SWMM_OK);
    const auto* owner = e.livePassiveExchange(); ASSERT_TRUE(owner->failed()); ASSERT_GT(owner->legCount(), 0u);
    EXPECT_EQ(owner->leg(0).phase, Phase::FINAL); EXPECT_EQ(owner->leg(0).sample, e.context().nodes.conc[0]);
    EXPECT_TRUE(owner->leg(0).applied);
    const auto water = e.surfaceRouter2D().state().volume;
    const auto count = owner->legCount(); EXPECT_NE(e.end(), SWMM_OK);
    EXPECT_EQ(owner->legCount(), count); EXPECT_EQ(e.surfaceRouter2D().state().volume, water);
    EXPECT_EQ(e.surfaceRouter2D().state().live_passive_exchange, nullptr); EXPECT_EQ(e.close(), SWMM_OK);
}

TEST(LivePassiveStorageExchange, CanonicalBinaryReportFlushUsesActualPostQualitySample) {
    SWMMEngine e; ASSERT_NO_FATAL_FAILURE(start(e, "canonical-report", .5, 100, true));
    ASSERT_TRUE(e.attachLivePassiveExchange().empty());
    double elapsed = 0; int rc = SWMM_OK;
    for (int steps = 0; steps < 20 && rc == SWMM_OK; ++steps) rc = e.step(&elapsed);
    EXPECT_NE(rc, SWMM_OK);
    const auto* owner = e.livePassiveExchange(); ASSERT_TRUE(owner->failed()); ASSERT_GT(owner->legCount(), 0u);
    EXPECT_EQ(owner->leg(0).phase, Phase::REPORT); EXPECT_EQ(owner->leg(0).sample, e.context().nodes.conc[0]);
    EXPECT_EQ(owner->leg(0).batch_end_ms, e.context().elapsed_ms); EXPECT_GT(e.context().elapsed_ms, 0);
    EXPECT_TRUE(owner->leg(0).applied); EXPECT_TRUE(owner->receipts().receipts().empty());
    const auto water = e.surfaceRouter2D().state().volume;
    EXPECT_NE(e.end(), SWMM_OK); EXPECT_EQ(e.surfaceRouter2D().state().volume, water);
    EXPECT_EQ(e.surfaceRouter2D().state().live_passive_exchange, nullptr); EXPECT_EQ(e.close(), SWMM_OK);
}

TEST(LivePassiveRecovery, InitialPairedCaptureRestoreResaveAndNextNativeStep) {
    SWMMEngine original; ASSERT_NO_FATAL_FAILURE(start(original, "paired-initial-a", 0));
    ASSERT_TRUE(original.attachLivePassiveExchange().empty());
    const auto enabled = original.enableCheckpointing(); ASSERT_TRUE(enabled.empty()) << enabled;
    const auto a = evidence("paired-initial-a.bin"), b = evidence("paired-initial-b.bin");
    auto error = original.saveCheckpoint(a.string()); ASSERT_TRUE(error.empty()) << error;
    SWMMEngine resumed; ASSERT_NO_FATAL_FAILURE(start(resumed, "paired-initial-b", 0));
    ASSERT_TRUE(resumed.attachLivePassiveExchange().empty());
    error = resumed.enableCheckpointing(); ASSERT_TRUE(error.empty()) << error;
    error = resumed.loadCheckpoint(a.string()); ASSERT_TRUE(error.empty()) << error;
    error = resumed.saveCheckpoint(b.string()); ASSERT_TRUE(error.empty()) << error;
    const auto read = [](const auto& p) { std::ifstream f(p, std::ios::binary); return std::string(std::istreambuf_iterator<char>(f), {}); };
    EXPECT_EQ(read(a), read(b));
    double elapsed = 0;
    ASSERT_EQ(original.step(&elapsed), SWMM_OK) << original.context().error_message;
    ASSERT_EQ(resumed.step(&elapsed), SWMM_OK) << resumed.context().error_message;
    EXPECT_EQ(original.context().nodes.conc, resumed.context().nodes.conc);
    EXPECT_EQ(original.surfaceRouter2D().state().volume, resumed.surfaceRouter2D().state().volume);
    EXPECT_TRUE(original.saveCheckpoint(a.string()).empty()); EXPECT_TRUE(resumed.saveCheckpoint(b.string()).empty());
    EXPECT_EQ(read(a), read(b)); EXPECT_EQ(original.close(), SWMM_OK); EXPECT_EQ(resumed.close(), SWMM_OK);
}

TEST(LivePassiveRecovery, NativeDrySurfaceSpillUsesTheWetNetworkDonor) {
    Marcher f;
    f.state.volume = {2, 0}; f.state.depth = {2, 0}; f.state.head = {1, -1};
    f.state.transport.cell_mass = {2, 0};
    ASSERT_NO_THROW(f.march()); ASSERT_EQ(f.owner->legCount(), 2u);
    EXPECT_EQ(f.owner->leg(1).volume_before, 0); EXPECT_GT(f.owner->leg(1).volume_after, 0);
    EXPECT_TRUE(f.owner->leg(1).applied); EXPECT_TRUE(f.owner->receipts().receipts()[0].authenticated);
}

namespace {
void startPonded(SWMMEngine& e, const std::string& name, const std::string& node_c = "1",
                 const std::string& surface_c = "1", const std::string& area = "2",
                 const std::string& sync = ".0009765625", bool compact = false) {
    const auto p = evidence(name);
    std::ofstream(p.string()+".inp") <<
        "[OPTIONS]\nFLOW_UNITS CMS\nFLOW_ROUTING DYNWAVE\nSTART_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
        "END_DATE 01/01/2026\nEND_TIME 00:00:01\nROUTING_STEP 0.0009765625\nVARIABLE_STEP 0\nREPORT_STEP 00:00:01\nTHREADS 1\nALLOW_PONDING YES\n"
        "[POLLUTANTS]\nDye MG/L 0 0 0 0 NO * 0 0 " << node_c <<
        "\n[STORAGE]\nTank -10 1 10 FUNCTIONAL 0 0 " << area << " " << area << " 0\n"
        "[2D_OPTIONS]\nINTEGRATOR EXPLICIT\nBACKEND CPU\nLTS_TIERS 1\nMAX_TIMESTEP " << sync <<
        "\nCOUPLING_SYNC " << sync << "\nREPORT_2D NO\n"
        "[2D_VERTICES]\n" << (compact ?
        "0 0 -1\n.03 0 -1\n0 .01999777301694915 -1\n0 -.01999777301694915 -1\n"
        "0 -.0399955460338983 -1\n.03 -.01999777301694915 -1\n" :
        "0 0 -1\n2 0 -1\n0 1 -1\n4 0 -1\n6 0 -1\n4 1 -1\n") <<
        "[2D_TRIANGLES]\n0 1 2 .03 1.5\n3 4 5 .03 .5\n[2D_VERTEX_NODE_MAP]\n0 Tank .7 .01\n3 Tank .7 .01\n"
        "[2D_INITIAL_QUALITY]\n* Dye " << surface_c << "\n[REPORT]\nINPUT NO\n";
    ASSERT_EQ(e.open((p.string()+".inp").c_str(), (p.string()+".rpt").c_str(), nullptr), SWMM_OK) << e.context().error_message;
    ASSERT_EQ(e.initialize(), SWMM_OK) << e.context().error_message;
    ASSERT_EQ(e.start(0), SWMM_OK) << e.context().error_message;
    ASSERT_TRUE(e.surfaceRouter2D().isActive());
}
std::string readBytes(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary); return std::string(std::istreambuf_iterator<char>(f), {});
}
}
TEST(LivePassiveRecovery, AuthoredPondedStorageCapturesBothActualDirectionsAndResumesPendingProof) {
    SWMMEngine original; ASSERT_NO_FATAL_FAILURE(startPonded(original, "ponded-original"));
    const auto& c = original.context();
    ASSERT_GT(c.nodes.volume[0], c.nodes.full_volume[0]); ASSERT_EQ(c.nodes.head[0], 0);
    ASSERT_EQ(c.nodes.conc[0], 1);
    ASSERT_TRUE(original.attachLivePassiveExchange().empty());
    auto error = original.enableCheckpointing(); ASSERT_TRUE(error.empty()) << error;
    double elapsed = 0;
    ASSERT_EQ(original.step(&elapsed), SWMM_OK) << original.context().error_message;
    const auto* owner = original.livePassiveExchange(); ASSERT_EQ(owner->legCount(), 2u);
    EXPECT_TRUE(owner->leg(0).drain); EXPECT_FALSE(owner->leg(1).drain);
    ASSERT_GT(owner->receipts().pendingSpill(0), 0);
    const auto a = evidence("ponded-original.bin"), b = evidence("ponded-resumed.bin");
    error = original.saveCheckpoint(a.string()); ASSERT_TRUE(error.empty()) << error;
    SWMMEngine resumed; ASSERT_NO_FATAL_FAILURE(startPonded(resumed, "ponded-resumed"));
    ASSERT_TRUE(resumed.attachLivePassiveExchange().empty());
    error = resumed.enableCheckpointing(); ASSERT_TRUE(error.empty()) << error;
    error = resumed.loadCheckpoint(a.string()); ASSERT_TRUE(error.empty()) << error;
    error = resumed.saveCheckpoint(b.string()); ASSERT_TRUE(error.empty()) << error;
    EXPECT_EQ(readBytes(a), readBytes(b));
    EXPECT_EQ(resumed.livePassiveExchange()->receipts().receipts(), owner->receipts().receipts());
    EXPECT_EQ(resumed.surfaceRouter2D().state().transport.cell_mass, original.surfaceRouter2D().state().transport.cell_mass);
    const int rc = original.step(&elapsed), resumed_rc = resumed.step(&elapsed);
    EXPECT_EQ(rc, resumed_rc);
    ASSERT_EQ(rc, SWMM_OK) << original.context().error_message;
    EXPECT_EQ(original.context().nodes.conc, resumed.context().nodes.conc);
    EXPECT_EQ(original.surfaceRouter2D().state().volume, resumed.surfaceRouter2D().state().volume);
    EXPECT_EQ(original.surfaceRouter2D().state().transport.cell_mass, resumed.surfaceRouter2D().state().transport.cell_mass);
    if (rc == SWMM_OK) {
        EXPECT_TRUE(original.saveCheckpoint(a.string()).empty()); EXPECT_TRUE(resumed.saveCheckpoint(b.string()).empty());
        EXPECT_EQ(readBytes(a), readBytes(b));
    }
    EXPECT_EQ(original.close(), SWMM_OK); EXPECT_EQ(resumed.close(), SWMM_OK);
}

TEST(LivePassiveRecovery, AuthenticatedV2BookAndLiveOwnerRoundTripWithoutNativeMutation) {
    Marcher f(.3, .3); ASSERT_NO_THROW(f.march());
    quality::QualitySolver quality; quality.init(1, 0, 1);
    ASSERT_NO_THROW(f.owner->receive(f.ctx, quality, dt)); f.ctx.elapsed_ms = 1000 * dt;
    const auto source = f.owner->checkpoint(f.ctx, f.state);
    std::ofstream(evidence("authenticated-owner.bin"), std::ios::binary) << source;
    const auto native = f.state.transport.cell_mass;
    Marcher fresh(.3, .3);
    auto candidate = fresh.owner->prepareRestore(f.ctx, f.state, source);
    EXPECT_EQ(candidate->checkpoint(f.ctx, f.state), source);
    EXPECT_EQ(f.state.transport.cell_mass, native);
    EXPECT_EQ(candidate->receipts().receipts(), f.owner->receipts().receipts());
    EXPECT_GT(candidate->receipts().pendingSpill(0), 0);
    EXPECT_EQ(candidate->leg(0).volume_before, f.owner->leg(0).volume_before);
    const auto& actual_spill = f.owner->leg(1);
    EXPECT_NE(actual_spill.mass_native, actual_spill.water_native * actual_spill.sample);
    const auto saved = f.owner->receipts().checkpoint(f.ctx);
    live_recovery_test::Archive envelope(saved);
    std::string magic, payload; std::uint32_t version, endian, sum;
    envelope(magic, version, endian, sum, payload); envelope.finish();
    EXPECT_EQ(version, 2u);
    quality::PassiveStorageExchange standalone; ASSERT_NO_THROW(standalone.restore(f.ctx, saved));
    EXPECT_EQ(standalone.checkpoint(f.ctx), saved);
}

TEST(LivePassiveRecovery, CorruptAuthenticatedProofRefusesAtomicallyWithValidRetry) {
    using namespace live_recovery_test;
    SWMMEngine source; ASSERT_NO_FATAL_FAILURE(startPonded(source, "proof-source"));
    ASSERT_TRUE(source.attachLivePassiveExchange().empty()); ASSERT_TRUE(source.enableCheckpointing().empty());
    double elapsed = 0; ASSERT_EQ(source.step(&elapsed), SWMM_OK) << source.context().error_message;
    const auto good = evidence("proof-good.bin"); ASSERT_TRUE(source.saveCheckpoint(good.string()).empty());
    const auto bytes = readBytes(good); const auto proof = source.livePassiveExchange()->receipts().receipts().at(0).proof;
    using Proof = quality::PassiveStorageExchange::LiveProof;
    const std::vector<std::function<void(Proof&)>> mutations = {
        [](auto& p) { p.si_to_native *= 2; }, [](auto& p) { p.legs.back().sample += 1; },
        [](auto& p) { p.legs.front().volume_before += 1; }, [](auto& p) { p.legs.front().mass_native += 1; },
        [](auto& p) { p.points.front().cell += 1; }, [](auto& p) { p.legs.front().duration_s *= 2; },
        [](auto& p) { p.legs.clear(); }, [](auto& p) { std::reverse(p.legs.begin(), p.legs.end()); }};
    for (std::size_t i = 0; i < mutations.size(); ++i) {
        SCOPED_TRACE(i);
        const auto bad = evidence("proof-corrupt-"+std::to_string(i)+".bin");
        std::ofstream(bad, std::ios::binary) << corruptProof(bytes, proof, mutations[i]);
        SWMMEngine target; ASSERT_NO_FATAL_FAILURE(startPonded(target, "proof-target-"+std::to_string(i)));
        ASSERT_TRUE(target.attachLivePassiveExchange().empty()); ASSERT_TRUE(target.enableCheckpointing().empty());
        const auto before = evidence("proof-before-"+std::to_string(i)+".bin");
        ASSERT_TRUE(target.saveCheckpoint(before.string()).empty()); const auto pristine = readBytes(before);
        CheckpointInspection inspection; inspection.engine_seconds = 123;
        EXPECT_FALSE(target.inspectCheckpoint(bad.string(), inspection).empty()); EXPECT_EQ(inspection.engine_seconds, 123);
        EXPECT_FALSE(target.loadCheckpoint(bad.string()).empty());
        EXPECT_TRUE(target.saveCheckpoint(before.string()).empty()); EXPECT_EQ(readBytes(before), pristine);
        EXPECT_FALSE(target.livePassiveExchange()->failed()); EXPECT_EQ(target.livePassiveExchange()->legCount(), 0u);
        auto error = target.loadCheckpoint(good.string()); ASSERT_TRUE(error.empty()) << error;
        EXPECT_EQ(target.livePassiveExchange()->receipts().receipts(), source.livePassiveExchange()->receipts().receipts());
        EXPECT_EQ(target.close(), SWMM_OK);
    }
    EXPECT_EQ(source.close(), SWMM_OK);
}

TEST(LivePassiveRecovery, ChecksumValidMeshSourcesClosureAndInventoryRefuseBeforeInstallation) {
    using namespace live_recovery_test;
    SWMMEngine source; ASSERT_NO_FATAL_FAILURE(startPonded(source, "mesh-source"));
    ASSERT_TRUE(source.attachLivePassiveExchange().empty()); ASSERT_TRUE(source.enableCheckpointing().empty());
    double elapsed = 0; ASSERT_EQ(source.step(&elapsed), SWMM_OK) << source.context().error_message;
    const auto good = evidence("mesh-good.bin"); ASSERT_TRUE(source.saveCheckpoint(good.string()).empty());
    const auto bytes = readBytes(good);
    const std::vector<std::function<void(std::string&)>> mutations = {
        [](auto& p) { meshVector(p, "cell_mass", [](auto x, auto n) { addFirstDouble(x, n, 1); }); },
        [](auto& p) { meshVector(p, "rainfall", [](auto x, auto n) { addFirstDouble(x, n, .001); }); },
        [](auto& p) { meshVector(p, "coupling_flux", [](auto x, auto n) { addFirstDouble(x, n, .001); }); },
        [](auto& p) { meshVector(p, "rainfall_forced", [](auto x, auto n) { ASSERT_GT(n, 0u); x[0] = 1; }); },
        [](auto& p) { meshVector(p, "head", [](auto x, auto n) { addFirstDouble(x, n, .125); }); },
        [&](auto& p) {
            auto before = source.context().mass_balance_2d, after = before;
            after.coupling_1d_to_2d_in += 1; after.coupling_2d_to_1d_out += 1;
            const auto old = encodeWaterBalance(before), changed = encodeWaterBalance(after);
            const auto where = p.find(old); ASSERT_NE(where, std::string::npos); p.replace(where, old.size(), changed);
        },
        [&](auto& p) {
            for (const auto name : {"volume", "head", "depth"})
                meshVector(p, name, [](auto x, auto n) { addFirstDouble(x, n, .125); });
            auto before = source.context().mass_balance_2d, after = before; after.final_storage += .125;
            const auto old = encodeWaterBalance(before), replacement = encodeWaterBalance(after);
            const auto where = p.find(old); ASSERT_NE(where, std::string::npos); p.replace(where, old.size(), replacement);
        }};
    for (std::size_t i = 0; i < mutations.size(); ++i) {
        SCOPED_TRACE(i);
        const auto bad = evidence("mesh-corrupt-"+std::to_string(i)+".bin");
        std::ofstream(bad, std::ios::binary) << editSurface(bytes, mutations[i]);
        SWMMEngine target; ASSERT_NO_FATAL_FAILURE(startPonded(target, "mesh-target-"+std::to_string(i)));
        ASSERT_TRUE(target.attachLivePassiveExchange().empty()); ASSERT_TRUE(target.enableCheckpointing().empty());
        const auto before = evidence("mesh-before-"+std::to_string(i)+".bin");
        ASSERT_TRUE(target.saveCheckpoint(before.string()).empty()); const auto pristine = readBytes(before);
        CheckpointInspection inspection; inspection.engine_seconds = 456;
        EXPECT_FALSE(target.inspectCheckpoint(bad.string(), inspection).empty()); EXPECT_EQ(inspection.engine_seconds, 456);
        EXPECT_FALSE(target.loadCheckpoint(bad.string()).empty());
        EXPECT_TRUE(target.saveCheckpoint(before.string()).empty()); EXPECT_EQ(readBytes(before), pristine);
        auto error = target.loadCheckpoint(good.string()); ASSERT_TRUE(error.empty()) << error;
        EXPECT_EQ(target.surfaceRouter2D().state().transport.cell_mass, source.surfaceRouter2D().state().transport.cell_mass);
        EXPECT_EQ(target.close(), SWMM_OK);
    }
    EXPECT_EQ(source.close(), SWMM_OK);
}

TEST(LivePassiveRecovery, PendingMeshTimeAndFinalFlushMatchWithoutCheckpointAttachment) {
    SWMMEngine checkpointed, plain;
    ASSERT_NO_FATAL_FAILURE(start(checkpointed, "pending-window-source", 0, 100));
    ASSERT_NO_FATAL_FAILURE(start(plain, "pending-window-plain", 0, 100));
    ASSERT_TRUE(checkpointed.attachLivePassiveExchange().empty()); ASSERT_TRUE(plain.attachLivePassiveExchange().empty());
    ASSERT_TRUE(checkpointed.enableCheckpointing().empty());
    double elapsed = 0;
    ASSERT_EQ(checkpointed.step(&elapsed), SWMM_OK); ASSERT_EQ(plain.step(&elapsed), SWMM_OK);
    EXPECT_EQ(checkpointed.context().elapsed_ms, plain.context().elapsed_ms);
    EXPECT_EQ(checkpointed.surfaceRouter2D().state().volume, plain.surfaceRouter2D().state().volume);
    EXPECT_EQ(checkpointed.livePassiveExchange()->legCount(), 0u);
    const auto saved = evidence("pending-window.bin"); ASSERT_TRUE(checkpointed.saveCheckpoint(saved.string()).empty());
    SWMMEngine resumed; ASSERT_NO_FATAL_FAILURE(start(resumed, "pending-window-resumed", 0, 100));
    ASSERT_TRUE(resumed.attachLivePassiveExchange().empty()); ASSERT_TRUE(resumed.enableCheckpointing().empty());
    CheckpointInspection inspection; ASSERT_TRUE(resumed.inspectCheckpoint(saved.string(), inspection).empty());
    EXPECT_EQ(resumed.context().elapsed_ms, 0); EXPECT_EQ(inspection.engine_seconds, checkpointed.context().current_time);
    EXPECT_EQ(inspection.completed_source_seconds, 0);
    auto error = resumed.loadCheckpoint(saved.string()); ASSERT_TRUE(error.empty()) << error;
    ASSERT_EQ(resumed.step(&elapsed), SWMM_OK); ASSERT_EQ(checkpointed.step(&elapsed), SWMM_OK); ASSERT_EQ(plain.step(&elapsed), SWMM_OK);
    EXPECT_EQ(resumed.end(), SWMM_OK); EXPECT_EQ(checkpointed.end(), SWMM_OK); EXPECT_EQ(plain.end(), SWMM_OK);
    EXPECT_EQ(resumed.surfaceRouter2D().state().volume, plain.surfaceRouter2D().state().volume);
    EXPECT_EQ(resumed.surfaceRouter2D().state().transport.cell_mass, plain.surfaceRouter2D().state().transport.cell_mass);
    EXPECT_EQ(resumed.context().nodes.conc, plain.context().nodes.conc);
    EXPECT_EQ(checkpointed.surfaceRouter2D().state().volume, plain.surfaceRouter2D().state().volume);
    EXPECT_EQ(checkpointed.close(), SWMM_OK); EXPECT_EQ(resumed.close(), SWMM_OK); EXPECT_EQ(plain.close(), SWMM_OK);
}

TEST(LivePassiveRecovery, CheckpointAttachmentPreservesActualPondedTransferLegsAndConservation) {
    SWMMEngine checkpointed, plain;
    ASSERT_NO_FATAL_FAILURE(startPonded(checkpointed, "paired-leg-source"));
    ASSERT_NO_FATAL_FAILURE(startPonded(plain, "plain-leg-source"));
    ASSERT_TRUE(checkpointed.attachLivePassiveExchange().empty()); ASSERT_TRUE(plain.attachLivePassiveExchange().empty());
    ASSERT_TRUE(checkpointed.enableCheckpointing().empty());
    const auto total = [](const SWMMEngine& e) {
        const auto& c = e.context(); const auto& s = e.surfaceRouter2D().state();
        double mass = c.nodes.volume[0] * c.nodes.conc[0];
        for (double value : s.transport.cell_mass) mass += value * e.surfaceRouter2D().options().flow_2d_to_1d;
        return mass + e.livePassiveExchange()->receipts().pendingDrain(0) - e.livePassiveExchange()->receipts().pendingSpill(0);
    };
    const double initial = total(plain); double elapsed = 0;
    for (int i = 0; i < 2; ++i) {
        ASSERT_EQ(checkpointed.step(&elapsed), SWMM_OK) << checkpointed.context().error_message;
        ASSERT_EQ(plain.step(&elapsed), SWMM_OK) << plain.context().error_message;
        EXPECT_EQ(checkpointed.context().elapsed_ms, plain.context().elapsed_ms);
        EXPECT_EQ(checkpointed.context().nodes.volume, plain.context().nodes.volume);
        EXPECT_EQ(checkpointed.context().nodes.conc, plain.context().nodes.conc);
        EXPECT_EQ(checkpointed.surfaceRouter2D().state().volume, plain.surfaceRouter2D().state().volume);
        EXPECT_EQ(checkpointed.surfaceRouter2D().state().transport.cell_mass, plain.surfaceRouter2D().state().transport.cell_mass);
        ASSERT_EQ(checkpointed.livePassiveExchange()->legCount(), plain.livePassiveExchange()->legCount());
        for (std::size_t k = 0; k < plain.livePassiveExchange()->legCount(); ++k)
            EXPECT_EQ(checkpointed.livePassiveExchange()->leg(k), plain.livePassiveExchange()->leg(k));
        EXPECT_NEAR(total(plain), initial, 1e-12 * initial);
        EXPECT_NEAR(total(checkpointed), initial, 1e-12 * initial);
    }
    std::ofstream out(evidence("authored-ponded-legs.json")); out << std::setprecision(17);
    out << "{\"native_initial_volume_ft3\":" << plain.context().nodes.volume[0]
        << ",\"native_pond_base_ft3\":" << (*plain.surfaceRouter2D().state().node_pond_base)[0]
        << ",\"conserved_initial_amount_native\":" << initial << ",\"final_amount_native\":" << total(plain) << ",\"legs\":[";
    for (std::size_t i = 0; i < plain.livePassiveExchange()->legCount(); ++i) {
        const auto& l = plain.livePassiveExchange()->leg(i); if (i) out << ',';
        out << "{\"point\":" << l.point << ",\"drain\":" << (l.drain ? "true" : "false")
            << ",\"water_si\":" << l.water_si << ",\"mass_native\":" << l.mass_native
            << ",\"actual_sample\":" << l.sample << ",\"applied\":" << (l.applied ? "true" : "false") << '}';
    }
    out << "]}\n";
    EXPECT_EQ(checkpointed.close(), SWMM_OK); EXPECT_EQ(plain.close(), SWMM_OK);
}

TEST(LivePassiveRecovery, NeverUsedClosedNodeInventoryCannotChangeOrCancelAcrossPools) {
    using namespace live_recovery_test;
    const auto startQuiet = [](SWMMEngine& e, const std::string& name, bool two) {
        std::string text = deck(0);
        const auto at = text.find("Dye MG/L 0 0 0 0\n");
        text.replace(at, std::string("Dye MG/L 0 0 0 0\n").size(), "Dye MG/L 0 0 0 0 NO * 0 0 2\n");
        if (two) {
            const auto begin = text.find("[2D_VERTICES]"), end = text.find("[2D_INITIAL_QUALITY]");
            text.replace(begin, end-begin,
                "[2D_VERTICES]\n0 0 1\n2 0 1\n0 1 1\n4 0 1\n6 0 1\n4 1 1\n"
                "[2D_TRIANGLES]\n0 1 2 .03 0\n3 4 5 .03 0\n[2D_VERTEX_NODE_MAP]\n0 Tank .7 .01\n3 Second .7 .01\n");
            text += "[STORAGE]\nSecond 0 1 1 FUNCTIONAL 0 0 100 0 0\n";
        }
        const auto p = evidence(name); std::ofstream(p.string()+".inp") << text;
        ASSERT_EQ(e.open((p.string()+".inp").c_str(), (p.string()+".rpt").c_str(), nullptr), SWMM_OK);
        ASSERT_EQ(e.initialize(), SWMM_OK); ASSERT_EQ(e.start(0), SWMM_OK);
        ASSERT_TRUE(e.attachLivePassiveExchange().empty()); ASSERT_TRUE(e.enableCheckpointing().empty());
    };
    for (const bool two : {false, true}) {
        SCOPED_TRACE(two);
        const auto tag = std::string("closed-node-")+(two ? "two" : "one");
        SWMMEngine source; ASSERT_NO_FATAL_FAILURE(startQuiet(source, tag+"-source", two));
        double elapsed = 0; ASSERT_EQ(source.step(&elapsed), SWMM_OK) << source.context().error_message;
        ASSERT_EQ(source.livePassiveExchange()->legCount(), 0u); ASSERT_EQ(source.context().nodes.conc[0], 2);
        const auto good = evidence(tag+"-good.bin"), bad = evidence(tag+"-bad.bin");
        ASSERT_TRUE(source.saveCheckpoint(good.string()).empty());
        std::ofstream(bad, std::ios::binary) << corruptNodeConcentration(readBytes(good));
        SWMMEngine target; ASSERT_NO_FATAL_FAILURE(startQuiet(target, tag+"-target", two));
        const auto before = evidence(tag+"-pristine.bin"); ASSERT_TRUE(target.saveCheckpoint(before.string()).empty());
        const auto pristine = readBytes(before);
        const auto error = target.loadCheckpoint(bad.string()); EXPECT_FALSE(error.empty());
        EXPECT_NE(error.find("closed live native inventory"), std::string::npos) << error;
        EXPECT_TRUE(target.saveCheckpoint(before.string()).empty()); EXPECT_EQ(readBytes(before), pristine);
        auto loaded = target.loadCheckpoint(good.string()); ASSERT_TRUE(loaded.empty()) << loaded;
        EXPECT_EQ(target.context().nodes.conc, source.context().nodes.conc);
        EXPECT_EQ(target.close(), SWMM_OK); EXPECT_EQ(source.close(), SWMM_OK);
    }
}

TEST(LivePassiveRecovery, RefusedCheckpointAttachmentLeavesFreshOwnersReusable) {
    SWMMEngine e; ASSERT_NO_FATAL_FAILURE(start(e, "paired-attachment-retry", 0));
    ASSERT_TRUE(e.attachLivePassiveExchange().empty());
    const auto water = e.surfaceRouter2D().state().volume;
    const auto mass = e.context().nodes.conc;
    e.surfaceRouter2D().options().wall_drag_enabled = true;
    EXPECT_FALSE(e.enableCheckpointing().empty());
    EXPECT_FALSE(e.livePassiveExchange()->failed()); EXPECT_EQ(e.livePassiveExchange()->legCount(), 0u);
    EXPECT_EQ(e.surfaceRouter2D().state().volume, water); EXPECT_EQ(e.context().nodes.conc, mass);
    e.surfaceRouter2D().options().wall_drag_enabled = false;
    const auto error = e.enableCheckpointing(); ASSERT_TRUE(error.empty()) << error;
    EXPECT_TRUE(e.saveCheckpoint(evidence("paired-attachment-retry.bin").string()).empty());
    double elapsed = 0; EXPECT_EQ(e.step(&elapsed), SWMM_OK) << e.context().error_message;
    EXPECT_EQ(e.close(), SWMM_OK);
}

TEST(LivePassiveRecovery, ResolvedPointPolicyIsPartOfThePairedModelIdentity) {
    SWMMEngine source; ASSERT_NO_FATAL_FAILURE(startPonded(source, "policy-source"));
    ASSERT_TRUE(source.attachLivePassiveExchange().empty()); ASSERT_TRUE(source.enableCheckpointing().empty());
    double elapsed = 0; ASSERT_EQ(source.step(&elapsed), SWMM_OK);
    const auto saved = evidence("policy-source.bin"); ASSERT_TRUE(source.saveCheckpoint(saved.string()).empty());
    SWMMEngine target; ASSERT_NO_FATAL_FAILURE(startPonded(target, "policy-target"));
    auto* points = const_cast<std::vector<CouplingPoint>*>(target.surfaceRouter2D().state().node_coupling);
    ASSERT_NE(points, nullptr); points->at(0).equalize = true;
    ASSERT_TRUE(target.attachLivePassiveExchange().empty()); ASSERT_TRUE(target.enableCheckpointing().empty());
    const auto water = target.surfaceRouter2D().state().volume;
    EXPECT_FALSE(target.loadCheckpoint(saved.string()).empty());
    EXPECT_EQ(target.context().elapsed_ms, 0); EXPECT_EQ(target.livePassiveExchange()->legCount(), 0u);
    EXPECT_EQ(target.surfaceRouter2D().state().volume, water);
    EXPECT_EQ(target.close(), SWMM_OK); EXPECT_EQ(source.close(), SWMM_OK);
}

namespace {
double liveTotal(const SWMMEngine& e) {
    const auto& c = e.context(); const auto& s = e.surfaceRouter2D().state();
    double mass = c.nodes.volume[0] * c.nodes.conc[0];
    for (double value : s.transport.cell_mass)
        mass += value * e.surfaceRouter2D().options().flow_2d_to_1d;
    return mass + e.livePassiveExchange()->receipts().pendingDrain(0) -
                  e.livePassiveExchange()->receipts().pendingSpill(0);
}
void expectSameLiveState(const SWMMEngine& a, const SWMMEngine& b) {
    EXPECT_EQ(a.context().elapsed_ms, b.context().elapsed_ms);
    EXPECT_EQ(a.context().nodes.volume, b.context().nodes.volume);
    EXPECT_EQ(a.context().nodes.depth, b.context().nodes.depth);
    EXPECT_EQ(a.context().nodes.conc, b.context().nodes.conc);
    EXPECT_EQ(a.surfaceRouter2D().state().volume, b.surfaceRouter2D().state().volume);
    EXPECT_EQ(a.surfaceRouter2D().state().transport.cell_mass, b.surfaceRouter2D().state().transport.cell_mass);
    EXPECT_EQ(a.livePassiveExchange()->receipts().receipts(), b.livePassiveExchange()->receipts().receipts());
    ASSERT_EQ(a.livePassiveExchange()->legCount(), b.livePassiveExchange()->legCount());
    for (std::size_t k = 0; k < a.livePassiveExchange()->legCount(); ++k)
        EXPECT_EQ(a.livePassiveExchange()->leg(k), b.livePassiveExchange()->leg(k));
}
// Arithmetic-conditioned witnesses for the bounded exact profile, not a claim
// of general concentration/layout eligibility. Two routing steps form one real
// mesh window; the next native step settles it without firing another window.
void signedPondedRecovery(bool positive) {
    const std::string tag = positive ? "signed-positive" : "signed-negative";
    const std::string node = positive ? "5.3" : "1.5", surface = positive ? "8.6" : ".8";
    const auto startSigned = [&](SWMMEngine& e, const std::string& name, bool checkpointed) {
        ASSERT_NO_FATAL_FAILURE(startPonded(e, name, node, surface, ".0002", ".001953125", true));
        ASSERT_TRUE(e.attachLivePassiveExchange().empty());
        if (checkpointed) ASSERT_TRUE(e.enableCheckpointing().empty());
    };
    SWMMEngine original, plain;
    ASSERT_NO_FATAL_FAILURE(startSigned(original, tag+"-original", true));
    ASSERT_NO_FATAL_FAILURE(startSigned(plain, tag+"-plain", false));
    const double initial_c = original.context().nodes.conc[0];
    const double volume = original.context().nodes.volume[0], depth = original.context().nodes.depth[0];
    ASSERT_GT(volume, (*original.surfaceRouter2D().state().node_pond_base)[0]);
    // Each compact triangle contributes one third to the resolved pond. Its
    // authored height is .02 * (.3048^3 / .02832): storage coefficients use
    // legacy .02832 m3/ft3 whereas mesh coordinates use .3048 m/ft.
    // Initial above-rim stored water and depth must agree with that footprint.
    const auto& c = original.context();
    const double geometric_volume = c.nodes.full_volume[0] +
        (depth - c.nodes.full_depth[0]) * c.nodes.ponded_area[0];
    EXPECT_NEAR(volume, geometric_volume, 2 * std::numeric_limits<double>::epsilon() * volume);
    const double initial = liveTotal(original); double elapsed = 0;
    for (int i = 0; i < 2; ++i) {
        ASSERT_EQ(original.step(&elapsed), SWMM_OK) << original.context().error_message;
        ASSERT_EQ(plain.step(&elapsed), SWMM_OK) << plain.context().error_message;
        ASSERT_NO_FATAL_FAILURE(expectSameLiveState(original, plain));
        EXPECT_EQ(original.context().nodes.conc[0], initial_c);
        EXPECT_NEAR(liveTotal(original), initial, 1e-12 * initial);
    }
    const auto* owner = original.livePassiveExchange();
    ASSERT_EQ(owner->legCount(), 2u); ASSERT_EQ(owner->receipts().receipts().size(), 1u);
    const auto drain = owner->leg(0), spill = owner->leg(1);
    ASSERT_TRUE(drain.drain); ASSERT_FALSE(spill.drain);
    ASSERT_TRUE(drain.applied); ASSERT_TRUE(spill.applied);
    ASSERT_GT(drain.mass_native, 0); ASSERT_GT(spill.mass_native, 0);
    EXPECT_EQ(drain.water_native, spill.water_native); EXPECT_EQ(spill.sample, initial_c);
    const double amount = drain.mass_native - spill.mass_native;
    ASSERT_TRUE(positive ? amount > 0 : amount < 0);
    EXPECT_EQ(owner->receipts().pendingDrain(0), drain.mass_native);
    EXPECT_EQ(owner->receipts().pendingSpill(0), spill.mass_native);
    const auto pending = evidence(tag+"-pending.bin"), roundtrip = evidence(tag+"-roundtrip.bin");
    ASSERT_TRUE(original.saveCheckpoint(pending.string()).empty());
    SWMMEngine resumed; ASSERT_NO_FATAL_FAILURE(startSigned(resumed, tag+"-resumed", true));
    auto error = resumed.loadCheckpoint(pending.string()); ASSERT_TRUE(error.empty()) << error;
    ASSERT_TRUE(resumed.saveCheckpoint(roundtrip.string()).empty());
    EXPECT_EQ(readBytes(pending), readBytes(roundtrip));
    for (auto* e : {&original, &resumed, &plain}) {
        ASSERT_EQ(e->step(&elapsed), SWMM_OK) << e->context().error_message;
        EXPECT_EQ(e->context().nodes.volume[0], volume); EXPECT_EQ(e->context().nodes.depth[0], depth);
        const auto& n = e->context().nodes;
        EXPECT_NEAR(n.volume[0], n.full_volume[0] + (n.depth[0] - n.full_depth[0]) * n.ponded_area[0],
                    2 * std::numeric_limits<double>::epsilon() * volume);
        EXPECT_EQ(e->livePassiveExchange()->legCount(), 2u);
        EXPECT_EQ(e->livePassiveExchange()->receipts().pendingDrain(0), 0);
        EXPECT_EQ(e->livePassiveExchange()->receipts().pendingSpill(0), 0);
        EXPECT_TRUE(positive ? e->context().nodes.conc[0] > initial_c : e->context().nodes.conc[0] < initial_c);
        const long double target = (long double)volume * initial_c + drain.mass_native - spill.mass_native;
        EXPECT_LE(std::abs((long double)e->context().nodes.conc[0] * volume - target),
                  2 * std::numeric_limits<double>::epsilon() * std::abs(target));
        EXPECT_NEAR(liveTotal(*e), initial, 1e-12 * initial);
    }
    ASSERT_NO_FATAL_FAILURE(expectSameLiveState(original, resumed));
    ASSERT_NO_FATAL_FAILURE(expectSameLiveState(original, plain));
    std::ofstream summary(evidence(tag+"-summary.json")); summary << std::setprecision(17);
    summary << "{\"initial_native_volume\":" << volume << ",\"geometric_native_volume\":" << geometric_volume
        << ",\"authored_triangle_area_m2\":" << (.03 * .01999777301694915 / 2)
        << ",\"native_pond_area\":" << c.nodes.ponded_area[0] << ",\"initial_native_concentration\":" << initial_c
        << ",\"settled_native_concentration\":" << c.nodes.conc[0]
        << ",\"drain_water_native\":" << drain.water_native << ",\"spill_water_native\":" << spill.water_native
        << ",\"drain_mass_native\":" << drain.mass_native << ",\"spill_mass_native\":" << spill.mass_native
        << ",\"initial_total_native\":" << initial << ",\"settled_total_native\":" << liveTotal(original) << "}\n";
    const auto settled = evidence(tag+"-settled.bin");
    ASSERT_TRUE(original.saveCheckpoint(settled.string()).empty());
    ASSERT_TRUE(resumed.saveCheckpoint(roundtrip.string()).empty());
    EXPECT_EQ(readBytes(settled), readBytes(roundtrip));
    SWMMEngine retired; ASSERT_NO_FATAL_FAILURE(startSigned(retired, tag+"-retired", true));
    error = retired.loadCheckpoint(settled.string()); ASSERT_TRUE(error.empty()) << error;
    ASSERT_TRUE(retired.saveCheckpoint(roundtrip.string()).empty());
    EXPECT_EQ(readBytes(settled), readBytes(roundtrip));
    // The following real window has an unrepresentable aggregate and remains outside
    // this profile. Preserve the refusal through both pending/settled restart.
    for (auto* e : {&original, &resumed, &plain, &retired}) {
        EXPECT_NE(e->step(&elapsed), SWMM_OK);
        EXPECT_NE(e->context().error_message.find("native aggregate is not exactly representable"), std::string::npos);
        EXPECT_TRUE(e->livePassiveExchange()->failed()); EXPECT_EQ(e->livePassiveExchange()->legCount(), 4u);
        EXPECT_EQ(e->context().nodes.conc, original.context().nodes.conc);
        const auto masses = e->surfaceRouter2D().state().transport.cell_mass;
        EXPECT_NE(e->step(&elapsed), SWMM_OK);
        EXPECT_EQ(e->surfaceRouter2D().state().transport.cell_mass, masses);
        if (e != &plain) EXPECT_FALSE(e->saveCheckpoint(roundtrip.string()).empty());
    }
    ASSERT_NO_FATAL_FAILURE(expectSameLiveState(original, resumed));
    ASSERT_NO_FATAL_FAILURE(expectSameLiveState(original, retired));
    for (auto* e : {&original, &resumed, &plain, &retired}) EXPECT_EQ(e->close(), SWMM_OK);
}
}
TEST(LivePassiveRecovery, AuthoredPositiveSignedDeliverySettlesAndRecovers) { signedPondedRecovery(true); }
TEST(LivePassiveRecovery, AuthoredNegativeSignedDeliverySettlesAndRecovers) { signedPondedRecovery(false); }

TEST(LivePassiveRecovery, SimpleUnequalConcentrationsKeepExactArithmeticRefusalAndAppliedProof) {
    for (const bool positive : {false, true}) {
        const auto tag = positive ? "simple-positive" : "simple-negative";
        SWMMEngine e; ASSERT_NO_FATAL_FAILURE(startPonded(e, tag, positive ? "1" : "2", positive ? "2" : "1"));
        ASSERT_TRUE(e.attachLivePassiveExchange().empty()); ASSERT_TRUE(e.enableCheckpointing().empty());
        const auto concentration = e.context().nodes.conc; double elapsed = 0;
        EXPECT_NE(e.step(&elapsed), SWMM_OK);
        EXPECT_NE(e.context().error_message.find("native aggregate is not exactly representable"), std::string::npos);
        const auto* owner = e.livePassiveExchange(); ASSERT_TRUE(owner->failed());
        ASSERT_EQ(owner->legCount(), 2u); EXPECT_TRUE(owner->leg(0).applied); EXPECT_TRUE(owner->leg(1).applied);
        EXPECT_TRUE(owner->receipts().receipts().empty()); EXPECT_EQ(e.context().nodes.conc, concentration);
        const auto masses = e.surfaceRouter2D().state().transport.cell_mass;
        EXPECT_NE(e.step(&elapsed), SWMM_OK); EXPECT_EQ(e.surfaceRouter2D().state().transport.cell_mass, masses);
        EXPECT_FALSE(e.saveCheckpoint(evidence(std::string(tag)+"-failed.bin").string()).empty());
        EXPECT_EQ(e.close(), SWMM_OK);
    }
}
TEST(LivePassiveRecovery, InexactSpillReservationIsDistinguishedFromInsufficientInventory) {
    SWMMEngine e; ASSERT_NO_FATAL_FAILURE(startPonded(e, "inexact-spill", "5.3", "8.6", ".0002"));
    ASSERT_TRUE(e.attachLivePassiveExchange().empty()); double elapsed = 0;
    ASSERT_EQ(e.step(&elapsed), SWMM_OK); ASSERT_EQ(e.step(&elapsed), SWMM_OK);
    EXPECT_NE(e.step(&elapsed), SWMM_OK);
    EXPECT_EQ(e.context().error_message, "Live passive exchange spill reservation is not exactly representable.");
    const auto* owner = e.livePassiveExchange(); ASSERT_TRUE(owner->failed()); ASSERT_EQ(owner->legCount(), 5u);
    const auto& attempted = owner->attempted(); EXPECT_FALSE(attempted.drain); EXPECT_FALSE(attempted.applied);
    EXPECT_LT(owner->receipts().pendingSpill(0) + attempted.mass_native,
              e.context().nodes.volume[0] * e.context().nodes.conc[0]);
    EXPECT_EQ(e.surfaceRouter2D().state().transport.cell_mass[attempted.cell], attempted.before);
    EXPECT_TRUE(owner->leg(4).applied); EXPECT_EQ(e.close(), SWMM_OK);
}

namespace {
void roundoffNativeRecovery(bool positive) {
    using Arithmetic = quality::PassiveStorageArithmetic;
    const std::string tag = positive ? "roundoff-live-positive" : "roundoff-live-negative";
    const auto startR = [&](SWMMEngine& e, const std::string& name) {
        ASSERT_NO_FATAL_FAILURE(startPonded(e, name, positive ? "1" : "2", positive ? "2" : "1",
                                          ".0002", ".001953125", true));
        ASSERT_TRUE(e.attachLivePassiveExchange(Arithmetic::RETAIN_ROUNDOFF).empty());
        ASSERT_TRUE(e.enableCheckpointing().empty());
    };
    SWMMEngine source; ASSERT_NO_FATAL_FAILURE(startR(source, tag+"-source"));
    const double initial = liveTotal(source), initial_c = source.context().nodes.conc[0];
    std::ofstream audit(evidence(tag+".txt")); audit << std::hexfloat;
    audit << "initial " << source.context().nodes.volume[0]*source.context().nodes.conc[0] << '\n';
    double elapsed = 0;
    const auto step = [&](SWMMEngine& e, bool record) {
        const auto& book = e.livePassiveExchange()->receipts();
        const double before = e.context().nodes.volume[0]*e.context().nodes.conc[0];
        const double drain = book.pendingDrain(0), spill = book.pendingSpill(0);
        ASSERT_EQ(e.context().coupling_delivery_remaining, 0);
        ASSERT_EQ(e.step(&elapsed), SWMM_OK) << e.context().error_message;
        const auto& n = e.context().nodes;
        EXPECT_NEAR(n.volume[0], n.full_volume[0]+(n.depth[0]-n.full_depth[0])*n.ponded_area[0],
                    2*std::numeric_limits<double>::epsilon()*n.volume[0]);
        EXPECT_NEAR(liveTotal(e), initial, 1e-12*initial);
        if (record) {
            audit << before << ' ' << drain << ' ' << spill << ' ' << n.volume[0]*n.conc[0];
            for (double part : book.storedRoundoff(0).parts()) audit << ' ' << part;
            audit << '\n';
        }
    };
    ASSERT_NO_FATAL_FAILURE(step(source, true));
    ASSERT_NO_FATAL_FAILURE(step(source, true));
    ASSERT_EQ(source.livePassiveExchange()->legCount(), 2u);
    const auto pending = evidence(tag+"-pending.bin"), settled = evidence(tag+"-settled.bin"), copy = evidence(tag+"-copy.bin");
    ASSERT_TRUE(source.saveCheckpoint(pending.string()).empty());
    SWMMEngine pending_resume; ASSERT_NO_FATAL_FAILURE(startR(pending_resume, tag+"-pending-resume"));
    auto error = pending_resume.loadCheckpoint(pending.string()); ASSERT_TRUE(error.empty()) << error;
    ASSERT_TRUE(pending_resume.saveCheckpoint(copy.string()).empty()); EXPECT_EQ(readBytes(pending), readBytes(copy));
    ASSERT_NO_FATAL_FAILURE(step(source, true));
    ASSERT_NO_FATAL_FAILURE(step(pending_resume, false));
    ASSERT_NO_FATAL_FAILURE(expectSameLiveState(source, pending_resume));
    ASSERT_FALSE(source.livePassiveExchange()->receipts().storedRoundoff(0).zero());
    EXPECT_TRUE(positive ? source.context().nodes.conc[0] > initial_c : source.context().nodes.conc[0] < initial_c);
    ASSERT_TRUE(source.saveCheckpoint(settled.string()).empty());
    SWMMEngine retired; ASSERT_NO_FATAL_FAILURE(startR(retired, tag+"-retired-resume"));
    error = retired.loadCheckpoint(settled.string()); ASSERT_TRUE(error.empty()) << error;
    ASSERT_TRUE(retired.saveCheckpoint(copy.string()).empty()); EXPECT_EQ(readBytes(settled), readBytes(copy));
    EXPECT_EQ(retired.livePassiveExchange()->receipts().storedRoundoff(0), source.livePassiveExchange()->receipts().storedRoundoff(0));
    // A checksum-valid change to the retained amount must not install any state.
    using namespace live_recovery_test;
    const auto bad = editSurface(readBytes(settled), [](std::string& surface) {
        editBlob(surface, "OpenSWMM live passive owner", [](std::string& owner) {
            editOwner(owner, [](std::string& live) {
                editBlob(live, "OpenSWMM passive storage receipt owner", [](std::string& book) {
                    editOwner(book, [](std::string& payload) {
                        double part; std::memcpy(&part, payload.data()+payload.size()-sizeof(part), sizeof(part));
                        part *= 2; std::memcpy(payload.data()+payload.size()-sizeof(part), &part, sizeof(part));
                    });
                });
            });
        });
    });
    const auto corrupt = evidence(tag+"-corrupt.bin"); std::ofstream(corrupt, std::ios::binary) << bad;
    SWMMEngine target; ASSERT_NO_FATAL_FAILURE(startR(target, tag+"-corrupt-target"));
    ASSERT_TRUE(target.saveCheckpoint(copy.string()).empty()); const auto pristine = readBytes(copy);
    CheckpointInspection inspection; EXPECT_FALSE(target.inspectCheckpoint(corrupt.string(), inspection).empty());
    EXPECT_FALSE(target.loadCheckpoint(corrupt.string()).empty());
    ASSERT_TRUE(target.saveCheckpoint(copy.string()).empty()); EXPECT_EQ(readBytes(copy), pristine);
    error = target.loadCheckpoint(settled.string()); ASSERT_TRUE(error.empty()) << error;
    // The next real window and its receiving step must also work after restart.
    for (int i = 0; i < 2; ++i) {
        ASSERT_NO_FATAL_FAILURE(step(source, true));
        for (auto* e : {&pending_resume, &retired, &target}) {
            ASSERT_NO_FATAL_FAILURE(step(*e, false));
            ASSERT_NO_FATAL_FAILURE(expectSameLiveState(source, *e));
            EXPECT_EQ(e->livePassiveExchange()->receipts().storedRoundoff(0), source.livePassiveExchange()->receipts().storedRoundoff(0));
        }
    }
    EXPECT_EQ(source.livePassiveExchange()->legCount(), 4u);
    ASSERT_TRUE(source.saveCheckpoint(settled.string()).empty()); ASSERT_TRUE(retired.saveCheckpoint(copy.string()).empty());
    EXPECT_EQ(readBytes(settled), readBytes(copy));
    for (auto* e : {&source, &pending_resume, &retired, &target}) EXPECT_EQ(e->close(), SWMM_OK);
}
}
TEST(LivePassiveRoundoff, OrdinaryPositiveNativeWindowsSettleAndRecoverRetainedAccounting) { roundoffNativeRecovery(true); }
TEST(LivePassiveRoundoff, OrdinaryNegativeNativeWindowsSettleAndRecoverRetainedAccounting) { roundoffNativeRecovery(false); }
TEST(LivePassiveRoundoff, InitialEmptyBookStillRefusesDifferentAttachedArithmeticPolicy) {
    SWMMEngine strict, retained;
    ASSERT_NO_FATAL_FAILURE(startPonded(strict, "roundoff-initial-strict"));
    ASSERT_NO_FATAL_FAILURE(startPonded(retained, "roundoff-initial-retained"));
    ASSERT_TRUE(strict.attachLivePassiveExchange().empty());
    ASSERT_TRUE(retained.attachLivePassiveExchange(quality::PassiveStorageArithmetic::RETAIN_ROUNDOFF).empty());
    ASSERT_TRUE(strict.enableCheckpointing().empty()); ASSERT_TRUE(retained.enableCheckpointing().empty());
    const auto a=evidence("roundoff-initial-strict.bin"), b=evidence("roundoff-initial-retained.bin"), copy=evidence("roundoff-initial-copy.bin");
    ASSERT_TRUE(strict.saveCheckpoint(a.string()).empty()); ASSERT_TRUE(retained.saveCheckpoint(b.string()).empty());
    const auto before_strict=readBytes(a), before_retained=readBytes(b);
    EXPECT_FALSE(strict.loadCheckpoint(b.string()).empty()); EXPECT_FALSE(retained.loadCheckpoint(a.string()).empty());
    ASSERT_TRUE(strict.saveCheckpoint(copy.string()).empty()); EXPECT_EQ(readBytes(copy), before_strict);
    ASSERT_TRUE(retained.saveCheckpoint(copy.string()).empty()); EXPECT_EQ(readBytes(copy), before_retained);
    EXPECT_EQ(strict.close(), SWMM_OK); EXPECT_EQ(retained.close(), SWMM_OK);
}

namespace {
void startAggregatePonded(SWMMEngine& e,const std::string& name,bool positive,
        quality::PassiveStorageArithmetic policy=quality::PassiveStorageArithmetic::RETAIN_AGGREGATES, bool repeated=false, bool long_history=false, bool preflight=false, std::size_t capacity=4096) {
    const auto p=evidence(name);std::ostringstream text;text << std::setprecision(17);
    text << "[OPTIONS]\nFLOW_UNITS CMS\nFLOW_ROUTING DYNWAVE\nSTART_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
        "END_DATE 01/01/2026\nEND_TIME 00:00:01\nROUTING_STEP 0.0009765625\nVARIABLE_STEP 0\nREPORT_STEP 00:00:01\nTHREADS 1\nALLOW_PONDING YES\n"
        "[POLLUTANTS]\nDye MG/L 0 0 0 0 NO * 0 0 " << (repeated?(positive?.3:.6):(positive?1:2)) <<
        "\n[STORAGE]\nTank -10 1 10 FUNCTIONAL 0 0 " << (repeated?2*.02832/ft3:.0006) << ' ' << (repeated?2*.02832/ft3:.0006) << " 0\n"
        "[2D_OPTIONS]\nINTEGRATOR EXPLICIT\nBACKEND CPU\nLTS_TIERS 1\nMAX_TIMESTEP " << (repeated && !long_history?".0009765625":".001953125") <<
        "\nCOUPLING_SYNC .001953125\nREPORT_2D NO\n[2D_VERTICES]\n";
    // Dyadic translations preserve congruent cells. Repeated-firing storage
    // uses the native CMS volume conversion (.02832) to match its geometric
    // pond footprint (.3048^3); all values are authored input, never overrides.
    const double y=repeated?1:.019197862096271185,dx=repeated?2:.03125;
    for(int k=0;k<3;++k){const double x=repeated?8*k:double(k)/8;
        text<<x<<" 0 "<<(long_history?-1.5:-1)<<'\n'<<x+dx<<" 0 "<<(long_history?-1.5:-1)<<'\n'<<x<<' '<<y<<' '<<(long_history?-1.5:-1)<<'\n'
            <<x<<' '<<-y<<' '<<(long_history?-1.5:-1)<<'\n'<<x<<' '<<-2*y<<' '<<(long_history?-1.5:-1)<<'\n'<<x+dx<<' '<<-y<<' '<<(long_history?-1.5:-1)<<'\n';
    }
    text<<"[2D_TRIANGLES]\n";
    for(int k=0;k<6;++k)text<<3*k<<' '<<3*k+1<<' '<<3*k+2<<" .03 "<<(long_history?(k%2?1.25:1.75):(k%2?.5:1.5))<<'\n';
    text<<"[2D_VERTEX_NODE_MAP]\n";
    for(int k=0;k<6;++k)text<<3*k<<" Tank .7 .01\n";
    text<<"[2D_INITIAL_QUALITY]\n* Dye "<<(repeated?(positive?.6:.3):(positive?2:1))<<"\n[REPORT]\nINPUT NO\n";
    std::ofstream(p.string()+".inp")<<text.str();
    ASSERT_EQ(e.open((p.string()+".inp").c_str(),(p.string()+".rpt").c_str(),nullptr),SWMM_OK);
    ASSERT_EQ(e.initialize(),SWMM_OK);ASSERT_EQ(e.start(0),SWMM_OK);
    ASSERT_TRUE(e.attachLivePassiveExchange(policy,capacity,preflight).empty());
    ASSERT_TRUE(e.enableCheckpointing().empty());
}
double aggregatePhysicalTotal(const SWMMEngine& e){
    double total=e.context().nodes.volume[0]*e.context().nodes.conc[0];
    for(double m:e.surfaceRouter2D().state().transport.cell_mass)total+=m*e.surfaceRouter2D().options().flow_2d_to_1d;
    for(const auto& receipt:e.livePassiveExchange()->receipts().receipts())
        for(const auto& r:receipt.remaining){
            const auto drain=r.materialAmount(true),spill=r.materialAmount(false);
            for(double part:drain.parts())total+=part;
            for(double part:spill.parts())total-=part;
        }
    return total;
}
void aggregateNativeRecovery(bool positive){
    const std::string tag=positive?"aggregate-live-positive":"aggregate-live-negative";
    SWMMEngine source;ASSERT_NO_FATAL_FAILURE(startAggregatePonded(source,tag+"-source",positive));
    const double initial=aggregatePhysicalTotal(source);
    std::ofstream audit(evidence(tag+".txt"));audit<<std::hexfloat<<"initial "
        <<source.context().nodes.volume[0]*source.context().nodes.conc[0]<<'\n';
    double elapsed=0;
    const auto step=[&](SWMMEngine& e,bool record){
        const auto& book=e.livePassiveExchange()->receipts();
        const double before=e.context().nodes.conc[0],volume=e.context().nodes.volume[0];
        std::vector<quality::PassiveStorageExchange::Row> rows;
        for(const auto& receipt:book.receipts())for(const auto& r:receipt.remaining)
            if(r.drain_mass!=0 || r.spill_mass!=0)rows.push_back(r);
        ASSERT_EQ(e.context().coupling_delivery_remaining,0);
        ASSERT_EQ(e.step(&elapsed),SWMM_OK)<<e.context().error_message;
        const auto& n=e.context().nodes;
        EXPECT_NEAR(n.volume[0],n.full_volume[0]+(n.depth[0]-n.full_depth[0])*n.ponded_area[0],
                    2*std::numeric_limits<double>::epsilon()*volume);
        EXPECT_NEAR(aggregatePhysicalTotal(e),initial,1e-12*initial);
        if(record){
            audit<<before<<' '<<volume<<' '<<n.conc[0]<<" |";
            for(const auto& r:rows)audit<<' '<<r.drain_mass<<' '<<r.spill_mass;
            audit<<" |";for(double part:book.storedRoundoff(0).parts())audit<<' '<<part;audit<<'\n';
        }
    };
    ASSERT_NO_FATAL_FAILURE(step(source,true));
    ASSERT_NO_FATAL_FAILURE(step(source,true));
    const auto* owner=source.livePassiveExchange();ASSERT_EQ(owner->legCount(),6u);
    ASSERT_EQ(owner->receipts().receipts().size(),1u);
    ASSERT_GT(owner->receipts().pendingDrainAmount(0).parts().size(),1u);
    ASSERT_GT(owner->receipts().pendingSpillAmount(0).parts().size(),1u);
    EXPECT_THROW(owner->receipts().pendingDrain(0),std::invalid_argument);
    EXPECT_THROW(owner->receipts().pendingSpill(0),std::invalid_argument);
    const auto pending=evidence(tag+"-pending.bin"),settled=evidence(tag+"-settled.bin"),copy=evidence(tag+"-copy.bin");
    ASSERT_TRUE(source.saveCheckpoint(pending.string()).empty());
    SWMMEngine resumed;ASSERT_NO_FATAL_FAILURE(startAggregatePonded(resumed,tag+"-resumed",positive));
    auto error=resumed.loadCheckpoint(pending.string());ASSERT_TRUE(error.empty())<<error;
    ASSERT_TRUE(resumed.saveCheckpoint(copy.string()).empty());EXPECT_EQ(readBytes(copy),readBytes(pending));
    ASSERT_NO_FATAL_FAILURE(step(source,true));
    ASSERT_NO_FATAL_FAILURE(step(resumed,false));
    ASSERT_NO_FATAL_FAILURE(expectSameLiveState(source,resumed));
    ASSERT_FALSE(owner->receipts().storedRoundoff(0).zero());
    ASSERT_TRUE(source.saveCheckpoint(settled.string()).empty());
    SWMMEngine retired;ASSERT_NO_FATAL_FAILURE(startAggregatePonded(retired,tag+"-retired",positive));
    error=retired.loadCheckpoint(settled.string());ASSERT_TRUE(error.empty())<<error;
    ASSERT_TRUE(retired.saveCheckpoint(copy.string()).empty());EXPECT_EQ(readBytes(copy),readBytes(settled));
    for(int k=0;k<2;++k){
        ASSERT_NO_FATAL_FAILURE(step(source,true));
        for(auto* e:{&resumed,&retired}){
            ASSERT_NO_FATAL_FAILURE(step(*e,false));
            ASSERT_NO_FATAL_FAILURE(expectSameLiveState(source,*e));
            EXPECT_EQ(e->livePassiveExchange()->receipts().storedRoundoff(0),owner->receipts().storedRoundoff(0));
        }
    }
    ASSERT_EQ(owner->legCount(),12u);
    ASSERT_TRUE(source.saveCheckpoint(settled.string()).empty());
    ASSERT_TRUE(retired.saveCheckpoint(copy.string()).empty());EXPECT_EQ(readBytes(copy),readBytes(settled));
    for(auto* e:{&source,&resumed,&retired})EXPECT_EQ(e->close(),SWMM_OK);
}
}
TEST(LivePassiveAggregates, ThreeSharedPairsPositiveExactNodeAggregatesAndRecovery){aggregateNativeRecovery(true);}
TEST(LivePassiveAggregates, ThreeSharedPairsNegativeExactNodeAggregatesAndRecovery){aggregateNativeRecovery(false);}
TEST(LivePassiveAggregates, ExactSharedSpillBudgetRefusesBeforeCreditDespiteCurrentDrain){
    Marcher f(1,1,4096,quality::PassiveStorageArithmetic::RETAIN_AGGREGATES);
    f.owner->reserveInitial(f.ctx,{{99,0,"Dye",MassUnits::MG_PER_L,100,100,100,100}});
    EXPECT_THROW(f.march(),std::exception);ASSERT_TRUE(f.owner->failed());
    ASSERT_EQ(f.owner->legCount(),1u);EXPECT_TRUE(f.owner->leg(0).applied);
    const auto& attempted=f.owner->attempted();EXPECT_FALSE(attempted.drain);EXPECT_FALSE(attempted.applied);
    EXPECT_EQ(f.state.transport.cell_mass[attempted.cell],attempted.before);
    EXPECT_EQ(f.owner->receipts().pendingSpill(0),100);EXPECT_EQ(f.ctx.nodes.conc[0],1);
}
TEST(LivePassiveAggregates, InitialPolicyIsDistinctFromBothEarlierLiveFormats){
    using A=quality::PassiveStorageArithmetic;
    SWMMEngine target;ASSERT_NO_FATAL_FAILURE(startAggregatePonded(target,"aggregate-initial",true));
    const auto pristine=evidence("aggregate-initial.bin"),copy=evidence("aggregate-initial-copy.bin");
    ASSERT_TRUE(target.saveCheckpoint(pristine.string()).empty());const auto before=readBytes(pristine);
    for(const auto policy:{A::EXACT,A::RETAIN_ROUNDOFF}){
        SWMMEngine earlier;const auto tag=std::string("aggregate-policy-")+std::to_string(int(policy));
        ASSERT_NO_FATAL_FAILURE(startAggregatePonded(earlier,tag,true,policy));
        const auto old=evidence(tag+".bin");ASSERT_TRUE(earlier.saveCheckpoint(old.string()).empty());
        const auto old_bytes=readBytes(old);
        EXPECT_FALSE(target.loadCheckpoint(old.string()).empty());
        EXPECT_FALSE(earlier.loadCheckpoint(pristine.string()).empty());
        ASSERT_TRUE(target.saveCheckpoint(copy.string()).empty());EXPECT_EQ(readBytes(copy),before);
        ASSERT_TRUE(earlier.saveCheckpoint(copy.string()).empty());EXPECT_EQ(readBytes(copy),old_bytes);
        EXPECT_EQ(earlier.close(),SWMM_OK);
    }
    EXPECT_EQ(target.close(),SWMM_OK);
}
TEST(LivePassiveAggregates, NonrepresentablePointMaterialTotalStillRefusesWithActualLegsRetained){
    Marcher f(.3,.3,4096,quality::PassiveStorageArithmetic::RETAIN_AGGREGATES);
    f.owner->freeze(f.ctx,2*dt);f.owner->observeDrain(f.ctx);
    f.owner->beginWindow(f.ctx,f.state,0,2000*dt,Phase::ADVANCE);
    f.state.live_passive_exchange=f.owner.get();
    ASSERT_NO_THROW(f.solver.advance(0,2*dt));
    f.state.live_passive_exchange=nullptr;
    ASSERT_EQ(f.owner->legCount(),4u);
    quality::NativeAmountSum water,mass;
    for(std::size_t k=0;k<f.owner->legCount();++k)if(f.owner->leg(k).point==1){
        const auto& l=f.owner->leg(k);EXPECT_TRUE(l.applied);water.add(l.water_native);mass.add(l.mass_native);
    }
    ASSERT_NO_THROW(water.value());EXPECT_THROW(mass.value(),std::invalid_argument);
    EXPECT_THROW(f.owner->seal(f.ctx,f.solver.last_coupling_exchange()),std::invalid_argument);
    EXPECT_TRUE(f.owner->failed());EXPECT_EQ(f.owner->legCount(),4u);
    EXPECT_TRUE(f.owner->receipts().receipts().empty());
}

namespace {
void repeatMarcher(Marcher& f){
    f.owner->freeze(f.ctx,2*dt);
    f.ctx.coupling_delivery_remaining=std::max(0.,f.ctx.coupling_delivery_remaining-2*dt);
    f.owner->observeDrain(f.ctx);
    f.owner->beginWindow(f.ctx,f.state,0,2000*dt,Phase::ADVANCE);
    f.state.live_passive_exchange=f.owner.get();
    ASSERT_NO_THROW(f.solver.advance(0,2*dt));f.state.live_passive_exchange=nullptr;
    ASSERT_NO_THROW(f.owner->seal(f.ctx,f.solver.last_coupling_exchange()));
}
void repeatedPointRecovery(bool positive){
    using A=quality::PassiveStorageArithmetic;
    const std::string tag=positive?"point-live-positive":"point-live-negative";
    SWMMEngine source;
    ASSERT_NO_FATAL_FAILURE(startAggregatePonded(source,tag+"-source",positive,A::RETAIN_POINT_AGGREGATES,true));
    const double initial=aggregatePhysicalTotal(source);
    std::ofstream audit(evidence(tag+".txt"));audit<<std::hexfloat<<"initial "<<source.context().nodes.volume[0]*source.context().nodes.conc[0]<<'\n';
    std::ofstream legs(evidence(tag+"-legs.txt"));legs<<std::hexfloat;
    double elapsed=0;
    auto step=[&](SWMMEngine& e,bool record){
        const auto& book=e.livePassiveExchange()->receipts();
        const double before=e.context().nodes.conc[0],volume=e.context().nodes.volume[0];
        std::vector<quality::PassiveStorageExchange::AppliedLeg> pending;
        for(const auto& receipt:book.receipts()){
            const bool active=std::any_of(receipt.remaining.begin(),receipt.remaining.end(),[](const auto& r){return r.drain_water!=0 || r.spill_water!=0;});
            if(active)pending.insert(pending.end(),receipt.proof.legs.begin(),receipt.proof.legs.end());
        }
        ASSERT_EQ(e.context().coupling_delivery_remaining,0);
        ASSERT_EQ(e.step(&elapsed),SWMM_OK)<<e.context().error_message;
        const auto& n=e.context().nodes;
        EXPECT_NEAR(n.volume[0],n.full_volume[0]+(n.depth[0]-n.full_depth[0])*n.ponded_area[0],2*std::numeric_limits<double>::epsilon()*volume);
        EXPECT_NEAR(aggregatePhysicalTotal(e),initial,1e-12*initial);
        if(record){audit<<before<<' '<<volume<<' '<<e.context().nodes.conc[0]<<" |";
            for(const auto& l:pending)audit<<' '<<(l.drain?l.mass_native:0)<<' '<<(l.drain?0:l.mass_native);
            audit<<" |";for(double part:book.storedRoundoff(0).parts())audit<<' '<<part;audit<<'\n';
        }
    };
    ASSERT_NO_FATAL_FAILURE(step(source,true));
    ASSERT_NO_FATAL_FAILURE(step(source,true));
    const auto* owner=source.livePassiveExchange();ASSERT_EQ(owner->legCount(),12u);
    ASSERT_EQ(owner->receipts().receipts().size(),1u);
    bool multipart=false;
    for(const auto& r:owner->receipts().receipts().front().accepted){
        quality::NativeAmountSum drain,spill;
        int firings=0;
        for(const auto& l:owner->receipts().receipts().front().proof.legs)if(l.point==r.point){
            EXPECT_TRUE(l.applied);++firings;
            if(l.drain)drain.add(l.mass_native);else spill.add(l.mass_native);

        }
        EXPECT_EQ(firings,2);EXPECT_EQ(r.materialAmount(true),drain);EXPECT_EQ(r.materialAmount(false),spill);
        multipart|=drain.parts().size()>1 || spill.parts().size()>1;
        for(bool direction:{true,false}){
            const auto amount=r.materialAmount(direction);if(amount.zero())continue;
            legs<<r.point<<' '<<direction<<" |";
            for(const auto& l:owner->receipts().receipts().front().proof.legs)
                if(l.point==r.point && l.drain==direction)legs<<' '<<l.mass_native;
            legs<<" |";for(double part:amount.parts())legs<<' '<<part;legs<<'\n';
        }
    }
    ASSERT_TRUE(multipart);
    const auto pending=evidence(tag+"-pending.bin"),settled=evidence(tag+"-settled.bin"),copy=evidence(tag+"-copy.bin");
    auto error=source.saveCheckpoint(pending.string());ASSERT_TRUE(error.empty())<<error;
    SWMMEngine resumed;
    ASSERT_NO_FATAL_FAILURE(startAggregatePonded(resumed,tag+"-resumed",positive,A::RETAIN_POINT_AGGREGATES,true));
    error=resumed.loadCheckpoint(pending.string());ASSERT_TRUE(error.empty())<<error;
    ASSERT_TRUE(resumed.saveCheckpoint(copy.string()).empty());EXPECT_EQ(readBytes(copy),readBytes(pending));
    ASSERT_NO_FATAL_FAILURE(step(source,true));
    ASSERT_NO_FATAL_FAILURE(step(resumed,false));
    ASSERT_NO_FATAL_FAILURE(expectSameLiveState(source,resumed));
    error=source.saveCheckpoint(settled.string());ASSERT_TRUE(error.empty())<<error;
    SWMMEngine retired;
    ASSERT_NO_FATAL_FAILURE(startAggregatePonded(retired,tag+"-retired",positive,A::RETAIN_POINT_AGGREGATES,true));
    error=retired.loadCheckpoint(settled.string());ASSERT_TRUE(error.empty())<<error;
    ASSERT_TRUE(retired.saveCheckpoint(copy.string()).empty());EXPECT_EQ(readBytes(copy),readBytes(settled));
    for(auto* e:{&source,&resumed,&retired}){
        const auto before=e->context().nodes.conc;
        EXPECT_NE(e->step(&elapsed),SWMM_OK);
        EXPECT_NE(e->context().error_message.find("aggregate is not representable"),std::string::npos);
        EXPECT_TRUE(e->livePassiveExchange()->failed());
        EXPECT_EQ(e->livePassiveExchange()->legCount(),24u);
        EXPECT_EQ(e->livePassiveExchange()->receipts().receipts().size(),1u);
        EXPECT_EQ(e->context().nodes.conc,before);
        EXPECT_FALSE(e->saveCheckpoint(copy.string()).empty());
    }
    for(auto* e:{&source,&resumed,&retired})EXPECT_EQ(e->close(),SWMM_OK);
}
}
TEST(LivePassivePoints, RepeatedPositivePointFiringsAndNativeRestart){repeatedPointRecovery(true);}
TEST(LivePassivePoints, RepeatedNegativePointFiringsAndNativeRestart){repeatedPointRecovery(false);}
TEST(LivePassivePoints, MultipartPartialDeliveryRefusesBeforeMixAndRetainsActualProof){
    Marcher f(.3,.3,4096,quality::PassiveStorageArithmetic::RETAIN_POINT_AGGREGATES);
    f.ctx.coupling_delivery_remaining=4*dt;
    ASSERT_NO_FATAL_FAILURE(repeatMarcher(f));ASSERT_EQ(f.owner->legCount(),4u);
    quality::QualitySolver solver;solver.init(1,0,1);
    ASSERT_NO_THROW(f.owner->receive(f.ctx,solver,2*dt));f.ctx.elapsed_ms=2000*dt;
    const auto rows=f.owner->receipts().receipts();const auto concentration=f.ctx.nodes.conc;
    ASSERT_EQ(f.ctx.coupling_delivery_remaining,2*dt);
    f.owner->freeze(f.ctx,dt);f.ctx.coupling_delivery_remaining=dt;f.owner->observeDrain(f.ctx);
    EXPECT_THROW(f.owner->receive(f.ctx,solver,dt),std::invalid_argument);
    EXPECT_TRUE(f.owner->failed());EXPECT_EQ(f.owner->receipts().receipts(),rows);
    EXPECT_EQ(f.ctx.nodes.conc,concentration);EXPECT_EQ(f.owner->legCount(),4u);
    for(int k=0;k<4;++k)EXPECT_TRUE(f.owner->leg(k).applied);
}
TEST(LivePassivePoints, InitialPolicyIsDistinctFromAllEarlierLiveFormats){
    using A=quality::PassiveStorageArithmetic;
    SWMMEngine target;
    ASSERT_NO_FATAL_FAILURE(startAggregatePonded(target,"point-initial",true,A::RETAIN_POINT_AGGREGATES,true));
    const auto pristine=evidence("point-initial.bin"),copy=evidence("point-initial-copy.bin");
    ASSERT_TRUE(target.saveCheckpoint(pristine.string()).empty());const auto before=readBytes(pristine);
    for(const auto policy:{A::EXACT,A::RETAIN_ROUNDOFF,A::RETAIN_AGGREGATES}){
        SWMMEngine earlier;const auto tag=std::string("point-policy-")+std::to_string(int(policy));
        ASSERT_NO_FATAL_FAILURE(startAggregatePonded(earlier,tag,true,policy,true));
        const auto old=evidence(tag+".bin");ASSERT_TRUE(earlier.saveCheckpoint(old.string()).empty());const auto oldbytes=readBytes(old);
        EXPECT_FALSE(target.loadCheckpoint(old.string()).empty());EXPECT_FALSE(earlier.loadCheckpoint(pristine.string()).empty());
        ASSERT_TRUE(target.saveCheckpoint(copy.string()).empty());EXPECT_EQ(readBytes(copy),before);
        ASSERT_TRUE(earlier.saveCheckpoint(copy.string()).empty());EXPECT_EQ(readBytes(copy),oldbytes);EXPECT_EQ(earlier.close(),SWMM_OK);
    }
    EXPECT_EQ(target.close(),SWMM_OK);
}

namespace {
std::string encodePointRow(quality::PassiveStorageExchange::Row r){
    live_recovery_test::Archive a;
    a(r.point,r.node,r.pollutant,r.units,r.drain_water,r.spill_water,r.drain_mass,r.spill_mass);
    for(auto* parts:{&r.drain_mass_parts,&r.spill_mass_parts}){
        std::uint64_t n=parts->size();a(n);for(double part:*parts)a(part);
    }
    return a.bytes();
}
void replacePointRows(std::string& payload,const quality::PassiveStorageExchange::Row& original,
                      const quality::PassiveStorageExchange::Row& changed){
    const auto old=encodePointRow(original),replacement=encodePointRow(changed);
    std::size_t offset=0,count=0;
    while((offset=payload.find(old,offset))!=std::string::npos){
        payload.replace(offset,old.size(),replacement);offset+=replacement.size();++count;
    }
    if(count!=2)throw std::runtime_error("Missing accepted/remaining point rows in test archive.");
}
}
TEST(LivePassivePoints, ChecksumValidPointRedistributionAndMalformedComponentsRefuseAtomically){
    using A=quality::PassiveStorageArithmetic;
    SWMMEngine source;
    ASSERT_NO_FATAL_FAILURE(startAggregatePonded(source,"point-forge-source",true,A::RETAIN_POINT_AGGREGATES,true));
    double elapsed=0;for(int k=0;k<2;++k)ASSERT_EQ(source.step(&elapsed),SWMM_OK)<<source.context().error_message;
    const auto& book=source.livePassiveExchange()->receipts();const auto bytes=book.checkpoint(source.context());
    const auto rows=book.receipts().front().accepted;ASSERT_EQ(rows.size(),6u);
    auto first=rows[0],second=rows[2];
    auto a=first.materialAmount(true),b=second.materialAmount(true);
    a.add(std::ldexp(1.,-30));b.add(-std::ldexp(1.,-30));
    first.setMaterialAmount(true,a);second.setMaterialAmount(true,b);
    auto difference=a;difference.add(b);
    for(const auto* r:{&rows[0],&rows[2]}){const auto original=r->materialAmount(true);for(double part:original.parts())difference.add(-part);}
    ASSERT_TRUE(difference.zero());
    for(int mutation=0;mutation<5;++mutation){
        SCOPED_TRACE(mutation);auto bad=bytes;
        live_recovery_test::editOwner(bad,[&](std::string& payload){
            if(mutation==0){replacePointRows(payload,rows[0],first);replacePointRows(payload,rows[2],second);}
            else {
                auto changed=rows[0];
                if(mutation==1){changed.drain_mass=1;changed.drain_mass_parts={std::ldexp(1.,-60),.01};}
                if(mutation==2){changed.drain_mass=0;changed.drain_mass_parts={0,.01};}
                if(mutation==3){changed.drain_mass=0;changed.drain_mass_parts={std::numeric_limits<double>::infinity(),.01};}
                if(mutation==4){changed.drain_mass=0;changed.drain_mass_parts.assign(65,.01);}
                replacePointRows(payload,rows[0],changed);
            }
        });
        std::ofstream(evidence("point-forge-"+std::to_string(mutation)+".bin"),std::ios::binary)<<bad;
        quality::PassiveStorageExchange target(A::RETAIN_POINT_AGGREGATES);
        EXPECT_THROW(target.restore(source.context(),bad),std::exception);EXPECT_TRUE(target.checkpoint(source.context()).empty());
        ASSERT_NO_THROW(target.restore(source.context(),bytes));EXPECT_EQ(target.checkpoint(source.context()),bytes);
    }
    EXPECT_EQ(source.close(),SWMM_OK);
}

namespace {
void longerPointHistory(bool positive){
    using A=quality::PassiveStorageArithmetic;
    const std::string tag=positive?"point-history-positive":"point-history-negative";
    auto startHistory=[&](SWMMEngine& e,const std::string& suffix){
        ASSERT_NO_FATAL_FAILURE(startAggregatePonded(e,tag+suffix,positive,A::RETAIN_POINT_AGGREGATES,true,true));
    };
    SWMMEngine source;ASSERT_NO_FATAL_FAILURE(startHistory(source,"-source"));
    const double initial=aggregatePhysicalTotal(source);
    std::ofstream audit(evidence(tag+".txt"));audit<<std::hexfloat<<"initial "<<source.context().nodes.volume[0]*source.context().nodes.conc[0]<<'\n';
    std::unique_ptr<SWMMEngine> resumed;
    double elapsed=0;const auto saved=evidence(tag+"-five-windows.bin"),copy=evidence(tag+"-copy.bin");
    for(int step=1;step<=19;++step){
        const auto& book=source.livePassiveExchange()->receipts();
        const double before=source.context().nodes.conc[0],volume=source.context().nodes.volume[0];
        std::vector<quality::PassiveStorageExchange::AppliedLeg> pending;
        for(const auto& r:book.receipts())if(std::any_of(r.remaining.begin(),r.remaining.end(),[](const auto& row){return row.drain_water!=0 || row.spill_water!=0;}))
            pending.insert(pending.end(),r.proof.legs.begin(),r.proof.legs.end());
        ASSERT_EQ(source.step(&elapsed),SWMM_OK)<<"native step "<<step<<": "<<source.context().error_message;
        const auto& n=source.context().nodes;
        EXPECT_NEAR(n.volume[0],n.full_volume[0]+(n.depth[0]-n.full_depth[0])*n.ponded_area[0],2*std::numeric_limits<double>::epsilon()*volume);
        EXPECT_NEAR(aggregatePhysicalTotal(source),initial,1e-12*initial);
        audit<<before<<' '<<volume<<' '<<n.conc[0]<<" |";
        for(const auto& l:pending)audit<<' '<<(l.drain?l.mass_native:0)<<' '<<(l.drain?0:l.mass_native);
        audit<<" |";for(double part:book.storedRoundoff(0).parts())audit<<' '<<part;audit<<'\n';
        if(resumed){
            ASSERT_EQ(resumed->step(&elapsed),SWMM_OK)<<resumed->context().error_message;
            ASSERT_NO_FATAL_FAILURE(expectSameLiveState(source,*resumed));
        }
        if(step==11){
            ASSERT_EQ(book.receipts().size(),5u);
            const auto error=source.saveCheckpoint(saved.string());ASSERT_TRUE(error.empty())<<error;
            resumed=std::make_unique<SWMMEngine>();
            ASSERT_NO_FATAL_FAILURE(startHistory(*resumed,"-resumed"));
            const auto loaded=resumed->loadCheckpoint(saved.string());ASSERT_TRUE(loaded.empty())<<loaded;
            ASSERT_TRUE(resumed->saveCheckpoint(copy.string()).empty());EXPECT_EQ(readBytes(copy),readBytes(saved));
        }
    }
    const auto& book=source.livePassiveExchange()->receipts();ASSERT_EQ(book.receipts().size(),9u);
    ASSERT_EQ(source.livePassiveExchange()->legCount(),54u);
    EXPECT_EQ(book.pendingDrain(0),0);EXPECT_EQ(book.pendingSpill(0),0);
    std::ofstream points(evidence(tag+"-legs.txt"));points<<std::hexfloat;
    for(const auto& receipt:book.receipts())for(const auto& r:receipt.accepted){
        for(bool direction:{true,false}){
            const auto amount=r.materialAmount(direction);if(amount.zero())continue;
            points<<receipt.id<<' '<<r.point<<' '<<direction<<" |";
            for(const auto& l:receipt.proof.legs)if(l.point==r.point && l.drain==direction)points<<' '<<l.mass_native;
            points<<" |";for(double part:amount.parts())points<<' '<<part;points<<'\n';
        }
    }
    const auto bounded=evidence(tag+"-nine-windows.bin");
    const auto checkpoint=source.saveCheckpoint(bounded.string());ASSERT_TRUE(checkpoint.empty())<<checkpoint;
    const auto receipts=book.receipts();const auto mass=source.context().nodes.conc;
    const auto surface=source.surfaceRouter2D().state().transport.cell_mass;
    const auto refused=source.step(&elapsed);
    EXPECT_NE(refused,SWMM_OK);EXPECT_NE(source.context().error_message.find("work exceeds"),std::string::npos)<<source.context().error_message;
    const auto& retained=source.livePassiveExchange()->receipts().receipts();
    ASSERT_EQ(retained.size(),10u);
    for(std::size_t i=0;i<receipts.size();++i)EXPECT_EQ(retained[i],receipts[i]);
    EXPECT_EQ(source.context().nodes.conc,mass);
    EXPECT_NE(source.surfaceRouter2D().state().transport.cell_mass,surface);
    EXPECT_EQ(source.livePassiveExchange()->legCount(),60u);
    for(std::size_t i=54;i<60;++i)EXPECT_TRUE(source.livePassiveExchange()->leg(i).applied);
    EXPECT_FALSE(source.saveCheckpoint(copy.string()).empty());
    EXPECT_TRUE(source.livePassiveExchange()->failed());
    const auto failed_rows=source.livePassiveExchange()->receipts().receipts();
    const auto failed_surface=source.surfaceRouter2D().state().transport.cell_mass;
    EXPECT_NE(source.step(&elapsed),SWMM_OK);
    EXPECT_EQ(source.livePassiveExchange()->receipts().receipts(),failed_rows);
    EXPECT_EQ(source.surfaceRouter2D().state().transport.cell_mass,failed_surface);
    std::ofstream(evidence(tag+"-summary.json"))<<"{\"steps\":19,\"windows\":9,\"accepted_rows\":54,\"applied_legs\":54,\"restart_windows\":5,\"restart_step\":11,\"over_budget_step_retains_actual_applied_journal\":true}\n";
    EXPECT_EQ(source.close(),SWMM_OK);EXPECT_EQ(resumed->close(),SWMM_OK);
}
}
TEST(LivePassivePoints, LongerPositiveNativeHistoryRecoversAndRespectsReplayWorkCap){longerPointHistory(true);}
TEST(LivePassivePoints, LongerNegativeNativeHistoryRecoversAndRespectsReplayWorkCap){longerPointHistory(false);}

TEST(LivePassivePoints, ExactGrossFundingStillRefusesBeforeActualSpillCredit){
    Marcher f(1,1,4096,quality::PassiveStorageArithmetic::RETAIN_POINT_AGGREGATES);
    f.owner->reserveInitial(f.ctx,{{99,0,"Dye",MassUnits::MG_PER_L,100,100,100,100}});
    EXPECT_THROW(f.march(),std::exception);ASSERT_EQ(f.owner->legCount(),1u);
    EXPECT_TRUE(f.owner->leg(0).applied);const auto& attempted=f.owner->attempted();
    EXPECT_FALSE(attempted.drain);EXPECT_FALSE(attempted.applied);
    EXPECT_EQ(f.state.transport.cell_mass[attempted.cell],attempted.before);
    EXPECT_EQ(f.owner->receipts().pendingSpill(0),100);EXPECT_EQ(f.ctx.nodes.conc[0],1);
}

namespace {
void preflightHistoryRecovery(bool positive) {
    using A=quality::PassiveStorageArithmetic;
    const std::string tag=positive?"preflight-positive":"preflight-negative";
    const auto startBounded=[&](SWMMEngine& e,const std::string& name,bool enabled=true,std::size_t capacity=64){
        ASSERT_NO_FATAL_FAILURE(startAggregatePonded(e,tag+name,positive,A::RETAIN_POINT_AGGREGATES,true,true,enabled,capacity));
    };
    SWMMEngine source,control;
    ASSERT_NO_FATAL_FAILURE(startBounded(source,"-source"));
    ASSERT_NO_FATAL_FAILURE(startBounded(control,"-control",false));
    const auto before=evidence(tag+"-before.bin"),after=evidence(tag+"-after.bin");
    std::unique_ptr<SWMMEngine> resumed;
    bool refused=false;int accepted=0;
    for(int k=0;k<32;++k){
        ASSERT_TRUE(source.saveCheckpoint(before.string()).empty());const auto bytes=readBytes(before);
        const auto time=source.context().elapsed_ms;double elapsed=12345;
        const auto rc=source.step(&elapsed);
        if(rc!=SWMM_OK){
            refused=true;EXPECT_EQ(elapsed,12345);EXPECT_EQ(source.context().elapsed_ms,time);
            EXPECT_EQ(source.context().state,EngineState::RUNNING);EXPECT_EQ(source.context().error_code,SWMM_OK);
            EXPECT_FALSE(source.livePassiveExchange()->failed());
            EXPECT_NE(source.livePassiveExchange()->historyPreflightError().find("work exceeds"),std::string::npos);
            ASSERT_TRUE(source.saveCheckpoint(after.string()).empty());EXPECT_EQ(readBytes(after),bytes);
            ASSERT_NO_FATAL_FAILURE(expectSameLiveState(source,control));
            if(resumed){ASSERT_NO_FATAL_FAILURE(expectSameLiveState(source,*resumed));}
            SWMMEngine restored;
            ASSERT_NO_FATAL_FAILURE(startBounded(restored,"-refusal-restored"));
            auto error=restored.loadCheckpoint(before.string());ASSERT_TRUE(error.empty())<<error;
            for(auto* e:{&source,&restored})for(int retry=0;retry<2;++retry){
                const auto diagnostic=source.livePassiveExchange()->historyPreflightError();
                elapsed=12345;EXPECT_NE(e->step(&elapsed),SWMM_OK);EXPECT_EQ(elapsed,12345);
                EXPECT_EQ(e->livePassiveExchange()->historyPreflightError(),diagnostic);
                ASSERT_TRUE(e->saveCheckpoint(after.string()).empty());EXPECT_EQ(readBytes(after),bytes);
                EXPECT_FALSE(e->livePassiveExchange()->failed());
            }
            const auto& receipts=source.livePassiveExchange()->receipts().receipts();
            std::size_t rows=0,proof_items=0,parts=0;
            for(const auto& receipt:receipts){
                rows+=receipt.accepted.size();proof_items+=receipt.proof.points.size()+receipt.proof.legs.size();
                for(const auto* column:{&receipt.accepted,&receipt.remaining})for(const auto& r:*column)
                    parts+=r.drain_mass_parts.size()+r.spill_mass_parts.size();
            }
            std::ofstream(evidence(tag+"-summary.json"))<<"{\"accepted_steps\":"<<accepted<<",\"receipts\":"<<receipts.size()
                <<",\"rows\":"<<rows<<",\"proof_items\":"<<proof_items<<",\"material_parts\":"<<parts
                <<",\"applied_legs\":"<<source.livePassiveExchange()->legCount()<<",\"capacity\":64,\"points\":6,\"nodes\":1,\"native_nodes\":1}\n";
            EXPECT_EQ(restored.close(),SWMM_OK);break;
        }
        ++accepted;
        ASSERT_EQ(control.step(&elapsed),SWMM_OK)<<control.context().error_message;
        ASSERT_NO_FATAL_FAILURE(expectSameLiveState(source,control));
        if(resumed){
            ASSERT_EQ(resumed->step(&elapsed),SWMM_OK);
            ASSERT_NO_FATAL_FAILURE(expectSameLiveState(source,*resumed));
        }
        if(accepted==5){
            ASSERT_TRUE(source.saveCheckpoint(after.string()).empty());
            resumed=std::make_unique<SWMMEngine>();
            ASSERT_NO_FATAL_FAILURE(startBounded(*resumed,"-pending-resumed"));
            auto error=resumed->loadCheckpoint(after.string());ASSERT_TRUE(error.empty())<<error;
            ASSERT_TRUE(resumed->saveCheckpoint(before.string()).empty());EXPECT_EQ(readBytes(before),readBytes(after));
        }
    }
    EXPECT_TRUE(refused);EXPECT_GE(accepted,5);ASSERT_NE(resumed,nullptr);
    EXPECT_EQ(source.close(),SWMM_OK);EXPECT_EQ(control.close(),SWMM_OK);EXPECT_EQ(resumed->close(),SWMM_OK);
}
}
TEST(LivePassiveHistoryPreflight, PositiveProjectedWorkRefusalPreservesCheckpointAndRestoredRetry){preflightHistoryRecovery(true);}
TEST(LivePassiveHistoryPreflight, NegativeProjectedWorkRefusalPreservesCheckpointAndRestoredRetry){preflightHistoryRecovery(false);}

TEST(LivePassiveHistoryPreflight, ReservedFinalFlushCompletesAndMatchesRestoredPendingBoundary){
    using A=quality::PassiveStorageArithmetic;
    SWMMEngine source;
    ASSERT_NO_FATAL_FAILURE(startAggregatePonded(source,"preflight-final-source",true,A::RETAIN_POINT_AGGREGATES,true,true,true,64));
    double elapsed=0;bool refused=false;
    for(int k=0;k<32;++k)if(source.step(&elapsed)!=SWMM_OK){refused=true;break;}
    ASSERT_TRUE(refused);ASSERT_FALSE(source.livePassiveExchange()->failed());
    const auto before=evidence("preflight-final-before.bin"),after=evidence("preflight-final-after.bin");
    ASSERT_TRUE(source.saveCheckpoint(before.string()).empty());const auto bytes=readBytes(before);
    const auto time=source.context().elapsed_ms,physical=aggregatePhysicalTotal(source);
    const auto concentration=source.context().nodes.conc;
    const auto receipts=source.livePassiveExchange()->receipts().receipts().size();
    const auto legs=source.livePassiveExchange()->legCount();
    SWMMEngine restored;
    ASSERT_NO_FATAL_FAILURE(startAggregatePonded(restored,"preflight-final-restored",true,A::RETAIN_POINT_AGGREGATES,true,true,true,64));
    ASSERT_TRUE(restored.loadCheckpoint(before.string()).empty());
    ASSERT_TRUE(restored.saveCheckpoint(after.string()).empty());EXPECT_EQ(readBytes(after),bytes);
    for(auto* e:{&source,&restored}){
        ASSERT_EQ(e->end(),SWMM_OK)<<e->context().error_message;
        EXPECT_EQ(e->context().elapsed_ms,time);EXPECT_EQ(e->context().state,EngineState::ENDED);
        EXPECT_EQ(e->context().error_code,SWMM_OK);EXPECT_FALSE(e->livePassiveExchange()->failed());
        EXPECT_TRUE(e->livePassiveExchange()->historyPreflightError().empty());
        ASSERT_EQ(e->livePassiveExchange()->receipts().receipts().size(),receipts+1);
        EXPECT_EQ(e->livePassiveExchange()->legCount(),legs+6);
        EXPECT_EQ(e->livePassiveExchange()->receipts().receipts().back().publication.phase,Phase::FINAL);
        EXPECT_EQ(e->context().nodes.conc,concentration);
        EXPECT_NEAR(aggregatePhysicalTotal(*e),physical,1e-12*physical);
    }
    ASSERT_NO_FATAL_FAILURE(expectSameLiveState(source,restored));
    EXPECT_EQ(source.close(),SWMM_OK);EXPECT_EQ(restored.close(),SWMM_OK);
}

TEST(LivePassiveHistoryPreflight, InitialPolicyIdentityAndConservativeLargeCapacityRefusal){
    using A=quality::PassiveStorageArithmetic;
    SWMMEngine source,earlier;
    ASSERT_NO_FATAL_FAILURE(startAggregatePonded(source,"preflight-initial",true,A::RETAIN_POINT_AGGREGATES,true,true,true,4096));
    ASSERT_NO_FATAL_FAILURE(startAggregatePonded(earlier,"preflight-earlier",true,A::RETAIN_POINT_AGGREGATES,true,true,false,4096));
    const auto a=evidence("preflight-initial.bin"),b=evidence("preflight-earlier.bin"),copy=evidence("preflight-initial-copy.bin");
    ASSERT_TRUE(source.saveCheckpoint(a.string()).empty());ASSERT_TRUE(earlier.saveCheckpoint(b.string()).empty());
    const auto initial=readBytes(a),old=readBytes(b);
    EXPECT_FALSE(source.loadCheckpoint(b.string()).empty());EXPECT_FALSE(earlier.loadCheckpoint(a.string()).empty());
    double elapsed=54321;EXPECT_NE(source.step(&elapsed),SWMM_OK);EXPECT_EQ(elapsed,54321);
    EXPECT_EQ(source.livePassiveExchange()->legCount(),0u);EXPECT_FALSE(source.livePassiveExchange()->failed());
    EXPECT_NE(source.livePassiveExchange()->historyPreflightError().find("work exceeds"),std::string::npos);
    ASSERT_TRUE(source.saveCheckpoint(copy.string()).empty());EXPECT_EQ(readBytes(copy),initial);
    ASSERT_TRUE(earlier.saveCheckpoint(copy.string()).empty());EXPECT_EQ(readBytes(copy),old);
    EXPECT_EQ(source.close(),SWMM_OK);EXPECT_EQ(earlier.close(),SWMM_OK);
}

TEST(LivePassiveHistoryPreflight, OneShotForcingAndBoundaryEditsRefuseBeforePendingFlush){
    using A=quality::PassiveStorageArithmetic;
    SWMMEngine source;
    ASSERT_NO_FATAL_FAILURE(startAggregatePonded(source,"preflight-forcing",true,A::RETAIN_POINT_AGGREGATES,true,true,true,64));
    double elapsed=0;ASSERT_EQ(source.step(&elapsed),SWMM_OK);ASSERT_EQ(source.livePassiveExchange()->legCount(),0u);
    const auto before=evidence("preflight-forcing-before.bin"),after=evidence("preflight-forcing-after.bin");
    ASSERT_TRUE(source.saveCheckpoint(before.string()).empty());const auto bytes=readBytes(before);
    std::vector<std::function<int()>> calls={
        [&]{return swmm_2d_force_rainfall(&source,0,0,SWMM_FORCING_ADD,SWMM_FORCING_RESET);},
        [&]{return swmm_2d_force_rainfall_uniform(&source,0,SWMM_FORCING_ADD,SWMM_FORCING_RESET);},
        [&]{return swmm_2d_force_evap(&source,0,0,SWMM_FORCING_ADD,SWMM_FORCING_RESET);},
        [&]{return swmm_2d_force_evap_uniform(&source,0,SWMM_FORCING_ADD,SWMM_FORCING_RESET);},
        [&]{return swmm_2d_force_coupling_flux(&source,0,0,SWMM_FORCING_ADD,SWMM_FORCING_RESET);},
        [&]{return swmm_2d_force_clear_all(&source);},
        [&]{return swmm_2d_set_edge_bc_type(&source,0,0,SWMM_2D_BC_WALL);}};
    for(const auto& call:calls){
        EXPECT_EQ(call(),SWMM_ERR_LIFECYCLE);EXPECT_EQ(source.livePassiveExchange()->legCount(),0u);
        EXPECT_FALSE(source.livePassiveExchange()->failed());
        ASSERT_TRUE(source.saveCheckpoint(after.string()).empty());EXPECT_EQ(readBytes(after),bytes);
    }
    ASSERT_EQ(source.step(&elapsed),SWMM_OK)<<source.context().error_message;
    EXPECT_EQ(source.livePassiveExchange()->legCount(),6u);EXPECT_EQ(source.close(),SWMM_OK);
}

TEST(LivePassiveHistoryPreflight, CallbacksRefuseAtAttachmentAndBeforeInvocationAfterAttachment){
    using A=quality::PassiveStorageArithmetic;
    SWMMEngine source;ASSERT_NO_FATAL_FAILURE(startPonded(source,"preflight-callback"));
    int calls=0;auto callback=+[](void*,double,double,void* data){++*static_cast<int*>(data);};
    source.set_step_begin_callback(callback,&calls);
    EXPECT_FALSE(source.attachLivePassiveExchange(A::RETAIN_POINT_AGGREGATES,64,true).empty());
    EXPECT_EQ(source.livePassiveExchange(),nullptr);EXPECT_EQ(calls,0);
    source.set_step_begin_callback(nullptr,nullptr);
    EXPECT_FALSE(source.attachLivePassiveExchange(A::EXACT,64,true).empty());EXPECT_EQ(source.livePassiveExchange(),nullptr);
    ASSERT_TRUE(source.attachLivePassiveExchange(A::RETAIN_POINT_AGGREGATES,64,true).empty());
    source.set_step_begin_callback(callback,&calls);double elapsed=12345;
    const auto volume=source.surfaceRouter2D().state().volume;
    EXPECT_NE(source.step(&elapsed),SWMM_OK);EXPECT_EQ(calls,0);EXPECT_EQ(elapsed,12345);
    EXPECT_EQ(source.surfaceRouter2D().state().volume,volume);EXPECT_EQ(source.livePassiveExchange()->legCount(),0u);
    EXPECT_EQ(source.close(),SWMM_OK);
}

TEST(LivePassiveHistoryPreflight, SuccessfulArchiveBoundDoesNotPredictNativeJournalExhaustion){
    Marcher f(1,1,1,quality::PassiveStorageArithmetic::RETAIN_POINT_AGGREGATES,true);
    EXPECT_TRUE(f.owner->preflightHistory(f.ctx,true));
    EXPECT_THROW(f.march(),std::exception);EXPECT_TRUE(f.owner->failed());
    ASSERT_EQ(f.owner->legCount(),1u);EXPECT_TRUE(f.owner->leg(0).applied);
    EXPECT_FALSE(f.owner->attempted().applied);EXPECT_EQ(f.state.transport.cell_mass[1],.5);
}

TEST(LivePassiveHistoryPreflight, EmptyFinalBoundaryRetainsOutstandingNativeReceipt){
    using A=quality::PassiveStorageArithmetic;
    SWMMEngine source;
    ASSERT_NO_FATAL_FAILURE(startAggregatePonded(source,"preflight-empty-final",true,A::RETAIN_POINT_AGGREGATES,true,true,true,66));
    double elapsed=0;bool refused=false;
    for(int k=0;k<32;++k)if(source.step(&elapsed)!=SWMM_OK){refused=true;break;}
    ASSERT_TRUE(refused);EXPECT_FALSE(source.livePassiveExchange()->failed());
    EXPECT_EQ(source.context().elapsed_ms,12000*dt);
    const auto receipts=source.livePassiveExchange()->receipts().receipts();
    ASSERT_EQ(receipts.size(),6u);ASSERT_EQ(source.livePassiveExchange()->legCount(),36u);
    EXPECT_FALSE(source.livePassiveExchange()->receipts().pendingSpillAmount(0).zero());
    const auto surface=source.surfaceRouter2D().state().transport.cell_mass;
    EXPECT_EQ(source.end(),SWMM_OK);
    EXPECT_EQ(source.livePassiveExchange()->receipts().receipts(),receipts);
    EXPECT_EQ(source.livePassiveExchange()->legCount(),36u);
    EXPECT_EQ(source.surfaceRouter2D().state().transport.cell_mass,surface);
    EXPECT_EQ(source.close(),SWMM_OK);
}

namespace {
using Arithmetic=quality::PassiveStorageArithmetic;
double marcherPhysical(const Marcher& f){
    double total=f.ctx.nodes.volume[0]*f.ctx.nodes.conc[0];
    for(double value:f.state.transport.cell_mass)total+=value*f.options.flow_2d_to_1d;
    for(const auto& receipt:f.owner->receipts().receipts())for(const auto& r:receipt.remaining){
        const auto drain=r.materialAmount(true),spill=r.materialAmount(false);
        for(double part:drain.parts())total+=part;
        for(double part:spill.parts())total-=part;
    }
    return total;
}
void copyHalfIncoming(const Marcher& source,Marcher& target){
    // Standalone owner restart requires the caller's restored physical state.
    // No numerical marcher is copied or advanced during restore.
    target.ctx.nodes=source.ctx.nodes;
    target.ctx.elapsed_ms=source.ctx.elapsed_ms;target.ctx.old_elapsed_ms=source.ctx.old_elapsed_ms;
    target.ctx.coupling_delivery_remaining=source.ctx.coupling_delivery_remaining;
    target.state=source.state;target.published=source.published;
    target.state.nodes_1d=&target.ctx.nodes;target.state.node_coupling=&target.points;
    target.state.node_row_conc=&target.published;target.state.live_passive_exchange=nullptr;
}
void receiveHalf(Marcher& f,quality::QualitySolver& solver,double interval){
    const double span=f.ctx.coupling_delivery_remaining;
    if(f.owner->preflightsHistory() && !f.owner->preflightHistory(f.ctx,true))
        throw std::runtime_error(f.owner->historyPreflightError());
    f.ctx.nodes.conc_old=f.ctx.nodes.conc;
    f.owner->freeze(f.ctx,interval);
    f.ctx.coupling_delivery_remaining=std::max(0.,span-interval);f.owner->observeDrain(f.ctx);
    f.owner->receive(f.ctx,solver,interval);
    f.ctx.old_elapsed_ms=f.ctx.elapsed_ms;f.ctx.elapsed_ms+=1000*interval;
    f.published=f.ctx.nodes.conc;
}
void halfNativeRecovery(bool positive,bool preflight){
    const std::string tag=std::string("half-")+(positive?"positive":"negative")+(preflight?"-preflight":"");
    const double node=positive?.3:.6,surface=positive?.6:.3;
    Marcher source(node,surface,64,Arithmetic::RETAIN_EXACT_HALVES,preflight,3,4*dt);
    const double initial=marcherPhysical(source);
    EXPECT_TRUE(source.owner->preflightHistory(source.ctx,true));
    ASSERT_NO_FATAL_FAILURE(repeatMarcher(source));ASSERT_EQ(source.owner->legCount(),12u);
    EXPECT_EQ(source.ctx.coupling_delivery_remaining,2*dt);
    quality::QualitySolver solver;solver.init(1,0,1);
    std::ofstream audit(evidence(tag+".txt"));audit<<std::hexfloat<<"initial "<<source.ctx.nodes.volume[0]*source.ctx.nodes.conc[0]<<'\n';
    std::ofstream points(evidence(tag+"-points.txt"));points<<std::hexfloat;
    std::ofstream budget(evidence(tag+"-budget.txt"));
    std::ofstream spans(evidence(tag+"-spans.txt"));spans<<std::hexfloat<<4*dt<<' '<<2*dt<<' '<<2*dt<<'\n';
    // Original ADVANCE receipt is outside the frozen empty first cohort.
    ASSERT_NO_THROW(source.owner->receive(source.ctx,solver,2*dt));source.ctx.elapsed_ms=2000*dt;
    ASSERT_EQ(source.owner->receipts().receipts().size(),1u);
    const auto original=source.owner->receipts().receipts().front();
    bool multipart=false;for(const auto& r:original.accepted)multipart|=r.materialAmount(true).parts().size()>1 || r.materialAmount(false).parts().size()>1;
    ASSERT_TRUE(multipart);
    const auto logBudget=[&](const Marcher& f,int deliveries){
        const auto& receipt=f.owner->receipts().receipts().front();std::size_t accepted=0,remaining=0;
        for(const auto& r:receipt.accepted)accepted+=r.drain_mass_parts.size()+r.spill_mass_parts.size();
        for(const auto& r:receipt.remaining)remaining+=r.drain_mass_parts.size()+r.spill_mass_parts.size();
        budget<<deliveries<<' '<<accepted<<' '<<remaining<<' '<<int(receipt.half_state)<<' ';
        for(const auto& r:receipt.remaining)budget<<(r.drain_water!=0 || r.spill_water!=0)<<' ';
        budget<<'\n';
    };
    logBudget(source,1);
    const auto pending=source.owner->checkpoint(source.ctx,source.state);
    std::ofstream(evidence(tag+"-pending.bin"),std::ios::binary)<<pending;
    Marcher resumed(node,surface,64,Arithmetic::RETAIN_EXACT_HALVES,preflight,3,4*dt);
    copyHalfIncoming(source,resumed);
    resumed.owner=resumed.owner->prepareRestore(resumed.ctx,resumed.state,pending);
    EXPECT_EQ(resumed.owner->checkpoint(resumed.ctx,resumed.state),pending);
    quality::QualitySolver resumed_solver;resumed_solver.init(1,0,1);
    auto transition=[&](Marcher& f,quality::QualitySolver& quality,bool record){
        const double before=f.ctx.nodes.conc[0],volume=f.ctx.nodes.volume[0],span=f.ctx.coupling_delivery_remaining;
        const bool first=f.owner->receipts().receipts().front().half_state==0;
        const auto surface_mass=f.state.transport.cell_mass,surface_volume=f.state.volume;
        ASSERT_NO_THROW(receiveHalf(f,quality,dt));
        EXPECT_EQ(f.state.transport.cell_mass,surface_mass);EXPECT_EQ(f.state.volume,surface_volume);
        EXPECT_NEAR(marcherPhysical(f),initial,1e-12*initial);
        const auto& receipt=f.owner->receipts().receipts().front();EXPECT_EQ(receipt.half_state,1);
        if(record){
            audit<<before<<' '<<volume<<' '<<f.ctx.nodes.conc[0]<<' '<<span<<' '<<dt<<' '<<f.ctx.coupling_delivery_remaining<<' '<<first<<" |";
            for(const auto& leg:original.proof.legs)audit<<' '<<(leg.drain?leg.mass_native:0)<<' '<<(leg.drain?0:leg.mass_native);
            audit<<" |";for(double part:f.owner->receipts().storedRoundoff(0).parts())audit<<' '<<part;audit<<'\n';
            spans<<span<<' '<<dt<<' '<<f.ctx.coupling_delivery_remaining<<'\n';
            for(const auto& row:receipt.remaining)for(bool direction:{true,false}){
                const double accepted_water=direction?original.accepted[row.point].drain_water:original.accepted[row.point].spill_water;
                if(accepted_water==0)continue;
                points<<(first?1:2)<<' '<<row.point<<' '<<direction<<" |";
                for(const auto& leg:original.proof.legs)if(leg.point==row.point && leg.drain==direction)
                    points<<' '<<leg.water_native<<' '<<leg.mass_native;
                points<<" | "<<(direction?row.drain_water:row.spill_water)<<" |";
                const auto material=row.materialAmount(direction);for(double part:material.parts())points<<' '<<part;points<<'\n';
            }
            logBudget(f,first?2:3);
        }
    };
    ASSERT_NO_FATAL_FAILURE(transition(source,solver,true));
    ASSERT_NO_FATAL_FAILURE(transition(resumed,resumed_solver,false));
    EXPECT_EQ(source.ctx.nodes.conc,resumed.ctx.nodes.conc);
    EXPECT_EQ(source.owner->receipts().receipts(),resumed.owner->receipts().receipts());
    const auto half=source.owner->checkpoint(source.ctx,source.state);
    std::ofstream(evidence(tag+"-first-half.bin"),std::ios::binary)<<half;
    EXPECT_EQ(resumed.owner->checkpoint(resumed.ctx,resumed.state),half);
    Marcher halfway(node,surface,64,Arithmetic::RETAIN_EXACT_HALVES,preflight,3,4*dt);
    copyHalfIncoming(source,halfway);
    halfway.owner=halfway.owner->prepareRestore(halfway.ctx,halfway.state,half);
    EXPECT_EQ(halfway.owner->checkpoint(halfway.ctx,halfway.state),half);
    quality::QualitySolver halfway_solver;halfway_solver.init(1,0,1);
    ASSERT_NO_FATAL_FAILURE(transition(source,solver,true));
    ASSERT_NO_FATAL_FAILURE(transition(resumed,resumed_solver,false));
    ASSERT_NO_FATAL_FAILURE(transition(halfway,halfway_solver,false));
    EXPECT_EQ(source.owner->receipts().pendingDrain(0),0);EXPECT_EQ(source.owner->receipts().pendingSpill(0),0);
    const auto retired=source.owner->checkpoint(source.ctx,source.state);
    std::ofstream(evidence(tag+"-retired.bin"),std::ios::binary)<<retired;
    EXPECT_EQ(resumed.owner->checkpoint(resumed.ctx,resumed.state),retired);
    EXPECT_EQ(halfway.owner->checkpoint(halfway.ctx,halfway.state),retired);
    Marcher final(node,surface,64,Arithmetic::RETAIN_EXACT_HALVES,preflight,3,4*dt);copyHalfIncoming(source,final);
    final.owner=final.owner->prepareRestore(final.ctx,final.state,retired);
    EXPECT_EQ(final.owner->checkpoint(final.ctx,final.state),retired);
    EXPECT_EQ(final.owner->receipts().receipts().front().half_state,1);
}
}
TEST(LivePassiveHalves, PositiveNativeLegHalvesRecoverPendingFirstAndRetiredStates){halfNativeRecovery(true,false);}
TEST(LivePassiveHalves, NegativeNativeLegHalvesRecoverPendingFirstAndRetiredStates){halfNativeRecovery(false,false);}
TEST(LivePassiveHalves, PreflightedPositiveNativeHalvesKeepReplayBudgetAndIdentity){halfNativeRecovery(true,true);}
TEST(LivePassiveHalves, PreflightedNegativeNativeHalvesKeepReplayBudgetAndIdentity){halfNativeRecovery(false,true);}

TEST(LivePassiveHalves, QuarterAndFurtherPartialRequestsRetainOriginalPendingState){
    for(bool second:{false,true}){
        Marcher f(.3,.6,64,Arithmetic::RETAIN_EXACT_HALVES,false,3,second?4*dt:6*dt);
        ASSERT_NO_FATAL_FAILURE(repeatMarcher(f));quality::QualitySolver solver;solver.init(1,0,1);
        ASSERT_NO_THROW(f.owner->receive(f.ctx,solver,2*dt));f.ctx.elapsed_ms=2000*dt;
        if(second)ASSERT_NO_THROW(receiveHalf(f,solver,dt));
        const auto rows=f.owner->receipts().receipts();const auto concentration=f.ctx.nodes.conc;
        const auto roundoff=f.owner->receipts().storedRoundoff(0);
        EXPECT_THROW(receiveHalf(f,solver,second?dt/2:dt),std::invalid_argument);
        EXPECT_TRUE(f.owner->failed());EXPECT_EQ(f.owner->receipts().receipts(),rows);
        EXPECT_EQ(f.ctx.nodes.conc,concentration);EXPECT_EQ(f.owner->receipts().storedRoundoff(0),roundoff);
        EXPECT_EQ(f.owner->legCount(),12u);
    }
}
TEST(LivePassiveHalves, OddSubnormalLegRefusesEvenWhenAggregateHalfIsRepresentable){
    Marcher f(.3,8e-320,64,Arithmetic::RETAIN_EXACT_HALVES,false,1,4*dt);
    ASSERT_NO_FATAL_FAILURE(repeatMarcher(f));quality::QualitySolver solver;solver.init(1,0,1);
    ASSERT_NO_THROW(f.owner->receive(f.ctx,solver,2*dt));f.ctx.elapsed_ms=2000*dt;
    const double tiny=std::numeric_limits<double>::denorm_min();bool odd=false;
    std::ofstream audit(evidence("half-subnormal-legs.txt"));audit<<std::hexfloat;
    for(std::size_t k=0;k<f.owner->legCount();++k){const auto& l=f.owner->leg(k);
        audit<<l.point<<' '<<l.drain<<' '<<l.mass_native<<'\n';
        if(l.drain && l.mass_native>0){const auto units=l.mass_native/tiny;odd|=std::fmod(units,2.)!=0;}
    }
    ASSERT_TRUE(odd);const auto rows=f.owner->receipts().receipts();const auto concentration=f.ctx.nodes.conc;
    EXPECT_THROW(receiveHalf(f,solver,dt),std::invalid_argument);
    EXPECT_TRUE(f.owner->failed());EXPECT_EQ(f.owner->receipts().receipts(),rows);EXPECT_EQ(f.ctx.nodes.conc,concentration);
}

TEST(LivePassiveHalves, ChecksumValidProgressAndCrossPointRemainderForgeryRefuseAtomically){
    Marcher f(.3,.6,64,Arithmetic::RETAIN_EXACT_HALVES,false,3,4*dt);
    ASSERT_NO_FATAL_FAILURE(repeatMarcher(f));quality::QualitySolver solver;solver.init(1,0,1);
    ASSERT_NO_THROW(f.owner->receive(f.ctx,solver,2*dt));f.ctx.elapsed_ms=2000*dt;
    ASSERT_NO_THROW(receiveHalf(f,solver,dt));const auto bytes=f.owner->receipts().checkpoint(f.ctx);
    const auto& receipt=f.owner->receipts().receipts().front();
    for(int mutation=0;mutation<3;++mutation){
        auto bad=bytes;live_recovery_test::editOwner(bad,[&](std::string& payload){
            if(mutation<2){
                const auto proof=live_recovery_test::encodeProof(receipt.proof);const auto where=payload.find(proof);
                if(where==std::string::npos)throw std::runtime_error("Missing half proof.");
                payload.at(where+proof.size())=mutation==0?0:2;
            }else{
                auto first=receipt.remaining[0],other=receipt.remaining[2];
                auto a=first.materialAmount(true),b=other.materialAmount(true);
                a.add(std::ldexp(1.,-40));b.add(-std::ldexp(1.,-40));
                first.setMaterialAmount(true,a);other.setMaterialAmount(true,b);
                for(const auto& pair: {std::make_pair(receipt.remaining[0],first),std::make_pair(receipt.remaining[2],other)}){
                    const auto old=encodePointRow(pair.first),changed=encodePointRow(pair.second);const auto where=payload.find(old);
                    if(where==std::string::npos)throw std::runtime_error("Missing half remaining row.");
                    payload.replace(where,old.size(),changed);
                }
            }
        });
        std::ofstream(evidence("half-forge-"+std::to_string(mutation)+".bin"),std::ios::binary)<<bad;
        quality::PassiveStorageExchange target(Arithmetic::RETAIN_EXACT_HALVES);
        EXPECT_THROW(target.restore(f.ctx,bad),std::exception);EXPECT_TRUE(target.checkpoint(f.ctx).empty());
        ASSERT_NO_THROW(target.restore(f.ctx,bytes));EXPECT_EQ(target.checkpoint(f.ctx),bytes);
    }
}

TEST(LivePassiveHalves, InitialHalfAndPreflightPoliciesAreDistinctAndWholeNativeDeliveryWorks){
    for(bool preflight:{false,true}){
        SWMMEngine source;
        ASSERT_NO_FATAL_FAILURE(startAggregatePonded(source,"half-engine-"+std::to_string(preflight),true,Arithmetic::RETAIN_EXACT_HALVES,true,true,preflight,16));
        const auto path=evidence("half-engine-"+std::to_string(preflight)+".bin");ASSERT_TRUE(source.saveCheckpoint(path.string()).empty());
        const auto initial=readBytes(path);
        for(auto policy:{Arithmetic::RETAIN_POINT_AGGREGATES,Arithmetic::RETAIN_EXACT_HALVES}){
            SWMMEngine other;const bool other_preflight=policy==Arithmetic::RETAIN_EXACT_HALVES?!preflight:preflight;
            ASSERT_NO_FATAL_FAILURE(startAggregatePonded(other,"half-other-"+std::to_string(preflight)+"-"+std::to_string(int(policy)),true,policy,true,true,other_preflight,16));
            const auto copy=evidence("half-engine-copy.bin");ASSERT_TRUE(other.saveCheckpoint(copy.string()).empty());const auto prior=readBytes(copy);
            EXPECT_FALSE(other.loadCheckpoint(path.string()).empty());EXPECT_FALSE(source.loadCheckpoint(copy.string()).empty());
            ASSERT_TRUE(other.saveCheckpoint(copy.string()).empty());EXPECT_EQ(readBytes(copy),prior);
            ASSERT_TRUE(source.saveCheckpoint(path.string()).empty());EXPECT_EQ(readBytes(path),initial);EXPECT_EQ(other.close(),SWMM_OK);
        }
        double elapsed=0;for(int k=0;k<3;++k){ASSERT_EQ(source.context().coupling_delivery_remaining,0);ASSERT_EQ(source.step(&elapsed),SWMM_OK)<<source.livePassiveExchange()->historyPreflightError()<<source.context().error_message;}
        ASSERT_EQ(source.livePassiveExchange()->receipts().receipts().size(),1u);
        EXPECT_EQ(source.livePassiveExchange()->receipts().receipts().front().half_state,0);
        EXPECT_EQ(source.livePassiveExchange()->receipts().pendingSpill(0),0);
        ASSERT_TRUE(source.saveCheckpoint(path.string()).empty());EXPECT_EQ(source.close(),SWMM_OK);
    }
}

TEST(LivePassiveHalves, WaterRefusalRecordsObservedPointVolumesAndWouldBookNodeSum){
    SWMMEngine source;
    ASSERT_NO_FATAL_FAILURE(startAggregatePonded(source,"half-water-diagnostic",true,Arithmetic::RETAIN_POINT_AGGREGATES,true));
    const auto volume=source.context().nodes.volume,depth=source.context().nodes.depth;
    double elapsed=0;for(int k=0;k<3;++k)ASSERT_EQ(source.step(&elapsed),SWMM_OK);
    ASSERT_NE(source.step(&elapsed),SWMM_OK);const auto* owner=source.livePassiveExchange();ASSERT_EQ(owner->legCount(),24u);
    const auto& exchange=source.surfaceRouter2D().couplingExchangeVolumes();ASSERT_EQ(exchange.size(),6u);
    std::ofstream out(evidence("water-booking-review.txt"));out<<std::hexfloat;
    const auto& n=source.context().nodes;const double factor=source.surfaceRouter2D().options().flow_2d_to_1d;
    out<<"pool "<<volume[0]<<' '<<n.volume[0]<<' '<<depth[0]<<' '<<n.depth[0]<<' '<<n.coupling_volume[0]<<' '<<n.coupling_queue[0]<<' '<<factor<<'\n';
    double would_book=0;
    for(std::size_t point=0;point<exchange.size();++point){
        out<<"point "<<point<<' '<<exchange[point]<<'\n';would_book+=exchange[point]*factor;
        for(std::size_t k=12;k<24;++k){const auto& leg=owner->leg(k);if(leg.point==point)
            out<<"leg "<<point<<' '<<leg.drain<<' '<<leg.water_si<<' '<<leg.water_native<<'\n';}
    }
    out<<"would_book "<<would_book<<'\n';
    EXPECT_EQ(n.volume,volume);EXPECT_EQ(n.depth,depth);
    EXPECT_TRUE(owner->failed());EXPECT_EQ(source.close(),SWMM_OK);
}

TEST(LivePassiveHalves, PerLegReplayChargeRefusesBeforeAnyNativeEffects) {
    Marcher earlier(1,1,512,Arithmetic::RETAIN_POINT_AGGREGATES,true,3);
    Marcher bounded(1,1,512,Arithmetic::RETAIN_EXACT_HALVES,true,3);
    EXPECT_TRUE(earlier.owner->preflightHistory(earlier.ctx,true));
    const auto before=bounded.owner->checkpoint(bounded.ctx,bounded.state);
    EXPECT_FALSE(bounded.owner->preflightHistory(bounded.ctx,true));
    EXPECT_NE(bounded.owner->historyPreflightError().find("work exceeds"),std::string::npos);
    EXPECT_EQ(bounded.owner->legCount(),0u);EXPECT_FALSE(bounded.owner->failed());
    EXPECT_EQ(bounded.owner->checkpoint(bounded.ctx,bounded.state),before);
}
