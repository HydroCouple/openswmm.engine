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
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
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

    std::vector<int> subcatch_idx;     ///< Which subcatchment this unit belongs to
    std::vector<double> area;          ///< Unit area (ft2)

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

    // Storage layer
    std::vector<double> stor_thick;    ///< Storage thickness (ft)
    std::vector<double> stor_void;     ///< Storage void fraction
    std::vector<double> stor_ksat;     ///< Storage exfiltration K (ft/sec)

    // Drain
    std::vector<double> drain_coeff;   ///< Drain coefficient
    std::vector<double> drain_expon;   ///< Drain exponent
    std::vector<double> drain_offset;  ///< Drain offset depth (ft)

    // Pavement layer (PERM_PAVEMENT)
    std::vector<double> pave_thick;        ///< Pavement thickness (ft)
    std::vector<double> pave_void;         ///< Pavement void fraction
    std::vector<double> pave_imperv_frac;  ///< Impervious fraction of pavement
    std::vector<double> pave_ksat;         ///< Pavement saturated K (ft/sec)
    std::vector<double> pave_clog_factor;  ///< Pavement clog factor (ft of treated volume)

    // Drainage mat layer (GREEN_ROOF)
    std::vector<double> drainmat_thick;    ///< Drainage mat thickness (ft)
    std::vector<double> drainmat_void;     ///< Drainage mat void fraction
    std::vector<double> drainmat_rough;    ///< Drainage mat Manning's roughness

    // Surface geometry (for Manning's outflow)
    std::vector<double> surf_void_frac;    ///< Surface void fraction (default 1.0)
    std::vector<double> surf_alpha;        ///< Surface Manning alpha = sqrt(slope)/n
    std::vector<double> full_width;        ///< Full width for Manning's flow (ft)

    // State variables (updated each step)
    std::vector<double> surf_depth;    ///< Current surface ponded depth
    std::vector<double> soil_moist;    ///< Current soil moisture (0-porosity)
    std::vector<double> stor_depth;    ///< Current storage depth
    std::vector<double> pave_depth;    ///< Current pavement depth

    // Outputs (per unit)
    std::vector<double> surface_runoff;
    std::vector<double> drain_flow;
    std::vector<double> evap_loss;
    std::vector<double> infil_loss;

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
     * @param evap_rate Evaporation rate (ft/sec).
     */
    void execute(SimulationContext& ctx, double dt,
                 double rainfall, double evap_rate);

private:
    std::vector<LIDGroupSoA> groups_;

    /// Batch bio-cell flux rates — VECTORISABLE
    static void batchBioCellFlux(LIDGroupSoA& g, double rainfall,
                                  double evap_rate, double dt);

    /// Batch rain barrel flux rates — VECTORISABLE (simplest)
    static void batchBarrelFlux(LIDGroupSoA& g, double rainfall, double dt);

    /// Batch vegetative swale flux rates — VECTORISABLE
    static void batchSwaleFlux(LIDGroupSoA& g, double rainfall,
                                double evap_rate, double dt);

    /// Batch green roof flux rates — VECTORISABLE
    /// Like biocell but drainage mat replaces storage exfiltration
    static void batchGreenRoofFlux(LIDGroupSoA& g, double rainfall,
                                    double evap_rate, double dt);

    /// Batch permeable pavement flux rates — VECTORISABLE
    /// Surface → pavement → (optional soil) → storage → drain
    static void batchPavementFlux(LIDGroupSoA& g, double rainfall,
                                   double evap_rate, double dt);

    /// Batch roof disconnection flux rates — VECTORISABLE
    /// Simple rainfall → surface ponding → overflow/drain split
    static void batchRoofDisconFlux(LIDGroupSoA& g, double rainfall,
                                     double evap_rate, double dt);
};

} // namespace lid
} // namespace openswmm

#endif // OPENSWMM_LID_HPP
