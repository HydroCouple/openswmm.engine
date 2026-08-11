/**
 * @file Link.cpp
 * @brief Link hydraulics — numerically identical to legacy link.c.
 *
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "Link.hpp"
#include "XSectBatch.hpp"

#include <cmath>
#include <algorithm>

namespace openswmm {
namespace link {

// Translate LinkData::XsectShape to batch XSectShape (different enum orderings).
int translateShape(XsectShape s) {
    switch (s) {
        case XsectShape::CIRCULAR:        return static_cast<int>(XSectShape::CIRCULAR);
        case XsectShape::FILLED_CIRCULAR: return static_cast<int>(XSectShape::FILLED_CIRCULAR);
        case XsectShape::RECT_CLOSED:     return static_cast<int>(XSectShape::RECT_CLOSED);
        case XsectShape::RECT_OPEN:       return static_cast<int>(XSectShape::RECT_OPEN);
        case XsectShape::TRAPEZOIDAL:     return static_cast<int>(XSectShape::TRAPEZOIDAL);
        case XsectShape::TRIANGULAR:      return static_cast<int>(XSectShape::TRIANGULAR);
        case XsectShape::PARABOLIC:       return static_cast<int>(XSectShape::PARABOLIC);
        case XsectShape::POWER:           return static_cast<int>(XSectShape::POWERFUNC);
        case XsectShape::MODBASKETHANDLE: return static_cast<int>(XSectShape::MOD_BASKET);
        case XsectShape::EGGSHAPED:       return static_cast<int>(XSectShape::EGGSHAPED);
        case XsectShape::HORSESHOE:       return static_cast<int>(XSectShape::HORSESHOE);
        case XsectShape::GOTHIC:          return static_cast<int>(XSectShape::GOTHIC);
        case XsectShape::CATENARY:        return static_cast<int>(XSectShape::CATENARY);
        case XsectShape::SEMIELLIPTICAL:  return static_cast<int>(XSectShape::SEMIELLIPTICAL);
        case XsectShape::BASKETHANDLE:    return static_cast<int>(XSectShape::BASKETHANDLE);
        case XsectShape::SEMICIRCULAR:    return static_cast<int>(XSectShape::SEMICIRCULAR);
        case XsectShape::RECT_TRIANG:     return static_cast<int>(XSectShape::RECT_TRIANG);
        case XsectShape::RECT_ROUND:      return static_cast<int>(XSectShape::RECT_ROUND);
        case XsectShape::HORIZ_ELLIPSE:   return static_cast<int>(XSectShape::HORIZ_ELLIPSE);
        case XsectShape::VERT_ELLIPSE:    return static_cast<int>(XSectShape::VERT_ELLIPSE);
        case XsectShape::ARCH:            return static_cast<int>(XSectShape::ARCH);
        case XsectShape::IRREGULAR:       return static_cast<int>(XSectShape::IRREGULAR);
        case XsectShape::CUSTOM:          return static_cast<int>(XSectShape::CUSTOM);
        case XsectShape::FORCE_MAIN:      return static_cast<int>(XSectShape::FORCE_MAIN);
        case XsectShape::STREET_XSECT:    return static_cast<int>(XSectShape::STREET_XSECT);
        case XsectShape::DUMMY:           return static_cast<int>(XSectShape::DUMMY);
        default:                          return static_cast<int>(XSectShape::DUMMY);
    }
}

// ============================================================================
// Per-element: getVelocity
// ============================================================================

double getVelocity(const XSectParams& xs, double flow, double depth, int barrels) {
    if (depth <= 0.01) return 0.0;

    double q = flow / static_cast<double>(std::max(barrels, 1));
    double area = xsect::getAofY(xs, depth);

    if (area > constants::FUDGE) return q / area;
    return 0.0;
}

// ============================================================================
// Per-element: getFroude
// ============================================================================

double getFroude(const XSectParams& xs, double v, double y) {
    if (y <= constants::FUDGE) return 0.0;

    // For closed conduits: return 0 if full
    if (!xsect::isOpen(xs.type) && (xs.y_full - y) <= constants::FUDGE)
        return 0.0;

    // Hydraulic depth D_h = A / T
    double area = xsect::getAofY(xs, y);
    double width = xsect::getWofY(xs, y);
    if (width <= constants::FUDGE) return 0.0;

    double d_h = area / width;
    if (d_h <= 0.0) return 0.0;

    return std::fabs(v) / std::sqrt(constants::GRAVITY * d_h);
}

// ============================================================================
// Per-element: computeConveyance
// ============================================================================

void computeConveyance(double roughness, double slope, double s_full,
                       double& beta, double& rough_factor, double& q_full) {
    // beta = PHI * sqrt(|slope|) / n
    beta = constants::PHI * std::sqrt(std::fabs(slope)) / roughness;

    // rough_factor = g * (n / PHI)^2
    double n_over_phi = roughness / constants::PHI;
    rough_factor = constants::GRAVITY * n_over_phi * n_over_phi;

    // q_full = section_factor_at_full * beta
    q_full = s_full * beta;
}

// ============================================================================
// Per-element: getDepthFromFlow
// ============================================================================

double getDepthFromFlow(const XSectParams& xs, double beta, double q) {
    if (beta <= 0.0 || q <= 0.0) return 0.0;

    double s = q / beta;               // section factor needed
    double a = xsect::getAofS(xs, s);  // area from section factor
    return xsect::getYofA(xs, a);       // depth from area
}

// ============================================================================
// Per-element: getCapacity
// ============================================================================

double getCapacity(const XSectParams& xs, double depth) {
    if (xs.y_full <= 0.0) return 0.0;
    double area = xsect::getAofY(xs, depth);
    if (xs.a_full <= 0.0) return 0.0;
    return area / xs.a_full;
}

// ============================================================================
// Per-element: getHydPower
// ============================================================================

double getHydPower(double flow, double head_upstream, double head_downstream) {
    // P = gamma * Q * hL
    // gamma = specific weight of water = 62.4 lb/ft³
    // Q = flow (ft³/s), hL = head loss (ft)
    // Result in ft·lb/s (divide by 550 for horsepower)
    constexpr double GAMMA = 62.4;  // lb/ft³
    double hL = head_upstream - head_downstream;
    return GAMMA * std::fabs(flow) * std::fabs(hL);
}

// ============================================================================
// Per-element: buildXSectParams
// ============================================================================

XSectParams buildXSectParams(
    const LinkData& links, std::size_t uj,
    const std::vector<transect::TransectData>* transects) {
    XSectParams xs{};
    xs.type   = links.xsect_batch_shape[uj];
    xs.y_full = links.xsect_y_full[uj];
    xs.a_full = links.xsect_a_full[uj];
    xs.w_max  = links.xsect_w_max[uj];
    xs.r_full = links.xsect_r_full[uj];
    xs.s_full = links.xsect_s_full[uj];
    xs.s_max  = links.xsect_s_max[uj];
    xs.y_bot  = links.xsect_y_bot[uj];
    xs.a_bot  = links.xsect_a_bot[uj];
    xs.s_bot  = links.xsect_s_bot[uj];
    xs.r_bot  = links.xsect_r_bot[uj];

    // Attach the per-link transect tables for tabulated shapes. xsect_curve is
    // the unified index into ctx.transect_tables for IRREGULAR (transect),
    // CUSTOM (shape curve), and STREET_XSECT (street transect) — set by
    // PostParseResolver — mirroring XSectGroups::attachTransectTables for the
    // batch path. Self-contained shapes leave the table pointers null.
    const auto shape = static_cast<XSectShape>(xs.type);
    if (transects &&
        (shape == XSectShape::IRREGULAR ||
         shape == XSectShape::CUSTOM ||
         shape == XSectShape::STREET_XSECT)) {
        const int ti = links.xsect_curve[uj];
        if (ti >= 0 && static_cast<std::size_t>(ti) < transects->size()) {
            const auto& td = (*transects)[static_cast<std::size_t>(ti)];
            xs.transect          = ti;
            xs.area_tbl          = td.area_tbl;
            xs.hrad_tbl          = td.hrad_tbl;
            xs.width_tbl         = td.width_tbl;
            xs.transect_tbl_size = transect::N_TRANSECT_TBL;
        }
    }
    return xs;
}

// ============================================================================
// Per-element: applyTabulatedXSectParams
// ============================================================================

namespace {

// Height at the LOWEST widest point, from the normalized width table
// (xsect.c:688-699 for CUSTOM, xsect.c:1347-1358 for IRREGULAR). Scan up while
// the width is still increasing; the first decrease ends it.
double widestPointHeight(const transect::TransectData& td, double y_full) {
    int i_max = 0;
    double wt_max = td.width_tbl[0];
    for (int i = 1; i < transect::N_TRANSECT_TBL; ++i) {
        if (td.width_tbl[i] < wt_max) break;
        wt_max = td.width_tbl[i];
        i_max = i;
    }
    return y_full * static_cast<double>(i_max) /
           static_cast<double>(transect::N_TRANSECT_TBL - 1);
}

} // namespace

void applyTabulatedXSectParams(XSectParams& xs, const transect::TransectData& td,
                               double y_full, int transect_idx) {
    switch (static_cast<XSectShape>(xs.type)) {
    case XSectShape::CUSTOM:
        // PARITY: legacy xsect_setCustomXsectParams (xsect.c:668-700). The
        // shape curve is built at UNIT height, so the extensive properties
        // scale by y_full here rather than in buildCustomTables.
        xs.y_full = y_full;
        xs.a_full = td.a_full;
        xs.r_full = td.r_full;
        xs.w_max  = td.w_max;
        xs.s_full = xs.a_full * std::pow(xs.r_full, 2.0 / 3.0);  // xsect.c:684 (no clamp)
        xs.s_max  = td.s_max * y_full * y_full * std::pow(y_full, 2.0 / 3.0);  // xsect.c:685-686
        xs.a_bot  = td.a_max * y_full * y_full;
        xs.yw_max = widestPointHeight(td, xs.y_full);
        break;

    case XSectShape::IRREGULAR:
        // PARITY: legacy getTransectParams (xsect.c:1339-1358). The table is
        // built at true scale, so these are absolute.
        xs.y_full = td.y_full;
        xs.a_full = td.a_full;
        xs.r_full = td.r_full;
        xs.w_max  = td.w_max;
        xs.s_full = xs.a_full * std::pow(xs.r_full, 2.0 / 3.0);  // xsect.c:1343
        xs.s_max  = td.s_max;   // xsect.c:1344 (absolute)
        xs.a_bot  = td.a_max;   // xsect.c:1345
        xs.yw_max = widestPointHeight(td, xs.y_full);
        break;

    default:
        // STREET_XSECT. street::buildTransect does not compute a peak section
        // factor, so s_max stays at s_full and a_bot is left alone; yw_max is
        // pinned at full depth rather than scanned. Faithful to the street
        // branch of PostParseResolver, which this consolidates.
        xs.y_full = td.y_full;
        xs.a_full = td.a_full;
        xs.r_full = td.r_full;
        xs.w_max  = td.w_max;
        xs.s_full = xs.a_full * std::pow(xs.r_full, 2.0 / 3.0);
        xs.s_max  = xs.s_full;
        xs.yw_max = xs.y_full;
        break;
    }

    xs.transect          = transect_idx;
    xs.area_tbl          = td.area_tbl;
    xs.hrad_tbl          = td.hrad_tbl;
    xs.width_tbl         = td.width_tbl;
    xs.transect_tbl_size = transect::N_TRANSECT_TBL;
}

// Phase 6: the dead batch helpers computeVelocities/computeFroude/
// computeAllConveyance were removed — they had no callers repo-wide and
// referenced conduit fields now owned by ConduitData (LinkSubtypes.hpp).

} // namespace link
} // namespace openswmm
