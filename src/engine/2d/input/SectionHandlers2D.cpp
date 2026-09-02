/**
 * @file SectionHandlers2D.cpp
 * @brief Implementation of 2D input section parsers.
 *
 * @see SectionHandlers2D.hpp
 * @ingroup engine_2d
 */

#include "SectionHandlers2D.hpp"

#include "../data/BoundaryData.hpp"
#include "../../input/InputParseUtils.hpp"
#include "../../input/InputReader.hpp"
#include "../../input/Tokenizer.hpp"
#include "../../core/ErrorCodes.hpp"
#include "../../core/SimulationContext.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>

namespace openswmm::twoD {

namespace {

// Case-insensitive string comparison. Takes views so callers on the
// allocation-free scan paths (prescan2DUnitsHeader) need no temporaries;
// std::string arguments still convert implicitly.
bool iequals(std::string_view a, std::string_view b) {
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
    "ADVECTION, MAX_TIMESTEP, COUPLING_AREA.";

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
    } else if (iequals(key, "DISPERSION")) {
        // S2: isotropic species dispersion, m2/s. Refused negative, not
        // clamped (the 1D [TRANSPORT_OPTIONS] DISPERSION convention).
        opts.dispersion = tryParseDouble(val, ok);
        if (!ok || !std::isfinite(opts.dispersion) || opts.dispersion < 0.0)
            return "Invalid DISPERSION value (m2/s, must be finite and >= 0)";
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
    } else if (iequals(key, "ADVECTION")) {
        if (iequals(val, "YES") || iequals(val, "ON") || iequals(val, "TRUE"))
            opts.advection = true;
        else if (iequals(val, "NO") || iequals(val, "OFF") ||
                 iequals(val, "FALSE"))
            opts.advection = false;
        else
            return "Unknown ADVECTION: " + val + " (expected YES|NO)";
    } else if (iequals(key, "COUPLING_AREA")) {
        if (iequals(val, "AUTO"))
            opts.coupling_area_auto = true;
        else if (iequals(val, "DEFAULT"))
            opts.coupling_area_auto = false;
        else
            return "Unknown COUPLING_AREA: " + val + " (expected AUTO|DEFAULT)";
    } else if (iequals(key, "BACKEND")) {
        // Same token set as [OPTIONS] FV_BACKEND; unknown tokens are rejected
        // so a typo surfaces as a failed set instead of a silent AUTO.
        if      (iequals(val, "AUTO")) opts.backend = Backend2D::AUTO;
        else if (iequals(val, "CPU"))  opts.backend = Backend2D::CPU;
        else if (iequals(val, "OMP"))  opts.backend = Backend2D::OMP;
        else if (iequals(val, "CUDA")) opts.backend = Backend2D::CUDA;
        else if (iequals(val, "HIP"))  opts.backend = Backend2D::HIP;
        else if (iequals(val, "SYCL")) opts.backend = Backend2D::SYCL;
        else
            return "Unknown BACKEND: " + val +
                   " (expected AUTO|CPU|OMP|CUDA|HIP|SYCL)";
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
        "LTS_TIERS", "FROUDE_MAX", "ADVECTION", "COUPLING_AREA",
        "BACKEND",
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
    if (iequals(key, "ADVECTION"))     return opts.advection ? "YES" : "NO";
    if (iequals(key, "COUPLING_AREA")) return opts.coupling_area_auto ? "AUTO" : "DEFAULT";
    if (iequals(key, "BACKEND")) {
        switch (opts.backend) {
            case Backend2D::CPU:  return "CPU";
            case Backend2D::OMP:  return "OMP";
            case Backend2D::CUDA: return "CUDA";
            case Backend2D::HIP:  return "HIP";
            case Backend2D::SYCL: return "SYCL";
            case Backend2D::AUTO: break;
        }
        return "AUTO";
    }
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
    if (tokens.size() < 4)
        return "Expected V1 V2 V3 MANNINGS_N [INIT_DEPTH] [TAG]";

    bool ok = false;
    int v0 = tryParseInt(tokens[0], ok);
    if (!ok) return "Invalid V1 index";

    int v1 = tryParseInt(tokens[1], ok);
    if (!ok) return "Invalid V2 index";

    int v2 = tryParseInt(tokens[2], ok);
    if (!ok) return "Invalid V3 index";

    double n = tryParseDouble(tokens[3], ok);
    if (!ok) return "Invalid MANNINGS_N value";

    // Optional column 5: INIT_DEPTH when numeric (m, >= 0, default 0 = dry),
    // otherwise it is the TAG (backward compatible with the historical
    // `V1 V2 V3 MANNINGS_N TAG` form). Column 6 is TAG when INIT_DEPTH is
    // present. Files written by the engine/GUI always emit INIT_DEPTH when a
    // tag exists, so round-tripped files are unambiguous.
    double init_depth = 0.0;
    std::string tag;
    if (tokens.size() >= 5) {
        bool num = false;
        double d = tryParseDouble(tokens[4], num);
        if (num) {
            if (d < 0.0) return "Invalid INIT_DEPTH (must be >= 0)";
            init_depth = d;
            if (tokens.size() >= 6) tag = tokens[5];
        } else {
            tag = tokens[4];
        }
    }

    int idx = mesh.n_triangles();
    mesh.resize_triangles(idx + 1);
    mesh.tri_v0[idx] = v0;
    mesh.tri_v1[idx] = v1;
    mesh.tri_v2[idx] = v2;
    mesh.mannings_n[idx] = n;
    mesh.tri_init_depth[idx] = init_depth;
    mesh.tri_tag[idx] = tag;

    return {};
}


// [2D_INITIAL_VELOCITY] — optional per-triangle initial velocity (m/s):
//   TRI  U  V
// Default is (0, 0); rows may cover any subset of triangles. The explicit
// marcher projects (h·u, h·v) onto its face normals at initialize to seed the
// prognostic face discharges (a depth-only IC cannot represent solutions with
// v(t=0) ≠ 0, e.g. the SWASHES Thacker planar oscillation).
std::string parse2DInitialVelocityLine(const std::vector<std::string>& tokens,
                                       MeshData& mesh) {
    if (tokens.size() < 3) return "Expected TRI U V";

    bool ok = false;
    int tri = tryParseInt(tokens[0], ok);
    if (!ok || tri < 0 || tri >= mesh.n_triangles())
        return "Invalid triangle index: " + tokens[0];

    double u = tryParseDouble(tokens[1], ok);
    if (!ok) return "Invalid U value";
    double v = tryParseDouble(tokens[2], ok);
    if (!ok) return "Invalid V value";

    mesh.tri_init_u[tri] = u;
    mesh.tri_init_v[tri] = v;
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
// §5.5 track I — [2D_INFILTRATION_OPTIONS] / [2D_INFILTRATION_DEFAULTS] /
// [2D_INFILTRATION] line parsers
// ============================================================================

namespace {

// Shared tail of a [2D_INFILTRATION_DEFAULTS] / [2D_INFILTRATION] row:
//
//     METHOD  [P1 .. P5]  [DEST]
//
// starting at tokens[first]. Parameter columns are POSITIONAL and carry
// PROJECT UNITS — they are stored verbatim; Infil2D::resolve() does the
// conversion. "-" means "unset" and leaves the slot at its default 0.
// Trailing columns a method does not use may be omitted (the writer trims
// them with infil2DParamCount), so a row's length varies by method. DEST is
// recognised as the final token when it is neither numeric nor "-"; absent,
// the Infil2DRow default (LOST) stands.
std::string parseInfil2DRowTail(const std::vector<std::string>& tokens,
                                std::size_t first,
                                const char* section,
                                Infil2DRow& row)
{
    if (tokens.size() <= first)
        return std::string(section) + " missing METHOD";

    if (!parseInfil2DMethod(tokens[first], row.method, row.has_method))
        return std::string(section) + " unknown METHOD: " + tokens[first];

    std::size_t end = tokens.size();
    if (end > first + 1) {
        const std::string& last = tokens[end - 1];
        bool numeric = false;
        (void)tryParseDouble(last, numeric);
        if (!numeric && last != "-") {
            if (!parseInfil2DDest(last, row.dest))
                return std::string(section) + " unknown DEST: " + last;
            --end;
        }
    }

    for (std::size_t k = first + 1; k < end; ++k) {
        const std::size_t idx = k - (first + 1);
        if (idx >= static_cast<std::size_t>(kInfil2DMaxParams))
            return std::string(section) + " too many parameter columns (max "
                   + std::to_string(kInfil2DMaxParams) + ")";
        if (tokens[k] == "-") continue;  // unset
        bool ok = false;
        row.p[idx] = tryParseDouble(tokens[k], ok);
        if (!ok)
            return std::string(section) + " invalid parameter: " + tokens[k];
    }

    return {};
}

// makeSectionHandler's sibling for the [2D_INFILTRATION*] family. Those
// sections write into the Infil2D owned by SurfaceRouter2D, which — unlike the
// mesh, options and pending-row buffers — is reached through
// ctx.twod_io.infil rather than a captured reference. Null (engine built
// without 2D, or a detached context) means the section is skipped, matching
// how every other twod_io consumer runtime-guards.
using InfilLineParser =
    std::function<std::string(const std::vector<std::string>&, Infil2D&)>;

input::SectionHandler makeInfilSectionHandler(InfilLineParser line_parser) {
    return [lp = std::move(line_parser)](
        openswmm::SimulationContext& ctx,
        const std::vector<std::string>& lines)
    {
        Infil2D* infil = ctx.twod_io.infil;
        if (!infil) return;
        for (const auto& raw : lines) {
            auto tokens = openswmm::input::Tokenizer::tokenize(raw);
            if (tokens.empty()) continue;
            std::string err = lp(tokens, *infil);
            if (!err.empty()) {
                ctx.error_code    = 5;  // SWMM_ERR_PARSE (see makeSectionHandler)
                ctx.error_message = "[2D] " + err + " — line: " + raw;
                return;
            }
        }
    };
}

} // anonymous namespace


std::string parse2DInfiltrationOptionsLine(
    const std::vector<std::string>& tokens, Infil2D& infil)
{
    if (tokens.empty()) return {};
    if (tokens.size() < 2)
        return "[2D_INFILTRATION_OPTIONS] needs PARAMETER VALUE";

    if (!iequals(tokens[0], "INFIL_STEP"))
        return "Unknown 2D_INFILTRATION_OPTIONS parameter: " + tokens[0];

    // Same duration grammar as WET_STEP / DRY_STEP in [OPTIONS].
    const double secs = openswmm::input::parse_time_seconds(tokens[1]);
    if (secs < 0.0)
        return "[2D_INFILTRATION_OPTIONS] invalid INFIL_STEP (expected "
               "hh:mm:ss >= 0): " + tokens[1];

    infil.options().infil_step = secs;
    return {};
}


std::string parse2DInfiltrationDefaultsLine(
    const std::vector<std::string>& tokens, Infil2D& infil)
{
    if (tokens.empty()) return {};
    if (tokens.size() < 2)
        return "[2D_INFILTRATION_DEFAULTS] needs TAG METHOD [P1..P5] [DEST]";

    Infil2DDefault entry;
    entry.tag = tokens[0];

    const std::string err = parseInfil2DRowTail(
        tokens, 1, "[2D_INFILTRATION_DEFAULTS]", entry.row);
    if (!err.empty()) return err;

    infil.defaults().push_back(std::move(entry));
    return {};
}


std::string parse2DInfiltrationLine(
    const std::vector<std::string>& tokens, Infil2D& infil)
{
    if (tokens.empty()) return {};
    if (tokens.size() < 2)
        return "[2D_INFILTRATION] needs CELL METHOD [P1..P5] [DEST]";

    // CELL is 1-BASED in the file, 0-based in Infil2DOverride::tri. Only the
    // lower bound is checked here — the mesh may not be loaded yet, so the
    // upper bound is Infil2D::resolve()'s job.
    bool ok = false;
    const int cell = tryParseInt(tokens[0], ok);
    if (!ok || cell < 1)
        return "[2D_INFILTRATION] invalid CELL index (1-based): " + tokens[0];

    Infil2DOverride entry;
    entry.tri = cell - 1;

    const std::string err = parseInfil2DRowTail(
        tokens, 1, "[2D_INFILTRATION]", entry.row);
    if (!err.empty()) return err;

    infil.overrides().push_back(std::move(entry));
    return {};
}


// ============================================================================
// [2D_INITIAL_QUALITY] — overland transport S1/S2
// ============================================================================

std::string parse2DInitialQualityLine(
    const std::vector<std::string>& tokens,
    std::vector<SurfaceRouter2D::PendingInitialQualityRow>& rows)
{
    if (tokens.empty()) return {};
    // CELL <n> <species> <conc>  |  TAG <name> <species> <conc>  |  * <species> <conc>
    SurfaceRouter2D::PendingInitialQualityRow r;
    std::size_t at = 0;
    if (tokens[0] == "*") {
        if (tokens.size() != 3)
            return "[2D_INITIAL_QUALITY] '*' row needs SPECIES CONC";
        r.all = true;
        at = 1;
    } else if (iequals(tokens[0], "CELL")) {
        if (tokens.size() != 4)
            return "[2D_INITIAL_QUALITY] CELL row needs CELL <n> SPECIES CONC";
        bool ok = false;
        const int cell = tryParseInt(tokens[1], ok);
        if (!ok || cell < 1)
            return "[2D_INITIAL_QUALITY] invalid CELL index (1-based): " +
                   tokens[1];
        r.tri = cell - 1;   // upper bound is the router's, once the mesh exists
        at = 2;
    } else if (iequals(tokens[0], "TAG")) {
        if (tokens.size() != 4)
            return "[2D_INITIAL_QUALITY] TAG row needs TAG <name> SPECIES CONC";
        r.tag = tokens[1];
        at = 2;
    } else {
        return "[2D_INITIAL_QUALITY] row must start with CELL, TAG or '*': " +
               tokens[0];
    }
    r.species = tokens[at];
    bool okc = false;
    r.conc = tryParseDouble(tokens[at + 1], okc);
    // Refused, not clamped: a negative initial concentration is not a
    // modelling case, and a non-finite one is a typo that would otherwise
    // become NaN mass across the mesh.
    if (!okc || !std::isfinite(r.conc) || r.conc < 0.0)
        return "[2D_INITIAL_QUALITY] CONC must be a finite non-negative "
               "number, got '" + tokens[at + 1] + "'";
    rows.push_back(std::move(r));
    return {};
}

// ============================================================================
// [2D_BOUNDARY_QUALITY] — overland transport S2
// ============================================================================

std::string parse2DBoundaryQualityLine(
    const std::vector<std::string>& tokens,
    std::vector<SurfaceRouter2D::PendingBoundaryQualityRow>& rows)
{
    if (tokens.empty()) return {};
    if (tokens.size() != 4)
        return "[2D_BOUNDARY_QUALITY] needs TRI EDGE SPECIES CONC";
    bool ok = false;
    SurfaceRouter2D::PendingBoundaryQualityRow r;
    r.tri = tryParseInt(tokens[0], ok);
    if (!ok || r.tri < 0)
        return "[2D_BOUNDARY_QUALITY] invalid TRI index: " + tokens[0];
    r.edge = tryParseInt(tokens[1], ok);
    if (!ok || r.edge < 0 || r.edge > 2)
        return "[2D_BOUNDARY_QUALITY] invalid EDGE (must be 0..2): " + tokens[1];
    r.species = tokens[2];
    bool okc = false;
    r.conc = tryParseDouble(tokens[3], okc);
    if (!okc || !std::isfinite(r.conc) || r.conc < 0.0)
        return "[2D_BOUNDARY_QUALITY] CONC must be a finite non-negative "
               "number, got '" + tokens[3] + "'";
    rows.push_back(std::move(r));
    return {};
}

// ============================================================================
// register2DSections
// ============================================================================

void register2DSections(MeshData& mesh,
                        SolverOptions2D& options,
                        std::vector<SurfaceRouter2D::PendingBoundaryRow>& pending_bc_rows,
                        std::vector<SurfaceRouter2D::PendingEdgeConveyanceRow>& pending_ec_rows,
                        std::vector<SurfaceRouter2D::PendingInitialQualityRow>& pending_iq_rows,
                        std::vector<SurfaceRouter2D::PendingBoundaryQualityRow>& pending_bq_rows,
                        input::SectionRegistry& registry)
{
    // S1/S2: initial surface species concentration. Main .inp only in this
    // round — the .2dm sidecar's mini-registry does not carry it (recorded).
    registry.register_custom("2D_INITIAL_QUALITY",
        makeSectionHandler([&pending_iq_rows](const std::vector<std::string>& tokens) {
            return parse2DInitialQualityLine(tokens, pending_iq_rows);
        }));
    // S2: inflow concentration on a non-WALL boundary edge. Main .inp only.
    registry.register_custom("2D_BOUNDARY_QUALITY",
        makeSectionHandler([&pending_bq_rows](const std::vector<std::string>& tokens) {
            return parse2DBoundaryQualityLine(tokens, pending_bq_rows);
        }));

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

    registry.register_custom("2D_INITIAL_VELOCITY",
        makeSectionHandler([&mesh](const std::vector<std::string>& tokens) {
            return parse2DInitialVelocityLine(tokens, mesh);
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

    // §5.5 track I — per-cell infiltration. Rows land in the Infil2D reached
    // through ctx.twod_io.infil and are resolved against the mesh (tag/cell
    // precedence) in SurfaceRouter2D::initialize().
    registry.register_custom("2D_INFILTRATION_OPTIONS",
        makeInfilSectionHandler(parse2DInfiltrationOptionsLine));

    registry.register_custom("2D_INFILTRATION_DEFAULTS",
        makeInfilSectionHandler(parse2DInfiltrationDefaultsLine));

    registry.register_custom("2D_INFILTRATION",
        makeInfilSectionHandler(parse2DInfiltrationLine));

    // [2D_MESH_FILE] — capture only the first FILE token; mesh is loaded
    // after the main .inp is fully parsed (see SWMMEngine::open).
    registry.register_custom("2D_MESH_FILE",
        [&options](openswmm::SimulationContext& /*ctx*/,
                   const std::vector<std::string>& lines)
        {
            for (const auto& raw : lines) {
                auto tokens = openswmm::input::Tokenizer::tokenize(raw);
                if (tokens.size() >= 2 && iequals(tokens[0], "FILE")) {
                    // Path may contain spaces; the tokenizer split an unquoted
                    // path, so rejoin the tokens after FILE (a quoted path is a
                    // single token already). Fixes meshes like "My Model.2dm".
                    std::string path = tokens[1];
                    for (std::size_t k = 2; k < tokens.size(); ++k) { path += ' '; path += tokens[k]; }
                    if (path.size() >= 2 && path.front() == '"' && path.back() == '"')
                        path = path.substr(1, path.size() - 2);
                    options.mesh_file = path;
                    return; // only first FILE line
                }
            }
        });
}


// ============================================================================
// load2DMeshExternalFile
// ============================================================================

std::string load2DMeshExternalFile(MeshData& mesh,
                                   SolverOptions2D& opts,
                                   std::vector<SurfaceRouter2D::PendingBoundaryRow>& pending_bc_rows,
                                   std::vector<SurfaceRouter2D::PendingEdgeConveyanceRow>& pending_ec_rows,
                                   Infil2D* infil,
                                   const std::string& mesh_file,
                                   const std::string& inp_base_dir,
                                   std::vector<std::string>* warnings,
                                   std::vector<SurfaceRouter2D::PendingInitialQualityRow>* pending_iq_rows,
                                   std::vector<SurfaceRouter2D::PendingBoundaryQualityRow>* pending_bq_rows)
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
    mini.register_custom("2D_INITIAL_VELOCITY",
        makeSectionHandler([&mesh](const std::vector<std::string>& tokens) {
            return parse2DInitialVelocityLine(tokens, mesh);
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

    // S2/S3 — external .2dm may carry the surface quality sections too (they
    // travel with the mesh they address). Registered only when the caller
    // passes the pending stores, so older callers are unchanged.
    if (pending_iq_rows)
        mini.register_custom("2D_INITIAL_QUALITY",
            makeSectionHandler([pending_iq_rows](const std::vector<std::string>& tokens) {
                return parse2DInitialQualityLine(tokens, *pending_iq_rows);
            }));
    if (pending_bq_rows)
        mini.register_custom("2D_BOUNDARY_QUALITY",
            makeSectionHandler([pending_bq_rows](const std::vector<std::string>& tokens) {
                return parse2DBoundaryQualityLine(tokens, *pending_bq_rows);
            }));

    // §11A — external .2dm may carry its own [2D_EDGE_CONVEYANCE].
    mini.register_custom("2D_EDGE_CONVEYANCE",
        makeSectionHandler([&pending_ec_rows](const std::vector<std::string>& tokens) {
            return parse2DEdgeConveyanceLine(tokens, pending_ec_rows);
        }));

    // §5.5.5 — [2D_INFILTRATION*] are per-cell mesh attributes, so they follow
    // the mesh into the .2dm. The registry the main .inp uses reaches its
    // target through ctx.twod_io.infil, which the detached context below does
    // not carry, so the sidecar rows are parsed into a scratch object bound
    // here by reference and transplanted after the read. Per SECTION
    // precedence: a section the sidecar carries REPLACES the inline one
    // (external overrides inline, the [2D_MESH_FILE] rule) rather than
    // appending to it, so no row can be counted twice; sections the sidecar
    // omits keep whatever the .inp supplied.
    Infil2D sidecar_infil;
    mini.register_custom("2D_INFILTRATION_OPTIONS",
        makeSectionHandler([&sidecar_infil](const std::vector<std::string>& tokens) {
            return parse2DInfiltrationOptionsLine(tokens, sidecar_infil);
        }));
    mini.register_custom("2D_INFILTRATION_DEFAULTS",
        makeSectionHandler([&sidecar_infil](const std::vector<std::string>& tokens) {
            return parse2DInfiltrationDefaultsLine(tokens, sidecar_infil);
        }));
    mini.register_custom("2D_INFILTRATION",
        makeSectionHandler([&sidecar_infil](const std::vector<std::string>& tokens) {
            return parse2DInfiltrationLine(tokens, sidecar_infil);
        }));

    // The external .2dm may carry its own `;; UNITS:` header. Scan first
    // so SurfaceRouter2D::initialize sees the right flag before it runs.
    prescan2DUnitsHeader(p.string(), opts);

    openswmm::input::InputReader reader(mini);
    openswmm::SimulationContext  dummy;
    if (!reader.read(p.string(), dummy)) {
        return "2D_MESH_FILE: error reading '" + p.string() + "': " + dummy.error_message;
    }

    if (infil != nullptr) {
        if (!sidecar_infil.defaults().empty())
            infil->defaults() = std::move(sidecar_infil.defaults());
        if (!sidecar_infil.overrides().empty())
            infil->overrides() = std::move(sidecar_infil.overrides());
        if (sidecar_infil.options().infil_step > 0.0)
            infil->options() = sidecar_infil.options();
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

    // Views, not strings. This pass reads the ENTIRE .inp — see the note below
    // on why it cannot stop early — so on a large model it visits millions of
    // lines. The previous by-value `trim(std::string)` allocated twice per
    // line (the parameter copy and the substr result) for a scan that almost
    // always finds nothing.
    const auto trim = [](std::string_view s) noexcept {
        const auto issp = [](unsigned char c) { return std::isspace(c) != 0; };
        while (!s.empty() && issp(static_cast<unsigned char>(s.back())))
            s.remove_suffix(1);
        while (!s.empty() && issp(static_cast<unsigned char>(s.front())))
            s.remove_prefix(1);
        return s;
    };

    // NB: this deliberately scans to EOF rather than stopping at the first
    // "[SECTION]" header. The header is not always in the pre-section prefix —
    // InpWriter emits `;; UNITS: SI (m)` underneath [2D_VERTICES] (InpWriter.cpp
    // writeMesh2D), so every .inp the engine itself writes carries it mid-file.
    // Stopping early would silently drop SI mesh scaling on round-trip.
    // Last match wins, matching the previous behaviour.
    std::string line;
    while (std::getline(in, line)) {
        const std::string_view t = trim(line);
        if (t.size() < 2 || t[0] != ';' || t[1] != ';') continue;
        std::string_view rest = trim(t.substr(2));
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
        const std::string_view value = trim(rest.substr(kKey.size()));
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
