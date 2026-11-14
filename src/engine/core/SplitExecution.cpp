// SPDX-License-Identifier: Apache-2.0
#include "SWMMEngine.hpp"
#include "SplitFrame.hpp"
#include "MaterialPacket.hpp"
#include <openswmm/engine/openswmm_split_packet.h>
#include "openswmm_api_common.hpp"
#include "FileIO.hpp"
#include "../hydrology/Gage.hpp"
#include "../hydrology/SourceClockGroups.hpp"
#include "../hydrology/LidNode.hpp"
#include "../hydraulics/TimestepController.hpp"
#include "../quality/PeerPassiveStorageExchange.hpp"
#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_gw2d.h>
#include <openswmm/engine/openswmm_gw_transport.h>
#ifdef OPENSWMM_HAS_2D
#include "../2d/subsurface/CommonSourceStage.hpp"
#include "../hydrology/LidNodeAquifer.hpp"
#endif
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace {
using namespace openswmm;
std::atomic<uint64_t> nextIdentity{1};
using Frame = SplitFrame;
void require(bool ok, const char* message) {
    if (!ok) throw std::invalid_argument(message);
}
Frame& frame(SWMM_SplitFrame value) {
    require(value != nullptr, "A native split frame is required");
    return *static_cast<Frame*>(value);
}
std::unique_ptr<Frame> capture(const SimulationContext& ctx, const SplitExecution& state,
                             int phase, uint64_t sequence, double dt) {
    auto value = std::make_unique<Frame>();
    value->phase = phase;
    value->sequence = sequence;
    value->sender = state.identity;
    value->target = state.peer;
    value->start = ctx.elapsed_ms / 1000.;
    value->dt = dt;
    value->reply = state.role == SWMM_SPLIT_SURFACE;
    value->settled_reports = state.settled_reports;
    value->material = state.material;
    if (state.material) {
        value->material_species = ctx.pollutant_names.names()[0];
        value->material_units = ctx.pollutants.units[0];
        value->material_concentration = ctx.nodes.conc;
        value->material_full_volume = ctx.nodes.full_volume;
        value->material_ponded_area = ctx.nodes.ponded_area;
        value->material_rate = ctx.nodes.coupling_inflow;
        value->material_factor = state.material_factor;
        if (!phase) value->material_points = state.material_points;
    }
    if (state.settled_reports) value->report_time_ms = ctx.next_report_ms;
    value->node_count = ctx.n_nodes();
    value->source_count = state.source_count;
    value->aquifer = state.aquifer;
    value->spatial_sources = state.spatial_sources;
    value->source_returns = state.source_returns;
    value->source_groups = state.source_groups;
    value->lid_nodes = state.lid_nodes;
    value->mesh_richards_nodes = state.mesh_richards_nodes;
    value->extended_water = state.extended_water;
    value->reviewed_exchange = state.reviewed_exchange;
    value->lid_trial_sequence = state.lid_trial_sequence;
    value->infil_factor = ctx.climate_state.infil_factor;
    value->recovery_factor = ctx.climate_state.recovery_factor;
    if (!state.lid_nodes.empty() && (phase == SWMM_SPLIT_LID_TRIAL || phase == SWMM_SPLIT_LID_ACCEPT ||
        phase == SWMM_SPLIT_ADVANCE || phase == SWMM_SPLIT_FINISH)) {
        for (int node : state.lid_nodes) value->lids.push_back(Frame::LidRow::capture(ctx,node));
        value->evap_rate = ctx.climate_state.evap_rate;
        value->nodes.lat_flow = ctx.nodes.lat_flow;
    }
    if (state.aquifer && (phase == 0 || phase == SWMM_SPLIT_ADVANCE || phase == SWMM_SPLIT_FINISH ||
                         (state.reviewed_exchange && phase == SWMM_SPLIT_LID_ACCEPT)))
        value->conduits.capture(ctx.link_subtypes.conduits);
    if (state.source_count && state.role == SWMM_SPLIT_NETWORK && phase == SWMM_SPLIT_ADVANCE) {
        value->weather = state.weather;
        value->gage_rates = ctx.gages.rainfall;
        value->gage_current = ctx.gages.st_cur;
        value->evap_rate = ctx.climate_state.evap_rate;
    }
    value->mapped_outfalls = state.mapped_outfalls;
    if (phase == SWMM_SPLIT_OUTFALL_PREPARE && value->reply)
        for (int node : state.mapped_outfalls) {
            const auto row = ctx.node_subtypes.outfall_row(node);
            value->outfall_boundaries.push_back({ctx.node_subtypes.outfalls.head_2d[row],
                ctx.node_subtypes.outfalls.ramp_2d[row],ctx.surface_outfall_link_limit[node]});
        }
    value->nodes.capture(ctx.nodes);
    if (phase == 0) {
        value->model = state.model;
        value->coupled = ctx.coupled_node;
        value->nodes.ponded_area = ctx.nodes.ponded_area;
        value->pond_area = ctx.coupled_pond_area;
        value->pond_band = ctx.coupled_pond_band;
        if (state.aquifer) value->aquifer_bed = ctx.nodes.aquifer2d_bed;
    }
    value->remaining = ctx.coupling_delivery_remaining;
    value->balance = ctx.mass_balance_2d;
    return value;
}
#ifdef OPENSWMM_HAS_2D
void captureGroundwater(SWMMEngine* engine, Frame& value) {
    if (!value.aquifer) return;
    for (int i=0; i<16; ++i)
        require(swmm_gw2d_get_ledger(engine,i,&value.groundwater_ledger[i]) == SWMM_OK,
                "Cannot inspect split groundwater ledger");
}
void validateInputs(SWMMEngine* engine) {
    const auto& ctx = engine->context();
    const auto& o = ctx.options;
    const auto& s = engine->surfaceRouter2D().options();
    const auto& owners = engine->surfaceRouter2D().aquiferConfig().surface_owners;
    const bool spatial = !owners.empty();
    int rows = 0;
    char configured[32]{};
    require(swmm_gw2d_row_count(engine, &rows) == 0 &&
        swmm_gw2d_option_get(engine, "CONFIGURED", configured, sizeof(configured)) == 0,
        "Cannot inspect split aquifer ownership");
    const auto* backend = std::getenv("OPENSWMM_2D_BACKEND");
    const auto* dw_threads = std::getenv("SWMM_DW_THREADS");
    require(o.num_threads == 1 && (!dw_threads || std::atoi(dw_threads) <= 1),
            "Native split profile requires THREADS 1 and no multi-thread DW override");
    for (const char* key : {"TRANSPORT_POLLUTANTS", "TRANSPORT_MSX", "TRANSPORT_AGE", "TRANSPORT_TEMPERATURE"}) {
        char value[32]{};
        require(swmm_options_get_ext(engine, key, value, sizeof(value)) == 0 && std::string(value) == "NO",
                "Native split transport requires separate typed inventory qualification");
    }
    char bed[32]{};
    require(swmm_options_get_ext(engine, "BED_HEAT_MODEL", bed, sizeof(bed)) == 0 && std::string(bed) == "NONE",
            "Native split bed heat requires separate thermal ownership qualification");
    require(o.routing_model == RoutingModel::DYNWAVE && !o.ignore_routing &&
        !o.ignore_2d && o.ignore_quality && !o.heat_transport && !o.water_age &&
        !o.skip_steady_state && !o.rpt_averages &&
        o.node_continuity == NodeContinuity::EXPLICIT && ctx.events.empty() &&
        ctx.n_nodes() > 0 && ctx.n_links() > 0 &&
        ctx.n_pollutants() == 0 && ctx.reactions.n_species() == 0 &&
        ctx.plugin_specs.empty() && ctx.process_component_specs.empty() &&
        ctx.snowpack_names.names().empty(),
        "Native split profile requires water-only DYNWAVE without quality, snow, event or plugin owners");
    for (const auto& lid : ctx.node_subtypes.storages.lid)
        if (lid.control >= 0)
            require(!lidnode::layers(ctx,lid.control).empty() &&
                    (!lidnode::layers(ctx,lid.control).front().flow.enabled ||
                     (spatial && lid.bottom == LidNodeBottom::MeshAquifer)),
                    "Split Richards node LIDs require reviewed sources and MESH_AQUIFER ownership");
    if (rows > 0 || std::string(configured) == "YES") {
        const auto& aq = engine->surfaceRouter2D().aquiferConfig();
        require(rows > 0 && s.groundwater != 0 && !aq.options.per_subcatch &&
                aq.options.soil_char == twoD::SoilChar::GARDNER && (spatial || aq.options.gw_et == "NONE") &&
                aq.options.closure != twoD::GwClosure::AUTO &&
                std::all_of(aq.rows.begin(), aq.rows.end(), [](const auto& row) {
                    return (!row.soil_char_set || row.soil_char == twoD::SoilChar::GARDNER) &&
                           (!row.closure_set || row.closure != twoD::GwClosure::AUTO);
                }), "Split groundwater requires active MESH Gardner water, explicit closure and GW_ET NONE");
        for (const char* key : {"TRANSPORT_POLLUTANTS", "TRANSPORT_MSX", "TRANSPORT_AGE", "TRANSPORT_TEMPERATURE", "CONDUCTION"}) {
            char value[32]{};
            require(swmm_gw_transport_option_get(engine,key,value,sizeof(value)) == 0 && std::string(value) == "NO",
                    "Split groundwater transport requires paired inventory phases");
        }
        char thermal[32]{};
        require(swmm_gw_transport_option_get(engine,"THERMAL_MODEL",thermal,sizeof(thermal)) == 0 &&
                std::string(thermal) == "WATER_ONLY", "Split groundwater heat requires paired thermal phases");
    }
    require(std::all_of(ctx.subcatches.gw_aquifer.begin(), ctx.subcatches.gw_aquifer.end(),
                    [](int row) { return row < 0; }),
        "Split legacy groundwater sources require separate qualification");
    if (spatial) {
        const auto& r=engine->surfaceRouter2D();
        const auto preview=twoD::resolveSurfaceOwnership(ctx,r.mesh(),r.options(),r.aquiferConfig(),owners);
        require(preview.valid() && !preview.cancelled,"Split source ownership preview is invalid");
        require(rows > 0 && s.groundwater != 0 && !r.aquiferConfig().options.node_auto,
                "Reviewed split sources require mesh groundwater with NODE_ENROLMENT ROWS");
    }
    require(ctx.rdii_assigns.count() == 0, "Split RDII ownership requires separate qualification");
    for (const auto& path : ctx.lid_usage.rpt_file)
        require(path.empty(), "Split detailed LID reports require isolated output ownership");
    require(s.backend == twoD::Backend2D::CPU && s.reconstruction_order == 1 &&
        s.coupling_pond_mode == twoD::CouplingPondMode2D::FOOTPRINT &&
        s.coupling_rim_band <= 0 && (!backend || std::string(backend) == "cpu" || std::string(backend) == "CPU"),
        "Native split profile requires first-order CPU and footprint ponds");
    require(ctx.files.hotstart_saves.empty() && ctx.files.hotstart_use_path.empty() &&
        ctx.files.inflows_path.empty() && ctx.files.outflows_path.empty() &&
        ctx.files.runoff_mode == FileMode::NONE && ctx.files.rdii_mode == FileMode::NONE &&
        ctx.files.rainfall_mode == FileMode::NONE,
        "Split file interfaces and restart require separate ownership qualification");
    require(s.output_file.empty(), "Split model must omit the authored surface output path");
}
#endif
}

namespace openswmm {
SplitExecution::SplitExecution() = default;
SplitExecution::~SplitExecution() = default;

void validateMaterialSplitInput(const SimulationContext& ctx) {
#ifdef OPENSWMM_HAS_2D
    quality::PeerPassiveStorageFunding::validateProfile(ctx);
    require(!ctx.options.ignore_routing && !ctx.options.ignore_2d &&
        ctx.options.node_continuity==NodeContinuity::EXPLICIT && !ctx.options.skip_steady_state &&
        !ctx.options.rpt_averages && ctx.plugin_specs.empty() && ctx.process_component_specs.empty() &&
        !ctx.files.has_any(),"Private material role excludes other runtime owners");
#else
    throw std::invalid_argument("Material split requires the native surface build");
#endif
}

class SWMMEngine::MaterialWindow final : public quality::PeerPassiveStorageWindow {
public:
    explicit MaterialWindow(SWMMEngine& network) : network_(network) {}
    void start(int phase) {
        auto& s=*network_.split_;
        require(s.material_pair && !s.material_pair->active && s.material_pair->surface &&
            s.material_pair->network==&network_ && s.material_issuer && !s.material_issuer->failed(),
            "Missing or failed engine-owned material pair");
        s.material_pair->active=true;s.material_pair->phase=phase;issued_=false;reply_.reset();
    }
    std::unique_ptr<quality::PeerFundingOffer> ready(quality::PassiveStorageExchange::Phase phase,double start,double end) override {
        using P=quality::PassiveStorageExchange::Phase;
        auto& s=*network_.split_;
        const int expected=phase==P::ADVANCE?SWMM_SPLIT_ADVANCE:phase==P::REPORT?SplitExecution::REPORT:SWMM_SPLIT_FINISH;
        require(s.material_pair->active && s.material_pair->phase==expected && !issued_,
            "Material funding requires exactly one actual window in its native phase");
        auto offer=s.material_issuer->issue(phase,start,end);issued_=true;
        if(s.material_transport)relay(SWMM_MATERIAL_OFFER,materialOfferPacket(offer->view(),s.sequence,s.material_pair->phase));
        return offer;
    }
    void returned(std::unique_ptr<quality::PeerFundingReply> reply) override {
        require(network_.split_->material_pair->active && issued_ && !reply_ && reply,
            "Material proof is missing or already returned");
        reply_=std::move(reply);
    }
    void prepare(const Frame& request,const Frame& reply) {
        if(issued_) {
            require(reply_!=nullptr,"Material window did not return its single-use proof");
            auto& s=*network_.split_;
            if(s.material_transport)relay(SWMM_MATERIAL_PROOF,materialProofPacket(*reply_,s.sequence,s.material_pair->phase,
                reply.nodes.coupling_queue,reply.nodes.coupling_volume,reply.remaining));
            network_.split_->material_issuer->accept(std::move(reply_));
            network_.split_->material_issuer->validateWaterBooking(reply.nodes.coupling_queue,reply.nodes.coupling_volume,reply.remaining);
        } else require(!reply_ && reply.nodes.coupling_queue==request.nodes.coupling_queue &&
            reply.nodes.coupling_volume==request.nodes.coupling_volume && reply.remaining==request.remaining,
            "Material water booking without an actual funded producer window");
    }
    void confirm() {
        if(!issued_)return;
        auto& s=*network_.split_;auto* peer=s.material_pair->surface;
        require(peer && peer->split_ && peer->split_->material_producer,"Material recipient closed before acknowledgement");
        auto acknowledgement=s.material_issuer->confirmWater();
        if(s.material_transport)relay(SWMM_MATERIAL_ACK,materialAcknowledgementPacket(s.material_issuer->returnedReply()->view(),
            s.sequence,s.material_pair->phase,network_.ctx_.nodes.coupling_queue,network_.ctx_.coupling_delivery_remaining));
        peer->split_->material_producer->acknowledge(std::move(acknowledgement));
    }
    void finish(bool accepted) noexcept {
        auto& s=*network_.split_;
        if(!accepted) {
            if(s.material_issuer)s.material_issuer->fail("Material phase return failed; encumbrance retained.");
            if(s.material_pair && s.material_pair->surface && s.material_pair->surface->split_)
                s.material_pair->surface->split_->failed=true;
        }
        if(s.material_pair){s.material_pair->active=false;s.material_pair->phase=0;}
    }
private:
    void relay(int stage,const std::string& packet) {
        auto& s=*network_.split_;auto& pair=*s.material_pair;
        require(pair.active && !pair.transporting,"Material transport is not at its exclusive native phase");
        pair.transporting=true;
        struct Guard {bool& active;~Guard(){active=false;}} guard{pair.transporting};
        const char* response=nullptr;std::size_t size=0;
        int rc;
        try {rc=s.material_transport(&network_,stage,packet.data(),packet.size(),&response,&size,s.material_transport_user);}
        catch(...){throw std::runtime_error("Material transport callback threw; stop without retry");}
        require(rc==SWMM_OK && response && size && size<=SWMM_MATERIAL_PACKET_MAX_BYTES,
            "Material transport refused or lost its packet; state retained");
        matchMaterialPacket(packet,response,size,network_.ctx_);
    }
    SWMMEngine& network_;
    bool issued_=false;
    std::unique_ptr<quality::PeerFundingReply> reply_;
};

const quality::PeerPassiveStorageFunding* SWMMEngine::peerMaterialFunding() const noexcept {
    return split_?split_->material_issuer.get():nullptr;
}
const quality::PeerPassiveStorageProducer* SWMMEngine::peerMaterialProducer() const noexcept {
    return split_?split_->material_producer.get():nullptr;
}
bool SWMMEngine::materialCloseBlocked() const noexcept {
    return split_ && split_->material && (step_active_ || split_->reporting ||
        (split_->material_pair && (split_->material_pair->active ||
            (split_->material_pair->network && split_->material_pair->network->step_active_))));
}
bool SWMMEngine::materialEndBlocked() const noexcept {
    if(!split_ || !split_->material)return false;
    if(step_active_)return true;
    const auto& p=split_->material_pair;
    if(split_->role==SWMM_SPLIT_SURFACE)
        return !p || !p->active || p->phase!=SWMM_SPLIT_FINISH;
    return p && p->active;
}
void SWMMEngine::detachMaterialPeer() noexcept {
#ifdef OPENSWMM_HAS_2D
    if(!split_ || !split_->material)return;
    auto p=split_->material_pair;
    surface_router_.peer_passive_exchange_=nullptr;surface_router_.peer_window_=nullptr;
    if(!p)return;
    auto* other=split_->role==SWMM_SPLIT_NETWORK?p->surface:p->network;
    if(split_->role==SWMM_SPLIT_NETWORK)p->network=nullptr;else p->surface=nullptr;
    if(other && other->split_ && other->split_->material_pair==p) {
        other->surface_router_.peer_passive_exchange_=nullptr;other->surface_router_.peer_window_=nullptr;
        if(!split_->material_finalized || !other->split_->material_finalized) {
            other->split_->failed=true;
            if(other->split_->material_issuer)other->split_->material_issuer->fail("Material peer closed before completion.");
            if(other->split_->material_producer)other->split_->material_producer->fail("Material peer closed before completion.");
        }
    }
#endif
}
void SWMMEngine::acceptMaterialBoundary() {
    if(!split_ || !split_->material || split_->role!=SWMM_SPLIT_NETWORK)return;
    const auto& pair=split_->material_pair;
    require(pair && pair->surface && !pair->active && split_->material_issuer &&
        !split_->material_issuer->failed() && !split_->material_issuer->unresolved(),"Material acceptance has unresolved work");
    auto& peer=*pair->surface;auto& state=*peer.split_;
    require(state.material_acceptance_pending && !state.awaiting_report && !state.failed &&
        peer.ctx_.elapsed_ms==ctx_.elapsed_ms,"Material surface lacks its completed native boundary");
    peer.ctx_.nodes.conc=ctx_.nodes.conc;
    if(peer.callbacks_.on_step_end)
        peer.callbacks_.on_step_end(&peer,peer.ctx_.current_date,state.output_dt,peer.callbacks_.step_end_ud);
    peer.tracePhase(SWMM_PHASE_STEP_ACCEPTED,state.output_start,state.output_dt);
    state.material_acceptance_pending=false;
}
int SWMMEngine::setMaterialTransport(SWMM_MaterialTransport callback,void* user) noexcept {
#if !defined(OPENSWMM_HAS_2D) || !defined(OPENSWMM_HAS_HYDROCOUPLE)
    return SWMM_ERR_PLUGIN;
#else
    if(!callback || !split_ || !split_->material || split_->role!=SWMM_SPLIT_NETWORK ||
        step_active_ || split_->material_pair || ctx_.state!=EngineState::RUNNING || ctx_.elapsed_ms!=0)
        return SWMM_ERR_LIFECYCLE;
    split_->material_transport=callback;split_->material_transport_user=user;return SWMM_OK;
#endif
}
int SWMMEngine::bindMaterialPeer(SWMMEngine& peer,std::size_t capacity,bool require_transport) noexcept {
#ifndef OPENSWMM_HAS_2D
    return SWMM_ERR_PLUGIN;
#else
    if(&peer==this || !split_ || !peer.split_ || !split_->material || !peer.split_->material ||
        split_->role!=SWMM_SPLIT_NETWORK || peer.split_->role!=SWMM_SPLIT_SURFACE ||
        ctx_.state!=EngineState::RUNNING || peer.ctx_.state!=EngineState::RUNNING || step_active_ || peer.step_active_ ||
        ctx_.elapsed_ms!=0 || peer.ctx_.elapsed_ms!=0 || split_->failed || peer.split_->failed ||
        split_->material_pair || peer.split_->material_pair || split_->material_issuer || peer.split_->material_producer ||
        live_passive_exchange_ || peer.live_passive_exchange_ || checkpoint_enabled_ || peer.checkpoint_enabled_)
        return SWMM_ERR_LIFECYCLE;
    if(require_transport && !split_->material_transport)return SWMM_ERR_LIFECYCLE;
    try {
        require(split_->geometry && peer.split_->geometry && split_->peer==peer.split_->identity &&
            split_->model==peer.split_->model && split_->callback && split_->material_points==peer.split_->material_points &&
            split_->material_factor==peer.split_->material_factor && ctx_.node_names.names()==peer.ctx_.node_names.names() &&
            ctx_.pollutant_names.names()==peer.ctx_.pollutant_names.names() && ctx_.pollutants.units==peer.ctx_.pollutants.units,
            "Material peers require matching authored model, geometry, species and basis");
        require(ctx_.nodes.volume==peer.ctx_.nodes.volume && ctx_.nodes.depth==peer.ctx_.nodes.depth &&
            ctx_.nodes.head==peer.ctx_.nodes.head && ctx_.nodes.full_volume==peer.ctx_.nodes.full_volume &&
            ctx_.nodes.rpt_full_volume==peer.ctx_.nodes.rpt_full_volume && ctx_.nodes.ponded_area==peer.ctx_.nodes.ponded_area &&
            ctx_.nodes.conc==peer.ctx_.nodes.conc,"Initial material native hydraulic or quality stocks differ");
        quality::PeerPassiveStorageFunding::validateProfile(ctx_);
        quality::PeerPassiveStorageFunding::validateProfile(peer.ctx_);
        auto error=peer.surface_router_.changingVolumeProfile(peer.ctx_,peer.surface_router_.state_);
        require(error.empty(),error.c_str());
        auto issuer=std::make_unique<quality::PeerPassiveStorageFunding>(ctx_,split_->identity,peer.split_->identity,
            split_->material_points,split_->material_factor,capacity);
        auto producer=std::make_unique<quality::PeerPassiveStorageProducer>(peer.ctx_,peer.surface_router_.state_,
            issuer->bindRecipient(peer.ctx_),split_->material_factor,capacity);
        auto pair=std::make_shared<MaterialPairLifetime>();pair->network=this;pair->surface=&peer;
        auto channel=std::make_unique<MaterialWindow>(*this);
        split_->material_issuer=std::move(issuer);peer.split_->material_producer=std::move(producer);
        split_->material_pair=peer.split_->material_pair=pair;
        peer.split_->peer=split_->identity;
        split_->material_channel=std::move(channel);
        peer.surface_router_.peer_passive_exchange_=peer.split_->material_producer.get();
        peer.surface_router_.peer_window_=split_->material_channel.get();
        return SWMM_OK;
    } catch(const std::exception& error){ctx_.error_message=error.what();return SWMM_ERR_PLUGIN;}
#endif
}
bool reviewedGroundwaterExchange(const SimulationContext& ctx) {
#ifdef OPENSWMM_HAS_2D
    const auto* aq = ctx.twod_io.aquifer;
    if (!aq || aq->surface_owners.empty()) return false;
    if (aq->options.link_seepage != twoD::GwLinkMode::NONE) return true;
    for (size_t i=0; i<aq->node_beds.size(); ++i) {
        const auto& bed=aq->node_beds[i];
        // Role selection precedes mesh resolution. Use the authored native
        // name so initialization cannot change the packet ownership profile.
        const int node=ctx.twod_io.aquifer_nodes && i<ctx.twod_io.aquifer_nodes->size()
            ? ctx.node_names.find((*ctx.twod_io.aquifer_nodes)[i]) : bed.node;
        const auto bottom=lidnode::bottomMode(ctx,node);
        if (bed.exchange && bottom != LidNodeBottom::MeshAquifer &&
            bottom != LidNodeBottom::Sealed) return true;
    }
    return false;
#else
    return false;
#endif
}
void SWMMEngine::captureSplitSources(SplitFrame& value) {
#ifdef OPENSWMM_HAS_2D
    if (!split_->spatial_sources) return;
    require(bool(source_delivery_) && source_stage_,"Missing native reviewed source owner");
    value.source_totals=source_totals_;
    if (value.phase != SWMM_SPLIT_FINISH)
        split_->source_storage=spatialSourceStorage(source_stage_->sources());
    value.source_storage=split_->source_storage;
    if (value.phase == SWMM_SPLIT_SOURCE_PREPARE) {
        split_->source_rates.clear(); split_->source_previous_rates.clear();
        for (const auto& r : source_delivery_->receipts()) split_->source_rates.push_back(r.rate);
        for (const auto& r : source_delivery_->lastReceipts()) split_->source_previous_rates.push_back(r.rate);
    }
    value.source_rates=split_->source_rates;
    value.source_previous_rates=split_->source_previous_rates;
    if (value.source_rates.empty()) value.source_rates.resize(ctx_.n_nodes(),0);
    if (value.source_previous_rates.empty()) value.source_previous_rates.resize(ctx_.n_nodes(),0);
    value.source_pending_runoff.resize(ctx_.n_nodes(),0);
    value.source_pending_drain.resize(ctx_.n_nodes(),0);
    for (const auto& pending : source_delivery_->pending())
        (pending.water.drain ? value.source_pending_drain : value.source_pending_runoff)[pending.water.node] += pending.water.volume;
#endif
}
int SWMMEngine::configureSplit(int role, bool settled_reports) noexcept {
    return configureSplitImpl(role,settled_reports,false);
}
int SWMMEngine::configureMaterialSplit(int role) noexcept {return configureSplitImpl(role,true,true);}
int SWMMEngine::configureSplitImpl(int role, bool settled_reports, bool material) noexcept {
#ifndef OPENSWMM_HAS_2D
    return SWMM_ERR_PLUGIN;
#else
    if (metadata_only_ || ctx_.state != EngineState::OPENED || split_ || checkpoint_enabled_ ||
        !pending_hotstart_checkpoint_.empty())
        return SWMM_ERR_LIFECYCLE;
    if (role != SWMM_SPLIT_NETWORK && role != SWMM_SPLIT_SURFACE) return SWMM_ERR_BADPARAM;
    try {
        if(material) validateMaterialSplitInput(ctx_);
        else validateInputs(this);
        require(!surface_output_plugin_ && (role != SWMM_SPLIT_SURFACE || out_path_.empty()),
            "Split input must omit OUTPUT_FILE; bind a private surface file after role selection, with no surface-role binary output");
        auto state = std::make_unique<SplitExecution>();
        state->role = role;
        state->source_count = ctx_.n_subcatches();
        state->aquifer = !surface_router_.aquiferConfig().empty();
        state->spatial_sources = !surface_router_.aquiferConfig().surface_owners.empty();
        state->reviewed_exchange = reviewedGroundwaterExchange(ctx_);
        state->settled_reports = settled_reports;
        state->material = material;
        for (int n=0; n<ctx_.n_nodes(); ++n)
            if (ctx_.node_subtypes.storage_row(n) >= 0 &&
                ctx_.node_subtypes.storages.lid[ctx_.node_subtypes.storage_row(n)].control >= 0) {
                state->lid_nodes.push_back(n);
                const auto& lid=ctx_.node_subtypes.storages.lid[ctx_.node_subtypes.storage_row(n)];
                if (lid.bottom == LidNodeBottom::MeshAquifer && lidnode::layers(ctx_,lid.control).front().flow.enabled)
                    state->mesh_richards_nodes.push_back(n);
                else state->extended_water=true;
            }
        require(!settled_reports || (!state->source_count && !state->aquifer && state->lid_nodes.empty()),
                "Private settled reports require simple native water without source, aquifer or node-LID owners");
        if (state->spatial_sources) {
            std::vector<int> seeds;
            for (const auto& owner : surface_router_.aquiferConfig().surface_owners)
                seeds.push_back(ctx_.subcatch_names.find(owner.subcatch));
            runoff::SourceClockGroups groups;
            const auto error=groups.initialize(ctx_,seeds,true);
            require(error.empty(),error.c_str());
            for (int sc=0; sc<ctx_.n_subcatches(); ++sc) state->source_groups.push_back(groups.groupForSubcatch(sc));
            state->source_returns=std::any_of(ctx_.node_subtypes.outfalls.route_to.begin(),
                ctx_.node_subtypes.outfalls.route_to.end(),[](int sc) { return sc >= 0; });
        }
        state->identity = nextIdentity.fetch_add(1);
        const auto input = std::filesystem::weakly_canonical(io::utf8_path(ctx_.inp_file_path));
        std::ifstream source(input, std::ios::binary);
        require(bool(source), "Split input must be a readable native INP file");
        state->model = io::path_utf8(input) + '\n' +
            std::string(std::istreambuf_iterator<char>(source), {});
        split_ = std::move(state);
        if (role == SWMM_SPLIT_NETWORK) ctx_.options.ignore_2d = true;
        else ctx_.options.ignore_routing = true;
        return SWMM_OK;
    } catch (const std::exception& error) {
        // Profile refusal leaves OPENED so callers can inspect/edit the model.
        ctx_.error_message = error.what();
        return SWMM_ERR_PLUGIN;
    }
#endif
}

int SWMMEngine::splitGeometry(SWMM_SplitFrame* output) noexcept {
#ifndef OPENSWMM_HAS_2D
    return SWMM_ERR_PLUGIN;
#else
    if (!output) return SWMM_ERR_BADPARAM;
    if (!split_ || split_->role != SWMM_SPLIT_SURFACE || ctx_.state != EngineState::INITIALIZED)
        return SWMM_ERR_LIFECYCLE;
    try {
        std::vector<int> outfalls;
        for (int n = 0; n < ctx_.n_nodes(); ++n) {
            double engaged = 0, submerged = 0, head = 0;
            if (surface_router_.outfallExchange().tailwater(n,engaged,submerged,head))
                outfalls.push_back(n);
        }
        require(surface_router_.isActive() &&
            surface_router_.subsurface().active() == split_->aquifer &&
            (split_->aquifer || !outfalls.empty() || std::any_of(ctx_.coupled_node.begin(), ctx_.coupled_node.end(), [](auto v) { return v != 0; })),
            "Split surface requires a live coupled mesh and its configured groundwater owner");
        require(!split_->settled_reports || outfalls.empty(), "Private settled reports exclude mapped outfalls");
        if(split_->material) {
            auto error=surface_router_.changingVolumeProfile(ctx_,surface_router_.state_);
            require(error.empty(),error.c_str());
            split_->material_points.clear();
            for(std::size_t k=0;k<surface_router_.node_coupling_points_.size();++k) {
                const auto& point=surface_router_.node_coupling_points_[k];
                split_->material_points.push_back({k,point.cell_idx,point.node_idx,0});
            }
            split_->material_factor=surface_router_.options_.flow_2d_to_1d;
        }
        split_->mapped_outfalls = std::move(outfalls);
        require(!split_->spatial_sources || surface_output_plugin_,"Reviewed split sources require mesh results");
        if (split_->spatial_sources)
            for (int node : split_->mapped_outfalls) {
                const int sc=ctx_.node_subtypes.outfalls.route_to[ctx_.node_subtypes.outfall_row(node)];
                require(sc < 0 || split_->source_groups[sc] < 0,
                        "An outfall mapped to the mesh cannot also return the same water to a reviewed source");
            }
        auto value = capture(ctx_, *split_, 0, 0, 0);
        captureGroundwater(this,*value);
        if (split_->spatial_sources) captureSplitSources(*value);
        split_->geometry = true;
        *output = value.release();
        return SWMM_OK;
    } catch (const std::exception& error) { ctx_.error_message = error.what(); return SWMM_ERR_PLUGIN; }
#endif
}

int SWMMEngine::applySplitGeometry(SWMM_SplitFrame input) noexcept {
    if (!input) return SWMM_ERR_BADPARAM;
    if (!split_ || split_->role != SWMM_SPLIT_NETWORK || split_->geometry ||
        ctx_.state != EngineState::INITIALIZED) return SWMM_ERR_LIFECYCLE;
    try {
        const auto& value = frame(input);
        require(value.phase == 0 && value.sender != 0 && value.target == 0 &&
                value.model == split_->model && value.node_count == ctx_.nodes.count() &&
                value.source_count == split_->source_count && value.aquifer == split_->aquifer &&
                value.spatial_sources == split_->spatial_sources && value.source_groups == split_->source_groups &&
                value.source_returns == split_->source_returns &&
                value.lid_nodes == split_->lid_nodes && value.mesh_richards_nodes == split_->mesh_richards_nodes &&
                value.extended_water == split_->extended_water &&
                value.reviewed_exchange == split_->reviewed_exchange &&
                value.settled_reports == split_->settled_reports &&
                value.material == split_->material &&
                (!split_->material || (value.material_species==ctx_.pollutant_names.names()[0] &&
                    value.material_units==ctx_.pollutants.units[0] && !value.material_points.empty() &&
                    std::isfinite(value.material_factor) && value.material_factor>0)) &&
                (!split_->settled_reports || value.mapped_outfalls.empty()),
                "Split geometry/configuration differs from the network");
        auto outfalls = value.mapped_outfalls;
        auto limits = outfalls.empty() ? std::vector<double>{} :
            std::vector<double>(ctx_.n_nodes(),std::numeric_limits<double>::infinity());
        auto coupled = value.coupled;
        auto area = value.pond_area, band = value.pond_band, footprint = value.nodes.ponded_area;
        ctx_.coupled_node.swap(coupled);
        ctx_.coupled_pond_area.swap(area);
        ctx_.coupled_pond_band.swap(band);
        ctx_.nodes.ponded_area.swap(footprint);
        if (split_->aquifer) {
            for (int node : split_->lid_nodes)
                if (lidnode::bottomMode(ctx_,node) == LidNodeBottom::MeshAquifer) require(size_t(node) < value.aquifer_bed.size() && value.aquifer_bed[node],
                        "Split Richards geometry has no resolved paired aquifer bed");
            auto beds = value.aquifer_bed;
            ctx_.nodes.aquifer2d_bed.swap(beds);
            value.conduits.publish(ctx_.link_subtypes.conduits);
        }
        split_->mapped_outfalls.swap(outfalls);
        if(split_->material){split_->material_points=value.material_points;split_->material_factor=value.material_factor;}
        ctx_.surface_outfall_link_limit.swap(limits);
        split_->geometry = true;
        split_->peer = value.sender;
        if (split_->spatial_sources) {
            split_->source_storage=value.source_storage;
            split_->source_totals=value.source_totals;
            computeFinalStorage();
            ctx_.mass_balance.runoff_init_store=ctx_.mass_balance.runoff_final_store;
        }
        return SWMM_OK;
    } catch (const std::exception& error) { ctx_.error_message = error.what(); return SWMM_ERR_BADPARAM; }
}

int SWMMEngine::setSplitCallback(SWMM_SplitCallback callback, void* user) noexcept {
    if (!split_ || split_->role != SWMM_SPLIT_NETWORK || step_active_ ||
        ctx_.state != EngineState::INITIALIZED) return SWMM_ERR_LIFECYCLE;
    split_->callback = callback; split_->user = user;
    return SWMM_OK;
}

void SWMMEngine::splitExchange(int phase, double dt,
                              std::vector<std::pair<int, double>>* conductance) {
    require(split_ && split_->callback && !split_->failed, "Missing or failed native split participant");
    bool accepted = false;
    auto* material=split_->material?static_cast<MaterialWindow*>(split_->material_channel.get()):nullptr;
    struct PhaseGuard {
        SplitExecution& state;
        bool& accepted;
        MaterialWindow* material;
        ~PhaseGuard() { if (!accepted) state.failed = true;if(material)material->finish(accepted); }
    } guard{*split_, accepted,material};
    if(split_->material){require(material!=nullptr,"Material pair must be bound before native advancement");material->start(phase);}
    auto request = capture(ctx_, *split_, phase, ++split_->sequence, dt);
    if (phase == SWMM_SPLIT_ADVANCE && split_->source_returns) {
        require(split_->accepted_inflow.size() == size_t(ctx_.n_nodes()) &&
                split_->accepted_outflow.size() == size_t(ctx_.n_nodes()),"Missing accepted native outfall flow view");
        request->nodes.inflow=split_->accepted_inflow;
        request->nodes.outflow=split_->accepted_outflow;
        request->outfall_returns.resize(ctx_.n_subcatches(),0);
        for (int sc=0; sc<ctx_.n_subcatches(); ++sc)
            if (ownsSpatialSource(sc)) request->outfall_returns[sc]=ctx_.subcatches.outfall_runon_vol[sc];
    }
    request->report_flush = save_results_ != 0;
    SWMM_SplitFrame response = nullptr;
    int rc;
    try { rc = split_->callback(this, phase, request.get(), &response, split_->user); }
    catch (...) {
        if (response != request.get()) delete static_cast<Frame*>(response);
        throw std::runtime_error("Native split callback threw; stop without retry");
    }
    require(response != request.get(), "Split callback returned its borrowed request as an owned reply");
    std::unique_ptr<Frame> reply(static_cast<Frame*>(response));
    require(rc == SWMM_OK && reply && reply.get() != request.get(), "Split participant refused the native phase; stop without retry");
    require(reply->node_count == ctx_.n_nodes() && reply->reply && reply->sender == split_->peer && reply->target == split_->identity &&
        reply->material == split_->material &&
        (!split_->material || (reply->material_species==request->material_species && reply->material_units==request->material_units &&
            reply->material_factor==request->material_factor && reply->material_concentration==request->material_concentration &&
            reply->material_full_volume==request->material_full_volume && reply->material_ponded_area==request->material_ponded_area &&
            reply->nodes.head==request->nodes.head && reply->nodes.depth==request->nodes.depth && reply->nodes.volume==request->nodes.volume)) &&
        reply->settled_reports == split_->settled_reports &&
        (!split_->settled_reports || reply->report_time_ms == request->report_time_ms) &&
        reply->source_count == split_->source_count && reply->aquifer == split_->aquifer &&
        reply->spatial_sources == split_->spatial_sources && reply->lid_nodes == split_->lid_nodes &&
        reply->source_returns == split_->source_returns && reply->extended_water == split_->extended_water &&
        reply->reviewed_exchange == split_->reviewed_exchange &&
        reply->mesh_richards_nodes == split_->mesh_richards_nodes &&
        reply->mapped_outfalls == request->mapped_outfalls && reply->model == request->model && reply->phase == phase &&
        reply->sequence == request->sequence && reply->start == request->start && reply->dt == dt,
        "Stale or mismatched split phase receipt");
    if(material) {
        const auto nn=std::size_t(ctx_.n_nodes());
        require(reply->nodes.coupling_queue.size()==nn && reply->nodes.coupling_volume.size()==nn &&
            std::isfinite(reply->remaining) && reply->remaining>=0,"Material phase water columns differ");
        for(std::size_t k=0;k<nn;++k)
            require(std::isfinite(reply->nodes.coupling_queue[k]) && reply->nodes.coupling_volume[k]==0,
                "Material phase returned an invalid native water booking");
        material->prepare(*request,*reply);
    }
    if (phase == SWMM_SPLIT_LID_TRIAL) {
        require(!split_->mesh_richards_nodes.empty() && !split_->lid_trial_sequence && !split_->lid_prepared &&
                reply->lid_trial_sequence == reply->sequence &&
                reply->lids.size() == split_->lid_nodes.size(),"Invalid Mesh Richards trial receipt");
        for (size_t i=0; i<reply->lids.size(); ++i) {
            const auto& row=reply->lids[i];
            require(row.node == split_->lid_nodes[i],"LID row order differs");
            if (std::find(split_->mesh_richards_nodes.begin(),split_->mesh_richards_nodes.end(),row.node) != split_->mesh_richards_nodes.end())
            require(row.state.richards_mesh_pending_finish &&
                    !row.state.richards_mesh_routed && row.state.richards_mesh_start == ctx_.current_time &&
                    row.state.richards_completed_time == ctx_.current_time+dt,
                    "Richards proposal has the wrong owner interval");
        }
        split_->lid_trial_sequence=reply->sequence;
        split_->lid_dt=dt;
    } else if (phase == SWMM_SPLIT_LID_ACCEPT) {
        require(split_->lid_trial_sequence && reply->lid_trial_sequence == split_->lid_trial_sequence &&
                split_->lid_dt == dt && reply->lids.size() == split_->lid_nodes.size(),
                "Missing matching Mesh Richards acceptance");
        for (size_t i=0; i<reply->lids.size(); ++i) {
            const auto& row=reply->lids[i];
            require(row.node == split_->lid_nodes[i],"LID row order differs");
            if (std::find(split_->mesh_richards_nodes.begin(),split_->mesh_richards_nodes.end(),row.node) != split_->mesh_richards_nodes.end())
            require(row.state.richards_mesh_pending_finish &&
                    !row.state.richards_mesh_routed && row.state.richards_mesh_start == ctx_.current_time &&
                    row.state.richards_completed_time == ctx_.current_time+dt,
                    "Richards acceptance has the wrong owner interval");
            row.publish(ctx_);
        }
        split_->lid_trial_sequence=0;
        split_->lid_prepared=true;
        if (split_->reviewed_exchange) reply->conduits.publish(ctx_.link_subtypes.conduits);
    } else if (phase == SWMM_SPLIT_SOURCE_PREPARE) {
        require(split_->spatial_sources && !split_->source_prepared &&
                reply->source_rates.size() == size_t(ctx_.n_nodes()) &&
                reply->source_previous_rates.size() == size_t(ctx_.n_nodes()),"Invalid native source offer");
        split_->source_rates=std::move(reply->source_rates);
        split_->source_previous_rates=std::move(reply->source_previous_rates);
        split_->source_prepared=true;
        split_->source_prepared_dt=dt;
    } else if (phase == SWMM_SPLIT_CONDUCTANCE) {
        require(conductance != nullptr, "Missing conductance destination");
        *conductance = std::move(reply->conductance);
    } else if (phase == SWMM_SPLIT_OUTFALL_PREPARE) {
        require(reply->outfall_boundaries.size() == split_->mapped_outfalls.size(),
                "Missing prepared outfall boundaries");
        for (size_t i=0; i<split_->mapped_outfalls.size(); ++i) {
            const auto node = split_->mapped_outfalls[i];
            const auto row = ctx_.node_subtypes.outfall_row(node);
            const auto& boundary = reply->outfall_boundaries[i];
            ctx_.node_subtypes.outfalls.head_2d[row] = boundary.head;
            ctx_.node_subtypes.outfalls.ramp_2d[row] = boundary.ramp;
            ctx_.surface_outfall_link_limit[node] = boundary.limit;
        }
    } else {
        ctx_.nodes.coupling_queue = std::move(reply->nodes.coupling_queue);
        ctx_.nodes.coupling_volume = std::move(reply->nodes.coupling_volume);
        ctx_.coupling_delivery_remaining = reply->remaining;
        ctx_.mass_balance_2d = reply->balance;
        if (split_->aquifer) reply->conduits.publish(ctx_.link_subtypes.conduits);
        if (split_->spatial_sources) {
            require(phase == SWMM_SPLIT_FINISH || (split_->source_prepared && split_->source_prepared_dt == dt),
                    "Missing prepared reviewed source interval");
            auto& mb=ctx_.mass_balance;
            double* fields[]{&mb.runoff_rainfall,&mb.runoff_evap,&mb.runoff_infil,&mb.runoff_runon,&mb.runoff_runoff,&mb.runoff_lid_drain};
            for (int i=0; i<6; ++i) *fields[i] += reply->source_totals[i] - split_->source_totals[i];
            split_->source_totals=reply->source_totals;
            split_->source_storage=reply->source_storage;
            if (phase == SWMM_SPLIT_ADVANCE) split_->source_prepared=false;
        }
        if (phase == SWMM_SPLIT_ADVANCE && split_->source_returns) {
            require(reply->outfall_returns == request->outfall_returns,"Mismatched accepted outfall return receipt");
            for (int sc=0; sc<ctx_.n_subcatches(); ++sc)
                if (ownsSpatialSource(sc)) ctx_.subcatches.outfall_runon_vol[sc]=0;
            split_->accepted_inflow.clear(); split_->accepted_outflow.clear();
        }
    }
    if (phase == SWMM_SPLIT_ADVANCE) { split_->weather.clear(); split_->lid_prepared=false; }
    if(material)material->confirm();
    accepted = true;
}

int SWMMEngine::evaluateSplit(int phase, SWMM_SplitFrame input, SWMM_SplitFrame* output) noexcept {
#ifndef OPENSWMM_HAS_2D
    return SWMM_ERR_PLUGIN;
#else
    if (!input || !output) return SWMM_ERR_BADPARAM;
    if (!split_ || split_->role != SWMM_SPLIT_SURFACE || split_->failed || split_->reporting || step_active_ ||
        (split_->material_pair && split_->material_pair->transporting) ||
        ctx_.state != EngineState::RUNNING) return SWMM_ERR_LIFECYCLE;
    try {
        const auto& request = frame(input);
        require(request.phase == phase && ((phase >= SWMM_SPLIT_CONDUCTANCE && phase <= SWMM_SPLIT_LID_ACCEPT) ||
            (split_->settled_reports && phase == SplitExecution::REPORT)) &&
            request.settled_reports == split_->settled_reports && request.material == split_->material &&
            !request.reply && request.start == ctx_.elapsed_ms / 1000. &&
            request.sequence == split_->sequence + 1 && request.target == split_->identity &&
            request.sender != 0 && (!split_->peer || request.sender == split_->peer) &&
            request.node_count == ctx_.nodes.count() && request.source_count == split_->source_count &&
            request.aquifer == split_->aquifer && request.spatial_sources == split_->spatial_sources &&
            request.source_returns == split_->source_returns &&
            request.lid_nodes == split_->lid_nodes && request.mesh_richards_nodes == split_->mesh_richards_nodes &&
            request.extended_water == split_->extended_water &&
            request.reviewed_exchange == split_->reviewed_exchange &&
            request.mapped_outfalls == split_->mapped_outfalls &&
            (phase == SWMM_SPLIT_FINISH ? (!split_->settled_reports || request.dt == 0) :
                phase == SplitExecution::REPORT ? request.dt == 0 :
                (std::isfinite(request.dt) && request.dt > 0 &&
                request.dt <= std::max((ctx_.options.totalDurationMs() - ctx_.elapsed_ms) / 1000., .001))),
            "Split phase/configuration/clock mismatch");
        if(split_->material) {
            require(split_->material_producer && split_->material_pair && split_->material_pair->network &&
                split_->material_pair->active && split_->material_pair->phase==phase &&
                (phase==SplitExecution::REPORT || !split_->material_acceptance_pending),"Unbound or pending material peer phase");
            quality::PeerPassiveStorageFunding::validateProfile(ctx_);
            const auto nn=std::size_t(ctx_.n_nodes());
            require(request.material_species==ctx_.pollutant_names.names()[0] && request.material_units==ctx_.pollutants.units[0] &&
                request.material_factor==split_->material_factor,"Material species or native conversion differs");
            for(const auto* column:{&request.nodes.head,&request.nodes.depth,&request.nodes.volume,&request.nodes.rpt_full_volume,
                &request.nodes.coupling_queue,&request.nodes.coupling_volume,&request.nodes.inflow,&request.nodes.outflow,
                &request.material_concentration,&request.material_full_volume,&request.material_ponded_area,&request.material_rate}) {
                require(column->size()==nn,"Material native column extent differs");
                for(double value:*column)require(std::isfinite(value),"Material native column is nonfinite");
            }
            for(std::size_t k=0;k<nn;++k)
                require(request.material_concentration[k]>=0 && request.nodes.volume[k]>0 && request.nodes.depth[k]>0 &&
                    request.material_full_volume[k]>=0 && request.material_ponded_area[k]>=0,"Invalid material native stock boundary");
        }
        if (split_->settled_reports) {
            require(request.report_time_ms == ctx_.next_report_ms, "Private settled phase grid differs");
            require(split_->awaiting_report == (phase == SplitExecution::REPORT),
                    "Private settled output requires its pending REPORT before another phase");
            if (phase == SplitExecution::REPORT)
                require(hydraulics::TimestepController::output_due(ctx_) && request.report_time_ms == ctx_.next_report_ms,
                        "Private settled REPORT grid differs");
        }
        if (!split_->mesh_richards_nodes.empty()) {
            if (phase == SWMM_SPLIT_LID_TRIAL)
                require(!split_->lid_trial && !split_->lid_prepared,"Richards interval already has a trial or acceptance");
            else if (phase == SWMM_SPLIT_LID_ACCEPT)
                require(split_->lid_trial && request.lid_trial_sequence == split_->lid_trial_sequence &&
                        split_->lid_dt == request.dt,"Stale Richards trial acceptance");
            else {
                require(!split_->lid_trial,"Unaccepted Richards trial blocks other phases");
                if (phase == SWMM_SPLIT_FINISH || phase == SWMM_SPLIT_SOURCE_PREPARE || phase == SWMM_SPLIT_OUTFALL_PREPARE)
                    require(!split_->lid_prepared,"Unconsumed Richards acceptance");
                else require(split_->lid_prepared && split_->lid_dt == request.dt,
                             "Richards routing requires its accepted interval");
            }
        } else require(phase != SWMM_SPLIT_LID_TRIAL && phase != SWMM_SPLIT_LID_ACCEPT,"No Richards node owners");
        if (split_->spatial_sources) {
            if (phase == SWMM_SPLIT_SOURCE_PREPARE) require(!split_->source_prepared,"Source interval is already prepared");
            else if (phase == SWMM_SPLIT_FINISH) require(!split_->source_prepared,"Cannot finish a pending source interval");
            else require(split_->source_prepared && split_->source_prepared_dt == request.dt,
                         "Source phase requires its matching prepared interval");
        } else require(phase != SWMM_SPLIT_SOURCE_PREPARE,"No reviewed source owner");
        if (!split_->mapped_outfalls.empty()) {
            if (phase == SWMM_SPLIT_OUTFALL_PREPARE)
                require(!split_->routing_prepared, "Outfall reservation is already pending");
            else if (phase == SWMM_SPLIT_FINISH || phase == SWMM_SPLIT_SOURCE_PREPARE)
                require(!split_->routing_prepared, "Finish requires a consumed routing reservation");
            else
                require(split_->routing_prepared && split_->prepared_dt == request.dt,
                        "Routing phase requires its matching outfall preparation");
        } else require(phase != SWMM_SPLIT_OUTFALL_PREPARE, "No mapped outfalls to prepare");
        if (split_->source_returns && phase == SWMM_SPLIT_ADVANCE) {
            require(request.outfall_returns.size() == size_t(ctx_.n_subcatches()),"Incomplete source return inventory");
            for (int sc=0; sc<ctx_.n_subcatches(); ++sc)
                require(std::isfinite(request.outfall_returns[sc]) && request.outfall_returns[sc] >= 0 &&
                        (split_->source_groups[sc] >= 0 || request.outfall_returns[sc] == 0),
                        "Outfall return is negative, nonfinite or belongs to an ordinary source");
        }
        if (split_->source_count && phase == SWMM_SPLIT_ADVANCE) {
            require(request.gage_rates.size() == size_t(ctx_.n_gages()) &&
                    request.gage_current.size() == request.gage_rates.size() &&
                    std::isfinite(request.evap_rate) &&
                    std::all_of(request.gage_rates.begin(), request.gage_rates.end(),
                                [](double value) { return std::isfinite(value) && value >= 0; }),
                    "Invalid split weather state");
            for (int g=0; g<ctx_.n_gages(); ++g) {
                const auto* table = gage::gageRainSeries(ctx_,g);
                const int row = request.gage_current[g];
                require(row >= -2 && (row < 0 || (table && size_t(row) < table->x.size())),
                        "Invalid split report gage cursor");
            }
            double previous = split_->weather_since;
            for (const auto& sample : request.weather) {
                require(std::isfinite(sample.start) && sample.start > previous && sample.start >= 0 &&
                        sample.start <= request.start + request.dt &&
                        sample.rates.size() == request.gage_rates.size() &&
                        std::all_of(sample.rates.begin(), sample.rates.end(),
                                    [](double value) { return std::isfinite(value) && value >= 0; }),
                        "Stale or invalid native split gage journal");
                previous = sample.start;
            }
            require(split_->weather_since >= 0 || (!request.weather.empty() && request.weather.front().start == 0),
                    "Missing initial native split gage sample");
        }
        split_->peer = request.sender;
        split_->report_flush = request.report_flush;
        if (phase != SWMM_SPLIT_LID_ACCEPT) {
        if (split_->aquifer || !split_->mapped_outfalls.empty() || split_->material) {
            ctx_.nodes.inflow = request.nodes.inflow;
            ctx_.nodes.outflow = request.nodes.outflow;
        }
        ctx_.nodes.head = request.nodes.head;
        ctx_.nodes.depth = request.nodes.depth;
        ctx_.nodes.volume = request.nodes.volume;
        ctx_.nodes.rpt_full_volume = request.nodes.rpt_full_volume;
        ctx_.nodes.coupling_queue = request.nodes.coupling_queue;
        ctx_.nodes.coupling_volume = request.nodes.coupling_volume;
        ctx_.coupling_delivery_remaining = request.remaining;
        if(split_->material) {
            ctx_.nodes.conc=request.material_concentration;
            ctx_.nodes.full_volume=request.material_full_volume;ctx_.nodes.ponded_area=request.material_ponded_area;
            ctx_.nodes.coupling_inflow=ctx_.nodes.lat_flow=request.material_rate;
        }
        }
        if (!split_->lid_nodes.empty() && (phase == SWMM_SPLIT_LID_TRIAL || phase == SWMM_SPLIT_ADVANCE)) {
            require(request.lids.size() == split_->lid_nodes.size(),"Missing Richards boundary state");
            for (size_t i=0; i<request.lids.size(); ++i) {
                const auto& row=request.lids[i];
                require(row.node == split_->lid_nodes[i] && row.state.captured_flow == 0,"Unfinished LID boundary state");
                if (std::find(split_->mesh_richards_nodes.begin(),split_->mesh_richards_nodes.end(),row.node) != split_->mesh_richards_nodes.end())
                require(!row.state.richards_mesh_pending_finish &&
                        !row.state.richards_mesh_routed && row.state.captured_flow == 0 &&
                        row.state.mobile_delta == 0 &&
                        std::all_of(row.state.port_delta.begin(),row.state.port_delta.end(),[](double v) { return v == 0; }) &&
                        row.state.richards_completed_time ==
                            (phase == SWMM_SPLIT_ADVANCE ? ctx_.current_time+request.dt : ctx_.current_time),
                        "Richards boundary is not a finished native owner interval");
                row.publish(ctx_);
            }
            if (phase == SWMM_SPLIT_LID_TRIAL) {
                ctx_.nodes.lat_flow=request.nodes.lat_flow;
                ctx_.climate_state.evap_rate=request.evap_rate;
                ctx_.climate_state.infil_factor=request.infil_factor;
                ctx_.climate_state.recovery_factor=request.recovery_factor;
            }
        }
        auto reply = capture(ctx_, *split_, phase, request.sequence, request.dt);
        if (phase == SWMM_SPLIT_LID_TRIAL) {
            split_->lid_trial=std::make_shared<lidnode::MeshRichardsTrial>(lidnode::trialMeshRichardsStep(
                ctx_,surface_router_.subsurface(),surface_router_.state(),request.dt,request.evap_rate));
            split_->lid_trial_sequence=request.sequence;
            split_->lid_dt=request.dt;
            reply->lid_trial_sequence=request.sequence;
            for (auto& row : reply->lids) {
                const auto found=std::find_if(split_->lid_trial->drafts.begin(),split_->lid_trial->drafts.end(),
                    [&](const auto& draft) { return draft.node == row.node; });
                if (found == split_->lid_trial->drafts.end()) continue;
                const auto& draft=*found;
                row.state=draft.state; row.head=draft.head; row.depth=draft.depth;
                row.volume=draft.volume; row.full_volume=row.rpt_full_volume=draft.full_volume;
                row.lat_flow=draft.lat_flow;
            }
        } else if (phase == SWMM_SPLIT_LID_ACCEPT) {
            lidnode::acceptMeshRichardsTrial(ctx_,surface_router_.subsurface(),*split_->lid_trial);
            if (split_->reviewed_exchange) surface_router_.publishLinkCoupling(ctx_);
            split_->lid_trial.reset();
            split_->lid_prepared=true;
            reply=capture(ctx_,*split_,phase,request.sequence,request.dt);
            if (split_->reviewed_exchange) captureGroundwater(this,*reply);
        } else if (phase == SWMM_SPLIT_SOURCE_PREPARE) {
            const auto error=source_delivery_->stage(ctx_.current_time,ctx_.current_time+request.dt);
            require(error.empty(),error.c_str());
            split_->source_prepared=true;
            split_->source_prepared_dt=request.dt;
            captureSplitSources(*reply);
        } else if (phase == SWMM_SPLIT_OUTFALL_PREPARE) {
            surface_router_.updateOutfallsPreRouting(ctx_, request.dt);
            split_->routing_prepared = true;
            split_->prepared_dt = request.dt;
            reply = capture(ctx_, *split_, phase, request.sequence, request.dt);
        } else if (phase == SWMM_SPLIT_CONDUCTANCE) {
            surface_router_.computeCouplingConductances(ctx_, reply->conductance);
        } else {
            int rc = 0;
            if (phase == SWMM_SPLIT_ADVANCE) {
                if (split_->source_returns) ctx_.subcatches.outfall_runon_vol=request.outfall_returns;
                if (split_->aquifer)
                    ctx_.link_subtypes.conduits.seep_loss_rate = request.conduits.seepage;
                if (split_->source_count) {
                    for (const auto& sample : request.weather) {
                        ctx_.gages.rainfall = sample.rates;
                        surface_router_.bookGageRates(ctx_, sample.start);
                        split_->weather_since = sample.start;
                    }
                    ctx_.gages.rainfall = request.gage_rates;
                    // Boundary view for native read-only series reporting;
                    // no gage or watershed driver runs in this participant.
                    ctx_.gages.st_cur = request.gage_current;
                    ctx_.climate_state.evap_rate = request.evap_rate;
                }
                split_->requested_dt = request.dt;
                double elapsed = 0;
                rc = step(&elapsed);
                split_->requested_dt = 0;
            } else if (phase == SplitExecution::REPORT) {
                struct Reporting {
                    bool& active;
                    explicit Reporting(bool& value) : active(value) { active = true; }
                    ~Reporting() { active = false; }
                } reporting(split_->reporting);
                postOutputSnapshot(0);
                split_->awaiting_report = false;
                tracePhase(SWMM_PHASE_OUTPUTS_PUBLISHED, split_->output_start, split_->output_dt);
                if (!split_->material && callbacks_.on_step_end)
                    callbacks_.on_step_end(static_cast<void*>(this), ctx_.current_date, split_->output_dt,
                                           callbacks_.step_end_ud);
                if(!split_->material)tracePhase(SWMM_PHASE_STEP_ACCEPTED, split_->output_start, split_->output_dt);
            } else rc = end();
            if (rc != SWMM_OK) { split_->failed = true; return rc; }
            if (phase == SWMM_SPLIT_ADVANCE) split_->routing_prepared = false;
            if (phase == SWMM_SPLIT_ADVANCE) split_->source_prepared = false;
            if (phase == SWMM_SPLIT_ADVANCE) { split_->lid_prepared=false; split_->lid_trial_sequence=0; }
            reply->nodes.capture(ctx_.nodes);
            reply->remaining = ctx_.coupling_delivery_remaining;
            reply->balance = ctx_.mass_balance_2d;
            if (split_->aquifer) {
                reply->conduits.capture(ctx_.link_subtypes.conduits);
                captureGroundwater(this,*reply);
            }
            if (split_->spatial_sources) captureSplitSources(*reply);
            if (phase == SWMM_SPLIT_ADVANCE && split_->source_returns) reply->outfall_returns=request.outfall_returns;
        }
        split_->sequence = request.sequence;
        *output = reply.release();
        return SWMM_OK;
    } catch (const std::exception& error) {
        split_->requested_dt = 0;
        split_->failed = true;
        ctx_.error_message = error.what();
        return SWMM_ERR_BADPARAM;
    }
#endif
}
}

extern "C" {
int swmm_split_validate_input(SWMM_Engine e) {
    if (!e) return SWMM_ERR_BADHANDLE;
#ifdef OPENSWMM_HAS_2D
    auto* engine = to_engine(e);
    if (engine->context().state != openswmm::EngineState::OPENED) return SWMM_ERR_LIFECYCLE;
    try { validateInputs(engine); return SWMM_OK; }
    catch (const std::exception& ex) { engine->context().error_message = ex.what(); return SWMM_ERR_PLUGIN; }
#else
    return SWMM_ERR_PLUGIN;
#endif
}
int swmm_split_configure(SWMM_Engine e, int role) {
    return e ? to_engine(e)->configureSplit(role) : SWMM_ERR_BADHANDLE;
}
int swmm_split_geometry(SWMM_Engine e, SWMM_SplitFrame* f) {
    return e ? to_engine(e)->splitGeometry(f) : SWMM_ERR_BADHANDLE;
}
int swmm_split_apply_geometry(SWMM_Engine e, SWMM_SplitFrame f) {
    return e ? to_engine(e)->applySplitGeometry(f) : SWMM_ERR_BADHANDLE;
}
int swmm_split_set_callback(SWMM_Engine e, SWMM_SplitCallback cb, void* user) {
    return e ? to_engine(e)->setSplitCallback(cb, user) : SWMM_ERR_BADHANDLE;
}
int swmm_split_evaluate(SWMM_Engine e, int phase, SWMM_SplitFrame in, SWMM_SplitFrame* out) {
    return e ? to_engine(e)->evaluateSplit(phase, in, out) : SWMM_ERR_BADHANDLE;
}
void swmm_split_frame_destroy(SWMM_SplitFrame f) { delete static_cast<Frame*>(f); }
int swmm_split_frame_time(SWMM_SplitFrame f, uint64_t* sequence, double* start, double* dt) {
    if (!f || !sequence || !start || !dt) return SWMM_ERR_BADPARAM;
    const auto& value = frame(f);
    *sequence = value.sequence; *start = value.start; *dt = value.dt;
    return SWMM_OK;
}
int swmm_split_frame_node(SWMM_SplitFrame f, int node, double* head, double* depth,
                          double* queued, double* direct, double* remaining) {
    if (!f || !head || !depth || !queued || !direct || !remaining) return SWMM_ERR_BADPARAM;
    const auto& value = frame(f);
    if (node < 0 || node >= value.node_count) return SWMM_ERR_BADINDEX;
    // Decoded geometry and receipts contain only their published quantities.
    if (value.nodes.head.empty() || value.nodes.coupling_queue.empty()) return SWMM_ERR_BADPARAM;
    *head = value.nodes.head[node] * .3048;
    *depth = value.nodes.depth[node] * .3048;
    *queued = value.nodes.coupling_queue[node] * .028316846592;
    *direct = value.nodes.coupling_volume[node] * .028316846592;
    *remaining = value.remaining;
    return SWMM_OK;
}
int swmm_split_frame_source(SWMM_SplitFrame f, int node, double* rate, double* previous,
                             double* runoff, double* drain) {
    if (!f || !rate || !previous || !runoff || !drain) return SWMM_ERR_BADPARAM;
    const auto& value=frame(f);
    if (node < 0 || node >= value.node_count) return SWMM_ERR_BADINDEX;
    if (!value.spatial_sources || value.source_rates.size() != size_t(value.node_count) ||
        value.source_previous_rates.size() != size_t(value.node_count) ||
        value.source_pending_runoff.size() != size_t(value.node_count) ||
        value.source_pending_drain.size() != size_t(value.node_count)) return SWMM_ERR_BADPARAM;
    *rate=value.source_rates[node]; *previous=value.source_previous_rates[node];
    *runoff=value.source_pending_runoff[node]; *drain=value.source_pending_drain[node];
    return SWMM_OK;
}
int swmm_split_frame_return(SWMM_SplitFrame f, int sc, double* volume) {
    if (!f || !volume) return SWMM_ERR_BADPARAM;
    const auto& value=frame(f);
    if (sc < 0 || sc >= value.source_count) return SWMM_ERR_BADINDEX;
    if (!value.source_returns || value.phase != SWMM_SPLIT_ADVANCE ||
        value.outfall_returns.size() != size_t(value.source_count)) return SWMM_ERR_BADPARAM;
    *volume=value.outfall_returns[sc];
    return SWMM_OK;
}
}
