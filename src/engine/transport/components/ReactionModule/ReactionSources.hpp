// SPDX-License-Identifier: Apache-2.0

/**
 * @file      ReactionSources.hpp
 * @brief     Resolution and application of species reaction source terms.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include "../../../core/SimulationContext.hpp"
#include <algorithm>
#include <cmath>
namespace openswmm::transport {
/**
 * @brief Read a periodic reaction-source pattern factor at an elapsed simulation time.
 * @param[in] r Stable source_patterns, positive finite pattern_timestep seconds and nonempty
 * factor arrays.
 * @param[in] source Resolved source; pattern<0 means unpatterned, otherwise valid pattern index.
 * @param[in] seconds Finite elapsed seconds; negatives floor0 before phase wrap.
 * @return Pattern multiplier, or1 without pattern. Uses tiny timestep-scaled epsilon at
 * boundaries; periodic floor index wraps factor count. No shape/finite validation, source
 * consumption, allocation or clock mutation; empty factors/nonpositive timestep violate
 * preconditions.
 */
inline double reactionSourceFactor(const ReactionData& r, const ReactionData::Source& source,
                                   double seconds) {
    if (source.pattern < 0)
        return 1.;
    const auto& factors = r.source_patterns[source.pattern].factors;
    const double count = double(factors.size()), period = r.pattern_timestep * count;
    const double phase =
        std::isfinite(period) ? std::fmod(std::max(0., seconds), period) : std::max(0., seconds);
    const double epsilon = std::min(1e-6, r.pattern_timestep * 1e-6);
    return factors[std::size_t(
        std::fmod(std::floor((phase + epsilon) / r.pattern_timestep), count))];
}
// FLOWPACED doses arriving water once. SETPOINT raises the complete mixed
// store only during positive water exchange. Neither injects into a dry store
// or removes mass when the mixture is above its set point.
/**
 * @brief Compute and book the first matching node/species booster inventory addition.
 * @param[in,out] ctx Resolved reaction source patterns and current elapsed clock (read only);
 * updates booster_mass and armed MSX inflow ledger, never physical concentration directly.
 * @param[in] node Native node ID matched against source rows.
 * @param[in] species Valid native species row with covered ledger arrays.
 * @param[in] mass Existing mixed native concentration*ft3 inventory.
 * @param[in] mixed_volume Total mixed water ft3; <=1e-12/NaN returns0.
 * @param[in] arriving_volume Positive arriving water ft3; <=1e-12/NaN returns0 even for
 * setpoint.
 * @return FLOWPACED strength*arrival or SETPOINT max(strength*mixed-existing,0),
 * concentration*ft3; 0 if no matching nonzero booster. Pattern/source strength is not
 * finite/sign-clamped here.
 * @note May resize/reset booster totals when shape differs. Books only first matching booster,
 * no water/clock advance; caller adds returned inventory exactly once. Allocation can partially
 * reset, exclusive context/ledger ownership required.
 */
inline double reactionBoosterMass(SimulationContext& ctx, int node, int species, double mass,
                                  double mixed_volume, double arriving_volume) {
    if (!(mixed_volume > 1e-12) || !(arriving_volume > 1e-12))
        return 0.;
    auto& r = ctx.reactions;
    for (const auto& source : r.sources)
        if (source.booster && source.node == node && source.species == species) {
            const double strength =
                source.value * reactionSourceFactor(r, source, ctx.current_time);
            const double added = source.booster == 1 ? strength * arriving_volume
                                                     : std::max(0., strength * mixed_volume - mass);
            if (r.booster_mass.size() != std::size_t(r.n_species()))
                r.booster_mass.assign(r.n_species(), 0.);
            r.booster_mass[species] += added;
            if (r.msx_ledger.armed)
                r.msx_ledger.inflow[species] += added;
            return added;
        }
    return 0.;
}
} // namespace openswmm::transport
