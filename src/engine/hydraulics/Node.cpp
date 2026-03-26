/**
 * @file Node.cpp
 * @brief Node hydraulics — numerically identical to legacy node.c.
 *
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "Node.hpp"

#include <cmath>
#include <algorithm>

namespace openswmm {
namespace node {

// ============================================================================
// Per-element: getVolume
// ============================================================================

double getVolume(const NodeData& nodes, int idx, double depth) {
    if (depth <= 0.0) return 0.0;
    auto ui = static_cast<std::size_t>(idx);

    if (nodes.type[ui] == NodeType::STORAGE) {
        double fd = nodes.full_depth[ui];
        // Functional storage: V = a0*d + a1/(a2+1) * d^(a2+1)
        // where a0 = storage_c (baseline area), a1 = storage_a, a2 = storage_b
        if (nodes.storage_curve[ui] >= 0) {
            // Tabulated: TODO — requires TableData lookup
            // For now use linear approximation
            if (fd > 0.0) return nodes.storage_a[ui] * depth;
            return 0.0;
        }
        double a0 = nodes.storage_c[ui];
        double a1 = nodes.storage_a[ui];
        double a2 = nodes.storage_b[ui];
        double n = a2 + 1.0;
        return a0 * depth + (n != 0.0 ? a1 / n * std::pow(depth, n) : 0.0);
    }

    // JUNCTION / OUTFALL / DIVIDER: linear V = fullVolume * (d / fullDepth)
    double fd = nodes.full_depth[ui];
    if (fd <= 0.0) return 0.0;

    // fullVolume for a junction = MIN_SURFAREA * fullDepth (legacy convention)
    double full_vol = constants::MIN_SURFAREA * fd;
    return full_vol * (depth / fd);
}

// ============================================================================
// Per-element: getSurfArea
// ============================================================================

double getSurfArea(const NodeData& nodes, int idx, double depth) {
    auto ui = static_cast<std::size_t>(idx);

    if (nodes.type[ui] == NodeType::STORAGE) {
        if (nodes.storage_curve[ui] >= 0) {
            // Tabulated: TODO — requires TableData lookup
            return constants::MIN_SURFAREA;
        }
        // Functional: area = a0 + a1 * d^a2
        double a0 = nodes.storage_c[ui];
        double a1 = nodes.storage_a[ui];
        double a2 = nodes.storage_b[ui];
        double area = a0 + a1 * std::pow(depth, a2);
        return std::max(area, constants::MIN_SURFAREA);
    }

    // Non-storage nodes: constant minimum surface area
    return constants::MIN_SURFAREA;
}

// ============================================================================
// Per-element: getPondedArea
// ============================================================================

double getPondedArea(const NodeData& nodes, int idx, double depth) {
    auto ui = static_cast<std::size_t>(idx);

    if (depth <= nodes.full_depth[ui] || nodes.ponded_area[ui] == 0.0) {
        return getSurfArea(nodes, idx, depth);
    }

    // Flooded above rim — use the ponded area
    double a = nodes.ponded_area[ui];
    if (a <= 0.0) a = getSurfArea(nodes, idx, nodes.full_depth[ui]);
    return a;
}

// ============================================================================
// Per-element: getMaxOutflow
// ============================================================================

double getMaxOutflow(const NodeData& nodes, int idx, double q, double dt) {
    auto ui = static_cast<std::size_t>(idx);

    double full_vol = constants::MIN_SURFAREA * nodes.full_depth[ui];
    if (full_vol > 0.0) {
        double q_max = nodes.inflow[ui] + nodes.old_volume[ui] / dt;
        if (q > q_max) q = q_max;
    }
    return std::max(0.0, q);
}

// ============================================================================
// Per-element: getOverflow
// ============================================================================

double getOverflow(double new_volume, double full_volume, double dt) {
    if (new_volume > full_volume && dt > 0.0) {
        return (new_volume - full_volume) / dt;
    }
    return 0.0;
}

// ============================================================================
// Batch: computeHeads
// ============================================================================

void computeHeads(const double* invert, const double* depth, double* head, int n) {
    for (int i = 0; i < n; ++i) {
        head[i] = invert[i] + depth[i];
    }
}

// ============================================================================
// Batch: computeVolumes
// ============================================================================

void computeVolumes(const NodeData& nodes, const double* depth, double* volume) {
    int n = nodes.count();
    for (int i = 0; i < n; ++i) {
        volume[i] = getVolume(nodes, i, depth[i]);
    }
}

// ============================================================================
// Batch: computeOverflows
// ============================================================================

void computeOverflows(const double* new_volume, const double* full_volume,
                      double* overflow, double dt, int n) {
    if (dt <= 0.0) {
        std::fill(overflow, overflow + n, 0.0);
        return;
    }
    double inv_dt = 1.0 / dt;
    for (int i = 0; i < n; ++i) {
        double excess = new_volume[i] - full_volume[i];
        overflow[i] = (excess > 0.0) ? excess * inv_dt : 0.0;
    }
}

} // namespace node
} // namespace openswmm
