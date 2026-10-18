// SPDX-License-Identifier: Apache-2.0
#pragma once
namespace openswmm {
// Solver-neutral finite channel water. Region IDs belong to the actual
// transport engine; ARD regions are cells, LEGACY/LARD regions are links.
// Along-reach fractions map hydraulic discretization onto the full polyline.
struct ChannelThermalRegion {
    int link=-1,region=-1;
    double begin=0,end=1;
    double volume=0,temperature=0; // SI m3 and degC
    double length=0,surface_area=0,bed_area=0,depth=0; // SI m, m2, m2, m
};
struct ChannelThermalIncrement {
    int link=-1,region=-1;
    double celsius=0;
};
} // namespace openswmm
