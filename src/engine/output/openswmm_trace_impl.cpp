// SPDX-License-Identifier: Apache-2.0
#include "FlowTracer.hpp"
#include <algorithm>
#include <cmath>
#include <exception>
#include <memory>
#include <unordered_set>
using openswmm::trace::FlowTracer;
namespace
{
thread_local std::string lastError;
template <class F> int guarded(SWMM_Trace h, F &&f)
{
    try
    {
        return f();
    }
    catch (const std::exception &e)
    {
        if (h)
            static_cast<FlowTracer *>(h)->error = e.what();
        else
            lastError = e.what();
        return SWMM_TRACE_INVALID;
    }
    catch (...)
    {
        lastError = "Unexpected trace failure";
        return SWMM_TRACE_INVALID;
    }
}
} // namespace
extern "C"
{
    void swmm_trace_default_options(SWMM_TraceOptions *o)
    {
        if (o)
            *o = {1e-9, 1e-6, 0.8, 1e-11, 10000};
    }
    int swmm_trace_create(const SWMM_TraceNodeInput *ns, int nn, const SWMM_TraceLinkInput *ls,
                          int nl, const SWMM_TraceOptions *o, SWMM_Trace *out)
    {
        if (out)
            *out = nullptr;
        return guarded(
            nullptr,
            [&]() -> int
            {
                if (!out || nn <= 0 || nl < 0 || !ns || (nl && !ls))
                    return SWMM_TRACE_INVALID;
                auto t = std::make_unique<FlowTracer>();
                swmm_trace_default_options(&t->options);
                if (o)
                    t->options = *o;
                const auto &opt = t->options;
                if (!std::isfinite(opt.flow_epsilon_m3s) || opt.flow_epsilon_m3s <= 0 ||
                    !std::isfinite(opt.velocity_epsilon_mps) || opt.velocity_epsilon_mps <= 0 ||
                    !std::isfinite(opt.reversal_dominance) || opt.reversal_dominance < 0 ||
                    opt.reversal_dominance > 1 || !std::isfinite(opt.solver_tolerance) ||
                    opt.solver_tolerance <= 0 || opt.solver_tolerance > 0.01 ||
                    opt.max_iterations < 1)
                    return SWMM_TRACE_INVALID;
                std::unordered_set<std::string> ids;
                for (int i = 0; i < nn; ++i)
                {
                    if (!ns[i].id || !*ns[i].id || !ids.insert(ns[i].id).second || ns[i].type < 0 ||
                        ns[i].type > 3)
                        return SWMM_TRACE_INVALID;
                    t->nodes.push_back({ns[i].id, ns[i].type, ns[i].flags});
                }
                ids.clear();
                for (int i = 0; i < nl; ++i)
                {
                    const auto &l = ls[i];
                    if (!l.id || !*l.id || !ids.insert(l.id).second || l.from_node < 0 ||
                        l.from_node >= nn || l.to_node < 0 || l.to_node >= nn ||
                        !std::isfinite(l.length_m) || l.length_m < 0)
                        return SWMM_TRACE_INVALID;
                    t->links.push_back({l.id, l.from_node, l.to_node, l.type, l.length_m});
                }
                t->na.resize(nn);
                t->la.resize(nl);
                t->info = {2, 2, nn, nl, 0, 0, 0, 0, 0, 0};
                *out = t.release();
                return SWMM_TRACE_OK;
            });
    }
    void swmm_trace_close(SWMM_Trace h) { delete static_cast<FlowTracer *>(h); }
    const char *swmm_trace_error(SWMM_Trace h)
    {
        return h ? static_cast<FlowTracer *>(h)->error.c_str() : lastError.c_str();
    }
    int swmm_trace_prepare(SWMM_Trace h, const char *out, const char *cache, const char *digest,
                           SWMM_TraceProgress cb, void *u)
    {
        if (!h)
            return SWMM_TRACE_INVALID;
#if !defined(OPENSWMM_HAS_HDF5_MODEL)
        if (cache && *cache)
        {
            static_cast<FlowTracer *>(h)->error = "Engine built without HDF5 cache support";
            return SWMM_TRACE_NO_HDF5;
        }
#endif
        return guarded(
            h, [&]() -> int
            { return static_cast<FlowTracer *>(h)->prepare(out, cache, digest, cb, u); });
    }
    int swmm_trace_set_averages(SWMM_Trace h, const SWMM_TraceNodeAverage *n, int nn,
                                const SWMM_TraceLinkAverage *l, int nl, const SWMM_TraceInfo *info)
    {
        if (!h || !n || (nl && !l))
            return SWMM_TRACE_INVALID;
        return guarded(h,
                       [&]() -> int
                       {
                           auto &t = *static_cast<FlowTracer *>(h);
                           if (nn != int(t.nodes.size()) || nl != int(t.links.size()))
                               return SWMM_TRACE_INVALID;
                           t.prepared = false;
                           t.na.assign(n, n + nn);
                           t.la.clear();
                           if (nl)
                               t.la.assign(l, l + nl);
                           if (info)
                           {
                               if (info->duration_s < 0 || !std::isfinite(info->duration_s))
                                   return SWMM_TRACE_INVALID;
                               t.info = *info;
                               t.info.node_count = nn;
                               t.info.link_count = nl;
                               t.info.schema_version = t.info.algorithm_version = 2;
                           }
                           return t.derive();
                       });
    }
    int swmm_trace_get_info(SWMM_Trace h, SWMM_TraceInfo *i)
    {
        if (!h || !i)
            return SWMM_TRACE_INVALID;
        *i = static_cast<FlowTracer *>(h)->info;
        return 0;
    }
    int swmm_trace_get_averages(SWMM_Trace h, SWMM_TraceNodeAverage *n, int nn,
                                SWMM_TraceLinkAverage *l, int nl)
    {
        if (!h)
            return SWMM_TRACE_INVALID;
        auto &t = *static_cast<FlowTracer *>(h);
        if (!t.prepared || (n && nn < int(t.na.size())) || (l && nl < int(t.la.size())))
            return SWMM_TRACE_INVALID;
        if (n)
            std::copy(t.na.begin(), t.na.end(), n);
        if (l)
            std::copy(t.la.begin(), t.la.end(), l);
        return 0;
    }
    const char *swmm_trace_node_id(SWMM_Trace h, int i)
    {
        if (!h)
            return nullptr;
        auto &t = *static_cast<FlowTracer *>(h);
        return i >= 0 && i < int(t.nodes.size()) ? t.nodes[i].id.c_str() : nullptr;
    }
    const char *swmm_trace_link_id(SWMM_Trace h, int i)
    {
        if (!h)
            return nullptr;
        auto &t = *static_cast<FlowTracer *>(h);
        return i >= 0 && i < int(t.links.size()) ? t.links[i].id.c_str() : nullptr;
    }
    int swmm_trace_get_topology(SWMM_Trace h, SWMM_TraceNodeInput *n, int nn,
                                SWMM_TraceLinkInput *l, int nl)
    {
        if (!h)
            return SWMM_TRACE_INVALID;
        auto &t = *static_cast<FlowTracer *>(h);
        if ((n && nn < int(t.nodes.size())) || (l && nl < int(t.links.size())))
            return SWMM_TRACE_INVALID;
        if (n)
            for (size_t i = 0; i < t.nodes.size(); ++i)
            {
                const auto &v = t.nodes[i];
                n[i] = {v.id.c_str(), v.type, v.flags};
            }
        if (l)
            for (size_t i = 0; i < t.links.size(); ++i)
            {
                const auto &v = t.links[i];
                l[i] = {v.id.c_str(), v.from, v.to, v.type, v.length};
            }
        return 0;
    }
    int swmm_trace_estimate(SWMM_Trace h, int dir, int seed, SWMM_TraceValue *n, int nn,
                            SWMM_TraceValue *l, int nl, SWMM_TraceSummary *s, SWMM_TraceProgress cb,
                            void *u)
    {
        if (!h || !n || !s)
            return SWMM_TRACE_INVALID;
        return guarded(h,
                       [&]() -> int
                       {
                           auto &t = *static_cast<FlowTracer *>(h);
                           if (!t.prepared || dir < 0 || dir > 1 || seed < 0 ||
                               seed >= int(t.nodes.size()) || nn < int(t.nodes.size()) ||
                               nl < int(t.links.size()) || (!l && !t.links.empty()))
                               return SWMM_TRACE_INVALID;
                           return t.estimate(dir, seed, n, l, *s, cb, u);
                       });
    }
}
