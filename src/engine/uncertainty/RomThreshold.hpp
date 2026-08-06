/**
 * @file RomThreshold.hpp
 * @brief PR H10 — threshold-crossing probability and a cheap bimodality flag
 *        over an already-reconstructed ensemble.
 *
 * @details The sidecar reports `q05/q50/q95`. Two consequences this file
 *          addresses:
 *
 *          1. The decision-relevant question at a crown or a control setpoint
 *             is almost never "what is the 95th percentile depth" but "what
 *             FRACTION of the ensemble crosses this threshold". That number is
 *             directly available from the member values already reconstructed
 *             for the quantile sort — it was simply never computed.
 *
 *          2. When members genuinely split across a threshold the ensemble is
 *             bimodal, and reporting a bimodal ensemble as three order
 *             statistics is ACTIVELY MISLEADING: `q50` lands in the empty
 *             valley between the two clusters, a value no member actually
 *             holds. This is the honest answer to "a linear ROM cannot
 *             branch" — it cannot, so report the branch PROBABILITY and flag
 *             the split rather than implying a single smooth band.
 *
 *          Both are pure functions over a sorted array, with no dependency on
 *          SpectralROM1D or SimulationContext, so they can be pinned exactly
 *          against hand-built ensembles.
 *
 * @note Every function here expects values sorted ASCENDING.
 *       `SpectralROM1D::computeQuantiles()` already sorts each node's row of
 *       `recon_buf_` in place to extract the quantiles, so the caller reuses
 *       that buffer directly — no second reconstruction, no second sort.
 *
 * @ingroup engine_uncertainty
 */

#ifndef OPENSWMM_ENGINE_UNCERTAINTY_ROM_THRESHOLD_HPP
#define OPENSWMM_ENGINE_UNCERTAINTY_ROM_THRESHOLD_HPP

#include <algorithm>
#include <cmath>

namespace openswmm::uncertainty {

/// Which physical threshold an exceedance fraction was measured against.
/// Recorded per row so a reader can tell "5% of members surcharge" from
/// "5% of members flood" — very different statements.
enum class ThresholdKind : int {
    NONE      = 0,  ///< No usable threshold on this node — skipped, not reported
    CROWN     = 1,  ///< Conduit crown at the node: surcharge onset
    MAX_DEPTH = 2   ///< Node MaxDepth: flooding
};

/// Stable CSV token for a ThresholdKind.
inline const char* thresholdKindName(ThresholdKind k) noexcept {
    switch (k) {
        case ThresholdKind::CROWN:     return "CROWN";
        case ThresholdKind::MAX_DEPTH: return "MAX_DEPTH";
        case ThresholdKind::NONE:      break;
    }
    return "NONE";
}

/**
 * @brief Fraction of ensemble members STRICTLY above `threshold`.
 *
 * @param sorted_values Ascending member values (same units as `threshold`).
 * @param n             Number of members.
 * @param threshold     Crossing level.
 * @return count(value > threshold) / n, in [0,1]. Exactly 0.0 when every
 *         member is at or below the threshold and exactly 1.0 when every
 *         member is above it — a zero-perturbation ensemble (all members
 *         identical) therefore always yields exactly 0.0 or 1.0, never an
 *         intermediate value.
 */
inline double exceedanceFraction(const double* sorted_values, int n,
                                  double threshold) noexcept {
    if (sorted_values == nullptr || n <= 0) return 0.0;
    // First element strictly greater than the threshold; everything from
    // there to the end exceeds it (the input is sorted ascending).
    const double* first_above =
        std::upper_bound(sorted_values, sorted_values + n, threshold);
    const auto n_above = static_cast<double>((sorted_values + n) - first_above);
    return n_above / static_cast<double>(n);
}

/// Outcome of the gap-statistic bimodality check.
struct ModalityResult {
    bool   flagged       = false;  ///< Both criteria met
    double gap_ratio     = 0.0;    ///< g_max / IQR (0 when IQR is degenerate)
    double minority_frac = 0.0;    ///< Smaller side's share of the ensemble
    int    gap_index     = -1;     ///< Gap lies between sorted[i] and sorted[i+1]
};

/**
 * @brief Cheap bimodality flag via the gap statistic on order statistics.
 *
 * Deliberately NOT a mixture fit or a dip test — both are overkill at the
 * M ≈ 50 ensembles this sidecar runs, where the whole point is that the
 * check costs nothing on top of the sort the quantiles already did.
 *
 * Flags when BOTH hold:
 *   - the largest interior gap between consecutive order statistics, divided
 *     by the interquartile range, exceeds `gap_ratio_threshold`; and
 *   - at least `min_side_fraction` of members fall on EACH side of that gap.
 *
 * The second criterion is what keeps a single outlier from being reported as
 * a second mode: one stray member 10 IQRs away produces a huge gap ratio but
 * a 1/M minority share.
 *
 * @param sorted_values        Ascending member values.
 * @param n                    Number of members.
 * @param gap_ratio_threshold  g_max/IQR must EXCEED this (strictly).
 * @param min_side_fraction    Each side must hold AT LEAST this share.
 * @return Flag plus the two measured statistics, so a caller can report why.
 *
 * @note Degenerate spread (IQR ≈ 0 — e.g. a zero-perturbation ensemble where
 *       every member is identical) is never flagged: the ratio has no
 *       meaningful denominator, and an ensemble with no spread has no modes
 *       to separate. Reported as `gap_ratio = 0`.
 */
inline ModalityResult gapModality(const double* sorted_values, int n,
                                   double gap_ratio_threshold,
                                   double min_side_fraction) noexcept {
    ModalityResult r;
    // Below 4 members the quartile span is not meaningful.
    if (sorted_values == nullptr || n < 4) return r;

    // Nearest-rank quartiles, matching computeQuantiles()'s own convention.
    const auto idx = [n](double p) {
        return static_cast<int>(p * (static_cast<double>(n) - 1.0) + 0.5);
    };
    const double iqr = sorted_values[idx(0.75)] - sorted_values[idx(0.25)];
    if (!(iqr > 1.0e-12)) return r;   // degenerate spread → no modes

    // Largest interior gap.
    double g_max = 0.0;
    int    g_at  = -1;
    for (int i = 0; i + 1 < n; ++i) {
        const double g = sorted_values[i + 1] - sorted_values[i];
        if (g > g_max) { g_max = g; g_at = i; }
    }
    if (g_at < 0) return r;

    const double left  = static_cast<double>(g_at + 1);
    const double right = static_cast<double>(n - (g_at + 1));
    r.gap_index     = g_at;
    r.gap_ratio     = g_max / iqr;
    r.minority_frac = std::min(left, right) / static_cast<double>(n);
    r.flagged       = (r.gap_ratio > gap_ratio_threshold) &&
                      (r.minority_frac >= min_side_fraction);
    return r;
}

}  // namespace openswmm::uncertainty

#endif  // OPENSWMM_ENGINE_UNCERTAINTY_ROM_THRESHOLD_HPP
