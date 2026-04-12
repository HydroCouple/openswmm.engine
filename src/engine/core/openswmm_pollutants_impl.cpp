/**
 * @file openswmm_pollutants_impl.cpp
 * @brief C API implementation — pollutant identity, creation, properties, quality injection.
 *
 * @see include/openswmm/engine/openswmm_pollutants.h
 * @ingroup engine_api
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "openswmm_api_common.hpp"
#include "../../../include/openswmm/engine/openswmm_pollutants.h"

extern "C" {

// ============================================================================
// Identity
// ============================================================================

SWMM_ENGINE_API int swmm_pollutant_count(SWMM_Engine engine) {
    if (!engine) return -1;
    return to_engine(engine)->context().n_pollutants();
}

SWMM_ENGINE_API int swmm_pollutant_index(SWMM_Engine engine, const char* id) {
    if (!engine || !id) return -1;
    return to_engine(engine)->context().pollutant_names.find(id);
}

SWMM_ENGINE_API const char* swmm_pollutant_id(SWMM_Engine engine, int idx) {
    if (!engine) return nullptr;
    const auto& ctx = to_engine(engine)->context();
    if (idx < 0 || idx >= ctx.n_pollutants()) return nullptr;
    return ctx.pollutant_names.name_of(idx).c_str();
}

// ============================================================================
// Creation (BUILDING state only)
// ============================================================================

SWMM_ENGINE_API int swmm_pollutant_add(SWMM_Engine engine, const char* id, int units) {
    CHECK_HANDLE(engine);
    if (!id) return SWMM_ERR_BADPARAM;

    auto& ctx = to_engine(engine)->context();
    if (ctx.state != openswmm::EngineState::BUILDING)
        return SWMM_ERR_LIFECYCLE;

    // Check for duplicate ID
    if (ctx.pollutant_names.find(id) >= 0)
        return SWMM_ERR_BADPARAM;

    // Add to name index
    int idx = ctx.pollutant_names.add(id);

    // Resize pollutant definition arrays
    int n = ctx.pollutant_names.size();
    ctx.pollutants.resize_pollutants(n);

    // Set units for the new pollutant
    ctx.pollutants.units[static_cast<std::size_t>(idx)] = static_cast<openswmm::MassUnits>(units);

    return SWMM_OK;
}

// ============================================================================
// Property setters
// ============================================================================

SWMM_ENGINE_API int swmm_pollutant_set_kdecay(SWMM_Engine engine, int idx, double k) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_pollutants());
    ctx.pollutants.k_decay[static_cast<std::size_t>(idx)] = k;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_pollutant_set_rain_conc(SWMM_Engine engine, int idx, double conc) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_pollutants());
    ctx.pollutants.c_rain[static_cast<std::size_t>(idx)] = conc;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_pollutant_set_gw_conc(SWMM_Engine engine, int idx, double conc) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_pollutants());
    ctx.pollutants.c_gw[static_cast<std::size_t>(idx)] = conc;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_pollutant_set_init_conc(SWMM_Engine engine, int idx, double conc) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_pollutants());
    ctx.pollutants.init_conc[static_cast<std::size_t>(idx)] = conc;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_pollutant_get_units(SWMM_Engine engine, int idx, int* units) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_pollutants());
    if (units) *units = static_cast<int>(ctx.pollutants.units[static_cast<std::size_t>(idx)]);
    return SWMM_OK;
}

// ============================================================================
// Property getters
// ============================================================================

SWMM_ENGINE_API int swmm_pollutant_get_kdecay(SWMM_Engine engine, int idx, double* k) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_pollutants());
    if (k) *k = ctx.pollutants.k_decay[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_pollutant_get_rain_conc(SWMM_Engine engine, int idx, double* conc) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_pollutants());
    if (conc) *conc = ctx.pollutants.c_rain[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_pollutant_get_gw_conc(SWMM_Engine engine, int idx, double* conc) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_pollutants());
    if (conc) *conc = ctx.pollutants.c_gw[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_pollutant_get_init_conc(SWMM_Engine engine, int idx, double* conc) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_pollutants());
    if (conc) *conc = ctx.pollutants.init_conc[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_pollutant_set_rdii_conc(SWMM_Engine engine, int idx, double conc) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_pollutants());
    ctx.pollutants.c_rdii[static_cast<std::size_t>(idx)] = conc;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_pollutant_get_rdii_conc(SWMM_Engine engine, int idx, double* conc) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_pollutants());
    if (conc) *conc = ctx.pollutants.c_rdii[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_pollutant_set_mwt(SWMM_Engine engine, int idx, double mwt) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_pollutants());
    ctx.pollutants.mwt[static_cast<std::size_t>(idx)] = mwt;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_pollutant_get_mwt(SWMM_Engine engine, int idx, double* mwt) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_pollutants());
    if (mwt) *mwt = ctx.pollutants.mwt[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_pollutant_set_co_pollutant(SWMM_Engine engine, int idx, int co_idx, double frac) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_pollutants());
    ctx.pollutants.co_pollut[static_cast<std::size_t>(idx)] = co_idx;
    ctx.pollutants.co_frac[static_cast<std::size_t>(idx)] = frac;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_pollutant_get_co_pollutant(SWMM_Engine engine, int idx, int* co_idx, double* frac) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_pollutants());
    if (co_idx) *co_idx = ctx.pollutants.co_pollut[static_cast<std::size_t>(idx)];
    if (frac)   *frac   = ctx.pollutants.co_frac[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_pollutant_set_snow_only(SWMM_Engine engine, int idx, int flag) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_pollutants());
    ctx.pollutants.snow_only[static_cast<std::size_t>(idx)] = (flag != 0);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_pollutant_get_snow_only(SWMM_Engine engine, int idx, int* flag) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_pollutants());
    if (flag) *flag = ctx.pollutants.snow_only[static_cast<std::size_t>(idx)] ? 1 : 0;
    return SWMM_OK;
}

// ============================================================================
// Runtime quality injection
// ============================================================================

SWMM_ENGINE_API int swmm_node_set_quality(SWMM_Engine engine, int node_idx, int pollut_idx, double conc) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(node_idx >= 0 && node_idx < ctx.n_nodes());
    CHECK_INDEX(pollut_idx >= 0 && pollut_idx < ctx.n_pollutants());
    const auto np = static_cast<std::size_t>(ctx.n_pollutants());
    ctx.nodes.conc[static_cast<std::size_t>(node_idx) * np + static_cast<std::size_t>(pollut_idx)] = conc;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_set_quality(SWMM_Engine engine, int link_idx, int pollut_idx, double conc) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(link_idx >= 0 && link_idx < ctx.n_links());
    CHECK_INDEX(pollut_idx >= 0 && pollut_idx < ctx.n_pollutants());
    const auto np = static_cast<std::size_t>(ctx.n_pollutants());
    ctx.links.conc[static_cast<std::size_t>(link_idx) * np + static_cast<std::size_t>(pollut_idx)] = conc;
    return SWMM_OK;
}

} /* extern "C" */
