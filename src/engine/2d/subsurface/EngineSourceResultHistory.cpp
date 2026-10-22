// SPDX-License-Identifier: Apache-2.0

/**
 * @file      EngineSourceResultHistory.cpp
 * @brief     Water-driver history associated with source-interval results.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "EngineSourceResultHistory.hpp"
#include "CommonSourceStage.hpp"
#include "../../core/SWMMEngine.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace openswmm::twoD {
namespace {
constexpr double ft3 = .3048 * .3048 * .3048;
/**
 * @brief Refuse an invalid engine-history condition with a diagnostic exception.
 * @param[in] ok True returns normally; false throws.
 * @param[in] error Nonnull diagnostic text copied into the exception.
 * @throws std::runtime_error On false; no local rollback occurs here.
 */
void require(bool ok, const char* error) {
    if (!ok)
        throw std::runtime_error(error);
}
/**
 * @brief Compare finite saved water diagnostics with a relative/absolute balance tolerance.
 * @param[in] a First value, in the same units as b.
 * @param[in] b Second value.
 * @return abs(a-b) <= 1e-10*max(1,abs(a),abs(b)). Finite-value validation belongs to the caller;
 * this helper performs no mutation or allocation.
 */
bool close(double a, double b) {
    return std::abs(a - b) <= 1e-10 * std::max({1., std::abs(a), std::abs(b)});
}
/**
 * @brief Append an engine diagnostic row in existing schema-column order.
 * @param[in,out] t Prepared table to append.
 * @param[in] values Exactly t.columns.size() entries, in schema units/order.
 * @throws std::runtime_error On wrong value count before appending. Allocation errors can leave
 * an earlier-column prefix appended; callers build a private candidate rather than live history.
 */
void row(SourceResultTable& t, std::initializer_list<double> values) {
    require(values.size() == t.columns.size(), "Engine source result row shape differs.");
    std::size_t c = 0;
    for (double v : values)
        t.columns[c++].push_back(v);
}
/**
 * @brief Test a stored floating-point identifier for integral membership in a bounded index
 * range.
 * @param[in] v Candidate value, normally already validated finite by the containing history.
 * @param[in] count Exclusive object count within the history limits.
 * @return True when v is integral and in [0,count), otherwise false. No mutation or allocation.
 */
bool id(double v, std::size_t count) {
    return v >= 0 && v < double(count) && std::floor(v) == v;
}
}
/**
 * @brief Build empty diagnostic tables on the accepted whole-engine-step time axis.
 * @param[in] returns True includes water-only outfall credits and pending return snapshots.
 * @param[in] lids True includes LID/aquifer bottom receipts, samples and available porous
 * profiles.
 * @return Owned map with ordered fields/units: elapsed time s, SWMM start-date days, water m3,
 * flow m3/s, area m2 and heads/depths m. Identifiers/counts are dimensionless.
 * @note No engine/source attachment or data validation occurs. Optional capabilities preserve
 * base column positions and add tables/columns; absent profile rows indicate unavailable data.
 */
std::map<std::string, SourceResultTable> EngineSourceResultHistory::schema(bool returns,
                                                                           bool lids) {
    std::map<std::string, SourceResultTable> tables;
    const auto add = [&](const char* name,
                         std::initializer_list<std::pair<const char*, const char*>> fields) {
        auto& t = tables[name];
        for (const auto& [field, unit] : fields) {
            t.fields.push_back(field);
            t.units.push_back(unit);
            t.columns.emplace_back();
        }
    };
    add("boundaries", {{"frame", "1"},
                       {"start", "s"},
                       {"end", "s"},
                       {"source_intervals", "1"},
                       {"source_end", "s"},
                       {"outfall_end", "s"},
                       {"start_date", "SWMM_date"},
                       {"mesh_rain", "m3"},
                       {"mesh_evaporation", "m3"},
                       {"mesh_return", "m3"}});
    add("source_water", {{"frame", "1"},
                         {"source", "1"},
                         {"group", "1"},
                         {"inside_fraction", "1"},
                         {"initial_storage", "m3"},
                         {"rain", "m3"},
                         {"evaporation", "m3"},
                         {"infiltration", "m3"},
                         {"spatial_infiltration", "m3"},
                         {"outside_infiltration", "m3"},
                         {"aquifer_return", "m3"},
                         {"outlet", "m3"},
                         {"captured", "m3"},
                         {"runon", "m3"},
                         {"pervious_return", "m3"},
                         {"storage", "m3"}});
    add("group_water", {{"frame", "1"}, {"group", "1"}, {"pending", "m3"}, {"residual", "m3"}});
    add("node_delivery", {{"frame", "1"},
                          {"node", "1"},
                          {"captured_runoff", "m3"},
                          {"captured_drain", "m3"},
                          {"delivered_runoff", "m3"},
                          {"delivered_drain", "m3"},
                          {"last_runoff", "m3"},
                          {"last_drain", "m3"},
                          {"last_start", "s"},
                          {"last_end", "s"},
                          {"last_rate", "m3/s"}});
    add("pending_delivery", {{"frame", "1"},
                             {"row", "1"},
                             {"ordinal", "1"},
                             {"source", "1"},
                             {"node", "1"},
                             {"kind", "1"},
                             {"source_start", "s"},
                             {"source_end", "s"},
                             {"available_at", "s"},
                             {"volume", "m3"}});
    if (returns) {
        auto& t = tables.at("source_water");
        t.fields.push_back("outfall_return");
        t.units.push_back("m3");
        t.columns.emplace_back();
        add("pending_outfall_return", {{"frame", "1"},
                                       {"row", "1"},
                                       {"source", "1"},
                                       {"available_at", "s"},
                                       {"volume", "m3"}});
    }
    if (lids) {
        add("lid_bottom", {{"frame", "1"},
                           {"node", "1"},
                           {"bed", "1"},
                           {"cell", "1"},
                           {"flow_model", "1"},
                           {"contact_area", "m2"},
                           {"surface_head", "m"},
                           {"aquifer_head", "m"},
                           {"aquifer_time", "s"},
                           {"down_cumulative", "m3"},
                           {"up_cumulative", "m3"},
                           {"net_cumulative", "m3"},
                           {"pending_down", "m3"},
                           {"pending_up", "m3"},
                           {"groundwater_residual", "m3"}});
        add("lid_bottom_sample", {{"frame", "1"},
                                  {"node", "1"},
                                  {"valid", "1"},
                                  {"start", "s"},
                                  {"end", "s"},
                                  {"aquifer_time", "s"},
                                  {"lid_head", "m"},
                                  {"aquifer_head", "m"},
                                  {"requested_down", "m3"},
                                  {"requested_up", "m3"},
                                  {"reserved_down", "m3"},
                                  {"reserved_up", "m3"},
                                  {"accepted_down", "m3"},
                                  {"accepted_up", "m3"},
                                  {"net_rate", "m3/s"}});
        add("lid_profile", {{"frame", "1"},
                            {"node", "1"},
                            {"cell", "1"},
                            {"layer", "1"},
                            {"bottom", "m"},
                            {"top", "m"},
                            {"moisture", "1"}});
        // Absent rows mean unavailable; the classical formulation has no
        // porous pressure profile. Surface HGL remains separately labelled.
        add("richards_profile", {{"frame", "1"},
                                 {"node", "1"},
                                 {"cell", "1"},
                                 {"pressure", "m"},
                                 {"head", "m"},
                                 {"water", "m3"}});
    }
    return tables;
}
/**
 * @brief Construct one engine-boundary water/queue projection with stable saved identities.
 * @par Engine argument Prepared accepted-boundary engine with a live common source stage and
 * consistent source delivery bridge or enabled LID bottom receipts. All borrowed subsystem
 * arrays must remain stable and internally sized during this synchronous read.
 * @param[in] frame Exactly representable nonnegative result frame identifier.
 * @param[in] start Beginning of this accepted engine interval in elapsed seconds; end uses ctx
 * time.
 * @return Owned one-frame projection of cumulative source water, groups, node delivery, pending
 * queues and optional LID/porous diagnostics. Does not advance clocks or consume any queue.
 * @throws std::runtime_error On receipt/schema failure or pending ordinal above 2^53-1.
 * Allocation errors propagate. Low-level helper does not validate complete model checkpoint
 * eligibility.
 * @note Converts 1D feet/ft3 quantities to SI and preserves source clocks independently of
 * engine steps; repeated source clocks are valid. No whole-engine checkpoint bytes/live pointer
 * are bound until the surrounding append/resume operation publishes the candidate.
 */
EngineSourceResultHistory EngineSourceResultHistory::projection(const SWMMEngine& e,
                                                                std::size_t frame, double start) {
    const auto& stage = *e.surface_router_.commonSourceStage();
    const auto& source = stage.sources();
    const auto* bridge = e.source_delivery_.get();
    const auto& ctx = e.ctx_;
    const auto& gw = e.surface_router_.subsurface();
    EngineSourceResultHistory out;
    out.outfall_returns_ = source.acceptsOutfallReturns();
    out.start_date_ = ctx.options.start_date;
    out.lid_beds_ = gw.lidReceiptsEnabled();
    out.tables_ = schema(out.outfall_returns_, out.lid_beds_);
    for (int sc = 0; sc < ctx.n_subcatches(); ++sc)
        out.sources_.push_back(ctx.subcatch_names.name_of(sc));
    for (int n = 0; n < ctx.n_nodes(); ++n)
        out.nodes_.push_back(ctx.node_names.name_of(n));
    auto& t = out.tables_;
    const double f = double(frame);
    row(t.at("boundaries"),
        {f, start, ctx.current_time, double(stage.intervals()), stage.completedEnd(),
         out.outfall_returns_ ? source.outfallReturnEnd() : -1, out.start_date_, stage.meshRain(),
         stage.meshEvaporation(), stage.meshReturn()});
    for (std::size_t g = 0; g < source.clocks().groups().size(); ++g) {
        row(t.at("group_water"), {f, double(g), source.pendingVolume(int(g)) * ft3,
                                  source.balanceResidual(int(g)) * ft3});
        for (int sc : source.clocks().groups()[g].subcatches) {
            const auto& b = source.ledgers()[sc];
            auto& water = t.at("source_water");
            // Same cumulative definitions as component results, on a different axis.
            const std::initializer_list<double> values{f,
                                                       double(sc),
                                                       double(g),
                                                       source.spatialFractions()[sc],
                                                       b.initial_storage * ft3,
                                                       b.rain * ft3,
                                                       b.evaporation * ft3,
                                                       b.infiltration * ft3,
                                                       b.spatial_infiltration * ft3,
                                                       b.outside_infiltration * ft3,
                                                       b.aquifer_return * ft3,
                                                       b.outlet * ft3,
                                                       b.captured * ft3,
                                                       b.runon * ft3,
                                                       b.pervious_return * ft3,
                                                       source.storedVolume(sc) * ft3};
            std::size_t c = 0;
            for (double v : values)
                water.columns[c++].push_back(v);
            if (out.outfall_returns_)
                water.columns[c].push_back(b.outfall_return * ft3);
        }
    }
    for (int n = 0; n < ctx.n_nodes(); ++n) {
        if (!bridge) {
            row(t.at("node_delivery"),
                {f, double(n), 0, 0, 0, 0, 0, 0, start, ctx.current_time, 0});
            continue;
        }
        const auto& captured = bridge->received()[n];
        const auto& delivered = bridge->delivered()[n];
        const auto& last = bridge->lastReceipts()[n];
        row(t.at("node_delivery"),
            {f, double(n), captured.runoff * ft3, captured.drain * ft3, delivered.runoff * ft3,
             delivered.drain * ft3, last.runoff * ft3, last.drain * ft3, bridge->lastStart(),
             bridge->receiverEnd(), last.rate * ft3});
    }
    if (bridge)
        for (const auto& q : bridge->pending()) {
            auto& pending = t.at("pending_delivery");
            const auto& d = q.water;
            require(q.ordinal <= 9007199254740991ULL,
                    "Source delivery ordinal exceeds exact result precision.");
            row(pending,
                {f, double(pending.rows()), double(q.ordinal), double(d.source), double(d.node),
                 double(d.drain), d.start, d.end, q.available, d.volume * ft3});
        }
    if (out.outfall_returns_)
        for (const auto& q : source.pendingOutfallReturns()) {
            auto& pending = t.at("pending_outfall_return");
            row(pending,
                {f, double(pending.rows()), double(q.source), q.available, q.volume * ft3});
        }
    if (out.lid_beds_)
        for (std::size_t bed = 0; bed < gw.nodeBeds().size(); ++bed) {
            const auto& b = gw.nodeBeds()[bed];
            if (!b.exchange || b.cell < 0)
                continue;
            const int n = b.node, sr = ctx.node_subtypes.storage_row(n);
            if (sr < 0 || ctx.node_subtypes.storages.lid[sr].bottom != LidNodeBottom::MeshAquifer)
                continue;
            const auto& state = ctx.node_subtypes.storages.lid_state[sr];
            const double down = gw.richardsBedDownCumulative()[bed],
                         up = gw.richardsBedUpCumulative()[bed];
            row(t.at("lid_bottom"),
                {f, double(n), double(bed), double(b.cell), state.richards ? 2. : 1., b.area,
                 (ctx.nodes.invert_elev[n] + ctx.nodes.depth[n]) * .3048,
                 gw.state().z_bed[b.cell] + gw.state().hg[b.cell], gw.cellCompletedTime(b.cell),
                 down, up, down - up, gw.pendingBedDown(int(bed)), gw.pendingBedUp(int(bed)),
                 gw.state().continuityResidual()});
            const auto& d = gw.lidBedDiagnostics()[bed];
            const bool valid = d.end > d.start;
            row(t.at("lid_bottom_sample"),
                {f, double(n), double(valid), d.start, d.end, d.groundwater_time, d.lid_head,
                 d.groundwater_head, d.requested_down, d.requested_up, d.reserved_down,
                 d.reserved_up, d.accepted_down, d.accepted_up,
                 valid ? (d.accepted_down - d.accepted_up) / (d.end - d.start) : 0});
            for (std::size_t k = 0; k < state.cells.size(); ++k) {
                const auto& c = state.cells[k];
                const double theta =
                    state.richards && k ? c.theta
                                        : c.theta + (c.porosity - c.theta) *
                                                        std::clamp((ctx.nodes.depth[n] - c.bottom) /
                                                                       (c.top - c.bottom),
                                                                   0., 1.);
                row(t.at("lid_profile"), {f, double(n), double(k), double(c.layer),
                                          c.bottom * .3048, c.top * .3048, theta});
                if (state.richards) {
                    const double pressure =
                        k ? state.richards_pressure[k] : ctx.nodes.depth[n] - c.bottom;
                    row(t.at("richards_profile"),
                        {f, double(n), double(k), pressure * .3048,
                         (ctx.nodes.invert_elev[n] + (k ? .5 * (c.bottom + c.top) : c.bottom) +
                          pressure) *
                             .3048,
                         (k ? state.richards_water[k] : ctx.nodes.volume[n]) * ft3});
                }
            }
        }
    return out;
}
/**
 * @brief Capture initial state or the next accepted engine-step water boundary transactionally.
 * @par Engine argument Stable checkpoint-capable engine with a managed source bridge or enabled
 * LID bottom attachment. After frame zero its exact address/identities/capabilities/start date
 * must match.
 * @param[in] start Accepted interval start elapsed s, exactly the previous saved engine end.
 * Initial capture requires start/current time zero; subsequent engine time must be strictly
 * greater.
 * @return Empty after checkpoint capture, boundary projection and complete candidate validation;
 * diagnostic on refusal or caught standard exception, with history/physics unchanged.
 * @note Copies existing history before appending and offsets pending-row identifiers globally.
 * Detailed mode seeds source frame zero on first capture; intermediate source intervals are
 * captured through captureSourceInterval(). The engine remains borrowed and must outlive
 * recorder use. Checkpoint capture can settle associated output metadata; this method does not
 * advance physics.
 */
std::string EngineSourceResultHistory::append(const SWMMEngine& e, double start) {
    try {
        require(e.surface_router_.commonSourceStage() &&
                    (e.source_delivery_ || e.surface_router_.subsurface().lidReceiptsEnabled()),
                "Engine history requires a managed source or LID bottom attachment.");
        std::string bytes;
        auto error = e.checkpointBytes(bytes);
        if (!error.empty())
            return error;
        const auto frame = tables_.empty() ? 0 : tables_.at("boundaries").rows();
        require(frame ? engine_ == &e && start == tables_.at("boundaries").columns[2].back() &&
                            e.ctx_.current_time > start
                      : start == 0 && e.ctx_.current_time == 0,
                "Capture initial state and every accepted engine step exactly once.");
        auto current = projection(e, frame, start);
        auto trial = *this;
        if (!frame) {
            current.detailed_ = detailed_;
            if (detailed_) {
                error = current.source_intervals_.append(*e.surface_router_.commonSourceStage());
                if (!error.empty())
                    return error;
            }
            trial = std::move(current);
        } else {
            require(sources_ == current.sources_ && nodes_ == current.nodes_ &&
                        outfall_returns_ == current.outfall_returns_ &&
                        lid_beds_ == current.lid_beds_ && start_date_ == current.start_date_,
                    "Engine source result identities changed.");
            for (const auto& [name, t] : current.tables_) {
                auto& next = trial.tables_.at(name);
                const double offset = double(next.rows());
                for (std::size_t c = 0; c < t.columns.size(); ++c)
                    for (double v : t.columns[c])
                        next.columns[c].push_back(
                            c == 1 && name.starts_with("pending_") ? v + offset : v);
            }
        }
        trial.boundary_ = std::move(bytes);
        trial.engine_ = &e;
        error = trial.validate();
        if (!error.empty())
            return error;
        *this = std::move(trial);
        return {};
    } catch (const std::exception& e) {
        return e.what();
    }
}
/**
 * @brief Rebind a saved recorder only to an exact restored whole-engine checkpoint and
 * projection.
 * @par Engine argument Stable restored checkpoint-capable engine with matching
 * managed-source/LID ownership. Its address is retained only after complete validation and must
 * outlive future recorder use.
 * @return Empty after exact checkpoint bytes, identifiers/capabilities/start date and final
 * table projection agree; otherwise diagnostic with this recorder and engine unchanged. Caught
 * standard exceptions become diagnostics.
 * @note Detailed companion history is privately checked against its earlier captured component
 * boundary and rebound to this engine's common stage. No hydraulic/component state is installed;
 * whole-engine restoration must already have completed. Future captures append after the saved
 * end.
 */
std::string EngineSourceResultHistory::resume(const SWMMEngine& e) {
    try {
        const auto invalid = validate();
        if (!invalid.empty())
            return invalid;
        require(e.surface_router_.commonSourceStage() &&
                    (e.source_delivery_ || e.surface_router_.subsurface().lidReceiptsEnabled()),
                "Engine history requires a managed source or LID bottom attachment.");
        std::string bytes;
        auto error = e.checkpointBytes(bytes);
        if (!error.empty())
            return error;
        require(bytes == boundary_,
                "Engine source history does not match the restored whole-engine checkpoint.");
        const auto frame = tables_.at("boundaries").rows() - 1;
        const auto current = projection(e, frame, tables_.at("boundaries").columns[1].back());
        require(sources_ == current.sources_ && nodes_ == current.nodes_ &&
                    outfall_returns_ == current.outfall_returns_ &&
                    lid_beds_ == current.lid_beds_ && start_date_ == current.start_date_,
                "Engine source result identities differ from checkpoint.");
        for (const auto& [name, t] : tables_) {
            const auto begin =
                std::lower_bound(t.columns[0].begin(), t.columns[0].end(), double(frame));
            const auto offset = std::size_t(begin - t.columns[0].begin());
            const auto& expected = current.tables_.at(name);
            require(t.rows() - offset == expected.rows(),
                    "Engine source final snapshot shape differs from checkpoint.");
            for (std::size_t c = 0; c < t.columns.size(); ++c)
                for (std::size_t r = 0; r < expected.rows(); ++r)
                    require(t.columns[c][offset + r] ==
                                expected.columns[c][r] +
                                    (c == 1 && name.starts_with("pending_") ? double(offset) : 0),
                            "Engine source final diagnostics differ from checkpoint.");
        }
        auto trial = *this;
        if (detailed_) {
            error = trial.source_intervals_.resumeCapturedBoundary(
                *e.surface_router_.commonSourceStage());
            if (!error.empty())
                return error;
        }
        trial.engine_ = &e;
        *this = std::move(trial);
        return {};
    } catch (const std::exception& e) {
        return e.what();
    }
}
/**
 * @brief Validate bounded engine-step water, source ownership, delivery queues and paired
 * history.
 * @return Empty for a nonempty consistent history; otherwise the first diagnostic or caught
 * standard exception text. No recorder or engine state is changed.
 * @note Checks <=64 MiB checkpoint anchor, finite start date, unique bounded name tables, exact
 * schema/units and at most 8*1024*1024 numeric values. Validates contiguous engine frames,
 * monotone independent source clocks, cumulative water, fixed ownership, node/pending delivery
 * identities, ordinal precision and water balances, plus optional outfall and LID/aquifer
 * receipt consistency. Uses 1e-10 balance comparisons and 1e-12 time allowances where encoded.
 * Calls validatePairing() for a separately bounded detailed-source companion; actual engine
 * matching requires resume().
 */
std::string EngineSourceResultHistory::validate() const {
    try {
        const auto expected = schema(outfall_returns_, lid_beds_);
        require(!boundary_.empty() && boundary_.size() <= 64 * 1024 * 1024 &&
                    std::isfinite(start_date_) && tables_.size() == expected.size(),
                "Missing engine source result checkpoint or tables.");
        for (const auto* names : {&sources_, &nodes_}) {
            require((!names->empty() || (lid_beds_ && names == &sources_)) &&
                        names->size() <= 65536 &&
                        std::set<std::string>(names->begin(), names->end()).size() == names->size(),
                    "Invalid engine source result names.");
            for (const auto& name : *names)
                require(!name.empty() && name.size() < 512 && name.find('\0') == std::string::npos,
                        "Invalid engine source result name.");
        }
        std::size_t total = 0;
        for (const auto& [name, t] : tables_) {
            const auto& shape = expected.at(name);
            require(t.fields == shape.fields && t.units == shape.units &&
                        t.columns.size() == shape.columns.size(),
                    "Engine source result schema or units differ.");
            for (const auto& col : t.columns) {
                require(col.size() == t.rows() && col.size() <= 8 * 1024 * 1024 - total,
                        "Engine source result column size differs or exceeds limit.");
                total += col.size();
                for (double v : col)
                    require(std::isfinite(v), "Nonfinite engine source result value.");
            }
        }
        const auto& b = tables_.at("boundaries").columns;
        const auto frames = b[0].size();
        require(frames > 0, "Empty engine source result history.");
        for (std::size_t f = 0; f < frames; ++f) {
            require(b[0][f] == double(f) && b[1][f] == (f ? b[2][f - 1] : 0) &&
                        (f ? b[2][f] > b[1][f] : b[2][f] == 0) && b[3][f] >= 0 &&
                        std::floor(b[3][f]) == b[3][f] && b[4][f] >= 0 &&
                        b[4][f] <= b[2][f] + 1e-12 * std::max(1., b[2][f]) &&
                        b[5][f] == (outfall_returns_ ? b[2][f] : -1) && b[6][f] == start_date_,
                    "Invalid engine source result clocks.");
            require(f ? b[3][f] >= b[3][f - 1] && b[4][f] >= b[4][f - 1] &&
                            (b[3][f] == b[3][f - 1]) == (b[4][f] == b[4][f - 1])
                      : b[3][f] == 0 && b[4][f] == 0,
                    "Invalid completed source interval progression.");
            for (std::size_t c = 7; c < b.size(); ++c)
                require(b[c][f] >= 0 && (f ? b[c][f] >= b[c][f - 1] : b[c][f] == 0),
                        "Invalid cumulative mesh water.");
        }
        std::map<std::string, std::vector<std::size_t>> offsets;
        for (const auto& [name, t] : tables_)
            if (name != "boundaries") {
                auto& at = offsets[name];
                at.resize(frames + 1);
                std::size_t r = 0;
                for (std::size_t f = 0; f < frames; ++f) {
                    at[f] = r;
                    while (r < t.rows() && t.columns[0][r] == double(f))
                        ++r;
                }
                at[frames] = r;
                require(r == t.rows(), "Invalid engine source result frame order.");
            }
        const auto& w = tables_.at("source_water").columns;
        const auto& g = tables_.at("group_water").columns;
        const auto owners = offsets.at("source_water")[1], groups = offsets.at("group_water")[1];
        require((owners > 0 && groups > 0) || (lid_beds_ && owners == 0 && groups == 0),
                "Empty engine source ownership.");
        std::vector<int> owner(sources_.size(), -1);
        std::vector<std::size_t> members(groups, 0);
        for (std::size_t r = 0; r < owners; ++r) {
            require(id(w[1][r], sources_.size()) && id(w[2][r], groups) &&
                        owner[std::size_t(w[1][r])] < 0,
                    "Invalid engine source ownership identity.");
            owner[std::size_t(w[1][r])] = int(w[2][r]);
            ++members[std::size_t(w[2][r])];
        }
        require(std::all_of(members.begin(), members.end(), [](auto count) { return count > 0; }),
                "Engine source group has no owned source.");
        const auto& n = tables_.at("node_delivery").columns;
        const auto& d = tables_.at("pending_delivery").columns;
        require(offsets.at("pending_delivery")[1] == 0 &&
                    (!outfall_returns_ || offsets.at("pending_outfall_return")[1] == 0),
                "Initial engine source queues must be empty.");
        for (std::size_t f = 0; f < frames; ++f) {
            const auto wr = offsets.at("source_water")[f], gr = offsets.at("group_water")[f],
                       nr = offsets.at("node_delivery")[f];
            require(offsets.at("source_water")[f + 1] - wr == owners &&
                        offsets.at("group_water")[f + 1] - gr == groups &&
                        offsets.at("node_delivery")[f + 1] - nr == nodes_.size(),
                    "Engine source snapshot identity shape changed.");
            std::vector<double> balance(groups, 0), scale(groups, 1), outfall(groups, 0),
                queued(sources_.size(), 0), pending_runoff(nodes_.size(), 0),
                pending_drain(nodes_.size(), 0);
            for (std::size_t r = wr; r < wr + owners; ++r) {
                const auto first = r - wr;
                for (std::size_t c = 1; c <= 4; ++c)
                    require(w[c][r] == w[c][first],
                            "Engine source owner or initial storage changed.");
                require(w[3][r] >= 0 && w[3][r] <= 1, "Invalid engine source spatial fraction.");
                for (std::size_t c = 4; c < w.size(); ++c) {
                    require(w[c][r] >= 0, "Negative engine source water.");
                    if (c != 15)
                        require(f ? w[c][r] >= w[c][r - owners] : c == 4 || w[c][r] == 0,
                                "Invalid cumulative source water.");
                }
                require(close(w[7][r], w[8][r] + w[9][r]) && (f || w[15][r] == w[4][r]),
                        "Invalid source infiltration split or initial storage.");
                const auto group = std::size_t(w[2][r]);
                const auto input = w[4][r] + w[5][r] + w[10][r] + (outfall_returns_ ? w[16][r] : 0);
                scale[group] += input;
                balance[group] += input - w[6][r] - w[7][r] - w[11][r] - w[15][r];
            }
            if (outfall_returns_) {
                const auto& q = tables_.at("pending_outfall_return").columns;
                std::set<std::pair<double, double>> seen;
                double last = -1;
                for (std::size_t r = offsets.at("pending_outfall_return")[f];
                     r < offsets.at("pending_outfall_return")[f + 1]; ++r) {
                    require(q[1][r] == double(r) && id(q[2][r], owner.size()) &&
                                owner[std::size_t(q[2][r])] >= 0 && q[3][r] >= last &&
                                q[3][r] >= b[4][f] - 1e-12 * std::max(1., b[4][f]) &&
                                q[3][r] <= b[2][f] && q[4][r] > 0 &&
                                seen.emplace(q[2][r], q[3][r]).second,
                            "Invalid engine source outfall queue.");
                    last = q[3][r];
                    outfall[owner[std::size_t(q[2][r])]] += q[4][r];
                    queued[std::size_t(q[2][r])] += q[4][r];
                }
                for (std::size_t r = wr; r < wr + owners; ++r)
                    require(queued[std::size_t(w[1][r])] <=
                                w[16][r] + 1e-10 * std::max(1., w[16][r]),
                            "Queued outfall water exceeds source credit.");
            }
            for (std::size_t r = gr; r < gr + groups; ++r)
                require(g[1][r] == double(r - gr) && g[2][r] >= 0 &&
                            (f || (g[2][r] == 0 && g[3][r] == 0)) &&
                            outfall[r - gr] <= g[2][r] + 1e-10 * std::max(1., g[2][r]) &&
                            std::abs(balance[r - gr] - g[2][r] - g[3][r]) <= 1e-10 * scale[r - gr],
                        "Invalid engine source group balance.");
            double ordinal = -1, available = -1;
            for (std::size_t r = offsets.at("pending_delivery")[f];
                 r < offsets.at("pending_delivery")[f + 1]; ++r) {
                require(d[1][r] == double(r) && d[2][r] > ordinal &&
                            std::floor(d[2][r]) == d[2][r] && d[2][r] <= 9007199254740991. &&
                            id(d[3][r], owner.size()) && owner[std::size_t(d[3][r])] >= 0 &&
                            id(d[4][r], nodes_.size()) && (d[5][r] == 0 || d[5][r] == 1) &&
                            d[6][r] >= 0 && d[7][r] > d[6][r] &&
                            d[7][r] <= b[4][f] + 1e-12 * std::max(1., b[4][f]) &&
                            d[8][r] >= available &&
                            d[8][r] >= d[7][r] - 1e-12 * std::max(1., d[7][r]) &&
                            d[8][r] <= b[2][f] && d[9][r] > 0,
                        "Invalid engine source terminal queue.");
                ordinal = d[2][r];
                available = d[8][r];
                (d[5][r] == 0 ? pending_runoff : pending_drain)[std::size_t(d[4][r])] += d[9][r];
            }
            for (std::size_t r = nr; r < nr + nodes_.size(); ++r) {
                const auto node = r - nr;
                require(n[1][r] == double(node) && n[8][r] == b[1][f] && n[9][r] == b[2][f],
                        "Invalid source node delivery clock or identity.");
                for (std::size_t c = 2; c <= 7; ++c)
                    require(n[c][r] >= 0 &&
                                (c > 5 || (f ? n[c][r] >= n[c][r - nodes_.size()] : n[c][r] == 0)),
                            "Invalid cumulative node delivery water.");
                require(n[10][r] >= 0 && n[6][r] <= n[4][r] && n[7][r] <= n[5][r],
                        "Invalid accepted receipt or source rate.");
                require(close(n[2][r] - n[4][r], pending_runoff[node]) &&
                            close(n[3][r] - n[5][r], pending_drain[node]) &&
                            close(n[10][r], f ? (n[6][r] + n[7][r]) / (b[2][f] - b[1][f]) : 0),
                        "Invalid source node delivery balance or rate.");
                if (f)
                    require(close(n[4][r] - n[4][r - nodes_.size()], n[6][r]) &&
                                close(n[5][r] - n[5][r - nodes_.size()], n[7][r]),
                            "Invalid accepted source delivery increment.");
            }
        }
        if (lid_beds_) {
            const auto& l = tables_.at("lid_bottom").columns;
            const auto& d = tables_.at("lid_bottom_sample").columns;
            const auto count = offsets.at("lid_bottom")[1];
            require(count > 0, "Missing LID bottom identities.");
            for (std::size_t f = 0; f < frames; ++f) {
                const auto at = offsets.at("lid_bottom")[f],
                           sample = offsets.at("lid_bottom_sample")[f];
                require(offsets.at("lid_bottom")[f + 1] - at == count &&
                            offsets.at("lid_bottom_sample")[f + 1] - sample == count,
                        "LID bottom snapshot shape changed.");
                for (std::size_t k = 0; k < count; ++k) {
                    const auto r = at + k, q = sample + k;
                    require(id(l[1][r], nodes_.size()) && l[5][r] > 0 && l[8][r] >= 0 &&
                                l[8][r] <= b[2][f] + 1e-12 * std::max(1., b[2][f]),
                            "Invalid LID bottom identity or time.");
                    for (std::size_t c = 1; c <= 5; ++c)
                        require(l[c][r] == l[c][k], "LID bottom identity changed.");
                    for (std::size_t c : {9u, 10u, 12u, 13u})
                        require(l[c][r] >= 0, "Negative gross LID bottom water.");
                    require(close(l[11][r], l[9][r] - l[10][r]) && l[12][r] <= l[9][r] + 1e-10 &&
                                l[13][r] <= l[10][r] + 1e-10,
                            "Unpaired LID bottom water.");
                    require(d[1][q] == l[1][r] && (d[2][q] == 0 || d[2][q] == 1) && d[3][q] >= 0 &&
                                d[4][q] >= d[3][q] &&
                                d[4][q] <= b[2][f] + 1e-12 * std::max(1., b[2][f]) &&
                                d[5][q] >= 0 && d[5][q] <= d[4][q] + 1e-12 &&
                                d[12][q] <= d[10][q] && d[13][q] <= d[11][q],
                            "Invalid accepted LID bottom sample.");
                    if (f)
                        require(l[9][r] >= l[9][r - count] && l[10][r] >= l[10][r - count],
                                "LID bottom cumulative water decreased.");
                }
            }
        }
        return validatePairing();
    } catch (const std::exception& e) {
        return e.what();
    }
}
/**
 * @brief Append one fine source interval during the owning engine's active accepted-step work.
 * @param[in] stage Exact common source stage of the already attached live engine.
 * @return Empty after the component recorder captures the interval; diagnostic unless detailed
 * mode, matching live stage and engine step_active are all present, or component capture
 * refuses.
 * @note Component append validates exact interval ordering and uses candidate publication. This
 * hook changes only the detailed diagnostic history, not source physics or whole-engine frame
 * clocks.
 */
std::string EngineSourceResultHistory::captureSourceInterval(const CommonSourceStage& stage) {
    if (!detailed_ || !engine_ || engine_->surface_router_.commonSourceStage() != &stage ||
        !engine_->step_active_)
        return "Detailed source capture requires the attached active engine step.";
    return source_intervals_.append(stage);
}
/**
 * @brief Validate fine source snapshots against each engine frame's completed source boundary.
 * @return Empty when capability/identity/clock/ownership/water comparisons agree; otherwise a
 * diagnostic or caught standard exception. In engine-only mode requires the companion tables
 * empty.
 * @note Requires already valid containing tables. Detailed mode validates the component history,
 * combines both numeric tables under the 8*1024*1024 value budget, requires water-only matching
 * sources/return capability, exact interval counts and mesh/source totals, and adjusts only
 * pending group water for newer native outfall credits. Fine source outfall clocks remain no
 * later than the accepted engine step's start; earlier rows are never relabeled. No mutation or
 * synchronization.
 */
std::string EngineSourceResultHistory::validatePairing() const {
    try {
        if (!detailed_) {
            require(source_intervals_.tables_.empty(),
                    "Unexpected detailed source history in engine-only profile.");
            return {};
        }
        const auto invalid = source_intervals_.validate();
        if (!invalid.empty())
            return invalid;
        std::size_t total = 0;
        for (const auto* tables : {&tables_, &source_intervals_.tables_})
            for (const auto& [name, t] : *tables)
                for (const auto& c : t.columns) {
                    require(c.size() <= 8 * 1024 * 1024 - total,
                            "Combined paired history exceeds its value limit.");
                    total += c.size();
                }
        require(!source_intervals_.boundary_.empty() && source_intervals_.sources_ == sources_ &&
                    source_intervals_.pollutants_.empty() &&
                    source_intervals_.outfall_returns_ == outfall_returns_,
                "Paired source history identities, checkpoint or capabilities differ.");
        const auto& st = source_intervals_.tables_;
        const auto& sb = st.at("boundaries").columns;
        const auto& eb = tables_.at("boundaries").columns;
        require(sb[0].size() == std::size_t(eb[3].back()) + 1,
                "Paired source history omits or extends completed intervals.");
        const auto offsets = [](const SourceResultTable& t, std::size_t frames) {
            std::vector<std::size_t> at(frames + 1);
            std::size_t r = 0;
            for (std::size_t f = 0; f < frames; ++f) {
                at[f] = r;
                while (r < t.rows() && t.columns[0][r] == double(f))
                    ++r;
            }
            at[frames] = r;
            return at;
        };
        const auto sw = offsets(st.at("source_water"), sb[0].size()),
                   ew = offsets(tables_.at("source_water"), eb[0].size());
        const auto sg = offsets(st.at("group_water"), sb[0].size()),
                   eg = offsets(tables_.at("group_water"), eb[0].size());
        const auto& s = st.at("source_water").columns;
        const auto& w = tables_.at("source_water").columns;
        const auto& gs = st.at("group_water").columns;
        const auto& ge = tables_.at("group_water").columns;
        for (std::size_t f = 0; f < eb[0].size(); ++f) {
            const auto n = std::size_t(eb[3][f]);
            require(sb[3][n] == eb[4][f], "Paired source and engine clocks differ.");
            for (std::size_t c = 4; c < 7; ++c)
                require(sb[c][n] == eb[c + 3][f], "Paired cumulative mesh water differs.");
            require(sw[n + 1] - sw[n] == ew[f + 1] - ew[f] &&
                        sg[n + 1] - sg[n] == eg[f + 1] - eg[f],
                    "Paired source ownership shape differs.");
            std::vector<double> extra(eg[f + 1] - eg[f], 0);
            for (std::size_t r = sw[n]; r < sw[n + 1]; ++r) {
                const auto er = ew[f] + r - sw[n];
                for (std::size_t c = 1; c < 16; ++c)
                    require(s[c][r] == w[c][er],
                            "Paired source water differs from engine snapshot.");
                if (outfall_returns_) {
                    require(w[16][er] >= s[16][r],
                            "Engine outfall credit precedes source capture.");
                    extra[std::size_t(s[2][r])] += w[16][er] - s[16][r];
                }
            }
            for (std::size_t r = sg[n]; r < sg[n + 1]; ++r) {
                const auto er = eg[f] + r - sg[n];
                require(gs[1][r] == ge[1][er] && close(gs[2][r] + extra[r - sg[n]], ge[2][er]) &&
                            close(gs[3][r], ge[3][er]),
                        "Paired pending water or group residual differs.");
            }
        }
        // Fine source intervals are captured before this step's native outfall
        // credits. Preserve that earlier clock; never relabel their old rows.
        std::size_t f = 0;
        for (std::size_t n = 0; n < sb[0].size(); ++n) {
            while (f + 1 < eb[0].size() && eb[3][f] < double(n))
                ++f;
            require(sb[3][n] <= eb[2][f] + 1e-12 * std::max(1., eb[2][f]),
                    "Source interval extends its accepted engine step.");
            if (outfall_returns_)
                require(st.at("outfall_clock").columns[1][n] <=
                            eb[1][f] + 1e-12 * std::max(1., eb[1][f]),
                        "Source snapshot includes outfall credits from a later engine step.");
        }
        return {};
    } catch (const std::exception& e) {
        return e.what();
    }
}
}
