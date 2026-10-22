// SPDX-License-Identifier: Apache-2.0

/**
 * @file      GwSourceForcing.hpp
 * @brief     Immutable groundwater source series in elapsed simulation time.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_GW_SOURCE_FORCING_HPP
#define OPENSWMM_GW_SOURCE_FORCING_HPP
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>
namespace openswmm::twoD {
// Immutable linear series in elapsed simulation seconds. Endpoint hold matches
// the standard SWMM continuous table lookup. Empty times denotes a constant.
struct GwSourceSeries {
    std::vector<double> times, values;
    bool hasNegative = false;
};
struct GwSourceSignal {
    double constant = 0.0;
    // Inline vectors support small programmatic signals; resolved named
    // series share one immutable allocation across all cell source rows.
    std::vector<double> times, values;
    std::shared_ptr<const GwSourceSeries> series;
    /**
     * @brief Borrow the signal's elapsed-second knot vector.
     * @return Shared immutable series times when series is present, otherwise inline times.
     * Empty means constant forcing. Valid while the selected vector and this signal remain alive
     * and are not reassigned; concurrent mutation is unsupported. The call neither copies nor
     * allocates.
     */
    const std::vector<double>& timePoints() const noexcept {
        return series ? series->times : times;
    }
    /**
     * @brief Borrow ordinates associated with timePoints().
     * @return Shared series values when series is present, otherwise inline values. A prepared
     * nonconstant signal requires exactly one ordinate per knot. Units belong to the consuming
     * flow, concentration, temperature or loading prescription. Lifetime follows the selected
     * vector; no allocation or pointer retention occurs beyond the returned reference.
     */
    const std::vector<double>& ordinates() const noexcept {
        return series ? series->values : values;
    }
    /**
     * @brief Evaluate a constant or piecewise-linear signal with endpoint hold.
     * @param[in] t Finite elapsed simulation time s; may lie outside the knot range.
     * @return Constant when no time knots exist, otherwise linear interpolation in native signal
     * units and first/last ordinate outside the range. Shared series takes precedence over
     * inline data.
     * @note Requires finite strictly increasing times and equally sized finite ordinates for a
     * nonconstant signal. No shape/finite checks are made; NaN time is unsupported. Read-only,
     * allocation-free and noexcept, with all referenced data stable for the call.
     */
    double at(double t) const noexcept {
        const auto& times = timePoints();
        const auto& values = ordinates();
        if (times.empty())
            return constant;
        if (t <= times.front())
            return values.front();
        if (t >= times.back())
            return values.back();
        const auto hi = std::upper_bound(times.begin(), times.end(), t);
        const auto i = static_cast<std::size_t>(hi - times.begin());
        const double f = (t - times[i - 1]) / (times[i] - times[i - 1]);
        return values[i - 1] + f * (values[i] - values[i - 1]);
    }
};
struct GwResolvedSourceTerm {
    int row = -1;
    bool massRate = false; // pollutant native mass/s, converted by massScale
    double massScale = 1.0;
    GwSourceSignal signal;
};
struct GwResolvedSource {
    std::string name;
    double scale = 1.0;
    GwSourceSignal flow;
    std::vector<std::pair<int, double>> cells; // active cell, share of TOTAL source
    std::vector<GwResolvedSourceTerm> terms;
};
struct GwSourcePulse {
    double in = 0.0, out = 0.0; // separately integrated positive/negative flow
    std::vector<double> mass;
};
// Exact integral on each common linear interval, including sign changes in
// flow. CONC only enters on injection; extraction carries in-situ dissolved
// quality. MASS is an independent nonnegative mass loading (also at Q=0).
/**
 * @brief Accumulate signed water and injection/load species forcing over linear intervals.
 * @param[in] s Stable resolved source: flow is total m3/s, positive injection; scale is a
 * positive multiplier. Concentration terms retain native units; massRate terms are native
 * amount/s converted by massScale before accumulation. Signals must satisfy GwSourceSignal::at()
 * preconditions.
 * @param[in] t Finite interval start in elapsed simulation seconds.
 * @param[in] dt Finite interval duration s. Nonpositive/NaN values return without changing
 * pulse; positive infinity is unsupported rather than explicitly rejected.
 * @param[in] weight Finite positive source share, normally a cell fraction. Nonpositive/NaN
 * values or source scale return unchanged; values above one are not clipped or validated.
 * @param[in,out] pulse Existing accumulated result. Adds separately nonnegative requested input
 * and extraction water m3. Adds native_concentration*m3 inventory to existing mass entries;
 * invalid species rows are skipped, and this helper never resizes that vector.
 * @note Integrates each linear interval exactly, splitting flow sign changes. Concentration
 * enters only with positive injection; extraction composition belongs to the receiving solver.
 * Independent mass loading applies even at zero flow and is not sign-clipped here. No
 * donor/capacity limits or finite-result checks occur. Allocation failure can interrupt after
 * earlier subintervals have already accumulated; this routine provides no rollback or thread
 * synchronization.
 */
inline void integrateGwSource(const GwResolvedSource& s, double t, double dt, double weight,
                              GwSourcePulse& pulse) {
    if (!(dt > 0) || !(weight > 0) || !(s.scale > 0))
        return;
    const double end = t + dt, scale = weight * s.scale;
    std::vector<double> knots{t, end};
    auto add = [&](const GwSourceSignal& v) {
        const auto& times = v.timePoints();
        for (auto it = std::upper_bound(times.begin(), times.end(), t);
             it != times.end() && *it < end; ++it)
            knots.push_back(*it);
    };
    add(s.flow);
    for (const auto& term : s.terms)
        add(term.signal);
    std::sort(knots.begin(), knots.end());
    knots.erase(std::unique(knots.begin(), knots.end()), knots.end());
    for (std::size_t i = 1; i < knots.size(); ++i) {
        const double a = knots[i - 1], b = knots[i], qa = s.flow.at(a), qb = s.flow.at(b);
        std::vector<double> cuts{a, b};
        if ((qa < 0 && qb > 0) || (qa > 0 && qb < 0))
            cuts.insert(cuts.begin() + 1, a + (b - a) * (-qa) / (qb - qa));
        for (std::size_t k = 1; k < cuts.size(); ++k) {
            const double lo = cuts[k - 1], hi = cuts[k], h = hi - lo, q0 = s.flow.at(lo),
                         q1 = s.flow.at(hi);
            const double volume = .5 * (q0 + q1) * h * scale;
            if (volume >= 0)
                pulse.in += volume;
            else
                pulse.out -= volume;
            for (const auto& term : s.terms) {
                if (term.row < 0 || static_cast<std::size_t>(term.row) >= pulse.mass.size())
                    continue;
                const double c0 = term.signal.at(lo), c1 = term.signal.at(hi);
                double m = 0;
                if (term.massRate)
                    m = .5 * (c0 + c1) * h * term.massScale;
                else if (volume > 0)
                    m = h * (2 * q0 * c0 + q0 * c1 + q1 * c0 + 2 * q1 * c1) / 6;
                pulse.mass[static_cast<std::size_t>(term.row)] += m * scale;
            }
        }
    }
}
} // namespace openswmm::twoD
#endif
