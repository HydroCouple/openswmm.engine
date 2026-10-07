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
 * @file SurfaceQuality2D.cpp
 * @brief S7 — see SurfaceQuality2D.hpp. The buildup / washoff / sweeping
 *        formulas are the subcatchment step's (SWMMEngine::stepSurfaceQuality,
 *        the A7 sweeping block, initialize 11b — and their BW-MSX
 *        transcription in quality/MsxSurfaceQuality.cpp), written in the 2D
 *        module's SI: cell area m², runoff in m/s, cell rows in
 *        concentration × m³. The land-use functions themselves are unit-free
 *        in "days" and "user mass per normalizer unit", so buildup on a cell
 *        with no flow is bit-identical to buildup on a subcatchment (gate 1).
 */

#include "SurfaceQuality2D.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <sstream>

#include "../data/MeshData.hpp"
#include "../data/SolverOptions2D.hpp"
#include "../data/SurfaceStateData.hpp"
#include "../data/SurfaceTransportState.hpp"
#include "../../core/DateTime.hpp"
#include "../../core/SimulationContext.hpp"
#include "../../core/UnitConversion.hpp"
#include "../../input/InputParseUtils.hpp"
#include "../../input/Tokenizer.hpp"
#include "../../quality/MsxSurfaceQuality.hpp"

namespace openswmm::twoD {

namespace {

constexpr double kSqMPerAcre  = 4046.8564224;
constexpr double kSqMPerHa    = 10000.0;
constexpr double kM3PerFt3    = 0.028316846592;
constexpr double kMinRunoffMs = 1.0e-9 * 0.3048;   // the 1D ft/s threshold, in m/s

bool iequals(const std::string& a, const char* b) {
    std::size_t i = 0;
    for (; i < a.size() && b[i]; ++i)
        if (std::toupper(static_cast<unsigned char>(a[i])) !=
            std::toupper(static_cast<unsigned char>(b[i]))) return false;
    return i == a.size() && b[i] == '\0';
}
bool num(const std::string& s, double& v) {
    try { std::size_t n = 0; v = std::stod(s, &n); return n == s.size(); }
    catch (...) { return false; }
}

double buildupAt(int type, double c0, double c1, double c2, double days) {
    if (!(days > 0.0)) return 0.0;
    switch (type) {
        case 1: return std::min(c1 * std::pow(days, c2), c0);              // POW
        case 2: return c0 * (1.0 - std::exp(-c1 * days));                  // EXP
        case 3: return (c2 + days > 0.0) ? c0 * days / (c2 + days) : 0.0;  // SAT
        default: return 0.0;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Scope tokens and line parsers
// ---------------------------------------------------------------------------

std::string formatSqScope(const SqScopeKey& k) {
    switch (k.scope) {
        case SqScope::TAG:  return "TAG " + k.tag;
        case SqScope::CELL: return "CELL " + std::to_string(k.cell + 1);
        default:            return "*";
    }
}

std::string parseSqScope(const std::vector<std::string>& tokens, std::size_t& at,
                         SqScopeKey& key, const char* section) {
    if (tokens.empty()) return std::string(section) + " empty row";
    if (tokens[0] == "*") { key.scope = SqScope::GLOBAL; at = 1; return {}; }
    if (iequals(tokens[0], "TAG")) {
        if (tokens.size() < 2) return std::string(section) + " TAG needs a name";
        key.scope = SqScope::TAG; key.tag = tokens[1]; at = 2; return {};
    }
    if (iequals(tokens[0], "CELL")) {
        if (tokens.size() < 2) return std::string(section) + " CELL needs an index";
        double d = 0.0;
        if (!num(tokens[1], d) || d < 1.0 || d != std::floor(d))
            return std::string(section) + " invalid CELL index (1-based): " + tokens[1];
        key.scope = SqScope::CELL; key.cell = static_cast<int>(d) - 1; at = 2; return {};
    }
    return std::string(section) + " row must start with CELL, TAG or '*': " + tokens[0];
}

std::string parse2DCoveragesLine(const std::vector<std::string>& tokens,
                                 std::vector<CoverageRow2D>& rows) {
    CoverageRow2D r;
    std::size_t at = 0;
    std::string err = parseSqScope(tokens, at, r.key, "[2D_COVERAGES]");
    if (!err.empty()) return err;
    if (at >= tokens.size() || (tokens.size() - at) % 2 != 0)
        return "[2D_COVERAGES] expects LANDUSE PERCENT pairs after the scope";
    double sum = 0.0;
    for (; at + 1 < tokens.size(); at += 2) {
        double pct = 0.0;
        if (!num(tokens[at + 1], pct) || pct < 0.0)
            return "[2D_COVERAGES] invalid percent for '" + tokens[at] + "': " + tokens[at + 1];
        sum += pct;
        r.uses.emplace_back(tokens[at], pct);
    }
    if (sum > 100.0 + 1e-9)
        return "[2D_COVERAGES] percents for scope '" + formatSqScope(r.key) +
               "' sum to " + std::to_string(sum) + " (> 100)";
    // A row REPLACES the set for its scope (the subcatchment [COVERAGES]
    // semantics): drop an earlier row with the same key.
    rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const CoverageRow2D& o) {
        return o.key.scope == r.key.scope && o.key.tag == r.key.tag && o.key.cell == r.key.cell;
    }), rows.end());
    rows.push_back(std::move(r));
    return {};
}

std::string parse2DLoadingsLine(const std::vector<std::string>& tokens,
                                std::vector<LoadingRow2D>& rows) {
    LoadingRow2D r;
    std::size_t at = 0;
    std::string err = parseSqScope(tokens, at, r.key, "[2D_LOADINGS]");
    if (!err.empty()) return err;
    if (at + 2 != tokens.size()) return "[2D_LOADINGS] expects SPECIES VALUE after the scope";
    r.species = tokens[at];
    if (!num(tokens[at + 1], r.value) || r.value < 0.0)
        return "[2D_LOADINGS] invalid value for '" + r.species + "': " + tokens[at + 1];
    rows.push_back(std::move(r));
    return {};
}

std::string parse2DCurbLengthLine(const std::vector<std::string>& tokens,
                                  std::vector<CurbRow2D>& rows) {
    CurbRow2D r;
    std::size_t at = 0;
    std::string err = parseSqScope(tokens, at, r.key, "[2D_CURB_LENGTH]");
    if (!err.empty()) return err;
    if (at + 1 != tokens.size()) return "[2D_CURB_LENGTH] expects LENGTH after the scope";
    if (!num(tokens[at], r.length) || r.length < 0.0)
        return "[2D_CURB_LENGTH] invalid length: " + tokens[at];
    rows.push_back(std::move(r));
    return {};
}

void registerSurfaceQualitySections(SurfaceQuality2D& sq, input::SectionRegistry& registry) {
    auto make = [&sq](const char* tag, auto parser, auto member) {
        return [tag, parser, member, &sq](SimulationContext& ctx,
                                          const std::vector<std::string>& lines) {
            for (const auto& raw : lines) {
                auto tokens = input::Tokenizer::tokenize(raw);
                if (tokens.empty()) continue;
                const std::string err = parser(tokens, sq.*member);
                if (!err.empty()) {
                    ctx.error_code    = 5;   // SWMM_ERR_PARSE
                    ctx.error_message = std::string("[2D] ") + err + " — line: " + raw;
                    return;
                }
            }
            (void)tag;
        };
    };
    registry.register_custom("2D_COVERAGES",
        make("2D_COVERAGES", &parse2DCoveragesLine, &SurfaceQuality2D::coverage_rows));
    registry.register_custom("2D_LOADINGS",
        make("2D_LOADINGS", &parse2DLoadingsLine, &SurfaceQuality2D::loading_rows));
    registry.register_custom("2D_CURB_LENGTH",
        make("2D_CURB_LENGTH", &parse2DCurbLengthLine, &SurfaceQuality2D::curb_rows));
}

// ---------------------------------------------------------------------------
// Resolve
// ---------------------------------------------------------------------------

void SurfaceQuality2D::loadParams(const SimulationContext& ctx) {
    const int np = ctx.n_pollutants();
    params_.assign(static_cast<std::size_t>(n_lu_) * static_cast<std::size_t>(n_sp_), Params{});
    for (int lu = 0; lu < n_lu_; ++lu) {
        for (int s = 0; s < n_sp_; ++s) {
            Params& P = params_[static_cast<std::size_t>(lu) * static_cast<std::size_t>(n_sp_) +
                                static_cast<std::size_t>(s)];
            const int row = row_of_[static_cast<std::size_t>(s)];
            if (row < np) {                                    // pollutant tables
                const auto k = static_cast<std::size_t>(lu * np + row);
                if (ctx.buildup.n_pollutants == np && k < ctx.buildup.func_type.size()) {
                    P.bu_type = ctx.buildup.func_type[k];
                    P.c0 = ctx.buildup.coeff1[k]; P.c1 = ctx.buildup.coeff2[k]; P.c2 = ctx.buildup.coeff3[k];
                    if (P.bu_type == 4) P.ts_idx = static_cast<int>(ctx.buildup.coeff3[k]);
                    P.normalizer = ctx.buildup.normalizer[k];
                }
                if (ctx.washoff.n_pollutants == np && k < ctx.washoff.func_type.size()) {
                    P.wo_type = ctx.washoff.func_type[k];
                    P.coeff = ctx.washoff.coeff[k]; P.expon = ctx.washoff.expon[k];
                    P.sweep_effic = ctx.washoff.sweep_effic[k]; P.bmp_effic = ctx.washoff.bmp_effic[k];
                }
            } else {                                           // BW-MSX tables
                const auto& ms = ctx.reactions.surface;
                const int m = row - np;
                if (ms.n_landuses == n_lu_ && m >= 0 && m < ms.n_species) {
                    const auto k = ms.pidx(lu, m);
                    P.bu_type = ms.bu_type[k];
                    P.c0 = ms.bu_c1[k]; P.c1 = ms.bu_c2[k]; P.c2 = ms.bu_c3[k];
                    if (P.bu_type == 4) P.ts_idx = static_cast<int>(ms.bu_c3[k]);
                    P.normalizer = ms.bu_normalizer[k];
                    P.wo_type = ms.wo_type[k];
                    P.coeff = ms.wo_coeff[k]; P.expon = ms.wo_expon[k];
                    P.sweep_effic = ms.wo_sweep_effic[k]; P.bmp_effic = ms.wo_bmp_effic[k];
                }
            }
            // refreshLanduseParams(): time to reach 99.9 % of the maximum
            if (P.bu_type == 2 && P.c1 > 0.0)                               P.max_days = -std::log(0.001) / P.c1;
            else if (P.bu_type == 1 && P.c1 > 0.0 && P.c2 > 0.0 && P.c0 > 0.0) P.max_days = std::pow(P.c0 / P.c1, 1.0 / P.c2);
            else if (P.bu_type == 3 && P.c2 > 0.0 && P.c0 > 0.0)             P.max_days = 999.0 * P.c2;
        }
    }
}

std::vector<std::string> SurfaceQuality2D::resolve(const SimulationContext& ctx,
                                                   const MeshData& mesh,
                                                   const SurfaceTransportState& tr,
                                                   const SolverOptions2D& opts) {
    std::vector<std::string> out;
    active_ = false;
    if (!authored()) return out;
    if (coverage_rows.empty()) {
        out.push_back("[2D_LOADINGS] / [2D_CURB_LENGTH] rows are present but no "
                      "[2D_COVERAGES] names a land use on the mesh; nothing builds up.");
        return out;
    }

    n_cells_ = mesh.n_triangles();
    n_lu_    = ctx.n_landuses();
    if (n_lu_ <= 0) { out.push_back("ERROR [2D_COVERAGES]: the model defines no [LANDUSES]."); return out; }
    if (!tr.active()) {
        out.push_back("ERROR [2D_COVERAGES]: the model carries no transported species "
                      "([POLLUTANTS] empty or IGNORE_QUALITY set), so nothing could wash off.");
        return out;
    }
    si_ = (ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units)) == 1);
    area_to_landarea_ = si_ ? 1.0 / kSqMPerHa : 1.0 / kSqMPerAcre;

    // ---- surface species: the pollutant and MSX rows of the transport layout
    const int np = tr.n_pollut, nm = tr.n_msx;
    n_sp_ = np + nm;
    row_of_.clear(); names_.clear(); mcf_.clear();
    const double mass_ucf = ucf::UCF(ucf::MASS, ctx.options);
    for (int s = 0; s < n_sp_; ++s) {
        row_of_.push_back(s);                         // rows [0, np+nm) are exactly these
        names_.push_back(static_cast<std::size_t>(s) < tr.row_names.size() ? tr.row_names[static_cast<std::size_t>(s)] : std::string{});
        double f = mass_ucf;
        if (s < np) {
            const auto up = static_cast<std::size_t>(s);
            if (up < ctx.pollutants.units.size()) {
                if (ctx.pollutants.units[up] == MassUnits::UG_PER_L)     f = mass_ucf / 1000.0;
                if (ctx.pollutants.units[up] == MassUnits::COUNTS_PER_L) f = 1.0;
            }
        } else {
            const auto um = static_cast<std::size_t>(s - np);
            f = msxsurf::speciesUnitFactor(
                um < ctx.reactions.species_units.size() ? ctx.reactions.species_units[um] : std::string{},
                ctx.options);
        }
        mcf_.push_back(f);
    }
    loadParams(ctx);

    // ---- coverages: GLOBAL < TAG < CELL, a row replaces the set -----------
    cov_.assign(static_cast<std::size_t>(n_cells_) * static_cast<std::size_t>(n_lu_), 0.0);
    std::vector<int8_t> covered(static_cast<std::size_t>(n_cells_), 0);
    auto apply = [&](const CoverageRow2D& r, int c) {
        for (int lu = 0; lu < n_lu_; ++lu) cov_[cidx(c, lu)] = 0.0;
        for (const auto& u : r.uses) {
            const int lu = ctx.landuse_names.find(u.first);
            cov_[cidx(c, lu)] = u.second / 100.0;
        }
        covered[static_cast<std::size_t>(c)] = 1;
    };
    for (int pass = 0; pass < 3; ++pass) {
        for (const auto& r : coverage_rows) {
            if (static_cast<int>(r.key.scope) != pass) continue;
            bool bad = false;
            for (const auto& u : r.uses)
                if (ctx.landuse_names.find(u.first) < 0) {
                    out.push_back("ERROR [2D_COVERAGES] land use '" + u.first + "' is not defined.");
                    bad = true;
                }
            if (bad) continue;
            if (r.key.scope == SqScope::GLOBAL) {
                for (int c = 0; c < n_cells_; ++c) apply(r, c);
            } else if (r.key.scope == SqScope::TAG) {
                bool hit = false;
                for (int c = 0; c < n_cells_; ++c)
                    if (static_cast<std::size_t>(c) < mesh.tri_tag.size() && mesh.tri_tag[static_cast<std::size_t>(c)] == r.key.tag) {
                        apply(r, c); hit = true;
                    }
                if (!hit) out.push_back("ERROR [2D_COVERAGES] TAG '" + r.key.tag + "' matches no cell.");
            } else {
                if (r.key.cell < 0 || r.key.cell >= n_cells_) {
                    out.push_back("ERROR [2D_COVERAGES] CELL " + std::to_string(r.key.cell + 1) +
                                  " is beyond the mesh (" + std::to_string(n_cells_) + " cells).");
                    continue;
                }
                apply(r, r.key.cell);
            }
        }
    }
    for (const auto& d : out) if (d.rfind("ERROR", 0) == 0) return out;

    // ---- curb lengths ------------------------------------------------------
    curb_.assign(static_cast<std::size_t>(n_cells_), 0.0);
    for (int pass = 0; pass < 3; ++pass) {
        for (const auto& r : curb_rows) {
            if (static_cast<int>(r.key.scope) != pass) continue;
            if (r.key.scope == SqScope::GLOBAL) {
                std::fill(curb_.begin(), curb_.end(), r.length);
            } else if (r.key.scope == SqScope::TAG) {
                for (int c = 0; c < n_cells_; ++c)
                    if (static_cast<std::size_t>(c) < mesh.tri_tag.size() && mesh.tri_tag[static_cast<std::size_t>(c)] == r.key.tag)
                        curb_[static_cast<std::size_t>(c)] = r.length;
            } else if (r.key.cell >= 0 && r.key.cell < n_cells_) {
                curb_[static_cast<std::size_t>(r.key.cell)] = r.length;
            } else {
                out.push_back("ERROR [2D_CURB_LENGTH] CELL " + std::to_string(r.key.cell + 1) + " is beyond the mesh.");
                return out;
            }
        }
    }
    // D-A25: a PER_CURB land use on a covered cell needs a curb length.
    for (int c = 0; c < n_cells_; ++c)
        for (int lu = 0; lu < n_lu_; ++lu) {
            if (cov_[cidx(c, lu)] <= 0.0) continue;
            for (int s = 0; s < n_sp_; ++s)
                if (param(lu, s).bu_type != 0 && param(lu, s).normalizer == 1 &&
                    !(curb_[static_cast<std::size_t>(c)] > 0.0)) {
                    out.push_back("ERROR [2D_COVERAGES] cell " + std::to_string(c + 1) + " carries land use '" +
                                  ctx.landuse_names.name_of(lu) + "', whose buildup of '" + names_[static_cast<std::size_t>(s)] +
                                  "' is PER_CURB, but no [2D_CURB_LENGTH] gives it a curb length.");
                    return out;
                }
        }

    // ---- loadings ----------------------------------------------------------
    init_.assign(static_cast<std::size_t>(n_cells_) * static_cast<std::size_t>(n_sp_), 0.0);
    for (int pass = 0; pass < 3; ++pass) {
        for (const auto& r : loading_rows) {
            if (static_cast<int>(r.key.scope) != pass) continue;
            int s = -1;
            for (int i = 0; i < n_sp_; ++i) if (names_[static_cast<std::size_t>(i)] == r.species) { s = i; break; }
            if (s < 0) {
                out.push_back("ERROR [2D_LOADINGS] '" + r.species + "' is neither a pollutant nor a "
                              "reactions-component species (age and temperature do not build up).");
                return out;
            }
            auto set = [&](int c) { init_[static_cast<std::size_t>(c) * static_cast<std::size_t>(n_sp_) + static_cast<std::size_t>(s)] = r.value; };
            if (r.key.scope == SqScope::GLOBAL) { for (int c = 0; c < n_cells_; ++c) set(c); }
            else if (r.key.scope == SqScope::TAG) {
                for (int c = 0; c < n_cells_; ++c)
                    if (static_cast<std::size_t>(c) < mesh.tri_tag.size() && mesh.tri_tag[static_cast<std::size_t>(c)] == r.key.tag) set(c);
            } else if (r.key.cell >= 0 && r.key.cell < n_cells_) set(r.key.cell);
            else { out.push_back("ERROR [2D_LOADINGS] CELL " + std::to_string(r.key.cell + 1) + " is beyond the mesh."); return out; }
        }
    }

    // ---- ownership (D-A23) ---------------------------------------------------
    if (opts.rainfall_mode == RainfallMode::NONE) {
        out.push_back("[2D_COVERAGES] authored but RAINFALL_MODE NONE — the subcatchments own the "
                      "surface (no rain on the mesh); cell buildup / washoff is inert this run.");
        return out;
    }
    {
        // Covered cells inside a subcatchment that itself has [COVERAGES]:
        // the same storm would load the outlet twice. One warning, naming
        // the subcatchments; the run proceeds (a hybrid may be intended).
        const auto& px = ctx.spatial.subcatch_polygon_x;
        const auto& py = ctx.spatial.subcatch_polygon_y;
        const int nsub = ctx.n_subcatches(), nlu1 = ctx.subcatches.coverage_n_landuses;
        const double inv = (opts.mesh_to_si_factor > 0.0) ? 1.0 / opts.mesh_to_si_factor : 1.0;
        std::set<std::string> names;
        for (int sub = 0; sub < nsub; ++sub) {
            const auto us = static_cast<std::size_t>(sub);
            if (us >= px.size() || px[us].size() < 3) continue;
            bool sub_covered = false;
            for (int lu = 0; lu < nlu1 && !sub_covered; ++lu) {
                const auto k = us * static_cast<std::size_t>(nlu1) + static_cast<std::size_t>(lu);
                if (k < ctx.subcatches.coverage.size() && ctx.subcatches.coverage[k] > 0.0) sub_covered = true;
            }
            if (!sub_covered) continue;
            const auto& xs = px[us]; const auto& ys = py[us];
            for (int c = 0; c < n_cells_ && !names.count(ctx.subcatch_names.name_of(sub)); ++c) {
                if (!covered[static_cast<std::size_t>(c)]) continue;
                const double x = mesh.tri_cx[static_cast<std::size_t>(c)] * inv;
                const double y = mesh.tri_cy[static_cast<std::size_t>(c)] * inv;
                bool in = false;
                for (std::size_t i = 0, j = xs.size() - 1; i < xs.size(); j = i++)
                    if (((ys[i] > y) != (ys[j] > y)) &&
                        (x < (xs[j] - xs[i]) * (y - ys[i]) / (ys[j] - ys[i]) + xs[i])) in = !in;
                if (in) names.insert(ctx.subcatch_names.name_of(sub));
            }
        }
        if (!names.empty()) {
            std::string list;
            for (const auto& n : names) { if (!list.empty()) list += ", "; list += n; }
            out.push_back("[2D_COVERAGES] covered cells lie inside subcatchment(s) that also carry "
                          "[COVERAGES] (" + list + "): the same storm loads the outlet from both; "
                          "if that is not intended, remove one side's coverages.");
        }
    }

    area_la_.assign(static_cast<std::size_t>(n_cells_), 0.0);
    for (int c = 0; c < n_cells_; ++c)
        area_la_[static_cast<std::size_t>(c)] = mesh.tri_area[static_cast<std::size_t>(c)] * area_to_landarea_;
    bu_.assign(static_cast<std::size_t>(n_lu_) * static_cast<std::size_t>(n_sp_) * static_cast<std::size_t>(n_cells_), 0.0);
    last_swept_.assign(static_cast<std::size_t>(n_cells_) * static_cast<std::size_t>(n_lu_), 0.0);
    runoff_vol_.assign(static_cast<std::size_t>(n_cells_), 0.0);
    led_init_.assign(static_cast<std::size_t>(n_sp_), 0.0);
    led_bu_ = led_wo_ = led_sw_ = led_bmp_ = led_init_;
    active_ = true;
    return out;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

void SurfaceQuality2D::initState(const SimulationContext& ctx, const MeshData& mesh) {
    if (!active_) return;
    const double dry_days = ctx.options.dry_days;
    for (int c = 0; c < n_cells_; ++c) {
        const auto uc = static_cast<std::size_t>(c);
        const double area_la = mesh.tri_area[uc] * area_to_landarea_;
        for (int lu = 0; lu < n_lu_; ++lu) {
            const double frac = cov_[cidx(c, lu)];
            if (frac <= 0.0) continue;
            for (int s = 0; s < n_sp_; ++s) {
                const Params& P = param(lu, s);
                const double norm = (P.normalizer == 0) ? frac * area_la : frac * curb_[uc];
                const double load0 = init_[uc * static_cast<std::size_t>(n_sp_) + static_cast<std::size_t>(s)];
                double mass = 0.0;
                if (load0 > 0.0) {
                    mass = (P.normalizer == 0) ? load0
                         : (curb_[uc] > 0.0 ? load0 * area_la / curb_[uc] : 0.0);
                } else if (dry_days > 0.0 && P.bu_type != 0 && P.bu_type != 4) {
                    mass = buildupAt(P.bu_type, P.c0, P.c1, P.c2, dry_days);
                }
                if (mass <= 0.0) continue;
                bu_[bidx(c, lu, s)] = mass;
                led_init_[static_cast<std::size_t>(s)] += mass * norm;
            }
        }
    }
}

double SurfaceQuality2D::buildupPerArea(int c, int s) const noexcept {
    if (!active_) return 0.0;
    const auto uc = static_cast<std::size_t>(c);
    const double area_la = area_la_[uc];
    if (!(area_la > 0.0)) return 0.0;
    double mass = 0.0;   // user mass on the cell
    for (int lu = 0; lu < n_lu_; ++lu) {
        const double frac = cov_[cidx(c, lu)];
        if (frac <= 0.0) continue;
        const Params& P = param(lu, s);
        const double norm = (P.normalizer == 0) ? frac * area_la : frac * curb_[uc];
        mass += bu_[bidx(c, lu, s)] * norm;
    }
    return mass / area_la;
}

void SurfaceQuality2D::step(const SimulationContext& ctx, const MeshData& mesh,
                            SurfaceStateData& state, double dt_runoff, double abs_time,
                            bool is_raining) {
    if (!active_ || !(dt_runoff > 0.0)) return;
    auto& tr = state.transport;
    if (!tr.active() || tr.cell_runoff_vol.size() != static_cast<std::size_t>(n_cells_)) return;
    if (tr.gained_washoff.size() != static_cast<std::size_t>(tr.n_species))
        tr.gained_washoff.assign(static_cast<std::size_t>(tr.n_species), 0.0);

    const double dt_days = dt_runoff / ucf::SEC_PER_DAY;
    // washoff-law unit factors
    const double q_to_display = si_ ? 1000.0 * 3600.0 : 39.3700787 * 3600.0;   // m/s → mm/hr | in/hr
    const double ucf_flow     = ucf::UCF(ucf::FLOW, ctx.options);                 // cfs → display flow
    // sweeping season (the A7 block's rule)
    const int sweep_doy = datetime::dayOfYear(abs_time);
    const int ss = ctx.options.sweep_start, se = ctx.options.sweep_end;
    const bool in_season = (ss <= se) ? (sweep_doy >= ss && sweep_doy <= se)
                                      : (sweep_doy >= ss || sweep_doy <= se);

    for (int c = 0; c < n_cells_; ++c) {
        const auto uc = static_cast<std::size_t>(c);
        const double area_m2 = mesh.tri_area[uc];
        const double area_la = area_m2 * area_to_landarea_;
        // m³ the cell produced this step (net outflow, D-A22); a cell that
        // received more than it passed on made none.
        const double v_out   = std::max(tr.cell_runoff_vol[uc], 0.0);
        tr.cell_runoff_vol[uc] = 0.0;
        runoff_vol_[uc]     += v_out;
        const double q_ms    = (area_m2 > 0.0) ? v_out / dt_runoff / area_m2 : 0.0;   // m/s
        const double Q_m3s   = v_out / dt_runoff;
        const double q_expon = q_ms * q_to_display;
        const double q_flow  = (Q_m3s / kM3PerFt3) * ucf_flow;

        for (int lu = 0; lu < n_lu_; ++lu) {
            const double frac = cov_[cidx(c, lu)];
            if (frac <= 0.0) continue;
            const double interval = static_cast<std::size_t>(lu) < ctx.landuses.sweep_interval.size()
                                        ? ctx.landuses.sweep_interval[static_cast<std::size_t>(lu)] : 0.0;
            // --- sweeping event for this (cell, land use), the A7 rule -----------
            bool sweep_now = false;
            if (!is_raining && in_season && interval > 0.0) {
                double& ls = last_swept_[cidx(c, lu)];
                ls += dt_days;
                if (ls >= interval) { ls = 0.0; sweep_now = true; }
            }
            const double removal_frac = sweep_now ? ctx.landuses.sweep_removal[static_cast<std::size_t>(lu)] / 100.0 : 0.0;

            for (int s = 0; s < n_sp_; ++s) {
                const Params& P = param(lu, s);
                const auto us = static_cast<std::size_t>(s);
                const double mcf  = mcf_[us];
                const double norm = (P.normalizer == 0) ? frac * area_la : frac * curb_[uc];
                double& B = bu_[bidx(c, lu, s)];

                // --- buildup accrual (stepSurfaceQuality) ---------------------
                if (P.bu_type != 0) {
                    if (P.bu_type == 4) {                                   // EXT
                        double rate = 0.0;
                        if (P.ts_idx >= 0 && P.ts_idx < static_cast<int>(ctx.tables.tables.size()))
                            rate = P.c1 * table_lookup_cursor(
                                const_cast<Table&>(ctx.tables.tables[static_cast<std::size_t>(P.ts_idx)]), abs_time);
                        const double nb = std::max(0.0, std::min(B + rate * dt_days, P.c0));
                        const double change = nb - B;
                        if (change != 0.0) { B = nb; led_bu_[us] += change * norm; }
                    } else {
                        double days = 0.0;
                        if (B > 0.0) {
                            switch (P.bu_type) {
                                case 1: days = (P.c1 * P.c2 > 0.0) ? std::pow(B / P.c1, 1.0 / P.c2) : 0.0; break;
                                case 2: days = (P.c0 * P.c1 > 0.0 && B < P.c0) ? -std::log(1.0 - B / P.c0) / P.c1 : 0.0; break;
                                case 3: days = (P.c0 > B) ? B * P.c2 / (P.c0 - B) : P.max_days; break;
                                default: break;
                            }
                        }
                        days += dt_days;
                        const double nb = buildupAt(P.bu_type, P.c0, P.c1, P.c2, days);
                        const double change = nb - B;
                        B = nb;
                        if (change > 0.0) led_bu_[us] += change * norm;
                    }
                }

                // --- washoff into the cell row (stepSurfaceQuality, SI) ----------
                if (P.wo_type != 0 && q_ms > kMinRunoffMs) {
                    // W_row: concentration-units × m³ per second (the row's unit)
                    double w_row = 0.0;
                    const double b_mass_row = (mcf > 0.0) ? (B * norm) / mcf / 1000.0 : 0.0;  // user → conc-mass → row
                    switch (P.wo_type) {
                        case 3: w_row = P.coeff * Q_m3s; break;                                              // EMC
                        case 1: if (B > 0.0) w_row = (P.coeff / 3600.0) * std::pow(q_expon, P.expon) * b_mass_row; break;  // EXP
                        case 2: w_row = P.coeff * std::pow(q_flow, P.expon) / 1000.0; break;                 // RC
                        default: break;
                    }
                    const double max_row = b_mass_row / dt_runoff;
                    if (w_row > max_row && P.wo_type != 3) w_row = max_row;
                    const double washed_user = w_row * 1000.0 * mcf * dt_runoff;
                    if (P.bu_type == 0 && washed_user > B * norm) {
                        led_bu_[us] += washed_user;          // "no buildup function" ledger branch
                        B = 0.0;
                    } else if (norm > 0.0) {
                        B = std::max(B - washed_user / norm, 0.0);
                    }
                    const double bmp_row = w_row * (P.bmp_effic / 100.0);
                    if (bmp_row > 0.0) { led_bmp_[us] += bmp_row * 1000.0 * mcf * dt_runoff; w_row -= bmp_row; }
                    if (w_row > 0.0) {
                        const double dm = w_row * dt_runoff;
                        const int row = row_of_[us];
                        tr.cell_mass[tr.idx(row, c)] += dm;
                        tr.gained_washoff[static_cast<std::size_t>(row)] += dm;
                        led_wo_[us] += dm * 1000.0 * mcf;
                    }
                }

                // --- sweeping removal (A7) -----------------------------------------
                if (sweep_now) {
                    const double removed = B * removal_frac * (P.sweep_effic / 100.0);
                    if (removed > 0.0) { B = std::max(B - removed, 0.0); led_sw_[us] += removed * norm; }
                }
            }
        }
    }
}

}  // namespace openswmm::twoD
