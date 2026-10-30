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
 * @file      Inflow.hpp
 * @brief     External inflows, dry weather flows, and RDII at nodes.
 *
 * @details SoA layout for batch processing:
 *   - External inflows: per-node timeseries lookup + baseline pattern
 *   - DWF: per-node average value × monthly × daily × hourly/weekend patterns
 *   - All pattern lookups are table reads — vectorisable as batch gathers
 *
 * @note Legacy reference: src/legacy/engine/inflow.c
 * @ingroup new_engine
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_INFLOW_HPP
#define OPENSWMM_INFLOW_HPP

#include <cstdint>
#include <vector>

namespace openswmm {

struct SimulationContext;
class SWMMEngine;

namespace inflow {

// ============================================================================
// Pattern types
// ============================================================================

constexpr int MONTHLY_PATTERN = 0;
constexpr int DAILY_PATTERN = 1;
constexpr int HOURLY_PATTERN = 2;
constexpr int WEEKEND_PATTERN = 3;

// ============================================================================
// Per-node external inflow definition (SoA)
// ============================================================================

/// `[INFLOWS]` row kind. A row is either the node's direct flow hydrograph or
/// a pollutant load riding on it — the two must not be summed together.
/// @see Legacy: FLOW_INFLOW / CONCEN_INFLOW / MASS_INFLOW in enums.h
enum class ExtInflowKind : int {
    FLOW = 0,   ///< Volumetric inflow (constituent FLOW)
    CONCEN = 1, ///< Pollutant concentration; mass rate = value * node flow
    MASS = 2,   ///< Pollutant mass rate directly
    /// Z1 (amendment D-Y4): a row naming the reserved species
    /// `__WATER_AGE__`. Its value is the AGE (hours in the file, seconds
    /// after conv_factor) carried by the node's external inflow — it adds
    /// no water and no mass; the EXTERNAL_INFLOW loader books q·age with
    /// this age instead of the source table's.
    AGE = 3,
    /// U2 (2026-09-07): a row naming a reactions-component species.
    /// pollut_idx holds the SPECIES index (0..n_species-1). The load goes to
    /// ReactionData::msx_ext_mass_in, never to the pollutant arrays.
    MSX_CONCEN = 4,
    MSX_MASS = 5
};

struct ExtInflowSoA {
    int count = 0;
    std::vector<int> node_idx;        ///< Which node this inflow applies to
    std::vector<int> ts_idx;          ///< Timeseries index (-1 = none)
    std::vector<int> base_pat_idx;    ///< Baseline pattern index (-1 = none)
    std::vector<double> baseline;     ///< Constant baseline value
    std::vector<double> scale_factor; ///< Timeseries scaling factor
    std::vector<double> conv_factor;  ///< Units conversion factor
    /// Row kind (see ExtInflowKind). Without this every row — including
    /// pollutant rows — was added to the node's flow, injecting phantom water.
    std::vector<int> kind;
    /// Pollutant index for CONCEN/MASS rows, SPECIES index for MSX_CONCEN /
    /// MSX_MASS rows; -1 for FLOW rows (and for a constituent name that
    /// matches no declared pollutant or species).
    std::vector<int> pollut_idx;

    /** @brief Reset every external-inflow row to unresolved/default values.
     * @param n Nonnegative row count with allocatable vectors; unchecked before size_t
     * conversion. Sets count, assigns all parallel arrays (discarding old values),
     * invalidates element borrows as storage changes. Missing indices use -1, factors
     * one and values zero; external kind defaults FLOW, DWF is_flow defaults false.
     * Allocation failure propagates and can leave partially reset arrays. Caller must
     * populate/validate matching rows before runtime evaluation. */
    void resize(int n);
};

// ============================================================================
// Per-node dry weather flow definition (SoA)
// ============================================================================

struct DwfInflowSoA {
    int count = 0;
    std::vector<int> node_idx;     ///< Which node
    std::vector<double> avg_value; ///< Average DWF value
    std::vector<int> pat_monthly;  ///< Monthly pattern index (-1 = none)
    std::vector<int> pat_daily;    ///< Daily pattern index (-1 = none)
    std::vector<int> pat_hourly;   ///< Hourly pattern index (-1 = none)
    std::vector<int> pat_weekend;  ///< Weekend pattern index (-1 = none)
    /// Constituent is FLOW (legacy TDwfInflow.param == -1). A pollutant DWF
    /// row is a CONCENTRATION — it must never be added as water (legacy
    /// addDryWeatherInflows reads only the FLOW row for the hydrograph).
    std::vector<uint8_t> is_flow;
    /// Pollutant index for a non-FLOW row (-1 when FLOW or unmatched).
    std::vector<int> pollut_idx;
    /// U2: reactions-species index for a row naming a species (-1 else).
    /// Its concentration rides the node's DWF flow into msx_ext_mass_in.
    std::vector<int> msx_idx;

    /** @brief Reset every DWF row to unresolved/default values.
     * @param n Nonnegative row count with allocatable vectors; unchecked before size_t
     * conversion. Sets count, assigns all parallel arrays (discarding old values),
     * invalidates element borrows as storage changes. Missing indices use -1, factors
     * one and values zero; external kind defaults FLOW, DWF is_flow defaults false.
     * Allocation failure propagates and can leave partially reset arrays. Caller must
     * populate/validate matching rows before runtime evaluation. */
    void resize(int n);
};

// ============================================================================
// Time pattern table (12 monthly, 7 daily, 24 hourly values)
// ============================================================================

struct TimePattern {
    int type = 0;            ///< 0=monthly, 1=daily, 2=hourly, 3=weekend
    double factors[24] = {}; ///< Up to 24 factors (monthly=12, daily=7, hourly=24)
};

// ============================================================================
// Inflow solver
// ============================================================================

/** @brief Owned resolved inflow definitions and pattern lookup cache.
 * Serialize init/refresh/compute with model edits and table readers. Per-call
 * outputs are rates, not integrated amounts; routing assembles the decomposed
 * water arrays into lateral flow later. No rollback or per-call error status. */
class InflowSolver {
public:
    /** @brief Rebuild external/DWF row caches, bindings and unit conversions from the model.
     * @param[in,out] ctx Compatible initialized aligned inflow/pattern/node/pollutant/
     * reaction data with valid flow units [0,5]. Replaces private caches, appends unique
     * warnings for unsupported/unresolved external constituents and resets per-node
     * external inflow ages to NaN when water-age tracking is enabled. Does not compute
     * current hydraulic inflow or clear context water/quality accumulators.
     * Pattern names resolve case insensitively; up to four DWF patterns are reordered
     * by type, last repeated type wins. Pollutant lookup precedes case-insensitive FLOW
     * prefix matching. Exact reserved age rows carry hours converted to seconds;
     * reserved temperature rows are warned/inert. Reactions species bind separately;
     * wall species are ignored during evaluation. External MASS factors divide by
     * 28.317; flow converts display units to cfs. Last repeated node/constituent row
     * supersedes earlier rows across concentration/mass variants; unresolved inert rows
     * do not suppress a flow. Missing series/pattern bindings silently use zero/one.
     * Call after definition/name/count changes. No general physical/index validation,
     * atomic rollback or exception translation is supplied. */
    void init(SimulationContext& ctx);

    /** @brief Copy current pattern types/factors into private fixed-width runtime rows.
     * @param ctx Compatible initialized pattern names/types/factor arrays; borrowed only.
     * Resizes cache to count, initializes each twenty-four-slot row to one, then copies
     * at most twenty-four supplied factors. Does not validate values, re-resolve inflow
     * bindings or sort DWF rows. Stable name/index identity is required for a refresh;
     * use init() after count/name/order/binding changes. Allocations can throw and leave
     * partial cache updates. Subsequent computeAll sees copied factors; earlier model
     * edits alone do not change this cache. */
    void refreshPatterns(const SimulationContext& ctx);

    /** @brief Add resolved external/DWF water and quality rates to routing input channels.
     * @param[in,out] ctx Compatible initialized model matching cached definitions, with
     * aligned node water/pollutant/species/age arrays, valid units and sorted source
     * tables. Caller clears ext_inflow, dwf_inflow, ext_qual_mass and dwf_qual_mass for
     * this routing step before calling; this method scatter-adds into them, not lat_flow.
     * Selected table cursors advance. Reactions mass-rate storage is resized/reset at
     * the external-species seam then receives external, DWF and nonbooster source loads;
     * reaction source indices/pattern arrays and positive timestep must be qualified.
     * @param current_date Finite supported absolute SWMM/OLE day, not elapsed seconds.
     * Calendar pattern lookup requires nonnegative valid month/day/hour indices.
     * @param dt Unused compatibility step seconds; values are instantaneous rates and
     * this method does not integrate them or advance simulation clocks.
     * FLOW rows establish cfs first (absolute values below FLOW_TOL become zero), then
     * CONCEN multiplies that external water and MASS supplies a direct load. DWF uses
     * monthly*daily*(hourly or weekend), positive water for quality and subtracts the
     * pollutant default displaced by a node concentration. Wall species are skipped;
     * nonbooster reaction sources use positive external plus DWF water or declared
     * mass/28.317. Ages overwrite the per-node age in seconds without adding water.
     * No finite/range validation, transaction rollback or success diagnostic; allocations
     * and lookup exceptions propagate and can follow partial accumulator mutation. */
    void computeAll(SimulationContext& ctx, double current_date, double dt);

private:
    friend class openswmm::SWMMEngine;
    ExtInflowSoA ext_inflows_;
    DwfInflowSoA dwf_inflows_;
    std::vector<TimePattern> patterns_;

    /** @brief Read a cached pattern factor selected by its stored type.
     * @param pat_idx Cached pattern index; missing/out-of-range returns one.
     * @param month Nonnegative zero-based month, indexed modulo twelve for monthly type.
     * @param day Nonnegative zero-based weekday (Sunday zero), modulo seven for daily type.
     * @param hour Nonnegative hour, modulo twenty-four for hourly/weekend types. This
     * helper does not decide whether the date is a weekend; computeAll chooses the row.
     * @return Stored dimensionless multiplier or one for unknown type. No finite/range
     * validation, interpolation or cache mutation; negative modulo indices are unsupported. */
    double getPatternFactor(int pat_idx, int month, int day, int hour) const;
};

} // namespace inflow
} // namespace openswmm

#endif // OPENSWMM_INFLOW_HPP
