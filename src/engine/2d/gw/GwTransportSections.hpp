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
 * @file      GwTransportSections.hpp
 * @brief     U4 (2026-09-07) — parsers and registration for the `[GW_*]`
 *        subsurface-transport authoring sections. See GwTransportData.hpp
 *        for retained authoring shapes and runtime resolution contracts.
 *
 * @ingroup engine_2d
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
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

/**
 * @brief One `[GW_TRANSPORT_OPTIONS]` line (`KEY VALUE [ARG…]`). Empty = OK.
 *
 * @details Apply one groundwater transport/thermal authoring option without resolving physical
 * owners.
 * - Argument tokens (in): Tokenized KEY VALUE [ARG...] row; empty is accepted. Most keys ignore
 * surplus tokens; UNSAT_TRANSPORT alone requires exactly two.
 * - Argument opts (in,out): Mutable authoring options (o in the definition), including
 * thermal_authored tags.
 * @return Empty on recognized valid row/empty input, otherwise grammar/key/value diagnostic.
 * @note Supports transport/conduction/dispersion YES|NO flags, BULK|LAYERED, WATER_ONLY|
 * POROUS_EQUILIBRIUM, thermal layers integer1..128, positive max step s/conductivity
 * W/(m*K)/surface resistance m2*K/W, mixing rules, surface/deep temperature degC or heat flux
 * W/m2/series and optional nonnegative depth m; C_DIFF is finite(0,1]. Stores values/names
 * without scaling or table lookup. Some multi-token thermal branches set fields before
 * validating later values, so diagnostic failure can leave partial options. Allocation can
 * throw. No runtime owner rebuild/physics/clock update; exclusive authoring access required; use
 * a candidate for transactional edits.
 */
std::string parseGwTransportOptionsLine(const std::vector<std::string>& tokens,
                                        GwTransportOptions& opts);

/**
 * @brief One `[GW_TRANSPORT_PARAMS]` line: `* | TAG <t> | CELL <n>` then the positional matrix
 * properties (see GwParamsRow). Empty = OK.
 *
 * @details Parse one scoped positional groundwater material/dispersion authoring row.
 * - Argument tokens (in): Empty or (*|TAG name|CELL one-based-index) followed by up to nine
 * optional properties: rho_s kg/m3, c_s J/(kg*K), lambda_s W/(m*K), a_s m2/kg, alpha_L/alpha_T
 * m, D_m/D_v m2/s and geo_flux W/m2 or configured deep temperature degC. '-' retains struct
 * default.
 * - Argument rows (in,out): Existing rows retained; appends one complete parsed row on success.
 * @return Empty on empty/success, otherwise scope/count/nonfinite/numeric/negative-property
 * error.
 * @note Only ninth property may be negative. Converts CELL to zero-based but defers mesh/tag and
 * physical model validation. Integer tokens must be int-representable; numeric helper casts
 * before its integral check. No unit conversion/owner rebuild; allocation may throw, serial
 * authoring only.
 */
std::string parseGwParamsLine(const std::vector<std::string>& tokens,
                              std::vector<GwParamsRow>& rows);

/**
 * @brief One `[GW_SORPTION]` line: scope, SPECIES, Kd [decay]. Empty = OK.
 *
 * @details Parse scoped species partition coefficient and optional decay authoring data.
 * - Argument tokens (in): Empty or scope SPECIES Kd [decay]; Kd finite nonnegative L/kg, decay
 * finite nonnegative 1/day, '-' or omission retains default fallback to pollutant decay. Surplus
 * ignored.
 * - Argument rows (in,out): Appends one complete row on success, preserves earlier rows on
 * diagnostic failure.
 * @return Empty on success/empty, otherwise scope/arity/numeric/sign diagnostic.
 * @note CELL is authored one-based, stored zero-based, int-representable; species/tag/mesh
 * lookup and kinetic conversion are separate resolution. No physical state mutation; allocation
 * may throw.
 */
std::string parseGwSorptionLine(const std::vector<std::string>& tokens,
                                std::vector<GwSorptionRow>& rows);

/**
 * @brief One `[GW_INITIAL_QUALITY]` line: scope, `SAT|UNSAT|LAYER <j>`, SPECIES, VALUE — or
 * `FILE <path>`. Empty = OK.
 *
 * @details Parse a scoped initial native quality row or retain an external file prescription.
 * - Argument tokens (in): Empty; FILE path; or scope (SAT|UNSAT|LAYER one-based-layer) SPECIES
 * VALUE. VALUE must be finite; native concentration, age authored hours or temperature degC is
 * retained without unit/sign conversion here. Surplus tokens are ignored.
 * - Argument rows (in,out): Appends complete inline row only after syntax validation; FILE
 * appends none.
 * - Argument file (in,out): Replaced by FILE path token only for that branch, otherwise
 * retained.
 * @return Empty on empty/success, otherwise grammar/index/numeric diagnostic.
 * @note CELL becomes zero-based, LAYER remains one-based; integer input must be
 * int-representable. Does not open file or resolve mesh/species/sign/layout; use
 * resolveGwTransport and runtime owners. Allocation may throw; no physics/clock update,
 * exclusive authoring access.
 */
std::string parseGwInitialQualityLine(const std::vector<std::string>& tokens,
                                      std::vector<GwInitialQualityRow>& rows, std::string& file);

/**
 * @brief One `[GW_BOUNDARY_QUALITY]` line: `CELL EDGE SPECIES CONC v | TS n | MASSFLUX v|n |
 * HEATFLUX v|n`. CELL is 1-based in the file, EDGE is 0-based (`EDGE 0..nv-1` of that cell).
 * Empty = OK.
 *
 * @details Parse a lateral groundwater boundary native concentration or flux prescription.
 * - Argument tokens (in): Empty or CELL EDGE SPECIES (CONC|TS|MASSFLUX|HEATFLUX) VALUE/NAME.
 * CELL positive one-based becomes zero-based; EDGE nonnegative local zero-based, upper bound
 * deferred. Numeric indices must be int-representable. Surplus tokens ignored.
 * - Argument rows (in,out): Existing rows retained; appends one complete parsed candidate on
 * success.
 * @return Empty on empty/success, otherwise arity/index/kind diagnostic.
 * @note TS always stores name. Other kinds use finite numeric value or treat any nonnumeric
 * token as series name, including nonfinite numeric spellings. Retains prescribed native
 * concentration, mass-flux or W heat-flux conventions without scaling/sign/mesh/species/table
 * validation. Allocation can throw; resolution/physical acceptance are separate, serial
 * authoring only.
 */
std::string parseGwBoundaryQualityLine(const std::vector<std::string>& tokens,
                                       std::vector<GwBoundaryQualityRow>& rows);

/**
 * @brief One `[GW_SOURCES]` line: `NAME (CELL n | TAG t | XY x y) FLOW v|ts [SPECIES (CONC|MASS)
 * v|ts]…`. Empty = OK.
 *
 * @details Parse one named scoped groundwater injection/extraction and native constituent
 * prescription.
 * - Argument tokens (in): Empty or NAME (*|GLOBAL|CELL n|TAG name|XY x y) FLOW value/series
 * [SCALE factor] followed by repeated SPECIES (CONC|MASS) value/series. Flow m3/s is signed
 * positive injection; SCALE finite nonnegative multiplies total flow and mass rates, not
 * concentration.
 * - Argument rows (in,out): Appends one complete local candidate only after all syntax checks
 * succeed.
 * @return Empty on empty/success, otherwise location/arity/scale/species-kind diagnostic.
 * @note CELL is int-representable one-based converted to zero-based. XY finite coordinates must
 * be in the centroid frame used at resolution. Numeric/series discrimination defers
 * unknown/nonfinite spellings as names; duplicate source/species/table/coverage/native units are
 * resolved later. No physical source activation or clock advance; vector allocation may throw,
 * exclusive authoring access.
 */
std::string parseGwSourcesLine(const std::vector<std::string>& tokens,
                               std::vector<GwSourceRow>& rows);

/**
 * @brief Register every `[GW_*]` handler against @p registry, writing into @p gw.
 *
 * @details Register GW authoring callbacks that append into the borrowed model data.
 * - Argument gw (in,out): Stable model owner captured by reference, must outlive registry
 * callback use.
 * - Argument registry (in,out): Receives handlers for transport
 * options/params/sorption/initial/boundary quality and sources. Existing registry conflict
 * behavior is delegated.
 * @note Each handler marks authored, tokenizes nonblank lines, stops on first returned error
 * with ctx.error_code=5 and raw-line diagnostic; prior accepted rows remain. Initial/source
 * edits invalidate derived indexes. Captures no physical runtime owner; no
 * reset/rollback/physics. Allocation/callback exceptions propagate and registration may be
 * partial. Serial registry/model authoring required.
 */
void registerGwTransportSections(GwTransportData& gw, input::SectionRegistry& registry);

/**
 * @brief Validate the authored rows against the model and the species registry, once the mesh
 * and the species are final.
 *
 * @details Cell indices must be on the mesh, `EDGE` must be `0..nv-1` of ITS cell (cell-generic
 * — a triangle has 3, a quad 4), species must resolve through the one registry (pollutants,
 * reactions species, `__WATER_AGE__`, `__TEMPERATURE__`), timeseries names must exist. Returns
 * the diagnostics; empty on success. Also loads the `[GW_INITIAL_QUALITY] FILE` sidecar
 * (relative to the .inp).
 *
 * - Argument ctx (in,out): Matching prepared species registry, series names, input path and
 * model tables; logically read by this resolver.
 * - Argument mesh (in): Final cell count/tags/centroids/local vertex counts, stable and
 * consistently sized.
 * - Argument gw (in,out): Authoring rows; invalidates caches, loads external initial quality and
 * updates XY source cell assignments while preserving authored location fields.
 * @return All collected file/grammar/cell/tag/species/series/duplicate-source/sign diagnostics;
 * empty when these checks succeed. Does not guarantee complete runtime thermal/native profile
 * qualification.
 * @note Resolves sidecar relative to INP directory; comma/semicolon/tab become separators.
 * Removes previous non-LAYER file rows marked -2, ignores malformed physical first line as
 * header, appends other valid rows while collecting errors. LAYER rows lack that marker and are
 * not deduplicated on re-resolve. XY uses nearest centroid, first minimum wins; caller ensures
 * compatible coordinate units. Signed initial age/temperature accepted here, other negative
 * concentration refused. Does not seed native inventories, activate sources, convert units or
 * advance time. Errors may coexist with loaded/ resolved mutations; allocation/file exceptions
 * can propagate. Exclusive authoring/resolve access.
 */
std::vector<std::string> resolveGwTransport(SimulationContext& ctx, const MeshData& mesh,
                                            GwTransportData& gw);

/**
 * @brief The single "authored but no kernel" run-time warning (U4's contract). Empty when
 * nothing was authored.
 *
 * - Argument gw (in): Stable authoring record; default options alone do not count as authored
 * data.
 * @return Empty when data empty, otherwise owned warning text that authored GW rows have no
 * active aquifer kernel. Caller, not this helper, determines whether the kernel is absent.
 * @note Read-only, may allocate; does not validate, disable or mutate authoring/runtime owners.
 */
std::string gwTransportInertWarning(const GwTransportData& gw);

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_GW_TRANSPORT_SECTIONS_HPP
