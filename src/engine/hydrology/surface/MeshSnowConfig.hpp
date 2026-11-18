// SPDX-License-Identifier: Apache-2.0

/**
 * @file      MeshSnowConfig.hpp
 * @brief     Configuration and resolution of mesh snow packs and overrides.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include "UebConfig.hpp"

namespace openswmm::surface {
/** @brief One authored pack selection and optional site/geometry override.
 * Missing optionals inherit lower-precedence scope values; explicit zero is a
 * real override. forest is fraction, drift dimensionless, ground_heat W/m2,
 * extinction_depth metres, slope degrees and aspect degrees clockwise north.
 * pack is an unresolved model name or normalized NONE token. */
struct MeshSnowRow {
    std::string pack;
    std::optional<double> forest, drift, ground_heat, extinction_depth, slope, aspect;
};
struct MeshSnowConfig {
    enum class Mode { Auto, Yes, No };
    Mode mode = Mode::Auto;
    double step = 0;                             // inherit the shared SNOW_STEP
    std::map<std::string, MeshSnowRow> defaults; // * whole mesh, otherwise mesh tag
    std::map<int, MeshSnowRow> cells;            // zero-based cell index
    /** @brief Test whether any tag/default/cell assignment rows are stored.
     * @return True for a nonempty map even if mode is No or every row selects NONE.
     * Does not resolve packs, inspect mesh cell count or establish an active snow owner. */
    bool assigned() const { return !defaults.empty() || !cells.empty(); }
};
/** @brief Apply one mesh SNOWMELT/SNOW_STEP option after validating its token form.
 * @param[in,out] config Owned authored options, mutated on accepted value only.
 * Does not clear assignments, rebuild runtime columns or change global SnowConfig.
 * @param tokens Exactly two section-excluded tokens. SNOWMELT accepts AUTO/YES/NO;
 * SNOW_STEP accepts DEFAULT (stores zero/inherit) or shared strict seconds/clock
 * syntax with total [1,21600] seconds. Keywords are case insensitive.
 * @return Empty on success, otherwise unknown-key/syntax/range diagnostic retaining
 * config. Numeric step parser uses a temporary SnowConfig; only mesh step is copied.
 * Caller serializes authoring/runtime rebuild and handles allocation exceptions. */
std::string setMeshSnowOption(MeshSnowConfig& config, const std::vector<std::string>& tokens);
/** @brief Parse and replace one unresolved mesh snow assignment row.
 * @param[in,out] config Maps updated after successful row parsing; same-key assignment
 * replaces the whole prior row, so omitted options on that row are discarded.
 * @param tokens Optional CELL/TAG prefix (matching cell selector), key, SNOWPACK and
 * nonempty pack name, then unique key/value pairs. Cell keys are finite integral
 * one-based IDs in [1,INT_MAX], stored zero-based; tag keys are case-sensitive and
 * may be * for mesh-wide fallback. Tags/pack names cannot contain CR/LF/double quote;
 * caller tokenizes quoted text before this API. NONE is normalized case-insensitively.
 * Overrides: FC fraction[0,1], F drift>=0, QG signed W/m2, AEP positive metres, SLOPE
 * degrees[0,90), ASPECT degrees[0,360]; all finite and slope/aspect must occur together.
 * @param cell True targets numeric cell map, false exact tag/default map.
 * @return Empty on success, otherwise syntax/range/duplicate diagnostic retaining
 * maps. Does not check mesh size, tag existence, pack registry or runtime physical
 * compatibility. Allocation exceptions propagate; resolved inheritance is separate. */
std::string setMeshSnowRow(MeshSnowConfig& config, const std::vector<std::string>& tokens,
                           bool cell);
/** @brief Serialize nondefault mesh snow options without validating them.
 * @param config Coherent authored mode/step configuration.
 * @return Independent rows: YES/NO when mode is non-AUTO, positive step seconds with
 * stream precision17 when overridden. Zero step/inherited default is omitted. Does
 * not emit assignment maps or preserve input spelling/order; uses current stream locale. */
std::vector<std::string> meshSnowOptionRows(const MeshSnowConfig& config);
/** @brief Serialize one assignment map in deterministic key/schema order.
 * @param config Parser-qualified names/cell keys and finite override values; no
 * escaping or validation of manually inserted quotes/invalid indices occurs here.
 * @param cell True writes CELL and one-based numeric IDs in ascending order; false
 * writes quoted tag keys in lexical map order, without a TAG prefix.
 * @return Independent body rows with quoted SNOWPACK name then present FC/F/QG/AEP/
 * SLOPE/ASPECT fields in fixed order, precision17/current locale. Includes NONE
 * assignments and explicit zero overrides; does not expand inherited scopes. */
std::vector<std::string> meshSnowRows(const MeshSnowConfig& config, bool cell);
/** @brief Compose mesh-wide, exact-tag and exact-cell snow overrides.
 * @param config Borrowed authored maps; no runtime mutation or qualification.
 * @param cell Zero-based lookup key; no mesh-bound validation, absent cell just skips.
 * @param tag Exact case-sensitive tag string; no normalization or mesh membership lookup.
 * @return Independent row after *, then matching tag, then cell overlay. Each present
 * row always replaces pack and only its populated optional fields; absent optional
 * fields inherit earlier values, explicit zero replaces. NONE does not erase inherited
 * optional fields. An entirely absent selection returns empty pack/options. Does not
 * apply enabled mode, resolve pack identity or infer slope/aspect from mesh geometry. */
MeshSnowRow resolveMeshSnowRow(const MeshSnowConfig& config, int cell, const std::string& tag);
} // namespace openswmm::surface
