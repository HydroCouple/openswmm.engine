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

#include "LegacyShapeBoundary.hpp"
#include "XSectKernels.hpp"

#include <cmath>

namespace openswmm::xsboundary {

namespace {

constexpr double kPi = 3.14159265358979323846;

/// Four quarter-arc points, each bulging by tan(pi/8) (a 90-degree sweep) —
/// a TRUE circle, replacing the 51-point A_Circ/W_Circ/R_Circ tables.
/// Diameter = xs.y_full (already the resolved full depth for both CIRCULAR
/// and FORCE_MAIN).
bool buildCircle(double y_full, std::vector<BElem>& out) {
    if (y_full <= 0.0) return false;
    // fromArcSpec requires n >= 3, so a circle needs at least 3 points — two
    // antipodal points with bulge=1 each (a valid *closed loop* in principle)
    // is rejected outright. Four quarter-arc points, each bulging by
    // tan(pi/8) (a 90-degree sweep), is the same construction
    // test_xsect_boundary.cpp's FourQuarterArcBulgesFormAFullCircle already
    // proves traces an exact circle CCW.
    const double r = 0.5 * y_full;
    const double b = std::tan(0.25 * kPi / 2.0);   // tan(theta/4), theta = pi/2
    const double x[4] = {r, 0.0, -r, 0.0};
    const double y[4] = {0.0, r, 0.0, -r};
    const double bulge[4] = {b, b, b, b};
    return fromArcSpec(x, y, bulge, 4, out) == 0;
}

/// Ceiling on how many of a table's OWN points feed the polyline. Every
/// vertex in a straight-line-only polyline is a generic corner (Puiseux
/// exponent 1.0 — see XSectBoundary.hpp), so it is its own critical height;
/// a table with N points mirrored into a symmetric polygon needs N-1 pieces
/// just for the base split, with NO headroom left for compile()'s adaptive
/// subdivision. `kMaxPieces` (ChebSection.hpp) is 24, sized — per its own
/// documentation — for "a handful of arcs", not dense tabulated point
/// clouds; EGG/HORSESHOE/BASKETHANDLE/ARCH/HORIZ_ELLIPSE/VERT_ELLIPSE all
/// carry 26 raw table points, which need 25 pieces and blow the budget by
/// exactly one (compile() error 5), while GOTHIC/CATENARY/SEMIELLIPTICAL/
/// SEMICIRCULAR's 21-point tables just barely fit with zero headroom.
/// Rather than raise a shared, carefully-sized constant for six shapes,
/// subsample here: always keep the invert and crown (index 0 and n-1)
/// EXACTLY, since those are where the boundary's own singular/critical
/// behaviour lives and the table's own value there matters most, and
/// evenly thin the interior down to kMaxTablePoints. This still uses only
/// the table's OWN data (no invented geometry) and the smooth Chebyshev fit
/// between coarser samples remains far better than legacy's own LINEAR
/// interpolation between the denser ones, especially at the endpoints.
constexpr int kMaxTablePoints = 13;

/// Symmetric polyline from a shape's own normalized width table (thinned to
/// at most kMaxTablePoints per the note above): right side ascending
/// (invert to crown), left side descending (crown to invert).
///
/// @note A zero-width table endpoint (the usual case at the invert and/or
///       crown of a closed ovoid shape — e.g. EGG's W_Egg[0] and W_Egg[last]
///       are both 0) makes the right side's terminal point and the left
///       side's starting point COINCIDE exactly, producing a zero-length
///       segment. fromPolyline's pairwise segment check flags that as
///       self-intersecting (two segments sharing an endpoint with zero
///       separation) rather than the harmless degenerate vertex it actually
///       is — caught by test_legacy_shape_boundary.cpp when every one of the
///       10 width-table shapes failed to build. Fixed by simply not emitting
///       a point that coincides with the one already at the end of the
///       list (checked here rather than in fromPolyline, since a coincident
///       *interior* vertex is a real, meaningful degenerate case elsewhere
///       in that function and not something to special-case there). The
///       same check runs once more after the loop for the closing edge
///       (last point vs. first — fromPolyline closes the loop implicitly).
bool buildFromWidthTable(const double* w_table, int n, double y_full,
                         double w_max, std::vector<BElem>& out) {
    if (n < 2 || y_full <= 0.0 || w_max <= 0.0) return false;

    // Thinned index list: index 0 and n-1 exact, interior evenly spaced.
    const int m = (n <= kMaxTablePoints) ? n : kMaxTablePoints;
    std::vector<int> idx(static_cast<std::size_t>(m));
    for (int k = 0; k < m; ++k) {
        idx[static_cast<std::size_t>(k)] = (m == 1) ? 0 :
            static_cast<int>(std::lround(static_cast<double>(k) *
                                         static_cast<double>(n - 1) /
                                         static_cast<double>(m - 1)));
    }

    std::vector<double> px, py;
    px.reserve(static_cast<std::size_t>(2 * m));
    py.reserve(static_cast<std::size_t>(2 * m));

    constexpr double kCoincidentTol = 1.0e-12;
    auto appendIfDistinct = [&](double x, double y) {
        if (!px.empty()) {
            const double dx = x - px.back();
            const double dy = y - py.back();
            if (dx * dx + dy * dy < kCoincidentTol) return;
        }
        px.push_back(x);
        py.push_back(y);
    };

    const double inv_n1 = 1.0 / static_cast<double>(n - 1);
    for (int k = 0; k < m; ++k) {
        const int i = idx[static_cast<std::size_t>(k)];
        const double y_norm = static_cast<double>(i) * inv_n1;
        double half_w = 0.5 * w_table[i] * w_max;
        if (half_w < 0.0) half_w = 0.0;
        appendIfDistinct(half_w, y_norm * y_full);
    }
    for (int k = m - 1; k >= 0; --k) {
        const int i = idx[static_cast<std::size_t>(k)];
        const double y_norm = static_cast<double>(i) * inv_n1;
        double half_w = 0.5 * w_table[i] * w_max;
        if (half_w < 0.0) half_w = 0.0;
        appendIfDistinct(-half_w, y_norm * y_full);
    }
    // Closing edge: drop the last point if it coincides with the first —
    // fromPolyline connects them implicitly either way.
    if (px.size() >= 2) {
        const double dx = px.back() - px.front();
        const double dy = py.back() - py.front();
        if (dx * dx + dy * dy < kCoincidentTol) {
            px.pop_back();
            py.pop_back();
        }
    }

    return fromPolyline(px.data(), py.data(), static_cast<int>(px.size()), out) == 0;
}

} // namespace

bool buildLegacyBoundary(XSectShape shape, const XSectParams& xs,
                         std::vector<BElem>& out) {
    const xsect::XsectTables& tbl = xsect::hostTables();

    switch (shape) {
        case XSectShape::CIRCULAR:
        case XSectShape::FORCE_MAIN:
            return buildCircle(xs.y_full, out);

        case XSectShape::EGGSHAPED:
            return buildFromWidthTable(tbl.W_Egg, tbl.N_W_Egg, xs.y_full, xs.w_max, out);
        case XSectShape::HORSESHOE:
            return buildFromWidthTable(tbl.W_Horseshoe, tbl.N_W_Horseshoe, xs.y_full, xs.w_max, out);
        case XSectShape::GOTHIC:
            return buildFromWidthTable(tbl.W_Gothic, tbl.N_W_Gothic, xs.y_full, xs.w_max, out);
        case XSectShape::CATENARY:
            return buildFromWidthTable(tbl.W_Catenary, tbl.N_W_Catenary, xs.y_full, xs.w_max, out);
        case XSectShape::SEMIELLIPTICAL:
            return buildFromWidthTable(tbl.W_SemiEllip, tbl.N_W_SemiEllip, xs.y_full, xs.w_max, out);
        case XSectShape::BASKETHANDLE:
            return buildFromWidthTable(tbl.W_BasketHandle, tbl.N_W_BasketHandle, xs.y_full, xs.w_max, out);
        case XSectShape::SEMICIRCULAR:
            return buildFromWidthTable(tbl.W_SemiCirc, tbl.N_W_SemiCirc, xs.y_full, xs.w_max, out);
        case XSectShape::ARCH:
            return buildFromWidthTable(tbl.W_Arch, tbl.N_W_Arch, xs.y_full, xs.w_max, out);
        case XSectShape::HORIZ_ELLIPSE:
            return buildFromWidthTable(tbl.W_HorizEllipse, tbl.N_W_HorizEllipse, xs.y_full, xs.w_max, out);
        case XSectShape::VERT_ELLIPSE:
            return buildFromWidthTable(tbl.W_VertEllipse, tbl.N_W_VertEllipse, xs.y_full, xs.w_max, out);

        default:
            return false;
    }
}

} // namespace openswmm::xsboundary
