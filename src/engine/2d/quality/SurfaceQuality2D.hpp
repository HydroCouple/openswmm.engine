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
 * @file SurfaceQuality2D.hpp
 * @brief S7 (2026-09-19) — cell coverages, buildup, washoff and street
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
    SqScope     scope = SqScope::GLOBAL;
    std::string tag;          ///< TAG rows
    int         cell  = -1;   ///< CELL rows, 0-based internally (1-based in the file)
};

/// `[2D_COVERAGES]` — one row = the whole coverage set of its scope.
struct CoverageRow2D {
    SqScopeKey key;
    std::vector<std::pair<std::string, double>> uses;   ///< (land use, percent)
};
/// `[2D_LOADINGS]` — initial buildup, user mass per unit land area.
struct LoadingRow2D {
    SqScopeKey  key;
    std::string species;
    double      value = 0.0;
};
/// `[2D_CURB_LENGTH]` — curb length per cell, project length units.
struct CurbRow2D {
    SqScopeKey key;
    double     length = 0.0;
};

/**
 * @brief The S7 store: authored rows, resolved per-cell coverages and state,
 *        and the per-species ledgers.
 */
class SurfaceQuality2D {
public:
    // ---- authoring (section handlers, C API, writer) -----------------------
    std::vector<CoverageRow2D> coverage_rows;
    std::vector<LoadingRow2D>  loading_rows;
    std::vector<CurbRow2D>     curb_rows;
    bool authored() const noexcept {
        return !coverage_rows.empty() || !loading_rows.empty() || !curb_rows.empty();
    }
    void clearRows() { coverage_rows.clear(); loading_rows.clear(); curb_rows.clear(); }

    /**
     * @brief Bind the rows to the mesh, the land uses and the transport rows.
     * @return Diagnostics; an entry starting with "ERROR" is fatal (unknown
     *         land use / species / tag / cell, percent > 100, PER_CURB land
     *         use without a curb length), the rest are warnings (inert under
     *         RAINFALL_MODE NONE; double-counting with a covered
     *         subcatchment).
     */
    std::vector<std::string> resolve(const SimulationContext& ctx, const MeshData& mesh,
                                     const SurfaceTransportState& tr,
                                     const SolverOptions2D& opts);

    bool active() const noexcept { return active_; }

    /// Seed initial buildup: `[2D_LOADINGS]` row, else the buildup function
    /// at `[OPTIONS] DRY_DAYS` (legacy landuse_getInitBuildup).
    void initState(const SimulationContext& ctx, const MeshData& mesh);

    /**
     * @brief One runoff step: buildup accrual, washoff into the cell rows,
     *        street sweeping.
     * @param dt_runoff  runoff step (s)
     * @param abs_time   absolute date-time at the START of the step
     * @param is_raining engine's current-step rainfall flag (sweeping gate)
     */
    void step(const SimulationContext& ctx, const MeshData& mesh, SurfaceStateData& state,
              double dt_runoff, double abs_time, bool is_raining);

    // ---- read-back --------------------------------------------------------
    int nLandUses() const noexcept { return n_lu_; }
    int nSpecies()  const noexcept { return n_sp_; }          ///< surface species (rows that build up)
    int nCells()    const noexcept { return n_cells_; }
    /// Transport row index of surface species `s` (pollutants first, then MSX).
    int rowOf(int s) const noexcept { return row_of_[static_cast<std::size_t>(s)]; }
    const std::string& speciesName(int s) const noexcept { return names_[static_cast<std::size_t>(s)]; }
    double coverage(int c, int lu) const noexcept { return cov_[cidx(c, lu)]; }
    double curbLength(int c) const noexcept { return curb_[static_cast<std::size_t>(c)]; }
    /// Buildup of species s on land use lu of cell c, user mass per normalizer unit.
    double buildup(int c, int lu, int s) const noexcept { return bu_[bidx(c, lu, s)]; }
    /// Total buildup of species s on cell c per unit land area (user mass /
    /// acre or ha) — what the results file reports.
    double buildupPerArea(int c, int s) const noexcept;
    /// Per-species ledgers, user mass, cumulative.
    const std::vector<double>& ledInitBuildup() const noexcept { return led_init_; }
    const std::vector<double>& ledBuildup()     const noexcept { return led_bu_; }
    const std::vector<double>& ledWashoff()     const noexcept { return led_wo_; }
    const std::vector<double>& ledSweeping()    const noexcept { return led_sw_; }
    const std::vector<double>& ledBmp()         const noexcept { return led_bmp_; }
    /// Per-species conversion: concentration-mass units → user mass (lbs / kg).
    double mcf(int s) const noexcept { return mcf_[static_cast<std::size_t>(s)]; }
    /// Cumulative runoff volume (m³) the cell produced (the positive part of
    /// its net outflow per step, D-A22) — the `q·A·dt` the washoff laws saw.
    double runoffVolume(int c) const noexcept { return runoff_vol_[static_cast<std::size_t>(c)]; }

private:
    std::size_t cidx(int c, int lu) const noexcept {
        return static_cast<std::size_t>(c) * static_cast<std::size_t>(n_lu_) +
               static_cast<std::size_t>(lu);
    }
    std::size_t bidx(int c, int lu, int s) const noexcept {
        return (static_cast<std::size_t>(lu) * static_cast<std::size_t>(n_sp_) +
                static_cast<std::size_t>(s)) * static_cast<std::size_t>(n_cells_) +
               static_cast<std::size_t>(c);
    }
    struct Params {   // per (lu, s), read from the 1D tables at resolve
        int    bu_type = 0; double c0 = 0, c1 = 0, c2 = 0; int ts_idx = -1; int normalizer = 0; double max_days = 0;
        int    wo_type = 0; double coeff = 0, expon = 0, sweep_effic = 0, bmp_effic = 0;
    };
    const Params& param(int lu, int s) const noexcept {
        return params_[static_cast<std::size_t>(lu) * static_cast<std::size_t>(n_sp_) +
                       static_cast<std::size_t>(s)];
    }
    void loadParams(const SimulationContext& ctx);

    bool active_  = false;
    int  n_cells_ = 0, n_lu_ = 0, n_sp_ = 0;
    bool si_ = false;
    double area_to_landarea_ = 1.0;   ///< m² → acres (US) or ha (SI)
    std::vector<int>         row_of_;   ///< [s] transport row
    std::vector<std::string> names_;    ///< [s]
    std::vector<double>      mcf_;      ///< [s]
    std::vector<Params>      params_;   ///< [lu * n_sp + s]
    std::vector<double>      cov_;      ///< [c * n_lu + lu] fraction 0..1
    std::vector<double>      curb_;     ///< [c] project length units
    std::vector<double>      area_la_;  ///< [c] cell area in land-area units (acres | ha)
    std::vector<double>      init_;     ///< [c * n_sp + s] loading, user mass per area (0 = none)
    std::vector<double>      bu_;       ///< bidx(c, lu, s)
    std::vector<double>      last_swept_; ///< [c * n_lu + lu] days
    std::vector<double>      runoff_vol_; ///< [c] m³, cumulative
    std::vector<double>      led_init_, led_bu_, led_wo_, led_sw_, led_bmp_;   ///< [s] user mass
};

// ---- section parsers (exposed for tests) -----------------------------------
std::string parse2DCoveragesLine (const std::vector<std::string>& tokens, std::vector<CoverageRow2D>& rows);
std::string parse2DLoadingsLine  (const std::vector<std::string>& tokens, std::vector<LoadingRow2D>& rows);
std::string parse2DCurbLengthLine(const std::vector<std::string>& tokens, std::vector<CurbRow2D>& rows);

/// Register `[2D_COVERAGES]`, `[2D_LOADINGS]`, `[2D_CURB_LENGTH]` writing into @p sq.
void registerSurfaceQualitySections(SurfaceQuality2D& sq, input::SectionRegistry& registry);

/// Scope key ↔ file token ("*", "TAG name", "CELL n" — 1-based).
std::string formatSqScope(const SqScopeKey& k);
std::string parseSqScope(const std::vector<std::string>& tokens, std::size_t& at, SqScopeKey& key,
                         const char* section);

}  // namespace openswmm::twoD

#endif  // OPENSWMM_ENGINE_2D_QUALITY_SURFACE_QUALITY_2D_HPP
