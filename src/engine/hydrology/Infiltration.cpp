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
            state.tp = std::max(state.tp, 0.0);
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
        fp = std::min(fp, ia);
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
        fp = std::min(fp, ia);

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
// Modified Horton — linear decay: fp = f0 - kd * Fe
// Matching legacy infil.c modHorton_getInfil() exactly
// ============================================================================

double modHorton_getInfil(HortonState& state, double precip, double depth, double dt) {
    double f  = 0.0;
    double f0   = state.f0;
    double fmin = state.fmin;
    double df   = f0 - fmin;
    double kd   = state.decay;
    double kr   = state.regen;

    // Special cases: no infiltration or constant infiltration
    if (df < 0.0 || kd < 0.0 || kr < 0.0) return 0.0;
    if (df == 0.0 || kd == 0.0) {
        double fp = f0;
        double fa = precip + depth / dt;
        fp = std::min(fp, fa);
        return std::max(0.0, fp);
    }

    // Available water rate (ft/sec)
    double fa = precip + depth / dt;

    if (fa > TINY) {
        // Saturated condition check
        if (state.Fmax > 0.0 && state.Fe >= state.Fmax) return 0.0;

        // Modified Horton potential infiltration: linear decay
        double fp = f0 - kd * state.Fe;
        fp = std::max(fp, fmin);

        // Actual infiltration limited by available water
        f = std::min(fa, fp);

        // Limit cumulative infiltration to Fmax
        if (state.Fmax > 0.0) {
            if (state.Fmh + f * dt > state.Fmax)
                f = (state.Fmax - state.Fmh) / dt;
            f = std::max(f, 0.0);
            state.Fmh += f * dt;
        }

        // Update cumulative excess infiltration (above fmin)
        state.Fe += std::max(f - fmin, 0.0) * dt;
        if (state.Fmax > 0.0)
            state.Fe = std::min(state.Fe, state.Fmax);
    }
    // Dry period — exponential recovery
    else if (kr > 0.0) {
        double decay_factor = std::exp(-kr * dt);
        state.Fe  *= decay_factor;
        state.Fe   = std::max(state.Fe, 0.0);
        state.Fmh *= decay_factor;
        state.Fmh  = std::max(state.Fmh, 0.0);
    }

    return f;
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
    state.Fu     = 0.0;  // Legacy: starts dry (empty upper zone = max storage)
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

double grnampt_getInfil(GreenAmptState& state, double precip, double depth, double dt,
                        InfilModel model_type) {
    // Matching legacy infil.c grnampt_getInfil + grnampt_getUnsatInfil + grnampt_getSatInfil
    double ia = precip + depth / dt;
    double lu = state.Lu;
    double Fumax = state.Fumax;  // InfilFactor applied at call-site to Ks/Lu

    // Decrement inter-event timer (matching legacy line 672)
    state.T -= dt;

    if (ia <= 0.0) {
        // Dry period — recovery (matching legacy grnampt_getUnsatInfil lines 705-727)
        if (lu > 0.0 && Fumax > 0.0) {
            double kr = lu / 90000.0;  // recoveryFactor applied at call-site to Lu
            double dF = kr * Fumax * dt;
            state.F  -= dF;
            state.Fu -= dF;
            state.F = std::max(state.F, 0.0);
            state.Fu = std::max(state.Fu, 0.0);

            // Inter-event reset: when timer expires, reset IMD and F
            // Standard GA resets F; Modified GA does NOT (legacy line 736)
            if (model_type == InfilModel::GREEN_AMPT && state.T <= 0.0) {
                state.IMD = (Fumax > 0.0 && lu > 0.0)
                    ? (Fumax - state.Fu) / lu : state.IMDmax;
                state.F = 0.0;
            }
        }
        state.saturated = false;
        return 0.0;
    }

    // --- Wet period ---
    double c1 = (state.S + depth) * state.IMD;

    if (!state.saturated) {
        // Unsaturated path (matching legacy grnampt_getUnsatInfil)
        if (ia <= state.Ks) {
            // Light rain: infiltrate all
            state.F  += ia * dt;
            state.Fu  = std::min(state.Fu + ia * dt, Fumax);

            // Inter-event reset for standard Green-Ampt when timer expires
            // (matching legacy line 736: only GREEN_AMPT, not MOD_GREEN_AMPT)
            if (state.T <= 0.0) {
                state.IMD = (Fumax > 0.0 && lu > 0.0)
                    ? (Fumax - state.Fu) / lu : state.IMDmax;
                state.F = 0.0;
            }
            return ia;
        }

        // Heavy rain: renew inter-event timer
        // (matching legacy line 745: T = 5400 / lu / recoveryFactor)
        if (lu > 0.0) state.T = 5400.0 / lu;

        // Check if saturation occurs this step
        double Fs = (c1 > 0.0 && ia > state.Ks)
                    ? state.Ks * c1 / (ia - state.Ks) : 1.0e10;
        if (state.F + ia * dt <= Fs) {
            state.F  += ia * dt;
            state.Fu  = std::min(state.Fu + ia * dt, Fumax);
            return ia;
        }
        state.saturated = true;
    }

    // Saturated path: renew timer
    if (lu > 0.0) state.T = 5400.0 / lu;

    // Solve Green-Ampt equation
    double F2 = grnampt_getF2(state.F, state.Ks, c1, dt);
    double fp = (F2 - state.F) / dt;
    fp = std::min(fp, ia);
    fp = std::max(fp, 0.0);

    state.F  = F2;
    state.Fu = std::min(state.Fu + fp * dt, Fumax);
    return fp;
}

// ============================================================================
// SCS Curve Number
// ============================================================================

void curvenum_init(CurveNumState& state, double CN, double regen_days) {
    // SCS formula: S in inches, then convert to feet (always /12)
    // Legacy: infil->Smax = (1000.0/CN - 10.0) / 12.0
    CN = std::max(CN, 10.0);
    CN = std::min(CN, 99.0);
    state.Smax = (1000.0 / CN - 10.0) / 12.0;  // inches → feet (hardcoded /12)
    state.S    = state.Smax;
    state.Se   = state.Smax;
    state.P    = 0.0;
    state.F    = 0.0;
    state.f    = 0.0;
    // Legacy: regen = 1.0 / (dryingTime_days * SECperDAY)
    constexpr double SEC_PER_DAY = 86400.0;
    state.regen = (regen_days > 0.0) ? 1.0 / (regen_days * SEC_PER_DAY) : 0.0;
    state.T    = 0.0;
    state.Tmax = (state.regen > 0.0) ? 0.06 / state.regen : 1.0e10;
}

double curvenum_getInfil(CurveNumState& state, double precip, double depth, double dt) {
    // Matches legacy infil.c::curvenum_getInfil() exactly
    constexpr double MIN_TOTAL_DEPTH = 0.0001; // ft
    double fa = precip + depth / dt;  // max available rate
    double f1 = 0.0;

    // --- Case where there is rainfall ---
    if (precip > TINY) {
        // Check if new rain event
        if (state.T >= state.Tmax) {
            state.P  = 0.0;
            state.F  = 0.0;
            state.f  = 0.0;
            state.Se = state.S;  // Use CURRENT S (not Smax) as event start retention
        }
        state.T = 0.0;

        // Update cumulative precip
        state.P += precip * dt;

        // Find potential new cumulative infiltration
        double F1 = state.P * (1.0 - state.P / (state.P + state.Se));

        // Compute potential infiltration rate
        f1 = (F1 - state.F) / dt;
        if (f1 < 0.0 || state.S <= 0.0) f1 = 0.0;
    }
    // --- Case of no rainfall ---
    else {
        // If there is ponded water, use previous infil rate
        if (depth > MIN_TOTAL_DEPTH && state.S > 0.0) {
            f1 = state.f;
            if (f1 * dt > state.S) f1 = state.S / dt;
        }
        // Otherwise update inter-event time
        else {
            state.T += dt;
        }
    }

    // --- If there is some infiltration ---
    if (f1 > 0.0) {
        // Limit to max available rate
        f1 = std::min(f1, fa);
        f1 = std::max(f1, 0.0);

        // Update actual cumulative infiltration
        state.F += f1 * dt;

        // Deplete retention capacity S (legacy line 1014)
        if (state.regen > 0.0) {
            state.S -= f1 * dt;
            state.S = std::max(state.S, 0.0);
        }
    }
    // --- Otherwise regenerate capacity ---
    else {
        // Legacy line 1022: S += regen * Smax * tstep * recoveryFactor
        // recoveryFactor is applied at call-site
        state.S += state.regen * state.Smax * dt;
        state.S = std::min(state.S, state.Smax);
    }

    state.f = f1;
    return f1;
}

} // namespace infil
} // namespace openswmm
