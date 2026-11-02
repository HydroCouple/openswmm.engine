/**
 * @file test_ph_operator.cpp
 * @brief PORT_HAMILTONIAN phase 1 (card S-3): PHOperator two-contract core.
 *
 * Contract 1 (l2): two-way + diagonal parts reduce by Q-orthonormal Galerkin
 *   with R_r PSD, J_r skew.  Contract 2 (l1): one-way edges are M-matrix
 *   terms, non-expansive in ||Q psi||_1 and order preserving, and are never
 *   projected.
 */
#include <gtest/gtest.h>

#include "uncertainty/PHOperator.hpp"
#include "uncertainty/RomDirectionalOperator.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>
#include <vector>

using namespace openswmm::uncertainty;

namespace {

struct Graph { PHOperator op; };

// Random connected two-way graph: chain plus extras. jmax scales the skew strength.
PHOperator randomTwoWay(std::mt19937& rng, int n, double jmax, bool rdiag) {
    std::uniform_real_distribution<double> uw(0.1, 10.0), uq(0.01, 100.0), u01(0.0, 1.0);
    PHOperator op;
    op.n = n;
    op.Q.resize(static_cast<std::size_t>(n));
    op.Rdiag.assign(static_cast<std::size_t>(n), 0.0);
    for (int i = 0; i < n; ++i) {
        op.Q[static_cast<std::size_t>(i)] = uq(rng);
        if (rdiag && u01(rng) < 0.3) op.Rdiag[static_cast<std::size_t>(i)] = uw(rng);
    }
    auto add = [&](int a, int b) {
        PHOperator::TwoWay e;
        e.a = a; e.b = b; e.w = uw(rng);
        e.j = (2.0 * u01(rng) - 1.0) * jmax * e.w;
        op.two_way.push_back(e);
    };
    for (int i = 0; i + 1 < n; ++i) add(i, i + 1);
    const int extra = 2 + static_cast<int>(rng() % 3);
    for (int i = 0; i < extra; ++i) {
        const int a = static_cast<int>(rng() % static_cast<unsigned>(n));
        const int b = static_cast<int>(rng() % static_cast<unsigned>(n));
        if (a != b) add(a, b);
    }
    return op;
}

// P: k×n row-major, Q-orthonormal (modified Gram-Schmidt in the Q inner product).
std::vector<double> qOrthonormal(std::mt19937& rng, const std::vector<double>& Q, int k) {
    const int n = static_cast<int>(Q.size());
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    std::vector<double> P(static_cast<std::size_t>(k) * static_cast<std::size_t>(n));
    auto at = [&](int j, int i) -> double& {
        return P[static_cast<std::size_t>(j) * static_cast<std::size_t>(n) + static_cast<std::size_t>(i)];
    };
    for (int j = 0; j < k; ++j) {
        for (int i = 0; i < n; ++i) at(j, i) = u(rng);
        for (int pass = 0; pass < 2; ++pass)
            for (int j2 = 0; j2 < j; ++j2) {
                double dot = 0.0;
                for (int i = 0; i < n; ++i) dot += at(j2, i) * Q[static_cast<std::size_t>(i)] * at(j, i);
                for (int i = 0; i < n; ++i) at(j, i) -= dot * at(j2, i);
            }
        double nn = 0.0;
        for (int i = 0; i < n; ++i) nn += at(j, i) * Q[static_cast<std::size_t>(i)] * at(j, i);
        nn = std::sqrt(nn);
        for (int i = 0; i < n; ++i) at(j, i) /= nn;
    }
    return P;
}

// Independent PᵀLP from a dense L.
std::vector<double> denseGalerkin(const std::vector<double>& L, const std::vector<double>& P, int n, int k) {
    const auto un = static_cast<std::size_t>(n), uk = static_cast<std::size_t>(k);
    std::vector<double> M(uk * uk, 0.0);
    for (std::size_t p = 0; p < uk; ++p)
        for (std::size_t q = 0; q < uk; ++q) {
            double s = 0.0;
            for (std::size_t i = 0; i < un; ++i)
                for (std::size_t j = 0; j < un; ++j) s += P[p * un + i] * L[i * un + j] * P[q * un + j];
            M[p * uk + q] = s;
        }
    return M;
}

double maxAbs(const std::vector<double>& v) {
    double m = 0.0;
    for (double x : v) m = std::max(m, std::fabs(x));
    return m;
}

double l1Q(const std::vector<double>& Q, const std::vector<double>& a, const std::vector<double>& b) {
    double s = 0.0;
    for (std::size_t i = 0; i < Q.size(); ++i) s += Q[i] * std::fabs(a[i] - b[i]);
    return s;
}

} // namespace

// (1) J_r skew, R_r PSD, and reduce() equals an independent dense Galerkin.
TEST(PHOperator, ReducedTwoWayIsSkewPlusPsd) {
    std::mt19937 rng(7);
    double worst_eig = 1.0, worst_skew = 0.0, worst_vs_dense = 0.0;
    for (int t = 0; t < 50; ++t) {
        SCOPED_TRACE("case " + std::to_string(t));
        const int n = 6 + static_cast<int>(rng() % 25);
        const int k = std::min(8, n - 1);
        PHOperator op = randomTwoWay(rng, n, 3.0, true);   // |j| up to 3w: J is arbitrary
        const auto P = qOrthonormal(rng, op.Q, k);

        std::vector<double> M, L;
        ASSERT_TRUE(op.reduce(P.data(), k, M)) << op.last_error;
        ASSERT_TRUE(op.assembleFull(L));
        const auto Md = denseGalerkin(L, P, n, k);
        const double scale = std::max(1.0, maxAbs(M));
        double dd = 0.0;
        for (std::size_t i = 0; i < M.size(); ++i) dd = std::max(dd, std::fabs(M[i] - Md[i]));
        worst_vs_dense = std::max(worst_vs_dense, dd / scale);
        EXPECT_LE(dd, 1e-12 * scale);

        // J_r from the edge list, independently of reduce().
        const auto uk = static_cast<std::size_t>(k), un = static_cast<std::size_t>(n);
        std::vector<double> Jr(uk * uk, 0.0);
        for (const auto& e : op.two_way)
            for (std::size_t p = 0; p < uk; ++p)
                for (std::size_t q = 0; q < uk; ++q)
                    Jr[p * uk + q] += e.j * (P[p * un + static_cast<std::size_t>(e.a)] * P[q * un + static_cast<std::size_t>(e.b)]
                                           - P[p * un + static_cast<std::size_t>(e.b)] * P[q * un + static_cast<std::size_t>(e.a)]);
        // M = R_r − J_r ⇒ skewPart(M) = −J_r.
        const auto sk = skewPart(M, k);
        double sd = 0.0, anti = 0.0;
        for (std::size_t p = 0; p < uk; ++p)
            for (std::size_t q = 0; q < uk; ++q) {
                sd = std::max(sd, std::fabs(sk[p * uk + q] + Jr[p * uk + q]));
                anti = std::max(anti, std::fabs(Jr[p * uk + q] + Jr[q * uk + p]));
            }
        worst_skew = std::max(worst_skew, sd / scale);
        EXPECT_LE(sd, 1e-13 * scale);
        EXPECT_LE(anti, 1e-14 * scale);

        const double me = minEigSym(M, k);   // = min eig of R_r
        worst_eig = std::min(worst_eig, me / scale);
        EXPECT_GE(me, -1e-12 * scale);
    }
    std::cout << "[PH] worst min eig(R_r)/scale " << worst_eig << ", worst |skew(M)+J_r|/scale "
              << worst_skew << ", worst |reduce - dense Galerkin|/scale " << worst_vs_dense << "\n";
}

// (2) Full-order energy is non-increasing under backward Euler (two-way only).
TEST(PHOperator, BackwardEulerEnergyNonIncreasingTwoWay) {
    std::mt19937 rng(11);
    std::normal_distribution<double> nd(0.0, 1.0);
    double worst = -1e300;
    for (int t = 0; t < 50; ++t) {
        const int n = 5 + static_cast<int>(rng() % 30);
        PHOperator op = randomTwoWay(rng, n, 3.0, true);
        std::vector<double> psi(static_cast<std::size_t>(n));
        for (auto& x : psi) x = nd(rng);
        for (double dt : {0.01, 1.0, 100.0}) {
            auto p = psi;
            auto energy = [&](const std::vector<double>& v) {
                double e = 0.0;
                for (int i = 0; i < n; ++i) e += 0.5 * op.Q[static_cast<std::size_t>(i)] * v[static_cast<std::size_t>(i)] * v[static_cast<std::size_t>(i)];
                return e;
            };
            const double e0 = energy(p);
            for (int s = 0; s < 5; ++s) {
                const double before = energy(p);
                ASSERT_TRUE(op.backwardEulerStep(dt, nullptr, p.data())) << op.last_error;
                const double after = energy(p);
                EXPECT_LE(after, before * (1.0 + 1e-12)) << "case " << t << " dt " << dt;
                worst = std::max(worst, (after - before) / std::max(e0, 1e-300));
            }
        }
    }
    std::cout << "[PH] worst relative energy change per step " << worst << " (<=0 expected)\n";
}

// (3) l1 contraction and order preservation on one-way graphs with an outfall sink.
TEST(PHOperator, OneWayGraphIsL1ContractiveAndOrderPreserving) {
    std::mt19937 rng(23);
    std::uniform_real_distribution<double> ud(0.05, 8.0), u01(0.0, 1.0);
    std::normal_distribution<double> nd(0.0, 1.0);
    double worst_l1 = -1e300, worst_order = -1e300;
    int n_cases = 0;
    for (int t = 0; t < 50; ++t) {
        const int n = 4 + static_cast<int>(rng() % 20);
        PHOperator op = randomTwoWay(rng, n, 0.0, false);    // pure R: j = 0 (l1 needs it)
        for (int e = 0; e < n + 3; ++e) {                   // random one-way edges
            PHOperator::OneWay o;
            o.from = static_cast<int>(rng() % static_cast<unsigned>(n));
            o.to = static_cast<int>(rng() % static_cast<unsigned>(n));
            if (o.from == o.to) continue;
            o.d = ud(rng);
            op.one_way.push_back(o);
        }
        // Outfall sink(s): flow leaves the system from node 0 and a random node.
        op.one_way.push_back({0, -1, ud(rng)});
        op.one_way.push_back({static_cast<int>(rng() % static_cast<unsigned>(n)), -1, ud(rng)});
        ++n_cases;

        std::vector<double> L;
        ASSERT_TRUE(op.assembleFull(L));
        // Column-dominant M-matrix check on L itself.
        for (int c = 0; c < n; ++c) {
            double cs = 0.0;
            for (int r = 0; r < n; ++r) {
                const double v = L[static_cast<std::size_t>(r * n + c)];
                if (r != c) EXPECT_LE(v, 0.0);
                cs += v;
            }
            EXPECT_GE(cs, -1e-12);
        }

        std::vector<double> a(static_cast<std::size_t>(n)), b(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) { a[static_cast<std::size_t>(i)] = nd(rng); b[static_cast<std::size_t>(i)] = nd(rng); }
        for (double dt : {0.05, 1.0, 50.0}) {
            auto pa = a, pb = b;
            double prev = l1Q(op.Q, pa, pb);
            for (int s = 0; s < 4; ++s) {
                ASSERT_TRUE(op.backwardEulerStep(dt, nullptr, pa.data()));
                ASSERT_TRUE(op.backwardEulerStep(dt, nullptr, pb.data()));
                const double cur = l1Q(op.Q, pa, pb);
                worst_l1 = std::max(worst_l1, (cur - prev) / std::max(prev, 1e-300));
                EXPECT_LE(cur, prev * (1.0 + 1e-12) + 1e-12);
                prev = cur;
            }
            // Order preservation: lo <= hi nodewise, with a nonnegative source gap.
            std::vector<double> lo = a, hi = a, r_lo(static_cast<std::size_t>(n)), r_hi(static_cast<std::size_t>(n));
            for (int i = 0; i < n; ++i) {
                hi[static_cast<std::size_t>(i)] += u01(rng);
                r_lo[static_cast<std::size_t>(i)] = nd(rng);
                r_hi[static_cast<std::size_t>(i)] = r_lo[static_cast<std::size_t>(i)] + u01(rng);
            }
            ASSERT_TRUE(op.backwardEulerStep(dt, r_lo.data(), lo.data()));
            ASSERT_TRUE(op.backwardEulerStep(dt, r_hi.data(), hi.data()));
            for (int i = 0; i < n; ++i) {
                const double gap = hi[static_cast<std::size_t>(i)] - lo[static_cast<std::size_t>(i)];
                worst_order = std::max(worst_order, -gap);
                EXPECT_GE(gap, -1e-12);
            }
        }
    }
    std::cout << "[PH] " << n_cases << " one-way graphs: worst relative l1 growth " << worst_l1
              << " (<=0), worst order violation " << worst_order << " (<=0)\n";
}

// (4) Mutation: make one J entry symmetric instead of skew; the reduced operator
//     must then show a negative min eigenvalue in at least one case.
TEST(PHOperator, MutationFlippedJSignBreaksPsd) {
    std::mt19937 rng(31);
    int negative_cases = 0;
    double worst_clean = 1.0, worst_mut = 1.0;
    for (int t = 0; t < 50; ++t) {
        const int n = 6 + static_cast<int>(rng() % 6);
        PHOperator op = randomTwoWay(rng, n, 0.5, false);
        // Q = 1, P = I so the reduced operator is the full one (k = n).
        std::fill(op.Q.begin(), op.Q.end(), 1.0);
        std::vector<double> P(static_cast<std::size_t>(n * n), 0.0);
        for (int i = 0; i < n; ++i) P[static_cast<std::size_t>(i * n + i)] = 1.0;
        std::vector<double> M;
        ASSERT_TRUE(op.reduce(P.data(), n, M));
        worst_clean = std::min(worst_clean, minEigSym(M, n));
        EXPECT_GE(minEigSym(M, n), -1e-12);

        // Mutate edge 0: copy M[a][b] onto M[b][a] (J entry loses its sign flip).
        const auto& e = op.two_way[0];
        auto Mm = M;
        // Strengthen so a sign error is visible: add |j| = w on that pair.
        const auto a = static_cast<std::size_t>(e.a), b = static_cast<std::size_t>(e.b), un = static_cast<std::size_t>(n);
        const double jbig = 2.0 * e.w;
        Mm[a * un + b] -= jbig;       // L_ab − j
        Mm[b * un + a] -= jbig;       // L_ba − j  (correct skew would be +j here)
        const double me = minEigSym(Mm, n);
        worst_mut = std::min(worst_mut, me);
        if (me < -1e-6) ++negative_cases;
    }
    std::cout << "[PH] clean min eig " << worst_clean << ", mutated min eig " << worst_mut
              << ", cases with negative min eig " << negative_cases << "/50\n";
    EXPECT_GT(negative_cases, 0);
}

// (5) The §2.2 two-node one-way example: indefinite sym part; reduce() refuses.
TEST(PHOperator, OneWayTwoNodeExampleIsIndefiniteAndRefusedByReduce) {
    for (double d : {0.3, 1.0, 7.5}) {
        PHOperator op;
        op.n = 2;
        op.Q = {1.0, 1.0};
        op.Rdiag = {0.0, 0.0};
        op.one_way.push_back({0, 1, d});
        std::vector<double> L;
        ASSERT_TRUE(op.assembleFull(L));
        EXPECT_DOUBLE_EQ(L[0], d);
        EXPECT_DOUBLE_EQ(L[1], 0.0);
        EXPECT_DOUBLE_EQ(L[2], -d);
        EXPECT_DOUBLE_EQ(L[3], 0.0);
        const double me = minEigSym(L, 2);
        EXPECT_NEAR(me, d * (1.0 - std::sqrt(2.0)) / 2.0, 1e-12);
        EXPECT_LT(me, 0.0);

        const double P[4] = {1.0, 0.0, 0.0, 1.0};
        std::vector<double> M;
        EXPECT_FALSE(op.reduce(P, 2, M));
        EXPECT_FALSE(op.last_error.empty());
        EXPECT_NE(op.last_error.find("one-way"), std::string::npos);

        // Full order is fine: the step is l1-contractive and stable.
        double psi[2] = {1.0, -2.0};
        ASSERT_TRUE(op.backwardEulerStep(10.0, nullptr, psi));
        EXPECT_TRUE(std::isfinite(psi[0]) && std::isfinite(psi[1]));
    }
}

// (6) With no one-way edges, PHOperator reproduces RomDirectionalOperator
//     (gate g mapped to w' = w(1−g/2), j = −w·g/2, Rdiag += w·g/2 on both ends).
namespace {
struct DirGraph {
    int n; std::vector<int> a1, a2, sgn; std::vector<double> w, fr;
};
DirGraph makeDirGraph(std::mt19937& rng, int n, bool grounded, double fr_value) {
    std::uniform_real_distribution<double> uw(0.1, 10.0);
    DirGraph g; g.n = n;
    for (int i = 0; i + 1 < n; ++i) { g.a1.push_back(i); g.a2.push_back(i + 1); }
    for (int i = 0; i < 3; ++i) {
        const int a = static_cast<int>(rng() % static_cast<unsigned>(n)), b = static_cast<int>(rng() % static_cast<unsigned>(n));
        if (a != b) { g.a1.push_back(a); g.a2.push_back(b); }
    }
    if (grounded) { g.a1.push_back(n - 1); g.a2.push_back(-1); g.a1.push_back(-1); g.a2.push_back(0); }
    for (std::size_t c = 0; c < g.a1.size(); ++c) {
        g.w.push_back(uw(rng));
        g.sgn.push_back((rng() & 1u) ? 1 : -1);
        g.fr.push_back(fr_value);
    }
    return g;
}
PHOperator toPH(const DirGraph& g, const DirectionalOperatorConfig& cfg) {
    PHOperator op;
    op.n = g.n;
    op.Q.assign(static_cast<std::size_t>(g.n), 1.0);
    op.Rdiag.assign(static_cast<std::size_t>(g.n), 0.0);
    for (std::size_t c = 0; c < g.a1.size(); ++c) {
        const int i1 = g.a1[c], i2 = g.a2[c];
        const double w = g.w[c];
        if (i1 >= 0 && i2 >= 0) {
            int u = i1, d = i2;
            if (g.sgn[c] < 0) std::swap(u, d);
            const double gate = upstreamGate(std::fabs(g.fr[c]), cfg);
            PHOperator::TwoWay e;
            e.a = u; e.b = d; e.w = w * (1.0 - 0.5 * gate); e.j = -0.5 * w * gate;
            op.two_way.push_back(e);
            op.Rdiag[static_cast<std::size_t>(u)] += 0.5 * w * gate;
            op.Rdiag[static_cast<std::size_t>(d)] += 0.5 * w * gate;
        } else if (i1 >= 0 || i2 >= 0) {
            op.Rdiag[static_cast<std::size_t>(i1 >= 0 ? i1 : i2)] += w;
        }
    }
    return op;
}
} // namespace

TEST(PHOperator, MatchesRomDirectionalOperator) {
    std::mt19937 rng(41);
    DirectionalOperatorConfig cfg;
    struct Case { bool grounded; double fr; const char* name; bool exact; };
    const Case cases[] = {{false, 0.0, "g=0 ungrounded", true},
                          {true, 0.0, "g=0 grounded", false},
                          {false, 5.0, "g=1 ungrounded", false},
                          {true, 5.0, "g=1 grounded", false},
                          {true, 1.0, "g=0.5 grounded", false}};
    for (const auto& cs : cases) {
        double worst = 0.0;
        for (int t = 0; t < 20; ++t) {
            const int n = 7 + static_cast<int>(rng() % 12);
            const int k = std::min(6, n - 1);
            DirGraph g = makeDirGraph(rng, n, cs.grounded, cs.fr);
            std::vector<double> ones(static_cast<std::size_t>(n), 1.0);
            const auto P = qOrthonormal(rng, ones, k);
            std::vector<double> Mref, Mph;
            ASSERT_TRUE(assembleDirectionalReduced(P.data(), k, n, static_cast<int>(g.a1.size()),
                                                   g.a1.data(), g.a2.data(), g.w.data(),
                                                   g.sgn.data(), g.fr.data(), cfg, Mref));
            const PHOperator op = toPH(g, cfg);
            ASSERT_TRUE(op.reduce(P.data(), k, Mph)) << op.last_error;
            ASSERT_EQ(Mref.size(), Mph.size());
            const double scale = std::max(1.0, maxAbs(Mref));
            for (std::size_t i = 0; i < Mref.size(); ++i) {
                if (cs.exact) EXPECT_DOUBLE_EQ(Mref[i], Mph[i]) << cs.name;
                else          EXPECT_NEAR(Mref[i], Mph[i], 1e-13 * scale) << cs.name;
                worst = std::max(worst, std::fabs(Mref[i] - Mph[i]) / scale);
            }
        }
        std::cout << "[PH] " << cs.name << ": worst |ref - PH|/scale " << worst << "\n";
    }
}
