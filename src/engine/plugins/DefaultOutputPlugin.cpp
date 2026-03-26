/**
 * @file DefaultOutputPlugin.cpp
 * @brief DefaultOutputPlugin — SWMM 5.x binary .out file writer.
 *
 * @details Writes the standard SWMM 5.x binary output format:
 *   - Header: magic, version, flow units, object counts
 *   - ID names section
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
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "DefaultOutputPlugin.hpp"
#include "../../../include/openswmm/plugin_sdk/PluginState.hpp"
#include "../../../include/openswmm/plugin_sdk/SimulationSnapshot.hpp"
#include "../core/SimulationContext.hpp"

#include <cstring>

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

int DefaultOutputPlugin::prepare(const SimulationContext& ctx) {
    // Open binary output file
    out_file_ = std::fopen(out_path_.c_str(), "w+b");
    if (!out_file_) {
        last_error_ = "Cannot open output file: " + out_path_;
        return -1;
    }

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

    // Write date (8 bytes, double)
    writeReal8(snapshot.sim_time);

    // Write subcatchment results
    for (int j = 0; j < n_subcatch_; ++j) {
        auto uj = static_cast<std::size_t>(j);
        writeReal4(uj < snapshot.subcatch.runoff.size() ? static_cast<float>(snapshot.subcatch.runoff[uj]) : 0.0f);
        for (int v = 1; v < n_subcatch_vars_; ++v) writeReal4(0.0f);
    }

    // Write node results
    for (int j = 0; j < n_nodes_; ++j) {
        auto uj = static_cast<std::size_t>(j);
        writeReal4(uj < snapshot.nodes.depth.size() ? static_cast<float>(snapshot.nodes.depth[uj]) : 0.0f);
        writeReal4(uj < snapshot.nodes.head.size() ? static_cast<float>(snapshot.nodes.head[uj]) : 0.0f);
        writeReal4(uj < snapshot.nodes.volume.size() ? static_cast<float>(snapshot.nodes.volume[uj]) : 0.0f);
        writeReal4(uj < snapshot.nodes.lateral_inflow.size() ? static_cast<float>(snapshot.nodes.lateral_inflow[uj]) : 0.0f);
        writeReal4(0.0f);  // total inflow
        writeReal4(uj < snapshot.nodes.overflow.size() ? static_cast<float>(snapshot.nodes.overflow[uj]) : 0.0f);
        for (int v = 6; v < n_node_vars_; ++v) writeReal4(0.0f);
    }

    // Write link results
    for (int j = 0; j < n_links_; ++j) {
        auto uj = static_cast<std::size_t>(j);
        writeReal4(uj < snapshot.links.flow.size() ? static_cast<float>(snapshot.links.flow[uj]) : 0.0f);
        writeReal4(uj < snapshot.links.depth.size() ? static_cast<float>(snapshot.links.depth[uj]) : 0.0f);
        writeReal4(uj < snapshot.links.velocity.size() ? static_cast<float>(snapshot.links.velocity[uj]) : 0.0f);
        writeReal4(0.0f);  // volume
        writeReal4(uj < snapshot.links.capacity.size() ? static_cast<float>(snapshot.links.capacity[uj]) : 0.0f);
        for (int v = 5; v < n_link_vars_; ++v) writeReal4(0.0f);
    }

    // Write system results
    float sys[MAX_SYS_RESULTS] = {};
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
// Header writer
// ============================================================================

void DefaultOutputPlugin::writeHeader(const SimulationContext& ctx) {
    n_subcatch_ = ctx.n_subcatches();
    n_nodes_ = ctx.n_nodes();
    n_links_ = ctx.n_links();
    n_polluts_ = ctx.n_pollutants();  // Pollutant count from context

    n_subcatch_vars_ = 8 + n_polluts_;  // rainfall..soilmoist + pollutants
    n_node_vars_ = 6 + n_polluts_;      // depth..overflow + pollutants
    n_link_vars_ = 5 + n_polluts_;      // flow..capacity + pollutants

    // Magic, version, flow units, counts
    writeInt4(MAGIC_NUMBER);
    writeInt4(VERSION);
    writeInt4(0);  // flow units (CFS=0)
    writeInt4(n_subcatch_);
    writeInt4(n_nodes_);
    writeInt4(n_links_);
    writeInt4(n_polluts_);

    // ID names
    id_start_pos_ = std::ftell(out_file_);
    for (int j = 0; j < n_subcatch_; ++j)
        writeID(ctx.subcatch_names.name_of(j).c_str());
    for (int j = 0; j < n_nodes_; ++j)
        writeID(ctx.node_names.name_of(j).c_str());
    for (int j = 0; j < n_links_; ++j)
        writeID(ctx.link_names.name_of(j).c_str());

    // Pollutant concentration units (none for now)

    // Input data start
    input_start_pos_ = std::ftell(out_file_);

    // Subcatchment input: area
    writeInt4(1);  // 1 input property
    writeInt4(1);  // INPUT_AREA code
    for (int j = 0; j < n_subcatch_; ++j) {
        writeReal4(static_cast<float>(ctx.subcatches.area[static_cast<std::size_t>(j)]));
    }

    // Node input: type, invert, max depth
    writeInt4(3);
    writeInt4(0); writeInt4(2); writeInt4(4);  // type, invert, max_depth codes
    for (int j = 0; j < n_nodes_; ++j) {
        auto uj = static_cast<std::size_t>(j);
        writeInt4(static_cast<int>(ctx.nodes.type[uj]));
        writeReal4(static_cast<float>(ctx.nodes.invert_elev[uj]));
        writeReal4(static_cast<float>(ctx.nodes.full_depth[uj]));
    }

    // Link input: type, offset1, offset2, max depth, length
    writeInt4(5);
    writeInt4(0); writeInt4(6); writeInt4(7); writeInt4(4); writeInt4(8);
    for (int j = 0; j < n_links_; ++j) {
        auto uj = static_cast<std::size_t>(j);
        writeInt4(static_cast<int>(ctx.links.type[uj]));
        writeReal4(static_cast<float>(ctx.links.offset1[uj]));
        writeReal4(static_cast<float>(ctx.links.offset2[uj]));
        writeReal4(static_cast<float>(ctx.links.xsect_y_full[uj]));
        writeReal4(static_cast<float>(ctx.links.length[uj]));
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
