// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "SimulationContext.hpp"
#include "SplitExecution.hpp"

namespace openswmm {
struct SplitFrame {
    bool reply = false;
    int phase = 0;
    uint64_t sequence = 0;
    uint64_t sender = 0, target = 0;
    bool report_flush = false;
    double start = 0, dt = 0, remaining = 0;
    std::string model;
    int node_count = 0;
    int source_count = 0;
    bool aquifer = false;
    bool spatial_sources = false;
    bool source_returns = false;
    std::vector<double> outfall_returns;
    std::vector<int> source_groups;
    std::vector<double> source_rates, source_previous_rates;
    std::vector<double> source_pending_runoff, source_pending_drain;
    std::array<double,6> source_totals{};
    double source_storage = 0;
    std::vector<int> lid_nodes;
    std::vector<int> mesh_richards_nodes;
    bool extended_water = false;
    uint64_t lid_trial_sequence = 0;
    double infil_factor = 1;
    double recovery_factor = 1;
    struct LidRow {
        int node = -1;
        LidNodeState state;
        double head=0, depth=0, volume=0, full_volume=0, rpt_full_volume=0, lat_flow=0;
        double evap_loss=0, exfil_loss=0;
        static LidRow capture(const SimulationContext& ctx, int node) {
            const int r=ctx.node_subtypes.storage_row(node);
            const auto& st=ctx.node_subtypes.storages;
            return {node,st.lid_state[r],ctx.nodes.head[node],ctx.nodes.depth[node],
                ctx.nodes.volume[node],ctx.nodes.full_volume[node],ctx.nodes.rpt_full_volume[node],
                ctx.nodes.lat_flow[node],st.evap_loss[r],st.exfil_loss[r]};
        }
        void publish(SimulationContext& ctx) const {
            const int r=ctx.node_subtypes.storage_row(node);
            auto& st=ctx.node_subtypes.storages;
            st.lid_state[r]=state;
            st.evap_loss[r]=evap_loss; st.exfil_loss[r]=exfil_loss;
            ctx.nodes.head[node]=head; ctx.nodes.depth[node]=depth;
            ctx.nodes.volume[node]=volume; ctx.nodes.full_volume[node]=full_volume;
            ctx.nodes.rpt_full_volume[node]=rpt_full_volume; ctx.nodes.lat_flow[node]=lat_flow;
        }
    };
    std::vector<LidRow> lids;
    std::vector<uint8_t> aquifer_bed;
    struct Conduits {
        std::vector<uint8_t> coupled;
        std::vector<double> head, conductivity, path, gain_limit, seepage;
        void capture(const ConduitData& source) {
            coupled = source.gw_coupled;
            head = source.gw_head_rel;
            conductivity = source.gw_kc;
            path = source.gw_dc;
            gain_limit = source.gw_gain_max;
            seepage = source.seep_loss_rate;
        }
        void publish(ConduitData& target) const {
            target.gw_coupled = coupled;
            target.gw_head_rel = head;
            target.gw_kc = conductivity;
            target.gw_dc = path;
            target.gw_gain_max = gain_limit;
        }
    } conduits;
    std::array<double,16> groundwater_ledger{};
    std::vector<SplitGageSample> weather;
    std::vector<double> gage_rates;
    std::vector<int> gage_current;
    double evap_rate = 0;
    std::vector<int> mapped_outfalls;
    struct OutfallBoundary {
        double head, ramp, limit;
    };
    std::vector<OutfallBoundary> outfall_boundaries;
    struct Columns {
        std::vector<double> head, depth, volume, rpt_full_volume, coupling_queue, coupling_volume;
        std::vector<double> ponded_area, inflow, outflow, lat_flow;
        void capture(const NodeData& source) {
            inflow = source.inflow;
            outflow = source.outflow;
            head = source.head;
            depth = source.depth;
            volume = source.volume;
            rpt_full_volume = source.rpt_full_volume;
            coupling_queue = source.coupling_queue;
            coupling_volume = source.coupling_volume;
        }
    } nodes;
    std::vector<uint8_t> coupled;
    std::vector<double> pond_area, pond_band;
    SimulationContext::MassBalance2D balance;
    std::vector<std::pair<int, double>> conductance;
};
}
