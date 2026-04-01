/**
 * @file Routing.cpp
 * @brief Top-level routing dispatcher.
 *
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "Routing.hpp"
#include "../core/Constants.hpp"
#include "Outfall.hpp"
#include "Divider.hpp"
#include "Node.hpp"
#include "../core/SimulationContext.hpp"

#include <cmath>
#include <algorithm>

namespace openswmm {

// ============================================================================
// Init
// ============================================================================

void Router::init(SimulationContext& ctx, RouteModel model) {
    model_ = model;

    int n_links = ctx.n_links();
    int n_nodes = ctx.n_nodes();

    // Build XSectParams array from ctx.links SoA fields
    // NOTE: LinkData::XsectShape (legacy ordering: CIRCULAR=0) must be
    //       translated to XSectBatch::XSectShape (DUMMY=0, CIRCULAR=1).
    //       The batch enum prepends DUMMY=0, so batch_shape = link_shape + 1
    //       for all standard shapes (CIRCULAR through STREET_XSECT).
    //       LinkData::DUMMY(22) maps to XSectBatch::DUMMY(0).
    auto translateShape = [](XsectShape link_shape) -> int {
        if (link_shape == XsectShape::DUMMY)
            return static_cast<int>(XSectShape::DUMMY);  // 0
        return static_cast<int>(link_shape) + 1;
    };

    std::vector<XSectParams> xsect_params(static_cast<std::size_t>(n_links));
    for (int j = 0; j < n_links; ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (ctx.links.type[uj] != LinkType::CONDUIT) continue;

        auto& xs = xsect_params[uj];
        xs.type   = translateShape(ctx.links.xsect_shape[uj]);
        xs.y_full = ctx.links.xsect_y_full[uj];
        xs.a_full = ctx.links.xsect_a_full[uj];
        xs.w_max  = ctx.links.xsect_w_max[uj];
        xs.r_full = ctx.links.xsect_r_full[uj];
        xs.s_full = ctx.links.xsect_s_full[uj];

        // Shape-specific parameters for batch kernels
        xs.y_bot  = ctx.links.xsect_y_bot[uj];   // bottom width (TRAP), bottom depth (FILLED_CIRC)
        xs.a_bot  = ctx.links.xsect_a_bot[uj];   // bottom area
        xs.s_bot  = ctx.links.xsect_s_bot[uj];   // side slope (TRAP), C-factor (FORCE_MAIN)
        xs.r_bot  = ctx.links.xsect_r_bot[uj];   // side slope2, wetted perimeter

        // Compute full-depth properties if not already set
        if (xs.a_full == 0.0 && xs.y_full > 0.0) {
            double p[4] = {xs.y_full, xs.w_max, 0, 0};
            xsect::setParams(xs, xs.type, p, 1.0);
            // Write back computed values to link SoA
            ctx.links.xsect_a_full[uj] = xs.a_full;
            ctx.links.xsect_r_full[uj] = xs.r_full;
            ctx.links.xsect_s_full[uj] = xs.s_full;
            ctx.links.xsect_w_max[uj]  = xs.w_max;
        }
    }

    // Compute modified conduit lengths for CFL stability
    // (matching legacy conduit_getLengthFactor in link.c)
    {
        using constants::PHI;
        using constants::GRAVITY;
        double route_step = ctx.options.routing_step;
        double lengthening_step = ctx.options.lengthening_step;
        double tStep = (lengthening_step > 0.0)
                        ? std::min(route_step, lengthening_step)
                        : route_step;

        for (int j = 0; j < n_links; ++j) {
            auto uj = static_cast<std::size_t>(j);
            if (ctx.links.type[uj] != LinkType::CONDUIT) {
                ctx.links.mod_length[uj] = ctx.links.length[uj];
                continue;
            }

            double L = ctx.links.length[uj];
            if (L <= 0.0) { ctx.links.mod_length[uj] = L; continue; }

            double yFull = ctx.links.xsect_y_full[uj];
            double aFull = ctx.links.xsect_a_full[uj];
            double sFull = ctx.links.xsect_s_full[uj];
            double n_rough = ctx.links.roughness[uj];
            double slope_abs = std::fabs(ctx.links.slope[uj]);

            if (aFull > 0.0 && n_rough > 0.0 && slope_abs > 0.0) {
                double vFull = PHI / n_rough * sFull * std::sqrt(slope_abs) / aFull;
                double ratio = (std::sqrt(GRAVITY * yFull) + vFull) * tStep / L;
                double factor = (ratio > 1.0) ? ratio : 1.0;
                ctx.links.mod_length[uj] = factor * L;
            } else {
                ctx.links.mod_length[uj] = L;
            }
        }
    }

    // Build shape-grouped batch index
    groups_.build(xsect_params.data(), n_links);

    // Attach transect tables for IRREGULAR cross-sections
    groups_.attachTransectTables(ctx);

    // Init solvers
    switch (model_) {
        case RouteModel::KINWAVE:
            kw_solver_.init(n_links, groups_);
            break;
        case RouteModel::DYNWAVE:
            dw_solver_.init(n_nodes, n_links, groups_);
            break;
        case RouteModel::STEADY:
            break;
    }

    // Build divider SoA from node data
    {
        int nd = 0;
        for (int i = 0; i < n_nodes; ++i)
            if (ctx.nodes.type[static_cast<std::size_t>(i)] == NodeType::DIVIDER) ++nd;
        dividers_.resize(nd);
        int d = 0;
        for (int i = 0; i < n_nodes && d < nd; ++i) {
            auto ui = static_cast<std::size_t>(i);
            if (ctx.nodes.type[ui] != NodeType::DIVIDER) continue;
            auto ud = static_cast<std::size_t>(d);
            dividers_.node_idx[ud]      = i;
            dividers_.method[ud]        = static_cast<int>(ctx.nodes.divider_type[ui]);
            dividers_.cutoff_flow[ud]   = ctx.nodes.divider_cutoff[ui];
            dividers_.weir_cd[ud]       = ctx.nodes.divider_cd[ui];
            dividers_.weir_max_depth[ud]= ctx.nodes.divider_max_depth[ui];
            dividers_.table_idx[ud]     = ctx.nodes.divider_curve[ui];
            dividers_.div_link_idx[ud]  = ctx.nodes.divider_link[ui];
            ++d;
        }
    }
}

// ============================================================================
// Step
// ============================================================================

int Router::step(SimulationContext& ctx, double dt,
                 dynwave::DWSolver::NonConduitFlowFunc non_conduit_fn) {
    // 1. Save old states
    saveOldStates(ctx);

    // 2. Init node flows from laterals and losses
    initNodeFlows(ctx, dt);

    // 3. Set outfall boundary depths (P8-G03)
    outfall::setAllOutfallDepths(ctx, ctx.current_date);

    // 4. Dispatch to solver
    int iters = 0;
    switch (model_) {
        case RouteModel::KINWAVE:
            iters = kw_solver_.execute(ctx, dt);

            // Storage node iterative update for KW (P8-G07)
            // After KW routing, storage nodes need depth from volume balance
            for (int j = 0; j < ctx.n_nodes(); ++j) {
                auto uj = static_cast<std::size_t>(j);
                if (ctx.nodes.type[uj] != NodeType::STORAGE) continue;

                double v_old = ctx.nodes.old_volume[uj];
                double q_in = ctx.nodes.inflow[uj];
                double q_out = 0.0;
                // Sum outgoing link flows
                for (int k = 0; k < ctx.n_links(); ++k) {
                    auto uk = static_cast<std::size_t>(k);
                    if (ctx.links.node1[uk] == j && ctx.links.flow[uk] > 0.0)
                        q_out += ctx.links.flow[uk];
                    if (ctx.links.node2[uk] == j && ctx.links.flow[uk] < 0.0)
                        q_out -= ctx.links.flow[uk];
                }

                // Successive approximation (omega=0.55, max 10 iters)
                double d1 = ctx.nodes.depth[uj];
                for (int iter = 0; iter < 10; ++iter) {
                    double v_new = v_old + (q_in - q_out) * dt;
                    if (v_new < 0.0) v_new = 0.0;

                    // Overflow check
                    double full_vol = node::getVolume(ctx.nodes, j, ctx.nodes.full_depth[uj], &ctx.tables);
                    if (v_new > full_vol) {
                        ctx.nodes.overflow[uj] = (v_new - full_vol) / dt;
                        v_new = full_vol;
                    }

                    ctx.nodes.volume[uj] = v_new;
                    // Invert volume → depth
                    // Simple linear for now; full: Newton inversion
                    double d2 = (full_vol > 0.0)
                        ? ctx.nodes.full_depth[uj] * (v_new / full_vol)
                        : 0.0;

                    // Under-relaxation
                    d2 = 0.45 * d1 + 0.55 * d2;
                    if (std::fabs(d2 - d1) < 0.005) { d1 = d2; break; }
                    d1 = d2;
                }
                ctx.nodes.depth[uj] = d1;
                ctx.nodes.head[uj] = ctx.nodes.invert_elev[uj] + d1;
            }
            break;

        case RouteModel::DYNWAVE:
            iters = dw_solver_.execute(ctx, dt, non_conduit_fn);
            break;

        case RouteModel::STEADY:
            for (int j = 0; j < ctx.n_links(); ++j) {
                auto uj = static_cast<std::size_t>(j);
                ctx.links.flow[uj] = ctx.links.old_flow[uj];
            }
            iters = 1;
            break;
    }

    // 5. Compute divider flows (P8-G02)
    divider::computeDividerFlows(ctx, dividers_);

    // 6. Update link final states (depth, volume)
    updateLinkStates(ctx);

    return iters;
}

// ============================================================================
// getAdaptiveStep
// ============================================================================

double Router::getAdaptiveStep(const SimulationContext& ctx,
                                double fixed_step, double courant) const {
    if (model_ == RouteModel::DYNWAVE) {
        return dw_solver_.getRoutingStep(ctx, fixed_step, courant);
    }
    return fixed_step;
}

// ============================================================================
// Internal helpers
// ============================================================================

void Router::saveOldStates(SimulationContext& ctx) {
    ctx.nodes.save_state();
    ctx.links.save_state();
}

void Router::initNodeFlows(SimulationContext& ctx, double dt) {
    auto& nodes = ctx.nodes;
    int n = ctx.n_nodes();
    for (int i = 0; i < n; ++i) {
        auto ui = static_cast<std::size_t>(i);

        // Initialize inflow from lateral flow and outflow from losses
        // (matching legacy node.c node_initFlows lines 320-321)
        nodes.inflow[ui]  = nodes.lat_flow[ui];
        nodes.outflow[ui] = nodes.losses[ui];

        // Set overflow from excess stored volume
        // (matching legacy node.c node_initFlows lines 324-326)
        if (nodes.volume[ui] > nodes.full_volume[ui] && dt > 0.0) {
            nodes.overflow[ui] = (nodes.volume[ui] - nodes.full_volume[ui]) / dt;
        } else {
            nodes.overflow[ui] = 0.0;
        }
    }
}

void Router::updateLinkStates(SimulationContext& ctx) {
    auto& links = ctx.links;

    // Batch-compute depths from areas for all links
    // (area_mid is stored in the DW solver; for KW, compute from a1/a2)
    // For now: depth and volume are set directly by the solvers

    // Update node heads
    node::computeHeads(ctx.nodes.invert_elev.data(),
                       ctx.nodes.depth.data(),
                       ctx.nodes.head.data(),
                       ctx.n_nodes());
}

} // namespace openswmm
