/**
 * @file      AquiferLinkExchange.hpp
 * @brief     G-X4 (2026-09-20) — the signed conduit ⇄ two-zone-aquifer
 *        conductance law, shared by the two places that compute a conduit's
 *        seepage rate (DynamicWave's fused kernel and
 *        routing::computeConduitLosses for KINWAVE / STEADY / FV).
 *
 * @details Legacy SWMM gives a conduit a one-way seepage LOSS at a fixed
 *          rate: `q = K · W · L`, an assumed unit gradient, whatever the
 *          groundwater does. With a `[2D_AQUIFER]` under the conduit and
 *          `LINK_SEEPAGE TWO_WAY`, that becomes a MODFLOW-River exchange
 *          about the conduit's own invert:
 *
 *              h_link = z_inv + depth                     (water surface)
 *              h_eff  = max(h_gw, z_inv)                  (bottom clamp)
 *              Q      = K · W · L · min((h_link − h_eff)/d_c, 1)
 *
 *          `+` is out of the pipe, the sign the whole engine already uses
 *          for `seep_loss_rate`, so a gaining reach is simply a negative
 *          seepage. The three properties that matter:
 *
 *          1. **It reproduces legacy exactly where legacy is right.** With
 *             the table at or below the invert and the pipe full
 *             (`depth ≥ d_c`, whose default IS the conduit's full depth),
 *             the factor is 1 and the rate is the legacy `K · W · L` to the
 *             bit. This is MODFLOW's disconnected-stream limit: once the
 *             aquifer head falls below the bed, the leakage saturates.
 *          2. **It is continuous through the water table.** A rising table
 *             throttles the loss smoothly to zero and then reverses.
 *          3. **The gain is bounded by the aquifer, not by the pipe.**
 *             `gw_gain_max` is the aquifer's frozen per-step share of the
 *             drainable water in the cells the conduit crosses, so the 1D
 *             can never take water the 2D does not have — the same
 *             budget-then-deliver rule the node exchange uses (G-X1).
 *
 *          The publisher (SurfaceRouter2D, before routing) owns the datum:
 *          it hands over the table elevation already relative to the
 *          conduit's mid invert, so neither loss site needs node inverts or
 *          the mesh datum.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_HYDRAULICS_AQUIFER_LINK_EXCHANGE_HPP
#define OPENSWMM_ENGINE_HYDRAULICS_AQUIFER_LINK_EXCHANGE_HPP

#include <algorithm>
#include <cstddef>

#include "../core/SimulationContext.hpp"

namespace openswmm::hydraulics {

/**
 * @brief Compute the signed conductance multiplier for conduit-to-aquifer seepage.
 *
 * Does not bound gaining magnitude or reject nonfinite inputs; caller applies donor/gain caps
 * separately. All three lengths may share another consistent unit without changing the
 * dimensionless result.
 *
 * @param[in] depth Conduit water depth, normally native ft.
 * @param[in] gw_head_rel Aquifer table elevation relative to conduit mid-invert in matching length
 *     units; negative means below bed.
 * @param[in] d_c Positive characteristic bed path length in matching units; nonpositive or NaN
 *     selects legacy factor1.
 * @return min((depth-max(gw_head_rel,0))/d_c,1); positive loses conduit water, negative gains it.
 */
inline double aquiferSeepFactor(double depth, double gw_head_rel, double d_c) noexcept {
    if (!(d_c > 0.0))
        return 1.0; // no path length: legacy
    const double h_eff = (gw_head_rel > 0.0) ? gw_head_rel : 0.0;
    return std::min((depth - h_eff) / d_c, 1.0);
}

/**
 * @brief Limit the gaining direction of a signed conduit seepage rate.
 *
 * No finite/nonnegative validation or inventory booking; caller supplies a physically valid cap.
 *
 * @param[in] seep_rate Signed cfs for one barrel: positive conduit loss, negative aquifer gain.
 * @param[in] gain_max Nonnegative available aquifer gain cap in the same cfs/barrel convention.
 * @return max(seep_rate,-gain_max); positive loss is not limited.
 */
inline double capAquiferGain(double seep_rate, double gain_max) noexcept {
    return (seep_rate < -gain_max) ? -gain_max : seep_rate;
}

/**
 * @brief Apply the registered host seepage override or addition to a conduit rate.
 *
 * Other modes leave the rate unchanged. Does not validate values, enforce donor/gain caps or
 * consume SINGLE forcing; routing invokes it after physical caps. Caller serializes forcing edits
 * and evaluation.
 *
 * @param[in] ctx Borrowed aligned forcing mode/value vectors, stored in native cfs.
 * @param[in] link Zero-based global link slot; out-of-mode-vector range is a no-op.
 * @param[in,out] seep_rate Signed native cfs rate, positive out of conduit; OVERRIDE replaces and
 *     ADD increments.
 */
inline void applySeepageForcing(const SimulationContext& ctx, std::size_t link,
                                double& seep_rate) noexcept {
    const auto& f = ctx.forcing;
    if (link >= f.link_seepage_mode.size())
        return;
    switch (f.link_seepage_mode[link]) {
    case ForcingMode::OVERRIDE:
        seep_rate = f.link_seepage_value[link];
        break;
    case ForcingMode::ADD:
        seep_rate += f.link_seepage_value[link];
        break;
    default:
        break;
    }
}

} // namespace openswmm::hydraulics

#endif // OPENSWMM_ENGINE_HYDRAULICS_AQUIFER_LINK_EXCHANGE_HPP
