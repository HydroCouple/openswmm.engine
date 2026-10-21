// SPDX-License-Identifier: Apache-2.0
#ifndef OPENSWMM_RK2_RUNTIME_SOURCES_HPP
#define OPENSWMM_RK2_RUNTIME_SOURCES_HPP
#include "RuntimeCoupling.hpp"
#include <cstdint>
#include <map>
namespace openswmm {struct HeatState;namespace climate {struct ClimateState;}namespace detail {class CompletedSourceArchive;}}
namespace openswmm::twoD {
class SubsurfaceSolver;struct BoundaryData;struct MeshData;
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
    const BoundaryData* boundary=nullptr;
    const std::map<int,std::vector<double>>* boundary_quality=nullptr;
};
bool rk2HasSurfaceBoundary(const BoundaryData*);
std::string validateRk2SurfaceBoundary(const MeshData&,const SurfaceStateData&);
void rk2SurfaceBoundaryArchive(detail::CompletedSourceArchive&,BoundaryData&,std::map<int,std::vector<double>>&,bool model,int species);
void rk2ManagedRuntimeModel(detail::CompletedSourceArchive&,RuntimeSources&,RuntimeSources&,RuntimeForcings&,int species,
                           BoundaryData* boundary=nullptr,std::map<int,std::vector<double>>* quality=nullptr);
std::string validateRk2ReplayIds(const RuntimeSources&,const RuntimeSources&,const RuntimeForcings&,double completed);
}
#endif
