/**
 * @file ForceMain.hpp
 * @brief Force main friction — Hazen-Williams and Darcy-Weisbach.
 *
 * @details Alternative friction models for pressurised pipes. Called by
 *          DynamicWave solver when xsect type is FORCE_MAIN.
 *
 *          Both formulas compute friction slope Sf from velocity and
 *          hydraulic radius — vectorisable over all force main links.
 *
 * @note Legacy reference: src/legacy/engine/forcmain.c
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_FORCEMAIN_HPP
#define OPENSWMM_FORCEMAIN_HPP

namespace openswmm {

namespace forcemain {

constexpr double VISCOS = 1.1e-5;  ///< Kinematic viscosity @ 20C (ft2/sec)

enum class FrictionModel : int {
    HAZEN_WILLIAMS = 0,
    DARCY_WEISBACH = 1
};

/**
 * @brief Compute friction slope for Hazen-Williams.
 *
 * @param velocity  Flow velocity (ft/sec).
 * @param hyd_rad   Hydraulic radius (ft).
 * @param c_hw      Hazen-Williams C coefficient.
 * @returns Friction slope Sf (dimensionless).
 */
double getFricSlope_HW(double velocity, double hyd_rad, double c_hw);

/**
 * @brief Compute friction slope for Darcy-Weisbach.
 *
 * @param velocity  Flow velocity (ft/sec).
 * @param hyd_rad   Hydraulic radius (ft).
 * @param roughness Pipe roughness height (ft).
 * @returns Friction slope Sf (dimensionless).
 */
double getFricSlope_DW(double velocity, double hyd_rad, double roughness);

/**
 * @brief Batch compute friction slopes for all force mains — VECTORISABLE.
 *
 * @param velocity   [in]  Velocity array (indexed by force-main group).
 * @param hyd_rad    [in]  Hydraulic radius array.
 * @param param      [in]  C_HW or roughness height, depending on model.
 * @param fric_slope [out] Friction slope array.
 * @param model      Hazen-Williams or Darcy-Weisbach.
 * @param count      Number of force mains.
 */
void batchFricSlope(const double* velocity, const double* hyd_rad,
                    const double* param, double* fric_slope,
                    FrictionModel model, int count);

} // namespace forcemain
} // namespace openswmm

#endif // OPENSWMM_FORCEMAIN_HPP
