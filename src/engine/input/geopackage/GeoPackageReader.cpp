/**
 * @file GeoPackageReader.cpp
 * @brief Reads a GeoPackage file into a SimulationContext (all SWMM input sections).
 * @ingroup engine_geopackage
 */

#include "GeoPackageReader.hpp"
#include "ExternalContentReader.hpp"
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
#include "data/HydrologyData.hpp"

// 2D model definition (plain define-free data structs; reached at runtime
// through ctx.twod_io — null in non-2D engine builds).
#include "2d/data/MeshData.hpp"
#include "2d/data/SolverOptions2D.hpp"
#include "2d/data/BoundaryData.hpp"
#include "2d/data/PendingRows2D.hpp"

#include "core/DateTime.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <sstream>

namespace openswmm::gpkg {

namespace {
// Parse a timestamp string written by GeoPackageWriter back to an OADate.
// Accepts "MM/DD/YYYY HH:MM" (absolute) or decimal-hours string (relative).
static double parse_ts_timestamp(const std::string& s) {
    // The writer stores the raw OADate at full precision (%.17g). A "/" date
    // string is decoded defensively (legacy / hand-edited files), but the
    // canonical form is a bare double returned verbatim — bit-exact round-trip.
    if (s.find('/') != std::string::npos) {
        int mo = 0, d = 0, y = 0, h = 0, mi = 0;
        std::sscanf(s.c_str(), "%d/%d/%d %d:%d", &mo, &d, &y, &h, &mi);
        return datetime::encodeDate(y, mo, d) + datetime::encodeTime(h, mi, 0);
    }
    return std::stod(s);
}
} // anonymous namespace

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

// Apply one "2D_*" option key from the options table to SolverOptions2D.
// Keys/values mirror write_options_2d in GeoPackageWriter.cpp and use the
// same value tokens as the [2D_OPTIONS] inp parser (parse2DOptionsLine) —
// implemented locally because that parser TU only exists in 2D builds.
// Silently ignored when the engine has no 2D module (ctx.twod_io.options
// is null) or for unknown future keys.
static void apply_option_2d(SimulationContext& ctx, const std::string& key,
                            const std::string& val) {
    auto* o = ctx.twod_io.options;
    if (!o) return;

    if      (key == "2D_MAX_TIMESTEP")      o->max_timestep      = std::stod(val);
    else if (key == "2D_MIN_TIMESTEP")      o->min_timestep      = std::stod(val);
    else if (key == "2D_REL_TOLERANCE")     o->rel_tolerance     = std::stod(val);
    else if (key == "2D_ABS_TOLERANCE")     o->abs_tolerance     = std::stod(val);
    else if (key == "2D_DRY_DEPTH")         o->dry_depth         = std::stod(val);
    else if (key == "2D_LIMITER_EPSILON")   o->limiter_epsilon   = std::stod(val);
    else if (key == "2D_COUPLING_CD")       o->coupling_cd       = std::stod(val);
    else if (key == "2D_MAX_KRYLOV_DIM")    o->max_krylov_dim    = std::stoi(val);
    else if (key == "2D_COUPLING_INTERVAL") o->coupling_interval = std::stoi(val);
    else if (key == "2D_COUPLING_WINDOW")   o->coupling_window   = std::stod(val);
    else if (key == "2D_MAX_CVODE_STEPS")   o->max_cvode_steps   = std::stoi(val);
    else if (key == "2D_LINEAR_SOLVER") {
        if      (val == "GMRES")    o->linear_solver = twoD::LinearSolverType::GMRES;
        else if (val == "BICGSTAB") o->linear_solver = twoD::LinearSolverType::BICGSTAB;
        else if (val == "TFQMR")    o->linear_solver = twoD::LinearSolverType::TFQMR;
    }
    else if (key == "2D_PRECONDITIONER") {
        if      (val == "NONE")   o->preconditioner = twoD::PreconditionerType::NONE;
        else if (val == "JACOBI") o->preconditioner = twoD::PreconditionerType::JACOBI;
        else if (val == "ILU")    o->preconditioner = twoD::PreconditionerType::ILU;
        else if (val == "AMG")    o->preconditioner = twoD::PreconditionerType::AMG;
    }
    else if (key == "2D_RAINFALL_MODE") {
        if      (val == "NATURAL_NEIGHBOUR") o->rainfall_mode = twoD::RainfallMode::NATURAL_NEIGHBOUR;
        else if (val == "SYSTEM")            o->rainfall_mode = twoD::RainfallMode::SYSTEM;
        else if (val == "NONE")              o->rainfall_mode = twoD::RainfallMode::NONE;
    }
    else if (key == "2D_REPORT_2D")     o->report_2d = (val == "YES");
    // HDF5 results path — restoring it lets SWMMEngine::open re-create the
    // Default2DOutputPlugin (2D results always stream to HDF5, never gpkg).
    else if (key == "2D_OUTPUT_FILE")   o->output_file = val;
    else if (key == "2D_MESH_UNITS_SI") o->mesh_units_si = (val == "YES");
    // 2D_MESH_FILE_SOURCE is provenance only — intentionally NOT restored
    // into SolverOptions2D::mesh_file, otherwise SWMMEngine::open would
    // attempt a second external-file mesh load on top of the gpkg mesh.
}

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
        else if (key == "MAP_UNITS") ctx.spatial.map_units = val;
        else if (key == "MAP_X1") ctx.spatial.map_x1 = std::stod(val);
        else if (key == "MAP_Y1") ctx.spatial.map_y1 = std::stod(val);
        else if (key == "MAP_X2") ctx.spatial.map_x2 = std::stod(val);
        else if (key == "MAP_Y2") ctx.spatial.map_y2 = std::stod(val);
        else if (key.rfind("2D_", 0) == 0) apply_option_2d(ctx, key, val);
    }
}

static bool table_exists(sqlite3* db, const std::string& name);  // defined below

static void read_nodes(sqlite3* db, SimulationContext& ctx, const std::string& sim_id) {
    // --- base nodes (common columns + discriminator + geometry) ---
    {
        auto stmt = prepare(db,
            "SELECT node_id, node_type, geom, invert_elev, max_depth, init_depth, "
            "surcharge_depth, ponded_area, tag "
            "FROM nodes WHERE simulation_id = ? ORDER BY fid");
        bind_text(stmt.get(), 1, sim_id);
        while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
            std::string name = column_text(stmt.get(), 0);
            int idx = ctx.node_names.find(name);
            if (idx < 0) idx = ctx.node_names.add(name);
            ensure_node_capacity(ctx, idx);

            ctx.node_subtypes.set_node_type(ctx.nodes, idx, parse_node_type(column_text(stmt.get(), 1)));

            if (!column_is_null(stmt.get(), 2)) {
                auto pt = decode_point(column_blob(stmt.get(), 2));
                ctx.spatial.node_x[idx] = pt.x;
                ctx.spatial.node_y[idx] = pt.y;
            }
            ctx.nodes.invert_elev[idx] = column_double(stmt.get(), 3);
            ctx.nodes.full_depth[idx]  = column_double(stmt.get(), 4);
            ctx.nodes.init_depth[idx]  = column_double(stmt.get(), 5);
            ctx.nodes.sur_depth[idx]   = column_double(stmt.get(), 6);
            ctx.nodes.ponded_area[idx] = column_double(stmt.get(), 7);
            if (!column_is_null(stmt.get(), 8)) {
                const auto u = static_cast<std::size_t>(idx);
                if (u >= ctx.nodes.tags.size()) ctx.nodes.tags.resize(u + 1);
                ctx.nodes.tags[u] = column_text(stmt.get(), 8);
            }
        }
    }

    // --- storages child table → side-table (rows created above by set_node_type) ---
    {
        auto stmt = prepare(db,
            "SELECT node_id, curve_name, a, b, c, seep_rate, evap_frac, "
            "exfil_suction, exfil_ksat, exfil_imd FROM storages WHERE simulation_id = ?");
        bind_text(stmt.get(), 1, sim_id);
        auto& S = ctx.node_subtypes.storages;
        while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
            const int r = ctx.node_subtypes.storage_row(ctx.node_names.find(column_text(stmt.get(), 0)));
            if (r < 0) continue;
            const auto ur = static_cast<std::size_t>(r);
            if (!column_is_null(stmt.get(), 1)) S.curve_name[ur] = column_text(stmt.get(), 1);
            S.a[ur]             = column_double(stmt.get(), 2);
            S.b[ur]             = column_double(stmt.get(), 3);
            S.c[ur]             = column_double(stmt.get(), 4);
            S.seep_rate[ur]     = column_double(stmt.get(), 5);
            S.evap_frac[ur]     = column_double(stmt.get(), 6);
            S.exfil_suction[ur] = column_double(stmt.get(), 7);
            S.exfil_ksat[ur]    = column_double(stmt.get(), 8);
            S.exfil_imd[ur]     = column_double(stmt.get(), 9);
        }
    }

    // --- outfalls child table → side-table (route_to resolved after subcatchments) ---
    {
        auto stmt = prepare(db,
            "SELECT node_id, outfall_type, param, has_flap_gate "
            "FROM outfalls WHERE simulation_id = ?");
        bind_text(stmt.get(), 1, sim_id);
        auto& O = ctx.node_subtypes.outfalls;
        while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
            const int r = ctx.node_subtypes.outfall_row(ctx.node_names.find(column_text(stmt.get(), 0)));
            if (r < 0) continue;
            const auto ur = static_cast<std::size_t>(r);
            O.bc_type[ur]       = parse_outfall_type(column_text(stmt.get(), 1));
            O.param[ur]         = column_double(stmt.get(), 2);
            O.has_flap_gate[ur] = column_int(stmt.get(), 3) != 0;
        }
    }

    // --- dividers child table → side-table ---
    {
        auto stmt = prepare(db,
            "SELECT node_id, divider_type, cutoff, cd, max_depth, curve_name, divider_link "
            "FROM dividers WHERE simulation_id = ?");
        bind_text(stmt.get(), 1, sim_id);
        auto& D = ctx.node_subtypes.dividers;
        while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
            const int r = ctx.node_subtypes.divider_row(ctx.node_names.find(column_text(stmt.get(), 0)));
            if (r < 0) continue;
            const auto ur = static_cast<std::size_t>(r);
            D.method[ur]    = parse_divider_type(column_text(stmt.get(), 1));
            D.cutoff[ur]    = column_double(stmt.get(), 2);
            D.cd[ur]        = column_double(stmt.get(), 3);
            D.max_depth[ur] = column_double(stmt.get(), 4);
            if (!column_is_null(stmt.get(), 5)) D.curve_name[ur] = column_text(stmt.get(), 5);
            if (!column_is_null(stmt.get(), 6)) D.link_name[ur]  = column_text(stmt.get(), 6);
        }
    }
}

// Resolve outfall route_to (subcatchment NAME → index). Deferred: outfalls are
// read before [SUBCATCHMENTS], so subcatch_names isn't populated until after.
static void resolve_outfall_route_to(sqlite3* db, SimulationContext& ctx,
                                     const std::string& sim_id) {
    auto stmt = prepare(db,
        "SELECT node_id, route_to FROM outfalls "
        "WHERE simulation_id = ? AND route_to IS NOT NULL");
    bind_text(stmt.get(), 1, sim_id);
    auto& O = ctx.node_subtypes.outfalls;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        const int r = ctx.node_subtypes.outfall_row(ctx.node_names.find(column_text(stmt.get(), 0)));
        if (r < 0) continue;
        O.route_to[static_cast<std::size_t>(r)] = ctx.subcatch_names.find(column_text(stmt.get(), 1));
    }
}

static void read_links(sqlite3* db, SimulationContext& ctx, const std::string& sim_id) {
    // Phase 7: slim base row; subtype config comes from the child tables below.
    auto stmt = prepare(db,
        "SELECT link_id, link_type, geom, from_node, to_node, offset1, offset2, "
        "q0, q_limit, direction, "
        "xsect_shape, xsect_geom1, xsect_geom2, xsect_geom3, xsect_geom4, xsect_curve, "
        "has_flap_gate, tag "
        "FROM links WHERE simulation_id = ? ORDER BY fid");
    bind_text(stmt.get(), 1, sim_id);

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        std::string name = column_text(stmt.get(), 0);
        std::string type_str = column_text(stmt.get(), 1);

        int idx = ctx.link_names.find(name);
        if (idx < 0) idx = ctx.link_names.add(name);
        ensure_link_capacity(ctx, idx);

        LinkType ltype = parse_link_type(type_str);
        // Phase 6 Stage C: create the subtype side-table row (sets links.type too).
        ctx.link_subtypes.set_link_type(ctx.links, idx, ltype);

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
        ctx.links.q0[idx]      = column_double(stmt.get(), 7);
        ctx.links.q_limit[idx] = column_double(stmt.get(), 8);
        ctx.links.direction[idx] = column_int(stmt.get(), 9);

        if (!column_is_null(stmt.get(), 10))
            ctx.links.xsect_shape[idx] = parse_xsect_shape(column_text(stmt.get(), 10));
        // xsect_geom1-4 hold the RAW [XSECTIONS] parameters (display units).
        // Restore them and replay the parser's pre-init field assignment so
        // convert_inputs_to_internal + the init xsect setup derive the final
        // geometry identically to the .inp path (lossless for TRAPEZOIDAL etc.).
        // RAW [XSECTIONS] geom1-4 (display units — the only display-unit fields
        // the .gpkg stores). Keep them verbatim for serialization, and rebuild
        // y_full/w_max/y_bot/r_bot applying the SAME display→internal scaling
        // convert_inputs_to_internal would (since that global pass is skipped for
        // gpkg loads). Keep this in lock-step with that function's xsect block.
        const double g1 = column_double(stmt.get(), 11);
        const double g2 = column_double(stmt.get(), 12);
        const double g3 = column_double(stmt.get(), 13);
        const double g4 = column_double(stmt.get(), 14);
        ctx.links.xsect_geom1[idx] = g1;
        ctx.links.xsect_geom2[idx] = g2;
        ctx.links.xsect_geom3[idx] = g3;
        ctx.links.xsect_geom4[idx] = g4;
        const XsectShape shp = ctx.links.xsect_shape[idx];
        if (shp != XsectShape::IRREGULAR && shp != XsectShape::STREET_XSECT) {
            const int us = ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));
            const double inv_len = ucf::Ucf_inv[ucf::LENGTH][static_cast<std::size_t>(us)];
            ctx.links.xsect_y_full[idx] = g1 * inv_len;   // geom1 (depth/diameter) — always a length
            if (shp != XsectShape::CUSTOM)
                ctx.links.xsect_w_max[idx] =
                    (shp != XsectShape::FORCE_MAIN) ? g2 * inv_len : g2;   // geom2 (width); FM = H-W C
            ctx.links.xsect_y_bot[idx] =
                (shp == XsectShape::RECT_TRIANG || shp == XsectShape::RECT_ROUND ||
                 shp == XsectShape::MODBASKETHANDLE) ? g3 * inv_len : g3;  // geom3 (length only for these)
            ctx.links.xsect_r_bot[idx] = g4;              // geom4 (dimensionless / not convert-scaled)
        }
        // xsect named reference (IRREGULAR/STREET/CUSTOM): transect/street/
        // shape-curve NAME kept in pump_curve_name (resolve_cross_references
        // re-resolves it); other shapes resolve to a [CURVES] index.
        if (!column_is_null(stmt.get(), 15)) {
            std::string ref_name = column_text(stmt.get(), 15);
            const XsectShape rshp = ctx.links.xsect_shape[idx];
            if (rshp == XsectShape::IRREGULAR || rshp == XsectShape::STREET_XSECT ||
                rshp == XsectShape::CUSTOM) {
                ctx.links.pump_curve_name[idx] = ref_name;
                ctx.links.xsect_curve[idx] = -1;
            } else {
                ctx.links.xsect_curve[idx] = ctx.table_names.find(ref_name);
            }
        }
        ctx.links.has_flap_gate[idx] = column_int(stmt.get(), 16) != 0;
        if (!column_is_null(stmt.get(), 17)) {
            const auto u = static_cast<std::size_t>(idx);
            if (u >= ctx.links.tags.size()) ctx.links.tags.resize(u + 1);
            ctx.links.tags[u] = column_text(stmt.get(), 17);
        }
    }

    // ---- Subtype child tables → side-tables (rows created above; GPKG stores
    //      display units and skips the global convert pass, so write verbatim) ----
    {  // conduits
        auto cs = prepare(db,
            "SELECT link_id, roughness, length, xsect_barrels, xsect_culvert, "
            "loss_inlet, loss_outlet, loss_avg, seep_rate FROM conduits "
            "WHERE simulation_id = ?");
        bind_text(cs.get(), 1, sim_id);
        auto& CD = ctx.link_subtypes.conduits;
        while (sqlite3_step(cs.get()) == SQLITE_ROW) {
            const int idx = ctx.link_names.find(column_text(cs.get(), 0));
            const int cr = (idx >= 0) ? ctx.link_subtypes.conduit_row(idx) : -1;
            if (cr < 0) continue;
            const auto u = (size_t)cr;
            CD.roughness[u]    = column_double(cs.get(), 1);
            CD.length[u]       = column_double(cs.get(), 2);
            CD.barrels[u]      = column_int(cs.get(), 3);
            CD.culvert_code[u] = column_int(cs.get(), 4);
            CD.loss_inlet[u]   = column_double(cs.get(), 5);
            CD.loss_outlet[u]  = column_double(cs.get(), 6);
            CD.loss_avg[u]     = column_double(cs.get(), 7);
            CD.seep_rate[u]    = column_double(cs.get(), 8);
        }
    }
    {  // pumps
        auto ps = prepare(db,
            "SELECT link_id, pump_curve, init_state, startup_depth, shutoff_depth "
            "FROM pumps WHERE simulation_id = ?");
        bind_text(ps.get(), 1, sim_id);
        auto& PD = ctx.link_subtypes.pumps;
        while (sqlite3_step(ps.get()) == SQLITE_ROW) {
            const int idx = ctx.link_names.find(column_text(ps.get(), 0));
            const int pr = (idx >= 0) ? ctx.link_subtypes.pump_row(idx) : -1;
            if (pr < 0) continue;
            if (!column_is_null(ps.get(), 1))
                ctx.links.pump_curve_name[idx] = column_text(ps.get(), 1);
            const auto u = (size_t)pr;
            PD.init_state[u] = (column_double(ps.get(), 2) > 0.5) ? uint8_t{1} : uint8_t{0};
            PD.startup[u]    = column_double(ps.get(), 3);
            PD.shutoff[u]    = column_double(ps.get(), 4);
        }
    }
    {  // orifices
        auto os = prepare(db,
            "SELECT link_id, orientation, discharge_coeff, orate FROM orifices "
            "WHERE simulation_id = ?");
        bind_text(os.get(), 1, sim_id);
        auto& ORF = ctx.link_subtypes.orifices;
        while (sqlite3_step(os.get()) == SQLITE_ROW) {
            const int idx = ctx.link_names.find(column_text(os.get(), 0));
            const int orr = (idx >= 0) ? ctx.link_subtypes.orifice_row(idx) : -1;
            if (orr < 0) continue;
            const auto u = (size_t)orr;
            ORF.orifice_type[u] = (column_text(os.get(), 1) == std::string("SIDE")) ? 1.0 : 0.0;
            ORF.cd[u]           = column_double(os.get(), 2);
            ORF.orate[u]        = column_double(os.get(), 3);
        }
    }
    {  // weirs
        auto ws = prepare(db,
            "SELECT link_id, weir_type, discharge_coeff, crest_height, end_contractions "
            "FROM weirs WHERE simulation_id = ?");
        bind_text(ws.get(), 1, sim_id);
        auto& WD = ctx.link_subtypes.weirs;
        while (sqlite3_step(ws.get()) == SQLITE_ROW) {
            const int idx = ctx.link_names.find(column_text(ws.get(), 0));
            const int wr = (idx >= 0) ? ctx.link_subtypes.weir_row(idx) : -1;
            if (wr < 0) continue;
            const auto u = (size_t)wr;
            const std::string wt = column_text(ws.get(), 1);
            WD.weir_type[u] = (wt == "SIDEFLOW") ? 1.0 : (wt == "V-NOTCH") ? 2.0
                            : (wt == "TRAPEZOIDAL") ? 3.0 : (wt == "ROADWAY") ? 4.0 : 0.0;
            WD.cd[u]               = column_double(ws.get(), 2);
            WD.crest_height[u]     = column_double(ws.get(), 3);
            WD.end_contractions[u] = static_cast<double>(column_int(ws.get(), 4));
        }
    }
    {  // outlets
        auto outs = prepare(db,
            "SELECT link_id, rating_type, rating_curve, q_coeff, q_expon, crest_height "
            "FROM outlets WHERE simulation_id = ?");
        bind_text(outs.get(), 1, sim_id);
        auto& OUT = ctx.link_subtypes.outlets;
        while (sqlite3_step(outs.get()) == SQLITE_ROW) {
            const int idx = ctx.link_names.find(column_text(outs.get(), 0));
            const int olr = (idx >= 0) ? ctx.link_subtypes.outlet_row(idx) : -1;
            if (olr < 0) continue;
            const auto u = (size_t)olr;
            const std::string rt = column_text(outs.get(), 1);
            OUT.outlet_type[u] = (rt == "FUNCTIONAL/DEPTH") ? 1.0 : (rt == "TABULAR/HEAD") ? 2.0
                               : (rt == "TABULAR/DEPTH") ? 3.0 : 0.0;
            if (!column_is_null(outs.get(), 2))      // TABULAR rating-curve NAME
                ctx.links.pump_curve_name[idx] = column_text(outs.get(), 2);
            OUT.coeff[u]        = column_double(outs.get(), 3);
            OUT.expon[u]        = column_double(outs.get(), 4);
            OUT.crest_height[u] = column_double(outs.get(), 5);
        }
    }
}

static void read_subcatchments(sqlite3* db, SimulationContext& ctx, const std::string& sim_id) {
    auto stmt = prepare(db,
        "SELECT subcatch_id, geom, outlet_node, outlet_subcatch, rain_gage, "
        "area, width, slope, curb_length, frac_imperv, "
        "n_imperv, n_perv, ds_imperv, ds_perv, pct_zero_imperv, "
        "subarea_routing, pct_routed, "
        "infil_model, infil_p1, infil_p2, infil_p3, infil_p4, infil_p5, tag "
        "FROM subcatchments WHERE simulation_id = ? ORDER BY fid");
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

        if (!column_is_null(stmt.get(), 23)) {
            const auto u = static_cast<std::size_t>(idx);
            if (u >= ctx.subcatches.tags.size()) ctx.subcatches.tags.resize(u + 1);
            ctx.subcatches.tags[u] = column_text(stmt.get(), 23);
        }
    }
}

static void read_rain_gages(sqlite3* db, SimulationContext& ctx, const std::string& sim_id) {
    auto stmt = prepare(db,
        "SELECT gage_id, geom, rain_type, rain_interval, snow_catch, data_source, source_name "
        "FROM rain_gages WHERE simulation_id = ? ORDER BY fid");
    bind_text(stmt.get(), 1, sim_id);

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        std::string name = column_text(stmt.get(), 0);
        int idx = ctx.gage_names.find(name);
        if (idx < 0) idx = ctx.gage_names.add(name);

        size_t n = static_cast<size_t>(idx + 1);
        // Size every gage SoA column (not just the ones written below) so the
        // resolve pass — which indexes ts_name/co_gage_index/etc. up to
        // n_gages — never reads past the end. count()==source.size() would
        // otherwise equal n_gages and skip resolve's guarded resize.
        ctx.gages.grow_to(idx + 1);
        auto grow = [n](auto& vec) { if (vec.size() < n) vec.resize(n); };
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
            // Store the source name so resolve_cross_references can re-link the
            // timeseries: gages are read before [TIMESERIES], so find() here is
            // -1 until the deferred re-resolution runs against ts_name.
            ctx.gages.ts_name[static_cast<size_t>(idx)] = src;
            ctx.gages.ts_index[idx] = ctx.table_names.find(src);
        }
    }
}

static void read_curves(sqlite3* db, SimulationContext& ctx, const std::string& sim_id) {
    auto stmt = prepare(db,
        "SELECT curve_id, curve_type, x_value, y_value "
        "FROM curves WHERE simulation_id = ? ORDER BY fid");
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
        "FROM input_timeseries WHERE simulation_id = ? ORDER BY fid");
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
        double ts = parse_ts_timestamp(column_text(stmt.get(), 1));
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
        "FROM patterns WHERE simulation_id = ? ORDER BY fid");
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

/// True if @p column exists on @p table. Used to read columns added in a later
/// schema revision while still loading older GeoPackages that predate them.
static bool column_exists(sqlite3* db, const std::string& table,
                          const std::string& column) {
    auto stmt = prepare(db,
        "SELECT COUNT(*) FROM pragma_table_info(?) WHERE name=?");
    bind_text(stmt.get(), 1, table);
    bind_text(stmt.get(), 2, column);
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

    // snow_elev / snow_dtlong were added in a later schema revision; read them
    // only when present so pre-revision GeoPackages still load.
    if (column_exists(db, "climate_settings", "snow_dtlong")) {
        auto s2 = prepare(db,
            "SELECT snow_elev, snow_dtlong FROM climate_settings "
            "WHERE simulation_id = ?");
        bind_text(s2.get(), 1, sim_id);
        if (sqlite3_step(s2.get()) == SQLITE_ROW) {
            ctx.options.snow_elev   = column_double(s2.get(), 0);
            ctx.options.snow_dtlong = column_double(s2.get(), 1);
        }
    }

    // temp_units added in a later schema revision; guard independently.
    if (column_exists(db, "climate_settings", "temp_units")) {
        auto s3 = prepare(db,
            "SELECT temp_units FROM climate_settings WHERE simulation_id = ?");
        bind_text(s3.get(), 1, sim_id);
        if (sqlite3_step(s3.get()) == SQLITE_ROW)
            ctx.options.temp_units = column_int(s3.get(), 0);
    }
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

    // RDII exponential-decay parameters (optional — older GeoPackages won't have it)
    if (table_exists(db, "rdii_decay")) {
        auto stmt = prepare(db,
            "SELECT uh_name, response, k_dep, k_0, k_T, T_ref, theta_rec, T_freeze "
            "FROM rdii_decay WHERE simulation_id = ?");
        bind_text(stmt.get(), 1, sim_id);
        while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
            RDIIDecayEntry e{};
            e.uh_name = column_text(stmt.get(), 0);
            std::string resp = column_text(stmt.get(), 1);
            if      (resp == "SHORT")  e.response = 0;
            else if (resp == "MEDIUM") e.response = 1;
            else if (resp == "LONG")   e.response = 2;
            else continue;
            e.k_dep     = sqlite3_column_double(stmt.get(), 2);
            e.k_0       = sqlite3_column_double(stmt.get(), 3);
            e.k_T       = sqlite3_column_double(stmt.get(), 4);
            e.T_ref     = sqlite3_column_double(stmt.get(), 5);
            e.theta_rec = sqlite3_column_double(stmt.get(), 6);
            e.T_freeze  = sqlite3_column_double(stmt.get(), 7);
            ctx.rdii_decay.add(e);
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

static void read_inflows(sqlite3* db, SimulationContext& ctx, const std::string& sim_id) {
    if (!table_exists(db, "inflows")) return;
    auto stmt = prepare(db,
        "SELECT node_id, constituent, timeseries, inflow_type, m_factor, s_factor, "
        "baseline, pattern FROM inflows WHERE simulation_id = ? ORDER BY fid");
    bind_text(stmt.get(), 1, sim_id);
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        const int ni = ctx.node_names.find(column_text(stmt.get(), 0));
        if (ni < 0) continue;
        std::string cons = column_text(stmt.get(), 1);
        std::string ts   = column_is_null(stmt.get(), 2) ? std::string{} : column_text(stmt.get(), 2);
        std::string type = column_is_null(stmt.get(), 3) ? std::string("FLOW") : column_text(stmt.get(), 3);
        double mf   = column_double(stmt.get(), 4);
        double sf   = column_double(stmt.get(), 5);
        double base = column_double(stmt.get(), 6);
        std::string pat = column_is_null(stmt.get(), 7) ? std::string{} : column_text(stmt.get(), 7);
        ctx.ext_inflows.add(ni, cons, ts, type, mf, sf, base, pat);
    }
}

static void read_controls(sqlite3* db, SimulationContext& ctx, const std::string& sim_id) {
    if (!table_exists(db, "control_rules")) return;
    auto stmt = prepare(db,
        "SELECT rule_text FROM control_rules WHERE simulation_id = ? ORDER BY ordinal");
    bind_text(stmt.get(), 1, sim_id);
    while (sqlite3_step(stmt.get()) == SQLITE_ROW)
        ctx.control_rules.rule_text.push_back(column_text(stmt.get(), 0));
}

static void read_transects(sqlite3* db, SimulationContext& ctx, const std::string& sim_id) {
    if (!table_exists(db, "transects")) return;
    auto stmt = prepare(db,
        "SELECT transect_id, ordinal, station, elevation, n_left, n_right, n_channel, "
        "x_left_bank, x_right_bank, x_left_encroach, x_right_encroach, "
        "x_factor, y_factor, length_factor, comment "
        "FROM transects WHERE simulation_id = ? ORDER BY fid");
    bind_text(stmt.get(), 1, sim_id);
    auto& T = ctx.transects;
    std::string prev;
    int ti = -1;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        std::string name = column_text(stmt.get(), 0);
        if (ti < 0 || name != prev) {
            // New transect — push its per-transect scalars (mirrors handle_transects).
            T.names.push_back(name);
            T.comments.push_back(column_is_null(stmt.get(), 14) ? std::string{}
                                                                : column_text(stmt.get(), 14));
            T.n_left.push_back(column_double(stmt.get(), 4));
            T.n_right.push_back(column_double(stmt.get(), 5));
            T.n_channel.push_back(column_double(stmt.get(), 6));
            T.x_left_bank.push_back(column_double(stmt.get(), 7));
            T.x_right_bank.push_back(column_double(stmt.get(), 8));
            T.x_left_encroachment.push_back(column_double(stmt.get(), 9));
            T.x_right_encroachment.push_back(column_double(stmt.get(), 10));
            T.x_factor.push_back(column_double(stmt.get(), 11));
            T.y_factor.push_back(column_double(stmt.get(), 12));
            T.length_factor.push_back(column_double(stmt.get(), 13));
            T.stations.emplace_back();
            T.elevations.emplace_back();
            ti = T.count() - 1;
            prev = name;
        }
        const auto ut = static_cast<std::size_t>(ti);
        T.stations[ut].push_back(column_double(stmt.get(), 2));
        T.elevations[ut].push_back(column_double(stmt.get(), 3));
    }
}

static void read_dwf(sqlite3* db, SimulationContext& ctx, const std::string& sim_id) {
    if (!table_exists(db, "dwf_inflows")) return;
    auto stmt = prepare(db,
        "SELECT node_id, constituent, avg_value, pat1, pat2, pat3, pat4 "
        "FROM dwf_inflows WHERE simulation_id = ? ORDER BY fid");
    bind_text(stmt.get(), 1, sim_id);
    auto txt = [&](int col) {
        return column_is_null(stmt.get(), col) ? std::string{} : column_text(stmt.get(), col);
    };
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        const int ni = ctx.node_names.find(column_text(stmt.get(), 0));
        if (ni < 0) continue;
        ctx.dwf_inflows.add(ni, column_text(stmt.get(), 1), column_double(stmt.get(), 2),
                            txt(3), txt(4), txt(5), txt(6));
    }
}

// ============================================================================
// Part E — 2D surface-routing mesh (model definition; 2D results live in
// the HDF5 file referenced by the 2D_OUTPUT_FILE option key, never here).
//
// Populates the engine's 2D storage through ctx.twod_io exactly as the
// [2D_*] inp section handlers would: primary data only (vertices, triangle
// connectivity, coupling NAMES, pending BC/conveyance rows). Derived
// topology, coupling name→index resolution, BC/conveyance drains, and unit
// scaling all happen later in SurfaceRouter2D::initialize(), so a gpkg-read
// model is bit-identical to the same model read from .inp.
// ============================================================================

static int bc_type_from_token(const std::string& tok) {
    if (tok == "WALL")            return 0; // BoundaryType::WALL
    if (tok == "NORMAL_FLOW")     return 1; // BoundaryType::NORMAL_FLOW
    if (tok == "SPECIFIED_STAGE") return 2; // BoundaryType::SPECIFIED_STAGE
    if (tok == "TS_STAGE")        return 2;
    if (tok == "SPECIFIED_FLOW")  return 3; // BoundaryType::SPECIFIED_FLOW
    if (tok == "TS_FLOW")         return 3;
    if (tok == "RATING_CURVE")    return 4; // BoundaryType::RATING_CURVE
    return -1;
}

static void read_mesh_2d(sqlite3* db, SimulationContext& ctx,
                         const std::string& sim_id) {
    if (!table_exists(db, "mesh_2d_vertices") ||
        !table_exists(db, "mesh_2d_triangles"))
        return; // pre-2D GeoPackage — nothing to do

    // Engine built without the 2D module: surface a warning if this file
    // actually carries a mesh for the requested simulation, then skip.
    if (!ctx.twod_io.mesh) {
        auto cnt = prepare(db,
            "SELECT COUNT(*) FROM mesh_2d_vertices WHERE simulation_id = ?");
        bind_text(cnt.get(), 1, sim_id);
        if (sqlite3_step(cnt.get()) == SQLITE_ROW &&
            column_int(cnt.get(), 0) > 0) {
            ctx.warnings.push_back(
                "WARNING: GeoPackage contains a 2D mesh but this engine "
                "build has no 2D surface-routing module — mesh skipped.");
        }
        return;
    }

    auto& mesh = *ctx.twod_io.mesh;

    // ---- vertices ----------------------------------------------------------
    {
        auto cnt = prepare(db,
            "SELECT COUNT(*), COALESCE(MAX(vertex_idx), -1) "
            "FROM mesh_2d_vertices WHERE simulation_id = ?");
        bind_text(cnt.get(), 1, sim_id);
        if (sqlite3_step(cnt.get()) != SQLITE_ROW) return;
        const int n = column_int(cnt.get(), 0);
        if (n == 0) return;
        // Defensive against hand-edited files: vertex ordinals must form
        // the contiguous range [0, n) because triangle connectivity and
        // conveyance rows index into it positionally.
        if (column_int(cnt.get(), 1) != n - 1)
            throw GpkgError("mesh_2d_vertices: vertex_idx values are not "
                            "contiguous [0, n)");
        mesh.resize_vertices(n);

        auto stmt = prepare(db,
            "SELECT vertex_idx, x, y, z, tag FROM mesh_2d_vertices "
            "WHERE simulation_id = ? ORDER BY vertex_idx");
        bind_text(stmt.get(), 1, sim_id);
        while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
            const int i = column_int(stmt.get(), 0);
            if (i < 0 || i >= n) continue;
            mesh.vx[i]   = column_double(stmt.get(), 1);
            mesh.vy[i]   = column_double(stmt.get(), 2);
            mesh.vz[i]   = column_double(stmt.get(), 3);
            mesh.vtag[i] = column_text(stmt.get(), 4);
        }
    }

    // ---- triangles ---------------------------------------------------------
    // geom / bed_elev / coupled_node are derived presentation columns and
    // intentionally ignored; v0/v1/v2 + vertex x/y are canonical.
    {
        auto cnt = prepare(db,
            "SELECT COUNT(*), COALESCE(MAX(tri_idx), -1) "
            "FROM mesh_2d_triangles WHERE simulation_id = ?");
        bind_text(cnt.get(), 1, sim_id);
        if (sqlite3_step(cnt.get()) != SQLITE_ROW) return;
        const int n = column_int(cnt.get(), 0);
        if (n == 0) return;
        if (column_int(cnt.get(), 1) != n - 1)
            throw GpkgError("mesh_2d_triangles: tri_idx values are not "
                            "contiguous [0, n)");
        mesh.resize_triangles(n);

        auto stmt = prepare(db,
            "SELECT tri_idx, v0, v1, v2, mannings_n, tag FROM mesh_2d_triangles "
            "WHERE simulation_id = ? ORDER BY tri_idx");
        bind_text(stmt.get(), 1, sim_id);
        while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
            const int t = column_int(stmt.get(), 0);
            if (t < 0 || t >= n) continue;
            mesh.tri_v0[t]      = column_int(stmt.get(), 1);
            mesh.tri_v1[t]      = column_int(stmt.get(), 2);
            mesh.tri_v2[t]      = column_int(stmt.get(), 3);
            mesh.mannings_n[t]  = column_double(stmt.get(), 4);
            mesh.tri_tag[t]     = column_text(stmt.get(), 5);
        }
    }

    // ---- coupling maps -------------------------------------------------------
    // Restored as NAMES (indices stay -1): SurfaceRouter2D::initialize()
    // resolves them against ctx.node_names with the same unknown-node fatal
    // behavior as the inp path.
    if (table_exists(db, "mesh_2d_vertex_coupling")) {
        auto stmt = prepare(db,
            "SELECT vertex_idx, node_id, coupling_cd, coupling_area "
            "FROM mesh_2d_vertex_coupling WHERE simulation_id = ?");
        bind_text(stmt.get(), 1, sim_id);
        while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
            const int i = column_int(stmt.get(), 0);
            if (i < 0 || i >= mesh.n_vertices()) continue;
            mesh.vert_coupled_node_name[i] = column_text(stmt.get(), 1);
            mesh.vert_coupling_cd[i]       = column_double(stmt.get(), 2);
            mesh.vert_coupling_area[i]     = column_double(stmt.get(), 3);
        }
    }
    if (table_exists(db, "mesh_2d_triangle_coupling")) {
        auto stmt = prepare(db,
            "SELECT tri_idx, node_id, coupling_cd, coupling_area "
            "FROM mesh_2d_triangle_coupling WHERE simulation_id = ?");
        bind_text(stmt.get(), 1, sim_id);
        while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
            const int t = column_int(stmt.get(), 0);
            if (t < 0 || t >= mesh.n_triangles()) continue;
            mesh.tri_coupled_node_name[t] = column_text(stmt.get(), 1);
            mesh.tri_coupling_cd[t]       = column_double(stmt.get(), 2);
            mesh.tri_coupling_area[t]     = column_double(stmt.get(), 3);
        }
    }

    // ---- boundary conditions → pending rows ----------------------------------
    if (ctx.twod_io.pending_bc && table_exists(db, "mesh_2d_boundary_conditions")) {
        auto stmt = prepare(db,
            "SELECT tri_idx, edge, bc_type, param1, ref_name, bc_group "
            "FROM mesh_2d_boundary_conditions WHERE simulation_id = ? "
            "ORDER BY tri_idx, edge");
        bind_text(stmt.get(), 1, sim_id);
        while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
            const int type = bc_type_from_token(column_text(stmt.get(), 2));
            if (type < 0) continue; // unknown future token — skip
            twoD::PendingBoundaryRow row;
            row.tri     = column_int(stmt.get(), 0);
            row.edge    = column_int(stmt.get(), 1);
            row.bc_type = type;
            if (!column_is_null(stmt.get(), 3))
                row.param1 = column_double(stmt.get(), 3);
            row.name  = column_text(stmt.get(), 4);
            row.group = column_text(stmt.get(), 5);
            ctx.twod_io.pending_bc->push_back(std::move(row));
        }
    }

    // ---- edge conveyance → pending rows --------------------------------------
    if (ctx.twod_io.pending_ec && table_exists(db, "mesh_2d_edge_conveyance")) {
        auto stmt = prepare(db,
            "SELECT v_from, v_to, conveyance FROM mesh_2d_edge_conveyance "
            "WHERE simulation_id = ? ORDER BY v_from, v_to");
        bind_text(stmt.get(), 1, sim_id);
        while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
            twoD::PendingEdgeConveyanceRow row;
            row.v_from     = column_int(stmt.get(), 0);
            row.v_to       = column_int(stmt.get(), 1);
            row.conveyance = column_double(stmt.get(), 2);
            ctx.twod_io.pending_ec->push_back(std::move(row));
        }
    }
}

// ============================================================================
// Public API
// ============================================================================

int read_model(sqlite3* db, SimulationContext& ctx,
                const std::string& simulation_id,
                const std::string& scratch_dir) {
    try {
        // Hard schema-version gate (no backward compatibility). The relational
        // node schema carries storages/outfalls/dividers child tables; a
        // pre-relational flat .gpkg (subtype columns on `nodes`, no child
        // tables) or a foreign file is rejected with an actionable error rather
        // than silently misread.
        if (!table_exists(db, "storages") || !table_exists(db, "outfalls") ||
            !table_exists(db, "dividers")) {
            ctx.errors.push_back(
                "GeoPackage uses an unsupported (pre-relational) node schema: "
                "missing storages/outfalls/dividers tables. Re-export the model "
                "with this engine version.");
            return -1;
        }
        // Phase 7: the link schema is normalized too — a slim `links` base plus
        // conduits/pumps/orifices/weirs/outlets child tables. A pre-Phase-7 file
        // (flat `links` with NULL-padded subtype + param1/param2 columns, no link
        // child tables) — including a Phase-5-era file (normalized nodes, flat
        // links) — is rejected rather than silently misread.
        if (!table_exists(db, "conduits") || !table_exists(db, "pumps") ||
            !table_exists(db, "orifices") || !table_exists(db, "weirs") ||
            !table_exists(db, "outlets")) {
            ctx.errors.push_back(
                "GeoPackage uses an unsupported (pre-relational) link schema: "
                "missing conduits/pumps/orifices/weirs/outlets tables. Re-export "
                "the model with this engine version.");
            return -1;
        }

        // The .gpkg stores hydraulic fields in canonical internal units, so
        // resolve_cross_references must SKIP the display→internal conversion
        // (bit-exact round-trip). See SimulationContext::gpkg_units_internal.
        ctx.gpkg_units_internal = true;
        read_options(db, ctx, simulation_id);
        read_nodes(db, ctx, simulation_id);
        read_links(db, ctx, simulation_id);
        read_rain_gages(db, ctx, simulation_id);
        read_subcatchments(db, ctx, simulation_id);
        resolve_outfall_route_to(db, ctx, simulation_id);  // needs subcatch_names
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
        read_inflows(db, ctx, simulation_id);
        read_dwf(db, ctx, simulation_id);
        read_transects(db, ctx, simulation_id);
        read_controls(db, ctx, simulation_id);

        // Part E — 2D mesh model definition. Options keys (2D_*) were
        // already applied by read_options above, so mesh_units_si is set
        // before SurfaceRouter2D::initialize() ever looks at it.
        read_mesh_2d(db, ctx, simulation_id);

        // Slice IO-8 — hydrate external-file slots from Part D tables
        // and materialise scratch files. Skipped when no scratch_dir
        // was provided (callers that only need model definition).
        read_external_content(db, ctx, simulation_id, scratch_dir);
        return 0;
    } catch (const std::exception&) {
        return -1;
    }
}

int read_from_file(const std::string& path, SimulationContext& ctx,
                   const std::string& simulation_id) {
    try {
        auto db = open_database(path, SQLITE_OPEN_READONLY);
        // Slice IO-8: scratch dir lives next to the .gpkg.
        const std::string scratch = scratchDirFor(path);
        return read_model(db.get(), ctx, simulation_id, scratch);
    } catch (const std::exception&) {
        return -1;
    }
}

} // namespace openswmm::gpkg
