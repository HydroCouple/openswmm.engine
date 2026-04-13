/**
 * @file openswmm_links.h
 * @brief OpenSWMM Engine — Link (conduit/pump/orifice/weir/outlet) C API.
 *
 * @details Link add (BUILDING state), geometry/cross-section setters,
 *          connectivity, state get/set, control setting injection, bulk access.
 *
 * @ingroup engine_api
 * @see openswmm_engine.h
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_LINKS_H
#define OPENSWMM_LINKS_H

#include "openswmm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Link type classification.
 *
 * @details Every link in a SWMM model belongs to one of these types. The type
 *          determines which property setters are valid (e.g., pump curves
 *          only apply to SWMM_LINK_PUMP).
 */
typedef enum SWMM_LinkType {
    SWMM_LINK_CONDUIT = 0, /**< Closed or open conduit conveying flow by gravity. */
    SWMM_LINK_PUMP    = 1, /**< Pump link (flow determined by pump curve). */
    SWMM_LINK_ORIFICE = 2, /**< Orifice (circular or rectangular opening). */
    SWMM_LINK_WEIR    = 3, /**< Weir (transverse/side/V-notch/trapezoidal). */
    SWMM_LINK_OUTLET  = 4  /**< Flow-vs-head outlet structure. */
} SWMM_LinkType;

/**
 * @brief Cross-section shape codes for conduit links.
 *
 * @details Used with swmm_link_set_xsect() and swmm_link_get_xsect(). The
 *          interpretation of geom1–geom4 depends on the shape; see the
 *          SWMM 5.2 reference manual for cross-section geometry definitions.
 */
typedef enum SWMM_XSectShape {
    SWMM_XSECT_CIRCULAR        =  0, /**< Full circular pipe.                   geom1=diameter. */
    SWMM_XSECT_FILLED_CIRCULAR =  1, /**< Circular pipe with sediment deposit.  geom1=diameter, geom2=filled depth. */
    SWMM_XSECT_RECT_CLOSED     =  2, /**< Closed rectangular conduit.           geom1=height, geom2=width. */
    SWMM_XSECT_RECT_OPEN       =  3, /**< Open rectangular channel.             geom1=height, geom2=width. */
    SWMM_XSECT_TRAPEZOIDAL     =  4, /**< Trapezoidal channel.                  geom1=height, geom2=bottom width, geom3=side slope. */
    SWMM_XSECT_TRIANGULAR      =  5, /**< Triangular channel.                   geom1=height, geom2=top width. */
    SWMM_XSECT_PARABOLIC       =  6, /**< Parabolic channel.                    geom1=height, geom2=top width. */
    SWMM_XSECT_POWER           =  7, /**< Power-law shaped channel.             geom1=height, geom2=top width, geom3=exponent. */
    SWMM_XSECT_RECT_TRIANG     =  8, /**< Rectangular-triangular channel.       geom1=height, geom2=top width, geom3=triangle height. */
    SWMM_XSECT_RECT_ROUND      =  9, /**< Rectangular-round channel.            geom1=height, geom2=top width, geom3=bottom radius. */
    SWMM_XSECT_MOD_BASKET      = 10, /**< Modified baskethandle.                geom1=height, geom2=bottom width, geom3=top radius. */
    SWMM_XSECT_HORIZ_ELLIPSE   = 11, /**< Horizontal ellipse.                   geom1=height, geom2=width. */
    SWMM_XSECT_VERT_ELLIPSE    = 12, /**< Vertical ellipse.                     geom1=height, geom2=width. */
    SWMM_XSECT_ARCH            = 13, /**< Arch pipe.                            geom1=height, geom2=width. */
    SWMM_XSECT_EGGSHAPED       = 14, /**< Egg-shaped (standard).                geom1=height. */
    SWMM_XSECT_HORSESHOE       = 15, /**< Horseshoe.                            geom1=height. */
    SWMM_XSECT_GOTHIC          = 16, /**< Gothic.                               geom1=height. */
    SWMM_XSECT_CATENARY        = 17, /**< Catenary.                             geom1=height. */
    SWMM_XSECT_SEMIELLIPTICAL  = 18, /**< Semi-elliptical.                      geom1=height. */
    SWMM_XSECT_IRREGULAR       = 19  /**< Irregular (from transect data).       geom1=transect index. */
} SWMM_XSectShape;

/* =========================================================================
 * Identity
 * ========================================================================= */

/**
 * @brief Get the total number of links in the model.
 * @param engine  Engine handle.
 * @returns Number of links, or -1 on error.
 */
SWMM_ENGINE_API int swmm_link_count(SWMM_Engine engine);

/**
 * @brief Look up a link's zero-based index by its string identifier.
 * @param engine  Engine handle.
 * @param id      Null-terminated link identifier.
 * @returns Zero-based index, or -1 if not found.
 */
SWMM_ENGINE_API int swmm_link_index(SWMM_Engine engine, const char* id);

/**
 * @brief Get the string identifier of a link by index.
 * @param engine  Engine handle.
 * @param idx     Zero-based link index.
 * @returns Null-terminated string owned by the engine, or NULL on error.
 */
SWMM_ENGINE_API const char* swmm_link_id(SWMM_Engine engine, int idx);

/* =========================================================================
 * Creation (BUILDING state only)
 * ========================================================================= */

/**
 * @brief Add a new link to the model.
 *
 * @details The engine must be in SWMM_STATE_BUILDING. After creation, use
 *          swmm_link_set_nodes() to specify connectivity and the appropriate
 *          geometry/cross-section setters.
 *
 * @param engine  Engine handle.
 * @param id      Unique null-terminated identifier for the new link.
 * @param type    Link type (see @ref SWMM_LinkType).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_add(SWMM_Engine engine, const char* id, int type);

/* =========================================================================
 * Connectivity (BUILDING or OPENED)
 * ========================================================================= */

/**
 * @brief Set the upstream and downstream nodes of a link.
 * @param engine         Engine handle.
 * @param idx            Zero-based link index.
 * @param from_node_idx  Zero-based index of the upstream (inlet) node.
 * @param to_node_idx    Zero-based index of the downstream (outlet) node.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_set_nodes(SWMM_Engine engine, int idx,
                                         int from_node_idx, int to_node_idx);

/**
 * @brief Get the upstream (inlet) node index of a link.
 * @param engine          Engine handle.
 * @param idx             Zero-based link index.
 * @param[out] node_idx   Receives the upstream node index.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_get_from_node(SWMM_Engine engine, int idx, int* node_idx);

/**
 * @brief Get the downstream (outlet) node index of a link.
 * @param engine          Engine handle.
 * @param idx             Zero-based link index.
 * @param[out] node_idx   Receives the downstream node index.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_get_to_node(SWMM_Engine engine, int idx, int* node_idx);

/* =========================================================================
 * Geometry setters (BUILDING or OPENED)
 * ========================================================================= */

/**
 * @brief Set the conduit length.
 * @param engine  Engine handle.
 * @param idx     Zero-based link index.
 * @param length  Conduit length in project length units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_set_length(SWMM_Engine engine, int idx, double length);

/**
 * @brief Set the Manning's roughness coefficient.
 * @param engine  Engine handle.
 * @param idx     Zero-based link index.
 * @param n       Manning's n value.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_set_roughness(SWMM_Engine engine, int idx, double n);

/**
 * @brief Set the upstream (inlet) offset above the upstream node invert.
 * @param engine  Engine handle.
 * @param idx     Zero-based link index.
 * @param offset  Offset in project length units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_set_offset_up(SWMM_Engine engine, int idx, double offset);

/**
 * @brief Set the downstream (outlet) offset above the downstream node invert.
 * @param engine  Engine handle.
 * @param idx     Zero-based link index.
 * @param offset  Offset in project length units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_set_offset_dn(SWMM_Engine engine, int idx, double offset);

/**
 * @brief Set the initial flow in a link at simulation start.
 * @param engine  Engine handle.
 * @param idx     Zero-based link index.
 * @param flow    Initial flow in project flow units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_set_initial_flow(SWMM_Engine engine, int idx, double flow);

/**
 * @brief Set the maximum allowable flow in a link.
 * @param engine  Engine handle.
 * @param idx     Zero-based link index.
 * @param flow    Maximum flow in project flow units (0 = no limit).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_set_max_flow(SWMM_Engine engine, int idx, double flow);

/* =========================================================================
 * Cross-section (BUILDING or OPENED)
 * ========================================================================= */

/**
 * @brief Set the cross-section geometry for a conduit link.
 *
 * @details The meaning of geom1–geom4 depends on the shape; see @ref SWMM_XSectShape
 *          for per-shape documentation.
 *
 * @param engine  Engine handle.
 * @param idx     Zero-based link index.
 * @param shape   Cross-section shape code (see @ref SWMM_XSectShape).
 * @param geom1   Primary geometry parameter (usually height or diameter).
 * @param geom2   Secondary geometry parameter (usually width).
 * @param geom3   Tertiary geometry parameter (shape-dependent).
 * @param geom4   Quaternary geometry parameter (shape-dependent).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_set_xsect(SWMM_Engine engine, int idx,
                                          int shape, double geom1, double geom2,
                                          double geom3, double geom4);

/**
 * @brief Get the cross-section geometry for a conduit link.
 * @param engine       Engine handle.
 * @param idx          Zero-based link index.
 * @param[out] shape   Receives the shape code (see @ref SWMM_XSectShape).
 * @param[out] geom1   Receives the primary dimension.
 * @param[out] geom2   Receives the secondary dimension.
 * @param[out] geom3   Receives the tertiary dimension.
 * @param[out] geom4   Receives the quaternary dimension.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_get_xsect(SWMM_Engine engine, int idx,
                                          int* shape, double* geom1, double* geom2,
                                          double* geom3, double* geom4);

/* =========================================================================
 * Geometry getters
 * ========================================================================= */

/**
 * @brief Get the type of a link.
 * @param engine     Engine handle.
 * @param idx        Zero-based link index.
 * @param[out] type  Receives the link type (see @ref SWMM_LinkType).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_get_type(SWMM_Engine engine, int idx, int* type);

/**
 * @brief Get the conduit length.
 * @param engine       Engine handle.
 * @param idx          Zero-based link index.
 * @param[out] length  Receives the length in project length units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_get_length(SWMM_Engine engine, int idx, double* length);

/**
 * @brief Get the Manning's roughness coefficient.
 * @param engine  Engine handle.
 * @param idx     Zero-based link index.
 * @param[out] n  Receives the Manning's n value.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_get_roughness(SWMM_Engine engine, int idx, double* n);

/* =========================================================================
 * Hydraulic state getters/setters
 * ========================================================================= */

/**
 * @brief Get the current flow rate in a link.
 * @param engine     Engine handle.
 * @param idx        Zero-based link index.
 * @param[out] flow  Receives the flow in project flow units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_get_flow(SWMM_Engine engine, int idx, double* flow);

/**
 * @brief Set the flow rate in a link (runtime override).
 * @param engine  Engine handle (RUNNING state).
 * @param idx     Zero-based link index.
 * @param flow    New flow in project flow units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_set_flow(SWMM_Engine engine, int idx, double flow);

/**
 * @brief Get the current water depth in a link.
 * @param engine      Engine handle.
 * @param idx         Zero-based link index.
 * @param[out] depth  Receives the depth in project length units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_get_depth(SWMM_Engine engine, int idx, double* depth);

/**
 * @brief Get the current flow velocity in a link.
 * @param engine          Engine handle.
 * @param idx             Zero-based link index.
 * @param[out] velocity   Receives the velocity in project velocity units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_get_velocity(SWMM_Engine engine, int idx, double* velocity);

/**
 * @brief Get the current flow capacity utilization (depth / full depth).
 * @param engine          Engine handle.
 * @param idx             Zero-based link index.
 * @param[out] capacity   Receives the ratio [0, 1+] (can exceed 1 if surcharged).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_get_capacity(SWMM_Engine engine, int idx, double* capacity);

/**
 * @brief Get the current water volume stored in a link.
 * @param engine       Engine handle.
 * @param idx          Zero-based link index.
 * @param[out] volume  Receives the volume in project volume units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_get_volume(SWMM_Engine engine, int idx, double* volume);

/* --- Runtime forcing (RUNNING state only) --- */

/**
 * @brief Override control/pump setting on a link.
 *
 * @details For pumps: 0.0 = off, 1.0 = full speed. For orifices/weirs:
 *          fractional opening [0, 1]. Applied for current timestep only.
 */
SWMM_ENGINE_API int swmm_link_set_control_setting(SWMM_Engine engine, int idx, double setting);

/** @brief Get current control setting. */
SWMM_ENGINE_API int swmm_link_get_control_setting(SWMM_Engine engine, int idx, double* setting);

/**
 * @brief Set the target setting for a link (for gradual transitions).
 *
 * @details The target setting is what the link transitions towards. For
 *          pumps/orifices/weirs, the actual setting moves toward the target
 *          based on the link's transition rate. Use this when replicating
 *          control rule SET actions that specify a target rather than
 *          an immediate override.
 */
SWMM_ENGINE_API int swmm_link_set_target_setting(SWMM_Engine engine, int idx, double setting);

/** @brief Get the current target setting. */
SWMM_ENGINE_API int swmm_link_get_target_setting(SWMM_Engine engine, int idx, double* setting);

/**
 * @brief Open or close a link.
 *
 * @param engine  Engine handle (RUNNING state).
 * @param idx     Link index.
 * @param closed  Non-zero to close; zero to open.
 */
SWMM_ENGINE_API int swmm_link_set_closed(SWMM_Engine engine, int idx, int closed);

/** @brief Get link open/closed status. Returns 1 if closed, 0 if open. */
SWMM_ENGINE_API int swmm_link_get_closed(SWMM_Engine engine, int idx, int* closed);

/* =========================================================================
 * Pump Link API
 * ========================================================================= */

/**
 * @brief Assign a pump curve to a pump link.
 *
 * @details The curve defines the relationship between head (or volume or
 *          depth) and pump flow rate.
 *
 * @param engine     Engine handle.
 * @param idx        Zero-based link index (must be SWMM_LINK_PUMP).
 * @param curve_idx  Zero-based curve index (from swmm_curve_add()).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_set_pump_curve(SWMM_Engine engine, int idx, int curve_idx);

/**
 * @brief Get the pump curve index assigned to a pump link.
 * @param engine         Engine handle.
 * @param idx            Zero-based link index.
 * @param[out] curve_idx Receives the curve index.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_get_pump_curve(SWMM_Engine engine, int idx, int* curve_idx);

/**
 * @brief Set the initial on/off state of a pump at simulation start.
 * @param engine  Engine handle.
 * @param idx     Zero-based link index (must be SWMM_LINK_PUMP).
 * @param on      Non-zero for ON; zero for OFF.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_set_pump_init_state(SWMM_Engine engine, int idx, int on);

/**
 * @brief Get the initial on/off state of a pump.
 * @param engine   Engine handle.
 * @param idx      Zero-based link index.
 * @param[out] on  Receives 1 if initially ON, 0 if OFF.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_get_pump_init_state(SWMM_Engine engine, int idx, int* on);

/* =========================================================================
 * Weir Link API
 * ========================================================================= */

/**
 * @brief Set the crest height for a weir link.
 * @param engine  Engine handle.
 * @param idx     Zero-based link index (must be SWMM_LINK_WEIR).
 * @param h       Crest height in project length units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_set_crest_height(SWMM_Engine engine, int idx, double h);

/**
 * @brief Get the crest height for a weir link.
 * @param engine  Engine handle.
 * @param idx     Zero-based link index.
 * @param[out] h  Receives the crest height.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_get_crest_height(SWMM_Engine engine, int idx, double* h);

/**
 * @brief Set the discharge coefficient for a weir link.
 * @param engine  Engine handle.
 * @param idx     Zero-based link index (must be SWMM_LINK_WEIR).
 * @param cd      Discharge coefficient (dimensionless).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_set_discharge_coeff(SWMM_Engine engine, int idx, double cd);

/**
 * @brief Get the discharge coefficient for a weir link.
 * @param engine   Engine handle.
 * @param idx      Zero-based link index.
 * @param[out] cd  Receives the discharge coefficient.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_get_discharge_coeff(SWMM_Engine engine, int idx, double* cd);

/**
 * @brief Set the number of end contractions for a weir link.
 *
 * @details End contractions reduce the effective crest length. Typical values
 *          are 0, 1, or 2 for standard weir configurations.
 *
 * @param engine  Engine handle.
 * @param idx     Zero-based link index (must be SWMM_LINK_WEIR).
 * @param n       Number of end contractions.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_set_end_contractions(SWMM_Engine engine, int idx, double n);

/**
 * @brief Get the number of end contractions for a weir link.
 * @param engine  Engine handle.
 * @param idx     Zero-based link index.
 * @param[out] n  Receives the number of end contractions.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_get_end_contractions(SWMM_Engine engine, int idx, double* n);

/* =========================================================================
 * Conduit Loss Coefficients
 * ========================================================================= */

/**
 * @brief Set entry, exit, and average loss coefficients for a conduit.
 * @param engine  Engine handle.
 * @param idx     Zero-based link index (must be SWMM_LINK_CONDUIT).
 * @param inlet   Inlet (entry) loss coefficient.
 * @param outlet  Outlet (exit) loss coefficient.
 * @param avg     Average loss coefficient along the conduit length.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_set_loss_coeff(SWMM_Engine engine, int idx, double inlet, double outlet, double avg);

/**
 * @brief Get entry, exit, and average loss coefficients for a conduit.
 * @param engine       Engine handle.
 * @param idx          Zero-based link index.
 * @param[out] inlet   Receives the inlet loss coefficient.
 * @param[out] outlet  Receives the outlet loss coefficient.
 * @param[out] avg     Receives the average loss coefficient.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_get_loss_coeff(SWMM_Engine engine, int idx, double* inlet, double* outlet, double* avg);

/**
 * @brief Set whether a flap gate exists on a link.
 *
 * @details A flap gate prevents reverse flow through the link.
 *
 * @param engine    Engine handle.
 * @param idx       Zero-based link index.
 * @param has_gate  Non-zero to enable; zero to disable.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_set_flap_gate(SWMM_Engine engine, int idx, int has_gate);

/**
 * @brief Get whether a flap gate exists on a link.
 * @param engine        Engine handle.
 * @param idx           Zero-based link index.
 * @param[out] has_gate Receives 1 if flap gate present, 0 otherwise.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_get_flap_gate(SWMM_Engine engine, int idx, int* has_gate);

/**
 * @brief Set the seepage loss rate for a conduit.
 * @param engine  Engine handle.
 * @param idx     Zero-based link index.
 * @param rate    Seepage rate in project length/time units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_set_seep_rate(SWMM_Engine engine, int idx, double rate);

/**
 * @brief Get the seepage loss rate for a conduit.
 * @param engine     Engine handle.
 * @param idx        Zero-based link index.
 * @param[out] rate  Receives the seepage rate.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_get_seep_rate(SWMM_Engine engine, int idx, double* rate);

/**
 * @brief Set the FHWA culvert inlet geometry code.
 *
 * @details Used for computing inlet-controlled culvert flow. Code values
 *          correspond to FHWA HDS-5 chart numbers (0 = not a culvert).
 *
 * @param engine  Engine handle.
 * @param idx     Zero-based link index.
 * @param code    Culvert code (0 = none).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_set_culvert_code(SWMM_Engine engine, int idx, int code);

/**
 * @brief Get the FHWA culvert inlet geometry code.
 * @param engine     Engine handle.
 * @param idx        Zero-based link index.
 * @param[out] code  Receives the culvert code.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_get_culvert_code(SWMM_Engine engine, int idx, int* code);

/**
 * @brief Set the number of parallel barrels in a conduit.
 * @param engine  Engine handle.
 * @param idx     Zero-based link index.
 * @param n       Number of barrels (>= 1).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_set_barrels(SWMM_Engine engine, int idx, int n);

/**
 * @brief Get the number of parallel barrels in a conduit.
 * @param engine  Engine handle.
 * @param idx     Zero-based link index.
 * @param[out] n  Receives the barrel count.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_get_barrels(SWMM_Engine engine, int idx, int* n);

/**
 * @brief Get the computed slope of a conduit.
 * @param engine      Engine handle.
 * @param idx         Zero-based link index.
 * @param[out] slope  Receives the slope (dimensionless, rise/run).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_get_slope(SWMM_Engine engine, int idx, double* slope);

/**
 * @brief Get the upstream (inlet) offset.
 * @param engine       Engine handle.
 * @param idx          Zero-based link index.
 * @param[out] offset  Receives the offset in project length units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_get_offset_up(SWMM_Engine engine, int idx, double* offset);

/**
 * @brief Get the downstream (outlet) offset.
 * @param engine       Engine handle.
 * @param idx          Zero-based link index.
 * @param[out] offset  Receives the offset in project length units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_get_offset_dn(SWMM_Engine engine, int idx, double* offset);

/* =========================================================================
 * Link Statistics
 * ========================================================================= */

/**
 * @brief Get the maximum flow recorded in a link during the simulation.
 * @param engine    Engine handle (ENDED or RUNNING state).
 * @param idx       Zero-based link index.
 * @param[out] val  Receives the maximum flow in project flow units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_get_stat_max_flow(SWMM_Engine engine, int idx, double* val);

/**
 * @brief Get the maximum velocity recorded in a link.
 * @param engine    Engine handle.
 * @param idx       Zero-based link index.
 * @param[out] val  Receives the maximum velocity in project velocity units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_get_stat_max_velocity(SWMM_Engine engine, int idx, double* val);

/**
 * @brief Get the maximum depth/full-depth ratio (filling) in a link.
 * @param engine    Engine handle.
 * @param idx       Zero-based link index.
 * @param[out] val  Receives the maximum filling ratio.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_get_stat_max_filling(SWMM_Engine engine, int idx, double* val);

/**
 * @brief Get the total volume conveyed through a link.
 * @param engine    Engine handle.
 * @param idx       Zero-based link index.
 * @param[out] val  Receives the total volume in project volume units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_get_stat_vol_flow(SWMM_Engine engine, int idx, double* val);

/**
 * @brief Get the total surcharge duration for a link.
 * @param engine    Engine handle.
 * @param idx       Zero-based link index.
 * @param[out] val  Receives the surcharge duration in hours.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_get_stat_surcharge_time(SWMM_Engine engine, int idx, double* val);

/* =========================================================================
 * Water quality
 * ========================================================================= */

/**
 * @brief Get the pollutant concentration in a link.
 * @param engine        Engine handle.
 * @param link_idx      Zero-based link index.
 * @param pollutant_idx Zero-based pollutant index.
 * @param[out] conc     Receives the concentration in pollutant units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_get_quality(SWMM_Engine engine, int link_idx,
                                           int pollutant_idx, double* conc);

/* =========================================================================
 * Bulk access
 * ========================================================================= */

/**
 * @brief Get flow rates for all links in a single call.
 * @param engine    Engine handle.
 * @param[out] buf  Caller-allocated buffer of at least @p count doubles.
 * @param count     Number of elements (should equal swmm_link_count()).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_get_flows_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Get water depths for all links in a single call.
 * @param engine    Engine handle.
 * @param[out] buf  Caller-allocated buffer of at least @p count doubles.
 * @param count     Number of elements.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_get_depths_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Set flow rates for all links in a single call (runtime override).
 * @param engine  Engine handle (RUNNING state).
 * @param buf     Array of flow values, one per link.
 * @param count   Number of elements.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_set_flows_bulk(SWMM_Engine engine, const double* buf, int count);

/**
 * @brief Get pollutant concentrations for all links for one pollutant.
 * @param engine        Engine handle.
 * @param pollutant_idx Zero-based pollutant index.
 * @param[out] buf      Caller-allocated buffer of at least @p count doubles.
 * @param count         Number of elements (should equal swmm_link_count()).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_get_quality_bulk(SWMM_Engine engine, int pollutant_idx,
                                                 double* buf, int count);

/* =========================================================================
 * Pump utilization statistics
 * ========================================================================= */

/** @brief Get pump on/off cycle count. */
SWMM_ENGINE_API int swmm_link_get_stat_pump_cycles(SWMM_Engine engine, int idx, int* cycles);

/** @brief Get pump total on-time (seconds). */
SWMM_ENGINE_API int swmm_link_get_stat_pump_on_time(SWMM_Engine engine, int idx, double* seconds);

/** @brief Get pump total volume pumped (ft3). */
SWMM_ENGINE_API int swmm_link_get_stat_pump_volume(SWMM_Engine engine, int idx, double* volume);

/* =========================================================================
 * Hydraulic power
 * ========================================================================= */

/** @brief Get hydraulic power dissipated in a link (ft-lb/s). P = gamma * |Q| * |hL|. */
SWMM_ENGINE_API int swmm_link_get_hyd_power(SWMM_Engine engine, int idx, double* power);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_LINKS_H */
