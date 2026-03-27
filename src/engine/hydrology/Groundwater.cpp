/**
 * @file Groundwater.cpp
 * @brief Two-zone groundwater — batch-oriented, vectorisable kernels.
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "Groundwater.hpp"
#include "../math/SIMD.hpp"
#include <cmath>
#include <algorithm>

namespace openswmm {
namespace groundwater {

void GWSoA::resize(int n) {
    n_subcatch = n;
    auto un = static_cast<std::size_t>(n);

    porosity.assign(un, 0.4);
    field_cap.assign(un, 0.2);
    wilt_point.assign(un, 0.1);
    k_sat.assign(un, 0.0);
    k_slope.assign(un, 0.0);
    tension_slope.assign(un, 0.0);
    upper_evap_frac.assign(un, 0.0);
    lower_evap_depth.assign(un, 0.0);
    lower_loss_coeff.assign(un, 0.0);
    total_depth.assign(un, 0.0);

    a1.assign(un, 0.0); b1.assign(un, 0.0);
    a2.assign(un, 0.0); b2.assign(un, 0.0);
    a3.assign(un, 0.0);
    h_star.assign(un, 0.0);

    theta.assign(un, 0.2);
    lower_depth.assign(un, 0.0);

    gw_flow.assign(un, 0.0);
    upper_evap.assign(un, 0.0);
    lower_evap.assign(un, 0.0);
    deep_loss.assign(un, 0.0);
}

void GWSolver::init(int n_subcatch) {
    soa_.resize(n_subcatch);
}

// ============================================================================
// Batch upper zone percolation — VECTORISABLE
// ============================================================================

void GWSolver::batchUpperPerc(
    const double* OPENSWMM_RESTRICT theta,
    const double* OPENSWMM_RESTRICT field_cap,
    const double* OPENSWMM_RESTRICT k_sat,
    const double* OPENSWMM_RESTRICT k_slope,
    double*       OPENSWMM_RESTRICT perc,
    int count
) {
    // Unsaturated conductivity: K = Ks * exp(k_slope * (θ - porosity))
    // Percolation ~ K (simplified Darcy flux)
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC ivdep
#endif
    for (int i = 0; i < count; ++i) {
        double delta = theta[i] - field_cap[i];
        if (delta <= 0.0) {
            perc[i] = 0.0;
        } else {
            perc[i] = k_sat[i] * std::exp(k_slope[i] * delta);
        }
    }
}

// ============================================================================
// Batch lateral GW flow — VECTORISABLE
// ============================================================================

void GWSolver::batchGWFlow(
    const double* OPENSWMM_RESTRICT lower_depth,
    const double* OPENSWMM_RESTRICT h_star,
    const double* OPENSWMM_RESTRICT a1, const double* OPENSWMM_RESTRICT b1,
    const double* OPENSWMM_RESTRICT a2, const double* OPENSWMM_RESTRICT b2,
    const double* OPENSWMM_RESTRICT a3,
    const double* OPENSWMM_RESTRICT sw_head,
    double*       OPENSWMM_RESTRICT gw_flow,
    int count
) {
    // Q = a1*(H-H*)^b1 - a2*(Hsw-H*)^b2 + a3*H*Hsw
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC ivdep
#endif
    for (int i = 0; i < count; ++i) {
        double H = lower_depth[i];
        double hs = h_star[i];
        double Hsw = sw_head[i];

        if (H <= hs) {
            gw_flow[i] = 0.0;
            continue;
        }

        double t1 = (b1[i] == 0.0) ? a1[i] : a1[i] * std::pow(H - hs, b1[i]);

        double t2 = 0.0;
        if (Hsw > hs) {
            t2 = (b2[i] == 0.0) ? a2[i] : a2[i] * std::pow(Hsw - hs, b2[i]);
        }

        double t3 = a3[i] * H * Hsw;

        double q = t1 - t2 + t3;
        if (q < 0.0 && a3[i] != 0.0) q = 0.0;
        gw_flow[i] = q;
    }
}

// ============================================================================
// Execute — all subcatchments batch
// ============================================================================

void GWSolver::execute(SimulationContext& /*ctx*/, double dt, double max_evap,
                       const double* infil_rate, const double* sw_head) {
    int n = soa_.n_subcatch;
    if (n == 0) return;

    std::vector<double> perc(static_cast<std::size_t>(n));

    // 1. Batch upper zone percolation
    batchUpperPerc(soa_.theta.data(), soa_.field_cap.data(),
                   soa_.k_sat.data(), soa_.k_slope.data(),
                   perc.data(), n);

    // 2. Batch lateral GW flow
    batchGWFlow(soa_.lower_depth.data(), soa_.h_star.data(),
                soa_.a1.data(), soa_.b1.data(),
                soa_.a2.data(), soa_.b2.data(),
                soa_.a3.data(), sw_head,
                soa_.gw_flow.data(), n);

    // 3. Batch deep percolation
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC ivdep
#endif
    for (int i = 0; i < n; ++i) {
        auto ui = static_cast<std::size_t>(i);
        double td = soa_.total_depth[ui];
        soa_.deep_loss[ui] = (td > 0.0)
            ? soa_.lower_loss_coeff[ui] * (soa_.lower_depth[ui] / td)
            : 0.0;
    }

    // 4. Batch ET from upper/lower zones
    for (int i = 0; i < n; ++i) {
        auto ui = static_cast<std::size_t>(i);
        double avail_evap = max_evap;

        // Upper zone evap
        if (soa_.theta[ui] > soa_.wilt_point[ui]) {
            soa_.upper_evap[ui] = soa_.upper_evap_frac[ui] * max_evap;
            soa_.upper_evap[ui] = std::min(soa_.upper_evap[ui], avail_evap);
        } else {
            soa_.upper_evap[ui] = 0.0;
        }
        avail_evap -= soa_.upper_evap[ui];

        // Lower zone evap
        if (soa_.lower_evap_depth[ui] > 0.0) {
            double upper_d = soa_.total_depth[ui] - soa_.lower_depth[ui];
            double frac = std::max(0.0,
                (soa_.lower_evap_depth[ui] - upper_d) / soa_.lower_evap_depth[ui]);
            frac = std::min(frac, 1.0);
            soa_.lower_evap[ui] = frac * (1.0 - soa_.upper_evap_frac[ui]) * max_evap;
            soa_.lower_evap[ui] = std::min(soa_.lower_evap[ui], avail_evap);
        } else {
            soa_.lower_evap[ui] = 0.0;
        }
    }

    // 5. Euler step — batch update state variables
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC ivdep
#endif
    for (int i = 0; i < n; ++i) {
        auto ui = static_cast<std::size_t>(i);
        double upper_depth = soa_.total_depth[ui] - soa_.lower_depth[ui];
        if (upper_depth <= 0.0) upper_depth = 0.001;

        // dθ/dt = (infil - upper_evap - perc) / upper_depth
        double dtheta = (infil_rate[i] - soa_.upper_evap[ui] - perc[ui]) / upper_depth;
        soa_.theta[ui] += dtheta * dt;

        // dH/dt = (perc - deep_loss - lower_evap - gw_flow) / (φ - θ)
        double stor = soa_.porosity[ui] - soa_.theta[ui];
        if (stor <= 0.001) stor = 0.001;
        double dH = (perc[ui] - soa_.deep_loss[ui] - soa_.lower_evap[ui] - soa_.gw_flow[ui]) / stor;
        soa_.lower_depth[ui] += dH * dt;
    }

    // Batch clamp theta to [wilt_point, porosity] and lower_depth to [0, total_depth].
    // Per-element clamp bounds differ, so we use a vectorised loop with ivdep.
    auto un = static_cast<std::size_t>(n);
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC ivdep
#endif
    for (std::size_t ui = 0; ui < un; ++ui) {
        soa_.theta[ui] = std::max(soa_.theta[ui], soa_.wilt_point[ui]);
        soa_.theta[ui] = std::min(soa_.theta[ui], soa_.porosity[ui]);
        soa_.lower_depth[ui] = std::max(soa_.lower_depth[ui], 0.0);
        soa_.lower_depth[ui] = std::min(soa_.lower_depth[ui], soa_.total_depth[ui]);
    }
}

} // namespace groundwater
} // namespace openswmm
