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
#include "../../core/SimulationContext.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <stdexcept>

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

} // anonymous namespace


std::string parse2DOptionsLine(const std::vector<std::string>& tokens,
                                SolverOptions2D& opts) {
    if (tokens.size() < 2) return "Expected PARAMETER VALUE";

    const auto& key = tokens[0];
    const auto& val = tokens[1];
    bool ok = false;

    if (iequals(key, "MAX_TIMESTEP")) {
        opts.max_timestep = tryParseDouble(val, ok);
        if (!ok) return "Invalid MAX_TIMESTEP value";
    } else if (iequals(key, "MIN_TIMESTEP")) {
        opts.min_timestep = tryParseDouble(val, ok);
        if (!ok) return "Invalid MIN_TIMESTEP value";
    } else if (iequals(key, "REL_TOLERANCE")) {
        opts.rel_tolerance = tryParseDouble(val, ok);
        if (!ok) return "Invalid REL_TOLERANCE value";
    } else if (iequals(key, "ABS_TOLERANCE")) {
        opts.abs_tolerance = tryParseDouble(val, ok);
        if (!ok) return "Invalid ABS_TOLERANCE value";
    } else if (iequals(key, "DRY_DEPTH")) {
        opts.dry_depth = tryParseDouble(val, ok);
        if (!ok) return "Invalid DRY_DEPTH value";
    } else if (iequals(key, "MAX_KRYLOV_DIM")) {
        opts.max_krylov_dim = tryParseInt(val, ok);
        if (!ok) return "Invalid MAX_KRYLOV_DIM value";
    } else if (iequals(key, "COUPLING_INTERVAL")) {
        opts.coupling_interval = tryParseInt(val, ok);
        if (!ok) return "Invalid COUPLING_INTERVAL value";
    } else if (iequals(key, "COUPLING_CD")) {
        opts.coupling_cd = tryParseDouble(val, ok);
        if (!ok) return "Invalid COUPLING_CD value";
    } else if (iequals(key, "LIMITER_EPSILON")) {
        opts.limiter_epsilon = tryParseDouble(val, ok);
        if (!ok) return "Invalid LIMITER_EPSILON value";
    } else if (iequals(key, "MAX_CVODE_STEPS")) {
        opts.max_cvode_steps = tryParseInt(val, ok);
        if (!ok) return "Invalid MAX_CVODE_STEPS value";
    } else if (iequals(key, "LINEAR_SOLVER")) {
        if (iequals(val, "GMRES"))
            opts.linear_solver = LinearSolverType::GMRES;
        else if (iequals(val, "BICGSTAB"))
            opts.linear_solver = LinearSolverType::BICGSTAB;
        else if (iequals(val, "TFQMR"))
            opts.linear_solver = LinearSolverType::TFQMR;
        else
            return "Unknown LINEAR_SOLVER: " + val;
    } else if (iequals(key, "PRECONDITIONER")) {
        if (iequals(val, "NONE"))
            opts.preconditioner = PreconditionerType::NONE;
        else if (iequals(val, "JACOBI"))
            opts.preconditioner = PreconditionerType::JACOBI;
        else if (iequals(val, "ILU"))
            opts.preconditioner = PreconditionerType::ILU;
        else
            return "Unknown PRECONDITIONER: " + val;
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
    } else {
        return "Unknown 2D_OPTIONS parameter: " + key;
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

    // Try to parse as integer index first
    bool ok = false;
    int vidx = tryParseInt(tokens[0], ok);
    if (!ok) {
        // Try as tag name
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
        if (ok) mesh.vert_coupling_area[vidx] = area;
    }

    return {};
}


std::string parse2DTriangleNodeMapLine(const std::vector<std::string>& tokens,
                                        MeshData& mesh) {
    if (tokens.size() < 2) return "Expected TRIANGLE_INDEX_OR_TAG SWMM_NODE_NAME [CD] [AREA]";

    bool ok = false;
    int tidx = tryParseInt(tokens[0], ok);
    if (!ok) {
        tidx = findTriangleByTag(mesh, tokens[0]);
        if (tidx < 0) return "Unknown triangle index or tag: " + tokens[0];
    }

    if (tidx < 0 || tidx >= mesh.n_triangles())
        return "Triangle index out of range: " + tokens[0];

    mesh.tri_coupled_node_name[tidx] = tokens[1];

    if (tokens.size() >= 3) {
        double cd = tryParseDouble(tokens[2], ok);
        if (ok) mesh.tri_coupling_cd[tidx] = cd;
    }

    if (tokens.size() >= 4) {
        double area = tryParseDouble(tokens[3], ok);
        if (ok) mesh.tri_coupling_area[tidx] = area;
    }

    return {};
}


// ============================================================================
// parse2DROMLine
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
        opts.rom_k_eff = v;   // <= 0 means AUTO mode (PR 4)
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
    } else {
        return "Unknown 2D_ROM parameter: " + key;
    }
    return {};
}

// ============================================================================
// parseUncertaintyLine
// ============================================================================

std::string parseUncertaintyLine(
    const std::vector<std::string>& tokens,
    SolverOptions2D& opts,
    openswmm::uncertainty::UncertaintyConfig& config)
{
    // Format: LAYER PARAMETER [DISTRIBUTION] PERTURBATION
    // Minimal: "2D MANNINGS_N 0.20"  (DISTRIBUTION defaults to UNIFORM)
    //          "2D RAINFALL UNIFORM 0.15"
    if (tokens.size() < 3)
        return "Expected LAYER PARAMETER [DISTRIBUTION] PERTURBATION";

    const auto& layer_str = tokens[0];
    const auto& param_str = tokens[1];

    // Determine whether optional DISTRIBUTION token is present
    // (3 tokens → no dist; 4+ tokens → dist is tokens[2])
    std::string dist_str  = "UNIFORM";
    std::string pert_str;
    if (tokens.size() >= 4) {
        dist_str = tokens[2];
        pert_str = tokens[3];
    } else {
        pert_str = tokens[2];
    }

    // --- Parse LAYER ---
    openswmm::uncertainty::LayerTarget layer;
    if (iequals(layer_str, "2D"))
        layer = openswmm::uncertainty::LayerTarget::TWO_D;
    else if (iequals(layer_str, "1D"))
        layer = openswmm::uncertainty::LayerTarget::ONE_D;
    else
        return "Unsupported LAYER '" + layer_str + "' (supported: '2D', '1D')";

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

    // --- Parse PERTURBATION ---
    bool ok = false;
    double pert = tryParseDouble(pert_str, ok);
    if (!ok || pert < 0.0)
        return "PERTURBATION must be a non-negative number";

    // --- Validate PARAMETER for Phase 0/1 scope ---
    std::string param_upper = param_str;
    std::transform(param_upper.begin(), param_upper.end(), param_upper.begin(),
                   [](unsigned char c){ return static_cast<char>(std::toupper(c)); });

    if (param_upper != "MANNINGS_N" && param_upper != "RAINFALL")
        return "Unsupported PARAMETER '" + param_str
               + "' (supported: MANNINGS_N, RAINFALL)";

    // --- Build spec and record ---
    openswmm::uncertainty::UncertaintySourceSpec spec;
    spec.name        = param_upper;
    spec.layer       = layer;
    spec.dist        = dist;
    spec.perturbation = pert;
    config.sources.push_back(spec);

    // For 2D specs, [UNCERTAINTY] takes precedence over [2D_ROM] values.
    // For 1D specs, only update the uncertainty config (not the 2D solver opts).
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
                ctx.error_code    = 1;
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
// register2DSections
// ============================================================================

void register2DSections(MeshData& mesh,
                        SolverOptions2D& options,
                        openswmm::uncertainty::UncertaintyConfig& config,
                        std::vector<SurfaceRouter2D::PendingBoundaryRow>& pending_bc_rows,
                        input::SectionRegistry& registry)
{
    registry.register_custom("2D_OPTIONS",
        makeSectionHandler([&options](const std::vector<std::string>& tokens) {
            return parse2DOptionsLine(tokens, options);
        }));

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

    // [2D_ROM] — scalar ROM configuration (enable, size, perturbations, K_eff).
    registry.register_custom("2D_ROM",
        makeSectionHandler([&options](const std::vector<std::string>& tokens) {
            return parse2DROMLine(tokens, options);
        }));

    // [UNCERTAINTY] — scalar parameter uncertainty for 2D layer (Phase 0/1).
    // Entries here OVERRIDE the corresponding scalar fields in [2D_ROM].
    registry.register_custom("UNCERTAINTY",
        makeSectionHandler([&options, &config](const std::vector<std::string>& tokens) {
            return parseUncertaintyLine(tokens, options, config);
        }));
}


// ============================================================================
// load2DMeshExternalFile
// ============================================================================

std::string load2DMeshExternalFile(MeshData& mesh,
                                   SolverOptions2D& opts,
                                   std::vector<SurfaceRouter2D::PendingBoundaryRow>& pending_bc_rows,
                                   const std::string& mesh_file,
                                   const std::string& inp_base_dir)
{
    namespace fs = std::filesystem;

    // Resolve path
    fs::path p(mesh_file);
    if (p.is_relative() && !inp_base_dir.empty())
        p = fs::path(inp_base_dir) / p;

    // Build a minimal registry (no 2D_MESH_FILE — prevents recursion)
    openswmm::input::SectionRegistry mini;
    mini.register_custom("2D_OPTIONS",
        makeSectionHandler([&opts](const std::vector<std::string>& tokens) {
            return parse2DOptionsLine(tokens, opts);
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

    openswmm::input::InputReader reader(mini);
    openswmm::SimulationContext  dummy;
    if (!reader.read(p.string(), dummy)) {
        return "2D_MESH_FILE: error reading '" + p.string() + "': " + dummy.error_message;
    }
    return {};
}

} // namespace openswmm::twoD
