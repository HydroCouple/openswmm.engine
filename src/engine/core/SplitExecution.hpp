// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <openswmm/engine/openswmm_split.h>
#include <openswmm/engine/openswmm_split_material.h>
#include <string>
#include <vector>
#include <cstdint>
#include <array>
#include <memory>
#include "../quality/PassiveStorageExchange.hpp"

namespace openswmm {
struct SimulationContext;
class SWMMEngine;
namespace quality { class PeerPassiveStorageFunding; class PeerPassiveStorageProducer; class PeerPassiveStorageWindow; }
struct MaterialPairLifetime {
    SWMMEngine* network = nullptr;
    SWMMEngine* surface = nullptr;
    bool active = false;
    bool transporting = false;
    int phase = 0;
    bool checkpoint_enabled = false, checkpoint_accepted = false;
    bool checkpoint_outputs = false;
    std::string checkpoint_model;
    double initial_material = 0, initial_loss = 0, initial_gain = 0;
};
bool reviewedGroundwaterExchange(const SimulationContext&);
namespace lidnode { struct MeshRichardsTrial; }
struct SplitGageSample {
    double start = 0;
    std::vector<double> rates;
};
struct SplitExecution {
    SplitExecution();
    ~SplitExecution();
    static constexpr int REPORT = 8; // private native mode; not a schemas 1-9 phase
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
    bool settled_reports = false, awaiting_report = false, reporting = false;
    double output_start = 0, output_dt = 0;
    bool material = false;
    bool material_acceptance_pending = false;
    bool material_finalized = false;
    SWMM_MaterialTransport material_transport = nullptr;
    void* material_transport_user = nullptr;
    std::vector<quality::PassiveStorageExchange::HydraulicPoint> material_points;
    double material_factor = 0;
    std::shared_ptr<MaterialPairLifetime> material_pair;
    std::unique_ptr<quality::PeerPassiveStorageFunding> material_issuer;
    std::unique_ptr<quality::PeerPassiveStorageProducer> material_producer;
    std::unique_ptr<quality::PeerPassiveStorageWindow> material_channel;
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
