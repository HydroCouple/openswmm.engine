/**
 * @file openswmm_controls.h
 * @brief OpenSWMM Engine — Control Rules C API.
 *
 * @details Control rule addition, retrieval, clearing, and direct
 *          link setting/status overrides.
 *
 * @ingroup engine_api
 * @see openswmm_engine.h
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_CONTROLS_H
#define OPENSWMM_CONTROLS_H

#include "openswmm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Control rules
 * ========================================================================= */

/**
 * @brief Add a control rule from its text representation.
 *
 * @details The rule text follows the standard SWMM control rule syntax
 *          (e.g., "RULE R1\\nIF NODE J1 DEPTH > 5\\nTHEN PUMP P1 STATUS = ON").
 *          Lines are separated by newline characters within the string.
 *
 * @param engine     Engine handle.
 * @param rule_text  Null-terminated string containing the full rule text.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_control_add_rule(SWMM_Engine engine, const char* rule_text);

/**
 * @brief Get the total number of control rules defined.
 * @param engine  Engine handle.
 * @returns Number of control rules, or -1 on error.
 */
SWMM_ENGINE_API int swmm_control_count(SWMM_Engine engine);

/**
 * @brief Get the text of a control rule by index.
 * @param engine  Engine handle.
 * @param idx     Zero-based rule index.
 * @param buf     Caller-allocated buffer to receive the rule text.
 * @param buflen  Size of @p buf in bytes.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_control_get_rule(SWMM_Engine engine, int idx, char* buf, int buflen);

/**
 * @brief Remove all control rules from the model.
 * @param engine  Engine handle.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_control_clear_rules(SWMM_Engine engine);

/* =========================================================================
 * Direct control actions (without rules)
 * ========================================================================= */

/**
 * @brief Directly set the control setting of a link.
 *
 * @details Bypasses the control rule engine. For pumps: 0.0=off, 1.0=full.
 *          For orifices/weirs: fractional opening [0, 1].
 *
 * @param engine    Engine handle (RUNNING state).
 * @param link_idx  Zero-based link index.
 * @param setting   Setting value.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_control_set_link_setting(SWMM_Engine engine, int link_idx, double setting);

/**
 * @brief Directly set the open/close status of a link.
 *
 * @details Bypasses the control rule engine. Status: 0=closed, 1=open.
 *
 * @param engine    Engine handle (RUNNING state).
 * @param link_idx  Zero-based link index.
 * @param status    0 for closed, non-zero for open.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_control_set_link_status(SWMM_Engine engine, int link_idx, int status);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_CONTROLS_H */
