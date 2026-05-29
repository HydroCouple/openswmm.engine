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
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_POST_PARSE_RESOLVER_HPP
#define OPENSWMM_ENGINE_POST_PARSE_RESOLVER_HPP

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

} /* namespace openswmm::input */

#endif /* OPENSWMM_ENGINE_POST_PARSE_RESOLVER_HPP */
