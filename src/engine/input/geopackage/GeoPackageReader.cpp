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
#include "data/HydrologyData.hpp"

#include <algorithm>
#include <array>
#include <sstream>

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
    if (s == "HORIZ_ELLIPSE")   return XsectShape::HORIZ_ELLIPSE;
    if (s == "VERT_ELLIPSE")    return XsectShape::VERT_ELLIPSE;
    if (s == "ARCH")            return XsectShape::ARCH;
    return XsectShape::CIRCULAR;
}

// ============================================================================
// Ensure capacity helpers (same pattern as handler code)
// ============================================================================

static void ensure_node_capacity(SimulationContext& ctx, int idx) {
    ctx.nodes.grow_to(idx + 1);
    auto n = static_cast<size_t>(idx + 1);
    if (ctx.spatial.node_x.size() < n) ctx.spatial.node_x.resize(n, 0.0);
    if (ctx.spatial.node_y.size() < n) ctx.spatial.node_y.resize(n, 0.0);
}

static void ensure_link_capacity(SimulationContext& ctx, int idx) {
    ctx.links.grow_to(idx + 1);
    auto n = static_cast<size_t>(idx + 1);
    if (ctx.spatial.link_vertices_x.size() < n) ctx.spatial.link_vertices_x.resize(n);
    if (ctx.spatial.link_vertices_y.size() < n) ctx.spatial.link_vertices_y.resize(n);
    if (ctx.spatial.link_x.size() < n) ctx.spatial.link_x.resize(n, 0.0);
    if (ctx.spatial.link_y.size() < n) ctx.spatial.link_y.resize(n, 0.0);
}

static void ensure_subcatch_capacity(SimulationContext& ctx, int idx) {
    ctx.subcatches.grow_to(idx + 1);
    auto n = static_cast<size_t>(idx + 1);
    if (ctx.spatial.subcatch_polygon_x.size() < n) ctx.spatial.subcatch_polygon_x.resize(n);
    if (ctx.spatial.subcatch_polygon_y.size() < n) ctx.spatial.subcatch_polygon_y.resize(n);
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
        else if (key == "NODE_CONTINUITY") ctx.options.node_continuity = static_cast<NodeContinuity>(std::stoi(val));
        else if (key == "ANDERSON_ACCEL") ctx.options.anderson_accel = (std::stoi(val) != 0);
        else if (key == "SURCHARGE_METHOD") ctx.options.surcharge_method = std::stoi(val);
        else if (key == "DPS_CELERITY") ctx.options.dps_target_celerity = std::stod(val);
        else if (key == "DPS_ALPHA") ctx.options.dps_alpha = std::stod(val);
        else if (key == "DPS_DECAY_TIME") ctx.options.dps_decay_time = std::stod(val);
        else if (key == "INERTIAL_DAMPING") ctx.options.inertial_damping = std::stoi(val);
        else if (key == "NORMAL_FLOW_LIMITED") ctx.options.normal_flow_ltd = std::stoi(val);
        else if (key == "MAX_TRIALS") ctx.options.max_trials = std::stoi(val);
        else if (key == "HEAD_TOLERANCE") ctx.options.head_tol = std::stod(val);
        else if (key == "VARIABLE_STEP") ctx.options.variable_step = std::stod(val);
        else if (key == "MINIMUM_STEP") ctx.options.min_routing_step = std::stod(val);
        else if (key == "LENGTHENING_STEP") ctx.options.lengthening_step = std::stod(val);
        else if (key == "MIN_SLOPE") ctx.options.min_slope = std::stod(val);
        else if (key == "MIN_SURFAREA") ctx.options.min_surf_area = std::stod(val);
        else if (key == "SYS_FLOW_TOL") ctx.options.sys_flow_tol = std::stod(val);
        else if (key == "LAT_FLOW_TOL") ctx.options.lat_flow_tol = std::stod(val);
        else if (key == "THREADS") ctx.options.num_threads = std::stoi(val);
        else if (key == "DRY_DAYS") ctx.options.dry_days = std::stod(val);
        else if (key == "IGNORE_RDII") ctx.options.ignore_rdii = (val == "YES");
        else if (key == "RPT_DISABLED") ctx.options.rpt_disabled = (val == "YES");
        else if (key == "RPT_INPUT") ctx.options.rpt_input = (val == "YES");
        else if (key == "RPT_CONTINUITY") ctx.options.rpt_continuity = (val == "YES");
        else if (key == "RPT_FLOWSTATS") ctx.options.rpt_flowstats = (val == "YES");
        else if (key == "RPT_CONTROLS") ctx.options.rpt_controls = (val == "YES");
        else if (key == "RPT_AVERAGES") ctx.options.rpt_averages = (val == "YES");
        else if (key == "RPT_SUBCATCHMENTS") {
            if (val == "NONE") ctx.options.rpt_subcatchments = 0;
            else if (val == "ALL") ctx.options.rpt_subcatchments = 1;
            else {
                ctx.options.rpt_subcatchments = 2;
                std::string token; std::istringstream ss(val);
                while (std::getline(ss, token, ','))
                    if (!token.empty()) ctx.options.rpt_subcatch_names.push_back(token);
            }
        }
        else if (key == "RPT_NODES") {
            if (val == "NONE") ctx.options.rpt_nodes = 0;
            else if (val == "ALL") ctx.options.rpt_nodes = 1;
            else {
                ctx.options.rpt_nodes = 2;
                std::string token; std::istringstream ss(val);
                while (std::getline(ss, token, ','))
                    if (!token.empty()) ctx.options.rpt_node_names.push_back(token);
            }
        }
        else if (key == "RPT_LINKS") {
            if (val == "NONE") ctx.options.rpt_links = 0;
            else if (val == "ALL") ctx.options.rpt_links = 1;
            else {
                ctx.options.rpt_links = 2;
                std::string token; std::istringstream ss(val);
                while (std::getline(ss, token, ','))
                    if (!token.empty()) ctx.options.rpt_link_names.push_back(token);
            }
        }
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

        // Node connectivity
        std::string from_name = column_text(stmt.get(), 3);
        std::string to_name = column_text(stmt.get(), 4);
        int n1 = ctx.node_names.find(from_name);
        int n2 = ctx.node_names.find(to_name);
        ctx.links.node1[idx] = n1;
        ctx.links.node2[idx] = n2;

        // Reset interior vertices for deterministic re-reads.
        ctx.spatial.link_vertices_x[idx].clear();
        ctx.spatial.link_vertices_y[idx].clear();

        // Geometry: extract vertices from linestring
        if (!column_is_null(stmt.get(), 2)) {
            auto blob = column_blob(stmt.get(), 2);
            auto ls = decode_linestring(blob);
            bool reverse_to_match_link = false;

            // Keep interior vertex order consistent with node1 -> node2 direction.
            if (ls.xs.size() >= 2 &&
                n1 >= 0 && n1 < ctx.n_nodes() &&
                n2 >= 0 && n2 < ctx.n_nodes()) {
                const auto u1 = static_cast<std::size_t>(n1);
                const auto u2 = static_cast<std::size_t>(n2);

                const double x0 = ls.xs.front();
                const double y0 = ls.ys.front();
                const double xN = ls.xs.back();
                const double yN = ls.ys.back();

                const double n1x = ctx.spatial.node_x[u1];
                const double n1y = ctx.spatial.node_y[u1];
                const double n2x = ctx.spatial.node_x[u2];
                const double n2y = ctx.spatial.node_y[u2];

                const auto sq = [](double a) { return a * a; };
                const double forward_err =
                    sq(x0 - n1x) + sq(y0 - n1y) + sq(xN - n2x) + sq(yN - n2y);
                const double reverse_err =
                    sq(x0 - n2x) + sq(y0 - n2y) + sq(xN - n1x) + sq(yN - n1y);

                reverse_to_match_link = reverse_err < forward_err;
            }

            // First/last points are endpoints; copy only interior vertices.
            if (ls.xs.size() > 2) {
                if (!reverse_to_match_link) {
                    ctx.spatial.link_vertices_x[idx].assign(ls.xs.begin() + 1, ls.xs.end() - 1);
                    ctx.spatial.link_vertices_y[idx].assign(ls.ys.begin() + 1, ls.ys.end() - 1);
                } else {
                    ctx.spatial.link_vertices_x[idx].assign(ls.xs.rbegin() + 1, ls.xs.rend() - 1);
                    ctx.spatial.link_vertices_y[idx].assign(ls.ys.rbegin() + 1, ls.ys.rend() - 1);
                }
            }
        }

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

        // Reset polygon vertices for deterministic re-reads.
        ctx.spatial.subcatch_polygon_x[idx].clear();
        ctx.spatial.subcatch_polygon_y[idx].clear();

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
// Climate read functions
// ============================================================================

/// Check if a table exists in the database (for backward compat with older .gpkg).
static bool table_exists(sqlite3* db, const std::string& name) {
    auto stmt = prepare(db,
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name=?");
    bind_text(stmt.get(), 1, name);
    if (sqlite3_step(stmt.get()) == SQLITE_ROW)
        return column_int(stmt.get(), 0) > 0;
    return false;
}

/// Parse a comma-separated string of doubles into a fixed-size array.
static void parse_csv_doubles(const std::string& csv, double* out, int n) {
    std::istringstream ss(csv);
    std::string token;
    for (int i = 0; i < n && std::getline(ss, token, ','); ++i) {
        try { out[i] = std::stod(token); } catch (...) {}
    }
}

static void read_evaporation(sqlite3* db, SimulationContext& ctx,
                              const std::string& sim_id) {
    if (!table_exists(db, "evaporation")) return;
    auto stmt = prepare(db,
        "SELECT evap_type, evap_values, ts_name, pan_coeff, "
        "recovery_pat, dry_only FROM evaporation WHERE simulation_id = ?");
    bind_text(stmt.get(), 1, sim_id);

    if (sqlite3_step(stmt.get()) != SQLITE_ROW) return;

    std::string type_str = column_text(stmt.get(), 0);
    if (type_str == "CONSTANT")         ctx.options.evap_type = 0;
    else if (type_str == "MONTHLY")     ctx.options.evap_type = 1;
    else if (type_str == "TIMESERIES")  ctx.options.evap_type = 2;
    else if (type_str == "TEMPERATURE") ctx.options.evap_type = 3;
    else if (type_str == "FILE")        ctx.options.evap_type = 4;

    if (!column_is_null(stmt.get(), 1))
        parse_csv_doubles(column_text(stmt.get(), 1), ctx.options.evap_values, 12);

    if (!column_is_null(stmt.get(), 2))
        ctx.options.evap_ts_name = column_text(stmt.get(), 2);

    if (!column_is_null(stmt.get(), 3))
        parse_csv_doubles(column_text(stmt.get(), 3), ctx.options.pan_coeff, 12);

    if (!column_is_null(stmt.get(), 4))
        ctx.options.evap_recovery_pat = column_text(stmt.get(), 4);

    ctx.options.evap_dry_only = column_int(stmt.get(), 5) != 0;
}

static void read_climate_settings(sqlite3* db, SimulationContext& ctx,
                                   const std::string& sim_id) {
    if (!table_exists(db, "climate_settings")) return;
    auto stmt = prepare(db,
        "SELECT temp_source, temp_ts_name, temp_file, temp_file_start, "
        "wind_type, wind_speed, snow_divt, snow_ati_wt, snow_nrg_ratio, "
        "snow_lat, snow_min_melt, snow_max_melt, adc_imperv, adc_perv "
        "FROM climate_settings WHERE simulation_id = ?");
    bind_text(stmt.get(), 1, sim_id);

    if (sqlite3_step(stmt.get()) != SQLITE_ROW) return;

    std::string src = column_text(stmt.get(), 0);
    if (src == "NONE")            ctx.options.temp_source = 0;
    else if (src == "TIMESERIES") ctx.options.temp_source = 1;
    else if (src == "FILE")       ctx.options.temp_source = 2;

    if (!column_is_null(stmt.get(), 1))
        ctx.options.temp_ts_name = column_text(stmt.get(), 1);

    if (!column_is_null(stmt.get(), 2))
        ctx.options.temp_file = column_text(stmt.get(), 2);

    ctx.options.temp_file_start = column_double(stmt.get(), 3);

    std::string wtype = column_text(stmt.get(), 4);
    ctx.options.wind_type = (wtype == "FILE") ? 1 : 0;

    if (!column_is_null(stmt.get(), 5))
        parse_csv_doubles(column_text(stmt.get(), 5), ctx.options.wind_speed, 12);

    ctx.options.snow_divt      = column_double(stmt.get(), 6);
    ctx.options.snow_ati_wt    = column_double(stmt.get(), 7);
    ctx.options.snow_nrg_ratio = column_double(stmt.get(), 8);
    ctx.options.snow_lat       = column_double(stmt.get(), 9);
    ctx.options.snow_min_melt  = column_double(stmt.get(), 10);
    ctx.options.snow_max_melt  = column_double(stmt.get(), 11);

    if (!column_is_null(stmt.get(), 12))
        parse_csv_doubles(column_text(stmt.get(), 12), ctx.options.adc_imperv, 10);

    if (!column_is_null(stmt.get(), 13))
        parse_csv_doubles(column_text(stmt.get(), 13), ctx.options.adc_perv, 10);
}

static void read_snowpacks(sqlite3* db, SimulationContext& ctx,
                            const std::string& sim_id) {
    if (!table_exists(db, "snowpacks")) return;
    auto stmt = prepare(db,
        "SELECT snowpack_id, surface_type, p1, p2, p3, p4, p5, p6, p7, "
        "removal_subcatch FROM snowpacks WHERE simulation_id = ? "
        "ORDER BY snowpack_id, surface_type");
    bind_text(stmt.get(), 1, sim_id);

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        std::string name = column_text(stmt.get(), 0);
        std::string surface = column_text(stmt.get(), 1);

        int idx = ctx.snowpack_names.find(name);
        if (idx < 0) {
            idx = ctx.snowpack_names.add(name);
            ctx.snowpacks.names.push_back(name);
        }

        // Ensure capacity
        auto ui = static_cast<size_t>(idx + 1);
        if (ctx.snowpacks.plowable.size() < ui) ctx.snowpacks.plowable.resize(ui);
        if (ctx.snowpacks.impervious.size() < ui) ctx.snowpacks.impervious.resize(ui);
        if (ctx.snowpacks.pervious.size() < ui) ctx.snowpacks.pervious.resize(ui);
        if (ctx.snowpacks.removal.size() < ui) ctx.snowpacks.removal.resize(ui);
        if (ctx.snowpacks.removal_subcatch.size() < ui) ctx.snowpacks.removal_subcatch.resize(ui);

        if (surface == "PLOWABLE" || surface == "IMPERVIOUS" || surface == "PERVIOUS") {
            std::array<double, 7> params{};
            for (int k = 0; k < 7; ++k)
                params[static_cast<size_t>(k)] = column_double(stmt.get(), 2 + k);
            if (surface == "PLOWABLE")        ctx.snowpacks.plowable[idx] = params;
            else if (surface == "IMPERVIOUS") ctx.snowpacks.impervious[idx] = params;
            else                              ctx.snowpacks.pervious[idx] = params;
        }
        else if (surface == "REMOVAL") {
            std::array<double, 6> params{};
            for (int k = 0; k < 6; ++k)
                params[static_cast<size_t>(k)] = column_double(stmt.get(), 2 + k);
            ctx.snowpacks.removal[idx] = params;
            if (!column_is_null(stmt.get(), 9))
                ctx.snowpacks.removal_subcatch[idx] = column_text(stmt.get(), 9);
        }
    }
}

static void read_adjustments(sqlite3* db, SimulationContext& ctx,
                              const std::string& sim_id) {
    // Monthly adjustments
    if (table_exists(db, "adjustments")) {
        auto stmt = prepare(db,
            "SELECT adjust_type, adj_values FROM adjustments WHERE simulation_id = ?");
        bind_text(stmt.get(), 1, sim_id);

        while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
            std::string atype = column_text(stmt.get(), 0);
            std::string vals  = column_text(stmt.get(), 1);

            if (atype == "TEMP")         parse_csv_doubles(vals, ctx.adjust_temp, 12);
            else if (atype == "EVAP")    parse_csv_doubles(vals, ctx.adjust_evap, 12);
            else if (atype == "RAIN")    parse_csv_doubles(vals, ctx.adjust_rain, 12);
            else if (atype == "CONDUCT") parse_csv_doubles(vals, ctx.adjust_hydcon, 12);
        }
    }

    // Subcatchment pattern adjustments
    if (table_exists(db, "subcatch_adjustments")) {
        auto stmt = prepare(db,
            "SELECT subcatch_id, adjust_type, pattern_id "
            "FROM subcatch_adjustments WHERE simulation_id = ?");
        bind_text(stmt.get(), 1, sim_id);

        while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
            std::string sc_name = column_text(stmt.get(), 0);
            std::string atype   = column_text(stmt.get(), 1);
            std::string pat_name = column_text(stmt.get(), 2);

            int si = ctx.subcatch_names.find(sc_name);
            int pi = ctx.table_names.find(pat_name);
            if (si < 0 || pi < 0) continue;

            auto usi = static_cast<size_t>(si);
            if (atype == "N-PERV") {
                if (ctx.subcatch_n_perv_pattern.size() <= usi)
                    ctx.subcatch_n_perv_pattern.resize(usi + 1, -1);
                ctx.subcatch_n_perv_pattern[usi] = pi;
            }
            else if (atype == "DSTORE") {
                if (ctx.subcatch_d_store_pattern.size() <= usi)
                    ctx.subcatch_d_store_pattern.resize(usi + 1, -1);
                ctx.subcatch_d_store_pattern[usi] = pi;
            }
            else if (atype == "INFIL") {
                if (ctx.subcatch_infil_pattern.size() <= usi)
                    ctx.subcatch_infil_pattern.resize(usi + 1, -1);
                ctx.subcatch_infil_pattern[usi] = pi;
            }
        }
    }
}

// ============================================================================
// LID read functions
// ============================================================================

static void read_lid_controls(sqlite3* db, SimulationContext& ctx,
                               const std::string& sim_id) {
    if (!table_exists(db, "lid_controls")) return;
    auto stmt = prepare(db,
        "SELECT lid_id, layer_type, p1, p2, p3, p4, p5, p6, p7 "
        "FROM lid_controls WHERE simulation_id = ? "
        "ORDER BY lid_id, fid");
    bind_text(stmt.get(), 1, sim_id);

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        std::string name  = column_text(stmt.get(), 0);
        std::string layer = column_text(stmt.get(), 1);

        int idx = ctx.lid_names.find(name);
        if (idx < 0) {
            idx = ctx.lid_names.add(name);
            ctx.lid_controls.names.push_back(name);
        }

        // Ensure capacity
        auto ui = static_cast<size_t>(idx + 1);
        if (ctx.lid_controls.lid_type.size() < ui) ctx.lid_controls.lid_type.resize(ui);
        if (ctx.lid_controls.surface.size() < ui)  ctx.lid_controls.surface.resize(ui);
        if (ctx.lid_controls.soil.size() < ui)     ctx.lid_controls.soil.resize(ui);
        if (ctx.lid_controls.pavement.size() < ui) ctx.lid_controls.pavement.resize(ui);
        if (ctx.lid_controls.storage.size() < ui)  ctx.lid_controls.storage.resize(ui);
        if (ctx.lid_controls.drain.size() < ui)    ctx.lid_controls.drain.resize(ui);
        if (ctx.lid_controls.drainmat.size() < ui) ctx.lid_controls.drainmat.resize(ui);

        // Type code row (BC, RG, GR, etc.) has no meaningful p values
        if (layer == "SURFACE") {
            for (int k = 0; k < 5; ++k)
                ctx.lid_controls.surface[idx][k] = column_double(stmt.get(), 2 + k);
        }
        else if (layer == "SOIL") {
            for (int k = 0; k < 7; ++k)
                ctx.lid_controls.soil[idx][k] = column_double(stmt.get(), 2 + k);
        }
        else if (layer == "PAVEMENT") {
            for (int k = 0; k < 6; ++k)
                ctx.lid_controls.pavement[idx][k] = column_double(stmt.get(), 2 + k);
        }
        else if (layer == "STORAGE") {
            for (int k = 0; k < 4; ++k)
                ctx.lid_controls.storage[idx][k] = column_double(stmt.get(), 2 + k);
        }
        else if (layer == "DRAIN") {
            for (int k = 0; k < 6; ++k)
                ctx.lid_controls.drain[idx][k] = column_double(stmt.get(), 2 + k);
        }
        else if (layer == "DRAINMAT") {
            for (int k = 0; k < 3; ++k)
                ctx.lid_controls.drainmat[idx][k] = column_double(stmt.get(), 2 + k);
        }
        else {
            // This is the LID type code row (e.g., "BC", "GR", etc.)
            ctx.lid_controls.lid_type[idx] = layer;
        }
    }
}

static void read_lid_usage(sqlite3* db, SimulationContext& ctx,
                            const std::string& sim_id) {
    if (!table_exists(db, "lid_usage")) return;
    auto stmt = prepare(db,
        "SELECT subcatch_id, lid_id, number, area, width, init_sat, "
        "from_imperv, to_perv, rpt_file, drain_to, from_perv "
        "FROM lid_usage WHERE simulation_id = ?");
    bind_text(stmt.get(), 1, sim_id);

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        std::string sc_name  = column_text(stmt.get(), 0);
        std::string lid_name = column_text(stmt.get(), 1);

        int sc_idx  = ctx.subcatch_names.find(sc_name);
        int lid_idx = ctx.lid_names.find(lid_name);

        ctx.lid_usage.subcatch_index.push_back(sc_idx);
        ctx.lid_usage.lid_index.push_back(lid_idx);
        ctx.lid_usage.number.push_back(column_int(stmt.get(), 2));
        ctx.lid_usage.area.push_back(column_double(stmt.get(), 3));
        ctx.lid_usage.width.push_back(column_double(stmt.get(), 4));
        ctx.lid_usage.init_sat.push_back(column_double(stmt.get(), 5));
        ctx.lid_usage.from_imperv.push_back(column_double(stmt.get(), 6));
        ctx.lid_usage.to_perv.push_back(column_int(stmt.get(), 7));
        ctx.lid_usage.rpt_file.push_back(
            column_is_null(stmt.get(), 8) ? std::string{} : column_text(stmt.get(), 8));
        ctx.lid_usage.drain_to.push_back(
            column_is_null(stmt.get(), 9) ? std::string{} : column_text(stmt.get(), 9));
        ctx.lid_usage.from_perv.push_back(column_double(stmt.get(), 10));
    }
}

static void read_rdii(sqlite3* db, SimulationContext& ctx,
                      const std::string& sim_id) {
    // RDII assignments
    if (table_exists(db, "rdii_assignments")) {
        auto stmt = prepare(db,
            "SELECT node_name, uh_name, sewer_area "
            "FROM rdii_assignments WHERE simulation_id = ?");
        bind_text(stmt.get(), 1, sim_id);
        while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
            std::string node_name = column_text(stmt.get(), 0);
            std::string uh_name   = column_text(stmt.get(), 1);
            double area           = sqlite3_column_double(stmt.get(), 2);
            int ni = ctx.node_names.find(node_name);
            if (ni < 0) continue;
            ctx.rdii_assigns.add(ni, uh_name, area);
        }
    }

    // Unit hydrographs (gage lines + parameter lines)
    if (table_exists(db, "unit_hydrographs")) {
        auto stmt = prepare(db,
            "SELECT uh_name, gage_name, month, response, r, t, k, dmax, drecov, dinit "
            "FROM unit_hydrographs WHERE simulation_id = ?");
        bind_text(stmt.get(), 1, sim_id);

        static const char* month_names[] = {"JAN","FEB","MAR","APR","MAY","JUN",
                                            "JUL","AUG","SEP","OCT","NOV","DEC"};
        while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
            std::string uh_name   = column_text(stmt.get(), 0);
            std::string gage_name = column_text(stmt.get(), 1);
            std::string month_str = column_text(stmt.get(), 2);
            std::string resp_str  = column_text(stmt.get(), 3);

            // Gage assignment line (response is NULL/empty)
            if (resp_str.empty() && !gage_name.empty()) {
                ctx.unit_hyds.add_gage(uh_name, gage_name);
                continue;
            }

            // Parameter line
            UnitHydEntry e{};
            e.name = uh_name;
            e.gage_name = gage_name;

            // Parse month
            e.month = -1; // ALL
            for (int m = 0; m < 12; ++m) {
                if (month_str == month_names[m]) { e.month = m; break; }
            }

            // Parse response
            if (resp_str == "SHORT")       e.response = 0;
            else if (resp_str == "MEDIUM") e.response = 1;
            else if (resp_str == "LONG")   e.response = 2;
            else continue;

            e.r      = sqlite3_column_double(stmt.get(), 4);
            e.t      = sqlite3_column_double(stmt.get(), 5);
            e.k      = sqlite3_column_double(stmt.get(), 6);
            e.dmax   = sqlite3_column_double(stmt.get(), 7);
            e.drecov = sqlite3_column_double(stmt.get(), 8);
            e.dinit  = sqlite3_column_double(stmt.get(), 9);
            ctx.unit_hyds.add(e);
        }
    }
}

static void read_treatment(sqlite3* db, SimulationContext& ctx,
                            const std::string& sim_id) {
    if (!table_exists(db, "treatment")) return;
    auto stmt = prepare(db,
        "SELECT node_id, pollutant_id, expression "
        "FROM treatment WHERE simulation_id = ?");
    bind_text(stmt.get(), 1, sim_id);

    int nn = ctx.n_nodes();
    int np = ctx.n_pollutants();
    if (nn <= 0 || np <= 0) return;

    // Ensure treatment data is sized
    if (ctx.treatment.n_nodes == 0)
        ctx.treatment.resize(nn, np);

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        std::string node_name = column_text(stmt.get(), 0);
        std::string poll_name = column_text(stmt.get(), 1);
        std::string expr      = column_text(stmt.get(), 2);

        int ni = ctx.node_names.find(node_name);
        int pi = ctx.pollutant_names.find(poll_name);
        if (ni < 0 || pi < 0 || ni >= nn || pi >= np) continue;

        auto idx = static_cast<size_t>(ni * np + pi);
        if (idx < ctx.treatment.expressions.size())
            ctx.treatment.expressions[idx] = expr;
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
        read_evaporation(db, ctx, simulation_id);
        read_climate_settings(db, ctx, simulation_id);
        read_snowpacks(db, ctx, simulation_id);
        read_adjustments(db, ctx, simulation_id);
        read_lid_controls(db, ctx, simulation_id);
        read_lid_usage(db, ctx, simulation_id);
        read_rdii(db, ctx, simulation_id);
        read_treatment(db, ctx, simulation_id);
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
