// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../../../core/SimulationContext.hpp"
#include <algorithm>
#include <cmath>
namespace openswmm::transport {
inline double reactionSourceFactor(const ReactionData &r, const ReactionData::Source &source,
                                   double seconds) {
    if (source.pattern < 0)
        return 1.;
    const auto &factors = r.source_patterns[source.pattern].factors;
    const double count = double(factors.size()), period = r.pattern_timestep * count;
    const double phase =
        std::isfinite(period) ? std::fmod(std::max(0., seconds), period) : std::max(0., seconds);
    const double epsilon = std::min(1e-6, r.pattern_timestep * 1e-6);
    return factors[std::size_t(std::fmod(std::floor((phase + epsilon) / r.pattern_timestep), count))];
}
// FLOWPACED doses arriving water once. SETPOINT raises the complete mixed
// store only during positive water exchange. Neither injects into a dry store
// or removes mass when the mixture is above its set point.
inline double reactionBoosterMass(SimulationContext &ctx, int node, int species, double mass,
                                  double mixed_volume, double arriving_volume) {
    if (!(mixed_volume > 1e-12) || !(arriving_volume > 1e-12))
        return 0.;
    auto &r = ctx.reactions;
    for (const auto &source : r.sources)
        if (source.booster && source.node == node && source.species == species) {
            const double strength = source.value * reactionSourceFactor(r, source, ctx.current_time);
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
