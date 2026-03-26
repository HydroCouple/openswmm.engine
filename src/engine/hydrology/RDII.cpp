/**
 * @file RDII.cpp
 * @brief RDII unit hydrograph convolution — numerically identical to legacy.
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "RDII.hpp"
#include "../core/SimulationContext.hpp"
#include <cmath>
#include <algorithm>

namespace openswmm {
namespace rdii {

void RDIIGroupSoA::resize(int n) {
    count = n;
    auto un = static_cast<size_t>(n);
    node_idx.assign(un, -1);
    uh_idx.assign(un, -1);
    area.assign(un, 0.0);
    past_rain.resize(un);
    past_month.resize(un);
    period.assign(un, 0);
    max_periods.assign(un, 0);
}

double RDIISolver::uhOrdinate(const UnitHydParams& uh, int month, int response, double t) const {
    int m = month % 12;
    int k = response % 3;
    double tp = uh.tPeak[m][k];
    double tb = uh.tBase[m][k];
    if (tp <= 0.0 || tb <= 0.0) return 0.0;

    double qPeak = 2.0 / (tb * 3600.0);  // peak ordinate
    double t2 = tb - tp;

    double f;
    if (t <= tp) {
        f = (tp > 0.0) ? t / tp : 0.0;
    } else {
        f = (t2 > 0.0) ? 1.0 - (t - tp) / t2 : 0.0;
    }
    f = std::max(f, 0.0);
    return f * qPeak;
}

int RDIISolver::addUnitHydParams(const std::string& name, const UnitHydParams& params) {
    auto it = uh_name_to_idx_.find(name);
    if (it != uh_name_to_idx_.end()) {
        // Update existing entry
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
    // Legacy: getRainInterval() — start with wet step, reduce to smallest
    // UH limb duration across all months and responses
    int ri = static_cast<int>(wet_step);
    if (ri <= 0) ri = 300; // default 5 min

    for (int m = 0; m < 12; ++m) {
        for (int k = 0; k < 3; ++k) {
            double tp = uh.tPeak[m][k];
            if (tp > 0.0) {
                int tLimb = static_cast<int>(tp);
                if (tLimb > 0) ri = std::min(ri, tLimb);

                int tFalling = static_cast<int>(uh.tBase[m][k] - tp);
                if (tFalling > 0) ri = std::min(ri, tFalling);
            }
        }
    }
    return std::max(ri, 1); // never zero
}

int RDIISolver::getMaxPeriods(const UnitHydParams& uh, int response, int rainInterval) {
    // Legacy: getMaxPeriods() — max across all 12 months of (tBase / interval + 1)
    if (rainInterval <= 0) return 0;
    int k = response % 3;
    int nMax = 0;
    for (int m = 0; m < 12; ++m) {
        int n = static_cast<int>(uh.tBase[m][k] / rainInterval) + 1;
        nMax = std::max(n, nMax);
    }
    return nMax;
}

void RDIISolver::init(SimulationContext& ctx) {
    const auto& assigns = ctx.rdii_assigns;
    int n_assigns = assigns.count();
    if (n_assigns == 0) {
        groups_.count = 0;
        return;
    }

    // Resolve each RDII assignment to a group entry
    // Each assignment = one group (node + UH + area)
    groups_.resize(n_assigns);

    double wet_step = ctx.options.wet_step;

    for (int i = 0; i < n_assigns; ++i) {
        auto ui = static_cast<size_t>(i);
        groups_.node_idx[ui] = assigns.node_idx[ui];
        groups_.area[ui]     = assigns.sewer_area[ui];

        // Resolve UH name to index
        const std::string& uh_name = assigns.uh_name[ui];
        int uh_i = findUnitHyd(uh_name);
        groups_.uh_idx[ui] = uh_i;

        if (uh_i < 0 || uh_i >= static_cast<int>(uh_params.size())) {
            // UH not found — skip this group (no buffer needed)
            groups_.max_periods[ui] = 0;
            continue;
        }

        const auto& uh = uh_params[static_cast<size_t>(uh_i)];

        // Compute rain interval for this UH group (min limb duration)
        int rainInterval = getRainInterval(uh, wet_step);

        // Compute max periods for each of the 3 responses, take the maximum
        int maxPer = 0;
        for (int k = 0; k < 3; ++k) {
            int mp = getMaxPeriods(uh, k, rainInterval);
            maxPer = std::max(maxPer, mp);
        }

        groups_.max_periods[ui] = maxPer;

        // Allocate circular buffers
        if (maxPer > 0) {
            groups_.past_rain[ui].assign(static_cast<size_t>(maxPer), 0.0);
            groups_.past_month[ui].assign(static_cast<size_t>(maxPer), 0);
        }

        // Initialize period counter so first rainfall triggers new event
        // Legacy: period = maxPeriods + 1, drySeconds = maxPeriods * interval + 1
        groups_.period[ui] = 0;
    }
}

void RDIISolver::computeAll(SimulationContext& ctx, double rainfall,
                             int month, double dt) {
    for (int g = 0; g < groups_.count; ++g) {
        auto ug = static_cast<size_t>(g);
        int uh_i = groups_.uh_idx[ug];
        if (uh_i < 0 || uh_i >= static_cast<int>(uh_params.size())) continue;
        const auto& uh = uh_params[static_cast<size_t>(uh_i)];

        int mp = groups_.max_periods[ug];
        if (mp <= 0) continue;

        // Store current rainfall in circular buffer
        int p = groups_.period[ug];
        groups_.past_rain[ug][static_cast<size_t>(p % mp)] = rainfall * dt;
        groups_.past_month[ug][static_cast<size_t>(p % mp)] = month;
        groups_.period[ug] = p + 1;

        // Convolution: sum over 3 response types × past periods
        double rdii_flow = 0.0;
        for (int k = 0; k < 3; ++k) {
            double r_frac = uh.r[month % 12][k];
            if (r_frac <= 0.0) continue;

            // Inner product over past periods — vectorisable
            for (int i = 0; i < std::min(p + 1, mp); ++i) {
                int idx = (p - i) % mp;
                if (idx < 0) idx += mp;
                auto ui = static_cast<size_t>(idx);

                double past_r = groups_.past_rain[ug][ui];
                if (past_r <= 0.0) continue;

                int past_m = groups_.past_month[ug][ui];
                double t = (static_cast<double>(i) + 0.5) * dt;
                double u = uhOrdinate(uh, past_m, k, t);

                rdii_flow += past_r * r_frac * u * groups_.area[ug];
            }
        }

        // Scatter to node lateral flow
        int ni = groups_.node_idx[ug];
        if (ni >= 0 && ni < static_cast<int>(ctx.nodes.lat_flow.size())) {
            ctx.nodes.lat_flow[static_cast<std::size_t>(ni)] += rdii_flow;
        }
    }
}

} // namespace rdii
} // namespace openswmm
