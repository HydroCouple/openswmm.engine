// SPDX-License-Identifier: Apache-2.0

/**
 * @file      UebReference.hpp
 * @brief     C++ adapter and translated Utah Energy Balance reference snow routines.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
namespace openswmm::surface::ueb_reference {
/** @brief Run one equilibrium-temperature UEB reference interval with padded arrays.
 * @param dt Positive duration hours, already bounded/qualified by the point adapter.
 * @param input Borrowed meteorology buffer pointing at logical input[8]: seven values
 * (Celsius air temperature, precipitation m/h, wind m/s, humidity fraction, shortwave
 * and longwave kJ/(m2 h), cosine). Eight allocated leading padding doubles are required.
 * @param site Borrowed five values starting after one allocated padding double:
 * forest fraction, effective snow drift/catch, pressure Pa, ground heat kJ/(m2 h),
 * optical extinction depth m. Caller guarantees finite physical ranges.
 * @param[in,out] state Five native values after one allocated padding double:
 * energy kJ/m2, SWE m, dimensionless optical age, current/total refreezing depths m.
 * @param parameters Borrowed formulation-specific parameter table after one allocated
 * padding double; layout must match the reference method and point adapter.
 * @param[out] output Writable diagnostic/flux table after one allocated padding double.
 * Slots 1..13: rain/snow m/h, albedo, sensible/latent kJ/(m2 h), signed sublimation m/h,
 * liquid m/h, outgoing melt/net energy kJ/(m2 h), net water m/h, bulk/surface Celsius,
 * net radiation kJ/(m2 h). Fluxes are rates, not integrated interval amounts.
 * @param dust Dimensionless optical-age impurity contribution, finite nonnegative.
 * @param fresh New-snow reset depth m; see formulation-specific behavior.
 * @param cv Visible-band fractional optical-age response in [0,1].
 * @param cir Near-infrared fractional optical-age response in [0,1].
 * @param fraction -1 uses temperature rain/snow partition; [0,1] prescribes snow share.
 * @return Number of intercepted reference diagnostic records, zero for no diagnostic.
 * No validation, ownership, retry or transaction layer: valid nonnull disjoint arrays,
 * positive physical coefficients and correctly padded extents are caller requirements.
 * Updates state/output and thread-local scratch even when diagnostics report trouble;
 * negative-melt STOP throws. Concurrent threads have independent scratch, but calls
 * must not reenter on one thread or share writable arrays. Public ueb::advance owns
 * finite/conservation checks and isolated candidate publication.
 * Parameters have 30 entries, in the exact order built in ueb::step; output needs
 * 13 entries and state at least five. Equilibrium updates the first three state
 * entries; refreezing-depth slots are retained. Optical ageing uses the historical
 * 0.01 m reset scale: fresh is stored by the adapter but unused in this formulation.
 * Radiation is supplied separately, albedo evolves, and reference print output is off.
 * @par Equilibrium parameter table (one-based slots)
 * 1 rain threshold C; 2 snow threshold C; 3 freezing reference C; 4 Kelvin offset;
 * 5 emissivity; 6 Stefan-Boltzmann kJ/(m2 h K4); 7 fusion kJ/kg; 8 sublimation
 * kJ/kg; 9 water heat capacity kJ/(kg K); 10 ice heat capacity; 11 soil heat
 * capacity; 12 air heat capacity; 13 dry-air gas constant J/(kg K); 14 von Karman
 * constant; 15 measurement height m; 16 roughness m; 17 seconds/hour conversion;
 * 18 ice density kg/m3; 19 water density; 20 snow density; 21 soil density;
 * 22 liquid retention ratio; 23 drainage conductivity m/h; 24 active soil depth m;
 * 25 bulk/surface conductance m/h; 26 gravity m/s2; 27 ground albedo;
 * 28 fresh visible albedo; 29 fresh near-infrared albedo; 30 stability blend.
 */
int equilibriumStep(double dt, double* input, double* site, double* state, double* parameters,
                    double* output, double dust, double fresh, double cv = .2, double cir = .5,
                    double fraction = -1);
/** @brief Run one modified-force-restore UEB reference interval with padded arrays.
 * @param dt Positive duration hours, already bounded/qualified by the point adapter.
 * @param input Borrowed meteorology buffer pointing at logical input[8]: seven values
 * (Celsius air temperature, precipitation m/h, wind m/s, humidity fraction, shortwave
 * and longwave kJ/(m2 h), cosine). Eight allocated leading padding doubles are required.
 * @param site Borrowed five values starting after one allocated padding double:
 * forest fraction, effective snow drift/catch, pressure Pa, ground heat kJ/(m2 h),
 * optical extinction depth m. Caller guarantees finite physical ranges.
 * @param[in,out] state Five native values after one allocated padding double:
 * energy kJ/m2, SWE m, dimensionless optical age, current/total refreezing depths m.
 * @param parameters Borrowed formulation-specific parameter table after one allocated
 * padding double; layout must match the reference method and point adapter.
 * @param[out] output Writable diagnostic/flux table after one allocated padding double.
 * Slots 1..13: rain/snow m/h, albedo, sensible/latent kJ/(m2 h), signed sublimation m/h,
 * liquid m/h, outgoing melt/net energy kJ/(m2 h), net water m/h, bulk/surface Celsius,
 * net radiation kJ/(m2 h). Fluxes are rates, not integrated interval amounts.
 * @param dust Dimensionless optical-age impurity contribution, finite nonnegative.
 * @param cv Visible-band fractional optical-age response in [0,1].
 * @param cir Near-infrared fractional optical-age response in [0,1].
 * @param fraction -1 uses temperature rain/snow partition; [0,1] prescribes snow share.
 * @return Number of intercepted reference diagnostic records, zero for no diagnostic.
 * No validation, ownership, retry or transaction layer: valid nonnull disjoint arrays,
 * positive physical coefficients and correctly padded extents are caller requirements.
 * Updates state/output and thread-local scratch even when diagnostics report trouble;
 * negative-melt STOP throws. Concurrent threads have independent scratch, but calls
 * must not reenter on one thread or share writable arrays. Public ueb::advance owns
 * finite/conservation checks and isolated candidate publication.
 * @param[in,out] temperatures Chronological surface Celsius history after one
 * allocated padding double; count entries, shifted and appended by the reference.
 * @param[in,out] averages Same-sized/padded bulk Celsius history, similarly updated.
 * @param count Positive history entry count; both arrays must provide count values.
 * @param surface_mean Duration-weighted preceding 24-hour surface Celsius supplied
 * by the point owner, not recomputed from potentially irregular history samples.
 * @param bulk_mean Corresponding duration-weighted bulk Celsius.
 * Parameters have 22 entries in the exact ueb::step order; output needs 14 entries,
 * with slot 14 surface melt m/h. Uses method flag four, separate incoming radiation
 * and evolving albedo. Parameter slot 21 controls optical age reset (zero resets
 * on any snowfall).
 * All five inventory entries and both history buffers can change. Caller supplies
 * private candidates; these wrappers do not interpret diagnostic return as failure.
 * @par Modified-force-restore parameter table (one-based slots)
 * 1 rain threshold C; 2 snow threshold C; 3 emissivity; 4 soil heat capacity
 * kJ/(kg K); 5 measurement height m; 6 roughness m; 7 snow density kg/m3;
 * 8 soil density; 9 liquid retention ratio; 10 drainage conductivity m/h;
 * 11 active soil depth m; 12 ground albedo; 13 fresh visible albedo;
 * 14 fresh near-infrared albedo; 15 snow conductivity kJ/(m K h);
 * 16 soil conductivity; 17 low-frequency angular frequency rad/h;
 * 18 dimensionless penetration multiplier; 19 stability blend;
 * 20 legacy reference temperature; 21 new-snow optical-age reset depth m;
 * 22 surface-melt outflow fraction. The point adapter supplies slots 20/22 as zero.
 */
int forceRestoreStep(double dt, double* input, double* site, double* state, double* temperatures,
                     double* averages, int count, double* parameters, double* output, double dust,
                     double surface_mean, double bulk_mean, double cv = .2, double cir = .5,
                     double fraction = -1);
}
