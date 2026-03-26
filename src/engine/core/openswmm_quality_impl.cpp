/**
 * @file openswmm_quality_impl.cpp
 * @brief C API implementation — landuse, buildup, washoff, treatment.
 *
 * @see include/openswmm/engine/openswmm_quality.h
 * @ingroup engine_api
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "openswmm_api_common.hpp"
#include "../../../include/openswmm/engine/openswmm_quality.h"

extern "C" {

// ============================================================================
// Landuse — Identity
// ============================================================================

SWMM_ENGINE_API int swmm_landuse_count(SWMM_Engine engine) {
    if (!engine) return -1;
    return to_engine(engine)->context().n_landuses();
}

SWMM_ENGINE_API int swmm_landuse_index(SWMM_Engine engine, const char* id) {
    if (!engine || !id) return -1;
    return to_engine(engine)->context().landuse_names.find(id);
}

SWMM_ENGINE_API const char* swmm_landuse_id(SWMM_Engine engine, int idx) {
    if (!engine) return nullptr;
    const auto& ctx = to_engine(engine)->context();
    if (idx < 0 || idx >= ctx.n_landuses()) return nullptr;
    return ctx.landuse_names.name_of(idx).c_str();
}

// ============================================================================
// Landuse — Creation (BUILDING state only)
// ============================================================================

SWMM_ENGINE_API int swmm_landuse_add(SWMM_Engine engine, const char* id) {
    CHECK_HANDLE(engine);
    if (!id) return SWMM_ERR_BADPARAM;

    auto& ctx = to_engine(engine)->context();
    if (ctx.state != openswmm::EngineState::BUILDING)
        return SWMM_ERR_LIFECYCLE;

    // Check for duplicate
    if (ctx.landuse_names.find(id) >= 0)
        return SWMM_ERR_BADPARAM;

    ctx.landuse_names.add(id);
    int n = ctx.landuse_names.size();
    ctx.landuses.resize(n);

    // If pollutants exist, resize buildup/washoff matrices
    if (ctx.n_pollutants() > 0) {
        ctx.buildup.resize(n, ctx.n_pollutants());
        ctx.washoff.resize(n, ctx.n_pollutants());
    }

    // Resize coverage matrix if subcatchments exist
    if (ctx.n_subcatches() > 0) {
        ctx.subcatches.resize_coverage(ctx.n_subcatches(), n);
    }

    return SWMM_OK;
}

// ============================================================================
// Landuse — Property setters/getters
// ============================================================================

SWMM_ENGINE_API int swmm_landuse_set_sweep_interval(SWMM_Engine engine, int idx, double days) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_landuses());
    ctx.landuses.sweep_interval[static_cast<std::size_t>(idx)] = days;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_landuse_get_sweep_interval(SWMM_Engine engine, int idx, double* days) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_landuses());
    if (days) *days = ctx.landuses.sweep_interval[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_landuse_set_sweep_removal(SWMM_Engine engine, int idx, double frac) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_landuses());
    ctx.landuses.sweep_removal[static_cast<std::size_t>(idx)] = frac;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_landuse_get_sweep_removal(SWMM_Engine engine, int idx, double* frac) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_landuses());
    if (frac) *frac = ctx.landuses.sweep_removal[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

// ============================================================================
// Buildup — set/get
// ============================================================================

SWMM_ENGINE_API int swmm_buildup_set(SWMM_Engine engine, int lu_idx, int pollut_idx,
                                       int func_type, double c1, double c2, double c3,
                                       int normalizer) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(lu_idx >= 0 && lu_idx < ctx.n_landuses());
    CHECK_INDEX(pollut_idx >= 0 && pollut_idx < ctx.n_pollutants());

    // Ensure buildup matrix is sized
    if (ctx.buildup.n_landuses != ctx.n_landuses() ||
        ctx.buildup.n_pollutants != ctx.n_pollutants()) {
        ctx.buildup.resize(ctx.n_landuses(), ctx.n_pollutants());
    }

    auto k = static_cast<std::size_t>(lu_idx) *
             static_cast<std::size_t>(ctx.n_pollutants()) +
             static_cast<std::size_t>(pollut_idx);
    ctx.buildup.func_type[k]  = func_type;
    ctx.buildup.coeff1[k]     = c1;
    ctx.buildup.coeff2[k]     = c2;
    ctx.buildup.coeff3[k]     = c3;
    ctx.buildup.normalizer[k] = normalizer;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_buildup_get(SWMM_Engine engine, int lu_idx, int pollut_idx,
                                       int* func_type, double* c1, double* c2, double* c3,
                                       int* normalizer) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(lu_idx >= 0 && lu_idx < ctx.n_landuses());
    CHECK_INDEX(pollut_idx >= 0 && pollut_idx < ctx.n_pollutants());

    auto k = static_cast<std::size_t>(lu_idx) *
             static_cast<std::size_t>(ctx.n_pollutants()) +
             static_cast<std::size_t>(pollut_idx);

    if (k >= ctx.buildup.func_type.size()) return SWMM_ERR_BADINDEX;

    if (func_type)  *func_type  = ctx.buildup.func_type[k];
    if (c1)         *c1         = ctx.buildup.coeff1[k];
    if (c2)         *c2         = ctx.buildup.coeff2[k];
    if (c3)         *c3         = ctx.buildup.coeff3[k];
    if (normalizer) *normalizer = ctx.buildup.normalizer[k];
    return SWMM_OK;
}

// ============================================================================
// Washoff — set/get
// ============================================================================

SWMM_ENGINE_API int swmm_washoff_set(SWMM_Engine engine, int lu_idx, int pollut_idx,
                                       int func_type, double coeff, double expon,
                                       double sweep_effic, double bmp_effic) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(lu_idx >= 0 && lu_idx < ctx.n_landuses());
    CHECK_INDEX(pollut_idx >= 0 && pollut_idx < ctx.n_pollutants());

    // Ensure washoff matrix is sized
    if (ctx.washoff.n_landuses != ctx.n_landuses() ||
        ctx.washoff.n_pollutants != ctx.n_pollutants()) {
        ctx.washoff.resize(ctx.n_landuses(), ctx.n_pollutants());
    }

    auto k = static_cast<std::size_t>(lu_idx) *
             static_cast<std::size_t>(ctx.n_pollutants()) +
             static_cast<std::size_t>(pollut_idx);
    ctx.washoff.func_type[k]   = func_type;
    ctx.washoff.coeff[k]       = coeff;
    ctx.washoff.expon[k]       = expon;
    ctx.washoff.sweep_effic[k] = sweep_effic;
    ctx.washoff.bmp_effic[k]   = bmp_effic;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_washoff_get(SWMM_Engine engine, int lu_idx, int pollut_idx,
                                       int* func_type, double* coeff, double* expon,
                                       double* sweep_effic, double* bmp_effic) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(lu_idx >= 0 && lu_idx < ctx.n_landuses());
    CHECK_INDEX(pollut_idx >= 0 && pollut_idx < ctx.n_pollutants());

    auto k = static_cast<std::size_t>(lu_idx) *
             static_cast<std::size_t>(ctx.n_pollutants()) +
             static_cast<std::size_t>(pollut_idx);

    if (k >= ctx.washoff.func_type.size()) return SWMM_ERR_BADINDEX;

    if (func_type)   *func_type   = ctx.washoff.func_type[k];
    if (coeff)       *coeff       = ctx.washoff.coeff[k];
    if (expon)       *expon       = ctx.washoff.expon[k];
    if (sweep_effic) *sweep_effic = ctx.washoff.sweep_effic[k];
    if (bmp_effic)   *bmp_effic   = ctx.washoff.bmp_effic[k];
    return SWMM_OK;
}

// ============================================================================
// Treatment — set/get/clear
// ============================================================================

SWMM_ENGINE_API int swmm_treatment_set(SWMM_Engine engine, int node_idx, int pollut_idx,
                                         const char* expression) {
    CHECK_HANDLE(engine);
    if (!expression) return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(node_idx >= 0 && node_idx < ctx.n_nodes());
    CHECK_INDEX(pollut_idx >= 0 && pollut_idx < ctx.n_pollutants());

    // Ensure treatment matrix is sized
    if (ctx.treatment.n_nodes != ctx.n_nodes() ||
        ctx.treatment.n_pollutants != ctx.n_pollutants()) {
        ctx.treatment.resize(ctx.n_nodes(), ctx.n_pollutants());
    }

    auto k = static_cast<std::size_t>(node_idx) *
             static_cast<std::size_t>(ctx.n_pollutants()) +
             static_cast<std::size_t>(pollut_idx);
    ctx.treatment.expressions[k] = expression;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_treatment_get(SWMM_Engine engine, int node_idx, int pollut_idx,
                                         char* buf, int buflen) {
    CHECK_HANDLE(engine);
    if (!buf || buflen <= 0) return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(node_idx >= 0 && node_idx < ctx.n_nodes());
    CHECK_INDEX(pollut_idx >= 0 && pollut_idx < ctx.n_pollutants());

    auto k = static_cast<std::size_t>(node_idx) *
             static_cast<std::size_t>(ctx.n_pollutants()) +
             static_cast<std::size_t>(pollut_idx);

    if (k >= ctx.treatment.expressions.size()) {
        buf[0] = '\0';
        return SWMM_OK;
    }

    const auto& expr = ctx.treatment.expressions[k];
    std::strncpy(buf, expr.c_str(), static_cast<std::size_t>(buflen - 1));
    buf[buflen - 1] = '\0';
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_treatment_clear(SWMM_Engine engine, int node_idx, int pollut_idx) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(node_idx >= 0 && node_idx < ctx.n_nodes());
    CHECK_INDEX(pollut_idx >= 0 && pollut_idx < ctx.n_pollutants());

    auto k = static_cast<std::size_t>(node_idx) *
             static_cast<std::size_t>(ctx.n_pollutants()) +
             static_cast<std::size_t>(pollut_idx);

    if (k < ctx.treatment.expressions.size()) {
        ctx.treatment.expressions[k].clear();
    }
    return SWMM_OK;
}

} /* extern "C" */
