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
 * @file      SigmaColumn.hpp
 * @brief     G-steps 4 and 5 — closure B: the σ-coordinate explicit unsaturated
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
 *   saturated zone gains. `SubsurfaceSolver` settles the table using the
 *   actual bounded `F_bot`, compression surplus and residual support from
 *   the same sweep. Its physical recharge diagnostic remains `q₀_phys`;
 *   boundary handover is internal and is excluded from external continuity.
 *
 * @note **Specific yield.** `θ_s − θ_bot` gives the table predictor. The
 *       final SIGMA bracket pairs saturated storage `θ_s·h_g` with the
 *       actual column transfer rather than correcting the table after a
 *       column has collapsed to a zero-yield saturated profile.
 *
 * @ingroup engine_2d
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_SIGMA_COLUMN_HPP
#define OPENSWMM_ENGINE_2D_SIGMA_COLUMN_HPP

#include "SoilCharacteristic.hpp"
#include <vector>

namespace openswmm::twoD::sigma {

/// Accepted water depths, positive downward at each of m+1 faces. The
/// nonlinear table solver requests these only for its final accepted sweep.
/// Separate phase records retain redistribution through compression/support;
/// transport never reconstructs gravity or replays rejected table trials.
struct ColumnTransfers {
    double old_length = 0, new_length = 0, infiltration = 0, evaporation = 0;
    std::vector<double> old_water, new_water, faces, compression, support, grid, contacts;
};

/// What one column sweep needs and what it reports back.
struct ColumnStep {
    // ---- in ----
    double L_old = 1.0;             ///< column thickness before the table moved (m)
    double L_new = 1.0;             ///< …and after (m). Default `L̇ = (L_new − L_old)/dt`.
    double dt = 1.0;                ///< the cell's tier step (s)
    bool forward_euler = false;     ///< hold the old-state ALE velocity in an RK stage
    double grid_rate = 0.0;         ///< supplied Ldot for forward_euler; geometry still uses L_new
    double q_in = 0.0;              ///< infiltration offered at the top (m/s, ≥ 0)
    double wilting_suction = 150.0; ///< SI suction magnitude; one top-boundary stress evaluation
    double et_stress = 0.0;
    double q_et = 0.0; ///< ET demand at the top (m/s, ≥ 0)
    double handover_theta =
        -1.0;               ///< <0: actual bottom layer; otherwise paired table storage coefficient
    double q0_phys = 0.0;   ///< physical Darcy flux across the table (m/s)
    bool capillary = false; ///< include the diffusive term

    const std::vector<double>* layer_inflow =
        nullptr;                          ///< accepted internal contact water depths (m)
    ColumnTransfers* transfers = nullptr; ///< optional accepted-sweep receipt

    // ---- out ----
    double f_top = 0.0;       ///< accepted top flux (m/s) — `q_in` after rejection
    double rejected = 0.0;    ///< `q_in − f_top` (m/s, ≥ 0) back to the surface
    double f_bot = 0.0;       ///< total bottom flux incl. handover (m/s)
    double et_taken = 0.0;    ///< ET actually removed (m/s, ≤ `q_et`)
    double theta_bot = 0.0;   ///< bottom-layer θ after the sweep (specific yield)
    double dt_limit = 1.0e30; ///< the column's own explicit stability bound (s)
    /// Water (m/s) the compressed column could not hold, delivered DOWN to
    /// the saturated zone in this same firing. A rising table saturates the
    /// column from below; clamping this away is the ALE failure plan gate 5
    /// exists to catch, so it is routed and the solver must apply it.
    double overflow_to_sat = 0.0;
    /// …and the mirror: water (m/s) drawn UP from the table to hold layers
    /// at residual when the column stretches faster than the fluxes fill it.
    double deficit_from_sat = 0.0;
};

/**
 * @brief Advance one σ column by `st.dt`.
 *
 * @param[in] p      the cell's soil law and parameters
 * @param[in,out] theta  `m` layer water contents, layer 0 at the surface. Read and written in
 * place; stride is the caller's (the SoA is layer-major, so the caller passes a
 * gathered/scattered buffer or a strided view — see `stride`).
 * @param[in] m      layer count
 * @param[in] stride distance between consecutive layers in @p theta (1 for a packed buffer,
 * `n_cells` for the layer-major SoA)
 * @param[in,out] st     step inputs and outputs
 *
 * Positivity is enforced per face by an availability share, the same idiom as the surface
 * solver's volume share: a face may not take more water than its donor layer holds above `θ_r`,
 * and the shares are scaled together when a layer's outgoing faces over-subscribe it.
 * First-order upwind in v1 (D-N4); MUSCL is a recorded follow-up.
 *
 * @pre For an advancing step, theta points to at least 1+(m-1)*stride doubles, stride > 0, and p
 * contains validated SI soil parameters. Optional layer_inflow has at least m water-depth
 * entries and does not alias outputs.
 * @post Updates strided volumetric theta and st's accepted/rejected fluxes in m/s, stability
 * bound in s, and optional transfer water depths in m. Actual table/surface storage updates
 * remain the caller's responsibility.
 * @note m <= 0 or nonpositive/NaN dt returns immediately without resetting outputs. Uses
 * per-thread reusable vectors; distinct input/output buffers permit separate-thread calls.
 * Vector/receipt allocation occurs inside noexcept, so allocation failure terminates rather than
 * returning a status. No caller pointers are retained.
 */
void advanceColumn(const soil::Params& p, double* theta, int m, int stride,
                   ColumnStep& st) noexcept;

/**
 * @brief The explicit stability bound of a column, without advancing it (§2.3): `Δt_u ≤
 * C_col·min_j[ L·Δσ / ( c(θ_j) + |σ_j·L̇| + 2·D̄_j/(L·Δσ) ) ]` with celerity `c(θ) = dK/dθ`.
 *
 * @param[in] p Borrowed validated SI soil-law parameters.
 * @param[in] theta Required strided layer water-content array, dimensionless; at least
 * 1+(m-1)*stride readable doubles.
 * @param[in] m Strictly positive layer count.
 * @param[in] stride Strictly positive distance in doubles between layers; 1 packed or n_cells
 * for a layer-major SoA.
 * @param[in] L Finite column thickness in m.
 * @param[in] Ldot Finite column-thickness rate in m/s, positive for a falling table.
 * @param[in] c_col Finite Courant factor, numerically floored at 1e-6.
 * @param[in] capillary Whether to include the moisture diffusivity contribution.
 * @return Explicit stability interval in s, with L floored at 1e-3 m; a near-zero total celerity
 * leaves the large 1e30 s sentinel scaled by c_col.
 * @note No bounds/physical validation, allocation, state advancement or stored references.
 */
double columnDtLimit(const soil::Params& p, const double* theta, int m, int stride, double L,
                     double Ldot, double c_col, bool capillary) noexcept;

/**
 * @brief Seed a column to hydrostatic equilibrium above a table at depth `L`.
 *
 * @param[in] p Borrowed validated SI soil-law parameters.
 * @param[out] theta Required strided layer water-content array, dimensionless; at least
 * 1+(m-1)*stride writable doubles.
 * @param[in] m Strictly positive layer count.
 * @param[in] stride Strictly positive distance in doubles between layers; 1 packed or n_cells
 * for a layer-major SoA.
 * @param[in] L Finite column thickness in m.
 * @post Sets each layer's center water content using its height above the table; negative L
 * clamps to zero. Caller owns the array; no allocation or validation, and no other state is
 * updated.
 */
void seedHydrostatic(const soil::Params& p, double* theta, int m, int stride, double L) noexcept;

/**
 * @brief Total water in the column (m of water) — `Σ θ_j·L·Δσ`.
 *
 * @param[in] theta Required borrowed dimensionless layer water contents, at least 1+(m-1)*stride
 * doubles.
 * @param[in] m Strictly positive layer count.
 * @param[in] stride Strictly positive distance in doubles between layers.
 * @param[in] L Finite column thickness, m; negative thickness clamps to zero.
 * @return Summed water storage in metres of water. Read-only, no allocation/validation; contents
 * are not clamped to residual/saturation here.
 */
double columnStorage(const double* theta, int m, int stride, double L) noexcept;

} // namespace openswmm::twoD::sigma

#endif // OPENSWMM_ENGINE_2D_SIGMA_COLUMN_HPP
