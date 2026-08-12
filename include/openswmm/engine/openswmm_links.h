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
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
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
 *
 * @warning **Renumbered in 6.0.** These codes are the engine's own storage
 *          codes (`openswmm::XsectShape`). Prior to 6.0 every constant from
 *          8 upward named a *different* shape than the one the engine stored,
 *          so e.g. `SWMM_XSECT_IRREGULAR` silently produced a vertical
 *          ellipse. Code that passed these constants symbolically and is
 *          recompiled against this header is corrected automatically; code
 *          that hard-coded the old integers must be renumbered.
 *          `swmm_xsect_shape_name()` is provided to check a code at runtime.
 *
 * @note These are NOT the legacy SWMM 5 `XsectType` codes (which put DUMMY at
 *       0 and are used internally by the geometry kernels); the engine
 *       translates between the two at its own boundary.
 */
typedef enum SWMM_XSectShape {
    SWMM_XSECT_CIRCULAR        =  0, /**< Full circular pipe.                   geom1=diameter. */
    SWMM_XSECT_FILLED_CIRCULAR =  1, /**< Circular pipe with sediment deposit.  geom1=diameter, geom2=filled depth. */
    SWMM_XSECT_RECT_CLOSED     =  2, /**< Closed rectangular conduit.           geom1=height, geom2=width. */
    SWMM_XSECT_RECT_OPEN       =  3, /**< Open rectangular channel.             geom1=height, geom2=width, geom3=# sides removed (0/1/2). */
    SWMM_XSECT_TRAPEZOIDAL     =  4, /**< Trapezoidal channel.                  geom1=height, geom2=bottom width, geom3/geom4=side slopes. */
    SWMM_XSECT_TRIANGULAR      =  5, /**< Triangular channel.                   geom1=height, geom2=top width. */
    SWMM_XSECT_PARABOLIC       =  6, /**< Parabolic channel.                    geom1=height, geom2=top width. */
    SWMM_XSECT_POWER           =  7, /**< Power-law shaped channel.             geom1=height, geom2=top width, geom3=exponent. */
    SWMM_XSECT_MOD_BASKET      =  8, /**< Modified baskethandle.                geom1=height, geom2=bottom width, geom3=top radius. */
    SWMM_XSECT_EGGSHAPED       =  9, /**< Egg-shaped (standard).                geom1=height. */
    SWMM_XSECT_HORSESHOE       = 10, /**< Horseshoe.                            geom1=height. */
    SWMM_XSECT_GOTHIC          = 11, /**< Gothic.                               geom1=height. */
    SWMM_XSECT_CATENARY        = 12, /**< Catenary.                             geom1=height. */
    SWMM_XSECT_SEMIELLIPTICAL  = 13, /**< Semi-elliptical.                      geom1=height. */
    SWMM_XSECT_BASKETHANDLE    = 14, /**< Baskethandle.                         geom1=height. */
    SWMM_XSECT_SEMICIRCULAR    = 15, /**< Semi-circular.                        geom1=height. */
    SWMM_XSECT_RECT_TRIANG     = 16, /**< Rectangular-triangular channel.       geom1=height, geom2=top width, geom3=triangle height. */
    SWMM_XSECT_RECT_ROUND      = 17, /**< Rectangular-round channel.            geom1=height, geom2=top width, geom3=bottom radius. */
    SWMM_XSECT_HORIZ_ELLIPSE   = 18, /**< Horizontal ellipse.                   geom1=height, geom2=width. */
    SWMM_XSECT_VERT_ELLIPSE    = 19, /**< Vertical ellipse.                     geom1=height, geom2=width. */
    SWMM_XSECT_ARCH            = 20, /**< Arch pipe.                            geom1=height, geom2=width. */
    SWMM_XSECT_IRREGULAR       = 21, /**< Irregular (from transect data).       geom1=transect index. */
    SWMM_XSECT_CUSTOM          = 22, /**< Custom shape (from a shape curve).    geom1=height, geom2=shape-curve index. */
    SWMM_XSECT_FORCE_MAIN      = 23, /**< Circular force main.                  geom1=diameter, geom2=roughness (H-W C or D-W eps). */
    SWMM_XSECT_STREET          = 24, /**< Street cross-section (from [STREETS]). geom1=street index. */
    SWMM_XSECT_DUMMY           = 25  /**< Dummy — no geometry; all queries return 0. */
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
 * Creation (BUILDING or OPENED — "editable" states)
 * ========================================================================= */

/**
 * @brief Add a new link to the model.
 *
 * @details The engine must be in SWMM_STATE_BUILDING (programmatic
 *          construction) or SWMM_STATE_OPENED (interactive editing after
 *          the .inp has been parsed). Returns SWMM_ERR_LIFECYCLE for any
 *          other state. After creation, use swmm_link_set_nodes() to
 *          specify connectivity and the appropriate geometry /
 *          cross-section setters.
 *
 * @param engine  Engine handle.
 * @param id      Unique null-terminated identifier for the new link.
 * @param type    Link type (see @ref SWMM_LinkType).
 * @returns SWMM_OK on success, SWMM_ERR_LIFECYCLE if not in an editable
 *          state, or another error code.
 */
SWMM_ENGINE_API int swmm_link_add(SWMM_Engine engine, const char* id, int type);

/**
 * @brief Remove the most recently added link (undo-of-add).
 *
 * @details Pops the tail of the link list. The engine must be in
 *          SWMM_STATE_BUILDING or SWMM_STATE_OPENED. Returns
 *          SWMM_ERR_BADINDEX if the tail doesn't match \p id, or
 *          SWMM_ERR_LIFECYCLE if the state is wrong.
 *
 *          As with @ref swmm_node_pop_last, this is the narrow
 *          "rollback-of-add" surface; a general swmm_link_remove(idx)
 *          requires renumbering every reference (controls, reports,
 *          etc.) and is tracked separately.
 *
 * @param engine  Engine handle.
 * @param id      Expected tail identifier (null-terminated).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_link_pop_last(SWMM_Engine engine, const char* id);

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

/**
 * @brief Get the initial flow in a link at simulation start.
 *
 * @details Symmetric getter for @ref swmm_link_set_initial_flow. Reads the
 *          same SoA slot the setter writes; safe to call in any
 *          post-construction engine state.
 *
 * @param engine     Engine handle.
 * @param idx        Zero-based link index.
 * @param[out] flow  Receives the initial flow in project flow units.
 * @returns SWMM_OK on success, or an error code.
 * @since 6.0.0 (engine gap BN-LINK-01a, added 2026-05-25)
 */
SWMM_ENGINE_API int swmm_link_get_initial_flow(SWMM_Engine engine, int idx, double* flow);

/**
 * @brief Get the maximum allowable flow in a link.
 *
 * @details Symmetric getter for @ref swmm_link_set_max_flow. Returns 0.0
 *          when no limit is configured (mirrors the setter's contract).
 *
 * @param engine     Engine handle.
 * @param idx        Zero-based link index.
 * @param[out] flow  Receives the maximum flow in project flow units.
 * @returns SWMM_OK on success, or an error code.
 * @since 6.0.0 (engine gap BN-LINK-01b, added 2026-05-25)
 */
SWMM_ENGINE_API int swmm_link_get_max_flow(SWMM_Engine engine, int idx, double* flow);

/**
 * @brief Orifice flow-attack classification.
 *
 * @details Used with @ref swmm_link_set_orifice_type and
 *          @ref swmm_link_get_orifice_type. Order matches the legacy
 *          SWMM-GUI combo (`SWMM-GUI/Epaswmm5/objprops.txt:862`).
 * @since 6.0.0 (engine gap BN-LINK-02, added 2026-05-25)
 */
typedef enum SWMM_OrificeType {
    SWMM_ORIFICE_SIDE   = 0, /**< Orifice opens on the side of the upstream node. */
    SWMM_ORIFICE_BOTTOM = 1, /**< Orifice opens through the bottom of the upstream node. */
} SWMM_OrificeType;

/**
 * @brief Set the orifice flow-attack classification (SIDE / BOTTOM).
 *
 * @details Only valid on links of type @ref SWMM_LINK_ORIFICE; returns
 *          @c SWMM_ERR_BADPARAM otherwise.
 *
 * @param engine  Engine handle.
 * @param idx     Zero-based link index.
 * @param type    Orifice type (see @ref SWMM_OrificeType).
 * @returns @c SWMM_OK on success, @c SWMM_ERR_BADPARAM if @p idx names a
 *          non-orifice link or @p type is out of range.
 * @since 6.0.0 (engine gap BN-LINK-02, added 2026-05-25)
 */
SWMM_ENGINE_API int swmm_link_set_orifice_type(SWMM_Engine engine, int idx, int type);

/**
 * @brief Get the orifice flow-attack classification.
 *
 * @param engine     Engine handle.
 * @param idx        Zero-based link index.
 * @param[out] type  Receives the orifice type (see @ref SWMM_OrificeType).
 * @returns @c SWMM_OK on success, @c SWMM_ERR_BADPARAM if @p idx names a
 *          non-orifice link.
 * @since 6.0.0 (engine gap BN-LINK-02, added 2026-05-25)
 */
SWMM_ENGINE_API int swmm_link_get_orifice_type(SWMM_Engine engine, int idx, int* type);

/**
 * @brief Weir-flow classification.
 *
 * @details Used with @ref swmm_link_set_weir_type and
 *          @ref swmm_link_get_weir_type. Numeric order matches the
 *          legacy WeirType enum in `legacy/engine/enums.h:925` and the
 *          legacy SWMM-GUI combo (`SWMM-GUI/Epaswmm5/objprops.txt:160`).
 *
 *          The companion "Shape" attribute in the legacy GUI is derived
 *          from the weir type (see `objprops.txt:162` for the mapping)
 *          and need not be stored separately; clients that want the
 *          shape should consult @ref swmm_link_get_xsect.
 *
 * @since 6.0.0 (engine gap BN-LINK-03, added 2026-05-25)
 */
typedef enum SWMM_WeirType {
    SWMM_WEIR_TRANSVERSE  = 0, /**< Sharp-crested transverse weir.       */
    SWMM_WEIR_SIDEFLOW    = 1, /**< Side-flow weir (USBR formula).        */
    SWMM_WEIR_VNOTCH      = 2, /**< Triangular / V-notch weir.            */
    SWMM_WEIR_TRAPEZOIDAL = 3, /**< Trapezoidal weir.                     */
    SWMM_WEIR_ROADWAY     = 4, /**< FHWA HDS-5 roadway weir.              */
} SWMM_WeirType;

/**
 * @brief Set the weir flow classification.
 *
 * @details Only valid on links of type @ref SWMM_LINK_WEIR; returns
 *          @c SWMM_ERR_BADPARAM otherwise.
 *
 * @param engine  Engine handle.
 * @param idx     Zero-based link index.
 * @param type    Weir type (see @ref SWMM_WeirType).
 * @returns @c SWMM_OK on success, @c SWMM_ERR_BADPARAM if @p idx names a
 *          non-weir link or @p type is out of range.
 * @since 6.0.0 (engine gap BN-LINK-03, added 2026-05-25)
 */
SWMM_ENGINE_API int swmm_link_set_weir_type(SWMM_Engine engine, int idx, int type);

/**
 * @brief Get the weir flow classification.
 *
 * @param engine     Engine handle.
 * @param idx        Zero-based link index.
 * @param[out] type  Receives the weir type (see @ref SWMM_WeirType).
 * @returns @c SWMM_OK on success, @c SWMM_ERR_BADPARAM if @p idx names a
 *          non-weir link.
 * @since 6.0.0 (engine gap BN-LINK-03, added 2026-05-25)
 */
SWMM_ENGINE_API int swmm_link_get_weir_type(SWMM_Engine engine, int idx, int* type);

/**
 * @brief Outlet rating-curve classification.
 *
 * @details Used with @ref swmm_link_set_outlet_rating_type and
 *          @ref swmm_link_get_outlet_rating_type. Numeric encoding
 *          matches the legacy `LinksHandler::handle_outlets`
 *          convention (`src/engine/input/handlers/LinksHandler.cpp:214-221`)
 *          and the legacy SWMM-GUI combo order at
 *          `SWMM-GUI/Epaswmm5/objprops.txt:913`.
 *
 *          FUNCTIONAL types use the @c cd (coefficient) and the
 *          outlet exponent (see @ref swmm_link_set_outlet_expon)
 *          to define the rating curve; TABULAR types use the
 *          curve assigned via @ref swmm_link_set_pump_curve (the
 *          engine shares the curve-index slot between pumps and
 *          tabular outlets).
 *
 * @since 6.0.0 (engine gap BN-LINK-04, added 2026-05-25)
 */
typedef enum SWMM_OutletRatingType {
    SWMM_OUTLET_FUNCTIONAL_HEAD  = 0, /**< Q = Cd · H^expon (head above invert). */
    SWMM_OUTLET_FUNCTIONAL_DEPTH = 1, /**< Q = Cd · y^expon (depth at upstream node). */
    SWMM_OUTLET_TABULAR_HEAD     = 2, /**< Q from rating curve indexed by head. */
    SWMM_OUTLET_TABULAR_DEPTH    = 3, /**< Q from rating curve indexed by depth. */
} SWMM_OutletRatingType;

/**
 * @brief Set the outlet rating-curve classification.
 * @returns @c SWMM_OK on success, @c SWMM_ERR_BADPARAM if @p idx names a
 *          non-outlet link or @p type is out of range.
 * @since 6.0.0 (engine gap BN-LINK-04, added 2026-05-25)
 */
SWMM_ENGINE_API int swmm_link_set_outlet_rating_type(SWMM_Engine engine, int idx, int type);

/**
 * @brief Get the outlet rating-curve classification.
 * @returns @c SWMM_OK on success, @c SWMM_ERR_BADPARAM if @p idx names a
 *          non-outlet link.
 * @since 6.0.0 (engine gap BN-LINK-04, added 2026-05-25)
 */
SWMM_ENGINE_API int swmm_link_get_outlet_rating_type(SWMM_Engine engine, int idx, int* type);

/**
 * @brief Set the outlet functional-form exponent.
 *
 * @details Only meaningful for FUNCTIONAL_* rating types — the engine
 *          ignores the stored value when the type is TABULAR_*. The
 *          coefficient term (Cd) is accessed via
 *          @ref swmm_link_set_discharge_coeff / @ref swmm_link_get_discharge_coeff.
 *
 * @returns @c SWMM_OK on success, @c SWMM_ERR_BADPARAM if @p idx names a
 *          non-outlet link.
 * @since 6.0.0 (engine gap BN-LINK-04, added 2026-05-25)
 */
SWMM_ENGINE_API int swmm_link_set_outlet_expon(SWMM_Engine engine, int idx, double expon);

/**
 * @brief Get the outlet functional-form exponent.
 * @returns @c SWMM_OK on success, @c SWMM_ERR_BADPARAM if @p idx names a
 *          non-outlet link.
 * @since 6.0.0 (engine gap BN-LINK-04, added 2026-05-25)
 */
SWMM_ENGINE_API int swmm_link_get_outlet_expon(SWMM_Engine engine, int idx, double* expon);

/**
 * @brief Set the pump startup depth (depth at upstream node when the
 *        pump turns on, project length units).
 * @returns @c SWMM_OK on success, @c SWMM_ERR_BADPARAM if @p idx names a
 *          non-pump link.
 * @since 6.0.0 (engine gap BN-LINK-05, added 2026-05-25)
 */
SWMM_ENGINE_API int swmm_link_set_pump_startup_depth(SWMM_Engine engine, int idx, double depth);

/** @brief Get the pump startup depth. @since 6.0.0 (BN-LINK-05) */
SWMM_ENGINE_API int swmm_link_get_pump_startup_depth(SWMM_Engine engine, int idx, double* depth);

/**
 * @brief Set the pump shutoff depth (depth at upstream node when the
 *        pump turns off, project length units).
 * @returns @c SWMM_OK on success, @c SWMM_ERR_BADPARAM if @p idx names a
 *          non-pump link.
 * @since 6.0.0 (engine gap BN-LINK-05, added 2026-05-25)
 */
SWMM_ENGINE_API int swmm_link_set_pump_shutoff_depth(SWMM_Engine engine, int idx, double depth);

/** @brief Get the pump shutoff depth. @since 6.0.0 (BN-LINK-05) */
SWMM_ENGINE_API int swmm_link_get_pump_shutoff_depth(SWMM_Engine engine, int idx, double* depth);

/**
 * @brief Set the orifice open/close rate (fraction per second).
 *
 * @details 0 means instantaneous open/close. The legacy SWMM-GUI surfaces
 *          this field as "Time to Open/Close" measured in hours; clients
 *          that want the hours-based UX should compute
 *          @c rate = 1.0 / (3600 * hours) before calling this setter.
 *
 * @returns @c SWMM_OK on success, @c SWMM_ERR_BADPARAM if @p idx names a
 *          non-orifice link.
 * @since 6.0.0 (engine gap BN-LINK-06, added 2026-05-25)
 */
SWMM_ENGINE_API int swmm_link_set_orifice_open_close_rate(SWMM_Engine engine, int idx, double rate);

/** @brief Get the orifice open/close rate (fraction per second). @since 6.0.0 (BN-LINK-06) */
SWMM_ENGINE_API int swmm_link_get_orifice_open_close_rate(SWMM_Engine engine, int idx, double* rate);

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
 * Phase 3 bulk getters — added in OpenSWMM 6.0.0 to eliminate the N
 * round-trip cost of per-link scalar accessors in whole-network consumers
 * (notably the MCP server's get_link_info(all) path and post-run reports).
 *
 * Note: velocities, capacities, and hydraulic powers are *derived* values
 * (depth/flow ratios; flow * head loss). Their bulk variants do a per-link
 * loop in C — there is no SoA column to memcpy from — but they still
 * eliminate the C ABI crossing overhead and any Python-level looping cost.
 * ========================================================================= */

/**
 * @brief Get cross-sectional velocities for all links in a single call.
 * @details Bulk variant of @ref swmm_link_get_velocity. The C side
 *          recomputes @c q / area per link (area approximated from
 *          @c d / y_full * a_full), so this is a per-link loop rather
 *          than a memcpy — but still O(n_links) and free of per-call ABI
 *          overhead.
 * @param engine    Engine handle.
 * @param[out] buf  Caller-allocated buffer of at least @p count doubles.
 * @param count     Number of elements (should equal swmm_link_count()).
 * @returns @c SWMM_OK on success, or an error code.
 * @since 6.0.0
 */
SWMM_ENGINE_API int swmm_link_get_velocities_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Get capacity ratios (q/q_full) for all links in a single call.
 * @details Bulk variant of @ref swmm_link_get_capacity. Per-link loop
 *          (capacity is derived from flow / full-flow).
 * @since 6.0.0
 */
SWMM_ENGINE_API int swmm_link_get_capacities_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Get stored volumes for all links in a single call.
 * @details Bulk variant of @ref swmm_link_get_volume. Simple SoA memcpy.
 * @since 6.0.0
 */
SWMM_ENGINE_API int swmm_link_get_volumes_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Get active control settings (0..1) for all links in a single call.
 * @details Bulk variant of @ref swmm_link_get_control_setting.
 * @since 6.0.0
 */
SWMM_ENGINE_API int swmm_link_get_control_settings_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Get target control settings for all links in a single call.
 * @details Bulk variant of @ref swmm_link_get_target_setting.
 * @since 6.0.0
 */
SWMM_ENGINE_API int swmm_link_get_target_settings_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Get hydraulic power dissipated in every link in a single call.
 * @details Bulk variant of @ref swmm_link_get_hyd_power. Per-link loop:
 *          @c P = gamma * |Q| * |h_up - h_dn| (ft-lb/s); non-conduit
 *          links produce the same expression with whatever flow they
 *          report. Use cycles[i] from @ref swmm_link_get_pump_stats_bulk
 *          to filter to pumps if needed.
 * @since 6.0.0
 */
SWMM_ENGINE_API int swmm_link_get_hyd_powers_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Get link IDs for all links in a single call (stride-packed UTF-8).
 *
 * @details Stride-packed format matching @ref swmm_node_get_ids_bulk: each
 *          ID is written into the slot @c buf[i*stride .. i*stride+stride-1]
 *          and NUL-terminated within its slot (truncated to @c stride-1
 *          bytes if longer). The function zero-fills the requested region
 *          on entry so trailing bytes are always NUL.
 *
 * @param engine    Engine handle.
 * @param[out] buf  Caller-allocated buffer of @c stride*count bytes.
 * @param stride    Per-ID slot size in bytes (must be > 1).
 * @param count     Number of IDs to read.
 * @returns @c SWMM_OK on success; @c SWMM_ERR_BADHANDLE if @p engine is
 *          invalid; @c SWMM_ERR_BADPARAM if @p buf is NULL,
 *          @p stride < 2, or @p count <= 0.
 *
 * @see swmm_link_id, swmm_node_get_ids_bulk
 * @since 6.0.0
 */
SWMM_ENGINE_API int swmm_link_get_ids_bulk(SWMM_Engine engine,
                                            char* buf,
                                            int stride,
                                            int count);

/* =========================================================================
 * Pump utilization statistics
 * ========================================================================= */

/** @brief Get pump on/off cycle count. */
SWMM_ENGINE_API int swmm_link_get_stat_pump_cycles(SWMM_Engine engine, int idx, int* cycles);

/** @brief Get pump total on-time (seconds). */
SWMM_ENGINE_API int swmm_link_get_stat_pump_on_time(SWMM_Engine engine, int idx, double* seconds);

/** @brief Get pump total volume pumped (ft3). */
SWMM_ENGINE_API int swmm_link_get_stat_pump_volume(SWMM_Engine engine, int idx, double* volume);

/**
 * @brief Get pump utilization statistics for **all** links in a single call.
 *
 * @details Single-pass bulk accessor that avoids @c N round-trips through the
 *          C ABI when caller needs pump stats across the network (e.g. when
 *          building a network-wide pump summary report). For links whose type
 *          is not @c LinkType::PUMP, the corresponding @p cycles entry is set
 *          to @c -1 and the @p on_time / @p volume entries to @c 0.0 — this
 *          allows the caller to distinguish "non-pump" from "pump with zero
 *          cycles".
 *
 *          Any of @p cycles, @p on_time, @p volume may be @c NULL if the
 *          caller does not need that output; the function still iterates the
 *          full link array (the cost is identical) but skips the store.
 *
 * @param engine        Engine handle (must be in INITIALIZED state or later
 *                      so the statistics vectors are sized).
 * @param[out] cycles   Caller-allocated @c int buffer of at least @p count
 *                      entries, or @c NULL. Non-pump links get @c -1.
 * @param[out] on_time  Caller-allocated @c double buffer of at least @p count
 *                      entries (seconds), or @c NULL.
 * @param[out] volume   Caller-allocated @c double buffer of at least @p count
 *                      entries (ft3), or @c NULL.
 * @param count         Length of the caller-allocated buffers. If smaller
 *                      than the link count, only the first @c min(count,
 *                      n_links) entries are written.
 *
 * @returns @c SWMM_OK on success; @c SWMM_ERR_BADHANDLE if @p engine is
 *          invalid; @c SWMM_ERR_BADPARAM if @p count is non-positive or all
 *          three output pointers are NULL.
 *
 * @par Example
 * @code{.c}
 *   int n = swmm_link_count(eng);
 *   int* cycles = malloc(n * sizeof(int));
 *   double* on_time = malloc(n * sizeof(double));
 *   double* volume = malloc(n * sizeof(double));
 *   swmm_link_get_pump_stats_bulk(eng, cycles, on_time, volume, n);
 *   for (int i = 0; i < n; ++i) {
 *       if (cycles[i] < 0) continue;            // not a pump
 *       printf("link %d: %d cycles, %.1f s, %.2f ft3\n",
 *              i, cycles[i], on_time[i], volume[i]);
 *   }
 * @endcode
 *
 * @note Equivalent to calling @ref swmm_link_get_stat_pump_cycles,
 *       @ref swmm_link_get_stat_pump_on_time, and
 *       @ref swmm_link_get_stat_pump_volume for every link, but with one C
 *       ABI crossing instead of @c 3N.
 *
 * @see swmm_link_get_stat_pump_cycles
 * @see swmm_link_get_stat_pump_on_time
 * @see swmm_link_get_stat_pump_volume
 *
 * @since 6.0.0
 */
SWMM_ENGINE_API int swmm_link_get_pump_stats_bulk(SWMM_Engine engine,
                                                   int* cycles,
                                                   double* on_time,
                                                   double* volume,
                                                   int count);

/* =========================================================================
 * Hydraulic power
 * ========================================================================= */

/** @brief Get hydraulic power dissipated in a link (ft-lb/s). P = gamma * |Q| * |hL|. */
SWMM_ENGINE_API int swmm_link_get_hyd_power(SWMM_Engine engine, int idx, double* power);

/** @brief Rename the link at `idx` to `newId`.
 *  Returns SWMM_ERR_BADPARAM if newId is null, empty, already in use, or
 *  idx is out of range. */
SWMM_ENGINE_API int swmm_link_rename(SWMM_Engine engine, int idx, const char* newId);

/* =========================================================================
 * Tag — free-form string label from the INP `[TAGS]` section
 * ========================================================================= */

/** @brief Read the link's tag into `buf` (NUL-terminated, truncated if too small). */
SWMM_ENGINE_API int swmm_link_get_tag(SWMM_Engine engine, int idx,
                                       char* buf, int buflen);

/** @brief Set or clear the link's tag. Null/empty clears. Persists across rename. */
SWMM_ENGINE_API int swmm_link_set_tag(SWMM_Engine engine, int idx,
                                       const char* tag);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_LINKS_H */
