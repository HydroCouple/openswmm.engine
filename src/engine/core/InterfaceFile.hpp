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
 * @file      InterfaceFile.hpp
 * @brief     Routing interface file — coupling between separate SWMM simulations.
 *
 * @details An upstream model writes outfall results to a text interface file;
 *          a downstream model reads those results as inflow boundary conditions.
 *          Temporal interpolation between interface file periods ensures smooth
 *          coupling even when the two models use different timesteps.
 *
 *          SoA storage: per-node flows and quality values stored in flat arrays
 *          for vectorisable interpolation.
 *
 * @note Legacy reference: src/legacy/engine/iface.c
 * @ingroup engine_core
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_INTERFACE_FILE_HPP
#define OPENSWMM_INTERFACE_FILE_HPP

#include <vector>
#include <string>
#include <cstdio>

namespace openswmm {

struct SimulationContext;

namespace iface {

// ============================================================================
// SoA for interface node data (flows + quality per node)
// ============================================================================

struct InterfaceFileSoA {
    int count = 0;
    std::vector<int> node_idx;      ///< Node indices in this model
    std::vector<double> old_values; ///< Previous timestep values (flow + polluts per node)
    std::vector<double> new_values; ///< Current timestep values (flow + polluts per node)

    /**
     * @brief Replace interface-node mappings while retaining value arrays.
     *
     * Initializes node_idx=-1. Caller separately sizes old/new values to count*(1+pollutants); does not
     * clear their previous contents. Allocation can throw after count changes.
     *
     * @param[in] n Requested entry count; stored verbatim in count, but negative allocates zero node
     *     mappings. Caller should supply nonnegative input.
     */
    void resize(int n);
};

// ============================================================================
// InterfaceManager — manages reading/writing of routing interface files
// ============================================================================

class InterfaceManager {
public:
    /**
     * @brief Close prior files and reset per-run interpolation/mapping metadata.
     *
     * Sets active count zero, times/fraction zero and pollutant stride one; clears pollutant mapping
     * but retains SoA vector contents and the prior flow-unit/report-step fields until a new header is
     * parsed. Call before openFiles when reusing a manager.
     *
     * @param[in,out] ctx Context argument retained for the interface; current implementation does not
     *     read or modify it.
     */
    void init(SimulationContext& ctx);

    /**
     * @brief Open optional text routing interface streams.
     *
     * Output opens first and remains open if input later fails. Caller closes existing streams before
     * reuse and calls closeFiles on failure/end; no destructor cleanup is declared. Path aliases are
     * not checked for file identity.
     *
     * @param[in] infile_path UTF-8 input path; empty disables opening input.
     * @param[in] outfile_path UTF-8 output path; empty disables opening output. Existing file is
     *     truncated.
     * @return 0 on success; -1 for exactly equal nonempty path strings, -2 for output-open failure, -3
     *     for input-open failure.
     */
    int openFiles(const std::string& infile_path, const std::string& outfile_path);

    /**
     * @brief Read interface units, constituent mapping, nodes and the first data period.
     *
     * Populates manager incrementally without rollback; unmatched node IDs map to -1, unmatched
     * pollutants remain absent. Unknown flow-unit text falls back to CFS; pollutant unit labels are
     * read but not converted. First-period parse failure is ignored by this method, so success is not
     * complete data validation.
     *
     * @param[in,out] ctx Borrowed model names/counts for lookup; model data is not changed by this
     *     header parser.
     * @return 0 after a syntactically accepted header; -1 for no input or header read/format refusal.
     */
    int readFileHeader(SimulationContext& ctx);

    /**
     * @brief Write the routing interface header for current outlet nodes.
     *
     * No-op without output stream. Lists FLOW in project display units, all pollutant labels as MG/L
     * regardless of their configured units, then selected outlets. Uses printf IO without
     * returning/checking write errors; does not flush.
     *
     * @param[in] ctx Borrowed aligned model options/names/type/degree arrays; report_step must be
     *     int-representable seconds.
     */
    void writeFileHeader(const SimulationContext& ctx);

    /**
     * @brief Advance file periods and add interpolated flow/quality into model source arrays.
     *
     * Lazily reads header if old_values is empty; header refusal closes both streams. Skips flows below
     * FLOW_TOL and adds quality load only for positive flow and positive interpolated concentration.
     * Input flow is converted to cfs; quality stays in file concentration units. No rollback on partial
     * data parsing; no error code or clock advancement.
     *
     * @param[in,out] ctx Aligned model source arrays; caller clears per-step iface accumulators before
     *     this additive load.
     * @param[in] current_time Absolute OADate days, expected nondecreasing; not elapsed simulation
     *     seconds.
     */
    void readInflows(SimulationContext& ctx, double current_time);

    /**
     * @brief Append one period of selected outlet flow and pollutant results.
     *
     * No-op without output stream. Writes node inflow converted from cfs to project flow units;
     * unavailable concentration entries become zero. Uses current concentrations as stored, does not
     * interpolate to report time or check/flush write errors.
     *
     * @param[in] ctx Borrowed model with aligned node names/inflow/type/degree arrays and optional
     *     pollutant concentration storage.
     * @param[in] current_time Absolute OADate timestamp decoded without timezone conversion.
     */
    void writeOutfallResults(const SimulationContext& ctx, double current_time);

    /**
     * @brief Close both owned interface streams if open.
     *
     * Sets handles NULL and ignores fclose results. Preserves mappings, interpolation values and times;
     * call init for a per-run metadata reset.
     */
    void closeFiles();

    /**
     * @brief Interpolate flow for an interface-entry index.
     *
     * Requires old/new arrays sized to parsed stride. Does not read more data or verify temporal
     * bracketing.
     *
     * @param[in] node_idx Interface file entry index, not the project node index; outside
     *     [0,getNumIfaceNodes()) returns zero.
     * @param[in] frac Interpolation fraction; normally getFrac() in [0,1], but this method does not
     *     clamp.
     * @return (1-frac)*old+frac*new in internal cfs; out-of-range entry returns zero.
     */
    double getFlow(int node_idx, double frac) const;

    /**
     * @brief Interpolate a mapped pollutant concentration for an interface entry.
     *
     * Caller ensures parsed value arrays/mapping are coherent. No pollutant unit conversion or
     * concentration-range check.
     *
     * @param[in] node_idx Interface entry index, not project node index.
     * @param[in] pollut_idx Project pollutant index; absent or out-of-range mapping returns zero.
     * @param[in] frac Interpolation fraction, passed through without clamping.
     * @return Stored-file-unit concentration, or zero for invalid entry/missing pollutant.
     */
    double getQuality(int node_idx, int pollut_idx, double frac) const;

    /// Get number of interface nodes currently active
    int getNumIfaceNodes() const { return data_.count; }

    /**
     * @brief Translate an interface file entry into a project node index.
     *
     * @param[in] index Zero-based interface entry index.
     * @return Mapped project node index, or -1 for out-of-range entry or unresolved node name.
     */
    int getIfaceNode(int index) const;

    /// Get the interpolation fraction (set during readInflows)
    double getFrac() const { return iface_frac_; }

private:
    InterfaceFileSoA data_;
    FILE* infile_ = nullptr;
    FILE* outfile_ = nullptr;

    double old_time_ = 0.0;     ///< Previous interface file date (decimal days)
    double new_time_ = 0.0;     ///< Next interface file date (decimal days)
    int n_iface_polluts_ = 0;   ///< Number of pollutants on interface file
    int n_values_per_node_ = 1; ///< 1 (flow) + n_iface_polluts_
    int iface_flow_units_ = 0;  ///< Flow units used in interface file
    int iface_step_ = 0;        ///< Reporting timestep from interface file (sec)
    double iface_frac_ = 0.0;   ///< Interpolation fraction between old/new times

    std::vector<int> pollut_map_; ///< Map: project pollutant idx -> interface file col (-1 = none)

    /**
     * @brief Parse one row for every interface node from the current input position.
     *
     * Changes new_values/new_time incrementally and can leave partial rows on failure; timestamps are
     * taken from the final row without cross-row checks. Row node-name tokens are ignored and row order
     * is trusted. Numeric atof/atoi parsing is permissive. Uses strtok on non-MSVC builds, so separate
     * managers require external synchronization during parsing.
     * @return True if every row parses; false for no stream, EOF or missing token.
     */
    bool readNextPeriod();

    /**
     * @brief Copy the complete current period into the previous interpolation slot.
     *
     * Requires allocated old/new arrays for count*n_values_per_node. Sets old_time=new_time and copies
     * values; does not read a new period or change fraction.
     */
    void setOldValues();

    /**
     * @brief Apply routing-method-specific outlet selection.
     *
     * @param[in] ctx Borrowed aligned node type/degree arrays and routing method.
     * @param[in] node_idx Existing project node index; unchecked.
     * @return True only for OUTFALL under DYNWAVE/FV, otherwise degree==0 for downstream-only routing.
     */
    static bool isOutletNode(const SimulationContext& ctx, int node_idx);
};

} // namespace iface
} // namespace openswmm

#endif // OPENSWMM_INTERFACE_FILE_HPP
