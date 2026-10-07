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
 * @file VirtualJunctionOps.hpp
 * @brief Internal C++ API for virtual-junction editing: rule validation,
 *        derived geometry, set/clear flag, conduit split and pair re-fusion.
 *
 * @details Virtual junctions are zero-storage, momentum-transmitting
 *          JUNCTION-typed nodes connecting exactly two conduits of identical
 *          cross-section (see plans/VIRTUAL_JUNCTION_IMPLEMENTATION_PLAN.md).
 *          This module is the single source of truth for the usage rules —
 *          PostParseResolver (batch load-time validation) and the C API
 *          (swmm_node_set_virtual / swmm_conduit_split /
 *          swmm_virtual_junction_fuse) both call into it, so GUI, CLI,
 *          Python and MCP share identical semantics. Refactored engine only.
 *
 * @ingroup engine_edit
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_VIRTUAL_JUNCTION_OPS_HPP
#define OPENSWMM_ENGINE_VIRTUAL_JUNCTION_OPS_HPP

#include "../core/SimulationContext.hpp"
#include <string>

namespace openswmm::edit {

/**
 * @brief Check the virtual-junction usage rules for a node.
 *
 * @details Rules (plan §6): exactly two attached links, both conduits
 *          (ERR_VJ_LINK_COUNT); identical cross-section — shape, geom1..4,
 *          curve/transect, barrels; roughness may differ
 *          (ERR_VJ_XSECT_MISMATCH); zero offsets at the node (ERR_VJ_OFFSET);
 *          the node is not a 2D surface-coupling point
 *          (ERR_VJ_LATERAL_INFLOW). Point lateral sources — [INFLOWS],
 *          [DWF], RDII, subcatchment outlets, LID drains, the runtime API —
 *          are permitted. The routing-model rule is model-level and checked
 *          by PostParseResolver, not here.
 *
 * @returns 0 when all rules pass, else the ERR_VJ_* code of the first
 *          violated rule (ErrorCodes.hpp).
 */
int vj_rule_violation(const SimulationContext& ctx, int node_idx);

/**
 * @brief Apply the derived-geometry contract to a validated virtual junction:
 *        full depth = pipe crown (xsect y_full), zero surcharge depth,
 *        zero ponded area. The invert and the rendering-only rim depth are
 *        user/split-supplied data and are left alone.
 */
void vj_apply_derived_geometry(SimulationContext& ctx, int node_idx);

/**
 * @brief Clear a node's virtual flag, promoting its rendering rim depth (if
 *        any) back to the real full depth.
 *
 * @details The single exit from virtual: shared by swmm_node_set_virtual(0)
 *          and the node type converter, so `J(4 ft) → VJ → J` returns 4 ft
 *          instead of keeping the pipe crown. A no-op on a non-virtual node.
 *          An inlet junction is a virtual junction too, so this also clears
 *          `is_inlet` and drops the node's inlet-usage row — nothing may hold
 *          a node-hosted usage once the node is a plain junction again.
 */
void vj_clear_virtual(SimulationContext& ctx, int node_idx);

/**
 * @brief Set or clear a node's virtual flag.
 *
 * @details Setting runs the full rule check first and applies the derived
 *          geometry on success; the node must be JUNCTION-typed. A max depth
 *          taller than the pipe crown is carried over as the node's rendering
 *          rim depth so the drawn ground surface survives the conversion.
 *          Clearing (vj_clear_virtual) always succeeds for a virtual node.
 *
 * @returns 0 on success, ERR_VJ_* on a violated rule, or -1 for a bad
 *          index / non-junction node.
 */
int vj_set_virtual(SimulationContext& ctx, int node_idx, bool make_virtual);

/** @brief Result of vj_split_conduit. */
struct SplitResult {
    int new_node_idx = -1;
    int new_link_idx = -1;
    int err = 0;   ///< 0 ok; ERR_VJ_* or -1 (bad param / duplicate name)
};

/**
 * @brief Split a conduit at normalized position t along its vertex-aware
 *        polyline, inserting a new node (virtual junction when
 *        @p make_virtual) and a new downstream conduit.
 *
 * @details The original conduit keeps its name and upstream end and is
 *          shortened to t·L; the new conduit carries the remaining
 *          (1−t)·L, copies the cross-section, roughness, barrels and
 *          related properties, and takes over the downstream offset. The
 *          break-point invert is interpolated along the conduit gradient,
 *          interior vertices are partitioned between the two conduits, and
 *          the new node's coordinate is the polyline point at t.
 */
SplitResult vj_split_conduit(SimulationContext& ctx, int link_idx, double t,
                             const std::string& new_node_name,
                             const std::string& new_link_name,
                             bool make_virtual);

/**
 * @brief Re-fuse the two conduits of a virtual junction into one, deleting
 *        the node (inverse of a virtual split).
 *
 * @details The upstream conduit's name survives; lengths sum; the node
 *          coordinate becomes an interior vertex of the merged conduit so
 *          map alignment is preserved; the downstream conduit's identifiers
 *          are retired. Requires a through orientation.
 *
 * @param surviving_link_out  Optional: receives the surviving conduit's
 *                            index AFTER deletions renumber the arrays.
 * @returns 0 on success, ERR_VJ_LINK_COUNT if the node is not a through
 *          two-conduit virtual junction, or -1 for a bad index / non-virtual
 *          node.
 */
int vj_fuse(SimulationContext& ctx, int node_idx, int* surviving_link_out);

// ---------------------------------------------------------------------------
// Inlet junctions — a virtual junction that also carries a street inlet
// (plans/INLET_JUNCTION_IMPLEMENTATION_PLAN_2026-09-05.md §2.6).
// ---------------------------------------------------------------------------

/**
 * @brief Check the inlet-junction usage rules for a node: every
 *        virtual-junction rule, then rule 623 — both attached conduits carry
 *        the cross-section the design needs (STREET_XSECT, or
 *        RECT_OPEN/TRAPEZOIDAL when @p for_drop_inlet).
 *
 * @returns 0 when eligible, else the ERR_VJ_* / ERR_IJ_NOT_STREET code.
 */
int ij_rule_violation(const SimulationContext& ctx, int node_idx, bool for_drop_inlet);

/**
 * @brief The conduit whose cross-section governs an inlet usage row.
 *
 * @param host_kind 0 = link host (the conduit itself), 1 = node host (the
 *                  conduit approaching the inlet junction — `node2 == host` —
 *                  else the first attached conduit).
 * @returns The link index, or -1 when there is none.
 */
int ij_host_conduit(const SimulationContext& ctx, int host_kind, int host_idx);

/**
 * @brief Legacy inlet_validate shape rule (inlet.c:498-505): CUSTOM designs
 *        fit any shape, DROP_* need RECT_OPEN/TRAPEZOIDAL, everything else
 *        needs STREET_XSECT.
 */
bool ij_usage_shape_ok(const SimulationContext& ctx, int design_idx, int link_idx);

/**
 * @brief Promote a node to an inlet junction or demote it.
 *
 * @details Promotion runs @ref ij_rule_violation first (nothing is changed on
 *          a violation), makes the node virtual when it is not already, and
 *          sets `is_inlet`. The drop-inlet variant of rule 623 is selected
 *          from the node's existing usage row when it has one. Demotion clears
 *          `is_inlet` and erases the node's usage row, leaving a plain virtual
 *          junction (use @ref vj_set_virtual to go further).
 *
 * @returns 0 on success, a rule code on a violation, or -1 for a bad index /
 *          non-junction node.
 */
int ij_set_inlet(SimulationContext& ctx, int node_idx, bool make_inlet);

/**
 * @brief Split a conduit and make the inserted node an inlet junction with the
 *        given design and capture node.
 *
 * @details @ref vj_split_conduit (make_virtual) followed by the usage row and
 *          @ref ij_set_inlet. Atomic: any failure after the split is undone
 *          with @ref vj_fuse and reported through SplitResult::err
 *          (625 for a bad design, 627 for a bad capture node, else a rule code).
 */
SplitResult ij_split_conduit(SimulationContext& ctx, int link_idx, double t,
                             const std::string& new_node_name,
                             const std::string& new_link_name,
                             int design_idx, int capture_node_idx);

/**
 * @brief Inverse of @ref ij_split_conduit: re-fuse an inlet junction's conduit
 *        pair. The node deletion inside @ref vj_fuse cascades the node-hosted
 *        usage row away with the node; a failed fuse changes nothing.
 *
 * @returns 0 on success, ERR_VJ_LINK_COUNT when the node is not a through
 *          two-conduit pair, or -1 when it is not an inlet junction.
 */
int ij_fuse(SimulationContext& ctx, int node_idx, int* surviving_link_out);

} // namespace openswmm::edit

#endif /* OPENSWMM_ENGINE_VIRTUAL_JUNCTION_OPS_HPP */
