// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <openswmm/engine/openswmm_split.h>
#include <string>
#include <vector>
#include <cstdint>
#include <array>
#include <memory>

namespace openswmm {
struct SimulationContext;
bool reviewedGroundwaterExchange(const SimulationContext&);
namespace lidnode { struct MeshRichardsTrial; }
struct SplitGageSample {
    double start = 0;
    std::vector<double> rates;
};
struct SplitExecution {
    int role = 0;
    std::string model;
    SWMM_SplitCallback callback = nullptr;
    void* user = nullptr;
    uint64_t sequence = 0;
    uint64_t identity = 0, peer = 0;
    bool geometry = false;
    bool failed = false;
    bool report_flush = false;
    bool finished = false;
    std::vector<int> mapped_outfalls;
    bool routing_prepared = false;
    double prepared_dt = 0;
    double requested_dt = 0;
    int source_count = 0;
    bool aquifer = false;
    bool spatial_sources = false;
    bool source_returns = false;
    std::vector<double> accepted_inflow, accepted_outflow;
    std::vector<int> source_groups;
    std::vector<double> source_rates, source_previous_rates;
    std::array<double,6> source_totals{};
    double source_storage = 0;
    bool source_prepared = false;
    double source_prepared_dt = 0;
    std::vector<int> lid_nodes;
    std::vector<int> mesh_richards_nodes;
    bool extended_water = false;
    bool reviewed_exchange = false;
    std::shared_ptr<lidnode::MeshRichardsTrial> lid_trial;
    uint64_t lid_trial_sequence = 0;
    bool lid_prepared = false;
    double lid_dt = 0;
    std::vector<SplitGageSample> weather;
    double weather_since = -1;
};
}
