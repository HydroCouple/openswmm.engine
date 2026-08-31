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
 *   AQUIFERS, GROUNDWATER, GWF,
 *   JUNCTIONS, OUTFALLS, DIVIDERS, STORAGE, CONDUITS, PUMPS, ORIFICES,
 *   WEIRS, OUTLETS, XSECTIONS, LOSSES, TRANSECTS, STREETS, INLETS,
 *   INLET_USAGE,
 *   CONTROLS, REPORT, POLLUTANTS, LANDUSES, COVERAGES, BUILDUP, WASHOFF,
 *   LOADINGS, TREATMENT,
 *   INFLOWS, DWF, RDII, PATTERNS, TIMESERIES, CURVES,
 *   MAP, COORDINATES, VERTICES, Polygons, SYMBOLS,
 *   USER_FLAGS, USER_FLAG_VALUES, PLUGINS,
 *   2D_OPTIONS, 2D_INFILTRATION_OPTIONS, 2D_INFILTRATION_DEFAULTS,
 *   2D_INFILTRATION, 2D_MESH_FILE (external mode) or 2D_VERTICES,
 *   2D_TRIANGLES, 2D_VERTEX_NODE_MAP, 2D_TRIANGLE_NODE_MAP,
 *   2D_BOUNDARY_CONDITIONS, 2D_EDGE_CONVEYANCE (inline mode)
 *
 * @ingroup engine_core
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "InpWriter.hpp"
#include "PathResolver.hpp"
#include "SimulationContext.hpp"
#include "../data/StorageGeometry.hpp"
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

#ifdef OPENSWMM_HAS_2D
// Unlike the 2D data headers above, Infil2D's grammar helpers
// (infil2DMethodToken / infil2DDestToken / infil2DParamCount) are defined in
// 2d/infil/Infil2D.cpp, which is compiled only when the 2D module is built —
// so the [2D_INFILTRATION*] emission is guarded rather than runtime-checked.
#include "../2d/infil/Infil2D.hpp"
#endif

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

// IO3a: the component save hook — each component writes its own config file.
#include "../plugins/ProcessComponentRegistry.hpp"
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
static const char* tN(const SimulationContext& c, int i) {
    return (i>=0 && i<c.n_tables()) ? c.tables[i].id.c_str() : "*";
}
static const char* pN(const SimulationContext& c, int i) {
    return (i>=0 && i<c.n_pollutants()) ? c.pollutant_names.name_of(i).c_str() : "*";
}

// ---------------------------------------------------------------------------
// Index-or-name accessors.
//
// Several model fields keep BOTH a resolved index and the raw name parsed from
// the .inp (for deferred resolution). Writing the index alone emits '*' when it
// is -1, which either fails reload with ERR_NAME (209) or silently drops the
// reference. These helpers fall back to the retained name before giving up.
// All returned pointers alias stable members of the context.
// ---------------------------------------------------------------------------

// [SUBCATCHMENTS] Outlet: a node, another subcatchment, or the raw name.
// A subcatch draining to another subcatch (or itself) has outlet_node == -1,
// so nN() alone would emit '*' and fail reload with ERR_NAME (209).
static const char* oN(const SimulationContext& c, size_t u) {
    const int n = c.subcatches.outlet_node[u];
    if (n>=0 && n<c.n_nodes())      return c.node_names.name_of(n).c_str();
    const int s = c.subcatches.outlet_subcatch[u];
    if (s>=0 && s<c.n_subcatches()) return c.subcatch_names.name_of(s).c_str();
    const std::string& nm = c.subcatches.outlet_name[u];
    return nm.empty() ? "*" : nm.c_str();
}

// [SUBCATCHMENTS] RainGage: gage index, else the retained gage name.
// An unresolved gage is also a fatal ERR_NAME (209) on reload.
static const char* sgN(const SimulationContext& c, size_t u) {
    const int g = c.subcatches.gage[u];
    if (g>=0 && g<c.n_gages()) return c.gage_names.name_of(g).c_str();
    if (u < c.subcatches.gage_name.size() && !c.subcatches.gage_name[u].empty())
        return c.subcatches.gage_name[u].c_str();
    return "*";
}

// [SUBCATCHMENTS] Snowpack: pack index, else the retained pack name.
// '*' is a legal positional placeholder here (CatchmentHandler), so an
// unresolved name is data loss rather than a reload failure.
static const char* sspN(const SimulationContext& c, size_t u) {
    const int s = c.subcatches.snowpack[u];
    if (s>=0 && s<static_cast<int>(c.snowpack_names.size()))
        return c.snowpack_names.name_of(s).c_str();
    if (u < c.subcatches.snowpack_name.size() &&
        !c.subcatches.snowpack_name[u].empty())
        return c.subcatches.snowpack_name[u].c_str();
    return "*";
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
// Virtual junctions are JUNCTION-typed but emit into [VIRTUAL_JUNCTIONS], not [JUNCTIONS].
static bool isVirtualNode(const SimulationContext& c, size_t u) {
    return u < c.nodes.is_virtual.size() && c.nodes.is_virtual[u] != 0;
}
static bool hasRegularJunction(const SimulationContext& c) {
    for(int j=0;j<c.n_nodes();++j){auto u=static_cast<size_t>(j);
        if(c.nodes.type[u]==NodeType::JUNCTION && !isVirtualNode(c,u)) return true;}
    return false;
}
static bool hasVirtualJunction(const SimulationContext& c) {
    for(int j=0;j<c.n_nodes();++j) if(isVirtualNode(c,static_cast<size_t>(j))) return true;
    return false;
}
// True when any virtual junction carries a rendering rim depth, which is what
// widens [VIRTUAL_JUNCTIONS] to its optional third column. Models without one
// keep writing the two-column section byte-for-byte.
static bool hasVirtualJunctionRim(const SimulationContext& c) {
    for(int j=0;j<c.n_nodes();++j){auto u=static_cast<size_t>(j);
        if(isVirtualNode(c,u) && u<c.nodes.rim_depth.size() && c.nodes.rim_depth[u]>0.0) return true;}
    return false;
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

#ifdef OPENSWMM_HAS_2D
// ---- [2D_INFILTRATION_OPTIONS] / _DEFAULTS / [2D_INFILTRATION] (plan §5.5) --
//
// §5.5.5: these are per-cell mesh attributes, so they FOLLOW THE MESH exactly
// as [2D_VERTICES]/[2D_TRIANGLES] do — into the .2dm sidecar in external-mesh
// mode, into the .inp when the mesh is inline. This function is therefore
// called from exactly one place, the tail of emit2DMeshSections(), which is
// itself invoked once per save against one destination stream; double emission
// is impossible by construction rather than being resolved by read-side
// precedence. Emitting into both files would be actively wrong:
// load2DMeshExternalFile applies SIDECAR-WINS-PER-SECTION, so an .inp copy of
// a section the .2dm also carries is silently discarded on reload.
//
// [2D_TRIANGLES] is deliberately NOT extended with infiltration columns
// (§5.5.3): its columns are positional (V1 V2 V3 MANNINGS_N [INIT_DEPTH]
// [TAG], a numeric 5th token meaning INIT_DEPTH) and appending would break
// hand-authored meshes.
static void emit2DInfilSections(FILE* f, const SimulationContext& ctx) {
    const twoD::Infil2D* infil = ctx.twod_io.infil;
    if (!infil) return;

    // METHOD [P1..Pn] DEST. Parameter columns are POSITIONAL and in PROJECT
    // UNITS (stored verbatim; Infil2D::resolve converts). Trailing columns the
    // method does not use are trimmed with infil2DParamCount(); the one
    // interior no-op in the legacy [INFILTRATION] layout — CURVE_NUMBER's
    // middle column, see the table on Infil2DRow — is written as "-" so the
    // columns after it keep their positions.
    auto emit_row = [f](const twoD::Infil2DRow& row) {
        std::fprintf(f, " %-20s", twoD::infil2DMethodToken(row));
        if (!row.has_method) { std::fprintf(f, "\n"); return; }
        const int np = twoD::infil2DParamCount(row.method);
        for (int k = 0; k < np && k < twoD::kInfil2DMaxParams; ++k) {
            if (row.method == InfilModel::CURVE_NUM && k == 1)
                std::fprintf(f, " %-12s", "-");
            else
                std::fprintf(f, " %-12.12g", row.p[k]);
        }
        std::fprintf(f, " %s\n", twoD::infil2DDestToken(row.dest));
    };

    if (infil->options().infil_step > 0.0) {
        char sb[32];
        fmt_step(sb, infil->options().infil_step);
        sec(f, "2D_INFILTRATION_OPTIONS");
        std::fprintf(f, ";;%-20s %s\n", "Parameter", "Value");
        std::fprintf(f, "%-22s %s\n", "INFIL_STEP", sb);
    }

    if (!infil->defaults().empty()) {
        sec(f, "2D_INFILTRATION_DEFAULTS");
        std::fprintf(f, ";;%-14s %-20s %-12s %-12s %-12s %-12s %-12s %s\n",
                     "TAG", "METHOD", "P1", "P2", "P3", "P4", "P5", "DEST");
        for (const auto& d : infil->defaults()) {
            std::fprintf(f, "%-16s", d.tag.c_str());
            emit_row(d.row);
        }
    }

    if (!infil->overrides().empty()) {
        sec(f, "2D_INFILTRATION");
        std::fprintf(f, ";;%-14s %-20s %-12s %-12s %-12s %-12s %-12s %s\n",
                     "CELL", "METHOD", "P1", "P2", "P3", "P4", "P5", "DEST");
        for (const auto& o : infil->overrides()) {
            // CELL is 1-BASED in the file (tri is 0-based internally).
            std::fprintf(f, "%-16d", o.tri + 1);
            emit_row(o.row);
        }
    }
}
#endif // OPENSWMM_HAS_2D

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
    static const char* sRainMode[]  = {"NATURAL_NEIGHBOUR", "SYSTEM", "NONE"};
    sec(f, "2D_OPTIONS");
    std::fprintf(f, ";;%-20s %s\n", "Parameter", "Value");
    std::fprintf(f, "%-22s %.12g\n", "MAX_TIMESTEP",      o.max_timestep);
    std::fprintf(f, "%-22s %.12g\n", "DRY_DEPTH",         o.dry_depth);
    std::fprintf(f, "%-22s %.12g\n", "LIMITER_EPSILON",   o.limiter_epsilon);
    std::fprintf(f, "%-22s %.12g\n", "FLUX_DH_EPS",       o.flux_dh_eps);
    std::fprintf(f, "%-22s %.12g\n", "COUPLING_CD",       o.coupling_cd);
    std::fprintf(f, "%-22s %.12g\n", "COUPLING_SYNC",     o.coupling_sync);
    std::fprintf(f, "%-22s %s\n",    "RAINFALL_MODE",
                 sRainMode[static_cast<int>(o.rainfall_mode) >= 0 &&
                           static_cast<int>(o.rainfall_mode) <= 2
                               ? static_cast<int>(o.rainfall_mode) : 0]);
    std::fprintf(f, "%-22s %s\n",    "REPORT_2D", o.report_2d ? "YES" : "NO");
    std::fprintf(f, "%-22s %s\n",    "CELL_CLOSURE",
                 o.cell_closure == twoD::CellClosure2D::VFR ? "VFR" : "FLAT");
    std::fprintf(f, "%-22s %s\n",    "FACE_RECONSTRUCTION",
                 o.face_reconstruction == twoD::FaceDepth2D::VFR_FACE
                     ? "VFR_FACE" : "MEAN");
    std::fprintf(f, "%-22s %.12g\n", "VFR_MIN_WET_FRAC",  o.vfr_min_wet_frac);
    // Explicit-marcher configuration (the only 2D integrator).
    std::fprintf(f, "%-22s %s\n",    "INTEGRATOR",        "EXPLICIT");
    std::fprintf(f, "%-22s %.12g\n", "THETA",             o.theta);
    std::fprintf(f, "%-22s %.12g\n", "CFL_NUMBER",        o.cfl_number);
    std::fprintf(f, "%-22s %.12g\n", "H_MOVE",            o.h_move);
    std::fprintf(f, "%-22s %d\n",    "LTS_TIERS",         o.lts_tiers);
    std::fprintf(f, "%-22s %.12g\n", "FROUDE_MAX",        o.froude_max);
    std::fprintf(f, "%-22s %s\n",    "ADVECTION",
                 o.advection ? "YES" : "NO");
    std::fprintf(f, "%-22s %s\n",    "COUPLING_AREA",
                 o.coupling_area_auto ? "AUTO" : "DEFAULT");
    {
        static const char* sBackend2D[] = {"CPU","AUTO","OMP","CUDA","HIP","SYCL"};
        const int bi = static_cast<int>(o.backend);
        std::fprintf(f, "%-22s %s\n", "BACKEND",
                     (bi >= 0 && bi <= 5) ? sBackend2D[bi] : "AUTO");
    }
    if (!o.output_file.empty()) {
        const std::string of_tok =
            emit_path_token(o.output_file, dst_dir, force_abs_paths, warnings);
        if (!of_tok.empty())
            std::fprintf(f, "%-22s %s\n", "OUTPUT_FILE", of_tok.c_str());
    }

    // ---- [2D_MESH_FILE] — keep the reference, refresh the sidecar ----------
    if (external) {
        // TWO regimes, and the difference matters on Save-As:
        //
        //  * has_mesh — the mesh is in memory, so the sidecar below is REWRITTEN
        //    at the token's destination-resolved location. The mesh therefore
        //    TRAVELS with the .inp and the stored token stays correct as-is
        //    (only an absolute token needs rebasing, so the model does not carry
        //    a machine-specific path). Re-anchoring here would be actively
        //    wrong: it would point the reference back at the source folder AND
        //    make Save-As overwrite the original .2dm.
        //
        //  * !has_mesh — nothing is written (the external mesh failed to load,
        //    or the model was opened leniently). A bare relative token then
        //    silently starts resolving against the NEW directory, where no mesh
        //    exists, and the saved model opens 1D-only with no diagnostic. Here
        //    the reference must be re-anchored against `.absolute` — which
        //    resolve_external_file_slots filled from the SOURCE .inp dir — so it
        //    keeps pointing at the file that actually holds the mesh.
        std::string tok;
        if (has_mesh) {
            tok = o.mesh_file.original;
            if (!force_abs_paths && !dst_dir.empty() && io::isAbsolutePath(tok)) {
                auto r = io::makeRelative(tok, dst_dir);
                if (warnings && r.classification != io::PathClass::Relative
                    && !r.warning.empty())
                    warnings->push_back(r.warning);
                tok = r.path;
            }
        } else {
            tok = emit_path_token(o.mesh_file, dst_dir, force_abs_paths, warnings);
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
// / [2D_EDGE_CONVEYANCE] / the [2D_INFILTRATION*] family from the in-memory
// mesh state. Target is either the main .inp (inline mode) or the external
// .2dm sidecar — both are parsed by the same section grammar
// (SectionHandlers2D / load2DMeshExternalFile). Exactly one destination per
// save, which is what keeps every section here single-emission.
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
    // Optional INIT_DEPTH (mesh length units — see the UNITS header above;
    // default 0 = dry) precedes TAG. The column is
    // emitted for EVERY row whenever any triangle has a nonzero initial depth
    // or a tag, so TAG's position stays unambiguous on re-read (a numeric
    // 5th token always means INIT_DEPTH).
    bool any_init_depth = false, any_tag = false;
    for (int t = 0; t < nt; ++t) {
        if (mesh.tri_init_depth[t] != 0.0) any_init_depth = true;
        if (!mesh.tri_tag[t].empty()) any_tag = true;
    }
    const bool write_depth_col = any_init_depth || any_tag;
    sec(f, "2D_TRIANGLES");
    if (write_depth_col)
        std::fprintf(f, ";;%-6s %-8s %-8s %-12s %-12s %s\n", "V1", "V2", "V3",
                     "MANNINGS_N", "INIT_DEPTH", "TAG");
    else
        std::fprintf(f, ";;%-6s %-8s %-8s %-12s %s\n", "V1", "V2", "V3",
                     "MANNINGS_N", "TAG");
    for (int t = 0; t < nt; ++t) {
        std::fprintf(f, "%-8d %-8d %-8d %-12.6g", mesh.tri_v0[t],
                     mesh.tri_v1[t], mesh.tri_v2[t], mesh.mannings_n[t]);
        if (write_depth_col)
            std::fprintf(f, " %-12.6g", mesh.tri_init_depth[t]);
        if (!mesh.tri_tag[t].empty())
            std::fprintf(f, " %s", mesh.tri_tag[t].c_str());
        std::fprintf(f, "\n");
    }

    // ---- [2D_INITIAL_VELOCITY] ------------------------------------------------
    // Sparse: only triangles with a nonzero initial velocity get a row.
    // Emitted after [2D_TRIANGLES] — rows validate against loaded triangles.
    {
        bool any_uv = false;
        for (int t = 0; t < nt && !any_uv; ++t)
            any_uv = mesh.tri_init_u[t] != 0.0 || mesh.tri_init_v[t] != 0.0;
        if (any_uv) {
            sec(f, "2D_INITIAL_VELOCITY");
            std::fprintf(f, ";;%-6s %-12s %s\n", "TRI", "U", "V");
            for (int t = 0; t < nt; ++t) {
                if (mesh.tri_init_u[t] == 0.0 && mesh.tri_init_v[t] == 0.0)
                    continue;
                std::fprintf(f, "%-8d %-12.6g %.6g\n", t, mesh.tri_init_u[t],
                             mesh.tri_init_v[t]);
            }
        }
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
    // Repeated-row form: mesh.tri_couplings is the source of truth (several
    // nodes may couple to one triangle). Fall back to the legacy per-triangle
    // arrays only when no rows exist (meshes authored before resolve, or via
    // paths that never synthesised rows).
    {
        if (!mesh.tri_couplings.empty()) {
            sec(f, "2D_TRIANGLE_NODE_MAP");
            std::fprintf(f, ";;%-6s %-16s %-10s %s\n", "TRIANGLE", "NODE", "CD",
                         "AREA");
            for (const auto& row : mesh.tri_couplings) {
                const std::string cn = node_name_for(row.node_name, row.node);
                if (cn.empty()) continue;
                if (row.tri < 0 || row.tri >= nt) continue;
                std::fprintf(f, "%-8d %-16s %-10.6g %.12g\n", row.tri,
                             cn.c_str(), row.cd, row.area);
            }
        } else {
            bool any = false;
            for (int t = 0; t < nt && !any; ++t)
                any = !node_name_for(mesh.tri_coupled_node_name[t],
                                     mesh.tri_coupled_node[t]).empty();
            if (any) {
                sec(f, "2D_TRIANGLE_NODE_MAP");
                std::fprintf(f, ";;%-6s %-16s %-10s %s\n", "TRIANGLE", "NODE",
                             "CD", "AREA");
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

#ifdef OPENSWMM_HAS_2D
    // ---- [2D_INFILTRATION_OPTIONS] / _DEFAULTS / [2D_INFILTRATION] ------------
    // Per-cell mesh attributes (§5.5.5): they travel with the mesh, into
    // whichever single destination this function was pointed at. THE ONLY CALL
    // SITE — see emit2DInfilSections.
    emit2DInfilSections(f, ctx);
#endif
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
    static const char* sRouting[]    = {"STEADY","KINWAVE","DYNWAVE","FV"};
    static const char* sInertial[]   = {"NONE","PARTIAL","FULL"};
    static const char* sNormFlow[]   = {"SLOPE","FROUDE","BOTH","NEITHER"};
    static const char* sSurcharge[]  = {"EXTRAN","SLOT","DYNAMIC_SLOT","TPA"};
    static const char* sUnsteadyFriction[] = {"NONE","VITKOVSKY"};  // issue #156

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
    std::fprintf(f,"%-20s %s\n",  "FLOW_ROUTING",     (rm>=0&&rm<=3)?sRouting[rm]:"DYNWAVE");
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
    // OpenSWMM extension, not a legacy key — emit only when set so 1D-only
    // models keep a legacy-clean [OPTIONS] block.
    if (o.ignore_2d)
        std::fprintf(f,"%-20s %s\n",  "IGNORE_2D",         "YES");
    // Same rule for the two transport engine-selection keys. Both were
    // dropped on save: a EULERIAN_ARD model came back LEGACY and a
    // WATER_AGE model came back with age tracking off, silently. They are
    // written TOGETHER because either alone is worse than neither — a deck
    // carrying WATER_AGE ON without its EULERIAN_ARD line opens with the
    // "no age is tracked this simulation" warning instead of the model the
    // user saved.
    if (o.quality_solver == QualitySolverKind::EULERIAN_ARD)
        std::fprintf(f,"%-20s %s\n",  "QUALITY_SOLVER",    "EULERIAN_ARD");
    // X1: same rule for LAGRANGIAN — a save-as must not silently reopen as
    // LEGACY (the A1a defect shape, third instance guarded).
    if (o.quality_solver == QualitySolverKind::LAGRANGIAN)
        std::fprintf(f,"%-20s %s\n",  "QUALITY_SOLVER",    "LAGRANGIAN");
    // X3a: the LARD stepping keys ride the same save-as rule — dropping
    // either silently changes the transport discretization on reopen.
    if (o.quality_step > 0.0) {
        char qsb[32];
        fmt_step(qsb, o.quality_step);
        std::fprintf(f,"%-20s %s\n",  "QUALITY_STEP",      qsb);
    }
    if (o.max_segments_per_link != 100)
        std::fprintf(f,"%-20s %d\n",  "MAX_SEGMENTS_PER_LINK",
                     o.max_segments_per_link);
    // X3b: the RWPT keys ride the same rule.
    if (o.lard_rwpt)
        std::fprintf(f,"%-20s %s\n",  "DISPERSION",         "RWPT");
    if (o.rwpt_seed != 0)
        std::fprintf(f,"%-20s %d\n",  "RWPT_SEED",          o.rwpt_seed);
    if (o.water_age)
        std::fprintf(f,"%-20s %s\n",  "WATER_AGE",         "ON");
    // Same save-as rule: dropping this line silently reverts a fresh-boundary
    // model to the legacy held-quality backflow on reopen.
    if (o.outfall_backflow_zero)
        std::fprintf(f,"%-20s %s\n",  "OUTFALL_BACKFLOW_QUALITY", "ZERO");
    // H1: same rule, same reason — a save-as that dropped this reopened as a
    // model with no temperature tracking, silently (the A1a defect).
    if (o.heat_transport)
        std::fprintf(f,"%-20s %s\n",  "HEAT_TRANSPORT",    "ON");
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
    std::fprintf(f,"%-20s %s\n",  "SURCHARGE_METHOD",    (sm>=0&&sm<=3)?sSurcharge[sm]:"EXTRAN");
    if (sm == 3)   // issue #156: TPA acoustic celerity, non-default method only
        std::fprintf(f,"%-20s %g\n","TPA_CELERITY", o.tpa_celerity);
    {   // Unsteady friction (issue #156): round-tripped unconditionally, like
        // SURCHARGE_METHOD — the keys are inert unless a consuming solver runs.
        const int uf = o.unsteady_friction;
        std::fprintf(f,"%-20s %s\n","UNSTEADY_FRICTION",
                     (uf>=0&&uf<=1)?sUnsteadyFriction[uf]:"NONE");
        std::fprintf(f,"%-20s %g\n","UF_K3", o.uf_k3);
        if (o.report_signed_heads)   // issue #156 O-6: non-default only
            std::fprintf(f,"%-20s %s\n","REPORT_SIGNED_HEADS","YES");
    }
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
    // VIRTUAL_JUNCTION_MOMENTUM is not emitted: FULL is retired (see
    // SimulationOptions.hpp) and virtual_junction_momentum is now always 0,
    // so writing the key could only ever re-emit the retired value.

    // Explicit finite-volume solver knobs. Emitted only under FLOW_ROUTING FV
    // so a DW model's [OPTIONS] block stays legacy-clean; the keys are inert
    // under other routing models, so a round-trip that changes FLOW_ROUTING
    // does not lose a user's FV configuration mid-session — it is simply not
    // written until FV is selected again.
    if (o.routing_model == RoutingModel::FV) {
        static const char* sRiemann[] = {"HLL","HLLC"};
        static const char* sLimiter[] = {"MINMOD","VANLEER","SUPERBEE"};
        static const char* sScalar[]  = {"UPWIND","MUSCL","QUICKEST_ULTIMATE"};
        static const char* sTime[]    = {"EULER","RK2"};
        static const char* sBackend[] = {"CPU","AUTO","OMP","CUDA","HIP","SYCL"};
        const auto& fvo = o.fv;
        std::fprintf(f,"%-20s %g\n", "FV_CELL_LENGTH",  fvo.cell_length);
        std::fprintf(f,"%-20s %d\n", "FV_MIN_CELLS",    fvo.min_cells);
        std::fprintf(f,"%-20s %g\n", "FV_CFL",          fvo.cfl);
        std::fprintf(f,"%-20s %s\n", "FV_RIEMANN",      sRiemann[static_cast<int>(fvo.riemann)]);
        std::fprintf(f,"%-20s %d\n", "FV_ORDER",        fvo.order);
        std::fprintf(f,"%-20s %s\n", "FV_LIMITER",      sLimiter[static_cast<int>(fvo.limiter)]);
        std::fprintf(f,"%-20s %s\n", "FV_SCALAR_SCHEME",sScalar[static_cast<int>(fvo.scalar_scheme)]);
        std::fprintf(f,"%-20s %s\n", "FV_TIME_INTEGRATION",
                     sTime[static_cast<int>(fvo.time_integration)]);
        std::fprintf(f,"%-20s %g\n", "FV_SLOT_CELERITY", fvo.slot_celerity);
        if (fvo.pressurized_implicit)
            std::fprintf(f,"%-20s %s\n", "FV_PRESSURIZED_IMPLICIT", "YES");
        if (fvo.pressure_closure == 1)  // issue #156: non-default only
            std::fprintf(f,"%-20s %s\n", "FV_PRESSURE_CLOSURE", "TPA");
        if (fvo.dispersion > 0.0)
            std::fprintf(f,"%-20s %g\n", "FV_DISPERSION", fvo.dispersion);
        if (fvo.structure_coupling != fv::StructureCoupling::SUBSTEP)
            std::fprintf(f,"%-20s %s\n", "FV_STRUCTURE_COUPLING", "ROUTING_STEP");
        if (!fvo.compaction)
            std::fprintf(f,"%-20s %s\n", "FV_COMPACTION", "NO");
        std::fprintf(f,"%-20s %s\n", "FV_BACKEND",      sBackend[static_cast<int>(fvo.backend)]);
        std::fprintf(f,"%-20s %ld\n","FV_MIN_PARALLEL_CELLS", fvo.min_parallel_cells);
        if (!fvo.lts)
            std::fprintf(f,"%-20s %s\n", "FV_LTS", "NO");
        std::fprintf(f,"%-20s %d\n", "FV_LTS_MAX_TIERS", fvo.lts_max_tiers);
        if (fvo.cfl_census_interval != 1)
            std::fprintf(f,"%-20s %d\n","FV_CFL_CENSUS_INTERVAL", fvo.cfl_census_interval);
    }
    if (!o.crs.empty())
        std::fprintf(f,"%-20s %s\n",  "CRS",            o.crs.c_str());
    if (o.write_absolute_paths)
        std::fprintf(f,"%-20s %s\n",  "WRITE_ABSOLUTE_PATHS", "YES");
    for (const auto& kv : o.ext_options) {
        // "GWF:<subcatch>:<type>" entries are the parsed [GWF] section, not real
        // options — they are re-emitted by the [GWF] writer. Round-tripping them
        // through here corrupts them: handle_options() uppercases the key and
        // keeps only the first value token.
        if (kv.first.rfind("GWF:", 0) == 0) continue;
        std::fprintf(f,"%-20s %s\n",  kv.first.c_str(), kv.second.c_str());
    }
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
    const double sf = ctx.gages.scale_factor[u];
    if(ts>=0){
        std::fprintf(f,"%-16s %-12s %d:%02d     %.2f     TIMESERIES %s",ctx.gage_names.name_of(j).c_str(),fmt,h,m,ctx.gages.snow_factor[u],tN(ctx,ts));
        if(sf!=1.0)std::fprintf(f," %.4g",sf);
        std::fprintf(f,"\n");
    }
    else if(!ctx.gages.file_path[u].empty()){
        const std::string tok = emit_path_token(ctx.gages.file_path[u],
                                                 dst_dir, force_abs_paths, warnings);
        if(ctx.gages.file_format[u]==RainFileFormat::USER_CSV){
            // Compact openswmm extension — the reader expects one "path:col"
            // token. An EMPTY column is a legal state (it means "first data
            // column"), so emit the bare path rather than a dangling
            // "path:" that reads as malformed to EPA SWMM / PCSWMM.
            const std::string& col = ctx.gages.col_name[u];
            const std::string src = col.empty() ? tok : tok + ":" + col;
            std::fprintf(f,"%-16s %-12s %d:%02d     %.2f     FILE \"%s\"",
                          ctx.gage_names.name_of(j).c_str(),fmt,h,m,
                          ctx.gages.snow_factor[u],
                          src.c_str());
            if(sf!=1.0)std::fprintf(f," %.4g",sf);
        }else{
            // Legacy FILE grammar: Fname Station Units [StartDate] [SF] — the
            // station + units tokens are REQUIRED (gage.c errors with ERROR 203
            // without them). An empty station gets a '*' placeholder so the
            // line stays parseable; legacy will match no rows on it.
            const std::string& sta = ctx.gages.station_id[u];
            if(sta.empty() && warnings)
                warnings->push_back("[RAINGAGES] gage \""+ctx.gage_names.name_of(j)+
                                    "\": no station ID set; wrote '*' — the legacy "
                                    "engine will match no rows in \""+tok+"\"");
            std::fprintf(f,"%-16s %-12s %d:%02d     %.2f     FILE \"%s\" %s %s",
                          ctx.gage_names.name_of(j).c_str(),fmt,h,m,
                          ctx.gages.snow_factor[u],
                          tok.c_str(),
                          sta.empty() ? "*" : sta.c_str(),
                          ctx.gages.rain_units[u]==1 ? "MM" : "IN");
            if(sf!=1.0)std::fprintf(f," * %.4g",sf); // '*' = no start date (tok[8])
        }
        std::fprintf(f,"\n");
    }
    else{
        // Neither a resolved series index nor a file path. Dropping the row
        // entirely would delete the gage while [SUBCATCHMENTS] still names it,
        // which fails reload with a fatal ERR_NAME (209) on the subcatchment.
        // Emit the retained series name (or the '*' placeholder, which the gage
        // resolver tolerates) so the gage definition always survives.
        const std::string& tsn = ctx.gages.ts_name[u];
        if(tsn.empty() && warnings)
            warnings->push_back("[RAINGAGES] gage \""+ctx.gage_names.name_of(j)+
                                "\": no rainfall source set; wrote 'TIMESERIES *'");
        std::fprintf(f,"%-16s %-12s %d:%02d     %.2f     TIMESERIES %s",
                      ctx.gage_names.name_of(j).c_str(),fmt,h,m,
                      ctx.gages.snow_factor[u],
                      tsn.empty() ? "*" : tsn.c_str());
        if(sf!=1.0)std::fprintf(f," %.4g",sf);
        std::fprintf(f,"\n");
    }
    }}

    // [SUBCATCHMENTS]
    // Grammar: Name RainGage Outlet Area %Imperv Width %Slope CurbLen
    //          [Snowpack] [RainScale] [SnowScale]
    // The Snowpack token (8) was previously never written, silently dropping
    // snow pack assignments on round-trip. It is now emitted whenever a pack is
    // assigned, and also as a '*' placeholder when a scale factor needs to be
    // written past it (same convention as the [RAINGAGES] FILE start date).
    // Scale factors are omitted entirely when both are 1.0, so models that do
    // not use them round-trip byte-identically to before.
    if(ctx.n_subcatches()>0){sec(f,"SUBCATCHMENTS");
    std::fprintf(f,";;%-16s %-16s %-16s %-12s %-10s %-12s %-10s %-10s %-16s %-10s %-10s\n","Name","RainGage","Outlet","Area","%%Imperv","Width","%%Slope","CurbLen","Snowpack","RainScale","SnowScale");
    std::fprintf(f,";;%-16s %-16s %-16s %-12s %-10s %-12s %-10s %-10s %-16s %-10s %-10s\n","----------------","----------------","----------------","------------","----------","------------","----------","----------","----------------","----------","----------");
    for(int j=0;j<ctx.n_subcatches();++j){auto u=static_cast<size_t>(j);
    write_obj_comment(f, ctx.subcatches.comments, u);
    const char* spname = sspN(ctx,u);          // index, else retained name, else "*"
    const bool  has_sp = std::strcmp(spname,"*")!=0;
    const double rsf = ctx.subcatches.rain_scale_factor[u];
    const double ssf = ctx.subcatches.snow_scale_factor[u];
    const bool need_scale = (rsf!=1.0 || ssf!=1.0);
    std::fprintf(f,"%-16s %-16s %-16s %12.4f %10.2f %12.4f %10.4f %10.4f",ctx.subcatch_names.name_of(j).c_str(),sgN(ctx,u),oN(ctx,u),ctx.subcatches.area[u],ctx.subcatches.frac_imperv[u]*100.0,ctx.subcatches.width[u],ctx.subcatches.slope[u]*100.0,ctx.subcatches.curb_length[u]);
    // Token 8 must be present to reach tokens 9/10 positionally.
    if(has_sp || need_scale) std::fprintf(f," %-16s",spname);
    if(need_scale){
        // RainScale must be written even when 1.0 if SnowScale is not, to hold
        // the position of token 10.
        std::fprintf(f," %-10.4g",rsf);
        if(ssf!=1.0) std::fprintf(f," %-10.4g",ssf);
    }
    std::fprintf(f,"\n");
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

    // [AQUIFERS]
    // Grammar: Name Por WP FC Ksat Kslope Tslope ETu ETs Seep Ebot Egw Umc [ETupat]
    // %.10g preserves full double precision without scientific notation for the
    // magnitudes seen in elevations and conductivities.
    if(ctx.aquifers.count()>0){sec(f,"AQUIFERS");
    std::fprintf(f,";;%-16s %-10s %-10s %-10s %-10s %-10s %-10s %-10s %-10s %-10s %-10s %-10s %-10s %-16s\n","Name","Porosity","WiltPoint","FieldCap","Ksat","Kslope","Tslope","ETu","ETs","Seepage","Ebot","Egw","Umc","ETupat");
    std::fprintf(f,";;%-16s %-10s %-10s %-10s %-10s %-10s %-10s %-10s %-10s %-10s %-10s %-10s %-10s %-16s\n","----------------","----------","----------","----------","----------","----------","----------","----------","----------","----------","----------","----------","----------","----------------");
    for(int j=0;j<ctx.aquifers.count();++j){auto u=static_cast<size_t>(j);
    std::fprintf(f,"%-16s %-10.10g %-10.10g %-10.10g %-10.10g %-10.10g %-10.10g %-10.10g %-10.10g %-10.10g %-10.10g %-10.10g %-10.10g",
        ctx.aquifers.names[u].c_str(),
        ctx.aquifers.porosity[u],ctx.aquifers.wilting_point[u],ctx.aquifers.field_capacity[u],
        ctx.aquifers.conductivity[u],ctx.aquifers.conduct_slope[u],ctx.aquifers.tension_slope[u],
        ctx.aquifers.upper_evap[u],ctx.aquifers.lower_evap[u],ctx.aquifers.lower_loss[u],
        ctx.aquifers.bottom_elev[u],ctx.aquifers.water_table_elev[u],ctx.aquifers.upper_moist[u]);
    if(!ctx.aquifers.upper_evap_pat[u].empty())
        std::fprintf(f," %-16s",ctx.aquifers.upper_evap_pat[u].c_str());
    std::fprintf(f,"\n");
    }}

    // [GROUNDWATER]
    // Grammar: Subcatch Aquifer Node SurfElev A1 B1 A2 B2 A3 Twgr Hstar
    // A subcatchment carries a groundwater row iff gw_aquifer >= 0. gw_node is
    // resolved by PostParseResolver (the section normally precedes [JUNCTIONS]);
    // a row whose node still will not resolve is skipped with a warning rather
    // than written with '*', which would fail reload with ERR_NAME (209).
    // The gw_* vectors are grown per-row by the parser (ensure_subcatch_gw_capacity),
    // so they can be SHORTER than the subcatchment count — bound the loop by them.
    {size_t nGw=static_cast<size_t>(ctx.n_subcatches());
    if(ctx.subcatches.gw_aquifer.size()<nGw) nGw=ctx.subcatches.gw_aquifer.size();
    if(ctx.subcatches.gw_node.size()   <nGw) nGw=ctx.subcatches.gw_node.size();
    bool anyGw=false;
    for(size_t us=0;us<nGw;++us){
        const int a=ctx.subcatches.gw_aquifer[us];
        if(a>=0 && a<ctx.aquifers.count() && ctx.subcatches.gw_node[us]>=0){anyGw=true;break;}}
    if(anyGw){sec(f,"GROUNDWATER");
    std::fprintf(f,";;%-16s %-16s %-16s %-10s %-10s %-10s %-10s %-10s %-10s %-10s %-10s\n","Subcatchment","Aquifer","Node","SurfElev","A1","B1","A2","B2","A3","Tw","Hstar");
    std::fprintf(f,";;%-16s %-16s %-16s %-10s %-10s %-10s %-10s %-10s %-10s %-10s %-10s\n","----------------","----------------","----------------","----------","----------","----------","----------","----------","----------","----------","----------");
    for(size_t u=0;u<nGw;++u){const int s=static_cast<int>(u);
    const int aq=ctx.subcatches.gw_aquifer[u];
    if(aq<0 || aq>=ctx.aquifers.count()) continue;
    if(ctx.subcatches.gw_node[u]<0){
        if(warnings)
            warnings->push_back("[GROUNDWATER] subcatchment \""+ctx.subcatch_names.name_of(s)+
                                "\": receiving node unresolved; row omitted");
        continue;
    }
    std::fprintf(f,"%-16s %-16s %-16s %-10.10g %-10.10g %-10.10g %-10.10g %-10.10g %-10.10g %-10.10g %-10.10g\n",
        ctx.subcatch_names.name_of(s).c_str(),
        ctx.aquifers.names[static_cast<size_t>(aq)].c_str(),
        nN(ctx,ctx.subcatches.gw_node[u]),
        ctx.subcatches.gw_surf_elev[u],ctx.subcatches.gw_a1[u],ctx.subcatches.gw_b1[u],
        ctx.subcatches.gw_a2[u],ctx.subcatches.gw_b2[u],ctx.subcatches.gw_a3[u],
        ctx.subcatches.gw_tw[u],ctx.subcatches.gw_hstar[u]);
    }}}

    // [GWF]
    // Grammar: Subcatch LATERAL|DEEP <expression>
    // handle_gwf() stores these in options.ext_options under "GWF:<sub>:<type>"
    // (SWMMEngine reads them back from there). They must NOT be emitted through
    // the [OPTIONS] ext_options passthrough: handle_options() uppercases the key
    // and keeps only the first value token, which mangles the key case and
    // truncates any multi-token expression. Iterate subcatchments (not the
    // unordered_map) so the output order is deterministic.
    {bool wroteGwf=false;
    static const char* kGwfTypes[]={"LATERAL","DEEP"};
    for(int s=0;s<ctx.n_subcatches();++s){
        const std::string& scn = ctx.subcatch_names.name_of(s);
        for(const char* ty : kGwfTypes){
            auto it = ctx.options.ext_options.find("GWF:"+scn+":"+ty);
            if(it==ctx.options.ext_options.end() || it->second.empty()) continue;
            if(!wroteGwf){sec(f,"GWF");
                std::fprintf(f,";;%-16s %-10s %s\n","Subcatchment","Type","Expression");
                wroteGwf=true;}
            std::fprintf(f,"%-16s %-10s %s\n",scn.c_str(),ty,it->second.c_str());
        }
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
            const std::string rpt_tok =
                (uj < ctx.lid_usage.rpt_file.size())
                    ? emit_path_token(ctx.lid_usage.rpt_file[uj], dst_dir,
                                      force_abs_paths, warnings)
                    : std::string{};
            std::fprintf(f," %s", rpt_tok.empty() ? "*" : rpt_tok.c_str());
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
    if(hasRegularJunction(ctx)){sec(f,"JUNCTIONS");
    std::fprintf(f,";;%-16s %-12s %-12s %-12s %-12s %-12s\n","Name","Elev","MaxDepth","InitDepth","SurDepth","Aponded");
    std::fprintf(f,";;%-16s %-12s %-12s %-12s %-12s %-12s\n","----------------","------------","------------","------------","------------","------------");
    for(int j=0;j<ctx.n_nodes();++j){auto u=static_cast<size_t>(j);if(ctx.nodes.type[u]!=NodeType::JUNCTION||isVirtualNode(ctx,u))continue;
    write_obj_comment(f, ctx.nodes.comments, u);
    std::fprintf(f,"%-16s %12.4f %12.4f %12.4f %12.4f %12.4f\n",ctx.node_names.name_of(j).c_str(),ctx.nodes.invert_elev[u],ctx.nodes.full_depth[u],ctx.nodes.init_depth[u],ctx.nodes.sur_depth[u],ctx.nodes.ponded_area[u]);
    }}

    // [VIRTUAL_JUNCTIONS] — name + invert elevation, plus an optional MaxDepth
    // that is used ONLY to draw the ground surface. All solver geometry is
    // derived from the attached conduits at load time (refactored engine only).
    if(hasVirtualJunction(ctx)){sec(f,"VIRTUAL_JUNCTIONS");
    const bool anyRim = hasVirtualJunctionRim(ctx);
    if(anyRim){
        std::fprintf(f,";;%-16s %-12s %-12s\n","Name","Elev","MaxDepth");
        std::fprintf(f,";;%-16s %-12s %-12s\n","----------------","------------","------------");
    } else {
        std::fprintf(f,";;%-16s %-12s\n","Name","Elev");
        std::fprintf(f,";;%-16s %-12s\n","----------------","------------");
    }
    for(int j=0;j<ctx.n_nodes();++j){auto u=static_cast<size_t>(j);if(!isVirtualNode(ctx,u))continue;
    write_obj_comment(f, ctx.nodes.comments, u);
    const double rim = (u<ctx.nodes.rim_depth.size()) ? ctx.nodes.rim_depth[u] : 0.0;
    if(rim>0.0)
        std::fprintf(f,"%-16s %12.4f %12.4f\n",ctx.node_names.name_of(j).c_str(),ctx.nodes.invert_elev[u],rim);
    else
        std::fprintf(f,"%-16s %12.4f\n",ctx.node_names.name_of(j).c_str(),ctx.nodes.invert_elev[u]);
    }}

    // [OUTFALLS]
    // Column order must match outfall_readParams() / handle_outfalls():
    //   Name Elev Type [StageData] Gated [RouteTo]
    // StageData is present only for FIXED (numeric stage) and TIDAL/TIMESERIES
    // (table NAME, not the internal index).
    if(hasNT(ctx,NodeType::OUTFALL)){sec(f,"OUTFALLS");
    std::fprintf(f,";;%-16s %-12s %-12s %-16s %-8s %-16s\n","Name","Elev","Type","Stage Data","Gated","Route To");
    std::fprintf(f,";;%-16s %-12s %-12s %-16s %-8s %-16s\n","----------------","------------","------------","----------------","--------","----------------");
    for(int j=0;j<ctx.n_nodes();++j){auto u=static_cast<size_t>(j);if(ctx.nodes.type[u]!=NodeType::OUTFALL)continue;
    write_obj_comment(f, ctx.nodes.comments, u);
    // Relational side-table (Phase 4).
    const int orow = ctx.node_subtypes.outfall_row(j); const auto& O = ctx.node_subtypes.outfalls;
    const OutfallType otype = (orow>=0)?O.bc_type[static_cast<size_t>(orow)]:OutfallType::FREE;
    const int oflap = (orow>=0)?O.has_flap_gate[static_cast<size_t>(orow)]:0;
    const double oparam = (orow>=0)?O.param[static_cast<size_t>(orow)]:0.0;
    const int oroute = (orow>=0)?O.route_to[static_cast<size_t>(orow)]:-1;

    // Stage-data column.
    char stage[64]; stage[0]='\0';
    if(otype==OutfallType::FIXED){
        std::snprintf(stage,sizeof(stage),"%.4f",oparam);
    } else if(otype==OutfallType::TIDAL||otype==OutfallType::TIMESERIES){
        const int t = static_cast<int>(oparam);
        if(t>=0 && t<static_cast<int>(ctx.tables.tables.size()))
            std::snprintf(stage,sizeof(stage),"%s",ctx.tables[t].id.c_str());
    }

    std::fprintf(f,"%-16s %12.4f %-12s",ctx.node_names.name_of(j).c_str(),ctx.nodes.invert_elev[u],ofName(otype));
    if(stage[0]!='\0')std::fprintf(f," %-16s",stage);
    std::fprintf(f," %-8s",oflap?"YES":"NO");
    if(oroute>=0 && oroute<ctx.n_subcatches())
        std::fprintf(f," %-16s",ctx.subcatch_names.name_of(oroute).c_str());
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
        if(ci >= 0 && ci < ctx.n_tables()) {
            cnStr = ctx.tables[ci].id; curveName = cnStr.c_str();
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
    const StorageShape sshape = (srow>=0)?S.shape[static_cast<size_t>(srow)]:StorageShape::FUNCTIONAL;
    if(scurve>=0)
        std::fprintf(f,"%-16s %12.4f %12.4f %12.4f TABULAR    %s 0 0 %12.4f\n",ctx.node_names.name_of(j).c_str(),ctx.nodes.invert_elev[u],ctx.nodes.full_depth[u],ctx.nodes.init_depth[u],tN(ctx,scurve),ctx.nodes.sur_depth[u]);
    else if(storage_shape_is_geometric(sshape)){
        // Geometric shapes re-emit the RAW L/W/Z the user gave us, not the derived
        // a/b/c — that is the whole point of keeping p1..p3 in the SoA. Writing the
        // coefficients here would silently downgrade the node to FUNCTIONAL on save.
        const double q1=S.p1[static_cast<size_t>(srow)];
        const double q2=S.p2[static_cast<size_t>(srow)];
        const double q3=S.p3[static_cast<size_t>(srow)];
        std::fprintf(f,"%-16s %12.4f %12.4f %12.4f %-10s %g %g %g 0 %12.4f\n",ctx.node_names.name_of(j).c_str(),ctx.nodes.invert_elev[u],ctx.nodes.full_depth[u],ctx.nodes.init_depth[u],storage_shape_keyword(sshape),q1,q2,q3,ctx.nodes.sur_depth[u]);
    }
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
    // Length and roughness are emitted at full double precision (%.15g), not a
    // fixed 4/6-decimal field: real models carry sub-metre lengths and 1/n-derived
    // roughness with many significant figures, and truncating them perturbs the
    // routing solution — noticeable on large, control-heavy (chaos-sensitive) models.
    std::fprintf(f,"%-16s %-16s %-16s %15.15g %15.15g %12.4f %12.4f %10.4f %10.4f\n",ctx.link_names.name_of(j).c_str(),nN(ctx,ctx.links.node1[u]),nN(ctx,ctx.links.node2[u]),(cr>=0)?CD.length[static_cast<size_t>(cr)]:0.0,(cr>=0)?CD.roughness[static_cast<size_t>(cr)]:0.01,ctx.links.offset1[u],ctx.links.offset2[u],ctx.links.q0[u],ctx.links.q_limit[u]);
    }}

    // [PUMPS]
    if(hasLT(ctx,LinkType::PUMP)){sec(f,"PUMPS");
    std::fprintf(f,";;%-16s %-16s %-16s %-16s %-10s %-10s %-10s\n","Name","FromNode","ToNode","PumpCurve","Status","Startup","Shutoff");
    std::fprintf(f,";;%-16s %-16s %-16s %-16s %-10s %-10s %-10s\n","----------------","----------------","----------------","----------------","----------","----------","----------");
    for(int j=0;j<ctx.n_links();++j){auto u=static_cast<size_t>(j);if(ctx.links.type[u]!=LinkType::PUMP)continue;
    write_obj_comment(f, ctx.links.comments, u);
    const int pr=ctx.link_subtypes.pump_row(j); const auto& PD=ctx.link_subtypes.pumps;
    // Emit the real startup/shutoff depths (and initial state) rather than a
    // hardcoded "0 0": those are the wet-well depths at which the pump switches
    // on/off. Dropping them makes every pump run unconditionally on re-read,
    // changing the pumping regime and downstream flooding/continuity.
    const char* pstat=(pr>=0)?(PD.init_state[static_cast<size_t>(pr)]?"ON":"OFF"):(ctx.links.setting[u]>0?"ON":"OFF");
    // Pump curve: prefer the resolved index, then the retained curve name (the
    // [OUTLETS] writer already does this). Writing tN() alone silently
    // downgraded a curved pump to IDEAL whenever only the name was known.
    // '*' is the legal IDEAL placeholder, so an empty name is not an error.
    const int pcurve=(pr>=0)?PD.curve[static_cast<size_t>(pr)]:-1;
    const char* pcname=(pcurve>=0)?tN(ctx,pcurve)
                      :(ctx.links.pump_curve_name[u].empty()?"*"
                        :ctx.links.pump_curve_name[u].c_str());
    std::fprintf(f,"%-16s %-16s %-16s %-16s %-10s %.10g %.10g\n",ctx.link_names.name_of(j).c_str(),nN(ctx,ctx.links.node1[u]),nN(ctx,ctx.links.node2[u]),pcname,pstat,(pr>=0)?PD.startup[static_cast<size_t>(pr)]:0.0,(pr>=0)?PD.shutoff[static_cast<size_t>(pr)]:0.0);
    }}

    // [ORIFICES]
    if(hasLT(ctx,LinkType::ORIFICE)){sec(f,"ORIFICES");
    std::fprintf(f,";;%-16s %-16s %-16s %-10s %-10s %-10s %-8s %-10s\n","Name","FromNode","ToNode","Type","Offset","Cd","Gated","CloseTime");
    std::fprintf(f,";;%-16s %-16s %-16s %-10s %-10s %-10s %-8s %-10s\n","----------------","----------------","----------------","----------","----------","----------","--------","----------");
    for(int j=0;j<ctx.n_links();++j){auto u=static_cast<size_t>(j);if(ctx.links.type[u]!=LinkType::ORIFICE)continue;
    write_obj_comment(f, ctx.links.comments, u);
    const int orr=ctx.link_subtypes.orifice_row(j);
    const auto uorr=static_cast<size_t>(orr);
    // orifice_type: 0 = BOTTOM, 1 = SIDE (LinkSubtypes.hpp). `orate` is stored
    // as parsed — HOURS, converted at use (SWMMEngine.cpp:2927) — so it is
    // emitted verbatim. All four were previously hardcoded/dropped, which
    // destroyed the orientation, the flap gate and the close time on save.
    const char* otype=(orr>=0&&ctx.link_subtypes.orifices.orifice_type[uorr]!=0.0)?"SIDE":"BOTTOM";
    std::fprintf(f,"%-16s %-16s %-16s %-10s %10.4f %10.4f %-8s %10.4f\n",ctx.link_names.name_of(j).c_str(),nN(ctx,ctx.links.node1[u]),nN(ctx,ctx.links.node2[u]),otype,ctx.links.offset1[u],(orr>=0)?ctx.link_subtypes.orifices.cd[uorr]:0.0,ctx.links.has_flap_gate[u]?"YES":"NO",(orr>=0)?ctx.link_subtypes.orifices.orate[uorr]:0.0);
    }}

    // [WEIRS]
    if(hasLT(ctx,LinkType::WEIR)){sec(f,"WEIRS");
    std::fprintf(f,";;%-16s %-16s %-16s %-12s %-10s %-10s %-8s %-10s %-10s %-10s\n","Name","FromNode","ToNode","Type","CrestHt","Cd","Gated","EndCon","EndCoeff","Surcharge");
    std::fprintf(f,";;%-16s %-16s %-16s %-12s %-10s %-10s %-8s %-10s %-10s %-10s\n","----------------","----------------","----------------","------------","----------","----------","--------","----------","----------","----------");
    static const char* WEIR_TYPE[]={"TRANSVERSE","SIDEFLOW","V-NOTCH","TRAPEZOIDAL"};
    for(int j=0;j<ctx.n_links();++j){auto u=static_cast<size_t>(j);if(ctx.links.type[u]!=LinkType::WEIR)continue;
    write_obj_comment(f, ctx.links.comments, u);
    const int wr=ctx.link_subtypes.weir_row(j); const auto& WD=ctx.link_subtypes.weirs;
    // Emit the real weir type, gate, end-contractions, end-coeff and the
    // can-surcharge flag rather than hardcoding TRANSVERSE/NO/0/0: dropping the
    // surcharge flag defaults it back to YES on re-read, changing drowned-weir
    // flow (weir↔orifice switch) and hence overflow/flooding.
    const int wt=(wr>=0)?static_cast<int>(WD.weir_type[static_cast<size_t>(wr)]):0;
    const char* wtStr=(wt>=0&&wt<4)?WEIR_TYPE[wt]:"TRANSVERSE";
    const char* wgate=ctx.links.has_flap_gate[u]?"YES":"NO";
    const char* wsurch=(wr>=0&&WD.can_surcharge[static_cast<size_t>(wr)])?"YES":"NO";
    std::fprintf(f,"%-16s %-16s %-16s %-12s %10.4f %10.4f %-8s %10.4f %10.4f %s\n",
        ctx.link_names.name_of(j).c_str(),nN(ctx,ctx.links.node1[u]),nN(ctx,ctx.links.node2[u]),
        wtStr,
        (wr>=0)?WD.crest_height[static_cast<size_t>(wr)]:0.0,
        (wr>=0)?WD.cd[static_cast<size_t>(wr)]:0.0,
        wgate,
        (wr>=0)?WD.end_contractions[static_cast<size_t>(wr)]:0.0,
        (wr>=0)?WD.cd2[static_cast<size_t>(wr)]:0.0,
        wsurch);
    }}

    // [OUTLETS]
    if(hasLT(ctx,LinkType::OUTLET)){sec(f,"OUTLETS");
    std::fprintf(f,";;%-16s %-16s %-16s %-10s %-16s %-10s %-10s\n","Name","FromNode","ToNode","Offset","Type","Coeff","Expon");
    std::fprintf(f,";;%-16s %-16s %-16s %-10s %-16s %-10s %-10s\n","----------------","----------------","----------------","----------","----------------","----------","----------");
    for(int j=0;j<ctx.n_links();++j){auto u=static_cast<size_t>(j);if(ctx.links.type[u]!=LinkType::OUTLET)continue;
    write_obj_comment(f, ctx.links.comments, u);
    const int olr=ctx.link_subtypes.outlet_row(j); const auto& OUT=ctx.link_subtypes.outlets;
    // Rating type (param1): 0 FUNCTIONAL/HEAD, 1 FUNCTIONAL/DEPTH, 2 TABULAR/HEAD,
    // 3 TABULAR/DEPTH. Legacy SWMM requires the compound TYPE token (and the curve
    // name for TABULAR); a bare "FUNCTIONAL" both loses the rating curve and
    // segfaults the legacy parser (strcomp(NULL,"HEAD")).
    const int otype=(olr>=0)?static_cast<int>(OUT.outlet_type[static_cast<size_t>(olr)]):0;
    const bool otab=(otype>=2), odepth=(otype==1||otype==3);
    const char* otypeStr=otab?(odepth?"TABULAR/DEPTH":"TABULAR/HEAD")
                              :(odepth?"FUNCTIONAL/DEPTH":"FUNCTIONAL/HEAD");
    const double ocrest=(olr>=0)?OUT.crest_height[static_cast<size_t>(olr)]:ctx.links.offset1[u];
    const char* ogate=ctx.links.has_flap_gate[u]?"YES":"NO";
    if(otab){
        const int oci=(olr>=0)?OUT.curve[static_cast<size_t>(olr)]:-1;
        const char* ocurve=(oci>=0)?tN(ctx,oci):ctx.links.pump_curve_name[u].c_str();
        std::fprintf(f,"%-16s %-16s %-16s %10.4f %-16s %-16s %s\n",ctx.link_names.name_of(j).c_str(),nN(ctx,ctx.links.node1[u]),nN(ctx,ctx.links.node2[u]),ocrest,otypeStr,ocurve,ogate);
    } else {
        std::fprintf(f,"%-16s %-16s %-16s %10.4f %-16s %10g %10g %s\n",ctx.link_names.name_of(j).c_str(),nN(ctx,ctx.links.node1[u]),nN(ctx,ctx.links.node2[u]),ocrest,otypeStr,(olr>=0)?OUT.coeff[static_cast<size_t>(olr)]:0.0,(olr>=0)?OUT.expon[static_cast<size_t>(olr)]:0.0,ogate);
    }
    }}

    // [XSECTIONS]
    {sec(f,"XSECTIONS");
    std::fprintf(f,";;%-16s %-16s %-12s %-12s %-12s %-12s %-8s\n","Link","Shape","Geom1","Geom2","Geom3","Geom4","Barrels");
    std::fprintf(f,";;%-16s %-16s %-12s %-12s %-12s %-12s %-8s\n","----------------","----------------","------------","------------","------------","------------","--------");
    for(int j=0;j<ctx.n_links();++j){auto u=static_cast<size_t>(j);
    // [XSECTIONS] applies to conduits, orifices and weirs only. Pumps and
    // outlets have no cross-section; emitting one (a default CIRCULAR with
    // Geom1=0) makes legacy SWMM reject it with ERROR 211 (invalid number).
    if(ctx.links.type[u]==LinkType::PUMP||ctx.links.type[u]==LinkType::OUTLET)continue;
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
    // IRREGULAR cross-sections reference a named [TRANSECTS] entry: Geom1 is
    // the transect NAME, not a dimension. The parser retains it in
    // pump_curve_name (and keeps xsect_geom1 at 0), so falling through to the
    // numeric emission below wrote the resolver-derived y_full/w_max where the
    // name belongs — on reload that numeric failed to resolve as a transect
    // and the conduit silently degenerated to zero area. The trailing zeros
    // land in w_max/y_bot/r_bot at reparse but the resolver re-derives all of
    // them from the transect table; barrels stay in the tok[6] column. Legacy
    // SWMM returns right after reading the name, so the tail is harmless there.
    if(ctx.links.xsect_shape[u]==XsectShape::IRREGULAR){
        const char* tname=ctx.links.pump_curve_name[u].c_str();
        // API-built link that resolved a transect without retaining the name.
        if(!*tname&&ctx.links.xsect_curve[u]>=0&&
           ctx.links.xsect_curve[u]<ctx.transects.count())
            tname=ctx.transects.names[static_cast<size_t>(ctx.links.xsect_curve[u])].c_str();
        std::fprintf(f,"%-16s %-16s %-12s %12.4f %12.4f %12.4f %8d\n",
            ctx.link_names.name_of(j).c_str(),
            xsName(static_cast<int>(ctx.links.xsect_shape[u])),
            tname,0.0,0.0,0.0,xbarrels);
        continue;
    }
    // CUSTOM cross-sections reference a named shape curve: Geom1 is the max
    // height and the Geom2 slot holds the curve NAME (not a dimension), barrels
    // stay in the Geom5 column. Emitting numeric geometry drops the curve name,
    // so legacy SWMM reads "0.0000" as the curve id (ERROR 209). The name is
    // retained in pump_curve_name by the [XSECTIONS] parser.
    if(ctx.links.xsect_shape[u]==XsectShape::CUSTOM){
        const double cy=(ctx.links.xsect_geom1[u]!=0.0)?ctx.links.xsect_geom1[u]:ctx.links.xsect_y_full[u];
        std::fprintf(f,"%-16s %-16s %.15g %-16s %12.4f %12.4f %8d\n",
            ctx.link_names.name_of(j).c_str(),
            xsName(static_cast<int>(ctx.links.xsect_shape[u])),
            cy,ctx.links.pump_curve_name[u].c_str(),0.0,0.0,xbarrels);
        continue;
    }
    double g1,g2,g3,g4;
    if(ctx.links.xsect_geom1[u]!=0.0){
        g1=ctx.links.xsect_geom1[u]; g2=ctx.links.xsect_geom2[u];
        g3=ctx.links.xsect_geom3[u]; g4=ctx.links.xsect_geom4[u];
    } else {
        g1=ctx.links.xsect_y_full[u]; g2=ctx.links.xsect_w_max[u]; g3=0.0; g4=0.0;
    }
    // Full precision on the geometry: xsect dimensions (e.g. CUSTOM/irregular
    // heights like 0.61875) set conduit conveyance; %.4f truncation perturbs routing.
    std::fprintf(f,"%-16s %-16s %.15g %.15g %.15g %.15g %8d\n",ctx.link_names.name_of(j).c_str(),xsName(static_cast<int>(ctx.links.xsect_shape[u])),g1,g2,g3,g4,xbarrels);
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
    // Full precision on the GR pairs: they are token-parsed on both engines
    // (column width is not load-bearing) and %.4f silently rounded >4-dp
    // stations/elevations on every save. Matches the [XSECTIONS] precedent.
    for(int k=0;k<nsta;++k){auto uk=static_cast<size_t>(k);
    if(k%5==0)std::fprintf(f,"GR");
    std::fprintf(f," %10.15g %10.15g",ctx.transects.elevations[ut][uk],ctx.transects.stations[ut][uk]);
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

    // [INLET_USAGE]
    // Grammar: Link Inlet Node #Inlets %Clog Qmax aLocal wLocal Placement
    // Parsed into ctx.inlet_usages but previously never written, so every
    // inlet-to-link assignment was lost on save and [STREETS]/[INLETS] became
    // inert. clog_factor is stored as 1 - pctClogged/100 (InfraHandler).
    if(ctx.inlet_usages.count()>0){sec(f,"INLET_USAGE");
    std::fprintf(f,";;%-16s %-16s %-16s %-8s %-8s %-10s %-10s %-10s %-10s\n","Link","Inlet","Node","#Inlets","%Clog","Qmax","aLocal","wLocal","Placement");
    std::fprintf(f,";;%-16s %-16s %-16s %-8s %-8s %-10s %-10s %-10s %-10s\n","----------------","----------------","----------------","--------","--------","----------","----------","----------","----------");
    static const char* kPlacement[]={"AUTOMATIC","ON_GRADE","ON_SAG"};
    for(int j=0;j<ctx.inlet_usages.count();++j){auto u=static_cast<size_t>(j);
    const int li=ctx.inlet_usages.link_index[u];
    const int di=ctx.inlet_usages.design_index[u];
    const int ni=ctx.inlet_usages.node_index[u];
    // The reader drops rows naming an unknown link/inlet/node, so a stale index
    // here would be silently discarded on reload — skip it with a warning
    // instead of writing '*'.
    if(li<0||li>=ctx.n_links()||di<0||di>=ctx.inlets.count()||ni<0||ni>=ctx.n_nodes()){
        if(warnings)
            warnings->push_back("[INLET_USAGE] row "+std::to_string(j)+
                                ": unresolved link/inlet/node; row omitted");
        continue;
    }
    int pl=ctx.inlet_usages.placement[u]; if(pl<0||pl>2)pl=0;
    std::fprintf(f,"%-16s %-16s %-16s %8d %8.4g %10.4g %10.4g %10.4g %-10s\n",
        ctx.link_names.name_of(li).c_str(),
        ctx.inlets.names[static_cast<size_t>(di)].c_str(),
        ctx.node_names.name_of(ni).c_str(),
        ctx.inlet_usages.num_inlets[u],
        (1.0-ctx.inlet_usages.clog_factor[u])*100.0,
        ctx.inlet_usages.flow_limit[u],
        ctx.inlet_usages.local_depress[u],
        ctx.inlet_usages.local_width[u],
        kPlacement[pl]);
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

    // [INITIAL_QUALITY] — per-element initial concentrations (raw constituent
    // name + raw value retained by the store; resolved element names preferred,
    // falling back to the retained raw name for never-resolved rows).
    if(ctx.initial_quality.count()>0){sec(f,"INITIAL_QUALITY");
    std::fprintf(f,";;%-8s %-16s %-16s %-10s\n","Scope","Element","Constituent","Value");
    std::fprintf(f,";;%-8s %-16s %-16s %-10s\n","--------","----------------","----------------","----------");
    for(int j=0;j<ctx.initial_quality.count();++j){auto u=static_cast<size_t>(j);
    const auto& iq=ctx.initial_quality;
    const bool link=iq.is_link[u]!=0;
    const int ei=iq.elem_idx[u];
    const char*en;
    if(link)en=(ei>=0&&ei<ctx.n_links())?ctx.link_names.name_of(ei).c_str():iq.elem_name[u].c_str();
    else en=(ei>=0&&ei<ctx.n_nodes())?ctx.node_names.name_of(ei).c_str():iq.elem_name[u].c_str();
    std::fprintf(f,"%-8s %-16s %-16s %10.4f\n",link?"LINK":"NODE",en,
        iq.constituent[u].c_str(),iq.value[u]);
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

    // [COVERAGES] — percent of each subcatchment covered by each land use
    // (stored verbatim in percent, matching handle_coverages). Zero rows
    // are skipped: absent coverage rows parse back to 0.
    if(ctx.subcatches.coverage_n_landuses>0&&ctx.n_subcatches()>0){
    const int nLu=ctx.subcatches.coverage_n_landuses;
    bool any=false;
    for(std::size_t i=0;i<ctx.subcatches.coverage.size()&&!any;++i)
        if(ctx.subcatches.coverage[i]!=0.0)any=true;
    if(any){sec(f,"COVERAGES");
    std::fprintf(f,";;%-16s %-16s %-10s\n","Subcatchment","LandUse","Percent");
    std::fprintf(f,";;%-16s %-16s %-10s\n","----------------","----------------","----------");
    for(int s=0;s<ctx.n_subcatches();++s){
    for(int lu=0;lu<nLu;++lu){
    auto idx=static_cast<size_t>(s)*static_cast<size_t>(nLu)+static_cast<size_t>(lu);
    if(idx>=ctx.subcatches.coverage.size())break;
    const double pct=ctx.subcatches.coverage[idx];
    if(pct==0.0)continue;
    std::fprintf(f,"%-16s %-16s %10.4f\n",
        ctx.subcatch_names.name_of(s).c_str(),
        ctx.landuse_names.name_of(lu).c_str(),pct);
    }}}}

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

    // [LOADINGS] — initial pollutant buildup per subcatchment (stored in
    // ctx.subcatches.conc by handle_loadings; see also the object-deletion
    // re-pack tests). Zero rows are skipped: absent rows parse back to 0.
    if(ctx.subcatches.conc_n_pollutants>0&&ctx.n_subcatches()>0&&ctx.n_pollutants()>0){
    const int np=ctx.subcatches.conc_n_pollutants;
    bool any=false;
    for(std::size_t i=0;i<ctx.subcatches.conc.size()&&!any;++i)
        if(ctx.subcatches.conc[i]!=0.0)any=true;
    if(any){sec(f,"LOADINGS");
    std::fprintf(f,";;%-16s %-16s %-10s\n","Subcatchment","Pollutant","Buildup");
    std::fprintf(f,";;%-16s %-16s %-10s\n","----------------","----------------","----------");
    for(int s=0;s<ctx.n_subcatches();++s){
    for(int p=0;p<np&&p<ctx.n_pollutants();++p){
    auto idx=static_cast<size_t>(s)*static_cast<size_t>(np)+static_cast<size_t>(p);
    if(idx>=ctx.subcatches.conc.size())break;
    const double w=ctx.subcatches.conc[idx];
    if(w==0.0)continue;
    std::fprintf(f,"%-16s %-16s %10.4f\n",
        ctx.subcatch_names.name_of(s).c_str(),pN(ctx,p),w);
    }}}}

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
    // Full precision on the average value: DWF base flows are small (~1e-4 m3/s)
    // with many significant figures; %.6f truncation shifts the base sewage load.
    std::fprintf(f,"%-16s %-16s %.15g",
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

    // [RDII_DECAY] (exponential IA decay parameters; optional degree-day snow
    // clause "SNOW snow_T snow_ddf" appended to rows with the snow model on)
    if(ctx.rdii_decay.count()>0){sec(f,"RDII_DECAY");
    std::fprintf(f,";;%-16s %-8s %-10s %-10s %-10s %-8s %-10s %-10s %-4s %-8s %-10s\n",
        "UHGroup","Response","k_dep","k_0","k_T","T_ref","theta_rec","T_freeze",
        "Snow","snow_T","snow_ddf");
    std::fprintf(f,";;%-16s %-8s %-10s %-10s %-10s %-8s %-10s %-10s %-4s %-8s %-10s\n",
        "----------------","--------","----------","----------","----------",
        "--------","----------","----------","----","--------","----------");
    static const char* decayResp[]={"SHORT","MEDIUM","LONG"};
    for(const auto& e:ctx.rdii_decay.entries){
    const char* r=(e.response>=0&&e.response<=2)?decayResp[e.response]:"SHORT";
    std::fprintf(f,"%-16s %-8s %10.5f %10.5f %10.5f %8.2f %10.5f %10.5f",
        e.uh_name.c_str(),r,
        e.k_dep,e.k_0,e.k_T,e.T_ref,e.theta_rec,e.T_freeze);
    if(e.snow_on)
        std::fprintf(f," SNOW %8.2f %10.5f",e.snow_T,e.snow_ddf);
    std::fprintf(f,"\n");
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
    // The curve TYPE keyword goes on the FIRST row only; continuation rows are
    // "Name X Y". Legacy SWMM reads a repeated type on a later row as the
    // X-value → ERROR 211 (invalid number).
    for(size_t k=0;k<tb.x.size();++k)std::fprintf(f,"%-16s %-12s %12.6f %12.6f\n",tN(ctx,t),(k==0?lbl:""),tb.x[k],tb.y[k]);
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

    // [PROCESS_COMPONENTS] — Unified Transport suite D-UT8 (round-trip; the
    // component config FILES are each component's own to write, never ours).
    // The config= reference is an external-file slot like any other, so it
    // goes through emit_path_token (Slice IO-4): an absolute path is rebased
    // against the destination directory, a relative one passes through.
    if(!ctx.process_component_specs.empty()){sec(f,"PROCESS_COMPONENTS");
    for(const auto&pc:ctx.process_component_specs){std::fprintf(f,"%s",pc.id.c_str());
    if(!pc.config_path.empty()){const std::string cfg=
    emit_path_token(pc.config_path,dst_dir,force_abs_paths,warnings);
    std::fprintf(f," config=\"%s\"",cfg.c_str());

    // IO3 carry-alongside: a RELATIVE config= reference resolves against
    // the .inp's own directory, so saving the deck somewhere else would
    // leave it dangling. Copy the file the model was actually read from
    // (resolved_config_path, set at open) next to the written .inp when
    // the destination differs. Absolute references were rebased by
    // emit_path_token above and need no copy. Failures WARN, never fail
    // the save — the deck text itself is intact.
    namespace fsys=std::filesystem;

    // IO3a: ask the COMPONENT to write its own file first. The writer's rule
    // above ("each component's own to write, never ours") was the intent all
    // along; until this hook existed nothing acted on it, so a model.heat or
    // model.rxn edited through the C API or the GUI was copied back in its
    // ORIGINAL form and the edit vanished — silently, and unlike the embedded
    // case, unwarned.
    //
    // An EMPTY render means the component declines (nothing configured, or it
    // has not implemented saving), and the carry-alongside copy below runs
    // instead. That fallback is what lets components adopt saving one at a
    // time without any intermediate state losing data.
    bool component_wrote=false;
    if(!pc.config_path.empty()){
    const auto*entry=components::ProcessComponentRegistry::instance().find(pc.id);
    if(entry&&entry->save){
    const std::string text=entry->save(ctx,pc);
    if(!text.empty()){
    std::error_code wec;
    fsys::path rel_w(pc.config_path);
    fsys::path dst_w=rel_w.is_absolute()
    ?fsys::path(emit_path_token(pc.config_path,dst_dir,force_abs_paths,nullptr))
    :(dst_dir.empty()?rel_w:fsys::path(dst_dir)/rel_w);
    if(dst_w.has_parent_path())fsys::create_directories(dst_w.parent_path(),wec);
    std::ofstream cf(dst_w,std::ios::binary|std::ios::trunc);
    if(cf.is_open()){cf<<text;component_wrote=cf.good();cf.close();}
    if(!component_wrote&&warnings)warnings->push_back(
    "Could not write component config '"+pc.config_path+"' for '"+pc.id+
    "' — the model's in-memory configuration for that component was NOT "
    "saved. The previous file, if any, is unchanged.");
    }}}

    if(!component_wrote&&!pc.resolved_config_path.empty()){
    fsys::path src(pc.resolved_config_path);
    fsys::path rel(pc.config_path);
    if(rel.is_relative()&&!dst_dir.empty()){
    std::error_code ec;
    fsys::path dst=fsys::path(dst_dir)/rel;
    if(fsys::exists(src,ec)&&
    !fsys::equivalent(src,dst,ec)){
    // Overwriting is REQUIRED for the feature to be correct: re-saving a
    // model into a folder that already holds last save's copy must refresh
    // it, or the deck ships with a stale config. But an existing file with
    // DIFFERENT content may belong to another model in that folder, and
    // destroying it silently is not something a save should do. Measured
    // before this guard: a save-as replaced an unrelated model.rxn and
    // reported nothing.
    bool replacing_different=false;
    if(fsys::exists(dst,ec)){
    std::ifstream a(src,std::ios::binary),b(dst,std::ios::binary);
    const std::string sa((std::istreambuf_iterator<char>(a)),
    std::istreambuf_iterator<char>());
    const std::string sb((std::istreambuf_iterator<char>(b)),
    std::istreambuf_iterator<char>());
    replacing_different=(sa!=sb);
    }
    if(dst.has_parent_path())fsys::create_directories(dst.parent_path(),ec);
    fsys::copy_file(src,dst,fsys::copy_options::overwrite_existing,ec);
    if(ec&&warnings)warnings->push_back(
    "Could not copy component config '"+pc.resolved_config_path+
    "' alongside the saved model ("+ec.message()+") — the written "
    "config=\""+pc.config_path+"\" reference may dangle.");
    else if(replacing_different&&warnings)warnings->push_back(
    "Saving this model replaced an existing, different '"+pc.config_path+
    "' in the destination folder with the copy this model uses.");
    }}}
    }
    for(const auto&a:pc.args)std::fprintf(f," %s=\"%s\"",a.first.c_str(),a.second.c_str());
    std::fprintf(f,"\n");
    }}

    // Embedded component sections ([REACTION_*] today) are NOT serialized —
    // there is no per-component saveData() until IO3, and the intended layout
    // is an external config file anyway. Say so rather than dropping
    // user-authored model data silently: whether they were applied or
    // overridden by an external file, they are gone from the deck we just
    // wrote.
    if(warnings && !ctx.embedded_component_sections.empty()){
    std::string tags;
    for(const auto&es:ctx.embedded_component_sections){
    if(!tags.empty())tags+=", ";tags+="["+es.first+"]";}
    warnings->push_back(
    "Embedded component sections are NOT written back to the .inp and are "
    "lost from this save: "+tags+". Move them to an external component "
    "config file registered in [PROCESS_COMPONENTS] (config=\"model.rxn\") "
    "to keep them — per-component serialization arrives with plan phase IO3.");
    }

    // [2D_*] — 2D surface-routing model definition (no-op for 1D models
    // and for engine builds without the 2D module).
    write2DSections(f, ctx, dst_dir, force_abs_paths, warnings);

    std::fclose(f);
    return 0;
}

} // namespace inp_writer
} // namespace openswmm
