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
 * @file HeatOverrides.cpp
 * @brief Plan PE — resolution and lookup. See the header.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "HeatOverrides.hpp"

#include <cstddef>

#include "../../../core/SimulationContext.hpp"

namespace openswmm::transport::heat {

namespace {

/// Write one attribute into a radiative block. Split from the sediment
/// version rather than merged behind a tag: two switches over disjoint enum
/// ranges are clearer than one switch with an unreachable half, and the
/// compiler warns on a missing case in each.
void patchRadiative(RadiativeConfig& c, HeatAttr a, double v) noexcept {
    switch (a) {
        case HeatAttr::ALBEDO:          c.albedo = v; break;
        case HeatAttr::SHADE_FACTOR:    c.shade_factor = v; break;
        case HeatAttr::SKY_VIEW:        c.sky_view = v; break;
        case HeatAttr::EMISS_WATER:     c.emiss_water = v; break;
        case HeatAttr::EMISS_LANDCOVER: c.emiss_landcover = v; break;
        case HeatAttr::LANDCOVER_TEMP:  c.landcover_temp = v; break;
        default: break;   // sediment attrs never reach here (isSedimentAttr)
    }
}

void patchSediment(SedimentConfig& c, HeatAttr a, double v) noexcept {
    switch (a) {
        case HeatAttr::SED_THERMAL_DIFFUSIVITY: c.thermal_diffusivity = v; break;
        case HeatAttr::SED_SOLUTE_DIFFUSIVITY:  c.solute_diffusivity = v; break;
        case HeatAttr::SED_BED_THICKNESS:       c.bed_thickness = v; break;
        case HeatAttr::SED_GROUND_DEPTH:        c.ground_depth = v; break;
        case HeatAttr::SED_GROUND_TEMP:
            c.ground_temp = v;
            // A per-element ground temperature is by definition stated, so
            // it also silences the "you did not set GROUND_TEMPERATURE"
            // warning for models that set it only per element. Without this
            // a fully-specified model is warned about the one thing it was
            // most careful with.
            c.has_ground_temp = true;
            break;
        case HeatAttr::SED_HYPORHEIC_VELOCITY:  c.hyporheic_velocity = v; break;
        case HeatAttr::SED_DENSITY:             c.sed_density = v; break;
        case HeatAttr::SED_SPECIFIC_HEAT:       c.sed_specific_heat = v; break;
        default: break;
    }
}

}  // namespace

std::vector<std::string> resolveHeatOverrides(SimulationContext& ctx) {
    std::vector<std::string> errors;
    auto& ov = ctx.heat_config.overrides;
    ov.resolved = true;
    if (ov.rows.empty()) return errors;   // nothing sized, nothing allocated

    const int nl = ctx.n_links();
    const int nn = ctx.n_nodes();

    // Which families are actually targeted. Sizing only what is used is
    // D-PE2's whole point: a deck that shades three conduits must not
    // allocate a SedimentConfig for every link in the network.
    bool want_rad_link = false, want_rad_node = false, want_sed = false;
    for (const auto& r : ov.rows) {
        if (r.scope == HeatScope::GLOBAL) continue;     // already in the base
        if (isSedimentAttr(r.attr)) {
            want_sed = true;
        } else if (r.scope == HeatScope::NODE) {
            want_rad_node = true;
        } else {
            // TAG rows can land on either kind, so a TAG row arms both.
            want_rad_link = true;
            if (r.scope == HeatScope::TAG) want_rad_node = true;
        }
    }

    if (want_rad_link && nl > 0)
        ov.rad_link.assign(static_cast<std::size_t>(nl),
                           ctx.heat_config.radiative);
    if (want_rad_node && nn > 0)
        ov.rad_node.assign(static_cast<std::size_t>(nn),
                           ctx.heat_config.radiative);
    if (want_sed && nl > 0)
        ov.sed_link.assign(static_cast<std::size_t>(nl),
                           ctx.heat_config.sediment);

    // Two passes, in specificity order (D-PE3). Precedence is a property of
    // the ORDER, not of a comparison: TAG writes first, element writes
    // second and simply wins. Encoding it as a comparison would state the
    // ordering twice and let the two spellings disagree.
    for (int pass = 0; pass < 2; ++pass) {
        const HeatScope want = (pass == 0) ? HeatScope::TAG : HeatScope::NODE;
        for (const auto& r : ov.rows) {
            if (r.scope == HeatScope::GLOBAL) continue;
            const bool is_elem = (r.scope == HeatScope::NODE ||
                                  r.scope == HeatScope::LINK);
            if (pass == 0 && r.scope != HeatScope::TAG) continue;
            if (pass == 1 && !is_elem) continue;
            (void)want;

            if (r.scope == HeatScope::TAG) {
                bool hit = false;
                for (int j = 0; j < nl; ++j) {
                    const auto uj = static_cast<std::size_t>(j);
                    if (uj >= ctx.links.tags.size() ||
                        ctx.links.tags[uj] != r.name)
                        continue;
                    hit = true;
                    if (isSedimentAttr(r.attr)) {
                        if (uj < ov.sed_link.size())
                            patchSediment(ov.sed_link[uj], r.attr, r.value);
                    } else if (uj < ov.rad_link.size()) {
                        patchRadiative(ov.rad_link[uj], r.attr, r.value);
                    }
                }
                if (!isSedimentAttr(r.attr)) {
                    for (int i = 0; i < nn; ++i) {
                        const auto ui = static_cast<std::size_t>(i);
                        if (ui >= ctx.nodes.tags.size() ||
                            ctx.nodes.tags[ui] != r.name)
                            continue;
                        hit = true;
                        if (ui < ov.rad_node.size())
                            patchRadiative(ov.rad_node[ui], r.attr, r.value);
                    }
                }
                // A tag that matches nothing is a typo with the same
                // consequence as a bad element name — the row silently does
                // nothing — so it is fatal for the same reason (D-PE5).
                if (!hit)
                    errors.push_back(
                        "model.heat: TAG '" + r.name + "' matches no link or "
                        "node, so this override would silently do nothing. "
                        "Check [TAGS].");
                continue;
            }

            if (r.scope == HeatScope::LINK) {
                const int j = ctx.link_names.find(r.name);
                if (j < 0) {
                    errors.push_back("model.heat: unknown link '" + r.name +
                                     "' in a per-element heat override.");
                    continue;
                }
                const auto uj = static_cast<std::size_t>(j);
                if (isSedimentAttr(r.attr)) {
                    if (uj < ov.sed_link.size())
                        patchSediment(ov.sed_link[uj], r.attr, r.value);
                } else if (uj < ov.rad_link.size()) {
                    patchRadiative(ov.rad_link[uj], r.attr, r.value);
                }
                continue;
            }

            // NODE scope. Sediment attributes are refused at parse (the bed
            // is conduits only), so only radiative rows reach here.
            const int i = ctx.node_names.find(r.name);
            if (i < 0) {
                errors.push_back("model.heat: unknown node '" + r.name +
                                 "' in a per-element heat override.");
                continue;
            }
            const auto ui = static_cast<std::size_t>(i);
            if (ui < ov.rad_node.size())
                patchRadiative(ov.rad_node[ui], r.attr, r.value);
        }
    }
    return errors;
}

const RadiativeConfig& radiativeFor(const SimulationContext& ctx,
                                    const HeatElement& e) noexcept {
    const auto& ov = ctx.heat_config.overrides;
    if (e.index >= 0) {
        const auto ui = static_cast<std::size_t>(e.index);
        if (e.kind == HeatElemKind::LINK && ui < ov.rad_link.size())
            return ov.rad_link[ui];
        if (e.kind == HeatElemKind::NODE && ui < ov.rad_node.size())
            return ov.rad_node[ui];
    }
    // SUBCATCH and LID have no radiative override table: a subcatchment's
    // shading is a watershed property this program models through its own
    // land-cover inputs, and a LID layer is below the surface. Both read the
    // global, which is what they did before PE.
    return ctx.heat_config.radiative;
}

const SedimentConfig& sedimentFor(const SimulationContext& ctx,
                                  const HeatElement& e) noexcept {
    const auto& ov = ctx.heat_config.overrides;
    if (e.kind == HeatElemKind::LINK && e.index >= 0) {
        const auto ui = static_cast<std::size_t>(e.index);
        if (ui < ov.sed_link.size()) return ov.sed_link[ui];
    }
    return ctx.heat_config.sediment;
}

}  // namespace openswmm::transport::heat
