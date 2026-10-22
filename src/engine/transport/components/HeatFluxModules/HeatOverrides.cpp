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
 * @file      HeatOverrides.cpp
 * @brief     Plan PE — resolution and lookup. See the header.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "HeatOverrides.hpp"

#include <cstddef>
#include <charconv>

#include "../../../core/SimulationContext.hpp"
#ifdef OPENSWMM_HAS_2D
#include "../../../2d/data/MeshData.hpp"
#endif

namespace openswmm::transport::heat {

namespace {

/**
 * @brief Write one attribute into a radiative block. Split from the sediment version rather than
 * merged behind a tag: two switches over disjoint enum ranges are clearer than one switch with
 * an unreachable half, and the compiler warns on a missing case in each.
 *
 * @details Assign one retained radiative override attribute to a resolved candidate block.
 * @param[in,out] c Mutable resolved radiative config, changed only for supported radiative
 * attribute.
 * @param[in] a Attribute selector; unrelated sediment/unknown keys ignored.
 * @param[in] v Already validated dimensionless fraction/emissivity or land-cover degC value,
 * verbatim.
 * @note No range/finite checks, allocation, weather refresh or physical/clock mutation.
 */
void patchRadiative(RadiativeConfig& c, HeatAttr a, double v) noexcept {
    switch (a) {
    case HeatAttr::ALBEDO:
        c.albedo = v;
        break;
    case HeatAttr::SHADE_FACTOR:
        c.shade_factor = v;
        break;
    case HeatAttr::SKY_VIEW:
        c.sky_view = v;
        break;
    case HeatAttr::EMISS_WATER:
        c.emiss_water = v;
        break;
    case HeatAttr::EMISS_LANDCOVER:
        c.emiss_landcover = v;
        break;
    case HeatAttr::LANDCOVER_TEMP:
        c.landcover_temp = v;
        break;
    default:
        break; // sediment attrs never reach here (isSedimentAttr)
    }
}

/**
 * @brief Assign one retained sediment override attribute to a resolved candidate block.
 * @param[in,out] c Resolved sediment config, progressively patched.
 * @param[in] a Sediment attribute selector; unknown/unrelated ignored.
 * @param[in] v Already validated SI value (m2/s diffusivity, m thickness/depth, degC ground, m/s
 * hyporheic, kg/m3density, J/kg/Kspecific heat, dimensionless porosity/source selector).
 * @note Ground temperature also marks explicit constant, ground source maps nonzero to
 * mesh_ground. No conversion/range/finite validation, allocation or physical/clock update.
 */
void patchSediment(SedimentConfig& c, HeatAttr a, double v) noexcept {
    switch (a) {
    case HeatAttr::SED_THERMAL_DIFFUSIVITY:
        c.thermal_diffusivity = v;
        break;
    case HeatAttr::SED_SOLUTE_DIFFUSIVITY:
        c.solute_diffusivity = v;
        break;
    case HeatAttr::SED_BED_THICKNESS:
        c.bed_thickness = v;
        break;
    case HeatAttr::SED_GROUND_DEPTH:
        c.ground_depth = v;
        break;
    case HeatAttr::SED_GROUND_TEMP:
        c.ground_temp = v;
        // A per-element ground temperature is by definition stated, so
        // it also silences the "you did not set GROUND_TEMPERATURE"
        // warning for models that set it only per element. Without this
        // a fully-specified model is warned about the one thing it was
        // most careful with.
        c.has_ground_temp = true;
        break;
    case HeatAttr::SED_HYPORHEIC_VELOCITY:
        c.hyporheic_velocity = v;
        break;
    case HeatAttr::SED_DENSITY:
        c.sed_density = v;
        break;
    case HeatAttr::SED_SPECIFIC_HEAT:
        c.sed_specific_heat = v;
        break;
    case HeatAttr::SED_MESH_POROSITY:
        c.mesh_porosity = v;
        break;
    case HeatAttr::SED_GROUND_SOURCE:
        c.mesh_ground = v != 0;
        break;
    default:
        break;
    }
}

} // namespace

/**
 * @brief Materialize retained per-element/tag/mesh heat attributes in dense lookup vectors.
 * @param[in,out] ctx Fully parsed matching native names/tags and optional mesh, global
 * radiative/ sediment config and retained override rows; receives resolved flag and dense family
 * vectors.
 * @return Owned missing element/tag/invalid one-based CELL2D/no-2D-build diagnostics, empty on
 * these resolution checks succeeding. Does not itself validate every attribute value or runtime
 * heat profile.
 * @note Sets resolved=true before work, even on diagnostic return. Allocates only
 * families/scopes requested by current rows using global base, applies MESH_TAG then CELL2D;
 * native TAG then explicit NODE/LINK with later row wins. GLOBAL already incorporated by parser
 * and is skipped. Native tag matches exact strings; sediment TAG searches links only, radiative
 * TAG links+nodes. CELL2D string strictly parses positive one-based index then stores dense
 * zero-based lookup. Does not clear unused/stale dense caches when rows removed or empty; caller
 * resets/invalidates those through authoring lifecycle before re-resolution. Allocation can
 * leave partial caches and resolved flag, no rollback. No physical water/heat/native
 * inventory/clock advance; exclusive setup and all borrowed model identity vectors stable
 * through call.
 */
std::vector<std::string> resolveHeatOverrides(SimulationContext& ctx) {
    std::vector<std::string> errors;
    auto& ov = ctx.heat_config.overrides;
    ov.resolved = true;
    if (ov.rows.empty())
        return errors; // nothing sized, nothing allocated

    const int nl = ctx.n_links();
    const int nn = ctx.n_nodes();

    // Which families are actually targeted. Sizing only what is used is
    // D-PE2's whole point: a deck that shades three conduits must not
    // allocate a SedimentConfig for every link in the network.
    bool want_rad_link = false, want_rad_node = false, want_sed = false;
    bool want_rad_cell = false, want_sed_cell = false;
    for (const auto& r : ov.rows) {
        if (r.scope == HeatScope::GLOBAL)
            continue; // already in the base
        if (r.scope == HeatScope::CELL2D || r.scope == HeatScope::MESH_TAG) {
            if (isSedimentAttr(r.attr))
                want_sed_cell = true;
            else
                want_rad_cell = true;
            continue;
        }
        if (isSedimentAttr(r.attr)) {
            want_sed = true;
        } else if (r.scope == HeatScope::NODE) {
            want_rad_node = true;
        } else {
            // TAG rows can land on either kind, so a TAG row arms both.
            want_rad_link = true;
            if (r.scope == HeatScope::TAG)
                want_rad_node = true;
        }
    }

    if (want_rad_link && nl > 0)
        ov.rad_link.assign(static_cast<std::size_t>(nl), ctx.heat_config.radiative);
    if (want_rad_node && nn > 0)
        ov.rad_node.assign(static_cast<std::size_t>(nn), ctx.heat_config.radiative);
    if (want_sed && nl > 0)
        ov.sed_link.assign(static_cast<std::size_t>(nl), ctx.heat_config.sediment);

    if (want_rad_cell || want_sed_cell) {
#ifdef OPENSWMM_HAS_2D
        const auto* mesh = ctx.twod_io.mesh;
        const int nc = mesh ? mesh->n_cells() : 0;
        if (want_rad_cell)
            ov.rad_cell.assign(nc, ctx.heat_config.radiative);
        if (want_sed_cell)
            ov.sed_cell.assign(nc, ctx.heat_config.sediment);
        for (const auto scope : {HeatScope::MESH_TAG, HeatScope::CELL2D})
            for (const auto& r : ov.rows) {
                if (r.scope != scope)
                    continue;
                int cell = -1;
                if (scope == HeatScope::CELL2D) {
                    const auto parsed =
                        std::from_chars(r.name.data(), r.name.data() + r.name.size(), cell);
                    if (parsed.ec != std::errc{} || parsed.ptr != r.name.data() + r.name.size() ||
                        cell < 1 || cell > nc) {
                        errors.push_back("model.heat: CELL2D '" + r.name +
                                         "' is not a valid 1-based mesh cell.");
                        continue;
                    }
                    --cell;
                }
                bool hit = false;
                for (int i = 0; i < nc; ++i) {
                    if (scope == HeatScope::CELL2D ? i != cell
                                                   : (std::size_t(i) >= mesh->tri_tag.size() ||
                                                      mesh->tri_tag[i] != r.name))
                        continue;
                    hit = true;
                    if (isSedimentAttr(r.attr))
                        patchSediment(ov.sed_cell[i], r.attr, r.value);
                    else
                        patchRadiative(ov.rad_cell[i], r.attr, r.value);
                }
                if (!hit)
                    errors.push_back("model.heat: MESH_TAG '" + r.name +
                                     "' matches no mesh region.");
            }
#else
        errors.push_back("model.heat: mesh overrides require a 2D-enabled engine.");
#endif
    }

    // Two passes, in specificity order (D-PE3). Precedence is a property of
    // the ORDER, not of a comparison: TAG writes first, element writes
    // second and simply wins. Encoding it as a comparison would state the
    // ordering twice and let the two spellings disagree.
    for (int pass = 0; pass < 2; ++pass) {
        const HeatScope want = (pass == 0) ? HeatScope::TAG : HeatScope::NODE;
        for (const auto& r : ov.rows) {
            if (r.scope == HeatScope::GLOBAL)
                continue;
            const bool is_elem = (r.scope == HeatScope::NODE || r.scope == HeatScope::LINK);
            if (pass == 0 && r.scope != HeatScope::TAG)
                continue;
            if (pass == 1 && !is_elem)
                continue;
            (void)want;

            if (r.scope == HeatScope::TAG) {
                bool hit = false;
                for (int j = 0; j < nl; ++j) {
                    const auto uj = static_cast<std::size_t>(j);
                    if (uj >= ctx.links.tags.size() || ctx.links.tags[uj] != r.name)
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
                        if (ui >= ctx.nodes.tags.size() || ctx.nodes.tags[ui] != r.name)
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
                    errors.push_back("model.heat: TAG '" + r.name +
                                     "' matches no link or "
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

/**
 * @brief Borrow resolved element radiation parameters with global fallback.
 * @param[in] ctx Stable already-resolved dense radiative overrides and global base, alive
 * through use.
 * @param[in] e Element kind/index; nonnegative in-range LINK/NODE/CELL2D selects corresponding
 * vector.
 * @return Nonowned const reference to matching dense row, else exact global config object.
 * Invalid/ unsupported kinds/indices fall back, no diagnostic. Reference invalidated by
 * context/cache edits, resolution/move/destruction.
 * @note Does not inspect resolved flag, rebuild cache, allocate, validate, sample forcing or
 * mutate physical state/clock. Caller ensures prepared cache is current and stable, serial
 * authoring access.
 */
const RadiativeConfig& radiativeFor(const SimulationContext& ctx, const HeatElement& e) noexcept {
    const auto& ov = ctx.heat_config.overrides;
    if (e.index >= 0) {
        const auto ui = static_cast<std::size_t>(e.index);
        if (e.kind == HeatElemKind::LINK && ui < ov.rad_link.size())
            return ov.rad_link[ui];
        if (e.kind == HeatElemKind::NODE && ui < ov.rad_node.size())
            return ov.rad_node[ui];
        if (e.kind == HeatElemKind::CELL2D && ui < ov.rad_cell.size())
            return ov.rad_cell[ui];
    }
    // SUBCATCH and LID have no radiative override table: a subcatchment's
    // shading is a watershed property this program models through its own
    // land-cover inputs, and a LID layer is below the surface. Both read the
    // global, which is what they did before PE.
    return ctx.heat_config.radiative;
}

/**
 * @brief Borrow resolved conduit/mesh-cell sediment parameters with global fallback.
 * @param[in] ctx Stable already-resolved dense sediment overrides/global base, alive through
 * use.
 * @param[in] e Nonnegative in-range LINK or CELL2D selects matching dense row;
 * NODE/global/invalid falls back to global block.
 * @return Nonowned const config reference; cache/context replacement/resolution invalidates
 * views.
 * @note No cache rebuild/resolved-flag test, allocation, validation or physical/clock mutation.
 * Caller qualifies family applicability/ownership and retains stable prepared lookup during use.
 */
const SedimentConfig& sedimentFor(const SimulationContext& ctx, const HeatElement& e) noexcept {
    const auto& ov = ctx.heat_config.overrides;
    if (e.kind == HeatElemKind::CELL2D && e.index >= 0 && std::size_t(e.index) < ov.sed_cell.size())
        return ov.sed_cell[e.index];
    if (e.kind == HeatElemKind::LINK && e.index >= 0) {
        const auto ui = static_cast<std::size_t>(e.index);
        if (ui < ov.sed_link.size())
            return ov.sed_link[ui];
    }
    return ctx.heat_config.sediment;
}

} // namespace openswmm::transport::heat
