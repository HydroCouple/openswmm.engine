/**
 * @file openswmm_massbalance.h
 * @brief OpenSWMM Engine — Mass Balance / Continuity C API.
 *
 * @details Query continuity errors and cumulative flux totals for runoff,
 *          routing, and water quality. Available after SWMM_STATE_ENDED.
 *
 * @ingroup engine_api
 * @see openswmm_engine.h
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_MASSBALANCE_H
#define OPENSWMM_MASSBALANCE_H

#include "openswmm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Continuity error queries (valid after SWMM_STATE_ENDED, or during RUNNING)
 * ========================================================================= */

/** @brief Get the runoff continuity error (fraction, e.g. 0.001 = 0.1%). */
SWMM_ENGINE_API int swmm_get_runoff_continuity_error (SWMM_Engine engine, double* error);

/** @brief Get the routing continuity error (fraction). */
SWMM_ENGINE_API int swmm_get_routing_continuity_error(SWMM_Engine engine, double* error);

/** @brief Get the quality continuity error for a pollutant (fraction). */
SWMM_ENGINE_API int swmm_get_quality_continuity_error(SWMM_Engine engine,
                                                       int pollutant_idx, double* error);

/* =========================================================================
 * Cumulative flux totals (populated during simulation)
 * ========================================================================= */

/** @brief Runoff mass balance component codes. */
typedef enum SWMM_RunoffTotal {
    SWMM_RUNOFF_RAINFALL   = 0, /**< Cumulative rainfall volume. */
    SWMM_RUNOFF_EVAP       = 1, /**< Cumulative evaporation loss. */
    SWMM_RUNOFF_INFIL      = 2, /**< Cumulative infiltration loss. */
    SWMM_RUNOFF_RUNOFF     = 3, /**< Cumulative surface runoff volume. */
    SWMM_RUNOFF_SNOWREMOV  = 4, /**< Cumulative snow removal volume. */
    SWMM_RUNOFF_INITSTORE  = 5, /**< Initial surface storage volume. */
    SWMM_RUNOFF_FINALSTORE = 6  /**< Final surface storage volume. */
} SWMM_RunoffTotal;

/** @brief Routing mass balance component codes. */
typedef enum SWMM_RoutingTotal {
    SWMM_ROUTING_DRY_WEATHER   = 0,  /**< Cumulative dry weather inflow volume. */
    SWMM_ROUTING_WET_WEATHER   = 1,  /**< Cumulative wet weather (runoff) inflow volume. */
    SWMM_ROUTING_GW_INFLOW     = 2,  /**< Cumulative groundwater inflow volume. */
    SWMM_ROUTING_RDII          = 3,  /**< Cumulative rainfall-dependent I/I volume. */
    SWMM_ROUTING_EXTERNAL      = 4,  /**< Cumulative external (user-defined) inflow volume. */
    SWMM_ROUTING_FLOODING      = 5,  /**< Cumulative flooding (surcharge overflow) volume. */
    SWMM_ROUTING_OUTFLOW       = 6,  /**< Cumulative outfall discharge volume. */
    SWMM_ROUTING_EVAP_LOSS     = 7,  /**< Cumulative evaporation loss from conveyance. */
    SWMM_ROUTING_SEEP_LOSS     = 8,  /**< Cumulative seepage loss from conveyance. */
    SWMM_ROUTING_INIT_STORAGE  = 9,  /**< Initial in-system storage volume. */
    SWMM_ROUTING_FINAL_STORAGE = 10  /**< Final in-system storage volume. */
} SWMM_RoutingTotal;

/**
 * @brief Get a runoff mass balance total (cumulative volume).
 * @param engine     Engine handle.
 * @param component  SWMM_RunoffTotal code.
 * @param volume     [out] Cumulative volume (project volume units).
 * @returns SWMM_OK or error code.
 */
SWMM_ENGINE_API int swmm_get_runoff_total (SWMM_Engine engine, int component, double* volume);

/**
 * @brief Get a routing mass balance total (cumulative volume).
 * @param engine     Engine handle.
 * @param component  SWMM_RoutingTotal code.
 * @param volume     [out] Cumulative volume (project volume units).
 * @returns SWMM_OK or error code.
 */
SWMM_ENGINE_API int swmm_get_routing_total(SWMM_Engine engine, int component, double* volume);

/* =========================================================================
 * Routing diagnostics (combined stats in one call)
 * ========================================================================= */

/** @brief Get combined routing statistics in a single call. */
SWMM_ENGINE_API int swmm_get_routing_stats(SWMM_Engine engine,
    double* avg_step, double* min_step, double* max_step,
    int* n_steps, double* pct_non_converged,
    double* avg_iterations, double* max_courant);

/** @brief Get maximum Courant number observed during simulation. */
SWMM_ENGINE_API int swmm_get_max_courant(SWMM_Engine engine, double* max_courant);

/* =========================================================================
 * Quality mass losses (per-pollutant)
 * ========================================================================= */

/** @brief Get quality mass lost to seepage (per pollutant). */
SWMM_ENGINE_API int swmm_get_quality_seep_loss(SWMM_Engine engine, int pollutant_idx, double* mass);

/** @brief Get quality mass lost to evaporation (per pollutant). */
SWMM_ENGINE_API int swmm_get_quality_evap_loss(SWMM_Engine engine, int pollutant_idx, double* mass);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_MASSBALANCE_H */
