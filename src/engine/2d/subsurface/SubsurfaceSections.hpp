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
 * @file SubsurfaceSections.hpp
 * @brief G-step 1 — `[2D_AQUIFER_OPTIONS]`, `[2D_AQUIFER]` and
 *        `[2D_AQUIFER_NODE]`: parsing, unit resolution and writing.
 *
 * @section aq_units One rule for units, not seven
 *
 * Rows are authored in the project's own unit system and converted once, by
 * the kernel, through `GwUnitFactors` (declared in SubsurfaceData.hpp). The
 * rule is deliberately uniform so a user who has read one row can read them
 * all:
 *
 * | quantity | US | SI |
 * |---|---|---|
 * | lengths — `ZS`, `PSI_B`, `HG0`, `DC` | ft | m |
 * | rates — `KS`, `C_LOSS`, `KC`        | in/hr | mm/hr |
 * | inverse lengths — `ALPHA`           | 1/ft | 1/m |
 * | areas — `AREA`                      | ft² | m² |
 * | `THETA_S`, `THETA_R`, `LAMBDA`, `N`, `L` | — | — |
 *
 * The rates follow `[INFILTRATION]` and `[AQUIFERS]` (in/hr, mm/hr), which is
 * what a SWMM user already expects for a conductivity. The lengths follow the
 * project length unit rather than `[INFILTRATION]`'s inches, because `ZS` is a
 * soil-column depth of metres, not a suction head of millimetres, and mixing
 * the two in one section is how unit bugs are authored.
 *
 * @section aq_syntax Syntax
 *
 * ```
 * [2D_AQUIFER_OPTIONS]
 * SOIL_CHAR      RUSSO | GARDNER | BROOKS_COREY | VAN_GENUCHTEN
 * CLOSURE        AUTO | CLOSED_FORM | ENSLAVED | SIGMA
 * M_LAYERS       8
 * CAPILLARY_DIFF NO
 * C_GW           0.5
 * C_COL          0.9
 * FORCE_CLOSED_FORM NO
 * MODE           MESH | PER_SUBCATCH
 * DUNNE          YES
 * GW_ET          NONE | CAPILLARY_RISE | BOUNDARY_ET | BOTH
 *
 * [2D_AQUIFER]
 * ;;scope         KS      ZS   THETA_S THETA_R ALPHA  [key value]…
 * *               0.5     5.0  0.45    0.10    2.0    C_LOSS 0.0  HG0 1.0
 * TAG  clay       0.02    3.0  0.50    0.15    1.2    SOIL_CHAR VAN_GENUCHTEN N 1.3
 * CELL 42         1.0     6.0  0.42    0.08    3.0    CLOSURE SIGMA M_LAYERS 16
 *
 * [2D_AQUIFER_NODE]
 * ;;node   cell   [KC kc DC dc] [AREA a]
 * J1       17     KC 0.01 DC 0.5
 * ```
 *
 * The five positional columns are the ones every law needs. Everything a
 * particular law adds (`PSI_B`, `LAMBDA`, `N`, `L`) is a keyword, so adding a
 * law later does not renumber anyone's file.
 *
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_SUBSURFACE_SECTIONS_HPP
#define OPENSWMM_ENGINE_2D_SUBSURFACE_SECTIONS_HPP

#include "SubsurfaceData.hpp"
#include "../../input/SectionRegistry.hpp"

#include <string>
#include <vector>

namespace openswmm {
struct SimulationContext;
}

namespace openswmm::twoD {

struct MeshData;

/// One `[2D_AQUIFER_OPTIONS]` line (`KEY VALUE`). Empty return = OK.
std::string parseAquiferOptionsLine(const std::vector<std::string>& tokens,
                                    GwOptions& opts);

/// One `[2D_AQUIFER]` line. Empty return = OK.
std::string parseAquiferLine(const std::vector<std::string>& tokens,
                             std::vector<GwAquiferRow>& rows);

/// One `[2D_AQUIFER_NODE]` line. `node` is left as a NAME in @p names until
/// `resolveSubsurface` maps it to an index. Empty return = OK.
std::string parseAquiferNodeLine(const std::vector<std::string>& tokens,
                                 std::vector<GwNodeBed>& beds,
                                 std::vector<std::string>& names);

/// Register the three handlers against @p registry, writing into @p cfg.
void registerSubsurfaceSections(SubsurfaceConfig& cfg,
                                std::vector<std::string>& node_names,
                                input::SectionRegistry& registry);

/// The factors for this project's unit system.
GwUnitFactors gwUnitFactors(const SimulationContext& ctx) noexcept;

/// Resolve node names to indices and each bed's cell to a mesh cell.
/// Returns diagnostics; empty on success.
std::vector<std::string> resolveSubsurface(SimulationContext& ctx,
                                           const MeshData& mesh,
                                           SubsurfaceConfig& cfg,
                                           const std::vector<std::string>& names);

/// Write the three sections back in the project's own units — that is, the
/// authored values verbatim — omitting anything at its default so a
/// round-trip with no edits adds nothing.
void writeSubsurfaceSections(const SubsurfaceConfig& cfg,
                             const std::vector<std::string>& node_names,
                             std::string& out);

}  // namespace openswmm::twoD

#endif  // OPENSWMM_ENGINE_2D_SUBSURFACE_SECTIONS_HPP
