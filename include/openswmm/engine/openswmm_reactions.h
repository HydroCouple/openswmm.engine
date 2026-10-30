/* SPDX-License-Identifier: Apache-2.0
 *
 * Copyright 2026 Caleb Buahin
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file      openswmm_reactions.h
 * @brief     C API — the reaction system ([REACTION_*] / .rxn) surface for GUI
 *        editors (E-C1: validation + discovery).
 *
 * @details The discovery getters are the AUTHORITATIVE completer/highlighter
 *          vocabulary (MSX plan GUI contract): a GUI must enumerate species,
 *          coefficients, terms, hydraulic variables, and functions from here
 *          rather than hard-coding lists, so vocabulary drift is structural,
 *          not disciplinary. The hydraulic-variable and function getters are
 *          engine-less statics — usable before any model is open, and there
 *          is exactly one copy of the truth (the expression compiler's own
 *          tables).
 *
 * @ingroup engine_api
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_REACTIONS_H
#define OPENSWMM_REACTIONS_H

#include "openswmm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Expression scopes for validation. */
#define SWMM_RXN_SCOPE_TERM 0
#define SWMM_RXN_SCOPE_PIPE 1
#define SWMM_RXN_SCOPE_TANK 2
#define SWMM_RXN_SCOPE_SUBCATCHMENT 4 /**< Wet non-LID subcatchment pool. */
#define SWMM_RXN_SCOPE_SUBSURFACE 3   /**< Explicit aquifer expression validation. */

/* Expression forms — mirrors ReactionExprForm. */
#define SWMM_RXN_FORM_NONE 0
#define SWMM_RXN_FORM_RATE 1
#define SWMM_RXN_FORM_EQUIL 2
#define SWMM_RXN_FORM_FORMULA 3

/**
 * @brief Compile an expression against the current reaction/model vocabulary without changing
 * state.
 *
 * TERM validation accepts references to all currently declared terms; it does not validate
 * forward-only file ordering, which is enforced during whole-system compilation. Uses disposable
 * token/span storage and applies populated-hydraulic-variable context checks.
 *
 * @param[in] engine Non-null live engine with exclusive caller access.
 * @param[in] scope SWMM_RXN_SCOPE_TERM, PIPE, TANK, SUBSURFACE or SUBCATCHMENT; selects
 *     context-specific availability checks.
 * @param[in] expr Non-null borrowed NUL-terminated expression text.
 * @param[out] errbuf Optional caller-owned diagnostic buffer; NUL-terminated/truncated when
 *     buflen>0, cleared before substantive validation.
 * @param[in] buflen Diagnostic capacity in bytes including NUL; nonpositive suppresses diagnostic
 *     copying.
 * @param[out] col_out Optional 1-based error column; initialized to -1 when no precise location is
 *     available.
 * @return SWMM_OK if expression/context compilation succeeds; SWMM_ERR_BADHANDLE for null engine or
 *     SWMM_ERR_BADPARAM for null text, invalid scope or a compile/context refusal.
 */
SWMM_ENGINE_API int swmm_reaction_validate_expression(SWMM_Engine engine, int scope,
                                                      const char* expr, char* errbuf, int buflen,
                                                      int* col_out);

/* ---- Model discovery (require an engine with a parsed model) ----------- */

/**
 * @brief Count declared reaction species.
 *
 * @param[in] engine Non-null live engine with exclusive caller access.
 * @return Nonnegative stored row count, or -1 for null engine.
 */
SWMM_ENGINE_API int swmm_reaction_species_count(SWMM_Engine engine);

/**
 * @brief Read one reaction-species declaration.
 *
 * Returns declaration metadata without initializing runtime transport. Every listed output is
 * required.
 *
 * @param[in] engine Non-null live engine with exclusive caller access.
 * @param[in] idx Zero-based reaction-species index.
 * @param[out] name Non-null caller-owned name buffer, NUL-terminated and silently truncated to
 *     name_len-1 bytes.
 * @param[in] name_len Positive name-buffer capacity in bytes including NUL.
 * @param[out] is_wall Non-null destination: 0=BULK, 1=WALL.
 * @param[out] units Non-null caller-owned declared unit-label buffer, NUL-terminated/truncated.
 * @param[in] units_len Positive unit-label capacity including NUL.
 * @param[out] atol Non-null stored species absolute tolerance; 0 means use the global tolerance.
 * @param[out] rtol Non-null stored species relative tolerance; 0 means use the global tolerance.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for invalid
 *     row/species/element index, or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_reaction_species_get(SWMM_Engine engine, int idx, char* name, int name_len,
                                              int* is_wall, char* units, int units_len,
                                              double* atol, double* rtol);

/**
 * @brief Count declared coefficients.
 *
 * @param[in] engine Non-null live engine with exclusive caller access.
 * @return Nonnegative stored row count, or -1 for null engine.
 */
SWMM_ENGINE_API int swmm_reaction_coeff_count(SWMM_Engine engine);

/**
 * @brief Read one reaction coefficient declaration.
 *
 * @param[in] engine Non-null live engine with exclusive caller access.
 * @param[in] idx Zero-based coefficient index.
 * @param[out] name Non-null caller-owned name buffer, NUL-terminated and silently truncated to
 *     name_len-1 bytes.
 * @param[in] name_len Positive name-buffer capacity in bytes including NUL.
 * @param[out] is_param Non-null destination: 1=PARAMETER, 0=CONSTANT.
 * @param[out] value Non-null destination for the stored coefficient value; units are those assumed
 *     by its expressions.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for invalid
 *     row/species/element index, or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_reaction_coeff_get(SWMM_Engine engine, int idx, char* name, int name_len,
                                            int* is_param, double* value);

/**
 * @brief Count declared intermediate terms.
 *
 * @param[in] engine Non-null live engine with exclusive caller access.
 * @return Nonnegative stored row count, or -1 for null engine.
 */
SWMM_ENGINE_API int swmm_reaction_term_count(SWMM_Engine engine);

/**
 * @brief Read an intermediate term name and source expression.
 *
 * @param[in] engine Non-null live engine with exclusive caller access.
 * @param[in] idx Zero-based term index in declaration order.
 * @param[out] name Non-null caller-owned name buffer, NUL-terminated and silently truncated to
 *     name_len-1 bytes.
 * @param[in] name_len Positive name-buffer capacity in bytes including NUL.
 * @param[out] expr Non-null caller-owned expression buffer, NUL-terminated and silently truncated
 *     to expr_len-1 bytes.
 * @param[in] expr_len Positive expression-buffer capacity including NUL.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for invalid
 *     row/species/element index, or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_reaction_term_get(SWMM_Engine engine, int idx, char* name, int name_len,
                                           char* expr, int expr_len);

/**
 * @brief Read a species reaction form and source text in one supported domain.
 *
 * @param[in] engine Non-null live engine with exclusive caller access.
 * @param[in] scope SWMM_RXN_SCOPE_PIPE, TANK or SUBCATCHMENT only; TERM/SUBSURFACE are not
 *     expression-table selectors here.
 * @param[in] species_idx Zero-based reaction-species index, not a pollutant or unified registry
 *     index.
 * @param[out] form Non-null SWMM_RXN_FORM_* output; NONE when no expression is configured.
 * @param[out] expr Non-null caller-owned expression buffer, NUL-terminated and silently truncated
 *     to expr_len-1 bytes.
 * @param[in] expr_len Positive expression-buffer capacity including NUL.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for invalid
 *     row/species/element index, or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_reaction_expr_get(SWMM_Engine engine, int scope, int species_idx,
                                           int* form, char* expr, int expr_len);

/**
 * @brief Format a reaction option as its canonical token or numeric string.
 *
 * Temperature is degrees C, reaction timesteps follow their configured option convention, and
 * numeric formatting uses the implementation standard decimal formatting; a truncated result still
 * succeeds.
 *
 * @param[in] engine Non-null live engine with exclusive caller access.
 * @param[in] key Non-null case-insensitive SOLVER, COUPLING, RATE_UNITS, WALL_GEOMETRY, AREA_UNITS,
 *     PATTERN_TIMESTEP, TIMESTEP, ATOL, RTOL or TEMPERATURE.
 * @param[out] value Non-null caller-owned result buffer, NUL-terminated/truncated to value_len-1
 *     bytes.
 * @param[in] value_len Positive byte capacity including NUL.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for invalid
 *     row/species/element index, or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_reaction_option_get(SWMM_Engine engine, const char* key, char* value,
                                             int value_len);

/* ---- CRUD (E-C2) — BUILDING/OPENED only (rows only seed at start).
 * Mutators use operation-specific validation. Changes affecting expression
 * indexes/source text recompile with reaction/registry snapshots; simple
 * coefficient/source-value assignments do not recompile. Species add/remove
 * rebuild the registry MSX block, and referenced species removal is refused.
 * See each operation for side effects and rollback scope. --------------------- */

/**
 * @brief Append a species and rebuild its unified registry block.
 *
 * Appends empty PIPE/TANK/SUBCATCHMENT forms and zero global/rain concentrations, preserves the
 * reserved registry tail, then recompiles. Reaction/registry snapshots are restored on compile
 * refusal; success marks reactions configured.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED; mutation is refused after
 *     initialization.
 * @param[in] name Non-null, nonempty copied name; collisions with registry species, coefficients or
 *     terms are refused.
 * @param[in] is_wall 0=BULK; any nonzero value=WALL.
 * @param[in] units Non-null copied species unit label; this API does not perform unit dimensional
 *     analysis.
 * @param[in] atol Nonnegative species absolute tolerance; 0 uses global. Caller must provide a
 *     finite value.
 * @param[in] rtol Nonnegative species relative tolerance; 0 uses global. Caller must provide a
 *     finite value.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for invalid
 *     row/species/element index, or SWMM_ERR_BADPARAM for invalid required arguments.
 *     SWMM_ERR_LIFECYCLE outside BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_reaction_species_add(SWMM_Engine engine, const char* name, int is_wall,
                                              const char* units, double atol, double rtol);

/**
 * @brief Remove an unreferenced species and renumber dependent reaction indices.
 *
 * Refuses a species referenced by compiled expressions, subsurface laws or source rows. Removes its
 * expression/init/law rows, prunes matching [INITIAL_QUALITY] species rows, shifts later species
 * indices and rebuilds the registry. Recompilation restores reaction/registry snapshots on refusal;
 * the [INITIAL_QUALITY] pruning is performed separately by this implementation and is not part of
 * those snapshots.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED; mutation is refused after
 *     initialization.
 * @param[in] idx Zero-based existing reaction-species index.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for invalid
 *     row/species/element index, or SWMM_ERR_BADPARAM for invalid required arguments.
 *     SWMM_ERR_LIFECYCLE outside BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_reaction_species_remove(SWMM_Engine engine, int idx);

/**
 * @brief Append a coefficient without recompiling unchanged expression indices.
 *
 * Appending preserves existing coefficient indices; does not automatically mark/activate a complete
 * reaction transport profile.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED; mutation is refused after
 *     initialization.
 * @param[in] name Non-null, nonempty copied name; duplicate coefficient/species/term names are
 *     refused.
 * @param[in] is_param 0=CONSTANT, any nonzero value=PARAMETER.
 * @param[in] value Stored coefficient value, with expression-specific units; assigned without
 *     finite/range validation.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for invalid
 *     row/species/element index, or SWMM_ERR_BADPARAM for invalid required arguments.
 *     SWMM_ERR_LIFECYCLE outside BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_reaction_coeff_add(SWMM_Engine engine, const char* name, int is_param,
                                            double value);

/**
 * @brief Replace one coefficient value without changing its identity or compiled index.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED; mutation is refused after
 *     initialization.
 * @param[in] idx Zero-based coefficient index.
 * @param[in] value New raw coefficient value; no finite/range validation is imposed.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for invalid
 *     row/species/element index, or SWMM_ERR_BADPARAM for invalid required arguments.
 *     SWMM_ERR_LIFECYCLE outside BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_reaction_coeff_set_value(SWMM_Engine engine, int idx, double value);

/**
 * @brief Remove a coefficient only when no compiled expression/override uses it.
 *
 * Referenced coefficients or parameter-override bindings return BADPARAM. Later
 * coefficient/override indices shift; the system is recompiled and reaction/registry snapshots
 * restored on compile refusal.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED; mutation is refused after
 *     initialization.
 * @param[in] idx Zero-based coefficient index.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for invalid
 *     row/species/element index, or SWMM_ERR_BADPARAM for invalid required arguments.
 *     SWMM_ERR_LIFECYCLE outside BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_reaction_coeff_remove(SWMM_Engine engine, int idx);

/**
 * @brief Append a copied intermediate term and eagerly compile the system.
 *
 * Restores reaction/registry snapshots on compilation refusal. Later terms may reference this term
 * after successful append.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED; mutation is refused after
 *     initialization.
 * @param[in] name Non-null, nonempty copied unique term name; collision with a
 *     term/species/coefficient is refused.
 * @param[in] expr Non-null copied expression. Term references must obey declaration ordering when
 *     recompiling the full system.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for invalid
 *     row/species/element index, or SWMM_ERR_BADPARAM for invalid required arguments.
 *     SWMM_ERR_LIFECYCLE outside BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_reaction_term_add(SWMM_Engine engine, const char* name, const char* expr);

/**
 * @brief Replace a term source expression and recompile dependent expressions.
 *
 * Restores reaction/registry snapshots on compilation refusal, keeping the old compiled expression
 * system.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED; mutation is refused after
 *     initialization.
 * @param[in] idx Zero-based existing term index.
 * @param[in] expr Non-null copied expression text; full-system ordering/context rules apply.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for invalid
 *     row/species/element index, or SWMM_ERR_BADPARAM for invalid required arguments.
 *     SWMM_ERR_LIFECYCLE outside BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_reaction_term_set_expr(SWMM_Engine engine, int idx, const char* expr);

/**
 * @brief Remove an intermediate term that has no compiled references.
 *
 * Compiled references return BADPARAM. Later term indices shift; recompilation derives new indices
 * and restores reaction/registry snapshots on refusal.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED; mutation is refused after
 *     initialization.
 * @param[in] idx Zero-based existing term index.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for invalid
 *     row/species/element index, or SWMM_ERR_BADPARAM for invalid required arguments.
 *     SWMM_ERR_LIFECYCLE outside BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_reaction_term_remove(SWMM_Engine engine, int idx);

/**
 * @brief Set or clear a species reaction expression in one domain.
 *
 * Recompiles the full system and restores reaction/registry snapshots on refusal. Does not offer a
 * SUBSURFACE-law setter through this per-species expression-table API.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED; mutation is refused after
 *     initialization.
 * @param[in] scope SWMM_RXN_SCOPE_PIPE, TANK or SUBCATCHMENT; other scopes are refused.
 * @param[in] species_idx Zero-based reaction-species index, not a pollutant or unified registry
 *     index.
 * @param[in] form SWMM_RXN_FORM_NONE, RATE, EQUIL or FORMULA.
 * @param[in] expr Borrowed text copied for non-NONE form; must then be non-null/nonempty. NONE
 *     ignores this pointer and clears the source text.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for invalid
 *     row/species/element index, or SWMM_ERR_BADPARAM for invalid required arguments.
 *     SWMM_ERR_LIFECYCLE outside BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_reaction_expr_set(SWMM_Engine engine, int scope, int species_idx, int form,
                                           const char* expr);

/**
 * @brief Parse and assign one reaction option before initialization.
 *
 * SOLVER accepts EUL/RK5/ROS2/BDF2, COUPLING NONE/FULL, RATE_UNITS SEC/MIN/HR/DAY, AREA_UNITS
 * FT2/M2/CM2, WALL_GEOMETRY YES/NO. PATTERN_TIMESTEP requires finite >0; TIMESTEP checks >=0;
 * ATOL/RTOL check >0; TEMPERATURE requires finite degrees C. Supply finite numbers even where only
 * comparison guards exist. WALL_GEOMETRY recompiles with rollback; the other assignments do not
 * recompile the full system.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED; mutation is refused after
 *     initialization.
 * @param[in] key Non-null case-insensitive option key accepted by swmm_reaction_option_get().
 * @param[in] value Non-null input token/string. Enum values are case-insensitive; numeric strings
 *     must parse completely.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for invalid
 *     row/species/element index, or SWMM_ERR_BADPARAM for invalid required arguments.
 *     SWMM_ERR_LIFECYCLE outside BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_reaction_option_set(SWMM_Engine engine, const char* key,
                                             const char* value);

/* ---- Initial quality (GLOBAL + the E-B NODE/LINK rows) ----------------- */

/**
 * @brief Read a species global initial concentration.
 *
 * @param[in] engine Non-null live engine with exclusive caller access.
 * @param[in] species_idx Zero-based reaction-species index, not a pollutant or unified registry
 *     index.
 * @param[out] value Non-null concentration output in declared species units; zero when the aligned
 *     optional array has no entry.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for invalid
 *     row/species/element index, or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_reaction_init_global_get(SWMM_Engine engine, int species_idx,
                                                  double* value);
/**
 * @brief Assign a species global initial concentration before initialization.
 *
 * Grows the aligned source/seed array with zero defaults if needed. The negative comparison guard
 * does not independently reject NaN/infinity.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED; mutation is refused after
 *     initialization.
 * @param[in] species_idx Zero-based reaction-species index, not a pollutant or unified registry
 *     index.
 * @param[in] value Nonnegative concentration in declared species units; caller must provide a
 *     finite value.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for invalid
 *     row/species/element index, or SWMM_ERR_BADPARAM for invalid required arguments.
 *     SWMM_ERR_LIFECYCLE outside BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_reaction_init_global_set(SWMM_Engine engine, int species_idx,
                                                  double value);
/**
 * @brief Read a species rainfall concentration.
 *
 * @param[in] engine Non-null live engine with exclusive caller access.
 * @param[in] species_idx Zero-based reaction-species index, not a pollutant or unified registry
 *     index.
 * @param[out] value Non-null concentration output in declared species units; zero when the aligned
 *     optional array has no entry.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for invalid
 *     row/species/element index, or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_reaction_rain_conc_get(SWMM_Engine engine, int species_idx, double* value);
/**
 * @brief Assign a species rainfall concentration before initialization.
 *
 * Grows the aligned source/seed array with zero defaults if needed. The >=0 guard rejects NaN but
 * does not independently reject positive infinity.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED; mutation is refused after
 *     initialization.
 * @param[in] species_idx Zero-based reaction-species index, not a pollutant or unified registry
 *     index.
 * @param[in] value Nonnegative concentration in declared species units; caller must provide a
 *     finite value.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for invalid
 *     row/species/element index, or SWMM_ERR_BADPARAM for invalid required arguments.
 *     SWMM_ERR_LIFECYCLE outside BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_reaction_rain_conc_set(SWMM_Engine engine, int species_idx, double value);
/**
 * @brief Count declared per-element reaction seed rows.
 *
 * @param[in] engine Non-null live engine with exclusive caller access.
 * @return Nonnegative stored row count, or -1 for null engine.
 */
SWMM_ENGINE_API int swmm_reaction_init_elem_count(SWMM_Engine engine);
/**
 * @brief Read a reaction per-element initial concentration row.
 *
 * @param[in] engine Non-null live engine with exclusive caller access.
 * @param[in] entry_idx Zero-based initial-row index.
 * @param[out] is_link Non-null destination: 0=node, 1=link.
 * @param[out] elem_idx Non-null stored element-index destination.
 * @param[out] species_idx Non-null reaction-species-index destination.
 * @param[out] value Non-null stored concentration destination, in the declared species units.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for invalid
 *     row/species/element index, or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_reaction_init_elem_get(SWMM_Engine engine, int entry_idx, int* is_link,
                                                int* elem_idx, int* species_idx, double* value);
/**
 * @brief Upsert a reaction per-element initial concentration.
 *
 * Updates an existing (kind,element,species) row in place or appends. Seeds initial state rather
 * than modifying running concentrations; this wrapper updates reaction seed arrays directly and
 * does not mirror a general [INITIAL_QUALITY] authoring row.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED; mutation is refused after
 *     initialization.
 * @param[in] is_link 0 selects node; any nonzero value selects link.
 * @param[in] elem_idx Zero-based existing element index in the selected table.
 * @param[in] species_idx Zero-based reaction-species index, not a pollutant or unified registry
 *     index.
 * @param[in] value Concentration in declared species units; negative values are refused. Caller
 *     must provide a finite value (no separate finiteness guard).
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for invalid
 *     row/species/element index, or SWMM_ERR_BADPARAM for invalid required arguments.
 *     SWMM_ERR_LIFECYCLE outside BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_reaction_init_elem_set(SWMM_Engine engine, int is_link, int elem_idx,
                                                int species_idx, double value);
/**
 * @brief Remove one per-element reaction seed row and shift later indices down.
 *
 * Removes the reaction seed arrays directly; does not delete an independent [INITIAL_QUALITY]
 * authoring row through this wrapper.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED; mutation is refused after
 *     initialization.
 * @param[in] entry_idx Zero-based existing initial-row index.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for invalid
 *     row/species/element index, or SWMM_ERR_BADPARAM for invalid required arguments.
 *     SWMM_ERR_LIFECYCLE outside BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_reaction_init_elem_remove(SWMM_Engine engine, int entry_idx);

/* ---- Whole-file text surface (E-C3) — the GUI text tab's contract.
 * serialize -> apply_text -> serialize is byte-identical (D-RC6). -------- */

/**
 * @brief Serialize the current reaction system into complete NUL-terminated canonical text.
 *
 * Does not change authoring or write a file; the canonical writer determines section ordering and
 * numeric spelling.
 *
 * @param[in] engine Non-null live engine with exclusive caller access.
 * @param[out] buf Caller-owned text destination, or NULL to query size regardless of buflen. Short
 *     non-null buffers receive no bytes.
 * @param[in] buflen Destination capacity in bytes including NUL; must cover needed_len when buf is
 *     non-null.
 * @param[out] needed_len Non-null required byte count INCLUDING NUL, written before short-buffer
 *     refusal.
 * @return SWMM_OK after sizing/full copy; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM
 *     for null needed_len/insufficient non-null buffer.
 */
SWMM_ENGINE_API int swmm_reactions_serialize(SWMM_Engine engine, char* buf, int buflen,
                                             int* needed_len);

/**
 * @brief Parse and compile complete reaction text without retaining the staged system.
 *
 * Stages through the live reaction/registry context and restores both after validation; exclusive
 * editable-state access is therefore required. Returns the first parser/application diagnostic on
 * refusal and leaves stored reaction/registry state unchanged on normal success/refusal paths.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED; mutation is refused after
 *     initialization.
 * @param[in] text Non-null, nonempty borrowed component-format .rxn text.
 * @param[out] errbuf Optional caller-owned diagnostic buffer; NUL-terminated/truncated when
 *     buflen>0, cleared before substantive validation.
 * @param[in] buflen Diagnostic capacity in bytes including NUL; nonpositive suppresses diagnostic
 *     copying.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for invalid
 *     row/species/element index, or SWMM_ERR_BADPARAM for invalid required arguments.
 *     SWMM_ERR_LIFECYCLE outside BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_reactions_check_text(SWMM_Engine engine, const char* text, char* errbuf,
                                              int buflen);

/**
 * @brief Replace the reaction system and its registry block from complete component text.
 *
 * Replaces the current reaction registry block while preserving the reserved-species tail. On
 * parser/application refusal, restores the backed-up reaction system and registry and returns the
 * first diagnostic. Does not write a component file or update its bound path automatically.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED; mutation is refused after
 *     initialization.
 * @param[in] text Non-null, nonempty borrowed .rxn text; parsed/compiled before retaining the
 *     replacement.
 * @param[out] errbuf Optional caller-owned diagnostic buffer; NUL-terminated/truncated when
 *     buflen>0, cleared before substantive validation.
 * @param[in] buflen Diagnostic capacity in bytes including NUL; nonpositive suppresses diagnostic
 *     copying.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for invalid
 *     row/species/element index, or SWMM_ERR_BADPARAM for invalid required arguments.
 *     SWMM_ERR_LIFECYCLE outside BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_reactions_apply_text(SWMM_Engine engine, const char* text, char* errbuf,
                                              int buflen);

/**
 * @brief Write canonical reaction text to an explicit or bound config path.
 *
 * No lifecycle guard. Opens binary output with truncation, does not provide atomic publication, and
 * does not update registration path metadata. An explicit path overrides the bound path only for
 * this call.
 *
 * @param[in] engine Non-null live engine with exclusive caller access.
 * @param[in] path_or_null Optional NUL-terminated UTF-8 destination. NULL/empty uses the reactions
 *     component binding, preferring its resolved path to the authored token.
 * @return SWMM_OK after a good stream write; SWMM_ERR_BADHANDLE for null engine or
 *     SWMM_ERR_BADPARAM when no path is available or open/write fails.
 */
SWMM_ENGINE_API int swmm_reactions_save(SWMM_Engine engine, const char* path_or_null);

/* ---- Static vocabulary (no engine handle needed) ----------------------- */

/**
 * @brief Count hydraulic identifiers exposed by the expression compiler.
 *
 * Engine-independent metadata; does not certify that a variable is populated in every reaction
 * scope.
 * @return Number of compiler hydraulic-variable vocabulary entries.
 */
SWMM_ENGINE_API int swmm_reaction_hydvar_count(void);

/**
 * @brief Copy one compiler hydraulic-variable name and optional description.
 *
 * @param[in] idx Zero-based index in [0,swmm_reaction_hydvar_count()).
 * @param[out] name Non-null caller-owned name buffer, NUL-terminated and silently truncated to
 *     name_len-1 bytes.
 * @param[in] name_len Positive name-buffer capacity in bytes including NUL.
 * @param[out] description Optional caller-owned description buffer, NUL-terminated/truncated when
 *     desc_len>0.
 * @param[in] desc_len Description capacity including NUL; nonpositive suppresses copying.
 * @return SWMM_OK; SWMM_ERR_BADINDEX for invalid idx or SWMM_ERR_BADPARAM for
 *     null/nonpositive-capacity name output.
 */
SWMM_ENGINE_API int swmm_reaction_hydvar_get(int idx, char* name, int name_len, char* description,
                                             int desc_len);

/**
 * @brief Count built-in expression functions.
 * @return Number of function metadata entries in the compiler vocabulary.
 */
SWMM_ENGINE_API int swmm_reaction_function_count(void);

/**
 * @brief Copy a compiler function name and its argument arity.
 *
 * @param[in] idx Zero-based index in [0,swmm_reaction_function_count()).
 * @param[out] name Non-null caller-owned name buffer, NUL-terminated and silently truncated to
 *     name_len-1 bytes.
 * @param[in] name_len Positive name-buffer capacity in bytes including NUL.
 * @param[out] arity Non-null integer output for the number of accepted arguments.
 * @return SWMM_OK; SWMM_ERR_BADINDEX for invalid idx or SWMM_ERR_BADPARAM for invalid required
 *     outputs.
 */
SWMM_ENGINE_API int swmm_reaction_function_get(int idx, char* name, int name_len, int* arity);

#ifdef __cplusplus
}
#endif

#endif /* OPENSWMM_REACTIONS_H */
