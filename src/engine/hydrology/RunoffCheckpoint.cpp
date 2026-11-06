// SPDX-License-Identifier: Apache-2.0

/**
 * @file      RunoffCheckpoint.cpp
 * @brief     Checkpoint persistence for runoff and hydrological state.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "Runoff.hpp"
#include "../core/CompletedSourceArchive.hpp"
#include <cmath>
namespace openswmm::runoff {
void RunoffSolver::checkpoint(detail::CompletedSourceArchive& a, bool model) {
    if (model) {
        a(soa_.area, soa_.width, soa_.slope, soa_.imperv_pct, soa_.frac_imperv0, soa_.frac_imperv1);
        a(soa_.alpha_imperv, soa_.alpha_perv, soa_.ds_imperv, soa_.ds_perv, soa_.n_imperv,
          soa_.n_perv);
        a(spatial_full_area_ft2_);
        infil_bank_.checkpoint(a, true);
    } else {
        a.fixed(soa_.depth_imperv0, soa_.depth_imperv1, soa_.depth_perv, soa_.old_runoff_imperv0,
                soa_.old_runoff_imperv1, soa_.old_runoff_perv);
        a.fixed(soa_.runoff, soa_.evap_loss, soa_.infil_loss, soa_.perv_evap_vol,
                soa_.actual_perv_evap_vol, soa_.spatial_infil_vol);
        a.fixed(soa_.native_infil_rate, soa_.infil_vol, soa_.subarea_runoff_rate,
                soa_.imperv_runoff_cfs, soa_.perv_runoff_cfs, soa_.outflow_vol);
        a.fixed(infil_factor_used_, completed_step_seconds_);
        // Reused weather buffers are allocated lazily by executeCompleted.
        a(precip_, evap_rate_, infil_rate_);
        if (a.reading())
            for (const auto* v : {&precip_, &evap_rate_, &infil_rate_})
                if (!v->empty() && v->size() != soa_.area.size())
                    throw std::runtime_error("Invalid runoff checkpoint buffer shape.");
        a.sequence(
            soa_.completed,
            [&](auto& pools) {
                for (auto& w : pools)
                    a(w.area, w.opening, w.rain, w.runon, w.evaporation, w.infiltration,
                      w.spatial_infiltration, w.outflow, w.outlet, w.closing);
            },
            true);
        infil_bank_.checkpoint(a, false);
    }
}
void RunoffSolver::validateCheckpoint(const RunoffSolver& model) const {
    const auto require = [](bool ok) {
        if (!ok)
            throw std::runtime_error("Invalid runoff checkpoint state.");
    };
    require(soa_.n_subcatch == model.soa_.n_subcatch && soa_.area == model.soa_.area);
    for (const auto* row : {&soa_.depth_imperv0,
                            &soa_.depth_imperv1,
                            &soa_.depth_perv,
                            &soa_.old_runoff_imperv0,
                            &soa_.old_runoff_imperv1,
                            &soa_.old_runoff_perv,
                            &soa_.runoff,
                            &soa_.evap_loss,
                            &soa_.infil_loss,
                            &soa_.perv_evap_vol,
                            &soa_.actual_perv_evap_vol,
                            &soa_.spatial_infil_vol,
                            &soa_.native_infil_rate,
                            &soa_.infil_vol,
                            &soa_.subarea_runoff_rate,
                            &soa_.imperv_runoff_cfs,
                            &soa_.perv_runoff_cfs,
                            &soa_.outflow_vol,
                            &infil_factor_used_,
                            &completed_step_seconds_,
                            &precip_,
                            &evap_rate_,
                            &infil_rate_})
        for (double v : *row)
            require(std::isfinite(v) && v >= 0);
    for (const auto& pools : soa_.completed)
        for (const auto& w : pools)
            for (double v : {w.area, w.opening, w.rain, w.runon, w.evaporation, w.infiltration,
                             w.spatial_infiltration, w.outflow, w.outlet, w.closing})
                require(std::isfinite(v) && v >= 0);
    infil_bank_.validateCheckpoint(model.infil_bank_);
}

}
