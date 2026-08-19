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
 * @file XSectBoundary.cpp
 * @ingroup engine_hydraulics
 */

#include "XSectBoundary.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace openswmm::xsboundary {

namespace {

constexpr double kTwoPi = 2.0 * M_PI;

bool allFinite(const double* a, int n) noexcept {
    for (int i = 0; i < n; ++i) {
        if (!std::isfinite(a[i])) return false;
    }
    return true;
}

/// Angles (within one sweep of @p e, magnitude <= 2*pi) where sin(theta) ==
/// (y-cy)/r, i.e. where the arc's own y(theta) crosses @p y. Ascending order.
int arcCrossingAngles(const BElem& e, double y, double theta_out[2]) noexcept {
    const double s = (y - e.cy) / e.radius;
    if (s < -1.0 || s > 1.0) return 0;

    const double lo = std::min(e.a0, e.a1);
    const double hi = std::max(e.a0, e.a1);
    const double clamped = std::clamp(s, -1.0, 1.0);
    const double thetaA = std::asin(clamped);   // in [-pi/2, pi/2]
    const double thetaB = M_PI - thetaA;         // in [pi/2, 3pi/2]

    int count = 0;
    double found[2] = {0.0, 0.0};
    for (double base : {thetaA, thetaB}) {
        const double kmin = std::ceil((lo - base) / kTwoPi - 1.0e-12);
        const double kmax = std::floor((hi - base) / kTwoPi + 1.0e-12);
        for (double k = kmin; k <= kmax; k += 1.0) {
            const double theta = base + k * kTwoPi;
            if (theta < lo - 1.0e-12 || theta > hi + 1.0e-12) continue;
            bool dup = false;
            for (int j = 0; j < count; ++j) {
                if (std::fabs(found[j] - theta) < 1.0e-9) { dup = true; break; }
            }
            if (!dup && count < 2) found[count++] = theta;
        }
    }
    if (count == 2 && found[0] > found[1]) std::swap(found[0], found[1]);
    theta_out[0] = found[0];
    theta_out[1] = found[1];
    return count;
}

/// True when some angle congruent to @p base (mod 2*pi) lies within the arc's
/// sweep. Shared by elemMinY/elemMaxY and the tangency scan in
/// findCriticalHeights, and using the same k-range convention as
/// arcCrossingAngles so all four agree about what "within the sweep" means.
bool sweepHits(const BElem& e, double base) noexcept {
    const double lo = std::min(e.a0, e.a1);
    const double hi = std::max(e.a0, e.a1);
    const double kmin = std::ceil((lo - base) / kTwoPi - 1.0e-12);
    const double kmax = std::floor((hi - base) / kTwoPi + 1.0e-12);
    return kmin <= kmax;
}

/// True minimum y over the whole element, including an arc's interior low
/// point (theta == -pi/2 mod 2*pi) when that angle falls within its sweep —
/// not just its two endpoints.
double elemMinY(const BElem& e) noexcept {
    if (e.radius == 0.0) return std::min(e.y0, e.y1);
    const double m = std::min(e.y0, e.y1);
    return sweepHits(e, -0.5 * M_PI) ? std::min(m, e.cy - e.radius) : m;
}

/// True maximum y over the whole element — the mirror of elemMinY, counting an
/// arc's interior high point (theta == +pi/2 mod 2*pi) only when the sweep
/// actually reaches it. A pointed arch, whose arcs stop short of their own
/// crowns, must report its apex height rather than cy + r.
double elemMaxY(const BElem& e) noexcept {
    if (e.radius == 0.0) return std::max(e.y0, e.y1);
    const double m = std::max(e.y0, e.y1);
    return sweepHits(e, 0.5 * M_PI) ? std::max(m, e.cy + e.radius) : m;
}

int orientation(double ax, double ay, double bx, double by,
                double cx, double cy) noexcept {
    const double v = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (v > 1.0e-12) return 1;
    if (v < -1.0e-12) return -1;
    return 0;
}

bool onSegment(double ax, double ay, double bx, double by,
               double px, double py) noexcept {
    return std::min(ax, bx) - 1.0e-9 <= px && px <= std::max(ax, bx) + 1.0e-9 &&
           std::min(ay, by) - 1.0e-9 <= py && py <= std::max(ay, by) + 1.0e-9;
}

/// Standard orientation-based segment/segment intersection test (chords only
/// — arcs are approximated by their chords here, as the spec permits for
/// this O(n^2) build-time check).
bool segmentsIntersect(double ax, double ay, double bx, double by,
                       double cx, double cy, double dx, double dy) noexcept {
    const int o1 = orientation(ax, ay, bx, by, cx, cy);
    const int o2 = orientation(ax, ay, bx, by, dx, dy);
    const int o3 = orientation(cx, cy, dx, dy, ax, ay);
    const int o4 = orientation(cx, cy, dx, dy, bx, by);
    if (o1 != o2 && o3 != o4) return true;
    if (o1 == 0 && onSegment(ax, ay, bx, by, cx, cy)) return true;
    if (o2 == 0 && onSegment(ax, ay, bx, by, dx, dy)) return true;
    if (o3 == 0 && onSegment(cx, cy, dx, dy, ax, ay)) return true;
    if (o4 == 0 && onSegment(cx, cy, dx, dy, bx, by)) return true;
    return false;
}

/// Builds the closed n-element chain from points + optional DXF bulges, with
/// no validation and no reorientation — the shared core of fromArcSpec()'s
/// first build and its rebuild-after-reversal.
void buildChain(const double* x, const double* y, const double* bulge, int n,
                std::vector<BElem>& out) {
    out.clear();
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        const double x0 = x[i], y0 = y[i], x1 = x[j], y1 = y[j];
        const double b = bulge ? bulge[i] : 0.0;

        BElem e{};
        e.x0 = x0; e.y0 = y0; e.x1 = x1; e.y1 = y1;

        if (b != 0.0) {
            const double dx = x1 - x0, dy = y1 - y0;
            const double c = std::sqrt(dx * dx + dy * dy);
            const double theta = 4.0 * std::atan(b);
            const double r = c / (2.0 * std::fabs(std::sin(0.5 * theta)));
            const double mx = 0.5 * (x0 + x1), my = 0.5 * (y0 + y1);
            const double nx = -dy / c, ny = dx / c;   // unit normal, left of p0->p1
            const double sagitta = r * std::cos(0.5 * theta);
            const double sgn = (b > 0.0) ? 1.0 : -1.0;
            const double cx = mx + sgn * sagitta * nx;
            const double cy = my + sgn * sagitta * ny;
            const double a0 = std::atan2(y0 - cy, x0 - cx);

            e.cx = cx; e.cy = cy; e.radius = r;
            e.a0 = a0; e.a1 = a0 + theta;
        }
        out.push_back(e);
    }
}

} // namespace

// ===========================================================================
// Green's theorem primitives
// ===========================================================================

double greenArea(const BElem& e) noexcept {
    if (e.radius == 0.0) {
        return 0.5 * (e.x0 * e.y1 - e.x1 * e.y0);
    }
    const double r = e.radius;
    return 0.5 * (r * r * (e.a1 - e.a0) +
                  r * (e.cx * (std::sin(e.a1) - std::sin(e.a0)) -
                       e.cy * (std::cos(e.a1) - std::cos(e.a0))));
}

double greenMomentY(const BElem& e) noexcept {
    if (e.radius == 0.0) {
        return -0.5 * (e.x1 - e.x0) *
               (e.y0 * e.y0 + e.y0 * e.y1 + e.y1 * e.y1) / 3.0;
    }
    const double r = e.radius, cy = e.cy;
    auto F = [cy, r](double t) {
        const double c = std::cos(t);
        return cy * cy * (-c) +
               2.0 * cy * r * (0.5 * t - 0.25 * std::sin(2.0 * t)) +
               r * r * (-c + c * c * c / 3.0);
    };
    return (r / 2.0) * (F(e.a1) - F(e.a0));
}

double arcLength(const BElem& e) noexcept {
    if (e.radius == 0.0) {
        const double dx = e.x1 - e.x0, dy = e.y1 - e.y0;
        return std::sqrt(dx * dx + dy * dy);
    }
    return e.radius * std::fabs(e.a1 - e.a0);
}

// ===========================================================================
// Height queries
// ===========================================================================

int crossingsAt(const BElem& e, double y, double xout[2]) noexcept {
    if (e.radius == 0.0) {
        const double y0 = e.y0, y1 = e.y1;
        if (y0 == y1) return 0;
        // Half-open on the element's OWN endpoint order so a scanline through
        // a shared vertex between two consecutive elements is not double
        // counted.
        const bool crosses = (y0 <= y && y < y1) || (y1 <= y && y < y0);
        if (!crosses) return 0;
        const double t = (y - y0) / (y1 - y0);
        xout[0] = e.x0 + t * (e.x1 - e.x0);
        return 1;
    }

    double theta[2];
    const int n = arcCrossingAngles(e, y, theta);
    for (int i = 0; i < n; ++i) {
        xout[i] = e.cx + e.radius * std::cos(theta[i]);
    }
    return n;
}

int clipBelow(const BElem& e, double y, BElem out[2]) noexcept {
    if (e.radius == 0.0) {
        const bool below0 = e.y0 <= y;
        const bool below1 = e.y1 <= y;
        if (below0 && below1) { out[0] = e; return 1; }
        if (!below0 && !below1) return 0;

        const double t = (y - e.y0) / (e.y1 - e.y0);
        const double xc = e.x0 + t * (e.x1 - e.x0);
        BElem r{};
        if (below0) { r.x0 = e.x0; r.y0 = e.y0; r.x1 = xc;   r.y1 = y; }
        else        { r.x0 = xc;   r.y0 = y;    r.x1 = e.x1; r.y1 = e.y1; }
        out[0] = r;
        return 1;
    }

    double theta[2];
    const int nc = arcCrossingAngles(e, y, theta);
    const double lo = std::min(e.a0, e.a1);
    const double hi = std::max(e.a0, e.a1);
    const double dir = (e.a1 >= e.a0) ? 1.0 : -1.0;

    double bounds[4];
    int nb = 0;
    bounds[nb++] = lo;
    for (int i = 0; i < nc; ++i) bounds[nb++] = theta[i];
    bounds[nb++] = hi;

    int count = 0;
    for (int i = 0; i + 1 < nb && count < 2; ++i) {
        const double s0 = bounds[i], s1 = bounds[i + 1];
        if (s1 - s0 < 1.0e-13) continue;              // degenerate (tangent) sliver
        const double mid = 0.5 * (s0 + s1);
        const double ymid = e.cy + e.radius * std::sin(mid);
        if (ymid > y) continue;                        // this sub-arc is above the cut

        BElem r{};
        r.cx = e.cx; r.cy = e.cy; r.radius = e.radius;
        if (dir > 0.0) { r.a0 = s0; r.a1 = s1; }
        else           { r.a0 = s1; r.a1 = s0; }
        r.x0 = e.cx + e.radius * std::cos(r.a0);
        r.y0 = e.cy + e.radius * std::sin(r.a0);
        r.x1 = e.cx + e.radius * std::cos(r.a1);
        r.y1 = e.cy + e.radius * std::sin(r.a1);
        out[count++] = r;
    }
    return count;
}

// ===========================================================================
// Whole-boundary evaluation
// ===========================================================================

ExactProps evalExact(const BElem* elems, int n, double y) {
    ExactProps out;
    if (n <= 0 || y <= 0.0) return out;

    double area = 0.0, my = 0.0, perim = 0.0;
    std::vector<double> xs;
    xs.reserve(static_cast<std::size_t>(n) * 2);

    BElem clipped[2];
    for (int i = 0; i < n; ++i) {
        const int nc = clipBelow(elems[i], y, clipped);
        for (int k = 0; k < nc; ++k) {
            const BElem& c = clipped[k];
            area += greenArea(c);
            my   += greenMomentY(c);
            // A WALL element lying exactly on the y == level cut (a flat
            // bench precisely at the query depth) is the free surface, not
            // wetted perimeter.
            const bool flatAtSurface = (c.radius == 0.0 && c.y0 == y && c.y1 == y);
            if (!flatAtSurface) perim += arcLength(c);
        }

        double xo[2];
        const int ncr = crossingsAt(elems[i], y, xo);
        for (int k = 0; k < ncr; ++k) xs.push_back(xo[k]);
    }

    std::sort(xs.begin(), xs.end());

    // Close each wet span at the free surface with a synthetic horizontal cap
    // (right crossing -> left crossing, so the whole contour stays CCW) so
    // the Green's-theorem sums are over CLOSED loops — see the @note on
    // evalExact() for why this is required, not optional. The cap's own
    // length is never added to `perim`.
    double width = 0.0;
    for (std::size_t i = 0; i + 1 < xs.size(); i += 2) {
        const double xl = xs[i], xr = xs[i + 1];
        width += xr - xl;
        BElem cap{};
        cap.x0 = xr; cap.y0 = y;
        cap.x1 = xl; cap.y1 = y;
        area += greenArea(cap);
        my   += greenMomentY(cap);
    }

    out.area  = area;
    out.perim = perim;
    out.width = width;
    out.ybar  = (area > 0.0) ? my / area : 0.0;
    out.i1    = area * (y - out.ybar);
    out.ncomp = static_cast<int>(xs.size() / 2);
    return out;
}

// ===========================================================================
// Critical heights
// ===========================================================================

void findCriticalHeights(const BElem* elems, int n,
                         std::vector<CriticalHeight>& out) {
    out.clear();
    if (n <= 0) return;

    double y_full = 0.0;
    for (int i = 0; i < n; ++i) y_full = std::max(y_full, elemMaxY(elems[i]));
    if (!(y_full > 0.0)) return;

    // Clamp rather than discard: a tangency computed as cy - r can land an ulp
    // below 0 (or cy + r an ulp above y_full) after the invert shift, and
    // dropping it would silently lose the 1.5 tag on a round invert or crown —
    // exactly the tag that matters most.
    auto clampY = [y_full](double y) { return std::clamp(y, 0.0, y_full); };

    // Source 1 — element endpoints: corners, tangential joints, and the ends
    // of horizontal segments. Plus both domain ends, unconditionally.
    std::vector<double> endpoints;
    endpoints.reserve(static_cast<std::size_t>(2 * n + 2));
    for (int i = 0; i < n; ++i) {
        endpoints.push_back(clampY(elems[i].y0));
        endpoints.push_back(clampY(elems[i].y1));
    }
    endpoints.push_back(0.0);
    endpoints.push_back(y_full);

    std::vector<CriticalHeight> claims;
    claims.reserve(endpoints.size() + static_cast<std::size_t>(2 * n));
    for (const double y : endpoints) claims.push_back(CriticalHeight{y, 1.0, 1.0});

    // Source 2 — arc horizontal tangencies, the ONLY source of a 1.5 tag. An
    // arc lies ABOVE its own lowest point and BELOW its own highest one, so
    // that is the side the half-power lands on. This holds whether the
    // tangency is interior to the sweep or sits at one of its ends, which is
    // why no interior/endpoint distinction is needed.
    const double snap = 1.0e-9 * y_full;
    for (int i = 0; i < n; ++i) {
        const BElem& e = elems[i];
        if (e.radius == 0.0) continue;
        for (int top = 0; top < 2; ++top) {
            const double base = (top != 0) ? 0.5 * M_PI : -0.5 * M_PI;
            if (!sweepHits(e, base)) continue;

            double y_t = clampY((top != 0) ? (e.cy + e.radius)
                                           : (e.cy - e.radius));
            // Snap onto the NEAREST endpoint within range instead of leaving a
            // sub-1e-9 sliver piece behind it (see the @note on
            // findCriticalHeights). Nearest rather than first-found so the
            // result cannot depend on element ordering.
            const double y_raw = y_t;
            double best = snap;
            for (const double ye : endpoints) {
                const double d = std::fabs(ye - y_raw);
                if (d <= best) { best = d; y_t = ye; }
            }

            CriticalHeight c{y_t, 1.0, 1.0};
            if (top != 0) c.exp_below = 1.5;
            else          c.exp_above = 1.5;
            claims.push_back(c);
        }
    }

    std::sort(claims.begin(), claims.end(),
              [](const CriticalHeight& a, const CriticalHeight& b) {
                  return a.y < b.y;
              });

    // Dedupe, unioning the tags by MAX so a 1.5 claim from any contributing
    // feature survives the merge. This is also what makes three-or-more
    // simultaneous component merges need no special case.
    const double tol = 1.0e-12 * y_full;
    for (const CriticalHeight& c : claims) {
        if (!out.empty() && c.y - out.back().y <= tol) {
            out.back().exp_above = std::max(out.back().exp_above, c.exp_above);
            out.back().exp_below = std::max(out.back().exp_below, c.exp_below);
            continue;
        }
        out.push_back(c);
    }

    // Pin the domain ends exactly — downstream piece construction indexes off
    // these two values, so they must be 0 and y_full to the bit.
    out.front().y = 0.0;
    out.back().y  = y_full;
}

// ===========================================================================
// Construction
// ===========================================================================

int fromPolyline(const double* x, const double* y, int n,
                 std::vector<BElem>& out) {
    return fromArcSpec(x, y, nullptr, n, out);
}

int fromArcSpec(const double* x, const double* y, const double* bulge, int n,
                std::vector<BElem>& out) {
    if (n < 3) return static_cast<int>(BoundaryError::TOO_FEW_POINTS);
    if (!allFinite(x, n) || !allFinite(y, n) ||
        (bulge != nullptr && !allFinite(bulge, n))) {
        return static_cast<int>(BoundaryError::NON_FINITE);
    }
    if (bulge != nullptr) {
        for (int i = 0; i < n; ++i) {
            if (std::fabs(bulge[i]) > 1.0) {
                return static_cast<int>(BoundaryError::BULGE_TOO_LARGE);
            }
        }
    }

    std::vector<BElem> chain;
    buildChain(x, y, bulge, n, chain);

    // Simple-polygon check on the chord chain (orientation-independent, so
    // this need only run once regardless of any reversal below).
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            const bool adjacent = (j == i + 1) || (i == 0 && j == n - 1);
            if (adjacent) continue;
            const BElem& ei = chain[static_cast<std::size_t>(i)];
            const BElem& ej = chain[static_cast<std::size_t>(j)];
            if (segmentsIntersect(ei.x0, ei.y0, ei.x1, ei.y1,
                                  ej.x0, ej.y0, ej.x1, ej.y1)) {
                return static_cast<int>(BoundaryError::SELF_INTERSECTING);
            }
        }
    }

    double area = 0.0;
    for (const BElem& e : chain) area += greenArea(e);
    if (std::fabs(area) < 1.0e-12) {
        return static_cast<int>(BoundaryError::ZERO_AREA);
    }

    if (area < 0.0) {
        // Input traced clockwise: reverse the point order AND negate every
        // bulge (so the same physical arcs are kept, just walked backward),
        // then rebuild from scratch rather than algebraically flipping the
        // already-built elements.
        std::vector<double> rx(static_cast<std::size_t>(n)),
                            ry(static_cast<std::size_t>(n)),
                            rb(static_cast<std::size_t>(n), 0.0);
        for (int k = 0; k < n; ++k) {
            const int src = (n - k) % n;
            rx[static_cast<std::size_t>(k)] = x[src];
            ry[static_cast<std::size_t>(k)] = y[src];
        }
        if (bulge != nullptr) {
            for (int k = 0; k < n; ++k) {
                const int src = (n - 1 - k) % n;
                rb[static_cast<std::size_t>(k)] = -bulge[src];
            }
        }
        buildChain(rx.data(), ry.data(), bulge ? rb.data() : nullptr, n, chain);
    }

    // Shift so the boundary's true minimum y (including any arc's interior
    // low point, not just its endpoints) lands at 0. A pure vertical
    // translation leaves every a0/a1 angle unchanged.
    double y_min = std::numeric_limits<double>::infinity();
    for (const BElem& e : chain) y_min = std::min(y_min, elemMinY(e));
    if (std::isfinite(y_min) && y_min != 0.0) {
        for (BElem& e : chain) {
            e.y0 -= y_min;
            e.y1 -= y_min;
            if (e.radius != 0.0) e.cy -= y_min;
        }
    }

    out = std::move(chain);
    return static_cast<int>(BoundaryError::OK);
}

} // namespace openswmm::xsboundary
