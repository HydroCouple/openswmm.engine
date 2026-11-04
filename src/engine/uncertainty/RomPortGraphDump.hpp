/**
 * @file RomPortGraphDump.hpp
 * @brief Failure dumps of the per-member port-graph solve (PORT_HAMILTONIAN O-6).
 *
 * Diagnostic only. installPortGraphDump() sets PortGraph::on_failure so that a
 * solve whose Newton fails writes one binary file: the whole graph (areas,
 * pins, sources, start levels, plane edges, the N set and its Schur base), the
 * start, Newton's last iterate and the final iterate, Newton's per-iteration
 * trace, and every store and port law TABULATED (the laws are lambdas). A
 * replay interpolates the tables piecewise-linearly (bilinear for two-argument
 * ports, linear extrapolation outside), which keeps every law monotone in each
 * argument.
 *
 * Tabulation per argument over centre ± max(1 ft, 4·|z|) (centre = Newton's
 * exit level, |z| the largest of the start, exit and final levels): 41 uniform
 * points, the level 0 and the breakpoints of the law found by sampling it (2001
 * points over the range and 2001 within ±1 ft of 0 and of the start, exit and
 * final levels, reduced by Douglas–Peucker at the smallest tolerance that keeps
 * ~110 points; at most ~150 knots per argument, knots closer than 1e-6 ft
 * merged). The first Bellinge dumps (2026-11-04) were written with the
 * start/exit/final levels as knots as well; the reader takes both. An argument
 * the law does not depend on (an end at −1, a pinned node, or a table constant
 * along it) keeps a single knot.
 */
#ifndef OPENSWMM_ENGINE_UNCERTAINTY_ROM_PORT_GRAPH_DUMP_HPP
#define OPENSWMM_ENGINE_UNCERTAINTY_ROM_PORT_GRAPH_DUMP_HPP

#include "RomPortGraph.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace openswmm::uncertainty::dump {

struct Table1 { std::vector<double> x, v; };
struct PortTable {
    int up = -1, dn = -1;
    std::vector<double> gu, gd, tab;   ///< tab[iu * gd.size() + id]
};

struct PortGraphDump {
    int kind = 0;            ///< 1 fallback (Jacobi converged), 2 not converged
    int member = 0, n = 0;
    double sim_time = 0.0, dt = 0.0;
    std::vector<double> S, b, z_prev, level, z_start, z_newton, z_final;
    std::vector<int> pinned, store_of;
    std::vector<std::string> names;
    std::vector<Table1> stores;
    std::vector<double> dV_prev;
    std::vector<FlowEdge> edges;
    std::vector<PortTable> ports;
    std::vector<int> Nn;
    std::vector<double> Jbase;
    std::vector<PortGraph::TraceRec> trace;
};

/// Segment of ascending knots x holding xx (the end segments extend outside) and its fraction.
inline void locate(const std::vector<double>& x, double xx, std::size_t& k, double& t) {
    const std::size_t m = x.size();
    if (m < 2) { k = 0; t = 0.0; return; }
    if (xx <= x[0]) k = 0;
    else if (xx >= x[m - 1]) k = m - 2;
    else k = static_cast<std::size_t>(std::upper_bound(x.begin(), x.end(), xx) - x.begin()) - 1;
    t = (xx - x[k]) / (x[k + 1] - x[k]);
}

/// Piecewise-linear interpolation on ascending knots, linear extrapolation outside.
inline double interp1(const std::vector<double>& x, const std::vector<double>& v, double xx) {
    if (x.size() == 1) return v[0];
    std::size_t k; double t;
    locate(x, xx, k, t);
    return v[k] + t * (v[k + 1] - v[k]);
}

/// Bilinear interpolation of a port table (linear extrapolation outside):
/// monotone in each argument when the table is.
inline double interpPort(const PortTable& p, double zu, double zd) {
    const std::size_t nu = p.gu.size(), nd = p.gd.size();
    std::size_t ku = 0, kd = 0;
    double tu = 0.0, td = 0.0;
    if (nu > 1) locate(p.gu, zu, ku, tu);
    if (nd > 1) locate(p.gd, zd, kd, td);
    auto row = [&](std::size_t iu) {
        const double* r = &p.tab[iu * nd];
        return nd == 1 ? r[0] : r[kd] + td * (r[kd + 1] - r[kd]);
    };
    if (nu == 1) return row(0);
    const double a = row(ku), c = row(ku + 1);
    return a + tu * (c - a);
}

namespace detail {

inline void dpKeep(const std::vector<double>& x, const std::vector<double>& y, std::size_t i0, std::size_t i1, double eps, std::vector<char>& keep) {
    // Iterative Douglas–Peucker (explicit stack: the sample count can be 10^4).
    std::vector<std::pair<std::size_t, std::size_t>> st{{i0, i1}};
    while (!st.empty()) {
        auto [a, c] = st.back();
        st.pop_back();
        if (c <= a + 1) continue;
        double worst = -1.0;
        std::size_t wi = a;
        for (std::size_t i = a + 1; i < c; ++i) {
            const double t = (x[i] - x[a]) / (x[c] - x[a]);
            const double e = std::fabs(y[i] - (y[a] + t * (y[c] - y[a])));
            if (e > worst) { worst = e; wi = i; }
        }
        if (worst > eps) { keep[wi] = 1; st.push_back({a, wi}); st.push_back({wi, c}); }
    }
}

/// Knots closer than 1e-6·max(1 ft, |x|) are merged (the first kept, 0 always kept):
/// a pair a roundoff apart carries a roundoff slope (an anchor next to its own
/// neighbour gave 0 and 1e4 cfs/ft in the first Bellinge dumps).
inline std::vector<std::size_t> mergeClose(const std::vector<double>& x) {
    std::vector<std::size_t> keep;
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (!keep.empty() && x[i] - x[keep.back()] < 1.0e-6 * std::max(1.0, std::fabs(x[i]))) {
            if (x[i] == 0.0) keep.back() = i;
            continue;
        }
        keep.push_back(i);
    }
    return keep;
}

/// Thin a sorted knot set to about 150 points, evenly, keeping the anchors in [lo, hi].
inline void thin(std::vector<double>& out, const std::vector<double>& anchors, double lo, double hi) {
    constexpr std::size_t kCap = 150;
    if (out.size() <= kCap) return;
    std::vector<double> th;
    const double step = static_cast<double>(out.size()) / static_cast<double>(kCap);
    for (double i = 0.0; i < static_cast<double>(out.size()); i += step) th.push_back(out[static_cast<std::size_t>(i)]);
    th.push_back(out.back());
    for (double a : anchors) if (a >= lo && a <= hi) th.push_back(a);
    std::sort(th.begin(), th.end());
    th.erase(std::unique(th.begin(), th.end()), th.end());
    out.swap(th);
}

/// Knots of f on [lo, hi] (see the file comment).
inline std::vector<double> axisKnots(const std::function<double(double)>& f, double lo, double hi, const std::vector<double>& anchors,
                                     std::size_t max_pts = 110) {
    std::vector<double> xs;
    constexpr int kS = 2001;
    for (int m = 0; m < kS; ++m) xs.push_back(lo + (hi - lo) * m / (kS - 1));
    for (double a : anchors)
        for (int m = 0; m < kS; ++m) {
            const double v = a - 1.0 + 2.0 * m / (kS - 1);
            if (v >= lo && v <= hi) xs.push_back(v);
        }
    std::sort(xs.begin(), xs.end());
    xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
    std::vector<double> ys(xs.size());
    double ymin = 1e300, ymax = -1e300;
    for (std::size_t i = 0; i < xs.size(); ++i) {
        ys[i] = f(xs[i]);
        ymin = std::min(ymin, ys[i]);
        ymax = std::max(ymax, ys[i]);
    }
    // Douglas–Peucker at the smallest tolerance (from 1e-7 of the law's range,
    // ×4 per try) that keeps at most max_pts points, so the curvature is spread
    // evenly over the knots instead of thinned at random.
    std::vector<double> out;
    for (double rel = 1.0e-7;; rel *= 4.0) {
        std::vector<char> keep(xs.size(), 0);
        keep.front() = keep.back() = 1;
        dpKeep(xs, ys, 0, xs.size() - 1, rel * (ymax - ymin) + 1.0e-14, keep);
        out.clear();
        for (std::size_t i = 0; i < xs.size(); ++i) if (keep[i]) out.push_back(xs[i]);
        if (out.size() <= max_pts || rel > 1.0) break;
    }
    // 41 uniform knots and 0 (the main run); the other anchors are not knots: a
    // knot at a level Newton converges to would be a kink of the table exactly
    // at the solution (first Bellinge dumps: pond slopes −11 | −75 on the two
    // sides of an anchor knot).
    for (int m = 0; m <= 40; ++m) out.push_back(lo + (hi - lo) * m / 40.0);
    if (0.0 >= lo && 0.0 <= hi) out.push_back(0.0);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    thin(out, {0.0}, lo, hi);
    std::vector<double> m;
    for (std::size_t i : mergeClose(out)) m.push_back(out[i]);
    return m;
}

template <class T> inline void w(std::ofstream& o, const T& v) { o.write(reinterpret_cast<const char*>(&v), sizeof(T)); }
template <class T> inline void wv(std::ofstream& o, const std::vector<T>& v) {
    const auto m = static_cast<std::int64_t>(v.size());
    w(o, m);
    if (m) o.write(reinterpret_cast<const char*>(v.data()), static_cast<std::streamsize>(sizeof(T) * v.size()));
}
template <class T> inline bool r(std::ifstream& i, T& v) { return static_cast<bool>(i.read(reinterpret_cast<char*>(&v), sizeof(T))); }
template <class T> inline bool rv(std::ifstream& i, std::vector<T>& v) {
    std::int64_t m = 0;
    if (!r(i, m) || m < 0 || m > (1LL << 31)) return false;
    v.resize(static_cast<std::size_t>(m));
    return m == 0 || static_cast<bool>(i.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(sizeof(T) * v.size())));
}

}  // namespace detail

inline constexpr char kMagic[8] = {'P', 'H', 'D', 'U', 'M', 'P', '0', '1'};

/// Write one dump (see the file comment). Returns false on an I/O error.
/// with_schur = false writes an empty Schur base (k² doubles; the compacted test
/// dumps leave it out: it is a diagnostic, the replay rebuilds it).
inline bool writePortGraphDump(const std::string& path, const PortGraph& g, const PortGraph::FailureInfo& info,
                               const std::vector<double>& z_final, bool with_schur = true) {
    using namespace detail;
    const auto un = static_cast<std::size_t>(g.n);
    auto zat = [&](const std::vector<double>& z, int t) {
        if (t < 0 || g.pinned[static_cast<std::size_t>(t)] || static_cast<std::size_t>(t) >= z.size()) return 0.0;
        const double v = z[static_cast<std::size_t>(t)];
        return std::isfinite(v) ? v : 0.0;
    };
    // Per node: tabulation centre, half range and anchors.
    auto range = [&](int t, double& lo, double& hi, std::vector<double>& anc) {
        const double c = zat(info.z_newton, t);
        const double zm = std::max({std::fabs(zat(info.z_start, t)), std::fabs(c), std::fabs(zat(z_final, t))});
        const double R = std::max(1.0, 4.0 * zm);
        lo = c - R; hi = c + R;
        anc = {0.0, zat(info.z_start, t), c, zat(z_final, t)};
        std::sort(anc.begin(), anc.end());
        anc.erase(std::unique(anc.begin(), anc.end()), anc.end());
    };
    std::ofstream o(path, std::ios::binary);
    if (!o) return false;
    o.write(kMagic, 8);
    const std::int32_t version = 1, kind = info.converged ? 1 : 2, member = g.member, n = g.n;
    w(o, version); w(o, kind); w(o, member); w(o, g.sim_time); w(o, g.dt); w(o, n);
    wv(o, g.S); wv(o, g.b); wv(o, g.z_prev); wv(o, g.level);
    wv(o, info.z_start); wv(o, info.z_newton); wv(o, z_final);
    std::vector<std::int32_t> pin(un), sto(un);
    for (std::size_t t = 0; t < un; ++t) { pin[t] = g.pinned[t]; sto[t] = g.store_of[t]; }
    wv(o, pin); wv(o, sto);
    const std::int64_t nn = g.node_names ? static_cast<std::int64_t>(g.node_names->size()) : 0;
    w(o, nn);
    for (std::int64_t q = 0; q < nn; ++q) {
        const std::string& s = (*g.node_names)[static_cast<std::size_t>(q)];
        std::vector<char> cs(s.begin(), s.end());
        wv(o, cs);
    }
    // Stores: V(z) tabulated.
    const std::int64_t ns = static_cast<std::int64_t>(g.stores.size());
    w(o, ns);
    std::vector<int> node_of_store(g.stores.size(), -1);
    for (std::size_t t = 0; t < un; ++t) if (g.store_of[t] >= 0) node_of_store[static_cast<std::size_t>(g.store_of[t])] = static_cast<int>(t);
    for (std::size_t s = 0; s < g.stores.size(); ++s) {
        const int t = node_of_store[s];
        double lo = -1.0, hi = 1.0;
        std::vector<double> anc{0.0};
        if (t >= 0) range(t, lo, hi, anc);
        auto f = [&](double z) { return g.stores[s].dV(z, nullptr); };
        Table1 T;
        T.x = axisKnots(f, lo, hi, anc);
        for (double x : T.x) T.v.push_back(f(x));
        wv(o, T.x); wv(o, T.v);
    }
    wv(o, g.dV_prev);
    // Plane edges.
    const std::int64_t ne = static_cast<std::int64_t>(g.edges.size());
    w(o, ne);
    for (const auto& e : g.edges) {
        const std::int32_t up = e.up, dn = e.dn, link = e.link, cd = e.conduit ? 1 : 0;
        w(o, up); w(o, dn); w(o, link); w(o, cd); w(o, e.q); w(o, e.ju); w(o, e.jd); w(o, e.d);
    }
    // Ports: tabulated at this member.
    const std::int64_t np = static_cast<std::int64_t>(g.ports.size());
    w(o, np);
    const int mem = g.member;
    for (const auto& p : g.ports) {
        const bool hu = p.up >= 0 && !g.pinned[static_cast<std::size_t>(p.up)];
        const bool hd = p.dn >= 0 && !g.pinned[static_cast<std::size_t>(p.dn)];
        PortTable T;
        T.up = p.up; T.dn = p.dn;
        double ulo = 0, uhi = 0, dlo = 0, dhi = 0;
        std::vector<double> ua{0.0}, da{0.0};
        if (hu) range(p.up, ulo, uhi, ua);
        if (hd) range(p.dn, dlo, dhi, da);
        if (hu) {
            std::vector<double> ks;
            for (double yd : da) {
                auto k1 = axisKnots([&](double x) { return p.dq(mem, x, yd); }, ulo, uhi, ua);
                ks.insert(ks.end(), k1.begin(), k1.end());
            }
            std::sort(ks.begin(), ks.end());
            ks.erase(std::unique(ks.begin(), ks.end()), ks.end());
            thin(ks, {0.0}, ulo, uhi);
            for (std::size_t i : mergeClose(ks)) T.gu.push_back(ks[i]);
        } else T.gu = {0.0};
        if (hd) {
            std::vector<double> ks;
            for (double yu : ua) {
                auto k1 = axisKnots([&](double x) { return p.dq(mem, yu, x); }, dlo, dhi, da);
                ks.insert(ks.end(), k1.begin(), k1.end());
            }
            std::sort(ks.begin(), ks.end());
            ks.erase(std::unique(ks.begin(), ks.end()), ks.end());
            thin(ks, {0.0}, dlo, dhi);
            for (std::size_t i : mergeClose(ks)) T.gd.push_back(ks[i]);
        } else T.gd = {0.0};
        const std::size_t nu = T.gu.size(), nd = T.gd.size();
        T.tab.resize(nu * nd);
        for (std::size_t iu = 0; iu < nu; ++iu)
            for (std::size_t id = 0; id < nd; ++id) T.tab[iu * nd + id] = p.dq(mem, T.gu[iu], T.gd[id]);
        // An argument the table does not depend on keeps one knot.
        if (nd > 1) {
            bool flat = true;
            for (std::size_t iu = 0; iu < nu && flat; ++iu)
                for (std::size_t id = 1; id < nd && flat; ++id) flat = T.tab[iu * nd + id] == T.tab[iu * nd];
            if (flat) {
                std::vector<double> t2(nu);
                for (std::size_t iu = 0; iu < nu; ++iu) t2[iu] = T.tab[iu * nd];
                T.tab.swap(t2); T.gd = {0.0};
            }
        }
        if (nu > 1) {
            const std::size_t nd2 = T.gd.size();
            bool flat = true;
            for (std::size_t iu = 1; iu < nu && flat; ++iu)
                for (std::size_t id = 0; id < nd2 && flat; ++id) flat = T.tab[iu * nd2 + id] == T.tab[id];
            if (flat) { T.tab.resize(nd2); T.gu = {0.0}; }
        }
        const std::int32_t up = T.up, dn = T.dn;
        w(o, up); w(o, dn);
        wv(o, T.gu); wv(o, T.gd); wv(o, T.tab);
    }
    std::vector<std::int32_t> Nn(g.nSet().begin(), g.nSet().end());
    wv(o, Nn);
    wv(o, with_schur ? g.schurBase() : std::vector<double>());
    const std::int64_t ntr = static_cast<std::int64_t>(info.trace.size());
    w(o, ntr);
    for (const auto& tr : info.trace) {
        const std::int32_t fl = tr.flags, wo = tr.worst, bi = tr.big;
        w(o, tr.r0); w(o, tr.r1); w(o, tr.lam); w(o, fl); w(o, wo); w(o, tr.worst_F); w(o, bi); w(o, tr.big_dz);
    }
    return static_cast<bool>(o);
}

/// Read one dump. Returns false if the file is not a complete version-1 dump.
inline bool readPortGraphDump(const std::string& path, PortGraphDump& d) {
    using namespace detail;
    std::ifstream i(path, std::ios::binary);
    char mg[8];
    if (!i.read(mg, 8) || !std::equal(mg, mg + 8, kMagic)) return false;
    std::int32_t version = 0, kind = 0, member = 0, n = 0;
    if (!r(i, version) || version != 1 || !r(i, kind) || !r(i, member) || !r(i, d.sim_time) || !r(i, d.dt) || !r(i, n)) return false;
    d.kind = kind; d.member = member; d.n = n;
    if (!rv(i, d.S) || !rv(i, d.b) || !rv(i, d.z_prev) || !rv(i, d.level) || !rv(i, d.z_start) || !rv(i, d.z_newton) || !rv(i, d.z_final))
        return false;
    std::vector<std::int32_t> pin, sto;
    if (!rv(i, pin) || !rv(i, sto)) return false;
    d.pinned.assign(pin.begin(), pin.end());
    d.store_of.assign(sto.begin(), sto.end());
    std::int64_t nn = 0;
    if (!r(i, nn)) return false;
    d.names.resize(static_cast<std::size_t>(nn));
    for (auto& s : d.names) { std::vector<char> cs; if (!rv(i, cs)) return false; s.assign(cs.begin(), cs.end()); }
    std::int64_t ns = 0;
    if (!r(i, ns)) return false;
    d.stores.resize(static_cast<std::size_t>(ns));
    for (auto& T : d.stores) if (!rv(i, T.x) || !rv(i, T.v)) return false;
    if (!rv(i, d.dV_prev)) return false;
    std::int64_t ne = 0;
    if (!r(i, ne)) return false;
    d.edges.resize(static_cast<std::size_t>(ne));
    for (auto& e : d.edges) {
        std::int32_t up, dn, link, cd;
        if (!r(i, up) || !r(i, dn) || !r(i, link) || !r(i, cd) || !r(i, e.q) || !r(i, e.ju) || !r(i, e.jd) || !r(i, e.d)) return false;
        e.up = up; e.dn = dn; e.link = link; e.conduit = cd != 0;
    }
    std::int64_t np = 0;
    if (!r(i, np)) return false;
    d.ports.resize(static_cast<std::size_t>(np));
    for (auto& p : d.ports) {
        std::int32_t up, dn;
        if (!r(i, up) || !r(i, dn) || !rv(i, p.gu) || !rv(i, p.gd) || !rv(i, p.tab)) return false;
        p.up = up; p.dn = dn;
    }
    std::vector<std::int32_t> Nn;
    if (!rv(i, Nn) || !rv(i, d.Jbase)) return false;
    d.Nn.assign(Nn.begin(), Nn.end());
    std::int64_t ntr = 0;
    if (!r(i, ntr)) return false;
    d.trace.resize(static_cast<std::size_t>(ntr));
    for (auto& tr : d.trace) {
        std::int32_t fl, wo, bi;
        if (!r(i, tr.r0) || !r(i, tr.r1) || !r(i, tr.lam) || !r(i, fl) || !r(i, wo) || !r(i, tr.worst_F) || !r(i, bi) || !r(i, tr.big_dz)) return false;
        tr.flags = fl; tr.worst = wo; tr.big = bi;
    }
    return true;
}

/// Install the dump hook on g: files <dir>/ph_<seq>_<nc|fb>_t<s>_m<member>.bin
/// plus one line per dump in <dir>/index.csv. Non-converged solves are dumped
/// up to max_nc, Jacobi fallbacks that converged up to max_fb (per process).
inline void installPortGraphDump(PortGraph& g, const std::string& dir, int max_nc = 400, int max_fb = 200) {
    static std::atomic<int> seq{0}, n_nc{0}, n_fb{0};
    g.on_failure = [dir, max_nc, max_fb](const PortGraph& gg, const PortGraph::FailureInfo& info, const std::vector<double>& zf) {
        auto& cnt = info.converged ? n_fb : n_nc;
        if (cnt.fetch_add(1) >= (info.converged ? max_fb : max_nc)) return;
        const int s = seq.fetch_add(1);
        char name[160];
        std::snprintf(name, sizeof name, "ph_%04d_%s_t%.0f_m%02d.bin", s, info.converged ? "fb" : "nc", gg.sim_time, gg.member);
        const std::string path = dir + "/" + name;
        const bool ok = writePortGraphDump(path, gg, info, zf);
        // Index line: file, kind, time, member, k, ports, Newton iterations, last ‖F‖₁, worst node.
        std::FILE* f = std::fopen((dir + "/index.csv").c_str(), "a");
        if (f) {
            const auto& tr = info.trace;
            const int worst = tr.empty() ? -1 : tr.back().worst;
            const std::string wn = (worst >= 0 && gg.node_names && static_cast<std::size_t>(worst) < gg.node_names->size()) ? (*gg.node_names)[static_cast<std::size_t>(worst)] : std::to_string(worst);
            std::fprintf(f, "%s,%s,%.1f,%d,%zu,%zu,%zu,%.6g,%s,%d\n", name, info.converged ? "fb" : "nc", gg.sim_time, gg.member,
                         gg.nSet().size(), gg.ports.size(), tr.size(), tr.empty() ? 0.0 : tr.back().r1, wn.c_str(), ok ? 1 : 0);
            std::fclose(f);
        }
    };
}

}  // namespace openswmm::uncertainty::dump

#endif  // OPENSWMM_ENGINE_UNCERTAINTY_ROM_PORT_GRAPH_DUMP_HPP
