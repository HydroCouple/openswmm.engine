/**
 * @file RomDiagTrust.hpp
 * @brief PR H3 — H_skew trust diagnostic: flow-weighted mean squared Froude
 *        number (`fr_trust`) and active-node surcharge fraction
 *        (`surcharge_frac`).
 *
 * @details The Picard operator H carries ONE dqdh per link, applied
 *          antisymmetrically to the two endpoint sumdqdh accumulators, so the
 *          assembled H is symmetric by construction — an entrywise skew
 *          ratio is identically zero (upstream audit finding, §0.5.4 of
 *          HSYM_RESIDUALS_PR_CHECKLIST.md). The O(Fr^2) band-width error the
 *          roadmap accepts at peak flow instead lives in the gap between the
 *          Picard surrogate and the true Saint-Venant Jacobian, whose
 *          off-diagonal asymmetry scales with the advective terms the
 *          surrogate drops. `fr_trust` is the honest, computable proxy for
 *          that gap; `surcharge_frac` flags the regime (Fr -> 0 under
 *          surcharge) where the symmetric surrogate is instead nearly exact.
 *
 *          Pure functions over plain arrays — no dependency on DWSolver or
 *          SimulationContext, so they can be unit-tested with hand-built
 *          fixtures independent of the real hydraulics.
 *
 * @ingroup engine_uncertainty
 */

#ifndef OPENSWMM_ENGINE_UNCERTAINTY_ROM_DIAG_TRUST_HPP
#define OPENSWMM_ENGINE_UNCERTAINTY_ROM_DIAG_TRUST_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace openswmm::uncertainty {

/**
 * @brief Flow-weighted mean squared Froude number over eligible conduits.
 *
 * `fr_trust = sum_e |Q_e|*Fr_e^2 / sum_e |Q_e|`, Fr clamped to [0, 1.5]
 * before squaring (keeps one supercritical trickle from dominating).
 *
 * @param flow      Per-link flow (any consistent units), length n_links.
 * @param froude    Per-link Froude number, length n_links.
 * @param eligible  Per-link inclusion mask (non-null entries only), length
 *                  n_links. Callers combine "is a conduit" and "not
 *                  bypassed this step" into this single mask -- froude_ is
 *                  only meaningful for conduits, and a bypassed link's
 *                  flow/Froude are held from a prior converged state.
 *                  Pass nullptr to include every link.
 * @param n_links   Length of all three arrays.
 * @return The weighted mean, or exactly 0.0 when the weighted denominator
 *         (sum of |flow| over eligible links) is zero -- e.g. still water.
 *         Never NaN.
 */
inline double computeFrTrust(const double* flow, const double* froude,
                              const uint8_t* eligible, int n_links) noexcept {
    double num = 0.0;
    double den = 0.0;
    for (int j = 0; j < n_links; ++j) {
        if (eligible != nullptr && eligible[j] == 0) continue;
        const double q = std::fabs(flow[j]);
        const double fr = std::clamp(froude[j], 0.0, 1.5);
        num += q * fr * fr;
        den += q;
    }
    return (den > 0.0) ? (num / den) : 0.0;
}

/**
 * @brief Fraction of the given active nodes with `node_surcharged` set.
 *
 * @param node_surcharged  Full-node-space surcharge flags, length n_nodes.
 * @param n_nodes          Length of node_surcharged.
 * @param active_full_idx  Active-index -> full-node-index map, length
 *                          n_active (e.g. SpectralROM1D::full_to_active
 *                          inverted, or the engine's rom1d_active_map_).
 * @param n_active         Length of active_full_idx.
 * @return count(surcharged active nodes) / n_active, or 0.0 if n_active<=0.
 */
inline double computeSurchargeFrac(const uint8_t* node_surcharged, int n_nodes,
                                    const int* active_full_idx,
                                    int n_active) noexcept {
    if (n_active <= 0) return 0.0;
    int count = 0;
    for (int ai = 0; ai < n_active; ++ai) {
        const int ui = active_full_idx[ai];
        if (ui >= 0 && ui < n_nodes && node_surcharged[ui] != 0) ++count;
    }
    return static_cast<double>(count) / static_cast<double>(n_active);
}

}  // namespace openswmm::uncertainty

#endif  // OPENSWMM_ENGINE_UNCERTAINTY_ROM_DIAG_TRUST_HPP
