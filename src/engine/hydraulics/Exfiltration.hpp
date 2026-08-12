// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file Exfiltration.hpp
 * @brief Storage node exfiltration — Green-Ampt bottom/bank seepage.
 *
 * @details Batch over all storage nodes with exfiltration. Each storage
 *          node has independent bottom and bank Green-Ampt state.
 *
 * @note Legacy reference: src/legacy/engine/exfil.c
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_EXFILTRATION_HPP
#define OPENSWMM_EXFILTRATION_HPP

#include "../hydrology/Infiltration.hpp"
#include <vector>

namespace openswmm {

struct SimulationContext;

namespace exfil {

struct ExfilSoA {
    int count = 0;
    std::vector<int>    node_idx;       ///< Storage node index
    std::vector<double> btm_area;       ///< Bottom area (ft2)
    std::vector<double> bank_min_depth; ///< Depth where bank seepage starts
    std::vector<double> bank_max_depth; ///< Depth at max bank area
    std::vector<double> bank_max_area;  ///< Maximum bank seepage area (ft2)
    std::vector<GreenAmptState> btm_ga;  ///< Bottom Green-Ampt state
    std::vector<GreenAmptState> bank_ga; ///< Bank Green-Ampt state

    void resize(int n);
};

class ExfilSolver {
public:
    void init(SimulationContext& ctx);

    /**
     * @brief Batch compute exfiltration for all storage nodes.
     *
     * @param ctx  Simulation context (reads node depths, writes losses).
     * @param dt   Timestep (seconds).
     */
    void computeAll(SimulationContext& ctx, double dt);

    ExfilSoA& state() { return soa_; }

private:
    ExfilSoA soa_;
};

} // namespace exfil
} // namespace openswmm

#endif // OPENSWMM_EXFILTRATION_HPP
