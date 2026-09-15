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
 * @file TransportPolicy.cpp
 * @brief Domain × Species-class transport contract (see the header).
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "TransportPolicy.hpp"

#include "../core/SimulationContext.hpp"
#include "../2d/data/MeshData.hpp"
#include "../2d/data/SolverOptions2D.hpp"

#include <cstdio>

namespace openswmm::transport {

namespace {

bool reactionsActive(const SimulationContext& ctx) noexcept {
    return ctx.reactions.configured && ctx.reactions.compiled;
}

bool reactionsHaveWall(const SimulationContext& ctx) noexcept {
    for (const auto w : ctx.reactions.species_is_wall)
        if (w != 0) return true;
    return false;
}

bool hasMesh2D(const SimulationContext& ctx) noexcept {
    return ctx.twod_io.mesh && ctx.twod_io.mesh->n_cells() > 0;
}

const twoD::SolverOptions2D* opts2D(const SimulationContext& ctx) noexcept {
    return ctx.twod_io.options;
}

void enabled(Cell& c, int count) {
    c.state = CellState::ENABLED;
    c.count = count;
    c.reason.clear();
}
void disabled(Cell& c, const char* key) {
    c.state = CellState::DISABLED_BY_USER;
    c.count = 0;
    c.reason = key;
}
void unavailable(Cell& c, const char* why) {
    c.state = CellState::UNAVAILABLE;
    c.count = 0;
    c.reason = why;
}

}  // namespace

// ---------------------------------------------------------------------------
// Enables (allocation-free)
// ---------------------------------------------------------------------------

ClassEnables network1DEnables(const SimulationContext& ctx) noexcept {
    ClassEnables e;
    const bool q = !ctx.options.ignore_quality;
    e.n_pollut     = q ? ctx.n_pollutants() : 0;
    e.msx_has_wall = reactionsHaveWall(ctx);
    e.n_msx        = (q && reactionsActive(ctx)) ? ctx.reactions.n_species() : 0;
    e.age          = q && ctx.options.water_age;
    e.temperature  = q && ctx.options.heat_transport;
    return e;
}

ClassEnables surface2DEnables(const SimulationContext& ctx) noexcept {
    ClassEnables e;
    const auto* o = opts2D(ctx);
    const bool q = !ctx.options.ignore_quality;
    const bool tp = !o || o->transport_pollutants;
    const bool tm = !o || o->transport_msx;
    const bool ta = !o || o->transport_age;
    const bool tt = !o || o->transport_temperature;
    e.n_pollut     = (q && tp) ? ctx.n_pollutants() : 0;
    e.msx_has_wall = reactionsHaveWall(ctx);
    // WALL species have no surface transport semantics: the 2D row block
    // refuses them (with its warning) exactly as before E2.
    e.n_msx        = (q && tm && reactionsActive(ctx) && !e.msx_has_wall)
                         ? ctx.reactions.n_species() : 0;
    e.age          = ta && ctx.options.water_age;
    e.temperature  = tt && ctx.options.heat_transport;
    return e;
}

RowLayout canonicalRows(const SimulationContext& ctx, const ClassEnables& e) {
    RowLayout L;
    L.n_pollut = e.n_pollut;
    L.n_msx    = e.n_msx;
    L.age_row  = e.age ? L.n_pollut + L.n_msx : -1;
    L.temp_row = e.temperature ? L.n_pollut + L.n_msx + (e.age ? 1 : 0) : -1;
    L.ns       = e.total();
    L.names.reserve(static_cast<std::size_t>(L.ns));
    for (int p = 0; p < L.n_pollut; ++p) L.names.push_back(ctx.pollutant_names.name_of(p));
    for (int m = 0; m < L.n_msx; ++m)
        L.names.push_back(ctx.reactions.species_name[static_cast<std::size_t>(m)]);
    if (e.age)         L.names.push_back("__WATER_AGE__");
    if (e.temperature) L.names.push_back("__TEMPERATURE__");
    return L;
}

// ---------------------------------------------------------------------------
// The matrix
// ---------------------------------------------------------------------------

Matrix resolve(const SimulationContext& ctx) {
    Matrix m;
    const int  np       = ctx.n_pollutants();
    const bool ignoreQ  = ctx.options.ignore_quality;
    const bool msxOn    = reactionsActive(ctx);
    const int  nm       = msxOn ? ctx.reactions.n_species() : 0;
    const bool msxWall  = msxOn && reactionsHaveWall(ctx);
    const bool ageOn    = ctx.options.water_age;
    const bool heatOn   = ctx.options.heat_transport;

    // ---- Runoff / LID ------------------------------------------------------
    {
        const bool has_sub = ctx.n_subcatches() > 0;
        Cell& pol = m.at(Domain::RUNOFF, SpeciesClass::POLLUTANTS);
        if (!has_sub)      unavailable(pol, "no subcatchments");
        else if (np == 0)  unavailable(pol, "no [POLLUTANTS]");
        else if (ignoreQ)  disabled(pol, "IGNORE_QUALITY");
        else               enabled(pol, np);

        unavailable(m.at(Domain::RUNOFF, SpeciesClass::MSX),
                    msxOn ? "runoff and LID have no reactions binding"
                          : "no reactions component");

        Cell& age = m.at(Domain::RUNOFF, SpeciesClass::AGE);
        if (!has_sub)      unavailable(age, "no subcatchments");
        else if (!ageOn)   unavailable(age, "WATER_AGE OFF");
        else if (ignoreQ)  disabled(age, "IGNORE_QUALITY");
        else               enabled(age, 1);

        Cell& tmp = m.at(Domain::RUNOFF, SpeciesClass::TEMPERATURE);
        if (!has_sub)      unavailable(tmp, "no subcatchments");
        else if (!heatOn)  unavailable(tmp, "HEAT_TRANSPORT OFF");
        else if (ignoreQ)  disabled(tmp, "IGNORE_QUALITY");
        else               enabled(tmp, 1);
    }

    // ---- Groundwater (legacy aquifers) -------------------------------------
    {
        const bool has_aq = ctx.aquifer_names.size() > 0;
        const char* no_aq = "no [AQUIFERS]";
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

    // ---- 1D network --------------------------------------------------------
    {
        Cell& pol = m.at(Domain::NETWORK_1D, SpeciesClass::POLLUTANTS);
        if (np == 0)       unavailable(pol, "no [POLLUTANTS]");
        else if (ignoreQ)  disabled(pol, "IGNORE_QUALITY");
        else               enabled(pol, np);

        Cell& msx = m.at(Domain::NETWORK_1D, SpeciesClass::MSX);
        if (!msxOn)        unavailable(msx, "no reactions component");
        else if (ignoreQ)  disabled(msx, "IGNORE_QUALITY");
        else               enabled(msx, nm);
        if (msx.state == CellState::ENABLED && msxWall &&
            ctx.options.quality_solver == QualitySolverKind::EULERIAN_ARD)
            msx.reason = "WALL species: EULERIAN_ARD falls back to LEGACY";

        Cell& age = m.at(Domain::NETWORK_1D, SpeciesClass::AGE);
        if (!ageOn)        unavailable(age, "WATER_AGE OFF");
        else if (ignoreQ)  disabled(age, "IGNORE_QUALITY");
        else               enabled(age, 1);

        Cell& tmp = m.at(Domain::NETWORK_1D, SpeciesClass::TEMPERATURE);
        if (!heatOn)       unavailable(tmp, "HEAT_TRANSPORT OFF");
        else if (ignoreQ)  disabled(tmp, "IGNORE_QUALITY");
        else               enabled(tmp, 1);
    }

    // ---- 2D surface --------------------------------------------------------
    {
        const bool mesh = hasMesh2D(ctx);
        const auto* o   = opts2D(ctx);
        const bool ig2d = ctx.options.ignore_2d;
        auto gate = [&](Cell& c, bool project_on, const char* off_why,
                        bool key_on, const char* key, bool quality_gated,
                        int count, const char* unavailable_why = nullptr) {
            if (!mesh)                     { unavailable(c, "no 2D mesh"); return; }
            if (!project_on)               { unavailable(c, off_why); return; }
            if (ig2d)                      { disabled(c, "IGNORE_2D"); return; }
            if (quality_gated && ignoreQ)  { disabled(c, "IGNORE_QUALITY"); return; }
            if (unavailable_why)           { unavailable(c, unavailable_why); return; }
            if (!key_on)                   { disabled(c, key); return; }
            enabled(c, count);
        };
        gate(m.at(Domain::SURFACE_2D, SpeciesClass::POLLUTANTS),
             np > 0, "no [POLLUTANTS]",
             !o || o->transport_pollutants, "TRANSPORT_POLLUTANTS", true, np);
        gate(m.at(Domain::SURFACE_2D, SpeciesClass::MSX),
             msxOn, "no reactions component",
             !o || o->transport_msx, "TRANSPORT_MSX", true, nm,
             msxWall ? "WALL species have no surface transport semantics" : nullptr);
        gate(m.at(Domain::SURFACE_2D, SpeciesClass::AGE),
             ageOn, "WATER_AGE OFF",
             !o || o->transport_age, "TRANSPORT_AGE", false, 1);
        gate(m.at(Domain::SURFACE_2D, SpeciesClass::TEMPERATURE),
             heatOn, "HEAT_TRANSPORT OFF",
             !o || o->transport_temperature, "TRANSPORT_TEMPERATURE", false, 1);
    }

    return m;
}

// ---------------------------------------------------------------------------
// Names and the report block
// ---------------------------------------------------------------------------

const char* domainName(Domain d) noexcept {
    switch (d) {
        case Domain::RUNOFF:      return "Runoff / LID";
        case Domain::GROUNDWATER: return "Groundwater";
        case Domain::NETWORK_1D:  return "1D network";
        case Domain::SURFACE_2D:  return "2D surface";
        default:                  return "?";
    }
}

const char* speciesClassName(SpeciesClass c) noexcept {
    switch (c) {
        case SpeciesClass::POLLUTANTS:  return "Pollutants";
        case SpeciesClass::MSX:         return "MSX";
        case SpeciesClass::AGE:         return "Age";
        case SpeciesClass::TEMPERATURE: return "Temperature";
        default:                        return "?";
    }
}

const char* cellStateName(CellState s) noexcept {
    switch (s) {
        case CellState::ENABLED:          return "ENABLED";
        case CellState::DISABLED_BY_USER: return "DISABLED_BY_USER";
        case CellState::UNAVAILABLE:      return "UNAVAILABLE";
        default:                          return "?";
    }
}

std::string formatReportBlock(const Matrix& m) {
    std::string out;
    out += "  Transport by domain (Domain x Species; on(n) = rows carried,\n";
    out += "  off:KEY = switched off by that option, n/a = not carried):\n";
    char buf[256];
    std::snprintf(buf, sizeof(buf), "  %-14s %-14s %-14s %-14s %-14s\n",
                  "", "Pollutants", "MSX", "Age", "Temperature");
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
            if (cell.state != CellState::UNAVAILABLE || cell.reason.empty()) continue;
            if (!any) { out += "  n/a because:\n"; any = true; }
            std::snprintf(buf, sizeof(buf), "    %-14s %-12s %s\n",
                          domainName(static_cast<Domain>(d)),
                          speciesClassName(static_cast<SpeciesClass>(c)),
                          cell.reason.c_str());
            out += buf;
        }
    }
    return out;
}

}  // namespace openswmm::transport
