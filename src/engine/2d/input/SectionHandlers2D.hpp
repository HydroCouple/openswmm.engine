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
 * @file      SectionHandlers2D.hpp
 * @brief     Input section parsers for the 2D surface routing module.
 *
 * @details Provides handler functions for the [2D_*] input sections:
 *          - [2D_OPTIONS]           — Solver options and tolerances
 *          - [2D_VERTICES]          — Mesh vertex coordinates
 *          - [2D_TRIANGLES]         — Triangle connectivity and roughness
 *          - [2D_VERTEX_NODE_MAP]   — Vertex-to-SWMM-node coupling
 *          - [2D_TRIANGLE_NODE_MAP] — Triangle-to-SWMM-node coupling
 *
 *          Each handler is registered via SectionRegistry::register_custom()
 *          and conforms to the SectionHandler signature.
 *
 * @see TWO_DIMENSIONAL_SURFACE_ROUTING_IMPLEMENTATION_STRATEGY.md §1
 * @ingroup engine_2d
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_SECTION_HANDLERS_HPP
#define OPENSWMM_ENGINE_2D_SECTION_HANDLERS_HPP

#include "../data/MeshData.hpp"
#include "../data/SolverOptions2D.hpp"
#include "../infil/Infil2D.hpp"
#include "../SurfaceRouter2D.hpp"
#include "../../input/SectionRegistry.hpp"

#include <span>
#include <string>
#include <vector>

namespace openswmm::twoD {

/**
 * @brief Parse a single line from the [2D_OPTIONS] section.
 *
 * @param tokens   Whitespace-split tokens from the line.
 * @param opts     Output solver options to populate.
 * @param warnings When non-null (the .inp file-load path), keys retired with the CVODE/ARKODE
 * stack — and retired INTEGRATOR values — are IGNORED with a WARNING 104 pushed here, so legacy
 * models still open. When null (the programmatic swmm_options_set_ext path), they remain hard
 * errors.
 * @return Empty string on success, or error description.
 *
 * @details Recognizes authoring
 * process/backend/momentum/closure/coupling/report/heat/infiltration policy; options carry their
 * documented SI solver units (s, m, m2/s, dimensionless coefficients).
 * REPORT_2D_VARIABLES/SPECIES consume all value tokens; most other keys use first value and
 * ignore surplus. Enum values/ranges are checked per key, but numeric parsing is not uniformly
 * finite: DISPERSION and report/infil duration explicitly enforce finiteness, while several
 * legacy scalars can retain NaN/Inf or unbounded values for later validation. Some branches
 * assign before returning a diagnostic, so use a copy for transactional edits. Retired
 * keys/INTEGRATOR values append warning and return success only with a nonnull warning sink;
 * otherwise diagnostic failure. No physical owner rebuild or clock advance; allocations can
 * throw after partial options/warnings changes.
 */
std::string parse2DOptionsLine(const std::vector<std::string>& tokens, SolverOptions2D& opts,
                               std::vector<std::string>* warnings = nullptr);

/**
 * @brief Parse a single line from the [2D_VERTICES] section.
 *
 * Format: X Y Z [TAG]
 *
 * @param tokens Whitespace-split tokens from the line.
 * @param mesh   Mesh data to append vertex to.
 * @return Empty string on success, or error description.
 *
 * @details Appends a zero-based vertex in input row order; X/Y/Z retain authored project length
 * or sidecar-declared SI units, converted only at initialization. Optional tag copied exactly,
 * surplus ignored. Syntax errors return before resizing, but no finite/geometry/duplicate-tag
 * validation occurs. No topology/physical owner rebuild or clock advance occurs. Requires
 * exclusive authoring access; allocation may throw after resizing/appending begins. Numeric
 * helpers accept full strtod/strtol syntax without general finite/range/errno checks; deferred
 * model validation remains necessary.
 */
std::string parse2DVertexLine(const std::vector<std::string>& tokens, MeshData& mesh);

/**
 * @brief Parse a single line from the [2D_TRIANGLES] section.
 *
 * Format: V1 V2 V3 MANNINGS_N [INIT_DEPTH] [TAG]
 *
 * @param tokens Whitespace-split tokens from the line.
 * @param mesh   Mesh data to append triangle to.
 * @return Empty string on success, or error description.
 *
 * @details Vertex indices are zero-based integer tokens, with mesh range/degeneracy/orientation
 * checked later. INIT_DEPTH optional numeric nonnegative authored mean depth; a nonnumeric fifth
 * token is legacy TAG. Sixth is TAG only with numeric depth. Mannings n is retained without
 * positivity validation. Refuses triangles after any quad to preserve unified cell numbering;
 * surplus ignored. No topology/physical owner rebuild or clock advance occurs. Requires
 * exclusive authoring access; allocation may throw after resizing/appending begins. Numeric
 * helpers accept full strtod/strtol syntax without general finite/range/errno checks; deferred
 * model validation remains necessary.
 */
std::string parse2DTriangleLine(const std::vector<std::string>& tokens, MeshData& mesh);

/**
 * @brief Parse a single line from the [2D_QUADS] section.
 *
 * Format: V1 V2 V3 V4 MANNINGS_N [INIT_DEPTH] [TAG] Quads are appended after every triangle
 * (cell index = n_triangles + j).
 *
 * - Argument tokens (in): V1 V2 V3 V4 MANNINGS_N [INIT_DEPTH] [TAG], zero-based int vertex
 * indices in cyclic order. Numeric optional mean depth must be nonnegative; nonnumeric optional
 * token is TAG.
 * - Argument mesh (in,out): Appends one four-vertex cell after existing unified cells, retaining
 * authored length/depth units and n coefficient. Earlier rows retained on syntax error.
 * @return Empty on parsed row, otherwise arity/index/numeric/depth diagnostic.
 * @note No finite/index-upper-bound/convexity/degeneracy validation; caller keeps all triangles
 * before quads. Surplus ignored. No topology/physical owner rebuild or clock advance occurs.
 * Requires exclusive authoring access; allocation may throw after resizing/appending begins.
 * Numeric helpers accept full strtod/strtol syntax without general finite/range/errno checks;
 * deferred model validation remains necessary.
 */
std::string parse2DQuadLine(const std::vector<std::string>& tokens, MeshData& mesh);

/**
 * @brief Parse a single line from the [2D_INITIAL_VELOCITY] section.
 *
 * Format: TRI U V   (m/s; default 0,0 — rows may cover any subset)
 *
 * @param tokens Whitespace-split tokens from the line.
 * @param mesh   Mesh data to update (tri_init_u / tri_init_v).
 * @return Empty string on success, or error description.
 *
 * @details TRI is zero-based unified cell index already present; U/V are m/s without
 * project-length conversion. Writes both components only after both numeric syntax checks; no
 * finite check, surplus ignored. Requires tri_init_u/v extents matching mesh. No
 * allocation/physics/clock update; serial edit.
 */
std::string parse2DInitialVelocityLine(const std::vector<std::string>& tokens, MeshData& mesh);

/**
 * @brief Parse a single line from the [2D_VERTEX_NODE_MAP] section.
 *
 * Format: VERTEX_INDEX_OR_TAG SWMM_NODE_NAME [CD] [AREA]
 *
 * @param tokens Whitespace-split tokens from the line.
 * @param mesh   Mesh data to update coupling map.
 * @return Empty string on success, or error description.
 *
 * @details Resolves zero-based index first, then exact tag fallback (including out-of-range
 * numeric tags), first matching tag wins. Stores deferred native node name; optional CD
 * dimensionless/AREA project-length2 are applied only when numeric, malformed optional tokens
 * are silently ignored. Valid optional values are not range/finite checked. No node-name lookup
 * yet; surplus ignored. Name allocation can throw; exclusive edit access, later
 * resolution/scaling required.
 */
std::string parse2DVertexNodeMapLine(const std::vector<std::string>& tokens, MeshData& mesh);

/**
 * @brief Parse a single line from the [2D_TRIANGLE_NODE_MAP] section.
 *
 * Format: TRIANGLE_INDEX_OR_TAG SWMM_NODE_NAME [CD] [AREA]
 *
 * @param tokens Whitespace-split tokens from the line.
 * @param mesh   Mesh data to update coupling map.
 * @return Empty string on success, or error description.
 *
 * @details Resolves zero-based unified cell index or exact first matching tag fallback,
 * including out-of-range numeric tags. Every row appends a distinct coupling (multiple nodes per
 * cell) and updates legacy per-cell name/CD/area mirror, last row wins there. Optional malformed
 * CD/area is ignored; no finite/sign/node-name check, retained AREA in project-length2. Array
 * mirrors change before vector append, so allocation failure can leave partial mutation. No
 * runtime owner rebuild or physics/calendar advance; exclusive authoring access and prepared
 * per-cell arrays required.
 */
std::string parse2DTriangleNodeMapLine(const std::vector<std::string>& tokens, MeshData& mesh);

/**
 * @brief V-E3 — parse a single line from the [2D_BOUNDARY_CONDITIONS] section into a
 * SurfaceRouter2D::PendingBoundaryRow appended to
 * @p pending_rows.
 *
 * Format: TRI EDGE TYPE [PARAM_1 [PARAM_2 [GROUP]]] TYPE ∈ WALL / NORMAL_FLOW / SPECIFIED_STAGE
 * / TS_STAGE / SPECIFIED_FLOW / TS_FLOW / RATING_CURVE PARAM_1 = slope / head / TS name / flow /
 * TS name / curve name (by type) PARAM_2 reserved, always "*" GROUP   optional named group ("*"
 * = none)
 *
 * Rows are drained into BoundaryData inside SurfaceRouter2D::initialize() after
 * boundary_.resize() has sized the per-edge slots.
 *
 * - Argument tokens (in): Empty or zero-based TRI EDGE TYPE [PARAM_1 [reserved [GROUP]]]. EDGE
 * checked against max four slots, actual cell shape/upper bound deferred. TYPE
 * wall/normal/stage/flow/series/ rating; '*' or missing parameter leaves default. PARAM_1 is
 * dimensionless slope, authored head, display flow per metre, or deferred table name by type.
 * Group copied from sixth token unless '*'.
 * - Argument pending_rows (in,out): Existing rows retained; appends one complete candidate on
 * success.
 * @return Empty on empty/parsed row, otherwise arity/index/type/numeric diagnostic.
 * @note No table/mesh/physical finite validation; reserved fifth/surplus tokens ignored. No
 * scaling, activation or clock advance; allocation may throw, exclusive retained authoring
 * access.
 */
std::string
parse2DBoundaryConditionsLine(const std::vector<std::string>& tokens,
                              std::vector<SurfaceRouter2D::PendingBoundaryRow>& pending_rows);

/**
 * @brief §11A — parse a single `[2D_EDGE_CONVEYANCE]` line.
 *
 * Format: `FROM_VERTEX TO_VERTEX CONVEYANCE` FROM_VERTEX, TO_VERTEX : non-negative integer
 * mesh-vertex indices. Must differ. The pair is undirected. CONVEYANCE             : double in
 * [0, 1] (out-of-range values rejected, not clamped).
 *
 * Rows are accumulated into `pending` (a scratch buffer on `SurfaceRouter2D`) and resolved
 * against the mesh topology in `SurfaceRouter2D::initialize()` after `buildMeshTopology`
 * populates the neighbour table.
 *
 * - Argument tokens (in): Empty or FROM_VERTEX TO_VERTEX CONVEYANCE; distinct nonnegative
 * zero-based int vertex indices, dimensionless multiplier [0,1]. Surplus ignored.
 * - Argument pending_rows (in,out): Appends one parsed undirected vertex-pair candidate on
 * success.
 * @return Empty on empty/success; arity/index/numeric/out-of-range multiplier diagnostic
 * otherwise.
 * @note Range is rejected, not clamped. strtod NaN passes ordinary comparisons, so this is not a
 * complete finite-value validator. Vertex existence/edge topology deferred; no scaling/physical
 * advance. Allocation may throw; exclusive authoring access.
 */
std::string
parse2DEdgeConveyanceLine(const std::vector<std::string>& tokens,
                          std::vector<SurfaceRouter2D::PendingEdgeConveyanceRow>& pending_rows);

/**
 * @brief §5.5 track I — parse a single `[2D_INFILTRATION_OPTIONS]` line.
 *
 * Format: `INFIL_STEP <hh:mm:ss>` The only key. Accepts the same duration grammar as
 * WET_STEP/DRY_STEP in [OPTIONS] (`hh:mm:ss`, `hh:mm`, or plain seconds) and stores seconds in
 * Infil2DOptions::infil_step. 0 means "use the project WET_STEP" (D-I1).
 *
 * - Argument tokens (in): Empty or INFIL_STEP duration (hh:mm:ss, hh:mm or seconds); surplus
 * ignored.
 * - Argument infil (in,out): Replaces authored cadence seconds on accepted duration, otherwise
 * unchanged.
 * @return Empty on empty/accepted row, otherwise key/arity/negative parsed duration diagnostic.
 * @note Zero defers to wet-step resolution. This branch checks <0 without an independent finite
 * check. Does not resolve methods/advance infiltration capacity or clocks; serial authoring
 * only.
 */
std::string parse2DInfiltrationOptionsLine(const std::vector<std::string>& tokens, Infil2D& infil);

/**
 * @brief §5.5 track I — parse a single `[2D_INFILTRATION_DEFAULTS]` line.
 *
 * Format: `TAG|*  METHOD  [P1 .. P5]  [DEST]` TAG      : `[2D_TRIANGLES]` TAG to match, or `*`
 * for the mesh-wide default. METHOD   : HORTON / MODIFIED_HORTON / GREEN_AMPT /
 * MODIFIED_GREEN_AMPT / CURVE_NUMBER / CONSTANT / NONE (parseInfil2DMethod). P1..P5   :
 * POSITIONAL parameters in PROJECT UNITS, laid out per method exactly like legacy [INFILTRATION]
 * (see the table on Infil2DRow). `-` means "unset"; trailing columns a method does not use may
 * be omitted. DEST     : retained authoring destination, validated against resolved source
 * ownership. Initialization chooses the supported aquifer/legacy/lost receiver.
 *
 * Rows are appended to `infil.defaults()` in file order and resolved against the mesh in
 * Infil2D::resolve() (D-I3: override > tag > `*` > none).
 *
 * - Argument tokens (in): Empty or TAG|* METHOD [P1..P5] [DEST], method positional parameters in
 * project units; '-' retains zero default, recognized trailing destination optional.
 * - Argument infil (in,out): Appends a complete default/tag row on success; previous rows
 * retained.
 * @return Empty on empty/success, otherwise grammar/method/destination/parameter diagnostic.
 * @note Defers tag/exact parameter/physical/ownership validation and unit conversion to
 * resolve(). No runtime rate/cumulative/clock advance; allocation may throw, exclusive authoring
 * access.
 */
std::string parse2DInfiltrationDefaultsLine(const std::vector<std::string>& tokens, Infil2D& infil);

/**
 * @brief §5.5 track I — parse a single `[2D_INFILTRATION]` line.
 *
 * Format: `CELL  METHOD  [P1 .. P5]  [DEST]` CELL is the triangle index, **1-BASED in the file**
 * and stored 0-based in Infil2DOverride::tri. The upper bound is checked in Infil2D::resolve() —
 * the mesh is not necessarily loaded when this section is read. The remaining columns are
 * identical to [2D_INFILTRATION_DEFAULTS].
 *
 * Rows are appended to `infil.overrides()` and take precedence over every default row for that
 * cell.
 *
 * - Argument tokens (in): Empty or one-based CELL METHOD [P1..P5] [DEST], positional
 * project-unit parameters and optional final destination; '-' retains zero default.
 * - Argument infil (in,out): Appends complete per-cell override with stored zero-based cell on
 * success.
 * @return Empty on empty/success, otherwise cell lower-bound/grammar/method/parameter
 * diagnostic.
 * @note Mesh upper bound, physical parameters/ownership and unit conversion deferred to
 * resolve(). Does not alter prepared rate/cumulative/clock state; allocation can throw,
 * exclusive authoring only.
 */
std::string parse2DInfiltrationLine(const std::vector<std::string>& tokens, Infil2D& infil);

/**
 * @brief True when @p key (case-insensitive) is a [2D_OPTIONS] parameter accepted by
 * parse2DOptionsLine.
 *
 * Used by the swmm_options_get_ext / swmm_options_set_ext C API to route these keys to the live
 * SolverOptions2D (via SimulationContext::twod_io) instead of the generic ext_options map — that
 * routing is what makes GUI/API edits of 2D options reach the solver and persist through the
 * InpWriter [2D_OPTIONS] emission and the GeoPackage 2D_* option keys.
 *
 * - Argument key (in): Whole current option key, byte case-insensitive.
 * @return True for recognized current 2D option key, excluding retired-key list.
 * @note Does not validate an option value or its runtime availability; no allocation/mutation.
 */
bool is2DOptionKey(const std::string& key);

/**
 * @brief Every current [2D_OPTIONS] key accepted by is2DOptionKey, in documentation order.
 *
 * Enumerated by swmm_options_ext_key_count / swmm_options_ext_key_at.
 *
 * @return Process-owned key list; retired keys excluded. No allocation or mutation.
 */
std::span<const char* const> current2DOptionKeys();

/**
 * @brief True when @p key (case-insensitive) is a [2D_OPTIONS] parameter retired with the
 * CVODE/ARKODE stack (D2, 2026-07-29).
 *
 * parse2DOptionsLine hard-errors on these unless given a warnings sink (the file-load path warns
 * and ignores them); swmm_options_set_ext uses this predicate to reject them up front so they
 * can never land in the generic ext_options map (where they would silently persist to the next
 * save).
 *
 * - Argument key (in): Whole option key, byte case-insensitive.
 * @return True only for recognized retired CVODE/ARKODE keys; no warning or parse side effect.
 * @note Allocation-free lookup, no runtime validation or clock change.
 */
bool is2DRetiredOptionKey(const std::string& key);

/**
 * @brief Token ↔ bitmask helpers for [2D_OPTIONS] REPORT_2D_VARIABLES.
 *
 * report2DVarTokens() lists the group tokens in bit order (DEPTH, VELOCITY, EDGE_FLUX,
 * NODE_HEAD, SPECIES, RAINFALL, INFILTRATION, COUPLING, GRADIENTS, CONTINUITY, ENVELOPES,
 * BUILDUP (S7), GROUNDWATER, GW_DETAILED, HEAT (G-O)). parseReport2DVars() accepts a
 * whitespace/comma-separated list of those tokens or one of the presets DEFAULT | MINIMAL | ALL
 * (case-insensitive); it returns an empty error string on success. formatReport2DVars() writes a
 * preset name when the mask equals one, else the token list.
 *
 * @details Borrow canonical report-group keyword tokens in defined bit order.
 * @return Process-owned const vector from report2d::tokens; caller must not mutate. No model
 * effect; first construction may allocate, later reads require no owner synchronization.
 */
const std::vector<std::string>& report2DVarTokens();
/**
 * @brief Parse report-group keyword list/preset through the shared mask grammar.
 * - Argument text (in): Whole whitespace/comma-separated group list or DEFAULT|MINIMAL|ALL
 * preset.
 * - Argument mask (out): Parsed bitmask on success; delegated parseMask failure contract
 * applies.
 * @return Empty on success, otherwise unknown-token diagnostic; no model/options mutation.
 * @note Delegates canonical parser, may allocate; caller explicitly adopts successful mask.
 */
std::string parseReport2DVars(const std::string& text, unsigned& mask);
/**
 * @brief Format a report bitmask using canonical presets or ordered keyword list.
 * - Argument mask (in): Requested report-group bits.
 * @return Owned canonical preset/list string from formatMask. No model mutation, may allocate.
 */
std::string formatReport2DVars(unsigned mask);

/**
 * @details Numeric scalars use %.12g, enum/boolean/preset names canonical uppercase, durations
 * shared report step formatting; stored AUTO policy is not resolved to runtime enables. No model
 * or runtime owner mutation/validation; may allocate. This is a display/serialization token, not
 * a source receipt.
 *
 * @brief Format a stored 2D option as a canonical authoring/display token.
 * - Argument opts (in): Stable retained solver options, not effective runtime owner state.
 * - Argument key (in): Whole byte-case-insensitive option key.
 * @return Owned value token, or empty for unknown key/unset OUTPUT_FILE. Numeric values use12
 * significant digits; policy AUTO and configured unit conventions remain stored. No resolution,
 * validation or model mutation; allocation may throw.
 *
 * @details Format the current value of a [2D_OPTIONS] parameter as the string token
 * parse2DOptionsLine accepts (round-trip safe).
 *
 * - Original argument opts: Solver options to read.
 * - Original argument key: Parameter name (case-insensitive).
 * @return The value token, or an empty string for unknown keys (and for an unset OUTPUT_FILE,
 * whose value token is optional).
 */
std::string format2DOptionValue(const SolverOptions2D& opts, const std::string& key);

/**
 * @brief format2DOptionValue with the E2 infiltration keys resolved against the model's
 * [2D_INFILTRATION*] rows: INFIL_STEP falls back to the section value, INFIL_DEFAULT_METHOD to
 * the '*' row's method. INFILTRATION stays AUTO | YES | NO as stored. @p infil may be null.
 *
 * @details Format a stored option with optional borrowed authoring-owner fallbacks.
 * - Argument opts (in): Stable stored solver options.
 * - Argument infil (in): Optional stable infiltration authoring owner; consulted only during
 * call.
 * - Argument key (in): Whole case-insensitive option key.
 * @return Owned canonical token, empty for unknown/unset output path. Unset infiltration cadence
 * uses owner options; unset default method uses first '*' row or NONE. No physics/calendar
 * mutation; may allocate. Does not resolve stored process AUTO to a runtime enable.
 */
std::string format2DOptionValueEx(const SolverOptions2D& opts, const Infil2D* infil,
                                  const std::string& key);

/**
 * @brief As above, and additionally resolves the groundwater enables against the `[2D_AQUIFER*]`
 * authoring: GROUNDWATER AUTO reports the YES|NO the kernel will actually take, and GW_ET reads
 * the `[2D_AQUIFER_OPTIONS]` field that owns it. @p aquifer may be null.
 *
 * @details Format a stored option with optional borrowed authoring-owner fallbacks.
 * - Argument opts (in): Stable stored solver options.
 * - Argument infil (in): Optional stable infiltration authoring owner; consulted only during
 * call.
 * - Argument key (in): Whole case-insensitive option key.
 * @return Owned canonical token, empty for unknown/unset output path. Unset infiltration cadence
 * uses owner options; unset default method uses first '*' row or NONE. No physics/calendar
 * mutation; may allocate. Does not resolve stored process AUTO to a runtime enable.
 * - Argument aquifer (in): Optional stable aquifer config; unset GW_ET uses its policy or AUTO.
 */
std::string format2DOptionValueEx(const SolverOptions2D& opts, const Infil2D* infil,
                                  const SubsurfaceConfig* aquifer, const std::string& key);

/**
 * @brief Register all 2D input section handlers with the section registry.
 *
 * Call during input reader setup (conditional on OPENSWMM_HAS_2D). The handlers will populate
 * the mesh and options data in SimulationContext.
 *
 * - Argument mesh: Mesh data to populate.
 * - Argument options: Solver options to populate.
 * - Argument registry: Section registry to register handlers into.
 *
 * @details Register mesh/options/native-quality/boundary/infiltration authoring section
 * handlers.
 * - Additional argument mesh: Stable borrowed mesh target, must outlive callbacks.
 * - Additional argument options: Stable borrowed authoring options, must outlive callbacks.
 * - Argument pending_bc_rows (in,out): Borrowed retained hydraulic boundary rows, appended by
 * handlers.
 * - Argument pending_ec_rows (in,out): Borrowed retained undirected edge conveyance rows.
 * - Argument pending_iq_rows (in,out): Borrowed retained surface initial native quality rows.
 * - Argument pending_ibq_rows (in,out): Borrowed retained local-bed initial native quality rows.
 * - Argument pending_bq_rows (in,out): Borrowed retained boundary native quality rows.
 * - Additional argument registry: Receives custom handlers; conflict/replacement policy
 * delegated to registry.
 * @note Captured references remain externally owned. Infiltration target is taken from callback
 * context twod_io.infil (null skips). Mesh-file handler takes first FILE row only and preserves
 * raw unquoted path spacing. Row handlers tokenize/skip blanks, stop on first diagnostic with
 * context code5/raw line, retaining earlier rows; options receive warning sink for retired-key
 * compatibility. No automatic reset/rollback/model activation/clock advance. Allocation can
 * leave registration partial; callbacks/parsers may throw. Requires serial registry/model
 * lifecycle.
 */
void register2DSections(MeshData& mesh, SolverOptions2D& options,
                        std::vector<SurfaceRouter2D::PendingBoundaryRow>& pending_bc_rows,
                        std::vector<SurfaceRouter2D::PendingEdgeConveyanceRow>& pending_ec_rows,
                        std::vector<SurfaceRouter2D::PendingInitialQualityRow>& pending_iq_rows,
                        std::vector<SurfaceRouter2D::PendingInitialBedQualityRow>& pending_ibq_rows,
                        std::vector<SurfaceRouter2D::PendingBoundaryQualityRow>& pending_bq_rows,
                        input::SectionRegistry& registry);

/**
 * @brief S1/S2 — one `[2D_INITIAL_QUALITY]` line. Empty string = OK.
 *
 * @details Parse one scoped initial surface native concentration authoring row.
 * - Argument tokens (in): Empty; '* SPECIES CONC'; 'TAG name SPECIES CONC'; or 'CELL n SPECIES
 * CONC'. Exact arity required, CELL positive one-based stored zero-based. CONC finite
 * nonnegative except case-insensitive __TEMPERATURE__ may be negative. Age retained in authored
 * hours, temperature degC.
 * - Argument rows (in,out): Existing rows retained; appends one complete candidate on success.
 * @return Empty on empty/success, otherwise scope/count/index/numeric/sign diagnostic.
 * @note Species/tag/mesh existence and canonical case/layout, age conversion and mass seeding
 * are separate initialization work. No water/native/clock change; allocation may throw, serial
 * authoring.
 */
std::string parse2DInitialQualityLine(const std::vector<std::string>& tokens,
                                      std::vector<SurfaceRouter2D::PendingInitialQualityRow>& rows);

/**
 * @brief Initial local-bed porewater concentrations; same scope grammar as surface.
 *
 * @details Parse a finite local-bed native concentration seed without resolving a physical bed.
 * - Argument tokens (in): Empty or exact global/tag/one-based-CELL SPECIES CONC row, same scope
 * grammar as surface initial quality. CONC finite nonnegative, including any temperature-named
 * token here.
 * - Argument rows (in,out): Existing retained rows; appends one complete candidate on success.
 * @return Empty on empty/success, otherwise scope/arity/index/value diagnostic.
 * @note Bed owner/species support (including later temperature refusal) and mesh/tag/cell lookup
 * are deferred. Native concentration/age authored hours retained without conversion or physical
 * seeding. Allocation may throw, exclusive authoring access; no water/energy/clock advance.
 */
std::string
parse2DInitialBedQualityLine(const std::vector<std::string>& tokens,
                             std::vector<SurfaceRouter2D::PendingInitialBedQualityRow>& rows);

/**
 * @brief S2 — one `[2D_BOUNDARY_QUALITY]` line (`TRI EDGE SPECIES CONC`). Empty = OK.
 *
 * @details Parse one zero-based surface boundary native concentration row.
 * - Argument tokens (in): Empty or exact TRI EDGE SPECIES CONC. TRI nonnegative zero-based
 * unified cell, EDGE in max four slots; actual cell bound/shape and boundary type checked later.
 * CONC finite nonnegative except case-insensitive __TEMPERATURE__ signed degC; age retained
 * authored hours.
 * - Argument rows (in,out): Appends one complete candidate on success, retaining earlier rows.
 * @return Empty on empty/success, otherwise count/index/numeric/sign diagnostic.
 * @note No table/native-row resolution, scaling, boundary flux, mass or clock change. Allocation
 * can throw; exclusive authoring access, initialization validates actual model identity.
 */
std::string
parse2DBoundaryQualityLine(const std::vector<std::string>& tokens,
                           std::vector<SurfaceRouter2D::PendingBoundaryQualityRow>& rows);

/**
 * @brief Load 2D mesh sections from an external file.
 *
 * Opens the file referenced by @p mesh_file (resolved relative to
 * @p inp_base_dir if it is a relative path) and parses any [2D_OPTIONS], [2D_VERTICES],
 * [2D_TRIANGLES], [2D_VERTEX_NODE_MAP], [2D_TRIANGLE_NODE_MAP] and [2D_INFILTRATION*] sections
 * found in it.
 *
 * - Argument mesh: Mesh data to populate.
 * - Argument opts: Solver options to populate.
 * - Argument mesh_file: Path from the [2D_MESH_FILE] FILE token.
 * - Argument inp_base_dir: Directory of the parent .inp file (may be empty).
 * - Argument infil: Per-cell infiltration to populate from the sidecar's [2D_INFILTRATION*]
 * sections (plan §5.5.5 — these are per-cell mesh attributes, so they travel with the mesh
 * exactly like [2D_VERTICES]/[2D_TRIANGLES]). The .2dm is parsed against a detached context, so
 * the handlers cannot reach it through ctx.twod_io.infil and it must be passed explicitly. May
 * be null (rows are then dropped). A section present in BOTH files resolves in favour of the
 * sidecar, matching the [2D_MESH_FILE] rule that the external file overrides the inline one;
 * rows are never merged, so nothing is double-counted.
 * - Argument warnings: Optional sink for retired-key warnings (see parse2DOptionsLine); pass
 * &ctx.warnings so a legacy external mesh file still loads.
 * @returns Empty string on success, or an error description on failure.
 *
 * @details Read a mesh sidecar through a local registry into caller-supplied authoring targets.
 * - Additional argument mesh: Borrowed mesh append target; caller clears/reconciles inline
 * geometry as needed before this call. This loader does not itself clear mesh or pending
 * boundary stores.
 * - Additional argument opts: Authoring option target and mesh-units marker, may change during
 * partial read.
 * - Argument pending_bc_rows (in,out): Retained hydraulic boundary rows; sidecar lines append.
 * - Argument pending_ec_rows (in,out): Retained edge conveyance rows; sidecar lines append.
 * - Additional argument infil: Optional target; after successful read, nonempty sidecar
 * defaults/overrides replace corresponding target vectors, positive sidecar cadence replaces
 * options. Empty/zero keeps inline values, including a present section that produces no rows.
 * - Additional argument mesh_file: UTF-8 absolute or relative sidecar path.
 * - Additional argument inp_base_dir: UTF-8 base directory for relative mesh_file, empty leaves
 * it relative.
 * - Additional argument warnings: Optional borrowed retired-option warning sink; null makes
 * those options hard errors. Other row diagnostics return in result.
 * - Argument pending_iq_rows (in,out): Optional borrowed surface seed target; null omits its
 * handler.
 * - Argument pending_ibq_rows (in,out): Optional borrowed local-bed seed target; null omits its
 * handler.
 * - Argument pending_bq_rows (in,out): Optional borrowed boundary-quality target; null omits its
 * handler.
 * @return Empty on successful InputReader read/adoption; otherwise path-qualified parse/read
 * diagnostic.
 * @note Registry excludes 2D_MESH_FILE to prevent recursion. Pre-scans SI marker, then uses
 * detached context for reader errors. Mesh/options/pending rows are direct targets, so errors
 * may leave partial append/mutation; only infiltration parses privately before adoption. No unit
 * conversion/topology build/physical advance. Filesystem/allocation/parser exceptions may
 * propagate. Exclusive authoring access; every referenced target must remain stable/alive
 * through synchronous read.
 */
std::string load2DMeshExternalFile(
    MeshData& mesh, SolverOptions2D& opts,
    std::vector<SurfaceRouter2D::PendingBoundaryRow>& pending_bc_rows,
    std::vector<SurfaceRouter2D::PendingEdgeConveyanceRow>& pending_ec_rows, Infil2D* infil,
    const std::string& mesh_file, const std::string& inp_base_dir,
    std::vector<std::string>* warnings = nullptr,
    std::vector<SurfaceRouter2D::PendingInitialQualityRow>* pending_iq_rows = nullptr,
    std::vector<SurfaceRouter2D::PendingInitialBedQualityRow>* pending_ibq_rows = nullptr,
    std::vector<SurfaceRouter2D::PendingBoundaryQualityRow>* pending_bq_rows = nullptr);

/**
 * @brief Scan @p inp_path for a `;; UNITS: <value>` comment header and set
 * @p opts.mesh_units_si to true when the value names metres.
 *
 * Recognised SI markers (case-insensitive): `SI (m)`, `m`, `metre`, `metres`, `meter`, `meters`.
 * Any other value (or absent header) leaves the flag unchanged.
 *
 * Safe to call multiple times; e.g. once on the inline `.inp` and again on the resolved `.2dm` —
 * the most recent observation wins, which is the intended precedence (external file overrides
 * inline).
 *
 * Quietly does nothing if the file cannot be opened — the caller already reports the
 * missing-file error via the normal read path.
 *
 * @details Scan an entire UTF-8 input/mesh file for a recognized SI mesh-units comment marker.
 * - Argument inp_path (in): UTF-8 file path, relative paths use process working directory.
 * - Argument opts (in,out): Sets mesh_units_si=true on case-insensitive ';; UNITS: SI (m)' or
 * metric synonym; no recognized marker, unknown units or ft leaves current flag unchanged. Never
 * resets false.
 * @note Missing/unreadable file silently returns so caller can report normal read errors. Scans
 * to EOF because writer can emit marker inside sections, trims whitespace by borrowed views;
 * does not parse topology or convert coordinates. File/string allocation can throw; exclusive
 * options edit. Multiple markers only enable SI, so later unknown/nonmetric text does not undo
 * an earlier match.
 */
void prescan2DUnitsHeader(const std::string& inp_path, SolverOptions2D& opts);

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_SECTION_HANDLERS_HPP
