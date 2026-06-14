/**
 * @file openswmm_infrastructure.h
 * @brief OpenSWMM Engine — Infrastructure (Transects, Streets, Inlets, LIDs) C API.
 *
 * @details Transect creation and station data, street parameters, inlet
 *          definitions, LID control layers, and LID usage assignment.
 *
 * @ingroup engine_api
 * @see openswmm_engine.h
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_INFRASTRUCTURE_H
#define OPENSWMM_INFRASTRUCTURE_H

#include "openswmm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Transects
 * ========================================================================= */

/**
 * @brief Add a new transect for irregular cross-sections.
 * @param engine  Engine handle (SWMM_STATE_BUILDING).
 * @param id      Unique null-terminated identifier.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_transect_add(SWMM_Engine engine, const char* id);

/**
 * @brief Set Manning's roughness for left overbank, right overbank, and channel.
 * @param engine     Engine handle.
 * @param idx        Zero-based transect index.
 * @param n_left     Manning's n for the left overbank.
 * @param n_right    Manning's n for the right overbank.
 * @param n_channel  Manning's n for the main channel.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_transect_set_roughness(SWMM_Engine engine, int idx, double n_left, double n_right, double n_channel);

/**
 * @brief Add a station–elevation data point to a transect.
 *
 * @details Stations must be added in order from left to right across the
 *          cross-section.
 *
 * @param engine     Engine handle.
 * @param idx        Zero-based transect index.
 * @param station    Horizontal distance (station) from a reference.
 * @param elevation  Elevation at this station.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_transect_add_station(SWMM_Engine engine, int idx, double station, double elevation);

/**
 * @brief Get the total number of transects in the model.
 * @param engine  Engine handle.
 * @returns Number of transects, or -1 on error.
 */
SWMM_ENGINE_API int swmm_transect_count(SWMM_Engine engine);

/**
 * @brief Look up a transect's zero-based index by its string identifier.
 * @param engine  Engine handle.
 * @param id      Null-terminated transect identifier.
 * @returns Zero-based index, or -1 if not found.
 */
SWMM_ENGINE_API int swmm_transect_index(SWMM_Engine engine, const char* id);

/**
 * @brief Get the string identifier of a transect by index.
 * @param engine  Engine handle.
 * @param idx     Zero-based transect index.
 * @returns Null-terminated string owned by the engine, or NULL on error.
 */
SWMM_ENGINE_API const char* swmm_transect_id(SWMM_Engine engine, int idx);

/* -------------------------------------------------------------------------
 * Per-field getters / setters (DA-ENG-09 + BQ-TR-02)
 *
 * GUI editors (Slice BQ Phase 6.7.4 TransectEditor) need round-trip access
 * to every transect field; the legacy 3-function surface (add / set_roughness
 * / add_station) only covers a fraction. The functions below close that gap.
 * ------------------------------------------------------------------------- */

/**
 * @brief Get the Manning's roughness values for a transect.
 * @param engine     Engine handle.
 * @param idx        Zero-based transect index.
 * @param n_left     [out] Manning's n for the left overbank. May be NULL.
 * @param n_right    [out] Manning's n for the right overbank. May be NULL.
 * @param n_channel  [out] Manning's n for the main channel. May be NULL.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_transect_get_roughness(SWMM_Engine engine, int idx,
                                                 double* n_left, double* n_right, double* n_channel);

/**
 * @brief Set the left and right bank stations for a transect.
 *
 * @details Bank stations delimit the main channel from the overbanks; they
 *          are independent of the encroachment stations (BQ-TR-02).
 *
 * @param engine   Engine handle.
 * @param idx      Zero-based transect index.
 * @param x_left   Station of the left bank.
 * @param x_right  Station of the right bank.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_transect_set_bank_stations(SWMM_Engine engine, int idx,
                                                     double x_left, double x_right);

/**
 * @brief Get the left and right bank stations for a transect.
 * @param engine   Engine handle.
 * @param idx      Zero-based transect index.
 * @param x_left   [out] Station of the left bank. May be NULL.
 * @param x_right  [out] Station of the right bank. May be NULL.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_transect_get_bank_stations(SWMM_Engine engine, int idx,
                                                     double* x_left, double* x_right);

/**
 * @brief Set the left and right encroachment stations for a transect (BQ-TR-02).
 *
 * @details Encroachment stations are distinct from bank stations and identify
 *          floodplain encroachment limits (HEC-RAS convention). On legacy
 *          `[TRANSECTS]` X1 records that omit the trailing encroachment
 *          columns, the INP parser may default these to the bank stations
 *          to preserve backward compatibility; callers writing programmatic
 *          values via this API are setting them explicitly.
 *
 * @param engine   Engine handle.
 * @param idx      Zero-based transect index.
 * @param x_left   Station of the left encroachment limit.
 * @param x_right  Station of the right encroachment limit.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_transect_set_encroachment_stations(SWMM_Engine engine, int idx,
                                                             double x_left, double x_right);

/**
 * @brief Get the left and right encroachment stations for a transect (BQ-TR-02).
 * @param engine   Engine handle.
 * @param idx      Zero-based transect index.
 * @param x_left   [out] Station of the left encroachment limit. May be NULL.
 * @param x_right  [out] Station of the right encroachment limit. May be NULL.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_transect_get_encroachment_stations(SWMM_Engine engine, int idx,
                                                             double* x_left, double* x_right);

/**
 * @brief Set the station, elevation, and meander modifiers for a transect.
 *
 * @details Maps to the `xFactor`, `yFactor`, and `lengthFactor` parameters
 *          on the `[TRANSECTS]` X1 record:
 *          - `x_factor`      = station-spacing multiplier (default 1.0).
 *          - `y_factor`      = elevation offset added to every station
 *                              elevation (default 0.0 — engine stores 1.0
 *                              as a no-op marker at `swmm_transect_add` time).
 *          - `length_factor` = meander factor = channel / floodplain length
 *                              ratio (default 1.0).
 *
 * @param engine         Engine handle.
 * @param idx            Zero-based transect index.
 * @param x_factor       Station spacing multiplier.
 * @param y_factor       Elevation offset.
 * @param length_factor  Meander factor (channel/floodplain length ratio).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_transect_set_modifiers(SWMM_Engine engine, int idx,
                                                 double x_factor, double y_factor, double length_factor);

/**
 * @brief Get the station, elevation, and meander modifiers for a transect.
 * @param engine         Engine handle.
 * @param idx            Zero-based transect index.
 * @param x_factor       [out] Station spacing multiplier. May be NULL.
 * @param y_factor       [out] Elevation offset. May be NULL.
 * @param length_factor  [out] Meander factor. May be NULL.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_transect_get_modifiers(SWMM_Engine engine, int idx,
                                                 double* x_factor, double* y_factor, double* length_factor);

/**
 * @brief Set the free-form comments / description for a transect.
 * @param engine  Engine handle.
 * @param idx     Zero-based transect index.
 * @param text    Null-terminated comment string. NULL clears the comment.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_transect_set_comments(SWMM_Engine engine, int idx, const char* text);

/**
 * @brief Get the free-form comments / description for a transect.
 *
 * @details Writes the comment into @p buf with NUL termination; truncates
 *          if the buffer is smaller than the comment. Always returns SWMM_OK
 *          (an empty comment results in @p buf[0] == '\0').
 *
 * @param engine  Engine handle.
 * @param idx     Zero-based transect index.
 * @param buf     Destination buffer.
 * @param buflen  Size of @p buf in bytes (must be > 0).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_transect_get_comments(SWMM_Engine engine, int idx, char* buf, int buflen);

/**
 * @brief Get the number of station–elevation points stored for a transect.
 * @param engine  Engine handle.
 * @param idx     Zero-based transect index.
 * @returns Station count, or -1 on error.
 */
SWMM_ENGINE_API int swmm_transect_get_station_count(SWMM_Engine engine, int idx);

/**
 * @brief Get a single station–elevation pair from a transect.
 * @param engine       Engine handle.
 * @param idx          Zero-based transect index.
 * @param station_idx  Zero-based station-pair index.
 * @param station      [out] Horizontal distance. May be NULL.
 * @param elevation    [out] Elevation at this station. May be NULL.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_transect_get_station(SWMM_Engine engine, int idx, int station_idx,
                                               double* station, double* elevation);

/**
 * @brief Remove all station–elevation pairs from a transect.
 *
 * @details Used by the GUI's snapshot-and-rewrite path: clear then re-add
 *          the full station list in one shot.
 *
 * @param engine  Engine handle.
 * @param idx     Zero-based transect index.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_transect_clear_stations(SWMM_Engine engine, int idx);

/**
 * @brief Rename an existing transect.
 *
 * @details Refuses on collision with another existing transect name (case
 *          insensitive); same-name is a no-op SWMM_OK.
 *
 * @param engine   Engine handle.
 * @param idx      Zero-based transect index.
 * @param new_id   New null-terminated identifier.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_transect_rename(SWMM_Engine engine, int idx, const char* new_id);

/**
 * @brief Remove a transect by index.
 *
 * @details Out-of-range indices are a SWMM_OK no-op (mirrors the pattern
 *          mutation API — see test_pattern_mutation_api.cpp). Remaining
 *          transects preserve their relative order; their indices shift
 *          down by one.
 *
 * @param engine  Engine handle.
 * @param idx     Zero-based transect index.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_transect_remove(SWMM_Engine engine, int idx);

/* =========================================================================
 * Streets
 * ========================================================================= */

/**
 * @brief Add a new street cross-section definition.
 * @param engine  Engine handle (SWMM_STATE_BUILDING).
 * @param id      Unique null-terminated identifier.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_street_add(SWMM_Engine engine, const char* id);

/**
 * @brief Set the geometric parameters for a street cross-section.
 *
 * @param engine         Engine handle.
 * @param idx            Zero-based street index.
 * @param t_crown        Crown thickness (rise of the crown above the gutter).
 * @param h_curb         Curb height.
 * @param sx             Cross slope of the roadway.
 * @param n_road         Manning's n for the road surface.
 * @param gutter_depres  Gutter depression depth.
 * @param gutter_width   Gutter width.
 * @param sides          Number of sides (1 or 2).
 * @param back_width     Backing (sidewalk) width.
 * @param back_slope     Backing slope.
 * @param back_n         Manning's n for the backing area.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_street_set_params(SWMM_Engine engine, int idx,
                                             double t_crown, double h_curb, double sx, double n_road,
                                             double gutter_depres, double gutter_width, int sides,
                                             double back_width, double back_slope, double back_n);

/**
 * @brief Get the total number of street definitions in the model.
 * @param engine  Engine handle.
 * @returns Number of streets, or -1 on error.
 */
SWMM_ENGINE_API int swmm_street_count(SWMM_Engine engine);

/**
 * @brief Look up a street's zero-based index by its string identifier.
 * @param engine  Engine handle.
 * @param id      Null-terminated street identifier.
 * @returns Zero-based index, or -1 if not found.
 */
SWMM_ENGINE_API int swmm_street_index(SWMM_Engine engine, const char* id);

/**
 * @brief Get the string identifier of a street by index.
 * @param engine  Engine handle.
 * @param idx     Zero-based street index.
 * @returns Null-terminated string owned by the engine, or NULL on error.
 */
SWMM_ENGINE_API const char* swmm_street_id(SWMM_Engine engine, int idx);

/**
 * @brief Read back the geometric parameters of a street cross-section.
 *
 * @details Inverse of @ref swmm_street_set_params. Any out-pointer may be NULL
 *          if that field is not needed. Values are returned in the same units
 *          they were supplied (display units).
 *
 * @param engine         Engine handle.
 * @param idx            Zero-based street index.
 * @param[out] t_crown        Crown thickness.
 * @param[out] h_curb         Curb height.
 * @param[out] sx             Cross slope of the roadway.
 * @param[out] n_road         Manning's n for the road surface.
 * @param[out] gutter_depres  Gutter depression depth.
 * @param[out] gutter_width   Gutter width.
 * @param[out] sides          Number of sides (1 or 2).
 * @param[out] back_width     Backing (sidewalk) width.
 * @param[out] back_slope     Backing slope.
 * @param[out] back_n         Manning's n for the backing area.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_street_get_params(SWMM_Engine engine, int idx,
                                             double* t_crown, double* h_curb, double* sx, double* n_road,
                                             double* gutter_depres, double* gutter_width, int* sides,
                                             double* back_width, double* back_slope, double* back_n);

/* =========================================================================
 * Inlets
 * ========================================================================= */

/**
 * @brief Add a new inlet definition.
 * @param engine  Engine handle (SWMM_STATE_BUILDING).
 * @param id      Unique null-terminated identifier.
 * @param type    Inlet type string (e.g., "GRATE", "CURB", "SLOTTED", "CUSTOM").
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_inlet_add(SWMM_Engine engine, const char* id, const char* type);

/**
 * @brief Set the geometric parameters for an inlet.
 * @param engine        Engine handle.
 * @param idx           Zero-based inlet index.
 * @param length        Inlet length.
 * @param width         Inlet width.
 * @param grate_type    Grate type string (e.g., "P-50", "GENERIC").
 * @param open_area     Open area fraction (0–1).
 * @param splash_veloc  Splash-over velocity threshold.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_inlet_set_params(SWMM_Engine engine, int idx, double length, double width,
                                            const char* grate_type, double open_area, double splash_veloc);

/**
 * @brief Get the total number of inlet definitions in the model.
 * @param engine  Engine handle.
 * @returns Number of inlets, or -1 on error.
 */
SWMM_ENGINE_API int swmm_inlet_count(SWMM_Engine engine);

/**
 * @brief Look up an inlet's zero-based index by its string identifier.
 * @param engine  Engine handle.
 * @param id      Null-terminated inlet identifier.
 * @returns Zero-based index, or -1 if not found.
 */
SWMM_ENGINE_API int swmm_inlet_index(SWMM_Engine engine, const char* id);

/**
 * @brief Get the string identifier of an inlet by index.
 * @param engine  Engine handle.
 * @param idx     Zero-based inlet index.
 * @returns Null-terminated string owned by the engine, or NULL on error.
 */
SWMM_ENGINE_API const char* swmm_inlet_id(SWMM_Engine engine, int idx);

/* =========================================================================
 * LID controls
 * ========================================================================= */

/**
 * @brief Add a new LID (Low Impact Development) control.
 *
 * @details LID types: 0=BIO_CELL, 1=RAIN_GARDEN, 2=GREEN_ROOF,
 *          3=INFIL_TRENCH, 4=PERM_PAVEMENT, 5=RAIN_BARREL,
 *          6=ROOFTOP_DISCONN, 7=VEGETATIVE_SWALE.
 *
 * @param engine  Engine handle (SWMM_STATE_BUILDING).
 * @param id      Unique null-terminated identifier.
 * @param type    LID type code.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_lid_add(SWMM_Engine engine, const char* id, int type);

/**
 * @brief Set the surface layer properties for a LID control.
 * @param engine     Engine handle.
 * @param idx        Zero-based LID index.
 * @param storage    Surface storage depth.
 * @param roughness  Surface Manning's n.
 * @param slope      Surface slope (fraction).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_lid_set_surface(SWMM_Engine engine, int idx, double storage, double roughness, double slope);

/**
 * @brief Set the soil layer properties for a LID control.
 * @param engine    Engine handle.
 * @param idx       Zero-based LID index.
 * @param thick     Soil thickness.
 * @param porosity  Soil porosity (fraction).
 * @param fc        Field capacity (fraction).
 * @param wp        Wilting point (fraction).
 * @param ksat      Saturated hydraulic conductivity.
 * @param kslope    Slope of the conductivity–moisture curve.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_lid_set_soil(SWMM_Engine engine, int idx, double thick, double porosity, double fc, double wp, double ksat, double kslope);

/**
 * @brief Set the storage layer properties for a LID control.
 * @param engine    Engine handle.
 * @param idx       Zero-based LID index.
 * @param thick     Storage layer thickness.
 * @param void_frac Void fraction (porosity of gravel/aggregate).
 * @param ksat      Seepage rate through the storage layer bottom.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_lid_set_storage(SWMM_Engine engine, int idx, double thick, double void_frac, double ksat);

/**
 * @brief Set the underdrain properties for a LID control.
 * @param engine  Engine handle.
 * @param idx     Zero-based LID index.
 * @param coeff   Drain coefficient.
 * @param expon   Drain exponent.
 * @param offset  Drain offset height above the storage layer bottom.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_lid_set_drain(SWMM_Engine engine, int idx, double coeff, double expon, double offset);

/**
 * @brief Get the total number of LID controls in the model.
 * @param engine  Engine handle.
 * @returns Number of LID controls, or -1 on error.
 */
SWMM_ENGINE_API int swmm_lid_count(SWMM_Engine engine);

/**
 * @brief Look up a LID control's zero-based index by its string identifier.
 * @param engine  Engine handle.
 * @param id      Null-terminated LID identifier.
 * @returns Zero-based index, or -1 if not found.
 */
SWMM_ENGINE_API int swmm_lid_index(SWMM_Engine engine, const char* id);

/**
 * @brief Get the string identifier of a LID control by index.
 * @param engine  Engine handle.
 * @param idx     Zero-based LID index.
 * @returns Null-terminated string owned by the engine, or NULL on error.
 */
SWMM_ENGINE_API const char* swmm_lid_id(SWMM_Engine engine, int idx);

/* =========================================================================
 * LID usage (assign LID to subcatchment)
 * ========================================================================= */

/**
 * @brief Assign a LID control to a subcatchment.
 *
 * @details Multiple LID units of the same or different types can be assigned
 *          to a single subcatchment.
 *
 * @param engine       Engine handle.
 * @param subcatch_idx Zero-based subcatchment index.
 * @param lid_idx      Zero-based LID control index.
 * @param number       Number of replicate LID units.
 * @param area         Area of each LID unit in project area units.
 * @param width        Top width of the overland flow surface per unit.
 * @param init_sat     Initial saturation of the soil layer (0–1).
 * @param from_imperv  Fraction of impervious area runoff treated by this LID (0–1).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_lid_usage_add(SWMM_Engine engine, int subcatch_idx, int lid_idx, int number, double area, double width, double init_sat, double from_imperv);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_INFRASTRUCTURE_H */
