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
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "uncertainty/RomStorePorts.hpp"
#include "uncertainty/RomPortGraph.hpp"
#include "uncertainty/RomAdditiveChannel.hpp"
#include "uncertainty/RomAdmissionClosure.hpp"
#include "rom_port_graph_full_reference.hpp"

#ifdef OPENSWMM_HAS_2D
#include "openswmm/engine/openswmm_engine.h"
#include "openswmm/engine/openswmm_2d.h"
#include "core/SWMMEngine.hpp"
#include "uncertainty/SpectralROM1D.hpp"
#include "uncertainty/LhsShuffle.hpp"
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
    // Street ponds (O-10). pond_mode 1: a pond node + orifice two-port (the O-9
    // formulation, kept as the reference); pond_mode 2: ONE composite port per
    // manhole whose pond level is solved inside the law, the pond's start level
    // read from *zpp.
    struct Pond { int mh = -1, node = -1; std::size_t port = 0, slot = 0; P::PondOrificePort po; };
    std::vector<Pond> ponds;
    std::shared_ptr<std::vector<double>> zpp = std::make_shared<std::vector<double>>();
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

// The same manhole-pond pair as ONE composite port on the manhole (O-10): the
// pond's start level is *zpp[slot], its level solved inside the law.
void addPondComposite(RandomGraph& r, int mh, double a, double c, double h_mh, double d_mh, double z_or, double p_det) {
    P::PondOrificePort po;
    po.c_or = c; po.two_g = kTwoG; po.d0 = P::kDelta0; po.ft_to_m = 1.0; po.cfs_per_cms = 1.0;
    po.h_det = h_mh; po.z_or = z_or; po.p_det = p_det; po.d_mh = d_mh; po.a = a;
    po.anchor();
    RandomGraph::Pond pd;
    pd.mh = mh; pd.po = po; pd.slot = r.zpp->size();
    r.zpp->push_back(0.0);
    PortGraph::Port p;
    p.up = mh; p.dn = -1;
    const double dt = r.g.dt;
    auto zpp = r.zpp;
    const std::size_t slot = pd.slot;
    p.dq = [po, dt, zpp, slot](int, double zu, double) { return po.compositeFlow(1.0, zu, (*zpp)[slot], dt); };
    pd.port = r.g.ports.size();
    r.g.ports.push_back(std::move(p));
    r.ponds.push_back(pd);
}

// One-way structure ports with the engine's O-9 laws (SWMMEngine.cpp): a depth-rated
// pump / structure table through P::anchoredRatedFlow (a rating T(y) = cap·max(d* + y, 0),
// flow >= 0), and the floored fallback line max(q* + d·z_up, 0) for a structure whose
// table cannot be built. Both are nondecreasing in z_up and exact at z = 0.
PortGraph::Port ratedOneWayPort(int up, int dn, double cap, double d_star, double q_star) {
    PortGraph::Port p;
    p.up = up; p.dn = dn;
    const double T0 = cap * std::max(d_star, 0.0) * P::ramp(d_star);
    p.dq = [=](int, double zu, double) {
        if (zu == 0.0) return 0.0;
        const double Ty = cap * std::max(d_star + zu, 0.0) * P::ramp(d_star + zu);
        return P::anchoredRatedFlow(Ty, T0, q_star, d_star, zu) - q_star;
    };
    return p;
}
PortGraph::Port flooredOneWayLine(int up, int dn, double q_star, double d, double d_star) {
    PortGraph::Port p;
    p.up = up; p.dn = dn;
    p.dq = [=](int, double zu, double zd) {
        if (zu == 0.0 && zd == 0.0) return 0.0;
        return P::flooredFlow(std::max(q_star + d * zu, 0.0), P::dryFactor(d_star, zu), 1.0) - q_star;
    };
    return p;
}

RandomGraph makeRandomGraph(std::mt19937& rng, int n, bool ponds = false, bool one_way = false, int pond_mode = 1) {
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
    if (one_way) {
        // O-9: pumps and structures as floored one-way ports (rated table or floored line).
        for (int k = 0; k < n / 2; ++k) {
            const int a = static_cast<int>(U(rng) * (n - 1)), c = a + 1 + static_cast<int>(U(rng) * (n - 1 - a));
            const double d_star = U(rng) < 0.25 ? 0.0 : 0.1 + U(rng);
            const double cap = 0.5 + 3.0 * U(rng);
            const double q_star = std::min(cap * d_star, cap) * U(rng);
            if (U(rng) < 0.5) g.ports.push_back(ratedOneWayPort(a, c, cap, d_star, q_star));
            else g.ports.push_back(flooredOneWayLine(a, c, q_star + 0.2 * U(rng), 0.5 + 4.0 * U(rng), d_star));
        }
    }
    if (ponds) {
        // About a quarter of the network nodes are coupled manholes with a street
        // pond: manhole above, near or below the street; pond wet or dry.
        for (int t = 0; t < n; ++t) {
            if (U(rng) > 0.25) continue;
            const double z_or = 1.0 + 0.5 * U(rng);
            const double h_mh = z_or + 0.6 * (U(rng) - 0.5);
            const double p_det = U(rng) < 0.3 ? 0.0 : 0.4 * U(rng);
            if (pond_mode == 2) {
                addPondComposite(r, t, std::pow(10.0, 1.0 + 3.0 * U(rng)), 0.05 + 0.5 * U(rng), h_mh, 0.5 + U(rng), z_or, p_det);
            } else {
                RandomGraph::Pond pd;
                pd.mh = t;
                pd.port = g.ports.size();
                pd.node = addPond(g, t, std::pow(10.0, 1.0 + 3.0 * U(rng)), 0.05 + 0.5 * U(rng), h_mh, 0.5 + U(rng), z_or, p_det);
                r.ponds.push_back(pd);
            }
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
    std::vector<double> z(30, 0.0);
    ASSERT_TRUE(r.g.solve(z));
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
    AdditiveSystem plane;
    plane.build(n, g.edges, g.S.data(), g.dt, &g.pinned);
    std::vector<double> psi = g.z_prev;
    plane.step(g.b.data(), psi.data());
    std::vector<double> z = g.z_prev;
    ASSERT_TRUE(g.solve(z));
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
        g.b = b;
        std::vector<double> z = zp;
        ASSERT_TRUE(g.solve(z));
        g.b = bp;
        std::vector<double> z2 = zp;
        ASSERT_TRUE(g.solve(z2));
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
    setup(z1p, b1);
    std::vector<double> dV1p(n), dV2p(n), z1 = z1p, z2 = z2p;
    for (int t = 0; t < n; ++t) dV1p[static_cast<std::size_t>(t)] = g.volumePrev(t);
    EXPECT_TRUE(g.solve(z1));
    setup(z2p, b2);
    for (int t = 0; t < n; ++t) dV2p[static_cast<std::size_t>(t)] = g.volumePrev(t);
    EXPECT_TRUE(g.solve(z2));
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
// O-9: one-way structures and pumps are floored ports, never plane lines
// ---------------------------------------------------------------------------

namespace {
struct WetWell {
    PortGraph g;
    std::size_t orifice_port = 0, pump_port = 0;
};
// A wet well W (store, area 12 ft^2, main run 0.8 ft deep) drained by a side orifice to a
// lateral L (40 ft^2, which drains to an outfall through its own conduit) and by a pump to
// the outfall. Main run: orifice 0.5 cfs, pump 1.0 cfs, steady inflow 1.5 cfs.
// as_port: the orifice and the pump are rated ports; otherwise the plane lines q* + d·z_up
// (the pre-O-9 behaviour of a structure with no unknown end), booked as plane edges.
WetWell makeWetWell(bool as_port) {
    WetWell w;
    PortGraph& g = w.g;
    g.resize(2);
    g.dt = 5.0;
    g.S = {12.0, 40.0};
    g.store_of[0] = 0;
    PortGraph::Store st;
    st.dV = [](double z, double* a) { if (a) *a = 12.0; return 12.0 * z; };
    g.stores.push_back(st);
    FlowEdge out; out.up = 1; out.dn = -1; out.ju = 2.0; out.jd = 0.0; out.conduit = true; g.edges.push_back(out);
    const double d_star = 0.8;
    if (as_port) {
        w.orifice_port = g.ports.size();
        g.ports.push_back(ratedOneWayPort(0, 1, 3.0, d_star, 0.5));
        w.pump_port = g.ports.size();
        g.ports.push_back(ratedOneWayPort(0, -1, 2.5, d_star, 1.0));
    } else {
        FlowEdge o; o.up = 0; o.dn = 1; o.conduit = false; o.d = 3.0; g.edges.push_back(o);
        FlowEdge p; p.up = 0; p.dn = -1; p.conduit = false; p.d = 2.5; g.edges.push_back(p);
    }
    // The member's negative extra inflow at the well is a sink limited by its water (as in the engine).
    PortGraph::Port sink;
    sink.up = 0; sink.dn = -1;
    sink.dq = [d_star](int, double zu, double) { return 4.0 * (P::dryFactor(d_star, zu) - 1.0); };
    g.ports.push_back(sink);
    g.dV_prev.assign(1, 0.0);
    return w;
}
}  // namespace

TEST(OneWayPorts, WetWellOutletsNeverRunBackwardsAndNeverSinkTheLateral) {
    // A member that loses 4 cfs of inflow at W (b = -4 cfs, a sink limited by its water) while the
    // main run keeps its structures running: its water at W falls to the invert.
    // As ports the orifice's flow is q* + dq >= 0 and the pump's too, so the lateral L loses
    // at most the orifice's q* per step (the orifice shuts) and W stops at its floor.
    // As plane lines (mutation) the orifice runs backwards once d·|z_W| > q*, pulling water
    // out of L, and W has no floor at all.
    const double dt = 5.0, S_L = 40.0, q_or = 0.5, q_pump = 1.0;
    for (bool as_port : {true, false}) {
        WetWell w = makeWetWell(as_port);
        PortGraph& g = w.g;
        g.b = {-4.0, 0.0};
        std::vector<double> z = {0.0, 0.0};
        double zL_min = 0.0, zL_prev = 0.0, worst_step_drop = 0.0, min_or = q_or, min_pump = q_pump;
        for (int step = 0; step < 40; ++step) {
            g.z_prev = z;
            g.dV_prev[0] = g.volume(0, z[0], nullptr);
            ASSERT_TRUE(g.solve(z)) << "step " << step;
            zL_min = std::min(zL_min, z[1]);
            worst_step_drop = std::max(worst_step_drop, zL_prev - z[1]);
            zL_prev = z[1];
            if (as_port) {
                min_or = std::min(min_or, q_or + g.portVolume(w.orifice_port, z) / dt);
                min_pump = std::min(min_pump, q_pump + g.portVolume(w.pump_port, z) / dt);
            } else {
                min_or = std::min(min_or, q_or + 3.0 * z[0]);
                min_pump = std::min(min_pump, q_pump + 2.5 * z[0]);
            }
        }
        std::printf("[PH-ports] O-9 wet well %s: lateral min z %.3f ft, worst one-step drop %.4f ft (bound %.4f), min orifice flow %.3f cfs, min pump flow %.3f cfs\n",
                    as_port ? "ports" : "plane lines", zL_min, worst_step_drop, dt * q_or / S_L, min_or, min_pump);
        if (as_port) {
            EXPECT_GE(min_or, -1e-9);
            EXPECT_GE(min_pump, -1e-9);
            // L can lose only what the orifice carried, per step (its own outlet only slows that)
            EXPECT_LE(worst_step_drop, dt * q_or / S_L + 1e-9);
            // and W stops at its floor: the pump and the orifice shut as the well empties
            EXPECT_GE(0.8 + z[0], -P::kDeltaDry - 1e-9);
        } else {
            EXPECT_LT(min_or, -1.0) << "the plane line should run backwards";
            EXPECT_LT(zL_min, -5.0 * dt * q_or / S_L) << "and sink the lateral";
        }
    }
}

TEST(OneWayPorts, WetWellConservesWaterWithFlooredOneWayPorts) {
    // Σ dV = dt·(sources − water leaving the graph), booked once per port (the orifice moves water W -> L).
    WetWell w = makeWetWell(true);
    PortGraph& g = w.g;
    for (double b : {-1.5, -0.4, 0.0, 0.7}) {
        g.b = {b, 0.1};
        std::vector<double> z = {0.0, 0.0};
        for (int step = 0; step < 6; ++step) {
            g.z_prev = z;
            g.dV_prev[0] = g.volume(0, z[0], nullptr);
            std::vector<double> z1 = z;
            ASSERT_TRUE(g.solve(z1));
            double dv = 0.0;
            for (int t = 0; t < 2; ++t) dv += g.volume(t, z1[static_cast<std::size_t>(t)], nullptr) - g.volumePrev(t);
            EXPECT_NEAR(dv, g.dt * (g.b[0] + g.b[1] - g.boundaryOutflow(z1)), 1e-9 * (1.0 + std::fabs(dv)));
            z = z1;
        }
    }
}

TEST(OneWayPorts, RandomGraphsWithFlooredOneWayPortsAreOrderPreservingAndContractive) {
    std::mt19937 rng(91);
    std::uniform_real_distribution<double> U(0.0, 1.0);
    double worst_rel = 0.0;
    for (int trial = 0; trial < 60; ++trial) {
        const int n = 8 + static_cast<int>(U(rng) * 30);
        RandomGraph r = makeRandomGraph(rng, n, false, true);
        PortGraph& g = r.g;
        // order preservation
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
        g.b = b;
        std::vector<double> z = zp;
        ASSERT_TRUE(g.solve(z)) << "trial " << trial;
        g.b = bp;
        std::vector<double> z2 = zp;
        ASSERT_TRUE(g.solve(z2)) << "trial " << trial;
        for (int t = 0; t < n; ++t) EXPECT_LE(z[static_cast<std::size_t>(t)], z2[static_cast<std::size_t>(t)] + 1e-9) << "trial " << trial << " node " << t;
        // l1 contraction
        double rel = 0.0;
        const auto [lhs, rhs] = contraction(g, rng, &rel);
        EXPECT_LE(lhs, rhs * (1.0 + 1e-9)) << "trial " << trial;
        worst_rel = std::max(worst_rel, rel);
    }
    std::printf("[PH-ports] O-9 one-way floored ports: l1 contraction worst lhs/rhs %.4f over 60 random graphs\n", worst_rel);
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
    std::vector<double> z(static_cast<std::size_t>(r.g.n), 0.0);
    ASSERT_TRUE(r.g.solve(z));
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
        g.b = b;
        std::vector<double> z = zp;
        ASSERT_TRUE(g.solve(z));
        g.b = bp;
        std::vector<double> z2 = zp;
        ASSERT_TRUE(g.solve(z2));
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

// ---------------------------------------------------------------------------
// O-10: the pond eliminated analytically -- one composite port on the manhole
// ---------------------------------------------------------------------------
TEST(CompositePondPort, LawSolvesThePondEquationAndIsMonotoneAnchoredAndConservative) {
    std::mt19937 rng(101);
    std::uniform_real_distribution<double> U(0.0, 1.0);
    int into = 0, outof = 0, dry = 0;
    for (int k = 0; k < 4000; ++k) {
        P::PondOrificePort po;
        po.c_or = 0.05 + 0.5 * U(rng); po.two_g = kTwoG; po.d0 = P::kDelta0; po.ft_to_m = 1.0; po.cfs_per_cms = 1.0;
        po.z_or = 1.0 + 0.5 * U(rng); po.h_det = po.z_or + 0.6 * (U(rng) - 0.5);
        po.p_det = U(rng) < 0.3 ? 0.0 : 0.4 * U(rng); po.d_mh = 0.5 + U(rng);
        po.a = std::pow(10.0, 1.0 + 3.0 * U(rng));
        po.anchor();
        const double dt = 5.0 + 55.0 * U(rng), cm = 0.6 + 0.8 * U(rng);
        const double zu = 1.2 * (U(rng) - 0.5);
        const double zpp = std::max(0.5 * (U(rng) - 0.4), -po.p_det);
        const double zp = po.pondLevel(cm, zu, zpp, dt);
        const double Q = po.compositeFlow(cm, zu, zpp, dt);
        // The pond's backward-Euler equation, to roundoff, and the water it gains is dt·Q.
        EXPECT_NEAR(po.a * (zp - zpp) / dt, Q, 1e-12 * (1.0 + std::fabs(Q)) + 1e-13 * po.a / dt * (1.0 + std::fabs(zp)));
        EXPECT_NEAR(Q, po.extraFlow(cm, zu, zp), 0.0);
        (Q > 0.0 ? into : outof) += std::fabs(Q) > 1e-9;
        dry += (po.p_det + zp) <= 0.0;
        // Nondecreasing in the manhole level (P1), and the pond rises with it.
        const double h = 1e-5;
        EXPECT_GE(po.compositeFlow(cm, zu + h, zpp, dt) - Q, -1e-10 * (1.0 + std::fabs(Q)));
        EXPECT_GE(po.pondLevel(cm, zu + h, zpp, dt) - zp, -1e-12);
        // A fuller pond to start with takes less.
        EXPECT_LE(po.compositeFlow(cm, zu, zpp + h, dt) - Q, 1e-10 * (1.0 + std::fabs(Q)));
        // A pond cannot gain more than a full step of the flow it started with.
        const double e0 = po.extraFlow(cm, zu, zpp);
        EXPECT_LE(std::fabs(Q), std::fabs(e0) * (1.0 + 1e-12) + 1e-12);
        EXPECT_GE(Q * e0, -1e-12);
    }
    // Anchored at the main run: nothing at all, bit for bit.
    P::PondOrificePort po;
    po.c_or = 0.3; po.two_g = kTwoG; po.d0 = P::kDelta0; po.ft_to_m = 1.0; po.cfs_per_cms = 1.0;
    po.z_or = 1.2; po.h_det = 1.3; po.p_det = 0.1; po.d_mh = 0.8; po.a = 200.0;
    po.anchor();
    EXPECT_EQ(po.compositeFlow(1.0, 0.0, 0.0, 30.0), 0.0);
    EXPECT_EQ(po.pondLevel(1.0, 0.0, 0.0, 30.0), 0.0);
    EXPECT_GT(into, 500);
    EXPECT_GT(outof, 500);
    EXPECT_GT(dry, 20);
}

TEST(CompositePondPort, CompositeSolveEqualsTheTwoUnknownSolveOnRandomGraphs) {
    // The composite law is the pond store nested into the manhole's port: the
    // member levels and the booked volumes must be those of the O-9 graph (a
    // pond node and the orifice two-port) to 1e-9.
    std::mt19937 seeds(211), draw(212);
    std::uniform_real_distribution<double> U(0.0, 1.0);
    int with_ponds = 0, checked_ponds = 0;
    double worst_z = 0.0, worst_v = 0.0, worst_zp = 0.0;
    for (int trial = 0; trial < 60; ++trial) {
        const unsigned sd = seeds();
        const int nnet = 6 + static_cast<int>(U(draw) * 30);
        const bool ow = trial % 2 == 1;
        std::mt19937 r1(sd), r2(sd);
        RandomGraph ref = makeRandomGraph(r1, nnet, true, ow, 1);
        RandomGraph cmp = makeRandomGraph(r2, nnet, true, ow, 2);
        ASSERT_EQ(ref.ponds.size(), cmp.ponds.size());
        with_ponds += !ref.ponds.empty();
        PortGraph &gr = ref.g, &gc = cmp.g;
        ASSERT_EQ(gc.n, nnet);
        ASSERT_EQ(gr.n, nnet + static_cast<int>(ref.ponds.size()));
        std::vector<double> b(nnet), zp0(nnet);
        for (int t = 0; t < nnet; ++t) { b[static_cast<std::size_t>(t)] = 4.0 * (U(draw) - 0.5); zp0[static_cast<std::size_t>(t)] = U(draw) - 0.5; }
        for (int t = 0; t < nnet; ++t) {
            const int s = gr.store_of[static_cast<std::size_t>(t)];
            if (s >= 0) gr.dV_prev[static_cast<std::size_t>(s)] = gr.volume(t, zp0[static_cast<std::size_t>(t)], nullptr);
        }
        gr.b.assign(static_cast<std::size_t>(gr.n), 0.0);
        gr.z_prev.assign(static_cast<std::size_t>(gr.n), 0.0);
        for (int t = 0; t < nnet; ++t) { gr.b[static_cast<std::size_t>(t)] = b[static_cast<std::size_t>(t)]; gr.z_prev[static_cast<std::size_t>(t)] = zp0[static_cast<std::size_t>(t)]; }
        gc.b = b; gc.z_prev = zp0;
        for (int t = 0; t < nnet; ++t) {
            const int s = gc.store_of[static_cast<std::size_t>(t)];
            if (s >= 0) gc.dV_prev[static_cast<std::size_t>(s)] = gc.volume(t, zp0[static_cast<std::size_t>(t)], nullptr);
        }
        // Ponds start at or above empty, wet or dry.
        for (std::size_t k = 0; k < ref.ponds.size(); ++k) {
            const double zpp = std::max(0.4 * (U(draw) - 0.4), -cmp.ponds[k].po.p_det);
            (*cmp.zpp)[cmp.ponds[k].slot] = zpp;
            const auto un = static_cast<std::size_t>(ref.ponds[k].node);
            gr.z_prev[un] = zpp;
            gr.dV_prev[static_cast<std::size_t>(gr.store_of[un])] = gr.volume(ref.ponds[k].node, zpp, nullptr);
        }
        std::vector<double> zr = gr.z_prev, zc = gc.z_prev;
        ASSERT_TRUE(gr.solve(zr)) << "trial " << trial;
        ASSERT_TRUE(gc.solve(zc)) << "trial " << trial;
        EXPECT_EQ(gc.stats.jacobi_fallbacks, 0);
        for (int t = 0; t < nnet; ++t) {
            const double a = zr[static_cast<std::size_t>(t)], c = zc[static_cast<std::size_t>(t)];
            const double e = std::fabs(a - c) / std::max(1.0, std::fabs(a));
            worst_z = std::max(worst_z, e);
            EXPECT_LE(e, 1e-9) << "trial " << trial << " node " << t;
        }
        for (std::size_t k = 0; k < ref.ponds.size(); ++k) {
            ++checked_ponds;
            const double v1 = gr.portVolume(ref.ponds[k].port, zr), v2 = gc.portVolume(cmp.ponds[k].port, zc);
            const double sc = std::max({std::fabs(v1), std::fabs(v2), gr.dt * 1.0e-3});
            // The reference stops at its own residual tolerance on the pond
            // equation (1e-10 ft of pond level: 1e-10·a of volume); the
            // composite solves it to roundoff.
            const double tol = 1e-9 * sc + 2e-10 * cmp.ponds[k].po.a;
            worst_v = std::max(worst_v, std::fabs(v1 - v2) / sc);
            EXPECT_LE(std::fabs(v1 - v2), tol) << "trial " << trial << " pond " << k;
            // The pond's own level, recovered from the composite law, is the reference node's.
            const double zpc = cmp.ponds[k].po.pondLevel(1.0, zc[static_cast<std::size_t>(cmp.ponds[k].mh)], (*cmp.zpp)[cmp.ponds[k].slot], gc.dt);
            const double zpr = zr[static_cast<std::size_t>(ref.ponds[k].node)];
            worst_zp = std::max(worst_zp, std::fabs(zpc - zpr) / std::max(1.0, std::fabs(zpr)));
            EXPECT_LE(std::fabs(zpc - zpr) / std::max(1.0, std::fabs(zpr)), 1e-9) << "trial " << trial << " pond " << k;
            // Conservation: the water the pond gained is the water the manhole sent.
            EXPECT_NEAR(cmp.ponds[k].po.a * (zpc - (*cmp.zpp)[cmp.ponds[k].slot]), v2, 1e-9 * std::max(sc, 1.0e-3));
        }
    }
    EXPECT_GE(with_ponds, 50);
    EXPECT_GT(checked_ponds, 200);
    std::printf("[PH-O10] composite vs two-unknown over 60 random graphs, %d ponds: worst member-level error %.2e, booked volume %.2e, pond level %.2e (relative)\n",
                checked_ponds, worst_z, worst_v, worst_zp);
}

TEST(CompositePondPort, ZeroDataIsExactAndNoPondNodeInTheSystem) {
    std::mt19937 rng(41);
    RandomGraph r = makeRandomGraph(rng, 30, true, false, 2);
    ASSERT_FALSE(r.ponds.empty());
    EXPECT_EQ(r.g.n, 30);   // no pond node: the system has the network's nodes only
    std::vector<double> z(30, 0.0);
    ASSERT_TRUE(r.g.solve(z));
    for (double v : z) EXPECT_EQ(v, 0.0);
    EXPECT_EQ(r.g.stats.zero_exits, 1);
}

TEST(CompositePondPort, IsOrderPreservingAndL1ContractiveWithTheCompositeDefinition) {
    // Order preservation (b <= b' => z <= z') and the l1 contraction of the
    // water in the network AND in the ponds, with composite pond ports.
    std::mt19937 rng(223);
    std::uniform_real_distribution<double> U(0.0, 1.0);
    int with_ponds = 0;
    double worst = 0.0;
    for (int trial = 0; trial < 60; ++trial) {
        const int n = 6 + static_cast<int>(U(rng) * 30);
        RandomGraph r = makeRandomGraph(rng, n, true, trial % 3 == 2, 2);
        PortGraph& g = r.g;
        with_ponds += !r.ponds.empty();
        // order
        {
            std::vector<double> b(n), bp(n), zp(n);
            for (int t = 0; t < n; ++t) {
                b[static_cast<std::size_t>(t)] = 4.0 * (U(rng) - 0.5);
                bp[static_cast<std::size_t>(t)] = b[static_cast<std::size_t>(t)] + 3.0 * U(rng) * (U(rng) < 0.5);
                zp[static_cast<std::size_t>(t)] = U(rng) - 0.5;
            }
            for (auto& pd : r.ponds) (*r.zpp)[pd.slot] = std::max(0.4 * (U(rng) - 0.4), -pd.po.p_det);
            for (int t = 0; t < n; ++t)
                if (g.store_of[static_cast<std::size_t>(t)] >= 0)
                    g.dV_prev[static_cast<std::size_t>(g.store_of[static_cast<std::size_t>(t)])] = g.volume(t, zp[static_cast<std::size_t>(t)], nullptr);
            g.z_prev = zp; g.b = b;
            std::vector<double> z = zp;
            ASSERT_TRUE(g.solve(z));
            g.b = bp;
            std::vector<double> z2 = zp;
            ASSERT_TRUE(g.solve(z2));
            for (int t = 0; t < n; ++t) EXPECT_LE(z[static_cast<std::size_t>(t)], z2[static_cast<std::size_t>(t)] + 1e-9) << "trial " << trial << " node " << t;
        }
        // l1 contraction of the total water (network + ponds) for two members
        {
            std::vector<double> zA(n), zB(n), bA(n), bB(n);
            for (int t = 0; t < n; ++t) {
                zA[static_cast<std::size_t>(t)] = 1.5 * (U(rng) - 0.5); zB[static_cast<std::size_t>(t)] = 1.5 * (U(rng) - 0.5);
                bA[static_cast<std::size_t>(t)] = 3.0 * (U(rng) - 0.5); bB[static_cast<std::size_t>(t)] = 3.0 * (U(rng) - 0.5);
            }
            std::vector<double> pA(r.ponds.size()), pB(r.ponds.size());
            for (std::size_t k = 0; k < r.ponds.size(); ++k) {
                pA[k] = std::max(0.5 * (U(rng) - 0.4), -r.ponds[k].po.p_det);
                pB[k] = std::max(0.5 * (U(rng) - 0.4), -r.ponds[k].po.p_det);
            }
            auto run = [&](const std::vector<double>& zp, const std::vector<double>& bb, const std::vector<double>& pp, std::vector<double>& z, std::vector<double>& dVprev) {
                g.z_prev = zp; g.b = bb;
                for (int t = 0; t < n; ++t)
                    if (g.store_of[static_cast<std::size_t>(t)] >= 0)
                        g.dV_prev[static_cast<std::size_t>(g.store_of[static_cast<std::size_t>(t)])] = g.volume(t, zp[static_cast<std::size_t>(t)], nullptr);
                for (std::size_t k = 0; k < r.ponds.size(); ++k) (*r.zpp)[r.ponds[k].slot] = pp[k];
                z = zp;
                dVprev.assign(static_cast<std::size_t>(n), 0.0);
                for (int t = 0; t < n; ++t) dVprev[static_cast<std::size_t>(t)] = g.volumePrev(t);
                EXPECT_TRUE(g.solve(z));
            };
            std::vector<double> z1, z2, d1, d2;
            run(zA, bA, pA, z1, d1);
            run(zB, bB, pB, z2, d2);
            double lhs = 0.0, rhs = 0.0;
            for (int t = 0; t < n; ++t) {
                if (g.pinned[static_cast<std::size_t>(t)]) continue;
                lhs += std::fabs(g.volume(t, z1[static_cast<std::size_t>(t)], nullptr) - g.volume(t, z2[static_cast<std::size_t>(t)], nullptr));
                rhs += std::fabs(d1[static_cast<std::size_t>(t)] - d2[static_cast<std::size_t>(t)]) + g.dt * std::fabs(bA[static_cast<std::size_t>(t)] - bB[static_cast<std::size_t>(t)]);
            }
            for (std::size_t k = 0; k < r.ponds.size(); ++k) {
                const auto& pd = r.ponds[k];
                const double u1 = pd.po.a * pd.po.pondLevel(1.0, z1[static_cast<std::size_t>(pd.mh)], pA[k], g.dt);
                const double u2 = pd.po.a * pd.po.pondLevel(1.0, z2[static_cast<std::size_t>(pd.mh)], pB[k], g.dt);
                lhs += std::fabs(u1 - u2);
                rhs += pd.po.a * std::fabs(pA[k] - pB[k]);
            }
            EXPECT_LE(lhs, rhs * (1.0 + 1e-9)) << "trial " << trial;
            worst = std::max(worst, lhs / rhs);
        }
    }
    EXPECT_GE(with_ponds, 50);
    std::printf("[PH-O10] l1 contraction with composite pond ports: worst lhs/rhs %.4f over 60 random port graphs\n", worst);
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
        std::vector<double> z = g.z_prev;
        ASSERT_TRUE(g.solve(z));
        const auto& port = g.ports.back();
        const double q_port = port.dq(0, z[0], z[static_cast<std::size_t>(pn)]);   // extra flow manhole -> pond
        const double gained = g.volume(pn, z[static_cast<std::size_t>(pn)], nullptr) - g.volumePrev(pn);
        // Equal to the solve's own tolerance on the pond equation, 1e-10 ft of
        // pond level (|F| <= 1e-10·a/dt + 1e-12·|terms|): not 1e-10 relative,
        // which a converged Newton step does not promise for a large footprint.
        EXPECT_NEAR(gained, g.dt * q_port, 1e-10 * a + 1e-12 * g.dt * std::fabs(q_port)) << "trial " << trial;
        // What the engine books to the 2D ROM is the port's own flow over the
        // step, to rounding (1e-12 relative) -- not the pond's volume change.
        const double booked = g.portVolume(g.ports.size() - 1, z);
        EXPECT_NEAR(booked, g.dt * q_port, 1e-12 * g.dt * std::fabs(q_port)) << "trial " << trial;
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
        std::vector<double> zA = {0.0, 0.0};   // the main run's own member: stays at 0
        g.z_prev = zA;
        ASSERT_TRUE(g.solve(zA));
        std::vector<double> zB = {0.1, 0.0};
        g.z_prev = zB;
        g.solve(zB);
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
        std::vector<double> z = g.z_prev;
        ASSERT_TRUE(g.solve(z));
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
        std::vector<double> z = {0.0, 0.0, 0.0};
        g.z_prev = z;
        g.solve(z);
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
        for (int step = 0; step < 40; ++step) {
            g.z_prev = z;
            g.solve(z);
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
        double zmin = 0.0;
        for (int step = 0; step < 30; ++step) { g.z_prev = z; ASSERT_TRUE(g.solve(z)); zmin = std::min(zmin, z[0]); }
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

// ---------------------------------------------------------------------------
// O-13: a conduit carries no member water against the member's own head
// (RomStorePorts.hpp HeadBound). Bellinge 2026-11-17: the dry pipe G70S214 ->
// G70S213 kept delivering its full normal-flow capacity (0.99 cfs) into a linear
// node standing 89 ft above the store it drained; G71F044 -> G71F043 (a straight
// line with |jd| < ju) pushed 1.3 cfs against 1.8 ft of adverse member head.
// ---------------------------------------------------------------------------
TEST(HeadBound, AnchoredMonotoneAndZeroAgainstTheMembersHead) {
    const double hu = 88.58, hd = 84.32, z_end = 84.32, Kf = 0.445;
    // q* below the full-bore flow at the main run's head: K stays K_full
    const P::HeadBound b = P::HeadBound::make(Kf, 0.3, hu, hd, z_end);
    ASSERT_TRUE(b.active);
    EXPECT_EQ(b.K, Kf);
    EXPECT_EQ(b.apply(0.3, 0.0, 0.0), 0.3);                     // anchor exact
    // q* above it: K raised so the bound never binds at the main run
    const P::HeadBound c = P::HeadBound::make(Kf, 5.0, hu, hd, z_end);
    EXPECT_GE(c.cap(0.0, 0.0), 5.0 - 1e-12);
    EXPECT_EQ(c.apply(5.0, 0.0, 0.0), 5.0);
    // a drowned / adverse main run: off (the straight line keeps its own jd)
    EXPECT_FALSE(P::HeadBound::make(Kf, 0.3, 84.33, 84.32, 84.0).active);
    EXPECT_FALSE(P::HeadBound::make(Kf, 0.3, 84.0, 84.5, 84.0).active);
    EXPECT_EQ(P::HeadBound{}.apply(7.0, 1.0, 50.0), 7.0);
    // zero once the member's downstream water reaches its upstream water
    for (double zu = -2.0; zu <= 10.0; zu += 0.5) {
        const double zd = hu + zu - hd;                           // h_dn = h_up
        EXPECT_EQ(b.apply(0.99, zu, zd), 0.0);
        EXPECT_EQ(b.apply(0.99, zu, zd + 3.0), 0.0);
    }
    // monotone: nondecreasing in z_up, nonincreasing in z_dn (law: a capped rating of z_up)
    auto law = [&](double zu, double zd) { return b.apply(std::min(0.99, std::max(0.0, 0.2 * zu)), zu, zd); };
    for (double zu = -1.0; zu < 12.0; zu += 0.037)
        for (double zd = -1.0; zd < 15.0; zd += 0.041) {
            EXPECT_LE(law(zu, zd), law(zu + 0.037, zd) + 1e-15);
            EXPECT_GE(law(zu, zd), law(zu, zd + 0.041) - 1e-15);
        }
}

namespace {
// A dry main-run manhole A (store held near its rim by its member pond: inflow b)
// drains through a dry pipe (one-way normal-flow rating of A's depth, capped at
// full bore) into a linear manhole B that has no other outlet (Bellinge G70S214 ->
// G70S213). Returns the largest member head excess h_B - h_A over the run (ft).
double uphillExcess(bool bounded, double b_in, double* zB_end = nullptr) {
    const double hu = 88.58, hd = 84.32, z_end = 84.32, z_rimA = 6.56;
    const double coef = 0.6, Qfull = 0.92, Kf = 0.445, dt = 10.0;
    const P::HeadBound hb = bounded ? P::HeadBound::make(Kf, 0.0, hu, hd, z_end) : P::HeadBound{};
    PortGraph g;
    g.resize(2);
    g.dt = dt;
    g.S = {1.17, 5.5};
    PortGraph::Port pipe;
    pipe.up = 0; pipe.dn = 1;
    pipe.dq = [=](int, double zu, double zd) {
        if (zu == 0.0 && zd == 0.0) return 0.0;
        const double Qn = zu > 0.0 ? std::min(coef * std::pow(zu, 5.0 / 3.0), Qfull) : 0.0;
        return hb.apply(P::anchoredRatedFlow(Qn, 0.0, 0.0, 0.0, zu), zu, zd);
    };
    g.ports.push_back(pipe);
    PortGraph::Port rim;   // A's rim overflow: a loss above its rim
    rim.up = 0; rim.dn = -1;
    const double k = 100.0 * 1.17 / dt;
    rim.dq = [=](int, double zu, double) { return zu == 0.0 ? 0.0 : P::overflowFlow(0.0, k, z_rimA, zu); };
    g.ports.push_back(rim);
    g.b = {b_in, 0.0};
    std::vector<double> z = {0.0, 0.0};
    double worst = -1e300;
    for (int step = 0; step < 2000; ++step) {
        g.z_prev = z;
        if (!g.solve(z)) return 1e300;
        worst = std::max(worst, (hd + z[1]) - (hu + z[0]));
    }
    if (zB_end) *zB_end = z[1];
    return worst;
}
}  // namespace

TEST(HeadBound, DryPipeIntoALinearNodeNeverDeliversUphill) {
    double zB = 0.0;
    const double excess = uphillExcess(true, 1.0, &zB);
    std::printf("[O-13] bounded: max h_B - h_A %.3g ft, z_B end %.3f ft\n", excess, zB);
    EXPECT_LE(excess, 1e-6);
    EXPECT_GT(zB, 4.0);            // B fills to A's level: the water is there, not lost
    double zB0 = 0.0;
    const double excess0 = uphillExcess(false, 1.0, &zB0);
    std::printf("[O-13] unbounded (the law before O-13): max h_B - h_A %.3g ft, z_B end %.1f ft\n", excess0, zB0);
    EXPECT_GT(excess0, 100.0);     // the defect: B climbs without limit above A
    // zero data is exact
    double zz = 1.0;
    EXPECT_LE(uphillExcess(true, 0.0, &zz), -4.26 + 1e-12);
    EXPECT_EQ(zz, 0.0);
}

// ---------------------------------------------------------------------------
// Piecewise-smooth solve (PORT_HAMILTONIAN O-4): kinks in monotone port laws
// ---------------------------------------------------------------------------
namespace {
// Two stores joined by a flow brake whose kickback dip is under its monotone
// envelope (rises to the main run, then a plateau), the first store also has
// a straight-line outlet floored at its bottom; the second drains through a
// pump at a frozen setting, floored (dry ramp) below and opening its spare
// capacity over δ_dry above. Every law is C⁰ with strong corners: dry
// ramp → steep rise → plateau (an S), and the pump's floor/spare corners.
// Plain Newton, the reference the O-4 solver replaced: forward-difference
// Jacobian of the whole residual, dense solve, blind Armijo halving on ‖F‖₁
// (11 halvings, then the last trial is taken), 20 iterations, ‖F‖∞ < 1e-9.
// Linear nodes only (the fixtures below). Returns converged; iters = iterations used.
bool plainNewton(const PortGraph& g, std::vector<double>& z, int* iters) {
    const std::size_t n = z.size();
    auto norm1 = [](const std::vector<double>& v) { double s = 0.0; for (double x : v) s += std::fabs(x); return s; };
    auto ninf = [](const std::vector<double>& v) { double s = 0.0; for (double x : v) s = std::max(s, std::fabs(x)); return s; };
    std::vector<double> F, F1, Ft, zt(n);
    g.residual(z, F);
    *iters = 0;
    for (int it = 0; it < 20; ++it) {
        if (ninf(F) < 1e-9) return true;
        ++*iters;
        std::vector<std::vector<double>> J(n, std::vector<double>(n + 1, 0.0));
        for (std::size_t c = 0; c < n; ++c) {
            auto zp = z;
            const double h = 1e-7 * std::max(1.0, std::fabs(z[c]));
            zp[c] += h;
            g.residual(zp, F1);
            for (std::size_t r = 0; r < n; ++r) J[r][c] = (F1[r] - F[r]) / h + (r == c ? g.S[r] / g.dt : 0.0);
        }
        for (std::size_t r = 0; r < n; ++r) J[r][n] = -F[r];
        for (std::size_t c = 0; c < n; ++c) {   // Gauss with partial pivoting
            std::size_t pv = c;
            for (std::size_t r = c + 1; r < n; ++r) if (std::fabs(J[r][c]) > std::fabs(J[pv][c])) pv = r;
            std::swap(J[c], J[pv]);
            if (J[c][c] == 0.0) return false;
            for (std::size_t r = c + 1; r < n; ++r) {
                const double f = J[r][c] / J[c][c];
                for (std::size_t k = c; k <= n; ++k) J[r][k] -= f * J[c][k];
            }
        }
        std::vector<double> dz(n);
        for (std::size_t r = n; r-- > 0;) {
            double v = J[r][n];
            for (std::size_t k = r + 1; k < n; ++k) v -= J[r][k] * dz[k];
            dz[r] = v / J[r][r];
        }
        const double r0 = norm1(F);
        double lam = 1.0;
        for (int h = 0; h < 12; ++h, lam *= 0.5) {
            for (std::size_t t = 0; t < n; ++t) zt[t] = z[t] + lam * dz[t];
            g.residual(zt, Ft);
            if (norm1(Ft) <= (1.0 - 1e-4 * lam) * r0) break;
        }
        z = zt; F = Ft;
    }
    return ninf(F) < 1e-9;
}

struct KinkSolve { bool ok = false; PortGraph::Stats st; std::vector<double> z, F; };
KinkSolve solveKinkSystem(bool plain) {
    PortGraph g;
    g.resize(2);
    g.dt = 15.8;
    g.S = {12.0, 61.0};
    g.max_jacobi = 0;   // measure Newton alone
    const double qs = 1.1, plate = 0.74, qp = 0.97, Tp = 2.07, dp = 0.0057, ju = 1.72;
    RatingTable T;   // the brake: rises, dips (kickback), rises again; envelope = plateau
    T.y = {-1.0, -0.01, 0.0, 0.02, 0.05, 0.3, 1.0};
    T.q = {0.0, 0.0, qs, qs * 1.2, qs * 0.7, qs * 0.9, qs * 1.2 + plate};
    T.makeMonotone();
    EXPECT_GT(T.raised, 0);
    const double T0 = T.eval(0.0), q_br = 0.9 * qs;   // the main run passes less than its rating
    PortGraph::Port br;
    br.up = 0; br.dn = 1;
    br.dq = [=](int, double zu, double) {
        if (zu == 0.0) return 0.0;
        return P::anchoredRatedFlow(T.eval(zu) * P::ramp(0.5 + zu), T0, q_br, 0.5, zu) - q_br;
    };
    PortGraph::Port pu;
    pu.up = 1; pu.dn = -1;
    pu.dq = [=](int, double zu, double) {
        if (zu == 0.0) return 0.0;
        return P::anchoredRatedFlow(Tp * P::ramp(dp + zu), Tp * P::ramp(dp), qp, dp, zu) - qp;
    };
    PortGraph::Port ln;
    ln.up = 0; ln.dn = -1;
    ln.dq = [=](int, double zu, double) { return P::flooredFlow(0.3 + ju * zu, P::dryFactor(0.5, zu), 1.0) - 0.3; };
    g.ports = {br, pu, ln};
    g.b = {0.78, -0.135};
    g.z_prev = {0.081, -0.059};
    KinkSolve r;
    r.z = g.z_prev;
    if (plain) {
        int it = 0;
        r.ok = plainNewton(g, r.z, &it);
        r.st.newton_iters = it;
    } else {
        r.ok = g.solve(r.z);
        r.st = g.stats;
    }
    g.residual(r.z, r.F);
    return r;
}
}  // namespace

TEST(PortGraph, KinkBoundedNewtonCrossesEnvelopeAndPumpFloorCorners) {
    // Reference: plain Newton (forward slopes, blind halving) stalls on this system.
    const KinkSolve plain = solveKinkSystem(true);
    EXPECT_FALSE(plain.ok) << "plain Newton should stall on this system";
    const KinkSolve s = solveKinkSystem(false);
    ASSERT_TRUE(s.ok);
    EXPECT_LE(s.st.newton_iters, 8);
    EXPECT_EQ(s.st.jacobi_fallbacks, 0);
    EXPECT_GE(s.st.kink_bounded, 1) << "the fixture must exercise the kink bound";
    for (double f : s.F) EXPECT_LT(std::fabs(f), 1e-9);
}

TEST(PortGraph, ActiveSetSlopeAtAKinkWithinTheDifferenceStep) {
    // Bellinge G72R33Bb -> G72R33B (2026-11-02): a node drains through a rated
    // outlet whose main run passes q* with its rating ~0 at the main run's
    // level (below: q*·ramp(δ + y), slope 61.8; above: the rating's 1.56) and
    // through a plane edge. The member's root sits 2.4e-8 ft below the kink,
    // inside the forward-difference step (1e-7): the forward slope blends both
    // pieces, Newton converges linearly with alternating sign and stalls.
    auto solveAt = [](bool plain, double zp, PortGraph::Stats* st) {
        PortGraph g;
        g.resize(2);
        g.dt = 10.0;
        g.S = {17.17, 53.82};
        g.max_jacobi = 0;
        RatingTable T;
        T.y = {-1.0, 0.0, 1.0};
        T.q = {0.0, 0.0, 1.56};
        const double qs = 0.618;
        PortGraph::Port R;
        R.up = 0; R.dn = 1;
        R.dq = [=](int, double zu, double) { return zu == 0.0 ? 0.0 : P::anchoredRatedFlow(T.eval(zu), 0.0, qs, 0.5, zu) - qs; };
        PortGraph::Port L;
        L.up = 0; L.dn = -1;
        L.dq = [](int, double zu, double) { return 0.4621 * zu; };
        PortGraph::Port W;   // the basin's weir, closed at the main run
        W.up = 1; W.dn = -1;
        W.dq = [](int, double zu, double) { return zu > 0.0 ? 3.0 * zu * std::sqrt(zu) : 0.0; };
        g.ports = {R, L, W};
        g.b = {-1.5e-6, -3.065e-5};
        g.z_prev = {zp, -3.5e-4};
        std::vector<double> z = g.z_prev;
        bool ok;
        if (plain) { int it = 0; ok = plainNewton(g, z, &it); }
        else { ok = g.solve(z); *st = g.stats; }
        EXPECT_TRUE(!ok || (z[0] < 0.0 && z[0] > -1e-7)) << "root just below the kink";
        return ok;
    };
    for (double zp : {-2.5e-8, 0.0}) {
        PortGraph::Stats st, st_m;
        EXPECT_TRUE(solveAt(false, zp, &st)) << "zp " << zp;
        EXPECT_LE(st.newton_iters, 3) << "zp " << zp;
        EXPECT_EQ(st.jacobi_fallbacks, 0) << "zp " << zp;
        EXPECT_FALSE(solveAt(true, zp, &st_m)) << "forward-difference slope at the kink should stall (zp " << zp << ")";
    }
}

// ---------------------------------------------------------------------------
// O-5: Schur complement on the port/store nodes, linear part factored once
// ---------------------------------------------------------------------------
namespace {
using FullRef = openswmm::uncertainty::test_reference::FullNetworkPortGraph;

// A linear plane network of 30–200 nodes (tree with branches and a few loops,
// conduit and structure edges, outfalls) with 2–6 nonlinear stores embedded:
// each store drains through a weir or a raised-end two-port, carries a
// member-dependent sink floored by its water, and some have a street pond.
// Random pins. Built identically into G (the O-5 graph or the full reference)
// from the same seed. Members get their own b, z_prev, dV_prev.
template <class G>
void buildEmbedded(G& g, unsigned seed, int M) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> U(0.0, 1.0);
    const int n = 30 + static_cast<int>(U(rng) * 171);
    g.resize(n);
    g.dt = 5.0 + 50.0 * U(rng);
    for (int t = 0; t < n; ++t) {
        g.S[static_cast<std::size_t>(t)] = std::pow(10.0, 1.0 + 3.0 * U(rng));
        g.level[static_cast<std::size_t>(t)] = 90.0 + 20.0 * U(rng);
    }
    for (int t = 1; t < n; ++t) {
        const int up = t - 1 - static_cast<int>(U(rng) * std::min(t, 4));
        FlowEdge e; e.up = up; e.dn = t;
        if (U(rng) < 0.8) { e.conduit = true; e.ju = 0.1 + 5.0 * U(rng); e.jd = -e.ju * U(rng); }
        else { e.conduit = false; e.d = 0.05 + 2.0 * U(rng); }
        g.edges.push_back(e);
    }
    for (int q = 0; q < n / 15; ++q) {   // loops
        const int a = static_cast<int>(U(rng) * n), c = static_cast<int>(U(rng) * n);
        if (a == c) continue;
        FlowEdge e; e.up = std::min(a, c); e.dn = std::max(a, c); e.conduit = true; e.ju = 2.0 * U(rng); e.jd = -e.ju * U(rng);
        g.edges.push_back(e);
    }
    for (int q = 0; q < 3; ++q) {   // outfalls
        FlowEdge o; o.up = n - 1 - static_cast<int>(U(rng) * n / 3); o.dn = -1; o.conduit = false; o.d = 0.5 + 2.0 * U(rng);
        g.edges.push_back(o);
    }
    const int ns = 2 + static_cast<int>(U(rng) * 5);
    std::vector<int> st_nodes;
    for (int s = 0; s < ns; ++s) {
        const int t = static_cast<int>(U(rng) * (n - 1));
        if (std::find(st_nodes.begin(), st_nodes.end(), t) != st_nodes.end()) continue;
        st_nodes.push_back(t);
        const double S0 = g.S[static_cast<std::size_t>(t)], c3 = S0 * U(rng);
        g.store_of[static_cast<std::size_t>(t)] = static_cast<int>(g.stores.size());
        typename G::Store st;
        st.dV = [S0, c3](double z, double* a) { if (a) *a = S0 + 3.0 * c3 * z * z; return S0 * z + c3 * z * z * z; };
        g.stores.push_back(st);
        const int dn = t + 1 + static_cast<int>(U(rng) * std::min(5, n - 1 - t));
        typename G::Port p;
        if (U(rng) < 0.5) {
            const double c = 0.5 + 3.0 * U(rng), a = 0.05 + U(rng);
            p.up = t; p.dn = dn;
            p.dq = [c, a](int, double zu, double) {
                return zu == 0.0 ? 0.0 : c * (std::pow(std::max(a + zu, 0.0), 1.5) - std::pow(a, 1.5));
            };
        } else {
            const double K = 1.0 + 4.0 * U(rng), zB = U(rng), zF = zB + 0.5 * U(rng);
            const double hB = zB + 0.5 * U(rng), hF = zF + U(rng) - 0.5, q0 = P::raisedEndFlow(K, hB, zB, hF, zF);
            p.up = t; p.dn = dn;
            p.dq = [=](int, double zu, double zd) {
                return (zu == 0.0 && zd == 0.0) ? 0.0 : P::raisedEndFlow(K, hB + zu, zB, hF + zd, zF) - q0;
            };
        }
        g.ports.push_back(p);
        std::vector<double> sink(static_cast<std::size_t>(M));
        for (auto& v : sink) v = U(rng) < 0.5 ? 2.0 * U(rng) : 0.0;
        const double d_star = 0.2 + U(rng);
        typename G::Port k;
        k.up = t; k.dn = -1;
        k.dq = [sink, d_star](int i, double zu, double) {
            const double s = sink[static_cast<std::size_t>(i)];
            return s > 0.0 ? s * (P::dryFactor(d_star, zu) - 1.0) : 0.0;
        };
        g.ports.push_back(k);
        if (U(rng) < 0.5) {
            const double a = std::pow(10.0, 1.0 + 3.0 * U(rng)), c = 0.05 + 0.5 * U(rng);
            const double z_or = 1.0 + 0.5 * U(rng), h_mh = z_or + 0.6 * (U(rng) - 0.5), p_det = U(rng) < 0.3 ? 0.0 : 0.4 * U(rng);
            const double d_mh = 0.5 + U(rng);
            const int pn = g.addNode(a);
            P::PondStore ps; ps.a = a; ps.p_star = p_det;
            typename G::Store pst;
            pst.dV = [ps](double z, double* ar) { return ps.dV(z, ar); };
            g.store_of[static_cast<std::size_t>(pn)] = static_cast<int>(g.stores.size());
            g.stores.push_back(pst);
            const double q_det = P::orificePortFlow(c, kTwoG, h_mh, z_or, p_det, P::kDelta0, P::ramp(d_mh), P::ramp(p_det));
            typename G::Port o;
            o.up = t; o.dn = pn;
            o.dq = [=](int, double zu, double zp) {
                if (zu == 0.0 && zp == 0.0) return 0.0;
                return P::orificePortFlow(c, kTwoG, h_mh + zu, z_or, p_det + zp, P::kDelta0, P::ramp(d_mh + zu), P::ramp(p_det + zp)) - q_det;
            };
            g.ports.push_back(o);
        }
    }
    const int npin = static_cast<int>(U(rng) * 4);
    for (int q = 0; q < npin; ++q) g.pinned[static_cast<std::size_t>(U(rng) * n)] = 1;
    g.dV_prev.assign(g.stores.size(), 0.0);
}

// Member i's data, the same for both graphs.
template <class G>
void setMember(G& g, unsigned seed, int i) {
    std::mt19937 rng(seed * 7919u + static_cast<unsigned>(i));
    std::uniform_real_distribution<double> U(0.0, 1.0);
    g.member = i;
    for (int t = 0; t < g.n; ++t) {
        g.b[static_cast<std::size_t>(t)] = 3.0 * (U(rng) - 0.45);
        g.z_prev[static_cast<std::size_t>(t)] = 0.6 * (U(rng) - 0.5);
    }
    for (int t = 0; t < g.n; ++t)
        if (g.store_of[static_cast<std::size_t>(t)] >= 0)
            g.dV_prev[static_cast<std::size_t>(g.store_of[static_cast<std::size_t>(t)])] =
                g.volume(t, g.z_prev[static_cast<std::size_t>(t)], nullptr) + 0.2 * (U(rng) - 0.5);
}
}  // namespace

TEST(PortGraph, SchurComplementSolveMatchesFullNetworkSolve) {
    // O-5: the solve on the k×k Schur complement, with the linear part factored
    // once per graph, reaches the same solution as the full-network Newton it
    // replaced (kept only as the test reference), member by member.
    constexpr int kGraphs = 50, kMembers = 3;
    double worst = 0.0;
    long it_new = 0, it_ref = 0, fb_new = 0, fb_ref = 0, kmax = 0, nmax = 0, lfact = 0;
    for (unsigned gseed = 1; gseed <= kGraphs; ++gseed) {
        PortGraph g;
        FullRef ref;
        buildEmbedded(g, gseed, kMembers);
        buildEmbedded(ref, gseed, kMembers);
        ASSERT_EQ(g.n, ref.n);
        nmax = std::max<long>(nmax, g.n);
        AdditiveSystem sys;
        for (int i = 0; i < kMembers; ++i) {
            setMember(g, gseed, i);
            setMember(ref, gseed, i);
            std::vector<double> z = g.z_prev, zr = ref.z_prev;
            const bool ok = g.solve(z);
            const bool okr = ref.solve(zr, sys);
            ASSERT_TRUE(okr) << "graph " << gseed << " member " << i << ": reference did not converge";
            ASSERT_TRUE(ok) << "graph " << gseed << " member " << i;
            double zmax = 0.0, dmax = 0.0;
            for (std::size_t t = 0; t < z.size(); ++t) { zmax = std::max(zmax, std::fabs(zr[t])); dmax = std::max(dmax, std::fabs(z[t] - zr[t])); }
            const double rel = dmax / std::max(zmax, 1.0e-300);
            worst = std::max(worst, rel);
            EXPECT_LE(rel, 1.0e-9) << "graph " << gseed << " (n " << g.n << ") member " << i << ": max |dz| " << dmax << " of " << zmax;
            // The O-5 solution satisfies the full residual tolerance on every node.
            std::vector<double> F, mag;
            g.residual(z, F, &mag);
            for (int t = 0; t < g.n; ++t) {
                if (g.pinned[static_cast<std::size_t>(t)]) continue;
                double a = 0.0;
                g.volume(t, z[static_cast<std::size_t>(t)], &a);
                const double tol = 1.0e-10 * std::max(a, 1.0e-6) / g.dt + 1.0e-12 * mag[static_cast<std::size_t>(t)];
                // The roundoff floor may stop a solve above the tolerance: allow its bound.
                EXPECT_LE(std::fabs(F[static_cast<std::size_t>(t)]), std::max(tol, 1.0e-9 * (1.0 + mag[static_cast<std::size_t>(t)])))
                    << "graph " << gseed << " member " << i << " node " << t;
            }
        }
        it_new += g.stats.newton_iters; it_ref += ref.stats.newton_iters;
        fb_new += g.stats.jacobi_fallbacks; fb_ref += ref.stats.jacobi_fallbacks;
        kmax = std::max(kmax, g.stats.k_max);
        lfact += g.stats.l_factorisations;
        EXPECT_EQ(g.stats.l_factorisations, 1) << "graph " << gseed << ": one L factorisation per graph, shared by its members";
    }
    std::printf("[PH-O5] %d graphs x %d members (n <= %ld, k <= %ld): worst max|dz|/max|z| %.2e; Newton iterations O-5 %ld / full %ld; "
                "Jacobi fallbacks %ld / %ld; L factorisations %ld\n",
                kGraphs, kMembers, nmax, kmax, worst, it_new, it_ref, fb_new, fb_ref, lfact);
}

TEST(PortGraph, LinearFactorIsSharedByMembersAndRebuiltWhenTheGraphChanges) {
    PortGraph g;
    buildEmbedded(g, 77, 10);
    for (int i = 0; i < 10; ++i) {
        setMember(g, 77, i);
        std::vector<double> z = g.z_prev;
        ASSERT_TRUE(g.solve(z));
    }
    EXPECT_EQ(g.stats.l_factorisations, 1);
    const long solves_one = g.stats.l_solves;
    EXPECT_GT(solves_one, 0);
    // A plane edge changes (a new step's Jacobian): refactored once, on the next solve.
    g.edges[0].ju *= 1.5;
    for (int i = 0; i < 10; ++i) {
        setMember(g, 77, i);
        std::vector<double> z = g.z_prev;
        ASSERT_TRUE(g.solve(z));
    }
    EXPECT_EQ(g.stats.l_factorisations, 2);
    // resize() always starts a new graph.
    buildEmbedded(g, 77, 10);
    g.prepare();
    g.prepare();
    EXPECT_EQ(g.stats.l_factorisations, 3);
}

// ===========================================================================
// O-11 — sparse Newton path (full-network LU, cached pattern) vs the dense Schur path
// ===========================================================================
namespace {

// A tree-like plane network (chain with branches, conduit/structure edges, outfalls)
// with `ns` nonlinear stores. Each store drains through a weir (to a downstream
// neighbour, or out of the system when `to_outfall`), some through a raised-end
// two-port, some carry a floored one-way structure, a few have a composite street
// pond. Random pins. Returns the graph and the per-pond start-level slots.
RandomGraph makeTreeStoreGraph(std::mt19937& rng, int n, int ns, bool to_outfall, bool extras) {
    std::uniform_real_distribution<double> U(0.0, 1.0);
    RandomGraph r;
    PortGraph& g = r.g;
    g.resize(n);
    g.dt = 5.0 + 50.0 * U(rng);
    for (int t = 0; t < n; ++t) {
        g.S[static_cast<std::size_t>(t)] = std::pow(10.0, 1.0 + 3.0 * U(rng));
        g.level[static_cast<std::size_t>(t)] = 90.0 + 20.0 * U(rng);
    }
    for (int t = 1; t < n; ++t) {
        const int up = t - 1 - static_cast<int>(U(rng) * std::min(t, 3));
        FlowEdge e; e.up = up; e.dn = t;
        if (U(rng) < 0.85) { e.conduit = true; e.ju = 0.1 + 5.0 * U(rng); e.jd = -e.ju * U(rng); }
        else { e.conduit = false; e.d = 0.05 + 2.0 * U(rng); }
        g.edges.push_back(e);
    }
    for (int q = 0; q < n / 25; ++q) {   // a few loops
        const int a = static_cast<int>(U(rng) * n), c = static_cast<int>(U(rng) * n);
        if (a == c) continue;
        FlowEdge e; e.up = std::min(a, c); e.dn = std::max(a, c); e.conduit = true; e.ju = 2.0 * U(rng); e.jd = -e.ju * U(rng);
        g.edges.push_back(e);
    }
    for (int q = 0; q < 3; ++q) {
        FlowEdge o; o.up = n - 1 - static_cast<int>(U(rng) * n / 3); o.dn = -1; o.conduit = false; o.d = 0.5 + 2.0 * U(rng);
        g.edges.push_back(o);
    }
    std::vector<int> nodes(static_cast<std::size_t>(n));
    for (int t = 0; t < n; ++t) nodes[static_cast<std::size_t>(t)] = t;
    std::shuffle(nodes.begin(), nodes.end(), rng);
    nodes.resize(static_cast<std::size_t>(std::min(ns, n)));
    for (int t : nodes) {
        const double S0 = g.S[static_cast<std::size_t>(t)], c3 = S0 * U(rng);
        g.store_of[static_cast<std::size_t>(t)] = static_cast<int>(g.stores.size());
        PortGraph::Store st;
        st.dV = [S0, c3](double z, double* a) { if (a) *a = S0 + 3.0 * c3 * z * z; return S0 * z + c3 * z * z * z; };
        g.stores.push_back(st);
        const int dn = (to_outfall || t == n - 1) ? -1 : t + 1 + static_cast<int>(U(rng) * std::min(4, n - 1 - t));
        if (!extras || U(rng) < 0.6) {
            g.ports.push_back(weirPort(t, dn, 0.5 + 3.0 * U(rng), 0.05 + U(rng)));
        } else {
            const double zB = U(rng), zF = zB + 0.5 * U(rng);
            g.ports.push_back(raisedPort(t, dn, 1.0 + 4.0 * U(rng), zB + 0.5 * U(rng), zB, zF + U(rng) - 0.5, zF));
        }
        if (extras && U(rng) < 0.3) {   // one-way structure to a downstream node, floored
            const int c2 = std::min(n - 1, t + 1 + static_cast<int>(U(rng) * 5));
            if (c2 != t) {
                const double d_star = U(rng) < 0.25 ? 0.0 : 0.1 + U(rng);
                const double cap = 0.5 + 3.0 * U(rng);
                const double q_star = std::min(cap * d_star, cap) * U(rng);
                if (U(rng) < 0.5) g.ports.push_back(ratedOneWayPort(t, c2, cap, d_star, q_star));
                else g.ports.push_back(flooredOneWayLine(t, c2, q_star + 0.2 * U(rng), 0.5 + 4.0 * U(rng), d_star));
            }
        }
        if (extras && U(rng) < 0.25) {   // composite street pond
            const double z_or = 1.0 + 0.5 * U(rng);
            addPondComposite(r, t, std::pow(10.0, 1.0 + 3.0 * U(rng)), 0.05 + 0.5 * U(rng), z_or + 0.6 * (U(rng) - 0.5), 0.5 + U(rng), z_or, U(rng) < 0.3 ? 0.0 : 0.4 * U(rng));
        }
    }
    if (extras)
        for (int q = 0, np = static_cast<int>(U(rng) * 4); q < np; ++q) g.pinned[static_cast<std::size_t>(U(rng) * n)] = 1;
    g.dV_prev.assign(g.stores.size(), 0.0);
    return r;
}

// Member data from a seed: sources, start levels, store volumes, pond start levels.
void setRandomMember(RandomGraph& r, unsigned seed, double b_amp = 4.0) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> U(0.0, 1.0);
    PortGraph& g = r.g;
    for (int t = 0; t < g.n; ++t) {
        g.b[static_cast<std::size_t>(t)] = b_amp * (U(rng) - 0.4);
        g.z_prev[static_cast<std::size_t>(t)] = 0.6 * (U(rng) - 0.5);
    }
    for (int t = 0; t < g.n; ++t) {
        const int s = g.store_of[static_cast<std::size_t>(t)];
        if (s >= 0) g.dV_prev[static_cast<std::size_t>(s)] = g.volume(t, g.z_prev[static_cast<std::size_t>(t)], nullptr);
    }
    for (const auto& pd : r.ponds)
        if (pd.node < 0) (*r.zpp)[pd.slot] = std::max(0.4 * (U(rng) - 0.4), -pd.po.p_det);
}

}  // namespace

TEST(SparseNewton, MatchesTheDenseSchurPathOnRandomGraphs) {
    // O-11: the same Newton system factored two ways (dense k×k Schur complement, or
    // the full network's sparse LU with a cached pattern) gives the same iterates:
    // 50 random graphs, N sets of 5 to ~400 nodes, composite ponds, floored one-way
    // ports, pins, loops, 3 members each.
    std::mt19937 seeds(1101);
    std::uniform_real_distribution<double> U(0.0, 1.0);
    double worst = 0.0;
    long it_d = 0, it_s = 0, fb_d = 0, fb_s = 0, kmax = 0, sparse_fact = 0, with_ponds = 0;
    for (int trial = 0; trial < 50; ++trial) {
        const unsigned sd = seeds();
        std::mt19937 rng(sd);
        RandomGraph rd;
        if (trial < 20) {   // the generic random graphs (loops, weirs, raised ends)
            rd = makeRandomGraph(rng, 6 + trial * 3, trial % 2 == 0, trial % 4 >= 2, 2);
        } else {            // tree-like with many stores: k up to ~400
            const int ns = 5 + (trial - 20) * 13;
            rd = makeTreeStoreGraph(rng, std::max(60, ns + 40 + static_cast<int>(U(rng) * 80)), ns, trial % 3 == 0, true);
        }
        with_ponds += !rd.ponds.empty();
        // Random pins on the generic graphs too.
        if (trial < 20 && trial % 3 == 1) rd.g.pinned[static_cast<std::size_t>(U(rng) * rd.g.n)] = 1;
        RandomGraph rs = rd;   // copy: shares the pond start-level slots (read only)
        rd.g.k_dense = static_cast<std::size_t>(-1);   // always dense
        rs.g.k_dense = 0;                              // always sparse
        for (int m = 0; m < 3; ++m) {
            setRandomMember(rd, sd * 31u + static_cast<unsigned>(m));
            rs.g.b = rd.g.b; rs.g.z_prev = rd.g.z_prev; rs.g.dV_prev = rd.g.dV_prev;
            rd.g.member = rs.g.member = m;
            std::vector<double> zd = rd.g.z_prev, zs = rs.g.z_prev;
            const bool okd = rd.g.solve(zd), oks = rs.g.solve(zs);
            ASSERT_EQ(okd, oks) << "trial " << trial << " member " << m;
            ASSERT_TRUE(okd) << "trial " << trial << " member " << m;
            double zmax = 0.0, dmax = 0.0;
            for (std::size_t t = 0; t < zd.size(); ++t) { zmax = std::max(zmax, std::fabs(zd[t])); dmax = std::max(dmax, std::fabs(zd[t] - zs[t])); }
            const double rel = dmax / std::max(zmax, 1.0e-300);
            worst = std::max(worst, rel);
            EXPECT_LE(rel, 1.0e-10) << "trial " << trial << " (n " << rd.g.n << ", k " << rd.g.stats.k_max << ") member " << m;
        }
        EXPECT_EQ(rd.g.stats.sparse_factorisations, 0) << "trial " << trial;
        if (rs.g.stats.newton_iters > 0) EXPECT_GT(rs.g.stats.sparse_factorisations, 0) << "trial " << trial;
        EXPECT_EQ(rs.g.stats.factorisations, 0) << "trial " << trial << ": the sparse path forms no dense system";
        EXPECT_EQ(rd.g.stats.l_factorisations, rs.g.stats.l_factorisations);
        EXPECT_EQ(rd.g.stats.k_max, rs.g.stats.k_max);
        it_d += rd.g.stats.newton_iters; it_s += rs.g.stats.newton_iters;
        fb_d += rd.g.stats.jacobi_fallbacks; fb_s += rs.g.stats.jacobi_fallbacks;
        kmax = std::max(kmax, rd.g.stats.k_max);
        sparse_fact += rs.g.stats.sparse_factorisations;
    }
    EXPECT_GE(kmax, 300);
    EXPECT_GE(with_ponds, 25);
    EXPECT_EQ(fb_d, fb_s);
    std::printf("[PH-O11] 50 random graphs x 3 members (k <= %ld): worst max|dz|/max|z| dense vs sparse %.2e; Newton iterations dense %ld / sparse %ld; "
                "Jacobi fallbacks %ld / %ld; sparse factorisations %ld; graphs with ponds %ld\n",
                kmax, worst, it_d, it_s, fb_d, fb_s, sparse_fact, with_ponds);
}

TEST(SparseNewton, CrossoverMicroBenchmark) {
    // Per Newton iteration, the dense k×k Schur path against the sparse full-network
    // path on a chain-like network of ~1,300 nodes whose N set is k stores (each
    // drains through a weir out of the system). The time is prepare() plus 4 members,
    // divided by the Newton iterations: the share of prepare() per member is what a
    // step with ~4 members pays; the engine runs more members per step, which
    // favours the dense path slightly at small k (its prepare is the k L-solves).
    using clk = std::chrono::steady_clock;
    const std::vector<int> ks = {10, 30, 100, 300,
#ifdef NDEBUG
                                 1000
#endif
    };
    int crossover = -1;
    double t_d_last = 0.0, t_s_last = 0.0;
    int k_last = 0;
    for (int ks_i : ks) {
        double ms[2] = {0.0, 0.0};
        long iters[2] = {0, 0};
        for (int path = 0; path < 2; ++path) {   // 0 dense, 1 sparse
            std::mt19937 rng(7000 + static_cast<unsigned>(ks_i));
            RandomGraph r = makeTreeStoreGraph(rng, 1300, ks_i, true, false);
            r.g.k_dense = path == 0 ? static_cast<std::size_t>(-1) : 0;
            setRandomMember(r, 5u);
            const auto t0 = clk::now();
            r.g.prepare();
            for (int m = 0; m < 4; ++m) {
                setRandomMember(r, 5u + static_cast<unsigned>(m), 6.0);
                r.g.member = m;
                std::vector<double> z = r.g.z_prev;
                EXPECT_TRUE(r.g.solve(z));
            }
            ms[path] = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
            iters[path] = r.g.stats.newton_iters;
            EXPECT_EQ(r.g.stats.jacobi_fallbacks, 0);
            EXPECT_EQ(r.g.stats.k_max, ks_i);
        }
        const double pd = ms[0] / std::max<long>(iters[0], 1), ps = ms[1] / std::max<long>(iters[1], 1);
        std::printf("[PH-O11] k %4d: dense %9.3f ms (%ld Newton iterations, %.3f ms/iteration) | sparse %9.3f ms (%ld, %.3f ms/iteration)\n",
                    ks_i, ms[0], iters[0], pd, ms[1], iters[1], ps);
        if (crossover < 0 && ps <= pd) crossover = ks_i;
        t_d_last = ms[0]; t_s_last = ms[1]; k_last = ks_i;
    }
    std::printf("[PH-O11] smallest measured k at which the sparse path is not slower: %d\n", crossover);
#ifdef NDEBUG
    EXPECT_EQ(k_last, 1000);
    EXPECT_LT(t_s_last, t_d_last) << "at k = 1000 the sparse path must beat the dense Schur path";
#else
    (void)t_d_last; (void)t_s_last; (void)k_last;
#endif
}

// ===========================================================================
// Step 3 — engine wiring (hook rom1dStorageMembersEnabled())
// ===========================================================================

// ---------------------------------------------------------------------------
// Admission closure (PORT_HAMILTONIAN O-8, uncertainty/RomAdmissionClosure.hpp)
// ---------------------------------------------------------------------------
namespace {
using CL = ClosureLink;
CL lk(int a, int b, CL::Kind k = CL::FREE) { CL l; l.a = a; l.b = b; l.kind = k; return l; }
std::vector<char> flags(int n, std::initializer_list<int> on) {
    std::vector<char> f(static_cast<std::size_t>(n), 0);
    for (int t : on) f[static_cast<std::size_t>(t)] = 1;
    return f;
}
/// Independent check: a non-admitted node is grounded when a breadth-first
/// walk over free links between non-admitted nodes reaches the outside.
bool groundedByWalk(int n, const std::vector<CL>& links, const std::vector<char>& adm, int start) {
    std::vector<char> seen(static_cast<std::size_t>(n), 0);
    std::vector<int> st{start};
    seen[static_cast<std::size_t>(start)] = 1;
    while (!st.empty()) {
        const int x = st.back();
        st.pop_back();
        for (const CL& l : links) {
            if (l.kind != CL::FREE || (l.a != x && l.b != x)) continue;
            const int y = l.a == x ? l.b : l.a;
            if (y < 0) return true;
            if (adm[static_cast<std::size_t>(y)] || seen[static_cast<std::size_t>(y)]) continue;
            seen[static_cast<std::size_t>(y)] = 1;
            st.push_back(y);
        }
    }
    return false;
}
}  // namespace

// Depth-1 closure (O-8b, O-12). A wet well W (admitted: a pump leaves it) with the
// chain U1 -> U2 -> W behind it by two-way conduits: nothing joins (the conduit into
// W is a floored line; O-12 dropped the FREE clause). A node behind W by a ONE_WAY
// port joins, and only it: U1 behind it stays linear.
TEST(AdmissionClosure, ChainBehindAPumpAdmitsOnlyOneWayFarEnds) {
    // 0 U1, 1 U2, 2 W (admitted), 3 D
    std::vector<CL> links{lk(0, 1), lk(1, 2), lk(2, 3, CL::ONE_WAY), lk(3, -1)};
    EXPECT_TRUE(admissionClosure(4, links, flags(4, {2})).empty());
    // A one-way line from W into U2: U2 is the far end of a one-way link and joins.
    links.push_back(lk(2, 1, CL::ONE_WAY));
    const auto add = admissionClosure(4, links, flags(4, {2}));
    EXPECT_EQ(add, (std::vector<int>{1}));
    EXPECT_TRUE(std::find(add.begin(), add.end(), 0) == add.end()) << "the component rule would admit U1 too";
    // Mutation: an overflow pipe from U2 to the outfall grounds it.
    auto with_free = links;
    with_free.push_back(lk(1, -1));
    EXPECT_TRUE(admissionClosure(4, with_free, flags(4, {2})).empty());
}

// Two basins joined by a raised-end pipe: B1 is admitted, B2 (the far end of
// the RAISED link) joins; B2's lateral L does not (one hop). Both basins are
// stores. A free conduit from B2 to the trunk grounds B2.
TEST(AdmissionClosure, BasinBehindARaisedEndPipeIsAdmitted) {
    // 0 B1 (admitted), 1 B2, 2 L, 3 T
    const std::vector<CL> links{lk(0, 1, CL::RAISED), lk(2, 1), lk(0, 3, CL::ONE_WAY), lk(3, -1)};
    EXPECT_EQ(admissionClosure(4, links, flags(4, {0})), (std::vector<int>{1}));
    auto with_free = links;
    with_free.push_back(lk(1, 3));
    EXPECT_TRUE(admissionClosure(4, with_free, flags(4, {0})).empty());
}

// O-12: a lateral behind a plain two-way (floored-line) conduit into an admitted
// store is NOT admitted. The engine floors every conduit touching an admitted node
// at both ends (the line's flow carries the dry factor of the end the water leaves),
// so the lateral's deficit is bounded through the line; only ONE_WAY and RAISED
// far ends need a floor of their own. A grounded flag changes nothing here.
TEST(AdmissionClosure, LateralBehindAFlooredTwoWayLineStaysLinear) {
    // 0 A (admitted), 1 L1, 2 L2, 3 X, 4 P
    const std::vector<CL> links{lk(2, 1), lk(1, 0), lk(3, 0), lk(3, 4), lk(4, -1), lk(0, 4, CL::ONE_WAY)};
    EXPECT_TRUE(admissionClosure(5, links, flags(5, {0})).empty());
    EXPECT_TRUE(admissionClosure(5, links, flags(5, {0}), flags(5, {2})).empty());
    // The one-way far end still joins when it has no grounded path.
    const std::vector<CL> ow{lk(1, 0, CL::ONE_WAY), lk(2, 1)};
    EXPECT_EQ(admissionClosure(3, ow, flags(3, {0})), (std::vector<int>{1}));
}

// O-12 regression: 20 junctions in a chain, two-way conduits everywhere, one admitted
// store in the middle, the far end open to the outside or not. Nothing joins.
// Mutation: with the FREE clause the closure admitted both neighbours of the store
// (and, applied again under sticky admission, the whole chain).
TEST(AdmissionClosure, TwoWayChainAroundOneAdmittedStoreAdmitsNothing) {
    const int n = 20;
    std::vector<CL> links;
    for (int t = 0; t + 1 < n; ++t) links.push_back(lk(t, t + 1));
    for (const bool open_end : {false, true}) {
        auto l = links;
        if (open_end) l.push_back(lk(n - 1, -1));
        std::vector<char> adm = flags(n, {10});
        EXPECT_TRUE(admissionClosure(n, l, adm).empty()) << "open_end " << open_end;
        // Repeated application (sticky admission) stays empty.
        adm[3] = 1;
        EXPECT_TRUE(admissionClosure(n, l, adm).empty());
    }
}

// Decision (d): a node behind a one-way port (G71O020 behind G71F12P's orifice
// line) with ANOTHER free path to the grounded plane is not admitted. Its deficit
// is carried to the outfall by the linear response, and the engine's floored
// line holds the dry factor at both ends, so the line cannot pull it below its
// invert; the free path is the node's drain. Without that path it is admitted.
TEST(AdmissionClosure, NodeWithAFreePathToTheOutfallStaysLinearEvenBehindAOneWayPort) {
    // 0 W (admitted), 1 Y (one-way line from W, free conduit to P), 2 P, 3 Z (one-way from W, no free path)
    const std::vector<CL> links{lk(0, 1, CL::ONE_WAY), lk(1, 2), lk(2, -1), lk(0, 3, CL::ONE_WAY)};
    EXPECT_EQ(admissionClosure(4, links, flags(4, {0})), (std::vector<int>{3}));
}

// Random graphs: only boundary nodes are taken, each taken node is ungrounded
// by an independent walk, each boundary node left out is grounded, and
// repeating the closure terminates.
TEST(AdmissionClosure, BoundaryOnlyUngroundedAndTerminatingOnRandomGraphs) {
    std::mt19937 rng(8);
    for (int trial = 0; trial < 300; ++trial) {
        const int n = 5 + static_cast<int>(rng() % 60);
        std::vector<CL> links;
        for (int t = 1; t < n; ++t) links.push_back(lk(t, static_cast<int>(rng() % static_cast<unsigned>(t)),
                                                      static_cast<CL::Kind>(rng() % 6 == 0 ? 1 + rng() % 2 : 0)));
        const int extra = static_cast<int>(rng() % static_cast<unsigned>(n));
        for (int e = 0; e < extra; ++e) {
            const int a = static_cast<int>(rng() % static_cast<unsigned>(n));
            const int b = rng() % 5 == 0 ? -1 : static_cast<int>(rng() % static_cast<unsigned>(n));
            if (a != b) links.push_back(lk(a, b, static_cast<CL::Kind>(rng() % 4 == 0 ? 1 + rng() % 2 : 0)));
        }
        std::vector<char> adm(static_cast<std::size_t>(n), 0);
        for (int t = 0; t < n; ++t) adm[static_cast<std::size_t>(t)] = rng() % 7 == 0;
        auto isBoundary = [&](int t) {
            for (const CL& l : links)
                if (l.kind != CL::FREE && ((l.a == t && l.b >= 0 && adm[static_cast<std::size_t>(l.b)]) || (l.b == t && l.a >= 0 && adm[static_cast<std::size_t>(l.a)]))) return true;
            return false;
        };
        const auto add = admissionClosure(n, links, adm);
        std::vector<char> taken(static_cast<std::size_t>(n), 0);
        for (int t : add) {
            ASSERT_FALSE(adm[static_cast<std::size_t>(t)]);
            EXPECT_TRUE(isBoundary(t)) << "trial " << trial << " node " << t;
            EXPECT_FALSE(groundedByWalk(n, links, adm, t)) << "trial " << trial << " node " << t;
            taken[static_cast<std::size_t>(t)] = 1;
        }
        for (int t = 0; t < n; ++t)
            if (!adm[static_cast<std::size_t>(t)] && !taken[static_cast<std::size_t>(t)] && isBoundary(t))
                EXPECT_TRUE(groundedByWalk(n, links, adm, t)) << "trial " << trial << " node " << t;
        // Repeated application (sticky admission) reaches a fixed point.
        auto cur = adm;
        int rounds = 0;
        for (; rounds <= n; ++rounds) {
            const auto more = admissionClosure(n, links, cur);
            if (more.empty()) break;
            for (int t : more) { ASSERT_FALSE(cur[static_cast<std::size_t>(t)]); cur[static_cast<std::size_t>(t)] = 1; }
        }
        EXPECT_LE(rounds, n) << "trial " << trial;
    }
}

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
// Main-run volume-mismatch sharing (SWMMEngine::stepRom1dStorageMembers, "store_scale"):
// the main run's storage change V(d*_new) - V(d*_old) differs from dt*net* by an
// unaccounted m. A loss (m < 0) is shared by members in proportion to their water:
// every member's absolute water is multiplied by s = (V(d*_new) - dt*net*)/V(d*_old),
// i.e. its deviation volume is carried as s*dV (s < 1); a gain is shared equally
// (dV carried unchanged, s = 1). The clamp keeps s in [0, 1], so a deviation is never
// scaled up. Identities asserted below, per step and store:
//   carried_i = s * dV_old_i                         (what the solve started from)
//   0 <= s <= 1, |carried_i| <= |dV_old_i|           (never scaled up)
//   (V_old_i - V_carried_i) / V_old_i = 1 - s        with V_i = W + dV_i, V_carried_i = s*W_old + carried_i
//                                                    (loss share is the same fraction for every member)
//   dV_old_i <= dV_old_j  =>  carried_i <= carried_j (the sharing preserves member order)
// The fixture: a basin behind a zero-crest weir (it drains to near empty, crossing the
// 0.01 ft dry threshold), rain-only soft forcing, several members.
// ---------------------------------------------------------------------------
namespace {
struct ShareStep {
    std::vector<double> scale, scale_, wold, wafter, carried, dv, nly, corr;
};
std::vector<ShareStep> runSharing(const std::string& inp, const std::string& tag, std::size_t& M, std::size_t& nb) {
    std::vector<ShareStep> out;
    const std::string pfx = "rom_member_ports_share_" + std::to_string(getpid()) + "_" + tag;
    const std::string ip = pfx + ".inp", rp = pfx + ".rpt";
    { std::ofstream f(ip); f << inp; }
    SWMM_Engine h = swmm_engine_create();
    auto cleanup = [&]() {
        swmm_engine_end(h); swmm_engine_close(h); swmm_engine_destroy(h);
        for (const char* ext : {".inp", ".rpt", ".uncertainty.csv", ".rom_threshold.csv", ".rom_diag.csv", ".out"})
            std::remove((pfx + ext).c_str());
    };
    if (swmm_engine_open(h, ip.c_str(), rp.c_str(), nullptr, nullptr) != 0) { cleanup(); return out; }
    auto* eng = static_cast<openswmm::SWMMEngine*>(h);
    eng->rom1dStorageMembersEnabled() = true;
    if (swmm_engine_initialize(h) != 0 || swmm_engine_start(h, 0) != 0) { cleanup(); return out; }
    double el = 1.0;
    long seen = 0;
    for (int guard = 0; el != 0.0 && guard < 200000; ++guard) {
        if (swmm_engine_step(h, &el) != 0) break;
        if (eng->rom1dPortGraphSteps() == seen) continue;
        seen = eng->rom1dPortGraphSteps();
        ShareStep st;
        st.scale = eng->rom1dStoreCarryFloor();   // lower bound of the carried dV
        st.scale_ = eng->rom1dStoreCarryScale();
        st.wold = eng->rom1dStoreWaterBefore();
        st.wafter = eng->rom1dStoreWaterAfter();
        st.carried = eng->rom1dStoreCarriedVolumes();
        st.dv = eng->rom1dMemberStoreVolumes();
        st.nly = eng->rom1dStorageMemberLevels();
        st.corr = eng->rom1dMemberCorrection();
        nb = eng->rom1dStorageMemberNodes().size();
        M = nb ? st.dv.size() / nb : 0;
        out.push_back(std::move(st));
        if (el > 0.0 && el * 86400.0 >= 5400.0) break;
    }
    cleanup();
    return out;
}
}  // namespace

TEST(StoreVolumeMismatchSharing, ZeroDataGivesZeroCorrectionExactly) {
    std::string inp = engineFixture(true, true, 0.0);
    const auto pos = inp.find("CV 0.2");
    ASSERT_NE(pos, std::string::npos);
    inp.replace(pos, 6, "CV 0.0");
    std::size_t M = 0, nb = 0;
    const auto run = runSharing(inp, "zero", M, nb);
    ASSERT_FALSE(run.empty());
    ASSERT_GE(nb, 1u);
    for (const auto& st : run) {
        for (double c : st.corr) ASSERT_EQ(c, 0.0);
        for (double v : st.dv) ASSERT_EQ(v, 0.0);
        for (double v : st.nly) ASSERT_EQ(v, 0.0);
    }
}

TEST(StoreVolumeMismatchSharing, MismatchSharedBothSidesFlooredAtEmptyOrderKept) {
    std::size_t M = 0, nb = 0;
    const auto run = runSharing(engineFixture(true, true, 0.0), "share", M, nb);
    ASSERT_GE(run.size(), 100u);
    ASSERT_GE(nb, 1u);
    ASSERT_GE(M, 4u);
    long loss_steps = 0, gain_steps = 0, dry_cross = 0, ordered_checks = 0;
    double min_depth_w = 1e300;
    for (std::size_t n = 1; n < run.size(); ++n) {
        const ShareStep& cur = run[n];
        const ShareStep& prev = run[n - 1];
        ASSERT_EQ(cur.carried.size(), nb * M);
        ASSERT_EQ(prev.dv.size(), nb * M);
        for (std::size_t b = 0; b < nb; ++b) {
            const double sc = cur.scale_[b];
            const double floor_dv = cur.scale[b];
            ASSERT_GE(sc, 0.0);
            ASSERT_LE(floor_dv, 0.0);
            const double Wold = cur.wold[b];
            const double Wnew = -floor_dv;
            if (sc < 1.0) ++loss_steps; else ++gain_steps;
            if (Wold < 0.05 * run[0].wafter[b] + 1e-9 || Wold < 1e-6) ++dry_cross;
            min_depth_w = std::min(min_depth_w, Wold);
            for (std::size_t i = 0; i < M; ++i) {
                const double old_dv = prev.dv[b * M + i];
                const double carried = cur.carried[b * M + i];
                // O-15: the share s applies on a loss and on a gain; bounded below by empty.
                EXPECT_EQ(carried, std::max(sc * old_dv, floor_dv)) << "step " << n << " member " << i;
                EXPECT_GE(Wnew + carried, -1e-9 * std::max(Wnew, 1.0)) << "member water negative, step " << n;
            }
            // order preservation of the sharing, and of the levels the solve returns
            for (std::size_t i = 0; i < M; ++i)
                for (std::size_t j = 0; j < M; ++j)
                    if (prev.dv[b * M + i] <= prev.dv[b * M + j]) {
                        EXPECT_LE(cur.carried[b * M + i], cur.carried[b * M + j]) << "carried order, step " << n;
                        ++ordered_checks;
                    }
        }
    }
    // Member order through the run: the rank of the levels by member, fixed at the first spread,
    // must hold weakly (ties allowed) at every later step, through the dry threshold.
    std::vector<std::size_t> rank;
    int sign = 0;
    long order_violations = 0, order_steps = 0;
    for (std::size_t n = 0; n < run.size(); ++n) {
        const auto& z = run[n].nly;   // store x member levels (ft deviation)
        for (std::size_t b = 0; b < 1; ++b) {
            std::vector<double> v(z.begin() + static_cast<std::ptrdiff_t>(b * M), z.begin() + static_cast<std::ptrdiff_t>((b + 1) * M));
            const double spread = *std::max_element(v.begin(), v.end()) - *std::min_element(v.begin(), v.end());
            if (rank.empty()) {
                if (spread < 1e-4) continue;
                rank.resize(M);
                for (std::size_t i = 0; i < M; ++i) rank[i] = i;
                std::sort(rank.begin(), rank.end(), [&](std::size_t a, std::size_t c) { return v[a] < v[c]; });
                sign = 1;
                continue;
            }
            ++order_steps;
            for (std::size_t k = 0; k + 1 < M; ++k)
                if (v[rank[k]] > v[rank[k + 1]] + 1e-12) ++order_violations;
        }
    }
    std::printf("[O-15 sharing] steps %zu, loss steps %ld, gain steps %ld, near-dry steps %ld, min main water %.4g ft^3, order checks %ld, rank steps %ld\n",
                run.size(), loss_steps, gain_steps, dry_cross, min_depth_w, ordered_checks, order_steps);
    ASSERT_GT(order_steps, 20) << "the members never spread; the order check proves nothing";
    EXPECT_EQ(order_violations, 0);
    (void)sign;
    EXPECT_GT(loss_steps + gain_steps, 0);
}

// O-15 (e): the carry cannot ratchet. A store whose main run alternates a loss and a gain of
// equal size (so its water returns every cycle) must hand each member back its deviation volume.
// The earlier rule (s clamped <= 1: a loss shrinks every deviation, a gain leaves it) decays it
// geometrically; asserted as the mutation.
TEST(StoreVolumeMismatchSharing, AlternatingMismatchDoesNotRatchetTheDeviation) {
    namespace PP = openswmm::uncertainty::ports;
    const double W0 = 1000.0, m = 40.0, dry = 1e-6;     // main water, mismatch size (ft^3)
    const std::vector<double> dv0 = {-120.0, -30.0, 0.0, 15.0, 90.0};
    std::vector<double> now = dv0, old_rule = dv0;
    double W = W0;
    for (int cycle = 0; cycle < 100; ++cycle) {
        for (double sign : {-1.0, 1.0}) {
            const double Wn = W + sign * m;             // the mismatch lands on the main run's water (net* = 0)
            for (std::size_t i = 0; i < now.size(); ++i) {
                now[i] = PP::carryStoreVolume(PP::storeCarryScale(W, Wn, dry) * now[i], -Wn);
                old_rule[i] *= std::min(PP::storeCarryScale(W, Wn, dry), 1.0);
            }
            W = Wn;
        }
    }
    double worst_old = 0.0;
    for (std::size_t i = 0; i < dv0.size(); ++i) {
        EXPECT_NEAR(now[i], dv0[i], 1e-10) << "member " << i;
        if (dv0[i] != 0.0) worst_old = std::max(worst_old, 1.0 - std::fabs(old_rule[i] / dv0[i]));
    }
    EXPECT_GT(worst_old, 0.3) << "mutation: the old rule must visibly ratchet";
    // order kept and zero exact, with the floor engaged
    for (double fl : {-50.0, -100.0}) {
        double prev = PP::carryStoreVolume(dv0[0], fl);
        for (std::size_t i = 1; i < dv0.size(); ++i) {
            const double c = PP::carryStoreVolume(dv0[i], fl);
            EXPECT_GE(c, prev);
            prev = c;
        }
        EXPECT_EQ(PP::carryStoreVolume(0.0, fl), 0.0);
    }
    EXPECT_EQ(PP::storeCarryScale(0.0, 5.0, dry), 1.0);   // near-empty main run: no divisor
}

// ---------------------------------------------------------------------------
// Coupled manhole (O-3): a 1D chain J1..J4 whose head manhole J1 sits under the
// low corner of a sloped 2D patch. Rain on the patch ponds over J1; the
// subcatchment overloads the 0.3 m pipes so J1 also surcharges to the street.
// ---------------------------------------------------------------------------
namespace {

std::string coupledFixture(double cv, double sync = 0.0, double rain = 80.0, bool mesh_rain = true, double diam = 0.3, double len = 30.0) {
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
        f << "TS1 01/01/2026 00:" << std::setfill('0') << std::setw(2) << m << " " << (m < 15 ? rain : 0.0) << "\n";
    f << std::setfill(' ') << "\n";
    f << "[SUBCATCHMENTS]\nS1 RG1 J1 1.0 100 50 1.0 0\n\n[SUBAREAS]\nS1 0.015 0.20 0.00 0.00 100 OUTLET\n\n";
    f << "[INFILTRATION]\nS1 0.0 0.0 0.0 0.0 0.0\n\n";
    f << "[JUNCTIONS]\nJ1 0.0 1.0 0 0 0\nJ2 -0.1 1.0 0 0 0\nJ3 -0.2 1.0 0 0 0\nJ4 -0.3 1.0 0 0 0\n\n";
    f << "[OUTFALLS]\nO1 -0.5 FREE NO\n\n";
    f << "[CONDUITS]\nC1 J1 J2 " << len << " 0.013 0 0 0\nC2 J2 J3 " << len << " 0.013 0 0 0\nC3 J3 J4 " << len << " 0.013 0 0 0\nC4 J4 O1 " << len << " 0.013 0 0 0\n\n";
    f << "[XSECTIONS]\n";
    for (const char* c : {"C1", "C2", "C3", "C4"}) f << c << " CIRCULAR " << diam << " 0 0 0 1\n";
    f << "\n[2D_OPTIONS]\nMAX_TIMESTEP 4\nDRY_DEPTH 0.002\nCOUPLING_CD 0.7\nREPORT_2D NO\n";
    if (sync > 0.0) f << "COUPLING_SYNC " << sync << "\n";
    if (!mesh_rain) f << "RAINFALL_MODE NONE\n";
    f << "\n[2D_VERTICES]\n";
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
    std::vector<double> head, flow, surf, q05, q95, corr, cvol, pending, street, cvol_at_admission;
    std::size_t admitted = 0, admitted_coupled = 0;
    long steps = 0, pg_steps = 0, law_evals = 0, port_evals = 0, solves = 0, newton = 0, jacobi = 0, factor = 0, armijo = 0, lfact = 0, lsolves = 0, kmax = 0;
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
        // The 2D's own volume when the per-member path first exchanged: what the
        // 2D booked by its local orifice law before the manhole was admitted.
        if (tr.cvol_at_admission.empty() && !eng->rom1dStreetExchangeVolume().empty())
            if (const auto* r2 = eng->surfaceRouter2D().rom())
                for (std::size_t k = 0; k < eng->surfaceRouter2D().nodeCouplingPoints().size(); ++k)
                    for (int i = 0; i < r2->n_ensemble; ++i) tr.cvol_at_admission.push_back(r2->couplingVolume(k, i));
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
                tr.pending.push_back(r2->pendingExternalCouplingVolume(k, i));
                tr.max_ext = std::max(tr.max_ext, std::fabs(r2->couplingVolume(k, i)));
            }
    }
    tr.street = eng->rom1dStreetExchangeVolume();
    if (tr.street.empty()) tr.street.assign(tr.cvol.size(), 0.0);
    tr.pg_steps = eng->rom1dPortGraphSteps();
    tr.law_evals = eng->rom1dCouplingLawEvals();
    const auto& st = eng->rom1dPortGraphStats();
    tr.port_evals = st.port_evals; tr.solves = st.solves; tr.newton = st.newton_iters; tr.jacobi = st.jacobi_fallbacks;
    tr.factor = st.factorisations; tr.armijo = st.armijo_failures;
    tr.lfact = st.l_factorisations; tr.lsolves = st.l_solves; tr.kmax = st.k_max;
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
    // Cost diagnostic (plan §6 runtime line): street-orifice law evaluations,
    // port evaluations, Newton iterations and factorisations per per-member
    // step, wall time ON vs OFF over 6 runs each (min / median). Printed, not
    // asserted (wall time in Debug is noisy).
    constexpr int kRuns = 6;
    std::vector<double> w_off, w_on;
    CoupledTrace on;
    for (int r = 0; r < kRuns; ++r) {
        const CoupledTrace off_r = runCoupled(coupledFixture(0.2), "cost_off", false, 0.2);
        const CoupledTrace on_r = runCoupled(coupledFixture(0.2), "cost_on", true, 0.2);
        ASSERT_TRUE(off_r.ok && on_r.ok);
        w_off.push_back(off_r.wall_s); w_on.push_back(on_r.wall_s);
        on = on_r;
    }
    ASSERT_GT(on.pg_steps, 0);
    auto mn = [](std::vector<double> v) { return *std::min_element(v.begin(), v.end()); };
    auto md = [](std::vector<double> v) { std::sort(v.begin(), v.end()); return 0.5 * (v[v.size() / 2] + v[(v.size() - 1) / 2]); };
    const double st = static_cast<double>(on.pg_steps);
    std::printf("[PH-O3 cost] steps %ld, per-member steps %ld, admitted %zu (coupled %zu)\n",
                on.steps, on.pg_steps, on.admitted, on.admitted_coupled);
    std::printf("[PH-O3 cost] per step: orifice-law evals %.1f, port evals %.1f, newton iters %.1f, factorisations %.1f; "
                "totals: solves %ld, newton %ld, factorisations %ld, armijo failures %ld, jacobi %ld\n",
                static_cast<double>(on.law_evals) / st, static_cast<double>(on.port_evals) / st,
                static_cast<double>(on.newton) / st, static_cast<double>(on.factor) / st,
                on.solves, on.newton, on.factor, on.armijo, on.jacobi);
    std::printf("[PH-O5 cost] k x k factorisations %ld (k <= %ld), L factorisations %ld for %ld per-member steps (%d members), L solves %ld\n",
                on.factor, on.kmax, on.lfact, on.pg_steps, 20, on.lsolves);
    // O-5: the linear part is factored once per step and shared by the members.
    EXPECT_EQ(on.lfact, on.pg_steps);
    std::printf("[PH-O3 cost] wall OFF min %.3f med %.3f s, ON min %.3f med %.3f s, ratio min %.3f med %.3f\n",
                mn(w_off), md(w_off), mn(w_on), md(w_on), mn(w_on) / mn(w_off), md(w_on) / md(w_off));
}
TEST(PerMemberPortsCoupled, StreetExchangeIsBookedOnceUnderSyncBatches) {
    // O-7 (Bellinge demo_ph5, 2026-11-05): the 2D advances in sync batches of
    // several routing steps (here COUPLING_SYNC 30 s over 5 s routing steps),
    // while the per-member solve runs every routing step and hands its street
    // exchange to the 2D ROM each time. Every step's volume must be booked: the
    // 2D ROM's booked plus pending volume equals the sum of what the members
    // moved through the orifice, per coupling point and member. When a later
    // step REPLACED the earlier ones, only the batch's last step was booked
    // (here 10.1 of 12.3 m^3 left unbooked for the worst member) and the pond,
    // re-read from the un-updated 2D state, refilled itself every routing step:
    // on Bellinge member ponds held shallow laterals at their rim for hours (1D
    // bands 1.4-2.3 m vs an MC span of 0-2 cm). Small 0.15 m pipes, 80 m long:
    // the head manhole surcharges to the street. Volume the 2D booked by its own
    // law before the manhole was admitted is not part of the per-member path.
    // The pond level seeing pending volumes is pinned in test_2d_spectral_rom
    // (ExternalCouplingVolumesAddUpUntilBookedAndShowInThePond).
    for (double sync : {30.0, 0.0}) {
        const CoupledTrace on = runCoupled(coupledFixture(0.2, sync, 20.0, false, 0.15, 80.0), "book", true, 0.2);
        ASSERT_TRUE(on.ok);
        ASSERT_GE(on.admitted_coupled, 1u);
        ASSERT_EQ(on.street.size(), on.cvol.size());
        ASSERT_EQ(on.cvol_at_admission.size(), on.cvol.size());
        double worst = 0.0, scale = 0.0;
        for (std::size_t k = 0; k < on.street.size(); ++k) {
            worst = std::max(worst, std::fabs(on.cvol[k] - on.cvol_at_admission[k] + on.pending[k] - on.street[k]));
            scale = std::max(scale, std::fabs(on.street[k]));
        }
        std::printf("[O-7] sync %.0f s: max |street exchange| %.3f m^3, max |booked + pending - street| %.3g m^3\n", sync, scale, worst);
        EXPECT_GT(scale, 0.1) << "the members must reach the street, or the check proves nothing";
        EXPECT_LE(worst, 1.0e-9 * scale) << "sync " << sync;
    }
}

// ---------------------------------------------------------------------------
// R-1: a failed member solve is rejected for the step; the member rides the
// shared plane (C_i = 0, level = plane level, no street exchange booked, next
// step starts from the plane level). The failure is real: the seam starves the
// member's Newton and Jacobi iterations (max 0), so solve() returns false.
// ---------------------------------------------------------------------------
namespace {
struct RejSnap {
    std::vector<double> corr, nly, street, pending;
    long rejected = 0, jfail = 0, pg_steps = 0, solves = 0;
    std::size_t un = 0, nb = 0;
};
struct RejRun {
    RejSnap before, at, after;
    bool ok = false;
};
RejRun runWithFailure(int member, long K, const std::string& tag) {
    RejRun r;
    const std::string pfx = "rom_member_ports_rej_" + std::to_string(getpid()) + "_" + tag;
    const std::string ip = pfx + ".inp", rp = pfx + ".rpt";
    { std::ofstream f(ip); f << coupledFixture(0.2); }
    SWMM_Engine h = swmm_engine_create();
    auto cleanup = [&]() {
        swmm_engine_end(h); swmm_engine_close(h); swmm_engine_destroy(h);
        for (const char* ext : {".inp", ".rpt", ".uncertainty.csv", ".rom_threshold.csv", ".rom_diag.csv", ".out"})
            std::remove((pfx + ext).c_str());
    };
    if (swmm_engine_open(h, ip.c_str(), rp.c_str(), nullptr, nullptr) != 0) { cleanup(); return r; }
    auto* eng = static_cast<openswmm::SWMMEngine*>(h);
    auto& o = eng->surfaceRouter2D().options();
    o.enable_rom = true; o.rom_members = 20; o.rom_modes = 6;
    o.rom_mannings_pert = 0.2; o.rom_rainfall_pert = 0.2; o.rom_cd_pert = 0.1;
    eng->rom1dStorageMembersEnabled() = true;
    if (member >= 0) eng->rom1dForceMemberSolveFailureForTest(member, K);
    if (swmm_engine_initialize(h) != 0 || swmm_engine_start(h, 0) != 0) { cleanup(); return r; }
    auto snap = [&](RejSnap& s) {
        s.corr = eng->rom1dMemberCorrection();
        s.nly = eng->rom1dStorageMemberLevels();
        s.street = eng->rom1dStreetExchangeVolume();
        s.rejected = eng->rom1dMemberSolvesRejected();
        s.jfail = eng->rom1dPortGraphStats().jacobi_failed;
        s.pg_steps = eng->rom1dPortGraphSteps();
        s.solves = eng->rom1dPortGraphStats().solves;
        s.nb = eng->rom1dStorageMemberNodes().size();
        s.un = s.corr.size() / 20;
        if (const auto* r2 = eng->surfaceRouter2D().rom())
            for (std::size_t k = 0; k < eng->surfaceRouter2D().nodeCouplingPoints().size(); ++k)
                for (int i = 0; i < r2->n_ensemble; ++i) s.pending.push_back(r2->pendingExternalCouplingVolume(k, i));
    };
    auto stepTo = [&](long target) {
        double el = 1.0;
        for (int guard = 0; guard < 200000 && eng->rom1dPortGraphSteps() < target; ++guard)
            if (swmm_engine_step(h, &el) != 0 || el == 0.0) break;
        return eng->rom1dPortGraphSteps() >= target;
    };
    if (stepTo(K - 1)) {
        snap(r.before);
        if (stepTo(K)) {
            snap(r.at);
            if (stepTo(K + 1)) { snap(r.after); r.ok = true; }
        }
    }
    cleanup();
    return r;
}
}  // namespace

TEST(PerMemberPortsCoupled, FailedMemberSolveIsRejectedAndRidesThePlane) {
    constexpr long K = 60;
    const RejRun twin = runWithFailure(-1, K, "twin");
    ASSERT_TRUE(twin.ok);
    ASSERT_GE(twin.at.nb, 1u);
    ASSERT_EQ(twin.at.rejected, 0);
    // The member whose correction is largest at the step: the one whose failed iterate would show.
    const std::size_t un = twin.at.un;
    int m = 0;
    double big = 0.0;
    for (int i = 0; i < 20; ++i) {
        double c = 0.0;
        for (std::size_t t = 0; t < un; ++t) c = std::max(c, std::fabs(twin.at.corr[static_cast<std::size_t>(i) * un + t]));
        if (c > big) { big = c; m = i; }
    }
    ASSERT_GT(big, 1e-6) << "the twin's members must differ from the plane, or the test proves nothing";
    const auto um = static_cast<std::size_t>(m);

    const RejRun run = runWithFailure(m, K, "fail");
    ASSERT_TRUE(run.ok);
    ASSERT_EQ(run.at.un, un);
    // (counter) exactly one rejected member-step; the solver's own failure counter agrees.
    EXPECT_EQ(run.before.rejected, 0);
    EXPECT_EQ(run.at.rejected, 1);
    EXPECT_EQ(run.at.jfail - run.before.jfail, 1);
    // (i) C_i = 0 exactly; the other members are bit-identical to the twin's.
    for (std::size_t t = 0; t < un; ++t) EXPECT_EQ(run.at.corr[um * un + t], 0.0) << "node " << t;
    for (int i = 0; i < 20; ++i) {
        if (i == m) continue;
        for (std::size_t t = 0; t < un; ++t)
            ASSERT_EQ(run.at.corr[static_cast<std::size_t>(i) * un + t], twin.at.corr[static_cast<std::size_t>(i) * un + t]) << "member " << i;
    }
    // (i) the published store level is the plane level: twin level minus the twin's correction at that node.
    for (std::size_t b = 0; b < run.at.nb; ++b) {
        double lvl_twin = twin.at.nly[b * 20 + um];
        // the correction at the store node: find the node whose twin level matches z (z = u + corr)
        double best = 1e300;
        for (std::size_t t = 0; t < un; ++t) {
            const double plane = lvl_twin - twin.at.corr[um * un + t];
            best = std::min(best, std::fabs(run.at.nly[b * 20 + um] - plane));
        }
        EXPECT_LT(best, 1e-12) << "store " << b;
    }
    // (ii) no street exchange handed to the 2D ROM for the member this step (its ext_vol slot stays
    // NaN, which addExternalCouplingVolume skips); the twin hands over some. (The 2D ROM's pending/booked
    // totals also move with its sync flushes and its own law, so the cumulative hand-over is the measure.)
    ASSERT_EQ(run.at.street.size(), twin.at.street.size());
    ASSERT_FALSE(run.at.street.empty());
    ASSERT_EQ(run.before.street.size(), run.at.street.size());
    const std::size_t ncp = run.at.street.size() / 20;
    double twin_move = 0.0;
    for (std::size_t k = 0; k < ncp; ++k) {
        const std::size_t ix = k * 20 + um;
        EXPECT_EQ(run.at.street[ix] - run.before.street[ix], 0.0) << "cp " << k;
        twin_move = std::max(twin_move, std::fabs(twin.at.street[ix] - twin.before.street[ix]));
    }
    EXPECT_GT(twin_move, 0.0) << "the unfailed member must exchange with the street, or (ii) proves nothing";
    // (iii)/(iv)/(v) the next step solves normally from the plane level: no further rejection,
    // no further solver failure, a finite correction and a booked exchange again.
    EXPECT_EQ(run.after.rejected, 1);
    EXPECT_EQ(run.after.jfail, run.at.jfail);
    for (std::size_t t = 0; t < un; ++t) EXPECT_TRUE(std::isfinite(run.after.corr[um * un + t]));
    double moved = 0.0;
    for (std::size_t k = 0; k < ncp; ++k) moved = std::max(moved, std::fabs(run.after.street[k * 20 + um] - run.at.street[k * 20 + um]));
    EXPECT_GT(moved, 0.0) << "the member must book its exchange again on the step after the rejection";
}

// ---------------------------------------------------------------------------
// O-14: the additive planes carry the members' extra VOLUME (PORT_HAMILTONIAN
// §6 O-14). A small-storage junction behind an undersized pipe takes a sharp
// rain burst: the pipe runs full, the junction and its inflow pipe surcharge
// (S falls to MIN_SURFAREA), then drain on the recession (S rises to half the
// top-width area of the pipes again). Carrying head across that rise booked
// (S_new - S_old)·Psi of water no member has.
// ---------------------------------------------------------------------------
namespace {

std::string burstFixture(double rain_scale, bool soft) {
    std::ostringstream f;
    f << std::setprecision(10);
    f << "[TITLE]\nO-14 burst into a small-storage junction\n\n[OPTIONS]\n";
    f << "FLOW_UNITS CFS\nINFILTRATION HORTON\nFLOW_ROUTING DYNWAVE\n";
    f << "START_DATE 01/01/2025\nSTART_TIME 00:00:00\nREPORT_START_DATE 01/01/2025\nREPORT_START_TIME 00:00:00\n";
    f << "END_DATE 01/01/2025\nEND_TIME 01:30:00\n";
    f << "REPORT_STEP 00:05:00\nWET_STEP 00:01:00\nDRY_STEP 00:05:00\nROUTING_STEP 00:00:05\n";
    f << "MINIMUM_STEP 0.5\nTHREADS 1\n\n[EVAPORATION]\nCONSTANT 0.0\n\n";
    f << "[RAINGAGES]\nRG1 INTENSITY 0:01 1.0 TIMESERIES TS0\n\n[TIMESERIES]\n";
    for (int m = 0; m <= 90; ++m) {   // a 10-minute triangular burst, 6 in/hr peak at 0:25
        const double tri = (m >= 20 && m <= 30) ? (1.0 - std::fabs(m - 25.0) / 5.0) : 0.0;
        f << "TS0 01/01/2025 " << std::setfill('0') << std::setw(2) << m / 60 << ":" << std::setfill('0') << std::setw(2)
          << m % 60 << " " << 6.0 * tri * rain_scale << "\n";
    }
    f << std::setfill(' ') << "\n";
    f << "[SUBCATCHMENTS]\nS1 RG1 J0 4.0 100 600 1.0 0\n\n[SUBAREAS]\nS1 0.015 0.20 0.00 0.00 100 OUTLET\n\n";
    f << "[INFILTRATION]\nS1 0.0 0.0 0.0 0.0 0.0\n\n";
    f << "[JUNCTIONS]\nJ0 101 12 0 0 0\nJ1 100 12 0 0 0\nJ2 99 10 0 0 0\nJ3 98 10 0 0 0\n\n";
    f << "[OUTFALLS]\nO1 97 FREE NO\n\n";
    f << "[CONDUITS]\nC0 J0 J1 200 0.013 0 0 0 0\nC1 J1 J2 200 0.013 0 0 0 0\nC2 J2 J3 200 0.013 0 0 0 0\nC3 J3 O1 200 0.013 0 0 0 0\n\n";
    f << "[XSECTIONS]\nC0 CIRCULAR 1.5 0 0 0 1\nC1 CIRCULAR 0.75 0 0 0 1\nC2 CIRCULAR 3 0 0 0 1\nC3 CIRCULAR 3 0 0 0 1\n\n";
    f << "[REPORT]\nINPUT NO\nCONTINUITY YES\nNODES ALL\nLINKS ALL\n\n";
    if (soft) f << "[SOFT_RAINGAGES]\nRG1 NORMAL CV 0.20\n";
    return f.str();
}

struct BurstRun {
    std::vector<double> t, h1;                 // elapsed s, head at J1 (ft)
    std::vector<double> psi1, S1, vol, src;    // plane 0 at J1, S at J1, plane volume, cumulative source (ft^3)
    double S1_min = 1.0e300, S1_max = 0.0;
    bool ok = false;
};

BurstRun runBurst(double rain_scale, bool soft, const std::string& tag) {
    BurstRun r;
    const std::string pfx = "rom_o14_" + std::to_string(getpid()) + "_" + tag;
    const std::string ip = pfx + ".inp", rp = pfx + ".rpt";
    { std::ofstream f(ip); f << burstFixture(rain_scale, soft); }
    SWMM_Engine h = swmm_engine_create();
    auto cleanup = [&]() {
        swmm_engine_end(h); swmm_engine_close(h); swmm_engine_destroy(h);
        for (const char* ext : {".inp", ".rpt", ".uncertainty.csv", ".rom_threshold.csv", ".rom_diag.csv", ".out"})
            std::remove((pfx + ext).c_str());
    };
    if (swmm_engine_open(h, ip.c_str(), rp.c_str(), nullptr, nullptr) != 0) { cleanup(); return r; }
    auto* eng = static_cast<openswmm::SWMMEngine*>(h);
    if (swmm_engine_initialize(h) != 0 || swmm_engine_start(h, 0) != 0) { cleanup(); return r; }
    const auto& ctx = eng->context();
    const int j1 = ctx.node_names.find("J1");
    double el = 1.0, t_prev = 0.0, src = 0.0;
    for (int guard = 0; el != 0.0 && guard < 200000; ++guard) {
        if (swmm_engine_step(h, &el) != 0) break;
        const double t = el * 86400.0;
        r.t.push_back(t);
        r.h1.push_back(ctx.nodes.head[static_cast<std::size_t>(j1)]);
        const auto* rom = eng->rom1d();
        const auto& st = eng->rom1dAdditiveState();
        const auto& S = eng->rom1dStorage();
        const auto& qa = eng->rom1dSoftSource();
        const std::size_t na = S.size();
        if (soft && rom && rom->is_ready() && na > 0 && st.size() >= na && qa.size() == na) {
            const int a1 = rom->full_to_active[static_cast<std::size_t>(j1)];
            double v = 0.0, qs = 0.0;
            for (std::size_t a = 0; a < na; ++a) { v += S[a] * st[a]; qs += qa[a]; }
            src += qs * (t - t_prev);
            r.psi1.push_back(st[static_cast<std::size_t>(a1)]);
            r.S1.push_back(S[static_cast<std::size_t>(a1)]);
            r.S1_min = std::min(r.S1_min, S[static_cast<std::size_t>(a1)]);
            r.S1_max = std::max(r.S1_max, S[static_cast<std::size_t>(a1)]);
            r.vol.push_back(v);
            r.src.push_back(src);
        }
        t_prev = t;
        if (el > 0.0 && t >= 5400.0) break;
    }
    r.ok = !r.t.empty();
    cleanup();
    return r;
}

double headAt(const BurstRun& r, double t) {
    if (r.t.empty()) return 0.0;
    if (t <= r.t.front()) return r.h1.front();
    for (std::size_t k = 1; k < r.t.size(); ++k)
        if (r.t[k] >= t) {
            const double w = (t - r.t[k - 1]) / std::max(r.t[k] - r.t[k - 1], 1.0e-12);
            return (1.0 - w) * r.h1[k - 1] + w * r.h1[k];
        }
    return r.h1.back();
}

}  // namespace

TEST(PlaneVolumeCarry, BurstIntoASmallStorageJunctionCreatesNoWater) {
    const BurstRun r = runBurst(1.0, true, "vol");
    ASSERT_TRUE(r.ok);
    ASSERT_FALSE(r.vol.empty()) << "the soft-rain plane should run";
    // The fixture does what it is for: J1's storage falls to MIN_SURFAREA while
    // the pipes run full and rises again on the recession.
    EXPECT_LT(r.S1_min, 1.0e-3 * r.S1_max + 13.0) << "J1 never surcharged";
    EXPECT_GT(r.S1_max, 10.0 * r.S1_min) << "J1's storage never rose again";
    // With positive sources and an M-matrix step every plane level is >= 0, and
    // the plane's volume sum S·Psi can only lose water (through the outfall):
    // it never exceeds what the sources put in.
    double worst = 0.0;
    for (std::size_t k = 0; k < r.vol.size(); ++k) {
        EXPECT_GE(r.psi1[k], -1.0e-12) << "step " << k;
        worst = std::max(worst, r.vol[k] / std::max(r.src[k], 1.0e-9));
        EXPECT_LE(r.vol[k], r.src[k] * (1.0 + 1.0e-9) + 1.0e-9) << "step " << k << ": plane volume above its sources";
    }
    std::printf("[O-14] plane volume / cumulative source, max over the run: %.4f\n", worst);
}

TEST(PlaneVolumeCarry, PlaneTracksTheMembersHeadOnTheRecession) {
    // Two deterministic members at rain x1.2 and x0.8 (c = +-1 at CV 0.20) give
    // the head sensitivity per unit coefficient at J1. Once J1 has drained (its
    // storage back above half its largest value, after the members' peak), the
    // plane's level must follow that sensitivity, not hold the surcharge head
    // it had while J1 was full (the shape of Bellinge's shallow laterals after
    // the 06:25 burst). Measured 2026-11-18: the plane over the members on the
    // drained recession, 0.78 (volume carried) against 2.31 (head carried).
    const BurstRun rom = runBurst(1.0, true, "rom");
    const BurstRun wet = runBurst(1.2, false, "wet");
    const BurstRun dry = runBurst(0.8, false, "dry");
    ASSERT_TRUE(rom.ok && wet.ok && dry.ok);
    ASSERT_EQ(rom.psi1.size(), rom.t.size());
    auto fdAt = [&](double t) { return 0.5 * (headAt(wet, t) - headAt(dry, t)); };
    double fd_max = 0.0;
    std::size_t k_peak = 0;
    for (std::size_t k = 0; k < rom.t.size(); ++k)
        if (fdAt(rom.t[k]) > fd_max) { fd_max = fdAt(rom.t[k]); k_peak = k; }
    ASSERT_GT(fd_max, 1.0) << "the burst should surcharge J1 differently in the two members";
    double sum_psi = 0.0, sum_fd = 0.0;
    int n = 0;
    for (std::size_t k = k_peak; k < rom.t.size(); ++k) {
        const double fd = fdAt(rom.t[k]);
        if (rom.S1[k] < 0.5 * rom.S1_max || fd < 0.05) continue;
        sum_psi += rom.psi1[k];
        sum_fd += fd;
        ++n;
    }
    ASSERT_GT(n, 20) << "no drained recession samples";
    const double ratio = sum_psi / sum_fd;
    std::printf("[O-14] J1 drained recession: %d samples, plane / members' sensitivity %.3f\n", n, ratio);
    EXPECT_GT(ratio, 0.5) << "the plane lost the members' band on the recession";
    EXPECT_LT(ratio, 2.0) << "the plane holds a surcharge head the members no longer have";
}
// ---------------------------------------------------------------------------
// O-16b — sign-split planes with one-sided secants
// (docs/uncertainty/O16_PLANE_SINKS.md §4, §6). Rain-only soft forcing, per-member
// path off (the shipped default). Two threshold outlets:
//   J0 -> side orifice (crest above the main run's peak) -> J1 (junction, dry in
//         the main run) -> 0.5 ft pipe -> O1;  J0 also drains by pipes via J2 to O3;
//   W  (wet well, 300 ft^2) -> PUMP4 with a FLAT curve (dQ/dH = 0), off in the
//         main run (startup depth above its peak, shutoff below it) -> O2; W also
//         drains through small pipes via J3 to O4.
// The Monte Carlo: 21 deterministic runs at rain x (1 + 0.2 c_k),
// c_k = probit((k + 1/2)/21).
// ---------------------------------------------------------------------------
namespace {

constexpr double kO16Crest = 0.85, kO16YOn = 1.0, kO16YOff = 0.7, kO16Cap = 3.0;

std::string o16Fixture(double rain_scale, bool soft) {
    std::ostringstream f;
    f << std::setprecision(10);
    f << "[TITLE]\nO-16b threshold outlets\n\n[OPTIONS]\n";
    f << "FLOW_UNITS CFS\nINFILTRATION HORTON\nFLOW_ROUTING DYNWAVE\n";
    f << "START_DATE 01/01/2025\nSTART_TIME 00:00:00\nREPORT_START_DATE 01/01/2025\nREPORT_START_TIME 00:00:00\n";
    f << "END_DATE 01/01/2025\nEND_TIME 01:30:00\n";
    f << "REPORT_STEP 00:05:00\nWET_STEP 00:01:00\nDRY_STEP 00:05:00\nROUTING_STEP 00:00:05\n";
    f << "MINIMUM_STEP 0.5\nTHREADS 1\n\n[EVAPORATION]\nCONSTANT 0.0\n\n";
    f << "[RAINGAGES]\nRG1 INTENSITY 0:05 1.0 TIMESERIES TS0\n\n[TIMESERIES]\n";
    for (int m = 0; m <= 90; m += 5) {
        const double tri = (m <= 60) ? (1.0 - std::fabs(m - 30.0) / 30.0) : 0.0;
        f << "TS0 01/01/2025 " << std::setfill('0') << std::setw(2) << m / 60 << ":" << std::setfill('0') << std::setw(2)
          << m % 60 << " " << 1.0 * tri * rain_scale << "\n";
    }
    f << std::setfill(' ') << "\n";
    f << "[SUBCATCHMENTS]\nS1 RG1 J0 2.0 300 100 1.0 0\nS2 RG1 W 0.5 300 100 1.0 0\n\n";
    f << "[SUBAREAS]\nS1 0.015 0.20 0.00 0.00 100 OUTLET\nS2 0.015 0.20 0.00 0.00 100 OUTLET\n\n";
    f << "[INFILTRATION]\nS1 0.0 0.0 0.0 0.0 0.0\nS2 0.0 0.0 0.0 0.0 0.0\n\n";
    f << "[JUNCTIONS]\nJ0 100 10 0 0 0\nJ1 99 5 0 0 0\nJ2 99.5 5 0 0 0\nJ3 99.5 5 0 0 0\n\n";
    f << "[OUTFALLS]\nO1 98.5 FREE NO\nO2 99 FREE NO\nO3 99 FREE NO\nO4 99 FREE NO\n\n";
    f << "[STORAGE]\nW 100 10 0 FUNCTIONAL 0 0 300\n\n";
    f << "[CONDUITS]\nC0 J0 J2 200 0.015 0 0 0 0\nC3 J2 O3 200 0.015 0 0 0 0\nC1 J1 O1 200 0.015 0 0 0 0\n";
    f << "C2 W J3 200 0.015 0 0 0 0\nC4 J3 O4 200 0.015 0 0 0 0\n\n";
    f << "[PUMPS]\nP1 W O2 PC1 OFF " << kO16YOn << " " << kO16YOff << "\n\n";
    f << "[ORIFICES]\nOR1 J0 J1 SIDE " << kO16Crest << " 0.65 NO 0\n\n";
    f << "[XSECTIONS]\nC0 CIRCULAR 1.0 0 0 0 1\nC3 CIRCULAR 1.0 0 0 0 1\nC1 CIRCULAR 0.5 0 0 0 1\nC2 CIRCULAR 0.5 0 0 0 1\n";
    f << "C4 CIRCULAR 0.5 0 0 0 1\nOR1 RECT_CLOSED 0.5 1.0 0 0\n\n";
    f << "[CURVES]\nPC1 PUMP4 0 " << kO16Cap << "\nPC1 10 " << kO16Cap << "\n\n";
    f << "[REPORT]\nINPUT NO\nCONTINUITY YES\nNODES ALL\nLINKS ALL\n\n";
    if (soft) f << "[SOFT_RAINGAGES]\nRG1 NORMAL CV 0.20\n";
    return f.str();
}

struct O16Run {
    std::vector<double> t, hJ0, hJ1, hW, qP;   // main run per step
    std::vector<double> devJ1, devW;           // ROM: steps x M additive deviation (ft), unclamped
    std::vector<double> c;                     // member coefficients
    double invJ1 = 0.0, invW = 0.0;
    int planes_max = 0;
    bool pump_upper_line = false;              // the + half's pump edge carried a line (d+ > 0)
    long pump_lower_line_while_off = 0;        // steps the - half's pump edge had d- > 0 while the main pump was off
    bool ok = false;
};

O16Run runO16(double rain_scale, bool soft, const std::string& tag) {
    O16Run r;
    const std::string pfx = "rom_o16b_" + std::to_string(getpid()) + "_" + tag;
    const std::string ip = pfx + ".inp", rp = pfx + ".rpt";
    { std::ofstream f(ip); f << o16Fixture(rain_scale, soft); }
    SWMM_Engine h = swmm_engine_create();
    auto cleanup = [&]() {
        swmm_engine_end(h); swmm_engine_close(h); swmm_engine_destroy(h);
        for (const char* ext : {".inp", ".rpt", ".uncertainty.csv", ".rom_threshold.csv", ".rom_diag.csv", ".out"})
            std::remove((pfx + ext).c_str());
    };
    if (const int rc = swmm_engine_open(h, ip.c_str(), rp.c_str(), nullptr, nullptr); rc != 0) {
        std::printf("[O-16b] %s: open failed (%d)\n", tag.c_str(), rc);
        cleanup();
        return r;
    }
    auto* eng = static_cast<openswmm::SWMMEngine*>(h);
    if (swmm_engine_initialize(h) != 0 || swmm_engine_start(h, 0) != 0) {
        std::printf("[O-16b] %s: initialize/start failed\n", tag.c_str());
        cleanup();
        return r;
    }
    const auto& ctx = eng->context();
    const int j0 = ctx.node_names.find("J0"), j1 = ctx.node_names.find("J1"), w = ctx.node_names.find("W");
    const int p1 = ctx.link_names.find("P1");
    r.invJ1 = ctx.nodes.invert_elev[static_cast<std::size_t>(j1)];
    r.invW = ctx.nodes.invert_elev[static_cast<std::size_t>(w)];
    double el = 1.0;
    long not_ready = 0;
    for (int guard = 0; el != 0.0 && guard < 200000; ++guard) {
        if (const int rc = swmm_engine_step(h, &el); rc != 0) { std::printf("[O-16b] %s: step failed (%d)\n", tag.c_str(), rc); break; }
        const double t = el * 86400.0;
        const auto* rom = eng->rom1d();
        const auto& st = eng->rom1dAdditiveState();
        const int np = eng->rom1dAdditivePlanes();
        if (soft && !(rom && rom->is_ready() && np > 0)) ++not_ready;
        const std::size_t na = rom ? rom->full_to_active.size() : 0;
        if (soft) {
            if (!(rom && rom->is_ready() && np > 0)) continue;
            const std::size_t nact = st.size() / static_cast<std::size_t>(np);
            const int a1 = rom->full_to_active[static_cast<std::size_t>(j1)], aw = rom->full_to_active[static_cast<std::size_t>(w)];
            if (na == 0 || a1 < 0 || aw < 0) continue;
            if (r.c.empty()) r.c = rom->softCoefficients(openswmm::uncertainty::DistType::NORMAL);
            r.planes_max = std::max(r.planes_max, np);
            for (double ci : r.c) {
                // np = 1: kappa = c; np = 2: kappa+ = max(c, 0) on plane 0, kappa- = min(c, 0) on plane 1.
                auto dev = [&](int a) {
                    if (np == 1) return ci * st[static_cast<std::size_t>(a)];
                    return std::max(ci, 0.0) * st[static_cast<std::size_t>(a)] + std::min(ci, 0.0) * st[nact + static_cast<std::size_t>(a)];
                };
                r.devJ1.push_back(dev(a1));
                r.devW.push_back(dev(aw));
            }
            const auto& ep = eng->rom1dAdditiveEdges();
            const auto& em = eng->rom1dAdditiveEdgesNeg();
            for (std::size_t k = 0; k < ep.size(); ++k) {
                if (ep[k].link != p1) continue;
                if (ep[k].d > 0.0) r.pump_upper_line = true;
                if (k < em.size() && em[k].d > 0.0 && std::fabs(ctx.links.flow[static_cast<std::size_t>(p1)]) < 1.0e-6)
                    ++r.pump_lower_line_while_off;
            }
        }
        r.t.push_back(t);
        r.hJ0.push_back(ctx.nodes.head[static_cast<std::size_t>(j0)]);
        r.hJ1.push_back(ctx.nodes.head[static_cast<std::size_t>(j1)]);
        r.hW.push_back(ctx.nodes.head[static_cast<std::size_t>(w)]);
        r.qP.push_back(ctx.links.flow[static_cast<std::size_t>(p1)]);
        if (el > 0.0 && t >= 5400.0) break;
    }
    r.ok = !r.t.empty();
    if (!r.ok) std::printf("[O-16b] %s: no samples (steps without a ready ROM/plane: %ld)\n", tag.c_str(), not_ready);
    cleanup();
    return r;
}

double interpAt(const std::vector<double>& ts, const std::vector<double>& v, double t) {
    if (ts.empty()) return 0.0;
    if (t <= ts.front()) return v.front();
    for (std::size_t k = 1; k < ts.size(); ++k)
        if (ts[k] >= t) {
            const double a = (t - ts[k - 1]) / std::max(ts[k] - ts[k - 1], 1.0e-12);
            return (1.0 - a) * v[k - 1] + a * v[k];
        }
    return v.back();
}

// Hazen (midpoint) quantile of a sample.
double hazen(std::vector<double> x, double p) {
    std::sort(x.begin(), x.end());
    const double r = p * static_cast<double>(x.size()) - 0.5;
    if (r <= 0.0) return x.front();
    if (r >= static_cast<double>(x.size() - 1)) return x.back();
    const auto k = static_cast<std::size_t>(r);
    return x[k] + (r - static_cast<double>(k)) * (x[k + 1] - x[k]);
}

}  // namespace

TEST(AdditiveSignSplit, ThresholdOutletsDoNotDrainDryMembers) {
    const O16Run rom = runO16(1.0, true, "rom");
    ASSERT_TRUE(rom.ok);
    ASSERT_FALSE(rom.c.empty()) << "the soft-rain plane should run";
    const std::size_t M = rom.c.size();
    ASSERT_EQ(rom.devJ1.size(), rom.t.size() * M);
    std::vector<O16Run> mc;
    const int K = 21;
    for (int k = 0; k < K; ++k) {
        const double ck = openswmm::uncertainty::probit((k + 0.5) / K);
        mc.push_back(runO16(1.0 + 0.2 * ck, false, "mc" + std::to_string(k)));
        ASSERT_TRUE(mc.back().ok);
    }
    double peakJ0 = 0.0, peakW = 0.0;
    for (std::size_t s = 0; s < rom.t.size(); ++s) {
        peakJ0 = std::max(peakJ0, rom.hJ0[s] - 100.0);
        peakW = std::max(peakW, rom.hW[s] - rom.invW);
    }
    std::printf("[O-16b] main run: J0 peak depth %.3f ft (orifice crest %.2f), W peak depth %.3f ft (pump on %.2f / off %.2f), planes %d\n",
                peakJ0, kO16Crest, peakW, kO16YOn, kO16YOff, rom.planes_max);
    ASSERT_LT(peakJ0, kO16Crest) << "the orifice must be closed in the main run";
    ASSERT_LT(peakW, kO16YOn) << "the pump must stay off in the main run";
    ASSERT_GT(peakW, kO16YOff) << "the main run must rise above the pump's shutoff depth";

    // (a) J1 (dry behind the closed orifice): no member below the main run is
    // drained below the invert; the closed orifice has no lower line (d- = 0).
    double low = 0.0, high = 0.0;
    long below = 0;
    for (std::size_t s = 0; s < rom.t.size(); ++s)
        for (std::size_t i = 0; i < M; ++i) {
            const double v = rom.devJ1[s * M + i];
            high = std::max(high, v);
            if (rom.c[i] >= 0.0) continue;
            const double lev = rom.hJ1[s] + v - rom.invJ1;
            low = std::min(low, lev);
            if (lev < -1.0e-9) ++below;
        }
    double mc_hi_J1 = 0.0;
    for (const auto& m : mc)
        for (std::size_t s = 0; s < m.t.size(); ++s) mc_hi_J1 = std::max(mc_hi_J1, m.hJ1[s] - m.invJ1);
    std::printf("[O-16b] J1: lowest member depth %.4f ft (member-steps below invert %ld); highest member deviation %.3f ft; MC highest depth %.3f ft\n",
                low, below, high, mc_hi_J1);
    EXPECT_EQ(below, 0) << "members below the main run drained through a closed orifice";

    // (b) W while the main pump is below capacity (here: off): the band against
    // the Monte Carlo's, over the samples where the Monte Carlo has a band.
    std::vector<double> ratios, up_ratios;
    double mc_w_max = 0.0, rom_w_max = 0.0;
    for (std::size_t s = 0; s < rom.t.size(); ++s) {
        if (rom.qP[s] >= kO16Cap * (1.0 - 1.0e-9)) continue;
        std::vector<double> lev(M), ml(static_cast<std::size_t>(K));
        for (std::size_t i = 0; i < M; ++i) lev[i] = std::max(rom.hW[s] + rom.devW[s * M + i], rom.invW);
        for (int k = 0; k < K; ++k) ml[static_cast<std::size_t>(k)] = interpAt(mc[static_cast<std::size_t>(k)].t, mc[static_cast<std::size_t>(k)].hW, rom.t[s]);
        const double wr = hazen(lev, 0.95) - hazen(lev, 0.05);
        const double wm = hazen(ml, 0.95) - hazen(ml, 0.05);
        mc_w_max = std::max(mc_w_max, wm);
        rom_w_max = std::max(rom_w_max, wr);
        if (wm > 0.05) ratios.push_back(wr / wm);
        // The upper half alone (q95 - median): where the pump acts.
        const double ur = hazen(lev, 0.95) - hazen(lev, 0.5), um = hazen(ml, 0.95) - hazen(ml, 0.5);
        if (um > 0.05) up_ratios.push_back(ur / um);
    }
    ASSERT_GT(ratios.size(), 20u) << "the Monte Carlo should carry a band at W";
    std::vector<double> rs = ratios;
    std::sort(rs.begin(), rs.end());
    const double med = rs[rs.size() / 2];
    std::printf("[O-16b] W: %zu samples, width ratio ROM/MC min %.3f median %.3f max %.3f; widest band ROM %.3f ft, MC %.3f ft\n",
                rs.size(), rs.front(), med, rs.back(), rom_w_max, mc_w_max);
    EXPECT_GE(med, 0.5);
    EXPECT_LE(med, 2.0) << "the wetter members did not pass their extra water through the pump";
    ASSERT_GT(up_ratios.size(), 20u);
    std::sort(up_ratios.begin(), up_ratios.end());
    const double med_up = up_ratios[up_ratios.size() / 2];
    std::printf("[O-16b] W upper half (q95 - q50): %zu samples, ratio ROM/MC median %.3f\n", up_ratios.size(), med_up);
    EXPECT_GE(med_up, 0.5);
    EXPECT_LE(med_up, 2.0) << "the wetter members did not pass their extra water through the pump";

    // (c) the positive side passes extra water through the pump; the negative
    // side has no pump line while the main pump is off (nothing to stop).
    EXPECT_TRUE(rom.pump_upper_line) << "the pump never entered the upper half's edge list";
    EXPECT_EQ(rom.pump_lower_line_while_off, 0);
}
#endif  // OPENSWMM_HAS_2D
