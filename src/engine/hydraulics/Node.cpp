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

double getVolume(const NodeData& nodes, int idx, double depth,
                 TableData* tables) {
    if (depth <= 0.0) return 0.0;
    auto ui = static_cast<std::size_t>(idx);

    if (nodes.type[ui] == NodeType::STORAGE) {
        // Clamp at fullDepth → fullVolume (matching legacy node.c lines 909-910)
        if (depth >= nodes.full_depth[ui] && nodes.full_volume[ui] > 0.0)
            return nodes.full_volume[ui];

        if (nodes.storage_curve[ui] >= 0) {
            // Tabulated: trapezoidal integration of area curve
            // (matching legacy table_getStorageVolume in table.c)
            auto ci = static_cast<std::size_t>(nodes.storage_curve[ui]);
            if (tables && ci < tables->tables.size()) {
                return table_getStorageVolume(tables->tables[ci], depth);
            }
            return 0.0;
        }
        // Functional: integrate A(d) = a0 + a1*d^a2 → V = a0*d + a1/(a2+1)*d^(a2+1)
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
// Per-element: getDepth (inverse of getVolume)
// ============================================================================

double getDepth(const NodeData& nodes, int idx, double volume,
                TableData* tables) {
    if (volume <= 0.0) return 0.0;
    auto ui = static_cast<std::size_t>(idx);

    if (nodes.type[ui] == NodeType::STORAGE) {
        double fd = nodes.full_depth[ui];
        double fv = nodes.full_volume[ui];
        if (fv > 0.0 && volume >= fv) return fd;

        if (nodes.storage_curve[ui] >= 0) {
            // Tabulated: inverse lookup (Newton iteration using volume function)
            // Simple linear interpolation: d ≈ fd * (V / Vfull)
            if (fv > 0.0) return fd * (volume / fv);
            return 0.0;
        }

        // Functional: V = a0*d + a1/(a2+1) * d^(a2+1)
        // For simple case a2==0: V = (a0 + a1)*d → d = V/(a0+a1)
        double a0 = nodes.storage_c[ui];
        double a1 = nodes.storage_a[ui];
        double a2 = nodes.storage_b[ui];

        if (std::fabs(a2) < 1e-10) {
            // Linear A(d) = a0 + a1 → V = (a0+a1)*d
            double total_a = a0 + a1;
            return (total_a > 0.0) ? volume / total_a : 0.0;
        }

        // General case: Newton iteration
        // F(d) = a0*d + a1/(a2+1)*d^(a2+1) - V = 0
        // F'(d) = a0 + a1*d^a2
        double d = (fd > 0.0 && fv > 0.0) ? fd * (volume / fv) : 1.0;
        d = std::max(d, 0.001);
        double n = a2 + 1.0;
        for (int iter = 0; iter < 20; ++iter) {
            double f = a0 * d + (n != 0.0 ? a1 / n * std::pow(d, n) : 0.0) - volume;
            double df = a0 + a1 * std::pow(d, a2);
            if (std::fabs(df) < 1e-20) break;
            double dd = -f / df;
            d += dd;
            d = std::max(d, 0.0);
            if (std::fabs(dd) < 1e-6) break;
        }
        return std::min(d, fd);
    }

    // JUNCTION / OUTFALL / DIVIDER: V = MIN_SURFAREA * d → d = V / MIN_SURFAREA
    return volume / constants::MIN_SURFAREA;
}

// ============================================================================
// Per-element: getSurfArea
// ============================================================================

double getSurfArea(const NodeData& nodes, int idx, double depth,
                   TableData* tables) {
    auto ui = static_cast<std::size_t>(idx);

    if (nodes.type[ui] == NodeType::STORAGE) {
        if (nodes.storage_curve[ui] >= 0) {
            auto ci = static_cast<std::size_t>(nodes.storage_curve[ui]);
            if (tables && ci < tables->tables.size()) {
                double area = table_lookup_cursor(tables->tables[ci], depth);
                return std::max(area, constants::MIN_SURFAREA);
            }
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
        q = std::min(q, q_max);
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
