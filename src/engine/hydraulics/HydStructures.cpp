/**
 * @file HydStructures.cpp
 * @brief Non-conduit link flow — batch by type, numerically identical to legacy.
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "HydStructures.hpp"
#include "../core/Constants.hpp"
#include "../core/SimulationContext.hpp"
#include "../core/UnitConversion.hpp"
#include "../math/SIMD.hpp"
#include "XSectBatch.hpp"
#include <cmath>
#include <algorithm>

namespace openswmm {
namespace hydstruct {

void PumpGroup::resize(int n)    { count=n; auto u=static_cast<size_t>(n); link_idx.resize(u); curve_idx.resize(u,-1); curve_type.resize(u,0); speed.resize(u,1.0); y_on.resize(u,0); y_off.resize(u,0); }
void OrificeGroup::resize(int n) { count=n; auto u=static_cast<size_t>(n); link_idx.resize(u); shape.resize(u,0); c_orifice.resize(u,0); c_weir.resize(u,0); h_crit.resize(u,0); has_flap.resize(u,false); surf_area.resize(u,0); length_eff.resize(u,0); }
void WeirGroup::resize(int n)    { count=n; auto u=static_cast<size_t>(n); link_idx.resize(u); weir_type.resize(u,0); c_disch1.resize(u,0); c_disch2.resize(u,0); end_con.resize(u,0); slope.resize(u,0); cd_curve.resize(u,-1); has_flap.resize(u,false); surf_area.resize(u,0); length_eff.resize(u,0); }
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

    // Second pass: populate
    int ip = 0, io = 0, iw = 0, ix = 0;
    for (int j = 0; j < ctx.n_links(); ++j) {
        auto uj = static_cast<size_t>(j);
        switch (ctx.links.type[uj]) {
            case LinkType::PUMP: {
                auto uk = static_cast<size_t>(ip);
                pumps_.link_idx[uk] = j;
                pumps_.curve_idx[uk] = ctx.links.pump_curve[uj];
                pumps_.speed[uk] = ctx.links.setting[uj];
                pumps_.y_on[uk]  = ctx.links.pump_startup[uj];
                pumps_.y_off[uk] = ctx.links.pump_shutoff[uj];
                // Determine curve type from table type
                int ci = ctx.links.pump_curve[uj];
                if (ci >= 0 && ci < static_cast<int>(ctx.tables.tables.size())) {
                    int tt = static_cast<int>(ctx.tables.tables[static_cast<size_t>(ci)].type);
                    // TableType CURVE_PUMP1=7, PUMP2=8, PUMP3=9, PUMP4=10, PUMP5=11
                    // Map to curve_type 1..5 (matching legacy PumpCurve enum)
                    if (tt >= 7 && tt <= 11)
                        pumps_.curve_type[uk] = tt - 6; // 7→1, 8→2, 9→3, 10→4, 11→5
                    else
                        pumps_.curve_type[uk] = 6; // Ideal pump if no curve
                }
                // Mirror curve_type into LinkData for use by DW non_conduit_fn
                // (TYPE4_PUMP excluded from dqdh per legacy dynwave.c:565-575)
                ctx.links.pump_curve_type[uj] = pumps_.curve_type[uk];
                ++ip;
                break;
            }
            case LinkType::ORIFICE: {
                auto uk = static_cast<size_t>(io);
                orifices_.link_idx[uk] = j;
                orifices_.has_flap[uk] = ctx.links.has_flap_gate[uj];

                // Pre-compute orifice coefficients matching legacy orifice.c
                // c_orifice = Cd * A_full * sqrt(2g)  (full orifice flow)
                // c_weir = Cw * L                      (weir-like partial flow)
                // h_crit = A_full / L                  (transition depth)
                using constants::GRAVITY;
                double a_full = ctx.links.xsect_a_full[uj];
                double y_full = ctx.links.xsect_y_full[uj];
                double cd_val = ctx.links.cd[uj];

                orifices_.c_orifice[uk] = cd_val * a_full * std::sqrt(2.0 * GRAVITY);
                // Weir coefficient for partially-open orifice
                // Legacy: CW = Cd * L * sqrt(2g) where L = perimeter
                // For rectangular: L = 2*(w+h), for circular: L = pi*d
                double w_max = ctx.links.xsect_w_max[uj];
                double perim = (w_max > 0.0 && y_full > 0.0)
                    ? 2.0 * (w_max + y_full) : 3.14159 * y_full;
                orifices_.c_weir[uk] = cd_val * perim * std::sqrt(2.0 * GRAVITY);
                orifices_.h_crit[uk] = (perim > 0.0) ? a_full / perim : y_full;
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
                weirs_.link_idx[uk]   = j;
                weirs_.c_disch1[uk]   = ctx.links.cd[uj];
                weirs_.has_flap[uk]   = ctx.links.has_flap_gate[uj];
                weirs_.weir_type[uk]  = static_cast<int>(ctx.links.param1[uj]);
                weirs_.end_con[uk]    = ctx.links.param2[uj];
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
                outlets_.link_idx[uk] = j;
                outlets_.q_coeff[uk]  = ctx.links.cd[uj];
                outlets_.q_expon[uk]  = ctx.links.param2[uj];
                // pump_curve stores resolved rating-curve index for TABULAR outlets
                outlets_.curve_idx[uk] = ctx.links.pump_curve[uj];
                ++ix;
                break;
            }
            default: break;
        }
    }

    // Build flat index of all non-conduit links for fast iteration
    nc_indices_.clear();
    nc_indices_.reserve(static_cast<size_t>(n_pumps + n_orifices + n_weirs + n_outlets));
    for (int j = 0; j < ctx.n_links(); ++j) {
        auto uj = static_cast<size_t>(j);
        if (ctx.links.type[uj] != LinkType::CONDUIT)
            nc_indices_.push_back(j);
    }
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
    auto& links = ctx.links;
    auto& nodes = ctx.nodes;

    // Flow unit conversion: pump curves store flow in display units
    int fu = static_cast<int>(ctx.options.flow_units);
    double ucf_flow = ucf::Qcf[fu]; // display → CFS
    int unit_sys  = ucf::getUnitSystem(fu);
    double ucf_len = ucf::Ucf[ucf::LENGTH][unit_sys]; // internal ft → display
    double ucf_vol = ucf::Ucf[ucf::VOLUME][unit_sys]; // internal ft³ → display

    for (int k = 0; k < pumps_.count; ++k) {
        auto uk = static_cast<size_t>(k);
        int j = pumps_.link_idx[uk];
        auto uj = static_cast<size_t>(j);

        int n1 = links.node1[uj];
        int n2 = links.node2[uj];
        if (n1 < 0 || n2 < 0) { links.flow[uj] = 0.0; continue; }
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
            continue;
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
                    q = table_lookup_cursor(ctx.tables.tables[uci], h * ucf_len) * s;
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
            } else {
                // Non-storage: if pumping would make depth negative, clamp
                // q to inflow. Legacy uses Xnode[j].newSurfArea (accumulated
                // from connected conduits this iter); refactored falls back
                // to MIN_SURFAREA if caller didn't supply it.
                double net_inflow = nodes.inflow[un1] - nodes.outflow[un1] - q;
                double net_vol = 0.5 * (nodes.old_net_inflow[un1] + net_inflow) * dt;
                double surf = (node_new_surf_area != nullptr)
                                ? node_new_surf_area[un1]
                                : constants::MIN_SURFAREA;
                surf = std::max(surf, constants::MIN_SURFAREA);
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

void StructureSolver::computeOrificeFlows(SimulationContext& ctx,
                                          double* node_new_surf_area) {
    auto& links = ctx.links;
    auto& nodes = ctx.nodes;
    using constants::GRAVITY;
    constexpr double FUDGE_ORI = 0.0001;

    for (int k = 0; k < orifices_.count; ++k) {
        auto uk = static_cast<size_t>(k);
        int j = orifices_.link_idx[uk];
        auto uj = static_cast<size_t>(j);

        int n1 = links.node1[uj];
        int n2 = links.node2[uj];
        if (n1 < 0 || n2 < 0) { links.flow[uj] = 0.0; continue; }
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
        double cd_val = links.cd[uj];
        double h_open = setting * y_full;
        if (h_open < FUDGE_ORI) {
            links.flow[uj] = 0.0;
            links.depth[uj] = 0.0;
            orifices_.surf_area[uk] = 0.0;
            continue;
        }

        // Use xsect::getAofY for proper cross-section area at partial opening
        XSectParams xs = buildXSP(links, uj);
        double a_eff = xsect::getAofY(xs, h_open);
        double f_area = a_eff * std::sqrt(2.0 * GRAVITY);
        double cOrif = cd_val * f_area;

        // Critical depth and weir coefficient (matching legacy orifice_getWeirCoeff)
        bool is_side = (links.param1[uj] > 0.5); // 1=SIDE, 0=BOTTOM
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
        double h2 = nodes.depth[un2] + nodes.invert_elev[un2];
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
            // Legacy orifice_getInflow: on dry-exit, surfArea = FUDGE·length
            // so the node depth solver still sees a non-zero equivalent area.
            orifices_.surf_area[uk] = FUDGE_ORI * orifices_.length_eff[uk];
            continue;
        }

        // Flap gate: block reverse flow
        if (links.has_flap_gate[uj] && dir < 0.0) {
            links.flow[uj] = 0.0;
            links.depth[uj] = 0.0;
            links.dqdh[uj] = 0.0;
            orifices_.surf_area[uk] = FUDGE_ORI * orifices_.length_eff[uk];
            continue;
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
        double q = 0.0;
        double dqdh = 0.0;
        if (f <= 0.0) {
            q = 0.0;
        } else if (f < 1.0) {
            q = cWeir * fastmath::pow3_2(f);
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
                q = cWeir * fastmath::pow3_2(f);
                dqdh = 1.5 * q / (f * hCrit);
            } else {
                q = cOrif * std::sqrt(head);
                dqdh = q / (2.0 * head);
            }
        }

        // --- Villemonte submergence correction (matching legacy) ---
        if (f < 1.0 && h2 > hcrest && h1 > hcrest) {
            double ratio = (h2 - hcrest) / (h1 - hcrest);
            if (ratio < 1.0 && ratio > 0.0) {
                double inner = 1.0 - fastmath::pow3_2(ratio);
                if (inner > 0.0) q *= std::pow(inner, 0.385);
            }
        }

        q = std::max(q, 0.0);
        links.flow[uj] = q * dir;
        links.dqdh[uj] = dqdh;

        // Scatter orifice surface area to end nodes (half each, zero for
        // STORAGE ends). Matches legacy findNonConduitSurfArea.
        if (node_new_surf_area != nullptr) {
            double sa_half = orifices_.surf_area[uk] * 0.5;
            if (nodes.type[un1] != NodeType::STORAGE)
                node_new_surf_area[un1] += sa_half;
            if (nodes.type[un2] != NodeType::STORAGE)
                node_new_surf_area[un2] += sa_half;
        }
    }
}

// ============================================================================
// Batch weir flow — Q = Cd*L*H^expon, VECTORISABLE
// ============================================================================

void StructureSolver::computeWeirFlows(SimulationContext& ctx,
                                       double* node_new_surf_area) {
    auto& links = ctx.links;
    auto& nodes = ctx.nodes;
    constexpr double FUDGE_W = 0.0001;

    for (int k = 0; k < weirs_.count; ++k) {
        auto uk = static_cast<size_t>(k);
        int j = weirs_.link_idx[uk];
        auto uj = static_cast<size_t>(j);

        int n1 = links.node1[uj];
        int n2 = links.node2[uj];
        if (n1 < 0 || n2 < 0) {
            links.flow[uj] = 0.0;
            weirs_.surf_area[uk] = 0.0;
            continue;
        }
        auto un1 = static_cast<size_t>(n1);
        auto un2 = static_cast<size_t>(n2);

        double hgl1 = nodes.depth[un1] + nodes.invert_elev[un1];
        double hgl2 = nodes.depth[un2] + nodes.invert_elev[un2];
        double dir = (hgl1 >= hgl2) ? 1.0 : -1.0;

        // Flap gate check — legacy link_setFlapGate: if forward flow blocked,
        // zero flow. Our has_flap_gate flag matches the legacy sense.
        if (links.has_flap_gate[uj] && dir < 0.0) {
            links.flow[uj] = 0.0;
            weirs_.surf_area[uk] = 0.0;
            continue;
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
        double hcrest = nodes.invert_elev[un1]
                      + links.crest_height[uj]
                      + (1.0 - setting) * y_full;

        double hcrown = hcrest + y_full * setting;
        double head = hgl1 - hcrest;
        if (head <= FUDGE_W || hcrest >= hcrown) {
            links.flow[uj] = 0.0;
            weirs_.surf_area[uk] = 0.0;
            continue;
        }

        double cd     = weirs_.c_disch1[uk];
        double length = links.xsect_w_max[uj];
        int    wt     = weirs_.weir_type[uk];
        double q = 0.0;

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

        // --- Surcharge path: legacy weir_getInflow lines 2285-2301
        if (hgl1 >= hcrown && y_full > 0.0 && setting > 0.0) {
            // c_surcharge = q(at h_full) / sqrt(h_full/2). Recomputing this
            // per call rather than caching because `setting` can change via
            // controls. Uses the SAME weir formula as below so the two modes
            // match at the transition point.
            double h_full = setting * y_full;
            double q_full = 0.0;
            switch (wt) {
                case 0: q_full = cd * length * fastmath::pow3_2(h_full); break;
                case 1: q_full = cd * std::pow(length, 0.83)
                                    * fastmath::pow5_3(h_full); break;
                case 2: q_full = cd * weirs_.slope[uk]
                                    * fastmath::pow5_2(h_full); break;
                case 3: { // TRAPEZOIDAL: q = q1 (rect) + q2 (V-notch sides)
                    double y_bot = links.xsect_y_bot[uj];
                    double q1 = cd * y_bot * fastmath::pow3_2(h_full);
                    double q2 = weirs_.c_disch2[uk] * weirs_.slope[uk]
                              * fastmath::pow5_2(h_full);
                    q_full = q1 + q2;
                    break;
                }
            }
            double h_half = h_full * 0.5;
            double c_surcharge = (h_half > 0.0) ? q_full / std::sqrt(h_half) : 0.0;

            double y_mid = (hcrest + hcrown) * 0.5;
            double h_orif = (hgl2 < y_mid) ? hgl1 - y_mid : hgl1 - hgl2;
            h_orif = std::max(h_orif, 0.0);
            q = c_surcharge * std::sqrt(h_orif);
        } else {
            // --- Free (weir) flow path
            switch (wt) {
                case 0: { // TRANSVERSE — Q = Cd·L·H^1.5
                    double ec = weirs_.end_con[uk];
                    double L = length - 0.1 * ec * head;
                    if (L < 0.0) L = 0.0;
                    q = cd * L * fastmath::pow3_2(head);
                    break;
                }
                case 1: // SIDEFLOW — reverse flow behaves as TRANSVERSE
                        // (legacy link.c:2380-2388).
                    if (dir < 0.0)
                        q = cd * length * fastmath::pow3_2(head);
                    else
                        q = cd * std::pow(length, 0.83) * fastmath::pow5_3(head);
                    break;
                case 2: // V-NOTCH — Q = Cd·slope·H^2.5
                    q = cd * weirs_.slope[uk] * fastmath::pow5_2(head);
                    break;
                case 3: { // TRAPEZOIDAL — q1 (rect crest) + q2 (V-notch sides)
                    double y_bot = links.xsect_y_bot[uj];
                    double q1 = cd * y_bot * fastmath::pow3_2(head);
                    double q2 = weirs_.c_disch2[uk] * weirs_.slope[uk]
                              * fastmath::pow5_2(head);
                    q = q1 + q2;
                    break;
                }
            }

            // Villemonte submergence correction — leave the 0.385 power
            // as std::pow (no closed form); replace the inner 1.5.
            double h_tail = hgl2 - hcrest;
            if (h_tail > 0.0 && head > 0.0) {
                double ratio = h_tail / head;
                if (ratio > 0.0 && ratio < 1.0) {
                    double inner = 1.0 - fastmath::pow3_2(ratio);
                    if (inner > 0.0) q *= std::pow(inner, 0.385);
                }
            }
        }

        if (q < 0.0) q = 0.0;
        links.flow[uj] = q * dir;

        // dqdh — crude `q/(2·head)` derivative, matching legacy
        // weir_getdqdh at a first approximation. Saves the SWMMEngine
        // fallback from having to re-derive it from finite differences.
        links.dqdh[uj] = (head > FUDGE_W) ? q / (2.0 * head) : 0.0;

        // Scatter weir surface area to the two end nodes (half each,
        // zero for STORAGE ends). Matches legacy findNonConduitSurfArea:
        //     Link[i].surfArea1 = Weir[k].surfArea / 2.0
        //     Link[i].surfArea2 = Link[i].surfArea1
        // — then updateNodeFlows scatters to un1, un2.
        if (node_new_surf_area != nullptr) {
            double sa_half = weirs_.surf_area[uk] * 0.5;
            if (nodes.type[un1] != NodeType::STORAGE)
                node_new_surf_area[un1] += sa_half;
            if (nodes.type[un2] != NodeType::STORAGE)
                node_new_surf_area[un2] += sa_half;
        }
    }
}

// ============================================================================
// Batch outlet flow — Q = coeff*H^expon or curve, VECTORISABLE
// ============================================================================

void StructureSolver::computeOutletFlows(SimulationContext& ctx) {
    auto& links = ctx.links;
    auto& nodes = ctx.nodes;

    int fu = static_cast<int>(ctx.options.flow_units);
    int unit_sys  = ucf::getUnitSystem(fu);
    double ucf_len  = ucf::Ucf[ucf::LENGTH][unit_sys];
    double ucf_flow = ucf::Qcf[fu];
    constexpr double FUDGE_OUT = 0.0001;

    for (int k = 0; k < outlets_.count; ++k) {
        auto uk = static_cast<size_t>(k);
        int j = outlets_.link_idx[uk];
        auto uj = static_cast<size_t>(j);

        int n1 = links.node1[uj];
        int n2 = links.node2[uj];
        if (n1 < 0 || n2 < 0) {
            links.flow[uj] = 0.0;
            links.depth[uj] = 0.0;
            continue;
        }
        auto un1 = static_cast<size_t>(n1);
        auto un2 = static_cast<size_t>(n2);

        double h1 = nodes.depth[un1] + nodes.invert_elev[un1];
        double h2 = nodes.depth[un2] + nodes.invert_elev[un2];
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
        int outlet_type = static_cast<int>(links.param1[uj]);
        bool depth_based = (outlet_type == 1 || outlet_type == 3);

        double head = depth_based
                        ? (h1 - hcrest)
                        : (h1 - std::max(h2, hcrest));
        // Flap gate (closed against reverse flow)
        bool blocked_by_flap = (links.has_flap_gate[uj] && dir < 0);

        if (head <= FUDGE_OUT || y1 <= FUDGE_OUT || blocked_by_flap) {
            links.flow[uj] = 0.0;
            links.depth[uj] = 0.0;
            links.flow_class[uj] = FlowClass::DRY;
            links.dqdh[uj] = 0.0;
            continue;
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
            double head_ft = lookup_val / ucf_len;
            if (head_ft > 0.0)
                q = outlets_.q_coeff[uk] * std::pow(head_ft, outlets_.q_expon[uk]);
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
        links.dqdh[uj] = (head > FUDGE_OUT) ? q / (2.0 * head) : 0.0;
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

} // namespace hydstruct
} // namespace openswmm
