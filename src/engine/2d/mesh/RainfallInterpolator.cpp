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
 * @file RainfallInterpolator.cpp
 * @brief Implementation of natural-neighbour / IDW rainfall interpolation.
 *
 * @see RainfallInterpolator.hpp
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "RainfallInterpolator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>
#include <utility>
#include <stdexcept>
#include <limits>

namespace openswmm::twoD {
namespace {

struct Pt { double x, y; };
struct Tri { int a, b, c; };   ///< site indices, kept counter-clockwise (CCW)

inline double orient2d(const Pt& a, const Pt& b, const Pt& c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

inline double dist2(const Pt& p, const Pt& q) {
    const double dx = p.x - q.x, dy = p.y - q.y;
    return dx * dx + dy * dy;
}

/// Circumcenter of triangle (a,b,c). Caller must guard against collinear input
/// (orient2d ≈ 0), where the denominator d vanishes.
inline Pt circumcenter(const Pt& a, const Pt& b, const Pt& c) {
    // Translate before squaring: large projected coordinates must not
    // cancel in the circumcenter numerator.
    const double bx = b.x - a.x, by = b.y - a.y;
    const double cx = c.x - a.x, cy = c.y - a.y;
    const double d = 2.0 * (bx * cy - by * cx);
    const double b2 = bx * bx + by * by, c2 = cx * cx + cy * cy;
    return {a.x + (b2 * cy - c2 * by) / d,
            a.y + (bx * c2 - cx * b2) / d};
}

/// True if p lies strictly inside the circumcircle of CCW triangle (a,b,c).
inline bool inCircle(const Pt& a, const Pt& b, const Pt& c, const Pt& p) {
    const double ax = a.x - p.x, ay = a.y - p.y;
    const double bx = b.x - p.x, by = b.y - p.y;
    const double cx = c.x - p.x, cy = c.y - p.y;
    const double det =
        (ax * ax + ay * ay) * (bx * cy - cx * by)
      - (bx * bx + by * by) * (ax * cy - cx * ay)
      + (cx * cx + cy * cy) * (ax * by - bx * ay);
    return det > 0.0;
}

/// Boundary directed edges of a cavity (triangles flagged "bad"): an edge (u,v)
/// is on the boundary when its reverse (v,u) is not also a bad-triangle edge.
/// Sizes are tiny (cavity ⊂ a < ~30-site triangulation), so O(n²) is fine.
std::vector<std::array<int, 2>>
cavityBoundary(const std::vector<std::array<int, 2>>& badEdges) {
    std::vector<std::array<int, 2>> boundary;
    for (const auto& e : badEdges) {
        bool shared = false;
        for (const auto& f : badEdges) {
            if (f[0] == e[1] && f[1] == e[0]) { shared = true; break; }
        }
        if (!shared) boundary.push_back(e);
    }
    return boundary;
}

/// Delaunay triangulation of `site` via Bowyer–Watson. Returns CCW triangles
/// over the site indices, or an empty list if the sites are degenerate
/// (all coincident or collinear) — the caller then falls back to IDW.
std::vector<Tri> delaunay(const std::vector<Pt>& site) {
    const int m = static_cast<int>(site.size());
    if (m < 3) return {};

    double minx = site[0].x, maxx = site[0].x, miny = site[0].y, maxy = site[0].y;
    for (const auto& s : site) {
        minx = std::min(minx, s.x); maxx = std::max(maxx, s.x);
        miny = std::min(miny, s.y); maxy = std::max(maxy, s.y);
    }
    const double dmax = std::max(maxx - minx, maxy - miny);
    if (dmax <= 0.0) return {};   // all coincident

    // Points array = sites + 3 super-triangle vertices enclosing the bbox.
    std::vector<Pt> P = site;
    const double midx = 0.5 * (minx + maxx), midy = 0.5 * (miny + maxy);
    const double R = 20.0 * dmax;
    P.push_back(Pt{midx - 2.0 * R, midy - R});
    P.push_back(Pt{midx + 2.0 * R, midy - R});
    P.push_back(Pt{midx,           midy + 2.0 * R});
    const int s0 = m, s1 = m + 1, s2 = m + 2;

    auto ccw = [&](int a, int b, int c) -> Tri {
        if (orient2d(P[a], P[b], P[c]) < 0.0) std::swap(b, c);
        return Tri{a, b, c};
    };

    std::vector<Tri> tris;
    tris.push_back(ccw(s0, s1, s2));

    for (int i = 0; i < m; ++i) {
        const Pt& p = P[i];
        std::vector<std::array<int, 2>> badEdges;
        std::vector<Tri> good;
        good.reserve(tris.size());
        for (const auto& t : tris) {
            if (inCircle(P[t.a], P[t.b], P[t.c], p)) {
                badEdges.push_back({t.a, t.b});
                badEdges.push_back({t.b, t.c});
                badEdges.push_back({t.c, t.a});
            } else {
                good.push_back(t);
            }
        }
        tris.swap(good);
        for (const auto& e : cavityBoundary(badEdges))
            tris.push_back(ccw(e[0], e[1], i));
    }

    // Drop triangles touching a super vertex.
    std::vector<Tri> result;
    result.reserve(tris.size());
    for (const auto& t : tris)
        if (t.a < m && t.b < m && t.c < m) result.push_back(t);
    return result;
}

using Weights = std::vector<std::pair<int, double>>;   // (global gage idx, weight)

/// Inverse-distance (power 2) weighting over all located sites.
Weights idwAll(const Pt& p, const std::vector<Pt>& site, const std::vector<int>& gid) {
    const int m = static_cast<int>(site.size());
    for (int j = 0; j < m; ++j)
        if (dist2(p, site[j]) < 1.0e-18) return {{gid[j], 1.0}};
    double minD2 = std::numeric_limits<double>::infinity();
    for (const auto& s : site) minD2 = std::min(minD2, dist2(p, s));
    Weights w;
    w.reserve(m);
    double sum = 0.0;
    for (int j = 0; j < m; ++j) {
        const double wj = minD2 / dist2(p, site[j]); // rescaled 1/d²; avoids overflow
        w.push_back({gid[j], wj});
        sum += wj;
    }
    for (auto& e : w) e.second /= sum;
    return w;
}

/// Laplace (non-Sibsonian) natural-neighbour weights at p over the Delaunay
/// triangulation `tris`. Returns empty when p is outside the convex hull or the
/// construction is degenerate — the caller then falls back to IDW.
Weights laplaceWeights(const Pt& p, const std::vector<Pt>& site,
                       const std::vector<int>& gid, const std::vector<Tri>& tris) {
    const int m = static_cast<int>(site.size());
    for (int j = 0; j < m; ++j)
        if (dist2(p, site[j]) < 1.0e-18) return {{gid[j], 1.0}};

    // Inside-hull test: p must lie in some Delaunay triangle (their union is the
    // convex hull). A CCW triangle contains p when all three orient2d ≥ 0.
    bool inside = false;
    for (const auto& t : tris) {
        const double d0 = orient2d(site[t.a], site[t.b], p);
        const double d1 = orient2d(site[t.b], site[t.c], p);
        const double d2 = orient2d(site[t.c], site[t.a], p);
        if (!((d0 < 0 || d1 < 0 || d2 < 0) && (d0 > 0 || d1 > 0 || d2 > 0))) {
            inside = true;
            break;
        }
    }
    if (!inside) return {};

    // Insertion cavity: triangles whose circumcircle contains p. Inserting p
    // creates a fan of triangles (p, u, v) over the cavity boundary edges. The
    // Voronoi facet between p and a boundary site s is bounded by the
    // circumcenters of the two fan triangles incident to s, so its length is
    // |circ(p, prev, s) − circ(p, s, next)|. Laplace weight = facet / |p − s|.
    std::vector<std::array<int, 2>> badEdges;
    for (const auto& t : tris) {
        if (inCircle(site[t.a], site[t.b], site[t.c], p)) {
            badEdges.push_back({t.a, t.b});
            badEdges.push_back({t.b, t.c});
            badEdges.push_back({t.c, t.a});
        }
    }
    const auto boundary = cavityBoundary(badEdges);
    if (boundary.empty()) return {};

    // Circumcenter of each fan triangle (p, u, v); index by tail u and head v.
    std::unordered_map<int, Pt> cByTail, cByHead;
    for (const auto& e : boundary) {
        const Pt& u = site[e[0]];
        const Pt& v = site[e[1]];
        if (std::fabs(orient2d(p, u, v)) < 1.0e-20) return {};  // collinear → IDW
        const Pt cc = circumcenter(p, u, v);
        cByTail[e[0]] = cc;
        cByHead[e[1]] = cc;
    }

    Weights w;
    double sum = 0.0;
    for (const auto& e : boundary) {
        const int s = e[1];   // each boundary site is the head of exactly one edge
        auto inEdge  = cByHead.find(s);   // fan triangle (p, prev, s)
        auto outEdge = cByTail.find(s);   // fan triangle (p, s, next)
        if (inEdge == cByHead.end() || outEdge == cByTail.end()) return {};
        const double facet = std::sqrt(dist2(inEdge->second, outEdge->second));
        const double wj    = facet / std::sqrt(dist2(p, site[s]));
        w.push_back({gid[s], wj});
        sum += wj;
    }
    if (!(sum > 0.0) || !std::isfinite(sum)) return {};
    for (auto& e : w) e.second /= sum;
    return w;
}

} // namespace

void RainfallInterpolator::build(const std::vector<double>& cx,
                                 const std::vector<double>& cy,
                                 const std::vector<double>& gage_x,
                                 const std::vector<double>& gage_y,
                                 int n_gages, double gage_scale, Method method) {
    ready_ = false;
    diagnostics_ = {};
    if (cx.size() != cy.size() || !std::isfinite(gage_scale) || gage_scale <= 0.0)
        throw std::invalid_argument("2D rainfall: invalid coordinate dimensions or scale");
    for (std::size_t i = 0; i < cx.size(); ++i)
        if (!std::isfinite(cx[i]) || !std::isfinite(cy[i]))
            throw std::invalid_argument("2D rainfall: nonfinite cell centroid " + std::to_string(i));
    nt_ = static_cast<int>(cx.size());
    w_ptr_.assign(static_cast<std::size_t>(nt_) + 1, 0);
    w_gage_.clear();
    w_val_.clear();
    cell_method_.assign(static_cast<std::size_t>(nt_), CellMethod::Nearest);

    // Collect located gages (skip the un-located (0,0) sentinel and any gage
    // that duplicates an already-collected position — a duplicate would make
    // the triangulation degenerate, and natural neighbour is ill-defined for
    // two gages at one point).
    std::vector<Pt>  site;
    std::vector<int> gid;
    const int gx_n = static_cast<int>(gage_x.size());
    const int gy_n = static_cast<int>(gage_y.size());
    for (int g = 0; g < n_gages; ++g) {
        const double gx = (g < gx_n) ? gage_x[g] : 0.0;
        const double gy = (g < gy_n) ? gage_y[g] : 0.0;
        if (gx == 0.0 && gy == 0.0) { ++diagnostics_.unlocated; continue; }
        const Pt s{gx * gage_scale, gy * gage_scale};
        if (!std::isfinite(s.x) || !std::isfinite(s.y)) { ++diagnostics_.invalid; continue; }
        bool dup = false;
        for (const auto& e : site)
            if (dist2(e, s) < 1.0e-12) { dup = true; break; }
        if (dup) { ++diagnostics_.duplicates; continue; }
        site.push_back(s);
        gid.push_back(g);
    }

    const int m = static_cast<int>(site.size());
    if (m == 0 || nt_ == 0) return;   // ready_ stays false → caller uses SYSTEM mean

    // Work in one dimensionless local frame for both sites and cells.
    // Rainfall weights should be independent of CRS origin and length scale.
    const Pt origin = site.front();
    double scale = 0.0;
    for (const auto& s : site)
        scale = std::max(scale, std::max(std::fabs(s.x - origin.x), std::fabs(s.y - origin.y)));
    if (!(scale > 0.0)) scale = 1.0;
    for (auto& s : site) { s.x = (s.x - origin.x) / scale; s.y = (s.y - origin.y) / scale; }

    std::vector<Weights> rows(static_cast<std::size_t>(nt_));
    const auto tris = method == Method::NaturalNeighbour ? delaunay(site) : std::vector<Tri>{};
    for (int i = 0; i < nt_; ++i) {
        const Pt p{(cx[i] - origin.x) / scale, (cy[i] - origin.y) / scale};
        Weights w;
        if (m == 1) w = {{gid[0], 1.0}};
        else if (method == Method::NearestNeighbour) {
            int nearest = 0;
            double best = dist2(p, site[0]);
            for (int g = 1; g < m; ++g) {
                const double d = dist2(p, site[g]);
                if (d < best) { best = d; nearest = g; }
            }
            w = {{gid[nearest], 1.0}};
        } else {
            cell_method_[static_cast<std::size_t>(i)] = CellMethod::NaturalNeighbour;
            if (!tris.empty()) w = laplaceWeights(p, site, gid, tris);
            if (w.empty()) {
                w = idwAll(p, site, gid);
                ++diagnostics_.idwCells;
                cell_method_[static_cast<std::size_t>(i)] = CellMethod::InverseDistance;
            }
        }
        double sum = 0.0;
        for (const auto& e : w) {
            if (!std::isfinite(e.second) || e.second < 0.0)
                throw std::runtime_error("2D rainfall: invalid weight at cell " + std::to_string(i));
            sum += e.second;
        }
        if (!(sum > 0.0) || !std::isfinite(sum))
            throw std::runtime_error("2D rainfall: no valid weights at cell " + std::to_string(i));
        for (auto& e : w) e.second /= sum;
        rows[i] = std::move(w);
    }

    int total = 0;
    for (const auto& r : rows) total += static_cast<int>(r.size());
    w_gage_.reserve(static_cast<std::size_t>(total));
    w_val_.reserve(static_cast<std::size_t>(total));
    for (int i = 0; i < nt_; ++i) {
        w_ptr_[static_cast<std::size_t>(i)] = static_cast<int>(w_gage_.size());
        for (const auto& e : rows[i]) {
            w_gage_.push_back(e.first);
            w_val_.push_back(e.second);
        }
    }
    w_ptr_[static_cast<std::size_t>(nt_)] = static_cast<int>(w_gage_.size());
    ready_ = true;
}

void RainfallInterpolator::apply(const std::vector<double>& rain,
                                 std::vector<double>& out) const {
    if (static_cast<int>(out.size()) != nt_) out.assign(static_cast<std::size_t>(nt_), 0.0);
    for (int i = 0; i < nt_; ++i) {
        double acc = 0.0;
        const int beg = w_ptr_[static_cast<std::size_t>(i)];
        const int end = w_ptr_[static_cast<std::size_t>(i) + 1];
        for (int k = beg; k < end; ++k)
            acc += w_val_[static_cast<std::size_t>(k)]
                 * rain[static_cast<std::size_t>(w_gage_[static_cast<std::size_t>(k)])];
        out[static_cast<std::size_t>(i)] = acc;
    }
}

void RainfallInterpolator::cellWeights(int i, std::vector<int>& gages,
                                       std::vector<double>& weights) const {
    const int beg = w_ptr_[static_cast<std::size_t>(i)];
    const int end = w_ptr_[static_cast<std::size_t>(i) + 1];
    gages.assign(w_gage_.begin() + beg, w_gage_.begin() + end);
    weights.assign(w_val_.begin() + beg, w_val_.begin() + end);
}

} // namespace openswmm::twoD
