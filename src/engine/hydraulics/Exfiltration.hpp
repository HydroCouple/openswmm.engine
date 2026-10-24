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
 * @file      Exfiltration.hpp
 * @brief     Storage node exfiltration — Green-Ampt bottom/bank seepage.
 *
 * @details Batch over all storage nodes with exfiltration. Each storage
 *          node has independent bottom and bank Green-Ampt state.
 *
 * @note Legacy reference: src/legacy/engine/exfil.c
 * @ingroup new_engine
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
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
    std::vector<int> node_idx;           ///< Storage node index
    std::vector<double> btm_area;        ///< Bottom area (ft2)
    std::vector<double> bank_min_depth;  ///< Depth where bank seepage starts
    std::vector<double> bank_max_depth;  ///< Depth at max bank area
    std::vector<double> bank_max_area;   ///< Maximum bank seepage area (ft2)
    std::vector<GreenAmptState> btm_ga;  ///< Bottom Green-Ampt state
    std::vector<GreenAmptState> bank_ga; ///< Bank Green-Ampt state

    /**
     * @brief Resize exfiltration columns and reset geometry/index columns.
     *
     * Assigns count, resets node indices to-1 and geometry values0; Green-Ampt state vectors resize
     * preserving their prefix. Allocation can throw after partial resize. Borrowed element references
     * may invalidate.
     *
     * @param[in] n Nonnegative row count; negative casts to an excessive size and is not validated.
     */
    void resize(int n);
};

class ExfilSolver {
public:
    /**
     * @brief Build storage exfiltration geometry and independent bottom/bank Green-Ampt states.
     *
     * Selects storage nodes with positive bottomConductivity. If none, sets count0 while retaining
     * previous vector storage. Otherwise resets geometry, initializes GA states and derives bottom/bank
     * areas. Tabular authored geometry converts to ft/ft2; functional geometry preserves legacy
     * raw-coefficient behavior. Does not perform current-step loss booking or validate every
     * curve/index. Allocation/init failures can leave partial solver state.
     *
     * @param[in,out] ctx Borrowed resolved model with aligned storage subtype/curve data and authored
     *     exfiltration parameters.
     */
    void init(SimulationContext& ctx);

    /**
     * @brief Advance eligible storage exfiltration states and publish raw loss rates.
     *
     * Suppresses legacy seepage for Richards/non-current LID bottoms or aquifer-owned beds. Computes
     * bottom/clogging and wetted-bank losses, including dry-state recovery. Publishes uncapped raw
     * rate; Router later applies the joint evaporation/exfiltration water-volume cap and books loss
     * volume. No independent donor cap, model validation or synchronization.
     *
     * @param[in,out] ctx Borrowed live node depths/climate factors; writes storage exfil_rate in cfs,
     *     with coherent mappings from init.
     * @param[in] dt Positive step seconds used by modified Green-Ampt evolution; caller validates.
     */
    void computeAll(SimulationContext& ctx, double dt);

    /**
     * @brief Borrow mutable exfiltration solver state for checkpoint/inspection.
     * @return Owned SoA reference; resizing/reinitialization invalidates element views. Caller
     *     preserves dimensions and serializes mutation.
     */
    ExfilSoA& state() { return soa_; }
    /**
     * @brief Borrow read-only exfiltration solver state.
     * @return Owned SoA reference valid while solver lives; element views invalidate on mutable
     *     resize/init.
     */
    const ExfilSoA& state() const { return soa_; }

private:
    ExfilSoA soa_;
};

} // namespace exfil
} // namespace openswmm

#endif // OPENSWMM_EXFILTRATION_HPP
