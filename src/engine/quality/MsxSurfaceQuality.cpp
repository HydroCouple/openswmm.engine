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
 * @file MsxSurfaceQuality.cpp
 * @brief BW-MSX — see MsxSurfaceQuality.hpp. Every formula below is the
 *        pollutant step's (SWMMEngine::stepSurfaceQuality / A7 sweeping /
 *        initialize 11b), transcribed with `p → m` and the pollutant arrays
 *        replaced by `ReactionData::surface`. Keep them in lock-step: the
 *        BW-MSX gate asserts an MSX species with a pollutant's parameters
 *        produces the pollutant's loads bit-for-bit.
 */

#include "MsxSurfaceQuality.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

#include "../core/DateTime.hpp"
#include "../core/SimulationContext.hpp"
#include "../core/UnitConversion.hpp"

namespace openswmm::msxsurf {

namespace {

constexpr double kMinRunoffRate = 1.0e-9;   // ft/s threshold (stepSurfaceQuality)
constexpr double kLperFt3       = 28.317;

std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}


/// Buildup function forward form (mass per normalizer unit after `days`).
double buildupAt(int type, double c0, double c1, double c2, double days) {
    if (!(days > 0.0)) return 0.0;
    switch (type) {
        case 1: return std::min(c1 * std::pow(days, c2), c0);                        // POW
        case 2: return c0 * (1.0 - std::exp(-c1 * days));                            // EXP
        case 3: return (c2 + days > 0.0) ? c0 * days / (c2 + days) : 0.0;            // SAT
        default: return 0.0;
    }
}

} // namespace

double speciesUnitFactor(const std::string& units, const SimulationOptions& opts) {
    // D-A27: concentration-mass → user-mass factor from the species' units.
    const double mass_ucf = ucf::UCF(ucf::MASS, opts);
    const std::string u = upper(units);
    if (u.rfind("UG", 0) == 0) return mass_ucf / 1000.0;
    if (u.rfind("MG", 0) == 0) return mass_ucf;
    return 1.0;   // MMOL, counts, dimensionless: the pollutant COUNTS rule
}

bool active(const SimulationContext& ctx) noexcept {
    return ctx.reactions.surface.active();
}

namespace {

/// A parked row whose constituent turns out to be a pollutant declared
/// later in the file than the section that named it: apply it to the
/// pollutant tables now (the handlers dispatch in file order and used to
/// drop such rows silently). Returns true when consumed.
bool applyLatePollutantRow(SimulationContext& ctx, const MsxSurfaceRows::Buildup& r) {
    const int p = ctx.pollutant_names.find(r.species);
    const int lu = ctx.landuse_names.find(r.landuse);
    if (p < 0 || lu < 0) return p >= 0;
    const int np = ctx.n_pollutants(), nlu = ctx.n_landuses();
    if (ctx.buildup.n_landuses != nlu || ctx.buildup.n_pollutants != np)
        ctx.buildup.resize(nlu, np);
    const auto k = static_cast<std::size_t>(lu * np + p);
    ctx.buildup.func_type[k] = r.func_type;
    ctx.buildup.coeff1[k] = r.c1; ctx.buildup.coeff2[k] = r.c2;
    ctx.buildup.coeff3[k] = (r.func_type == 4) ? static_cast<double>(ctx.find_timeseries(r.ts_name)) : r.c3;
    ctx.buildup.normalizer[k] = r.normalizer;
    return true;
}
bool applyLatePollutantRow(SimulationContext& ctx, const MsxSurfaceRows::Washoff& r) {
    const int p = ctx.pollutant_names.find(r.species);
    const int lu = ctx.landuse_names.find(r.landuse);
    if (p < 0 || lu < 0) return p >= 0;
    const int np = ctx.n_pollutants(), nlu = ctx.n_landuses();
    if (ctx.washoff.n_landuses != nlu || ctx.washoff.n_pollutants != np)
        ctx.washoff.resize(nlu, np);
    const auto k = static_cast<std::size_t>(lu * np + p);
    ctx.washoff.func_type[k] = r.func_type;
    ctx.washoff.coeff[k] = r.coeff; ctx.washoff.expon[k] = r.expon;
    ctx.washoff.sweep_effic[k] = r.sweep_effic; ctx.washoff.bmp_effic[k] = r.bmp_effic;
    return true;
}
bool applyLatePollutantRow(SimulationContext& ctx, const MsxSurfaceRows::Loading& r) {
    const int p = ctx.pollutant_names.find(r.species);
    if (p < 0) return false;
    const int sc = ctx.subcatch_names.find(r.subcatch);
    const int np = ctx.n_pollutants();
    if (sc >= 0 && ctx.subcatches.conc_n_pollutants == np) {
        const auto k = static_cast<std::size_t>(sc * np + p);
        if (k < ctx.subcatches.conc.size()) ctx.subcatches.conc[k] = r.value;
    }
    return true;
}

} // namespace

std::vector<std::string> resolve(SimulationContext& ctx) {
    std::vector<std::string> warnings;
    auto& s  = ctx.reactions.surface;
    s.resolved = false;
    if (s.rows.empty()) return warnings;

    // Rows naming a pollutant declared after the section go back to the
    // pollutant tables; what remains is MSX (or a typo).
    {
        MsxSurfaceRows keep;
        for (const auto& r : s.rows.buildup)  if (!applyLatePollutantRow(ctx, r)) keep.buildup.push_back(r);
        for (const auto& r : s.rows.washoff)  if (!applyLatePollutantRow(ctx, r)) keep.washoff.push_back(r);
        for (const auto& r : s.rows.loadings) if (!applyLatePollutantRow(ctx, r)) keep.loadings.push_back(r);
        s.rows = std::move(keep);
    }
    const auto& rows = s.rows;
    if (rows.empty()) return warnings;

    const int nm  = ctx.reactions.configured ? ctx.reactions.n_species() : 0;
    const int nlu = ctx.n_landuses();
    const int nsc = ctx.n_subcatches();
    if (nm <= 0) {
        warnings.push_back(
            "[BUILDUP]/[WASHOFF]/[LOADINGS]: " +
            std::to_string(rows.buildup.size() + rows.washoff.size() + rows.loadings.size()) +
            " row(s) name a constituent that is neither a [POLLUTANTS] entry nor "
            "a reactions-component species; they are ignored.");
        return warnings;
    }
    s.resize_params(std::max(nlu, 0), nm);
    s.init_loading.assign(static_cast<std::size_t>(std::max(nsc, 0)) *
                          static_cast<std::size_t>(nm), 0.0);
    for (int m = 0; m < nm; ++m)
        s.mcf[static_cast<std::size_t>(m)] =
            speciesUnitFactor(static_cast<std::size_t>(m) < ctx.reactions.species_units.size()
                           ? ctx.reactions.species_units[static_cast<std::size_t>(m)]
                           : std::string{},
                       ctx.options);

    auto species = [&](const std::string& name, const char* section) -> int {
        const int m = ctx.reactions.find_species(name);
        if (m < 0) {
            warnings.push_back(std::string("[") + section + "] '" + name +
                               "' is neither a pollutant nor a reactions-component "
                               "species; row ignored.");
            return -1;
        }
        if (static_cast<std::size_t>(m) < ctx.reactions.species_is_wall.size() &&
            ctx.reactions.species_is_wall[static_cast<std::size_t>(m)]) {
            warnings.push_back(std::string("[") + section + "] '" + name +
                               "' is a WALL species and cannot build up on a "
                               "surface; row ignored.");
            return -1;
        }
        return m;
    };
    auto landuse = [&](const std::string& name, const char* section) -> int {
        const int lu = ctx.landuse_names.find(name);
        if (lu < 0)
            warnings.push_back(std::string("[") + section + "] land use '" + name +
                               "' is not defined; row ignored.");
        return lu;
    };

    bool any = false;
    for (const auto& r : rows.buildup) {
        const int lu = landuse(r.landuse, "BUILDUP"); if (lu < 0) continue;
        const int m  = species(r.species, "BUILDUP");  if (m  < 0) continue;
        const auto k = s.pidx(lu, m);
        s.bu_type[k] = r.func_type;
        s.bu_c1[k] = r.c1; s.bu_c2[k] = r.c2;
        if (r.func_type == 4) {
            const int ts = ctx.find_timeseries(r.ts_name);
            if (ts < 0)
                warnings.push_back("[BUILDUP] EXT time series '" + r.ts_name +
                                   "' for '" + r.species + "' not found; no external buildup.");
            s.bu_c3[k] = static_cast<double>(ts);
        } else {
            s.bu_c3[k] = r.c3;
        }
        s.bu_normalizer[k] = r.normalizer;
        any = any || r.func_type != 0;
    }
    for (const auto& r : rows.washoff) {
        const int lu = landuse(r.landuse, "WASHOFF"); if (lu < 0) continue;
        const int m  = species(r.species, "WASHOFF");  if (m  < 0) continue;
        const auto k = s.pidx(lu, m);
        s.wo_type[k] = r.func_type;
        s.wo_coeff[k] = r.coeff; s.wo_expon[k] = r.expon;
        s.wo_sweep_effic[k] = r.sweep_effic; s.wo_bmp_effic[k] = r.bmp_effic;
        any = any || r.func_type != 0;
    }
    for (const auto& r : rows.loadings) {
        const int sc = ctx.subcatch_names.find(r.subcatch);
        if (sc < 0) {
            warnings.push_back("[LOADINGS] subcatchment '" + r.subcatch +
                               "' is not defined; row ignored.");
            continue;
        }
        const int m = species(r.species, "LOADINGS"); if (m < 0) continue;
        s.init_loading[s.sidx(sc, m)] = r.value;
        any = true;
    }
    s.resolved = any && nlu > 0;
    refreshDerived(ctx);
    return warnings;
}

void refreshDerived(SimulationContext& ctx) noexcept {
    auto& s = ctx.reactions.surface;
    for (std::size_t k = 0; k < s.bu_type.size(); ++k) {
        const double c0 = s.bu_c1[k], c1 = s.bu_c2[k], c2 = s.bu_c3[k];
        double md = 0.0;
        // refreshLanduseParams(): time to reach 99.9 % of the maximum
        if (s.bu_type[k] == 2 && c1 > 0.0)                               md = -std::log(0.001) / c1;
        else if (s.bu_type[k] == 1 && c1 > 0.0 && c2 > 0.0 && c0 > 0.0) md = std::pow(c0 / c1, 1.0 / c2);
        else if (s.bu_type[k] == 3 && c2 > 0.0 && c0 > 0.0)             md = 999.0 * c2;
        s.bu_max_days[k] = md;
    }
}

void initState(SimulationContext& ctx) {
    auto& s = ctx.reactions.surface;
    if (!s.active()) return;
    const int nsc = ctx.n_subcatches(), nlu = s.n_landuses, nm = s.n_species;
    s.resize_state(nsc);
    const double dry_days = ctx.options.dry_days;
    // Legacy landuse_getInitBuildup: a supplied initial loading wins (per unit
    // area, so it is stored per normalizer unit directly for PER_AREA land
    // uses); otherwise the buildup function over the antecedent dry period.
    for (int i = 0; i < nsc; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        for (int lu = 0; lu < nlu; ++lu) {
            const auto cov = ui * static_cast<std::size_t>(nlu) + static_cast<std::size_t>(lu);
            const double frac = (cov < ctx.subcatches.coverage.size())
                                    ? ctx.subcatches.coverage[cov] / 100.0 : 0.0;
            if (frac <= 0.0) continue;
            for (int m = 0; m < nm; ++m) {
                const auto k = s.pidx(lu, m);
                const double norm = (s.bu_normalizer[k] == 0)
                                        ? frac * ctx.subcatches.area[ui]
                                        : frac * ctx.subcatches.curb_length[ui];
                double mass = 0.0;
                const double load0 = s.init_loading[s.sidx(i, m)];
                if (load0 > 0.0) {
                    // Legacy: absolute mass = loading × land-use AREA, whatever
                    // the normalizer; per normalizer unit that is the loading
                    // itself for PER_AREA and loading × area / curb for PER_CURB.
                    if (s.bu_normalizer[k] == 0)
                        mass = load0;
                    else
                        mass = (ctx.subcatches.curb_length[ui] > 0.0)
                                   ? load0 * ctx.subcatches.area[ui] / ctx.subcatches.curb_length[ui]
                                   : 0.0;
                } else if (dry_days > 0.0 && s.bu_type[k] != 0 && s.bu_type[k] != 4) {
                    mass = buildupAt(s.bu_type[k], s.bu_c1[k], s.bu_c2[k], s.bu_c3[k], dry_days);
                }
                if (mass <= 0.0) continue;
                s.buildup[s.bu_idx(i, lu, m)] = mass;
                s.led_init_buildup[static_cast<std::size_t>(m)] += mass * norm;
            }
        }
    }
}

void step(SimulationContext& ctx, double dt_runoff, double abs_time) {
    auto& s = ctx.reactions.surface;
    if (!s.active() || !(dt_runoff > 0.0)) return;
    const int nsc = ctx.n_subcatches(), nlu = s.n_landuses, nm = s.n_species;
    const double dt_days = dt_runoff / ucf::SEC_PER_DAY;
    const double ucf_rain = ucf::UCF(ucf::RAINFALL, ctx.options);
    const double ucf_flow = ucf::UCF(ucf::FLOW, ctx.options);

    for (int i = 0; i < nsc; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        const double q       = ctx.subcatches.runoff[ui];    // cfs
        const double area_ac = ctx.subcatches.area[ui];      // acres (LANDAREA units)
        for (int m = 0; m < nm; ++m) {
            const double mcf_m = s.mcf[static_cast<std::size_t>(m)];
            double total_load = 0.0;   // concentration-mass units / s
            for (int lu = 0; lu < nlu; ++lu) {
                const auto cov = ui * static_cast<std::size_t>(nlu) + static_cast<std::size_t>(lu);
                const double frac = (cov < ctx.subcatches.coverage.size())
                                        ? ctx.subcatches.coverage[cov] / 100.0 : 0.0;
                if (frac <= 0.0) continue;
                const auto k  = s.pidx(lu, m);
                const auto bu = s.bu_idx(i, lu, m);
                const int  bt = s.bu_type[k];
                const double norm = (s.bu_normalizer[k] == 0)
                                        ? frac * area_ac : frac * ctx.subcatches.curb_length[ui];

                // --- Buildup accumulation (stepSurfaceQuality) ---------------
                if (bt != 0) {
                    double mass = s.buildup[bu];
                    if (bt == 4) {                                   // EXTERNAL
                        const double max_bu = s.bu_c1[k], sf = s.bu_c2[k];
                        const int ts_idx = static_cast<int>(s.bu_c3[k]);
                        double rate = 0.0;
                        if (ts_idx >= 0 && ts_idx < static_cast<int>(ctx.tables.tables.size()))
                            rate = sf * table_lookup_cursor(
                                ctx.tables.tables[static_cast<std::size_t>(ts_idx)], abs_time);
                        double new_mass = std::min(mass + rate * dt_days, max_bu);
                        new_mass = std::max(new_mass, 0.0);
                        const double change = new_mass - mass;
                        if (change != 0.0) {
                            s.buildup[bu] = new_mass;
                            s.led_buildup[static_cast<std::size_t>(m)] += change * norm;
                        }
                    } else {
                        const double c0 = s.bu_c1[k], c1 = s.bu_c2[k], c2 = s.bu_c3[k];
                        double days = 0.0;
                        if (mass > 0.0) {
                            switch (bt) {                            // inverse: mass → days
                                case 1: days = (c1 * c2 > 0.0) ? std::pow(mass / c1, 1.0 / c2) : 0.0; break;
                                case 2: days = (c0 * c1 > 0.0 && mass < c0) ? -std::log(1.0 - mass / c0) / c1 : 0.0; break;
                                case 3: days = (c0 > mass) ? mass * c2 / (c0 - mass) : s.bu_max_days[k]; break;
                                default: break;
                            }
                        }
                        days += dt_days;
                        const double new_mass = buildupAt(bt, c0, c1, c2, days);
                        const double change = new_mass - mass;
                        s.buildup[bu] = new_mass;
                        if (change > 0.0)
                            s.led_buildup[static_cast<std::size_t>(m)] += change * norm;
                    }
                }

                // --- Washoff (stepSurfaceQuality) -----------------------------
                const int wt = s.wo_type[k];
                if (wt != 0 && q > kMinRunoffRate) {
                    const double buildup  = s.buildup[bu];
                    const double area_ft2 = area_ac * 43560.0;
                    const double q_expon  = (area_ft2 > 0.0) ? (q / area_ft2) * ucf_rain : 0.0;
                    const double q_flow   = q * ucf_flow;
                    double load = 0.0;
                    switch (wt) {
                        case 3: load = s.wo_coeff[k] * kLperFt3 * q * frac; break;                 // EMC
                        case 1:                                                                    // EXP
                            if (buildup > 0.0 && mcf_m > 0.0)
                                load = (s.wo_coeff[k] / 3600.0) * std::pow(q_expon, s.wo_expon[k])
                                     * (buildup * norm) / mcf_m;
                            break;
                        case 2: load = s.wo_coeff[k] * std::pow(q_flow, s.wo_expon[k]) * frac; break; // RC
                        default: break;
                    }
                    const double avail    = (mcf_m > 0.0) ? (buildup * norm) / mcf_m : 0.0;
                    const double max_load = avail / dt_runoff;
                    if (load > max_load && wt != 3) load = max_load;

                    const double washed_mass = load * mcf_m * dt_runoff;
                    if (bt == 0 && washed_mass > buildup * norm) {
                        // no buildup function: book the washoff as buildup so the
                        // ledger balances (landuse.c:585-593)
                        s.led_buildup[static_cast<std::size_t>(m)] += washed_mass;
                        s.buildup[bu] = 0.0;
                    } else if (norm > 0.0) {
                        s.buildup[bu] = std::max(s.buildup[bu] - washed_mass / norm, 0.0);
                    }
                    const double bmp_removed = load * (s.wo_bmp_effic[k] / 100.0);
                    if (bmp_removed > 0.0) {
                        s.led_bmp_removal[static_cast<std::size_t>(m)] += bmp_removed * dt_runoff * mcf_m;
                        load -= bmp_removed;
                    }
                    total_load += load;
                }
            }

            // Reported concentration (surfqual.c:370) and the two bookings.
            double conc = 0.0;
            if (q > kMinRunoffRate && total_load > 0.0) conc = total_load / q / kLperFt3;
            s.washoff_conc[s.sidx(i, m)] = conc;
            if (total_load > 0.0) {
                const double mass = total_load * dt_runoff * mcf_m;
                const bool reaches_system =
                    (ctx.subcatches.outlet_node[ui] >= 0 ||
                     ctx.subcatches.outlet_subcatch[ui] == i);
                if (reaches_system) s.led_runoff_load[static_cast<std::size_t>(m)] += mass;
                s.led_subcatch_load[s.sidx(i, m)] += mass;
            }
        }
    }
}

void sweep(SimulationContext& ctx, int sc, int lu, double frac, double removal_frac) {
    auto& s = ctx.reactions.surface;
    if (!s.active() || sc < 0 || sc >= s.n_subcatch || lu < 0 || lu >= s.n_landuses) return;
    const auto ui = static_cast<std::size_t>(sc);
    for (int m = 0; m < s.n_species; ++m) {
        const auto k  = s.pidx(lu, m);
        const auto bu = s.bu_idx(sc, lu, m);
        const double effic = s.wo_sweep_effic[k] / 100.0;
        const double norm = (s.bu_normalizer[k] == 0)
                                ? frac * ctx.subcatches.area[ui]
                                : frac * ctx.subcatches.curb_length[ui];
        const double old_bu  = s.buildup[bu];
        const double removed = old_bu * removal_frac * effic;
        s.buildup[bu] = std::max(old_bu - removed, 0.0);
        s.led_sweeping[static_cast<std::size_t>(m)] += removed * norm;
    }
}

void deliver(SimulationContext& ctx, double /*dt_routing*/) {
    auto& s = ctx.reactions.surface;
    if (!s.active()) return;
    auto& rx = ctx.reactions;
    const int nm = rx.n_species();
    if (nm != s.n_species || ctx.n_nodes() <= 0) return;
    const auto want = static_cast<std::size_t>(ctx.n_nodes()) * static_cast<std::size_t>(nm);
    if (rx.msx_ext_mass_in.size() != want) rx.msx_ext_mass_in.assign(want, 0.0);
    for (int i = 0; i < ctx.n_subcatches(); ++i) {
        const auto ui = static_cast<std::size_t>(i);
        const int out_node = ctx.subcatches.outlet_node[ui];
        if (out_node < 0 || out_node >= ctx.n_nodes()) continue;
        // addWetWeatherLoads: trapezoid on old / new runoff
        const double q = 0.5 * (ctx.subcatches.old_runoff[ui] + ctx.subcatches.runoff[ui]);
        if (q <= 0.0) continue;
        const auto base = static_cast<std::size_t>(out_node) * static_cast<std::size_t>(nm);
        for (int m = 0; m < nm; ++m) {
            const double c = s.washoff_conc[s.sidx(i, m)];
            if (c > 0.0) rx.msx_ext_mass_in[base + static_cast<std::size_t>(m)] += q * c;
        }
    }
}

} // namespace openswmm::msxsurf
