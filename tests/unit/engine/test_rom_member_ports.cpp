/**
 * @file test_rom_member_ports.cpp
 * @brief Per-member stores and ports (PORT_HAMILTONIAN phase 5,
 *        docs/uncertainty/PH_STORES_AND_PORTS.md §6 steps 1–3).
 *
 * Laws (RomStorePorts.hpp): monotone, anchored, the raised-end law only
 * downhill, monotone envelopes, the (b1) pump. Solver (RomPortGraph.hpp):
 * l1 contraction, order preservation, conservation, zero data, linear limit.
 * Engine: per-member stores off ⇒ C ≡ 0; deterministic run bit-identical.
 *
 * The quadratic energy ½ zᵀ S z is deliberately not tested: it is not monotone
 * for one-way ports (note §2.2).
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "uncertainty/RomStorePorts.hpp"
#include "uncertainty/RomPortGraph.hpp"
#include "uncertainty/RomAdditiveChannel.hpp"

#ifdef OPENSWMM_HAS_2D
#include "openswmm/engine/openswmm_engine.h"
#include "openswmm/engine/openswmm_2d.h"
#include "core/SWMMEngine.hpp"
#include "uncertainty/SpectralROM1D.hpp"
#endif

#if defined(_WIN32)
#  include <process.h>
#  define getpid _getpid
#else
#  include <unistd.h>
#endif

using namespace openswmm::uncertainty;
namespace P = openswmm::uncertainty::ports;
using P::RatingTable;
using P::clusteredOffsets;

namespace {

// ---------------------------------------------------------------------------
// Random monotone port graphs
// ---------------------------------------------------------------------------
struct RandomGraph {
    PortGraph g;
    std::vector<double> c3;   // cubic store coefficient per store
};

// A one-way weir-like port: Q = c·max(a + z, 0)^1.5, anchored.
PortGraph::Port weirPort(int up, int dn, double c, double a) {
    PortGraph::Port p;
    p.up = up; p.dn = dn;
    p.dq = [c, a](int, double zu, double) {
        if (zu == 0.0) return 0.0;
        return c * (std::pow(std::max(a + zu, 0.0), 1.5) - std::pow(a, 1.5));
    };
    return p;
}

// A two-way raised-end port between B and F, anchored at the main-run heads.
PortGraph::Port raisedPort(int up, int dn, double K, double hB, double zB, double hF, double zF, bool legacy = false) {
    PortGraph::Port p;
    p.up = up; p.dn = dn;
    auto law = legacy ? &P::raisedEndFlowLegacyKind6 : &P::raisedEndFlow;
    const double q0 = law(K, hB, zB, hF, zF);
    p.dq = [=](int, double zu, double zd) {
        if (zu == 0.0 && zd == 0.0) return 0.0;
        return law(K, hB + zu, zB, hF + zd, zF) - q0;
    };
    return p;
}

// The street exchange at a coupled manhole (O-3): an orifice two-port from a
// network node to its own pond store, anchored at the main run (the same law at
// z = 0), floored by each side's water.
constexpr double kTwoG = 64.348;   // 2g, ft/s^2
PortGraph::Port orificePort(int mh, int pond, double c, double h_mh, double d_mh, double z_or, double p_det) {
    const double q_det = P::orificePortFlow(c, kTwoG, h_mh, z_or, p_det, P::kDelta0, P::ramp(d_mh), P::ramp(p_det));
    PortGraph::Port p;
    p.up = mh; p.dn = pond;
    p.dq = [=](int, double zu, double zp) {
        if (zu == 0.0 && zp == 0.0) return 0.0;
        return P::orificePortFlow(c, kTwoG, h_mh + zu, z_or, p_det + zp, P::kDelta0, P::ramp(d_mh + zu), P::ramp(p_det + zp)) - q_det;
    };
    return p;
}

// Attach a street pond store (linear, a·z) to network node mh; returns the pond node.
int addPond(PortGraph& g, int mh, double a, double c, double h_mh, double d_mh, double z_or, double p_det) {
    const int pn = g.addNode(a);
    P::PondStore ps; ps.a = a; ps.p_star = p_det;
    PortGraph::Store st;
    st.dV = [ps](double z, double* ar) { return ps.dV(z, ar); };
    g.store_of[static_cast<std::size_t>(pn)] = static_cast<int>(g.stores.size());
    g.stores.push_back(st);
    g.ports.push_back(orificePort(mh, pn, c, h_mh, d_mh, z_or, p_det));
    return pn;
}

RandomGraph makeRandomGraph(std::mt19937& rng, int n, bool ponds = false) {
    std::uniform_real_distribution<double> U(0.0, 1.0);
    RandomGraph r;
    PortGraph& g = r.g;
    g.resize(n);
    g.dt = 5.0 + 50.0 * U(rng);
    for (int t = 0; t < n; ++t) g.S[static_cast<std::size_t>(t)] = std::pow(10.0, 1.0 + 3.0 * U(rng));
    // About a third of the nodes are nonlinear stores: dV = S z + c z^3 (strictly increasing).
    for (int t = 0; t < n; ++t) {
        if (U(rng) > 0.35) continue;
        const double S = g.S[static_cast<std::size_t>(t)];
        const double c = S * U(rng);
        g.store_of[static_cast<std::size_t>(t)] = static_cast<int>(g.stores.size());
        PortGraph::Store s;
        s.dV = [S, c](double z, double* a) { if (a) *a = S + 3.0 * c * z * z; return S * z + c * z * z * z; };
        g.stores.push_back(s);
        r.c3.push_back(c);
    }
    // Spanning chain plus random extra links: plane edges, weirs, raised-end pipes; outlets at the end.
    for (int t = 1; t < n; ++t) {
        const int up = t - 1 - static_cast<int>(U(rng) * std::min(t, 3));
        const int dn = t;
        const double kind = U(rng);
        if (kind < 0.4) {
            FlowEdge e; e.up = up; e.dn = dn; e.conduit = true;
            e.ju = 0.1 + 5.0 * U(rng); e.jd = -e.ju * U(rng);
            g.edges.push_back(e);
        } else if (kind < 0.7) {
            g.ports.push_back(weirPort(up, dn, 0.5 + 3.0 * U(rng), 0.05 + U(rng)));
        } else {
            const double zB = U(rng), zF = zB + 0.5 * U(rng);
            g.ports.push_back(raisedPort(up, dn, 1.0 + 4.0 * U(rng), zB + 0.5 * U(rng), zB, zF + U(rng) - 0.5, zF));
        }
    }
    for (int k = 0; k < n / 2; ++k) {   // loops
        const int a = static_cast<int>(U(rng) * n), c = static_cast<int>(U(rng) * n);
        if (a == c) continue;
        g.ports.push_back(raisedPort(std::min(a, c), std::max(a, c), 2.0 * U(rng), 1.0, 0.5, 0.8 + 0.4 * U(rng), 0.6));
    }
    g.ports.push_back(weirPort(n - 1, -1, 2.0, 0.3));      // outfall
    FlowEdge out; out.up = n / 2; out.dn = -1; out.conduit = false; out.d = 0.5;
    g.edges.push_back(out);
    if (ponds) {
        // About a quarter of the network nodes are coupled manholes with a street
        // pond: manhole above, near or below the street; pond wet or dry.
        for (int t = 0; t < n; ++t) {
            if (U(rng) > 0.25) continue;
            const double z_or = 1.0 + 0.5 * U(rng);
            const double h_mh = z_or + 0.6 * (U(rng) - 0.5);
            const double p_det = U(rng) < 0.3 ? 0.0 : 0.4 * U(rng);
            addPond(g, t, std::pow(10.0, 1.0 + 3.0 * U(rng)), 0.05 + 0.5 * U(rng), h_mh, 0.5 + U(rng), z_or, p_det);
        }
    }
    g.dV_prev.assign(g.stores.size(), 0.0);
    return r;
}

double sumDV(const PortGraph& g, const std::vector<double>& z) {
    double s = 0.0;
    for (int t = 0; t < g.n; ++t) if (!g.pinned[static_cast<std::size_t>(t)]) s += g.volume(t, z[static_cast<std::size_t>(t)], nullptr) - g.volumePrev(t);
    return s;
}

}  // namespace

// ===========================================================================
// Step 1 — port library
// ===========================================================================

TEST(StorePorts, StorePortsAreMonotoneAndConservative) {
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> U(-2.0, 2.0);
    const double h = 1.0e-6;
    int pos = 0, neg = 0;
    for (int k = 0; k < 20000; ++k) {
        const double hB = U(rng), zB = U(rng), hF = U(rng), zF = U(rng), K = 3.0;
        const double q = P::raisedEndFlow(K, hB, zB, hF, zF);
        // (P1) monotone in each argument
        EXPECT_GE(P::raisedEndFlow(K, hB + h, zB, hF, zF) - q, -1e-12);
        EXPECT_LE(P::raisedEndFlow(K, hB, zB, hF + h, zF) - q, 1e-12);
        // Q_BF > 0 only when h_B > max(h_F, z_F); Q_FB > 0 only when h_F > max(h_B, z_B)
        if (q > 0.0) { ++pos; EXPECT_GT(hB, std::max(hF, zF)); }
        if (q < 0.0) { ++neg; EXPECT_GT(hF, std::max(hB, zB)); }
        // (P2): the law is antisymmetric in its ends (B->F = -(F->B))
        EXPECT_DOUBLE_EQ(q, -P::raisedEndFlow(K, hF, zF, hB, zB));
        // free-end conduit: nondecreasing in the member's upstream conveyance and head
        const double H0 = 0.2 + std::fabs(U(rng)), H = H0 + U(rng);
        const double f = P::freeEndFlow(1.5, 2.0, 2.0, H, H0, true);
        EXPECT_GE(P::freeEndFlow(1.5, 2.0, 2.0, H + h, H0, true) - f, -1e-12);
        EXPECT_GE(f, 0.0);
    }
    EXPECT_GT(pos, 1000);
    EXPECT_GT(neg, 1000);
    // (P3) anchors: exactly the main run's flow at zero offset
    EXPECT_EQ(P::anchoredRatedFlow(3.0, 3.0, 2.5, 1.0, 0.0), 2.5);
    EXPECT_EQ(P::freeEndFlow(1.7, 0.9, 0.9, 0.4, 0.4, true), 1.7);
    EXPECT_EQ(P::overflowFlow(0.0, 100.0, 0.5, 0.0), 0.0);
    EXPECT_EQ(P::overflowFlow(2.0, 100.0, -0.1, 0.0), 2.0);
}

TEST(StorePorts, RaisedEndLawOnlyDownhillAndLegacyLawFails) {
    // Mutation: today's kind-6 law sign(Δ)·sqrt|Δ| on the clamped heads drains
    // B while both nodes stand below their pipe ends.
    const double K = 2.0, zB = 1.0, zF = 2.0;
    const double hB = 0.5, hF = 0.2;   // both below their ends
    EXPECT_EQ(P::raisedEndFlow(K, hB, zB, hF, zF), 0.0);
    const double legacy = P::raisedEndFlowLegacyKind6(K, hB, zB, hF, zF);
    EXPECT_NE(legacy, 0.0) << "the legacy law is supposed to show the phantom";
    EXPECT_FALSE(legacy < 0.0 ? hF > std::max(hB, zB) : hB > std::max(hF, zF));
    // A main run above z_B, a member below it: the legacy law keeps a constant flow.
    const double l1 = P::raisedEndFlowLegacyKind6(K, 0.9, 1.5, 0.1, 1.0);
    const double l2 = P::raisedEndFlowLegacyKind6(K, 0.2, 1.5, 0.1, 1.0);
    EXPECT_EQ(l1, l2);
    EXPECT_GT(l1, 0.0);
    EXPECT_EQ(P::raisedEndFlow(K, 0.2, 1.5, 0.1, 1.0), 0.0);
    // Regularised slope: finite as the pair becomes level (Δ -> 0+).
    double smax = 0.0;
    for (double d = 1e-9; d < 0.2; d *= 1.3) {
        const double s = (P::raisedEndFlow(K, 3.0 + d + 1e-10, 0.0, 3.0, 0.0) - P::raisedEndFlow(K, 3.0 + d, 0.0, 3.0, 0.0)) / 1e-10;
        smax = std::max(smax, s);
    }
    EXPECT_LE(smax, 1.01 * K / std::sqrt(P::kDelta0));
    EXPECT_TRUE(std::isfinite(smax));
}

TEST(StorePorts, MonotoneEnvelopeOfKickbackBrake) {
    // A vortex flow brake: rises to 12 L/s, kicks back to 8, rises again.
    RatingTable T;
    T.y = {-1.0, -0.5, -0.2, 0.0, 0.2, 0.5, 1.0, 1.5};
    T.q = {0.0, 0.20, 0.38, 0.42, 0.30, 0.28, 0.40, 0.55};
    // Mutation: the raw table is not monotone.
    bool raw_mono = true;
    for (std::size_t k = 1; k < T.q.size(); ++k) raw_mono &= T.q[k] >= T.q[k - 1];
    EXPECT_FALSE(raw_mono);
    T.makeMonotone();
    EXPECT_EQ(T.raised, 3);
    for (std::size_t k = 1; k < T.q.size(); ++k) EXPECT_GE(T.q[k], T.q[k - 1]);
    // Q~(h) = max_{s <= h} Q(s): the kickback is held at the 0.42 peak.
    EXPECT_DOUBLE_EQ(T.eval(0.3), 0.42);
    EXPECT_DOUBLE_EQ(T.eval(1.0), 0.42);
    EXPECT_DOUBLE_EQ(T.eval(1.5), 0.55);
    for (double y = -2.0; y < 3.0; y += 0.01) EXPECT_GE(T.eval(y + 0.01), T.eval(y));
    EXPECT_GE(T.eval(5.0), 0.55);   // extrapolated upward with the last slope
    EXPECT_DOUBLE_EQ(T.eval(-5.0), 0.0);
}

TEST(StorePorts, FrozenSettingPumpRunsOnItsCurveAtMemberDepth) {
    // (b1): the member's pump runs at the main run's setting sigma*, curve at the
    // member's depth; the dry ramp is applied at the member's own depth.
    auto curve = [](double d) { return d < 0.5 ? 1.0 : 2.0; };   // PUMP2 step curve (cfs)
    auto build = [&](double sigma, double d_star, double Y) {
        RatingTable T;
        T.y = clusteredOffsets(Y);
        for (double y : T.y) T.q.push_back(sigma * curve(std::max(d_star + y, 0.0)));
        T.makeMonotone();
        return T;
    };
    auto law = [](const RatingTable& T, double d_star, double y) { return T.eval(y) * P::ramp(d_star + y); };
    const double d_star = 0.8;
    for (double sigma : {0.0, 1.0}) {
        const RatingTable T = build(sigma, d_star, 1.0);
        const double T0 = law(T, d_star, 0.0), q_star = sigma * curve(d_star);
        for (double y = -1.2; y <= 1.0; y += 0.05) {
            const double q = P::anchoredRatedFlow(law(T, d_star, y), T0, q_star, d_star, y);
            if (sigma == 0.0) EXPECT_EQ(q, 0.0) << "a wetter member cannot start a pump the main run has off";
            if (sigma == 1.0 && y > 0.0) EXPECT_NEAR(q, law(T, d_star, y), 1e-12);
            EXPECT_GE(q, 0.0);
            EXPECT_GE(P::anchoredRatedFlow(law(T, d_star, y + 0.01), T0, q_star, d_star, y + 0.01), q);
            if (d_star + y <= 0.0) EXPECT_EQ(q, 0.0) << "a dry member pumps nothing";
        }
    }
    // An empty wet well in the main run pumps only what arrives (q* < curve):
    // a drier member passes less, to zero once dry; a wetter one reaches the curve within δ_dry.
    const double d0 = 0.004, q_in = 0.3;
    const RatingTable T = build(1.0, d0, 0.5);
    const double T0 = law(T, d0, 0.0);
    EXPECT_GT(T0, q_in);
    EXPECT_LE(P::anchoredRatedFlow(law(T, d0, -0.002), T0, q_in, d0, -0.002), q_in);
    EXPECT_EQ(P::anchoredRatedFlow(law(T, d0, -0.02), T0, q_in, d0, -0.02), 0.0);
    EXPECT_NEAR(P::anchoredRatedFlow(law(T, d0, 0.02), T0, q_in, d0, 0.02), law(T, d0, 0.02), 1e-12);
}

// ===========================================================================
// Step 2 — M-function Newton on the port graph
// ===========================================================================

TEST(PortGraph, PortGraphZeroDataIsExact) {
    std::mt19937 rng(11);
    RandomGraph r = makeRandomGraph(rng, 30);
    AdditiveSystem sys;
    std::vector<double> z(30, 0.0);
    ASSERT_TRUE(r.g.solve(z, sys));
    for (double v : z) EXPECT_EQ(v, 0.0);
    EXPECT_EQ(r.g.stats.zero_exits, 1);
    EXPECT_EQ(r.g.stats.newton_iters, 0);
}

TEST(PortGraph, PortGraphLinearReducesToPlane) {
    std::mt19937 rng(3);
    std::uniform_real_distribution<double> U(0.0, 1.0);
    const int n = 40;
    PortGraph g;
    g.resize(n);
    g.dt = 10.0;
    for (int t = 0; t < n; ++t) { g.S[static_cast<std::size_t>(t)] = 50.0 + 500.0 * U(rng); g.b[static_cast<std::size_t>(t)] = U(rng) - 0.3; g.z_prev[static_cast<std::size_t>(t)] = U(rng) - 0.5; }
    for (int t = 1; t < n; ++t) {
        FlowEdge e; e.up = t - 1; e.dn = t; e.conduit = (t % 3 != 0);
        e.ju = 2.0 * U(rng); e.jd = -e.ju * U(rng); e.d = U(rng);
        g.edges.push_back(e);
    }
    FlowEdge out; out.up = n - 1; out.dn = -1; out.conduit = false; out.d = 3.0; g.edges.push_back(out);
    g.pinned[7] = 1;
    AdditiveSystem plane, sys;
    plane.build(n, g.edges, g.S.data(), g.dt, &g.pinned);
    std::vector<double> psi = g.z_prev;
    plane.step(g.b.data(), psi.data());
    std::vector<double> z = g.z_prev;
    ASSERT_TRUE(g.solve(z, sys));
    for (int t = 0; t < n; ++t) EXPECT_NEAR(z[static_cast<std::size_t>(t)], psi[static_cast<std::size_t>(t)], 1e-12 * (1.0 + std::fabs(psi[static_cast<std::size_t>(t)])));
    EXPECT_EQ(g.stats.jacobi_fallbacks, 0);
}

TEST(PortGraph, PortGraphIsOrderPreserving) {
    std::mt19937 rng(21);
    std::uniform_real_distribution<double> U(0.0, 1.0);
    int checked = 0;
    for (int trial = 0; trial < 60; ++trial) {
        const int n = 8 + static_cast<int>(U(rng) * 30);
        RandomGraph r = makeRandomGraph(rng, n);
        PortGraph& g = r.g;
        std::vector<double> b(n), bp(n), zp(n);
        for (int t = 0; t < n; ++t) {
            b[static_cast<std::size_t>(t)] = 4.0 * (U(rng) - 0.5);
            bp[static_cast<std::size_t>(t)] = b[static_cast<std::size_t>(t)] + 3.0 * U(rng) * (U(rng) < 0.5);
            zp[static_cast<std::size_t>(t)] = U(rng) - 0.5;
        }
        for (std::size_t s = 0; s < g.stores.size(); ++s) g.dV_prev[s] = 0.0;
        for (int t = 0; t < n; ++t)
            if (g.store_of[static_cast<std::size_t>(t)] >= 0)
                g.dV_prev[static_cast<std::size_t>(g.store_of[static_cast<std::size_t>(t)])] = g.volume(t, zp[static_cast<std::size_t>(t)], nullptr);
        g.z_prev = zp;
        AdditiveSystem sys;
        g.b = b;
        std::vector<double> z = zp;
        ASSERT_TRUE(g.solve(z, sys));
        g.b = bp;
        std::vector<double> z2 = zp;
        ASSERT_TRUE(g.solve(z2, sys));
        for (int t = 0; t < n; ++t) EXPECT_LE(z[static_cast<std::size_t>(t)], z2[static_cast<std::size_t>(t)] + 1e-9) << "trial " << trial << " node " << t;
        ++checked;
    }
    EXPECT_EQ(checked, 60);
}

namespace {
// Run two members on the same graph and return (lhs, rhs) of the l1 contraction.
std::pair<double, double> contraction(PortGraph& g, std::mt19937& rng, double* out_rel = nullptr) {
    std::uniform_real_distribution<double> U(0.0, 1.0);
    const int n = g.n;
    std::vector<double> z1p(n), z2p(n), b1(n), b2(n);
    for (int t = 0; t < n; ++t) {
        z1p[static_cast<std::size_t>(t)] = 1.5 * (U(rng) - 0.5);
        z2p[static_cast<std::size_t>(t)] = 1.5 * (U(rng) - 0.5);
        b1[static_cast<std::size_t>(t)] = 3.0 * (U(rng) - 0.5);
        b2[static_cast<std::size_t>(t)] = 3.0 * (U(rng) - 0.5);
    }
    auto setup = [&](const std::vector<double>& zp, const std::vector<double>& bb) {
        g.z_prev = zp; g.b = bb;
        for (int t = 0; t < n; ++t)
            if (g.store_of[static_cast<std::size_t>(t)] >= 0)
                g.dV_prev[static_cast<std::size_t>(g.store_of[static_cast<std::size_t>(t)])] = g.volume(t, zp[static_cast<std::size_t>(t)], nullptr);
    };
    AdditiveSystem sys;
    setup(z1p, b1);
    std::vector<double> dV1p(n), dV2p(n), z1 = z1p, z2 = z2p;
    for (int t = 0; t < n; ++t) dV1p[static_cast<std::size_t>(t)] = g.volumePrev(t);
    EXPECT_TRUE(g.solve(z1, sys));
    setup(z2p, b2);
    for (int t = 0; t < n; ++t) dV2p[static_cast<std::size_t>(t)] = g.volumePrev(t);
    EXPECT_TRUE(g.solve(z2, sys));
    double lhs = 0.0, rhs = 0.0;
    for (int t = 0; t < n; ++t) {
        if (g.pinned[static_cast<std::size_t>(t)]) continue;
        lhs += std::fabs(g.volume(t, z1[static_cast<std::size_t>(t)], nullptr) - g.volume(t, z2[static_cast<std::size_t>(t)], nullptr));
        rhs += std::fabs(dV1p[static_cast<std::size_t>(t)] - dV2p[static_cast<std::size_t>(t)]) + g.dt * std::fabs(b1[static_cast<std::size_t>(t)] - b2[static_cast<std::size_t>(t)]);
    }
    if (out_rel) *out_rel = lhs / rhs;
    return {lhs, rhs};
}
}  // namespace

TEST(PortGraph, PortGraphIsL1Contractive) {
    std::mt19937 rng(5);
    std::uniform_real_distribution<double> U(0.0, 1.0);
    double worst = 0.0;
    for (int trial = 0; trial < 80; ++trial) {
        RandomGraph r = makeRandomGraph(rng, 6 + static_cast<int>(U(rng) * 30));
        double rel = 0.0;
        const auto [lhs, rhs] = contraction(r.g, rng, &rel);
        EXPECT_LE(lhs, rhs * (1.0 + 1e-9)) << "trial " << trial;
        worst = std::max(worst, rel);
    }
    std::printf("[PH-ports] l1 contraction: worst lhs/rhs %.4f over 80 random port graphs\n", worst);
}

// ---------------------------------------------------------------------------
// O-3: the street pond as a store, the orifice as a two-port
// ---------------------------------------------------------------------------

TEST(StorePorts, OrificeTwoPortIsMonotonePassiveAndRegularised) {
    std::mt19937 rng(31);
    std::uniform_real_distribution<double> U(-1.0, 1.0);
    const double c = 0.3, h = 1.0e-7;
    for (int k = 0; k < 20000; ++k) {
        const double hm = 1.0 + U(rng), zor = 1.0 + 0.3 * U(rng), p = 0.4 * U(rng);
        const double gm = P::ramp(0.5 + U(rng)), gp = P::ramp(p);
        const double q = P::orificePortFlow(c, kTwoG, hm, zor, p, P::kDelta0, gm, gp);
        // (P1): nondecreasing in the manhole head, nonincreasing in the pond depth
        // (the dry factors follow their own level).
        EXPECT_GE(P::orificePortFlow(c, kTwoG, hm + h, zor, p, P::kDelta0, gm, gp) - q, -1e-12);
        EXPECT_LE(P::orificePortFlow(c, kTwoG, hm, zor, p + h, P::kDelta0, gm, P::ramp(p + h)) - q, 1e-12);
        // passive: flow only downhill, (h_mh − (z_or + p))·Q >= 0
        EXPECT_GE((hm - (zor + p)) * q, 0.0);
        // an empty pond gives nothing back
        if (p <= 0.0) EXPECT_GE(q, 0.0);
        // the bare law is odd in Δ
        EXPECT_DOUBLE_EQ(P::orificeFlow(c, kTwoG, hm, zor, p, P::kDelta0), -P::orificeFlow(c, kTwoG, zor + std::max(p, 0.0), hm, 0.0, P::kDelta0));
    }
    // Regularised: the slope stays finite as the two levels meet (Δ -> 0), so the
    // Newton Jacobian does; the bare square root's slope grows without bound.
    const double bound = c * std::sqrt(kTwoG) / std::sqrt(P::kDelta0);
    double smax = 0.0;
    for (double d = 1e-12; d < 0.5; d *= 1.2)
        for (double sgn : {1.0, -1.0}) {
            // levels about 0 so that the finite difference resolves d = 1e-12
            const double hm = sgn * d, eps = 1e-3 * d;
            const double s = (P::orificeFlow(c, kTwoG, hm + eps, 0.0, 0.0, P::kDelta0) - P::orificeFlow(c, kTwoG, hm, 0.0, 0.0, P::kDelta0)) / eps;
            smax = std::max(smax, s);
        }
    EXPECT_TRUE(std::isfinite(smax));
    EXPECT_LE(smax, 1.01 * bound) << "orifice slope near level is unbounded: regularisation missing";
    // Beyond δ0 it is the orifice law itself.
    EXPECT_DOUBLE_EQ(P::orificeFlow(c, kTwoG, 3.0, 2.0, 0.5, P::kDelta0), c * std::sqrt(kTwoG) * std::sqrt(0.5));
    // Pond store: linear volume, floor through its absolute dry factor.
    P::PondStore ps; ps.a = 250.0; ps.p_star = 0.2;
    double ar = 0.0;
    EXPECT_EQ(ps.dV(0.0, &ar), 0.0);
    EXPECT_EQ(ar, 250.0);
    EXPECT_DOUBLE_EQ(ps.dV(-0.1, nullptr), -25.0);
    EXPECT_EQ(ps.dry(0.0), 1.0);
    EXPECT_EQ(ps.dry(-0.2), 0.0);
    EXPECT_GT(ps.dry(-0.195), 0.0);
}

TEST(PortGraph, WithPondsZeroDataIsExact) {
    std::mt19937 rng(41);
    RandomGraph r = makeRandomGraph(rng, 30, true);
    ASSERT_GT(r.g.n, 30) << "no pond drawn";
    AdditiveSystem sys;
    std::vector<double> z(static_cast<std::size_t>(r.g.n), 0.0);
    ASSERT_TRUE(r.g.solve(z, sys));
    for (double v : z) EXPECT_EQ(v, 0.0);
    EXPECT_EQ(r.g.stats.zero_exits, 1);
}

TEST(PortGraph, WithPondsIsOrderPreserving) {
    std::mt19937 rng(43);
    std::uniform_real_distribution<double> U(0.0, 1.0);
    int with_ponds = 0;
    for (int trial = 0; trial < 60; ++trial) {
        const int nnet = 8 + static_cast<int>(U(rng) * 30);
        RandomGraph r = makeRandomGraph(rng, nnet, true);
        PortGraph& g = r.g;
        const int n = g.n;
        with_ponds += n > nnet;
        std::vector<double> b(n), bp(n), zp(n);
        for (int t = 0; t < n; ++t) {
            b[static_cast<std::size_t>(t)] = 4.0 * (U(rng) - 0.5);
            bp[static_cast<std::size_t>(t)] = b[static_cast<std::size_t>(t)] + 3.0 * U(rng) * (U(rng) < 0.5);
            zp[static_cast<std::size_t>(t)] = U(rng) - 0.5;
        }
        for (int t = 0; t < n; ++t)
            if (g.store_of[static_cast<std::size_t>(t)] >= 0)
                g.dV_prev[static_cast<std::size_t>(g.store_of[static_cast<std::size_t>(t)])] = g.volume(t, zp[static_cast<std::size_t>(t)], nullptr);
        g.z_prev = zp;
        AdditiveSystem sys;
        g.b = b;
        std::vector<double> z = zp;
        ASSERT_TRUE(g.solve(z, sys));
        g.b = bp;
        std::vector<double> z2 = zp;
        ASSERT_TRUE(g.solve(z2, sys));
        for (int t = 0; t < n; ++t) EXPECT_LE(z[static_cast<std::size_t>(t)], z2[static_cast<std::size_t>(t)] + 1e-9) << "trial " << trial << " node " << t;
    }
    EXPECT_GT(with_ponds, 40);
}

TEST(PortGraph, WithPondsIsL1Contractive) {
    std::mt19937 rng(47);
    std::uniform_real_distribution<double> U(0.0, 1.0);
    double worst = 0.0;
    int with_ponds = 0;
    for (int trial = 0; trial < 80; ++trial) {
        const int nnet = 6 + static_cast<int>(U(rng) * 30);
        RandomGraph r = makeRandomGraph(rng, nnet, true);
        with_ponds += r.g.n > nnet;
        double rel = 0.0;
        const auto [lhs, rhs] = contraction(r.g, rng, &rel);
        EXPECT_LE(lhs, rhs * (1.0 + 1e-9)) << "trial " << trial;
        worst = std::max(worst, rel);
    }
    EXPECT_GT(with_ponds, 50);
    std::printf("[PH-ports] l1 contraction with street ponds: worst lhs/rhs %.4f over 80 random port graphs\n", worst);
}

TEST(PortGraph, ManholeToPondConservesWater) {
    // (c) The water a manhole store sends through the orifice is the water its
    // pond store gains: dt·Q_port(z) = a·(z_p − z_p^n) after the solve, both
    // ways (surcharging manhole; pond draining into a manhole below the street).
    std::mt19937 rng(53);
    std::uniform_real_distribution<double> U(0.0, 1.0);
    int up = 0, down = 0;
    for (int trial = 0; trial < 200; ++trial) {
        PortGraph g;
        g.resize(2);
        g.dt = 5.0 + 30.0 * U(rng);
        g.S = {20.0 + 200.0 * U(rng), 50.0 + 100.0 * U(rng)};
        const double S0 = g.S[0], c3 = S0 * U(rng);
        g.store_of[0] = 0;
        PortGraph::Store mh;
        mh.dV = [S0, c3](double z, double* a) { if (a) *a = S0 + 3.0 * c3 * z * z; return S0 * z + c3 * z * z * z; };
        g.stores.push_back(mh);
        FlowEdge e; e.up = 0; e.dn = 1; e.ju = 2.0 * U(rng); e.jd = -e.ju * U(rng); g.edges.push_back(e);
        FlowEdge o; o.up = 1; o.dn = -1; o.conduit = false; o.d = 1.0 + U(rng); g.edges.push_back(o);
        const double z_or = 3.0, h_mh = z_or + 0.4 * (U(rng) - 0.5), p_det = U(rng) < 0.25 ? 0.0 : 0.3 * U(rng);
        const double a = std::pow(10.0, 1.0 + 3.0 * U(rng));
        const int pn = addPond(g, 0, a, 0.05 + 0.5 * U(rng), h_mh, 2.0, z_or, p_det);
        g.dV_prev.assign(g.stores.size(), 0.0);
        g.b = {6.0 * (U(rng) - 0.5), 0.0, 0.0};
        g.z_prev = {0.4 * (U(rng) - 0.5), 0.0, std::max(0.3 * (U(rng) - 0.3), -p_det)};   // a member pond starts at or above empty
        for (int t = 0; t < g.n; ++t)
            if (g.store_of[static_cast<std::size_t>(t)] >= 0)
                g.dV_prev[static_cast<std::size_t>(g.store_of[static_cast<std::size_t>(t)])] = g.volume(t, g.z_prev[static_cast<std::size_t>(t)], nullptr);
        AdditiveSystem sys;
        std::vector<double> z = g.z_prev;
        ASSERT_TRUE(g.solve(z, sys));
        const auto& port = g.ports.back();
        const double q_port = port.dq(0, z[0], z[static_cast<std::size_t>(pn)]);   // extra flow manhole -> pond
        const double gained = g.volume(pn, z[static_cast<std::size_t>(pn)], nullptr) - g.volumePrev(pn);
        // Equal to the solve's own tolerance on the pond equation, 1e-10 ft of
        // pond level (|F| <= 1e-10·a/dt + 1e-12·|terms|): not 1e-10 relative,
        // which a converged Newton step does not promise for a large footprint.
        EXPECT_NEAR(gained, g.dt * q_port, 1e-10 * a + 1e-12 * g.dt * std::fabs(q_port)) << "trial " << trial;
        (gained > 0.0 ? up : down) += std::fabs(gained) > 1e-9;
        // An empty member pond gives nothing back. (p_det + z_p may end below 0
        // in deviation form: the main run's pond also moves over the step, by
        // dt·q_det/a here, so the member's own water is p_det + dt·q_det/a + z_p.)
        const double pm = p_det + z[static_cast<std::size_t>(pn)];
        const double q_abs = P::orificePortFlow(1.0, kTwoG, h_mh + z[0], z_or, pm, P::kDelta0, 1.0, P::ramp(pm));
        if (pm <= 0.0) EXPECT_GE(q_abs, 0.0) << "trial " << trial;
    }
    EXPECT_GT(up, 20);
    EXPECT_GT(down, 20);
}

TEST(PortGraph, NonMonotonePortBreaksL1Contraction) {
    // Mutation: a raw kickback brake whose operating point sits on its falling
    // limb (dQ/dh_up = -S/(2 dt)) is a negative resistance. A member that
    // starts above the main run moves further away from it.
    for (bool envelope : {false, true}) {
        PortGraph g;
        g.resize(2);
        g.dt = 10.0;
        g.S = {100.0, 100.0};
        RatingTable T;
        T.y = {-1.0, 0.0, 1.0};
        T.q = {10.0 + 5.0, 10.0, 10.0 - 5.0};   // falling: slope -5 cfs/ft = -S/(2 dt)
        T.q[0] = 15.0;
        if (envelope) T.makeMonotone();
        const double T0 = T.eval(0.0);
        PortGraph::Port p;
        p.up = 0; p.dn = 1;
        p.dq = [T, T0](int, double zu, double) { return zu == 0.0 ? 0.0 : T.eval(zu) - T0; };
        g.ports.push_back(p);
        FlowEdge out; out.up = 1; out.dn = -1; out.conduit = false; out.d = 2.0; g.edges.push_back(out);
        AdditiveSystem sys;
        std::vector<double> zA = {0.0, 0.0};   // the main run's own member: stays at 0
        g.z_prev = zA;
        ASSERT_TRUE(g.solve(zA, sys));
        std::vector<double> zB = {0.1, 0.0};
        g.z_prev = zB;
        g.solve(zB, sys);
        const double lhs = std::fabs(100.0 * zB[0]) + std::fabs(100.0 * zB[1]);
        const double rhs = 100.0 * 0.1;
        if (envelope) EXPECT_LE(lhs, rhs * (1.0 + 1e-9));
        else EXPECT_GT(lhs, rhs * 1.05) << "the raw kickback table should expand the volume difference";
    }
}

TEST(PortGraph, PortGraphConservesVolume) {
    // Every flow booked exactly once: Σ dV change = dt·(Σ sources − water leaving the graph).
    std::mt19937 rng(17);
    std::uniform_real_distribution<double> U(0.0, 1.0);
    for (int trial = 0; trial < 40; ++trial) {
        const int n = 6 + static_cast<int>(U(rng) * 30);
        RandomGraph r = makeRandomGraph(rng, n);
        PortGraph& g = r.g;
        if (trial % 3 == 0) g.pinned[static_cast<std::size_t>(n / 3)] = 1;
        for (int t = 0; t < n; ++t) { g.b[static_cast<std::size_t>(t)] = 3.0 * (U(rng) - 0.4); g.z_prev[static_cast<std::size_t>(t)] = U(rng) - 0.5; }
        for (int t = 0; t < n; ++t)
            if (g.store_of[static_cast<std::size_t>(t)] >= 0)
                g.dV_prev[static_cast<std::size_t>(g.store_of[static_cast<std::size_t>(t)])] = g.volume(t, g.z_prev[static_cast<std::size_t>(t)], nullptr) + 0.3 * U(rng);
        AdditiveSystem sys;
        std::vector<double> z = g.z_prev;
        ASSERT_TRUE(g.solve(z, sys));
        double src = 0.0;
        for (int t = 0; t < n; ++t) if (!g.pinned[static_cast<std::size_t>(t)]) src += g.b[static_cast<std::size_t>(t)];
        const double dv = sumDV(g, z);
        const double rhs = g.dt * (src - g.boundaryOutflow(z));
        double scale = 0.0;
        for (int t = 0; t < n; ++t) scale += std::fabs(g.volume(t, z[static_cast<std::size_t>(t)], nullptr)) + std::fabs(g.volumePrev(t));
        EXPECT_NEAR(dv, rhs, 1e-10 * std::max(scale, 1.0)) << "trial " << trial;
    }
}

TEST(PortGraph, FloorClampCreatesWater) {
    // Mutation: restoring the old projection z >= floor after the solve breaks conservation.
    PortGraph g;
    g.resize(3);
    g.dt = 10.0;
    g.S = {100.0, 100.0, 100.0};
    FlowEdge e1; e1.up = 0; e1.dn = 1; e1.ju = 5.0; e1.jd = -5.0; g.edges.push_back(e1);
    FlowEdge e2; e2.up = 1; e2.dn = 2; e2.ju = 5.0; e2.jd = -5.0; g.edges.push_back(e2);
    FlowEdge o; o.up = 2; o.dn = -1; o.conduit = false; o.d = 3.0; g.edges.push_back(o);
    g.b = {-40.0, 0.0, 0.0};   // a member short of water at node 0
    for (bool clamp : {false, true}) {
        g.floor_projection = clamp;
        g.floor = {-0.5, -0.5, -0.5};
        AdditiveSystem sys;
        std::vector<double> z = {0.0, 0.0, 0.0};
        g.z_prev = z;
        g.solve(z, sys);
        const double dv = sumDV(g, z), rhs = g.dt * (-40.0 - g.boundaryOutflow(z));
        if (clamp) EXPECT_GT(std::fabs(dv - rhs), 1.0) << "the clamp should create water";
        else EXPECT_NEAR(dv, rhs, 1e-9);
    }
}

TEST(PortGraph, PhantomRaisedEndDrainsBelowThePipeEnd) {
    // Mutation (note §1.3): basin B's pipe end z_B sits 1 ft above the far
    // node F's end z_F. The main run stands just above z_B and drains into F.
    // A member starts below z_B and is drier at F as well (F falls toward its
    // end). Corrected law: B's exchange stops at z_B, the member keeps its
    // water and rises back. Legacy kind-6 law: K·sqrt(z_B − max(h_F, z_F))
    // keeps draining B below its end, water the member does not have.
    const double zB_end = 101.0, zF_end = 100.0, hB = 101.05, hF = 100.5, K = 4.0;
    for (bool legacy : {false, true}) {
        PortGraph g;
        g.resize(2);
        g.dt = 30.0;
        g.S = {200.0, 20.0};
        g.ports.push_back(raisedPort(0, 1, K, hB, zB_end, hF, zF_end, legacy));
        FlowEdge o; o.up = 1; o.dn = -1; o.conduit = false; o.d = 10.0; g.edges.push_back(o);
        g.b = {0.0, -5.0};
        const double q_star = (legacy ? P::raisedEndFlowLegacyKind6 : P::raisedEndFlow)(K, hB, zB_end, hF, zF_end);
        std::vector<double> z = {-0.5, 0.0};   // member B at 100.55: below its pipe end
        AdditiveSystem sys;
        for (int step = 0; step < 40; ++step) {
            g.z_prev = z;
            g.solve(z, sys);
        }
        const double hBm = hB + z[0], hFm = hF + z[1];
        const double qm = q_star + g.ports[0].dq(0, z[0], z[1]);   // the member's absolute exchange
        if (!legacy) {
            EXPECT_GE(hBm, zB_end - 1e-6) << "corrected law: a member stops draining at its pipe end";
            if (qm > 1e-12) { EXPECT_GT(hBm, std::max(hFm, zF_end)); }
        } else {
            EXPECT_LT(hBm, zB_end - 0.05) << "the legacy law should drain B below its end (phantom)";
            EXPECT_GT(qm, 0.0);
            EXPECT_FALSE(hBm > std::max(hFm, zF_end)) << "legacy: Q_BF > 0 with h_B below max(h_F, z_F)";
        }
    }
}

TEST(PortGraph, StraightLineOutletOfAStoreStopsAtItsFloor) {
    // A store (main-run depth 0.5 ft) drained by a straight-line outlet whose
    // main-run flow exceeds ju·d* (a full pipe with a large head drop), and a
    // member that receives none of the main run's inflow. Bare line: the member keeps draining below
    // the invert (mutation; Bellinge G72R83B read -3,600 ft). Floored by the
    // store's dry factor: it stops within δ_dry of the invert. The main run's
    // inflow is q* (steady), so b = -q* is a member that receives nothing.
    const double d_star = 0.5, q_star = 10.0, ju = 4.0;   // q* > ju·d*
    for (bool floored : {false, true}) {
        PortGraph g;
        g.resize(2);
        g.dt = 10.0;
        g.S = {100.0, 100.0};
        PortGraph::Port p;
        p.up = 0; p.dn = 1;
        p.dq = [=](int, double zu, double) {
            if (zu == 0.0) return 0.0;
            const double qm = q_star + ju * zu;
            return (floored ? P::flooredFlow(qm, P::dryFactor(d_star, zu), 1.0) : qm) - q_star;
        };
        g.ports.push_back(p);
        FlowEdge o; o.up = 1; o.dn = -1; o.conduit = false; o.d = 2.0; g.edges.push_back(o);
        g.b = {-q_star, 0.0};
        std::vector<double> z = {0.0, 0.0};
        AdditiveSystem sys;
        double zmin = 0.0;
        for (int step = 0; step < 30; ++step) { g.z_prev = z; ASSERT_TRUE(g.solve(z, sys)); zmin = std::min(zmin, z[0]); }
        if (floored) EXPECT_GE(d_star + zmin, -P::kDeltaDry - 1e-9);
        else EXPECT_LT(d_star + zmin, -1.0);
    }
    // The wrapper is monotone and exact at the main run.
    for (double zu = -1.0; zu < 1.0; zu += 0.01) {
        const double a = P::flooredFlow(q_star + ju * zu, P::dryFactor(d_star, zu), 1.0);
        const double b = P::flooredFlow(q_star + ju * (zu + 0.01), P::dryFactor(d_star, zu + 0.01), 1.0);
        EXPECT_GE(b, a);
    }
    EXPECT_EQ(P::flooredFlow(q_star, P::dryFactor(d_star, 0.0), 1.0), q_star);
}

// ===========================================================================
// Step 3 — engine wiring (hook rom1dStorageMembersEnabled())
// ===========================================================================
#ifdef OPENSWMM_HAS_2D
namespace {

// Basin behind a weir (the storage-threshold gate's network), or the same
// network with the basin replaced by a junction (no admitted node).
std::string engineFixture(bool basin, bool soft, double crest = 2.468) {
    std::ostringstream f;
    f << std::setprecision(10);
    f << "[TITLE]\nper-member ports\n\n[OPTIONS]\n";
    f << "FLOW_UNITS CFS\nINFILTRATION HORTON\nFLOW_ROUTING DYNWAVE\n";
    f << "START_DATE 01/01/2025\nSTART_TIME 00:00:00\nREPORT_START_DATE 01/01/2025\nREPORT_START_TIME 00:00:00\n";
    f << "END_DATE 01/01/2025\nEND_TIME 01:30:00\n";
    f << "REPORT_STEP 00:05:00\nWET_STEP 00:01:00\nDRY_STEP 00:05:00\nROUTING_STEP 00:00:10\n";
    f << "MINIMUM_STEP 0.5\nTHREADS 1\n\n[EVAPORATION]\nCONSTANT 0.0\n\n";
    f << "[RAINGAGES]\nRG1 INTENSITY 0:05 1.0 TIMESERIES TS0\n\n[TIMESERIES]\n";
    for (int m = 0; m <= 90; m += 5) {
        const double tri = (m <= 60) ? (1.0 - std::fabs(m - 30.0) / 30.0) : 0.0;
        f << "TS0 01/01/2025 " << std::setfill('0') << std::setw(2) << m / 60 << ":" << std::setfill('0') << std::setw(2)
          << m % 60 << " " << 2.0 * tri << "\n";
    }
    f << std::setfill(' ') << "\n";
    f << "[SUBCATCHMENTS]\nS1 RG1 B1 2.0 300 100 1.0 0\n\n[SUBAREAS]\nS1 0.015 0.20 0.00 0.00 100 OUTLET\n\n";
    f << "[INFILTRATION]\nS1 0.0 0.0 0.0 0.0 0.0\n\n";
    f << "[JUNCTIONS]\n";
    if (!basin) f << "B1 100 20 0 0 0\n";
    f << "J2 99 10 0 0 0\nJ3 98 10 0 0 0\nJ4 97 10 0 0 0\n\n";
    f << "[OUTFALLS]\nO1 96 FREE NO\n\n";
    if (basin) f << "[STORAGE]\nB1 100 20 0 FUNCTIONAL 0 0 3000\n\n";
    f << "[CONDUITS]\nC1 J2 J3 300 0.015 0 0 0 0\nC2 J3 J4 300 0.015 0 0 0 0\nC3 J4 O1 300 0.015 0 0 0 0\n";
    if (!basin) f << "C0 B1 J2 300 0.015 0 0 0 0\n";
    f << "\n";
    if (basin) f << "[WEIRS]\nW1 B1 J2 TRANSVERSE " << crest << " 3.33 NO 0 0 YES\n\n";
    f << "[XSECTIONS]\nC1 CIRCULAR 2 0 0 0 1\nC2 CIRCULAR 2 0 0 0 1\nC3 CIRCULAR 2 0 0 0 1\n";
    f << (basin ? "W1 RECT_OPEN 4 3 0 0\n" : "C0 CIRCULAR 2 0 0 0 1\n") << "\n";
    f << "[REPORT]\nINPUT NO\nCONTINUITY YES\nNODES ALL\nLINKS ALL\n\n";
    if (soft) f << "[SOFT_RAINGAGES]\nRG1 NORMAL CV 0.20\n";
    return f.str();
}

struct Trace {
    std::vector<double> head, flow, q05, q95, corr;
    std::size_t admitted = 0;
    bool ok = false;
};

Trace runEngine(const std::string& inp, const std::string& tag, bool members) {
    Trace tr;
    const std::string pfx = "rom_member_ports_" + std::to_string(getpid()) + "_" + tag;
    const std::string ip = pfx + ".inp", rp = pfx + ".rpt";
    { std::ofstream f(ip); f << inp; }
    SWMM_Engine h = swmm_engine_create();
    auto cleanup = [&]() {
        swmm_engine_end(h); swmm_engine_close(h); swmm_engine_destroy(h);
        for (const char* ext : {".inp", ".rpt", ".uncertainty.csv", ".rom_threshold.csv", ".rom_diag.csv", ".out"})
            std::remove((pfx + ext).c_str());
    };
    if (swmm_engine_open(h, ip.c_str(), rp.c_str(), nullptr, nullptr) != 0) { cleanup(); return tr; }
    auto* eng = static_cast<openswmm::SWMMEngine*>(h);
    eng->rom1dStorageMembersEnabled() = members;
    if (swmm_engine_initialize(h) != 0 || swmm_engine_start(h, 0) != 0) { cleanup(); return tr; }
    const auto& ctx = eng->context();
    double el = 1.0;
    for (int guard = 0; el != 0.0 && guard < 200000; ++guard) {
        if (swmm_engine_step(h, &el) != 0) break;
        for (int i = 0; i < ctx.n_nodes(); ++i) tr.head.push_back(ctx.nodes.head[static_cast<std::size_t>(i)]);
        for (int j = 0; j < ctx.n_links(); ++j) tr.flow.push_back(ctx.links.flow[static_cast<std::size_t>(j)]);
        if (const auto* rom = eng->rom1d(); rom && rom->is_ready()) {
            tr.q05.insert(tr.q05.end(), rom->q05.begin(), rom->q05.end());
            tr.q95.insert(tr.q95.end(), rom->q95.begin(), rom->q95.end());
        }
        const auto& c = eng->rom1dMemberCorrection();
        tr.corr.insert(tr.corr.end(), c.begin(), c.end());
        if (el > 0.0 && el * 86400.0 >= 5400.0) break;
    }
    tr.admitted = eng->rom1dStorageMemberNodes().size();
    tr.ok = !tr.head.empty();
    cleanup();
    return tr;
}

}  // namespace

TEST(PerMemberPortsEngine, StructureSecantKeepsDeterministicRunBitIdenticalWithStoresOn) {
    // The deterministic run is untouched by the sidecar, with per-member stores
    // on: every rating table is built through flowAtShiftedDepth, which restores state.
    const Trace off = runEngine(engineFixture(true, false), "det_plain", false);
    const Trace on = runEngine(engineFixture(true, true), "det_members", true);
    ASSERT_TRUE(off.ok && on.ok);
    ASSERT_GE(on.admitted, 1u) << "the basin should carry per-member levels";
    ASSERT_EQ(off.head.size(), on.head.size());
    ASSERT_EQ(off.flow.size(), on.flow.size());
    for (std::size_t k = 0; k < off.head.size(); ++k) EXPECT_EQ(off.head[k], on.head[k]) << "head sample " << k;
    for (std::size_t k = 0; k < off.flow.size(); ++k) EXPECT_EQ(off.flow[k], on.flow[k]) << "flow sample " << k;
}

TEST(PerMemberPortsEngine, PerMemberStoresOffIsBitIdentical) {
    // No admitted node (no storage, no coupling): C ≡ 0 and the bands are
    // bit-identical to per-member off.
    const Trace off = runEngine(engineFixture(false, true), "noadm_off", false);
    const Trace on = runEngine(engineFixture(false, true), "noadm_on", true);
    ASSERT_TRUE(off.ok && on.ok);
    EXPECT_EQ(on.admitted, 0u);
    ASSERT_FALSE(on.q05.empty());
    ASSERT_EQ(off.q05.size(), on.q05.size());
    for (std::size_t k = 0; k < on.q05.size(); ++k) {
        EXPECT_EQ(off.q05[k], on.q05[k]);
        EXPECT_EQ(off.q95[k], on.q95[k]);
    }
    for (double c : on.corr) EXPECT_EQ(c, 0.0);
}

TEST(PerMemberPortsEngine, AdmittedBasinChangesTheBandOnly) {
    // With a basin behind a weir the per-member path is live: the band moves,
    // the deterministic head does not.
    const Trace off = runEngine(engineFixture(true, true), "adm_off", false);
    const Trace on = runEngine(engineFixture(true, true), "adm_on", true);
    ASSERT_TRUE(off.ok && on.ok);
    EXPECT_GE(on.admitted, 1u);
    ASSERT_EQ(off.head.size(), on.head.size());
    for (std::size_t k = 0; k < off.head.size(); ++k) EXPECT_EQ(off.head[k], on.head[k]);
    double diff = 0.0;
    for (std::size_t k = 0; k < std::min(on.q95.size(), off.q95.size()); ++k) diff = std::max(diff, std::fabs(on.q95[k] - off.q95[k]));
    EXPECT_GT(diff, 1e-3);
}

// ---------------------------------------------------------------------------
// Coupled manhole (O-3): a 1D chain J1..J4 whose head manhole J1 sits under the
// low corner of a sloped 2D patch. Rain on the patch ponds over J1; the
// subcatchment overloads the 0.3 m pipes so J1 also surcharges to the street.
// ---------------------------------------------------------------------------
namespace {

std::string coupledFixture(double cv) {
    std::ostringstream f;
    f << std::setprecision(10);
    f << "[TITLE]\ncoupled per-member ports\n\n[OPTIONS]\n";
    f << "FLOW_UNITS CMS\nINFILTRATION HORTON\nFLOW_ROUTING DYNWAVE\n";
    f << "START_DATE 01/01/2026\nSTART_TIME 00:00:00\nREPORT_START_DATE 01/01/2026\nREPORT_START_TIME 00:00:00\n";
    f << "END_DATE 01/01/2026\nEND_TIME 00:40:00\nREPORT_STEP 00:01:00\nWET_STEP 00:01:00\nDRY_STEP 00:05:00\n";
    f << "ROUTING_STEP 5\nMINIMUM_STEP 0.5\nTHREADS 1\nALLOW_PONDING NO\n\n[EVAPORATION]\nCONSTANT 0.0\n\n";
    f << "[RAINGAGES]\nRG1 INTENSITY 0:01 1.0 TIMESERIES TS1\n\n";
    f << "[TIMESERIES]\n";
    for (int m = 0; m <= 40; ++m)
        f << "TS1 01/01/2026 00:" << std::setfill('0') << std::setw(2) << m << " " << (m < 15 ? 80.0 : 0.0) << "\n";
    f << std::setfill(' ') << "\n";
    f << "[SUBCATCHMENTS]\nS1 RG1 J1 1.0 100 50 1.0 0\n\n[SUBAREAS]\nS1 0.015 0.20 0.00 0.00 100 OUTLET\n\n";
    f << "[INFILTRATION]\nS1 0.0 0.0 0.0 0.0 0.0\n\n";
    f << "[JUNCTIONS]\nJ1 0.0 1.0 0 0 0\nJ2 -0.1 1.0 0 0 0\nJ3 -0.2 1.0 0 0 0\nJ4 -0.3 1.0 0 0 0\n\n";
    f << "[OUTFALLS]\nO1 -0.5 FREE NO\n\n";
    f << "[CONDUITS]\nC1 J1 J2 30 0.013 0 0 0\nC2 J2 J3 30 0.013 0 0 0\nC3 J3 J4 30 0.013 0 0 0\nC4 J4 O1 30 0.013 0 0 0\n\n";
    f << "[XSECTIONS]\n";
    for (const char* c : {"C1", "C2", "C3", "C4"}) f << c << " CIRCULAR 0.3 0 0 0 1\n";
    f << "\n[2D_OPTIONS]\nMAX_TIMESTEP 4\nDRY_DEPTH 0.002\nCOUPLING_CD 0.7\nREPORT_2D NO\n\n[2D_VERTICES]\n";
    constexpr int kN = 4;
    const double d = 12.0 / kN;
    for (int i = 0; i <= kN; ++i)
        for (int j = 0; j <= kN; ++j) f << j * d << " " << i * d << " " << 1.0 + 0.02 * (i + j) << "\n";
    f << "\n[2D_TRIANGLES]\n";
    for (int i = 0; i < kN; ++i)
        for (int j = 0; j < kN; ++j) {
            const int v00 = i * (kN + 1) + j, v01 = v00 + 1, v10 = v00 + kN + 1, v11 = v10 + 1;
            f << v00 << " " << v01 << " " << v11 << " 0.03\n" << v00 << " " << v11 << " " << v10 << " 0.03\n";
        }
    f << "\n[2D_VERTEX_NODE_MAP]\n0 J1 0.7 0.1\n\n";
    f << "[REPORT]\nINPUT NO\nCONTINUITY YES\nNODES ALL\nLINKS ALL\n\n";
    f << "[SOFT_RAINGAGES]\nRG1 NORMAL CV " << cv << "\n";
    return f.str();
}

struct CoupledTrace {
    std::vector<double> head, flow, surf, q05, q95, corr, cvol;
    std::size_t admitted = 0, admitted_coupled = 0;
    long steps = 0, pg_steps = 0, law_evals = 0, port_evals = 0, solves = 0, newton = 0, jacobi = 0;
    double wall_s = 0.0, max_ext = 0.0;
    bool ok = false;
};

CoupledTrace runCoupled(const std::string& inp, const std::string& tag, bool members, double pert) {
    CoupledTrace tr;
    const std::string pfx = "rom_member_ports_cpl_" + std::to_string(getpid()) + "_" + tag;
    const std::string ip = pfx + ".inp", rp = pfx + ".rpt";
    { std::ofstream f(ip); f << inp; }
    SWMM_Engine h = swmm_engine_create();
    auto cleanup = [&]() {
        swmm_engine_end(h); swmm_engine_close(h); swmm_engine_destroy(h);
        for (const char* ext : {".inp", ".rpt", ".uncertainty.csv", ".rom_threshold.csv", ".rom_diag.csv", ".out"})
            std::remove((pfx + ext).c_str());
    };
    if (swmm_engine_open(h, ip.c_str(), rp.c_str(), nullptr, nullptr) != 0) { cleanup(); return tr; }
    auto* eng = static_cast<openswmm::SWMMEngine*>(h);
    auto& o = eng->surfaceRouter2D().options();
    o.enable_rom = true;
    o.rom_members = 20;
    o.rom_modes = 6;
    o.rom_mannings_pert = pert;
    o.rom_rainfall_pert = pert;
    o.rom_cd_pert = 0.5 * pert;
    eng->rom1dStorageMembersEnabled() = members;
    if (swmm_engine_initialize(h) != 0 || swmm_engine_start(h, 0) != 0) { cleanup(); return tr; }
    const auto& ctx = eng->context();
    const auto t0 = std::chrono::steady_clock::now();
    double el = 1.0;
    for (int guard = 0; el != 0.0 && guard < 200000; ++guard) {
        if (swmm_engine_step(h, &el) != 0) break;
        ++tr.steps;
        for (int i = 0; i < ctx.n_nodes(); ++i) tr.head.push_back(ctx.nodes.head[static_cast<std::size_t>(i)]);
        for (int j = 0; j < ctx.n_links(); ++j) tr.flow.push_back(ctx.links.flow[static_cast<std::size_t>(j)]);
        double v = 0.0;
        swmm_2d_get_total_volume(h, &v);
        tr.surf.push_back(v);
        if (const auto* rom = eng->rom1d(); rom && rom->is_ready()) {
            tr.q05.insert(tr.q05.end(), rom->q05.begin(), rom->q05.end());
            tr.q95.insert(tr.q95.end(), rom->q95.begin(), rom->q95.end());
        }
        const auto& c = eng->rom1dMemberCorrection();
        tr.corr.insert(tr.corr.end(), c.begin(), c.end());
        if (el > 0.0 && el * 86400.0 >= 2400.0) break;
    }
    tr.wall_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    tr.admitted = eng->rom1dStorageMemberNodes().size();
    if (const auto* rom1 = eng->rom1d()) {
        const auto& cps = eng->surfaceRouter2D().nodeCouplingPoints();
        for (int a : eng->rom1dStorageMemberNodes())
            for (const auto& cp : cps)
                if (cp.node_idx >= 0 && static_cast<std::size_t>(cp.node_idx) < rom1->full_to_active.size() &&
                    rom1->full_to_active[static_cast<std::size_t>(cp.node_idx)] == a) ++tr.admitted_coupled;
    }
    if (const auto* r2 = eng->surfaceRouter2D().rom()) {
        for (std::size_t k = 0; k < eng->surfaceRouter2D().nodeCouplingPoints().size(); ++k)
            for (int i = 0; i < r2->n_ensemble; ++i) {
                tr.cvol.push_back(r2->couplingVolume(k, i));
                tr.max_ext = std::max(tr.max_ext, std::fabs(r2->couplingVolume(k, i)));
            }
    }
    tr.pg_steps = eng->rom1dPortGraphSteps();
    tr.law_evals = eng->rom1dCouplingLawEvals();
    const auto& st = eng->rom1dPortGraphStats();
    tr.port_evals = st.port_evals; tr.solves = st.solves; tr.newton = st.newton_iters; tr.jacobi = st.jacobi_fallbacks;
    tr.ok = !tr.head.empty();
    cleanup();
    return tr;
}

}  // namespace

TEST(PerMemberPortsCoupled, DeterministicRunBitIdenticalWithCoupledStores) {
    // (d) with a coupled manhole carried per member: the deterministic 1D and
    // 2D runs are untouched, bit for bit; the per-member path really ran.
    const CoupledTrace off = runCoupled(coupledFixture(0.2), "det_off", false, 0.2);
    const CoupledTrace on = runCoupled(coupledFixture(0.2), "det_on", true, 0.2);
    ASSERT_TRUE(off.ok && on.ok);
    ASSERT_GE(on.admitted_coupled, 1u) << "the coupled manhole should carry per-member levels";
    EXPECT_GT(on.max_ext, 0.0) << "members exchanged nothing: the coupling port never ran";
    ASSERT_EQ(off.head.size(), on.head.size());
    for (std::size_t k = 0; k < off.head.size(); ++k) ASSERT_EQ(off.head[k], on.head[k]) << "head sample " << k;
    for (std::size_t k = 0; k < off.flow.size(); ++k) ASSERT_EQ(off.flow[k], on.flow[k]) << "flow sample " << k;
    for (std::size_t k = 0; k < off.surf.size(); ++k) ASSERT_EQ(off.surf[k], on.surf[k]) << "2D volume sample " << k;
}

TEST(PerMemberPortsCoupled, ZeroPerturbationAddsNothingThroughPerMemberPath) {
    // (e) CouplingFluxZeroPerturbationAddsNothing through the per-member path:
    // no perturbation anywhere ⇒ every member's correction and every booked
    // exchange deviation is exactly zero, with the coupled manhole admitted.
    const CoupledTrace on = runCoupled(coupledFixture(0.0), "zero", true, 0.0);
    ASSERT_TRUE(on.ok);
    ASSERT_GE(on.admitted_coupled, 1u);
    ASSERT_GT(on.pg_steps, 0);
    for (double c : on.corr) ASSERT_EQ(c, 0.0);
    ASSERT_FALSE(on.cvol.empty());
    for (double v : on.cvol) ASSERT_EQ(v, 0.0);
}

TEST(PerMemberPortsCoupled, CostOfThePerMemberPathOnACoupledManhole) {
    // Cost diagnostic (plan §6 runtime line): street-orifice law evaluations
    // and port evaluations per per-member step, wall time ON vs OFF. Printed,
    // not asserted (wall time in Debug is noisy).
    const CoupledTrace off = runCoupled(coupledFixture(0.2), "cost_off", false, 0.2);
    const CoupledTrace on = runCoupled(coupledFixture(0.2), "cost_on", true, 0.2);
    ASSERT_TRUE(off.ok && on.ok);
    ASSERT_GT(on.pg_steps, 0);
    const double st = static_cast<double>(on.pg_steps);
    std::printf("[PH-O3 cost] steps %ld, per-member steps %ld, admitted %zu (coupled %zu)\n",
                on.steps, on.pg_steps, on.admitted, on.admitted_coupled);
    std::printf("[PH-O3 cost] orifice-law evals/step %.1f, port evals/step %.1f, solves %ld, newton %ld, jacobi %ld\n",
                static_cast<double>(on.law_evals) / st, static_cast<double>(on.port_evals) / st, on.solves, on.newton, on.jacobi);
    std::printf("[PH-O3 cost] wall OFF %.3f s, ON %.3f s, ratio %.3f\n", off.wall_s, on.wall_s, on.wall_s / off.wall_s);
}
#endif  // OPENSWMM_HAS_2D
