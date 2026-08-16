/**
 * @file RomSurchargeAttenuation.hpp
 * @brief PR H5 — surcharged-regime sensitivity attenuation: per-active-node
 *        smooth factor `alpha_n` that damps the 1D ROM's Manning-sensitivity
 *        projection where the network is surcharged.
 *
 * @details The modal Manning-sensitivity source term
 *          `-lam_j*K1d*(1/mm-1)*b_j` (SpectralROM1D::advance) encodes
 *          *conveyance* sensitivity: the free-surface friction law
 *          `K ~ h^(5/3)/n`. Under surcharge, node heads are instead set by
 *          mass balance and backwater in a pressurized network, where
 *          `dh/dn` is small — but the ROM keeps scaling sensitivity with the
 *          (now large) surcharge depth anyway, over-predicting band widths
 *          50-190x (VALIDATION.md, reform PR 10). `alpha_n` folds elementwise
 *          into the sensitivity reference BEFORE projection
 *          (`b_j = P[:,j]^T*(alpha ⊙ bref)`), damping only the
 *          Manning-sensitivity channel; the forcing-sensitivity channel
 *          (runoff/soft-rain) is projected separately and untouched.
 *
 *          Formulation note (H5 design decision, amending the checklist's
 *          literal "fraction of incident conduits free-surface" candidate):
 *          the engine exposes crown data as a per-NODE scalar
 *          (`ctx_.nodes.crown_elev`), the same quantity DWSolver already uses
 *          for the binary `HSnapshot::node_surcharged` flag ("TRUE when node
 *          depth > crown elevation") — there is no clean per-conduit soffit
 *          field to build a true per-conduit ramp from without inventing new
 *          geometry plumbing for a value that gets re-aggregated back to a
 *          node scalar immediately anyway. alpha_n is instead a smooth
 *          (ramped) relaxation of that same node-level depth-vs-crown
 *          quantity: `ratio_n = (head-invert)/(crown_elev-invert)`, ramped
 *          to 0 over `[ramp_lo, ramp_hi]` (default [0.9, 1.1]), matching the
 *          checklist's literal ramp band while staying numerically
 *          consistent with the existing `node_surcharged` diagnostic.
 *
 *          Pure function over plain arrays — no dependency on DWSolver or
 *          SimulationContext, so it can be unit-tested with hand-built
 *          fixtures independent of the real hydraulics.
 *
 * @ingroup engine_uncertainty
 */

#ifndef OPENSWMM_ENGINE_UNCERTAINTY_ROM_SURCHARGE_ATTENUATION_HPP
#define OPENSWMM_ENGINE_UNCERTAINTY_ROM_SURCHARGE_ATTENUATION_HPP

#include <algorithm>

namespace openswmm::uncertainty {

/// Ramp band for the surcharge attenuation factor, as config fields (not
/// hardcoded constants) so fixtures/tests can exercise non-default bands.
struct SurchargeAttenuationConfig {
    /// depth/crown_depth ratio at and below which alpha == 1.0 (unattenuated).
    double ramp_lo = 0.9;
    /// depth/crown_depth ratio at and above which alpha reaches alpha_floor.
    double ramp_hi = 1.1;
    /// Floor alpha never ramps below, however deep the surcharge.
    ///
    /// A pressurized pipe still loses head to friction, and that loss still
    /// depends on n -- it just no longer follows the free-surface h^(5/3)/n
    /// conveyance law the un-attenuated ROM assumes. Ramping all the way to
    /// 0 (the checklist's literal candidate) claims Manning's n has NO effect
    /// once surcharged; validated against brute-force MC on a fixture forced
    /// into sustained deep surcharge, that over-corrected the pre-H5 50-190x
    /// over-prediction into a near-total collapse of the band (measured
    /// width ratio ~0 against the checklist's own [0.3,3] floor -- see
    /// VALIDATION.md, PR H5). 0.05 is the calibrated value: small enough that
    /// free-surface behavior (alpha=1) and the original 50-190x defect stay
    /// fixed, large enough that the residual pressurized-friction sensitivity
    /// survives in the band the MC actually shows.
    double alpha_floor = 0.05;
};

/**
 * @brief Per-active-node smooth Manning-sensitivity attenuation factor.
 *
 * `alpha_n = max(alpha_floor, clamp((ramp_hi - ratio_n) / (ramp_hi -
 * ramp_lo), 0, 1))`, where `ratio_n = (head_n - invert_n) / (crown_elev_n -
 * invert_n)`. alpha_n == 1 well below crown (unattenuated — bit-identical to
 * pre-H5 behavior), == alpha_floor well above crown (attenuated, not
 * eliminated — see the config field's own doc comment for why a hard 0 is
 * wrong), linear ramp in between.
 *
 * Degenerate guard: a node whose crown_depth (`crown_elev - invert_elev`) is
 * <= 1e-6 (outfalls, or any node without a classifiable crown) cannot be
 * ramped and is left at alpha == 1.0 — fail open, never silently attenuate a
 * node that can't be classified.
 *
 * @param n_active       Length of active_to_full and alpha_out.
 * @param active_to_full Active-index -> full-node-index map, length
 *                        n_active (e.g. the engine's rom1d_active_map_).
 * @param crown_elev     Full-node-space crown elevation, length n_nodes_full.
 * @param invert_elev    Full-node-space invert elevation, length n_nodes_full.
 * @param head           Full-node-space current head, length n_nodes_full.
 * @param n_nodes_full    Length of crown_elev/invert_elev/head.
 * @param cfg            Ramp band + floor (defaults [0.9, 1.1], floor 0.05).
 * @param alpha_out      Output buffer, length n_active. Out-of-range indices
 *                        in active_to_full (< 0 or >= n_nodes_full) write
 *                        alpha == 1.0 (fail open, same as the degenerate
 *                        crown_depth guard).
 */
inline void computeSurchargeAlpha(int n_active, const int* active_to_full,
                                   const double* crown_elev,
                                   const double* invert_elev,
                                   const double* head, int n_nodes_full,
                                   const SurchargeAttenuationConfig& cfg,
                                   double* alpha_out) noexcept {
    const double band = cfg.ramp_hi - cfg.ramp_lo;
    for (int ai = 0; ai < n_active; ++ai) {
        const int ui = active_to_full[ai];
        if (ui < 0 || ui >= n_nodes_full) {
            alpha_out[ai] = 1.0;
            continue;
        }
        const double crown_depth = crown_elev[ui] - invert_elev[ui];
        if (crown_depth <= 1.0e-6) {
            alpha_out[ai] = 1.0;
            continue;
        }
        const double depth = head[ui] - invert_elev[ui];
        const double ratio = depth / crown_depth;
        double a = (band > 0.0) ? (cfg.ramp_hi - ratio) / band : (ratio < cfg.ramp_lo ? 1.0 : 0.0);
        a = std::clamp(a, 0.0, 1.0);
        alpha_out[ai] = std::max(a, cfg.alpha_floor);
    }
}

}  // namespace openswmm::uncertainty

#endif  // OPENSWMM_ENGINE_UNCERTAINTY_ROM_SURCHARGE_ATTENUATION_HPP
