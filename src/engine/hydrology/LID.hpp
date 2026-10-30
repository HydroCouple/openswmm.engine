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
 * @file      LID.hpp
 * @brief     Low Impact Development (LID) control modules.
 *
 * @details Supports 8 LID types with a layered vertical model:
 *   surface → soil → storage → drain
 *
 *   **Vectorization strategy**: Group all LID units of the same type
 *   across all subcatchments into contiguous arrays (similar to XSectGroups).
 *   Each LID type uses the same flux-rate formulas — batch-compute over
 *   all units of that type simultaneously.
 *
 *   **LID type groups (SoA per type):**
 *   - BIO_CELL group: N units → batch biocellFluxRates()
 *   - PERM_PAVEMENT group: M units → batch pavementFluxRates()
 *   - etc.
 *
 * @note Legacy reference: src/legacy/engine/lid.c, lidproc.c
 * @ingroup new_engine
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_LID_HPP
#define OPENSWMM_LID_HPP

#include "Infiltration.hpp"
#include <utility>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <string>
#include <fstream>
#include <map>
#include <memory>

namespace openswmm::detail {
class CompletedSourceArchive;
}

namespace openswmm {

struct SimulationContext;

namespace lid {

// ============================================================================
// LID type codes
// ============================================================================

enum class LIDType : int {
    BIO_CELL = 0,
    RAIN_GARDEN = 1,
    GREEN_ROOF = 2,
    INFIL_TRENCH = 3,
    PERM_PAVEMENT = 4,
    RAIN_BARREL = 5,
    VEG_SWALE = 6,
    ROOF_DISCON = 7
};

/**
 * @brief Classify whether an enumerated LID has an infiltrating native-soil bottom.
 *
 * @param[in] type LID type; swales always qualify, and BC/RG/IT/PP qualify under the storage test.
 * @param[in] storage_thickness Storage-layer thickness; only its sign matters, in any consistent
 *     length unit.
 * @param[in] storage_ksat Storage exfiltration conductivity; only its sign matters.
 * @return True for a swale or an eligible type with absent storage (thickness <= 0) or positive
 *     storage conductivity; false for other types.
 */
inline bool hasNativeBottom(LIDType type, double storage_thickness, double storage_ksat) {
    if (type == LIDType::VEG_SWALE)
        return true;
    if (type != LIDType::BIO_CELL && type != LIDType::RAIN_GARDEN &&
        type != LIDType::INFIL_TRENCH && type != LIDType::PERM_PAVEMENT)
        return false;
    return storage_thickness <= 0 || storage_ksat > 0;
}
/**
 * @brief Classify native-soil contact from an exact authored type code.
 *
 * @param[in] type Case-sensitive code: VS, BC, RG, IT or PP can qualify; unknown codes return
 *     false.
 * @param[in] thickness Storage thickness; any consistent length unit, tested against zero.
 * @param[in] ksat Storage exfiltration conductivity, tested against zero.
 * @return True for VS, or for BC/RG/IT/PP when thickness <= 0 or ksat > 0.
 */
inline bool hasNativeBottom(const std::string& type, double thickness, double ksat) {
    if (type == "VS")
        return true;
    return (type == "BC" || type == "RG" || type == "IT" || type == "PP") &&
           (thickness <= 0 || ksat > 0);
}

// ============================================================================
// Layer state (per LID unit)
// ============================================================================

constexpr int SURF = 0;
constexpr int SOIL = 1;
constexpr int STOR = 2;
constexpr int PAVE = 3;
constexpr int N_LAYERS = 4;

// ============================================================================
// LID unit parameters (SoA for batch processing)
// ============================================================================

struct LIDGroupSoA {
    LIDType type = LIDType::BIO_CELL;
    int count = 0;

    // Unit-conversion factors for the model's flow units (legacy UCF()).
    // Layer parameters are converted to internal ft / ft-per-sec at init();
    // these two factors also drive the underdrain head/rate conversion in
    // getDrainRate(). Defaults are the US-customary values (issue #102).
    double ucf_raindepth = 12.0;   ///< ft → in|mm  (RAINDEPTH)
    double ucf_rainfall = 43200.0; ///< ft/sec → in/hr|mm/hr  (RAINFALL)

    std::vector<int> subcatch_idx;   ///< Which subcatchment this unit belongs to
    std::vector<int> control_idx;    ///< LID control index (into ctx.lid_controls)
    std::vector<double> area;        ///< Unit area (ft2)
    std::vector<double> from_imperv; ///< Fraction of impervious runoff treated (0-1)
    std::vector<double> from_perv;   ///< Fraction of pervious runoff treated (0-1)
    std::vector<int> to_perv;        ///< Route surface outflow to pervious area (1=yes)
    std::vector<int> drain_node;     ///< Resolved drain-to node index (-1=none)
    std::vector<int> drain_subcatch; ///< Resolved drain-to subcatch index (-1=none)
    std::vector<double> inflow;      ///< Per-unit inflow rate (ft/sec) — set before execute()
    std::vector<double>
        evap_rate_unit; ///< Per-unit effective PET rate (ft/sec) — filled by execute()
    std::vector<double>
        subcatch_rain; ///< Parent subcatchment rainfall (ft/sec) — for the rain-barrel dry-time reset (legacy lid.c:1920)
    std::vector<std::string>
        rpt_path; ///< Per-unit LID report file ([LID_USAGE] col 9); empty = none
    // Per-step kernel rates staged for the LID report file (ft/s). Legacy
    // lidproc_saveResults reads these straight from lidproc.c's static flux
    // variables; the kernel here is per-call, so runUnitLegacy stages them.
    std::vector<double> rpt_surf_infil; ///< SurfaceInfil this step
    std::vector<double> rpt_pave_perc;  ///< PavePerc this step
    std::vector<double> rpt_soil_perc;  ///< SoilPerc this step

    // Surface layer
    std::vector<double> surf_store; ///< Surface storage depth (ft)
    std::vector<double> surf_rough; ///< Surface Manning's n
    std::vector<double> surf_slope; ///< Surface slope

    // Soil layer
    std::vector<double> soil_thick;   ///< Soil thickness (ft)
    std::vector<double> soil_poros;   ///< Soil porosity
    std::vector<double> soil_fc;      ///< Soil field capacity
    std::vector<double> soil_wp;      ///< Soil wilting point
    std::vector<double> soil_ksat;    ///< Soil saturated K (ft/sec)
    std::vector<double> soil_kslope;  ///< Conductivity slope
    std::vector<double> soil_suction; ///< Suction head for Green-Ampt (ft)

    // Storage layer
    std::vector<double> stor_thick; ///< Storage thickness (ft)
    std::vector<double> stor_void;  ///< Storage void fraction
    std::vector<double> stor_ksat;  ///< Storage exfiltration K (ft/sec)
    std::vector<double> stor_clog;  ///< Storage clogging factor (ft)
    std::vector<int> stor_covered;  ///< 1 if rain barrel is covered (blocks rainfall)

    // Drain
    std::vector<double> drain_coeff;  ///< Drain coefficient
    std::vector<double> drain_expon;  ///< Drain exponent
    std::vector<double> drain_offset; ///< Drain offset depth (ft)
    std::vector<double> drain_delay;  ///< Drain delay time (sec, rain barrel)
    std::vector<double> drain_hopen;  ///< Head to open drain valve (ft)
    std::vector<double> drain_hclose; ///< Head to close drain valve (ft)
    std::vector<int> drain_open;      ///< Current drain valve state (1=open, 0=closed)

    // Pavement layer (PERM_PAVEMENT)
    std::vector<double> pave_thick;       ///< Pavement thickness (ft)
    std::vector<double> pave_void;        ///< Pavement void fraction
    std::vector<double> pave_imperv_frac; ///< Impervious fraction of pavement
    std::vector<double> pave_ksat;        ///< Pavement saturated K (ft/sec)
    std::vector<double> pave_clog_factor; ///< Pavement clog factor (ft of treated volume)
    std::vector<double> pave_regen_days;  ///< Pavement regeneration interval (days)
    std::vector<double> pave_regen_deg;   ///< Pavement regeneration degree (0-1)
    std::vector<double> next_regen_day;   ///< Next day for pavement regeneration (OADate)

    // Drainage mat layer (GREEN_ROOF)
    std::vector<double> drainmat_thick; ///< Drainage mat thickness (ft)
    std::vector<double> drainmat_void;  ///< Drainage mat void fraction
    std::vector<double> drainmat_rough; ///< Drainage mat Manning's roughness

    // Surface geometry (for Manning's outflow)
    std::vector<double> surf_void_frac;  ///< Surface void fraction (default 1.0)
    std::vector<double> surf_alpha;      ///< Surface Manning alpha = sqrt(slope)/n
    std::vector<double> surf_side_slope; ///< Swale side slope (run/rise)
    std::vector<double> full_width;      ///< Full width for Manning's flow (ft)
    std::vector<double> unit_area;       ///< ONE unit's area (ft2) — legacy lidUnit->area
    std::vector<double> unit_width;      ///< ONE unit's width (ft) — legacy lidUnit->fullWidth
    std::vector<double> dry_time;        ///< Seconds since last rainfall

    // State variables (updated each step)
    std::vector<double> surf_depth; ///< Current surface ponded depth
    std::vector<double> soil_moist; ///< Current soil moisture (0-porosity)
    std::vector<double> stor_depth; ///< Current storage depth
    std::vector<double> pave_depth; ///< Current pavement depth

    // Legacy lidproc.c per-unit state the kernel needs beyond the layer
    // depths (lid.c TLidUnit): the unit's own Green-Ampt infiltration
    // state for the soil layer (soilInfil), the drain flow the previous
    // step delivered (oldDrainFlow, the underdrain's hOpen/hClose
    // hysteresis memory), and the process's canOverflow flag
    // (validateLidProc: false for a roof disconnection and for a
    // bio-cell / rain garden / trench / pavement / green roof whose surface
    // has a Manning alpha).
    std::vector<GreenAmptState> soil_infil;
    std::vector<double> old_drain_flow;
    std::vector<uint8_t>
        is_wet; ///< legacy lidproc_saveResults isDry == FALSE this step (HasWetLids)
    std::vector<uint8_t> can_overflow;
    std::vector<double>
        drainmat_alpha; ///< legacy drainMat.alpha = PHI / roughness * sqrt(surfSlope)

    // Outputs (per unit)
    std::vector<double> surface_runoff;
    std::vector<double> drain_flow;
    std::vector<double> evap_loss;  ///< Evaporation loss this step (ft of depth)
    std::vector<double> infil_loss; ///< Native infiltration loss this step (ft)

    // Pollutant drain removal fractions: drain_rmvl[unit * n_pollutants + pollutant]
    std::vector<double> drain_rmvl; ///< Removal fraction per unit per pollutant
    int n_pollutants = 0;           ///< Number of pollutants (for indexing drain_rmvl)

    // ---- A4: per-layer INFLOW rates (ft/sec of water per unit area) ----
    // Each layer receives from exactly one place: the layer above it in the
    // present stack, or externally for the topmost present layer. Every
    // batch*Flux routine already computes these as locals (soil_infil,
    // soil_perc, pavePerc, storageInflow, ...); they are published here
    // because a complete-mix age needs the INFLOW and nothing else — the
    // outflow leaves at the layer's own age.
    //
    // These are NOT the `f_old_*` below. Those are the Modified Puls
    // time-weighting term: `f_old_surf` is the NET dx/dt of the surface
    // layer, and the other three are never written at all. A net rate of
    // change is exactly the quantity that made phase A3 report elapsed time
    // instead of age.
    std::vector<double> in_surf; ///< External inflow onto the surface
    std::vector<double> in_pave; ///< Surface → pavement
    std::vector<double> in_soil; ///< Layer above → soil
    std::vector<double> in_stor; ///< Layer above → storage
    /// Completed-only post-clamp evaporation rates (ft/s); pollutant
    /// transport removes water but retains solute in the donor layer.
    std::vector<double> evap_surf, evap_pave, evap_soil, evap_stor;

    // Previous flux rates (for Modified Puls time weighting)
    std::vector<double> f_old_surf; ///< Previous surface flux rate
    std::vector<double> f_old_soil; ///< Previous soil flux rate
    std::vector<double> f_old_stor; ///< Previous storage flux rate
    std::vector<double> f_old_pave; ///< Previous pavement flux rate

    // Water balance tracking (cumulative per unit)
    std::vector<double> wb_inflow; ///< Total inflow volume (ft)
    std::vector<double> wb_evap;   ///< Total evaporation volume (ft)
    /// Completed spatial bottom water, ft over the full unit footprint.
    std::vector<double> spatial_infil_loss, wb_spatial_infil;
    std::vector<double> wb_infil;      ///< Total exfiltration volume (ft)
    std::vector<double> wb_surf_flow;  ///< Total surface outflow volume (ft)
    std::vector<double> wb_drain_flow; ///< Total drain outflow volume (ft)
    std::vector<double> wb_init_vol;   ///< Initial stored volume (ft)
    std::vector<double> wb_final_vol;  ///< Final stored volume (ft)
    std::vector<double> vol_treated;   ///< Cumulative volume treated (ft, for clog model)

    /**
     * @brief Reset a type group to default per-unit arrays.
     *
     * Resizes and overwrites all per-unit parameter/state/report arrays and clears drain_rmvl. The
     * scalar type, conversion factors and n_pollutants remain unchanged; callers must size pollutant
     * removals separately. Allocation failures propagate.
     *
     * @param[in] n Nonnegative number of usage rows; a negative value is not checked before conversion
     *     to size_t.
     */
    void resize(int n);
};

/**
 * @brief Measure one completed-source unit using its physical layer geometry.
 *
 * For a swale, a positive valid surface height is required by the minimum-bottom-width correction.
 * This differs from the planar arithmetic used by storedVolume().
 *
 * @param[in] g Initialized group with all arrays covering unit; internal ft and ft2 units.
 * @param[in] unit Unit index in [0, g.count); bounds are unchecked.
 * @return Stored water volume in ft3. Swales use the trapezoidal cross section and aggregate
 *     footprint; other types sum void-weighted layer stores.
 */
double completedUnitStorage(const LIDGroupSoA& g, int unit);

// ============================================================================
// LID solver
// ============================================================================

class LIDSolver {
public:
    /**
     * @brief Build eight type groups and initialize water state from resolved LID controls and usage.
     *
     * Converts authored lengths, areas and rates to internal ft/ft2/ft/s, resolves drain destinations,
     * seeds moisture, Green-Ampt history and water ledgers, and orders units by subcatchment then
     * reverse usage parse order. Invalid control indices or unknown type codes are skipped. Intended
     * for a fresh solver: existing report streams and ancillary forcing buffers are not cleared; the
     * zero-usage early return also leaves usage_order_ unchanged. No report file is opened here;
     * allocation failures propagate.
     *
     * @param[in,out] ctx Validated model with parallel control/usage arrays; usage water ledgers and
     *     subcatchment total_lid_area_ft2 are updated.
     */
    void init(SimulationContext& ctx);

    struct CompletedUnitInput {
        int type = -1, unit = -1;
        double inflow = 0.0, rain = 0.0, pet = 0.0;
        double native_infil = 0.0, max_native_infil = 1.0e10, infil_factor = 1.0;
        // Negative fraction retains ordinary completed-kernel arithmetic.
        double spatial_fraction = -1.0, spatial_max_infil = 1.0e10;
    };
    /**
     * @brief Copy water parameters, runtime state and ledgers for an independent trial.
     *
     * The copy performs no forcing resolution or file IO. Advancing the trial does not mutate this
     * solver; allocation failures propagate.
     * @return A solver with independent group/order/forcing storage and clocks, and no report streams.
     */
    LIDSolver waterTrial() const;
    /**
     * @brief Advance selected units with already-resolved completed-source forcing.
     *
     * Validates the entire input batch before evaluation, throwing std::invalid_argument on a bad
     * interval, key or value. Zero/nonpositive-area units are skipped. Updates selected water states,
     * per-layer rates, ledgers, resolved inflow/PET/rain and dry clocks using completed-volume rules;
     * it does not resolve forcing, write report files or scatter runoff to the context. Existing
     * old_drain_flow/drain_open history is not refreshed by this entry point. Evaluation is in-place
     * after validation; use waterTrial() for transactional trial ownership.
     *
     * @param[in] dt Finite positive interval length in seconds.
     * @param[in] start Finite nonnegative interval start in elapsed runoff seconds, used for pavement
     *     regeneration.
     * @param[in] recovery_factor Finite nonnegative Green-Ampt recovery multiplier.
     * @param[in] inputs Strictly increasing unique (type, unit) keys within initialized group bounds.
     *     All rate/ceiling/factor fields must be finite and nonnegative; spatial_fraction must be -1 or
     *     in [0,1]. Rates are ft/s; fractions and factors are dimensionless.
     */
    void executeCompleted(double dt, double start, double recovery_factor,
                          const std::vector<CompletedUnitInput>& inputs);

    /**
     * @brief Borrow mutable parameters and water state for a type group.
     *
     * @param[in] type_index Unchecked index in [0, numGroups()); after init(), the LIDType value 0..7.
     * @return Reference owned by this solver, invalidated by group reallocation or solver destruction.
     *     Caller edits must preserve parallel-array sizes and physical invariants.
     */
    LIDGroupSoA& group(int type_index) { return groups_[static_cast<size_t>(type_index)]; }
    /**
     * @brief Borrow read-only parameters and water state for a type group.
     *
     * @param[in] type_index Unchecked index in [0, numGroups()).
     * @return Const reference owned by this solver, invalidated by reinitialization, group reallocation
     *     or destruction.
     */
    const LIDGroupSoA& group(int type_index) const {
        return groups_[static_cast<size_t>(type_index)];
    }
    /**
     * @brief Return the number of currently allocated type groups.
     * @return Zero for a default solver and eight after init().
     */
    int numGroups() const { return static_cast<int>(groups_.size()); }

    /**
     * @brief Borrow the deterministic legacy accumulation order for usage rows.
     *
     * Shared per-subcatchment sums use this order to preserve floating-point addition order.
     * Invalidated by reinitialization or destruction; the zero-usage init early return does not rebuild
     * it.
     * @return Solver-owned (group index, unit index) pairs, ordered by ascending subcatchment index and
     *     reverse parse order within each subcatchment after init().
     */
    const std::vector<std::pair<int, int>>& usageOrder() const { return usage_order_; }

    /**
     * @brief Sum current planar, void-weighted layer storage for ordinary runoff continuity.
     *
     * @param[in] source_owners Optional borrowed subcatchment ownership map; nonnegative entries
     *     exclude completed-source units. Must cover every nonnegative subcatch_idx accessed; short
     *     maps throw std::out_of_range.
     * @return Current included water in ft3, computed from layer states and aggregate footprints;
     *     swales use planar surface depth here. No ownership exclusion when null.
     */
    double storedVolume(const std::vector<int>* source_owners = nullptr) const;
    /**
     * @brief Visit LID model identity or accepted-boundary water state in archive order.
     *
     * The group sequence and Green-Ampt state use fixed shapes. Report streams are never persisted.
     * Call only at an accepted completed-source boundary with exclusive access, and validate restored
     * state with validateCheckpoint(); this visitor itself does not validate or roll back reads.
     *
     * @param[in,out] a Archive that controls read, write or digest behavior; archive errors propagate
     *     and reads mutate this solver.
     * @param[in] model True visits authored/resolved material, unit identities and order; false visits
     *     runtime water, Green-Ampt state, clocks and forcing buffers.
     * @param[in] lazy_buffers For state archives only, allow the three runoff forcing buffers to use
     *     dynamic sizes for fresh session restore; false enforces existing fixed buffer shapes.
     */
    void checkpoint(detail::CompletedSourceArchive& a, bool model,
                    bool lazy_buffers = false); // internal accepted-boundary IO
    /**
     * @brief Check restored LID water state against an initialized model.
     *
     * Throws std::runtime_error on the first invalid state/material: mismatched counts, open report
     * streams, invalid clocks, buffer values/shapes, nonfinite runtime fields, invalid
     * moisture/depth/flags/ledgers, changed initial volumes or changed Green-Ampt material. Small
     * negative depth/ledger roundoff is tolerated to 1e-12 and soil moisture to porosity + 1e-8.
     * Fixed-array shape compatibility must already have been enforced by archive loading; this method
     * uses unchecked indexing within each unit.
     *
     * @param[in] model Compatible initialized reference with matching group counts and
     *     already-compatible array shapes; not modified.
     * @param[in] runoff_count Required size of any nonempty per-subcatchment forcing buffer.
     */
    void validateCheckpoint(const LIDSolver& model, std::size_t runoff_count) const;

    /**
     * @brief Sum the latest kernel water-balance final-store ledger over all unit footprints.
     * @return Total in ft3, with no source-owner exclusion. Uses wb_final_vol rather than recomputing
     *     current layer storage; kernel evaluation conventions can differ from storedVolume().
     */
    double totalStoredVolume() const;
    /**
     * @brief Sum the initialized water-balance store ledger over all unit footprints.
     * @return Total in ft3, with no source-owner exclusion. Each per-area ledger depth is multiplied by
     *     the aggregate usage footprint.
     */
    double totalInitVolume() const;
    /**
     * @brief Sum cumulative native plus spatial bottom exfiltration over all unit footprints.
     * @return Total in ft3, with no source-owner exclusion. Each per-area ledger depth is multiplied by
     *     the aggregate usage footprint.
     */
    double totalInfilVolume() const;
    /**
     * @brief Sum cumulative evaporation over all unit footprints.
     * @return Total in ft3, with no source-owner exclusion. Each per-area ledger depth is multiplied by
     *     the aggregate usage footprint.
     */
    double totalEvapVolume() const;

    /**
     * @brief Advance ordinary LID units with staged inflow and context-resolved evaporation.
     *
     * A mismatched ownership-map size throws std::invalid_argument before evaluation. Skips nonpositive
     * footprints; uses previously staged native infiltration/factors and runoff start time. Updates
     * water states, fluxes, ledgers, wet flags and dry clocks, and lazily writes configured report
     * files. No rollback is provided after evaluation starts. The per-unit old_drain_flow/drain_open
     * history is left as stored; standalone batch entry points explicitly refresh it.
     *
     * @param[in,out] ctx Initialized model, names, forcing and simulation dates used to resolve PET and
     *     format optional reports; this routine does not scatter unit runoff/drains to context totals.
     * @param[in] dt Positive runoff interval in seconds; unchecked before divisions in the kernel.
     * @param[in] rainfall Compatibility argument, ignored; each group inflow must already contain rain
     *     plus captured runoff.
     * @param[in] evap_rate Climate potential evaporation fallback in ft/s; DRY_ONLY suppresses it
     *     during rain, and prescribed parent-subcatchment PET can override it.
     * @param[in] source_owners Optional ownership map exactly ctx.n_subcatches() long; nonnegative
     *     entries exclude units owned by completed sources. Null evaluates all eligible units.
     */
    void execute(SimulationContext& ctx, double dt, double rainfall, double evap_rate,
                 const std::vector<int>* source_owners = nullptr);

    /**
     * @brief Set the old runoff clock used by regeneration and report timestamps.
     *
     * @param[in] old_runoff_sec Elapsed seconds at the beginning of the next execute() interval;
     *     expected finite and nonnegative, unchecked.
     */
    void setRunoffTime(double old_runoff_sec) { old_runoff_sec_ = old_runoff_sec; }

    /**
     * @brief Take ownership of per-subcatchment native-soil rates for subsequent ordinary steps.
     *
     * Arrays may be empty or shorter than the subcatchment count; no size or value validation is
     * performed here. Completed execution instead uses each explicit CompletedUnitInput.
     *
     * @param[in] native Native infiltration capacities in ft/s; moved into the solver. Missing indices
     *     use zero.
     * @param[in] max_native Bottom exfiltration ceilings in ft/s; moved into the solver. Missing
     *     indices use 1e10 ft/s.
     */
    void setNativeInfil(std::vector<double> native, std::vector<double> max_native) {
        native_infil_ = std::move(native);
        max_native_infil_ = std::move(max_native);
    }

    /**
     * @brief Take ownership of infiltration multipliers and set the evaporation recovery factor.
     *
     * @param[in] infil_factor Per-subcatchment dimensionless Green-Ampt conductivity factors; moved
     *     into the solver. Missing indices default to 1.
     * @param[in] recovery_factor Dimensionless Green-Ampt recovery multiplier used by subsequent
     *     ordinary steps; expected finite and nonnegative, unchecked.
     */
    void setInfilFactors(std::vector<double> infil_factor, double recovery_factor) {
        infil_factor_ = std::move(infil_factor);
        recovery_factor_ = recovery_factor;
    }

    /**
     * @brief Test the last evaluated wet flags of included units.
     *
     * @param[in] source_owners Optional subcatchment map excluding nonnegative completed-source owners;
     *     must cover each nonnegative subcatch_idx examined, or at() throws std::out_of_range.
     * @return True if an included unit has a nonzero is_wet flag; false if none do. Flags reflect the
     *     most recent evaluation of each unit, including skipped units retaining older values.
     */
    bool anyWet(const std::vector<int>* source_owners = nullptr) const;

    /**
     * @brief Advance a standalone biocell/rain-garden group through the shared legacy kernel.
     *
     * Updates depths, rates, ledgers, wet/dry clocks and drain history without a context or report IO.
     * Native infiltration is zero, its ceiling 1e10 ft/s, factors 1 and old runoff time zero. All
     * wrappers use the same dispatcher; swales iterate with omega=0.5, other ordinary types use one
     * explicit Modified Puls evaluation. The PET array is borrowed and never retained.
     *
     * @param[in,out] g Initialized parallel arrays for the intended biocell/rain-garden type; dispatch
     *     actually follows g.type, not the wrapper name. Each unit needs positive geometry required by
     *     that type.
     * @param[in] rainfall Fallback inflow in ft/s when a unit staged inflow is not positive; covered
     *     rain barrels force inflow to zero.
     * @param[in] evap_rate Optional readable g.count-element PET array in ft/s, used only during this
     *     call; null means zero.
     * @param[in] dt Positive interval in seconds; unchecked.
     */
    static void batchBioCellFlux(LIDGroupSoA& g, double rainfall, const double* evap_rate,
                                 double dt);

    /**
     * @brief Advance a standalone rain-barrel group through the shared legacy kernel.
     *
     * Updates depths, rates, ledgers, wet/dry clocks and drain history without a context or report IO.
     * Native infiltration is zero, its ceiling 1e10 ft/s, factors 1 and old runoff time zero. All
     * wrappers use the same dispatcher; swales iterate with omega=0.5, other ordinary types use one
     * explicit Modified Puls evaluation. Evaporation is zero for this entry point.
     *
     * @param[in,out] g Initialized parallel arrays for the intended rain-barrel type; dispatch actually
     *     follows g.type, not the wrapper name. Each unit needs positive geometry required by that
     *     type.
     * @param[in] rainfall Fallback inflow in ft/s when a unit staged inflow is not positive; covered
     *     rain barrels force inflow to zero.
     * @param[in] dt Positive interval in seconds; unchecked.
     */
    static void batchBarrelFlux(LIDGroupSoA& g, double rainfall, double dt);

    /**
     * @brief Advance a standalone vegetative-swale group through the shared legacy kernel.
     *
     * Updates depths, rates, ledgers, wet/dry clocks and drain history without a context or report IO.
     * Native infiltration is zero, its ceiling 1e10 ft/s, factors 1 and old runoff time zero. All
     * wrappers use the same dispatcher; swales iterate with omega=0.5, other ordinary types use one
     * explicit Modified Puls evaluation. The PET array is borrowed and never retained.
     *
     * @param[in,out] g Initialized parallel arrays for the intended vegetative-swale type; dispatch
     *     actually follows g.type, not the wrapper name. Each unit needs positive geometry required by
     *     that type.
     * @param[in] rainfall Fallback inflow in ft/s when a unit staged inflow is not positive; covered
     *     rain barrels force inflow to zero.
     * @param[in] evap_rate Optional readable g.count-element PET array in ft/s, used only during this
     *     call; null means zero.
     * @param[in] dt Positive interval in seconds; unchecked.
     */
    static void batchSwaleFlux(LIDGroupSoA& g, double rainfall, const double* evap_rate, double dt);

    /**
     * @brief Advance a standalone green-roof group through the shared legacy kernel.
     *
     * Updates depths, rates, ledgers, wet/dry clocks and drain history without a context or report IO.
     * Native infiltration is zero, its ceiling 1e10 ft/s, factors 1 and old runoff time zero. All
     * wrappers use the same dispatcher; swales iterate with omega=0.5, other ordinary types use one
     * explicit Modified Puls evaluation. The PET array is borrowed and never retained.
     *
     * @param[in,out] g Initialized parallel arrays for the intended green-roof type; dispatch actually
     *     follows g.type, not the wrapper name. Each unit needs positive geometry required by that
     *     type.
     * @param[in] rainfall Fallback inflow in ft/s when a unit staged inflow is not positive; covered
     *     rain barrels force inflow to zero.
     * @param[in] evap_rate Optional readable g.count-element PET array in ft/s, used only during this
     *     call; null means zero.
     * @param[in] dt Positive interval in seconds; unchecked.
     */
    static void batchGreenRoofFlux(LIDGroupSoA& g, double rainfall, const double* evap_rate,
                                   double dt);

    /**
     * @brief Advance a standalone infiltration-trench group through the shared legacy kernel.
     *
     * Updates depths, rates, ledgers, wet/dry clocks and drain history without a context or report IO.
     * Native infiltration is zero, its ceiling 1e10 ft/s, factors 1 and old runoff time zero. All
     * wrappers use the same dispatcher; swales iterate with omega=0.5, other ordinary types use one
     * explicit Modified Puls evaluation. The PET array is borrowed and never retained.
     *
     * @param[in,out] g Initialized parallel arrays for the intended infiltration-trench type; dispatch
     *     actually follows g.type, not the wrapper name. Each unit needs positive geometry required by
     *     that type.
     * @param[in] rainfall Fallback inflow in ft/s when a unit staged inflow is not positive; covered
     *     rain barrels force inflow to zero.
     * @param[in] evap_rate Optional readable g.count-element PET array in ft/s, used only during this
     *     call; null means zero.
     * @param[in] dt Positive interval in seconds; unchecked.
     */
    static void batchInfilTrenchFlux(LIDGroupSoA& g, double rainfall, const double* evap_rate,
                                     double dt);

    /**
     * @brief Advance a standalone permeable-pavement group through the shared legacy kernel.
     *
     * Updates depths, rates, ledgers, wet/dry clocks and drain history without a context or report IO.
     * Native infiltration is zero, its ceiling 1e10 ft/s, factors 1 and old runoff time zero. All
     * wrappers use the same dispatcher; swales iterate with omega=0.5, other ordinary types use one
     * explicit Modified Puls evaluation. The PET array is borrowed and never retained.
     *
     * @param[in,out] g Initialized parallel arrays for the intended permeable-pavement type; dispatch
     *     actually follows g.type, not the wrapper name. Each unit needs positive geometry required by
     *     that type.
     * @param[in] rainfall Fallback inflow in ft/s when a unit staged inflow is not positive; covered
     *     rain barrels force inflow to zero.
     * @param[in] evap_rate Optional readable g.count-element PET array in ft/s, used only during this
     *     call; null means zero.
     * @param[in] dt Positive interval in seconds; unchecked.
     */
    static void batchPavementFlux(LIDGroupSoA& g, double rainfall, const double* evap_rate,
                                  double dt);

    /**
     * @brief Advance a standalone roof-disconnection group through the shared legacy kernel.
     *
     * Updates depths, rates, ledgers, wet/dry clocks and drain history without a context or report IO.
     * Native infiltration is zero, its ceiling 1e10 ft/s, factors 1 and old runoff time zero. All
     * wrappers use the same dispatcher; swales iterate with omega=0.5, other ordinary types use one
     * explicit Modified Puls evaluation. The PET array is borrowed and never retained.
     *
     * @param[in,out] g Initialized parallel arrays for the intended roof-disconnection type; dispatch
     *     actually follows g.type, not the wrapper name. Each unit needs positive geometry required by
     *     that type.
     * @param[in] rainfall Fallback inflow in ft/s when a unit staged inflow is not positive; covered
     *     rain barrels force inflow to zero.
     * @param[in] evap_rate Optional readable g.count-element PET array in ft/s, used only during this
     *     call; null means zero.
     * @param[in] dt Positive interval in seconds; unchecked.
     */
    static void batchRoofDisconFlux(LIDGroupSoA& g, double rainfall, const double* evap_rate,
                                    double dt);

    /**
     * @brief Advance a standalone Modified Puls swale group through the shared legacy kernel.
     *
     * Updates depths, rates, ledgers, wet/dry clocks and drain history without a context or report IO.
     * Native infiltration is zero, its ceiling 1e10 ft/s, factors 1 and old runoff time zero. All
     * wrappers use the same dispatcher; swales iterate with omega=0.5, other ordinary types use one
     * explicit Modified Puls evaluation. The PET array is borrowed and never retained.
     *
     * @param[in,out] g Initialized parallel arrays for the intended Modified Puls swale type; dispatch
     *     actually follows g.type, not the wrapper name. Each unit needs positive geometry required by
     *     that type.
     * @param[in] rainfall Fallback inflow in ft/s when a unit staged inflow is not positive; covered
     *     rain barrels force inflow to zero.
     * @param[in] evap_rate Optional readable g.count-element PET array in ft/s, used only during this
     *     call; null means zero.
     * @param[in] dt Positive interval in seconds; unchecked.
     */
    static void batchSwaleModPuls(LIDGroupSoA& g, double rainfall, const double* evap_rate,
                                  double dt);

private:
    std::vector<LIDGroupSoA> groups_;
    std::vector<std::pair<int, int>> usage_order_; ///< see usageOrder()
    double old_runoff_sec_ = 0.0;

    /// One legacy TLidRptFile per reporting unit (legacy lid.h:150-156):
    /// the stream, the count of successive dry periods (seeded 1 by the
    /// header, initLidRptFile), and the held row that marks the end of a
    /// dry spell. Keyed (group index, unit index); opened lazily on the
    /// unit's first evaluated step.
    struct LidRptFile {
        std::ofstream f;
        int was_dry = 1;
        std::string held;
        bool failed = false;
    };
    std::map<std::pair<int, int>, std::unique_ptr<LidRptFile>> rpt_files_;
    /**
     * @brief Write a unit report row with legacy dry-spell compression.
     *
     * Lazily opens/truncates the configured path, writes headings, and keeps the stream plus held dry
     * row in this solver. An open failure marks the stream failed and silently suppresses later rows;
     * ordinary stream write failure flags are not surfaced. Rows use old_runoff_sec_ + dt and the group
     * display conversions. Intermediate dry rows are held until a later wet step requires the last dry
     * state.
     *
     * @param[in] ctx Model names, first title line and start date used for display.
     * @param[in] gi Valid group index paired with u as the report stream key.
     * @param[in,out] g Initialized group holding report path, staged rates and current layer state; its
     *     arrays are read only.
     * @param[in] u Unchecked valid unit index in g.
     * @param[in] dt Step seconds; positive values convert interval loss depths to rates; nonpositive
     *     values display zero loss rates.
     */
    void saveLidReport(SimulationContext& ctx, int gi, LIDGroupSoA& g, std::size_t u, double dt);
    double recovery_factor_ = 1.0;
    std::vector<double> native_infil_;
    std::vector<double> max_native_infil_;
    std::vector<double> infil_factor_;
};

} // namespace lid
} // namespace openswmm

#endif // OPENSWMM_LID_HPP
