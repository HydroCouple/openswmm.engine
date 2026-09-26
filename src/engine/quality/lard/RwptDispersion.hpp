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
 * @file RwptDispersion.hpp
 * @brief X3b: RWPT longitudinal dispersion on LARD segments.
 *
 * @details Subplan X3b (strategy §5, §2.5; §16 D-L4/D-L6). Longitudinal
 *          dispersion emerges from resolved VERTICAL shear plus a vertical
 *          random walk — the Taylor/Elder mechanism — rather than from a
 *          fitted D_L coefficient (that deterministic alternative is the
 *          ARD engine's E3 machinery; the user chose RWPT for LARD).
 *
 *          **D-X3b1 — particles carry no mass; they estimate the
 *          inter-segment dispersive exchange.** Persistent per-particle
 *          state is (ζ, η): ζ ∈ [0, V] the volume coordinate from the
 *          link's FRONT (upstream) boundary — volume coordinates make the
 *          substep's bulk advection an exact uniform shift (ζ += V_in) —
 *          and η ∈ (0,1) the normalized depth, which holds the velocity
 *          memory that shear dispersion correlates against position.
 *          Each substep, after the segment field is final (post-RELEASE):
 *
 *            1. ζ += V_in (ride the water); particles pushed past V are
 *               water that left — recycled as fresh entries near the
 *               front with a fresh η (deterministic draw).
 *            2. Shear + walk: ζ += (u(η) − ū)·dt·A ; η does a reflected
 *               random walk with the Itô drift term dD_η/dη·dt (omitting
 *               the drift piles particles into low-diffusivity zones —
 *               the standard RWPT defect).
 *            3. Every boundary crossing carries δm_s = C_s(ORIGIN
 *               segment)·V/N of each species across — an upwind-carried
 *               Monte-Carlo estimate of the dispersive flux. A uniform
 *               field exchanges symmetrically and does not move (no
 *               spurious drift); total mass is conserved EXACTLY by
 *               construction (every debit has a matching credit).
 *
 *          Profiles (v1, wide-channel forms, documented deviations):
 *          - Turbulent (Re ≥ 2000): log-law deviation
 *            u(η) − ū = (u_star/κ)(1 + ln η), κ = 0.41; D_t = κ·u_star·h·η(1−η)
 *            (Rouse parabola). u* = √(g·R_h·S_f), S_f from Manning at the
 *            link's own n, R_h exact for CIRCULAR, ≈ h otherwise.
 *            The emergent longitudinal coefficient's reference is
 *            **Elder, D_L = 5.93·u*·h** — vertical-shear-only, exactly
 *            what is resolved here.
 *          - Laminar (Re < 2000): parabolic u(η) = 1.5·ū·η(2−η) deviation,
 *            D_t = D_m (molecular, 1.25e-9 m²/s → 1.3454e-8 ft²/s).
 *
 *          **D-L6 counter RNG**: draws are a pure function of
 *          (seed, link, global substep counter, particle, draw#) via a
 *          splitmix64-style hash — bit-reproducible under any threading,
 *          schedule, or run repetition; RWPT_SEED changes every draw.
 *
 *          NOT here (recorded): inter-LINK particle dispersion (ζ reflects
 *          at both link ends; segment exchange stops at the boundary
 *          node's CSTR, which already mixes), transverse-shear dispersion
 *          (Fischer's field-scale term — a calibrated-D machinery, not a
 *          resolved one), particle-count adaptivity, wall species.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_QUALITY_LARD_RWPT_DISPERSION_HPP
#define OPENSWMM_QUALITY_LARD_RWPT_DISPERSION_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "../../core/SimulationContext.hpp"
#include "SegmentStore.hpp"

namespace openswmm {
namespace lard {

constexpr int    kRwptParticlesPerLink = 2000;
constexpr double kKappa   = 0.41;        ///< von Kármán
constexpr double kNuWater = 1.05e-5;     ///< kinematic viscosity, ft²/s
constexpr double kDm      = 1.3454e-8;   ///< molecular diffusivity, ft²/s
constexpr double kEtaMin  = 1.0e-3;      ///< log-law floor
constexpr double kReTurb  = 2000.0;

// ---------------------------------------------------------------------------
// D-L6 counter-based RNG: pure function of the key, no state anywhere.
// ---------------------------------------------------------------------------
inline std::uint64_t rwpt_hash(std::uint64_t z) {
    z += 0x9e3779b97f4a7c15ULL;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}
/// Uniform in (0,1), keyed. `draw` distinguishes multiple draws per particle.
inline double rwpt_uniform(std::uint64_t seed, std::uint64_t link,
                           std::uint64_t stepc, std::uint64_t particle,
                           std::uint64_t draw) {
    std::uint64_t k = rwpt_hash(seed ^ rwpt_hash(link));
    k = rwpt_hash(k ^ rwpt_hash(stepc));
    k = rwpt_hash(k ^ rwpt_hash(particle * 8ULL + draw));
    // 53-bit mantissa, strictly inside (0,1).
    return (static_cast<double>(k >> 11) + 0.5) * (1.0 / 9007199254740992.0);
}
/// Standard normal (Box–Muller on two keyed uniforms).
inline double rwpt_normal(std::uint64_t seed, std::uint64_t link,
                          std::uint64_t stepc, std::uint64_t particle,
                          std::uint64_t draw) {
    const double u1 = rwpt_uniform(seed, link, stepc, particle, draw);
    const double u2 = rwpt_uniform(seed, link, stepc, particle, draw + 1);
    return std::sqrt(-2.0 * std::log(u1)) *
           std::cos(6.283185307179586 * u2);
}

// ---------------------------------------------------------------------------
// Profile kernels — exposed free functions so the unit gates can pin the
// math without an engine run.
// ---------------------------------------------------------------------------
/// Velocity DEVIATION u(η) − ū. Turbulent: log-law; laminar: parabola.
inline double rwpt_u_dev(double eta, double ubar, double ustar,
                         bool turbulent) {
    // The kEtaMin floor belongs to the LOG-LAW only (ln 0 = -inf); clamping
    // the laminar parabola too biased its mean by ~1.5e-6*ubar — gate 4's
    // mean-free quadrature caught it (X3b round).
    if (turbulent)
        return (ustar / kKappa) * (1.0 + std::log(std::max(eta, kEtaMin)));
    return 1.5 * ubar * eta * (2.0 - eta) - ubar;  // parabola, mean-free
}
/// Vertical diffusivity in η-units (D_t/h²), and its η-gradient (the Itô
/// drift). Turbulent: Rouse parabola κ·u*·h·η(1−η); laminar: molecular.
inline double rwpt_d_eta(double eta, double h, double ustar, bool turbulent) {
    if (!turbulent) return kDm / (h * h);
    const double e = std::min(std::max(eta, 0.0), 1.0);
    return kKappa * ustar * e * (1.0 - e) / h;  // (κ u* h η(1-η))/h²
}
inline double rwpt_d_eta_grad(double eta, double h, double ustar,
                              bool turbulent) {
    if (!turbulent) return 0.0;
    return kKappa * ustar * (1.0 - 2.0 * eta) / h;
}

/// Hydraulic radius: exact for CIRCULAR from flow area, ≈ depth otherwise
/// (wide-channel v1 approximation, recorded).
inline double rwpt_hyd_radius(double area, double depth, double diam,
                              bool circular) {
    if (!circular || diam <= 0.0 || depth <= 0.0) return std::max(depth, 0.0);
    const double ratio =
        std::min(std::max(1.0 - 2.0 * depth / diam, -1.0), 1.0);
    const double theta = 2.0 * std::acos(ratio);
    if (theta <= 1.0e-6) return std::max(depth, 0.0);
    const double a = diam * diam / 8.0 * (theta - std::sin(theta));
    const double p = diam * theta / 2.0;
    (void)area;
    return (p > 0.0) ? a / p : std::max(depth, 0.0);
}

/**
 * @brief Persistent particle field + the per-substep exchange operator.
 */
class RwptDispersion {
public:
    void resize(int n_links, int n_per_link = kRwptParticlesPerLink) {
        nl_ = n_links;
        npart_ = n_per_link;
        const std::size_t total =
            static_cast<std::size_t>(nl_) * static_cast<std::size_t>(npart_);
        zeta_.assign(total, -1.0);  // <0 marks "not yet seeded"
        eta_.assign(total, 0.5);
        seg_idx_.assign(total, 0);
    }

    /**
     * @brief One substep of dispersive exchange on link `l`'s segments.
     *
     * @param v_in     the volume that entered at the FRONT this substep
     *                 (bulk-advection shift in volume coordinates).
     * @param stepc    global substep counter (RNG key component).
     * @param delta    scratch, ns entries — reused across calls.
     */
    void disperse(SimulationContext& ctx, SegmentStore& segs, int l,
                  double ubar, double h, double rh, double n_manning,
                  double v_in, double q_dt, double dt, std::uint64_t stepc,
                  std::uint64_t seed, std::vector<double>& delta) {
        (void)ctx;
        const int nseg = segs.count(l);
        if (nseg < 2 || h <= 1.0e-3 || ubar <= 0.0) return;
        const double vol = segs.total_volume(l);
        if (vol <= 0.0) return;
        const int ns = segs.species();

        // Friction: Manning S_f at the link's own n → u*. Regime by Re.
        const double sf = (n_manning * ubar) * (n_manning * ubar) /
                          (2.208 * std::pow(std::max(rh, 1.0e-3), 4.0 / 3.0));
        const double ustar =
            std::sqrt(32.174 * std::max(rh, 1.0e-3) * sf);
        const bool turbulent = (ubar * 4.0 * rh / kNuWater) >= kReTurb;

        // Segment boundaries in volume coordinates, front → back.
        bounds_.assign(static_cast<std::size_t>(nseg) + 1, 0.0);
        for (int j = 0; j < nseg; ++j)
            bounds_[static_cast<std::size_t>(j) + 1] =
                bounds_[static_cast<std::size_t>(j)] + segs.seg_volume(l, j);

        const std::size_t base = static_cast<std::size_t>(l) *
                                 static_cast<std::size_t>(npart_);
        for (int p = 0; p < npart_; ++p) {
            const std::size_t ip = base + static_cast<std::size_t>(p);
            double z = zeta_[ip];
            double e = eta_[ip];

            // Seed / recycle: unseeded particles spread evenly; particles
            // that rode out the back are new water entering at the front.
            if (z < 0.0) {
                z = vol * (static_cast<double>(p) + 0.5) /
                    static_cast<double>(npart_);
                e = rwpt_uniform(seed, static_cast<std::uint64_t>(l), stepc,
                                 static_cast<std::uint64_t>(p), 7);
            } else {
                z += v_in;  // ride the bulk advection (volume coordinates)
                if (z >= vol) {
                    z = std::min(v_in,
                                 vol * rwpt_uniform(
                                           seed, static_cast<std::uint64_t>(l),
                                           stepc,
                                           static_cast<std::uint64_t>(p), 8));
                    e = rwpt_uniform(seed, static_cast<std::uint64_t>(l),
                                     stepc, static_cast<std::uint64_t>(p), 9);
                }
            }

            const int j_from = locate(z, nseg);

            // Shear displacement in VOLUME units through the ratio form:
            // dζ = u_dev·dt·A = (u_dev/ū)·(ū·A·dt) = (u_dev/ū)·(Q·dt) —
            // exact and unit-safe; Q·dt is the substep's through-volume,
            // supplied by the caller (NOT v_in, which differs from Q·dt
            // whenever the link's volume is changing).
            const double dz =
                (rwpt_u_dev(e, ubar, ustar, turbulent) / ubar) * q_dt;

            // Vertical walk with Itô drift, reflected into (0,1). The
            // kernels already return η-units (D_t/h² and its η-gradient).
            const double d_eta = rwpt_d_eta(e, h, ustar, turbulent);
            const double drift = rwpt_d_eta_grad(e, h, ustar, turbulent);
            e += drift * dt +
                 rwpt_normal(seed, static_cast<std::uint64_t>(l), stepc,
                             static_cast<std::uint64_t>(p), 3) *
                     std::sqrt(std::max(2.0 * d_eta * dt, 0.0));
            while (e < 0.0 || e > 1.0) {
                if (e < 0.0) e = -e;
                if (e > 1.0) e = 2.0 - e;
            }

            // Longitudinal reflection at the link ends (no inter-link
            // particle transfer — recorded).
            z += dz;
            while (z < 0.0 || z > vol) {
                if (z < 0.0) z = -z;
                if (z > vol) z = 2.0 * vol - z;
            }

            const int j_to = locate(z, nseg);

            // Upwind-carried exchange: each crossing moves the ORIGIN
            // segment's concentration × (V/N) of water-equivalent mass,
            // one boundary at a time so multi-segment jumps stay upwind.
            if (j_to != j_from) {
                const double vshare = vol / static_cast<double>(npart_);
                int j = j_from;
                const int stepdir = (j_to > j_from) ? 1 : -1;
                while (j != j_to) {
                    const int jn = j + stepdir;
                    // Quantum = upwind conc x the PENETRATION past this
                    // boundary (capped at the particle's own water, V/N).
                    // A fixed V/N quantum let random-walk RE-crossings each
                    // move a full share while the limiter zeroed the
                    // up-gradient legs -- a rectified pump measured at
                    // ~10x Elder on the 6000 ft probe (X3b round). The
                    // penetration scale restores flux ~ displacement, so
                    // the exchange magnitude is dt-robust.
                    const double bnd =
                        bounds_[static_cast<std::size_t>(std::max(j, jn))];
                    const double pen = std::min(std::abs(z - bnd), vshare);
                    for (int s = 0; s < ns; ++s)
                        delta[static_cast<std::size_t>(s)] =
                            segs.seg_conc(l, j, s) * pen;
                    exchange(segs, l, j, jn, delta.data(), ns);
                    j = jn;
                }
            }

            zeta_[ip] = z;
            eta_[ip] = e;
            seg_idx_[ip] = j_to;
        }
    }

private:
    /// D-L4's intent is "no per-particle binary search on the hot path";
    /// segment counts are ≤ cap (≤ ~100) and this branch-light linear walk
    /// is exact. The maintained-sort two-pointer refinement is an L6
    /// perf-pass item, recorded.
    int locate(double z, int nseg) const {
        int j = 0;
        while (j + 1 < nseg &&
               z >= bounds_[static_cast<std::size_t>(j) + 1])
            ++j;
        return j;
    }

    /// Move `delta` (ns masses) from segment j to jn, with the D-X3b1
    /// equalizing limiter: an event may move the receiver TOWARD the donor
    /// but never past it (`take ≤ (c_j − c_n)·v_n`), and never more than
    /// the donor holds. Consequences, all deliberate: the maximum
    /// principle holds by construction per event; a uniform field is a
    /// strict no-op (zero spurious drift, exactly); mass conserves exactly
    /// (every debit pairs with a credit).
    void exchange(SegmentStore& segs, int l, int j, int jn,
                  const double* delta, int ns) {
        for (int s = 0; s < ns; ++s) {
            const double vj = segs.seg_volume(l, j);
            const double vn = segs.seg_volume(l, jn);
            if (vj <= 0.0 || vn <= 0.0) continue;
            const double cj = segs.seg_conc(l, j, s);
            const double cn = segs.seg_conc(l, jn, s);
            const double take =
                std::min({delta[s], cj * vj,
                          std::max(0.0, (cj - cn)) * vn});
            if (take <= 0.0) continue;
            segs.set_seg_conc(l, j, s, (cj * vj - take) / vj);
            segs.set_seg_conc(l, jn, s, (cn * vn + take) / vn);
        }
    }

    int nl_ = 0, npart_ = 0;
    std::vector<double> zeta_, eta_;
    std::vector<int> seg_idx_;
    std::vector<double> bounds_;
};

}  // namespace lard
}  // namespace openswmm

#endif  // OPENSWMM_QUALITY_LARD_RWPT_DISPERSION_HPP
