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
 * @file GwTransportSections.hpp
 * @brief U4 (2026-09-07) — parsers and registration for the `[GW_*]`
 *        subsurface-transport authoring sections. See GwTransportData.hpp
 *        for the shape and the authoring-only contract.
 *
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_GW_TRANSPORT_SECTIONS_HPP
#define OPENSWMM_ENGINE_2D_GW_TRANSPORT_SECTIONS_HPP

#include "GwTransportData.hpp"
#include "../../input/SectionRegistry.hpp"

#include <string>
#include <vector>

namespace openswmm {
struct SimulationContext;
}

namespace openswmm::twoD {

struct MeshData;

/// One `[GW_TRANSPORT_OPTIONS]` line (`KEY VALUE [ARG…]`). Empty = OK.
std::string parseGwTransportOptionsLine(const std::vector<std::string>& tokens,
                                        GwTransportOptions& opts);

/// One `[GW_TRANSPORT_PARAMS]` line: `* | TAG <t> | CELL <n>` then the
/// positional matrix properties (see GwParamsRow). Empty = OK.
std::string parseGwParamsLine(const std::vector<std::string>& tokens,
                              std::vector<GwParamsRow>& rows);

/// One `[GW_SORPTION]` line: scope, SPECIES, Kd [decay]. Empty = OK.
std::string parseGwSorptionLine(const std::vector<std::string>& tokens,
                                std::vector<GwSorptionRow>& rows);

/// One `[GW_INITIAL_QUALITY]` line: scope, `SAT|UNSAT|LAYER <j>`, SPECIES,
/// VALUE — or `FILE <path>`. Empty = OK.
std::string parseGwInitialQualityLine(const std::vector<std::string>& tokens,
                                      std::vector<GwInitialQualityRow>& rows,
                                      std::string& file);

/// One `[GW_BOUNDARY_QUALITY]` line: `CELL EDGE SPECIES CONC v | TS n |
/// MASSFLUX v|n | HEATFLUX v|n`. CELL is 1-based in the file, EDGE is
/// 0-based (`EDGE 0..nv-1` of that cell). Empty = OK.
std::string parseGwBoundaryQualityLine(const std::vector<std::string>& tokens,
                                       std::vector<GwBoundaryQualityRow>& rows);

/// One `[GW_SOURCES]` line: `NAME (CELL n | TAG t | XY x y) FLOW v|ts
/// [SPECIES (CONC|MASS) v|ts]…`. Empty = OK.
std::string parseGwSourcesLine(const std::vector<std::string>& tokens,
                               std::vector<GwSourceRow>& rows);

/// Register every `[GW_*]` handler against @p registry, writing into @p gw.
void registerGwTransportSections(GwTransportData& gw,
                                 input::SectionRegistry& registry);

/**
 * @brief Validate the authored rows against the model and the species
 *        registry, once the mesh and the species are final.
 *
 * @details Cell indices must be on the mesh, `EDGE` must be `0..nv-1` of ITS
 *          cell (cell-generic — a triangle has 3, a quad 4), species must
 *          resolve through the one registry (pollutants, reactions species,
 *          `__WATER_AGE__`, `__TEMPERATURE__`), timeseries names must exist.
 *          Returns the diagnostics; empty on success. Also loads the
 *          `[GW_INITIAL_QUALITY] FILE` sidecar (relative to the .inp).
 */
std::vector<std::string> resolveGwTransport(SimulationContext& ctx,
                                            const MeshData& mesh,
                                            GwTransportData& gw);

/// The single "authored but no kernel" run-time warning (U4's contract).
/// Empty when nothing was authored.
std::string gwTransportInertWarning(const GwTransportData& gw);

}  // namespace openswmm::twoD

#endif  // OPENSWMM_ENGINE_2D_GW_TRANSPORT_SECTIONS_HPP
