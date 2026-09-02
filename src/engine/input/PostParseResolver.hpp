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
 * @file PostParseResolver.hpp
 * @brief Post-parse cross-reference resolution for the input system.
 *
 * @details After all sections are parsed, some object references that
 *          appear forward (e.g., a subcatchment referencing a gage that
 *          appears later in the file) need to be resolved. This module
 *          performs a single sweep to fix up all -1 indices.
 *
 * Resolutions performed:
 * - `subcatch.gage[i]`     — re-look up by gage name
 * - `subcatch.outlet_node` — re-look up outlet by name
 * - `gages.ts_index`       — re-look up time series by name
 * - `nodes.outfall_param`  for TIDAL/TIMESERIES outfalls — resolve curve index
 * - `links.pump_curve`     — resolve pump curve by name
 *
 * @see InputReader.hpp — calls resolve() after all sections are dispatched
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_POST_PARSE_RESOLVER_HPP
#define OPENSWMM_ENGINE_POST_PARSE_RESOLVER_HPP

#include <string>

namespace openswmm {
    struct SimulationContext;
}

namespace openswmm::input {

/**
 * @brief Resolve all cross-references in ctx after all sections are parsed.
 *
 * @details This is a best-effort pass: references that still can't be
 *          resolved after the full file is read are left as -1 and a
 *          warning is stored in `ctx.warning_code`.
 *
 * @param ctx  Simulation context (mutated in place).
 */
void resolve_cross_references(SimulationContext& ctx);

/**
 * @brief (Re)load every FILE-source rain gage's data from disk.
 *
 * @details Runs as part of resolve_cross_references() during open(). Exposed
 *          separately so an editing host can pick up a changed file path,
 *          station, or units without reopening the model — nothing else
 *          re-runs it, so `swmm_file_path_set` on a rain gage would otherwise
 *          have no effect until the next open and callers would silently keep
 *          reading the previous file's data.
 *
 *          Rebuilds `ctx.gages.rain_series` and the rainfall-file summary
 *          statistics, windowed to the current [OPTIONS] simulation dates.
 */
void load_external_rain_files(SimulationContext& ctx);

/**
 * @brief Convert the input fields the reader scaled to internal units back to
 *        display units (matching @c ctx.options.flow_units).
 *
 * @details Exact inverse of the file-local @c convert_inputs_to_internal that
 *          @ref resolve_cross_references applies at parse time: it multiplies
 *          the same node/link/subcatchment length-, area-, flow-, and
 *          rainfall-dimensioned fields by the forward UCF instead of its
 *          reciprocal. The .inp writer needs display units, but the engine
 *          stores everything internally in feet/cfs, so a save must undo the
 *          parse-time conversion or each save→reopen cycle compounds the
 *          factor (the m→ft "exploding model" bug). US-unit models are a
 *          no-op (factor 1.0).
 *
 *          MUST stay field-for-field in sync with @c convert_inputs_to_internal
 *          in PostParseResolver.cpp — editing one without the other reintroduces
 *          the asymmetry.
 *
 * @param ctx  Simulation context (mutated in place).
 */
void convert_internal_to_display(SimulationContext& ctx);

/**
 * @brief True when the context holds parse-time normalisations that the .inp
 *        writer must undo: LINK_OFFSETS=ELEVATION (offsets stored as depths)
 *        or any conduit reversed for adverse slope (direction == -1).
 */
bool needs_authored_conversion(const SimulationContext& ctx);

/**
 * @brief Undo the parse-time adverse-slope reversal on every conduit with
 *        direction == -1 (node1/node2, offset1/offset2, q0 sign, inlet/outlet
 *        losses, slope sign) and reset direction to +1.
 * @returns Number of conduits restored.
 */
int restore_authored_orientation(SimulationContext& ctx);

/**
 * @brief Restore authored link data for writing: un-reverse adverse-slope
 *        conduits (node1/node2, offset1/offset2, q0 sign, inlet/outlet losses)
 *        and, in ELEVATION offset mode, re-add each node's invert to the
 *        depth-normalised offsets and weir/outlet crests.
 *
 * @details Exact inverse of the reversal and ELEV_OFFSET passes inside
 *          @ref resolve_cross_references. Operate on a copy — the live engine
 *          state must keep its canonical (depth, positive-slope) form.
 *          Vertices are untouched (the reversal never reorders them).
 */
void convert_internal_to_authored(SimulationContext& ctx);

/**
 * @brief Slice IO-3: Resolve every external-file slot's `.original` token
 *        against an anchor directory and populate `.absolute`.
 *
 * @details Walks `ctx.files` (rainfall/runoff/rdii/inflows/outflows/hotstart),
 *          `ctx.gages.file_path[]`, `ctx.options.temp_file`, and every
 *          timeseries `Table::file_path`. For each non-empty slot,
 *          assigns `slot.absolute = io::resolveRelative(slot.original,
 *          anchor_dir)`. Empty slots get an empty `.absolute`.
 *
 *          Called as part of `resolve_cross_references` after all
 *          handlers have run. Exposed here so unit tests can exercise the
 *          resolution pass without building a complete simulation
 *          context.
 *
 *          The `original` token is left untouched — `InpWriter` continues
 *          to consume it verbatim until Slice IO-4 wires the
 *          rebase-on-write step.
 *
 * @param ctx          Simulation context whose external-file slots are
 *                     resolved in place.
 * @param anchor_dir   Directory the `.original` tokens were authored
 *                     against; typically `io::parentDir(ctx.inp_file_path)`.
 *                     Empty means "no anchor" — relative tokens are left
 *                     lexically normalised against an empty base.
 */
void resolve_external_file_slots(SimulationContext& ctx,
                                  const std::string& anchor_dir);

/**
 * @brief Recompute a single conduit's derived dynamic-wave flow properties.
 *
 * @details Recomputes the routing coefficients the dynamic-wave solver
 *          consumes — @c rough_factor, @c beta, @c q_full, @c q_max,
 *          @c volume (and the force-main roughness factor) — from the
 *          conduit's current @c roughness, @c slope, and full-section
 *          geometry. This is the per-conduit body shared by
 *          @ref resolve_cross_references (parse time) and the runtime
 *          geometry setters, so an edit to a conduit's roughness after
 *          @c open() propagates into the simulation instead of leaving
 *          stale parse-time coefficients in place.
 *
 *          Non-conduit links, and conduits with non-positive roughness or
 *          full area, are left untouched.
 *
 * @param ctx  Simulation context (mutated in place).
 * @param j    Link index.
 */
void recompute_conduit_flow_properties(SimulationContext& ctx, int j);

} /* namespace openswmm::input */

#endif /* OPENSWMM_ENGINE_POST_PARSE_RESOLVER_HPP */
