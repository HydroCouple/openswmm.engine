// SPDX-License-Identifier: Apache-2.0

/**
 * @file      openswmm_snow2d.h
 * @brief     Public C API for mesh snow pack assignments and SI-unit overrides.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_SNOW2D_H
#define OPENSWMM_SNOW2D_H
#include "openswmm_snow.h"
#ifdef __cplusplus
extern "C" {
#endif
// Shared named packs; all overrides are SI, slope/aspect in degrees.
// Priority: whole mesh (*), tag, cell. NONE explicitly disables a cell/tag.
// Cell keys in section text are 1-based, as in the other 2D input sections.
/**
 * @brief Read mesh snow enable mode and authored cadence.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[out] mode Non-null slot: 0 AUTO, 1 YES, 2 NO.
 * @param[out] step_s Non-null authored seconds; zero inherits the shared cadence.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note Returns configuration even before the mesh snow runtime is active.
 */
SWMM_ENGINE_API int swmm_snow2d_get_options(SWMM_Engine engine, int* mode,
                                            double* step_s); // 0 AUTO, 1 YES, 2 NO; 0 step inherits
/**
 * @brief Count authored mesh-snow default/tag rows.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @return Nonnegative row count; a null engine returns zero rather than an
 * error status. Does not require an initialized surface runtime.
 */
SWMM_ENGINE_API int swmm_snow2d_defaults_count(SWMM_Engine engine);
/**
 * @brief Count authored mesh-snow explicit cell rows.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @return Nonnegative row count; a null engine returns zero rather than an
 * error status. Does not require an initialized surface runtime.
 */
SWMM_ENGINE_API int swmm_snow2d_cells_count(SWMM_Engine engine);
// Section 0 options, 1 defaults, 2 cells. Required capacity includes the
// terminal NUL. Query with buffer=NULL and capacity=0 to obtain that size.
/**
 * @brief Serialize one mesh-snow section body and report its required capacity.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] section 0 options, 1 defaults/tags, 2 explicit cells.
 * @param[out] buffer Writable text buffer, or NULL with capacity == 0 for a size query.
 * @param[in] capacity Nonnegative byte capacity; must include the terminal NUL for copying.
 * @param[out] required Non-null slot for exact required bytes including NUL,
 * set before a short-buffer failure when section serialization succeeds.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, SWMM_ERR_BUFSIZE when a
 * buffer is shorter than *required, or SWMM_ERR_BADPARAM for invalid arguments. Output storage is
 * caller-owned.
 * @note Returns newline-terminated body rows without section headers; no
 * partial successful truncation is allowed.
 */
SWMM_ENGINE_API int swmm_snow2d_get_configuration(SWMM_Engine engine, int section, char* buffer,
                                                  int capacity, int* required);
typedef struct SWMM_MeshSnowOverrides {
    // NaN means inherited. Slope/aspect are either both set or both inherited.
    double forest, drift, ground_heat_wm2, extinction_depth_m, slope_deg, aspect_deg;
} SWMM_MeshSnowOverrides;
// Section 1 defaults (cell=-1, tag=* or region); section 2 cells (zero-based
// cell, empty tag). Reads do not resolve/invent inherited parameter values.
/**
 * @brief Read an authored mesh-snow assignment without resolving inherited overrides.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] section 1 for default/tag rows, 2 for explicit cell rows.
 * @param[in] row Zero-based position in that section's current map iteration order.
 * @param[out] cell Non-null slot; -1 for defaults or the zero-based cell for explicit rows.
 * @param[out] tag Non-null writable tag buffer; empty for explicit cell rows.
 * @param[in] tag_capacity Positive capacity including the terminator; short buffers fail.
 * @param[out] pack Non-null writable pack-name buffer; NONE explicitly disables snow.
 * @param[in] pack_capacity Positive pack-name capacity including the terminator.
 * @param[out] values Non-null override structure, with NaN for inherited fields.
 * SI values use W/m2 and m; slope/aspect are degrees and fraction/drift dimensionless.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, SWMM_ERR_BADINDEX for a
 * row outside the section, SWMM_ERR_BUFSIZE when the tag or pack buffer is too short (nothing is
 * written), or SWMM_ERR_BADPARAM for invalid arguments, including a section other than 1 or 2.
 * Output storage is caller-owned.
 * The setter/parser maintains paired slope/aspect inheritance.
 */
SWMM_ENGINE_API int swmm_snow2d_get_row(SWMM_Engine engine, int section, int row, int* cell,
                                        char* tag, int tag_capacity, char* pack, int pack_capacity,
                                        SWMM_MeshSnowOverrides* values);
typedef struct SWMM_MeshSnowLedger {
    double completed_s, initial_m3, raw_precipitation_m3, precipitation_m3, liquid_m3;
    double sublimation_m3, stored_m3, residual_m3, net_energy_kj, initial_energy_kj,
        stored_energy_kj, energy_residual_kj;
} SWMM_MeshSnowLedger;
// Bulk snapshots in mesh cell order. Unassigned cells have model DEFAULT
// and NaN quantities, distinguishing unavailable results from a bare pack.
/**
 * @brief Snapshot a contiguous range of accepted mesh-snow states.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] first First zero-based cell index, at most the mesh cell count.
 * @param[in] count Nonnegative range length; first + count must fit the mesh.
 * @param[out] output Borrowed writable array of count SWMM_SnowState entries; may be
 * NULL for count == 0. Values use m SWE, m/s water rates, degrees C temperatures, kJ/m2 energy
 * and dimensionless fractions/age.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE, or SWMM_ERR_BADPARAM for invalid ranges,
 * buffers, inactive surface routing, or builds without 2D support.
 * @note Unassigned cells contain NaN quantities and model SWMM_SNOW_DEFAULT; UEB-only
 * energy fields also remain NaN for non-UEB cells. No storage is retained.
 */
SWMM_ENGINE_API int swmm_snow2d_get_states(SWMM_Engine engine, int first, int count,
                                           SWMM_SnowState* output);
/**
 * @brief Snapshot a contiguous range of accepted mesh-snow ledgers.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] first First zero-based cell index, at most the mesh cell count.
 * @param[in] count Nonnegative range length; first + count must fit the mesh.
 * @param[out] output Borrowed writable array of count SWMM_MeshSnowLedger entries; may be
 * NULL for count == 0. Values use seconds, m3 water inventories and kJ energy inventories.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE, or SWMM_ERR_BADPARAM for invalid ranges,
 * buffers, inactive surface routing, or builds without 2D support.
 * @note Unassigned cells contain NaN quantities; UEB-only
 * energy fields also remain NaN for non-UEB cells. No storage is retained.
 */
SWMM_ENGINE_API int swmm_snow2d_get_ledgers(SWMM_Engine engine, int first, int count,
                                            SWMM_MeshSnowLedger* output);
// UEB cover/depth remapping energy [kJ]. Non-UEB and unassigned cells return NaN.
/**
 * @brief Read accumulated UEB geometry-remapping energy for a cell range.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] first First zero-based mesh cell, at most cell count.
 * @param[in] count Nonnegative number of cells; the full range must fit the mesh.
 * @param[out] energy_kj Borrowed count-element destination in kJ; NULL allowed for zero count.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE, or SWMM_ERR_BADPARAM for an inactive/unbuilt
 * 2D module, invalid range, or missing nonempty output buffer.
 * @note Unassigned and non-UEB cells return NaN. No runtime state is advanced.
 */
SWMM_ENGINE_API int swmm_snow2d_get_geometry_energy(SWMM_Engine engine, int first, int count,
                                                    double* energy_kj);
// Atomic replacement through the same strict parsers as the INP reader.
// Empty section bodies clear that section. No section headers are supplied.
/**
 * @brief Validate and atomically replace all mesh-snow authored section bodies.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] options Non-null options text without a section header.
 * @param[in] defaults Non-null whole-mesh/tag rows; precedence is cell > tag > whole mesh.
 * @param[in] cells Non-null explicit-cell rows; section text uses one-based cell keys.
 * Empty section strings clear their section. NONE explicitly disables a scope.
 * @param[out] diagnostic Non-null writable diagnostic text; empty on success.
 * @param[in] capacity Positive byte capacity; failure text is safely truncated if necessary.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note Validates all parsers and named-pack references on a copy before
 * publication. Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 */
SWMM_ENGINE_API int swmm_snow2d_apply_configuration(SWMM_Engine engine, const char* options,
                                                    const char* defaults, const char* cells,
                                                    char* diagnostic, int capacity);
#ifdef __cplusplus
}
#endif
#endif
