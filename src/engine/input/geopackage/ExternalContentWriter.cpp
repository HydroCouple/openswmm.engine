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
 * @file      ExternalContentWriter.cpp
 * @brief     Slice IO-7 — implementation of the Part D content fan-out.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "ExternalContentWriter.hpp"
#include "core/FileIO.hpp" // issue #7: UTF-8 paths on Windows
#include "GpkgUtils.hpp"
#include "formats/ClimateFormat.hpp"
#include "formats/HotstartFormat.hpp"
#include "formats/RaingageFormat.hpp"
#include "formats/RoutingInterfaceFormat.hpp"
#include "formats/TimeseriesFormat.hpp"

#include "../../core/DateTime.hpp"
#include "../../core/FilePathPair.hpp"
#include "../../core/HotStartManager.hpp"
#include "../../core/SimulationContext.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace openswmm::gpkg {

namespace {

using openswmm::FilePathPair;
using openswmm::SimulationContext;
using openswmm::TableType;
using openswmm::FileMode;

// -------------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------------

/**
 * @brief Test whether a UTF-8 path can currently be opened for binary reading.
 *
 * Not a regular-file/size/format check; access may change before subsequent parsing.
 *
 * @param[in] path Borrowed filesystem path; empty text returns false.
 * @return True when fopen succeeds and its stream is closed, otherwise false.
 */
bool fileExists(const std::string& path) {
    if (path.empty())
        return false;
    std::FILE* fp = openswmm::io::fopen_utf8(path, "rb");
    if (!fp)
        return false;
    std::fclose(fp);
    return true;
}

// Returns the path token most suitable for fopen: prefer `.absolute` when
// IO-3 resolution populated it, otherwise fall back to `.original`.
/**
 * @brief Copy the preferred effective external-file path token.
 *
 * @param[in] slot Borrowed path pair; nonempty absolute takes precedence over original.
 * @return Owned selected text, possibly containing a :column suffix; no existence/resolution check.
 */
std::string fopenToken(const FilePathPair& slot) {
    return !slot.absolute.empty() ? slot.absolute : slot.str();
}

// Strip the optional `:column` suffix on a CSV-style external path.
// Preserves Windows drive letters ("C:\path"): only strips when the colon
// position is past index 1.
/**
 * @brief Split the last colon suffix except a leading Windows drive colon.
 *
 * No filesystem or CSV validation. A later colon in a legal filename is interpreted as a selector;
 * empty suffix is permitted.
 *
 * @param[in] token Owned/copied path token; last colon must be beyond index1 to split.
 * @return Owned path and column strings; unsplit tokens produce an empty column.
 */
std::pair<std::string, std::string> splitColumn(std::string token) {
    auto pos = token.rfind(':');
    if (pos == std::string::npos || pos <= 1)
        return {std::move(token), {}};
    std::string column = token.substr(pos + 1);
    token.resize(pos);
    return {std::move(token), std::move(column)};
}

// POSIX/Win path basename. Just the filename component.
/**
 * @brief Copy the suffix after the final slash or backslash.
 *
 * @param[in] path Borrowed path text; both separator conventions are recognized.
 * @return Owned leaf, entire input when no separator, or empty for a trailing separator.
 */
std::string basenameOf(const std::string& path) {
    auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

// Convert OADate (decimal days since 12/30/1899) to ISO-8601 timestamp.
/**
 * @brief Format an OADate as an ISO-like whole-second timestamp.
 *
 * No finite/range check; Z is a persisted spelling rather than a timezone adjustment.
 *
 * @param[in] oa Finite calendar-encodable decimal days since1899-12-30, interpreted without
 *     timezone conversion.
 * @return Owned YYYY-MM-DDTHH:MM:SSZ text using datetime decode rounding.
 */
std::string oadateToIso(double oa) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    datetime::decodeDate(oa, y, mo, d);
    datetime::decodeTime(oa, h, mi, s);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ", y, mo, d, h, mi, s);
    return buf;
}

// -------------------------------------------------------------------------
// Insertion helpers — one per Part D table family
// -------------------------------------------------------------------------

/**
 * @brief Insert ordered external-series samples and source provenance.
 *
 * Assigns zero-based ordinals and imported_from_file source; OADate converts to whole-second text
 * without timezone arithmetic. Does not validate chronology or duplicate keys.
 * Preparation/allocation may throw; bind and step return codes are ignored, so void completion does
 * not certify row insertion. No local transaction or rollback.
 *
 * @param[in,out] db Borrowed writable schema-compatible SQLite connection; caller owns transaction,
 *     cleanup and parent catalogue rows.
 * @param[in] sim Borrowed simulation key copied into inserted/updated rows.
 * @param[in] series_id Series key matching the model catalogue.
 * @param[in] source_filename Persisted source basename copied per row.
 * @param[in] source_column Optional selector; empty binds SQL NULL.
 * @param[in] rows Borrowed samples; size must fit int ordinal range, timestamps calendar-encodable,
 *     values retain series units.
 */
void insert_input_timeseries(sqlite3* db, const std::string& sim, const std::string& series_id,
                             const std::string& source_filename, const std::string& source_column,
                             const std::vector<formats::TimeseriesRow>& rows) {
    auto stmt = prepare(db, "INSERT INTO input_timeseries "
                            "(simulation_id, series_id, timestamp, value, ordinal, "
                            " source, source_filename, source_column) "
                            "VALUES (?, ?, ?, ?, ?, 'imported_from_file', ?, ?)");
    int ordinal = 0;
    for (const auto& r : rows) {
        sqlite3_reset(stmt.get());
        bind_text(stmt.get(), 1, sim);
        bind_text(stmt.get(), 2, series_id);
        bind_text(stmt.get(), 3, oadateToIso(r.timestamp_oa));
        bind_double(stmt.get(), 4, r.value);
        bind_int(stmt.get(), 5, ordinal++);
        bind_text(stmt.get(), 6, source_filename);
        if (source_column.empty())
            sqlite3_bind_null(stmt.get(), 7);
        else
            bind_text(stmt.get(), 7, source_column);
        sqlite3_step(stmt.get());
    }
}

/**
 * @brief Insert embedded gage rainfall observations.
 *
 * Writes whole-second timestamp text; no unit conversion, deduplication or date-order check.
 * Preparation/allocation may throw; bind and step return codes are ignored, so void completion does
 * not certify row insertion. No local transaction or rollback.
 *
 * @param[in,out] db Borrowed writable schema-compatible SQLite connection; caller owns transaction,
 *     cleanup and parent catalogue rows.
 * @param[in] sim Borrowed simulation key copied into inserted/updated rows.
 * @param[in] gage_id Registered gage key.
 * @param[in] rows Borrowed calendar-encodable samples; rainfall retains original recording units
 *     and empty station binds NULL.
 */
void insert_raingage_data(sqlite3* db, const std::string& sim, const std::string& gage_id,
                          const std::vector<formats::RaingageRow>& rows) {
    auto stmt = prepare(db, "INSERT INTO raingage_data "
                            "(simulation_id, gage_id, record_time, rainfall_value, station_id) "
                            "VALUES (?, ?, ?, ?, ?)");
    for (const auto& r : rows) {
        sqlite3_reset(stmt.get());
        bind_text(stmt.get(), 1, sim);
        bind_text(stmt.get(), 2, gage_id);
        bind_text(stmt.get(), 3, oadateToIso(r.timestamp_oa));
        bind_double(stmt.get(), 4, r.rainfall);
        if (r.station_id.empty())
            sqlite3_bind_null(stmt.get(), 5);
        else
            bind_text(stmt.get(), 5, r.station_id);
        sqlite3_step(stmt.get());
    }
}

/**
 * @brief Insert embedded daily climate rows without unit conversion.
 *
 * Copies date strings and all numeric columns directly; no finite, date or range validation.
 * Preparation/allocation may throw; bind and step return codes are ignored, so void completion does
 * not certify row insertion. No local transaction or rollback.
 *
 * @param[in,out] db Borrowed writable schema-compatible SQLite connection; caller owns transaction,
 *     cleanup and parent catalogue rows.
 * @param[in] sim Borrowed simulation key copied into inserted/updated rows.
 * @param[in] rows Borrowed dates and temperature/evaporation/wind/sky/humidity scalars in their
 *     retained format conventions.
 */
void insert_climate_data(sqlite3* db, const std::string& sim,
                         const std::vector<formats::ClimateRow>& rows) {
    auto stmt = prepare(db, "INSERT INTO climate_data "
                            "(simulation_id, record_date, tmin, tmax, evaporation, "
                            " wind_speed, sky_cover, humidity) "
                            "VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
    for (const auto& r : rows) {
        sqlite3_reset(stmt.get());
        bind_text(stmt.get(), 1, sim);
        bind_text(stmt.get(), 2, r.record_date);
        bind_double(stmt.get(), 3, r.tmin);
        bind_double(stmt.get(), 4, r.tmax);
        bind_double(stmt.get(), 5, r.evaporation);
        bind_double(stmt.get(), 6, r.wind_speed);
        bind_double(stmt.get(), 7, r.sky_cover);
        bind_double(stmt.get(), 8, r.humidity);
        sqlite3_step(stmt.get());
    }
}

// Routing interface rows live in three sibling tables keyed by object kind.
// The role determines which table receives the row.
/**
 * @brief Insert one node interface flow and available pollutant child values.
 *
 * Flow/concentrations are copied without unit conversion or metadata validation; excess values/IDs
 * are ignored. Preparation/allocation may throw; bind and step return codes are ignored, so void
 * completion does not certify row insertion. No local transaction or rollback.
 *
 * @param[in,out] db Borrowed writable schema-compatible SQLite connection; caller owns transaction,
 *     cleanup and parent catalogue rows.
 * @param[in] sim Borrowed simulation key copied into inserted/updated rows.
 * @param[in] role Copied file-role token; no enum validation.
 * @param[in] direction Copied USE/SAVE token; no validation.
 * @param[in] row Borrowed observation; object ID must match its catalogue, timestamp
 *     calendar-encodable, numeric values retain file units.
 * @param[in] pollutant_ids Borrowed catalogue; writes min(catalogue size,row concentration size)
 *     child rows in matching order.
 */
void insert_routing_node_row(sqlite3* db, const std::string& sim, const std::string& role,
                             const std::string& direction, const formats::RoutingInterfaceRow& row,
                             const std::vector<std::string>& pollutant_ids) {
    {
        auto stmt =
            prepare(db, "INSERT INTO routing_interface_node "
                        "(simulation_id, role, direction, node_id, record_time, flow_value) "
                        "VALUES (?, ?, ?, ?, ?, ?)");
        bind_text(stmt.get(), 1, sim);
        bind_text(stmt.get(), 2, role);
        bind_text(stmt.get(), 3, direction);
        bind_text(stmt.get(), 4, row.object_id);
        bind_text(stmt.get(), 5, oadateToIso(row.timestamp_oa));
        bind_double(stmt.get(), 6, row.flow_value);
        sqlite3_step(stmt.get());
    }
    // Pollutant child rows — one per pollutant_id at this timestamp.
    for (std::size_t p = 0; p < pollutant_ids.size() && p < row.pollutant_values.size(); ++p) {
        auto stmt = prepare(db, "INSERT INTO routing_interface_node_pollutants "
                                "(simulation_id, role, direction, node_id, record_time, "
                                " pollutant_id, concentration) "
                                "VALUES (?, ?, ?, ?, ?, ?, ?)");
        bind_text(stmt.get(), 1, sim);
        bind_text(stmt.get(), 2, role);
        bind_text(stmt.get(), 3, direction);
        bind_text(stmt.get(), 4, row.object_id);
        bind_text(stmt.get(), 5, oadateToIso(row.timestamp_oa));
        bind_text(stmt.get(), 6, pollutant_ids[p]);
        bind_double(stmt.get(), 7, row.pollutant_values[p]);
        sqlite3_step(stmt.get());
    }
}

/**
 * @brief Insert one catchment interface flow observation.
 *
 * Does not store pollutant children or a separate units catalogue; copies flow as supplied.
 * Preparation/allocation may throw; bind and step return codes are ignored, so void completion does
 * not certify row insertion. No local transaction or rollback.
 *
 * @param[in,out] db Borrowed writable schema-compatible SQLite connection; caller owns transaction,
 *     cleanup and parent catalogue rows.
 * @param[in] sim Borrowed simulation key copied into inserted/updated rows.
 * @param[in] role Copied file-role token; no enum validation.
 * @param[in] direction Copied USE/SAVE token; no validation.
 * @param[in] row Borrowed observation; object ID must match its catalogue, timestamp
 *     calendar-encodable, numeric values retain file units.
 */
void insert_routing_subcatch_row(sqlite3* db, const std::string& sim, const std::string& role,
                                 const std::string& direction,
                                 const formats::RoutingInterfaceRow& row) {
    auto stmt =
        prepare(db, "INSERT INTO routing_interface_subcatch "
                    "(simulation_id, role, direction, subcatch_id, record_time, flow_value) "
                    "VALUES (?, ?, ?, ?, ?, ?)");
    bind_text(stmt.get(), 1, sim);
    bind_text(stmt.get(), 2, role);
    bind_text(stmt.get(), 3, direction);
    bind_text(stmt.get(), 4, row.object_id);
    bind_text(stmt.get(), 5, oadateToIso(row.timestamp_oa));
    bind_double(stmt.get(), 6, row.flow_value);
    sqlite3_step(stmt.get());
}

/**
 * @brief Insert one gage interface rainfall observation.
 *
 * Copies row.flow_value into rainfall_value without unit conversion; no pollutant children.
 * Preparation/allocation may throw; bind and step return codes are ignored, so void completion does
 * not certify row insertion. No local transaction or rollback.
 *
 * @param[in,out] db Borrowed writable schema-compatible SQLite connection; caller owns transaction,
 *     cleanup and parent catalogue rows.
 * @param[in] sim Borrowed simulation key copied into inserted/updated rows.
 * @param[in] role Copied file-role token; no enum validation.
 * @param[in] direction Copied USE/SAVE token; no validation.
 * @param[in] row Borrowed observation; object ID must match its catalogue, timestamp
 *     calendar-encodable, numeric values retain file units.
 */
void insert_routing_gage_row(sqlite3* db, const std::string& sim, const std::string& role,
                             const std::string& direction,
                             const formats::RoutingInterfaceRow& row) {
    auto stmt =
        prepare(db, "INSERT INTO routing_interface_gage "
                    "(simulation_id, role, direction, gage_id, record_time, rainfall_value) "
                    "VALUES (?, ?, ?, ?, ?, ?)");
    bind_text(stmt.get(), 1, sim);
    bind_text(stmt.get(), 2, role);
    bind_text(stmt.get(), 3, direction);
    bind_text(stmt.get(), 4, row.object_id);
    bind_text(stmt.get(), 5, oadateToIso(row.timestamp_oa));
    bind_double(stmt.get(), 6, row.flow_value);
    sqlite3_step(stmt.get());
}

/**
 * @brief Insert a legacy hotstart slot parent and save-format provenance.
 *
 * No state rows or file bytes are inserted by this helper. Does not validate enum compatibility or
 * overwrite existing slots. Preparation/allocation may throw; bind and step return codes are
 * ignored, so void completion does not certify row insertion. No local transaction or rollback.
 *
 * @param[in,out] db Borrowed writable schema-compatible SQLite connection; caller owns transaction,
 *     cleanup and parent catalogue rows.
 * @param[in] sim Borrowed simulation key copied into inserted/updated rows.
 * @param[in] slot_name Unique slot key within the simulation.
 * @param[in] direction Copied USE/SAVE token.
 * @param[in] save_datetime OADate schedule; positive binds value, otherwise NULL. Populated
 *     captured_at is formatted from this value even if zero.
 * @param[in] format_version Copied legacy file-version integer.
 * @param[in] flow_units Copied label; empty binds NULL.
 * @param[in] num_pollut Copied pollutant count; caller supplies nonnegative valid dimensions.
 * @param[in] status Exact populated adds captured_at; other tokens bind NULL there.
 * @param[in] save_format LEGACY/NATIVE persists a token; AUTO or unknown ordinal binds NULL.
 */
void insert_hotstart_slot_row(sqlite3* db, const std::string& sim, const std::string& slot_name,
                              const std::string& direction, double save_datetime,
                              int format_version, const std::string& flow_units, int num_pollut,
                              const std::string& status,
                              HotstartFormat save_format = HotstartFormat::AUTO) {
    auto stmt = prepare(db, "INSERT INTO hotstart_slots "
                            "(simulation_id, slot_name, direction, save_datetime, "
                            " format_version, flow_units, num_pollutants, captured_at, status, "
                            " save_format) "
                            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    bind_text(stmt.get(), 1, sim);
    bind_text(stmt.get(), 2, slot_name);
    bind_text(stmt.get(), 3, direction);
    if (save_datetime > 0.0)
        bind_double(stmt.get(), 4, save_datetime);
    else
        sqlite3_bind_null(stmt.get(), 4);
    bind_int(stmt.get(), 5, format_version);
    if (flow_units.empty())
        sqlite3_bind_null(stmt.get(), 6);
    else
        bind_text(stmt.get(), 6, flow_units);
    bind_int(stmt.get(), 7, num_pollut);
    if (status == "populated")
        bind_text(stmt.get(), 8, oadateToIso(save_datetime));
    else
        sqlite3_bind_null(stmt.get(), 8);
    bind_text(stmt.get(), 9, status);
    // NULL = AUTO, so a row written for an older .gpkg reads back unchanged.
    if (save_format == HotstartFormat::LEGACY)
        bind_text(stmt.get(), 10, "LEGACY");
    else if (save_format == HotstartFormat::NATIVE)
        bind_text(stmt.get(), 10, "NATIVE");
    else
        sqlite3_bind_null(stmt.get(), 10);
    sqlite3_step(stmt.get());
}

/**
 * @brief Insert name-keyed legacy node/link state and opaque runoff/trailer bytes.
 *
 * Stops node/link/pollutant loops at available registry names. Copies depth ft, flow cfs, volume
 * ft3 and retained setting/time/concentration conventions, with optional HRT. Updates slot counts
 * and opaque runoff/tail blobs; no model/snapshot compatibility validation or runtime restoration.
 * Preparation/allocation may throw; bind and step return codes are ignored, so void completion does
 * not certify row insertion. No local transaction or rollback.
 *
 * @param[in,out] db Borrowed writable schema-compatible SQLite connection; caller owns transaction,
 *     cleanup and parent catalogue rows.
 * @param[in] sim Borrowed simulation key copied into inserted/updated rows.
 * @param[in] slot Existing slot parent key updated with payload metadata.
 * @param[in] ctx Borrowed registries whose order must match snapshot indices.
 * @param[in] snap Borrowed native legacy state; blob lengths must fit int for SQLite binding.
 */
void insert_hotstart_state(sqlite3* db, const std::string& sim, const std::string& slot,
                           const SimulationContext& ctx, const formats::HotstartSnapshot& snap) {
    // Node state (HSF order matches ctx.nodes order — SWMM convention).
    {
        auto stmt =
            prepare(db, "INSERT INTO hotstart_node_state "
                        "(simulation_id, slot_name, node_id, depth, lateral_inflow, overflow, hrt) "
                        "VALUES (?, ?, ?, ?, ?, ?, ?)");
        for (std::size_t i = 0; i < snap.node_state.size(); ++i) {
            if (static_cast<int>(i) >= ctx.node_names.size())
                break;
            const auto& s = snap.node_state[i];
            sqlite3_reset(stmt.get());
            bind_text(stmt.get(), 1, sim);
            bind_text(stmt.get(), 2, slot);
            bind_text(stmt.get(), 3, ctx.node_names.name_of(static_cast<int>(i)));
            bind_double(stmt.get(), 4, s.depth);
            bind_double(stmt.get(), 5, s.lateral_inflow);
            bind_double(stmt.get(), 6, s.overflow);
            if (s.has_hrt)
                bind_double(stmt.get(), 7, s.hrt);
            else
                sqlite3_bind_null(stmt.get(), 7);
            sqlite3_step(stmt.get());
        }
    }
    // Node pollutant state.
    if (snap.header.num_pollut > 0) {
        auto stmt = prepare(db, "INSERT INTO hotstart_node_pollutant_state "
                                "(simulation_id, slot_name, node_id, pollutant_id, concentration) "
                                "VALUES (?, ?, ?, ?, ?)");
        for (std::size_t i = 0; i < snap.node_state.size(); ++i) {
            if (static_cast<int>(i) >= ctx.node_names.size())
                break;
            const std::string node_id = ctx.node_names.name_of(static_cast<int>(i));
            const auto& concs = snap.node_state[i].concentration;
            for (std::size_t p = 0; p < concs.size(); ++p) {
                if (static_cast<int>(p) >= ctx.pollutant_names.size())
                    break;
                sqlite3_reset(stmt.get());
                bind_text(stmt.get(), 1, sim);
                bind_text(stmt.get(), 2, slot);
                bind_text(stmt.get(), 3, node_id);
                bind_text(stmt.get(), 4, ctx.pollutant_names.name_of(static_cast<int>(p)));
                bind_double(stmt.get(), 5, concs[p]);
                sqlite3_step(stmt.get());
            }
        }
    }
    // Link state.
    {
        auto stmt = prepare(db, "INSERT INTO hotstart_link_state "
                                "(simulation_id, slot_name, link_id, flow, depth, volume, "
                                " setting, target_setting, time_open, time_closed) "
                                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
        for (std::size_t i = 0; i < snap.link_state.size(); ++i) {
            if (static_cast<int>(i) >= ctx.link_names.size())
                break;
            const auto& s = snap.link_state[i];
            sqlite3_reset(stmt.get());
            bind_text(stmt.get(), 1, sim);
            bind_text(stmt.get(), 2, slot);
            bind_text(stmt.get(), 3, ctx.link_names.name_of(static_cast<int>(i)));
            bind_double(stmt.get(), 4, s.flow);
            bind_double(stmt.get(), 5, s.depth);
            bind_double(stmt.get(), 6, s.volume);
            bind_double(stmt.get(), 7, s.setting);
            bind_double(stmt.get(), 8, s.target_setting);
            bind_double(stmt.get(), 9, s.time_open);
            bind_double(stmt.get(), 10, s.time_closed);
            sqlite3_step(stmt.get());
        }
    }
    // Link pollutant state.
    if (snap.header.num_pollut > 0) {
        auto stmt = prepare(db, "INSERT INTO hotstart_link_pollutant_state "
                                "(simulation_id, slot_name, link_id, pollutant_id, concentration) "
                                "VALUES (?, ?, ?, ?, ?)");
        for (std::size_t i = 0; i < snap.link_state.size(); ++i) {
            if (static_cast<int>(i) >= ctx.link_names.size())
                break;
            const std::string link_id = ctx.link_names.name_of(static_cast<int>(i));
            const auto& concs = snap.link_state[i].concentration;
            for (std::size_t p = 0; p < concs.size(); ++p) {
                if (static_cast<int>(p) >= ctx.pollutant_names.size())
                    break;
                sqlite3_reset(stmt.get());
                bind_text(stmt.get(), 1, sim);
                bind_text(stmt.get(), 2, slot);
                bind_text(stmt.get(), 3, link_id);
                bind_text(stmt.get(), 4, ctx.pollutant_names.name_of(static_cast<int>(p)));
                bind_double(stmt.get(), 5, concs[p]);
                sqlite3_step(stmt.get());
            }
        }
    }
    // The subcatchment section and any trailer ride on the slot row as they
    // are in the file (their layout depends on the model, not on names).
    {
        auto stmt =
            prepare(db, "UPDATE hotstart_slots SET num_subcatch=?, num_landuses=?, "
                        "runoff_section=?, tail_section=? WHERE simulation_id=? AND slot_name=?");
        bind_int(stmt.get(), 1, snap.runoff_raw.empty() ? 0 : snap.header.num_subcatch);
        bind_int(stmt.get(), 2, snap.header.num_landuses);
        auto bind_bytes = [&](int col, const std::string& b) {
            if (b.empty())
                sqlite3_bind_null(stmt.get(), col);
            else
                bind_blob(stmt.get(), col, b.data(), static_cast<int>(b.size()));
        };
        bind_bytes(3, snap.runoff_raw);
        bind_bytes(4, snap.tail_raw);
        bind_text(stmt.get(), 5, sim);
        bind_text(stmt.get(), 6, slot);
        sqlite3_step(stmt.get());
    }
}

// The model's side of a legacy file's layout (see formats::HsfLayout).
/**
 * @brief Build a legacy hotstart layout descriptor borrowing the current model.
 *
 * Does not snapshot model dimensions for the callback or inspect a hotstart file.
 *
 * @param[in] ctx Borrowed coherent model; must outlive use of the returned runoff_doubles callback.
 * @return Owned descriptor with copied storage flags/count and a callback capturing ctx by
 *     reference.
 */
formats::HsfLayout hsfLayout(const SimulationContext& ctx) {
    formats::HsfLayout layout;
    layout.n_subcatch = ctx.n_subcatches();
    layout.runoff_doubles = [&ctx](int n_landuses) {
        return HotStartManager::legacy_runoff_size(ctx, n_landuses);
    };
    layout.storage.assign(ctx.nodes.type.size(), 0);
    for (std::size_t i = 0; i < ctx.nodes.type.size(); ++i)
        layout.storage[i] = ctx.nodes.type[i] == NodeType::STORAGE;
    return layout;
}

// -------------------------------------------------------------------------
// Slot dispatchers
// -------------------------------------------------------------------------

/**
 * @brief Parse and embed every nonempty time-series external-file slot.
 *
 * Uses effective path and last-colon selector, checks readable input and throws GpkgError for
 * missing/parse failure. Parses all text rows; selector is retained as provenance rather than
 * applied by this dispatcher. Persists basename, raw series values and whole-second timestamps.
 * Existing database samples are not cleared. Preparation/allocation may throw; bind and step return
 * codes are ignored, so void completion does not certify row insertion. No local transaction or
 * rollback.
 *
 * @param[in,out] db Borrowed writable schema-compatible SQLite connection; caller owns transaction,
 *     cleanup and parent catalogue rows.
 * @param[in] ctx Borrowed coherent model; file slots are read, not mutated, and registries must
 *     match Part A rows.
 * @param[in] sim Borrowed simulation key copied into inserted/updated rows.
 */
void writeTimeseriesSlots(sqlite3* db, const SimulationContext& ctx, const std::string& sim) {
    for (std::size_t t = 0; t < ctx.tables.tables.size(); ++t) {
        const auto& tbl = ctx.tables.tables[t];
        if (tbl.type != TableType::TIMESERIES)
            continue;
        if (tbl.file_path.empty())
            continue;
        auto [path, column] = splitColumn(fopenToken(tbl.file_path));
        if (!fileExists(path)) {
            throw GpkgError("timeseries FILE reference not readable: '" + path + "'");
        }
        std::vector<formats::TimeseriesRow> rows;
        auto r = formats::parseTimeseriesText(path, rows);
        if (!r.ok)
            throw GpkgError("timeseries parse failed: " + r.error);
        const std::string series_id = ctx.tables[static_cast<int>(t)].id;
        insert_input_timeseries(db, sim, series_id, basenameOf(path), column, rows);
    }
}

/**
 * @brief Parse and embed every nonempty gage external-file slot.
 *
 * Strips an optional column suffix, requires readable standard gage text and throws GpkgError for
 * read/parse failure. Parsing occurs before verifying a matching model gage row; extra file slots
 * can be read then discarded. Rainfall values retain source units. Preparation/allocation may
 * throw; bind and step return codes are ignored, so void completion does not certify row insertion.
 * No local transaction or rollback.
 *
 * @param[in,out] db Borrowed writable schema-compatible SQLite connection; caller owns transaction,
 *     cleanup and parent catalogue rows.
 * @param[in] ctx Borrowed coherent model; file slots are read, not mutated, and registries must
 *     match Part A rows.
 * @param[in] sim Borrowed simulation key copied into inserted/updated rows.
 */
void writeRaingageSlots(sqlite3* db, const SimulationContext& ctx, const std::string& sim) {
    for (std::size_t i = 0; i < ctx.gages.file_path.size(); ++i) {
        const auto& slot = ctx.gages.file_path[i];
        if (slot.empty())
            continue;
        auto [path, /*column*/ _] = splitColumn(fopenToken(slot));
        if (!fileExists(path)) {
            throw GpkgError("raingage FILE reference not readable: '" + path + "'");
        }
        std::vector<formats::RaingageRow> rows;
        auto r = formats::parseRaingageStd(path, rows);
        if (!r.ok)
            throw GpkgError("raingage parse failed: " + r.error);
        if (static_cast<int>(i) >= ctx.gage_names.size())
            continue;
        const std::string gage_id = ctx.gage_names.name_of(static_cast<int>(i));
        insert_raingage_data(db, sim, gage_id, rows);
    }
}

/**
 * @brief Parse and embed the nonempty model temperature-file slot.
 *
 * Requires readable climate CSV and throws GpkgError on failure. Uses whole effective token without
 * :column stripping and copies format values without conversion. No-op for an empty slot.
 * Preparation/allocation may throw; bind and step return codes are ignored, so void completion does
 * not certify row insertion. No local transaction or rollback.
 *
 * @param[in,out] db Borrowed writable schema-compatible SQLite connection; caller owns transaction,
 *     cleanup and parent catalogue rows.
 * @param[in] ctx Borrowed coherent model; file slots are read, not mutated, and registries must
 *     match Part A rows.
 * @param[in] sim Borrowed simulation key copied into inserted/updated rows.
 */
void writeClimateSlot(sqlite3* db, const SimulationContext& ctx, const std::string& sim) {
    if (ctx.options.temp_file.empty())
        return;
    const std::string path = fopenToken(ctx.options.temp_file);
    if (!fileExists(path)) {
        throw GpkgError("climate FILE reference not readable: '" + path + "'");
    }
    std::vector<formats::ClimateRow> rows;
    auto r = formats::parseClimateCsv(path, rows);
    if (!r.ok)
        throw GpkgError("climate parse failed: " + r.error);
    insert_climate_data(db, sim, rows);
}

/**
 * @brief Parse one nonempty routing-interface slot and dispatch observations by object kind.
 *
 * Missing non-USE file is omitted without a pending-row record. Readable files must parse or throw
 * GpkgError. NODE retains available pollutant children; other kinds retain only flow/rainfall.
 * Parser metadata is not checked against ctx or persisted as a unit catalogue; values retain input
 * file units. Preparation/allocation may throw; bind and step return codes are ignored, so void
 * completion does not certify row insertion. No local transaction or rollback.
 *
 * @param[in,out] db Borrowed writable schema-compatible SQLite connection; caller owns transaction,
 *     cleanup and parent catalogue rows.
 * @param[in] ctx Borrowed coherent model; file slots are read, not mutated, and registries must
 *     match Part A rows.
 * @param[in] sim Borrowed simulation key copied into inserted/updated rows.
 * @param[in] slot Borrowed path pair; empty performs no work.
 * @param[in] role Non-NULL NUL-terminated role copied into rows and diagnostics.
 * @param[in] mode SAVE labels SAVE; other values label USE. Only exact USE makes missing files an
 *     error.
 * @param[in] object_kind Non-NULL exact NODE/SUBCATCH/GAGE token; unknown tokens parse the file
 *     then insert no rows.
 */
void writeRoutingInterfaceSlot(sqlite3* db, const SimulationContext& ctx, const std::string& sim,
                               const FilePathPair& slot, const char* role, FileMode mode,
                               const char* object_kind) {
    if (slot.empty())
        return;
    const std::string path = fopenToken(slot);
    const char* direction = (mode == FileMode::SAVE) ? "SAVE" : "USE";

    if (!fileExists(path)) {
        if (mode == FileMode::USE) {
            throw GpkgError(std::string("routing interface USE file not "
                                        "readable: '") +
                            path + "'");
        }
        // SAVE-direction: file will be populated after the run; nothing
        // to insert today. The legacy schema has no pending-row registry
        // for routing-interface SAVE; the slot is implicit via the
        // [FILES] entry that persists through the [OPTIONS] block on
        // open and is re-discovered by the writer on subsequent saves.
        return;
    }

    formats::RoutingInterfaceMetadata meta;
    std::vector<formats::RoutingInterfaceRow> rows;
    auto r = formats::parseRoutingInterfaceText(path, meta, rows);
    if (!r.ok) {
        throw GpkgError(std::string("routing interface parse failed for '") + path +
                        "': " + r.error);
    }
    (void)ctx; // metadata could later be cross-checked against ctx
    for (const auto& row : rows) {
        const std::string kind = object_kind;
        if (kind == "NODE")
            insert_routing_node_row(db, sim, role, direction, row, meta.pollutant_ids);
        else if (kind == "SUBCATCH")
            insert_routing_subcatch_row(db, sim, role, direction, row);
        else if (kind == "GAGE")
            insert_routing_gage_row(db, sim, role, direction, row);
    }
}

/**
 * @brief Map a legacy flow-unit ordinal to its static label.
 *
 * @param[in] fu Candidate CFS/GPM/MGD/CMS/LPS/MLD ordinal0..5.
 * @return Static NUL-terminated label, empty string outside0..5; never free.
 */
const char* flowUnitsToken(int fu) {
    static const char* sFlowUnits[] = {"CFS", "GPM", "MGD", "CMS", "LPS", "MLD"};
    return (fu >= 0 && fu < 6) ? sFlowUnits[fu] : "";
}

/**
 * @brief Embed the configured USE hotstart only when it is legacy SWMM5 format.
 *
 * Empty slot is a no-op; unreadable or native OpenSWMM file throws GpkgError. Parses using the
 * model-borrowing legacy layout, inserts populated use parent then state. Does not validate every
 * registry/layout correspondence or restore runtime state. Preparation/allocation may throw; bind
 * and step return codes are ignored, so void completion does not certify row insertion. No local
 * transaction or rollback.
 *
 * @param[in,out] db Borrowed writable schema-compatible SQLite connection; caller owns transaction,
 *     cleanup and parent catalogue rows.
 * @param[in] ctx Borrowed coherent model; file slots are read, not mutated, and registries must
 *     match Part A rows.
 * @param[in] sim Borrowed simulation key copied into inserted/updated rows.
 */
void writeHotstartUseSlot(sqlite3* db, const SimulationContext& ctx, const std::string& sim) {
    const auto& slot = ctx.files.hotstart_use_path;
    if (slot.empty())
        return;
    const std::string path = fopenToken(slot);
    if (!fileExists(path)) {
        throw GpkgError("hotstart USE file not readable: '" + path + "'");
    }
    if (HotStartManager::is_native_file(path)) {
        throw GpkgError("hotstart USE file '" + path +
                        "' is a native "
                        "OpenSWMM hotstart; a GeoPackage can embed only the "
                        "legacy SWMM5 format");
    }
    formats::HotstartSnapshot snap;
    auto r = formats::parseHotstartHsf(path, snap, hsfLayout(ctx));
    if (!r.ok)
        throw GpkgError("hotstart parse failed: " + r.error);

    insert_hotstart_slot_row(db, sim, "use", "USE", 0.0, snap.header.file_version,
                             flowUnitsToken(snap.header.flow_units), snap.header.num_pollut,
                             "populated");
    insert_hotstart_state(db, sim, "use", ctx, snap);
}

/**
 * @brief Embed existing legacy SAVE hotstarts or persist pending slot metadata.
 *
 * Names entries save_N using original vector position, skipping empty paths.
 * Missing/unreadable/native files become pending v4 rows with model counts and requested save
 * format; readable legacy files parse into populated state or throw GpkgError. Retains OADate
 * schedule. Does not execute saves or modify source files. Preparation/allocation may throw; bind
 * and step return codes are ignored, so void completion does not certify row insertion. No local
 * transaction or rollback.
 *
 * @param[in,out] db Borrowed writable schema-compatible SQLite connection; caller owns transaction,
 *     cleanup and parent catalogue rows.
 * @param[in] ctx Borrowed coherent model; file slots are read, not mutated, and registries must
 *     match Part A rows.
 * @param[in] sim Borrowed simulation key copied into inserted/updated rows.
 */
void writeHotstartSaveSlots(sqlite3* db, const SimulationContext& ctx, const std::string& sim) {
    int slot_idx = 0;
    for (const auto& save : ctx.files.hotstart_saves) {
        const std::string slot_name = "save_" + std::to_string(slot_idx++);
        if (save.path.empty())
            continue;
        const std::string path = fopenToken(save.path);
        // Pending: parent row only, no state. A native file from an earlier
        // run is also left out: the state tables hold the legacy layout only,
        // and the run that follows writes the file again.
        if (!fileExists(path) || HotStartManager::is_native_file(path)) {
            insert_hotstart_slot_row(db, sim, slot_name, "SAVE", save.datetime,
                                     /*format_version=*/4,
                                     flowUnitsToken(static_cast<int>(ctx.options.flow_units)),
                                     ctx.n_pollutants(), "pending", save.format);
            continue;
        }
        formats::HotstartSnapshot snap;
        auto r = formats::parseHotstartHsf(path, snap, hsfLayout(ctx));
        if (!r.ok) {
            throw GpkgError("hotstart SAVE parse failed for '" + path + "': " + r.error);
        }
        insert_hotstart_slot_row(db, sim, slot_name, "SAVE", save.datetime,
                                 snap.header.file_version, flowUnitsToken(snap.header.flow_units),
                                 snap.header.num_pollut, "populated", save.format);
        insert_hotstart_state(db, sim, slot_name, ctx, snap);
    }
}

} // anonymous namespace

// ============================================================================
// Public entry
// ============================================================================

void write_external_content(sqlite3* db, const openswmm::SimulationContext& ctx,
                            const std::string& sim) {
    // Order is deliberate: timeseries / raingage / climate first (they
    // depend only on the model objects in Part A); then routing interface
    // and hotstart, which depend on the pollutant catalogue Part A wrote
    // a moment earlier.
    writeTimeseriesSlots(db, ctx, sim);
    writeRaingageSlots(db, ctx, sim);
    writeClimateSlot(db, ctx, sim);

    writeRoutingInterfaceSlot(db, ctx, sim, ctx.files.rainfall_path, "RAINFALL",
                              ctx.files.rainfall_mode, "GAGE");
    writeRoutingInterfaceSlot(db, ctx, sim, ctx.files.runoff_path, "RUNOFF", ctx.files.runoff_mode,
                              "SUBCATCH");
    writeRoutingInterfaceSlot(db, ctx, sim, ctx.files.rdii_path, "RDII", ctx.files.rdii_mode,
                              "NODE");
    writeRoutingInterfaceSlot(db, ctx, sim, ctx.files.inflows_path, "INFLOWS", FileMode::USE,
                              "NODE");
    writeRoutingInterfaceSlot(db, ctx, sim, ctx.files.outflows_path, "OUTFLOWS", FileMode::SAVE,
                              "NODE");

    writeHotstartUseSlot(db, ctx, sim);
    writeHotstartSaveSlots(db, ctx, sim);
}

} // namespace openswmm::gpkg
