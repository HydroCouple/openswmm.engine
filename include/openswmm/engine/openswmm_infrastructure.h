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
