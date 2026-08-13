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
 * @file DefaultOutputPlugin.cpp
 * @brief DefaultOutputPlugin — SWMM 5.x binary .out file writer.
 *
 * @details Writes the standard SWMM 5.x binary output format:
 *   - Header: magic, version, flow units, object counts
 *   - ID names section (subcatch, node, link, pollutant)
 *   - Pollutant concentration unit codes
 *   - Input data (subcatch areas, node inverts/depths, link offsets/lengths)
 *   - Variable codes (subcatch/node/link/system result variable IDs)
 *   - Report start date + report step
 *   - Per-period: date (8B) + subcatch results + node results + link results + system results
 *   - Footer: ID/Input/Output positions, Nperiods, error code, magic
 *
 * @see DefaultOutputPlugin.hpp
 * @ingroup engine_plugins
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "DefaultOutputPlugin.hpp"
#include "../../../include/openswmm/plugin_sdk/PluginState.hpp"
#include "../../../include/openswmm/plugin_sdk/SimulationSnapshot.hpp"
#include "../core/SimulationContext.hpp"
#include "../core/UnitConversion.hpp"

#include <cstring>
#include <cmath>

namespace openswmm {

DefaultOutputPlugin::DefaultOutputPlugin(std::string out_path)
    : out_path_(std::move(out_path))
    , state_(PluginState::LOADED)
{}

int DefaultOutputPlugin::initialize(const std::vector<std::string>& /*init_args*/,
                                    const IPluginComponentInfo* /*info*/) {
    step_count_ = 0;
    n_periods_ = 0;
    state_ = PluginState::INITIALIZED;
    return 0;
}

int DefaultOutputPlugin::validate(const SimulationContext& /*ctx*/) {
    state_ = PluginState::VALIDATED;
    return 0;
}

namespace {
/// Output-stream buffer size. 1 MB: large enough that a 500k-element
/// header is a handful of writes, small enough to be irrelevant next to
/// the model itself.
constexpr std::size_t kOutputBufferBytes = 1u << 20;
}  // namespace

int DefaultOutputPlugin::prepare(const SimulationContext& ctx) {
    // Open binary output file
    out_file_ = std::fopen(out_path_.c_str(), "w+b");
    if (!out_file_) {
        last_error_ = "Cannot open output file: " + out_path_;
        return -1;
    }

    // Every scalar in the header and in every report period is its own fwrite —
    // writeInt4/writeReal4/writeReal8 below — so a 500k-element model issues
    // millions of calls, each taking the FILE lock and testing the buffer. The
    // default buffer is a few kilobytes; a 1 MB one cuts the flush count by
    // orders of magnitude for the cost of one allocation. Must be set before
    // any I/O on the stream, which is why it sits immediately after fopen.
    //
    // Failure is not an error: setvbuf declining just leaves the default
    // buffer, and the file is still perfectly writable.
    std::setvbuf(out_file_, nullptr, _IOFBF, kOutputBufferBytes);

    // Per-timestep results arrive pre-converted to display units (engine
    // boundary). Only the static-object header below is written from the
    // SimulationContext (internal units), so retain just the length / land-area
    // factors it needs.
    flow_units_code_ = static_cast<int>(ctx.options.flow_units);
    ucf_length_      = ucf::UCF(ucf::LENGTH,   ctx.options);
    ucf_landarea_    = ucf::UCF(ucf::LANDAREA, ctx.options);

    // Write header
    writeHeader(ctx);

    step_count_ = 0;
    n_periods_ = 0;
    state_ = PluginState::PREPARED;
    return 0;
}

int DefaultOutputPlugin::update(const SimulationSnapshot& snapshot) {
    if (!out_file_) return -1;
    state_ = PluginState::UPDATING;

    // The snapshot arrives already in project display units (the engine applies
    // the single conversion boundary before posting), so values are written
    // directly. Dimensionless fields (soil moisture, capacity, pollutant
    // concentrations) need no conversion either.

    // Write date/time (8 bytes, REAL8 — SWMM DateTime)
    writeReal8(snapshot.sim_time);

    // -----------------------------------------------------------------------
    // Subcatchment results (matching legacy subcatch_getResults field order)
    // rainfall, snow_depth, evap, infil, runoff, gw_flow, gw_elev, soil_moist
    // + pollutant washoff
    // -----------------------------------------------------------------------
    for (int j = 0; j < snapshot.subcatch_count; ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (uj < subcatch_rpt_flag_.size() && !subcatch_rpt_flag_[uj]) continue;
        writeReal4(static_cast<float>(uj < snapshot.subcatch.rainfall.size()   ? snapshot.subcatch.rainfall[uj]   : 0.0)); // [0] RAINFALL
        writeReal4(static_cast<float>(uj < snapshot.subcatch.snow_depth.size() ? snapshot.subcatch.snow_depth[uj] : 0.0)); // [1] SNOWDEPTH
        writeReal4(static_cast<float>(uj < snapshot.subcatch.evap.size()       ? snapshot.subcatch.evap[uj]       : 0.0)); // [2] EVAP
        writeReal4(static_cast<float>(uj < snapshot.subcatch.infil.size()      ? snapshot.subcatch.infil[uj]      : 0.0)); // [3] INFIL
        writeReal4(static_cast<float>(uj < snapshot.subcatch.runoff.size()     ? snapshot.subcatch.runoff[uj]     : 0.0)); // [4] RUNOFF
        writeReal4(static_cast<float>(uj < snapshot.subcatch.gw_flow.size()    ? snapshot.subcatch.gw_flow[uj]    : 0.0)); // [5] GW_FLOW
        writeReal4(static_cast<float>(uj < snapshot.subcatch.gw_elev.size()    ? snapshot.subcatch.gw_elev[uj]    : 0.0)); // [6] GW_ELEV
        writeReal4(static_cast<float>(uj < snapshot.subcatch.soil_moist.size() ? snapshot.subcatch.soil_moist[uj] : 0.0)); // [7] SOIL_MOIST (dimensionless)
        // [8..] Pollutant washoff concentrations
        for (int p = 0; p < n_polluts_; ++p) {
            auto qi = uj * static_cast<std::size_t>(n_polluts_) + static_cast<std::size_t>(p);
            writeReal4(static_cast<float>(
                qi < snapshot.subcatch_quality.size() ? snapshot.subcatch_quality[qi] : 0.0));
        }
    }

    // -----------------------------------------------------------------------
    // Node results (matching legacy node_getResults field order)
    // depth, head, volume, lateral_inflow, total_inflow, overflow + pollutants
    // -----------------------------------------------------------------------
    for (int j = 0; j < snapshot.node_count; ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (uj < node_rpt_flag_.size() && !node_rpt_flag_[uj]) continue;
        writeReal4(static_cast<float>(uj < snapshot.nodes.depth.size()          ? snapshot.nodes.depth[uj]          : 0.0)); // [0] DEPTH
        writeReal4(static_cast<float>(uj < snapshot.nodes.head.size()           ? snapshot.nodes.head[uj]           : 0.0)); // [1] HEAD
        writeReal4(static_cast<float>(uj < snapshot.nodes.volume.size()         ? snapshot.nodes.volume[uj]         : 0.0)); // [2] VOLUME
        writeReal4(static_cast<float>(uj < snapshot.nodes.lateral_inflow.size() ? snapshot.nodes.lateral_inflow[uj] : 0.0)); // [3] LATFLOW
        writeReal4(static_cast<float>(uj < snapshot.nodes.total_inflow.size()   ? snapshot.nodes.total_inflow[uj]   : 0.0)); // [4] INFLOW
        writeReal4(static_cast<float>(uj < snapshot.nodes.overflow.size()       ? snapshot.nodes.overflow[uj]       : 0.0)); // [5] OVERFLOW
        // [6..] Pollutant concentrations
        for (int p = 0; p < n_polluts_; ++p) {
            auto qi = uj * static_cast<std::size_t>(n_polluts_) + static_cast<std::size_t>(p);
            writeReal4(static_cast<float>(
                qi < snapshot.node_quality.size() ? snapshot.node_quality[qi] : 0.0));
        }
    }

    // -----------------------------------------------------------------------
    // Link results (matching legacy link_getResults field order)
    // flow, depth, velocity, volume, capacity + pollutants
    // Direction already applied in snapshot builder.
    // -----------------------------------------------------------------------
    for (int j = 0; j < snapshot.link_count; ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (uj < link_rpt_flag_.size() && !link_rpt_flag_[uj]) continue;
        writeReal4(static_cast<float>(uj < snapshot.links.flow.size()     ? snapshot.links.flow[uj]     : 0.0)); // [0] FLOW (direction pre-applied)
        writeReal4(static_cast<float>(uj < snapshot.links.depth.size()    ? snapshot.links.depth[uj]    : 0.0)); // [1] DEPTH
        writeReal4(static_cast<float>(uj < snapshot.links.velocity.size() ? snapshot.links.velocity[uj] : 0.0)); // [2] VELOCITY (direction pre-applied)
        writeReal4(static_cast<float>(uj < snapshot.links.volume.size()   ? snapshot.links.volume[uj]   : 0.0)); // [3] VOLUME
        writeReal4(static_cast<float>(uj < snapshot.links.capacity.size() ? snapshot.links.capacity[uj] : 0.0)); // [4] CAPACITY (dimensionless)
        // [5..] Pollutant concentrations
        for (int p = 0; p < n_polluts_; ++p) {
            auto qi = uj * static_cast<std::size_t>(n_polluts_) + static_cast<std::size_t>(p);
            writeReal4(static_cast<float>(
                qi < snapshot.link_quality.size() ? snapshot.link_quality[qi] : 0.0));
        }
    }

    // -----------------------------------------------------------------------
    // System results (15 REAL4 values, matching legacy SysResults order).
    // All already in display units (temperature converted in the boundary too).
    // -----------------------------------------------------------------------
    float sys[MAX_SYS_RESULTS] = {};
    sys[0]  = static_cast<float>(snapshot.sys_temperature); // SYS_TEMPERATURE
    sys[1]  = static_cast<float>(snapshot.sys_rainfall);    // SYS_RAINFALL
    sys[2]  = static_cast<float>(snapshot.sys_snow_depth);  // SYS_SNOWDEPTH
    sys[3]  = static_cast<float>(snapshot.sys_infil);       // SYS_INFIL
    sys[4]  = static_cast<float>(snapshot.sys_runoff);      // SYS_RUNOFF
    sys[5]  = static_cast<float>(snapshot.sys_dw_inflow);   // SYS_DWFLOW
    sys[6]  = static_cast<float>(snapshot.sys_gw_inflow);   // SYS_GWFLOW
    sys[7]  = static_cast<float>(snapshot.sys_ii_inflow);   // SYS_IIFLOW
    sys[8]  = static_cast<float>(snapshot.sys_ext_inflow);  // SYS_EXFLOW
    sys[9]  = sys[4] + sys[5] + sys[6] + sys[7] + sys[8];   // SYS_INFLOW (sum)
    sys[10] = static_cast<float>(snapshot.sys_flooding);    // SYS_FLOODING
    sys[11] = static_cast<float>(snapshot.sys_outflow);     // SYS_OUTFLOW
    sys[12] = static_cast<float>(snapshot.sys_storage);     // SYS_STORAGE
    sys[13] = static_cast<float>(snapshot.sys_evap);        // SYS_EVAP
    sys[14] = static_cast<float>(snapshot.sys_pet);         // SYS_PET

    std::fwrite(sys, sizeof(float), MAX_SYS_RESULTS, out_file_);

    ++n_periods_;
    ++step_count_;
    return 0;
}

int DefaultOutputPlugin::finalize(const SimulationContext& /*ctx*/) {
    if (!out_file_) {
        state_ = PluginState::FINALIZED;
        return 0;
    }

    // Write footer
    writeInt4(static_cast<int>(id_start_pos_));
    writeInt4(static_cast<int>(input_start_pos_));
    writeInt4(static_cast<int>(output_start_pos_));
    writeInt4(n_periods_);
    writeInt4(0);  // error code
    writeInt4(MAGIC_NUMBER);

    std::fclose(out_file_);
    out_file_ = nullptr;
    state_ = PluginState::FINALIZED;
    return 0;
}

// ============================================================================
// Header writer — matching legacy output_open() layout
// ============================================================================

void DefaultOutputPlugin::writeHeader(const SimulationContext& ctx) {
    // Cache per-object report flags
    subcatch_rpt_flag_ = ctx.subcatches.rpt_flag;
    node_rpt_flag_     = ctx.nodes.rpt_flag;
    link_rpt_flag_     = ctx.links.rpt_flag;

    // Count only objects with rpt_flag set (matches legacy output.c:169-171)
    n_subcatch_ = 0;
    for (const auto& f : subcatch_rpt_flag_) if (f) ++n_subcatch_;
    n_nodes_ = 0;
    for (const auto& f : node_rpt_flag_) if (f) ++n_nodes_;
    n_links_ = 0;
    for (const auto& f : link_rpt_flag_) if (f) ++n_links_;
    // IGNORE_QUALITY: drop all pollutant columns from the binary .out file so
    // OutputReader reads zero pollutants (legacy output.c:150 NumPolluts = 0).
    n_polluts_ = ctx.options.ignore_quality ? 0 : ctx.n_pollutants();

    n_subcatch_vars_ = 8 + n_polluts_;  // rainfall..soilmoist + pollutants
    n_node_vars_ = 6 + n_polluts_;      // depth..overflow + pollutants
    n_link_vars_ = 5 + n_polluts_;      // flow..capacity + pollutants

    // Magic, version, flow units, counts
    writeInt4(MAGIC_NUMBER);
    writeInt4(VERSION);
    writeInt4(flow_units_code_);
    writeInt4(n_subcatch_);
    writeInt4(n_nodes_);
    writeInt4(n_links_);
    writeInt4(n_polluts_);

    // ID names: subcatch, node, link, pollutant (only flagged objects)
    id_start_pos_ = std::ftell(out_file_);
    for (int j = 0; j < ctx.n_subcatches(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (uj < subcatch_rpt_flag_.size() && subcatch_rpt_flag_[uj])
            writeID(ctx.subcatch_names.name_of(j).c_str());
    }
    for (int j = 0; j < ctx.n_nodes(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (uj < node_rpt_flag_.size() && node_rpt_flag_[uj])
            writeID(ctx.node_names.name_of(j).c_str());
    }
    for (int j = 0; j < ctx.n_links(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (uj < link_rpt_flag_.size() && link_rpt_flag_[uj])
            writeID(ctx.link_names.name_of(j).c_str());
    }
    for (int p = 0; p < n_polluts_; ++p)
        writeID(ctx.pollutant_names.name_of(p).c_str());

    // Pollutant concentration unit codes
    for (int p = 0; p < n_polluts_; ++p) {
        writeInt4(static_cast<int>(ctx.pollutants.units[static_cast<std::size_t>(p)]));
    }

    // Input data start
    input_start_pos_ = std::ftell(out_file_);

    // Subcatchment input: area (converting to display units)
    writeInt4(1);
    writeInt4(1);  // INPUT_AREA code
    for (int j = 0; j < ctx.n_subcatches(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (uj >= subcatch_rpt_flag_.size() || !subcatch_rpt_flag_[uj]) continue;
        writeReal4(static_cast<float>(
            ctx.subcatches.area[uj] * ucf_landarea_));
    }

    // Node input: type, invert, max depth
    writeInt4(3);
    writeInt4(0); writeInt4(2); writeInt4(4);
    for (int j = 0; j < ctx.n_nodes(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (uj >= node_rpt_flag_.size() || !node_rpt_flag_[uj]) continue;
        writeInt4(static_cast<int>(ctx.nodes.type[uj]));
        writeReal4(static_cast<float>(ctx.nodes.invert_elev[uj] * ucf_length_));
        writeReal4(static_cast<float>(ctx.nodes.full_depth[uj] * ucf_length_));
    }

    // Link input: type, offset1, offset2, max depth, length
    // Special handling for pumps and outlets (matching legacy)
    writeInt4(5);
    writeInt4(0); writeInt4(6); writeInt4(7); writeInt4(4); writeInt4(8);
    for (int j = 0; j < ctx.n_links(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (uj >= link_rpt_flag_.size() || !link_rpt_flag_[uj]) continue;
        auto lt = ctx.links.type[uj];
        writeInt4(static_cast<int>(lt));

        if (lt == LinkType::PUMP) {
            // Pumps: all zeros (matching legacy)
            writeReal4(0.0f);
            writeReal4(0.0f);
            writeReal4(0.0f);
            writeReal4(0.0f);
        } else {
            // Offsets with direction swap (matching legacy)
            double off1 = ctx.links.offset1[uj] * ucf_length_;
            double off2 = ctx.links.offset2[uj] * ucf_length_;
            if (ctx.links.direction[uj] < 0)
                std::swap(off1, off2);
            writeReal4(static_cast<float>(off1));
            writeReal4(static_cast<float>(off2));

            // Max depth (outlet = 0)
            if (lt == LinkType::OUTLET)
                writeReal4(0.0f);
            else
                writeReal4(static_cast<float>(ctx.links.xsect_y_full[uj] * ucf_length_));

            // Length (conduit only)
            if (lt == LinkType::CONDUIT) {
                const int cr = ctx.link_subtypes.conduit_row(j);
                const double len = (cr >= 0) ? ctx.link_subtypes.conduits.length[static_cast<size_t>(cr)] : 0.0;
                writeReal4(static_cast<float>(len * ucf_length_));
            } else
                writeReal4(0.0f);
        }
    }

    // Variable codes
    writeInt4(n_subcatch_vars_);
    for (int v = 0; v < n_subcatch_vars_; ++v) writeInt4(v);

    writeInt4(n_node_vars_);
    for (int v = 0; v < n_node_vars_; ++v) writeInt4(v);

    writeInt4(n_link_vars_);
    for (int v = 0; v < n_link_vars_; ++v) writeInt4(v);

    writeInt4(MAX_SYS_RESULTS);
    for (int v = 0; v < MAX_SYS_RESULTS; ++v) writeInt4(v);

    // Report start date and step
    writeReal8(ctx.options.start_date);
    writeInt4(static_cast<int>(ctx.options.report_step));

    output_start_pos_ = std::ftell(out_file_);
}

// ============================================================================
// Binary write helpers
// ============================================================================

void DefaultOutputPlugin::writeID(const char* id) {
    int len = static_cast<int>(std::strlen(id));
    writeInt4(len);
    std::fwrite(id, 1, static_cast<std::size_t>(len), out_file_);
}

void DefaultOutputPlugin::writeInt4(int value) {
    std::fwrite(&value, sizeof(int), 1, out_file_);
}

void DefaultOutputPlugin::writeReal4(float value) {
    std::fwrite(&value, sizeof(float), 1, out_file_);
}

void DefaultOutputPlugin::writeReal8(double value) {
    std::fwrite(&value, sizeof(double), 1, out_file_);
}

} /* namespace openswmm */
