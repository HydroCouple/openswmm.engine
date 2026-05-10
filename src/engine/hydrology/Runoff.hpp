/**
 * @file Runoff.hpp
 * @brief Subcatchment runoff generation — batch-oriented nonlinear reservoir.
 *
 * @details Three subareas per subcatchment (matching legacy subcatch.c):
 *   - IMPERV0: Impervious with zero depression storage (PctZero fraction)
 *   - IMPERV1: Impervious with depression storage
 *   - PERV:    Pervious with depression storage and infiltration
 *
 *   Depth integration uses RK45 adaptive ODE solver (matching legacy
 *   odesolve.c) for implicit solution of:
 *     dd/dt = inflow - alpha * (d - Ds)^(5/3)
 *
 * @note Legacy reference: src/legacy/engine/runoff.c, subcatch.c, odesolve.c
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
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

constexpr double MEXP = 5.0 / 3.0;       ///< Manning's exponent
constexpr double ODETOL = 0.0001;         ///< ODE solver tolerance (matching legacy)
constexpr double PHI = 1.486;             ///< Manning's US customary constant

// ============================================================================
// Per-subcatchment subarea state (SoA for vectorization)
// ============================================================================

struct RunoffSoA {
    int n_subcatch = 0;

    // Per-subcatchment properties (set at init)
    std::vector<double> area;          ///< Subcatchment area (ft²)
    std::vector<double> width;         ///< Subcatchment width (ft)
    std::vector<double> slope;         ///< Average slope (ft/ft)
    std::vector<double> imperv_pct;    ///< Impervious fraction (0-1)
    std::vector<double> imperv0_pct;   ///< Fraction of imperv with zero dStore (0-1)

    // Per-subarea SoA: alpha = runoff coefficient
    std::vector<double> alpha_imperv;  ///< Alpha for impervious subareas
    std::vector<double> alpha_perv;    ///< Alpha for pervious subarea

    // Per-subarea SoA: depression storage (ft)
    std::vector<double> ds_imperv;     ///< Depression storage for IMPERV1
    std::vector<double> ds_perv;       ///< Depression storage for PERV

    // Per-subarea SoA: Manning's n
    std::vector<double> n_imperv;      ///< Manning's n, impervious
    std::vector<double> n_perv;        ///< Manning's n, pervious

    // Per-subarea SoA: ponded depth (state, updated each step)
    std::vector<double> depth_imperv0; ///< Ponded depth, IMPERV0 (ft) — dStore=0
    std::vector<double> depth_imperv1; ///< Ponded depth, IMPERV1 (ft) — dStore>0
    std::vector<double> depth_perv;    ///< Ponded depth, PERV (ft)

    // Per-subarea SoA: previous-step runoff rates (ft/sec, area-averaged)
    // Used for inter-subarea routing (legacy subcatch_getRunon)
    std::vector<double> old_runoff_imperv0; ///< Previous IMPERV0 runoff (ft/sec)
    std::vector<double> old_runoff_imperv1; ///< Previous IMPERV1 runoff (ft/sec)
    std::vector<double> old_runoff_perv;    ///< Previous PERV runoff (ft/sec)

    // Per-subcatchment: computed runoff (output)
    std::vector<double> runoff;             ///< Total runoff rate (cfs)
    std::vector<double> evap_loss;          ///< Evaporation loss (ft3)
    std::vector<double> infil_loss;         ///< Infiltration loss (ft3)

    // Per-subcatchment: per-subarea runoff CFS from non-LID area (Gap #23)
    // Used by SWMMEngine to compute LID unit inflow from impervious/pervious fractions.
    std::vector<double> imperv_runoff_cfs;  ///< Impervious subarea runoff (CFS, non-LID area)
    std::vector<double> perv_runoff_cfs;    ///< Pervious subarea runoff (CFS, non-LID area)

    void resize(int n);
    void computeAlpha();
};

// ============================================================================
// Runoff solver
// ============================================================================

class RunoffSolver {
public:
    void init(SimulationContext& ctx);
    /**
     * @param infil_factor    Monthly infiltration rate multiplier (default 1.0).
     * @param recovery_factor Monthly soil recovery multiplier (default 1.0).
     */
    void execute(SimulationContext& ctx, double dt, double evap_rate = 0.0,
                 double infil_factor = 1.0, double recovery_factor = 1.0,
                 int month = -1);

    const RunoffSoA& soa() const { return soa_; }

    // -----------------------------------------------------------------------
    // Hot start helpers — Gap #54
    // -----------------------------------------------------------------------

    /**
     * @brief Pack infiltration state for subcatchment @p i into a flat 6-element array.
     *
     * Layout (matches legacy infil_getState() encoding):
     *  - Horton/ModHorton (0,1): [tp, Fe, Fmh, 0, 0, 0]
     *  - GA/ModGA        (2,3): [IMD, F, Fu, T, sat(0/1), 0]
     *  - CurveNum        (4):   [S, Se, P, F, f, T]
     *
     * @param i        Subcatchment index.
     * @param model    [out] InfilModel enum value.
     * @param state    [out] 6-element flat state array.
     */
    void infil_get_state(int i, int& model, double state[6]) const noexcept;

    /**
     * @brief Restore infiltration state for subcatchment @p i from a flat 6-element array.
     *
     * Only updates the state fields; does not reinitialise parameters.
     * Silently ignores out-of-range @p i or mismatched model type.
     *
     * @param i        Subcatchment index.
     * @param model    InfilModel enum value stored in the hot start file.
     * @param state    6-element flat state array written by infil_get_state().
     */
    void infil_set_state(int i, int model, const double state[6]) noexcept;

private:
    RunoffSoA soa_;

    // Infiltration state (one per subcatchment)
    std::vector<InfilModel>     infil_models_;   ///< Per-subcatchment model type (BUG FIX: was a single shared field)
    std::vector<HortonState>    horton_states_;
    std::vector<GreenAmptState> grnampt_states_;
    std::vector<CurveNumState>  curvenum_states_;

    // Working buffers (reused each step, sized to n_subcatch)
    std::vector<double> precip_;
    std::vector<double> evap_rate_;
    std::vector<double> infil_rate_;

    /// Solve dd/dt = inflow - alpha*(d-Ds)^(5/3) using RK45.
    /// Matches legacy updatePondedDepth() + odesolve_integrate().
    static void updatePondedDepth(double& depth, double inflow, double alpha,
                                  double dStore, double dt);

    /// Compute runoff rate from final depth (after ODE integration).
    static double getRunoffRate(double depth, double dStore, double alpha);
};

} // namespace runoff
} // namespace openswmm

#endif // OPENSWMM_RUNOFF_HPP
