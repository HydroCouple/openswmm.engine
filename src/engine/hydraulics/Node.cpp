/**
 * @file Node.cpp
 * @brief Node hydraulics — numerically identical to legacy node.c.
 *
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "Node.hpp"

#include "../core/UnitConversion.hpp"
#include "../data/NodeSubtypes.hpp"
#include "../math/FindRoot.hpp"
#include <cmath>
#include <algorithm>

namespace openswmm {
namespace node {

// ============================================================================
// Storage geometry accessor (relational side-table)
// ============================================================================
//
// Relational node refactor (Phase 4): storage geometry (curve, a, b, c) lives in
// the dense StorageData side-table (the wide NodeData arrays are gone). Returns
// the row's geometry when a NodeSubtypes with a matching storage row is supplied;
// otherwise the resize defaults (-1 curve / 0 a,b,c) for the degenerate case
// (no side-table, or a non-storage node — callers gate on type[i]==STORAGE).
namespace {
struct StorageGeom { int curve; double a; double b; double c; };

inline StorageGeom storageGeom(const NodeData& nodes, const NodeSubtypes* subs,
                               std::size_t ui) {
    (void)nodes;
    if (subs != nullptr) {
        const int r = subs->storage_row(static_cast<int>(ui));
        if (r >= 0) {
            const auto ur = static_cast<std::size_t>(r);
            return StorageGeom{ subs->storages.curve[ur], subs->storages.a[ur],
                                subs->storages.b[ur], subs->storages.c[ur] };
        }
    }
    return StorageGeom{ -1, 0.0, 0.0, 0.0 };
}
}  // namespace

// ============================================================================
// Per-element: getVolume
// ============================================================================

double getVolume(const NodeData& nodes, int idx, double depth,
                 TableData* tables, int unit_sys, const NodeSubtypes* subs) {
    if (depth <= 0.0) return 0.0;
    auto ui = static_cast<std::size_t>(idx);

    if (nodes.type[ui] == NodeType::STORAGE) {
        // Clamp at fullDepth → fullVolume (matching legacy node.c lines 909-910)
        if (depth >= nodes.full_depth[ui] && nodes.full_volume[ui] > 0.0)
            return nodes.full_volume[ui];

        // Geometry fetched lazily, only after the clamp early-out (as legacy did).
        const StorageGeom g = storageGeom(nodes, subs, ui);

        if (g.curve >= 0) {
            // Tabulated: trapezoidal integration of area curve
            // (matching legacy table_getStorageVolume in table.c)
            auto ci = static_cast<std::size_t>(g.curve);
            if (tables && ci < tables->tables.size()) {
                double ucf_len  = ucf::Ucf[ucf::LENGTH][unit_sys];
                double ucf_vol  = ucf::Ucf[ucf::VOLUME][unit_sys];
                double vol_disp = table_getStorageVolume(tables->tables[ci], depth * ucf_len);
                return vol_disp / ucf_vol;
            }
            return 0.0;
        }
        // Functional: integrate A(d) = a0 + a1*d^a2 → V = a0*d + a1/(a2+1)*d^(a2+1)
        //
        // PARITY: legacy storage_getVolume (node.c:923-927) keeps a0/a1/a2 in
        // USER units and converts per call:
        //   d *= UCF(LENGTH);
        //   n = Storage[k].a2 + 1.0;
        //   v = (Storage[k].a0 * d) + Storage[k].a1 / n * pow(d, n);
        //   return v / UCF(VOLUME);
        // The SI VOLUME factor is the TRUNCATED 0.02832 (swmm5.c:157), not
        // 0.3048³, so this per-call regime is FP-distinguishable from
        // evaluating pre-converted internal coefficients — mirror it exactly.
        double d = depth * ucf::Ucf[ucf::LENGTH][unit_sys];
        double n = g.b + 1.0;
        double v = (g.c * d) + g.a / n * std::pow(d, n);
        return v / ucf::Ucf[ucf::VOLUME][unit_sys];
    }

    // JUNCTION / OUTFALL / DIVIDER: linear V = fullVolume * (d / fullDepth)
    double fd = nodes.full_depth[ui];
    if (fd <= 0.0) return 0.0;

    // fullVolume for a junction = MIN_SURFAREA * fullDepth (legacy convention),
    // UNLESS an override has been stored (e.g. a Type-1 pump wet well, set in
    // SWMMEngine::initialize from the pump curve's max volume — matches legacy
    // pump_validate). full_volume is 0 before init, so fall back then.
    double full_vol = nodes.full_volume[ui] > 0.0
                          ? nodes.full_volume[ui]
                          : constants::MIN_SURFAREA * fd;
    return full_vol * (depth / fd);
}

// ============================================================================
// Per-element: getDepth (inverse of getVolume)
// ============================================================================

double getDepth(const NodeData& nodes, int idx, double volume,
                TableData* tables, int unit_sys, const NodeSubtypes* subs) {
    if (volume <= 0.0) return 0.0;
    auto ui = static_cast<std::size_t>(idx);

    if (nodes.type[ui] == NodeType::STORAGE) {
        double fd = nodes.full_depth[ui];
        double fv = nodes.full_volume[ui];
        if (fv > 0.0 && volume >= fv) return fd;

        // Geometry fetched lazily, only after the clamp early-out (as legacy did).
        const StorageGeom g = storageGeom(nodes, subs, ui);

        if (g.curve >= 0) {
            // Tabulated: quadratic solve per interval (Gap #12).
            // Matches legacy table_getStorageDepth() in table.c.
            auto ci = static_cast<std::size_t>(g.curve);
            if (tables && ci < tables->tables.size()) {
                double ucf_len = ucf::Ucf[ucf::LENGTH][unit_sys];
                double ucf_vol = ucf::Ucf[ucf::VOLUME][unit_sys];
                double vol_disp = volume * ucf_vol;       // internal ft³ → display units
                double d_disp   = table_getStorageDepth(tables->tables[ci], vol_disp);
                return d_disp / ucf_len;                  // display units → ft
            }
            if (fv > 0.0) return fd * (volume / fv);     // fallback if no table
            return 0.0;
        }

        // Functional: V = a0*d + a1/(a2+1) * d^(a2+1)
        //
        // PARITY: legacy storage_getDepth (node.c:778-866) keeps a0/a1/a2 in
        // USER units: it converts the target volume to user units up front
        // (v *= UCF(VOLUME), node.c:801), solves for depth in user units per
        // the case split below, then d /= UCF(LENGTH) and clamps to fullDepth
        // (node.c:861-864). Mirror the exact expressions and operand order.
        double a0 = g.c;
        double a1 = g.a;
        double a2 = g.b;
        const double ucf_len = ucf::Ucf[ucf::LENGTH][unit_sys];
        double v = volume * ucf::Ucf[ucf::VOLUME][unit_sys];
        double d;

        if (a2 == 0.0) {
            // area = a0 + a1; v = (a0 + a1) * d              (node.c:825-829)
            d = v / (a0 + a1);
        } else if (a0 == 0.0) {
            // area = a1*d^a2; v = a1/(a2+1)*d^(a2+1)         (node.c:831-836)
            double e = 1.0 / (a2 + 1.0);
            d = std::pow(v / (a1 * e), e);
        } else if (a2 == 1.0 && a1 > 0.0) {
            // area = a0 + a1*d; v = a0*d + (a1/2)*d^2        (node.c:838-841)
            d = (std::sqrt(a0 * a0 + 2. * a1 * v) - a0) / a1;
        } else {
            // area = a0 + a1*d^a2 — Newton/bisection on the USER-unit depth
            // over [0, fullDepth*UCF(LENGTH)], xacc 0.001  (node.c:843-847).
            // Legacy's storage_getVolDiff (node.c:871-891) differences the
            // INTERNAL-unit storage_getVolume at the user-unit trial depth
            // against the USER-unit target v, and uses the internal-unit
            // storage_getSurfArea as the derivative — a legacy unit quirk,
            // replicated as-is. (Legacy also passes the storage subIndex
            // where those functions expect a node index; we pass the node
            // index, which resolves to the same storage geometry.)
            d = v / (a0 + a1);
            findroot::newton(0.0, fd * ucf_len, &d, 0.001,
                [&](double y, double* f, double* df) {
                    *f  = getVolume(nodes, idx, y, tables, unit_sys, subs) - v;
                    *df = getSurfArea(nodes, idx, y, tables, unit_sys, subs);
                });
        }
        d /= ucf_len;                                       // node.c:861
        return std::min(d, fd);                             // node.c:862-864
    }

    // JUNCTION / OUTFALL / DIVIDER: V = MIN_SURFAREA * d → d = V / MIN_SURFAREA
    return volume / constants::MIN_SURFAREA;
}

// ============================================================================
// Per-element: getSurfArea
// ============================================================================

double getSurfArea(const NodeData& nodes, int idx, double depth,
                   TableData* tables, int unit_sys, const NodeSubtypes* subs) {
    auto ui = static_cast<std::size_t>(idx);

    if (nodes.type[ui] == NodeType::STORAGE) {
        const StorageGeom g = storageGeom(nodes, subs, ui);
        // Return RAW storage-curve area (no MIN_SURFAREA clamp here).
        //
        // Legacy storage_getSurfArea (node.c:944) returns the curve value
        // directly; the MIN_SURFAREA floor is applied downstream in
        // setNodeDepth via MAX(surfArea, MinSurfArea). Clamping here
        // over-reports the effective area at degenerate storages (e.g.
        // A=0,B=0,C=19.625 → legacy returns 19.625; clamped returns 50),
        // which then interacts with the STORAGE-end scatter rule to keep
        // pipe halves the legacy would drop. On the Rich_BC_CSO model
        // this mis-scaled the Picard denominator by ~3× and shifted
        // flooding to non-legacy nodes.
        if (g.curve >= 0) {
            auto ci = static_cast<std::size_t>(g.curve);
            if (tables && ci < tables->tables.size()) {
                double ucf_len  = ucf::Ucf[ucf::LENGTH][unit_sys];
                // PARITY: legacy storage_getSurfArea (node.c) uses the
                // EXTRAPOLATING table_lookupEx (table.c:469) — not the
                // clamped table_lookup — and converts units with two
                // successive divisions: `area / UCF(LENGTH) / UCF(LENGTH)`.
                // A single divide by (ucf_len*ucf_len) rounds differently.
                double area = table_lookupEx(tables->tables[ci], depth * ucf_len);
                return area / ucf_len / ucf_len;
            }
            return 0.0;
        }
        // Functional: area = a0 + a1 * d^a2
        //
        // PARITY: legacy storage_getSurfArea (node.c:966-968, 977) keeps
        // a0/a1/a2 in USER units and converts per call, with TWO successive
        // divisions (not one divide by len²):
        //   area = Storage[k].a0 + Storage[k].a1 * pow(d*UCF(LENGTH), Storage[k].a2);
        //   return area / UCF(LENGTH) / UCF(LENGTH);
        double ucf_len = ucf::Ucf[ucf::LENGTH][unit_sys];
        double area = g.c + g.a * std::pow(depth * ucf_len, g.b);
        return area / ucf_len / ucf_len;
    }

    // Non-storage nodes: legacy node_getSurfArea returns 0 for everything
    // other than STORAGE. The MinSurfArea floor is applied in setNodeDepth.
    return 0.0;
}

// ============================================================================
// Per-element: getPondedArea
// ============================================================================

double getPondedArea(const NodeData& nodes, int idx, double depth,
                     TableData* tables, int unit_sys, const NodeSubtypes* subs) {
    auto ui = static_cast<std::size_t>(idx);

    if (depth <= nodes.full_depth[ui] || nodes.ponded_area[ui] == 0.0) {
        return getSurfArea(nodes, idx, depth, tables, unit_sys, subs);
    }

    // Flooded above rim — use the ponded area
    double a = nodes.ponded_area[ui];
    if (a <= 0.0) a = getSurfArea(nodes, idx, nodes.full_depth[ui], tables, unit_sys, subs);
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

void computeVolumes(const NodeData& nodes, const double* depth, double* volume,
                    const NodeSubtypes* subs) {
    int n = nodes.count();
    for (int i = 0; i < n; ++i) {
        volume[i] = getVolume(nodes, i, depth[i], nullptr, 0, subs);
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
