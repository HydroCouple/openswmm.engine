/**
 * @file InpWriter.cpp
 * @brief Comprehensive .inp serialisation — round-trip identical output.
 *
 * @details Writes every SWMM input section with exact column layouts matching
 *          the legacy format so that read -> write -> read produces identical
 *          model state. Shape names, enum strings, and field widths are taken
 *          directly from legacy text.h and keywords.c.
 *
 * All standard SWMM sections implemented:
 *   TITLE, OPTIONS, EVAPORATION, TEMPERATURE, SNOWPACKS, ADJUSTMENTS,
 *   EVENTS,
 *   RAINGAGES, SUBCATCHMENTS, SUBAREAS, INFILTRATION,
 *   JUNCTIONS, OUTFALLS, DIVIDERS, STORAGE, CONDUITS, PUMPS, ORIFICES,
 *   WEIRS, OUTLETS, XSECTIONS, LOSSES, TRANSECTS, STREETS, INLETS,
 *   CONTROLS, REPORT, POLLUTANTS, LANDUSES, BUILDUP, WASHOFF, TREATMENT,
 *   INFLOWS, DWF, RDII, PATTERNS, TIMESERIES, CURVES,
 *   MAP, COORDINATES, VERTICES, Polygons, SYMBOLS,
 *   USER_FLAGS, USER_FLAG_VALUES, PLUGINS,
 *   2D_OPTIONS, 2D_MESH_FILE (external mode) or 2D_VERTICES, 2D_TRIANGLES,
 *   2D_VERTEX_NODE_MAP, 2D_TRIANGLE_NODE_MAP, 2D_BOUNDARY_CONDITIONS,
 *   2D_EDGE_CONVEYANCE (inline mode)
 *
 * @ingroup engine_core
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "InpWriter.hpp"
#include "PathResolver.hpp"
#include "SimulationContext.hpp"
#include "DateTime.hpp"
#include "UnitConversion.hpp"
#include "../input/PostParseResolver.hpp"

// 2D model definition (plain define-free data structs; reached at runtime
// through ctx.twod_io — null in non-2D engine builds).
#include "../2d/data/MeshData.hpp"
#include "../2d/data/SolverOptions2D.hpp"
#include "../2d/data/BoundaryData.hpp"
#include "../2d/data/PendingRows2D.hpp"
#include "../2d/data/Serialize2D.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace openswmm {
namespace inp_writer {

static void sec(FILE* f, const char* name) {
    std::fprintf(f, "\n[%s]\n", name);
}

// Format an OADate as MM/DD/YYYY
template<std::size_t N>
static void fmt_date(char (&buf)[N], double oadate) {
    int y, m, d;
    datetime::decodeDate(oadate, y, m, d);
    std::snprintf(buf, N, "%02d/%02d/%04d", m, d, y);
}

// Format the time-of-day part of an OADate as HH:MM:SS
template<std::size_t N>
static void fmt_time(char (&buf)[N], double oadate) {
    int h, m, s;
    datetime::decodeTime(oadate, h, m, s);
    std::snprintf(buf, N, "%02d:%02d:%02d", h, m, s);
}

// Format a timestep (seconds).  Whole-second values use H:MM:SS (matching
// legacy SWMM GUI GetTimeString); fractional values use %g decimal notation.
template<std::size_t N>
static void fmt_step(char (&buf)[N], double secs) {
    long r = std::lround(secs);
    if (std::fabs(secs - static_cast<double>(r)) < 0.001) {
        int ss = static_cast<int>(r % 60);
        int mm = static_cast<int>((r / 60) % 60);
        int hh = static_cast<int>(r / 3600);
        std::snprintf(buf, N, "%d:%02d:%02d", hh, mm, ss);
    } else {
        std::snprintf(buf, N, "%g", secs);
    }
}

// Format a day-of-year integer as M/D (no leading zeros, matching legacy GUI)
template<std::size_t N>
static void fmt_sweep(char (&buf)[N], int doy) {
    // Anchor to a non-leap year to get a stable month/day
    double dt = datetime::encodeDate(2001, 1, 1) + static_cast<double>(doy - 1);
    int y, m, d;
    datetime::decodeDate(dt, y, m, d);
    std::snprintf(buf, N, "%d/%d", m, d);
}

// Write per-object comment lines. Multi-line comments are stored with literal
// "\\n" (backslash + n) as separator; each part is emitted as a ;-prefixed row.
static void write_obj_comment(FILE* f,
                               const std::vector<std::string>& comments,
                               std::size_t idx)
{
    if (idx >= comments.size() || comments[idx].empty()) return;
    const std::string& c = comments[idx];
    const char* sep = "\\n";   // literal two-character token
    std::size_t start = 0;
    while (start <= c.size()) {
        std::size_t end = c.find(sep, start);
        if (end == std::string::npos) end = c.size();
        std::fprintf(f, ";%.*s\n",
                     static_cast<int>(end - start), c.data() + start);
        if (end == c.size()) break;
        start = end + 2;  // skip the two-character "\\n" token
    }
}

static const std::unordered_map<int, const char*> CURVE_TYPE_LABEL = {
    {static_cast<int>(TableType::CURVE_STORAGE),   "STORAGE"},
    {static_cast<int>(TableType::CURVE_DIVERSION),  "DIVERSION"},
    {static_cast<int>(TableType::CURVE_RATING),     "RATING"},
    {static_cast<int>(TableType::CURVE_SHAPE),      "SHAPE"},
    {static_cast<int>(TableType::CURVE_CONTROL),    "CONTROL"},
    {static_cast<int>(TableType::CURVE_TIDAL),      "TIDAL"},
    {static_cast<int>(TableType::CURVE_PUMP1),      "PUMP1"},
    {static_cast<int>(TableType::CURVE_PUMP2),      "PUMP2"},
    {static_cast<int>(TableType::CURVE_PUMP3),      "PUMP3"},
    {static_cast<int>(TableType::CURVE_PUMP4),      "PUMP4"},
    {static_cast<int>(TableType::CURVE_PUMP5),      "PUMP5"},
};

static const char* nN(const SimulationContext& c, int i) {
    return (i>=0 && i<c.n_nodes()) ? c.node_names.name_of(i).c_str() : "*";
}
static const char* gN(const SimulationContext& c, int i) {
    return (i>=0 && i<c.n_gages()) ? c.gage_names.name_of(i).c_str() : "*";
}
static const char* tN(const SimulationContext& c, int i) {
    return (i>=0 && i<static_cast<int>(c.table_names.size())) ? c.table_names.name_of(i).c_str() : "*";
}
static const char* pN(const SimulationContext& c, int i) {
    return (i>=0 && i<c.n_pollutants()) ? c.pollutant_names.name_of(i).c_str() : "*";
}

static const char* xsName(int s) {
    // Indexed by LinkData::XsectShape enum values
    static const char* n[] = {
        "CIRCULAR","FILLED_CIRCULAR","RECT_CLOSED","RECT_OPEN",
        "TRAPEZOIDAL","TRIANGULAR","PARABOLIC","POWER","MODBASKETHANDLE",
        "EGG","HORSESHOE","GOTHIC","CATENARY","SEMIELLIPTICAL",
        "BASKETHANDLE","SEMICIRCULAR","RECT_TRIANGULAR","RECT_ROUND",
        "HORIZ_ELLIPSE","VERT_ELLIPSE","ARCH",
        "IRREGULAR","CUSTOM","FORCE_MAIN","STREET","DUMMY"
    };
    return (s>=0&&s<=25) ? n[s] : "CIRCULAR";
}
static const char* ofName(OutfallType t) {
    switch(t){case OutfallType::NORMAL:return"NORMAL";case OutfallType::FIXED:return"FIXED";
    case OutfallType::TIDAL:return"TIDAL";case OutfallType::TIMESERIES:return"TIMESERIES";default:return"FREE";}
}
static bool hasNT(const SimulationContext& c, NodeType t) {
    for(int j=0;j<c.n_nodes();++j) if(c.nodes.type[static_cast<size_t>(j)]==t) return true; return false;
}
static bool hasLT(const SimulationContext& c, LinkType t) {
    for(int j=0;j<c.n_links();++j) if(c.links.type[static_cast<size_t>(j)]==t) return true; return false;
}

// Slice IO-4 helper — pick the path token to emit for an external-file slot.
// Honors ctx.options.write_absolute_paths and falls back to absolute form
// when relative is impossible (cross-volume, beyond depth cap, etc.).
//
// Inputs:
//   slot          The FilePathPair carrying {absolute, original}.
//   dst_dir       Destination .inp directory (anchor for rebase). Empty
//                 when writeInpFile was called with a bare filename in the
//                 current working directory.
//   force_abs     True when WRITE_ABSOLUTE_PATHS is set; bypasses rebase.
//   warnings_out  Optional sink for cross-volume / over-depth diagnostics.
//
// Returns the token (without surrounding quotes) to emit inside the .inp.
// Empty return means "don't emit the row".
static std::string emit_path_token(const FilePathPair& slot,
                                    const std::string&  dst_dir,
                                    bool                force_abs,
                                    std::vector<std::string>* warnings_out) {
    if (slot.absolute.empty() && slot.original.empty()) return {};

    if (force_abs) {
        // Power-user opt-out — emit absolute form verbatim. Prefer the
        // resolver's resolution when available; otherwise the original
        // token may itself already be absolute.
        return !slot.absolute.empty() ? slot.absolute : slot.original;
    }

    if (dst_dir.empty()) {
        // No anchor — keep the original token (writer was called without
        // a directory context, e.g. bare "out.inp"). Fall back to absolute
        // when the original is empty.
        return slot.original.empty() ? slot.absolute : slot.original;
    }

    // If the resolver pass never ran (programmatic-model path) the slot
    // carries only `.original`. If that token is itself absolute, rebase
    // it; otherwise treat it as already relative to the source `.inp`
    // and pass through unchanged — the writer has no anchor to rebase
    // against.
    const std::string& target = !slot.absolute.empty() ? slot.absolute
                                                       : slot.original;
    if (slot.absolute.empty() && !io::isAbsolutePath(slot.original)) {
        return slot.original;
    }

    auto r = io::makeRelative(target, dst_dir);
    if (warnings_out
        && r.classification != io::PathClass::Relative
        && !r.warning.empty()) {
        warnings_out->push_back(r.warning);
    }
    return r.path;
}

// ============================================================================
// [2D_*] sections — 2D surface-routing model definition.
//
// Sources are reached through ctx.twod_io (non-owning pointers wired by
// SWMMEngine); all null in engine builds without 2D support, so this is a
// guarded no-op there and for pure-1D models (no defaults pollution).
//
// External-mesh policy: when SolverOptions2D::mesh_file is set, the main
// .inp gets [2D_OPTIONS] plus the [2D_MESH_FILE] reference (never inline
// geometry), and the CURRENT in-memory mesh state is written to the .2dm
// sidecar resolved against the destination directory — external mode means
// "the mesh lives in a sidecar", so saving the model saves both files.
// This persists API mutations made after the external load (vertex Z,
// conveyance, BC edits) and keeps the reference valid when saving to a
// different directory. With no mesh_file, all sections are inlined.
//
// Units: a `;; UNITS: SI (m)` header is emitted under [2D_VERTICES] when
// the in-memory mesh is in SI metres — either authored that way
// (mesh_units_si) or converted in place by SurfaceRouter2D::initialize()
// (mesh_scaled_to_si). prescan2DUnitsHeader picks it up on reload and
// skips the FLOW_UNITS rescale, so post-run saves of US-unit projects
// round-trip without double-scaling. Coupling AREA values share the same
// units convention as the mesh and are emitted as stored.
// ============================================================================

// [2D_BOUNDARY_CONDITIONS] grammar token for a pending row (TS_* spellings
// chosen when the parameter is a timeseries name, so type round-trips).
static const char* bc2d_type_token(const twoD::PendingBoundaryRow& r) {
    switch (r.bc_type) {
        case 1:  return "NORMAL_FLOW";
        case 2:  return r.name.empty() ? "SPECIFIED_STAGE" : "TS_STAGE";
        case 3:  return r.name.empty() ? "SPECIFIED_FLOW"  : "TS_FLOW";
        case 4:  return "RATING_CURVE";
        default: return "WALL";
    }
}

static void emit2DMeshSections(FILE* f, const SimulationContext& ctx);

static void write2DSections(FILE* f, const SimulationContext& ctx,
                            const std::string& dst_dir,
                            bool force_abs_paths,
                            std::vector<std::string>* warnings) {
    const auto& tio = ctx.twod_io;
    if (!tio.mesh || !tio.options) return;
    const auto& mesh = *tio.mesh;
    const auto& o    = *tio.options;

    const bool has_mesh = mesh.n_vertices() > 0 && mesh.n_triangles() > 0;
    const bool external = !o.mesh_file.empty();
    if (!has_mesh && !external) return;

    // ---- [2D_OPTIONS] -----------------------------------------------------
    // Exact key set accepted by parse2DOptionsLine — nothing else (unknown
    // keys are parse errors on reload).
    static const char* sLinSolver[] = {"GMRES", "BICGSTAB", "TFQMR"};
    static const char* sPrecond[]   = {"NONE", "JACOBI", "ILU", "AMG"};
    static const char* sRainMode[]  = {"NATURAL_NEIGHBOUR", "SYSTEM", "NONE"};
    sec(f, "2D_OPTIONS");
    std::fprintf(f, ";;%-20s %s\n", "Parameter", "Value");
    std::fprintf(f, "%-22s %.12g\n", "MAX_TIMESTEP",      o.max_timestep);
    std::fprintf(f, "%-22s %.12g\n", "MIN_TIMESTEP",      o.min_timestep);
    std::fprintf(f, "%-22s %.12g\n", "REL_TOLERANCE",     o.rel_tolerance);
    std::fprintf(f, "%-22s %.12g\n", "ABS_TOLERANCE",     o.abs_tolerance);
    std::fprintf(f, "%-22s %.12g\n", "DRY_DEPTH",         o.dry_depth);
    std::fprintf(f, "%-22s %.12g\n", "LIMITER_EPSILON",   o.limiter_epsilon);
    std::fprintf(f, "%-22s %.12g\n", "COUPLING_CD",       o.coupling_cd);
    std::fprintf(f, "%-22s %d\n",    "MAX_KRYLOV_DIM",    o.max_krylov_dim);
    std::fprintf(f, "%-22s %d\n",    "COUPLING_INTERVAL", o.coupling_interval);
    std::fprintf(f, "%-22s %.12g\n", "COUPLING_WINDOW",   o.coupling_window);
    std::fprintf(f, "%-22s %d\n",    "MAX_CVODE_STEPS",   o.max_cvode_steps);
    std::fprintf(f, "%-22s %s\n",    "LINEAR_SOLVER",
                 sLinSolver[static_cast<int>(o.linear_solver) >= 0 &&
                            static_cast<int>(o.linear_solver) <= 2
                                ? static_cast<int>(o.linear_solver) : 0]);
    std::fprintf(f, "%-22s %s\n",    "PRECONDITIONER",
                 sPrecond[static_cast<int>(o.preconditioner) >= 0 &&
                          static_cast<int>(o.preconditioner) <= 3
                              ? static_cast<int>(o.preconditioner) : 0]);
    std::fprintf(f, "%-22s %s\n",    "RAINFALL_MODE",
                 sRainMode[static_cast<int>(o.rainfall_mode) >= 0 &&
                           static_cast<int>(o.rainfall_mode) <= 2
                               ? static_cast<int>(o.rainfall_mode) : 0]);
    std::fprintf(f, "%-22s %s\n",    "REPORT_2D", o.report_2d ? "YES" : "NO");
    if (!o.output_file.empty())
        std::fprintf(f, "%-22s %s\n", "OUTPUT_FILE", o.output_file.c_str());

    // ---- [2D_MESH_FILE] — keep the reference, refresh the sidecar ----------
    if (external) {
        std::string tok = o.mesh_file;
        if (!force_abs_paths && !dst_dir.empty() && io::isAbsolutePath(tok)) {
            auto r = io::makeRelative(tok, dst_dir);
            if (warnings && r.classification != io::PathClass::Relative
                && !r.warning.empty())
                warnings->push_back(r.warning);
            tok = r.path;
        }
        sec(f, "2D_MESH_FILE");
        std::fprintf(f, "FILE %s\n", tok.c_str());

        // External mode means "the mesh lives in a sidecar": write the
        // CURRENT in-memory state to the .2dm the emitted reference resolves
        // to, so post-load API mutations persist and a save to a different
        // directory carries its mesh along. Skipped when no mesh is loaded
        // (e.g. the external file failed to load) so a good sidecar is
        // never clobbered with emptiness.
        if (has_mesh) {
            namespace fs = std::filesystem;
            fs::path sp(tok);
            if (sp.is_relative() && !dst_dir.empty()) sp = fs::path(dst_dir) / sp;
            std::error_code ec;
            if (sp.has_parent_path()) fs::create_directories(sp.parent_path(), ec);
            if (FILE* sf = std::fopen(sp.string().c_str(), "w")) {
                std::fprintf(sf, ";; OpenSWMM 2D mesh — written by the engine "
                                 "alongside the .inp save.\n");
                emit2DMeshSections(sf, ctx);
                std::fclose(sf);
            } else if (warnings) {
                warnings->push_back(
                    "[2D_MESH_FILE]: could not write mesh sidecar '"
                    + sp.string() + "' — mesh state was NOT saved");
            }
        }
        return;
    }

    emit2DMeshSections(f, ctx);
}

// Emit [2D_VERTICES] / [2D_TRIANGLES] / node maps / [2D_BOUNDARY_CONDITIONS]
// / [2D_EDGE_CONVEYANCE] from the in-memory mesh state. Target is either the
// main .inp (inline mode) or the external .2dm sidecar — both are parsed by
// the same section grammar (SectionHandlers2D / load2DMeshExternalFile).
static void emit2DMeshSections(FILE* f, const SimulationContext& ctx) {
    const auto& tio  = ctx.twod_io;
    const auto& mesh = *tio.mesh;
    const auto& o    = *tio.options;

    // ---- [2D_VERTICES] ------------------------------------------------------
    const bool si = o.mesh_units_si || o.mesh_scaled_to_si;
    const int nv = mesh.n_vertices();
    const int nt = mesh.n_triangles();

    sec(f, "2D_VERTICES");
    if (si) std::fprintf(f, ";; UNITS: SI (m)\n");
    std::fprintf(f, ";;%-16s %-18s %-14s %s\n", "X", "Y", "Z", "TAG");
    for (int i = 0; i < nv; ++i) {
        std::fprintf(f, "%-18.10g %-18.10g %-14.10g", mesh.vx[i], mesh.vy[i],
                     mesh.vz[i]);
        if (!mesh.vtag[i].empty()) std::fprintf(f, " %s", mesh.vtag[i].c_str());
        std::fprintf(f, "\n");
    }

    // ---- [2D_TRIANGLES] -------------------------------------------------------
    sec(f, "2D_TRIANGLES");
    std::fprintf(f, ";;%-6s %-8s %-8s %-12s %s\n", "V1", "V2", "V3",
                 "MANNINGS_N", "TAG");
    for (int t = 0; t < nt; ++t) {
        std::fprintf(f, "%-8d %-8d %-8d %-12.6g", mesh.tri_v0[t],
                     mesh.tri_v1[t], mesh.tri_v2[t], mesh.mannings_n[t]);
        if (!mesh.tri_tag[t].empty())
            std::fprintf(f, " %s", mesh.tri_tag[t].c_str());
        std::fprintf(f, "\n");
    }

    // Coupled-node name: prefer the authored name, fall back to the
    // resolved index (API-built models; rename-safe).
    auto node_name_for = [&ctx](const std::string& name, int idx) -> std::string {
        if (!name.empty()) return name;
        if (idx >= 0 && idx < ctx.node_names.size())
            return ctx.node_names.name_of(idx);
        return {};
    };

    // ---- [2D_VERTEX_NODE_MAP] -------------------------------------------------
    {
        bool any = false;
        for (int i = 0; i < nv && !any; ++i)
            any = !node_name_for(mesh.vert_coupled_node_name[i],
                                 mesh.vert_coupled_node[i]).empty();
        if (any) {
            sec(f, "2D_VERTEX_NODE_MAP");
            std::fprintf(f, ";;%-6s %-16s %-10s %s\n", "VERTEX", "NODE", "CD",
                         "AREA");
            for (int i = 0; i < nv; ++i) {
                const std::string cn = node_name_for(
                    mesh.vert_coupled_node_name[i], mesh.vert_coupled_node[i]);
                if (cn.empty()) continue;
                std::fprintf(f, "%-8d %-16s %-10.6g %.12g\n", i, cn.c_str(),
                             mesh.vert_coupling_cd[i],
                             mesh.vert_coupling_area[i]);
            }
        }
    }

    // ---- [2D_TRIANGLE_NODE_MAP] -------------------------------------------------
    {
        bool any = false;
        for (int t = 0; t < nt && !any; ++t)
            any = !node_name_for(mesh.tri_coupled_node_name[t],
                                 mesh.tri_coupled_node[t]).empty();
        if (any) {
            sec(f, "2D_TRIANGLE_NODE_MAP");
            std::fprintf(f, ";;%-6s %-16s %-10s %s\n", "TRIANGLE", "NODE", "CD",
                         "AREA");
            for (int t = 0; t < nt; ++t) {
                const std::string cn = node_name_for(
                    mesh.tri_coupled_node_name[t], mesh.tri_coupled_node[t]);
                if (cn.empty()) continue;
                std::fprintf(f, "%-8d %-16s %-10.6g %.12g\n", t, cn.c_str(),
                             mesh.tri_coupling_cd[t],
                             mesh.tri_coupling_area[t]);
            }
        }
    }

    // ---- [2D_BOUNDARY_CONDITIONS] -------------------------------------------------
    // Authored pending rows preferred (retained after the initialize()
    // drain); BoundaryData reconstruction fallback loses the GROUP label.
    {
        const auto rows = twoD::collectBCRows(tio.pending_bc, tio.boundary,
                                              o.pending_rows_drained);
        if (!rows.empty()) {
            sec(f, "2D_BOUNDARY_CONDITIONS");
            std::fprintf(f, ";;%-4s %-4s %-16s %-14s %-8s %s\n", "TRI", "EDGE",
                         "TYPE", "PARAM_1", "PARAM_2", "GROUP");
            for (const auto& r : rows) {
                char p1[32];
                if (!r.name.empty()) {
                    std::snprintf(p1, sizeof(p1), "%s", r.name.c_str());
                } else if (r.bc_type == 0) { // WALL — no parameter
                    std::snprintf(p1, sizeof(p1), "*");
                } else {
                    std::snprintf(p1, sizeof(p1), "%.12g", r.param1);
                }
                if (!r.group.empty()) {
                    std::fprintf(f, "%-6d %-4d %-16s %-14s %-8s %s\n", r.tri,
                                 r.edge, bc2d_type_token(r), p1, "*",
                                 r.group.c_str());
                } else {
                    std::fprintf(f, "%-6d %-4d %-16s %s\n", r.tri, r.edge,
                                 bc2d_type_token(r), p1);
                }
            }
        }
    }

    // ---- [2D_EDGE_CONVEYANCE] -------------------------------------------------
    {
        const auto rows = twoD::collectConveyanceRows(tio.pending_ec, tio.mesh,
                                                      o.pending_rows_drained);
        if (!rows.empty()) {
            sec(f, "2D_EDGE_CONVEYANCE");
            std::fprintf(f, ";;%-10s %-12s %s\n", "FROM_VERTEX", "TO_VERTEX",
                         "CONVEYANCE");
            for (const auto& r : rows) {
                std::fprintf(f, "%-12d %-12d %.12g\n", r.v_from, r.v_to,
                             r.conveyance);
            }
        }
    }
}

int writeInpFile(const SimulationContext& ctx_internal,
                 const std::string&       path,
                 std::vector<std::string>* warnings) {
    // The engine stores 1D input fields in internal units (feet/cfs); the .inp
    // must carry display units matching FLOW_UNITS. For SI models, convert a
    // local copy back to display units so the live engine state is never
    // mutated. Skipped for US models (factor 1.0) so the common case pays
    // nothing. Without this, each save dumps internal feet and the next open
    // re-applies the m→ft factor, compounding ×3.28084 per cycle.
    const int us_check = ucf::getUnitSystem(
        static_cast<int>(ctx_internal.options.flow_units));
    const bool needs_display_conv =
        ucf::Ucf[ucf::LENGTH][static_cast<std::size_t>(us_check)] != 1.0;

    SimulationContext ctx_display;
    if (needs_display_conv) {
        ctx_display = ctx_internal;
        input::convert_internal_to_display(ctx_display);
    }
    const SimulationContext& ctx = needs_display_conv ? ctx_display : ctx_internal;

    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return -1;

    // Slice IO-4: pre-compute the rebase anchor + opt-out flag once so each
    // section can pass them to emit_path_token() without re-deriving.
    const std::string dst_dir = openswmm::io::parentDir(path);
    const bool        force_abs_paths = ctx.options.write_absolute_paths;

    sec(f,"TITLE");
    std::fprintf(f,";;Project Title/Notes\n");
    // Skip any re-parsed copies of the section-header comment so repeated
    // load/save cycles don't accumulate duplicate ";;Project Title/Notes"
    // lines in the [TITLE] block.
    for (const auto& line : ctx.title_notes) {
        if (line == ";;Project Title/Notes") continue;
        std::fprintf(f,"%s\n", line.c_str());
    }

    // [OPTIONS] — writes every option unconditionally, matching legacy SWMM GUI
    // ExportOptions() structure: core group, ignore group, date/time group,
    // dynamic wave solver group, then engine-specific extensions.
    {
    sec(f,"OPTIONS");
    std::fprintf(f,";;%-20s %s\n","Option","Value");

    static const char* sFlowUnits[]  = {"CFS","GPM","MGD","CMS","LPS","MLD"};
    static const char* sInfilt[]     = {"HORTON","MODIFIED_HORTON","GREEN_AMPT","MODIFIED_GREEN_AMPT","CURVE_NUMBER"};
    static const char* sRouting[]    = {"STEADY","KINWAVE","DYNWAVE"};
    static const char* sInertial[]   = {"NONE","PARTIAL","FULL"};
    static const char* sNormFlow[]   = {"SLOPE","FROUDE","BOTH","NEITHER"};
    static const char* sSurcharge[]  = {"EXTRAN","SLOT","DYNAMIC_SLOT"};

    const SimulationOptions& o = ctx.options;
    int fu  = static_cast<int>(o.flow_units);
    int inf = static_cast<int>(o.infiltration);
    int rm  = static_cast<int>(o.routing_model);
    int id  = o.inertial_damping;
    int nfl = o.normal_flow_ltd;
    int sm  = o.surcharge_method;

    // --- Group 1: Core process options (FLOW_UNITS .. SKIP_STEADY_STATE) ---
    std::fprintf(f,"%-20s %s\n",  "FLOW_UNITS",       (fu>=0&&fu<=5)?sFlowUnits[fu]:"CFS");
    std::fprintf(f,"%-20s %s\n",  "INFILTRATION",     (inf>=0&&inf<=4)?sInfilt[inf]:"HORTON");
    std::fprintf(f,"%-20s %s\n",  "FLOW_ROUTING",     (rm>=0&&rm<=2)?sRouting[rm]:"DYNWAVE");
    std::fprintf(f,"%-20s %s\n",  "LINK_OFFSETS",     o.link_offsets==1?"ELEVATION":"DEPTH");
    std::fprintf(f,"%-20s %g\n",  "MIN_SLOPE",        o.min_slope);
    std::fprintf(f,"%-20s %s\n",  "ALLOW_PONDING",    o.allow_ponding?"YES":"NO");
    std::fprintf(f,"%-20s %s\n",  "SKIP_STEADY_STATE",o.skip_steady_state?"YES":"NO");
    std::fprintf(f,"\n");

    // --- Group 2: Ignore flags (write all, even when NO) ---
    std::fprintf(f,"%-20s %s\n",  "IGNORE_RAINFALL",   o.ignore_rainfall?"YES":"NO");
    std::fprintf(f,"%-20s %s\n",  "IGNORE_SNOWMELT",   o.ignore_snow_melt?"YES":"NO");
    std::fprintf(f,"%-20s %s\n",  "IGNORE_GROUNDWATER",o.ignore_groundwater?"YES":"NO");
    std::fprintf(f,"%-20s %s\n",  "IGNORE_RDII",       o.ignore_rdii?"YES":"NO");
    std::fprintf(f,"%-20s %s\n",  "IGNORE_ROUTING",    o.ignore_routing?"YES":"NO");
    std::fprintf(f,"%-20s %s\n",  "IGNORE_QUALITY",    o.ignore_quality?"YES":"NO");
    std::fprintf(f,"\n");

    // --- Group 3: Date / time options (START_DATE .. RULE_STEP) ---
    char db[32], tb[32], sb[32];
    fmt_date(db, o.start_date);  fmt_time(tb, o.start_date);
    std::fprintf(f,"%-20s %s\n",  "START_DATE",        db);
    std::fprintf(f,"%-20s %s\n",  "START_TIME",        tb);

    double rpt_start = (o.report_start > 0.0) ? o.report_start : o.start_date;
    fmt_date(db, rpt_start);  fmt_time(tb, rpt_start);
    std::fprintf(f,"%-20s %s\n",  "REPORT_START_DATE", db);
    std::fprintf(f,"%-20s %s\n",  "REPORT_START_TIME", tb);

    double end_dt = (o.end_date > 0.0) ? o.end_date : o.start_date;
    fmt_date(db, end_dt);  fmt_time(tb, end_dt);
    std::fprintf(f,"%-20s %s\n",  "END_DATE",          db);
    std::fprintf(f,"%-20s %s\n",  "END_TIME",          tb);

    fmt_sweep(sb, o.sweep_start);
    std::fprintf(f,"%-20s %s\n",  "SWEEP_START",       sb);
    fmt_sweep(sb, o.sweep_end);
    std::fprintf(f,"%-20s %s\n",  "SWEEP_END",         sb);

    std::fprintf(f,"%-20s %g\n",  "DRY_DAYS",          o.dry_days);
    fmt_step(sb, o.report_step);
    std::fprintf(f,"%-20s %s\n",  "REPORT_STEP",       sb);
    fmt_step(sb, o.wet_step);
    std::fprintf(f,"%-20s %s\n",  "WET_STEP",          sb);
    fmt_step(sb, o.dry_step);
    std::fprintf(f,"%-20s %s\n",  "DRY_STEP",          sb);
    fmt_step(sb, o.routing_step);
    std::fprintf(f,"%-20s %s\n",  "ROUTING_STEP",      sb);
    fmt_step(sb, o.rule_step);
    std::fprintf(f,"%-20s %s\n",  "RULE_STEP",         sb);
    std::fprintf(f,"\n");

    // --- Group 4: Dynamic wave / solver options (INERTIAL_DAMPING .. THREADS) ---
    std::fprintf(f,"%-20s %s\n",  "INERTIAL_DAMPING",    (id>=0&&id<=2)?sInertial[id]:"PARTIAL");
    std::fprintf(f,"%-20s %s\n",  "NORMAL_FLOW_LIMITED", (nfl>=0&&nfl<=3)?sNormFlow[nfl]:"BOTH");
    std::fprintf(f,"%-20s %s\n",  "FORCE_MAIN_EQUATION", o.force_main_eqn==1?"D-W":"H-W");
    std::fprintf(f,"%-20s %s\n",  "SURCHARGE_METHOD",    (sm>=0&&sm<=2)?sSurcharge[sm]:"EXTRAN");
    std::fprintf(f,"%-20s %.2f\n","VARIABLE_STEP",       o.variable_step);
    std::fprintf(f,"%-20s %g\n",  "LENGTHENING_STEP",    o.lengthening_step);
    std::fprintf(f,"%-20s %g\n",  "MIN_SURFAREA",        o.min_surf_area);
    std::fprintf(f,"%-20s %d\n",  "MAX_TRIALS",          o.max_trials);
    std::fprintf(f,"%-20s %g\n",  "HEAD_TOLERANCE",      o.head_tol);
    std::fprintf(f,"%-20s %g\n",  "SYS_FLOW_TOL",        o.sys_flow_tol * 100.0);
    std::fprintf(f,"%-20s %g\n",  "LAT_FLOW_TOL",        o.lat_flow_tol * 100.0);
    fmt_step(sb, o.min_routing_step);
    std::fprintf(f,"%-20s %s\n",  "MINIMUM_STEP",        sb);
    std::fprintf(f,"%-20s %d\n",  "THREADS",             o.num_threads);

    // --- Engine-specific extensions (not in legacy GUI) ---
    if (sm == 2) {
        std::fprintf(f,"%-20s %.4f\n","DPS_CELERITY",   o.dps_target_celerity);
        std::fprintf(f,"%-20s %.4f\n","DPS_ALPHA",      o.dps_alpha);
        std::fprintf(f,"%-20s %.4f\n","DPS_DECAY_TIME", o.dps_decay_time);
    }
    if (o.node_continuity != NodeContinuity::EXPLICIT)
        std::fprintf(f,"%-20s %s\n",  "NODE_CONTINUITY","SEMI_IMPLICIT");
    if (o.anderson_accel)
        std::fprintf(f,"%-20s %s\n",  "ANDERSON_ACCEL", "YES");
    if (!o.crs.empty())
        std::fprintf(f,"%-20s %s\n",  "CRS",            o.crs.c_str());
    if (o.write_absolute_paths)
        std::fprintf(f,"%-20s %s\n",  "WRITE_ABSOLUTE_PATHS", "YES");
    for (const auto& kv : o.ext_options)
        std::fprintf(f,"%-20s %s\n",  kv.first.c_str(), kv.second.c_str());
    }

    // [EVAPORATION]
    {
        const auto& opts = ctx.options;
        sec(f,"EVAPORATION");
        std::fprintf(f,";;Type       Parameters\n");
        std::fprintf(f,";;---------- ----------\n");
        switch (opts.evap_type) {
            case 0:  // CONSTANT
                std::fprintf(f,"CONSTANT     %.4f\n", opts.evap_values[0]);
                break;
            case 1:  // MONTHLY
                std::fprintf(f,"MONTHLY     ");
                for (int i = 0; i < 12; ++i)
                    std::fprintf(f," %.4f", opts.evap_values[i]);
                std::fprintf(f,"\n");
                break;
            case 2:  // TIMESERIES
                std::fprintf(f,"TIMESERIES   %s\n", opts.evap_ts_name.c_str());
                break;
            case 3:  // TEMPERATURE
                std::fprintf(f,"TEMPERATURE\n");
                break;
            case 4:  // FILE
                std::fprintf(f,"FILE        ");
                for (int i = 0; i < 12; ++i)
                    std::fprintf(f," %.4f", opts.pan_coeff[i]);
                std::fprintf(f,"\n");
                break;
        }
        if (!opts.evap_recovery_pat.empty())
            std::fprintf(f,"RECOVERY     %s\n", opts.evap_recovery_pat.c_str());
        std::fprintf(f,"DRY_ONLY     %s\n", opts.evap_dry_only ? "YES" : "NO");
    }

    // [TEMPERATURE]
    {
        const auto& opts = ctx.options;
        bool has_temp = (opts.temp_source > 0 || opts.wind_type > 0 ||
                         opts.snow_divt != 34.0 || opts.snow_lat != 0.0 ||
                         opts.snow_dtlong != 0.0);
        for (int i = 0; i < 10 && !has_temp; ++i)
            if (opts.adc_imperv[i] != 1.0 || opts.adc_perv[i] != 1.0) has_temp = true;
        // Check monthly wind speeds
        for (int i = 0; i < 12 && !has_temp; ++i)
            if (opts.wind_speed[i] != 0.0) has_temp = true;

        if (has_temp) {
            sec(f,"TEMPERATURE");
            if (opts.temp_source == 1)
                std::fprintf(f,"TIMESERIES   %s\n", opts.temp_ts_name.c_str());
            else if (opts.temp_source == 2) {
                const std::string tok =
                    emit_path_token(opts.temp_file, dst_dir, force_abs_paths, warnings);
                std::fprintf(f,"FILE         \"%s\"", tok.c_str());
                // Legacy positional form: FILE fname [startdate] [units]. Emit a
                // "*" start-date placeholder when units are set without a date.
                if (opts.temp_file_start > 0.0)
                    std::fprintf(f," %.6f", opts.temp_file_start);
                else if (opts.temp_units >= 0)
                    std::fprintf(f," *");
                if (opts.temp_units >= 0) {
                    static const char* kUnitsWords[] = {"C10", "C", "F"};
                    std::fprintf(f," %s", kUnitsWords[opts.temp_units]);
                }
                std::fprintf(f,"\n");
            }

            if (opts.wind_type == 0) {
                std::fprintf(f,"WINDSPEED    MONTHLY");
                for (int i = 0; i < 12; ++i)
                    std::fprintf(f," %.4f", opts.wind_speed[i]);
                std::fprintf(f,"\n");
            } else {
                std::fprintf(f,"WINDSPEED    FILE\n");
            }

            if (opts.snow_dtlong != 0.0) {
                // Legacy 9-token form carries the longitude/solar-time
                // correction (minutes):
                //   SNOWMELT divt ati nrg elev lat dtlong minMelt maxMelt
                std::fprintf(f,"SNOWMELT     %.2f %.4f %.4f %.4f %.4f %.4f %.6f %.6f\n",
                             opts.snow_divt, opts.snow_ati_wt, opts.snow_nrg_ratio,
                             opts.snow_elev, opts.snow_lat, opts.snow_dtlong,
                             opts.snow_min_melt, opts.snow_max_melt);
            } else {
                std::fprintf(f,"SNOWMELT     %.2f %.4f %.4f %.4f %.6f %.6f %.4f\n",
                             opts.snow_divt, opts.snow_ati_wt, opts.snow_nrg_ratio,
                             opts.snow_lat, opts.snow_min_melt, opts.snow_max_melt,
                             opts.snow_elev);
            }

            std::fprintf(f,"ADC          IMPERVIOUS");
            for (int i = 0; i < 10; ++i)
                std::fprintf(f," %.4f", opts.adc_imperv[i]);
            std::fprintf(f,"\n");
            std::fprintf(f,"ADC          PERVIOUS");
            for (int i = 0; i < 10; ++i)
                std::fprintf(f," %.4f", opts.adc_perv[i]);
            std::fprintf(f,"\n");
        }
    }

    // [SNOWPACKS]
    if (!ctx.snowpacks.names.empty()) {
        sec(f,"SNOWPACKS");
        std::fprintf(f,";;%-16s %-12s Parameters\n","Name","Surface");
        std::fprintf(f,";;---------- ---------- ----------\n");
        for (size_t j = 0; j < ctx.snowpacks.names.size(); ++j) {
            const char* name = ctx.snowpacks.names[j].c_str();
            if (j < ctx.snowpacks.plowable.size()) {
                const auto& p = ctx.snowpacks.plowable[j];
                std::fprintf(f,"%-16s PLOWABLE    ", name);
                for (int k = 0; k < 7; ++k) std::fprintf(f," %10.4f", p[static_cast<size_t>(k)]);
                std::fprintf(f,"\n");
            }
            if (j < ctx.snowpacks.impervious.size()) {
                const auto& p = ctx.snowpacks.impervious[j];
                std::fprintf(f,"%-16s IMPERVIOUS  ", name);
                for (int k = 0; k < 7; ++k) std::fprintf(f," %10.4f", p[static_cast<size_t>(k)]);
                std::fprintf(f,"\n");
            }
            if (j < ctx.snowpacks.pervious.size()) {
                const auto& p = ctx.snowpacks.pervious[j];
                std::fprintf(f,"%-16s PERVIOUS    ", name);
                for (int k = 0; k < 7; ++k) std::fprintf(f," %10.4f", p[static_cast<size_t>(k)]);
                std::fprintf(f,"\n");
            }
            if (j < ctx.snowpacks.removal.size()) {
                const auto& r = ctx.snowpacks.removal[j];
                bool has_removal = false;
                for (int k = 0; k < 6; ++k)
                    if (r[static_cast<size_t>(k)] != 0.0) { has_removal = true; break; }
                if (has_removal) {
                    std::fprintf(f,"%-16s REMOVAL     ", name);
                    for (int k = 0; k < 6; ++k) std::fprintf(f," %10.4f", r[static_cast<size_t>(k)]);
                    if (j < ctx.snowpacks.removal_subcatch.size() &&
                        !ctx.snowpacks.removal_subcatch[j].empty())
                        std::fprintf(f," %s", ctx.snowpacks.removal_subcatch[j].c_str());
                    std::fprintf(f,"\n");
                }
            }
        }
    }

    // [ADJUSTMENTS]
    {
        bool has_adj = false;
        for (int i = 0; i < 12; ++i) {
            if (ctx.adjust_temp[i] != 0.0 || ctx.adjust_evap[i] != 1.0 ||
                ctx.adjust_rain[i] != 1.0 || ctx.adjust_hydcon[i] != 1.0)
            { has_adj = true; break; }
        }
        for (size_t i = 0; i < ctx.subcatch_n_perv_pattern.size() && !has_adj; ++i)
            if (ctx.subcatch_n_perv_pattern[i] >= 0) has_adj = true;
        for (size_t i = 0; i < ctx.subcatch_d_store_pattern.size() && !has_adj; ++i)
            if (ctx.subcatch_d_store_pattern[i] >= 0) has_adj = true;
        for (size_t i = 0; i < ctx.subcatch_infil_pattern.size() && !has_adj; ++i)
            if (ctx.subcatch_infil_pattern[i] >= 0) has_adj = true;

        if (has_adj) {
            sec(f,"ADJUSTMENTS");
            auto write12 = [&](const char* key, const double* arr) {
                std::fprintf(f,"%-12s", key);
                for (int i = 0; i < 12; ++i)
                    std::fprintf(f," %10.4f", arr[i]);
                std::fprintf(f,"\n");
            };
            write12("TEMP",    ctx.adjust_temp);
            write12("EVAP",    ctx.adjust_evap);
            write12("RAIN",    ctx.adjust_rain);
            write12("CONDUCT", ctx.adjust_hydcon);

            for (size_t i = 0; i < ctx.subcatch_n_perv_pattern.size(); ++i) {
                int pi = ctx.subcatch_n_perv_pattern[i];
                if (pi >= 0)
                    std::fprintf(f,"N-PERV       %-16s %s\n",
                                 ctx.subcatch_names.name_of(static_cast<int>(i)).c_str(),
                                 tN(ctx, pi));
            }
            for (size_t i = 0; i < ctx.subcatch_d_store_pattern.size(); ++i) {
                int pi = ctx.subcatch_d_store_pattern[i];
                if (pi >= 0)
                    std::fprintf(f,"DSTORE       %-16s %s\n",
                                 ctx.subcatch_names.name_of(static_cast<int>(i)).c_str(),
                                 tN(ctx, pi));
            }
            for (size_t i = 0; i < ctx.subcatch_infil_pattern.size(); ++i) {
                int pi = ctx.subcatch_infil_pattern[i];
                if (pi >= 0)
                    std::fprintf(f,"INFIL        %-16s %s\n",
                                 ctx.subcatch_names.name_of(static_cast<int>(i)).c_str(),
                                 tN(ctx, pi));
            }
        }
    }

    // [EVENTS]  — Slice CW (added 2026-05-21)
    // Format matches legacy SWMM 5.2: 4 whitespace-separated columns
    //   StartDate StartTime EndDate EndTime   (HH:MM resolution)
    if (!ctx.events.empty()) {
        sec(f, "EVENTS");
        std::fprintf(f, ";;Start Date  Start Time  End Date    End Time\n");
        std::fprintf(f, ";;----------  ----------  ----------  ----------\n");
        char sd[16], st[16], ed[16], et[16];
        for (const auto& ev : ctx.events) {
            fmt_date(sd, ev.start);
            fmt_time(st, ev.start);
            fmt_date(ed, ev.end);
            fmt_time(et, ev.end);
            // Drop the :SS tail for legacy SWMM 5.2 parity (HH:MM resolution).
            // fmt_time emits HH:MM:SS — truncate to HH:MM for the writer.
            st[5] = '\0';
            et[5] = '\0';
            std::fprintf(f, "%-10s  %-10s  %-10s  %-10s\n", sd, st, ed, et);
        }
    }

    // [RAINGAGES]
    if(ctx.n_gages()>0){sec(f,"RAINGAGES");
    std::fprintf(f,";;%-16s %-12s %-8s %-8s %-16s\n","Name","Format","Intvl","SCF","Source");
    std::fprintf(f,";;%-16s %-12s %-8s %-8s %-16s\n","----------------","------------","--------","--------","----------------");
    for(int j=0;j<ctx.n_gages();++j){auto u=static_cast<size_t>(j);
    write_obj_comment(f, ctx.gages.comments, u);
    int iv=ctx.gages.interval_sec[u];int h=iv/3600,m=(iv%3600)/60;int ts=ctx.gages.ts_index[u];
    // Emit the actual rain-data format (0=INTENSITY,1=VOLUME,2=CUMULATIVE) — a
    // prior hardcoded "INTENSITY" silently rewrote VOLUME/CUMULATIVE gages.
    const char* fmt = ctx.gages.rain_type[u]==1 ? "VOLUME"
                    : ctx.gages.rain_type[u]==2 ? "CUMULATIVE" : "INTENSITY";
    if(ts>=0)std::fprintf(f,"%-16s %-12s %d:%02d     %.2f     TIMESERIES %s\n",ctx.gage_names.name_of(j).c_str(),fmt,h,m,ctx.gages.snow_factor[u],tN(ctx,ts));
    else if(!ctx.gages.file_path[u].empty()){
        const std::string tok = emit_path_token(ctx.gages.file_path[u],
                                                 dst_dir, force_abs_paths, warnings);
        std::fprintf(f,"%-16s %-12s %d:%02d     %.2f     FILE \"%s\" %s\n",
                      ctx.gage_names.name_of(j).c_str(),fmt,h,m,
                      ctx.gages.snow_factor[u],
                      tok.c_str(),
                      ctx.gages.col_name[u].c_str());
    }
    }}

    // [SUBCATCHMENTS]
    if(ctx.n_subcatches()>0){sec(f,"SUBCATCHMENTS");
    std::fprintf(f,";;%-16s %-16s %-16s %-12s %-10s %-12s %-10s %-10s\n","Name","RainGage","Outlet","Area","%%Imperv","Width","%%Slope","CurbLen");
    std::fprintf(f,";;%-16s %-16s %-16s %-12s %-10s %-12s %-10s %-10s\n","----------------","----------------","----------------","------------","----------","------------","----------","----------");
    for(int j=0;j<ctx.n_subcatches();++j){auto u=static_cast<size_t>(j);
    write_obj_comment(f, ctx.subcatches.comments, u);
    std::fprintf(f,"%-16s %-16s %-16s %12.4f %10.2f %12.4f %10.4f %10.4f\n",ctx.subcatch_names.name_of(j).c_str(),gN(ctx,ctx.subcatches.gage[u]),nN(ctx,ctx.subcatches.outlet_node[u]),ctx.subcatches.area[u],ctx.subcatches.frac_imperv[u]*100.0,ctx.subcatches.width[u],ctx.subcatches.slope[u]*100.0,ctx.subcatches.curb_length[u]);
    }}

    // [SUBAREAS]
    if(ctx.n_subcatches()>0){
    static const char* kRouteToNames[]={"OUTLET","IMPERV","PERV"};
    sec(f,"SUBAREAS");
    std::fprintf(f,";;%-16s %-10s %-10s %-10s %-10s %-10s %-8s %-10s\n","Subcatch","N-Imperv","N-Perv","S-Imperv","S-Perv","%%ZeroImp","RouteTo","PctRouted");
    std::fprintf(f,";;%-16s %-10s %-10s %-10s %-10s %-10s %-8s %-10s\n","----------------","----------","----------","----------","----------","----------","--------","----------");
    for(int j=0;j<ctx.n_subcatches();++j){auto u=static_cast<size_t>(j);
    int rt=ctx.subcatches.subarea_routing[u];
    if(rt<0||rt>2)rt=0;
    // S-Imperv/S-Perv are stored in display depth units (Runoff converts them
    // via UCF(RAINDEPTH) at use), so emit as-is — a prior *12.0 corrupted them
    // (and broke file→write→reparse round-trips).
    std::fprintf(f,"%-16s %10.4f %10.4f %10.4f %10.4f %10.2f %-8s %10.2f\n",ctx.subcatch_names.name_of(j).c_str(),ctx.subcatches.n_imperv[u],ctx.subcatches.n_perv[u],ctx.subcatches.ds_imperv[u],ctx.subcatches.ds_perv[u],ctx.subcatches.frac_imperv_no_store[u]*100.0,kRouteToNames[rt],ctx.subcatches.pct_routed[u]*100.0);
    }}

    // [INFILTRATION]
    if(ctx.n_subcatches()>0){sec(f,"INFILTRATION");
    std::fprintf(f,";;%-16s %-10s %-10s %-10s %-10s %-10s\n","Subcatch","Param1","Param2","Param3","Param4","Param5");
    std::fprintf(f,";;%-16s %-10s %-10s %-10s %-10s %-10s\n","----------------","----------","----------","----------","----------","----------");
    static const char* infilNames[]={"HORTON","MODIFIED_HORTON","GREEN_AMPT","MODIFIED_GREEN_AMPT","CURVE_NUMBER"};
    for(int j=0;j<ctx.n_subcatches();++j){auto u=static_cast<size_t>(j);
    int im=ctx.subcatches.infil_model[u];
    const char* mn=(im>=0&&im<=4)?infilNames[im]:"HORTON";
    std::fprintf(f,"%-16s %10.4f %10.4f %10.4f %10.4f %10.4f %s\n",
        ctx.subcatch_names.name_of(j).c_str(),
        ctx.subcatches.infil_p1[u],ctx.subcatches.infil_p2[u],
        ctx.subcatches.infil_p3[u],ctx.subcatches.infil_p4[u],
        ctx.subcatches.infil_p5[u],mn);
    }}

    // [LID_CONTROLS]
    if (ctx.lid_controls.count() > 0) {
        sec(f,"LID_CONTROLS");
        std::fprintf(f,";;%-16s %-12s Parameters\n","Name","Type/Layer");
        std::fprintf(f,";;---------- ---------- ----------\n");
        for (int j = 0; j < ctx.lid_controls.count(); ++j) {
            auto uj = static_cast<size_t>(j);
            const char* name = ctx.lid_controls.names[uj].c_str();
            const char* ltype = ctx.lid_controls.lid_type[uj].c_str();
            // Type line
            std::fprintf(f,"%-16s %s\n", name, ltype);
            // SURFACE (5 params)
            if (uj < ctx.lid_controls.surface.size()) {
                const auto& p = ctx.lid_controls.surface[uj];
                bool has = false; for (int k=0;k<5;++k) if(p[k]!=0.0){has=true;break;}
                if (has) {
                    std::fprintf(f,"%-16s SURFACE    ", name);
                    for (int k=0;k<5;++k) std::fprintf(f," %10.4f",p[k]);
                    std::fprintf(f,"\n");
                }
            }
            // SOIL (7 params)
            if (uj < ctx.lid_controls.soil.size()) {
                const auto& p = ctx.lid_controls.soil[uj];
                bool has = false; for (int k=0;k<7;++k) if(p[k]!=0.0){has=true;break;}
                if (has) {
                    std::fprintf(f,"%-16s SOIL       ", name);
                    for (int k=0;k<7;++k) std::fprintf(f," %10.4f",p[k]);
                    std::fprintf(f,"\n");
                }
            }
            // PAVEMENT (6 params)
            if (uj < ctx.lid_controls.pavement.size()) {
                const auto& p = ctx.lid_controls.pavement[uj];
                bool has = false; for (int k=0;k<6;++k) if(p[k]!=0.0){has=true;break;}
                if (has) {
                    std::fprintf(f,"%-16s PAVEMENT   ", name);
                    for (int k=0;k<6;++k) std::fprintf(f," %10.4f",p[k]);
                    std::fprintf(f,"\n");
                }
            }
            // STORAGE (4 params)
            if (uj < ctx.lid_controls.storage.size()) {
                const auto& p = ctx.lid_controls.storage[uj];
                bool has = false; for (int k=0;k<4;++k) if(p[k]!=0.0){has=true;break;}
                if (has) {
                    std::fprintf(f,"%-16s STORAGE    ", name);
                    for (int k=0;k<4;++k) std::fprintf(f," %10.4f",p[k]);
                    std::fprintf(f,"\n");
                }
            }
            // DRAIN (6 params)
            if (uj < ctx.lid_controls.drain.size()) {
                const auto& p = ctx.lid_controls.drain[uj];
                bool has = false; for (int k=0;k<6;++k) if(p[k]!=0.0){has=true;break;}
                if (has) {
                    std::fprintf(f,"%-16s DRAIN      ", name);
                    for (int k=0;k<6;++k) std::fprintf(f," %10.4f",p[k]);
                    std::fprintf(f,"\n");
                }
            }
            // DRAINMAT (3 params)
            if (uj < ctx.lid_controls.drainmat.size()) {
                const auto& p = ctx.lid_controls.drainmat[uj];
                bool has = false; for (int k=0;k<3;++k) if(p[k]!=0.0){has=true;break;}
                if (has) {
                    std::fprintf(f,"%-16s DRAINMAT   ", name);
                    for (int k=0;k<3;++k) std::fprintf(f," %10.4f",p[k]);
                    std::fprintf(f,"\n");
                }
            }
        }
    }

    // [LID_USAGE]
    if (ctx.lid_usage.count() > 0) {
        sec(f,"LID_USAGE");
        std::fprintf(f,";;%-16s %-16s %-8s %-12s %-10s %-8s %-8s %-8s %-16s %-16s %-8s\n",
                     "Subcatch","LID","Number","Area","Width","InitSat",
                     "FromImp","ToPerv","RptFile","DrainTo","FromPerv");
        for (int j = 0; j < ctx.lid_usage.count(); ++j) {
            auto uj = static_cast<size_t>(j);
            int sc = ctx.lid_usage.subcatch_index[uj];
            int li = ctx.lid_usage.lid_index[uj];
            const char* sc_name = (sc >= 0) ? ctx.subcatch_names.name_of(sc).c_str() : "*";
            const char* lid_name = (li >= 0 && li < ctx.lid_names.size())
                                   ? ctx.lid_names.name_of(li).c_str() : "*";
            std::fprintf(f,"%-16s %-16s %8d %12.2f %10.2f %8.2f %8.2f %8d",
                         sc_name, lid_name,
                         ctx.lid_usage.number[uj],
                         ctx.lid_usage.area[uj],
                         ctx.lid_usage.width[uj],
                         ctx.lid_usage.init_sat[uj],
                         ctx.lid_usage.from_imperv[uj],
                         ctx.lid_usage.to_perv[uj]);
            if (uj < ctx.lid_usage.rpt_file.size() && !ctx.lid_usage.rpt_file[uj].empty())
                std::fprintf(f," %s", ctx.lid_usage.rpt_file[uj].c_str());
            else
                std::fprintf(f," *");
            if (uj < ctx.lid_usage.drain_to.size() && !ctx.lid_usage.drain_to[uj].empty())
                std::fprintf(f," %s", ctx.lid_usage.drain_to[uj].c_str());
            else
                std::fprintf(f," *");
            if (uj < ctx.lid_usage.from_perv.size())
                std::fprintf(f," %8.2f", ctx.lid_usage.from_perv[uj]);
            std::fprintf(f,"\n");
        }
    }

    // [JUNCTIONS]
    if(hasNT(ctx,NodeType::JUNCTION)){sec(f,"JUNCTIONS");
    std::fprintf(f,";;%-16s %-12s %-12s %-12s %-12s %-12s\n","Name","Elev","MaxDepth","InitDepth","SurDepth","Aponded");
    std::fprintf(f,";;%-16s %-12s %-12s %-12s %-12s %-12s\n","----------------","------------","------------","------------","------------","------------");
    for(int j=0;j<ctx.n_nodes();++j){auto u=static_cast<size_t>(j);if(ctx.nodes.type[u]!=NodeType::JUNCTION)continue;
    write_obj_comment(f, ctx.nodes.comments, u);
    std::fprintf(f,"%-16s %12.4f %12.4f %12.4f %12.4f %12.4f\n",ctx.node_names.name_of(j).c_str(),ctx.nodes.invert_elev[u],ctx.nodes.full_depth[u],ctx.nodes.init_depth[u],ctx.nodes.sur_depth[u],ctx.nodes.ponded_area[u]);
    }}

    // [OUTFALLS]
    if(hasNT(ctx,NodeType::OUTFALL)){sec(f,"OUTFALLS");
    std::fprintf(f,";;%-16s %-12s %-12s %-8s\n","Name","Elev","Type","Gated");
    std::fprintf(f,";;%-16s %-12s %-12s %-8s\n","----------------","------------","------------","--------");
    for(int j=0;j<ctx.n_nodes();++j){auto u=static_cast<size_t>(j);if(ctx.nodes.type[u]!=NodeType::OUTFALL)continue;
    write_obj_comment(f, ctx.nodes.comments, u);
    // Relational side-table (Phase 4).
    const int orow = ctx.node_subtypes.outfall_row(j); const auto& O = ctx.node_subtypes.outfalls;
    const OutfallType otype = (orow>=0)?O.bc_type[static_cast<size_t>(orow)]:OutfallType::FREE;
    const int oflap = (orow>=0)?O.has_flap_gate[static_cast<size_t>(orow)]:0;
    const double oparam = (orow>=0)?O.param[static_cast<size_t>(orow)]:0.0;
    std::fprintf(f,"%-16s %12.4f %-12s %s",ctx.node_names.name_of(j).c_str(),ctx.nodes.invert_elev[u],ofName(otype),oflap?"YES":"NO");
    if(otype==OutfallType::FIXED)std::fprintf(f," %12.4f",oparam);
    std::fprintf(f,"\n");
    }}

    // [DIVIDERS]
    if(hasNT(ctx,NodeType::DIVIDER)){sec(f,"DIVIDERS");
    std::fprintf(f,";;%-16s %-12s %-16s %-12s\n","Name","Elev","DivLink","Type");
    std::fprintf(f,";;%-16s %-12s %-16s %-12s\n","----------------","------------","----------------","------------");
    for(int j=0;j<ctx.n_nodes();++j){auto u=static_cast<size_t>(j);if(ctx.nodes.type[u]!=NodeType::DIVIDER)continue;
    write_obj_comment(f, ctx.nodes.comments, u);
    // Relational side-table (Phase 4).
    const int drow = ctx.node_subtypes.divider_row(j); const auto& D = ctx.node_subtypes.dividers;
    // Resolve diversion link name
    const char* divLinkName = "*";
    std::string dlnStr;
    int dlIdx = (drow>=0) ? D.link[static_cast<size_t>(drow)]
                          : -1;
    if(dlIdx >= 0 && dlIdx < ctx.link_names.size()) {
        dlnStr = ctx.link_names.name_of(dlIdx); divLinkName = dlnStr.c_str();
    } else if(drow>=0 && !D.link_name[static_cast<size_t>(drow)].empty()) {
        divLinkName = D.link_name[static_cast<size_t>(drow)].c_str();
    }
    auto dtype = (drow>=0) ? D.method[static_cast<size_t>(drow)] : DividerType::CUTOFF;
    double cutoff = (drow>=0) ? D.cutoff[static_cast<size_t>(drow)] : 0.0;
    switch(dtype) {
    case DividerType::CUTOFF:
        std::fprintf(f,"%-16s %12.4f %-16s CUTOFF   %12.4f %12.4f %12.4f %12.4f %12.4f\n",
            ctx.node_names.name_of(j).c_str(), ctx.nodes.invert_elev[u], divLinkName,
            cutoff, ctx.nodes.full_depth[u], ctx.nodes.init_depth[u],
            ctx.nodes.sur_depth[u], ctx.nodes.ponded_area[u]);
        break;
    case DividerType::OVERFLOW_DIV:
        std::fprintf(f,"%-16s %12.4f %-16s OVERFLOW %12.4f %12.4f %12.4f %12.4f\n",
            ctx.node_names.name_of(j).c_str(), ctx.nodes.invert_elev[u], divLinkName,
            ctx.nodes.full_depth[u], ctx.nodes.init_depth[u],
            ctx.nodes.sur_depth[u], ctx.nodes.ponded_area[u]);
        break;
    case DividerType::TABULAR: {
        const char* curveName = "*";
        std::string cnStr;
        int ci = (drow>=0) ? D.curve[static_cast<size_t>(drow)]
                           : -1;
        if(ci >= 0 && ci < ctx.table_names.size()) {
            cnStr = ctx.table_names.name_of(ci); curveName = cnStr.c_str();
        } else if(drow>=0 && !D.curve_name[static_cast<size_t>(drow)].empty()) {
            curveName = D.curve_name[static_cast<size_t>(drow)].c_str();
        }
        std::fprintf(f,"%-16s %12.4f %-16s TABULAR  %-16s %12.4f %12.4f %12.4f %12.4f\n",
            ctx.node_names.name_of(j).c_str(), ctx.nodes.invert_elev[u], divLinkName,
            curveName, ctx.nodes.full_depth[u], ctx.nodes.init_depth[u],
            ctx.nodes.sur_depth[u], ctx.nodes.ponded_area[u]);
        break;
    }
    case DividerType::WEIR: {
        double cd = (drow>=0) ? D.cd[static_cast<size_t>(drow)] : 0.0;
        double maxd = (drow>=0) ? D.max_depth[static_cast<size_t>(drow)] : 0.0;
        std::fprintf(f,"%-16s %12.4f %-16s WEIR     %12.4f %12.4f %12.4f %12.4f %12.4f %12.4f %12.4f\n",
            ctx.node_names.name_of(j).c_str(), ctx.nodes.invert_elev[u], divLinkName,
            cutoff, cd, maxd, ctx.nodes.full_depth[u], ctx.nodes.init_depth[u],
            ctx.nodes.sur_depth[u], ctx.nodes.ponded_area[u]);
        break;
    }
    }
    }}

    // [STORAGE]
    if(hasNT(ctx,NodeType::STORAGE)){sec(f,"STORAGE");
    std::fprintf(f,";;%-16s %-12s %-12s %-12s %-12s\n","Name","Elev","MaxDepth","InitDepth","Shape");
    std::fprintf(f,";;%-16s %-12s %-12s %-12s %-12s\n","----------------","------------","------------","------------","------------");
    for(int j=0;j<ctx.n_nodes();++j){auto u=static_cast<size_t>(j);if(ctx.nodes.type[u]!=NodeType::STORAGE)continue;
    write_obj_comment(f, ctx.nodes.comments, u);
    // Relational side-table (Phase 4).
    const int srow = ctx.node_subtypes.storage_row(j); const auto& S = ctx.node_subtypes.storages;
    const int scurve = (srow>=0)?S.curve[static_cast<size_t>(srow)]:-1;
    if(scurve>=0)
        std::fprintf(f,"%-16s %12.4f %12.4f %12.4f TABULAR    %s 0 0 %12.4f\n",ctx.node_names.name_of(j).c_str(),ctx.nodes.invert_elev[u],ctx.nodes.full_depth[u],ctx.nodes.init_depth[u],tN(ctx,scurve),ctx.nodes.sur_depth[u]);
    else {
        const double sa=(srow>=0)?S.a[static_cast<size_t>(srow)]:0.0;
        const double sb=(srow>=0)?S.b[static_cast<size_t>(srow)]:0.0;
        const double sc=(srow>=0)?S.c[static_cast<size_t>(srow)]:0.0;
        std::fprintf(f,"%-16s %12.4f %12.4f %12.4f FUNCTIONAL %g %g %g 0 %12.4f\n",ctx.node_names.name_of(j).c_str(),ctx.nodes.invert_elev[u],ctx.nodes.full_depth[u],ctx.nodes.init_depth[u],sa,sb,sc,ctx.nodes.sur_depth[u]);
    }
    }}

    // [CONDUITS]
    if(hasLT(ctx,LinkType::CONDUIT)){sec(f,"CONDUITS");
    std::fprintf(f,";;%-16s %-16s %-16s %-12s %-12s %-12s %-12s %-10s %-10s\n","Name","FromNode","ToNode","Length","Roughness","InOffset","OutOffset","InitFlow","MaxFlow");
    std::fprintf(f,";;%-16s %-16s %-16s %-12s %-12s %-12s %-12s %-10s %-10s\n","----------------","----------------","----------------","------------","------------","------------","------------","----------","----------");
    for(int j=0;j<ctx.n_links();++j){auto u=static_cast<size_t>(j);if(ctx.links.type[u]!=LinkType::CONDUIT)continue;
    write_obj_comment(f, ctx.links.comments, u);
    const int cr=ctx.link_subtypes.conduit_row(j); const auto& CD=ctx.link_subtypes.conduits;
    std::fprintf(f,"%-16s %-16s %-16s %12.4f %12.6f %12.4f %12.4f %10.4f %10.4f\n",ctx.link_names.name_of(j).c_str(),nN(ctx,ctx.links.node1[u]),nN(ctx,ctx.links.node2[u]),(cr>=0)?CD.length[static_cast<size_t>(cr)]:0.0,(cr>=0)?CD.roughness[static_cast<size_t>(cr)]:0.01,ctx.links.offset1[u],ctx.links.offset2[u],ctx.links.q0[u],ctx.links.q_limit[u]);
    }}

    // [PUMPS]
    if(hasLT(ctx,LinkType::PUMP)){sec(f,"PUMPS");
    std::fprintf(f,";;%-16s %-16s %-16s %-16s %-10s %-10s %-10s\n","Name","FromNode","ToNode","PumpCurve","Status","Startup","Shutoff");
    std::fprintf(f,";;%-16s %-16s %-16s %-16s %-10s %-10s %-10s\n","----------------","----------------","----------------","----------------","----------","----------","----------");
    for(int j=0;j<ctx.n_links();++j){auto u=static_cast<size_t>(j);if(ctx.links.type[u]!=LinkType::PUMP)continue;
    write_obj_comment(f, ctx.links.comments, u);
    const int pr=ctx.link_subtypes.pump_row(j);
    std::fprintf(f,"%-16s %-16s %-16s %-16s %-10s 0          0\n",ctx.link_names.name_of(j).c_str(),nN(ctx,ctx.links.node1[u]),nN(ctx,ctx.links.node2[u]),tN(ctx,(pr>=0)?ctx.link_subtypes.pumps.curve[static_cast<size_t>(pr)]:-1),ctx.links.setting[u]>0?"ON":"OFF");
    }}

    // [ORIFICES]
    if(hasLT(ctx,LinkType::ORIFICE)){sec(f,"ORIFICES");
    std::fprintf(f,";;%-16s %-16s %-16s %-10s %-10s %-10s %-8s\n","Name","FromNode","ToNode","Type","Offset","Cd","Gated");
    std::fprintf(f,";;%-16s %-16s %-16s %-10s %-10s %-10s %-8s\n","----------------","----------------","----------------","----------","----------","----------","--------");
    for(int j=0;j<ctx.n_links();++j){auto u=static_cast<size_t>(j);if(ctx.links.type[u]!=LinkType::ORIFICE)continue;
    write_obj_comment(f, ctx.links.comments, u);
    const int orr=ctx.link_subtypes.orifice_row(j);
    std::fprintf(f,"%-16s %-16s %-16s SIDE       %10.4f %10.4f NO\n",ctx.link_names.name_of(j).c_str(),nN(ctx,ctx.links.node1[u]),nN(ctx,ctx.links.node2[u]),ctx.links.offset1[u],(orr>=0)?ctx.link_subtypes.orifices.cd[static_cast<size_t>(orr)]:0.0);
    }}

    // [WEIRS]
    if(hasLT(ctx,LinkType::WEIR)){sec(f,"WEIRS");
    std::fprintf(f,";;%-16s %-16s %-16s %-12s %-10s %-10s %-8s %-10s %-10s\n","Name","FromNode","ToNode","Type","CrestHt","Cd","Gated","EndCon","EndCoeff");
    std::fprintf(f,";;%-16s %-16s %-16s %-12s %-10s %-10s %-8s %-10s %-10s\n","----------------","----------------","----------------","------------","----------","----------","--------","----------","----------");
    for(int j=0;j<ctx.n_links();++j){auto u=static_cast<size_t>(j);if(ctx.links.type[u]!=LinkType::WEIR)continue;
    write_obj_comment(f, ctx.links.comments, u);
    const int wr=ctx.link_subtypes.weir_row(j); const auto& WD=ctx.link_subtypes.weirs;
    std::fprintf(f,"%-16s %-16s %-16s %-12s %10.4f %10.4f NO       0          0\n",ctx.link_names.name_of(j).c_str(),nN(ctx,ctx.links.node1[u]),nN(ctx,ctx.links.node2[u]),"TRANSVERSE",(wr>=0)?WD.crest_height[static_cast<size_t>(wr)]:0.0,(wr>=0)?WD.cd[static_cast<size_t>(wr)]:0.0);
    }}

    // [OUTLETS]
    if(hasLT(ctx,LinkType::OUTLET)){sec(f,"OUTLETS");
    std::fprintf(f,";;%-16s %-16s %-16s %-10s %-16s %-10s %-10s\n","Name","FromNode","ToNode","Offset","Type","Coeff","Expon");
    std::fprintf(f,";;%-16s %-16s %-16s %-10s %-16s %-10s %-10s\n","----------------","----------------","----------------","----------","----------------","----------","----------");
    for(int j=0;j<ctx.n_links();++j){auto u=static_cast<size_t>(j);if(ctx.links.type[u]!=LinkType::OUTLET)continue;
    write_obj_comment(f, ctx.links.comments, u);
    const int olr=ctx.link_subtypes.outlet_row(j); const auto& OUT=ctx.link_subtypes.outlets;
    std::fprintf(f,"%-16s %-16s %-16s %10.4f FUNCTIONAL   %10g %10g\n",ctx.link_names.name_of(j).c_str(),nN(ctx,ctx.links.node1[u]),nN(ctx,ctx.links.node2[u]),ctx.links.offset1[u],(olr>=0)?OUT.coeff[static_cast<size_t>(olr)]:0.0,(olr>=0)?OUT.expon[static_cast<size_t>(olr)]:0.0);
    }}

    // [XSECTIONS]
    {sec(f,"XSECTIONS");
    std::fprintf(f,";;%-16s %-16s %-12s %-12s %-12s %-12s %-8s\n","Link","Shape","Geom1","Geom2","Geom3","Geom4","Barrels");
    std::fprintf(f,";;%-16s %-16s %-12s %-12s %-12s %-12s %-8s\n","----------------","----------------","------------","------------","------------","------------","--------");
    for(int j=0;j<ctx.n_links();++j){auto u=static_cast<size_t>(j);
    if(ctx.links.type[u]==LinkType::PUMP)continue;
    const int cr=ctx.link_subtypes.conduit_row(j);
    const int xbarrels=(cr>=0)?ctx.link_subtypes.conduits.barrels[static_cast<size_t>(cr)]:1;
    // Emit the retained raw Geom1–Geom4 (preserves trapezoid bottom width /
    // side slopes the derived fields can't reproduce).  xsect_geom1 == 0 means
    // the object was built by a path that didn't populate them; fall back to
    // the legacy derived-field emission.  See LinkData::xsect_geom1.
    // STREET cross-sections store a named [STREETS] reference (Geom1 is the
    // street name, not a dimension), so emit the retained name instead of the
    // numeric geometry — matching the [XSECTIONS] parser's STREET handling.
    if(ctx.links.xsect_shape[u]==XsectShape::STREET_XSECT){
        std::fprintf(f,"%-16s %-16s %-12s %12.4f %12.4f %12.4f %8d\n",
            ctx.link_names.name_of(j).c_str(),
            xsName(static_cast<int>(ctx.links.xsect_shape[u])),
            ctx.links.pump_curve_name[u].c_str(),0.0,0.0,0.0,xbarrels);
        continue;
    }
    double g1,g2,g3,g4;
    if(ctx.links.xsect_geom1[u]!=0.0){
        g1=ctx.links.xsect_geom1[u]; g2=ctx.links.xsect_geom2[u];
        g3=ctx.links.xsect_geom3[u]; g4=ctx.links.xsect_geom4[u];
    } else {
        g1=ctx.links.xsect_y_full[u]; g2=ctx.links.xsect_w_max[u]; g3=0.0; g4=0.0;
    }
    std::fprintf(f,"%-16s %-16s %12.4f %12.4f %12.4f %12.4f %8d\n",ctx.link_names.name_of(j).c_str(),xsName(static_cast<int>(ctx.links.xsect_shape[u])),g1,g2,g3,g4,xbarrels);
    }}

    // [LOSSES]
    {bool hasLoss=false;
    for(int j=0;j<ctx.n_links();++j){auto u=static_cast<size_t>(j);
    {const int cr=ctx.link_subtypes.conduit_row(j);const auto&CD=ctx.link_subtypes.conduits;
    if(ctx.links.type[u]==LinkType::CONDUIT&&cr>=0&&(CD.loss_inlet[static_cast<size_t>(cr)]!=0||CD.loss_outlet[static_cast<size_t>(cr)]!=0||CD.loss_avg[static_cast<size_t>(cr)]!=0||ctx.links.has_flap_gate[u]||CD.seep_rate[static_cast<size_t>(cr)]!=0)){hasLoss=true;break;}}}
    if(hasLoss){sec(f,"LOSSES");
    std::fprintf(f,";;%-16s %-10s %-10s %-10s %-8s %-10s\n","Link","Kentry","Kexit","Kavg","Flap","Seepage");
    std::fprintf(f,";;%-16s %-10s %-10s %-10s %-8s %-10s\n","----------------","----------","----------","----------","--------","----------");
    for(int j=0;j<ctx.n_links();++j){auto u=static_cast<size_t>(j);
    if(ctx.links.type[u]!=LinkType::CONDUIT)continue;
    const int cr=ctx.link_subtypes.conduit_row(j); const auto& CD=ctx.link_subtypes.conduits;
    if(cr<0)continue;
    const auto ucr=static_cast<size_t>(cr);
    if(CD.loss_inlet[ucr]==0&&CD.loss_outlet[ucr]==0&&CD.loss_avg[ucr]==0&&!ctx.links.has_flap_gate[u]&&CD.seep_rate[ucr]==0)continue;
    std::fprintf(f,"%-16s %10.4f %10.4f %10.4f %-8s %10.6f\n",
        ctx.link_names.name_of(j).c_str(),
        CD.loss_inlet[ucr],CD.loss_outlet[ucr],CD.loss_avg[ucr],
        ctx.links.has_flap_gate[u]?"YES":"NO",CD.seep_rate[ucr]);
    }}}

    // [TRANSECTS]
    if(ctx.transects.count()>0){sec(f,"TRANSECTS");
    for(int t=0;t<ctx.transects.count();++t){auto ut=static_cast<size_t>(t);
    std::fprintf(f,"NC %10.4f %10.4f %10.4f\n",ctx.transects.n_left[ut],ctx.transects.n_right[ut],ctx.transects.n_channel[ut]);
    int nsta=static_cast<int>(ctx.transects.stations[ut].size());
    // X1 layout per EPA SWMM 5 (transect.c::setParams):
    //   X1 Name Nsta Xleft Xright 0 0 Lfactor Xfactor Yfactor
    // i.e. TWO placeholder zeros between Xright and Lfactor (not three).
    // An extra zero shifts Lfactor/Xfactor/Yfactor by one column, which
    // every SWMM-5 parser then misreads.
    std::fprintf(f,"X1 %-16s %10d %10.4f %10.4f 0 0 %10.4f %10.4f %10.4f\n",
        ctx.transects.names[ut].c_str(),nsta,ctx.transects.x_left_bank[ut],
        ctx.transects.x_right_bank[ut],
        ctx.transects.length_factor[ut],
        ctx.transects.x_factor[ut],
        ctx.transects.y_factor[ut]);
    for(int k=0;k<nsta;++k){auto uk=static_cast<size_t>(k);
    if(k%5==0)std::fprintf(f,"GR");
    std::fprintf(f," %10.4f %10.4f",ctx.transects.elevations[ut][uk],ctx.transects.stations[ut][uk]);
    if(k%5==4||k==nsta-1)std::fprintf(f,"\n");
    }}}

    // [STREETS]
    if(ctx.streets.count()>0){sec(f,"STREETS");
    std::fprintf(f,";;%-16s %-10s %-10s %-10s %-10s %-10s %-10s %-8s %-10s %-10s %-10s\n",
        "Name","Tcrown","Hcurb","Sx","nRoad","a","Wdep","Sides","Tback","Sback","nBack");
    for(int j=0;j<ctx.streets.count();++j){auto u=static_cast<size_t>(j);
    std::fprintf(f,"%-16s %10.4f %10.4f %10.4f %10.6f %10.4f %10.4f %8d %10.4f %10.4f %10.6f\n",
        ctx.streets.names[u].c_str(),ctx.streets.t_crown[u],ctx.streets.h_curb[u],
        ctx.streets.sx[u],ctx.streets.n_road[u],ctx.streets.gutter_depres[u],
        ctx.streets.gutter_width[u],ctx.streets.sides[u],
        ctx.streets.back_width[u],ctx.streets.back_slope[u],ctx.streets.back_n[u]);
    }}

    // [INLETS]
    if(ctx.inlets.count()>0){sec(f,"INLETS");
    for(int j=0;j<ctx.inlets.count();++j){auto u=static_cast<size_t>(j);
    std::fprintf(f,"%-16s %-12s %10.4f %10.4f",
        ctx.inlets.names[u].c_str(),ctx.inlets.inlet_type[u].c_str(),
        ctx.inlets.length[u],ctx.inlets.width[u]);
    if(!ctx.inlets.grate_type[u].empty())
        std::fprintf(f," %s",ctx.inlets.grate_type[u].c_str());
    if(ctx.inlets.open_area[u]>0)
        std::fprintf(f," %g",ctx.inlets.open_area[u]);
    if(ctx.inlets.splash_veloc[u]>0)
        std::fprintf(f," %g",ctx.inlets.splash_veloc[u]);
    std::fprintf(f,"\n");
    }}

    // [CONTROLS]
    if(ctx.control_rules.count()>0){sec(f,"CONTROLS");
    for(int j=0;j<ctx.control_rules.count();++j){
    std::fprintf(f,"%s\n",ctx.control_rules.rule_text[static_cast<size_t>(j)].c_str());
    }}

    // [REPORT]
    {sec(f,"REPORT");
    std::fprintf(f,";;%-16s %s\n","Keyword","Value");
    std::fprintf(f,"%-20s %s\n","DISABLED",ctx.options.rpt_disabled?"YES":"NO");
    std::fprintf(f,"%-20s %s\n","INPUT",ctx.options.rpt_input?"YES":"NO");
    std::fprintf(f,"%-20s %s\n","CONTINUITY",ctx.options.rpt_continuity?"YES":"NO");
    std::fprintf(f,"%-20s %s\n","FLOWSTATS",ctx.options.rpt_flowstats?"YES":"NO");
    std::fprintf(f,"%-20s %s\n","CONTROLS",ctx.options.rpt_controls?"YES":"NO");
    std::fprintf(f,"%-20s %s\n","AVERAGES",ctx.options.rpt_averages?"YES":"NO");
    if(ctx.options.rpt_subcatchments==0)std::fprintf(f,"%-20s %s\n","SUBCATCHMENTS","NONE");
    else if(ctx.options.rpt_subcatchments==1)std::fprintf(f,"%-20s %s\n","SUBCATCHMENTS","ALL");
    else for(const auto&n:ctx.options.rpt_subcatch_names)std::fprintf(f,"%-20s %s\n","SUBCATCHMENTS",n.c_str());
    if(ctx.options.rpt_nodes==0)std::fprintf(f,"%-20s %s\n","NODES","NONE");
    else if(ctx.options.rpt_nodes==1)std::fprintf(f,"%-20s %s\n","NODES","ALL");
    else for(const auto&n:ctx.options.rpt_node_names)std::fprintf(f,"%-20s %s\n","NODES",n.c_str());
    if(ctx.options.rpt_links==0)std::fprintf(f,"%-20s %s\n","LINKS","NONE");
    else if(ctx.options.rpt_links==1)std::fprintf(f,"%-20s %s\n","LINKS","ALL");
    else for(const auto&n:ctx.options.rpt_link_names)std::fprintf(f,"%-20s %s\n","LINKS",n.c_str());
    }

    // [POLLUTANTS]
    if(ctx.n_pollutants()>0){sec(f,"POLLUTANTS");
    std::fprintf(f,";;%-16s %-8s %-10s %-10s %-10s %-10s %-10s %-16s %-10s %-10s %-10s\n","Name","Units","Crain","Cgw","Crdii","Kdecay","SnowOnly","CoPollut","CoFrac","Cdwf","Cinit");
    std::fprintf(f,";;%-16s %-8s %-10s %-10s %-10s %-10s %-10s %-16s %-10s %-10s %-10s\n","----------------","--------","----------","----------","----------","----------","----------","----------------","----------","----------","----------");
    for(int p=0;p<ctx.n_pollutants();++p){auto u=static_cast<size_t>(p);
    write_obj_comment(f, ctx.pollutants.comments, u);
    const char*un="MG/L";if(ctx.pollutants.units[u]==MassUnits::UG_PER_L)un="UG/L";if(ctx.pollutants.units[u]==MassUnits::COUNTS_PER_L)un="#/L";
    std::fprintf(f,"%-16s %-8s %10.4f %10.4f %10.4f %10.4f %-10s %-16s %10.4f %10.4f %10.4f\n",pN(ctx,p),un,ctx.pollutants.c_rain[u],ctx.pollutants.c_gw[u],ctx.pollutants.c_rdii[u],ctx.pollutants.k_decay[u],ctx.pollutants.snow_only[u]?"YES":"NO",ctx.pollutants.co_pollut[u]>=0?pN(ctx,ctx.pollutants.co_pollut[u]):"*",ctx.pollutants.co_frac[u],ctx.pollutants.c_dwf[u],ctx.pollutants.init_conc[u]);
    }}

    // [LANDUSES]
    if(ctx.n_landuses()>0){sec(f,"LANDUSES");
    std::fprintf(f,";;%-16s %-12s %-12s %-12s\n","Name","SweepIntrvl","MaxRemoval","LastSwept");
    std::fprintf(f,";;%-16s %-12s %-12s %-12s\n","----------------","------------","------------","------------");
    for(int j=0;j<ctx.n_landuses();++j){auto u=static_cast<size_t>(j);
    write_obj_comment(f, ctx.landuses.comments, u);
    std::fprintf(f,"%-16s %12.2f %12.2f %12.2f\n",ctx.landuse_names.name_of(j).c_str(),
        ctx.landuses.sweep_interval[u],ctx.landuses.sweep_removal[u],ctx.landuses.last_swept[u]);
    }}

    // [BUILDUP]
    if(ctx.buildup.n_landuses>0&&ctx.buildup.n_pollutants>0){sec(f,"BUILDUP");
    std::fprintf(f,";;%-16s %-16s %-10s %-10s %-10s %-10s %-8s\n","LandUse","Pollutant","FuncType","Coeff1","Coeff2","Coeff3","PerUnit");
    std::fprintf(f,";;%-16s %-16s %-10s %-10s %-10s %-10s %-8s\n","----------------","----------------","----------","----------","----------","----------","--------");
    static const char* buNames[]={"NONE","POW","EXP","SAT","EXT"};
    for(int lu=0;lu<ctx.buildup.n_landuses;++lu){
    for(int p=0;p<ctx.buildup.n_pollutants;++p){
    auto idx=static_cast<size_t>(lu*ctx.buildup.n_pollutants+p);
    int ft=ctx.buildup.func_type[idx];
    if(ft==0)continue;
    std::fprintf(f,"%-16s %-16s %-10s %10.4f %10.4f %10.4f %-8s\n",
        ctx.landuse_names.name_of(lu).c_str(),pN(ctx,p),
        (ft>=0&&ft<=4)?buNames[ft]:"NONE",
        ctx.buildup.coeff1[idx],ctx.buildup.coeff2[idx],ctx.buildup.coeff3[idx],
        ctx.buildup.normalizer[idx]==0?"AREA":"CURB");
    }}}

    // [WASHOFF]
    if(ctx.washoff.n_landuses>0&&ctx.washoff.n_pollutants>0){sec(f,"WASHOFF");
    std::fprintf(f,";;%-16s %-16s %-10s %-10s %-10s %-10s %-10s\n","LandUse","Pollutant","FuncType","Coeff","Expon","SweepEff","BmpEff");
    std::fprintf(f,";;%-16s %-16s %-10s %-10s %-10s %-10s %-10s\n","----------------","----------------","----------","----------","----------","----------","----------");
    static const char* woNames[]={"NONE","EXP","RC","EMC"};
    for(int lu=0;lu<ctx.washoff.n_landuses;++lu){
    for(int p=0;p<ctx.washoff.n_pollutants;++p){
    auto idx=static_cast<size_t>(lu*ctx.washoff.n_pollutants+p);
    int ft=ctx.washoff.func_type[idx];
    if(ft==0)continue;
    std::fprintf(f,"%-16s %-16s %-10s %10.4f %10.4f %10.2f %10.2f\n",
        ctx.landuse_names.name_of(lu).c_str(),pN(ctx,p),
        (ft>=0&&ft<=3)?woNames[ft]:"NONE",
        ctx.washoff.coeff[idx],ctx.washoff.expon[idx],
        ctx.washoff.sweep_effic[idx],ctx.washoff.bmp_effic[idx]);
    }}}

    // [TREATMENT]
    if(ctx.treatment.hasAny()){sec(f,"TREATMENT");
    std::fprintf(f,";;%-16s %-16s %s\n","Node","Pollutant","Function");
    std::fprintf(f,";;%-16s %-16s\n","----------------","----------------");
    for(int n=0;n<ctx.treatment.n_nodes;++n){
    for(int p=0;p<ctx.treatment.n_pollutants;++p){
    auto idx=static_cast<size_t>(n*ctx.treatment.n_pollutants+p);
    if(ctx.treatment.expressions[idx].empty())continue;
    std::fprintf(f,"%-16s %-16s %s\n",nN(ctx,n),pN(ctx,p),ctx.treatment.expressions[idx].c_str());
    }}}

    // [INFLOWS]
    if(ctx.ext_inflows.count()>0){sec(f,"INFLOWS");
    std::fprintf(f,";;%-16s %-16s %-16s %-16s %-10s %-10s %-10s %-16s\n",
        "Node","Constituent","TimeSeries","Type","Mfactor","Sfactor","Baseline","Pattern");
    std::fprintf(f,";;%-16s %-16s %-16s %-16s %-10s %-10s %-10s %-16s\n",
        "----------------","----------------","----------------","----------------","----------","----------","----------","----------------");
    for(int j=0;j<ctx.ext_inflows.count();++j){auto u=static_cast<size_t>(j);
    std::fprintf(f,"%-16s %-16s %-16s %-16s %10.4f %10.4f %10.4f %-16s\n",
        nN(ctx,ctx.ext_inflows.node_idx[u]),
        ctx.ext_inflows.constituent[u].c_str(),
        ctx.ext_inflows.ts_name[u].empty()?"\"\"":ctx.ext_inflows.ts_name[u].c_str(),
        ctx.ext_inflows.inflow_type[u].c_str(),
        ctx.ext_inflows.m_factor[u],ctx.ext_inflows.s_factor[u],
        ctx.ext_inflows.baseline[u],
        ctx.ext_inflows.pattern_name[u].empty()?"":ctx.ext_inflows.pattern_name[u].c_str());
    }}

    // [DWF]
    if(ctx.dwf_inflows.count()>0){sec(f,"DWF");
    std::fprintf(f,";;%-16s %-16s %-12s %-16s %-16s %-16s %-16s\n",
        "Node","Constituent","AvgValue","Pat1","Pat2","Pat3","Pat4");
    std::fprintf(f,";;%-16s %-16s %-12s %-16s %-16s %-16s %-16s\n",
        "----------------","----------------","------------","----------------","----------------","----------------","----------------");
    for(int j=0;j<ctx.dwf_inflows.count();++j){auto u=static_cast<size_t>(j);
    std::fprintf(f,"%-16s %-16s %12.6f",
        nN(ctx,ctx.dwf_inflows.node_idx[u]),
        ctx.dwf_inflows.constituent[u].c_str(),
        ctx.dwf_inflows.avg_value[u]);
    if(!ctx.dwf_inflows.pat1[u].empty())std::fprintf(f," %-16s",ctx.dwf_inflows.pat1[u].c_str());
    if(!ctx.dwf_inflows.pat2[u].empty())std::fprintf(f," %-16s",ctx.dwf_inflows.pat2[u].c_str());
    if(!ctx.dwf_inflows.pat3[u].empty())std::fprintf(f," %-16s",ctx.dwf_inflows.pat3[u].c_str());
    if(!ctx.dwf_inflows.pat4[u].empty())std::fprintf(f," %-16s",ctx.dwf_inflows.pat4[u].c_str());
    std::fprintf(f,"\n");
    }}

    // [RDII]
    if(ctx.rdii_assigns.count()>0){sec(f,"RDII");
    std::fprintf(f,";;%-16s %-16s %-12s\n","Node","UnitHyd","SewerArea");
    std::fprintf(f,";;%-16s %-16s %-12s\n","----------------","----------------","------------");
    for(int j=0;j<ctx.rdii_assigns.count();++j){auto u=static_cast<size_t>(j);
    std::fprintf(f,"%-16s %-16s %12.4f\n",
        nN(ctx,ctx.rdii_assigns.node_idx[u]),
        ctx.rdii_assigns.uh_name[u].c_str(),
        ctx.rdii_assigns.sewer_area[u]);
    }}

    // [HYDROGRAPHS]
    if(ctx.unit_hyds.count()>0||!ctx.unit_hyds.gage_assignments.empty()){
    sec(f,"HYDROGRAPHS");
    std::fprintf(f,";;%-16s %-16s %-8s %-8s %-8s %-8s %-8s %-8s %-8s\n",
        "Name","Month/Gage","Response","R","T","K","Dmax","Drecov","Dinit");
    std::fprintf(f,";;%-16s %-16s %-8s %-8s %-8s %-8s %-8s %-8s %-8s\n",
        "----------------","----------------","--------","--------","--------","--------","--------","--------","--------");
    // Gage assignment lines
    for(size_t i=0;i<ctx.unit_hyds.gage_assignments.size();++i){
    std::fprintf(f,"%-16s %s\n",
        ctx.unit_hyds.gage_assignments[i].c_str(),
        ctx.unit_hyds.gage_names[i].c_str());
    }
    // Parameter lines
    static const char* uhMonths[]={"JAN","FEB","MAR","APR","MAY","JUN",
                                   "JUL","AUG","SEP","OCT","NOV","DEC"};
    static const char* uhResp[]={"SHORT","MEDIUM","LONG"};
    for(const auto& e:ctx.unit_hyds.entries){
    const char* mon=(e.month<0)?"ALL":uhMonths[e.month];
    std::fprintf(f,"%-16s %-9s %-8s %8.4f %8.4f %8.4f",
        e.name.c_str(),mon,uhResp[e.response],e.r,e.t,e.k);
    if(e.dmax>0.0||e.drecov>0.0||e.dinit>0.0)
        std::fprintf(f," %8.4f %8.4f %8.4f",e.dmax,e.drecov,e.dinit);
    std::fprintf(f,"\n");
    }}

    // [RDII_DECAY] (exponential IA decay parameters)
    if(ctx.rdii_decay.count()>0){sec(f,"RDII_DECAY");
    std::fprintf(f,";;%-16s %-8s %-10s %-10s %-10s %-8s %-10s %-10s\n",
        "UHGroup","Response","k_dep","k_0","k_T","T_ref","theta_rec","T_freeze");
    std::fprintf(f,";;%-16s %-8s %-10s %-10s %-10s %-8s %-10s %-10s\n",
        "----------------","--------","----------","----------","----------",
        "--------","----------","----------");
    static const char* decayResp[]={"SHORT","MEDIUM","LONG"};
    for(const auto& e:ctx.rdii_decay.entries){
    const char* r=(e.response>=0&&e.response<=2)?decayResp[e.response]:"SHORT";
    std::fprintf(f,"%-16s %-8s %10.5f %10.5f %10.5f %8.2f %10.5f %10.5f\n",
        e.uh_name.c_str(),r,
        e.k_dep,e.k_0,e.k_T,e.T_ref,e.theta_rec,e.T_freeze);
    }}

    // [PATTERNS]
    if(ctx.patterns.count()>0){sec(f,"PATTERNS");
    std::fprintf(f,";;%-16s %-12s\n","Name","Type");
    std::fprintf(f,";;%-16s %-12s\n","----------------","------------");
    static const char* patNames[]={"MONTHLY","DAILY","HOURLY","WEEKEND"};
    for(int j=0;j<ctx.patterns.count();++j){auto u=static_cast<size_t>(j);
    int pt=ctx.patterns.types[u];
    const char* ptn=(pt>=0&&pt<=3)?patNames[pt]:"MONTHLY";
    const auto& facs=ctx.patterns.factors[u];
    // First line: name + type + first batch of values
    std::fprintf(f,"%-16s %-12s",ctx.patterns.names[u].c_str(),ptn);
    for(size_t k=0;k<facs.size()&&k<6;++k)std::fprintf(f," %10.4f",facs[k]);
    std::fprintf(f,"\n");
    // Continuation lines (6 values per line)
    for(size_t k=6;k<facs.size();k+=6){
    std::fprintf(f,"%-16s            ",ctx.patterns.names[u].c_str());
    for(size_t m=k;m<facs.size()&&m<k+6;++m)std::fprintf(f," %10.4f",facs[m]);
    std::fprintf(f,"\n");
    }}}

    // [TIMESERIES]
    {bool has=false;for(const auto&t:ctx.tables.tables)if(t.type==TableType::TIMESERIES){has=true;break;}
    if(has){sec(f,"TIMESERIES");
    std::fprintf(f,";;%-16s %-20s %-12s\n","Name","Date/Time","Value");
    std::fprintf(f,";;%-16s %-20s %-12s\n","----------------","--------------------","------------");
    for(int t=0;t<static_cast<int>(ctx.tables.tables.size());++t){const auto&tb=ctx.tables.tables[static_cast<size_t>(t)];
    if(tb.type!=TableType::TIMESERIES)continue;
    if(!tb.comment.empty()){
        const char*sep="\\n";std::size_t s=0;
        while(s<=tb.comment.size()){std::size_t e=tb.comment.find(sep,s);
        if(e==std::string::npos)e=tb.comment.size();
        std::fprintf(f,";%.*s\n",static_cast<int>(e-s),tb.comment.data()+s);
        if(e==tb.comment.size())break;s=e+2;}
    }
    // File-backed series — preserve the FILE reference rather than dumping
    // the in-memory cache of rows. The path token (possibly carrying a
    // `:column` suffix) is stored verbatim by TablesHandler; Slice IO-4
    // rebases the path portion relative to the destination directory.
    if(!tb.file_path.empty()){
        const std::string tok = emit_path_token(tb.file_path, dst_dir,
                                                 force_abs_paths, warnings);
        std::fprintf(f,"%-16s FILE         \"%s\"\n",tN(ctx,t),tok.c_str());
        continue;
    }
    // PostParseResolver offsets relative time series by start_date so the
    // engine can do absolute OADate lookups.  Detect this using the same
    // condition it uses (x[0] - start_date < 366) and strip the offset
    // before writing so the output matches the original time-only format.
    const double startDate = ctx.options.start_date;
    const double x0 = tb.x.empty() ? 0.0 : tb.x.front();
    const bool wasRelative = (x0 - startDate) >= 0.0 && (x0 - startDate) < 366.0;
    // A true absolute series (calendar dates entered by the user) has x values
    // that are large OADates even before any start_date offset would be added
    // — i.e. x0 is already >> 3650 regardless of start_date.
    const bool isAbsolute  = !wasRelative && x0 >= 3650.0;

    char dateBuf[16], timeBuf[12];
    for(size_t k=0;k<tb.x.size();++k){
        const double xv = wasRelative ? (tb.x[k] - startDate) : tb.x[k];
        if(isAbsolute){
            fmt_date(dateBuf, xv);
            fmt_time(timeBuf, xv);
            std::fprintf(f,"%-16s %-12s %-8s %12.6f\n",tN(ctx,t),dateBuf,timeBuf,tb.y[k]);
        } else {
            // Relative: convert fractional days → HH:MM; allow hours > 23.
            const double totalHours = xv * 24.0;
            const int hh = static_cast<int>(totalHours);
            const int mm = static_cast<int>((totalHours - hh) * 60.0 + 0.5);
            std::fprintf(f,"%-16s %d:%02d %12.6f\n",tN(ctx,t),hh,mm,tb.y[k]);
        }
    }
    }}}

    // [CURVES]
    {bool has=false;for(const auto&t:ctx.tables.tables)if(t.type!=TableType::TIMESERIES){has=true;break;}
    if(has){sec(f,"CURVES");
    std::fprintf(f,";;%-16s %-12s %-12s %-12s\n","Name","Type","X-Value","Y-Value");
    std::fprintf(f,";;%-16s %-12s %-12s %-12s\n","----------------","------------","------------","------------");
    for(int t=0;t<static_cast<int>(ctx.tables.tables.size());++t){const auto&tb=ctx.tables.tables[static_cast<size_t>(t)];
    if(tb.type==TableType::TIMESERIES)continue;
    auto lbl_it=CURVE_TYPE_LABEL.find(static_cast<int>(tb.type));
    const char* lbl=lbl_it!=CURVE_TYPE_LABEL.end()?lbl_it->second:"STORAGE";
    if(!tb.comment.empty()){
        const char*sep="\\n";std::size_t s=0;
        while(s<=tb.comment.size()){std::size_t e=tb.comment.find(sep,s);
        if(e==std::string::npos)e=tb.comment.size();
        std::fprintf(f,";%.*s\n",static_cast<int>(e-s),tb.comment.data()+s);
        if(e==tb.comment.size())break;s=e+2;}
    }
    for(size_t k=0;k<tb.x.size();++k)std::fprintf(f,"%-16s %-12s %12.6f %12.6f\n",tN(ctx,t),lbl,tb.x[k],tb.y[k]);
    }}}

    // Geospatial block — section order matches the legacy SWMM GUI ExportMap():
    //   [MAP]  →  [COORDINATES]  →  [VERTICES]  →  [Polygons]  →  [SYMBOLS]
    //
    // [MAP] — always written when any spatial data exists, matching legacy GUI
    // behaviour of always serialising map dimensions and units.
    {
    const bool has_nodes   = !ctx.spatial.node_x.empty();
    bool has_verts = false;
    for(const auto& vx:ctx.spatial.link_vertices_x) if(!vx.empty()){has_verts=true;break;}
    bool has_polys = false;
    for(const auto& px:ctx.spatial.subcatch_polygon_x) if(!px.empty()){has_polys=true;break;}
    const bool has_gages   = !ctx.spatial.gage_x.empty();
    const bool has_spatial = has_nodes||has_verts||has_polys||has_gages
                             ||ctx.spatial.map_x2!=0.0||ctx.spatial.map_y2!=0.0
                             ||!ctx.spatial.map_units.empty();

    if(has_spatial){
    // [MAP] first — legacy writes this before coordinates
    sec(f,"MAP");
    std::fprintf(f,"DIMENSIONS %-18.4f %-18.4f %-18.4f %-18.4f\n",
        ctx.spatial.map_x1, ctx.spatial.map_y1,
        ctx.spatial.map_x2, ctx.spatial.map_y2);
    // "Units" (mixed case) matches legacy GUI keyword exactly.
    // Always written; defaults to "None" when unspecified.
    const char* map_units = ctx.spatial.map_units.empty()
                            ? "None" : ctx.spatial.map_units.c_str();
    std::fprintf(f,"Units      %s\n", map_units);
    }

    // [COORDINATES] — all nodes (junctions, outfalls, dividers, storage)
    // Column layout: %-16s name | %-18.4f X | %-18.4f Y  (matches legacy Fmt)
    if(has_nodes){sec(f,"COORDINATES");
    std::fprintf(f,";;%-14s %-18s %-18s\n","Node","X-Coord","Y-Coord");
    std::fprintf(f,";;%-14s %-18s %-18s\n","--------------","------------------","------------------");
    for(int j=0;j<ctx.n_nodes();++j){auto u=static_cast<size_t>(j);
    if(u<ctx.spatial.node_x.size())
        std::fprintf(f,"%-16s %-18.4f %-18.4f\n",
            ctx.node_names.name_of(j).c_str(),
            ctx.spatial.node_x[u], ctx.spatial.node_y[u]);
    }}

    // [VERTICES] — link polyline interior vertices (conduits through outlets)
    if(has_verts){sec(f,"VERTICES");
    std::fprintf(f,";;%-14s %-18s %-18s\n","Link","X-Coord","Y-Coord");
    std::fprintf(f,";;%-14s %-18s %-18s\n","--------------","------------------","------------------");
    for(int j=0;j<ctx.n_links();++j){auto u=static_cast<size_t>(j);
    if(u>=ctx.spatial.link_vertices_x.size())continue;
    const auto& vx=ctx.spatial.link_vertices_x[u];
    const auto& vy=ctx.spatial.link_vertices_y[u];
    for(size_t k=0;k<vx.size();++k)
        std::fprintf(f,"%-16s %-18.4f %-18.4f\n",
            ctx.link_names.name_of(j).c_str(), vx[k], vy[k]);
    }}

    // [Polygons] — subcatchment boundary polygons.
    // Mixed-case tag matches legacy SWMM GUI (readers are case-insensitive).
    // Legacy also appends storage-node polygons under the same section tag;
    // storage polygon data is not yet modelled in SpatialFrame.
    if(has_polys){std::fprintf(f,"\n[Polygons]\n");
    std::fprintf(f,";;%-14s %-18s %-18s\n","Subcatchment","X-Coord","Y-Coord");
    std::fprintf(f,";;%-14s %-18s %-18s\n","--------------","------------------","------------------");
    for(int j=0;j<ctx.n_subcatches();++j){auto u=static_cast<size_t>(j);
    if(u>=ctx.spatial.subcatch_polygon_x.size())continue;
    const auto& px=ctx.spatial.subcatch_polygon_x[u];
    const auto& py=ctx.spatial.subcatch_polygon_y[u];
    for(size_t k=0;k<px.size();++k)
        std::fprintf(f,"%-16s %-18.4f %-18.4f\n",
            ctx.subcatch_names.name_of(j).c_str(), px[k], py[k]);
    }}

    // [SYMBOLS] — rain gage symbol coordinates
    if(has_gages){sec(f,"SYMBOLS");
    std::fprintf(f,";;%-14s %-18s %-18s\n","Gage","X-Coord","Y-Coord");
    std::fprintf(f,";;%-14s %-18s %-18s\n","--------------","------------------","------------------");
    for(int j=0;j<ctx.n_gages();++j){auto u=static_cast<size_t>(j);
    if(u<ctx.spatial.gage_x.size())
        std::fprintf(f,"%-16s %-18.4f %-18.4f\n",
            ctx.gage_names.name_of(j).c_str(),
            ctx.spatial.gage_x[u], ctx.spatial.gage_y[u]);
    }}
    }

    // [TAGS] — per-object free-form labels. Tags are stored per-SoA
    // index (NodeData::tags / LinkData::tags / SubcatchData::tags) so
    // they survive swmm_*_rename. Only objects with a non-empty tag
    // are emitted; section is skipped entirely if nothing's tagged.
    {
        auto has_any = [](const std::vector<std::string>& v){
            for (const auto& s : v) if (!s.empty()) return true;
            return false;
        };
        const bool any =
            has_any(ctx.nodes.tags) ||
            has_any(ctx.links.tags) ||
            has_any(ctx.subcatches.tags);
        if (any) {
            sec(f,"TAGS");
            std::fprintf(f,";;%-10s %-16s %s\n","Type","Name","Tag");
            std::fprintf(f,";;%-10s %-16s %s\n","----------","----------------","---");
            for (int j = 0; j < ctx.n_nodes(); ++j) {
                const auto u = static_cast<size_t>(j);
                if (u < ctx.nodes.tags.size() && !ctx.nodes.tags[u].empty())
                    std::fprintf(f,"%-10s %-16s %s\n", "Node",
                        ctx.node_names.name_of(j).c_str(),
                        ctx.nodes.tags[u].c_str());
            }
            for (int j = 0; j < ctx.n_links(); ++j) {
                const auto u = static_cast<size_t>(j);
                if (u < ctx.links.tags.size() && !ctx.links.tags[u].empty())
                    std::fprintf(f,"%-10s %-16s %s\n", "Link",
                        ctx.link_names.name_of(j).c_str(),
                        ctx.links.tags[u].c_str());
            }
            for (int j = 0; j < ctx.n_subcatches(); ++j) {
                const auto u = static_cast<size_t>(j);
                if (u < ctx.subcatches.tags.size() && !ctx.subcatches.tags[u].empty())
                    std::fprintf(f,"%-10s %-16s %s\n", "Subcatch",
                        ctx.subcatch_names.name_of(j).c_str(),
                        ctx.subcatches.tags[u].c_str());
            }
        }
    }

    // [USER_FLAGS]
    if(ctx.user_flags.def_count()>0){sec(f,"USER_FLAGS");
    std::fprintf(f,";;%-20s %-10s %s\n","Name","Type","Description");
    std::fprintf(f,";;%-20s %-10s\n","--------------------","----------");
    for(const auto&d:ctx.user_flags.all_defs()){
    const char*ts="BOOLEAN";switch(d.type){case UserFlagType::INTEGER:ts="INTEGER";break;case UserFlagType::REAL:ts="REAL";break;case UserFlagType::STRING:ts="STRING";break;default:break;}
    if(d.description.empty())std::fprintf(f,"%-20s %-10s\n",d.name.c_str(),ts);
    else std::fprintf(f,"%-20s %-10s \"%s\"\n",d.name.c_str(),ts,d.description.c_str());
    }}

    // [USER_FLAG_VALUES]
    if(ctx.user_flags.value_count()>0){sec(f,"USER_FLAG_VALUES");
    std::fprintf(f,";;%-14s %-16s %-20s %s\n","ObjectType","ObjectName","FlagName","Value");
    std::fprintf(f,";;%-14s %-16s %-20s\n","--------------","----------------","--------------------");
    for(const auto&kv:ctx.user_flags.all_values()){const auto&k=kv.first;
    auto p1=k.find(':');if(p1==std::string::npos)continue;auto p2=k.find(':',p1+1);if(p2==std::string::npos)continue;
    std::fprintf(f,"%-14s %-16s %-20s ",k.substr(0,p1).c_str(),k.substr(p1+1,p2-p1-1).c_str(),k.substr(p2+1).c_str());
    const auto&v=kv.second;
    if(std::holds_alternative<bool>(v))std::fprintf(f,"%s\n",std::get<bool>(v)?"YES":"NO");
    else if(std::holds_alternative<int>(v))std::fprintf(f,"%d\n",std::get<int>(v));
    else if(std::holds_alternative<double>(v))std::fprintf(f,"%g\n",std::get<double>(v));
    else if(std::holds_alternative<std::string>(v)){const auto&s=std::get<std::string>(v);
    if(s.find(' ')!=std::string::npos)std::fprintf(f,"\"%s\"\n",s.c_str());else std::fprintf(f,"%s\n",s.c_str());}
    }}

    // [FILES] — secondary file references (rainfall / runoff / RDII /
    // inflows / outflows / hot-start save & use).  Mode → keyword
    // mapping mirrors the legacy parser. Slice IO-4: every path token
    // is rebased relative to the destination directory (unless
    // WRITE_ABSOLUTE_PATHS is set).
    if (ctx.files.has_any()) {
        sec(f, "FILES");
        std::fprintf(f, ";;%-12s %-10s %s\n", "Mode", "FileType", "Path");
        auto mode_word = [](FileMode m) {
            return m == FileMode::SAVE ? "SAVE" :
                   m == FileMode::USE  ? "USE"  : "";
        };
        auto write_pair = [&](FileMode mode, const char* kind,
                               const FilePathPair& slot) {
            if (mode == FileMode::NONE || slot.empty()) return;
            const std::string tok = emit_path_token(slot, dst_dir,
                                                     force_abs_paths, warnings);
            std::fprintf(f, "%-13s %-10s \"%s\"\n",
                          mode_word(mode), kind, tok.c_str());
        };
        write_pair(ctx.files.rainfall_mode, "RAINFALL", ctx.files.rainfall_path);
        write_pair(ctx.files.runoff_mode,   "RUNOFF",   ctx.files.runoff_path);
        write_pair(ctx.files.rdii_mode,     "RDII",     ctx.files.rdii_path);
        if (!ctx.files.inflows_path.empty()) {
            const std::string tok = emit_path_token(ctx.files.inflows_path,
                                                     dst_dir, force_abs_paths, warnings);
            std::fprintf(f, "%-13s %-10s \"%s\"\n", "USE",  "INFLOWS", tok.c_str());
        }
        if (!ctx.files.outflows_path.empty()) {
            const std::string tok = emit_path_token(ctx.files.outflows_path,
                                                     dst_dir, force_abs_paths, warnings);
            std::fprintf(f, "%-13s %-10s \"%s\"\n", "SAVE", "OUTFLOWS", tok.c_str());
        }
        if (!ctx.files.hotstart_use_path.empty()) {
            const std::string tok = emit_path_token(ctx.files.hotstart_use_path,
                                                     dst_dir, force_abs_paths, warnings);
            std::fprintf(f, "%-13s %-10s \"%s\"\n", "USE", "HOTSTART", tok.c_str());
        }
        for (const auto &save : ctx.files.hotstart_saves) {
            if (save.path.empty()) continue;
            const std::string tok = emit_path_token(save.path, dst_dir,
                                                     force_abs_paths, warnings);
            if (save.datetime > 0.0) {
                char date_buf[16], time_buf[16];
                fmt_date(date_buf, save.datetime);
                fmt_time(time_buf, save.datetime);
                std::fprintf(f, "%-13s %-10s \"%s\" %s %s\n",
                              "SAVE", "HOTSTART",
                              tok.c_str(),
                              date_buf, time_buf);
            } else {
                std::fprintf(f, "%-13s %-10s \"%s\"\n", "SAVE", "HOTSTART",
                              tok.c_str());
            }
        }
    }

    // [PLUGINS]
    if(!ctx.plugin_specs.empty()){sec(f,"PLUGINS");
    for(const auto&ps:ctx.plugin_specs){std::fprintf(f,"%s",ps.path.c_str());
    for(const auto&a:ps.init_args)std::fprintf(f," %s",a.c_str());std::fprintf(f,"\n");
    }}

    // [2D_*] — 2D surface-routing model definition (no-op for 1D models
    // and for engine builds without the 2D module).
    write2DSections(f, ctx, dst_dir, force_abs_paths, warnings);

    std::fclose(f);
    return 0;
}

} // namespace inp_writer
} // namespace openswmm
