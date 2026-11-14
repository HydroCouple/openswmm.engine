// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file      openswmm_massbalance.h
 * @brief     OpenSWMM Engine — Mass Balance / Continuity C API.
 *
 * @details Query continuity errors and cumulative flux totals for runoff,
 *          routing, and water quality. Finalized after SWMM_STATE_ENDED; getters also permit
 * intermediate reads.
 *
 * @ingroup engine_api
 * @see openswmm_engine.h
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
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

/**
 * @brief Compute the signed runoff water-continuity residual fraction.
 *
 * Uses the current stored balance totals without converting units or updating final storage. A
 * running query may therefore differ from the finalized end-of-run balance.
 *
 * @param[in] engine Non-null live engine. No lifecycle guard is imposed; final totals are available
 *     after end, and running reads reflect currently accumulated state.
 * @param[out] error Optional fraction output, e.g. 0.001 means 0.1%. Positive indicates accounted
 *     input exceeds output/storage; zero when total input including initial storage is nonpositive.
 * @return SWMM_OK, or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_get_runoff_continuity_error(SWMM_Engine engine, double* error);

/**
 * @brief Compute the signed routing water-continuity residual fraction.
 *
 * Uses the current stored balance totals without converting units or updating final storage. A
 * running query may therefore differ from the finalized end-of-run balance.
 *
 * @param[in] engine Non-null live engine. No lifecycle guard is imposed; final totals are available
 *     after end, and running reads reflect currently accumulated state.
 * @param[out] error Optional fraction output, e.g. 0.001 means 0.1%. Positive indicates accounted
 *     input exceeds output/storage; zero when total input including initial storage is nonpositive.
 * @return SWMM_OK, or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_get_routing_continuity_error(SWMM_Engine engine, double* error);

/**
 * @brief Compute a pollutant routing-continuity residual from stored mass totals.
 *
 * Sums the implementation wet-deposition/runoff/wet/RDII/dry-weather/external inflow terms plus
 * initial storage against outflow/flooding/reaction and final storage. Residuals below 0.001 native
 * amount are zeroed; otherwise divides by input when positive, or output when only output is
 * positive. This function does not apply a percent conversion.
 *
 * @param[in] engine Non-null live engine. No lifecycle guard is imposed; final totals are available
 *     after end, and running reads reflect currently accumulated state.
 * @param[in] pollutant_idx Zero-based pollutant row; an unavailable/negative row writes zero and
 *     still succeeds.
 * @param[out] error Optional signed residual fraction; NULL returns success after checking the
 *     engine.
 * @return SWMM_OK, or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_get_quality_continuity_error(SWMM_Engine engine, int pollutant_idx,
                                                      double* error);

/* =========================================================================
 * Cumulative flux totals (populated during simulation)
 * ========================================================================= */

/** @brief Runoff mass balance component codes. */
typedef enum SWMM_RunoffTotal {
    SWMM_RUNOFF_RAINFALL = 0,   /**< Cumulative rainfall volume. */
    SWMM_RUNOFF_EVAP = 1,       /**< Cumulative evaporation loss. */
    SWMM_RUNOFF_INFIL = 2,      /**< Cumulative infiltration loss. */
    SWMM_RUNOFF_RUNOFF = 3,     /**< Cumulative surface runoff volume. */
    SWMM_RUNOFF_SNOWREMOV = 4,  /**< Cumulative snow removal volume. */
    SWMM_RUNOFF_INITSTORE = 5,  /**< Initial surface storage volume. */
    SWMM_RUNOFF_FINALSTORE = 6, /**< Final surface storage volume. */
    SWMM_RUNOFF_INITSNOW = 7,   /**< Initial snow cover volume (SWE + free water). */
    SWMM_RUNOFF_FINALSNOW = 8   /**< Final snow cover volume (SWE + free water). */
} SWMM_RunoffTotal;

/** @brief Routing mass balance component codes. */
typedef enum SWMM_RoutingTotal {
    SWMM_ROUTING_DRY_WEATHER = 0,    /**< Cumulative dry weather inflow volume. */
    SWMM_ROUTING_WET_WEATHER = 1,    /**< Cumulative wet weather (runoff) inflow volume. */
    SWMM_ROUTING_GW_INFLOW = 2,      /**< Cumulative groundwater inflow volume. */
    SWMM_ROUTING_RDII = 3,           /**< Cumulative rainfall-dependent I/I volume. */
    SWMM_ROUTING_EXTERNAL = 4,       /**< Cumulative external (user-defined) inflow volume. */
    SWMM_ROUTING_FLOODING = 5,       /**< Cumulative flooding (surcharge overflow) volume. */
    SWMM_ROUTING_OUTFLOW = 6,        /**< Cumulative outfall discharge volume. */
    SWMM_ROUTING_EVAP_LOSS = 7,      /**< Cumulative evaporation loss from conveyance. */
    SWMM_ROUTING_SEEP_LOSS = 8,      /**< Cumulative seepage loss from conveyance. */
    SWMM_ROUTING_INIT_STORAGE = 9,   /**< Initial in-system storage volume. */
    SWMM_ROUTING_FINAL_STORAGE = 10, /**< Final in-system storage volume. */
    SWMM_ROUTING_FORCING_INFLOW = 11 /**< Cumulative runtime-API forced
                                       *   lateral inflow volume (i.e. flow
                                       *   injected via
                                       *   `swmm_node_set_lateral_inflow` or
                                       *   transient ForcingData). A subset of
                                       *   `SWMM_ROUTING_EXTERNAL`, which also
                                       *   counts INP `[INFLOWS]`-derived,
                                       *   interface-file, and 2D-coupling
                                       *   inflow (issue #113). */
    ,
    SWMM_ROUTING_LINK_GW_INFLOW = 13 /**< G-X4 (2026-09-20): cumulative volume
                                       *   the two-zone [2D_AQUIFER] handed to
                                       *   conduits below the water table
                                       *   (gaining reaches). The negative half
                                       *   of the signed conduit seepage; the
                                       *   losing half stays
                                       *   SWMM_ROUTING_SEEP_LOSS. */
    ,
    SWMM_ROUTING_COUPLING_OUT = 12 /**< C2 (2026-09-07): cumulative volume
                                       *   the 1D→2D coupling spill removed
                                       *   from coupled nodes.
                                       *
                                       *   Previously folded into
                                       *   `SWMM_ROUTING_FLOODING`, which
                                       *   reported a coupling TRANSFER as
                                       *   flooding. It is still an OUTFLOW of
                                       *   the 1D system and still enters the
                                       *   routing continuity error, so a host
                                       *   summing the outflow categories must
                                       *   now add this one too.
                                       *
                                       *   Zero (and the volume stays in
                                       *   `SWMM_ROUTING_FLOODING`) under
                                       *   `[2D_OPTIONS] COUPLING_IN_FLOODING
                                       *   YES`. */
} SWMM_RoutingTotal;

/**
 * @brief Read a stored runoff water-balance volume.
 *
 * Includes cumulative flux and initial/final storage selectors. The value is read directly from the
 * current mass-balance accumulator; final storage is only as current as the last balance update.
 *
 * @param[in] engine Non-null live engine. No lifecycle guard is imposed; final totals are available
 *     after end, and running reads reflect currently accumulated state.
 * @param[in] component SWMM_RunoffTotal selector declared in this header.
 * @param[out] volume Non-null destination in internal cubic feet (ft3), regardless of project
 *     display units. No display-volume conversion is performed.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for null volume/unknown
 *     component.
 */
SWMM_ENGINE_API int swmm_get_runoff_total(SWMM_Engine engine, int component, double* volume);

/**
 * @brief Read a stored routing water-balance volume.
 *
 * Includes cumulative flux and initial/final storage selectors. The value is read directly from the
 * current mass-balance accumulator; final storage is only as current as the last balance update.
 *
 * @param[in] engine Non-null live engine. No lifecycle guard is imposed; final totals are available
 *     after end, and running reads reflect currently accumulated state.
 * @param[in] component SWMM_RoutingTotal selector declared in this header.
 * @param[out] volume Non-null destination in internal cubic feet (ft3), regardless of project
 *     display units. No display-volume conversion is performed.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for null volume/unknown
 *     component.
 */
SWMM_ENGINE_API int swmm_get_routing_total(SWMM_Engine engine, int component, double* volume);

/* =========================================================================
 * Routing diagnostics (combined stats in one call)
 * ========================================================================= */

/**
 * @brief Read accumulated routing timestep and convergence diagnostics.
 *
 * All output pointers may be NULL independently; this getter does not advance or finalize the
 * simulation.
 *
 * @param[in] engine Non-null live engine. No lifecycle guard is imposed; final totals are available
 *     after end, and running reads reflect currently accumulated state.
 * @param[out] avg_step Optional mean physical routing step in seconds; zero when no steps are
 *     counted.
 * @param[out] min_step Optional minimum step in seconds; the uninitialized sentinel is returned as
 *     zero.
 * @param[out] max_step Optional maximum step in seconds.
 * @param[out] n_steps Optional number of counted routing steps, converted to int.
 * @param[out] pct_non_converged Optional percent of counted steps that did not converge, in [0,100]
 *     under normal accounting.
 * @param[out] avg_iterations Optional mean solver iterations per counted step.
 * @param[out] max_courant Optional maximum recorded Courant number (dimensionless).
 * @return SWMM_OK, or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_get_routing_stats(SWMM_Engine engine, double* avg_step, double* min_step,
                                           double* max_step, int* n_steps,
                                           double* pct_non_converged, double* avg_iterations,
                                           double* max_courant);

/**
 * @brief Read the largest recorded routing Courant number.
 *
 * @param[in] engine Non-null live engine. No lifecycle guard is imposed; final totals are available
 *     after end, and running reads reflect currently accumulated state.
 * @param[out] max_courant Optional destination for the dimensionless maximum; initialized value is
 *     zero.
 * @return SWMM_OK, or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_get_max_courant(SWMM_Engine engine, double* max_courant);

/* =========================================================================
 * Quality mass losses (per-pollutant)
 * ========================================================================= */

/**
 * @brief Read cumulative pollutant amount booked to seepage.
 *
 * Returns the stored loss entry only; does not infer additional losses from water fluxes or current
 * concentrations.
 *
 * @param[in] engine Non-null live engine. No lifecycle guard is imposed; final totals are available
 *     after end, and running reads reflect currently accumulated state.
 * @param[in] pollutant_idx Zero-based native pollutant-ledger row; out-of-range or negative rows
 *     return zero without an index error.
 * @param[out] mass Optional destination for the raw stored quality-ledger amount. Routing ledger
 *     amounts use internal water volume (ft3) times the concentration row unit; no
 *     pollutant/display-mass conversion is applied here.
 * @return SWMM_OK, or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_get_quality_seep_loss(SWMM_Engine engine, int pollutant_idx, double* mass);

/**
 * @brief Read cumulative pollutant amount booked to evaporation.
 *
 * Returns the stored loss entry only; does not infer additional losses from water fluxes or current
 * concentrations.
 *
 * @param[in] engine Non-null live engine. No lifecycle guard is imposed; final totals are available
 *     after end, and running reads reflect currently accumulated state.
 * @param[in] pollutant_idx Zero-based native pollutant-ledger row; out-of-range or negative rows
 *     return zero without an index error.
 * @param[out] mass Optional destination for the raw stored quality-ledger amount. Routing ledger
 *     amounts use internal water volume (ft3) times the concentration row unit; no
 *     pollutant/display-mass conversion is applied here.
 * @return SWMM_OK, or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_get_quality_evap_loss(SWMM_Engine engine, int pollutant_idx, double* mass);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_MASSBALANCE_H */
