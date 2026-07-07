/**
 * @file Transect.cpp
 * @brief Irregular transect — numerically identical to legacy transect.c.
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "Transect.hpp"
#include <cmath>
#include <algorithm>
#include <numeric>

namespace openswmm {
namespace transect {

static constexpr double PHI = 1.486;

// Faithful 1:1 port of legacy transect.c (transect_validate + createTables +
// getGeometry + getSliceGeom + getFlow). AREA and WIDTH are the same geometric
// integrals as a simple area/perimeter sweep, but the hydraulic radius is NOT
// area/perimeter — legacy back-solves an *effective* hyd-radius from the
// divided-channel composite-roughness conveyance, so that (used with the main
// channel n) it reproduces the total normal flow. A lumped geometric hrad is
// too SMALL on overbank / multi-roughness sections (e.g. user5 T171: R=2.46 vs
// legacy 3.22 at full depth), over-stating friction and collapsing the initial
// flow on q0 transect conduits (extran8a). See src/legacy/engine/transect.c.
void buildTables(TransectData& td) {
    int n_in = static_cast<int>(td.stations.size());
    if (n_in < 2 || static_cast<int>(td.elevations.size()) != n_in) return;

    double nChannel = td.n_channel;
    if (nChannel <= 0.0) return;
    double nLeft  = (td.n_left  > 0.0) ? td.n_left  : nChannel;
    double nRight = (td.n_right > 0.0) ? td.n_right : nChannel;

    double lFactor = (td.length_factor > 0.0) ? td.length_factor : 1.0;
    nChannel *= std::sqrt(lFactor);

    const double xLeftBank  = td.x_left_bank;
    const double xRightBank = td.x_right_bank;

    double ymin = td.elevations[0], ymax = td.elevations[0];
    for (int i = 1; i < n_in; ++i) {
        ymin = std::min(ymin, td.elevations[i]);
        ymax = std::max(ymax, td.elevations[i]);
    }
    td.y_full = ymax - ymin;
    if (td.y_full <= 0.0) return;

    // add vertical end-walls reaching full height (legacy transect_validate)
    int N = n_in;
    std::vector<double> X(static_cast<size_t>(N) + 2);
    std::vector<double> Y(static_cast<size_t>(N) + 2);
    X[0] = td.stations[0];              Y[0] = ymax;
    for (int i = 0; i < N; ++i) {
        X[static_cast<size_t>(i + 1)] = td.stations[static_cast<size_t>(i)];
        Y[static_cast<size_t>(i + 1)] = td.elevations[static_cast<size_t>(i)];
    }
    X[static_cast<size_t>(N + 1)] = td.stations[static_cast<size_t>(N - 1)];
    Y[static_cast<size_t>(N + 1)] = ymax;
    const int Nsta = N + 1;

    auto getFlow = [&](int k, double a, double wp, bool findFlow) -> double {
        if (!findFlow) {
            if (k == Nsta - 1) {
                findFlow = true;
            } else if (X[static_cast<size_t>(k)] == xLeftBank) {
                if (nLeft != nChannel &&
                    X[static_cast<size_t>(k)] != X[static_cast<size_t>(k - 1)])
                    findFlow = true;
            } else if (X[static_cast<size_t>(k)] == xRightBank) {
                if (nRight != nChannel &&
                    X[static_cast<size_t>(k)] != X[static_cast<size_t>(k + 1)])
                    findFlow = true;
            }
        }
        if (findFlow) {
            double n = nChannel;
            if (X[static_cast<size_t>(k - 1)] < xLeftBank)  n = nLeft;
            if (X[static_cast<size_t>(k)]     > xRightBank) n = nRight;
            return PHI / n * a * std::pow(a / wp, 2.0 / 3.0);
        }
        return 0.0;
    };

    const double dy = td.y_full / static_cast<double>(N_TRANSECT_TBL - 1);

    td.area_tbl[0] = 0.0;
    td.hrad_tbl[0] = 0.0;
    td.width_tbl[0] = 0.0;

    // PARITY: legacy createTables (transect.c:281-289) ACCUMULATES the depth
    // `y = ymin; y += dy` each row rather than recomputing `ymin + idx*dy`.
    // The accumulated rounding path makes ALL transect tables — including
    // a_full/r_full/w_max — bit-identical to legacy. (This is one of the three
    // coupled pieces of the IRREGULAR parity fix; the other two are the
    // physical s_max/a_max computed below and the getTransectParams propagation
    // in PostParseResolver + the getAofS getAmax IRREGULAR path.)
    double y = ymin;
    for (int idx = 1; idx < N_TRANSECT_TBL; ++idx) {
        y += dy;
        double wpSum = 0.0, aSum = 0.0, qSum = 0.0;
        double areaT = 0.0, widthT = 0.0;

        for (int k = 1; k <= Nsta; ++k) {
            const double ek  = Y[static_cast<size_t>(k)];
            const double ekm = Y[static_cast<size_t>(k - 1)];
            const double yhi = std::max(ekm, ek);
            const double ylo = std::min(ekm, ek);
            if (ylo >= y) continue;

            const double width = std::fabs(X[static_cast<size_t>(k)] -
                                           X[static_cast<size_t>(k - 1)]);
            double w  = width;
            double wp = std::sqrt(width * width + (yhi - ylo) * (yhi - ylo));
            double a  = 0.0;
            if (y > yhi) {
                a = width * ((y - yhi) + (y - ylo)) / 2.0;
            } else if (yhi > ylo) {
                const double ratio = (y - ylo) / (yhi - ylo);
                a   = width * (yhi - ylo) / 2.0 * ratio * ratio;
                w  *= ratio;
                wp *= ratio;
            }

            wpSum  += wp;
            aSum   += a;
            areaT  += a;
            widthT += w;

            const bool findFlow = (ek >= y);
            const double q = getFlow(k, aSum, wpSum, findFlow);
            if (q > 0.0) { qSum += q; aSum = 0.0; wpSum = 0.0; }
        }

        td.area_tbl[idx]  = areaT;
        td.width_tbl[idx] = widthT;
        if (areaT == 0.0)
            td.hrad_tbl[idx] = td.hrad_tbl[idx - 1];
        else
            td.hrad_tbl[idx] = std::pow(qSum * nChannel / PHI / areaT, 1.5);
    }

    // PARITY: legacy setMaxSectionFactor (transect.c:596-618) — the physical
    // maximum section factor and the area at which it occurs, computed on the
    // UNNORMALIZED tables BEFORE the normalization below. Stored ABSOLUTE (real
    // ft units), unlike the unit-height CUSTOM s_max/a_max (buildCustomTables)
    // that the caller scales by yFull. The IRREGULAR propagation in
    // PostParseResolver uses these directly for xsect sMax / aBot. For channels
    // whose section factor peaks below full depth (vertical end-walls raise the
    // wetted perimeter faster than the area near the top) aMax < aFull and
    // sMax > sFull; getAofS's Newton bracket then needs aMax, not aFull.
    td.s_max = 0.0;
    td.a_max = 0.0;
    for (int i = 1; i < N_TRANSECT_TBL; ++i) {
        double sf = td.area_tbl[i] * std::pow(td.hrad_tbl[i], 2.0 / 3.0);
        if (sf > td.s_max) {
            td.s_max = sf;
            td.a_max = td.area_tbl[i];
        }
    }

    const int nLast = N_TRANSECT_TBL - 1;
    td.a_full = td.area_tbl[nLast];
    td.r_full = td.hrad_tbl[nLast];
    td.w_max  = td.width_tbl[nLast];

    for (int i = 1; i <= nLast; ++i) {
        if (td.a_full > 0.0) td.area_tbl[i]  /= td.a_full;
        if (td.r_full > 0.0) td.hrad_tbl[i]  /= td.r_full;
        if (td.w_max  > 0.0) td.width_tbl[i] /= td.w_max;
    }
    // width at zero depth = width at first increment (legacy createTables:309)
    td.width_tbl[0] = td.width_tbl[1];
}

// ============================================================================
// PARITY helpers — op-for-op transliterations of legacy src/legacy/engine/
// shape.c. Every expression keeps legacy grouping and operand order so the
// 51-entry CUSTOM geometry tables are bit-identical to legacy TShape tables.
// ============================================================================
namespace {

// shape.c:307-324 getWidth. NOTE grouping: (y-y1)/(y2-y1) evaluated FIRST,
// then multiplied by (w2-w1) — not (w2-w1)*(y-y1)/(y2-y1).
inline double shapeGetWidth(double y, double y1, double y2,
                            double w1, double w2) {
    if (y2 == y1) return w2;
    return w1 + (y - y1) / (y2 - y1) * (w2 - w1);
}

// shape.c:328-349 getArea — trapezoid as (wMin + (wMax-wMin)/2)*(y-y1),
// NOT 0.5*(w1+w)*(y-y1) (same value mathematically, different rounding).
inline double shapeGetArea(double y, double w, double y1, double w1) {
    double wMin, wMax;
    if (w > w1) { wMin = w1; wMax = w; }
    else        { wMin = w;  wMax = w1; }
    return (wMin + (wMax - wMin) / 2.0) * (y - y1);
}

// shape.c:353-369 getPerim
inline double shapeGetPerim(double y, double w, double y1, double w1) {
    double dy = y - y1;
    double dw = std::fabs(w - w1) / 2.0;
    return 2.0 * std::sqrt(dy * dy + dw * dw);
}

// shape.c:246-303 getNextInterval. Atotal/Ptotal are the legacy file-scope
// statics passed by reference; the cursor ci replaces table_getNextEntry.
// yLast/wLast are by value, exactly like legacy (caller re-syncs from y1/w1).
inline bool shapeGetNextInterval(const double* curve_x, const double* curve_y,
                                 int n_pts, int& ci, double y,
                                 double yLast, double wLast,
                                 double& y1, double& y2,
                                 double& w1, double& w2, double& wMax,
                                 double& Atotal, double& Ptotal) {
    while (y > y2) {
        // shape.c:272-277 — book the sub-interval up to the curve boundary
        // using the CURVE's endpoint width w2 (no interpolation).
        if (y2 > yLast) {
            Atotal += shapeGetArea(y2, w2, yLast, wLast);
            Ptotal += shapeGetPerim(y2, w2, yLast, wLast);
            yLast = y2;
            wLast = w2;
        }
        // shape.c:280-285 — advance to the next curve table interval
        y1 = y2;
        w1 = w2;
        if (ci >= n_pts) {          // table_getNextEntry failed
            y2 = 1.0;               // shape.c:284
            return true;
        }
        y2 = curve_x[ci];
        w2 = curve_y[ci];
        ++ci;
        if (w2 > wMax) wMax = w2;                // shape.c:288-290
        if (y2 < y1 || w2 < 0.0) return false;   // shape.c:293-295
        if (y2 > 1.0) y2 = 1.0;                  // shape.c:297-299
    }
    return true;
}

} // namespace

void buildCustomTables(TransectData& td, double y_full,
                       const double* curve_x, const double* curve_y, int n_pts) {
    if (n_pts < 1 || y_full <= 0.0) return;

    // ================================================================
    // PARITY: op-for-op transliteration of legacy shape.c shape_validate
    //   = computeShapeTables (shape.c:64) + getSmax (shape.c:189)
    //   + normalizeShapeTables (shape.c:216),
    // then scaled to physical units exactly as xsect_setCustomXsectParams
    // does (xsect.c:681-686). The shape curve is width v. height for a
    // section of UNIT height; tables are built in that space.
    // ================================================================

    td.y_full = y_full;

    // --- get first entry of user's shape curve (shape.c:81-89)
    double y1 = curve_x[0], w1 = curve_y[0];
    if (y1 < 0.0 || y1 >= 1.0 || w1 < 0.0) return;
    double wMax = w1;

    double y2, w2;
    int ci = 1;   // curve cursor == legacy table iterator position

    // --- if first entry not at zero ht. then add an initial entry (shape.c:92-97)
    if (y1 != 0.0) {
        y2 = y1; w2 = w1;
        y1 = 0.0; w1 = 0.0;
    }
    // --- otherwise get next entry in the user's shape curve (shape.c:99-115)
    else {
        if (ci >= n_pts) return;
        y2 = curve_x[ci]; w2 = curve_y[ci]; ++ci;
        if (y2 < y1 || w2 < 0.0) return;
        if (y2 > 1.0) y2 = 1.0;
        if (w2 > wMax) wMax = w2;
    }

    // --- number of entries & interval size in geom. tables (shape.c:118-120)
    const int n = N_TRANSECT_TBL - 1;   // == legacy N_SHAPE_TBL - 1
    const double dy = 1.0 / static_cast<double>(n);

    // --- initialize geometry tables (shape.c:123-127)
    td.area_tbl[0]  = 0.0;
    td.hrad_tbl[0]  = 0.0;
    td.width_tbl[0] = w1;
    double Ptotal = w1;
    double Atotal = 0.0;

    // --- fill in rest of geometry tables (shape.c:130-174)
    double y = 0.0;
    double w = w1;
    for (int i = 1; i <= n; ++i) {
        double yLast = y;
        double wLast = w;
        y = y + dy;

        // do not allow height to exceed 1.0 (shape.c:140-142, TINY = 1e-6)
        if (std::fabs(y - 1.0) < 1.0e-6) y = 1.0;

        // if height exceeds current curve interval, move to next one
        // (shape.c:146-154 — an if, not a loop; getNextInterval loops inside)
        if (y > y2) {
            if (!shapeGetNextInterval(curve_x, curve_y, n_pts, ci, y,
                                      yLast, wLast, y1, y2, w1, w2, wMax,
                                      Atotal, Ptotal))
                return;
            yLast = y1;
            wLast = w1;
        }

        // top width, area & perimeter of current interval (shape.c:157-159)
        w = shapeGetWidth(y, y1, y2, w1, w2);
        Atotal += shapeGetArea(y, w, yLast, wLast);
        Ptotal += shapeGetPerim(y, w, yLast, wLast);

        // add top width to total perimeter if at top of shape
        // (shape.c:162-164 — legacy adds w2, NOT the interpolated w)
        if (y == 1.0) Ptotal += w2;

        // update table values (shape.c:167-173)
        td.width_tbl[i] = w;
        td.area_tbl[i]  = Atotal;
        if (Ptotal > 0.0) td.hrad_tbl[i] = Atotal / Ptotal;
        else              td.hrad_tbl[i] = 0.0;
    }

    // --- unit-height full-depth properties (shape.c:177-178)
    double aFull = td.area_tbl[n];
    double rFull = td.hrad_tbl[n];

    // --- max. section factor & its area (getSmax, shape.c:189-212), computed
    //     on the UNNORMALIZED tables before normalization, like legacy.
    double sMax = 0.0, aMax = 0.0;
    for (int i = 1; i <= n; ++i) {
        double sf = td.area_tbl[i] * std::pow(td.hrad_tbl[i], 2.0 / 3.0);
        if (sf > sMax) { sMax = sf; aMax = td.area_tbl[i]; }
    }
    td.s_max = sMax;   // unit-height values; caller scales per xsect.c:685-686
    td.a_max = aMax;

    // --- normalize tables (normalizeShapeTables, shape.c:229-239)
    if (aFull == 0.0 || rFull == 0.0 || wMax == 0.0) return;
    for (int i = 0; i <= n; ++i) {
        td.area_tbl[i]  /= aFull;
        td.hrad_tbl[i]  /= rFull;
        td.width_tbl[i] /= wMax;
    }

    // --- scale to physical units (xsect_setCustomXsectParams, xsect.c:681-683)
    td.w_max  = wMax * y_full;
    td.a_full = aFull * y_full * y_full;
    td.r_full = rFull * y_full;
}

} // namespace transect
} // namespace openswmm
