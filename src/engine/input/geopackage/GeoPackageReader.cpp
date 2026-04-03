/**
 * @file GeoPackageReader.cpp
 * @brief Reads a GeoPackage file into a SimulationContext (all SWMM input sections).
 * @ingroup engine_geopackage
 */

#include "GeoPackageReader.hpp"
#include "GpkgUtils.hpp"
#include "GpkgGeometry.hpp"

#include "core/SimulationContext.hpp"
#include "data/NodeData.hpp"
#include "data/LinkData.hpp"
#include "data/SubcatchData.hpp"
#include "data/GageData.hpp"
#include "data/TableData.hpp"
#include "data/PollutantData.hpp"
#include "data/InflowData.hpp"

#include <algorithm>

namespace openswmm::gpkg {

// ============================================================================
// Enum parsing helpers
// ============================================================================

static NodeType parse_node_type(const std::string& s) {
    if (s == "OUTFALL")  return NodeType::OUTFALL;
    if (s == "DIVIDER")  return NodeType::DIVIDER;
    if (s == "STORAGE")  return NodeType::STORAGE;
    return NodeType::JUNCTION;
}

static LinkType parse_link_type(const std::string& s) {
    if (s == "PUMP")    return LinkType::PUMP;
    if (s == "ORIFICE") return LinkType::ORIFICE;
    if (s == "WEIR")    return LinkType::WEIR;
    if (s == "OUTLET")  return LinkType::OUTLET;
    return LinkType::CONDUIT;
}

static OutfallType parse_outfall_type(const std::string& s) {
    if (s == "NORMAL")     return OutfallType::NORMAL;
    if (s == "FIXED")      return OutfallType::FIXED;
    if (s == "TIDAL")      return OutfallType::TIDAL;
    if (s == "TIMESERIES") return OutfallType::TIMESERIES;
    return OutfallType::FREE;
}

static DividerType parse_divider_type(const std::string& s) {
    if (s == "OVERFLOW") return DividerType::OVERFLOW_DIV;
    if (s == "TABULAR")  return DividerType::TABULAR;
    if (s == "WEIR")     return DividerType::WEIR;
    return DividerType::CUTOFF;
}

static XsectShape parse_xsect_shape(const std::string& s) {
    if (s == "CIRCULAR")        return XsectShape::CIRCULAR;
    if (s == "FILLED_CIRCULAR") return XsectShape::FILLED_CIRCULAR;
    if (s == "RECT_CLOSED")     return XsectShape::RECT_CLOSED;
    if (s == "RECT_OPEN")       return XsectShape::RECT_OPEN;
    if (s == "TRAPEZOIDAL")     return XsectShape::TRAPEZOIDAL;
    if (s == "TRIANGULAR")      return XsectShape::TRIANGULAR;
    if (s == "PARABOLIC")       return XsectShape::PARABOLIC;
    if (s == "POWER")           return XsectShape::POWER;
    if (s == "IRREGULAR")       return XsectShape::IRREGULAR;
    if (s == "CUSTOM")          return XsectShape::CUSTOM;
    if (s == "FORCE_MAIN")      return XsectShape::FORCE_MAIN;
    if (s == "STREET")          return XsectShape::STREET_XSECT;
    if (s == "EGGSHAPED")       return XsectShape::EGGSHAPED;
    if (s == "HORSESHOE")       return XsectShape::HORSESHOE;
    if (s == "GOTHIC")          return XsectShape::GOTHIC;
    if (s == "CATENARY")        return XsectShape::CATENARY;
    if (s == "SEMIELLIPTICAL")  return XsectShape::SEMIELLIPTICAL;
    if (s == "BASKETHANDLE")    return XsectShape::BASKETHANDLE;
    if (s == "SEMICIRCULAR")    return XsectShape::SEMICIRCULAR;
    if (s == "MODBASKETHANDLE") return XsectShape::MODBASKETHANDLE;
    if (s == "RECT_TRIANG")     return XsectShape::RECT_TRIANG;
    if (s == "RECT_ROUND")      return XsectShape::RECT_ROUND;
    return XsectShape::CIRCULAR;
}

// ============================================================================
// Ensure capacity helpers (same pattern as handler code)
// ============================================================================

static void ensure_node_capacity(SimulationContext& ctx, int idx) {
    size_t n = static_cast<size_t>(idx + 1);
    auto grow = [n](auto& vec) { if (vec.size() < n) vec.resize(n); };
    grow(ctx.nodes.type);
    grow(ctx.nodes.invert_elev);
    grow(ctx.nodes.full_depth);
    grow(ctx.nodes.init_depth);
    grow(ctx.nodes.sur_depth);
    grow(ctx.nodes.ponded_area);
    grow(ctx.nodes.outfall_type);
    grow(ctx.nodes.outfall_param);
    grow(ctx.nodes.outfall_has_flap_gate);
    grow(ctx.nodes.storage_curve);
    grow(ctx.nodes.storage_curve_name);
    grow(ctx.nodes.storage_a);
    grow(ctx.nodes.storage_b);
    grow(ctx.nodes.storage_c);
    grow(ctx.nodes.divider_type);
    grow(ctx.nodes.divider_cutoff);
    grow(ctx.nodes.divider_curve);
    grow(ctx.nodes.divider_curve_name);
    grow(ctx.spatial.node_x);
    grow(ctx.spatial.node_y);
}

static void ensure_link_capacity(SimulationContext& ctx, int idx) {
    size_t n = static_cast<size_t>(idx + 1);
    auto grow = [n](auto& vec) { if (vec.size() < n) vec.resize(n); };
    grow(ctx.links.type);
    grow(ctx.links.node1);
    grow(ctx.links.node2);
    grow(ctx.links.offset1);
    grow(ctx.links.offset2);
    grow(ctx.links.q0);
    grow(ctx.links.q_limit);
    grow(ctx.links.xsect_shape);
    grow(ctx.links.xsect_y_full);
    grow(ctx.links.xsect_a_full);
    grow(ctx.links.xsect_w_max);
    grow(ctx.links.xsect_curve);
    grow(ctx.links.roughness);
    grow(ctx.links.length);
    grow(ctx.links.barrels);
    grow(ctx.links.culvert_code);
    grow(ctx.links.loss_inlet);
    grow(ctx.links.loss_outlet);
    grow(ctx.links.loss_avg);
    grow(ctx.links.has_flap_gate);
    grow(ctx.links.seep_rate);
    grow(ctx.links.pump_curve);
    grow(ctx.links.pump_curve_name);
    grow(ctx.links.pump_init_state);
    grow(ctx.links.pump_startup);
    grow(ctx.links.pump_shutoff);
    grow(ctx.links.crest_height);
    grow(ctx.links.cd);
    grow(ctx.links.xsect_y_bot);
    grow(ctx.spatial.link_vertices_x);
    grow(ctx.spatial.link_vertices_y);
    grow(ctx.spatial.link_x);
    grow(ctx.spatial.link_y);
}

static void ensure_subcatch_capacity(SimulationContext& ctx, int idx) {
    size_t n = static_cast<size_t>(idx + 1);
    auto grow = [n](auto& vec) { if (vec.size() < n) vec.resize(n); };
    grow(ctx.subcatches.outlet_node);
    grow(ctx.subcatches.outlet_subcatch);
    grow(ctx.subcatches.outlet_name);
    grow(ctx.subcatches.gage);
    grow(ctx.subcatches.area);
    grow(ctx.subcatches.width);
    grow(ctx.subcatches.slope);
    grow(ctx.subcatches.curb_length);
    grow(ctx.subcatches.frac_imperv);
    grow(ctx.subcatches.n_imperv);
    grow(ctx.subcatches.n_perv);
    grow(ctx.subcatches.ds_imperv);
    grow(ctx.subcatches.ds_perv);
    grow(ctx.subcatches.frac_imperv_no_store);
    grow(ctx.subcatches.subarea_routing);
    grow(ctx.subcatches.pct_routed);
    grow(ctx.subcatches.infil_model);
    grow(ctx.subcatches.infil_p1);
    grow(ctx.subcatches.infil_p2);
    grow(ctx.subcatches.infil_p3);
    grow(ctx.subcatches.infil_p4);
    grow(ctx.subcatches.infil_p5);
    grow(ctx.spatial.subcatch_polygon_x);
    grow(ctx.spatial.subcatch_polygon_y);
}

// ============================================================================
// Read sections
// ============================================================================

static void read_options(sqlite3* db, SimulationContext& ctx, const std::string& sim_id) {
    auto stmt = prepare(db,
        "SELECT key, value FROM options WHERE simulation_id = ?");
    bind_text(stmt.get(), 1, sim_id);

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        std::string key = column_text(stmt.get(), 0);
        std::string val = column_text(stmt.get(), 1);

        if (key == "FLOW_UNITS") ctx.options.flow_units = static_cast<FlowUnits>(std::stoi(val));
        else if (key == "INFILTRATION") ctx.options.infiltration = static_cast<InfiltrationModel>(std::stoi(val));
        else if (key == "ROUTING_MODEL") ctx.options.routing_model = static_cast<RoutingModel>(std::stoi(val));
        else if (key == "LINK_OFFSETS") ctx.options.link_offsets = std::stoi(val);
        else if (key == "FORCE_MAIN_EQN") ctx.options.force_main_eqn = std::stoi(val);
        else if (key == "ALLOW_PONDING") ctx.options.allow_ponding = (val == "YES");
        else if (key == "IGNORE_RAINFALL") ctx.options.ignore_rainfall = (val == "YES");
        else if (key == "IGNORE_SNOWMELT") ctx.options.ignore_snow_melt = (val == "YES");
        else if (key == "IGNORE_GW") ctx.options.ignore_groundwater = (val == "YES");
        else if (key == "IGNORE_ROUTING") ctx.options.ignore_routing = (val == "YES");
        else if (key == "IGNORE_QUALITY") ctx.options.ignore_quality = (val == "YES");
        else if (key == "WET_STEP") ctx.options.wet_step = std::stod(val);
        else if (key == "DRY_STEP") ctx.options.dry_step = std::stod(val);
        else if (key == "ROUTING_STEP") ctx.options.routing_step = std::stod(val);
        else if (key == "REPORT_STEP") ctx.options.report_step = std::stod(val);
        else if (key == "START_DATE") ctx.options.start_date = std::stod(val);
        else if (key == "END_DATE") ctx.options.end_date = std::stod(val);
        else if (key == "REPORT_START") ctx.options.report_start = std::stod(val);
        else if (key == "SWEEP_START") ctx.options.sweep_start = std::stoi(val);
        else if (key == "SWEEP_END") ctx.options.sweep_end = std::stoi(val);
        else if (key == "CRS") ctx.spatial.crs = val;
    }
}

static void read_nodes(sqlite3* db, SimulationContext& ctx, const std::string& sim_id) {
    auto stmt = prepare(db,
        "SELECT node_id, node_type, geom, invert_elev, max_depth, init_depth, "
        "surcharge_depth, ponded_area, outfall_type, outfall_stage, outfall_has_flap_gate, "
        "divider_type, divider_cutoff, divider_curve, "
        "storage_curve, storage_a, storage_b, storage_c, tag "
        "FROM nodes WHERE simulation_id = ?");
    bind_text(stmt.get(), 1, sim_id);

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        std::string name = column_text(stmt.get(), 0);
        std::string type_str = column_text(stmt.get(), 1);

        int idx = ctx.node_names.find(name);
        if (idx < 0) idx = ctx.node_names.add(name);
        ensure_node_capacity(ctx, idx);

        NodeType ntype = parse_node_type(type_str);
        ctx.nodes.type[idx] = ntype;

        // Geometry
        if (!column_is_null(stmt.get(), 2)) {
            auto blob = column_blob(stmt.get(), 2);
            auto pt = decode_point(blob);
            ctx.spatial.node_x[idx] = pt.x;
            ctx.spatial.node_y[idx] = pt.y;
        }

        ctx.nodes.invert_elev[idx] = column_double(stmt.get(), 3);
        ctx.nodes.full_depth[idx] = column_double(stmt.get(), 4);
        ctx.nodes.init_depth[idx] = column_double(stmt.get(), 5);
        ctx.nodes.sur_depth[idx] = column_double(stmt.get(), 6);
        ctx.nodes.ponded_area[idx] = column_double(stmt.get(), 7);

        if (ntype == NodeType::OUTFALL && !column_is_null(stmt.get(), 8)) {
            ctx.nodes.outfall_type[idx] = parse_outfall_type(column_text(stmt.get(), 8));
            ctx.nodes.outfall_param[idx] = column_double(stmt.get(), 9);
            ctx.nodes.outfall_has_flap_gate[idx] = column_int(stmt.get(), 10) != 0;
        }

        if (ntype == NodeType::DIVIDER && !column_is_null(stmt.get(), 11)) {
            ctx.nodes.divider_type[idx] = parse_divider_type(column_text(stmt.get(), 11));
            ctx.nodes.divider_cutoff[idx] = column_double(stmt.get(), 12);
            if (!column_is_null(stmt.get(), 13))
                ctx.nodes.divider_curve_name[idx] = column_text(stmt.get(), 13);
        }

        if (ntype == NodeType::STORAGE) {
            if (!column_is_null(stmt.get(), 14))
                ctx.nodes.storage_curve_name[idx] = column_text(stmt.get(), 14);
            ctx.nodes.storage_a[idx] = column_double(stmt.get(), 15);
            ctx.nodes.storage_b[idx] = column_double(stmt.get(), 16);
            ctx.nodes.storage_c[idx] = column_double(stmt.get(), 17);
        }

        if (!column_is_null(stmt.get(), 18))
            ctx.node_tags[name] = column_text(stmt.get(), 18);
    }
}

static void read_links(sqlite3* db, SimulationContext& ctx, const std::string& sim_id) {
    auto stmt = prepare(db,
        "SELECT link_id, link_type, geom, from_node, to_node, offset1, offset2, "
        "xsect_shape, xsect_geom1, xsect_geom2, xsect_geom3, xsect_geom4, "
        "xsect_barrels, xsect_culvert, xsect_curve, "
        "roughness, length, loss_inlet, loss_outlet, loss_avg, "
        "has_flap_gate, seep_rate, q0, q_limit, "
        "pump_curve, pump_init_state, pump_startup, pump_shutoff, "
        "crest_height, discharge_coeff, tag "
        "FROM links WHERE simulation_id = ?");
    bind_text(stmt.get(), 1, sim_id);

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        std::string name = column_text(stmt.get(), 0);
        std::string type_str = column_text(stmt.get(), 1);

        int idx = ctx.link_names.find(name);
        if (idx < 0) idx = ctx.link_names.add(name);
        ensure_link_capacity(ctx, idx);

        LinkType ltype = parse_link_type(type_str);
        ctx.links.type[idx] = ltype;

        // Geometry: extract vertices from linestring
        if (!column_is_null(stmt.get(), 2)) {
            auto blob = column_blob(stmt.get(), 2);
            auto ls = decode_linestring(blob);
            // First and last points are node coordinates; interior points are vertices
            if (ls.xs.size() > 2) {
                ctx.spatial.link_vertices_x[idx].assign(ls.xs.begin() + 1, ls.xs.end() - 1);
                ctx.spatial.link_vertices_y[idx].assign(ls.ys.begin() + 1, ls.ys.end() - 1);
            }
        }

        // Node connectivity
        std::string from_name = column_text(stmt.get(), 3);
        std::string to_name = column_text(stmt.get(), 4);
        int n1 = ctx.node_names.find(from_name);
        int n2 = ctx.node_names.find(to_name);
        ctx.links.node1[idx] = n1;
        ctx.links.node2[idx] = n2;

        ctx.links.offset1[idx] = column_double(stmt.get(), 5);
        ctx.links.offset2[idx] = column_double(stmt.get(), 6);

        if (!column_is_null(stmt.get(), 7))
            ctx.links.xsect_shape[idx] = parse_xsect_shape(column_text(stmt.get(), 7));
        ctx.links.xsect_y_full[idx] = column_double(stmt.get(), 8);
        ctx.links.xsect_w_max[idx] = column_double(stmt.get(), 9);
        ctx.links.xsect_a_full[idx] = column_double(stmt.get(), 10);
        ctx.links.xsect_y_bot[idx] = column_double(stmt.get(), 11);
        ctx.links.barrels[idx] = column_int(stmt.get(), 12);
        ctx.links.culvert_code[idx] = column_int(stmt.get(), 13);

        if (!column_is_null(stmt.get(), 14)) {
            std::string curve_name = column_text(stmt.get(), 14);
            int ci = ctx.table_names.find(curve_name);
            ctx.links.xsect_curve[idx] = ci;
        }

        ctx.links.roughness[idx] = column_double(stmt.get(), 15);
        ctx.links.length[idx] = column_double(stmt.get(), 16);
        ctx.links.loss_inlet[idx] = column_double(stmt.get(), 17);
        ctx.links.loss_outlet[idx] = column_double(stmt.get(), 18);
        ctx.links.loss_avg[idx] = column_double(stmt.get(), 19);
        ctx.links.has_flap_gate[idx] = column_int(stmt.get(), 20) != 0;
        ctx.links.seep_rate[idx] = column_double(stmt.get(), 21);
        ctx.links.q0[idx] = column_double(stmt.get(), 22);
        ctx.links.q_limit[idx] = column_double(stmt.get(), 23);

        if (ltype == LinkType::PUMP) {
            if (!column_is_null(stmt.get(), 24))
                ctx.links.pump_curve_name[idx] = column_text(stmt.get(), 24);
            ctx.links.pump_init_state[idx] = column_double(stmt.get(), 25) > 0.5;
            ctx.links.pump_startup[idx] = column_double(stmt.get(), 26);
            ctx.links.pump_shutoff[idx] = column_double(stmt.get(), 27);
        }

        ctx.links.crest_height[idx] = column_double(stmt.get(), 28);
        ctx.links.cd[idx] = column_double(stmt.get(), 29);

        if (!column_is_null(stmt.get(), 30))
            ctx.link_tags[name] = column_text(stmt.get(), 30);
    }
}

static void read_subcatchments(sqlite3* db, SimulationContext& ctx, const std::string& sim_id) {
    auto stmt = prepare(db,
        "SELECT subcatch_id, geom, outlet_node, outlet_subcatch, rain_gage, "
        "area, width, slope, curb_length, frac_imperv, "
        "n_imperv, n_perv, ds_imperv, ds_perv, pct_zero_imperv, "
        "subarea_routing, pct_routed, "
        "infil_model, infil_p1, infil_p2, infil_p3, infil_p4, infil_p5, tag "
        "FROM subcatchments WHERE simulation_id = ?");
    bind_text(stmt.get(), 1, sim_id);

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        std::string name = column_text(stmt.get(), 0);

        int idx = ctx.subcatch_names.find(name);
        if (idx < 0) idx = ctx.subcatch_names.add(name);
        ensure_subcatch_capacity(ctx, idx);

        // Geometry
        if (!column_is_null(stmt.get(), 1)) {
            auto blob = column_blob(stmt.get(), 1);
            auto mp = decode_multipolygon(blob);
            ctx.spatial.subcatch_polygon_x[idx] = std::move(mp.xs);
            ctx.spatial.subcatch_polygon_y[idx] = std::move(mp.ys);
        }

        // Outlet
        if (!column_is_null(stmt.get(), 2)) {
            std::string outlet = column_text(stmt.get(), 2);
            ctx.subcatches.outlet_name[idx] = outlet;
            int ni = ctx.node_names.find(outlet);
            ctx.subcatches.outlet_node[idx] = ni;
            ctx.subcatches.outlet_subcatch[idx] = -1;
        } else if (!column_is_null(stmt.get(), 3)) {
            std::string outlet = column_text(stmt.get(), 3);
            ctx.subcatches.outlet_name[idx] = outlet;
            ctx.subcatches.outlet_node[idx] = -1;
            int si = ctx.subcatch_names.find(outlet);
            ctx.subcatches.outlet_subcatch[idx] = si;
        }

        // Rain gage
        if (!column_is_null(stmt.get(), 4)) {
            std::string gage = column_text(stmt.get(), 4);
            int gi = ctx.gage_names.find(gage);
            ctx.subcatches.gage[idx] = gi;
        }

        ctx.subcatches.area[idx] = column_double(stmt.get(), 5);
        ctx.subcatches.width[idx] = column_double(stmt.get(), 6);
        ctx.subcatches.slope[idx] = column_double(stmt.get(), 7);
        ctx.subcatches.curb_length[idx] = column_double(stmt.get(), 8);
        ctx.subcatches.frac_imperv[idx] = column_double(stmt.get(), 9);
        ctx.subcatches.n_imperv[idx] = column_double(stmt.get(), 10);
        ctx.subcatches.n_perv[idx] = column_double(stmt.get(), 11);
        ctx.subcatches.ds_imperv[idx] = column_double(stmt.get(), 12);
        ctx.subcatches.ds_perv[idx] = column_double(stmt.get(), 13);
        ctx.subcatches.frac_imperv_no_store[idx] = column_double(stmt.get(), 14);
        ctx.subcatches.subarea_routing[idx] = column_int(stmt.get(), 15);
        ctx.subcatches.pct_routed[idx] = column_double(stmt.get(), 16);
        ctx.subcatches.infil_model[idx] = column_int(stmt.get(), 17);
        ctx.subcatches.infil_p1[idx] = column_double(stmt.get(), 18);
        ctx.subcatches.infil_p2[idx] = column_double(stmt.get(), 19);
        ctx.subcatches.infil_p3[idx] = column_double(stmt.get(), 20);
        ctx.subcatches.infil_p4[idx] = column_double(stmt.get(), 21);
        ctx.subcatches.infil_p5[idx] = column_double(stmt.get(), 22);

        if (!column_is_null(stmt.get(), 23))
            ctx.subcatch_tags[name] = column_text(stmt.get(), 23);
    }
}

static void read_rain_gages(sqlite3* db, SimulationContext& ctx, const std::string& sim_id) {
    auto stmt = prepare(db,
        "SELECT gage_id, geom, rain_type, rain_interval, snow_catch, data_source, source_name "
        "FROM rain_gages WHERE simulation_id = ?");
    bind_text(stmt.get(), 1, sim_id);

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        std::string name = column_text(stmt.get(), 0);
        int idx = ctx.gage_names.find(name);
        if (idx < 0) idx = ctx.gage_names.add(name);

        size_t n = static_cast<size_t>(idx + 1);
        auto grow = [n](auto& vec) { if (vec.size() < n) vec.resize(n); };
        grow(ctx.gages.rain_type);
        grow(ctx.gages.interval_sec);
        grow(ctx.gages.snow_factor);
        grow(ctx.gages.source);
        grow(ctx.gages.ts_index);
        grow(ctx.spatial.gage_x);
        grow(ctx.spatial.gage_y);

        if (!column_is_null(stmt.get(), 1)) {
            auto blob = column_blob(stmt.get(), 1);
            auto pt = decode_point(blob);
            ctx.spatial.gage_x[idx] = pt.x;
            ctx.spatial.gage_y[idx] = pt.y;
        }

        ctx.gages.rain_type[idx] = column_int(stmt.get(), 2);
        ctx.gages.interval_sec[idx] = column_int(stmt.get(), 3);
        ctx.gages.snow_factor[idx] = column_double(stmt.get(), 4);
        ctx.gages.source[idx] = static_cast<RainSource>(column_int(stmt.get(), 5));

        if (!column_is_null(stmt.get(), 6)) {
            std::string src = column_text(stmt.get(), 6);
            int ti = ctx.table_names.find(src);
            ctx.gages.ts_index[idx] = ti;
        }
    }
}

static void read_curves(sqlite3* db, SimulationContext& ctx, const std::string& sim_id) {
    auto stmt = prepare(db,
        "SELECT curve_id, curve_type, x_value, y_value "
        "FROM curves WHERE simulation_id = ? ORDER BY curve_id, ordinal");
    bind_text(stmt.get(), 1, sim_id);

    std::string prev_name;
    int idx = -1;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        std::string name = column_text(stmt.get(), 0);
        if (name != prev_name) {
            idx = ctx.table_names.find(name);
            if (idx < 0) {
                // Determine the curve type from the stored integer
                int ctype_int = std::stoi(column_text(stmt.get(), 1));
                TableType ttype = static_cast<TableType>(ctype_int);
                idx = ctx.table_names.add(name);
                ctx.tables.add(name, ttype);
            }
            prev_name = name;
        }
        double x = column_double(stmt.get(), 2);
        double y = column_double(stmt.get(), 3);
        ctx.tables[idx].x.push_back(x);
        ctx.tables[idx].y.push_back(y);
    }
}

static void read_timeseries(sqlite3* db, SimulationContext& ctx, const std::string& sim_id) {
    auto stmt = prepare(db,
        "SELECT series_id, timestamp, value "
        "FROM input_timeseries WHERE simulation_id = ? ORDER BY series_id, ordinal");
    bind_text(stmt.get(), 1, sim_id);

    std::string prev_name;
    int idx = -1;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        std::string name = column_text(stmt.get(), 0);
        if (name != prev_name) {
            idx = ctx.table_names.find(name);
            if (idx < 0) {
                idx = ctx.table_names.add(name);
                ctx.tables.add(name, TableType::TIMESERIES);
            }
            prev_name = name;
        }
        double ts = std::stod(column_text(stmt.get(), 1));
        double val = column_double(stmt.get(), 2);
        ctx.tables[idx].x.push_back(ts);
        ctx.tables[idx].y.push_back(val);
    }
}

static void read_pollutants(sqlite3* db, SimulationContext& ctx, const std::string& sim_id) {
    auto stmt = prepare(db,
        "SELECT pollutant_id, units, rain_conc, gw_conc, decay_coeff, "
        "snow_only, co_pollutant, co_fraction "
        "FROM pollutants WHERE simulation_id = ?");
    bind_text(stmt.get(), 1, sim_id);

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        std::string name = column_text(stmt.get(), 0);
        int idx = ctx.pollutant_names.find(name);
        if (idx < 0) idx = ctx.pollutant_names.add(name);

        size_t n = static_cast<size_t>(idx + 1);
        auto grow = [n](auto& vec) { if (vec.size() < n) vec.resize(n); };
        grow(ctx.pollutants.units);
        grow(ctx.pollutants.c_rain);
        grow(ctx.pollutants.c_gw);
        grow(ctx.pollutants.k_decay);
        grow(ctx.pollutants.snow_only);
        grow(ctx.pollutants.co_pollut);
        grow(ctx.pollutants.co_frac);

        ctx.pollutants.units[idx] = static_cast<MassUnits>(column_int(stmt.get(), 1));
        ctx.pollutants.c_rain[idx] = column_double(stmt.get(), 2);
        ctx.pollutants.c_gw[idx] = column_double(stmt.get(), 3);
        ctx.pollutants.k_decay[idx] = column_double(stmt.get(), 4);
        ctx.pollutants.snow_only[idx] = column_int(stmt.get(), 5) != 0;

        if (!column_is_null(stmt.get(), 6)) {
            std::string co = column_text(stmt.get(), 6);
            ctx.pollutants.co_pollut[idx] = ctx.pollutant_names.find(co);
        }
        ctx.pollutants.co_frac[idx] = column_double(stmt.get(), 7);
    }
}

static void read_patterns(sqlite3* db, SimulationContext& ctx, const std::string& sim_id) {
    auto stmt = prepare(db,
        "SELECT pattern_id, pattern_type, factor "
        "FROM patterns WHERE simulation_id = ? ORDER BY pattern_id, ordinal");
    bind_text(stmt.get(), 1, sim_id);

    std::string prev_name;
    int idx = -1;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        std::string name = column_text(stmt.get(), 0);
        if (name != prev_name) {
            // Search for existing pattern by name
            idx = -1;
            for (int k = 0; k < static_cast<int>(ctx.patterns.names.size()); ++k) {
                if (ctx.patterns.names[k] == name) { idx = k; break; }
            }
            if (idx < 0) {
                idx = static_cast<int>(ctx.patterns.names.size());
                ctx.patterns.names.push_back(name);
                ctx.patterns.types.push_back(0);
                ctx.patterns.factors.push_back({});
            }
            ctx.patterns.types[idx] = column_int(stmt.get(), 1);
            prev_name = name;
        }
        ctx.patterns.factors[idx].push_back(column_double(stmt.get(), 2));
    }
}

// ============================================================================
// Public API
// ============================================================================

int read_model(sqlite3* db, SimulationContext& ctx, const std::string& simulation_id) {
    try {
        read_options(db, ctx, simulation_id);
        read_nodes(db, ctx, simulation_id);
        read_links(db, ctx, simulation_id);
        read_rain_gages(db, ctx, simulation_id);
        read_subcatchments(db, ctx, simulation_id);
        read_curves(db, ctx, simulation_id);
        read_timeseries(db, ctx, simulation_id);
        read_pollutants(db, ctx, simulation_id);
        read_patterns(db, ctx, simulation_id);
        return 0;
    } catch (const std::exception&) {
        return -1;
    }
}

int read_from_file(const std::string& path, SimulationContext& ctx,
                   const std::string& simulation_id) {
    try {
        auto db = open_database(path, SQLITE_OPEN_READONLY);
        return read_model(db.get(), ctx, simulation_id);
    } catch (const std::exception&) {
        return -1;
    }
}

} // namespace openswmm::gpkg
