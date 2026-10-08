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
 * @file openswmm_sq2d.h
 * @brief S7 (2026-09-19) — C API for the 2D surface-quality rows
 *        (`[2D_COVERAGES]`, `[2D_LOADINGS]`, `[2D_CURB_LENGTH]`) and the
 *        cell buildup store.
 *
 * @details Rows are authored per SCOPE — the `*` row, a TAG row (every cell
 *          carrying the tag) or a CELL row — and resolve GLOBAL < TAG < CELL
 *          at initialize, exactly as the `.inp` sections do (one parser, one
 *          rule). A coverage row is the WHOLE land-use set of its scope:
 *          setting it replaces any earlier row with the same scope key.
 *
 *          Row edits need the engine OPENED or BUILDING (SWMM_ERR_LIFECYCLE
 *          otherwise); the read-backs work in every state, and the buildup
 *          store reads back once the run has started.
 *
 *          Units follow the project: percent for coverages, user mass per
 *          acre | hectare for loadings and buildup (lbs | kg; a reactions
 *          species declared in UG scales through its own unit factor), and
 *          the project's length unit for curb lengths.
 */

#ifndef OPENSWMM_SQ2D_H
#define OPENSWMM_SQ2D_H

#include "openswmm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SWMM_SQ2D_SCOPE_GLOBAL 0   /**< the `*` row */
#define SWMM_SQ2D_SCOPE_TAG    1   /**< every cell carrying a tag */
#define SWMM_SQ2D_SCOPE_CELL   2   /**< one cell, 0-based here (1-based in the file) */

/* =========================================================================
 * [2D_COVERAGES]
 * ========================================================================= */

/** Number of authored coverage rows. */
SWMM_ENGINE_API int swmm_2d_coverage_count(SWMM_Engine engine, int* count);

/**
 * @brief Set the coverage set of one scope. The row is `n` (land use,
 *        percent) pairs; the percents must sum to 100 or less and every
 *        land use must exist. Replaces an existing row with the same scope.
 * @param scope     SWMM_SQ2D_SCOPE_*
 * @param tag       tag name for SCOPE_TAG (ignored otherwise; may be NULL)
 * @param cell      0-based cell for SCOPE_CELL (ignored otherwise)
 * @param landuses  `n` land-use names
 * @param percents  `n` percents
 */
SWMM_ENGINE_API int swmm_2d_coverage_set(SWMM_Engine engine, int scope,
                                         const char* tag, int cell,
                                         const char* const* landuses,
                                         const double* percents, int n);

/** Number of (land use, percent) pairs in row @p index. */
SWMM_ENGINE_API int swmm_2d_coverage_row_size(SWMM_Engine engine, int index, int* n);

/**
 * @brief Read row @p index back. Any out pointer may be NULL; @p tag needs
 *        @p taglen bytes; @p landuse needs @p lulen bytes and receives the
 *        name of pair @p k (0-based), @p percent its percent.
 */
SWMM_ENGINE_API int swmm_2d_coverage_get(SWMM_Engine engine, int index, int k,
                                         int* scope, char* tag, int taglen,
                                         int* cell, char* landuse, int lulen,
                                         double* percent);

/** Remove row @p index. */
SWMM_ENGINE_API int swmm_2d_coverage_remove(SWMM_Engine engine, int index);

/* =========================================================================
 * [2D_LOADINGS]
 * ========================================================================= */

SWMM_ENGINE_API int swmm_2d_loading_count(SWMM_Engine engine, int* count);

/** Set the initial buildup of @p species (a pollutant or a reactions
 *  species that builds up) on one scope, user mass per acre | hectare.
 *  Replaces an existing row with the same scope and species. */
SWMM_ENGINE_API int swmm_2d_loading_set(SWMM_Engine engine, int scope,
                                        const char* tag, int cell,
                                        const char* species, double value);

SWMM_ENGINE_API int swmm_2d_loading_get(SWMM_Engine engine, int index,
                                        int* scope, char* tag, int taglen,
                                        int* cell, char* species, int splen,
                                        double* value);

SWMM_ENGINE_API int swmm_2d_loading_remove(SWMM_Engine engine, int index);

/* =========================================================================
 * [2D_CURB_LENGTH]
 * ========================================================================= */

SWMM_ENGINE_API int swmm_2d_curb_length_count(SWMM_Engine engine, int* count);

/** Set the curb length (project length units, > 0) of one scope. Replaces
 *  an existing row with the same scope. */
SWMM_ENGINE_API int swmm_2d_curb_length_set(SWMM_Engine engine, int scope,
                                            const char* tag, int cell,
                                            double length);

SWMM_ENGINE_API int swmm_2d_curb_length_get(SWMM_Engine engine, int index,
                                            int* scope, char* tag, int taglen,
                                            int* cell, double* length);

SWMM_ENGINE_API int swmm_2d_curb_length_remove(SWMM_Engine engine, int index);

/* =========================================================================
 * Buildup store (running model)
 * ========================================================================= */

/**
 * @brief Per-cell buildup of @p species (name of a pollutant or a
 *        reactions species), user mass per acre | hectare, summed over the
 *        cell's land uses — the value the results file reports. @p n is
 *        the length of @p out (at least the cell count). Returns
 *        SWMM_ERR_LIFECYCLE before the run started or when no row
 *        resolved (no coverages, IGNORE_QUALITY, RAINFALL_MODE NONE).
 */
SWMM_ENGINE_API int swmm_2d_get_buildup_bulk(SWMM_Engine engine,
                                             const char* species,
                                             double* out, int n);

#ifdef __cplusplus
}
#endif

#endif /* OPENSWMM_SQ2D_H */
