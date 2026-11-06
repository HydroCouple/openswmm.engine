// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "GpkgUtils.hpp"
#include "../../core/InpWriter.hpp"
#include "../../core/NativeModelBundle.hpp"
#include "../../core/SimulationContext.hpp"
#include "../../plugins/DefaultInputPlugin.hpp"
#include "../Tokenizer.hpp"
#include <set>
#include <sstream>
#include <iomanip>

namespace openswmm::gpkg {
inline void checked_model_step(sqlite3* db, sqlite3_stmt* statement) {
    const int rc = sqlite3_step(statement);
    if (rc != SQLITE_DONE) {
        const std::string error = sqlite3_errmsg(db);
        sqlite3_reset(statement);
        throw GpkgError(error, rc);
    }
}


// Relational columns remain authoritative. Native sections supply only fields
// the relational schema cannot represent, so GIS edits to existing tables still
// take effect. Numeric sections use the same internal-unit convention as tables.
inline void write_model_supplement(sqlite3* db, const SimulationContext& ctx,
                                   const std::string& simulation) {
    exec(db, "CREATE TABLE IF NOT EXISTS model_supplement (simulation_id TEXT PRIMARY KEY, "
             "version INTEGER NOT NULL, sections TEXT NOT NULL)");
    exec(db, "CREATE TABLE IF NOT EXISTS model_properties (simulation_id TEXT NOT NULL, "
             "scope TEXT NOT NULL, object_id TEXT NOT NULL, property TEXT NOT NULL, value TEXT NOT NULL, "
             "PRIMARY KEY(simulation_id,scope,object_id,property))");
    auto clear = prepare(db, "DELETE FROM model_properties WHERE simulation_id=?");
    bind_text(clear.get(), 1, simulation);
    if (sqlite3_step(clear.get()) != SQLITE_DONE) throw GpkgError(sqlite3_errmsg(db));
    auto property = prepare(db, "INSERT INTO model_properties VALUES (?,?,?,?,?)");
    auto put = [&](const std::string& scope, const std::string& id,
                   const std::string& key, const auto& value) {
        std::ostringstream out;
        out << std::setprecision(17) << value;
        sqlite3_reset(property.get());
        bind_text(property.get(), 1, simulation); bind_text(property.get(), 2, scope);
        bind_text(property.get(), 3, id); bind_text(property.get(), 4, key);
        bind_text(property.get(), 5, out.str());
        if (sqlite3_step(property.get()) != SQLITE_DONE) throw GpkgError(sqlite3_errmsg(db));
    };
    auto comments = [&](const char* scope, const auto& names, const auto& values) {
        for (int i = 0; i < names.size(); ++i)
            if (static_cast<std::size_t>(i) < values.size())
                put(scope, names.name_of(i), "comment", values[i]);
    };
    put("model", "", "title_count", ctx.title_notes.size());
    for (std::size_t i=0; i<ctx.title_notes.size(); ++i)
        put("title", std::to_string(i), "line", ctx.title_notes[i]);
    comments("node", ctx.node_names, ctx.nodes.comments);
    comments("link", ctx.link_names, ctx.links.comments);
    comments("subcatch", ctx.subcatch_names, ctx.subcatches.comments);
    comments("gage", ctx.gage_names, ctx.gages.comments);
    comments("pollutant", ctx.pollutant_names, ctx.pollutants.comments);
    for (int i = 0; i < ctx.n_tables(); ++i)
        put("table", ctx.tables[i].id, "comment", ctx.tables[i].comment);
    for (int i = 0; i < ctx.gage_names.size(); ++i)
        put("gage", ctx.gage_names.name_of(i), "scale",
            i < static_cast<int>(ctx.gages.scale_factor.size()) ? ctx.gages.scale_factor[i] : 1.0);
    for (int i = 0; i < ctx.n_subcatches(); ++i) {
        const auto& s = ctx.subcatches;
        std::string name = i < static_cast<int>(s.snowpack_name.size()) ? s.snowpack_name[i] : "";
        if (i < static_cast<int>(s.snowpack.size()) && s.snowpack[i] >= 0)
            name = ctx.snowpack_names.name_of(s.snowpack[i]);
        put("subcatch", ctx.subcatch_names.name_of(i), "snowpack", name);
    }
    for (int i = 0; i < ctx.n_pollutants(); ++i)
        put("pollutant", ctx.pollutant_names.name_of(i), "mwt",
            i < static_cast<int>(ctx.pollutants.mwt.size()) ? ctx.pollutants.mwt[i] : 1.0);
    for (int i = 0; i < ctx.n_links(); ++i) {
        const int row = ctx.link_subtypes.weir_row(i);
        if (row < 0) continue;
        const auto& w = ctx.link_subtypes.weirs;
        const auto& id = ctx.link_names.name_of(i);
        put("weir", id, "cd2", w.cd2[row]);
        put("weir", id, "end_contractions", w.end_contractions[row]);
        put("weir", id, "can_surcharge", int(w.can_surcharge[row]));
        put("weir", id, "road_width", w.road_width[row]);
        put("weir", id, "road_surface", int(w.road_surface[row]));
        put("weir", id, "cd_curve", w.cd_curve[row] >= 0 ? ctx.tables[w.cd_curve[row]].id : "");
    }

    for (const auto& [id, text] : native_model::configurations(ctx))
        put("component", id, "configuration", text);

    std::set<std::string> options;
    auto keys = prepare(db, "SELECT key FROM options WHERE simulation_id=?");
    bind_text(keys.get(), 1, simulation);
    while (sqlite3_step(keys.get()) == SQLITE_ROW) options.insert(column_text(keys.get(), 0));
    // Core sections already represented in normalized tables.
    const std::set<std::string> relational = {
        "TITLE", "EVAPORATION", "ADJUSTMENTS", "RAINGAGES", "SUBCATCHMENTS", "SUBAREAS", "INFILTRATION",
        "JUNCTIONS", "VIRTUAL_JUNCTIONS", "INLET_JUNCTIONS", "OUTFALLS", "DIVIDERS", "STORAGE",
        "CONDUITS", "PUMPS", "ORIFICES", "WEIRS", "OUTLETS", "XSECTIONS", "LOSSES",
        "TRANSECTS", "STREETS", "INLETS", "INLET_USAGE", "2D_NODE_COUPLING",
        "POLLUTANTS", "LANDUSES", "BUILDUP", "WASHOFF", "COVERAGES", "LOADINGS", "TREATMENT",
        "INFLOWS", "DWF", "RDII", "HYDROGRAPHS", "PATTERNS", "TIMESERIES", "CURVES",
        "CONTROLS", "REPORT", "MAP", "COORDINATES", "VERTICES", "POLYGONS", "SYMBOLS",
        "LID_USAGE", "LID_NODES", "LID_NODE_OUTLETS", "LID_LAYER_TREATMENT", "LID_RICHARDS",
        "2D_VERTICES", "2D_TRIANGLES", "2D_QUADS", "2D_VERTEX_NODE_MAP", "2D_TRIANGLE_NODE_MAP",
        "2D_BOUNDARY_CONDITIONS", "2D_EDGE_CONVEYANCE", "FILES"};
    const char* destination = sqlite3_db_filename(db, "main");
    std::istringstream native(inp_writer::modelText(ctx, destination && *destination ? destination : ctx.inp_file_path, true));
    std::string line, section, previous;
    std::ostringstream extra;
    while (std::getline(native, line)) {
        if (!line.empty() && line.front() == '[') {
            section = input::Tokenizer::to_upper(line.substr(1, line.find(']') - 1));
            continue;
        }
        auto words = input::Tokenizer::tokenize(line);
        if (words.empty() || relational.contains(section)) continue;
        if (section == "OPTIONS") {
            const std::string key = words[0];
            // Dates are represented together in the relational option values.
            if (options.contains(key) || key == "FLOW_ROUTING" || key == "FORCE_MAIN_EQUATION" ||
                key == "START_TIME" || key == "END_TIME" || key.starts_with("REPORT_START_")) continue;
        } else if (section == "2D_OPTIONS") {
            if (options.contains("2D_" + words[0])) continue;
        } else if (section == "TEMPERATURE") {
            if (words[0] == "TIMESERIES" || words[0] == "FILE" || words[0] == "WINDSPEED" ||
                words[0] == "SNOWMELT" || words[0] == "ADC") continue;
        } else if (section == "SNOWPACKS") {
            if (words.size() < 2 || words[1] != "UEB") continue;
        } else if (section == "LID_CONTROLS") {
            if (words.size() < 2 || (words[1] != "REMOVALS" && words[1] != "DRAIN")) continue;
        }
        if (section != previous) { extra << '\n' << '[' << section << "]\n"; previous = section; }
        extra << line << '\n';
    }
    auto row = prepare(db, "INSERT OR REPLACE INTO model_supplement VALUES (?,1,?)");
    bind_text(row.get(), 1, simulation); bind_text(row.get(), 2, extra.str());
    if (sqlite3_step(row.get()) != SQLITE_DONE) throw GpkgError(sqlite3_errmsg(db));
}

inline bool has_model_supplement(sqlite3* db, const std::string& simulation) {
    auto exists = prepare(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='model_supplement'");
    if (sqlite3_step(exists.get()) != SQLITE_ROW) return false;
    auto row = prepare(db, "SELECT 1 FROM model_supplement WHERE simulation_id=?");
    bind_text(row.get(), 1, simulation);
    return sqlite3_step(row.get()) == SQLITE_ROW;
}

inline void read_model_supplement(sqlite3* db, SimulationContext& ctx,
                                  const std::string& simulation) {
    if (!has_model_supplement(db, simulation)) return;
    auto row = prepare(db, "SELECT version,sections FROM model_supplement WHERE simulation_id=?");
    bind_text(row.get(), 1, simulation);
    if (sqlite3_step(row.get()) != SQLITE_ROW) return;
    if (column_int(row.get(), 0) != 1) throw GpkgError("Unsupported model supplement version");
    DefaultInputPlugin parser;
    parser.registerContextSections(ctx);
    std::istringstream input(column_text(row.get(), 1));
    std::string line, section;
    std::vector<std::string> body;
    auto flush = [&] {
        if (section.empty()) return;
        if (section.starts_with("REACTION_")) ctx.embedded_component_sections.emplace_back(section, body);
        else if (parser.registry().has(section)) {
            const double reportStart = ctx.options.report_start;
            parser.registry().dispatch(section, ctx, body);
            // The OPTIONS handler defaults an absent report-start line to START_DATE;
            // relational dates are authoritative for a partial section replay.
            if (section == "OPTIONS") ctx.options.report_start = reportStart;
        }
        else if (section == "2D_OPTIONS" && !ctx.twod_io.options)
            ctx.warnings.push_back("[2D_OPTIONS] ignored: no 2D options storage is attached");
        else throw GpkgError("Unsupported saved model section: " + section);
        body.clear();
    };
    while (std::getline(input, line)) {
        if (!line.empty() && line.front() == '[') {
            flush(); section = line.substr(1, line.find(']') - 1);
        } else body.push_back(line);
    }
    flush();
    auto props = prepare(db, "SELECT scope,object_id,property,value FROM model_properties WHERE simulation_id=?");
    bind_text(props.get(), 1, simulation);
    auto comment = [](auto& values, int index, const std::string& value) {
        if (index >= 0) { if (values.size() <= std::size_t(index)) values.resize(index + 1); values[index] = value; }
    };
    std::map<std::string, std::string> configs;
    std::map<int, std::string> title;
    bool hasTitle = false;
    while (sqlite3_step(props.get()) == SQLITE_ROW) {
        const auto scope = column_text(props.get(), 0), id = column_text(props.get(), 1),
                   key = column_text(props.get(), 2), value = column_text(props.get(), 3);
        if (scope == "model" && key == "title_count") hasTitle = true;
        else if (scope == "title") title[std::stoi(id)] = value;
        else if (scope == "component") { configs[id] = value; }
        else if (scope == "node") comment(ctx.nodes.comments, ctx.node_names.find(id), value);
        else if (scope == "link") comment(ctx.links.comments, ctx.link_names.find(id), value);
        else if (scope == "table") { int i = ctx.find_table_any(id); if (i >= 0) ctx.tables[i].comment = value; }
        else if (scope == "gage") {
            int i = ctx.gage_names.find(id); if (i < 0) continue;
            if (key == "comment") comment(ctx.gages.comments, i, value);
            else if (key == "scale") ctx.gages.scale_factor[i] = std::stod(value);
        } else if (scope == "subcatch") {
            int i = ctx.subcatch_names.find(id); if (i < 0) continue;
            if (key == "comment") comment(ctx.subcatches.comments, i, value);
            else if (key == "snowpack") {
                comment(ctx.subcatches.snowpack_name, i, value);
                ctx.subcatches.snowpack[i] = ctx.snowpack_names.find(value);
            }
        } else if (scope == "pollutant") {
            int i = ctx.pollutant_names.find(id); if (i < 0) continue;
            if (key == "comment") comment(ctx.pollutants.comments, i, value);
            else if (key == "mwt") ctx.pollutants.mwt[i] = std::stod(value);
        } else if (scope == "weir") {
            int i = ctx.link_names.find(id); int r = i < 0 ? -1 : ctx.link_subtypes.weir_row(i);
            if (r < 0) continue;
            auto& w = ctx.link_subtypes.weirs;
            if (key == "cd2") w.cd2[r] = std::stod(value);
            else if (key == "end_contractions") w.end_contractions[r] = std::stod(value);
            else if (key == "can_surcharge") w.can_surcharge[r] = std::stoi(value);
            else if (key == "road_width") w.road_width[r] = std::stod(value);
            else if (key == "road_surface") w.road_surface[r] = std::stoi(value);
            else if (key == "cd_curve") w.cd_curve[r] = ctx.find_table_any(value);
        }
    }
    if (hasTitle) {
        ctx.title_notes.clear();
        for (const auto& [index, line] : title) ctx.title_notes.push_back(line);
    }
    native_model::restoreConfigurations(ctx, configs);
}
} // namespace openswmm::gpkg
