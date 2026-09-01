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
 * @file HeatOverrides.hpp
 * @brief Plan PE — resolve per-element attributes, and read them.
 *
 * @details Two accessors and one resolver. Everything else about the
 *          feature is parsing (HeatComponent.cpp) or threading
 *          (the flux evaluators).
 *
 * @ingroup engine_transport
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_TRANSPORT_HEAT_OVERRIDES_HPP
#define OPENSWMM_ENGINE_TRANSPORT_HEAT_OVERRIDES_HPP

#include <string>
#include <vector>

#include "../../../data/HeatData.hpp"

namespace openswmm {
struct SimulationContext;
}

namespace openswmm::transport::heat {

/**
 * @brief Materialise the dense per-element vectors from the parsed rows.
 *
 * @details Called ONCE from `SWMMEngine::open`, after every process
 *          component has applied — the D-RQ1 timing, so `link_names`,
 *          `node_names` and the `[TAGS]` columns are all complete and an
 *          unknown element name can be reported with the row that named it.
 *
 *          Precedence is applied by CONSTRUCTION rather than by comparison:
 *          rows are swept in scope order (GLOBAL is already in the base
 *          config, then TAG, then element), so a later write simply wins.
 *          A comparison-based resolver would have to encode the ordering a
 *          second time and the two spellings could disagree.
 *
 * @return Diagnostics; non-empty means the model must not open. Unknown
 *         element names are FATAL (D-PE5): a silently ignored override
 *         produces a plausible answer, which is the failure mode lessons
 *         10/20 are about.
 */
std::vector<std::string> resolveHeatOverrides(SimulationContext& ctx);

/**
 * @brief The radiative attributes in force for `e`.
 *
 * @details Returns the GLOBAL config when no override targets this family,
 *          when the element index is out of range, or when the element is
 *          not a kind that carries radiative overrides. **The empty-vector
 *          path returns the very same object the pre-PE engine used**, which
 *          is what makes a non-PE model bit-identical by construction rather
 *          than by tolerance.
 */
const RadiativeConfig& radiativeFor(const SimulationContext& ctx,
                                    const HeatElement& e) noexcept;

/**
 * @brief The bed attributes in force for `e`.
 *
 * @details Bed overrides are LINK-scoped only — the bed zone itself is
 *          conduits only (`BedZoneState`), so a node-scoped bed attribute
 *          would describe a body that does not exist. A NODE element
 *          therefore always reads the global here, and the parser refuses
 *          `NODE` scope on a `[SEDIMENT_EXCHANGE]` row rather than accepting
 *          a row that could never take effect.
 */
const SedimentConfig& sedimentFor(const SimulationContext& ctx,
                                  const HeatElement& e) noexcept;

// The NaN sentinel on `landcover_temp` ("use air temperature") is resolved
// in exactly one place: `netRadiativeFluxOut`, which receives the resolved
// per-element RadiativeConfig and is the only reader of the field. The
// handoff's draft declared a `landcoverTempC` accessor here instead — and no
// call site ever used it, which left the key parsed, validated, serialized
// and physically inert. The check deleted the accessor and moved the
// resolution to the reader.

}  // namespace openswmm::transport::heat

#endif  // OPENSWMM_ENGINE_TRANSPORT_HEAT_OVERRIDES_HPP
