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
 * @file SoilCharacteristic.hpp
 * @brief G-steps 2 and 13 — the four production soil-characteristic laws and
 *        the closure-A quasi-steady recharge `q₀(hᵤ, h_g)`.
 *
 * @section soilchar_forms The laws
 *
 * Each law supplies effective saturation `Se(ψ)`, water content `θ(ψ)` and
 * relative conductivity `Kr(ψ)`, with suction head `ψ ≥ 0` measured upward
 * from the water table (so `ψ = 0` at the table and `ψ = L` at the surface):
 *
 * | Law | `Kr(ψ)` |
 * |---|---|
 * | Gardner (1958)       | `e^(−αψ)` |
 * | Russo (1988)         | `[(1 + ½αψ)·e^(−½αψ)]²` |
 * | Brooks–Corey (1964)  | `1` for `ψ ≤ ψ_b`, else `(ψ_b/ψ)^(2+3λ)` |
 * | van Genuchten (1980) | `Se^L·[1 − (1 − Se^(1/m))^m]²`, `Se = [1+(αψ)^n]^(−m)` |
 *
 * @section soilchar_q0 How q₀ is computed — read this before trusting a number
 *
 * For **Gardner**, Qu & Duffy's eq. 22 is used verbatim:
 *
 * ```
 * q₀ = Ks · [ 1 − e^(−αL) − α·hᵤ ] / [ αL · (1 − e^(−αL)) ] ,   L = z_s − h_g
 * ```
 *
 * That expression is algebraically identical to
 *
 * ```
 * q₀ = C(L) · [ hᵤ*(L) − hᵤ ] ,
 *      hᵤ*(L) = (1 − e^(−αL)) / α          <- exactly eq. 39, the enslaved form
 *      C(L)   = Ks / ( L · (1 − e^(−αL)) )
 * ```
 *
 * — a relaxation of the column's stored water toward its hydrostatic
 * equilibrium, at a conductivity-set rate. The equivalence is a genuine
 * identity, not an approximation, and it is worth stating because it makes
 * the sign convention obvious (`hᵤ < hᵤ*` ⇒ `q₀ > 0`, downward recharge;
 * `hᵤ > hᵤ*` ⇒ capillary rise) and because it is the form that generalises.
 *
 * **The other three laws use that generalised form**, with `hᵤ*(L)` the
 * hydrostatic-equilibrium storage `∫₀ᴸ θ(ψ) dψ` of *that* law (closed form
 * for Brooks–Corey, Gauss–Legendre for van Genuchten and Russo) and `C(L)`
 * the same conductivity/length scale evaluated with the law's own `Kr`.
 *
 * @warning **This generalisation is ours, not the literature's.** Gardner
 *          reproduces the published closed form exactly and is verified
 *          against it. Russo, Brooks–Corey and van Genuchten reproduce the
 *          correct *equilibrium* (`q₀ = 0` at hydrostatic storage) and the
 *          correct sign, but their *relaxation rate* is a modelling choice
 *          this file makes explicit rather than a published closed form.
 *          Plan step 18's closure-ladder benchmark and step 4's HYDRUS-1D
 *          comparison are what license them; until those run, prefer Gardner
 *          or Russo for anything quantitative, and prefer closure B (the σ
 *          column, which integrates the real Richards flux and needs none of
 *          this) where the answer matters. Recorded for the migration guide.
 *
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_SOIL_CHARACTERISTIC_HPP
#define OPENSWMM_ENGINE_2D_SOIL_CHARACTERISTIC_HPP

#include "SubsurfaceData.hpp"

namespace openswmm::twoD::soil {

/// The parameters one cell's law needs, gathered so the kernels take one
/// argument and the SoA gather happens once per firing.
struct Params {
    SoilChar law     = SoilChar::RUSSO;
    double   Ks      = 1.0e-5;
    double   theta_s = 0.45;
    double   theta_r = 0.10;
    double   alpha   = 2.0;
    double   psi_b   = 0.20;
    double   lambda  = 0.40;
    double   vg_n    = 1.6;
    double   vg_L    = 0.5;
};

/// Effective saturation at suction head @p psi (m, ≥ 0). `Se ∈ (0, 1]`.
double effectiveSaturation(const Params& p, double psi) noexcept;

/// Water content θ(ψ) = θ_r + Se·(θ_s − θ_r).
double waterContent(const Params& p, double psi) noexcept;

/// Relative conductivity `Kr(ψ) ∈ (0, 1]`.
double relativeConductivity(const Params& p, double psi) noexcept;

/// Unsaturated conductivity `K(ψ) = Ks·Kr(ψ)` (m/s).
double conductivity(const Params& p, double psi) noexcept;

/// Suction head at a given effective saturation — the retention inverse,
/// needed to give the σ column a ψ from its θ. Returns 0 at `Se ≥ 1`.
double suctionAtSaturation(const Params& p, double Se) noexcept;

/// Soil-moisture diffusivity `D(θ) = K(ψ)·dψ/dθ` (m²/s) at suction @p psi.
/// Zero when the capillary term is off; evaluated by the analytic
/// `dSe/dψ` of each law.
double diffusivity(const Params& p, double psi) noexcept;

/// Hydrostatic-equilibrium storage of a column of thickness @p L (m of
/// water): `∫₀ᴸ θ(ψ) dψ`. Gardner and Brooks–Corey are closed form; Russo
/// and van Genuchten use fixed-order Gauss–Legendre.
double equilibriumStorage(const Params& p, double L) noexcept;

/// Closure-A quasi-steady recharge (m/s). `+` down, `−` capillary rise.
/// @param L  unsaturated column thickness `z_s − h_g` (m)
/// @param hu bulk unsaturated storage as an equivalent water depth (m)
/// For Gardner this IS eq. 22; see the file header for the other three.
double rechargeQ0(const Params& p, double L, double hu) noexcept;

/// The ENSLAVED reduction: `hᵤ` algebraic in `h_g` — the storage at which
/// `rechargeQ0` vanishes. For Gardner this is eq. 39.
double enslavedStorage(const Params& p, double L) noexcept;

/// `αL`, the dimensionless group the AUTO closure selection reads. For
/// Brooks–Corey and van Genuchten the law's own inverse length replaces α
/// (`1/ψ_b`, and the van Genuchten `α`), so the thresholds mean the same
/// thing across laws.
double alphaL(const Params& p, double L) noexcept;

/// Feddes-style smooth stress multiplier on potential ET, from the column's
/// mean suction. 1 in the readily-available range, falling smoothly to 0 at
/// the wilting point — no `if (infil <= 0)` gate, which is the behaviour §7
/// retires. @p psi_w wilting suction (m).
double feddesStress(double psi, double psi_w) noexcept;

}  // namespace openswmm::twoD::soil

#endif  // OPENSWMM_ENGINE_2D_SOIL_CHARACTERISTIC_HPP
