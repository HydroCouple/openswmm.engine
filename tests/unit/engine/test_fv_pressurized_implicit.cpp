/**
 * @file test_fv_pressurized_implicit.cpp
 * @brief Slot program R2a — gates for the implicit pressurized head update
 *        (FV_PRESSURIZED_IMPLICIT / PressurizedHeadSolver).
 *
 * The gates follow plans/FV_SLOT_STORAGE_PROGRAM_2026-08-24.md §R2:
 *   G1  steady full-bore head loss equals Manning and is INVARIANT in the
 *       slot celerity (the base explicit scheme fails this by design);
 *   G2  inertness — a run that never pressurizes is bit-identical with the
 *       option on;
 *   G3  lake-at-rest while fully pressurized, Euler and RK2;
 *   G4  a filling bore's arrival is preserved (transition faces keep the
 *       explicit Godunov flux);
 *   G5  exact volume conservation through a pressurized slosh;
 *   G7/G8 (unit-scale) — the substep count is set by advection, not by the
 *       slot celerity: flat wall-vs-c, and an order-of-magnitude drop
 *       against the explicit baseline;
 *   plus the degree-3 fold (surcharged tee → the CG component path).
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#include "fv_test_support.hpp"

using namespace fvtest;
namespace k = openswmm::fv::kernels;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

FvOptions pressOptions(double, bool implicit_on) {
    FvOptions o = defaultOptions();
    o.pressurized_implicit = implicit_on;
    return o;
}

/// Advance in fixed chunks (a stand-in for routing steps).
void advanceBy(ExplicitFvSolver& s, double t0, double t_end, double chunk,
               const FvStepForcing& f, long* substeps = nullptr) {
    double t = t0;
    while (t < t_end - 1.0e-9) {
        const double target = std::min(t + chunk, t_end);
        t = s.advance(t, target, f);
        if (substeps) *substeps += s.last_num_steps();
    }
}

} // namespace

// ---------------------------------------------------------------------------
// G3 — lake at rest while fully pressurized
// ---------------------------------------------------------------------------

class PressLakeAtRest : public ::testing::TestWithParam<int> {};

INSTANTIATE_TEST_SUITE_P(Integrators, PressLakeAtRest, ::testing::Values(0, 1),
                         [](const ::testing::TestParamInfo<int>& i) {
                             return i.param == 0 ? std::string("Euler")
                                                 : std::string("RK2");
                         });

TEST_P(PressLakeAtRest, staysAtRestToMachinePrecision) {
    Channel ch = makeWalledChannel(circular(3.0), 24, 50.0,
                                   [](double) { return 0.0; }, 0.015, 100.0);
    const double eta0 = 9.0;   // 3 diameters of surcharge — every cell in P
    seedLevel(ch, eta0);

    FvOptions o = pressOptions(100.0, true);
    o.time_integration =
        (GetParam() == 1) ? TimeIntegration::RK2 : TimeIntegration::EULER;

    ExplicitFvSolver s;
    s.initialize(ch.mesh, ch.state, o);
    FvStepForcing f{};
    advanceBy(s, 0.0, 600.0, 30.0, f);

    for (int i = 0; i < ch.n; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        EXPECT_NEAR(ch.state.cell_q[ui], 0.0, 1.0e-9) << "cell " << i;
        const double eta = ch.mesh.cell_zb[ui] +
                           k::depthOfArea(ch.mesh.geom[0], ch.state.cell_a[ui]);
        EXPECT_NEAR(eta, eta0, 1.0e-9) << "cell " << i;
    }
}

// Both tests above surcharge every cell (eta0 = 9.0 is three diameters of a
// D=3 pipe), so neither exercises the regime where a still pool STRADDLES the
// crown — part of the pipe pressurized, part still open-channel. That is the
// hardest case for the slot closure, and it was an uncovered gap: found while
// investigating the partly-full lake-at-rest drift reported during the POLYGON
// geometry work, by sweeping fill level instead of testing a single one.
//
// Measured over a 12-level sweep on a D=4 pipe across a slope break, worst
// free-surface drift after 600 s at rest:
//
//   fully open, no cell pressurized    1.8e-15 ft  (option inert here)
//   straddling the crown, implicit ON  4.4e-14 ft
//   straddling the crown, implicit OFF 3.3e-02 ft  <-- 10^12 worse
//
// The first row was 7.4e-05 when this test was written, and was described
// here as a separate pre-existing limitation of the Preissmann taper band
// that the option could not touch. That attribution was wrong: it was the
// discontinuity in i1OfDepth (Simpson nodes vs trapezoid refinement), fixed
// in the same change that added this note, and it is now at the
// floating-point floor. See the residual term in FvKernels.hpp.
//
// What remains in row three is genuinely a different mechanism and genuinely
// needs the implicit solver: with cells straddling y_crown and the option
// off, the explicit acoustic step is the binding problem, not the closure.
// Also confirmed independent of the substep-acceptance fix in this same
// change — that fix left every number in the sweep identical, because a pool
// at rest has u = 0 everywhere and never saturates the positivity limiter.
TEST(PressLakeAtRest, holdsWhileStraddlingTheCrown) {
    // Slope break, so cells sit at a range of bed elevations and one pool
    // level puts some above the crown and some below it.
    auto bed = [](double x) {
        if (x < 150.0) return 10.0 - 0.02 * x;
        if (x < 300.0) return 7.0 + 0.02 * (x - 150.0);
        return 10.0 - 0.01 * (x - 300.0);
    };
    const double eta0 = 11.5;

    Channel ch = makeWalledChannel(circular(4.0), 150, 3.0, bed, 0.014);
    seedLevel(ch, eta0);

    // Non-vacuity: the point of this test is the MIXED state, so assert the
    // fixture actually produces one. Without this the test would still pass
    // if a future change to SLOT_CROWN_CUTOFF or to the bed left every cell
    // on one side of the crown, and it would then be testing nothing.
    // Depth is taken from the bed the way seedLevel itself computes it:
    // seedLevel writes cell_a and cell_q only, and cell_h is not derived
    // until the solver runs, so reading cell_h here would see zeros and the
    // guard would misfire (it did, on the first cut of this test).
    const FvGeometry& g = ch.mesh.geom[0];
    int n_press = 0, n_open = 0;
    for (int i = 0; i < ch.n; ++i) {
        const double h = std::max(
            0.0, eta0 - ch.mesh.cell_zb[static_cast<std::size_t>(i)]);
        if (h <= k::kDryDepth) continue;
        if (h >= g.y_crown) ++n_press; else ++n_open;
    }
    ASSERT_GT(n_press, 0) << "fixture does not pressurize any cell";
    ASSERT_GT(n_open, 0)  << "fixture pressurizes every cell -- not straddling";

    FvOptions o = pressOptions(100.0, true);
    ExplicitFvSolver s;
    s.initialize(ch.mesh, ch.state, o);
    FvStepForcing f{};
    advanceBy(s, 0.0, 600.0, 60.0, f);

    // Same 1e-9 the sibling lake-at-rest tests demand: well-balancedness is a
    // property of the scheme, and crossing the crown is not licence to lose it.
    for (int i = 0; i < ch.n; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        if (ch.state.cell_h[ui] <= k::kDryDepth) continue;
        const double eta = ch.mesh.cell_zb[ui] +
                           k::depthOfArea(ch.mesh.geom[0], ch.state.cell_a[ui]);
        EXPECT_NEAR(eta, eta0, 1.0e-9) << "cell " << i;
    }
}

// The rest state must also hold across a BED STEP under pressure — the
// piezometric head, not the depth, is what the implicit system diffuses.
TEST(PressLakeAtRest, holdsOverAnUnevenBed) {
    Channel ch = makeWalledChannel(
        circular(3.0), 24, 50.0,
        [](double x) { return (x > 600.0) ? 1.5 : 0.0; }, 0.015, 100.0);
    const double eta0 = 9.0;
    seedLevel(ch, eta0);

    FvOptions o = pressOptions(100.0, true);
    ExplicitFvSolver s;
    s.initialize(ch.mesh, ch.state, o);
    FvStepForcing f{};
    advanceBy(s, 0.0, 600.0, 30.0, f);

    for (int i = 0; i < ch.n; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        EXPECT_NEAR(ch.state.cell_q[ui], 0.0, 1.0e-9) << "cell " << i;
        const double eta = ch.mesh.cell_zb[ui] +
                           k::depthOfArea(ch.mesh.geom[0], ch.state.cell_a[ui]);
        EXPECT_NEAR(eta, eta0, 1.0e-9) << "cell " << i;
    }
}

// ---------------------------------------------------------------------------
// G2 — inertness: never-pressurized run is bit-identical with the option on
// ---------------------------------------------------------------------------

TEST(PressInertness, freeSurfaceRunIsBitIdenticalWithTheOptionOn) {
    auto run = [](bool option_on) {
        Channel ch = makeWalledChannel(circular(3.0), 32, 40.0,
                                       [](double x) { return -0.001 * x; },
                                       0.015, 100.0);
        // A genuine transient, everywhere below band entry.
        for (int i = 0; i < ch.n; ++i) {
            const auto ui = static_cast<std::size_t>(i);
            const double h = (i < 8) ? 1.2 : 0.3;
            ch.state.cell_a[ui] = k::areaOfDepth(ch.mesh.geom[0], h);
            ch.state.cell_q[ui] = 0.0;
        }
        FvOptions o = pressOptions(100.0, option_on);
        ExplicitFvSolver s;
        s.initialize(ch.mesh, ch.state, o);
        FvStepForcing f{};
        advanceBy(s, 0.0, 300.0, 15.0, f);
        return std::make_pair(ch.state.cell_a, ch.state.cell_q);
    };
    const auto off = run(false);
    const auto on  = run(true);
    ASSERT_EQ(off.first.size(), on.first.size());
    EXPECT_EQ(0, std::memcmp(off.first.data(), on.first.data(),
                             off.first.size() * sizeof(double)));
    EXPECT_EQ(0, std::memcmp(off.second.data(), on.second.data(),
                             off.second.size() * sizeof(double)));
}

// ---------------------------------------------------------------------------
// G5 — exact conservation through a pressurized slosh
// ---------------------------------------------------------------------------

TEST(PressConservation, sloshConservesVolumeExactly) {
    Channel ch = makeWalledChannel(circular(3.0), 32, 40.0,
                                   [](double) { return 0.0; }, 0.012, 150.0);
    // Tilted pressurized surface, at rest: the relaxation is an acoustic
    // slosh handled entirely by the implicit pass (walls stay explicit).
    for (int i = 0; i < ch.n; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        const double eta = 8.0 + 2.0 * (static_cast<double>(i) /
                                        static_cast<double>(ch.n - 1));
        ch.state.cell_a[ui] = k::areaOfDepth(ch.mesh.geom[0], eta);
        ch.state.cell_q[ui] = 0.0;
    }
    const double v0 = totalVolume(ch);

    FvOptions o = pressOptions(150.0, true);
    ExplicitFvSolver s;
    s.initialize(ch.mesh, ch.state, o);
    FvStepForcing f{};
    advanceBy(s, 0.0, 900.0, 30.0, f);

    for (int i = 0; i < ch.n; ++i)
        ASSERT_TRUE(std::isfinite(ch.state.cell_q[static_cast<std::size_t>(i)]));
    EXPECT_NEAR(totalVolume(ch), v0, 1.0e-10 * v0);
    // And the slosh must actually DAMP (friction), not ring up.
    double qmax = 0.0;
    for (int i = 0; i < ch.n; ++i)
        qmax = std::max(qmax,
                        std::fabs(ch.state.cell_q[static_cast<std::size_t>(i)]));
    EXPECT_LT(qmax, 50.0);
}

// ---------------------------------------------------------------------------
// G1 — steady full-bore head loss: Manning, invariant in the slot celerity
// ---------------------------------------------------------------------------

namespace {

struct HeadLossResult {
    double dh = 0.0;       // upstream head − downstream head (ft)
    double q_mid = 0.0;    // discharge at mid-pipe (cfs)
    long   substeps = 0;
    int    chunks = 0;     // 30 s routing chunks actually run
    bool   converged = false;
    /// Substeps per simulated second — the only form in which two runs that
    /// converged after DIFFERENT numbers of chunks may be compared. The
    /// explicit baseline needs hundreds of chunks where the implicit run needs
    /// twelve, so comparing raw totals flatters the implicit side by the span
    /// ratio on top of the effect being measured.
    double perSecond() const {
        return static_cast<double>(substeps) /
               (static_cast<double>(chunks) * 30.0);
    }
};

HeadLossResult runHeadLoss(double celerity, bool implicit_on,
                           double q_in = 50.0) {
    const int    n  = 64;
    const double dx = 25.0;
    Channel ch = makeNodedChannel(circular(3.0), n, dx,
                                  [](double) { return 0.0; }, 0.013, celerity);
    const double h_dn = 20.0;
    seedLevel(ch, h_dn);
    ch.state.node_head[0] = h_dn;
    ch.state.node_head[1] = h_dn;

    FvOptions o = pressOptions(celerity, implicit_on);
    ExplicitFvSolver s;
    s.initialize(ch.mesh, ch.state, o);

    std::vector<double> lat   = {q_in, 0.0};
    std::vector<double> fixed = {kNaN, h_dn};
    FvStepForcing f{};
    f.node_lateral    = lat.data();
    f.node_fixed_head = fixed.data();
    f.n_nodes = 2;

    HeadLossResult r;
    double t = 0.0;
    for (int chunk = 0; chunk < 400; ++chunk) {
        t = s.advance(t, t + 30.0, f);
        r.substeps += s.last_num_steps();
        double dev = 0.0;
        for (int i = 0; i < ch.n; ++i)
            dev = std::max(dev,
                           std::fabs(ch.state.cell_q[static_cast<std::size_t>(
                                         i)] - q_in));
        r.chunks = chunk + 1;
        if (dev < 0.002 * q_in && chunk > 10) {
            r.converged = true;
            break;
        }
    }
    r.dh = ch.state.node_head[0] - ch.state.node_head[1];
    r.q_mid = ch.state.cell_q[static_cast<std::size_t>(n / 2)];
    return r;
}

} // namespace

TEST(PressHeadLoss, matchesManningAndIsSlotWidthInvariant) {
    // Analytic: S_f = (n·V / (φ·R^(2/3)))², h_f = S_f · L.
    const double A  = 0.25 * M_PI * 9.0;
    const double V  = 50.0 / A;
    const double R  = 0.75;
    const double sf = std::pow(0.013 * V / (1.486 * std::pow(R, 2.0 / 3.0)), 2.0);
    const double hf = sf * 64.0 * 25.0;

    double dh_min = 1.0e30, dh_max = -1.0e30;
    for (const double c : {30.0, 100.0, 300.0, 1000.0}) {
        const HeadLossResult r = runHeadLoss(c, true);
        ASSERT_TRUE(r.converged) << "c=" << c;
        EXPECT_NEAR(r.q_mid, 50.0, 0.5) << "c=" << c;
        EXPECT_NEAR(r.dh, hf, 0.025 * hf) << "c=" << c << " analytic=" << hf;
        dh_min = std::min(dh_min, r.dh);
        dh_max = std::max(dh_max, r.dh);
    }
    // The thesis gate: the spread across a 33x width range is < 0.5%.
    EXPECT_LT((dh_max - dh_min) / (0.5 * (dh_max + dh_min)), 0.005);
}

// ---------------------------------------------------------------------------
// G8 (unit scale) — the census is advection-bound: wall flat in c, and far
// below the explicit baseline
// ---------------------------------------------------------------------------

TEST(PressCensus, substepCountIsCelerityInvariantAndFarBelowExplicit) {
    const HeadLossResult on_100  = runHeadLoss(100.0, true);
    const HeadLossResult on_1000 = runHeadLoss(1000.0, true);
    ASSERT_TRUE(on_100.converged);
    ASSERT_TRUE(on_1000.converged);
    // Flat wall-vs-c (the substep count is advective).  ±10% per the plan.
    EXPECT_NEAR(static_cast<double>(on_1000.substeps) /
                    static_cast<double>(on_100.substeps),
                1.0, 0.10);

    // And the explicit baseline at the same celerity needs at least 5x the
    // substeps to cover the same simulated span (it is acoustic-bound).
    //
    // Per SIMULATED SECOND, not raw totals: the explicit run converges after
    // hundreds of 30 s chunks where the implicit run takes twelve, so
    // comparing totals would multiply the effect under test by that span
    // ratio and pass even if the per-second rates were equal. Both runs are
    // required to have converged, so both spans are meaningful.
    const HeadLossResult off_300 = runHeadLoss(300.0, false);
    const HeadLossResult on_300  = runHeadLoss(300.0, true);
    ASSERT_TRUE(on_300.converged);
    ASSERT_TRUE(off_300.converged);
    EXPECT_GT(off_300.perSecond(), 5.0 * on_300.perSecond());
}

// ---------------------------------------------------------------------------
// G4 — the filling bore is preserved (transition faces stay Godunov)
// ---------------------------------------------------------------------------

namespace {

/// Time at which the LAST interior cell pressurizes under a fixed upstream
/// head driving a bore into an initially near-empty pipe.
double boreArrival(double celerity, bool implicit_on) {
    const int    n  = 48;
    const double dx = 25.0;
    Channel ch = makeNodedChannel(circular(3.0), n, dx,
                                  [](double) { return 0.0; }, 0.013, celerity);
    seedLevel(ch, 0.3);
    const double h_up = 12.0;
    ch.state.node_head[0] = h_up;
    ch.state.node_head[1] = ch.mesh.node_invert[1];

    FvOptions o = pressOptions(celerity, implicit_on);
    ExplicitFvSolver s;
    s.initialize(ch.mesh, ch.state, o);

    std::vector<double> fixed = {h_up, 0.3};
    FvStepForcing f{};
    f.node_fixed_head = fixed.data();
    f.n_nodes = 2;

    // Watch a cell at 60% of the length: with H_up = 12 ft over 1200 ft the
    // steady hydraulic grade crosses the crown near x ≈ 900 ft, so the
    // pressurization front never reaches the last cells — it STALLS there
    // physically, under either scheme.
    const FvGeometry& g = ch.mesh.geom[0];
    const auto watch = static_cast<std::size_t>((6 * n) / 10);
    double t = 0.0;
    while (t < 4000.0) {
        t = s.advance(t, t + 5.0, f);
        if (ch.state.cell_h[watch] >= g.y_full) return t;
    }
    return -1.0;
}

} // namespace

// The reference here is the QUASI-STEADY MANNING FILL, not the explicit
// scheme: the base binary under-delivers through a pressurized entrance
// (the force-main pathology of R1/B1 — slot transients eat driving head),
// measured at 115 s against the 42 s analytic on this deck. The front
// machinery itself is identical under both schemes (transition faces stay
// explicit, flux and census bound alike); what the implicit pass fixes is
// the DELIVERY through the pressurized section feeding the front.
TEST(PressBore, arrivalMatchesTheQuasiSteadyFillAndIsCelerityStable) {
    const double t_on  = boreArrival(150.0, true);
    ASSERT_GT(t_on, 0.0);

    // dL/dt = Q(L)/ΔA with Q(L) = (φ/n)·A_full·R^(2/3)·√(H_d/L):
    // t(L_w) = (2/3)·L_w^(3/2)·ΔA / (K·√H_d).
    FvGeometry g;
    buildGeometry(circular(3.0), false, 150.0, g);
    const double a_full = 0.25 * M_PI * 9.0;
    const double a0     = k::areaOfDepth(g, 0.3);
    const double K  = (1.486 / 0.013) * a_full * std::pow(0.75, 2.0 / 3.0);
    const double hd = 12.0 - 3.0;
    const double lw = (6.0 * 48.0 / 10.0) * 25.0;   // watch cell station
    const double t_est =
        (2.0 / 3.0) * std::pow(lw, 1.5) * (a_full - a0) / (K * std::sqrt(hd));
    EXPECT_NEAR(t_on, t_est, 0.5 * t_est) << "analytic " << t_est;

    // Celerity invariance of the arrival — the slot width must not set the
    // front speed (5% + the 5 s advance quantum).
    const double t_on_660 = boreArrival(660.0, true);
    ASSERT_GT(t_on_660, 0.0);
    EXPECT_NEAR(t_on_660, t_on, 0.05 * t_on + 10.0);

    // The explicit baseline's arrival goes on record, not on a gate: its
    // entrance deficit is the very defect the program exists to remove.
    const double t_off = boreArrival(150.0, false);
    ::testing::Test::RecordProperty("explicit_arrival_s",
                                    std::to_string(t_off));
    ::testing::Test::RecordProperty("implicit_arrival_s",
                                    std::to_string(t_on));
    ::testing::Test::RecordProperty("analytic_arrival_s",
                                    std::to_string(t_est));
}

// ---------------------------------------------------------------------------
// Degree-3 fold — the surcharged tee drives the CG component path
// ---------------------------------------------------------------------------

namespace {

/// Three identical pressurized pipes star-joined at one algebraic junction:
/// inflow node → tee → two outfalls at a common fixed head. Exercises the
/// junction fold at degree 3 (no simple path — the CG branch must solve it).
struct Tee {
    NetworkMeshData  mesh;
    NetworkStateData state;
    int cells_per_pipe = 4;
};

Tee makeTee(double celerity) {
    Tee t;
    const int    nc = t.cells_per_pipe;
    const double dx = 50.0;

    t.mesh.geom.resize(1);
    buildGeometry(circular(3.0), false, celerity, t.mesh.geom[0]);
    t.mesh.geom[0].roughness = 0.013;
    t.mesh.geom[0].rough_factor = 32.2 * (0.013 / 1.486) * (0.013 / 1.486);
    t.mesh.geom[0].barrels = 1;

    // Cells: pipe p occupies [p·nc, (p+1)·nc). All beds flat at 0.
    const int ncell = 3 * nc;
    for (int i = 0; i < ncell; ++i) {
        t.mesh.cell_geom.push_back(0);
        t.mesh.cell_conduit.push_back(i / nc);
        t.mesh.cell_dx.push_back(dx);
        t.mesh.cell_zb.push_back(0.0);
        t.mesh.cell_dzdx.push_back(0.0);
    }
    t.mesh.conduit_cell_begin = {0, nc, 2 * nc};
    t.mesh.conduit_cell_count = {nc, nc, nc};
    t.mesh.conduit_link       = {0, 1, 2};

    auto face = [&](int cl, int cr, int node) {
        t.mesh.face_cl.push_back(cl);
        t.mesh.face_cr.push_back(cr);
        t.mesh.face_node.push_back(node);
        t.mesh.face_zb.push_back(0.0);
        t.mesh.face_dx.push_back((cl >= 0 && cr >= 0) ? dx : 0.5 * dx);
        t.mesh.face_dir_l.push_back(1);
        t.mesh.face_dir_r.push_back(1);
        t.mesh.face_virtual.push_back(0);
        t.mesh.face_vj_node.push_back(-1);
    };

    // Nodes: 0 = inflow junction, 1 = tee junction, 2,3 = outfalls.
    // Pipe 0: node0 → tee. Pipe 1: tee → node2. Pipe 2: tee → node3.
    face(-1, 0, 0);                                  // f0  node0 | pipe0
    for (int i = 1; i < nc; ++i) face(i - 1, i, -1); // interior pipe0
    face(nc - 1, -1, 1);                             // pipe0 | tee
    face(-1, nc, 1);                                 // tee | pipe1
    for (int i = 1; i < nc; ++i) face(nc + i - 1, nc + i, -1);
    face(2 * nc - 1, -1, 2);                         // pipe1 | out1
    face(-1, 2 * nc, 1);                             // tee | pipe2
    for (int i = 1; i < nc; ++i) face(2 * nc + i - 1, 2 * nc + i, -1);
    face(3 * nc - 1, -1, 3);                         // pipe2 | out2

    const int nf = t.mesh.n_faces();
    t.mesh.cell_face0.assign(static_cast<std::size_t>(ncell), -1);
    t.mesh.cell_face1.assign(static_cast<std::size_t>(ncell), -1);
    t.mesh.cell_side0.assign(static_cast<std::size_t>(ncell), 0);
    t.mesh.cell_side1.assign(static_cast<std::size_t>(ncell), 0);
    for (int f2 = 0; f2 < nf; ++f2) {
        const auto uf = static_cast<std::size_t>(f2);
        auto attach = [&](int cell, int8_t side) {
            const auto uc = static_cast<std::size_t>(cell);
            if (t.mesh.cell_face0[uc] < 0) {
                t.mesh.cell_face0[uc] = f2;
                t.mesh.cell_side0[uc] = side;
            } else {
                t.mesh.cell_face1[uc] = f2;
                t.mesh.cell_side1[uc] = side;
            }
        };
        if (t.mesh.face_cl[uf] >= 0) attach(t.mesh.face_cl[uf], 0);
        if (t.mesh.face_cr[uf] >= 0) attach(t.mesh.face_cr[uf], 1);
    }

    // Chains: one per pipe.
    t.mesh.chain_ptr = {0, nc, 2 * nc, 3 * nc};
    t.mesh.cell_chain.resize(static_cast<std::size_t>(ncell));
    t.mesh.cell_chain_pos.resize(static_cast<std::size_t>(ncell));
    for (int i = 0; i < ncell; ++i) {
        t.mesh.chain_cells.push_back(i);
        t.mesh.chain_dir.push_back(1);
        t.mesh.cell_chain[static_cast<std::size_t>(i)] = i / nc;
        t.mesh.cell_chain_pos[static_cast<std::size_t>(i)] = i % nc;
    }

    t.mesh.node_invert      = {0.0, 0.0, 0.0, 0.0};
    t.mesh.node_full_depth  = {1.0e6, 1.0e6, 1.0e6, 1.0e6};
    t.mesh.node_ponded_area = {0.0, 0.0, 0.0, 0.0};
    t.mesh.node_kind = {kNodeJunction, kNodeJunction, kNodeOutfall,
                        kNodeOutfall};
    t.mesh.node_area.assign(4, openswmm::constants::MIN_SURFAREA);
    t.mesh.node_vol_off  = {-1, -1, -1, -1};
    t.mesh.node_vol_dmax = {0.0, 0.0, 0.0, 0.0};
    t.mesh.node_vol_atop = {0.0, 0.0, 0.0, 0.0};

    // Node → face CSR. Tee faces: pipe0's downstream end (enters tee, +1),
    // pipe1's and pipe2's upstream ends (leave tee, −1).
    const int f_up   = 0;
    const int f_p0dn = nc;
    const int f_p1up = nc + 1;
    const int f_p1dn = 2 * nc + 1;
    const int f_p2up = 2 * nc + 2;
    const int f_p2dn = 3 * nc + 2;
    t.mesh.node_face_ptr  = {0, 1, 4, 5, 6};
    t.mesh.node_face_idx  = {f_up, f_p0dn, f_p1up, f_p2up, f_p1dn, f_p2dn};
    t.mesh.node_face_sign = {-1.0, 1.0, -1.0, -1.0, 1.0, 1.0};
    t.mesh.node_face_zb   = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};

    t.state.resize(ncell, 4, 0);
    return t;
}

} // namespace

TEST(PressTee, degreeThreeFoldSplitsSymmetricallyUnderCG) {
    Tee tee = makeTee(300.0);
    const double h0 = 15.0;
    const FvGeometry& g = tee.mesh.geom[0];
    for (std::size_t i = 0; i < tee.state.cell_a.size(); ++i)
        tee.state.cell_a[i] = k::areaOfDepth(g, h0);
    for (int n2 = 0; n2 < 4; ++n2) tee.state.node_head[static_cast<std::size_t>(n2)] = h0;

    FvOptions o = pressOptions(300.0, true);
    ExplicitFvSolver s;
    s.initialize(tee.mesh, tee.state, o);

    std::vector<double> lat   = {40.0, 0.0, 0.0, 0.0};
    std::vector<double> fixed = {kNaN, kNaN, h0, h0};
    FvStepForcing f{};
    f.node_lateral    = lat.data();
    f.node_fixed_head = fixed.data();
    f.n_nodes = 4;

    double t = 0.0;
    for (int chunk = 0; chunk < 240; ++chunk) t = s.advance(t, t + 30.0, f);

    for (const double q : tee.state.cell_q) ASSERT_TRUE(std::isfinite(q));
    const int nc = tee.cells_per_pipe;
    const double q_in = tee.state.cell_q[static_cast<std::size_t>(nc / 2)];
    const double q_b1 = tee.state.cell_q[static_cast<std::size_t>(nc + nc / 2)];
    const double q_b2 =
        tee.state.cell_q[static_cast<std::size_t>(2 * nc + nc / 2)];
    EXPECT_NEAR(q_in, 40.0, 0.5);
    EXPECT_NEAR(q_b1, 20.0, 0.5);
    EXPECT_NEAR(q_b2, 20.0, 0.5);
    EXPECT_NEAR(q_b1, q_b2, 0.1);   // symmetry: identical branches
    // Both junctions sit above the tee outfalls' head — flow runs downhill
    // in HEAD even though every bed is flat.
    EXPECT_GT(tee.state.node_head[0], tee.state.node_head[1]);
    EXPECT_GT(tee.state.node_head[1], h0);
}
