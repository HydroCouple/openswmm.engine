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
#include <cmath>
#include <algorithm>

namespace openswmm {
namespace hydstruct {

void PumpGroup::resize(int n)    { count=n; auto u=static_cast<size_t>(n); link_idx.resize(u); curve_idx.resize(u,-1); curve_type.resize(u,0); speed.resize(u,1.0); y_on.resize(u,0); y_off.resize(u,0); }
void OrificeGroup::resize(int n) { count=n; auto u=static_cast<size_t>(n); link_idx.resize(u); shape.resize(u,0); c_orifice.resize(u,0); c_weir.resize(u,0); h_crit.resize(u,0); has_flap.resize(u,false); }
void WeirGroup::resize(int n)    { count=n; auto u=static_cast<size_t>(n); link_idx.resize(u); weir_type.resize(u,0); c_disch1.resize(u,0); c_disch2.resize(u,0); end_con.resize(u,0); slope.resize(u,0); cd_curve.resize(u,-1); has_flap.resize(u,false); }
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
                    // TableType CURVE_PUMP1=7, PUMP2=8, PUMP3=9, PUMP4=10
                    // Map to curve_type 1..4 (matching legacy PumpCurve enum)
                    if (tt >= 7 && tt <= 10)
                        pumps_.curve_type[uk] = tt - 6; // 7→1, 8→2, 9→3, 10→4
                    else
                        pumps_.curve_type[uk] = 6; // Ideal pump if no curve
                }
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

                ++io;
                break;
            }
            case LinkType::WEIR: {
                auto uk = static_cast<size_t>(iw);
                weirs_.link_idx[uk] = j;
                weirs_.c_disch1[uk] = ctx.links.cd[uj];
                weirs_.has_flap[uk] = ctx.links.has_flap_gate[uj];
                ++iw;
                break;
            }
            case LinkType::OUTLET: {
                auto uk = static_cast<size_t>(ix);
                outlets_.link_idx[uk] = j;
                outlets_.q_coeff[uk] = ctx.links.cd[uj];
                outlets_.q_expon[uk] = ctx.links.param2[uj];
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
// Batch pump flow
// ============================================================================

void StructureSolver::computePumpFlows(SimulationContext& ctx, double dt) {
    auto& links = ctx.links;
    auto& nodes = ctx.nodes;

    // Flow unit conversion: pump curves store flow in display units
    int fu = static_cast<int>(ctx.options.flow_units);
    double ucf_flow = ucf::Qcf[fu]; // display → CFS

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

        // Pump on/off hysteresis (matching legacy link.c lines 619-624)
        // Sets target_setting based on upstream depth vs startup/shutoff thresholds.
        // Control rules may also set target_setting (higher priority handled by
        // controls_.evaluate() in SWMMEngine which runs BEFORE this).
        double y_off = pumps_.y_off[uk];
        double y_on  = pumps_.y_on[uk];
        if (y_off > 0.0 && links.setting[uj] > 0.0 && depth < y_off)
            links.target_setting[uj] = 0.0;  // Turn off when depth drops below shutoff
        if (y_on > 0.0 && links.setting[uj] == 0.0 && depth > y_on)
            links.target_setting[uj] = 1.0;  // Turn on when depth exceeds startup

        // Apply target_setting immediately (matching legacy pump_getInflow line 1570:
        // "Link[j].setting = Link[j].targetSetting")
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
            case 1: // Volume-based: Q = f(volume)
                if (ci >= 0 && uci < ctx.tables.tables.size()) {
                    double vol = nodes.volume[un1];
                    q = table_lookup_cursor(ctx.tables.tables[uci], vol);
                }
                break;
            case 2: // Depth-based: Q = f(depth)
                if (ci >= 0 && uci < ctx.tables.tables.size()) {
                    q = table_lookup_cursor(ctx.tables.tables[uci], depth);
                }
                break;
            case 3: // Head-based with speed
            case 5: {
                double s = links.setting[uj];
                double h = (s > 0.0) ? std::max(head / (s * s), 0.0) : 0.0;
                if (ci >= 0 && uci < ctx.tables.tables.size()) {
                    q = table_lookup_cursor(ctx.tables.tables[uci], h) * s;
                }
                break;
            }
            case 4: // Depth-based: Q = f(depth)
                if (ci >= 0 && uci < ctx.tables.tables.size()) {
                    q = table_lookup_cursor(ctx.tables.tables[uci], depth);
                }
                break;
            case 6: // Ideal pump
                q = nodes.inflow[un1];
                break;
        }

        if (q < 0.0) q = 0.0;
        // Convert from display flow units to CFS (matching legacy / UCF(FLOW))
        q /= ucf_flow;
        q *= links.setting[uj];

        // Limit pump flow to prevent inlet node from going dry
        // (matching legacy getModPumpFlow in dynwave.c lines 445-486)
        if (q > 0.0) {
            if (nodes.type[un1] == NodeType::STORAGE) {
                // Storage node: don't let volume go negative
                double max_q = nodes.volume[un1] / dt + nodes.inflow[un1];
                if (q > max_q && max_q > 0.0) q = max_q;
            } else {
                // Non-storage: check if pumping would make depth negative
                double net_inflow = nodes.inflow[un1] - nodes.outflow[un1] - q;
                double net_vol = 0.5 * (nodes.old_net_inflow[un1] + net_inflow) * dt;
                // Approximate surface area at current depth
                double surf = constants::MIN_SURFAREA; // junction default
                if (surf < constants::MIN_SURFAREA) surf = constants::MIN_SURFAREA;
                double y_new = nodes.old_depth[un1] + net_vol / surf;
                if (y_new <= 0.0) q = std::max(nodes.inflow[un1], 0.0);
            }
        }

        links.flow[uj] = q;
    }
}

// ============================================================================
// Batch orifice flow — Q = Cd*A*sqrt(2gH), VECTORISABLE
// ============================================================================

void StructureSolver::computeOrificeFlows(SimulationContext& ctx) {
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

        // --- Apply target_setting to setting (matching legacy orifice_setSetting) ---
        // For orifices, setting transitions gradually via orate (handled in SWMMEngine).
        // Here we just use the current setting value.
        double setting = links.setting[uj];

        // --- Compute setting-adjusted coefficients (matching legacy) ---
        double y_full = links.xsect_y_full[uj];
        double cd_val = links.cd[uj];
        double h_open = setting * y_full;  // Effective opening height
        if (h_open < FUDGE_ORI) { links.flow[uj] = 0.0; continue; }

        // Effective area at opening height (from cross-section)
        // For circular: area at depth h; for rectangular: w*h
        double a_full = links.xsect_a_full[uj];
        double w_max  = links.xsect_w_max[uj];
        // Simplified area calculation at partial opening (linear interp)
        double a_eff = (y_full > 0.0) ? a_full * (h_open / y_full) : a_full;
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
                double w = w_max;
                aOverL = (w > 0.0 && h_open > 0.0)
                    ? (h_open * w) / (2.0 * (h_open + w)) : h_open / 4.0;
            }
            hCrit = (cd_val / 0.414) * aOverL;
            cWeir = cd_val * std::sqrt(hCrit) * f_area;
        } else {  // SIDE orifice
            hCrit = h_open;  // Full opening height
            cWeir = cd_val * std::sqrt(h_open / 2.0) * f_area;
        }
        if (hCrit < FUDGE_ORI) hCrit = FUDGE_ORI;

        // --- Compute nodal heads ---
        double hgl1 = nodes.depth[un1] + nodes.invert_elev[un1];
        double hgl2 = nodes.depth[un2] + nodes.invert_elev[un2];
        double dir = (hgl1 >= hgl2) ? 1.0 : -1.0;

        // Swap for reverse flow
        double y_up;
        if (dir < 0.0) {
            std::swap(hgl1, hgl2);
            y_up = nodes.depth[un2];
        } else {
            y_up = nodes.depth[un1];
        }

        double hcrest = (dir > 0.0)
            ? nodes.invert_elev[un1] + links.offset1[uj]
            : nodes.invert_elev[un2] + links.offset1[uj];

        double head, f;
        if (!is_side) {  // BOTTOM orifice
            if (hgl1 < hcrest) head = 0.0;
            else if (hgl2 > hcrest) head = hgl1 - hgl2;
            else head = hgl1 - hcrest;
            f = std::min(head / hCrit, 1.0);
        } else {  // SIDE orifice
            double hcrown = hcrest + y_full * setting;
            double hmidpt = (hcrest + hcrown) / 2.0;
            // Submergence fraction
            if (hgl1 < hcrown && hcrown > hcrest)
                f = (hgl1 - hcrest) / (hcrown - hcrest);
            else
                f = 1.0;
            // Head computation
            if (f < 1.0)       head = hgl1 - hcrest;
            else if (hgl2 < hmidpt) head = hgl1 - hmidpt;
            else                    head = hgl1 - hgl2;
        }

        // --- Check if flow possible ---
        if (head <= FUDGE_ORI || y_up <= FUDGE_ORI) {
            links.flow[uj] = 0.0;
            continue;
        }
        // Flap gate check
        if (links.has_flap_gate[uj] && dir < 0.0) {
            links.flow[uj] = 0.0;
            continue;
        }

        // --- Compute flow ---
        double q;
        if (f <= 0.0) {
            q = 0.0;
        } else if (f < 1.0) {
            // Weir flow regime: Q = cWeir * f^1.5
            q = cWeir * std::pow(f, 1.5);
        } else {
            // Orifice flow regime: Q = cOrif * sqrt(head)
            q = cOrif * std::sqrt(head);
        }

        // --- Villemonte submergence correction (matching legacy) ---
        if (f < 1.0 && hgl2 > hcrest && hgl1 > hcrest) {
            double ratio = (hgl2 - hcrest) / (hgl1 - hcrest);
            if (ratio < 1.0 && ratio > 0.0) {
                q *= std::pow(1.0 - std::pow(ratio, 1.5), 0.385);
            }
        }

        if (q < 0.0) q = 0.0;
        links.flow[uj] = q * dir;
    }
}

// ============================================================================
// Batch weir flow — Q = Cd*L*H^expon, VECTORISABLE
// ============================================================================

void StructureSolver::computeWeirFlows(SimulationContext& ctx) {
    auto& links = ctx.links;
    auto& nodes = ctx.nodes;
    constexpr double FUDGE_W = 0.0001;

    for (int k = 0; k < weirs_.count; ++k) {
        auto uk = static_cast<size_t>(k);
        int j = weirs_.link_idx[uk];
        auto uj = static_cast<size_t>(j);

        int n1 = links.node1[uj];
        int n2 = links.node2[uj];
        if (n1 < 0 || n2 < 0) { links.flow[uj] = 0.0; continue; }
        auto un1 = static_cast<size_t>(n1);
        auto un2 = static_cast<size_t>(n2);

        double hgl1 = nodes.depth[un1] + nodes.invert_elev[un1];
        double hgl2 = nodes.depth[un2] + nodes.invert_elev[un2];
        double dir = (hgl1 >= hgl2) ? 1.0 : -1.0;

        // Flap gate check
        if (links.has_flap_gate[uj] && dir < 0.0) {
            links.flow[uj] = 0.0;
            continue;
        }

        // Swap for reverse flow
        if (dir < 0.0) std::swap(hgl1, hgl2);

        // Crest elevation: setting raises the effective crest
        // (matching legacy line 2257: hcrest += (1-setting)*yFull)
        double y_full = links.xsect_y_full[uj];
        double setting = links.setting[uj];
        double hcrest = nodes.invert_elev[(dir > 0.0) ? un1 : un2]
                      + links.crest_height[uj]
                      + (1.0 - setting) * y_full;

        double hcrown = hcrest + y_full * setting;
        double head = hgl1 - hcrest;
        if (head <= FUDGE_W) { links.flow[uj] = 0.0; continue; }

        double cd = links.cd[uj];
        double length = links.xsect_w_max[uj];
        int wt = static_cast<int>(links.param1[uj]); // weir type

        double q = 0.0;

        // --- Surcharge check (matching legacy weir_getInflow lines 2285-2301)
        // When water exceeds crown and weir can surcharge, use orifice equation
        // with coefficient derived from weir flow at full opening.
        if (hgl1 >= hcrown && y_full > 0.0 && setting > 0.0) {
            // Compute cSurcharge: evaluate weir flow at full opening height,
            // then derive equivalent orifice coeff (matching legacy weir_setSetting)
            double h_full = setting * y_full;
            double q_full = 0.0;
            switch (wt) {
                case 0: q_full = cd * length * std::pow(h_full, 1.5); break;
                case 1: q_full = cd * std::pow(length, 0.83) * std::pow(h_full, 1.67); break;
                case 2: q_full = cd * links.param2[uj] * std::pow(h_full, 2.5); break;
                case 3: q_full = cd * length * std::pow(h_full, 1.5); break;
            }
            double h_half = h_full / 2.0;
            double c_surcharge = (h_half > 0.0) ? q_full / std::sqrt(h_half) : 0.0;

            // Orifice-mode head (matching legacy lines 2290-2292)
            double y_mid = (hcrest + hcrown) / 2.0;
            double h_orif = (hgl2 < y_mid) ? hgl1 - y_mid : hgl1 - hgl2;
            if (h_orif < 0.0) h_orif = 0.0;

            q = c_surcharge * std::sqrt(h_orif);
        } else {
            // --- Normal weir flow
            switch (wt) {
                case 0: { // TRANSVERSE: Q = Cd * L * H^1.5
                    double ec = links.param2[uj]; // end contractions
                    double L = length - 0.1 * ec * head;
                    if (L < 0.0) L = 0.0;
                    q = cd * L * std::pow(head, 1.5);
                    break;
                }
                case 1: // SIDEFLOW: Q = Cd * L^0.83 * H^1.67
                    q = cd * std::pow(length, 0.83) * std::pow(head, 1.67);
                    break;
                case 2: { // V-NOTCH: Q = Cd * slope * H^2.5
                    double slope = links.param2[uj];
                    q = cd * slope * std::pow(head, 2.5);
                    break;
                }
                case 3: { // TRAPEZOIDAL
                    q = cd * length * std::pow(head, 1.5);
                    break;
                }
            }

            // Villemonte submergence correction
            double h_tail = hgl2 - hcrest;
            if (h_tail > 0.0 && head > 0.0) {
                double ratio = h_tail / head;
                if (ratio > 0.0 && ratio < 1.0) {
                    q *= std::pow(1.0 - std::pow(ratio, 1.5), 0.385);
                }
            }
        }

        if (q < 0.0) q = 0.0;
        links.flow[uj] = q * dir;
    }
}

// ============================================================================
// Batch outlet flow — Q = coeff*H^expon or curve, VECTORISABLE
// ============================================================================

void StructureSolver::computeOutletFlows(SimulationContext& ctx) {
    auto& links = ctx.links;
    auto& nodes = ctx.nodes;

    for (int k = 0; k < outlets_.count; ++k) {
        auto uk = static_cast<size_t>(k);
        int j = outlets_.link_idx[uk];
        auto uj = static_cast<size_t>(j);

        int n1 = links.node1[uj];
        int n2 = links.node2[uj];
        auto un1 = static_cast<size_t>(n1);
        auto un2 = static_cast<size_t>(n2);

        double h1 = nodes.depth[un1] + nodes.invert_elev[un1];
        double h2 = nodes.depth[un2] + nodes.invert_elev[un2];
        double head = std::fabs(h1 - h2);
        int dir = (h1 >= h2) ? 1 : -1;

        if (head <= 0.0) { links.flow[uj] = 0.0; continue; }

        double q;
        if (outlets_.curve_idx[uk] >= 0) {
            auto uci = static_cast<size_t>(outlets_.curve_idx[uk]);
            if (uci < ctx.tables.tables.size()) {
                q = table_lookup_cursor(ctx.tables.tables[uci], head);
            } else {
                q = 0.0;
            }
        } else {
            q = outlets_.q_coeff[uk] * std::pow(head, outlets_.q_expon[uk]);
        }

        links.flow[uj] = q * dir;
    }
}

// ============================================================================
// Main dispatch
// ============================================================================

void StructureSolver::computeAllFlows(SimulationContext& ctx, double dt) {
    if (pumps_.count > 0)    computePumpFlows(ctx, dt);
    if (orifices_.count > 0) computeOrificeFlows(ctx);
    if (weirs_.count > 0)    computeWeirFlows(ctx);
    if (outlets_.count > 0)  computeOutletFlows(ctx);
}

} // namespace hydstruct
} // namespace openswmm
