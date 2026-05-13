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
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
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
 * @brief Compute equivalent Manning's n for a force main (Gap #22).
 *
 * @details For DW routing with a force main, the Manning's n equivalent that
 *          produces the same full-pipe normal flow as the HW or DW formula.
 *          Matches legacy forcemain_getEquivN() in forcmain.c.
 *
 * @param model     Friction model (HAZEN_WILLIAMS or DARCY_WEISBACH).
 * @param r_bot     Hydraulic radius parameter stored in xsect (rBot in legacy):
 *                  for HW: the C-factor; for DW: roughness height (ft).
 * @param y_full    Full-pipe depth (ft).
 * @param slope     Conduit slope (ft/ft, positive).
 * @param n_raw     Raw Manning's roughness (fallback if model unrecognized).
 * @returns Equivalent Manning's n.
 */
double getEquivN(FrictionModel model, double r_bot, double y_full,
                 double slope, double n_raw);

/**
 * @brief Compute roughness adjustment factor for a force main (Gap #22).
 *
 * @details The returned value is stored in xsect_s_bot and used by
 *          getFricSlope_HW / getFricSlope_DW to compute friction slope.
 *          Matches legacy forcemain_getRoughFactor() in forcmain.c.
 *
 * @param model         Friction model.
 * @param r_bot         rBot parameter (C for HW, roughness height for DW).
 * @param length_factor Factor by which the conduit was artificially lengthened.
 * @returns Roughness factor stored in xsect.sBot.
 */
double getRoughFactor(FrictionModel model, double r_bot, double length_factor);

/**
 * @brief Compute friction factor f (Darcy-Weisbach) for fully-turbulent flow.
 *
 * @details Used internally by getEquivN for DW force mains.
 *          Matches legacy forcemain_getFricFactor() in forcmain.c at Re=1e12.
 *
 * @param r_bot     Roughness height (ft).
 * @param hrad      Hydraulic radius (ft).
 * @param re        Reynolds number (use 1e12 for fully turbulent).
 * @returns Darcy-Weisbach friction factor f.
 */
double getFricFactor(double r_bot, double hrad, double re);

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
