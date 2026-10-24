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
 * @file      MsxSurfaceQuality.cpp
 * @brief     BW-MSX — see MsxSurfaceQuality.hpp. Every formula below is the
 *        pollutant step's (SWMMEngine::stepSurfaceQuality / A7 sweeping /
 *        initialize 11b), transcribed with `p → m` and the pollutant arrays
 *        replaced by `ReactionData::surface`. Keep them in lock-step: the
 *        BW-MSX gate asserts an MSX species with a pollutant's parameters
 *        produces the pollutant's loads bit-for-bit.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "MsxSurfaceQuality.hpp"
#include "MsxLidReactions.hpp"
#include "../transport/components/ReactionModule/ReactionGeometry.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

#include "../core/DateTime.hpp"
#include "../core/SimulationContext.hpp"
#include "../core/UnitConversion.hpp"
#include "SurfaceSpeciesKernel.hpp"
#include "QualityRouting.hpp"
#include "../transport/components/ReactionModule/ReactionIntegrator.hpp"
#include "../transport/components/ReactionModule/ReactionLegacyBinding.hpp"

namespace openswmm::msxsurf {

namespace {

/**
 * @brief Uppercase a private byte-string copy for unit-prefix matching.
 * @param[in] s Owned text; characters converted through unsigned char before std::toupper.
 * @return Uppercase copy in current C locale. No context mutation; copying can allocate.
 */
std::string upper(std::string s) {
    for (auto& c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

/**
 * @brief Buildup function forward form (mass per normalizer unit after `days`).
 *
 * @details Evaluate the initial analytic buildup per normalizer unit after a dry period.
 * @param[in] type Integer kind:1 power,2 exponential,3 saturation; others yield0.
 * @param[in] c0 Maximum buildup per user area/curb normalizer unit.
 * @param[in] c1 Rate coefficient with units prescribed by the selected buildup kind.
 * @param[in] c2 Power exponent or saturation time days, depending on kind.
 * @param[in] days Antecedent dry days; nonpositive/NaN yields0.
 * @return Analytic value, capped at c0 for power; saturation returns0 for nonpositive
 * denominator. No external time-series evaluation, coefficient validation, finite repair or
 * state mutation.
 */
double buildupAt(int type, double c0, double c1, double c2, double days) {
    if (!(days > 0.0))
        return 0.0;
    switch (type) {
    case 1:
        return std::min(c1 * std::pow(days, c2), c0); // POW
    case 2:
        return c0 * (1.0 - std::exp(-c1 * days)); // EXP
    case 3:
        return (c2 + days > 0.0) ? c0 * days / (c2 + days) : 0.0; // SAT
    default:
        return 0.0;
    }
}

} // namespace

/**
 * @brief Map a species unit prefix to its concentration-mass to user-mass factor.
 * @param[in] units Case-insensitive text: prefixes UG and MG select micrograms and milligrams;
 * all other strings, including count units, select factor1. Prefixes need not occupy the full
 * string.
 * @param[in] opts Unit system used by UCF(MASS).
 * @return UCF(MASS)/1000 for UG, UCF(MASS) for MG, otherwise1. No model mutation; text copy can
 * allocate.
 */
double speciesUnitFactor(const std::string& units, const SimulationOptions& opts) {
    // D-A27: concentration-mass → user-mass factor from the species' units.
    const double mass_ucf = ucf::UCF(ucf::MASS, opts);
    const std::string u = upper(units);
    if (u.rfind("UG", 0) == 0)
        return mass_ucf / 1000.0;
    if (u.rfind("MG", 0) == 0)
        return mass_ucf;
    return 1.0; // MMOL, counts, dimensionless: the pollutant COUNTS rule
}

/**
 * @brief Read whether the resolved surface species store is active.
 * @param[in] ctx Stable reaction surface dimensions/resolution flag.
 * @return surface.active(), without resolving rows, rebuilding state or allocating.
 */
bool active(const SimulationContext& ctx) noexcept {
    return ctx.reactions.surface.active();
}

namespace {

/**
 * @brief A parked row whose constituent turns out to be a pollutant declared later in the file
 * than the section that named it: apply it to the pollutant tables now (the handlers dispatch in
 * file order and used to drop such rows silently). Returns true when consumed.
 *
 * @details Fold a deferred row naming an existing pollutant into the pollutant-owned table.
 * @param[in,out] ctx Final pollutant/name/land-use tables and writable pollutant parameters or
 * loading state.
 * @param[in] r By-name deferred buildup row; retained independently by caller until return.
 * @return True whenever pollutant name resolves, including unknown land use/subcatchment; false
 * for unknown pollutant so native-species resolution can try it. Stores type, coefficients and
 * normalizer, using time-series index for external buildup.
 * @note Setup-only, exclusive access. Parameter-table dimension mismatch resizes/reset tables;
 * allocation can throw after partial changes. No hydraulic water or clock advance.
 */
bool applyLatePollutantRow(SimulationContext& ctx, const MsxSurfaceRows::Buildup& r) {
    const int p = ctx.pollutant_names.find(r.species);
    const int lu = ctx.landuse_names.find(r.landuse);
    if (p < 0 || lu < 0)
        return p >= 0;
    const int np = ctx.n_pollutants(), nlu = ctx.n_landuses();
    if (ctx.buildup.n_landuses != nlu || ctx.buildup.n_pollutants != np)
        ctx.buildup.resize(nlu, np);
    const auto k = static_cast<std::size_t>(lu * np + p);
    ctx.buildup.func_type[k] = r.func_type;
    ctx.buildup.coeff1[k] = r.c1;
    ctx.buildup.coeff2[k] = r.c2;
    ctx.buildup.coeff3[k] =
        (r.func_type == 4) ? static_cast<double>(ctx.find_timeseries(r.ts_name)) : r.c3;
    ctx.buildup.normalizer[k] = r.normalizer;
    return true;
}
/**
 * @brief Fold a deferred row naming an existing pollutant into the pollutant-owned table.
 * @param[in,out] ctx Final pollutant/name/land-use tables and writable pollutant parameters or
 * loading state.
 * @param[in] r By-name deferred washoff row; retained independently by caller until return.
 * @return True whenever pollutant name resolves, including unknown land use/subcatchment; false
 * for unknown pollutant so native-species resolution can try it. Stores type, coefficient,
 * exponent and sweep/BMP efficiencies.
 * @note Setup-only, exclusive access. Parameter-table dimension mismatch resizes/reset tables;
 * allocation can throw after partial changes. No hydraulic water or clock advance.
 */
bool applyLatePollutantRow(SimulationContext& ctx, const MsxSurfaceRows::Washoff& r) {
    const int p = ctx.pollutant_names.find(r.species);
    const int lu = ctx.landuse_names.find(r.landuse);
    if (p < 0 || lu < 0)
        return p >= 0;
    const int np = ctx.n_pollutants(), nlu = ctx.n_landuses();
    if (ctx.washoff.n_landuses != nlu || ctx.washoff.n_pollutants != np)
        ctx.washoff.resize(nlu, np);
    const auto k = static_cast<std::size_t>(lu * np + p);
    ctx.washoff.func_type[k] = r.func_type;
    ctx.washoff.coeff[k] = r.coeff;
    ctx.washoff.expon[k] = r.expon;
    ctx.washoff.sweep_effic[k] = r.sweep_effic;
    ctx.washoff.bmp_effic[k] = r.bmp_effic;
    return true;
}
/**
 * @brief Fold a deferred row naming an existing pollutant into the pollutant-owned table.
 * @param[in,out] ctx Final pollutant/name/land-use tables and writable pollutant parameters or
 * loading state.
 * @param[in] r By-name deferred loading row; retained independently by caller until return.
 * @return True whenever pollutant name resolves, including unknown land use/subcatchment; false
 * for unknown pollutant so native-species resolution can try it. Writes supplied initial
 * concentration only when subcatchment and existing pollutant-state shape/tuple are covered;
 * otherwise still consumes the recognized-pollutant row.
 * @note Setup-only, exclusive access. Parameter-table dimension mismatch resizes/reset tables;
 * allocation can throw after partial changes. No hydraulic water or clock advance.
 */
bool applyLatePollutantRow(SimulationContext& ctx, const MsxSurfaceRows::Loading& r) {
    const int p = ctx.pollutant_names.find(r.species);
    if (p < 0)
        return false;
    const int sc = ctx.subcatch_names.find(r.subcatch);
    const int np = ctx.n_pollutants();
    if (sc >= 0 && ctx.subcatches.conc_n_pollutants == np) {
        const auto k = static_cast<std::size_t>(sc * np + p);
        if (k < ctx.subcatches.conc.size())
            ctx.subcatches.conc[k] = r.value;
    }
    return true;
}

} // namespace

// Multispecies Phase 2 (S3): LID REMOVALS entries kept by name at parse time.
// A pollutant declared after [LID_CONTROLS] (the usual file order) goes back
// to the pollutant table, where the parser could not put it; a species gets
// its drain removal fraction; anything else is reported.
/**
 * @brief Bind by-name LID removals to pollutant or native species identities.
 * @param[in,out] ctx Final pollutant/species/LID control names; resets dense control*n_species
 * MSX removals and migrates recognized pollutant rows into each control's indexed pollutant
 * removal list.
 * @param[in,out] warnings Appends unknown-species warnings; preserves previous entries.
 * @note MSX rows remain by-name for repeat resolution, pollutant rows are removed after
 * migration. Fractions are copied without range/finite validation. Requires stable matching
 * counts; allocations can leave partial tables/migrations. Exclusive configuration phase; no
 * hydraulic/clock change.
 */
void resolveLidRemovals(SimulationContext& ctx, std::vector<std::string>& warnings) {
    auto& lc = ctx.lid_controls;
    auto& s = ctx.reactions.surface;
    const int nm = ctx.reactions.configured ? ctx.reactions.n_species() : 0;
    const int nc = lc.count();
    s.lid_rmvl.assign(
        nm > 0 && nc > 0 ? static_cast<std::size_t>(nc) * static_cast<std::size_t>(nm) : 0, 0.0);
    for (std::size_t c = 0; c < lc.removals_by_name.size(); ++c) {
        auto& rows = lc.removals_by_name[c];
        std::vector<std::pair<std::string, double>> keep;
        for (const auto& [name, frac] : rows) {
            const int pi = ctx.pollutant_names.find(name);
            if (pi >= 0) {
                if (lc.removals.size() <= c)
                    lc.removals.resize(c + 1);
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
/**
 * @brief Resolve deferred buildup, washoff, loading and LID-removal names into native tables.
 * @param[in,out] ctx Final name tables/reaction configuration. Clears surface resolved flag,
 * folds recognized late pollutant rows into pollutant tables and removes them from deferred
 * surface rows; remaining species rows rebuild parameters, initial loading, conversion factors
 * and derived limits.
 * @return Collected nonfatal warnings for missing names/time series or unsupported WALL species.
 * @note Later repeated rows overwrite earlier entries. Species activation requires nonzero
 * configured buildup/washoff or a loading and at least one land use; rainfall activation is
 * handled separately. Unknown land-use rows naming an existing pollutant are consumed by
 * late-row handling. No physical runoff/clock advance; exclusive setup, prepared counts/extents
 * and allocation-success required. Exceptions can leave reset/rebuilt tables and migrated rows
 * partially changed; no rollback.
 */
std::vector<std::string> resolveRows(SimulationContext& ctx) {
    std::vector<std::string> warnings;
    auto& s = ctx.reactions.surface;
    s.resolved = false;
    resolveLidRemovals(ctx, warnings);
    if (s.rows.empty())
        return warnings;

    // Rows naming a pollutant declared after the section go back to the
    // pollutant tables; what remains is MSX (or a typo).
    {
        MsxSurfaceRows keep;
        for (const auto& r : s.rows.buildup)
            if (!applyLatePollutantRow(ctx, r))
                keep.buildup.push_back(r);
        for (const auto& r : s.rows.washoff)
            if (!applyLatePollutantRow(ctx, r))
                keep.washoff.push_back(r);
        for (const auto& r : s.rows.loadings)
            if (!applyLatePollutantRow(ctx, r))
                keep.loadings.push_back(r);
        s.rows = std::move(keep);
    }
    const auto& rows = s.rows;
    if (rows.empty())
        return warnings;

    const int nm = ctx.reactions.configured ? ctx.reactions.n_species() : 0;
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
    s.init_loading.assign(static_cast<std::size_t>(std::max(nsc, 0)) * static_cast<std::size_t>(nm),
                          0.0);
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
        const int lu = landuse(r.landuse, "BUILDUP");
        if (lu < 0)
            continue;
        const int m = species(r.species, "BUILDUP");
        if (m < 0)
            continue;
        const auto k = s.pidx(lu, m);
        s.bu_type[k] = r.func_type;
        s.bu_c1[k] = r.c1;
        s.bu_c2[k] = r.c2;
        if (r.func_type == 4) {
            const int ts = ctx.find_timeseries(r.ts_name);
            if (ts < 0)
                warnings.push_back("[BUILDUP] EXT time series '" + r.ts_name + "' for '" +
                                   r.species + "' not found; no external buildup.");
            s.bu_c3[k] = static_cast<double>(ts);
        } else {
            s.bu_c3[k] = r.c3;
        }
        s.bu_normalizer[k] = r.normalizer;
        any = any || r.func_type != 0;
    }
    for (const auto& r : rows.washoff) {
        const int lu = landuse(r.landuse, "WASHOFF");
        if (lu < 0)
            continue;
        const int m = species(r.species, "WASHOFF");
        if (m < 0)
            continue;
        const auto k = s.pidx(lu, m);
        s.wo_type[k] = r.func_type;
        s.wo_coeff[k] = r.coeff;
        s.wo_expon[k] = r.expon;
        s.wo_sweep_effic[k] = r.sweep_effic;
        s.wo_bmp_effic[k] = r.bmp_effic;
        any = any || r.func_type != 0;
    }
    for (const auto& r : rows.loadings) {
        const int sc = ctx.subcatch_names.find(r.subcatch);
        if (sc < 0) {
            warnings.push_back("[LOADINGS] subcatchment '" + r.subcatch +
                               "' is not defined; row ignored.");
            continue;
        }
        const int m = species(r.species, "LOADINGS");
        if (m < 0)
            continue;
        s.init_loading[s.sidx(sc, m)] = r.value;
        any = true;
    }
    s.resolved = any && nlu > 0;
    refreshDerived(ctx);
    return warnings;
}
} // namespace

// Multispecies Phase 2 (S4): a species that rains in deposits on every
// subcatchment, land use or not — the pollutant kernel's wet deposition has
// no land-use condition. Such a deck may have no species buildup rows, so the
// store is set up here with empty land-use parameters.
/**
 * @brief Enable native surface storage needed by rainfall or scoped reaction/LID laws.
 * @param[in,out] ctx Configured native species and final land-use/subcatchment counts; may
 * resize parameters and initial loading, set conversion factors/derived limits and mark
 * resolved.
 * @note Returns when already resolved or reactions unconfigured. Positive rain concentration,
 * subcatchment/buildup reaction laws or any LID control can activate otherwise empty rows.
 * Dimension mismatch resets parameter/loading arrays; existing evolved state should not be
 * passed through reconfiguration. No water/clock update; exclusive lifecycle. Allocations in
 * this noexcept routine terminate on allocation failure rather than returning an error.
 */
void activateForRain(SimulationContext& ctx) noexcept {
    auto& s = ctx.reactions.surface;
    auto& rx = ctx.reactions;
    if (s.resolved || !rx.configured)
        return;
    const int nm = rx.n_species();
    bool rain = false;
    for (int m = 0; m < nm && m < static_cast<int>(rx.rain_conc.size()); ++m)
        rain = rain || rx.rain_conc[static_cast<std::size_t>(m)] > 0.0;
    if (!rain && !rx.has_subcatch_reactions() && rx.buildup_laws.empty() && rx.lid_controls.empty())
        return;
    if (s.n_species != nm || s.n_landuses != std::max(ctx.n_landuses(), 0)) {
        s.resize_params(std::max(ctx.n_landuses(), 0), nm);
        s.init_loading.assign(static_cast<std::size_t>(std::max(ctx.n_subcatches(), 0)) *
                                  static_cast<std::size_t>(nm),
                              0.0);
        for (int m = 0; m < nm; ++m)
            s.mcf[static_cast<std::size_t>(m)] =
                speciesUnitFactor(static_cast<std::size_t>(m) < rx.species_units.size()
                                      ? rx.species_units[static_cast<std::size_t>(m)]
                                      : std::string{},
                                  ctx.options);
        refreshDerived(ctx);
    }
    s.resolved = true;
}

/**
 * @brief Resolve named surface rows then activate rainfall/reaction-only species storage.
 * @param[in,out] ctx Stable final species, pollutant, land-use, subcatchment and control names;
 * receives resolved parameters/removal tables and activation state.
 * @return Nonfatal row/removal warnings from resolveRows. Allocation/configuration exceptions
 * can leave partial changes; no rollback, physical water advance or engine clock change.
 * Exclusive setup.
 */
std::vector<std::string> resolve(SimulationContext& ctx) {
    auto warnings = resolveRows(ctx);
    activateForRain(ctx);
    return warnings;
}

/**
 * @brief Recalculate analytic buildup saturation times after a parameter edit.
 * @param[in,out] ctx Parallel surface bu_type/c1/c2/c3/max_days arrays with equal covered
 * extents. Writes max_days for each configured row: exponential .1-percent remainder time, power
 * cap time, saturation999*c3, otherwise0.
 * @note No resize, finite/domain validation beyond positive coefficient guards, clock or
 * inventory change. Caller serializes parameter edits and subsequent surface steps.
 */
void refreshDerived(SimulationContext& ctx) noexcept {
    auto& s = ctx.reactions.surface;
    for (std::size_t k = 0; k < s.bu_type.size(); ++k) {
        const double c0 = s.bu_c1[k], c1 = s.bu_c2[k], c2 = s.bu_c3[k];
        double md = 0.0;
        // refreshLanduseParams(): time to reach 99.9 % of the maximum
        if (s.bu_type[k] == 2 && c1 > 0.0)
            md = -std::log(0.001) / c1;
        else if (s.bu_type[k] == 1 && c1 > 0.0 && c2 > 0.0 && c0 > 0.0)
            md = std::pow(c0 / c1, 1.0 / c2);
        else if (s.bu_type[k] == 3 && c2 > 0.0 && c0 > 0.0)
            md = 999.0 * c2;
        s.bu_max_days[k] = md;
    }
}

/**
 * @brief Reset native surface state and seed initial dry buildup from loadings or dry days.
 * @param[in,out] ctx Resolved species/land-use parameters, final coverage percentages, user-unit
 * areas/curb lengths and DRY_DAYS. May activate rain-only storage, then resize/reset
 * state/ledgers.
 * @note Positive loading takes precedence for every covered land use: per-area loading is
 * direct; per-curb loading is scaled area/curb,0 for absent curb. Otherwise positive dry days
 * use analytic power/exponential/saturation, never external series. Books
 * mass*coverage-normalizer to initial user mass ledger. Requires prepared positive dimensions
 * and covered parallel arrays. No water/clock advance. Exclusive initial lifecycle; repeating
 * resets evolved stores. Allocations may partially reset.
 */
void initState(SimulationContext& ctx) {
    auto& s = ctx.reactions.surface;
    activateForRain(ctx); // a rain concentration set through the API after open
    if (!s.active())
        return;
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
            const double frac =
                (cov < ctx.subcatches.coverage.size()) ? ctx.subcatches.coverage[cov] / 100.0 : 0.0;
            if (frac <= 0.0)
                continue;
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
                        mass =
                            (ctx.subcatches.curb_length[ui] > 0.0)
                                ? load0 * ctx.subcatches.area[ui] / ctx.subcatches.curb_length[ui]
                                : 0.0;
                } else if (dry_days > 0.0 && s.bu_type[k] != 0 && s.bu_type[k] != 4) {
                    mass = buildupAt(s.bu_type[k], s.bu_c1[k], s.bu_c2[k], s.bu_c3[k], dry_days);
                }
                if (mass <= 0.0)
                    continue;
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
/**
 * @brief Materialize shared-kernel buildup/washoff parameter tuples from native species arrays.
 * @param[in] s Stable parameter arrays covering every bu_type entry and matching landuse*species
 * layout.
 * @return Owning vectors of copied buildup and washoff parameters; no borrowed context storage.
 * No validation or state mutation; allocation can throw. Caller retains this copy while a
 * speciesBlock borrows its vectors.
 */
SpeciesParams speciesParams(const MsxSurfaceData& s) {
    SpeciesParams p;
    const std::size_t n = s.bu_type.size();
    p.bu.resize(n);
    p.wo.resize(n);
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
/**
 * @brief Assemble a nonowning shared surface-kernel view of native species and ledgers.
 * @param[in,out] ctx Stable rainfall, sweep, land-use and conventional-LID ownership-mask
 * stores.
 * @param[in,out] s Matching native parameter dimensions, writable buildup/ponded/concentration
 * and user-mass ledgers; every accessed array must cover the shared kernel's dimensions.
 * @param[in] p Materialized parameter vectors, alive and unmodified throughout synchronous use.
 * @return Borrowed block: pointers alias p, s and ctx; no allocation, ownership transfer or
 * mutation by construction. Rain concentration uses reaction array only when its size equals
 * species count, otherwise zero/default surface rainfall. Resize/reconfiguration invalidates
 * view; exclusive step access.
 */
surfacespecies::Block speciesBlock(SimulationContext& ctx, MsxSurfaceData& s,
                                   const SpeciesParams& p) {
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
                   ? &ctx.reactions.rain_conc
                   : &s.c_rain;
    b.snow_only = &s.snow_only;
    b.led_surface_buildup = &s.led_buildup;
    b.led_wet_deposition = &s.led_wet_deposition;
    b.led_bmp_removal = &s.led_bmp_removal;
    b.led_infil_loss = &s.led_infil_loss;
    b.led_runoff_load = &s.led_runoff_load;
    b.led_sweeping = &s.led_sweeping;
    b.led_ponded_lost = &s.led_ponded_lost;
    b.sweep_now = &ctx.subcatches.sweep_now;
    b.sweep_removal = &ctx.landuses.sweep_removal;
    b.lid_reactor_mask = &ctx.reactions.lid.subcatchments;
    return b;
}

// Gather the complete wet pool before the scalar kernel divides its mass
// among infiltration, runoff and storage. A local copy contains failed steps;
// only a successful, finite block can change the transport inventory.
/**
 * @brief React complete wet subcatchment species pools before shared-kernel water partitioning.
 * @param[in,out] ctx Prepared compiled wet laws/workspace identities, pre-step ponded species
 * mass, rain/runon, hydraulic areas/depths and finite optional runoff temperature. Books
 * accepted signed reacted and negative-RATE clamp user mass; can append a once-per-context
 * failure warning.
 * @param[in] dt Positive finite runoff/reaction duration s, supplied by step after its positive
 * guard.
 * @return Owning subcatchment*species concentration-mass vector; enabled pools contain old
 * ponded plus rainfall/runon mass, reacted only for successful finite admissible solve. Other
 * entries remain0.
 * @note Uses non-LID native area ft2, volume ft3 and28.317L/ft3; divides mass by litres for
 * kinetics. Tiny/failed pools retain unreacted mixed mass. Negative values are allowed only for
 * RATE before clipping0. Does not itself install canonical ponded storage or change water/clock.
 * Allocations, integrator or warning operations can throw after earlier ledger changes;
 * exclusive prepared context.
 */
std::vector<double> reactPondedPools(SimulationContext& ctx, double dt) {
    auto& rx = ctx.reactions;
    auto& surface = rx.surface;
    const int ns = rx.n_species();
    std::vector<double> mixed(surface.ponded_qual.size(), 0.0);
    std::vector<double> species(static_cast<std::size_t>(ns)), before(species.size());
    transport::RxWorkspace workspace;
    workspace.init(rx);
    const double area_factor = ucf::UCF(ucf::LANDAREA, ctx.options);
    constexpr double litres_per_ft3 = 28.317;
    for (int sc = 0; sc < ctx.n_subcatches(); ++sc) {
        const auto i = static_cast<std::size_t>(sc);
        if (!rx.wet_enabled(sc))
            continue;
        auto forms = rx.subcatch_form;
        auto expressions = rx.subcatch_expr;
        for (const auto& law : rx.wet_laws)
            if (law.subcatchment == sc) {
                forms[law.species] = law.form;
                expressions[law.species] = law.compiled;
            }
        const double lid = i < ctx.subcatches.total_lid_area_ft2.size()
                               ? ctx.subcatches.total_lid_area_ft2[i]
                               : 0.0;
        const double area = ctx.subcatches.area[i] / area_factor - lid;
        if (!(area > 0.0))
            continue;
        const double rain_volume = std::max(0.0, ctx.subcatches.rainfall[i]) * area * dt;
        const double volume = ctx.subcatches.old_ponded_depth[i] * area + rain_volume +
                              ctx.subcatches.runon_inflow[i] * dt;
        if (!(volume > 0.0))
            continue; // a dry store is not a reactor
        const double litres = volume * litres_per_ft3;
        for (int m = 0; m < ns; ++m) {
            const auto k = surface.sidx(sc, m);
            const double rain = m < static_cast<int>(rx.rain_conc.size()) ? rx.rain_conc[m] : 0.0;
            mixed[k] = surface.ponded_qual[k] + rain * litres_per_ft3 * rain_volume +
                       surface.runon_qual_rate[k] * dt;
        }
        if (volume <= quality::ZERO_VOLUME)
            continue;
        for (int m = 0; m < ns; ++m)
            before[m] = species[m] = mixed[surface.sidx(sc, m)] / litres;
        double variables[static_cast<int>(RxHydVar::COUNT_)] = {};
        // Explicit thermal coupling: beginning-of-step published runoff
        // temperature, never a temperature computed later on this step.
        variables[static_cast<int>(RxHydVar::TEMP)] = rx.default_temp_c;
        if (ctx.options.heat_transport && i < ctx.heat_state.subcatch_runoff_temp.size() &&
            std::isfinite(ctx.heat_state.subcatch_runoff_temp[i]))
            variables[static_cast<int>(RxHydVar::TEMP)] = ctx.heat_state.subcatch_runoff_temp[i];
        const auto report = transport::ReactionIntegrator::step(
            rx, forms, expressions, dt, species.data(), variables, workspace);
        bool physical = report.ok;
        for (int m = 0; m < ns && physical; ++m)
            physical = std::isfinite(species[m]) &&
                       (species[m] >= 0.0 || forms[m] == ReactionExprForm::RATE);
        if (!physical) {
            if (!rx.warned_react_failure) {
                rx.warned_react_failure = true;
                ctx.warnings.push_back(
                    "Subcatchment reaction step failed at '" + ctx.subcatch_names.name_of(sc) +
                    "' (unreacted mixed pool retained): " +
                    (report.ok ? "negative algebraic concentration" : report.error));
            }
            continue;
        }
        for (int m = 0; m < ns; ++m) {
            const double raw = species[m];
            const double clamped = std::max(0.0, raw);
            const double factor = litres * surface.mcf[m];
            surface.led_reacted[m] += (before[m] - raw) * factor;
            surface.led_reaction_clamped[m] += (clamped - raw) * factor;
            if (clamped != raw)
                transport::noteNegativeClamp(ctx, "a wet subcatchment pool");
            mixed[surface.sidx(sc, m)] = clamped * litres;
        }
    }
    return mixed;
}
// React the inventory present at the beginning of the runoff step, before
// accrual, sweeping and washoff. Values are declared species mass / AREA_UNITS,
// independently of each species' authored area/curb buildup normalizer.
/**
 * @brief Apply scoped dry-store reaction laws to each covered land-use/species inventory.
 * @param[in,out] ctx Prepared compiled buildup laws, user-mass conversion/normalizers, coverage
 * and area units; updates accepted buildup, signed reacted/clamped ledgers and diagnostic
 * warnings.
 * @param[in] dt Positive finite reaction duration s; caller qualifies duration.
 * @note Global law rows precede matching subcatchment rows. Converts covered mass to declared
 * reaction-area concentration and back to per-normalizer storage. Zero area/normalizer is
 * skipped; failed or inadmissible solves preserve that pool and warn once, later pools still
 * proceed. Uses default reaction temperature. No hydraulic or clock advance;
 * allocation/integration/warning can throw after earlier accepted pools, no rollback. Exclusive
 * prepared arrays/context ownership.
 */
void reactBuildup(SimulationContext& ctx, double dt) {
    auto& rx = ctx.reactions;
    auto& s = rx.surface;
    if (rx.buildup_laws.empty())
        return;
    const int ns = rx.n_species();
    transport::RxWorkspace ws;
    ws.init(rx);
    std::vector<double> y(ns), before(ns), normalizer(ns);
    std::vector<ReactionExprForm> forms(ns);
    std::vector<RxExprSpan> expr(ns);
    for (int sc = 0; sc < ctx.n_subcatches(); ++sc)
        for (int lu = 0; lu < s.n_landuses; ++lu) {
            const auto cov = std::size_t(sc) * s.n_landuses + lu;
            const double fraction =
                cov < ctx.subcatches.coverage.size() ? ctx.subcatches.coverage[cov] / 100. : 0.;
            const double area = fraction * ctx.subcatches.area[sc] /
                                ucf::UCF(ucf::LANDAREA, ctx.options) *
                                transport::reactionAreaFactor(rx.area_units);
            if (!(area > 0))
                continue;
            std::fill(forms.begin(), forms.end(), ReactionExprForm::NONE);
            bool any = false;
            for (int selected : {-1, sc})
                for (const auto& l : rx.buildup_laws)
                    if (l.landuse == lu && l.subcatchment == selected) {
                        forms[l.species] = l.form;
                        expr[l.species] = l.compiled;
                        any = true;
                    }
            if (!any)
                continue;
            bool supported = true;
            for (int m = 0; m < ns; ++m) {
                const double norm = fraction * (s.bu_normalizer[s.pidx(lu, m)] == 0
                                                    ? ctx.subcatches.area[sc]
                                                    : ctx.subcatches.curb_length[sc]);
                normalizer[m] = norm;
                before[m] = y[m] =
                    norm > 0 ? s.buildup[s.bu_idx(sc, lu, m)] * norm / s.mcf[m] / area : 0.;
                // A product cannot be retained with a zero-length curb normalizer.
                if (forms[m] != ReactionExprForm::NONE && !(norm > 0))
                    supported = false;
            }
            double vars[static_cast<int>(RxHydVar::COUNT_)] = {};
            vars[static_cast<int>(RxHydVar::TEMP)] = rx.default_temp_c;
            const auto report = supported ? transport::ReactionIntegrator::step(rx, forms, expr, dt,
                                                                                y.data(), vars, ws)
                                          : transport::RxStepReport{};
            bool physical = supported && report.ok;
            for (int m = 0; m < ns && physical; ++m)
                physical = std::isfinite(y[m]) && (y[m] >= 0 || forms[m] == ReactionExprForm::RATE);
            if (!physical) {
                if (!rx.warned_react_failure) {
                    rx.warned_react_failure = true;
                    ctx.warnings.push_back(
                        "Dry buildup reaction failed or has a zero normalizer; original inventory retained.");
                }
                continue;
            }
            for (int m = 0; m < ns; ++m) {
                const double bounded = std::max(0., y[m]), factor = area * s.mcf[m];
                s.led_reacted[m] += (before[m] - y[m]) * factor;
                s.led_reaction_clamped[m] += (bounded - y[m]) * factor;
                if (y[m] != bounded)
                    transport::noteNegativeClamp(ctx, "a dry buildup store");
                if (normalizer[m] > 0)
                    s.buildup[s.bu_idx(sc, lu, m)] = bounded * factor / normalizer[m];
            }
        }
}

} // namespace

/**
 * @brief Advance native dry/wet species reactions and shared surface buildup/washoff once.
 * @param[in,out] ctx Active resolved surface species, final parameters and completed runoff
 * hydraulics; receives inventories/concentrations, user-mass ledgers and possible reaction
 * warnings.
 * @param[in] dt_runoff Positive finite accepted runoff duration s; inactive/nonpositive/NaN
 * returns.
 * @param[in] old_runoff_time Runoff step start in elapsed simulation seconds for external
 * buildup/sweeps.
 * @note Materializes parameters, reacts dry buildup, optionally reacts complete ponded pools
 * then passes borrowed synchronous views to SurfaceSpeciesKernel. Caller enforces quality
 * enable/phase, matching extents and exactly-once chronology; no water or clock advance.
 * Allocation or reaction errors may leave earlier inventory/ledger changes; no global rollback
 * or thread-safe shared access.
 */
void step(SimulationContext& ctx, double dt_runoff, double old_runoff_time) {
    auto& s = ctx.reactions.surface;
    if (!s.active() || !(dt_runoff > 0.0))
        return;
    const auto p = speciesParams(s);
    auto b = speciesBlock(ctx, s, p);
    reactBuildup(ctx, dt_runoff);
    std::vector<double> reacted;
    std::vector<uint8_t> wet_mask(static_cast<std::size_t>(ctx.n_subcatches()));
    if (ctx.reactions.has_subcatch_reactions()) {
        reacted = reactPondedPools(ctx, dt_runoff);
        b.ponded_reacted_mass = &reacted;
        for (int sc = 0; sc < ctx.n_subcatches(); ++sc)
            wet_mask[sc] = ctx.reactions.wet_enabled(sc);
        b.ponded_reactor_mask = &wet_mask;
    }
    surfacespecies::step(ctx, b, dt_runoff, old_runoff_time);
}

/**
 * @brief Book native species deposition/infiltration and runoff after conventional LID routing.
 * @param[in,out] ctx Active resolved native surface stores and matching accepted LID/runoff
 * hydraulics; receives shared-kernel user-mass ledgers.
 * @param[in] dt_runoff Matching positive finite runoff interval s, caller qualified.
 * @param[in] lid_infil_vol Per-subcatchment accepted LID infiltration volumes ft3, covered for
 * all sources.
 * @note Inactive/no-subcatchment returns. Builds temporary parameter vectors and borrows
 * synchronous views; shared kernel controls ignored-quality handling. No water/clock advance;
 * exactly-once post-LID phase and exclusive state required. Allocations/partial ledger changes
 * may throw.
 */
void bookPostLid(SimulationContext& ctx, double dt_runoff,
                 const std::vector<double>& lid_infil_vol) {
    auto& s = ctx.reactions.surface;
    if (!s.active() || ctx.n_subcatches() <= 0)
        return;
    const auto p = speciesParams(s);
    auto b = speciesBlock(ctx, s, p);
    surfacespecies::bookPostLid(ctx, b, dt_runoff, lid_infil_vol);
}

/**
 * @brief Queue one conventional LID underdrain species export and optional runoff/BMP mass
 * bookings.
 * @param[in,out] ctx Active surface concentrations, removal fractions, conversion factors and
 * ledgers.
 * @param[in] control LID control ID; missing/invalid removal tuple uses fraction0, without
 * clamping.
 * @param[in] sc Valid source subcatchment ID, required when source is null or source-ledger
 * booking occurs.
 * @param[in] node Native destination node ID; invalid returns before state access.
 * @param[in] drain_cfs Accepted underdrain flow ft3/s, caller validates sign/finite value.
 * @param[in] to_perv True suppresses runoff/BMP/user-load bookings; destination queue still
 * receives rate.
 * @param[in] runoff_load True books positive post-removal load as runoff when not routed to
 * pervious land.
 * @param[in] dt_runoff Matching accepted runoff duration s used only for mass bookings.
 * @param[in] source Optional borrowed n_species underdrain concentrations; null uses source
 * washoff. When supplied, books source total load as well. Must remain stable and not alias
 * writable ledgers.
 * @note User-mass bookings use drain*c*dt*28.317*mcf. Does not transfer water or change clock;
 * exactly-once accepted export and exclusive queue/context ownership required. No generic
 * extent/physical validation. Node queue is resized/reset when shape differs, then adds
 * concentration*ft3/s without litre factor. Allocation inside noexcept terminates on failure;
 * reset can discard previous mismatched queue.
 */
void addLidDrain(SimulationContext& ctx, int control, int sc, int node, double drain_cfs,
                 bool to_perv, bool runoff_load, double dt_runoff, const double* source) noexcept {
    auto& s = ctx.reactions.surface;
    if (!s.active() || node < 0 || node >= ctx.n_nodes())
        return;
    const int nm = s.n_species;
    const auto want = static_cast<std::size_t>(ctx.n_nodes()) * static_cast<std::size_t>(nm);
    if (s.lid_drain_load.size() != want)
        s.lid_drain_load.assign(want, 0.0);
    // The pollutant lines in the A6b drain loop, species side: the drain
    // carries the subcatchment's washoff concentration less the unit's
    // removal, and books the removed share as BMP removal.
    for (int m = 0; m < nm; ++m) {
        const auto rk = static_cast<std::size_t>(control) * static_cast<std::size_t>(nm) +
                        static_cast<std::size_t>(m);
        const double rmvl = (control >= 0 && rk < s.lid_rmvl.size()) ? s.lid_rmvl[rk] : 0.0;
        const double c_src = source ? source[m] : s.washoff_conc[s.sidx(sc, m)];
        s.lid_drain_load[static_cast<std::size_t>(node) * static_cast<std::size_t>(nm) +
                         static_cast<std::size_t>(m)] += drain_cfs * c_src * (1.0 - rmvl);
        if (!to_perv) {
            const double w =
                drain_cfs * c_src * dt_runoff * 28.317 * s.mcf[static_cast<std::size_t>(m)];
            if (source)
                s.led_subcatch_load[s.sidx(sc, m)] += w;
            if (w > 0.0) {
                s.led_bmp_removal[static_cast<std::size_t>(m)] += rmvl * w;
                if (runoff_load)
                    s.led_runoff_load[static_cast<std::size_t>(m)] += w * (1.0 - rmvl);
            }
        }
    }
}

/**
 * @brief Queue one conventional LID underdrain species export and optional runoff/BMP mass
 * bookings.
 * @param[in,out] ctx Active surface concentrations, removal fractions, conversion factors and
 * ledgers.
 * @param[in] control LID control ID; missing/invalid removal tuple uses fraction0, without
 * clamping.
 * @param[in] sc Valid source subcatchment ID, required when source is null or source-ledger
 * booking occurs.
 * @param[in] receiver Destination subcatchment ID; invalid returns. Covered run-on tuples
 * accumulate concentration-mass/s using28.317L/ft3; queue is not resized here.
 * @param[in] drain_cfs Accepted underdrain flow ft3/s, caller validates sign/finite value.
 * @param[in] to_perv True suppresses runoff/BMP/user-load bookings; destination queue still
 * receives rate.
 * @param[in] runoff_load True books positive post-removal load as runoff when not routed to
 * pervious land.
 * @param[in] dt_runoff Matching accepted runoff duration s used only for mass bookings.
 * @param[in] source Optional borrowed n_species underdrain concentrations; null uses source
 * washoff. When supplied, books source total load as well. Must remain stable and not alias
 * writable ledgers.
 * @note User-mass bookings use drain*c*dt*28.317*mcf. Does not transfer water or change clock;
 * exactly-once accepted export and exclusive queue/context ownership required. No generic
 * extent/physical validation. Allocation-free; uncovered destination tuples are skipped while
 * mass bookings may still proceed.
 */
void addLidDrainRunon(SimulationContext& ctx, int control, int sc, int receiver, double drain_cfs,
                      bool to_perv, bool runoff_load, double dt_runoff,
                      const double* source) noexcept {
    auto& s = ctx.reactions.surface;
    if (!s.active() || receiver < 0 || receiver >= ctx.n_subcatches())
        return;
    const int nm = s.n_species;
    for (int m = 0; m < nm; ++m) {
        const auto rk = static_cast<std::size_t>(control) * static_cast<std::size_t>(nm) +
                        static_cast<std::size_t>(m);
        const double rmvl = (control >= 0 && rk < s.lid_rmvl.size()) ? s.lid_rmvl[rk] : 0.0;
        const double c_src = source ? source[m] : s.washoff_conc[s.sidx(sc, m)];
        const auto k = s.sidx(receiver, m);
        if (k < s.lid_drain_runon_load.size())
            s.lid_drain_runon_load[k] += (drain_cfs * c_src * 28.317) * (1.0 - rmvl);
        if (!to_perv) {
            const double w =
                drain_cfs * c_src * dt_runoff * 28.317 * s.mcf[static_cast<std::size_t>(m)];
            if (source)
                s.led_subcatch_load[s.sidx(sc, m)] += w;
            if (w > 0.0) {
                s.led_bmp_removal[static_cast<std::size_t>(m)] += rmvl * w;
                if (runoff_load)
                    s.led_runoff_load[static_cast<std::size_t>(m)] += w * (1.0 - rmvl);
            }
        }
    }
}

/**
 * @brief Move queued LID underdrain species run-on into one receiver and clear that queue.
 * @param[in,out] ctx Active surface store with matching runon and underdrain queue extents.
 * @param[in] receiver Valid nonnegative subcatchment ID, with products fitting indexing types.
 * @note For covered tuples adds concentration-mass/s then zeros queue; inactive returns. No
 * allocation, water/clock change or mass conversion. Caller serializes queue
 * production/consumption and consumes once before the receiver's runoff interval; uncovered
 * tuple remains queued.
 */
void consumeLidDrainRunon(SimulationContext& ctx, int receiver) noexcept {
    auto& s = ctx.reactions.surface;
    if (!s.active())
        return;
    for (int m = 0; m < s.n_species; ++m) {
        const auto k = s.sidx(receiver, m);
        if (k < s.lid_drain_runon_load.size() && k < s.runon_qual_rate.size()) {
            s.runon_qual_rate[k] += s.lid_drain_runon_load[k];
            s.lid_drain_runon_load[k] = 0.0;
        }
    }
}

/**
 * @brief Retain current node underdrain species rates as previous step then clear current rates.
 * @param[in,out] ctx Surface current/old node*species underdrain rate arrays, even when
 * inactive.
 * @note Copies concentration*ft3/s and clears only current node rates, not subcatchment run-on
 * queue. No physical water/clock/ledger change. Exclusive once-per-runoff chronology; vector
 * copy may allocate inside noexcept, terminating on allocation failure.
 */
void rollLidDrain(SimulationContext& ctx) noexcept {
    auto& s = ctx.reactions.surface;
    s.lid_drain_load_old = s.lid_drain_load;
    std::fill(s.lid_drain_load.begin(), s.lid_drain_load.end(), 0.0);
}

/**
 * @brief Sum retained surface and conventional-LID species inventory for final user-mass
 * reporting.
 * @param[in] ctx Stable matching coverage, user-unit geometry, normalizers, conversion factors
 * and surface/conventional-LID storage; all parallel configured extents prepared.
 * @param[in] m Zero-based native species; inactive/invalid yields0.
 * @return User mass from covered normalized dry buildup, ponded mass*mcf, conventional LID
 * layer/carry native mass*28.317*mcf and retained ponded-loss ledger. Does not include native
 * LID-node stores or undelivered node/run-on queues. Read-only, no allocation or receipt
 * consumption; consistent phase required.
 */
double remainingBuildup(const SimulationContext& ctx, int m) noexcept {
    const auto& s = ctx.reactions.surface;
    if (!s.active() || m < 0 || m >= s.n_species)
        return 0.0;
    double total = 0.0;
    const int nlu = s.n_landuses;
    for (int sc = 0; sc < ctx.n_subcatches(); ++sc) {
        const auto usc = static_cast<std::size_t>(sc);
        for (int lu = 0; lu < nlu; ++lu) {
            const auto cov = usc * static_cast<std::size_t>(nlu) + static_cast<std::size_t>(lu);
            const double frac =
                cov < ctx.subcatches.coverage.size() ? ctx.subcatches.coverage[cov] / 100.0 : 0.0;
            if (frac <= 0.0)
                continue;
            const double norm = (s.bu_normalizer[s.pidx(lu, m)] == 0)
                                    ? frac * ctx.subcatches.area[usc]
                                    : frac * ctx.subcatches.curb_length[usc];
            const auto k = s.bu_idx(sc, lu, m);
            if (k < s.buildup.size())
                total += s.buildup[k] * norm;
        }
        const auto pk = s.sidx(sc, m);
        if (pk < s.ponded_qual.size())
            total += s.ponded_qual[pk] * s.mcf[static_cast<std::size_t>(m)];
    }
    for (std::size_t k = std::size_t(m); k < ctx.reactions.lid.mass.size();
         k += std::size_t(s.n_species))
        total += ctx.reactions.lid.mass[k] * 28.317 * s.mcf[m];
    for (std::size_t k = std::size_t(m); k < ctx.reactions.lid.surface_carry.size();
         k += std::size_t(s.n_species))
        total += ctx.reactions.lid.surface_carry[k] * 28.317 * s.mcf[m];
    if (static_cast<std::size_t>(m) < s.led_ponded_lost.size())
        total += s.led_ponded_lost[static_cast<std::size_t>(m)];
    return total;
}

/**
 * @brief Clear every native surface species run-on mass-rate entry before assembly.
 * @param[in,out] ctx Writable surface runon_qual_rate, regardless of active flag.
 * @note No allocation, other queues/ledgers, water or clock changes. Caller owns exclusive
 * assembly phase; clearing after addRunon/consumeLidDrainRunon discards those pending rates.
 */
void clearRunon(SimulationContext& ctx) noexcept {
    auto& s = ctx.reactions.surface;
    std::fill(s.runon_qual_rate.begin(), s.runon_qual_rate.end(), 0.0);
}

/**
 * @brief Add donor previous-runoff species concentration to one receiver's run-on mass rate.
 * @param[in,out] ctx Active surface old washoff concentration and writable runon arrays,
 * matching dimensions.
 * @param[in] donor Valid nonnegative subcatchment index; source tuple must be covered.
 * @param[in] receiver Valid nonnegative destination subcatchment index; destination tuple must
 * be covered.
 * @param[in] q Routed runoff ft3/s; sign/finite value is not validated or clamped.
 * @note Adds q*old_concentration*28.317L/ft3 for covered tuples; inactive returns. Does not
 * clear queues, move hydraulic water, book global mass or advance clock. No allocation;
 * exclusive assembly access.
 */
void addRunon(SimulationContext& ctx, int donor, int receiver, double q) noexcept {
    auto& s = ctx.reactions.surface;
    if (!s.active())
        return;
    // The pollutant line in assembleRunon, species side: legacy
    // subcatch.c:553, newQual += oldRunoff * oldQual * LperFT3.
    for (int m = 0; m < s.n_species; ++m) {
        const auto src = s.sidx(donor, m), dst = s.sidx(receiver, m);
        if (src < s.washoff_conc_old.size() && dst < s.runon_qual_rate.size())
            s.runon_qual_rate[dst] += q * s.washoff_conc_old[src] * 28.317;
    }
}

/*
 * @brief Add time-interpolated surface and LID drain species mass rates to native node inflows.
 * @param[in,out] ctx Active surface/native species with matching counts, valid outlet identities
 * and old/new runoff/underdrain histories; adds to msx_ext_mass_in[node*species+m].
 * @note Routing duration is unused. Uses f=runoff_interp_f without clipping: (1-f)*q_old*c_old+
 * f*q_new*c_new, rather than multiplying mean flow by new concentration. Positive interpolated
 * water gates each source; only positive mass-rate blends are added. No28.317 conversion here:
 * native rate convention is concentration*ft3/s. Mismatched destination shape is reset0,
 * otherwise existing assembled rates retained. Call once after external assembly clears
 * destination. No hydraulic water/clock/global-ledger change; allocation can throw and later
 * sources may follow partial additions.
 */
void deliver(SimulationContext& ctx, double /*dt_routing*/) {
    auto& s = ctx.reactions.surface;
    if (!s.active())
        return;
    auto& rx = ctx.reactions;
    const int nm = rx.n_species();
    if (nm != s.n_species || ctx.n_nodes() <= 0)
        return;
    const auto want = static_cast<std::size_t>(ctx.n_nodes()) * static_cast<std::size_t>(nm);
    if (rx.msx_ext_mass_in.size() != want)
        rx.msx_ext_mass_in.assign(want, 0.0);
    for (int i = 0; i < ctx.n_subcatches(); ++i) {
        const auto ui = static_cast<std::size_t>(i);
        const int out_node = ctx.subcatches.outlet_node[ui];
        if (out_node < 0 || out_node >= ctx.n_nodes())
            continue;
        // addWetWeatherLoads, species side. Legacy weights the runoff flow and
        // the load beside it by where THIS routing instant falls between the
        // two runoff times; the fixed midpoint that used to stand here
        // delivered the storm's average every routing step instead of its
        // schedule. The pollutant kernel carries the same two lines and
        // LoadReachesTheNetworkUnderBothSolvers holds the two together.
        const double f = ctx.runoff_interp_f;
        const double f1 = 1.0 - f;
        const double q_old = ctx.subcatches.old_runoff[ui];
        const double q_new = ctx.subcatches.runoff[ui];
        const double q = f1 * q_old + f * q_new;
        if (q <= 0.0)
            continue;
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
        const double f = ctx.runoff_interp_f;
        const double f1 = 1.0 - f;
        const bool has_old = s.lid_drain_load_old.size() == want;
        for (int j = 0; j < ctx.n_nodes(); ++j) {
            const auto uj = static_cast<std::size_t>(j);
            const double v_new =
                uj < ctx.nodes.lid_drain_qual_vol.size() ? ctx.nodes.lid_drain_qual_vol[uj] : 0.0;
            const double v_old = uj < ctx.nodes.lid_drain_qual_vol_old.size()
                                     ? ctx.nodes.lid_drain_qual_vol_old[uj]
                                     : 0.0;
            if (f1 * v_old + f * v_new <= 0.0)
                continue;
            for (int m = 0; m < nm; ++m) {
                const auto k = uj * static_cast<std::size_t>(nm) + static_cast<std::size_t>(m);
                const double w_new = s.lid_drain_load[k];
                const double w_old = has_old ? s.lid_drain_load_old[k] : 0.0;
                const double load = f1 * w_old + f * w_new;
                if (load > 0.0)
                    rx.msx_ext_mass_in[k] += load;
            }
        }
    }
}

} // namespace openswmm::msxsurf
