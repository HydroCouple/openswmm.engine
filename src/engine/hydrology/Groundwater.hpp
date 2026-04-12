/**
 * @file Groundwater.hpp
 * @brief Two-zone groundwater model — batch-oriented ODE integration.
 *
 * @details Each subcatchment has an independent aquifer with upper (unsaturated)
 *          and lower (saturated) zones. The ODE system is:
 *
 *            dθ/dt = (Infil - UpperEvap - UpperPerc) / upperDepth
 *            dH/dt = (UpperPerc - DeepPerc - LowerEvap - GWFlow) / (φ - θ)
 *
 *          Since each subcatchment is independent, the ODE integration is
 *          embarrassingly parallel — vectorise over subcatchments.
 *
 *          The lateral GW flow formula:
 *            Q = a1*(H-H*)^b1 - a2*(Hsw-H*)^b2 + a3*H*Hsw
 *          is arithmetic-only and vectorisable when b1/b2 are uniform.
 *
 * @note Legacy reference: src/legacy/engine/gwater.c
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_GROUNDWATER_HPP
#define OPENSWMM_GROUNDWATER_HPP

#ifndef OPENSWMM_RESTRICT
#  if defined(_MSC_VER)
#    define OPENSWMM_RESTRICT __restrict
#  else
#    define OPENSWMM_RESTRICT __restrict__
#  endif
#endif

#include <vector>
#include "../math/MathExpr.hpp"

namespace openswmm {

struct SimulationContext;

namespace groundwater {

/// GW expression variable indices (matching legacy GWvariables enum).
enum GWVar : int {
    GWV_HGW   = 0,  ///< Water table height
    GWV_HSW   = 1,  ///< Surface water head
    GWV_HCB   = 2,  ///< Channel bottom height (h_star)
    GWV_HGS   = 3,  ///< Ground surface height (total_depth)
    GWV_KS    = 4,  ///< Saturated conductivity
    GWV_K     = 5,  ///< Unsaturated conductivity
    GWV_THETA = 6,  ///< Upper zone moisture content
    GWV_PHI   = 7,  ///< Porosity
    GWV_FI    = 8,  ///< Surface infiltration rate
    GWV_FU    = 9,  ///< Upper zone percolation rate
    GWV_A     = 10, ///< Subcatchment area
    GWV_MAX   = 11
};

/// Variable name table for bind_variables().
static const char* GW_VAR_NAMES[] = {
    "HGW", "HSW", "HCB", "HGS", "KS", "K",
    "THETA", "PHI", "FI", "FU", "A"
};

// ============================================================================
// Per-subcatchment GW state (SoA for vectorization)
// ============================================================================

struct GWSoA {
    int n_subcatch = 0;

    // Aquifer properties (set at init, constant during sim)
    std::vector<double> porosity;
    std::vector<double> field_cap;
    std::vector<double> wilt_point;
    std::vector<double> k_sat;        ///< Saturated conductivity (ft/sec)
    std::vector<double> k_slope;      ///< Exponential decay slope
    std::vector<double> tension_slope;
    std::vector<double> upper_evap_frac;
    std::vector<int>    upper_evap_pat;   ///< Pattern index for monthly evap adjustment (-1 = none)
    std::vector<double> lower_evap_depth;
    std::vector<double> lower_loss_coeff;   ///< Deep percolation coeff
    std::vector<double> total_depth;        ///< Aquifer thickness (ft)

    // Lateral flow coefficients
    std::vector<double> a1, b1;       ///< GW outflow
    std::vector<double> a2, b2;       ///< Surface water interaction
    std::vector<double> a3;           ///< Cross-interaction
    std::vector<double> h_star;       ///< Threshold water table height (ft)

    // State variables (updated each timestep)
    std::vector<double> theta;        ///< Upper zone moisture content (0-φ)
    std::vector<double> lower_depth;  ///< Lower zone depth (ft)

    // State
    std::vector<double> old_flow;     ///< Previous step GW flow (for trapezoidal avg)

    // Outputs
    std::vector<double> gw_flow;      ///< Lateral GW flow to node (cfs)
    std::vector<double> upper_evap;   ///< Upper zone evap (ft3/sec)
    std::vector<double> lower_evap;   ///< Lower zone evap (ft3/sec)
    std::vector<double> deep_loss;    ///< Deep percolation (ft3/sec)

    // Custom flow expressions (from [GWF] section)
    /// Per-subcatch compiled lateral flow expression (added to standard formula).
    std::vector<mathexpr::Expression> lateral_expr;
    /// Per-subcatch compiled deep percolation expression (replaces standard formula).
    std::vector<mathexpr::Expression> deep_expr;

    void resize(int n);
};

// ============================================================================
// GW solver
// ============================================================================

class GWSolver {
public:
    void init(int n_subcatch);

    /**
     * @brief Compute groundwater for all subcatchments (batch).
     *
     * @details Steps (per subcatchment, all independent → vectorisable):
     *   1. Compute upper zone percolation — batch exp() + multiply
     *   2. Compute lateral GW flow — batch pow() + arithmetic
     *   3. Compute deep percolation — batch multiply
     *   4. Compute ET from upper/lower zones — batch clamp
     *   5. Euler step: θ += dθ/dt * dt, H += dH/dt * dt — batch add
     *
     * @param ctx  Simulation context.
     * @param dt   Timestep (seconds).
     * @param max_evap  Maximum evaporation rate (ft/sec, scalar broadcast).
     * @param infil_rate Per-subcatchment infiltration rate (ft/sec over full area).
     * @param sw_head    Per-subcatchment surface water head (ft).
     * @param frac_perv  Per-subcatchment pervious fraction (0-1).
     * @param perv_evap_rate Per-subcatchment pervious evap rate already exerted (ft/sec over full area).
     */
    void execute(SimulationContext& ctx, double dt, double max_evap,
                 const double* infil_rate, const double* sw_head,
                 const double* frac_perv, const double* perv_evap_rate);

    GWSoA& state() { return soa_; }

private:
    GWSoA soa_;

    /// Batch upper zone percolation — VECTORISABLE
    static void batchUpperPerc(
        const double* OPENSWMM_RESTRICT theta,
        const double* OPENSWMM_RESTRICT field_cap,
        const double* OPENSWMM_RESTRICT k_sat,
        const double* OPENSWMM_RESTRICT k_slope,
        double*       OPENSWMM_RESTRICT perc,
        int count
    );

    /// Batch lateral GW flow — VECTORISABLE
    static void batchGWFlow(
        const double* OPENSWMM_RESTRICT lower_depth,
        const double* OPENSWMM_RESTRICT h_star,
        const double* OPENSWMM_RESTRICT a1, const double* OPENSWMM_RESTRICT b1,
        const double* OPENSWMM_RESTRICT a2, const double* OPENSWMM_RESTRICT b2,
        const double* OPENSWMM_RESTRICT a3,
        const double* OPENSWMM_RESTRICT sw_head,
        double*       OPENSWMM_RESTRICT gw_flow,
        int count
    );
};

} // namespace groundwater
} // namespace openswmm

#endif // OPENSWMM_GROUNDWATER_HPP
