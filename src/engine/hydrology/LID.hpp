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
 * @file LID.hpp
 * @brief Low Impact Development (LID) control modules.
 *
 * @details Supports 8 LID types with a layered vertical model:
 *   surface → soil → storage → drain
 *
 *   **Vectorization strategy**: Group all LID units of the same type
 *   across all subcatchments into contiguous arrays (similar to XSectGroups).
 *   Each LID type uses the same flux-rate formulas — batch-compute over
 *   all units of that type simultaneously.
 *
 *   **LID type groups (SoA per type):**
 *   - BIO_CELL group: N units → batch biocellFluxRates()
 *   - PERM_PAVEMENT group: M units → batch pavementFluxRates()
 *   - etc.
 *
 * @note Legacy reference: src/legacy/engine/lid.c, lidproc.c
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_LID_HPP
#define OPENSWMM_LID_HPP

#include <vector>
#include <cstddef>

namespace openswmm {

struct SimulationContext;

namespace lid {

// ============================================================================
// LID type codes
// ============================================================================

enum class LIDType : int {
    BIO_CELL        = 0,
    RAIN_GARDEN     = 1,
    GREEN_ROOF      = 2,
    INFIL_TRENCH    = 3,
    PERM_PAVEMENT   = 4,
    RAIN_BARREL     = 5,
    VEG_SWALE       = 6,
    ROOF_DISCON     = 7
};

// ============================================================================
// Layer state (per LID unit)
// ============================================================================

constexpr int SURF = 0;
constexpr int SOIL = 1;
constexpr int STOR = 2;
constexpr int PAVE = 3;
constexpr int N_LAYERS = 4;

// ============================================================================
// LID unit parameters (SoA for batch processing)
// ============================================================================

struct LIDGroupSoA {
    LIDType type = LIDType::BIO_CELL;
    int count = 0;

    // Unit-conversion factors for the model's flow units (legacy UCF()).
    // Layer parameters are converted to internal ft / ft-per-sec at init();
    // these two factors also drive the underdrain head/rate conversion in
    // getDrainRate(). Defaults are the US-customary values (issue #102).
    double ucf_raindepth = 12.0;    ///< ft → in|mm  (RAINDEPTH)
    double ucf_rainfall  = 43200.0; ///< ft/sec → in/hr|mm/hr  (RAINFALL)

    std::vector<int> subcatch_idx;     ///< Which subcatchment this unit belongs to
    std::vector<int> control_idx;      ///< LID control index (into ctx.lid_controls)
    std::vector<double> area;          ///< Unit area (ft2)
    std::vector<double> from_imperv;   ///< Fraction of impervious runoff treated (0-1)
    std::vector<double> from_perv;     ///< Fraction of pervious runoff treated (0-1)
    std::vector<int>    to_perv;       ///< Route surface outflow to pervious area (1=yes)
    std::vector<int>    drain_node;    ///< Resolved drain-to node index (-1=none)
    std::vector<int>    drain_subcatch;///< Resolved drain-to subcatch index (-1=none)
    std::vector<double> inflow;        ///< Per-unit inflow rate (ft/sec) — set before execute()
    std::vector<double> evap_rate_unit;///< Per-unit effective PET rate (ft/sec) — filled by execute()

    // Surface layer
    std::vector<double> surf_store;    ///< Surface storage depth (ft)
    std::vector<double> surf_rough;    ///< Surface Manning's n
    std::vector<double> surf_slope;    ///< Surface slope

    // Soil layer
    std::vector<double> soil_thick;    ///< Soil thickness (ft)
    std::vector<double> soil_poros;    ///< Soil porosity
    std::vector<double> soil_fc;       ///< Soil field capacity
    std::vector<double> soil_wp;       ///< Soil wilting point
    std::vector<double> soil_ksat;     ///< Soil saturated K (ft/sec)
    std::vector<double> soil_kslope;   ///< Conductivity slope
    std::vector<double> soil_suction;  ///< Suction head for Green-Ampt (ft)

    // Storage layer
    std::vector<double> stor_thick;    ///< Storage thickness (ft)
    std::vector<double> stor_void;     ///< Storage void fraction
    std::vector<double> stor_ksat;     ///< Storage exfiltration K (ft/sec)
    std::vector<double> stor_clog;     ///< Storage clogging factor (ft)
    std::vector<int>    stor_covered;  ///< 1 if rain barrel is covered (blocks rainfall)

    // Drain
    std::vector<double> drain_coeff;   ///< Drain coefficient
    std::vector<double> drain_expon;   ///< Drain exponent
    std::vector<double> drain_offset;  ///< Drain offset depth (ft)
    std::vector<double> drain_delay;   ///< Drain delay time (sec, rain barrel)
    std::vector<double> drain_hopen;   ///< Head to open drain valve (ft)
    std::vector<double> drain_hclose;  ///< Head to close drain valve (ft)
    std::vector<int>    drain_open;    ///< Current drain valve state (1=open, 0=closed)

    // Pavement layer (PERM_PAVEMENT)
    std::vector<double> pave_thick;        ///< Pavement thickness (ft)
    std::vector<double> pave_void;         ///< Pavement void fraction
    std::vector<double> pave_imperv_frac;  ///< Impervious fraction of pavement
    std::vector<double> pave_ksat;         ///< Pavement saturated K (ft/sec)
    std::vector<double> pave_clog_factor;  ///< Pavement clog factor (ft of treated volume)
    std::vector<double> pave_regen_days;   ///< Pavement regeneration interval (days)
    std::vector<double> pave_regen_deg;    ///< Pavement regeneration degree (0-1)
    std::vector<double> next_regen_day;    ///< Next day for pavement regeneration (OADate)

    // Drainage mat layer (GREEN_ROOF)
    std::vector<double> drainmat_thick;    ///< Drainage mat thickness (ft)
    std::vector<double> drainmat_void;     ///< Drainage mat void fraction
    std::vector<double> drainmat_rough;    ///< Drainage mat Manning's roughness

    // Surface geometry (for Manning's outflow)
    std::vector<double> surf_void_frac;    ///< Surface void fraction (default 1.0)
    std::vector<double> surf_alpha;        ///< Surface Manning alpha = sqrt(slope)/n
    std::vector<double> surf_side_slope;   ///< Swale side slope (run/rise)
    std::vector<double> full_width;        ///< Full width for Manning's flow (ft)
    std::vector<double> dry_time;          ///< Seconds since last rainfall

    // State variables (updated each step)
    std::vector<double> surf_depth;    ///< Current surface ponded depth
    std::vector<double> soil_moist;    ///< Current soil moisture (0-porosity)
    std::vector<double> stor_depth;    ///< Current storage depth
    std::vector<double> pave_depth;    ///< Current pavement depth

    // Outputs (per unit)
    std::vector<double> surface_runoff;
    std::vector<double> drain_flow;
    std::vector<double> evap_loss;   ///< Evaporation loss this step (ft of depth)
    std::vector<double> infil_loss;  ///< Native infiltration loss this step (ft)

    // Pollutant drain removal fractions: drain_rmvl[unit * n_pollutants + pollutant]
    std::vector<double> drain_rmvl;  ///< Removal fraction per unit per pollutant
    int n_pollutants = 0;            ///< Number of pollutants (for indexing drain_rmvl)

    // ---- A4: per-layer INFLOW rates (ft/sec of water per unit area) ----
    // Each layer receives from exactly one place: the layer above it in the
    // present stack, or externally for the topmost present layer. Every
    // batch*Flux routine already computes these as locals (soil_infil,
    // soil_perc, pavePerc, storageInflow, ...); they are published here
    // because a complete-mix age needs the INFLOW and nothing else — the
    // outflow leaves at the layer's own age.
    //
    // These are NOT the `f_old_*` below. Those are the Modified Puls
    // time-weighting term: `f_old_surf` is the NET dx/dt of the surface
    // layer, and the other three are never written at all. A net rate of
    // change is exactly the quantity that made phase A3 report elapsed time
    // instead of age.
    std::vector<double> in_surf;      ///< External inflow onto the surface
    std::vector<double> in_pave;      ///< Surface → pavement
    std::vector<double> in_soil;      ///< Layer above → soil
    std::vector<double> in_stor;      ///< Layer above → storage

    // Previous flux rates (for Modified Puls time weighting)
    std::vector<double> f_old_surf;   ///< Previous surface flux rate
    std::vector<double> f_old_soil;   ///< Previous soil flux rate
    std::vector<double> f_old_stor;   ///< Previous storage flux rate
    std::vector<double> f_old_pave;   ///< Previous pavement flux rate

    // Water balance tracking (cumulative per unit)
    std::vector<double> wb_inflow;     ///< Total inflow volume (ft)
    std::vector<double> wb_evap;       ///< Total evaporation volume (ft)
    std::vector<double> wb_infil;      ///< Total exfiltration volume (ft)
    std::vector<double> wb_surf_flow;  ///< Total surface outflow volume (ft)
    std::vector<double> wb_drain_flow; ///< Total drain outflow volume (ft)
    std::vector<double> wb_init_vol;   ///< Initial stored volume (ft)
    std::vector<double> wb_final_vol;  ///< Final stored volume (ft)
    std::vector<double> vol_treated;   ///< Cumulative volume treated (ft, for clog model)

    void resize(int n);
};

// ============================================================================
// LID solver
// ============================================================================

class LIDSolver {
public:
    void init(SimulationContext& ctx);

    /// Access a type group (for testing or external queries).
    LIDGroupSoA& group(int type_index) { return groups_[static_cast<size_t>(type_index)]; }
    const LIDGroupSoA& group(int type_index) const { return groups_[static_cast<size_t>(type_index)]; }
    int numGroups() const { return static_cast<int>(groups_.size()); }

    /// Total water volume currently stored in all LID units (ft³), for the
    /// runoff mass balance (legacy lid_getStoredVolume()).
    double storedVolume() const;

    /**
     * @brief Compute LID performance for all units (batch by type).
     *
     * @details For each LID type group:
     *   1. Gather inputs (rainfall, evap, inflow from impervious area)
     *   2. Batch flux-rate computation (type-specific, vectorisable)
     *   3. Batch Euler integration of layer depths
     *   4. Scatter outputs to subcatchment runoff totals
     *
     * @param ctx       Simulation context.
     * @param dt        Timestep (seconds).
     * @param rainfall  Rainfall rate (ft/sec).
     * @param evap_rate Evaporation rate (ft/sec). Per-unit effective rates
     *                  are resolved internally from any prescribed PET
     *                  forcing on each unit's parent subcatchment.
     */
    void execute(SimulationContext& ctx, double dt,
                 double rainfall, double evap_rate);

    /// Batch bio-cell flux rates — VECTORISABLE
    static void batchBioCellFlux(LIDGroupSoA& g, double rainfall,
                                  const double* evap_rate, double dt);

    /// Batch rain barrel flux rates — VECTORISABLE (simplest)
    static void batchBarrelFlux(LIDGroupSoA& g, double rainfall, double dt);

    /// Batch vegetative swale flux rates — VECTORISABLE
    static void batchSwaleFlux(LIDGroupSoA& g, double rainfall,
                                const double* evap_rate, double dt);

    /// Batch green roof flux rates — VECTORISABLE
    static void batchGreenRoofFlux(LIDGroupSoA& g, double rainfall,
                                    const double* evap_rate, double dt);

    /// Batch infiltration trench flux rates — VECTORISABLE
    /// Surface drains directly to storage (no soil layer), matching legacy trenchFluxRates().
    static void batchInfilTrenchFlux(LIDGroupSoA& g, double rainfall,
                                      const double* evap_rate, double dt);

    /// Batch permeable pavement flux rates — VECTORISABLE
    static void batchPavementFlux(LIDGroupSoA& g, double rainfall,
                                   const double* evap_rate, double dt);

    /// Batch roof disconnection flux rates — VECTORISABLE
    static void batchRoofDisconFlux(LIDGroupSoA& g, double rainfall,
                                     const double* evap_rate, double dt);

    /// Batch Modified Puls solver for swale (omega=0.5, iterative).
    /// Computes flux rates and iterates until convergence.
    static void batchSwaleModPuls(LIDGroupSoA& g, double rainfall,
                                   const double* evap_rate, double dt);

private:
    std::vector<LIDGroupSoA> groups_;
};

} // namespace lid
} // namespace openswmm

#endif // OPENSWMM_LID_HPP
