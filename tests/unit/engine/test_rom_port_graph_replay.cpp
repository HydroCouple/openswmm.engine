/**
 * @file test_rom_port_graph_replay.cpp
 * @brief Replay of per-member port-graph failure dumps (PORT_HAMILTONIAN O-6).
 *
 * The engine writes a dump (uncertainty/RomPortGraphDump.hpp) for every member
 * solve whose Newton fails when OPENSWMM_PH_DUMP_DIR is set. Here a dump is
 * loaded and the solve re-run on its tabulated laws (piecewise-linear,
 * monotone in each argument).
 *
 *  - DumpRoundTrip: a graph whose Newton is cut short writes a dump; the
 *    replayed residual matches the original laws at the dumped levels.
 *  - CommittedBellingeDumpsConverge: the committed subset of the Bellinge
 *    storm-peak dumps (tests/unit/engine/data/ph_dumps) converge by Newton,
 *    <= 20 iterations, no Jacobi fallback.
 *  - ReplayDirectory (env OPENSWMM_PH_REPLAY_DIR): the same over a whole dump
 *    directory, with a per-dump summary (diagnostic; skipped when unset).
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "uncertainty/RomPortGraph.hpp"
#include "uncertainty/RomPortGraphDump.hpp"
#include "uncertainty/RomStorePorts.hpp"

using namespace openswmm::uncertainty;
namespace D = openswmm::uncertainty::dump;
namespace P = openswmm::uncertainty::ports;
namespace fs = std::filesystem;

namespace {

/// Dumps written before the writer merged close knots: merge them here.
void cleanTables(D::PortGraphDump& d) {
    for (auto& T : d.stores) {
        D::Table1 c;
        for (std::size_t i : D::detail::mergeClose(T.x)) { c.x.push_back(T.x[i]); c.v.push_back(T.v[i]); }
        T = c;
    }
    for (auto& p : d.ports) {
        const auto ku = D::detail::mergeClose(p.gu), kd = D::detail::mergeClose(p.gd);
        if (ku.size() == p.gu.size() && kd.size() == p.gd.size()) continue;
        D::PortTable c;
        c.up = p.up; c.dn = p.dn;
        for (std::size_t i : ku) c.gu.push_back(p.gu[i]);
        for (std::size_t j : kd) c.gd.push_back(p.gd[j]);
        for (std::size_t i : ku)
            for (std::size_t j : kd) c.tab.push_back(p.tab[i * p.gd.size() + j]);
        p = c;
    }
}

/// The PortGraph of a dump: the same graph, its laws replaced by the tables.
void buildReplay(const D::PortGraphDump& d, PortGraph& g) {
    g.resize(d.n);
    g.dt = d.dt;
    g.member = d.member;
    const auto un = static_cast<std::size_t>(d.n);
    for (std::size_t t = 0; t < un; ++t) {
        g.S[t] = d.S[t];
        g.pinned[t] = static_cast<char>(d.pinned[t]);
        g.store_of[t] = d.store_of[t];
        g.b[t] = d.b[t];
        g.z_prev[t] = d.z_prev[t];
        g.level[t] = d.level[t];
    }
    for (const auto& T : d.stores) {
        PortGraph::Store st;
        st.dV = [T](double z, double* area) {
            std::size_t k; double f;
            D::locate(T.x, z, k, f);
            if (area) *area = (T.v[k + 1] - T.v[k]) / (T.x[k + 1] - T.x[k]);
            return T.v[k] + f * (T.v[k + 1] - T.v[k]);
        };
        g.stores.push_back(std::move(st));
    }
    g.dV_prev = d.dV_prev;
    g.edges = d.edges;
    for (const auto& pt : d.ports) {
        PortGraph::Port p;
        p.up = pt.up; p.dn = pt.dn;
        p.dq = [pt](int, double zu, double zd) { return D::interpPort(pt, zu, zd); };
        g.ports.push_back(std::move(p));
    }
}

std::string nameOf(const D::PortGraphDump& d, int t) {
    return (t >= 0 && static_cast<std::size_t>(t) < d.names.size()) ? d.names[static_cast<std::size_t>(t)] : std::to_string(t);
}

struct ReplayResult { bool ok = false; long newton = 0, fallbacks = 0, failed = 0; double r_end = 0.0; };

ReplayResult replay(const D::PortGraphDump& d, std::size_t k_dense = 150) {
    PortGraph g;
    g.k_dense = k_dense;   // O-11: -1 forces the dense Schur path, 0 the sparse full-network path
    buildReplay(d, g);
    std::vector<double> z = d.z_start;
    ReplayResult r;
    r.ok = g.solve(z);
    r.newton = g.stats.newton_iters;
    r.fallbacks = g.stats.jacobi_fallbacks;
    r.failed = g.stats.jacobi_failed;
    std::vector<double> F;
    g.residual(z, F);
    for (double v : F) r.r_end += std::fabs(v);
    return r;
}

std::vector<std::string> dumpsIn(const std::string& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.path().extension() == ".bin") out.push_back(e.path().string());
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

// A small graph whose Newton is cut to one iteration fails, is dumped, and the
// replay of the dump reproduces the laws at the dumped levels.
TEST(PortGraphDump, DumpRoundTrip) {
    const fs::path dir = fs::path(PH_TEST_OUTPUT_DIR) / "ph_dump_roundtrip";
    fs::remove_all(dir);
    fs::create_directories(dir);
    PortGraph g;
    g.resize(4);
    g.dt = 30.0;
    for (int t = 0; t < 4; ++t) g.S[static_cast<std::size_t>(t)] = 20.0 + 10.0 * t;
    // Node 0: a store (area 50 ft² above −0.3 ft, 5 ft² below: a corner).
    g.store_of[0] = 0;
    PortGraph::Store st;
    st.dV = [](double z, double* a) {
        const double zc = -0.3;
        if (z >= zc) { if (a) *a = 50.0; return 50.0 * z; }
        if (a) *a = 5.0;
        return 50.0 * zc + 5.0 * (z - zc);
    };
    g.stores.push_back(st);
    g.dV_prev = {0.0};
    // 0 -> 1: a weir-like one-way port with a dry ramp; 1 -> 2 plane edge; 2 -> 3 a raised-end two-port; 3 -> out.
    PortGraph::Port p1;
    p1.up = 0; p1.dn = 1;
    p1.dq = [](int, double zu, double) { return zu == 0.0 ? 0.0 : 3.0 * (std::pow(std::max(0.2 + zu, 0.0), 1.5) - std::pow(0.2, 1.5)); };
    g.ports.push_back(p1);
    FlowEdge e; e.up = 1; e.dn = 2; e.conduit = true; e.ju = 2.0; e.jd = -1.5;
    g.edges.push_back(e);
    PortGraph::Port p2;
    p2.up = 2; p2.dn = 3;
    const double q0 = P::raisedEndFlow(4.0, 10.2, 10.0, 10.1, 10.15);
    p2.dq = [q0](int, double zu, double zd) {
        if (zu == 0.0 && zd == 0.0) return 0.0;
        return P::raisedEndFlow(4.0, 10.2 + zu, 10.0, 10.1 + zd, 10.15) - q0;
    };
    g.ports.push_back(p2);
    FlowEdge e2; e2.up = 3; e2.dn = -1; e2.conduit = false; e2.d = 0.5;
    g.edges.push_back(e2);
    g.b = {4.0, 0.0, -1.0, 0.5};
    g.level = {100.0, 99.0, 98.0, 97.0};
    g.max_newton = 1;
    std::vector<std::string> names{"S0", "J1", "J2", "J3"};
    g.node_names = &names;
    g.sim_time = 1234.0;
    D::installPortGraphDump(g, dir.string());
    std::vector<double> z(4, 0.0);
    g.solve(z);
    ASSERT_EQ(g.stats.jacobi_fallbacks, 1);
    const auto files = dumpsIn(dir.string());
    ASSERT_EQ(files.size(), 1u);
    D::PortGraphDump d;
    ASSERT_TRUE(D::readPortGraphDump(files[0], d));
    EXPECT_EQ(d.n, 4);
    EXPECT_EQ(d.kind, 1);
    EXPECT_DOUBLE_EQ(d.sim_time, 1234.0);
    EXPECT_EQ(d.names[2], "J2");
    EXPECT_EQ(d.ports.size(), 2u);
    EXPECT_EQ(d.ports[0].gd.size(), 1u);   // the weir depends on z_up only
    EXPECT_GT(d.ports[1].gd.size(), 1u);
    EXPECT_EQ(d.trace.size(), 1u);
    // The tabulated residual at the dumped levels matches the original laws to
    // the tabulation's accuracy (Douglas–Peucker keeps ~110 knots per argument:
    // ~1e-4 of the law's scale here).
    PortGraph rg;
    buildReplay(d, rg);
    for (const auto* zs : {&d.z_start, &d.z_newton, &d.z_final}) {
        std::vector<double> F0, F1;
        g.residual(*zs, F0);
        rg.residual(*zs, F1);
        for (std::size_t t = 0; t < F0.size(); ++t) EXPECT_NEAR(F0[t], F1[t], 1e-3 * (1.0 + std::fabs(F0[t]))) << t;
    }
    // At 0 (a knot of every table) the replay is exact: the main run's flows.
    std::vector<double> z0(4, 0.0), F0, F1;
    g.residual(z0, F0);
    rg.residual(z0, F1);
    for (std::size_t t = 0; t < F0.size(); ++t) EXPECT_EQ(F0[t], F1[t]) << t;
    // The replay converges by Newton (20 iterations allowed).
    const ReplayResult r = replay(d);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.fallbacks, 0);
}

// The committed Bellinge storm-peak dumps (see data/ph_dumps/README.md).
TEST(PortGraphDump, CommittedBellingeDumpsConverge) {
    const auto files = dumpsIn(PH_DUMP_DATA_DIR);
    if (files.empty()) GTEST_SKIP() << "no dumps in " << PH_DUMP_DATA_DIR;
    for (const auto& f : files) {
        D::PortGraphDump d;
        ASSERT_TRUE(D::readPortGraphDump(f, d)) << f;
        cleanTables(d);
        // O-11: both factorisations of the same Newton system converge on every dump.
        for (const std::size_t kd : {static_cast<std::size_t>(-1), static_cast<std::size_t>(0)}) {
            const ReplayResult r = replay(d, kd);
            EXPECT_TRUE(r.ok) << f << (kd == 0 ? " (sparse)" : " (dense)");
            EXPECT_EQ(r.fallbacks, 0) << f << (kd == 0 ? " (sparse)" : " (dense)");
            EXPECT_LE(r.newton, 20) << f << (kd == 0 ? " (sparse)" : " (dense)");
        }
    }
}

// Diagnostic over a whole dump directory (env OPENSWMM_PH_REPLAY_DIR).
TEST(PortGraphDump, ReplayDirectory) {
    const char* dir = std::getenv("OPENSWMM_PH_REPLAY_DIR");
    if (!dir || !*dir) GTEST_SKIP() << "OPENSWMM_PH_REPLAY_DIR not set";
    const auto files = dumpsIn(dir);
    long conv = 0, fb = 0;
    for (const auto& f : files) {
        D::PortGraphDump d;
        if (!D::readPortGraphDump(f, d)) { std::printf("unreadable %s\n", f.c_str()); continue; }
        cleanTables(d);
        const ReplayResult r = replay(d);
        conv += (r.ok && r.fallbacks == 0) ? 1 : 0;
        fb += r.fallbacks;
        std::printf("%s kind %d k %zu ports %zu | replay newton %ld fallbacks %ld failed %ld |F| %.3g\n",
                    fs::path(f).filename().string().c_str(), d.kind, d.Nn.size(), d.ports.size(), r.newton, r.fallbacks, r.failed, r.r_end);
    }
    std::printf("replayed %zu dumps: %ld converged by Newton, %ld fallbacks\n", files.size(), conv, fb);
}

// Per-dump diagnosis (env OPENSWMM_PH_REPLAY_DIR): the node Newton leaves
// furthest from balance (its level error |F|·dt/A), what it is (pond, store,
// port end, linear), its ports with their one-sided slopes at Newton's exit and
// whether a kink sits there, and the shape of Newton's trace. One line per dump
// on stdout and in <dir>/diagnose.csv.
TEST(PortGraphDump, DiagnoseDirectory) {
    const char* dir = std::getenv("OPENSWMM_PH_REPLAY_DIR");
    if (!dir || !*dir) GTEST_SKIP() << "OPENSWMM_PH_REPLAY_DIR not set";
    const auto files = dumpsIn(dir);
    std::FILE* csv = std::fopen((std::string(dir) + "/diagnose.csv").c_str(), "w");
    if (csv) std::fprintf(csv, "file,kind,time,member,k,ports,iters,armijo_fail,kink_bounded,min_lam,r_first,r_last,worst,worst_class,worst_area,worst_dz_ft,ports_at_worst,kinked_ports,pond_ratio,big_nodes,replay_newton,replay_fallbacks\n");
    for (const auto& f : files) {
        D::PortGraphDump d;
        if (!D::readPortGraphDump(f, d)) continue;
        cleanTables(d);
        PortGraph g;
        buildReplay(d, g);
        std::vector<double> F;
        g.residual(d.z_newton, F);
        // Worst node by level error.
        int worst = -1; double wl = -1.0, warea = 0.0;
        for (int t = 0; t < d.n; ++t) {
            if (d.pinned[static_cast<std::size_t>(t)]) continue;
            double a = 0.0;
            g.volume(t, d.z_newton[static_cast<std::size_t>(t)], &a);
            const double lv = std::fabs(F[static_cast<std::size_t>(t)]) * d.dt / std::max(a, 1e-6);
            if (lv > wl) { wl = lv; worst = t; warea = a; }
        }
        const std::string wn = nameOf(d, worst);
        const bool is_pond = wn.rfind("pond:", 0) == 0;
        const bool is_store = worst >= 0 && d.store_of[static_cast<std::size_t>(worst)] >= 0;
        bool in_N = std::find(d.Nn.begin(), d.Nn.end(), worst) != d.Nn.end();
        const char* cls = is_pond ? "pond" : is_store ? "store" : in_N ? "port_end" : "linear";
        // Ports at the worst node, with one-sided slopes at Newton's exit.
        std::string pl;
        int kinked = 0;
        double pond_ratio = 0.0;
        auto zat = [&](int t) { return (t < 0 || d.pinned[static_cast<std::size_t>(t)]) ? 0.0 : d.z_newton[static_cast<std::size_t>(t)]; };
        for (const auto& pt : d.ports) {
            if (pt.up != worst && pt.dn != worst) continue;
            const double zu = zat(pt.up), zd = zat(pt.dn), h = 1e-6;
            const double f0 = D::interpPort(pt, zu, zd);
            const double sup = (D::interpPort(pt, zu + h, zd) - f0) / h, sum = (f0 - D::interpPort(pt, zu - h, zd)) / h;
            const double sdp = (D::interpPort(pt, zu, zd + h) - f0) / h, sdm = (f0 - D::interpPort(pt, zu, zd - h)) / h;
            const bool kink = std::fabs(sup - sum) > 1e-3 * (std::fabs(sup) + std::fabs(sum)) + 1e-9 ||
                              std::fabs(sdp - sdm) > 1e-3 * (std::fabs(sdp) + std::fabs(sdm)) + 1e-9;
            kinked += kink ? 1 : 0;
            const int other = pt.up == worst ? pt.dn : pt.up;
            const std::string on = other < 0 ? "OUT" : nameOf(d, other);
            if (on.rfind("pond:", 0) == 0 || is_pond) {
                double ao = 0.0, aw = 0.0;
                if (other >= 0) g.volume(other, zat(other), &ao);
                g.volume(worst, zat(worst), &aw);
                if (other >= 0 && ao > 0 && aw > 0) pond_ratio = is_pond ? aw / ao : ao / aw;
            }
            char buf[256];
            std::snprintf(buf, sizeof buf, "%s%s(q=%.3g su=%.3g|%.3g sd=%.3g|%.3g%s) ", pt.up == worst ? "->" : "<-", on.c_str(), f0, sum, sup, sdm, sdp, kink ? " KINK" : "");
            pl += buf;
        }
        // Trace.
        int af = 0, kb = 0;
        double minlam = 1.0;
        std::string bigs;
        for (std::size_t q = 0; q < d.trace.size(); ++q) {
            const auto& tr = d.trace[q];
            af += (tr.flags & 2) ? 1 : 0;
            kb += (tr.flags & 1) ? 1 : 0;
            if (!(tr.flags & 4)) minlam = std::min(minlam, tr.lam);
            if (q + 6 >= d.trace.size()) { bigs += nameOf(d, tr.big); bigs += (tr.big_dz >= 0 ? "+ " : "- "); }
        }
        const ReplayResult r = replay(d);
        const double r_first = d.trace.empty() ? 0.0 : d.trace.front().r0, r_last = d.trace.empty() ? 0.0 : d.trace.back().r1;
        std::printf("%s %s t=%.0f m=%d k=%zu ports=%zu its=%zu af=%d kb=%d minlam=%.2g r %.3g->%.3g | worst %s [%s A=%.3g dz=%.3g ft] %s| ratio %.3g | big %s| replay %ld/%ld\n",
                    fs::path(f).filename().string().c_str(), d.kind == 2 ? "NC" : "FB", d.sim_time, d.member, d.Nn.size(), d.ports.size(),
                    d.trace.size(), af, kb, minlam, r_first, r_last, wn.c_str(), cls, warea, wl, pl.c_str(), pond_ratio, bigs.c_str(), r.newton, r.fallbacks);
        if (csv)
            std::fprintf(csv, "%s,%d,%.0f,%d,%zu,%zu,%zu,%d,%d,%.3g,%.4g,%.4g,%s,%s,%.4g,%.4g,\"%s\",%d,%.4g,\"%s\",%ld,%ld\n",
                         fs::path(f).filename().string().c_str(), d.kind, d.sim_time, d.member, d.Nn.size(), d.ports.size(), d.trace.size(), af, kb,
                         minlam, r_first, r_last, wn.c_str(), cls, warea, wl, pl.c_str(), kinked, pond_ratio, bigs.c_str(), r.newton, r.fallbacks);
    }
    if (csv) std::fclose(csv);
}

// Compaction of a dump for committing (env OPENSWMM_PH_COMPACT_FROM = a dump
// directory, OPENSWMM_PH_COMPACT_TO = the output directory, optional
// OPENSWMM_PH_COMPACT_FILES = comma-separated file names). A port whose table is
// exactly a plane through the origin, ju·z_up + jd·z_dn (to 1e-9 of its scale),
// on a 9×9 grid over the box its levels span in the dump (start, Newton exit,
// final, ±0.05 ft) becomes a plane edge with those slopes; a port whose ends
// hardly move in the solve (within 1e-3 ft) becomes its tangent plane at the
// final levels (an approximation: what the compacted dump reproduces is checked);
// every other port keeps its table. A store whose table is exactly a·z (the
// street ponds) becomes a linear node of area a. The Schur base is left out. The graph is then written again with the dump writer (tables of the
// tables). What a compacted dump reproduces is checked by the tests above, not
// assumed.
TEST(PortGraphDump, CompactDumps) {
    const char* from = std::getenv("OPENSWMM_PH_COMPACT_FROM");
    const char* to = std::getenv("OPENSWMM_PH_COMPACT_TO");
    if (!from || !to || !*from || !*to) GTEST_SKIP() << "OPENSWMM_PH_COMPACT_FROM/_TO not set";
    std::vector<std::string> files;
    if (const char* fl = std::getenv("OPENSWMM_PH_COMPACT_FILES")) {
        std::string all(fl), cur;
        for (char ch : all + ",") { if (ch == ',') { if (!cur.empty()) files.push_back(std::string(from) + "/" + cur); cur.clear(); } else cur += ch; }
    } else files = dumpsIn(from);
    fs::create_directories(to);
    for (const auto& f : files) {
        D::PortGraphDump d;
        ASSERT_TRUE(D::readPortGraphDump(f, d)) << f;
        cleanTables(d);
        auto lo_hi = [&](int t, double& lo, double& hi) {
            lo = hi = 0.0;
            if (t < 0 || d.pinned[static_cast<std::size_t>(t)]) return;
            const auto u = static_cast<std::size_t>(t);
            lo = std::min({d.z_start[u], d.z_newton[u], d.z_final[u], 0.0}) - 0.05;
            hi = std::max({d.z_start[u], d.z_newton[u], d.z_final[u], 0.0}) + 0.05;
        };
        std::vector<D::PortTable> kept;
        std::size_t planes = 0, tangents = 0;
        const std::size_t n_ports = d.ports.size();
        for (const auto& pt : d.ports) {
            double ul, uh, dl, dh;
            lo_hi(pt.up, ul, uh);
            lo_hi(pt.dn, dl, dh);
            const double h = 1e-3;
            const double ju = (D::interpPort(pt, h, 0.0) - D::interpPort(pt, -h, 0.0)) / (2 * h);
            const double jd = (D::interpPort(pt, 0.0, h) - D::interpPort(pt, 0.0, -h)) / (2 * h);
            bool plane = ju >= 0.0 && jd <= 0.0 && D::interpPort(pt, 0.0, 0.0) == 0.0;
            double scale = 0.0;
            for (int a = 0; a <= 8 && plane; ++a)
                for (int b = 0; b <= 8 && plane; ++b) {
                    const double zu = ul + (uh - ul) * a / 8.0, zd = dl + (dh - dl) * b / 8.0;
                    const double v = D::interpPort(pt, zu, zd), w = ju * zu + jd * zd;
                    scale = std::max(scale, std::fabs(v));
                    plane = std::fabs(v - w) <= 1e-9 * (1.0 + std::fabs(v));
                }
            if (plane) {
                FlowEdge e;
                e.up = pt.up; e.dn = pt.dn; e.conduit = true; e.ju = ju; e.jd = pt.dn >= 0 ? jd : 0.0;
                d.edges.push_back(e);
                ++planes;
                continue;
            }
            // A port whose ends hardly move in the solve (start, Newton exit and
            // final within 1e-3 ft) becomes its tangent plane at the final
            // levels: slopes as edge, the offset as opposite sources at its ends.
            auto still = [&](int t) {
                if (t < 0 || d.pinned[static_cast<std::size_t>(t)]) return true;
                const auto u = static_cast<std::size_t>(t);
                return std::fabs(d.z_start[u] - d.z_final[u]) < 1e-3 && std::fabs(d.z_newton[u] - d.z_final[u]) < 1e-3;
            };
            if (still(pt.up) && still(pt.dn)) {
                const auto zf = [&](int t) { return (t < 0 || d.pinned[static_cast<std::size_t>(t)]) ? 0.0 : d.z_final[static_cast<std::size_t>(t)]; };
                const double zu = zf(pt.up), zd = zf(pt.dn), f0 = D::interpPort(pt, zu, zd);
                const double su = (D::interpPort(pt, zu + h, zd) - D::interpPort(pt, zu - h, zd)) / (2 * h);
                const double sd = (D::interpPort(pt, zu, zd + h) - D::interpPort(pt, zu, zd - h)) / (2 * h);
                if (su >= 0.0 && sd <= 0.0) {
                    FlowEdge e;
                    e.up = pt.up; e.dn = pt.dn; e.conduit = true; e.ju = su; e.jd = pt.dn >= 0 ? sd : 0.0;
                    d.edges.push_back(e);
                    const double c = f0 - su * zu - (pt.dn >= 0 ? sd * zd : 0.0);
                    if (pt.up >= 0) d.b[static_cast<std::size_t>(pt.up)] -= c;
                    if (pt.dn >= 0) d.b[static_cast<std::size_t>(pt.dn)] += c;
                    ++tangents;
                    continue;
                }
            }
            kept.push_back(pt);
        }
        d.ports.swap(kept);
        // A store whose table is exactly a·z (the street ponds) becomes a linear
        // node of area a.
        std::size_t linear_stores = 0;
        {
            std::vector<int> new_index(d.stores.size(), -1);
            std::vector<D::Table1> st;
            std::vector<double> dvp;
            for (std::size_t q = 0; q < d.stores.size(); ++q) {
                const auto& T = d.stores[q];
                const double a = T.x.size() > 1 ? (T.v.back() - T.v.front()) / (T.x.back() - T.x.front()) : 0.0;
                bool lin = a > 0.0;
                for (std::size_t i = 0; i < T.x.size() && lin; ++i) lin = std::fabs(T.v[i] - a * T.x[i]) <= 1e-12 * (1.0 + std::fabs(T.v[i]));
                if (!lin) { new_index[q] = static_cast<int>(st.size()); st.push_back(T); dvp.push_back(d.dV_prev[q]); }
                else ++linear_stores;
                for (int t = 0; t < d.n && lin; ++t)
                    if (d.store_of[static_cast<std::size_t>(t)] == static_cast<int>(q)) {
                        d.S[static_cast<std::size_t>(t)] = a;
                        d.z_prev[static_cast<std::size_t>(t)] = d.dV_prev[q] / a;
                    }
            }
            for (auto& so : d.store_of) if (so >= 0) so = new_index[static_cast<std::size_t>(so)];
            d.stores.swap(st);
            d.dV_prev.swap(dvp);
        }
        PortGraph g;
        buildReplay(d, g);
        g.node_names = &d.names;
        g.sim_time = d.sim_time;
        g.prepare();
        PortGraph::FailureInfo info;
        info.converged = d.kind == 1;
        info.z_start = d.z_start;
        info.z_newton = d.z_newton;
        info.trace = d.trace;
        const std::string out = std::string(to) + "/" + fs::path(f).filename().string();
        ASSERT_TRUE(D::writePortGraphDump(out, g, info, d.z_final, /*with_schur=*/false)) << out;
        std::printf("%s: %zu ports -> %zu tables + %zu planes + %zu tangent planes, %zu linear stores, %ju bytes\n", fs::path(f).filename().string().c_str(),
                    n_ports, d.ports.size(), planes, tangents, linear_stores, static_cast<std::uintmax_t>(fs::file_size(out)));
    }
}
