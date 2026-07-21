/**
 * @file GridMappingWeights.hpp
 * @brief Grid→target weight builders for soft-rainfall mappings (SR-4a).
 *
 * @details Header-only geometry helpers for the BILINEAR (2D cells) and
 *          AREA_MEAN (subcatchment polygons) grid mappings described in
 *          SOFT_RAINFALL_DESIGN.md §5. The grid is regular: pixel (ix,iy) is
 *          centered at (x[ix], y[iy]) and spans the half-distance to its
 *          neighbours (edge cells extend by half a cell).
 *
 *          - `bilinearWeights`: 4-pixel weighted gather for one query point.
 *          - `polygonPixelWeights`: polygon∩pixel area-fraction weights via
 *            Sutherland–Hodgman rectangle clipping + shoelace area.
 *
 *          Both are pure functions with no engine dependencies, so they are
 *          unit-testable in isolation.
 *
 * @ingroup engine_uncertainty
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_GRID_MAPPING_WEIGHTS_HPP
#define OPENSWMM_ENGINE_GRID_MAPPING_WEIGHTS_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace openswmm::uncertainty {

// ============================================================================
// BILINEAR
// ============================================================================

/**
 * @brief 4-pixel bilinear weights for query point (cx, cy) on a regular grid.
 *
 * Fills @p idx (4 flat pixel indices, row-major `iy*nx + ix`) and @p w (4
 * weights summing to 1). The query is clamped to [0,1] within its cell so
 * points outside the grid extent use edge values (no extrapolation), matching
 * the CENTROID clamp-to-edge policy.
 *
 * @return false when the grid is degenerate (nx<2 or ny<2) — the caller
 *         should fall back to CENTROID.
 */
inline bool bilinearWeights(double cx, double cy,
                            const std::vector<double>& xc,
                            const std::vector<double>& yc,
                            std::uint32_t idx[4], double w[4]) {
    const int nx = static_cast<int>(xc.size());
    const int ny = static_cast<int>(yc.size());
    if (nx < 2 || ny < 2) return false;

    // Lower-left cell (ix0, iy0): largest index with coord <= c, clamped to
    // [0, n-2] so (ix0+1, iy0+1) is always in range.
    int ix0 = 0;
    for (int j = 0; j < nx - 1; ++j)
        if (xc[static_cast<std::size_t>(j)] <= cx) ix0 = j;
    int iy0 = 0;
    for (int j = 0; j < ny - 1; ++j)
        if (yc[static_cast<std::size_t>(j)] <= cy) iy0 = j;

    const double x0 = xc[static_cast<std::size_t>(ix0)];
    const double x1 = xc[static_cast<std::size_t>(ix0 + 1)];
    const double y0 = yc[static_cast<std::size_t>(iy0)];
    const double y1 = yc[static_cast<std::size_t>(iy0 + 1)];

    double tx = (x1 > x0) ? (cx - x0) / (x1 - x0) : 0.0;
    double ty = (y1 > y0) ? (cy - y0) / (y1 - y0) : 0.0;
    tx = std::clamp(tx, 0.0, 1.0);
    ty = std::clamp(ty, 0.0, 1.0);

    idx[0] = static_cast<std::uint32_t>(iy0 * nx + ix0);
    idx[1] = static_cast<std::uint32_t>(iy0 * nx + ix0 + 1);
    idx[2] = static_cast<std::uint32_t>((iy0 + 1) * nx + ix0);
    idx[3] = static_cast<std::uint32_t>((iy0 + 1) * nx + ix0 + 1);
    w[0] = (1.0 - tx) * (1.0 - ty);
    w[1] = tx * (1.0 - ty);
    w[2] = (1.0 - tx) * ty;
    w[3] = tx * ty;
    return true;
}

// ============================================================================
// AREA_MEAN — polygon∩pixel geometry
// ============================================================================

namespace detail {

struct MapPt { double x, y; };

/// Absolute area of a simple polygon (shoelace).
inline double polygonArea(const std::vector<MapPt>& p) {
    const std::size_t n = p.size();
    if (n < 3) return 0.0;
    double a = 0.0;
    for (std::size_t i = 0, j = n - 1; i < n; j = i++)
        a += (p[j].x + p[i].x) * (p[j].y - p[i].y);
    return 0.5 * std::abs(a);
}

/// Sutherland–Hodgman clip against one half-plane.
template <typename KeepFn, typename IsectFn>
inline std::vector<MapPt> clipHalfplane(const std::vector<MapPt>& in,
                                        KeepFn keep, IsectFn isect) {
    std::vector<MapPt> out;
    const std::size_t n = in.size();
    if (n == 0) return out;
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const MapPt& cur = in[i];
        const MapPt& prev = in[j];
        const bool cur_in = keep(cur);
        const bool prev_in = keep(prev);
        if (cur_in) {
            if (!prev_in) out.push_back(isect(prev, cur));
            out.push_back(cur);
        } else if (prev_in) {
            out.push_back(isect(prev, cur));
        }
    }
    return out;
}

/// Clip polygon to axis-aligned rectangle [xmin,xmax]×[ymin,ymax].
inline std::vector<MapPt> clipToRect(const std::vector<MapPt>& poly,
                                     double xmin, double xmax,
                                     double ymin, double ymax) {
    std::vector<MapPt> p = poly;
    p = clipHalfplane(p, [&](const MapPt& q){ return q.x >= xmin; },
        [&](const MapPt& a, const MapPt& b){ double t=(xmin-a.x)/(b.x-a.x); return MapPt{xmin, a.y+t*(b.y-a.y)}; });
    if (p.empty()) return p;
    p = clipHalfplane(p, [&](const MapPt& q){ return q.x <= xmax; },
        [&](const MapPt& a, const MapPt& b){ double t=(xmax-a.x)/(b.x-a.x); return MapPt{xmax, a.y+t*(b.y-a.y)}; });
    if (p.empty()) return p;
    p = clipHalfplane(p, [&](const MapPt& q){ return q.y >= ymin; },
        [&](const MapPt& a, const MapPt& b){ double t=(ymin-a.y)/(b.y-a.y); return MapPt{a.x+t*(b.x-a.x), ymin}; });
    if (p.empty()) return p;
    p = clipHalfplane(p, [&](const MapPt& q){ return q.y <= ymax; },
        [&](const MapPt& a, const MapPt& b){ double t=(ymax-a.y)/(b.y-a.y); return MapPt{a.x+t*(b.x-a.x), ymax}; });
    return p;
}

/// Bounds [lo, hi] of pixel index i along a regular cell-center axis.
inline void pixelBounds(const std::vector<double>& coords, int i,
                        double& lo, double& hi) {
    const int n = static_cast<int>(coords.size());
    const double c = coords[static_cast<std::size_t>(i)];
    if (n == 1) { lo = c - 0.5; hi = c + 0.5; return; }
    if (i == 0)     lo = c - 0.5 * (coords[1] - coords[0]);
    else            lo = 0.5 * (coords[static_cast<std::size_t>(i - 1)] + c);
    if (i == n - 1) hi = c + 0.5 * (c - coords[static_cast<std::size_t>(n - 2)]);
    else            hi = 0.5 * (c + coords[static_cast<std::size_t>(i + 1)]);
}

} // namespace detail

/**
 * @brief Polygon∩pixel area-fraction weights for the AREA_MEAN mapping.
 *
 * Fills @p out_px (flat pixel indices `iy*nx + ix`) and @p out_w (area
 * fractions summing to 1 over the covered area). An empty result means the
 * polygon is degenerate (<3 vertices, zero area) or does not overlap the grid
 * — the caller should fall back to CENTROID.
 */
inline void polygonPixelWeights(const std::vector<double>& px,
                                const std::vector<double>& py,
                                const std::vector<double>& xc,
                                const std::vector<double>& yc,
                                std::vector<std::uint32_t>& out_px,
                                std::vector<float>& out_w) {
    using detail::MapPt;
    out_px.clear();
    out_w.clear();
    const std::size_t nv = px.size();
    if (nv < 3 || py.size() != nv) return;

    std::vector<MapPt> poly(nv);
    for (std::size_t k = 0; k < nv; ++k) poly[k] = MapPt{px[k], py[k]};
    if (detail::polygonArea(poly) <= 0.0) return;

    const int nx = static_cast<int>(xc.size());
    const int ny = static_cast<int>(yc.size());

    double bx0 = px[0], bx1 = px[0], by0 = py[0], by1 = py[0];
    for (std::size_t k = 1; k < nv; ++k) {
        bx0 = std::min(bx0, px[k]); bx1 = std::max(bx1, px[k]);
        by0 = std::min(by0, py[k]); by1 = std::max(by1, py[k]);
    }

    double total = 0.0;
    for (int iy = 0; iy < ny; ++iy) {
        double ylo, yhi; detail::pixelBounds(yc, iy, ylo, yhi);
        if (yhi < by0 || ylo > by1) continue;
        for (int ix = 0; ix < nx; ++ix) {
            double xlo, xhi; detail::pixelBounds(xc, ix, xlo, xhi);
            if (xhi < bx0 || xlo > bx1) continue;
            const double a = detail::polygonArea(detail::clipToRect(poly, xlo, xhi, ylo, yhi));
            if (a > 0.0) {
                out_px.push_back(static_cast<std::uint32_t>(iy * nx + ix));
                out_w.push_back(static_cast<float>(a));
                total += a;
            }
        }
    }

    if (total <= 0.0 || out_px.empty()) { out_px.clear(); out_w.clear(); return; }
    for (float& w : out_w) w = static_cast<float>(w / total);
}

} // namespace openswmm::uncertainty

#endif // OPENSWMM_ENGINE_GRID_MAPPING_WEIGHTS_HPP
