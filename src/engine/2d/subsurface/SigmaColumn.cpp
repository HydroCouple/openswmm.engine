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
 * @file SigmaColumn.cpp
 * @brief G-steps 4 and 5 — see the header for the coordinate, the flux forms
 *        and the handover convention.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "SigmaColumn.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace openswmm::twoD::sigma {

namespace {

constexpr double kLMin  = 1.0e-3;   ///< column thickness floor (m)
constexpr double kTiny  = 1.0e-300;

inline double& at(double* t, int j, int stride) noexcept {
    return t[static_cast<std::size_t>(j) * static_cast<std::size_t>(stride)];
}
inline double at(const double* t, int j, int stride) noexcept {
    return t[static_cast<std::size_t>(j) * static_cast<std::size_t>(stride)];
}

/// Harmonic mean, zero when either side is zero (the conductive-series rule).
inline double harmonic(double a, double b) noexcept {
    const double s = a + b;
    return (s > kTiny) ? (2.0 * a * b / s) : 0.0;
}

}  // namespace

// ---------------------------------------------------------------------------

double columnStorage(const double* theta, int m, int stride, double L) noexcept {
    const double dz = std::max(L, kLMin) / static_cast<double>(m);
    double s = 0.0;
    for (int j = 0; j < m; ++j) s += at(theta, j, stride) * dz;
    return s;
}

void seedHydrostatic(const soil::Params& p, double* theta, int m, int stride,
                     double L) noexcept {
    const double Lc = std::max(L, kLMin);
    const double dsig = 1.0 / static_cast<double>(m);
    for (int j = 0; j < m; ++j) {
        // Layer centre: σ = (j + ½)/m, and ψ is the height ABOVE the table,
        // so ψ = L·(1 − σ). Layer 0 is at the surface (largest suction).
        const double sig = (static_cast<double>(j) + 0.5) * dsig;
        const double psi = Lc * (1.0 - sig);
        at(theta, j, stride) = soil::waterContent(p, psi);
    }
}

double columnDtLimit(const soil::Params& p, const double* theta, int m,
                     int stride, double L, double Ldot, double c_col,
                     bool capillary) noexcept {
    const double Lc = std::max(L, kLMin);
    const double dsig = 1.0 / static_cast<double>(m);
    const double dz = Lc * dsig;
    double lim = 1.0e30;
    for (int j = 0; j < m; ++j) {
        const double th = std::clamp(at(theta, j, stride), p.theta_r, p.theta_s);
        // Celerity c(θ) = dK/dθ, by a centred difference through the
        // retention inverse: the analytic form differs per law and per
        // branch, and this runs once per rebuild, not per firing.
        const double span = std::max(p.theta_s - p.theta_r, 1.0e-9);
        const double se   = std::clamp((th - p.theta_r) / span, 1.0e-8, 1.0);
        const double dse  = 1.0e-3;
        const double se_hi = std::min(se + dse, 1.0);
        const double se_lo = std::max(se - dse, 1.0e-8);
        const double k_hi = soil::conductivity(p, soil::suctionAtSaturation(p, se_hi));
        const double k_lo = soil::conductivity(p, soil::suctionAtSaturation(p, se_lo));
        const double c = std::fabs(k_hi - k_lo) /
                         std::max((se_hi - se_lo) * span, 1.0e-12);

        const double sig = (static_cast<double>(j) + 0.5) * dsig;
        const double grid = std::fabs(sig * Ldot);
        double diff = 0.0;
        if (capillary) {
            const double psi = soil::suctionAtSaturation(p, se);
            diff = 2.0 * soil::diffusivity(p, psi) / std::max(dz, 1.0e-12);
        }
        const double denom = c + grid + diff;
        if (denom > kTiny) lim = std::min(lim, dz / denom);
    }
    return std::max(c_col, 1.0e-6) * lim;
}

// ---------------------------------------------------------------------------

void advanceColumn(const soil::Params& p, double* theta, int m, int stride,
                   ColumnStep& st) noexcept {
    if (m <= 0 || !(st.dt > 0.0)) return;

    const double dsig = 1.0 / static_cast<double>(m);
    const double L0   = std::max(st.L_old, kLMin);
    const double L1   = std::max(st.L_new, kLMin);
    const double Ldot = (L1 - L0) / st.dt;

    st.dt_limit = columnDtLimit(p, theta, m, stride, L0, Ldot, 1.0, st.capillary);

    // ---- 1. w_j from the OLD geometry. w is the conserved quantity; θ is a
    //         derived view of it, and it is re-derived from the NEW L at the
    //         end. That ordering is the whole ALE bookkeeping.
    static thread_local std::vector<double> w, w_new, th_old, f;
    w.assign(static_cast<std::size_t>(m), 0.0);
    th_old.assign(static_cast<std::size_t>(m), 0.0);
    f.assign(static_cast<std::size_t>(m) + 1, 0.0);
    const double dz0 = L0 * dsig;
    for (int j = 0; j < m; ++j) {
        th_old[static_cast<std::size_t>(j)] =
            std::clamp(at(theta, j, stride), p.theta_r, p.theta_s);
        w[static_cast<std::size_t>(j)] = th_old[static_cast<std::size_t>(j)] * dz0;
    }

    auto psiOf = [&](double th) {
        const double span = std::max(p.theta_s - p.theta_r, 1.0e-9);
        const double se = std::clamp((th - p.theta_r) / span, 1.0e-8, 1.0);
        return soil::suctionAtSaturation(p, se);
    };

    // ---- 2. Interior faces. f[j] is the face ABOVE layer j (σ = j/m), so
    //         f[0] is the top boundary and f[m] the bottom.
    for (int jf = 1; jf < m; ++jf) {
        const int up = jf - 1;              // layer above the face
        const int dn = jf;                  // layer below
        const double th_up = th_old[static_cast<std::size_t>(up)];
        const double th_dn = th_old[static_cast<std::size_t>(dn)];
        const double sig_f = static_cast<double>(jf) * dsig;

        // (a) gravity — downward, donor is the layer above.
        double flux = soil::conductivity(p, psiOf(th_up));

        // (b) grid motion, upwinded on its own sign. The face moves at
        //     ż = σ_f·L̇; the relative flux is −θ_donor·ż, so L̇ > 0 (faces
        //     sweeping down into the column) takes the layer BELOW.
        const double zdot = sig_f * Ldot;
        const double th_grid = (zdot > 0.0) ? th_dn : th_up;
        flux -= th_grid * zdot;

        // (c) capillary diffusion, harmonic-mean D̄ across the face.
        if (st.capillary) {
            const double d_up = soil::diffusivity(p, psiOf(th_up));
            const double d_dn = soil::diffusivity(p, psiOf(th_dn));
            const double dbar = harmonic(d_up, d_dn);
            flux -= dbar * (th_dn - th_up) / std::max(L0 * dsig, 1.0e-12);
        }
        f[static_cast<std::size_t>(jf)] = flux;
    }

    // ---- 3. Top boundary: infiltration in, ET out, with acceptance.
    //         The top layer can hold at most (θ_s − θ_top)·dz more water;
    //         anything beyond that is rejected to the surface.
    {
        const double th_top = th_old[0];
        const double headroom = std::max(p.theta_s - th_top, 0.0) * dz0 / st.dt;
        const double accept = std::min(std::max(st.q_in, 0.0), headroom);
        st.f_top    = accept;
        st.rejected = std::max(st.q_in, 0.0) - accept;

        // ET is limited by what the top layer holds above residual, and by
        // the Feddes stress at its own suction — no `if (infil <= 0)` gate.
        const double avail = std::max(th_top - p.theta_r, 0.0) * dz0 / st.dt;
        const double psi_top = psiOf(th_top);
        // Wilting suction from the law's own residual end: the suction at
        // 1 % effective saturation is a stable, law-agnostic proxy.
        const double psi_w = soil::suctionAtSaturation(p, 0.01);
        const double stress = soil::feddesStress(psi_top, psi_w);
        st.et_taken = std::min(std::max(st.q_et, 0.0) * stress, avail);
        f[0] = st.f_top - st.et_taken;
    }

    // ---- 4. Bottom boundary: physical Darcy across the table PLUS the
    //         handover swept by the moving boundary (σ = 1, ż = L̇).
    {
        const double th_bot = th_old[static_cast<std::size_t>(m - 1)];
        st.f_bot = st.q0_phys - th_bot * Ldot;
        f[static_cast<std::size_t>(m)] = st.f_bot;
    }

    // ---- 5. Positivity share. A layer may not export more than it holds
    //         above residual over this step; when its outgoing faces
    //         over-subscribe it they are scaled together, exactly like the
    //         surface solver's volume share. Applied to interior faces and
    //         the bottom face (the top face is already capped above).
    for (int j = 0; j < m; ++j) {
        const double avail = std::max(w[static_cast<std::size_t>(j)] -
                                      p.theta_r * dz0, 0.0);
        double out = 0.0;
        if (f[static_cast<std::size_t>(j)] < 0.0)
            out += -f[static_cast<std::size_t>(j)] * st.dt;      // upward out of j
        if (f[static_cast<std::size_t>(j + 1)] > 0.0)
            out += f[static_cast<std::size_t>(j + 1)] * st.dt;   // downward out of j
        if (out > avail && out > kTiny) {
            const double beta = avail / out;
            if (f[static_cast<std::size_t>(j)] < 0.0)
                f[static_cast<std::size_t>(j)] *= beta;
            if (f[static_cast<std::size_t>(j + 1)] > 0.0) {
                f[static_cast<std::size_t>(j + 1)] *= beta;
                if (j == m - 1) st.f_bot = f[static_cast<std::size_t>(m)];
            }
        }
    }

    // ---- 6. Update w, then re-derive θ from the NEW geometry. This is where
    //         the layers compress: same water, new dz.
    //
    //   A rising table (L shrinking) compresses every layer. A layer whose
    //   water then exceeds θ_s·dz_new is SATURATED, and the surplus belongs
    //   to the saturated zone — it must not be clamped away. Clamping it was
    //   the first version of this function and it lost 3.9e-1 m of water on
    //   the closed-column oscillation gate: the classic σ-grid failure the
    //   plan names in its risks. So the surplus cascades DOWNWARD layer by
    //   layer (each full layer pushes into the one below) and whatever
    //   reaches the bottom is reported as `overflow_to_sat`, which the
    //   solver adds to the saturated zone in this same firing. A deficit
    //   below residual is drawn the same way, reported as
    //   `deficit_from_sat`. With both routed, Σw is conserved exactly and
    //   the gate passes to machine precision.
    const double dz1 = L1 * dsig;
    w_new.assign(static_cast<std::size_t>(m), 0.0);
    for (int j = 0; j < m; ++j) {
        w_new[static_cast<std::size_t>(j)] =
            w[static_cast<std::size_t>(j)] +
            st.dt * (f[static_cast<std::size_t>(j)] -
                     f[static_cast<std::size_t>(j + 1)]);
    }

    const double w_max = p.theta_s * dz1;
    const double w_min = p.theta_r * dz1;

    // Cascade surplus downward; the bottom layer's surplus leaves the column.
    double carry = 0.0;
    for (int j = 0; j < m; ++j) {
        double wj = w_new[static_cast<std::size_t>(j)] + carry;
        carry = 0.0;
        if (wj > w_max) { carry = wj - w_max; wj = w_max; }
        w_new[static_cast<std::size_t>(j)] = wj;
    }
    st.overflow_to_sat = carry / st.dt;      // m/s down across the table

    // Draw any residual deficit upward from the table (capillary support).
    double draw = 0.0;
    for (int j = m - 1; j >= 0; --j) {
        double wj = w_new[static_cast<std::size_t>(j)] - draw;
        draw = 0.0;
        if (wj < w_min) { draw = w_min - wj; wj = w_min; }
        w_new[static_cast<std::size_t>(j)] = wj;
    }
    st.deficit_from_sat = draw / st.dt;      // m/s up across the table

    for (int j = 0; j < m; ++j)
        at(theta, j, stride) =
            w_new[static_cast<std::size_t>(j)] / std::max(dz1, 1.0e-12);
    st.theta_bot = at(theta, m - 1, stride);
}

}  // namespace openswmm::twoD::sigma
