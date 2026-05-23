/**
 * @file RDII.cpp
 * @brief RDII unit hydrograph convolution — matching legacy rdii.c.
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "RDII.hpp"
#include "../core/SimulationContext.hpp"
#include "../core/UnitConversion.hpp"
#include "../core/DateTime.hpp"
#include <cmath>
#include <algorithm>

namespace openswmm {
namespace rdii {

void RDIIGroupSoA::resize(int n) {
    count = n;
    auto un = static_cast<size_t>(n);
    node_idx.assign(un, -1);
    uh_idx.assign(un, -1);
    gage_idx.assign(un, -1);
    area.assign(un, 0.0);
    uh_data.resize(un * 3);  // 3 responses per group
    rain_interval.assign(un, 300);
    time_accum.assign(un, 0.0);
    rain_at_start.assign(un, 0.0);
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
// validateExpDecay — warn if exponential decay is configured but no temperature
// data source is available; recovery will fall back to T_ref in that case.
// ---------------------------------------------------------------------------
void RDIISolver::validateExpDecay(SimulationContext& ctx) const {
    bool any_active = false;
    for (const auto& triple : decay_params) {
        for (const auto& dp : triple) {
            if (dp.active) { any_active = true; break; }
        }
        if (any_active) break;
    }
    if (any_active && ctx.options.temp_source == 0) {
        ctx.warnings.emplace_back(
            "WARNING: [RDII_DECAY] exponential IA model is active but no "
            "temperature source is configured. Recovery rate will be evaluated "
            "at T_ref for every group; no seasonal variation will be produced.");
    }
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
        double tBase_sec = entry.t * (1.0 + entry.k) * 3600.0;

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

    // Resize decay_params parallel to uh_params; default-constructed entries
    // mean `active == false`, i.e. the response uses the linear IA model.
    decay_params.assign(uh_params.size(), std::array<ExpDecayParams, 3>{});

    // Populate decay_params from parsed [RDII_DECAY] data
    for (const auto& d : ctx.rdii_decay.entries) {
        int idx = findUnitHyd(d.uh_name);
        if (idx < 0) continue;            // unknown UH group — silently skip
        if (d.response < 0 || d.response > 2) continue;
        auto& dp = decay_params[static_cast<size_t>(idx)][static_cast<size_t>(d.response)];
        dp.active    = true;
        dp.k_dep     = d.k_dep;
        dp.k_0       = d.k_0;
        dp.k_T       = d.k_T;
        dp.T_ref     = d.T_ref;
        dp.theta_rec = d.theta_rec;
        dp.T_freeze  = d.T_freeze;
    }
    validateExpDecay(ctx);

    // Build UH name → gage index mapping from parsed [HYDROGRAPHS] gage lines.
    // Legacy: each UnitHyd[j] has a rainGage field set during parsing.
    std::unordered_map<std::string, int> uh_gage_map;
    for (size_t gi = 0; gi < ctx.unit_hyds.gage_assignments.size(); ++gi) {
        const auto& uh_name = ctx.unit_hyds.gage_assignments[gi];
        const auto& gage_name = ctx.unit_hyds.gage_names[gi];
        int gidx = ctx.gage_names.find(gage_name);
        if (gidx >= 0) {
            uh_gage_map[uh_name] = gidx;
        }
    }

    const auto& assigns = ctx.rdii_assigns;
    int n_assigns = assigns.count();
    if (n_assigns == 0) {
        groups_.count = 0;
        return;
    }

    groups_.resize(n_assigns);
    node_rdii_flow_.assign(static_cast<size_t>(ctx.n_nodes()), 0.0);
    double wet_step = ctx.options.wet_step;

    for (int i = 0; i < n_assigns; ++i) {
        auto ui = static_cast<size_t>(i);
        groups_.node_idx[ui] = assigns.node_idx[ui];
        groups_.area[ui]     = assigns.sewer_area[ui];

        const std::string& uh_name = assigns.uh_name[ui];
        int uh_i = findUnitHyd(uh_name);
        groups_.uh_idx[ui] = uh_i;

        // Resolve per-UH rain gage (legacy: UnitHyd[j].rainGage)
        auto git = uh_gage_map.find(uh_name);
        groups_.gage_idx[ui] = (git != uh_gage_map.end()) ? git->second : -1;

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
// updateIA_linear — matches legacy applyIA() exactly.
//
// Subtracts initial abstraction from rainfall depth.
// During dry periods, recovers IA at iaRecov rate.
// ---------------------------------------------------------------------------
static double updateIA_linear(const UnitHydParams& uh, UHResponseData& rd,
                              int month, int response,
                              double rainDepth, double dt_sec) {
    int m = month % 12;
    int k = response % 3;
    double iaMax = uh.iaMax[m][k];

    // Determine unused IA
    double ia = iaMax - rd.ia_used;
    ia = std::max(ia, 0.0);

    double netRainDepth;
    if (rainDepth > 0.0) {
        // Reduce rain by unused IA
        netRainDepth = rainDepth - ia;
        netRainDepth = std::max(netRainDepth, 0.0);
        // Update IA used
        rd.ia_used += (rainDepth - netRainDepth);
    } else {
        // Recover IA during dry period
        rd.ia_used -= dt_sec / 86400.0 * uh.iaRecov[m][k];
        rd.ia_used = std::max(rd.ia_used, 0.0);
        netRainDepth = 0.0;
    }
    return netRainDepth;
}

// ---------------------------------------------------------------------------
// Additive recovery rate k_rec(T) = k_0 + k_T * exp(theta_rec * (T - T_ref))
// with frozen-ground suppression below T_freeze. T is in deg Celsius.
// @see docs/RDII_ExpDecay_Implementation.md §2.2
// ---------------------------------------------------------------------------
static double getRecoveryRate(const ExpDecayParams& dp, double T_celsius) {
    if (T_celsius <= dp.T_freeze) return 0.0;
    double k = dp.k_0 + dp.k_T
               * std::exp(dp.theta_rec * (T_celsius - dp.T_ref));
    return std::max(0.0, k);
}

/// Fahrenheit → Celsius. ClimateState.temperature stores degrees F.
static inline double fToC(double tf) { return (tf - 32.0) * 5.0 / 9.0; }

// ---------------------------------------------------------------------------
// updateIA_exp — exponential IA depletion during storms, additive temperature-
// dependent recovery during dry periods. Falls back to T_ref when no
// temperature source is configured (warned at init time).
// @see docs/RDII_ExpDecay_Implementation.md §2.1
// ---------------------------------------------------------------------------
static double updateIA_exp(const UnitHydParams& uh, UHResponseData& rd,
                           const ExpDecayParams& dp,
                           int month, int response,
                           double rainDepth, double dt_sec,
                           const SimulationContext& ctx) {
    int m = month % 12;
    int k = response % 3;
    double iaMax = uh.iaMax[m][k];

    double ia_avail = iaMax - rd.ia_used;
    ia_avail = std::clamp(ia_avail, 0.0, iaMax);

    if (rainDepth > 0.0) {
        // Exponential depletion — temperature-independent
        double ia_new = ia_avail * std::exp(-dp.k_dep * rainDepth);
        ia_new = std::max(0.0, ia_new);
        double ia_consumed = ia_avail - ia_new;
        double netRain = std::max(0.0, rainDepth - ia_consumed);
        rd.ia_used = iaMax - ia_new;
        return netRain;
    }

    // Dry — additive recovery
    double T_c = (ctx.options.temp_source != 0)
                   ? fToC(ctx.climate_state.temperature)
                   : dp.T_ref;
    double kr = getRecoveryRate(dp, T_c);
    if (kr <= 0.0) return 0.0;  // frozen ground or fully recovered

    double dt_hr = dt_sec / 3600.0;
    double ia_new = iaMax - (iaMax - ia_avail) * std::exp(-kr * dt_hr);
    ia_new = std::clamp(ia_new, 0.0, iaMax);
    rd.ia_used = iaMax - ia_new;
    return 0.0;
}

// ---------------------------------------------------------------------------
// updateDryPeriod — matches legacy updateDryPeriod().
//
// Resets buffer if dry period exceeds buffer capacity.
// ---------------------------------------------------------------------------
static void updateDryPeriod(UHResponseData& rd, double rainDepth,
                            int rainInterval) {
    if (rainDepth > 0.0) {
        // If previous dry period was long enough, begin a new RDII event
        // by clearing the buffer and resetting period to 0
        // (matching legacy rdii.c lines 1278-1286)
        if (rd.dry_seconds >=
            static_cast<long>(rainInterval) * rd.max_periods) {
            for (int i = 0; i < rd.max_periods; ++i)
                rd.past_rain[static_cast<size_t>(i)] = 0.0;
            rd.period = 0;
        }
        rd.dry_seconds = 0;
        rd.has_past_rain = 1;
    } else {
        rd.dry_seconds += rainInterval;
        if (rd.dry_seconds >=
            static_cast<long>(rainInterval) * rd.max_periods) {
            rd.has_past_rain = 0;
        } else {
            rd.has_past_rain = 1;
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
void RDIISolver::computeAll(SimulationContext& ctx, int month, double dt) {
    int unit_sys = ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));

    // Zero the per-node RDII buffer before accumulating
    std::fill(node_rdii_flow_.begin(), node_rdii_flow_.end(), 0.0);

    for (int g = 0; g < groups_.count; ++g) {
        auto ug = static_cast<size_t>(g);
        int uh_i = groups_.uh_idx[ug];
        if (uh_i < 0 || uh_i >= static_cast<int>(uh_params.size())) continue;
        const auto& uh = uh_params[static_cast<size_t>(uh_i)];

        int ri = groups_.rain_interval[ug];

        // Read rainfall from this group's assigned gage (legacy: UnitHyd[j].rainGage)
        int gi = groups_.gage_idx[ug];
        double rainfall = 0.0;
        if (gi >= 0 && gi < static_cast<int>(ctx.gages.rainfall.size())) {
            rainfall = ctx.gages.rainfall[static_cast<size_t>(gi)];
        }

        // Capture rainfall at start of each interval (matching legacy
        // gage_setState at gageDate, which is the interval start time).
        if (groups_.time_accum[ug] == 0.0) {
            groups_.rain_at_start[ug] = rainfall;
        }

        // Track elapsed time within current rain interval
        groups_.time_accum[ug] += dt;

        // Only process when full rain interval has elapsed
        if (groups_.time_accum[ug] < static_cast<double>(ri)) {
            // Not yet at rain interval boundary — continue with existing
            // convolution from buffer for continuous RDII output.
        } else {
            // Full interval elapsed — compute rainfall depth using the
            // rainfall rate captured at the START of the interval (matching
            // legacy: gage_setState(g, gageDate) where gageDate is the
            // interval start time).
            double rainDepth = groups_.rain_at_start[ug]
                             * static_cast<double>(ri) / 3600.0;
            groups_.time_accum[ug] = 0.0;

            // Store into per-response buffers with IA subtraction
            for (int k = 0; k < 3; ++k) {
                auto& rd = groups_.uh_data[ug * 3 + static_cast<size_t>(k)];
                if (rd.max_periods <= 0) continue;

                // Apply initial abstraction — exponential model if a
                // [RDII_DECAY] row is active for this (group, response),
                // otherwise legacy linear iaRecov.
                bool exp_on = (uh_i < static_cast<int>(decay_params.size())) &&
                              decay_params[static_cast<size_t>(uh_i)]
                                  [static_cast<size_t>(k)].active;
                double excessDepth = exp_on
                    ? updateIA_exp(uh, rd,
                                   decay_params[static_cast<size_t>(uh_i)]
                                                [static_cast<size_t>(k)],
                                   month, k, rainDepth,
                                   static_cast<double>(ri), ctx)
                    : updateIA_linear(uh, rd, month, k, rainDepth,
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

        // Buffer RDII flow per node (applied to lat_flow later via applyRdiiInflows)
        int ni = groups_.node_idx[ug];
        if (ni >= 0 && ni < static_cast<int>(node_rdii_flow_.size())) {
            node_rdii_flow_[static_cast<std::size_t>(ni)] += rdii_cfs;
        }
    }
}

// ---------------------------------------------------------------------------
// applyRdiiInflows — add buffered RDII flows to node lateral inflows.
// Matching legacy addRdiiInflows() in routing.c which reads pre-computed
// RDII from the interface file and adds to Node[j].newLatFlow.
// ---------------------------------------------------------------------------
void RDIISolver::applyRdiiInflows(SimulationContext& ctx) const {
    for (int i = 0; i < static_cast<int>(node_rdii_flow_.size()); ++i) {
        double q = node_rdii_flow_[static_cast<size_t>(i)];
        if (q == 0.0) continue;
        ctx.nodes.rdii_inflow[static_cast<size_t>(i)] += q;
    }
}

} // namespace rdii
} // namespace openswmm
