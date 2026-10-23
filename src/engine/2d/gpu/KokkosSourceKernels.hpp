// SPDX-License-Identifier: Apache-2.0

/**
 * @file      KokkosSourceKernels.hpp
 * @brief     Kokkos kernels for explicit surface water source preparation.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include <Kokkos_Core.hpp>

namespace openswmm::twoD::gpu {
// Water-only counterpart of ExplicitInertialSolver::prepareCellSources.
// Input volume has already received face transfers; depth is the pre-source
// depth, as on the CPU. All returned transfers are volumes, not rates.
struct SourceBudget {
    double volume, infiltration, evaporation, coupling;
    bool applied;
};
/**
 * @brief Apply the cubic wet/dry ramp to a held nonnegative sink demand.
 * @param[in] rate Finite sink rate m/s; nonpositive returns zero.
 * @param[in] h Finite pre-source cell depth m; nonpositive returns zero.
 * @param[in] dry Finite positive dry-depth threshold m; at/above it the full rate applies.
 * @return Effective sink rate m/s, using rate*t*t*(3-2*t) below the threshold. This computes no
 * volume budget by itself; no general finite/threshold validation occurs. Pure host/device math.
 */
KOKKOS_INLINE_FUNCTION double sourceSink(double rate, double h, double dry) {
    if (rate <= 0.0 || h <= 0.0)
        return 0.0;
    if (h >= dry)
        return rate;
    const double t = h / dry;
    return rate * t * t * (3.0 - 2.0 * t);
}
/**
 * @brief Compute a donor-limited water-only cell source update without mutating device arrays.
 * @param[in] volume Finite cell water m3 after face transfers; positive part is used with
 * incoming sources, except the no-source fast path returns volume unchanged.
 * @param[in] h Finite pre-source depth m used for evaporation/infiltration wet/dry ramps.
 * @param[in] area Finite positive cell area m2.
 * @param[in] dt Finite nonnegative source interval s.
 * @param[in] dry Finite positive dry-depth ramp threshold m.
 * @param[in] rain_rate Finite rainfall m/s; negative rainfall contributes zero.
 * @param[in] coupling_rate Finite signed coupling m/s, positive into the surface.
 * @param[in] infil_rate Finite held infiltration demand m/s, negative treated as zero.
 * @param[in] evap_rate Finite held evaporation demand m/s, negative treated as zero.
 * @return SourceBudget by value: final water m3, nonnegative actual infiltration/evaporation m3,
 * signed actual coupling m3 and applied flag. If every integrated transfer is zero,
 * applied=false; otherwise sinks and outgoing coupling share one availability scale against
 * volume plus inflows.
 * @note No general shape/finite validation, allocation, ownership transfer or state booking
 * occurs. Caller writes returned budgets/receipts once; quality/thermal paired transfers are
 * handled outside this water-only kernel. Intended inputs keep all products finite and volume
 * nonnegative.
 */
KOKKOS_INLINE_FUNCTION SourceBudget sourceBudget(double volume, double h, double area, double dt,
                                                 double dry, double rain_rate, double coupling_rate,
                                                 double infil_rate, double evap_rate) {
    const double adt = area * dt;
    const double rain = (rain_rate > 0.0 ? rain_rate : 0.0) * adt;
    const double coupling = coupling_rate * adt;
    const double inf = sourceSink(infil_rate, h, dry) * adt;
    const double evap = sourceSink(evap_rate, h, dry) * adt;
    if (rain == 0.0 && coupling == 0.0 && inf == 0.0 && evap == 0.0)
        return {volume, 0.0, 0.0, 0.0, false};
    const double incoming = rain + (coupling > 0.0 ? coupling : 0.0);
    const double available = volume + incoming > 0.0 ? volume + incoming : 0.0;
    const double out = coupling < 0.0 ? -coupling : 0.0;
    const double requested = inf + evap + out;
    const double scale = requested > available && requested > 0.0 ? available / requested : 1.0;
    return {requested >= available ? 0.0 : available - requested, inf * scale, evap * scale,
            (coupling > 0.0 ? coupling : 0.0) - out * scale, true};
}
} // namespace openswmm::twoD::gpu
