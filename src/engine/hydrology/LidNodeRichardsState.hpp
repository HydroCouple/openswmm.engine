// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "LidNode.hpp"

namespace openswmm::lidnode::detail {
// Shared geometry/material conversion and private publication draft. All node
// geometry stays in canonical feet; kernel columns and reports are SI.
richards::Column column(const SimulationContext&,int row,double dt,double evaporation,bool continuous=false);
struct RichardsNodeDraft {
    int row=-1,node=-1;
    LidNodeState state;
    double volume=0,depth=0,head=0,full_volume=0,lat_flow=0;
};
RichardsNodeDraft draft(const SimulationContext&,int row,const richards::Column&,
    const richards::Report&,double dt,double completed,bool mesh=false);
void publish(SimulationContext&,RichardsNodeDraft&&) noexcept;
LidNodeState routed(const SimulationContext&,int row);
void checkMeshInterval(const LidNodeState&,double start,double end);
}
