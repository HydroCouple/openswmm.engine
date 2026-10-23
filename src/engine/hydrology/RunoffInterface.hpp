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
 * @file      RunoffInterface.hpp
 * @brief     Runoff interface file — save/load pre-computed runoff results.
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
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_RUNOFF_INTERFACE_HPP
#define OPENSWMM_RUNOFF_INTERFACE_HPP

#include <string>
#include <vector>
#include <cstdio>

namespace openswmm {

struct SimulationContext;

namespace runoff_iface {

static constexpr int N_FIXED_RESULTS = 8; ///< Fixed result fields per subcatchment
static const char FILE_STAMP[] = "SWMM5-RUNOFF";

/**
 * @brief Runoff interface file manager.
 * @details Owns a native FILE cursor; do not copy an owner with active IO.
 * Serialize all reads/writes/close, and call open only on a closed object: open
 * overwrites fp_ without closing an earlier stream. Binary layout is host-native
 * and requires compatible model index order; header counts alone do not prove it.
 */
class RunoffInterfaceFile {
public:
    /** @brief Finalize a SAVE header and close any owned stream through close().
     * Close/write errors are ignored; caller first serializes outstanding IO. */
    ~RunoffInterfaceFile() { close(); }

    /** @brief Open/truncate a binary runoff output and write a placeholder header.
     * @param path UTF-8 destination; owner must be closed first. No staged replacement.
     * @param n_subcatch Nonnegative subcatchment count fitting int32, matching later ctx arrays.
     * @param n_pollut Nonnegative pollutant count with 8+n_pollut representable as int
     * and buffer capacity; counts are not validated here.
     * @param flow_units Valid project enum [0,5] selecting display flow and US/SI conversions.
     * @return -1 for open failure, otherwise zero after attempted header writes. Does not
     * check fwrite/ftell or catch allocation failures. Header maxSteps is finalized by
     * close(); records use native float representation and no model-name metadata. */
    int openForWrite(const std::string& path, int n_subcatch, int n_pollut, int flow_units);

    /** @brief Open a compatible native runoff file and position at its first record.
     * @param path UTF-8 filename; owner must already be closed.
     * @param n_subcatch Expected nonnegative model count fitting int32; later ctx arrays
     * must have at least this many compatible ordered rows.
     * @param n_pollut Expected nonnegative count with 8+n_pollut representable and allocatable.
     * @param flow_units Expected valid enum [0,5]; exact header equality required.
     * @return Zero after header comparison/setup; -1 open failure, -2 bad stamp, -3 count/
     * unit mismatch. Rejection closes the newly opened stream. Header scalar read
     * counts are unchecked; use only complete qualified headers. Ignores maxSteps and
     * does not validate payload length, dates, names, value finiteness or endianness.
     * Allocation failures propagate; no previous stream is closed automatically. */
    int openForRead(const std::string& path, int n_subcatch, int n_pollut, int flow_units);

    /** @brief Append one native timestep and project-unit subcatchment result record.
     * @param ctx Borrowed compatible model with aligned arrays and selected open counts;
     * reads current gage rainfall, evaporation/infiltration, cfs runoff/GW flow and
     * pollutant concentrations. Does not advance clocks or modify context.
     * @param dt Finite positive timestep seconds, narrowed to float without validation.
     * No-op unless open for SAVE. Converts to header project units; runoff below
     * MIN_RUNOFF*area is zeroed with washoff. Snow depth/GW elevation/soil moisture are
     * written zero; rainfall is live gage rate, and LID drain is not added here. Pollutant
     * slots beyond available concentration storage are zero. Ignores write failures
     * and increments step_count regardless; no explicit flush/status is provided. */
    void saveResults(const SimulationContext& ctx, double dt);

    /** @brief Restore one timestep's native runoff record with legacy unit conversions.
     * @param[in,out] ctx Initialized compatible ordered subcatchment arrays with at least
     * stored count; replaces evap/infil/runoff/GW-flow and available pollutant slots.
     * Rainfall/snow-depth/GW-elevation/soil-moisture fields are skipped. Evaporation
     * intentionally divides by rainfall conversion (legacy asymmetry), not EVAPRATE.
     * @param[out] dt_out Optional writable seconds output; assigned immediately after a
     * successful timestep read, even if a later subcatchment record is truncated.
     * @return True after all rows and count increment, false if closed/writing or any
     * required read is short. False can follow partial context/output mutation. Does
     * not honor header maxSteps, reject nonfinite/negative durations/values, roll back,
     * or distinguish EOF from IO errors. No clock advancement is performed here. */
    bool readResults(SimulationContext& ctx, double* dt_out = nullptr);

    /** @brief Backpatch SAVE maxSteps then close the owned stream.
     * Safe when closed; ignores seek/write/close failures. Stored step count narrows to
     * int32; caller keeps it representable. Clears only fp_, retaining mode, counts,
     * unit selection and buffer. Does not modify any simulation context. */
    void close();

    /** @brief Test owned stream presence.
     * @return True exactly when fp_ is nonnull, including EOF/error streams. This does
     * not indicate mode, valid header/next record, or successful pending writes. */
    bool isOpen() const { return fp_ != nullptr; }

private:
    std::FILE* fp_ = nullptr;
    bool writing_ = false;
    int n_subcatch_ = 0;
    int n_pollut_ = 0;
    int n_results_ = 0;   ///< N_FIXED_RESULTS + n_pollut
    int flow_units_ = 0;  ///< Project flow units (for UCF(FLOW) = Qcf)
    int unit_system_ = 0; ///< 0 = US, 1 = SI (for Ucf lookups)
    int step_count_ = 0;
    long max_steps_pos_ = 0; ///< File position of maxSteps field
    std::vector<float> buf_; ///< Reusable buffer for read/write
};

} // namespace runoff_iface
} // namespace openswmm

#endif // OPENSWMM_RUNOFF_INTERFACE_HPP
