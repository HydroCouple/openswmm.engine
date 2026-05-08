/**
 * @file test_ode_solver.cpp
 * @brief Unit and benchmark tests for the ODE integrator and root-finder.
 *
 * @see src/engine/math/OdeSolver.hpp
 * @see src/engine/math/FindRoot.hpp
 * @ingroup engine_math
 */

#include <gtest/gtest.h>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "math/OdeSolver.hpp"
#include "math/FindRoot.hpp"

// ============================================================================
// ODE Solver basic tests
// ============================================================================

TEST(OdeSolver, ExponentialDecay) {
    // dy/dt = -y, y(0) = 1 → y(t) = exp(-t)
    double y = 1.0;
    auto derivs = [](double /*x*/, const double* y, double* dydx) {
        dydx[0] = -y[0];
    };

    int rc = openswmm::ode::integrate(&y, 1, 0.0, 1.0, 1e-6, 0.1, derivs);
    EXPECT_EQ(rc, 0);
    EXPECT_NEAR(y, std::exp(-1.0), 1e-5);
}

TEST(OdeSolver, LinearGrowth) {
    // dy/dt = 1, y(0) = 0 → y(t) = t
    double y = 0.0;
    auto derivs = [](double /*x*/, const double* /*y*/, double* dydx) {
        dydx[0] = 1.0;
    };

    int rc = openswmm::ode::integrate(&y, 1, 0.0, 5.0, 1e-6, 0.5, derivs);
    EXPECT_EQ(rc, 0);
    EXPECT_NEAR(y, 5.0, 1e-10);
}

TEST(OdeSolver, HarmonicOscillator) {
    // dy1/dt = y2, dy2/dt = -y1 → y1(t) = cos(t), y2(t) = -sin(t)
    double y[2] = {1.0, 0.0};  // y1(0)=1, y2(0)=0
    auto derivs = [](double /*x*/, const double* y, double* dydx) {
        dydx[0] = y[1];
        dydx[1] = -y[0];
    };

    int rc = openswmm::ode::integrate(y, 2, 0.0, 2.0 * 3.14159265, 1e-8, 0.01, derivs);
    EXPECT_EQ(rc, 0);
    // After one full period, should return to initial state
    EXPECT_NEAR(y[0], 1.0, 1e-5);
    EXPECT_NEAR(y[1], 0.0, 1e-5);
}

TEST(OdeSolver, QuadraticGrowth) {
    // dy/dt = 2*t, y(0) = 0 → y(t) = t²
    double y = 0.0;
    auto derivs = [](double x, const double* /*y*/, double* dydx) {
        dydx[0] = 2.0 * x;
    };

    int rc = openswmm::ode::integrate(&y, 1, 0.0, 3.0, 1e-8, 0.1, derivs);
    EXPECT_EQ(rc, 0);
    EXPECT_NEAR(y, 9.0, 1e-6);
}

// ============================================================================
// FindRoot basic tests
// ============================================================================

TEST(FindRoot, NewtonSquareRoot) {
    // Find x such that x^2 - 2 = 0 → x = sqrt(2)
    double root = 1.5;
    auto func = [](double x, double* f, double* df) {
        *f = x * x - 2.0;
        *df = 2.0 * x;
    };
    int iters = openswmm::findroot::newton(0.1, 10.0, &root, 1e-10, func);
    EXPECT_GT(iters, 0);
    EXPECT_NEAR(root, std::sqrt(2.0), 1e-10);
}

TEST(FindRoot, RidderCubicRoot) {
    // Find x such that x^3 - 8 = 0 → x = 2
    auto func = [](double x) -> double { return x * x * x - 8.0; };
    double root = openswmm::findroot::ridder(0.0, 5.0, 1e-10, func);
    EXPECT_NEAR(root, 2.0, 1e-8);
}

TEST(FindRoot, NewtonLinearExact) {
    // f(x) = 2x - 6 = 0 → x = 3
    double root = 1.0;
    auto func = [](double x, double* f, double* df) {
        *f = 2.0 * x - 6.0;
        *df = 2.0;
    };
    int iters = openswmm::findroot::newton(0.0, 10.0, &root, 1e-12, func);
    EXPECT_GT(iters, 0);
    EXPECT_NEAR(root, 3.0, 1e-12);
}

TEST(FindRoot, RidderTrigRoot) {
    // Find x such that sin(x) = 0 near x = pi
    auto func = [](double x) -> double { return std::sin(x); };
    double root = openswmm::findroot::ridder(2.5, 3.8, 1e-10, func);
    EXPECT_NEAR(root, 3.14159265358979, 1e-8);
}

// ============================================================================
// Benchmark trajectory tests
//
// These tests load reference datasets from tests/benchmarks/manufactured/ and
// compare the solver output against analytical reference values.
//
// BENCHMARK_DATA_DIR is injected by CMake as an absolute path.
// If it is absent or the file is missing, the test is skipped rather than
// failing so that builds without the benchmark tree still pass.
// ============================================================================

#ifndef BENCHMARK_DATA_DIR
#  define BENCHMARK_DATA_DIR ""
#endif

namespace {

struct OdeBenchRow      { double t_s; double y_exact; };
struct LogisticBenchRow { double t_s; double y_exact; };
struct SIRBenchRow      { double t_s; double S; double I; double R; };

static std::vector<OdeBenchRow> load_ode_bench(const std::string& path) {
    std::vector<OdeBenchRow> rows;
    std::ifstream in(path);
    if (!in.is_open()) return rows;
    std::string line;
    bool header_seen = false;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (!header_seen) { header_seen = true; continue; }
        // columns: t_s, y_exact
        std::istringstream ss(line);
        std::string tok;
        double vals[2] = {};
        int col = 0;
        while (std::getline(ss, tok, ',') && col < 2)
            vals[col++] = std::stod(tok);
        if (col >= 2)
            rows.push_back({vals[0], vals[1]});
    }
    return rows;
}

static std::vector<LogisticBenchRow> load_logistic_bench(const std::string& path) {
    std::vector<LogisticBenchRow> rows;
    std::ifstream in(path);
    if (!in.is_open()) return rows;
    std::string line;
    bool header_seen = false;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (!header_seen) { header_seen = true; continue; }
        std::istringstream ss(line);
        std::string tok;
        double vals[2] = {};
        int col = 0;
        while (std::getline(ss, tok, ',') && col < 2)
            vals[col++] = std::stod(tok);
        if (col >= 2)
            rows.push_back({vals[0], vals[1]});
    }
    return rows;
}

static std::vector<SIRBenchRow> load_sir_bench(const std::string& path) {
    std::vector<SIRBenchRow> rows;
    std::ifstream in(path);
    if (!in.is_open()) return rows;
    std::string line;
    bool header_seen = false;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (!header_seen) { header_seen = true; continue; }
        std::istringstream ss(line);
        std::string tok;
        double vals[4] = {};
        int col = 0;
        while (std::getline(ss, tok, ',') && col < 4)
            vals[col++] = std::stod(tok);
        if (col >= 4)
            rows.push_back({vals[0], vals[1], vals[2], vals[3]});
    }
    return rows;
}

}  // namespace

// ODE exponential-decay grid walk against manufactured benchmark.
//
// integrate() is called across successive sub-intervals; the endpoint value is
// compared against y(t) = exp(-0.5*t) at each reference grid point.
TEST(OdeSolver, ExponentialDecayTrajectory) {
    std::string path = std::string(BENCHMARK_DATA_DIR)
        + "/manufactured/odesolve-exponential-decay/reference.csv";

    auto rows = load_ode_bench(path);
    if (rows.empty()) {
        GTEST_SKIP() << "Benchmark data not found: " << path;
    }

    auto derivs = [](double /*x*/, const double* y, double* dydx) {
        dydx[0] = -0.5 * y[0];
    };

    double y      = 1.0;
    double max_err = 0.0, sum_sq = 0.0;
    double prev_t  = rows[0].t_s;  // t=0; y(0)=1 matches reference

    for (size_t i = 1; i < rows.size(); ++i) {
        int rc = openswmm::ode::integrate(&y, 1, prev_t, rows[i].t_s,
                                          1e-6, 0.1, derivs);
        ASSERT_EQ(rc, 0) << "ODE integrator failed at t=" << rows[i].t_s;

        double err = std::abs(y - rows[i].y_exact);
        max_err    = std::max(max_err, err);
        sum_sq    += err * err;
        prev_t     = rows[i].t_s;
    }

    double rms_err = std::sqrt(sum_sq / static_cast<double>(rows.size() - 1));
    EXPECT_LT(max_err, 1e-5)
        << "ODE exponential-decay max error " << max_err
        << " exceeds 1e-5 tolerance (benchmark: " << path << ")";
    EXPECT_LT(rms_err, 1e-6)
        << "ODE exponential-decay RMS error " << rms_err
        << " exceeds 1e-6 tolerance";
}

// Logistic growth ODE trajectory against manufactured benchmark.
//
// Integrates dy/dt = 0.5*y*(1-y) from y(0)=0.1 across successive sub-intervals
// and compares against y(t) = 1/(1+9*exp(-0.5*t)).
// RK45 with eps=1e-6 should deliver global error well below 1e-4 on this smooth
// nonlinear trajectory; any larger deviation indicates an integrator regression.
TEST(OdeSolver, LogisticGrowthTrajectory) {
    std::string path = std::string(BENCHMARK_DATA_DIR)
        + "/manufactured/odesolve-logistic-growth/reference.csv";

    auto rows = load_logistic_bench(path);
    if (rows.empty()) {
        GTEST_SKIP() << "Benchmark data not found: " << path;
    }

    auto derivs = [](double /*x*/, const double* y, double* dydx) {
        dydx[0] = 0.5 * y[0] * (1.0 - y[0]);
    };

    double y      = rows[0].y_exact;  // 0.1
    double max_err = 0.0;
    double prev_t  = rows[0].t_s;     // t=0

    for (size_t i = 1; i < rows.size(); ++i) {
        int rc = openswmm::ode::integrate(&y, 1, prev_t, rows[i].t_s,
                                          1e-6, 0.1, derivs);
        ASSERT_EQ(rc, 0) << "ODE integrator failed at t=" << rows[i].t_s;

        double err = std::abs(y - rows[i].y_exact);
        max_err    = std::max(max_err, err);
        prev_t     = rows[i].t_s;
    }

    EXPECT_LT(max_err, 1e-4)
        << "ODE logistic-growth max error " << max_err
        << " exceeds 1e-4 tolerance (benchmark: " << path << ")";
}

// SIR epidemic ODE — invariant, phase-plane, and qualitative dynamics.
//
// Tests the 3-variable coupled ODE path of integrate(), which all prior ODE
// tests bypass (they pass n=1).  Three mathematically exact properties are
// checked without needing a reference CSV:
//
//   1. Conservation: S(t) + I(t) + R(t) = N = 1, exact from ODE structure.
//   2. Phase-plane: S = S0 * exp(-R0 * R), derived from dS/dR = -R0*S.
//   3. Qualitative dynamics: S decreasing, R increasing, epidemic peak occurs,
//      S_final < herd-immunity threshold gamma/beta.
//
// These three checks together verify that the coupling terms, the signs, and
// the multi-variable step rejection logic are all correct.  They are sensitive
// to bugs that a trajectory comparison might miss (e.g. a wrong beta/gamma
// ratio that still produces a plausible-looking curve).
TEST(OdeSolver, SIREpidemicInvariantsAndDynamics) {
    const double beta  = 0.3;
    const double gamma_rate = 0.1;
    const double R0_basic   = beta / gamma_rate;  // = 3.0
    const double N     = 1.0;

    double y[3] = {0.99, 0.01, 0.0};  // [S, I, R]
    const double S0 = y[0];

    auto sir_rhs = [&](double /*t*/, const double* y, double* dydx) {
        const double force = beta * y[0] * y[1] / N;
        dydx[0] = -force;
        dydx[1] =  force - gamma_rate * y[1];
        dydx[2] =  gamma_rate * y[1];
    };

    double max_conserv_err = 0.0;
    double max_phase_err   = 0.0;
    double prev_I = y[1];
    bool   peak_seen = false;

    // Run to t=120: with R0=3, γ=0.1 the epidemic peaks near t=30 and I drops
    // below 1% only after t≈80 (recovery period 1/γ=10 units).
    for (int step = 0; step < 120; ++step) {
        const double t = static_cast<double>(step);
        int rc = openswmm::ode::integrate(y, 3, t, t + 1.0, 1e-6, 0.1, sir_rhs);
        ASSERT_EQ(rc, 0) << "ODE integrator failed at t=" << t;

        // 1. Conservation invariant (machine-precision expectation)
        max_conserv_err = std::max(max_conserv_err,
                                   std::abs(y[0] + y[1] + y[2] - N));

        // 2. Phase-plane relationship S = S0 * exp(-R0 * R)
        max_phase_err = std::max(max_phase_err,
                                 std::abs(y[0] - S0 * std::exp(-R0_basic * y[2])));

        // 3a. Epidemic peak detection
        if (y[1] < prev_I) peak_seen = true;
        prev_I = y[1];
    }

    // 1. Conservation to near machine epsilon
    EXPECT_LT(max_conserv_err, 1e-10)
        << "SIR conservation S+I+R violated: max |S+I+R-1| = " << max_conserv_err;

    // 2. Phase-plane trajectory consistent with exact dS/dR = -R0*S
    EXPECT_LT(max_phase_err, 1e-4)
        << "SIR phase-plane S=S0*exp(-R0*R) max error = " << max_phase_err;

    // 3b. Epidemic peak occurred (I went up then came down)
    EXPECT_TRUE(peak_seen) << "Epidemic peak (dI/dt=0) not observed over [0,120]";

    // 3c. Herd immunity overshoot: final S below gamma/beta threshold
    EXPECT_LT(y[0], gamma_rate / beta)
        << "S(120) = " << y[0] << " should be below herd-immunity threshold "
        << gamma_rate / beta;

    // 3d. Epidemic resolved: I small at t=120
    EXPECT_LT(y[1], 1e-2)
        << "I(120) = " << y[1] << " should be near zero at end of epidemic";
}

// SIR epidemic trajectory comparison against Flash-X-generated reference.
//
// Skipped when reference.csv contains only the placeholder t=0 row.
// To activate, populate reference.csv by running
// tools/generate_sir_reference.py or the Flash-X CashKarp45 driver.
TEST(OdeSolver, SIRTrajectoryMatchesBenchmark) {
    std::string path = std::string(BENCHMARK_DATA_DIR)
        + "/manufactured/odesolve-sir-epidemic/reference.csv";

    auto rows = load_sir_bench(path);
    if (rows.size() < 2) {
        GTEST_SKIP() << "SIR reference trajectory not yet populated: " << path;
    }

    const double beta  = 0.3;
    const double gamma_rate = 0.1;
    const double N     = 1.0;

    double y[3] = {rows[0].S, rows[0].I, rows[0].R};

    auto sir_rhs = [&](double /*t*/, const double* y, double* dydx) {
        const double force = beta * y[0] * y[1] / N;
        dydx[0] = -force;
        dydx[1] =  force - gamma_rate * y[1];
        dydx[2] =  gamma_rate * y[1];
    };

    double max_err_S = 0.0, max_err_I = 0.0, max_err_R = 0.0;
    double prev_t = rows[0].t_s;

    for (size_t i = 1; i < rows.size(); ++i) {
        int rc = openswmm::ode::integrate(y, 3, prev_t, rows[i].t_s,
                                          1e-6, 0.1, sir_rhs);
        ASSERT_EQ(rc, 0) << "ODE integrator failed at t=" << rows[i].t_s;

        max_err_S = std::max(max_err_S, std::abs(y[0] - rows[i].S));
        max_err_I = std::max(max_err_I, std::abs(y[1] - rows[i].I));
        max_err_R = std::max(max_err_R, std::abs(y[2] - rows[i].R));
        prev_t = rows[i].t_s;
    }

    EXPECT_LT(max_err_S, 1e-3) << "SIR S max error " << max_err_S << " (benchmark: " << path << ")";
    EXPECT_LT(max_err_I, 1e-3) << "SIR I max error " << max_err_I;
    EXPECT_LT(max_err_R, 1e-3) << "SIR R max error " << max_err_R;
}
