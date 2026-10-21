// SPDX-License-Identifier: Apache-2.0
#ifndef OPENSWMM_RK2_RUNTIME_SOURCES_HPP
#define OPENSWMM_RK2_RUNTIME_SOURCES_HPP
#include "RuntimeCoupling.hpp"
#include <cstdint>
namespace openswmm {struct HeatState;namespace climate {struct ClimateState;}namespace detail {class CompletedSourceArchive;}}
namespace openswmm::twoD {
class SubsurfaceSolver;
struct SurfaceStateData;
std::string validateRk2Sources(const RuntimeSources&,int cells,int species,int temperature,int age,
                               const std::vector<uint8_t>* coverage=nullptr);
std::string rk2SurfaceRuntimeProfile(const SurfaceStateData&,const SubsurfaceSolver* ground=nullptr);
// A managed restore binds copied prescriptions and prepared native weather.
// The standalone numerical loader retains its strict caller-held identity.
struct Rk2CheckpointBinding {
    const climate::ClimateState* climate=nullptr;
    const HeatState* heat=nullptr;
    const RuntimeSources* surface=nullptr;
    const RuntimeSources* groundwater=nullptr;
    const RuntimeForcings* forcing=nullptr;
};
void rk2ManagedRuntimeModel(detail::CompletedSourceArchive&,RuntimeSources&,RuntimeSources&,RuntimeForcings&,int cells);
std::string validateRk2ReplayIds(const RuntimeSources&,const RuntimeSources&,const RuntimeForcings&,double completed);
}
#endif
