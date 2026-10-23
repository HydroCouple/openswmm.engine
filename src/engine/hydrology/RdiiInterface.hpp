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
 * @file      RdiiInterface.hpp
 * @brief     RDII interface file — use/save pre-computed RDII inflows.
 *
 * @details `[FILES] USE RDII <file>` bypasses the internal unit-hydrograph
 *          computation entirely (legacy rdii_openRdii() skips
 *          createRdiiFile() when Frdii.mode == USE_FILE); the file's flows
 *          become the RDII inflows during routing. `SAVE RDII <file>`
 *          exports the internally computed RDII flows.
 *
 *          Two legacy formats are supported for reading, auto-detected:
 *          - Binary ("SWMM5-RDII" stamp): header = stamp, RdiiStep (int32),
 *            NumRdiiNodes (int32), node indices (int32 × N); records =
 *            date (float64 OADate) + flows (float32 × N, cfs).
 *          - Text ("SWMM5 Interface File"): title, step, constituent count,
 *            "FLOW <units>", node count, node names, headings, then rows
 *            `name yr mon day hr min sec flow` (flow in declared units).
 *
 *          Writing uses the binary format (legacy SAVE parity).
 *
 *          Lookup is step-aligned, NOT interpolated: a record's flows apply
 *          on [date, date + step) — legacy rdii_getNumRdiiFlows().
 *
 * @note Legacy reference: src/legacy/engine/rdii.c
 * @ingroup new_engine
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_RDII_INTERFACE_HPP
#define OPENSWMM_RDII_INTERFACE_HPP

#include <cstdio>
#include <string>
#include <vector>

namespace openswmm {

struct SimulationContext;

namespace rdii_iface {

static const char RDII_FILE_STAMP[] = "SWMM5-RDII";

/**
 * @brief RDII interface file manager (USE = read/override, SAVE = export).
 * @details Owns a native FILE cursor; serialize all operations and do not copy
 * this object while owning a stream. Binary IO uses native integer/float layout
 * and model node order, with no endian conversion or identity fingerprint.
 */
class RdiiInterfaceFile {
public:
    /** @brief Close the owned stream, ignoring flush/close failure.
     * Caller first ends active access; does not publish any additional record. */
    ~RdiiInterfaceFile() { close(); }

    /** @brief Replace the owned stream with a binary or text RDII input and prime a record.
     * @param ctx Borrowed compatible model for binary index bounds/text node-name lookup;
     * name/count arrays must agree. Hydraulic state is not modified.
     * @param path UTF-8 filename, opened through fopen_utf8; no relative-path resolution.
     * @return Zero after a valid header, -1 for open/reopen failure, -2 for invalid header
     * or unresolved node. Zero can still mean no complete first record (priming result
     * is ignored). Closes a previous stream first. Bad header closes the new stream;
     * partial header metadata can remain. Binary data uses native layout. Exceptions
     * can leave a partially initialized open owner; caller/destructor must close it. */
    int openForRead(SimulationContext& ctx, const std::string& path);

    /** @brief Close prior IO, truncate a binary RDII destination and write its header.
     * @param path UTF-8 destination; no staging or atomic replacement is performed.
     * @param rdii_step Record validity seconds; nonpositive input defaults to 300.
     * @param node_idx Copied project node indices in output order, nonempty and count
     * representable as int32. Indices are not validated/deduplicated here.
     * @return Zero after attempting header writes, -1 for failed open, -2 for empty list.
     * Write results are unchecked, so zero does not guarantee persisted bytes. Caller
     * keeps native-layout/model ordering compatible and serializes all writes/close. */
    int openForWrite(const std::string& path, int rdii_step, const std::vector<int>& node_idx);

    /** @brief Add the bracketing record's RDII flow into the context node accumulators.
     * @param[in,out] ctx Compatible initialized node arrays in the model used at open;
     * adds cfs to rdii_inflow without clearing it. Duplicate indices add repeatedly.
     * @param current_date Finite supported absolute SWMM/OLE day; query monotonically
     * forward because the file cursor cannot rewind. Applies on [record_start,record_end).
     * No-op if closed/writing, before the next start or beyond complete records. Advances
     * through expired records, ignores individual indices invalid in ctx, and skips
     * absolute flow below FLOW_TOL. Negative flows and nonfinite data are not rejected;
     * caller owns per-step accumulator reset and complete file-data qualification. */
    void applyFlows(SimulationContext& ctx, double current_date);

    /** @brief Append a native double date and float cfs values for selected node indices.
     * @param current_date Finite supported absolute SWMM/OLE start day, normally ordered
     * and separated by step(); no interval/date validation occurs here.
     * @param node_flows Borrowed project-indexed cfs vector. Missing/out-of-range indices
     * (including stored negative indices converted to size_t) write zero; values narrow
     * to float without finite/range checks.
     * No-op unless open for writing. fwrite failures are ignored, no explicit flush or
     * success status is supplied, and the caller serializes against close(). */
    void saveFlows(double current_date, const std::vector<double>& node_flows);

    /** @brief Close owned IO and clear writing/have-record flags.
     * Ignores fclose status; retains step, format, node mapping, flow buffers and dates.
     * Safe when already closed. Does not reset or clear any context inflow accumulator. */
    void close();

    /** @brief Test stream ownership without checking EOF or error state.
     * @return True exactly when fp_ is nonnull; not proof of a usable record. */
    bool isOpen() const { return fp_ != nullptr; }
    /** @brief Read the current mode flag without checking stream/error state.
     * @return Stored writing flag; close resets it to false. */
    bool isWriting() const { return writing_; }
    /** @brief Read the last stored record-validity duration.
     * @return Seconds from the successful/partial header or write setup; zero initially.
     * close() retains this value, so it does not prove an active/valid reader. */
    int step() const { return rdii_step_; }

private:
    std::FILE* fp_ = nullptr;
    bool writing_ = false;
    bool binary_ = true;

    int rdii_step_ = 0;         ///< Record validity interval (sec)
    int flow_units_ = 0;        ///< Text format: declared flow units
    std::vector<int> node_idx_; ///< Interface entry → project node
    std::vector<float> flows_;  ///< Current record flows (cfs)
    double start_date_ = 0.0;   ///< Current record start (OADate)
    double end_date_ = 0.0;     ///< Current record end (OADate)
    bool have_record_ = false;

    /** @brief Decode native binary header fields after the already consumed stamp.
     * @param ctx Compatible node count used to bound stored project indices.
     * @return Zero for positive step/count and fully read valid indices, -2 otherwise.
     * Requires owned readable binary fp_. Allocates declared count without a separate
     * cap, can leave partial metadata on failure, and does not validate identity,
     * duplicate indices, endianness or any record payload. Does not close the stream. */
    int readBinaryHeader(SimulationContext& ctx);
    /** @brief Parse text-interface metadata and map ordered node names to the model.
     * @param ctx Borrowed compatible node-name table.
     * @return Zero after a SWMM5-prefixed magic token, positive step/node count, recognized
     * uppercase flow-unit prefix, resolved names and skipped headings; -2 otherwise.
     * Requires owned readable stream at file start. Title/constituent count/column labels
     * are skipped without validation, and duplicate names are allowed. Partial fields
     * remain on rejection; does not close or read the first record. */
    int readTextHeader(SimulationContext& ctx);
    /** @brief Advance to one complete binary or per-node text record.
     * @return True after setting start/end/have_record; false if closed/writing or any
     * required read/scan fails. Clears have_record/dates first; a truncated record can
     * partially change flow storage. Text row names/date agreement are not checked;
     * last row's date is used and declared flow units convert to cfs. Binary values
     * already are cfs. No finite, date-order or record-step validation occurs, and EOF
     * is not distinguished from malformed/IO failure. */
    bool readNextRecord(); ///< Advance to next record (either format)
};

} // namespace rdii_iface
} // namespace openswmm

#endif // OPENSWMM_RDII_INTERFACE_HPP
