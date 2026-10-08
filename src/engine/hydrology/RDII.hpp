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
 * @file RDII.hpp
 * @brief RDII — rainfall-dependent infiltration/inflow via unit hydrograph.
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
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
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
    double r[12][3]     = {};  ///< Rainfall fraction per month × response
    double tPeak[12][3] = {};  ///< Time to peak (sec) per month × response
    double tBase[12][3] = {};  ///< Base time (sec) per month × response
    double iaMax[12][3] = {};  ///< Initial abstraction max depth
    double iaRecov[12][3] = {};///< IA recovery rate (linear model)
    double iaInit[12][3] = {}; ///< Initial IA used
};

/// Exponential IA decay parameters for one RDII response (SHORT/MEDIUM/LONG).
/// When `active` is true, this response uses the temperature-dependent
/// exponential IA model in place of the legacy linear iaRecov logic.
/// Coefficients use project depth units for k_dep (consistent with iaMax),
/// hours for k_0/k_T, and degrees Celsius for temperature thresholds.
/// @see docs/RDII_ExpDecay_Implementation.md
struct ExpDecayParams {
    bool   active    = false;
    double k_dep     = 0.0;   ///< Depletion rate (1/project-depth) — temperature-independent
    double k_0       = 0.0;   ///< Base recovery rate (1/hr)
    double k_T       = 0.0;   ///< Thermal recovery rate at T_ref (1/hr)
    double T_ref     = 10.0;  ///< Reference temperature (deg C)
    double theta_rec = 0.0;   ///< Temperature sensitivity (1/deg C)
    double T_freeze  = 0.0;   ///< Recovery suppressed below this temperature (deg C)

    // Optional degree-day snow model. When `snow_on`, precipitation at
    // T <= snow_T accumulates as SWE with no liquid input; at T > snow_T,
    // melt = min(SWE, snow_ddf*(T - snow_T)*dt_days) is added to rainfall
    // (rain-on-snow) before the IA depletion step.
    bool   snow_on   = false; ///< Degree-day snow model enabled
    double snow_T    = 1.0;   ///< Rain/snow threshold & melt base (deg C)
    double snow_ddf  = 0.0;   ///< Degree-day melt factor (project-depth/degC/day)
};

/// Per-response (SHORT/MEDIUM/LONG) unit hydrograph data.
/// Legacy equivalent: TUHData uh[3] inside TUHGroup.
struct UHResponseData {
    std::vector<double> past_rain;      ///< circular buffer of past rainfall depths
    std::vector<int>    past_month;     ///< month for each past rainfall entry
    int    period      = 0;             ///< current buffer write position
    int    max_periods = 0;             ///< buffer capacity
    int    has_past_rain = 0;           ///< true if any non-zero past rain
    double ia_used     = 0.0;           ///< initial abstraction used so far
    double swe         = 0.0;           ///< snow water equivalent (project depth; degree-day snow model)
    long   dry_seconds = 0;             ///< seconds since last non-zero rainfall

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

/// Additive recovery rate k_rec(T) = k_0 + k_T * exp(theta_rec * (T - T_ref)),
/// suppressed (returns 0) strictly below T_freeze. T in deg Celsius.
/// Exposed for unit testing against the reference IAModel implementation.
double getRecoveryRate(const ExpDecayParams& dp, double T_celsius);

/// One exponential-IA update step: mass-consistent depletion when
/// rainDepth > 0, temperature-dependent recovery otherwise. When the row's
/// degree-day snow model is on, the precipitation input is partitioned
/// through the SWE store (accumulate below snow_T; melt above it) before
/// the depletion/recovery branch runs.
/// Returns the excess rainfall depth (project rain-depth units).
/// Exposed for unit testing against the reference IAModel implementation.
double updateIA_exp(const UnitHydParams& uh, UHResponseData& rd,
                    const ExpDecayParams& dp, int month, int response,
                    double rainDepth, double dt_sec,
                    const SimulationContext& ctx);

struct RDIIGroupSoA {
    int count = 0;
    std::vector<int>    node_idx;       ///< Target node index
    std::vector<int>    uh_idx;         ///< Unit hydrograph parameter index
    std::vector<int>    gage_idx;       ///< Rain gage index per UH group (legacy: UnitHyd[j].rainGage)
    std::vector<double> area;           ///< Contributing area (acres, project units)

    /// Per-response data: [group * 3 + response]
    std::vector<UHResponseData> uh_data;

    std::vector<int>    rain_interval;  ///< Rain processing interval (sec) per group

    // Strict-grid driver state (legacy createRdiiFile embedded at runtime).
    // Chunk starts advance in rain_interval steps from simulation start;
    // rates are recorded per chunk while the runoff substep containing the
    // chunk's start is current (the substep never crosses a gage entry
    // boundary, so the current gage rate IS the rate at the chunk start).
    std::vector<double> gage_elapsed;         ///< Chunk cursor (sec, legacy UHGroup.gageDate)
    std::vector<double> rate_recorded_until;  ///< Chunk starts with recorded rates (sec)
    std::vector<std::vector<double>> pending_rates;  ///< FIFO of recorded chunk rates

    void resize(int n);
};

class RDIISolver {
public:
    void init(SimulationContext& ctx);

    /**
     * @brief Register a unit hydrograph parameter set by name.
     *
     * @param name  Unit hydrograph name (e.g. from [HYDROGRAPHS] section).
     * @param params  Complete UH parameters for all 12 months x 3 responses.
     * @returns Index of the registered UH parameter set.
     */
    int addUnitHydParams(const std::string& name, const UnitHydParams& params);

    /**
     * @brief Look up unit hydrograph index by name.
     * @returns Index, or -1 if not found.
     */
    int findUnitHyd(const std::string& name) const;

    /// UH name → index map type; case-insensitive (legacy hash.c parity).
    using UhNameMap = std::unordered_map<std::string, int, CiHash, CiEqual>;

    /// Read-only access to the UH name → index map (for validation).
    const UhNameMap& uhNameIndex() const { return uh_name_to_idx_; }

    /**
     * @brief Advance the strict-grid RDII computation to a runoff clock time.
     *
     * @details Embeds legacy createRdiiFile()'s driver at runtime: RDII is
     *          evaluated on the fixed RdiiStep (= wet_step) grid anchored at
     *          simulation start, with each group's rainfall processed in its
     *          own rain_interval chunks — NOT on the runoff substep cadence.
     *          (The previous per-substep accumulator crossed at most one rain
     *          interval per runoff substep, so a DRY_STEP jump swallowed the
     *          intermediate intervals: dry-period tracking, IA recovery and
     *          the convolution grid all corrupted, showing as an RDII onset
     *          lead/lag of about one wet step.)
     *
     *          Call once per runoff substep after gage rates (and monthly
     *          adjustment) are updated, with the substep's END elapsed time.
     *          The substep never crosses a gage entry boundary (the runoff
     *          timestep is capped to the next rain date), so the current gage
     *          rate is exact for every chunk starting inside the substep.
     *          Per-tick node flows are stored float32 with the legacy
     *          ZERO_RDII threshold, one grid row per tick, zero-order held
     *          over [tick, tick + wet_step) at lookup, matching the RDII
     *          interface file's record windows.
     */
    void advance(SimulationContext& ctx, double new_elapsed_sec);

    /**
     * @brief Apply RDII inflows at a routing time to node lateral flows.
     *
     * @details Looks up the strict-grid row whose window contains
     *          elapsed_sec (the START of the routing step, matching legacy
     *          addRdiiInflows(currentDate) with currentDate taken at
     *          routing_execute entry) and adds the stored float32 flows to
     *          nodes.rdii_inflow.
     */
    void applyRdiiInflows(SimulationContext& ctx, double elapsed_sec) const;

    std::vector<UnitHydParams> uh_params;

    /// Exponential decay params per UH group × response.
    /// Same indexing as uh_params; each entry holds [SHORT, MEDIUM, LONG].
    /// `active == false` means the response uses the legacy linear IA model.
    std::vector<std::array<ExpDecayParams, 3>> decay_params;

    /// Buffered per-node RDII flow (CFS), sized n_nodes. Read by the RDII
    /// interface file SAVE path (RdiiInterfaceFile::saveFlows).
    const std::vector<double>& nodeFlows() const { return node_rdii_flow_; }

    /// Sorted unique node indices that have an RDII unit-hydrograph inflow
    /// (legacy RdiiNodeIndex; used as the SAVE-file header node list).
    std::vector<int> rdiiNodeList() const;

    /// Compute rain processing interval for a UH (minimum limb duration, capped by wet_step).
    static int getRainInterval(const UnitHydParams& uh, double wet_step);

    /// Compute max past periods for a UH response given a rain interval.
    static int getMaxPeriods(const UnitHydParams& uh, int response, int rainInterval);

private:
    RDIIGroupSoA groups_;
    UhNameMap uh_name_to_idx_;  ///< UH group name → index (case-insensitive)
    std::vector<double> node_rdii_flow_;  ///< Latest tick's per-node RDII flow (CFS)

    // Strict grid (legacy createRdiiFile records, kept in memory).
    double grid_step_ = 0.0;        ///< RdiiStep (sec) = wet_step
    double next_tick_ = 0.0;        ///< Next un-emitted tick (elapsed sec)
    std::vector<int>   grid_node_;  ///< Column → node index (sorted unique)
    std::vector<float> grid_flows_; ///< Emitted rows × grid_node_.size(), REAL4

    /// Emit one strict-grid tick at elapsed time T: catch each group's chunk
    /// processing up to T, convolve, threshold and store the node-flow row.
    void emitTick(SimulationContext& ctx, double T);

    /// Compute UH ordinate at time t for response k, month m.
    double uhOrdinate(const UnitHydParams& uh, int month, int response, double t) const;

    /// Push a warning if any decay row is active but no temperature source is configured.
    void validateExpDecay(SimulationContext& ctx) const;
};

} // namespace rdii
} // namespace openswmm

#endif // OPENSWMM_RDII_HPP
