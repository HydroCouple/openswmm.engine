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
 * @file openswmm_infil2d.h
 * @brief Per-cell infiltration for the 2D overland-flow mesh — C API.
 *
 * @details Implements the API surface of
 *          `plans/TWO_ZONE_GROUNDWATER_EXPLICIT_LTS_PLAN_2026-08-15.md` §5.5.6
 *          (track I, step I6). This is the `GROUNDWATER OFF` infiltration path:
 *          a per-cell loss model so surface-only 2D runs do not overestimate
 *          pervious runoff. It supersedes the `swmm_gw2d_infil_set/get`
 *          sketch in §8.5 — the capability ships first and owns this header;
 *          `openswmm_gw2d.h` will refer to it rather than redeclare it.
 *
 *          Configuration mirrors the three input sections:
 *
 *          | section                      | API                                          |
 *          |------------------------------|----------------------------------------------|
 *          | `[2D_INFILTRATION_OPTIONS]`  | @ref swmm_infil2d_get_options / `set_options` |
 *          | `[2D_INFILTRATION_DEFAULTS]` | the `*_default` family (tag rows, incl. `*`)  |
 *          | `[2D_INFILTRATION]`          | the `*_cell` family (sparse per-cell override)|
 *
 *          **Resolution order (D-I3), most specific wins:**
 *          `per-cell override > tag row > '*' row > no infiltration`.
 *          Resolution happens ONCE, when the 2D surface initializes; the
 *          solver never consults tags afterwards.
 *
 *          **Units — read this before touching any row.** Every value in
 *          @ref SWMM_Infil2DRow::p is in **PROJECT UNITS** — the same number a
 *          user types into a legacy `[INFILTRATION]` row (in/hr and in on a
 *          US-`FLOW_UNITS` project, mm/hr and mm on an SI project). This
 *          deliberately differs from the SI convention of the rest of the 2D
 *          API, so that `[INFILTRATION]` and `[2D_INFILTRATION*]` cannot
 *          disagree. The *readback* channels are the exception and are SI, as
 *          the rest of the 2D API is: @ref swmm_infil2d_get_rate_bulk is m/s,
 *          @ref swmm_infil2d_get_cum_bulk is m, and
 *          @ref swmm_infil2d_get_total_volume is m³.
 *
 *          **Lifecycle.** The readers work in any state (see each function).
 *          Every setter that changes parameters is rejected with
 *          `SWMM_ERR_LIFECYCLE` unless the engine is in `SWMM_STATE_OPENED`
 *          (or `BUILDING`) — see the "Staleness" note on
 *          @ref swmm_infil2d_set_options.
 *
 *          All functions return `SWMM_ERR_BADPARAM` when the 2D module carries
 *          no mesh, and require the engine to have been compiled with
 *          `OPENSWMM_BUILD_2D`.
 *
 * @defgroup engine_infil2d 2D Infiltration API
 * @ingroup  engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_INFIL2D_H
#define OPENSWMM_INFIL2D_H

#include "openswmm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Codes and limits
 * ========================================================================= */

/** @brief Number of positional parameter columns carried per row.
 *
 *  Matches the widest legacy `[INFILTRATION]` method (Horton:
 *  `f0 fmin decay dry_time Fmax`). Mirrors
 *  `openswmm::twoD::kInfil2DMaxParams`.
 *  @ingroup engine_infil2d */
#define SWMM_INFIL2D_MAX_PARAMS 5

/** Infiltration method codes. Mirror `openswmm::InfilModel` value-for-value
 *  (D-I6: six methods). `CONSTANT` has no legacy `[INFILTRATION]` token — it
 *  is 2D-only. */
#define SWMM_INFIL2D_HORTON         0  /**< Horton. */
#define SWMM_INFIL2D_MOD_HORTON     1  /**< Modified Horton (linear decay). */
#define SWMM_INFIL2D_GREEN_AMPT     2  /**< Green-Ampt. */
#define SWMM_INFIL2D_MOD_GREEN_AMPT 3  /**< Modified Green-Ampt (F not reset). */
#define SWMM_INFIL2D_CURVE_NUMBER   4  /**< SCS curve number. */
#define SWMM_INFIL2D_CONSTANT       5  /**< Constant rate, capacity-bounded. */

/** Destination codes for infiltrated water. Mirror
 *  `openswmm::twoD::Infil2DDest`.
 *
 *  D-I4: `LOST` is the only destination this release routes. The other two
 *  exist so the grammar is stable and are rejected at validation with a
 *  "not supported in this release" message. */
#define SWMM_INFIL2D_DEST_LOST             0  /**< Leaves the domain; booked to the `infil_out` ledger row. */
#define SWMM_INFIL2D_DEST_SUBCATCH_AQUIFER 1  /**< Reserved — legacy subcatchment aquifer. */
#define SWMM_INFIL2D_DEST_AQUIFER_2D       2  /**< Reserved — the two-zone 2D kernel. */

/* =========================================================================
 * Value types
 * ========================================================================= */

/** @brief `[2D_INFILTRATION_OPTIONS]` — POD mirror of
 *         `openswmm::twoD::Infil2DOptions`.
 *  @ingroup engine_infil2d */
typedef struct SWMM_Infil2DOptions {
    /** Evaluation cadence in SECONDS (D-I1: infiltration is a held rate,
     *  recomputed on this cadence and held constant between updates).
     *  `<= 0` means "use the project `WET_STEP`", which the 2D surface
     *  resolves when it initializes. */
    double infil_step;
} SWMM_Infil2DOptions;

/**
 * @brief One infiltration specification — POD mirror of
 *        `openswmm::twoD::Infil2DRow`.
 *
 * @details Parameters are POSITIONAL and in **PROJECT UNITS**, matching legacy
 *          `[INFILTRATION]` exactly:
 *
 *          | method                        | p[0]      | p[1] | p[2]         | p[3]        | p[4] |
 *          |-------------------------------|-----------|------|--------------|-------------|------|
 *          | `SWMM_INFIL2D_HORTON`         | f0        | fmin | decay (1/hr) | dry_time (d)| Fmax |
 *          | `SWMM_INFIL2D_MOD_HORTON`     | f0        | fmin | decay (1/hr) | dry_time (d)| Fmax |
 *          | `SWMM_INFIL2D_GREEN_AMPT`     | S suction | Ks   | IMD          | —           | —    |
 *          | `SWMM_INFIL2D_MOD_GREEN_AMPT` | S suction | Ks   | IMD          | —           | —    |
 *          | `SWMM_INFIL2D_CURVE_NUMBER`   | CN        | —    | dry_time (d) | —           | —    |
 *          | `SWMM_INFIL2D_CONSTANT`       | rate      | —    | —            | —           | —    |
 *
 *          `f0` / `fmin` / `Ks` / `rate` are in/hr (US) or mm/hr (SI); `S` and
 *          `Fmax` are in (US) or mm (SI). `CURVE_NUMBER`'s `p[1]` is unused
 *          and ignored, matching the legacy column layout where the middle
 *          value is a no-op. Slots a method does not use are ignored; write 0.
 *
 * @ingroup engine_infil2d
 */
typedef struct SWMM_Infil2DRow {
    /** 0 = `NONE` (the cell/tag has no infiltration model). When 0, every
     *  other field is meaningless and is ignored on write. */
    int    has_method;
    /** One of the `SWMM_INFIL2D_*` method codes. */
    int    method;
    /** Positional parameters in PROJECT UNITS — see the table above. */
    double p[SWMM_INFIL2D_MAX_PARAMS];
    /** One of the `SWMM_INFIL2D_DEST_*` codes. Only
     *  `SWMM_INFIL2D_DEST_LOST` is accepted in this release (D-I4). */
    int    dest;
} SWMM_Infil2DRow;

/* =========================================================================
 * Options — `[2D_INFILTRATION_OPTIONS]`
 * ========================================================================= */

/** @brief Get the 2D infiltration options (`INFIL_STEP`).
 *
 *  Valid in any state once a 2D mesh is present. Reports the AUTHORED value,
 *  which may still be `<= 0` ("use `WET_STEP`") — the cadence the solver
 *  actually resolved is not exposed here.
 *
 *  @param engine  Engine handle.
 *  @param options Output; must not be NULL.
 *  @returns `SWMM_OK`; `SWMM_ERR_BADHANDLE` on a bad handle;
 *           `SWMM_ERR_BADPARAM` on a NULL argument or when no 2D mesh exists.
 *  @ingroup engine_infil2d */
SWMM_ENGINE_API int swmm_infil2d_get_options(SWMM_Engine engine,
                                             SWMM_Infil2DOptions* options);

/** @brief Set the 2D infiltration options (`INFIL_STEP`, seconds).
 *
 *  Persisted in `[2D_INFILTRATION_OPTIONS]` on save.
 *
 *  **Staleness (applies to every setter in this header).** Infiltration
 *  parameters are baked into per-cell Horton / Green-Ampt / curve-number
 *  kernel state ONCE, when the 2D surface initializes. The only re-entry
 *  point rebuilds that state for the WHOLE mesh and zeroes the per-cell
 *  cumulative-depth array, which would discard the integration history of
 *  every cell the caller never touched and desynchronize the `infil_out`
 *  ledger row. Rather than corrupt a run, parameter setters are **rejected
 *  after initialize()**: they return `SWMM_ERR_LIFECYCLE` unless the engine is
 *  in `SWMM_STATE_OPENED` (the state the GUI edits in) or the programmatic
 *  `BUILDING` state. Re-open or re-initialize to apply a change.
 *
 *  @param engine  Engine handle.
 *  @param options New options; must not be NULL. `infil_step <= 0` selects
 *                 the project `WET_STEP`; non-finite values are rejected.
 *  @returns `SWMM_OK`; `SWMM_ERR_BADHANDLE`; `SWMM_ERR_BADPARAM` on a NULL /
 *           non-finite argument or when no 2D mesh exists;
 *           `SWMM_ERR_LIFECYCLE` when called after initialize().
 *  @ingroup engine_infil2d */
SWMM_ENGINE_API int swmm_infil2d_set_options(SWMM_Engine engine,
                                             const SWMM_Infil2DOptions* options);

/* =========================================================================
 * Tag defaults — `[2D_INFILTRATION_DEFAULTS]`
 * ========================================================================= */

/** @brief Number of authored tag-default rows, including the `'*'` row.
 *
 *  The `*_count` companion for @ref swmm_infil2d_get_default /
 *  @ref swmm_infil2d_get_default_tag.
 *
 *  @param engine Engine handle.
 *  @param count  Output row count (0 when the model authors no defaults).
 *  @returns `SWMM_OK`; `SWMM_ERR_BADHANDLE`; `SWMM_ERR_BADPARAM` on NULL
 *           @p count or when no 2D mesh exists.
 *  @ingroup engine_infil2d */
SWMM_ENGINE_API int swmm_infil2d_defaults_count(SWMM_Engine engine, int* count);

/** @brief Read one tag-default row by index.
 *
 *  Row parameters are in **PROJECT UNITS** — see @ref SWMM_Infil2DRow.
 *  Pair with @ref swmm_infil2d_get_default_tag to learn which tag the row
 *  belongs to.
 *
 *  @param engine Engine handle.
 *  @param idx    Row index in `[0, swmm_infil2d_defaults_count)`.
 *  @param row    Output row; must not be NULL.
 *  @returns `SWMM_OK`; `SWMM_ERR_BADINDEX` on an out-of-range index;
 *           `SWMM_ERR_BADPARAM` on NULL @p row or when no 2D mesh exists.
 *  @ingroup engine_infil2d */
SWMM_ENGINE_API int swmm_infil2d_get_default(SWMM_Engine engine, int idx,
                                             SWMM_Infil2DRow* row);

/** @brief Read the TAG of one default row by index.
 *
 *  Copies up to `buflen-1` bytes into @p buf and always NUL-terminates
 *  (the `swmm_2d_get_triangle_tag` convention). `"*"` identifies the
 *  mesh-wide fallback row.
 *
 *  @param engine Engine handle.
 *  @param idx    Row index in `[0, swmm_infil2d_defaults_count)`.
 *  @param buf    Caller-allocated output buffer.
 *  @param buflen Capacity of @p buf in bytes; must be > 0.
 *  @returns `SWMM_OK`; `SWMM_ERR_BADINDEX`; `SWMM_ERR_BADPARAM`.
 *  @ingroup engine_infil2d */
SWMM_ENGINE_API int swmm_infil2d_get_default_tag(SWMM_Engine engine, int idx,
                                                 char* buf, int buflen);

/** @brief Add or replace the default row for a tag.
 *
 *  Upsert semantics: any existing rows carrying @p tag are removed and one
 *  row is appended, so a tag never has two definitions. Pass `"*"` to set the
 *  mesh-wide fallback. A row with `has_method == 0` (`NONE`) is meaningful and
 *  is stored — for a tag it deliberately CLEARS the `'*'` default for that
 *  tag's cells.
 *
 *  Row parameters are in **PROJECT UNITS** — see @ref SWMM_Infil2DRow.
 *  Persisted in `[2D_INFILTRATION_DEFAULTS]` on save. Subject to the
 *  staleness rule documented on @ref swmm_infil2d_set_options.
 *
 *  @param engine Engine handle.
 *  @param tag    Tag to define; must be non-empty.
 *  @param row    Row to store; must not be NULL.
 *  @returns `SWMM_OK`; `SWMM_ERR_BADHANDLE`; `SWMM_ERR_BADPARAM` on a NULL /
 *           empty tag, a NULL row, an unknown method code, a destination
 *           other than `SWMM_INFIL2D_DEST_LOST`, an out-of-range parameter,
 *           or when no 2D mesh exists; `SWMM_ERR_LIFECYCLE` after
 *           initialize().
 *  @ingroup engine_infil2d */
SWMM_ENGINE_API int swmm_infil2d_set_default(SWMM_Engine engine,
                                             const char* tag,
                                             const SWMM_Infil2DRow* row);

/** @brief Remove every default row carrying a tag.
 *
 *  Removing the `'*'` row leaves cells with no tag row resolving to no
 *  infiltration. Subject to the staleness rule documented on
 *  @ref swmm_infil2d_set_options.
 *
 *  @param engine Engine handle.
 *  @param tag    Tag to remove; must be non-empty.
 *  @returns `SWMM_OK` (also when the tag was not present);
 *           `SWMM_ERR_BADHANDLE`; `SWMM_ERR_BADPARAM` on a NULL / empty tag
 *           or when no 2D mesh exists; `SWMM_ERR_LIFECYCLE` after
 *           initialize().
 *  @ingroup engine_infil2d */
SWMM_ENGINE_API int swmm_infil2d_remove_default(SWMM_Engine engine,
                                                const char* tag);

/* =========================================================================
 * Per-cell overrides — `[2D_INFILTRATION]`
 * ========================================================================= */

/** @brief Read the infiltration specification in force at one triangle.
 *
 *  Row parameters are in **PROJECT UNITS** — see @ref SWMM_Infil2DRow.
 *
 *  Behaviour depends on whether the 2D surface has resolved yet:
 *
 *  - **After initialize()** the RESOLVED row is reported (D-I3 precedence
 *    already applied) and `*is_override` is 1 exactly when the resolved
 *    provenance is the per-cell `[2D_INFILTRATION]` layer.
 *  - **Before initialize()** only the per-cell override layer is visible, so
 *    the authored override for @p tri is reported with `*is_override == 1`,
 *    and a cell with no override reports `has_method == 0` with
 *    `*is_override == 0` even when a tag or `'*'` default would later apply.
 *    Combine with @ref swmm_infil2d_get_default and the triangle's tag
 *    (`swmm_2d_get_triangle_tag`) to preview the resolution in that state.
 *
 *  @param engine      Engine handle.
 *  @param tri         Triangle index (0-based) in `[0, triangle_count)`.
 *  @param row         Output row; must not be NULL.
 *  @param is_override Output flag, 1 / 0. May be NULL.
 *  @returns `SWMM_OK`; `SWMM_ERR_BADINDEX` on an out-of-range triangle;
 *           `SWMM_ERR_BADPARAM` on NULL @p row or when no 2D mesh exists.
 *  @ingroup engine_infil2d */
SWMM_ENGINE_API int swmm_infil2d_get_cell(SWMM_Engine engine, int tri,
                                          SWMM_Infil2DRow* row,
                                          int* is_override);

/** @brief Set (or clear) the per-cell `[2D_INFILTRATION]` override of one
 *         triangle.
 *
 *  Row parameters are in **PROJECT UNITS** — see @ref SWMM_Infil2DRow.
 *  Passing `row == NULL` CLEARS the override, so the cell falls back to its
 *  tag row / the `'*'` row / no infiltration. Passing a row with
 *  `has_method == 0` is different: it stores an explicit `NONE` override,
 *  which suppresses the tag and `'*'` defaults for that cell.
 *
 *  Persisted as one `[2D_INFILTRATION]` row on save. Subject to the staleness
 *  rule documented on @ref swmm_infil2d_set_options.
 *
 *  @param engine Engine handle.
 *  @param tri    Triangle index (0-based) in `[0, triangle_count)`.
 *  @param row    Row to store, or NULL to clear the override.
 *  @returns `SWMM_OK`; `SWMM_ERR_BADINDEX` on an out-of-range triangle;
 *           `SWMM_ERR_BADPARAM` on an invalid row or when no 2D mesh exists;
 *           `SWMM_ERR_LIFECYCLE` after initialize().
 *  @ingroup engine_infil2d */
SWMM_ENGINE_API int swmm_infil2d_set_cell(SWMM_Engine engine, int tri,
                                          const SWMM_Infil2DRow* row);

/** @brief Assign one specification to many triangles in a single call.
 *
 *  The GUI's select-many-cells-then-assign entry point: one validation pass,
 *  then one apply. **All-or-nothing** — the row and EVERY entry of @p tris are
 *  validated first, and if any triangle index is out of range (or the row is
 *  invalid) nothing at all is written. Duplicate indices are tolerated; the
 *  cell simply ends up with the one row.
 *
 *  Row parameters are in **PROJECT UNITS** — see @ref SWMM_Infil2DRow.
 *  Passing `row == NULL` clears the override on every listed triangle, with
 *  the same all-or-nothing contract. Subject to the staleness rule documented
 *  on @ref swmm_infil2d_set_options.
 *
 *  @param engine Engine handle.
 *  @param tris   Caller-owned array of `n` 0-based triangle indices.
 *  @param n      Number of entries in @p tris; must be > 0.
 *  @param row    Row to store on every listed triangle, or NULL to clear.
 *  @returns `SWMM_OK`; `SWMM_ERR_BADINDEX` when any entry of @p tris is out
 *           of range (nothing applied); `SWMM_ERR_BADPARAM` on a NULL @p tris,
 *           `n <= 0`, an invalid row, or when no 2D mesh exists;
 *           `SWMM_ERR_LIFECYCLE` after initialize().
 *  @ingroup engine_infil2d */
SWMM_ENGINE_API int swmm_infil2d_set_cells(SWMM_Engine engine,
                                           const int* tris, int n,
                                           const SWMM_Infil2DRow* row);

/* =========================================================================
 * State readback (SI, like the rest of the 2D API)
 * ========================================================================= */

/** @brief Bulk get the held per-cell infiltration rate (m/s, >= 0).
 *
 *  This is the rate the marcher is consuming — recomputed on the `INFIL_STEP`
 *  cadence and held constant between updates (D-I1), NOT re-evaluated per
 *  sub-step. Values are SI regardless of the project unit system.
 *
 *  Fills `min(n, triangle_count)` entries. A mesh with no resolved
 *  infiltration model is a legitimate configuration, not an error: the buffer
 *  is zero-filled and `SWMM_OK` is returned.
 *
 *  @param engine Engine handle.
 *  @param f      Caller-allocated output buffer of at least @p n doubles.
 *  @param n      Capacity of @p f; must be > 0.
 *  @returns `SWMM_OK`; `SWMM_ERR_BADHANDLE`; `SWMM_ERR_BADPARAM` on a NULL
 *           buffer, `n <= 0`, or when no 2D mesh exists.
 *  @ingroup engine_infil2d */
SWMM_ENGINE_API int swmm_infil2d_get_rate_bulk(SWMM_Engine engine,
                                               double* f, int n);

/** @brief Bulk get the cumulative infiltrated depth per cell (m).
 *
 *  The `infil_cum` sidecar variable: the running time-integral of the loss the
 *  solver actually applied (the held rate after the wet/dry depth ramp), so
 *  `sum(F[i] * cell_area[i])` equals @ref swmm_infil2d_get_total_volume.
 *  Values are SI regardless of the project unit system.
 *
 *  Fills `min(n, triangle_count)` entries, and zero-fills + returns `SWMM_OK`
 *  when no infiltration model is resolved (same contract as
 *  @ref swmm_infil2d_get_rate_bulk).
 *
 *  @param engine Engine handle.
 *  @param F      Caller-allocated output buffer of at least @p n doubles.
 *  @param n      Capacity of @p F; must be > 0.
 *  @returns `SWMM_OK`; `SWMM_ERR_BADHANDLE`; `SWMM_ERR_BADPARAM` on a NULL
 *           buffer, `n <= 0`, or when no 2D mesh exists.
 *  @ingroup engine_infil2d */
SWMM_ENGINE_API int swmm_infil2d_get_cum_bulk(SWMM_Engine engine,
                                              double* F, int n);

/** @brief Get the cumulative 2D infiltration loss (m³) — the `infil_out`
 *         mass-balance ledger row.
 *
 *  The whole-domain companion to @ref swmm_infil2d_get_cum_bulk, reported
 *  beside `evap_out` in the continuity report. Reads the same ledger as
 *  @ref swmm_2d_get_mass_balance and, like it, requires the 2D mass balance
 *  to be live.
 *
 *  @param engine Engine handle.
 *  @param volume Output volume in m³; must not be NULL.
 *  @returns `SWMM_OK`; `SWMM_ERR_BADHANDLE`; `SWMM_ERR_BADPARAM` on NULL
 *           @p volume or when the 2D mass balance is not active.
 *  @ingroup engine_infil2d */
SWMM_ENGINE_API int swmm_infil2d_get_total_volume(SWMM_Engine engine,
                                                  double* volume);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_INFIL2D_H */
