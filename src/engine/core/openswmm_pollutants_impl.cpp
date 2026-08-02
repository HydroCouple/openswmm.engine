/**
 * @file openswmm_pollutants_impl.cpp
 * @brief C API implementation — pollutant identity, creation, properties, quality injection.
 *
 * @see include/openswmm/engine/openswmm_pollutants.h
 * @ingroup engine_api
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "openswmm_api_common.hpp"
#include "../../../include/openswmm/engine/openswmm_pollutants.h"

namespace {

/// Insert one zero column into a row-major [rows x np_old] matrix, giving
/// [rows x (np_old+1)] with existing entries preserved (pollutant is the
/// fast dimension, so the stride changes and rows re-pack back-to-front).
template <typename T>
void insert_pollut_column(std::vector<T>& v, int rows, int np_old, T def = {}) {
    if (rows <= 0 || np_old < 0) return;
    if (static_cast<int>(v.size()) != rows * np_old) return;
    const int np_new = np_old + 1;
    v.resize(static_cast<std::size_t>(rows) * static_cast<std::size_t>(np_new), def);
    for (int r = rows - 1; r >= 0; --r) {
        for (int p = np_old - 1; p >= 0; --p)
            v[static_cast<std::size_t>(r) * static_cast<std::size_t>(np_new)
              + static_cast<std::size_t>(p)] =
                v[static_cast<std::size_t>(r) * static_cast<std::size_t>(np_old)
                  + static_cast<std::size_t>(p)];
        v[static_cast<std::size_t>(r) * static_cast<std::size_t>(np_new)
          + static_cast<std::size_t>(np_old)] = def;
    }
}

} // namespace

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
    // Pollutants may be added during programmatic construction (BUILDING) or
    // on a model opened from an .inp (OPENED) — mirroring swmm_landuse_add
    // (iteration 4; previously BUILDING-only).
    if (ctx.state != openswmm::EngineState::BUILDING &&
        ctx.state != openswmm::EngineState::OPENED)
        return SWMM_ERR_LIFECYCLE;

    // Check for duplicate ID
    if (ctx.pollutant_names.find(id) >= 0)
        return SWMM_ERR_BADPARAM;

    // Add to name index
    int idx = ctx.pollutant_names.add(id);
    int n = ctx.pollutant_names.size();
    const int np_old = n - 1;

    // GROW-PRESERVING (iteration 4): resize_pollutants assign-and-wipes, so
    // grow each definition array individually — existing pollutants keep
    // their parameters.
    const auto un = static_cast<std::size_t>(n);
    ctx.pollutants.units.resize(un, openswmm::MassUnits::MG_PER_L);
    ctx.pollutants.mwt.resize(un, 1.0);
    ctx.pollutants.k_decay.resize(un, 0.0);
    ctx.pollutants.c_rain.resize(un, 0.0);
    ctx.pollutants.c_gw.resize(un, 0.0);
    ctx.pollutants.c_rdii.resize(un, 0.0);
    ctx.pollutants.c_dwf.resize(un, 0.0);
    ctx.pollutants.init_conc.resize(un, 0.0);
    ctx.pollutants.co_pollut.resize(un, -1);
    ctx.pollutants.co_frac.resize(un, 0.0);
    ctx.pollutants.snow_only.resize(un, false);
    ctx.pollutants.comments.resize(un);

    // Pollutant is the FAST dimension of the (x, pollutant) matrices, so an
    // add changes their stride — insert a zero column wherever a matrix is
    // sized for the old count so existing entries survive.
    if (ctx.buildup.n_pollutants == np_old && ctx.buildup.n_landuses > 0) {
        const int rows = ctx.buildup.n_landuses;
        insert_pollut_column(ctx.buildup.func_type,  rows, np_old);
        insert_pollut_column(ctx.buildup.coeff1,     rows, np_old);
        insert_pollut_column(ctx.buildup.coeff2,     rows, np_old);
        insert_pollut_column(ctx.buildup.coeff3,     rows, np_old);
        insert_pollut_column(ctx.buildup.normalizer, rows, np_old);
        ctx.buildup.n_pollutants = n;
    }
    if (ctx.washoff.n_pollutants == np_old && ctx.washoff.n_landuses > 0) {
        const int rows = ctx.washoff.n_landuses;
        insert_pollut_column(ctx.washoff.func_type,   rows, np_old);
        insert_pollut_column(ctx.washoff.coeff,       rows, np_old);
        insert_pollut_column(ctx.washoff.expon,       rows, np_old);
        insert_pollut_column(ctx.washoff.sweep_effic, rows, np_old);
        insert_pollut_column(ctx.washoff.bmp_effic,   rows, np_old);
        ctx.washoff.n_pollutants = n;
    }
    if (ctx.treatment.n_pollutants == np_old && ctx.treatment.n_nodes > 0) {
        const int rows = ctx.treatment.n_nodes;
        insert_pollut_column(ctx.treatment.expressions, rows, np_old);
        insert_pollut_column(ctx.treatment.compiled,    rows, np_old);
        ctx.treatment.cin.resize(un, 0.0);
        ctx.treatment.removal.resize(un, -1.0);
        ctx.treatment.n_pollutants = n;
    }
    if (ctx.subcatches.conc_n_pollutants == np_old && ctx.n_subcatches() > 0) {
        const int rows = ctx.n_subcatches();
        insert_pollut_column(ctx.subcatches.conc,         rows, np_old);
        insert_pollut_column(ctx.subcatches.conc_old,     rows, np_old);
        insert_pollut_column(ctx.subcatches.ponded_qual,  rows, np_old);
        insert_pollut_column(ctx.subcatches.washoff_load, rows, np_old);
        ctx.subcatches.conc_n_pollutants = n;
    }

    // Set units for the new pollutant
    ctx.pollutants.units[static_cast<std::size_t>(idx)] = static_cast<openswmm::MassUnits>(units);

    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_pollutant_rename(SWMM_Engine engine, int idx, const char* new_id) {
    CHECK_HANDLE(engine);
    if (!new_id || new_id[0] == '\0') return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_pollutants());

    const std::string prev = ctx.pollutant_names.name_of(idx);
    if (prev == new_id) return SWMM_OK;
    if (!ctx.pollutant_names.rename(idx, new_id)) return SWMM_ERR_BADPARAM;

    // Follow the name-stored references ([INFLOWS]/[DWF] constituent
    // strings — the same set ObjectDeleter cascades on delete). Everything
    // else (co-pollutant, LID removals, buildup/washoff, treatment) is
    // index/positional and unaffected.
    const std::string next = new_id;
    for (auto& c : ctx.ext_inflows.constituent)
        if (c == prev) c = next;
    for (auto& c : ctx.dwf_inflows.constituent)
        if (c == prev) c = next;
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
    // Initial conveyance-network concentration only seeds state at start(); it
    // has no per-step consumer, so a mid-run edit would silently no-op. Guard
    // it to the editable (pre-start) states to make the contract honest.
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_pollutants());
    ctx.pollutants.init_conc[static_cast<std::size_t>(idx)] = conc;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_pollutant_set_units(SWMM_Engine engine, int idx, int units) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    // Units define how every stored/reported concentration is read; only
    // meaningful while still building the model (pre-start), like init_conc.
    if (ctx.state != openswmm::EngineState::BUILDING &&
        ctx.state != openswmm::EngineState::OPENED)
        return SWMM_ERR_LIFECYCLE;
    CHECK_INDEX(idx >= 0 && idx < ctx.n_pollutants());
    if (units < 0 || units > 2) return SWMM_ERR_BADPARAM;
    ctx.pollutants.units[static_cast<std::size_t>(idx)] = static_cast<openswmm::MassUnits>(units);
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

SWMM_ENGINE_API int swmm_pollutant_set_dwf_conc(SWMM_Engine engine, int idx, double conc) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_pollutants());
    ctx.pollutants.c_dwf[static_cast<std::size_t>(idx)] = conc;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_pollutant_get_dwf_conc(SWMM_Engine engine, int idx, double* conc) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_pollutants());
    if (conc) *conc = ctx.pollutants.c_dwf[static_cast<std::size_t>(idx)];
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
