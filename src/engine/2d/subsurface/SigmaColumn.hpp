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
 * @file SigmaColumn.hpp
 * @brief G-steps 4 and 5 — closure B: the σ-coordinate explicit unsaturated
 *        column with a moving lower boundary (the water table).
 *
 * @section sigma_layout Coordinate and state
 *
 * The column occupies `z ∈ [0, L]` measured DOWNWARD from the ground
 * surface, `L = z_s − h_g`. Map to `σ = z/L ∈ [0,1]` with a **fixed** layer
 * count `m`; layer `j` owns `σ ∈ [j/m, (j+1)/m]`, `Δσ = 1/m`. Layers
 * stretch and compress as the table moves — no regridding, no variable layer
 * counts, fixed SoA stride.
 *
 * The conserved quantity is **water depth per layer**, `w_j = θ_j·L·Δσ`
 * (metres of water). Evolving `w` and deriving `θ = w/(L·Δσ)` from the NEW
 * `L` is what makes the compression exact: a column whose faces move but
 * whose fluxes vanish keeps every `w_j` unchanged, so total water is
 * invariant to machine precision. That is plan gate 5, and it holds by
 * construction here rather than by cancellation.
 *
 * @section sigma_fluxes Fluxes
 *
 * ```
 * w_j ← w_j + Δt·( F_{j−1/2} − F_{j+1/2} )          [downward-positive]
 *
 * F_{j+1/2} = K(θ_up)                                  advective, upwind
 *           − θ_donor·σ_{j+1/2}·L̇                      grid motion (ALE)
 *           − ( D̄_{j+1/2} / (L·Δσ) )·( θ_{j+1} − θ_j ) optional capillary
 * ```
 *
 * - Gravity is downward, so the advective donor is always the layer above.
 * - The grid term is upwinded on its own sign: with `L̇ > 0` (table falling,
 *   column growing) the faces sweep downward and the donor is the layer
 *   below; with `L̇ < 0` it is the layer above.
 * - `D̄` is the harmonic mean of the two layers' diffusivities. Off by
 *   default (D-N3).
 *
 * @section sigma_bc Boundaries and the handover
 *
 * - **Top (σ=0):** `F_top = q⁺ − q_ET`, with the top layer's acceptance
 *   capping `q⁺` — the excess is *rejected* and returned to the caller for
 *   the surface state (Hortonian / saturation rejection).
 * - **Bottom (σ=1):** `F_bot = q₀_phys − θ_bot·L̇`. The first term is the
 *   physical Darcy flux across the table; the second is the **handover** —
 *   the water swept across the moving boundary. Both together are what the
 *   saturated zone gains, which is why `SubsurfaceSolver` books
 *   `F_bot` and not `q₀_phys` as the recharge.
 *
 * @note **Specific yield.** Making the handover explicit forces the
 *       saturated update's storage coefficient to be `θ_s − θ_bot`, not
 *       `θ_s`. The plan's §2.1 writes `θ_s`; `θ_s − θ_bot` is what actually
 *       conserves once the column retains water above a falling table, and
 *       it is the textbook specific yield. This is the one place this
 *       implementation is deliberately more precise than the plan text, and
 *       it is called out again in `SubsurfaceSolver::fireCell`.
 *
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_SIGMA_COLUMN_HPP
#define OPENSWMM_ENGINE_2D_SIGMA_COLUMN_HPP

#include "SoilCharacteristic.hpp"

namespace openswmm::twoD::sigma {

/// What one column sweep needs and what it reports back.
struct ColumnStep {
    // ---- in ----
    double L_old   = 1.0;    ///< column thickness before the table moved (m)
    double L_new   = 1.0;    ///< …and after (m). `L̇ = (L_new − L_old)/dt`.
    double dt      = 1.0;    ///< the cell's tier step (s)
    double q_in    = 0.0;    ///< infiltration offered at the top (m/s, ≥ 0)
    double q_et    = 0.0;    ///< ET demand at the top (m/s, ≥ 0)
    double q0_phys = 0.0;    ///< physical Darcy flux across the table (m/s)
    bool   capillary = false;///< include the diffusive term

    // ---- out ----
    double f_top     = 0.0;  ///< accepted top flux (m/s) — `q_in` after rejection
    double rejected  = 0.0;  ///< `q_in − f_top` (m/s, ≥ 0) back to the surface
    double f_bot     = 0.0;  ///< total bottom flux incl. handover (m/s)
    double et_taken  = 0.0;  ///< ET actually removed (m/s, ≤ `q_et`)
    double theta_bot = 0.0;  ///< bottom-layer θ after the sweep (specific yield)
    double dt_limit  = 1.0e30; ///< the column's own explicit stability bound (s)
    /// Water (m/s) the compressed column could not hold, delivered DOWN to
    /// the saturated zone in this same firing. A rising table saturates the
    /// column from below; clamping this away is the ALE failure plan gate 5
    /// exists to catch, so it is routed and the solver must apply it.
    double overflow_to_sat  = 0.0;
    /// …and the mirror: water (m/s) drawn UP from the table to hold layers
    /// at residual when the column stretches faster than the fluxes fill it.
    double deficit_from_sat = 0.0;
};

/**
 * @brief Advance one σ column by `st.dt`.
 *
 * @param p      the cell's soil law and parameters
 * @param theta  `m` layer water contents, layer 0 at the surface. Read and
 *               written in place; stride is the caller's (the SoA is
 *               layer-major, so the caller passes a gathered/scattered
 *               buffer or a strided view — see `stride`).
 * @param m      layer count
 * @param stride distance between consecutive layers in @p theta (1 for a
 *               packed buffer, `n_cells` for the layer-major SoA)
 * @param st     step inputs and outputs
 *
 * Positivity is enforced per face by an availability share, the same idiom
 * as the surface solver's volume share: a face may not take more water than
 * its donor layer holds above `θ_r`, and the shares are scaled together when
 * a layer's outgoing faces over-subscribe it. First-order upwind in v1
 * (D-N4); MUSCL is a recorded follow-up.
 */
void advanceColumn(const soil::Params& p, double* theta, int m, int stride,
                   ColumnStep& st) noexcept;

/// The explicit stability bound of a column, without advancing it (§2.3):
/// `Δt_u ≤ C_col·min_j[ L·Δσ / ( c(θ_j) + |σ_j·L̇| + 2·D̄_j/(L·Δσ) ) ]`
/// with celerity `c(θ) = dK/dθ`.
double columnDtLimit(const soil::Params& p, const double* theta, int m,
                     int stride, double L, double Ldot, double c_col,
                     bool capillary) noexcept;

/// Seed a column to hydrostatic equilibrium above a table at depth `L`.
void seedHydrostatic(const soil::Params& p, double* theta, int m, int stride,
                     double L) noexcept;

/// Total water in the column (m of water) — `Σ θ_j·L·Δσ`.
double columnStorage(const double* theta, int m, int stride, double L) noexcept;

}  // namespace openswmm::twoD::sigma

#endif  // OPENSWMM_ENGINE_2D_SIGMA_COLUMN_HPP
