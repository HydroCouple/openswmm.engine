/**
 * @file openswmm_inflows_impl.cpp
 * @brief C API implementation — external inflows, DWF, RDII.
 *
 * @see include/openswmm/engine/openswmm_inflows.h
 * @ingroup engine_api
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "openswmm_api_common.hpp"
#include "../../../include/openswmm/engine/openswmm_inflows.h"
#include "../data/InflowData.hpp"
#include "../edit/ObjectDeleter.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace {

/// Copy a std::string into a NUL-terminated caller buffer, truncating safely.
inline void copy_to_buf(const std::string& src, char* buf, int buflen) {
    if (!buf || buflen <= 0) return;
    const int copy_len = std::min(static_cast<int>(src.size()), buflen - 1);
    std::memcpy(buf, src.c_str(), static_cast<std::size_t>(copy_len));
    buf[copy_len] = '\0';
}

/// Walk parameter entries + gage assignments in first-occurrence order and
/// emit a de-duplicated list of unit-hydrograph group names. Groups can be
/// introduced via either list (e.g. a gage-only group has no parameter rows
/// yet, a parameter-only group exists before its rain gage is attached).
inline std::vector<std::string> unique_uh_group_names(
    const openswmm::UnitHydData& uh) {
    std::vector<std::string> out;
    out.reserve(uh.entries.size() + uh.gage_assignments.size());
    auto seen_or_push = [&out](const std::string& name) {
        if (name.empty()) return;
        for (const auto& s : out) if (s == name) return;
        out.push_back(name);
    };
    for (const auto& e : uh.entries)          seen_or_push(e.name);
    for (const auto& g : uh.gage_assignments) seen_or_push(g);
    return out;
}

/// Rebuild the inflow solver's per-step SoA cache from the live context so a
/// runtime inflow edit takes effect on the next step. The solver caches
/// ext/DWF definitions at start(); editing ctx alone leaves the cache stale.
/// No-op until the simulation is running (pre-start edits flow through
/// InflowSolver::init at start()).
inline void refresh_inflows_if_running(SWMM_Engine engine,
                                       openswmm::SimulationContext& ctx) {
    if (ctx.state != openswmm::EngineState::BUILDING &&
        ctx.state != openswmm::EngineState::OPENED) {
        to_engine(engine)->inflowSolver().init(ctx);
    }
}

}  // namespace

extern "C" {

// ============================================================================
// External inflows
// ============================================================================

SWMM_ENGINE_API int swmm_ext_inflow_add(SWMM_Engine engine, int node_idx, const char* constituent,
                                          const char* ts_name, const char* type,
                                          double m_factor, double s_factor, double baseline,
                                          const char* pattern) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(node_idx >= 0 && node_idx < ctx.n_nodes());
    if (!constituent) return SWMM_ERR_BADPARAM;

    ctx.ext_inflows.add(
        node_idx,
        constituent,
        ts_name   ? ts_name   : "",
        type      ? type      : "FLOW",
        m_factor,
        s_factor,
        baseline,
        pattern   ? pattern   : ""
    );

    refresh_inflows_if_running(engine, ctx);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_ext_inflow_set_scale(SWMM_Engine engine, int entry_idx,
                                                double scale) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(entry_idx >= 0 && entry_idx < ctx.ext_inflows.count());
    ctx.ext_inflows.s_factor[static_cast<std::size_t>(entry_idx)] = scale;
    refresh_inflows_if_running(engine, ctx);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_ext_inflow_set_baseline(SWMM_Engine engine, int entry_idx,
                                                   double baseline) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(entry_idx >= 0 && entry_idx < ctx.ext_inflows.count());
    ctx.ext_inflows.baseline[static_cast<std::size_t>(entry_idx)] = baseline;
    refresh_inflows_if_running(engine, ctx);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_ext_inflow_get(SWMM_Engine engine, int entry_idx,
                                          int* node_idx,
                                          char* constituent_buf, int constituent_buflen,
                                          char* ts_buf,          int ts_buflen,
                                          char* type_buf,        int type_buflen,
                                          double* m_factor, double* s_factor, double* baseline,
                                          char* pattern_buf,     int pattern_buflen) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(entry_idx >= 0 && entry_idx < ctx.ext_inflows.count());
    if (!node_idx ||
        !constituent_buf || constituent_buflen <= 0 ||
        !ts_buf          || ts_buflen          <= 0 ||
        !type_buf        || type_buflen        <= 0 ||
        !m_factor || !s_factor || !baseline ||
        !pattern_buf     || pattern_buflen     <= 0)
        return SWMM_ERR_BADPARAM;

    const auto u = static_cast<std::size_t>(entry_idx);
    const auto& ei = ctx.ext_inflows;
    *node_idx = ei.node_idx[u];
    *m_factor = ei.m_factor[u];
    *s_factor = ei.s_factor[u];
    *baseline = ei.baseline[u];
    copy_to_buf(ei.constituent[u],  constituent_buf, constituent_buflen);
    copy_to_buf(ei.ts_name[u],      ts_buf,          ts_buflen);
    copy_to_buf(ei.inflow_type[u],  type_buf,        type_buflen);
    copy_to_buf(ei.pattern_name[u], pattern_buf,     pattern_buflen);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_ext_inflow_remove(SWMM_Engine engine, int entry_idx) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(entry_idx >= 0 && entry_idx < ctx.ext_inflows.count());
    ctx.ext_inflows.erase(entry_idx);
    refresh_inflows_if_running(engine, ctx);
    return SWMM_OK;
}

// ============================================================================
// Dry weather flow
// ============================================================================

SWMM_ENGINE_API int swmm_dwf_add(SWMM_Engine engine, int node_idx, const char* constituent,
                                   double avg_value, const char* pat1, const char* pat2,
                                   const char* pat3, const char* pat4) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(node_idx >= 0 && node_idx < ctx.n_nodes());
    if (!constituent) return SWMM_ERR_BADPARAM;

    ctx.dwf_inflows.add(
        node_idx,
        constituent,
        avg_value,
        pat1 ? pat1 : "",
        pat2 ? pat2 : "",
        pat3 ? pat3 : "",
        pat4 ? pat4 : ""
    );

    refresh_inflows_if_running(engine, ctx);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_dwf_get(SWMM_Engine engine, int entry_idx,
                                   int* node_idx,
                                   char* constituent_buf, int constituent_buflen,
                                   double* avg_value,
                                   char* pat1_buf, int pat1_buflen,
                                   char* pat2_buf, int pat2_buflen,
                                   char* pat3_buf, int pat3_buflen,
                                   char* pat4_buf, int pat4_buflen) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(entry_idx >= 0 && entry_idx < ctx.dwf_inflows.count());
    if (!node_idx ||
        !constituent_buf || constituent_buflen <= 0 ||
        !avg_value       ||
        !pat1_buf        || pat1_buflen        <= 0 ||
        !pat2_buf        || pat2_buflen        <= 0 ||
        !pat3_buf        || pat3_buflen        <= 0 ||
        !pat4_buf        || pat4_buflen        <= 0)
        return SWMM_ERR_BADPARAM;

    const auto u = static_cast<std::size_t>(entry_idx);
    const auto& dw = ctx.dwf_inflows;
    *node_idx  = dw.node_idx[u];
    *avg_value = dw.avg_value[u];
    copy_to_buf(dw.constituent[u], constituent_buf, constituent_buflen);
    copy_to_buf(dw.pat1[u],        pat1_buf,        pat1_buflen);
    copy_to_buf(dw.pat2[u],        pat2_buf,        pat2_buflen);
    copy_to_buf(dw.pat3[u],        pat3_buf,        pat3_buflen);
    copy_to_buf(dw.pat4[u],        pat4_buf,        pat4_buflen);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_dwf_remove(SWMM_Engine engine, int entry_idx) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(entry_idx >= 0 && entry_idx < ctx.dwf_inflows.count());
    ctx.dwf_inflows.erase(entry_idx);
    refresh_inflows_if_running(engine, ctx);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_dwf_set_baseline(SWMM_Engine engine, int entry_idx,
                                            double avg_value) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(entry_idx >= 0 && entry_idx < ctx.dwf_inflows.count());
    ctx.dwf_inflows.avg_value[static_cast<std::size_t>(entry_idx)] = avg_value;
    refresh_inflows_if_running(engine, ctx);
    return SWMM_OK;
}

// ============================================================================
// RDII
// ============================================================================

SWMM_ENGINE_API int swmm_rdii_add(SWMM_Engine engine, int node_idx, const char* uh_name, double area) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(node_idx >= 0 && node_idx < ctx.n_nodes());
    if (!uh_name) return SWMM_ERR_BADPARAM;

    ctx.rdii_assigns.add(node_idx, uh_name, area);

    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_rdii_get(SWMM_Engine engine, int entry_idx,
                                    int* node_idx, char* uh_buf, int buflen,
                                    double* area) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(entry_idx >= 0 && entry_idx < ctx.rdii_assigns.count());
    if (!node_idx || !uh_buf || buflen <= 0 || !area) return SWMM_ERR_BADPARAM;

    const auto u = static_cast<std::size_t>(entry_idx);
    *node_idx = ctx.rdii_assigns.node_idx[u];
    *area     = ctx.rdii_assigns.sewer_area[u];
    copy_to_buf(ctx.rdii_assigns.uh_name[u], uh_buf, buflen);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_rdii_remove(SWMM_Engine engine, int entry_idx) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(entry_idx >= 0 && entry_idx < ctx.rdii_assigns.count());
    ctx.rdii_assigns.erase(entry_idx);
    return SWMM_OK;
}

// ============================================================================
// Unit hydrographs ([HYDROGRAPHS])
// ============================================================================

SWMM_ENGINE_API int swmm_hydrograph_add(SWMM_Engine engine, const char* uh_name,
                                          int month, int response,
                                          double r, double t, double k,
                                          double dmax, double drecov, double dinit) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    if (!uh_name) return SWMM_ERR_BADPARAM;
    if (response < 0 || response > 2) return SWMM_ERR_BADPARAM;
    if (month < -1 || month > 11)     return SWMM_ERR_BADPARAM;

    openswmm::UnitHydEntry e{};
    e.name     = uh_name;
    e.month    = month;
    e.response = response;
    e.r        = r;
    e.t        = t;
    e.k        = k;
    e.dmax     = dmax;
    e.drecov   = drecov;
    e.dinit    = dinit;
    ctx.unit_hyds.add(e);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_hydrograph_get(SWMM_Engine engine, int entry_idx,
                                          char* uh_buf, int buflen,
                                          int* month, int* response,
                                          double* r, double* t, double* k,
                                          double* dmax, double* drecov, double* dinit) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(entry_idx >= 0 && entry_idx < ctx.unit_hyds.count());
    if (!uh_buf || buflen <= 0 || !month || !response ||
        !r || !t || !k || !dmax || !drecov || !dinit)
        return SWMM_ERR_BADPARAM;

    const auto& e = ctx.unit_hyds.entries[static_cast<std::size_t>(entry_idx)];
    copy_to_buf(e.name, uh_buf, buflen);
    *month    = e.month;
    *response = e.response;
    *r        = e.r;
    *t        = e.t;
    *k        = e.k;
    *dmax     = e.dmax;
    *drecov   = e.drecov;
    *dinit    = e.dinit;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_hydrograph_count(SWMM_Engine engine) {
    if (!engine) return -1;
    return to_engine(engine)->context().unit_hyds.count();
}

SWMM_ENGINE_API int swmm_hydrograph_add_gage(SWMM_Engine engine,
                                               const char* uh_name,
                                               const char* gage_name) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    if (!uh_name || !gage_name) return SWMM_ERR_BADPARAM;
    ctx.unit_hyds.add_gage(uh_name, gage_name);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_hydrograph_get_gage(SWMM_Engine engine, int entry_idx,
                                               char* uh_buf, int uh_buflen,
                                               char* gage_buf, int gage_buflen) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    const int n = static_cast<int>(ctx.unit_hyds.gage_assignments.size());
    CHECK_INDEX(entry_idx >= 0 && entry_idx < n);
    if (!uh_buf || uh_buflen <= 0 || !gage_buf || gage_buflen <= 0)
        return SWMM_ERR_BADPARAM;

    const auto u = static_cast<std::size_t>(entry_idx);
    copy_to_buf(ctx.unit_hyds.gage_assignments[u], uh_buf,   uh_buflen);
    copy_to_buf(ctx.unit_hyds.gage_names[u],       gage_buf, gage_buflen);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_hydrograph_gage_count(SWMM_Engine engine) {
    if (!engine) return -1;
    return static_cast<int>(
        to_engine(engine)->context().unit_hyds.gage_assignments.size());
}

SWMM_ENGINE_API int swmm_hydrograph_group_count(SWMM_Engine engine) {
    if (!engine) return -1;
    const auto& uh = to_engine(engine)->context().unit_hyds;
    return static_cast<int>(unique_uh_group_names(uh).size());
}

SWMM_ENGINE_API int swmm_hydrograph_group_id(SWMM_Engine engine, int idx,
                                               char* buf, int buflen) {
    CHECK_HANDLE(engine);
    if (!buf || buflen <= 0) return SWMM_ERR_BADPARAM;
    const auto& uh = to_engine(engine)->context().unit_hyds;
    const auto names = unique_uh_group_names(uh);
    CHECK_INDEX(idx >= 0 && idx < static_cast<int>(names.size()));
    copy_to_buf(names[static_cast<std::size_t>(idx)], buf, buflen);
    return SWMM_OK;
}

// ============================================================================
// Exponential IA decay ([RDII_DECAY])
// ============================================================================

SWMM_ENGINE_API int swmm_rdii_decay_add(SWMM_Engine engine, const char* uh_name,
                                          int response,
                                          double k_dep, double k_0, double k_T,
                                          double T_ref, double theta_rec, double T_freeze,
                                          int snow_on, double snow_T, double snow_ddf) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    if (!uh_name) return SWMM_ERR_BADPARAM;
    if (response < 0 || response > 2) return SWMM_ERR_BADPARAM;
    if (k_dep < 0.0 || k_0 < 0.0 || k_T < 0.0) return SWMM_ERR_BADPARAM;
    if (snow_ddf < 0.0) return SWMM_ERR_BADPARAM;

    openswmm::RDIIDecayEntry e{};
    e.uh_name   = uh_name;
    e.response  = response;
    e.k_dep     = k_dep;
    e.k_0       = k_0;
    e.k_T       = k_T;
    e.T_ref     = T_ref;
    e.theta_rec = theta_rec;
    e.T_freeze  = T_freeze;
    e.snow_on   = (snow_on != 0);
    e.snow_T    = snow_T;
    e.snow_ddf  = snow_ddf;
    ctx.rdii_decay.add(e);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_rdii_decay_get(SWMM_Engine engine, int entry_idx,
                                          char* uh_buf, int buflen,
                                          int* response,
                                          double* k_dep, double* k_0, double* k_T,
                                          double* T_ref, double* theta_rec, double* T_freeze,
                                          int* snow_on, double* snow_T, double* snow_ddf) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(entry_idx >= 0 && entry_idx < ctx.rdii_decay.count());
    if (!uh_buf || buflen <= 0 || !response ||
        !k_dep || !k_0 || !k_T ||
        !T_ref || !theta_rec || !T_freeze ||
        !snow_on || !snow_T || !snow_ddf)
        return SWMM_ERR_BADPARAM;

    const auto& e = ctx.rdii_decay.entries[static_cast<std::size_t>(entry_idx)];
    copy_to_buf(e.uh_name, uh_buf, buflen);
    *response  = e.response;
    *k_dep     = e.k_dep;
    *k_0       = e.k_0;
    *k_T       = e.k_T;
    *T_ref     = e.T_ref;
    *theta_rec = e.theta_rec;
    *T_freeze  = e.T_freeze;
    *snow_on   = e.snow_on ? 1 : 0;
    *snow_T    = e.snow_T;
    *snow_ddf  = e.snow_ddf;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_rdii_decay_count(SWMM_Engine engine) {
    if (!engine) return -1;
    return to_engine(engine)->context().rdii_decay.count();
}

// ============================================================================
// Mutation surface (BS-02) — upsert + key-based remove + rename
// ============================================================================
//
// Helpers live here rather than in the anonymous namespace at the top of the
// file because they need to mutate `openswmm::UnitHydData` / `RDIIDecayData`
// / `RDIIAssignData` and reading those types adds noise to the file header.

namespace {

inline int find_uh_entry(const openswmm::UnitHydData& uh,
                          const std::string& name, int month, int response) {
    const auto n = uh.entries.size();
    for (std::size_t i = 0; i < n; ++i) {
        const auto& e = uh.entries[i];
        if (e.name == name && e.month == month && e.response == response)
            return static_cast<int>(i);
    }
    return -1;
}

inline int find_uh_gage_assignment(const openswmm::UnitHydData& uh,
                                    const std::string& name) {
    const auto n = uh.gage_assignments.size();
    for (std::size_t i = 0; i < n; ++i) {
        if (uh.gage_assignments[i] == name) return static_cast<int>(i);
    }
    return -1;
}

inline void erase_uh_gage_assignment(openswmm::UnitHydData& uh, std::size_t i) {
    uh.gage_assignments.erase(uh.gage_assignments.begin() + static_cast<std::ptrdiff_t>(i));
    uh.gage_names.erase(uh.gage_names.begin()             + static_cast<std::ptrdiff_t>(i));
}

inline int find_decay_entry(const openswmm::RDIIDecayData& dd,
                              const std::string& name, int response) {
    const auto n = dd.entries.size();
    for (std::size_t i = 0; i < n; ++i) {
        const auto& e = dd.entries[i];
        if (e.uh_name == name && e.response == response) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace

SWMM_ENGINE_API int swmm_hydrograph_set_rtk(SWMM_Engine engine, const char* uh_name,
                                              int month, int response,
                                              double r, double t, double k) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    if (!uh_name || !*uh_name) return SWMM_ERR_BADPARAM;
    if (response < 0 || response > 2) return SWMM_ERR_BADPARAM;
    if (month < -1 || month > 11)     return SWMM_ERR_BADPARAM;

    const std::string name(uh_name);
    const int existing = find_uh_entry(ctx.unit_hyds, name, month, response);
    if (existing >= 0) {
        auto& e = ctx.unit_hyds.entries[static_cast<std::size_t>(existing)];
        e.r = r; e.t = t; e.k = k;
        return SWMM_OK;
    }

    openswmm::UnitHydEntry e{};
    e.name     = name;
    e.month    = month;
    e.response = response;
    e.r = r; e.t = t; e.k = k;
    e.dmax = 0.0; e.drecov = 0.0; e.dinit = 0.0;
    ctx.unit_hyds.add(e);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_hydrograph_set_ia(SWMM_Engine engine, const char* uh_name,
                                             int month, int response,
                                             double dmax, double drecov, double dinit) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    if (!uh_name || !*uh_name) return SWMM_ERR_BADPARAM;
    if (response < 0 || response > 2) return SWMM_ERR_BADPARAM;
    if (month < -1 || month > 11)     return SWMM_ERR_BADPARAM;

    const std::string name(uh_name);
    const int existing = find_uh_entry(ctx.unit_hyds, name, month, response);
    if (existing >= 0) {
        auto& e = ctx.unit_hyds.entries[static_cast<std::size_t>(existing)];
        e.dmax = dmax; e.drecov = drecov; e.dinit = dinit;
        return SWMM_OK;
    }

    openswmm::UnitHydEntry e{};
    e.name     = name;
    e.month    = month;
    e.response = response;
    e.r = 0.0; e.t = 0.0; e.k = 0.0;
    e.dmax = dmax; e.drecov = drecov; e.dinit = dinit;
    ctx.unit_hyds.add(e);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_hydrograph_remove_entry(SWMM_Engine engine, const char* uh_name,
                                                   int month, int response) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    if (!uh_name || !*uh_name) return SWMM_ERR_BADPARAM;
    if (response < 0 || response > 2) return SWMM_ERR_BADPARAM;
    if (month < -1 || month > 11)     return SWMM_ERR_BADPARAM;

    const int i = find_uh_entry(ctx.unit_hyds, std::string(uh_name), month, response);
    if (i < 0) return SWMM_OK;
    auto& v = ctx.unit_hyds.entries;
    v.erase(v.begin() + i);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_hydrograph_remove_group(SWMM_Engine engine, const char* uh_name) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    if (!uh_name || !*uh_name) return SWMM_ERR_BADPARAM;
    const std::string name(uh_name);

    // Cascade mirrors swmm_hydrograph_group_rename's touch set (entries,
    // gage assignment, RDII_DECAY rows, RDII node assignments) — same four
    // stores, deleting matching rows instead of relabeling them.
    auto& uh = ctx.unit_hyds;
    auto& entries = uh.entries;
    entries.erase(
        std::remove_if(entries.begin(), entries.end(),
                       [&](const openswmm::UnitHydEntry& e) { return e.name == name; }),
        entries.end());

    const int gage_idx = find_uh_gage_assignment(uh, name);
    if (gage_idx >= 0) erase_uh_gage_assignment(uh, static_cast<std::size_t>(gage_idx));

    auto& decay = ctx.rdii_decay.entries;
    decay.erase(
        std::remove_if(decay.begin(), decay.end(),
                       [&](const openswmm::RDIIDecayEntry& e) { return e.uh_name == name; }),
        decay.end());

    auto& ra = ctx.rdii_assigns;
    for (int i = static_cast<int>(ra.uh_name.size()) - 1; i >= 0; --i) {
        if (ra.uh_name[static_cast<std::size_t>(i)] == name) ra.erase(i);
    }

    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_hydrograph_clear_group_months(SWMM_Engine engine, const char* uh_name) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    if (!uh_name || !*uh_name) return SWMM_ERR_BADPARAM;
    const std::string name(uh_name);

    auto& v = ctx.unit_hyds.entries;
    v.erase(
        std::remove_if(v.begin(), v.end(),
                       [&](const openswmm::UnitHydEntry& e) {
                           return e.name == name && e.month != -1;
                       }),
        v.end());
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_hydrograph_set_gage(SWMM_Engine engine, const char* uh_name,
                                               const char* gage_name) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    if (!uh_name || !*uh_name) return SWMM_ERR_BADPARAM;
    const std::string name(uh_name);
    const std::string gage = (gage_name && *gage_name) ? gage_name : "";

    auto& uh = ctx.unit_hyds;
    const int existing = find_uh_gage_assignment(uh, name);

    if (gage.empty()) {
        if (existing >= 0) erase_uh_gage_assignment(uh, static_cast<std::size_t>(existing));
        return SWMM_OK;
    }

    if (existing >= 0) {
        uh.gage_names[static_cast<std::size_t>(existing)] = gage;
    } else {
        uh.add_gage(name, gage);
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_hydrograph_group_rename(SWMM_Engine engine, int idx,
                                                   const char* new_id) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    if (!new_id || !*new_id) return SWMM_ERR_BADPARAM;

    auto& uh = ctx.unit_hyds;
    const auto names = unique_uh_group_names(uh);
    CHECK_INDEX(idx >= 0 && idx < static_cast<int>(names.size()));

    const std::string old_name = names[static_cast<std::size_t>(idx)];
    const std::string new_name(new_id);
    if (old_name == new_name) return SWMM_OK;

    for (const auto& s : names) {
        if (s == new_name) return SWMM_ERR_BADPARAM;
    }

    for (auto& e : uh.entries) {
        if (e.name == old_name) e.name = new_name;
    }
    for (auto& g : uh.gage_assignments) {
        if (g == old_name) g = new_name;
    }
    for (auto& e : ctx.rdii_decay.entries) {
        if (e.uh_name == old_name) e.uh_name = new_name;
    }
    auto& ra = ctx.rdii_assigns;
    for (std::size_t i = 0; i < ra.uh_name.size(); ++i) {
        if (ra.uh_name[i] == old_name) ra.uh_name[i] = new_name;
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_rdii_decay_set(SWMM_Engine engine, const char* uh_name,
                                          int response,
                                          double k_dep, double k_0, double k_T,
                                          double T_ref, double theta_rec, double T_freeze,
                                          int snow_on, double snow_T, double snow_ddf) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    if (!uh_name || !*uh_name) return SWMM_ERR_BADPARAM;
    if (response < 0 || response > 2) return SWMM_ERR_BADPARAM;
    if (k_dep < 0.0 || k_0 < 0.0 || k_T < 0.0) return SWMM_ERR_BADPARAM;
    if (snow_ddf < 0.0) return SWMM_ERR_BADPARAM;

    const std::string name(uh_name);
    const int existing = find_decay_entry(ctx.rdii_decay, name, response);
    if (existing >= 0) {
        auto& e = ctx.rdii_decay.entries[static_cast<std::size_t>(existing)];
        e.k_dep = k_dep; e.k_0 = k_0; e.k_T = k_T;
        e.T_ref = T_ref; e.theta_rec = theta_rec; e.T_freeze = T_freeze;
        e.snow_on = (snow_on != 0); e.snow_T = snow_T; e.snow_ddf = snow_ddf;
        return SWMM_OK;
    }

    openswmm::RDIIDecayEntry e{};
    e.uh_name   = name;
    e.response  = response;
    e.k_dep     = k_dep;
    e.k_0       = k_0;
    e.k_T       = k_T;
    e.T_ref     = T_ref;
    e.theta_rec = theta_rec;
    e.T_freeze  = T_freeze;
    e.snow_on   = (snow_on != 0);
    e.snow_T    = snow_T;
    e.snow_ddf  = snow_ddf;
    ctx.rdii_decay.add(e);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_rdii_decay_remove(SWMM_Engine engine, const char* uh_name,
                                             int response) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    if (!uh_name || !*uh_name) return SWMM_ERR_BADPARAM;
    if (response < 0 || response > 2) return SWMM_ERR_BADPARAM;

    const int i = find_decay_entry(ctx.rdii_decay, std::string(uh_name), response);
    if (i < 0) return SWMM_OK;
    auto& v = ctx.rdii_decay.entries;
    v.erase(v.begin() + i);
    return SWMM_OK;
}

// ============================================================================
// Count queries
// ============================================================================

SWMM_ENGINE_API int swmm_ext_inflow_count(SWMM_Engine engine) {
    if (!engine) return -1;
    return to_engine(engine)->context().ext_inflows.count();
}

SWMM_ENGINE_API int swmm_dwf_count(SWMM_Engine engine) {
    if (!engine) return -1;
    return to_engine(engine)->context().dwf_inflows.count();
}

SWMM_ENGINE_API int swmm_rdii_count(SWMM_Engine engine) {
    if (!engine) return -1;
    return to_engine(engine)->context().rdii_assigns.count();
}

} /* extern "C" */
