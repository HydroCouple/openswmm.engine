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
 * @file      SubsurfaceSections.hpp
 * @brief     G-step 1 — `[2D_AQUIFER_OPTIONS]`, `[2D_AQUIFER]` and
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
 * | lengths — `ZS`, `PSI_B`, `HG0`, `DC`, `WILTING_SUCTION` | ft | m |
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
 * GW_ET          AUTO | NONE | CAPILLARY_RISE | BOUNDARY_ET | BOTH
 * WILTING_SUCTION AUTO | positive_project_length
 * LINK_SEEPAGE   DEFAULT | ONE_WAY | TWO_WAY | NONE
 * ;; GW_ET AUTO resolves BOTH on a mesh, NONE in PER_SUBCATCH.
 * ;; LINK_SEEPAGE DEFAULT resolves TWO_WAY on a mesh; legacy AUTO
 * ;; remains an accepted explicit ONE_WAY spelling.
 * ;; Automatic wilting suction is 150 m in either project unit system.
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
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_SUBSURFACE_SECTIONS_HPP
#define OPENSWMM_ENGINE_2D_SUBSURFACE_SECTIONS_HPP

#include "SubsurfaceData.hpp"
#include "../../input/SectionRegistry.hpp"

#include <string>
#include <functional>
#include <limits>
#include <vector>

namespace openswmm {
struct SimulationContext;
}

namespace openswmm::twoD {

struct MeshData;

/**
 * @brief One `[2D_AQUIFER_OPTIONS]` line (`KEY VALUE`). Empty return = OK.
 *
 * @details Apply one aquifer authoring option without initializing a physical column.
 * - Argument tokens (in): Nonempty KEY [VALUE] vector; empty input is outside contract because
 * diagnostic path reads tokens[0]. Success requires at least two; surplus tokens ignored.
 * - Argument opts (in,out): Authoring defaults/process policies, updated only for recognized
 * valid values.
 * @return Empty on success, otherwise key/value diagnostic.
 * @note Parses soil/closure enums, layers2..128, boolean flags, finite C_GW/C_COL(0,1],
 * mode/node eligibility/link-seepage/ET policies and positive project-length wilting suction
 * (AUTO restores implicit150m policy). Integer helper requires int-representable input. No unit
 * conversion/runtime rebuild/physics/clock advance. Allocations can throw, exclusive authoring
 * access required.
 */
std::string parseAquiferOptionsLine(const std::vector<std::string>& tokens, GwOptions& opts);

/**
 * @brief One `[2D_AQUIFER]` line. Empty return = OK.
 *
 * @details Parse a scoped aquifer hydraulic authoring row into a local candidate.
 * - Argument tokens (in): Nonempty (*|TAG name|CELL positive-one-based-index) followed by KS ZS
 * THETA_S THETA_R ALPHA and optional key/value law/closure/layer fields. KS/C_LOSS project
 * rainfall units in/hr or mm/hr, ZS/PSI_B/HG0 project length, ALPHA inverse project length,
 * others dimensionless.
 * - Argument rows (in,out): Existing rows retained, appends one complete candidate only after
 * validation.
 * @return Empty on success, otherwise scope/arity/finite numeric/enum/range diagnostic.
 * @note Requires KS/ZS/ALPHA/lambda>0, theta_s(0,1], theta_r[0,theta_s), vg_n>1,
 * psi_b/c_loss>=0, optional layers2..128. Stores CELL zero-based; integer tokens must be
 * int-representable. HG0 and vg_L are finite but not additionally bounded here. Defers
 * mesh/tag/law compatibility and conversion. No physical inventory/clock mutation; allocation
 * may throw, exclusive authoring access.
 */
std::string parseAquiferLine(const std::vector<std::string>& tokens,
                             std::vector<GwAquiferRow>& rows);

/**
 * @brief One `[2D_AQUIFER_NODE]` line. `node` is left as a NAME in @p names until
 * `resolveSubsurface` maps it to an index. Empty return = OK.
 *
 * @details Parse one named native-node aquifer bed with explicit cell or deferred location.
 * - Argument tokens (in): NODE (positive-one-based-CELL|AUTO) [KC k DC d] [AREA a] [EXCHANGE
 * bool]. KC project in/hr or mm/hr; DC project length; AREA project length2. Integers must fit
 * int.
 * - Argument beds (in,out): Appends complete candidate after syntax/finite/nonnegative checks;
 * positive KC requires positive DC. AUTO records locate=true, explicit CELL stored zero-based.
 * - Argument names (in,out): Appends exact unresolved node name parallel to beds, not cleared.
 * @return Empty on success, otherwise arity/index/key/value/bed-thickness diagnostic.
 * @note Name/cell/ownership resolution is separate. beds append precedes names append, so
 * allocation failure can desynchronize parallel stores; use staged vectors for transactional
 * edits. No physics or clock advance, exclusive authoring access.
 */
std::string parseAquiferNodeLine(const std::vector<std::string>& tokens,
                                 std::vector<GwNodeBed>& beds, std::vector<std::string>& names);

/**
 * @brief One `[2D_AQUIFER_LINKS]` line (G-X4). `link` stays a NAME in @p names until
 * `resolveLinkSeepage` maps it. Empty return = OK.
 *
 * @details Parse named conduit bed coefficient/path and exchange opt-out authoring row.
 * - Argument tokens (in): LINK [KC finite-nonnegative-k] [DC finite-nonnegative-d] [EXCHANGE
 * bool], with KC in project rainfall units and DC project length; omission retains defaults.
 * - Argument rows (in,out): Appends complete candidate after syntax/value checks.
 * - Argument names (in,out): Appends exact unresolved link name parallel to rows.
 * @return Empty on success, otherwise empty/keyword/arity/value diagnostic.
 * @note Unlike node parser does not require positive DC when KC>0; runtime fallback/path
 * validation is separate. Does not resolve conduit identity/geometry/units or withdraw water.
 * Allocation may leave rows appended before name; exclusive authoring, no physics/calendar
 * advance.
 */
std::string parseAquiferLinkLine(const std::vector<std::string>& tokens,
                                 std::vector<GwLinkRow>& rows, std::vector<std::string>& names);

/**
 * @brief Register the four handlers against @p registry, writing into @p cfg.
 *
 * @details Parse a unique hydraulic aquifer exterior-edge prescription.
 * - Argument tokens (in): Exactly CELL n EDGE e HEAD|FLOW CONST value|SERIES name. CELL positive
 * one-based stored zero-based; EDGE local zero-based0..3, int-representable. Constant finite
 * signed head in project length or total signed edge flow m3/s, retained for later conversion;
 * series name nonempty.
 * - Argument rows (in,out): Existing retained prescriptions; appends complete row if cell/edge
 * pair unique.
 * @return Empty on success, otherwise grammar/index/kind/value/duplicate diagnostic.
 * @note Defers actual cell shape/exterior/coverage/table/physical checks. No water/native/clock
 * advance; allocation can throw, exclusive authoring access. Declaration calls t tokens.
 */
std::string parseAquiferBoundaryLine(const std::vector<std::string>& tokens,
                                     std::vector<GwBoundaryRow>& rows);

/**
 * @brief Register aquifer option/cell/node/link/boundary and surface-ownership authoring
 * callbacks.
 * - Argument cfg (in,out): Borrowed target configuration; must remain stable/alive through
 * callback use.
 * - Argument node_names (in,out): Borrowed parallel native-node name store.
 * - Argument link_names (in,out): Borrowed parallel conduit name store.
 * - Argument registry (in,out): Receives six custom section handlers, conflict policy delegated.
 * @note Aquifer wrappers mark options.authored, tokenize nonblank lines, stop at first
 * diagnostic with code5/raw line while preserving prior rows. Surface-ownership wrapper omits
 * authored marker and raw-line suffix. No reset/rollback/runtime ownership activation.
 * Allocation/row exceptions can propagate and registration may be partial; serial authoring
 * lifecycle required.
 */
void registerSubsurfaceSections(SubsurfaceConfig& cfg, std::vector<std::string>& node_names,
                                std::vector<std::string>& link_names,
                                input::SectionRegistry& registry);

/**
 * @brief The factors for this project's unit system.
 *
 * @details Resolve authored aquifer units into SI kernel conversion factors.
 * - Argument ctx (in): Stable flow-unit system selector, read only.
 * @return For US: length .3048 m/ft, rate .0254/3600 m/s per in/hr, inverse-length1/.3048, area
 * .3048^2; metric: length/area/inverse1, rate .001/3600 m/s per mm/hr.
 * @note No conversion of actual rows, model mutation, allocation or physical/calendar advance.
 */
GwUnitFactors gwUnitFactors(const SimulationContext& ctx) noexcept;

/**
 * @brief Resolve node names to indices and each bed's cell to a mesh cell; locate `CELL AUTO`
 * rows and (G-X2, `NODE_ENROLMENT AUTO`) enrol every node whose [COORDINATES] fall in a cell,
 * appending to @p cfg.node_beds and @p names.
 * @p node_xy_to_mesh converts node coordinates (project map units) into the mesh's coordinates
 * (SI metres once the router has scaled it). Returns diagnostics; empty on success.
 *
 * @details Resolve native-node bed identity/location and optional coordinate-based automatic
 * enrollment.
 * - Argument ctx (in,out): Matching native node-name/coordinate/LID metadata; appends advisory
 * on enrollment.
 * - Argument mesh (in): Stable prepared cell geometry/vertex indices and consistent finite
 * coordinates.
 * - Argument cfg (in,out): Retained aquifer rows/beds; replaces resolved node/cell on existing
 * beds and may append automatic beds. Authored exchange flags preserved, sealed native LIDs
 * suppress effective checks.
 * - Argument names (in,out): Parallel bed node names, appended with automatic rows; existing
 * entries used for native identity resolution, absent entry treated empty name.
 * - Argument node_xy_to_mesh (in): Positive finite project-map-length to mesh-coordinate factor,
 * normally authored ft/m to already SI mesh m.
 * @return All invalid explicit aquifer cells, missing native nodes/coordinates/cells and
 * duplicate node-bed diagnostics; empty on these checks succeeding. Does not fully validate
 * thermal/native profiles.
 * @note Empty config returns. AUTO locates first containing cell by binned ray casting.
 * Enrollment runs only if prior checks have no errors and node_auto enabled; explicit opt-out
 * still blocks auto. No row unit conversion/water/clock advance. Resolution can mutate even on
 * diagnostics; allocation may leave parallel appends partly complete. Exclusive authoring/setup
 * access required.
 */
std::vector<std::string> resolveSubsurface(SimulationContext& ctx, const MeshData& mesh,
                                           SubsurfaceConfig& cfg, std::vector<std::string>& names,
                                           double node_xy_to_mesh);

struct MeshPolylineInterval {
    int cell = -1;
    double begin = 0, end = 0; ///< SI chainage along the complete polyline, m.
    double share = 1;          ///< coincident shared-edge ownership, never added length.
};
struct MeshPolylineMapping {
    std::string error;
    double length = 0, covered_length = 0;
    bool cancelled = false;
    std::size_t interval_count = 0, contact_cells = 0;
    std::vector<MeshPolylineInterval> intervals;
};
/**
 * @brief Exact convex-cell clipping with along-polyline intervals. Shared-edge ownership is
 * divided only over that interval; ordinary interior lengths are not diluted. Reuses the
 * hydraulic locator without changing its path. Invalid points or overlapping mesh interiors
 * return an error and no rows.
 *
 * @details Clip a polyline into exact convex-cell chainage intervals with local shared-edge
 * ownership.
 * @note The unnamed mesh reference is stable validated finite convex CCW geometry in the x/y
 * coordinate frame, borrowed only for this call; output retains indices, not references.
 * - Argument x (in): Polyline x coordinates, finite, same extent as y and at least two points;
 * SI m for normal runtime/output chainage convention.
 * - Argument y (in): Matching y coordinates.
 * - Argument progress (in): Optional callback receiving integer percent0..100; true requests
 * cancellation (the ownership preview callback uses the opposite convention). Exceptions
 * propagate.
 * - Argument first_interval (in): Zero-based first generated ownership interval to retain.
 * - Argument max_intervals (in): Maximum retained interval rows; all intervals still
 * counted/validated.
 * - Argument count_contact_cells (in): True to count distinct contacted cells across all
 * generated intervals, independent of retained page; false leaves contact_cells zero.
 * @return Owned mapping with complete polyline length/covered union length, total interval
 * count, optional distinct-cell count and requested page. Chainage in input coordinate length
 * units; share 1/number-of-coincident-boundary-owners divides ownership without adding covered
 * length. Empty error means geometry accepted unless cancelled. Invalid geometry/overlapping
 * interiors gives error/no rows; other partial metadata can remain on error. Cancellation clears
 * rows/count/contact/covered length and sets cancelled, but may retain traversed full length.
 * Zero-length segments skipped.
 * @note No hydraulic/native/clock mutation; constructor/clipping allocates and may throw. Cell
 * edge clipping requires convex CCW prepared mesh; nonfinite/overflow polyline is refused.
 * Serial callback and stable mesh required, no background resources retained.
 */
MeshPolylineMapping
mapMeshPolyline(const MeshData&, const std::vector<double>& x, const std::vector<double>& y,
                const std::function<bool(int)>& progress = {}, std::size_t first_interval = 0,
                std::size_t max_intervals = std::numeric_limits<std::size_t>::max(),
                bool count_contact_cells = false);

/**
 * @brief G-X3 (2026-09-19): the length-weighted (conduit, cell) shares for every conduit with a
 * [LOSSES] seepage rate whose polyline crosses the mesh (`LINK_SEEPAGE AUTO`). @p n_conduits
 * receives how many conduits got at least one share. Same coordinate factor as
 * resolveSubsurface.
 *
 * @details Resolve conduit polylines into normalized aquifer cell shares and vertical contact
 * reaches.
 * - Argument ctx (in,out): Prepared link/name/conduit/node/spatial tables; appends warnings for
 * unknown or non-conduit explicit rows.
 * - Argument mesh (in): Stable validated finite convex CCW cell geometry, consistent with
 * converted map frame.
 * - Argument cfg (in,out): Retained conduit overrides, whose resolved link indices are replaced;
 * exchange opt-out honored. DEFAULT mesh mode chooses two-way coefficient eligibility, legacy
 * AUTO stays one-way.
 * - Argument link_names (in,out): Parallel authored names, read without mutation; absent treated
 * empty.
 * - Argument node_xy_to_mesh (in): Positive finite conversion from authored map to mesh
 * coordinate length.
 * - Argument n_conduits (out): Reset zero, incremented for each eligible conduit with positive
 * clipped coverage.
 * @return Owned per-(link,cell) dimensionless shares plus contact fractions and SI endpoint
 * bed-elevation reaches. Denominator=max(total polyline length,sum clipped length) prevents
 * aggregate overbooking when shared edges appear in both cells. Output follows native link
 * order, then candidate discovery.
 * @note Eligible positive legacy seepage or two-way authored KC, coordinates and actual conduit
 * subtype are required; missing geometry/ineligible links skipped. Native inverts converted ft
 * to m, optional DC project length subtracted. Does not apply water/native seepage or require
 * aquifer coverage (caller filters that later). No full overlap/nonfinite repair; allocations
 * may throw after name/warning edits. Requires exclusive setup/model identity; no
 * engine/calendar advance.
 */
std::vector<GwLinkShare> resolveLinkSeepage(SimulationContext& ctx, const MeshData& mesh,
                                            SubsurfaceConfig& cfg,
                                            std::vector<std::string>& link_names,
                                            double node_xy_to_mesh, int& n_conduits);

/**
 * @brief The area-weighted (subcatchment, cell) shares for every subcatchment whose [Polygons]
 * overlap the mesh. Shares are resolved for all of them; the router skips a subcatchment with a
 * lumped [GROUNDWATER] aquifer at delivery. @p n_subcatch receives how many subcatchments got at
 * least one share. Same coordinate factor as resolveSubsurface.
 *
 * @details Compute normalized polygon-area shares from every subcatchment footprint to mesh
 * cells.
 * - Argument ctx (in): Stable authored subcatchment polygon x/y vectors and count.
 * - Argument mesh (in): Valid finite convex CCW cell geometry in the converted map frame.
 * - Argument node_xy_to_mesh (in): Positive finite authored map to mesh length factor.
 * - Argument n_subcatch (out): Reset zero, incremented for each subcatchment with positive
 * clipped coverage.
 * @return Owned (zero-based subcatch,cell,weight) rows with denominator=max(polygon area,sum
 * cell intersection area), so total share<=1. No lumped-aquifer exclusion here; delivery owner
 * chooses scope.
 * @note Uses common prefix of mismatched polygon x/y arrays, skips fewer-than3/nonpositive area,
 * stops when a whole polygon entry missing. No full geometry/finite validation beyond assumed
 * prepared inputs. Allocates/clips read-only; no water/native/source ownership activation or
 * clock update.
 */
std::vector<GwSubcatchShare> resolveSubcatchInfiltration(const SimulationContext& ctx,
                                                         const MeshData& mesh,
                                                         double node_xy_to_mesh, int& n_subcatch);

/**
 * @brief Write the three sections back in the project's own units — that is, the authored values
 * verbatim — omitting anything at its default so a round-trip with no edits adds nothing.
 *
 * @details Append retained aquifer/ownership/boundary authoring sections in project units.
 * - Argument cfg (in): Stable retained authored config, not the converted live kernel copy.
 * - Argument node_names (in): Parallel bed native names; missing authored name emits '?'.
 * - Argument link_names (in): Parallel conduit names; missing name emits '?'.
 * - Argument out (in,out): Existing text retained; sections appended with canonical grammar and
 * spacing.
 * - Argument full_precision (in): True uses %.17g doubles for identity/roundtrip detail; false
 * %.6g for human-readable authoring. Neither performs unit conversion or validation.
 * @note Writes ownership/boundaries even before cfg.empty early return. Omits default options,
 * retains explicit nondefault rows, skips automatic node beds so AUTO can re-enroll them later;
 * CELL restored one-based, boundary edge remains zero-based. Allocation can leave appended
 * prefix. No file I/O, physical/native/clock changes; exclusive output buffer and stable
 * authoring views.
 */
void writeSubsurfaceSections(const SubsurfaceConfig& cfg,
                             const std::vector<std::string>& node_names,
                             const std::vector<std::string>& link_names, std::string& out,
                             bool full_precision = false);

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_SUBSURFACE_SECTIONS_HPP
