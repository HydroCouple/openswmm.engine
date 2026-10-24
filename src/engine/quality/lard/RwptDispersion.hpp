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
 * @file      RwptDispersion.hpp
 * @brief     X3b: RWPT longitudinal dispersion on LARD segments.
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
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_QUALITY_LARD_RWPT_DISPERSION_HPP
#define OPENSWMM_QUALITY_LARD_RWPT_DISPERSION_HPP
#include "../../core/CompletedSourceArchive.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "../../core/SimulationContext.hpp"
#include "SegmentStore.hpp"

namespace openswmm {
namespace lard {

constexpr int kRwptParticlesPerLink = 2000;
constexpr double kKappa = 0.41;      ///< von Kármán
constexpr double kNuWater = 1.05e-5; ///< kinematic viscosity, ft²/s
constexpr double kDm = 1.3454e-8;    ///< molecular diffusivity, ft²/s
constexpr double kEtaMin = 1.0e-3;   ///< log-law floor
constexpr double kReTurb = 2000.0;

// ---------------------------------------------------------------------------
// D-L6 counter-based RNG: pure function of the key, no state anywhere.
// ---------------------------------------------------------------------------
/**
 * @brief Mix one unsigned64-bit deterministic random key.
 * @param[in] z Counter/key input, unsigned overflow intentionally wraps modulo2^64.
 * @return SplitMix-style mixed key; pure, allocation-free, independent of shared random state.
 */
inline std::uint64_t rwpt_hash(std::uint64_t z) {
    z += 0x9e3779b97f4a7c15ULL;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}
/**
 * @brief Uniform in (0,1), keyed. `draw` distinguishes multiple draws per particle.
 *
 * @details Generate a deterministic counter-keyed pseudorandom uniform variate.
 * @param[in] seed Reproducibility seed, no global generator state.
 * @param[in] link Stable native link identity key.
 * @param[in] stepc Accepted quality-step counter; repeat tuple reproduces sample.
 * @param[in] particle Zero-based particle key; particle*8+draw wraps unsigned arithmetic.
 * @param[in] draw Draw stream key (draw+1 also used by normal).
 * @return Nominal open-unit variate from53 key bits with half-unit offset; final floating
 * rounding can reach1.
 * @note Pure/no allocation or model mutation; counter tuple design, not call order, controls
 * reproducibility.
 */
inline double rwpt_uniform(std::uint64_t seed, std::uint64_t link, std::uint64_t stepc,
                           std::uint64_t particle, std::uint64_t draw) {
    std::uint64_t k = rwpt_hash(seed ^ rwpt_hash(link));
    k = rwpt_hash(k ^ rwpt_hash(stepc));
    k = rwpt_hash(k ^ rwpt_hash(particle * 8ULL + draw));
    // 53-bit mantissa, strictly inside (0,1).
    return (static_cast<double>(k >> 11) + 0.5) * (1.0 / 9007199254740992.0);
}
/**
 * @brief Standard normal (Box–Muller on two keyed uniforms).
 *
 * @details Generate a deterministic Box-Muller normal variate from adjacent uniform draws.
 * @param[in] seed Reproducibility seed, no global generator state.
 * @param[in] link Stable native link identity key.
 * @param[in] stepc Accepted quality-step counter; repeat tuple reproduces sample.
 * @param[in] particle Zero-based particle key; particle*8+draw wraps unsigned arithmetic.
 * @param[in] draw Draw stream key (draw+1 also used by normal).
 * @return Standard normal sample using draws draw and draw+1; no cached paired variate.
 * @note Pure/no allocation or model mutation; counter tuple design, not call order, controls
 * reproducibility.
 */
inline double rwpt_normal(std::uint64_t seed, std::uint64_t link, std::uint64_t stepc,
                          std::uint64_t particle, std::uint64_t draw) {
    const double u1 = rwpt_uniform(seed, link, stepc, particle, draw);
    const double u2 = rwpt_uniform(seed, link, stepc, particle, draw + 1);
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
}

// ---------------------------------------------------------------------------
// Profile kernels — exposed free functions so the unit gates can pin the
// math without an engine run.
// ---------------------------------------------------------------------------
/**
 * @brief Velocity DEVIATION u(η) − ū. Turbulent: log-law; laminar: parabola.
 *
 * @details Evaluate longitudinal particle velocity relative to cross-sectional mean.
 * @param[in] eta Dimensionless ordinate, normally0..1; turbulent log floors at1e-3.
 * @param[in] ubar Mean positive speed ft/s used by laminar profile.
 * @param[in] ustar Friction speed ft/s used by turbulent log profile.
 * @param[in] turbulent True selects log-law, false parabolic laminar deviation.
 * @return Velocity deviation ft/s, not absolute velocity. No finite/range repair outside log
 * floor, allocation or state mutation.
 */
inline double rwpt_u_dev(double eta, double ubar, double ustar, bool turbulent) {
    // The kEtaMin floor belongs to the LOG-LAW only (ln 0 = -inf); clamping
    // the laminar parabola too biased its mean by ~1.5e-6*ubar — gate 4's
    // mean-free quadrature caught it (X3b round).
    if (turbulent)
        return (ustar / kKappa) * (1.0 + std::log(std::max(eta, kEtaMin)));
    return 1.5 * ubar * eta * (2.0 - eta) - ubar; // parabola, mean-free
}
/**
 * @brief Vertical diffusivity in η-units (D_t/h²), and its η-gradient (the Itô drift).
 * Turbulent: Rouse parabola κ·u*·h·η(1−η); laminar: molecular.
 *
 * @details Evaluate diffusivity in the dimensionless vertical particle coordinate.
 * @param[in] eta Dimensionless particle ordinate, caller supplies0..1 for physical profile.
 * @param[in] h Finite positive water depth ft; no zero guard.
 * @param[in] ustar Finite nonnegative friction speed ft/s.
 * @param[in] turbulent True selects shear profile, false molecular laminar profile.
 * @return Inverse-seconds diffusivity: laminar molecular Dm/h², turbulent
 * kappa*ustar*clamp(eta,0,1)*(1-clamp(eta,0,1))/h.
 * @note Pure/no allocation; no generic sign/finite/dry-depth validation.
 */
inline double rwpt_d_eta(double eta, double h, double ustar, bool turbulent) {
    if (!turbulent)
        return kDm / (h * h);
    const double e = std::min(std::max(eta, 0.0), 1.0);
    return kKappa * ustar * e * (1.0 - e) / h; // (κ u* h η(1-η))/h²
}
/**
 * @brief Evaluate coordinate diffusivity gradient used as stochastic drift.
 * @param[in] eta Dimensionless particle ordinate, caller supplies0..1 for physical profile.
 * @param[in] h Finite positive water depth ft; no zero guard.
 * @param[in] ustar Finite nonnegative friction speed ft/s.
 * @param[in] turbulent True selects shear profile, false molecular laminar profile.
 * @return 0 for laminar, otherwise kappa*ustar*(1-2*eta)/h in inverse seconds; eta is not
 * clamped here.
 * @note Pure/no allocation; no generic sign/finite/dry-depth validation.
 */
inline double rwpt_d_eta_grad(double eta, double h, double ustar, bool turbulent) {
    if (!turbulent)
        return 0.0;
    return kKappa * ustar * (1.0 - 2.0 * eta) / h;
}

/**
 * @brief Hydraulic radius: exact for CIRCULAR from flow area, ≈ depth otherwise (wide-channel v1
 * approximation, recorded).
 *
 * @details Estimate hydraulic radius for the RWPT turbulence calculation.
 * @param[in] area Unused area argument retained for geometry-call symmetry; circle area is
 * recomputed.
 * @param[in] depth Water depth ft; noncircular/invalid-circle fallback max(depth,0).
 * @param[in] diam Positive circular full diameter ft when circular is true.
 * @param[in] circular True selects circular-segment wetted geometry.
 * @return Radius ft from circle area/perimeter after clamped acos argument; tiny angle/perimeter
 * uses depth fallback. No finite geometry repair, allocation or state mutation.
 */
inline double rwpt_hyd_radius(double area, double depth, double diam, bool circular) {
    if (!circular || diam <= 0.0 || depth <= 0.0)
        return std::max(depth, 0.0);
    const double ratio = std::min(std::max(1.0 - 2.0 * depth / diam, -1.0), 1.0);
    const double theta = 2.0 * std::acos(ratio);
    if (theta <= 1.0e-6)
        return std::max(depth, 0.0);
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
    /**
     * @brief Serialize/restore fixed particle state then validate position/ordinate values.
     * @param[in,out] a Archive already dimensioned for this link/particle layout; fixed() may
     * mutate zeta, eta and segment-index buffers on load. Counts/dimensions and scratch bounds
     * are not archived.
     * @throws std::runtime_error Nonfinite position or ordinate, or ordinate outside0..1;
     * archive errors propagate. Negative finite zeta sentinel is accepted, seg_idx bounds are
     * not validated here.
     * @note Exclusive completed-source barrier; failures can leave changed arrays, no rollback
     * or clock advance.
     */
    void checkpoint(detail::CompletedSourceArchive& a) {
        a.fixed(zeta_, eta_, seg_idx_);
        for (double v : zeta_)
            if (!std::isfinite(v))
                throw std::runtime_error("Invalid LARD particle position.");
        for (double v : eta_)
            if (!std::isfinite(v) || v < 0 || v > 1)
                throw std::runtime_error("Invalid LARD particle ordinate.");
    }
    /**
     * @brief Reset deterministic particle histories for new owner dimensions.
     * @param[in] n_links Nonnegative link count, with fitting products.
     * @param[in] n_per_link Positive particles per link, default2000 for useful dispersion.
     * @note Assigns zeta=-1 uninitialized sentinel, eta=.5, segment index0. Does not reset
     * shared scratch bounds or hydraulic/clock state. No dimension/overflow validation;
     * allocation may partially reset. Exclusive initialization/reconfiguration; reproducibility
     * changes when particle count changes.
     */
    void resize(int n_links, int n_per_link = kRwptParticlesPerLink) {
        nl_ = n_links;
        npart_ = n_per_link;
        const std::size_t total = static_cast<std::size_t>(nl_) * static_cast<std::size_t>(npart_);
        zeta_.assign(total, -1.0); // <0 marks "not yet seeded"
        eta_.assign(total, 0.5);
        seg_idx_.assign(total, 0);
    }

    /**
     * @brief One substep of dispersive exchange on link `l`'s segments.
     *
     * @param v_in     the volume that entered at the FRONT this substep (bulk-advection shift in
     * volume coordinates).
     * @param stepc    global substep counter (RNG key component).
     * @param delta    scratch, ns entries — reused across calls.
     *
     * @details Exchange segment species inventory using deterministic reflected RWPT particles.
     * @param[in,out] ctx Unused context retained by the transport interface; no mutation through
     * this argument.
     * @param[in,out] segs Prepared finite nonnegative segment ring for l, matching initialized
     * particle owner; updates concentrations by bounded pairwise transfers while preserving
     * represented family inventory.
     * @param[in] l Valid native link below resized particle and segment counts.
     * @param[in] ubar Positive finite absolute mean speed ft/s; nonpositive returns.
     * @param[in] h Finite water depth ft; <=1e-3 returns.
     * @param[in] rh Finite nonnegative hydraulic radius ft, floored1e-3 for friction estimate.
     * @param[in] n_manning Finite Manning coefficient in native US formulation used for slope
     * estimate.
     * - Additional argument v_in: Accepted incoming water ft3 shifting retained
     * volume-coordinate particles.
     * @param[in] q_dt Nonnegative absolute transported volume ft3 over this interval, scales
     * velocity deviation.
     * @param[in] dt Positive finite accepted quality duration s for vertical diffusion/drift.
     * - Additional argument stepc: Deterministic accepted-step counter; do not change on
     * repeated trial of same phase.
     * @param[in] seed Fixed simulation reproducibility seed.
     * - Additional argument delta: Scratch with at least segs.species() entries, nonaliasing
     * ring/particle state; overwritten for each crossed segment boundary, not cleared on skipped
     * call.
     * @note Fewer than2 segments or nonpositive total water returns. Rebuilds cumulative volume
     * bounds, initializes/recycles particles, reflects ordinate in0..1 and volume coordinate
     * in0..total water, then limits downhill concentration transfers by source mass and receiver
     * concentration difference. Requires finite qualified inputs: infinite reflected positions
     * can fail to terminate, no generic physical validation. Allocates bounds and mutates
     * particle/segment state progressively; no rollback, global ledger/water/clock mutation.
     * Scratch and store imply exclusive call, even for distinct links.
     */
    void disperse(SimulationContext& ctx, SegmentStore& segs, int l, double ubar, double h,
                  double rh, double n_manning, double v_in, double q_dt, double dt,
                  std::uint64_t stepc, std::uint64_t seed, std::vector<double>& delta) {
        (void)ctx;
        const int nseg = segs.count(l);
        if (nseg < 2 || h <= 1.0e-3 || ubar <= 0.0)
            return;
        const double vol = segs.total_volume(l);
        if (vol <= 0.0)
            return;
        const int ns = segs.species();

        // Friction: Manning S_f at the link's own n → u*. Regime by Re.
        const double sf = (n_manning * ubar) * (n_manning * ubar) /
                          (2.208 * std::pow(std::max(rh, 1.0e-3), 4.0 / 3.0));
        const double ustar = std::sqrt(32.174 * std::max(rh, 1.0e-3) * sf);
        const bool turbulent = (ubar * 4.0 * rh / kNuWater) >= kReTurb;

        // Segment boundaries in volume coordinates, front → back.
        bounds_.assign(static_cast<std::size_t>(nseg) + 1, 0.0);
        for (int j = 0; j < nseg; ++j)
            bounds_[static_cast<std::size_t>(j) + 1] =
                bounds_[static_cast<std::size_t>(j)] + segs.seg_volume(l, j);

        const std::size_t base = static_cast<std::size_t>(l) * static_cast<std::size_t>(npart_);
        for (int p = 0; p < npart_; ++p) {
            const std::size_t ip = base + static_cast<std::size_t>(p);
            double z = zeta_[ip];
            double e = eta_[ip];

            // Seed / recycle: unseeded particles spread evenly; particles
            // that rode out the back are new water entering at the front.
            if (z < 0.0) {
                z = vol * (static_cast<double>(p) + 0.5) / static_cast<double>(npart_);
                e = rwpt_uniform(seed, static_cast<std::uint64_t>(l), stepc,
                                 static_cast<std::uint64_t>(p), 7);
            } else {
                z += v_in; // ride the bulk advection (volume coordinates)
                if (z >= vol) {
                    z = std::min(v_in, vol * rwpt_uniform(seed, static_cast<std::uint64_t>(l),
                                                          stepc, static_cast<std::uint64_t>(p), 8));
                    e = rwpt_uniform(seed, static_cast<std::uint64_t>(l), stepc,
                                     static_cast<std::uint64_t>(p), 9);
                }
            }

            const int j_from = locate(z, nseg);

            // Shear displacement in VOLUME units through the ratio form:
            // dζ = u_dev·dt·A = (u_dev/ū)·(ū·A·dt) = (u_dev/ū)·(Q·dt) —
            // exact and unit-safe; Q·dt is the substep's through-volume,
            // supplied by the caller (NOT v_in, which differs from Q·dt
            // whenever the link's volume is changing).
            const double dz = (rwpt_u_dev(e, ubar, ustar, turbulent) / ubar) * q_dt;

            // Vertical walk with Itô drift, reflected into (0,1). The
            // kernels already return η-units (D_t/h² and its η-gradient).
            const double d_eta = rwpt_d_eta(e, h, ustar, turbulent);
            const double drift = rwpt_d_eta_grad(e, h, ustar, turbulent);
            e += drift * dt + rwpt_normal(seed, static_cast<std::uint64_t>(l), stepc,
                                          static_cast<std::uint64_t>(p), 3) *
                                  std::sqrt(std::max(2.0 * d_eta * dt, 0.0));
            while (e < 0.0 || e > 1.0) {
                if (e < 0.0)
                    e = -e;
                if (e > 1.0)
                    e = 2.0 - e;
            }

            // Longitudinal reflection at the link ends (no inter-link
            // particle transfer — recorded).
            z += dz;
            while (z < 0.0 || z > vol) {
                if (z < 0.0)
                    z = -z;
                if (z > vol)
                    z = 2.0 * vol - z;
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
                    const double bnd = bounds_[static_cast<std::size_t>(std::max(j, jn))];
                    const double pen = std::min(std::abs(z - bnd), vshare);
                    for (int s = 0; s < ns; ++s)
                        delta[static_cast<std::size_t>(s)] = segs.seg_conc(l, j, s) * pen;
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
    /**
     * @brief D-L4's intent is "no per-particle binary search on the hot path"; segment counts
     * are ≤ cap (≤ ~100) and this branch-light linear walk is exact. The maintained-sort
     * two-pointer refinement is an L6 perf-pass item, recorded.
     *
     * @details Locate a particle's segment from current cumulative volume boundaries.
     * @param[in] z Finite volume coordinate ft3, caller keeps0..total volume.
     * @param[in] nseg Positive live segment count with bounds_ covering nseg+1 entries.
     * @return First segment whose upper boundary exceeds z, or last segment at total volume;
     * boundary equality selects downstream logical segment. No validation, allocation or
     * mutation.
     */
    int locate(double z, int nseg) const {
        int j = 0;
        while (j + 1 < nseg && z >= bounds_[static_cast<std::size_t>(j) + 1])
            ++j;
        return j;
    }

    /**
     * @brief Move `delta` (ns masses) from segment j to jn, with the D-X3b1 equalizing limiter:
     * an event may move the receiver TOWARD the donor but never past it (`take ≤ (c_j −
     * c_n)·v_n`), and never more than the donor holds. Consequences, all deliberate: the maximum
     * principle holds by construction per event; a uniform field is a strict no-op (zero
     * spurious drift, exactly); mass conserves exactly (every debit pairs with a credit).
     *
     * @details Transfer bounded downhill species inventory between two segments without moving
     * water.
     * @param[in,out] segs Valid finite segment owner; updates donor/receiver concentrations
     * only.
     * @param[in] l Valid initialized link.
     * @param[in] j Active donor logical segment index.
     * @param[in] jn Active receiver logical segment index, distinct from j.
     * @param[in] delta Readable ns requested concentration*ft3 transfer tuple, nonaliasing
     * mutable ring.
     * @param[in] ns Valid species count not exceeding ring rows or tuple extent.
     * @note Each species transfers min(requested, donor mass, positive concentration
     * difference*receiver volume), only for positive water and transfer. No finite repair or
     * positivity clamp beyond bound; no allocation/global ledger/clock change. Exclusive segment
     * mutation and caller-owned scratch.
     */
    void exchange(SegmentStore& segs, int l, int j, int jn, const double* delta, int ns) {
        for (int s = 0; s < ns; ++s) {
            const double vj = segs.seg_volume(l, j);
            const double vn = segs.seg_volume(l, jn);
            if (vj <= 0.0 || vn <= 0.0)
                continue;
            const double cj = segs.seg_conc(l, j, s);
            const double cn = segs.seg_conc(l, jn, s);
            const double take = std::min({delta[s], cj * vj, std::max(0.0, (cj - cn)) * vn});
            if (take <= 0.0)
                continue;
            segs.set_seg_conc(l, j, s, (cj * vj - take) / vj);
            segs.set_seg_conc(l, jn, s, (cn * vn + take) / vn);
        }
    }

    int nl_ = 0, npart_ = 0;
    std::vector<double> zeta_, eta_;
    std::vector<int> seg_idx_;
    std::vector<double> bounds_;
};

} // namespace lard
} // namespace openswmm

#endif // OPENSWMM_QUALITY_LARD_RWPT_DISPERSION_HPP
