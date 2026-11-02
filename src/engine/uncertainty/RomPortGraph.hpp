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
 * column-dominant M-matrix, so the AdditiveSystem LU (minimum-degree order, no
 * pivoting) factors it as it is; its symbolic pattern is shared with the planes.
 *
 * Solve: Newton (forward-difference port slopes = the one-sided slope at a
 * kink), Armijo on ‖F‖₁; fallback monotone nonlinear Jacobi, counted. F == 0
 * exactly at the start (zero data) returns without iterating.
 */
#ifndef OPENSWMM_ENGINE_UNCERTAINTY_ROM_PORT_GRAPH_HPP
#define OPENSWMM_ENGINE_UNCERTAINTY_ROM_PORT_GRAPH_HPP

#include "RomAdditiveChannel.hpp"
#include "RomFlowResponse.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
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
    struct Stats { long solves = 0, newton_iters = 0, armijo_failures = 0, jacobi_fallbacks = 0, jacobi_failed = 0, zero_exits = 0; };
    Stats stats;

    void resize(int n_nodes) {
        n = n_nodes;
        const auto un = static_cast<std::size_t>(std::max(n, 0));
        S.assign(un, 1.0); pinned.assign(un, 0); store_of.assign(un, -1);
        b.assign(un, 0.0); z_prev.assign(un, 0.0);
        stores.clear(); edges.clear(); ports.clear(); dV_prev.clear();
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
            const double f = p.dq(member, zz(p.up), zz(p.dn));
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
        for (const auto& p : ports) s += out(p.up, p.dn) * p.dq(member, zz(p.up), zz(p.dn));
        return s;
    }

    /// Solve F(z) = 0 for the current member. z: in = start (normally z_prev), out = solution.
    /// Returns true when converged (Newton, or the Jacobi fallback).
    bool solve(std::vector<double>& z, AdditiveSystem& sys) {
        ++stats.solves;
        const auto un = static_cast<std::size_t>(n);
        for (std::size_t t = 0; t < un; ++t) if (pinned[t]) z[t] = 0.0;
        std::vector<double> F, mag;
        residual(z, F, &mag);
        bool all_zero = true;
        for (double v : F) if (v != 0.0) { all_zero = false; break; }
        if (all_zero) { ++stats.zero_exits; return true; }
        auto converged = [&](const std::vector<double>& FF, const std::vector<double>& M, const std::vector<double>& zc) {
            for (std::size_t t = 0; t < un; ++t) {
                double a = 0.0;
                volume(static_cast<int>(t), zc[t], &a);
                const double tol = 1.0e-10 * std::max(a, 1.0e-6) / dt + 1.0e-12 * M[t];
                if (!(std::fabs(FF[t]) <= tol)) return false;
            }
            return true;
        };
        auto norm1 = [](const std::vector<double>& v) { double s = 0.0; for (double x : v) s += std::fabs(x); return s; };
        std::vector<FlowEdge> jac;
        std::vector<double> A(un), dq(un), dz(un), zt(un), Ft, Mt;
        bool ok = converged(F, mag, z);
        for (int it = 0; it < max_newton && !ok; ++it) {
            ++stats.newton_iters;
            // Jacobian: store areas, plane edges, port slopes (forward difference = one-sided slope).
            jac = edges;
            for (const auto& p : ports) {
                const double zu = (p.up >= 0 && !pinned[static_cast<std::size_t>(p.up)]) ? z[static_cast<std::size_t>(p.up)] : 0.0;
                const double zd = (p.dn >= 0 && !pinned[static_cast<std::size_t>(p.dn)]) ? z[static_cast<std::size_t>(p.dn)] : 0.0;
                const double f0 = p.dq(member, zu, zd);
                const double hu = 1.0e-7 * std::max(1.0, std::fabs(zu)), hd = 1.0e-7 * std::max(1.0, std::fabs(zd));
                FlowEdge e;
                e.up = p.up; e.dn = p.dn; e.conduit = true;
                e.ju = std::max((p.dq(member, zu + hu, zd) - f0) / hu, 0.0);
                e.jd = p.dn >= 0 ? std::min((p.dq(member, zu, zd + hd) - f0) / hd, 0.0) : 0.0;
                if (e.up < 0) {   // an inflow port from outside: only its dn slope matters
                    e.up = p.dn; e.dn = -1; e.ju = -e.jd; e.jd = 0.0;
                    if (e.up < 0) continue;
                }
                jac.push_back(e);
            }
            for (std::size_t t = 0; t < un; ++t) { volume(static_cast<int>(t), z[t], &A[t]); A[t] = std::max(A[t], 1.0e-9); }
            sys.build(n, jac, A.data(), dt, &pinned);
            for (std::size_t t = 0; t < un; ++t) { dq[t] = -F[t]; dz[t] = 0.0; }
            sys.step(dq.data(), dz.data());
            // Armijo on ‖F‖₁. At a kink (dry ramp, table point, raised end) the
            // one-sided slope can make no step decrease ‖F‖₁; the best trial is
            // then taken anyway (the full step if none is better) and Newton
            // goes on: a monotone piecewise-smooth system usually settles on the
            // right piece within a few such steps. Only Newton that has not
            // converged after max_newton iterations hands over to Jacobi.
            const double r0 = norm1(F);
            double lam = 1.0, best = 0.0, best_lam = 1.0;
            bool acc = false;
            for (int ls = 0; ls < 12; ++ls, lam *= 0.5) {
                for (std::size_t t = 0; t < un; ++t) zt[t] = z[t] + lam * dz[t];
                residual(zt, Ft, &Mt);
                const double r = norm1(Ft);
                if (r <= (1.0 - 1.0e-4 * lam) * r0 || converged(Ft, Mt, zt)) { acc = true; break; }
                if (ls == 0 || r < best) { best = r; best_lam = lam; }
            }
            if (!acc) {
                ++stats.armijo_failures;
                const double l = best < r0 ? best_lam : 1.0;
                for (std::size_t t = 0; t < un; ++t) zt[t] = z[t] + l * dz[t];
                residual(zt, Ft, &Mt);
            }
            z.swap(zt); F.swap(Ft); mag.swap(Mt);
            ok = converged(F, mag, z);
        }
        if (!ok) {
            ++stats.jacobi_fallbacks;
            ok = jacobi_(z);
            if (!ok) ++stats.jacobi_failed;
        }
        if (floor_projection)
            for (std::size_t t = 0; t < un && t < floor.size(); ++t) z[t] = std::max(z[t], floor[t]);
        return ok;
    }

private:
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
        std::vector<double> znew(un), F, mag;
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
                        const double f = p.dq(member, zz(p.up, x), zz(p.dn, x));
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
            z = znew;
            residual(z, F, &mag);
            bool ok = true;
            for (std::size_t t = 0; t < un && ok; ++t) {
                double a = 0.0;
                volume(static_cast<int>(t), z[t], &a);
                ok = std::fabs(F[t]) <= 1.0e-8 * std::max(a, 1.0e-6) / dt + 1.0e-12 * mag[t];
            }
            if (ok) return true;
        }
        return false;
    }
};

}  // namespace openswmm::uncertainty

#endif  // OPENSWMM_ENGINE_UNCERTAINTY_ROM_PORT_GRAPH_HPP
