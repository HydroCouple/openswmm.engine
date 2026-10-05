// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin

#include "InfilBank.hpp"
#include "../../core/SimulationOptions.hpp"
#include "../../core/UnitConversion.hpp"
#include <algorithm>

namespace openswmm::surface {
void InfilBank::init(int n) {
    clear();
    methods_.assign(n, InfilModel::HORTON);
    owners_.assign(n, Owner::BANK);
    constant_.assign(n, 0.0);
    cumulative_.assign(n, 0.0);
}
void InfilBank::clear() {
    methods_.clear(); owners_.clear(); horton_.clear(); grnampt_.clear();
    curvenum_.clear(); constant_.clear(); cumulative_.clear();
}
void InfilBank::setMethod(int e, InfilModel method, const double p[5], const SimulationOptions& opts) {
    methods_[e] = method;
    switch (method) {
    case InfilModel::HORTON:
    case InfilModel::MOD_HORTON:
        if (horton_.empty()) horton_.resize(methods_.size());
        infil::horton_init(horton_[e], p[0], p[1], p[2], p[3], p[4], opts);
        break;
    case InfilModel::GREEN_AMPT:
    case InfilModel::MOD_GREEN_AMPT:
        if (grnampt_.empty()) grnampt_.resize(methods_.size());
        infil::grnampt_init(grnampt_[e], p[0], p[1], p[2], opts);
        break;
    case InfilModel::CURVE_NUM:
        if (curvenum_.empty()) curvenum_.resize(methods_.size());
        infil::curvenum_init(curvenum_[e], p[0], p[2]);
        break;
    case InfilModel::CONSTANT:
        // Dedicated stateless rate, preserving the original init operation order.
        constant_[e] = p[0] / ucf::UCF(ucf::RAINFALL, opts);
        break;
    }
}
double InfilBank::rateFeet(int e, double precip, double runon, double depth, double dt, Factors factors) {
    if (owners_[e] == Owner::EXTERNAL || dt <= 0.0) return 0.0;
    const double f = evaluate(e, precip, runon, depth, dt, factors);
    cumulative_[e] += std::max(0.0, f * 0.3048) * dt;
    return f;
}
double InfilBank::rate(int e, double precip, double runon, double depth, double dt, Factors factors) {
    constexpr double feet_per_metre = 3.280839895013123;
    return std::max(0.0, rateFeet(e, precip * feet_per_metre, runon * feet_per_metre,
                                depth * feet_per_metre, dt, factors) * 0.3048);
}
double InfilBank::evaluate(int i, double precip, double runon, double depth, double dt, Factors factors) {
    const auto ui = static_cast<std::size_t>(i);
    double infil = 0.0;
    const InfilModel im = methods_[ui];
    switch (im) {
        case InfilModel::HORTON:
        case InfilModel::MOD_HORTON: {
            auto& hs = horton_[ui];
            double save_f0 = hs.f0, save_fmin = hs.fmin, save_regen = hs.regen;
            hs.f0   *= factors.infiltration;
            hs.fmin *= factors.infiltration;
            hs.regen *= factors.recovery;
            infil = (im == InfilModel::HORTON)
                ? infil::horton_getInfil(hs, precip + runon, depth, dt)
                : infil::modHorton_getInfil(hs, precip + runon, depth, dt);
            hs.f0 = save_f0; hs.fmin = save_fmin; hs.regen = save_regen;
            break;
        }
        case InfilModel::GREEN_AMPT:
        case InfilModel::MOD_GREEN_AMPT: {
            // Legacy applies InfilFactor and Evap.recoveryFactor inside
            // grnampt_getInfil, per call, from the unscaled state (ks = Ks*IF,
            // lu = Lu*sqrt(IF), Fumax = IMDmax*Lu*sqrt(IF), kr = lu/90000*RF,
            // T = 5400/lu/RF). Baking them into Ks/Lu/Fumax changed both the
            // operation order and Fumax (which scaled by IF instead of sqrt(IF)).
            infil = infil::grnampt_getInfil(grnampt_[ui],
                                            precip + runon, depth, dt, im,
                                            factors.infiltration, factors.recovery);
            break;
        }
        case InfilModel::CURVE_NUM: {
            auto& cs = curvenum_[ui];
            // Gap #7: CN treats runon as ponded depth, not as a rainfall rate.
            // Legacy infil.c lines 318-319: depth += runon * tstep; then pass
            // only rainfall (not rainfall+runon) as the rate argument.
            double cn_depth = depth + runon * dt;
            infil = infil::curvenum_getInfil(cs, precip, cn_depth, dt,
                                             factors.recovery);
            break;
        }
        case InfilModel::CONSTANT:
            infil = infil::constant_getInfil(constant_[ui] * factors.infiltration, precip + runon, depth, dt);
            break;
    }
    return infil;
}

void InfilBank::pack(int i, int& model, double state[6]) const noexcept {
    std::fill(state, state + 6, 0.0);
    if (i < 0 || static_cast<std::size_t>(i) >= methods_.size()) {
        model = 0;
        return;
    }
    const auto ui = static_cast<std::size_t>(i);
    model = static_cast<int>(methods_[ui]);

    switch (methods_[ui]) {
        case InfilModel::HORTON:
        case InfilModel::MOD_HORTON: {
            const auto& h = horton_[ui];
            state[0] = h.tp;
            state[1] = h.Fe;
            state[2] = h.Fmh;
            break;
        }
        case InfilModel::GREEN_AMPT:
        case InfilModel::MOD_GREEN_AMPT: {
            const auto& g = grnampt_[ui];
            state[0] = g.IMD;
            state[1] = g.F;
            state[2] = g.Fu;
            state[3] = g.T;
            state[4] = g.saturated ? 1.0 : 0.0;
            break;
        }
        case InfilModel::CURVE_NUM: {
            const auto& c = curvenum_[ui];
            state[0] = c.S;
            state[1] = c.Se;
            state[2] = c.P;
            state[3] = c.F;
            state[4] = c.f;
            state[5] = c.T;
            break;
        }
        case InfilModel::CONSTANT:
            break;  // 2D-only, stateless — nothing to save
    }
}

void InfilBank::unpack(int i, int model, const double state[6]) noexcept {
    if (i < 0 || static_cast<std::size_t>(i) >= methods_.size()) return;
    const auto ui = static_cast<std::size_t>(i);

    // Only restore if model matches the initialised type
    if (model != static_cast<int>(methods_[ui])) return;

    switch (methods_[ui]) {
        case InfilModel::HORTON:
        case InfilModel::MOD_HORTON: {
            auto& h = horton_[ui];
            h.tp  = state[0];
            h.Fe  = state[1];
            h.Fmh = state[2];
            break;
        }
        case InfilModel::GREEN_AMPT:
        case InfilModel::MOD_GREEN_AMPT: {
            auto& g = grnampt_[ui];
            g.IMD       = state[0];
            g.F         = state[1];
            g.Fu        = state[2];
            g.T         = state[3];
            g.saturated = (state[4] != 0.0);
            break;
        }
        case InfilModel::CURVE_NUM: {
            auto& c = curvenum_[ui];
            c.S  = state[0];
            c.Se = state[1];
            c.P  = state[2];
            c.F  = state[3];
            c.f  = state[4];
            c.T  = state[5];
            break;
        }
        case InfilModel::CONSTANT:
            break;  // 2D-only, stateless — nothing to restore
    }
}

} // namespace openswmm::surface
