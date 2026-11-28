// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file      Infiltration.hpp
 * @brief     Infiltration models — Horton, Green-Ampt, SCS Curve Number.
 *
 * @details All three models are implemented with exact numerical parity
 *          to legacy infil.c. Each model maintains per-subcatchment state
 *          and computes infiltration rate given current precipitation,
 *          ponded depth, and timestep.
 *
 * @note Legacy reference: src/legacy/engine/infil.c
 * @ingroup new_engine
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_INFILTRATION_HPP
#define OPENSWMM_INFILTRATION_HPP

namespace openswmm {

struct SimulationOptions;

// ============================================================================
// Infiltration model type
// ============================================================================

enum class InfilModel : int {
    HORTON = 0,
    MOD_HORTON = 1, ///< Modified Horton: linear decay fp = f0 - kd * Fe
    GREEN_AMPT = 2,
    MOD_GREEN_AMPT = 3, ///< Modified Green-Ampt: does not reset F between events
    CURVE_NUM = 4,
    CONSTANT = 5 ///< Constant rate, capacity-bounded. 2D-only (no legacy
                 ///< [INFILTRATION] token); see plan §5.5.1.
};

// ============================================================================
// Horton infiltration state (per subcatchment)
// ============================================================================

struct HortonState {
    double f0 = 0.0;    ///< Initial infiltration rate (ft/sec)
    double fmin = 0.0;  ///< Minimum (ultimate) rate (ft/sec)
    double decay = 0.0; ///< Decay constant k (1/sec)
    double regen = 0.0; ///< Regeneration constant kr (1/sec)
    double Fmax = 0.0;  ///< Max cumulative infiltration (ft)
    double tp = 0.0;    ///< Cumulative infiltration time (sec)
    double Fe = 0.0;    ///< Cumulative excess infiltration (ft)
    double Fmh = 0.0;   ///< Cumulative infiltration for Modified Horton (ft)
};

// ============================================================================
// Green-Ampt state (per subcatchment)
// ============================================================================

struct GreenAmptState {
    double S = 0.0;         ///< Capillary suction head (ft)
    double Ks = 0.0;        ///< Saturated hydraulic conductivity (ft/sec)
    double IMDmax = 0.0;    ///< Maximum initial moisture deficit (0-1)
    double IMD = 0.0;       ///< Current moisture deficit (0-1)
    double F = 0.0;         ///< Cumulative infiltration (ft)
    double Fu = 0.0;        ///< Upper zone saturation volume (ft)
    double Fumax = 0.0;     ///< Max upper zone saturation (ft)
    double Lu = 0.0;        ///< Upper zone depth (ft)
    double T = 0.0;         ///< Inter-event timer (sec, counts down)
    bool saturated = false; ///< True when surface is saturated
};

// ============================================================================
// SCS Curve Number state (per subcatchment)
// ============================================================================

struct CurveNumState {
    double Smax = 0.0;  ///< Max retention S = (1000/CN - 10)/12 (ft)
    double S = 0.0;     ///< Current retention (ft)
    double Se = 0.0;    ///< Effective retention at event start (ft)
    double P = 0.0;     ///< Cumulative precipitation (ft)
    double F = 0.0;     ///< Cumulative infiltration (ft)
    double f = 0.0;     ///< Previous infiltration rate (ft/sec)
    double regen = 0.0; ///< Regeneration rate (1/sec)
    double T = 0.0;     ///< Time since last rainfall (sec)
    double Tmax = 0.0;  ///< Inter-event time (sec)
};

// ============================================================================
// Infiltration functions
// ============================================================================

/** @brief Native infiltration kernels with caller-managed accepted-step state.
 * These methods mutate the supplied state immediately and provide no rollback,
 * finite-input validation or failure status. Call once per accepted source step,
 * or evaluate a private state copy for a trial. Internal rates/depths are ft/s/ft;
 * initialization alone accepts project input units. Serialize shared state use. */
namespace infil {

// The state structs above are declared at `openswmm` scope. Re-export them
// here so callers may also spell them `infil::HortonState` (the 2D adapter
// contract, src/engine/2d/infil/Infil2D.hpp, uses that qualification).
using ::openswmm::HortonState;
using ::openswmm::GreenAmptState;
using ::openswmm::CurveNumState;

/** @brief Integrate exponential Horton capacity over one step with dry recovery.
 * @param[in,out] state Initialized parameters/history; normally finite f0>=fmin>=0,
 * nonnegative decay/regen/Fmax and meaningful cumulative tp/Fe. Caller applies any
 * conductivity/recovery multipliers before calling. Updates tp and, when capped,
 * Fe; Fmh is not used. No rollback if an intermediate becomes nonfinite.
 * @param precip Rainfall plus included runon rate ft/s, normally finite nonnegative.
 * @param depth Ponded feet available over this interval, normally finite nonnegative.
 * @param dt Finite positive seconds; no zero/negative-step guard.
 * @return Actual ft/s bounded by available precip+depth/dt for valid state. Negative
 * capacity difference/decay/regen returns zero unchanged. Equal f0/fmin or zero decay
 * uses constant capacity without advancing history or enforcing Fmax. Wet regular
 * steps solve equivalent cumulative time (up to twenty Newton iterations); dry
 * steps recover time and capped infiltration. Numerical failure has no diagnostic. */
double horton_getInfil(HortonState& state, double precip, double depth, double dt);

/** @brief Advance the linear excess-infiltration Modified Horton model.
 * @param[in,out] state Compatible initialized Horton parameters with finite
 * f0>=fmin>=0 and nonnegative decay/regen. Fe drives capacity f0-decay*Fe; Fmh tracks
 * total infiltration against positive Fmax. Caller resets Fmh when starting a new
 * simulation because horton_init does not do so. Updates Fe/Fmh, not tp.
 * @param precip Finite nonnegative incoming rate ft/s including caller-supplied runon.
 * @param depth Finite nonnegative ponded depth feet.
 * @param dt Finite positive step seconds; unchecked.
 * @return Capacity/supply-limited ft/s. Invalid negative parameter relationships
 * return zero unchanged; constant-capacity branches bypass cumulative state/cap.
 * Dry periods exponentially reduce Fe and Fmh. No finite validation/error status
 * or trial rollback is supplied. */
double modHorton_getInfil(HortonState& state, double precip, double depth, double dt);

/** @brief Advance saturated/unsaturated Green-Ampt history and compute actual infiltration.
 * @param[in,out] state Initialized unscaled Ks/Lu/suction/moisture state. Updates T,
 * F, Fu, IMD and saturated status; base Ks/Lu are not scaled in place. Caller
 * provides physically valid finite state and owns trial/accepted-step copying.
 * @param precip Finite nonnegative rainfall plus included runon ft/s.
 * @param depth Finite nonnegative ponded depth feet.
 * @param dt Finite positive seconds; decrements T before choosing the branch.
 * @param model_type GREEN_AMPT resets event F in the low-rain timer branch;
 * MOD_GREEN_AMPT skips that particular reset. Dry-zone emptying/reset logic is
 * shared; other enum values also follow the non-GREEN_AMPT branch.
 * @param infil_factor Conductivity multiplier: ks=Ks*factor and lu=Lu*sqrt(factor).
 * Positive finite factor is needed for nonzero finite lu in division branches;
 * zero/negative/nonfinite factors receive no protective guard.
 * @param recovery_factor Dimensionless dry-recovery multiplier, normally positive
 * finite; zero can produce an infinite inter-event timer and is not special-cased.
 * @return Infiltration ft/s from the state-dependent supply/capacity solve. Factors
 * are applied locally, upper-zone drainage/event resets follow the legacy algorithm,
 * and no finite-output check, convergence status or rollback is supplied. */
double grnampt_getInfil(GreenAmptState& state, double precip, double depth, double dt,
                        InfilModel model_type = InfilModel::GREEN_AMPT, double infil_factor = 1.0,
                        double recovery_factor = 1.0);

/** @brief Advance SCS Curve Number event retention with ponded-water continuation.
 * @param[in,out] state Initialized finite CN retention/history; updates event P/F,
 * remaining S/Se, inter-event T and previous rate f immediately.
 * @param precip Finite nonnegative rainfall ft/s; caller determines runon inclusion.
 * @param depth Finite nonnegative ponded feet. During no rain, depth>0.004167 ft
 * can continue the preceding rate while retention remains.
 * @param dt Finite positive seconds, unchecked.
 * @param recovery_factor Finite nonnegative multiplier on dry capacity regeneration;
 * zero suppresses that recovery. Applied after regen*Smax*dt in legacy arithmetic order.
 * @return Nonnegative supply-limited ft/s for valid inputs. New rainfall after Tmax
 * resets event totals using current retention; positive regen depletes S with
 * infiltration, otherwise it remains unchanged. No numeric validation/status/rollback. */
double curvenum_getInfil(CurveNumState& state, double precip, double depth, double dt,
                         double recovery_factor = 1.0);

/** @brief Return a stateless capacity bounded by available water.
 * @param rate_ftsec Finite authored capacity ft/s, normally nonnegative.
 * @param precip Finite incoming rate ft/s, normally nonnegative.
 * @param depth Finite nonnegative available ponded feet.
 * @param dt Finite positive seconds; unchecked division occurs even if capacity is zero.
 * @return max(0,min(rate_ftsec,precip+depth/dt)) ft/s. No state is read/modified and
 * no finite-value/error handling is added. This model has no legacy input token. */
double constant_getInfil(double rate_ftsec, double precip, double depth, double dt);

/** @brief Convert authored Horton parameters and reset ordinary Horton history.
 * @param[out] state Receives parameters plus tp=Fe=0. Existing Fmh is retained;
 * use a fresh/default state or explicitly reset it before Modified Horton reuse.
 * @param f0 Initial capacity in project rainfall units (in/hr or mm/hr).
 * @param fmin Ultimate capacity in the same units, normally 0<=fmin<=f0.
 * @param decay Finite nonnegative exponential/linear decay constant per hour,
 * converted to per second.
 * @param regen Drying time in days, not a regeneration rate; positive becomes
 * -log(0.02)/(regen*86400), nonpositive disables recovery.
 * @param Fmax Cumulative infiltration depth in inches/mm; positive converts to feet,
 * nonpositive stores zero (unlimited). No validity/finite checks are performed.
 * @param opts Compatible valid flow-unit selection for rainfall/rain-depth conversions.
 * Does not apply monthly/pattern conductivity/recovery factors. */
void horton_init(HortonState& state, double f0, double fmin, double decay, double regen,
                 double Fmax, const SimulationOptions& opts);

/** @brief Convert authored Green-Ampt parameters and initialize the upper zone.
 * @param[out] state Receives S/Ks/IMD/IMDmax, F=Fu=0, derived Lu/Fumax and
 * saturated=false. Existing inter-event T is retained; caller initializes it for
 * new simulation state. Invalid values are not rejected or clamped.
 * @param S Suction depth in project inches/mm, normally nonnegative.
 * @param Ks Positive saturated conductivity in project in/hr or mm/hr, converted
 * first to ft/s; Lu is derived from conductivity expressed in in/hr in all systems.
 * @param IMD Initial moisture-deficit fraction, physically in [0,1].
 * @param opts Valid project unit options used by rainfall/depth conversions.
 * No forcing factors or accepted water-store changes occur here. */
void grnampt_init(GreenAmptState& state, double S, double Ks, double IMD,
                  const SimulationOptions& opts);

/** @brief Initialize Curve Number retention and all event-history fields.
 * @param[out] state Replaced with initialized Smax/S/Se/P/F/f/regen/T/Tmax values.
 * @param CN Finite authored curve number, clamped to [10,99] before the retention
 * formula (1000/CN-10)/12 feet. Does not reject nonfinite input explicitly.
 * @param regen Drying time days (despite the parameter name); positive becomes
 * 1/(days*86400), otherwise zero. Tmax=0.06/regen when positive, else 1e10 seconds.
 * No project-unit conversion is needed for this empirical inch-based formula. */
void curvenum_init(CurveNumState& state, double CN, double regen);

} // namespace infil
} // namespace openswmm

#endif // OPENSWMM_INFILTRATION_HPP
