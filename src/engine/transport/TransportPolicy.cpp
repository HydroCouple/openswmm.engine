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
 * @file      TransportPolicy.cpp
 * @brief     Domain × Species-class transport contract (see the header).
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "TransportPolicy.hpp"

#include "../core/SimulationContext.hpp"
#include "../2d/data/MeshData.hpp"
#include "../2d/data/SolverOptions2D.hpp"
#include "../2d/gw/GwTransportData.hpp"        // T7.1
#include "../2d/subsurface/SubsurfaceData.hpp" // T7.1

#include <cstdio>
#include <algorithm>

namespace openswmm::transport {

namespace {

/**
 * @brief Test whether the native reaction configuration is both configured and compiled.
 * @param[in] ctx Stable reaction state.
 * @return Conjunction of configured/compiled flags, without checking row extent/kinetic support.
 * @note Read-only, allocation-free; does not compile, execute reactions or advance time.
 */
bool reactionsActive(const SimulationContext& ctx) noexcept {
    return ctx.reactions.configured && ctx.reactions.compiled;
}

/**
 * @brief Test whether the prepared reaction species flags include any WALL species.
 * @param[in] ctx Stable species_is_wall vector, normally one flag per reaction species.
 * @return True if any stored byte nonzero, independent of configured/compiled state.
 * @note No allocation, model mutation or physics advance; caller applies domain support policy.
 */
bool reactionsHaveWall(const SimulationContext& ctx) noexcept {
    for (const auto w : ctx.reactions.species_is_wall)
        if (w != 0)
            return true;
    return false;
}

/**
 * @brief Test whether the borrowed authoring mesh exists and contains at least one cell.
 * @param[in] ctx Stable twod_io binding.
 * @return True for nonnull mesh with positive unified cell count, independent of solver
 * activation.
 * @note Read-only and allocation-free; no mesh validation or initialization.
 */
bool hasMesh2D(const SimulationContext& ctx) noexcept {
    return ctx.twod_io.mesh && ctx.twod_io.mesh->n_cells() > 0;
}

/**
 * @brief Borrow the context's optional surface solver authoring options.
 * @param[in] ctx Stable twod_io binding, retained alive for use of returned pointer.
 * @return Nullable nonowned options pointer; no validation or ownership transfer.
 * @note Pointer validity follows context/router lifecycle; allocation-free and read-only.
 */
const twoD::SolverOptions2D* opts2D(const SimulationContext& ctx) noexcept {
    return ctx.twod_io.options;
}

/**
 * @brief Mark one transport matrix cell enabled with a supplied class row count.
 * @param[in,out] c Cell receives ENABLED, count and cleared reason; previous value replaced.
 * @param[in] count Nonnegative eligible row count supplied by domain resolver, not checked here.
 * @note No physical state/clock change; caller exclusively owns result cell.
 */
void enabled(Cell& c, int count) {
    c.state = CellState::ENABLED;
    c.count = count;
    c.reason.clear();
}
/**
 * @brief Mark a transport matrix cell explicitly disabled by an authoring option.
 * @param[in,out] c Receives DISABLED_BY_USER, zero count and copied reason.
 * @param[in] key Nonnull option/key explanation string, copied during call.
 * @note String allocation may throw after state/count change; no physical/clock mutation.
 */
void disabled(Cell& c, const char* key) {
    c.state = CellState::DISABLED_BY_USER;
    c.count = 0;
    c.reason = key;
}
/**
 * @brief Mark a transport class unavailable for an absent model feature or unsupported solver.
 * @param[in,out] c Receives UNAVAILABLE, zero count and copied reason.
 * @param[in] why Nonnull reason string copied into cell.
 * @note Allocation can throw after state/count change; no physical owner/clock mutation.
 */
void unavailable(Cell& c, const char* why) {
    c.state = CellState::UNAVAILABLE;
    c.count = 0;
    c.reason = why;
}

} // namespace

// ---------------------------------------------------------------------------
// Enables (allocation-free)
// ---------------------------------------------------------------------------

/**
 * @brief Resolve allocation-free native network class counts from current project policy.
 * @param[in] ctx Stable prepared pollutant/reaction counts and process flags.
 * @return Pollutants/MSX only when !ignore_quality, MSX requires configured+compiled; age and
 * thermal flags also gated by !ignore_quality. Reports WALL presence even when quality disabled.
 * @note Does not apply actual solver-specific fallback, validate native layout or create rows.
 * Read-only/no allocation/physics/clock update; consistent reaction metadata required.
 */
ClassEnables network1DEnables(const SimulationContext& ctx) noexcept {
    ClassEnables e;
    const bool q = !ctx.options.ignore_quality;
    e.n_pollut = q ? ctx.n_pollutants() : 0;
    e.msx_has_wall = reactionsHaveWall(ctx);
    e.n_msx = (q && reactionsActive(ctx)) ? ctx.reactions.n_species() : 0;
    e.age = q && ctx.options.water_age;
    e.temperature = q && ctx.options.heat_transport;
    return e;
}

/**
 * @brief Resolve allocation-free surface native class eligibility from project and 2D opt-outs.
 * @param[in] ctx Stable process/reaction/pollutant metadata and optional borrowed 2D options.
 * @return Pollutants/MSX gated by !ignore_quality and respective domain enable; MSX requires
 * compiled reaction and no WALL species. Age/temperature use project process flags and domain
 * opt-outs, independent of ignore_quality. Null options mean domain classes inherit on.
 * @note Does not require a present/active mesh or initialize solver rows; read-only/no
 * allocation. No warnings, kinetic execution or clock mutation; runtime owner performs profile
 * qualification.
 */
ClassEnables surface2DEnables(const SimulationContext& ctx) noexcept {
    ClassEnables e;
    const auto* o = opts2D(ctx);
    const bool q = !ctx.options.ignore_quality;
    const bool tp = !o || o->transport_pollutants;
    const bool tm = !o || o->transport_msx;
    const bool ta = !o || o->transport_age;
    const bool tt = !o || o->transport_temperature;
    e.n_pollut = (q && tp) ? ctx.n_pollutants() : 0;
    e.msx_has_wall = reactionsHaveWall(ctx);
    // WALL species have no surface transport semantics: the 2D row block
    // refuses them (with its warning) exactly as before E2.
    e.n_msx = (q && tm && reactionsActive(ctx) && !e.msx_has_wall) ? ctx.reactions.n_species() : 0;
    e.age = ta && ctx.options.water_age;
    e.temperature = tt && ctx.options.heat_transport;
    return e;
}

/**
 * @brief Resolve allocation-free canonical aquifer class eligibility for an active integrated
 * kernel.
 * @param[in] ctx Stable twod_io aquifer state/options and complete native reaction/pollutant
 * metadata.
 * @return Empty enables without active positive-cell aquifer. Otherwise pollutants and BULK-only
 * compiled MSX obey !ignore_quality and GW opt-outs; age/temperature obey independent project
 * flags and GW enables. Sets msx_bulk_only, retains WALL-presence diagnostic and counts zero
 * wall flags.
 * @note Uses GW options directly, independently of surface opt-outs. No owner creation,
 * validation, reaction/physical advance or clock mutation; consistent metadata extents required.
 */
ClassEnables groundwaterEnables(const SimulationContext& ctx) noexcept {
    ClassEnables e;
    // The kernel is the gate: no resolved [2D_AQUIFER], no subsurface
    // transport, whatever the [GW_*] rows say.
    const auto* aq = ctx.twod_io.aquifer_state;
    if (aq == nullptr || !aq->active || aq->n_cells <= 0)
        return e;
    const auto* g = ctx.twod_io.gw;
    const bool q = !ctx.options.ignore_quality;
    const bool tp = !g || g->options.transport_pollutants;
    const bool tm = !g || g->options.transport_msx;
    const bool ta = !g || g->options.transport_age;
    const bool tt = !g || g->options.transport_temperature;
    e.n_pollut = (q && tp) ? ctx.n_pollutants() : 0;
    e.msx_has_wall = reactionsHaveWall(ctx);
    e.msx_bulk_only = true;
    e.n_msx = (q && tm && reactionsActive(ctx))
                  ? int(std::count(ctx.reactions.species_is_wall.begin(),
                                   ctx.reactions.species_is_wall.end(), uint8_t(0)))
                  : 0;
    e.age = ta && ctx.options.water_age;
    e.temperature = tt && ctx.options.heat_transport;
    return e;
}

/**
 * @brief Build an owned canonical pollutant/MSX/age/temperature row layout for supplied enables.
 * @param[in] ctx Stable pollutant and reaction names/flags covering supplied counts.
 * @param[in] e Nonnegative qualified class counts and optional age/temperature/BULK-only flags,
 * consistent with available native name/flag arrays.
 * @return Owned layout with counts, names and age/temp index (-1 when absent), order pollutants,
 * selected reaction species, __WATER_AGE__, __TEMPERATURE__. BULK-only skips WALL species.
 * @note Allocates/copies names and may throw; does not repair inconsistent counts or guarantee
 * names extent equals advertised ns for invalid enables. No registry/owner/physical/clock
 * mutation.
 */
RowLayout canonicalRows(const SimulationContext& ctx, const ClassEnables& e) {
    RowLayout L;
    L.n_pollut = e.n_pollut;
    L.n_msx = e.n_msx;
    L.age_row = e.age ? L.n_pollut + L.n_msx : -1;
    L.temp_row = e.temperature ? L.n_pollut + L.n_msx + (e.age ? 1 : 0) : -1;
    L.ns = e.total();
    L.names.reserve(static_cast<std::size_t>(L.ns));
    for (int p = 0; p < L.n_pollut; ++p)
        L.names.push_back(ctx.pollutant_names.name_of(p));
    for (int m = 0, added = 0; added < L.n_msx && m < ctx.reactions.n_species(); ++m) {
        if (e.msx_bulk_only && ctx.reactions.species_is_wall[m])
            continue;
        L.names.push_back(ctx.reactions.species_name[m]);
        ++added;
    }
    if (e.age)
        L.names.push_back("__WATER_AGE__");
    if (e.temperature)
        L.names.push_back("__TEMPERATURE__");
    return L;
}

// ---------------------------------------------------------------------------
// The matrix
// ---------------------------------------------------------------------------

/**
 * @brief Build a domain-by-species transport capability matrix with explicit reasons and counts.
 * @param[in] ctx Stable complete model/process, native reaction/surface-loading, optional 2D
 * mesh, aquifer and domain policy metadata. Read only, including warnings.
 * @param[in] draft Optional borrowed preview overrides for quality/age/heat/mesh/groundwater/2D
 * ignore and four surface class flags; null resolves current configuration. No pointer retained.
 * @return Owned 4x4matrix (runoff/LID, groundwater, network, surface) by
 * pollutant/MSX/age/temperature, with ENABLED counts, DISABLED_BY_USER key or UNAVAILABLE
 * reason. Preview does not qualify actual solver physics or promise successful initialization.
 * @note Integrated authored/active aquifer can carry groundwater native rows, legacy
 * per-subcatch seam does not. Runoff MSX requires active surface loading and reports no surface
 * reactions; surface WALL MSX unavailable, network WALL marks fallback advisory. Age/temperature
 * ignore-quality gating differs between network/runoff and integrated surface/GW. Checks feature
 * availability before domain opt-outs in defined branch order; no emitted warnings or model
 * mutation. Allocations may throw; requires stable serial/const-consistent model views, no
 * clock/physical advancement.
 */
Matrix resolve(const SimulationContext& ctx, const Draft* draft) {
    Matrix m;
    const int np = ctx.n_pollutants();
    const bool ignoreQ = draft ? draft->ignore_quality : ctx.options.ignore_quality;
    const bool msxOn = reactionsActive(ctx);
    const int nm = msxOn ? ctx.reactions.n_species() : 0;
    const bool msxWall = msxOn && reactionsHaveWall(ctx);
    const bool ageOn = draft ? draft->age : ctx.options.water_age;
    const bool heatOn = draft ? draft->heat : ctx.options.heat_transport;

    // ---- Runoff / LID ------------------------------------------------------
    {
        const bool has_sub = ctx.n_subcatches() > 0;
        Cell& pol = m.at(Domain::RUNOFF, SpeciesClass::POLLUTANTS);
        if (!has_sub)
            unavailable(pol, "no subcatchments");
        else if (np == 0)
            unavailable(pol, "no [POLLUTANTS]");
        else if (ignoreQ)
            disabled(pol, "IGNORE_QUALITY");
        else
            enabled(pol, np);

        // Multispecies Phase 2: species build up, wash off, rain in and
        // pass LID units through the shared surface kernel; no reaction
        // kinetics run on the land surface.
        Cell& msx = m.at(Domain::RUNOFF, SpeciesClass::MSX);
        if (!msxOn)
            unavailable(msx, "no reactions component");
        else if (!has_sub)
            unavailable(msx, "no subcatchments");
        else if (ignoreQ)
            disabled(msx, "IGNORE_QUALITY");
        else if (!ctx.reactions.surface.active())
            unavailable(msx, "no species buildup, washoff or rainfall rows");
        else {
            enabled(msx, nm);
            msx.reason = "loading and transport; no surface reactions";
        }

        Cell& age = m.at(Domain::RUNOFF, SpeciesClass::AGE);
        if (!has_sub)
            unavailable(age, "no subcatchments");
        else if (!ageOn)
            unavailable(age, "WATER_AGE OFF");
        else if (ignoreQ)
            disabled(age, "IGNORE_QUALITY");
        else
            enabled(age, 1);

        Cell& tmp = m.at(Domain::RUNOFF, SpeciesClass::TEMPERATURE);
        if (!has_sub)
            unavailable(tmp, "no subcatchments");
        else if (!heatOn)
            unavailable(tmp, "HEAT_TRANSPORT OFF");
        else if (ignoreQ)
            disabled(tmp, "IGNORE_QUALITY");
        else
            enabled(tmp, 1);
    }

    // ---- Groundwater --------------------------------------------------------
    // T7.1: the two-zone [2D_AQUIFER] kernel transports. A deck with only the
    // legacy subcatchment [AQUIFERS] keeps the old row verbatim — that
    // aquifer still has no transported quality of its own.
    {
        const auto* aq = ctx.twod_io.aquifer_state;
        const auto* options = opts2D(ctx);
        const bool authored = (!aq || aq->n_cells == 0) && hasMesh2D(ctx) && ctx.twod_io.aquifer &&
                              !ctx.twod_io.aquifer->empty() && !ctx.options.ignore_2d &&
                              (!options || options->groundwater != 0);
        const bool planned = draft || authored;
        const bool two_zone = draft ? draft->groundwater
                                    : authored || (aq != nullptr && aq->active && aq->n_cells > 0);
        if (two_zone) {
            const ClassEnables e = groundwaterEnables(ctx);
            const auto* g = ctx.twod_io.gw;
            Cell& pol = m.at(Domain::GROUNDWATER, SpeciesClass::POLLUTANTS);
            if (np == 0)
                unavailable(pol, "no [POLLUTANTS]");
            else if (ignoreQ)
                disabled(pol, "IGNORE_QUALITY");
            else if (g && !g->options.transport_pollutants)
                disabled(pol, "TRANSPORT_POLLUTANTS NO");
            else
                enabled(pol, planned ? np : e.n_pollut);

            Cell& msx = m.at(Domain::GROUNDWATER, SpeciesClass::MSX);
            if (!reactionsActive(ctx))
                unavailable(msx, "no reactions component");
            else if (std::count(ctx.reactions.species_is_wall.begin(),
                                ctx.reactions.species_is_wall.end(), uint8_t(0)) == 0)
                unavailable(msx, "no BULK species for subsurface transport");
            else if (ignoreQ)
                disabled(msx, "IGNORE_QUALITY");
            else if (g && !g->options.transport_msx)
                disabled(msx, "TRANSPORT_MSX NO");
            else
                enabled(msx, planned
                                 ? int(std::count(ctx.reactions.species_is_wall.begin(),
                                                  ctx.reactions.species_is_wall.end(), uint8_t(0)))
                                 : e.n_msx);

            Cell& age = m.at(Domain::GROUNDWATER, SpeciesClass::AGE);
            if (!ageOn)
                unavailable(age, "WATER_AGE OFF");
            else if (g && !g->options.transport_age)
                disabled(age, "TRANSPORT_AGE NO");
            else
                enabled(age, 1);

            Cell& tmp = m.at(Domain::GROUNDWATER, SpeciesClass::TEMPERATURE);
            if (!heatOn)
                unavailable(tmp, "HEAT_TRANSPORT OFF");
            else if (g && !g->options.transport_temperature)
                disabled(tmp, "TRANSPORT_TEMPERATURE NO");
            else
                enabled(tmp, 1);
        } else {
            const bool has_aq = ctx.aquifer_names.size() > 0;
            const char* no_aq = "no [AQUIFERS] or [2D_AQUIFER]";
            unavailable(m.at(Domain::GROUNDWATER, SpeciesClass::POLLUTANTS),
                        has_aq ? "no transported quality — the aquifer supplies the "
                                 "[POLLUTANTS] GW concentration at the node seam (legacy)"
                               : no_aq);
            unavailable(m.at(Domain::GROUNDWATER, SpeciesClass::MSX),
                        has_aq ? "no transported quality in groundwater" : no_aq);
            unavailable(m.at(Domain::GROUNDWATER, SpeciesClass::AGE),
                        has_aq ? "source volume only (new water enters at age 0)" : no_aq);
            unavailable(m.at(Domain::GROUNDWATER, SpeciesClass::TEMPERATURE),
                        has_aq ? "source volume only (GROUNDWATER source temperature)" : no_aq);
        }
    }

    // ---- 1D network --------------------------------------------------------
    {
        Cell& pol = m.at(Domain::NETWORK_1D, SpeciesClass::POLLUTANTS);
        if (np == 0)
            unavailable(pol, "no [POLLUTANTS]");
        else if (ignoreQ)
            disabled(pol, "IGNORE_QUALITY");
        else
            enabled(pol, np);

        Cell& msx = m.at(Domain::NETWORK_1D, SpeciesClass::MSX);
        if (!msxOn)
            unavailable(msx, "no reactions component");
        else if (ignoreQ)
            disabled(msx, "IGNORE_QUALITY");
        else
            enabled(msx, nm);
        if (msx.state == CellState::ENABLED && msxWall &&
            (draft || ctx.options.quality_solver == QualitySolverKind::EULERIAN_ARD))
            msx.reason = draft ? "WALL species: solver support confirmed at initialization"
                               : "WALL species: EULERIAN_ARD falls back to LEGACY";

        Cell& age = m.at(Domain::NETWORK_1D, SpeciesClass::AGE);
        if (!ageOn)
            unavailable(age, "WATER_AGE OFF");
        else if (ignoreQ)
            disabled(age, "IGNORE_QUALITY");
        else
            enabled(age, 1);

        Cell& tmp = m.at(Domain::NETWORK_1D, SpeciesClass::TEMPERATURE);
        if (!heatOn)
            unavailable(tmp, "HEAT_TRANSPORT OFF");
        else if (ignoreQ)
            disabled(tmp, "IGNORE_QUALITY");
        else
            enabled(tmp, 1);
    }

    // ---- 2D surface --------------------------------------------------------
    {
        const bool mesh = draft ? draft->mesh : hasMesh2D(ctx);
        const auto* o = opts2D(ctx);
        const bool ig2d = draft ? draft->ignore_2d : ctx.options.ignore_2d;
        auto gate = [&](Cell& c, bool project_on, const char* off_why, bool key_on, const char* key,
                        bool quality_gated, int count, const char* unavailable_why = nullptr) {
            if (!mesh) {
                unavailable(c, "no 2D mesh");
                return;
            }
            if (!project_on) {
                unavailable(c, off_why);
                return;
            }
            if (ig2d) {
                disabled(c, "IGNORE_2D");
                return;
            }
            if (quality_gated && ignoreQ) {
                disabled(c, "IGNORE_QUALITY");
                return;
            }
            if (unavailable_why) {
                unavailable(c, unavailable_why);
                return;
            }
            if (!key_on) {
                disabled(c, key);
                return;
            }
            enabled(c, count);
        };
        gate(m.at(Domain::SURFACE_2D, SpeciesClass::POLLUTANTS), np > 0, "no [POLLUTANTS]",
             draft ? draft->surface_class[0] : !o || o->transport_pollutants,
             "TRANSPORT_POLLUTANTS", true, np);
        gate(m.at(Domain::SURFACE_2D, SpeciesClass::MSX), msxOn, "no reactions component",
             draft ? draft->surface_class[1] : !o || o->transport_msx, "TRANSPORT_MSX", true, nm,
             msxWall ? "WALL species have no surface transport semantics" : nullptr);
        gate(m.at(Domain::SURFACE_2D, SpeciesClass::AGE), ageOn, "WATER_AGE OFF",
             draft ? draft->surface_class[2] : !o || o->transport_age, "TRANSPORT_AGE", false, 1);
        gate(m.at(Domain::SURFACE_2D, SpeciesClass::TEMPERATURE), heatOn, "HEAT_TRANSPORT OFF",
             draft ? draft->surface_class[3] : !o || o->transport_temperature,
             "TRANSPORT_TEMPERATURE", false, 1);
    }

    return m;
}

// ---------------------------------------------------------------------------
// Names and the report block
// ---------------------------------------------------------------------------

/**
 * @brief Format a transport matrix domain as a human-readable label.
 * @param[in] d Domain enum; COUNT/invalid values are permitted for fallback.
 * @return Static nonowned null-terminated label or '?' for unknown. No allocation/mutation.
 */
const char* domainName(Domain d) noexcept {
    switch (d) {
    case Domain::RUNOFF:
        return "Runoff / LID";
    case Domain::GROUNDWATER:
        return "Groundwater";
    case Domain::NETWORK_1D:
        return "1D network";
    case Domain::SURFACE_2D:
        return "2D surface";
    default:
        return "?";
    }
}

/**
 * @brief Format a transport species class as a human-readable label.
 * @param[in] c Class enum; COUNT/invalid values use fallback.
 * @return Static nonowned Pollutants/MSX/Age/Temperature literal or '?'. No mutation/allocation.
 */
const char* speciesClassName(SpeciesClass c) noexcept {
    switch (c) {
    case SpeciesClass::POLLUTANTS:
        return "Pollutants";
    case SpeciesClass::MSX:
        return "MSX";
    case SpeciesClass::AGE:
        return "Age";
    case SpeciesClass::TEMPERATURE:
        return "Temperature";
    default:
        return "?";
    }
}

/**
 * @brief Format a transport capability state as its canonical uppercase label.
 * @param[in] s State enum, invalid values permitted for fallback.
 * @return Static nonowned ENABLED/DISABLED_BY_USER/UNAVAILABLE literal or '?'. No
 * mutation/allocation.
 */
const char* cellStateName(CellState s) noexcept {
    switch (s) {
    case CellState::ENABLED:
        return "ENABLED";
    case CellState::DISABLED_BY_USER:
        return "DISABLED_BY_USER";
    case CellState::UNAVAILABLE:
        return "UNAVAILABLE";
    default:
        return "?";
    }
}

/**
 * @brief Format a capability matrix as a readable fixed-column report and unavailable
 * explanations.
 * @param[in] m Stable fully initialized4x4matrix; reason strings and counts already resolved.
 * @return Owned text ending newline, each enabled cell 'on(n)', disabled 'off:KEY', unavailable
 * 'n/a'; then one explanation per unavailable nonempty cell. Enabled advisory reasons are not
 * emitted here.
 * @note Fixed256-byte formatting buffer can truncate long individual rows/reasons; no
 * wrap/reflow or matrix repair. Allocates and may throw; no file I/O, warnings,
 * model/physics/clock changes.
 */
std::string formatReportBlock(const Matrix& m) {
    std::string out;
    out += "  Transport by domain (Domain x Species; on(n) = rows carried,\n";
    out += "  off:KEY = switched off by that option, n/a = not carried):\n";
    char buf[256];
    std::snprintf(buf, sizeof(buf), "  %-14s %-14s %-14s %-14s %-14s\n", "", "Pollutants", "MSX",
                  "Age", "Temperature");
    out += buf;
    for (int d = 0; d < static_cast<int>(Domain::COUNT); ++d) {
        std::string line = "  ";
        std::snprintf(buf, sizeof(buf), "%-14s ", domainName(static_cast<Domain>(d)));
        line += buf;
        for (int c = 0; c < static_cast<int>(SpeciesClass::COUNT); ++c) {
            const Cell& cell = m.cells[d][c];
            std::string tok;
            switch (cell.state) {
            case CellState::ENABLED:
                tok = "on(" + std::to_string(cell.count) + ")";
                break;
            case CellState::DISABLED_BY_USER:
                tok = "off:" + cell.reason;
                break;
            default:
                tok = "n/a";
                break;
            }
            std::snprintf(buf, sizeof(buf), "%-14s ", tok.c_str());
            line += buf;
        }
        out += line;
        out += '\n';
    }
    // The n/a explanations, once each, so the table stays narrow.
    bool any = false;
    for (int d = 0; d < static_cast<int>(Domain::COUNT); ++d) {
        for (int c = 0; c < static_cast<int>(SpeciesClass::COUNT); ++c) {
            const Cell& cell = m.cells[d][c];
            if (cell.state != CellState::UNAVAILABLE || cell.reason.empty())
                continue;
            if (!any) {
                out += "  n/a because:\n";
                any = true;
            }
            std::snprintf(buf, sizeof(buf), "    %-14s %-12s %s\n",
                          domainName(static_cast<Domain>(d)),
                          speciesClassName(static_cast<SpeciesClass>(c)), cell.reason.c_str());
            out += buf;
        }
    }
    return out;
}

} // namespace openswmm::transport
