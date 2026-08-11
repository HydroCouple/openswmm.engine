/**
 * @file openswmm_quality_impl.cpp
 * @brief C API implementation — landuse, buildup, washoff, treatment.
 *
 * @see include/openswmm/engine/openswmm_quality.h
 * @ingroup engine_api
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "openswmm_api_common.hpp"
#include "../../../include/openswmm/engine/openswmm_quality.h"
#include "../quality/Treatment.hpp"

#include <cstring>

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
    // Landuses may be added during programmatic construction (BUILDING) or on a
    // model opened from an .inp (OPENED) — mirroring swmm_lid_add. Buildup /
    // washoff configuration itself imposes no lifecycle gate.
    if (ctx.state != openswmm::EngineState::BUILDING &&
        ctx.state != openswmm::EngineState::OPENED)
        return SWMM_ERR_LIFECYCLE;

    // Check for duplicate
    if (ctx.landuse_names.find(id) >= 0)
        return SWMM_ERR_BADPARAM;

    ctx.landuse_names.add(id);
    int n = ctx.landuse_names.size();

    // GROW-PRESERVING (iteration 4): the *Data::resize helpers assign-and-
    // wipe, which used to destroy existing sweeping params, buildup/washoff
    // rows, and every subcatchment coverage whenever a land use was added
    // to a populated model. Land use is the slow dimension of the
    // [lu*np + p] matrices, so appending zeros keeps existing entries at
    // their flat indices; the [sc*nlu + lu] coverage stride changes, so
    // those rows re-pack back-to-front.
    ctx.landuses.sweep_interval.resize(static_cast<std::size_t>(n), 0.0);
    ctx.landuses.sweep_removal.resize(static_cast<std::size_t>(n), 0.0);
    ctx.landuses.last_swept.resize(static_cast<std::size_t>(n), 0.0);
    ctx.landuses.comments.resize(static_cast<std::size_t>(n));

    const int np = ctx.n_pollutants();
    if (np > 0) {
        const auto total = static_cast<std::size_t>(n) * static_cast<std::size_t>(np);
        auto growBW = [&](auto& vec) { vec.resize(total, {}); };
        growBW(ctx.buildup.func_type);
        growBW(ctx.buildup.coeff1);
        growBW(ctx.buildup.coeff2);
        growBW(ctx.buildup.coeff3);
        growBW(ctx.buildup.normalizer);
        ctx.buildup.n_landuses = n;
        ctx.buildup.n_pollutants = np;
        growBW(ctx.washoff.func_type);
        growBW(ctx.washoff.coeff);
        growBW(ctx.washoff.expon);
        growBW(ctx.washoff.sweep_effic);
        growBW(ctx.washoff.bmp_effic);
        ctx.washoff.n_landuses = n;
        ctx.washoff.n_pollutants = np;
    }

    const int nSc = ctx.n_subcatches();
    if (nSc > 0) {
        const int nluOld = ctx.subcatches.coverage_n_landuses;
        auto repack = [&](std::vector<double>& v) {
            if (nluOld == n - 1 &&
                static_cast<int>(v.size()) == nSc * nluOld) {
                v.resize(static_cast<std::size_t>(nSc) * static_cast<std::size_t>(n), 0.0);
                for (int s = nSc - 1; s >= 0; --s) {
                    for (int lu = nluOld - 1; lu >= 0; --lu)
                        v[static_cast<std::size_t>(s) * static_cast<std::size_t>(n)
                          + static_cast<std::size_t>(lu)] =
                            v[static_cast<std::size_t>(s) * static_cast<std::size_t>(nluOld)
                              + static_cast<std::size_t>(lu)];
                    v[static_cast<std::size_t>(s) * static_cast<std::size_t>(n)
                      + static_cast<std::size_t>(n - 1)] = 0.0;
                }
            } else {
                v.assign(static_cast<std::size_t>(nSc) * static_cast<std::size_t>(n), 0.0);
            }
        };
        repack(ctx.subcatches.coverage);
        repack(ctx.subcatches.sweep_last_swept);
        ctx.subcatches.coverage_n_landuses = n;
    }

    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_landuse_rename(SWMM_Engine engine, int idx, const char* new_id) {
    CHECK_HANDLE(engine);
    if (!new_id || new_id[0] == '\0') return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_landuses());
    // Land uses are referenced positionally (matrix dimensions) — the name
    // registry is the only holder of the name.
    return ctx.landuse_names.rename(idx, new_id) ? SWMM_OK : SWMM_ERR_BADPARAM;
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
    // Refresh the per-step buildup/washoff parameter cache so a mid-run edit
    // takes effect on the next step (the accumulated pool is preserved).
    to_engine(engine)->refreshLanduseParams();
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
    // Refresh the per-step parameter cache so a mid-run edit takes effect.
    to_engine(engine)->refreshLanduseParams();
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
    // The step loop evaluates the compiled cache, so recompile the edited cell
    // (mid-run edits take effect next step). A parse failure restores the
    // previous expression and rejects the call rather than leaving the cell
    // silently inert.
    const std::string prev = ctx.treatment.expressions[k];
    ctx.treatment.expressions[k] = expression;
    if (to_engine(engine)->refreshTreatment(node_idx, pollut_idx) != 0) {
        ctx.treatment.expressions[k] = prev;
        to_engine(engine)->refreshTreatment(node_idx, pollut_idx);
        return SWMM_ERR_BADPARAM;
    }
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
        // Clear the compiled cell + per-node flag so the removal applies
        // on the next step.
        to_engine(engine)->refreshTreatment(node_idx, pollut_idx);
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_treatment_validate_expression(SWMM_Engine engine, const char* expr,
                                                        char* errbuf, int buflen, int* col_out) {
    CHECK_HANDLE(engine);
    if (errbuf && buflen > 0) errbuf[0] = '\0';
    if (col_out) *col_out = -1;
    if (!expr) return SWMM_ERR_BADPARAM;

    std::string msg;
    int col = -1;
    const int rc = openswmm::treatment::validate(expr, msg, col);
    if (rc == 0) return SWMM_OK;

    if (errbuf && buflen > 0) {
        std::strncpy(errbuf, msg.c_str(), static_cast<std::size_t>(buflen - 1));
        errbuf[buflen - 1] = '\0';
    }
    if (col_out) *col_out = col;
    return SWMM_ERR_BADPARAM;
}

} /* extern "C" */
