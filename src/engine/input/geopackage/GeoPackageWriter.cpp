/**
 * @file GeoPackageWriter.cpp
 * @brief Writes a SimulationContext to GeoPackage (all SWMM input sections).
 * @ingroup engine_geopackage
 */

#include "GeoPackageWriter.hpp"
#include "GeoPackageSchema.hpp"
#include "GpkgUtils.hpp"
#include "GpkgGeometry.hpp"

#include "core/SimulationContext.hpp"
#include "core/UnitConversion.hpp"
#include "data/NodeData.hpp"
#include "data/LinkData.hpp"
#include "data/SubcatchData.hpp"
#include "data/GageData.hpp"
#include "data/TableData.hpp"
#include "data/PollutantData.hpp"
#include "data/InflowData.hpp"

#include <cmath>
#include <string>

#include "data/HydrologyData.hpp"

namespace openswmm::gpkg {

// ============================================================================
// Helper: enum to string conversions
// ============================================================================

static const char* node_type_str(NodeType t) {
    switch (t) {
        case NodeType::JUNCTION: return "JUNCTION";
        case NodeType::OUTFALL:  return "OUTFALL";
        case NodeType::DIVIDER:  return "DIVIDER";
        case NodeType::STORAGE:  return "STORAGE";
    }
    return "JUNCTION";
}

static const char* link_type_str(LinkType t) {
    switch (t) {
        case LinkType::CONDUIT: return "CONDUIT";
        case LinkType::PUMP:    return "PUMP";
        case LinkType::ORIFICE: return "ORIFICE";
        case LinkType::WEIR:    return "WEIR";
        case LinkType::OUTLET:  return "OUTLET";
    }
    return "CONDUIT";
}

static const char* outfall_type_str(OutfallType t) {
    switch (t) {
        case OutfallType::FREE:       return "FREE";
        case OutfallType::NORMAL:     return "NORMAL";
        case OutfallType::FIXED:      return "FIXED";
        case OutfallType::TIDAL:      return "TIDAL";
        case OutfallType::TIMESERIES: return "TIMESERIES";
    }
    return "FREE";
}

static const char* divider_type_str(DividerType t) {
    switch (t) {
        case DividerType::CUTOFF:       return "CUTOFF";
        case DividerType::OVERFLOW_DIV: return "OVERFLOW";
        case DividerType::TABULAR:      return "TABULAR";
        case DividerType::WEIR:         return "WEIR";
    }
    return "CUTOFF";
}

static const char* xsect_shape_str(XsectShape s) {
    switch (s) {
        case XsectShape::CIRCULAR:        return "CIRCULAR";
        case XsectShape::FILLED_CIRCULAR: return "FILLED_CIRCULAR";
        case XsectShape::RECT_CLOSED:     return "RECT_CLOSED";
        case XsectShape::RECT_OPEN:       return "RECT_OPEN";
        case XsectShape::TRAPEZOIDAL:     return "TRAPEZOIDAL";
        case XsectShape::TRIANGULAR:      return "TRIANGULAR";
        case XsectShape::PARABOLIC:       return "PARABOLIC";
        case XsectShape::POWER:           return "POWER";
        case XsectShape::MODBASKETHANDLE: return "MODBASKETHANDLE";
        case XsectShape::EGGSHAPED:       return "EGGSHAPED";
        case XsectShape::HORSESHOE:       return "HORSESHOE";
        case XsectShape::GOTHIC:          return "GOTHIC";
        case XsectShape::CATENARY:        return "CATENARY";
        case XsectShape::SEMIELLIPTICAL:  return "SEMIELLIPTICAL";
        case XsectShape::BASKETHANDLE:    return "BASKETHANDLE";
        case XsectShape::SEMICIRCULAR:    return "SEMICIRCULAR";
        case XsectShape::RECT_TRIANG:     return "RECT_TRIANG";
        case XsectShape::RECT_ROUND:      return "RECT_ROUND";
        case XsectShape::HORIZ_ELLIPSE:   return "HORIZ_ELLIPSE";
        case XsectShape::VERT_ELLIPSE:    return "VERT_ELLIPSE";
        case XsectShape::ARCH:            return "ARCH";
        case XsectShape::IRREGULAR:       return "IRREGULAR";
        case XsectShape::CUSTOM:          return "CUSTOM";
        case XsectShape::FORCE_MAIN:      return "FORCE_MAIN";
        case XsectShape::STREET_XSECT:    return "STREET";
        case XsectShape::DUMMY:           return "DUMMY";
    }
    return "CIRCULAR";
}

// ============================================================================
// Safe access helpers (avoid out-of-bounds on undersized SoA arrays)
// ============================================================================

template<typename T>
static T safe_get(const std::vector<T>& v, size_t i, T def = {}) {
    return i < v.size() ? v[i] : def;
}

static double safe_dbl(const std::vector<double>& v, size_t i) {
    return i < v.size() ? v[i] : 0.0;
}

static int safe_int(const std::vector<int>& v, size_t i) {
    return i < v.size() ? v[i] : -1;
}

// ============================================================================
// Write sections
// ============================================================================

static void write_options(sqlite3* db, const SimulationContext& ctx,
                          const std::string& sim_id) {
    auto stmt = prepare(db,
        "INSERT INTO options (simulation_id, key, value) VALUES (?, ?, ?)");
    const auto& opts = ctx.options;

    auto insert = [&](const std::string& key, const std::string& val) {
        sqlite3_reset(stmt.get());
        sqlite3_clear_bindings(stmt.get());
        bind_text(stmt.get(), 1, sim_id);
        bind_text(stmt.get(), 2, key);
        bind_text(stmt.get(), 3, val);
        sqlite3_step(stmt.get());
    };

    insert("FLOW_UNITS", std::to_string(static_cast<int>(opts.flow_units)));
    insert("INFILTRATION", std::to_string(static_cast<int>(opts.infiltration)));
    insert("ROUTING_MODEL", std::to_string(static_cast<int>(opts.routing_model)));
    insert("LINK_OFFSETS", std::to_string(opts.link_offsets));
    insert("FORCE_MAIN_EQN", std::to_string(opts.force_main_eqn));
    insert("ALLOW_PONDING", opts.allow_ponding ? "YES" : "NO");
    insert("IGNORE_RAINFALL", opts.ignore_rainfall ? "YES" : "NO");
    insert("IGNORE_SNOWMELT", opts.ignore_snow_melt ? "YES" : "NO");
    insert("IGNORE_GW", opts.ignore_groundwater ? "YES" : "NO");
    insert("IGNORE_ROUTING", opts.ignore_routing ? "YES" : "NO");
    insert("IGNORE_QUALITY", opts.ignore_quality ? "YES" : "NO");
    insert("WET_STEP", std::to_string(opts.wet_step));
    insert("DRY_STEP", std::to_string(opts.dry_step));
    insert("ROUTING_STEP", std::to_string(opts.routing_step));
    insert("REPORT_STEP", std::to_string(opts.report_step));
    insert("START_DATE", std::to_string(opts.start_date));
    insert("END_DATE", std::to_string(opts.end_date));
    insert("REPORT_START", std::to_string(opts.report_start));
    insert("SWEEP_START", std::to_string(opts.sweep_start));
    insert("SWEEP_END", std::to_string(opts.sweep_end));
    insert("NODE_CONTINUITY", std::to_string(static_cast<int>(opts.node_continuity)));
    insert("ANDERSON_ACCEL", std::to_string(opts.anderson_accel ? 1 : 0));
    insert("SURCHARGE_METHOD", std::to_string(opts.surcharge_method));
    if (opts.surcharge_method == 2) {
        insert("DPS_CELERITY", std::to_string(opts.dps_target_celerity));
        insert("DPS_ALPHA", std::to_string(opts.dps_alpha));
        insert("DPS_DECAY_TIME", std::to_string(opts.dps_decay_time));
    }
    insert("INERTIAL_DAMPING", std::to_string(opts.inertial_damping));
    insert("NORMAL_FLOW_LIMITED", std::to_string(opts.normal_flow_ltd));
    insert("MAX_TRIALS", std::to_string(opts.max_trials));
    insert("HEAD_TOLERANCE", std::to_string(opts.head_tol));
    insert("VARIABLE_STEP", std::to_string(opts.variable_step));
    insert("MINIMUM_STEP", std::to_string(opts.min_routing_step));
    insert("LENGTHENING_STEP", std::to_string(opts.lengthening_step));
    insert("MIN_SLOPE", std::to_string(opts.min_slope));
    insert("MIN_SURFAREA", std::to_string(opts.min_surf_area));
    insert("SYS_FLOW_TOL", std::to_string(opts.sys_flow_tol));
    insert("LAT_FLOW_TOL", std::to_string(opts.lat_flow_tol));
    insert("THREADS", std::to_string(opts.num_threads));
    insert("DRY_DAYS", std::to_string(opts.dry_days));
    insert("IGNORE_RDII", opts.ignore_rdii ? "YES" : "NO");

    // Report settings
    insert("RPT_DISABLED", opts.rpt_disabled ? "YES" : "NO");
    insert("RPT_INPUT", opts.rpt_input ? "YES" : "NO");
    insert("RPT_CONTINUITY", opts.rpt_continuity ? "YES" : "NO");
    insert("RPT_FLOWSTATS", opts.rpt_flowstats ? "YES" : "NO");
    insert("RPT_CONTROLS", opts.rpt_controls ? "YES" : "NO");
    insert("RPT_AVERAGES", opts.rpt_averages ? "YES" : "NO");
    if (opts.rpt_subcatchments == 0) insert("RPT_SUBCATCHMENTS", "NONE");
    else if (opts.rpt_subcatchments == 1) insert("RPT_SUBCATCHMENTS", "ALL");
    else {
        std::string names;
        for (const auto& n : opts.rpt_subcatch_names) {
            if (!names.empty()) names += ',';
            names += n;
        }
        insert("RPT_SUBCATCHMENTS", names);
    }
    if (opts.rpt_nodes == 0) insert("RPT_NODES", "NONE");
    else if (opts.rpt_nodes == 1) insert("RPT_NODES", "ALL");
    else {
        std::string names;
        for (const auto& n : opts.rpt_node_names) {
            if (!names.empty()) names += ',';
            names += n;
        }
        insert("RPT_NODES", names);
    }
    if (opts.rpt_links == 0) insert("RPT_LINKS", "NONE");
    else if (opts.rpt_links == 1) insert("RPT_LINKS", "ALL");
    else {
        std::string names;
        for (const auto& n : opts.rpt_link_names) {
            if (!names.empty()) names += ',';
            names += n;
        }
        insert("RPT_LINKS", names);
    }

    if (!ctx.spatial.crs.empty())
        insert("CRS", ctx.spatial.crs);
    if (!ctx.spatial.map_units.empty())
        insert("MAP_UNITS", ctx.spatial.map_units);
    if (ctx.spatial.map_x2 != 0.0 || ctx.spatial.map_y2 != 0.0) {
        insert("MAP_X1", std::to_string(ctx.spatial.map_x1));
        insert("MAP_Y1", std::to_string(ctx.spatial.map_y1));
        insert("MAP_X2", std::to_string(ctx.spatial.map_x2));
        insert("MAP_Y2", std::to_string(ctx.spatial.map_y2));
    }
}

static void write_nodes(sqlite3* db, const SimulationContext& ctx,
                        const std::string& sim_id, int srs_id) {
    auto stmt = prepare(db,
        "INSERT INTO nodes (simulation_id, node_id, node_type, geom, "
        "invert_elev, max_depth, init_depth, surcharge_depth, ponded_area, "
        "outfall_type, outfall_stage, outfall_has_flap_gate, "
        "divider_type, divider_cutoff, divider_curve, "
        "storage_curve, storage_a, storage_b, storage_c, tag) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");

    int n = ctx.node_names.size();
    for (int i = 0; i < n; ++i) {
        sqlite3_reset(stmt.get());
        sqlite3_clear_bindings(stmt.get());

        const auto& name = ctx.node_names.name_of(i);
        NodeType ntype = safe_get(ctx.nodes.type, (size_t)i, NodeType::JUNCTION);

        bind_text(stmt.get(), 1, sim_id);
        bind_text(stmt.get(), 2, name);
        bind_text(stmt.get(), 3, node_type_str(ntype));

        // Geometry
        if (i < (int)ctx.spatial.node_x.size() && i < (int)ctx.spatial.node_y.size()) {
            auto geom = encode_point(ctx.spatial.node_x[i], ctx.spatial.node_y[i], srs_id);
            bind_blob(stmt.get(), 4, geom.data(), static_cast<int>(geom.size()));
        } else {
            bind_null(stmt.get(), 4);
        }

        bind_double(stmt.get(), 5, safe_dbl(ctx.nodes.invert_elev, i));
        bind_double(stmt.get(), 6, safe_dbl(ctx.nodes.full_depth, i));
        bind_double(stmt.get(), 7, safe_dbl(ctx.nodes.init_depth, i));
        bind_double(stmt.get(), 8, safe_dbl(ctx.nodes.sur_depth, i));
        bind_double(stmt.get(), 9, safe_dbl(ctx.nodes.ponded_area, i));

        // Outfall fields
        if (ntype == NodeType::OUTFALL) {
            bind_text(stmt.get(), 10, outfall_type_str(safe_get(ctx.nodes.outfall_type, (size_t)i, OutfallType::FREE)));
            bind_double(stmt.get(), 11, safe_dbl(ctx.nodes.outfall_param, i));
            bind_int(stmt.get(), 12, safe_get(ctx.nodes.outfall_has_flap_gate, (size_t)i, uint8_t{0}) ? 1 : 0);
        } else {
            bind_null(stmt.get(), 10);
            bind_null(stmt.get(), 11);
            bind_null(stmt.get(), 12);
        }

        // Divider fields
        if (ntype == NodeType::DIVIDER) {
            bind_text(stmt.get(), 13, divider_type_str(safe_get(ctx.nodes.divider_type, (size_t)i, DividerType::CUTOFF)));
            bind_double(stmt.get(), 14, safe_dbl(ctx.nodes.divider_cutoff, i));
            std::string cname = i < (int)ctx.nodes.divider_curve_name.size() ? ctx.nodes.divider_curve_name[i] : "";
            if (!cname.empty()) bind_text(stmt.get(), 15, cname);
            else bind_null(stmt.get(), 15);
        } else {
            bind_null(stmt.get(), 13);
            bind_null(stmt.get(), 14);
            bind_null(stmt.get(), 15);
        }

        // Storage fields
        if (ntype == NodeType::STORAGE) {
            std::string cname = i < (int)ctx.nodes.storage_curve_name.size() ? ctx.nodes.storage_curve_name[i] : "";
            if (!cname.empty()) bind_text(stmt.get(), 16, cname);
            else bind_null(stmt.get(), 16);
            bind_double(stmt.get(), 17, safe_dbl(ctx.nodes.storage_a, i));
            bind_double(stmt.get(), 18, safe_dbl(ctx.nodes.storage_b, i));
            bind_double(stmt.get(), 19, safe_dbl(ctx.nodes.storage_c, i));
        } else {
            bind_null(stmt.get(), 16);
            bind_null(stmt.get(), 17);
            bind_null(stmt.get(), 18);
            bind_null(stmt.get(), 19);
        }

        // Tag
        auto tag_it = ctx.node_tags.find(name);
        if (tag_it != ctx.node_tags.end())
            bind_text(stmt.get(), 20, tag_it->second);
        else
            bind_null(stmt.get(), 20);

        sqlite3_step(stmt.get());
    }
}

static void write_links(sqlite3* db, const SimulationContext& ctx,
                        const std::string& sim_id, int srs_id) {
    int unit_sys = ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));
    double ucf_len = ucf::Ucf[ucf::LENGTH][unit_sys]; // ft → display (1.0 US, 0.3048 SI)
    auto stmt = prepare(db,
        "INSERT INTO links (simulation_id, link_id, link_type, geom, "
        "from_node, to_node, offset1, offset2, "
        "xsect_shape, xsect_geom1, xsect_geom2, xsect_geom3, xsect_geom4, "
        "xsect_barrels, xsect_culvert, xsect_curve, "
        "roughness, length, loss_inlet, loss_outlet, loss_avg, "
        "has_flap_gate, seep_rate, q0, q_limit, "
        "pump_curve, pump_init_state, pump_startup, pump_shutoff, "
        "crest_height, discharge_coeff, tag) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");

    int n = ctx.link_names.size();
    for (int i = 0; i < n; ++i) {
        sqlite3_reset(stmt.get());
        sqlite3_clear_bindings(stmt.get());

        const auto& name = ctx.link_names.name_of(i);
        LinkType ltype = safe_get(ctx.links.type, (size_t)i, LinkType::CONDUIT);

        bind_text(stmt.get(), 1, sim_id);
        bind_text(stmt.get(), 2, name);
        bind_text(stmt.get(), 3, link_type_str(ltype));

        // Geometry: build linestring from node1 -> vertices -> node2
        int n1 = safe_int(ctx.links.node1, i);
        int n2 = safe_int(ctx.links.node2, i);
        std::string from_name = (n1 >= 0 && n1 < ctx.node_names.size()) ? ctx.node_names.name_of(n1) : "";
        std::string to_name = (n2 >= 0 && n2 < ctx.node_names.size()) ? ctx.node_names.name_of(n2) : "";

        std::vector<double> xs, ys;
        if (n1 >= 0 && n1 < (int)ctx.spatial.node_x.size())
            { xs.push_back(ctx.spatial.node_x[n1]); ys.push_back(ctx.spatial.node_y[n1]); }
        if (i < (int)ctx.spatial.link_vertices_x.size()) {
            const auto& vx = ctx.spatial.link_vertices_x[i];
            const auto& vy = ctx.spatial.link_vertices_y[i];
            for (size_t v = 0; v < vx.size(); ++v) {
                xs.push_back(vx[v]); ys.push_back(vy[v]);
            }
        }
        if (n2 >= 0 && n2 < (int)ctx.spatial.node_x.size())
            { xs.push_back(ctx.spatial.node_x[n2]); ys.push_back(ctx.spatial.node_y[n2]); }

        if (xs.size() >= 2) {
            auto geom = encode_linestring(xs, ys, srs_id);
            bind_blob(stmt.get(), 4, geom.data(), static_cast<int>(geom.size()));
        } else {
            bind_null(stmt.get(), 4);
        }

        bind_text(stmt.get(), 5, from_name);
        bind_text(stmt.get(), 6, to_name);
        bind_double(stmt.get(), 7, safe_dbl(ctx.links.offset1, i));
        bind_double(stmt.get(), 8, safe_dbl(ctx.links.offset2, i));

        // Cross-section
        bind_text(stmt.get(), 9, xsect_shape_str(safe_get(ctx.links.xsect_shape, (size_t)i, XsectShape::CIRCULAR)));
        bind_double(stmt.get(), 10, safe_dbl(ctx.links.xsect_y_full, i));
        bind_double(stmt.get(), 11, safe_dbl(ctx.links.xsect_w_max, i));
        bind_double(stmt.get(), 12, safe_dbl(ctx.links.xsect_a_full, i));
        bind_double(stmt.get(), 13, safe_dbl(ctx.links.xsect_y_bot, i));
        bind_int(stmt.get(), 14, safe_get(ctx.links.barrels, (size_t)i, 1));
        bind_int(stmt.get(), 15, safe_get(ctx.links.culvert_code, (size_t)i, 0));

        // xsect curve name
        int xc = safe_int(ctx.links.xsect_curve, i);
        if (xc >= 0 && xc < ctx.table_names.size())
            bind_text(stmt.get(), 16, ctx.table_names.name_of(xc));
        else
            bind_null(stmt.get(), 16);

        bind_double(stmt.get(), 17, safe_dbl(ctx.links.roughness, i));
        bind_double(stmt.get(), 18, safe_dbl(ctx.links.length, i));
        bind_double(stmt.get(), 19, safe_dbl(ctx.links.loss_inlet, i));
        bind_double(stmt.get(), 20, safe_dbl(ctx.links.loss_outlet, i));
        bind_double(stmt.get(), 21, safe_dbl(ctx.links.loss_avg, i));
        bind_int(stmt.get(), 22, safe_get(ctx.links.has_flap_gate, (size_t)i, uint8_t{0}) ? 1 : 0);
        bind_double(stmt.get(), 23, safe_dbl(ctx.links.seep_rate, i));
        bind_double(stmt.get(), 24, safe_dbl(ctx.links.q0, i));
        bind_double(stmt.get(), 25, safe_dbl(ctx.links.q_limit, i));

        // Pump
        if (ltype == LinkType::PUMP) {
            std::string pcname = i < (int)ctx.links.pump_curve_name.size() ? ctx.links.pump_curve_name[i] : "";
            if (!pcname.empty()) bind_text(stmt.get(), 26, pcname);
            else bind_null(stmt.get(), 26);
            bind_double(stmt.get(), 27, safe_get(ctx.links.pump_init_state, (size_t)i, false) ? 1.0 : 0.0);
            bind_double(stmt.get(), 28, safe_dbl(ctx.links.pump_startup, i) * ucf_len);
            bind_double(stmt.get(), 29, safe_dbl(ctx.links.pump_shutoff, i) * ucf_len);
        } else {
            bind_null(stmt.get(), 26);
            bind_null(stmt.get(), 27);
            bind_null(stmt.get(), 28);
            bind_null(stmt.get(), 29);
        }

        // Weir/Orifice
        bind_double(stmt.get(), 30, safe_dbl(ctx.links.crest_height, i));
        bind_double(stmt.get(), 31, safe_dbl(ctx.links.cd, i));

        // Tag
        auto tag_it = ctx.link_tags.find(name);
        if (tag_it != ctx.link_tags.end())
            bind_text(stmt.get(), 32, tag_it->second);
        else
            bind_null(stmt.get(), 32);

        sqlite3_step(stmt.get());
    }
}

static void write_subcatchments(sqlite3* db, const SimulationContext& ctx,
                                const std::string& sim_id, int srs_id) {
    auto stmt = prepare(db,
        "INSERT INTO subcatchments (simulation_id, subcatch_id, geom, "
        "outlet_node, outlet_subcatch, rain_gage, "
        "area, width, slope, curb_length, frac_imperv, "
        "n_imperv, n_perv, ds_imperv, ds_perv, pct_zero_imperv, "
        "subarea_routing, pct_routed, "
        "infil_model, infil_p1, infil_p2, infil_p3, infil_p4, infil_p5, tag) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");

    int n = ctx.subcatch_names.size();
    for (int i = 0; i < n; ++i) {
        sqlite3_reset(stmt.get());
        sqlite3_clear_bindings(stmt.get());

        const auto& name = ctx.subcatch_names.name_of(i);
        bind_text(stmt.get(), 1, sim_id);
        bind_text(stmt.get(), 2, name);

        // Geometry
        if (i < (int)ctx.spatial.subcatch_polygon_x.size() &&
            !ctx.spatial.subcatch_polygon_x[i].empty()) {
            auto geom = encode_multipolygon(ctx.spatial.subcatch_polygon_x[i],
                                            ctx.spatial.subcatch_polygon_y[i], srs_id);
            bind_blob(stmt.get(), 3, geom.data(), static_cast<int>(geom.size()));
        } else {
            bind_null(stmt.get(), 3);
        }

        // Outlet
        int out_node = safe_int(ctx.subcatches.outlet_node, i);
        int out_sub = safe_int(ctx.subcatches.outlet_subcatch, i);
        if (out_node >= 0 && out_node < ctx.node_names.size())
            bind_text(stmt.get(), 4, ctx.node_names.name_of(out_node));
        else
            bind_null(stmt.get(), 4);
        if (out_sub >= 0 && out_sub < ctx.subcatch_names.size())
            bind_text(stmt.get(), 5, ctx.subcatch_names.name_of(out_sub));
        else
            bind_null(stmt.get(), 5);

        // Rain gage
        int gage_idx = safe_int(ctx.subcatches.gage, i);
        if (gage_idx >= 0 && gage_idx < ctx.gage_names.size())
            bind_text(stmt.get(), 6, ctx.gage_names.name_of(gage_idx));
        else
            bind_null(stmt.get(), 6);

        bind_double(stmt.get(), 7, safe_dbl(ctx.subcatches.area, i));
        bind_double(stmt.get(), 8, safe_dbl(ctx.subcatches.width, i));
        bind_double(stmt.get(), 9, safe_dbl(ctx.subcatches.slope, i));
        bind_double(stmt.get(), 10, safe_dbl(ctx.subcatches.curb_length, i));
        bind_double(stmt.get(), 11, safe_dbl(ctx.subcatches.frac_imperv, i));
        bind_double(stmt.get(), 12, safe_dbl(ctx.subcatches.n_imperv, i));
        bind_double(stmt.get(), 13, safe_dbl(ctx.subcatches.n_perv, i));
        bind_double(stmt.get(), 14, safe_dbl(ctx.subcatches.ds_imperv, i));
        bind_double(stmt.get(), 15, safe_dbl(ctx.subcatches.ds_perv, i));
        bind_double(stmt.get(), 16, safe_dbl(ctx.subcatches.frac_imperv_no_store, i));
        bind_int(stmt.get(), 17, safe_get(ctx.subcatches.subarea_routing, (size_t)i, 0));
        bind_double(stmt.get(), 18, safe_dbl(ctx.subcatches.pct_routed, i));
        bind_int(stmt.get(), 19, safe_get(ctx.subcatches.infil_model, (size_t)i, 0));
        bind_double(stmt.get(), 20, safe_dbl(ctx.subcatches.infil_p1, i));
        bind_double(stmt.get(), 21, safe_dbl(ctx.subcatches.infil_p2, i));
        bind_double(stmt.get(), 22, safe_dbl(ctx.subcatches.infil_p3, i));
        bind_double(stmt.get(), 23, safe_dbl(ctx.subcatches.infil_p4, i));
        bind_double(stmt.get(), 24, safe_dbl(ctx.subcatches.infil_p5, i));

        auto tag_it = ctx.subcatch_tags.find(name);
        if (tag_it != ctx.subcatch_tags.end())
            bind_text(stmt.get(), 25, tag_it->second);
        else
            bind_null(stmt.get(), 25);

        sqlite3_step(stmt.get());
    }
}

static void write_rain_gages(sqlite3* db, const SimulationContext& ctx,
                             const std::string& sim_id, int srs_id) {
    auto stmt = prepare(db,
        "INSERT INTO rain_gages (simulation_id, gage_id, geom, "
        "rain_type, rain_interval, snow_catch, data_source, source_name) "
        "VALUES (?,?,?,?,?,?,?,?)");

    int n = ctx.gage_names.size();
    for (int i = 0; i < n; ++i) {
        sqlite3_reset(stmt.get());
        sqlite3_clear_bindings(stmt.get());

        bind_text(stmt.get(), 1, sim_id);
        bind_text(stmt.get(), 2, ctx.gage_names.name_of(i));

        if (i < (int)ctx.spatial.gage_x.size()) {
            auto geom = encode_point(ctx.spatial.gage_x[i], ctx.spatial.gage_y[i], srs_id);
            bind_blob(stmt.get(), 3, geom.data(), static_cast<int>(geom.size()));
        } else {
            bind_null(stmt.get(), 3);
        }

        bind_int(stmt.get(), 4, safe_get(ctx.gages.rain_type, (size_t)i, 0));
        bind_int(stmt.get(), 5, safe_get(ctx.gages.interval_sec, (size_t)i, 3600));
        bind_double(stmt.get(), 6, safe_dbl(ctx.gages.snow_factor, i));
        bind_int(stmt.get(), 7, static_cast<int>(safe_get(ctx.gages.source, (size_t)i, RainSource::TIMESERIES)));

        int ts = safe_int(ctx.gages.ts_index, i);
        if (ts >= 0 && ts < ctx.table_names.size())
            bind_text(stmt.get(), 8, ctx.table_names.name_of(ts));
        else
            bind_null(stmt.get(), 8);

        sqlite3_step(stmt.get());
    }
}

static void write_topology(sqlite3* db, const SimulationContext& ctx,
                           const std::string& sim_id) {
    // node_links
    auto stmt = prepare(db,
        "INSERT INTO node_links (simulation_id, link_id, from_node, to_node, link_type, direction) "
        "VALUES (?,?,?,?,?,?)");

    int n_links = ctx.link_names.size();
    for (int i = 0; i < n_links; ++i) {
        sqlite3_reset(stmt.get());
        sqlite3_clear_bindings(stmt.get());

        int n1 = safe_int(ctx.links.node1, i);
        int n2 = safe_int(ctx.links.node2, i);
        bind_text(stmt.get(), 1, sim_id);
        bind_text(stmt.get(), 2, ctx.link_names.name_of(i));
        bind_text(stmt.get(), 3, (n1 >= 0 && n1 < ctx.node_names.size()) ? ctx.node_names.name_of(n1) : "");
        bind_text(stmt.get(), 4, (n2 >= 0 && n2 < ctx.node_names.size()) ? ctx.node_names.name_of(n2) : "");
        bind_text(stmt.get(), 5, link_type_str(safe_get(ctx.links.type, (size_t)i, LinkType::CONDUIT)));
        bind_int(stmt.get(), 6, 1);
        sqlite3_step(stmt.get());
    }

    // subcatch_routing
    auto stmt2 = prepare(db,
        "INSERT INTO subcatch_routing (simulation_id, subcatch_id, outlet_type, outlet_node, outlet_subcatch) "
        "VALUES (?,?,?,?,?)");

    int n_sub = ctx.subcatch_names.size();
    for (int i = 0; i < n_sub; ++i) {
        sqlite3_reset(stmt2.get());
        sqlite3_clear_bindings(stmt2.get());

        int out_node = safe_int(ctx.subcatches.outlet_node, i);
        int out_sub = safe_int(ctx.subcatches.outlet_subcatch, i);

        bind_text(stmt2.get(), 1, sim_id);
        bind_text(stmt2.get(), 2, ctx.subcatch_names.name_of(i));

        if (out_node >= 0 && out_node < ctx.node_names.size()) {
            bind_text(stmt2.get(), 3, "NODE");
            bind_text(stmt2.get(), 4, ctx.node_names.name_of(out_node));
            bind_null(stmt2.get(), 5);
        } else if (out_sub >= 0 && out_sub < ctx.subcatch_names.size()) {
            bind_text(stmt2.get(), 3, "SUBCATCHMENT");
            bind_null(stmt2.get(), 4);
            bind_text(stmt2.get(), 5, ctx.subcatch_names.name_of(out_sub));
        } else {
            bind_text(stmt2.get(), 3, "NODE");
            bind_text(stmt2.get(), 4, "");
            bind_null(stmt2.get(), 5);
        }
        sqlite3_step(stmt2.get());
    }
}

static void write_curves(sqlite3* db, const SimulationContext& ctx,
                         const std::string& sim_id) {
    auto stmt = prepare(db,
        "INSERT INTO curves (simulation_id, curve_id, curve_type, x_value, y_value, ordinal) "
        "VALUES (?,?,?,?,?,?)");

    int n = ctx.table_names.size();
    for (int i = 0; i < n; ++i) {
        if (i >= (int)ctx.tables.count()) break;
        const auto& tbl = ctx.tables[i];
        if (tbl.type == TableType::TIMESERIES) continue;

        const auto& name = ctx.table_names.name_of(i);
        std::string ctype = std::to_string(static_cast<int>(tbl.type));

        for (int j = 0; j < (int)tbl.x.size(); ++j) {
            sqlite3_reset(stmt.get());
            sqlite3_clear_bindings(stmt.get());
            bind_text(stmt.get(), 1, sim_id);
            bind_text(stmt.get(), 2, name);
            bind_text(stmt.get(), 3, ctype);
            bind_double(stmt.get(), 4, tbl.x[j]);
            bind_double(stmt.get(), 5, tbl.y[j]);
            bind_int(stmt.get(), 6, j);
            sqlite3_step(stmt.get());
        }
    }
}

static void write_timeseries(sqlite3* db, const SimulationContext& ctx,
                             const std::string& sim_id) {
    auto stmt = prepare(db,
        "INSERT INTO input_timeseries (simulation_id, series_id, timestamp, value, ordinal) "
        "VALUES (?,?,?,?,?)");

    int n = ctx.table_names.size();
    for (int i = 0; i < n; ++i) {
        if (i >= (int)ctx.tables.count()) break;
        const auto& tbl = ctx.tables[i];
        if (tbl.type != TableType::TIMESERIES) continue;

        const auto& name = ctx.table_names.name_of(i);

        for (int j = 0; j < (int)tbl.x.size(); ++j) {
            sqlite3_reset(stmt.get());
            sqlite3_clear_bindings(stmt.get());
            bind_text(stmt.get(), 1, sim_id);
            bind_text(stmt.get(), 2, name);
            bind_text(stmt.get(), 3, std::to_string(tbl.x[j]));
            bind_double(stmt.get(), 4, tbl.y[j]);
            bind_int(stmt.get(), 5, j);
            sqlite3_step(stmt.get());
        }
    }
}

// ============================================================================
// Climate write functions
// ============================================================================

/// Serialize a fixed-size double array as comma-separated string.
static std::string join_doubles(const double* arr, int n) {
    std::string out;
    for (int i = 0; i < n; ++i) {
        if (i > 0) out += ',';
        out += std::to_string(arr[i]);
    }
    return out;
}

static void write_evaporation(sqlite3* db, const SimulationContext& ctx,
                               const std::string& sim_id) {
    auto stmt = prepare(db,
        "INSERT INTO evaporation (simulation_id, evap_type, evap_values, "
        "ts_name, pan_coeff, recovery_pat, dry_only) VALUES (?,?,?,?,?,?,?)");

    static const char* evap_names[] = {"CONSTANT","MONTHLY","TIMESERIES","TEMPERATURE","FILE"};
    const auto& opts = ctx.options;
    int et = opts.evap_type;
    if (et < 0 || et > 4) et = 0;

    bind_text(stmt.get(), 1, sim_id);
    bind_text(stmt.get(), 2, evap_names[et]);
    bind_text(stmt.get(), 3, join_doubles(opts.evap_values, 12));

    if (!opts.evap_ts_name.empty())
        bind_text(stmt.get(), 4, opts.evap_ts_name);
    else
        bind_null(stmt.get(), 4);

    bind_text(stmt.get(), 5, join_doubles(opts.pan_coeff, 12));

    if (!opts.evap_recovery_pat.empty())
        bind_text(stmt.get(), 6, opts.evap_recovery_pat);
    else
        bind_null(stmt.get(), 6);

    bind_int(stmt.get(), 7, opts.evap_dry_only ? 1 : 0);
    sqlite3_step(stmt.get());
}

static void write_climate_settings(sqlite3* db, const SimulationContext& ctx,
                                    const std::string& sim_id) {
    auto stmt = prepare(db,
        "INSERT INTO climate_settings (simulation_id, temp_source, temp_ts_name, "
        "temp_file, temp_file_start, wind_type, wind_speed, snow_divt, snow_ati_wt, "
        "snow_nrg_ratio, snow_lat, snow_min_melt, snow_max_melt, "
        "adc_imperv, adc_perv) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    const auto& opts = ctx.options;

    static const char* src_names[] = {"NONE", "TIMESERIES", "FILE"};
    int ts = opts.temp_source;
    if (ts < 0 || ts > 2) ts = 0;

    bind_text(stmt.get(),   1, sim_id);
    bind_text(stmt.get(),   2, src_names[ts]);

    if (!opts.temp_ts_name.empty())
        bind_text(stmt.get(), 3, opts.temp_ts_name);
    else
        bind_null(stmt.get(), 3);

    if (!opts.temp_file.empty())
        bind_text(stmt.get(), 4, opts.temp_file);
    else
        bind_null(stmt.get(), 4);

    bind_double(stmt.get(), 5, opts.temp_file_start);

    bind_text(stmt.get(),   6, opts.wind_type == 1 ? "FILE" : "MONTHLY");
    bind_text(stmt.get(),   7, join_doubles(opts.wind_speed, 12));
    bind_double(stmt.get(), 8, opts.snow_divt);
    bind_double(stmt.get(), 9, opts.snow_ati_wt);
    bind_double(stmt.get(), 10, opts.snow_nrg_ratio);
    bind_double(stmt.get(), 11, opts.snow_lat);
    bind_double(stmt.get(), 12, opts.snow_min_melt);
    bind_double(stmt.get(), 13, opts.snow_max_melt);
    bind_text(stmt.get(),   14, join_doubles(opts.adc_imperv, 10));
    bind_text(stmt.get(),   15, join_doubles(opts.adc_perv, 10));
    sqlite3_step(stmt.get());
}

static void write_snowpacks(sqlite3* db, const SimulationContext& ctx,
                             const std::string& sim_id) {
    auto stmt = prepare(db,
        "INSERT INTO snowpacks (simulation_id, snowpack_id, surface_type, "
        "p1, p2, p3, p4, p5, p6, p7, removal_subcatch) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?)");

    for (size_t j = 0; j < ctx.snowpacks.names.size(); ++j) {
        const auto& name = ctx.snowpacks.names[j];

        // Helper to write one surface row (7-param surfaces)
        auto write_surface7 = [&](const char* stype, const std::array<double,7>& p) {
            sqlite3_reset(stmt.get());
            sqlite3_clear_bindings(stmt.get());
            bind_text(stmt.get(), 1, sim_id);
            bind_text(stmt.get(), 2, name);
            bind_text(stmt.get(), 3, stype);
            for (int k = 0; k < 7; ++k)
                bind_double(stmt.get(), 4 + k, p[static_cast<size_t>(k)]);
            bind_null(stmt.get(), 11);
            sqlite3_step(stmt.get());
        };

        if (j < ctx.snowpacks.plowable.size())
            write_surface7("PLOWABLE", ctx.snowpacks.plowable[j]);
        if (j < ctx.snowpacks.impervious.size())
            write_surface7("IMPERVIOUS", ctx.snowpacks.impervious[j]);
        if (j < ctx.snowpacks.pervious.size())
            write_surface7("PERVIOUS", ctx.snowpacks.pervious[j]);

        // REMOVAL: 6 params + optional subcatchment
        if (j < ctx.snowpacks.removal.size()) {
            const auto& r = ctx.snowpacks.removal[j];
            sqlite3_reset(stmt.get());
            sqlite3_clear_bindings(stmt.get());
            bind_text(stmt.get(), 1, sim_id);
            bind_text(stmt.get(), 2, name);
            bind_text(stmt.get(), 3, "REMOVAL");
            for (int k = 0; k < 6; ++k)
                bind_double(stmt.get(), 4 + k, r[static_cast<size_t>(k)]);
            bind_double(stmt.get(), 10, 0.0); // p7 unused
            if (j < ctx.snowpacks.removal_subcatch.size() &&
                !ctx.snowpacks.removal_subcatch[j].empty())
                bind_text(stmt.get(), 11, ctx.snowpacks.removal_subcatch[j]);
            else
                bind_null(stmt.get(), 11);
            sqlite3_step(stmt.get());
        }
    }
}

static void write_adjustments(sqlite3* db, const SimulationContext& ctx,
                               const std::string& sim_id) {
    // Monthly adjustments
    {
        auto stmt = prepare(db,
            "INSERT INTO adjustments (simulation_id, adjust_type, adj_values) "
            "VALUES (?,?,?)");

        auto write_row = [&](const char* atype, const double* arr) {
            sqlite3_reset(stmt.get());
            sqlite3_clear_bindings(stmt.get());
            bind_text(stmt.get(), 1, sim_id);
            bind_text(stmt.get(), 2, atype);
            bind_text(stmt.get(), 3, join_doubles(arr, 12));
            sqlite3_step(stmt.get());
        };

        write_row("TEMP",    ctx.adjust_temp);
        write_row("EVAP",    ctx.adjust_evap);
        write_row("RAIN",    ctx.adjust_rain);
        write_row("CONDUCT", ctx.adjust_hydcon);
    }

    // Subcatchment pattern adjustments
    {
        auto stmt = prepare(db,
            "INSERT INTO subcatch_adjustments (simulation_id, subcatch_id, "
            "adjust_type, pattern_id) VALUES (?,?,?,?)");

        auto write_pat = [&](const char* atype, const std::vector<int>& pats) {
            for (size_t i = 0; i < pats.size(); ++i) {
                if (pats[i] < 0) continue;
                sqlite3_reset(stmt.get());
                sqlite3_clear_bindings(stmt.get());
                bind_text(stmt.get(), 1, sim_id);
                bind_text(stmt.get(), 2, ctx.subcatch_names.name_of(static_cast<int>(i)));
                bind_text(stmt.get(), 3, atype);
                bind_text(stmt.get(), 4, ctx.table_names.name_of(pats[i]));
                sqlite3_step(stmt.get());
            }
        };

        write_pat("N-PERV", ctx.subcatch_n_perv_pattern);
        write_pat("DSTORE", ctx.subcatch_d_store_pattern);
        write_pat("INFIL",  ctx.subcatch_infil_pattern);
    }
}

static void write_pollutants(sqlite3* db, const SimulationContext& ctx,
                             const std::string& sim_id) {
    auto stmt = prepare(db,
        "INSERT INTO pollutants (simulation_id, pollutant_id, units, rain_conc, gw_conc, "
        "decay_coeff, snow_only, co_pollutant, co_fraction) "
        "VALUES (?,?,?,?,?,?,?,?,?)");

    int n = ctx.pollutant_names.size();
    for (int i = 0; i < n; ++i) {
        sqlite3_reset(stmt.get());
        sqlite3_clear_bindings(stmt.get());
        bind_text(stmt.get(), 1, sim_id);
        bind_text(stmt.get(), 2, ctx.pollutant_names.name_of(i));
        bind_int(stmt.get(), 3, static_cast<int>(safe_get(ctx.pollutants.units, (size_t)i, MassUnits::MG_PER_L)));
        bind_double(stmt.get(), 4, safe_dbl(ctx.pollutants.c_rain, i));
        bind_double(stmt.get(), 5, safe_dbl(ctx.pollutants.c_gw, i));
        bind_double(stmt.get(), 6, safe_dbl(ctx.pollutants.k_decay, i));
        bind_int(stmt.get(), 7, safe_get(ctx.pollutants.snow_only, (size_t)i, false) ? 1 : 0);
        int co = safe_int(ctx.pollutants.co_pollut, i);
        if (co >= 0 && co < ctx.pollutant_names.size())
            bind_text(stmt.get(), 8, ctx.pollutant_names.name_of(co));
        else
            bind_null(stmt.get(), 8);
        bind_double(stmt.get(), 9, safe_dbl(ctx.pollutants.co_frac, i));
        sqlite3_step(stmt.get());
    }
}

static void write_patterns(sqlite3* db, const SimulationContext& ctx,
                           const std::string& sim_id) {
    auto stmt = prepare(db,
        "INSERT INTO patterns (simulation_id, pattern_id, pattern_type, ordinal, factor) "
        "VALUES (?,?,?,?,?)");

    int n = ctx.patterns.count();
    for (int i = 0; i < n; ++i) {
        if (i >= (int)ctx.patterns.types.size()) break;
        if (i >= (int)ctx.patterns.factors.size()) break;

        const auto& factors = ctx.patterns.factors[i];
        for (int j = 0; j < (int)factors.size(); ++j) {
            sqlite3_reset(stmt.get());
            sqlite3_clear_bindings(stmt.get());
            bind_text(stmt.get(), 1, sim_id);
            bind_text(stmt.get(), 2, ctx.patterns.names[i]);
            bind_int(stmt.get(), 3, ctx.patterns.types[i]);
            bind_int(stmt.get(), 4, j);
            bind_double(stmt.get(), 5, factors[j]);
            sqlite3_step(stmt.get());
        }
    }
}

static void write_lid_controls(sqlite3* db, const SimulationContext& ctx,
                                const std::string& sim_id) {
    auto stmt = prepare(db,
        "INSERT INTO lid_controls (simulation_id, lid_id, layer_type, "
        "p1, p2, p3, p4, p5, p6, p7) VALUES (?,?,?,?,?,?,?,?,?,?)");

    for (int j = 0; j < ctx.lid_controls.count(); ++j) {
        auto uj = static_cast<size_t>(j);
        const auto& name = ctx.lid_controls.names[uj];

        // Write type row (layer_type = the LID type code e.g. "BC")
        auto write_row = [&](const char* layer, const double* p, int np) {
            sqlite3_reset(stmt.get());
            sqlite3_clear_bindings(stmt.get());
            bind_text(stmt.get(), 1, sim_id);
            bind_text(stmt.get(), 2, name);
            bind_text(stmt.get(), 3, layer);
            for (int k = 0; k < 7; ++k)
                bind_double(stmt.get(), 4 + k, (k < np) ? p[k] : 0.0);
            sqlite3_step(stmt.get());
        };

        // Type code
        double zero = 0.0;
        write_row(ctx.lid_controls.lid_type[uj].c_str(), &zero, 0);

        if (uj < ctx.lid_controls.surface.size())
            write_row("SURFACE", ctx.lid_controls.surface[uj].data(), 5);
        if (uj < ctx.lid_controls.soil.size())
            write_row("SOIL", ctx.lid_controls.soil[uj].data(), 7);
        if (uj < ctx.lid_controls.pavement.size())
            write_row("PAVEMENT", ctx.lid_controls.pavement[uj].data(), 6);
        if (uj < ctx.lid_controls.storage.size())
            write_row("STORAGE", ctx.lid_controls.storage[uj].data(), 4);
        if (uj < ctx.lid_controls.drain.size())
            write_row("DRAIN", ctx.lid_controls.drain[uj].data(), 6);
        if (uj < ctx.lid_controls.drainmat.size())
            write_row("DRAINMAT", ctx.lid_controls.drainmat[uj].data(), 3);
    }
}

static void write_lid_usage(sqlite3* db, const SimulationContext& ctx,
                             const std::string& sim_id) {
    auto stmt = prepare(db,
        "INSERT INTO lid_usage (simulation_id, subcatch_id, lid_id, number, "
        "area, width, init_sat, from_imperv, to_perv, rpt_file, drain_to, "
        "from_perv) VALUES (?,?,?,?,?,?,?,?,?,?,?,?)");

    for (int j = 0; j < ctx.lid_usage.count(); ++j) {
        auto uj = static_cast<size_t>(j);
        sqlite3_reset(stmt.get());
        sqlite3_clear_bindings(stmt.get());

        bind_text(stmt.get(), 1, sim_id);

        int sc = ctx.lid_usage.subcatch_index[uj];
        bind_text(stmt.get(), 2, (sc >= 0) ? ctx.subcatch_names.name_of(sc) : std::string("*"));

        int li = ctx.lid_usage.lid_index[uj];
        bind_text(stmt.get(), 3, (li >= 0 && li < ctx.lid_names.size())
                  ? ctx.lid_names.name_of(li) : std::string("*"));

        bind_int(stmt.get(), 4, ctx.lid_usage.number[uj]);
        bind_double(stmt.get(), 5, ctx.lid_usage.area[uj]);
        bind_double(stmt.get(), 6, ctx.lid_usage.width[uj]);
        bind_double(stmt.get(), 7, ctx.lid_usage.init_sat[uj]);
        bind_double(stmt.get(), 8, ctx.lid_usage.from_imperv[uj]);
        bind_int(stmt.get(), 9, ctx.lid_usage.to_perv[uj]);

        if (uj < ctx.lid_usage.rpt_file.size() && !ctx.lid_usage.rpt_file[uj].empty())
            bind_text(stmt.get(), 10, ctx.lid_usage.rpt_file[uj]);
        else
            bind_null(stmt.get(), 10);

        if (uj < ctx.lid_usage.drain_to.size() && !ctx.lid_usage.drain_to[uj].empty())
            bind_text(stmt.get(), 11, ctx.lid_usage.drain_to[uj]);
        else
            bind_null(stmt.get(), 11);

        bind_double(stmt.get(), 12, (uj < ctx.lid_usage.from_perv.size())
                    ? ctx.lid_usage.from_perv[uj] : 0.0);
        sqlite3_step(stmt.get());
    }
}

static void write_rdii(sqlite3* db, const SimulationContext& ctx,
                       const std::string& sim_id) {
    // RDII assignments
    if (ctx.rdii_assigns.count() > 0) {
        auto stmt = prepare(db,
            "INSERT INTO rdii_assignments (simulation_id, node_name, uh_name, sewer_area) "
            "VALUES (?,?,?,?)");
        for (int j = 0; j < ctx.rdii_assigns.count(); ++j) {
            auto uj = static_cast<size_t>(j);
            sqlite3_reset(stmt.get());
            sqlite3_clear_bindings(stmt.get());
            bind_text(stmt.get(), 1, sim_id);
            bind_text(stmt.get(), 2, (ctx.rdii_assigns.node_idx[uj] >= 0 &&
                      ctx.rdii_assigns.node_idx[uj] < ctx.node_names.size())
                      ? ctx.node_names.name_of(ctx.rdii_assigns.node_idx[uj])
                      : std::string("*"));
            bind_text(stmt.get(), 3, ctx.rdii_assigns.uh_name[uj]);
            sqlite3_bind_double(stmt.get(), 4, ctx.rdii_assigns.sewer_area[uj]);
            sqlite3_step(stmt.get());
        }
    }

    // Unit hydrograph gage assignments
    if (!ctx.unit_hyds.gage_assignments.empty()) {
        auto stmt = prepare(db,
            "INSERT INTO unit_hydrographs (simulation_id, uh_name, gage_name) "
            "VALUES (?,?,?)");
        for (size_t i = 0; i < ctx.unit_hyds.gage_assignments.size(); ++i) {
            sqlite3_reset(stmt.get());
            sqlite3_clear_bindings(stmt.get());
            bind_text(stmt.get(), 1, sim_id);
            bind_text(stmt.get(), 2, ctx.unit_hyds.gage_assignments[i]);
            bind_text(stmt.get(), 3, ctx.unit_hyds.gage_names[i]);
            sqlite3_step(stmt.get());
        }
    }

    // Unit hydrograph parameter entries
    if (ctx.unit_hyds.count() > 0) {
        auto stmt = prepare(db,
            "INSERT INTO unit_hydrographs (simulation_id, uh_name, month, response, "
            "r, t, k, dmax, drecov, dinit) VALUES (?,?,?,?,?,?,?,?,?,?)");
        static const char* months[] = {"JAN","FEB","MAR","APR","MAY","JUN",
                                       "JUL","AUG","SEP","OCT","NOV","DEC"};
        static const char* responses[] = {"SHORT","MEDIUM","LONG"};
        for (const auto& e : ctx.unit_hyds.entries) {
            sqlite3_reset(stmt.get());
            sqlite3_clear_bindings(stmt.get());
            bind_text(stmt.get(), 1, sim_id);
            bind_text(stmt.get(), 2, e.name);
            bind_text(stmt.get(), 3, (e.month < 0) ? "ALL" : months[e.month]);
            bind_text(stmt.get(), 4, responses[e.response]);
            sqlite3_bind_double(stmt.get(), 5, e.r);
            sqlite3_bind_double(stmt.get(), 6, e.t);
            sqlite3_bind_double(stmt.get(), 7, e.k);
            sqlite3_bind_double(stmt.get(), 8, e.dmax);
            sqlite3_bind_double(stmt.get(), 9, e.drecov);
            sqlite3_bind_double(stmt.get(), 10, e.dinit);
            sqlite3_step(stmt.get());
        }
    }
}

static void write_treatment(sqlite3* db, const SimulationContext& ctx,
                             const std::string& sim_id) {
    if (!ctx.treatment.hasAny()) return;
    auto stmt = prepare(db,
        "INSERT INTO treatment (simulation_id, node_id, pollutant_id, expression) "
        "VALUES (?,?,?,?)");

    int np = ctx.treatment.n_pollutants;
    for (int n = 0; n < ctx.treatment.n_nodes; ++n) {
        for (int p = 0; p < np; ++p) {
            auto idx = static_cast<size_t>(n * np + p);
            if (idx >= ctx.treatment.expressions.size()) continue;
            if (ctx.treatment.expressions[idx].empty()) continue;

            sqlite3_reset(stmt.get());
            sqlite3_clear_bindings(stmt.get());
            bind_text(stmt.get(), 1, sim_id);
            bind_text(stmt.get(), 2, (n >= 0 && n < ctx.node_names.size())
                      ? ctx.node_names.name_of(n) : std::string("*"));
            bind_text(stmt.get(), 3, (p >= 0 && p < ctx.pollutant_names.size())
                      ? ctx.pollutant_names.name_of(p) : std::string("*"));
            bind_text(stmt.get(), 4, ctx.treatment.expressions[idx]);
            sqlite3_step(stmt.get());
        }
    }
}

// ============================================================================
// Public API
// ============================================================================

void write_model(sqlite3* db, const SimulationContext& ctx,
                 const std::string& simulation_id, int srs_id) {
    Transaction txn(db);

    // Register CRS and feature tables
    const auto& sp = ctx.spatial;
    register_feature_table(db, "nodes", "POINT", srs_id,
        "Network Nodes", "Junctions, outfalls, dividers, storage units",
        sp.map_x1, sp.map_y1, sp.map_x2, sp.map_y2);
    register_feature_table(db, "links", "LINESTRING", srs_id,
        "Network Links", "Conduits, pumps, orifices, weirs, outlets",
        sp.map_x1, sp.map_y1, sp.map_x2, sp.map_y2);
    register_feature_table(db, "subcatchments", "MULTIPOLYGON", srs_id,
        "Subcatchments", "Subcatchment areas",
        sp.map_x1, sp.map_y1, sp.map_x2, sp.map_y2);
    register_feature_table(db, "rain_gages", "POINT", srs_id,
        "Rain Gages", "Rainfall measurement stations",
        sp.map_x1, sp.map_y1, sp.map_x2, sp.map_y2);

    populate_default_variables(db);

    // Write all sections
    write_options(db, ctx, simulation_id);
    write_nodes(db, ctx, simulation_id, srs_id);
    write_links(db, ctx, simulation_id, srs_id);
    write_subcatchments(db, ctx, simulation_id, srs_id);
    write_rain_gages(db, ctx, simulation_id, srs_id);
    write_topology(db, ctx, simulation_id);
    write_curves(db, ctx, simulation_id);
    write_timeseries(db, ctx, simulation_id);
    write_pollutants(db, ctx, simulation_id);
    write_patterns(db, ctx, simulation_id);
    write_evaporation(db, ctx, simulation_id);
    write_climate_settings(db, ctx, simulation_id);
    write_snowpacks(db, ctx, simulation_id);
    write_adjustments(db, ctx, simulation_id);
    write_lid_controls(db, ctx, simulation_id);
    write_lid_usage(db, ctx, simulation_id);
    write_rdii(db, ctx, simulation_id);
    write_treatment(db, ctx, simulation_id);

    txn.commit();
}

int write_to_file(const std::string& path, const SimulationContext& ctx,
                  const std::string& simulation_id) {
    try {
        auto db = open_database(path);
        create_schema(db.get());
        write_model(db.get(), ctx, simulation_id);
        return 0;
    } catch (const std::exception&) {
        return -1;
    }
}

} // namespace openswmm::gpkg
