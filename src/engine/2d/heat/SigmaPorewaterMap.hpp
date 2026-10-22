// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin

/**
 * @file      SigmaPorewaterMap.hpp
 * @brief     Derived overlap mapping for canonical sigma-layer porewater chemistry.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include <string>
#include <vector>
namespace openswmm::twoD {
struct SubsurfaceTransportState;
// A sparse contact view of canonical SIGMA chemistry. It owns only derived
// overlap values and unconsumed contact receipts, never a second reservoir.
struct SigmaPorewaterMap {
    struct Overlap {
        int physical = 0, reactor = 0;
        double water = 0, bulk = 0;
    };
    int layers = 0, species = 0;
    std::vector<Overlap> overlaps;
    std::vector<double> capacity, mass, reserved, pending;
    /**
     * @brief Test whether a stretching-layer overlap map is configured.
     * @return layers > 0; does not validate species, overlap geometry or inventory extents.
     */
    bool active() const noexcept { return layers > 0; }
    std::string configure(int layer_count, int species_count, std::vector<Overlap>,
                          const std::vector<double>& partition,
                          const std::vector<double>& canonical,
                          const std::vector<double>& saturated_reserved);
    std::string rebase(const std::vector<double>& canonical,
                       const std::vector<double>& saturated_reserved);
    bool apply(int row, int physical, double increment) noexcept;
    std::string publish(SubsurfaceTransportState&, int cell);
};
} // namespace openswmm::twoD
