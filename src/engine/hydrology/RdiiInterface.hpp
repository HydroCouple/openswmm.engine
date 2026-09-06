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
 * @file RdiiInterface.hpp
 * @brief RDII interface file — use/save pre-computed RDII inflows.
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
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
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
 */
class RdiiInterfaceFile {
public:
    ~RdiiInterfaceFile() { close(); }

    /**
     * @brief Open an existing RDII file for reading (USE mode).
     * @details Auto-detects binary vs text format and parses the header.
     * @returns 0 on success, -1 cannot open, -2 invalid format /
     *          unresolvable node.
     */
    int openForRead(SimulationContext& ctx, const std::string& path);

    /**
     * @brief Open an RDII file for writing (SAVE mode, binary format).
     * @param rdii_step  Header time step in seconds (wet weather step).
     * @param node_idx   Node indices carrying RDII (RDIISolver::rdiiNodeList).
     * @returns 0 on success, -1 cannot open, -2 nothing to save (empty list).
     */
    int openForWrite(const std::string& path, int rdii_step,
                     const std::vector<int>& node_idx);

    /**
     * @brief USE mode: add the file's RDII flows for `current_date` to
     *        `ctx.nodes.rdii_inflow` (step-aligned, legacy semantics).
     */
    void applyFlows(SimulationContext& ctx, double current_date);

    /**
     * @brief SAVE mode: write one record (date + per-node flows in cfs).
     * @param node_flows  Per-node RDII flow array sized n_nodes
     *                    (RDIISolver::nodeFlows()).
     */
    void saveFlows(double current_date, const std::vector<double>& node_flows);

    void close();

    bool isOpen()    const { return fp_ != nullptr; }
    bool isWriting() const { return writing_; }
    int  step()      const { return rdii_step_; }

private:
    std::FILE* fp_ = nullptr;
    bool writing_ = false;
    bool binary_  = true;

    int rdii_step_  = 0;              ///< Record validity interval (sec)
    int flow_units_ = 0;              ///< Text format: declared flow units
    std::vector<int>   node_idx_;     ///< Interface entry → project node
    std::vector<float> flows_;        ///< Current record flows (cfs)
    double start_date_ = 0.0;         ///< Current record start (OADate)
    double end_date_   = 0.0;         ///< Current record end (OADate)
    bool   have_record_ = false;

    int  readBinaryHeader(SimulationContext& ctx);
    int  readTextHeader(SimulationContext& ctx);
    bool readNextRecord();            ///< Advance to next record (either format)
};

} // namespace rdii_iface
} // namespace openswmm

#endif // OPENSWMM_RDII_INTERFACE_HPP
