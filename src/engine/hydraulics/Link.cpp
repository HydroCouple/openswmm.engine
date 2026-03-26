/**
 * @file Link.cpp
 * @brief Link hydraulics — numerically identical to legacy link.c.
 *
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "Link.hpp"
#include "XSectBatch.hpp"

#include <cmath>
#include <algorithm>

namespace openswmm {
namespace link {

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
        xs.type   = static_cast<int>(links.xsect_shape[ui]);
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
        xs.type   = static_cast<int>(links.xsect_shape[ui]);
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
