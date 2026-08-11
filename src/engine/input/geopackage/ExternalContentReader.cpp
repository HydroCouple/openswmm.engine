/**
 * @file ExternalContentReader.cpp
 * @brief Slice IO-8 — Part D content → SimulationContext slot hydration +
 *        scratch-file materialisation.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "ExternalContentReader.hpp"
#include "GpkgUtils.hpp"
#include "formats/ClimateFormat.hpp"
#include "formats/HotstartFormat.hpp"
#include "formats/RaingageFormat.hpp"
#include "formats/RoutingInterfaceFormat.hpp"
#include "formats/TimeseriesFormat.hpp"

#include "../../core/DateTime.hpp"
#include "../../core/FilePathPair.hpp"
#include "../../core/SimulationContext.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace openswmm::gpkg {

namespace {

using openswmm::FilePathPair;
using openswmm::SimulationContext;
using openswmm::TableType;
using openswmm::FileMode;

// ============================================================================
// Helpers
// ============================================================================

// Parse an ISO-8601 timestamp written by the writer ("YYYY-MM-DDTHH:MM:SSZ")
// into an OADate value. Loose — we only support the writer's exact form.
double isoToOaDate(const char* iso) {
    if (!iso) return 0.0;
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (std::sscanf(iso, "%d-%d-%dT%d:%d:%dZ",
                     &y, &mo, &d, &h, &mi, &s) < 3)
        return 0.0;
    return datetime::encodeDate(y, mo, d) + datetime::encodeTime(h, mi, s);
}

void ensureDir(const std::string& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
}

std::string joinScratch(const std::string& scratch_dir,
                         const std::string& filename) {
    fs::path p = fs::path(scratch_dir) / filename;
    return p.string();
}

void bindSlot(FilePathPair& slot,
               const std::string& sentinel,
               const std::string& scratch_path) {
    slot.original = sentinel;
    slot.absolute = scratch_path;
}

// ============================================================================
// Per-role materialise + hydrate
// ============================================================================

void hydrateTimeseries(sqlite3* db, SimulationContext& ctx,
                       const std::string& sim, const std::string& scratch) {
    // Pull every series imported from file along with its original
    // source_filename so the scratch file has a recognisable name.
    auto stmt = prepare(db,
        "SELECT DISTINCT series_id, source_filename, source_column "
        "FROM input_timeseries "
        "WHERE simulation_id=? AND source='imported_from_file'");
    bind_text(stmt.get(), 1, sim);

    struct Spec { std::string filename, column; };
    std::unordered_map<std::string, Spec> series_specs;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        const char* sid = reinterpret_cast<const char*>(
            sqlite3_column_text(stmt.get(), 0));
        const char* fn  = reinterpret_cast<const char*>(
            sqlite3_column_text(stmt.get(), 1));
        const char* col = reinterpret_cast<const char*>(
            sqlite3_column_text(stmt.get(), 2));
        if (!sid) continue;
        Spec s;
        s.filename = fn  ? fn  : (std::string("ts_") + sid + ".dat");
        s.column   = col ? col : "";
        series_specs[sid] = std::move(s);
    }

    for (const auto& kv : series_specs) {
        const std::string& sid  = kv.first;
        const Spec&        spec = kv.second;

        // Load rows in ordinal order.
        auto rs = prepare(db,
            "SELECT timestamp, value FROM input_timeseries "
            "WHERE simulation_id=? AND series_id=? "
            "ORDER BY ordinal");
        bind_text(rs.get(), 1, sim);
        bind_text(rs.get(), 2, sid);

        std::vector<formats::TimeseriesRow> rows;
        while (sqlite3_step(rs.get()) == SQLITE_ROW) {
            formats::TimeseriesRow r;
            const char* ts = reinterpret_cast<const char*>(
                sqlite3_column_text(rs.get(), 0));
            r.timestamp_oa = isoToOaDate(ts);
            r.value        = sqlite3_column_double(rs.get(), 1);
            rows.push_back(r);
        }

        const std::string scratch_path = joinScratch(scratch, spec.filename);
        auto wr = formats::writeTimeseriesText(scratch_path, rows);
        if (!wr.ok) throw GpkgError("timeseries materialise failed: " + wr.error);

        // Bind to the matching timeseries Table in ctx (create if absent).
        int t = ctx.find_timeseries(sid);
        if (t < 0) {
            t = ctx.tables.add(sid, TableType::TIMESERIES);
        }
        std::string token = spec.column.empty()
                              ? scratch_path
                              : scratch_path + ":" + spec.column;
        bindSlot(ctx.tables[t].file_path,
                  "<gpkg:input_timeseries:" + sid + ">",
                  token);
    }
}

void hydrateRaingage(sqlite3* db, SimulationContext& ctx,
                     const std::string& sim, const std::string& scratch) {
    // Distinct gages with data rows.
    auto stmt = prepare(db,
        "SELECT DISTINCT gage_id FROM raingage_data WHERE simulation_id=?");
    bind_text(stmt.get(), 1, sim);

    std::vector<std::string> gages;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        const char* g = reinterpret_cast<const char*>(
            sqlite3_column_text(stmt.get(), 0));
        if (g) gages.emplace_back(g);
    }

    for (const auto& gid : gages) {
        auto rs = prepare(db,
            "SELECT record_time, rainfall_value, station_id "
            "FROM raingage_data WHERE simulation_id=? AND gage_id=? "
            "ORDER BY record_time");
        bind_text(rs.get(), 1, sim);
        bind_text(rs.get(), 2, gid);

        std::vector<formats::RaingageRow> rows;
        while (sqlite3_step(rs.get()) == SQLITE_ROW) {
            formats::RaingageRow r;
            const char* ts = reinterpret_cast<const char*>(
                sqlite3_column_text(rs.get(), 0));
            r.timestamp_oa = isoToOaDate(ts);
            r.rainfall     = sqlite3_column_double(rs.get(), 1);
            const char* st = reinterpret_cast<const char*>(
                sqlite3_column_text(rs.get(), 2));
            r.station_id   = st ? st : gid;
            rows.push_back(r);
        }

        const std::string scratch_path =
            joinScratch(scratch, "raingage_" + gid + ".dat");
        auto wr = formats::writeRaingageStd(scratch_path, rows);
        if (!wr.ok) throw GpkgError("raingage materialise failed: " + wr.error);

        int g = ctx.gage_names.find(gid);
        if (g < 0) continue;  // No matching gage in model — silently skip
        if (static_cast<int>(ctx.gages.file_path.size()) <= g)
            ctx.gages.file_path.resize(static_cast<std::size_t>(g) + 1);
        bindSlot(ctx.gages.file_path[static_cast<std::size_t>(g)],
                  "<gpkg:raingage_data:" + gid + ">",
                  scratch_path);
    }
}

void hydrateClimate(sqlite3* db, SimulationContext& ctx,
                    const std::string& sim, const std::string& scratch) {
    // Single climate file per simulation.
    auto stmt = prepare(db,
        "SELECT record_date, tmin, tmax, evaporation, "
        "       wind_speed, sky_cover, humidity "
        "FROM climate_data WHERE simulation_id=? ORDER BY record_date");
    bind_text(stmt.get(), 1, sim);

    std::vector<formats::ClimateRow> rows;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        formats::ClimateRow r;
        const char* d = reinterpret_cast<const char*>(
            sqlite3_column_text(stmt.get(), 0));
        r.record_date  = d ? d : "";
        r.tmin         = sqlite3_column_double(stmt.get(), 1);
        r.tmax         = sqlite3_column_double(stmt.get(), 2);
        r.evaporation  = sqlite3_column_double(stmt.get(), 3);
        r.wind_speed   = sqlite3_column_double(stmt.get(), 4);
        r.sky_cover    = sqlite3_column_double(stmt.get(), 5);
        r.humidity     = sqlite3_column_double(stmt.get(), 6);
        rows.push_back(std::move(r));
    }
    if (rows.empty()) return;

    const std::string scratch_path = joinScratch(scratch, "climate.csv");
    auto wr = formats::writeClimateCsv(scratch_path, rows);
    if (!wr.ok) throw GpkgError("climate materialise failed: " + wr.error);

    bindSlot(ctx.options.temp_file,
              "<gpkg:climate_data>",
              scratch_path);
}

void hydrateRoutingInterface(sqlite3* db, SimulationContext& ctx,
                              const std::string& sim,
                              const std::string& scratch) {
    // For each (role, direction) combination present in routing_interface_node,
    // assemble metadata + rows and materialise an interface text file.
    // Node-kind covers INFLOWS / OUTFLOWS / RDII.
    auto roles = prepare(db,
        "SELECT DISTINCT role, direction "
        "FROM routing_interface_node WHERE simulation_id=?");
    bind_text(roles.get(), 1, sim);

    struct Combo { std::string role, dir; };
    std::vector<Combo> combos;
    while (sqlite3_step(roles.get()) == SQLITE_ROW) {
        Combo c;
        const char* r = reinterpret_cast<const char*>(
            sqlite3_column_text(roles.get(), 0));
        const char* dr = reinterpret_cast<const char*>(
            sqlite3_column_text(roles.get(), 1));
        if (r)  c.role = r;
        if (dr) c.dir  = dr;
        combos.push_back(std::move(c));
    }

    for (const auto& c : combos) {
        // Pollutant catalogue for this role/direction.
        formats::RoutingInterfaceMetadata meta;
        meta.title           = "OpenSWMM " + c.role;
        meta.report_step_sec = 0;
        meta.flow_units      = "CFS";

        auto pstmt = prepare(db,
            "SELECT DISTINCT pollutant_id FROM routing_interface_node_pollutants "
            "WHERE simulation_id=? AND role=? AND direction=? "
            "ORDER BY pollutant_id");
        bind_text(pstmt.get(), 1, sim);
        bind_text(pstmt.get(), 2, c.role);
        bind_text(pstmt.get(), 3, c.dir);
        while (sqlite3_step(pstmt.get()) == SQLITE_ROW) {
            const char* p = reinterpret_cast<const char*>(
                sqlite3_column_text(pstmt.get(), 0));
            if (p) { meta.pollutant_ids.emplace_back(p);
                     meta.pollutant_units.emplace_back("MG/L"); }
        }

        // Object catalogue (node ids).
        auto ostmt = prepare(db,
            "SELECT DISTINCT node_id FROM routing_interface_node "
            "WHERE simulation_id=? AND role=? AND direction=? "
            "ORDER BY node_id");
        bind_text(ostmt.get(), 1, sim);
        bind_text(ostmt.get(), 2, c.role);
        bind_text(ostmt.get(), 3, c.dir);
        while (sqlite3_step(ostmt.get()) == SQLITE_ROW) {
            const char* n = reinterpret_cast<const char*>(
                sqlite3_column_text(ostmt.get(), 0));
            if (n) meta.object_ids.emplace_back(n);
        }

        // Data rows + pollutant child rows.
        auto rstmt = prepare(db,
            "SELECT node_id, record_time, flow_value "
            "FROM routing_interface_node "
            "WHERE simulation_id=? AND role=? AND direction=? "
            "ORDER BY record_time, node_id");
        bind_text(rstmt.get(), 1, sim);
        bind_text(rstmt.get(), 2, c.role);
        bind_text(rstmt.get(), 3, c.dir);

        std::vector<formats::RoutingInterfaceRow> rows;
        while (sqlite3_step(rstmt.get()) == SQLITE_ROW) {
            formats::RoutingInterfaceRow r;
            const char* nid = reinterpret_cast<const char*>(
                sqlite3_column_text(rstmt.get(), 0));
            const char* ts  = reinterpret_cast<const char*>(
                sqlite3_column_text(rstmt.get(), 1));
            r.object_id    = nid ? nid : "";
            r.timestamp_oa = isoToOaDate(ts);
            r.flow_value   = sqlite3_column_double(rstmt.get(), 2);

            // Pollutant values in catalogue order.
            r.pollutant_values.resize(meta.pollutant_ids.size(), 0.0);
            auto qstmt = prepare(db,
                "SELECT pollutant_id, concentration "
                "FROM routing_interface_node_pollutants "
                "WHERE simulation_id=? AND role=? AND direction=? "
                " AND node_id=? AND record_time=?");
            bind_text(qstmt.get(), 1, sim);
            bind_text(qstmt.get(), 2, c.role);
            bind_text(qstmt.get(), 3, c.dir);
            bind_text(qstmt.get(), 4, r.object_id);
            bind_text(qstmt.get(), 5, ts ? ts : "");
            while (sqlite3_step(qstmt.get()) == SQLITE_ROW) {
                const char* pid = reinterpret_cast<const char*>(
                    sqlite3_column_text(qstmt.get(), 0));
                if (!pid) continue;
                auto it = std::find(meta.pollutant_ids.begin(),
                                     meta.pollutant_ids.end(),
                                     std::string(pid));
                if (it != meta.pollutant_ids.end()) {
                    auto idx = static_cast<std::size_t>(
                        it - meta.pollutant_ids.begin());
                    r.pollutant_values[idx] =
                        sqlite3_column_double(qstmt.get(), 1);
                }
            }
            rows.push_back(std::move(r));
        }

        const std::string scratch_path = joinScratch(
            scratch, "routing_" + c.role + "_" + c.dir + ".txt");
        auto wr = formats::writeRoutingInterfaceText(scratch_path, meta, rows);
        if (!wr.ok)
            throw GpkgError("routing interface materialise failed: " + wr.error);

        // Bind to the right ctx.files slot based on role + direction.
        FilePathPair* target = nullptr;
        if      (c.role == "RDII")     target = &ctx.files.rdii_path;
        else if (c.role == "INFLOWS")  target = &ctx.files.inflows_path;
        else if (c.role == "OUTFLOWS") target = &ctx.files.outflows_path;
        if (target) {
            bindSlot(*target,
                      "<gpkg:routing_interface_node:" + c.role + ":" + c.dir + ">",
                      scratch_path);
            if (c.role == "RDII") {
                ctx.files.rdii_mode = (c.dir == "SAVE") ? FileMode::SAVE
                                                         : FileMode::USE;
            }
        }
    }
    // RUNOFF / RAINFALL kinds covered analogously when subcatch / gage
    // routing-interface rows accumulate (deferred sub-slice — the
    // schemas are in place, hydration is a future extension).
}

void hydrateHotstart(sqlite3* db, SimulationContext& ctx,
                     const std::string& sim, const std::string& scratch) {
    // Each row in hotstart_slots becomes one scratch HSF file.
    auto stmt = prepare(db,
        "SELECT slot_name, direction, save_datetime, format_version, "
        "       flow_units, num_pollutants, status "
        "FROM hotstart_slots WHERE simulation_id=? ORDER BY slot_name");
    bind_text(stmt.get(), 1, sim);

    struct SlotMeta {
        std::string name, direction, flow_units, status;
        double save_datetime = 0.0;
        int    format_version = 4;
        int    num_pollutants = 0;
    };
    std::vector<SlotMeta> slots;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        SlotMeta s;
        const char* n  = reinterpret_cast<const char*>(
            sqlite3_column_text(stmt.get(), 0));
        const char* dr = reinterpret_cast<const char*>(
            sqlite3_column_text(stmt.get(), 1));
        const char* fu = reinterpret_cast<const char*>(
            sqlite3_column_text(stmt.get(), 4));
        const char* st = reinterpret_cast<const char*>(
            sqlite3_column_text(stmt.get(), 6));
        s.name           = n  ? n  : "";
        s.direction      = dr ? dr : "USE";
        s.save_datetime  = sqlite3_column_double(stmt.get(), 2);
        s.format_version = sqlite3_column_int(stmt.get(), 3);
        s.flow_units     = fu ? fu : "CFS";
        s.num_pollutants = sqlite3_column_int(stmt.get(), 5);
        s.status         = st ? st : "pending";
        slots.push_back(std::move(s));
    }

    static const char* sFlowUnits[] = {"CFS","GPM","MGD","CMS","LPS","MLD"};
    auto flowUnitsIdx = [&](const std::string& u) -> int {
        for (int i = 0; i < 6; ++i) if (u == sFlowUnits[i]) return i;
        return 0;
    };

    int save_slot_idx = 0;
    for (const auto& s : slots) {
        // SAVE pending: bind the slot's path to a "pending" scratch file
        // placeholder (zero-byte) so callers see a non-empty token; the
        // engine output plugin will overwrite this after the run.
        const std::string scratch_path =
            joinScratch(scratch, s.name + ".hsf");

        if (s.status != "populated") {
            // Create an empty placeholder so the path exists for the GUI.
            std::FILE* fp = std::fopen(scratch_path.c_str(), "wb");
            if (fp) std::fclose(fp);
        } else {
            // Build a HotstartSnapshot from state rows.
            formats::HotstartSnapshot snap;
            snap.header.file_stamp     = "SWMM5-HOTSTART4";
            snap.header.file_version   = s.format_version;
            snap.header.num_subcatch   = 0;
            snap.header.num_landuses   = 0;
            snap.header.num_pollut     = s.num_pollutants;
            snap.header.flow_units     = flowUnitsIdx(s.flow_units);

            // Node state — in ctx.node_names order so HSF index matches.
            const int n_nodes = ctx.node_names.size();
            snap.header.num_nodes = n_nodes;
            snap.node_state.resize(static_cast<std::size_t>(n_nodes));
            for (auto& ns : snap.node_state)
                ns.concentration.resize(static_cast<std::size_t>(
                    s.num_pollutants));
            {
                auto ns = prepare(db,
                    "SELECT node_id, depth, lateral_inflow, overflow "
                    "FROM hotstart_node_state "
                    "WHERE simulation_id=? AND slot_name=?");
                bind_text(ns.get(), 1, sim);
                bind_text(ns.get(), 2, s.name);
                while (sqlite3_step(ns.get()) == SQLITE_ROW) {
                    const char* nid = reinterpret_cast<const char*>(
                        sqlite3_column_text(ns.get(), 0));
                    int idx = ctx.node_names.find(nid ? nid : "");
                    if (idx < 0 || idx >= n_nodes) continue;
                    auto& dst = snap.node_state[static_cast<std::size_t>(idx)];
                    dst.depth          = sqlite3_column_double(ns.get(), 1);
                    dst.lateral_inflow = sqlite3_column_double(ns.get(), 2);
                    dst.overflow       = sqlite3_column_double(ns.get(), 3);
                }
            }
            // Node pollutant state.
            if (s.num_pollutants > 0) {
                auto ns = prepare(db,
                    "SELECT node_id, pollutant_id, concentration "
                    "FROM hotstart_node_pollutant_state "
                    "WHERE simulation_id=? AND slot_name=?");
                bind_text(ns.get(), 1, sim);
                bind_text(ns.get(), 2, s.name);
                while (sqlite3_step(ns.get()) == SQLITE_ROW) {
                    const char* nid = reinterpret_cast<const char*>(
                        sqlite3_column_text(ns.get(), 0));
                    const char* pid = reinterpret_cast<const char*>(
                        sqlite3_column_text(ns.get(), 1));
                    int n_idx = ctx.node_names.find(nid ? nid : "");
                    int p_idx = ctx.pollutant_names.find(pid ? pid : "");
                    if (n_idx < 0 || n_idx >= n_nodes) continue;
                    if (p_idx < 0 || p_idx >= s.num_pollutants) continue;
                    snap.node_state[static_cast<std::size_t>(n_idx)]
                        .concentration[static_cast<std::size_t>(p_idx)] =
                        sqlite3_column_double(ns.get(), 2);
                }
            }

            // Link state.
            const int n_links = ctx.link_names.size();
            snap.header.num_links = n_links;
            snap.link_state.resize(static_cast<std::size_t>(n_links));
            for (auto& ls : snap.link_state)
                ls.concentration.resize(static_cast<std::size_t>(
                    s.num_pollutants));
            {
                auto ls = prepare(db,
                    "SELECT link_id, flow, depth, volume, setting, "
                    "       target_setting, time_open, time_closed "
                    "FROM hotstart_link_state "
                    "WHERE simulation_id=? AND slot_name=?");
                bind_text(ls.get(), 1, sim);
                bind_text(ls.get(), 2, s.name);
                while (sqlite3_step(ls.get()) == SQLITE_ROW) {
                    const char* lid = reinterpret_cast<const char*>(
                        sqlite3_column_text(ls.get(), 0));
                    int idx = ctx.link_names.find(lid ? lid : "");
                    if (idx < 0 || idx >= n_links) continue;
                    auto& dst = snap.link_state[static_cast<std::size_t>(idx)];
                    dst.flow           = sqlite3_column_double(ls.get(), 1);
                    dst.depth          = sqlite3_column_double(ls.get(), 2);
                    dst.volume         = sqlite3_column_double(ls.get(), 3);
                    dst.setting        = sqlite3_column_double(ls.get(), 4);
                    dst.target_setting = sqlite3_column_double(ls.get(), 5);
                    dst.time_open      = sqlite3_column_double(ls.get(), 6);
                    dst.time_closed    = sqlite3_column_double(ls.get(), 7);
                }
            }
            // Link pollutant state.
            if (s.num_pollutants > 0) {
                auto ls = prepare(db,
                    "SELECT link_id, pollutant_id, concentration "
                    "FROM hotstart_link_pollutant_state "
                    "WHERE simulation_id=? AND slot_name=?");
                bind_text(ls.get(), 1, sim);
                bind_text(ls.get(), 2, s.name);
                while (sqlite3_step(ls.get()) == SQLITE_ROW) {
                    const char* lid = reinterpret_cast<const char*>(
                        sqlite3_column_text(ls.get(), 0));
                    const char* pid = reinterpret_cast<const char*>(
                        sqlite3_column_text(ls.get(), 1));
                    int l_idx = ctx.link_names.find(lid ? lid : "");
                    int p_idx = ctx.pollutant_names.find(pid ? pid : "");
                    if (l_idx < 0 || l_idx >= n_links) continue;
                    if (p_idx < 0 || p_idx >= s.num_pollutants) continue;
                    snap.link_state[static_cast<std::size_t>(l_idx)]
                        .concentration[static_cast<std::size_t>(p_idx)] =
                        sqlite3_column_double(ls.get(), 2);
                }
            }

            auto wr = formats::writeHotstartHsf(scratch_path, snap);
            if (!wr.ok)
                throw GpkgError("hotstart materialise failed: " + wr.error);
        }

        // Bind to ctx.files slot.
        if (s.direction == "USE") {
            bindSlot(ctx.files.hotstart_use_path,
                      "<gpkg:hotstart_slots:" + s.name + ">",
                      scratch_path);
        } else {
            openswmm::HotstartSaveEntry entry;
            entry.path     = std::string{};
            entry.datetime = s.save_datetime;
            bindSlot(entry.path,
                      "<gpkg:hotstart_slots:" + s.name + ">",
                      scratch_path);
            ctx.files.hotstart_saves.push_back(std::move(entry));
            (void)save_slot_idx;
        }
    }
}

} // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

std::string scratchDirFor(const std::string& gpkg_path) {
    fs::path p(gpkg_path);
    std::string stem = p.stem().string();
    if (stem.empty()) stem = "gpkg";
    const std::string leaf = stem + ".scratch";
    // Emit a portable forward-slash path. fs::path::operator/ joins with the
    // native separator ('\\' on Windows), which would yield mixed separators
    // like "/tmp\\foo.scratch"; build the sibling path with '/' instead.
    const fs::path parent = p.parent_path();
    if (parent.empty()) return leaf;
    return parent.generic_string() + "/" + leaf;
}

void read_external_content(sqlite3*               db,
                            SimulationContext&     ctx,
                            const std::string&     sim,
                            const std::string&     scratch_dir) {
    if (scratch_dir.empty()) return;
    ensureDir(scratch_dir);

    hydrateTimeseries        (db, ctx, sim, scratch_dir);
    hydrateRaingage          (db, ctx, sim, scratch_dir);
    hydrateClimate           (db, ctx, sim, scratch_dir);
    hydrateRoutingInterface  (db, ctx, sim, scratch_dir);
    hydrateHotstart          (db, ctx, sim, scratch_dir);
}

} // namespace openswmm::gpkg
