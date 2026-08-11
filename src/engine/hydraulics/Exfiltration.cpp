/**
 * @file Exfiltration.cpp
 * @brief Storage exfiltration — numerically identical to legacy exfil.c.
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "Exfiltration.hpp"
#include "Node.hpp"
#include "../core/SimulationContext.hpp"
#include "../core/UnitConversion.hpp"
#include "../data/StorageGeometry.hpp"
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
        // Relational refactor (Phase 3): exfil conductivity from the side-table
        // (ExfilSolver::init runs after node_subtypes.build), wide fallback otherwise.
        const int sr = ctx.node_subtypes.storage_row(i);
        const double ksat = (sr >= 0)
            ? ctx.node_subtypes.storages.exfil_ksat[static_cast<size_t>(sr)] : 0.0;
        if (nodes.type[ui] == NodeType::STORAGE && ksat > 0.0) {
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
        // Relational refactor (Phase 3): exfil Green-Ampt params from the
        // side-table (fresh at init), with a wide-array fallback.
        const int sr = ctx.node_subtypes.storage_row(i);
        const double exf_suction = (sr >= 0)
            ? ctx.node_subtypes.storages.exfil_suction[static_cast<size_t>(sr)] : 0.0;
        const double exf_ksat = (sr >= 0)
            ? ctx.node_subtypes.storages.exfil_ksat[static_cast<size_t>(sr)] : 0.0;
        const double exf_imd = (sr >= 0)
            ? ctx.node_subtypes.storages.exfil_imd[static_cast<size_t>(sr)] : 0.0;
        if (nodes.type[ui] != NodeType::STORAGE || exf_ksat <= 0.0) {
            continue;
        }

        auto uk = static_cast<size_t>(k);
        soa_.node_idx[uk] = i;

        // --- Initialize Green-Ampt states for bottom and bank
        //     Uses the same soil parameters for both (matching legacy createStorageExfil)
        infil::grnampt_init(soa_.btm_ga[uk],
                            exf_suction,
                            exf_ksat,
                            exf_imd,
                            ctx.options);
        infil::grnampt_init(soa_.bank_ga[uk],
                            exf_suction,
                            exf_ksat,
                            exf_imd,
                            ctx.options);

        // --- Compute bottom area and bank geometry from storage shape
        //     Storage geometry from the side-table (sr from above), wide fallback.
        int curve_idx = (sr >= 0)
            ? ctx.node_subtypes.storages.curve[static_cast<size_t>(sr)] : -1;

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

            // Note: legacy converts TABULAR areas/depths to internal units
            // here (exfil.c:126-129, inside the TABULAR case only).

        } else {
            // --- FUNCTIONAL: area = A * depth^B + C
            //     Legacy: exfil_initState() FUNCTIONAL case (exfil.c:134-141)
            //     Bottom area: at depth=0, area = A*0^B + C = C
            //     Exception: if B==0 (exponent is zero), area = A*1 + C = A+C
            //     PARITY QUIRK: legacy does NOT unit-convert the FUNCTIONAL
            //     bottom area (its /UCF at exfil.c:126-129 is inside the
            //     TABULAR case only), so the raw USER-unit coefficients are
            //     used directly — storage coefficients stay user-unit
            //     end-to-end (see PostParseResolver), matching this quirk.
            double a_coeff = (sr >= 0) ? ctx.node_subtypes.storages.a[static_cast<size_t>(sr)] : 0.0;
            double b_coeff = (sr >= 0) ? ctx.node_subtypes.storages.b[static_cast<size_t>(sr)] : 0.0;
            double c_coeff = (sr >= 0) ? ctx.node_subtypes.storages.c[static_cast<size_t>(sr)] : 0.0;
            const StorageShape sshape = (sr >= 0)
                ? ctx.node_subtypes.storages.shape[static_cast<size_t>(sr)]
                : StorageShape::FUNCTIONAL;

            double btm = c_coeff;
            if (storage_shape_is_geometric(sshape)) {
                // Geometric shapes: the area relation is the quadratic c + a*d + b*d²,
                // so the bottom area is simply c — the "+a when b == 0" rule above is a
                // property of the power law and must NOT be applied here.
                // Legacy exfil.c:143-150 spells this out for CYLINDRICAL/CONICAL/
                // PYRAMIDAL (btmArea = a0, no addend). PARABOLOID has no case there at
                // all, leaving btmArea at 0 — which is the same answer, since a
                // paraboloid's c is 0 (it comes to a point at the invert). Routing it
                // through the FUNCTIONAL rule instead would add a1 = π·A·B/Z and give a
                // paraboloid a spurious bottom area.
            } else if (b_coeff == 0.0) {
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

            // Cap bank area at bank_max_area (matching legacy exfil.c line 191:
            // area = MIN(area, exfil->bankMaxArea))
            double area = openswmm::node::getSurfArea(ctx.nodes, soa_.node_idx[uk], depth,
                                            &ctx.tables,
                                            openswmm::ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units)),
                                            &ctx.node_subtypes);
            double bank_area = std::min(area, soa_.bank_max_area[uk]);
            double bank_rate = infil::grnampt_getInfil(soa_.bank_ga[uk], 0.0, bank_depth, dt);
            total_loss += bank_rate * bank_area;
        }

        // Limit to available volume
        double max_loss = nodes.volume[uni] / dt;
        total_loss = std::min(total_loss, max_loss);

        // Write pre-computed exfil volume (ft3) into the side-table for
        // Router::initNodeFlows. Volume is reduced through the routing continuity
        // equation (nodes.losses) rather than here, so that evap + exfil are
        // jointly capped to available storage before advancing the timestep.
        const int sr = ctx.node_subtypes.storage_row(static_cast<int>(uni));
        if (sr >= 0)
            ctx.node_subtypes.storages.exfil_loss[static_cast<std::size_t>(sr)] = total_loss * dt;
    }
}

} // namespace exfil
} // namespace openswmm
