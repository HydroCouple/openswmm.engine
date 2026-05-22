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
                                          double T_ref, double theta_rec, double T_freeze) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    if (!uh_name) return SWMM_ERR_BADPARAM;
    if (response < 0 || response > 2) return SWMM_ERR_BADPARAM;
    if (k_dep < 0.0 || k_0 < 0.0 || k_T < 0.0) return SWMM_ERR_BADPARAM;

    openswmm::RDIIDecayEntry e{};
    e.uh_name   = uh_name;
    e.response  = response;
    e.k_dep     = k_dep;
    e.k_0       = k_0;
    e.k_T       = k_T;
    e.T_ref     = T_ref;
    e.theta_rec = theta_rec;
    e.T_freeze  = T_freeze;
    ctx.rdii_decay.add(e);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_rdii_decay_get(SWMM_Engine engine, int entry_idx,
                                          char* uh_buf, int buflen,
                                          int* response,
                                          double* k_dep, double* k_0, double* k_T,
                                          double* T_ref, double* theta_rec, double* T_freeze) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(entry_idx >= 0 && entry_idx < ctx.rdii_decay.count());
    if (!uh_buf || buflen <= 0 || !response ||
        !k_dep || !k_0 || !k_T ||
        !T_ref || !theta_rec || !T_freeze)
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
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_rdii_decay_count(SWMM_Engine engine) {
    if (!engine) return -1;
    return to_engine(engine)->context().rdii_decay.count();
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
