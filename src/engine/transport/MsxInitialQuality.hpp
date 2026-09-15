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
 * @file MsxInitialQuality.hpp
 * @brief U2 (2026-09-07) — [INITIAL_QUALITY] rows for MSX species.
 *
 * @details `[INITIAL_QUALITY]` is the canonical home of a per-element initial
 *          value for ANY constituent — pollutant, reactions-component species,
 *          age, temperature. The reactions engines (LEGACY binding, ARD,
 *          LARD) seed MSX species from `ReactionData::init_elem_*`, the
 *          table `[REACTION_QUALITY] NODE|LINK` fills. This helper mirrors
 *          the MSX-kind rows of `ctx.initial_quality` into that table so
 *          every engine sees them without a fourth seed site:
 *
 *          - called from PostParseResolver after the rows are classified,
 *          - called again by the reactions component whenever it re-parses
 *            its `[REACTION_QUALITY]` block (the block reset clears the
 *            table; the mirrored rows must come back).
 *
 *          A row the .rxn also carries (same element, same species) is an
 *          error — the same duplicate policy `[INITIAL_QUALITY]` applies to
 *          itself. The ReactionsWriter skips mirrored rows so a save emits
 *          each row once, in `[INITIAL_QUALITY]`.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_TRANSPORT_MSX_INITIAL_QUALITY_HPP
#define OPENSWMM_ENGINE_TRANSPORT_MSX_INITIAL_QUALITY_HPP

#include <string>
#include <vector>

namespace openswmm {
struct SimulationContext;
}

namespace openswmm::transport {

/// Mirror every MSX-kind `[INITIAL_QUALITY]` row into
/// `ctx.reactions.init_elem_*` (upsert; a conflicting .rxn row is reported).
/// Returns the diagnostics (empty on success).
std::vector<std::string> mirrorInitialQualityMsxRows(SimulationContext& ctx);

/// True when `ctx.initial_quality` carries an MSX row for (is_link, elem,
/// species) — the ReactionsWriter's "already written elsewhere" test.
bool initialQualityHasMsxRow(const SimulationContext& ctx, bool is_link,
                             int elem_idx, int species);

}  // namespace openswmm::transport

#endif  // OPENSWMM_ENGINE_TRANSPORT_MSX_INITIAL_QUALITY_HPP
