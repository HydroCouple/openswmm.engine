// SPDX-License-Identifier: Apache-2.0

/**
 * @file      test_kokkos_explicit_alignment.cpp
 * @brief     Tests for kokkos explicit alignment.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "2d/coupling/NodeCoupling.hpp"
#include "2d/data/BoundaryData.hpp"
#include "data/NodeData.hpp"
#include "2d/data/SolverOptions2D.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include "2d/gpu/ExplicitKokkosSurfaceSolver.hpp"
#include "2d/gpu/KokkosSweKernels.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include "2d/solver/ExplicitInertialSolver.hpp"
#include "2d/solver/InertialKernels.hpp"
#include <cmath>
#include <iostream>
#include <numeric>
#include <stdexcept>
using namespace openswmm::twoD;
namespace gpu = openswmm::twoD::gpu;
namespace sk = gpu::swe_kernels;
namespace openswmm::twoD {
struct ExplicitInertialSolverTestAccess {
    /**
     * @brief Recompute CPU limited SWE reconstruction gradients for the device comparison.
     *
     * Mutates solver gradient buffers only through the production reconstruction routine. Kokkos
     * initialization is not needed for this CPU accessor.
     *
     * @param[in,out] s Initialized CPU FULL_SWE solver with valid attached mesh/state and gradient
     *     storage; borrowed dependencies remain alive.
     */
    static void gradients(ExplicitInertialSolver& s) { s.computeLimitedGradientsSwe(); }
    /**
     * @brief Fire all currently active CPU SWE faces for one Euler comparison step.
     *
     * Uses the true firing flag and mutates per-face water/momentum accumulators and discharge. Caller
     * manages stage preparation and subsequent cell gathering.
     *
     * @param[in,out] s Initialized FULL_SWE solver with consistent active-face list, state and
     *     reconstruction buffers.
     * @param[in] dt Positive face integration interval in seconds.
     */
    static void faces(ExplicitInertialSolver& s, double dt) {
        s.fireFacesSwe(s.active_faces_, dt, true);
    }
    /**
     * @brief Consume active-cell transfers and apply the CPU cell operation.
     *
     * Uses the true firing flag; updates attached water state, momentum and consumed transfer buffers.
     * Does not schedule a complete model advance.
     *
     * @param[in,out] s Initialized solver whose active-cell list and face-transfer buffers describe the
     *     intended Euler stage.
     * @param[in] dt Positive cell integration interval in seconds.
     */
    static void cells(ExplicitInertialSolver& s, double dt) {
        s.fireCells(s.active_cells_, dt, true);
    }
    /**
     * @brief Expose CPU face topology and metric storage to the kernel fixture.
     *
     * Borrowed storage must not outlive its solver; operations that resize the corresponding container
     * may invalidate element references/iterators.
     *
     * @param[in,out] s Live initialized CPU solver; caller preserves buffer sizes, valid indices and
     *     solver invariants when mutating the returned reference.
     * @return Mutable reference to s.edges_; invalidated by solver destruction or topology/storage
     *     rebuild.
     */
    static auto& edges(ExplicitInertialSolver& s) { return s.edges_; }
    /**
     * @brief Expose the CPU active-cell index list.
     *
     * Borrowed storage must not outlive its solver; operations that resize the corresponding container
     * may invalidate element references/iterators.
     *
     * @param[in,out] s Live initialized CPU solver; caller preserves buffer sizes, valid indices and
     *     solver invariants when mutating the returned reference.
     * @return Mutable reference to the solver-owned cell list; valid until destruction or a
     *     list/storage rebuild.
     */
    static auto& cells(ExplicitInertialSolver& s) { return s.active_cells_; }
    /**
     * @brief Expose the CPU active-face index list.
     *
     * Borrowed storage must not outlive its solver; operations that resize the corresponding container
     * may invalidate element references/iterators.
     *
     * @param[in,out] s Live initialized CPU solver; caller preserves buffer sizes, valid indices and
     *     solver invariants when mutating the returned reference.
     * @return Mutable reference to the solver-owned face list; valid until destruction or a
     *     list/storage rebuild.
     */
    static auto& faceList(ExplicitInertialSolver& s) { return s.active_faces_; }
    /**
     * @brief Expose CPU cell activity flags.
     *
     * Borrowed storage must not outlive its solver; operations that resize the corresponding container
     * may invalidate element references/iterators.
     *
     * @param[in,out] s Live initialized CPU solver; caller preserves buffer sizes, valid indices and
     *     solver invariants when mutating the returned reference.
     * @return Mutable reference to solver-owned activity flags in cell-index order; no copy or
     *     ownership transfer.
     */
    static auto& active(ExplicitInertialSolver& s) { return s.cell_active_; }
    /**
     * @brief Expose CPU local-timestep cell tiers.
     *
     * Borrowed storage must not outlive its solver; operations that resize the corresponding container
     * may invalidate element references/iterators.
     *
     * @param[in,out] s Live initialized CPU solver; caller preserves buffer sizes, valid indices and
     *     solver invariants when mutating the returned reference.
     * @return Mutable reference to solver-owned cell-tier storage; no copy or ownership transfer.
     */
    static auto& tiers(ExplicitInertialSolver& s) { return s.tier_; }
    /**
     * @brief Expose CPU x-directed depth-integrated momentum.
     *
     * Borrowed storage must not outlive its solver; operations that resize the corresponding container
     * may invalidate element references/iterators.
     *
     * @param[in,out] s Live initialized CPU solver; caller preserves buffer sizes, valid indices and
     *     solver invariants when mutating the returned reference.
     * @return Mutable reference to solver-owned per-cell qx in m2/s; no copy or ownership transfer.
     */
    static auto& qx(ExplicitInertialSolver& s) { return s.qcx_; }
    /**
     * @brief Expose CPU y-directed depth-integrated momentum.
     *
     * Borrowed storage must not outlive its solver; operations that resize the corresponding container
     * may invalidate element references/iterators.
     *
     * @param[in,out] s Live initialized CPU solver; caller preserves buffer sizes, valid indices and
     *     solver invariants when mutating the returned reference.
     * @return Mutable reference to solver-owned per-cell qy in m2/s; no copy or ownership transfer.
     */
    static auto& qy(ExplicitInertialSolver& s) { return s.qcy_; }
    /**
     * @brief Pack CPU gradients into the device kernel comparison layout.
     *
     * Does not mutate solver state. Requires each selected eta vector to contain at least
     * state.depth.size() entries; no bounds validation occurs.
     *
     * @param[in] s Initialized CPU solver with attached state and previously computed gradient buffers
     *     covering all depth entries.
     * @param[in] axis Zero selects x gradients; any nonzero value selects y gradients.
     * @return Owned vector containing cell eta gradients, all u gradients, all v gradients, then any
     *     remaining eta entries, in native reconstruction units.
     */
    static std::vector<double> gradients(ExplicitInertialSolver& s, int axis) {
        std::vector<double> v;
        const auto n = s.state_->depth.size();
        auto& eta = axis ? s.gey_ : s.gex_;
        auto& u = axis ? s.guy_ : s.gux_;
        auto& w = axis ? s.gvy_ : s.gvx_;
        v.insert(v.end(), eta.begin(), eta.begin() + n);
        v.insert(v.end(), u.begin(), u.end());
        v.insert(v.end(), w.begin(), w.end());
        v.insert(v.end(), eta.begin() + n, eta.end());
        return v;
    }
    /**
     * @brief Snapshot CPU face water/momentum transfers and discharge for comparison.
     *
     * @param[in] s Initialized CPU solver after the face operation under test.
     * @return Owned copies in order water-left, water-right, x-momentum-left/right,
     *     y-momentum-left/right, discharge; each retains the corresponding production buffer layout and
     *     units.
     */
    static std::vector<std::vector<double>> transfers(ExplicitInertialSolver& s) {
        return {s.facc_L_, s.facc_R_, s.macc_x_L_, s.macc_x_R_, s.macc_y_L_, s.macc_y_R_, s.q_};
    }
};
} // namespace openswmm::twoD
namespace openswmm::twoD::gpu {
struct ExplicitKokkosSurfaceSolverTestAccess {
    /**
     * @brief Upload current boundary/source inputs and rebuild the device firing state at time zero.
     *
     * Calls pushForcings then syncAndRebuild(0.0); intended before manual boundary substeps, rather
     * than as a complete scheduled advance.
     *
     * @param[in,out] s Initialized Kokkos solver with live attached forcing/state dependencies and
     *     initialized Kokkos runtime.
     */
    static void prepareBoundarySteps(ExplicitKokkosSurfaceSolver& s) {
        s.pushForcings();
        s.syncAndRebuild(0.0);
    }
    /**
     * @brief Upload changed boundary values and directly fire device tier zero.
     *
     * Uses fireCells(0,dt,true); mutates device water state and receipt accumulators without
     * publication or full advance scheduling.
     *
     * @param[in,out] s Initialized and prepared Kokkos solver; attached forcing dependencies and
     *     runtime remain valid.
     * @param[in] dt Positive interval in seconds for the manual cell firing.
     */
    static void boundarySubstep(ExplicitKokkosSurfaceSolver& s, double dt) {
        s.pushForcings();
        s.fireCells(0, dt, true);
    }
    /**
     * @brief Publish a manually fired device boundary window to host state.
     *
     * Calls publishAndCopyBack with start time zero. Applies production accumulator-draining semantics;
     * attached host state must remain alive.
     *
     * @param[in,out] s Initialized solver holding the device state/receipt accumulators to publish.
     * @param[in] span Positive reporting interval in seconds used for mean-flux publication.
     */
    static void publish(ExplicitKokkosSurfaceSolver& s, double span) {
        s.publishAndCopyBack(0.0, span);
    }
    /**
     * @brief Read this solver instance's advance performance counter.
     *
     * @param[in] s Live Kokkos solver whose per-instance performance attribution is under test.
     * @return Recorded advance count; this accessor does not enable performance collection or
     *     synchronize device work.
     */
    static long advances(const ExplicitKokkosSurfaceSolver& s) {
        return s.perf_counters_.n_advance;
    }
    /**
     * @brief Prepare and find the first active device cell assigned a coarse time tier.
     *
     * Uploads forcings, rebuilds at time zero and copies active/tier views to host; modifies the
     * prepared solver state before searching.
     *
     * @param[in,out] s Initialized Kokkos solver with valid forcing/state dependencies and runtime.
     * @return First cell index with active flag and tier greater than zero, or -1 if none exists.
     */
    static int coarse(ExplicitKokkosSurfaceSolver& s) {
        s.pushForcings();
        s.syncAndRebuild(0.0);
        auto active = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace{}, s.d_active_);
        auto tiers = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace{}, s.d_tier_);
        for (int i = 0; i < int(tiers.extent(0)); ++i)
            if (active(i) && tiers(i) > 0)
                return i;
        return -1;
    }
    /**
     * @brief Change one device depth and measure the refreshed frozen-tier CFL ratio.
     *
     * Writes the device depth and refreshes dt0; does not restore either. Requires a nonnegative tier
     * small enough for the integer left shift. No index, shift-range or zero-denominator guard is added
     * by this accessor.
     *
     * @param[in,out] s Initialized Kokkos solver with valid device depth/tier storage, mesh metrics,
     *     options and runtime.
     * @param[in] i Valid cell index in every accessed state/metric/tier array.
     * @param[in] depth Replacement water depth in metres producing a positive finite zero-velocity CFL
     *     bound.
     * @return Refreshed base timestep times 2^tier divided by the cell CFL interval; values at most one
     *     satisfy this fixture bound.
     */
    static double checkBound(ExplicitKokkosSurfaceSolver& s, int i, double depth) {
        auto h = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace{}, s.d_depth_);
        h(i) = depth;
        Kokkos::deep_copy(s.d_depth_, h);
        s.refreshDt0();
        auto tiers = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace{}, s.d_tier_);
        const double bound =
            inertial::cellCflDt(s.opts_->cfl_number, s.edges_.cell_lchar[i], depth, 0.0);
        return s.dt0_ * (1 << tiers(i)) / bound;
    }
};
} // namespace openswmm::twoD::gpu
using A = ExplicitInertialSolverTestAccess;
/**
 * @brief Fail the standalone alignment test when a required condition is false.
 *
 * Throws std::runtime_error when yes is false; otherwise has no effect.
 *
 * @param[in] yes Condition that must hold.
 * @param[in] what Non-null diagnostic string, copied into the exception on failure.
 */
void require(bool yes, const char* what) {
    if (!yes)
        throw std::runtime_error(what);
}
/**
 * @brief Check a finite scalar against a reference with a relative-plus-unit-floor tolerance.
 *
 * Throws std::runtime_error if either value is nonfinite or abs(a-b)>tol*(1+abs(b)). The unit floor
 * is literal 1 in the compared quantity's native units; no input mutation occurs.
 *
 * @param[in] a Actual scalar in the same native units as b.
 * @param[in] b Reference scalar; its magnitude scales the tolerance.
 * @param[in] what Non-null failure label written to stderr and copied into the exception.
 * @param[in] tol Nonnegative tolerance multiplier, default 2e-11.
 */
void near(double a, double b, const char* what, double tol = 2e-11) {
    if (!std::isfinite(a) || !std::isfinite(b) || std::abs(a - b) > tol * (1 + std::abs(b))) {
        std::cerr << what << ": " << a << " != " << b << '\n';
        throw std::runtime_error(what);
    }
}
/**
 * @brief Copy a one-dimensional host vector into a newly allocated device-space Kokkos view.
 *
 * Requires initialized Kokkos. Uses a host mirror and deep_copy; does not retain references to the
 * input or mutate it. Allocation/runtime errors propagate.
 *
 * @param[in] data Readable vector whose element type supports assignment between compatible
 *     host/device views; may be empty.
 * @return Owning reference-counted Kokkos::View<T*,gpu::MemSpace> with extent data.size() and
 *     copied values.
 */
template <class T> auto device(const std::vector<T>& data) {
    Kokkos::View<T*, gpu::MemSpace> d("test", data.size());
    auto h = Kokkos::create_mirror_view(d);
    for (size_t i = 0; i < data.size(); ++i)
        h(i) = data[i];
    Kokkos::deep_copy(d, h);
    return d;
}
/**
 * @brief Copy a one-dimensional Kokkos view into an owned standard vector.
 *
 * Uses create_mirror_view_and_copy and does not retain or mutate the input view; runtime/allocation
 * errors propagate.
 *
 * @param[in] d Valid readable rank-one view with initialized Kokkos runtime and copyable
 *     non_const_value_type.
 * @return Owned vector of extent d.extent(0), containing a host mirror copy in index order.
 */
template <class V> auto host(const V& d) {
    auto h = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace{}, d);
    std::vector<typename V::non_const_value_type> v(d.extent(0));
    for (size_t i = 0; i < v.size(); ++i)
        v[i] = h(i);
    return v;
}
/**
 * @brief Compare equally sized scalar vectors using the standalone finite-value assertion.
 *
 * Throws for unequal sizes with the label "size", or for the first failed/nonfinite element through
 * near(); inputs are unchanged.
 *
 * @param[in] a Actual values, in matching index order and units to b.
 * @param[in] b Reference values; may be empty when a is empty.
 * @param[in] what Non-null element failure diagnostic passed to near().
 * @param[in] tol Nonnegative multiplier for abs(a[i]-b[i]) <= tol*(1+abs(b[i])).
 */
void compare(const std::vector<double>& a, const std::vector<double>& b, const char* what,
             double tol = 2e-11) {
    require(a.size() == b.size(), "size");
    for (size_t i = 0; i < a.size(); ++i)
        near(a[i], b[i], what, tol);
}
/**
 * @brief Sum a scalar fixture vector in index order from a zero accumulator.
 *
 * @param[in] v Readable vector in consistent native units; may be empty.
 * @return Double-precision sum in the input units, or zero for an empty vector; nonfinite values
 *     propagate.
 */
double sum(const std::vector<double>& v) {
    return std::accumulate(v.begin(), v.end(), 0.0);
}
/**
 * @brief Build a metre-spaced square topology for CPU/device alignment cases.
 *
 * Leaves unrelated properties at MeshData defaults. Count/index preconditions are not checked here;
 * allocation and topology-builder failures propagate.
 *
 * @param[in] n Positive number of unit-square subdivisions along each axis; derived vertex/cell
 *     counts must fit integer/storage limits.
 * @param[in] shape 1 builds quads; 2 alternates quad and split-square triangle patches; other
 *     values split every square into two triangles.
 * @param[in] skew Perturb interior vertex x/y by bounded sine/cosine offsets of 0.15 m.
 * @param[in] slope Set vertex bed elevation to 0.015*x+0.007*y metres before interior horizontal
 *     perturbations.
 * @return Owned mesh with populated geometry and built adjacency/edge topology.
 */
MeshData mesh(int n, int shape = 0, bool skew = false, bool slope = false) {
    MeshData m;
    m.resize_vertices((n + 1) * (n + 1));
    for (int y = 0; y <= n; ++y)
        for (int x = 0; x <= n; ++x) {
            int v = y * (n + 1) + x;
            m.vx[v] = x;
            m.vy[v] = y;
            if (slope)
                m.vz[v] = .015 * x + .007 * y;
            if (skew && x > 0 && x < n && y > 0 && y < n) {
                m.vx[v] += .15 * std::sin(v);
                m.vy[v] += .15 * std::cos(v);
            }
        }
    int count = 0;
    for (int j = 0; j < n * n; ++j)
        count += shape == 1 || (shape == 2 && j % 2) ? 1 : 2;
    m.resize_triangles(count);
    int c = 0;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            int a = y * (n + 1) + x, b = a + 1, d = a + n + 1, e = d + 1;
            if (shape == 1 || (shape == 2 && (y * n + x) % 2))
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
 * @brief Allocate a surface state and seed uniform area-scaled water volumes.
 *
 * Does not initialize head/depth from volume; subsequent solver initialization performs that
 * closure. Mesh is neither mutated nor retained by this helper.
 *
 * @param[in] m Mesh with consistent cell/vertex counts and valid per-cell area in m2.
 * @param[in] depth Intended nonnegative uniform initial water depth in metres.
 * @return Owned resized state with volume[i]=depth*area[i] in m3; other fields retain resize
 *     defaults.
 */
SurfaceStateData state(const MeshData& m, double depth) {
    SurfaceStateData s;
    s.resize(m.n_cells(), m.n_vertices());
    for (int i = 0; i < m.n_cells(); ++i)
        s.volume[i] = depth * m.tri_area[i];
    return s;
}
/**
 * @brief Create alignment options with a fixed one-second cap and CFL number 0.4.
 *
 * @param[in] tiers Supported positive local-timestep tier count; the helper does not validate it.
 * @return Owned options with lts_tiers=tiers, max_timestep=1 s and num_threads=1; all other fields
 *     retain production defaults.
 */
SolverOptions2D options(int tiers = 1) {
    SolverOptions2D o;
    o.lts_tiers = tiers;
    o.max_timestep = 1;
    o.cfl_number = .4;
    o.num_threads = 1;
    return o;
}
/**
 * @brief Check that a changed device depth still respects the frozen coarse-tier CFL bound.
 *
 * Builds a 16-by-16 triangular depth-contrast fixture with four tiers, locates an active coarse
 * cell and raises its device depth to 0.2 m before checking the refreshed base timestep. Requires
 * initialized Kokkos; failures throw through require().
 */
void frozenTier() {
    auto m = mesh(16);
    auto s = state(m, .0041);
    auto o = options(4);
    for (int i = 0; i < m.n_cells(); ++i)
        if (m.tri_cx[i] < 4)
            s.volume[i] = .3 * m.tri_area[i];
    gpu::ExplicitKokkosSurfaceSolver solver;
    solver.initialize(m, s, o);
    using Access = gpu::ExplicitKokkosSurfaceSolverTestAccess;
    int i = Access::coarse(solver);
    require(i >= 0, "coarse CFL fixture");
    require(Access::checkBound(solver, i, .2) <= 1 + 1e-14, "frozen-tier CFL bound");
}
/**
 * @brief Require direct Kokkos initialization to reject unsupported fixture configurations.
 *
 * Separately tries a quad mesh, FULL_SWE mode and a nonempty transport state, requiring
 * std::invalid_argument each time. Requires initialized Kokkos; unexpected exceptions propagate and
 * missing rejection throws std::runtime_error.
 */
void unsupported() {
    for (int kind = 0; kind < 3; ++kind) {
        auto m = mesh(2, kind == 0 ? 1 : 0);
        auto s = state(m, .1);
        auto o = options();
        if (kind == 1)
            o.momentum = Momentum2D::FULL_SWE;
        if (kind == 2)
            s.transport.resize(1, m.n_cells(), 0);
        bool rejected = false;
        try {
            gpu::ExplicitKokkosSurfaceSolver solver;
            solver.initialize(m, s, o);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "unsupported direct Kokkos initialization");
    }
}
/**
 * @brief Compare complete CPU/Kokkos inertial advances across wetting and source cases.
 *
 * Exercises FLAT/VFR closures, one/four tiers and five
 * dam-break/rain/infiltration/evaporation/coupling scenarios over three windows ending at 2 s.
 * Checks water, applied sources and selected conservation budgets at relative-plus-floor tolerance
 * 2e-9. Requires initialized Kokkos; emits step/PASS progress and throws at the first mismatch.
 */
void marcher() {
    for (bool vfr : {false, true})
        for (int tiers : {1, 4})
            for (int scenario = 0; scenario < 5; ++scenario) {
                auto m = mesh(12, 0, false, vfr);
                auto s = state(m, .1);
                auto o = options(tiers);
                if (vfr)
                    o.cell_closure = CellClosure2D::VFR;
                for (int i = 0; i < m.n_cells(); ++i) {
                    if (scenario == 0)
                        s.volume[i] = (m.tri_cx[i] < 6 ? .2 : 0) * m.tri_area[i];
                    if (scenario == 1) {
                        s.infil_rate[i] = .15;
                        s.evap_rate[i] = .1;
                        s.coupling_flux[i] = -.2;
                    }
                    if (scenario == 2) {
                        s.volume[i] = 0;
                        s.rainfall[i] = .02;
                    }
                    if (scenario == 3) {
                        s.rainfall[i] = .1;
                        s.infil_rate[i] = .1;
                    }
                    if (scenario == 4) {
                        s.volume[i] = (m.tri_cx[i] < 6 ? .2 : 0) * m.tri_area[i];
                        s.infil_rate[i] = .2;
                    }
                }
                auto ref = s;
                auto ro = o;
                ExplicitInertialSolver cpu;
                gpu::ExplicitKokkosSurfaceSolver kk;
                cpu.initialize(m, ref, ro);
                kk.initialize(m, s, o);
                double t = 0;
                for (double end : {.93282250995272253, 1.8656450199054451, 2.0}) {
                    cpu.advance(t, end);
                    kk.advance(t, end);
                    std::cout << "step tiers=" << tiers << " case=" << scenario << " end=" << end
                              << " cpu=" << cpu.last_num_steps() << " kk=" << kk.last_num_steps()
                              << std::endl;
                    compare(s.volume, ref.volume, "marcher volume", 2e-9);
                    compare(s.infil_applied, ref.infil_applied, "infiltration", 2e-9);
                    compare(s.coupling_applied, ref.coupling_applied, "coupling", 2e-9);
                    near(s.evap_loss_total, ref.evap_loss_total, "evaporation", 2e-9);
                    if (scenario == 0)
                        near(sum(s.volume), .2 * 72, "closed volume");
                    if (scenario == 1) {
                        double infiltrated = 0;
                        for (int i = 0; i < m.n_cells(); ++i)
                            infiltrated += s.infil_applied[i] * m.tri_area[i];
                        near(sum(s.volume) + infiltrated + s.evap_loss_total -
                                 sum(s.coupling_applied),
                             14.4, "source budget conservation");
                    }
                    if (scenario == 2)
                        near(sum(s.volume), .02 * 144 * end, "rain storage");
                    t = end;
                }
                std::cout << "marcher tiers=" << tiers << " scenario=" << scenario << " PASS\n";
            }
}
// All non-wall laws, in both directions and closures. Boundary values are
// resolved by the router before advance; rating curves arrive as held flows.
/**
 * @brief Compare open-boundary CPU/device storage, fluxes and gross receipts in both directions.
 *
 * Tests normal flow, specified stage/flow and router-resolved held rating-curve flows with both
 * closures and tier counts over three 0.37 s windows. Verifies published receipt arrays,
 * conservation and that the device plugin leaves the engine receipt map empty. Requires initialized
 * Kokkos; prints PASS or propagates an assertion/runtime failure.
 */
void boundaries() {
    for (bool vfr : {false, true})
        for (int tiers : {1, 4})
            for (auto type : {BoundaryType::NORMAL_FLOW, BoundaryType::SPECIFIED_STAGE,
                              BoundaryType::SPECIFIED_FLOW, BoundaryType::RATING_CURVE}) {
                auto m = mesh(3, 0, false, vfr);
                auto a = state(m, .2), b = a;
                auto o = options(tiers);
                if (vfr)
                    o.cell_closure = CellClosure2D::VFR;
                BoundaryData ba;
                ba.resize(m.n_cells() * kMaxCellVerts);
                std::vector<int> slots;
                for (int i = 0; i < m.n_cells(); ++i)
                    for (int e = 0; e < m.cell_vertex_count(i); ++e)
                        if (m.cell_neighbour(i, e) < 0) {
                            const int slot = MeshData::slot(i, e);
                            slots.push_back(slot);
                            ba.edge_bc_type[slot] = static_cast<int8_t>(type);
                        }
                auto bb = ba;
                a.boundary = &ba;
                b.boundary = &bb;
                ExplicitInertialSolver cpu;
                gpu::ExplicitKokkosSurfaceSolver kk;
                cpu.initialize(m, a, o);
                kk.initialize(m, b, o);
                const double initial = sum(a.volume);
                double t = 0;
                for (int phase = 0; phase < 3; ++phase) {
                    const double sign = phase == 1 ? 1.0 : -1.0;
                    for (int slot : slots) {
                        ba.edge_bed_slope[slot] = bb.edge_bed_slope[slot] = sign * .001;
                        ba.edge_bc_flow[slot] = bb.edge_bc_flow[slot] = sign * .03;
                        ba.edge_bc_head[slot] = bb.edge_bc_head[slot] = phase == 1 ? 0.0 : .6;
                    }
                    const double end = t + .37;
                    cpu.advance(t, end);
                    kk.advance(t, end);
                    compare(b.volume, a.volume, "boundary storage", 2e-9);
                    compare(b.edge_flux, a.edge_flux, "boundary flux", 2e-9);
                    double incoming = 0, outgoing = 0;
                    require(b.boundary_receipt_slots == slots, "boundary receipt slots");
                    for (std::size_t k = 0; k < slots.size(); ++k) {
                        const auto& r = ba.receipts.at(slots[k]);
                        near(b.boundary_in_applied[k], r.water_in, "gross boundary in", 2e-9);
                        near(b.boundary_out_applied[k], r.water_out, "gross boundary out", 2e-9);
                        near(b.boundary_requested[k], r.requested, "boundary requested", 2e-9);
                        near(b.boundary_last_flow[k], r.last_flow, "boundary last flow", 2e-9);
                        near(b.boundary_last_dt[k], r.last_dt, "boundary last dt", 2e-9);
                        incoming += r.water_in;
                        outgoing += r.water_out;
                    }
                    near(sum(b.volume) + outgoing - incoming, initial, "boundary conservation");
                    if (phase > 0)
                        require(incoming > 0 && outgoing > 0, "both boundary directions exercised");
                    t = end;
                }
                require(bb.receipts.empty(), "plugin must not mutate engine receipt map");
            }
    std::cout << "Open boundary laws and gross receipts PASS\n";
}

// Reverse a prescribed flow between two cell firings before one publication.
// The mean flux is zero, but both gross transfers must survive. Then publish
// again without a firing to prove the device accumulators were drained.
/**
 * @brief Check gross boundary receipt preservation through reversal, publication and dry-out.
 *
 * Manually fires equal inward/outward prescribed flows for 0.1 s each before publishing a 0.2 s
 * window, checks zero mean flux with nonzero gross receipts, then republishes without double
 * counting. A large outflow request must drain only available water and retain the rejected-request
 * distinction. Requires initialized Kokkos; prints PASS or throws.
 */
void boundaryReversal() {
    auto m = mesh(1);
    auto s = state(m, .2);
    auto o = options();
    BoundaryData bc;
    bc.resize(m.n_cells() * kMaxCellVerts);
    int slot = -1;
    for (int e = 0; e < 3; ++e)
        if (m.cell_neighbour(0, e) < 0) {
            slot = MeshData::slot(0, e);
            break;
        }
    require(slot >= 0, "boundary reversal fixture");
    bc.edge_bc_type[slot] = static_cast<int8_t>(BoundaryType::SPECIFIED_FLOW);
    s.boundary = &bc;
    gpu::ExplicitKokkosSurfaceSolver kk;
    kk.initialize(m, s, o);
    using Access = gpu::ExplicitKokkosSurfaceSolverTestAccess;
    const double initial = sum(s.volume);
    bc.edge_bc_flow[slot] = -.02;
    Access::prepareBoundarySteps(kk);
    Access::boundarySubstep(kk, .1);
    bc.edge_bc_flow[slot] = .02;
    Access::boundarySubstep(kk, .1);
    Access::publish(kk, .2);
    const double expected = .002 * m.edge_length[slot];
    near(s.boundary_in_applied[0], expected, "reversal inflow");
    near(s.boundary_out_applied[0], expected, "reversal outflow");
    near(s.edge_flux[slot], 0, "reversal mean flux");
    near(sum(s.volume), initial, "reversal storage");
    Access::publish(kk, .2);
    near(s.boundary_in_applied[0], expected, "no repeated inflow publication");
    near(s.boundary_out_applied[0], expected, "no repeated outflow publication");
    const double available = s.volume[0];
    bc.edge_bc_flow[slot] = 1000.;
    Access::boundarySubstep(kk, .1);
    Access::publish(kk, .1);
    near(s.volume[0], 0, "boundary availability floor");
    near(s.boundary_out_applied[0], expected + available, "clamped applied boundary outflow");
    require(-s.boundary_requested[0] > s.boundary_out_applied[0],
            "boundary request distinguishes rejected water");
    near(sum(s.volume) + s.boundary_out_applied[0] - s.boundary_in_applied[0], initial,
         "clamped boundary conservation");
    std::cout << "Within-window boundary reversal and dry-out PASS\n";
}

/**
 * @brief Compare device/CPU drain and spill exchange with a shared finite node pond budget.
 *
 * Exercises both closures and two coupling laws with two cells sharing one node over windows ending
 * at 0.37/0.91 s. Checks exchange direction, applied ledgers, surface conservation and at most 0.01
 * m3 shared pond spill per window. Borrowed node/coupling/state fixtures outlive both solvers.
 * Requires initialized Kokkos; prints PASS or throws.
 */
void nodeExchange() {
    for (bool vfr : {false, true})
        for (int law : {0, 1})
            for (bool spill : {false, true}) {
                auto m = mesh(3, 0, false, vfr);
                auto a = state(m, .2), b = a;
                auto o = options(4);
                if (vfr)
                    o.cell_closure = CellClosure2D::VFR;
                openswmm::NodeData nodes;
                nodes.resize(1);
                nodes.invert_elev[0] = -.1;
                nodes.full_depth[0] = .1;
                nodes.full_volume[0] = 1.;
                nodes.volume[0] = 1.01; // Only the .01 ponded share may spill.
                nodes.head[0] = spill ? .8 : -.05;
                nodes.depth[0] = nodes.head[0] - nodes.invert_elev[0];
                std::vector<CouplingPoint> points;
                for (int cell : {0, 1}) {
                    CouplingPoint cp{};
                    cp.cell_idx = cell;
                    cp.vertex_idx = -1;
                    cp.node_idx = 0;
                    cp.cd = .6;
                    cp.area = .1;
                    cp.law = law;
                    cp.geo.z_crest = 0;
                    cp.geo.l_weir = .4;
                    cp.geo.a_orif = .1;
                    points.push_back(cp);
                }
                a.nodes_1d = b.nodes_1d = &nodes;
                a.node_coupling = b.node_coupling = &points;
                ExplicitInertialSolver cpu;
                gpu::ExplicitKokkosSurfaceSolver kk;
                cpu.initialize(m, a, o);
                kk.initialize(m, b, o);
                const double initial = sum(a.volume);
                double total = 0, t = 0;
                for (double end : {.37, .91}) {
                    cpu.advance(t, end);
                    kk.advance(t, end);
                    compare(b.volume, a.volume, "node exchange storage", 2e-9);
                    compare(kk.last_coupling_exchange(), cpu.last_coupling_exchange(),
                            "node exchange ledger", 2e-9);
                    compare(b.coupling_applied, a.coupling_applied, "node applied exchange", 2e-9);
                    const double exchanged = sum(kk.last_coupling_exchange());
                    require(spill ? exchanged < 0 : exchanged > 0, "node exchange direction");
                    if (spill)
                        require(-exchanged <= .01 + 1e-12, "shared node ponded-only spill budget");
                    total += exchanged;
                    near(sum(b.volume) + total, initial, "node exchange conservation");
                    t = end;
                }
            }
    std::cout << "Node drain/spill and shared pond budget PASS\n";
}

/**
 * @brief Copy host mesh/edge geometry into the portable SWE kernel view layout.
 *
 * Requires initialized Kokkos and mutually consistent array sizes/indices. Computes
 * midpoint-to-centroid slot arms on host, then deep-copies each field; input references are not
 * retained. No topology/index validation is performed here; allocation/runtime failures propagate.
 *
 * @param[in] m Borrowed mesh with built topology, cell metrics, edge normals/lengths/conveyance and
 *     valid vertex indices; not mutated.
 * @param[in] e Borrowed compatible inertial edge/CSR/metric arrays; every referenced left slot must
 *     index m edge storage; not mutated.
 * @return Owned collection of reference-counted Kokkos device views for geometry, topology, slot
 *     arms, boundary metrics and conveyance.
 */
sk::Mesh mirrorMesh(MeshData& m, InertialEdges& e) {
    sk::Mesh d;
    std::vector<int> nv(m.n_cells()), nei(4 * m.n_cells(), -1),
        sign(e.cell_sign.begin(), e.cell_sign.end());
    std::vector<double> ax(nei.size()), ay(nei.size()), conv(e.ne);
    for (int i = 0; i < m.n_cells(); ++i) {
        nv[i] = m.cell_vertex_count(i);
        for (int k = 0; k < nv[i]; ++k) {
            int slot = 4 * i + k;
            nei[slot] = m.cell_neighbour(i, k);
            int a = m.cell_vertex(i, k), b = m.cell_vertex(i, (k + 1) % nv[i]);
            ax[slot] = .5 * (m.vx[a] + m.vx[b]) - m.tri_cx[i];
            ay[slot] = .5 * (m.vy[a] + m.vy[b]) - m.tri_cy[i];
        }
    }
    for (int k = 0; k < e.ne; ++k)
        conv[k] = m.edge_conveyance[e.slotL[k]];
    d.nv = device(nv);
    d.neighbour = device(nei);
    d.cell_ptr = device(e.cell_ptr);
    d.cell_edge = device(e.cell_edge);
    d.cell_sign = device(sign);
    d.cL = device(e.cL);
    d.cR = device(e.cR);
    d.cx = device(m.tri_cx);
    d.cy = device(m.tri_cy);
    d.area = device(m.tri_area);
    d.nx = device(e.nx);
    d.ny = device(e.ny);
    d.xi = device(e.xi);
    d.mx = device(e.mx);
    d.my = device(e.my);
    d.conveyance = device(conv);
    d.cell_arm_x = device(e.cell_arm_x);
    d.cell_arm_y = device(e.cell_arm_y);
    d.slot_arm_x = device(ax);
    d.slot_arm_y = device(ay);
    d.boundary_nx = device(m.edge_nx);
    d.boundary_ny = device(m.edge_ny);
    d.boundary_length = device(m.edge_length);
    return d;
}
/**
 * @brief Compare portable device SWE kernel stages with the CPU Euler implementation.
 *
 * Tests triangles/quads/mixed meshes, first/second-order reconstruction and four wet/dry/conveyance
 * scenarios. Compares gradients, face water/momentum transfers and discharge, independent host
 * gathering, transfer consumption, then flat closure plus wall/friction momentum. Exercises kernels
 * directly rather than enabling unsupported FULL_SWE Kokkos solver initialization. Requires
 * initialized Kokkos; prints case progress and throws on mismatch.
 */
void sweChecks() {
    for (int shape : {0, 1, 2})
        for (int scenario = 0; scenario < 4; ++scenario)
            for (int order : {1, 2}) {
                auto m = mesh(6, shape, true);
                if (scenario == 3)
                    for (double& c : m.edge_conveyance)
                        c = .35;
                auto s = state(m, .3);
                auto o = options();
                o.momentum = Momentum2D::FULL_SWE;
                o.reconstruction_order = order;
                o.dry_depth = 1e-5;
                for (int i = 0; i < m.n_cells(); ++i) {
                    s.volume[i] = (.12 + .03 * m.tri_cx[i]) * m.tri_area[i];
                    if (scenario > 0 && m.tri_cx[i] > 4)
                        s.volume[i] = scenario == 2 ? 1e-7 * m.tri_area[i] : 0;
                }
                ExplicitInertialSolver cpu;
                cpu.initialize(m, s, o);
                for (int i = 0; i < m.n_cells(); ++i) {
                    A::qx(cpu)[i] = s.depth[i] * (.1 + .02 * m.tri_cy[i]);
                    A::qy(cpu)[i] = s.depth[i] * (-.03 + .01 * m.tri_cx[i]);
                    if (scenario == 3 && m.tri_cx[i] > 3)
                        s.head[i] += 1;
                }
                auto& e = A::edges(cpu);
                auto dm = mirrorMesh(m, e);
                std::vector<int> active(A::active(cpu).begin(), A::active(cpu).end());
                sk::State ds{device(s.head),
                             device(s.depth),
                             device(s.volume),
                             device(A::qx(cpu)),
                             device(A::qy(cpu)),
                             device(active),
                             device(std::vector<int>(A::tiers(cpu).begin(), A::tiers(cpu).end()))};
                sk::Gradients g{gpu::DView("gx", 4 * m.n_cells()),
                                gpu::DView("gy", 4 * m.n_cells())};
                auto cells = device(A::cells(cpu));
                auto faces = device(A::faceList(cpu));
                gpu::IView tiers("face_tiers", e.ne);
                if (order == 2) {
                    A::gradients(cpu);
                    sk::gradients(dm, ds, g, cells, A::cells(cpu).size(), o.dry_depth);
                    compare(host(g.x), A::gradients(cpu, 0), "gradient x");
                    compare(host(g.y), A::gradients(cpu, 1), "gradient y");
                }
                sk::Transfers tr{gpu::DView("wl", e.ne), gpu::DView("wr", e.ne),
                                 gpu::DView("xl", e.ne), gpu::DView("xr", e.ne),
                                 gpu::DView("yl", e.ne), gpu::DView("yr", e.ne),
                                 gpu::DView("q", e.ne)};
                double dt = scenario == 2 ? 10. : .03;
                A::faces(cpu, dt);
                sk::faces(dm, ds, g, tr, faces, A::faceList(cpu).size(), tiers, dt, o.dry_depth,
                          o.exchange_beta, order == 2, true);
                auto expected = A::transfers(cpu);
                int k = 0;
                for (auto v : {tr.waterL, tr.waterR, tr.mxL, tr.mxR, tr.myL, tr.myR, tr.discharge})
                    compare(host(v), expected[k++], "face transfer");
                near(sum(host(tr.waterL)) + sum(host(tr.waterR)), 0, "face conservation");
                gpu::DView dx("dx", m.n_cells()), dy("dy", m.n_cells());
                sk::gather(dm, ds, tr, cells, A::cells(cpu).size(), dx, dy);
                std::vector<double> ev = s.volume, ex(m.n_cells()), ey(m.n_cells());
                for (int i : A::cells(cpu)) {
                    double dv = 0;
                    for (int p = e.cell_ptr[i]; p < e.cell_ptr[i + 1]; ++p) {
                        int edge = e.cell_edge[p], side = e.cell_sign[p] > 0 ? 0 : 1;
                        dv += expected[side][edge];
                        ex[i] += expected[2 + side][edge];
                        ey[i] += expected[4 + side][edge];
                    }
                    ev[i] = std::max(0., ev[i] + dv);
                }
                compare(host(ds.volume), ev, "gather volume");
                compare(host(dx), ex, "gather mx");
                compare(host(dy), ey, "gather my");
                for (auto v : {tr.waterL, tr.waterR, tr.mxL, tr.mxR, tr.myL, tr.myR})
                    near(sum(host(v)), 0, "consumed transfers");
                // Complete the source-free Euler cell operation: FLAT closure followed
                // by wall pressure and friction. Compare the integrated CPU cell routine.
                A::cells(cpu, dt);
                std::vector<double> depth = ev, head = ev;
                for (int i = 0; i < m.n_cells(); ++i) {
                    depth[i] = ev[i] / m.tri_area[i];
                    head[i] = m.tri_cz[i] + depth[i];
                }
                Kokkos::deep_copy(ds.depth, device(depth));
                Kokkos::deep_copy(ds.head, device(head));
                std::vector<int> wallptr(m.n_cells() + 1), wallslots;
                for (int i = 0; i < m.n_cells(); ++i) {
                    wallptr[i] = wallslots.size();
                    for (int k = 0; k < m.cell_vertex_count(i); ++k)
                        if (m.cell_neighbour(i, k) < 0)
                            wallslots.push_back(4 * i + k);
                }
                wallptr.back() = wallslots.size();
                auto wall_ptr = device(wallptr);
                auto wall_slot = device(wallslots);
                auto roughness = device(m.mannings_n);
                sk::momentum(dm, ds, cells, A::cells(cpu).size(), dx, dy, wall_ptr, wall_slot,
                             roughness, dt, o.dry_depth);
                compare(host(ds.volume), s.volume, "Euler volume");
                compare(host(ds.qx), A::qx(cpu), "Euler momentum x");
                compare(host(ds.qy), A::qy(cpu), "Euler momentum y");
                std::cout << "SWE shape=" << shape << " scenario=" << scenario << " order=" << order
                          << " PASS\n";
            }
}
/**
 * @brief Verify external host volume injection reactivates dry CPU/device flow consistently.
 *
 * Advances an initially dry four-tier fixture, injects 1 m3 into cell zero at t=1 s, resynchronizes
 * and advances to t=2 s. Checks conserved total water, active redistribution and CPU/device parity.
 * Requires initialized Kokkos; prints PASS or throws.
 */
void externalVolumeResync() {
    auto m = mesh(4);
    auto a = state(m, 0.0), b = a;
    auto o = options(4);
    ExplicitInertialSolver cpu;
    gpu::ExplicitKokkosSurfaceSolver device_solver;
    cpu.initialize(m, a, o);
    device_solver.initialize(m, b, o);
    cpu.advance(0, 1);
    device_solver.advance(0, 1);
    a.volume[0] = b.volume[0] = 1.0;
    cpu.resyncFromVolumes(1);
    device_solver.resyncFromVolumes(1);
    cpu.advance(1, 2);
    device_solver.advance(1, 2);
    near(sum(a.volume), 1.0, "CPU external transfer conservation");
    near(sum(b.volume), 1.0, "Kokkos external transfer conservation");
    require(a.volume[0] < 1.0 && b.volume[0] < 1.0, "external transfer activates faces");
    compare(a.volume, b.volume, "external transfer CPU/Kokkos parity", 2e-9);
    std::cout << "External volume resync PASS\n";
}
/**
 * @brief Check advance counters remain attributable to separate device solver instances.
 *
 * Returns immediately when performance collection is disabled; does not enable it. Otherwise
 * advances one instance twice and another once, verifies counts 2/1, and prints PASS. Requires
 * initialized Kokkos; assertion/runtime failures propagate.
 */
void perModelCounters() {
    if (!openswmm::perf::enabled())
        return;
    auto m = mesh(2);
    auto a = state(m, .1), b = state(m, .2);
    auto o = options();
    gpu::ExplicitKokkosSurfaceSolver x, y;
    x.initialize(m, a, o);
    y.initialize(m, b, o);
    x.advance(0, .1);
    y.advance(0, .1);
    x.advance(.1, .2);
    require(gpu::ExplicitKokkosSurfaceSolverTestAccess::advances(x) == 2, "first model counters");
    require(gpu::ExplicitKokkosSurfaceSolverTestAccess::advances(y) == 1, "second model counters");
    std::cout << "Per-model performance attribution PASS\n";
}
/**
 * @brief Run the standalone host/device alignment checks inside a Kokkos runtime lifetime.
 *
 * Initializes Kokkos and prints execution-space/concurrency before the try block; finalizes after
 * caught test failures. Initialization, initial output or finalization failures are not caught
 * here. OpenMP builds also check execution space and, when present, OMP_NUM_THREADS using atoi.
 * Runs all case functions and the final ledger average check; writes progress/PASS diagnostics to
 * stdout.
 *
 * @param[in,out] argc Command-line argument count passed by reference to Kokkos initialization; may
 *     be modified by Kokkos.
 * @param[in,out] argv Writable argument pointer array accepted by Kokkos for argc entries;
 *     pointed-to strings and sentinel follow its command-line conventions.
 * @return 0 after all assertions pass; 1 when a std::exception from the guarded test body is
 *     printed to stderr.
 */
int main(int argc, char** argv) {
    Kokkos::initialize(argc, argv);
    std::cout << "Execution space: " << gpu::ExecSpace::name()
              << ", concurrency=" << gpu::ExecSpace().concurrency() << std::endl;
    int rc = 0;
    try {
#ifdef OPENSWMM_GPU_EXECSPACE_OPENMP
        require(std::string(gpu::ExecSpace::name()) == "OpenMP", "OpenMP execution space required");
        if (const char* n = std::getenv("OMP_NUM_THREADS"))
            require(gpu::ExecSpace().concurrency() == std::atoi(n),
                    "requested OpenMP concurrency was not active");
#endif
        perModelCounters();
        frozenTier();
        unsupported();
        marcher();
        boundaries();
        boundaryReversal();
        nodeExchange();
        externalVolumeResync();
        sweChecks();
        auto initial = device(std::vector<double>{7., -3., 0.});
        auto live = device(std::vector<double>{11., 5., 2.});
        sk::averageLedger(initial, live);
        compare(host(live), {9., 1., 1.}, "RK ledger average");
        std::cout << "PASS host/device-space alignment checks\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        rc = 1;
    }
    Kokkos::finalize();
    return rc;
}
