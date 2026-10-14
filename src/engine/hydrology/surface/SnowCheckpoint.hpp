// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string>
#include <array>
namespace openswmm {
struct SimulationContext;
namespace snow {struct SnowSoA;}
namespace detail {class CompletedSourceArchive;}
namespace surface {
class MeshSnow;
std::string saveCombinedSnowCheckpoint(const SimulationContext&,const snow::SnowSoA&,const MeshSnow*);
std::string restoreCombinedSnowCheckpoint(SimulationContext&,snow::SnowSoA&,MeshSnow*,const std::string&,bool apply,bool seed);
namespace ueb {struct HeldState;}
std::string snowModelIdentity(const SimulationContext&,const snow::SnowSoA&);
std::string snowForcingIdentity(const SimulationContext&);
std::array<double,4> snowRuntimeForcing(const SimulationContext&);
bool snowTransientForcing(const SimulationContext&);
void archiveHeldSnow(detail::CompletedSourceArchive&,ueb::HeldState&);
void archiveDegreeDaySnow(detail::CompletedSourceArchive&,snow::SnowSoA&);
// Internal versioned archive of both native snow formulations, including the
// unconsumed part of held UEB solves and their 24-hour temperature memories.
std::string saveSnowCheckpoint(const SimulationContext&,const snow::SnowSoA&);
// Validation is performed against a private candidate before publication.
// seed=true retains the accepted pack but discards future held predictions
// and resets cumulative ledgers for a new hot-start simulation.
std::string restoreSnowCheckpoint(SimulationContext&,snow::SnowSoA&,const std::string&,
                                 bool apply=true,bool seed=false);
} // namespace surface
} // namespace openswmm
