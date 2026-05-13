/**
 * @file Node.hpp
 * @brief Node hydraulics — volume/depth/head conversions, surface area, overflow.
 *
 * @details All functions operate on SoA arrays from NodeData. They are designed
 *          for batch operation over all nodes in a single call.
 *
 * @note Legacy reference: src/legacy/engine/node.c
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_NODE_HPP
#define OPENSWMM_NODE_HPP

#include "../core/Constants.hpp"
#include "../data/NodeData.hpp"
#include "../data/TableData.hpp"

namespace openswmm {

struct SimulationContext;

namespace node {

// ============================================================================
// Per-element functions (for C API / single-object queries)
// ============================================================================

/**
 * @brief Compute volume at a given depth for a single node.
 *
 * @details For JUNCTION: V = fullVolume * (depth / fullDepth).
 *          For STORAGE: uses functional or tabulated relationship.
 *
 * @param nodes  SoA node data.
 * @param idx    Node index.
 * @param depth  Water depth (ft).
 * @returns Volume (ft3).
 */
double getVolume(const NodeData& nodes, int idx, double depth,
                 TableData* tables = nullptr, int unit_sys = 0);

/**
 * @brief Compute surface area at a given depth for a single node.
 *
 * @details For JUNCTION: returns MIN_SURFAREA (small constant).
 *          For STORAGE: uses functional or tabulated relationship.
 *
 * @param nodes  SoA node data.
 * @param idx    Node index.
 * @param depth  Water depth (ft).
 * @returns Surface area (ft2).
 */
double getSurfArea(const NodeData& nodes, int idx, double depth,
                   TableData* tables = nullptr, int unit_sys = 0);

/**
 * @brief Get the ponded area (for overflow above rim).
 *
 * @details If depth <= fullDepth, returns normal surface area.
 *          If depth > fullDepth and pondedArea > 0, returns pondedArea.
 *
 * @param nodes  SoA node data.
 * @param idx    Node index.
 * @param depth  Water depth (ft).
 * @returns Effective surface area (ft2).
 */
double getPondedArea(const NodeData& nodes, int idx, double depth);

/**
 * @brief Compute max outflow limited by available volume.
 *
 * @details qMax = inflow + oldVolume / dt. Outflow cannot exceed this.
 *
 * @param nodes   SoA node data.
 * @param idx     Node index.
 * @param q       Proposed outflow (ft3/s).
 * @param dt      Timestep (seconds).
 * @returns Clamped outflow (ft3/s, >= 0).
 */
double getMaxOutflow(const NodeData& nodes, int idx, double q, double dt);

/**
 * @brief Compute overflow rate at a node.
 *
 * @details overflow = max(0, (newVolume - fullVolume) / dt)
 *
 * @param nodes      SoA node data.
 * @param idx        Node index.
 * @param new_volume Current volume (ft3).
 * @param full_volume Volume at full depth (ft3).
 * @param dt         Timestep (seconds).
 * @returns Overflow rate (ft3/s).
 */
double getOverflow(double new_volume, double full_volume, double dt);

/**
 * @brief Compute depth from volume for a single node (inverse of getVolume).
 *
 * @details For JUNCTION: d = V / MIN_SURFAREA.
 *          For STORAGE functional: Newton iteration or direct inversion.
 *          For STORAGE tabular: quadratic solve per interval (Gap #12).
 *  @see Legacy: node_getDepth() in node.c
 */
double getDepth(const NodeData& nodes, int idx, double volume,
                TableData* tables = nullptr, int unit_sys = 0);

/**
 * @brief Compute head from depth: head = invert + depth.
 */
inline double getHead(double invert_elev, double depth) {
    return invert_elev + depth;
}

// ============================================================================
// Batch functions (for routing hot loop)
// ============================================================================

/**
 * @brief Compute head = invert + depth for all nodes.
 *
 * @param invert  [in]  Invert elevation array.
 * @param depth   [in]  Depth array.
 * @param head    [out] Head array.
 * @param n       Number of nodes.
 */
void computeHeads(const double* invert, const double* depth, double* head, int n);

/**
 * @brief Compute volumes for all nodes from their current depths.
 *
 * @param nodes   SoA node data (reads type, full_depth, storage params).
 * @param depth   [in]  Depth array (may differ from nodes.depth).
 * @param volume  [out] Volume array.
 */
void computeVolumes(const NodeData& nodes, const double* depth, double* volume);

/**
 * @brief Compute overflow for all nodes.
 *
 * @param new_volume  [in]  Current volume array.
 * @param full_volume [in]  Full volume array.
 * @param overflow    [out] Overflow rate array.
 * @param dt          Timestep (seconds).
 * @param n           Number of nodes.
 */
void computeOverflows(const double* new_volume, const double* full_volume,
                      double* overflow, double dt, int n);

} // namespace node

// Constants now consolidated in core/Constants.hpp

} // namespace openswmm

#endif // OPENSWMM_NODE_HPP
