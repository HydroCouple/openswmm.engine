/**
 * @file openswmm_inflows_impl.cpp
 * @brief C API implementation — external inflows, DWF, RDII.
 *
 * @see include/openswmm/engine/openswmm_inflows.h
 * @ingroup engine_api
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "openswmm_api_common.hpp"
#include "../../../include/openswmm/engine/openswmm_inflows.h"

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
