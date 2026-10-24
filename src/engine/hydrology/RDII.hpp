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
 * @file      RDII.hpp
 * @brief     RDII — rainfall-dependent infiltration/inflow via unit hydrograph.
 *
 * @details Each RDII node has 3 triangular unit hydrographs (short/medium/long
 *          response). The RDII inflow at each timestep is the convolution of
 *          past rainfall with the UH ordinates.
 *
 *          SoA: past rainfall stored as circular buffer per UH group.
 *          Convolution is a dot-product — vectorisable.
 *
 * @note Legacy reference: src/legacy/engine/rdii.c
 * @ingroup new_engine
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_RDII_HPP
#define OPENSWMM_RDII_HPP

#include <array>
#include <vector>
#include <string>
#include <unordered_map>

#include "../core/StringCase.hpp"

namespace openswmm {

struct SimulationContext;

namespace rdii {

struct UnitHydParams {
    double r[12][3] = {};       ///< Rainfall fraction per month × response
    double tPeak[12][3] = {};   ///< Time to peak (sec) per month × response
    double tBase[12][3] = {};   ///< Base time (sec) per month × response
    double iaMax[12][3] = {};   ///< Maximum abstraction, project rain-depth units
    double iaRecov[12][3] = {}; ///< Linear recovery, project rain-depth units/day
    double iaInit[12][3] = {};  ///< Initial used abstraction, project rain-depth units
};

/// Exponential IA decay parameters for one RDII response (SHORT/MEDIUM/LONG).
/// When `active` is true, this response uses the temperature-dependent
/// exponential IA model in place of the legacy linear iaRecov logic.
/// Coefficients use project depth units for k_dep (consistent with iaMax),
/// hours for k_0/k_T, and degrees Celsius for temperature thresholds.
/// @see docs/RDII_ExpDecay_Implementation.md
struct ExpDecayParams {
    bool active = false;
    double k_dep = 0.0;     ///< Depletion rate (1/project-depth) — temperature-independent
    double k_0 = 0.0;       ///< Base recovery rate (1/hr)
    double k_T = 0.0;       ///< Thermal recovery rate at T_ref (1/hr)
    double T_ref = 10.0;    ///< Reference temperature (deg C)
    double theta_rec = 0.0; ///< Temperature sensitivity (1/deg C)
    double T_freeze = 0.0;  ///< Recovery suppressed below this temperature (deg C)

    // Optional degree-day snow model. When `snow_on`, precipitation at
    // T <= snow_T accumulates as SWE with no liquid input; at T > snow_T,
    // melt = min(SWE, snow_ddf*(T - snow_T)*dt_days) is added to rainfall
    // (rain-on-snow) before the IA depletion step.
    bool snow_on = false;  ///< Degree-day snow model enabled
    double snow_T = 1.0;   ///< Rain/snow threshold & melt base (deg C)
    double snow_ddf = 0.0; ///< Degree-day melt factor (project-depth/degC/day)
};

/// Per-response (SHORT/MEDIUM/LONG) unit hydrograph data.
/// Legacy equivalent: TUHData uh[3] inside TUHGroup.
struct UHResponseData {
    std::vector<double> past_rain; ///< circular buffer of past rainfall depths
    std::vector<int> past_month;   ///< month for each past rainfall entry
    int period = 0;                ///< current buffer write position
    int max_periods = 0;           ///< buffer capacity
    int has_past_rain = 0;         ///< true if any non-zero past rain
    double ia_used = 0.0;          ///< initial abstraction used so far
    double swe = 0.0;              ///< snow water equivalent (project depth; degree-day snow model)
    long dry_seconds = 0;          ///< seconds since last non-zero rainfall

    /** @brief Reset one response's circular rainfall/month buffers and abstraction/SWE state.
     * @param n Nonnegative buffer capacity fitting vector sizes and long dry-time arithmetic.
     * Installs zero rain/month entries, write position zero, no past rain, zero IA/SWE
     * and initial dry_seconds=n*300+1. init later adjusts this using actual rain interval.
     * Allocation failure can leave a partial reset; negative n is not checked. */
    void allocate(int n) {
        max_periods = n;
        past_rain.assign(static_cast<std::size_t>(n), 0.0);
        past_month.assign(static_cast<std::size_t>(n), 0);
        period = 0;
        has_past_rain = 0;
        ia_used = 0.0;
        swe = 0.0;
        dry_seconds = static_cast<long>(n) * 300 + 1; // start dry
    }
};

/** @brief Compute exponential-IA additive recovery at one air temperature.
 * @param dp Finite qualified 1/hour base/thermal rates, Celsius reference/freeze
 * thresholds and inverse-Celsius sensitivity; active is not tested here.
 * @param T_celsius Finite air temperature Celsius.
 * @return Zero strictly below freeze threshold; otherwise max(0,k_0+k_T*exp(theta_rec
 * *(T-T_ref))) in 1/hour. Equality at freeze permits recovery. Does not clamp exponent
 * overflow or validate parameters, consume time or mutate state. */
double getRecoveryRate(const ExpDecayParams& dp, double T_celsius);

/** @brief Update exponential abstraction and optional degree-day SWE for one rain chunk.
 * @param uh Qualified monthly response parameters; iaMax is project rain-depth units.
 * @param[in,out] rd Current used-abstraction and optional SWE in the same project
 * units. Does not modify rainfall circular buffers/dry counters.
 * @param dp Qualified exponential/snow coefficients; active is not checked here.
 * @param month Nonnegative zero-based month, reduced modulo 12.
 * @param response Nonnegative SHORT/MEDIUM/LONG index, reduced modulo three.
 * @param rainDepth Finite nonnegative precipitation depth in project rain-depth units.
 * @param dt_sec Finite positive chunk seconds.
 * @param ctx Borrowed climate state; configured temperature is Fahrenheit converted
 * to Celsius, otherwise uses dp.T_ref without sampling any new climate time.
 * @return Nonnegative liquid excess in project depth units. Optional snow accumulates
 * all input at T<=snow_T (then follows recovery); above threshold, bounded SWE melt
 * is added before wet abstraction. Wet available IA decays exp(-k_dep*rainDepth),
 * used IA increases by the computed change, and returned excess is clipped to zero.
 * The computed abstraction is not additionally bounded by rainDepth, so this helper
 * does not guarantee per-chunk mass closure for arbitrary coefficients. Dry IA
 * recovers exponentially in hours, suppressed strictly below freeze. No argument/
 * finite/overflow validation, transaction or global water/clock bookkeeping. */
double updateIA_exp(const UnitHydParams& uh, UHResponseData& rd, const ExpDecayParams& dp,
                    int month, int response, double rainDepth, double dt_sec,
                    const SimulationContext& ctx);

struct RDIIGroupSoA {
    int count = 0;
    std::vector<int> node_idx; ///< Target node index
    std::vector<int> uh_idx;   ///< Unit hydrograph parameter index
    std::vector<int> gage_idx; ///< Rain gage index per UH group (legacy: UnitHyd[j].rainGage)
    std::vector<double> area;  ///< Contributing area, project land-area units (acres/ha)

    /// Per-response data: [group * 3 + response]
    std::vector<UHResponseData> uh_data;

    std::vector<int> rain_interval; ///< Rain processing interval (sec) per group

    // Strict-grid driver state (legacy createRdiiFile embedded at runtime).
    // Chunk starts advance in rain_interval steps from simulation start;
    // rates are recorded per chunk while the runoff substep containing the
    // chunk's start is current (the substep never crosses a gage entry
    // boundary, so the current gage rate IS the rate at the chunk start).
    std::vector<double> gage_elapsed;               ///< Chunk cursor (sec, legacy UHGroup.gageDate)
    std::vector<double> rate_recorded_until;        ///< Chunk starts with recorded rates (sec)
    std::vector<std::vector<double>> pending_rates; ///< FIFO of recorded chunk rates

    /** @brief Reset group metadata/queues and resize response objects.
     * @param n Nonnegative assignment count with 3*n and allocations representable.
     * Sets indices -1, areas/clocks zero, interval 300 s and empty queues. Existing
     * UHResponseData entries survive resize (their buffers/IA/SWE are not reset here);
     * init allocates active responses separately. Allocation failure can leave a prefix. */
    void resize(int n);
};

/** @brief Mutable strict-grid UH convolution, rainfall queues and node-flow history.
 * Configure validated UH/decay/assignment rows before init. Serialize all access;
 * advance mutates queues, abstraction, optional SWE and context balance totals.
 * Stored float32 grid rows grow for the whole run; no streaming eviction/rollback.
 * Direct public uh_params/decay_params edits require compatible layout and a fresh
 * initialization before reuse. Exceptions can leave partial changes. */
class RDIISolver {
public:
    /** @brief Resolve authored UH/gage/decay assignments and reset the strict-grid driver.
     * @param[in,out] ctx Valid parsed monthly response indices, assignments/gages/node
     * indices, unit enums, start date and finite positive wet_step. Appends warnings for
     * repeated rows/missing temperature/degenerate decay; hydraulic arrays are not filled.
     * Copies rows into existing UH registry (does not clear previously registered groups
     * or unassigned monthly slots), truncates peak/base hours to whole seconds, resets
     * decay rows and builds per-assignment rainfall buffers with starting-month IA.
     * Resets emitted grid, tick clock and selected-node columns. With no assignments it
     * only sets group count zero, retaining old group vectors/latest node-flow buffer;
     * with invalid/no-period responses some prior response storage may remain unused.
     * Requires fresh/compatible lifetime and prevalidated extents; no failure rollback.
     * Does not call climate/gage updates, emit time zero or advance simulation clocks. */
    void init(SimulationContext& ctx);

    /** @brief Copy a named monthly UH parameter table into the registry.
     * @param name Copied case-insensitive key; existing key replaces its table in place.
     * @param params Complete qualified 12-month by three-response array; no validation.
     * @return Existing/new zero-based index. New insertion appends before map assignment,
     * so allocation failure can leave an unindexed appended table. Does not rebuild
     * active group buffers/grid or align decay_params; configure before init. */
    int addUnitHydParams(const std::string& name, const UnitHydParams& params);

    /** @brief Find a registered UH parameter table by case-insensitive name.
     * @param name Borrowed key; does not insert.
     * @return Zero-based stored index or -1. No table/active-group layout validation. */
    int findUnitHyd(const std::string& name) const;

    /// UH name → index map type; case-insensitive (legacy hash.c parity).
    using UhNameMap = std::unordered_map<std::string, int, CiHash, CiEqual>;

    /** @brief Borrow the case-insensitive UH registry index.
     * @return Member map reference until destruction; init/add can change/reallocate
     * entries. Serialize with mutation and copy a snapshot before retaining iterators.
     * The map alone does not certify active response/grid initialization. */
    const UhNameMap& uhNameIndex() const { return uh_name_to_idx_; }

    /** @brief Record current rain chunks and emit every reached fixed wet-step grid tick.
     * @param[in,out] ctx Matching initialized model/gages/climate and balance totals.
     * Current gage rainfall must already include monthly adjustment in project depth/hour;
     * climate temperature must match each processed chunk's intended forcing. Accumulates
     * RDII rainfall/produced volumes in ft3; does not mutate node inflow arrays.
     * @param new_elapsed_sec Finite nonnegative, monotonically nondecreasing END seconds
     * of a runoff substep capped at every gage boundary. Current gage rate is recorded
     * for chunk starts strictly before this endpoint. Caller guarantees bounded run size.
     * No-op for zero groups/nonpositive grid step. Emits inclusive grid ticks starting at
     * zero; legacy sampling uses the first pending chunk rate for every chunk of a tick.
     * Retains all float32 rows and latest node flow. Does not advance global simulation
     * clocks, rescan historical gages, validate time ordering or roll back partial queues/
     * balances on allocation/error; repeated same time ordinarily emits nothing new. */
    void advance(SimulationContext& ctx, double new_elapsed_sec);

    /** @brief Add an emitted zero-order-held RDII row into the decomposed node inflow array.
     * @param[in,out] ctx Matching initialized node indices/rdii_inflow; existing values
     * are incremented, so caller clears decomposition once per routing interval.
     * @param elapsed_sec Finite routing START seconds, with floor(time/grid_step) safely
     * representable. Negative or beyond emitted coverage contributes nothing.
     * Uses stored float32 cfs and skips abs(q)<FLOW_TOL. Does not generate missing rows,
     * write nodes.lat_flow, advance clocks or update RDII continuity; caller assembles
     * lateral totals afterward. Serial read relative to advance/init is required. */
    void applyRdiiInflows(SimulationContext& ctx, double elapsed_sec) const;

    std::vector<UnitHydParams> uh_params;

    /// Exponential decay params per UH group × response.
    /// Same indexing as uh_params; each entry holds [SHORT, MEDIUM, LONG].
    /// `active == false` means the response uses the legacy linear IA model.
    std::vector<std::array<ExpDecayParams, 3>> decay_params;

    /** @brief Borrow the latest emitted strict-grid node-flow row.
     * @return Member per-node cfs values quantized through float32; not a lookup at a
     * caller-selected routing time. init without assignments can retain old values.
     * Reference lasts until destruction, buffers/elements can change on init/advance;
     * serialize access and copy before retaining a snapshot. */
    const std::vector<double>& nodeFlows() const { return node_rdii_flow_; }

    /** @brief Copy sorted unique node indices currently stored in group assignment vectors.
     * @return Independent sorted vector, including negative invalid indices if present.
     * Uses the vectors rather than group count, so init with no new assignments can
     * retain an old list. Does not filter/check current context node membership; inputs
     * should already be validated before use as an RDII interface-file header. */
    std::vector<int> rdiiNodeList() const;

    /** @brief Select whole-second UH rainfall processing interval.
     * @param uh Qualified monthly peak/base seconds; values must fit integer conversions.
     * @param wet_step Finite wet-step seconds fitting int; truncated, nonpositive falls
     * back to 300 before comparing positive truncated rise/fall limbs.
     * @return Minimum positive whole-second wet-step/limb duration, at least one second.
     * Scans all months/responses, skipping nonpositive limbs; no model mutation. */
    static int getRainInterval(const UnitHydParams& uh, double wet_step);

    /** @brief Size one response rainfall ring to its longest monthly base duration.
     * @param uh Qualified nonnegative finite base seconds fitting integer conversions.
     * @param response Nonnegative response index, reduced modulo three.
     * @param rainInterval Whole seconds; nonpositive returns zero before response lookup.
     * @return max over months of int(base/rainInterval)+1, with initial lower bound zero.
     * No allocation/parameter validation; caller supplies a compatible ring afterward. */
    static int getMaxPeriods(const UnitHydParams& uh, int response, int rainInterval);

private:
    RDIIGroupSoA groups_;
    UhNameMap uh_name_to_idx_;           ///< UH group name → index (case-insensitive)
    std::vector<double> node_rdii_flow_; ///< Latest tick's per-node RDII flow (CFS)

    // Strict grid (legacy createRdiiFile records, kept in memory).
    double grid_step_ = 0.0;        ///< RdiiStep (sec) = wet_step
    double next_tick_ = 0.0;        ///< Next un-emitted tick (elapsed sec)
    std::vector<int> grid_node_;    ///< Column → node index (sorted unique)
    std::vector<float> grid_flows_; ///< Emitted rows × grid_node_.size(), REAL4

    /** @brief Consume queued rain chunks and append one generated strict-grid row.
     * @param[in,out] ctx Compatible model/units/start date/current climate and ft3 RDII
     * rainfall/produced totals; queues/rings and latest/grid flow buffers also mutate.
     * @param T Finite tick elapsed seconds matching the next_tick_ schedule.
     * Each group's chunks use its first pending rate for this tick (missing is zero),
     * stamp IA with chunk month and ring with tick month, then convolve triangular
     * ordinates at mid-chunk lags. Converts project rainfall/land area to cfs, sums per
     * node, zeroes q<0.0001 cfs and stores float32. Produced ft3 uses thresholded unrounded
     * q*grid_step, including final tick. Requires prevalidated in-range node assignments,
     * positive steps and aligned arrays. Does not update next_tick_ itself or route flows;
     * partial queues/ledgers/grid can remain after failure. */
    void emitTick(SimulationContext& ctx, double T);

    /** @brief Evaluate a triangular unit-hydrograph ordinate at one lag.
     * @param uh Qualified monthly peak/base seconds.
     * @param month Nonnegative month reduced modulo 12.
     * @param response Nonnegative response reduced modulo three.
     * @param t Finite lag seconds; negative yields zero through lower clamping.
     * @return Ordinate 1/hour with peak 2/base*3600, zero for nonpositive peak/base or
     * lag>=base. R rainfall fraction is applied by caller. No input validation/mutation. */
    double uhOrdinate(const UnitHydParams& uh, int month, int response, double t) const;

    /** @brief Append diagnostics for configured decay/snow rows without changing coefficients.
     * @param[in,out] ctx Parsed decay rows, temperature-source flag and warnings store.
     * Warns for active decay without temperature source, known-row zero depletion,
     * freeze>=reference threshold and enabled snow with fixed reference temperature.
     * Unknown groups/invalid responses are skipped. Does not reject rows, synthesize
     * weather or validate every bound; allocation failure can leave warning prefix. */
    void validateExpDecay(SimulationContext& ctx) const;
};

} // namespace rdii
} // namespace openswmm

#endif // OPENSWMM_RDII_HPP
