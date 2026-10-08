// SPDX-License-Identifier: Apache-2.0
#include "SourceClockGroups.hpp"
#include "../core/CompletedSourceArchive.hpp"
#include <cmath>
namespace openswmm::runoff {
void SourceClockGroups::checkpoint(detail::CompletedSourceArchive& a,bool model) {
    if(model) {
        a(initialized_,ignore_rain_,dry_only_,start_date_,duration_,rain_ucf_,evap_ucf_,evap_method_);
        a(monthly_evap_,adjust_evap_,adjust_rain_,infil_,recovery_,evap_times_,evap_values_,rain_scale_,source_gage_,source_group_);
        a(forcing_.subcatch_rainfall_mode,forcing_.subcatch_rainfall_value,forcing_.subcatch_evap_mode,forcing_.subcatch_evap_value,
            forcing_.climate_evap_mode,forcing_.climate_evap_value);
        a.sequence(gages_,[&](auto& g){a(g.times,g.rates,g.period,g.override_rate,g.mode,g.prescribed);},true);
    }
    a.sequence(groups_,[&](auto& g) {
        if(model)a(g.subcatches,g.lid_usages);
        else {
            if(!g.pending.empty())throw std::runtime_error("Checkpoint requires committed source clocks.");
            a(g.completed_end);
            if(!std::isfinite(g.completed_end) || g.completed_end<0 || g.completed_end>duration_)
                throw std::runtime_error("Invalid source restart clock.");
        }
    },true);
}
}
