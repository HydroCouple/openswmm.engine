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
 * @file InterfaceFile.hpp
 * @brief Routing interface file — coupling between separate SWMM simulations.
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
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
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
    std::vector<int>    node_idx;       ///< Node indices in this model
    std::vector<double> old_values;     ///< Previous timestep values (flow + polluts per node)
    std::vector<double> new_values;     ///< Current timestep values (flow + polluts per node)

    void resize(int n);
};

// ============================================================================
// InterfaceManager — manages reading/writing of routing interface files
// ============================================================================

class InterfaceManager {
public:
    void init(SimulationContext& ctx);

    /// Open routing interface files for reading/writing
    int openFiles(const std::string& infile_path, const std::string& outfile_path);

    /// Read the inflows file header (flow units, pollutants, nodes).
    /// Called eagerly from SWMMEngine::start() after openFiles().
    int readFileHeader(SimulationContext& ctx);

    /// Write the outflows file header. Called from SWMMEngine::start().
    void writeFileHeader(const SimulationContext& ctx);

    /// Read next set of inflow values from upstream interface file
    void readInflows(SimulationContext& ctx, double current_time);

    /// Write current outfall results to downstream interface file
    void writeOutfallResults(const SimulationContext& ctx, double current_time);

    /// Close all interface files
    void closeFiles();

    /// Get interpolated interface flow for a node
    double getFlow(int node_idx, double frac) const;

    /// Get interpolated interface quality for a node and pollutant
    double getQuality(int node_idx, int pollut_idx, double frac) const;

    /// Get number of interface nodes currently active
    int getNumIfaceNodes() const { return data_.count; }

    /// Get the node index for a given interface file entry
    int getIfaceNode(int index) const;

    /// Get the interpolation fraction (set during readInflows)
    double getFrac() const { return iface_frac_; }

private:
    InterfaceFileSoA data_;
    FILE* infile_  = nullptr;
    FILE* outfile_ = nullptr;

    double old_time_ = 0.0;         ///< Previous interface file date (decimal days)
    double new_time_ = 0.0;         ///< Next interface file date (decimal days)
    int n_iface_polluts_ = 0;       ///< Number of pollutants on interface file
    int n_values_per_node_ = 1;     ///< 1 (flow) + n_iface_polluts_
    int iface_flow_units_ = 0;      ///< Flow units used in interface file
    int iface_step_ = 0;            ///< Reporting timestep from interface file (sec)
    double iface_frac_ = 0.0;       ///< Interpolation fraction between old/new times

    std::vector<int> pollut_map_;   ///< Map: project pollutant idx -> interface file col (-1 = none)

    /// Read next period of data from the interface file
    bool readNextPeriod();

    /// Copy new values to old values and advance time
    void setOldValues();

    /// Check if a node is an outlet (for writing)
    static bool isOutletNode(const SimulationContext& ctx, int node_idx);
};

} // namespace iface
} // namespace openswmm

#endif // OPENSWMM_INTERFACE_FILE_HPP
