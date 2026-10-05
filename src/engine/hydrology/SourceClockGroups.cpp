// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include "SourceClockGroups.hpp"
#include "Gage.hpp"
#include "../core/SimulationContext.hpp"
#include "../core/UnitConversion.hpp"
#include "../core/DateTime.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace openswmm::runoff {
namespace {
bool nonnegative(double x) { return std::isfinite(x) && x >= 0.0; }
std::size_t at(const std::vector<double>& times, double t) {
    return static_cast<std::size_t>(std::upper_bound(times.begin(), times.end(), t) - times.begin());
}
double next(const std::vector<double>& times, double t, double end) {
    const auto p = at(times, t);
    return p < times.size() ? std::min(end, times[p]) : end;
}
}

std::string SourceClockGroups::initialize(const SimulationContext& ctx, const std::vector<int>& seeds) {
    for (const auto& g : groups_)
        if (!g.pending.empty() || g.completed_end > 0.0)
            return "Pending or committed source clocks require a fresh adapter for a new simulation.";
    SourceClockGroups result;
    const int n = ctx.n_subcatches();
    std::vector<int> parent(static_cast<std::size_t>(n));
    std::iota(parent.begin(), parent.end(), 0);
    const auto root = [&](int i) {
        int r = i; while (parent[r] != r) r = parent[r];
        while (parent[i] != i) { const int p = parent[i]; parent[i] = r; i = p; }
        return r;
    };
    const auto join = [&](int a, int b) { parent[root(a)] = root(b); };
    for (int i = 0; i < n; ++i) {
        const int target = ctx.subcatches.outlet_subcatch[i];
        if (target < -1 || target >= n) return "Invalid runoff graph destination.";
        if (target >= 0 && target != i) join(i, target);
    }
    for (int u = 0; u < ctx.lid_usage.count(); ++u) {
        const int sc = ctx.lid_usage.subcatch_index[u];
        if (sc < 0 || sc >= n) return "Invalid LID graph parent.";
        if (u >= static_cast<int>(ctx.lid_usage.drain_to.size()) || ctx.lid_usage.drain_to[u].empty()) continue;
        const auto& target = ctx.lid_usage.drain_to[u];
        if (ctx.node_names.find(target) >= 0) continue; // Same precedence as LIDSolver.
        const int dest = ctx.subcatch_names.find(target);
        if (dest < 0 || dest >= n) return "Unresolved LID drain graph destination: " + target;
        if (dest != sc) join(sc, dest);
    }
    std::vector<bool> selected(static_cast<std::size_t>(n), false);
    for (int sc : seeds) {
        if (sc < 0 || sc >= n) return "Invalid spatial source graph seed.";
        selected[root(sc)] = true;
    }
    result.source_group_.assign(static_cast<std::size_t>(n), -1);
    std::vector<int> root_group(static_cast<std::size_t>(n), -1);
    for (int sc = 0; sc < n; ++sc) {
        const int r = root(sc);
        if (!selected[r]) continue;
        if (root_group[r] < 0) {
            root_group[r] = static_cast<int>(result.groups_.size());
            result.groups_.push_back({});
        }
        const int g = root_group[r];
        result.groups_[g].subcatches.push_back(sc);
        result.source_group_[sc] = g;
    }
    for (int u = 0; u < ctx.lid_usage.count(); ++u) {
        const int g = result.source_group_[ctx.lid_usage.subcatch_index[u]];
        if (g >= 0) result.groups_[g].lid_usages.push_back(u);
    }
    result.initialized_ = true;
    if (result.groups_.empty()) { *this = std::move(result); return {}; }

    // These source kernels need separate qualification, not an inferred rate
    // from a future global state. Keep a named refusal until those adapters land.
    if (!ctx.options.ignore_snow_melt) return "Source clock forcing currently requires IGNORE_SNOWMELT YES; snow forcing is not qualified.";
    if (ctx.files.runoff_mode == FileMode::USE) return "Completed source clocks cannot use precomputed USE RUNOFF losses.";
    for (int sc : ctx.node_subtypes.outfalls.route_to)
        if (sc >= 0 && result.groupForSubcatch(sc) >= 0)
            return "Source clock graph has an unqualified hydraulic outfall return.";
    result.evap_method_ = ctx.climate_state.evap_method;
    if (result.evap_method_ != climate::EvapMethod::CONSTANT &&
        result.evap_method_ != climate::EvapMethod::MONTHLY &&
        result.evap_method_ != climate::EvapMethod::TIMESERIES)
        return "Source clock forcing supports constant, monthly or time-series PET; temperature/pan history is not qualified.";
    result.start_date_ = ctx.options.start_date;
    result.duration_ = ctx.options.totalDurationMs() / 1000.0;
    if (!std::isfinite(result.start_date_) || !std::isfinite(result.duration_) || result.duration_ <= 0.0)
        return "Invalid source clock simulation interval.";
    result.rain_ucf_ = ucf::UCF(ucf::RAINFALL, ctx.options);
    result.evap_ucf_ = ucf::UCF(ucf::EVAPRATE, ctx.options);
    result.ignore_rain_ = ctx.options.ignore_rainfall;
    result.dry_only_ = ctx.options.evap_dry_only;
    result.forcing_.resize(0, 0, n, 0, 0);
    result.forcing_.subcatch_rainfall_mode = ctx.forcing.subcatch_rainfall_mode;
    result.forcing_.subcatch_rainfall_value = ctx.forcing.subcatch_rainfall_value;
    result.forcing_.subcatch_evap_mode = ctx.forcing.subcatch_evap_mode;
    result.forcing_.subcatch_evap_value = ctx.forcing.subcatch_evap_value;
    result.forcing_.climate_evap_mode = ctx.forcing.climate_evap_mode;
    result.forcing_.climate_evap_value = ctx.forcing.climate_evap_value;
    if (ctx.forcing.climate_evap_mode != ForcingMode::NONE && ctx.forcing.climate_evap_persist != ForcingPersist::PERSIST)
        return "Source clock forcing needs timestamped updates for one-step climate prescriptions.";
    for (int m = 0; m < 12; ++m) {
        result.monthly_evap_[m] = ctx.climate_state.monthly_evap[m] / result.evap_ucf_;
        result.adjust_evap_[m] = ctx.adjust_evap[m] / result.evap_ucf_;
        result.adjust_rain_[m] = ctx.adjust_rain[m];
        result.infil_[m] = ctx.adjust_hydcon[m];
        result.recovery_[m] = 1.0;
        const int p = ctx.climate_state.recovery_pat_index;
        if (p >= 0 && p < static_cast<int>(ctx.patterns.factors.size()) && m < static_cast<int>(ctx.patterns.factors[p].size()))
            result.recovery_[m] = ctx.patterns.factors[p][m];
        if (!std::isfinite(result.monthly_evap_[m]) || !std::isfinite(result.adjust_evap_[m]) ||
            !nonnegative(result.adjust_rain_[m]) || !nonnegative(result.infil_[m]) || !nonnegative(result.recovery_[m]))
            return "Invalid source clock monthly forcing.";
    }
    result.source_gage_ = ctx.subcatches.gage;
    result.rain_scale_ = ctx.subcatches.rain_scale_factor;
    result.gages_.resize(static_cast<std::size_t>(ctx.n_gages()));
    std::vector<bool> needed(static_cast<std::size_t>(ctx.n_gages()), false);
    for (const auto& g : result.groups_) for (int sc : g.subcatches) {
        if (!nonnegative(result.rain_scale_[sc])) return "Invalid source rainfall scale.";
        const auto reset = [&](const auto& modes, const auto& persistence) {
            return sc < static_cast<int>(modes.size()) && modes[sc] != ForcingMode::NONE &&
                   (sc >= static_cast<int>(persistence.size()) || persistence[sc] != ForcingPersist::PERSIST);
        };
        if (reset(ctx.forcing.subcatch_rainfall_mode, ctx.forcing.subcatch_rainfall_persist) ||
            reset(ctx.forcing.subcatch_evap_mode, ctx.forcing.subcatch_evap_persist))
            return "Source clock forcing needs timestamped updates for one-step source prescriptions.";
        if (ctx.forcing.effective_snowfall(sc, 0.0) != 0.0) return "Prescribed snowfall is not qualified by source clocks.";
        const int gi = result.source_gage_[sc];
        if (gi < -1 || gi >= ctx.n_gages()) return "Invalid source rain gage.";
        if (gi >= 0) needed[gi] = true;
    }
    for (int gi = 0; gi < ctx.n_gages(); ++gi) {
        if (!needed[gi] || result.ignore_rain_) continue;
        auto& series = result.gages_[gi];
        if (gi < static_cast<int>(ctx.forcing.gage_rainfall_mode.size())) {
            series.mode = ctx.forcing.gage_rainfall_mode[gi];
            series.prescribed = ctx.forcing.gage_rainfall_value[gi];
            if (series.mode != ForcingMode::NONE && ctx.forcing.gage_rainfall_persist[gi] != ForcingPersist::PERSIST)
                return "Source clock forcing needs timestamped updates for one-step gage prescriptions.";
        }
        series.override_rate = ctx.gages.api_rainfall[gi];
        if (!std::isfinite(series.override_rate)) return "Invalid prescribed gage rainfall.";
        if (series.override_rate >= 0.0) {
            if (!nonnegative(series.override_rate)) return "Invalid prescribed gage rainfall.";
            continue;
        }
        const Table* table = gage::gageRainSeries(ctx, gi);
        if (!table || table->x.empty()) return "Unresolved source rain series.";
        series.period = ctx.gages.interval_sec[gi];
        if (!std::isfinite(series.period) || series.period <= 0.0 || table->x.size() != table->y.size() ||
            ctx.gages.rain_type[gi] < 0 || ctx.gages.rain_type[gi] > 2)
            return "Invalid source rain recording interval or series.";
        double previous = 0.0;
        for (std::size_t k = 0; k < table->x.size(); ++k) {
            const double t = (table->x[k] - result.start_date_) * 86400.0;
            const double r = gage::convertGageValue(table->y[k], ctx.gages.rain_type[gi], series.period,
                previous, gage::gageUnitsFactor(ctx, gi), ctx.gages.scale_factor[gi]);
            if (!std::isfinite(t) || (!series.times.empty() && t <= series.times.back()) || !nonnegative(r))
                return "Invalid or unordered source rain records.";
            series.times.push_back(t); series.rates.push_back(r);
        }
    }
    if (result.evap_method_ == climate::EvapMethod::TIMESERIES) {
        const int ti = ctx.climate_state.evap_ts_index;
        if (ti < 0 || ti >= static_cast<int>(ctx.tables.tables.size())) return "Unresolved source PET series.";
        const auto& table = ctx.tables.tables[ti];
        if (table.x.empty() || table.x.size() != table.y.size()) return "Invalid source PET series.";
        for (std::size_t k = 0; k < table.x.size(); ++k) {
            const double t = (table.x[k] - result.start_date_) * 86400.0;
            if (!std::isfinite(t) || (!result.evap_times_.empty() && t <= result.evap_times_.back()) || !nonnegative(table.y[k]))
                return "Invalid or unordered source PET records.";
            result.evap_times_.push_back(t); result.evap_values_.push_back(table.y[k] / result.evap_ucf_);
        }
    }
    *this = std::move(result);
    return {};
}

int SourceClockGroups::groupForSubcatch(int source) const {
    return source >= 0 && source < static_cast<int>(source_group_.size()) ? source_group_[source] : -1;
}

std::string SourceClockGroups::stage(int group, double end, double completed_through) {
    if (!initialized_ || group < 0 || group >= static_cast<int>(groups_.size())) return "Invalid source clock group.";
    auto& g = groups_[group];
    if (!g.pending.empty()) return "Source group already has a pending interval.";
    if (!std::isfinite(end) || !std::isfinite(completed_through) || end <= g.completed_end ||
        end > completed_through || end > duration_) return "Source interval must be completed, contiguous and within the simulation.";
    std::vector<SourceForcingInterval> intervals;
    for (double t = g.completed_end; t < end;) {
        const double date = datetime::addSeconds(start_date_, t);
        int year, mon, day; datetime::decodeDate(date, year, mon, day);
        const int m = mon - 1;
        const double month_end = (datetime::encodeDate(mon == 12 ? year + 1 : year, mon == 12 ? 1 : mon + 1, 1) - start_date_) * 86400.0;
        SourceForcingInterval f; f.start = t; f.end = std::min(end, month_end); f.month = m;
        f.infil_factor = infil_[m]; f.recovery_factor = recovery_[m];
        double pet = evap_method_ == climate::EvapMethod::MONTHLY ? monthly_evap_[m] : monthly_evap_[0];
        if (evap_method_ == climate::EvapMethod::TIMESERIES) {
            const auto p = at(evap_times_, t);
            pet = p > 0 ? evap_values_[p - 1] : 0.0;
            f.end = next(evap_times_, t, f.end);
        }
        pet = forcing_.effective_climate_evap(pet + adjust_evap_[m]);
        for (int sc : g.subcatches) {
            double rain = 0.0;
            const int gi = source_gage_[sc];
            if (!ignore_rain_ && gi >= 0) {
                const auto& s = gages_[gi];
                if (s.override_rate >= 0.0) rain = s.override_rate;
                else {
                    const auto p = at(s.times, t);
                    f.end = next(s.times, t, f.end);
                    if (p > 0) {
                        const double expiry = s.times[p - 1] + s.period;
                        if (t < expiry) { rain = s.rates[p - 1]; f.end = std::min(f.end, expiry); }
                    }
                }
                if (s.mode == ForcingMode::OVERRIDE) rain = s.prescribed;
                else if (s.mode == ForcingMode::ADD) rain += s.prescribed;
                rain *= adjust_rain_[m];
            }
            // Match splitPrecip's scale and unit order, then apply the
            // source prescription in the API's project rainfall units.
            rain = rain / rain_ucf_ * rain_scale_[sc];
            if (sc < static_cast<int>(forcing_.subcatch_rainfall_mode.size()) && forcing_.subcatch_rainfall_mode[sc] != ForcingMode::NONE)
                rain = forcing_.effective_rainfall(sc, rain * rain_ucf_) / rain_ucf_;
            const double source_pet = forcing_.effective_evap_rate(sc, dry_only_ && rain > 0.0 ? 0.0 : pet);
            if (!nonnegative(rain) || !nonnegative(source_pet)) return "Source interval has invalid resolved rain or PET.";
            f.sources.push_back({sc, rain, source_pet});
        }
        if (f.end <= t) return "Source forcing boundary failed to advance.";
        t = f.end; intervals.push_back(std::move(f));
    }
    g.pending = std::move(intervals);
    return {};
}

std::string SourceClockGroups::commit(int group) {
    if (group < 0 || group >= static_cast<int>(groups_.size()) || groups_[group].pending.empty())
        return "No pending source interval to commit.";
    auto& g = groups_[group]; g.completed_end = g.pending.back().end; g.pending.clear();
    return {};
}
void SourceClockGroups::cancel(int group) {
    if (group >= 0 && group < static_cast<int>(groups_.size())) groups_[group].pending.clear();
}
}
