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
 * @file SubsurfaceData.hpp
 * @brief G-step 1 — state, parameters and options for the two-zone
 *        groundwater kernel (TWO_ZONE_GROUNDWATER_EXPLICIT_LTS_PLAN §4).
 *
 * @details Two zones per mesh cell, glued at a moving water table:
 *
 *          - **Saturated:** one state `hg` (m of saturated thickness above the
 *            aquifer bottom), updated by an explicit FV balance of recharge,
 *            lateral Darcy across the cell's edges, deep loss and node
 *            exchange (§2.1).
 *          - **Unsaturated:** either closure A — one bulk state `hu` (m of
 *            water stored as an equivalent depth) driven by a closed-form
 *            quasi-steady recharge — or closure B, an explicit σ-coordinate
 *            column of `m` layers whose bottom flux IS the recharge (§2.3).
 *
 *          **Everything here is SI.** The 2D module runs in metres and
 *          seconds throughout; the 1D engine's feet never reach this code.
 *          `[2D_AQUIFER]` rows are authored in the project's own units and
 *          converted once, at parse.
 *
 *          **Cell-generic.** A cell is a triangle or a quad; nothing here
 *          divides by 3 or indexes a fixed edge count. Lateral edges come
 *          from the mesh's own per-cell slot range.
 *
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_SUBSURFACE_DATA_HPP
#define OPENSWMM_ENGINE_2D_SUBSURFACE_DATA_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace openswmm::twoD {

/// Soil-characteristic law: how `K(ψ)` and `θ(ψ)` are shaped.
/// (Draft plan §2.5. Broadbridge–White is verification-only and is not a
/// production selection — it lives in the test harness, not here.)
enum class SoilChar : int8_t {
    RUSSO         = 0,  ///< default; Mualem-consistent, same α slot as Gardner
    GARDNER       = 1,  ///< K = Ks·e^(αψ); the closed form of eq. 22
    BROOKS_COREY  = 2,  ///< piecewise about the air-entry pressure
    VAN_GENUCHTEN = 3   ///< Rosetta/UNSODA parameter databases
};

/// Unsaturated-zone closure, resolved per cell from AUTO at initialize.
enum class GwClosure : int8_t {
    AUTO        = -1, ///< select from αL at initialize (never stored per cell)
    CLOSED_FORM = 0,  ///< closure A: bulk `hu` + quasi-steady recharge
    ENSLAVED    = 1,  ///< closure A reduced: `hu` algebraic in `hg` (eq. 39)
    SIGMA       = 2   ///< closure B: explicit σ column of `m` layers
};

/// Node ↔ aquifer exchange configuration for one coupled node.
/// Absent bed layer ⇒ direct Darcy (MODFLOW-River config a).
struct GwNodeBed {
    int    node = -1;   ///< 1D node index
    int    cell = -1;   ///< containing mesh cell, resolved at initialize
    double Kc   = 0.0;  ///< semi-confining bed conductivity (m/s); 0 = direct
    double dC   = 0.0;  ///< bed thickness (m)
    double area = 0.0;  ///< exchange area (m²); 0 ⇒ derived from the cell area
};

/// `[2D_AQUIFER_OPTIONS]`. Defaults are the plan's starred values.
struct GwOptions {
    SoilChar  soil_char = SoilChar::RUSSO;
    GwClosure closure   = GwClosure::AUTO;
    int       m_layers  = 8;      ///< σ layers under closure B (global default)
    bool      capillary_diff = false;  ///< closure B optional diffusive term (D-N3: OFF)
    double    c_gw   = 0.5;       ///< saturated stability safety factor
    double    c_col  = 0.9;       ///< column stability safety factor
    bool      force_closed_form = false;  ///< suppress the αL ≥ 5 auto-override
    /// `MODE PER_SUBCATCH` — one degenerate cell per subcatchment, no mesh
    /// sections, lateral flux zero, node exchange to the outlet node. This is
    /// G1's vehicle and shares the whole code path (§8).
    bool      per_subcatch = false;
    /// Dunne saturation-excess transfer to the surface twin. Default ON —
    /// it IS the mass balance (G0 sign-off, draft decision 9); the flag
    /// exists for compatibility comparisons, not as a modelling choice.
    bool      dunne = true;
    /// GW_ET: NONE | CAPILLARY_RISE | BOUNDARY_ET | BOTH.
    std::string gw_et = "NONE";
    /// True once any [2D_AQUIFER*] row was authored.
    bool      authored = false;
};

/// One authored `[2D_AQUIFER]` row, before resolution onto cells.
/// Scope `*` / `TAG <t>` / `CELL <n>`, resolved `* < TAG < CELL` by order.
struct GwAquiferRow {
    int         scope = 0;      ///< 0 GLOBAL, 1 TAG, 2 CELL (mirrors GwScope)
    std::string tag;
    int         cell = -1;      ///< 0-based

    double Ks      = 1.0e-5;    ///< saturated hydraulic conductivity (m/s)
    double zs      = 5.0;       ///< soil column thickness, bottom→surface (m)
    double theta_s = 0.45;      ///< porosity / saturated water content
    double theta_r = 0.10;      ///< residual water content
    double alpha   = 2.0;       ///< Gardner/Russo sorptive number (1/m)
    /// Brooks–Corey: `psi_b` air-entry head (m, positive) and `lambda` shape.
    double psi_b   = 0.20;
    double lambda  = 0.40;
    /// van Genuchten: `n` (m = 1 − 1/n) and the Mualem exponent `L`.
    double vg_n    = 1.6;
    double vg_L    = 0.5;
    /// Deep-loss coefficient (m/s at full saturation): q⁻ = c_loss·hg/zs.
    double c_loss  = 0.0;
    /// Initial saturated thickness (m). < 0 ⇒ the option default seeds it.
    double hg0     = -1.0;

    SoilChar  soil_char = SoilChar::RUSSO;
    GwClosure closure   = GwClosure::AUTO;
    int       m_layers  = -1;   ///< < 0 ⇒ the global M_LAYERS
    bool      soil_char_set = false, closure_set = false;
};

/**
 * @brief Per-cell resolved parameters and state (SoA).
 *
 * @details Sized to the mesh cell count at initialize. Under
 *          `MODE PER_SUBCATCH` the "mesh" is one degenerate cell per
 *          subcatchment and every lateral term is absent — the identical
 *          code path, which is what makes G1 and G2 the same kernel.
 */
struct SubsurfaceState {
    int n_cells = 0;
    int m_layers = 0;            ///< σ layers per closure-B column (fixed)

    // ---- resolved parameters (per cell) ---------------------------------
    std::vector<double>  Ks, zs, theta_s, theta_r, alpha;
    std::vector<double>  psi_b, lambda, vg_n, vg_L, c_loss;
    std::vector<int8_t>  soil_char;   ///< SoilChar
    std::vector<int8_t>  closure;     ///< GwClosure, AUTO already resolved
    std::vector<double>  area;        ///< cell planimetric area (m²)
    std::vector<double>  z_bed;       ///< aquifer bottom elevation (m) = cell z − zs

    // ---- state ----------------------------------------------------------
    std::vector<double>  hg;          ///< saturated thickness (m)
    std::vector<double>  hu;          ///< closure A: bulk unsat storage (m of water)
    /// Closure B: layer water content θ, `[layer * n_cells + cell]` — layer-major
    /// so a fixed-layer sweep walks contiguous cells and vectorizes ACROSS
    /// columns (the σ pivot's key win over the draft's per-cell MOC column).
    std::vector<double>  theta_sigma;

    // ---- per-firing diagnostics + transport donors ------------------------
    std::vector<double>  q0_last;     ///< recharge (m/s), + down, − capillary rise
    std::vector<double>  qnode_last;  ///< node exchange (m³/s), + into the pipe
    std::vector<double>  qlat_last;   ///< net lateral Darcy into the cell (m³/s)
    std::vector<double>  qdeep_last;  ///< deep loss (m/s)
    std::vector<double>  qet_last;    ///< subsurface ET (m/s, ≥ 0 out)
    std::vector<double>  dunne_last;  ///< saturation-excess to the surface (m³/s)
    std::vector<double>  qplus_last;  ///< infiltration delivered in (m/s)

    // ---- LTS ------------------------------------------------------------
    std::vector<double>  dt_cell;     ///< min(Δt_g, Δt_u) per cell (s)
    std::vector<uint8_t> tier;        ///< assigned tier
    /// Lateral Darcy side accumulators, one pair per unique GW edge — the
    /// same ±ΔV strategy as the surface `facc_L_/facc_R_`, which is what
    /// makes cross-tier conservation a property inherited rather than
    /// re-proved (G-B).
    std::vector<double>  eacc_L, eacc_R;
    /// Cross-domain accumulator: volume (m³) the SURFACE booked for this GW
    /// cell at the surface's finer cadence, gathered at the GW firing.
    std::vector<double>  xacc_from_surface;
    /// …and the reverse: volume the GW cell owes the surface twin (Dunne,
    /// exfiltration), gathered by the surface cell.
    std::vector<double>  xacc_to_surface;
    /// Node-exchange accumulator, per coupled node (m³), booked at tier-0
    /// node-head sampling and gathered at the GW cell's firing.
    std::vector<double>  nacc;

    // ---- ledger (m³, cumulative) ----------------------------------------
    double led_recharge = 0.0;   ///< unsat → sat (negative = capillary rise)
    double led_lateral  = 0.0;   ///< net lateral Darcy across the domain edge
    double led_deep     = 0.0;   ///< deep percolation out
    double led_node     = 0.0;   ///< node exchange, + out of the aquifer
    double led_dunne    = 0.0;   ///< saturation excess to the surface
    double led_caprise  = 0.0;   ///< capillary rise (the negative recharge share)
    double led_et       = 0.0;   ///< subsurface ET out
    double led_infil_in = 0.0;   ///< q⁺ delivered from the surface
    double led_init_storage  = 0.0;
    double led_final_storage = 0.0;

    bool active = false;         ///< the kernel ran this simulation

    void resize(int n, int m);
    /// Total water in both zones (m³) — the continuity check's storage term.
    double storage() const noexcept;
};

/**
 * @brief Project-unit → SI multipliers for the four kinds of aquifer number.
 *
 * @details The authored `SubsurfaceConfig` is never converted in place. It
 *          stays in the user's units for its whole life, and the SI values
 *          exist only in `SubsurfaceState`, where the kernel needs them.
 *          That is what makes the writer trivial and correct: it writes the
 *          rows it parsed, unchanged, so a load-and-save with no edits is
 *          byte-identical and no double-conversion is reachable. (An earlier
 *          shape converted the config and let the writer invert; a hotstart
 *          re-initialize then converted twice and shrank every aquifer by
 *          3.28×. The state/config split removes the bug rather than
 *          guarding it with a flag.)
 */
struct GwUnitFactors {
    double length = 1.0;   ///< ft → m,      or m → m
    double rate   = 1.0;   ///< in/hr → m/s, or mm/hr → m/s
    double inv_len = 1.0;  ///< 1/ft → 1/m,  or 1/m → 1/m
    double area   = 1.0;   ///< ft² → m²,    or m² → m²
};

/// Everything the parser fills, before resolution onto cells.
struct SubsurfaceConfig {
    GwOptions                 options;
    std::vector<GwAquiferRow> rows;
    std::vector<GwNodeBed>    node_beds;

    bool empty() const noexcept {
        return !options.authored && rows.empty() && node_beds.empty();
    }
    void clear() { *this = SubsurfaceConfig{}; }
};

const char* soilCharToken(SoilChar s) noexcept;
bool        parseSoilChar(const std::string& t, SoilChar& s) noexcept;
const char* gwClosureToken(GwClosure c) noexcept;
bool        parseGwClosure(const std::string& t, GwClosure& c) noexcept;

}  // namespace openswmm::twoD

#endif  // OPENSWMM_ENGINE_2D_SUBSURFACE_DATA_HPP
