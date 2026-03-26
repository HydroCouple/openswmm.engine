/**
 * @file Infiltration.cpp
 * @brief Infiltration models — numerically identical to legacy infil.c.
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "Infiltration.hpp"
#include "../core/UnitConversion.hpp"
#include "../core/SimulationOptions.hpp"
#include <cmath>
#include <algorithm>

namespace openswmm {
namespace infil {

static constexpr double TINY = 1.0e-10;  // Matches legacy ZERO = 1.E-10 (consts.h)

// ============================================================================
// Horton
// ============================================================================

void horton_init(HortonState& state, double f0, double fmin,
                 double decay, double regen, double Fmax,
                 const SimulationOptions& opts) {
    // Convert from project rain units (in/hr or mm/hr) to ft/sec via UCF
    double ucf_rain  = ucf::UCF(ucf::RAINFALL, opts);
    double ucf_depth = ucf::UCF(ucf::RAINDEPTH, opts);
    state.f0    = f0 / ucf_rain;
    state.fmin  = fmin / ucf_rain;
    state.decay = decay / 3600.0;     // 1/hr → 1/sec (time-only, unit-system independent)
    // regen parameter is drying time in days (matching legacy infil.c line 350)
    // Legacy: regen = -log(1.0 - 0.98) / p[3] / SECperDAY
    state.regen = (regen > 0.0) ? -std::log(1.0 - 0.98) / regen / ucf::SEC_PER_DAY : 0.0;
    state.Fmax  = (Fmax > 0.0) ? Fmax / ucf_depth : 0.0;  // in/mm → ft (0 = unlimited)
    state.tp    = 0.0;
    state.Fe    = 0.0;
}

double horton_getInfil(HortonState& state, double precip, double depth, double dt) {
    double ia = precip + depth / dt;  // available water rate (ft/sec)

    if (ia <= 0.0) {
        // Dry period — recovery
        if (state.regen > 0.0 && state.tp > 0.0) {
            double r = std::exp(-state.regen * dt);
            double kd = state.decay;
            if (kd > 0.0) {
                double x = 1.0 - std::exp(-kd * state.tp);
                state.tp = (x > 0.0) ? -std::log(1.0 - r * x) / kd : 0.0;
            }
            if (state.tp < 0.0) state.tp = 0.0;
        }
        return 0.0;
    }

    // Wet period — matching legacy infil.c horton_getInfil() exactly
    double f0   = state.f0;     // InfilFactor applied at call-site when monthly patterns are resolved
    double fmin = state.fmin;   // InfilFactor applied at call-site when monthly patterns are resolved
    double df   = f0 - fmin;
    double kd   = state.decay;
    double Fmax = state.Fmax;
    double tp   = state.tp;
    double kr   = state.regen;  // Recovery factor applied at call-site via ClimateState.recovery_factor

    // Special cases: no infiltration or constant infiltration
    if (df < 0.0 || kd < 0.0 || kr < 0.0) return 0.0;
    if (df == 0.0 || kd == 0.0) {
        double fp = f0;
        if (fp > ia) fp = ia;
        return std::max(0.0, fp);
    }

    double fp = 0.0;
    double tlim = 16.0 / kd;

    if (ia > TINY) {
        // Compute average infiltration rate over timestep
        double t1 = tp + dt;
        double Fp, F1;
        if (tp >= tlim) {
            Fp = fmin * tp + df / kd;
            F1 = Fp + fmin * dt;
        } else {
            Fp = fmin * tp + df / kd * (1.0 - std::exp(-kd * tp));
            F1 = fmin * t1 + df / kd * (1.0 - std::exp(-kd * t1));
        }
        fp = (F1 - Fp) / dt;
        fp = std::max(fp, fmin);  // Floor at fmin (matching legacy line 443)

        // Limit infiltration rate to available water
        if (fp > ia) fp = ia;

        // Update cumulative time tp
        if (t1 > tlim) {
            tp = t1;
        } else if (fp < ia) {
            // Infiltration capacity not exhausted
            tp = t1;
        } else {
            // Infiltration limited by available water — Newton-Raphson
            // to find tp where F(tp) matches actual infiltrated volume
            F1 = Fp + fp * dt;
            tp = tp + dt / 2.0;
            for (int iter = 0; iter < 20; ++iter) {
                double kt = std::min(60.0, kd * tp);
                double ex = std::exp(-kt);
                double FF = fmin * tp + df / kd * (1.0 - ex) - F1;
                double FF1 = fmin + df * ex;
                double r = FF / FF1;
                tp -= r;
                if (std::fabs(r) <= 0.001 * dt) break;
            }
        }

        // Limit cumulative infiltration to Fmax
        if (Fmax > 0.0) {
            if (state.Fe + fp * dt > Fmax)
                fp = (Fmax - state.Fe) / dt;
            fp = std::max(fp, 0.0);
            state.Fe += fp * dt;
        }
    }
    // Dry period with regeneration
    else if (kr > 0.0) {
        double r = std::exp(-kr * dt);
        tp = 1.0 - std::exp(-kd * tp);
        tp = -std::log(1.0 - r * tp) / kd;

        if (Fmax > 0.0) {
            state.Fe = fmin * tp + (df / kd) * (1.0 - std::exp(-kd * tp));
        }
    }

    state.tp = tp;
    return fp;
}

// ============================================================================
// Green-Ampt
// ============================================================================

void grnampt_init(GreenAmptState& state, double S, double Ks, double IMD,
                  const SimulationOptions& opts) {
    double ucf_rain  = ucf::UCF(ucf::RAINFALL, opts);
    double ucf_depth = ucf::UCF(ucf::RAINDEPTH, opts);
    state.S      = S / ucf_depth;                       // in/mm → ft
    state.Ks     = Ks / ucf_rain;                       // in/hr or mm/hr → ft/sec
    state.IMDmax = IMD;
    state.IMD    = IMD;
    state.F      = 0.0;
    state.Lu     = 4.0 * std::sqrt(state.Ks * ucf_rain) / ucf_depth;
    state.Fumax  = state.IMDmax * state.Lu;
    state.Fu     = state.Fumax;
    state.saturated = false;
}

/// Newton solve for cumulative infiltration F2 given F1 and timestep.
static double grnampt_getF2(double F1, double Ks, double c1, double dt) {
    double f2 = F1 + Ks * dt;
    double c2 = c1 * std::log(F1 + c1) - Ks * dt;

    for (int i = 0; i < 20; ++i) {
        double df2 = (f2 - F1 - c1 * std::log(f2 + c1) + c2);
        double denom = 1.0 - c1 / (f2 + c1);
        if (std::fabs(denom) < TINY) break;
        df2 /= denom;
        if (std::fabs(df2) < 0.00001) break;
        f2 -= df2;
    }
    return f2;
}

double grnampt_getInfil(GreenAmptState& state, double precip, double depth, double dt) {
    double ia = precip + depth / dt;

    if (ia <= 0.0) {
        // Dry period — recovery
        if (state.Lu > 0.0) {
            double kr = state.Lu / 90000.0;  // recovery rate
            state.Fu -= kr * dt;
            if (state.Fu < 0.0) state.Fu = 0.0;
            state.IMD = state.IMDmax * (1.0 - state.Fu / state.Fumax);
        }
        state.saturated = false;
        return 0.0;
    }

    // Check if surface saturation occurs
    double c1 = (state.S + depth) * state.IMD;

    if (!state.saturated) {
        // Unsaturated: infiltrate at rainfall rate
        if (ia <= state.Ks) {
            state.F += ia * dt;
            state.Fu = std::min(state.Fu + ia * dt, state.Fumax);
            return ia;
        }
        // Check if saturation occurs this step
        double Fs = (c1 > 0.0 && ia > state.Ks)
                    ? state.Ks * c1 / (ia - state.Ks) : 1.0e10;
        if (state.F + ia * dt <= Fs) {
            state.F += ia * dt;
            state.Fu = std::min(state.Fu + ia * dt, state.Fumax);
            return ia;
        }
        state.saturated = true;
    }

    // Saturated: solve Green-Ampt equation
    double F2 = grnampt_getF2(state.F, state.Ks, c1, dt);
    double fp = (F2 - state.F) / dt;
    fp = std::min(fp, ia);

    state.F = F2;
    state.Fu = std::min(state.Fu + fp * dt, state.Fumax);
    return fp;
}

// ============================================================================
// SCS Curve Number
// ============================================================================

void curvenum_init(CurveNumState& state, double CN, double regen) {
    // SCS formula always produces inches regardless of unit system
    state.Smax = (1000.0 / CN - 10.0) / ucf::Ucf[ucf::RAINDEPTH][0];  // inches → feet
    state.S    = state.Smax;
    state.P    = 0.0;
    state.F    = 0.0;
    state.regen = regen;
    state.T    = 0.0;
    state.Tmax = (regen > 0.0) ? 0.06 / regen : 1.0e10;
}

double curvenum_getInfil(CurveNumState& state, double precip, double depth, double dt) {
    double ia = precip + depth / dt;

    if (ia <= 0.0) {
        // Dry period — recovery
        state.T += dt;
        if (state.T >= state.Tmax) {
            // New event — reset
            state.P = 0.0;
            state.F = 0.0;
            state.S = state.Smax;
        } else if (state.regen > 0.0) {
            state.S += state.regen * state.Smax * dt;
            if (state.S > state.Smax) state.S = state.Smax;
        }
        return 0.0;
    }

    state.T = 0.0;  // reset dry-period timer

    // Cumulative precipitation
    double P1 = state.P;
    state.P += ia * dt;
    double P2 = state.P;

    double Se = state.S;
    if (Se <= 0.0) return 0.0;

    // Cumulative infiltration at P1 and P2
    // F(P) = P * (1 - P/(P+Se))  or equivalently  F = P - P^2/(P+Se)
    double F1 = (P1 > 0.0) ? P1 - P1 * P1 / (P1 + Se) : 0.0;
    double F2 = P2 - P2 * P2 / (P2 + Se);

    double fp = (F2 - F1) / dt;
    fp = std::min(fp, ia);
    fp = std::max(fp, 0.0);

    state.F = F2;
    return fp;
}

} // namespace infil
} // namespace openswmm
