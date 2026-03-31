/**
 * @file RDII.cpp
 * @brief RDII unit hydrograph convolution — matching legacy rdii.c.
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "RDII.hpp"
#include "../core/SimulationContext.hpp"
#include "../core/UnitConversion.hpp"
#include "../core/DateTime.hpp"
#include <cmath>
#include <cstdio>
#include <algorithm>

namespace openswmm {
namespace rdii {

void RDIIGroupSoA::resize(int n) {
    count = n;
    auto un = static_cast<size_t>(n);
    node_idx.assign(un, -1);
    uh_idx.assign(un, -1);
    area.assign(un, 0.0);
    uh_data.resize(un * 3);  // 3 responses per group
    rain_interval.assign(un, 300);
    rain_accum.assign(un, 0.0);
    time_accum.assign(un, 0.0);
}

// ---------------------------------------------------------------------------
// UH ordinate — matches legacy getUnitHydOrd() exactly.
//
// Legacy stores tPeak and tBase in seconds, and computes:
//   qPeak = 2.0 / tBase * 3600.0
// This gives the ordinate in 1/hr units (rainfall rate per unit depth).
// We replicate this exactly so the rest of the math is identical.
// ---------------------------------------------------------------------------
double RDIISolver::uhOrdinate(const UnitHydParams& uh, int month,
                              int response, double t) const {
    int m = month % 12;
    int k = response % 3;
    double tp    = uh.tPeak[m][k];
    double tBase = uh.tBase[m][k];
    if (tp <= 0.0 || tBase <= 0.0) return 0.0;
    if (t >= tBase) return 0.0;

    // Peak ordinate in 1/hr (matching legacy: 2./tBase*3600.)
    double qPeak = 2.0 / tBase * 3600.0;
    double t2 = tBase - tp;

    double f;
    if (t <= tp) f = t / tp;
    else         f = (t2 > 0.0) ? 1.0 - (t - tp) / t2 : 0.0;
    f = std::max(f, 0.0);
    return f * qPeak;
}

int RDIISolver::addUnitHydParams(const std::string& name,
                                 const UnitHydParams& params) {
    auto it = uh_name_to_idx_.find(name);
    if (it != uh_name_to_idx_.end()) {
        uh_params[static_cast<size_t>(it->second)] = params;
        return it->second;
    }
    int idx = static_cast<int>(uh_params.size());
    uh_params.push_back(params);
    uh_name_to_idx_[name] = idx;
    return idx;
}

int RDIISolver::findUnitHyd(const std::string& name) const {
    auto it = uh_name_to_idx_.find(name);
    if (it == uh_name_to_idx_.end()) return -1;
    return it->second;
}

int RDIISolver::getRainInterval(const UnitHydParams& uh, double wet_step) {
    int ri = static_cast<int>(wet_step);
    if (ri <= 0) ri = 300;
    for (int m = 0; m < 12; ++m) {
        for (int k = 0; k < 3; ++k) {
            double tp = uh.tPeak[m][k];
            if (tp > 0.0) {
                int tLimb = static_cast<int>(tp);
                if (tLimb > 0) ri = std::min(ri, tLimb);
                int tFall = static_cast<int>(uh.tBase[m][k] - tp);
                if (tFall > 0) ri = std::min(ri, tFall);
            }
        }
    }
    return std::max(ri, 1);
}

int RDIISolver::getMaxPeriods(const UnitHydParams& uh, int response,
                              int rainInterval) {
    if (rainInterval <= 0) return 0;
    int k = response % 3;
    int nMax = 0;
    for (int m = 0; m < 12; ++m) {
        int n = static_cast<int>(uh.tBase[m][k] / rainInterval) + 1;
        nMax = std::max(n, nMax);
    }
    return nMax;
}

// ---------------------------------------------------------------------------
// init() — populate UH params from parsed data, allocate per-response buffers.
// ---------------------------------------------------------------------------
void RDIISolver::init(SimulationContext& ctx) {

    // Populate UH params from parsed [HYDROGRAPHS] data
    for (const auto& entry : ctx.unit_hyds.entries) {
        int idx = findUnitHyd(entry.name);
        if (idx < 0) {
            UnitHydParams params{};
            idx = addUnitHydParams(entry.name, params);
        }
        auto& uh = uh_params[static_cast<size_t>(idx)];

        double tPeak_sec = entry.t * 3600.0;
        double tBase_sec = entry.t * entry.k * 3600.0;

        int m_start = (entry.month < 0) ? 0  : entry.month;
        int m_end   = (entry.month < 0) ? 11 : entry.month;
        int k = entry.response;

        for (int m = m_start; m <= m_end; ++m) {
            uh.r[m][k]       = entry.r;
            uh.tPeak[m][k]   = tPeak_sec;
            uh.tBase[m][k]   = tBase_sec;
            uh.iaMax[m][k]   = entry.dmax;
            uh.iaRecov[m][k] = entry.drecov;
            uh.iaInit[m][k]  = entry.dinit;
        }
    }

    const auto& assigns = ctx.rdii_assigns;
    int n_assigns = assigns.count();
    if (n_assigns == 0) {
        groups_.count = 0;
        return;
    }

    groups_.resize(n_assigns);
    double wet_step = ctx.options.wet_step;

    for (int i = 0; i < n_assigns; ++i) {
        auto ui = static_cast<size_t>(i);
        groups_.node_idx[ui] = assigns.node_idx[ui];
        groups_.area[ui]     = assigns.sewer_area[ui];

        const std::string& uh_name = assigns.uh_name[ui];
        int uh_i = findUnitHyd(uh_name);
        groups_.uh_idx[ui] = uh_i;

        if (uh_i < 0 || uh_i >= static_cast<int>(uh_params.size())) {
            groups_.rain_interval[ui] = 300;
            // Zero out the per-response data
            for (int k = 0; k < 3; ++k)
                groups_.uh_data[ui * 3 + static_cast<size_t>(k)].max_periods = 0;
            continue;
        }

        const auto& uh = uh_params[static_cast<size_t>(uh_i)];
        int rainInterval = getRainInterval(uh, wet_step);
        groups_.rain_interval[ui] = rainInterval;

        // Allocate per-response circular buffers (matching legacy TUHData uh[3])
        // Get starting month for iaInit initialization
        int start_month = datetime::monthOfYear(ctx.options.start_date) - 1;

        for (int k = 0; k < 3; ++k) {
            int maxPer = getMaxPeriods(uh, k, rainInterval);
            auto& rd = groups_.uh_data[ui * 3 + static_cast<size_t>(k)];
            if (maxPer > 0) {
                rd.allocate(maxPer);
                // Match legacy init (rdii.c line 1079-1085):
                //   drySeconds = maxPeriods * rainInterval + 1
                //   period = maxPeriods + 1  (wraps to 0 on first write)
                //   iaUsed = iaInit[month][k]
                rd.dry_seconds = static_cast<long>(maxPer) * rainInterval + 1;
                rd.period = maxPer + 1;
                rd.ia_used = uh.iaInit[start_month][k];
            }
        }
    }
}

// ---------------------------------------------------------------------------
// applyIA — matches legacy applyIA() exactly.
//
// Subtracts initial abstraction from rainfall depth.
// During dry periods, recovers IA at iaRecov rate.
// ---------------------------------------------------------------------------
static double applyIA(const UnitHydParams& uh, UHResponseData& rd,
                       int month, int response,
                       double rainDepth, double dt_sec) {
    int m = month % 12;
    int k = response % 3;
    double iaMax = uh.iaMax[m][k];

    // Determine unused IA
    double ia = iaMax - rd.ia_used;
    if (ia < 0.0) ia = 0.0;

    double netRainDepth;
    if (rainDepth > 0.0) {
        // Reduce rain by unused IA
        netRainDepth = rainDepth - ia;
        if (netRainDepth < 0.0) netRainDepth = 0.0;
        // Update IA used
        rd.ia_used += (rainDepth - netRainDepth);
    } else {
        // Recover IA during dry period
        rd.ia_used -= dt_sec / 86400.0 * uh.iaRecov[m][k];
        if (rd.ia_used < 0.0) rd.ia_used = 0.0;
        netRainDepth = 0.0;
    }
    return netRainDepth;
}

// ---------------------------------------------------------------------------
// updateDryPeriod — matches legacy updateDryPeriod().
//
// Resets buffer if dry period exceeds buffer capacity.
// ---------------------------------------------------------------------------
static void updateDryPeriod(UHResponseData& rd, double rainDepth,
                            int rainInterval) {
    if (rainDepth > 0.0) {
        rd.has_past_rain = 1;
        rd.dry_seconds = 0;
    } else {
        rd.dry_seconds += rainInterval;
        if (rd.dry_seconds >=
            static_cast<long>(rd.max_periods) * rainInterval) {
            rd.has_past_rain = 0;
        }
    }
}

// ---------------------------------------------------------------------------
// computeAll — matches legacy getUnitHydRdii() + getUnitHydConvol().
//
// 1. Accumulates rainfall over the rain interval
// 2. At each rain interval boundary, stores rainfall depth (with IA) into
//    per-response circular buffers
// 3. Convolves past rainfall with UH ordinates
// 4. Scatters RDII flow to nodes (area × rdii / UCF(RAINFALL))
// ---------------------------------------------------------------------------
void RDIISolver::computeAll(SimulationContext& ctx, double rainfall,
                             int month, double dt) {
    int unit_sys = ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));

    for (int g = 0; g < groups_.count; ++g) {
        auto ug = static_cast<size_t>(g);
        int uh_i = groups_.uh_idx[ug];
        if (uh_i < 0 || uh_i >= static_cast<int>(uh_params.size())) continue;
        const auto& uh = uh_params[static_cast<size_t>(uh_i)];

        int ri = groups_.rain_interval[ug];

        // Track elapsed time within current rain interval
        groups_.time_accum[ug] += dt;

        // Only process when full rain interval has elapsed
        if (groups_.time_accum[ug] < static_cast<double>(ri)) {
            // Not yet at rain interval boundary — continue with existing
            // convolution from buffer for continuous RDII output.
        } else {
            // Full interval elapsed — compute rainfall depth in one step
            // matching legacy: rainDepth = Gage[g].rainfall * rainInterval / 3600
            // This avoids floating-point accumulation error from summing
            // many small dt increments.
            double rainDepth = rainfall * static_cast<double>(ri) / 3600.0;
            groups_.time_accum[ug] = 0.0;

            // Store into per-response buffers with IA subtraction
            for (int k = 0; k < 3; ++k) {
                auto& rd = groups_.uh_data[ug * 3 + static_cast<size_t>(k)];
                if (rd.max_periods <= 0) continue;

                // Apply initial abstraction (matching legacy applyIA)
                double excessDepth = applyIA(uh, rd, month, k, rainDepth,
                                             static_cast<double>(ri));

                // Update dry period tracking (matching legacy updateDryPeriod)
                updateDryPeriod(rd, excessDepth, ri);

                // Store in circular buffer
                int p = rd.period;
                if (p >= rd.max_periods) p = 0;
                rd.past_rain[static_cast<size_t>(p)] = excessDepth;
                rd.past_month[static_cast<size_t>(p)] = month;
                rd.period = p + 1;
            }
        }

        // Convolution for each response — matches legacy getUnitHydConvol()
        // rdii_group is in rainfall-rate units (in/hr)
        double rdii_group = 0.0;
        for (int k = 0; k < 3; ++k) {
            auto& rd = groups_.uh_data[ug * 3 + static_cast<size_t>(k)];
            if (!rd.has_past_rain || rd.max_periods <= 0) continue;

            int pMax = rd.max_periods;
            // Start from most recent period and work backwards
            int i_buf = rd.period - 1;
            if (i_buf < 0) i_buf = pMax - 1;
            int p = 1;

            while (p < pMax) {
                double v = rd.past_rain[static_cast<size_t>(i_buf)];
                int    m = rd.past_month[static_cast<size_t>(i_buf)];
                if (v > 0.0) {
                    // Mid-point time of UH period (matching legacy)
                    double t = (static_cast<double>(p) - 0.5)
                               * static_cast<double>(ri);
                    // ordinate × R fraction (legacy: getUnitHydOrd * r[m][k])
                    double u = uhOrdinate(uh, m, k, t) * uh.r[m % 12][k];
                    rdii_group += u * v;
                }
                p++;
                i_buf--;
                if (i_buf < 0) i_buf = pMax - 1;
            }
        }

        // Convert RDII from rainfall-rate units to CFS:
        //   rdii_cfs = rdii_group * area_ft2 / UCF(RAINFALL)
        // where area is in project units (acres for US),
        // converted to ft2 by dividing by Ucf[LANDAREA].
        double area_ft2 = groups_.area[ug] / ucf::Ucf[ucf::LANDAREA][unit_sys];
        double rdii_cfs = rdii_group * area_ft2
                        / ucf::Ucf[ucf::RAINFALL][unit_sys];

        // Scatter to node lateral flow
        int ni = groups_.node_idx[ug];
        if (ni >= 0 && ni < static_cast<int>(ctx.nodes.lat_flow.size())) {
            ctx.nodes.lat_flow[static_cast<std::size_t>(ni)] += rdii_cfs;
        }

        // Mass balance tracking
        if (rdii_cfs > 0.0) {
            ctx.mass_balance.step_rdii_inflow += rdii_cfs;
        }
    }
}

} // namespace rdii
} // namespace openswmm
