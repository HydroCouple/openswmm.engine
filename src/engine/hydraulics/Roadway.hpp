/**
 * @file Roadway.hpp
 * @brief Roadway weir overflow — FHWA HDS-5.
 *
 * @details Q = Cd * L * hWr^1.5 where Cd depends on head/width ratio
 *          and road surface type (paved/gravel). Submergence correction
 *          factor Kt applied when tailwater is significant.
 *
 * @note Legacy reference: src/legacy/engine/roadway.c
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ROADWAY_HPP
#define OPENSWMM_ROADWAY_HPP

namespace openswmm {

namespace roadway {

enum class SurfaceType : int {
    PAVED  = 0,
    GRAVEL = 1
};

/**
 * @brief Compute roadway weir overflow.
 *
 * @param head_up    Upstream head above road crest (ft).
 * @param head_down  Downstream head above road crest (ft).
 * @param road_width Road width (ft).
 * @param road_length Crest length perpendicular to flow (ft).
 * @param surface    Road surface type.
 * @param[out] dqdh  Derivative dQ/dH.
 * @returns Overflow rate (cfs).
 */
double getFlow(double head_up, double head_down,
               double road_width, double road_length,
               SurfaceType surface, double& dqdh);

/**
 * @brief Get discharge coefficient Cr from head/width ratio.
 *
 * @param hw_ratio  head / road_width.
 * @param surface   Paved or gravel.
 * @returns Discharge coefficient.
 */
double getDischargeCoeff(double hw_ratio, SurfaceType surface);

/**
 * @brief Get submergence correction factor Kt.
 *
 * @param ht_ratio  head_down / head_up.
 * @param surface   Paved or gravel.
 * @returns Submergence factor (0-1).
 */
double getSubmergenceFactor(double ht_ratio, SurfaceType surface);

} // namespace roadway
} // namespace openswmm

#endif // OPENSWMM_ROADWAY_HPP
