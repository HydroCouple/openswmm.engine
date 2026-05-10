/**
 * @file openswmm_pollutants.h
 * @brief OpenSWMM Engine — Pollutant / Water Quality C API.
 *
 * @details Pollutant identity, creation, property setters, and runtime
 *          quality injection at nodes and links.
 *
 * @ingroup engine_api
 * @see openswmm_engine.h
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_POLLUTANTS_H
#define OPENSWMM_POLLUTANTS_H

#include "openswmm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Identity
 * ========================================================================= */

/**
 * @brief Get the total number of pollutants in the model.
 * @param engine  Engine handle.
 * @returns Number of pollutants, or -1 on error.
 */
SWMM_ENGINE_API int swmm_pollutant_count(SWMM_Engine engine);

/**
 * @brief Look up a pollutant's zero-based index by its string identifier.
 * @param engine  Engine handle.
 * @param id      Null-terminated pollutant identifier.
 * @returns Zero-based index, or -1 if not found.
 */
SWMM_ENGINE_API int swmm_pollutant_index(SWMM_Engine engine, const char* id);

/**
 * @brief Get the string identifier of a pollutant by index.
 * @param engine  Engine handle.
 * @param idx     Zero-based pollutant index.
 * @returns Null-terminated string owned by the engine, or NULL on error.
 */
SWMM_ENGINE_API const char* swmm_pollutant_id(SWMM_Engine engine, int idx);

/* =========================================================================
 * Creation (BUILDING state only)
 * ========================================================================= */

/**
 * @brief Add a new pollutant to the model.
 * @param engine  Engine handle (SWMM_STATE_BUILDING).
 * @param id      Unique null-terminated identifier for the new pollutant.
 * @param units   Concentration units code (0=MG/L, 1=UG/L, 2=#/L).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_pollutant_add(SWMM_Engine engine, const char* id, int units);

/* =========================================================================
 * Property setters (BUILDING or OPENED)
 * ========================================================================= */

/**
 * @brief Set the first-order decay coefficient for a pollutant.
 * @param engine  Engine handle.
 * @param idx     Zero-based pollutant index.
 * @param k       Decay coefficient (1/day).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_pollutant_set_kdecay(SWMM_Engine engine, int idx, double k);

/**
 * @brief Set the pollutant concentration in rainfall.
 * @param engine  Engine handle.
 * @param idx     Zero-based pollutant index.
 * @param conc    Concentration in pollutant units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_pollutant_set_rain_conc(SWMM_Engine engine, int idx, double conc);

/**
 * @brief Set the pollutant concentration in groundwater.
 * @param engine  Engine handle.
 * @param idx     Zero-based pollutant index.
 * @param conc    Concentration in pollutant units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_pollutant_set_gw_conc(SWMM_Engine engine, int idx, double conc);

/**
 * @brief Set the initial concentration throughout the conveyance system.
 * @param engine  Engine handle.
 * @param idx     Zero-based pollutant index.
 * @param conc    Initial concentration in pollutant units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_pollutant_set_init_conc(SWMM_Engine engine, int idx, double conc);

/**
 * @brief Get the concentration units code for a pollutant.
 * @param engine      Engine handle.
 * @param idx         Zero-based pollutant index.
 * @param[out] units  Receives the units code (0=MG/L, 1=UG/L, 2=#/L).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_pollutant_get_units(SWMM_Engine engine, int idx, int* units);

/* =========================================================================
 * Property getters
 * ========================================================================= */

/**
 * @brief Get the first-order decay coefficient.
 * @param engine  Engine handle.
 * @param idx     Zero-based pollutant index.
 * @param[out] k  Receives the decay coefficient (1/day).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_pollutant_get_kdecay(SWMM_Engine engine, int idx, double* k);

/**
 * @brief Get the pollutant concentration in rainfall.
 * @param engine     Engine handle.
 * @param idx        Zero-based pollutant index.
 * @param[out] conc  Receives the concentration.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_pollutant_get_rain_conc(SWMM_Engine engine, int idx, double* conc);

/**
 * @brief Get the pollutant concentration in groundwater.
 * @param engine     Engine handle.
 * @param idx        Zero-based pollutant index.
 * @param[out] conc  Receives the concentration.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_pollutant_get_gw_conc(SWMM_Engine engine, int idx, double* conc);

/**
 * @brief Get the initial concentration throughout the conveyance system.
 * @param engine     Engine handle.
 * @param idx        Zero-based pollutant index.
 * @param[out] conc  Receives the initial concentration.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_pollutant_get_init_conc(SWMM_Engine engine, int idx, double* conc);

/**
 * @brief Set the RDII (Rainfall-Dependent Infiltration/Inflow) concentration.
 * @param engine  Engine handle.
 * @param idx     Zero-based pollutant index.
 * @param conc    RDII concentration in pollutant units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_pollutant_set_rdii_conc(SWMM_Engine engine, int idx, double conc);

/**
 * @brief Get the RDII concentration.
 * @param engine     Engine handle.
 * @param idx        Zero-based pollutant index.
 * @param[out] conc  Receives the RDII concentration.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_pollutant_get_rdii_conc(SWMM_Engine engine, int idx, double* conc);

/**
 * @brief Set the molecular weight of a pollutant.
 * @param engine  Engine handle.
 * @param idx     Zero-based pollutant index.
 * @param mwt     Molecular weight (g/mol).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_pollutant_set_mwt(SWMM_Engine engine, int idx, double mwt);

/**
 * @brief Get the molecular weight of a pollutant.
 * @param engine    Engine handle.
 * @param idx       Zero-based pollutant index.
 * @param[out] mwt  Receives the molecular weight.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_pollutant_get_mwt(SWMM_Engine engine, int idx, double* mwt);

/**
 * @brief Set a co-pollutant relationship (concentration = fraction * co-pollutant).
 * @param engine  Engine handle.
 * @param idx     Zero-based pollutant index.
 * @param co_idx  Zero-based index of the co-pollutant.
 * @param frac    Fraction of the co-pollutant concentration.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_pollutant_set_co_pollutant(SWMM_Engine engine, int idx, int co_idx, double frac);

/**
 * @brief Get a co-pollutant relationship.
 * @param engine       Engine handle.
 * @param idx          Zero-based pollutant index.
 * @param[out] co_idx  Receives the co-pollutant index (-1 if none).
 * @param[out] frac    Receives the fraction.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_pollutant_get_co_pollutant(SWMM_Engine engine, int idx, int* co_idx, double* frac);

/**
 * @brief Set whether a pollutant only builds up during snowfall events.
 * @param engine  Engine handle.
 * @param idx     Zero-based pollutant index.
 * @param flag    Non-zero for snow-only; zero otherwise.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_pollutant_set_snow_only(SWMM_Engine engine, int idx, int flag);

/**
 * @brief Get whether a pollutant only builds up during snowfall events.
 * @param engine     Engine handle.
 * @param idx        Zero-based pollutant index.
 * @param[out] flag  Receives 1 for snow-only, 0 otherwise.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_pollutant_get_snow_only(SWMM_Engine engine, int idx, int* flag);

/* =========================================================================
 * Runtime quality injection
 * ========================================================================= */

/**
 * @brief Set the pollutant concentration at a node (runtime override).
 * @param engine       Engine handle (RUNNING state).
 * @param node_idx     Zero-based node index.
 * @param pollut_idx   Zero-based pollutant index.
 * @param conc         Concentration in pollutant units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_set_quality(SWMM_Engine engine, int node_idx, int pollut_idx, double conc);

/**
 * @brief Set the pollutant concentration in a link (runtime override).
 * @param engine       Engine handle (RUNNING state).
 * @param link_idx     Zero-based link index.
 * @param pollut_idx   Zero-based pollutant index.
 * @param conc         Concentration in pollutant units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_set_quality(SWMM_Engine engine, int link_idx, int pollut_idx, double conc);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_POLLUTANTS_H */
