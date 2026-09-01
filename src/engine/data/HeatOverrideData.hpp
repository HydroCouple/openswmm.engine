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
 * @file HeatOverrideData.hpp
 * @brief Plan PE — per-element radiative and bed attributes.
 *
 * @details Shading, sky view, land-cover emissivity and temperature, burial
 *          depth, ground temperature, hyporheic velocity and bed material
 *          are properties of a PLACE. Until PE they were one number for the
 *          whole model, which collapses exactly the spatial signal a heat
 *          model exists to produce: a shaded riparian reach and an exposed
 *          concrete channel shared one shade factor, and a 4 m trunk and a
 *          0.8 m lateral shared one ground depth.
 *
 * @par The element token (D-PE1)
 *      Flux evaluators take a `HeatElement` — a kind and an index. **ARD
 *      cells and LARD parcels deliberately do NOT get their own kind**: both
 *      resolve to their parent LINK. Shading does not vary within one
 *      conduit in any data a modeller can supply, and a finer table would be
 *      one nothing can fill. Do not "improve" this into a per-cell table.
 *
 * @par Resolution is dense-on-demand (D-PE2)
 *      Parsing produces sparse rows. `resolveHeatOverrides` runs once at
 *      open, after every component has applied, and materialises a dense
 *      vector **only if at least one row targets that family**. The step
 *      loop then reads `ov.empty() ? global : ov[i]` — one predictable
 *      branch, no hashing, no search.
 *
 *      Two properties follow, and both are the reason for the choice:
 *      a model with no overrides **allocates nothing and passes the same
 *      global object it passed before PE**, so byte-identity is structural
 *      rather than tested; and the per-step cost of this whole feature on
 *      such a model is one `.empty()` check.
 *
 * @par Precedence (D-PE3)
 *      `GLOBAL` < `TAG` < element. Most specific wins. `TAG` reads SWMM's
 *      own `[TAGS]` section, already parsed and already round-tripped, so
 *      fifty shaded conduits are tagged once rather than listed fifty times.
 *      **This replaces the reference's `<from> <to>` element range**, which
 *      does not map onto a network graph with no linear element ordering.
 *
 * @see plans/transport/PER_ELEMENT_HEAT_ATTRIBUTES_PLAN_2026-09-01.md
 * @ingroup engine_data
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_DATA_HEAT_OVERRIDE_DATA_HPP
#define OPENSWMM_ENGINE_DATA_HEAT_OVERRIDE_DATA_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace openswmm {

/// What kind of element a flux is being evaluated for (D-PE1).
enum class HeatElemKind : std::uint8_t {
    NODE     = 0,
    LINK     = 1,
    SUBCATCH = 2,
    LID      = 3
};

/**
 * @brief Which element a flux evaluator is being called for.
 *
 * @details Default-constructs to an INVALID index deliberately: a binding
 *          that forgets to pass a real element gets the global config (the
 *          accessors treat index < 0 as "no override"), which is the safe
 *          answer, while a gate that pins per-element behaviour still
 *          catches the omission. The alternative — defaulting to index 0 —
 *          would silently give every element the FIRST link's attributes.
 */
struct HeatElement {
    HeatElemKind kind  = HeatElemKind::LINK;
    int          index = -1;

    static HeatElement node(int i) { return {HeatElemKind::NODE, i}; }
    static HeatElement link(int i) { return {HeatElemKind::LINK, i}; }
    static HeatElement subcatch(int i) { return {HeatElemKind::SUBCATCH, i}; }
};

/// Scope of one override row (D-PE3). Ordered by specificity.
enum class HeatScope : std::uint8_t {
    GLOBAL = 0,
    TAG    = 1,
    NODE   = 2,
    LINK   = 3
};

/**
 * @brief Which attribute an override row sets.
 *
 * @details One enum spanning both config structs, so one row type serves
 *          both and the parser, the resolver and the serializer share a
 *          single table. `COUNT_` is load-bearing: the attribute-name table
 *          in `HeatComponent.cpp` static_asserts against it, so adding an
 *          attribute here without teaching the parser and the writer breaks
 *          the build rather than silently dropping the new key on save.
 */
enum class HeatAttr : int {
    // RadiativeConfig
    ALBEDO = 0,
    SHADE_FACTOR,
    SKY_VIEW,
    EMISS_WATER,
    EMISS_LANDCOVER,
    LANDCOVER_TEMP,        ///< NEW (PE2) — NaN means "use air temperature"
    // SedimentConfig
    SED_THERMAL_DIFFUSIVITY,
    SED_SOLUTE_DIFFUSIVITY,
    SED_BED_THICKNESS,
    SED_GROUND_DEPTH,
    SED_GROUND_TEMP,
    SED_HYPORHEIC_VELOCITY,
    SED_DENSITY,
    SED_SPECIFIC_HEAT,
    COUNT_
};

/// True when the attribute belongs to `SedimentConfig` rather than
/// `RadiativeConfig`. One predicate, so the two families cannot drift apart
/// about which struct owns a key.
inline bool isSedimentAttr(HeatAttr a) noexcept {
    return static_cast<int>(a) >= static_cast<int>(HeatAttr::SED_THERMAL_DIFFUSIVITY);
}

/// One parsed override row, kept verbatim so the serializer can render what
/// the user wrote rather than a resolved expansion of it.
struct HeatOverrideRow {
    HeatAttr    attr  = HeatAttr::ALBEDO;
    HeatScope   scope = HeatScope::GLOBAL;
    std::string name;    ///< tag or element name; empty at GLOBAL scope
    double      value = 0.0;
};

// The dense storage that these rows resolve into lives in HeatData.hpp, as
// `HeatOverrideData`, because it holds real `RadiativeConfig` /
// `SedimentConfig` values and those are declared there. Keeping the TOKENS
// here lets BedExchange.hpp and the flux evaluators take a `HeatElement`
// without pulling in the whole heat-config header.

}  // namespace openswmm

#endif  // OPENSWMM_ENGINE_DATA_HEAT_OVERRIDE_DATA_HPP
