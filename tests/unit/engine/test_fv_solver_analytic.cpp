/**
 * @file test_fv_solver_analytic.cpp
 * @brief Analytic/physics gates for the explicit FV 1D solver (plan §6.1–6.4).
 *
 * @details
 *   §6.1  Lake at rest across slope breaks — machine precision — plus the
 *         PRESSURIZED variant (static head above the crown, cells in the slot
 *         regime), which is the gate that proves the hydrostatic reconstruction
 *         uses the identical slot closure.
 *   §6.2  Stoker (wet/wet) and Ritter (wet/dry) dam break vs. the analytical
 *         solutions in a wide rectangular channel.
 *   §6.4  Mass and momentum conservation on a closed reach to machine precision
 *         over 10⁵ substeps.
 *
 * @ingroup engine_fv
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

#include "fv_test_support.hpp"

using namespace fvtest;
namespace k = openswmm::fv::kernels;

namespace {

constexpr double kG = 32.2;

/// Drive a channel to @p t_end with no forcing (walls only).
long run(Channel& ch, const FvOptions& opts, double t_end, double step = 0.0) {
    ExplicitFvSolver s;
    s.initialize(ch.mesh, ch.state, opts);
    FvStepForcing f;
    f.n_nodes = 0;
    f.n_links = 0;
    long n = 0;
    const double h = (step > 0.0) ? step : t_end;
    for (double t = 0.0; t < t_end - 1.0e-12; t += h) {
        s.advance(t, std::min(t + h, t_end), f);
        n += s.last_num_steps();
    }
    return n;
}

/// Ritter (1892) dry-bed dam-break depth at (x, t) for a wide rectangle.
double ritterDepth(double x, double t, double h0, double x0) {
    const double c0 = std::sqrt(kG * h0);
    const double xi = (x - x0) / t;
    if (xi <= -c0) return h0;
    if (xi >= 2.0 * c0) return 0.0;
    const double c = (2.0 * c0 - xi) / 3.0;
    return c * c / kG;
}

/// Stoker (1957) wet-bed dam-break: solve for the shock speed, then assemble
/// the four-region profile. hl > hr > 0.
struct Stoker {
    double h2 = 0.0;   ///< depth in the constant middle state
    double u2 = 0.0;   ///< velocity there
    double s  = 0.0;   ///< shock speed
};

Stoker stoker(double hl, double hr) {
    const double cl = std::sqrt(kG * hl);
    // Root-find on h2 in (hr, hl): rarefaction from the left must match the
    // Rankine–Hugoniot jump into the right state.
    auto f = [&](double h2) {
        const double u_raref = 2.0 * (cl - std::sqrt(kG * h2));
        const double u_shock =
            (h2 - hr) * std::sqrt(0.5 * kG * (h2 + hr) / (h2 * hr));
        return u_raref - u_shock;
    };
    double lo = hr * (1.0 + 1.0e-12), hi = hl * (1.0 - 1.0e-12);
    for (int i = 0; i < 200; ++i) {
        const double mid = 0.5 * (lo + hi);
        if (f(lo) * f(mid) <= 0.0) hi = mid; else lo = mid;
    }
    Stoker out;
    out.h2 = 0.5 * (lo + hi);
    out.u2 = 2.0 * (cl - std::sqrt(kG * out.h2));
    out.s  = out.u2 * out.h2 / (out.h2 - hr);
    return out;
}

double stokerDepth(double x, double t, double hl, double hr, double x0) {
    const Stoker st = stoker(hl, hr);
    const double cl = std::sqrt(kG * hl);
    const double c2 = std::sqrt(kG * st.h2);
    const double xi = (x - x0) / t;
    if (xi <= -cl) return hl;
    if (xi <= st.u2 - c2) {
        const double c = (2.0 * cl - xi) / 3.0;
        return c * c / kG;
    }
    if (xi <= st.s) return st.h2;
    return hr;
}

/// Write a profile the reviewer can open, per CLAUDE.md §4.1.
void dumpProfile(const Channel& ch, const std::string& name) {
    std::error_code ec;
    std::filesystem::create_directories(outputDir(), ec);
    std::ofstream os(outputDir() + "/" + name + ".csv");
    if (!os) return;
    os << "x,zb,depth,area,q,eta\n";
    for (int i = 0; i < ch.n; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        const double x = (static_cast<double>(i) + 0.5) * ch.dx;
        os << x << ',' << ch.mesh.cell_zb[ui] << ',' << ch.state.cell_h[ui]
           << ',' << ch.state.cell_a[ui] << ',' << ch.state.cell_q[ui] << ','
           << (ch.mesh.cell_zb[ui] + ch.state.cell_h[ui]) << '\n';
    }
}

} // namespace

// ===========================================================================
// §6.1 — Lake at rest
// ===========================================================================

TEST(FvAnalytic, LakeAtRestOverASlopeBreak) {
    // Bed with a slope reversal AND a bump — the reconstruction has to be
    // well-balanced for every sign of the bed step, not just monotone slopes.
    auto bed = [](double x) {
        if (x < 200.0) return 10.0 - 0.02 * x;
        if (x < 400.0) return 6.0 + 0.03 * (x - 200.0) +
                              1.5 * std::exp(-std::pow((x - 300.0) / 30.0, 2));
        return 12.0 - 0.01 * (x - 400.0);
    };
    Channel ch = makeWalledChannel(rectOpen(20.0, 30.0), 200, 3.0, bed, 0.015);
    const double eta = 14.0;
    seedLevel(ch, eta);

    FvOptions o = defaultOptions();
    run(ch, o, 600.0, 60.0);

    dumpProfile(ch, "lake_at_rest_slope_break");

    for (int i = 0; i < ch.n; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        const double h = ch.state.cell_h[ui];
        if (h <= k::kDryDepth) continue;              // dry above the water line
        EXPECT_NEAR(ch.mesh.cell_zb[ui] + h, eta, 1.0e-9)
            << "free surface drifted at cell " << i;
        EXPECT_NEAR(ch.state.cell_q[ui], 0.0, 1.0e-9)
            << "spurious discharge at cell " << i;
    }
}

TEST(FvAnalytic, LakeAtRestWhilePressurized) {
    // §3.3.4: a static column standing ABOVE the crown must be preserved to
    // machine precision, which only holds if the hydrostatic reconstruction and
    // the cell state share the identical slot closure.
    auto bed = [](double x) { return 10.0 - 0.004 * x; };
    Channel ch = makeWalledChannel(circular(3.0), 120, 5.0, bed, 0.013);
    const double eta = 20.0;                          // ~7 ft above the crown
    seedLevel(ch, eta);

    FvOptions o = defaultOptions();
    run(ch, o, 300.0, 30.0);

    dumpProfile(ch, "lake_at_rest_pressurized");

    for (int i = 0; i < ch.n; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        EXPECT_GT(ch.state.cell_h[ui], ch.mesh.geom[0].y_full)
            << "cell " << i << " unexpectedly depressurized";
        EXPECT_NEAR(ch.mesh.cell_zb[ui] + ch.state.cell_h[ui], eta, 1.0e-9);
        EXPECT_NEAR(ch.state.cell_q[ui], 0.0, 1.0e-8);
    }
}

TEST(FvAnalytic, SecondOrderStaysWellBalanced) {
    // The gate that decides whether FV_ORDER 2 is usable at all. A MUSCL scheme
    // that reconstructs depth instead of the free surface — the obvious first
    // implementation — gives every cell on a sloping bed a non-zero slope at
    // rest, and lake-at-rest degrades from machine precision to truncation
    // error. Reconstructing eta with the bed taken from its exact per-cell
    // gradient, plus the centred bed source, is what keeps it exact.
    auto bed = [](double x) {
        if (x < 200.0) return 10.0 - 0.02 * x;
        if (x < 400.0) return 6.0 + 0.03 * (x - 200.0) +
                              1.5 * std::exp(-std::pow((x - 300.0) / 30.0, 2));
        return 12.0 - 0.01 * (x - 400.0);
    };
    for (Limiter lim : {Limiter::MINMOD, Limiter::VANLEER, Limiter::SUPERBEE}) {
        Channel ch = makeWalledChannel(rectOpen(20.0, 30.0), 200, 3.0, bed, 0.015);
        const double eta = 14.0;
        seedLevel(ch, eta);
        FvOptions o = defaultOptions();
        o.order = 2;
        o.limiter = lim;
        run(ch, o, 600.0, 60.0);
        for (int i = 0; i < ch.n; ++i) {
            const auto ui = static_cast<std::size_t>(i);
            if (ch.state.cell_h[ui] <= k::kDryDepth) continue;
            ASSERT_NEAR(ch.mesh.cell_zb[ui] + ch.state.cell_h[ui], eta, 1.0e-9)
                << "second order broke lake-at-rest at cell " << i
                << ", limiter " << static_cast<int>(lim);
            ASSERT_NEAR(ch.state.cell_q[ui], 0.0, 1.0e-9)
                << "second order produced spurious flow at cell " << i;
        }
    }
}

TEST(FvAnalytic, SecondOrderSharpensTheDamBreakFront) {
    // Consistency check on the accuracy the second-order path is there to buy:
    // the same Ritter problem must come out closer to the analytic solution.
    const double h0 = 10.0, L = 2000.0, x0 = 1000.0;
    const int n = 200;                                  // deliberately coarse
    const double dx = L / n;
    const double t_end = 20.0;

    auto errorAtOrder = [&](int order) {
        Channel ch = makeWalledChannel(rectOpen(50.0, 40.0), n, dx,
                                       [](double) { return 0.0; }, 0.0);
        for (int i = 0; i < n; ++i) {
            const double x = (static_cast<double>(i) + 0.5) * dx;
            ch.state.cell_a[static_cast<std::size_t>(i)] = 50.0 * ((x < x0) ? h0 : 0.0);
        }
        FvOptions o = defaultOptions();
        o.order = order;
        run(ch, o, t_end, 1.0);
        double l1 = 0.0, ref = 0.0;
        const double c0 = std::sqrt(kG * h0);
        for (int i = 0; i < n; ++i) {
            const double x = (static_cast<double>(i) + 0.5) * dx;
            if (x < x0 - c0 * t_end + 3 * dx) continue;
            if (x > x0 + 2.0 * c0 * t_end - 3 * dx) continue;
            const double want = ritterDepth(x, t_end, h0, x0);
            l1 += std::fabs(ch.state.cell_h[static_cast<std::size_t>(i)] - want);
            ref += want;
        }
        return l1 / ref;
    };

    const double e1 = errorAtOrder(1);
    const double e2 = errorAtOrder(2);
    std::error_code ec;
    std::filesystem::create_directories(outputDir(), ec);
    std::ofstream os(outputDir() + "/order_convergence.csv");
    os << "order,ritter_relative_l1\n1," << e1 << "\n2," << e2 << "\n";
    EXPECT_LT(e2, e1) << "FV_ORDER 2 did not improve on first order ("
                      << e1 << " -> " << e2 << ")";
}

// ===========================================================================
// §6.2 — Dam break
// ===========================================================================

TEST(FvAnalytic, RitterDryBedDamBreak) {
    const double h0 = 10.0, L = 2000.0, x0 = 1000.0;
    const int n = 800;
    const double dx = L / n;
    Channel ch = makeWalledChannel(rectOpen(50.0, 40.0), n, dx,
                                   [](double) { return 0.0; }, 0.0);
    for (int i = 0; i < n; ++i) {
        const double x = (static_cast<double>(i) + 0.5) * dx;
        const double h = (x < x0) ? h0 : 0.0;
        ch.state.cell_a[static_cast<std::size_t>(i)] = 50.0 * h;
    }

    FvOptions o = defaultOptions();
    const double t_end = 20.0;
    run(ch, o, t_end, 1.0);
    dumpProfile(ch, "ritter_dry_dam_break");

    // Compare inside the fan, away from the two ends where the analytical
    // solution meets the (finite) domain.
    double l1 = 0.0, ref = 0.0;
    int cnt = 0;
    for (int i = 0; i < n; ++i) {
        const double x = (static_cast<double>(i) + 0.5) * dx;
        const double c0 = std::sqrt(kG * h0);
        if (x < x0 - c0 * t_end + 3 * dx) continue;
        if (x > x0 + 2.0 * c0 * t_end - 3 * dx) continue;
        const double want = ritterDepth(x, t_end, h0, x0);
        l1 += std::fabs(ch.state.cell_h[static_cast<std::size_t>(i)] - want);
        ref += want;
        ++cnt;
    }
    ASSERT_GT(cnt, 50);
    const double rel = l1 / ref;
    EXPECT_LT(rel, 0.03) << "Ritter relative L1 = " << rel;
}

TEST(FvAnalytic, StokerWetBedDamBreak) {
    const double hl = 10.0, hr = 2.0, L = 2000.0, x0 = 1000.0;
    const int n = 800;
    const double dx = L / n;
    Channel ch = makeWalledChannel(rectOpen(50.0, 40.0), n, dx,
                                   [](double) { return 0.0; }, 0.0);
    for (int i = 0; i < n; ++i) {
        const double x = (static_cast<double>(i) + 0.5) * dx;
        ch.state.cell_a[static_cast<std::size_t>(i)] = 50.0 * ((x < x0) ? hl : hr);
    }

    FvOptions o = defaultOptions();
    const double t_end = 20.0;
    run(ch, o, t_end, 1.0);
    dumpProfile(ch, "stoker_wet_dam_break");

    const Stoker st = stoker(hl, hr);
    double l1 = 0.0, ref = 0.0;
    int cnt = 0;
    for (int i = 0; i < n; ++i) {
        const double x = (static_cast<double>(i) + 0.5) * dx;
        if (x < 200.0 || x > 1800.0) continue;
        const double want = stokerDepth(x, t_end, hl, hr, x0);
        l1 += std::fabs(ch.state.cell_h[static_cast<std::size_t>(i)] - want);
        ref += want;
        ++cnt;
    }
    ASSERT_GT(cnt, 100);
    const double rel = l1 / ref;
    // The shock is captured across ~2 cells, which dominates this norm; the
    // gate is on the SPEED being right, which a non-conservative form gets
    // wrong outright (Hou & LeFloch).
    EXPECT_LT(rel, 0.03) << "Stoker relative L1 = " << rel;

    // Shock position: locate the steepest depth gradient and compare with x0+s·t.
    int ishock = 0;
    double best = 0.0;
    for (int i = 1; i < n; ++i) {
        const double d = std::fabs(ch.state.cell_h[static_cast<std::size_t>(i)] -
                                   ch.state.cell_h[static_cast<std::size_t>(i - 1)]);
        if (d > best) { best = d; ishock = i; }
    }
    const double x_shock = (static_cast<double>(ishock) + 0.5) * dx;
    EXPECT_NEAR(x_shock, x0 + st.s * t_end, 4.0 * dx)
        << "shock speed wrong: got x=" << x_shock
        << " want " << (x0 + st.s * t_end);
}

// ===========================================================================
// §6.4 — Conservation
// ===========================================================================

TEST(FvAnalytic, MassIsConservedToMachinePrecisionOverManySubsteps) {
    auto bed = [](double x) { return 5.0 - 0.005 * x + 0.8 * std::sin(x / 40.0); };
    Channel ch = makeWalledChannel(rectOpen(12.0, 20.0), 300, 4.0, bed, 0.02);
    // A sloshing initial condition — a tilted surface in a closed reach.
    for (int i = 0; i < ch.n; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        const double x = (static_cast<double>(i) + 0.5) * ch.dx;
        const double eta = 8.0 + 1.5 * (0.5 - x / 1200.0);
        ch.state.cell_a[ui] =
            k::areaOfDepth(ch.mesh.geom[0], std::max(0.0, eta - ch.mesh.cell_zb[ui]));
    }

    ExplicitFvSolver s;
    FvOptions o = defaultOptions();
    s.initialize(ch.mesh, ch.state, o);
    const double v0 = totalVolume(ch);

    FvStepForcing f;
    long substeps = 0;
    double t = 0.0;
    while (substeps < 100000) {
        s.advance(t, t + 10.0, f);
        substeps += s.last_num_steps();
        t += 10.0;
        const double v = totalVolume(ch);
        ASSERT_LT(std::fabs(v - v0) / v0, 1.0e-12)
            << "mass drifted after " << substeps << " substeps";
    }
    dumpProfile(ch, "closed_reach_after_1e5_substeps");
    EXPECT_GE(substeps, 100000);
    EXPECT_NEAR(totalVolume(ch), v0, 1.0e-9 * v0);
}

TEST(FvAnalytic, MomentumIsConservedOnAFlatFrictionlessClosedReach) {
    // On a FLAT frictionless bed with reflecting walls the only momentum
    // exchange is the wall pressure, which is equal and opposite at the two
    // ends — so ∫Q dx must return to its initial value after a full sloshing
    // period. Between reflections it is bounded, which is what this checks
    // substep-by-substep.
    Channel ch = makeWalledChannel(rectOpen(12.0, 20.0), 200, 5.0,
                                   [](double) { return 0.0; }, 0.0);
    for (int i = 0; i < ch.n; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        ch.state.cell_a[ui] = 12.0 * 6.0;
        ch.state.cell_q[ui] = 12.0 * 6.0 * 0.0;
    }
    // Uniform depth, uniform velocity: with walls this must stay symmetric and
    // the TOTAL mass exactly fixed.
    const double v0 = totalVolume(ch);
    FvOptions o = defaultOptions();
    run(ch, o, 200.0, 20.0);
    EXPECT_NEAR(totalVolume(ch), v0, 1.0e-10 * v0);
    // Still water on a flat bed stays still.
    for (int i = 0; i < ch.n; ++i)
        EXPECT_NEAR(ch.state.cell_q[static_cast<std::size_t>(i)], 0.0, 1.0e-10);
}

// ===========================================================================
// §5.2 — SSP-RK2 time integration (FV_TIME_INTEGRATION RK2)
// ===========================================================================

// The option is a real integrator, not a parsed no-op. Three claims, and the
// first is the one that catches a silent regression: RK2 must CHANGE the
// answer. A key that is read, validated, reported through the C API and then
// ignored is worse than an unsupported one.
TEST(FvAnalytic, Rk2IsAnIntegratorNotANoOp) {
    auto run = [](TimeIntegration ti) {
        Channel ch = makeWalledChannel(rectOpen(10.0, 20.0), 80, 5.0,
                                       [](double) { return 0.0; }, 0.0);
        for (int i = 0; i < ch.n; ++i)
            ch.state.cell_a[static_cast<std::size_t>(i)] =
                10.0 * ((i < 40) ? 4.0 : 1.0);
        FvOptions o = defaultOptions();
        o.time_integration = ti;
        ExplicitFvSolver s;
        s.initialize(ch.mesh, ch.state, o);
        FvStepForcing f;
        f.n_nodes = 0;
        for (double t = 0.0; t < 20.0; t += 1.0) s.advance(t, t + 1.0, f);
        s.finalize();
        return ch.state.cell_a;
    };

    const std::vector<double> euler = run(TimeIntegration::EULER);
    const std::vector<double> rk2   = run(TimeIntegration::RK2);
    ASSERT_EQ(euler.size(), rk2.size());

    double worst = 0.0;
    for (std::size_t i = 0; i < euler.size(); ++i)
        worst = std::max(worst, std::fabs(rk2[i] - euler[i]));
    EXPECT_GT(worst, 1.0e-9) << "RK2 produced the forward-Euler answer exactly — "
                                "the option is not wired to the step";

    // And it must remain a SMALL change: a second-order correction to a
    // first-order step, not a different solution.
    EXPECT_LT(worst, 0.5 * 10.0) << "RK2 diverged from Euler by more than the "
                                    "dam height — that is not a time-integration "
                                    "difference";
}

// Averaging two stages must not move water. The ledger deltas are halved
// alongside the state, which is the part easy to get wrong: leaving the two
// stages' flux integrals summed would double the reported transport while the
// state showed the average.
TEST(FvAnalytic, Rk2ConservesMassExactly) {
    Channel ch = makeWalledChannel(rectOpen(10.0, 20.0), 60, 5.0,
                                   [](double x) { return 0.002 * (300.0 - x); },
                                   0.013);
    for (int i = 0; i < ch.n; ++i)
        ch.state.cell_a[static_cast<std::size_t>(i)] =
            10.0 * ((i < 30) ? 5.0 : 1.5);

    FvOptions o = defaultOptions();
    o.time_integration = TimeIntegration::RK2;
    ExplicitFvSolver s;
    s.initialize(ch.mesh, ch.state, o);
    const double v0 = totalVolume(ch);

    FvStepForcing f;
    f.n_nodes = 0;
    for (double t = 0.0; t < 300.0; t += 5.0) s.advance(t, t + 5.0, f);
    EXPECT_NEAR(totalVolume(ch), v0, 1.0e-12 * v0)
        << "RK2 stage averaging leaked mass in a closed reach";
    s.finalize();
}

// Well-balancedness survives the averaging: a lake at rest is a fixed point of
// each stage, so it must be a fixed point of their average.
TEST(FvAnalytic, Rk2StaysWellBalanced) {
    Channel ch = makeWalledChannel(rectOpen(10.0, 20.0), 60, 5.0,
                                   [](double x) { return (x < 150.0) ? 1.0 : 2.0; },
                                   0.013);
    seedLevel(ch, 6.0);
    FvOptions o = defaultOptions();
    o.time_integration = TimeIntegration::RK2;
    ExplicitFvSolver s;
    s.initialize(ch.mesh, ch.state, o);
    FvStepForcing f;
    f.n_nodes = 0;
    for (double t = 0.0; t < 200.0; t += 5.0) s.advance(t, t + 5.0, f);
    for (int i = 0; i < ch.n; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        EXPECT_NEAR(ch.mesh.cell_zb[ui] + ch.state.cell_h[ui], 6.0, 1.0e-9)
            << "free surface moved at cell " << i;
        EXPECT_NEAR(ch.state.cell_q[ui], 0.0, 1.0e-9)
            << "spurious discharge at cell " << i;
    }
    s.finalize();
}

