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
 * @file HydStructures.cpp
 * @brief Non-conduit link flow — batch by type, numerically identical to legacy.
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "HydStructures.hpp"
#include "../core/Constants.hpp"
#include "../core/SimulationContext.hpp"
#include "../core/UnitConversion.hpp"
#include "../math/SIMD.hpp"
#include "Divider.hpp"
#include "Node.hpp"
#include "XSectBatch.hpp"
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace openswmm {

// A3 parity tracing: routing-step serial defined in SWMMEngine.cpp, used to
// step-gate the per-orifice term trace (SWMM_TRACE_ORIF + SWMM_TRACE_LSTEP).
extern long g_trace_rstep_sn;

namespace hydstruct {

// PARITY link.c:1836-1845 (orifice), 2266-2275 (weir), 2664-2673 (outlet).
// Legacy reads the tailwater head from the DOWNSTREAM node only under DW:
//     if ( RouteModel == DW ) h2 = Node[n2].newDepth + Node[n2].invertElev;
//     else                    h2 = Node[n1].invertElev;
// Steady and Kinematic Wave routing carry no downstream head — they route on
// a tree layout where a link's flow can only depend on its upstream end — so
// every structure discharges freely to its own upstream invert. Using the
// DW form under KW/SF submerges a structure against whatever depth its
// downstream junction happened to reach, which is not information KW has.
// FV keeps the DW form: it does solve for downstream head.
static inline double tailwaterHead(const SimulationContext& ctx,
                                   std::size_t un1, std::size_t un2) {
    const auto rm = ctx.options.routing_model;
    if (rm == RoutingModel::KINWAVE || rm == RoutingModel::STEADY)
        return ctx.nodes.invert_elev[un1];
    return ctx.nodes.depth[un2] + ctx.nodes.invert_elev[un2];
}

// PARITY link.c:646-673 link_setFlapGate. Both halves, in legacy's order.
bool flapGateBlocks(const SimulationContext& ctx, int j, int n1, int n2, double q) {
    const auto uj = static_cast<size_t>(j);

    // --- reverse flow through the link's own flap gate
    if (ctx.links.has_flap_gate[uj] &&
        q * static_cast<double>(ctx.links.direction[uj]) < 0.0)
        return true;

    // --- gated OUTFALL on the INFLOW end of the link. q == 0 selects neither
    //     end, matching legacy's `n = -1` initialisation.
    int n = -1;
    if (q < 0.0) n = n2;
    if (q > 0.0) n = n1;
    if (n < 0) return false;
    if (ctx.nodes.type[static_cast<size_t>(n)] != NodeType::OUTFALL) return false;
    const int ofr = ctx.node_subtypes.outfall_row(n);
    return ofr >= 0 &&
           ctx.node_subtypes.outfalls.has_flap_gate[static_cast<size_t>(ofr)] != 0;
}

void PumpGroup::resize(int n)    { count=n; auto u=static_cast<size_t>(n); link_idx.resize(u); curve_idx.resize(u,-1); curve_type.resize(u,0); speed.resize(u,1.0); y_on.resize(u,0); y_off.resize(u,0); }
void OrificeGroup::resize(int n) { count=n; auto u=static_cast<size_t>(n); link_idx.resize(u); shape.resize(u,0); surf_area.resize(u,0); length_eff.resize(u,0); }
void WeirGroup::resize(int n)    { count=n; auto u=static_cast<size_t>(n); link_idx.resize(u); weir_type.resize(u,0); c_disch1.resize(u,0); c_disch2.resize(u,0); end_con.resize(u,0); slope.resize(u,0); cd_curve.resize(u,-1); surf_area.resize(u,0); length_eff.resize(u,0); can_surcharge.resize(u,1); }
void OutletGroup::resize(int n)  { count=n; auto u=static_cast<size_t>(n); link_idx.resize(u); curve_idx.resize(u,-1); q_coeff.resize(u,0); q_expon.resize(u,1); }

void StructureSolver::init(SimulationContext& ctx) {
    // Scan links and group by type, populating SoA groups.
    // Matching legacy link.c initialization pattern.
    int n_pumps = 0, n_orifices = 0, n_weirs = 0, n_outlets = 0;

    // First pass: count
    for (int j = 0; j < ctx.n_links(); ++j) {
        auto uj = static_cast<size_t>(j);
        switch (ctx.links.type[uj]) {
            case LinkType::PUMP:    ++n_pumps; break;
            case LinkType::ORIFICE: ++n_orifices; break;
            case LinkType::WEIR:    ++n_weirs; break;
            case LinkType::OUTLET:  ++n_outlets; break;
            default: break;
        }
    }

    pumps_.resize(n_pumps);
    orifices_.resize(n_orifices);
    weirs_.resize(n_weirs);
    outlets_.resize(n_outlets);

    // PARITY link.c weir_getFlow: legacy evaluates weir Q in the project's
    // DISPLAY units per call — length & head multiplied by UCF(LENGTH), the
    // formula applied with the RAW discharge coefficient, and the CMS result
    // divided by M3perFT3 for SI. Folding UCF^2.5/M3perFT3 into the stored
    // coefficient is algebraically identical but reorders the FP ops and
    // costs ~1 ULP per evaluation, so the coefficients are stored RAW and
    // computeWeirFlowK replicates the legacy per-call conversion sequence.

    // Second pass: populate
    int ip = 0, io = 0, iw = 0, ix = 0;
    for (int j = 0; j < ctx.n_links(); ++j) {
        auto uj = static_cast<size_t>(j);
        switch (ctx.links.type[uj]) {
            case LinkType::PUMP: {
                auto uk = static_cast<size_t>(ip);
                // Phase 6: pump config from the relational side-table; `setting`
                // is common control state and stays on base LinkData.
                const auto& P = ctx.link_subtypes.pumps;
                const auto pr = static_cast<size_t>(ctx.link_subtypes.pump_row(j));
                pumps_.link_idx[uk] = j;
                pumps_.curve_idx[uk] = P.curve[pr];
                pumps_.speed[uk] = ctx.links.setting[uj];
                pumps_.y_on[uk]  = P.startup[pr];
                pumps_.y_off[uk] = P.shutoff[pr];
                // Determine curve type from table type
                int ci = P.curve[pr];
                if (ci >= 0 && ci < static_cast<int>(ctx.tables.tables.size())) {
                    int tt = static_cast<int>(ctx.tables.tables[static_cast<size_t>(ci)].type);
                    // TableType CURVE_PUMP1=7, PUMP2=8, PUMP3=9, PUMP4=10, PUMP5=11
                    // Map to curve_type 1..5 (matching legacy PumpCurve enum)
                    if (tt >= 7 && tt <= 11)
                        pumps_.curve_type[uk] = tt - 6; // 7→1, 8→2, 9→3, 10→4, 11→5
                    else
                        pumps_.curve_type[uk] = 6; // Ideal pump if no curve
                }
                // Mirror curve_type for use by DW non_conduit_fn (TYPE4_PUMP
                // excluded from dqdh per legacy dynwave.c:565-575). Phase 6
                // Stage B: PumpData.curve_type is the authoritative store (this
                // runs AFTER build() at engine init, so it is not clobbered);
                // dual-write the wide slot until Stage D removes it.
                ctx.link_subtypes.pumps.curve_type[pr] = pumps_.curve_type[uk];
                ++ip;
                break;
            }
            case LinkType::ORIFICE: {
                auto uk = static_cast<size_t>(io);
                orifices_.link_idx[uk] = j;

                // No init-time cOrif/cWeir/hCrit snapshot: legacy
                // orifice_setSetting derives them from the CURRENT setting,
                // so computeOrificeFlowK computes them inline per call
                // (legacy-exact, including the getWeirCoeff bottom/side
                // split). See the OrificeGroup note in HydStructures.hpp.
                using constants::GRAVITY;
                double y_full = ctx.links.xsect_y_full[uj];
                // Equivalent length for SIDE-orifice surface-area scatter
                // (legacy link.c:1724: max(200, 2·routingStep·sqrt(g·yFull))).
                {
                    double route_step = ctx.options.routing_step;
                    double L = 2.0 * route_step * std::sqrt(GRAVITY * y_full);
                    orifices_.length_eff[uk] = std::max(200.0, L);
                }

                ++io;
                break;
            }
            case LinkType::WEIR: {
                auto uk = static_cast<size_t>(iw);
                const auto& W = ctx.link_subtypes.weirs;
                const auto wr = static_cast<size_t>(ctx.link_subtypes.weir_row(j));
                weirs_.link_idx[uk]   = j;
                weirs_.c_disch1[uk]   = W.cd[wr];   // RAW (see PARITY note above)
                weirs_.c_disch2[uk]   = W.cd2[wr];  // RAW end-section coeff
                weirs_.can_surcharge[uk] = W.can_surcharge[wr];
                weirs_.weir_type[uk]  = static_cast<int>(W.weir_type[wr]);
                weirs_.end_con[uk]    = W.end_contractions[wr];
                // V-notch / trapezoidal side slope comes from the cross-section,
                // not from the INP weir row. Legacy: Weir[k].slope = xsect.sBot
                // (populated by weir_validate). SIDEFLOW / TRANSVERSE → 0.
                weirs_.slope[uk]      = ctx.links.xsect_s_bot[uj];
                // Effective length for surface-area scatter. Legacy weir_validate:
                //   Weir[k].length = max(200, 2·routingStep·sqrt(g·yFull))
                {
                    using constants::GRAVITY;
                    double y_full = ctx.links.xsect_y_full[uj];
                    double route_step = ctx.options.routing_step;
                    double L = 2.0 * route_step * std::sqrt(GRAVITY * y_full);
                    weirs_.length_eff[uk] = std::max(200.0, L);
                }
                ++iw;
                break;
            }
            case LinkType::OUTLET: {
                auto uk = static_cast<size_t>(ix);
                const auto& O = ctx.link_subtypes.outlets;
                const auto xr = static_cast<size_t>(ctx.link_subtypes.outlet_row(j));
                outlets_.link_idx[uk] = j;
                outlets_.q_coeff[uk]  = O.coeff[xr];
                outlets_.q_expon[uk]  = O.expon[xr];
                outlets_.curve_idx[uk] = O.curve[xr];
                ++ix;
                break;
            }
            default: break;
        }
    }

    // Build flat index of all non-conduit links for fast iteration.
    // DUMMY-xsect conduits belong here too: legacy isTrueConduit
    // (dynwave.c:411-414) is false for them, so findLinkFlows routes them
    // through findNonConduitFlow in link-index order — their flow is the
    // upstream node's inflow + overflow, not a momentum solve.
    nc_indices_.clear();
    nc_indices_.reserve(static_cast<size_t>(n_pumps + n_orifices + n_weirs + n_outlets));
    for (int j = 0; j < ctx.n_links(); ++j) {
        auto uj = static_cast<size_t>(j);
        if (ctx.links.type[uj] != LinkType::CONDUIT ||
            ctx.links.xsect_shape[uj] == XsectShape::DUMMY)
            nc_indices_.push_back(j);
    }

    // Map each non-conduit link index to its row in the per-type group so the
    // per-link sequential path (computeNonConduitFlowOne) can dispatch.
    nc_group_k_.assign(static_cast<size_t>(ctx.n_links()), -1);
    for (int k = 0; k < pumps_.count; ++k)
        nc_group_k_[static_cast<size_t>(pumps_.link_idx[static_cast<size_t>(k)])] = k;
    for (int k = 0; k < orifices_.count; ++k)
        nc_group_k_[static_cast<size_t>(orifices_.link_idx[static_cast<size_t>(k)])] = k;
    for (int k = 0; k < weirs_.count; ++k)
        nc_group_k_[static_cast<size_t>(weirs_.link_idx[static_cast<size_t>(k)])] = k;
    for (int k = 0; k < outlets_.count; ++k)
        nc_group_k_[static_cast<size_t>(outlets_.link_idx[static_cast<size_t>(k)])] = k;
}

// ============================================================================
// Pump startup/shutoff depth hysteresis — called ONCE per timestep
// (matches legacy link_setTargetSetting timing in routing.c line 231)
// ============================================================================

void StructureSolver::updatePumpTargetSettings(SimulationContext& ctx) {
    auto& links = ctx.links;
    auto& nodes = ctx.nodes;

    for (int k = 0; k < pumps_.count; ++k) {
        auto uk = static_cast<size_t>(k);
        int j = pumps_.link_idx[uk];
        auto uj = static_cast<size_t>(j);

        int n1 = links.node1[uj];
        if (n1 < 0) continue;
        auto un1 = static_cast<size_t>(n1);

        // Reset target to current setting so hysteresis checks below are the
        // only thing that can override. Matches legacy link.c:618
        // (targetSetting = setting, then conditional overrides).
        links.target_setting[uj] = links.setting[uj];

        // Use depth from START of timestep (matching legacy Node[n1].newDepth)
        double depth = nodes.depth[un1];

        double y_off = pumps_.y_off[uk];
        double y_on  = pumps_.y_on[uk];
        if (y_off > 0.0 && links.setting[uj] > 0.0 && depth < y_off)
            links.target_setting[uj] = 0.0;  // Turn off when depth drops below shutoff
        if (y_on > 0.0 && links.setting[uj] == 0.0 && depth > y_on)
            links.target_setting[uj] = 1.0;  // Turn on when depth exceeds startup
    }
}

// ============================================================================
// Batch pump flow
// ============================================================================

void StructureSolver::computePumpFlows(SimulationContext& ctx, double dt,
                                       const double* node_new_surf_area) {
    for (int k = 0; k < pumps_.count; ++k)
        computePumpFlowK(ctx, dt, node_new_surf_area, k);
}

void StructureSolver::computePumpFlowK(SimulationContext& ctx, double dt,
                                       const double* node_new_surf_area, int k) {
    auto& links = ctx.links;
    auto& nodes = ctx.nodes;

    // Flow unit conversion: pump curves store flow in display units
    int fu = static_cast<int>(ctx.options.flow_units);
    double ucf_flow = ucf::Qcf[fu]; // display → CFS
    int unit_sys  = ucf::getUnitSystem(fu);
    double ucf_len = ucf::Ucf[ucf::LENGTH][unit_sys]; // internal ft → display
    double ucf_vol = ucf::Ucf[ucf::VOLUME][unit_sys]; // internal ft³ → display

    {
        auto uk = static_cast<size_t>(k);
        int j = pumps_.link_idx[uk];
        auto uj = static_cast<size_t>(j);

        int n1 = links.node1[uj];
        int n2 = links.node2[uj];
        if (n1 < 0 || n2 < 0) { links.flow[uj] = 0.0; return; }
        auto un1 = static_cast<size_t>(n1);
        auto un2 = static_cast<size_t>(n2);

        double depth = nodes.depth[un1];
        double head = (nodes.depth[un2] + nodes.invert_elev[un2])
                    - (nodes.depth[un1] + nodes.invert_elev[un1]);

        // Apply target_setting immediately (matching legacy pump_getInflow line 1570:
        // "Link[j].setting = Link[j].targetSetting")
        // NOTE: pump startup/shutoff hysteresis is evaluated ONCE per timestep
        // in updatePumpTargetSettings(), NOT here inside the DW iteration loop.
        links.setting[uj] = links.target_setting[uj];

        // If pump is off, no flow
        if (links.setting[uj] == 0.0) {
            links.flow[uj] = 0.0;
            return;
        }

        double q = 0.0;
        int ct = pumps_.curve_type[uk];
        int ci = pumps_.curve_idx[uk];
        auto uci = static_cast<size_t>(ci);

        switch (ct) {
            case 1: // Volume-based: Q = f(volume) — PUMP1_CURVE
                // Legacy uses table_intervalLookup (step function): curve
                // describes discrete operating volumes, not a smooth Q(V).
                if (ci >= 0 && uci < ctx.tables.tables.size()) {
                    double vol = nodes.volume[un1] * ucf_vol;
                    q = table_intervalLookup(ctx.tables.tables[uci], vol);
                }
                break;
            case 2: // Depth-based: Q = f(depth) — PUMP2_CURVE
                // Legacy uses table_intervalLookup (step function): curve
                // describes discrete operating depths, not a smooth Q(d).
                if (ci >= 0 && uci < ctx.tables.tables.size()) {
                    q = table_intervalLookup(ctx.tables.tables[uci], depth * ucf_len);
                }
                break;
            case 3: // Head-based with speed
            case 5: {
                double s = links.setting[uj];
                double h = (s > 0.0) ? std::max(head / (s * s), 0.0) : 0.0;
                if (ci >= 0 && uci < ctx.tables.tables.size()) {
                    auto& curve = ctx.tables.tables[uci];
                    q = table_lookup_cursor(curve, h * ucf_len) * s;
                    // dQ/dh matching legacy pump.c PUMP3/5 lines 1606-1609:
                    //   Link[j].dqdh = -table_getSlope(&Curve[m], head)
                    //                  * UCF(LENGTH) / UCF(FLOW) / s
                    // sign reversed because flow decreases with increasing head.
                    if (s > 0.0) {
                        double slope = table_getSlope(curve, h * ucf_len);
                        links.dqdh[uj] = -slope * ucf_len / ucf_flow / s;
                    }
                }
                break;
            }
            case 4: { // Depth-based: Q = f(depth) — legacy PUMP4_CURVE
                if (ci >= 0 && uci < ctx.tables.tables.size()) {
                    q = table_lookup_cursor(ctx.tables.tables[uci], depth * ucf_len);
                    // Compute dQ/dh matching legacy pump.c PUMP4_CURVE lines 1621-1622:
                    // dqdh = (Q(depth+dh) - Q(depth)) / dh  (in CFS/ft)
                    constexpr double dh = 0.001; // matching legacy
                    double q1 = table_lookup_cursor(ctx.tables.tables[uci], (depth + dh) * ucf_len);
                    links.dqdh[uj] = (q1 - q) / (dh * ucf_flow);
                }
                break;
            }
            case 6: // Ideal pump
                q = nodes.inflow[un1] + nodes.overflow[un1];
                break;
        }

        q = std::max(q, 0.0);
        // Convert from display flow units to CFS (matching legacy / UCF(FLOW))
        q /= ucf_flow;
        q *= links.setting[uj];

        // Limit pump flow to prevent inlet node from going dry
        // (matching legacy getModPumpFlow in dynwave.c lines 445-486)
        if (q > 0.0) {
            if (nodes.type[un1] == NodeType::STORAGE ||
                pumps_.curve_type[uk] == 1 /* TYPE1 pump, legacy node_getMaxOutflow */) {
                // Storage node (or TYPE1 pump on any node): cap q so node
                // volume doesn't go negative. Legacy uses oldVolume (the
                // start-of-step volume), not the current-iter volume, to
                // avoid cascading clamps across Picard iterations.
                if (nodes.full_volume[un1] > 0.0) {
                    double max_q = nodes.inflow[un1] + nodes.old_volume[un1] / dt;
                    if (q > max_q) q = max_q;
                }
                if (q < 0.0) q = 0.0;
            } else if (ct == 2 || ct == 3 || ct == 4) {
                // Non-storage TYPE2/3/4 pump: if pumping would make depth
                // negative, clamp q to inflow.
                // PARITY: legacy getModPumpFlow (dynwave.c:477-484) applies
                // this check ONLY for TYPE2/TYPE4/TYPE3 pumps and divides by
                // the RAW Xnode.newSurfArea — NO MinSurfArea floor. Flooring
                // the area suppresses the clamp exactly when a small-area
                // wet-well junction is about to be drawn dry (y <= 0), letting
                // the pump run at full curve rate where legacy pins it to the
                // node inflow (seen at Bellinge G70F11Pp1). A zero area gives
                // y = ±inf/NaN with the same comparison outcome as legacy.
                double net_inflow = nodes.inflow[un1] - nodes.outflow[un1] - q;
                double net_vol = 0.5 * (nodes.old_net_inflow[un1] + net_inflow) * dt;
                double surf = (node_new_surf_area != nullptr)
                                ? node_new_surf_area[un1]
                                : constants::MIN_SURFAREA;
                double y_new = nodes.old_depth[un1] + net_vol / surf;
                if (y_new <= 0.0) q = std::max(nodes.inflow[un1], 0.0);
            }
        }

        links.flow[uj] = q;
    }
}

// ============================================================================
// Helper: build XSectParams from link SoA data
// ============================================================================

static XSectParams buildXSP(const LinkData& links, std::size_t uk) {
    XSectParams xs{};
    auto ls = links.xsect_shape[uk];
    xs.type = (ls == XsectShape::DUMMY) ? 0 : static_cast<int>(ls) + 1;
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
// Batch orifice flow — Q = Cd*A*sqrt(2gH), matching legacy orifice_getInflow
// ============================================================================

// Scatter an orifice's surface area to its end nodes, replicating legacy
// findNonConduitSurfArea (dynwave.c:498-510): HALF of Orifice.surfArea is
// added to each end node, then the contribution to node1 is zeroed when the
// link is UP_CRITICAL (or node1 is STORAGE) and the contribution to node2 is
// zeroed when DN_CRITICAL (or node2 is STORAGE). Must run on EVERY exit path
// (including dry/flap) — legacy adds the FUDGE*length baseline even when the
// orifice carries no flow. Omitting that baseline, and omitting the
// critical-class zeroing, understated/overstated node surface area at low
// depth and seeded a per-iteration node-head divergence (extran3 1570/1630).
void StructureSolver::scatterOrificeSurfArea(SimulationContext& ctx,
                                             double* node_new_surf_area,
                                             std::size_t uk_, std::size_t uj_,
                                             std::size_t un1_, std::size_t un2_) {
    if (node_new_surf_area == nullptr) return;
    auto& links = ctx.links;
    auto& nodes = ctx.nodes;
    double sa1 = orifices_.surf_area[uk_] * 0.5;
    double sa2 = sa1;
    auto fc = links.flow_class[uj_];
    if (fc == FlowClass::UP_CRITICAL || nodes.type[un1_] == NodeType::STORAGE)
        sa1 = 0.0;
    if (fc == FlowClass::DN_CRITICAL || nodes.type[un2_] == NodeType::STORAGE)
        sa2 = 0.0;
    node_new_surf_area[un1_] += sa1;
    node_new_surf_area[un2_] += sa2;
}

void StructureSolver::computeOrificeFlows(SimulationContext& ctx,
                                          double* node_new_surf_area) {
    for (int k = 0; k < orifices_.count; ++k)
        computeOrificeFlowK(ctx, node_new_surf_area, k);
}

void StructureSolver::computeOrificeFlowK(SimulationContext& ctx,
                                          double* node_new_surf_area, int k) {
    auto& links = ctx.links;
    auto& nodes = ctx.nodes;
    using constants::GRAVITY;
    constexpr double FUDGE_ORI = 0.0001;

    {
        auto uk = static_cast<size_t>(k);
        int j = orifices_.link_idx[uk];
        auto uj = static_cast<size_t>(j);

        int n1 = links.node1[uj];
        int n2 = links.node2[uj];
        if (n1 < 0 || n2 < 0) { links.flow[uj] = 0.0; return; }
        auto un1 = static_cast<size_t>(n1);
        auto un2 = static_cast<size_t>(n2);

        // --- Current setting (transitions handled by SWMMEngine) ---
        double setting = links.setting[uj];

        // --- Compute setting-adjusted coefficients ---
        // Matching legacy orifice_setSetting():
        //   h = setting * yFull
        //   f_area = xsect_getAofY(xsect, h) * sqrt(2g)
        //   cOrif = cDisch * f_area
        //   cWeir = orifice_getWeirCoeff(j, k, h) * f_area
        double y_full = links.xsect_y_full[uj];
        double cd_val = ctx.link_subtypes.orifices.cd[static_cast<size_t>(ctx.link_subtypes.orifice_row(j))];
        double h_open = setting * y_full;
        if (h_open < FUDGE_ORI) {
            links.flow[uj] = 0.0;
            // PARITY link.c:553: legacy link_getInflow early-returns on
            // setting == 0 WITHOUT touching newDepth — a closed orifice's
            // reported depth stays FROZEN at its last open-state value.
            // (links.depth deliberately not zeroed here.)
            orifices_.surf_area[uk] = 0.0;
            return;
        }

        // Use xsect::getAofY for proper cross-section area at partial opening
        XSectParams xs = buildXSP(links, uj);
        double a_eff = xsect::getAofY(xs, h_open);
        double f_area = a_eff * std::sqrt(2.0 * GRAVITY);
        double cOrif = cd_val * f_area;

        // Critical depth and weir coefficient (matching legacy orifice_getWeirCoeff)
        bool is_side = (ctx.link_subtypes.orifices.orifice_type[static_cast<size_t>(ctx.link_subtypes.orifice_row(j))] > 0.5); // 1=SIDE, 0=BOTTOM
        double hCrit, cWeir;
        if (!is_side) {  // BOTTOM orifice
            double aOverL;
            if (links.xsect_shape[uj] == XsectShape::CIRCULAR) {
                aOverL = h_open / 4.0;
            } else {
                double w = links.xsect_w_max[uj];
                aOverL = (w > 0.0 && h_open > 0.0)
                    ? (h_open * w) / (2.0 * (h_open + w)) : h_open / 4.0;
            }
            hCrit = (cd_val / 0.414) * aOverL;
            cWeir = cd_val * std::sqrt(hCrit) * f_area;
        } else {  // SIDE orifice
            hCrit = h_open;
            cWeir = cd_val * std::sqrt(h_open / 2.0) * f_area;
        }
        hCrit = std::max(hCrit, FUDGE_ORI);

        // --- Compute nodal heads (matching legacy orifice_getInflow) ---
        double h1 = nodes.depth[un1] + nodes.invert_elev[un1];
        double h2 = tailwaterHead(ctx, un1, un2);
        double dir = (h1 >= h2) ? 1.0 : -1.0;

        double y1 = nodes.depth[un1];
        if (dir < 0.0) {
            std::swap(h1, h2);
            y1 = nodes.depth[un2];
        }

        // hcrest always uses n1 invert (link's declared upstream node)
        double hcrest = nodes.invert_elev[un1] + links.offset1[uj];
        double hcrown = 0.0;

        double head, f;
        if (!is_side) {  // BOTTOM orifice
            if (h1 < hcrest) head = 0.0;
            else if (h2 > hcrest) head = h1 - h2;
            else head = h1 - hcrest;
            f = std::min(head / hCrit, 1.0);
        } else {  // SIDE orifice
            hcrown = hcrest + y_full * setting;
            double hmidpt = (hcrest + hcrown) / 2.0;
            if (h1 < hcrown && hcrown > hcrest)
                f = (h1 - hcrest) / (hcrown - hcrest);
            else
                f = 1.0;
            if (f < 1.0)            head = h1 - hcrest;
            else if (h2 < hmidpt)   head = h1 - hmidpt;
            else                    head = h1 - h2;
        }

        // --- Check if flow possible ---
        if (head <= FUDGE_ORI || y1 <= FUDGE_ORI) {
            links.flow[uj] = 0.0;
            links.depth[uj] = 0.0;
            links.dqdh[uj] = 0.0;
            links.flow_class[uj] = FlowClass::DRY;   // legacy link.c:1898
            // Legacy orifice_getInflow:1899: on dry-exit, surfArea = FUDGE·length
            // so the node depth solver still sees a non-zero equivalent area.
            orifices_.surf_area[uk] = FUDGE_ORI * orifices_.length_eff[uk];
            scatterOrificeSurfArea(ctx, node_new_surf_area, uk, uj, un1, un2);  // DRY -> both ends, no zeroing
            return;
        }

        // Flap gate: the link's own gate OR a gated outfall on the inflow end
        // (legacy link.c:1895 passes `dir` to link_setFlapGate).
        if (flapGateBlocks(ctx, j, n1, n2, dir)) {
            links.flow[uj] = 0.0;
            links.depth[uj] = 0.0;
            links.dqdh[uj] = 0.0;
            links.flow_class[uj] = FlowClass::DRY;
            orifices_.surf_area[uk] = FUDGE_ORI * orifices_.length_eff[uk];
            scatterOrificeSurfArea(ctx, node_new_surf_area, uk, uj, un1, un2);
            return;
        }

        // --- Determine flow class (matching legacy) ---
        if (hcrest > h2) {
            links.flow_class[uj] = (dir > 0.0) ? FlowClass::DN_CRITICAL : FlowClass::UP_CRITICAL;
        } else {
            links.flow_class[uj] = FlowClass::SUBCRITICAL;
        }

        // --- Compute flow depth and surface area (matching legacy
        //     orifice_getInflow lines 1911-1919):
        //     SIDE   : newDepth = y1·f ; surfArea = W(newDepth)·length_eff
        //     BOTTOM : newDepth = y1   ; surfArea = A(y1)
        double y_link = y_full * setting;
        if (is_side) {
            double link_depth = y_link * std::max(f, 0.0);
            links.depth[uj] = link_depth;
            double width_at_depth = xsect::getWofY(xs, link_depth);
            orifices_.surf_area[uk] = width_at_depth * orifices_.length_eff[uk];
        } else {
            links.depth[uj] = y_link;
            orifices_.surf_area[uk] = xsect::getAofY(xs, y_link);
        }

        // --- Compute flow (matching legacy orifice_getFlow) ---
        // PARITY link.c orifice_getFlow: legacy uses pow(f, 1.5) — the
        // x·sqrt(x) closed form is not bit-identical, so keep std::pow.
        double q = 0.0;
        double dqdh = 0.0;
        if (f <= 0.0) {
            q = 0.0;
        } else if (f < 1.0) {
            q = cWeir * std::pow(f, 1.5);
            dqdh = 1.5 * q / (f * hCrit);
        } else {
            q = cOrif * std::sqrt(head);
            dqdh = q / (2.0 * head);
        }

        // --- ARMCO flap gate head loss (matching legacy orifice_getFlow) ---
        if (links.has_flap_gate[uj] && q > 0.0 && a_eff > FUDGE_ORI) {
            double veloc = q / a_eff;
            double hLoss = (4.0 / GRAVITY) * veloc * veloc *
                           std::exp(-1.15 * veloc / std::sqrt(head));
            if (f < 1.0) {
                f = f - hLoss / hCrit;
                f = std::max(f, 0.0);
            } else {
                head = head - hLoss;
                head = std::max(head, 0.0);
            }
            // Recompute flow at adjusted head/f (matching legacy recursive call)
            if (f <= 0.0 || head <= 0.0) {
                q = 0.0;
                dqdh = 0.0;
            } else if (f < 1.0) {
                q = cWeir * std::pow(f, 1.5);   // PARITY: legacy pow(f, 1.5)
                dqdh = 1.5 * q / (f * hCrit);
            } else {
                q = cOrif * std::sqrt(head);
                dqdh = q / (2.0 * head);
            }
        }

        // --- Villemonte submergence correction ---
        // PARITY link.c orifice_getInflow:1929-1934: legacy applies the
        // correction UNGUARDED whenever f < 1 and h2 > hcrest, with
        // pow(ratio, 1.5) (ratio ≤ 1 since h1 ≥ h2 after the direction
        // swap; ratio == 1 must zero the flow, which the previous
        // `ratio < 1` guard skipped).
        if (f < 1.0 && h2 > hcrest) {
            double ratio = (h2 - hcrest) / (h1 - hcrest);
            q *= std::pow(1.0 - std::pow(ratio, 1.5), 0.385);
        }

        q = std::max(q, 0.0);
        links.flow[uj] = q * dir;
        links.dqdh[uj] = dqdh;

        // A3 parity term tracing for one orifice (SWMM_TRACE_ORIF=<index>,
        // step-gated via SWMM_TRACE_LSTEP; format-matched to legacy link.c).
        {
            static FILE* of = nullptr;
            static long  of_target = -2;
            static long  of_step = 0;
            static int   of_rows = 0;
            if (of_target == -2) {
                const char* p  = std::getenv("SWMM_TRACE_ORIF");
                const char* tr = std::getenv("SWMM_TRACE_RSTEP");
                const char* ls = std::getenv("SWMM_TRACE_LSTEP");
                of_target = -1;
                if (ls && *ls) of_step = std::atol(ls);
                if (p && *p && tr && *tr) {
                    char fname[512];
                    of_target = std::atol(p);
                    std::snprintf(fname, sizeof(fname), "%s.orif%ld", tr, of_target);
                    of = std::fopen(fname, "w");
                    if (of) std::fprintf(of,
                        "h1,h2,hcrest,hcrown,f,head,cWeir,cOrif,hCrit,dqdh,q\n");
                }
            }
            if (of && j == of_target &&
                (of_step <= 0 || g_trace_rstep_sn + 1 >= of_step) && of_rows < 128) {
                ++of_rows;
                std::fprintf(of, "%a,%a,%a,%a,%a,%a,%a,%a,%a,%a,%a\n",
                             h1, h2, hcrest, hcrown, f, head,
                             cWeir, cOrif, hCrit, dqdh, q * dir);
                if (of_rows >= 128) { std::fclose(of); of = nullptr; }
            }
        }

        // Scatter orifice surface area to end nodes via legacy
        // findNonConduitSurfArea (half each, then zero the UP_CRITICAL end's
        // node1 / DN_CRITICAL end's node2 and any STORAGE end).
        scatterOrificeSurfArea(ctx, node_new_surf_area, uk, uj, un1, un2);
    }
}

// ============================================================================
// Batch weir flow — Q = Cd*L*H^expon, VECTORISABLE
// ============================================================================

void StructureSolver::computeWeirFlows(SimulationContext& ctx,
                                       double* node_new_surf_area) {
    for (int k = 0; k < weirs_.count; ++k)
        computeWeirFlowK(ctx, node_new_surf_area, k);
}

void StructureSolver::computeWeirFlowK(SimulationContext& ctx,
                                       double* node_new_surf_area, int k) {
    auto& links = ctx.links;
    auto& nodes = ctx.nodes;
    constexpr double FUDGE_W = 0.0001;

    {
        auto uk = static_cast<size_t>(k);
        int j = weirs_.link_idx[uk];
        auto uj = static_cast<size_t>(j);

        int n1 = links.node1[uj];
        int n2 = links.node2[uj];
        if (n1 < 0 || n2 < 0) {
            links.flow[uj] = 0.0;
            links.depth[uj] = 0.0;
            weirs_.surf_area[uk] = 0.0;
            return;
        }
        auto un1 = static_cast<size_t>(n1);
        auto un2 = static_cast<size_t>(n2);

        double hgl1 = nodes.depth[un1] + nodes.invert_elev[un1];
        double hgl2 = tailwaterHead(ctx, un1, un2);
        double dir = (hgl1 >= hgl2) ? 1.0 : -1.0;

        // Flap gate check — legacy link_setFlapGate (link.c:2303 passes `dir`):
        // the link's own gate OR a gated outfall on the inflow end.
        if (flapGateBlocks(ctx, j, n1, n2, dir)) {
            links.flow[uj] = 0.0;
            links.depth[uj] = 0.0;
            links.dqdh[uj] = 0.0;
            weirs_.surf_area[uk] = 0.0;
            return;
        }

        // Swap hgl values for reverse flow so that hgl1 is always the
        // *upstream* head, matching legacy lines 2230-2234.
        if (dir < 0.0) std::swap(hgl1, hgl2);

        // Crest elevation — ALWAYS referenced to node1's invert, regardless
        // of flow direction (the weir crest is a physical feature of the
        // link, not a function of which side is upstream this iteration).
        // Legacy link.c:2252-2257 always uses Node[n1].invertElev.
        double y_full = links.xsect_y_full[uj];
        double setting = links.setting[uj];
        // PARITY link.c:2252-2259: legacy computes hcrown from the DESIGN
        // crest (invert + offset1) BEFORE the partial-open adjustment, then
        // raises hcrest by (1-setting)*yFull. Folding the adjustment into
        // hcrest first and adding yFull*setting back is algebraically equal
        // but reorders the FP ops (differs for 0 < setting < 1).
        double hcrest = nodes.invert_elev[un1]
                      + ctx.link_subtypes.weirs.crest_height[static_cast<size_t>(ctx.link_subtypes.weir_row(j))];
        double hcrown = hcrest + y_full;
        hcrest += (1.0 - setting) * y_full;
        double head = hgl1 - hcrest;
        if (head <= FUDGE_W || hcrest >= hcrown) {
            links.flow[uj] = 0.0;
            links.depth[uj] = 0.0;   // legacy weir_getInflow: DRY → newDepth=0
            links.dqdh[uj] = 0.0;
            weirs_.surf_area[uk] = 0.0;
            return;
        }

        double cd     = weirs_.c_disch1[uk];
        double cd2    = weirs_.c_disch2[uk];
        double length = links.xsect_w_max[uj];
        int    wt     = weirs_.weir_type[uk];
        // The ARMCO head loss keys on the LINK's own gate only — legacy passes
        // Link[j].hasFlapGate into weir_getFlow (link.c:2343) rather than
        // calling link_setFlapGate, so an outfall gate must NOT trigger it.
        const bool flap = links.has_flap_gate[uj] != 0;
        double q = 0.0;

        // PARITY link.c weir_getFlow (2323-2412): legacy evaluates the weir
        // formula in DISPLAY units — length & head × UCF(LENGTH), RAW Cd,
        // std::pow with the legacy literal exponents (1.5 / 0.83 / 1.67 /
        // 2.5; NOT x·sqrt(x) closed forms) — then divides the CMS result by
        // M3perFT3 for SI. Replicated op-for-op for bit parity.
        constexpr double M3perFT3 = 0.028317;  // legacy consts.h
        const int    us   = ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));
        const double ucfL = ucf::Ucf[ucf::LENGTH][us];
        auto weirFlow = [&](double head_ft, double dir_f,
                            double& q1_out, double& q2_out) {
            q1_out = 0.0;
            q2_out = 0.0;
            if (head_ft <= 0.0) return;
            double lengthD = length * ucfL;
            double h = head_ft * ucfL;
            // Partially-open V-notch behaves as a trapezoidal weir
            // (legacy link.c:2358-2360).
            int wType = wt;
            if (wType == 2 && setting < 1.0) wType = 3;
            switch (wType) {
                case 0:  // TRANSVERSE — Q = Cd·L·H^1.5
                    lengthD -= 0.1 * weirs_.end_con[uk] * h;
                    lengthD = std::max(lengthD, 0.0);
                    q1_out = cd * lengthD * std::pow(h, 1.5);
                    break;
                case 1:  // SIDEFLOW — reverse flow behaves as TRANSVERSE
                    lengthD -= 0.1 * weirs_.end_con[uk] * h;
                    lengthD = std::max(lengthD, 0.0);
                    if (dir_f < 0.0)
                        q1_out = cd * lengthD * std::pow(h, 1.5);
                    else
                        q1_out = cd * std::pow(lengthD, 0.83) * std::pow(h, 1.67);
                    break;
                case 2:  // V-NOTCH (fully open) — Q = Cd·slope·H^2.5
                    q1_out = cd * weirs_.slope[uk] * std::pow(h, 2.5);
                    break;
                case 3: { // TRAPEZOIDAL (incl. partly-open V-notch)
                    // Legacy: length = W(y)·UCF at y = (1-setting)·yFull.
                    // W(y) inline per shape: triangular w = 2·s·y,
                    // trapezoid w = base + 2·s·y (base from xsect_y_bot).
                    double y = (1.0 - setting) * y_full;
                    double w = (wt == 2)
                        ? 2.0 * weirs_.slope[uk] * y
                        : links.xsect_y_bot[uj] + 2.0 * weirs_.slope[uk] * y;
                    q1_out = cd * (w * ucfL) * std::pow(h, 1.5);
                    q2_out = cd2 * weirs_.slope[uk] * std::pow(h, 2.5);
                    break;
                }
            }
            if (us == 1) {  // SI: CMS → CFS (legacy link.c:2404-2408)
                q1_out /= M3perFT3;
                q2_out /= M3perFT3;
            }
        };

        // Flow area between the (possibly raised) crest and crest+y —
        // legacy weir_getOpenArea (link.c), A(y) inlined per weir shape.
        auto weirOpenArea = [&](double y) -> double {
            double z = (1.0 - setting) * y_full;
            double zy = std::min(z + y, y_full);
            auto areaOf = [&](double d) -> double {
                switch (wt) {
                    case 2:  return weirs_.slope[uk] * d * d;      // TRIANGULAR
                    case 3:  return (links.xsect_y_bot[uj]
                                     + weirs_.slope[uk] * d) * d;  // TRAPEZOIDAL
                    default: return length * d;                    // RECT_OPEN
                }
            };
            return areaOf(zy) - areaOf(z);
        };

        // Surface-area contribution: matches legacy weir_getInflow lines
        // 2271-2273 —   y = yFull - (hcrown - min(h1, hcrown))
        //               surfArea = xsect_getWofY(xsect, y) * length_eff
        // Scattered into node surface-area accumulators by the non_conduit_fn
        // callback in SWMMEngine::stepRouting. Zero when the weir is fully
        // surcharged (h1 >= hcrown) so that it behaves like an orifice.
        {
            double h1_min = std::min(hgl1, hcrown);
            double y_sa = y_full - (hcrown - h1_min);
            // For the common rectangular/triangular/trapezoidal weirs the
            // width at depth y can be computed inline without a full xsect
            // dispatch; fall back to xsect::getWofY for anything else.
            double width_at_y = 0.0;
            if (y_sa > 0.0) {
                switch (wt) {
                    case 0: // TRANSVERSE — rectangular open channel
                    case 1: // SIDEFLOW   — rectangular open channel
                        width_at_y = length;
                        break;
                    case 2: // V-NOTCH    — triangular, w = 2·slope·y
                        width_at_y = 2.0 * weirs_.slope[uk] * y_sa;
                        break;
                    case 3: { // TRAPEZOIDAL — w = base + 2·slope·y
                        double y_bot = links.xsect_y_bot[uj];  // base (crest) width
                        width_at_y = y_bot + 2.0 * weirs_.slope[uk] * y_sa;
                        break;
                    }
                    default: width_at_y = length; break;
                }
            }
            weirs_.surf_area[uk] = width_at_y * weirs_.length_eff[uk];
        }

        // --- Head above crown: legacy weir_getInflow link.c:2285-2304.
        //     canSurcharge=YES → equivalent-orifice formulation;
        //     canSurcharge=NO  → cap head at the weir opening height and
        //     keep the ordinary weir equation.
        if (hgl1 >= hcrown && weirs_.can_surcharge[uk]) {
            // cSurcharge — legacy caches q(setting·yFull)/sqrt(setting·yFull/2)
            // at validation (weir_validate) and on control changes
            // (weir_setSetting), computed THROUGH weir_getFlow (so end
            // contractions apply). Recomputed here with the identical op
            // order — bit-identical while `setting` is unchanged.
            double h_full = setting * y_full;
            double q1f = 0.0, q2f = 0.0;
            weirFlow(h_full, 1.0, q1f, q2f);
            double h_half = h_full / 2.0;
            double c_surcharge =
                (h_half > 0.0) ? (q1f + q2f) / std::sqrt(h_half) : 0.0;

            // weir_getOrificeFlow (link.c): q = c·sqrt(head) with the head
            // measured to the weir-opening midpoint (or to the tailwater when
            // submerged above it), plus the ARMCO flap-gate head loss.
            double y_mid = (hcrest + hcrown) / 2.0;
            double h_orif = (hgl2 < y_mid) ? hgl1 - y_mid : hgl1 - hgl2;
            double y_open = hcrown - hcrest;
            q = c_surcharge * std::sqrt(h_orif);
            if (flap) {
                double a = weirOpenArea(y_open);
                if (a > 0.0) {
                    double v = q / a;
                    double hloss = (4.0 / constants::GRAVITY) * v * v
                                 * std::exp(-1.15 * v / std::sqrt(y_open));
                    h_orif -= hloss;
                    h_orif = std::max(h_orif, 0.0);
                    q = c_surcharge * std::sqrt(h_orif);
                }
            }

            // Reported weir depth on the surcharge (orifice-equivalent) path:
            // legacy weir_getInflow link.c:2294-2296 sets newDepth =
            // hcrown-hcrest (the weir opening height).
            links.depth[uj] = y_open;

            // Surcharged weir dqdh uses the ORIFICE head (legacy
            // weir_getOrificeFlow sets dqdh = q / (2·head)).
            links.dqdh[uj] = (h_orif > 0.0) ? q / (2.0 * h_orif) : 0.0;
        } else {
            // canSurcharge == NO with head above crown: legacy link.c:2303
            // limits the head to the height of the weir opening.
            if (hgl1 >= hcrown) head = hcrown - hcrest;

            // --- Free (weir) flow path — legacy weir_getFlow + Villemonte.
            double q1 = 0.0, q2 = 0.0;
            weirFlow(head, dir, q1, q2);

            // ARMCO flap-gate head-loss adjustment (legacy weir_getFlow):
            // velocity through the weir opening → head loss → one
            // re-evaluation of the weir formula at the adjusted head.
            if (flap) {
                double area = weirOpenArea(head);
                if (area > 1.0e-6) {  // legacy TINY
                    double veloc = (q1 + q2) / area;
                    double hLoss = (4.0 / constants::GRAVITY) * veloc * veloc
                                 * std::exp(-1.15 * veloc / std::sqrt(head));
                    head = head - hLoss;
                    if (head < 0.0) head = 0.0;
                    weirFlow(head, dir, q1, q2);
                }
            }

            // dqdh from the UN-submerged q1/q2 at the (possibly ARMCO-
            // adjusted) head — legacy weir_getdqdh (link.c:2495), called at
            // the end of weir_getFlow BEFORE Villemonte is applied:
            //   TRANSVERSE  : 1.5·|q1/h|
            //   SIDEFLOW    : reverse 1.5·|q1/h|, forward 1.67·|q1/h|
            //   V-NOTCH     : 2.5·|q1/h| (fully open, q2==0)
            //   TRAPEZOIDAL : 1.5·|q1/h| + 2.5·|q2/h|
            if (std::fabs(head) >= FUDGE_W) {
                double q1h = std::fabs(q1 / head);
                double q2h = std::fabs(q2 / head);
                switch (wt) {
                    case 0:  links.dqdh[uj] = 1.5 * q1h; break;
                    case 1:  links.dqdh[uj] = (dir < 0.0 ? 1.5 : 1.67) * q1h; break;
                    case 2:  links.dqdh[uj] = (q2h == 0.0) ? 2.5 * q1h
                                                           : 1.5 * q1h + 2.5 * q2h; break;
                    default: links.dqdh[uj] = 1.5 * q1h + 2.5 * q2h; break;
                }
            } else {
                links.dqdh[uj] = 0.0;
            }

            // Villemonte submergence correction — legacy weir_getInflow
            // link.c:2306-2315: applied PER COMPONENT with the weir-type
            // power {1.5, 5/3, 2.5, 1.5} for q1 and the V-notch power 2.5
            // for q2; the ratio uses the RAW heads above the crest (not the
            // surcharge-capped head).
            if (hgl2 > hcrest) {
                static const double weirPower[4] = {1.5, 5.0 / 3.0, 2.5, 1.5};
                double ratio = (hgl2 - hcrest) / (hgl1 - hcrest);
                double wp = (wt >= 0 && wt < 4) ? weirPower[wt] : 1.5;
                q1 *= std::pow(1.0 - std::pow(ratio, wp), 0.385);
                if (q2 > 0.0)
                    q2 *= std::pow(1.0 - std::pow(ratio, 2.5), 0.385);
            }

            q = q1 + q2;

            // Reported weir depth on the free-flow path: legacy link.c:2318
            // newDepth = MIN(h1 - hcrest, yFull) — RAW head above crest,
            // not the surcharge-capped one.
            links.depth[uj] = std::min(hgl1 - hcrest, y_full);
        }

        if (q < 0.0) q = 0.0;
        links.flow[uj] = q * dir;

        // Legacy findNonConduitSurfArea (dynwave.c:503-506) explicitly
        // sets weir surfArea1/surfArea2 = 0 "to maintain SWMM 4 compatibility"
        // — i.e. weirs contribute NO surface area to their end nodes.
        // The refactored engine previously scattered `width_at_y*length_eff/2`
        // here, which added a huge phantom term (e.g. ~2 550 ft² per end for
        // the Rich_BC_CSO TWIN-WEIR) that biased the Picard denominator at
        // weir-fed junctions and drove the TWIN66-0 over-surcharge. Leaving
        // node_new_surf_area untouched restores alignment with legacy.
        (void)node_new_surf_area;
        (void)un1;
        (void)un2;
    }
}

// ============================================================================
// Batch outlet flow — Q = coeff*H^expon or curve, VECTORISABLE
// ============================================================================

void StructureSolver::computeOutletFlows(SimulationContext& ctx) {
    for (int k = 0; k < outlets_.count; ++k)
        computeOutletFlowK(ctx, k);
}

void StructureSolver::computeOutletFlowK(SimulationContext& ctx, int k) {
    auto& links = ctx.links;
    auto& nodes = ctx.nodes;

    int fu = static_cast<int>(ctx.options.flow_units);
    int unit_sys  = ucf::getUnitSystem(fu);
    double ucf_len  = ucf::Ucf[ucf::LENGTH][unit_sys];
    double ucf_flow = ucf::Qcf[fu];
    constexpr double FUDGE_OUT = 0.0001;

    {
        auto uk = static_cast<size_t>(k);
        int j = outlets_.link_idx[uk];
        auto uj = static_cast<size_t>(j);

        int n1 = links.node1[uj];
        int n2 = links.node2[uj];
        if (n1 < 0 || n2 < 0) {
            links.flow[uj] = 0.0;
            links.depth[uj] = 0.0;
            return;
        }
        auto un1 = static_cast<size_t>(n1);
        auto un2 = static_cast<size_t>(n2);

        double h1 = nodes.depth[un1] + nodes.invert_elev[un1];
        double h2 = tailwaterHead(ctx, un1, un2);
        int dir = (h1 >= h2) ? 1 : -1;

        // Track which node provides the upstream depth (legacy: y1 is
        // Node[n1].newDepth, swapped to Node[n2].newDepth for reverse flow).
        double y1 = nodes.depth[un1];
        if (dir < 0) {
            std::swap(h1, h2);
            y1 = nodes.depth[un2];
        }

        // Crest elevation is a physical feature of the outlet — ALWAYS
        // referenced to n1's invert regardless of flow direction. Matches
        // legacy link.c:2650  hcrest = Node[n1].invertElev + Link[j].offset1.
        double hcrest = nodes.invert_elev[un1] + links.offset1[uj];

        // Effective head — NODE_HEAD (functional/tabular head-based) outlets
        // in DW routing account for downstream submergence via
        //     head = h1 - max(h2, hcrest)
        // matching legacy link.c:2651-2653. NODE_DEPTH outlets use simply
        //     head = h1 - hcrest (= y1 for upstream node).
        // outlet_type encoding in the refactored parser:
        //   0 = FUNCTIONAL_HEAD, 1 = FUNCTIONAL_DEPTH,
        //   2 = TABULAR_HEAD,    3 = TABULAR_DEPTH
        int outlet_type = static_cast<int>(ctx.link_subtypes.outlets.outlet_type[static_cast<size_t>(ctx.link_subtypes.outlet_row(j))]);
        bool depth_based = (outlet_type == 1 || outlet_type == 3);

        double head = depth_based
                        ? (h1 - hcrest)
                        : (h1 - std::max(h2, hcrest));
        // Flap gate (closed against reverse flow) — the link's own gate OR a
        // gated outfall on the inflow end (legacy link.c:2697 passes `dir`).
        bool blocked_by_flap = flapGateBlocks(ctx, j, n1, n2, dir);

        if (head <= FUDGE_OUT || y1 <= FUDGE_OUT || blocked_by_flap) {
            links.flow[uj] = 0.0;
            links.depth[uj] = 0.0;
            links.flow_class[uj] = FlowClass::DRY;
            links.dqdh[uj] = 0.0;
            return;
        }

        // Rating curve is indexed on user-units of length (legacy calls
        // UCF(LENGTH) before the table lookup).
        double lookup_val = depth_based
                                ? (y1 * ucf_len)
                                : (head * ucf_len);

        double q = 0.0;
        int ci = outlets_.curve_idx[uk];
        if (ci >= 0 && static_cast<size_t>(ci) < ctx.tables.tables.size()) {
            q = table_lookup_cursor(ctx.tables.tables[static_cast<size_t>(ci)], lookup_val);
            q /= ucf_flow;
        } else {
            // FUNCTIONAL outlet — legacy outlet_getFlow:
            //   q = qCoeff * pow(head*UCF(LENGTH), qExpon) / UCF(FLOW)
            // i.e. the user coefficient operates on the DISPLAY-unit head and
            // yields a DISPLAY-unit flow, which is then converted to CFS. Use
            // lookup_val (already head*ucf_len) directly and divide by ucf_flow.
            if (lookup_val > 0.0)
                q = outlets_.q_coeff[uk]
                  * std::pow(lookup_val, outlets_.q_expon[uk]) / ucf_flow;
        }

        // Legacy outlet_getInflow line 2669 applies the setting multiplier:
        //   return dir * Link[j].setting * outlet_getFlow(k, head)
        // The refactored code previously overwrote links.setting[uj] with a
        // 0/1 flag, which silently clobbered any control-rule action on
        // the outlet. Preserve the setting, apply it as a multiplier.
        q *= links.setting[uj];

        if (q < 0.0) q = 0.0;
        links.flow[uj] = q * static_cast<double>(dir);
        links.depth[uj] = head;
        links.flow_class[uj] = FlowClass::SUBCRITICAL;
        // PARITY: legacy leaves outlet dqdh at 0.0 — findNonConduitFlow
        // (src/legacy/engine/dynwave.c:513) zeroes Link.dqdh and
        // outlet_getInflow (src/legacy/engine/link.c:2611-2670) never sets it.
        // The q/(2*head) form previously used here is the ORIFICE formula
        // (link.c:1974) and inflated sumdqdh at nodes adjacent to outlets,
        // diverging the EXTRAN surcharge depth update.
        links.dqdh[uj] = 0.0;
    }
}

// ============================================================================
// Main dispatch
// ============================================================================

void StructureSolver::computeAllFlows(SimulationContext& ctx, double dt,
                                      double* node_new_surf_area) {
    // Ordering: pumps first (they READ node_new_surf_area via the flow-
    // limiter); orifices + weirs next (they WRITE their per-link surfArea
    // contribution into node_new_surf_area, matching legacy
    // findNonConduitSurfArea); outlets last (zero surfArea per legacy).
    if (pumps_.count > 0)    computePumpFlows(ctx, dt, node_new_surf_area);
    if (orifices_.count > 0) computeOrificeFlows(ctx, node_new_surf_area);
    if (weirs_.count > 0)    computeWeirFlows(ctx, node_new_surf_area);
    if (outlets_.count > 0)  computeOutletFlows(ctx);
}

// ============================================================================
// Per-link sequential path (PARITY with legacy findLinkFlows, dynwave.c:383-398)
// ============================================================================

void StructureSolver::computeNonConduitFlowOne(SimulationContext& ctx, double dt,
                                               double* node_new_surf_area,
                                               int link_idx) {
    auto uj = static_cast<std::size_t>(link_idx);

    // DUMMY conduit (legacy isTrueConduit == false): findNonConduitFlow →
    // link_getInflow → conduit_getInflow — node_getOutflow's dispatch
    // (DIVIDER split, else the upstream node's inflow + overflow) passes
    // straight through, capped by the conduit's MaxFlow (link.c:1323-1333);
    // a closed setting carries no flow (link.c:553). The STORAGE branch is
    // unreachable for dummies — a storage node cannot have a dummy outflow
    // link (link.c:1006 raises an input error).
    // Surface area: findNonConduitSurfArea (dynwave.c:498) contributes 0
    // for anything but an orifice, so there is nothing to scatter.
    if (ctx.links.type[uj] == LinkType::CONDUIT) {
        ctx.links.dqdh[uj] = 0.0;
        double q = 0.0;
        if (ctx.links.setting[uj] != 0.0) {
            const int n1 = ctx.links.node1[uj];
            if (n1 >= 0) {
                const auto un1 = static_cast<std::size_t>(n1);
                if (ctx.nodes.type[un1] == NodeType::DIVIDER)
                    q = divider::getOutflow(ctx, n1, link_idx);
                else
                    q = ctx.nodes.inflow[un1] + ctx.nodes.overflow[un1];
            }
            const double qlim = ctx.links.q_limit[uj];
            if (qlim > 0.0 && q > qlim) q = qlim;
        }
        ctx.links.flow[uj] = q;
        return;
    }

    int k = (uj < nc_group_k_.size()) ? nc_group_k_[uj] : -1;
    if (k < 0) return;

    // PARITY: legacy findNonConduitFlow (dynwave.c:423) zeroes Link.dqdh
    // before the per-type flow routine; only routines that compute a
    // derivative overwrite it.
    ctx.links.dqdh[uj] = 0.0;

    switch (ctx.links.type[uj]) {
        case LinkType::PUMP:
            computePumpFlowK(ctx, dt, node_new_surf_area, k);
            break;
        case LinkType::ORIFICE:
            computeOrificeFlowK(ctx, node_new_surf_area, k);
            break;
        case LinkType::WEIR:
            computeWeirFlowK(ctx, node_new_surf_area, k);
            break;
        case LinkType::OUTLET:
            computeOutletFlowK(ctx, k);
            break;
        default: break;
    }
}

void StructureSolver::scatterHeldSurfArea(SimulationContext& ctx,
                                          double* node_new_surf_area,
                                          int link_idx) {
    // Legacy updateNodeFlows runs for BYPASSED links too, scattering the
    // previous iteration's Link.surfArea1/2 into Xnode.newSurfArea. Only
    // orifices carry a non-zero held area (weir/pump/outlet contribute none).
    auto uj = static_cast<std::size_t>(link_idx);
    if (ctx.links.type[uj] != LinkType::ORIFICE) return;
    int k = (uj < nc_group_k_.size()) ? nc_group_k_[uj] : -1;
    if (k < 0) return;
    int n1 = ctx.links.node1[uj];
    int n2 = ctx.links.node2[uj];
    if (n1 < 0 || n2 < 0) return;
    scatterOrificeSurfArea(ctx, node_new_surf_area, static_cast<std::size_t>(k),
                           uj, static_cast<std::size_t>(n1),
                           static_cast<std::size_t>(n2));
}

} // namespace hydstruct
} // namespace openswmm
