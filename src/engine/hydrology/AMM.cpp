/**
 * @file AMM.cpp
 * @brief Antecedent Moisture Model — engine integration.
 *
 * @details Equations numbered per Edgren, Czachorski & Gonwa (2024),
 *          "Reparameterizing the Antecedent Moisture Model",
 *          J. Water Management Modeling, C525.
 *
 * @ingroup new_engine
 * @license  MIT License
 */

#include "AMM.hpp"
#include "../core/SimulationContext.hpp"
#include <cmath>
#include <algorithm>
#include <numeric>

namespace openswmm {
namespace amm {

// ============================================================================
// Public interface
// ============================================================================

int AMMSolver::addComponentParams(const std::string& name, const AMMParams& params) {
    auto it = name_to_idx_.find(name);
    if (it != name_to_idx_.end()) {
        params_[static_cast<size_t>(it->second)] = params;
        return it->second;
    }
    int idx = static_cast<int>(params_.size());
    params_.push_back(params);
    name_to_idx_[name] = idx;
    return idx;
}

int AMMSolver::findComponent(const std::string& name) const {
    auto it = name_to_idx_.find(name);
    if (it == name_to_idx_.end()) return -1;
    return it->second;
}

void AMMSolver::init(SimulationContext& ctx) {
    const auto& ad = ctx.amm_assigns;

    // Transfer parsed component definitions into the solver's parameter registry
    for (const auto& [name, def] : ad.component_defs) {
        AMMParams p;
        p.area      = def.area;
        p.RD        = def.RD;
        p.PAT       = def.PAT;
        p.HHL       = def.HHL;
        p.AMHL      = def.AMHL;
        p.cold_SHCF = def.cold_SHCF;
        p.hot_SHCF  = def.hot_SHCF;
        p.cold_temp = def.cold_temp;
        p.hot_temp  = def.hot_temp;
        p.TAT       = def.TAT;
        p.is_baseflow = def.is_baseflow;
        p.cold_R    = def.cold_R;
        p.hot_R     = def.hot_R;
        addComponentParams(name, p);
    }

    int n = ad.count();
    if (n == 0) {
        assigns_.clear();
        return;
    }

    assigns_.resize(static_cast<size_t>(n));
    states_.resize(static_cast<size_t>(n));
    last_SHCF_.assign(static_cast<size_t>(n), 0.0);
    last_RW_.assign(static_cast<size_t>(n), 0.0);
    last_MAP_.assign(static_cast<size_t>(n), 0.0);

    for (int i = 0; i < n; ++i) {
        auto ui = static_cast<size_t>(i);
        assigns_[ui].node_idx  = ad.node_idx[ui];
        assigns_[ui].area      = ad.sewer_area[ui];

        int pi = findComponent(ad.amm_name[ui]);
        assigns_[ui].param_idx = pi;

        auto& s = states_[ui];
        s.Q_prev = 0.0;
        s.RW_prev = 0.0;

        if (pi >= 0 && pi < static_cast<int>(params_.size())) {
            const auto& p = params_[static_cast<size_t>(pi)];
            // Initial ring-buffer sizing (will be resized on first step if dt differs)
            int precip_slots = 1;
            if (p.PAT > 0.0 && p.HHL > 0.0) {
                double est_dt = std::min(p.HHL, p.PAT);
                precip_slots = static_cast<int>(p.PAT / est_dt) + 2;
            }
            s.precip_buf.assign(static_cast<size_t>(precip_slots), 0.0);
            s.precip_head = 0;

            int temp_slots = 1;
            if (p.TAT > 0.0 && p.HHL > 0.0) {
                double est_dt = std::min(p.HHL, p.TAT);
                temp_slots = static_cast<int>(p.TAT / est_dt) + 2;
            }
            s.temp_buf.assign(static_cast<size_t>(temp_slots), 0.0);
            s.temp_head = 0;
        } else {
            s.precip_buf.assign(1, 0.0);
            s.precip_head = 0;
            s.temp_buf.assign(1, 0.0);
            s.temp_head = 0;
        }
    }
}

void AMMSolver::reset() {
    for (auto& s : states_) {
        s.Q_prev = 0.0;
        s.RW_prev = 0.0;
        std::fill(s.precip_buf.begin(), s.precip_buf.end(), 0.0);
        s.precip_head = 0;
        std::fill(s.temp_buf.begin(), s.temp_buf.end(), 0.0);
        s.temp_head = 0;
    }
    std::fill(last_SHCF_.begin(), last_SHCF_.end(), 0.0);
    std::fill(last_RW_.begin(), last_RW_.end(), 0.0);
    std::fill(last_MAP_.begin(), last_MAP_.end(), 0.0);
}

void AMMSolver::computeAll(SimulationContext& ctx, double rainfall,
                           double air_temp, double dt) {
    if (dt <= 0.0) return;

    int n_nodes = ctx.n_nodes();

    for (size_t c = 0; c < assigns_.size(); ++c) {
        int pi = assigns_[c].param_idx;
        if (pi < 0 || pi >= static_cast<int>(params_.size())) continue;

        const auto& p = params_[static_cast<size_t>(pi)];
        auto& s = states_[c];

        // Use the assigned area, not the parameter area
        double area = assigns_[c].area;

        // Ensure ring-buffer sizes match dt
        int precip_slots = std::max(static_cast<int>(p.PAT / dt) + 2, 1);
        if (static_cast<int>(s.precip_buf.size()) != precip_slots) {
            s.precip_buf.assign(static_cast<size_t>(precip_slots), 0.0);
            s.precip_head = 0;
        }

        int temp_slots = std::max(static_cast<int>(p.TAT / dt) + 2, 1);
        if (static_cast<int>(s.temp_buf.size()) != temp_slots) {
            s.temp_buf.assign(static_cast<size_t>(temp_slots), 0.0);
            s.temp_head = 0;
        }

        // Eq 3: Moving Average Precipitation
        double MAP = updateMovingAverage(s.precip_buf, s.precip_head, rainfall);

        // Eq 11: Moving Average Temperature
        double MATemp = updateMovingAverage(s.temp_buf, s.temp_head, air_temp);

        // Eq 2: Shape factor
        double SF = shapeFactor(dt, p.HHL);

        double Q_t;
        double RW_t;

        if (p.is_baseflow) {
            // Baseflow component (2-level, Eq 12-16)
            double R_t = computeBaseflowR(MATemp, p.cold_R, p.hot_R,
                                          p.cold_temp, p.hot_temp);
            Q_t = computeQ(area, 0.0, R_t, s.RW_prev,
                           MAP, SF, dt, s.Q_prev, p.area_to_flow);
            RW_t = R_t;
            last_SHCF_[c] = 0.0;
        } else {
            // Standard 3-level component (Eq 1-11)
            double SHCF = computeSHCF(MATemp,
                                       p.cold_SHCF, p.hot_SHCF,
                                       p.cold_temp, p.hot_temp);
            double AMRF = antecedentMoistureRetentionFactor(dt, p.AMHL);
            RW_t = computeRW(AMRF, SHCF, MAP, s.RW_prev);
            Q_t = computeQ(area, p.RD, RW_t, s.RW_prev,
                           MAP, SF, dt, s.Q_prev, p.area_to_flow);
            last_SHCF_[c] = SHCF;
        }

        last_RW_[c] = RW_t;
        last_MAP_[c] = MAP;

        // Scatter flow to node lateral inflow
        int ni = assigns_[c].node_idx;
        if (ni >= 0 && ni < n_nodes) {
            ctx.nodes.lat_flow[static_cast<size_t>(ni)] += Q_t;
        }

        s.Q_prev  = Q_t;
        s.RW_prev = RW_t;
    }
}

// ============================================================================
// Static helpers — paper equations
// ============================================================================

// Eq 2: SF = 0.5^(dt / HHL)
double AMMSolver::shapeFactor(double dt, double HHL) {
    if (HHL <= 0.0) return 0.0;
    return std::pow(0.5, dt / HHL);
}

// Eq 6: AMRF = 0.5^(dt / AMHL)
double AMMSolver::antecedentMoistureRetentionFactor(double dt, double AMHL) {
    if (AMHL <= 0.0) return 0.0;
    return std::pow(0.5, dt / AMHL);
}

// Eq 3 / Eq 11: Circular-buffer moving average
double AMMSolver::updateMovingAverage(std::vector<double>& buf, int& head,
                                       double new_value) {
    if (buf.empty()) return new_value;
    buf[static_cast<size_t>(head % static_cast<int>(buf.size()))] = new_value;
    head = (head + 1) % static_cast<int>(buf.size());

    double sum = 0.0;
    for (double v : buf) sum += v;
    return sum / static_cast<double>(buf.size());
}

// Eq 7-10: SHCF from temperature sigmoid
double AMMSolver::computeSHCF(double MATemp,
                              double cold_SHCF, double hot_SHCF,
                              double cold_temp, double hot_temp) {
    double L = 1.2 * (cold_SHCF - hot_SHCF);
    double temp_range = cold_temp - hot_temp;
    if (std::abs(temp_range) < 1.0e-10) return cold_SHCF;

    double k = 4.7964 / temp_range;
    double x0 = (cold_temp + hot_temp) / 2.0;
    double SHCF = L / (1.0 + std::exp(-k * (MATemp - x0)))
                + cold_SHCF - (11.0 / 12.0) * L;
    return SHCF;
}

// Eq 5: RW_t from antecedent moisture
double AMMSolver::computeRW(double AMRF, double SHCF, double MAP,
                            double RW_prev) {
    if (AMRF <= 0.0 || AMRF >= 1.0 - 1.0e-15) {
        return SHCF * MAP + RW_prev;
    }
    double correction = (AMRF - 1.0) / std::log(AMRF);
    return std::max(correction * SHCF * MAP + AMRF * RW_prev, 0.0);
}

// Eq 13: Baseflow capture fraction
double AMMSolver::computeBaseflowR(double MATemp,
                                   double cold_R, double hot_R,
                                   double cold_temp, double hot_temp) {
    double L = 1.2 * (cold_R - hot_R);
    double temp_range = cold_temp - hot_temp;
    if (std::abs(temp_range) < 1.0e-10) return cold_R;

    double k = 4.7964 / temp_range;
    double x0 = (cold_temp + hot_temp) / 2.0;
    double R = L / (1.0 + std::exp(-k * (MATemp - x0)))
             + cold_R - (11.0 / 12.0) * L;
    return std::max(R, 0.0);
}

// Eq 1: Q_t = A * (RD + (RW_t + RW_prev)/2) * MAP * (1-SF)/dt + SF * Q_prev
double AMMSolver::computeQ(double area, double RD, double RW_t, double RW_prev,
                           double MAP, double SF, double dt,
                           double Q_prev, double area_to_flow) {
    if (dt <= 0.0) return 0.0;
    double capture = std::max(RD + (RW_t + RW_prev) / 2.0, 0.0);
    double Q_new = area * area_to_flow * capture * MAP * (1.0 - SF) / dt;
    return std::max(Q_new + SF * Q_prev, 0.0);
}

} // namespace amm
} // namespace openswmm
