/**
 * @file Exfiltration.cpp
 * @brief Storage exfiltration — numerically identical to legacy exfil.c.
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "Exfiltration.hpp"
#include "../core/SimulationContext.hpp"
#include <cmath>
#include <algorithm>

namespace openswmm {
namespace exfil {

void ExfilSoA::resize(int n) {
    count = n;
    auto un = static_cast<size_t>(n);
    node_idx.assign(un, -1);
    btm_area.assign(un, 0.0);
    bank_min_depth.assign(un, 0.0);
    bank_max_depth.assign(un, 0.0);
    bank_max_area.assign(un, 0.0);
    btm_ga.resize(un);
    bank_ga.resize(un);
}

void ExfilSolver::init(SimulationContext& ctx) {
    auto& nodes = ctx.nodes;
    int n_nodes = nodes.count();

    // --- First pass: count storage nodes that have exfiltration parameters
    //     A node has exfiltration if it is STORAGE type and exfil_ksat > 0
    int n_exfil = 0;
    for (int i = 0; i < n_nodes; ++i) {
        auto ui = static_cast<size_t>(i);
        if (nodes.type[ui] == NodeType::STORAGE && nodes.exfil_ksat[ui] > 0.0) {
            ++n_exfil;
        }
    }

    if (n_exfil == 0) {
        soa_.count = 0;
        return;
    }

    // --- Second pass: populate ExfilSoA
    soa_.resize(n_exfil);

    constexpr double BIG = 1.0E10;
    int k = 0;
    for (int i = 0; i < n_nodes; ++i) {
        auto ui = static_cast<size_t>(i);
        if (nodes.type[ui] != NodeType::STORAGE || nodes.exfil_ksat[ui] <= 0.0) {
            continue;
        }

        auto uk = static_cast<size_t>(k);
        soa_.node_idx[uk] = i;

        // --- Initialize Green-Ampt states for bottom and bank
        //     Uses the same soil parameters for both (matching legacy createStorageExfil)
        infil::grnampt_init(soa_.btm_ga[uk],
                            nodes.exfil_suction[ui],
                            nodes.exfil_ksat[ui],
                            nodes.exfil_imd[ui],
                            ctx.options);
        infil::grnampt_init(soa_.bank_ga[uk],
                            nodes.exfil_suction[ui],
                            nodes.exfil_ksat[ui],
                            nodes.exfil_imd[ui],
                            ctx.options);

        // --- Compute bottom area and bank geometry from storage shape
        int curve_idx = nodes.storage_curve[ui];

        if (curve_idx >= 0) {
            // --- TABULAR: storage shape given by a storage curve
            //     Legacy: exfil_initState() TABULAR case
            auto& curve = ctx.tables[curve_idx];

            // Bottom area = curve value at depth 0
            soa_.btm_area[uk] = table_lookup_cursor(curve, 0.0);

            // Find bank min/max depths and max bank area by scanning curve
            soa_.bank_min_depth[uk] = 0.0;
            soa_.bank_max_depth[uk] = 0.0;
            soa_.bank_max_area[uk]  = 0.0;

            if (!curve.x.empty()) {
                double alast = curve.y[0];
                for (size_t ci = 1; ci < curve.x.size(); ++ci) {
                    double d = curve.x[ci];
                    double a = curve.y[ci];

                    if (a < alast) {
                        break;
                    } else if (a > alast) {
                        soa_.bank_max_area[uk]  = a;
                        soa_.bank_max_depth[uk] = d;
                    } else if (soa_.bank_max_area[uk] == 0.0) {
                        soa_.bank_min_depth[uk] = d;
                    } else {
                        break;
                    }
                    alast = a;
                }
            }

            // Note: legacy converts from user units to internal units here.
            // In the new engine, values are assumed to already be in internal
            // units (ft) after parsing. If unit conversion is needed during
            // parsing, it should be done there, not here.

        } else {
            // --- FUNCTIONAL: area = A * depth^B + C
            //     Legacy: exfil_initState() FUNCTIONAL case
            //     Bottom area: at depth=0, area = A*0^B + C = C
            //     Exception: if B==0 (exponent is zero), area = A*1 + C = A+C
            double a_coeff = nodes.storage_a[ui];
            double b_coeff = nodes.storage_b[ui];
            double c_coeff = nodes.storage_c[ui];

            double btm = c_coeff;
            if (b_coeff == 0.0) {
                btm += a_coeff;
            }
            soa_.btm_area[uk] = btm;

            // For functional/cylindrical/conical/pyramidal shapes,
            // bank seepage extends from depth 0 to infinity (BIG)
            // Legacy: bankMinDepth=0, bankMaxDepth=BIG, bankMaxArea=BIG
            soa_.bank_min_depth[uk] = 0.0;
            soa_.bank_max_depth[uk] = BIG;
            soa_.bank_max_area[uk]  = BIG;
        }

        ++k;
    }
}

void ExfilSolver::computeAll(SimulationContext& ctx, double dt) {
    auto& nodes = ctx.nodes;

    for (int k = 0; k < soa_.count; ++k) {
        auto uk = static_cast<size_t>(k);
        int ni = soa_.node_idx[uk];
        if (ni < 0) continue;
        auto uni = static_cast<size_t>(ni);

        double depth = nodes.depth[uni];
        if (depth <= 0.0) continue;

        double total_loss = 0.0;

        // Bottom exfiltration
        double btm_rate = infil::grnampt_getInfil(soa_.btm_ga[uk], 0.0, depth, dt);
        total_loss += btm_rate * soa_.btm_area[uk];

        // Bank exfiltration (only above bank_min_depth)
        if (depth > soa_.bank_min_depth[uk] && soa_.bank_max_area[uk] > 0.0) {
            double bank_depth;
            if (depth > soa_.bank_max_depth[uk]) {
                bank_depth = depth - soa_.bank_max_depth[uk]
                           + (soa_.bank_max_depth[uk] - soa_.bank_min_depth[uk]) / 2.0;
            } else {
                bank_depth = (depth - soa_.bank_min_depth[uk]) / 2.0;
            }

            double bank_area = soa_.bank_max_area[uk];
            double bank_rate = infil::grnampt_getInfil(soa_.bank_ga[uk], 0.0, bank_depth, dt);
            total_loss += bank_rate * bank_area;
        }

        // Limit to available volume
        double max_loss = nodes.volume[uni] / dt;
        total_loss = std::min(total_loss, max_loss);

        // Apply as node loss (reduce volume)
        nodes.volume[uni] -= total_loss * dt;
        if (nodes.volume[uni] < 0.0) nodes.volume[uni] = 0.0;
    }
}

} // namespace exfil
} // namespace openswmm
