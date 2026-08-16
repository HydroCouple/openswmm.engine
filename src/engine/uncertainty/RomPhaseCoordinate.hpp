/**
 * @file RomPhaseCoordinate.hpp
 * @brief PR H11 — per-member phase coordinate for the 1D ROM: a travel-time
 *        field `T̄(x)` and a bounded deterministic-head history ring that
 *        together let a member's reconstructed head be sampled at its own
 *        shifted time instead of the shared deterministic clock.
 *
 * @details The 1D sidecar is an AMPLITUDE method: member `i` carries
 *          `δa_i = Pᵀ(h_i − h_det)`, a deviation on a fixed spatial basis
 *          evolved by a dissipative operator. A PHASE error — a filling
 *          front arriving minutes early or late depending on Manning's n —
 *          is neither: it does not decay, and it is not expressible as an
 *          amplitude on a fixed basis. VALIDATION.md documents this as an
 *          unrepresented 2-4 m transient width at front passage.
 *
 *          Manning's law gives conveyance velocity `u ∝ 1/n`, and
 *          `LOCAL_INERTIAL_DEVIATION_OPERATOR.md` establishes the kinematic
 *          celerity `c_k = (5/3)·u`. A member with roughness multiplier
 *          `mm_i` therefore travels at `c_i = c̄/mm_i`, giving travel time
 *          `T_i = mm_i·T̄` and a per-member phase offset
 *
 *              τ_i(x) = (mm_i − 1) · T̄(x)
 *
 *          — the SAME `(mm−1)` structure as the already-validated amplitude
 *          fixed point `δa_ss = (mm−1)·b_j` (both are the first-order
 *          response of a `1/n` conveyance law). Reconstruction becomes
 *          `h_i(x,t) = h_det(x, t − τ_i) + P·δa_i`: one scalar per member,
 *          plus a bounded ring buffer of `h_det` planes.
 *
 *          `T̄(x)` is a PATH INTEGRAL over the deterministic flow graph, not
 *          a solved field: an edge contributes `t_e = L_e / ((5/3)·|u_e|)`
 *          when `|u_e| >= u_min`, oriented by the sign of its flow, and
 *          accumulated downstream by a bounded max-relaxation (handles
 *          reversal, loops, and multi-inflow nodes without a topological
 *          sort). A STAGNANT edge (`|u_e| < u_min`) contributes EXACTLY ZERO
 *          — not infinity. This is the deliberate answer to the `u -> 0`
 *          regime that broke the 2D W3 drain-to-pond fixture: the phase
 *          coordinate models a TRAVELLING kinematic signal, and where there
 *          is no conveyance-driven translation there is no phase error to
 *          model (the amplitude channel, governed by H5's attenuation, is
 *          the operative mechanism there instead). `tau_edge_max` and
 *          `tau_total_max` provide two further saturation layers so nothing
 *          grows without bound in ponding.
 *
 *          `DetHistoryRing` stores a bounded, evenly-spaced history of
 *          `h_det` planes so a member with `τ_i > 0` can sample
 *          `H(t − τ_i)`. A member with `mm_i < 1` has `τ_i < 0` — a query
 *          into the FUTURE, which no history buffer holds — resolved by a
 *          past-anchored reflection `h_ref = 2·H(t) − H(t − |τ_i|)`, the
 *          first-order forward extrapolation over the same interval using
 *          only history that already exists. On a STEADY `h_det` this
 *          reflection and the `τ > 0` sample both equal `H(t)` exactly, so
 *          the phase channel contributes NO extra width once the network
 *          has settled — the same saturated-regime invariant the existing
 *          coverage gates already measure in.
 *
 *          Both `computeTravelTime` and `DetHistoryRing` are pure / plain-
 *          array structures with no dependency on DWSolver or
 *          SimulationContext, so they unit-test against hand-built fixtures
 *          independent of the real hydraulics.
 *
 * @ingroup engine_uncertainty
 */

#ifndef OPENSWMM_ENGINE_UNCERTAINTY_ROM_PHASE_COORDINATE_HPP
#define OPENSWMM_ENGINE_UNCERTAINTY_ROM_PHASE_COORDINATE_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace openswmm::uncertainty {

/// Configuration for the phase-coordinate machinery. Physical dials
/// (`u_min`, `tau_edge_max`, `tau_total_max`) are legitimate calibration
/// targets against brute-force MC (same category as H5's `alpha_floor`);
/// `enabled` is the off switch that preserves pre-H11 bit-identity.
struct PhaseConfig {
    /// Off switch. false ⇒ engine never refreshes/passes T̄; SpectralROM1D's
    /// unphased computeQuantiles() path runs unchanged.
    bool   enabled         = true;
    /// c_k = celerity_factor * u (Manning kinematic-wave celerity, 5/3).
    double celerity_factor = 5.0 / 3.0;
    /// Edges with |velocity| below this (ft/s or m/s, matching the caller's
    /// unit system) are treated as stagnant: contribute t_e = 0, not L/c.
    double u_min           = 1.0e-3;
    /// Per-edge cap on t_e (s) — bounds one anomalously slow edge.
    double tau_edge_max    = 3600.0;
    /// Whole-path cap on T̄(x) (s) — bounds cumulative saturation.
    double tau_total_max   = 7200.0;
    /// Max relaxation sweeps for computeTravelTime's downstream propagation.
    int    max_sweeps      = 64;
    /// Max planes retained by DetHistoryRing.
    int    max_planes      = 256;
};

/**
 * @brief Accumulate downstream travel time T̄(x) over a directed flow graph.
 *
 * Per edge e: `u_e = |vel_signed[e]|`; `t_e = (u_e >= cfg.u_min) ?
 * min(len[e] / (cfg.celerity_factor * u_e), cfg.tau_edge_max) : 0.0`.
 * Orientation: `up = (vel_signed[e] >= 0) ? n1[e] : n2[e]`, `dn` the other
 * endpoint. Propagated by bounded max-relaxation:
 *
 *     tbar_full[*] = 0
 *     repeat min(n_nodes_full, cfg.max_sweeps) times, over every edge:
 *         tbar_full[dn] = max(tbar_full[dn], tbar_full[up] + t_e)
 *     tbar_full[*] = min(tbar_full[*], cfg.tau_total_max)
 *
 * `max` over incoming paths (not sum, not first-wins) — the slowest
 * upstream path sets a node's front-arrival time, the conservative choice
 * for band width. The fixed sweep count makes this well-defined and
 * terminating even on a graph with flow cycles (a converged acyclic-flow
 * result is reached in at most `longest_path_edges` sweeps; a cyclic one
 * simply stops relaxing further once the caps saturate it).
 *
 * @param n_edges     Number of edges (conduits).
 * @param n1          Edge endpoint 1 (full node index), length n_edges.
 * @param n2          Edge endpoint 2 (full node index), length n_edges.
 * @param len         Edge length (same units as vel_signed*time), length n_edges.
 * @param vel_signed  Signed velocity per edge (>=0 ⇒ n1->n2 is downstream),
 *                    length n_edges.
 * @param n_nodes_full Length of tbar_full.
 * @param cfg         Phase configuration (celerity_factor, u_min, caps, sweeps).
 * @param tbar_full   Output, length n_nodes_full. Overwritten (not accumulated).
 */
inline void computeTravelTime(int n_edges, const int* n1, const int* n2,
                               const double* len, const double* vel_signed,
                               int n_nodes_full, const PhaseConfig& cfg,
                               double* tbar_full) noexcept {
    std::fill(tbar_full, tbar_full + n_nodes_full, 0.0);
    if (n_edges <= 0 || n_nodes_full <= 0) return;

    // Precompute per-edge (up, dn, t_e) once; reused across every sweep.
    std::vector<int>    up(static_cast<std::size_t>(n_edges));
    std::vector<int>    dn(static_cast<std::size_t>(n_edges));
    std::vector<double> t_e(static_cast<std::size_t>(n_edges));
    for (int e = 0; e < n_edges; ++e) {
        const auto ue = static_cast<std::size_t>(e);
        const bool forward = vel_signed[e] >= 0.0;
        up[ue] = forward ? n1[e] : n2[e];
        dn[ue] = forward ? n2[e] : n1[e];
        const double u = std::fabs(vel_signed[e]);
        if (u >= cfg.u_min && cfg.celerity_factor > 0.0) {
            const double c = cfg.celerity_factor * u;
            t_e[ue] = std::min(len[e] / c, cfg.tau_edge_max);
        } else {
            t_e[ue] = 0.0;
        }
    }

    const int n_sweeps = std::min(n_nodes_full, std::max(1, cfg.max_sweeps));
    for (int s = 0; s < n_sweeps; ++s) {
        bool changed = false;
        for (int e = 0; e < n_edges; ++e) {
            const auto ue = static_cast<std::size_t>(e);
            const int u_idx = up[ue];
            const int d_idx = dn[ue];
            if (u_idx < 0 || u_idx >= n_nodes_full || d_idx < 0 || d_idx >= n_nodes_full)
                continue;
            const double cand = tbar_full[u_idx] + t_e[ue];
            if (cand > tbar_full[d_idx]) {
                tbar_full[d_idx] = cand;
                changed = true;
            }
        }
        if (!changed) break;
    }

    for (int i = 0; i < n_nodes_full; ++i)
        tbar_full[i] = std::min(tbar_full[i], cfg.tau_total_max);
}

/// τ_i(x) = (mm_i − 1) · T̄(x). Exactly 0.0 at mm_i == 1.0 for any T̄ — the
/// invariant the whole design rests on (nominal member never phase-shifts).
inline double phaseOffset(double mm, double tbar) noexcept {
    return (mm - 1.0) * tbar;
}

/**
 * @brief Bounded, evenly-spaced circular history of h_det planes, sampled by
 *        linear interpolation for the phase-coordinate reconstruction.
 *
 * Enforces a minimum push spacing so a horizon can be covered within
 * max_planes regardless of the caller's (possibly adaptive) step size.
 * Queries before the oldest retained plane clamp to the oldest plane
 * (fail-safe: degrades to LESS spread early on, same shape as the existing
 * amplitude-channel spin-up); queries after the newest clamp to the newest.
 */
class DetHistoryRing {
public:
    /// (Re)configure storage. min_spacing <= 0 disables spacing suppression
    /// (every push() is stored, subject to the capacity wrap).
    void configure(int n_nodes, int max_planes, double min_spacing) noexcept {
        n_nodes_ = std::max(0, n_nodes);
        cap_ = std::max(2, max_planes);
        min_spacing_ = std::max(0.0, min_spacing);
        reset();
    }

    /// Clear all stored planes (does not change configuration).
    void reset() noexcept {
        times_.clear();
        planes_.clear();
        head_ = 0;
        count_ = 0;
    }

    bool empty() const noexcept { return count_ == 0; }

    /// Store plane at time t (length n_nodes_). Skipped if a plane was
    /// already pushed within min_spacing_ of t (except the very first push,
    /// which always stores). On overflow, evicts the oldest plane.
    void push(double t, const double* plane) noexcept {
        if (n_nodes_ <= 0) return;
        if (times_.empty())
            ensureStorage_();
        if (count_ > 0) {
            const double t_last = times_[lastIndex_()];
            if (t - t_last < min_spacing_) return;
        }
        const int slot = (head_ + count_) % cap_;
        std::copy(plane, plane + n_nodes_, &planes_[static_cast<std::size_t>(slot) * static_cast<std::size_t>(n_nodes_)]);
        times_[static_cast<std::size_t>(slot)] = t;
        if (count_ < cap_) {
            ++count_;
        } else {
            head_ = (head_ + 1) % cap_;  // overwrote the oldest slot; advance head
        }
    }

    /// Sample node `node` at time t_query via linear interpolation between
    /// the two bracketing stored planes; clamps outside the stored range.
    /// Returns 0.0 if empty or node is out of range (caller should guard
    /// with empty() first; this is a defensive fallback, not a contract).
    double sample(double t_query, int node) const noexcept {
        if (count_ == 0 || node < 0 || node >= n_nodes_) return 0.0;
        if (count_ == 1) return at_(0, node);

        const double t_oldest = times_[static_cast<std::size_t>(oldestSlot_())];
        const double t_newest = times_[static_cast<std::size_t>(lastIndex_())];
        if (t_query <= t_oldest) return at_(0, node);
        if (t_query >= t_newest) return at_(count_ - 1, node);

        // Binary search over logical index [0, count_) for the bracket.
        int lo = 0, hi = count_ - 1;
        while (hi - lo > 1) {
            const int mid = (lo + hi) / 2;
            if (timeAt_(mid) <= t_query) lo = mid; else hi = mid;
        }
        const double t0 = timeAt_(lo), t1 = timeAt_(hi);
        const double v0 = at_(lo, node), v1 = at_(hi, node);
        if (t1 <= t0) return v0;
        const double w = (t_query - t0) / (t1 - t0);
        return v0 + w * (v1 - v0);
    }

private:
    int n_nodes_ = 0;
    int cap_ = 2;
    double min_spacing_ = 0.0;
    std::vector<double> times_;
    std::vector<double> planes_;  // cap_ * n_nodes_, row-major by physical slot
    int head_ = 0;   // physical slot of the oldest stored plane
    int count_ = 0;  // number of valid planes currently stored

    void ensureStorage_() noexcept {
        times_.assign(static_cast<std::size_t>(cap_), 0.0);
        planes_.assign(static_cast<std::size_t>(cap_) * static_cast<std::size_t>(n_nodes_), 0.0);
    }

    int oldestSlot_() const noexcept { return head_; }
    int lastIndex_() const noexcept { return (head_ + count_ - 1) % cap_; }

    // logical index i in [0, count_) -> physical slot
    int slotOf_(int i) const noexcept { return (head_ + i) % cap_; }
    double timeAt_(int i) const noexcept { return times_[static_cast<std::size_t>(slotOf_(i))]; }
    double at_(int i, int node) const noexcept {
        const int slot = slotOf_(i);
        return planes_[static_cast<std::size_t>(slot) * static_cast<std::size_t>(n_nodes_) + static_cast<std::size_t>(node)];
    }
};

}  // namespace openswmm::uncertainty

#endif  // OPENSWMM_ENGINE_UNCERTAINTY_ROM_PHASE_COORDINATE_HPP
