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
#include "FootprintGeometry.hpp"
#include <iomanip>
#include <sstream>
#include <set>

#include "../data/MeshData.hpp"
#include "../data/SolverOptions2D.hpp"
#include <memory>
#include <array>
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
    if (iequals(k, "LINK_SEEPAGE")) {                   // G-X3 / G-X4
        if (iequals(v, "DEFAULT")) { opts.link_seepage = GwLinkMode::DEFAULT; return {}; }
        if (iequals(v, "AUTO") || iequals(v, "ONE_WAY")) { opts.link_seepage = GwLinkMode::AUTO; return {}; }
        if (iequals(v, "NONE"))    { opts.link_seepage = GwLinkMode::NONE;    return {}; }
        if (iequals(v, "TWO_WAY")) { opts.link_seepage = GwLinkMode::TWO_WAY; return {}; }
        return "LINK_SEEPAGE must be DEFAULT, ONE_WAY (legacy AUTO), NONE or TWO_WAY: " + v;
    }
    if (iequals(k, "GW_ET")) {
        if (iequals(v, "AUTO") || iequals(v, "NONE") || iequals(v, "CAPILLARY_RISE") ||
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
    if (iequals(k,"WILTING_SUCTION")) {
        if(iequals(v,"AUTO")){opts.wilting_suction=150.0;opts.wilting_suction_set=false;return {};}
        double suction=0.0;
        if(!num(v,suction)||!std::isfinite(suction)||!(suction>0.0))return "WILTING_SUCTION must be AUTO or a positive finite project length";
        opts.wilting_suction=suction;opts.wilting_suction_set=true;return {};
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

// G-X4: one `[2D_AQUIFER_LINKS]` line — `LINK [KC k] [DC d] [EXCHANGE YES|NO]`.
std::string parseAquiferLinkLine(const std::vector<std::string>& tokens,
                                 std::vector<GwLinkRow>& rows,
                                 std::vector<std::string>& names) {
    if (tokens.empty())
        return "expected LINK [KC k] [DC d] [EXCHANGE YES|NO]";
    GwLinkRow r;
    std::size_t at = 1;
    while (at < tokens.size()) {
        const std::string& k = tokens[at];
        if (at + 1 >= tokens.size()) return "keyword " + k + " needs a value";
        const std::string& v = tokens[at + 1];
        at += 2;
        if      (iequals(k, "KC")) { if (!num(v, r.Kc)) return "invalid KC: " + v; }
        else if (iequals(k, "DC")) { if (!num(v, r.dC)) return "invalid DC: " + v; }
        else if (iequals(k, "EXCHANGE")) {
            if (!boolTok(v, r.exchange)) return "EXCHANGE must be YES/NO: " + v;
        }
        else return "unknown keyword: " + k;
    }
    if (r.Kc < 0.0 || r.dC < 0.0) return "KC and DC must be >= 0";
    rows.push_back(r);
    names.push_back(tokens[0]);
    return {};
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void registerSubsurfaceSections(SubsurfaceConfig& cfg,
                                std::vector<std::string>& node_names,
                                std::vector<std::string>& link_names,
                                input::SectionRegistry& registry) {
    registry.register_custom("2D_SURFACE_OWNERSHIP",[&cfg](SimulationContext& ctx,const std::vector<std::string>& lines){
        for(const auto& raw:lines){const auto t=input::Tokenizer::tokenize(raw);if(t.empty())continue;
            const auto error=parseSurfaceOwnerLine(t,cfg.surface_owners);
            if(!error.empty()){ctx.error_code=5;ctx.error_message="[2D_SURFACE_OWNERSHIP] "+error;return;}
        }
    });
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
    registry.register_custom("2D_AQUIFER_LINKS",              // G-X4
        makeHandler("2D_AQUIFER_LINKS",
            [&cfg, &link_names](const std::vector<std::string>& t) {
                return parseAquiferLinkLine(t, cfg.link_rows, link_names);
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
    /// G-X3: every cell whose bounding box may meet the box [x0,x1]×[y0,y1]
    /// (the union of the bins the box covers, de-duplicated), for a segment
    /// clip. Empty when the box misses the mesh entirely.
    std::vector<int> candidates(double x0, double y0, double x1, double y1) const {
        std::vector<int> out;
        if (bins_.empty()) return out;
        if (std::max(x0, x1) < xmin_ || std::min(x0, x1) > xmax_ ||
            std::max(y0, y1) < ymin_ || std::min(y0, y1) > ymax_) return out;
        const int i0 = bin(std::min(x0, x1), xmin_, dx_), i1 = bin(std::max(x0, x1), xmin_, dx_);
        const int j0 = bin(std::min(y0, y1), ymin_, dy_), j1 = bin(std::max(y0, y1), ymin_, dy_);
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i)
                for (const int c : bins_[static_cast<std::size_t>(j) * nb_ + i])
                    out.push_back(c);
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }
    /// G-X3: the length of the segment (x0,y0)→(x1,y1) inside cell `c`.
    /// Cells are CCW and convex (MeshBuilder validates both), so this is
    /// the Cyrus–Beck clip: the parameter interval [t_lo, t_hi] that lies on
    /// the inner side of every edge's half-plane. Exact for a straight
    /// segment — no sampling, no step error.
    double lengthInside(int c, double x0, double y0, double x1, double y1) const {
        const int nv = m_.cell_nv[static_cast<std::size_t>(c)];
        const double dx = x1 - x0, dy = y1 - y0;
        double t_lo = 0.0, t_hi = 1.0;
        for (int i = 0, j = nv - 1; i < nv; j = i++) {
            const auto vi = static_cast<std::size_t>(m_.cell_vertex(c, j));
            const auto vj = static_cast<std::size_t>(m_.cell_vertex(c, i));
            const double ex = m_.vx[vj] - m_.vx[vi], ey = m_.vy[vj] - m_.vy[vi];
            // signed distance ∝ cross(edge, P − v_i): ≥ 0 inside for CCW
            const double d0 = ex * (y0 - m_.vy[vi]) - ey * (x0 - m_.vx[vi]);
            const double d1 = ex * (y1 - m_.vy[vi]) - ey * (x1 - m_.vx[vi]);
            const double dd = d1 - d0;                // d(t) = d0 + dd·t
            if (std::fabs(dd) < 1.0e-300) {
                if (d0 < 0.0) return 0.0;             // parallel, outside
                continue;                             // parallel, inside/on the edge
            }
            const double t = -d0 / dd;
            if (dd < 0.0) t_hi = std::min(t_hi, t);   // leaving the half-plane
            else          t_lo = std::max(t_lo, t);   // entering it
            if (t_lo >= t_hi) return 0.0;
        }
        return (t_hi - t_lo) * std::hypot(dx, dy);
    }
    /// The area of the polygon (px, py) inside cell `c`. Sutherland–Hodgman
    /// against the cell's edge half-planes — exact for the convex CCW clip
    /// (the subject may be concave; any degenerate slivers it leaves have
    /// zero area).
    double areaInside(int c, const std::vector<double>& px,
                      const std::vector<double>& py) const {
        std::vector<double> sx(px), sy(py), ox, oy;
        const int nv = m_.cell_nv[static_cast<std::size_t>(c)];
        for (int i = 0, j = nv - 1; i < nv && !sx.empty(); j = i++) {
            const auto vi = static_cast<std::size_t>(m_.cell_vertex(c, j));
            const auto vj = static_cast<std::size_t>(m_.cell_vertex(c, i));
            const double ax = m_.vx[vi], ay = m_.vy[vi];
            const double ex = m_.vx[vj] - ax, ey = m_.vy[vj] - ay;
            auto side = [&](double x, double y) { return ex * (y - ay) - ey * (x - ax); };
            ox.clear(); oy.clear();
            const std::size_t n = sx.size();
            for (std::size_t k = 0; k < n; ++k) {
                const std::size_t k1 = (k + 1) % n;
                const double s0 = side(sx[k], sy[k]), s1 = side(sx[k1], sy[k1]);
                if (s0 >= 0.0) { ox.push_back(sx[k]); oy.push_back(sy[k]); }
                if ((s0 >= 0.0) != (s1 >= 0.0)) {
                    const double t = s0 / (s0 - s1);
                    ox.push_back(sx[k] + t * (sx[k1] - sx[k]));
                    oy.push_back(sy[k] + t * (sy[k1] - sy[k]));
                }
            }
            sx.swap(ox); sy.swap(oy);
        }
        return polygonArea(sx, sy);
    }
    static double polygonArea(const std::vector<double>& x,
                              const std::vector<double>& y) {
        if (x.size() < 3) return 0.0;
        double a = 0.0;
        for (std::size_t i = 0, j = x.size() - 1; i < x.size(); j = i++)
            a += x[j] * y[i] - x[i] * y[j];
        return 0.5 * std::fabs(a);
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

// G-X3 (2026-09-19): which cells each seeping conduit crosses, and by how
// much of its length. The polyline (node1 → [VERTICES] → node2, project map
// units) is clipped segment by segment against the convex cells the locator
// offers for its bounding box; the per-cell length over the whole polyline
// length is the share — exact, so an evenly split conduit gives 0.5/0.5 to
// the last bit. A segment lying ON a shared edge is inside both cells; the
// shares are then scaled so a conduit never delivers more than its length
// (each side gets half). Only conduits with a [LOSSES] seepage rate at
// initialize take part (the others have nothing to deliver).
std::vector<GwLinkShare> resolveLinkSeepage(SimulationContext& ctx,
                                            const MeshData& mesh,
                                            SubsurfaceConfig& cfg,
                                            std::vector<std::string>& link_names,
                                            double node_xy_to_mesh,
                                            int& n_conduits) {
    std::vector<GwLinkShare> out;
    n_conduits = 0;
    const int n = mesh.n_cells();
    if (n == 0 || mesh.vx.empty()) return out;
    const CellLocator locator(mesh);

    // G-X4: resolve the `[2D_AQUIFER_LINKS]` names once. A row on a link the
    // model does not have, or on something that is not a conduit, is an
    // authoring error — it would otherwise be silently ignored and the
    // modeller would never learn why the reach did not gain.
    for (std::size_t i = 0; i < cfg.link_rows.size(); ++i) {
        auto& r = cfg.link_rows[i];
        const std::string& nm = (i < link_names.size()) ? link_names[i]
                                                        : std::string{};
        r.link = ctx.link_names.find(nm);
        if (r.link < 0) {
            ctx.warnings.push_back("[2D_AQUIFER_LINKS] unknown link '" + nm +
                                   "' — the row was ignored.");
        } else if (ctx.links.type[static_cast<std::size_t>(r.link)] !=
                   LinkType::CONDUIT) {
            ctx.warnings.push_back("[2D_AQUIFER_LINKS] link '" + nm +
                                   "' is not a conduit — only conduits exchange "
                                   "with the aquifer along their length; the row "
                                   "was ignored.");
            r.link = -1;
        }
    }
    auto rowFor = [&cfg](int link) -> const GwLinkRow* {
        for (const auto& r : cfg.link_rows) if (r.link == link) return &r;
        return nullptr;
    };
    const bool two_way = cfg.options.link_seepage == GwLinkMode::TWO_WAY ||
                         (cfg.options.link_seepage == GwLinkMode::DEFAULT && !cfg.options.per_subcatch);

    auto xy = [&](int ni, double& x, double& y) -> bool {
        const auto u = static_cast<std::size_t>(ni);
        if (ni < 0 || u >= ctx.spatial.node_x.size() || u >= ctx.spatial.node_y.size()) return false;
        if (u >= ctx.spatial.node_has_xy.size() || !ctx.spatial.node_has_xy[u]) return false;
        x = ctx.spatial.node_x[u] * node_xy_to_mesh;
        y = ctx.spatial.node_y[u] * node_xy_to_mesh;
        return true;
    };

    const auto& CD = ctx.link_subtypes.conduits;
    for (int j = 0; j < ctx.n_links(); ++j) {
        const auto uj = static_cast<std::size_t>(j);
        if (ctx.links.type[uj] != LinkType::CONDUIT) continue;
        const int cr = ctx.link_subtypes.conduit_row(j);
        if (cr < 0) continue;
        // G-X4: TWO_WAY, a row may give a conduit that never leaked its own
        // conductivity (`KC`) — a pipe under the table gains without ever
        // having had a [LOSSES] seepage rate — and `EXCHANGE NO` keeps a
        // conduit out of the channel entirely.
        const GwLinkRow* row = rowFor(j);
        if (row != nullptr && !row->exchange) continue;
        const double k_authored =
            (two_way && row != nullptr && row->Kc > 0.0)
                ? row->Kc : CD.seep_rate[static_cast<std::size_t>(cr)];
        if (k_authored <= 0.0) continue;

        std::vector<double> px, py;
        double x = 0.0, y = 0.0;
        if (!xy(ctx.links.node1[uj], x, y)) continue;
        px.push_back(x); py.push_back(y);
        if (uj < ctx.spatial.link_vertices_x.size() &&
            uj < ctx.spatial.link_vertices_y.size()) {
            const auto& vx = ctx.spatial.link_vertices_x[uj];
            const auto& vy = ctx.spatial.link_vertices_y[uj];
            for (std::size_t k = 0; k < vx.size() && k < vy.size(); ++k) {
                px.push_back(vx[k] * node_xy_to_mesh);
                py.push_back(vy[k] * node_xy_to_mesh);
            }
        }
        if (!xy(ctx.links.node2[uj], x, y)) continue;
        px.push_back(x); py.push_back(y);

        double L = 0.0;
        for (std::size_t k = 1; k < px.size(); ++k)
            L += std::hypot(px[k] - px[k - 1], py[k] - py[k - 1]);
        if (L <= 0.0) continue;

        std::vector<std::pair<int, double>> len_in;   // cell, length
        double sum = 0.0;
        for (std::size_t k = 1; k < px.size(); ++k) {
            const double x0 = px[k - 1], y0 = py[k - 1], x1 = px[k], y1 = py[k];
            if (std::hypot(x1 - x0, y1 - y0) <= 0.0) continue;
            for (const int c : locator.candidates(x0, y0, x1, y1)) {
                const double len = locator.lengthInside(c, x0, y0, x1, y1);
                if (len <= 0.0) continue;
                sum += len;
                auto it = std::find_if(len_in.begin(), len_in.end(),
                                       [c](const std::pair<int, double>& e) { return e.first == c; });
                if (it == len_in.end()) len_in.emplace_back(c, len);
                else it->second += len;
            }
        }
        if (len_in.empty()) continue;
        ++n_conduits;
        const double denom = std::max(L, sum);   // on-edge double coverage → halves
        for (const auto& e : len_in) {
            GwLinkShare sh;
            sh.link = j; sh.cell = e.first; sh.weight = e.second / denom;
            out.push_back(sh);
        }
    }
    return out;
}

// plans/INFILTRATION_TO_2D_AQUIFER_AND_REMAP_PLAN_2026-10-03.md Step 1: which
// cells each subcatchment's polygon covers, and by how much of its area. The
// polygon ([Polygons], project map units) is clipped against the convex cells
// the locator offers for its bounding box; the per-cell area over the polygon
// area is the share, capped so a polygon never delivers more than it has
// (cells tile the mesh, so the shares only exceed 1 by round-off).
std::vector<GwSubcatchShare> resolveSubcatchInfiltration(const SimulationContext& ctx,
                                                         const MeshData& mesh,
                                                         double node_xy_to_mesh,
                                                         int& n_subcatch) {
    std::vector<GwSubcatchShare> out;
    n_subcatch = 0;
    if (mesh.n_cells() == 0 || mesh.vx.empty()) return out;
    const CellLocator locator(mesh);
    const auto& PX = ctx.spatial.subcatch_polygon_x;
    const auto& PY = ctx.spatial.subcatch_polygon_y;
    for (int s = 0; s < ctx.n_subcatches(); ++s) {
        const auto us = static_cast<std::size_t>(s);
        if (us >= PX.size() || us >= PY.size()) break;
        const std::size_t nv = std::min(PX[us].size(), PY[us].size());
        if (nv < 3) continue;
        std::vector<double> px(nv), py(nv);
        double x0 = 1.0e300, y0 = 1.0e300, x1 = -1.0e300, y1 = -1.0e300;
        for (std::size_t k = 0; k < nv; ++k) {
            px[k] = PX[us][k] * node_xy_to_mesh;
            py[k] = PY[us][k] * node_xy_to_mesh;
            x0 = std::min(x0, px[k]); x1 = std::max(x1, px[k]);
            y0 = std::min(y0, py[k]); y1 = std::max(y1, py[k]);
        }
        const double A = CellLocator::polygonArea(px, py);
        if (A <= 0.0) continue;
        std::vector<std::pair<int, double>> area_in;
        double sum = 0.0;
        for (const int c : locator.candidates(x0, y0, x1, y1)) {
            const double a = locator.areaInside(c, px, py);
            if (a <= 0.0) continue;
            area_in.emplace_back(c, a);
            sum += a;
        }
        if (area_in.empty()) continue;
        ++n_subcatch;
        const double denom = std::max(A, sum);
        for (const auto& e : area_in) {
            GwSubcatchShare sh;
            sh.subcatch = s; sh.cell = e.first; sh.weight = e.second / denom;
            out.push_back(sh);
        }
    }
    return out;
}

std::string parseSurfaceOwnerLine(const std::vector<std::string>& t,std::vector<SurfaceOwnerRecord>& rows){
    if(t.size()!=3||!iequals(t[1],"SUBCATCH")||!iequals(t[2],"UNIFORM"))
        return "expected Subcatchment SUBCATCH UNIFORM; mesh conversion and unreviewed distribution are unavailable.";
    for(const auto& r:rows)if(iequals(r.subcatch,t[0].c_str()))return "duplicate ownership record for "+t[0];
    rows.push_back({t[0]});return {};
}

SurfaceOwnershipPreview resolveSurfaceOwnership(const SimulationContext& ctx,const MeshData& mesh,
    const SolverOptions2D& opts,const SubsurfaceConfig& cfg,const std::vector<SurfaceOwnerRecord>& proposed,
    const std::function<bool(int,int)>& progress){
    using namespace footprint;
    SurfaceOwnershipPreview p;
    const double projectLength=gwUnitFactors(ctx).length;
    const double meshLength=(opts.mesh_units_si||opts.mesh_scaled_to_si)?1.0:projectLength;
    const double areaUnit=projectLength==1?10000.0:43560.0*.3048*.3048;
    auto cancelled=[&](int done){if(progress&&!progress(done,mesh.n_cells()+ctx.n_subcatches())){p.cancelled=true;p.errors.push_back("Preview cancelled; no changes applied.");return true;}return false;};
    std::ostringstream fingerprint;fingerprint<<std::setprecision(17);
    auto hashValue=[&](auto v){fingerprint<<v<<'|';};
    hashValue(ctx.spatial.crs);hashValue(int(ctx.options.flow_units));hashValue(opts.mesh_units_si);hashValue(opts.mesh_scaled_to_si);
    hashValue(opts.groundwater);hashValue(ctx.options.ignore_groundwater);hashValue(ctx.options.ignore_2d);
    hashValue(ctx.options.ignore_routing);hashValue(cfg.options.per_subcatch);hashValue(int(ctx.files.runoff_mode));
    hashValue(ctx.options.water_age);hashValue(ctx.options.heat_transport);hashValue(opts.transport_msx);
    std::string aq;writeSubsurfaceSections(cfg,{}, {},aq,true);hashValue(aq);
    hashValue(cfg.options.wilting_suction);hashValue(cfg.options.c_gw);hashValue(cfg.options.c_col);
    for(const auto& r:cfg.rows){for(double v:{r.Ks,r.zs,r.theta_s,r.theta_r,r.alpha,r.psi_b,r.lambda,r.vg_n,r.vg_L,r.c_loss,r.hg0})hashValue(v);}
    for(const auto& r:cfg.surface_owners)hashValue(r.subcatch);
    for(std::size_t i=0;i<mesh.vx.size();++i){hashValue(mesh.vx[i]);hashValue(mesh.vy[i]);hashValue(mesh.vz[i]);}
    std::vector<Ring> cells(mesh.n_cells());
    for(int c=0;c<mesh.n_cells();++c){
        if(cancelled(c))return p;
        auto& ring=cells[c];bool ok=true;const int nv=mesh.cell_vertex_count(c);hashValue(nv);
        for(int k=0;k<nv;++k){const int v=mesh.cell_vertex(c,k);hashValue(v);
            if(v<0||v>=mesh.n_vertices()||!std::isfinite(mesh.vx[v])||!std::isfinite(mesh.vy[v])){ok=false;break;}ring.push_back({mesh.vx[v],mesh.vy[v]});}
        if(!ok||nv<3||nv>4||std::abs(signedArea(ring))<=tolerance(ring)){ok=false;p.errors.push_back("Invalid geometry for cell "+std::to_string(c+1));}
        if(signedArea(ring)<0)std::reverse(ring.begin(),ring.end());
        for(int k=0;k<nv&&ok;++k)if(cross(ring[k],ring[(k+1)%nv],ring[(k+2)%nv])<=tolerance(ring)){ok=false;p.errors.push_back("Nonconvex/degenerate cell "+std::to_string(c+1));}
        p.mesh_weather_area.push_back(std::abs(signedArea(ring))*meshLength*meshLength);
    }
    std::unique_ptr<CellLocator> locator;
    if(p.errors.empty())locator=std::make_unique<CellLocator>(mesh);
    std::set<int> reviewed;
    for(const auto& r:proposed){const int s=ctx.subcatch_names.find(r.subcatch);
        if(s<0)p.errors.push_back("Unknown subcatchment '"+r.subcatch+"'.");
        else if(!reviewed.insert(s).second)p.errors.push_back("Duplicate ownership for '"+r.subcatch+"'.");
    }
    std::vector<Ring> polygons(ctx.n_subcatches());
    std::vector<std::vector<Ring>> triangles(ctx.n_subcatches());
    for(int s=0;s<ctx.n_subcatches();++s){
        if(cancelled(mesh.n_cells()+s))return p;
        SurfaceOwnerObject o;o.subcatch=s;o.name=ctx.subcatch_names.name_of(s);o.reviewed=reviewed.count(s);
        if(s<int(ctx.subcatches.tags.size()))o.tag=ctx.subcatches.tags[s];
        o.declared_area=ctx.subcatches.area[s]*areaUnit;o.lumped=ctx.subcatches.gw_aquifer[s]>=0;
        hashValue(o.name);hashValue(o.tag);hashValue(ctx.subcatches.area[s]);hashValue(ctx.subcatches.frac_imperv[s]);hashValue(ctx.subcatches.gw_aquifer[s]);hashValue(ctx.subcatches.snowpack[s]);
        for(int u=0;u<ctx.lid_usage.count();++u)if(ctx.lid_usage.subcatch_index[u]==s){
            const double a=ctx.lid_usage.area[u]*ctx.lid_usage.number[u]*projectLength*projectLength;
            hashValue(u);hashValue(ctx.lid_usage.lid_index[u]);hashValue(ctx.lid_usage.number[u]);hashValue(ctx.lid_usage.area[u]);
            hashValue(ctx.lid_usage.from_imperv[u]);hashValue(ctx.lid_usage.from_perv[u]);hashValue(ctx.lid_usage.drain_to[u]);
            const int lid=ctx.lid_usage.lid_index[u];
            if(lid<0||lid>=ctx.lid_controls.count()||ctx.lid_usage.area[u]<0||ctx.lid_usage.number[u]<0||a<0||!std::isfinite(a)){o.reason="Invalid LID area or control.";continue;}
            o.lid_area+=a;const auto& storage=ctx.lid_controls.storage[lid];
            for(double v:storage)hashValue(v);
            if(storage[0]==0||storage[2]>0)o.native_lid_area+=a;
        }
        const double f=ctx.subcatches.frac_imperv[s];
        if(!(o.declared_area>0)||!std::isfinite(o.declared_area)||o.lid_area>o.declared_area||!(f>=0&&f<=1))o.reason="Invalid declared, LID or pervious area.";
        o.pervious_area=(o.declared_area-o.lid_area)*(1-f);o.impervious_area=(o.declared_area-o.lid_area)*f;
        auto& poly=polygons[s];
        if(s<int(ctx.spatial.subcatch_polygon_x.size())&&s<int(ctx.spatial.subcatch_polygon_y.size())){
            const auto& x=ctx.spatial.subcatch_polygon_x[s];const auto& y=ctx.spatial.subcatch_polygon_y[s];
            if(x.size()!=y.size())o.reason="Polygon coordinate arrays differ.";
            for(std::size_t k=0;k<std::min(x.size(),y.size());++k){hashValue(x[k]);hashValue(y[k]);poly.push_back({x[k]*projectLength/meshLength,y[k]*projectLength/meshLength});}
        }
        const auto geometryError=triangulate(poly,triangles[s],[&]{return !cancelled(mesh.n_cells()+s);});
        if(p.cancelled)return p;
        if(!geometryError.empty())o.reason=geometryError;
        o.polygon_area=std::abs(signedArea(poly))*meshLength*meshLength;
        const double eps=std::max(1e-8*std::max(o.declared_area,o.polygon_area),tolerance(poly)*meshLength*meshLength);
        if(o.reason.empty()&&std::abs(o.declared_area-o.polygon_area)>eps)o.reason="Declared/polygon area mismatch; repair area or geometry (no automatic normalization).";
        if(o.reason.empty()){
            double x0=poly[0].x,x1=x0,y0=poly[0].y,y1=y0;
            for(auto v:poly){x0=std::min(x0,v.x);x1=std::max(x1,v.x);y0=std::min(y0,v.y);y1=std::max(y1,v.y);}
            for(int c:locator?locator->candidates(x0,y0,x1,y1):std::vector<int>{}){
                double a=0;for(const auto& tri:triangles[s])a+=intersectionArea(tri,cells[c]);
                a*=meshLength*meshLength;if(a<=eps*1e-3)continue;
                SurfaceOwnerShare sh;sh.subcatch=s;sh.cell=c;sh.weather_area=a;
                const double fraction=a/o.polygon_area;
                sh.pervious_area=o.pervious_area*fraction;sh.impervious_area=o.impervious_area*fraction;
                sh.lid_area=o.lid_area*fraction;sh.native_lid_area=o.native_lid_area*fraction;
                p.shares.push_back(sh);o.inside_area+=a;
            }
            if(o.inside_area>o.polygon_area+eps)o.reason="Mesh intersections overbook the source footprint.";
        }
        o.outside_area=std::max(0.0,o.declared_area-o.inside_area);
        o.status=!o.reason.empty()?3:o.inside_area==0?0:o.reviewed?2:1;
        if(o.reason.empty())o.reason=o.inside_area==0?"Outside mesh; existing path.":o.lumped?"Lumped groundwater wins; no spatial recharge.":o.reviewed?"SUBCATCH UNIFORM; shared receiving limits required.":"Geometric overlap only; ownership unreviewed and recharge unavailable.";
        p.objects.push_back(std::move(o));
    }
    std::set<int> region=reviewed;
    std::vector<std::vector<int>> sourceCells(ctx.n_subcatches()),cellSources(mesh.n_cells());
    for(const auto& sh:p.shares){sourceCells[sh.subcatch].push_back(sh.cell);cellSources[sh.cell].push_back(sh.subcatch);}
    std::vector<int> queue(reviewed.begin(),reviewed.end());std::vector<bool> visitedCell(mesh.n_cells(),false);
    for(std::size_t next=0;next<queue.size();++next){
        if(cancelled(mesh.n_cells()+ctx.n_subcatches()))return p;
        for(int c:sourceCells[queue[next]])if(!visitedCell[c]){visitedCell[c]=true;
            for(int s:cellSources[c])if(region.insert(s).second)queue.push_back(s);
        }
    }
    if(!proposed.empty()){
        if(mesh.n_cells()==0)p.errors.push_back("No mesh; spatial ownership is unavailable.");
        if(cfg.empty()||cfg.options.per_subcatch||opts.groundwater==0||ctx.options.ignore_2d||ctx.options.ignore_routing)
            p.errors.push_back("An enabled 2D MODE MESH aquifer and active routing are required.");
        for(auto& o:p.objects){
            if(o.status==3)p.errors.push_back(o.name+": "+o.reason);
            else if(region.count(o.subcatch)&&o.inside_area>0&&!o.reviewed)p.errors.push_back(o.name+": competing footprint is not reviewed.");
        }
        // Sweep along x so disjoint source footprints do not form an all-pairs scan.
        struct Bounds{int source;double x0,y0,x1,y1;};std::vector<Bounds> bounds;
        for(int i=0;i<int(polygons.size());++i)if(!polygons[i].empty()){
            const auto& poly=polygons[i];Bounds box{i,poly[0].x,poly[0].y,poly[0].x,poly[0].y};
            for(auto v:poly){box.x0=std::min(box.x0,v.x);box.y0=std::min(box.y0,v.y);box.x1=std::max(box.x1,v.x);box.y1=std::max(box.y1,v.y);}bounds.push_back(box);
        }
        std::sort(bounds.begin(),bounds.end(),[](const auto& a,const auto& b){return a.x0<b.x0;});
        for(std::size_t a=0;a<bounds.size();++a)for(std::size_t b=a+1;b<bounds.size()&&bounds[b].x0<bounds[a].x1;++b){
            if(cancelled(mesh.n_cells()+ctx.n_subcatches()))return p;
            const auto& bi=bounds[a];const auto& bj=bounds[b];const int i=bi.source,j=bj.source;
            if(!(region.count(i)||region.count(j))||bi.y1<=bj.y0||bj.y1<=bi.y0)continue;
            double overlap=0;for(const auto& ta:triangles[i])for(const auto& tb:triangles[j])overlap+=intersectionArea(ta,tb);
            if(overlap*meshLength*meshLength>1e-8*std::max(p.objects[i].polygon_area,p.objects[j].polygon_area)){
                const auto error="Overlapping subcatchment footprints: "+p.objects[i].name+" / "+p.objects[j].name+"; edit geometry.";
                p.errors.push_back(error);p.objects[i].status=p.objects[j].status=3;p.objects[i].reason=p.objects[j].reason=error;
            }
        }
    }
    for(const auto& sh:p.shares)if(reviewed.count(sh.subcatch))p.mesh_weather_area[sh.cell]-=sh.weather_area;
    for(int c=0;c<mesh.n_cells();++c){const double eps=1e-8*std::abs(signedArea(cells[c]))*meshLength*meshLength;
        if(p.mesh_weather_area[c]<-eps)p.errors.push_back("Cell "+std::to_string(c+1)+" weather area is overbooked.");
        p.mesh_weather_area[c]=std::max(0.0,p.mesh_weather_area[c]);
    }
    std::sort(p.shares.begin(),p.shares.end(),[](const auto& a,const auto& b){return a.cell!=b.cell?a.cell<b.cell:a.subcatch<b.subcatch;});
    uint64_t hash=14695981039346656037ULL;for(unsigned char c:fingerprint.str()){hash^=c;hash*=1099511628211ULL;}
    std::ostringstream token;token<<std::hex<<std::setw(16)<<std::setfill('0')<<hash;p.token=token.str();
    return p;
}

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------

void writeSubsurfaceSections(const SubsurfaceConfig& cfg,
                             const std::vector<std::string>& node_names,
                             const std::vector<std::string>& link_names,
                             std::string& out, bool full_precision) {
    auto fmt=[full_precision](double v){if(!full_precision)return ::openswmm::twoD::fmt(v);char buf[40];std::snprintf(buf,sizeof buf,"%.17g",v);return std::string(buf);};
    if(!cfg.surface_owners.empty()){
        out+="\n[2D_SURFACE_OWNERSHIP]\n;;Subcatchment       Representation Distribution\n";
        for(const auto& r:cfg.surface_owners)out+=r.subcatch+" SUBCATCH UNIFORM\n";
    }
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
        if(cfg.options.wilting_suction_set)kv("WILTING_SUCTION",fmt(cfg.options.wilting_suction));
        if (cfg.options.node_auto != d.node_auto)
            kv("NODE_ENROLMENT", cfg.options.node_auto ? "AUTO" : "ROWS");   // G-X2
        if (cfg.options.link_seepage != d.link_seepage)                    // G-X3/G-X4
            kv("LINK_SEEPAGE",
               cfg.options.link_seepage == GwLinkMode::NONE    ? "NONE"
             : cfg.options.link_seepage == GwLinkMode::TWO_WAY ? "TWO_WAY"
                                                               : "ONE_WAY");
        if(body.empty()&&cfg.rows.empty())kv("GW_ET","AUTO"); // Preserve implicit aquifer configuration.
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
            line.append(std::max<int>(1, 18 - int(line.size())), ' ');
            for (double v : {r.Ks, r.zs, r.theta_s, r.theta_r, r.alpha}) {
                const std::string s = fmt(v);
                line += s;
                line.append(std::max<int>(1, 11 - int(s.size())), ' ');
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
            line.append(std::max<int>(1, 19 - int(line.size())), ' ');
            const std::string c = b.locate ? std::string("AUTO")
                                           : std::to_string(b.cell + 1);
            line += c;
            line.append(std::max<int>(1, 11 - int(c.size())), ' ');
            if (b.Kc   > 0.0) { line += "KC ";   line += fmt(b.Kc);   line += ' '; }
            if (b.dC   > 0.0) { line += "DC ";   line += fmt(b.dC);   line += ' '; }
            if (b.area > 0.0) { line += "AREA "; line += fmt(b.area); line += ' '; }
            if (!b.exchange)  { line += "EXCHANGE NO "; }
            while (!line.empty() && line.back() == ' ') line.pop_back();
            out += line;
            out += '\n';
        }
    }

    // G-X4: the per-conduit overrides. Echoed verbatim — the rows carry the
    // authored numbers, never a converted copy (the GwUnitFactors note).
    if (!cfg.link_rows.empty()) {
        out += "\n[2D_AQUIFER_LINKS]\n";
        out += ";;Link              Options\n";
        for (std::size_t i = 0; i < cfg.link_rows.size(); ++i) {
            const auto& r = cfg.link_rows[i];
            std::string line = (i < link_names.size()) ? link_names[i]
                                                       : std::string("?");
            line.append(std::max<int>(1, 19 - int(line.size())), ' ');
            if (r.Kc > 0.0) { line += "KC "; line += fmt(r.Kc); line += ' '; }
            if (r.dC > 0.0) { line += "DC "; line += fmt(r.dC); line += ' '; }
            if (!r.exchange) { line += "EXCHANGE NO "; }
            while (!line.empty() && line.back() == ' ') line.pop_back();
            out += line;
            out += '\n';
        }
    }
}

}  // namespace openswmm::twoD
