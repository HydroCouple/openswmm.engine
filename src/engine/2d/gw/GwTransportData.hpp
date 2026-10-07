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
 * @file GwTransportData.hpp
 * @brief U4 (2026-09-07) — authoring state for subsurface (groundwater)
 *        transport: `[GW_TRANSPORT_OPTIONS]`, `[GW_TRANSPORT_PARAMS]`,
 *        `[GW_SORPTION]`, `[GW_INITIAL_QUALITY]`, `[GW_BOUNDARY_QUALITY]`
 *        and `[GW_SOURCES]`.
 *
 * @details **Authoring only in this release.** The two-zone integrated
 *          groundwater kernel (G1 of the two-zone plan) does not exist yet,
 *          so these rows are parsed, validated against the mesh and the
 *          species registry, held here, round-tripped by the writer and
 *          exposed through `openswmm_gw_transport.h` — and a run emits ONE
 *          warning saying the kernel is not available. That is the
 *          deliberate shape: a user can author, save, reopen and share a
 *          complete subsurface-transport model before the solver lands, and
 *          nothing about the file changes when it does.
 *
 *          **Cell-generic** (the tri/quad amendment): every scope is a CELL
 *          index or a TAG, never a "triangle"; an edge is `EDGE 0..nv-1` of
 *          its cell, validated against `MeshData::cell_nv`. No `/3`, no
 *          `* 3`, no `tri_` naming.
 *
 *          Resolution ladder, the same one `[2D_INFILTRATION]` and
 *          `[2D_INITIAL_QUALITY]` use: `*` < `TAG` < `CELL`, applied by
 *          ORDER (later rows win) rather than by comparison.
 *
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_GW_TRANSPORT_DATA_HPP
#define OPENSWMM_ENGINE_2D_GW_TRANSPORT_DATA_HPP

#include <cstdint>
#include <limits>
#include <unordered_map>
#include <string>
#include <vector>

namespace openswmm::twoD {

/// Which zone of the two-zone column a row addresses.
enum class GwZone : int8_t {
    SAT   = 0,   ///< saturated (lower) zone
    UNSAT = 1,   ///< unsaturated (upper) zone
    LAYER = 2    ///< an explicit layer index (`layer` field)
};

/// Scope of a per-cell row.
enum class GwScope : int8_t {
    GLOBAL = 0,  ///< the `*` row
    TAG    = 1,  ///< every cell carrying `tag`
    CELL   = 2   ///< one cell, by 0-based index (1-based in the file)
};

/// `[GW_TRANSPORT_OPTIONS]` — the subsurface column of the Domain × Species
/// matrix plus the process switches. Defaults reproduce "everything the
/// kernel would carry", so a deck that only names the section gets the
/// full model when the kernel arrives.
struct GwTransportOptions {
    bool transport_pollutants  = true;
    bool transport_msx         = true;
    bool transport_age         = true;
    bool transport_temperature = true;
    bool dispersion            = true;   ///< α_L / α_T / D_m active
    bool conduction            = true;   ///< vertical + lateral heat conduction
    /// SURFACE_THERMAL_BC kind and its argument: AIR | SURFACE_WATER |
    /// FIXED <degC> | TIMESERIES <name>.
    std::string surface_thermal_bc = "AIR";
    std::string surface_thermal_arg;
    /// DEEP_THERMAL_BC: GEOTHERMAL_FLUX <W/m2> | FIXED_TEMP <degC> |
    /// TIMESERIES <name>, with DEPTH <z_deep> in the `deep_depth` field.
    std::string deep_thermal_bc = "GEOTHERMAL_FLUX";
    std::string deep_thermal_arg;
    double      deep_depth = 0.0;
    /// THERMAL_MIXING ARITHMETIC | GEOMETRIC.
    std::string thermal_mixing = "ARITHMETIC";
    /// Explicit diffusion Courant number for the kernel's tier bound.
    double c_diff = 0.4;
    /// True once any [GW_*] section was seen — the run-time warning and the
    /// writer both key on it, so a deck with no subsurface authoring is
    /// byte-identical.
    bool authored = false;
};

/// One `[GW_TRANSPORT_PARAMS]` row: solute + thermal matrix properties.
struct GwParamsRow {
    GwScope     scope = GwScope::GLOBAL;
    std::string tag;              ///< TAG scope
    int         cell = -1;        ///< CELL scope (0-based)
    double rho_s    = 2650.0;     ///< grain density (kg/m3)
    double c_s      = 880.0;      ///< grain specific heat (J/kg/K)
    double lambda_s = 2.0;        ///< grain thermal conductivity (W/m/K)
    double a_s      = 0.0;        ///< specific surface area (m2/kg)
    double alpha_L  = 1.0;        ///< longitudinal dispersivity (m)
    double alpha_T  = 0.1;        ///< transverse dispersivity (m)
    double D_m      = 1.0e-9;     ///< molecular diffusivity (m2/s)
    double D_v      = 1.0e-9;     ///< vapour diffusivity (m2/s)
    double geo_flux = 0.065;      ///< geothermal flux (W/m2) or deep temp (degC)
};

/// One `[GW_SORPTION]` row: retardation input for one species in one scope.
struct GwSorptionRow {
    GwScope     scope = GwScope::GLOBAL;
    std::string tag;
    int         cell = -1;
    std::string species;
    double      kd    = 0.0;      ///< partition coefficient (L/kg)
    double      decay = -1.0;     ///< 1/day; < 0 = "use [POLLUTANTS] kdecay"
};

/// One `[GW_INITIAL_QUALITY]` row.
struct GwInitialQualityRow {
    GwScope     scope = GwScope::GLOBAL;
    std::string tag;
    int         cell  = -1;
    GwZone      zone  = GwZone::SAT;
    int         layer = -1;       ///< LAYER zone only
    std::string species;
    double      value = 0.0;
};

/// One `[GW_BOUNDARY_QUALITY]` row — a lateral edge BC.
struct GwBoundaryQualityRow {
    int         cell    = -1;     ///< 0-based cell index
    int         edge    = -1;     ///< local edge 0..nv-1 of that cell
    std::string species;
    /// CONC | TS | MASSFLUX | HEATFLUX.
    std::string kind    = "CONC";
    double      value   = 0.0;    ///< constant form
    std::string ts_name;          ///< TS form (kind == "TS", or a TS argument)
};

/// One `[GW_SOURCES]` species term.
struct GwSourceSpeciesTerm {
    std::string species;
    std::string kind = "CONC";    ///< CONC | MASS
    double      value = 0.0;
    std::string ts_name;
};

/// One `[GW_SOURCES]` row — a well / point source or sink.
struct GwSourceRow {
    std::string name;
    GwScope     scope = GwScope::CELL;   ///< CELL | TAG | (XY resolves to CELL)
    std::string tag;
    int         cell = -1;
    double      x = 0.0, y = 0.0;        ///< XY form (retained for the writer)
    bool        by_xy = false;
    double      flow = 0.0;              ///< m3/s, + inject / − extract
    std::string flow_ts;                 ///< when the FLOW argument is a series
    double scale = 1.0;                  ///< source-total multiplier, also applied to MASS (not CONC)
    std::vector<GwSourceSpeciesTerm> species;
};

/**
 * @brief Every `[GW_*]` authoring row of one model.
 *
 * @details Owned by SurfaceRouter2D (the 2D module's data home) and reached
 *          through `SimulationContext::twod_io.gw` so the writer, the C API
 *          and the GeoPackage path can read it without depending on the
 *          router.
 */
struct GwTransportData {
    GwTransportOptions               options;
    std::vector<GwParamsRow>         params;
    std::vector<GwSorptionRow>       sorption;
    std::vector<GwInitialQualityRow> initial_quality;
    /// `[GW_INITIAL_QUALITY] FILE <csv>` (element,zone,species,value).
    std::string                      initial_quality_file;
    std::vector<GwBoundaryQualityRow> boundary_quality;
    std::vector<GwSourceRow>         sources;

    bool empty() const noexcept {
        return !options.authored && params.empty() && sorption.empty() &&
               initial_quality.empty() && initial_quality_file.empty() &&
               boundary_quality.empty() && sources.empty();
    }

    void clear() { *this = GwTransportData{}; }

    // Derived authoring indexes contain positions, never vector pointers. Direct
    // key edits or bulk replacement must invalidate them; parser and FILE reload
    // do so explicitly. Size checks also catch ordinary direct append/erase.
    void invalidateAuthoringIndexes() noexcept {
        source_index_size_ = quality_index_size_ = noRow;
    }
    static constexpr std::size_t noRow = std::numeric_limits<std::size_t>::max();

    std::size_t findSource(const std::string& name) {
        if (source_index_size_ != sources.size()) {
            source_index_size_ = noRow;
            source_index_.clear();
            source_index_.reserve(sources.size());
            for (std::size_t i = 0; i < sources.size(); ++i)
                source_index_.emplace(sources[i].name, i); // first match wins
            source_index_size_ = sources.size();
        }
        const auto it = source_index_.find(name);
        return it == source_index_.end() ? noRow : it->second;
    }
    std::size_t findInitialQuality(const GwInitialQualityRow& row) {
        if (quality_index_size_ != initial_quality.size()) {
            quality_index_size_ = noRow;
            quality_index_.clear();
            quality_index_.reserve(initial_quality.size());
            for (std::size_t i = 0; i < initial_quality.size(); ++i)
                quality_index_.emplace(QualityKey(initial_quality[i]), i);
            quality_index_size_ = initial_quality.size();
        }
        const auto it = quality_index_.find(QualityKey(row));
        return it == quality_index_.end() ? noRow : it->second;
    }
    void sourceAppended() {
        if (source_index_size_ != noRow && source_index_size_ + 1 == sources.size()) {
            source_index_.emplace(sources.back().name, sources.size() - 1);
            source_index_size_ = sources.size();
        }
    }
    void initialQualityAppended() {
        if (quality_index_size_ != noRow && quality_index_size_ + 1 == initial_quality.size()) {
            quality_index_.emplace(QualityKey(initial_quality.back()), initial_quality.size() - 1);
            quality_index_size_ = initial_quality.size();
        }
    }
    void removeSource(std::size_t index) {
        if (source_index_size_ == sources.size() && index + 1 == sources.size()) {
            const auto it = source_index_.find(sources[index].name);
            if (it != source_index_.end() && it->second == index) source_index_.erase(it);
            --source_index_size_;
        } else source_index_size_ = noRow;
        sources.erase(sources.begin() + index);
    }
    void removeInitialQuality(std::size_t index) {
        if (quality_index_size_ == initial_quality.size() && index + 1 == initial_quality.size()) {
            const auto it = quality_index_.find(QualityKey(initial_quality[index]));
            if (it != quality_index_.end() && it->second == index) quality_index_.erase(it);
            --quality_index_size_;
        } else quality_index_size_ = noRow;
        initial_quality.erase(initial_quality.begin() + index);
    }

private:
    struct QualityKey {
        GwScope scope;
        std::string tag;
        int cell;
        GwZone zone;
        int layer;
        std::string species;
        explicit QualityKey(const GwInitialQualityRow& r)
            : scope(r.scope), tag(r.tag), cell(r.cell), zone(r.zone), layer(r.layer), species(r.species) {}
        bool operator==(const QualityKey& r) const noexcept {
            return scope == r.scope && tag == r.tag && cell == r.cell &&
                   zone == r.zone && layer == r.layer && species == r.species;
        }
    };
    struct QualityHash {
        std::size_t operator()(const QualityKey& r) const noexcept {
            std::size_t h = 0;
            const auto combine = [&h](std::size_t v) { h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2); };
            combine(std::hash<int>{}(static_cast<int>(r.scope)));
            combine(std::hash<std::string>{}(r.tag));
            combine(std::hash<int>{}(r.cell));
            combine(std::hash<int>{}(static_cast<int>(r.zone)));
            combine(std::hash<int>{}(r.layer));
            combine(std::hash<std::string>{}(r.species));
            return h;
        }
    };
    std::unordered_map<std::string, std::size_t> source_index_;
    std::unordered_map<QualityKey, std::size_t, QualityHash> quality_index_;
    std::size_t source_index_size_ = noRow;
    std::size_t quality_index_size_ = noRow;
};

const char* gwZoneToken(GwZone z) noexcept;
bool        parseGwZone(const std::string& token, GwZone& z) noexcept;
const char* gwScopeToken(GwScope s) noexcept;

}  // namespace openswmm::twoD

#endif  // OPENSWMM_ENGINE_2D_GW_TRANSPORT_DATA_HPP
