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
 * @file      Node.hpp
 * @brief     Node hydraulics — volume/depth/head conversions, surface area, overflow.
 *
 * @details All functions operate on SoA arrays from NodeData. They are designed
 *          for batch operation over all nodes in a single call.
 *
 * @note Legacy reference: src/legacy/engine/node.c
 * @ingroup new_engine
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_NODE_HPP
#define OPENSWMM_NODE_HPP

#include "../core/Constants.hpp"
#include "../data/NodeData.hpp"
#include "../data/TableData.hpp"

namespace openswmm {

struct SimulationContext;
struct NodeSubtypes; // relational storage/outfall/divider side-tables (optional source)

namespace node {

// ============================================================================
// Per-element functions (for C API / single-object queries)
// ============================================================================

/**
 * @brief Evaluate a node depth-volume relation in native units.
 *
 * Storage LID cells take precedence: Richards retains only surface pore water; other LID cells sum
 * depth-filled mobile pore capacity. Ordinary storage clamps to positive full_volume at full_depth,
 * then uses tabular integration or authored functional/quadratic coefficients with per-call
 * display-unit conversion. Other node types scale positive full_volume (or MIN_SURFAREA*full_depth)
 * linearly with depth, without an upper clamp; nonpositive full_depth returns zero.
 *
 * @param[in] nodes Initialized node arrays; all accessed arrays must contain idx.
 * @param[in] idx Valid nonnegative global node index, unchecked.
 * @param[in] depth Water depth in ft; values <=0 return zero before node indexing.
 * @param[in] tables Borrowed project tables, or nullptr. Referenced missing tables produce zero
 *     volume/area; depth uses its documented fallback. These queries do not mutate
 *     table contents or lookup cursors.
 * @param[in] unit_sys Valid Ucf unit-system column (0 US, 1 SI); storage coefficients and tables
 *     use this project display system.
 * @param[in] subs Borrowed node subtype rows, or nullptr for zero functional storage coefficients.
 *     Required for actual storage/LID geometry; no pointer is retained.
 * @return Volume in ft3; no allocation or geometry validation.
 */
double getVolume(const NodeData& nodes, int idx, double depth, TableData* tables = nullptr,
                 int unit_sys = 0, const NodeSubtypes* subs = nullptr);

/**
 * @brief Evaluate the raw derivative of storage volume with depth.
 *
 * LID storage returns the active cell mobile pore area; Richards uses surface area times porosity.
 * Ordinary tables extrapolate and functional/quadratic coefficients are evaluated in display units
 * before conversion. No MIN_SURFAREA floor is applied here; the routing depth update supplies its
 * own floor.
 *
 * @param[in] nodes Initialized node arrays; all accessed arrays must contain idx.
 * @param[in] idx Valid nonnegative global node index, unchecked.
 * @param[in] depth Water depth in ft; not clamped to zero or full depth.
 * @param[in] tables Borrowed project tables, or nullptr. Referenced missing tables produce zero
 *     volume/area; depth uses its documented fallback. These queries do not mutate
 *     table contents or lookup cursors.
 * @param[in] unit_sys Valid Ucf unit-system column (0 US, 1 SI); storage coefficients and tables
 *     use this project display system.
 * @param[in] subs Borrowed node subtype rows, or nullptr for zero functional storage coefficients.
 *     Required for actual storage/LID geometry; no pointer is retained.
 * @return Effective storage surface area in ft2, or zero for every non-storage node.
 */
double getSurfArea(const NodeData& nodes, int idx, double depth, TableData* tables = nullptr,
                   int unit_sys = 0, const NodeSubtypes* subs = nullptr);

/**
 * @brief Select an above-rim ponded area or the raw storage surface area.
 *
 * At/below full_depth, or with exactly zero ponded_area, delegates to getSurfArea at depth. Above
 * the rim a positive ponded_area is returned; a negative stored value falls back to getSurfArea at
 * full_depth. No routing ponding-enable option is examined.
 *
 * @param[in] nodes Initialized node arrays; all accessed arrays must contain idx.
 * @param[in] idx Valid nonnegative global node index, unchecked.
 * @param[in] depth Water depth in ft.
 * @param[in] tables Borrowed project tables, or nullptr. Referenced missing tables produce zero
 *     volume/area; depth uses its documented fallback. These queries do not mutate
 *     table contents or lookup cursors.
 * @param[in] unit_sys Valid Ucf unit-system column (0 US, 1 SI); storage coefficients and tables
 *     use this project display system.
 * @param[in] subs Borrowed node subtype rows, or nullptr for zero functional storage coefficients.
 *     Required for actual storage/LID geometry; no pointer is retained.
 * @return Area in ft2.
 */
double getPondedArea(const NodeData& nodes, int idx, double depth, TableData* tables = nullptr,
                     int unit_sys = 0, const NodeSubtypes* subs = nullptr);

/**
 * @brief Limit a proposed nonnegative withdrawal by legacy node storage availability.
 *
 * The eligibility test uses rpt_full_volume, not the internal full_volume bucket. Ordinary
 * junctions/outfalls/dividers are therefore not capped merely because internal mixed storage has a
 * positive capacity.
 *
 * @param[in] nodes Initialized node arrays; all accessed arrays must contain idx.
 * @param[in] idx Valid nonnegative global node index, unchecked.
 * @param[in] q Proposed flow in ft3/s; negative input is returned as zero.
 * @param[in] dt Positive timestep in seconds when rpt_full_volume is positive; unchecked.
 * @return max(0,min(q,inflow+old_volume/dt)) for positive rpt_full_volume, otherwise max(0,q).
 */
double getMaxOutflow(const NodeData& nodes, int idx, double q, double dt);

/**
 * @brief Convert excess node water volume into a timestep overflow rate.
 *
 * @param[in] new_volume Booked current volume in ft3.
 * @param[in] full_volume Rim capacity in ft3.
 * @param[in] dt Timestep in seconds; nonpositive values return zero.
 * @return (new_volume-full_volume)/dt in ft3/s when both excess and dt are positive; zero
 *     otherwise.
 */
double getOverflow(double new_volume, double full_volume, double dt);

/**
 * @brief Evaluate the mixed transport store from current node state.
 *
 * Requires initialized geometry and current state arrays. No state is modified.
 *
 * @param[in] nodes Initialized node arrays; all accessed arrays must contain idx.
 * @param[in] idx Valid nonnegative global node index, unchecked.
 * @return Store volume in ft3; delegates to the explicit-pair overload with nodes.depth and
 *     nodes.volume.
 */
double storeVolume(const NodeData& nodes, int idx);

/**
 * @brief Evaluate a transport store using an explicit depth and booked volume.
 *
 * Nonpositive full_depth returns the clamped booked volume. Outfalls retain the supplied booked
 * volume (normally zero), rather than an artificial junction bucket. For junctions/dividers
 * positive full_depth uses min(depth,full_depth)/full_depth; negative depth is not independently
 * clamped. This reconstructs the mixed below-rim store that differs from legacy booked volume.
 *
 * @param[in] nodes Initialized node arrays; all accessed arrays must contain idx.
 * @param[in] idx Valid nonnegative global node index, unchecked.
 * @param[in] depth Water depth in ft; callers supply nonnegative physical depth.
 * @param[in] volume Booked node volume in ft3; clamped to >=0 before use.
 * @return Storage/outfall booked volume, or a below-rim full_volume fraction plus booked ponded
 *     volume above the rim, in ft3.
 */
double storeVolume(const NodeData& nodes, int idx, double depth, double volume);

/**
 * @brief Convert booked storage volume to depth using native geometry.
 *
 * Ordinary storage uses a tabular inverse, direct power/quadratic inverses, or safeguarded Newton
 * in display-unit depth with tolerance 0.001. Positive full_volume clamps to full_depth. A missing
 * referenced table falls back to full_depth*volume/full_volume, or zero without positive
 * full_volume. Non-storage nodes always return volume/MIN_SURFAREA; this need not invert getVolume
 * when full_volume has been overridden. LID Richards divides by surface pore area and caps at
 * surface.top; other LID storage consumes mobile pore capacities in reverse cell order. Valid
 * nondegenerate coefficients/cell dimensions are required; zero denominators and nonfinite input
 * are not diagnosed.
 *
 * @param[in] nodes Initialized node arrays; all accessed arrays must contain idx.
 * @param[in] idx Valid nonnegative global node index, unchecked.
 * @param[in] volume Water volume in ft3; negative values return zero before indexing.
 * @param[in] tables Borrowed project tables, or nullptr. Referenced missing tables produce zero
 *     volume/area; depth uses its documented fallback. These queries do not mutate
 *     table contents or lookup cursors.
 * @param[in] unit_sys Valid Ucf unit-system column (0 US, 1 SI); storage coefficients and tables
 *     use this project display system.
 * @param[in] subs Borrowed node subtype rows, or nullptr for zero functional storage coefficients.
 *     Required for actual storage/LID geometry; no pointer is retained.
 * @return Depth in ft; general Newton cases return the last iterate, capped at full_depth, without
 *     reporting convergence.
 */
double getDepth(const NodeData& nodes, int idx, double volume, TableData* tables = nullptr,
                int unit_sys = 0, const NodeSubtypes* subs = nullptr);

/**
 * @brief Compute hydraulic head by adding node invert and depth.
 *
 * @param[in] invert_elev Invert elevation in native ft or another unit shared by depth.
 * @param[in] depth Water depth in the same length unit.
 * @return invert_elev+depth in their shared datum/unit; no clamping or finite validation.
 */
inline double getHead(double invert_elev, double depth) {
    return invert_elev + depth;
}

// ============================================================================
// Batch functions (for routing hot loop)
// ============================================================================

/**
 * @brief Add native invert elevation and depth elementwise.
 *
 * No allocation, clamping or finite validation. Partial overlapping spans are not supported.
 *
 * @param[in] invert Readable n-element elevation array in ft.
 * @param[in] depth Readable n-element depth array in ft.
 * @param[out] head Writable n-element head array in ft; exact alias with an input is valid.
 * @param[in] n Nonnegative node count, unchecked.
 */
void computeHeads(const double* invert, const double* depth, double* head, int n);

/**
 * @brief Evaluate every node volume with US-unit, table-free scalar geometry.
 *
 * Calls getVolume with nullptr tables and unit_sys=0 for each node. Consequently referenced tabular
 * storage returns zero unless an earlier full-volume clamp applies, and authored SI coefficients
 * are not converted with an SI column. Use scalar calls with tables/unit_sys where those project
 * features are required.
 *
 * @param[in] nodes Initialized node arrays; count() determines all buffer lengths.
 * @param[in] depth Readable nodes.count()-element depths in ft; may differ from nodes.depth.
 * @param[out] volume Writable nodes.count()-element volumes in ft3; exact alias with depth is
 *     valid.
 * @param[in] subs Borrowed optional subtype/LID rows used by getVolume; nullptr supplies default
 *     storage coefficients.
 */
void computeVolumes(const NodeData& nodes, const double* depth, double* volume,
                    const NodeSubtypes* subs = nullptr);

/**
 * @brief Compute per-node overflow from excess native volumes.
 *
 * For positive dt, outputs max(new_volume-full_volume,0)/dt. Partial overlaps are unsupported;
 * inputs are not checked for finite values.
 *
 * @param[in] new_volume Readable n-element current-volume array in ft3.
 * @param[in] full_volume Readable n-element rim-volume array in ft3.
 * @param[out] overflow Writable n-element overflow rates in ft3/s; exact alias with an input is
 *     valid.
 * @param[in] dt Timestep in seconds; <=0 zero-fills output without reading volume arrays.
 * @param[in] n Nonnegative count; pointer ranges must remain valid even for zero-fill.
 */
void computeOverflows(const double* new_volume, const double* full_volume, double* overflow,
                      double dt, int n);

} // namespace node

// Constants now consolidated in core/Constants.hpp

} // namespace openswmm

#endif // OPENSWMM_NODE_HPP
