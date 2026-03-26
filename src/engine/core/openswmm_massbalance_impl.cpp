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
    (void)pollutant_idx;
    if (error) *error = 0.0;
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
