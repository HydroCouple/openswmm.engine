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
 * @file openswmm_reactions.h
 * @brief C API — the reaction system ([REACTION_*] / .rxn) surface for GUI
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
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
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

/* Expression forms — mirrors ReactionExprForm. */
#define SWMM_RXN_FORM_NONE    0
#define SWMM_RXN_FORM_RATE    1
#define SWMM_RXN_FORM_EQUIL   2
#define SWMM_RXN_FORM_FORMULA 3

/**
 * @brief Compile-only validation of a reaction expression against the
 *        model's live vocabulary (species, coefficients, terms, pollutants,
 *        hydraulic variables). No state change.
 * @details Scope selects the identifier-resolution vocabulary. For
 *          SWMM_RXN_SCOPE_TERM, references to ALL terms are accepted — the
 *          forward-only ordering rule stays enforced at file-apply time,
 *          where ordinal position exists (D-RC1): validation answers "is
 *          this well-formed against the vocabulary," not "is the file
 *          orderable."
 * @param engine  Engine handle.
 * @param scope   SWMM_RXN_SCOPE_*.
 * @param expr    Expression text.
 * @param errbuf  [out, optional] Diagnostic (NUL-terminated).
 * @param buflen  Size of errbuf in bytes.
 * @param col_out [out, optional] 1-based error column; -1 when not
 *                attributable.
 * @return SWMM_OK if the expression compiles; SWMM_ERR_BADPARAM otherwise.
 */
SWMM_ENGINE_API int swmm_reaction_validate_expression(SWMM_Engine engine,
        int scope, const char* expr, char* errbuf, int buflen, int* col_out);

/* ---- Model discovery (require an engine with a parsed model) ----------- */

/** @brief Number of declared MSX species (0 when no reactions configured). */
SWMM_ENGINE_API int swmm_reaction_species_count(SWMM_Engine engine);

/** @brief Read one species: name, BULK(0)/WALL(1), units, tolerances. */
SWMM_ENGINE_API int swmm_reaction_species_get(SWMM_Engine engine, int idx,
        char* name, int name_len, int* is_wall,
        char* units, int units_len, double* atol, double* rtol);

/** @brief Number of [REACTION_COEFFICIENTS] entries. */
SWMM_ENGINE_API int swmm_reaction_coeff_count(SWMM_Engine engine);

/** @brief Read one coefficient: name, PARAMETER(1)/CONSTANT(0), value. */
SWMM_ENGINE_API int swmm_reaction_coeff_get(SWMM_Engine engine, int idx,
        char* name, int name_len, int* is_param, double* value);

/** @brief Number of [REACTION_TERMS] entries. */
SWMM_ENGINE_API int swmm_reaction_term_count(SWMM_Engine engine);

/** @brief Read one term: name and its source expression. */
SWMM_ENGINE_API int swmm_reaction_term_get(SWMM_Engine engine, int idx,
        char* name, int name_len, char* expr, int expr_len);

/**
 * @brief Read a species' expression in one scope (PIPE or TANK).
 * @param form [out] SWMM_RXN_FORM_* (NONE when the species has none).
 */
SWMM_ENGINE_API int swmm_reaction_expr_get(SWMM_Engine engine, int scope,
        int species_idx, int* form, char* expr, int expr_len);

/**
 * @brief Read a [REACTION_OPTIONS] value as its canonical token.
 * @param key SOLVER, COUPLING, RATE_UNITS, AREA_UNITS, TIMESTEP, ATOL, RTOL.
 */
SWMM_ENGINE_API int swmm_reaction_option_get(SWMM_Engine engine,
        const char* key, char* value, int value_len);

/* ---- CRUD (E-C2) — BUILDING/OPENED only (rows only seed at start).
 * Every mutator validates eagerly: the whole system is recompiled before
 * the call returns, and a mutation that leaves any expression uncompilable
 * is rolled back and refused (SWMM_ERR_BADPARAM) — the GUI can never store
 * an uncompilable model (D-RC5). Species add/remove rebuild the species
 * registry's MSX block in place (D-RC3); removal of a species still
 * referenced by any compiled expression is refused. --------------------- */

/** @brief Declare a species (is_wall: 0 BULK, 1 WALL; atol/rtol 0 = global). */
SWMM_ENGINE_API int swmm_reaction_species_add(SWMM_Engine engine,
        const char* name, int is_wall, const char* units, double atol,
        double rtol);

/** @brief Remove species @p idx. Refused while any expression references it. */
SWMM_ENGINE_API int swmm_reaction_species_remove(SWMM_Engine engine, int idx);

/** @brief Add a coefficient (is_param: 1 PARAMETER, 0 CONSTANT). */
SWMM_ENGINE_API int swmm_reaction_coeff_add(SWMM_Engine engine,
        const char* name, int is_param, double value);

/** @brief Change a coefficient's value. */
SWMM_ENGINE_API int swmm_reaction_coeff_set_value(SWMM_Engine engine, int idx,
        double value);

/** @brief Remove coefficient @p idx. Refused while referenced. */
SWMM_ENGINE_API int swmm_reaction_coeff_remove(SWMM_Engine engine, int idx);

/** @brief Add an intermediate term (referencable by later terms only). */
SWMM_ENGINE_API int swmm_reaction_term_add(SWMM_Engine engine,
        const char* name, const char* expr);

/** @brief Replace term @p idx's expression. */
SWMM_ENGINE_API int swmm_reaction_term_set_expr(SWMM_Engine engine, int idx,
        const char* expr);

/** @brief Remove term @p idx. Refused while referenced. */
SWMM_ENGINE_API int swmm_reaction_term_remove(SWMM_Engine engine, int idx);

/**
 * @brief Set (or clear, with SWMM_RXN_FORM_NONE) a species' expression in
 *        one scope (PIPE or TANK).
 */
SWMM_ENGINE_API int swmm_reaction_expr_set(SWMM_Engine engine, int scope,
        int species_idx, int form, const char* expr);

/** @brief Set a [REACTION_OPTIONS] value from its canonical token. */
SWMM_ENGINE_API int swmm_reaction_option_set(SWMM_Engine engine,
        const char* key, const char* value);

/* ---- Initial quality (GLOBAL + the E-B NODE/LINK rows) ----------------- */

SWMM_ENGINE_API int swmm_reaction_init_global_get(SWMM_Engine engine,
        int species_idx, double* value);
SWMM_ENGINE_API int swmm_reaction_init_global_set(SWMM_Engine engine,
        int species_idx, double value);
SWMM_ENGINE_API int swmm_reaction_init_elem_count(SWMM_Engine engine);
SWMM_ENGINE_API int swmm_reaction_init_elem_get(SWMM_Engine engine,
        int entry_idx, int* is_link, int* elem_idx, int* species_idx,
        double* value);
/** @brief Upsert on (is_link, elem_idx, species_idx). Value must be >= 0. */
SWMM_ENGINE_API int swmm_reaction_init_elem_set(SWMM_Engine engine,
        int is_link, int elem_idx, int species_idx, double value);
SWMM_ENGINE_API int swmm_reaction_init_elem_remove(SWMM_Engine engine,
        int entry_idx);

/* ---- Static vocabulary (no engine handle needed) ----------------------- */

/** @brief Number of built-in hydraulic variables (D Q U RE US FF AV HRT DT). */
SWMM_ENGINE_API int swmm_reaction_hydvar_count(void);

/** @brief Read one hydraulic variable's name and description. */
SWMM_ENGINE_API int swmm_reaction_hydvar_get(int idx, char* name,
        int name_len, char* description, int desc_len);

/** @brief Number of built-in functions (EXP … TAN arity 1; MIN MAX POW 2). */
SWMM_ENGINE_API int swmm_reaction_function_count(void);

/** @brief Read one function's name and arity. */
SWMM_ENGINE_API int swmm_reaction_function_get(int idx, char* name,
        int name_len, int* arity);

#ifdef __cplusplus
}
#endif

#endif /* OPENSWMM_REACTIONS_H */
