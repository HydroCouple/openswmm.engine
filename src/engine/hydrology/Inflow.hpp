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
 * @license  MIT License
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

struct ExtInflowSoA {
    int count = 0;
    std::vector<int>    node_idx;       ///< Which node this inflow applies to
    std::vector<int>    ts_idx;         ///< Timeseries index (-1 = none)
    std::vector<int>    base_pat_idx;   ///< Baseline pattern index (-1 = none)
    std::vector<double> baseline;       ///< Constant baseline value
    std::vector<double> scale_factor;   ///< Timeseries scaling factor
    std::vector<double> conv_factor;    ///< Units conversion factor

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
