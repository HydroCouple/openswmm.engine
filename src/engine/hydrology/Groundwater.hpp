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

#include <vector>

namespace openswmm {

struct SimulationContext;

namespace groundwater {

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

    // Outputs
    std::vector<double> gw_flow;      ///< Lateral GW flow to node (cfs)
    std::vector<double> upper_evap;   ///< Upper zone evap (ft3/sec)
    std::vector<double> lower_evap;   ///< Lower zone evap (ft3/sec)
    std::vector<double> deep_loss;    ///< Deep percolation (ft3/sec)

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
     * @param infil_rate Per-subcatchment infiltration rate (ft/sec).
     * @param sw_head    Per-subcatchment surface water head (ft).
     */
    void execute(SimulationContext& ctx, double dt, double max_evap,
                 const double* infil_rate, const double* sw_head);

    GWSoA& state() { return soa_; }

private:
    GWSoA soa_;

    /// Batch upper zone percolation — VECTORISABLE
    static void batchUpperPerc(
        const double* __restrict__ theta,
        const double* __restrict__ field_cap,
        const double* __restrict__ k_sat,
        const double* __restrict__ k_slope,
        double*       __restrict__ perc,
        int count
    );

    /// Batch lateral GW flow — VECTORISABLE
    static void batchGWFlow(
        const double* __restrict__ lower_depth,
        const double* __restrict__ h_star,
        const double* __restrict__ a1, const double* __restrict__ b1,
        const double* __restrict__ a2, const double* __restrict__ b2,
        const double* __restrict__ a3,
        const double* __restrict__ sw_head,
        double*       __restrict__ gw_flow,
        int count
    );
};

} // namespace groundwater
} // namespace openswmm

#endif // OPENSWMM_GROUNDWATER_HPP
