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
 * @file Inflow.hpp
 * @brief External inflows, dry weather flows, and RDII at nodes.
 *
 * @details SoA layout for batch processing:
 *   - External inflows: per-node timeseries lookup + baseline pattern
 *   - DWF: per-node average value × monthly × daily × hourly/weekend patterns
 *   - All pattern lookups are table reads — vectorisable as batch gathers
 *
 * @note Legacy reference: src/legacy/engine/inflow.c
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_INFLOW_HPP
#define OPENSWMM_INFLOW_HPP

#include <vector>

namespace openswmm {

struct SimulationContext;

namespace inflow {

// ============================================================================
// Pattern types
// ============================================================================

constexpr int MONTHLY_PATTERN = 0;
constexpr int DAILY_PATTERN   = 1;
constexpr int HOURLY_PATTERN  = 2;
constexpr int WEEKEND_PATTERN = 3;

// ============================================================================
// Per-node external inflow definition (SoA)
// ============================================================================

/// `[INFLOWS]` row kind. A row is either the node's direct flow hydrograph or
/// a pollutant load riding on it — the two must not be summed together.
/// @see Legacy: FLOW_INFLOW / CONCEN_INFLOW / MASS_INFLOW in enums.h
enum class ExtInflowKind : int {
    FLOW   = 0,  ///< Volumetric inflow (constituent FLOW)
    CONCEN = 1,  ///< Pollutant concentration; mass rate = value * node flow
    MASS   = 2   ///< Pollutant mass rate directly
};

struct ExtInflowSoA {
    int count = 0;
    std::vector<int>    node_idx;       ///< Which node this inflow applies to
    std::vector<int>    ts_idx;         ///< Timeseries index (-1 = none)
    std::vector<int>    base_pat_idx;   ///< Baseline pattern index (-1 = none)
    std::vector<double> baseline;       ///< Constant baseline value
    std::vector<double> scale_factor;   ///< Timeseries scaling factor
    std::vector<double> conv_factor;    ///< Units conversion factor
    /// Row kind (see ExtInflowKind). Without this every row — including
    /// pollutant rows — was added to the node's flow, injecting phantom water.
    std::vector<int>    kind;
    /// Pollutant index for CONCEN/MASS rows; -1 for FLOW rows (and for a
    /// constituent name that matches no declared pollutant).
    std::vector<int>    pollut_idx;

    void resize(int n);
};

// ============================================================================
// Per-node dry weather flow definition (SoA)
// ============================================================================

struct DwfInflowSoA {
    int count = 0;
    std::vector<int>    node_idx;       ///< Which node
    std::vector<double> avg_value;      ///< Average DWF value
    std::vector<int>    pat_monthly;    ///< Monthly pattern index (-1 = none)
    std::vector<int>    pat_daily;      ///< Daily pattern index (-1 = none)
    std::vector<int>    pat_hourly;     ///< Hourly pattern index (-1 = none)
    std::vector<int>    pat_weekend;    ///< Weekend pattern index (-1 = none)

    void resize(int n);
};

// ============================================================================
// Time pattern table (12 monthly, 7 daily, 24 hourly values)
// ============================================================================

struct TimePattern {
    int type = 0;               ///< 0=monthly, 1=daily, 2=hourly, 3=weekend
    double factors[24] = {};    ///< Up to 24 factors (monthly=12, daily=7, hourly=24)
};

// ============================================================================
// Inflow solver
// ============================================================================

class InflowSolver {
public:
    void init(SimulationContext& ctx);

    /**
     * @brief Re-copy time-pattern factors from the context into the solver's
     *        per-step lookup cache.
     *
     * @details ::init copies @c ctx.patterns into a private cache for fast
     * per-step DWF/external-inflow scaling. A runtime pattern edit
     * (@c swmm_pattern_set_factors) mutates @c ctx.patterns only, so the C API
     * calls this to refresh the cache and let mid-run pattern changes take
     * effect on the next step. Pattern count is assumed unchanged.
     *
     * @param ctx Simulation context holding the authoritative pattern data.
     */
    void refreshPatterns(const SimulationContext& ctx);

    /**
     * @brief Compute all external + DWF inflows and add to node lateral flow.
     *
     * @details Batch operations:
     *   1. Batch timeseries lookup for all ext inflows (gather from tables)
     *   2. Batch pattern factor computation (vectorisable index arithmetic)
     *   3. Batch multiply: inflow = conv * (ts_value * scale + baseline * pat)
     *   4. Scatter-add to node lat_flow array
     *
     * @param ctx           Simulation context.
     * @param current_date  Current absolute date (decimal days).
     * @param dt            Timestep (seconds).
     */
    void computeAll(SimulationContext& ctx, double current_date, double dt);

private:
    ExtInflowSoA ext_inflows_;
    DwfInflowSoA dwf_inflows_;
    std::vector<TimePattern> patterns_;

    /// Get pattern factor for a given pattern index, month, day, hour.
    double getPatternFactor(int pat_idx, int month, int day, int hour) const;
};

} // namespace inflow
} // namespace openswmm

#endif // OPENSWMM_INFLOW_HPP
