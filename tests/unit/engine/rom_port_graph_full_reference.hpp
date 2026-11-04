/**
 * @file rom_port_graph_full_reference.hpp
 * @brief TEST REFERENCE ONLY: the per-member port-graph solve as it was before
 *        PORT_HAMILTONIAN O-5 (RomPortGraph.hpp at a83777ce), which factors the
 *        whole active network on every Newton iteration. Kept verbatim (renamed)
 *        so test_rom_member_ports.cpp can check that the Schur-complement solve
 *        reaches the same solution. Not compiled into the engine.
 */
#ifndef OPENSWMM_TEST_ROM_PORT_GRAPH_FULL_REFERENCE_HPP
#define OPENSWMM_TEST_ROM_PORT_GRAPH_FULL_REFERENCE_HPP

#include "uncertainty/RomAdditiveChannel.hpp"
#include "uncertainty/RomFlowResponse.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <vector>

namespace openswmm::uncertainty::test_reference {

struct FullNetworkPortGraph {
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
    struct Stats { long solves = 0, newton_iters = 0, armijo_failures = 0, jacobi_fallbacks = 0, jacobi_failed = 0, zero_exits = 0, port_evals = 0, factorisations = 0, side_switches = 0, kink_bounded = 0, fp_floor_converged = 0; };
    mutable Stats stats;   ///< mutable: residual() (const) counts port evaluations

    void resize(int n_nodes) {
        n = n_nodes;
        const auto un = static_cast<std::size_t>(std::max(n, 0));
        S.assign(un, 1.0); pinned.assign(un, 0); store_of.assign(un, -1);
        b.assign(un, 0.0); z_prev.assign(un, 0.0); level.assign(un, 0.0);
        stores.clear(); edges.clear(); ports.clear(); dV_prev.clear();
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
            return residualOk_(FF, M, zc);
        };
        auto norm1 = [](const std::vector<double>& v) { double s = 0.0; for (double x : v) s += std::fabs(x); return s; };
        std::vector<FlowEdge> jac;
        std::vector<double> A(un), dq(un), dz(un), zt(un), Ft, Mt;
        bool ok = converged(F, mag, z);
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
        auto differ = [](double a, double c) { return std::fabs(a - c) > 1.0e-6 * (std::fabs(a) + std::fabs(c)) + 1.0e-300; };
        auto slopes = [&](const Port& p, double zu, double zd, Slope& S) {
            S.f0 = evalPort(p, zu, zd);
            const bool has_u = p.up >= 0 && !pinned[static_cast<std::size_t>(p.up)];
            const bool has_d = p.dn >= 0 && !pinned[static_cast<std::size_t>(p.dn)];
            auto one = [&](bool has, bool upside, double x, double& sp, double& sm) {
                sp = sm = 0.0;
                if (!has) return;
                const double h = kDiff * std::max(1.0, std::fabs(x));
                auto f = [&](double dx) { return upside ? evalPort(p, zu + dx, zd) : evalPort(p, zu, zd + dx); };
                const double fp = f(h);
                sp = (fp - S.f0) / h;
                const double fm = f(-h);
                sm = (S.f0 - fm) / h;
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
            one(has_u, true, zu, S.up_p, S.up_m);
            one(has_d, false, zd, S.dn_p, S.dn_m);
        };
        auto assemble = [&]() {
            jac = edges;
            for (std::size_t k = 0; k < ports.size(); ++k) {
                const Port& p = ports[k];
                const Slope& S = sl[k];
                FlowEdge e;
                e.up = p.up; e.dn = p.dn; e.conduit = true;
                e.ju = S.ju;
                e.jd = p.dn >= 0 ? S.jd : 0.0;
                if (e.up < 0) {   // an inflow port from outside: only its dn slope matters
                    e.up = p.dn; e.dn = -1; e.ju = -e.jd; e.jd = 0.0;
                    if (e.up < 0) continue;
                }
                jac.push_back(e);
            }
            for (std::size_t t = 0; t < un; ++t) A[t] = std::max(st_[t].a, 1.0e-9);
            sys.build(n, jac, A.data(), dt, &pinned);
            diag_slope_.assign(un, 0.0);
            for (std::size_t t = 0; t < un; ++t) diag_slope_[t] = A[t] / dt;
            for (const auto& e : jac) {
                if (e.up >= 0) diag_slope_[static_cast<std::size_t>(e.up)] += std::fabs(e.conduit ? e.ju : e.d);
                if (e.dn >= 0 && e.conduit) diag_slope_[static_cast<std::size_t>(e.dn)] += std::fabs(e.jd);
            }
            ++stats.factorisations;
            for (std::size_t t = 0; t < un; ++t) { dq[t] = -F[t]; dz[t] = 0.0; }
            sys.step(dq.data(), dz.data());
        };
        // Directional linearisation error of port k along dz at step t (cfs).
        auto kinkErr = [&](std::size_t k, double t) {
            const Port& p = ports[k];
            const Slope& S = sl[k];
            const double du = zOf(dz, p.up), dd = zOf(dz, p.dn);
            const double g0 = S.ju * du + S.jd * dd;
            return evalPort(p, zOf(z, p.up) + t * du, zOf(z, p.dn) + t * dd) - S.f0 - t * g0;
        };
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
            for (std::size_t k = 0; k < ports.size(); ++k) {
                const Port& p = ports[k];
                Slope& S = sl[k];
                slopes(p, zOf(z, p.up), zOf(z, p.dn), S);
                const bool ku = differ(S.up_p, S.up_m), kd = differ(S.dn_p, S.dn_m);
                S.kink = ku || kd;
                const bool back_u = S.kink && ku && p.up >= 0 && dz_last[static_cast<std::size_t>(p.up)] < 0.0;
                const bool back_d = S.kink && kd && p.dn >= 0 && dz_last[static_cast<std::size_t>(p.dn)] < 0.0;
                S.ju = std::max(back_u ? S.up_m : S.up_p, 0.0);
                S.jd = std::min(back_d ? S.dn_m : S.dn_p, 0.0);
            }
            for (std::size_t t = 0; t < un; ++t) {
                StoreSlope& T = st_[t];
                const int ti = static_cast<int>(t);
                volume(ti, z[t], &T.a);
                T.kink = false;
                if (store_of[t] < 0 || pinned[t]) continue;
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
                for (std::size_t k = 0; k < ports.size(); ++k) {
                    Slope& S = sl[k];
                    const Port& p = ports[k];
                    const bool has_u = p.up >= 0 && !pinned[static_cast<std::size_t>(p.up)];
                    const bool has_d = p.dn >= 0 && !pinned[static_cast<std::size_t>(p.dn)];
                    const double zu = zOf(z, p.up), zd = zOf(z, p.dn), du = zOf(dz, p.up), dd = zOf(dz, p.dn);
                    const double hu = kDiff * std::max(1.0, std::fabs(zu)), hd = kDiff * std::max(1.0, std::fabs(zd));
                    const double m = std::max(std::fabs(du) / hu, std::fabs(dd) / hd);
                    if (!(m > 0.0)) continue;
                    if (!S.kink) continue;
                    const double eta = std::min(1.0, 3.0 / m);
                    const double pu = zu + eta * du, pd = zd + eta * dd, f0 = evalPort(p, pu, pd);
                    const double ju = has_u ? std::max((evalPort(p, pu + hu, pd) - f0) / hu, 0.0) : 0.0;
                    const double jd = has_d ? std::min((evalPort(p, pu, pd + hd) - f0) / hd, 0.0) : 0.0;
                    if (differ(ju, S.ju) || differ(jd, S.jd)) { S.ju = ju; S.jd = jd; changed = true; }
                }
                for (std::size_t t = 0; t < un; ++t) {
                    StoreSlope& T = st_[t];
                    if (!T.kink || dz[t] == 0.0) continue;
                    const double a = dz[t] > 0.0 ? T.a_p : T.a_m;
                    if (a != T.a) { T.a = a; changed = true; }
                }
                if (!changed) break;
                ++stats.side_switches;
            }
            // Roundoff floor: no step the arithmetic can resolve is left (see atFloor_).
            if (atFloor_(F, z, dz)) { ++stats.fp_floor_converged; ok = true; break; }
            // Armijo on ‖F‖₁ from the full step. If the full step fails, the
            // step is bounded by the first strong kink along it instead of being
            // halved blindly: a port whose law bends where the step crosses it
            // (dry ramp, brake plateau, raised end, a flow reversing into an
            // empty store) caps the step at twice the kink's distance (cross it
            // by no more than its distance), then at the kink itself, then
            // halving. Next iteration the iterate sits on the new piece and the
            // active-set slopes take over. Without this, Newton jumped across an
            // S-shaped corner both ways (Bellinge G72R33Bb, G71F06R_ovl).
            const double r0 = norm1(F);
            std::vector<double> lams;
            auto trial = [&](double lam) {
                for (std::size_t t = 0; t < un; ++t) zt[t] = z[t] + lam * dz[t];
                residual(zt, Ft, &Mt);
                return norm1(Ft);
            };
            double lam = 1.0, r = trial(1.0), best = r, best_lam = 1.0;
            bool acc = r <= (1.0 - 1.0e-4) * r0 || converged(Ft, Mt, zt);
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
                auto bound = [&](const std::function<double(double)>& err, const std::function<bool(double)>& moves) {
                    const double e1 = std::fabs(err(1.0));
                    if (!(e1 >= 1.0e-2 * std::min(r, r0))) return;
                    double lo = 0.0, hi = 1.0;
                    for (int bi = 0; bi < 40 && hi > 1.0e-14; ++bi) {
                        const double m = 0.5 * (lo + hi);
                        (std::fabs(err(m)) > 0.5 * e1 * m ? hi : lo) = m;
                    }
                    if (!(hi < 1.0) || std::fabs(err(hi / (2.0 - hi))) > 0.1 * std::fabs(err(hi))) return;   // smooth
                    const double c = hi;
                    lo = 0.0;
                    for (int bi = 0; bi < 40 && hi - lo > 1.0e-6 * c; ++bi) {
                        const double m = 0.5 * (lo + hi);
                        (std::fabs(err(m)) > 1.0e-6 * e1 ? hi : lo) = m;
                    }
                    // A kink within one difference step of the iterate is the slope
                    // classification's (it is "at" the iterate); bounding by it would stall
                    // the step at roundoff (Bellinge G71F100, 2026-11-02: kink 7e-15 ft away).
                    if (!moves(hi)) return;
                    tc = std::min(tc, c);
                    tp = std::min(tp, hi);
                };
                auto far = [](double d, double x) { return std::fabs(d) >= kDiff * std::max(1.0, std::fabs(x)); };
                {
                    for (std::size_t k = 0; k < ports.size(); ++k) {
                        const Port& p = ports[k];
                        bound([&](double t) { return kinkErr(k, t); }, [&](double t) {
                            return far(t * zOf(dz, p.up), zOf(z, p.up)) || far(t * zOf(dz, p.dn), zOf(z, p.dn));
                        });
                    }
                    for (std::size_t t = 0; t < un; ++t) {
                        if (store_of[t] < 0 || pinned[t] || dz[t] == 0.0) continue;
                        const int ti = static_cast<int>(t);
                        const double v0 = volume(ti, z[t], nullptr);
                        bound([&](double s) { return (volume(ti, z[t] + s * dz[t], nullptr) - v0 - s * st_[t].a * dz[t]) / dt; },
                              [&](double s) { return far(s * dz[t], z[t]); });
                    }
                }
                if (tc < 1.0) {
                    ++stats.kink_bounded;
                    lams.push_back(tc);
                    if (tp < tc) lams.push_back(tp);
                    for (double l = 0.5 * tp; lams.size() < 14; l *= 0.5) lams.push_back(l);
                } else {
                    for (double l = 0.5; lams.size() < 11; l *= 0.5) lams.push_back(l);
                }
                for (double l : lams) {
                    r = trial(l);
                    if (r <= (1.0 - 1.0e-4 * l) * r0 || converged(Ft, Mt, zt)) { acc = true; lam = l; break; }
                    if (r < best) { best = r; best_lam = l; }
                }
                if (!acc) {
                    ++stats.armijo_failures;
                    // No step decreases ‖F‖₁: take the best one, or (none better) step just
                    // past the kink, which moves the iterate to the next piece. Never the
                    // blind full step: taken anyway, it fed a period-6 cycle through
                    // residuals 4 -> 4e4 (Bellinge G71F320/G71F68Y, 2026-11-02).
                    lam = best < r0 ? best_lam : (tc < 1.0 ? tp : best_lam);
                    trial(lam);
                }
            }
            for (std::size_t t = 0; t < un; ++t) dz_last[t] = lam * dz[t];
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

}  // namespace openswmm::uncertainty::test_reference

#endif  // OPENSWMM_TEST_ROM_PORT_GRAPH_FULL_REFERENCE_HPP
