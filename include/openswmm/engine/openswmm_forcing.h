/**
 * @file openswmm_forcing.h
 * @brief Runtime forcing API — inject fluxes with mass-balance tracking.
 *
 * @details Provides per-element runtime forcing of lateral inflows, head
 *          boundaries, rainfall, evaporation, link settings, and quality mass
 *          fluxes. Each forcing specifies:
 *
 *            - **mode**: NONE (disabled), OVERRIDE (replace computed value),
 *                        ADD (add to computed value)
 *            - **persistence**: RESET (auto-clear after each timestep),
 *                               PERSIST (keep until explicitly cleared)
 *
 *          Forced volumes are tracked in the mass balance and reported
 *          separately via SWMM_ROUTING_FORCING.
 *
 *          All forcing functions require SWMM_STATE_RUNNING.
 *
 * @section forcing_lifecycle Lifecycle
 *
 * @code
 * swmm_engine_start(e, 1);
 * while (swmm_engine_step(e, &elapsed) == SWMM_OK && elapsed > 0) {
 *     // Set forcings for this step (or let PERSIST carry forward)
 *     swmm_forcing_node_lat_inflow(e, j1, 10.0,
 *         SWMM_FORCING_ADD, SWMM_FORCING_PERSIST);
 *
 *     // ... step runs, forcing is applied, mass balance tracks it ...
 *     // RESET forcings auto-clear; PERSIST forcings remain
 * }
 * swmm_engine_end(e);
 * @endcode
 *
 * @note All value parameters are in the simulation's internal units (ft, sec,
 *       CFS) unless otherwise noted. Rainfall values are in user display units
 *       (in/hr for US, mm/hr for SI) — the engine converts internally.
 *
 * @defgroup engine_forcing Runtime Forcing API
 * @ingroup  engine_api
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_FORCING_H
#define OPENSWMM_FORCING_H

#include "openswmm_callbacks.h"

#ifdef OPENSWMM_ENGINE_STATIC
#  define SWMM_ENGINE_API
#else
#  ifdef _WIN32
#    ifdef openswmm_engine_EXPORTS
#      define SWMM_ENGINE_API __declspec(dllexport)
#    else
#      define SWMM_ENGINE_API __declspec(dllimport)
#    endif
#  else
#    define SWMM_ENGINE_API __attribute__((visibility("default")))
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Enumerations
 * ========================================================================= */

/** @brief How the forcing value is applied relative to the computed value. */
typedef enum SWMM_ForcingMode {
    SWMM_FORCING_NONE     = 0,   /**< No forcing — use model-computed value. */
    SWMM_FORCING_OVERRIDE = 1,   /**< Replace computed value with user value. */
    SWMM_FORCING_ADD      = 2    /**< Add user value to computed value. */
} SWMM_ForcingMode;

/** @brief Whether the forcing persists across timesteps. */
typedef enum SWMM_ForcingPersist {
    SWMM_FORCING_RESET   = 0,   /**< Auto-clear after each timestep. */
    SWMM_FORCING_PERSIST = 1    /**< Keep until explicitly cleared. */
} SWMM_ForcingPersist;

/** @brief Forcing channel identifier (for targeted clear). */
typedef enum SWMM_ForcingType {
    SWMM_FORCE_NODE_LAT_INFLOW    = 0,
    SWMM_FORCE_NODE_HEAD_BOUNDARY = 1,
    SWMM_FORCE_NODE_QUALITY       = 2,
    SWMM_FORCE_LINK_FLOW          = 3,
    SWMM_FORCE_LINK_SETTING       = 4,
    SWMM_FORCE_SUBCATCH_RAINFALL  = 5,
    SWMM_FORCE_SUBCATCH_EVAP      = 6,
    SWMM_FORCE_GAGE_RAINFALL      = 7
} SWMM_ForcingType;

/* =========================================================================
 * Node forcing
 * ========================================================================= */

/**
 * @brief Force a lateral inflow at a node.
 *
 * @param engine   Engine handle.
 * @param idx      Node index.
 * @param value    Lateral inflow rate (CFS for US, CMS for SI).
 * @param mode     SWMM_FORCING_OVERRIDE or SWMM_FORCING_ADD.
 * @param persist  SWMM_FORCING_RESET or SWMM_FORCING_PERSIST.
 * @returns SWMM_OK or error code.
 * @ingroup engine_forcing
 */
SWMM_ENGINE_API int swmm_forcing_node_lat_inflow(
    SWMM_Engine engine, int idx, double value, int mode, int persist);

/**
 * @brief Force a head boundary at an outfall node.
 *
 * @param engine   Engine handle.
 * @param idx      Node index (must be OUTFALL type).
 * @param value    Boundary head (ft for US, m for SI).
 * @param mode     SWMM_FORCING_OVERRIDE (only valid mode for head boundary).
 * @param persist  SWMM_FORCING_RESET or SWMM_FORCING_PERSIST.
 * @returns SWMM_OK or error code.
 * @ingroup engine_forcing
 */
SWMM_ENGINE_API int swmm_forcing_node_head_boundary(
    SWMM_Engine engine, int idx, double value, int mode, int persist);

/**
 * @brief Force a quality mass flux at a node.
 *
 * @param engine        Engine handle.
 * @param node_idx      Node index.
 * @param pollutant_idx Pollutant index.
 * @param mass_rate     Mass injection rate (mass/sec in concentration units × CFS).
 * @param mode          SWMM_FORCING_OVERRIDE or SWMM_FORCING_ADD.
 * @param persist       SWMM_FORCING_RESET or SWMM_FORCING_PERSIST.
 * @returns SWMM_OK or error code.
 * @ingroup engine_forcing
 */
SWMM_ENGINE_API int swmm_forcing_node_quality(
    SWMM_Engine engine, int node_idx, int pollutant_idx,
    double mass_rate, int mode, int persist);

/* =========================================================================
 * Link forcing
 * ========================================================================= */

/**
 * @brief Force a flow in a link.
 *
 * @param engine   Engine handle.
 * @param idx      Link index.
 * @param value    Flow rate (CFS for US, CMS for SI).
 * @param mode     SWMM_FORCING_OVERRIDE or SWMM_FORCING_ADD.
 * @param persist  SWMM_FORCING_RESET or SWMM_FORCING_PERSIST.
 * @returns SWMM_OK or error code.
 * @ingroup engine_forcing
 */
SWMM_ENGINE_API int swmm_forcing_link_flow(
    SWMM_Engine engine, int idx, double value, int mode, int persist);

/**
 * @brief Force a control setting on a link (pump, orifice, weir, outlet).
 *
 * @param engine   Engine handle.
 * @param idx      Link index.
 * @param value    Setting value (0.0–1.0 for fraction open; pump speed).
 * @param mode     SWMM_FORCING_OVERRIDE or SWMM_FORCING_ADD.
 * @param persist  SWMM_FORCING_RESET or SWMM_FORCING_PERSIST.
 * @returns SWMM_OK or error code.
 * @ingroup engine_forcing
 */
SWMM_ENGINE_API int swmm_forcing_link_setting(
    SWMM_Engine engine, int idx, double value, int mode, int persist);

/* =========================================================================
 * Subcatchment forcing
 * ========================================================================= */

/**
 * @brief Force rainfall on a subcatchment (bypasses gage lookup).
 *
 * @param engine   Engine handle.
 * @param idx      Subcatchment index.
 * @param value    Rainfall rate (in/hr for US, mm/hr for SI).
 * @param mode     SWMM_FORCING_OVERRIDE or SWMM_FORCING_ADD.
 * @param persist  SWMM_FORCING_RESET or SWMM_FORCING_PERSIST.
 * @returns SWMM_OK or error code.
 * @ingroup engine_forcing
 */
SWMM_ENGINE_API int swmm_forcing_subcatch_rainfall(
    SWMM_Engine engine, int idx, double value, int mode, int persist);

/**
 * @brief Force an evaporation rate on a subcatchment.
 *
 * @param engine   Engine handle.
 * @param idx      Subcatchment index.
 * @param value    Evaporation rate (ft/sec internal units).
 * @param mode     SWMM_FORCING_OVERRIDE or SWMM_FORCING_ADD.
 * @param persist  SWMM_FORCING_RESET or SWMM_FORCING_PERSIST.
 * @returns SWMM_OK or error code.
 * @ingroup engine_forcing
 */
SWMM_ENGINE_API int swmm_forcing_subcatch_evap(
    SWMM_Engine engine, int idx, double value, int mode, int persist);

/* =========================================================================
 * Gage forcing
 * ========================================================================= */

/**
 * @brief Force rainfall on a rain gage (affects all linked subcatchments).
 *
 * @param engine   Engine handle.
 * @param idx      Gage index.
 * @param value    Rainfall rate (in/hr for US, mm/hr for SI).
 * @param mode     SWMM_FORCING_OVERRIDE or SWMM_FORCING_ADD.
 * @param persist  SWMM_FORCING_RESET or SWMM_FORCING_PERSIST.
 * @returns SWMM_OK or error code.
 * @ingroup engine_forcing
 */
SWMM_ENGINE_API int swmm_forcing_gage_rainfall(
    SWMM_Engine engine, int idx, double value, int mode, int persist);

/* =========================================================================
 * Clear forcing
 * ========================================================================= */

/**
 * @brief Clear forcing on a specific element and channel.
 *
 * @param engine   Engine handle.
 * @param type     Forcing channel (SWMM_ForcingType enum).
 * @param idx      Element index within that channel.
 * @returns SWMM_OK or error code.
 * @ingroup engine_forcing
 */
SWMM_ENGINE_API int swmm_forcing_clear(SWMM_Engine engine, int type, int idx);

/**
 * @brief Clear ALL forcings on ALL elements.
 *
 * @param engine   Engine handle.
 * @returns SWMM_OK or error code.
 * @ingroup engine_forcing
 */
SWMM_ENGINE_API int swmm_forcing_clear_all(SWMM_Engine engine);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_FORCING_H */
