/**
 * @file RunoffEnsemble.cpp
 * @brief RunoffEnsemble — implementation.
 *
 * @ingroup engine_uncertainty
 */

#include "RunoffEnsemble.hpp"
#include "core/SimulationOptions.hpp"
#include <stdexcept>

namespace openswmm::uncertainty {

// ============================================================================
// initialize
// ============================================================================

void RunoffEnsemble::initialize(int M, const std::vector<double>& soil_mult_in) {
    if (M < 1)
        throw std::invalid_argument(
            "RunoffEnsemble::initialize: n_members must be >= 1");
    if (static_cast<int>(soil_mult_in.size()) != M)
        throw std::invalid_argument(
            "RunoffEnsemble::initialize: soil_mult_in size must equal M");

    n_members  = M;
    soil_mult  = soil_mult_in;
    infil_rates.assign(static_cast<std::size_t>(M), 0.0);

    // Clear any previously active states
    horton_states.clear();
    ga_states.clear();
    cn_states.clear();
}

// ============================================================================
// initHorton
// ============================================================================

void RunoffEnsemble::initHorton(double f0, double fmin, double decay,
                                double regen, double Fmax,
                                const SimulationOptions& opts) {
    horton_states.resize(static_cast<std::size_t>(n_members));
    for (int i = 0; i < n_members; ++i) {
        double m = soil_mult[static_cast<std::size_t>(i)];
        infil::horton_init(horton_states[static_cast<std::size_t>(i)],
                           f0 * m, fmin * m, decay, regen, Fmax, opts);
    }
}

// ============================================================================
// initGreenAmpt
// ============================================================================

void RunoffEnsemble::initGreenAmpt(double S, double Ks, double IMDmax,
                                   const SimulationOptions& opts) {
    ga_states.resize(static_cast<std::size_t>(n_members));
    for (int i = 0; i < n_members; ++i) {
        double m = soil_mult[static_cast<std::size_t>(i)];
        infil::grnampt_init(ga_states[static_cast<std::size_t>(i)],
                            S, Ks * m, IMDmax, opts);
    }
}

// ============================================================================
// initCurveNum
// ============================================================================

void RunoffEnsemble::initCurveNum(double CN, double regen) {
    cn_states.resize(static_cast<std::size_t>(n_members));
    for (int i = 0; i < n_members; ++i) {
        // Scale effective retention Smax by 1/mult[i]: higher mult → lower S →
        // more runoff, consistent with higher conductivity meaning more
        // infiltration capacity.  CN is conceptually the inverse of S.
        double m = soil_mult[static_cast<std::size_t>(i)];
        // Compute adjusted CN that gives Smax/m: CN_adj = 1000/(12*Smax/m+10)
        // Equivalent: pass a modified CN such that the derived S reflects scaling.
        // Simplest correct approach: initialise normally, then rescale Smax and S.
        infil::curvenum_init(cn_states[static_cast<std::size_t>(i)], CN, regen);
        cn_states[static_cast<std::size_t>(i)].Smax /= m;
        cn_states[static_cast<std::size_t>(i)].S    /= m;
    }
}

// ============================================================================
// step
// ============================================================================

void RunoffEnsemble::step(double precip, double depth, double dt,
                          InfilModel model) {
    const auto sz = static_cast<std::size_t>(n_members);

    switch (model) {
    case InfilModel::HORTON:
        for (std::size_t i = 0; i < sz; ++i)
            infil_rates[i] = infil::horton_getInfil(
                horton_states[i], precip, depth, dt);
        break;

    case InfilModel::MOD_HORTON:
        for (std::size_t i = 0; i < sz; ++i)
            infil_rates[i] = infil::modHorton_getInfil(
                horton_states[i], precip, depth, dt);
        break;

    case InfilModel::GREEN_AMPT:
    case InfilModel::MOD_GREEN_AMPT:
        for (std::size_t i = 0; i < sz; ++i)
            infil_rates[i] = infil::grnampt_getInfil(
                ga_states[i], precip, depth, dt, model);
        break;

    case InfilModel::CURVE_NUM:
        for (std::size_t i = 0; i < sz; ++i)
            infil_rates[i] = infil::curvenum_getInfil(
                cn_states[i], precip, depth, dt);
        break;
    }
}

} // namespace openswmm::uncertainty
