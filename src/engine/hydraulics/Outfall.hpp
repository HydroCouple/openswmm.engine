/**
 * @file Outfall.hpp
 * @brief Outfall boundary depth computation — free/normal/fixed/tidal/timeseries.
 *
 * @details Computes the water depth at outfall nodes based on boundary
 *          condition type. Called before each routing step.
 *
 * @note Legacy reference: src/legacy/engine/node.c (outfall_setOutletDepth)
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_OUTFALL_HPP
#define OPENSWMM_OUTFALL_HPP

namespace openswmm {

struct SimulationContext;

namespace outfall {

/**
 * @brief Compute boundary depth at all outfall nodes.
 *
 * @details For each outfall:
 *   - FREE: critical depth from downstream conduit
 *   - NORMAL: normal depth from downstream conduit
 *   - FIXED: specified water surface elevation
 *   - TIDAL: elevation from tidal curve at current time
 *   - TIMESERIES: elevation from timeseries at current time
 *
 * @param ctx           Simulation context.
 * @param current_time  Current simulation time (decimal days).
 */
void setAllOutfallDepths(SimulationContext& ctx, double current_time);

} // namespace outfall
} // namespace openswmm

#endif // OPENSWMM_OUTFALL_HPP
