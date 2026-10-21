// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CompletedSourceArchive.hpp"
#include "SimulationContext.hpp"
#include <algorithm>

namespace openswmm::detail {
// Native LEGACY mixing owns its persistent state on NodeData/LinkData and
// MassBalanceData. Its lateral assembly buffers reset on every execute.
inline bool checkpointNetworkPollutants(const SimulationContext& c) {
    return c.n_nodes()>0 && c.n_pollutants()>0 && !c.options.ignore_quality &&
        c.options.quality_solver==QualitySolverKind::LEGACY && !c.options.water_age &&
        !c.options.heat_transport && !c.reactions.configured && c.n_subcatches()==0 &&
        c.lid_usage.subcatch_index.empty() &&
        std::none_of(c.node_subtypes.storages.lid.begin(),c.node_subtypes.storages.lid.end(),
                     [](const auto& lid){return lid.control>=0;}) &&
        std::all_of(c.treatment.expressions.begin(),c.treatment.expressions.end(),
                    [](const auto& expression){return expression.empty();});
}
inline void networkPollutantModel(CompletedSourceArchive& a,const SimulationContext& c) {
    if(c.n_nodes()==0 || c.n_pollutants()==0 || c.options.ignore_quality)return;
    std::string policy="native LEGACY network pollutant checkpoint v1";
    auto names=c.pollutant_names.names();auto p=c.pollutants;auto q=c.initial_quality;
    auto solver=c.options.quality_solver;auto treatment=c.treatment.expressions;
    a(policy,solver,names,p.units,p.mwt,p.k_decay,p.c_rain,p.c_gw,p.c_rdii,p.c_dwf,
      p.init_conc,p.co_pollut,p.co_frac,p.snow_only,q.is_link,q.elem_name,q.elem_idx,
      q.constituent,q.kind,q.value,q.from_file,q.file,treatment);
}
} // namespace openswmm::detail
