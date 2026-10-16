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
#include "SurfaceSpeciesKernel.hpp"

namespace openswmm::msxsurf {

namespace {


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

// Multispecies Phase 2 (S3): LID REMOVALS entries kept by name at parse time.
// A pollutant declared after [LID_CONTROLS] (the usual file order) goes back
// to the pollutant table, where the parser could not put it; a species gets
// its drain removal fraction; anything else is reported.
void resolveLidRemovals(SimulationContext& ctx, std::vector<std::string>& warnings) {
    auto& lc = ctx.lid_controls;
    auto& s = ctx.reactions.surface;
    const int nm = ctx.reactions.configured ? ctx.reactions.n_species() : 0;
    const int nc = lc.count();
    s.lid_rmvl.assign(nm > 0 && nc > 0 ? static_cast<std::size_t>(nc) * static_cast<std::size_t>(nm) : 0, 0.0);
    for (std::size_t c = 0; c < lc.removals_by_name.size(); ++c) {
        auto& rows = lc.removals_by_name[c];
        std::vector<std::pair<std::string, double>> keep;
        for (const auto& [name, frac] : rows) {
            const int pi = ctx.pollutant_names.find(name);
            if (pi >= 0) {
                if (lc.removals.size() <= c) lc.removals.resize(c + 1);
                lc.removals[c].push_back({pi, frac});
                continue;
            }
            keep.push_back({name, frac});
            const int m = nm > 0 ? ctx.reactions.find_species(name) : -1;
            if (m >= 0 && static_cast<int>(c) < nc)
                s.lid_rmvl[c * static_cast<std::size_t>(nm) + static_cast<std::size_t>(m)] = frac;
            else
                warnings.push_back("[LID_CONTROLS] " +
                                   (c < lc.names.size() ? lc.names[c] : std::string("?")) +
                                   " REMOVALS: '" + name +
                                   "' is neither a pollutant nor a reaction species; ignored.");
        }
        rows = std::move(keep);
    }
}

namespace {
std::vector<std::string> resolveRows(SimulationContext& ctx) {
    std::vector<std::string> warnings;
    auto& s  = ctx.reactions.surface;
    s.resolved = false;
    resolveLidRemovals(ctx, warnings);
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
}  // namespace

// Multispecies Phase 2 (S4): a species that rains in deposits on every
// subcatchment, land use or not — the pollutant kernel's wet deposition has
// no land-use condition. Such a deck may have no species buildup rows, so the
// store is set up here with empty land-use parameters.
void activateForRain(SimulationContext& ctx) noexcept {
    auto& s = ctx.reactions.surface;
    auto& rx = ctx.reactions;
    if (s.resolved || !rx.configured) return;
    const int nm = rx.n_species();
    bool rain = false;
    for (int m = 0; m < nm && m < static_cast<int>(rx.rain_conc.size()); ++m)
        rain = rain || rx.rain_conc[static_cast<std::size_t>(m)] > 0.0;
    if (!rain) return;
    if (s.n_species != nm || s.n_landuses != std::max(ctx.n_landuses(), 0)) {
        s.resize_params(std::max(ctx.n_landuses(), 0), nm);
        s.init_loading.assign(static_cast<std::size_t>(std::max(ctx.n_subcatches(), 0)) *
                              static_cast<std::size_t>(nm), 0.0);
        for (int m = 0; m < nm; ++m)
            s.mcf[static_cast<std::size_t>(m)] =
                speciesUnitFactor(static_cast<std::size_t>(m) < rx.species_units.size()
                                      ? rx.species_units[static_cast<std::size_t>(m)] : std::string{},
                                  ctx.options);
        refreshDerived(ctx);
    }
    s.resolved = true;
}

std::vector<std::string> resolve(SimulationContext& ctx) {
    auto warnings = resolveRows(ctx);
    activateForRain(ctx);
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
    activateForRain(ctx);   // a rain concentration set through the API after open
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

namespace {
// Multispecies Phase 2 (S2): the species family as a surface-species block,
// so it runs the pollutant kernel's expressions (SurfaceSpeciesKernel). The
// parameter structs are rebuilt from the species arrays on every call, as the
// pollutant side's refreshLanduseParams does from ctx.buildup / ctx.washoff,
// so a C API edit takes effect on the next step.
struct SpeciesParams {
    std::vector<landuse::BuildupParams> bu;
    std::vector<landuse::WashoffParams> wo;
};
SpeciesParams speciesParams(const MsxSurfaceData& s) {
    SpeciesParams p;
    const std::size_t n = s.bu_type.size();
    p.bu.resize(n); p.wo.resize(n);
    for (std::size_t k = 0; k < n; ++k) {
        auto& bp = p.bu[k];
        bp.type = static_cast<landuse::BuildupType>(s.bu_type[k]);
        bp.coeff[0] = s.bu_c1[k];
        bp.coeff[1] = s.bu_c2[k];
        bp.coeff[2] = s.bu_c3[k];
        bp.normalizer = s.bu_normalizer[k];
        bp.max_days = s.bu_max_days[k];
        auto& wp = p.wo[k];
        wp.type = static_cast<landuse::WashoffType>(s.wo_type[k]);
        wp.coeff = s.wo_coeff[k];
        wp.expon = s.wo_expon[k];
        wp.sweep_effic = s.wo_sweep_effic[k];
        wp.bmp_effic = s.wo_bmp_effic[k];
    }
    return p;
}
surfacespecies::Block speciesBlock(SimulationContext& ctx, MsxSurfaceData& s, const SpeciesParams& p) {
    surfacespecies::Block b;
    b.n = s.n_species;
    b.nlu = s.n_landuses;
    b.bu_params = &p.bu;
    b.wo_params = &p.wo;
    b.buildup = &s.buildup;
    b.conc = &s.washoff_conc;
    b.ponded_qual = &s.ponded_qual;
    b.runon_qual_rate = &s.runon_qual_rate;
    b.total_load = &s.led_subcatch_load;
    b.mcf = &s.mcf;
    // [REACTION_RAINFALL] when given, else the store's zeros (rain falls clean).
    b.c_rain = ctx.reactions.rain_conc.size() == static_cast<std::size_t>(s.n_species)
                   ? &ctx.reactions.rain_conc : &s.c_rain;
    b.snow_only = &s.snow_only;
    b.led_surface_buildup = &s.led_buildup;
    b.led_wet_deposition = &s.led_wet_deposition;
    b.led_bmp_removal = &s.led_bmp_removal;
    b.led_infil_loss = &s.led_infil_loss;
    b.led_runoff_load = &s.led_runoff_load;
    b.led_sweeping = &s.led_sweeping;
    b.sweep_now = &ctx.subcatches.sweep_now;
    b.sweep_removal = &ctx.landuses.sweep_removal;
    return b;
}
}  // namespace

void step(SimulationContext& ctx, double dt_runoff, double old_runoff_time) {
    auto& s = ctx.reactions.surface;
    if (!s.active() || !(dt_runoff > 0.0)) return;
    const auto p = speciesParams(s);
    auto b = speciesBlock(ctx, s, p);
    surfacespecies::step(ctx, b, dt_runoff, old_runoff_time);
}

void bookPostLid(SimulationContext& ctx, double dt_runoff,
                 const std::vector<double>& lid_infil_vol) {
    auto& s = ctx.reactions.surface;
    if (!s.active() || ctx.n_subcatches() <= 0) return;
    const auto p = speciesParams(s);
    auto b = speciesBlock(ctx, s, p);
    surfacespecies::bookPostLid(ctx, b, dt_runoff, lid_infil_vol);
}

void addLidDrain(SimulationContext& ctx, int control, int sc, int node, double drain_cfs,
                 bool to_perv, bool runoff_load, double dt_runoff) noexcept {
    auto& s = ctx.reactions.surface;
    if (!s.active() || node < 0 || node >= ctx.n_nodes()) return;
    const int nm = s.n_species;
    const auto want = static_cast<std::size_t>(ctx.n_nodes()) * static_cast<std::size_t>(nm);
    if (s.lid_drain_load.size() != want) s.lid_drain_load.assign(want, 0.0);
    // The pollutant lines in the A6b drain loop, species side: the drain
    // carries the subcatchment's washoff concentration less the unit's
    // removal, and books the removed share as BMP removal.
    for (int m = 0; m < nm; ++m) {
        const auto rk = static_cast<std::size_t>(control) * static_cast<std::size_t>(nm) + static_cast<std::size_t>(m);
        const double rmvl = (control >= 0 && rk < s.lid_rmvl.size()) ? s.lid_rmvl[rk] : 0.0;
        const double c_src = s.washoff_conc[s.sidx(sc, m)];
        s.lid_drain_load[static_cast<std::size_t>(node) * static_cast<std::size_t>(nm) + static_cast<std::size_t>(m)] +=
            drain_cfs * c_src * (1.0 - rmvl);
        if (!to_perv) {
            const double w = drain_cfs * c_src * dt_runoff * 28.317 * s.mcf[static_cast<std::size_t>(m)];
            if (w > 0.0) {
                s.led_bmp_removal[static_cast<std::size_t>(m)] += rmvl * w;
                if (runoff_load) s.led_runoff_load[static_cast<std::size_t>(m)] += w * (1.0 - rmvl);
            }
        }
    }
}

void addLidDrainRunon(SimulationContext& ctx, int control, int sc, int receiver, double drain_cfs,
                      bool to_perv, bool runoff_load, double dt_runoff) noexcept {
    auto& s = ctx.reactions.surface;
    if (!s.active() || receiver < 0 || receiver >= ctx.n_subcatches()) return;
    const int nm = s.n_species;
    for (int m = 0; m < nm; ++m) {
        const auto rk = static_cast<std::size_t>(control) * static_cast<std::size_t>(nm) + static_cast<std::size_t>(m);
        const double rmvl = (control >= 0 && rk < s.lid_rmvl.size()) ? s.lid_rmvl[rk] : 0.0;
        const double c_src = s.washoff_conc[s.sidx(sc, m)];
        const auto k = s.sidx(receiver, m);
        if (k < s.lid_drain_runon_load.size())
            s.lid_drain_runon_load[k] += (drain_cfs * c_src * 28.317) * (1.0 - rmvl);
        if (!to_perv) {
            const double w = drain_cfs * c_src * dt_runoff * 28.317 * s.mcf[static_cast<std::size_t>(m)];
            if (w > 0.0) {
                s.led_bmp_removal[static_cast<std::size_t>(m)] += rmvl * w;
                if (runoff_load) s.led_runoff_load[static_cast<std::size_t>(m)] += w * (1.0 - rmvl);
            }
        }
    }
}

void consumeLidDrainRunon(SimulationContext& ctx, int receiver) noexcept {
    auto& s = ctx.reactions.surface;
    if (!s.active()) return;
    for (int m = 0; m < s.n_species; ++m) {
        const auto k = s.sidx(receiver, m);
        if (k < s.lid_drain_runon_load.size() && k < s.runon_qual_rate.size()) {
            s.runon_qual_rate[k] += s.lid_drain_runon_load[k];
            s.lid_drain_runon_load[k] = 0.0;
        }
    }
}

void rollLidDrain(SimulationContext& ctx) noexcept {
    auto& s = ctx.reactions.surface;
    s.lid_drain_load_old = s.lid_drain_load;
    std::fill(s.lid_drain_load.begin(), s.lid_drain_load.end(), 0.0);
}

void clearRunon(SimulationContext& ctx) noexcept {
    auto& s = ctx.reactions.surface;
    std::fill(s.runon_qual_rate.begin(), s.runon_qual_rate.end(), 0.0);
}

void addRunon(SimulationContext& ctx, int donor, int receiver, double q) noexcept {
    auto& s = ctx.reactions.surface;
    if (!s.active()) return;
    // The pollutant line in assembleRunon, species side: legacy
    // subcatch.c:553, newQual += oldRunoff * oldQual * LperFT3.
    for (int m = 0; m < s.n_species; ++m) {
        const auto src = s.sidx(donor, m), dst = s.sidx(receiver, m);
        if (src < s.washoff_conc_old.size() && dst < s.runon_qual_rate.size())
            s.runon_qual_rate[dst] += q * s.washoff_conc_old[src] * 28.317;
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
        // addWetWeatherLoads, species side. Legacy weights the runoff flow and
        // the load beside it by where THIS routing instant falls between the
        // two runoff times; the fixed midpoint that used to stand here
        // delivered the storm's average every routing step instead of its
        // schedule. The pollutant kernel carries the same two lines and
        // LoadReachesTheNetworkUnderBothSolvers holds the two together.
        const double f  = ctx.runoff_interp_f;
        const double f1 = 1.0 - f;
        const double q_old = ctx.subcatches.old_runoff[ui];
        const double q_new = ctx.subcatches.runoff[ui];
        const double q = f1 * q_old + f * q_new;
        if (q <= 0.0) continue;
        const bool has_old = s.washoff_conc_old.size() == s.washoff_conc.size();
        const auto base = static_cast<std::size_t>(out_node) * static_cast<std::size_t>(nm);
        for (int m = 0; m < nm; ++m) {
            const auto si = s.sidx(i, m);
            const double c_new = s.washoff_conc[si];
            const double c_old = has_old ? s.washoff_conc_old[si] : c_new;
            // the interpolated PRODUCT runoff*conc, not the product of the two
            // interpolations (legacy surfqual_getWtdWashoff).
            const double mass_rate = f1 * (q_old * c_old) + f * (q_new * c_new);
            if (mass_rate > 0.0)
                rx.msx_ext_mass_in[base + static_cast<std::size_t>(m)] += mass_rate;
        }
    }
    // Multispecies Phase 2 (S3): LID underdrain loads, the species half of
    // QualitySolver::addLidDrainLoads (same interpolation, same node gate on
    // the drained water).
    if (s.lid_drain_load.size() == want) {
        const double f  = ctx.runoff_interp_f;
        const double f1 = 1.0 - f;
        const bool has_old = s.lid_drain_load_old.size() == want;
        for (int j = 0; j < ctx.n_nodes(); ++j) {
            const auto uj = static_cast<std::size_t>(j);
            const double v_new = uj < ctx.nodes.lid_drain_qual_vol.size() ? ctx.nodes.lid_drain_qual_vol[uj] : 0.0;
            const double v_old = uj < ctx.nodes.lid_drain_qual_vol_old.size() ? ctx.nodes.lid_drain_qual_vol_old[uj] : 0.0;
            if (f1 * v_old + f * v_new <= 0.0) continue;
            for (int m = 0; m < nm; ++m) {
                const auto k = uj * static_cast<std::size_t>(nm) + static_cast<std::size_t>(m);
                const double w_new = s.lid_drain_load[k];
                const double w_old = has_old ? s.lid_drain_load_old[k] : 0.0;
                const double load = f1 * w_old + f * w_new;
                if (load > 0.0) rx.msx_ext_mass_in[k] += load;
            }
        }
    }
}

} // namespace openswmm::msxsurf
