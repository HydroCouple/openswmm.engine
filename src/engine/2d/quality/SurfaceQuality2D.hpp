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
 * @file      SurfaceQuality2D.hpp
 * @brief     S7 (2026-09-19) — cell coverages, buildup, washoff and street
 *        sweeping on the 2D overland surface.
 *
 * @details The subcatchment land-use convention on the mesh: a cell (or a
 *          tag, or the whole mesh) names which EXISTING land uses cover it
 *          and by what percent; buildup accrues per (cell, land use, surface
 *          species) with the land use's own `[BUILDUP]` function, is swept on
 *          the `[LANDUSES]` schedule, and is washed off by the cell's runoff
 *          into the cell's species row — from where it advects to the 1D
 *          network with the water. Surface species = pollutants ∪ the
 *          reactions component's (MSX) species (BW-MSX), keyed by the
 *          transport row layout, so nothing here is pollutant-shaped.
 *
 *          Sections (`[2D_*]` family in the `.inp` for v1; scope keys and the
 *          1-based CELL index as `[2D_INFILTRATION]` / `[GW_*]`):
 *
 *              [2D_COVERAGES]     scope LANDUSE PERCENT [LANDUSE PERCENT]…
 *              [2D_LOADINGS]      scope SPECIES INITIAL_BUILDUP   (mass / unit area)
 *              [2D_CURB_LENGTH]   scope LENGTH                    (project length units)
 *
 *          scope = `*` | `TAG name` | `CELL n`; GLOBAL < TAG < CELL; a
 *          coverage row REPLACES the set for its scope (no merging).
 *
 *          Cell runoff rate (D-A22, revised 2026-09-19): the cell's NET
 *          outflow over the runoff step — what left across its faces, open
 *          boundary edges and drains to nodes, minus what arrived across
 *          faces, from node spills, outfall discharge and inflow boundaries —
 *          clamped at zero, per unit area; accumulated by the marcher in
 *          `SurfaceTransportState::cell_runoff_vol`. That is the runoff the
 *          cell itself produced (rain excess plus storage release), the cell
 *          analogue of the subcatchment step's `q = outflow / area`: zero on
 *          a still pond, zero on a cell that only passes upstream water
 *          through, and independent of how finely a plane is meshed. (The
 *          gross outward face flux was considered and rejected: it grows
 *          with the count of cells upstream, so EMC and RC loads — and EXP's
 *          rate — would scale with the mesh.) Defined on triangles and quads
 *          alike.
 *
 *          Cadence (D-A26): buildup, washoff and sweeping run at the RUNOFF
 *          step, right after the subcatchment surface-quality step, so the
 *          two share one calendar (dry days, sweep season); the washoff mass
 *          is added to the cell row as a source, which is conservative at any
 *          cadence.
 *
 *          Design: `plans/transport/OVERLAND_TRANSPORT_HEAT_MSX_PLAN_2026-09-01.md`
 *          §8 (2026-09-19); program plan D-A22–D-A26.
 *
 * @ingroup engine_2d
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_QUALITY_SURFACE_QUALITY_2D_HPP
#define OPENSWMM_ENGINE_2D_QUALITY_SURFACE_QUALITY_2D_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "../../input/SectionRegistry.hpp"

namespace openswmm {
struct SimulationContext;
}

namespace openswmm::twoD {

struct MeshData;
struct SurfaceStateData;
struct SurfaceTransportState;
struct SolverOptions2D;

/// Row scope — the `[2D_INFILTRATION]` / `[GW_*]` ladder.
enum class SqScope : int8_t { GLOBAL = 0, TAG = 1, CELL = 2 };

struct SqScopeKey {
    SqScope scope = SqScope::GLOBAL;
    std::string tag; ///< TAG rows
    int cell = -1;   ///< CELL rows, 0-based internally (1-based in the file)
};

/// `[2D_COVERAGES]` — one row = the whole coverage set of its scope.
struct CoverageRow2D {
    SqScopeKey key;
    std::vector<std::pair<std::string, double>> uses; ///< (land use, percent)
};
/// `[2D_LOADINGS]` — initial buildup, user mass per unit land area.
struct LoadingRow2D {
    SqScopeKey key;
    std::string species;
    double value = 0.0;
};
/// `[2D_CURB_LENGTH]` — curb length per cell, project length units.
struct CurbRow2D {
    SqScopeKey key;
    double length = 0.0;
};

/**
 * @brief The S7 store: authored rows, resolved per-cell coverages and state, and the per-species
 * ledgers.
 */
class SurfaceQuality2D {
public:
    // ---- authoring (section handlers, C API, writer) -----------------------
    std::vector<CoverageRow2D> coverage_rows;
    std::vector<LoadingRow2D> loading_rows;
    std::vector<CurbRow2D> curb_rows;
    /**
     * @brief Test whether any coverage, loading or curb authoring row exists.
     * @return True for any nonempty authored vector, independent of resolved
     * activation/validity. No resolution, allocation or state change occurs.
     */
    bool authored() const noexcept {
        return !coverage_rows.empty() || !loading_rows.empty() || !curb_rows.empty();
    }
    /**
     * @brief Clear all authored coverage, loading and curb-length rows.
     * @note Does not clear resolved runtime arrays, activation, buildup or cumulative ledgers.
     * Caller re-resolves before reusing changed authoring. Invalidates row views and requires
     * exclusive access.
     */
    void clearRows() {
        coverage_rows.clear();
        loading_rows.clear();
        curb_rows.clear();
    }

    /**
     * @brief Bind the rows to the mesh, the land uses and the transport rows.
     * @return Diagnostics; an entry starting with "ERROR" is fatal (unknown land use / species /
     * tag / cell, percent > 100, PER_CURB land use without a curb length), the rest are warnings
     * (inert under RAINFALL_MODE NONE; double-counting with a covered subcatchment).
     *
     * @param[in] ctx Prepared land-use, pollutant/MSX, unit-system and subcatchment identity.
     * Parameters are copied; source context must remain compatible for subsequent
     * initialization/steps.
     * @param[in] mesh Prepared SI cell areas/centroids/tags and stable cell identity.
     * @param[in] tr Prepared native transport layout, with pollutant/MSX rows first and matching
     * names. Age/temperature are excluded from buildup rows.
     * @param[in] opts Resolved rainfall ownership and mesh coordinate conversion. RAINFALL_MODE
     * NONE leaves runtime inert after authoring validation; possible subcatchment double-loading
     * emits warning.
     * @note Resets active first and progressively rebuilds copied parameters, scope-resolved
     * fractions, curb/loadings and zeroed runtime arrays/ledgers. GLOBAL < TAG < CELL overrides
     * replace each coverage set; same-pass row ordering is retained. Calls initState()
     * separately after successful activation. Failure can leave partial/inactive arrays;
     * allocations can throw. Does not modify context/mesh/tr. Requires exclusive access;
     * authored numeric rows must satisfy parser constraints.
     */
    std::vector<std::string> resolve(const SimulationContext& ctx, const MeshData& mesh,
                                     const SurfaceTransportState& tr, const SolverOptions2D& opts);

    /**
     * @brief Read whether the last resolution activated mesh surface quality.
     * @return Stored flag, false after unresolved/failed/inert resolution. Does not validate
     * current external row identity or inspect whether any cell currently has runoff/buildup. No
     * mutation.
     */
    bool active() const noexcept { return active_; }

    /**
     * @brief Seed initial buildup: `[2D_LOADINGS]` row, else the buildup function at `[OPTIONS]
     * DRY_DAYS` (legacy landuse_getInitBuildup).
     *
     * @param[in] ctx Context with initial dry_days; must match resolved land-use/species
     * identity.
     * @param[in] mesh Matching SI cell areas; all configured cells must be present.
     * @note Inactive returns. Positive authored initial loading overrides function seeding;
     * otherwise positive dry days seeds nonexternal buildup laws. Writes normalized buildup and
     * adds user mass to led_init_ for covered cells. Zero seeding does not clear previous
     * buildup, and ledgers accumulate: call once after resolve() initializes arrays, not
     * repeatedly as a reset. No water/species is added to surface transport until later washoff.
     * Requires exclusive runtime access.
     */
    void initState(const SimulationContext& ctx, const MeshData& mesh);

    /**
     * @brief One runoff step: buildup accrual, washoff into the cell rows, street sweeping.
     * @param dt_runoff  runoff step (s)
     * @param abs_time   absolute date-time at the START of the step
     * @param is_raining engine's current-step rainfall flag (sweeping gate)
     *
     * @param[in] ctx Matching engine land-use schedule, current unit system and external buildup
     * tables. Although const-qualified, external buildup lookups update shared table cursors
     * through const_cast; caller serializes with other cursor users.
     * @param[in] mesh Matching SI cell area array covering nCells().
     * @param[in,out] state Prepared matching surface transport. Consumes and zeros each
     * cell_runoff_vol, adds native concentration*m3 washoff inventory to
     * cell_mass/gained_washoff and updates this owner's buildup, calendar sweep clocks and
     * cumulative user-mass/runoff ledgers.
     * @note dt_runoff must be finite positive; inactive/nonpositive and mismatched/inactive
     * transport returns without work. abs_time is the engine DateTime in days, not elapsed
     * seconds. Buildup on dry runoff steps precedes scheduled dry/in-season sweeping, then wet
     * washoff/BMP removal. Net positive produced runoff determines the rate, not gross lateral
     * throughput. No hydraulic water is removed. State changes progressively; allocation or
     * lookup failures do not roll back earlier cells/rows.
     */
    void step(const SimulationContext& ctx, const MeshData& mesh, SurfaceStateData& state,
              double dt_runoff, double abs_time, bool is_raining);

    // ---- read-back --------------------------------------------------------
    /**
     * @brief Read the resolved land-use count.
     * @return Stored nonnegative count after valid resolution; not a live query of context
     * authoring. An inactive object can retain counts from partial/previous resolution. No
     * allocation or mutation.
     */
    int nLandUses() const noexcept { return n_lu_; }
    /**
     * @brief Read the resolved pollutant plus MSX buildup row count, excluding age/temperature.
     * @return Stored nonnegative count after valid resolution; not a live query of context
     * authoring. An inactive object can retain counts from partial/previous resolution. No
     * allocation or mutation.
     */
    int nSpecies() const noexcept {
        return n_sp_;
    } /**
     * @brief < surface species (rows that build up)
     *
     * @details Read the resolved mesh-cell count.
     * @return Stored nonnegative count after valid resolution; not a live query of context
     * authoring. An inactive object can retain counts from partial/previous resolution. No
     * allocation or mutation.
     */
    int nCells() const noexcept { return n_cells_; }
    /**
     * @brief Transport row index of surface species `s` (pollutants first, then MSX).
     *
     * @details Read resolved per-species metadata.
     * @param[in] s Valid surface species index [0,nSpecies()); unchecked.
     * @return Native transport row index for the resolved surface species.
     * @note Requires stable resolved arrays.  No mutation or validation occurs.
     */
    int rowOf(int s) const noexcept { return row_of_[static_cast<std::size_t>(s)]; }
    /**
     * @brief Read resolved per-species metadata.
     * @param[in] s Valid surface species index [0,nSpecies()); unchecked.
     * @return Borrowed solver-owned exact species name; valid until resolve/replacement.
     * @note Requires stable resolved arrays. No copy or ownership transfer occurs. No mutation
     * or validation occurs.
     */
    const std::string& speciesName(int s) const noexcept {
        return names_[static_cast<std::size_t>(s)];
    }
    /**
     * @brief Read one resolved cell/land-use coverage fraction.
     * @param[in] c Valid cell [0,nCells()), unchecked.
     * @param[in] lu Valid land use [0,nLandUses()), unchecked.
     * @return Fraction 0..1 from resolved scope precedence, not percentage. Read-only and
     * allocation-free; does not infer active quality or current buildup.
     */
    double coverage(int c, int lu) const noexcept { return cov_[cidx(c, lu)]; }
    /**
     * @brief Read resolved curb normalization length.
     * @param[in] c Valid cell [0,nCells()), unchecked.
     * @return Stored project length units, zero where unauthored. Not SI-converted
     * automatically. Requires stable resolved arrays; no mutation, allocation or current
     * geometry lookup occurs.
     */
    double curbLength(int c) const noexcept { return curb_[static_cast<std::size_t>(c)]; }
    /**
     * @brief Buildup of species s on land use lu of cell c, user mass per normalizer unit.
     *
     * @param[in] c Valid resolved cell, unchecked.
     * @param[in] lu Valid resolved land-use row, unchecked.
     * @param[in] s Valid resolved buildup species, unchecked.
     * @return Stored user mass per land-area or curb normalizer unit. To obtain actual user
     * mass, multiply by coverage times cell land area or coverage times curb length according to
     * the law. Read-only and allocation-free; no normalization, sweeping or washoff is
     * performed.
     */
    double buildup(int c, int lu, int s) const noexcept { return bu_[bidx(c, lu, s)]; }
    /**
     * @brief Total buildup of species s on cell c per unit land area (user mass / acre or ha) —
     * what the results file reports.
     *
     * @param[in] c Valid resolved cell when active, unchecked.
     * @param[in] s Valid resolved species when active, unchecked.
     * @return Total stored buildup user mass per acre (US) or ha (SI), summed across covered
     * land uses. Inactive/nonpositive resolved land area returns zero. Uses each law's actual
     * area/curb normalizer; no mutation or allocation occurs.
     */
    double buildupPerArea(int c, int s) const noexcept;
    /**
     * @brief Per-species ledgers, user mass, cumulative.
     *
     * @details Borrow cumulative seeded initial buildup per surface species.
     * @return Solver-owned const vector in user mass units (lb/kg or configured counts
     * convention), ordered by rowOf()/speciesName(). resolve() resets/replaces it;
     * step/initState mutate it as applicable. Read access requires stable owner; no copy, reset
     * or ownership transfer occurs.
     */
    const std::vector<double>& ledInitBuildup() const noexcept { return led_init_; }
    /**
     * @brief Borrow cumulative accrued buildup, including generated storeless-washoff supply per
     * surface species.
     * @return Solver-owned const vector in user mass units (lb/kg or configured counts
     * convention), ordered by rowOf()/speciesName(). resolve() resets/replaces it;
     * step/initState mutate it as applicable. Read access requires stable owner; no copy, reset
     * or ownership transfer occurs.
     */
    const std::vector<double>& ledBuildup() const noexcept { return led_bu_; }
    /**
     * @brief Borrow cumulative net washoff delivered to surface transport after BMP per surface
     * species.
     * @return Solver-owned const vector in user mass units (lb/kg or configured counts
     * convention), ordered by rowOf()/speciesName(). resolve() resets/replaces it;
     * step/initState mutate it as applicable. Read access requires stable owner; no copy, reset
     * or ownership transfer occurs.
     */
    const std::vector<double>& ledWashoff() const noexcept { return led_wo_; }
    /**
     * @brief Borrow cumulative buildup removed by sweeping per surface species.
     * @return Solver-owned const vector in user mass units (lb/kg or configured counts
     * convention), ordered by rowOf()/speciesName(). resolve() resets/replaces it;
     * step/initState mutate it as applicable. Read access requires stable owner; no copy, reset
     * or ownership transfer occurs.
     */
    const std::vector<double>& ledSweeping() const noexcept { return led_sw_; }
    /**
     * @brief Borrow cumulative washoff removed by BMP treatment per surface species.
     * @return Solver-owned const vector in user mass units (lb/kg or configured counts
     * convention), ordered by rowOf()/speciesName(). resolve() resets/replaces it;
     * step/initState mutate it as applicable. Read access requires stable owner; no copy, reset
     * or ownership transfer occurs.
     */
    const std::vector<double>& ledBmp() const noexcept { return led_bmp_; }
    /**
     * @brief Per-species conversion: concentration-mass units → user mass (lbs / kg).
     *
     * @details Read resolved per-species metadata.
     * @param[in] s Valid surface species index [0,nSpecies()); unchecked.
     * @return Conversion from concentration-mass units to user mass (lb/kg, or counts
     * convention).
     * @note Requires stable resolved arrays. The separate row-volume factor 1000 converts
     * concentration*m3 to concentration-mass. No mutation or validation occurs.
     */
    double mcf(int s) const noexcept { return mcf_[static_cast<std::size_t>(s)]; }
    /**
     * @brief Cumulative runoff volume (m³) the cell produced (the positive part of its net
     * outflow per step, D-A22) — the `q·A·dt` the washoff laws saw.
     *
     * @param[in] c Valid resolved cell index, unchecked.
     * @return Cumulative nonnegative produced runoff, m3, since resolution. Wet eligibility uses
     * q >= MIN_RUNOFF, while actual washoff requires q > MIN_RUNOFF. This read does not consume
     * the per-step transport runoff accumulator or recompute volume. Requires stable arrays; no
     * allocation/mutation.
     */
    double runoffVolume(int c) const noexcept { return runoff_vol_[static_cast<std::size_t>(c)]; }
    /**
     * @brief …of which on steps fast enough to wash off (legacy MIN_RUNOFF): the volume an EMC
     * washes off at its concentration.
     *
     * @param[in] c Valid resolved cell index, unchecked.
     * @return Cumulative produced runoff on rate-threshold-eligible wet steps, m3, since
     * resolution. Wet eligibility uses q >= MIN_RUNOFF, while actual washoff requires q >
     * MIN_RUNOFF. This read does not consume the per-step transport runoff accumulator or
     * recompute volume. Requires stable arrays; no allocation/mutation.
     */
    double wetRunoffVolume(int c) const noexcept {
        return wet_runoff_vol_[static_cast<std::size_t>(c)];
    }

private:
    /**
     * @brief Flatten a resolved cell/land-use pair in cell-major coverage layout.
     * @param[in] c Valid cell index, unchecked.
     * @param[in] lu Valid land-use index, unchecked.
     * @return size_t(c)*n_lu_+lu. Negative indices become large unsigned values; callers
     * validate them. No bounds test, array access, allocation or mutation occurs.
     */
    std::size_t cidx(int c, int lu) const noexcept {
        return static_cast<std::size_t>(c) * static_cast<std::size_t>(n_lu_) +
               static_cast<std::size_t>(lu);
    }
    /**
     * @brief Flatten buildup inventory in land-use/species/cell order.
     * @param[in] c Valid cell index, unchecked.
     * @param[in] lu Valid land-use index, unchecked.
     * @param[in] s Valid surface buildup species, unchecked.
     * @return (lu*n_sp_+s)*n_cells_+c using size_t arithmetic. Prepared counts/products must fit
     * size_t; no bounds validation, array access, allocation or mutation occurs.
     */
    std::size_t bidx(int c, int lu, int s) const noexcept {
        return (static_cast<std::size_t>(lu) * static_cast<std::size_t>(n_sp_) +
                static_cast<std::size_t>(s)) *
                   static_cast<std::size_t>(n_cells_) +
               static_cast<std::size_t>(c);
    }
    struct Params { // per (lu, s), read from the 1D tables at resolve
        int bu_type = 0;
        double c0 = 0, c1 = 0, c2 = 0;
        int ts_idx = -1;
        int normalizer = 0;
        double max_days = 0;
        int wo_type = 0;
        double coeff = 0, expon = 0, sweep_effic = 0, bmp_effic = 0;
    };
    /**
     * @brief Borrow the copied buildup/washoff coefficients for one land use and species.
     * @param[in] lu Valid resolved land-use index, unchecked.
     * @param[in] s Valid resolved surface species index, unchecked.
     * @return Solver-owned Params reference in [lu*n_sp_+s] order, stable until
     * resolution/replacement. No live context refresh, allocation or validation occurs.
     */
    const Params& param(int lu, int s) const noexcept {
        return params_[static_cast<std::size_t>(lu) * static_cast<std::size_t>(n_sp_) +
                       static_cast<std::size_t>(s)];
    }
    /**
     * @brief Copy land-use pollutant/MSX buildup and washoff laws into resolved parameter rows.
     * @param[in] ctx Prepared matching land-use, pollutant and reaction surface tables. Parallel
     * arrays must have consistent counts/shapes; guarded primary fields do not validate all
     * companion arrays.
     * @note Replaces params_ for n_lu_*n_sp_, defaults unavailable laws, records external series
     * indices and derives the function's near-maximum dry-day horizon. Coefficients retain
     * engine user-mass, normalizer and day conventions; not all are SI values. Allocates and can
     * throw after replacement. Does not alter authored rows, buildup inventory or the source
     * tables; requires exclusive access.
     */
    void loadParams(const SimulationContext& ctx);

    bool active_ = false;
    int n_cells_ = 0, n_lu_ = 0, n_sp_ = 0;
    bool si_ = false;
    double area_to_landarea_ = 1.0;      ///< m² → acres (US) or ha (SI)
    std::vector<int> row_of_;            ///< [s] transport row
    std::vector<std::string> names_;     ///< [s]
    std::vector<double> mcf_;            ///< [s]
    std::vector<Params> params_;         ///< [lu * n_sp + s]
    std::vector<double> cov_;            ///< [c * n_lu + lu] fraction 0..1
    std::vector<double> curb_;           ///< [c] project length units
    std::vector<double> area_la_;        ///< [c] cell area in land-area units (acres | ha)
    std::vector<double> init_;           ///< [c * n_sp + s] loading, user mass per area (0 = none)
    std::vector<double> bu_;             ///< bidx(c, lu, s)
    std::vector<double> last_swept_;     ///< [c * n_lu + lu] days
    std::vector<double> runoff_vol_;     ///< [c] m³, cumulative
    std::vector<double> wet_runoff_vol_; ///< [c] m³, cumulative, washoff-eligible steps
    std::vector<double> led_init_, led_bu_, led_wo_, led_sw_, led_bmp_; ///< [s] user mass
};

// ---- section parsers (exposed for tests) -----------------------------------
/**
 * @brief Parse one scoped 2D coverage authoring row.
 * @param[in] tokens Complete tokenized row beginning *, TAG name or one-based CELL n, followed
 * by LANDUSE/PERCENT pairs, nonnegative percentages summing to at most 100+1e-9. Numeric
 * conversion accepts NaN/infinity where comparison checks do not reject them; upstream input
 * should supply finite representable values. IDs are resolved later.
 * @param[in,out] rows Destination authoring vector. On success erases any earlier identical full
 * scope key then appends the replacement coverage set.
 * @return Empty on success, otherwise parse diagnostic with rows unchanged before append/erase.
 * @note Allocation can throw; replacement erasure can precede a failing final allocation. No
 * runtime state or ledger is resolved.
 */
std::string parse2DCoveragesLine(const std::vector<std::string>& tokens,
                                 std::vector<CoverageRow2D>& rows);
/**
 * @brief Parse one scoped 2D loading authoring row.
 * @param[in] tokens Complete tokenized row beginning *, TAG name or one-based CELL n, followed
 * by SPECIES and nonnegative initial user mass per acre/ha. Numeric conversion accepts
 * NaN/infinity where comparison checks do not reject them; upstream input should supply finite
 * representable values. IDs are resolved later.
 * @param[in,out] rows Destination authoring vector. Appends on success; duplicate scope/species
 * handling is deferred to ordered resolution.
 * @return Empty on success, otherwise parse diagnostic with rows unchanged before append/erase.
 * @note Allocation can throw; ordinary validation failure appends nothing. No runtime state or
 * ledger is resolved.
 */
std::string parse2DLoadingsLine(const std::vector<std::string>& tokens,
                                std::vector<LoadingRow2D>& rows);
/**
 * @brief Parse one scoped 2D curb authoring row.
 * @param[in] tokens Complete tokenized row beginning *, TAG name or one-based CELL n, followed
 * by one nonnegative length in project length units. Numeric conversion accepts NaN/infinity
 * where comparison checks do not reject them; upstream input should supply finite representable
 * values. IDs are resolved later.
 * @param[in,out] rows Destination authoring vector. Appends on success; duplicate scope/species
 * handling is deferred to ordered resolution.
 * @return Empty on success, otherwise parse diagnostic with rows unchanged before append/erase.
 * @note Allocation can throw; ordinary validation failure appends nothing. No runtime state or
 * ledger is resolved.
 */
std::string parse2DCurbLengthLine(const std::vector<std::string>& tokens,
                                  std::vector<CurbRow2D>& rows);

/**
 * @brief Register `[2D_COVERAGES]`, `[2D_LOADINGS]`, `[2D_CURB_LENGTH]` writing into @p sq.
 *
 * @param[in,out] sq Authoring owner captured by reference in registered callbacks. Must outlive
 * registry callback use; prior rows are retained and new lines append/replace through parsers.
 * @param[in,out] registry Installs custom handlers for the three 2D surface-quality sections.
 * @note Handlers skip empty tokens and stop on the first parse error, set context error_code=5
 * and error_message with the original line. Earlier lines/sections remain applied.
 * Tokenization/string allocation exceptions are not caught here; registration/use require
 * serialized authoring access.
 */
void registerSurfaceQualitySections(SurfaceQuality2D& sq, input::SectionRegistry& registry);

/**
 * @brief Scope key ↔ file token ("*", "TAG name", "CELL n" — 1-based).
 *
 * @param[in] k Borrowed internal zero-based scope key.
 * @return Owned file token text: TAG plus exact name, CELL plus cell+1, otherwise *. Does not
 * quote or validate names/indices; caller supplies a valid resolved key. String allocation may
 * throw.
 */
std::string formatSqScope(const SqScopeKey& k);
/**
 * @brief Parse a scoped surface-quality row prefix and expose its first payload token.
 * @param[in] tokens Tokenized row, always parsed from tokens[0], independent of initial at.
 * @param[out] at Set to the first payload index (1 for *, 2 for TAG/CELL) only on successful
 * scope parse.
 * @param[in,out] key Receives parsed scope plus tag or zero-based cell; unrelated fields retain
 * old values. Failure generally preserves it; no mesh/name resolution occurs.
 * @param[in] section Nonnull terminated section label included in errors.
 * @return Empty on success; diagnostic on empty/unknown/missing/invalid scope. CELL must parse
 * as a positive integral double; representability as int is a caller/input precondition because
 * this parser does not check the conversion range. Allocation can throw; no authoring row is
 * appended.
 */
std::string parseSqScope(const std::vector<std::string>& tokens, std::size_t& at, SqScopeKey& key,
                         const char* section);

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_QUALITY_SURFACE_QUALITY_2D_HPP
