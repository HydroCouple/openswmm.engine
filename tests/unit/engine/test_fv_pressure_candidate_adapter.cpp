// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include <gtest/gtest.h>

#include <limits>
#include <numeric>
#include <stdexcept>

#include "../../verification/fv_implicit_phase0/network_cases.hpp"
#include "hydraulics/fv/CheckedNetworkAdvance.hpp"

namespace {
using namespace fvtest;
using Controls = NonlinearPressureStorage::Controls;

FvOptions options() {
    auto opts = defaultOptions();
    opts.lts = false;
    opts.pressurized_implicit = true;
    return opts;
}

std::vector<double> fixedHeads(const Channel& ch) {
    std::vector<double> fixed(ch.mesh.n_nodes(), std::numeric_limits<double>::quiet_NaN());
    for (int n = 0; n < ch.mesh.n_nodes(); ++n)
        if (ch.mesh.node_kind[n] == kNodeOutfall) fixed[n] = ch.state.node_head[n];
    return fixed;
}

double inventory(const Channel& ch, const ExplicitFvSolver& solver) {
    double volume = totalVolume(ch);
    for (int n = 0; n < ch.mesh.n_nodes(); ++n)
        if (ch.mesh.node_kind[n] != kNodeOutfall)
            volume += ch.state.node_volume[n] + std::min(0.0, solver.node_continuity_carry()[n]);
    return volume;
}

void sameState(const NetworkStateData& a, const NetworkStateData& b) {
    EXPECT_EQ(a.cell_a, b.cell_a); EXPECT_EQ(a.cell_q, b.cell_q); EXPECT_EQ(a.cell_h, b.cell_h);
    EXPECT_EQ(a.node_head, b.node_head); EXPECT_EQ(a.node_volume, b.node_volume);
    EXPECT_EQ(a.node_surf_area, b.node_surf_area); EXPECT_EQ(a.node_overflow, b.node_overflow);
    EXPECT_EQ(a.node_overflow_vol, b.node_overflow_vol);
    EXPECT_EQ(a.cell_phi, b.cell_phi); EXPECT_EQ(a.cell_tpa, b.cell_tpa);
}

struct ViewFixture {
    Channel ch = fvbaseline::makeNetwork("tee_pressure", 4, 100.0);
    FvOptions opts = options();
    std::vector<double> fixed = fixedHeads(ch), eta, velocity, mass, sstar, qstruct, carry, vfull, qlat;
    std::vector<kernels::FaceState> left, right;
    std::vector<kernels::FaceFlux> flux;
    std::vector<int> faces;
    std::vector<char> active;
    std::vector<std::uint8_t> algebraic, diverted;
    FvStepForcing forcing;
    PressurizedView view;
    ViewFixture() {
        ExplicitFvSolver normalize;
        normalize.initialize(ch.mesh, ch.state, opts);
        const int nc = ch.n, nf = ch.mesh.n_faces(), nn = ch.mesh.n_nodes();
        eta.assign(nc, 5.5); velocity.assign(nc, 0.0); mass.assign(nf, 7.0); sstar = mass;
        qstruct.assign(nn, 0); carry = qstruct; vfull = qstruct; qlat.assign(nc, 0);
        left.resize(nf); right.resize(nf); flux.resize(nf); faces.resize(nf);
        std::iota(faces.begin(), faces.end(), 0); active.assign(nc, 1);
        algebraic.assign(nn, 1); diverted.assign(nn, 0);
        for (int f = 0; f < nf; ++f) {
            const int cl = ch.mesh.face_cl[f], cr = ch.mesh.face_cr[f], node = ch.mesh.face_node[f];
            left[f].a = kernels::areaOfDepth(ch.mesh.geom[0], cl < 0 ? ch.state.node_head[node] : 5.5);
            right[f].a = kernels::areaOfDepth(ch.mesh.geom[0], cr < 0 ? ch.state.node_head[node] : 5.5);
            flux[f].mass = mass[f]; flux[f].sstar = sstar[f];
        }
        forcing.node_fixed_head = fixed.data(); forcing.n_nodes = nn;
        view.mesh = &ch.mesh; view.state = &ch.state; view.opts = &opts; view.forcing = &forcing;
        view.cell_eta = eta.data(); view.cell_u = velocity.data();
        view.f_mass = mass.data(); view.f_sstar = sstar.data();
        view.f_state_l = left.data(); view.f_state_r = right.data(); view.f_flux = flux.data();
        view.active_faces = &faces; view.cell_active = &active;
        view.node_qstruct = &qstruct; view.node_carry = &carry; view.node_alg = &algebraic;
        view.node_vfull = &vfull; view.node_lat_div = &diverted; view.cell_qlat = &qlat;
        // The full geometry helper seeds conserved area but not derived depth.
        ch.state.cell_h.assign(nc, 5.5);
    }
};
} // namespace

TEST(FvPressureCandidateAdapter, FailedSolveDoesNotPublishAnyFaceOrNode) {
    ViewFixture f;
    PressurizedHeadSolver solver;
    ASSERT_TRUE(solver.classify(f.view));
    const auto before = f.ch.state;
    const auto mass = f.mass, sstar = f.sstar;
    Controls controls; controls.max_newton = 0;
    const auto result = solver.solveCandidate(f.view, 0.1, controls);
    EXPECT_EQ(result.status, NonlinearPressureStorage::Status::NonlinearIterationLimit);
    sameState(f.ch.state, before);
    EXPECT_EQ(f.mass, mass); EXPECT_EQ(f.sstar, sstar);
    for (std::size_t i = 0; i < f.flux.size(); ++i) EXPECT_EQ(f.flux[i].mass, mass[i]);
}

TEST(FvPressureCandidateAdapter, SharedFluxCopiesAndCommittedEndpointAreVerified) {
    ViewFixture f;
    PressurizedHeadSolver solver;
    ASSERT_TRUE(solver.classify(f.view));
    const double dt = .1;
    const auto result = solver.solveCandidate(f.view, dt, {});
    ASSERT_TRUE(result.converged()) << static_cast<int>(result.status);
    for (std::size_t i = 0; i < f.mass.size(); ++i) {
        EXPECT_EQ(f.mass[i], f.flux[i].mass);
        EXPECT_EQ(f.mass[i], f.flux[i].sstar);
        EXPECT_EQ(f.mass[i], f.sstar[i]);
    }
    for (int c = 0; c < f.ch.n; ++c) {
        double out = 0.0;
        for (int face : {f.ch.mesh.cell_face0[c], f.ch.mesh.cell_face1[c]})
            out += (f.ch.mesh.face_cl[face] == c ? 1.0 : -1.0) * f.mass[face];
        f.ch.state.cell_a[c] -= dt * out / f.ch.mesh.cell_dx[c];
        f.ch.state.cell_h[c] = kernels::depthOfArea(f.ch.mesh.geom[0], f.ch.state.cell_a[c]);
    }
    ASSERT_TRUE(solver.verifyCandidateEndpoint(f.view, dt, {}));
    f.carry[1] = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(solver.verifyCandidateEndpoint(f.view, dt, {}));
    f.carry[1] = 0.0;
    f.mass[0] += 1.0; // simulate a downstream limiter changing the solved flux
    EXPECT_FALSE(solver.verifyCandidateEndpoint(f.view, dt, {}));
}

TEST(FvPressureCandidateAdapter, GraphRestRetainsEveryPhysicalCell) {
    for (const std::string name : {"tee_rest", "loop_rest"}) {
        for (int cells : {4, 8, 16}) {
            for (double celerity : {100.0, 300.0}) {
                SCOPED_TRACE(::testing::Message() << name << cells << celerity);
                auto ch = fvbaseline::makeNetwork(name, cells, celerity);
                const auto mesh_lengths = ch.mesh.cell_dx;
                auto fixed = fixedHeads(ch);
                FvStepForcing forcing; forcing.node_fixed_head = fixed.data();
                ExplicitFvSolver solver; solver.initialize(ch.mesh, ch.state, options());
                solver.enablePressureStorageCandidate();
                const double initial = inventory(ch, solver);
                EXPECT_EQ(solver.advance(0.0, 5.0, forcing), 5.0);
                EXPECT_EQ(ch.mesh.cell_dx, mesh_lengths);
                EXPECT_EQ(ch.state.cell_a.size(), mesh_lengths.size());
                EXPECT_NEAR(inventory(ch, solver), initial, 1.e-10 * initial);
                for (double q : ch.state.cell_q) EXPECT_NEAR(q, 0.0, 1.e-9);
                for (double h : ch.state.cell_h) EXPECT_NEAR(h, 5.5, 1.e-9);
            }
        }
    }
}

TEST(FvPressureCandidateAdapter, ActualRoutingLedgerBalancesGraphsAndLateralSources) {
    for (const std::string name : {"tee_pressure", "loop_pressure", "mixed_reversed"}) {
        auto ch = fvbaseline::makeNetwork(name, 8, 300.0);
        auto fixed = fixedHeads(ch);
        std::vector<double> lateral(ch.mesh.n_nodes(), 0.0); lateral[1] = .1;
        FvStepForcing forcing; forcing.node_fixed_head = fixed.data(); forcing.node_lateral = lateral.data();
        ExplicitFvSolver solver; solver.initialize(ch.mesh, ch.state, options());
        solver.enablePressureStorageCandidate();
        const double initial = inventory(ch, solver);
        double prescribed = 0.0;
        for (int step = 0; step < 6; ++step) {
            EXPECT_EQ(solver.advance(step * .5, (step + 1) * .5, forcing), (step + 1) * .5);
            for (int n = 0; n < ch.mesh.n_nodes(); ++n)
                if (std::isfinite(fixed[n])) prescribed += solver.node_exchange()[n];
            EXPECT_NEAR(inventory(ch, solver) - initial + prescribed - .1 * (step + 1) * .5,
                        0.0, 1.e-9 * initial);
            for (double flood : solver.node_flood_volume()) EXPECT_EQ(flood, 0.0);
        }
        if (name != "mixed_reversed") EXPECT_LE(fvbaseline::symmetryDifference(name, ch), 1.e-8);
        else EXPECT_LT(ch.state.cell_q.back(), 0.0);
        EXPECT_TRUE(solver.pressureCandidateResult().converged());
    }
}

TEST(FvPressureCandidateAdapter, RetryExhaustionRestoresPreviousWindowStateAndReceipts) {
    auto ch = fvbaseline::makeNetwork("tee_pressure", 4, 100.0);
    auto fixed = fixedHeads(ch);
    FvStepForcing forcing; forcing.node_fixed_head = fixed.data();
    ExplicitFvSolver solver; solver.initialize(ch.mesh, ch.state, options());
    solver.enablePressureStorageCandidate();
    ASSERT_EQ(solver.advance(0.0, .5, forcing), .5);
    const auto before = ch.state;
    const auto exchange = solver.node_exchange(), carry = solver.node_continuity_carry();
    const auto qint = solver.cell_flow_integral(), incoming = solver.node_inflow_volume(), outgoing = solver.node_outflow_volume();
    const auto flood = solver.node_flood_volume();
    const auto stats = solver.run_stats();
    Controls controls; controls.max_newton = 0;
    solver.enablePressureStorageCandidate(controls);
    EXPECT_THROW(solver.advance(.5, 1.0, forcing), std::runtime_error);
    EXPECT_GT(solver.pressureCandidateRejections(), 0);
    sameState(ch.state, before);
    EXPECT_EQ(solver.node_exchange(), exchange); EXPECT_EQ(solver.node_continuity_carry(), carry);
    EXPECT_EQ(solver.cell_flow_integral(), qint); EXPECT_EQ(solver.node_inflow_volume(), incoming);
    EXPECT_EQ(solver.node_outflow_volume(), outgoing); EXPECT_EQ(solver.node_flood_volume(), flood);
    EXPECT_EQ(solver.run_stats().nsteps, stats.nsteps);
    solver.enablePressureStorageCandidate();
    EXPECT_EQ(solver.advance(.5, 1.0, forcing), 1.0); // restored state remains usable
}

TEST(FvPressureCandidateAdapter, FailureAfterExplicitPrefixRollsBackEntireWindow) {
    auto ch = makeNodedChannel(circular(3.0), 8, 50.0, [](double) { return 0.0; }, .013, 100.0);
    seedLevel(ch, 2.9); ch.state.node_head = {3.5, 2.9};
    const double fixed[] = {3.5, 2.9};
    FvStepForcing forcing; forcing.node_fixed_head = fixed;
    ExplicitFvSolver solver; solver.initialize(ch.mesh, ch.state, options());
    Controls controls; controls.max_newton = 0;
    solver.enablePressureStorageCandidate(controls);
    const auto before = ch.state;
    try {
        solver.advance(0.0, 30.0, forcing);
        FAIL() << "expected a nontrivial pressure solve after the explicit filling prefix";
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        ASSERT_NE(message.find("reached="), std::string::npos) << message;
        EXPECT_EQ(message.find("reached=0.000000"), std::string::npos) << message;
    }
    sameState(ch.state, before);
    EXPECT_EQ(solver.run_stats().nsteps, 0);
    for (double v : solver.node_exchange()) EXPECT_EQ(v, 0.0);
}

TEST(FvPressureCandidateAdapter, UnsupportedModesAreRejectedBeforeStateMutation) {
    for (int feature = 0; feature < 5; ++feature) {
        auto ch = fvbaseline::makeNetwork("tee_rest", 4, 100.0);
        auto opts = options();
        if (feature == 0) opts.time_integration = TimeIntegration::RK2;
        if (feature == 1) opts.lts = true;
        if (feature == 2) opts.unsteady_friction = 1;
        if (feature == 3) opts.compaction = true;
        if (feature == 4) ch.mesh.node_can_pond[1] = 1;
        ExplicitFvSolver solver; solver.initialize(ch.mesh, ch.state, opts);
        solver.enablePressureStorageCandidate();
        const auto before = ch.state;
        EXPECT_THROW(solver.advance(0.0, 1.0, {}), std::invalid_argument);
        sameState(ch.state, before);
    }
}

TEST(FvPressureCandidateAdapter, RouterGuardRejectsShortNonfiniteAndOverrunTimes) {
    struct Backend : ExplicitFvSolver {
        double reached = 0.0;
        double advance(double, double, const FvStepForcing&) override { return reached; }
    } backend;
    for (double reached : {0.0, .5, 1.1, std::numeric_limits<double>::quiet_NaN()}) {
        backend.reached = reached;
        EXPECT_THROW(advanceNetworkChecked(backend, 0.0, 1.0, {}), std::runtime_error);
    }
    backend.reached = 1.0;
    EXPECT_EQ(advanceNetworkChecked(backend, 0.0, 1.0, {}), 1.0);
}
