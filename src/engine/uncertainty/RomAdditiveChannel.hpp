/**
 * @file RomAdditiveChannel.hpp
 * @brief PR H19: full-order linearized additive-uncertainty channel.
 *
 * One head-deviation field Psi per additive source, shared by all members:
 *
 *     S dPsi/dt = -J Psi + dq
 *
 * J is the flow-response Jacobian (per conduit dQ = Ju*Psi_up + Jd*Psi_dn, per
 * structure dQ = d*Psi_up, oriented by flow), so extra water is carried
 * downstream step by step, one-way where the flow response is one-way. Backward
 * Euler: (S/dt + J) Psi_new = (S/dt) Psi_old + dq. The matrix is a strictly
 * column-diagonally-dominant M-matrix (column sums = S/dt plus outfall losses:
 * volume conservation), so the step is unconditionally stable. It is solved
 * directly (sparse LU, factored once per step, shared by every plane). See
 * docs/uncertainty/ADDITIVE_CHANNEL.md.
 */
#ifndef OPENSWMM_ENGINE_UNCERTAINTY_ROM_ADDITIVE_CHANNEL_HPP
#define OPENSWMM_ENGINE_UNCERTAINTY_ROM_ADDITIVE_CHANNEL_HPP

#include "PHOperator.hpp"
#include "RomFlowResponse.hpp"

#include <cassert>
#include <cstdint>
#include <string>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace openswmm::uncertainty {

/// The backward-Euler system for one routing step, shared by every plane.
///
/// Solved directly: the matrix is the same for every plane, so it is factored
/// once per step (build) and each plane costs one forward and one backward
/// pass (step). The elimination order is a minimum-degree order computed on the
/// network's link pattern; it is recomputed only when a link pair appears that
/// the pattern does not yet hold (links drop out of the edge list when their
/// flow stops, so the pattern is the union of everything seen). On a dendritic
/// network the order eliminates leaves first and there is no fill; each loop
/// adds a few entries. No pivoting is needed: the matrix is a strictly
/// column-diagonally-dominant M-matrix (column sums = S/dt plus outfall
/// losses), and elimination in any order keeps it one, so every pivot stays
/// positive. Pinned (spilling) nodes are held at zero by removing their row and
/// column, which leaves a principal submatrix of the same kind.
///
/// Until 2026-10-24 the system was solved by Gauss-Seidel. On Bellinge's real
/// storm that took 122 sweeps per step on average and hit its 200-sweep cap on
/// more than a tenth of steps; the direct solve is exact and costs microseconds
/// per plane, which is what makes one plane per member affordable.
struct AdditiveSystem {
    int n = 0;
    double dt = 0.0;
    std::vector<double> s_dt;    ///< S/dt
    std::vector<char> pinned;    ///< nodes held at zero (spilling)
    int last_sweeps = 0;         ///< 1 after a direct solve (kept for diagnostics)
    int symbolic_builds = 0;     ///< how often the elimination order was recomputed

    bool built = false;          ///< false until a build succeeds (step() is a no-op before)
    std::string last_error;      ///< set when build() refuses its input

    /// Assemble from the oriented flow-response edges, storage S (> 0) and dt,
    /// and factor. Delegates to the PHOperator overload (one assembly path):
    /// each conduit becomes the one-way terms ju (up -> dn) and -jd (dn -> up),
    /// each structure the one-way term d (up -> dn).
    bool build(int n_nodes, const std::vector<FlowEdge>& edges, const double* S,
               double dt_s, const std::vector<char>* pinned_nodes = nullptr) {
        const auto un = static_cast<std::size_t>(std::max(n_nodes, 0));
        PHOperator& op = scratch_;
        op.n = n_nodes;
        op.Q.assign(S, S + un);
        op.Rdiag.clear();
        op.two_way.clear();
        op.one_way.clear();
        for (const auto& e : edges) {
            if (!(e.up >= 0 && e.up < n_nodes)) continue;
            const bool dn_ok = e.dn >= 0 && e.dn < n_nodes;
            const int to = dn_ok ? e.dn : -1;
            if (e.conduit) {
                op.one_way.push_back({e.up, to, e.ju});            // outflow grows with the upstream head
                if (dn_ok) op.one_way.push_back({e.dn, e.up, -e.jd});   // ...and falls as the downstream head rises
            } else {
                op.one_way.push_back({e.up, to, e.d});
            }
        }
        pinned_scratch_.assign(un, 0);
        if (pinned_nodes)
            for (std::size_t i = 0; i < un && i < pinned_nodes->size(); ++i)
                pinned_scratch_[i] = static_cast<std::uint8_t>((*pinned_nodes)[i]);
        return build(op, dt_s, pinned_scratch_.data());
    }

    /// Assemble from a PHOperator and factor. Maps Q -> S, Rdiag and two-way
    /// edges (j must be 0) -> symmetric conductance, one-way edges -> the
    /// upstream-diagonal / downstream-offdiagonal pair (to = -1: outfall).
    /// Refuses (returns false, `built` = false, `last_error` set, nothing else
    /// touched) on bad dimensions or a two-way edge with j != 0: a J edge is not
    /// column-dominant, so the l1 contract (PHOperator.hpp, H19 stability) fails.
    /// `pinned` (size n, nullable): nodes held at zero (spilling).
    bool build(const PHOperator& op, double dt_s, const std::uint8_t* pinned_nodes = nullptr) {
        if (op.n < 0 || op.Q.size() != static_cast<std::size_t>(op.n) ||
            (!op.Rdiag.empty() && op.Rdiag.size() != static_cast<std::size_t>(op.n))) {
            last_error = "AdditiveSystem::build: bad PHOperator dimensions";
            built = false;
            return false;
        }
        for (const auto& e : op.two_way) {
            if (e.a < 0 || e.a >= op.n || e.b < 0 || e.b >= op.n) {
                last_error = "AdditiveSystem::build: two-way index out of range";
                built = false;
                return false;
            }
            if (e.j != 0.0) {
                last_error = "AdditiveSystem::build: two-way edge with j != 0 breaks the l1 "
                             "(column-dominant) contract of the full-order channel";
                built = false;
                return false;
            }
        }
        for (const auto& e : op.one_way)
            if (e.from < 0 || e.from >= op.n || e.to >= op.n) {
                last_error = "AdditiveSystem::build: one-way index out of range";
                built = false;
                return false;
            }
        last_error.clear();
#ifndef NDEBUG
        // H19 stability: (S/dt + L) is an M-matrix with column sums >= S/dt only
        // while L is column-dominant (non-negative ju, d, and -jd).
        assert(op.columnDominantSparse() && "AdditiveSystem: operator is not column-dominant (l1 contract)");
#endif
        n = op.n;
        dt = dt_s;
        const auto un = static_cast<std::size_t>(std::max(n, 0));
        s_dt.assign(un, 0.0);
        pinned.assign(un, 0);
        if (pinned_nodes)
            for (std::size_t i = 0; i < un; ++i) pinned[i] = static_cast<char>(pinned_nodes[i]);
        for (std::size_t i = 0; i < un; ++i) s_dt[i] = op.Q[i] / dt;
        if (sym_n_ != n) { pattern_.assign(un, {}); sym_n_ = n; order_.clear(); }
        // Grow the pattern with any pair not yet in it.
        bool grew = order_.empty() && n > 0;
        for (const auto& e : op.one_way)
            if (e.to >= 0 && e.from != e.to) grew |= addPair_(e.from, e.to);
        for (const auto& e : op.two_way)
            if (e.a != e.b) grew |= addPair_(e.a, e.b);
        if (grew) symbolic_();
        // Numeric assembly: A x = rhs with A[i][i] = S/dt + outgoing slopes, and
        // A[row][col] = -(dQ into row per unit head at col).
        std::fill(D_.begin(), D_.end(), 0.0);
        std::fill(L_.begin(), L_.end(), 0.0);
        std::fill(U_.begin(), U_.end(), 0.0);
        for (std::size_t i = 0; i < un; ++i) D_[i] = s_dt[i];
        for (std::size_t i = 0; i < un && !op.Rdiag.empty(); ++i) D_[i] += op.Rdiag[i];
        for (const auto& e : op.two_way) {
            if (e.a == e.b) continue;
            D_[static_cast<std::size_t>(e.a)] += e.w;
            D_[static_cast<std::size_t>(e.b)] += e.w;
            addEntry_(e.a, e.b, -e.w);
            addEntry_(e.b, e.a, -e.w);
        }
        for (const auto& e : op.one_way) {
            D_[static_cast<std::size_t>(e.from)] += e.d;
            if (e.to >= 0) addEntry_(e.to, e.from, -e.d);
        }
        for (std::size_t k = 0; k < un; ++k) {
            if (!pinned[k]) continue;
            D_[k] = 1.0;
            for (int m = off_[k]; m < off_[k + 1]; ++m) { L_[static_cast<std::size_t>(m)] = 0.0; U_[static_cast<std::size_t>(m)] = 0.0; }
        }
        // Rows and columns of pinned nodes held by an earlier-eliminated node.
        for (std::size_t k = 0; k < un; ++k)
            for (int m = off_[k]; m < off_[k + 1]; ++m)
                if (pinned[static_cast<std::size_t>(later_[static_cast<std::size_t>(m)])]) {
                    L_[static_cast<std::size_t>(m)] = 0.0;
                    U_[static_cast<std::size_t>(m)] = 0.0;
                }
        // Factor in elimination order.
        for (int k : order_) {
            const auto uk = static_cast<std::size_t>(k);
            const double piv = D_[uk];
            if (!(piv > 0.0)) continue;   // cannot happen for S > 0; keeps a bad input finite
            const int a0 = off_[uk], a1 = off_[uk + 1];
            for (int ia = a0; ia < a1; ++ia) {
                const double lik = L_[static_cast<std::size_t>(ia)];   // A[a][k]
                if (lik == 0.0) continue;
                const double f = lik / piv;
                for (int ib = a0; ib < a1; ++ib) {
                    const double ukb = U_[static_cast<std::size_t>(ib)];   // A[k][b]
                    if (ukb == 0.0) continue;
                    const Slot& t = upd_[static_cast<std::size_t>(updOff_[static_cast<std::size_t>(ia)] + (ib - a0))];
                    if (t.idx < 0) continue;   // every fill entry exists by construction
                    *target_(t) -= f * ukb;
                }
            }
        }
        last_sweeps = 0;
        built = true;
        return true;
    }

    /// One backward-Euler step of one plane: psi (in: old, out: new), dq in cfs.
    /// The two trailing arguments are kept for interface compatibility with the
    /// former iterative solver and are ignored. Returns 1.
    int step(const double* dq, double* psi, int /*max_sweeps*/ = 200, double /*tol*/ = 1.0e-10) {
        if (!built) return 0;
        const auto un = static_cast<std::size_t>(std::max(n, 0));
        rhs_.resize(un);
        for (std::size_t i = 0; i < un; ++i) rhs_[i] = pinned[i] ? 0.0 : s_dt[i] * psi[i] + dq[i];
        for (int k : order_) {                       // forward: L
            const auto uk = static_cast<std::size_t>(k);
            const double bk = rhs_[uk];
            if (bk == 0.0) continue;
            const double f = bk / D_[uk];
            for (int m = off_[uk]; m < off_[uk + 1]; ++m)
                rhs_[static_cast<std::size_t>(later_[static_cast<std::size_t>(m)])] -= L_[static_cast<std::size_t>(m)] * f;
        }
        for (auto it = order_.rbegin(); it != order_.rend(); ++it) {   // backward: U
            const auto uk = static_cast<std::size_t>(*it);
            double v = rhs_[uk];
            for (int m = off_[uk]; m < off_[uk + 1]; ++m)
                v -= U_[static_cast<std::size_t>(m)] * psi[static_cast<std::size_t>(later_[static_cast<std::size_t>(m)])];
            v /= D_[uk];
            psi[uk] = (pinned[uk] || !std::isfinite(v)) ? 0.0 : v;
        }
        last_sweeps = 1;
        return 1;
    }

private:
    // Update target of eliminating node k on the pair (a, b) of its later
    // neighbours: the diagonal of a, or the L or U entry that holds A[a][b].
    struct Slot { int kind = 0; int idx = 0; };   // 0 = D[idx], 1 = U[idx], 2 = L[idx]
    double* target_(const Slot& t) {
        return t.kind == 0 ? &D_[static_cast<std::size_t>(t.idx)]
             : t.kind == 1 ? &U_[static_cast<std::size_t>(t.idx)] : &L_[static_cast<std::size_t>(t.idx)];
    }
    bool addPair_(int a, int b) {
        auto& pa = pattern_[static_cast<std::size_t>(a)];
        if (std::find(pa.begin(), pa.end(), b) != pa.end()) return false;
        pa.push_back(b);
        pattern_[static_cast<std::size_t>(b)].push_back(a);
        return true;
    }
    // Flat index of the entry between k and its later neighbour j, or -1.
    int find_(int k, int j) const {
        const auto uk = static_cast<std::size_t>(k);
        for (int m = off_[uk]; m < off_[uk + 1]; ++m)
            if (later_[static_cast<std::size_t>(m)] == j) return m;
        return -1;
    }
    void addEntry_(int row, int col, double v) {
        if (pos_[static_cast<std::size_t>(row)] < pos_[static_cast<std::size_t>(col)]) {
            const int m = find_(row, col);
            if (m >= 0) U_[static_cast<std::size_t>(m)] += v;   // A[row][col], row eliminated first
        } else {
            const int m = find_(col, row);
            if (m >= 0) L_[static_cast<std::size_t>(m)] += v;   // A[row][col], col eliminated first
        }
    }
    // Minimum-degree elimination order on the pattern, its fill, and the
    // update map used by the numeric factor.
    void symbolic_() {
        ++symbolic_builds;
        const auto un = static_cast<std::size_t>(std::max(n, 0));
        std::vector<std::vector<int>> g = pattern_;
        for (auto& v : g) std::sort(v.begin(), v.end());
        std::vector<char> done(un, 0);
        std::vector<std::vector<int>> later(un);
        order_.clear();
        order_.reserve(un);
        for (std::size_t step = 0; step < un; ++step) {
            int k = -1;
            std::size_t best = 0;
            for (std::size_t i = 0; i < un; ++i)
                if (!done[i] && (k < 0 || g[i].size() < best)) { k = static_cast<int>(i); best = g[i].size(); }
            const auto uk = static_cast<std::size_t>(k);
            done[uk] = 1;
            order_.push_back(k);
            later[uk] = g[uk];
            for (int a : g[uk]) {                    // k's neighbours become a clique
                auto& ga = g[static_cast<std::size_t>(a)];
                ga.erase(std::remove(ga.begin(), ga.end(), k), ga.end());
                for (int b : g[uk])
                    if (b != a && !std::binary_search(ga.begin(), ga.end(), b))
                        ga.insert(std::upper_bound(ga.begin(), ga.end(), b), b);
            }
            g[uk].clear();
        }
        pos_.assign(un, 0);
        for (std::size_t i = 0; i < un; ++i) pos_[static_cast<std::size_t>(order_[i])] = static_cast<int>(i);
        off_.assign(un + 1, 0);
        for (std::size_t i = 0; i < un; ++i) off_[i + 1] = off_[i] + static_cast<int>(later[i].size());
        later_.resize(static_cast<std::size_t>(off_[un]));
        for (std::size_t i = 0; i < un; ++i)
            std::copy(later[i].begin(), later[i].end(), later_.begin() + off_[i]);
        D_.assign(un, 0.0);
        L_.assign(later_.size(), 0.0);
        U_.assign(later_.size(), 0.0);
        // For every entry (k, a) and every (k, b) of the same node: where
        // A[a][b] lives.
        updOff_.assign(later_.size(), 0);
        upd_.clear();
        for (std::size_t k = 0; k < un; ++k)
            for (int ia = off_[k]; ia < off_[k + 1]; ++ia) {
                updOff_[static_cast<std::size_t>(ia)] = static_cast<int>(upd_.size());
                const int a = later_[static_cast<std::size_t>(ia)];
                for (int ib = off_[k]; ib < off_[k + 1]; ++ib) {
                    const int b = later_[static_cast<std::size_t>(ib)];
                    Slot t;
                    if (a == b) { t.kind = 0; t.idx = a; }
                    else if (pos_[static_cast<std::size_t>(a)] < pos_[static_cast<std::size_t>(b)]) { t.kind = 1; t.idx = find_(a, b); }
                    else { t.kind = 2; t.idx = find_(b, a); }
                    upd_.push_back(t);
                }
            }
    }

    int sym_n_ = -1;
    std::vector<std::vector<int>> pattern_;  ///< undirected link pairs seen so far
    std::vector<int> order_, pos_;           ///< elimination order and its inverse
    std::vector<int> off_, later_;           ///< per node: neighbours eliminated after it (with fill)
    std::vector<double> D_, L_, U_;          ///< pivots; A[later][k]; A[k][later]
    std::vector<int> updOff_;
    std::vector<Slot> upd_;
    std::vector<double> rhs_;
    PHOperator scratch_;                     ///< reused by the array overload of build()
    std::vector<std::uint8_t> pinned_scratch_;
};

}  // namespace openswmm::uncertainty

#endif  // OPENSWMM_ENGINE_UNCERTAINTY_ROM_ADDITIVE_CHANNEL_HPP
