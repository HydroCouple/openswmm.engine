/**
 * @file ExternalContentWriter.cpp
 * @brief Slice IO-7 — implementation of the Part D content fan-out.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "ExternalContentWriter.hpp"
#include "GpkgUtils.hpp"
#include "formats/ClimateFormat.hpp"
#include "formats/HotstartFormat.hpp"
#include "formats/RaingageFormat.hpp"
#include "formats/RoutingInterfaceFormat.hpp"
#include "formats/TimeseriesFormat.hpp"

#include "../../core/DateTime.hpp"
#include "../../core/FilePathPair.hpp"
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

bool fileExists(const std::string& path) {
    if (path.empty()) return false;
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return false;
    std::fclose(fp);
    return true;
}

// Returns the path token most suitable for fopen: prefer `.absolute` when
// IO-3 resolution populated it, otherwise fall back to `.original`.
std::string fopenToken(const FilePathPair& slot) {
    return !slot.absolute.empty() ? slot.absolute : slot.str();
}

// Strip the optional `:column` suffix on a CSV-style external path.
// Preserves Windows drive letters ("C:\path"): only strips when the colon
// position is past index 1.
std::pair<std::string, std::string> splitColumn(std::string token) {
    auto pos = token.rfind(':');
    if (pos == std::string::npos || pos <= 1) return {std::move(token), {}};
    std::string column = token.substr(pos + 1);
    token.resize(pos);
    return {std::move(token), std::move(column)};
}

// POSIX/Win path basename. Just the filename component.
std::string basenameOf(const std::string& path) {
    auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

// Convert OADate (decimal days since 12/30/1899) to ISO-8601 timestamp.
std::string oadateToIso(double oa) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    datetime::decodeDate(oa, y, mo, d);
    datetime::decodeTime(oa, h, mi, s);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                   y, mo, d, h, mi, s);
    return buf;
}

// -------------------------------------------------------------------------
// Insertion helpers — one per Part D table family
// -------------------------------------------------------------------------

void insert_input_timeseries(sqlite3*                                   db,
                              const std::string&                         sim,
                              const std::string&                         series_id,
                              const std::string&                         source_filename,
                              const std::string&                         source_column,
                              const std::vector<formats::TimeseriesRow>& rows) {
    auto stmt = prepare(db,
        "INSERT INTO input_timeseries "
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
        if (source_column.empty()) sqlite3_bind_null(stmt.get(), 7);
        else                       bind_text(stmt.get(), 7, source_column);
        sqlite3_step(stmt.get());
    }
}

void insert_raingage_data(sqlite3*                                db,
                           const std::string&                      sim,
                           const std::string&                      gage_id,
                           const std::vector<formats::RaingageRow>& rows) {
    auto stmt = prepare(db,
        "INSERT INTO raingage_data "
        "(simulation_id, gage_id, record_time, rainfall_value, station_id) "
        "VALUES (?, ?, ?, ?, ?)");
    for (const auto& r : rows) {
        sqlite3_reset(stmt.get());
        bind_text(stmt.get(), 1, sim);
        bind_text(stmt.get(), 2, gage_id);
        bind_text(stmt.get(), 3, oadateToIso(r.timestamp_oa));
        bind_double(stmt.get(), 4, r.rainfall);
        if (r.station_id.empty()) sqlite3_bind_null(stmt.get(), 5);
        else                      bind_text(stmt.get(), 5, r.station_id);
        sqlite3_step(stmt.get());
    }
}

void insert_climate_data(sqlite3*                                db,
                          const std::string&                      sim,
                          const std::vector<formats::ClimateRow>& rows) {
    auto stmt = prepare(db,
        "INSERT INTO climate_data "
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
void insert_routing_node_row(sqlite3*    db,
                              const std::string& sim,
                              const std::string& role,
                              const std::string& direction,
                              const formats::RoutingInterfaceRow& row,
                              const std::vector<std::string>& pollutant_ids) {
    {
        auto stmt = prepare(db,
            "INSERT INTO routing_interface_node "
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
    for (std::size_t p = 0;
         p < pollutant_ids.size() && p < row.pollutant_values.size();
         ++p) {
        auto stmt = prepare(db,
            "INSERT INTO routing_interface_node_pollutants "
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

void insert_routing_subcatch_row(sqlite3* db,
                                  const std::string& sim,
                                  const std::string& role,
                                  const std::string& direction,
                                  const formats::RoutingInterfaceRow& row) {
    auto stmt = prepare(db,
        "INSERT INTO routing_interface_subcatch "
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

void insert_routing_gage_row(sqlite3* db,
                              const std::string& sim,
                              const std::string& role,
                              const std::string& direction,
                              const formats::RoutingInterfaceRow& row) {
    auto stmt = prepare(db,
        "INSERT INTO routing_interface_gage "
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

void insert_hotstart_slot_row(sqlite3*            db,
                               const std::string&  sim,
                               const std::string&  slot_name,
                               const std::string&  direction,
                               double              save_datetime,
                               int                 format_version,
                               const std::string&  flow_units,
                               int                 num_pollut,
                               const std::string&  status) {
    auto stmt = prepare(db,
        "INSERT INTO hotstart_slots "
        "(simulation_id, slot_name, direction, save_datetime, "
        " format_version, flow_units, num_pollutants, captured_at, status) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
    bind_text(stmt.get(), 1, sim);
    bind_text(stmt.get(), 2, slot_name);
    bind_text(stmt.get(), 3, direction);
    if (save_datetime > 0.0) bind_double(stmt.get(), 4, save_datetime);
    else                     sqlite3_bind_null(stmt.get(), 4);
    bind_int(stmt.get(),    5, format_version);
    if (flow_units.empty())  sqlite3_bind_null(stmt.get(), 6);
    else                     bind_text(stmt.get(), 6, flow_units);
    bind_int(stmt.get(),    7, num_pollut);
    if (status == "populated") bind_text(stmt.get(), 8, oadateToIso(save_datetime));
    else                       sqlite3_bind_null(stmt.get(), 8);
    bind_text(stmt.get(), 9, status);
    sqlite3_step(stmt.get());
}

void insert_hotstart_state(sqlite3*                       db,
                            const std::string&             sim,
                            const std::string&             slot,
                            const SimulationContext&       ctx,
                            const formats::HotstartSnapshot& snap) {
    // Node state (HSF order matches ctx.nodes order — SWMM convention).
    {
        auto stmt = prepare(db,
            "INSERT INTO hotstart_node_state "
            "(simulation_id, slot_name, node_id, depth, lateral_inflow, overflow) "
            "VALUES (?, ?, ?, ?, ?, ?)");
        for (std::size_t i = 0; i < snap.node_state.size(); ++i) {
            if (static_cast<int>(i) >= ctx.node_names.size()) break;
            const auto& s = snap.node_state[i];
            sqlite3_reset(stmt.get());
            bind_text(stmt.get(), 1, sim);
            bind_text(stmt.get(), 2, slot);
            bind_text(stmt.get(), 3, ctx.node_names.name_of(static_cast<int>(i)));
            bind_double(stmt.get(), 4, s.depth);
            bind_double(stmt.get(), 5, s.lateral_inflow);
            bind_double(stmt.get(), 6, s.overflow);
            sqlite3_step(stmt.get());
        }
    }
    // Node pollutant state.
    if (snap.header.num_pollut > 0) {
        auto stmt = prepare(db,
            "INSERT INTO hotstart_node_pollutant_state "
            "(simulation_id, slot_name, node_id, pollutant_id, concentration) "
            "VALUES (?, ?, ?, ?, ?)");
        for (std::size_t i = 0; i < snap.node_state.size(); ++i) {
            if (static_cast<int>(i) >= ctx.node_names.size()) break;
            const std::string node_id = ctx.node_names.name_of(static_cast<int>(i));
            const auto& concs = snap.node_state[i].concentration;
            for (std::size_t p = 0; p < concs.size(); ++p) {
                if (static_cast<int>(p) >= ctx.pollutant_names.size()) break;
                sqlite3_reset(stmt.get());
                bind_text(stmt.get(), 1, sim);
                bind_text(stmt.get(), 2, slot);
                bind_text(stmt.get(), 3, node_id);
                bind_text(stmt.get(), 4,
                          ctx.pollutant_names.name_of(static_cast<int>(p)));
                bind_double(stmt.get(), 5, concs[p]);
                sqlite3_step(stmt.get());
            }
        }
    }
    // Link state.
    {
        auto stmt = prepare(db,
            "INSERT INTO hotstart_link_state "
            "(simulation_id, slot_name, link_id, flow, depth, volume, "
            " setting, target_setting, time_open, time_closed) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
        for (std::size_t i = 0; i < snap.link_state.size(); ++i) {
            if (static_cast<int>(i) >= ctx.link_names.size()) break;
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
        auto stmt = prepare(db,
            "INSERT INTO hotstart_link_pollutant_state "
            "(simulation_id, slot_name, link_id, pollutant_id, concentration) "
            "VALUES (?, ?, ?, ?, ?)");
        for (std::size_t i = 0; i < snap.link_state.size(); ++i) {
            if (static_cast<int>(i) >= ctx.link_names.size()) break;
            const std::string link_id = ctx.link_names.name_of(static_cast<int>(i));
            const auto& concs = snap.link_state[i].concentration;
            for (std::size_t p = 0; p < concs.size(); ++p) {
                if (static_cast<int>(p) >= ctx.pollutant_names.size()) break;
                sqlite3_reset(stmt.get());
                bind_text(stmt.get(), 1, sim);
                bind_text(stmt.get(), 2, slot);
                bind_text(stmt.get(), 3, link_id);
                bind_text(stmt.get(), 4,
                          ctx.pollutant_names.name_of(static_cast<int>(p)));
                bind_double(stmt.get(), 5, concs[p]);
                sqlite3_step(stmt.get());
            }
        }
    }
    // Subcatch runoff state is out of scope for IO-6e (see HotstartFormat.hpp).
}

// -------------------------------------------------------------------------
// Slot dispatchers
// -------------------------------------------------------------------------

void writeTimeseriesSlots(sqlite3* db, const SimulationContext& ctx,
                           const std::string& sim) {
    for (std::size_t t = 0; t < ctx.tables.tables.size(); ++t) {
        const auto& tbl = ctx.tables.tables[t];
        if (tbl.type != TableType::TIMESERIES) continue;
        if (tbl.file_path.empty()) continue;
        auto [path, column] = splitColumn(fopenToken(tbl.file_path));
        if (!fileExists(path)) {
            throw GpkgError("timeseries FILE reference not readable: '"
                             + path + "'");
        }
        std::vector<formats::TimeseriesRow> rows;
        auto r = formats::parseTimeseriesText(path, rows);
        if (!r.ok) throw GpkgError("timeseries parse failed: " + r.error);
        const std::string series_id = ctx.tables[static_cast<int>(t)].id;
        insert_input_timeseries(db, sim, series_id, basenameOf(path),
                                  column, rows);
    }
}

void writeRaingageSlots(sqlite3* db, const SimulationContext& ctx,
                         const std::string& sim) {
    for (std::size_t i = 0; i < ctx.gages.file_path.size(); ++i) {
        const auto& slot = ctx.gages.file_path[i];
        if (slot.empty()) continue;
        auto [path, /*column*/_] = splitColumn(fopenToken(slot));
        if (!fileExists(path)) {
            throw GpkgError("raingage FILE reference not readable: '"
                             + path + "'");
        }
        std::vector<formats::RaingageRow> rows;
        auto r = formats::parseRaingageStd(path, rows);
        if (!r.ok) throw GpkgError("raingage parse failed: " + r.error);
        if (static_cast<int>(i) >= ctx.gage_names.size()) continue;
        const std::string gage_id =
            ctx.gage_names.name_of(static_cast<int>(i));
        insert_raingage_data(db, sim, gage_id, rows);
    }
}

void writeClimateSlot(sqlite3* db, const SimulationContext& ctx,
                       const std::string& sim) {
    if (ctx.options.temp_file.empty()) return;
    const std::string path = fopenToken(ctx.options.temp_file);
    if (!fileExists(path)) {
        throw GpkgError("climate FILE reference not readable: '" + path + "'");
    }
    std::vector<formats::ClimateRow> rows;
    auto r = formats::parseClimateCsv(path, rows);
    if (!r.ok) throw GpkgError("climate parse failed: " + r.error);
    insert_climate_data(db, sim, rows);
}

void writeRoutingInterfaceSlot(sqlite3*                   db,
                                 const SimulationContext&  ctx,
                                 const std::string&        sim,
                                 const FilePathPair&       slot,
                                 const char*               role,
                                 FileMode                  mode,
                                 const char*               object_kind) {
    if (slot.empty()) return;
    const std::string path = fopenToken(slot);
    const char* direction = (mode == FileMode::SAVE) ? "SAVE" : "USE";

    if (!fileExists(path)) {
        if (mode == FileMode::USE) {
            throw GpkgError(std::string("routing interface USE file not "
                                          "readable: '") + path + "'");
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
        throw GpkgError(std::string("routing interface parse failed for '")
                          + path + "': " + r.error);
    }
    (void)ctx;  // metadata could later be cross-checked against ctx
    for (const auto& row : rows) {
        const std::string kind = object_kind;
        if (kind == "NODE")
            insert_routing_node_row(db, sim, role, direction, row,
                                     meta.pollutant_ids);
        else if (kind == "SUBCATCH")
            insert_routing_subcatch_row(db, sim, role, direction, row);
        else if (kind == "GAGE")
            insert_routing_gage_row(db, sim, role, direction, row);
    }
}

const char* flowUnitsToken(int fu) {
    static const char* sFlowUnits[] = {"CFS","GPM","MGD","CMS","LPS","MLD"};
    return (fu >= 0 && fu < 6) ? sFlowUnits[fu] : "";
}

void writeHotstartUseSlot(sqlite3* db, const SimulationContext& ctx,
                           const std::string& sim) {
    const auto& slot = ctx.files.hotstart_use_path;
    if (slot.empty()) return;
    const std::string path = fopenToken(slot);
    if (!fileExists(path)) {
        throw GpkgError("hotstart USE file not readable: '" + path + "'");
    }
    formats::HotstartSnapshot snap;
    auto r = formats::parseHotstartHsf(path, snap);
    if (!r.ok) throw GpkgError("hotstart parse failed: " + r.error);

    insert_hotstart_slot_row(db, sim, "use", "USE",
                              0.0,
                              snap.header.file_version,
                              flowUnitsToken(snap.header.flow_units),
                              snap.header.num_pollut,
                              "populated");
    insert_hotstart_state(db, sim, "use", ctx, snap);
}

void writeHotstartSaveSlots(sqlite3* db, const SimulationContext& ctx,
                             const std::string& sim) {
    int slot_idx = 0;
    for (const auto& save : ctx.files.hotstart_saves) {
        const std::string slot_name = "save_" + std::to_string(slot_idx++);
        if (save.path.empty()) continue;
        const std::string path = fopenToken(save.path);
        if (!fileExists(path)) {
            // Pending: parent row only, no state.
            insert_hotstart_slot_row(db, sim, slot_name, "SAVE",
                                      save.datetime,
                                      /*format_version=*/4,
                                      flowUnitsToken(
                                          static_cast<int>(
                                              ctx.options.flow_units)),
                                      ctx.n_pollutants(),
                                      "pending");
            continue;
        }
        formats::HotstartSnapshot snap;
        auto r = formats::parseHotstartHsf(path, snap);
        if (!r.ok) {
            throw GpkgError("hotstart SAVE parse failed for '" + path
                              + "': " + r.error);
        }
        insert_hotstart_slot_row(db, sim, slot_name, "SAVE",
                                  save.datetime,
                                  snap.header.file_version,
                                  flowUnitsToken(snap.header.flow_units),
                                  snap.header.num_pollut,
                                  "populated");
        insert_hotstart_state(db, sim, slot_name, ctx, snap);
    }
}

} // anonymous namespace

// ============================================================================
// Public entry
// ============================================================================

void write_external_content(sqlite3*                            db,
                             const openswmm::SimulationContext& ctx,
                             const std::string&                  sim) {
    // Order is deliberate: timeseries / raingage / climate first (they
    // depend only on the model objects in Part A); then routing interface
    // and hotstart, which depend on the pollutant catalogue Part A wrote
    // a moment earlier.
    writeTimeseriesSlots(db, ctx, sim);
    writeRaingageSlots(db, ctx, sim);
    writeClimateSlot(db, ctx, sim);

    writeRoutingInterfaceSlot(db, ctx, sim, ctx.files.rainfall_path,
                                "RAINFALL", ctx.files.rainfall_mode, "GAGE");
    writeRoutingInterfaceSlot(db, ctx, sim, ctx.files.runoff_path,
                                "RUNOFF",   ctx.files.runoff_mode,   "SUBCATCH");
    writeRoutingInterfaceSlot(db, ctx, sim, ctx.files.rdii_path,
                                "RDII",     ctx.files.rdii_mode,     "NODE");
    writeRoutingInterfaceSlot(db, ctx, sim, ctx.files.inflows_path,
                                "INFLOWS",  FileMode::USE,           "NODE");
    writeRoutingInterfaceSlot(db, ctx, sim, ctx.files.outflows_path,
                                "OUTFLOWS", FileMode::SAVE,          "NODE");

    writeHotstartUseSlot(db, ctx, sim);
    writeHotstartSaveSlots(db, ctx, sim);
}

} // namespace openswmm::gpkg
