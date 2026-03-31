/**
 * @file RDII.hpp
 * @brief RDII — rainfall-dependent infiltration/inflow via unit hydrograph.
 *
 * @details Each RDII node has 3 triangular unit hydrographs (short/medium/long
 *          response). The RDII inflow at each timestep is the convolution of
 *          past rainfall with the UH ordinates.
 *
 *          SoA: past rainfall stored as circular buffer per UH group.
 *          Convolution is a dot-product — vectorisable.
 *
 * @note Legacy reference: src/legacy/engine/rdii.c
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_RDII_HPP
#define OPENSWMM_RDII_HPP

#include <vector>
#include <string>
#include <unordered_map>

namespace openswmm {

struct SimulationContext;

namespace rdii {

struct UnitHydParams {
    double r[12][3]     = {};  ///< Rainfall fraction per month × response
    double tPeak[12][3] = {};  ///< Time to peak (sec) per month × response
    double tBase[12][3] = {};  ///< Base time (sec) per month × response
    double iaMax[12][3] = {};  ///< Initial abstraction max depth
    double iaRecov[12][3] = {};///< IA recovery rate
    double iaInit[12][3] = {}; ///< Initial IA used
};

/// Per-response (SHORT/MEDIUM/LONG) unit hydrograph data.
/// Legacy equivalent: TUHData uh[3] inside TUHGroup.
struct UHResponseData {
    std::vector<double> past_rain;      ///< circular buffer of past rainfall depths
    std::vector<int>    past_month;     ///< month for each past rainfall entry
    int    period      = 0;             ///< current buffer write position
    int    max_periods = 0;             ///< buffer capacity
    int    has_past_rain = 0;           ///< true if any non-zero past rain
    double ia_used     = 0.0;           ///< initial abstraction used so far
    long   dry_seconds = 0;             ///< seconds since last non-zero rainfall

    void allocate(int n) {
        max_periods = n;
        past_rain.assign(static_cast<std::size_t>(n), 0.0);
        past_month.assign(static_cast<std::size_t>(n), 0);
        period = 0;
        has_past_rain = 0;
        ia_used = 0.0;
        dry_seconds = static_cast<long>(n) * 300 + 1; // start dry
    }
};

struct RDIIGroupSoA {
    int count = 0;
    std::vector<int>    node_idx;       ///< Target node index
    std::vector<int>    uh_idx;         ///< Unit hydrograph parameter index
    std::vector<double> area;           ///< Contributing area (acres, project units)

    /// Per-response data: [group * 3 + response]
    std::vector<UHResponseData> uh_data;

    std::vector<int>    rain_interval;  ///< Rain processing interval (sec) per group
    std::vector<double> time_accum;     ///< Accumulated time (sec) within current interval
    std::vector<double> rain_at_start;  ///< Rainfall (in/hr) captured at start of interval

    void resize(int n);
};

class RDIISolver {
public:
    void init(SimulationContext& ctx);

    /**
     * @brief Register a unit hydrograph parameter set by name.
     *
     * @param name  Unit hydrograph name (e.g. from [HYDROGRAPHS] section).
     * @param params  Complete UH parameters for all 12 months x 3 responses.
     * @returns Index of the registered UH parameter set.
     */
    int addUnitHydParams(const std::string& name, const UnitHydParams& params);

    /**
     * @brief Look up unit hydrograph index by name.
     * @returns Index, or -1 if not found.
     */
    int findUnitHyd(const std::string& name) const;

    /**
     * @brief Compute RDII inflows for all groups and add to node lat_flow.
     *
     * @details Convolution: RDII = sum(pastRain[i] * r[m][k] * u(t))
     *          The inner product is vectorisable over past periods.
     */
    void computeAll(SimulationContext& ctx, double rainfall, int month, double dt);

    std::vector<UnitHydParams> uh_params;

private:
    RDIIGroupSoA groups_;
    std::unordered_map<std::string, int> uh_name_to_idx_;

    /// Compute UH ordinate at time t for response k, month m.
    double uhOrdinate(const UnitHydParams& uh, int month, int response, double t) const;

    /// Compute rain processing interval for a UH (minimum limb duration, capped by wet_step).
    static int getRainInterval(const UnitHydParams& uh, double wet_step);

    /// Compute max past periods for a UH response given a rain interval.
    static int getMaxPeriods(const UnitHydParams& uh, int response, int rainInterval);
};

} // namespace rdii
} // namespace openswmm

#endif // OPENSWMM_RDII_HPP
