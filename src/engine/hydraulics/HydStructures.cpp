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
#include "../core/SimulationContext.hpp"
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
                ++ip;
                break;
            }
            case LinkType::ORIFICE: {
                auto uk = static_cast<size_t>(io);
                orifices_.link_idx[uk] = j;
                orifices_.c_orifice[uk] = ctx.links.cd[uj];
                orifices_.has_flap[uk] = ctx.links.has_flap_gate[uj];
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
}

// ============================================================================
// Batch pump flow
// ============================================================================

void StructureSolver::computePumpFlows(SimulationContext& ctx) {
    auto& links = ctx.links;
    auto& nodes = ctx.nodes;

    for (int k = 0; k < pumps_.count; ++k) {
        auto uk = static_cast<size_t>(k);
        int j = pumps_.link_idx[uk];
        auto uj = static_cast<size_t>(j);

        if (links.setting[uj] == 0.0) {
            links.flow[uj] = 0.0;
            continue;
        }

        int n1 = links.node1[uj];
        int n2 = links.node2[uj];
        auto un1 = static_cast<size_t>(n1);
        auto un2 = static_cast<size_t>(n2);

        double depth = nodes.depth[un1];
        double head = (nodes.depth[un2] + nodes.invert_elev[un2])
                    - (nodes.depth[un1] + nodes.invert_elev[un1]);

        // Pump on/off check
        if (depth <= pumps_.y_off[uk]) {
            links.setting[uj] = 0.0;
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
        links.flow[uj] = q * links.setting[uj];
    }
}

// ============================================================================
// Batch orifice flow — Q = Cd*A*sqrt(2gH), VECTORISABLE
// ============================================================================

void StructureSolver::computeOrificeFlows(SimulationContext& ctx) {
    auto& links = ctx.links;
    auto& nodes = ctx.nodes;

    for (int k = 0; k < orifices_.count; ++k) {
        auto uk = static_cast<size_t>(k);
        int j = orifices_.link_idx[uk];
        auto uj = static_cast<size_t>(j);

        int n1 = links.node1[uj];
        int n2 = links.node2[uj];
        auto un1 = static_cast<size_t>(n1);
        auto un2 = static_cast<size_t>(n2);

        double h1 = nodes.depth[un1] + nodes.invert_elev[un1];
        double h2 = nodes.depth[un2] + nodes.invert_elev[un2];
        double head = h1 - h2;
        int dir = (head >= 0.0) ? 1 : -1;
        head = std::fabs(head);

        double crest = links.offset1[uj] + nodes.invert_elev[un1];
        head -= (crest - std::min(h1, h2));
        if (head <= 0.0) { links.flow[uj] = 0.0; continue; }

        double f = head / orifices_.h_crit[uk];  // fraction of critical depth

        double q;
        if (f < 1.0) {
            // Weir-like flow: Q = c_weir * f^1.5
            q = orifices_.c_weir[uk] * f * std::sqrt(f);
        } else {
            // Full orifice: Q = c_orifice * sqrt(head)
            q = orifices_.c_orifice[uk] * std::sqrt(head);
        }

        links.flow[uj] = q * dir * links.setting[uj];
    }
}

// ============================================================================
// Batch weir flow — Q = Cd*L*H^expon, VECTORISABLE
// ============================================================================

void StructureSolver::computeWeirFlows(SimulationContext& ctx) {
    auto& links = ctx.links;
    auto& nodes = ctx.nodes;

    for (int k = 0; k < weirs_.count; ++k) {
        auto uk = static_cast<size_t>(k);
        int j = weirs_.link_idx[uk];
        auto uj = static_cast<size_t>(j);

        int n1 = links.node1[uj];
        int n2 = links.node2[uj];
        auto un1 = static_cast<size_t>(n1);
        auto un2 = static_cast<size_t>(n2);

        double h1 = nodes.depth[un1] + nodes.invert_elev[un1];
        double h2 = nodes.depth[un2] + nodes.invert_elev[un2];
        double head = h1 - h2;
        int dir = (head >= 0.0) ? 1 : -1;
        head = std::fabs(head);

        double crest = links.crest_height[uj] + nodes.invert_elev[un1];
        head -= (crest - std::min(h1, h2));
        if (head <= 0.0) { links.flow[uj] = 0.0; continue; }

        double cd = weirs_.c_disch1[uk];
        double length = links.xsect_w_max[uj];

        double q = 0.0;
        int wt = weirs_.weir_type[uk];

        switch (wt) {
            case 0: // Transverse: Q = Cd * L * H^1.5
                length -= 0.1 * weirs_.end_con[uk] * head;
                if (length < 0.0) length = 0.0;
                q = cd * length * head * std::sqrt(head);
                break;
            case 1: // Sideflow: Q = Cd * L^0.83 * H^1.67
                length -= 0.1 * weirs_.end_con[uk] * head;
                if (length < 0.0) length = 0.0;
                q = cd * std::pow(length, 0.83) * std::pow(head, 1.67);
                break;
            case 2: // V-notch: Q = Cd * slope * H^2.5
                q = cd * weirs_.slope[uk] * std::pow(head, 2.5);
                break;
            case 3: // Trapezoidal: Q1 = Cd1*L*H^1.5 + Q2 = Cd2*slope*H^2.5
                q = cd * length * head * std::sqrt(head)
                  + weirs_.c_disch2[uk] * weirs_.slope[uk] * std::pow(head, 2.5);
                break;
        }

        // Weir submergence correction (P8-G19)
        // When tailwater > crest, reduce flow by submergence factor
        double h_down = std::fabs(h2 - (links.crest_height[uj] + nodes.invert_elev[un1]));
        if (h_down > 0.0 && head > 0.0) {
            double ht_ratio = h_down / head;
            if (ht_ratio > 0.0 && ht_ratio < 1.0) {
                // Villemonte submergence: Qs = Q * (1 - (ht/h)^n)^0.385
                double sub_factor = std::pow(1.0 - std::pow(ht_ratio, 1.5), 0.385);
                q *= sub_factor;
            }
        }

        links.flow[uj] = q * dir * links.setting[uj];
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

void StructureSolver::computeAllFlows(SimulationContext& ctx, double /*dt*/) {
    if (pumps_.count > 0)    computePumpFlows(ctx);
    if (orifices_.count > 0) computeOrificeFlows(ctx);
    if (weirs_.count > 0)    computeWeirFlows(ctx);
    if (outlets_.count > 0)  computeOutletFlows(ctx);
}

} // namespace hydstruct
} // namespace openswmm
