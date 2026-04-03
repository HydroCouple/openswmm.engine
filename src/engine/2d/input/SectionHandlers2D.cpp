/**
 * @file SectionHandlers2D.cpp
 * @brief Implementation of 2D input section parsers.
 *
 * @see SectionHandlers2D.hpp
 * @ingroup engine_2d
 */

#include "SectionHandlers2D.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

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

} // namespace openswmm::twoD
