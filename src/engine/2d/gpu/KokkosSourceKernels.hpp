// SPDX-License-Identifier: Apache-2.0
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
KOKKOS_INLINE_FUNCTION double sourceSink(double rate, double h, double dry) {
    if (rate <= 0.0 || h <= 0.0)
        return 0.0;
    if (h >= dry)
        return rate;
    const double t = h / dry;
    return rate * t * t * (3.0 - 2.0 * t);
}
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
