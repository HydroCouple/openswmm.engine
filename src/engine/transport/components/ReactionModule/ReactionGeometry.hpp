// SPDX-License-Identifier: Apache-2.0

/**
 * @file      ReactionGeometry.hpp
 * @brief     Geometric quantities required by reaction components.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include "../../../core/SimulationContext.hpp"
#include "../../../hydraulics/Link.hpp"
#include "../../../hydraulics/XSectBatch.hpp"
#include <algorithm>

namespace openswmm::transport {
/**
 * @brief Convert native square feet to configured reaction wall/dry-store area units.
 * @param[in] units M2, CM2 or FT2/other selector.
 * @return .09290304 for m2/ft2,929.0304 for cm2/ft2, otherwise1. Pure/allocation-free.
 */
inline double reactionAreaFactor(ReactionAreaUnits units) {
    return units == ReactionAreaUnits::M2    ? .09290304
           : units == ReactionAreaUnits::CM2 ? 929.0304
                                             : 1.;
}
// Contact area is explicit for tanks and non-conduit links. Conduits use the
// wetted perimeter of the actual section (including tabulated sections),
// physical routing length and barrel count. No tank shape is inferred.
/**
 * @brief Resolve explicit or hydraulic wetted reaction wall area for an element.
 * @param[in] c Stable wall override rows and prepared native conduit geometry/subtype tables.
 * @param[in] link_element True identifies a link, false a node.
 * @param[in] e Native element ID; explicit matching override returned before generic validity
 * check.
 * @param[in] reference True uses full section depth, false accepted current link depth. Explicit
 * area overrides use same fixed value for reference and active requests.
 * @return Declared AREA_UNITS area; first explicit row wins verbatim, otherwise conduit wetted
 * perimeter*true/nominal length*max(barrels,1) converted from ft2. No implicit node geometry;
 * returns0 for absent/nonconduit/invalid/nonpositive depth or radius. No finite/range repair,
 * allocation or mutation.
 */
inline double reactionWallArea(const SimulationContext& c, bool link_element, int e,
                               bool reference = false) {
    for (const auto& row : c.reactions.wall_areas)
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
    const auto& conduits = c.link_subtypes.conduits;
    const double length = r < int(conduits.true_length.size()) && conduits.true_length[r] > 0
                              ? conduits.true_length[r]
                              : conduits.length[r];
    return area / radius * length * std::max(1, conduits.barrels[r]) *
           reactionAreaFactor(c.reactions.area_units);
}
/**
 * @brief Compute active wetted reaction wall area per litre of water.
 * @param[in] c Stable prepared wall/geometry configuration.
 * @param[in] link_element True link, false node identity.
 * @param[in] e Valid element ID for geometry resolution.
 * @param[in] volume Native water ft3; only>.0353147 triggers geometry lookup.
 * @return Declared area units/L using28.317L/ft3, else0. No state/clock change or physical
 * validation.
 */
inline double reactionAreaVolume(const SimulationContext& c, bool link_element, int e,
                                 double volume) {
    return volume > .0353147 ? reactionWallArea(c, link_element, e) / (volume * 28.317) : 0.;
}
/**
 * @brief Build per-species active-to-reference wall coverage multipliers.
 * @param[in] c Stable configured species wall flags and prepared wall/geometry arrays.
 * @param[in] link_element True selects link identity, false node.
 * @param[in] e Valid native element ID.
 * @param[in] volume Water ft3; <=one litre/NaN uses active area0.
 * @param[out] scales Resized/reset n_species to1 for BULK and clamp(active/reference,0,1) for
 * WALL; reference<=0 gives WALL0. Owns no geometry pointers after return.
 * @note Allocation can throw; no physical/clock mutation. Caller owns exclusive output and
 * stable inputs.
 */
inline void reactionWallScales(const SimulationContext& c, bool link_element, int e, double volume,
                               std::vector<double>& scales) {
    scales.assign(c.reactions.n_species(), 1.);
    const double reference = reactionWallArea(c, link_element, e, true);
    const double active = volume > .0353147 ? reactionWallArea(c, link_element, e) : 0;
    for (int s = 0; s < c.reactions.n_species(); ++s)
        if (c.reactions.species_is_wall[s])
            scales[s] = reference > 0 ? std::clamp(active / reference, 0., 1.) : 0.;
}
} // namespace openswmm::transport
