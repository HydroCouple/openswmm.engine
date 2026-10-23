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
 * @file      openswmm_links.h
 * @brief     OpenSWMM Engine — Link (conduit/pump/orifice/weir/outlet) C API.
 *
 * @details Link add (BUILDING state), geometry/cross-section setters,
 *          connectivity, state get/set, control setting injection, bulk access.
 *
 * @ingroup engine_api
 * @see openswmm_engine.h
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
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
    SWMM_LINK_PUMP = 1,    /**< Pump link (flow determined by pump curve). */
    SWMM_LINK_ORIFICE = 2, /**< Orifice (circular or rectangular opening). */
    SWMM_LINK_WEIR = 3,    /**< Weir (transverse/side/V-notch/trapezoidal). */
    SWMM_LINK_OUTLET = 4   /**< Flow-vs-head outlet structure. */
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
    SWMM_XSECT_CIRCULAR = 0, /**< Full circular pipe.                   geom1=diameter. */
    SWMM_XSECT_FILLED_CIRCULAR =
        1, /**< Circular pipe with sediment deposit.  geom1=diameter, geom2=filled depth. */
    SWMM_XSECT_RECT_CLOSED =
        2, /**< Closed rectangular conduit.           geom1=height, geom2=width. */
    SWMM_XSECT_RECT_OPEN =
        3, /**< Open rectangular channel.             geom1=height, geom2=width, geom3=# sides removed (0/1/2). */
    SWMM_XSECT_TRAPEZOIDAL =
        4, /**< Trapezoidal channel.                  geom1=height, geom2=bottom width, geom3/geom4=side slopes. */
    SWMM_XSECT_TRIANGULAR =
        5, /**< Triangular channel.                   geom1=height, geom2=top width. */
    SWMM_XSECT_PARABOLIC =
        6, /**< Parabolic channel.                    geom1=height, geom2=top width. */
    SWMM_XSECT_POWER =
        7, /**< Power-law shaped channel.             geom1=height, geom2=top width, geom3=exponent. */
    SWMM_XSECT_MOD_BASKET =
        8, /**< Modified baskethandle.                geom1=height, geom2=bottom width, geom3=top radius. */
    SWMM_XSECT_EGGSHAPED = 9,       /**< Egg-shaped (standard).                geom1=height. */
    SWMM_XSECT_HORSESHOE = 10,      /**< Horseshoe.                            geom1=height. */
    SWMM_XSECT_GOTHIC = 11,         /**< Gothic.                               geom1=height. */
    SWMM_XSECT_CATENARY = 12,       /**< Catenary.                             geom1=height. */
    SWMM_XSECT_SEMIELLIPTICAL = 13, /**< Semi-elliptical.                      geom1=height. */
    SWMM_XSECT_BASKETHANDLE = 14,   /**< Baskethandle.                         geom1=height. */
    SWMM_XSECT_SEMICIRCULAR = 15,   /**< Semi-circular.                        geom1=height. */
    SWMM_XSECT_RECT_TRIANG =
        16, /**< Rectangular-triangular channel.       geom1=height, geom2=top width, geom3=triangle height. */
    SWMM_XSECT_RECT_ROUND =
        17, /**< Rectangular-round channel.            geom1=height, geom2=top width, geom3=bottom radius. */
    SWMM_XSECT_HORIZ_ELLIPSE =
        18, /**< Horizontal ellipse.                   geom1=height, geom2=width. */
    SWMM_XSECT_VERT_ELLIPSE =
        19,               /**< Vertical ellipse.                     geom1=height, geom2=width. */
    SWMM_XSECT_ARCH = 20, /**< Arch pipe.                            geom1=height, geom2=width. */
    SWMM_XSECT_IRREGULAR = 21, /**< Irregular (from transect data).       geom1=transect index. */
    SWMM_XSECT_CUSTOM =
        22, /**< Custom shape (from a shape curve).    geom1=height, geom2=shape-curve index. */
    SWMM_XSECT_FORCE_MAIN =
        23, /**< Circular force main.                  geom1=diameter, geom2=roughness (H-W C or D-W eps). */
    SWMM_XSECT_STREET = 24, /**< Street cross-section (from [STREETS]). geom1=street index. */
    SWMM_XSECT_DUMMY = 25,  /**< Dummy — no geometry; all queries return 0. */
    SWMM_XSECT_POLYGON = 26 /**< Arc/line boundary from a [CURVES] XPOLYGON entry, compressed to a
                                 piecewise-Chebyshev series. geom1=scale, geom2=open-channel flag (0/1). */
} SWMM_XSectShape;

/* =========================================================================
 * Identity
 * ========================================================================= */

/**
 * @brief Get the total number of links in the model.
 * @param[in] engine Engine handle.
 * @returns Number of links, or -1 on error.
 */
SWMM_ENGINE_API int swmm_link_count(SWMM_Engine engine);

/**
 * @brief Look up a link's zero-based index by its string identifier.
 * @param[in] engine Engine handle.
 * @param[in] id Null-terminated link identifier.
 * @returns Zero-based index, or -1 if not found.
 */
SWMM_ENGINE_API int swmm_link_index(SWMM_Engine engine, const char* id);

/**
 * @brief Get the string identifier of a link by index.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @returns Null-terminated string owned by the engine, or NULL on error.
 *
 *
 * @note Borrowed ID storage may be invalidated by name/topology edits, close/reopen or engine
 *     destruction; copy before retaining it across operations.
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
 * @param[in] engine Engine handle.
 * @param[in] id Unique null-terminated identifier for the new link.
 * @param[in] type Link type (see @ref SWMM_LinkType).
 * @returns SWMM_OK on success, SWMM_ERR_LIFECYCLE if not in an editable
 *          state, or another error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE.
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
 * @param[in] engine Engine handle.
 * @param[in] id Expected tail identifier (null-terminated).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE.
 */
SWMM_ENGINE_API int swmm_link_pop_last(SWMM_Engine engine, const char* id);

/* =========================================================================
 * Connectivity (BUILDING or OPENED)
 * ========================================================================= */

/**
 * @brief Set the upstream and downstream nodes of a link.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[in] from_node_idx Zero-based index of the upstream (inlet) node.
 * @param[in] to_node_idx Zero-based index of the downstream (outlet) node.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed link/pollutant index check returns SWMM_ERR_BADINDEX. The
 *     endpoint indices are stored without individual node-bound checks; supply existing node
 *     indices before validation/initialization.
 */
SWMM_ENGINE_API int swmm_link_set_nodes(SWMM_Engine engine, int idx, int from_node_idx,
                                        int to_node_idx);

/**
 * @brief Get the upstream (inlet) node index of a link.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[out] node_idx Receives the upstream node index.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note node_idx may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed link/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_get_from_node(SWMM_Engine engine, int idx, int* node_idx);

/**
 * @brief Get the downstream (outlet) node index of a link.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[out] node_idx Receives the downstream node index.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note node_idx may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed link/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_get_to_node(SWMM_Engine engine, int idx, int* node_idx);

/**
 * @brief Restore authored From/To orientation on every conduit the parser
 *        reversed for adverse slope (DYNWAVE/FV routing).
 *
 * @details At open, adverse-slope conduits have node1/node2, offset1/offset2,
 *          initial flow sign and inlet/outlet losses swapped in place. An
 *          editing host that displays and saves the live context should call
 *          this once after open so the user sees and edits the orientation
 *          they authored. Not needed for simulation-only hosts. The .inp
 *          writer undoes the reversal on its own regardless.
 * @param[in] engine Engine handle.
 * @param[out] count Optional; receives the number of conduits restored.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note count may be NULL to suppress that output. Requires BUILDING or OPENED; other states return
 *     SWMM_ERR_LIFECYCLE. A null engine returns SWMM_ERR_BADHANDLE.
 */
SWMM_ENGINE_API int swmm_links_restore_authored_orientation(SWMM_Engine engine, int* count);

/* =========================================================================
 * Geometry setters (BUILDING or OPENED)
 * ========================================================================= */

/**
 * @brief Set the conduit length.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[in] length Conduit length in project length units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed link/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_set_length(SWMM_Engine engine, int idx, double length);

/**
 * @brief Set the Manning's roughness coefficient.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[in] n Manning's n value.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed link/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_set_roughness(SWMM_Engine engine, int idx, double n);

/**
 * @brief Set the upstream (inlet) offset above the upstream node invert.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[in] offset Offset in project length units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed link/pollutant index check returns SWMM_ERR_BADINDEX. Offsets
 *     use authored values; the internal sediment-depth bump for FILLED_CIRCULAR sections is
 *     added/removed at this boundary.
 */
SWMM_ENGINE_API int swmm_link_set_offset_up(SWMM_Engine engine, int idx, double offset);

/**
 * @brief Set the downstream (outlet) offset above the downstream node invert.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[in] offset Offset in project length units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed link/pollutant index check returns SWMM_ERR_BADINDEX. Offsets
 *     use authored values; the internal sediment-depth bump for FILLED_CIRCULAR sections is
 *     added/removed at this boundary.
 */
SWMM_ENGINE_API int swmm_link_set_offset_dn(SWMM_Engine engine, int idx, double offset);

/**
 * @brief Set the initial flow in a link at simulation start.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[in] flow Initial flow in project flow units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Permitted in BUILDING, OPENED or INITIALIZED; other states return SWMM_ERR_LIFECYCLE. A
 *     null engine returns SWMM_ERR_BADHANDLE. A failed link/pollutant index check returns
 *     SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_set_initial_flow(SWMM_Engine engine, int idx, double flow);

/**
 * @brief Set the maximum allowable flow in a link.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[in] flow Maximum flow in project flow units (0 = no limit).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed link/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_set_max_flow(SWMM_Engine engine, int idx, double flow);

/**
 * @brief Get the initial flow in a link at simulation start.
 *
 * @details Symmetric getter for @ref swmm_link_set_initial_flow. Reads the
 *          same SoA slot the setter writes; safe to call in any
 *          post-construction engine state.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[out] flow Receives the initial flow in project flow units.
 * @returns SWMM_OK on success, or an error code.
 * @since 6.0.0 (engine gap BN-LINK-01a, added 2026-05-25)
 *
 *
 * @note flow may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed link/pollutant index check returns SWMM_ERR_BADINDEX. Reads the current flow column;
 *     during a run it no longer represents an untouched original initial condition.
 */
SWMM_ENGINE_API int swmm_link_get_initial_flow(SWMM_Engine engine, int idx, double* flow);

/**
 * @brief Get the maximum allowable flow in a link.
 *
 * @details Symmetric getter for @ref swmm_link_set_max_flow. Returns 0.0
 *          when no limit is configured (mirrors the setter's contract).
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[out] flow Receives the maximum flow in project flow units.
 * @returns SWMM_OK on success, or an error code.
 * @since 6.0.0 (engine gap BN-LINK-01b, added 2026-05-25)
 *
 *
 * @note flow may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed link/pollutant index check returns SWMM_ERR_BADINDEX.
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
    SWMM_ORIFICE_SIDE = 0,   /**< Orifice opens on the side of the upstream node. */
    SWMM_ORIFICE_BOTTOM = 1, /**< Orifice opens through the bottom of the upstream node. */
} SWMM_OrificeType;

/**
 * @brief Set the orifice flow-attack classification (SIDE / BOTTOM).
 *
 * @details Only valid on links of type @ref SWMM_LINK_ORIFICE; returns
 *          @c SWMM_ERR_BADPARAM otherwise.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[in] type Orifice type (see @ref SWMM_OrificeType).
 * @returns @c SWMM_OK on success, @c SWMM_ERR_BADPARAM if @p idx names a
 *          non-orifice link or @p type is out of range.
 * @since 6.0.0 (engine gap BN-LINK-02, added 2026-05-25)
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed link/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_set_orifice_type(SWMM_Engine engine, int idx, int type);

/**
 * @brief Get the orifice flow-attack classification.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[out] type Receives the orifice type (see @ref SWMM_OrificeType).
 * @returns @c SWMM_OK on success, @c SWMM_ERR_BADPARAM if @p idx names a
 *          non-orifice link.
 * @since 6.0.0 (engine gap BN-LINK-02, added 2026-05-25)
 *
 *
 * @note type may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed link/pollutant index check returns SWMM_ERR_BADINDEX.
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
    SWMM_WEIR_TRANSVERSE = 0,  /**< Sharp-crested transverse weir.       */
    SWMM_WEIR_SIDEFLOW = 1,    /**< Side-flow weir (USBR formula).        */
    SWMM_WEIR_VNOTCH = 2,      /**< Triangular / V-notch weir.            */
    SWMM_WEIR_TRAPEZOIDAL = 3, /**< Trapezoidal weir.                     */
    SWMM_WEIR_ROADWAY = 4,     /**< FHWA HDS-5 roadway weir.              */
} SWMM_WeirType;

/**
 * @brief Set the weir flow classification.
 *
 * @details Only valid on links of type @ref SWMM_LINK_WEIR; returns
 *          @c SWMM_ERR_BADPARAM otherwise.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[in] type Weir type (see @ref SWMM_WeirType).
 * @returns @c SWMM_OK on success, @c SWMM_ERR_BADPARAM if @p idx names a
 *          non-weir link or @p type is out of range.
 * @since 6.0.0 (engine gap BN-LINK-03, added 2026-05-25)
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed link/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_set_weir_type(SWMM_Engine engine, int idx, int type);

/**
 * @brief Get the weir flow classification.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[out] type Receives the weir type (see @ref SWMM_WeirType).
 * @returns @c SWMM_OK on success, @c SWMM_ERR_BADPARAM if @p idx names a
 *          non-weir link.
 * @since 6.0.0 (engine gap BN-LINK-03, added 2026-05-25)
 *
 *
 * @note type may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed link/pollutant index check returns SWMM_ERR_BADINDEX.
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
    SWMM_OUTLET_FUNCTIONAL_HEAD = 0,  /**< Q = Cd · H^expon (head above invert). */
    SWMM_OUTLET_FUNCTIONAL_DEPTH = 1, /**< Q = Cd · y^expon (depth at upstream node). */
    SWMM_OUTLET_TABULAR_HEAD = 2,     /**< Q from rating curve indexed by head. */
    SWMM_OUTLET_TABULAR_DEPTH = 3,    /**< Q from rating curve indexed by depth. */
} SWMM_OutletRatingType;

/**
 * @brief Set the outlet rating selector on an editable outlet link.
 *
 * Copies the field into its subtype storage. Does not convert the link type or validate associated
 * curves/other settings.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] idx Zero-based existing link index in [0,swmm_link_count(engine)).
 * @param[in] type 0=FUNCTIONAL_HEAD, 1=FUNCTIONAL_DEPTH, 2=TABULAR_HEAD, 3=TABULAR_DEPTH.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid link index.
 *     SWMM_ERR_LIFECYCLE outside BUILDING/OPENED; SWMM_ERR_BADPARAM for the wrong link type or
 *     selector outside 0..3.
 */
SWMM_ENGINE_API int swmm_link_set_outlet_rating_type(SWMM_Engine engine, int idx, int type);

/**
 * @brief Read the outlet rating selector.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] idx Zero-based existing link index in [0,swmm_link_count(engine)).
 * @param[out] type Optional destination. 0=FUNCTIONAL_HEAD, 1=FUNCTIONAL_DEPTH, 2=TABULAR_HEAD,
 *     3=TABULAR_DEPTH.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid link index.
 *     SWMM_ERR_BADPARAM for a non-OUTLET link.
 */
SWMM_ENGINE_API int swmm_link_get_outlet_rating_type(SWMM_Engine engine, int idx, int* type);

/**
 * @brief Set the outlet functional-rating exponent on an editable outlet link.
 *
 * Copies the field into its subtype storage. Does not convert the link type or validate associated
 * curves/other settings.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] idx Zero-based existing link index in [0,swmm_link_count(engine)).
 * @param[in] expon Dimensionless exponent; retained but not used for tabular rating types. No
 *     finite/range validation is imposed.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid link index.
 *     SWMM_ERR_LIFECYCLE outside BUILDING/OPENED; SWMM_ERR_BADPARAM for the wrong link type.
 */
SWMM_ENGINE_API int swmm_link_set_outlet_expon(SWMM_Engine engine, int idx, double expon);

/**
 * @brief Read the outlet functional-rating exponent.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] idx Zero-based existing link index in [0,swmm_link_count(engine)).
 * @param[out] expon Optional destination. Dimensionless exponent; retained but not used for tabular
 *     rating types.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid link index.
 *     SWMM_ERR_BADPARAM for a non-OUTLET link.
 */
SWMM_ENGINE_API int swmm_link_get_outlet_expon(SWMM_Engine engine, int idx, double* expon);

/**
 * @brief Set the pump startup depth on an editable pump link.
 *
 * Copies the field into its subtype storage. Does not convert the link type or validate associated
 * curves/other settings.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] idx Zero-based existing link index in [0,swmm_link_count(engine)).
 * @param[in] depth Project display length (ft US, m SI); no finite/nonnegative validation is
 *     imposed.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid link index.
 *     SWMM_ERR_LIFECYCLE outside BUILDING/OPENED; SWMM_ERR_BADPARAM for the wrong link type.
 */
SWMM_ENGINE_API int swmm_link_set_pump_startup_depth(SWMM_Engine engine, int idx, double depth);

/**
 * @brief Read the pump startup depth.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] idx Zero-based existing link index in [0,swmm_link_count(engine)).
 * @param[out] depth Optional destination. Project display length (ft US, m SI);
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid link index.
 *     SWMM_ERR_BADPARAM for a non-PUMP link.
 */
SWMM_ENGINE_API int swmm_link_get_pump_startup_depth(SWMM_Engine engine, int idx, double* depth);

/**
 * @brief Set the pump shutoff depth on an editable pump link.
 *
 * Copies the field into its subtype storage. Does not convert the link type or validate associated
 * curves/other settings.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] idx Zero-based existing link index in [0,swmm_link_count(engine)).
 * @param[in] depth Project display length (ft US, m SI); no finite/nonnegative validation is
 *     imposed.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid link index.
 *     SWMM_ERR_LIFECYCLE outside BUILDING/OPENED; SWMM_ERR_BADPARAM for the wrong link type.
 */
SWMM_ENGINE_API int swmm_link_set_pump_shutoff_depth(SWMM_Engine engine, int idx, double depth);

/**
 * @brief Read the pump shutoff depth.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] idx Zero-based existing link index in [0,swmm_link_count(engine)).
 * @param[out] depth Optional destination. Project display length (ft US, m SI);
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid link index.
 *     SWMM_ERR_BADPARAM for a non-PUMP link.
 */
SWMM_ENGINE_API int swmm_link_get_pump_shutoff_depth(SWMM_Engine engine, int idx, double* depth);

/**
 * @brief Set the orifice open/close rate on an editable orifice link.
 *
 * Copies the field into its subtype storage. Does not convert the link type or validate associated
 * curves/other settings.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] idx Zero-based existing link index in [0,swmm_link_count(engine)).
 * @param[in] rate Fraction of full opening per second, with 0 meaning instantaneous. This API takes
 *     a rate rather than the legacy editor time in hours; no finite/range validation is imposed.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid link index.
 *     SWMM_ERR_LIFECYCLE outside BUILDING/OPENED; SWMM_ERR_BADPARAM for the wrong link type.
 */
SWMM_ENGINE_API int swmm_link_set_orifice_open_close_rate(SWMM_Engine engine, int idx, double rate);

/**
 * @brief Read the orifice open/close rate.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] idx Zero-based existing link index in [0,swmm_link_count(engine)).
 * @param[out] rate Optional destination. Fraction of full opening per second, with 0 meaning
 *     instantaneous. This API takes a rate rather than the legacy editor time in hours; no
 *     finite/range validation is imposed.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid link index.
 *     SWMM_ERR_BADPARAM for a non-ORIFICE link.
 */
SWMM_ENGINE_API int swmm_link_get_orifice_open_close_rate(SWMM_Engine engine, int idx,
                                                          double* rate);

/* =========================================================================
 * Cross-section (BUILDING or OPENED)
 * ========================================================================= */

/**
 * @brief Set the cross-section geometry for a conduit link.
 *
 * @details The meaning of geom1–geom4 depends on the shape; see @ref SWMM_XSectShape
 *          for per-shape documentation.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[in] shape Cross-section shape code (see @ref SWMM_XSectShape).
 * @param[in] geom1 Primary geometry parameter (usually height or diameter).
 * @param[in] geom2 Secondary geometry parameter (usually width).
 * @param[in] geom3 Tertiary geometry parameter (shape-dependent).
 * @param[in] geom4 Quaternary geometry parameter (shape-dependent).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed link/pollutant index check returns SWMM_ERR_BADINDEX. Some
 *     reference-validation failures occur after shape/raw geometry fields have been assigned; this
 *     edit is not uniformly atomic. STREET/IRREGULAR geom1 is a store index, not a length.
 *     Unsupported shapes use the implementation generic geometry fallback.
 */
SWMM_ENGINE_API int swmm_link_set_xsect(SWMM_Engine engine, int idx, int shape, double geom1,
                                        double geom2, double geom3, double geom4);

/**
 * @brief Get the cross-section geometry for a conduit link.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[out] shape Receives the shape code (see @ref SWMM_XSectShape).
 * @param[out] geom1 Receives the primary dimension.
 * @param[out] geom2 Receives the secondary dimension.
 * @param[out] geom3 Receives the tertiary dimension.
 * @param[out] geom4 Receives the quaternary dimension.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note shape may be NULL to suppress that output. geom1 may be NULL to suppress that output. geom2
 *     may be NULL to suppress that output. geom3 may be NULL to suppress that output. geom4 may be
 *     NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A failed
 *     link/pollutant index check returns SWMM_ERR_BADINDEX. STREET geom1 is the referenced street
 *     index (or -1), with other geometry outputs zero. IRREGULAR geom1 is the transect index;
 *     retained geom2-4 are returned. Ordinary shapes prefer retained raw authoring; absent raw
 *     geom1 falls back to derived depth/width/area, so geom3 in that fallback is area rather than
 *     the original input field.
 */
SWMM_ENGINE_API int swmm_link_get_xsect(SWMM_Engine engine, int idx, int* shape, double* geom1,
                                        double* geom2, double* geom3, double* geom4);

/* =========================================================================
 * Geometry getters
 * ========================================================================= */

/**
 * @brief Get the type of a link.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[out] type Receives the link type (see @ref SWMM_LinkType).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note type may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed link/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_get_type(SWMM_Engine engine, int idx, int* type);

/**
 * @brief Get the conduit length.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[out] length Receives the length in project length units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note length may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed link/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_get_length(SWMM_Engine engine, int idx, double* length);

/**
 * @brief Get the Manning's roughness coefficient.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[out] n Receives the Manning's n value.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note n may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A failed
 *     link/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_get_roughness(SWMM_Engine engine, int idx, double* n);

/* =========================================================================
 * Hydraulic state getters/setters
 * ========================================================================= */

/**
 * @brief Get the current flow rate in a link.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[out] flow Receives the flow in project flow units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note flow may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed link/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_get_flow(SWMM_Engine engine, int idx, double* flow);

/**
 * @brief Set the flow rate in a link (runtime override).
 * @param[in] engine Engine handle (RUNNING state).
 * @param[in] idx Zero-based link index.
 * @param[in] flow New flow in project flow units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires RUNNING; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed link/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_set_flow(SWMM_Engine engine, int idx, double flow);

/**
 * @brief Get the current water depth in a link.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[out] depth Receives the depth in project length units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note depth may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed link/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_get_depth(SWMM_Engine engine, int idx, double* depth);

/**
 * @brief Get the current flow velocity in a link.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[out] velocity Receives the velocity in project velocity units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note velocity may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed link/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_get_velocity(SWMM_Engine engine, int idx, double* velocity);

/**
 * @brief Get the current flow capacity utilization (depth / full depth).
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[out] capacity Receives the ratio [0, 1+] (can exceed 1 if surcharged).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note capacity may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed link/pollutant index check returns SWMM_ERR_BADINDEX. The ratio is signed q/q_full,
 *     not absolute capacity use; returns zero for absent/nonpositive full-flow capacity.
 */
SWMM_ENGINE_API int swmm_link_get_capacity(SWMM_Engine engine, int idx, double* capacity);

/**
 * @brief Get the current water volume stored in a link.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[out] volume Receives the volume in project volume units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note volume may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed link/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_get_volume(SWMM_Engine engine, int idx, double* volume);

/**
 * @brief Get the portion of a link's stored volume held in the Preissmann slot.
 * @details Finite-volume routing only: the water standing above the pipe crown
 *          in the slot (`sum over cells of max(0, A - A_crown) * dx`). Always a
 *          subset of @ref swmm_link_get_volume; reads 0.0 under the dynamic-wave
 *          router and whenever the conduit is below its crown.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[out] volume Receives the slot volume in project volume units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note volume may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed link/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_get_slot_volume(SWMM_Engine engine, int idx, double* volume);

/* --- Runtime forcing (RUNNING state only) --- */

/**
 * @brief Assign a link current control setting directly during a run.
 *
 * Does not remove control rules or install a permanent override; subsequent control/actuator
 * evaluation can change effective values.
 *
 * @param[in] engine Non-null RUNNING engine.
 * @param[in] idx Zero-based existing link index in [0,swmm_link_count(engine)).
 * @param[in] setting Dimensionless setting, conventionally 0..1 for full opening/speed. Stored
 *     without clamping or finite/range validation.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid link index.
 *     SWMM_ERR_LIFECYCLE outside RUNNING.
 */
SWMM_ENGINE_API int swmm_link_set_control_setting(SWMM_Engine engine, int idx, double setting);

/**
 * @brief Read the stored link current control setting.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] idx Zero-based existing link index in [0,swmm_link_count(engine)).
 * @param[out] setting Optional dimensionless setting destination; no clamping is applied on read.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid link index.
 */
SWMM_ENGINE_API int swmm_link_get_control_setting(SWMM_Engine engine, int idx, double* setting);

/**
 * @brief Assign a link target control setting directly during a run.
 *
 * Does not remove control rules or install a permanent override; subsequent control/actuator
 * evaluation can change effective values.
 *
 * @param[in] engine Non-null RUNNING engine.
 * @param[in] idx Zero-based existing link index in [0,swmm_link_count(engine)).
 * @param[in] setting Dimensionless setting, conventionally 0..1 for full opening/speed. Stored
 *     without clamping or finite/range validation.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid link index.
 *     SWMM_ERR_LIFECYCLE outside RUNNING.
 */
SWMM_ENGINE_API int swmm_link_set_target_setting(SWMM_Engine engine, int idx, double setting);

/**
 * @brief Read the stored link target control setting.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] idx Zero-based existing link index in [0,swmm_link_count(engine)).
 * @param[out] setting Optional dimensionless setting destination; no clamping is applied on read.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid link index.
 */
SWMM_ENGINE_API int swmm_link_get_target_setting(SWMM_Engine engine, int idx, double* setting);

/**
 * @brief Open or close a link.
 *
 * @param[in] engine Engine handle (RUNNING state).
 * @param[in] idx Link index.
 * @param[in] closed Non-zero to close; zero to open.
 *
 *
 * @note Requires RUNNING; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed link/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_set_closed(SWMM_Engine engine, int idx, int closed);

/**
 * @brief Read the current link closed/open flag.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] idx Zero-based existing link index in [0,swmm_link_count(engine)).
 * @param[out] closed Optional destination: 1 when closed, 0 when open.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid link index.
 */
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
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index (must be SWMM_LINK_PUMP).
 * @param[in] curve_idx Zero-based curve index (from swmm_curve_add()).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed link/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_set_pump_curve(SWMM_Engine engine, int idx, int curve_idx);

/**
 * @brief Get the pump curve index assigned to a pump link.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[out] curve_idx Receives the curve index.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note curve_idx may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed link/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_get_pump_curve(SWMM_Engine engine, int idx, int* curve_idx);

/**
 * @brief Set the initial on/off state of a pump at simulation start.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index (must be SWMM_LINK_PUMP).
 * @param[in] on Non-zero for ON; zero for OFF.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Permitted in BUILDING, OPENED or INITIALIZED; other states return SWMM_ERR_LIFECYCLE. A
 *     null engine returns SWMM_ERR_BADHANDLE. A failed link/pollutant index check returns
 *     SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_set_pump_init_state(SWMM_Engine engine, int idx, int on);

/**
 * @brief Get the initial on/off state of a pump.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[out] on Receives 1 if initially ON, 0 if OFF.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note on may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A failed
 *     link/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_get_pump_init_state(SWMM_Engine engine, int idx, int* on);

/* =========================================================================
 * Weir Link API
 * ========================================================================= */

/**
 * @brief Set the crest height for a weir link.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index (must be SWMM_LINK_WEIR).
 * @param[in] h Crest height in project length units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed link/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_set_crest_height(SWMM_Engine engine, int idx, double h);

/**
 * @brief Get the crest height for a weir link.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[out] h Receives the crest height.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note h may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A failed
 *     link/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_get_crest_height(SWMM_Engine engine, int idx, double* h);

/**
 * @brief Set the discharge coefficient for a weir link.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index (must be SWMM_LINK_WEIR).
 * @param[in] cd Discharge coefficient (dimensionless).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed link/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_set_discharge_coeff(SWMM_Engine engine, int idx, double cd);

/**
 * @brief Get the discharge coefficient for a weir link.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[out] cd Receives the discharge coefficient.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note cd may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A failed
 *     link/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_get_discharge_coeff(SWMM_Engine engine, int idx, double* cd);

/**
 * @brief Set the number of end contractions for a weir link.
 *
 * @details End contractions reduce the effective crest length. Typical values
 *          are 0, 1, or 2 for standard weir configurations.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index (must be SWMM_LINK_WEIR).
 * @param[in] n Number of end contractions.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed link/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_set_end_contractions(SWMM_Engine engine, int idx, double n);

/**
 * @brief Get the number of end contractions for a weir link.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[out] n Receives the number of end contractions.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note n may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A failed
 *     link/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_get_end_contractions(SWMM_Engine engine, int idx, double* n);

/* =========================================================================
 * Conduit Loss Coefficients
 * ========================================================================= */

/**
 * @brief Set entry, exit, and average loss coefficients for a conduit.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index (must be SWMM_LINK_CONDUIT).
 * @param[in] inlet Inlet (entry) loss coefficient.
 * @param[in] outlet Outlet (exit) loss coefficient.
 * @param[in] avg Average loss coefficient along the conduit length.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed link/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_set_loss_coeff(SWMM_Engine engine, int idx, double inlet,
                                             double outlet, double avg);

/**
 * @brief Get entry, exit, and average loss coefficients for a conduit.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[out] inlet Receives the inlet loss coefficient.
 * @param[out] outlet Receives the outlet loss coefficient.
 * @param[out] avg Receives the average loss coefficient.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note inlet may be NULL to suppress that output. outlet may be NULL to suppress that output. avg
 *     may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A failed
 *     link/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_get_loss_coeff(SWMM_Engine engine, int idx, double* inlet,
                                             double* outlet, double* avg);

/**
 * @brief Set whether a flap gate exists on a link.
 *
 * @details A flap gate prevents reverse flow through the link.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[in] has_gate Non-zero to enable; zero to disable.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed link/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_set_flap_gate(SWMM_Engine engine, int idx, int has_gate);

/**
 * @brief Get whether a flap gate exists on a link.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[out] has_gate Receives 1 if flap gate present, 0 otherwise.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note has_gate may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed link/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_get_flap_gate(SWMM_Engine engine, int idx, int* has_gate);

/**
 * @brief Set the seepage loss rate for a conduit.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[in] rate Seepage rate in project length/time units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed link/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_set_seep_rate(SWMM_Engine engine, int idx, double rate);

/**
 * @brief Get the seepage loss rate for a conduit.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[out] rate Receives the seepage rate.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note rate may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed link/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_get_seep_rate(SWMM_Engine engine, int idx, double* rate);

/**
 * @brief Set the FHWA culvert inlet geometry code.
 *
 * @details Used for computing inlet-controlled culvert flow. Code values
 *          correspond to FHWA HDS-5 chart numbers (0 = not a culvert).
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[in] code Culvert code (0 = none).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed link/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_set_culvert_code(SWMM_Engine engine, int idx, int code);

/**
 * @brief Get the FHWA culvert inlet geometry code.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[out] code Receives the culvert code.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note code may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed link/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_get_culvert_code(SWMM_Engine engine, int idx, int* code);

/**
 * @brief Set the number of parallel barrels in a conduit.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[in] n Number of barrels (>= 1).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed link/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_set_barrels(SWMM_Engine engine, int idx, int n);

/**
 * @brief Get the number of parallel barrels in a conduit.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[out] n Receives the barrel count.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note n may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A failed
 *     link/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_get_barrels(SWMM_Engine engine, int idx, int* n);

/**
 * @brief Get the computed slope of a conduit.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[out] slope Receives the slope (dimensionless, rise/run).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note slope may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed link/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_get_slope(SWMM_Engine engine, int idx, double* slope);

/**
 * @brief Get the upstream (inlet) offset.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[out] offset Receives the offset in project length units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note offset may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed link/pollutant index check returns SWMM_ERR_BADINDEX. Offsets use authored values; the
 *     internal sediment-depth bump for FILLED_CIRCULAR sections is added/removed at this boundary.
 */
SWMM_ENGINE_API int swmm_link_get_offset_up(SWMM_Engine engine, int idx, double* offset);

/**
 * @brief Get the downstream (outlet) offset.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[out] offset Receives the offset in project length units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note offset may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed link/pollutant index check returns SWMM_ERR_BADINDEX. Offsets use authored values; the
 *     internal sediment-depth bump for FILLED_CIRCULAR sections is added/removed at this boundary.
 */
SWMM_ENGINE_API int swmm_link_get_offset_dn(SWMM_Engine engine, int idx, double* offset);

/* =========================================================================
 * Link Statistics
 * ========================================================================= */

/**
 * @brief Get the maximum flow recorded in a link during the simulation.
 * @param[in] engine Engine handle (ENDED or RUNNING state).
 * @param[in] idx Zero-based link index.
 * @param[out] val Receives the maximum flow in project flow units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note val may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A failed
 *     link/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_get_stat_max_flow(SWMM_Engine engine, int idx, double* val);

/**
 * @brief Get the maximum velocity recorded in a link.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[out] val Receives the maximum velocity in project velocity units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note val may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A failed
 *     link/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_get_stat_max_velocity(SWMM_Engine engine, int idx, double* val);

/**
 * @brief Get the maximum depth/full-depth ratio (filling) in a link.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[out] val Receives the maximum filling ratio.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note val may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A failed
 *     link/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_get_stat_max_filling(SWMM_Engine engine, int idx, double* val);

/**
 * @brief Read a link peak instantaneous numerical-slot storage share.
 *
 * Recorded by FV routing; zero under dynamic-wave routing.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] idx Zero-based existing link index in [0,swmm_link_count(engine)).
 * @param[out] val Optional destination in dimensionless ratio, normally [0,1].
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid link index.
 */
SWMM_ENGINE_API int swmm_link_get_stat_peak_slot_share(SWMM_Engine engine, int idx, double* val);

/**
 * @brief Read a link time-integrated numerical-slot storage share.
 *
 * Computes integral(slot volume dt)/integral(stored volume dt), returning zero when the denominator
 * is nonpositive.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] idx Zero-based existing link index in [0,swmm_link_count(engine)).
 * @param[out] val Optional destination in dimensionless ratio, normally [0,1].
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid link index.
 */
SWMM_ENGINE_API int swmm_link_get_stat_slot_share(SWMM_Engine engine, int idx, double* val);

/**
 * @brief Get the total volume conveyed through a link.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[out] val Receives the total volume in project volume units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note val may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A failed
 *     link/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_get_stat_vol_flow(SWMM_Engine engine, int idx, double* val);

/**
 * @brief Get the total surcharge duration for a link.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[out] val Receives the surcharge duration in seconds.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note val may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A failed
 *     link/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_get_stat_surcharge_time(SWMM_Engine engine, int idx, double* val);

/* =========================================================================
 * Water quality
 * ========================================================================= */

/**
 * @brief Get the pollutant concentration in a link.
 * @param[in] engine Engine handle.
 * @param[in] link_idx Zero-based link index.
 * @param[in] pollutant_idx Zero-based pollutant index.
 * @param[out] conc Receives the concentration in pollutant units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note conc may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed link/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_get_quality(SWMM_Engine engine, int link_idx, int pollutant_idx,
                                          double* conc);

/* =========================================================================
 * Bulk access
 * ========================================================================= */

/**
 * @brief Get flow rates for all links in a single call.
 * @param[in] engine Engine handle.
 * @param[out] buf Caller-allocated buffer of at least @p count doubles.
 * @param[in] count Number of elements (should equal swmm_link_count()).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A null buffer or nonpositive count returns
 *     SWMM_ERR_BADPARAM. Copies min(count,link count) records and leaves any buffer tail unchanged.
 */
SWMM_ENGINE_API int swmm_link_get_flows_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Get water depths for all links in a single call.
 * @param[in] engine Engine handle.
 * @param[out] buf Caller-allocated buffer of at least @p count doubles.
 * @param[in] count Number of elements.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A null buffer or nonpositive count returns
 *     SWMM_ERR_BADPARAM. Copies min(count,link count) records and leaves any buffer tail unchanged.
 */
SWMM_ENGINE_API int swmm_link_get_depths_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Set flow rates for all links in a single call (runtime override).
 * @param[in] engine Engine handle (RUNNING state).
 * @param[in] buf Array of flow values, one per link.
 * @param[in] count Number of elements.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires RUNNING; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A null buffer or nonpositive count returns SWMM_ERR_BADPARAM. Copies
 *     min(count,link count) records and leaves any buffer tail unchanged.
 */
SWMM_ENGINE_API int swmm_link_set_flows_bulk(SWMM_Engine engine, const double* buf, int count);

/**
 * @brief Get pollutant concentrations for all links for one pollutant.
 * @param[in] engine Engine handle.
 * @param[in] pollutant_idx Zero-based pollutant index.
 * @param[out] buf Caller-allocated buffer of at least @p count doubles.
 * @param[in] count Number of elements (should equal swmm_link_count()).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed link/pollutant index check returns
 *     SWMM_ERR_BADINDEX. A null buffer or nonpositive count returns SWMM_ERR_BADPARAM. Copies
 *     min(count,link count) records and leaves any buffer tail unchanged.
 */
SWMM_ENGINE_API int swmm_link_get_quality_bulk(SWMM_Engine engine, int pollutant_idx, double* buf,
                                               int count);

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
 * @brief Evaluate conveyance velocities for a prefix of links.
 *
 * Uses the true cross-section area at published depth, clamps above-crown depth to full conveyance
 * depth and splits aggregate conduit flow across barrels. Numerical-slot area does not dilute
 * conveyance velocity. Matches swmm_link_get_velocity().
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[out] buf Non-null caller-owned count-double array in project display length per second
 *     (ft/s US, m/s SI).
 * @param[in] count Positive capacity/requested prefix length; writes min(count,link count) entries
 *     and leaves the tail unchanged.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for null buf/count<=0.
 */
SWMM_ENGINE_API int swmm_link_get_velocities_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Copy signed capacity ratios for a prefix of the link table.
 *
 * Returns zero when full-flow discharge is not greater than 1e-12 internal ft3/s, including
 * non-conduit links; reverse flow yields a negative ratio.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[out] buf Non-null caller-owned array of count doubles in dimensionless q/q_full.
 * @param[in] count Positive capacity/requested prefix length; writes min(count,link count) entries
 *     and leaves the tail unchanged.
 * @return SWMM_OK, including partial prefix reads; SWMM_ERR_BADHANDLE for null engine or
 *     SWMM_ERR_BADPARAM for null buf/count<=0.
 */
SWMM_ENGINE_API int swmm_link_get_capacities_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Copy current stored water volumes for a prefix of the link table.
 *
 * Converts every stored volume from internal units; this is not a raw memcpy in SI projects.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[out] buf Non-null caller-owned array of count doubles in project display volume units.
 * @param[in] count Positive capacity/requested prefix length; writes min(count,link count) entries
 *     and leaves the tail unchanged.
 * @return SWMM_OK, including partial prefix reads; SWMM_ERR_BADHANDLE for null engine or
 *     SWMM_ERR_BADPARAM for null buf/count<=0.
 */
SWMM_ENGINE_API int swmm_link_get_volumes_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Copy current control settings for a prefix of the link table.
 *
 * Reads stored setting values without clamping.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[out] buf Non-null caller-owned array of count doubles in dimensionless.
 * @param[in] count Positive capacity/requested prefix length; writes min(count,link count) entries
 *     and leaves the tail unchanged.
 * @return SWMM_OK, including partial prefix reads; SWMM_ERR_BADHANDLE for null engine or
 *     SWMM_ERR_BADPARAM for null buf/count<=0.
 */
SWMM_ENGINE_API int swmm_link_get_control_settings_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Copy target control settings for a prefix of the link table.
 *
 * Reads stored target_setting values without clamping.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[out] buf Non-null caller-owned array of count doubles in dimensionless.
 * @param[in] count Positive capacity/requested prefix length; writes min(count,link count) entries
 *     and leaves the tail unchanged.
 * @return SWMM_OK, including partial prefix reads; SWMM_ERR_BADHANDLE for null engine or
 *     SWMM_ERR_BADPARAM for null buf/count<=0.
 */
SWMM_ENGINE_API int swmm_link_get_target_settings_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Copy hydraulic head-change power magnitudes for a prefix of the link table.
 *
 * Computes 62.4*abs(Q_cfs)*abs(head_from_ft-head_to_ft) with no conversion to watts or horsepower;
 * non-conduit links use the same expression.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[out] buf Non-null caller-owned array of count doubles in ft*lbf/s, regardless of project
 *     display units.
 * @param[in] count Positive capacity/requested prefix length; writes min(count,link count) entries
 *     and leaves the tail unchanged.
 * @return SWMM_OK, including partial prefix reads; SWMM_ERR_BADHANDLE for null engine or
 *     SWMM_ERR_BADPARAM for null buf/count<=0.
 */
SWMM_ENGINE_API int swmm_link_get_hyd_powers_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Get link IDs for all links in a single call (stride-packed UTF-8).
 *
 * @details Stride-packed format matching @ref swmm_node_get_ids_bulk : each
 *          ID is written into the slot @c buf[i*stride .. i*stride+stride-1]
 *          and NUL-terminated within its slot (truncated to @c stride-1
 *          bytes if longer). The function zero-fills the requested region
 *          on entry so trailing bytes are always NUL.
 *
 * @param[in] engine Engine handle.
 * @param[out] buf Caller-allocated buffer of @c stride*count bytes.
 * @param[in] stride Per-ID slot size in bytes (must be > 1).
 * @param[in] count Number of IDs to read.
 * @returns @c SWMM_OK on success; @c SWMM_ERR_BADHANDLE if @p engine is
 *          invalid; @c SWMM_ERR_BADPARAM if @p buf is NULL,
 *          @p stride < 2, or @p count <= 0.
 *
 * @see swmm_link_id, swmm_node_get_ids_bulk
 * @since 6.0.0
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE.
 */
SWMM_ENGINE_API int swmm_link_get_ids_bulk(SWMM_Engine engine, char* buf, int stride, int count);

/* =========================================================================
 * Pump utilization statistics
 * ========================================================================= */

/**
 * @brief Read a link recorded pump on/off cycle count.
 *
 * Does not reject non-pumps; it reads the link statistics column directly.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] idx Zero-based existing link index in [0,swmm_link_count(engine)).
 * @param[out] cycles Optional destination in integer count.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid link index.
 */
SWMM_ENGINE_API int swmm_link_get_stat_pump_cycles(SWMM_Engine engine, int idx, int* cycles);

/**
 * @brief Read a link recorded pump on-duration.
 *
 * Does not reject non-pumps; use link type or bulk sentinel outputs when classifying rows.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] idx Zero-based existing link index in [0,swmm_link_count(engine)).
 * @param[out] seconds Optional destination in seconds.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid link index.
 */
SWMM_ENGINE_API int swmm_link_get_stat_pump_on_time(SWMM_Engine engine, int idx, double* seconds);

/**
 * @brief Read a link recorded pumped water volume.
 *
 * Converts internal ft3 to display volume; does not reject non-pumps.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] idx Zero-based existing link index in [0,swmm_link_count(engine)).
 * @param[out] volume Optional destination in project display volume units.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid link index.
 */
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
 * @param[in] engine Engine handle (must be in INITIALIZED state or later
 *                      so the statistics vectors are sized).
 * @param[out] cycles Caller-allocated @c int buffer of at least @p count
 *                      entries, or @c NULL. Non-pump links get @c -1.
 * @param[out] on_time Caller-allocated @c double buffer of at least @p count
 *                      entries (seconds), or @c NULL.
 * @param[out] volume Caller-allocated @c double buffer of at least @p count
 *                      entries (ft3), or @c NULL.
 * @param[in] count Length of the caller-allocated buffers. If smaller
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
 *
 *
 * @note cycles may be NULL to suppress that output. on_time may be NULL to suppress that output.
 *     volume may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. All
 *     three output arrays NULL or count<=0 returns SWMM_ERR_BADPARAM. Copies min(count,link count)
 *     rows; uninitialized statistics are reported with the same non-pump sentinels.
 */
SWMM_ENGINE_API int swmm_link_get_pump_stats_bulk(SWMM_Engine engine, int* cycles, double* on_time,
                                                  double* volume, int count);

/* =========================================================================
 * Hydraulic power
 * ========================================================================= */

/**
 * @brief Read a link hydraulic head-change power magnitude.
 *
 * Computes 62.4*abs(Q_cfs)*abs(head_from_ft-head_to_ft); no conversion to watts/horsepower is
 * performed.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] idx Zero-based existing link index in [0,swmm_link_count(engine)).
 * @param[out] power Optional destination in ft*lbf/s, regardless of project display units.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid link index.
 */
SWMM_ENGINE_API int swmm_link_get_hyd_power(SWMM_Engine engine, int idx, double* power);

/**
 * @brief Rename an editable link in its name index.
 *
 * Numeric link indices are retained. Borrowed ID pointers may be invalidated; this wrapper does not
 * rewrite arbitrary stored text references.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] idx Zero-based existing link index in [0,swmm_link_count(engine)).
 * @param[in] newId Non-null, nonempty NUL-terminated unique ID, copied internally.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid link index.
 *     SWMM_ERR_LIFECYCLE outside BUILDING/OPENED or SWMM_ERR_BADPARAM for null/empty/duplicate ID.
 */
SWMM_ENGINE_API int swmm_link_rename(SWMM_Engine engine, int idx, const char* newId);

/* =========================================================================
 * Tag — free-form string label from the INP `[TAGS]` section
 * ========================================================================= */

/**
 * @brief Copy a link free-form tag.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] idx Zero-based existing link index in [0,swmm_link_count(engine)).
 * @param[out] buf Non-null caller-owned destination, empty when no tag exists; NUL-terminated and
 *     silently truncated to buflen-1 bytes.
 * @param[in] buflen Positive byte capacity including NUL.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid link index.
 *     SWMM_ERR_BADPARAM for null buf/nonpositive capacity.
 */
SWMM_ENGINE_API int swmm_link_get_tag(SWMM_Engine engine, int idx, char* buf, int buflen);

/**
 * @brief Copy or clear a link free-form tag.
 *
 * No lifecycle guard is imposed. Tags are keyed by link index and persist through rename.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] idx Zero-based existing link index in [0,swmm_link_count(engine)).
 * @param[in] tag Borrowed NUL-terminated text to copy; NULL/empty clears it.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid link index.
 */
SWMM_ENGINE_API int swmm_link_set_tag(SWMM_Engine engine, int idx, const char* tag);

/* =========================================================================
 * Polygon cross-section — exact arc/line boundary, changeable at run time
 * ========================================================================= */

/**
 * @brief Replace a conduit's cross-section with an arbitrary closed polygon.
 *
 * @details The boundary is compiled to a piecewise-Chebyshev closure and
 *          installed on the link, replacing whatever shape it carried. Legal
 *          both before the run and BETWEEN routing steps; during a run the
 *          water already in the conduit's cells is reconciled by @p policy.
 *
 * @param engine  engine handle.
 * @param link    link index.
 * @param x,y     boundary vertices in PROJECT length units, counter-clockwise
 *                or clockwise (the orientation is normalised). Point i joins
 *                point (i+1) mod n; the chain closes automatically, so do not
 *                repeat the first vertex.
 * @param n       vertex count (>= 3).
 * @param policy  0 = CONSERVE_DEPTH — solid material INTRUDES (sediment, a
 *                CIPP liner). The free surface stays put and the displaced
 *                water leaves the conduit.
 *                1 = CONSERVE_VOLUME — material is REMOVED (corrosion,
 *                erosion, cleaning). The water stays and the surface moves.
 *                There is deliberately no default: the two describe opposite
 *                physical events, and choosing wrongly is silent — the run
 *                completes and the numbers are wrong.
 * @param[out] displaced_volume_out  water volume removed from the conduit, in
 *                PROJECT volume units (+ = removed). Always 0 for
 *                CONSERVE_VOLUME by construction. **Book this into your mass
 *                balance**: an unbooked change is indistinguishable from a
 *                continuity error in the report. May be NULL.
 *
 * @returns SWMM_OK;
 *          SWMM_ERR_BADHANDLE / SWMM_ERR_BADINDEX / SWMM_ERR_BADPARAM as usual;
 *          SWMM_ERR_GEOMETRY when the polygon is not a valid simple closed
 *          boundary (fewer than 3 points, self-intersecting, zero area,
 *          non-finite coordinate), when it does not compile to a usable
 *          closure, or when the link is not an FV-routed conduit (below).
 *
 * @warning **Run-time change is FV-only.** Under `FLOW_ROUTING FV` the solver
 *          state is per-cell flow AREA, which is exactly what a section change
 *          has to be reconciled against. DYNWAVE and KINWAVE instead hold
 *          per-link state derived from the section at init (conveyance, full
 *          flow, the Picard depth/area history), and there is no defined way to
 *          re-seed that mid-Picard. Calling this once the run has STARTED under
 *          any other routing model returns SWMM_ERR_GEOMETRY rather than
 *          quietly producing state that half describes each section. Before the
 *          run starts it is accepted in every routing model — the section is
 *          simply part of the model at that point.
 *
 * @warning **Not carried in hot starts.** A hotstart file records depths and
 *          flows, not geometry, so a file saved after a run-time change reloads
 *          with the `.inp` shape and the changed run's depths. An end-of-run
 *          `[FILES] SAVE HOTSTART` appends a plain-text warning to the run's
 *          warning list when any link's geometry was changed at run time;
 *          `swmm_hotstart_save()`, called directly at an arbitrary point, does
 *          not check for this and emits no warning. Re-apply the polygons
 *          after loading either way.
 *
 * @since 6.0.0
 */
SWMM_ENGINE_API int swmm_link_set_polygon(SWMM_Engine engine, int link,
                                           const double* x, const double* y,
                                           int n, int policy,
                                           double* displaced_volume_out);

/**
 * @brief Read back a conduit's polygon boundary.
 *
 * @details Returns the vertices of the boundary chain currently installed on
 *          the link, in PROJECT length units. Pass @p x and @p y as NULL to
 *          query the count first.
 *
 * @param engine  Engine handle.
 * @param link    Zero-based link index.
 * @param[out] x  X-coordinates of the boundary vertices, or NULL to query
 *                the count only.
 * @param[out] y  Y-coordinates of the boundary vertices, or NULL to query
 *                the count only.
 * @param[in,out] n  on entry, the capacity of @p x / @p y; on return, the
 *                   vertex count. Required (non-NULL).
 *
 * @returns SWMM_OK; SWMM_ERR_GEOMETRY when the link carries no polygon
 *          boundary; SWMM_ERR_BADPARAM when @p n is NULL or the buffers are
 *          too small (`*n` is still set to the required count).
 *
 * @note Arc segments are reported by their endpoints only — the bulge is not
 *       recoverable through this call, so a boundary that came from a
 *       `[CURVES] XPOLYGON` with arcs reads back as the inscribed polyline.
 *       A boundary set through swmm_link_set_polygon round-trips exactly.
 *
 * @since 6.0.0
 */
SWMM_ENGINE_API int swmm_link_get_polygon(SWMM_Engine engine, int link,
                                           double* x, double* y, int* n);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_LINKS_H */
