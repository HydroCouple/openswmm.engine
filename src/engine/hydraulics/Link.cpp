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

XSectParams buildXSectParams(const LinkData& links, std::size_t uj) {
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
    return xs;
}

// ============================================================================
// Batch: computeVelocities
// ============================================================================

void computeVelocities(const LinkData& links, double* velocity) {
    int n = links.count();
    for (int i = 0; i < n; ++i) {
        auto ui = static_cast<std::size_t>(i);
        if (links.type[ui] != LinkType::CONDUIT) {
            velocity[i] = 0.0;
            continue;
        }
        if (links.depth[ui] <= 0.01) {
            velocity[i] = 0.0;
            continue;
        }

        // Build per-element XSectParams from SoA
        XSectParams xs;
        xs.type   = links.xsect_batch_shape[ui];
        xs.y_full = links.xsect_y_full[ui];
        xs.a_full = links.xsect_a_full[ui];
        xs.w_max  = links.xsect_w_max[ui];

        int b = links.barrels[ui];
        double q = links.flow[ui] / static_cast<double>(std::max(b, 1));
        double area = xsect::getAofY(xs, links.depth[ui]);

        velocity[i] = (area > constants::FUDGE) ? q / area : 0.0;
    }
}

// ============================================================================
// Batch: computeFroude
// ============================================================================

void computeFroude(const LinkData& links, const double* velocity, double* froude) {
    int n = links.count();
    for (int i = 0; i < n; ++i) {
        auto ui = static_cast<std::size_t>(i);
        if (links.type[ui] != LinkType::CONDUIT) {
            froude[i] = 0.0;
            continue;
        }

        double y = links.depth[ui];
        if (y <= constants::FUDGE) {
            froude[i] = 0.0;
            continue;
        }

        XSectParams xs;
        xs.type   = links.xsect_batch_shape[ui];
        xs.y_full = links.xsect_y_full[ui];
        xs.a_full = links.xsect_a_full[ui];
        xs.w_max  = links.xsect_w_max[ui];

        froude[i] = getFroude(xs, velocity[i], y);
    }
}

// ============================================================================
// Batch: computeAllConveyance
// ============================================================================

void computeAllConveyance(LinkData& links) {
    int n = links.count();
    for (int i = 0; i < n; ++i) {
        auto ui = static_cast<std::size_t>(i);
        if (links.type[ui] != LinkType::CONDUIT) continue;

        computeConveyance(
            links.roughness[ui],
            links.slope[ui],
            links.xsect_s_full[ui],
            links.beta[ui],
            links.rough_factor[ui],
            links.q_full[ui]
        );
    }
}

} // namespace link
} // namespace openswmm
