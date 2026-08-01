/**
 * @file WQUncertaintyBounds.hpp
 * @brief Analytical water-quality uncertainty bounds for first-order decay.
 *
 * @details Post-processing module: no dynamic ensemble state is maintained.
 *          Given a deterministic concentration and a first-order decay rate k
 *          with uncertain multiplier drawn from LHS[1-p, 1+p], the bounds on
 *          c(t+dt) = c(t) * exp(-k * km_i * dt) are computed analytically
 *          across M LHS members.
 *
 *          Higher k_mult → more decay → lower concentration.
 *          Therefore:
 *            q05 ≈ c * exp(-k * (1+p) * dt)   (most decay)
 *            q95 ≈ c * exp(-k * (1-p) * dt)   (least decay)
 *          with exact LHS stratification for finite M.
 *
 *          Usage:
 *          @code
 *          WQUncertaintyBounds wq;
 *          wq.decay_pert = 0.20;
 *          auto b = wq.compute(10.0, 0.1, 1.0, 50);
 *          // b.q05 ≈ 10*exp(-0.12), b.q95 ≈ 10*exp(-0.08)
 *          @endcode
 *
 * @ingroup engine_uncertainty
 */

#ifndef OPENSWMM_ENGINE_UNCERTAINTY_WQ_UNCERTAINTY_BOUNDS_HPP
#define OPENSWMM_ENGINE_UNCERTAINTY_WQ_UNCERTAINTY_BOUNDS_HPP

#include <vector>
#include <algorithm>
#include <cmath>

namespace openswmm::uncertainty {

/**
 * @brief Analytical WQ bounds for first-order decay under uncertain k.
 *
 * Thread-safe: compute() is const with no shared mutable state.
 */
struct WQUncertaintyBounds {

    /// Fractional perturbation on the decay rate k (half-range).
    /// k_i = k * km_i,  km_i ∈ [1 - decay_pert, 1 + decay_pert].
    double decay_pert = 0.20;

    struct Bounds {
        double q05;  ///< 5th-percentile concentration
        double q50;  ///< Median concentration
        double q95;  ///< 95th-percentile concentration
    };

    /**
     * @brief Compute q05/q50/q95 concentration bounds for one decay step.
     *
     * Generates M LHS multipliers km_i ∈ [1-p, 1+p] in ascending order,
     * computes c_i = c0 * exp(-k * km_i * dt) for each member, then
     * extracts percentiles.  Because larger km → smaller c, the i=0
     * member (smallest km) has the highest concentration (q95 direction).
     *
     * @param c0  Concentration at start of timestep (any consistent unit).
     * @param k   First-order decay rate constant (1/time).
     * @param dt  Timestep duration (same time unit as k).
     * @param M   Number of LHS members (≥ 2).
     * @returns   Bounds at t0 + dt.
     */
    Bounds compute(double c0, double k, double dt, int M) const {
        if (M < 2) M = 2;
        const double p  = decay_pert;
        const double lo = 1.0 - p;
        const double hi = 1.0 + p;
        const double Md = static_cast<double>(M);

        std::vector<double> conc(static_cast<std::size_t>(M));
        for (int i = 0; i < M; ++i) {
            // LHS stratum i: km_i ascending from lo to hi
            double km = lo + (static_cast<double>(i) + 0.5) / Md * (hi - lo);
            conc[static_cast<std::size_t>(i)] = c0 * std::exp(-k * km * dt);
        }
        // conc is in descending order (larger km → more decay → lower c),
        // so sort ascending to get percentile positions.
        std::sort(conc.begin(), conc.end());

        auto idx = [&](double q) -> double {
            double pos = q * static_cast<double>(M - 1);
            int    lo_i = static_cast<int>(pos);
            int    hi_i = lo_i + 1;
            if (hi_i >= M) return conc[static_cast<std::size_t>(M - 1)];
            double frac = pos - static_cast<double>(lo_i);
            return conc[static_cast<std::size_t>(lo_i)] * (1.0 - frac)
                 + conc[static_cast<std::size_t>(hi_i)] * frac;
        };

        return { idx(0.05), idx(0.50), idx(0.95) };
    }

    /**
     * @brief Convenience: advance concentration by one timestep and return bounds.
     *
     * Equivalent to compute(c_prev, k, dt, M).
     */
    Bounds step(double c_prev, double k, double dt, int M) const {
        return compute(c_prev, k, dt, M);
    }
};

} // namespace openswmm::uncertainty

#endif // OPENSWMM_ENGINE_UNCERTAINTY_WQ_UNCERTAINTY_BOUNDS_HPP
