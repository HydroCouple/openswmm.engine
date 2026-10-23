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
 * @file      Runoff.hpp
 * @brief     Subcatchment runoff generation — batch-oriented nonlinear reservoir.
 *
 * @details Three subareas per subcatchment (matching legacy subcatch.c):
 *   - IMPERV0: Impervious with zero depression storage (PctZero fraction)
 *   - IMPERV1: Impervious with depression storage
 *   - PERV:    Pervious with depression storage and infiltration
 *
 *   Depth integration uses the adaptive explicit RK45 ODE solver (matching
 *   legacy odesolve.c) to integrate:
 *     dd/dt = inflow - alpha * (d - Ds)^(5/3)
 *
 * @note Legacy reference: src/legacy/engine/runoff.c, subcatch.c, odesolve.c
 * @ingroup new_engine
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

/**
 * @defgroup engine_hydrology Hydrology and surface processes
 * @ingroup new_engine
 * @brief Rainfall, runoff, infiltration, groundwater, snow and source delivery.
 *
 * Contracts state native versus project/SI units, interval rates versus totals,
 * mutable process state and restart ownership at each interface.
 */

#ifndef OPENSWMM_RUNOFF_HPP
#define OPENSWMM_RUNOFF_HPP

#include "../data/SubcatchData.hpp"
#include "surface/InfilBank.hpp"
#include <vector>
#include <array>
#include <functional>
#include <utility>

namespace openswmm::detail {
class CompletedSourceArchive;
}

namespace openswmm {

struct SimulationContext;

namespace runoff {

/// Completed-interval rates, already resolved to internal ft/s. A non-null
/// batch selects only these sources and bypasses global gage/PET cursors.
/// Coupled outflow uses the integrated reservoir storage balance (interval
/// mean flux); an ordinary solve retains its legacy endpoint-rate rectangle.
struct RunoffSourceForcing {
    int subcatch = -1;
    double rain = 0.0, pet = 0.0;
    std::array<double, 3> surface_return{}; // ft/s by IMPERV0/IMPERV1/PERV; never rain
    double raw_precipitation = -1;          // before subcatchment scales and phase partition
    double pet_imperv = -1, pet_perv = -1;  // optional exposed atmospheric footprints
};
/// Actual completed subarea water budget (ft3; area in ft2). Pool order is
/// IMPERV0, IMPERV1, PERV. Ordinary runoff does not write these diagnostics.
struct CompletedSubareaWater {
    double area = 0, opening = 0, rain = 0, runon = 0, evaporation = 0;
    double infiltration = 0, spatial_infiltration = 0, outflow = 0, outlet = 0, closing = 0;
};

// ============================================================================
// Constants
// ============================================================================

constexpr double MEXP =
    1.6666667; ///< Manning's exponent (legacy subcatch.c:55 literal — match bit-for-bit, NOT 5.0/3.0)
constexpr double ODETOL = 0.0001; ///< ODE solver tolerance (matching legacy)
constexpr double PHI = 1.486;     ///< Manning's US customary constant

// ============================================================================
// Per-subcatchment subarea state (SoA for vectorization)
// ============================================================================

struct RunoffSoA {
    int n_subcatch = 0;

    // Per-subcatchment properties (set at init)
    std::vector<double> area;       ///< Subcatchment area (ft²)
    std::vector<double> width;      ///< Subcatchment width (ft)
    std::vector<double> slope;      ///< Average slope (ft/ft)
    std::vector<double> imperv_pct; ///< Impervious fraction (0-1)
    /// The three subarea area fractions, formed exactly as legacy forms them
    /// (subcatch.c:268-270) from the AUTHORED [SUBAREAS] PctZero percent:
    ///   frac_imperv0 = fracImperv * PctZero / 100
    ///   frac_imperv1 = fracImperv * (1 - PctZero / 100)
    /// Legacy multiplies by the percent and divides afterwards; dividing
    /// first and multiplying by the stored 0-1 fraction is a different
    /// double for about a tenth of all percentages, and the difference lands
    /// straight in the subarea's area. Every consumer — the runoff kernel,
    /// the stored-volume accounting, the water-age and heat watershed
    /// modules — reads these so there is one definition of the split.
    std::vector<double> frac_imperv0; ///< IMPERV0 (no depression storage)
    std::vector<double> frac_imperv1; ///< IMPERV1 (with depression storage)

    // Per-subarea SoA: alpha = runoff coefficient
    std::vector<double> alpha_imperv; ///< Alpha for impervious subareas
    std::vector<double> alpha_perv;   ///< Alpha for pervious subarea

    // Per-subarea SoA: depression storage (ft)
    std::vector<double> ds_imperv; ///< Depression storage for IMPERV1
    std::vector<double> ds_perv;   ///< Depression storage for PERV

    // Per-subarea SoA: Manning's n
    std::vector<double> n_imperv; ///< Manning's n, impervious
    std::vector<double> n_perv;   ///< Manning's n, pervious

    // Per-subarea SoA: ponded depth (state, updated each step)
    std::vector<double> depth_imperv0; ///< Ponded depth, IMPERV0 (ft) — dStore=0
    std::vector<double> depth_imperv1; ///< Ponded depth, IMPERV1 (ft) — dStore>0
    std::vector<double> depth_perv;    ///< Ponded depth, PERV (ft)

    // Per-subarea SoA: previous-step runoff rates (ft/sec, area-averaged)
    // Used for inter-subarea routing (legacy subcatch_getRunon)
    std::vector<double> old_runoff_imperv0; ///< Previous IMPERV0 runoff (ft/sec)
    std::vector<double> old_runoff_imperv1; ///< Previous IMPERV1 runoff (ft/sec)
    std::vector<double> old_runoff_perv;    ///< Previous PERV runoff (ft/sec)

    // Per-subcatchment: computed runoff (output)
    std::vector<double> runoff;    ///< Total runoff rate (cfs)
    std::vector<double> evap_loss; ///< Reserved serialized row; execute writes context ft/s losses.
    std::vector<double>
        infil_loss; ///< Reserved serialized row; execute writes context ft/s losses.
    std::vector<double>
        perv_evap_vol; ///< Pervious-subarea evaporation this step (ft3) — legacy Vpevap
    std::vector<double>
        actual_perv_evap_vol; ///< Actual PERV evaporation alone (ft3), excluding legacy impervious carry-in.
    /// Completed partition diagnostics: ft3 spatial intake and ft/s outside native capacity.
    std::vector<double> spatial_infil_vol, native_infil_rate;
    std::vector<double> infil_vol; ///< Non-LID infiltration this step (ft3) — legacy Vinfil
    /// legacy subcatch_getRunoff's return value: the three subareas' runoff
    /// summed over their areas and divided by the FULL area (ft/s), before
    /// the inter-subarea routing and the LID exchange — what runoff_execute
    /// tests for HasRunoff and hands surfqual_getWashoff.
    std::vector<double> subarea_runoff_rate;

    // Per-subcatchment: per-subarea runoff CFS from non-LID area (Gap #23)
    // Used by SWMMEngine to compute LID unit inflow from impervious/pervious fractions.
    std::vector<double> imperv_runoff_cfs; ///< Impervious subarea runoff (CFS, non-LID area)
    std::vector<double> perv_runoff_cfs;   ///< Pervious subarea runoff (CFS, non-LID area)
    /// Legacy Voutflow: this step's outlet runoff VOLUME from the non-LID
    /// area (ft3). Kept so the LID exchange can rebuild newRunoff in the
    /// volume domain exactly as legacy subcatch.c:746-751 does
    /// (`vOutflow = Voutflow - VlidIn + VlidOut; newRunoff = vOutflow/tStep`).
    std::vector<double> outflow_vol;
    std::vector<std::array<CompletedSubareaWater, 3>> completed;

    /** @brief Reset all three-pool geometry, native water histories and output rows.
     * @param n Nonnegative subcatchment count fitting allocations and int indexing.
     * Installs n rows: geometry/depth/rates/diagnostics zero, Manning n 0.01 impervious
     * and 0.1 pervious. Does not configure soil or context. Sequential allocations can
     * leave a partial reset on exception; callers qualify size before mutation. */
    void resize(int n);
    /** @brief Recompute native Manning reservoir coefficients from current geometry.
     * Uses feet/ft2, slope ft/ft, imperial PHI and authored n, preserving legacy operation
     * order. Impervious alpha uses combined impervious area for both pools. Positive-area
     * rows with nonpositive n/subarea area get zero alpha; total area<=0 retains prior
     * coefficients. Requires aligned arrays and finite nonnegative slope/fractions;
     * negative slope is not guarded and can produce NaN. No context synchronization. */
    void computeAlpha();
};

// ============================================================================
// Runoff solver
// ============================================================================

/** @brief Owned three-pool non-LID reservoir, native soil state and completed routing history.
 * Initialize against one compatible model before use. Geometry is cached; execute
 * mutates both owner and context, invokes an optional borrowed callback, and is
 * not a transaction. Completed-source drivers must trial copies and publish after
 * all coupled water owners accept. Serialize all mutation, borrowing and archives. */
class RunoffSolver {
public:
    /// Whole-pervious-area external boundary for a trial or bounded solve.
    /// Arguments: source index, ponded depth (ft), available water rate after
    /// surface evaporation (ft/s), output intake rate (ft/s). False preserves
    /// the native kernel. True replaces it without advancing native soil state.
    /// The caller must supply completed forcing and commit a validated trial;
    /// this boundary does not select sources or schedule. An optional completed
    /// spatial fraction weights its rate; the remainder uses native soil once.
    using InfiltrationBoundary = std::function<bool(int, double, double, double&)>;
    /** @brief Rebuild owned runoff/native infiltration state for one initialized model.
     * @param ctx Borrowed valid subcatchment geometry/soil parameters and unit enum;
     * width already feet, slope ratio, project area/rain-depth fields and LID area ft2.
     * Does not mutate context; required arrays must have n_subcatches aligned entries.
     * @param spatial_areas Optional unique (zero-based source,m2 non-LID area) overrides;
     * finite nonnegative values bypass project land-area conversion for selected sources.
     * @throws std::invalid_argument Invalid/duplicate override, before clearing this owner.
     * Reinitializes reservoir geometry/depths/history, coefficients, infiltration methods
     * and factors. Zero native drying time is normalized to TINY. Override full denominator
     * includes native LID area. Later allocations/native setup can fail after partial
     * replacement; no general rollback. Reinitialize after changing cached geometry/soil. */
    void init(SimulationContext& ctx,
              const std::vector<std::pair<int, double>>& spatial_areas = {});
    /** @brief Reconstruct only explicitly reviewed non-LID area overrides.
     * @return Copied ordered (source index,m2) pairs. Converts cached ft2 and, if needed,
     * uses one adjacent representable m2 value to reproduce exact division back to ft2.
     * Empty for ordinary initialization. Does not include project/default sources or
     * LID area and does not mutate owner.
     * @throws std::runtime_error No exact one-step reconstruction exists.
     * Allocation failures propagate; serialize with initialization/owner mutation. */
    std::vector<std::pair<int, double>> reviewedAreas() const;
    /** @brief Mutate selected or ordinary three-pool reservoirs over one completed interval.
     * @param[in,out] ctx Compatible initialized model/forcing/history/report/statistic
     * arrays. Publishes rainfall/runoff/ponded and ft/s loss reports, consumes LID return
     * cfs, adds ft3 evaporation/infiltration/subarea statistics and may access gage cursors.
     * Global routing/lateral totals and simulation clocks are handled by the caller.
     * @param dt Finite positive seconds required on completed path and assumed ordinary.
     * @param evap_rate Ordinary broadcast PET ft/s, subject to DRY_ONLY and per-source
     * overrides; ignored by completed forcing. Must be finite nonnegative.
     * @param infil_factor Finite nonnegative default soil multiplier; valid monthly source
     * INFIL pattern replaces it when present. Recorded only when native soil is evaluated.
     * @param recovery_factor Finite nonnegative native dry-recovery multiplier.
     * @param month Zero-based calendar month; negative disables monthly infiltration,
     * DSTORE and N-PERV lookups. Out-of-range factors fall back rather than reject.
     * @param boundary Optional borrowed synchronous pervious intake callback. Arguments:
     * source index, opening ponded feet, nonnegative available ft/s, writable intake ft/s.
     * False keeps native soil; true requires finite nonnegative rate and no lumped aquifer,
     * clips intake to availability, and bypasses native soil on fully spatial area. Callback
     * must outlive call, avoid reentry and keep side effects private until caller accepts.
     * @param source_forcing Null selects all ordinary sources with native gage/PET handling.
     * Non-null selects strictly increasing unique valid indices with nonnegative finite
     * rain/PET/surface-return ft/s. Empty selects none. Supplied rain bypasses gages;
     * existing enabled snow-net reports can still replace per-pool rain. Optional nonnegative
     * pet_imperv/pet_perv override footprints and must be finite (not separately checked);
     * raw_precipitation is not used here. Uses integrated reservoir storage balance for
     * mean discharge, records completed ft3 pool budgets and rescales prior routed volume
     * by previous/current duration. Ordinary retains endpoint-rate rectangle/history.
     * @param spatial_fractions Optional n-source [0,1] finite pervious spatial fractions,
     * requires completed forcing. Positive fraction requires successful callback; remaining
     * native fraction advances its soil once with post-evaporation depth. Full native
     * contribution/history remains separate from spatial intake bookkeeping.
     * @param source_owners Optional ordinary-only n-source map; nonnegative skips that
     * source in every phase, negative processes it. Cannot combine with explicit forcing.
     * @throws std::invalid_argument Invalid completed selection/fraction/owner layout or
     * invalid accepted external intake/lumped-aquifer combination.
     * @throws std::runtime_error Required spatial callback declined a positive fraction.
     * Preflight checks protect before loop; later callback/solver/allocation failures can
     * leave partial owner/context/callback changes. Native ODE status is not checked.
     * Completed callers must solve on private copies and coordinate external receipts.
     * Unselected/owned rows retain prior reports. Does not itself qualify full water closure. */
    void execute(SimulationContext& ctx, double dt, double evap_rate = 0.0,
                 double infil_factor = 1.0, double recovery_factor = 1.0, int month = -1,
                 const InfiltrationBoundary* boundary = nullptr,
                 const std::vector<RunoffSourceForcing>* source_forcing = nullptr,
                 const std::vector<double>* spatial_fractions = nullptr,
                 const std::vector<int>* source_owners = nullptr);

    /** @brief Advance native soil for a full-LID/fully impervious runoff source.
     * @param ctx Compatible native rainfall/runon ft/s rows, borrowed unchanged.
     * @param i Valid source index in context, ponding, factors and infiltration bank.
     * @param dt Finite positive seconds.
     * @param recovery_factor Finite nonnegative dry-recovery multiplier.
     * @return Native ft/s from own rainfall+runon, pervious ponded feet and last recorded
     * infiltration factor. Advances soil history without debiting water, checking actual
     * non-LID area, updating context losses or booking an external receipt; call once at
     * its scheduled native boundary. Bounds/finite input are not checked here. */
    double nativeInfilFullLid(SimulationContext& ctx, int i, double dt, double recovery_factor);

    /** @brief Read the last recorded native infiltration multiplier for one source.
     * @param i Valid nonnegative initialized source index; unchecked vector indexing.
     * @return Factor, initially one. Retained when soil is skipped or fully spatial;
     * therefore not necessarily the factor a current pattern would resolve. No mutation. */
    double infilFactorUsed(int i) const { return infil_factor_used_[static_cast<std::size_t>(i)]; }

    /** @brief Borrow the current owned reservoir geometry, water and diagnostics.
     * @return Member reference until destruction; init/execute/restore/assignment can
     * change or invalidate rows/buffers. Serialize access, copy snapshots across mutation
     * and do not free borrowed storage. Does not include all soil/coupling checkpoint data. */
    const RunoffSoA& soa() const { return soa_; }
    /** @brief Borrow mutable reservoir rows for compatible internal hot-start integration.
     * @return Owned member reference until destruction; same lifetime restrictions as
     * const view. Caller preserves all aligned extents/units and geometry compatibility;
     * manual changes do not rebuild coefficients, soil, routing durations or context
     * reports. Coordinate publication and validation with the surrounding restart owner. */
    RunoffSoA& soa() { return soa_; }
    /** @brief Stream either model identity rows or accepted runoff/soil state.
     * @param[in,out] archive Compatible native reader/writer; partial field mutation or
     * emission can precede failure. State vectors require matching initialized extents.
     * @param model True streams geometry/coefficients/area overrides and soil model data;
     * false streams depths/rates/loss diagnostics, factors/routing duration, lazily sized
     * weather buffers, completed pool budgets and native soil dynamic state.
     * Native feet/ft2/ft3/ft-per-second storage units are preserved. Does not stream
     * n_subcatch, finish/version the enclosing archive, compare model rows or generally
     * validate physical state. Reading checks nonempty working buffers match source count.
     * Caller decodes a copy, finishes the outer archive, calls validateCheckpoint and
     * coordinates all context/coupled owners before publication. Exceptions propagate. */
    void checkpoint(detail::CompletedSourceArchive& archive, bool model);
    /** @brief Validate selected decoded numeric fields against an initialized runoff model.
     * @param model Borrowed compatible initialization used to compare source count and
     * exact area vector, and qualify native infiltration model identity/state.
     * @throws std::runtime_error Count/area mismatch or nonfinite/negative dynamic rates,
     * depths, statistics, factors, routing durations, weather buffers or completed budgets.
     * No mutation. Does not independently compare all runoff geometry vectors, verify all
     * extents, pool water closure or full surrounding context identity; fixed-shape archive
     * and containing restart validation must supply those checks before publication. */
    void validateCheckpoint(const RunoffSolver& model) const;
    /** @brief Read completed inter-subarea outflow waiting for its next routing interval.
     * @param ctx Matching subarea route/pct_routed rows.
     * @param source Valid nonnegative initialized index in geometry and duration arrays.
     * @return ft3 from prior source rates, fractions, routed share, non-LID area and last
     * completed seconds. Zero when wholly impervious/pervious, route disabled or no
     * eligible receiving pool. No bounds/finite validation, water debit or history reset;
     * reading twice returns the same pending volume until another solve/publication. */
    double pendingRoutingVolume(const SimulationContext& ctx, int source) const;

    // -----------------------------------------------------------------------
    // Hot start helpers — Gap #54
    // -----------------------------------------------------------------------

    /** @brief Copy one native soil history into the legacy six-double hot-start layout.
     * @param i Source index; invalid returns model zero and six zeros.
     * @param[out] model Stored InfilModel enum; external ownership still reports its type.
     * @param[out] state Writable six doubles, nonnull. Horton: tp/Fe/Fmh/0/0/0;
     * Green-Ampt: IMD/F/Fu/T/saturated/0; Curve Number: S/Se/P/F/f/T. Native soil feet/
     * seconds and stored method conventions are retained. External/stateless returns zeros.
     * Does not serialize parameters, cumulative SI intake or owner identity; no mutation. */
    void infil_get_state(int i, int& model, double state[6]) const noexcept;

    /** @brief Restore matching native soil history from six legacy hot-start scalars.
     * @param i Source index; invalid or externally owned is ignored.
     * @param model Must exactly equal initialized method; mismatch silently ignored.
     * @param state Borrowed six doubles in infil_get_state native layout/units, nonnull
     * when used. Green-Ampt saturated is true for any nonzero slot four.
     * Does not validate finite/sign bounds, configure parameters, change owner or restore
     * cumulative SI intake; caller qualifies input and paired checkpoint before publishing. */
    void infil_set_state(int i, int model, const double state[6]) noexcept;

private:
    RunoffSoA soa_;
    std::vector<double>
        spatial_full_area_ft2_; ///< Reviewed denominator, absent on the legacy path.

    // Infiltration state (one per subcatchment)
    surface::InfilBank infil_bank_;
    std::vector<double>
        infil_factor_used_; ///< The InfilFactor applied to each subcatchment this step (pattern or global)
    std::vector<double> completed_step_seconds_; ///< Volume history for selected interval routing.

    /** @brief Dispatch native soil capacity with source-specific adjustment factors.
     * @param ctx Legacy context argument, unused by this adapter.
     * @param i Valid initialized native soil row.
     * @param precip Finite nonnegative precipitation ft/s.
     * @param runon Finite nonnegative separate runon ft/s.
     * @param depth Finite nonnegative available ponded feet.
     * @param dt Finite positive seconds.
     * @param local_infil Finite nonnegative infiltration multiplier.
     * @param recovery_factor Finite nonnegative native recovery multiplier.
     * @return Native ft/s, advancing method history through InfilBank::rateFeet. No
     * external water debit/global loss booking or additional argument checks here. */
    double infilGetInfil(SimulationContext& ctx, int i, double precip, double runon, double depth,
                         double dt, double local_infil, double recovery_factor);

    // Working buffers (reused each step, sized to n_subcatch)
    std::vector<double> precip_;
    std::vector<double> evap_rate_;
    std::vector<double> infil_rate_;

    /** @brief Integrate one nonlinear reservoir depth after filling depression storage.
     * @param[in,out] depth Finite nonnegative initial ponded feet, replaced and floored at zero.
     * @param inflow Finite net ft/s after evaporation/infiltration; may be negative.
     * @param alpha Finite nonnegative coefficient giving ft/s from excess_depth^MEXP.
     * @param dStore Finite nonnegative depression-storage feet.
     * @param dt Finite positive interval seconds.
     * @param[out] t_runoff Remaining tx seconds after filling storage; retains dt in
     * below-storage shortcut, so this output is not always actual above-storage duration.
     * Uses adaptive explicit RK45 with ODETOL and initial step tx when alpha/tx positive,
     * otherwise algebraic accumulation. Ignores solver return status and publishes its
     * returned depth even on a solver failure; caller owns broader acceptance/rollback.
     * No context/soil effects or general finite/physical qualification. */
    static void updatePondedDepth(double& depth, double inflow, double alpha, double dStore,
                                  double dt, double& t_runoff);
};

} // namespace runoff
} // namespace openswmm

#endif // OPENSWMM_RUNOFF_HPP
