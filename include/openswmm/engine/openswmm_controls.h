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
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
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
 * @brief Get the canonical name of a control rule by index.
 *
 * @details Parses the stored rule text and returns the first whitespace-delimited
 *          token that follows the `RULE` keyword (case-insensitive match, leading
 *          whitespace tolerated). On parse failure — when the rule text does not
 *          begin with a `RULE` keyword token — returns `SWMM_ERR_BADPARAM` so
 *          callers can surface a sentinel display name for malformed rules.
 *
 * @param engine  Engine handle.
 * @param idx     Zero-based rule index.
 * @param buf     [out] Caller-allocated buffer to receive the rule name.
 * @param buflen  Size of @p buf in bytes.
 * @returns SWMM_OK on success, `SWMM_ERR_BADPARAM` if the rule text has no
 *          parseable RULE token, or another error code.
 */
SWMM_ENGINE_API int swmm_control_get_id(SWMM_Engine engine, int idx, char* buf, int buflen);

/**
 * @brief Remove all control rules from the model.
 * @param engine  Engine handle.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_control_clear_rules(SWMM_Engine engine);

/**
 * @brief Remove one control rule by index.
 *
 * @details Later rule indices shift down by one. BUILDING/OPENED state only.
 *
 * @param engine  Engine handle.
 * @param idx     Zero-based rule index (0..swmm_control_count()-1).
 * @returns SWMM_OK, SWMM_ERR_LIFECYCLE, SWMM_ERR_BADHANDLE, or
 *          SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_control_remove_rule(SWMM_Engine engine, int idx);

/**
 * @brief Find control rules whose text references an object by name.
 *
 * @details Scans each rule's clauses for an object-type keyword (NODE, LINK,
 *          CONDUIT, PUMP, ORIFICE, WEIR, OUTLET) immediately followed by
 *          @p object_name (case-insensitive, matching legacy rule parsing).
 *          Read-only — no delete ever edits rule text; callers decide what to
 *          do with affected rules.
 *
 * @param engine            Engine handle.
 * @param object_name       Object name to search for.
 * @param rule_indices_out  Receives matching rule indices, ascending (may be
 *                          NULL to query the count only).
 * @param n_inout           In: capacity of rule_indices_out (ignored when it
 *                          is NULL). Out: total number of matching rules.
 * @returns SWMM_OK, SWMM_ERR_BADHANDLE, or SWMM_ERR_BADPARAM if object_name
 *          or n_inout is NULL.
 */
SWMM_ENGINE_API int swmm_control_find_references(SWMM_Engine engine,
                                                 const char* object_name,
                                                 int* rule_indices_out,
                                                 int* n_inout);

/**
 * @brief Validate a control-rule text block without storing it.
 *
 * @details Runs the engine's control-rule parser against the engine's live
 *          @ref SimulationContext for name resolution (NODE / LINK / CURVE /
 *          TIMESERIES references), but **does not** mutate the engine's rule
 *          list or PID state. Designed for GUI-side live validation where
 *          the editor needs the production parser's accept/reject verdict
 *          on each keystroke without side effects.
 *
 *          The validation contract matches @ref swmm_control_add_rule
 *          followed by a simulation-initialisation parse: identical input
 *          text yields identical accept/reject. Engine state across the
 *          call is invariant — `swmm_control_count`, `swmm_control_get_rule`,
 *          and the engine's internal `ControlEngine::rules()` vector are
 *          unchanged.
 *
 *          On reject the function returns @ref SWMM_ERR_BADPARAM and writes
 *          a short human-readable message to @p errbuf (truncated to fit).
 *          Line-precise error reporting is not yet wired through the parser;
 *          @p line_out is set to `-1` on reject. Future work may carry a
 *          1-based line index through the parser.
 *
 * @param engine     Engine handle.
 * @param rule_text  Null-terminated rule text. Same grammar as the
 *                   `[CONTROLS]` section.
 * @param errbuf     [out, optional] Buffer for the rejection message. May
 *                   be NULL or zero-length to suppress message capture.
 * @param buflen     Size of @p errbuf in bytes.
 * @param line_out   [out, optional] 1-based line number of the rejection,
 *                   or `-1` if not available. May be NULL.
 * @returns SWMM_OK if the parser accepts the text, SWMM_ERR_BADPARAM if it
 *          rejects, or another error code on infrastructure failure.
 */
SWMM_ENGINE_API int swmm_control_validate_rule(SWMM_Engine engine,
                                                const char* rule_text,
                                                char* errbuf, int buflen,
                                                int* line_out);

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
