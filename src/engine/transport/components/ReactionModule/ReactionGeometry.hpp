// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../../../core/SimulationContext.hpp"
#include "../../../hydraulics/Link.hpp"
#include "../../../hydraulics/XSectBatch.hpp"
#include <algorithm>

namespace openswmm::transport {
inline double reactionAreaFactor(ReactionAreaUnits units) {
    return units == ReactionAreaUnits::M2 ? .09290304 : units == ReactionAreaUnits::CM2 ? 929.0304 : 1.;
}
// Contact area is explicit for tanks and non-conduit links. Conduits use the
// wetted perimeter of the actual section (including tabulated sections),
// physical routing length and barrel count. No tank shape is inferred.
inline double reactionWallArea(const SimulationContext &c, bool link_element, int e, bool reference = false) {
    for (const auto &row : c.reactions.wall_areas)
        if (row.link == link_element && row.element == e)
            return row.area;
    if (!link_element || e < 0 || e >= c.n_links())
        return 0;
    const int r = c.link_subtypes.conduit_row(e);
    if (r < 0)
        return 0;
    const double y = reference ? c.links.xsect_y_full[e] : c.links.depth[e];
    if (!(y > 0))
        return 0;
    const auto xs = link::buildXSectParams(c.links, e, &c.transect_tables);
    const double area = xsect::getAofY(xs, y), radius = xsect::getRofY(xs, y);
    if (!(radius > 0))
        return 0;
    const auto &conduits = c.link_subtypes.conduits;
    const double length = r < int(conduits.true_length.size()) && conduits.true_length[r] > 0
                              ? conduits.true_length[r]
                              : conduits.length[r];
    return area / radius * length * std::max(1, conduits.barrels[r]) *
           reactionAreaFactor(c.reactions.area_units);
}
inline double reactionAreaVolume(const SimulationContext &c, bool link_element, int e, double volume) {
    return volume > .0353147 ? reactionWallArea(c, link_element, e) / (volume * 28.317) : 0.;
}
inline void reactionWallScales(const SimulationContext &c, bool link_element, int e, double volume,
                               std::vector<double> &scales) {
    scales.assign(c.reactions.n_species(), 1.);
    const double reference = reactionWallArea(c, link_element, e, true);
    const double active = volume > .0353147 ? reactionWallArea(c, link_element, e) : 0;
    for (int s = 0; s < c.reactions.n_species(); ++s)
        if (c.reactions.species_is_wall[s])
            scales[s] = reference > 0 ? std::clamp(active / reference, 0., 1.) : 0.;
}
} // namespace openswmm::transport
