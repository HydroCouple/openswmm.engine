// SPDX-License-Identifier: Apache-2.0

/**
 * @file      GwSourceResolver.cpp
 * @brief     Resolution of groundwater source prescriptions and ownership.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "GwSourceResolver.hpp"
#include "GwTransportData.hpp"
#include "../data/MeshData.hpp"
#include "../subsurface/SubsurfaceData.hpp"
#include "../subsurface/SubsurfaceTransportState.hpp"
#include "../../core/SimulationContext.hpp"
#include <stdexcept>
#include <unordered_map>
namespace openswmm::twoD {
/**
 * @brief Resolve a finite constant or copy/share a strictly ordered model time series.
 * @param[in,out] ctx Context supplying named time-series tables and absolute simulation start
 * serial. File-backed table cache loading can mutate ctx.tables and perform I/O.
 * @param[in] value Finite constant in caller-defined physical units, used only when name is
 * empty.
 * @param[in] name Optional exact model time-series name. A nonempty name takes precedence over
 * value.
 * @param[in,out] cache Call-scoped named signal cache for this same context/start-date/model.
 * Existing exact-name hits are trusted and copied; newly validated series are inserted only
 * after complete loading and conversion. Do not reuse across differing model identities or
 * edits.
 * @param[in] label Nonnull diagnostic section prefix used in exceptions.
 * @return Owned signal value; constants have no time knots, named series share an immutable
 * copied series, with times in elapsed seconds relative to ctx.options.start_date and
 * hasNegative metadata.
 * @throws std::runtime_error On invalid constant, unknown/empty series, nonfinite or
 * nonincreasing knots, multiple value columns, invalid/short cache or file read failure.
 * @note Named series ordinate units are unchanged. Allocation failures propagate. A failure can
 * change the context's file cache but does not insert an incomplete named signal into cache.
 * Returned series remain valid independently of future model-table cache replacement.
 */
GwSourceSignal resolveGwSignal(SimulationContext& ctx, double value, const std::string& name,
                               std::unordered_map<std::string, GwSourceSignal>& cache,
                               const char* label) {
    auto error = [&](const std::string& message) {
        throw std::runtime_error(std::string(label) + " " + message);
    };

    if (name.empty()) {
        if (!std::isfinite(value))
            error("nonfinite constant value");
        GwSourceSignal out;
        out.constant = value;
        return out;
    }
    if (auto i = cache.find(name); i != cache.end())
        return i->second;
    const int index = ctx.find_timeseries(name);
    if (index < 0)
        error("unknown series '" + name + "'");
    auto& table = ctx.tables.tables[static_cast<std::size_t>(index)];
    GwSourceSignal out;
    auto append = [&](double x, double y) {
        const double t = (x - ctx.options.start_date) * 86400.;
        if (!std::isfinite(t) || !std::isfinite(y) || (!out.times.empty() && t <= out.times.back()))
            error("series '" + name +
                  "' needs finite, strictly increasing times and finite values");
        out.times.push_back(t);
        out.values.push_back(y);
    };
    if (table.is_file_based) {
        if (table.num_cols != 1)
            error("source series '" + name + "' must have one value column");
        for (std::size_t row = 0; row < table.total_rows;) {
            if (!table_load_cache(table, row))
                error("cannot read source series '" + name + "'");
            const auto& block = table.cache;
            if (row < block.file_row_start || row >= block.file_row_start + block.x.size())
                error("invalid source series cache '" + name + "'");
            const auto offset = row - block.file_row_start;
            for (std::size_t j = offset; j < block.x.size(); ++j) {
                if (j >= block.y.size())
                    error("short source series '" + name + "'");
                append(block.x[j], block.y[j]);
                ++row;
            }
        }
    } else {
        if (table.x.size() != table.y.size())
            error("source series '" + name + "' must have one value column");
        for (std::size_t j = 0; j < table.x.size(); ++j)
            append(table.x[j], table.y[j]);
    }
    if (out.times.empty())
        error("source series '" + name + "' is empty");
    auto shared = std::make_shared<GwSourceSeries>();
    shared->times = std::move(out.times);
    shared->values = std::move(out.values);
    shared->hasNegative =
        std::any_of(shared->values.begin(), shared->values.end(), [](double v) { return v < 0; });
    out.series = std::move(shared);
    cache.emplace(name, out);
    return out;
}
/**
 * @brief Resolve authored groundwater sources to normalized active-cell shares and native
 * signals.
 * @param[in,out] ctx Model units, time-series tables and species metadata; file-backed signal
 * resolution can load/mutate table caches and perform I/O.
 * @param[in] mesh Stable mesh tags; a cell beyond tri_tag.size() cannot match a TAG selector.
 * @param[in] gw Authored source rows, including finite nonnegative SCALE and CELL/TAG/global
 * scopes.
 * @param[in] state Prepared hydraulic state; positive cell area and aquifer depth define active
 * receivers. Per-cell area/zs arrays must cover state.n_cells.
 * @param[in] tr Prepared transport row identity, pollutant count and optional age/temperature
 * rows.
 * @return Owned resolved sources in authored order. Cell shares are normalized by positive
 * matched area; flow is total m3/s, CONC uses row-native concentration/age/temperature units,
 * and pollutant MASS uses native amount/s with .001 conversion to native_concentration*m3/s.
 * @throws std::runtime_error On negative/nonfinite scale, no matched active cell, disabled or
 * duplicate species, unsupported term kind, MASS on non-pollutant rows, invalid signal, or
 * negative concentration/mass rate. Temperature CONC alone may be signed.
 * @note Zero scale is valid and later integrates no forcing. Sources need not prescribe every
 * row's composition here; unspecified rows receive no source loading. Model rows are unchanged;
 * local cache shares copied immutable series across sources. Allocation/I/O exceptions propagate
 * before any partial resolved vector is published.
 */
std::vector<GwResolvedSource> resolveGwSources(SimulationContext& ctx, const MeshData& mesh,
                                               const GwTransportData& gw,
                                               const SubsurfaceState& state,
                                               const SubsurfaceTransportState& tr) {
    std::vector<GwResolvedSource> result;
    auto error = [](const std::string& s) { throw std::runtime_error("[GW_SOURCES] " + s); };
    std::unordered_map<std::string, GwSourceSignal> cache;
    auto signal = [&](double value, const std::string& name) {
        return resolveGwSignal(ctx, value, name, cache, "[GW_SOURCES]");
    };
    std::unordered_map<std::string, std::vector<int>> cellsByTag;
    std::vector<int> activeCells;
    for (int cell = 0; cell < state.n_cells; ++cell) {
        const auto c = static_cast<std::size_t>(cell);
        if (!(state.area[c] > 0) || !(state.zs[c] > 0))
            continue;
        activeCells.push_back(cell);
        if (c < mesh.tri_tag.size())
            cellsByTag[mesh.tri_tag[c]].push_back(cell);
    }
    for (const auto& row : gw.sources) {
        GwResolvedSource source;
        source.name = row.name;
        source.scale = row.scale;
        if (!std::isfinite(source.scale) || source.scale < 0)
            error("SCALE must be finite and nonnegative");
        source.flow = signal(row.flow, row.flow_ts);
        double area = 0;
        auto addCell = [&](int cell) {
            if (cell < 0 || cell >= state.n_cells)
                return;
            const auto c = static_cast<std::size_t>(cell);
            if (!(state.area[c] > 0) || !(state.zs[c] > 0))
                return;
            source.cells.push_back({cell, state.area[c]});
            area += state.area[c];
        };
        if (row.scope == GwScope::CELL)
            addCell(row.cell);
        else if (row.scope == GwScope::TAG) {
            if (auto it = cellsByTag.find(row.tag); it != cellsByTag.end())
                for (int cell : it->second)
                    addCell(cell);
        } else
            for (int cell : activeCells)
                addCell(cell);
        if (!(area > 0))
            error("source '" + row.name + "' matches no active aquifer cells");
        for (auto& cell : source.cells)
            cell.second /= area;
        std::vector<int> seen;
        for (const auto& term : row.species) {
            GwResolvedSourceTerm resolved;
            resolved.row = tr.rowIndex(term.species);
            if (resolved.row < 0)
                error("source '" + row.name + "' species '" + term.species +
                      "' is not enabled in groundwater");
            if (std::find(seen.begin(), seen.end(), resolved.row) != seen.end())
                error("duplicate species term in source '" + row.name + "'");
            seen.push_back(resolved.row);
            if (term.kind != "CONC" && term.kind != "MASS")
                error("unsupported species source kind");
            resolved.massRate = term.kind == "MASS";
            if (resolved.massRate) {
                if (resolved.row >= tr.n_pollut)
                    error(
                        "MASS requires a pollutant with declared native mass units; use CONC for age, temperature or unresolved MSX units");
                resolved.massScale = .001; // mg/L, ug/L, count/L × m3; one m3 is 1000 L
            }
            resolved.signal = signal(term.value, term.ts_name);
            const bool signedValue = !resolved.massRate && resolved.row == tr.temp_row;
            if (!signedValue) {
                if (resolved.signal.constant < 0)
                    error("negative source concentration or mass rate");
                if (resolved.signal.series && resolved.signal.series->hasNegative)
                    error("negative source concentration or mass rate in series");
            }
            source.terms.push_back(std::move(resolved));
        }
        result.push_back(std::move(source));
    }
    return result;
}
} // namespace openswmm::twoD
