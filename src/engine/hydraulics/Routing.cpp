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
#include "../core/UnitConversion.hpp"
#include "Outfall.hpp"
#include "Divider.hpp"
#include "Node.hpp"
#include "Link.hpp"
#include "TopoSort.hpp"
#include "../core/SimulationContext.hpp"

#include <cmath>
#include <algorithm>

namespace openswmm {

// ============================================================================
// File-local helper: build XSectParams from link SoA data.
// Same implementation as KinematicWave.cpp::buildXSP_KW and
// HydStructures.cpp::buildXSP — each translation unit keeps its own copy
// because the function is tiny and tightly coupled to the SoA layout.
// ============================================================================

static XSectParams buildXSP(const LinkData& links, std::size_t uk) {
    XSectParams xs{};
    auto ls = links.xsect_shape[uk];
    xs.type   = (ls == XsectShape::DUMMY) ? 0 : static_cast<int>(ls) + 1;
    xs.y_full = links.xsect_y_full[uk];
    xs.a_full = links.xsect_a_full[uk];
    xs.w_max  = links.xsect_w_max[uk];
    xs.r_full = links.xsect_r_full[uk];
    xs.s_full = links.xsect_s_full[uk];
    xs.s_max  = links.xsect_s_max[uk];
    xs.y_bot  = links.xsect_y_bot[uk];
    xs.a_bot  = links.xsect_a_bot[uk];
    xs.s_bot  = links.xsect_s_bot[uk];
    xs.r_bot  = links.xsect_r_bot[uk];
    return xs;
}

// ============================================================================
// Init
// ============================================================================

void Router::init(SimulationContext& ctx, RouteModel model) {
    model_ = model;

    int n_links = ctx.n_links();
    int n_nodes = ctx.n_nodes();

    // Build XSectParams array from ctx.links SoA fields
    // NOTE: LinkData::XsectShape and XSectBatch::XSectShape have different
    //       orderings. LinkData follows legacy enums.h (CIRCULAR=0), while
    //       XSectBatch prepends DUMMY=0 and reorders some shapes.
    //       Use explicit mapping to avoid misalignment.
    auto translateShape = [](XsectShape link_shape) -> int {
        switch (link_shape) {
            case XsectShape::CIRCULAR:        return static_cast<int>(XSectShape::CIRCULAR);
            case XsectShape::FILLED_CIRCULAR: return static_cast<int>(XSectShape::FILLED_CIRCULAR);
            case XsectShape::RECT_CLOSED:     return static_cast<int>(XSectShape::RECT_CLOSED);
            case XsectShape::RECT_OPEN:       return static_cast<int>(XSectShape::RECT_OPEN);
            case XsectShape::TRAPEZOIDAL:     return static_cast<int>(XSectShape::TRAPEZOIDAL);
            case XsectShape::TRIANGULAR:      return static_cast<int>(XSectShape::TRIANGULAR);
            case XsectShape::PARABOLIC:       return static_cast<int>(XSectShape::PARABOLIC);
            case XsectShape::POWER:           return static_cast<int>(XSectShape::POWERFUNC);
            case XsectShape::MODBASKETHANDLE: return static_cast<int>(XSectShape::MOD_BASKET);
            case XsectShape::EGGSHAPED:       return static_cast<int>(XSectShape::EGGSHAPED);
            case XsectShape::HORSESHOE:       return static_cast<int>(XSectShape::HORSESHOE);
            case XsectShape::GOTHIC:          return static_cast<int>(XSectShape::GOTHIC);
            case XsectShape::CATENARY:        return static_cast<int>(XSectShape::CATENARY);
            case XsectShape::SEMIELLIPTICAL:  return static_cast<int>(XSectShape::SEMIELLIPTICAL);
            case XsectShape::BASKETHANDLE:    return static_cast<int>(XSectShape::BASKETHANDLE);
            case XsectShape::SEMICIRCULAR:    return static_cast<int>(XSectShape::SEMICIRCULAR);
            case XsectShape::RECT_TRIANG:     return static_cast<int>(XSectShape::RECT_TRIANG);
            case XsectShape::RECT_ROUND:      return static_cast<int>(XSectShape::RECT_ROUND);
            case XsectShape::HORIZ_ELLIPSE:   return static_cast<int>(XSectShape::HORIZ_ELLIPSE);
            case XsectShape::VERT_ELLIPSE:    return static_cast<int>(XSectShape::VERT_ELLIPSE);
            case XsectShape::ARCH:            return static_cast<int>(XSectShape::ARCH);
            case XsectShape::IRREGULAR:       return static_cast<int>(XSectShape::IRREGULAR);
            case XsectShape::CUSTOM:          return static_cast<int>(XSectShape::CUSTOM);
            case XsectShape::FORCE_MAIN:      return static_cast<int>(XSectShape::FORCE_MAIN);
            case XsectShape::STREET_XSECT:    return static_cast<int>(XSectShape::STREET_XSECT);
            case XsectShape::DUMMY:           return static_cast<int>(XSectShape::DUMMY);
            default:                          return static_cast<int>(XSectShape::DUMMY);
        }
    };

    std::vector<XSectParams> xsect_params(static_cast<std::size_t>(n_links));
    for (int j = 0; j < n_links; ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (ctx.links.type[uj] != LinkType::CONDUIT) continue;

        auto& xs = xsect_params[uj];
        xs.type   = translateShape(ctx.links.xsect_shape[uj]);
        // Cache translated shape code to avoid per-timestep switch dispatch
        ctx.links.xsect_batch_shape[uj] = xs.type;
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

    // Recompute conveyance properties accounting for lengthening
    // (legacy conduit_validate adjusts slope and roughness before computing
    //  beta, roughFactor, qFull when conduit is lengthened)
    {
        using constants::PHI;
        using constants::GRAVITY;
        for (int j = 0; j < n_links; ++j) {
            auto uj = static_cast<std::size_t>(j);
            if (ctx.links.type[uj] != LinkType::CONDUIT) continue;

            double L = ctx.links.length[uj];
            double modL = ctx.links.mod_length[uj];
            if (L <= 0.0 || modL <= L) continue;  // not lengthened

            double factor = modL / L;
            double slope_abs = std::fabs(ctx.links.slope[uj]) / factor;
            double roughness = ctx.links.roughness[uj] / std::sqrt(factor);

            // Update conveyance with adjusted slope and roughness
            double beta = PHI * std::sqrt(slope_abs) / roughness;
            ctx.links.beta[uj] = beta;
            ctx.links.rough_factor[uj] = GRAVITY * (roughness / PHI) * (roughness / PHI);
            ctx.links.q_full[uj] = ctx.links.xsect_s_full[uj] * beta;
            ctx.links.q_max[uj]  = ctx.links.xsect_s_max[uj] * beta;
        }
    }

    // Build shape-grouped batch index
    groups_.build(xsect_params.data(), n_links);

    // Attach transect tables for IRREGULAR cross-sections
    groups_.attachTransectTables(ctx);

    // Cache outfall → connecting-conduit mapping so setAllOutfallDepths
    // skips an O(n_links) inner scan on every Picard iteration.
    openswmm::outfall::buildOutfallLinkMap(ctx);

    // Init solvers
    cycle_detected_ = false;
    switch (model_) {
        case RouteModel::KINWAVE: {
            kw_solver_.init(n_links, groups_);
            // Build topological link order for upstream → downstream processing
            std::vector<int> sorted;
            int n_sorted = toposort::sortLinks(ctx.links.node1.data(),
                                               ctx.links.node2.data(),
                                               n_links, n_nodes, sorted);
            // Gap #44: detect routing loop (cycle) — matching legacy ERR_LOOP check
            if (n_sorted < n_links) cycle_detected_ = true;
            kw_solver_.setLinkOrder(sorted);
            break;
        }
        case RouteModel::DYNWAVE:
            dw_solver_.init(n_nodes, n_links, groups_, ctx);
            dw_solver_.head_tol = ctx.options.head_tol;
            dw_solver_.max_trials = ctx.options.max_trials;
            dw_solver_.surcharge_method =
                static_cast<dynwave::SurchargeMethod>(ctx.options.surcharge_method);
            dw_solver_.node_continuity = ctx.options.node_continuity;
            dw_solver_.anderson_accel = ctx.options.anderson_accel;
            break;
        case RouteModel::STEADY: {
            // Build topological link order (same as KW — upstream → downstream)
            int n_sorted = toposort::sortLinks(ctx.links.node1.data(),
                                               ctx.links.node2.data(),
                                               n_links, n_nodes, steady_sorted_links_);
            // Gap #44: detect routing loop (cycle)
            if (n_sorted < n_links) cycle_detected_ = true;
            break;
        }
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
                 double evap_rate,
                 dynwave::DWSolver::NonConduitFlowFunc non_conduit_fn) {
    // 1. Save old states
    saveOldStates(ctx);

    // 2. Init node flows from laterals and losses (includes storage evap)
    initNodeFlows(ctx, dt, evap_rate);

    // 2b. Compute conduit evaporation and seepage loss rates
    computeConduitLosses(ctx, dt, evap_rate);

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
                    v_new = std::max(v_new, 0.0);

                    // Overflow check
                    double full_vol = node::getVolume(ctx.nodes, j, ctx.nodes.full_depth[uj], &ctx.tables,
                        ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units)));
                    if (v_new > full_vol) {
                        ctx.nodes.overflow[uj] = (v_new - full_vol) / dt;
                        v_new = full_vol;
                    }

                    ctx.nodes.volume[uj] = v_new;
                    // Invert volume → depth using node::getDepth (Newton / table lookup)
                    // Matches legacy node_getDepth() in node.c (Gap #12)
                    int us = ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));
                    double d2 = node::getDepth(ctx.nodes, j, v_new, &ctx.tables, us);

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
            // Pass evap_rate so dq6 can be recomputed per Picard iteration (Gap #14)
            dw_solver_.evap_rate = evap_rate;
            iters = dw_solver_.execute(ctx, dt, non_conduit_fn);
            break;

        case RouteModel::STEADY:
            iters = executeSteadyFlow(ctx, dt);
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

double Router::getAdaptiveStep(SimulationContext& ctx,
                                double fixed_step, double courant) {
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

void Router::initNodeFlows(SimulationContext& ctx, double dt, double evap_rate) {
    auto& nodes = ctx.nodes;
    int n = ctx.n_nodes();
    for (int i = 0; i < n; ++i) {
        auto ui = static_cast<std::size_t>(i);

        // Compute storage node losses (evaporation + seepage)
        // (matching legacy node.c storage_getLosses)
        double loss_rate = 0.0;
        if (nodes.type[ui] == NodeType::STORAGE) {
            double stor_evap_rate = evap_rate * nodes.storage_evap_frac[ui];

            // exfil_cfs is pre-computed by ExfilSolver::computeAll() (called
            // before router_.step()) and stored as a volume in storage_exfil_loss.
            // Convert back to a rate for joint capping with evaporation.
            double exfil_cfs = (dt > 0.0) ? nodes.storage_exfil_loss[ui] / dt : 0.0;

            if (stor_evap_rate > 0.0 || nodes.storage_seep_rate[ui] > 0.0 || exfil_cfs > 0.0) {
                double depth = nodes.depth[ui];
                double area = node::getSurfArea(nodes, i, depth, &ctx.tables,
                    ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units)));

                // Evaporation rate over surface area (cfs)
                double evap_cfs = 0.0;
                if (nodes.volume[ui] > constants::FUDGE)
                    evap_cfs = area * stor_evap_rate;

                // Total loss cannot exceed stored volume
                // (also caps the pre-computed exfil rate from ExfilSolver)
                double total_loss = (evap_cfs + exfil_cfs) * dt;
                if (total_loss > nodes.volume[ui] && total_loss > 0.0) {
                    double ratio = nodes.volume[ui] / total_loss;
                    evap_cfs  *= ratio;
                    exfil_cfs *= ratio;
                }

                nodes.storage_evap_loss[ui]  = evap_cfs * dt;
                nodes.storage_exfil_loss[ui] = exfil_cfs * dt;
                loss_rate = evap_cfs + exfil_cfs;
            } else {
                nodes.storage_evap_loss[ui]  = 0.0;
                nodes.storage_exfil_loss[ui] = 0.0;
            }
        }
        nodes.losses[ui] = loss_rate;

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

// ============================================================================
// computeConduitLosses — evaporation and seepage from conduits
// (matching legacy link.c conduit_getLossRate)
// ============================================================================

void Router::computeConduitLosses(SimulationContext& ctx, double dt, double evap_rate) {
    auto& links = ctx.links;
    int n = ctx.n_links();

    for (int j = 0; j < n; ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (links.type[uj] != LinkType::CONDUIT) continue;

        double depth = 0.5 * (links.old_depth[uj] + links.depth[uj]);
        double evap_loss = 0.0;
        double seep_loss = 0.0;

        if (depth > constants::FUDGE) {
            double length = links.length[uj];
            int batch_shape = links.xsect_batch_shape[uj];

            // Evaporation for open conduits only
            if (xsect::isOpen(batch_shape) && evap_rate > 0.0) {
                double top_width = 0.0;
                // IRREGULAR/CUSTOM shapes: getWofY doesn't have transect
                // table access in per-element mode, so use w_max scaled by
                // depth fraction as approximation. For standard shapes, use
                // the proper geometric dispatch.
                if (batch_shape == static_cast<int>(XSectShape::IRREGULAR) ||
                    batch_shape == static_cast<int>(XSectShape::CUSTOM)) {
                    double y_full = links.xsect_y_full[uj];
                    double w_max  = links.xsect_w_max[uj];
                    // Linear interpolation: w ≈ w_max * (depth / y_full)
                    // (conservative estimate for natural channels)
                    top_width = (y_full > 0.0) ? w_max * std::min(depth / y_full, 1.0) : 0.0;
                } else {
                    XSectParams xs{};
                    xs.type   = batch_shape;
                    xs.y_full = links.xsect_y_full[uj];
                    xs.a_full = links.xsect_a_full[uj];
                    xs.w_max  = links.xsect_w_max[uj];
                    top_width = xsect::getWofY(xs, depth);
                }
                evap_loss = top_width * length * evap_rate;
            }

            // Seepage loss
            if (links.seep_rate[uj] > 0.0) {
                XSectParams xs{};
                xs.type   = batch_shape;
                xs.y_full = links.xsect_y_full[uj];
                xs.a_full = links.xsect_a_full[uj];
                xs.w_max  = links.xsect_w_max[uj];
                xs.yw_max = links.xsect_yw_max[uj];

                double d_seep = depth;
                // Limit depth to depth at max width (matching legacy)
                if (batch_shape == static_cast<int>(XSectShape::RECT_CLOSED))
                    ; // use wMax directly
                else if (d_seep >= xs.yw_max)
                    d_seep = xs.yw_max;
                double width = xsect::getWofY(xs, d_seep);
                seep_loss = links.seep_rate[uj] * width * length;
            }

            // Limit total to available volume (DW) or flow (other models)
            double total = evap_loss + seep_loss;
            if (total > 0.0) {
                double q_avail = (model_ == RouteModel::DYNWAVE)
                    ? links.volume[uj] / dt
                    : std::fabs(links.flow[uj]);
                if (total > q_avail && q_avail >= 0.0) {
                    double ratio = q_avail / total;
                    evap_loss *= ratio;
                    seep_loss *= ratio;
                }
            }
        }

        links.evap_loss_rate[uj] = evap_loss;
        links.seep_loss_rate[uj] = seep_loss;
    }
}

// ============================================================================
// executeSteadyFlow — Gap #33
// Matches legacy steadyflow_execute() in flowrout.c.
// For each conduit (in topological order):
//   q = upstream_inflow / barrels - loss_rate
//   s = q / beta  →  a = getAofS(xs, s)     (Manning normal-depth area)
//   a is the same at both inlet and outlet (uniform steady state)
// Non-conduits pass inflow through unchanged.
// ============================================================================

int Router::executeSteadyFlow(SimulationContext& ctx, double dt) {
    auto& links = ctx.links;
    auto& nodes = ctx.nodes;

    // Fall back to natural order if topo sort is empty (shouldn't happen)
    const auto& order = steady_sorted_links_.empty()
        ? [&]() -> const std::vector<int>& {
            static thread_local std::vector<int> fallback;
            int nl = ctx.n_links();
            fallback.resize(static_cast<std::size_t>(nl));
            for (int j = 0; j < nl; ++j) fallback[static_cast<std::size_t>(j)] = j;
            return fallback;
          }()
        : steady_sorted_links_;

    for (int idx = 0; idx < static_cast<int>(order.size()); ++idx) {
        int j   = order[static_cast<std::size_t>(idx)];
        auto uj = static_cast<std::size_t>(j);

        // Non-conduit links: outflow equals upstream node inflow (pass-through).
        // Matches legacy steadyflow_execute() else branch: *qout = *qin.
        if (links.type[uj] != LinkType::CONDUIT) {
            int n1 = links.node1[uj];
            if (n1 >= 0) {
                double q = nodes.inflow[static_cast<std::size_t>(n1)];
                links.flow[uj] = q;
                int n2 = links.node2[uj];
                if (n2 >= 0) nodes.inflow[static_cast<std::size_t>(n2)] += q;
            }
            continue;
        }

        // DUMMY cross-section: zero area, pass flow through.
        if (links.xsect_shape[uj] == XsectShape::DUMMY) {
            int n1 = links.node1[uj];
            int n2 = links.node2[uj];
            if (n1 >= 0) {
                double q = nodes.inflow[static_cast<std::size_t>(n1)];
                links.flow[uj] = q;
                if (n2 >= 0) nodes.inflow[static_cast<std::size_t>(n2)] += q;
            }
            links.depth[uj]  = 0.0;
            links.volume[uj] = 0.0;
            continue;
        }

        // Gather inflow at upstream node, limited by available volume.
        // Matches legacy getLinkInflow → node_getMaxOutflow.
        int n1 = links.node1[uj];
        double qin = 0.0;
        if (n1 >= 0) {
            auto un1 = static_cast<std::size_t>(n1);
            qin = nodes.inflow[un1];
            double q_max = node::getMaxOutflow(nodes, n1, qin, dt);
            qin = std::min(qin, q_max);
        }

        double barrels = static_cast<double>(std::max(links.barrels[uj], 1));
        double q       = qin / barrels;

        // Subtract pre-computed conduit loss rate (evap + seep per barrel).
        // Matches legacy link_getLossRate call in steadyflow_execute().
        double loss_rate = links.evap_loss_rate[uj] + links.seep_loss_rate[uj];
        q -= loss_rate;
        if (q < 0.0) q = 0.0;

        // Build cross-section params once (used for getAofS and getYofA below).
        XSectParams xs = buildXSP(links, uj);

        // Manning normal-depth area.
        double q_full = links.q_full[uj];
        double a_full = links.xsect_a_full[uj];
        double a;
        if (q >= q_full) {
            // Cap at full flow; adjust qin to reflect the cap.
            // Matches legacy: (*qin) = q * barrels when q > qFull.
            q   = q_full;
            a   = a_full;
            qin = q * barrels;
        } else {
            // s = q / beta  →  a = xsect::getAofS(xs, s)
            // Matches legacy: s = q / beta; a1 = xsect_getAofS(&xsect, s).
            double beta = links.beta[uj];
            if (beta > 0.0) {
                double s = q / beta;
                a = xsect::getAofS(xs, s);
            } else {
                a = 0.0;
            }
        }

        // Steady state: same area (and flow) at both inlet and outlet.
        double qout = q * barrels;
        links.flow[uj] = qout;

        // Update node flows.
        if (n1 >= 0) nodes.outflow[static_cast<std::size_t>(n1)] += qin;
        int n2 = links.node2[uj];
        if (n2 >= 0) nodes.inflow[static_cast<std::size_t>(n2)] += qout;

        // Update link depth and volume.
        // In steady state a1 == a2, so depth and volume are uniform.
        double y = xsect::getYofA(xs, a);
        double length = links.mod_length[uj];
        if (length <= 0.0) length = links.length[uj];

        links.depth[uj]  = y;
        links.volume[uj] = a * length * barrels;

        // Gap #57: steady flow — same area at both ends, so both full or neither.
        links.full_state[uj] = (a_full > 0.0 && a >= a_full) ? int8_t{3} : int8_t{0};

        // Update non-storage end-node depths (max of current and conduit end).
        // Matches legacy setNewLinkState → updateNodeDepth in flowrout.c.
        auto updateNodeDepth = [&](int ni, double y_conduit, double link_offset) {
            if (ni < 0) return;
            auto uni = static_cast<std::size_t>(ni);
            NodeType nt = nodes.type[uni];
            if (nt == NodeType::STORAGE) return;
            double y_node = y_conduit + link_offset;
            if (nt != NodeType::OUTFALL && nodes.overflow[uni] > 0.0)
                y_node = nodes.full_depth[uni];
            if (nodes.depth[uni] < y_node) {
                double full_d = nodes.full_depth[uni];
                nodes.depth[uni] = (full_d > 0.0) ? std::min(y_node, full_d) : y_node;
                nodes.head[uni]  = nodes.invert_elev[uni] + nodes.depth[uni];
            }
        };
        updateNodeDepth(n1, y, links.offset1[uj]);
        updateNodeDepth(n2, y, links.offset2[uj]);
    }

    return 1;  // steady flow always converges in one pass
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
