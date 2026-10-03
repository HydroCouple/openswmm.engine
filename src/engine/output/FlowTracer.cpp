// SPDX-License-Identifier: Apache-2.0
#include "FlowTracer.hpp"
#include "OutputReader.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <numeric>
#include <openswmm/engine/openswmm_output.h>
#include <queue>
#include <sstream>
#include <unordered_map>

namespace openswmm::trace
{
namespace
{
constexpr double nan = std::numeric_limits<double>::quiet_NaN();
double positiveIntegral(double a, double b, double dt)
{
    if (a >= 0 && b >= 0)
        return (a + b) * 0.5 * dt;
    if (a <= 0 && b <= 0)
        return 0;
    const double p = std::max(a, b);
    return 0.5 * dt * p * p / std::abs(b - a);
}
bool tick(SWMM_TraceProgress cb, void *u, double p, const char *s)
{
    return cb && cb(p, s, u) != 0;
}
struct Edge
{
    int from, to, link;
    double p, delay;
};
struct Graph
{
    int n;
    std::vector<Edge> edges;
    std::vector<std::vector<int>> out, in, components;
    std::vector<int> component, order;
    std::vector<bool> closed;
    std::vector<double> sink;
    std::vector<int> kind;
    bool cyclic = false;
    explicit Graph(int count)
        : n(count), out(count), in(count), component(count, -1), sink(count), kind(count)
    {
    }
    void add(Edge e)
    {
        int i = int(edges.size());
        edges.push_back(e);
        out[e.from].push_back(i);
        in[e.to].push_back(i);
    }
    void partition()
    {
        std::vector<int> finish;
        std::vector<bool> visited(n);
        for (int root = 0; root < n; ++root)
            if (!visited[root])
            {
                std::vector<std::pair<int, size_t>> stack{{root, 0}};
                visited[root] = true;
                while (!stack.empty())
                {
                    auto &[v, k] = stack.back();
                    if (k < out[v].size())
                    {
                        int w = edges[out[v][k++]].to;
                        if (!visited[w])
                        {
                            visited[w] = true;
                            stack.emplace_back(w, 0);
                        }
                    }
                    else
                    {
                        finish.push_back(v);
                        stack.pop_back();
                    }
                }
            }
        for (auto it = finish.rbegin(); it != finish.rend(); ++it)
            if (component[*it] < 0)
            {
                int c = int(components.size());
                components.emplace_back();
                std::vector<int> stack{*it};
                component[*it] = c;
                while (!stack.empty())
                {
                    int v = stack.back();
                    stack.pop_back();
                    components[c].push_back(v);
                    for (int ei : in[v])
                    {
                        int w = edges[ei].from;
                        if (component[w] < 0)
                        {
                            component[w] = c;
                            stack.push_back(w);
                        }
                    }
                }
            }
        std::vector<int> indegree(components.size());
        closed.assign(components.size(), true);
        for (int i = 0; i < n; ++i)
            if (sink[i] > 0)
                closed[component[i]] = false;
        for (auto e : edges)
        {
            if (component[e.from] != component[e.to])
            {
                ++indegree[component[e.to]];
                closed[component[e.from]] = false;
            }
            else if (e.from == e.to || components[component[e.from]].size() > 1)
                cyclic = true;
        }
        std::queue<int> q;
        for (int i = 0; i < int(indegree.size()); ++i)
            if (!indegree[i])
                q.push(i);
        while (!q.empty())
        {
            int c = q.front();
            q.pop();
            order.push_back(c);
            for (int v : components[c])
                for (int ei : out[v])
                {
                    int d = component[edges[ei].to];
                    if (d != c && !--indegree[d])
                        q.push(d);
                }
        }
    }
    // SCC-local sparse BiCGSTAB. Singleton DAG nodes are exact substitutions;
    // memory is O(nodes + edges), including when a component is very large.
    bool solve(std::vector<double> rhs, std::vector<double> &x, const SWMM_TraceOptions &opt,
               bool known, std::vector<double> *trapped, double &residual, SWMM_TraceProgress cb,
               void *user) const
    {
        x.assign(n, 0);
        std::vector<int> local(n, -1);
        for (int c : order)
        {
            if (tick(cb, user, 0.5, "Solving trace components"))
                return false;
            const auto &vs = components[c];
            int count = int(vs.size());
            if (closed[c])
            {
                if (trapped)
                    for (int v : vs)
                        (*trapped)[v] += rhs[v];
                continue;
            }
            for (int k = 0; k < count; ++k)
                local[vs[k]] = k;
            auto apply = [&](const std::vector<double> &a)
            {
                std::vector<double> b = a;
                for (int i = 0; i < count; ++i)
                    for (int ei : out[vs[i]])
                    {
                        const auto &e = edges[ei];
                        if (component[e.to] == c && (!known || std::isfinite(e.delay)))
                            b[local[e.to]] -= e.p * a[i];
                    }
                return b;
            };
            auto dot = [](const auto &a, const auto &b)
            { return std::inner_product(a.begin(), a.end(), b.begin(), 0.0); };
            std::vector<double> b(count), sol(count), r(count), shadow, p(count), v(count);
            for (int i = 0; i < count; ++i)
                b[i] = rhs[vs[i]];
            if (count == 1)
            {
                double diagonal = 1;
                for (int ei : out[vs[0]])
                {
                    const auto &e = edges[ei];
                    if (e.to == vs[0] && (!known || std::isfinite(e.delay)))
                        diagonal -= e.p;
                }
                if (diagonal <= 0)
                    return false;
                sol[0] = b[0] / diagonal;
            }
            else
            {
                r = shadow = b;
                double rhoOld = 1, alpha = 1, omega = 1;
                const double tol = opt.solver_tolerance * std::max(1.0, std::sqrt(dot(b, b)));
                int iteration = 0;
                while (std::sqrt(dot(r, r)) > tol && iteration++ < opt.max_iterations)
                {
                    if (iteration % 32 == 0 && tick(cb, user, 0.5, "Solving circulating flow"))
                        return false;
                    double rho = dot(shadow, r);
                    if (std::abs(rho) < 1e-300 || std::abs(omega) < 1e-300)
                        return false;
                    double beta = (rho / rhoOld) * (alpha / omega);
                    for (int i = 0; i < count; ++i)
                        p[i] = r[i] + beta * (p[i] - omega * v[i]);
                    v = apply(p);
                    double denom = dot(shadow, v);
                    if (std::abs(denom) < 1e-300)
                        return false;
                    alpha = rho / denom;
                    std::vector<double> s(count);
                    for (int i = 0; i < count; ++i)
                        s[i] = r[i] - alpha * v[i];
                    if (std::sqrt(dot(s, s)) <= tol)
                    {
                        for (int i = 0; i < count; ++i)
                            sol[i] += alpha * p[i];
                        r = s;
                        break;
                    }
                    auto t = apply(s);
                    double tt = dot(t, t);
                    if (tt < 1e-300)
                        return false;
                    omega = dot(t, s) / tt;
                    for (int i = 0; i < count; ++i)
                    {
                        sol[i] += alpha * p[i] + omega * s[i];
                        r[i] = s[i] - omega * t[i];
                    }
                    rhoOld = rho;
                }
                auto check = apply(sol);
                double err = 0;
                for (int i = 0; i < count; ++i)
                    err = std::max(err, std::abs(check[i] - b[i]));
                residual = std::max(residual, err);
                if (err > tol * 10)
                    return false;
            }
            for (int i = 0; i < count; ++i)
            {
                if (!std::isfinite(sol[i]) || sol[i] < -opt.solver_tolerance * 10)
                    return false;
                int from = vs[i];
                x[from] = std::max(0.0, sol[i]);
                for (int ei : out[from])
                {
                    const auto &e = edges[ei];
                    if (component[e.to] != c && (!known || std::isfinite(e.delay)))
                        rhs[e.to] += x[from] * e.p;
                }
            }
        }
        return true;
    }
};
} // namespace

std::string FlowTracer::cacheKey(const char *digest) const
{
    std::ostringstream s;
    s << "trace:2:2:" << (digest ? digest : "") << std::hexfloat << ':' << options.flow_epsilon_m3s
      << ':' << options.velocity_epsilon_mps << ':' << options.reversal_dominance;
    for (const auto &n : nodes)
        s << '|' << n.id.size() << ':' << n.id << ':' << n.type << ':' << n.flags;
    for (const auto &l : links)
        s << '|' << l.id.size() << ':' << l.id << ':' << l.from << ':' << l.to << ':' << l.type
          << ':' << l.length;
    return s.str();
}
int FlowTracer::derive()
{
    for (auto &n : na)
    {
        n.outgoing_m3s = 0;
        n.residence_s = 0;
        n.flags = 0;
    }
    for (size_t i = 0; i < links.size(); ++i)
    {
        auto &a = la[i];
        const auto &l = links[i];
        a.flags = 0;
        if (!std::isfinite(a.net_flow_m3s) || !std::isfinite(a.absolute_flow_m3s) ||
            a.absolute_flow_m3s < 0 || a.absolute_flow_m3s + 1e-12 < std::abs(a.net_flow_m3s) ||
            !std::isfinite(a.absolute_velocity_mps) || a.absolute_velocity_mps < 0)
        {
            error = "Invalid or incomplete hydraulic averages";
            return SWMM_TRACE_INVALID;
        }
        a.direction =
            std::abs(a.net_flow_m3s) > options.flow_epsilon_m3s ? (a.net_flow_m3s > 0 ? 1 : -1) : 0;
        if (!a.direction)
            a.flags |= SWMM_TRACE_NO_DIRECTION;
        if (a.absolute_flow_m3s > options.flow_epsilon_m3s &&
            std::abs(a.net_flow_m3s) / a.absolute_flow_m3s < options.reversal_dominance)
            a.flags |= SWMM_TRACE_REVERSAL;
        a.travel_s = l.type == 0
                         ? (l.length > 0 && a.absolute_velocity_mps > options.velocity_epsilon_mps
                                ? l.length / a.absolute_velocity_mps
                                : nan)
                         : 0;
        if (!std::isfinite(a.travel_s))
            a.flags |= SWMM_TRACE_UNKNOWN_TIME;
        if (a.direction)
            na[a.direction > 0 ? l.from : l.to].outgoing_m3s += std::abs(a.net_flow_m3s);
    }
    for (size_t i = 0; i < nodes.size(); ++i)
    {
        auto &a = na[i];
        for (double v :
             {a.volume_m3, a.inflow_m3s, a.lateral_in_m3s, a.withdrawal_m3s, a.overflow_m3s})
            if (!std::isfinite(v) || v < 0)
            {
                error = "Invalid node averages";
                return SWMM_TRACE_INVALID;
            }
        double loss = (nodes[i].flags & (SWMM_TRACE_PONDING | SWMM_TRACE_EXTERNAL_EXCHANGE))
                          ? 0
                          : a.overflow_m3s;
        if (nodes[i].type == 2)
        {
            double q = a.outgoing_m3s + a.withdrawal_m3s + loss;
            a.residence_s =
                q > options.flow_epsilon_m3s ? a.volume_m3 / q : (a.volume_m3 > 0 ? nan : 0);
        }
        if (nodes[i].flags)
        {
            a.flags |= SWMM_TRACE_APPROXIMATE;
            if (nodes[i].type == 2)
                a.residence_s = nan;
        }
        if (!std::isfinite(a.residence_s))
            a.flags |= SWMM_TRACE_UNKNOWN_TIME;
    }
    prepared = true;
    return SWMM_TRACE_OK;
}

int FlowTracer::prepare(const char *path, const char *cache, const char *digest,
                        SWMM_TraceProgress cb, void *user)
{
    prepared = false;
    error.clear();
    if (!path || !*path)
    {
        error = "Missing output path";
        return SWMM_TRACE_INVALID;
    }
    const std::string key = cacheKey(digest);
    if (tick(cb, user, 0, "Opening hydraulic output"))
        return SWMM_TRACE_CANCELLED;
    std::error_code ec;
    auto size = std::filesystem::file_size(path, ec);
    if (ec)
    {
        error = ec.message();
        return SWMM_TRACE_IO;
    }
    auto stamp = std::filesystem::last_write_time(path, ec);
    if (ec)
    {
        error = ec.message();
        return SWMM_TRACE_IO;
    }
    if (cache && *cache && digest && *digest && readCache(cache, key))
    {
        info.cache_loaded = 1;
        return derive();
    }
    OutputReader reader;
    if (!reader.open(path) || reader.error_code() != 0)
    {
        error = "A completed, successful output is required";
        return SWMM_TRACE_IO;
    }
    if (reader.node_count() != int(nodes.size()) || reader.link_count() != int(links.size()))
    {
        error = "Output and model object counts differ";
        return SWMM_TRACE_MISMATCH;
    }
    std::unordered_map<std::string, int> ni, li;
    for (int i = 0; i < reader.node_count(); ++i)
        ni[reader.node_id(i)] = i;
    for (int i = 0; i < reader.link_count(); ++i)
        li[reader.link_id(i)] = i;
    std::vector<int> nm, lm;
    for (const auto &n : nodes)
    {
        auto p = ni.find(n.id);
        if (p == ni.end())
        {
            error = "Missing output node: " + n.id;
            return SWMM_TRACE_MISMATCH;
        }
        nm.push_back(p->second);
    }
    for (const auto &l : links)
    {
        auto p = li.find(l.id);
        if (p == li.end())
        {
            error = "Missing output link: " + l.id;
            return SWMM_TRACE_MISMATCH;
        }
        lm.push_back(p->second);
    }
    info = {2,
            2,
            int(nodes.size()),
            int(links.size()),
            reader.period_count(),
            reader.flow_units(),
            0,
            0,
            0,
            0};
    if (info.periods < 1 || info.source_flow_units < 0 || info.source_flow_units > 5)
    {
        error = "No valid reporting periods or units";
        return SWMM_TRACE_INVALID;
    }
    constexpr double flowFactors[] = {0.028316846592, 0.0000630901964,     0.04381263638888889, 1,
                                      0.001,          0.011574074074074073};
    double fq = flowFactors[info.source_flow_units], fl = info.source_flow_units < 3 ? 0.3048 : 1,
           fvol = fl * fl * fl;
    na.assign(nodes.size(), {});
    la.assign(links.size(), {});
    struct Sample
    {
        std::vector<float> q, v, lv, nv, lat, in, ov;
    };
    auto sample = [&](int t, Sample &s)
    {
        s.q.resize(links.size());
        s.v.resize(links.size());
        s.lv.resize(links.size());
        s.nv.resize(nodes.size());
        s.lat.resize(nodes.size());
        s.in.resize(nodes.size());
        s.ov.resize(nodes.size());
        bool ok = reader.get_node_result(t, SWMM_OUT_NODE_VOLUME, s.nv.data()) &&
                  reader.get_node_result(t, SWMM_OUT_NODE_LATERAL_INFLOW, s.lat.data()) &&
                  reader.get_node_result(t, SWMM_OUT_NODE_TOTAL_INFLOW, s.in.data()) &&
                  reader.get_node_result(t, SWMM_OUT_NODE_OVERFLOW, s.ov.data());
        if (!links.empty())
            ok = ok && reader.get_link_result(t, SWMM_OUT_LINK_FLOW, s.q.data()) &&
                 reader.get_link_result(t, SWMM_OUT_LINK_VELOCITY, s.v.data()) &&
                 reader.get_link_result(t, SWMM_OUT_LINK_VOLUME, s.lv.data());
        for (const auto *a : {&s.q, &s.v, &s.lv, &s.nv, &s.lat, &s.in, &s.ov})
            for (float x : *a)
                ok = ok && std::isfinite(x);
        return ok;
    };
    Sample prev, cur;
    double last = 0;
    for (int t = 0; t < info.periods; ++t)
    {
        if (tick(cb, user, 0.05 + 0.8 * double(t) / info.periods,
                 "Averaging reported flows and velocities"))
            return SWMM_TRACE_CANCELLED;
        double date;
        if (!sample(t, cur) || !reader.get_period_time(t, &date) || !std::isfinite(date))
        {
            error = "Missing or invalid reported hydraulic data";
            return SWMM_TRACE_IO;
        }
        if (t == 0)
        {
            info.first_report_date = date;
            for (size_t i = 0; i < nodes.size(); ++i)
                na[i].first_volume_m3 = cur.nv[nm[i]] * fvol;
            for (size_t i = 0; i < links.size(); ++i)
                la[i].first_volume_m3 = cur.lv[lm[i]] * fvol;
        }
        if (t || info.periods == 1)
        {
            double dt = t ? (date - last) * 86400 : 1;
            if (!(dt > 0))
            {
                error = "Reporting timestamps must increase";
                return SWMM_TRACE_INVALID;
            }
            const auto &p = t ? prev : cur;
            for (size_t i = 0; i < links.size(); ++i)
            {
                int j = lm[i];
                auto &a = la[i];
                double x = p.q[j] * fq, y = cur.q[j] * fq;
                double pos = positiveIntegral(x, y, dt), neg = positiveIntegral(-x, -y, dt);
                a.forward_volume_m3 += pos;
                a.reverse_volume_m3 += neg;
                a.net_flow_m3s += (x + y) * dt * 0.5;
                a.absolute_flow_m3s += pos + neg;
                a.absolute_velocity_mps += positiveIntegral(p.v[j] * fl, cur.v[j] * fl, dt) +
                                           positiveIntegral(-p.v[j] * fl, -cur.v[j] * fl, dt);
            }
            for (size_t i = 0; i < nodes.size(); ++i)
            {
                int j = nm[i];
                auto &a = na[i];
                a.volume_m3 += (p.nv[j] + cur.nv[j]) * fvol * dt * 0.5;
                a.inflow_m3s += positiveIntegral(p.in[j] * fq, cur.in[j] * fq, dt);
                a.overflow_m3s += positiveIntegral(p.ov[j] * fq, cur.ov[j] * fq, dt);
                a.lateral_in_m3s += positiveIntegral(p.lat[j] * fq, cur.lat[j] * fq, dt);
                a.withdrawal_m3s += positiveIntegral(-p.lat[j] * fq, -cur.lat[j] * fq, dt);
            }
        }
        info.last_report_date = date;
        last = date;
        std::swap(prev, cur);
    }
    info.duration_s = (info.last_report_date - info.first_report_date) * 86400;
    double duration = info.periods == 1 ? 1 : info.duration_s;
    for (size_t i = 0; i < nodes.size(); ++i)
    {
        auto &a = na[i];
        a.volume_m3 /= duration;
        a.inflow_m3s /= duration;
        a.overflow_m3s /= duration;
        a.lateral_in_m3s /= duration;
        a.withdrawal_m3s /= duration;
        a.last_volume_m3 = prev.nv[nm[i]] * fvol;
    }
    for (size_t i = 0; i < links.size(); ++i)
    {
        auto &a = la[i];
        a.net_flow_m3s /= duration;
        a.absolute_flow_m3s /= duration;
        a.absolute_velocity_mps /= duration;
        a.last_volume_m3 = prev.lv[lm[i]] * fvol;
        if (info.periods == 1)
            a.forward_volume_m3 = a.reverse_volume_m3 = 0;
    }
    if (size != std::filesystem::file_size(path, ec) ||
        stamp != std::filesystem::last_write_time(path, ec) || ec)
    {
        error = "Output changed during preparation";
        return SWMM_TRACE_MISMATCH;
    }
    int result = derive();
    if (result)
        return result;
    if (tick(cb, user, 0.9, "Saving reusable hydraulic averages"))
    {
        prepared = false;
        return SWMM_TRACE_CANCELLED;
    }
    if (cache && *cache)
    {
        if (!digest || !*digest)
        {
            error = "A content fingerprint is required for a cache";
            return SWMM_TRACE_INVALID;
        }
        if (!writeCache(cache, key))
        {
            error = "Cannot write hydraulic cache";
            return SWMM_TRACE_IO;
        }
    }
    tick(cb, user, 1, "Hydraulic averages ready");
    return SWMM_TRACE_OK;
}

int FlowTracer::estimate(int direction, int seed, SWMM_TraceValue *nv, SWMM_TraceValue *lv,
                         SWMM_TraceSummary &summary, SWMM_TraceProgress cb, void *user)
{
    error.clear();
    summary = {};
    int n = int(nodes.size());
    Graph g(n);
    std::vector<double> incoming(n), denom(n);
    for (size_t i = 0; i < links.size(); ++i)
        if (la[i].direction)
        {
            int to = la[i].direction > 0 ? links[i].to : links[i].from;
            incoming[to] += std::abs(la[i].net_flow_m3s);
        }
    for (int i = 0; i < n; ++i)
    {
        const auto &a = na[i];
        int flags = nodes[i].flags;
        double sink =
            direction == SWMM_TRACE_UPSTREAM
                ? a.lateral_in_m3s
                : a.withdrawal_m3s + ((flags & (SWMM_TRACE_PONDING | SWMM_TRACE_EXTERNAL_EXCHANGE))
                                          ? 0
                                          : a.overflow_m3s);
        denom[i] = (direction == SWMM_TRACE_UPSTREAM ? incoming[i] : a.outgoing_m3s) + sink;
        g.kind[i] = direction == SWMM_TRACE_UPSTREAM ? SWMM_TRACE_SOURCE : SWMM_TRACE_LOSS;
        if (denom[i] <= options.flow_epsilon_m3s)
        {
            g.sink[i] = 1;
            g.kind[i] = direction == SWMM_TRACE_UPSTREAM
                            ? SWMM_TRACE_UNRESOLVED
                            : (nodes[i].type == 1 && !(flags & SWMM_TRACE_ROUTED_OUTFALL)
                                   ? SWMM_TRACE_OUTFALL
                                   : SWMM_TRACE_RETAINED);
            if (flags & (SWMM_TRACE_EXTERNAL_EXCHANGE | SWMM_TRACE_ROUTED_OUTFALL))
                g.kind[i] = SWMM_TRACE_UNRESOLVED;
        }
        else
            g.sink[i] = sink / denom[i];
    }
    for (int i = 0; i < int(links.size()); ++i)
        if (la[i].direction)
        {
            const auto &l = links[i];
            int a = la[i].direction > 0 ? l.from : l.to, b = la[i].direction > 0 ? l.to : l.from;
            double delay = la[i].travel_s + na[a].residence_s;
            int from = direction == SWMM_TRACE_UPSTREAM ? b : a,
                to = direction == SWMM_TRACE_UPSTREAM ? a : b;
            if (denom[from] > options.flow_epsilon_m3s)
                g.add({from, to, i, std::abs(la[i].net_flow_m3s) / denom[from], delay});
        }
    g.partition();
    std::vector<double> rhs(n), x, k, z, trap(n);
    rhs[seed] = 1;
    bool cancelled = false;
    struct Progress
    {
        SWMM_TraceProgress cb;
        void *user;
        bool *cancelled;
    } pc{cb, user, &cancelled};
    auto proxy = [](double p, const char *s, void *u) -> int
    {
        auto &c = *static_cast<Progress *>(u);
        if (c.cb && c.cb(p, s, c.user))
        {
            *c.cancelled = true;
            return 1;
        }
        return 0;
    };
    double residual = 0;
    if (!g.solve(rhs, x, options, false, &trap, residual, proxy, &pc) ||
        !g.solve(rhs, k, options, true, nullptr, residual, proxy, &pc))
    {
        error = cancelled ? "Analysis cancelled" : "Sparse trace solve did not converge";
        return cancelled ? SWMM_TRACE_CANCELLED : SWMM_TRACE_SOLVER;
    }
    std::fill(rhs.begin(), rhs.end(), 0);
    for (const auto &e : g.edges)
        if (std::isfinite(e.delay))
            rhs[e.to] += k[e.from] * e.p * e.delay;
    if (!g.solve(rhs, z, options, true, nullptr, residual, proxy, &pc))
    {
        error = cancelled ? "Analysis cancelled" : "Travel time solve did not converge";
        return cancelled ? SWMM_TRACE_CANCELLED : SWMM_TRACE_SOLVER;
    }
    std::vector<bool> reached(n);
    std::queue<int> queue;
    queue.push(seed);
    reached[seed] = true;
    while (!queue.empty())
    {
        int v = queue.front();
        queue.pop();
        for (int ei : g.out[v])
        {
            int w = g.edges[ei].to;
            if (!reached[w])
            {
                reached[w] = true;
                queue.push(w);
            }
        }
    }
    for (const auto &e : g.edges)
        if (reached[e.from] && g.component[e.from] == g.component[e.to] &&
            (e.from == e.to || g.components[g.component[e.from]].size() > 1))
        {
            summary.cyclic = 1;
            break;
        }
    auto value = [&](double ratio, double known, double moment, int flags)
    {
        SWMM_TraceValue v{};
        v.ratio = ratio;
        v.time_s = known > 0 ? moment / known : nan;
        v.from_time_s = v.to_time_s = nan;
        v.time_coverage = ratio > 0 ? std::clamp(known / ratio, 0.0, 1.0) : 0;
        v.flags = flags;
        v.terminal_kind = -1;
        if (!std::isfinite(v.time_s))
            v.flags |= SWMM_TRACE_UNKNOWN_TIME;
        else if (v.time_coverage < 1 - 1e-8)
            v.flags |= SWMM_TRACE_PARTIAL_TIME;
        return v;
    };
    for (int i = 0; i < n; ++i)
    {
        bool trapped = g.closed[g.component[i]] && reached[i];
        nv[i] = value(trapped ? nan : x[i], k[i], z[i],
                      na[i].flags | (reached[i] ? 0 : SWMM_TRACE_UNREACHABLE));
        if (reached[i])
            ++summary.reached_nodes;
        double absorbed = trapped ? trap[i] : x[i] * g.sink[i];
        int kind = trapped ? SWMM_TRACE_CIRCULATION : g.kind[i];
        nv[i].terminal_fraction = absorbed;
        nv[i].terminal_kind = kind;
        summary.terminal[kind] += absorbed;
        if (trapped)
            nv[i].flags |= SWMM_TRACE_TRAPPED;
    }
    for (size_t i = 0; i < links.size(); ++i)
        lv[i] = value(0, 0, 0, la[i].flags | SWMM_TRACE_UNREACHABLE);
    for (const auto &e : g.edges)
    {
        bool trapped = g.closed[g.component[e.from]] && reached[e.from];
        double ratio = x[e.from] * e.p;
        double known = std::isfinite(e.delay) ? k[e.from] * e.p : 0;
        double moment = known > 0 ? (z[e.from] * e.p + known * e.delay) : 0;
        auto &v = lv[e.link];
        v = value(trapped ? nan : ratio, known, moment,
                  la[e.link].flags | (reached[e.from] ? 0 : SWMM_TRACE_UNREACHABLE));
        if (reached[e.from])
            ++summary.reached_links;
        if (trapped)
            v.flags |= SWMM_TRACE_TRAPPED;
        double near = k[e.from] > 0 ? z[e.from] / k[e.from] : nan,
               far = known > 0 ? moment / known : nan;
        bool fromIsNear = (direction == SWMM_TRACE_DOWNSTREAM) == (la[e.link].direction > 0);
        v.from_time_s = fromIsNear ? near : far;
        v.to_time_s = fromIsNear ? far : near;
    }
    double total = 0;
    for (double f : summary.terminal)
        total += f;
    summary.accounting_error = total - 1;
    summary.solver_residual = residual;
    if (std::abs(summary.accounting_error) > options.solver_tolerance * 100)
    {
        error = "Trace terminal accounting did not close";
        return SWMM_TRACE_SOLVER;
    }
    for (size_t i = 0; i < links.size(); ++i)
    {
        const auto &l = links[i];
        const auto &a = la[i];
        bool f = reached[l.from], t = reached[l.to];
        if (f != t)
        {
            summary.boundary_in_m3 += t ? a.forward_volume_m3 : a.reverse_volume_m3;
            summary.boundary_out_m3 += f ? a.forward_volume_m3 : a.reverse_volume_m3;
        }
        if (f && t)
            summary.storage_change_m3 += a.last_volume_m3 - a.first_volume_m3;
    }
    for (int i = 0; i < n; ++i)
        if (reached[i])
        {
            const auto &a = na[i];
            summary.lateral_in_m3 += a.lateral_in_m3s * info.duration_s;
            summary.withdrawal_m3 += a.withdrawal_m3s * info.duration_s;
            if (!(nodes[i].flags & (SWMM_TRACE_PONDING | SWMM_TRACE_EXTERNAL_EXCHANGE)))
                summary.known_loss_m3 += a.overflow_m3s * info.duration_s;
            summary.storage_change_m3 += a.last_volume_m3 - a.first_volume_m3;
            if (nodes[i].type == 1 &&
                !(nodes[i].flags & (SWMM_TRACE_ROUTED_OUTFALL | SWMM_TRACE_EXTERNAL_EXCHANGE)))
            {
                // The .out does not report signed boundary exchange separately.
                // Include a net boundary estimate; retain the partial-budget label.
                double net = (incoming[i] + a.lateral_in_m3s - a.outgoing_m3s - a.withdrawal_m3s) *
                             info.duration_s;
                summary.boundary_out_m3 += std::max(0.0, net);
                summary.boundary_in_m3 += std::max(0.0, -net);
            }
        }
    summary.partial_balance_residual_m3 =
        info.duration_s > 0
            ? summary.boundary_in_m3 + summary.lateral_in_m3 - summary.boundary_out_m3 -
                  summary.withdrawal_m3 - summary.known_loss_m3 - summary.storage_change_m3
            : nan;
    tick(cb, user, 1, "Trace ready");
    return SWMM_TRACE_OK;
}
} // namespace openswmm::trace
