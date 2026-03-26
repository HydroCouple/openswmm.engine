/**
 * @file Runoff.cpp
 * @brief Subcatchment runoff — batch-oriented, vectorisable.
 *
 * @details The key batch kernels:
 *   - batchNonlinearReservoir: Q[i] = alpha[i] * max(0, D[i]-Ds[i])^(5/3)
 *     → pure arithmetic, no branches (clamp handles the max), vectorisable
 *   - batchDepthUpdate: D_new = D_old + (P - E - I - Q)*dt
 *     → vectorisable element-wise addition
 *   - Infiltration: per-subcatchment (model-specific state), but within
 *     each model type the computation is uniform and could be further
 *     batched by model type in the future
 *
 * @note Legacy reference: src/legacy/engine/runoff.c, subcatch.c
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "Runoff.hpp"
#include "Gage.hpp"
#include "../core/SimulationContext.hpp"
#include "../core/UnitConversion.hpp"
#include "../math/SIMD.hpp"

#include <cmath>
#include <algorithm>

// OpenMP support — graceful degradation when not available
#if defined(SWMM_USE_OPENMP)
#include <omp.h>
#endif

namespace openswmm {
namespace runoff {

// ============================================================================
// RunoffSoA
// ============================================================================

void RunoffSoA::resize(int n) {
    n_subcatch = n;
    auto un = static_cast<std::size_t>(n);

    area.assign(un, 0.0);
    width.assign(un, 0.0);
    slope.assign(un, 0.0);
    imperv_pct.assign(un, 0.0);

    alpha_imperv.assign(un, 0.0);
    alpha_perv.assign(un, 0.0);
    ds_imperv.assign(un, 0.0);
    ds_perv.assign(un, 0.0);
    n_imperv.assign(un, 0.01);
    n_perv.assign(un, 0.1);

    depth_imperv.assign(un, 0.0);
    depth_perv.assign(un, 0.0);

    runoff.assign(un, 0.0);
    evap_loss.assign(un, 0.0);
    infil_loss.assign(un, 0.0);
}

void RunoffSoA::computeAlpha() {
    // Alpha = (width / n_manning) * sqrt(slope) / area
    // This is the coefficient in Q = alpha * excess_depth^(5/3)
    // where Q is in ft/sec (depth rate) and excess_depth in ft
#if defined(SWMM_USE_OPENMP)
#pragma omp parallel for schedule(static)
#endif
    for (int i = 0; i < n_subcatch; ++i) {
        auto ui = static_cast<std::size_t>(i);
        double sq_slope = std::sqrt(slope[ui]);
        if (area[ui] > 0.0) {
            alpha_imperv[ui] = (n_imperv[ui] > 0.0)
                ? width[ui] / n_imperv[ui] * sq_slope / area[ui] : 0.0;
            alpha_perv[ui] = (n_perv[ui] > 0.0)
                ? width[ui] / n_perv[ui] * sq_slope / area[ui] : 0.0;
        }
    }
}

// ============================================================================
// Batch nonlinear reservoir — VECTORISABLE
// ============================================================================

void RunoffSolver::batchNonlinearReservoir(
    const double* __restrict__ alpha,
    const double* __restrict__ depth,
    const double* __restrict__ ds,
    double*       __restrict__ runoff_rate,
    int count
) {
    // Q[i] = alpha[i] * max(0, depth[i] - ds[i])^(5/3)
    //
    // Step 1: Compute excess depth = depth - ds into runoff_rate as scratch.
    // Step 2: Compute pow(excess, 5/3) and multiply by alpha.
    // The inner pow() prevents full SIMD vectorisation, but the excess
    // computation and the final multiply are vectorisable. We use pragmas
    // to hint the compiler that iterations are independent.

    auto un = static_cast<std::size_t>(count);

    // Compute excess depths into runoff_rate (scratch space)
    // This subtraction is vectorisable via simd::add with negated ds,
    // but since ds is const we do a simple loop with ivdep.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC ivdep
#endif
    for (int i = 0; i < count; ++i) {
        double excess = depth[i] - ds[i];
        if (excess > 0.0) {
            // pow(x, 5/3) = x * pow(x, 2/3) = x * cbrt(x*x)
            runoff_rate[i] = excess * std::pow(excess, 2.0 / 3.0);
        } else {
            runoff_rate[i] = 0.0;
        }
    }

    // Batch multiply: runoff_rate[i] *= alpha[i]
    // simd::multiply reads from two source arrays and writes to dst;
    // we use runoff_rate as both source and destination via a temp buffer
    // only if count is large enough to warrant it. For simplicity and
    // correctness (no aliasing), use the in-place ivdep loop above and
    // then apply the alpha scaling via simd::multiply.
    // Since simd::multiply requires separate src and dst (no alias),
    // we apply alpha in-place with a vectorisation hint.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC ivdep
#endif
    for (int i = 0; i < count; ++i) {
        runoff_rate[i] *= alpha[i];
    }
}

// ============================================================================
// Batch depth update — VECTORISABLE
// ============================================================================

void RunoffSolver::batchDepthUpdate(
    double*       __restrict__ depth,
    const double* __restrict__ precip,
    const double* __restrict__ evap,
    const double* __restrict__ infil,
    const double* __restrict__ runoff,
    double dt,
    int count
) {
    auto un = static_cast<std::size_t>(count);

    // Vectorisable depth increment: depth[i] += net_flux * dt
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC ivdep
#endif
    for (int i = 0; i < count; ++i) {
        depth[i] += (precip[i] - evap[i] - infil[i] - runoff[i]) * dt;
    }

    // Batch clamp depths to [0, +inf) — negative depths are non-physical.
    // Using a very large upper bound since there is no maximum depth constraint here.
    openswmm::simd::clamp(depth, 0.0, 1.0e30, un);
}

// ============================================================================
// Init
// ============================================================================

void RunoffSolver::init(SimulationContext& ctx) {
    int n = ctx.n_subcatches();
    soa_.resize(n);

    // Populate SoA from ctx.subcatches
    for (int i = 0; i < n; ++i) {
        auto ui = static_cast<std::size_t>(i);
        soa_.area[ui]       = ctx.subcatches.area[ui];
        soa_.width[ui]      = ctx.subcatches.width[ui];
        soa_.slope[ui]      = ctx.subcatches.slope[ui];
        soa_.imperv_pct[ui] = ctx.subcatches.frac_imperv[ui];
        soa_.n_imperv[ui]   = ctx.subcatches.n_imperv[ui];
        soa_.n_perv[ui]     = ctx.subcatches.n_perv[ui];
        soa_.ds_imperv[ui]  = ctx.subcatches.ds_imperv[ui];
        soa_.ds_perv[ui]    = ctx.subcatches.ds_perv[ui];
    }
    soa_.computeAlpha();

    // Initialise infiltration states from ctx per-subcatch params
    horton_states_.resize(static_cast<std::size_t>(n));
    grnampt_states_.resize(static_cast<std::size_t>(n));
    curvenum_states_.resize(static_cast<std::size_t>(n));

    for (int i = 0; i < n; ++i) {
        auto ui = static_cast<std::size_t>(i);
        int im = ctx.subcatches.infil_model[ui];
        switch (im) {
            case 0: case 1: // HORTON / MOD_HORTON
                infil_model_ = InfilModel::HORTON;
                infil::horton_init(horton_states_[ui],
                    ctx.subcatches.infil_p1[ui], ctx.subcatches.infil_p2[ui],
                    ctx.subcatches.infil_p3[ui], ctx.subcatches.infil_p4[ui],
                    ctx.subcatches.infil_p5[ui], ctx.options);
                break;
            case 2: case 3: // GREEN_AMPT / MOD_GREEN_AMPT
                infil_model_ = InfilModel::GREEN_AMPT;
                infil::grnampt_init(grnampt_states_[ui],
                    ctx.subcatches.infil_p1[ui], ctx.subcatches.infil_p2[ui],
                    ctx.subcatches.infil_p3[ui], ctx.options);
                break;
            case 4: // CURVE_NUMBER
                infil_model_ = InfilModel::CURVE_NUM;
                infil::curvenum_init(curvenum_states_[ui],
                    ctx.subcatches.infil_p1[ui], ctx.subcatches.infil_p4[ui]);
                break;
        }
    }
}

// ============================================================================
// Execute — one runoff timestep for ALL subcatchments
// ============================================================================

void RunoffSolver::execute(SimulationContext& ctx, double dt) {
    int n = soa_.n_subcatch;
    if (n == 0) return;

    auto un = static_cast<std::size_t>(n);
    precip_.resize(un);
    evap_rate_.resize(un);
    infil_rate_.resize(un);

    // ----- Step 1: Get rainfall from gages → net precip per subcatchment -----
    // Gage rainfall is in project rain units (in/hr for US).
    // Convert to ft/sec for the runoff equations.
    // Legacy: rainfall (in/hr) * 1/12 (ft/in) * 1/3600 (hr/sec) = ft/sec
    for (int i = 0; i < n; ++i) {
        auto ui = static_cast<std::size_t>(i);
        int gi = ctx.subcatches.gage[ui];
        double rain_inhr = 0.0;
        if (gi >= 0 && gi < ctx.n_gages()) {
            rain_inhr = ctx.gages.rainfall[static_cast<std::size_t>(gi)];
        }
        // Convert in/hr → ft/sec
        precip_[ui] = rain_inhr / 12.0 / 3600.0;
        // Write rainfall to subcatchment (for reporting, in project rain units)
        ctx.subcatches.rainfall[ui] = rain_inhr;
    }

    // ----- Step 2: Evaporation rate (from climate module) -----
    // For now use a simple approach; full climate wiring done in SWMMEngine::step()
    std::fill(evap_rate_.begin(), evap_rate_.end(), 0.0);

    // ----- Step 3: Batch infiltration (pervious subareas) -----
    // Per-subcatchment infiltration state is independent — parallelisable.
    // Each subcatchment owns its own Horton/Green-Ampt/CurveNum state.
#if defined(SWMM_USE_OPENMP)
#pragma omp parallel for schedule(static)
#endif
    for (int i = 0; i < n; ++i) {
        auto ui = static_cast<std::size_t>(i);
        double p = precip_[ui];
        double d = soa_.depth_perv[ui];

        switch (infil_model_) {
            case InfilModel::HORTON:
                infil_rate_[ui] = infil::horton_getInfil(horton_states_[ui], p, d, dt);
                break;
            case InfilModel::GREEN_AMPT:
                infil_rate_[ui] = infil::grnampt_getInfil(grnampt_states_[ui], p, d, dt);
                break;
            case InfilModel::CURVE_NUM:
                infil_rate_[ui] = infil::curvenum_getInfil(curvenum_states_[ui], p, d, dt);
                break;
        }
    }

    // ----- Step 4: Batch nonlinear reservoir routing -----

    // Impervious subareas — no infiltration
    std::vector<double> runoff_imperv(un);
    std::vector<double> zero_infil(un, 0.0);
    batchNonlinearReservoir(soa_.alpha_imperv.data(), soa_.depth_imperv.data(),
                            soa_.ds_imperv.data(), runoff_imperv.data(), n);
    batchDepthUpdate(soa_.depth_imperv.data(), precip_.data(), evap_rate_.data(),
                     zero_infil.data(), runoff_imperv.data(), dt, n);

    // Pervious subareas — with infiltration
    std::vector<double> runoff_perv(un);
    batchNonlinearReservoir(soa_.alpha_perv.data(), soa_.depth_perv.data(),
                            soa_.ds_perv.data(), runoff_perv.data(), n);
    batchDepthUpdate(soa_.depth_perv.data(), precip_.data(), evap_rate_.data(),
                     infil_rate_.data(), runoff_perv.data(), dt, n);

    // ----- Step 5: Total runoff per subcatchment -----
    for (int i = 0; i < n; ++i) {
        auto ui = static_cast<std::size_t>(i);
        double f_imperv = soa_.imperv_pct[ui];
        double f_perv = 1.0 - f_imperv;

        // Runoff rate (ft/sec * area = cfs)
        double q_imperv = runoff_imperv[ui] * soa_.area[ui] * f_imperv;
        double q_perv   = runoff_perv[ui]   * soa_.area[ui] * f_perv;
        soa_.runoff[ui] = q_imperv + q_perv;

        // Mass balance accumulators
        soa_.evap_loss[ui]  = evap_rate_[ui] * soa_.area[ui] * dt;
        soa_.infil_loss[ui] = infil_rate_[ui] * soa_.area[ui] * f_perv * dt;

        // Write back to SimulationContext SoA
        ctx.subcatches.runoff[ui]     = soa_.runoff[ui];
        ctx.subcatches.evap_loss[ui]  = evap_rate_[ui];   // rate (ft/sec)
        ctx.subcatches.infil_loss[ui] = infil_rate_[ui];  // rate (ft/sec)

        // Route runoff to outlet node
        int out_node = ctx.subcatches.outlet_node[ui];
        if (out_node >= 0 && out_node < ctx.n_nodes()) {
            ctx.nodes.lat_flow[static_cast<std::size_t>(out_node)] += soa_.runoff[ui];
        }

        // Mass balance is accumulated in SWMMEngine::step() — not here
        // to avoid double counting.
    }
}

} // namespace runoff
} // namespace openswmm
