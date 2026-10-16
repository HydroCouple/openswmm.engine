// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "DegreeDaySnow.hpp"
#include <string>
#include <array>
namespace openswmm::surface {
std::string initializeNativeSnow(SimulationContext&,snow::SnowSolver&);
bool hasUebSnow(const SimulationContext&);
std::array<double,10> nativeSnowResults(const SimulationContext&,const snow::SnowSoA&,int subcatch);
double nativeSnowDepth(const snow::SnowSoA&,int subcatch);
// Physical inventory includes retained free water; the legacy depth report
// above includes ice only. Areas are actual non-LID square feet.
double nativeSnowVolume(const snow::SnowSoA&,int subcatch,double area_ft2);
}
