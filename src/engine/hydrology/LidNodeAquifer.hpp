// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <vector>
namespace openswmm {
struct SimulationContext;
namespace twoD { class SubsurfaceSolver; struct SurfaceStateData; struct RichardsBedAcceptance; }
namespace lidnode {
// Internal water-only real LID-state bridge at an explicit closed owner interval.
// Uses current node geometry, pressure limits, forcing and clogging; no new UI
// parameters. Successful preparation requires one finishStep after routing ports.
// Production cross-tier 1D scheduling and joint restart remain gated.
twoD::RichardsBedAcceptance acceptRichardsNodesAtFiring(SimulationContext&,twoD::SubsurfaceSolver&,
    twoD::SurfaceStateData&,double start,double end,double evaporation,const std::vector<int>& nodes);
// Same atomic real-state publication, replacing one actual due GW tier firing
// including its ordinary cells. Caller supplies all due Richards node owners.
twoD::RichardsBedAcceptance acceptRichardsNodesForTier(SimulationContext&,twoD::SubsurfaceSolver&,
    twoD::SurfaceStateData&,int tier,double start,double end,double evaporation,const std::vector<int>& nodes);
}
}
