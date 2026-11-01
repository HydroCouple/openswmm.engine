/**
 * @file test_rom_additive_channel.cpp
 * @brief PR H19 — full-order linearized additive channel (RomAdditiveChannel.hpp).
 *
 * S dPsi/dt = -J Psi + dq, backward Euler, direct solve. Each case checks a
 * property the modal additive forcing could not hold: the steady response
 * carries the accumulated extra flow, the early response stays local, and
 * volume is conserved.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "uncertainty/RomAdditiveChannel.hpp"

using openswmm::uncertainty::AdditiveSystem;
using openswmm::uncertainty::FlowEdge;

namespace {

FlowEdge conduit(int up, int dn, double ju, double jd, double q = 1.0) {
    FlowEdge e;
    e.up = up; e.dn = dn; e.conduit = true; e.q = q; e.ju = ju; e.jd = jd;
    return e;
}

// Chain 0 -> 1 -> ... -> n-1 -> outfall, one conduit per node.
std::vector<FlowEdge> chain(int n, double ju, double jd) {
    std::vector<FlowEdge> e;
    for (int i = 0; i < n; ++i) e.push_back(conduit(i, i + 1 < n ? i + 1 : -1, ju, jd));
    return e;
}

}  // namespace

TEST(AdditiveChannel, SteadyStateCarriesTheAccumulatedExtraFlow) {
    // Kinematic chain (Jd = 0): at steady state conduit i carries the extra
    // inflow of every node above it, so Psi_i = (i + 1) * dq / Ju. This is the
    // transfer the symmetric modal operator dropped (SR-5 J5).
    const int n = 5;
    const double ju = 0.4, dq = 1.0;
    const auto edges = chain(n, ju, 0.0);
    std::vector<double> S(n, 100.0), q(n, dq), psi(n, 0.0);
    AdditiveSystem sys;
    sys.build(n, edges, S.data(), 1.0e9);
    for (int k = 0; k < 5; ++k) sys.step(q.data(), psi.data());
    for (int i = 0; i < n; ++i)
        EXPECT_NEAR(psi[static_cast<std::size_t>(i)], (i + 1) * dq / ju, 1.0e-6) << "node " << i;
}

TEST(AdditiveChannel, EarlyResponseStaysLocal) {
    // Storage-dominated (the SR-5 regime): one second after extra water enters
    // node 0, node 3 has barely moved. Relaxing toward the steady target moved
    // it at once (A.4b, VALIDATION "H18" s10b).
    const int n = 5;
    const auto edges = chain(n, 0.4, 0.0);
    std::vector<double> S(n, 2.0e4), q(n, 0.0), psi(n, 0.0);
    q[0] = 1.0;
    AdditiveSystem sys;
    sys.build(n, edges, S.data(), 1.0);
    sys.step(q.data(), psi.data());
    EXPECT_NEAR(psi[0], 1.0 / (2.0e4 + 0.4), 1.0e-12);   // essentially dq*dt/S
    EXPECT_LT(psi[3], 1.0e-12 * psi[0]);
}

TEST(AdditiveChannel, VolumeIsConserved) {
    // Each step: change in stored volume + volume leaving at the outfall =
    // extra inflow * dt (backward Euler, converged).
    const int n = 4;
    const double ju = 0.7, jd = -0.2, dt = 30.0;
    const auto edges = chain(n, ju, jd);
    std::vector<double> S = {50.0, 80.0, 120.0, 60.0}, q = {0.3, 0.1, 0.0, 0.2}, psi(n, 0.0);
    AdditiveSystem sys;
    sys.build(n, edges, S.data(), dt);
    for (int k = 0; k < 20; ++k) {
        std::vector<double> old = psi;
        sys.step(q.data(), psi.data(), 500, 1.0e-14);
        double dV = 0.0, in = 0.0;
        for (int i = 0; i < n; ++i) {
            dV += S[static_cast<std::size_t>(i)] * (psi[static_cast<std::size_t>(i)] - old[static_cast<std::size_t>(i)]);
            in += q[static_cast<std::size_t>(i)] * dt;
        }
        const double out = ju * psi[n - 1] * dt;   // last conduit to the outfall (Psi_outfall = 0)
        EXPECT_NEAR(dV + out, in, 1.0e-9 * in) << "step " << k;
    }
}

TEST(AdditiveChannel, CoupledSystemMatchesDenseSolve) {
    // Subcritical chain with downstream head coupling plus a structure into
    // the last node: Gauss-Seidel equals Gaussian elimination of the same
    // backward-Euler system.
    const int n = 4;
    std::vector<FlowEdge> edges = {conduit(0, 1, 0.9, -0.5), conduit(1, 2, 0.8, -0.4),
                                   conduit(2, -1, 0.6, -0.1)};
    FlowEdge st; st.up = 3; st.dn = 2; st.conduit = false; st.d = 0.3; st.q = 0.5;
    edges.push_back(st);
    std::vector<double> S = {10.0, 20.0, 15.0, 5.0}, q = {0.1, 0.0, 0.2, 0.05}, psi(n, 0.0);
    const double dt = 5.0;
    AdditiveSystem sys;
    sys.build(n, edges, S.data(), dt);
    sys.step(q.data(), psi.data(), 1000, 1.0e-15);
    // Dense matrix: row i = S_i/dt x_i + sum_out dQ - sum_in dQ.
    double A[4][4] = {}, b[4];
    for (int i = 0; i < n; ++i) { A[i][i] = S[static_cast<std::size_t>(i)] / dt; b[i] = q[static_cast<std::size_t>(i)]; }
    for (const auto& e : edges) {
        auto add = [&](int row, int c, double v) { if (row >= 0 && c >= 0) A[row][c] += v; };
        if (e.conduit) {
            add(e.up, e.up, e.ju); add(e.up, e.dn, e.jd);
            add(e.dn, e.up, -e.ju); add(e.dn, e.dn, -e.jd);
        } else {
            add(e.up, e.up, e.d); add(e.dn, e.up, -e.d);
        }
    }
    for (int k = 0; k < n; ++k)
        for (int r = k + 1; r < n; ++r) {
            const double f = A[r][k] / A[k][k];
            for (int c = k; c < n; ++c) A[r][c] -= f * A[k][c];
            b[r] -= f * b[k];
        }
    double x[4];
    for (int r = n - 1; r >= 0; --r) {
        double s = b[r];
        for (int c = r + 1; c < n; ++c) s -= A[r][c] * x[c];
        x[r] = s / A[r][r];
    }
    for (int i = 0; i < n; ++i) EXPECT_NEAR(psi[static_cast<std::size_t>(i)], x[i], 1.0e-12) << "node " << i;
}

TEST(AdditiveChannel, PinnedNodeStaysAtZeroAndPassesNothingOn) {
    const int n = 3;
    const auto edges = chain(n, 0.5, 0.0);
    std::vector<double> S(n, 10.0), q = {1.0, 0.0, 0.0}, psi(n, 0.0);
    std::vector<char> pinned = {0, 1, 0};
    AdditiveSystem sys;
    sys.build(n, edges, S.data(), 1.0e6, &pinned);
    for (int k = 0; k < 3; ++k) sys.step(q.data(), psi.data());
    EXPECT_EQ(psi[1], 0.0);
    EXPECT_NEAR(psi[2], 0.0, 1.0e-14);
    EXPECT_GT(psi[0], 0.0);
}

TEST(AdditiveChannel, ZeroSourceStaysExactlyZero) {
    const int n = 5;
    const auto edges = chain(n, 0.4, -0.1);
    std::vector<double> S(n, 30.0), q(n, 0.0), psi(n, 0.0);
    AdditiveSystem sys;
    sys.build(n, edges, S.data(), 10.0);
    for (int k = 0; k < 10; ++k) sys.step(q.data(), psi.data());
    for (double v : psi) EXPECT_EQ(v, 0.0);
}

namespace {

// Dense reference: the same backward-Euler rows, pinned rows replaced by
// x = 0, solved by Gaussian elimination with partial pivoting.
std::vector<double> denseSolve(int n, const std::vector<FlowEdge>& edges, const std::vector<double>& S,
                               double dt, const std::vector<double>& psi_old, const std::vector<double>& q,
                               const std::vector<char>& pinned) {
    std::vector<std::vector<double>> A(static_cast<std::size_t>(n), std::vector<double>(static_cast<std::size_t>(n), 0.0));
    std::vector<double> b(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        A[ui][ui] = S[ui] / dt;
        b[ui] = S[ui] / dt * psi_old[ui] + q[ui];
    }
    auto add = [&](int r, int c, double v) {
        if (r >= 0 && c >= 0 && r < n && c < n) A[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)] += v;
    };
    for (const auto& e : edges) {
        if (e.up < 0) continue;
        if (e.conduit) { add(e.up, e.up, e.ju); add(e.up, e.dn, e.jd); add(e.dn, e.up, -e.ju); add(e.dn, e.dn, -e.jd); }
        else { add(e.up, e.up, e.d); add(e.dn, e.up, -e.d); }
    }
    for (int i = 0; i < n; ++i)
        if (!pinned.empty() && pinned[static_cast<std::size_t>(i)]) {
            auto& row = A[static_cast<std::size_t>(i)];
            std::fill(row.begin(), row.end(), 0.0);
            row[static_cast<std::size_t>(i)] = 1.0;
            b[static_cast<std::size_t>(i)] = 0.0;
        }
    for (int k = 0; k < n; ++k) {
        int p = k;
        for (int r = k + 1; r < n; ++r)
            if (std::fabs(A[static_cast<std::size_t>(r)][static_cast<std::size_t>(k)]) >
                std::fabs(A[static_cast<std::size_t>(p)][static_cast<std::size_t>(k)])) p = r;
        std::swap(A[static_cast<std::size_t>(k)], A[static_cast<std::size_t>(p)]);
        std::swap(b[static_cast<std::size_t>(k)], b[static_cast<std::size_t>(p)]);
        for (int r = k + 1; r < n; ++r) {
            const double f = A[static_cast<std::size_t>(r)][static_cast<std::size_t>(k)] / A[static_cast<std::size_t>(k)][static_cast<std::size_t>(k)];
            if (f == 0.0) continue;
            for (int c = k; c < n; ++c)
                A[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)] -= f * A[static_cast<std::size_t>(k)][static_cast<std::size_t>(c)];
            b[static_cast<std::size_t>(r)] -= f * b[static_cast<std::size_t>(k)];
        }
    }
    std::vector<double> x(static_cast<std::size_t>(n));
    for (int r = n - 1; r >= 0; --r) {
        double s = b[static_cast<std::size_t>(r)];
        for (int c = r + 1; c < n; ++c) s -= A[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)] * x[static_cast<std::size_t>(c)];
        x[static_cast<std::size_t>(r)] = s / A[static_cast<std::size_t>(r)][static_cast<std::size_t>(r)];
    }
    return x;
}

// A branching network with loops (a ladder between two trunks), one-way and
// two-way conduits, structures, an outfall and a pinned node. Deterministic.
struct LoopedNet {
    int n = 0;
    std::vector<FlowEdge> edges;
    std::vector<double> S, q, psi_old;
    std::vector<char> pinned;
};
LoopedNet loopedNet() {
    LoopedNet m;
    m.n = 40;
    unsigned s = 12345u;
    auto rnd = [&]() { s = s * 1103515245u + 12345u; return static_cast<double>((s >> 8) & 0xFFFF) / 65535.0; };
    // Two trunks 0..19 and 20..39 draining toward 19 and 39; 39 -> outfall.
    for (int i = 0; i < 19; ++i) m.edges.push_back(conduit(i, i + 1, 0.2 + rnd(), rnd() < 0.3 ? 0.0 : -0.5 * rnd()));
    for (int i = 20; i < 39; ++i) m.edges.push_back(conduit(i, i + 1, 0.2 + rnd(), rnd() < 0.3 ? 0.0 : -0.5 * rnd()));
    m.edges.push_back(conduit(39, -1, 0.7, -0.2));
    m.edges.push_back(conduit(19, 39, 0.9, -0.3));
    // Rungs between the trunks: each closes a loop.
    for (int i = 2; i < 18; i += 3) m.edges.push_back(conduit(i, i + 20, 0.1 + rnd(), -0.1 * rnd()));
    // Structures, one of them still (d from a secant) and one into a rung node.
    FlowEdge w; w.conduit = false; w.up = 7; w.dn = 30; w.d = 0.4; m.edges.push_back(w);
    FlowEdge o; o.conduit = false; o.up = 33; o.dn = 12; o.d = 0.05; m.edges.push_back(o);
    for (int i = 0; i < m.n; ++i) {
        m.S.push_back(0.1 + 50.0 * rnd() * rnd());   // storage spans almost three decades
        m.q.push_back(rnd() < 0.5 ? 0.0 : rnd());
        m.psi_old.push_back(rnd() - 0.3);
    }
    m.pinned.assign(static_cast<std::size_t>(m.n), 0);
    m.pinned[25] = 1;
    return m;
}

}  // namespace

TEST(AdditiveChannelDirect, LoopedNetworkMatchesDenseElimination) {
    // The direct solve must be exact on a network that is not a tree: loops
    // create fill, which a leaf-to-outlet sweep alone would get wrong.
    const auto m = loopedNet();
    const double dt = 5.0;
    AdditiveSystem sys;
    sys.build(m.n, m.edges, m.S.data(), dt, &m.pinned);
    std::vector<double> psi = m.psi_old;
    sys.step(m.q.data(), psi.data());
    const auto x = denseSolve(m.n, m.edges, m.S, dt, m.psi_old, m.q, m.pinned);
    double scale = 0.0;
    for (double v : x) scale = std::max(scale, std::fabs(v));
    for (int i = 0; i < m.n; ++i)
        EXPECT_NEAR(psi[static_cast<std::size_t>(i)], x[static_cast<std::size_t>(i)], 1.0e-12 * scale) << "node " << i;
    EXPECT_EQ(psi[25], 0.0);
}

TEST(AdditiveChannelDirect, OneFactorServesEveryPlane) {
    // Planes share the matrix: stepping two planes after one build gives the
    // same answer as building separately for each.
    const auto m = loopedNet();
    AdditiveSystem shared, a, b;
    shared.build(m.n, m.edges, m.S.data(), 2.0, &m.pinned);
    a.build(m.n, m.edges, m.S.data(), 2.0, &m.pinned);
    b.build(m.n, m.edges, m.S.data(), 2.0, &m.pinned);
    std::vector<double> q2(m.q.rbegin(), m.q.rend());
    std::vector<double> p1 = m.psi_old, p2(m.psi_old.rbegin(), m.psi_old.rend());
    std::vector<double> r1 = p1, r2 = p2;
    shared.step(m.q.data(), p1.data());
    shared.step(q2.data(), p2.data());
    a.step(m.q.data(), r1.data());
    b.step(q2.data(), r2.data());
    for (int i = 0; i < m.n; ++i) {
        EXPECT_EQ(p1[static_cast<std::size_t>(i)], r1[static_cast<std::size_t>(i)]);
        EXPECT_EQ(p2[static_cast<std::size_t>(i)], r2[static_cast<std::size_t>(i)]);
    }
}

TEST(AdditiveChannelDirect, LinkThatAppearsLaterIsAddedToThePattern) {
    // A conduit whose flow starts mid-run enters the edge list later. The
    // elimination order must be recomputed for it, once, and stay exact.
    auto m = loopedNet();
    const FlowEdge late = conduit(5, 35, 0.6, -0.2);   // closes a new loop
    AdditiveSystem sys;
    sys.build(m.n, m.edges, m.S.data(), 3.0, &m.pinned);
    EXPECT_EQ(sys.symbolic_builds, 1);
    m.edges.push_back(late);
    sys.build(m.n, m.edges, m.S.data(), 3.0, &m.pinned);
    EXPECT_EQ(sys.symbolic_builds, 2);
    sys.build(m.n, m.edges, m.S.data(), 3.0, &m.pinned);   // nothing new: order kept
    EXPECT_EQ(sys.symbolic_builds, 2);
    std::vector<double> psi = m.psi_old;
    sys.step(m.q.data(), psi.data());
    const auto x = denseSolve(m.n, m.edges, m.S, 3.0, m.psi_old, m.q, m.pinned);
    double scale = 0.0;
    for (double v : x) scale = std::max(scale, std::fabs(v));
    for (int i = 0; i < m.n; ++i)
        EXPECT_NEAR(psi[static_cast<std::size_t>(i)], x[static_cast<std::size_t>(i)], 1.0e-12 * scale) << "node " << i;
    // The link stopping again keeps it in the pattern and stays exact.
    m.edges.pop_back();
    sys.build(m.n, m.edges, m.S.data(), 3.0, &m.pinned);
    EXPECT_EQ(sys.symbolic_builds, 2);
    psi = m.psi_old;
    sys.step(m.q.data(), psi.data());
    const auto x2 = denseSolve(m.n, m.edges, m.S, 3.0, m.psi_old, m.q, m.pinned);
    for (int i = 0; i < m.n; ++i)
        EXPECT_NEAR(psi[static_cast<std::size_t>(i)], x2[static_cast<std::size_t>(i)], 1.0e-12 * scale) << "node " << i;
}
