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
 * @file      openswmm_nodes.h
 * @brief     OpenSWMM Engine — Node C API.
 *
 * @details Node add (BUILDING state), geometry setters, state get/set,
 *          lateral inflow injection (RUNNING), bulk access, quality.
 *
 * @ingroup engine_api
 * @see openswmm_engine.h
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_NODES_H
#define OPENSWMM_NODES_H

#include "openswmm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Node type classification.
 *
 * @details Every node in a SWMM model belongs to one of these types. The type
 *          governs which property setters/getters are valid and how the node
 *          participates in routing.
 */
typedef enum SWMM_NodeType {
    SWMM_NODE_JUNCTION = 0, /**< Standard junction node. */
    SWMM_NODE_OUTFALL = 1,  /**< Outfall boundary node. */
    SWMM_NODE_STORAGE = 2,  /**< Storage unit (pond, tank, etc.). */
    SWMM_NODE_DIVIDER = 3   /**< Flow divider. */
} SWMM_NodeType;

/* =========================================================================
 * Identity
 * ========================================================================= */

/**
 * @brief Get the total number of nodes in the model.
 * @param[in] engine Engine handle.
 * @returns Number of nodes, or -1 on error.
 */
SWMM_ENGINE_API int swmm_node_count(SWMM_Engine engine);

/**
 * @brief Look up a node's zero-based index by its string identifier.
 * @param[in] engine Engine handle.
 * @param[in] id Null-terminated node identifier.
 * @returns Zero-based index, or -1 if not found.
 */
SWMM_ENGINE_API int swmm_node_index(SWMM_Engine engine, const char* id);

/**
 * @brief Get the string identifier of a node by index.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index.
 * @returns Null-terminated string owned by the engine, or NULL on error.
 *
 *
 * @note Borrowed ID storage may be invalidated by name/topology edits, close/reopen or engine
 *     destruction; copy it when retaining it across operations.
 */
SWMM_ENGINE_API const char* swmm_node_id(SWMM_Engine engine, int idx);

/** Cumulative committed 1D outfall transfers, independent of REPORT_START.
 * Positive exports have already left SWMM: consumers must not withdraw them
 * again. Reverse water is retained separately and is not a one-way export.
 * SI water is m3; heat is J relative to 0 degC using model rho/cp. Pollutant
 * arrays use kg for MG/L and UG/L, and counts for #/L. Read units first.
 * Flags: 1 external water, 2 LEGACY pollutant loads, 4 native heat.
 * Route-to-subcatchment and native 2D-owned outfalls are refused.
 * No MSX or ARD/LARD mass capability is implied by these receipts.
 */
typedef struct SWMM_OutfallExportReceipt {
    double exported_m3;
    double reversed_m3;
    double exported_heat_j;
    unsigned flags;
} SWMM_OutfallExportReceipt;
/**
 * @brief Read committed external 1D outfall exports without withdrawing water again.
 *
 * Requires an outfall with no route-to-subcatchment and no native surface-owned exchange.
 * Cumulative committed transfers are independent of REPORT_START; exported water already left SWMM.
 * Flags 1/2/4 indicate external water/LEGACY pollutant/native heat capability. No MSX or ARD/LARD
 * amount capability is implied.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] idx Zero-based node index in [0,swmm_node_count(engine)).
 * @param[out] receipt Non-null water/heat/availability record: exported and separately reversed
 *     water in m3, sensible heat in J relative to 0 degrees C using model rho/cp, and capability
 *     flags.
 * @param[out] pollutant_amounts Caller-owned array of count amounts in pollutant order; required
 *     only when count>0. MG/L and UG/L pollutants use kg; #/L uses counts.
 * @param[in] count Must equal the current model pollutant count exactly, including zero.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid node index.
 *     SWMM_ERR_BADPARAM refuses wrong outputs/count or ineligible node ownership;
 *     SWMM_ERR_LIFECYCLE indicates uninitialized receipt arrays.
 */
SWMM_ENGINE_API int swmm_node_get_export_receipt(SWMM_Engine engine, int idx,
                                                 SWMM_OutfallExportReceipt* receipt,
                                                 double* pollutant_amounts, int count);

/* =========================================================================
 * Creation (BUILDING or OPENED — "editable" states)
 * ========================================================================= */

/**
 * @brief Add a new node to the model.
 *
 * @details The engine must be in SWMM_STATE_BUILDING (programmatic
 *          construction) or SWMM_STATE_OPENED (interactive editing after
 *          the .inp has been parsed). Returns SWMM_ERR_LIFECYCLE for any
 *          other state — once the simulation has been initialized, started,
 *          or run, the node-count invariant is baked into solver state and
 *          the engine must be closed + re-opened to accept new objects.
 *
 *          The node is appended to the model's node list and its index
 *          equals the previous count.
 *
 * @param[in] engine Engine handle.
 * @param[in] id Unique null-terminated identifier for the new node.
 * @param[in] type Node type (see @ref SWMM_NodeType).
 * @returns SWMM_OK on success, SWMM_ERR_LIFECYCLE if not in an editable
 *          state, or another error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE.
 */
SWMM_ENGINE_API int swmm_node_add(SWMM_Engine engine, const char* id, int type);

/**
 * @brief Remove the most recently added node (undo-of-add).
 *
 * @details Pops the tail of the node list. The engine must be in
 *          SWMM_STATE_BUILDING or SWMM_STATE_OPENED. Returns
 *          SWMM_ERR_BADINDEX if the tail doesn't match \p id (guards
 *          against undo / redo order mismatches), SWMM_ERR_BADPARAM if
 *          any link references the tail node (the caller must cascade
 *          those removals first via @ref swmm_link_pop_last), or
 *          SWMM_ERR_LIFECYCLE for any other state.
 *
 *          This is an intentionally narrow surface that avoids
 *          renumbering any cross-references; for a general
 *          swmm_node_remove(idx) see the engine roadmap — full remove
 *          requires renumbering every link / subcatch / control / report
 *          reference and is tracked separately.
 *
 * @param[in] engine Engine handle.
 * @param[in] id Expected tail identifier (null-terminated).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE.
 */
SWMM_ENGINE_API int swmm_node_pop_last(SWMM_Engine engine, const char* id);

/* =========================================================================
 * Geometry setters (BUILDING or OPENED)
 * ========================================================================= */

/**
 * @brief Set a node's invert elevation.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index.
 * @param[in] elev Invert elevation in project length units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed node/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_node_set_invert_elev(SWMM_Engine engine, int idx, double elev);

/**
 * @brief Set a node's maximum depth (distance from invert to crown).
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index.
 * @param[in] depth Maximum depth in project length units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed node/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_node_set_max_depth(SWMM_Engine engine, int idx, double depth);

/**
 * @brief Set the allowed surcharge depth above the node's crown.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index.
 * @param[in] depth Surcharge depth in project length units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed node/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_node_set_surcharge_depth(SWMM_Engine engine, int idx, double depth);

/** Physical ordinary-junction chamber area in project area units (ft2 or m2). Zero retains legacy behavior.
 * Geometry edits only; nonfinite/negative values and virtual/inlet/nonjunction assignments fail. */
SWMM_ENGINE_API int swmm_node_set_storage_area(SWMM_Engine engine, int idx, double area);
/** Read authored junction storage area in project area units (ft2 or m2) (zero by default). */
SWMM_ENGINE_API int swmm_node_get_storage_area(SWMM_Engine engine, int idx, double* area);

/**
 * @brief Set the ponded surface area when depth exceeds the maximum.
 *
 * @details When ponding is modeled, excess water is stored on the surface
 *          using this area. Set to 0 to disable ponding at this node.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index.
 * @param[in] area Ponded area in project area units (e.g., ft² or m²).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed node/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_node_set_pond_area(SWMM_Engine engine, int idx, double area);

/**
 * @brief Set a node's initial water depth at simulation start.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index.
 * @param[in] depth Initial depth in project length units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Sets the authored initial condition, which swmm_node_get_initial_depth() reads, the .inp
 *     writer saves and initialization turns into the starting depth, head and volume. Requires
 *     BUILDING or OPENED: after initialization the starting state already exists, so other states
 *     return SWMM_ERR_LIFECYCLE (swmm_node_set_depth() changes a running depth). A null engine
 *     returns SWMM_ERR_BADHANDLE. A failed node index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_node_set_initial_depth(SWMM_Engine engine, int idx, double depth);

/**
 * @brief Set a node's rendering-only rim (ground) depth above the invert.
 *
 * @details RENDERING ONLY — no hydraulics, routing, reporting or output-file
 *          code reads this value, so setting it can never change a result.
 *          It exists for virtual junctions, whose max depth is derived (always
 *          the shared pipe crown): without it every viewer drawing a ground
 *          line collapses the surface to the crown at each break point. It is
 *          the optional third token of a [VIRTUAL_JUNCTIONS] row.
 *
 *          On other node types the value is simply carried; renderers use the
 *          real max depth there. Pass 0 to clear it ("unset" — renderers fall
 *          back to the max depth). Negative values are clamped to 0.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index.
 * @param[in] depth Rim depth above the invert in project length units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed node/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_node_set_rim_depth(SWMM_Engine engine, int idx, double depth);

/* =========================================================================
 * Geometry getters
 * ========================================================================= */

/**
 * @brief Get the type of a node.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index.
 * @param[out] type Receives the node type (see @ref SWMM_NodeType).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note type may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed node/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_node_get_type(SWMM_Engine engine, int idx, int* type);

/**
 * @brief Query whether a node is a virtual junction.
 *
 * @details A virtual junction is a zero-storage, momentum-transmitting
 *          JUNCTION-typed node connecting exactly two conduits of identical
 *          cross-section (INP section [VIRTUAL_JUNCTIONS]; refactored engine
 *          only).
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index.
 * @param[out] is_virtual Receives 1 if the node is a virtual junction, else 0.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note is_virtual may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed node/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_node_is_virtual(SWMM_Engine engine, int idx, int* is_virtual);

/**
 * @brief Set or clear a node's virtual-junction flag.
 *
 * @details Setting runs full validation of the usage rules and applies the
 *          derived-geometry contract (full depth = pipe crown, zero surcharge
 *          depth, zero ponded area). Clearing always succeeds for a virtual
 *          node. BUILDING or OPENED state only.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index (must be a JUNCTION to set).
 * @param[in] make_virtual 1 to set, 0 to clear.
 * @returns SWMM_OK on success; SWMM_ERR_LIFECYCLE / SWMM_ERR_BADHANDLE /
 *          SWMM_ERR_BADINDEX / SWMM_ERR_BADPARAM for generic failures; or a
 *          distinct rule code on a violated usage rule so callers can render
 *          actionable messages: 609 = not exactly two conduits, 611 =
 *          cross-section mismatch, 613 = nonzero offset at the node, 617 =
 *          the node is a 2D surface-coupling point. Point lateral inflows
 *          ([INFLOWS], [DWF], RDII, subcatchment outlets, LID drains, the
 *          runtime lateral-inflow API) are permitted at a virtual junction.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed node/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_node_set_virtual(SWMM_Engine engine, int idx, int make_virtual);

/**
 * @brief Dry-run check of the virtual-junction usage rules for a node.
 *
 * @details Read-only: evaluates the same structural rules that
 *          swmm_node_set_virtual() enforces (exactly two attached conduits of
 *          identical cross-section, zero offsets, no 2D surface coupling,
 *          dynamic-wave or FV routing) without changing any state and without
 *          requiring the node to currently be a JUNCTION — so callers can
 *          offer "convert to virtual junction" only when it would succeed.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index.
 * @param[out] rule_code Receives 0 when the node satisfies every rule, else
 *                       the distinct ERR_VJ_* rule code (see
 *                       swmm_node_set_virtual) identifying the violated rule.
 * @returns SWMM_OK on success, or an error code for a bad handle/index.
 *
 *
 * @note rule_code may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed node/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_node_virtual_eligible(SWMM_Engine engine, int idx, int* rule_code);

/* -------------------------------------------------------------------------
 * Inlet junctions (2026-09-05). An inlet junction is a virtual junction
 * (is_virtual = 1) that additionally carries is_inlet = 1, sits between two
 * STREET conduits (or RECT_OPEN/TRAPEZOIDAL for drop-inlet designs), owns one
 * inlet-usage row (swmm_inlet_usage_set with SWMM_INLET_HOST_NODE) naming the
 * inlet design and the capture node, and — unlike a plain virtual junction —
 * is NOT sealed: it floods above the street section's full depth (or the
 * [INLET_JUNCTIONS] MaxDepth when supplied) and receives the capture node's
 * overflow back as street flow.
 * ------------------------------------------------------------------------- */

/**
 * @brief Read whether a node carries the inlet-junction marker.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] idx Zero-based node index in [0,swmm_node_count(engine)).
 * @param[out] is_inlet Optional destination: 1 when marked inlet, otherwise 0.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid node index.
 */
SWMM_ENGINE_API int swmm_node_is_inlet(SWMM_Engine engine, int idx, int* is_inlet);

/**
 * @brief Evaluate inlet-junction topology rules without changing the node.
 *
 * A topology refusal is reported in rule_code while the API returns SWMM_OK; inspect the code
 * separately.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] idx Zero-based node index in [0,swmm_node_count(engine)).
 * @param[in] for_drop_inlet 0 evaluates the street-inlet profile; nonzero evaluates drop-inlet
 *     eligibility.
 * @param[out] rule_code Optional destination: 0 for eligible, otherwise the specific
 *     inlet/virtual-junction validation code.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid node index.
 */
SWMM_ENGINE_API int swmm_node_inlet_eligible(SWMM_Engine engine, int idx, int for_drop_inlet,
                                             int* rule_code);

/**
 * @brief Enable or disable inlet-junction authoring on an editable node.
 *
 * Validates through the inlet-junction edit helper before applying the requested marker. Treat a
 * nonzero returned rule code as a refused edit.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] idx Zero-based node index in [0,swmm_node_count(engine)).
 * @param[in] make_inlet 0 clears the marker; nonzero requests inlet conversion under current
 *     topology rules.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid node index.
 *     SWMM_ERR_LIFECYCLE outside BUILDING/OPENED; SWMM_ERR_BADPARAM for a non-junction target, or a
 *     specific inlet/virtual-junction rule code.
 */
SWMM_ENGINE_API int swmm_node_set_inlet(SWMM_Engine engine, int idx, int make_inlet);

/**
 * @brief Get a node's invert elevation.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index.
 * @param[out] elev Receives the invert elevation in project length units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note elev may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed node/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_node_get_invert_elev(SWMM_Engine engine, int idx, double* elev);

/**
 * @brief Get a node's maximum depth.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index.
 * @param[out] depth Receives the maximum depth in project length units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note depth may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed node/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_node_get_max_depth(SWMM_Engine engine, int idx, double* depth);

/**
 * @brief Get a node's rendering-only rim (ground) depth above the invert.
 *
 * @details See swmm_node_set_rim_depth(). 0 means unset: a renderer drawing a
 *          ground line should fall back to swmm_node_get_max_depth(), which
 *          for a virtual junction is the pipe crown.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index.
 * @param[out] depth Receives the rim depth in project length units, or 0.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note depth may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed node/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_node_get_rim_depth(SWMM_Engine engine, int idx, double* depth);

/* =========================================================================
 * Hydraulic state getters/setters
 * ========================================================================= */

/**
 * @brief Get the current water depth at a node.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index.
 * @param[out] depth Receives depth in project length units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note depth may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed node/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_node_get_depth(SWMM_Engine engine, int idx, double* depth);

/**
 * @brief Set the water depth at a node (runtime override).
 * @param[in] engine Engine handle (RUNNING state).
 * @param[in] idx Zero-based node index.
 * @param[in] depth New depth in project length units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires RUNNING; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed node/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_node_set_depth(SWMM_Engine engine, int idx, double depth);

/**
 * @brief Get the current hydraulic head at a node (invert + depth).
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index.
 * @param[out] head Receives the head in project length units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note head may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed node/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_node_get_head(SWMM_Engine engine, int idx, double* head);

/**
 * @brief Get the current stored water volume at a node.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index.
 * @param[out] volume Receives the volume in project volume units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note volume may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed node/pollutant index check returns SWMM_ERR_BADINDEX. Includes active storage-node LID
 *     held water in addition to the node water volume.
 */
SWMM_ENGINE_API int swmm_node_get_volume(SWMM_Engine engine, int idx, double* volume);

/**
 * @brief Get the current lateral inflow at a node.
 *
 * @details Lateral inflow is the externally applied flow (DWF, RDII, user
 *          inflows) as opposed to the total inflow which includes upstream
 *          link contributions.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index.
 * @param[out] inflow Receives the lateral inflow in project flow units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note inflow may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed node/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_node_get_lateral_inflow(SWMM_Engine engine, int idx, double* inflow);

/**
 * @brief Get the current overflow (flooding) rate at a node.
 *
 * @details Overflow occurs when the water depth exceeds the node's maximum
 *          depth and ponding is not enabled (or the ponded area is exceeded).
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index.
 * @param[out] overflow Receives the overflow rate in project flow units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note overflow may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed node/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_node_get_overflow(SWMM_Engine engine, int idx, double* overflow);

/**
 * @brief Read the current assembled lateral-inflow field.
 *
 * Despite the historical name, this wrapper reads lat_flow only; upstream link contributions are
 * not added. It is equivalent to swmm_node_get_lateral_inflow().
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] idx Zero-based node index in [0,swmm_node_count(engine)).
 * @param[out] inflow Optional destination in project display flow units.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid node index.
 */
SWMM_ENGINE_API int swmm_node_get_inflow(SWMM_Engine engine, int idx, double* inflow);

/* =========================================================================
 * Runtime forcing (RUNNING state only)
 * ========================================================================= */

/**
 * @brief Set a persistent runtime user lateral-inflow contribution.
 *
 * Stores user_lat_flow, not the current assembled lat_flow. Runtime forcing OVERRIDE/ADD can
 * overlay it; ordinary DWF/RDII/input contributions are assembled separately. The value persists
 * across steps and does not replace all authored inflows. No finite/range validation is applied.
 *
 * @param[in] engine Non-null RUNNING engine.
 * @param[in] idx Zero-based node index in [0,swmm_node_count(engine)).
 * @param[in] flow Signed project display flow, converted to internal flow and retained until
 *     changed; zero stops this contribution.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid node index.
 *     SWMM_ERR_LIFECYCLE outside RUNNING.
 */
SWMM_ENGINE_API int swmm_node_set_lateral_inflow(SWMM_Engine engine, int idx, double flow);

/**
 * @brief Set a persistent additive runtime pollutant amount-rate contribution.
 *
 * Persists until replaced, then contributes at routing steps. Negative extraction is subject to the
 * quality loader available-mass clamp; this setter itself does not validate a finite/range value.
 *
 * @param[in] engine Non-null RUNNING engine.
 * @param[in] node_idx Zero-based existing node index.
 * @param[in] pollutant_idx Zero-based existing pollutant index.
 * @param[in] mass_rate Raw internal concentration-unit*ft3/s amount rate; passed to the quality
 *     loader without a project-mass conversion. Zero stops the retained contribution.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside RUNNING or
 *     SWMM_ERR_BADINDEX for invalid node/pollutant.
 */
SWMM_ENGINE_API int swmm_node_set_quality_mass_flux(SWMM_Engine engine, int node_idx,
                                                    int pollutant_idx, double mass_rate);

/**
 * @brief Set an outfall fixed-head boundary retained in its subtype row.
 *
 * Sets the outfall type to FIXED and stores its stage parameter; it is not automatically reset
 * after one step. Does not check input finiteness or supply a per-boundary clear operation.
 *
 * @param[in] engine Non-null RUNNING engine.
 * @param[in] idx Zero-based node index in [0,swmm_node_count(engine)).
 * @param[in] head Datum head in project display length units, converted to internal length.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid node index.
 *     SWMM_ERR_LIFECYCLE outside RUNNING or SWMM_ERR_BADPARAM for a non-outfall.
 */
SWMM_ENGINE_API int swmm_node_set_head_boundary(SWMM_Engine engine, int idx, double head);

/* =========================================================================
 * Water quality
 * ========================================================================= */

/**
 * @brief Get the pollutant concentration at a node.
 * @param[in] engine Engine handle.
 * @param[in] node_idx Zero-based node index.
 * @param[in] pollutant_idx Zero-based pollutant index.
 * @param[out] conc Receives the concentration in pollutant units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note conc may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed node/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_node_get_quality(SWMM_Engine engine, int node_idx, int pollutant_idx,
                                          double* conc);

/* =========================================================================
 * Storage Node API
 * ========================================================================= */

/**
 * @brief Assign a storage curve (depth vs. area) to a storage node.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index (must be SWMM_NODE_STORAGE).
 * @param[in] curve_idx Zero-based curve index (from swmm_curve_add()).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed node/pollutant index check returns SWMM_ERR_BADINDEX. For an
 *     existing node with no matching subtype row, the subtype assignment is a successful no-op.
 */
SWMM_ENGINE_API int swmm_node_set_storage_curve(SWMM_Engine engine, int idx, int curve_idx);

/**
 * @brief Get the storage curve index assigned to a storage node.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index.
 * @param[out] curve_idx Receives the curve index, or -1 if functional.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note curve_idx may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed node/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_node_get_storage_curve(SWMM_Engine engine, int idx, int* curve_idx);

/**
 * @brief Set functional storage parameters: Area = a * Depth^b + c.
 *
 * @details Defines the depth–area relationship using a power-law equation
 *          instead of a tabular curve.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index (must be SWMM_NODE_STORAGE).
 * @param[in] a Coefficient a.
 * @param[in] b Exponent b.
 * @param[in] c Constant c.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed node/pollutant index check returns SWMM_ERR_BADINDEX. For an
 *     existing node with no matching subtype row, the subtype assignment is a successful no-op.
 */
SWMM_ENGINE_API int swmm_node_set_storage_functional(SWMM_Engine engine, int idx, double a,
                                                     double b, double c);

/**
 * @brief Get functional storage parameters.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index.
 * @param[out] a Receives coefficient a.
 * @param[out] b Receives exponent b.
 * @param[out] c Receives constant c.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note a may be NULL to suppress that output. b may be NULL to suppress that output. c may be NULL
 *     to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A failed node/pollutant
 *     index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_node_get_storage_functional(SWMM_Engine engine, int idx, double* a,
                                                     double* b, double* c);

/**
 * @brief Storage-unit surface-area relation codes.
 *
 * @details Ordinals match the legacy solver's `enum StorageType`, so the same int is
 *          valid on both engines. The four geometric shapes take three raw dimensions
 *          (see swmm_node_set_storage_geometry); TABULAR takes a curve; FUNCTIONAL
 *          takes the a/b/c power-law coefficients.
 */
typedef enum {
    SWMM_STORAGE_TABULAR = 0,     /**< Area vs. depth from a curve. */
    SWMM_STORAGE_FUNCTIONAL = 1,  /**< Area = c + a*d^b. */
    SWMM_STORAGE_CYLINDRICAL = 2, /**< Elliptical cylinder: p1 = major axis, p2 = minor axis. */
    SWMM_STORAGE_CONICAL = 3,     /**< Elliptical cone: p1, p2 = base axes, p3 = side slope. */
    SWMM_STORAGE_PARABOLOID =
        4, /**< Elliptical paraboloid: p1, p2 = top axes, p3 = height (≠ 0). */
    SWMM_STORAGE_PYRAMIDAL =
        5 /**< Rectangular pyramid: p1 = length, p2 = width, p3 = side slope. */
} SWMM_StorageShape;

/**
 * @brief Set a storage node's surface-area relation.
 *
 * @details Setting a geometric shape detaches any storage curve and re-derives the
 *          internal area coefficients from the node's current raw dimensions; follow
 *          with swmm_node_set_storage_geometry() to supply them. Setting
 *          SWMM_STORAGE_FUNCTIONAL also detaches the curve. Use
 *          swmm_node_set_storage_curve() to make a node TABULAR.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index (must be SWMM_NODE_STORAGE).
 * @param[in] shape A SWMM_StorageShape code.
 * @returns SWMM_OK on success, SWMM_ERR_BADPARAM if @p shape is not a valid code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed node/pollutant index check returns SWMM_ERR_BADINDEX. For an
 *     existing node with no matching subtype row, the subtype assignment is a successful no-op.
 */
SWMM_ENGINE_API int swmm_node_set_storage_shape(SWMM_Engine engine, int idx, int shape);

/**
 * @brief Get a storage node's surface-area relation.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index.
 * @param[out] shape Receives a SWMM_StorageShape code.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note shape may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed node/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_node_get_storage_shape(SWMM_Engine engine, int idx, int* shape);

/**
 * @brief Set the raw dimensions of a geometric storage shape.
 *
 * @details Meaning of p1/p2/p3 depends on the node's current shape — see
 *          SWMM_StorageShape. The engine stores the raw dimensions (so they survive a
 *          write back to .inp) and derives the area coefficients from them. The call
 *          is atomic: if the dimensions are invalid nothing is written.
 *
 *          Validity (mirrors the legacy solver): p1 > 0, p2 > 0, p3 >= 0, and p3 != 0
 *          for SWMM_STORAGE_PARABOLOID.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index (must be SWMM_NODE_STORAGE).
 * @param[in] p1 Major axis / base length.
 * @param[in] p2 Minor axis / base width.
 * @param[in] p3 Side slope (run/rise), or height for SWMM_STORAGE_PARABOLOID.
 * @returns SWMM_OK on success; SWMM_ERR_BADPARAM if the node's shape is not geometric
 *          or the dimensions are invalid.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed node/pollutant index check returns SWMM_ERR_BADINDEX. For an
 *     existing node with no matching subtype row, the subtype assignment is a successful no-op.
 */
SWMM_ENGINE_API int swmm_node_set_storage_geometry(SWMM_Engine engine, int idx, double p1,
                                                   double p2, double p3);

/**
 * @brief Get the raw dimensions of a geometric storage shape.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index.
 * @param[out] p1 Receives the major axis / base length.
 * @param[out] p2 Receives the minor axis / base width.
 * @param[out] p3 Receives the side slope / height.
 * @returns SWMM_OK on success, or an error code. Returns zeros for a non-geometric shape.
 *
 *
 * @note p1 may be NULL to suppress that output. p2 may be NULL to suppress that output. p3 may be
 *     NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A failed
 *     node/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_node_get_storage_geometry(SWMM_Engine engine, int idx, double* p1,
                                                   double* p2, double* p3);

/**
 * @brief Set a constant seepage (exfiltration) rate for a storage node.
 * @details The constant-rate form of storage exfiltration, as a [STORAGE] row
 *          ending in a bare Ksat: sets the exfiltration Ksat to @p rate with
 *          zero suction and IMD, replacing any Green-Ampt parameters (see
 *          swmm_node_set_exfil_params). A rate of 0 turns exfiltration off.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index (must be SWMM_NODE_STORAGE).
 * @param[in] rate Seepage rate, in/hr (US) or mm/hr (SI); must be >= 0.
 * @returns SWMM_OK on success; SWMM_ERR_BADPARAM on a negative rate.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed node/pollutant index check returns SWMM_ERR_BADINDEX. For an
 *     existing node with no matching subtype row, the subtype assignment is a successful no-op.
 */
SWMM_ENGINE_API int swmm_node_set_storage_seep_rate(SWMM_Engine engine, int idx, double rate);

/**
 * @brief Get the constant seepage (exfiltration) rate of a storage node.
 * @details The exfiltration Ksat when exfiltration uses its constant-rate form
 *          (zero suction and IMD); 0 when it uses Green-Ampt parameters or is
 *          off, and for a node that is not storage.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index.
 * @param[out] rate Receives the rate, in/hr (US) or mm/hr (SI).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note rate may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed node/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_node_get_storage_seep_rate(SWMM_Engine engine, int idx, double* rate);

/**
 * @brief Set Green–Ampt exfiltration parameters for a storage node.
 *
 * @details Exfiltration models soil infiltration losses from the bottom and
 *          banks of a storage unit using the Green–Ampt method.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index (must be SWMM_NODE_STORAGE).
 * @param[in] suction Soil capillary suction head (project length units).
 * @param[in] ksat Saturated hydraulic conductivity (project length/time).
 * @param[in] imd Initial moisture deficit (fraction: 0–1).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed node/pollutant index check returns SWMM_ERR_BADINDEX. For an
 *     existing node with no matching subtype row, the subtype assignment is a successful no-op.
 */
SWMM_ENGINE_API int swmm_node_set_exfil_params(SWMM_Engine engine, int idx, double suction,
                                               double ksat, double imd);

/**
 * @brief Get Green–Ampt exfiltration parameters for a storage node.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index.
 * @param[out] suction Receives the suction head.
 * @param[out] ksat Receives the saturated hydraulic conductivity.
 * @param[out] imd Receives the initial moisture deficit.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note suction may be NULL to suppress that output. ksat may be NULL to suppress that output. imd
 *     may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A failed
 *     node/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_node_get_exfil_params(SWMM_Engine engine, int idx, double* suction,
                                               double* ksat, double* imd);

/* =========================================================================
 * Outfall Node API
 * ========================================================================= */

/**
 * @brief Set the outfall boundary condition type.
 *
 * @details Common types: 0=FREE, 1=NORMAL, 2=FIXED, 3=TIDAL, 4=TIMESERIES.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index (must be SWMM_NODE_OUTFALL).
 * @param[in] type Outfall type code.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed node/pollutant index check returns SWMM_ERR_BADINDEX. For an
 *     existing node with no matching subtype row, the subtype assignment is a successful no-op.
 */
SWMM_ENGINE_API int swmm_node_set_outfall_type(SWMM_Engine engine, int idx, int type);

/**
 * @brief Get the outfall boundary condition type.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index.
 * @param[out] type Receives the outfall type code.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note type may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed node/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_node_get_outfall_type(SWMM_Engine engine, int idx, int* type);

/**
 * @brief Set a fixed outfall stage (for FIXED type outfalls).
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index (must be SWMM_NODE_OUTFALL).
 * @param[in] stage Fixed stage elevation in project length units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed node/pollutant index check returns SWMM_ERR_BADINDEX. For an
 *     existing node with no matching subtype row, the subtype assignment is a successful no-op.
 */
SWMM_ENGINE_API int swmm_node_set_outfall_stage(SWMM_Engine engine, int idx, double stage);

/**
 * @brief Assign a tidal curve to an outfall (for TIDAL type outfalls).
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index (must be SWMM_NODE_OUTFALL).
 * @param[in] curve_idx Zero-based curve index defining hour-of-day vs. stage.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed node/pollutant index check returns SWMM_ERR_BADINDEX. For an
 *     existing node with no matching subtype row, the subtype assignment is a successful no-op.
 */
SWMM_ENGINE_API int swmm_node_set_outfall_tidal(SWMM_Engine engine, int idx, int curve_idx);

/**
 * @brief Assign a time series to an outfall (for TIMESERIES type outfalls).
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index (must be SWMM_NODE_OUTFALL).
 * @param[in] ts_idx Zero-based time series index defining time vs. stage.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed node/pollutant index check returns SWMM_ERR_BADINDEX. For an
 *     existing node with no matching subtype row, the subtype assignment is a successful no-op.
 */
SWMM_ENGINE_API int swmm_node_set_outfall_timeseries(SWMM_Engine engine, int idx, int ts_idx);

/**
 * @brief Get the outfall stage parameter (fixed stage, or current computed stage).
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index.
 * @param[out] param Receives the outfall parameter value.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note param may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed node/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_node_get_outfall_param(SWMM_Engine engine, int idx, double* param);

/**
 * @brief Get the tidal curve index assigned to a TIDAL outfall.
 *
 * @details The outfall parameter slot is union-typed across stage / tidal-idx /
 *          ts-idx; this accessor returns the slot interpreted as a curve index
 *          only when the outfall is currently of TIDAL type. Returns
 *          @ref SWMM_ERR_BADPARAM if the outfall type is not TIDAL, so the
 *          caller can distinguish "unassigned" from a genuine index of 0.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index.
 * @param[out] curve_idx Receives the zero-based curve index.
 * @returns SWMM_OK on success; SWMM_ERR_BADPARAM if outfall type != TIDAL.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed node/pollutant index check returns
 *     SWMM_ERR_BADINDEX. curve_idx must be non-null and the outfall type must be TIDAL; otherwise
 *     returns SWMM_ERR_BADPARAM.
 */
SWMM_ENGINE_API int swmm_node_get_outfall_tidal(SWMM_Engine engine, int idx, int* curve_idx);

/**
 * @brief Get the time-series index assigned to a TIMESERIES outfall.
 *
 * @details Symmetric to @ref swmm_node_get_outfall_tidal. Returns
 *          @ref SWMM_ERR_BADPARAM unless the outfall is currently of
 *          TIMESERIES type.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index.
 * @param[out] ts_idx Receives the zero-based time-series index.
 * @returns SWMM_OK on success; SWMM_ERR_BADPARAM if outfall type != TIMESERIES.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed node/pollutant index check returns
 *     SWMM_ERR_BADINDEX. ts_idx must be non-null and the outfall type must be TIMESERIES; otherwise
 *     returns SWMM_ERR_BADPARAM.
 */
SWMM_ENGINE_API int swmm_node_get_outfall_timeseries(SWMM_Engine engine, int idx, int* ts_idx);

/**
 * @brief Set whether a flap gate exists at the outfall.
 *
 * @details A flap gate prevents reverse flow through the outfall.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index (must be SWMM_NODE_OUTFALL).
 * @param[in] has_gate Non-zero to enable flap gate; zero to disable.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed node/pollutant index check returns SWMM_ERR_BADINDEX. For an
 *     existing node with no matching subtype row, the subtype assignment is a successful no-op.
 */
SWMM_ENGINE_API int swmm_node_set_outfall_flap_gate(SWMM_Engine engine, int idx, int has_gate);

/**
 * @brief Get whether a flap gate exists at the outfall.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index.
 * @param[out] has_gate Receives 1 if flap gate present, 0 otherwise.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note has_gate may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed node/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_node_get_outfall_flap_gate(SWMM_Engine engine, int idx, int* has_gate);

/* =========================================================================
 * Divider Node API
 * ========================================================================= */

/**
 * @brief Flow divider method.
 *
 * @details Selects how a divider node splits inflow between its diversion
 *          link and its main outflow link. The value is stored in the
 *          per-node divider_type SoA array and only consulted when
 *          type[i] == @ref SWMM_NODE_DIVIDER.
 */
typedef enum SWMM_DividerType {
    SWMM_DIVIDER_CUTOFF = 0,   /**< Flow above cutoff is diverted. */
    SWMM_DIVIDER_OVERFLOW = 1, /**< Diverted flow = max link capacity exceedance. */
    SWMM_DIVIDER_TABULAR = 2,  /**< Diverted flow looked up on a curve. */
    SWMM_DIVIDER_WEIR = 3      /**< Weir equation governs diversion. */
} SWMM_DividerType;

/**
 * @brief Set the divider method for a flow-divider node.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index (must be SWMM_NODE_DIVIDER).
 * @param[in] type Divider type code (see @ref SWMM_DividerType).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed node/pollutant index check returns SWMM_ERR_BADINDEX. For an
 *     existing node with no matching subtype row, the subtype assignment is a successful no-op.
 */
SWMM_ENGINE_API int swmm_node_set_divider_type(SWMM_Engine engine, int idx, int type);

/**
 * @brief Get the divider method for a flow-divider node.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index.
 * @param[out] type Receives the divider type code.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note type may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed node/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_node_get_divider_type(SWMM_Engine engine, int idx, int* type);

/* =========================================================================
 * Additional Geometry / State Getters
 * ========================================================================= */

/**
 * @brief Get the surcharge depth above the node's crown.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index.
 * @param[out] depth Receives the surcharge depth in project length units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note depth may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed node/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_node_get_surcharge_depth(SWMM_Engine engine, int idx, double* depth);

/**
 * @brief Get the ponded area at a node.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index.
 * @param[out] area Receives the ponded area in project area units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note area may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed node/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_node_get_ponded_area(SWMM_Engine engine, int idx, double* area);

/**
 * @brief Get the initial depth at a node.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index.
 * @param[out] depth Receives the initial depth in project length units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note depth may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed node/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_node_get_initial_depth(SWMM_Engine engine, int idx, double* depth);

/**
 * @brief Get the crown elevation (invert + max depth) at a node.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index.
 * @param[out] elev Receives the crown elevation in project length units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note elev may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed node/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_node_get_crown_elev(SWMM_Engine engine, int idx, double* elev);

/**
 * @brief Get the full (maximum) stored volume at a node.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index.
 * @param[out] vol Receives the full volume in project volume units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note vol may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A failed
 *     node/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_node_get_full_volume(SWMM_Engine engine, int idx, double* vol);

/**
 * @brief Get the cumulative water losses at a node (evaporation + exfiltration).
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index.
 * @param[out] losses Receives the loss volume in project volume units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note losses may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed node/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_node_get_losses(SWMM_Engine engine, int idx, double* losses);

/**
 * @brief Get the total outflow from a node through downstream links.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index.
 * @param[out] outflow Receives the outflow in project flow units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note outflow may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed node/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_node_get_outflow(SWMM_Engine engine, int idx, double* outflow);

/**
 * @brief Get the degree (number of connected links) of a node.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index.
 * @param[out] degree Receives the number of links connected to this node.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note degree may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed node/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_node_get_degree(SWMM_Engine engine, int idx, int* degree);

/* =========================================================================
 * Node Statistics
 * ========================================================================= */

/**
 * @brief Get the maximum depth recorded at a node during the simulation.
 * @param[in] engine Engine handle (ENDED or RUNNING state).
 * @param[in] idx Zero-based node index.
 * @param[out] val Receives the maximum depth in project length units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note val may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A failed
 *     node/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_node_get_stat_max_depth(SWMM_Engine engine, int idx, double* val);

/**
 * @brief Get the maximum overflow rate recorded at a node.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index.
 * @param[out] val Receives the maximum overflow in project flow units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note val may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A failed
 *     node/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_node_get_stat_max_overflow(SWMM_Engine engine, int idx, double* val);

/**
 * @brief Get the total flood volume at a node over the simulation.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index.
 * @param[out] val Receives the flooded volume in project volume units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note val may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A failed
 *     node/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_node_get_stat_vol_flooded(SWMM_Engine engine, int idx, double* val);

/**
 * @brief Get the total time a node was flooded during the simulation.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index.
 * @param[out] val Receives the flooded duration in seconds.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note val may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A failed
 *     node/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_node_get_stat_time_flooded(SWMM_Engine engine, int idx, double* val);

/* =========================================================================
 * Bulk access
 * ========================================================================= */

/**
 * @brief Get depths for all nodes in a single call.
 * @param[in] engine Engine handle.
 * @param[out] buf Caller-allocated buffer of at least @p count doubles.
 * @param[in] count Number of elements (should equal swmm_node_count()).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A null buffer or nonpositive count returns
 *     SWMM_ERR_BADPARAM. Only min(count,node count) entries are copied; any buffer tail is
 *     unchanged.
 */
SWMM_ENGINE_API int swmm_node_get_depths_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Get hydraulic heads for all nodes in a single call.
 * @param[in] engine Engine handle.
 * @param[out] buf Caller-allocated buffer of at least @p count doubles.
 * @param[in] count Number of elements.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A null buffer or nonpositive count returns
 *     SWMM_ERR_BADPARAM. Only min(count,node count) entries are copied; any buffer tail is
 *     unchanged.
 */
SWMM_ENGINE_API int swmm_node_get_heads_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Copy a prefix of the assembled lateral-inflow field.
 *
 * Reads lat_flow only, without adding upstream link contributions; equivalent to
 * swmm_node_get_lateral_inflows_bulk().
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[out] buf Non-null caller-owned array of count doubles in project display flow units.
 * @param[in] count Positive capacity/requested prefix length; writes min(count,node count) values
 *     and leaves any remaining entries unchanged.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for null buf/count<=0.
 */
SWMM_ENGINE_API int swmm_node_get_inflows_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Get overflow rates for all nodes in a single call.
 * @param[in] engine Engine handle.
 * @param[out] buf Caller-allocated buffer of at least @p count doubles.
 * @param[in] count Number of elements.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A null buffer or nonpositive count returns
 *     SWMM_ERR_BADPARAM. Only min(count,node count) entries are copied; any buffer tail is
 *     unchanged.
 */
SWMM_ENGINE_API int swmm_node_get_overflows_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Set depths for all nodes in a single call (runtime override).
 * @param[in] engine Engine handle (RUNNING state).
 * @param[in] buf Array of depth values, one per node.
 * @param[in] count Number of elements (should equal swmm_node_count()).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires RUNNING; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A null buffer or nonpositive count returns SWMM_ERR_BADPARAM. Only
 *     min(count,node count) entries are copied; any buffer tail is unchanged.
 */
SWMM_ENGINE_API int swmm_node_set_depths_bulk(SWMM_Engine engine, const double* buf, int count);

/**
 * @brief Overwrite a prefix of the current assembled lateral-inflow field.
 *
 * Writes lat_flow directly, unlike the persistent user_lat_flow scalar setter. Subsequent inflow
 * assembly may replace these values. Does not check finite values or promise a persistent forcing
 * prescription.
 *
 * @param[in] engine Non-null RUNNING engine.
 * @param[in] buf Non-null borrowed array of count project display flow values, converted and copied
 *     synchronously.
 * @param[in] count Positive requested prefix length; uses min(count,node count).
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside RUNNING or
 *     SWMM_ERR_BADPARAM for null buf/count<=0.
 */
SWMM_ENGINE_API int swmm_node_set_lat_inflows_bulk(SWMM_Engine engine, const double* buf,
                                                   int count);

/**
 * @brief Get pollutant concentrations at all nodes for one pollutant.
 * @param[in] engine Engine handle.
 * @param[in] pollutant_idx Zero-based pollutant index.
 * @param[out] buf Caller-allocated buffer of at least @p count doubles.
 * @param[in] count Number of elements (should equal swmm_node_count()).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed node/pollutant index check returns
 *     SWMM_ERR_BADINDEX. A null buffer or nonpositive count returns SWMM_ERR_BADPARAM. Only
 *     min(count,node count) entries are copied; any buffer tail is unchanged.
 */
SWMM_ENGINE_API int swmm_node_get_quality_bulk(SWMM_Engine engine, int pollutant_idx, double* buf,
                                               int count);

/**
 * @brief Get current stored volumes for all nodes in a single call.
 *
 * @details Single-pass bulk variant of @ref swmm_node_get_volume — avoids
 *          @c N round-trips through the C ABI for whole-network reads.
 *          Used by the MCP server's per-node info builders and the
 *          `mass_balance` resource.
 *
 * @param[in] engine Engine handle.
 * @param[out] buf Caller-allocated buffer of at least @p count doubles.
 * @param[in] count Number of elements. If smaller than @c swmm_node_count()
 *                   only the first @c min(count, n_nodes) entries are written.
 * @returns @c SWMM_OK on success; @c SWMM_ERR_BADHANDLE if @p engine is
 *          invalid; @c SWMM_ERR_BADPARAM if @p buf is NULL or @p count <= 0.
 * @since 6.0.0
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A null buffer or nonpositive count returns
 *     SWMM_ERR_BADPARAM. Only min(count,node count) entries are copied; any buffer tail is
 *     unchanged. Includes active storage-node LID held water in addition to the node water volume.
 */
SWMM_ENGINE_API int swmm_node_get_volumes_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Get current outflows for all nodes in a single call.
 *
 * @details Single-pass bulk variant of @ref swmm_node_get_outflow.
 *
 * @param[in] engine Engine handle.
 * @param[out] buf Caller-allocated buffer of at least @p count doubles.
 * @param[in] count Number of elements.
 * @returns @c SWMM_OK on success, or an error code (see @ref swmm_node_get_volumes_bulk).
 * @since 6.0.0
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A null buffer or nonpositive count returns
 *     SWMM_ERR_BADPARAM. Only min(count,node count) entries are copied; any buffer tail is
 *     unchanged.
 */
SWMM_ENGINE_API int swmm_node_get_outflows_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Get accumulated node losses (exfil + evap) for all nodes in one call.
 *
 * @details Single-pass bulk variant of @ref swmm_node_get_losses.
 *
 * @param[in] engine Engine handle.
 * @param[out] buf Caller-allocated buffer of at least @p count doubles.
 * @param[in] count Number of elements.
 * @returns @c SWMM_OK on success, or an error code.
 * @since 6.0.0
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A null buffer or nonpositive count returns
 *     SWMM_ERR_BADPARAM. Only min(count,node count) entries are copied; any buffer tail is
 *     unchanged.
 */
SWMM_ENGINE_API int swmm_node_get_losses_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Get current lateral inflows for all nodes in a single call.
 *
 * @details Single-pass bulk variant of @ref swmm_node_get_lateral_inflow.
 *          The matching setter is @ref swmm_node_set_lat_inflows_bulk.
 *
 * @param[in] engine Engine handle.
 * @param[out] buf Caller-allocated buffer of at least @p count doubles.
 * @param[in] count Number of elements.
 * @returns @c SWMM_OK on success, or an error code.
 * @since 6.0.0
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A null buffer or nonpositive count returns
 *     SWMM_ERR_BADPARAM. Only min(count,node count) entries are copied; any buffer tail is
 *     unchanged.
 */
SWMM_ENGINE_API int swmm_node_get_lateral_inflows_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Get node IDs for all nodes in a single call (stride-packed UTF-8).
 *
 * @details Each ID is written into a fixed-size slot @c buf[i*stride .. i*stride+stride-1].
 *          The ID is NUL-terminated within its slot; if the ID is longer than
 *          @c stride-1 bytes it is truncated and still NUL-terminated. The
 *          caller can recover each ID via @c strlen(buf + i*stride) (or
 *          equivalent UTF-8-safe slicing).
 *
 *          This is the Phase 3 alternative to looping @ref swmm_node_id @c N
 *          times through the C ABI. A typical stride for SWMM node IDs is
 *          32–64 bytes (SWMM IDs are limited to MAX_ID_CHARS = 31 in legacy);
 *          callers should choose a stride that comfortably accommodates the
 *          longest ID in their model.
 *
 * @param[in] engine Engine handle.
 * @param[out] buf Caller-allocated buffer of @c stride*count bytes.
 * @param[in] stride Per-ID slot size in bytes (must be > 1 to allow at least
 *                 one character plus the NUL).
 * @param[in] count Number of IDs to read.
 * @returns @c SWMM_OK on success; @c SWMM_ERR_BADHANDLE if @p engine is
 *          invalid; @c SWMM_ERR_BADPARAM if @p buf is NULL, @p stride < 2,
 *          or @p count <= 0.
 *
 * @par Example
 * @code{.c}
 *   int n = swmm_node_count(eng);
 *   int stride = 64;
 *   char* buf = calloc(n, stride);
 *   swmm_node_get_ids_bulk(eng, buf, stride, n);
 *   for (int i = 0; i < n; ++i) {
 *       const char* id = buf + i * stride;
 *       printf("node %d: %s\n", i, id);
 *   }
 * @endcode
 *
 * @see swmm_node_id
 * @since 6.0.0
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A null buffer or nonpositive count returns
 *     SWMM_ERR_BADPARAM. Only min(count,node count) entries are copied; any buffer tail is
 *     unchanged.
 */
SWMM_ENGINE_API int swmm_node_get_ids_bulk(SWMM_Engine engine, char* buf, int stride, int count);

/* =========================================================================
 * Outfall-to-subcatchment routing
 * ========================================================================= */

/**
 * @brief Set an outfall route-to subcatchment index directly.
 *
 * No lifecycle guard is imposed. For a valid node without an outfall subtype this is a successful
 * no-op; it does not convert the node into an outfall.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] idx Zero-based node index in [0,swmm_node_count(engine)).
 * @param[in] subcatch_idx Subcatchment index to store, or -1 for no routing. This wrapper does not
 *     validate the supplied subcatchment index.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid node index.
 */
SWMM_ENGINE_API int swmm_node_set_outfall_route_to(SWMM_Engine engine, int idx, int subcatch_idx);

/**
 * @brief Read the outfall route-to subcatchment index.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] idx Zero-based node index in [0,swmm_node_count(engine)).
 * @param[out] subcatch_idx Optional destination: stored subcatchment index, or -1 when
 *     unset/non-outfall.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid node index.
 */
SWMM_ENGINE_API int swmm_node_get_outfall_route_to(SWMM_Engine engine, int idx, int* subcatch_idx);

/* =========================================================================
 * Depth from volume (inverse of getVolume)
 * ========================================================================= */

/**
 * @brief Evaluate node depth from a supplied display-unit water volume.
 *
 * Uses the configured node subtype, storage coefficients/curve and project unit system. Does not
 * change stored volume/depth or validate input finiteness in this wrapper.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] idx Zero-based node index in [0,swmm_node_count(engine)).
 * @param[in] volume Water volume in project display volume units; converted internally before
 *     evaluating storage geometry.
 * @param[out] depth Optional destination for depth in project display length units; NULL skips the
 *     calculation.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid node index.
 */
SWMM_ENGINE_API int swmm_node_get_depth_from_volume(SWMM_Engine engine, int idx, double volume,
                                                    double* depth);

/**
 * @brief Rename a node in the editable name index.
 *
 * Keeps numeric node indices. Borrowed ID pointers can be invalidated; the wrapper changes the name
 * index and does not itself rewrite arbitrary stored text references.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] idx Zero-based node index in [0,swmm_node_count(engine)).
 * @param[in] newId Non-null, nonempty NUL-terminated new ID; copied internally and required to be
 *     unique in the node index.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid node index.
 *     SWMM_ERR_LIFECYCLE outside BUILDING/OPENED or SWMM_ERR_BADPARAM for invalid/duplicate ID.
 */
SWMM_ENGINE_API int swmm_node_rename(SWMM_Engine engine, int idx, const char* newId);

/* =========================================================================
 * Tag — free-form string label from the INP `[TAGS]` section
 * ========================================================================= */

/**
 * @brief Copy a node free-form tag.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] idx Zero-based node index in [0,swmm_node_count(engine)).
 * @param[out] buf Non-null caller-owned text buffer; empty when no tag exists, NUL-terminated and
 *     silently truncated to buflen-1 bytes.
 * @param[in] buflen Positive buffer capacity in bytes including NUL.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid node index.
 *     SWMM_ERR_BADPARAM for null buf/nonpositive capacity.
 */
SWMM_ENGINE_API int swmm_node_get_tag(SWMM_Engine engine, int idx, char* buf, int buflen);

/**
 * @brief Copy or clear a node free-form tag without changing topology.
 *
 * No lifecycle guard is imposed. Tags are keyed by node index and persist through node rename.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] idx Zero-based node index in [0,swmm_node_count(engine)).
 * @param[in] tag Borrowed NUL-terminated tag to copy; NULL/empty clears it.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid node index.
 */
SWMM_ENGINE_API int swmm_node_set_tag(SWMM_Engine engine, int idx, const char* tag);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_NODES_H */
