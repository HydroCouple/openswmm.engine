// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#pragma once
#include "ConservativeMixing.hpp"
namespace openswmm::quality {
struct BoundaryWaterImport { std::size_t node, receiver; double volume; };
struct FvTransportWater {
    MixingWater water;
    std::vector<BoundaryWaterImport> boundary_imports;
    std::vector<double> withdrawal, flooding;
    bool ready=false;
};
} // namespace openswmm::quality
