/**
 * @file      test_runoff_ode_exact.cpp
 * @brief     RUNOFF_ODE_SOLVER EXACT (Track 1C, 2026-11-26): the exact ponded-depth step
 *            against a tight reference across its branches, the drain-through, the option
 *            through the input file and the C API, and a small deck's runoff against LEGACY.
 *
 * @details The reference is the legacy update's structure with the Cash-Karp integrator run at
 *          tolerance 1e-15 and the exponent 5/3, which is about 1e-13 accurate; the exact step
 *          must land within 1e-9 of it in excess depth and 1e-7 in outflow, while the legacy
 *          RK45 at its tolerance of 1e-4 is seen to be further away than that.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>

#include "core/SWMMEngine.hpp"
#include "hydrology/PondedDepthExact.hpp"
#include "hydrology/Runoff.hpp"
#include "math/OdeSolver.hpp"

namespace fs = std::filesystem;
using openswmm::runoff::ExactPondedDepth;

namespace {

constexpr double kM = 5.0 / 3.0;

/// Cash-Karp RK45 on dy/dx = f(y) at tolerance eps, the legacy loop in double.
template <typename F>
bool rk45(double& y, double x1, double x2, double eps, double h1, F&& derivs) {
    using namespace openswmm::ode::detail;
    double dydx, yscal, ytemp, yerr, ak2, ak3, ak4, ak5, ak6, hnext = 0, x = x1, h = h1;
    for (int nstp = 1; nstp <= 2000000; ++nstp) {
        derivs(y, dydx);
        yscal = std::fabs(y) + std::fabs(dydx * h) + 1e-30;
        if ((x + h - x2) * (x + h - x1) > 0)
            h = x2 - x;
        for (;;) {
            ytemp = y + b21 * h * dydx;
            derivs(ytemp, ak2);
            ytemp = y + h * (b31 * dydx + b32 * ak2);
            derivs(ytemp, ak3);
            ytemp = y + h * (b41 * dydx + b42 * ak2 + b43 * ak3);
            derivs(ytemp, ak4);
            ytemp = y + h * (b51 * dydx + b52 * ak2 + b53 * ak3 + b54 * ak4);
            derivs(ytemp, ak5);
            ytemp = y + h * (b61 * dydx + b62 * ak2 + b63 * ak3 + b64 * ak4 + b65 * ak5);
            derivs(ytemp, ak6);
            ytemp = y + h * (c1 * dydx + c3 * ak3 + c4 * ak4 + c6 * ak6);
            yerr = h * (dc1 * dydx + dc3 * ak3 + dc4 * ak4 + dc5 * ak5 + dc6 * ak6);
            const double errmax = std::fabs(yerr / yscal) / eps;
            if (errmax > 1) {
                const double htemp = 0.9 * h * std::pow(errmax, -0.25);
                h = (htemp > 0.1 * h) ? htemp : 0.1 * h;
                if (x + h == x)
                    return false;
                continue;
            }
            hnext = (errmax > 1.89e-4) ? 0.9 * h * std::pow(errmax, -0.2) : 5.0 * h;
            x += h;
            y = ytemp;
            break;
        }
        if ((x - x2) * (x2 - x1) >= 0)
            return true;
        h = hnext;
    }
    return false;
}

/// The legacy update's structure with the tight integrator: the reference.
bool referenceStep(double& depth, double inflow, double alpha, double dStore, double dt,
                   double& t_runoff) {
    bool ok = true;
    double tx = dt;
    if (depth + inflow * tx <= dStore) {
        depth += inflow * tx;
    } else {
        const double dx = dStore - depth;
        if (dx > 0 && inflow > 0) {
            tx -= dx / inflow;
            depth = dStore;
        }
        if (alpha > 0 && tx > 0) {
            ok = rk45(depth, 0.0, tx, 1e-15, tx, [=](double d, double& dddt) {
                const double rx = d - dStore;
                dddt = inflow - ((rx > 0) ? alpha * std::pow(rx, kM) : 0.0);
            });
        } else {
            tx = std::max(tx, 0.0);
            depth += inflow * tx;
        }
    }
    depth = std::max(depth, 0.0);
    t_runoff = tx;
    return ok;
}

double referenceOutflow(double depth, double dStore, double alpha) {
    const double x = depth - dStore;
    return x > 1.0e-10 ? alpha * std::pow(x, kM) : 0.0;
}

/// RunoffSolver::updatePondedDepth as written (it is private): the legacy structure over the
/// engine's own integrateScalar at ODETOL with legacy's exponent literal.
void legacyStep(double& depth, double inflow, double alpha, double dStore, double dt,
                double& t_runoff) {
    using openswmm::runoff::MEXP;
    using openswmm::runoff::ODETOL;
    double tx = dt;
    if (depth + inflow * tx <= dStore) {
        depth += inflow * tx;
    } else {
        const double dx = dStore - depth;
        if (dx > 0.0 && inflow > 0.0) {
            tx -= dx / inflow;
            depth = dStore;
        }
        if (alpha > 0.0 && tx > 0.0) {
            openswmm::ode::integrateScalar(
                &depth, 0.0, tx, ODETOL, tx, [=](double, const double* d, double* dddt) {
                    const double rx = *d - dStore;
                    *dddt = inflow - ((rx > 0.0) ? alpha * std::pow(rx, MEXP) : 0.0);
                });
        } else {
            tx = std::max(tx, 0.0);
            depth += inflow * tx;
        }
    }
    depth = std::max(depth, 0.0);
    t_runoff = tx;
}

struct SweepErrors {
    double depth = 0, outflow = 0, legacy_depth = 0;
    int cases = 0, drained = 0;
};

/// alpha = |i| = 1 (xe = 1, T = 1), dStore 0.1; u0 and dtau swept on log grids.
SweepErrors sweep(int sigma, double lo, double hi, bool one_minus) {
    SweepErrors e;
    ExactPondedDepth::ScaleCache cache;
    for (int iu = 0; iu <= 30; ++iu) {
        const double p = lo * std::pow(hi / lo, iu / 30.0);
        const double u0 = one_minus ? 1.0 - p : (sigma > 0 ? 1.0 + p : p);
        for (int it = 0; it <= 24; ++it) {
            const double dtau = 1e-6 * std::pow(1e8, it / 24.0);
            const double dStore = 0.1, depth_in = dStore + u0;
            double dr = depth_in, tr;
            if (!referenceStep(dr, sigma * 1.0, 1.0, dStore, dtau, tr))
                continue;
            const double qr = referenceOutflow(dr, dStore, 1.0);
            double te, qe;
            const double de = ExactPondedDepth::step(depth_in, sigma * 1.0, 1.0, 1.0, dStore, dtau,
                                                     te, qe, cache);
            double dl = depth_in, tl;
            legacyStep(dl, sigma * 1.0, 1.0, dStore, dtau, tl);
            if (dr <= dStore)
                ++e.drained;
            const double xr = dr - dStore;
            e.depth = std::max(e.depth, std::fabs(de - dr) / std::max(std::fabs(xr), 1e-9));
            e.outflow = std::max(e.outflow, std::fabs(qe - qr) / std::max(qr, 1e-9));
            e.legacy_depth =
                std::max(e.legacy_depth, std::fabs(dl - dr) / std::max(std::fabs(xr), 1e-9));
            EXPECT_EQ(te, tr);
            EXPECT_GE(de, 0.0);
            ++e.cases;
        }
    }
    return e;
}

} // namespace

TEST(ExactPondedDepth, MatchesTightReferenceWithNetInflowBelowEquilibrium) {
    const SweepErrors e = sweep(+1, 1e-14, 0.999, true);
    EXPECT_EQ(e.cases, 31 * 25);
    EXPECT_LT(e.depth, 1e-9) << "max relative excess-depth error";
    EXPECT_LT(e.outflow, 1e-7) << "max relative outflow error";
    EXPECT_GT(e.legacy_depth, e.depth) << "the reference must tell LEGACY from EXACT";
}

TEST(ExactPondedDepth, MatchesTightReferenceWithNetInflowAboveEquilibrium) {
    const SweepErrors e = sweep(+1, 1e-14, 1e6, false);
    EXPECT_EQ(e.cases, 31 * 25);
    EXPECT_LT(e.depth, 1e-9);
    EXPECT_LT(e.outflow, 1e-7);
    EXPECT_GT(e.legacy_depth, e.depth);
}

TEST(ExactPondedDepth, MatchesTightReferenceWithNetLossAndDrainsThrough) {
    const SweepErrors e = sweep(-1, 1e-14, 1e6, false);
    EXPECT_EQ(e.cases, 31 * 25);
    EXPECT_GT(e.drained, 100) << "the sweep must include drain-through cases";
    EXPECT_LT(e.depth, 1e-9);
    EXPECT_LT(e.outflow, 1e-7);
    EXPECT_GT(e.legacy_depth, e.depth);
}

TEST(ExactPondedDepth, DrainThroughFallsLinearlyBelowStorageAndFloorsAtZero) {
    ExactPondedDepth::ScaleCache cache;
    // alpha = 1, net loss 1 ft/s, excess 0.3 ft above a 0.1 ft storage: G(0.3) ~ 0.29 s to drain
    const double dStore = 0.1, a = 1.0;
    double tr, q;
    const double d1 =
        ExactPondedDepth::step(dStore + 0.3, -a, 1.0, 1.0, dStore, 0.35, tr, q, cache);
    double dref = dStore + 0.3, trr;
    ASSERT_TRUE(referenceStep(dref, -a, 1.0, dStore, 0.35, trr));
    EXPECT_LT(dref, dStore) << "the reference drains below storage in this step";
    EXPECT_NEAR(d1, dref, 1e-9);
    EXPECT_EQ(q, 0.0);
    EXPECT_EQ(tr, 0.35);
    // long enough to empty the surface entirely: floored at zero like legacy
    const double d2 = ExactPondedDepth::step(dStore + 0.3, -a, 1.0, 1.0, dStore, 5.0, tr, q, cache);
    EXPECT_EQ(d2, 0.0);
}

TEST(ExactPondedDepth, NoNetInflowUsesTheClosedForm) {
    ExactPondedDepth::ScaleCache cache;
    const double dStore = 0.05, alpha = 0.4, x0 = 0.02, dt = 60.0;
    double tr, q;
    const double d1 = ExactPondedDepth::step(dStore + x0, 0.0, alpha, 1.0 / std::pow(alpha, 0.6),
                                             dStore, dt, tr, q, cache);
    double dref = dStore + x0, trr;
    ASSERT_TRUE(referenceStep(dref, 0.0, alpha, dStore, dt, trr));
    EXPECT_NEAR(d1 - dStore, dref - dStore, 1e-12);
    EXPECT_NEAR(q, referenceOutflow(dref, dStore, alpha),
                1e-12 * referenceOutflow(dref, dStore, alpha));
}

namespace {

const char* kOptionsHead = R"([TITLE]
RUNOFF_ODE_SOLVER test deck

[OPTIONS]
FLOW_UNITS           CFS
INFILTRATION         HORTON
FLOW_ROUTING         KINWAVE
START_DATE           01/01/2026
START_TIME           00:00:00
END_DATE             01/01/2026
END_TIME             06:00:00
REPORT_STEP          00:15:00
WET_STEP             00:01:00
DRY_STEP             01:00:00
ROUTING_STEP         0:00:30
)";

const char* kDeckBody = R"(
[EVAPORATION]
CONSTANT             0.1
DRY_ONLY             NO

[RAINGAGES]
RG1  INTENSITY  0:05  1.0  TIMESERIES  TS1

[SUBCATCHMENTS]
S1  RG1  O1  10  50  500  1.0  0

[SUBAREAS]
S1  0.012  0.1  0.05  0.1  25  OUTLET

[INFILTRATION]
S1  3.0  0.5  4  7  0

[OUTFALLS]
O1  0  FREE  NO

[TIMESERIES]
TS1  0:00  0.0
TS1  0:15  1.0
TS1  1:00  2.0
TS1  1:45  1.0
TS1  2:00  0.0
TS1  6:00  0.0
)";

fs::path artifactDir() {
    const fs::path d = fs::current_path() / "test_runoff_ode_exact_artifacts";
    fs::create_directories(d);
    return d;
}

fs::path writeDeck(const char* name, const std::string& text) {
    const fs::path p = artifactDir() / name;
    std::ofstream(p) << text;
    return p;
}

struct OpenedDeck {
    SWMM_Engine e = nullptr;
    int rc = -1;
    explicit OpenedDeck(const fs::path& inp) {
        e = swmm_engine_create();
        const fs::path rpt = fs::path(inp).replace_extension(".rpt");
        const fs::path out = fs::path(inp).replace_extension(".out");
        rc = swmm_engine_open(e, inp.string().c_str(), rpt.string().c_str(), out.string().c_str(),
                              nullptr);
    }
    ~OpenedDeck() {
        if (e) {
            swmm_engine_close(e);
            swmm_engine_destroy(e);
        }
    }
};

std::string getOption(SWMM_Engine e, const char* key) {
    char buf[64] = {};
    if (swmm_options_get(e, key, buf, static_cast<int>(sizeof buf)) != SWMM_OK)
        return "<ERR>";
    return buf;
}

openswmm::SWMMEngine& cpp(SWMM_Engine e) {
    return *static_cast<openswmm::SWMMEngine*>(e);
}

/// Total runoff volume (ft3) of the one subcatchment after a full run; false on any failure.
bool runDeck(const fs::path& inp, double& volume) {
    OpenedDeck d(inp);
    EXPECT_EQ(d.rc, SWMM_OK) << "open";
    if (d.rc != SWMM_OK)
        return false;
    EXPECT_EQ(swmm_engine_initialize(d.e), SWMM_OK) << swmm_get_last_error_msg(d.e);
    EXPECT_EQ(swmm_engine_start(d.e, 1), SWMM_OK) << swmm_get_last_error_msg(d.e);
    double elapsed = 1.0;
    int rc = SWMM_OK;
    for (int guard = 0; guard < 100000 && elapsed > 0.0 && rc == SWMM_OK; ++guard)
        rc = swmm_engine_step(d.e, &elapsed);
    EXPECT_EQ(rc, SWMM_OK);
    EXPECT_EQ(swmm_engine_end(d.e), SWMM_OK);
    volume = cpp(d.e).context().subcatches.stat_runoff_vol[0];
    return rc == SWMM_OK;
}

} // namespace

TEST(RunoffOdeSolverOption, ParsesDefaultsAndRoundTripsThroughTheApi) {
    {
        OpenedDeck d(writeDeck("default.inp", std::string(kOptionsHead) + kDeckBody));
        ASSERT_EQ(d.rc, SWMM_OK);
        EXPECT_EQ(getOption(d.e, "RUNOFF_ODE_SOLVER"), "LEGACY");
        EXPECT_EQ(cpp(d.e).context().options.runoff_ode_solver, openswmm::RunoffOdeSolver::LEGACY);
        EXPECT_EQ(swmm_options_set(d.e, "RUNOFF_ODE_SOLVER", "exact"), SWMM_OK);
        EXPECT_EQ(getOption(d.e, "RUNOFF_ODE_SOLVER"), "EXACT");
        EXPECT_EQ(cpp(d.e).context().options.runoff_ode_solver, openswmm::RunoffOdeSolver::EXACT);
        EXPECT_NE(swmm_options_set(d.e, "RUNOFF_ODE_SOLVER", "RK45"), SWMM_OK);
        EXPECT_EQ(getOption(d.e, "RUNOFF_ODE_SOLVER"), "EXACT")
            << "a rejected value changes nothing";
        EXPECT_EQ(swmm_options_set(d.e, "RUNOFF_ODE_SOLVER", "LEGACY"), SWMM_OK);
        EXPECT_EQ(getOption(d.e, "RUNOFF_ODE_SOLVER"), "LEGACY");
    }
    {
        OpenedDeck d(writeDeck("exact.inp", std::string(kOptionsHead) +
                                                "RUNOFF_ODE_SOLVER    EXACT\n" + kDeckBody));
        ASSERT_EQ(d.rc, SWMM_OK);
        EXPECT_EQ(getOption(d.e, "RUNOFF_ODE_SOLVER"), "EXACT");
    }
    {
        OpenedDeck d(writeDeck("bad.inp", std::string(kOptionsHead) +
                                              "RUNOFF_ODE_SOLVER    RK45\n" + kDeckBody));
        EXPECT_NE(d.rc, SWMM_OK) << "an unknown solver word is an input error";
    }
}

TEST(RunoffOdeSolverOption, SmallDeckRunoffAgreesWithLegacyToTheRk45Tolerance) {
    const fs::path legacy = writeDeck("run_legacy.inp", std::string(kOptionsHead) + kDeckBody);
    const fs::path exact = writeDeck(
        "run_exact.inp", std::string(kOptionsHead) + "RUNOFF_ODE_SOLVER    EXACT\n" + kDeckBody);
    double v_legacy = 0.0, v_exact = 0.0;
    ASSERT_TRUE(runDeck(legacy, v_legacy));
    ASSERT_TRUE(runDeck(exact, v_exact));
    ASSERT_GT(v_legacy, 0.0);
    const double rel = std::fabs(v_exact - v_legacy) / v_legacy;
    EXPECT_LT(rel, 2e-3) << "EXACT differs from LEGACY by LEGACY's solver error at most";
    EXPECT_NE(v_exact, v_legacy) << "the two integrators are not the same arithmetic";
    // the report names the solver only when it is not the default
    std::ifstream rl(fs::path(legacy).replace_extension(".rpt")),
        re(fs::path(exact).replace_extension(".rpt"));
    const std::string tl((std::istreambuf_iterator<char>(rl)), std::istreambuf_iterator<char>());
    const std::string te((std::istreambuf_iterator<char>(re)), std::istreambuf_iterator<char>());
    EXPECT_EQ(tl.find("Runoff ODE Solver"), std::string::npos);
    EXPECT_NE(te.find("Runoff ODE Solver ........ EXACT"), std::string::npos);
}
