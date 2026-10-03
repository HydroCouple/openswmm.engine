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
 * @file RunoffInterface.hpp
 * @brief Runoff interface file — save/load pre-computed runoff results.
 *
 * @details Binary file format (matching legacy runoff.c exactly — files
 *          inter-operate with the legacy engine in both directions):
 *   Header:
 *     - File stamp: "SWMM5-RUNOFF" (12 bytes)
 *     - nSubcatch (int32)
 *     - nPollut (int32)
 *     - flowUnits (int32)
 *     - maxSteps (int32, updated at close)
 *   Per timestep:
 *     - tStep (float32, seconds)
 *     - Per subcatchment (values in USER units, per legacy
 *       subcatch_getResults(j, 1.0, x)):
 *       - rainfall (project rain units), snow_depth (×UCF(RAINDEPTH)),
 *         evap (×UCF(EVAPRATE)), infil (×UCF(RAINFALL)),
 *         runoff (×UCF(FLOW), zeroed below MIN_RUNOFF·area),
 *         gw_flow (total, ×UCF(FLOW)), gw_elev (×UCF(LENGTH)),
 *         soil_moist (8 float32)
 *       - washoff concentration per pollutant (nPollut float32,
 *         zeroed when runoff is zero)
 *
 * @note Legacy reference: src/legacy/engine/runoff.c,
 *       subcatch.c subcatch_getResults().
 * @note Fields with no refactored per-subcatchment state are written as 0
 *       and skipped on read: snow_depth, gw_elev, soil_moist. Legacy's
 *       LID-drain addition to reported runoff is not replicated either
 *       (the refactored LID drain is architected as next-step runon).
 * @note The rainfall slot (write-only in both engines — neither read path
 *       consumes it) carries the live gage rate here vs legacy's
 *       report-period accumulator, which lags one report step; the first
 *       record may differ. Verified byte-identical to legacy everywhere
 *       else for US and SI models (2026-07-02).
 * @note The read path reproduces legacy runoff_readFromFile verbatim —
 *       including its evap asymmetry (written ×UCF(EVAPRATE) but read back
 *       ÷UCF(RAINFALL), a ×24 inflation) — so USE-mode results match a
 *       legacy USE run on the same file bit-for-bit in intent.
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_RUNOFF_INTERFACE_HPP
#define OPENSWMM_RUNOFF_INTERFACE_HPP

#include <string>
#include <vector>
#include <cstdio>

namespace openswmm {

struct SimulationContext;

namespace runoff_iface {

static constexpr int N_FIXED_RESULTS = 8;  ///< Fixed result fields per subcatchment
static const char FILE_STAMP[] = "SWMM5-RUNOFF";

/**
 * @brief Runoff interface file manager.
 */
class RunoffInterfaceFile {
public:
    ~RunoffInterfaceFile() { close(); }

    /**
     * @brief Open file for writing (SAVE mode).
     * @returns 0 on success, error code on failure.
     */
    int openForWrite(const std::string& path, int n_subcatch, int n_pollut,
                     int flow_units);

    /**
     * @brief Open file for reading (USE mode).
     * @returns 0 on success, error code on failure.
     */
    int openForRead(const std::string& path, int n_subcatch, int n_pollut,
                    int flow_units);

    /**
     * @brief Save one timestep of runoff results to file.
     * @param ctx  Simulation context (subcatchment results read from here).
     * @param dt   Timestep duration (seconds).
     */
    void saveResults(const SimulationContext& ctx, double dt);

    /**
     * @brief Read one timestep of runoff results from file.
     * @param ctx     Simulation context (subcatchment results written here).
     * @param dt_out  Optional: receives the record's runoff timestep (sec),
     *                used by the USE-mode runoff clock (legacy
     *                runoff_readFromFile advances NewRunoffTime by the
     *                file's tStep).
     * @returns true if data was read, false if EOF.
     */
    bool readResults(SimulationContext& ctx, double* dt_out = nullptr);

    /// Close the file and finalize header (write step count for SAVE mode).
    void close();

    bool isOpen() const { return fp_ != nullptr; }

private:
    std::FILE* fp_ = nullptr;
    bool writing_ = false;
    int n_subcatch_ = 0;
    int n_pollut_ = 0;
    int n_results_ = 0;       ///< N_FIXED_RESULTS + n_pollut
    int flow_units_ = 0;      ///< Project flow units (for UCF(FLOW) = Qcf)
    int unit_system_ = 0;     ///< 0 = US, 1 = SI (for Ucf lookups)
    int step_count_ = 0;
    long max_steps_pos_ = 0;   ///< File position of maxSteps field
    std::vector<float> buf_;   ///< Reusable buffer for read/write
};

} // namespace runoff_iface
} // namespace openswmm

#endif // OPENSWMM_RUNOFF_INTERFACE_HPP
