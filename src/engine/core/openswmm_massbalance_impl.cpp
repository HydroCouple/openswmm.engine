/**
 * @file openswmm_massbalance_impl.cpp
 * @brief C API implementation — continuity errors and flux totals.
 *
 * @see include/openswmm/engine/openswmm_massbalance.h
 * @ingroup engine_api
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "openswmm_api_common.hpp"
#include "../../../include/openswmm/engine/openswmm_massbalance.h"

extern "C" {

// ============================================================================
// Continuity errors
// ============================================================================

SWMM_ENGINE_API int swmm_get_runoff_continuity_error(SWMM_Engine engine, double* error) {
    CHECK_HANDLE(engine);
    if (error) *error = to_engine(engine)->context().mass_balance.runoff_error();
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_get_routing_continuity_error(SWMM_Engine engine, double* error) {
    CHECK_HANDLE(engine);
    if (error) *error = to_engine(engine)->context().mass_balance.routing_error();
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_get_quality_continuity_error(SWMM_Engine engine,
                                                       int pollutant_idx, double* error) {
    CHECK_HANDLE(engine);
    if (!error) return SWMM_OK;
    const auto& mb = to_engine(engine)->context().mass_balance;
    auto p = static_cast<std::size_t>(pollutant_idx);

    // Check bounds
    if (pollutant_idx < 0 || p >= mb.qual_routing_wet.size()) {
        *error = 0.0;
        return SWMM_OK;
    }

    // Quality continuity: error = |out - in - init + final| / in
    // Matching legacy massbal_getQualError():
    //   total_in  = wet_deposition + runoff_load + routing_wet + ii_in + ext_inflow
    //   total_out = routing_outflow + routing_flood + routing_reacted
    //   error     = (total_in + init - final - total_out) / total_in
    double total_in = 0.0;
    if (p < mb.qual_wet_deposition.size()) total_in += mb.qual_wet_deposition[p];
    if (p < mb.qual_runoff_load.size())    total_in += mb.qual_runoff_load[p];
    if (p < mb.qual_routing_wet.size())    total_in += mb.qual_routing_wet[p];
    if (p < mb.qual_routing_ii_in.size())  total_in += mb.qual_routing_ii_in[p];

    double total_out = 0.0;
    if (p < mb.qual_routing_outflow.size()) total_out += mb.qual_routing_outflow[p];
    if (p < mb.qual_routing_flood.size())   total_out += mb.qual_routing_flood[p];
    if (p < mb.qual_routing_reacted.size()) total_out += mb.qual_routing_reacted[p];

    double init_stored = (p < mb.qual_routing_init.size()) ? mb.qual_routing_init[p] : 0.0;
    double final_stored = (p < mb.qual_routing_final.size()) ? mb.qual_routing_final[p] : 0.0;

    if (total_in > 0.0) {
        *error = (total_in + init_stored - final_stored - total_out) / total_in;
    } else {
        *error = 0.0;
    }
    return SWMM_OK;
}

// ============================================================================
// Cumulative flux totals
// ============================================================================

SWMM_ENGINE_API int swmm_get_runoff_total(SWMM_Engine engine, int component, double* volume) {
    CHECK_HANDLE(engine);
    if (!volume) return SWMM_ERR_BADPARAM;
    const auto& mb = to_engine(engine)->context().mass_balance;
    switch (component) {
        case SWMM_RUNOFF_RAINFALL:   *volume = mb.runoff_rainfall;    break;
        case SWMM_RUNOFF_EVAP:       *volume = mb.runoff_evap;        break;
        case SWMM_RUNOFF_INFIL:      *volume = mb.runoff_infil;       break;
        case SWMM_RUNOFF_RUNOFF:     *volume = mb.runoff_runoff;      break;
        case SWMM_RUNOFF_SNOWREMOV:  *volume = mb.runoff_snowremov;   break;
        case SWMM_RUNOFF_INITSTORE:  *volume = mb.runoff_init_store;  break;
        case SWMM_RUNOFF_FINALSTORE: *volume = mb.runoff_final_store; break;
        default: return SWMM_ERR_BADPARAM;
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_get_routing_total(SWMM_Engine engine, int component, double* volume) {
    CHECK_HANDLE(engine);
    if (!volume) return SWMM_ERR_BADPARAM;
    const auto& mb = to_engine(engine)->context().mass_balance;
    switch (component) {
        case SWMM_ROUTING_DRY_WEATHER:   *volume = mb.routing_dry_weather;   break;
        case SWMM_ROUTING_WET_WEATHER:   *volume = mb.routing_wet_weather;   break;
        case SWMM_ROUTING_GW_INFLOW:     *volume = mb.routing_gw_inflow;     break;
        case SWMM_ROUTING_RDII:          *volume = mb.routing_rdii;          break;
        case SWMM_ROUTING_EXTERNAL:      *volume = mb.routing_external;      break;
        case SWMM_ROUTING_FLOODING:      *volume = mb.routing_flooding;      break;
        case SWMM_ROUTING_OUTFLOW:       *volume = mb.routing_outflow;       break;
        case SWMM_ROUTING_EVAP_LOSS:     *volume = mb.routing_evap_loss;     break;
        case SWMM_ROUTING_SEEP_LOSS:     *volume = mb.routing_seep_loss;     break;
        case SWMM_ROUTING_INIT_STORAGE:  *volume = mb.routing_init_storage;  break;
        case SWMM_ROUTING_FINAL_STORAGE: *volume = mb.routing_final_storage; break;
        default: return SWMM_ERR_BADPARAM;
    }
    return SWMM_OK;
}

} /* extern "C" */
