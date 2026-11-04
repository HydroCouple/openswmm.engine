/**
 * @file RomPortGraph.hpp
 * @brief One implicit per-member step on a graph of stores and monotone ports
 *        (PORT_HAMILTONIAN phase 5, docs/uncertainty/PH_STORES_AND_PORTS.md §1.4, §3).
 *
 * Unknowns z (head deviation from the main run, ft) on every node. Per node t
 *
 *   F_t(z) = [dV_t(z_t) − dV_t^n]/dt + Σ_out flow − Σ_in flow − b_t = 0
 *
 * with dV_t = S_t·z_t at a linear node and a strictly increasing volume
 * deviation at a store; each link is exactly once either a plane edge
 * (FlowEdge: ju·z_up + jd·z_dn, or d·z_up) or a port (a monotone law
 * dQ(z_up, z_dn)). A port or edge whose dn is −1 leaves the system (outfall,
 * overflow, street). Pinned nodes are held at zero, as in the shared planes.
 *
 * With monotone ports F is an M-function (Rheinboldt): unique solution,
 * b <= b' ⇒ z <= z', and ‖dV(z) − dV(z')‖₁ <= ‖dV^n − dV'^n‖₁ + dt‖b − b'‖₁
 * (conservation + monotonicity, Crandall–Tartar). The Jacobian is a
 * column-dominant M-matrix.
 *
 * Partition (O-5): N = every store and every node a port touches, L = every
 * other unpinned node. L rows are linear with a member-independent matrix
 * (S/dt + plane edges); it is factored once per graph (AdditiveSystem LU,
 * minimum-degree order, no pivoting) with its Schur complement on N, and each
 * Newton pass factors only the k×k system J_NN − A_NL·A_LL⁻¹·A_LN (dense, k ~ the
 * admitted stores, ponds and their port neighbours). The iterates are those of
 * the full-network Newton; z_L is recovered with one L solve per member.
 *
 * Solve: semismooth (active-set) Newton for piecewise-smooth monotone ports
 * (PORT_HAMILTONIAN O-4). Port slopes are one-sided differences on both sides;
 * a kink inside the difference step is resolved by the far differences (the
 * two pieces' slopes), and a port sitting on a kink takes its slopes on the
 * piece the solved step enters (the B-derivative in the step's direction;
 * re-solved while any changes).
 * Armijo on ‖F‖₁ from the full step; when it fails, each N node's step is
 * bounded by its own laws' first strong kink (O-6: no further than twice its
 * distance, then just past it; the L block takes the exact linear response),
 * then the whole step by the first kink anywhere, then halving. A kink is
 * placed at t_c/(2 − t_c) from the crossing parameter t_c when the law curves
 * before it. Convergence is the residual tolerance, or the
 * roundoff floor of the level (atFloor_; counted in stats.fp_floor_converged),
 * the same for Newton and for the fallback monotone nonlinear Jacobi, which is
 * counted as an anomaly. F == 0 exactly at the start (zero data) returns without iterating.
 */
#ifndef OPENSWMM_ENGINE_UNCERTAINTY_ROM_PORT_GRAPH_HPP
#define OPENSWMM_ENGINE_UNCERTAINTY_ROM_PORT_GRAPH_HPP

#include "RomAdditiveChannel.hpp"
#include "RomFlowResponse.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace openswmm::uncertainty {

struct PortGraph {
    /// A monotone two-port: extra flow up -> dn for member `member` at deviations
    /// (z_up, z_dn). Must be nondecreasing in z_up, nonincreasing in z_dn, and 0 at
    /// (0, 0) unless the member itself forces it (a member's own street pond).
    struct Port {
        int up = -1, dn = -1;
        std::function<double(int member, double z_up, double z_dn)> dq;
    };
    /// A store: volume deviation dV(z) (ft^3, strictly increasing, 0 at 0) and its area dV/dz (ft^2).
    struct Store {
        std::function<double(double z, double* area)> dV;
    };

    // ---- the graph (shared by members) ----
    int n = 0;
    double dt = 0.0;
    std::vector<double> S;                 ///< linear store area (ft^2) per node
    std::vector<char> pinned;              ///< held at zero
    std::vector<int> store_of;             ///< node -> index in stores, or -1 (linear)
    std::vector<Store> stores;
    std::vector<FlowEdge> edges;           ///< plane edges (linear)
    std::vector<Port> ports;

    // ---- per member ----
    int member = 0;
    std::vector<double> b;                 ///< extra inflow (cfs) per node
    std::vector<double> z_prev;            ///< level deviation at the start of the step (linear nodes)
    std::vector<double> dV_prev;           ///< per store: volume deviation at the start of the step

    // ---- solver settings and counters ----
    int max_newton = 20;
    int max_jacobi = 500;
    bool floor_projection = false;         ///< mutation hook only: clamp z >= floor (creates water)
    std::vector<double> floor;             ///< used only with floor_projection
    std::vector<double> level;             ///< main-run absolute head h* (ft) per node, for the fp floor (0 = unknown)
    /// factorisations: k×k Schur-complement factorisations (one per Newton pass);
    /// l_factorisations: factorisations of the linear L block (one per graph, prepare());
    /// l_solves: forward/backward passes on the L factor; k_max: largest N block seen.
    /// Failure dump hook (diagnostic only, PORT_HAMILTONIAN O-6): when set, every
    /// solve whose Newton fails calls it once after the Jacobi fallback, with the
    /// start, the Newton exit, the final iterate and Newton's per-iteration trace.
    /// It never changes the solve. The engine installs it only when
    /// OPENSWMM_PH_DUMP_DIR is set (RomPortGraphDump.hpp writes the file).
    struct TraceRec {
        double r0 = 0.0, r1 = 0.0, lam = 0.0;   ///< ‖F‖₁ before and after the step, step length
        int flags = 0;                          ///< 1 kink-bounded, 2 Armijo failed (best/past-kink step taken), 4 fp floor, 8 bounded per node
        int worst = -1;                         ///< node with the largest |F| after the step
        double worst_F = 0.0;
        int big = -1;                           ///< node with the largest |dz| of the step
        double big_dz = 0.0;
    };
    struct FailureInfo {
        bool converged = false;                 ///< the Jacobi fallback converged
        std::vector<double> z_start, z_newton;  ///< z at entry; Newton's last iterate (L recovered)
        std::vector<TraceRec> trace;
    };
    std::function<void(const PortGraph&, const FailureInfo&, const std::vector<double>& z_final)> on_failure;
    double sim_time = 0.0;                                   ///< label for the dump (s)
    const std::vector<std::string>* node_names = nullptr;    ///< label for the dump (per node)
    /// The N set of the last prepare() and its plane Schur base J_NN − A_NL·A_LL⁻¹·A_LN (k×k, row-major).
    const std::vector<int>& nSet() const { return part_.Nn; }
    const std::vector<double>& schurBase() const { return part_.Jbase; }

    struct Stats { long solves = 0, newton_iters = 0, armijo_failures = 0, jacobi_fallbacks = 0, jacobi_failed = 0, zero_exits = 0, port_evals = 0, factorisations = 0, side_switches = 0, kink_bounded = 0, fp_floor_converged = 0, node_bounded = 0, l_factorisations = 0, l_solves = 0, k_max = 0; };
    mutable Stats stats;   ///< mutable: residual() (const) counts port evaluations

    void resize(int n_nodes) {
        n = n_nodes;
        const auto un = static_cast<std::size_t>(std::max(n, 0));
        S.assign(un, 1.0); pinned.assign(un, 0); store_of.assign(un, -1);
        b.assign(un, 0.0); z_prev.assign(un, 0.0); level.assign(un, 0.0);
        stores.clear(); edges.clear(); ports.clear(); dV_prev.clear();
        part_.valid = false;
    }

    /// Water the port moves over the step (ft^3, up -> dn) at z: dt · Q_port.
    /// The 1D↔2D booking uses this, so the transfer to the 2D ROM is the port's
    /// own flow by construction.
    double portVolume(std::size_t port_index, const std::vector<double>& z) const {
        const Port& p = ports[port_index];
        auto zz = [&](int t) { return (t < 0 || pinned[static_cast<std::size_t>(t)]) ? 0.0 : z[static_cast<std::size_t>(t)]; };
        return dt * evalPort(p, zz(p.up), zz(p.dn));
    }

    /// One port-law evaluation for the current member (counted in stats.port_evals).
    double evalPort(const Port& p, double zu, double zd) const {
        ++stats.port_evals;
        return p.dq(member, zu, zd);
    }

    /// Append a node (after resize()): linear area S, not pinned, no source.
    /// Returns its index. Used for stores that are not network nodes (the
    /// street pond of a coupled manhole).
    int addNode(double area) {
        S.push_back(area); pinned.push_back(0); store_of.push_back(-1);
        b.push_back(0.0); z_prev.push_back(0.0); level.push_back(0.0);
        part_.valid = false;
        return n++;
    }

    /// Volume deviation at node t (store or linear) and its area.
    double volume(int t, double z, double* area) const {
        const int s = store_of[static_cast<std::size_t>(t)];
        if (s >= 0) return stores[static_cast<std::size_t>(s)].dV(z, area);
        if (area) *area = S[static_cast<std::size_t>(t)];
        return S[static_cast<std::size_t>(t)] * z;
    }
    double volumePrev(int t) const {
        const int s = store_of[static_cast<std::size_t>(t)];
        return s >= 0 ? dV_prev[static_cast<std::size_t>(s)] : S[static_cast<std::size_t>(t)] * z_prev[static_cast<std::size_t>(t)];
    }

    /// Residual F (cfs) and per-node magnitude of its terms (for the roundoff tolerance).
    void residual(const std::vector<double>& z, std::vector<double>& F, std::vector<double>* mag = nullptr) const {
        const auto un = static_cast<std::size_t>(n);
        F.assign(un, 0.0);
        if (mag) mag->assign(un, 0.0);
        auto zz = [&](int t) { return (t < 0 || pinned[static_cast<std::size_t>(t)]) ? 0.0 : z[static_cast<std::size_t>(t)]; };
        auto add = [&](int t, double v) {
            if (t < 0) return;
            F[static_cast<std::size_t>(t)] += v;
            if (mag) (*mag)[static_cast<std::size_t>(t)] += std::fabs(v);
        };
        for (int t = 0; t < n; ++t) {
            if (pinned[static_cast<std::size_t>(t)]) continue;
            add(t, (volume(t, z[static_cast<std::size_t>(t)], nullptr) - volumePrev(t)) / dt);
            add(t, -b[static_cast<std::size_t>(t)]);
        }
        for (const auto& e : edges) {
            if (e.up < 0) continue;
            const double f = e.conduit ? e.ju * zz(e.up) + (e.dn >= 0 ? e.jd * zz(e.dn) : 0.0) : e.d * zz(e.up);
            add(e.up, f); add(e.dn, -f);
        }
        for (const auto& p : ports) {
            const double f = evalPort(p, zz(p.up), zz(p.dn));
            add(p.up, f); add(p.dn, -f);
        }
        for (std::size_t t = 0; t < un; ++t)
            if (pinned[t]) F[t] = z[t];
    }

    /// Water leaving the graph per second at z: edges and ports to dn = −1 or into pinned nodes.
    double boundaryOutflow(const std::vector<double>& z) const {
        auto zz = [&](int t) { return (t < 0 || pinned[static_cast<std::size_t>(t)]) ? 0.0 : z[static_cast<std::size_t>(t)]; };
        auto out = [&](int up, int dn) {
            const bool up_in = up >= 0 && !pinned[static_cast<std::size_t>(up)];
            const bool dn_in = dn >= 0 && !pinned[static_cast<std::size_t>(dn)];
            return (up_in ? 1.0 : 0.0) * (dn_in ? 0.0 : 1.0) - (dn_in ? 1.0 : 0.0) * (up_in ? 0.0 : 1.0);
        };
        double s = 0.0;
        for (const auto& e : edges) {
            if (e.up < 0) continue;
            const double f = e.conduit ? e.ju * zz(e.up) + (e.dn >= 0 ? e.jd * zz(e.dn) : 0.0) : e.d * zz(e.up);
            s += out(e.up, e.dn) * f;
        }
        for (const auto& p : ports) s += out(p.up, p.dn) * evalPort(p, zz(p.up), zz(p.dn));
        return s;
    }

    /// Solve F(z) = 0 for the current member. z: in = start (normally z_prev), out = solution.
    /// Returns true when converged (Newton, or the Jacobi fallback).
    ///
    /// O-5: the unknowns split into N (every store and every node a port
    /// touches) and L (every other unpinned node). The L rows are linear, and
    /// their matrix (S/dt plus the plane edges) is the same for every member, so
    /// it is factored once per graph (prepare()) and Newton solves only the k×k
    /// Schur complement J_NN − A_NL·A_LL⁻¹·A_LN. The iterates are the
    /// full-network Newton iterates (same step, same Armijo norm, same
    /// tolerances): along a Newton step the linear L residual scales exactly by
    /// (1 − λ), so it is carried as one scalar, and z_L is recovered with one L
    /// solve at the end.
    bool solve(std::vector<double>& z) {
        ++stats.solves;
        const auto un = static_cast<std::size_t>(n);
        for (std::size_t t = 0; t < un; ++t) if (pinned[t]) z[t] = 0.0;
        std::vector<double> F, mag;
        residual(z, F, &mag);
        bool all_zero = true;
        for (double v : F) if (v != 0.0) { all_zero = false; break; }
        if (all_zero) { ++stats.zero_exits; return true; }
        bool ok = residualOk_(F, mag, z);
        if (!ok) {
            prepare();
            FailureInfo* fi = nullptr;
            FailureInfo info;
            if (on_failure) { fi = &info; info.z_start = z; }
            trace_ = fi ? &info.trace : nullptr;
            ok = newton_(z, F);
            trace_ = nullptr;
            if (!ok) {
                ++stats.jacobi_fallbacks;
                if (fi) info.z_newton = z;
                ok = jacobi_(z);
                if (!ok) ++stats.jacobi_failed;
                if (fi) { info.converged = ok; on_failure(*this, info, z); }
            }
        }
        if (floor_projection)
            for (std::size_t t = 0; t < un && t < floor.size(); ++t) z[t] = std::max(z[t], floor[t]);
        return ok;
    }

    /// Partition the unknowns into N and L and factor the linear L block, with
    /// its Schur complement on N (k L solves). Member-independent: solve() calls
    /// it, and it does the work only when the graph changed since the last call
    /// (resize() and addNode() always mark it stale).
    void prepare() {
        if (part_.valid && !structureChanged_()) return;
        Part& pt = part_;
        const auto un = static_cast<std::size_t>(n);
        pt.valid = true;
        pt.sig_n = n; pt.sig_dt = dt; pt.sig_S = S; pt.sig_pinned = pinned;
        pt.sig_store.assign(un, 0);
        for (std::size_t t = 0; t < un; ++t) pt.sig_store[t] = store_of[t] >= 0 ? 1 : 0;
        pt.sig_edges = edges;
        pt.sig_ports.clear();
        for (const auto& p : ports) pt.sig_ports.emplace_back(p.up, p.dn);
        // Classes: 0 pinned, 1 N (store or port end), 2 L.
        pt.cls.assign(un, 2);
        for (std::size_t t = 0; t < un; ++t) {
            if (pinned[t]) pt.cls[t] = 0;
            else if (store_of[t] >= 0) pt.cls[t] = 1;
        }
        for (const auto& p : ports)
            for (int e : {p.up, p.dn})
                if (e >= 0 && !pinned[static_cast<std::size_t>(e)]) pt.cls[static_cast<std::size_t>(e)] = 1;
        pt.Nn.clear(); pt.Ln.clear(); pt.loc.assign(un, -1);
        for (std::size_t t = 0; t < un; ++t) {
            if (pt.cls[t] == 1) { pt.loc[t] = static_cast<int>(pt.Nn.size()); pt.Nn.push_back(static_cast<int>(t)); }
            else if (pt.cls[t] == 2) { pt.loc[t] = static_cast<int>(pt.Ln.size()); pt.Ln.push_back(static_cast<int>(t)); }
        }
        const std::size_t k = pt.Nn.size(), nL = pt.Ln.size();
        stats.k_max = std::max(stats.k_max, static_cast<long>(k));
        auto cl = [&](int t) { return t < 0 ? 0 : pt.cls[static_cast<std::size_t>(t)]; };
        auto lc = [&](int t) { return static_cast<std::size_t>(pt.loc[static_cast<std::size_t>(t)]); };
        // Plane-edge Jacobian entries by block; the L block goes to the L system.
        pt.Jbase.assign(k * k, 0.0);
        pt.NL.clear(); pt.LN.clear(); pt.eN.clear();
        pt.edgeAbs.assign(un, 0.0);
        pt.bloc.assign(un, -1); pt.bnd.clear();
        PHOperator op;
        op.n = static_cast<int>(nL);
        op.Q.resize(nL);
        for (std::size_t l = 0; l < nL; ++l) op.Q[l] = std::max(S[static_cast<std::size_t>(pt.Ln[l])], 1.0e-9);
        auto ent = [&](int r, int c, double v) {
            const int cr = cl(r), cc = cl(c);
            if (cr == 0 || cc == 0) return;
            if (cr == 1 && cc == 1) pt.Jbase[lc(r) * k + lc(c)] += v;
            else if (cr == 1) pt.NL.push_back({r, c, v});
            else if (cc == 1) pt.LN.push_back({r, c, v});
        };
        auto markBnd = [&](int t) {
            if (cl(t) == 2 && pt.bloc[static_cast<std::size_t>(t)] < 0) {
                pt.bloc[static_cast<std::size_t>(t)] = static_cast<int>(pt.bnd.size());
                pt.bnd.push_back(t);
            }
        };
        for (std::size_t q = 0; q < edges.size(); ++q) {
            const auto& e = edges[q];
            if (!(e.up >= 0 && e.up < n)) continue;
            const int dn = (e.dn >= 0 && e.dn < n) ? e.dn : -1;
            if (cl(e.up) == 1 || cl(dn) == 1) {
                pt.eN.push_back(q);
                markBnd(e.up);
                if (dn >= 0) markBnd(dn);
            }
            if (e.conduit) {
                ent(e.up, e.up, e.ju);
                if (dn >= 0) { ent(e.up, dn, e.jd); ent(dn, e.up, -e.ju); ent(dn, dn, -e.jd); }
                pt.edgeAbs[static_cast<std::size_t>(e.up)] += std::fabs(e.ju);
                if (dn >= 0) pt.edgeAbs[static_cast<std::size_t>(dn)] += std::fabs(e.jd);
                if (cl(e.up) == 2) op.one_way.push_back({static_cast<int>(lc(e.up)), cl(dn) == 2 ? static_cast<int>(lc(dn)) : -1, e.ju});
                if (cl(dn) == 2) op.one_way.push_back({static_cast<int>(lc(dn)), cl(e.up) == 2 ? static_cast<int>(lc(e.up)) : -1, -e.jd});
            } else {
                ent(e.up, e.up, e.d);
                if (dn >= 0) ent(dn, e.up, -e.d);
                pt.edgeAbs[static_cast<std::size_t>(e.up)] += std::fabs(e.d);
                if (cl(e.up) == 2) op.one_way.push_back({static_cast<int>(lc(e.up)), cl(dn) == 2 ? static_cast<int>(lc(dn)) : -1, e.d});
            }
        }
        // Diagonal slopes of the L rows (member-independent) for the roundoff floor.
        diag_slope_.assign(un, 0.0);
        for (int t : pt.Ln) diag_slope_[static_cast<std::size_t>(t)] = op.Q[lc(t)] / dt + pt.edgeAbs[static_cast<std::size_t>(t)];
        const std::size_t nb = pt.bnd.size();
        pt.Wb.assign(nb * k, 0.0);
        if (nL == 0) return;
        pt.lsys.build(op, dt);
        ++stats.l_factorisations;
        // W = A_LL⁻¹ A_LN column by column (only N nodes with an L neighbour);
        // the Schur complement J_NN − A_NL W and W's boundary rows are kept.
        std::vector<std::vector<std::pair<int, double>>> col(k);
        for (const auto& e : pt.LN) col[lc(e.c)].emplace_back(static_cast<int>(lc(e.r)), e.v);
        std::vector<double> x(nL);
        for (std::size_t j = 0; j < k; ++j) {
            if (col[j].empty()) continue;
            std::fill(x.begin(), x.end(), 0.0);
            for (const auto& [l, v] : col[j]) x[static_cast<std::size_t>(l)] += v;
            lsolve_(x);
            for (std::size_t b = 0; b < nb; ++b) pt.Wb[b * k + j] = x[lc(pt.bnd[b])];
            for (const auto& e : pt.NL) pt.Jbase[lc(e.r) * k + j] -= e.v * x[lc(e.c)];
        }
    }

private:
    /// N–L partition and the factored L block (O-5, prepare()).
    struct Ent { int r = 0, c = 0; double v = 0.0; };
    struct Part {
        bool valid = false;
        std::vector<int> cls, loc;            ///< per node: 0 pinned, 1 N, 2 L; index within N or L
        std::vector<int> Nn, Ln;              ///< the N and L nodes
        std::vector<int> bnd, bloc;           ///< L nodes with a plane edge to N; per node its slot or -1
        std::vector<std::size_t> eN;          ///< plane edges with an end in N
        std::vector<Ent> NL, LN;              ///< plane-edge Jacobian entries across the blocks (node indices)
        std::vector<double> Jbase;            ///< k×k: plane-edge J_NN minus A_NL A_LL⁻¹ A_LN
        std::vector<double> Wb;               ///< bnd×k: rows of A_LL⁻¹ A_LN at the boundary L nodes
        std::vector<double> edgeAbs;          ///< per node: Σ |plane-edge slope| on its diagonal
        AdditiveSystem lsys;                  ///< A_LL (minimum-degree LU, no pivoting)
        // What the partition was built from.
        int sig_n = -1;
        double sig_dt = 0.0;
        std::vector<double> sig_S;
        std::vector<char> sig_pinned, sig_store;
        std::vector<FlowEdge> sig_edges;
        std::vector<std::pair<int, int>> sig_ports;
    };
    Part part_;
    std::vector<double> lpsi_;
    std::vector<TraceRec>* trace_ = nullptr;   ///< Newton's per-iteration record, only while on_failure is set

    bool structureChanged_() const {
        const Part& pt = part_;
        if (pt.sig_n != n || pt.sig_dt != dt || pt.sig_S != S || pt.sig_pinned != pinned) return true;
        if (pt.sig_edges.size() != edges.size() || pt.sig_ports.size() != ports.size()) return true;
        for (std::size_t t = 0; t < static_cast<std::size_t>(n); ++t)
            if (pt.sig_store[t] != (store_of[t] >= 0 ? 1 : 0)) return true;
        for (std::size_t q = 0; q < edges.size(); ++q) {
            const FlowEdge &a = edges[q], &b = pt.sig_edges[q];
            if (a.up != b.up || a.dn != b.dn || a.conduit != b.conduit || a.ju != b.ju || a.jd != b.jd || a.d != b.d) return true;
        }
        for (std::size_t q = 0; q < ports.size(); ++q)
            if (pt.sig_ports[q].first != ports[q].up || pt.sig_ports[q].second != ports[q].dn) return true;
        return false;
    }

    /// x <- A_LL⁻¹ x (one forward and one backward pass on the L factor).
    void lsolve_(std::vector<double>& x) {
        lpsi_.assign(x.size(), 0.0);
        part_.lsys.step(x.data(), lpsi_.data());
        x.swap(lpsi_);
        ++stats.l_solves;
    }

    /// Residual rows of the N nodes only (the other entries of F and mag are left
    /// as they are). Same terms, in the same order, as residual().
    void residualN_(const std::vector<double>& z, std::vector<double>& F, std::vector<double>& mag) const {
        const Part& pt = part_;
        const auto un = static_cast<std::size_t>(n);
        F.resize(un); mag.resize(un);
        for (int t : pt.Nn) { F[static_cast<std::size_t>(t)] = 0.0; mag[static_cast<std::size_t>(t)] = 0.0; }
        auto zz = [&](int t) { return (t < 0 || pinned[static_cast<std::size_t>(t)]) ? 0.0 : z[static_cast<std::size_t>(t)]; };
        auto add = [&](int t, double v) {
            if (t < 0 || pt.cls[static_cast<std::size_t>(t)] != 1) return;
            F[static_cast<std::size_t>(t)] += v;
            mag[static_cast<std::size_t>(t)] += std::fabs(v);
        };
        for (int t : pt.Nn) {
            add(t, (volume(t, z[static_cast<std::size_t>(t)], nullptr) - volumePrev(t)) / dt);
            add(t, -b[static_cast<std::size_t>(t)]);
        }
        for (std::size_t q : pt.eN) {
            const auto& e = edges[q];
            const double f = e.conduit ? e.ju * zz(e.up) + (e.dn >= 0 ? e.jd * zz(e.dn) : 0.0) : e.d * zz(e.up);
            add(e.up, f); add(e.dn, -f);
        }
        for (const auto& p : ports) {
            const double f = evalPort(p, zz(p.up), zz(p.dn));
            add(p.up, f); add(p.dn, -f);
        }
    }

    /// One row of the residual tolerance (see residualOk_).
    bool rowOk_(std::size_t t, const std::vector<double>& FF, const std::vector<double>& M, const std::vector<double>& zc) const {
        double a = 0.0;
        volume(static_cast<int>(t), zc[t], &a);
        const double tol = 1.0e-10 * std::max(a, 1.0e-6) / dt + 1.0e-12 * M[t];
        return std::fabs(FF[t]) <= tol;
    }

    // Small dense LU with partial pivoting, row-major k×k; b <- A⁻¹ b. A
    // non-finite result component is set to 0 (as AdditiveSystem::step does).
    static void denseSolve_(std::vector<double>& A, std::vector<double>& x, std::size_t k) {
        std::vector<std::size_t> perm(k);
        for (std::size_t i = 0; i < k; ++i) perm[i] = i;
        for (std::size_t c = 0; c < k; ++c) {
            std::size_t pv = c;
            for (std::size_t r = c + 1; r < k; ++r) if (std::fabs(A[r * k + c]) > std::fabs(A[pv * k + c])) pv = r;
            if (pv != c) {
                for (std::size_t m = 0; m < k; ++m) std::swap(A[c * k + m], A[pv * k + m]);
                std::swap(x[c], x[pv]);
            }
            const double piv = A[c * k + c];
            if (piv == 0.0) continue;
            for (std::size_t r = c + 1; r < k; ++r) {
                const double f = A[r * k + c] / piv;
                if (f == 0.0) continue;
                A[r * k + c] = 0.0;
                for (std::size_t m = c + 1; m < k; ++m) A[r * k + m] -= f * A[c * k + m];
                x[r] -= f * x[c];
            }
        }
        for (std::size_t r = k; r-- > 0;) {
            double v = x[r];
            for (std::size_t m = r + 1; m < k; ++m) v -= A[r * k + m] * x[m];
            v /= A[r * k + r];
            x[r] = std::isfinite(v) ? v : 0.0;
        }
    }

    /// Semismooth Newton on the Schur complement (see solve()). F0: the full
    /// residual at z. On return z holds every node (L recovered).
    bool newton_(std::vector<double>& z, const std::vector<double>& F0) {
        const auto un = static_cast<std::size_t>(n);
        const Part& pt = part_;
        const std::size_t k = pt.Nn.size(), nL = pt.Ln.size(), nb = pt.bnd.size();
        constexpr double k8eps = 8.0 * std::numeric_limits<double>::epsilon();
        auto lc = [&](int t) { return static_cast<std::size_t>(pt.loc[static_cast<std::size_t>(t)]); };
        // This member's L data: F_L at the start (it scales by (1 − λ) along
        // every Newton step: F_L(z) = c·F_L0), w0 = A_LL⁻¹ F_L0, g0 = A_NL w0.
        std::vector<double> FL0(nL), zL0(nL), w0(nL, 0.0), zN0(k), g0(k, 0.0), w0b(nb, 0.0);
        double nFL0 = 0.0, c = 1.0, boundL = 0.0;
        for (std::size_t l = 0; l < nL; ++l) {
            const auto t = static_cast<std::size_t>(pt.Ln[l]);
            FL0[l] = F0[t];
            zL0[l] = z[t];
            nFL0 += std::fabs(FL0[l]);
            const double hs = t < level.size() ? std::fabs(level[t]) : 0.0;
            boundL += diag_slope_[t] * k8eps * (hs + 1.0);
        }
        for (std::size_t i = 0; i < k; ++i) zN0[i] = z[static_cast<std::size_t>(pt.Nn[i])];
        if (nFL0 > 0.0) {
            w0 = FL0;
            lsolve_(w0);
            for (const auto& e : pt.NL) g0[lc(e.r)] += e.v * w0[lc(e.c)];
            for (std::size_t b2 = 0; b2 < nb; ++b2) w0b[b2] = w0[lc(pt.bnd[b2])];
        }
        // z_L at an iterate: z_L0 + A_LL⁻¹[(cc − 1)·F_L0 − A_LN (z_N − z_N0)] (one L solve).
        auto materialize = [&](std::vector<double>& zc, double cc) {
            if (nL == 0) return;
            std::vector<double> x(nL);
            for (std::size_t l = 0; l < nL; ++l) x[l] = (cc - 1.0) * FL0[l];
            for (const auto& e : pt.LN) x[lc(e.r)] -= e.v * (zc[static_cast<std::size_t>(e.c)] - zN0[lc(e.c)]);
            lsolve_(x);
            for (std::size_t l = 0; l < nL; ++l) zc[static_cast<std::size_t>(pt.Ln[l])] = zL0[l] + x[l];
        };
        // Convergence: residualOk_ on the N rows; the L rows are c·F_L0. Where
        // they pass only through the magnitude term, they are evaluated exactly.
        auto converged = [&](const std::vector<double>& FF, const std::vector<double>& M, const std::vector<double>& zc, double cc) {
            for (int t : pt.Nn) if (!rowOk_(static_cast<std::size_t>(t), FF, M, zc)) return false;
            if (cc == 0.0 || nFL0 == 0.0) return true;
            bool lin_ok = true;
            for (std::size_t l = 0; l < nL && lin_ok; ++l)
                lin_ok = std::fabs(cc * FL0[l]) <= 1.0e-10 * std::max(S[static_cast<std::size_t>(pt.Ln[l])], 1.0e-6) / dt;
            if (lin_ok) return true;
            std::vector<double> zf = zc, Ff, Mf;
            materialize(zf, cc);
            residual(zf, Ff, &Mf);
            for (int t : pt.Ln) if (!rowOk_(static_cast<std::size_t>(t), Ff, Mf, zf)) return false;
            return true;
        };
        auto normRed = [&](const std::vector<double>& FF, double cc) {
            double s = 0.0;
            for (int t : pt.Nn) s += std::fabs(FF[static_cast<std::size_t>(t)]);
            return s + cc * nFL0;
        };
        std::vector<double> F = F0, mag(un, 0.0);
        std::vector<double> dz(un, 0.0), zt = z, Ft, Mt;
        std::vector<double> J(k * k), dzN(k), capC(k, 1.0), capP(k, 1.0);
        // Difference step (relative to max(1, |z|)) for the port and store slopes.
        constexpr double kDiff = 1.0e-7;
        // Per port: value and both one-sided slopes in each argument at z.
        struct Slope { double f0 = 0.0, up_p = 0.0, up_m = 0.0, dn_p = 0.0, dn_m = 0.0, ju = 0.0, jd = 0.0; bool kink = false; };
        std::vector<Slope> sl(ports.size());
        // Per node: the store area on the two sides (a store's volume curve is
        // piecewise linear in depth, with a corner at the invert), and the one used.
        struct StoreSlope { double a = 0.0, a_p = 0.0, a_m = 0.0; bool kink = false; };
        std::vector<StoreSlope> st_(un);
        std::vector<double> dz_last(un, 0.0);
        auto zOf = [&](const std::vector<double>& zc, int t) {
            return (t < 0 || pinned[static_cast<std::size_t>(t)]) ? 0.0 : zc[static_cast<std::size_t>(t)];
        };
        // The two sides differ by more than roundoff: a kink within the difference step.
        auto differ = [](double a, double c2) { return std::fabs(a - c2) > 1.0e-6 * (std::fabs(a) + std::fabs(c2)) + 1.0e-300; };
        auto slopes = [&](const Port& p, double zu, double zd, Slope& Sl) {
            Sl.f0 = evalPort(p, zu, zd);
            const bool has_u = p.up >= 0 && !pinned[static_cast<std::size_t>(p.up)];
            const bool has_d = p.dn >= 0 && !pinned[static_cast<std::size_t>(p.dn)];
            auto one = [&](bool has, bool upside, double x, double& sp, double& sm) {
                sp = sm = 0.0;
                if (!has) return;
                const double h = kDiff * std::max(1.0, std::fabs(x));
                auto f = [&](double dx) { return upside ? evalPort(p, zu + dx, zd) : evalPort(p, zu, zd + dx); };
                const double fp = f(h);
                sp = (fp - Sl.f0) / h;
                const double fm = f(-h);
                sm = (Sl.f0 - fm) / h;
                if (!differ(sp, sm)) return;   // smooth here
                // A kink within one difference step of x: the near differences blend
                // the two pieces (Bellinge G72R33Bout1, 2026-11-02: kink 2.5e-8 ft away,
                // forward slope 16.6 against 61.8 and 1.56 on its two sides). The far
                // differences, which skip [x − h, x + h], are the slopes of the two pieces.
                const double sp2 = (f(2.0 * h) - fp) / h, sm2 = (fm - f(-2.0 * h)) / h;
                const bool clean_m = !differ(sm, sm2), clean_p = !differ(sp, sp2);
                if (clean_m && !clean_p) { sp = sm; return; }   // kink ahead: x is on the lower piece
                if (clean_p && !clean_m) { sm = sp; return; }   // kink behind: x is on the upper piece
                sp = sp2;   // on the kink (or several kinks): both pieces, the active set picks
                sm = sm2;
            };
            one(has_u, true, zu, Sl.up_p, Sl.up_m);
            one(has_d, false, zd, Sl.dn_p, Sl.dn_m);
        };
        // The k×k Schur system: the cached plane part plus this member's store
        // areas and port slopes; solved for dz_N, then dz at the boundary L nodes
        // (dz_L = −c·w0 − W dz_N; interior L nodes are recovered at the end).
        auto assemble = [&]() {
            J = pt.Jbase;
            for (std::size_t i = 0; i < k; ++i) {
                const auto t = static_cast<std::size_t>(pt.Nn[i]);
                const double A = std::max(st_[t].a, 1.0e-9);
                J[i * k + i] += A / dt;
                diag_slope_[t] = A / dt + pt.edgeAbs[t];
            }
            for (std::size_t q = 0; q < ports.size(); ++q) {
                const Port& p = ports[q];
                const Slope& Sl = sl[q];
                const bool hu = p.up >= 0 && !pinned[static_cast<std::size_t>(p.up)];
                const bool hd = p.dn >= 0 && !pinned[static_cast<std::size_t>(p.dn)];
                const double ju = Sl.ju, jd = p.dn >= 0 ? Sl.jd : 0.0;
                if (hu) {
                    const std::size_t u = lc(p.up);
                    J[u * k + u] += ju;
                    diag_slope_[static_cast<std::size_t>(p.up)] += std::fabs(ju);
                    if (hd) J[u * k + lc(p.dn)] += jd;
                }
                if (hd) {
                    const std::size_t d = lc(p.dn);
                    J[d * k + d] -= jd;
                    diag_slope_[static_cast<std::size_t>(p.dn)] += std::fabs(jd);
                    if (hu) J[d * k + lc(p.up)] -= ju;
                }
            }
            ++stats.factorisations;
            for (std::size_t i = 0; i < k; ++i) dzN[i] = -F[static_cast<std::size_t>(pt.Nn[i])] + c * g0[i];
            denseSolve_(J, dzN, k);
            for (std::size_t i = 0; i < k; ++i) dz[static_cast<std::size_t>(pt.Nn[i])] = dzN[i];
            for (std::size_t b2 = 0; b2 < nb; ++b2) {
                double v = -c * w0b[b2];
                const double* w = &pt.Wb[b2 * k];
                for (std::size_t j = 0; j < k; ++j) v -= w[j] * dzN[j];
                dz[static_cast<std::size_t>(pt.bnd[b2])] = v;
            }
        };
        // Roundoff floor (atFloor_) on the full network: the N rows and steps
        // here, the L rows as c·F_L0, the L steps (−c·w0 − A_LL⁻¹A_LN dz_N) only
        // when everything else is already at the floor.
        auto atFloorRed = [&](const std::vector<double>& FF, const std::vector<double>& zc, const std::vector<double>& dzc) {
            double r = c * nFL0, bound = boundL;
            for (int ti : pt.Nn) {
                const auto t = static_cast<std::size_t>(ti);
                const double hs = t < level.size() ? std::fabs(level[t]) : 0.0;
                if (!(std::fabs(dzc[t]) <= k8eps * (std::fabs(zc[t]) + hs + 1.0))) return false;
                r += std::fabs(FF[t]);
                bound += diag_slope_[t] * k8eps * (hs + 1.0);
            }
            if (!(r <= bound)) return false;
            if (nL == 0) return true;
            std::vector<double> zf = zc, x(nL, 0.0);
            materialize(zf, c);
            for (const auto& e : pt.LN) x[lc(e.r)] += e.v * dzc[static_cast<std::size_t>(e.c)];
            lsolve_(x);
            for (std::size_t l = 0; l < nL; ++l) {
                const auto t = static_cast<std::size_t>(pt.Ln[l]);
                const double hs = t < level.size() ? std::fabs(level[t]) : 0.0;
                if (!(std::fabs(-c * w0[l] - x[l]) <= k8eps * (std::fabs(zf[t]) + hs + 1.0))) return false;
            }
            return true;
        };
        // Directional linearisation error of port q along dz at step t (cfs).
        auto kinkErr = [&](std::size_t q, double t) {
            const Port& p = ports[q];
            const Slope& Sl = sl[q];
            const double du = zOf(dz, p.up), dd = zOf(dz, p.dn);
            const double g0p = Sl.ju * du + Sl.jd * dd;
            return evalPort(p, zOf(z, p.up) + t * du, zOf(z, p.dn) + t * dd) - Sl.f0 - t * g0p;
        };
        bool ok = false;
        for (int it = 0; it < max_newton && !ok; ++it) {
            ++stats.newton_iters;
            // Semismooth (active-set) Newton on the piecewise-smooth monotone ports
            // (note §3 "Jumps"). A port whose two one-sided slopes differ in an
            // argument sits on a kink: its Jacobian row must be the derivative on
            // the piece the step moves into (the B-derivative in the step's
            // direction), which per-axis sides cannot give for a kink along a
            // line in (z_up, z_dn) (a flow reversing, h_up − max(h_dn, z_end) = δ0,
            // the orifice's Δ = 0). Start from the side of the last move, solve;
            // then take the kinked ports' slopes just inside the piece the solved
            // step enters (forward differences at z + η·dz, η·|dz| = 3 difference
            // steps) and re-solve while any changes. Bellinge G71F320/G71F68Y,
            // 2026-11-02: per-axis sides at such a corner gave a non-descent
            // direction and a period-6 cycle. The Jacobian is a column-dominant
            // M-matrix for every choice (slopes clipped to their monotone sign).
            for (std::size_t q = 0; q < ports.size(); ++q) {
                const Port& p = ports[q];
                Slope& Sl = sl[q];
                slopes(p, zOf(z, p.up), zOf(z, p.dn), Sl);
                const bool ku = differ(Sl.up_p, Sl.up_m), kd = differ(Sl.dn_p, Sl.dn_m);
                Sl.kink = ku || kd;
                const bool back_u = Sl.kink && ku && p.up >= 0 && dz_last[static_cast<std::size_t>(p.up)] < 0.0;
                const bool back_d = Sl.kink && kd && p.dn >= 0 && dz_last[static_cast<std::size_t>(p.dn)] < 0.0;
                Sl.ju = std::max(back_u ? Sl.up_m : Sl.up_p, 0.0);
                Sl.jd = std::min(back_d ? Sl.dn_m : Sl.dn_p, 0.0);
            }
            for (int ti : pt.Nn) {
                const auto t = static_cast<std::size_t>(ti);
                StoreSlope& T = st_[t];
                volume(ti, z[t], &T.a);
                T.kink = false;
                if (store_of[t] < 0) continue;
                const double h = kDiff * std::max(1.0, std::fabs(z[t]));
                const double v0 = volume(ti, z[t], nullptr), vp = volume(ti, z[t] + h, nullptr), vm = volume(ti, z[t] - h, nullptr);
                T.a_p = (vp - v0) / h;
                T.a_m = (v0 - vm) / h;
                if (!differ(T.a_p, T.a_m)) { T.a = T.a_p; continue; }
                const double ap2 = (volume(ti, z[t] + 2.0 * h, nullptr) - vp) / h, am2 = (vm - volume(ti, z[t] - 2.0 * h, nullptr)) / h;
                const bool clean_m = !differ(T.a_m, am2), clean_p = !differ(T.a_p, ap2);
                if (clean_m && !clean_p) { T.a = T.a_m; continue; }   // corner ahead
                if (clean_p && !clean_m) { T.a = T.a_p; continue; }   // corner behind
                T.a_p = ap2; T.a_m = am2; T.kink = true;
                T.a = dz_last[t] < 0.0 ? T.a_m : T.a_p;
            }
            for (int pass = 0; pass < 4; ++pass) {
                assemble();
                if (pass == 3) break;
                bool changed = false;
                for (std::size_t q = 0; q < ports.size(); ++q) {
                    Slope& Sl = sl[q];
                    const Port& p = ports[q];
                    const bool has_u = p.up >= 0 && !pinned[static_cast<std::size_t>(p.up)];
                    const bool has_d = p.dn >= 0 && !pinned[static_cast<std::size_t>(p.dn)];
                    const double zu = zOf(z, p.up), zd = zOf(z, p.dn), du = zOf(dz, p.up), dd = zOf(dz, p.dn);
                    const double hu = kDiff * std::max(1.0, std::fabs(zu)), hd = kDiff * std::max(1.0, std::fabs(zd));
                    const double m = std::max(std::fabs(du) / hu, std::fabs(dd) / hd);
                    if (!(m > 0.0)) continue;
                    if (!Sl.kink) continue;
                    const double eta = std::min(1.0, 3.0 / m);
                    const double pu = zu + eta * du, pd = zd + eta * dd, f0 = evalPort(p, pu, pd);
                    const double ju = has_u ? std::max((evalPort(p, pu + hu, pd) - f0) / hu, 0.0) : 0.0;
                    const double jd = has_d ? std::min((evalPort(p, pu, pd + hd) - f0) / hd, 0.0) : 0.0;
                    if (differ(ju, Sl.ju) || differ(jd, Sl.jd)) { Sl.ju = ju; Sl.jd = jd; changed = true; }
                }
                for (int ti : pt.Nn) {
                    const auto t = static_cast<std::size_t>(ti);
                    StoreSlope& T = st_[t];
                    if (!T.kink || dz[t] == 0.0) continue;
                    const double a = dz[t] > 0.0 ? T.a_p : T.a_m;
                    if (a != T.a) { T.a = a; changed = true; }
                }
                if (!changed) break;
                ++stats.side_switches;
            }
            // Roundoff floor: no step the arithmetic can resolve is left (see atFloor_).
            if (atFloorRed(F, z, dz)) {
                ++stats.fp_floor_converged;
                if (trace_) { TraceRec tr; tr.r0 = tr.r1 = normRed(F, c); tr.flags = 4; trace_->push_back(tr); }
                ok = true;
                break;
            }
            // Armijo on ‖F‖₁ from the full step. If the full step fails, the
            // step is bounded by the first strong kink along it instead of being
            // halved blindly: a port whose law bends where the step crosses it
            // (dry ramp, brake plateau, raised end, a flow reversing into an
            // empty store) caps the step at twice the kink's distance (cross it
            // by no more than its distance), then at the kink itself, then
            // halving. Next iteration the iterate sits on the new piece and the
            // active-set slopes take over. Without this, Newton jumped across an
            // S-shaped corner both ways (Bellinge G72R33Bb, G71F06R_ovl).
            const double r0 = normRed(F, c);
            std::vector<double> lams;
            auto trial = [&](double lam) {
                for (int t : pt.Nn) zt[static_cast<std::size_t>(t)] = z[static_cast<std::size_t>(t)] + lam * dz[static_cast<std::size_t>(t)];
                for (int t : pt.bnd) zt[static_cast<std::size_t>(t)] = z[static_cast<std::size_t>(t)] + lam * dz[static_cast<std::size_t>(t)];
                residualN_(zt, Ft, Mt);
                return normRed(Ft, c * (1.0 - lam));
            };
            // A step bounded per node (O-6): N moves by cap ⊙ dz_N, the boundary
            // L nodes by the exact linear response −c·w0 − W·(cap ⊙ dz_N), and the
            // L residual is then zero.
            auto trialCap = [&](const std::vector<double>& cap) {
                for (std::size_t i = 0; i < k; ++i) {
                    const auto t = static_cast<std::size_t>(pt.Nn[i]);
                    zt[t] = z[t] + cap[i] * dz[t];
                }
                for (std::size_t b2 = 0; b2 < nb; ++b2) {
                    double v = -c * w0b[b2];
                    const double* w = &pt.Wb[b2 * k];
                    for (std::size_t j = 0; j < k; ++j) v -= w[j] * cap[j] * dzN[j];
                    const auto t = static_cast<std::size_t>(pt.bnd[b2]);
                    zt[t] = z[t] + v;
                }
                residualN_(zt, Ft, Mt);
                return normRed(Ft, 0.0);
            };
            const std::vector<double>* capped = nullptr;
            bool kb = false;
            double lam = 1.0, r = trial(1.0), best = r, best_lam = 1.0;
            bool acc = r <= (1.0 - 1.0e-4) * r0 || converged(Ft, Mt, zt, 0.0);
            if (!acc) {
                // Kink parameter, per port whose linearisation misses by >= 1 % of the
                // full step's residual (e = the port's linearisation error along the
                // step). Bisect for the first t with |e(t)| > ½·|e(1)|·t: a kink at t_k
                // between linear pieces (e = Δs·(t − t_k)⁺) gives t_c = 2t_k/(1 + t_k)
                // ∈ [t_k, 2t_k], a step that crosses it by at most its distance; a
                // smooth (quadratic) law gives t_c = ½, and e(t_c/(2 − t_c)) ≈ 0.44 e(t_c)
                // where a kink gives 0: only kinks bound the step. The kink itself is
                // then located to 1e-6 of the step and the second candidate lands just
                // past it, so the next iteration linearises on the new piece.
                double tc = 1.0, tp = 1.0;
                // err(t): one law's linearisation error at step t (cfs); move(t): whether the
                // step to t moves its arguments by more than one difference step.
                double kc = 1.0, kp = 1.0;   // this law's kink parameters (1: none)
                auto bound = [&](const std::function<double(double)>& err, const std::function<bool(double)>& moves) {
                    kc = kp = 1.0;
                    const double e1 = std::fabs(err(1.0));
                    if (!(e1 >= 1.0e-2 * std::min(r, r0))) return;
                    double lo = 0.0, hi = 1.0;
                    for (int bi = 0; bi < 40 && hi > 1.0e-14; ++bi) {
                        const double m = 0.5 * (lo + hi);
                        (std::fabs(err(m)) > 0.5 * e1 * m ? hi : lo) = m;
                    }
                    if (!(hi < 1.0) || std::fabs(err(hi / (2.0 - hi))) > 0.1 * std::fabs(err(hi))) return;   // smooth
                    const double cc = hi;
                    lo = 0.0;
                    for (int bi = 0; bi < 40 && hi - lo > 1.0e-6 * cc; ++bi) {
                        const double m = 0.5 * (lo + hi);
                        (std::fabs(err(m)) > 1.0e-6 * e1 ? hi : lo) = m;
                    }
                    // The departure point found above is the kink only when the law is
                    // straight up to it. A law that curves before its kink (the orifice's
                    // square root below the street-level crossing, Bellinge G71F044,
                    // 2026-11-04) departs from its tangent long before the kink: "just
                    // past the kink" then landed at t ≈ 0.003–0.02 of a step whose kink
                    // sits near 0.27, and Newton crawled 20 iterations. The crossing
                    // parameter places a kink between straight pieces exactly at
                    // t_k = t_c/(2 − t_c); the later of the two is the kink estimate.
                    hi = std::max(hi, cc / (2.0 - cc));

                    // A kink within one difference step of the iterate is the slope
                    // classification's (it is "at" the iterate); bounding by it would stall
                    // the step at roundoff (Bellinge G71F100, 2026-11-02: kink 7e-15 ft away).
                    if (!moves(hi)) return;
                    tc = std::min(tc, cc);
                    tp = std::min(tp, hi);
                    kc = cc;
                    kp = hi;
                };
                // Per node: the kink parameters of its own laws (O-6). capC crosses
                // each node's first kink by at most its distance, capP lands just past it.
                std::fill(capC.begin(), capC.end(), 1.0);
                std::fill(capP.begin(), capP.end(), 1.0);
                auto capAt = [&](int t, double cc, double cp) {
                    if (t < 0 || pt.cls[static_cast<std::size_t>(t)] != 1) return;
                    const std::size_t i = lc(t);
                    capC[i] = std::min(capC[i], cc);
                    capP[i] = std::min(capP[i], cp);
                };
                auto far = [](double d, double x) { return std::fabs(d) >= kDiff * std::max(1.0, std::fabs(x)); };
                {
                    for (std::size_t q = 0; q < ports.size(); ++q) {
                        const Port& p = ports[q];
                        bound([&](double t) { return kinkErr(q, t); }, [&](double t) {
                            return far(t * zOf(dz, p.up), zOf(z, p.up)) || far(t * zOf(dz, p.dn), zOf(z, p.dn));
                        });
                        if (kc < 1.0) capAt(p.up, kc, kp), capAt(p.dn, kc, kp);
                    }
                    for (int ti : pt.Nn) {
                        const auto t = static_cast<std::size_t>(ti);
                        if (store_of[t] < 0 || dz[t] == 0.0) continue;
                        const double v0 = volume(ti, z[t], nullptr);
                        bound([&](double s) { return (volume(ti, z[t] + s * dz[t], nullptr) - v0 - s * st_[t].a * dz[t]) / dt; },
                              [&](double s) { return far(s * dz[t], z[t]); });
                        if (kc < 1.0) capAt(ti, kc, kp);
                    }
                }
                if (tc < 1.0) {
                    ++stats.kink_bounded;
                    kb = true;
                    // O-6: bound each node by its own laws' kinks first. The global
                    // bound stops every node at the nearest kink anywhere, so a step
                    // that crosses m independent corners takes m iterations (Bellinge
                    // storm peak, 2026-11-04: ~20 coupled manholes, each a dry ramp
                    // 0.01 ft from its member level, one crossed per iteration, 20
                    // iterations at λ ≈ 1e-5..1e-3). The L block takes the exact
                    // response to the bounded N step (its rows are linear).
                    const double mC = *std::min_element(capC.begin(), capC.end());
                    const double mP = *std::min_element(capP.begin(), capP.end());
                    if (trialCap(capC) <= (1.0 - 1.0e-4 * mC) * r0 || converged(Ft, Mt, zt, 0.0)) { acc = true; capped = &capC; }
                    else if (trialCap(capP) <= (1.0 - 1.0e-4 * mP) * r0 || converged(Ft, Mt, zt, 0.0)) { acc = true; capped = &capP; }
                    if (!acc) {
                        lams.push_back(tc);
                        if (tp < tc) lams.push_back(tp);
                        for (double l = 0.5 * tp; lams.size() < 14; l *= 0.5) lams.push_back(l);
                    }
                } else {
                    for (double l = 0.5; lams.size() < 11; l *= 0.5) lams.push_back(l);
                }
                for (double l : lams) {
                    if (acc) break;
                    r = trial(l);
                    if (r <= (1.0 - 1.0e-4 * l) * r0 || converged(Ft, Mt, zt, c * (1.0 - l))) { acc = true; lam = l; break; }
                    if (r < best) { best = r; best_lam = l; }
                }
                if (!acc) {
                    ++stats.armijo_failures;
                    // No step decreases ‖F‖₁: take the best one, or (none better) step
                    // every kinked node just past its own kink, which moves each to its
                    // next piece. Never the blind full step: taken anyway, it fed a
                    // period-6 cycle through residuals 4 -> 4e4 (Bellinge
                    // G71F320/G71F68Y, 2026-11-02).
                    if (best < r0 || !(tc < 1.0)) { lam = best_lam; trial(lam); }
                    else { trialCap(capP); capped = &capP; }
                }
            }
            if (capped) {
                ++stats.node_bounded;
                lam = 0.0;   // the L residual is solved exactly by the bounded step
                for (std::size_t i = 0; i < k; ++i) dz[static_cast<std::size_t>(pt.Nn[i])] *= (*capped)[i];
            }
            for (int t : pt.Nn) dz_last[static_cast<std::size_t>(t)] = (capped ? 1.0 : lam) * dz[static_cast<std::size_t>(t)];
            if (trace_) {
                TraceRec tr;
                tr.r0 = r0; tr.r1 = normRed(Ft, capped ? 0.0 : c * (1.0 - lam)); tr.lam = capped ? -1.0 : lam;
                tr.flags = (kb ? 1 : 0) | (acc ? 0 : 2) | (capped ? 8 : 0);
                for (int t : pt.Nn) {
                    const auto ut = static_cast<std::size_t>(t);
                    if (std::fabs(Ft[ut]) > std::fabs(tr.worst_F)) { tr.worst = t; tr.worst_F = Ft[ut]; }
                    if (std::fabs(dz[ut]) > std::fabs(tr.big_dz)) { tr.big = t; tr.big_dz = dz[ut]; }
                }
                trace_->push_back(tr);
            }
            z.swap(zt); F.swap(Ft); mag.swap(Mt);
            c = capped ? 0.0 : c * (1.0 - lam);
            ok = converged(F, mag, z, c);
        }
        materialize(z, c);
        return ok;
    }

private:
    mutable std::vector<double> diag_slope_;   ///< |dF_t/dz_t| at the last assembled Jacobian

    /// The residual tolerance (cfs per node), shared by Newton and the Jacobi
    /// fallback: 1e-10·A/dt plus 1e-12 of the magnitude of the node's own terms.
    bool residualOk_(const std::vector<double>& FF, const std::vector<double>& M, const std::vector<double>& zc) const {
        for (std::size_t t = 0; t < static_cast<std::size_t>(n); ++t) {
            double a = 0.0;
            volume(static_cast<int>(t), zc[t], &a);
            const double tol = 1.0e-10 * std::max(a, 1.0e-6) / dt + 1.0e-12 * M[t];
            if (!(std::fabs(FF[t]) <= tol)) return false;
        }
        return true;
    }

    /// Roundoff floor (an engineering floor of double arithmetic, not a looser
    /// tolerance). The unknown is the deviation z about the main run's level
    /// h*, but the physics evaluates the absolute level h* + z, which carries
    /// 8·eps·(|z| + |h*| + 1 ft) of resolution. Converged when every proposed
    /// step is within that resolution AND the residual is within what that
    /// resolution can produce, Σ|slope_t|·8·eps·(|h*_t| + 1), the node's slope
    /// being the diagonal of the Jacobian. Past it a step moves no bit of the
    /// level the ports see (Bellinge, 2026-11-02: 462 of 474 fallbacks were
    /// steps <= 1e-12 ft at tiny nodes with ‖F‖ < 1e-6 cfs).
    bool atFloor_(const std::vector<double>& FF, const std::vector<double>& zc, const std::vector<double>& dzc) const {
        constexpr double k8eps = 8.0 * std::numeric_limits<double>::epsilon();
        if (diag_slope_.size() != static_cast<std::size_t>(n)) return false;
        double r = 0.0, bound = 0.0;
        for (std::size_t t = 0; t < static_cast<std::size_t>(n); ++t) {
            if (pinned[t]) continue;
            const double hs = t < level.size() ? std::fabs(level[t]) : 0.0;
            if (!(std::fabs(dzc[t]) <= k8eps * (std::fabs(zc[t]) + hs + 1.0))) return false;
            r += std::fabs(FF[t]);
            bound += diag_slope_[t] * k8eps * (hs + 1.0);
        }
        return r <= bound;
    }

    // Monotone nonlinear Jacobi: each node solves its own equation with the
    // others frozen, simultaneous update.
    bool jacobi_(std::vector<double>& z) {
        const auto un = static_cast<std::size_t>(n);
        std::vector<std::vector<int>> inc_e(un), inc_p(un);
        for (std::size_t k = 0; k < edges.size(); ++k) {
            if (edges[k].up >= 0) inc_e[static_cast<std::size_t>(edges[k].up)].push_back(static_cast<int>(k));
            if (edges[k].dn >= 0) inc_e[static_cast<std::size_t>(edges[k].dn)].push_back(static_cast<int>(k));
        }
        for (std::size_t k = 0; k < ports.size(); ++k) {
            if (ports[k].up >= 0) inc_p[static_cast<std::size_t>(ports[k].up)].push_back(static_cast<int>(k));
            if (ports[k].dn >= 0) inc_p[static_cast<std::size_t>(ports[k].dn)].push_back(static_cast<int>(k));
        }
        std::vector<double> znew(un), dzj(un), F, mag;
        for (int sw = 0; sw < max_jacobi; ++sw) {
            for (std::size_t t = 0; t < un; ++t) {
                if (pinned[t]) { znew[t] = 0.0; continue; }
                const int ti = static_cast<int>(t);
                auto zz = [&](int s, double x) {
                    if (s < 0 || pinned[static_cast<std::size_t>(s)]) return 0.0;
                    return s == ti ? x : z[static_cast<std::size_t>(s)];
                };
                auto g = [&](double x) {   // nondecreasing in x
                    double r = (volume(ti, x, nullptr) - volumePrev(ti)) / dt - b[t];
                    for (int k : inc_e[t]) {
                        const auto& e = edges[static_cast<std::size_t>(k)];
                        const double f = e.conduit ? e.ju * zz(e.up, x) + (e.dn >= 0 ? e.jd * zz(e.dn, x) : 0.0) : e.d * zz(e.up, x);
                        r += (e.up == ti ? f : 0.0) - (e.dn == ti ? f : 0.0);
                    }
                    for (int k : inc_p[t]) {
                        const auto& p = ports[static_cast<std::size_t>(k)];
                        const double f = evalPort(p, zz(p.up, x), zz(p.dn, x));
                        r += (p.up == ti ? f : 0.0) - (p.dn == ti ? f : 0.0);
                    }
                    return r;
                };
                double lo = z[t] - 1.0, hi = z[t] + 1.0;
                for (int k = 0; k < 60 && g(lo) > 0.0; ++k) lo -= 2.0 * (hi - lo);
                for (int k = 0; k < 60 && g(hi) < 0.0; ++k) hi += 2.0 * (hi - lo);
                for (int k = 0; k < 100; ++k) {
                    const double mid = 0.5 * (lo + hi);
                    (g(mid) > 0.0 ? hi : lo) = mid;
                }
                znew[t] = 0.5 * (lo + hi);
            }
            for (std::size_t t = 0; t < un; ++t) dzj[t] = znew[t] - z[t];
            z = znew;
            residual(z, F, &mag);
            // The same criterion as Newton's: the residual tolerance, or the roundoff floor.
            if (residualOk_(F, mag, z)) return true;
            if (atFloor_(F, z, dzj)) { ++stats.fp_floor_converged; return true; }
            continue;
        }
        return false;
    }
};

}  // namespace openswmm::uncertainty

#endif  // OPENSWMM_ENGINE_UNCERTAINTY_ROM_PORT_GRAPH_HPP
