// SPDX-License-Identifier: Apache-2.0

/**
 * @file      NativeSnow.hpp
 * @brief     Adapter for native SWMM snow accumulation and melt calculations.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include "DegreeDaySnow.hpp"
#include <string>
#include <array>
namespace openswmm::surface {
/** @brief Load native degree-day geometry/history and initialize assigned UEB packs.
 * @param[in,out] context Compatible initialized model/options with valid project unit
 * enum, aligned source arrays and physically qualified snowpack parameter/removal
 * rows. Updates UEB runtime through initializeSubcatchments(), then source snow_depth
 * on success. Does not advance context clocks or gate all native loading on ignore-snow.
 * @param[in,out] solver Resized/reset per-source arrays, then receives global tipm/rnm/
 * ADC curves, converted degree-day parameters, surface fractions, initial SWE/free
 * water and resolved plowing targets. Free water caps at FWF*initial SWE; zero-area
 * surfaces zero water/SI. init retains some global fields such as removed/season/
 * age tracking flags, so caller owns fresh/global initialization as appropriate.
 * @return Empty on accepted UEB initialization, otherwise its diagnostic; preceding
 * solver/context changes remain. No all-model parameter/index validation or rollback;
 * allocation/setup exceptions propagate. Caller sets seasonal melt coefficients
 * before stepping, then applies the correct native/UEB ownership lifecycle. */
std::string initializeNativeSnow(SimulationContext& context, snow::SnowSolver& solver);
/** @brief Detect an enabled authored UEB snowpack assignment without initializing it.
 * @param context Borrowed subcatchment pack assignments and snow model configuration.
 * @return False when snowmelt ignored, otherwise true for any nonnegative assigned
 * pack whose configured modelFor() is UEB. Does not establish valid parameters,
 * resolved weather, allocated runtime columns or nonzero physical pack water. */
bool hasUebSnow(const SimulationContext& context);
/** @brief Assemble SI UEB snow-report fields from accepted native owner snapshots.
 * @param context Matching UEB runtime columns/last flux/sample arrays and parameters.
 * @param state Matching initialized native SnowSoA with valid aligned external,
 * area-fraction and liquid-output arrays; ownership must be stable during reading.
 * @param subcatch Zero-based source index; invalid, degree-day-owned or insufficient
 * runtime columns returns ten NaNs. Other runtime array extents are caller-qualified.
 * @return Ten values in order: basin SWE m (includes point water), represented energy
 * kJ/m2, surface C, bulk C, albedo, cover fraction, melt m/s, liquid net m/s, sublimation
 * m/s and latest accepted sample-end elapsed seconds. Geometry weights each positive
 * subarea; values are not renormalized over snow-covered fraction. Melt and net may
 * include mixed rainfall. Albedo alone is NaN before any positive-duration sample;
 * other valid-owner initial values still report. No state mutation or live prediction
 * is exposed; caller copies results across concurrent owner updates. */
std::array<double, 10> nativeSnowResults(const SimulationContext& context,
                                         const snow::SnowSoA& state, int subcatch);
/** @brief Compute the legacy reported solid-snow depth for a native source.
 * @param state Qualified aligned three-subarea SWE and fractional-area arrays.
 * @param subcatch Valid original source index, unchecked.
 * @return Area-weighted wsnow in feet, excluding retained free water. Does not
 * convert project units, account for full footprint area or mutate histories.
 * Use nativeSnowVolume() for physical water inventory including free water. */
double nativeSnowDepth(const snow::SnowSoA& state, int subcatch);
// Physical inventory includes retained free water; the legacy depth report
// above includes ice only. Areas are actual non-LID square feet.
/** @brief Compute physical solid-plus-free-water inventory over actual source area.
 * @param state Qualified aligned three-subarea wsnow/fw/fArea arrays.
 * @param subcatch Valid original source index, unchecked.
 * @param area_ft2 Finite nonnegative actual non-LID footprint ft2, excluding LIDs.
 * @return Sum((wsnow+fw)*fraction)*area in ft3, without finite/range validation.
 * Does not include external pending runoff/returns or independently debit stores. */
double nativeSnowVolume(const snow::SnowSoA& state, int subcatch, double area_ft2);
}
