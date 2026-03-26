/**
 * @file Runoff.hpp
 * @brief Subcatchment runoff generation — batch-oriented nonlinear reservoir.
 *
 * @details The runoff module is designed for vectorized batch execution:
 *
 *   **Batch architecture:**
 *   1. Batch update rainfall at all gages
 *   2. Batch compute net precipitation for all subcatchments
 *   3. Batch compute infiltration for all pervious subareas
 *   4. Batch compute runoff via nonlinear reservoir (Manning's) for all subareas
 *   5. Batch accumulate subcatchment outflow and mass balance
 *
 *   The nonlinear reservoir equation is:
 *     Q = Alpha * (D - Ds)^(5/3)
 *   where Alpha = W/(n*A) * sqrt(S), applied identically to every subarea.
 *   This is trivially vectorisable over all subcatchments.
 *
 * @note Legacy reference: src/legacy/engine/runoff.c, subcatch.c
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_RUNOFF_HPP
#define OPENSWMM_RUNOFF_HPP

#include "../data/SubcatchData.hpp"
#include "Infiltration.hpp"
#include <vector>

namespace openswmm {

struct SimulationContext;

namespace runoff {

// ============================================================================
// Constants
// ============================================================================

constexpr double MEXP = 5.0 / 3.0;   ///< Manning's exponent

// ============================================================================
// Per-subcatchment subarea state (SoA for vectorization)
// ============================================================================

/**
 * @brief SoA working arrays for all subcatchment subareas.
 *
 * @details Three subareas per subcatchment:
 *   [0] = impervious without depression storage (IMPERV0)
 *   [1] = impervious with depression storage (IMPERV)
 *   [2] = pervious (PERV)
 *
 *   For batch processing, all subcatchments' subareas of the same type
 *   are stored contiguously (e.g., all pervious depths together).
 */
struct RunoffSoA {
    int n_subcatch = 0;

    // Per-subcatchment properties (set at init)
    std::vector<double> area;          ///< Subcatchment area (ft2)
    std::vector<double> width;         ///< Subcatchment width (ft)
    std::vector<double> slope;         ///< Average slope (ft/ft)
    std::vector<double> imperv_pct;    ///< Impervious fraction (0-1)

    // Per-subarea SoA: alpha = runoff coefficient
    std::vector<double> alpha_imperv;  ///< Alpha for impervious (W/n * sqrt(S) / A)
    std::vector<double> alpha_perv;    ///< Alpha for pervious

    // Per-subarea SoA: depression storage (ft)
    std::vector<double> ds_imperv;     ///< Depression storage, impervious
    std::vector<double> ds_perv;       ///< Depression storage, pervious

    // Per-subarea SoA: Manning's n
    std::vector<double> n_imperv;      ///< Manning's n, impervious
    std::vector<double> n_perv;        ///< Manning's n, pervious

    // Per-subarea SoA: ponded depth (state, updated each step)
    std::vector<double> depth_imperv;  ///< Ponded depth, impervious (ft)
    std::vector<double> depth_perv;    ///< Ponded depth, pervious (ft)

    // Per-subcatchment: computed runoff (output)
    std::vector<double> runoff;        ///< Total runoff rate (cfs)
    std::vector<double> evap_loss;     ///< Evaporation loss (ft3)
    std::vector<double> infil_loss;    ///< Infiltration loss (ft3)

    void resize(int n);
    void computeAlpha();  ///< Compute alpha from width, n, slope, area
};

// ============================================================================
// Runoff solver
// ============================================================================

class RunoffSolver {
public:
    void init(SimulationContext& ctx);

    /**
     * @brief Execute one runoff timestep for all subcatchments.
     *
     * @details Steps (all batch):
     *   1. Get rainfall from gages → net precip per subcatchment
     *   2. Compute evaporation rate
     *   3. Batch infiltration (all pervious subareas)
     *   4. Batch nonlinear reservoir routing (all subareas)
     *   5. Accumulate runoff and mass balance
     *
     * @param ctx  Simulation context.
     * @param dt   Runoff timestep (seconds).
     */
    void execute(SimulationContext& ctx, double dt);

private:
    RunoffSoA soa_;

    // Infiltration state (one per subcatchment)
    InfilModel infil_model_ = InfilModel::HORTON;
    std::vector<HortonState>    horton_states_;
    std::vector<GreenAmptState> grnampt_states_;
    std::vector<CurveNumState>  curvenum_states_;

    // Working buffers (reused each step, sized to n_subcatch)
    std::vector<double> precip_;     ///< Net precipitation rate (ft/sec)
    std::vector<double> evap_rate_;  ///< Evaporation rate (ft/sec)
    std::vector<double> infil_rate_; ///< Infiltration rate per subcatch (ft/sec)

    /// Batch nonlinear reservoir: runoff[i] = alpha[i] * max(0, depth[i]-ds[i])^(5/3)
    static void batchNonlinearReservoir(
        const double* __restrict__ alpha,
        const double* __restrict__ depth,
        const double* __restrict__ ds,
        double*       __restrict__ runoff_rate,
        int count
    );

    /// Batch depth update: depth_new = depth_old + (precip - evap - infil - runoff) * dt
    static void batchDepthUpdate(
        double*       __restrict__ depth,
        const double* __restrict__ precip,
        const double* __restrict__ evap,
        const double* __restrict__ infil,
        const double* __restrict__ runoff,
        double dt,
        int count
    );
};

} // namespace runoff
} // namespace openswmm

#endif // OPENSWMM_RUNOFF_HPP
