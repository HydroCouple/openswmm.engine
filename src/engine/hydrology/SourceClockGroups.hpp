// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#ifndef OPENSWMM_SOURCE_CLOCK_GROUPS_HPP
#define OPENSWMM_SOURCE_CLOCK_GROUPS_HPP
#include "Runoff.hpp"
#include "Climate.hpp"
#include "../data/ForcingData.hpp"
#include <array>
#include <string>
#include <vector>

namespace openswmm::runoff {

struct SourceForcingInterval {
    double start = 0.0, end = 0.0;
    int month = 0;
    double infil_factor = 1.0, recovery_factor = 1.0;
    std::vector<RunoffSourceForcing> sources;
};

struct SourceClockGroup {
    std::vector<int> subcatches, lid_usages;
    double completed_end = 0.0;
    std::vector<SourceForcingInterval> pending;
};

/// Internal completed-interval clock/forcing adapter. Seeds select whole weakly
/// connected runoff/LID-drain graphs. Immutable source records, not the global
/// gage/climate cursor, supply group rates. No production scheduler calls this yet.
/// Stage changes no source water. The driver must trial every enabled store,
/// settle intake, install accepted source states, then commit the group marker.
/// Cancellation restores no water: uncommitted source trials must stay private.
class SourceClockGroups {
public:
    std::string initialize(const SimulationContext&, const std::vector<int>& seeds);
    std::string stage(int group, double end, double completed_through);
    std::string commit(int group);
    void cancel(int group);
    const std::vector<SourceClockGroup>& groups() const { return groups_; }
    int groupForSubcatch(int source) const;

private:
    struct RainSeries {
        std::vector<double> times, rates;
        double period = 0.0, override_rate = -1.0;
        ForcingMode mode = ForcingMode::NONE;
        double prescribed = 0.0;
    };
    bool initialized_ = false, ignore_rain_ = false, dry_only_ = false;
    double start_date_ = 0.0, duration_ = 0.0, rain_ucf_ = 1.0, evap_ucf_ = 1.0;
    climate::EvapMethod evap_method_ = climate::EvapMethod::CONSTANT;
    ForcingData forcing_;
    std::array<double, 12> monthly_evap_{}, adjust_evap_{}, adjust_rain_{}, infil_{}, recovery_{};
    std::vector<double> evap_times_, evap_values_, rain_scale_;
    std::vector<int> source_gage_, source_group_;
    std::vector<RainSeries> gages_;
    std::vector<SourceClockGroup> groups_;
};
}
#endif
