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
 * @file      openswmm_controls.h
 * @brief     OpenSWMM Engine — Control Rules C API.
 *
 * @details Control rule addition, retrieval, clearing, and direct
 *          link setting/status overrides.
 *
 * @ingroup engine_api
 * @see openswmm_engine.h
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
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
 * @brief Store a control-rule text block and compile immediately when the engine is initialized.
 *
 * CREATED/BUILDING/OPENED store text verbatim and defer parsing to initialization, allowing
 * unresolved objects during model construction. Other states first validate in a fresh control
 * engine, then append to the live engine. Rejected text is not stored and the last-error message
 * identifies the line/reason. A live block must contain at least one RULE; references to
 * VARIABLE/EXPRESSION declarations in earlier blocks do not resolve in the validation sandbox. One
 * stored text block may contain several compiled rules.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] rule_text Non-null NUL-terminated SWMM [CONTROLS] text; copied internally. May contain
 *     newline-separated rules and declarations.
 * @return SWMM_OK on storage/compilation; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM
 *     for null text or a live parser rejection.
 */
SWMM_ENGINE_API int swmm_control_add_rule(SWMM_Engine engine, const char* rule_text);

/**
 * @brief Count stored control text blocks.
 *
 * The count is the number of entries added to the text store, not necessarily the number of
 * compiled RULE clauses inside them.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @return Number of stored blocks, or -1 for null engine.
 */
SWMM_ENGINE_API int swmm_control_count(SWMM_Engine engine);

/**
 * @brief Copy a stored control text block by index.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] idx Zero-based stored text-block index in [0,swmm_control_count(engine)).
 * @param[out] buf Non-null caller-owned destination, NUL-terminated on success. Excess text is
 *     silently truncated to buflen-1 bytes.
 * @param[in] buflen Positive destination capacity in bytes, including the NUL terminator.
 * @return SWMM_OK, including when truncated; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX
 *     for invalid idx, or SWMM_ERR_BADPARAM for null/empty-capacity buffer.
 */
SWMM_ENGINE_API int swmm_control_get_rule(SWMM_Engine engine, int idx, char* buf, int buflen);

/**
 * @brief Copy the RULE identifier at the beginning of a stored text block.
 *
 * Skips leading whitespace, matches RULE case-insensitively as a complete token and takes the next
 * whitespace-delimited token. Leading declarations/comments cause refusal; the function does not
 * search for a later RULE. Outputs are unchanged on refusal.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] idx Zero-based stored text-block index in [0,swmm_control_count(engine)).
 * @param[out] buf Non-null caller-owned destination, NUL-terminated on success. Excess text is
 *     silently truncated to buflen-1 bytes.
 * @param[in] buflen Positive destination capacity in bytes, including the NUL terminator.
 * @return SWMM_OK, including when truncated; SWMM_ERR_BADHANDLE, SWMM_ERR_BADINDEX, or
 *     SWMM_ERR_BADPARAM for invalid buffer or text without a RULE/name prefix.
 */
SWMM_ENGINE_API int swmm_control_get_id(SWMM_Engine engine, int idx, char* buf, int buflen);

/**
 * @brief Clear the stored control text blocks.
 *
 * Clears the text store without a lifecycle guard. It does not clear the already compiled live
 * ControlEngine or its PID state; clear before initialization when the intent is to run without
 * these rules.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @return SWMM_OK, or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_control_clear_rules(SWMM_Engine engine);

/**
 * @brief Remove one stored control text block while the model is editable.
 *
 * Later stored-block indices shift down by one. This operation changes the text store before
 * compilation.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] idx Zero-based stored text-block index in [0,swmm_control_count(engine)).
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside BUILDING/OPENED,
 *     or SWMM_ERR_BADINDEX for invalid idx.
 */
SWMM_ENGINE_API int swmm_control_remove_rule(SWMM_Engine engine, int idx);

/**
 * @brief Find stored rule blocks that reference an object name.
 *
 * Read-only token-based search using the object-reference scanner. Does not rewrite rules or
 * resolve references against the object tables.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] object_name Non-null NUL-terminated name, matched case-insensitively after an object
 *     keyword (NODE, LINK, CONDUIT, PUMP, ORIFICE, WEIR, OUTLET or GAGE).
 * @param[out] rule_indices_out Caller-owned array receiving ascending matching indices; NULL
 *     requests only the total count. A short array receives a prefix without an error.
 * @param[in,out] n_inout Non-null pointer: on input nonnegative array capacity (ignored for NULL
 *     array); on output total matches, which may exceed the number copied.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for null name/count.
 */
SWMM_ENGINE_API int swmm_control_find_references(SWMM_Engine engine, const char* object_name,
                                                 int* rule_indices_out, int* n_inout);

/**
 * @brief Parse a control text block in a disposable engine without storing it.
 *
 * Leaves stored rules, live compiled rules and PID state unchanged. Validation uses a fresh control
 * engine, so a block referencing VARIABLE/EXPRESSION declarations from an earlier stored block
 * cannot resolve them. Rejection messages use line N: reason when a line is available.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] rule_text Non-null NUL-terminated [CONTROLS] text. Object/curve/time-series names
 *     resolve against this engine model.
 * @param[out] errbuf Optional caller-owned rejection-message buffer; NULL or nonpositive buflen
 *     suppresses capture. NUL-terminated and truncated to fit when provided; emptied on success.
 * @param[in] buflen Capacity in bytes, including the NUL terminator; nonpositive means no message
 *     output.
 * @param[out] line_out Optional destination for the 1-based rejected line, counting blank lines; -1
 *     on success or when there is no RULE and thus no single offending line.
 * @return SWMM_OK when at least one RULE parses; SWMM_ERR_BADPARAM for null text, parser rejection
 *     or no RULE; SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_control_validate_rule(SWMM_Engine engine, const char* rule_text,
                                               char* errbuf, int buflen, int* line_out);

/* =========================================================================
 * Direct control actions (without rules)
 * ========================================================================= */

/**
 * @brief Assign the current setting field for one link directly.
 *
 * The implementation applies no RUNNING guard and directly stores the value. It does not remove a
 * rule or establish a persistent override; subsequent rule evaluation or link physics can change
 * the effective setting.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] link_idx Zero-based link index in the current model.
 * @param[in] setting Setting to store without range/finite-value validation: pumps conventionally
 *     use 0=off and 1=full, orifices/weirs use a fractional opening in [0,1].
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid link index.
 */
SWMM_ENGINE_API int swmm_control_set_link_setting(SWMM_Engine engine, int link_idx, double setting);

/**
 * @brief Assign the current closed/open flag for one link directly.
 *
 * No lifecycle guard or persistent override is installed. This updates is_closed directly; it does
 * not change the stored setting or remove control rules, and subsequent simulation/control work may
 * change link behavior.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] link_idx Zero-based link index in the current model.
 * @param[in] status 0 stores closed; every nonzero value stores open.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid link index.
 */
SWMM_ENGINE_API int swmm_control_set_link_status(SWMM_Engine engine, int link_idx, int status);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_CONTROLS_H */
