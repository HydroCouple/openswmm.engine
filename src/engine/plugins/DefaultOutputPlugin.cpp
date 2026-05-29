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
 * @license  MIT License
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

int DefaultOutputPlugin::prepare(const SimulationContext& ctx) {
    // Open binary output file
    out_file_ = std::fopen(out_path_.c_str(), "w+b");
    if (!out_file_) {
        last_error_ = "Cannot open output file: " + out_path_;
        return -1;
    }

    // Store flow units code and compute UCF factors once
    flow_units_code_ = static_cast<int>(ctx.options.flow_units);
    unit_system_     = ucf::getUnitSystem(flow_units_code_);
    ucf_rainfall_    = ucf::UCF(ucf::RAINFALL,  ctx.options);
    ucf_raindepth_   = ucf::UCF(ucf::RAINDEPTH, ctx.options);
    ucf_evaprate_    = ucf::UCF(ucf::EVAPRATE,  ctx.options);
    ucf_length_      = ucf::UCF(ucf::LENGTH,    ctx.options);
    ucf_landarea_    = ucf::UCF(ucf::LANDAREA,   ctx.options);
    ucf_volume_      = ucf::UCF(ucf::VOLUME,    ctx.options);
    ucf_flow_        = ucf::UCF(ucf::FLOW,      ctx.options);

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
        // [0] SUBCATCH_RAINFALL — internal ft/sec → display
        writeReal4(static_cast<float>(
            (uj < snapshot.subcatch.rainfall.size()
                ? snapshot.subcatch.rainfall[uj] : 0.0) * ucf_rainfall_));
        // [1] SUBCATCH_SNOWDEPTH — internal ft → display
        writeReal4(static_cast<float>(
            (uj < snapshot.subcatch.snow_depth.size()
                ? snapshot.subcatch.snow_depth[uj] : 0.0) * ucf_raindepth_));
        // [2] SUBCATCH_EVAP — internal ft/sec → display
        writeReal4(static_cast<float>(
            (uj < snapshot.subcatch.evap.size()
                ? snapshot.subcatch.evap[uj] : 0.0) * ucf_evaprate_));
        // [3] SUBCATCH_INFIL — internal ft/sec → display
        writeReal4(static_cast<float>(
            (uj < snapshot.subcatch.infil.size()
                ? snapshot.subcatch.infil[uj] : 0.0) * ucf_rainfall_));
        // [4] SUBCATCH_RUNOFF — internal cfs → display flow
        writeReal4(static_cast<float>(
            (uj < snapshot.subcatch.runoff.size()
                ? snapshot.subcatch.runoff[uj] : 0.0) * ucf_flow_));
        // [5] SUBCATCH_GW_FLOW — internal cfs → display flow
        writeReal4(static_cast<float>(
            (uj < snapshot.subcatch.gw_flow.size()
                ? snapshot.subcatch.gw_flow[uj] : 0.0) * ucf_flow_));
        // [6] SUBCATCH_GW_ELEV — internal ft → display length
        writeReal4(static_cast<float>(
            (uj < snapshot.subcatch.gw_elev.size()
                ? snapshot.subcatch.gw_elev[uj] : 0.0) * ucf_length_));
        // [7] SUBCATCH_SOIL_MOIST — dimensionless
        writeReal4(static_cast<float>(
            uj < snapshot.subcatch.soil_moist.size()
                ? snapshot.subcatch.soil_moist[uj] : 0.0));
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
        // [0] NODE_DEPTH — internal ft → display length
        double depth_display = (uj < snapshot.nodes.depth.size()
            ? snapshot.nodes.depth[uj] : 0.0) * ucf_length_;
        writeReal4(static_cast<float>(depth_display));
        // [1] NODE_HEAD — internal ft → display length
        writeReal4(static_cast<float>(
            (uj < snapshot.nodes.head.size()
                ? snapshot.nodes.head[uj] : 0.0) * ucf_length_));
        // [2] NODE_VOLUME — internal ft³ → display volume
        writeReal4(static_cast<float>(
            (uj < snapshot.nodes.volume.size()
                ? snapshot.nodes.volume[uj] : 0.0) * ucf_volume_));
        // [3] NODE_LATFLOW — internal cfs → display flow
        writeReal4(static_cast<float>(
            (uj < snapshot.nodes.lateral_inflow.size()
                ? snapshot.nodes.lateral_inflow[uj] : 0.0) * ucf_flow_));
        // [4] NODE_INFLOW — internal cfs → display flow
        writeReal4(static_cast<float>(
            (uj < snapshot.nodes.total_inflow.size()
                ? snapshot.nodes.total_inflow[uj] : 0.0) * ucf_flow_));
        // [5] NODE_OVERFLOW — internal cfs → display flow
        writeReal4(static_cast<float>(
            (uj < snapshot.nodes.overflow.size()
                ? snapshot.nodes.overflow[uj] : 0.0) * ucf_flow_));
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
        // [0] LINK_FLOW — internal cfs → display flow (direction pre-applied)
        writeReal4(static_cast<float>(
            (uj < snapshot.links.flow.size()
                ? snapshot.links.flow[uj] : 0.0) * ucf_flow_));
        // [1] LINK_DEPTH — internal ft → display length
        writeReal4(static_cast<float>(
            (uj < snapshot.links.depth.size()
                ? snapshot.links.depth[uj] : 0.0) * ucf_length_));
        // [2] LINK_VELOCITY — internal ft/sec → display length/sec (direction pre-applied)
        writeReal4(static_cast<float>(
            (uj < snapshot.links.velocity.size()
                ? snapshot.links.velocity[uj] : 0.0) * ucf_length_));
        // [3] LINK_VOLUME — internal ft³ → display volume
        writeReal4(static_cast<float>(
            (uj < snapshot.links.volume.size()
                ? snapshot.links.volume[uj] : 0.0) * ucf_volume_));
        // [4] LINK_CAPACITY — dimensionless [0, 1]
        writeReal4(static_cast<float>(
            uj < snapshot.links.capacity.size()
                ? snapshot.links.capacity[uj] : 0.0));
        // [5..] Pollutant concentrations
        for (int p = 0; p < n_polluts_; ++p) {
            auto qi = uj * static_cast<std::size_t>(n_polluts_) + static_cast<std::size_t>(p);
            writeReal4(static_cast<float>(
                qi < snapshot.link_quality.size() ? snapshot.link_quality[qi] : 0.0));
        }
    }

    // -----------------------------------------------------------------------
    // System results (15 REAL4 values, matching legacy SysResults order)
    // -----------------------------------------------------------------------
    float sys[MAX_SYS_RESULTS] = {};

    // [0] SYS_TEMPERATURE — °F → display (°F for US, °C for SI)
    if (unit_system_ == 0)
        sys[0] = static_cast<float>(snapshot.sys_temperature);
    else
        sys[0] = static_cast<float>((5.0 / 9.0) * (snapshot.sys_temperature - 32.0));

    // [1] SYS_RAINFALL — area-weighted avg ft/sec → display rate
    sys[1] = static_cast<float>(snapshot.sys_rainfall * ucf_rainfall_);
    // [2] SYS_SNOWDEPTH — area-weighted avg ft → display depth
    sys[2] = static_cast<float>(snapshot.sys_snow_depth * ucf_raindepth_);
    // [3] SYS_INFIL — area-weighted avg ft/sec → display rate
    sys[3] = static_cast<float>(snapshot.sys_infil * ucf_rainfall_);
    // [4] SYS_RUNOFF — total cfs → display flow
    sys[4] = static_cast<float>(snapshot.sys_runoff * ucf_flow_);
    // [5] SYS_DWFLOW — cfs → display flow
    sys[5] = static_cast<float>(snapshot.sys_dw_inflow * ucf_flow_);
    // [6] SYS_GWFLOW — cfs → display flow
    sys[6] = static_cast<float>(snapshot.sys_gw_inflow * ucf_flow_);
    // [7] SYS_IIFLOW — RDII, cfs → display flow
    sys[7] = static_cast<float>(snapshot.sys_ii_inflow * ucf_flow_);
    // [8] SYS_EXFLOW — external inflow, cfs → display flow
    sys[8] = static_cast<float>(snapshot.sys_ext_inflow * ucf_flow_);
    // [9] SYS_INFLOW — total lateral inflow = runoff + dw + gw + ii + ext
    sys[9] = sys[4] + sys[5] + sys[6] + sys[7] + sys[8];
    // [10] SYS_FLOODING — cfs → display flow
    sys[10] = static_cast<float>(snapshot.sys_flooding * ucf_flow_);
    // [11] SYS_OUTFLOW — cfs → display flow
    sys[11] = static_cast<float>(snapshot.sys_outflow * ucf_flow_);
    // [12] SYS_STORAGE — total ft³ → display volume
    sys[12] = static_cast<float>(snapshot.sys_storage * ucf_volume_);
    // [13] SYS_EVAP — area-weighted avg ft/sec → display rate
    sys[13] = static_cast<float>(snapshot.sys_evap * ucf_evaprate_);
    // [14] SYS_PET — ft/sec → display evap rate
    sys[14] = static_cast<float>(snapshot.sys_pet * ucf_evaprate_);

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
    n_polluts_ = ctx.n_pollutants();

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
            if (lt == LinkType::CONDUIT)
                writeReal4(static_cast<float>(ctx.links.length[uj] * ucf_length_));
            else
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
