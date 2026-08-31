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
 * @file Routing.cpp
 * @brief Top-level routing dispatcher.
 *
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "Routing.hpp"
#include "fv/ExplicitFvSolver.hpp"
#include "fv/NetworkMeshBuilder.hpp"
#include "fv/NetworkSolverFactory.hpp"
#include "../core/Constants.hpp"
#include "../core/ErrorCodes.hpp"
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
    // (matching legacy link.c conduit_getLengthFactor / conduit_validate:
    //  lengthening is only applied when LENGTHENING_STEP > 0 in OPTIONS)
    {
        using constants::PHI;
        using constants::GRAVITY;
        double route_step = ctx.options.routing_step;
        double lengthening_step = ctx.options.lengthening_step;

        auto& CD = ctx.link_subtypes.conduits;  // Phase 6 Stage B: mod_length authority
        if (lengthening_step <= 0.0) {
            // Legacy: skip Courant lengthening when LENGTHENING_STEP not set
            for (int r = 0; r < CD.count(); ++r) {
                const auto ur = static_cast<std::size_t>(r);
                CD.mod_length[ur] = CD.length[ur];
            }
        } else {
            double tStep = std::min(route_step, lengthening_step);

            for (int j = 0; j < n_links; ++j) {
                auto uj = static_cast<std::size_t>(j);
                if (ctx.links.type[uj] != LinkType::CONDUIT) continue;
                const int cr = ctx.link_subtypes.conduit_row(j);  // ≥0 (conduit)
                const auto ucr = static_cast<std::size_t>(cr);

                double L = CD.length[ucr];
                if (L <= 0.0) {
                    CD.mod_length[ucr] = L;
                    continue;
                }

                double yFull = ctx.links.xsect_y_full[uj];
                double aFull = ctx.links.xsect_a_full[uj];
                double sFull = ctx.links.xsect_s_full[uj];
                double n_rough = CD.roughness[ucr];
                double slope_abs = std::fabs(CD.slope[ucr]);

                // For open channels, use hydraulic depth (aFull / top-width)
                // rather than geometric full depth for the wave-speed term.
                // Matches legacy link.c:1241-1243:
                //   if (xsect_isOpen(type)) yFull = aFull / getWofY(yFull)
                bool is_open = (ctx.links.xsect_shape[uj] == XsectShape::TRAPEZOIDAL ||
                                ctx.links.xsect_shape[uj] == XsectShape::RECT_OPEN   ||
                                ctx.links.xsect_shape[uj] == XsectShape::TRIANGULAR  ||
                                ctx.links.xsect_shape[uj] == XsectShape::PARABOLIC);
                if (is_open) {
                    double wFull = ctx.links.xsect_w_max[uj];
                    if (wFull > 0.0) yFull = aFull / wFull;
                }

                if (aFull > 0.0 && n_rough > 0.0 && slope_abs > 0.0) {
                    double vFull = PHI / n_rough * sFull * std::sqrt(slope_abs) / aFull;
                    double ratio = (std::sqrt(GRAVITY * yFull) + vFull) * tStep / L;
                    double factor = (ratio > 1.0) ? ratio : 1.0;
                    CD.mod_length[ucr] = factor * L;
                } else {
                    CD.mod_length[ucr] = L;
                }
            }
        }
    }

    // Recompute conveyance properties accounting for lengthening
    // (legacy conduit_validate adjusts slope and roughness before computing
    //  beta, roughFactor, qFull when conduit is lengthened)
    {
        using constants::PHI;
        using constants::GRAVITY;
        auto& CD = ctx.link_subtypes.conduits;  // Phase 6 Stage B: lengthened conveyance
        for (int j = 0; j < n_links; ++j) {
            auto uj = static_cast<std::size_t>(j);
            if (ctx.links.type[uj] != LinkType::CONDUIT) continue;
            const int cr = ctx.link_subtypes.conduit_row(j);
            const auto ucr = static_cast<std::size_t>(cr);

            double L = CD.length[ucr];
            double modL = CD.mod_length[ucr];
            if (L <= 0.0 || modL <= L) continue;  // not lengthened

            double factor = modL / L;
            double slope_abs = std::fabs(CD.slope[ucr]) / factor;
            double roughness = CD.roughness[ucr] / std::sqrt(factor);

            // Update conveyance with adjusted slope and roughness
            double beta = PHI * std::sqrt(slope_abs) / roughness;
            CD.beta[ucr]         = beta;
            // PARITY link.c:1133: GRAVITY * SQR(roughness/PHI) — square first.
            CD.rough_factor[ucr] = GRAVITY * ((roughness / PHI) * (roughness / PHI));
            CD.q_full[ucr]       = ctx.links.xsect_s_full[uj] * beta;
            CD.q_max[ucr]        = ctx.links.xsect_s_max[uj] * beta;
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
        case RouteModel::DYNWAVE: {
            // Configure the solver from options BEFORE init(): init() allocates
            // surcharge-method-specific state — e.g. the Dynamic Preissmann Slot
            // (DPS) arrays are resized only when surcharge_method ==
            // DYNAMIC_SLOT. Setting the method after init() left those arrays
            // empty and segfaulted in applyDPSGeometry. EXTRAN (the default) is
            // unaffected since it needs no extra allocation.
            //
            // HEAD_TOLERANCE is entered in project length units (ft for US,
            // m for SI).  Legacy dynwave_init converts the user value to
            // internal feet via `HeadTol /= UCF(LENGTH)` (dynwave.c:183); the
            // compiled default (DEFAULT_HEAD_TOL, already in feet) is left as-is.
            // Skipping this made an SI model's 0.0015 m tolerance act as
            // 0.0015 ft (~3.3× too tight) → chronic non-convergence.
            //
            // A deck may also carry an explicit ZERO for either option.
            // Legacy treats zero as "use the compiled default"
            // (dynwave.c:182-184):
            //     if (HeadTol == 0.0)  HeadTol  = DEFAULT_HEADTOL;
            //     if (MaxTrials == 0)  MaxTrials = DEFAULT_MAXTRIALS;
            // and SWMM itself writes those zeros — project.c:897-898 sets
            // MaxTrials/HeadTol to 0 precisely to "force use of default", so
            // decks saved by the GUI ship them. Without the substitution
            // `while (t_steps < max_trials)` runs ZERO Picard iterations: every
            // link flow and node depth stays at its initial value for the whole
            // simulation, silently, with no error and a clean mass balance.
            // 362 of the 1,396 corpus decks (26%) carry MAX_TRIALS 0.
            const double ucf_len = ucf::Ucf[ucf::LENGTH][
                ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units))];
            dw_solver_.head_tol =
                (ctx.options.head_tol == 0.0
                 || ctx.options.head_tol == constants::DEFAULT_HEAD_TOL)
                    ? constants::DEFAULT_HEAD_TOL
                    : ctx.options.head_tol / ucf_len;
            dw_solver_.max_trials = (ctx.options.max_trials > 0)
                    ? ctx.options.max_trials
                    : constants::DEFAULT_MAX_TRIALS;
            dw_solver_.surcharge_method =
                static_cast<dynwave::SurchargeMethod>(ctx.options.surcharge_method);
            dw_solver_.node_continuity = ctx.options.node_continuity;
            dw_solver_.anderson_accel = ctx.options.anderson_accel;
            // Unsteady friction (issue #156): dimensionless, no unit
            // conversion.
            dw_solver_.unsteady_friction = ctx.options.unsteady_friction;
            dw_solver_.uf_k3             = ctx.options.uf_k3;
            dw_solver_.tpa_celerity      = ctx.options.tpa_celerity;  // #156

            dw_solver_.init(n_nodes, n_links, groups_, ctx);
            break;
        }
        case RouteModel::FV: {
            initFv(ctx);
            break;
        }
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

    // Divider data now lives in the relational side-table
    // ctx.node_subtypes.dividers, built once at hydraulics init (after this
    // Router::init returns). computeDividerFlows() reads it directly, so the
    // former local DividerSoA build has been removed.
    // See docs/relational/RELATIONAL_NODE_REFACTOR_PLAN.md (Phase 2).
}

// ============================================================================
// Step
// ============================================================================

int Router::step(SimulationContext& ctx, double dt,
                 double evap_rate,
                 dynwave::DWSolver::NonConduitFlowFunc non_conduit_fn) {
    // 1. Old states are saved ONCE per step by SWMMEngine::step() BEFORE
    //    lateral inflows are assembled — matching legacy routing_execute,
    //    which snapshots old hyd state / oldLatFlow (initSystemInflows,
    //    routing.c:431) before addSystemInflows fills the new laterals.
    //    A second save here clobbered old_lat_flow with the CURRENT step's
    //    just-assembled laterals, corrupting the reported (interpolated)
    //    NODE_LATFLOW while leaving solver state untouched. Standalone
    //    Router users (unit tests) must call ctx.nodes/links.save_state()
    //    themselves before step().

    // 2. Init node flows from laterals and losses (includes storage evap)
    initNodeFlows(ctx, dt, evap_rate);

    // 2b. Compute conduit evaporation and seepage loss rates.
    // For DYNWAVE this is done PER Picard iteration inside DWSolver::execute
    // (recomputeConduitLosses), matching legacy dwflow which calls
    // link_getLossRate every iteration with a flow-class gate. Computing it
    // once here (start-of-step, ungated) would leak loss onto dry/up-dry
    // conduits and freeze the rate. KINWAVE/STEADY are non-iterative, so the
    // once-per-step computation matches their legacy behavior.
    if (model_ != RouteModel::DYNWAVE)
        computeConduitLosses(ctx, dt, evap_rate);

    // 3. Set outfall boundary depths (P8-G03)
    outfall::setAllOutfallDepths(ctx, ctx.current_date);

    // 4. Dispatch to solver
    int iters = 0;
    switch (model_) {
        case RouteModel::KINWAVE:
            // Storage nodes are converged INSIDE the sorted-link loop, before
            // the links they drain through are routed (legacy flowrout.c:183).
            // A post-pass cannot do it: an outlet structure's flow depends on
            // the very depth the pass is trying to find.
            iters = kw_solver_.execute(ctx, dt, structures_);
            break;

        case RouteModel::DYNWAVE:
            // Pass evap_rate so dq6 can be recomputed per Picard iteration (Gap #14)
            dw_solver_.evap_rate = evap_rate;
            iters = dw_solver_.execute(ctx, dt, non_conduit_fn);
            break;

        case RouteModel::FV:
            iters = stepFv(ctx, dt, non_conduit_fn);
            break;

        case RouteModel::STEADY:
            iters = executeSteadyFlow(ctx, dt);
            break;
    }

    // 5. Compute divider flows (P8-G02)
    divider::computeDividerFlows(ctx, ctx.node_subtypes.dividers);

    // 6. Update link final states (depth, volume)
    //
    // NOT under FV: publishFv has already written every node's head, and an
    // FV head is PIEZOMETRIC. `head = invert + depth` cannot express one that
    // sits below the node's own invert, because the published depth is a
    // water depth floored at zero — so recomputing here silently replaced it
    // with the invert. Measured on the issue #156 TPA sealed-drawdown
    // fixture: the apex head reached 3.5 ft inside the step (invert 6.0) and
    // every reader outside the solver saw exactly 6.0, which is what made the
    // sub-atmospheric gate unfalsifiable. For every node whose head is at or
    // above its invert the two agree exactly, so this changes nothing else.
    //
    // The other routers still need it: they set depth as the primary variable
    // and derive head from it, and DYNWAVE's own head writes happen per
    // Picard pass rather than once at the end.
    if (ctx.options.routing_model != RoutingModel::FV)
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
    if (model_ == RouteModel::FV && fv_solver_) {
        // The FV solver substeps internally at its own CFL limit, so the
        // ROUTING step is only the reporting/forcing cadence — there is no
        // stability reason to shrink it. Publishing the substep census as the
        // suggestion anyway lets VARIABLE_STEP models see the same signal DW
        // gives them, and keeps a routing step from spanning so many substeps
        // that structure flows (held constant across them, D-FV3) go stale.
        const double sug = fv_solver_->suggested_step();
        if (courant > 0.0 && sug > 0.0) {
            const double cap = std::max(fixed_step * courant, sug);
            return std::min(fixed_step, cap);
        }
    }
    return fixed_step;
}

// ============================================================================
// Internal helpers
// ============================================================================

void Router::initNodeFlows(SimulationContext& ctx, double dt, double evap_rate) {
    auto& nodes = ctx.nodes;
    int n = ctx.n_nodes();
    for (int i = 0; i < n; ++i) {
        auto ui = static_cast<std::size_t>(i);

        // Compute storage node losses (evaporation + seepage)
        // (matching legacy node.c storage_getLosses)
        double loss_rate = 0.0;
        if (nodes.type[ui] == NodeType::STORAGE) {
            // Relational refactor (Phase 4): storage config (evap fraction, seep
            // rate) and per-step losses live in the dense side-table; storage_row
            // is valid for any STORAGE node.
            const int sr = ctx.node_subtypes.storage_row(i);
            auto& st = ctx.node_subtypes.storages;
            const auto sru = static_cast<std::size_t>(sr);
            const double evap_frac = (sr >= 0) ? st.evap_frac[sru] : 0.0;
            const double seep_rate = (sr >= 0) ? st.seep_rate[sru] : 0.0;
            double stor_evap_rate = evap_rate * evap_frac;

            // exfil_cfs is pre-computed by ExfilSolver::computeAll() (called before
            // router_.step()) and stored as a volume in the side-table's exfil_loss.
            // Convert back to a rate for joint capping with evaporation.
            double exfil_cfs = 0.0;
            if (dt > 0.0 && sr >= 0)
                exfil_cfs = st.exfil_loss[sru] / dt;

            if (stor_evap_rate > 0.0 || seep_rate > 0.0 || exfil_cfs > 0.0) {
                double depth = nodes.depth[ui];
                double area = node::getSurfArea(nodes, i, depth, &ctx.tables,
                    ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units)),
                    &ctx.node_subtypes);

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

                if (sr >= 0) {
                    st.evap_loss[sru]  = evap_cfs * dt;
                    st.exfil_loss[sru] = exfil_cfs * dt;
                }
                loss_rate = evap_cfs + exfil_cfs;
            } else if (sr >= 0) {
                st.evap_loss[sru]  = 0.0;
                st.exfil_loss[sru] = 0.0;
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
        auto& CD = ctx.link_subtypes.conduits;
        const auto ucr = static_cast<std::size_t>(ctx.link_subtypes.conduit_row(j));

        double depth = 0.5 * (links.old_depth[uj] + links.depth[uj]);
        double evap_loss = 0.0;
        double seep_loss = 0.0;

        if (depth > constants::FUDGE) {
            // Use RAW user-input length (matching legacy
            // `link.c::conduit_getLossRate` line 1358:
            //   length = conduit_getLength(j);
            // which returns `Conduit[k].length` (raw) for non-IRREGULAR
            // conduits — NOT `Conduit[k].modLength` (lengthened).
            // The lengthened length appears only in the momentum equation
            // (`dwflow.c:133`), not in loss-volume accounting.
            double length = CD.length[ucr];
            if (length <= 0.0) length = CD.mod_length[ucr];
            int batch_shape = links.xsect_batch_shape[uj];

            // Evaporation for open conduits only.
            //
            // NOTE (IRREGULAR conduit evap, user2/user5): the cross-section
            // GEOMETRY is bit-faithful to legacy (verified: extran8a's IRREGULAR
            // conduits are byte-identical; buildTables + lookup are 1:1 ports).
            // The residual divergence is NOT geometry — it is the conduit-loss
            // ACCOUNTING timing: legacy computes conduit evap PER PICARD ITERATION
            // inside dwflow_findConduitFlow (using the current-iteration depth +
            // the DW volume cap, and skipping bypassed conduits), whereas this
            // runs ONCE per step before the Picard loop using the start-of-step
            // depth. On natural channels this seeds a ~1e-5 cfs difference that
            // the surcharge dynamics amplify. Reproducing legacy bit-for-bit
            // requires moving conduit-loss evaluation into the iteration loop.
            // The transect top-width below uses the linear approximation; the
            // faithful getWofY was tried and does NOT resolve the parity gap
            // (the gap is the timing, not the width). See PARITY_FINDINGS.
            if (xsect::isOpen(batch_shape) && evap_rate > 0.0) {
                double top_width = 0.0;
                if (batch_shape == static_cast<int>(XSectShape::IRREGULAR) ||
                    batch_shape == static_cast<int>(XSectShape::CUSTOM)) {
                    double y_full = links.xsect_y_full[uj];
                    double w_max  = links.xsect_w_max[uj];
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
            if (CD.seep_rate[ucr] > 0.0) {
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
                seep_loss = CD.seep_rate[ucr] * width * length;
            }

            // Limit the total to what is actually there. A volume-tracking
            // solver caps on the water it holds; KINWAVE/STEADY have no conduit
            // volume state of their own and cap on the flow instead. FV holds
            // volume, so capping it on |flow| meant standing water in a conduit
            // with no flow never evaporated at all.
            double total = evap_loss + seep_loss;
            if (total > 0.0) {
                double q_avail = (model_ == RouteModel::DYNWAVE ||
                                  model_ == RouteModel::FV)
                    ? links.volume[uj] / dt
                    : std::fabs(links.flow[uj]);
                if (total > q_avail && q_avail >= 0.0) {
                    double ratio = q_avail / total;
                    evap_loss *= ratio;
                    seep_loss *= ratio;
                }
            }
        }

        CD.evap_loss_rate[ucr] = evap_loss;
        CD.seep_loss_rate[ucr] = seep_loss;
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

    steady_storage_updated_.assign(static_cast<std::size_t>(ctx.n_nodes()), 0);

    for (int idx = 0; idx < static_cast<int>(order.size()); ++idx) {
        int j   = order[static_cast<std::size_t>(idx)];
        auto uj = static_cast<std::size_t>(j);

        // PARITY flowrout.c:181-183 — converge a storage unit's depth before
        // routing the links it drains through (see the KINWAVE case).
        {
            int n1s = links.node1[uj];
            if (n1s >= 0
                && nodes.type[static_cast<std::size_t>(n1s)] == NodeType::STORAGE
                && !steady_storage_updated_[static_cast<std::size_t>(n1s)]) {
                kinwave::updateStorageState(ctx, structures_, order, idx, n1s, dt);
                steady_storage_updated_[static_cast<std::size_t>(n1s)] = 1;
            }
        }

        // Non-conduit links are not routed — steadyflow_execute returns
        // *qout = *qin for them — but their INFLOW is their own head-discharge
        // relation, not the upstream node's inflow (legacy getLinkInflow).
        if (links.type[uj] != LinkType::CONDUIT) {
            int n1 = links.node1[uj];
            int n2 = links.node2[uj];
            double q = kinwave::getLinkInflow(ctx, structures_, j, dt);
            links.flow[uj] = q;
            if (n1 >= 0) nodes.outflow[static_cast<std::size_t>(n1)] += q;
            if (n2 >= 0) nodes.inflow[static_cast<std::size_t>(n2)] += q;
            continue;
        }
        auto& CD = ctx.link_subtypes.conduits;
        const auto ucr = static_cast<std::size_t>(ctx.link_subtypes.conduit_row(j));

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
            qin = (nodes.type[un1] == NodeType::STORAGE)
                ? kinwave::getLinkInflow(ctx, structures_, j, dt)
                : nodes.inflow[un1];
            double q_max = node::getMaxOutflow(nodes, n1, qin, dt);
            qin = std::min(qin, q_max);
        }

        double barrels = static_cast<double>(std::max(CD.barrels[ucr], 1));
        double q       = qin / barrels;

        // Subtract pre-computed conduit loss rate (evap + seep per barrel).
        // Matches legacy link_getLossRate call in steadyflow_execute().
        double loss_rate = CD.evap_loss_rate[ucr] + CD.seep_loss_rate[ucr];
        q -= loss_rate;
        if (q < 0.0) q = 0.0;

        // Build cross-section params once (used for getAofS and getYofA below).
        XSectParams xs = buildXSP(links, uj);

        // Manning normal-depth area.
        double q_full = CD.q_full[ucr];
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
            double beta = CD.beta[ucr];
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
        double length = CD.mod_length[ucr];
        if (length <= 0.0) length = CD.length[ucr];

        links.depth[uj]  = y;
        links.volume[uj] = a * length * barrels;

        // Gap #57: steady flow — same area at both ends, so both full or neither.
        CD.full_state[ucr] = (a_full > 0.0 && a >= a_full) ? int8_t{3} : int8_t{0};

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

// ============================================================================
// Explicit finite-volume routing (FLOW_ROUTING FV)
// ============================================================================

/**
 * @brief Build the FV mesh, pick a backend and seed the solver state.
 *
 * Called from Router::init AFTER the Courant-lengthening block has written
 * mod_length/rough_factor: the mesh reuses that lengthening as its Δx floor and
 * the friction closure as its rough_factor, so the two must already agree.
 */
void Router::initFv(SimulationContext& ctx) {
    fv_warnings_.clear();
    fv_errors_.clear();

    // FV_* keys are entered in project display units; convert to internal feet
    // here, the same treatment HEAD_TOLERANCE gets in the DYNWAVE arm above.
    const double ucf_len = ucf::Ucf[ucf::LENGTH][
        ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units))];
    fv_opts_ = ctx.options.fv;
    fv_opts_.cell_length   = ctx.options.fv.cell_length   / ucf_len;
    fv_opts_.slot_celerity = ctx.options.fv.slot_celerity / ucf_len;
    fv_opts_.dispersion    = ctx.options.fv.dispersion    / (ucf_len * ucf_len);
    // Unsteady friction (issue #156): shared [OPTIONS] keys, dimensionless —
    // copied here so the solver keeps seeing only FvOptions.
    fv_opts_.unsteady_friction = ctx.options.unsteady_friction;
    fv_opts_.uf_k3             = ctx.options.uf_k3;

    // Options that are parsed and stored but reach nothing. Saying so at open
    // is the whole point: a model calibrated with FV_DISPERSION set would
    // otherwise run as if the value had been honoured.
    if (ctx.options.fv.dispersion != 0.0)
        fv_warnings_.push_back(format_warning(WARN_FV_OPTION_INERT,
                                              "FV_DISPERSION"));
    if (ctx.options.fv.scalar_scheme != fv::ScalarScheme::MUSCL)
        fv_warnings_.push_back(format_warning(WARN_FV_OPTION_INERT,
                                              "FV_SCALAR_SCHEME"));

    // Dynamic wave options that carry no meaning for an explicit solver. FV
    // takes one step rather than iterating, resolves transcritical flow
    // natively, and picks its own substep from the Courant limit.
    if (ctx.options.inertial_damping != 1)
        fv_warnings_.push_back(format_warning(WARN_DW_OPTION_UNDER_FV,
                                              "INERTIAL_DAMPING"));
    if (ctx.options.surcharge_method != 0)
        fv_warnings_.push_back(format_warning(WARN_DW_OPTION_UNDER_FV,
                                              "SURCHARGE_METHOD"));
    if (ctx.options.normal_flow_ltd != 2)
        fv_warnings_.push_back(format_warning(WARN_DW_OPTION_UNDER_FV,
                                              "NORMAL_FLOW_LIMITED"));

    const fv::MeshBuildReport rep =
        fv::buildNetworkMesh(ctx, fv_opts_, fv_mesh_);
    fv_warnings_.insert(fv_warnings_.end(), rep.warnings.begin(),
                        rep.warnings.end());
    fv_errors_   = rep.errors;
    if (!rep.errors.empty()) return;

    const int nc = fv_mesh_.n_cells();
    const int nn = fv_mesh_.n_nodes();
    fv_state_.resize(nc, nn, 0);

    // Seed cell state from the model's initial condition. LinkData carries ONE
    // depth per conduit, and that depth is the average of the two end-node
    // DEPTHS — which are measured from different inverts. Laying it down as a
    // uniform depth is therefore only the right projection on a level bed; over
    // a sloping one a uniform depth IS a sloping free surface, which is not at
    // rest, and over a shoreline it is worse than that.
    //
    // On the SWASHES emerged lake-at-rest (a 0.1 m lake split by a bank rising
    // to 0.15 m) the ramp conduit averages the wet bank's 0.1 with the dry
    // crest's 0 and lays 0.05 over a mid-bed of 0.075: a surface at 0.125, i.e.
    // 25 mm of water perched on dry ground. It slumps, and because that pool has
    // no outlet the excess never leaves — the lake settles at 0.102747 m against
    // an analytic 0.1, which is (0.8 + 0.009 + 0.05 + 0.075)/9.09 to six
    // figures. The whole error, and it is an initial condition, not a scheme:
    // the pool it produces is flat and well balanced, just at the wrong height.
    //
    // So where one bank is DRY, project the free SURFACE instead: a dry node
    // reports depth 0 and therefore defines no surface at all, so there is
    // nothing to average — the wet end carries its surface across and each cell
    // takes the depth that leaves over its own bed.
    //
    // Only where one bank is dry. With both ends wet the link's depth is an
    // average of two real surfaces, and which projection is right depends on a
    // regime the deck does not state: an at-rest pool wants a level surface, a
    // channel at normal depth wants a uniform depth parallel to the bed, and a
    // pressurized main wants a linear HGL. Overriding that case is not a fix but
    // a different guess — measured, it starts a 30.09 cfs transient in a force
    // main whose steady flow is the 25 cfs being pumped in. So it is left alone.
    for (int r = 0; r < fv_mesh_.n_conduits(); ++r) {
        const auto ur = static_cast<std::size_t>(r);
        const int begin = fv_mesh_.conduit_cell_begin[ur];
        const int count = fv_mesh_.conduit_cell_count[ur];
        if (begin < 0) continue;
        const int j = fv_mesh_.conduit_link[ur];
        const auto uj = static_cast<std::size_t>(j);
        const fv::FvGeometry& g = fv_mesh_.geom[ur];
        // areaOfDepth already returns the AGGREGATE section of all barrels, and
        // links.flow is the aggregate discharge, so neither is divided here.
        const double q = ctx.links.flow[uj];

        // Surface at each end, where that end has water to define one.
        auto bank_eta = [&](int nd, double& eta) {
            if (nd < 0) return false;
            const auto und = static_cast<std::size_t>(nd);
            if (ctx.nodes.depth[und] <= 0.0) return false;
            eta = ctx.nodes.invert_elev[und] + ctx.nodes.depth[und];
            return true;
        };
        double eta1 = 0.0, eta2 = 0.0;
        const bool wet1 = bank_eta(ctx.links.node1[uj], eta1);
        const bool wet2 = bank_eta(ctx.links.node2[uj], eta2);
        // ...and only across a BED STEP. Two depths measured from the same
        // invert average to something meaningful; two measured from different
        // inverts do not, and only the latter can perch water on a dry bank. On
        // a level conduit the average is not merely harmless but right: it is
        // the cell mean of a discontinuity, which is what a dam break puts
        // there. Overriding it moves Ritter's dam half a cell downstream and
        // costs 4 % of the L1 depth error (0.0522 -> 0.0544).
        const bool step = (fv_mesh_.cell_dzdx[static_cast<std::size_t>(begin)]
                           != 0.0);
        const bool shoreline = (wet1 != wet2) && step;
        const double eta_wet = wet1 ? eta1 : eta2;

        const double uniform =
            fv::kernels::areaOfDepth(g, ctx.links.depth[uj]);

        double vol = 0.0, a_len = 0.0, sum_len = 0.0, slot_vol = 0.0;
        for (int c = begin; c < begin + count; ++c) {
            const auto uc = static_cast<std::size_t>(c);
            double a_c = uniform;
            if (shoreline) {
                a_c = fv::kernels::areaOfDepth(
                    g, std::max(0.0, eta_wet - fv_mesh_.cell_zb[uc]));
            }
            fv_state_.cell_a[uc] = a_c;
            // A cell the projection leaves dry carries no discharge either:
            // keeping q on it would divide by the area floor on the first
            // refresh and launch a velocity that was never in the model.
            fv_state_.cell_q[uc] = (a_c > fv::kernels::kDryArea) ? q : 0.0;

            const double dx = fv_mesh_.cell_dx[uc];
            sum_len  += dx;
            a_len    += a_c * dx;
            vol      += a_c * dx;
            slot_vol += std::max(0.0, a_c - g.a_crown) * dx;
        }

        // Publish what was actually seeded, by the same reduction publishFv
        // uses. initMassBalance books the run's initial storage from
        // links.volume AFTER this returns, so without it the ledger opens on the
        // DW-shaped volume while the solver integrates the projected one, and
        // the difference is reported as water lost during the run: on the
        // emerged lake-at-rest that is the dry ramp cells, 688 ft^3 = 0.016
        // acre-feet, reported as a 17 % continuity error on a model where
        // nothing moves and nothing leaves.
        if (sum_len > 0.0) {
            ctx.links.volume[uj]      = vol;
            ctx.links.slot_volume[uj] = slot_vol;
            // R0b: published depth truncated to y_full (see publishFv).
            ctx.links.depth[uj]  = std::min(
                fv::kernels::depthOfArea(g, a_len / sum_len), g.y_full);
        }
    }
    for (int n = 0; n < nn; ++n) {
        const auto un = static_cast<std::size_t>(n);
        fv_state_.node_head[un] = ctx.nodes.invert_elev[un] + ctx.nodes.depth[un];
    }

    fv_solver_ = fv::makeNetworkSolver(fv_opts_, &fv_backend_, nc);

    // The per-substep refresh is a callback into HydStructures, which a device
    // backend cannot reach. Say so rather than letting the option go quiet.
    if (fv_opts_.structure_coupling == fv::StructureCoupling::SUBSTEP &&
        !dynamic_cast<fv::ExplicitFvSolver*>(fv_solver_.get())) {
        fv_opts_.structure_coupling = fv::StructureCoupling::ROUTING_STEP;
        fv_warnings_.push_back(format_warning(
            WARN_FV_OPTION_INERT,
            "FV_STRUCTURE_COUPLING SUBSTEP on the '" + fv_backend_ +
                "' backend, which cannot call the structure equations; "
                "clamped to ROUTING_STEP"));
    }
    fv_solver_->initialize(fv_mesh_, fv_state_, fv_opts_);

    fv_lateral_.assign(static_cast<std::size_t>(nn), 0.0);
    fv_fixed_head_.assign(static_cast<std::size_t>(nn),
                          std::numeric_limits<double>::quiet_NaN());
    fv_struct_flow_.assign(static_cast<std::size_t>(ctx.n_links()), 0.0);
    fv_cond_loss_.assign(static_cast<std::size_t>(fv_mesh_.n_conduits()), 0.0);
    fv_struct_int_.assign(static_cast<std::size_t>(ctx.n_links()), 0.0);
}

/**
 * @brief One FV routing step: assemble forcing, advance, publish.
 *
 * Structure equations are evaluated here, against the current node heads, and
 * under FV_STRUCTURE_COUPLING SUBSTEP (the default) re-evaluated at the top of
 * every substep through the forcing's refresh hook. That hook is a callback
 * into the engine, so it exists only on the CPU path: a device backend cannot
 * reach HydStructures and clamps to ROUTING_STEP.
 *
 * Control RULES are not re-evaluated — they run on the engine's own rule step
 * and are tuned for DW-scale cadence; only the head-dependent discharge of the
 * structures they set is refreshed.
 */
int Router::stepFv(SimulationContext& ctx, double dt,
                   dynwave::DWSolver::NonConduitFlowFunc non_conduit_fn) {
    if (!fv_solver_) return 0;

    const int nn = ctx.n_nodes();
    const int nl = ctx.n_links();

    // Lateral inflows exactly as assembled by SWMMEngine::assembleLateralInflows.
    // Virtual junctions cannot carry them (rule 5), so they never appear here.
    for (int n = 0; n < nn; ++n) {
        const auto un = static_cast<std::size_t>(n);
        // Net of the node's own losses. `nodes.losses` carries storage
        // evaporation and Green-Ampt exfiltration, computed by the SHARED
        // Router::initNodeFlows — the dynamic-wave solver drains it by seeding
        // nodes.outflow with it, and publishFv reports it as outflow, but
        // nothing ever removed it from the FV node's water. The mass balance
        // was charged for water the solver still held, a continuity error
        // scaling with the number of storage units that lose.
        fv_lateral_[un] = ctx.nodes.lat_flow[un] - ctx.nodes.losses[un];
        // Outfalls (and anything else the engine pins) are stage boundaries:
        // setAllOutfallDepths has already written the head for this step.
        fv_fixed_head_[un] =
            (ctx.nodes.type[un] == NodeType::OUTFALL)
                ? ctx.nodes.invert_elev[un] + ctx.nodes.depth[un]
                : std::numeric_limits<double>::quiet_NaN();
    }

    // Non-conduit structures, evaluated against the CURRENT node heads.
    auto evaluate_structures = [&]() {
        if (non_conduit_fn) non_conduit_fn(ctx, dt, 0);
        for (int j = 0; j < nl; ++j) {
            const auto uj = static_cast<std::size_t>(j);
            fv_struct_flow_[uj] = (ctx.links.type[uj] == LinkType::CONDUIT)
                                      ? 0.0 : ctx.links.flow[uj];
        }
    };
    std::fill(fv_struct_int_.begin(), fv_struct_int_.end(), 0.0);
    fv_struct_t_prev_ = 0.0;
    evaluate_structures();

    // Distributed conduit losses as a rate per unit length (ft²/s): the solver
    // subtracts them from the cell AREA, so the per-barrel volumetric rate has
    // to be divided by the conduit's meshed length.
    for (int r = 0; r < fv_mesh_.n_conduits(); ++r) {
        const auto ur = static_cast<std::size_t>(r);
        const auto& CD = ctx.link_subtypes.conduits;
        const int begin = fv_mesh_.conduit_cell_begin[ur];
        if (begin < 0) { fv_cond_loss_[ur] = 0.0; continue; }
        // computeConduitLosses returns the rate for ONE barrel while the cell
        // carries the aggregate section, so the loss is charged for all of
        // them — which is also what the mass balance books
        // (SWMMEngine.cpp:3270, rate x barrels). Dividing by the count instead
        // made a two-barrel conduit shed a quarter of its seepage.
        const double rate = (CD.evap_loss_rate[ur] + CD.seep_loss_rate[ur]) *
                            static_cast<double>(std::max(1, CD.barrels[ur]));
        const double len  = fv_mesh_.cell_dx[static_cast<std::size_t>(begin)] *
                            static_cast<double>(fv_mesh_.conduit_cell_count[ur]);
        fv_cond_loss_[ur] = (len > 0.0) ? rate / len : 0.0;
    }

    fv::FvStepForcing forcing;
    forcing.node_lateral    = fv_lateral_.data();
    forcing.node_fixed_head = fv_fixed_head_.data();
    forcing.structure_flow  = fv_struct_flow_.data();
    forcing.conduit_loss    = fv_cond_loss_.data();
    forcing.n_nodes = nn;
    forcing.n_links = nl;

    // Per-substep refresh (FV_STRUCTURE_COUPLING SUBSTEP). The structures read
    // ctx.nodes.head, which the solver has not published mid-advance, so the
    // hook publishes the heads it is holding, re-runs the SAME shared code the
    // dynamic wave solver calls — setAllOutfallDepths for the stage boundaries
    // and the non-conduit callback for the structures — and refills the two
    // forcing arrays in place.
    struct RefreshCtx {
        Router*            self;
        SimulationContext* ctx;
        const std::function<void()>* eval;
    };
    std::function<void()> eval_fn = evaluate_structures;
    RefreshCtx rctx{this, &ctx, &eval_fn};
    if (fv_opts_.structure_coupling == fv::StructureCoupling::SUBSTEP) {
        forcing.refresh_user = &rctx;
        forcing.refresh = [](void* user, double t_elapsed) {
            auto* r = static_cast<RefreshCtx*>(user);
            r->self->refreshFvBoundaryFlows(*r->ctx, t_elapsed, *r->eval);
        };
    }

    fv_solver_->advance(0.0, dt, forcing);
    publishFv(ctx, dt);
    return static_cast<int>(fv_solver_->last_num_steps());
}

// ============================================================================
// accumulateStructureFlows — time-weighted structure discharge
// ============================================================================

void Router::accumulateStructureFlows(const SimulationContext& ctx, double t_now) {
    const double span = t_now - fv_struct_t_prev_;
    if (span <= 0.0) return;
    for (int j = 0; j < ctx.n_links(); ++j) {
        const auto uj = static_cast<std::size_t>(j);
        if (ctx.links.type[uj] == LinkType::CONDUIT) continue;
        fv_struct_int_[uj] += fv_struct_flow_[uj] * span;
    }
    fv_struct_t_prev_ = t_now;
}

// ============================================================================
// refreshFvBoundaryFlows — mid-advance re-evaluation of head-dependent forcing
// ============================================================================

void Router::refreshFvBoundaryFlows(SimulationContext& ctx, double t_elapsed,
                                    const std::function<void()>& eval_structures) {
    auto* impl = dynamic_cast<fv::ExplicitFvSolver*>(fv_solver_.get());
    if (!impl) return;

    // Bank the discharge that has been in force since the last refresh, before
    // replacing it. The solver applied exactly this over exactly this span.
    accumulateStructureFlows(ctx, t_elapsed);

    // Publish the heads the solver is holding so the structure equations and
    // the outfall stage relations see the state they are being asked about.
    // Depth and head only — this is not publishFv: flows, volumes and the node
    // ledger belong to the end of the step.
    const int nn = ctx.n_nodes();
    for (int n = 0; n < nn; ++n) {
        const auto un = static_cast<std::size_t>(n);
        if (fv_mesh_.node_kind[un] == fv::kNodeVirtual) continue;
        const double depth = fv_state_.node_head[un] - fv_mesh_.node_invert[un];
        ctx.nodes.depth[un] = std::max(0.0, depth);
        ctx.nodes.head[un]  = fv_state_.node_head[un];
    }

    // Stage boundaries first — a FREE or NORMAL outfall's depth is a function
    // of the conduit that feeds it, so it moves within the step exactly as the
    // structures do.
    outfall::setAllOutfallDepths(ctx, ctx.current_date);
    for (int n = 0; n < nn; ++n) {
        const auto un = static_cast<std::size_t>(n);
        if (ctx.nodes.type[un] != NodeType::OUTFALL) continue;
        fv_fixed_head_[un] = ctx.nodes.invert_elev[un] + ctx.nodes.depth[un];
    }

    eval_structures();
}

/**
 * @brief Map cell/node state back onto the per-link and per-node reporting
 *        contracts (plan §4.3).
 *
 * Link flow is the length-weighted MEAN over the routing step, not an
 * end-of-step snapshot: a routing step can span hundreds of substeps and the
 * snapshot aliases badly against them. Depth and volume are instantaneous,
 * matching what DW reports.
 */
void Router::publishFv(SimulationContext& ctx, double dt) {
    auto* impl = dynamic_cast<fv::ExplicitFvSolver*>(fv_solver_.get());
    const int us = ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));

    // ---- links ------------------------------------------------------------
    for (int r = 0; r < fv_mesh_.n_conduits(); ++r) {
        const auto ur = static_cast<std::size_t>(r);
        const int begin = fv_mesh_.conduit_cell_begin[ur];
        const int count = fv_mesh_.conduit_cell_count[ur];
        if (begin < 0 || count <= 0) continue;
        const int j = fv_mesh_.conduit_link[ur];
        const auto uj = static_cast<std::size_t>(j);
        const fv::FvGeometry& g = fv_mesh_.geom[ur];
        // Cell area and discharge are ALREADY the aggregate of all barrels
        // (FvGeometry::barrel_scale), so nothing here is scaled by the count.
        double sum_len = 0.0, q_len = 0.0, a_len = 0.0, vol = 0.0;
        double slot_vol = 0.0;
        for (int c = begin; c < begin + count; ++c) {
            const auto uc = static_cast<std::size_t>(c);
            const double dx = fv_mesh_.cell_dx[uc];
            const double q = (impl && dt > 0.0)
                                 ? impl->cell_flow_integral()[uc] / dt
                                 : fv_state_.cell_q[uc];
            sum_len += dx;
            q_len   += q * dx;
            a_len   += fv_state_.cell_a[uc] * dx;
            vol     += fv_state_.cell_a[uc] * dx;
            // Slot share of the cell's storage: area above a_crown is water
            // standing in the Preissmann slot (the crown-band taper's tiny
            // content lives inside a_crown and is deliberately not counted).
            slot_vol += std::max(0.0, fv_state_.cell_a[uc] - g.a_crown) * dx;
        }
        const double q_mean = (sum_len > 0.0) ? q_len / sum_len : 0.0;
        const double a_mean = (sum_len > 0.0) ? a_len / sum_len : 0.0;

        ctx.links.flow[uj]   = q_mean;
        // Slot program R0b: the PUBLISHED depth is a water depth, truncated
        // to the pipe's true max height — matching the dynamic-wave
        // convention (DynamicWave.cpp links.depth = min(depth_mid, yf)).
        // Above the crown the piezometric surplus lives in node heads and
        // links.slot_volume; the .out depth column stops doubling as a head
        // channel, and stat_max_filling reads ≤ 1 like legacy. The clamp is
        // publish-only: every internal use of the mean state works from
        // a_mean, and widthOfDepth(y_full) == widthOfDepth(piezo) == t_slot,
        // so the derived hyd-depth/Froude below are unchanged.
        ctx.links.depth[uj]  = std::min(
            fv::kernels::depthOfArea(g, a_mean), g.y_full);
        // Inlet control was applied inside the solver, at the culvert's
        // upstream face; only the report flag comes back out here.
        if (impl) {
            const int cr = ctx.link_subtypes.conduit_row(j);
            if (cr >= 0)
                ctx.link_subtypes.conduits.inlet_control[
                    static_cast<std::size_t>(cr)] = impl->inlet_control()[ur];
        }
        ctx.links.volume[uj]      = vol;
        ctx.links.slot_volume[uj] = slot_vol;
        // Conveyance velocity: above the crown the conserved area carries
        // Preissmann-slot content that conveys no momentum, so Q/a_mean is
        // slot-diluted — the same rationale as the implicit face law
        // (PressurizedHeadSolver). Open sections have no slot; their area
        // above y_full is genuine conveyance and stays uncapped.
        const double a_conv = (!g.is_open && a_mean > g.a_crown) ? g.a_crown
                                                                 : a_mean;
        const double v = (a_conv > 0.0) ? q_mean / a_conv : 0.0;
        const double hyd_depth =
            (fv::kernels::widthOfDepth(g, ctx.links.depth[uj]) > 0.0)
                ? a_conv / fv::kernels::widthOfDepth(g, ctx.links.depth[uj])
                : 0.0;
        ctx.links.froude[uj] =
            (hyd_depth > 0.0)
                ? std::fabs(v) * constants::INV_SQRT_GRAVITY / std::sqrt(hyd_depth)
                : 0.0;

        // Flow class and the end-full flags. Neither was ever written under FV,
        // so the Flow Classification Summary read 100 % dry on every model and
        // the Conduit Surcharge Summary was permanently empty — both plausible
        // and both wrong.
        //
        // The classes FV can honestly report are the dry ones and the
        // sub/supercritical split, read from the cells themselves. UP_CRITICAL
        // and DN_CRITICAL are DW's END-CONDITION patches: FV imposes no
        // critical-depth boundary because the Riemann solver resolves the
        // transition natively, so those columns stay zero — that is the scheme
        // reporting what it does, not a gap.
        const auto uc_first = static_cast<std::size_t>(begin);
        const auto uc_last  = static_cast<std::size_t>(begin + count - 1);
        const bool up_dry = fv_state_.cell_h[uc_first] <= fv::kernels::kDryDepth;
        const bool dn_dry = fv_state_.cell_h[uc_last]  <= fv::kernels::kDryDepth;
        FlowClass fc;
        if (up_dry && dn_dry)   fc = FlowClass::DRY;
        else if (up_dry)        fc = FlowClass::UP_DRY;
        else if (dn_dry)        fc = FlowClass::DN_DRY;
        else fc = (ctx.links.froude[uj] > 1.0) ? FlowClass::SUPERCRITICAL
                                               : FlowClass::SUBCRITICAL;
        ctx.links.flow_class[uj] = fc;

        const int cr_fs = ctx.link_subtypes.conduit_row(j);
        if (cr_fs >= 0) {
            const double yf = g.y_full;
            const int8_t fs = static_cast<int8_t>(
                ((fv_state_.cell_h[uc_first] >= yf) ? 1 : 0) |
                ((fv_state_.cell_h[uc_last]  >= yf) ? 2 : 0));
            ctx.link_subtypes.conduits.full_state[
                static_cast<std::size_t>(cr_fs)] = fs;
        }
    }

    // ---- nodes ------------------------------------------------------------
    for (int n = 0; n < ctx.n_nodes(); ++n) {
        const auto un = static_cast<std::size_t>(n);
        if (fv_mesh_.node_kind[un] == fv::kNodeVirtual) {
            // A virtual junction has no volume of its own — its faces became
            // interior faces. Report the shared face state so the node still
            // has a sensible head in the .out file.
            continue;
        }
        double head = fv_state_.node_head[un];
        // Storage-less clean junctions, ANY degree: publish the
        // face-consistent stage — the same reconstruction the
        // virtual-junction loop below uses — instead of the solver's own
        // head, which sits ~S0·dx/2 above the face stage on steep COARSE
        // chains and steps against the VJ rule in the plotted HGL
        // (mechanism B).
        //
        // Originally this covered degree-2 pass-through junctions only,
        // because that is the set the SOLVER splices. Measured on Example1
        // under FLOW_ROUTING FV, that left two thirds of nodes publishing the
        // raw head and standing up to 0.91 ft above their own adjacent cell
        // at FV_MIN_CELLS 1 — falling ~first-order under refinement, so a
        // datum term, not physics. Nodes inside the set never exceeded
        // 0.0002 ft at any refinement, and the excess survives on nodes with
        // NO lateral inflow, which rules out "head needed to drive the
        // inflow out". See tests/manual/fv_node_stage/RESULTS.md.
        //
        // Culvert-inlet and flap-gate faces are excluded: those node heads
        // are genuine headwaters that must not be reconstructed away. So are
        // nodes carrying a structure flow or an unbled carry.
        //
        // REPORTING only, on purpose: changing the solver-internal head
        // instead destabilized the lake-at-rest diameter-change case through
        // the LTS-hold ghost channel (70 cfs standing oscillation from a
        // 0.007 cfs residual).
        // A dry cell's vote is bare ground, so only wet neighbours vote; the
        // all-dry case keeps the solver's lowest-adjacent-bed contract.
        if (impl && un < impl->node_publish_stage().size() &&
            impl->node_publish_stage()[un]) {
            const int b0 = fv_mesh_.node_face_ptr[un];
            const int e0 = fv_mesh_.node_face_ptr[un + 1];
            double eta = -1.0e30;
            bool wet = false;
            for (int p = b0; p < e0; ++p) {
                const auto uf = static_cast<std::size_t>(
                    fv_mesh_.node_face_idx[static_cast<std::size_t>(p)]);
                const int c = (fv_mesh_.face_cl[uf] >= 0) ? fv_mesh_.face_cl[uf]
                                                          : fv_mesh_.face_cr[uf];
                if (c < 0) continue;
                const auto uc = static_cast<std::size_t>(c);
                if (fv_state_.cell_h[uc] <= fv::kernels::kDryDepth) continue;
                // A face perched ABOVE the node's own water surface is
                // discharging as a free overfall (manual §8.6, ghost state
                // 8-28: "a node standing below the face invert presents a dry
                // ghost"). Its stage describes the pipe, not the node, and
                // letting it vote reads the pipe's OFFSET as node depth --
                // measured on Example1's J11, where conduit C2 enters 4 ft
                // above the invert and an unguarded vote drove the reported
                // average depth from 0.28 ft to 4.11 ft. The solver head is
                // used only as the submergence THRESHOLD here, never as the
                // published value, so its datum bias cannot leak back in.
                if (fv_mesh_.face_zb[uf] > head) continue;
                eta = std::max(eta, std::min(fv_mesh_.cell_zb[uc],
                                             fv_mesh_.face_zb[uf]) +
                                    fv_state_.cell_h[uc]);
                wet = true;
            }
            if (wet) head = eta;
        }
        const double depth = head - fv_mesh_.node_invert[un];
        ctx.nodes.depth[un] = std::max(0.0, depth);
        ctx.nodes.head[un]  = head;
        // Volume through the ENGINE's own storage relation, not the solver's
        // flattened table, so the reported mass balance stays on one authority.
        //
        // ABOVE THE RIM that relation stops describing the node, and DW writes
        // the volume directly there rather than deriving it from depth
        // (getFloodedDepth). FV must do the same or the mass balance misreads
        // the node: the flooding term is only booked while volume <=
        // full_volume, so a surcharged junction whose linear getVolume kept
        // climbing had its flooding silently dropped, and a ponded junction
        // whose retained water was reported as MIN_SURFAREA x depth lost the
        // pond from Final Stored Volume. Both showed up as tens of percent of
        // routing continuity error.
        const double full_d = fv_mesh_.node_full_depth[un];
        if (full_d > 0.0 && ctx.nodes.depth[un] > full_d) {
            ctx.nodes.volume[un] =
                fv_mesh_.node_can_pond[un]
                    ? std::max(fv_state_.node_volume[un], ctx.nodes.full_volume[un])
                    : ctx.nodes.full_volume[un];
        } else {
            ctx.nodes.volume[un] = node::getVolume(ctx.nodes, n, ctx.nodes.depth[un],
                                                   &ctx.tables, us, &ctx.node_subtypes);
        }
        if (impl && dt > 0.0)
            ctx.nodes.overflow[un] = impl->node_flood_volume()[un] / dt;

        // SWMM's node ledger keeps inflow and outflow as separate MAGNITUDES,
        // not a net figure — the mass balance books an outfall's system
        // discharge from `inflow` alone (SWMMEngine's node_getSystemOutflow
        // port), so publishing only the net would make every outfall discharge
        // vanish and drive routing continuity to ~100 %.
        //
        // The magnitudes come from the solver's accumulated BOUNDARY FLUXES,
        // not from the published mean link flow: the face flux at the node is
        // the water that actually crossed into it, which is what continuity
        // has to balance.
        if (impl && dt > 0.0) {
            const double lat = ctx.nodes.lat_flow[un];
            ctx.nodes.inflow[un]  = impl->node_inflow_volume()[un] / dt +
                                    ((lat > 0.0) ? lat : 0.0);
            ctx.nodes.outflow[un] = impl->node_outflow_volume()[un] / dt +
                                    ((lat < 0.0) ? -lat : 0.0) +
                                    ctx.nodes.losses[un];
        }
    }

    // Structure discharge is the routing-step MEAN, for the same reason link
    // flow is: under per-substep coupling the discharge moves within the step,
    // and the solver applied that whole trajectory. Publishing the last
    // substep's sample instead reported a number the mass balance never saw —
    // and it aliased badly, a weir peaking at 12.1 / 14.3 / 7.4 cfs as the mesh
    // refined while the pump and orifice converged smoothly.
    if (dt > 0.0) {
        const_cast<Router*>(this)->accumulateStructureFlows(ctx, dt);
        for (int j = 0; j < ctx.n_links(); ++j) {
            const auto uj = static_cast<std::size_t>(j);
            if (ctx.links.type[uj] == LinkType::CONDUIT) continue;
            ctx.links.flow[uj] = fv_struct_int_[uj] / dt;
        }
    }

    // Non-conduit structures move water between node ledgers exactly as they do
    // under DW (DynamicWave.cpp:2723-2727): positive flow leaves node1 and
    // arrives at node2.
    for (int j = 0; j < ctx.n_links(); ++j) {
        const auto uj = static_cast<std::size_t>(j);
        if (ctx.links.type[uj] == LinkType::CONDUIT) continue;
        const int n1 = ctx.links.node1[uj];
        const int n2 = ctx.links.node2[uj];
        if (n1 < 0 || n2 < 0) continue;
        const double q = ctx.links.flow[uj];
        const auto u1 = static_cast<std::size_t>(n1);
        const auto u2 = static_cast<std::size_t>(n2);
        if (q > 0.0) { ctx.nodes.outflow[u1] += q;  ctx.nodes.inflow[u2]  += q; }
        else         { ctx.nodes.inflow[u1]  -= q;  ctx.nodes.outflow[u2] -= q; }
    }

    // Virtual junctions take the head at their spliced face, reconstructed
    // from BOTH adjacent cells. The face→node association comes from the mesh
    // (face_vj_node, recorded at build); re-deriving it by matching inverts
    // collided whenever two virtual junctions shared a bit-identical invert,
    // leaving the loser permanently unreported (depth 0 for the whole run).
    //
    // Each cell's WSE extrapolated to the face is bracketed by its two
    // first-order models: flat water surface (cell_zb + h — exact when the
    // reach is ponded or pressurized) and uniform depth along the bed
    // (face_zb + h — exact in steep shallow flow, where the flat-surface
    // form from the downhill cell reads below the invert and used to clamp
    // the reported depth to zero). Taking the per-cell MIN of the bracket and
    // the MAX across the two cells reproduces the correct head in all three
    // regimes: the uphill cell's uniform-depth vote carries shallow flow, the
    // downhill cell's flat-surface vote carries ponding and surcharge.
    for (int f = 0; f < fv_mesh_.n_faces(); ++f) {
        const auto uf = static_cast<std::size_t>(f);
        if (!fv_mesh_.face_virtual[uf]) continue;
        const int n = fv_mesh_.face_vj_node[uf];
        if (n < 0) continue;
        const auto un = static_cast<std::size_t>(n);
        const double zf = fv_mesh_.face_zb[uf];
        // TPA (issue #156): a FLAGGED side carries a signed piezometric
        // level cell_zb + cell_h with no zf floor — the station VJ is
        // exactly where E3's sub-atmospheric trace is measured. Unflagged
        // sides keep the legacy wetting convention bit-for-bit.
        double eta = zf;                    // both sides dry ⇒ dry node
        bool any_tpa = false;
        double eta_tpa = 0.0;
        for (const int c : {fv_mesh_.face_cl[uf], fv_mesh_.face_cr[uf]}) {
            if (c < 0) continue;
            const auto uc = static_cast<std::size_t>(c);
            if (!fv_state_.cell_tpa.empty() && fv_state_.cell_tpa[uc] != 0) {
                const double e_t = fv_mesh_.cell_zb[uc] + fv_state_.cell_h[uc];
                eta_tpa = any_tpa ? std::max(eta_tpa, e_t) : e_t;
                any_tpa = true;
                continue;
            }
            eta = std::max(eta, std::min(fv_mesh_.cell_zb[uc], zf) +
                                fv_state_.cell_h[uc]);
        }
        // A TPA side sets the head; an unflagged side only competes when it
        // is genuinely wet above the floor (the bare zf floor must not mask
        // a signed TPA level).
        if (any_tpa) eta = (eta > zf) ? std::max(eta, eta_tpa) : eta_tpa;
        ctx.nodes.head[un]  = eta;
        ctx.nodes.depth[un] = std::max(0.0, eta - fv_mesh_.node_invert[un]);
        ctx.nodes.volume[un] = 0.0;   // zero-storage by construction
    }
}

} // namespace openswmm
