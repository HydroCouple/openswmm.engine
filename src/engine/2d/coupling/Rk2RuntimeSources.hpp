// SPDX-License-Identifier: Apache-2.0
#ifndef OPENSWMM_RK2_RUNTIME_SOURCES_HPP
#define OPENSWMM_RK2_RUNTIME_SOURCES_HPP
#include "RuntimeCoupling.hpp"
#include <cstdint>
namespace openswmm::twoD {
class SubsurfaceSolver;
struct SurfaceStateData;
std::string validateRk2Sources(const RuntimeSources&,int cells,int species,int temperature,int age,
                               const std::vector<uint8_t>* coverage=nullptr);
std::string rk2SurfaceRuntimeProfile(const SurfaceStateData&,const SubsurfaceSolver* ground=nullptr);
}
#endif
