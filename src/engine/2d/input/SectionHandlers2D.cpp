/**
 * @file SectionHandlers2D.cpp
 * @brief Implementation of 2D input section parsers.
 *
 * @see SectionHandlers2D.hpp
 * @ingroup engine_2d
 */

#include "SectionHandlers2D.hpp"

#include "../data/BoundaryData.hpp"
#include "../../input/InputReader.hpp"
#include "../../input/Tokenizer.hpp"
#include "../../core/ErrorCodes.hpp"
#include "../../core/SimulationContext.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>

namespace openswmm::twoD {

namespace {

// Case-insensitive string comparison
bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::toupper(static_cast<unsigned char>(a[i]))
            != std::toupper(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

// Try to parse an integer; return -1 on failure
int tryParseInt(const std::string& s, bool& ok) {
    char* end = nullptr;
    long val = std::strtol(s.c_str(), &end, 10);
    ok = (end != s.c_str() && *end == '\0');
    return static_cast<int>(val);
}

double tryParseDouble(const std::string& s, bool& ok) {
    char* end = nullptr;
    double val = std::strtod(s.c_str(), &end);
    ok = (end != s.c_str() && *end == '\0');
    return val;
}

// Find a vertex by tag name; returns index or -1
int findVertexByTag(const MeshData& mesh, const std::string& tag) {
    for (int i = 0; i < mesh.n_vertices(); ++i) {
        if (mesh.vtag[i] == tag) return i;
    }
    return -1;
}

// Find a triangle by tag name; returns index or -1
int findTriangleByTag(const MeshData& mesh, const std::string& tag) {
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        if (mesh.tri_tag[i] == tag) return i;
    }
    return -1;
}

/// Hard-error suffix for [2D_OPTIONS] keys retired with the CVODE/ARKODE
/// stack (D2, 2026-07-29).
const std::string RETIRED_SUFFIX =
    " was retired with the CVODE/ARKODE 2D solvers: the explicit "
    "local-inertial marcher is the only 2D integrator. Remove the line; "
    "marcher settings are THETA, CFL_NUMBER, LTS_TIERS, H_MOVE, FROUDE_MAX, "
    "MAX_TIMESTEP, COUPLING_AREA.";

/// Retired [2D_OPTIONS] material: warn-and-ignore when a warnings sink is
/// available (the file-load path — legacy models must still open), hard
/// error otherwise (the programmatic set path).
std::string retiredOption(const std::string& what,
                          std::vector<std::string>* warnings) {
    if (warnings) {
        warnings->push_back(
            openswmm::format_warning(openswmm::WARN_2D_OPTION_RETIRED, what));
        return {};
    }
    return what + RETIRED_SUFFIX;
}

} // anonymous namespace


std::string parse2DOptionsLine(const std::vector<std::string>& tokens,
                                SolverOptions2D& opts,
                                std::vector<std::string>* warnings) {
    if (tokens.size() < 2) return "Expected PARAMETER VALUE";

    const auto& key = tokens[0];
    const auto& val = tokens[1];
    bool ok = false;

    if (iequals(key, "MAX_TIMESTEP")) {
        opts.max_timestep = tryParseDouble(val, ok);
        if (!ok) return "Invalid MAX_TIMESTEP value";
    } else if (iequals(key, "DRY_DEPTH")) {
        opts.dry_depth = tryParseDouble(val, ok);
        if (!ok) return "Invalid DRY_DEPTH value";
    } else if (iequals(key, "COUPLING_CD")) {
        opts.coupling_cd = tryParseDouble(val, ok);
        if (!ok) return "Invalid COUPLING_CD value";
    } else if (iequals(key, "COUPLING_SYNC")) {
        opts.coupling_sync = tryParseDouble(val, ok);
        if (!ok || opts.coupling_sync < 0.0)
            return "Invalid COUPLING_SYNC value (seconds, >= 0)";
    } else if (iequals(key, "LIMITER_EPSILON")) {
        opts.limiter_epsilon = tryParseDouble(val, ok);
        if (!ok) return "Invalid LIMITER_EPSILON value";
    } else if (iequals(key, "FLUX_DH_EPS")) {
        opts.flux_dh_eps = tryParseDouble(val, ok);
        if (!ok) return "Invalid FLUX_DH_EPS value";
    } else if (iequals(key, "CELL_CLOSURE")) {
        if (iequals(val, "FLAT"))
            opts.cell_closure = CellClosure2D::FLAT;
        else if (iequals(val, "VFR"))
            opts.cell_closure = CellClosure2D::VFR;
        else
            return "Unknown CELL_CLOSURE: " + val;
    } else if (iequals(key, "FACE_RECONSTRUCTION")) {
        if (iequals(val, "MEAN"))
            opts.face_reconstruction = FaceDepth2D::MEAN;
        else if (iequals(val, "VFR_FACE"))
            opts.face_reconstruction = FaceDepth2D::VFR_FACE;
        else
            return "Unknown FACE_RECONSTRUCTION: " + val;
    } else if (iequals(key, "VFR_MIN_WET_FRAC")) {
        const double frac = tryParseDouble(val, ok);
        if (!ok || frac <= 0.0 || frac > 0.5)
            return "Invalid VFR_MIN_WET_FRAC value (expected (0, 0.5])";
        opts.vfr_min_wet_frac = frac;
    } else if (iequals(key, "RAINFALL_MODE")) {
        if (iequals(val, "NATURAL_NEIGHBOUR") || iequals(val, "NATURAL_NEIGHBOR"))
            opts.rainfall_mode = RainfallMode::NATURAL_NEIGHBOUR;
        else if (iequals(val, "SYSTEM"))
            opts.rainfall_mode = RainfallMode::SYSTEM;
        else if (iequals(val, "NONE"))
            opts.rainfall_mode = RainfallMode::NONE;
        else
            return "Unknown RAINFALL_MODE: " + val;
    } else if (iequals(key, "REPORT_2D")) {
        if (iequals(val, "YES") || val == "1")
            opts.report_2d = true;
        else if (iequals(val, "NO") || val == "0")
            opts.report_2d = false;
        else
            return "Invalid REPORT_2D value (YES/NO)";
    } else if (iequals(key, "OUTPUT_FILE")) {
        // Stored as the raw token; resolved against the .inp directory in
        // SWMMEngine::open when the Default2DOutputPlugin is instantiated.
        opts.output_file = val;
    } else if (iequals(key, "INTEGRATOR")) {
        // The explicit marcher is the only integrator (D2 retirement,
        // 2026-07-29). EXPLICIT is accepted (and the default); the retired
        // CVODE/ARKODE selections warn on file load (the model opens and
        // runs the marcher — not silent, WARNING 104 names the substitution)
        // and hard-error on the programmatic set path.
        if (!iequals(val, "EXPLICIT"))
            return retiredOption("INTEGRATOR " + val, warnings);
    } else if (iequals(key, "THETA")) {
        const double th = tryParseDouble(val, ok);
        if (!ok || th <= 0.0 || th > 1.0)
            return "Invalid THETA value (expected (0, 1])";
        opts.theta = th;
    } else if (iequals(key, "CFL_NUMBER")) {
        const double c = tryParseDouble(val, ok);
        if (!ok || c <= 0.0 || c > 1.0)
            return "Invalid CFL_NUMBER value (expected (0, 1])";
        opts.cfl_number = c;
    } else if (iequals(key, "H_MOVE")) {
        const double h = tryParseDouble(val, ok);
        if (!ok || h < 0.0)
            return "Invalid H_MOVE value (expected metres >= 0)";
        opts.h_move = h;
    } else if (iequals(key, "LTS_TIERS")) {
        const int k = tryParseInt(val, ok);
        if (!ok || k < 1 || k > 8)
            return "Invalid LTS_TIERS value (expected 1..8)";
        opts.lts_tiers = k;
    } else if (iequals(key, "FROUDE_MAX")) {
        const double f = tryParseDouble(val, ok);
        if (!ok || f <= 0.0)
            return "Invalid FROUDE_MAX value (expected > 0)";
        opts.froude_max = f;
    } else if (iequals(key, "COUPLING_AREA")) {
        if (iequals(val, "AUTO"))
            opts.coupling_area_auto = true;
        else if (iequals(val, "DEFAULT"))
            opts.coupling_area_auto = false;
        else
            return "Unknown COUPLING_AREA: " + val + " (expected AUTO|DEFAULT)";
    } else if (is2DRetiredOptionKey(key)) {
        // These keys configured the deleted CVODE/ARKODE stack. On file load
        // they are ignored with a WARNING 104 (legacy models must still
        // open); on the programmatic set path they stay hard errors.
        return retiredOption(key, warnings);
    } else {
        return "Unknown 2D_OPTIONS parameter: " + key;
    }

    return {};
}


bool is2DRetiredOptionKey(const std::string& key) {
    static const char* kRetired[] = {
        "MIN_TIMESTEP", "REL_TOLERANCE", "ABS_TOLERANCE", "MAX_CVODE_STEPS",
        "MAX_KRYLOV_DIM", "LINEAR_SOLVER", "PRECONDITIONER", "JACOBIAN",
        "ATOL_AREA_REF", "COUPLING_INTERVAL", "COUPLING_WINDOW",
        "ACTIVE_SET", "ACTIVE_SET_HALO", "MOMENTUM",
    };
    for (const char* k : kRetired) {
        if (iequals(key, k)) return true;
    }
    return false;
}


bool is2DOptionKey(const std::string& key) {
    static const char* kKeys[] = {
        "MAX_TIMESTEP", "DRY_DEPTH", "COUPLING_CD", "COUPLING_SYNC",
        "LIMITER_EPSILON", "FLUX_DH_EPS", "RAINFALL_MODE", "REPORT_2D",
        "CELL_CLOSURE", "FACE_RECONSTRUCTION", "VFR_MIN_WET_FRAC",
        "OUTPUT_FILE",
        "INTEGRATOR", "THETA", "CFL_NUMBER", "H_MOVE",
        "LTS_TIERS", "FROUDE_MAX", "COUPLING_AREA",
    };
    for (const char* k : kKeys) {
        if (iequals(key, k)) return true;
    }
    return false;
}


std::string format2DOptionValue(const SolverOptions2D& opts,
                                const std::string& key) {
    auto fmt_g = [](double v) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.12g", v);
        return std::string(buf);
    };

    if (iequals(key, "MAX_TIMESTEP"))      return fmt_g(opts.max_timestep);
    if (iequals(key, "DRY_DEPTH"))         return fmt_g(opts.dry_depth);
    if (iequals(key, "LIMITER_EPSILON"))   return fmt_g(opts.limiter_epsilon);
    if (iequals(key, "FLUX_DH_EPS"))       return fmt_g(opts.flux_dh_eps);
    if (iequals(key, "COUPLING_CD"))       return fmt_g(opts.coupling_cd);
    if (iequals(key, "COUPLING_SYNC"))     return fmt_g(opts.coupling_sync);
    if (iequals(key, "REPORT_2D"))         return opts.report_2d ? "YES" : "NO";
    if (iequals(key, "OUTPUT_FILE"))       return opts.output_file;
    if (iequals(key, "CELL_CLOSURE"))
        return (opts.cell_closure == CellClosure2D::VFR) ? "VFR" : "FLAT";
    if (iequals(key, "FACE_RECONSTRUCTION"))
        return (opts.face_reconstruction == FaceDepth2D::VFR_FACE) ? "VFR_FACE"
                                                                   : "MEAN";
    if (iequals(key, "VFR_MIN_WET_FRAC")) return fmt_g(opts.vfr_min_wet_frac);
    if (iequals(key, "RAINFALL_MODE")) {
        switch (opts.rainfall_mode) {
            case RainfallMode::NATURAL_NEIGHBOUR: return "NATURAL_NEIGHBOUR";
            case RainfallMode::SYSTEM:            return "SYSTEM";
            case RainfallMode::NONE:              return "NONE";
        }
        return "NATURAL_NEIGHBOUR";
    }
    if (iequals(key, "INTEGRATOR"))    return "EXPLICIT";
    if (iequals(key, "THETA"))         return fmt_g(opts.theta);
    if (iequals(key, "CFL_NUMBER"))    return fmt_g(opts.cfl_number);
    if (iequals(key, "H_MOVE"))        return fmt_g(opts.h_move);
    if (iequals(key, "LTS_TIERS"))     return std::to_string(opts.lts_tiers);
    if (iequals(key, "FROUDE_MAX"))    return fmt_g(opts.froude_max);
    if (iequals(key, "COUPLING_AREA")) return opts.coupling_area_auto ? "AUTO" : "DEFAULT";
    return {};
}


std::string parse2DVertexLine(const std::vector<std::string>& tokens,
                               MeshData& mesh) {
    if (tokens.size() < 3) return "Expected X Y Z [TAG]";

    bool ok = false;
    double x = tryParseDouble(tokens[0], ok);
    if (!ok) return "Invalid X coordinate";

    double y = tryParseDouble(tokens[1], ok);
    if (!ok) return "Invalid Y coordinate";

    double z = tryParseDouble(tokens[2], ok);
    if (!ok) return "Invalid Z coordinate";

    std::string tag;
    if (tokens.size() >= 4) tag = tokens[3];

    int idx = mesh.n_vertices();
    mesh.resize_vertices(idx + 1);
    mesh.vx[idx] = x;
    mesh.vy[idx] = y;
    mesh.vz[idx] = z;
    mesh.vtag[idx] = tag;

    return {};
}


std::string parse2DTriangleLine(const std::vector<std::string>& tokens,
                                 MeshData& mesh) {
    if (tokens.size() < 4) return "Expected V1 V2 V3 MANNINGS_N [TAG]";

    bool ok = false;
    int v0 = tryParseInt(tokens[0], ok);
    if (!ok) return "Invalid V1 index";

    int v1 = tryParseInt(tokens[1], ok);
    if (!ok) return "Invalid V2 index";

    int v2 = tryParseInt(tokens[2], ok);
    if (!ok) return "Invalid V3 index";

    double n = tryParseDouble(tokens[3], ok);
    if (!ok) return "Invalid MANNINGS_N value";

    std::string tag;
    if (tokens.size() >= 5) tag = tokens[4];

    int idx = mesh.n_triangles();
    mesh.resize_triangles(idx + 1);
    mesh.tri_v0[idx] = v0;
    mesh.tri_v1[idx] = v1;
    mesh.tri_v2[idx] = v2;
    mesh.mannings_n[idx] = n;
    mesh.tri_tag[idx] = tag;

    return {};
}


std::string parse2DVertexNodeMapLine(const std::vector<std::string>& tokens,
                                      MeshData& mesh) {
    if (tokens.size() < 2) return "Expected VERTEX_INDEX_OR_TAG SWMM_NODE_NAME [CD] [AREA]";

    // Token is VERTEX_INDEX_OR_TAG. Try a numeric index first, but if the number
    // is out of range fall back to a tag lookup — meshes can name vertices with
    // numeric tags (e.g. "5001"), which must not be mis-read as an index and
    // rejected as out-of-range.
    bool ok = false;
    int vidx = tryParseInt(tokens[0], ok);
    if (ok && (vidx < 0 || vidx >= mesh.n_vertices()))
        ok = false;  // numeric but not a valid index — treat as a tag
    if (!ok) {
        vidx = findVertexByTag(mesh, tokens[0]);
        if (vidx < 0) return "Unknown vertex index or tag: " + tokens[0];
    }

    if (vidx < 0 || vidx >= mesh.n_vertices())
        return "Vertex index out of range: " + tokens[0];

    mesh.vert_coupled_node_name[vidx] = tokens[1];

    // Optional discharge coefficient
    if (tokens.size() >= 3) {
        double cd = tryParseDouble(tokens[2], ok);
        if (ok) mesh.vert_coupling_cd[vidx] = cd;
    }

    // Optional exchange area
    if (tokens.size() >= 4) {
        double area = tryParseDouble(tokens[3], ok);
        if (ok) {
            mesh.vert_coupling_area[vidx] = area;
            mesh.vert_coupling_area_set[vidx] = 1;
        }
    }

    return {};
}


std::string parse2DTriangleNodeMapLine(const std::vector<std::string>& tokens,
                                        MeshData& mesh) {
    if (tokens.size() < 2) return "Expected TRIANGLE_INDEX_OR_TAG SWMM_NODE_NAME [CD] [AREA]";

    // Token is TRIANGLE_INDEX_OR_TAG. Try a numeric index first, but fall back to
    // a tag lookup when the number is out of range (numeric triangle tags must
    // not be mis-read as indices and rejected).
    bool ok = false;
    int tidx = tryParseInt(tokens[0], ok);
    if (ok && (tidx < 0 || tidx >= mesh.n_triangles()))
        ok = false;  // numeric but not a valid index — treat as a tag
    if (!ok) {
        tidx = findTriangleByTag(mesh, tokens[0]);
        if (tidx < 0) return "Unknown triangle index or tag: " + tokens[0];
    }

    if (tidx < 0 || tidx >= mesh.n_triangles())
        return "Triangle index out of range: " + tokens[0];

    // Repeated-row form: every line APPENDS a coupling row, so several nodes
    // may couple to the same triangle. (Previously per-triangle arrays were
    // overwritten — last line won.)
    MeshData::TriCouplingRow row;
    row.tri       = tidx;
    row.node_name = tokens[1];

    if (tokens.size() >= 3) {
        double cd = tryParseDouble(tokens[2], ok);
        if (ok) row.cd = cd;
    }

    if (tokens.size() >= 4) {
        double area = tryParseDouble(tokens[3], ok);
        if (ok) {
            row.area = area;
            row.area_set = true;
        }
    }

    // Keep the legacy per-triangle mirror in step (last row wins), same as
    // swmm_2d_add_triangle_coupling. Consumers that run BEFORE resolve —
    // the GeoPackage writer above all — read these arrays, so leaving them
    // to SurfaceRouter2D::initialize would drop couplings on an
    // opened-but-not-initialized model.
    mesh.tri_coupled_node_name[tidx] = row.node_name;
    mesh.tri_coupling_cd[tidx]       = row.cd;
    mesh.tri_coupling_area[tidx]     = row.area;

    mesh.tri_couplings.push_back(std::move(row));

    return {};
}


// ============================================================================
// Helper: build a section-level lambda that tokenizes each line and calls
// a line-oriented parser, reporting errors via the SimulationContext.
// ============================================================================

namespace {

using LineParser = std::function<std::string(const std::vector<std::string>&)>;

input::SectionHandler makeSectionHandler(LineParser line_parser) {
    return [lp = std::move(line_parser)](
        openswmm::SimulationContext& ctx,
        const std::vector<std::string>& lines)
    {
        for (const auto& raw : lines) {
            auto tokens = openswmm::input::Tokenizer::tokenize(raw);
            if (tokens.empty()) continue;
            std::string err = lp(tokens);
            if (!err.empty()) {
                // 5 = public SWMM_ERR_PARSE. Must be non-zero so InputReader
                // (which gates success on ctx.error_code == 0) treats the
                // section as failed; previously this was 1 (SWMM_ERR_NOMEM),
                // so every 2D section parse error surfaced as "Out of memory".
                ctx.error_code    = 5;
                ctx.error_message = "[2D] " + err + " — line: " + raw;
                return;
            }
        }
    };
}

} // anonymous namespace


// ============================================================================
// V-E3 — [2D_BOUNDARY_CONDITIONS] line parser
// ============================================================================

std::string parse2DBoundaryConditionsLine(
    const std::vector<std::string>& tokens,
    std::vector<SurfaceRouter2D::PendingBoundaryRow>& pending_rows)
{
    if (tokens.empty()) return {};
    if (tokens.size() < 3) {
        return "[2D_BOUNDARY_CONDITIONS] needs TRI EDGE TYPE [PARAM_1 [PARAM_2 [GROUP]]]";
    }

    bool ok = false;
    SurfaceRouter2D::PendingBoundaryRow row;
    row.tri  = tryParseInt(tokens[0], ok);
    if (!ok || row.tri < 0)
        return "[2D_BOUNDARY_CONDITIONS] invalid TRI index";
    row.edge = tryParseInt(tokens[1], ok);
    if (!ok || row.edge < 0 || row.edge > 2)
        return "[2D_BOUNDARY_CONDITIONS] invalid EDGE (must be 0..2)";

    const std::string &type_tok = tokens[2];
    if      (iequals(type_tok, "WALL"))            row.bc_type = static_cast<int>(BoundaryType::WALL);
    else if (iequals(type_tok, "NORMAL_FLOW"))     row.bc_type = static_cast<int>(BoundaryType::NORMAL_FLOW);
    else if (iequals(type_tok, "SPECIFIED_STAGE")) row.bc_type = static_cast<int>(BoundaryType::SPECIFIED_STAGE);
    else if (iequals(type_tok, "TS_STAGE"))        row.bc_type = static_cast<int>(BoundaryType::SPECIFIED_STAGE);
    else if (iequals(type_tok, "SPECIFIED_FLOW"))  row.bc_type = static_cast<int>(BoundaryType::SPECIFIED_FLOW);
    else if (iequals(type_tok, "TS_FLOW"))         row.bc_type = static_cast<int>(BoundaryType::SPECIFIED_FLOW);
    else if (iequals(type_tok, "RATING_CURVE"))    row.bc_type = static_cast<int>(BoundaryType::RATING_CURVE);
    else return "[2D_BOUNDARY_CONDITIONS] unknown TYPE: " + type_tok;

    // PARAM_1: typed by TYPE. For *_TS variants and RATING_CURVE the
    // parameter is a name. For NORMAL_FLOW it's the slope. For
    // SPECIFIED_STAGE it's the head. For SPECIFIED_FLOW it's the
    // per-metre discharge. "*" means "no value supplied".
    std::string p1 = (tokens.size() > 3) ? tokens[3] : "*";
    if (p1 == "*") p1.clear();

    // For Wall: PARAM_1 is irrelevant.
    if (!p1.empty()) {
        if (iequals(type_tok, "NORMAL_FLOW") ||
            iequals(type_tok, "SPECIFIED_STAGE") ||
            iequals(type_tok, "SPECIFIED_FLOW"))
        {
            row.param1 = tryParseDouble(p1, ok);
            if (!ok) return "[2D_BOUNDARY_CONDITIONS] non-numeric PARAM_1: " + p1;
        } else {
            // TS or curve name.
            row.name = p1;
        }
    }

    // PARAM_2 reserved; ignored today.
    // GROUP (optional).
    if (tokens.size() > 5 && tokens[5] != "*") {
        row.group = tokens[5];
    }

    pending_rows.push_back(std::move(row));
    return {};
}


// ============================================================================
// §11A — [2D_EDGE_CONVEYANCE] line parser
// ============================================================================

std::string parse2DEdgeConveyanceLine(
    const std::vector<std::string>& tokens,
    std::vector<SurfaceRouter2D::PendingEdgeConveyanceRow>& pending_rows)
{
    if (tokens.empty()) return {};
    if (tokens.size() < 3) {
        return "[2D_EDGE_CONVEYANCE] needs FROM_VERTEX TO_VERTEX CONVEYANCE";
    }

    SurfaceRouter2D::PendingEdgeConveyanceRow row;
    bool ok = false;

    row.v_from = tryParseInt(tokens[0], ok);
    if (!ok || row.v_from < 0)
        return "[2D_EDGE_CONVEYANCE] invalid FROM_VERTEX: " + tokens[0];

    row.v_to = tryParseInt(tokens[1], ok);
    if (!ok || row.v_to < 0)
        return "[2D_EDGE_CONVEYANCE] invalid TO_VERTEX: " + tokens[1];

    if (row.v_from == row.v_to)
        return "[2D_EDGE_CONVEYANCE] FROM_VERTEX and TO_VERTEX must differ";

    row.conveyance = tryParseDouble(tokens[2], ok);
    if (!ok)
        return "[2D_EDGE_CONVEYANCE] invalid CONVEYANCE: " + tokens[2];

    // Q1 — strict [0, 1] clamp at parse time.
    if (row.conveyance < 0.0 || row.conveyance > 1.0) {
        return "[2D_EDGE_CONVEYANCE] CONVEYANCE must be in [0, 1] (got "
               + tokens[2] + ")";
    }

    pending_rows.push_back(std::move(row));
    return {};
}


// ============================================================================
// [2D_ROM] — scalar surface uncertainty ROM configuration
// ============================================================================

std::string parse2DROMLine(const std::vector<std::string>& tokens,
                           SolverOptions2D& opts) {
    if (tokens.size() < 2) return "Expected PARAMETER VALUE";

    const auto& key = tokens[0];
    const auto& val = tokens[1];
    bool ok = false;

    if (iequals(key, "ENABLE")) {
        if (iequals(val, "YES") || val == "1")
            opts.enable_rom = true;
        else if (iequals(val, "NO") || val == "0")
            opts.enable_rom = false;
        else
            return "Invalid ENABLE value (YES/NO)";
    } else if (iequals(key, "MEMBERS")) {
        int v = tryParseInt(val, ok);
        if (!ok || v < 2) return "MEMBERS must be integer >= 2";
        opts.rom_members = v;
    } else if (iequals(key, "MODES")) {
        int v = tryParseInt(val, ok);
        if (!ok || v < 1) return "MODES must be integer >= 1";
        opts.rom_modes = v;
    } else if (iequals(key, "MANNINGS_PERT")) {
        double v = tryParseDouble(val, ok);
        if (!ok || v < 0.0) return "MANNINGS_PERT must be >= 0";
        opts.rom_mannings_pert = v;
    } else if (iequals(key, "RAINFALL_PERT")) {
        double v = tryParseDouble(val, ok);
        if (!ok || v < 0.0) return "RAINFALL_PERT must be >= 0";
        opts.rom_rainfall_pert = v;
    } else if (iequals(key, "K_EFF")) {
        double v = tryParseDouble(val, ok);
        if (!ok) return "Invalid K_EFF value";
        opts.rom_k_eff = v;   // <= 0 means AUTO
    } else if (iequals(key, "WET_RESEED_FRACTION")) {
        double v = tryParseDouble(val, ok);
        if (!ok || v < 0.0 || v > 1.0) return "WET_RESEED_FRACTION must be in [0, 1]";
        opts.rom_wet_reseed_fraction = v;
    } else if (iequals(key, "WET_RESEED_MIN_INTERVAL")) {
        double v = tryParseDouble(val, ok);
        if (!ok || v < 0.0) return "WET_RESEED_MIN_INTERVAL must be >= 0";
        opts.rom_wet_reseed_min_interval = v;
    } else if (iequals(key, "PARAMETRIC_TAILS")) {
        if (iequals(val, "YES") || val == "1")
            opts.rom_parametric_tails = true;
        else if (iequals(val, "NO") || val == "0")
            opts.rom_parametric_tails = false;
        else
            return "Invalid PARAMETRIC_TAILS value (YES/NO)";
    } else if (iequals(key, "MODE_DROP_THRESHOLD")) {
        double v = tryParseDouble(val, ok);
        if (!ok || v < 0.0) return "MODE_DROP_THRESHOLD must be >= 0";
        opts.rom_mode_drop_threshold = v;
    } else if (iequals(key, "MANNINGS_CORR_LEN")) {
        double v = tryParseDouble(val, ok);
        if (!ok || v < 0.0) return "MANNINGS_CORR_LEN must be >= 0";
        opts.rom_mannings_corr_len = v;
    } else if (iequals(key, "RAINFALL_CORR_LEN")) {
        double v = tryParseDouble(val, ok);
        if (!ok || v < 0.0) return "RAINFALL_CORR_LEN must be >= 0";
        opts.rom_rainfall_corr_len = v;
    } else if (iequals(key, "LEGACY_OPERATOR")) {
        if (iequals(val, "YES") || val == "1")
            opts.rom_legacy_operator = true;
        else if (iequals(val, "NO") || val == "0")
            opts.rom_legacy_operator = false;
        else
            return "Invalid LEGACY_OPERATOR value (YES/NO)";
    } else if (iequals(key, "ALPHA_PAR")) {
        double v = tryParseDouble(val, ok);
        if (!ok || v < 0.0) return "ALPHA_PAR must be >= 0";
        opts.rom_alpha_par = v;
    } else if (iequals(key, "ALPHA_PERP")) {
        double v = tryParseDouble(val, ok);
        if (!ok || v < 0.0) return "ALPHA_PERP must be >= 0";
        opts.rom_alpha_perp = v;
    } else if (iequals(key, "C_FACTOR")) {
        double v = tryParseDouble(val, ok);
        if (!ok) return "Invalid C_FACTOR value";
        opts.rom_c_factor = v;
    } else if (iequals(key, "GROUND_SCALE")) {
        double v = tryParseDouble(val, ok);
        if (!ok || v < 0.0) return "GROUND_SCALE must be >= 0";
        opts.rom_ground_scale = v;
    } else {
        return "Unknown 2D_ROM parameter: " + key;
    }
    return {};
}


// ============================================================================
// [UNCERTAINTY] — uncertain parameter source specification
// ============================================================================

std::string parseUncertaintyLine(
    const std::vector<std::string>& tokens,
    SolverOptions2D& opts,
    openswmm::uncertainty::UncertaintyConfig& config)
{
    // Grammar (PARAMETER_REGISTRY.md §6):
    //   new    : LAYER NAME PERT [DIST] [ENTRY]     e.g. "1D INFLOW 0.3 LOGNORMAL FORCING_VECTOR"
    //   legacy : LAYER NAME [DIST] PERT             e.g. "2D RAINFALL UNIFORM 0.15"
    // Disambiguation: if tokens[2] parses as a number the new order is in
    // effect, otherwise the legacy DIST-first order.
    if (tokens.size() < 3)
        return "Expected LAYER NAME PERT [DIST] [ENTRY]";

    const auto& layer_str = tokens[0];
    const auto& param_str = tokens[1];

    bool ok = false;
    std::string dist_str  = "UNIFORM";
    std::string entry_str;                 // empty = derive from NAME
    double      pert      = 0.0;

    const double t2_num = tryParseDouble(tokens[2], ok);
    if (ok) {
        // New order: PERT [DIST] [ENTRY]
        pert = t2_num;
        // tokens[3] could be DIST or ENTRY (DIST omitted). If it's a known
        // ENTRY keyword, treat it as ENTRY and keep the default DIST.
        auto is_entry_kw = [](const std::string& s) {
            return iequals(s, "RATE_MULT") || iequals(s, "FORCING_MULT")
                || iequals(s, "FORCING_VECTOR") || iequals(s, "COUPLING_MULT")
                || iequals(s, "QUALITY_MULT");
        };
        if (tokens.size() >= 4) {
            if (is_entry_kw(tokens[3]))
                entry_str = tokens[3];
            else
                dist_str  = tokens[3];
        }
        if (tokens.size() >= 5 && entry_str.empty()) entry_str = tokens[4];
    } else {
        // Legacy order: DIST PERT
        if (tokens.size() < 4)
            return "Expected LAYER NAME PERT [DIST] [ENTRY] (or legacy LAYER NAME DIST PERT)";
        dist_str = tokens[2];
        pert = tryParseDouble(tokens[3], ok);
        if (!ok)
            return "PERTURBATION must be a non-negative number";
    }
    if (pert < 0.0)
        return "PERTURBATION must be a non-negative number";

    // --- Parse LAYER ---
    openswmm::uncertainty::LayerTarget layer;
    if (iequals(layer_str, "2D"))
        layer = openswmm::uncertainty::LayerTarget::TWO_D;
    else if (iequals(layer_str, "1D"))
        layer = openswmm::uncertainty::LayerTarget::ONE_D;
    else if (iequals(layer_str, "QUALITY"))
        layer = openswmm::uncertainty::LayerTarget::QUALITY;
    else
        return "Unsupported LAYER '" + layer_str + "' (supported: '2D', '1D', 'QUALITY')";

    // --- Parse DISTRIBUTION ---
    openswmm::uncertainty::DistType dist;
    if (iequals(dist_str, "UNIFORM"))
        dist = openswmm::uncertainty::DistType::UNIFORM;
    else if (iequals(dist_str, "NORMAL"))
        dist = openswmm::uncertainty::DistType::NORMAL;
    else if (iequals(dist_str, "LOGNORMAL"))
        dist = openswmm::uncertainty::DistType::LOGNORMAL;
    else
        return "Unknown DISTRIBUTION '" + dist_str + "' (UNIFORM / NORMAL / LOGNORMAL)";

    // --- QUALITY layer validation ---
    if (layer == openswmm::uncertainty::LayerTarget::QUALITY) {
        if (dist != openswmm::uncertainty::DistType::UNIFORM)
            return "QUALITY layer only supports UNIFORM distribution";
    }

    // --- Resolve NAME and ENTRY ---
    std::string param_upper = param_str;
    std::transform(param_upper.begin(), param_upper.end(), param_upper.begin(),
                   [](unsigned char c){ return static_cast<char>(std::toupper(c)); });

    openswmm::uncertainty::ParamEntry entry;
    bool entry_known = true;
    if (!entry_str.empty()) {
        // Explicit ENTRY — accepts any NAME (the generality win).
        if (iequals(entry_str, "RATE_MULT"))
            entry = openswmm::uncertainty::ParamEntry::RATE_MULT;
        else if (iequals(entry_str, "FORCING_MULT"))
            entry = openswmm::uncertainty::ParamEntry::FORCING_MULT;
        else if (iequals(entry_str, "FORCING_VECTOR"))
            entry = openswmm::uncertainty::ParamEntry::FORCING_VECTOR;
        else if (iequals(entry_str, "COUPLING_MULT"))
            entry = openswmm::uncertainty::ParamEntry::COUPLING_MULT;
        else if (iequals(entry_str, "QUALITY_MULT"))
            entry = openswmm::uncertainty::ParamEntry::QUALITY_MULT;
        else
            return "Unknown ENTRY '" + entry_str
                   + "' (RATE_MULT / FORCING_MULT / FORCING_VECTOR / COUPLING_MULT / QUALITY_MULT)";
    } else {
        // Name-implied default entry; unknown NAME without ENTRY is an error.
        if (param_upper == "MANNINGS_N")
            entry = openswmm::uncertainty::ParamEntry::RATE_MULT;
        else if (param_upper == "RAINFALL")
            entry = openswmm::uncertainty::ParamEntry::FORCING_MULT;
        else if (param_upper == "INFLOW")
            entry = openswmm::uncertainty::ParamEntry::FORCING_VECTOR;
        else if (layer == openswmm::uncertainty::LayerTarget::QUALITY)
            entry = openswmm::uncertainty::ParamEntry::QUALITY_MULT;
        else
            entry_known = false;
        if (!entry_known)
            return "Unknown PARAMETER '" + param_str
                   + "' without an explicit ENTRY (known names: MANNINGS_N, "
                     "RAINFALL, INFLOW; or supply RATE_MULT / FORCING_MULT / "
                     "FORCING_VECTOR / COUPLING_MULT)";
    }

    // --- Build spec and record ---
    openswmm::uncertainty::UncertaintySourceSpec spec;
    spec.name         = param_upper;
    spec.layer        = layer;
    spec.dist         = dist;
    spec.perturbation = pert;
    spec.entry        = entry;
    config.sources.push_back(spec);

    // For 2D specs, [UNCERTAINTY] takes precedence over [2D_ROM] values (this
    // section is conventionally the later one in the file). 1D/QUALITY specs
    // only update config — nothing on this base consumes them yet (the 1D ROM
    // lifecycle is a separate track); recording them now means no re-parse is
    // needed once that lands.
    if (layer == openswmm::uncertainty::LayerTarget::TWO_D) {
        opts.enable_rom = true;
        if (param_upper == "MANNINGS_N")
            opts.rom_mannings_pert = pert;
        else if (param_upper == "RAINFALL")
            opts.rom_rainfall_pert = pert;
    }

    return {};
}


// ============================================================================
// register2DSections
// ============================================================================

void register2DSections(MeshData& mesh,
                        SolverOptions2D& options,
                        std::vector<SurfaceRouter2D::PendingBoundaryRow>& pending_bc_rows,
                        std::vector<SurfaceRouter2D::PendingEdgeConveyanceRow>& pending_ec_rows,
                        openswmm::uncertainty::UncertaintyConfig& uncertainty_config,
                        input::SectionRegistry& registry)
{
    // Full-form handler (not makeSectionHandler): parse2DOptionsLine needs
    // ctx.warnings so retired CVODE-era keys warn-and-ignore on file load.
    registry.register_custom("2D_OPTIONS",
        [&options](openswmm::SimulationContext& ctx,
                   const std::vector<std::string>& lines)
        {
            for (const auto& raw : lines) {
                auto tokens = openswmm::input::Tokenizer::tokenize(raw);
                if (tokens.empty()) continue;
                std::string err =
                    parse2DOptionsLine(tokens, options, &ctx.warnings);
                if (!err.empty()) {
                    ctx.error_code    = 5;  // SWMM_ERR_PARSE (see makeSectionHandler)
                    ctx.error_message = "[2D] " + err + " — line: " + raw;
                    return;
                }
            }
        });

    registry.register_custom("2D_VERTICES",
        makeSectionHandler([&mesh](const std::vector<std::string>& tokens) {
            return parse2DVertexLine(tokens, mesh);
        }));

    registry.register_custom("2D_TRIANGLES",
        makeSectionHandler([&mesh](const std::vector<std::string>& tokens) {
            return parse2DTriangleLine(tokens, mesh);
        }));

    registry.register_custom("2D_VERTEX_NODE_MAP",
        makeSectionHandler([&mesh](const std::vector<std::string>& tokens) {
            return parse2DVertexNodeMapLine(tokens, mesh);
        }));

    registry.register_custom("2D_TRIANGLE_NODE_MAP",
        makeSectionHandler([&mesh](const std::vector<std::string>& tokens) {
            return parse2DTriangleNodeMapLine(tokens, mesh);
        }));

    // V-E3 — [2D_BOUNDARY_CONDITIONS] accumulates pending rows; drained
    // during SurfaceRouter2D::initialize() after BoundaryData::resize.
    registry.register_custom("2D_BOUNDARY_CONDITIONS",
        makeSectionHandler([&pending_bc_rows](const std::vector<std::string>& tokens) {
            return parse2DBoundaryConditionsLine(tokens, pending_bc_rows);
        }));

    // §11A — [2D_EDGE_CONVEYANCE] accumulates pending rows; drained
    // during SurfaceRouter2D::initialize() after buildMeshTopology so
    // the vertex-pair → (tri, edge_local) lookup table is available.
    registry.register_custom("2D_EDGE_CONVEYANCE",
        makeSectionHandler([&pending_ec_rows](const std::vector<std::string>& tokens) {
            return parse2DEdgeConveyanceLine(tokens, pending_ec_rows);
        }));

    // [2D_MESH_FILE] — capture only the first FILE token; mesh is loaded
    // after the main .inp is fully parsed (see SWMMEngine::open).
    registry.register_custom("2D_MESH_FILE",
        [&options](openswmm::SimulationContext& /*ctx*/,
                   const std::vector<std::string>& lines)
        {
            for (const auto& raw : lines) {
                auto tokens = openswmm::input::Tokenizer::tokenize(raw);
                if (tokens.size() >= 2 && iequals(tokens[0], "FILE")) {
                    options.mesh_file = tokens[1];
                    return; // only first FILE line
                }
            }
        });

    // [2D_ROM] — scalar ROM configuration (enable, size, perturbations,
    // K_eff, and the W3-calibrated reduced-operator dials).
    registry.register_custom("2D_ROM",
        makeSectionHandler([&options](const std::vector<std::string>& tokens) {
            return parse2DROMLine(tokens, options);
        }));

    // [UNCERTAINTY] — uncertain parameter source specification. Entries here
    // OVERRIDE the corresponding scalar fields in [2D_ROM] for the 2D layer.
    registry.register_custom("UNCERTAINTY",
        makeSectionHandler([&options, &uncertainty_config](
                              const std::vector<std::string>& tokens) {
            return parseUncertaintyLine(tokens, options, uncertainty_config);
        }));
}


// ============================================================================
// load2DMeshExternalFile
// ============================================================================

std::string load2DMeshExternalFile(MeshData& mesh,
                                   SolverOptions2D& opts,
                                   std::vector<SurfaceRouter2D::PendingBoundaryRow>& pending_bc_rows,
                                   std::vector<SurfaceRouter2D::PendingEdgeConveyanceRow>& pending_ec_rows,
                                   const std::string& mesh_file,
                                   const std::string& inp_base_dir,
                                   std::vector<std::string>* warnings)
{
    namespace fs = std::filesystem;

    // Resolve path
    fs::path p(mesh_file);
    if (p.is_relative() && !inp_base_dir.empty())
        p = fs::path(inp_base_dir) / p;

    // Build a minimal registry (no 2D_MESH_FILE — prevents recursion)
    openswmm::input::SectionRegistry mini;
    mini.register_custom("2D_OPTIONS",
        makeSectionHandler([&opts, warnings](const std::vector<std::string>& tokens) {
            return parse2DOptionsLine(tokens, opts, warnings);
        }));
    mini.register_custom("2D_VERTICES",
        makeSectionHandler([&mesh](const std::vector<std::string>& tokens) {
            return parse2DVertexLine(tokens, mesh);
        }));
    mini.register_custom("2D_TRIANGLES",
        makeSectionHandler([&mesh](const std::vector<std::string>& tokens) {
            return parse2DTriangleLine(tokens, mesh);
        }));
    mini.register_custom("2D_VERTEX_NODE_MAP",
        makeSectionHandler([&mesh](const std::vector<std::string>& tokens) {
            return parse2DVertexNodeMapLine(tokens, mesh);
        }));
    mini.register_custom("2D_TRIANGLE_NODE_MAP",
        makeSectionHandler([&mesh](const std::vector<std::string>& tokens) {
            return parse2DTriangleNodeMapLine(tokens, mesh);
        }));

    // V-E3 — external .2dm may carry its own [2D_BOUNDARY_CONDITIONS].
    mini.register_custom("2D_BOUNDARY_CONDITIONS",
        makeSectionHandler([&pending_bc_rows](const std::vector<std::string>& tokens) {
            return parse2DBoundaryConditionsLine(tokens, pending_bc_rows);
        }));

    // §11A — external .2dm may carry its own [2D_EDGE_CONVEYANCE].
    mini.register_custom("2D_EDGE_CONVEYANCE",
        makeSectionHandler([&pending_ec_rows](const std::vector<std::string>& tokens) {
            return parse2DEdgeConveyanceLine(tokens, pending_ec_rows);
        }));

    // The external .2dm may carry its own `;; UNITS:` header. Scan first
    // so SurfaceRouter2D::initialize sees the right flag before it runs.
    prescan2DUnitsHeader(p.string(), opts);

    openswmm::input::InputReader reader(mini);
    openswmm::SimulationContext  dummy;
    if (!reader.read(p.string(), dummy)) {
        return "2D_MESH_FILE: error reading '" + p.string() + "': " + dummy.error_message;
    }
    return {};
}

// ============================================================================
// prescan2DUnitsHeader
// ============================================================================

void prescan2DUnitsHeader(const std::string& inp_path, SolverOptions2D& opts)
{
    std::ifstream in(inp_path);
    if (!in) return;  // file missing — caller will surface the error

    auto trim = [](std::string s) {
        const auto issp = [](unsigned char c) { return std::isspace(c) != 0; };
        while (!s.empty() && issp(static_cast<unsigned char>(s.back())))   s.pop_back();
        std::size_t i = 0;
        while (i < s.size() && issp(static_cast<unsigned char>(s[i]))) ++i;
        return s.substr(i);
    };

    std::string line;
    while (std::getline(in, line)) {
        const std::string t = trim(line);
        if (t.size() < 2 || t[0] != ';' || t[1] != ';') continue;
        std::string rest = trim(t.substr(2));
        // Match "UNITS:" prefix case-insensitively.
        constexpr std::string_view kKey = "UNITS:";
        if (rest.size() < kKey.size()) continue;
        bool match = true;
        for (std::size_t i = 0; i < kKey.size(); ++i) {
            if (std::toupper(static_cast<unsigned char>(rest[i])) != kKey[i]) {
                match = false; break;
            }
        }
        if (!match) continue;
        const std::string value = trim(rest.substr(kKey.size()));
        // Recognised metric markers.  Anything else (including absent /
        // unknown / explicit "ft") leaves the flag at its current value.
        const bool si =
               iequals(value, "SI (m)")
            || iequals(value, "m")
            || iequals(value, "metre")
            || iequals(value, "metres")
            || iequals(value, "meter")
            || iequals(value, "meters");
        if (si) opts.mesh_units_si = true;
        // We keep scanning so a later UNITS: line in the same file wins.
    }
}

} // namespace openswmm::twoD
