// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin

/**
 * @file      FootprintGeometry.hpp
 * @brief     Geometry and overlap calculations for source footprints.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_FOOTPRINT_GEOMETRY_HPP
#define OPENSWMM_FOOTPRINT_GEOMETRY_HPP
#include <algorithm>
#include <functional>
#include <cmath>
#include <numeric>
#include <limits>
#include <string>
#include <vector>
namespace openswmm::twoD::footprint {
struct Point {
    double x = 0, y = 0;
};
using Ring = std::vector<Point>;
/**
 * @brief Compute the oriented twice-area of a triangle in planar coordinates.
 * @param[in] a First point, serving as the subtraction origin.
 * @param[in] b Second point.
 * @param[in] c Third point.
 * @return Signed cross product (b-a) x (c-a), positive for counterclockwise order, in squared
 * coordinate units (m2 for SI mesh coordinates). No finite-value or degeneracy checks are made.
 */
inline double cross(Point a, Point b, Point c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}
/**
 * @brief Sum a polygon ring's signed triangle-fan area.
 * @param[in] p Stable planar vertices, implicitly closed; a repeated final vertex is permitted.
 * @return Signed area in squared coordinate units, positive for counterclockwise winding, or
 * zero for fewer than three vertices. Self intersections and nonfinite coordinates are
 * unchecked; simple-ring validation belongs to triangulate(). No allocation or input mutation
 * occurs.
 */
inline double signedArea(const Ring& p) {
    if (p.size() < 3)
        return 0;
    double sum = 0;
    for (std::size_t i = 1; i + 1 < p.size(); ++i)
        sum += cross(p[0], p[i], p[i + 1]);
    return .5 * sum;
}
/**
 * @brief Derive a scale-relative tolerance for ring cross products and areas.
 * @param[in] p Planar ring; coordinates should be finite. Extent is measured relative to its
 * first vertex using the largest absolute coordinate offset.
 * @return 128 machine epsilons times max(span squared, 1e-24), or zero for an empty ring, in
 * squared coordinate units. The lower floor is absolute, so coordinate-unit changes affect it.
 */
inline double tolerance(const Ring& p) {
    if (p.empty())
        return 0;
    double span = 0;
    for (auto v : p)
        span = std::max(span, std::max(std::abs(v.x - p[0].x), std::abs(v.y - p[0].y)));
    return 128 * std::numeric_limits<double>::epsilon() * std::max(span * span, 1e-24);
}
/**
 * @brief Test whether a point lies on a closed segment within a cross-product tolerance.
 * @param[in] a First segment endpoint.
 * @param[in] b Second segment endpoint; equality with a is allowed.
 * @param[in] p Point tested against the segment.
 * @param[in] eps Nonnegative tolerance in squared coordinate units for collinearity only.
 * @return True when the cross-product magnitude is at most eps and both point coordinates are
 * inside the exact endpoint bounds. Bounds are not expanded by eps; inputs are not validated.
 */
inline bool between(Point a, Point b, Point p, double eps) {
    return std::abs(cross(a, b, p)) <= eps && p.x >= std::min(a.x, b.x) &&
           p.x <= std::max(a.x, b.x) && p.y >= std::min(a.y, b.y) && p.y <= std::max(a.y, b.y);
}
/**
 * @brief Test closed planar segments for proper crossing or endpoint/collinear contact.
 * @param[in] a First endpoint of the first segment.
 * @param[in] b Second endpoint of the first segment.
 * @param[in] c First endpoint of the second segment.
 * @param[in] d Second endpoint of the second segment.
 * @param[in] eps Nonnegative cross-product tolerance in squared coordinate units.
 * @return True for opposite-sided crossings beyond eps or a between() contact; false otherwise.
 * Finite coordinates are assumed. No allocation or mutation occurs.
 */
inline bool intersects(Point a, Point b, Point c, Point d, double eps) {
    const auto x = cross(a, b, c), y = cross(a, b, d), z = cross(c, d, a), w = cross(c, d, b);
    if (((x > eps && y < -eps) || (x < -eps && y > eps)) &&
        ((z > eps && w < -eps) || (z < -eps && w > eps)))
        return true;
    return between(a, b, c, eps) || between(a, b, d, eps) || between(c, d, a, eps) ||
           between(c, d, b, eps);
}
// Validate simple rings, then triangulate: disconnected intersections of a
// concave footprint remain a sum of disjoint triangles, not a bridged ring.
/**
 * @brief Validate a simple ring and triangulate it by counterclockwise ear clipping.
 * @param[in,out] p Finite planar polygon. An exact repeated closing vertex is removed, clockwise
 * order is reversed and collinear boundary vertices may be erased. These changes can survive a
 * later failure; holes and multipart rings are unsupported.
 * @param[out] triangles Cleared immediately, then populated with owned three-point rings in p's
 * coordinate units. Cancellation or triangulation failure can leave a partial triangulation.
 * @param[in] running Optional synchronous continuation callback; false cancels validation or the
 * ear loop. The callback is not retained; its exceptions propagate.
 * @return Empty string on success; otherwise a diagnostic for missing/nonfinite/duplicate,
 * self-intersecting or degenerate geometry, cancellation or failure to find an ear.
 * @note Allocation exceptions propagate. Output must not alias p through an element of
 * triangles; the method supplies no rollback and requires exclusive access to both mutable
 * containers.
 */
inline std::string triangulate(Ring& p, std::vector<Ring>& triangles,
                               const std::function<bool()>& running = {}) {
    triangles.clear();
    if (p.size() > 1 && p.front().x == p.back().x && p.front().y == p.back().y)
        p.pop_back();
    if (p.size() < 3)
        return "Missing polygon (unmapped).";
    for (auto v : p)
        if (!std::isfinite(v.x) || !std::isfinite(v.y))
            return "Non-finite polygon coordinate.";
    const double eps = tolerance(p);
    for (std::size_t i = 0; i < p.size(); ++i) {
        if (running && !running())
            return "Preview cancelled.";
        const auto j = (i + 1) % p.size();
        if (p[i].x == p[j].x && p[i].y == p[j].y)
            return "Duplicate polygon vertex.";
        for (std::size_t k = i + 1; k < p.size(); ++k) {
            const auto l = (k + 1) % p.size();
            if (j == k || l == i)
                continue;
            if (intersects(p[i], p[j], p[k], p[l], eps))
                return "Self-intersecting polygon; holes/multipart rings are unsupported.";
        }
    }
    if (std::abs(signedArea(p)) <= eps)
        return "Degenerate polygon.";
    if (signedArea(p) < 0)
        std::reverse(p.begin(), p.end());
    // Collinear boundary points are valid but do not need to become ears.
    bool changed = true;
    while (changed && p.size() > 3) {
        changed = false;
        for (std::size_t i = 0; i < p.size(); ++i) {
            if (std::abs(cross(p[(i + p.size() - 1) % p.size()], p[i], p[(i + 1) % p.size()])) <=
                eps) {
                p.erase(p.begin() + i);
                changed = true;
                break;
            }
        }
    }
    std::vector<int> ids(p.size());
    std::iota(ids.begin(), ids.end(), 0);
    while (ids.size() > 3) {
        if (running && !running())
            return "Preview cancelled.";
        bool found = false;
        for (std::size_t k = 0; k < ids.size(); ++k) {
            const int a = ids[(k + ids.size() - 1) % ids.size()], b = ids[k],
                      c = ids[(k + 1) % ids.size()];
            if (cross(p[a], p[b], p[c]) <= eps)
                continue;
            bool contains = false;
            for (int d : ids)
                if (d != a && d != b && d != c && cross(p[a], p[b], p[d]) >= -eps &&
                    cross(p[b], p[c], p[d]) >= -eps && cross(p[c], p[a], p[d]) >= -eps) {
                    contains = true;
                    break;
                }
            if (contains)
                continue;
            triangles.push_back({p[a], p[b], p[c]});
            ids.erase(ids.begin() + k);
            found = true;
            break;
        }
        if (!found)
            return "Polygon triangulation failed; repair topology.";
    }
    triangles.push_back({p[ids[0]], p[ids[1]], p[ids[2]]});
    return {};
}
/**
 * @brief Clip a polygon against a counterclockwise convex ring and return its area.
 * @param[in] subject Owned working copy of the subject polygon, implicitly closed. Caller data
 * is unchanged; for general concave footprints, pass each triangle produced by triangulate().
 * @param[in] clip Stable counterclockwise convex clipping polygon in the same planar coordinate
 * system, with at least three valid vertices for a meaningful polygon intersection.
 * @return Nonnegative clipped area in squared coordinate units. An empty clip performs no
 * clipping and returns the subject area. Winding, convexity, finiteness and topology are
 * unchecked.
 * @note Uses exact half-plane sign tests without epsilon. Intermediate polygon allocation may
 * throw; no references to either input survive the call.
 */
inline double intersectionArea(Ring subject, const Ring& clip) {
    for (std::size_t i = 0; i < clip.size() && !subject.empty(); ++i) {
        const auto a = clip[i], b = clip[(i + 1) % clip.size()];
        Ring next;
        for (std::size_t k = 0; k < subject.size(); ++k) {
            const auto p = subject[k], q = subject[(k + 1) % subject.size()];
            const auto sp = cross(a, b, p), sq = cross(a, b, q);
            if (sp >= 0)
                next.push_back(p);
            if ((sp >= 0) != (sq >= 0)) {
                const auto t = sp / (sp - sq);
                next.push_back({p.x + t * (q.x - p.x), p.y + t * (q.y - p.y)});
            }
        }
        subject = std::move(next);
    }
    return std::abs(signedArea(subject));
}
}
#endif
