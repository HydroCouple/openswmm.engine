// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin

/**
 * @file      test_2d_cpu_correctness.cpp
 * @brief     Tests for 2d cpu correctness.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ostream>

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include "2d/mesh/MeshBuilder.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include "2d/solver/ExplicitInertialSolver.hpp"
#include "2d/solver/InertialKernels.hpp"
#include "2d/solver/SweKernels.hpp"
#include "2d/subsurface/SubsurfaceSolver.hpp"
#include "core/CompletedSourceArchive.hpp"
#include <limits>
using namespace openswmm::twoD;
namespace openswmm::twoD {
// Inspect the scheduling invariant without making debug state a public API.
struct ExplicitInertialSolverTestAccess {
    /**
     * @brief Read reconstructed cell speed for the scheduling-invariant assertion.
     *
     * Requires valid attached state/options; a dry cell returns zero without reading its momenta.
     *
     * @param[in] s Live initialized CPU solver with consistent attached mesh/state/options and internal
     *     buffers.
     * @param[in] i Valid zero-based cell index for all accessed solver/state arrays; unchecked here.
     * @return hypot(qx,qy)/depth in m/s for depth above dry_depth; zero otherwise.
     */
    static double speed(const ExplicitInertialSolver& s, int i) {
        const double h = s.state_->depth[i];
        return h > s.opts_->dry_depth ? std::hypot(s.qcx_[i], s.qcy_[i]) / h : 0.0;
    }
    /**
     * @brief Fire all active SWE faces directly for a fixture stage.
     *
     * Calls fireFacesSwe with the true firing flag, modifying discharge and transfer buffers; does not
     * gather cells or schedule a complete advance.
     *
     * @param[in,out] s Initialized FULL_SWE solver with active faces and prepared reconstruction/state
     *     buffers.
     * @param[in] dt Positive face integration interval in seconds.
     */
    static void fireSwe(ExplicitInertialSolver& s, double dt) {
        s.fireFacesSwe(s.active_faces_, dt, true);
    }
    /**
     * @brief Rebuild the CPU scheduler and synchronize state at time zero.
     *
     * Calls syncAndRebuild(0), modifying prepared activity/tier/state storage through production logic.
     *
     * @param[in,out] s Initialized solver whose attached dependencies and pending/prepared buffers are
     *     consistent for the requested operation.
     */
    static void rebuild(ExplicitInertialSolver& s) { s.syncAndRebuild(0); }
    /**
     * @brief Refresh the CPU base timestep from the current solver state.
     *
     * Calls refreshDt0 without rebuilding the tier assignment or advancing time.
     *
     * @param[in,out] s Initialized solver whose attached dependencies and pending/prepared buffers are
     *     consistent for the requested operation.
     */
    static void refresh(ExplicitInertialSolver& s) { s.refreshDt0(); }
    /**
     * @brief Read the scheduler tier of one cell.
     *
     * No bounds checks or state mutation occur.
     *
     * @param[in] s Live initialized CPU solver with consistent attached mesh/state/options and internal
     *     buffers.
     * @param[in] i Valid zero-based cell index for all accessed solver/state arrays; unchecked here.
     * @return Stored integer tier for the indexed cell.
     */
    static int tier(const ExplicitInertialSolver& s, int i) { return s.tier_[i]; }
    /**
     * @brief Compute the scheduled interval of one cell.
     *
     * No bounds checks or state mutation occur.
     *
     * @param[in] s Live initialized CPU solver with consistent attached mesh/state/options and internal
     *     buffers.
     * @param[in] i Valid zero-based cell index for all accessed solver/state arrays; unchecked here.
     * @return dt0*2^tier in seconds; the stored tier must permit a valid nonnegative integer left
     *     shift.
     */
    static double step(const ExplicitInertialSolver& s, int i) {
        return s.dt0_ * (1 << s.tier_[i]);
    }
    /**
     * @brief Read the scheduling characteristic length of one cell.
     *
     * No bounds checks or state mutation occur.
     *
     * @param[in] s Live initialized CPU solver with consistent attached mesh/state/options and internal
     *     buffers.
     * @param[in] i Valid zero-based cell index for all accessed solver/state arrays; unchecked here.
     * @return Stored cell characteristic length in metres.
     */
    static double length(const ExplicitInertialSolver& s, int i) { return s.edges_.cell_lchar[i]; }
    /**
     * @brief Rebuild/synchronize the scheduler at a specified model time.
     *
     * Delegates to syncAndRebuild(t), updating prepared state/tier/activity buffers without advancing a
     * time window.
     *
     * @param[in,out] s Initialized solver with live attached dependencies and consistent state arrays.
     * @param[in] t Synchronization time in model seconds, suitable for the attached forcing/state
     *     lifecycle.
     */
    static void rebuildAt(ExplicitInertialSolver& s, double t) { s.syncAndRebuild(t); }
    /**
     * @brief Borrow the CPU edge topology and metric arrays.
     *
     * @param[in] s Live initialized CPU solver with consistent attached mesh/state/options and internal
     *     buffers.
     * @return Read-only reference owned by s; it must not outlive the solver, and rebuilds/resizes can
     *     invalidate element references.
     */
    static const InertialEdges& edges(const ExplicitInertialSolver& s) { return s.edges_; }
    /**
     * @brief Borrow the current active-face index list.
     *
     * @param[in] s Live initialized CPU solver with consistent attached mesh/state/options and internal
     *     buffers.
     * @return Read-only solver-owned list; valid while the solver lives and until the corresponding
     *     storage/list is rebuilt.
     */
    static const std::vector<int>& faces(const ExplicitInertialSolver& s) {
        return s.active_faces_;
    }
    /**
     * @brief Read the activity flag of one cell.
     *
     * No bounds checks or state mutation occur.
     *
     * @param[in] s Live initialized CPU solver with consistent attached mesh/state/options and internal
     *     buffers.
     * @param[in] i Valid zero-based cell index for all accessed solver/state arrays; unchecked here.
     * @return True if its stored activity byte is nonzero; false otherwise.
     */
    static bool active(const ExplicitInertialSolver& s, int i) { return s.cell_active_[i] != 0; }
    /**
     * @brief Read one edge's currently stored discharge.
     *
     * @param[in] s Live initialized CPU solver with consistent attached mesh/state/options and internal
     *     buffers.
     * @param[in] e Valid zero-based solver edge index; unchecked here.
     * @return Discharge in m3/s, signed in the solver left-to-right edge orientation.
     */
    static double discharge(const ExplicitInertialSolver& s, int e) { return s.q_[e]; }
    /**
     * @brief Overwrite active-edge discharges with recognizable fixture markers.
     *
     * Sets q[e]=e+1 for active edges only. Values serve as markers rather than physical forcing; caller
     * prepares any subsequent operation that overwrites/uses them.
     *
     * @param[in,out] s Initialized solver with an active-face list indexing its discharge buffer.
     */
    static void markActiveFaces(ExplicitInertialSolver& s) {
        for (int e : s.active_faces_)
            s.q_[e] = e + 1.;
    }
    /**
     * @brief Book a prescribed interior face water/species transfer without settling cells.
     *
     * Subtracts volume from the left accumulator, adds it to the right, calls bookFaceSpecies with zero
     * secondary flux and the true firing flag, then marks accumulators pending. Does not check
     * indices/availability or alter cell volumes immediately.
     *
     * @param[in,out] s Initialized solver with consistent edge topology, water/species ledgers and
     *     attached cell/transport state.
     * @param[in] edge Valid interior edge index whose left/right cells and transfer storage exist.
     * @param[in] volume Signed water transfer in m3, positive from left to right; must satisfy the
     *     intended fixture state/availability assumptions.
     * @param[in] dt Positive transfer interval in seconds, forwarded to species bookkeeping.
     */
    static void bookFaceVolume(ExplicitInertialSolver& s, int edge, double volume, double dt) {
        const int a = s.edges_.cL[edge], b = s.edges_.cR[edge];
        [&]<class Solver>(Solver& solver) {
            if constexpr (requires { solver.face_accum_; }) {
                solver.face_accum_[edge * solver.face_accum_stride_] -= volume;
                solver.face_accum_[edge * solver.face_accum_stride_ + 1] += volume;
            } else {
                solver.facc_L_[edge] -= volume;
                solver.facc_R_[edge] += volume;
            }
        }(s);
        s.bookFaceSpecies(edge, a, b, volume, 0., dt, true);
        s.accumulators_pending_ = true;
    }
    /**
     * @brief Settle booked face water/species transfers into attached cell state.
     *
     * Calls settleAccumulators, consuming pending ledgers through production logic without a complete
     * scheduled advance.
     *
     * @param[in,out] s Initialized solver whose attached dependencies and pending/prepared buffers are
     *     consistent for the requested operation.
     */
    static void settle(ExplicitInertialSolver& s) { s.settleAccumulators(); }
    // Test-only logical view: no production aliases or cached strided views.
    template <class Solver> static auto packetRows(const Solver& s) {
        std::array<std::vector<double>, 6> rows;
        if constexpr (requires { s.face_accum_; }) {
            const auto stride = s.face_accum_stride_;
            for (std::size_t side = 0; side < stride; ++side)
                for (std::size_t e = 0; e < s.face_accum_.size() / stride; ++e)
                    rows[side].push_back(s.face_accum_[e * stride + side]);
        } else {
            rows = {s.facc_L_, s.facc_R_, s.macc_x_L_, s.macc_x_R_,
                    s.macc_y_L_, s.macc_y_R_};
        }
        return rows;
    }
    template <class Solver> static void seedPackets(Solver& s, bool nonfinite = false) {
        auto rows = packetRows(s);
        for (std::size_t side = 0; side < rows.size(); ++side)
            for (std::size_t e = 0; e < rows[side].size(); ++e)
                rows[side][e] = (e + side) % 7 == 0 ? -0.0 :
                    (side % 2 ? 1.0 : -1.0) * (side + 1) * (e + 1) * 0x1p-30;
        if (nonfinite && !rows[0].empty())
            rows[0][0] = std::numeric_limits<double>::quiet_NaN();
        if constexpr (requires { s.face_accum_; }) {
            for (std::size_t side = 0; side < s.face_accum_stride_; ++side)
                for (std::size_t e = 0; e < rows[side].size(); ++e)
                    s.face_accum_[e * s.face_accum_stride_ + side] = rows[side][e];
        } else {
            s.facc_L_ = rows[0]; s.facc_R_ = rows[1];
            s.macc_x_L_ = rows[2]; s.macc_x_R_ = rows[3];
            s.macc_y_L_ = rows[4]; s.macc_y_R_ = rows[5];
        }
        s.accumulators_pending_ = true;
    }
    static void firePackets(ExplicitInertialSolver& s, double dt) {
        s.fireFaces(s.active_faces_, dt, true);
    }
    static void rebindFixtureState(ExplicitInertialSolver& s, SurfaceStateData& state) {
        s.state_ = &state;
    }
    static bool livePackets(const ExplicitInertialSolver& s) { return s.initialized_; }
    static bool hasTierMix(const ExplicitInertialSolver& s) {
        int minimum = 1000, maximum = -1;
        for (int i : s.active_cells_) {
            minimum = std::min(minimum, int(s.tier_[i]));
            maximum = std::max(maximum, int(s.tier_[i]));
        }
        return minimum < maximum;
    }
    static int macroSubsteps(const ExplicitInertialSolver& s) {
        return 1 << (s.cells_by_tier_.size() - 1);
    }
    static double macroSpan(const ExplicitInertialSolver& s) {
        return macroSubsteps(s) * s.dt0_;
    }
    static long macroFaceVisits(const ExplicitInertialSolver& s) {
        long count = 0;
        for (std::size_t k = 0; k < s.edges_by_tier_.size(); ++k)
            count += s.edges_by_tier_[k].size() * (macroSubsteps(s) >> k);
        return count;
    }
    static bool allCellsInactive(const ExplicitInertialSolver& s) {
        return std::all_of(s.cell_active_.begin(), s.cell_active_.end(),
                           [](auto value) { return value == 0; });
    }
    static bool orderedLists(const ExplicitInertialSolver& s) {
        std::vector<int> active, faces;
        decltype(s.cells_by_tier_) cells(s.cells_by_tier_.size());
        decltype(s.edges_by_tier_) edges(s.edges_by_tier_.size());
        for (int i = 0; i < s.mesh_->n_cells(); ++i)
            if (s.cell_active_[i]) {
                active.push_back(i);
                cells[s.tier_[i]].push_back(i);
            }
        for (int e = 0; e < s.edges_.ne; ++e) {
            const int a = s.edges_.cL[e], b = s.edges_.cR[e];
            if (s.cell_active_[a] && s.cell_active_[b]) {
                const auto tier = std::min(s.tier_[a], s.tier_[b]);
                if (s.face_tier_[e] != tier)
                    return false;
                faces.push_back(e);
                edges[tier].push_back(e);
            } else if (s.q_[e] != 0.0) {
                return false;
            }
        }
        return active == s.active_cells_ && cells == s.cells_by_tier_ &&
               faces == s.active_faces_ && edges == s.edges_by_tier_;
    }
    static std::string orderedListsFrame(ExplicitInertialSolver& s) {
        openswmm::detail::CompletedSourceArchive a;
        a(s.dt0_, s.cell_active_, s.active_cells_, s.tier_, s.cells_by_tier_,
          s.face_tier_, s.edges_by_tier_, s.active_faces_, s.q_,
          s.state_->volume, s.state_->depth, s.state_->head, s.qcx_, s.qcy_);
        return a.bytes();
    }
    // Internal-state framing is deliberately separate from public restart qualification.
    static std::pair<std::string, std::string> packetArchive(ExplicitInertialSolver& s) {
        openswmm::detail::CompletedSourceArchive a;
        std::string error;
        try { s.checkpointState(a); }
        catch (const std::exception& e) { error = e.what(); }
        return {a.bytes(), error};
    }
    static std::pair<std::string, std::string> readPacketArchive(
            ExplicitInertialSolver& s, std::string bytes) {
        openswmm::detail::CompletedSourceArchive a(std::move(bytes));
        std::string error;
        try { s.checkpointState(a); a.finish(); }
        catch (const std::exception& e) { error = e.what(); }
        return {packetArchive(s).first, error};
    }

    /**
     * @brief Book a prescribed interior transfer and immediately settle pending accumulators.
     *
     * Calls bookFaceVolume then settle; mutates cell water/transport state and consumes pending ledgers
     * according to production settlement behavior. This is a fixture operation, not a complete advance.
     *
     * @param[in,out] s Initialized solver with consistent edge topology, water/species ledgers and
     *     attached cell/transport state.
     * @param[in] edge Valid interior edge index whose left/right cells and transfer storage exist.
     * @param[in] volume Signed water transfer in m3, positive from left to right; must satisfy the
     *     intended fixture state/availability assumptions.
     * @param[in] dt Positive transfer interval in seconds, forwarded to species bookkeeping.
     */
    static void transferFaceVolume(ExplicitInertialSolver& s, int edge, double volume, double dt) {
        bookFaceVolume(s, edge, volume, dt);
        settle(s);
    }
};
}
namespace {
/**
 * @brief Build a flat unit-spaced square mesh for scheduler/conservation cases.
 *
 * No count validation occurs here; allocation/topology failures propagate.
 *
 * @param[in] n Positive number of one-metre square subdivisions per axis; derived counts must fit
 *     integer/storage limits.
 * @param[in] quad True creates one quad per square; false creates two triangles.
 * @return Owned mesh with topology/geometry built and other properties at defaults.
 */
MeshData grid(int n, bool quad = false) {
    MeshData m;
    m.resize_vertices((n + 1) * (n + 1));
    for (int y = 0; y <= n; ++y)
        for (int x = 0; x <= n; ++x) {
            int v = y * (n + 1) + x;
            m.vx[v] = x;
            m.vy[v] = y;
        }
    m.resize_triangles((quad ? 1 : 2) * n * n);
    int c = 0;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            int a = y * (n + 1) + x, b = a + 1, d = a + n + 1, e = d + 1;
            if (quad)
                m.set_quad(c++, a, b, e, d);
            else {
                m.set_triangle(c++, a, b, e);
                m.set_triangle(c++, a, e, d);
            }
        }
    buildMeshTopology(m);
    return m;
}
/**
 * @brief Sum a fixture numeric vector without changing its entries.
 *
 * Uses accumulate with double zero; applies no weights, unit conversion, compensation or
 * finite-value checks.
 *
 * @param[in] x Borrowed values in caller-consistent units.
 * @return Ordered double sum in the input units, zero for an empty vector.
 */
double sum(const std::vector<double>& x) {
    return std::accumulate(x.begin(), x.end(), 0.);
}
/**
 * @brief Sum applied infiltration depths times cell area.
 *
 * Does not mutate either input or check array bounds; values are summed as stored without
 * finite/nonnegative filtering.
 *
 * @param[in] m Mesh with valid per-cell area in m2.
 * @param[in] s Compatible surface state with at least m.n_cells() applied infiltration depths in
 *     metres.
 * @return Total applied infiltration volume in m3; zero for no cells.
 */
double infiltration(const MeshData& m, const SurfaceStateData& s) {
    double v = 0;
    for (int i = 0; i < m.n_cells(); ++i)
        v += s.infil_applied[i] * m.tri_area[i];
    return v;
}
/**
 * @brief Allocate fixture state with uniform area-scaled initial water volume.
 *
 * @param[in] m Mesh with consistent cell/vertex counts and per-cell area in m2.
 * @param[in] h Intended nonnegative initial depth in metres, used without validation.
 * @return Owned resized surface state with volume=h*area in m3; head/depth and unrelated fields
 *     retain resize defaults until solver initialization.
 */
SurfaceStateData state(const MeshData& m, double h) {
    SurfaceStateData s;
    s.resize(m.n_cells(), m.n_vertices());
    for (int i = 0; i < m.n_cells(); ++i)
        s.volume[i] = h * m.tri_area[i];
    return s;
}
/**
 * @brief Create the CPU fixture options with one-second timestep cap and CFL 0.4.
 *
 * @param[in] threads Supported intended positive CPU thread count; not validated by this helper.
 * @param[in] tiers Supported intended positive local-timestep tier count; not validated by this
 *     helper.
 * @return Owned options with requested thread/tier settings, one-second cap and CFL 0.4; other
 *     options retain production defaults.
 */
SolverOptions2D options(int threads = 1, int tiers = 1) {
    SolverOptions2D o;
    o.num_threads = threads;
    o.lts_tiers = tiers;
    o.max_timestep = 1;
    o.cfl_number = .4;
    return o;
}
}
TEST(Cpu2DCorrectness, FrozenCoarseTierRespectsRefreshedCfl) {
    auto m = grid(16);
    auto s = state(m, .0041);
    auto o = options(4, 4);
    for (int i = 0; i < m.n_cells(); ++i)
        if (m.tri_cx[i] < 4)
            s.volume[i] = .3 * m.tri_area[i];
    ExplicitInertialSolver solver;
    solver.initialize(m, s, o);
    using A = ExplicitInertialSolverTestAccess;
    A::rebuild(solver);
    int coarse = -1;
    for (int i = 0; i < m.n_cells(); ++i)
        if (A::tier(solver, i) > 0) {
            coarse = i;
            break;
        }
    ASSERT_GE(coarse, 0);
    s.depth[coarse] = .2;
    const double bound = inertial::cellCflDt(o.cfl_number, A::length(solver, coarse), .2, 0);
    A::refresh(solver);
    EXPECT_LE(A::step(solver, coarse), bound * (1 + 1e-14));
}
TEST(Cpu2DCorrectness, MacroLandingPublishesAllConservedState) {
    for (int threads : {1, 4}) {
        auto m = grid(32);
        auto s = state(m, 0);
        auto o = options(threads, 4);
        s.transport.resize(1, m.n_cells(), 0);
        for (int i = 0; i < m.n_cells(); ++i) {
            s.volume[i] = m.tri_cx[i] < 16 ? .2 * m.tri_area[i] : 0;
            s.transport.cell_mass[i] = 3 * s.volume[i];
        }
        const double v = sum(s.volume), mass = sum(s.transport.cell_mass);
        ExplicitInertialSolver solver;
        solver.initialize(m, s, o);
        // Former exact macro-cycle boundary: storage excluded 3.55% in-flight water.
        double t = .93282250995272253;
        solver.advance(0, t);
        EXPECT_NEAR(sum(s.volume), v, 1e-9);
        EXPECT_NEAR(sum(s.transport.cell_mass), mass, 1e-9);
        solver.advance(t, 2 * t);
        EXPECT_NEAR(sum(s.volume), v, 1e-9);
        EXPECT_NEAR(sum(s.transport.cell_mass), mass, 1e-9);
        solver.finalize();
        EXPECT_NEAR(sum(s.volume), v, 1e-9);
    }
}
TEST(Cpu2DCorrectness, CshChannelHeatAdvectionMatchesAcceptedMeshFaceTransfer) {
    // CSHComponent commit 6d95455e6b655a7842041c108e66ad4ab7f4b7aa,
    // examples/Versteeg/case1/case1_hybrid.inp: with transient storage off,
    // the receiving element's heat capacity is rho_w*cp*V. Reducing its
    // computeDTDt advection term to a single inflow Q at temperature T_in,
    // with no outflow, dispersion or surface flux, gives d(V*T)/dt=Q*T_in.
    // Use the example's V=area*length=1*0.2 m3 and Q=0.1 m3/s, then compare
    // that independent finite-volume balance with the production mesh-face
    // transport accumulator. This is a source-equation comparison, not a
    // whole CSH executable trajectory comparison.
    auto m = grid(1);
    auto s = state(m, 0);
    auto o = options();
    o.dispersion = 0;
    constexpr double initial_volume = .2, initial_temperature = 10., source_temperature = 20.;
    constexpr double q = .1, dt = .2, transfer = q * dt;
    s.volume.assign(2, initial_volume);
    s.transport.resize(1, 2, 0);
    s.transport.temp_row = 0;
    s.transport.row_names = {"__TEMPERATURE__"};
    s.transport.cell_mass[0] = initial_volume * initial_temperature;
    s.transport.cell_mass[1] = initial_volume * source_temperature;
    ExplicitInertialSolver solver;
    solver.initialize(m, s, o);
    const auto& edges = ExplicitInertialSolverTestAccess::edges(solver);
    ASSERT_EQ(edges.ne, 1);
    const int source = edges.cL[0], receiver = edges.cR[0];
    s.transport.cell_mass[s.transport.idx(0, source)] = initial_volume * source_temperature;
    s.transport.cell_mass[s.transport.idx(0, receiver)] = initial_volume * initial_temperature;
    ExplicitInertialSolverTestAccess::transferFaceVolume(solver, 0, transfer, dt);

    const double expected_temperature =
        (initial_volume * initial_temperature + transfer * source_temperature) /
        (initial_volume + transfer);
    EXPECT_NEAR(s.volume[source], initial_volume - transfer, 1e-14);
    EXPECT_NEAR(s.volume[receiver], initial_volume + transfer, 1e-14);
    EXPECT_NEAR(s.transport.cell_mass[s.transport.idx(0, source)] / s.volume[source],
                source_temperature, 1e-13);
    EXPECT_NEAR(s.transport.cell_mass[s.transport.idx(0, receiver)] / s.volume[receiver],
                expected_temperature, 1e-13);
    const double rho_cp = 1000. * 4184.;
    const double receiver_heat_change =
        rho_cp * (s.transport.cell_mass[s.transport.idx(0, receiver)] -
                  initial_volume * initial_temperature);
    EXPECT_NEAR(receiver_heat_change, rho_cp * transfer * source_temperature, 1e-8);
    EXPECT_NEAR(s.transport.cell_mass[0] + s.transport.cell_mass[1],
                initial_volume * (initial_temperature + source_temperature), 1e-14);
}
TEST(Cpu2DCorrectness, CshFiveElementHeatAdvectionMatchesIndependentTrajectory) {
    // Match the five 0.2 m long, 1 m2 CSH elements and Q=0.1 m3/s in
    // HydroCouple/CSHComponent commit 6d95455e6b655a7842041c108e66ad4ab7f4b7aa,
    // examples/Versteeg/case1/case1_hybrid.inp. Disable transient storage,
    // longitudinal dispersion and all heat fluxes. Perturb the upstream
    // temperature to 20 C (the pinned sample starts every element at 10 C)
    // and integrate CSH's source advection ODE independently with RK4:
    // dT_i/dt = Q/V * (T_{i-1} - T_i), with a fixed-temperature inlet.
    // The 2D comparison uses five 1 m by 0.2 m quads, each with V=0.2 m3,
    // and the production face donor/gather path at a smaller explicit step.
    MeshData m;
    m.resize_vertices(12);
    for (int y = 0; y <= 1; ++y)
        for (int x = 0; x <= 5; ++x) {
            const int v = y * 6 + x;
            m.vx[v] = .2 * x;
            m.vy[v] = y;
        }
    m.resize_triangles(5);
    for (int i = 0; i < 5; ++i)
        m.set_quad(i, i, i + 1, 7 + i, 6 + i);
    buildMeshTopology(m);

    auto s = state(m, 0);
    s.volume.assign(5, .2);
    s.transport.resize(1, 5, 0);
    s.transport.temp_row = 0;
    s.transport.row_names = {"__TEMPERATURE__"};
    for (int i = 0; i < 5; ++i)
        s.transport.cell_mass[i] = .2 * 10.;
    auto o = options();
    o.dispersion = 0;
    ExplicitInertialSolver solver;
    solver.initialize(m, s, o);
    const auto& edges = ExplicitInertialSolverTestAccess::edges(solver);
    ASSERT_EQ(edges.ne, 4);
    std::array<int, 4> face{};
    face.fill(-1);
    for (int e = 0; e < edges.ne; ++e) {
        ASSERT_EQ(edges.cR[e], edges.cL[e] + 1);
        face[std::size_t(edges.cL[e])] = e;
    }
    for (int i = 0; i < 4; ++i)
        ASSERT_GE(face[std::size_t(i)], 0);

    constexpr double q = .1, water_volume = .2, inlet_temperature = 20.;
    constexpr double end_time = .2, dt_2d = .001, transfer = q * dt_2d;
    std::array<double, 5> reference{};
    reference.fill(10.);
    const auto rate = [&](const std::array<double, 5>& t) {
        std::array<double, 5> d{};
        d[0] = q / water_volume * (inlet_temperature - t[0]);
        for (int i = 1; i < 5; ++i)
            d[std::size_t(i)] = q / water_volume * (t[std::size_t(i - 1)] - t[std::size_t(i)]);
        return d;
    };
    constexpr double h = .0001;
    for (double elapsed = 0; elapsed < end_time; elapsed += h) {
        const auto k1 = rate(reference);
        auto stage = reference;
        for (int i = 0; i < 5; ++i)
            stage[std::size_t(i)] += .5 * h * k1[std::size_t(i)];
        const auto k2 = rate(stage);
        stage = reference;
        for (int i = 0; i < 5; ++i)
            stage[std::size_t(i)] += .5 * h * k2[std::size_t(i)];
        const auto k3 = rate(stage);
        stage = reference;
        for (int i = 0; i < 5; ++i)
            stage[std::size_t(i)] += h * k3[std::size_t(i)];
        const auto k4 = rate(stage);
        for (int i = 0; i < 5; ++i)
            reference[std::size_t(i)] += h *
                                         (k1[std::size_t(i)] + 2 * k2[std::size_t(i)] +
                                          2 * k3[std::size_t(i)] + k4[std::size_t(i)]) /
                                         6.;
    }

    constexpr int nsteps = static_cast<int>(end_time / dt_2d);
    double boundary_heat_j = 0;
    const double rho_cp = 1000. * 4184.;
    for (int step = 0; step < nsteps; ++step) {
        const double out_temperature = s.transport.cell_mass[4] / s.volume[4];
        boundary_heat_j += rho_cp * transfer * (inlet_temperature - out_temperature);
        for (int i = 0; i < 4; ++i)
            ExplicitInertialSolverTestAccess::bookFaceVolume(solver, face[std::size_t(i)], transfer,
                                                             dt_2d);
        s.volume[0] += transfer;
        s.transport.cell_mass[0] += transfer * inlet_temperature;
        s.volume[4] -= transfer;
        s.transport.cell_mass[4] -= transfer * out_temperature;
        ExplicitInertialSolverTestAccess::settle(solver);
    }
    double initial_temp_volume = 0, final_temp_volume = 0;
    for (int i = 0; i < 5; ++i) {
        const double temperature = s.transport.cell_mass[i] / s.volume[i];
        EXPECT_NEAR(s.volume[i], water_volume, 1e-13) << "element " << i;
        EXPECT_NEAR(temperature, reference[std::size_t(i)], 1e-3) << "element " << i;
        initial_temp_volume += water_volume * 10.;
        final_temp_volume += s.transport.cell_mass[i];
    }
    EXPECT_NEAR(rho_cp * (final_temp_volume - initial_temp_volume), boundary_heat_j,
                1e-10 * std::max(1., std::abs(boundary_heat_j)));
}
TEST(Cpu2DCorrectness, FlowAndHeldSinksShareAvailableWater) {
    for (bool quad : {false, true})
        for (int tiers : {1, 4})
            for (int mode : {0, 1, 2}) {
                auto m = grid(16, quad);
                auto s = state(m, 0);
                auto o = options(4, tiers);
                o.momentum = static_cast<Momentum2D>(mode);
                for (int i = 0; i < m.n_cells(); ++i) {
                    double h = m.tri_cx[i] < 8 ? .2 : 0;
                    s.volume[i] = h * m.tri_area[i];
                    s.infil_rate[i] = h;
                }
                double v = sum(s.volume);
                ExplicitInertialSolver solver;
                solver.initialize(m, s, o);
                solver.advance(0, 1);
                EXPECT_NEAR(sum(s.volume) + infiltration(m, s), v, 1e-9);
                EXPECT_GE(*std::min_element(s.volume.begin(), s.volume.end()), 0);
            }
}
TEST(Cpu2DCorrectness, CompetingSinksBookOnlyAppliedAmounts) {
    for (double h : {.002, .1}) {
        auto m = grid(1, true);
        auto s = state(m, h);
        auto o = options();
        s.infil_rate[0] = h * 2;
        s.evap_rate[0] = h * 2;
        s.coupling_flux[0] = -h * 2;
        s.transport.resize(3, 1, 0);
        s.transport.age_row = 1;
        s.transport.temp_row = 2;
        s.transport.cell_mass = {2 * h, 3 * h, -5 * h};
        ExplicitInertialSolver solver;
        solver.initialize(m, s, o);
        solver.advance(0, 1);
        EXPECT_NEAR(sum(s.volume) + infiltration(m, s) + s.evap_loss_total - s.coupling_applied[0],
                    h, 1e-13);
        EXPECT_NEAR(infiltration(m, s), h / 3, 1e-13);
        EXPECT_NEAR(s.evap_loss_total, h / 3, 1e-13);
        // Evaporation leaves solute behind, but carries age and signed temperature.
        EXPECT_NEAR(s.transport.cell_mass[0], 2 * h / 3, 1e-13);
        EXPECT_NEAR(s.transport.cell_mass[1], 0, 1e-13);
        EXPECT_NEAR(s.transport.cell_mass[2], 0, 1e-13);
        EXPECT_NEAR(s.transport.lost_infiltration[0], 2 * h / 3, 1e-13);
    }
}
TEST(Cpu2DCorrectness, BalancedInactiveWaterStillTransportsRainMass) {
    auto m = grid(1, true);
    auto s = state(m, .002);
    auto o = options();
    s.rainfall[0] = s.infil_rate[0] = .0001;
    s.transport.resize(1, 1, 0);
    s.transport.rain_conc = {2};
    ExplicitInertialSolver solver;
    solver.initialize(m, s, o);
    solver.advance(0, 1);
    EXPECT_NEAR(s.volume[0], .002, 1e-14);
    EXPECT_GT(s.transport.cell_mass[0], 0);
    EXPECT_NEAR(s.transport.cell_mass[0] + s.transport.lost_infiltration[0], .0002, 1e-14);
}
TEST(Cpu2DCorrectness, ParallelSpeciesAndRunoffLedgersClose) {
    std::vector<double> reference;
    for (int threads : {1, 2, 4, 8})
        for (int repeat = 0; repeat < 3; ++repeat) {
            auto m = grid(32);
            auto s = state(m, .1);
            auto o = options(threads, 4);
            s.transport.resize(2, m.n_cells(), 0);
            s.transport.rain_conc = {2, 7};
            s.transport.cell_runoff_vol.assign(m.n_cells(), 0);
            for (int i = 0; i < m.n_cells(); ++i) {
                s.volume[i] *= 1 + .2 * std::sin(m.tri_cx[i]);
                s.rainfall[i] = .0001;
                s.infil_rate[i] = .0002;
                for (int r = 0; r < 2; ++r)
                    s.transport.cell_mass[s.transport.idx(r, i)] = (r + 1) * s.volume[i];
            }
            double v = sum(s.volume);
            ExplicitInertialSolver solver;
            solver.initialize(m, s, o);
            solver.advance(0, 2);
            for (int r = 0; r < 2; ++r) {
                double mass = 0;
                for (int i = 0; i < m.n_cells(); ++i)
                    mass += s.transport.cell_mass[s.transport.idx(r, i)];
                EXPECT_NEAR(mass + s.transport.lost_infiltration[r] - (r + 1) * v -
                                s.transport.gained_rainfall[r],
                            0, 1e-9);
            }
            EXPECT_NEAR(sum(s.transport.cell_runoff_vol), 0, 1e-9);
            if (reference.empty())
                reference = s.transport.cell_runoff_vol;
            else
                EXPECT_EQ(s.transport.cell_runoff_vol, reference);
        }
}
TEST(Cpu2DCorrectness, ParallelInfiltrationTransfersCellLocalSpeciesToGroundwater) {
    for (int threads : {1, 4, 8}) {
        auto m = grid(24);
        auto s = state(m, .1);
        auto o = options(threads, 4);
        InertialEdges ed;
        ed.build(m);
        SubsurfaceSolver gw;
        SubsurfaceConfig cfg;
        GwAquiferRow row;
        row.hg0 = .5;
        row.Ks = 1e-6;
        cfg.rows.push_back(row);
        std::vector<std::string> warnings;
        ASSERT_TRUE(gw.initialize(m, ed, o, GwUnitFactors{}, 0, cfg, warnings).empty());
        RowLayoutLite layout;
        layout.n_species = layout.n_pollut = 1;
        layout.names = {"Tracer"};
        gw.initTransport(layout, nullptr, {}, warnings);
        s.transport.resize(1, m.n_cells(), 0);
        s.transport.row_names = layout.names;
        for (int i = 0; i < m.n_cells(); ++i) {
            s.infil_rate[i] = .001;
            s.transport.cell_mass[i] = (1 + i % 5) * s.volume[i];
        }
        const double initial = sum(s.transport.cell_mass);
        ExplicitInertialSolver solver;
        solver.setSubsurface(&gw);
        solver.initialize(m, s, o);
        solver.advance(0, 1);
        EXPECT_NEAR(sum(s.transport.cell_mass) + gw.transport().storage(0), initial, 1e-9);
        EXPECT_NEAR(s.transport.lost_infiltration[0], gw.transport().storage(0), 1e-9);
    }
}
TEST(Cpu2DCorrectness, GroundwaterCouplingRejectsMissingSpeciesNames) {
    for (bool missing_surface : {false, true}) {
        auto m = grid(1);
        auto s = state(m, .1);
        auto o = options();
        InertialEdges ed;
        ed.build(m);
        SubsurfaceSolver gw;
        SubsurfaceConfig cfg;
        GwAquiferRow row;
        row.hg0 = .5;
        cfg.rows.push_back(row);
        std::vector<std::string> warnings;
        ASSERT_TRUE(gw.initialize(m, ed, o, GwUnitFactors{}, 0, cfg, warnings).empty());
        RowLayoutLite layout;
        layout.n_species = layout.n_pollut = 1;
        layout.names = {"Tracer"};
        gw.initTransport(layout, nullptr, {}, warnings);
        s.transport.resize(1, m.n_cells(), 0);
        s.transport.row_names = layout.names;
        if (missing_surface)
            s.transport.row_names.clear();
        else
            gw.transport().row_names.clear();
        ExplicitInertialSolver solver;
        solver.setSubsurface(&gw);
        EXPECT_THROW(solver.initialize(m, s, o), std::invalid_argument);
    }
}
TEST(Cpu2DCorrectness, Rk2EvaporationLedgerMatchesItsAveragedState) {
    auto m = grid(2, true);
    auto s = state(m, .1);
    auto o = options();
    o.momentum = Momentum2D::FULL_SWE;
    o.reconstruction_order = 2;
    std::fill(s.evap_rate.begin(), s.evap_rate.end(), .01);
    double initial = sum(s.volume);
    ExplicitInertialSolver solver;
    solver.initialize(m, s, o);
    solver.advance(0, 1);
    EXPECT_NEAR(sum(s.volume) + s.evap_loss_total, initial, 1e-12);
}

TEST(Cpu2DCorrectness, BlockedWetDryFaceRetainsHydrostaticPressure) {
    swe::FaceFlux f;
    double lx, ly, rx, ry;
    EXPECT_TRUE(swe::faceFlux(.1, .1, 0, 0, 1, 0, 0, 0, 1, 0, 1e-7, f, lx, ly, rx, ry));
    EXPECT_EQ(f.mass, 0);
    EXPECT_EQ(f.mx, 0);
    EXPECT_NEAR(lx, -.5 * inertial::kGravity * .01, 1e-15);
    EXPECT_EQ(ly, 0);
    EXPECT_EQ(rx, 0);
    EXPECT_EQ(ry, 0);
    EXPECT_TRUE(swe::faceFluxRecon(.1, 0, 0, 0, .1, 1, 0, 0, 1, 0, 1, 0, 1e-7, f, lx, ly, rx, ry));
    EXPECT_EQ(f.mass, 0);
    EXPECT_NEAR(lx, -.5 * inertial::kGravity * .01, 1e-15);
}
TEST(Cpu2DCorrectness, LakeAtRestAgainstAnEmergedBump) {
    for (bool quad : {false, true})
        for (int order : {1, 2})
            for (auto closure : {CellClosure2D::FLAT, CellClosure2D::VFR}) {
                auto m = grid(16, quad);
                for (int i = 0; i < m.n_vertices(); ++i) {
                    double x = m.vx[i] - 8;
                    m.vz[i] = std::abs(x) < 2 ? .2 - .05 * x * x : 0;
                }
                buildMeshTopology(m);
                auto s = state(m, 0);
                auto o = options(4, 1);
                o.momentum = Momentum2D::FULL_SWE;
                o.reconstruction_order = order;
                o.cell_closure = closure;
                o.dry_depth = 1e-7;
                o.h_move = 1e-6;
                for (int i = 0; i < m.n_cells(); ++i) {
                    m.mannings_n[i] = 0;
                    s.volume[i] = inertial::cellVolumeFromEta(m, o, i, .1);
                }
                auto initial = s.volume;
                ExplicitInertialSolver solver;
                solver.initialize(m, s, o);
                solver.advance(0, 10);
                for (int i = 0; i < m.n_cells(); ++i)
                    EXPECT_NEAR(s.volume[i], initial[i], 1e-10);
            }
}
TEST(Cpu2DCorrectness, SecondOrderThackerDoesNotAccelerateTrappedFilms) {
    auto m = grid(64);
    for (int i = 0; i < m.n_vertices(); ++i) {
        m.vx[i] /= 16;
        m.vy[i] /= 16;
        double x = m.vx[i] - 2, y = m.vy[i] - 2;
        m.vz[i] = .1 * (x * x + y * y - 1);
    }
    buildMeshTopology(m);
    auto s = state(m, 0);
    auto o = options();
    o.momentum = Momentum2D::FULL_SWE;
    o.reconstruction_order = 2;
    o.dry_depth = 1e-7;
    o.h_move = 1e-6;
    o.max_timestep = .25;
    const double omega = std::sqrt(2 * inertial::kGravity * .1),
                 period = 2 * 3.14159265358979323846 / omega;
    for (int i = 0; i < m.n_cells(); ++i) {
        double x = m.tri_cx[i] - 2.5, y = m.tri_cy[i] - 2;
        s.volume[i] = std::max(0., .1 * (1 - x * x - y * y)) * m.tri_area[i];
        m.mannings_n[i] = 0;
        m.tri_init_v[i] = .5 * omega;
    }
    const double initial = sum(s.volume);
    ExplicitInertialSolver solver;
    solver.initialize(m, s, o);
    for (double t = 0; t < 2;) {
        double next = std::min(2., t + period / 16);
        solver.advance(t, next);
        t = next;
    }
    EXPECT_NEAR(sum(s.volume), initial, 1e-12);
    // The old wet-cell-only fallback developed >300 m/s in a 1.7 µm film,
    // requiring >23,000 substeps just to reach t=2 on this mesh.
    EXPECT_LT(solver.run_stats().nsteps, 10000);
    double err = 0, ref = 0;
    for (int i = 0; i < m.n_cells(); ++i) {
        double x = m.tri_cx[i] - 2 - .5 * std::cos(2 * omega),
               y = m.tri_cy[i] - 2 - .5 * std::sin(2 * omega);
        double h = std::max(0., .1 * (1 - x * x - y * y));
        err += std::abs(s.depth[i] - h) * m.tri_area[i];
        ref += h * m.tri_area[i];
    }
    EXPECT_LT(err / ref, .3);
}

TEST(Cpu2DCorrectness, InactiveRainLandingSeedsFlowAndClosesSpeciesBudget) {
    for (bool quad : {false, true})
        for (int mode : {0, 1, 2})
            for (int threads : {1, 4}) {
                auto m = grid(16, quad);
                auto s = state(m, 0);
                auto o = options(threads, 4);
                o.momentum = static_cast<Momentum2D>(mode);
                s.transport.resize(1, m.n_cells(), 0);
                s.transport.rain_conc = {3};
                ExplicitInertialSolver solver;
                solver.initialize(m, s, o);
                solver.advance(0, 2);
                EXPECT_EQ(solver.run_stats().nrhs, 0);
                double rain_rate = 0;
                for (int i = 0; i < m.n_cells(); ++i)
                    if (m.tri_cx[i] < 1) {
                        s.rainfall[i] = .01;
                        rain_rate += .01 * m.tri_area[i];
                    }
                // Land lazy sources directly at a rebuild, the path fused with seed
                // selection. Newly wetted cells must be eligible in this same rebuild.
                ExplicitInertialSolverTestAccess::rebuildAt(solver, 3);
                EXPECT_NEAR(sum(s.volume), rain_rate, 1e-12);
                EXPECT_FALSE(ExplicitInertialSolverTestAccess::faces(solver).empty());
                for (int i = 0; i < m.n_cells(); ++i)
                    if (m.tri_cx[i] < 1)
                        EXPECT_NEAR(s.depth[i], .01, 1e-12);
                solver.advance(3, 4);
                EXPECT_GT(solver.run_stats().nrhs, 0);
                std::fill(s.rainfall.begin(), s.rainfall.end(), 0);
                std::fill(s.infil_rate.begin(), s.infil_rate.end(), .02);
                for (int t = 4; t < 9; ++t) {
                    solver.advance(t, t + 1);
                    EXPECT_NEAR(sum(s.volume) + infiltration(m, s), 2 * rain_rate, 1e-10);
                    EXPECT_NEAR(sum(s.transport.cell_mass) + s.transport.lost_infiltration[0],
                                6 * rain_rate, 1e-10);
                    EXPECT_GE(*std::min_element(s.volume.begin(), s.volume.end()), 0);
                }
                EXPECT_LT(sum(s.volume), rain_rate);
            }
}

TEST(Cpu2DCorrectness, SparseRebuildRetiresMomentumAndOrdersFaces) {
    using A = ExplicitInertialSolverTestAccess;
    for (bool quad : {false, true})
        for (int mode : {0, 1, 2}) {
            auto m = grid(16, quad);
            auto s = state(m, .002);
            auto o = options(4, 4);
            o.momentum = static_cast<Momentum2D>(mode);
            std::fill(m.tri_init_u.begin(), m.tri_init_u.end(), .25);
            ExplicitInertialSolver solver;
            solver.initialize(m, s, o);
            A::rebuild(solver);
            // Optional initial velocity may seed q before any face list exists.
            // A first rebuild must clear that momentum on inactive faces too.
            EXPECT_TRUE(A::faces(solver).empty());
            for (int e = 0; e < A::edges(solver).ne; ++e)
                EXPECT_EQ(A::discharge(solver, e), 0);
            for (int i = 0; i < m.n_cells(); ++i)
                s.volume[i] = .1 * m.tri_area[i];
            solver.resyncFromVolumes(0);
            A::rebuild(solver);
            A::markActiveFaces(solver);
            // Retain a small wet patch, then dry everything, then rewet elsewhere.
            // resync keeps face momentum: rebuild must retire it as cells dry.
            for (int stage = 0; stage < 3; ++stage) {
                for (int i = 0; i < m.n_cells(); ++i) {
                    const bool wet = stage == 0 ? m.tri_cx[i] < 1 : stage == 2 && m.tri_cx[i] > 15;
                    s.volume[i] = wet ? .1 * m.tri_area[i] : 0;
                }
                solver.resyncFromVolumes(0);
                A::rebuild(solver);
                const auto& ed = A::edges(solver);
                std::vector<int> expected;
                for (int e = 0; e < ed.ne; ++e) {
                    if (A::active(solver, ed.cL[e]) && A::active(solver, ed.cR[e]))
                        expected.push_back(e);
                    else
                        EXPECT_EQ(A::discharge(solver, e), 0);
                    if (stage > 0)
                        EXPECT_EQ(A::discharge(solver, e), 0);
                }
                EXPECT_EQ(A::faces(solver), expected);
                if (stage == 1)
                    EXPECT_TRUE(expected.empty());
                else
                    EXPECT_FALSE(expected.empty());
            }
        }
}

TEST(Cpu2DCorrectness, ManySpeciesKeepIndependentBudgetsOnMixedWetDryMesh) {
    // Alternating quads and split squares give different CSR row lengths.
    // Distinct tracer values, including a signed row, expose cross-row mixing.
    constexpr int n = 8;
    MeshData m;
    m.resize_vertices((n + 1) * (n + 1));
    for (int y = 0; y <= n; ++y)
        for (int x = 0; x <= n; ++x) {
            const int v = y * (n + 1) + x;
            m.vx[v] = x;
            m.vy[v] = y;
        }
    m.resize_triangles(3 * n * n / 2);
    int cell = 0;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const int a = y * (n + 1) + x, b = a + 1, d = a + n + 1, e = d + 1;
            if ((x + y) % 2 == 0)
                m.set_quad(cell++, a, b, e, d);
            else {
                m.set_triangle(cell++, a, b, e);
                m.set_triangle(cell++, a, e, d);
            }
        }
    buildMeshTopology(m);
    for (int ns : {1, 7, 8, 9, 17})
        for (int threads : {1, 4})
            for (int mode : {0, 1, 2}) {
                SCOPED_TRACE(::testing::Message()
                             << "species=" << ns << " threads=" << threads << " mode=" << mode);
                auto s = state(m, 0);
                auto o = options(threads, 4);
                o.momentum = static_cast<Momentum2D>(mode);
                s.transport.resize(ns, m.n_cells(), 0);
                s.transport.temp_row = ns - 1;
                s.transport.rain_conc.resize(ns);
                for (int r = 0; r < ns; ++r)
                    s.transport.rain_conc[r] = r == ns - 1 ? -4.0 : .7 * (r + 1);
                for (int i = 0; i < m.n_cells(); ++i) {
                    s.volume[i] = m.tri_cx[i] < n / 2 ? .2 * m.tri_area[i] : 0;
                    s.rainfall[i] = .005;
                    s.infil_rate[i] = .001;
                    for (int r = 0; r < ns; ++r)
                        s.transport.cell_mass[s.transport.idx(r, i)] =
                            s.transport.rain_conc[r] * s.volume[i];
                }
                const double initial_volume = sum(s.volume);
                ExplicitInertialSolver solver;
                solver.initialize(m, s, o);
                double time = 0;
                for (double next : {.2, .6, 1.4, 2.0}) {
                    solver.advance(time, next);
                    // Explicit resync also settles any outstanding per-face bookings.
                    solver.resyncFromVolumes(next);
                    EXPECT_NEAR(sum(s.volume) + infiltration(m, s),
                                initial_volume + n * n * .005 * next, 1e-10);
                    for (int r = 0; r < ns; ++r) {
                        double mass = 0;
                        const double c = s.transport.rain_conc[r];
                        for (int i = 0; i < m.n_cells(); ++i) {
                            const double mi = s.transport.cell_mass[s.transport.idx(r, i)];
                            mass += mi;
                            EXPECT_NEAR(mi, c * s.volume[i], 1e-11);
                        }
                        EXPECT_NEAR(mass + s.transport.lost_infiltration[r] -
                                        s.transport.gained_rainfall[r],
                                    c * initial_volume, 1e-9);
                    }
                    time = next;
                }
            }
}

TEST(Cpu2DCorrectness, ManySpeciesTransfersConserveSurfaceAndGroundwaterMass) {
    for (int ns : {1, 9, 17})
        for (int threads : {1, 4})
            for (bool quad : {false, true}) {
                SCOPED_TRACE(::testing::Message()
                             << "species=" << ns << " threads=" << threads << " quad=" << quad);
                auto m = grid(8, quad);
                auto s = state(m, .1);
                auto o = options(threads, 4);
                InertialEdges ed;
                ed.build(m);
                SubsurfaceSolver gw;
                SubsurfaceConfig cfg;
                GwAquiferRow aquifer;
                aquifer.hg0 = .5;
                aquifer.Ks = 1e-6;
                cfg.rows.push_back(aquifer);
                std::vector<std::string> warnings;
                ASSERT_TRUE(gw.initialize(m, ed, o, GwUnitFactors{}, 0, cfg, warnings).empty());
                RowLayoutLite layout;
                layout.n_species = layout.n_pollut = ns;
                for (int r = 0; r < ns; ++r)
                    layout.names.push_back("Tracer" + std::to_string(r));
                gw.initTransport(layout, nullptr, {}, warnings);
                s.transport.resize(ns, m.n_cells(), 0);
                s.transport.row_names = layout.names;
                std::vector<double> initial(ns, 0.0);
                for (int i = 0; i < m.n_cells(); ++i) {
                    s.infil_rate[i] = .001;
                    for (int r = 0; r < ns; ++r) {
                        const double mass = (1 + r + i % 5) * s.volume[i];
                        s.transport.cell_mass[s.transport.idx(r, i)] = mass;
                        initial[r] += mass;
                    }
                }
                ExplicitInertialSolver solver;
                solver.setSubsurface(&gw);
                solver.initialize(m, s, o);
                for (int t = 0; t < 4; ++t) {
                    solver.advance(t, t + 1);
                    for (int r = 0; r < ns; ++r) {
                        double surface = 0;
                        for (int i = 0; i < m.n_cells(); ++i)
                            surface += s.transport.cell_mass[s.transport.idx(r, i)];
                        EXPECT_NEAR(surface + gw.transport().storage(r), initial[r], 1e-8);
                        EXPECT_NEAR(s.transport.lost_infiltration[r], gw.transport().storage(r),
                                    1e-8);
                    }
                }
            }
}

TEST(Cpu2DCorrectness, SpeciesBudgetsSurviveChangingRainCouplingAndEvaporation) {
    for (int ns : {3, 9, 17})
        for (int threads : {1, 4})
            for (int mode : {0, 1, 2}) {
                SCOPED_TRACE(::testing::Message()
                             << "species=" << ns << " threads=" << threads << " mode=" << mode);
                auto m = grid(8);
                auto s = state(m, .05);
                auto o = options(threads, 4);
                o.momentum = static_cast<Momentum2D>(mode);
                auto& tr = s.transport;
                tr.resize(ns, m.n_cells(), 0);
                tr.age_row = ns - 2;
                tr.temp_row = ns - 1;
                tr.rain_conc.resize(ns);
                tr.coupling_src.assign(ns * m.n_cells(), 0.0);
                for (int r = 0; r < ns; ++r)
                    tr.rain_conc[r] = r == tr.temp_row ? -4.0 : r + 1.0;
                for (int i = 0; i < m.n_cells(); ++i)
                    for (int r = 0; r < ns; ++r)
                        tr.cell_mass[tr.idx(r, i)] = tr.rain_conc[r] * s.volume[i];
                const double initial_volume = sum(s.volume);
                double rain_volume = 0;
                ExplicitInertialSolver solver;
                solver.initialize(m, s, o);
                for (int t = 0; t < 8; ++t) {
                    const int phase = t % 4;
                    for (int i = 0; i < m.n_cells(); ++i) {
                        s.rainfall[i] = (phase == 0 || phase == 2) ? .004 : 0;
                        s.infil_rate[i] = .001;
                        s.evap_rate[i] = phase == 3 ? .002 : 0;
                        s.coupling_flux[i] = phase == 1 ? (m.tri_cx[i] < 4 ? .002 : -.002) : 0;
                        rain_volume += s.rainfall[i] * m.tri_area[i];
                        for (int r = 0; r < ns; ++r)
                            tr.coupling_src[tr.idx(r, i)] =
                                std::max(0.0, s.coupling_flux[i]) * tr.rain_conc[r];
                    }
                    solver.advance(t, t + 1);
                    EXPECT_NEAR(sum(s.volume) + infiltration(m, s) + s.evap_loss_total -
                                    sum(s.coupling_applied),
                                initial_volume + rain_volume, 1e-9);
                    for (int r = 0; r < ns; ++r) {
                        double mass = 0;
                        for (int i = 0; i < m.n_cells(); ++i) {
                            const double mi = tr.cell_mass[tr.idx(r, i)];
                            mass += mi;
                            if (r == tr.age_row || r == tr.temp_row)
                                EXPECT_NEAR(mi, tr.rain_conc[r] * s.volume[i], 1e-9);
                        }
                        if (r != tr.age_row && r != tr.temp_row)
                            EXPECT_NEAR(mass + tr.lost_infiltration[r] + tr.lost_coupling[r] -
                                            tr.gained_rainfall[r] - tr.gained_coupling[r],
                                        tr.rain_conc[r] * initial_volume, 1e-8);
                    }
                }
            }
}

TEST(Cpu2DCorrectness, SecondOrderSlopeReconstructionCarriesPhysicalMassFlux) {
    using A = ExplicitInertialSolverTestAccess;
    for (bool quad : {false, true})
        for (int threads : {1, 4}) {
            auto m = grid(8, quad);
            for (int i = 0; i < m.n_vertices(); ++i)
                m.vz[i] = .04 * m.vx[i] + .03 * m.vy[i];
            buildMeshTopology(m);
            auto s = state(m, .5);
            auto o = options(threads, 1);
            o.momentum = Momentum2D::FULL_SWE;
            o.reconstruction_order = 2;
            for (int i = 0; i < m.n_cells(); ++i) {
                m.tri_init_u[i] = .2;
                m.tri_init_v[i] = -.1;
                m.mannings_n[i] = 0;
            }
            ExplicitInertialSolver solver;
            solver.initialize(m, s, o);
            A::rebuild(solver);
            A::fireSwe(solver, 1e-4);
            const auto& ed = A::edges(solver);
            int checked = 0;
            for (int e : A::faces(solver)) {
                const int a = ed.cL[e], b = ed.cR[e];
                auto interior = [&](int c) {
                    return m.tri_cx[c] > 2 && m.tri_cx[c] < 6 && m.tri_cy[c] > 2 && m.tri_cy[c] < 6;
                };
                if (!interior(a) || !interior(b))
                    continue;
                // Both reconstructed states have h=.5 and the same velocity.
                // A smooth bed slope must not lower the advected water depth.
                EXPECT_NEAR(A::discharge(solver, e), .5 * (.2 * ed.nx[e] - .1 * ed.ny[e]), 1e-13);
                ++checked;
            }
            EXPECT_GT(checked, 10);
        }
}

TEST(Cpu2DCorrectness, SecondOrderPlanarThackerRetainsRotationPhase) {
    constexpr double pi = 3.14159265358979323846;
    const double omega = std::sqrt(2 * inertial::kGravity * .1);
    const double period = 2 * pi / omega;
    for (bool quad : {false, true}) {
        auto m = grid(64, quad);
        for (int i = 0; i < m.n_vertices(); ++i) {
            m.vx[i] /= 16;
            m.vy[i] /= 16;
            const double x = m.vx[i] - 2, y = m.vy[i] - 2;
            m.vz[i] = .1 * (x * x + y * y - 1);
        }
        buildMeshTopology(m);
        auto s = state(m, 0);
        auto o = options(1, 1);
        o.momentum = Momentum2D::FULL_SWE;
        o.reconstruction_order = 2;
        o.max_timestep = .25;
        o.dry_depth = 1e-7;
        o.h_move = 1e-6;
        for (int i = 0; i < m.n_cells(); ++i) {
            const double x = m.tri_cx[i] - 2.5, y = m.tri_cy[i] - 2;
            s.volume[i] = std::max(0.0, .1 * (1 - x * x - y * y)) * m.tri_area[i];
            m.tri_init_v[i] = .5 * omega;
            m.mannings_n[i] = 0;
        }
        const double initial_volume = sum(s.volume);
        ExplicitInertialSolver solver;
        solver.initialize(m, s, o);
        double max_speed = 0;
        for (int k = 0; k < 192; ++k) {
            solver.advance(k * period / 64, (k + 1) * period / 64);
            for (int i = 0; i < m.n_cells(); ++i) {
                const double speed = ExplicitInertialSolverTestAccess::speed(solver, i);
                ASSERT_TRUE(std::isfinite(speed));
                max_speed = std::max(max_speed, speed);
            }
        }
        // The analytical velocity is uniform, with magnitude .5*omega.
        // Allow shoreline truncation error, but catch thin-film acceleration
        // even when total volume, centroid phase and depth error look good.
        EXPECT_LT(max_speed, 5 * .5 * omega) << "quad=" << quad;
        double x = 0, y = 0, depth_error = 0, reference_volume = 0;
        for (int i = 0; i < m.n_cells(); ++i) {
            x += s.volume[i] * (m.tri_cx[i] - 2);
            y += s.volume[i] * (m.tri_cy[i] - 2);
            EXPECT_GE(s.volume[i], 0);
            const double dx = m.tri_cx[i] - 2.5, dy = m.tri_cy[i] - 2;
            const double exact_h = std::max(0.0, .1 * (1 - dx * dx - dy * dy));
            depth_error += std::abs(s.depth[i] - exact_h) * m.tri_area[i];
            reference_volume += exact_h * m.tri_area[i];
        }
        EXPECT_NEAR(sum(s.volume), initial_volume, 1e-12);
        // Exact centroid returns to (+.5, 0). Allow spatial damping while
        // rejecting the former ~30-degree phase lag after three periods.
        EXPECT_GT(x / initial_volume, .3);
        EXPECT_LT(std::abs(std::atan2(y, x)), .15);
        EXPECT_LT(depth_error / reference_volume, .18);
    }
}

TEST(Cpu2DCorrectness, SecondOrderDistortedThackerKeepsShorelineVelocityBounded) {
    constexpr int n = 32;
    constexpr double dx = 4.0 / n, pi = 3.14159265358979323846;
    const double omega = std::sqrt(8 * inertial::kGravity * .1);
    const double period = 2 * pi / omega, amplitude = (1 - .8 * .8) / (1 + .8 * .8);
    const double root = std::sqrt(1 - amplitude * amplitude);
    // Maximize the analytical radial speed over the wet disk and a period.
    const double c = (1 - root) / amplitude;
    const double exact_max_speed =
        .5 * omega * amplitude * std::sqrt((1 - c * c) / ((1 - amplitude * c) * root));
    for (int shape : {0, 1, 2}) { // triangles, quadrilaterals, mixed cells
        SCOPED_TRACE(shape);
        MeshData m;
        m.resize_vertices((n + 1) * (n + 1));
        for (int y = 0; y <= n; ++y)
            for (int x = 0; x <= n; ++x) {
                const int i = y * (n + 1) + x;
                const bool inner = x > 0 && x < n && y > 0 && y < n;
                m.vx[i] = x * dx + (inner ? .15 * dx * std::sin(.7 * x + 1.1 * y) : 0);
                m.vy[i] = y * dx + (inner ? .15 * dx * std::cos(1.3 * x - .4 * y) : 0);
                const double xc = m.vx[i] - 2, yc = m.vy[i] - 2;
                m.vz[i] = .1 * (xc * xc + yc * yc - 1);
            }
        int nc = 0;
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
                nc += (shape == 1 || (shape == 2 && (x + y) % 2)) ? 1 : 2;
        m.resize_triangles(nc);
        int cell = 0;
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const int a = y * (n + 1) + x, b = a + 1, c = a + n + 1, d = c + 1;
                if (shape == 1 || (shape == 2 && (x + y) % 2))
                    m.set_quad(cell++, a, b, d, c);
                else {
                    m.set_triangle(cell++, a, b, d);
                    m.set_triangle(cell++, a, d, c);
                }
            }
        buildMeshTopology(m);
        ASSERT_TRUE(validateMesh(m).empty());
        auto s = state(m, 0);
        auto o = options(1, 1);
        o.momentum = Momentum2D::FULL_SWE;
        o.reconstruction_order = 2;
        o.max_timestep = .25;
        o.dry_depth = 1e-7;
        o.h_move = 1e-6;
        const double scale = root / (1 - amplitude);
        for (int i = 0; i < m.n_cells(); ++i) {
            const double x = m.tri_cx[i] - 2, y = m.tri_cy[i] - 2;
            s.volume[i] =
                std::max(0.0, .1 * (scale - (x * x + y * y) * scale * scale)) * m.tri_area[i];
            m.mannings_n[i] = 0;
        }
        const double initial_volume = sum(s.volume);
        ExplicitInertialSolver solver;
        solver.initialize(m, s, o);
        double max_speed = 0;
        for (int k = 0; k < 192; ++k) {
            solver.advance(k * period / 64, (k + 1) * period / 64);
            for (int i = 0; i < m.n_cells(); ++i) {
                const double speed = ExplicitInertialSolverTestAccess::speed(solver, i);
                ASSERT_TRUE(std::isfinite(speed));
                ASSERT_GE(s.volume[i], 0);
                max_speed = std::max(max_speed, speed);
            }
        }
        EXPECT_NEAR(sum(s.volume), initial_volume, 1e-12);
        // Generous truncation allowance: the rejected shoreline fit reached
        // 18–52 m/s here despite good depth error and exact conservation.
        EXPECT_LT(max_speed, 5 * exact_max_speed);
    }
}

TEST(Cpu2DCorrectness, ReconstructedBedPressureUsesTheCellToFaceDepthIntegral) {
    // For a linear profile, the bed-source integral is -g*mean(h)*delta(z).
    // Both sides agree at the face, so the hydrostatic correction is zero.
    swe::FaceFlux f;
    double lx, ly, rx, ry;
    const double h_cell = 2, h_face = 2.2, z_face = .1, eta_face = h_face + z_face;
    ASSERT_TRUE(swe::faceFluxReconBed(eta_face, 0, 0, 0, h_cell, eta_face, 0, 0, z_face, h_face, 1,
                                      0, 1e-7, f, lx, ly, rx, ry, z_face, z_face));
    EXPECT_NEAR(f.mass, 0, 1e-14);
    EXPECT_NEAR(lx, -inertial::kGravity * .5 * (h_cell + h_face) * z_face, 1e-14);
    EXPECT_EQ(ly, 0);
    EXPECT_NEAR(rx, 0, 1e-14);
    EXPECT_EQ(ry, 0);
    // A bounded positive depth polynomial cannot produce a bed-force per
    // unit water volume that diverges as the cell dries.
    for (double h : {1e-4, 1e-6, 1e-8}) {
        ASSERT_TRUE(swe::faceFluxReconBed(.01 + 2 * h, 0, 0, 0, h, .1, 0, 0, .1, 0, 1, 0, 1e-10, f,
                                          lx, ly, rx, ry, .01, .1));
        EXPECT_EQ(f.mass, 0);
        EXPECT_EQ(f.mx, 0);
        EXPECT_LT(std::abs(lx) / h, .02 * inertial::kGravity);
    }
}

TEST(Cpu2DCorrectness, SecondOrderShorelineKeepsConnectedWetFaceAccuracy) {
    using A = ExplicitInertialSolverTestAccess;
    for (bool quad : {false, true})
        for (int threads : {1, 4}) {
            auto m = grid(8, quad);
            for (int i = 0; i < m.n_vertices(); ++i)
                m.vz[i] = .04 * m.vx[i] + .03 * m.vy[i];
            buildMeshTopology(m);
            auto s = state(m, 0);
            auto o = options(threads, 1);
            o.momentum = Momentum2D::FULL_SWE;
            o.reconstruction_order = 2;
            for (int i = 0; i < m.n_cells(); ++i) {
                s.volume[i] = m.tri_cx[i] < 4 ? .5 * m.tri_area[i] : 0;
                m.tri_init_u[i] = .2;
                m.tri_init_v[i] = -.1;
                m.mannings_n[i] = 0;
            }
            ExplicitInertialSolver solver;
            solver.initialize(m, s, o);
            A::rebuild(solver);
            A::fireSwe(solver, 1e-4);
            const auto& ed = A::edges(solver);
            int checked = 0;
            auto shore = [&](int c) {
                for (int p = ed.cell_ptr[c]; p < ed.cell_ptr[c + 1]; ++p) {
                    const int e = ed.cell_edge[p], j = ed.cL[e] == c ? ed.cR[e] : ed.cL[e];
                    if (s.depth[j] == 0)
                        return true;
                }
                return false;
            };
            auto interior = [&](int c) {
                return m.tri_cx[c] > 1 && m.tri_cx[c] < 7 && m.tri_cy[c] > 1 && m.tri_cy[c] < 7;
            };
            for (int e : A::faces(solver)) {
                const int a = ed.cL[e], b = ed.cR[e];
                if (!interior(a) || !interior(b) || s.depth[a] == 0 || s.depth[b] == 0)
                    continue;
                if (!shore(a) && !shore(b))
                    continue;
                // The dry neighbor must not suppress a linear, fully wet face
                // reconstruction when two independent wet directions remain.
                EXPECT_NEAR(A::discharge(solver, e), .5 * (.2 * ed.nx[e] - .1 * ed.ny[e]), 1e-13);
                ++checked;
            }
            EXPECT_GT(checked, 3);
        }
}

// Shared fixture definitions are embedded verbatim in the normal unit source.
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include "core/CompletedSourceArchive.hpp"
#include "2d/heat/MeshCheckpointState.hpp"

namespace face_accum_fixture {
using Archive = openswmm::detail::CompletedSourceArchive;
using Rows = std::array<std::vector<double>, 6>;
using Access = openswmm::twoD::ExplicitInertialSolverTestAccess;
using namespace openswmm::twoD;

inline void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
inline std::string encodeRows(Rows rows) {
    Archive a;
    for (auto& row : rows) a.fixed(row);
    return a.bytes();
}
inline Rows unpack(const std::vector<double>& packed, std::size_t n, std::size_t stride) {
    Rows rows;
    for (std::size_t side = 0; side < stride; ++side)
        for (std::size_t e = 0; e < n; ++e)
            rows[side].push_back(packed[e * stride + side]);
    return rows;
}
inline void pack(const Rows& rows, std::vector<double>& packed, std::size_t stride) {
    for (std::size_t side = 0; side < stride; ++side)
        for (std::size_t e = 0; e < rows[side].size(); ++e)
            packed[e * stride + side] = rows[side][e];
}
template <class A>
void packedFields(A& a, std::vector<double>& packed, std::size_t n, std::size_t stride) {
    if constexpr (requires { a.fixedStrided(packed, n, std::size_t(0), stride); }) {
        for (std::size_t side = 0; side < 6; ++side)
            a.fixedStrided(packed, side < stride ? n : 0, side, stride);
    } else {
        // The baseline owns separate vectors. Preserve partial decode on a later exception.
        auto rows = unpack(packed, n, stride);
        try { for (auto& row : rows) a.fixed(row); }
        catch (...) { pack(rows, packed, stride); throw; }
        pack(rows, packed, stride);
    }
}
inline void consumeBudget(Archive& a) {
    // No-element visitor charges 64MiB using only 8KiB of count headers.
    std::vector<double> budget(8192);
    for (int i = 0; i < 1024; ++i)
        a.sequence(budget, [](double&) {});
}
inline std::string budgetPrefix() {
    Archive a;
    consumeBudget(a);
    return a.bytes();
}
inline void overwriteCount(std::string& bytes, std::size_t offset, uint64_t value) {
    require(offset + sizeof(value) <= bytes.size(), "fixture count offset");
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}
struct CodecResult { std::string records; std::size_t cases = 0; };
inline CodecResult codecHistory() {
    Archive history;
    std::string magic = "L1_FACE_CODEC_V1";
    history(magic);
    CodecResult result;
    for (std::size_t stride : {2, 6})
        for (std::size_t n : {0, 1, 8, 33, 257}) {
            Rows source;
            const uint64_t bits[] = {0, uint64_t(1) << 63, 0x3ff123456789abcdULL,
                0xbfc23456789abcdeULL, 0x7ff8000000000042ULL, 0x7ff0000000000000ULL,
                0xfff0000000000000ULL, 1};
            for (std::size_t side = 0; side < stride; ++side)
                for (std::size_t e = 0; e < n; ++e)
                    source[side].push_back(std::bit_cast<double>(bits[(side + e) % 8]));
            const auto wire = encodeRows(source);
            std::vector<double> packed(n * stride);
            pack(source, packed, stride);
            Archive writer;
            packedFields(writer, packed, n, stride);
            require(writer.bytes() == wire, "logical wire differs");
            std::array<std::size_t, 6> offsets{};
            std::size_t offset = 0;
            for (std::size_t side = 0; side < 6; ++side) {
                offsets[side] = offset;
                offset += sizeof(uint64_t) + source[side].size() * sizeof(double);
            }
            std::vector<std::pair<std::string, std::string>> inputs = {
                {"valid", wire}, {"empty", ""}, {"short-count", wire.substr(0, 7)},
                {"last-byte", wire.substr(0, wire.size() - 1)},
                {"mid-field", wire.substr(0, 8 + (n ? 3 : 0))},
                {"later-count", wire.substr(0, offsets[3] + 3)}, {"trailing", wire + 'x'}};
            for (std::size_t side : {0, 3, 5}) {
                auto bad = wire;
                overwriteCount(bad, offsets[side], source[side].size() + 1);
                inputs.emplace_back("larger-" + std::to_string(side), bad);
                if (!source[side].empty()) {
                    overwriteCount(bad, offsets[side], source[side].size() - 1);
                    inputs.emplace_back("smaller-" + std::to_string(side), bad);
                }
            }
            auto oversized = wire;
            overwriteCount(oversized, 0, 64 * 1024 * 1024 / 8 + 1);
            inputs.emplace_back("oversized-budget", oversized);
            // Padding lets each no-element sequence pass its original count<=remaining test.
            inputs.emplace_back("cumulative-budget", budgetPrefix() + wire + std::string(8192, '\0'));
            for (auto [label, input] : inputs) {
                Rows legacy = source;
                for (auto& row : legacy) for (double& v : row) v = -1234.5;
                std::vector<double> actual(n * stride + 2, -1234.5);
                actual[n * stride] = -0.0; actual[n * stride + 1] = 0x1.1234p+30;
                auto guards = std::string(reinterpret_cast<const char*>(actual.data() + n * stride), 16);
                Archive oldReader(input), newReader(input);
                std::string oldError, newError;
                const bool budget = label == "cumulative-budget";
                try {
                    if (budget) consumeBudget(oldReader);
                    for (auto& row : legacy) oldReader.fixed(row);
                    oldReader.finish();
                } catch (const std::exception& e) { oldError = e.what(); }
                try {
                    if (budget) consumeBudget(newReader);
                    packedFields(newReader, actual, n, stride);
                    newReader.finish();
                } catch (const std::exception& e) { newError = e.what(); }
                require(oldError == newError, "codec diagnostic differs: " + label);
                auto decoded = unpack(actual, n, stride);
                auto logical = encodeRows(decoded);
                require(logical == encodeRows(legacy), "codec partial-prefix differs: " + label);
                require(std::string(reinterpret_cast<const char*>(actual.data() + n * stride), 16) == guards,
                        "untouched codec guard differs");
                require((label == "valid") == oldError.empty(), "codec expected refusal differs");
                history(stride, n, label, input, oldError, logical, guards);
                ++result.cases;
            }
            // Write-budget behavior must match too, including already-written count headers.
            Archive oldWriter, newWriter;
            std::string oldError, newError;
            try { consumeBudget(oldWriter); for (auto& row : source) oldWriter.fixed(row); }
            catch (const std::exception& e) { oldError = e.what(); }
            try { consumeBudget(newWriter); packedFields(newWriter, packed, n, stride); }
            catch (const std::exception& e) { newError = e.what(); }
            require(oldError == newError && oldWriter.bytes() == newWriter.bytes(), "write-budget differs");
            auto written = oldWriter.bytes();
            history(stride, n, oldError, written);
        }
    history(result.cases);
    result.records = history.bytes();
    return result;
}

inline MeshData faceGrid(int n, bool quad) {
    MeshData m;
    m.resize_vertices((n + 1) * (n + 1));
    for (int y = 0; y <= n; ++y) for (int x = 0; x <= n; ++x) {
        const int v = y * (n + 1) + x;
        m.vx[v] = x; m.vy[v] = y;
        m.vz[v] = 0.001 * x + 0.0007 * y;
    }
    m.resize_triangles((quad ? 1 : 2) * n * n);
    int c = 0;
    for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x) {
        int a = y * (n + 1) + x, b = a + 1, d = a + n + 1, e = d + 1;
        if (quad) m.set_quad(c++, a, b, e, d);
        else { m.set_triangle(c++, a, b, e); m.set_triangle(c++, a, e, d); }
    }
    buildMeshTopology(m);
    return m;
}
inline SurfaceStateData faceState(const MeshData& m) {
    SurfaceStateData state;
    state.resize(m.n_cells(), m.n_vertices());
    for (int i = 0; i < m.n_cells(); ++i)
        state.volume[i] = (0.2 + (i % 11) * .003) * m.tri_area[i];
    return state;
}
inline SolverOptions2D faceOptions(Momentum2D mode, int threads) {
    SolverOptions2D options;
    options.num_threads = threads; options.momentum = mode;
    options.max_timestep = .005; options.lts_tiers = 4; options.cfl_number = .4;
    return options;
}
struct EngineResult { std::string records; std::size_t contexts = 0, stages = 0; };
inline EngineResult engineHistory() {
    Archive header;
    std::string magic = "L1_FACE_ENGINE_V1";
    header(magic);
    EngineResult result;
    result.records = header.bytes();
    auto append = [&](std::string bytes) {
        Archive frame;
        frame(bytes);
        result.records += frame.bytes();
    };
    ExplicitInertialSolver solver;
    for (int threads : {1, 4, 8}) for (bool quad : {false, true})
        for (int n : {1, 8, 2, 33, 1})
            for (auto mode : {Momentum2D::LOCAL_INERTIAL, Momentum2D::FULL_SWE,
                              Momentum2D::DIFFUSIVE_WAVE}) {
                auto mesh = faceGrid(n, quad);
                auto state = faceState(mesh);
                auto options = faceOptions(mode, threads);
                if (n == 33) {
                    options.max_timestep = 1;
                    for (int i = 0; i < mesh.n_cells(); ++i)
                        state.volume[i] = (mesh.tri_cx[i] < 4 ? .3 : .0041) * mesh.tri_area[i];
                }
                solver.initialize(mesh, state, options);
                ++result.contexts;
                auto capture = [&](std::string label, ExplicitInertialSolver& owner,
                                   SurfaceStateData& canonical, std::string refusal = {}) {
                    auto rows = Access::packetRows(owner);
                    Archive surface;
                    checkpoint::meshState(surface, canonical);
                    auto [wire, error] = Access::livePackets(owner) ? Access::packetArchive(owner) :
                        std::pair<std::string, std::string>{};
                    auto logical = encodeRows(rows);
                    auto canonicalBytes = surface.bytes();
                    Archive record;
                    uint8_t kind = 0;
                    record(kind, threads, quad, n, mode, label, logical, canonicalBytes,
                           wire, error, refusal);
                    append(record.bytes());
                    ++result.stages;
                };
                capture("initialize", solver, state);
                Access::rebuild(solver);
                capture("prepared", solver, state);
                Access::seedPackets(solver);
                capture("six-lane-seed", solver, state);
                const auto untouched = Access::packetArchive(solver);
                {
                    auto trial = solver;
                    auto trialState = state;
                    Access::rebindFixtureState(trial, trialState);
                    Access::settle(trial);
                    capture("copy-settle", trial, trialState);
                }
                require(Access::packetArchive(solver) == untouched, "copy changed original packets");
                Access::settle(solver);
                capture("settle", solver, state);
                Access::firePackets(solver, .0001);
                Access::firePackets(solver, .0002);
                capture("two-firings-pending", solver, state);
                Access::rebuild(solver);
                capture("rebuild-settles", solver, state);
                double firstEnd = .02;
                const auto before = solver.run_stats();
                long expectedFaces = 0;
                int expectedSubsteps = 0;
                if (n == 33) {
                    require(Access::hasTierMix(solver), "fixture has no heterogeneous tiers");
                    Access::refresh(solver);
                    firstEnd = Access::macroSpan(solver);
                    expectedFaces = Access::macroFaceVisits(solver);
                    expectedSubsteps = Access::macroSubsteps(solver);
                }
                solver.advance(0, firstEnd);
                if (n == 33) {
                    const auto after = solver.run_stats();
                    require(after.nsteps - before.nsteps == expectedSubsteps,
                            "fixture did not run exactly one full macrocycle");
                    require(after.nrhs - before.nrhs == expectedFaces,
                            "fixture face visits do not follow heterogeneous tier cadence");
                }
                capture("first-advance", solver, state);
                const double end = firstEnd + .021;
                solver.advance(firstEnd, end);
                capture("second-advance", solver, state);
                const auto clean = Access::packetArchive(solver);
                require(clean.second.empty(), "accepted internal state archive refused: " + clean.second);
                {
                    auto trial = solver;
                    auto trialState = state;
                    Access::rebindFixtureState(trial, trialState);
                    auto truncated = clean.first.substr(0, clean.first.size() - 1);
                    auto refused = Access::readPacketArchive(trial, truncated);
                    require(!refused.second.empty(), "truncated internal state accepted");
                    capture("private-refusal-trial", trial, trialState, refused.second);
                    auto restored = Access::readPacketArchive(trial, clean.first);
                    require(restored.second.empty(), "exact private restore refused");
                    require(Access::packetArchive(trial) == clean, "exact private restore differs");
                    capture("private-restore", trial, trialState);
                }
                require(Access::packetArchive(solver) == clean, "private refusal changed original");
                Access::seedPackets(solver, true);
                capture("nonfinite-refusal", solver, state);
                if (Access::edges(solver).ne > 0)
                    require(Access::packetArchive(solver).second == "Nonfinite CPU checkpoint state.",
                            "nonfinite packet diagnostic differs");
                solver.reinitialize(end);
                capture("reinitialize-drops", solver, state);
                std::fill(state.volume.begin(), state.volume.end(), 0.0);
                solver.reinitialize(end); // authoritative external volume edit reconstructs depths
                Access::seedPackets(solver);
                Access::rebuildAt(solver, end);
                require(Access::allCellsInactive(solver), "all-dry fixture retained active cells");
                for (const auto& row : Access::packetRows(solver))
                    for (double value : row) require(value == 0, "deactivation stranded a packet");
                capture("deactivate-rebuild", solver, state);
                Access::seedPackets(solver);
                solver.finalize();
                capture("finalize-settles-clears", solver, state);
                for (const auto& row : Access::packetRows(solver))
                    require(row.empty(), "finalize retained packets");
            }
    Archive footer;
    uint8_t kind = 1;
    footer(kind, result.contexts, result.stages);
    append(footer.bytes());
    return result;
}
inline void saveRecords(const std::filesystem::path& path, const std::string& bytes) {
    std::filesystem::create_directories(path.parent_path());
    require(!std::filesystem::exists(path), "refuse existing fixture output");
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    require(bool(out), "fixture output failed");
}
} // namespace face_accum_fixture

TEST(Cpu2DCorrectness, FaceAccumulatorLogicalWireAndPartialFailures) {
    const auto result = face_accum_fixture::codecHistory();
    EXPECT_GT(result.cases, 100u);
    if (const char* dir = std::getenv("OPENSWMM_FACE_ACCUM_TEST_OUT"))
        face_accum_fixture::saveRecords(std::filesystem::path(dir) / "codec.bin", result.records);
}
TEST(Cpu2DCorrectness, FaceAccumulatorSixLaneLifecycleHistory) {
    const auto result = face_accum_fixture::engineHistory();
    EXPECT_EQ(result.contexts, 90u);
    EXPECT_EQ(result.stages, 90u * 15u);
    if (const char* dir = std::getenv("OPENSWMM_FACE_ACCUM_TEST_OUT"))
        face_accum_fixture::saveRecords(std::filesystem::path(dir) / "engine.bin", result.records);
}

namespace ordered_lists_fixture {
using Access = openswmm::twoD::ExplicitInertialSolverTestAccess;
void seed(const MeshData& mesh, SurfaceStateData& state, int stage) {
    for (int i = 0; i < mesh.n_cells(); ++i) {
        const bool wet = stage % 4 == 0 || stage % 4 == 3 ||
                         (stage % 4 == 1 && mesh.tri_cx[i] < 2);
        state.volume[i] = wet ? (.01 + (i % 5) * .02) * mesh.tri_area[i] : 0.0;
    }
}
void capture(std::ostream& output, const std::string& bytes) {
    const std::uint64_t size = bytes.size();
    output.write(reinterpret_cast<const char*>(&size), sizeof(size));
    output.write(bytes.data(), bytes.size());
    if (!output)
        throw std::runtime_error("ordered-list fixture output failed");
}
void history(std::ostream* output) {
    for (bool quad : {false, true})
        for (bool front : {false, true})
            for (int threads : {1, 4, 8}) {
                auto mesh = grid(quad ? 256 : 192, quad);
                auto water = state(mesh, .1);
                auto opts = options(threads, 4);
                opts.front_rebuild = front;
                opts.momentum = front ? Momentum2D::FULL_SWE : Momentum2D::LOCAL_INERTIAL;
                ExplicitInertialSolver solver;
                solver.initialize(mesh, water, opts);
                for (int stage = 0; stage < 4; ++stage) {
                    Access::markActiveFaces(solver);
                    seed(mesh, water, stage);
                    // The scheduler seeds from depth; use the production closure resync
                    // after editing volumes. This retains the prior active face momentum.
                    solver.resyncFromVolumes(0.0);
                    Access::rebuildAt(solver, 0.0);
                    int active = 0;
                    for (int i = 0; i < mesh.n_cells(); ++i)
                        active += Access::active(solver, i);
                    if ((stage == 2 && active != 0) ||
                        ((stage == 0 || stage == 3) && active != mesh.n_cells()) ||
                        (stage == 1 && (active <= 0 || active >= mesh.n_cells() / 4)))
                        throw std::runtime_error("fixture activity transition was not exercised");
                    if ((stage == 0 || stage == 3) && !Access::hasTierMix(solver))
                        throw std::runtime_error("fixture did not exercise multiple tiers");
                    if (!Access::orderedLists(solver))
                        throw std::runtime_error("ordered scheduler lists differ from serial oracle");
                    if (output)
                        capture(*output, Access::orderedListsFrame(solver));
                }
            }
}
}

TEST(Cpu2DCorrectness, OrderedCompactionKeepsLargeSchedulerLists) {
    std::ofstream output;
    if (const char* directory = std::getenv("OPENSWMM_ORDERED_LISTS_TEST_OUT")) {
        std::filesystem::create_directories(directory);
        output.open(std::filesystem::path(directory) / "history.bin", std::ios::binary);
        ASSERT_TRUE(output);
    }
    ASSERT_NO_THROW(ordered_lists_fixture::history(output.is_open() ? &output : nullptr));
}
