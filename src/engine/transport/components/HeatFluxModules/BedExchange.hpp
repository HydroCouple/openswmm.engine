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
 * @file BedExchange.hpp
 * @brief Plan H6b — bed conduction, deep-ground conduction, and hyporheic
 *        exchange, integrated SIMULTANEOUSLY with the surface fluxes.
 *
 * @par Why this is not "one more term in netFluxOut"
 *      `HeatFluxes.hpp` predicted that H6b would be exactly that. It cannot
 *      be, for two independent reasons, and both were found by trying:
 *
 *      1. **`netFluxOut` returns W/m² at the FREE SURFACE.** Bed exchange
 *         acts on the wetted perimeter, which is a different area — and for
 *         a closed conduit the free-surface area is zero while the bed area
 *         is not. Summing them would multiply the bed flux by the wrong
 *         area, and would switch it off entirely in a full pipe.
 *      2. **The bed is a SECOND STATE VARIABLE.** `relaxT` relaxes one body
 *         toward a fixed equilibrium. Two bodies exchanging with each other
 *         do not have a fixed equilibrium to relax toward: each one's target
 *         moves as the other responds. Treating the bed as a constant-
 *         temperature reservoir inside `netFluxOut` would make a thin bed
 *         behave like an infinite heat sink, which is the opposite of what a
 *         thin bed does.
 *
 *      So H6b adds a coupled stepper rather than a term. **D-H5e's rule
 *      still holds and is why the stepper is coupled rather than sequential**:
 *      relaxations do not commute, so the surface families and the bed
 *      exchange are solved in ONE step, not one after the other. Doing it
 *      sequentially would reintroduce precisely the defect D-H5e removed,
 *      one phase after it was removed.
 *
 * @par The system
 *      With `J(T_w) ~ J0 + J'(T_w - T_w0)` linearized exactly as `relaxT`
 *      already linearizes it:
 *      @code
 *      C_w dT_w/dt = -A_s J(T_w) + G_wb (T_b - T_w)
 *      C_b dT_b/dt =  G_wb (T_w - T_b) + G_bg (T_gr - T_b)
 *      @endcode
 *      a 2x2 linear system `du/dt = M u + c` solved by its exact matrix
 *      exponential, `u(dt) = dt * phi1(M dt) * c`, `phi1(z) = expm1(z)/z`.
 *
 *      Both eigenvalues of `M` are real and non-positive — the discriminant
 *      is `(a11-a22)^2 + 4 a12 a21` with `a12, a21 >= 0`, so it can never be
 *      negative and the pair can never oscillate. **The step therefore cannot
 *      overshoot at any dt**, which is the property D-H5d bought for the
 *      single body and which would have been lost by any explicit coupling.
 *
 * @par Conservation, and the gate that checks it
 *      Adding the two equations, `G_wb` cancels identically:
 *      `C_w dT_w/dt + C_b dT_b/dt = -A_s J + G_bg (T_gr - T_b)`.
 *      Whatever the water loses to the bed, the bed gains. The exchange is
 *      therefore conservative BY CONSTRUCTION, not by a bookkeeping step
 *      that could be forgotten, and `BedExchangeConservesEnergy` asserts the
 *      identity rather than a tolerance on a ledger.
 *
 * @par Solutes
 *      The solute pair is the same system minus the surface flux and minus
 *      the ground term — `HTSComponent` leaves its ground solute exchange
 *      commented out (`element.cpp:161`) and parity keeps it out. Two bodies
 *      exchanging with nothing else has a closed form that needs no matrix:
 *      the total mass is invariant and the DIFFERENCE decays exponentially.
 *      `exchangePair` uses that form, so conservation is STRUCTURAL — there
 *      is no separate bookkeeping step that could be omitted.
 *
 *      It is **not bit-exact**, and the first draft of this file claimed it
 *      was. `vol_w * dc_w` evaluates `vol_w * (vol_b * share)` while
 *      `vol_b * dc_b` evaluates `vol_b * (vol_w * share)` — the same factors
 *      in a different association, which round differently once `share`
 *      saturates at large `dt`. The residual is relative round-off, ~1e-16,
 *      and the gate asserts that bound rather than equality. Measured, not
 *      assumed: a driver over `dt` in {1, 600, 7200, 1e6} agreed bit for bit
 *      on the first three and differed on the fourth.
 *
 * @see plans/transport/HEAT_TRANSPORT_PLAN.md §2.3, §6.5 D-H6b
 * @see HTSComponent/src/element.cpp:121-200 (the reference)
 * @ingroup engine_transport
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_TRANSPORT_BED_EXCHANGE_HPP
#define OPENSWMM_ENGINE_TRANSPORT_BED_EXCHANGE_HPP

namespace openswmm {
struct SimulationContext;
}

namespace openswmm::transport::heat {

/**
 * @brief One element's bed coupling, fully reduced to SI conductances.
 *
 * @details Everything geometric and material has already been folded in, so
 *          `relaxPair` contains no unit conversion and no reference to
 *          conduit shape. That separation is deliberate: the geometry is
 *          what differs between the LEGACY link binding and the ARD mesh
 *          binding, and the integration is what must not.
 */
struct BedCoupling {
    double g_wb = 0.0;  ///< Water<->bed conductance, W/K (conduction + advection)
    double g_bg = 0.0;  ///< Bed<->deep-ground conductance, W/K
    double c_w  = 0.0;  ///< Water heat capacity, J/K
    double c_b  = 0.0;  ///< Bed heat capacity, J/K
    double t_gr = 0.0;  ///< Deep-ground temperature, °C

    /// True when the pair is worth stepping at all.
    bool viable() const noexcept { return c_w > 0.0 && c_b > 0.0; }
};

/// Simultaneous temperature increments for the coupled pair, °C.
struct PairStep {
    double dt_w = 0.0;  ///< Channel water
    double dt_b = 0.0;  ///< Bed
};

/**
 * @brief One exact step of the coupled (water, bed) pair.
 *
 * @param g       Conductances and capacities for this element.
 * @param t_w     Current channel water temperature, °C.
 * @param t_b     Current bed temperature, °C.
 * @param j0      Net outward SURFACE flux at `t_w`, W/m² (`netFluxOut`).
 * @param j1      The same flux at `t_w + h` — the probe `relaxT` already
 *                takes, passed in rather than recomputed so the pair is
 *                stepped against exactly the function the single body was.
 * @param h       Probe offset, °C. `<= 0` disables the linearization and
 *                takes `j0` as a constant forcing.
 * @param area_m2 Free-surface area the surface flux acts on, m². May be 0
 *                for a closed conduit — the bed terms still apply.
 * @param dt      Step, seconds.
 *
 * @return Increments for both bodies. `{0,0}` when `g` is not viable.
 *
 * @note Reduces to `relaxT` when `g_wb == g_bg == 0`, to within 1-2 ULP —
 *       the two take different but algebraically identical routes to the
 *       same scalar, so the last bit can differ. Measured over `dt` in
 *       {1, 60, 900, 86400}: 1 ULP at every step, well inside the 4 ULP
 *       `EXPECT_DOUBLE_EQ` allows. **The reduction is what keeps pre-H6b
 *       answers unchanged, and it holds only because the module is OFF by
 *       default** — a deck without `SEDIMENT_EXCHANGE` never enters
 *       `relaxPair` at all, so byte-identity is exact for the reason that
 *       matters, not for this one.
 */
PairStep relaxPair(const BedCoupling& g, double t_w, double t_b,
                   double j0, double j1, double h, double area_m2,
                   double dt) noexcept;

/// Simultaneous concentration increments for the coupled pair.
struct SolutePairStep {
    double dc_w = 0.0;  ///< Channel water
    double dc_b = 0.0;  ///< Bed water
};

/**
 * @brief One exact step of the two-body solute exchange.
 *
 * @param c_w     Channel concentration.
 * @param c_b     Bed concentration.
 * @param vol_w   Channel water volume, m³.
 * @param vol_b   Bed water volume, m³ (bed volume x porosity is the honest
 *                reading; the reference uses the bulk volume and so does
 *                this, see the header note on porosity).
 * @param q_exch  Total exchange discharge, m³/s: `D_sed*A/Y + v_hyp*A`.
 * @param dt      Step, seconds.
 *
 * @return Increments satisfying `vol_w*dc_w + vol_b*dc_b == 0` to relative
 *         round-off. See the header on why this is not bit-exact.
 */
SolutePairStep exchangePair(double c_w, double c_b, double vol_w,
                            double vol_b, double q_exch, double dt) noexcept;

/**
 * @brief Bed conductances for link `j` from the deck configuration and the
 *        link's current hydraulics.
 *
 * @details Contact area is the WETTED PERIMETER times length times barrels —
 *          see `BedZoneData.hpp` divergence 1. Returns a non-viable coupling
 *          (which `relaxPair` treats as a no-op) for a dry link, a
 *          non-conduit, or a link whose perimeter cannot be formed.
 */
///          `t_gr` is passed in rather than resolved here because it is
///          GLOBAL scope: one timeseries lookup per STEP, not one per link.
///          That is `updateSolarForcing`'s pattern and the reason it exists.
BedCoupling bedCouplingForLink(const SimulationContext& ctx, int link,
                               double vol_ft3, double t_gr) noexcept;

/// The deep-ground temperature in force this step, °C — resolves the
/// `GROUND_TEMPERATURE TIMESERIES` spelling against the simulation clock.
/// Non-const because the timeseries lookup advances the table's own cursor,
/// exactly as `table_tseries_lookup_cursor` does everywhere else.
double groundTemperature(SimulationContext& ctx) noexcept;

/// True when `[HEAT_FLUXES] SEDIMENT_EXCHANGE` is on and heat transport is.
bool bedExchangeEnabled(const SimulationContext& ctx) noexcept;

/**
 * @brief Size and seed the bed temperature array, idempotently.
 *
 * @details There is deliberately **no `applyBedHeatExchange`**. A function by
 *          that name would be called beside `applyHeatFluxes` and would step
 *          the bed in its own relaxation — which is D-H5e's defect, one phase
 *          after D-H5e removed it. The heat coupling instead happens INSIDE
 *          `applyHeatFluxes`'s link loop, which calls `relaxPair` with the
 *          real `j0`/`j1`/area so the surface families and the bed are one
 *          simultaneous step. This function is only the state setup that
 *          loop needs first.
 */
void seedBedTemperature(SimulationContext& ctx);

/**
 * @brief Step every conduit's bed/channel solute exchange for one `dt`.
 *
 * @param link_conc  [link * n_species + s] channel concentrations, updated
 *                   in place. The caller owns the layout because LEGACY and
 *                   ARD hold their channel state differently; what they
 *                   share is this function and the bed array it moves mass
 *                   into.
 */
void applyBedSoluteExchange(SimulationContext& ctx, double* link_conc,
                            int n_species, double dt);

}  // namespace openswmm::transport::heat

#endif  // OPENSWMM_ENGINE_TRANSPORT_BED_EXCHANGE_HPP
