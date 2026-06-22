/**
 * @file GeoPackageWriter.cpp
 * @brief Writes a SimulationContext to GeoPackage (all SWMM input sections).
 * @ingroup engine_geopackage
 */

#include "GeoPackageWriter.hpp"
#include "ExternalContentWriter.hpp"
#include "GeoPackageSchema.hpp"
#include "GpkgUtils.hpp"
#include "GpkgGeometry.hpp"

#include "core/SimulationContext.hpp"
#include "core/UnitConversion.hpp"
#include "input/PostParseResolver.hpp"
#include "data/NodeData.hpp"
#include "data/LinkData.hpp"
#include "data/SubcatchData.hpp"
#include "data/GageData.hpp"
#include "data/TableData.hpp"
#include "data/PollutantData.hpp"
#include "data/InflowData.hpp"

// 2D model definition (plain define-free data structs; reached at runtime
// through ctx.twod_io — null in non-2D engine builds).
#include "2d/data/MeshData.hpp"
#include "2d/data/SolverOptions2D.hpp"
#include "2d/data/BoundaryData.hpp"
#include "2d/data/PendingRows2D.hpp"
#include "2d/data/Serialize2D.hpp"

#include <cmath>
#include <cstdio>
#include <string>

#include "data/HydrologyData.hpp"
#include "core/DateTime.hpp"

namespace openswmm::gpkg {

namespace {
// OADates for years >= 1910 are > 3650; no relative storm exceeds 10 years.
static constexpr double kAbsoluteTsThreshold = 3650.0;

// Format a time series x value for storage in the GeoPackage.
// Stored as the RAW OADate at full double precision (the canonical "store
// internal" form). By save time PostParseResolver has already absolutized every
// series (x>=366), and on read it applies NO offset to absolute series, so the
// value round-trips bit-for-bit. The prior relative-hours/date-string forms lost
// precision to the start_date subtraction (catastrophic cancellation at ~36000),
// the 6-dp start_date, and HH:MM truncation — breaking byte-exact .out parity.
static std::string format_ts_timestamp(double x, double /*startDate*/) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.17g", x);
    return buf;
}
} // anonymous namespace

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

// Phase 7: subtype discriminator code -> NAMED string (mirrors LinksHandler
// parse mapping). Round-tripped by the reader's *_from_str() below.
static const char* orifice_orientation_str(double t) {  // OrificeData.orifice_type
    return (t >= 0.5) ? "SIDE" : "BOTTOM";              // 1.0=SIDE, 0.0=BOTTOM
}
static const char* weir_type_str(double t) {            // WeirData.weir_type
    switch (static_cast<int>(t + 0.5)) {
        case 0: return "TRANSVERSE";
        case 1: return "SIDEFLOW";
        case 2: return "V-NOTCH";
        case 3: return "TRAPEZOIDAL";
        case 4: return "ROADWAY";
    }
    return "TRANSVERSE";
}
static const char* outlet_rating_str(double t) {        // OutletData.outlet_type
    switch (static_cast<int>(t + 0.5)) {
        case 0: return "FUNCTIONAL/HEAD";
        case 1: return "FUNCTIONAL/DEPTH";
        case 2: return "TABULAR/HEAD";
        case 3: return "TABULAR/DEPTH";
    }
    return "FUNCTIONAL/HEAD";
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
    // Double-valued options at FULL precision — std::to_string emits only 6 dp,
    // which rounds dates/steps/tolerances (e.g. START_DATE 36161.0416666… →
    // 36161.041667, a ~0.026 s shift) and breaks byte-exact .out round-trip.
    auto insert_d = [&](const std::string& key, double val) {
        char b[32]; std::snprintf(b, sizeof(b), "%.17g", val); insert(key, b);
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
    insert_d("WET_STEP", opts.wet_step);
    insert_d("DRY_STEP", opts.dry_step);
    insert_d("ROUTING_STEP", opts.routing_step);
    insert_d("REPORT_STEP", opts.report_step);
    insert_d("START_DATE", opts.start_date);
    insert_d("END_DATE", opts.end_date);
    insert_d("REPORT_START", opts.report_start);
    insert("SWEEP_START", std::to_string(opts.sweep_start));
    insert("SWEEP_END", std::to_string(opts.sweep_end));
    insert("NODE_CONTINUITY", std::to_string(static_cast<int>(opts.node_continuity)));
    insert("ANDERSON_ACCEL", std::to_string(opts.anderson_accel ? 1 : 0));
    insert("SURCHARGE_METHOD", std::to_string(opts.surcharge_method));
    if (opts.surcharge_method == 2) {
        insert_d("DPS_CELERITY", opts.dps_target_celerity);
        insert_d("DPS_ALPHA", opts.dps_alpha);
        insert_d("DPS_DECAY_TIME", opts.dps_decay_time);
    }
    insert("INERTIAL_DAMPING", std::to_string(opts.inertial_damping));
    insert("NORMAL_FLOW_LIMITED", std::to_string(opts.normal_flow_ltd));
    insert("MAX_TRIALS", std::to_string(opts.max_trials));
    insert_d("HEAD_TOLERANCE", opts.head_tol);
    insert_d("VARIABLE_STEP", opts.variable_step);
    insert_d("MINIMUM_STEP", opts.min_routing_step);
    insert_d("LENGTHENING_STEP", opts.lengthening_step);
    insert_d("MIN_SLOPE", opts.min_slope);
    insert_d("MIN_SURFAREA", opts.min_surf_area);
    insert_d("SYS_FLOW_TOL", opts.sys_flow_tol);
    insert_d("LAT_FLOW_TOL", opts.lat_flow_tol);
    insert("THREADS", std::to_string(opts.num_threads));
    insert_d("DRY_DAYS", opts.dry_days);
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
    // Relational node schema: base nodes row + one per-type child row sourced
    // from the node_subtypes side-tables. Child FK -> nodes(simulation_id,
    // node_id); the base row is inserted first each iteration so the FK resolves.
    auto stmt = prepare(db,
        "INSERT INTO nodes (simulation_id, node_id, node_type, geom, "
        "invert_elev, max_depth, init_depth, surcharge_depth, ponded_area, tag) "
        "VALUES (?,?,?,?,?,?,?,?,?,?)");
    auto st_stmt = prepare(db,
        "INSERT INTO storages (simulation_id, node_id, curve_name, a, b, c, "
        "seep_rate, evap_frac, exfil_suction, exfil_ksat, exfil_imd) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?)");
    auto of_stmt = prepare(db,
        "INSERT INTO outfalls (simulation_id, node_id, outfall_type, param, "
        "has_flap_gate, route_to) VALUES (?,?,?,?,?,?)");
    auto dv_stmt = prepare(db,
        "INSERT INTO dividers (simulation_id, node_id, divider_type, cutoff, cd, "
        "max_depth, curve_name, divider_link) VALUES (?,?,?,?,?,?,?,?)");

    auto bind_name = [](sqlite3_stmt* s, int col, const std::string& v) {
        if (!v.empty()) bind_text(s, col, v); else bind_null(s, col);
    };

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

        const auto utag = static_cast<std::size_t>(i);
        if (utag < ctx.nodes.tags.size() && !ctx.nodes.tags[utag].empty())
            bind_text(stmt.get(), 10, ctx.nodes.tags[utag]);
        else
            bind_null(stmt.get(), 10);

        sqlite3_step(stmt.get());

        // --- per-subtype child row ---
        if (ntype == NodeType::STORAGE) {
            const int r = ctx.node_subtypes.storage_row(i);
            const auto& S = ctx.node_subtypes.storages;
            const auto ur = static_cast<size_t>(r);
            sqlite3_reset(st_stmt.get()); sqlite3_clear_bindings(st_stmt.get());
            bind_text(st_stmt.get(), 1, sim_id);
            bind_text(st_stmt.get(), 2, name);
            bind_name(st_stmt.get(), 3, r >= 0 ? S.curve_name[ur] : std::string{});
            bind_double(st_stmt.get(), 4,  r >= 0 ? S.a[ur] : 0.0);
            bind_double(st_stmt.get(), 5,  r >= 0 ? S.b[ur] : 0.0);
            bind_double(st_stmt.get(), 6,  r >= 0 ? S.c[ur] : 0.0);
            bind_double(st_stmt.get(), 7,  r >= 0 ? S.seep_rate[ur] : 0.0);
            bind_double(st_stmt.get(), 8,  r >= 0 ? S.evap_frac[ur] : 0.0);
            bind_double(st_stmt.get(), 9,  r >= 0 ? S.exfil_suction[ur] : 0.0);
            bind_double(st_stmt.get(), 10, r >= 0 ? S.exfil_ksat[ur] : 0.0);
            bind_double(st_stmt.get(), 11, r >= 0 ? S.exfil_imd[ur] : 0.0);
            sqlite3_step(st_stmt.get());
        } else if (ntype == NodeType::OUTFALL) {
            const int r = ctx.node_subtypes.outfall_row(i);
            const auto& O = ctx.node_subtypes.outfalls;
            const auto ur = static_cast<size_t>(r);
            sqlite3_reset(of_stmt.get()); sqlite3_clear_bindings(of_stmt.get());
            bind_text(of_stmt.get(), 1, sim_id);
            bind_text(of_stmt.get(), 2, name);
            bind_text(of_stmt.get(), 3, outfall_type_str(r >= 0 ? O.bc_type[ur] : OutfallType::FREE));
            bind_double(of_stmt.get(), 4, r >= 0 ? O.param[ur] : 0.0);
            bind_int(of_stmt.get(), 5, (r >= 0 && O.has_flap_gate[ur]) ? 1 : 0);
            const int rt = (r >= 0) ? O.route_to[ur] : -1;   // subcatch index -> name
            bind_name(of_stmt.get(), 6,
                      (rt >= 0 && rt < ctx.subcatch_names.size())
                          ? ctx.subcatch_names.name_of(rt) : std::string{});
            sqlite3_step(of_stmt.get());
        } else if (ntype == NodeType::DIVIDER) {
            const int r = ctx.node_subtypes.divider_row(i);
            const auto& D = ctx.node_subtypes.dividers;
            const auto ur = static_cast<size_t>(r);
            sqlite3_reset(dv_stmt.get()); sqlite3_clear_bindings(dv_stmt.get());
            bind_text(dv_stmt.get(), 1, sim_id);
            bind_text(dv_stmt.get(), 2, name);
            bind_text(dv_stmt.get(), 3, divider_type_str(r >= 0 ? D.method[ur] : DividerType::CUTOFF));
            bind_double(dv_stmt.get(), 4, r >= 0 ? D.cutoff[ur] : 0.0);
            bind_double(dv_stmt.get(), 5, r >= 0 ? D.cd[ur] : 0.0);
            bind_double(dv_stmt.get(), 6, r >= 0 ? D.max_depth[ur] : 0.0);
            bind_name(dv_stmt.get(), 7, r >= 0 ? D.curve_name[ur] : std::string{});
            bind_name(dv_stmt.get(), 8, r >= 0 ? D.link_name[ur] : std::string{});
            sqlite3_step(dv_stmt.get());
        }
    }
}

static void write_links(sqlite3* db, const SimulationContext& ctx,
                        const std::string& sim_id, int srs_id) {
    // Unit conversion is handled globally in write_model (convert_internal_to_
    // display on a ctx copy); fields are written here as-is. pump_startup/
    // pump_shutoff are NOT in the convert set (left ctx-native, like the .inp
    // path), so they round-trip verbatim — the previous per-field ×ucf_len was
    // a one-sided conversion the reader never undid (broke SI round-trip).
    // Phase 7: slim base row + one 1:1 child row per link type.
    auto stmt = prepare(db,
        "INSERT INTO links (simulation_id, link_id, link_type, geom, "
        "from_node, to_node, offset1, offset2, q0, q_limit, direction, "
        "xsect_shape, xsect_geom1, xsect_geom2, xsect_geom3, xsect_geom4, xsect_curve, "
        "has_flap_gate, tag) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    auto st_conduit = prepare(db,
        "INSERT INTO conduits (simulation_id, link_id, roughness, length, "
        "xsect_barrels, xsect_culvert, loss_inlet, loss_outlet, loss_avg, seep_rate) "
        "VALUES (?,?,?,?,?,?,?,?,?,?)");
    auto st_pump = prepare(db,
        "INSERT INTO pumps (simulation_id, link_id, pump_curve, init_state, "
        "startup_depth, shutoff_depth) VALUES (?,?,?,?,?,?)");
    auto st_orifice = prepare(db,
        "INSERT INTO orifices (simulation_id, link_id, orientation, discharge_coeff, orate) "
        "VALUES (?,?,?,?,?)");
    auto st_weir = prepare(db,
        "INSERT INTO weirs (simulation_id, link_id, weir_type, discharge_coeff, "
        "crest_height, end_contractions) VALUES (?,?,?,?,?,?)");
    auto st_outlet = prepare(db,
        "INSERT INTO outlets (simulation_id, link_id, rating_type, rating_curve, "
        "q_coeff, q_expon, crest_height) VALUES (?,?,?,?,?,?,?)");

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

        bind_double(stmt.get(), 9,  safe_dbl(ctx.links.q0, i));
        bind_double(stmt.get(), 10, safe_dbl(ctx.links.q_limit, i));
        // Flow direction (+1/-1). Adverse-slope DW conduits are stored already
        // reversed (positive slope), so direction can't be re-derived on read.
        bind_int(stmt.get(), 11, safe_get(ctx.links.direction, (size_t)i, 1));

        // Cross-section (shared by conduit/orifice/weir) — persist the RAW
        // [XSECTIONS] geom1-4 (display units, preserved by the parser), NOT the
        // derived y_full/w_max/a_full/y_bot, which init re-derives identically.
        bind_text(stmt.get(), 12, xsect_shape_str(safe_get(ctx.links.xsect_shape, (size_t)i, XsectShape::CIRCULAR)));
        bind_double(stmt.get(), 13, safe_dbl(ctx.links.xsect_geom1, i));
        bind_double(stmt.get(), 14, safe_dbl(ctx.links.xsect_geom2, i));
        bind_double(stmt.get(), 15, safe_dbl(ctx.links.xsect_geom3, i));
        bind_double(stmt.get(), 16, safe_dbl(ctx.links.xsect_geom4, i));
        // xsect named reference (col 17): IRREGULAR/STREET/CUSTOM reference a
        // transect/street/shape-curve by NAME, kept in pump_curve_name.
        {
            const XsectShape wshp = safe_get(ctx.links.xsect_shape, (size_t)i, XsectShape::CIRCULAR);
            std::string xname;
            if ((wshp == XsectShape::IRREGULAR || wshp == XsectShape::STREET_XSECT ||
                 wshp == XsectShape::CUSTOM) && i < (int)ctx.links.pump_curve_name.size())
                xname = ctx.links.pump_curve_name[i];
            if (!xname.empty()) bind_text(stmt.get(), 17, xname);
            else                bind_null(stmt.get(), 17);
        }
        bind_int(stmt.get(), 18, safe_get(ctx.links.has_flap_gate, (size_t)i, uint8_t{0}) ? 1 : 0);
        const auto utag = static_cast<std::size_t>(i);
        if (utag < ctx.links.tags.size() && !ctx.links.tags[utag].empty())
            bind_text(stmt.get(), 19, ctx.links.tags[utag]);
        else
            bind_null(stmt.get(), 19);
        sqlite3_step(stmt.get());

        // ---- per-type child row, sourced from the relational side-tables ----
        const int cr  = ctx.link_subtypes.conduit_row(i);
        const int pr  = ctx.link_subtypes.pump_row(i);
        const int orr = ctx.link_subtypes.orifice_row(i);
        const int wr  = ctx.link_subtypes.weir_row(i);
        const int olr = ctx.link_subtypes.outlet_row(i);
        if (cr >= 0) {
            const auto& CD = ctx.link_subtypes.conduits; const auto u = (size_t)cr;
            sqlite3_reset(st_conduit.get()); sqlite3_clear_bindings(st_conduit.get());
            bind_text(st_conduit.get(), 1, sim_id); bind_text(st_conduit.get(), 2, name);
            bind_double(st_conduit.get(), 3, CD.roughness[u]);
            bind_double(st_conduit.get(), 4, CD.length[u]);
            bind_int(st_conduit.get(), 5, CD.barrels[u]);
            bind_int(st_conduit.get(), 6, CD.culvert_code[u]);
            bind_double(st_conduit.get(), 7, CD.loss_inlet[u]);
            bind_double(st_conduit.get(), 8, CD.loss_outlet[u]);
            bind_double(st_conduit.get(), 9, CD.loss_avg[u]);
            bind_double(st_conduit.get(), 10, CD.seep_rate[u]);
            sqlite3_step(st_conduit.get());
        } else if (pr >= 0) {
            const auto& PD = ctx.link_subtypes.pumps; const auto u = (size_t)pr;
            sqlite3_reset(st_pump.get()); sqlite3_clear_bindings(st_pump.get());
            bind_text(st_pump.get(), 1, sim_id); bind_text(st_pump.get(), 2, name);
            std::string pcname = i < (int)ctx.links.pump_curve_name.size() ? ctx.links.pump_curve_name[i] : "";
            if (!pcname.empty()) bind_text(st_pump.get(), 3, pcname); else bind_null(st_pump.get(), 3);
            bind_double(st_pump.get(), 4, PD.init_state[u] ? 1.0 : 0.0);
            bind_double(st_pump.get(), 5, PD.startup[u]);
            bind_double(st_pump.get(), 6, PD.shutoff[u]);
            sqlite3_step(st_pump.get());
        } else if (orr >= 0) {
            const auto& ORF = ctx.link_subtypes.orifices; const auto u = (size_t)orr;
            sqlite3_reset(st_orifice.get()); sqlite3_clear_bindings(st_orifice.get());
            bind_text(st_orifice.get(), 1, sim_id); bind_text(st_orifice.get(), 2, name);
            bind_text(st_orifice.get(), 3, orifice_orientation_str(ORF.orifice_type[u]));
            bind_double(st_orifice.get(), 4, ORF.cd[u]);
            bind_double(st_orifice.get(), 5, ORF.orate[u]);
            sqlite3_step(st_orifice.get());
        } else if (wr >= 0) {
            const auto& WD = ctx.link_subtypes.weirs; const auto u = (size_t)wr;
            sqlite3_reset(st_weir.get()); sqlite3_clear_bindings(st_weir.get());
            bind_text(st_weir.get(), 1, sim_id); bind_text(st_weir.get(), 2, name);
            bind_text(st_weir.get(), 3, weir_type_str(WD.weir_type[u]));
            bind_double(st_weir.get(), 4, WD.cd[u]);
            bind_double(st_weir.get(), 5, WD.crest_height[u]);
            bind_int(st_weir.get(), 6, static_cast<int>(WD.end_contractions[u] + 0.5));
            sqlite3_step(st_weir.get());
        } else if (olr >= 0) {
            const auto& OUT = ctx.link_subtypes.outlets; const auto u = (size_t)olr;
            sqlite3_reset(st_outlet.get()); sqlite3_clear_bindings(st_outlet.get());
            bind_text(st_outlet.get(), 1, sim_id); bind_text(st_outlet.get(), 2, name);
            bind_text(st_outlet.get(), 3, outlet_rating_str(OUT.outlet_type[u]));
            // TABULAR rating curve NAME is kept in pump_curve_name (base).
            std::string rname = (static_cast<int>(OUT.outlet_type[u] + 0.5) >= 2 &&
                                 i < (int)ctx.links.pump_curve_name.size())
                                ? ctx.links.pump_curve_name[i] : "";
            if (!rname.empty()) bind_text(st_outlet.get(), 4, rname); else bind_null(st_outlet.get(), 4);
            bind_double(st_outlet.get(), 5, OUT.coeff[u]);
            bind_double(st_outlet.get(), 6, OUT.expon[u]);
            bind_double(st_outlet.get(), 7, OUT.crest_height[u]);
            sqlite3_step(st_outlet.get());
        }
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

        const auto utag = static_cast<std::size_t>(i);
        if (utag < ctx.subcatches.tags.size() && !ctx.subcatches.tags[utag].empty())
            bind_text(stmt.get(), 25, ctx.subcatches.tags[utag]);
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
            bind_text(stmt.get(), 3, format_ts_timestamp(tbl.x[j], ctx.options.start_date));
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

    // RDII exponential-decay parameters
    if (ctx.rdii_decay.count() > 0) {
        auto stmt = prepare(db,
            "INSERT INTO rdii_decay (simulation_id, uh_name, response, "
            "k_dep, k_0, k_T, T_ref, theta_rec, T_freeze) "
            "VALUES (?,?,?,?,?,?,?,?,?)");
        static const char* responses[] = {"SHORT","MEDIUM","LONG"};
        for (const auto& e : ctx.rdii_decay.entries) {
            if (e.response < 0 || e.response > 2) continue;
            sqlite3_reset(stmt.get());
            sqlite3_clear_bindings(stmt.get());
            bind_text(stmt.get(), 1, sim_id);
            bind_text(stmt.get(), 2, e.uh_name);
            bind_text(stmt.get(), 3, responses[e.response]);
            sqlite3_bind_double(stmt.get(), 4, e.k_dep);
            sqlite3_bind_double(stmt.get(), 5, e.k_0);
            sqlite3_bind_double(stmt.get(), 6, e.k_T);
            sqlite3_bind_double(stmt.get(), 7, e.T_ref);
            sqlite3_bind_double(stmt.get(), 8, e.theta_rec);
            sqlite3_bind_double(stmt.get(), 9, e.T_freeze);
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

static void write_inflows(sqlite3* db, const SimulationContext& ctx,
                          const std::string& sim_id) {
    const auto& E = ctx.ext_inflows;
    if (E.count() == 0) return;
    auto stmt = prepare(db,
        "INSERT INTO inflows (simulation_id, node_id, constituent, timeseries, "
        "inflow_type, m_factor, s_factor, baseline, pattern) "
        "VALUES (?,?,?,?,?,?,?,?,?)");
    for (int j = 0; j < E.count(); ++j) {
        const auto u = static_cast<std::size_t>(j);
        const int ni = E.node_idx[u];
        if (ni < 0 || ni >= ctx.node_names.size()) continue;
        sqlite3_reset(stmt.get());
        sqlite3_clear_bindings(stmt.get());
        bind_text(stmt.get(), 1, sim_id);
        bind_text(stmt.get(), 2, ctx.node_names.name_of(ni));
        bind_text(stmt.get(), 3, E.constituent[u]);
        if (!E.ts_name[u].empty()) bind_text(stmt.get(), 4, E.ts_name[u]);
        else                       bind_null(stmt.get(), 4);
        bind_text(stmt.get(), 5, E.inflow_type[u]);
        bind_double(stmt.get(), 6, E.m_factor[u]);
        bind_double(stmt.get(), 7, E.s_factor[u]);
        bind_double(stmt.get(), 8, E.baseline[u]);
        if (!E.pattern_name[u].empty()) bind_text(stmt.get(), 9, E.pattern_name[u]);
        else                            bind_null(stmt.get(), 9);
        sqlite3_step(stmt.get());
    }
}

static void write_dwf(sqlite3* db, const SimulationContext& ctx,
                      const std::string& sim_id) {
    const auto& D = ctx.dwf_inflows;
    if (D.count() == 0) return;
    auto stmt = prepare(db,
        "INSERT INTO dwf_inflows (simulation_id, node_id, constituent, avg_value, "
        "pat1, pat2, pat3, pat4) VALUES (?,?,?,?,?,?,?,?)");
    auto bind_pat = [&](int col, const std::string& p) {
        if (!p.empty()) bind_text(stmt.get(), col, p); else bind_null(stmt.get(), col);
    };
    for (int j = 0; j < D.count(); ++j) {
        const auto u = static_cast<std::size_t>(j);
        const int ni = D.node_idx[u];
        if (ni < 0 || ni >= ctx.node_names.size()) continue;
        sqlite3_reset(stmt.get());
        sqlite3_clear_bindings(stmt.get());
        bind_text(stmt.get(), 1, sim_id);
        bind_text(stmt.get(), 2, ctx.node_names.name_of(ni));
        bind_text(stmt.get(), 3, D.constituent[u]);
        bind_double(stmt.get(), 4, D.avg_value[u]);
        bind_pat(5, D.pat1[u]); bind_pat(6, D.pat2[u]);
        bind_pat(7, D.pat3[u]); bind_pat(8, D.pat4[u]);
        sqlite3_step(stmt.get());
    }
}

static void write_transects(sqlite3* db, const SimulationContext& ctx,
                            const std::string& sim_id) {
    const auto& T = ctx.transects;
    if (T.count() == 0) return;
    auto stmt = prepare(db,
        "INSERT INTO transects (simulation_id, transect_id, ordinal, station, elevation, "
        "n_left, n_right, n_channel, x_left_bank, x_right_bank, "
        "x_left_encroach, x_right_encroach, x_factor, y_factor, length_factor, comment) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    for (int t = 0; t < T.count(); ++t) {
        const auto ut = static_cast<std::size_t>(t);
        const auto& xs = T.stations[ut];
        const auto& ys = T.elevations[ut];
        const int np = static_cast<int>(xs.size());
        for (int p = 0; p < np; ++p) {
            sqlite3_reset(stmt.get());
            sqlite3_clear_bindings(stmt.get());
            bind_text(stmt.get(), 1, sim_id);
            bind_text(stmt.get(), 2, T.names[ut]);
            bind_int(stmt.get(), 3, p);
            bind_double(stmt.get(), 4, xs[static_cast<std::size_t>(p)]);
            bind_double(stmt.get(), 5, ys[static_cast<std::size_t>(p)]);
            bind_double(stmt.get(), 6, T.n_left[ut]);
            bind_double(stmt.get(), 7, T.n_right[ut]);
            bind_double(stmt.get(), 8, T.n_channel[ut]);
            bind_double(stmt.get(), 9, T.x_left_bank[ut]);
            bind_double(stmt.get(), 10, T.x_right_bank[ut]);
            bind_double(stmt.get(), 11, T.x_left_encroachment[ut]);
            bind_double(stmt.get(), 12, T.x_right_encroachment[ut]);
            bind_double(stmt.get(), 13, T.x_factor[ut]);
            bind_double(stmt.get(), 14, T.y_factor[ut]);
            bind_double(stmt.get(), 15, T.length_factor[ut]);
            if (!T.comments[ut].empty()) bind_text(stmt.get(), 16, T.comments[ut]);
            else                         bind_null(stmt.get(), 16);
            sqlite3_step(stmt.get());
        }
    }
}

static void write_controls(sqlite3* db, const SimulationContext& ctx,
                           const std::string& sim_id) {
    const auto& C = ctx.control_rules;
    if (C.count() == 0) return;
    auto stmt = prepare(db,
        "INSERT INTO control_rules (simulation_id, ordinal, rule_text) VALUES (?,?,?)");
    for (int i = 0; i < C.count(); ++i) {
        sqlite3_reset(stmt.get());
        sqlite3_clear_bindings(stmt.get());
        bind_text(stmt.get(), 1, sim_id);
        bind_int(stmt.get(), 2, i);
        bind_text(stmt.get(), 3, C.rule_text[static_cast<std::size_t>(i)]);
        sqlite3_step(stmt.get());
    }
}

// ============================================================================
// Part E — 2D surface-routing mesh + solver options
//
// Persists the 2D MODEL DEFINITION only. 2D simulation results always go to
// the CF/UGRID HDF5 file referenced by the 2D_OUTPUT_FILE option key — they
// are deliberately never written to GeoPackage tables (performance).
//
// Sources are reached through ctx.twod_io (non-owning pointers wired by
// SWMMEngine); all of it is null in engine builds without 2D support, so
// every entry point runtime-guards and degrades to a no-op.
// ============================================================================

namespace {

// Lossless double → text for the options key-value table (std::to_string
// fixes 6 decimals and would destroy 1e-12-scale tolerances).
std::string fmt_g17(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return buf;
}

// sqlite3_step that surfaces failures (FK violations, CHECK violations,
// disk errors) as exceptions so the surrounding Transaction rolls the
// whole model write back.
void step_or_throw(sqlite3* db, sqlite3_stmt* stmt, const char* what) {
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        // Snapshot the message, then reset the aborted statement BEFORE we
        // unwind. A statement left un-reset after a step error (e.g. an FK
        // SQLITE_CONSTRAINT) keeps its implicit statement-journal lock, which
        // makes the enclosing Transaction guard's ROLLBACK fail with
        // SQLITE_BUSY ("statements in progress"). On platforms with mandatory
        // file locking (Windows) that leaves the transaction open — the same
        // connection then reads its own uncommitted rows and the .gpkg file
        // stays locked for the next writer. Resetting here releases the lock
        // deterministically rather than relying on finalize-during-unwind.
        std::string msg = std::string(what) + ": " + sqlite3_errmsg(db);
        sqlite3_reset(stmt);
        throw GpkgError(std::move(msg), rc);
    }
}

// [2D_BOUNDARY_CONDITIONS] grammar token for a pending row. The TS_*
// spellings are emitted when the parameter is a timeseries name so the
// type round-trips without inspecting param1/ref_name.
const char* bc_type_token(const twoD::PendingBoundaryRow& r) {
    switch (static_cast<twoD::BoundaryType>(r.bc_type)) {
        case twoD::BoundaryType::WALL:            return "WALL";
        case twoD::BoundaryType::NORMAL_FLOW:     return "NORMAL_FLOW";
        case twoD::BoundaryType::SPECIFIED_STAGE:
            return r.name.empty() ? "SPECIFIED_STAGE" : "TS_STAGE";
        case twoD::BoundaryType::SPECIFIED_FLOW:
            return r.name.empty() ? "SPECIFIED_FLOW" : "TS_FLOW";
        case twoD::BoundaryType::RATING_CURVE:    return "RATING_CURVE";
    }
    return "WALL";
}

const char* linear_solver_token(twoD::LinearSolverType t) {
    switch (t) {
        case twoD::LinearSolverType::GMRES:    return "GMRES";
        case twoD::LinearSolverType::BICGSTAB: return "BICGSTAB";
        case twoD::LinearSolverType::TFQMR:    return "TFQMR";
    }
    return "GMRES";
}

const char* preconditioner_token(twoD::PreconditionerType t) {
    switch (t) {
        case twoD::PreconditionerType::NONE:   return "NONE";
        case twoD::PreconditionerType::JACOBI: return "JACOBI";
        case twoD::PreconditionerType::ILU:    return "ILU";
        case twoD::PreconditionerType::AMG:    return "AMG";
    }
    return "JACOBI";
}

// True when a 2D mesh worth persisting is present (mirrors the
// SurfaceRouter2D::initialize() activity threshold).
bool has_mesh_2d(const SimulationContext& ctx) {
    const auto* mesh = ctx.twod_io.mesh;
    return mesh && mesh->n_vertices() >= 3 && mesh->n_triangles() >= 1;
}

} // anonymous namespace

// SolverOptions2D → options key-value rows. Keys are "2D_" + the exact
// [2D_OPTIONS] token; values use the same string tokens the inp parser
// accepts (parse2DOptionsLine), so the vocabularies cannot drift apart
// silently. 2D_MESH_UNITS_SI and 2D_MESH_FILE_SOURCE are gpkg-only keys.
static void write_options_2d(sqlite3* db, const SimulationContext& ctx,
                             const std::string& sim_id) {
    if (!ctx.twod_io.options || !has_mesh_2d(ctx)) return;
    const auto& o = *ctx.twod_io.options;

    auto stmt = prepare(db,
        "INSERT INTO options (simulation_id, key, value) VALUES (?, ?, ?)");
    auto insert = [&](const char* key, const std::string& val) {
        sqlite3_reset(stmt.get());
        sqlite3_clear_bindings(stmt.get());
        bind_text(stmt.get(), 1, sim_id);
        bind_text(stmt.get(), 2, key);
        bind_text(stmt.get(), 3, val);
        step_or_throw(db, stmt.get(), "options(2D) insert failed");
    };

    insert("2D_MAX_TIMESTEP",      fmt_g17(o.max_timestep));
    insert("2D_MIN_TIMESTEP",      fmt_g17(o.min_timestep));
    insert("2D_REL_TOLERANCE",     fmt_g17(o.rel_tolerance));
    insert("2D_ABS_TOLERANCE",     fmt_g17(o.abs_tolerance));
    insert("2D_DRY_DEPTH",         fmt_g17(o.dry_depth));
    insert("2D_LIMITER_EPSILON",   fmt_g17(o.limiter_epsilon));
    insert("2D_COUPLING_CD",       fmt_g17(o.coupling_cd));
    insert("2D_MAX_KRYLOV_DIM",    std::to_string(o.max_krylov_dim));
    insert("2D_COUPLING_INTERVAL", std::to_string(o.coupling_interval));
    insert("2D_MAX_CVODE_STEPS",   std::to_string(o.max_cvode_steps));
    insert("2D_LINEAR_SOLVER",     linear_solver_token(o.linear_solver));
    insert("2D_PRECONDITIONER",    preconditioner_token(o.preconditioner));
    insert("2D_REPORT_2D",         o.report_2d ? "YES" : "NO");
    // HDF5 results path — 2D outputs always go to HDF5, never gpkg tables.
    // Restored to SolverOptions2D::output_file on read so SWMMEngine::open
    // re-creates the Default2DOutputPlugin.
    if (!o.output_file.empty())
        insert("2D_OUTPUT_FILE", o.output_file);
    // Authored units flag (replaces the `;; UNITS: SI (m)` text header).
    insert("2D_MESH_UNITS_SI", o.mesh_units_si ? "YES" : "NO");
    // Provenance only. NEVER restored into SolverOptions2D::mesh_file — the
    // mesh lives in the gpkg tables, and restoring the path would make
    // SWMMEngine::open attempt a second external-file load.
    if (!o.mesh_file.empty())
        insert("2D_MESH_FILE_SOURCE", o.mesh_file);
}

// Mesh geometry, topology, BCs, conveyance, and 1D-2D coupling.
// Coordinates are un-scaled back to the AUTHORED project units when
// initialize() already converted the mesh to SI (mesh_scaled_to_si), so the
// feature layers stay aligned with nodes/links and the round-trip is exact
// for pre-initialize saves.
static void write_mesh_2d(sqlite3* db, const SimulationContext& ctx,
                          const std::string& sim_id, int srs_id) {
    if (!has_mesh_2d(ctx)) return;
    const auto& mesh = *ctx.twod_io.mesh;
    const auto* opts = ctx.twod_io.options;

    const double f = (opts && opts->mesh_scaled_to_si && opts->len_2d_to_1d > 0.0)
                         ? opts->len_2d_to_1d : 1.0;
    const double f2 = f * f;

    const int nv = mesh.n_vertices();
    const int nt = mesh.n_triangles();

    // Layer bbox from actual (authored-unit) vertex extents.
    double min_x = mesh.vx[0] * f, max_x = min_x;
    double min_y = mesh.vy[0] * f, max_y = min_y;
    for (int i = 1; i < nv; ++i) {
        const double x = mesh.vx[i] * f, y = mesh.vy[i] * f;
        min_x = std::min(min_x, x); max_x = std::max(max_x, x);
        min_y = std::min(min_y, y); max_y = std::max(max_y, y);
    }
    register_feature_table(db, "mesh_2d_vertices", "POINT", srs_id,
        "2D Mesh Vertices", "2D surface-routing mesh vertices",
        min_x, min_y, max_x, max_y);
    register_feature_table(db, "mesh_2d_triangles", "POLYGON", srs_id,
        "2D Mesh Triangles", "2D surface-routing mesh cells",
        min_x, min_y, max_x, max_y);

    // ---- vertices --------------------------------------------------------
    {
        auto stmt = prepare(db,
            "INSERT INTO mesh_2d_vertices "
            "(simulation_id, vertex_idx, geom, x, y, z, tag) "
            "VALUES (?,?,?,?,?,?,?)");
        for (int i = 0; i < nv; ++i) {
            sqlite3_reset(stmt.get());
            sqlite3_clear_bindings(stmt.get());
            const double x = mesh.vx[i] * f;
            const double y = mesh.vy[i] * f;
            bind_text(stmt.get(), 1, sim_id);
            bind_int(stmt.get(), 2, i);
            auto geom = encode_point(x, y, srs_id);
            bind_blob(stmt.get(), 3, geom.data(), static_cast<int>(geom.size()));
            bind_double(stmt.get(), 4, x);
            bind_double(stmt.get(), 5, y);
            bind_double(stmt.get(), 6, mesh.vz[i] * f);
            if (!mesh.vtag[i].empty()) bind_text(stmt.get(), 7, mesh.vtag[i]);
            else                       bind_null(stmt.get(), 7);
            step_or_throw(db, stmt.get(), "mesh_2d_vertices insert failed");
        }
    }

    // Coupled-node name for a triangle/vertex: prefer the authored name,
    // fall back to the resolved index (API-built models).
    auto node_name_for = [&ctx](const std::string& name, int idx) -> std::string {
        if (!name.empty()) return name;
        if (idx >= 0 && idx < ctx.node_names.size())
            return ctx.node_names.name_of(idx);
        return {};
    };

    // ---- triangles -------------------------------------------------------
    {
        auto stmt = prepare(db,
            "INSERT INTO mesh_2d_triangles "
            "(simulation_id, tri_idx, geom, v0, v1, v2, mannings_n, tag, "
            "bed_elev, coupled_node) "
            "VALUES (?,?,?,?,?,?,?,?,?,?)");
        std::vector<double> xs(3), ys(3);
        for (int t = 0; t < nt; ++t) {
            sqlite3_reset(stmt.get());
            sqlite3_clear_bindings(stmt.get());
            const int v[3] = { mesh.tri_v0[t], mesh.tri_v1[t], mesh.tri_v2[t] };
            bind_text(stmt.get(), 1, sim_id);
            bind_int(stmt.get(), 2, t);
            if (v[0] >= 0 && v[0] < nv && v[1] >= 0 && v[1] < nv &&
                v[2] >= 0 && v[2] < nv) {
                for (int k = 0; k < 3; ++k) {
                    xs[static_cast<size_t>(k)] = mesh.vx[v[k]] * f;
                    ys[static_cast<size_t>(k)] = mesh.vy[v[k]] * f;
                }
                auto geom = encode_polygon(xs, ys, srs_id);
                bind_blob(stmt.get(), 3, geom.data(), static_cast<int>(geom.size()));
                bind_double(stmt.get(), 9,
                    (mesh.vz[v[0]] + mesh.vz[v[1]] + mesh.vz[v[2]]) / 3.0 * f);
            } else {
                bind_null(stmt.get(), 3);
                bind_null(stmt.get(), 9);
            }
            bind_int(stmt.get(), 4, v[0]);
            bind_int(stmt.get(), 5, v[1]);
            bind_int(stmt.get(), 6, v[2]);
            bind_double(stmt.get(), 7, mesh.mannings_n[t]);
            if (!mesh.tri_tag[t].empty()) bind_text(stmt.get(), 8, mesh.tri_tag[t]);
            else                          bind_null(stmt.get(), 8);
            const std::string cn = node_name_for(mesh.tri_coupled_node_name[t],
                                                 mesh.tri_coupled_node[t]);
            if (!cn.empty()) bind_text(stmt.get(), 10, cn);
            else             bind_null(stmt.get(), 10);
            step_or_throw(db, stmt.get(), "mesh_2d_triangles insert failed");
        }
    }

    // ---- coupling maps ----------------------------------------------------
    {
        auto stmt = prepare(db,
            "INSERT INTO mesh_2d_vertex_coupling "
            "(simulation_id, vertex_idx, node_id, coupling_cd, coupling_area) "
            "VALUES (?,?,?,?,?)");
        for (int i = 0; i < nv; ++i) {
            const std::string cn = node_name_for(mesh.vert_coupled_node_name[i],
                                                 mesh.vert_coupled_node[i]);
            if (cn.empty()) continue;
            sqlite3_reset(stmt.get());
            sqlite3_clear_bindings(stmt.get());
            bind_text(stmt.get(), 1, sim_id);
            bind_int(stmt.get(), 2, i);
            bind_text(stmt.get(), 3, cn);
            bind_double(stmt.get(), 4, mesh.vert_coupling_cd[i]);
            bind_double(stmt.get(), 5, mesh.vert_coupling_area[i] * f2);
            step_or_throw(db, stmt.get(), "mesh_2d_vertex_coupling insert failed");
        }
    }
    {
        auto stmt = prepare(db,
            "INSERT INTO mesh_2d_triangle_coupling "
            "(simulation_id, tri_idx, node_id, coupling_cd, coupling_area) "
            "VALUES (?,?,?,?,?)");
        for (int t = 0; t < nt; ++t) {
            const std::string cn = node_name_for(mesh.tri_coupled_node_name[t],
                                                 mesh.tri_coupled_node[t]);
            if (cn.empty()) continue;
            sqlite3_reset(stmt.get());
            sqlite3_clear_bindings(stmt.get());
            bind_text(stmt.get(), 1, sim_id);
            bind_int(stmt.get(), 2, t);
            bind_text(stmt.get(), 3, cn);
            bind_double(stmt.get(), 4, mesh.tri_coupling_cd[t]);
            bind_double(stmt.get(), 5, mesh.tri_coupling_area[t] * f2);
            step_or_throw(db, stmt.get(), "mesh_2d_triangle_coupling insert failed");
        }
    }

    // ---- boundary conditions ----------------------------------------------
    // Authored pending rows preferred; BoundaryData reconstruction fallback
    // (loses the group label). INSERT OR REPLACE: duplicate authored rows for
    // the same (tri, edge) are last-write-wins, matching the drain semantics.
    {
        const auto rows = twoD::collectBCRows(
            ctx.twod_io.pending_bc, ctx.twod_io.boundary,
            opts && opts->pending_rows_drained);
        if (!rows.empty()) {
            auto stmt = prepare(db,
                "INSERT OR REPLACE INTO mesh_2d_boundary_conditions "
                "(simulation_id, tri_idx, edge, bc_type, param1, ref_name, bc_group) "
                "VALUES (?,?,?,?,?,?,?)");
            for (const auto& r : rows) {
                // Defensive: rows the initialize() drain would silently skip
                // must not abort the save via an FK violation.
                if (r.tri < 0 || r.tri >= nt || r.edge < 0 || r.edge > 2) continue;
                sqlite3_reset(stmt.get());
                sqlite3_clear_bindings(stmt.get());
                bind_text(stmt.get(), 1, sim_id);
                bind_int(stmt.get(), 2, r.tri);
                bind_int(stmt.get(), 3, r.edge);
                bind_text(stmt.get(), 4, bc_type_token(r));
                if (r.name.empty()) bind_double(stmt.get(), 5, r.param1);
                else                bind_null(stmt.get(), 5);
                if (!r.name.empty()) bind_text(stmt.get(), 6, r.name);
                else                 bind_null(stmt.get(), 6);
                if (!r.group.empty()) bind_text(stmt.get(), 7, r.group);
                else                  bind_null(stmt.get(), 7);
                step_or_throw(db, stmt.get(),
                              "mesh_2d_boundary_conditions insert failed");
            }
        }
    }

    // ---- edge conveyance ----------------------------------------------------
    {
        const auto rows = twoD::collectConveyanceRows(
            ctx.twod_io.pending_ec, &mesh,
            opts && opts->pending_rows_drained);
        if (!rows.empty()) {
            auto stmt = prepare(db,
                "INSERT OR REPLACE INTO mesh_2d_edge_conveyance "
                "(simulation_id, v_from, v_to, conveyance) VALUES (?,?,?,?)");
            for (const auto& r : rows) {
                if (r.v_from < 0 || r.v_from >= nv ||
                    r.v_to   < 0 || r.v_to   >= nv || r.v_from == r.v_to)
                    continue;
                sqlite3_reset(stmt.get());
                sqlite3_clear_bindings(stmt.get());
                bind_text(stmt.get(), 1, sim_id);
                bind_int(stmt.get(), 2, std::min(r.v_from, r.v_to));
                bind_int(stmt.get(), 3, std::max(r.v_from, r.v_to));
                bind_double(stmt.get(), 4, r.conveyance);
                step_or_throw(db, stmt.get(),
                              "mesh_2d_edge_conveyance insert failed");
            }
        }
    }
}

// ============================================================================
// Public API
// ============================================================================

void write_model(sqlite3* db, const SimulationContext& ctx,
                 const std::string& simulation_id, int srs_id) {
    // The .gpkg is the engine's CANONICAL store: values are persisted in
    // internal units (feet/cfs/ft³) exactly as the engine holds them after
    // open, NO display conversion. The read path sets ctx.gpkg_units_internal
    // and resolve_cross_references then SKIPS convert_inputs_to_internal, so the
    // round-trip is bit-for-bit identical for every unit system (a display-unit
    // store would lose ULPs to the non-invertible ×0.3048 / ×(1/0.3048) on
    // metric models and also re-trigger the GUI metric-save ×3.2808 inflation).
    // The ONLY display-unit fields written are the link cross-section RAW
    // geom1-4 (ctx-native display, like the .inp); the reader converts those.

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
    write_inflows(db, ctx, simulation_id);
    write_dwf(db, ctx, simulation_id);
    write_transects(db, ctx, simulation_id);
    write_controls(db, ctx, simulation_id);

    // Part E — 2D mesh model definition + solver options. No-ops when the
    // engine has no 2D module (ctx.twod_io pointers null) or no mesh is
    // loaded. Runs after write_nodes so the coupling-table FKs to
    // nodes(simulation_id, node_id) can resolve. 2D RESULTS are not written
    // here — they always stream to the HDF5 file named by 2D_OUTPUT_FILE.
    write_options_2d(db, ctx, simulation_id);
    write_mesh_2d(db, ctx, simulation_id, srs_id);

    // Slice IO-7 — fan out every external-file reference (timeseries
    // FILE, raingage FILE, climate FILE, [FILES] routing/hotstart) into
    // the Part D content tables created in IO-5. Runs inside the same
    // transaction so a Part D parse failure rolls back the whole save.
    write_external_content(db, ctx, simulation_id);

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
