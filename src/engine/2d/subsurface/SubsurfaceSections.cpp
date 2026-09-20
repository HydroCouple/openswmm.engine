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
 * @file SubsurfaceSections.cpp
 * @brief Parsers, resolution and writer for the `[2D_AQUIFER*]` sections.
 *
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "SubsurfaceSections.hpp"

#include "../data/MeshData.hpp"
#include "../../core/SimulationContext.hpp"
#include "../../core/UnitConversion.hpp"
#include "../../input/InputParseUtils.hpp"
#include "../../input/Tokenizer.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>

namespace openswmm::twoD {

namespace {

bool iequals(const std::string& a, const char* b) {
    std::size_t i = 0;
    for (; i < a.size() && b[i]; ++i)
        if (std::toupper(static_cast<unsigned char>(a[i])) !=
            std::toupper(static_cast<unsigned char>(b[i]))) return false;
    return i == a.size() && b[i] == '\0';
}

bool num(const std::string& s, double& v) {
    auto [p, ec] = openswmm::from_chars_double(s.data(), s.data() + s.size(), v);
    return ec == std::errc{} && p == s.data() + s.size() && std::isfinite(v);
}

bool inum(const std::string& s, int& v) {
    double d = 0.0;
    if (!num(s, d)) return false;
    v = static_cast<int>(d);
    return static_cast<double>(v) == d;
}

bool boolTok(const std::string& s, bool& v) {
    if (iequals(s, "YES") || iequals(s, "TRUE")  || s == "1") { v = true;  return true; }
    if (iequals(s, "NO")  || iequals(s, "FALSE") || s == "0") { v = false; return true; }
    return false;
}

/// Format a double the way the rest of the 2D writers do: shortest round-trip
/// that a human can still read, with trailing zeros trimmed.
std::string fmt(double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.6g", v);
    return buf;
}

input::SectionHandler makeHandler(
    const char* label,
    std::function<std::string(const std::vector<std::string>&)> lp,
    GwOptions& opts) {
    return [label, lp = std::move(lp), &opts](
               openswmm::SimulationContext& ctx,
               const std::vector<std::string>& lines) {
        opts.authored = true;
        for (const auto& raw : lines) {
            auto tokens = openswmm::input::Tokenizer::tokenize(raw);
            if (tokens.empty()) continue;
            const std::string err = lp(tokens);
            if (!err.empty()) {
                ctx.error_code    = 5;  // SWMM_ERR_PARSE
                ctx.error_message =
                    std::string("[") + label + "] " + err + " - line: " + raw;
                return;
            }
        }
    };
}

}  // namespace

// ---------------------------------------------------------------------------
// [2D_AQUIFER_OPTIONS]
// ---------------------------------------------------------------------------

std::string parseAquiferOptionsLine(const std::vector<std::string>& tokens,
                                    GwOptions& opts) {
    if (tokens.size() < 2) return "expected KEY VALUE: " + tokens[0];
    const std::string& k = tokens[0];
    const std::string& v = tokens[1];

    if (iequals(k, "SOIL_CHAR")) {
        if (!parseSoilChar(v, opts.soil_char)) return "unknown SOIL_CHAR: " + v;
        return {};
    }
    if (iequals(k, "CLOSURE")) {
        if (!parseGwClosure(v, opts.closure)) return "unknown CLOSURE: " + v;
        return {};
    }
    if (iequals(k, "M_LAYERS")) {
        int m = 0;
        if (!inum(v, m) || m < 2 || m > 128) return "M_LAYERS must be 2..128: " + v;
        opts.m_layers = m;
        return {};
    }
    if (iequals(k, "CAPILLARY_DIFF")) {
        if (!boolTok(v, opts.capillary_diff)) return "CAPILLARY_DIFF must be YES/NO: " + v;
        return {};
    }
    if (iequals(k, "FORCE_CLOSED_FORM")) {
        if (!boolTok(v, opts.force_closed_form)) return "FORCE_CLOSED_FORM must be YES/NO: " + v;
        return {};
    }
    if (iequals(k, "DUNNE")) {
        if (!boolTok(v, opts.dunne)) return "DUNNE must be YES/NO: " + v;
        return {};
    }
    if (iequals(k, "C_GW") || iequals(k, "C_COL")) {
        double d = 0.0;
        if (!num(v, d) || d <= 0.0 || d > 1.0)
            return std::string(iequals(k, "C_GW") ? "C_GW" : "C_COL") +
                   " must be in (0, 1]: " + v;
        (iequals(k, "C_GW") ? opts.c_gw : opts.c_col) = d;
        return {};
    }
    if (iequals(k, "MODE")) {
        if (iequals(v, "MESH"))              { opts.per_subcatch = false; return {}; }
        if (iequals(v, "PER_SUBCATCH"))      { opts.per_subcatch = true;  return {}; }
        return "MODE must be MESH or PER_SUBCATCH: " + v;
    }
    if (iequals(k, "NODE_ENROLMENT")) {                 // G-X2
        if (iequals(v, "AUTO")) { opts.node_auto = true;  return {}; }
        if (iequals(v, "ROWS")) { opts.node_auto = false; return {}; }
        return "NODE_ENROLMENT must be AUTO or ROWS: " + v;
    }
    if (iequals(k, "GW_ET")) {
        if (iequals(v, "NONE") || iequals(v, "CAPILLARY_RISE") ||
            iequals(v, "BOUNDARY_ET") || iequals(v, "BOTH")) {
            opts.gw_et.assign(v.size(), '\0');
            std::transform(v.begin(), v.end(), opts.gw_et.begin(),
                           [](unsigned char c) {
                               return static_cast<char>(std::toupper(c));
                           });
            return {};
        }
        return "GW_ET must be NONE, CAPILLARY_RISE, BOUNDARY_ET or BOTH: " + v;
    }
    return "unknown option: " + k;
}

// ---------------------------------------------------------------------------
// [2D_AQUIFER]
// ---------------------------------------------------------------------------

std::string parseAquiferLine(const std::vector<std::string>& tokens,
                             std::vector<GwAquiferRow>& rows) {
    GwAquiferRow r;
    std::size_t at = 0;

    if (tokens[0] == "*") { r.scope = 0; at = 1; }
    else if (iequals(tokens[0], "TAG")) {
        if (tokens.size() < 2) return "TAG needs a name";
        r.scope = 1; r.tag = tokens[1]; at = 2;
    } else if (iequals(tokens[0], "CELL")) {
        int n = 0;
        if (tokens.size() < 2 || !inum(tokens[1], n) || n < 1)
            return "CELL needs a 1-based index";
        r.scope = 2; r.cell = n - 1; at = 2;
    } else {
        return "row must start with CELL, TAG or '*': " + tokens[0];
    }

    // Five positional columns: KS ZS THETA_S THETA_R ALPHA.
    static const char* kPos[5] = {"KS", "ZS", "THETA_S", "THETA_R", "ALPHA"};
    double* const dst[5] = {&r.Ks, &r.zs, &r.theta_s, &r.theta_r, &r.alpha};
    for (int i = 0; i < 5; ++i) {
        if (at >= tokens.size()) return std::string("missing ") + kPos[i];
        if (!num(tokens[at], *dst[i]))
            return std::string("invalid ") + kPos[i] + ": " + tokens[at];
        ++at;
    }

    // Everything law-specific is a keyword, so a fifth law never renumbers an
    // existing file.
    while (at < tokens.size()) {
        const std::string& k = tokens[at];
        if (at + 1 >= tokens.size()) return "keyword " + k + " needs a value";
        const std::string& v = tokens[at + 1];
        at += 2;
        if      (iequals(k, "PSI_B"))  { if (!num(v, r.psi_b))  return "invalid PSI_B: " + v; }
        else if (iequals(k, "LAMBDA")) { if (!num(v, r.lambda)) return "invalid LAMBDA: " + v; }
        else if (iequals(k, "N"))      { if (!num(v, r.vg_n))   return "invalid N: " + v; }
        else if (iequals(k, "L"))      { if (!num(v, r.vg_L))   return "invalid L: " + v; }
        else if (iequals(k, "C_LOSS")) { if (!num(v, r.c_loss)) return "invalid C_LOSS: " + v; }
        else if (iequals(k, "HG0"))    { if (!num(v, r.hg0))    return "invalid HG0: " + v; }
        else if (iequals(k, "SOIL_CHAR")) {
            if (!parseSoilChar(v, r.soil_char)) return "unknown SOIL_CHAR: " + v;
            r.soil_char_set = true;
        } else if (iequals(k, "CLOSURE")) {
            if (!parseGwClosure(v, r.closure)) return "unknown CLOSURE: " + v;
            r.closure_set = true;
        } else if (iequals(k, "M_LAYERS")) {
            if (!inum(v, r.m_layers) || r.m_layers < 2 || r.m_layers > 128)
                return "M_LAYERS must be 2..128: " + v;
        } else {
            return "unknown keyword: " + k;
        }
    }

    if (r.Ks <= 0.0)      return "KS must be > 0";
    if (r.zs <= 0.0)      return "ZS must be > 0";
    if (r.theta_s <= 0.0 || r.theta_s > 1.0) return "THETA_S must be in (0, 1]";
    if (r.theta_r < 0.0 || r.theta_r >= r.theta_s)
        return "THETA_R must be in [0, THETA_S)";
    if (r.alpha <= 0.0)   return "ALPHA must be > 0";
    if (r.vg_n <= 1.0)    return "N must be > 1";
    if (r.lambda <= 0.0)  return "LAMBDA must be > 0";
    if (r.psi_b < 0.0)    return "PSI_B must be >= 0";
    if (r.c_loss < 0.0)   return "C_LOSS must be >= 0";

    rows.push_back(r);
    return {};
}

// ---------------------------------------------------------------------------
// [2D_AQUIFER_NODE]
// ---------------------------------------------------------------------------

std::string parseAquiferNodeLine(const std::vector<std::string>& tokens,
                                 std::vector<GwNodeBed>& beds,
                                 std::vector<std::string>& names) {
    if (tokens.size() < 2)
        return "expected NODE CELL|AUTO [KC k DC d] [AREA a] [EXCHANGE YES|NO]";
    GwNodeBed b;
    int cell = 0;
    if (iequals(tokens[1], "AUTO")) {
        b.locate = true;                      // G-X2: located at resolve
    } else {
        if (!inum(tokens[1], cell) || cell < 1)
            return "CELL must be a 1-based index or AUTO: " + tokens[1];
        b.cell = cell - 1;
    }

    std::size_t at = 2;
    while (at < tokens.size()) {
        const std::string& k = tokens[at];
        if (at + 1 >= tokens.size()) return "keyword " + k + " needs a value";
        const std::string& v = tokens[at + 1];
        at += 2;
        if      (iequals(k, "KC"))   { if (!num(v, b.Kc))   return "invalid KC: " + v; }
        else if (iequals(k, "DC"))   { if (!num(v, b.dC))   return "invalid DC: " + v; }
        else if (iequals(k, "AREA")) { if (!num(v, b.area)) return "invalid AREA: " + v; }
        else if (iequals(k, "EXCHANGE")) {              // G-X2 opt-out
            if (!boolTok(v, b.exchange)) return "EXCHANGE must be YES/NO: " + v;
        }
        else return "unknown keyword: " + k;
    }
    if (b.Kc < 0.0 || b.dC < 0.0 || b.area < 0.0)
        return "KC, DC and AREA must be >= 0";
    if (b.Kc > 0.0 && b.dC <= 0.0)
        return "KC given without a positive DC (a bed needs a thickness)";

    beds.push_back(b);
    names.push_back(tokens[0]);
    return {};
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void registerSubsurfaceSections(SubsurfaceConfig& cfg,
                                std::vector<std::string>& node_names,
                                input::SectionRegistry& registry) {
    registry.register_custom("2D_AQUIFER_OPTIONS",
        makeHandler("2D_AQUIFER_OPTIONS",
            [&cfg](const std::vector<std::string>& t) {
                return parseAquiferOptionsLine(t, cfg.options);
            }, cfg.options));
    registry.register_custom("2D_AQUIFER",
        makeHandler("2D_AQUIFER",
            [&cfg](const std::vector<std::string>& t) {
                return parseAquiferLine(t, cfg.rows);
            }, cfg.options));
    registry.register_custom("2D_AQUIFER_NODE",
        makeHandler("2D_AQUIFER_NODE",
            [&cfg, &node_names](const std::vector<std::string>& t) {
                return parseAquiferNodeLine(t, cfg.node_beds, node_names);
            }, cfg.options));
}

// ---------------------------------------------------------------------------
// Units and resolution
// ---------------------------------------------------------------------------

GwUnitFactors gwUnitFactors(const SimulationContext& ctx) noexcept {
    GwUnitFactors f;
    const int sys = ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));
    if (sys == 0) {                       // US
        f.length  = 0.3048;               // ft  → m
        f.rate    = 0.0254 / 3600.0;      // in/hr → m/s
        f.inv_len = 1.0 / 0.3048;         // 1/ft → 1/m
        f.area    = 0.3048 * 0.3048;      // ft² → m²
    } else {                              // SI
        f.length  = 1.0;                  // m
        f.rate    = 0.001 / 3600.0;       // mm/hr → m/s
        f.inv_len = 1.0;
        f.area    = 1.0;
    }
    return f;
}

namespace {

/// G-X2: which cell (−1 = none) contains the point (x, y) in the mesh's own
/// coordinates. A uniform bin grid over the cell bounding boxes keeps a
/// whole network's nodes O(n) rather than O(n_nodes × n_cells).
class CellLocator {
public:
    explicit CellLocator(const MeshData& m) : m_(m) {
        const int n = m.n_cells();
        if (n == 0 || m.vx.empty()) return;
        xmin_ = ymin_ =  1.0e300; xmax_ = ymax_ = -1.0e300;
        for (std::size_t v = 0; v < m.vx.size(); ++v) {
            xmin_ = std::min(xmin_, m.vx[v]); xmax_ = std::max(xmax_, m.vx[v]);
            ymin_ = std::min(ymin_, m.vy[v]); ymax_ = std::max(ymax_, m.vy[v]);
        }
        nb_ = std::max(1, static_cast<int>(std::sqrt(static_cast<double>(n))));
        dx_ = std::max((xmax_ - xmin_) / nb_, 1.0e-9);
        dy_ = std::max((ymax_ - ymin_) / nb_, 1.0e-9);
        bins_.assign(static_cast<std::size_t>(nb_) * nb_, {});
        for (int c = 0; c < n; ++c) {
            double bx0 = 1.0e300, by0 = 1.0e300, bx1 = -1.0e300, by1 = -1.0e300;
            for (int k = 0; k < m.cell_nv[static_cast<std::size_t>(c)]; ++k) {
                const auto v = static_cast<std::size_t>(m.cell_vertex(c, k));
                bx0 = std::min(bx0, m.vx[v]); bx1 = std::max(bx1, m.vx[v]);
                by0 = std::min(by0, m.vy[v]); by1 = std::max(by1, m.vy[v]);
            }
            for (int j = bin(by0, ymin_, dy_); j <= bin(by1, ymin_, dy_); ++j)
                for (int i = bin(bx0, xmin_, dx_); i <= bin(bx1, xmin_, dx_); ++i)
                    bins_[static_cast<std::size_t>(j) * nb_ + i].push_back(c);
        }
    }
    int locate(double x, double y) const {
        if (bins_.empty() || x < xmin_ || x > xmax_ || y < ymin_ || y > ymax_) return -1;
        const auto& cand = bins_[static_cast<std::size_t>(bin(y, ymin_, dy_)) * nb_ +
                                 bin(x, xmin_, dx_)];
        for (const int c : cand) if (inside(c, x, y)) return c;
        return -1;
    }
private:
    int bin(double v, double v0, double d) const {
        return std::clamp(static_cast<int>((v - v0) / d), 0, nb_ - 1);
    }
    bool inside(int c, double x, double y) const {   // ray casting, any polygon
        const int nv = m_.cell_nv[static_cast<std::size_t>(c)];
        bool in = false;
        for (int i = 0, j = nv - 1; i < nv; j = i++) {
            const auto vi = static_cast<std::size_t>(m_.cell_vertex(c, i));
            const auto vj = static_cast<std::size_t>(m_.cell_vertex(c, j));
            const double xi = m_.vx[vi], yi = m_.vy[vi], xj = m_.vx[vj], yj = m_.vy[vj];
            if (((yi > y) != (yj > y)) &&
                (x < (xj - xi) * (y - yi) / (yj - yi) + xi))
                in = !in;
        }
        return in;
    }
    const MeshData& m_;
    double xmin_ = 0, xmax_ = 0, ymin_ = 0, ymax_ = 0, dx_ = 1, dy_ = 1;
    int nb_ = 0;
    std::vector<std::vector<int>> bins_;
};

}  // namespace

std::vector<std::string> resolveSubsurface(
    SimulationContext& ctx, const MeshData& mesh, SubsurfaceConfig& cfg,
    std::vector<std::string>& names, double node_xy_to_mesh) {
    std::vector<std::string> errs;
    if (cfg.empty()) return errs;

    const int n = mesh.n_cells();
    for (const auto& r : cfg.rows) {
        if (r.scope == 2 && (r.cell < 0 || r.cell >= n))
            errs.push_back("[2D_AQUIFER] CELL " + std::to_string(r.cell + 1) +
                           " is not on the mesh (" + std::to_string(n) +
                           " cells).");
    }

    // G-X2: the locator serves `CELL AUTO` rows and auto-enrolment alike.
    const CellLocator locator(mesh);
    auto locateNode = [&](int ni) -> int {
        const auto u = static_cast<std::size_t>(ni);
        if (u >= ctx.spatial.node_x.size() || u >= ctx.spatial.node_y.size()) return -1;
        if (u >= ctx.spatial.node_has_xy.size() || !ctx.spatial.node_has_xy[u]) return -1;
        return locator.locate(ctx.spatial.node_x[u] * node_xy_to_mesh,
                              ctx.spatial.node_y[u] * node_xy_to_mesh);
    };

    for (std::size_t i = 0; i < cfg.node_beds.size(); ++i) {
        auto& b = cfg.node_beds[i];
        const std::string& nm = (i < names.size()) ? names[i] : std::string{};
        b.node = ctx.node_names.find(nm);
        if (b.node < 0) {
            errs.push_back("[2D_AQUIFER_NODE] unknown node '" + nm + "'.");
            continue;
        }
        if (b.locate) {
            b.cell = locateNode(b.node);
            if (b.cell < 0 && b.exchange)
                errs.push_back("[2D_AQUIFER_NODE] node '" + nm + "' CELL AUTO: the node's "
                               "[COORDINATES] fall in no mesh cell.");
        } else if (b.exchange && (b.cell < 0 || b.cell >= n)) {
            errs.push_back("[2D_AQUIFER_NODE] node '" + nm + "' CELL " +
                           std::to_string(b.cell + 1) +
                           " is not on the mesh.");
        }
    }

    // G-X2: auto-enrolment — every node whose coordinates fall in a cell
    // and that no row names gets a direct-Darcy bed over the cell's area.
    // Rows override (their bed, or their EXCHANGE NO). Nodes without
    // coordinates are simply not in any cell.
    if (cfg.options.node_auto && errs.empty()) {
        std::vector<char> named(static_cast<std::size_t>(ctx.n_nodes()), 0);
        for (const auto& b : cfg.node_beds)
            if (b.node >= 0 && b.node < ctx.n_nodes()) named[static_cast<std::size_t>(b.node)] = 1;
        int enrolled = 0;
        for (int ni = 0; ni < ctx.n_nodes(); ++ni) {
            if (named[static_cast<std::size_t>(ni)]) continue;
            const int c = locateNode(ni);
            if (c < 0) continue;
            GwNodeBed b;
            b.node = ni; b.cell = c; b.locate = true; b.automatic = true;
            cfg.node_beds.push_back(b);
            names.push_back(ctx.node_names.name_of(ni));
            ++enrolled;
        }
        if (enrolled > 0)
            ctx.warnings.push_back("2D aquifer: " + std::to_string(enrolled) +
                                   " node(s) inside the mesh enrolled in the node <-> aquifer "
                                   "exchange by their coordinates (NODE_ENROLMENT AUTO; a "
                                   "[2D_AQUIFER_NODE] row with EXCHANGE NO opts a node out).");
    }

    // One bed per node: two beds on one node would double the exchange and
    // the node would never know.
    std::vector<int> seen;
    for (const auto& b : cfg.node_beds) {
        if (b.node < 0) continue;
        if (std::find(seen.begin(), seen.end(), b.node) != seen.end())
            errs.push_back("[2D_AQUIFER_NODE] node index " +
                           std::to_string(b.node) +
                           " has more than one bed row.");
        else seen.push_back(b.node);
    }
    // G-X2: an EXCHANGE NO row has done its job (it kept auto-enrolment
    // away) — drop it from the beds the kernel sees. The authored row stays
    // in the config for the writer, so the flag is carried on the bed and
    // the solver skips it (SubsurfaceSolver::initialize).
    return errs;
}

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------

void writeSubsurfaceSections(const SubsurfaceConfig& cfg,
                             const std::vector<std::string>& node_names,
                             std::string& out) {
    if (cfg.empty()) return;
    const GwOptions d{};   // defaults, for the omit-if-unchanged rule

    if (cfg.options.authored) {
        std::string body;
        auto kv = [&body](const char* k, const std::string& v) {
            body += k;
            body.append(std::max<std::size_t>(1, 18 - std::strlen(k)), ' ');
            body += v;
            body += '\n';
        };
        if (cfg.options.soil_char != d.soil_char)
            kv("SOIL_CHAR", soilCharToken(cfg.options.soil_char));
        if (cfg.options.closure != d.closure)
            kv("CLOSURE", gwClosureToken(cfg.options.closure));
        if (cfg.options.m_layers != d.m_layers)
            kv("M_LAYERS", std::to_string(cfg.options.m_layers));
        if (cfg.options.capillary_diff != d.capillary_diff)
            kv("CAPILLARY_DIFF", cfg.options.capillary_diff ? "YES" : "NO");
        if (cfg.options.c_gw != d.c_gw)   kv("C_GW",  fmt(cfg.options.c_gw));
        if (cfg.options.c_col != d.c_col) kv("C_COL", fmt(cfg.options.c_col));
        if (cfg.options.force_closed_form != d.force_closed_form)
            kv("FORCE_CLOSED_FORM", cfg.options.force_closed_form ? "YES" : "NO");
        if (cfg.options.per_subcatch != d.per_subcatch)
            kv("MODE", cfg.options.per_subcatch ? "PER_SUBCATCH" : "MESH");
        if (cfg.options.dunne != d.dunne)
            kv("DUNNE", cfg.options.dunne ? "YES" : "NO");
        if (cfg.options.gw_et != d.gw_et) kv("GW_ET", cfg.options.gw_et);
        if (cfg.options.node_auto != d.node_auto)
            kv("NODE_ENROLMENT", cfg.options.node_auto ? "AUTO" : "ROWS");   // G-X2
        if (!body.empty()) {
            out += "\n[2D_AQUIFER_OPTIONS]\n";
            out += body;
        }
    }

    if (!cfg.rows.empty()) {
        out += "\n[2D_AQUIFER]\n";
        out += ";;Scope           KS         ZS         Theta_s    Theta_r    "
               "Alpha      Options\n";
        const GwAquiferRow rd{};
        for (const auto& r : cfg.rows) {
            std::string line;
            if      (r.scope == 0) line = "*";
            else if (r.scope == 1) line = "TAG  " + r.tag;
            else                   line = "CELL " + std::to_string(r.cell + 1);
            line.append(std::max<std::size_t>(1, 18 - line.size()), ' ');
            for (double v : {r.Ks, r.zs, r.theta_s, r.theta_r, r.alpha}) {
                const std::string s = fmt(v);
                line += s;
                line.append(std::max<std::size_t>(1, 11 - s.size()), ' ');
            }
            auto opt = [&line](const char* k, const std::string& v) {
                line += k; line += ' '; line += v; line += ' ';
            };
            if (r.psi_b  != rd.psi_b)  opt("PSI_B",  fmt(r.psi_b));
            if (r.lambda != rd.lambda) opt("LAMBDA", fmt(r.lambda));
            if (r.vg_n   != rd.vg_n)   opt("N",      fmt(r.vg_n));
            if (r.vg_L   != rd.vg_L)   opt("L",      fmt(r.vg_L));
            if (r.c_loss != rd.c_loss) opt("C_LOSS", fmt(r.c_loss));
            if (r.hg0    != rd.hg0)    opt("HG0",    fmt(r.hg0));
            if (r.soil_char_set) opt("SOIL_CHAR", soilCharToken(r.soil_char));
            if (r.closure_set)   opt("CLOSURE",   gwClosureToken(r.closure));
            if (r.m_layers > 0)  opt("M_LAYERS",  std::to_string(r.m_layers));
            while (!line.empty() && line.back() == ' ') line.pop_back();
            out += line;
            out += '\n';
        }
    }

    // G-X2: auto-enrolled beds are not echoed — NODE_ENROLMENT AUTO recreates
    // them from the coordinates, and echoing would turn a located bed into an
    // authored CELL row that no longer follows the node if it moves.
    bool any_authored = false;
    for (const auto& b : cfg.node_beds) if (!b.automatic) { any_authored = true; break; }
    if (any_authored) {
        out += "\n[2D_AQUIFER_NODE]\n";
        out += ";;Node             Cell       Options\n";
        for (std::size_t i = 0; i < cfg.node_beds.size(); ++i) {
            const auto& b = cfg.node_beds[i];
            if (b.automatic) continue;
            std::string line = (i < node_names.size()) ? node_names[i]
                                                       : std::string("?");
            line.append(std::max<std::size_t>(1, 19 - line.size()), ' ');
            const std::string c = b.locate ? std::string("AUTO")
                                           : std::to_string(b.cell + 1);
            line += c;
            line.append(std::max<std::size_t>(1, 11 - c.size()), ' ');
            if (b.Kc   > 0.0) { line += "KC ";   line += fmt(b.Kc);   line += ' '; }
            if (b.dC   > 0.0) { line += "DC ";   line += fmt(b.dC);   line += ' '; }
            if (b.area > 0.0) { line += "AREA "; line += fmt(b.area); line += ' '; }
            if (!b.exchange)  { line += "EXCHANGE NO "; }
            while (!line.empty() && line.back() == ' ') line.pop_back();
            out += line;
            out += '\n';
        }
    }
}

}  // namespace openswmm::twoD
