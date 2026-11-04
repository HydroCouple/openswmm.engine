// SPDX-License-Identifier: Apache-2.0
#include <openswmm/engine/openswmm_split_packet.h>
#include "openswmm_api_common.hpp"
#include "SplitFrame.hpp"
#include "../hydrology/Gage.hpp"
#if defined(OPENSWMM_HAS_HYDROCOUPLE) && defined(OPENSWMM_HAS_2D)
#include <nlohmann/json.hpp>
#include "../2d/subsurface/SubsurfaceData.hpp"
#include "../hydrology/LidNode.hpp"

namespace openswmm::richards {
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Transfer, from, to, volume)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Report, ok, error, accepted, rejected, rhs, newton,
    min_step, bottom_volume, evaporation, balance, bottom_outflow, bottom_inflow,
    inflow, bottom_out_limited, bottom_in_limited, transfers)
}
namespace openswmm {
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(GreenAmptState, S, Ks, IMDmax, IMD, F, Fu, Fumax, Lu, T, saturated)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LidWaterTransfer, from, to, volume)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LidPortTransfer, link, cell, volume)
// Authored cell geometry/materials stay in each matching native model. Only the
// qualified water state crosses this phase boundary; no chemistry/thermal state.
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LidNodeState, richards_water, richards_pressure,
    richards_report, richards_bottom_loss, richards_completed_time, richards_mesh_start,
    richards_mesh_pending_finish, richards_mesh_routed, infiltration_head,
    quality_old_water, quality_transfers, quality_ports, quality_old_mobile, mobile_delta,
    port_delta, held_volume, evap_volume, captured_flow, treated_volume)
}

namespace {
using Json = nlohmann::json;
using Frame = openswmm::SplitFrame;
using Balance = openswmm::SimulationContext::MassBalance2D;
constexpr auto schema = "openswmm.node-surface-water/1";
constexpr auto outfallSchema = "openswmm.node-surface-water/2";
constexpr auto sourceSchema = "openswmm.node-surface-water/3";
constexpr auto groundwaterSchema = "openswmm.node-surface-water/4";
constexpr auto footprintSchema = "openswmm.node-surface-water/5";
constexpr auto richardsSchema = "openswmm.node-surface-water/6";
constexpr auto returnSchema = "openswmm.node-surface-water/7";
constexpr auto lidSchema = "openswmm.node-surface-water/8";
void require(bool ok, const char* text) {
    if (!ok)
        throw std::invalid_argument(text);
}
// Named members keep the JSON independent of native struct layout and padding.
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Balance, init_storage, final_storage, external_in, external_out,
                                   rainfall_in, coupling_1d_to_2d_in, coupling_2d_to_1d_out,
                                   outfall_in, outfall_out, boundary_in, boundary_out, evap_out,
                                   infil_out, infil_to_aquifer, infil_aquifer_pending, aquifer_in,
                                   active, solver_nsteps, solver_nrhs, solver_avg_h, solver_last_h,
                                   solver_active_min, solver_active_mean, solver_active_max,
                                   solver_n_tiers)

Json encode(const Frame& f, const openswmm::SimulationContext& ctx) {
    Json j = {{"schema", f.extended_water ? lidSchema : f.source_returns ? returnSchema : !f.lid_nodes.empty() ? richardsSchema : f.spatial_sources ? footprintSchema : f.aquifer ? groundwaterSchema : f.source_count ? sourceSchema : f.mapped_outfalls.empty() ? schema : outfallSchema},
              {"phase", f.phase},
              {"sender", f.sender},
              {"target", f.target},
              {"sequence", f.sequence},
              {"start_s", f.start},
              {"duration_s", f.dt},
              {"node_count", f.node_count},
              {"kind", f.phase == 0 ? "geometry"
                       : f.reply    ? "receipt"
                                    : "request"}};
    if (f.source_returns) {
        if (!f.phase) j["outfall_return_targets"]=ctx.node_subtypes.outfalls.route_to;
        if (f.phase == SWMM_SPLIT_ADVANCE) j["outfall_returns_ft3"]=f.outfall_returns;
    }
    if (!f.lid_nodes.empty()) {
        j["lid_nodes"]=f.lid_nodes;
        if (f.extended_water) j["mesh_richards_nodes"]=f.mesh_richards_nodes;
        j["lid_trial_sequence"]=f.lid_trial_sequence;
        if (f.phase == SWMM_SPLIT_LID_TRIAL || f.phase == SWMM_SPLIT_LID_ACCEPT ||
            f.phase == SWMM_SPLIT_ADVANCE || f.phase == SWMM_SPLIT_FINISH) {
            const char* key=f.extended_water ? "lid_states" : "richards_nodes";
            j[key]=Json::array();
            for (const auto& row : f.lids) {
                Json item={{"node",row.node},{"state",row.state},
                    {"hydraulics",{row.head,row.depth,row.volume,row.full_volume,row.rpt_full_volume,row.lat_flow,
                                   row.evap_loss,row.exfil_loss}}};
                std::vector<double> theta;
                for (const auto& cell : row.state.cells) theta.push_back(cell.theta);
                item["theta"]=theta;
                if (f.extended_water) {
                    item["richards"]=row.state.richards;
                    item["state"]["green_ampt"]=row.state.surface_infil;
                    item["state"]["infiltration_cell"]=row.state.infiltration_cell;
                }
                j[key].push_back(std::move(item));
            }
            if (!f.reply) {
                j["lid_lateral_cfs"]=f.nodes.lat_flow;
                j["lid_evap_ft_per_s"]=f.evap_rate;
                j["lid_infil_factor"]=f.infil_factor;
                if (f.extended_water) j["lid_recovery_factor"]=f.recovery_factor;
            }
        }
    }
    if (f.extended_water || f.aquifer || f.source_count || !f.mapped_outfalls.empty())
        j["mapped_outfalls"] = f.mapped_outfalls;
    if (f.extended_water || f.aquifer || f.source_count) j["source_count"] = f.source_count;
    if (f.spatial_sources && (!f.phase || (f.reply &&
        (f.phase == SWMM_SPLIT_SOURCE_PREPARE || f.phase == SWMM_SPLIT_ADVANCE || f.phase == SWMM_SPLIT_FINISH)))) {
        j["reviewed_sources"]={{"held_rate_cfs",f.source_rates},{"previous_rate_cfs",f.source_previous_rates},
            {"pending_runoff_ft3",f.source_pending_runoff},{"pending_drain_ft3",f.source_pending_drain},
            {"totals_ft3",f.source_totals},{"storage_ft3",f.source_storage}};
    }
    if (f.aquifer) {
        j["conduit_count"] = ctx.link_subtypes.conduits.link_idx.size();
        if (!f.phase || (f.reply && (f.phase == SWMM_SPLIT_ADVANCE || f.phase == SWMM_SPLIT_FINISH))) {
            j["groundwater_conduits"] = {{"coupled",f.conduits.coupled}, {"head_relative_ft",f.conduits.head},
                {"conductivity_ft_per_s",f.conduits.conductivity}, {"path_ft",f.conduits.path},
                {"gain_limit_cfs",f.conduits.gain_limit}};
            j["groundwater_ledger_m3"] = f.groundwater_ledger;
        }
        if (!f.reply && f.phase == SWMM_SPLIT_ADVANCE)
            j["conduit_seepage_cfs"] = f.conduits.seepage;
    }
    if (!f.phase) {
        j["configuration"] = f.model;
        j["node_ids"] = ctx.node_names.names();
        if (f.spatial_sources) {
            j["subcatchment_ids"] = ctx.subcatch_names.names();
            j["source_groups"] = f.source_groups;
        }
        if (f.aquifer) {
            j["link_ids"] = ctx.link_names.names();
            j["conduit_links"] = ctx.link_subtypes.conduits.link_idx;
            j["aquifer_bed"] = f.aquifer_bed;
        }
        if (f.source_count) j["gage_ids"] = ctx.gage_names.names();
        j["coupled"] = f.coupled;
        j["ponded_area_ft2"] = f.nodes.ponded_area;
        j["pond_area_ft2"] = f.pond_area;
        j["pond_band_ft"] = f.pond_band;
    } else if (f.reply && f.phase == SWMM_SPLIT_SOURCE_PREPARE) {
        // The native bridge offer is carried above; no surface water advances.
    } else if (f.reply && f.phase == SWMM_SPLIT_OUTFALL_PREPARE) {
        j["outfall_boundaries"] = Json::array();
        for (const auto& b : f.outfall_boundaries)
            j["outfall_boundaries"].push_back({b.head, b.ramp, b.limit});
    } else if (f.reply && f.phase == SWMM_SPLIT_CONDUCTANCE) {
        j["conductance_ft2_per_s"] = f.conductance;
    } else {
        j["queued_ft3"] = f.nodes.coupling_queue;
        j["direct_ft3"] = f.nodes.coupling_volume;
        j["remaining_s"] = f.remaining;
        if (f.reply) {
            to_json(j["balance"], f.balance);
            j["tier_cells"] = f.balance.solver_tier_cells;
        } else {
            j["head_ft"] = f.nodes.head;
            j["depth_ft"] = f.nodes.depth;
            j["volume_ft3"] = f.nodes.volume;
            j["rim_volume_ft3"] = f.nodes.rpt_full_volume;
            j["flush_on_report"] = f.report_flush;
            if (f.aquifer || !f.mapped_outfalls.empty()) {
                j["node_inflow_cfs"] = f.nodes.inflow;
                j["node_outflow_cfs"] = f.nodes.outflow;
            }
            if (f.source_count && f.phase == SWMM_SPLIT_ADVANCE) {
                j["weather"] = {{"gage_rates", f.gage_rates}, {"gage_current", f.gage_current}, {"evap_ft_per_s", f.evap_rate},
                                 {"samples", Json::array()}};
                for (const auto& sample : f.weather)
                    j["weather"]["samples"].push_back({{"start_s", sample.start}, {"rates", sample.rates}});
            }
        }
    }
    return j;
}
std::vector<double> column(const Json& j, const char* name, size_t count, bool optional = false) {
    const auto& a = j.at(name);
    require(a.is_array() && (a.size() == count || (optional && a.empty())),
            "Invalid split column extent");
    for (const auto& element : a)
        require(element.is_number(), "Expected a numeric column");
    auto values = a.get<std::vector<double>>();
    for (double v : values)
        require(std::isfinite(v), "Invalid split numeric value");
    return values;
}
uint64_t integer(const Json& j, const char* name) {
    const auto& value = j.at(name);
    require(value.is_number_unsigned(), "Expected an unsigned packet integer");
    return value.get<uint64_t>();
}
std::vector<uint8_t> flags(const Json& j, size_t count) {
    require(j.is_array() && j.size() == count, "Invalid split flag extent");
    std::vector<uint8_t> result;
    for (const auto& v : j) {
        require(v.is_number_unsigned() && (v == 0 || v == 1), "Invalid split flag");
        result.push_back(v.get<uint8_t>());
    }
    return result;
}
std::unique_ptr<Frame> decode(const Json& j, const openswmm::SimulationContext& ctx) {
    const size_t count = ctx.n_nodes();
    std::vector<int> configured_lids, mesh_richards;
    for (int n=0; n<ctx.n_nodes(); ++n) {
        const int r=ctx.node_subtypes.storage_row(n);
        if (r < 0 || ctx.node_subtypes.storages.lid[r].control < 0) continue;
        configured_lids.push_back(n);
        const auto& lid=ctx.node_subtypes.storages.lid[r];
        if (lid.bottom == openswmm::LidNodeBottom::MeshAquifer &&
            openswmm::lidnode::layers(ctx,lid.control).front().flow.enabled) mesh_richards.push_back(n);
    }
    const bool extended = j.at("schema") == lidSchema;
    require(extended == (configured_lids.size() != mesh_richards.size()),"Split LID schema downgrade or unexpected formulation");
    const bool configured_spatial=ctx.twod_io.aquifer && !ctx.twod_io.aquifer->surface_owners.empty();
    const bool configured_groundwater=ctx.twod_io.aquifer && !ctx.twod_io.aquifer->empty();
    const bool configured_returns=configured_spatial && std::any_of(ctx.node_subtypes.outfalls.route_to.begin(),
        ctx.node_subtypes.outfalls.route_to.end(),[](int sc) { return sc >= 0; });
    const bool returns = j.at("schema") == returnSchema || (extended && configured_returns);
    const bool joint = j.at("schema") == richardsSchema || (returns && !extended && !configured_lids.empty()) ||
                       (extended && !mesh_richards.empty());
    const bool spatial = j.at("schema") == footprintSchema || (!extended && joint) || (!extended && returns) ||
                         (extended && configured_spatial);
    const bool groundwater = j.at("schema") == groundwaterSchema || (!extended && spatial) ||
                             (extended && configured_groundwater);
    const bool sources = j.at("schema") == sourceSchema || ((groundwater || extended) && ctx.n_subcatches() > 0);
    const bool outfalls = j.at("schema") == outfallSchema || sources || groundwater || extended;
    require((j.at("schema") == schema || outfalls) && count > 0 &&
                sources == (ctx.n_subcatches() > 0) &&
                groundwater == (ctx.twod_io.aquifer && !ctx.twod_io.aquifer->empty()) &&
                spatial == (ctx.twod_io.aquifer && !ctx.twod_io.aquifer->surface_owners.empty()) &&
                returns == (spatial && std::any_of(ctx.node_subtypes.outfalls.route_to.begin(),
                    ctx.node_subtypes.outfalls.route_to.end(),[](int sc) { return sc >= 0; })) &&
                integer(j, "node_count") == count,
            "Split schema or node count mismatch");
    auto f = std::make_unique<Frame>();
    const auto phase = integer(j, "phase");
    require(phase <= (joint ? SWMM_SPLIT_LID_ACCEPT : spatial ? SWMM_SPLIT_SOURCE_PREPARE : outfalls ? SWMM_SPLIT_OUTFALL_PREPARE : SWMM_SPLIT_FINISH),
            "Unknown split phase");
    f->phase = static_cast<int>(phase);
    f->sender = integer(j, "sender");
    f->target = integer(j, "target");
    f->sequence = integer(j, "sequence");
    f->start = j.at("start_s").get<double>();
    f->dt = j.at("duration_s").get<double>();
    const auto& kind = j.at("kind");
    require(f->sender &&
                (phase == 0 ? kind == "geometry" && !f->target && !f->sequence
                            : (kind == "request" || kind == "receipt") && f->target && f->sequence),
            "Invalid phase identity");
    require(f->start >= 0 && (phase == 0 || phase == SWMM_SPLIT_FINISH ? f->dt == 0 : f->dt > 0),
            "Invalid split phase interval");
    f->reply = kind != "request";
    f->node_count = ctx.n_nodes();
    f->aquifer = groundwater;
    f->spatial_sources = spatial;
    f->source_returns = returns;
    f->extended_water = extended;
    f->mesh_richards_nodes=mesh_richards;
    if (returns) {
        if (!phase) require(j.at("outfall_return_targets") == ctx.node_subtypes.outfalls.route_to,
                            "Split outfall return targets/order differ");
        if (phase == SWMM_SPLIT_ADVANCE) {
            f->outfall_returns=column(j,"outfall_returns_ft3",ctx.n_subcatches());
            require(std::all_of(f->outfall_returns.begin(),f->outfall_returns.end(),[](double v) { return v >= 0; }),
                    "Negative accepted outfall return");
        }
    }
    require((joint || extended) == !configured_lids.empty(),"Split LID schema downgrade or unexpected node owners");
    if (joint || extended) {
        if (extended) require(j.at("mesh_richards_nodes") == mesh_richards,"Split Mesh Richards subset differs");
        require(j.at("lid_nodes") == configured_lids,"Split Richards node identity/order differs");
        f->lid_nodes=configured_lids;
        f->lid_trial_sequence=integer(j,"lid_trial_sequence");
        if (phase == SWMM_SPLIT_LID_TRIAL || phase == SWMM_SPLIT_LID_ACCEPT ||
            phase == SWMM_SPLIT_ADVANCE || phase == SWMM_SPLIT_FINISH) {
            const auto& rows=j.at(extended ? "lid_states" : "richards_nodes");
            require(rows.is_array() && rows.size() == configured_lids.size(),"Incomplete Richards owner cohort");
            for (size_t i=0; i<rows.size(); ++i) {
                const auto& item=rows[i];
                const int n=configured_lids[i];
                require(item.at("node").is_number_integer() && item.at("node") == n,"Richards row order mismatch");
                auto row=Frame::LidRow::capture(ctx,n);
                const auto size=row.state.cells.size();
                require(size >= (row.state.richards ? 2u : 1u),"LID receiver is not initialized");
                if (extended) require(item.at("richards").is_boolean() && item.at("richards") == row.state.richards,"LID flow formulation differs");
                const auto& state=item.at("state");
                // Reject null/non-numeric/nonfinite fields before native conversion.
                const auto finite = [](const auto& self, const Json& value) -> bool {
                    if (value.is_number()) return std::isfinite(value.get<double>());
                    if (value.is_array() || value.is_object())
                        return std::all_of(value.begin(),value.end(),[&](const auto& v) { return self(self,v); });
                    return value.is_boolean() || value.is_string();
                };
                require(finite(finite,state),"Invalid Richards state number");
                openswmm::from_json(state,row.state);
                if (extended) {
                    require(state.at("infiltration_cell").is_number_integer() &&
                            state.at("infiltration_cell") == row.state.infiltration_cell,"LID infiltration cell differs");
                    openswmm::from_json(state.at("green_ampt"),row.state.surface_infil);
                }
                auto theta=column(item,"theta",size);
                require(row.state.richards_water.size() == (row.state.richards ? size : 0) &&
                        row.state.richards_pressure.size() == (row.state.richards ? size : 0) &&
                        row.state.port_delta.size() == size && (row.state.quality_old_water.empty() ||
                        row.state.quality_old_water.size() == size),"Invalid Richards column extent");
                for (const auto* amounts : {&row.state.richards_water,&row.state.quality_old_water,&theta})
                    require(std::all_of(amounts->begin(),amounts->end(),[](double v) { return v >= 0; }),"Negative Richards water");
                for (size_t k=0; k<size; ++k) row.state.cells[k].theta=theta[k];
                const auto& report=row.state.richards_report;
                require(report.ok && report.error.empty() && report.accepted >= 0 && report.rejected >= 0 &&
                        report.rhs >= 0 && report.newton >= 0 && report.bottom_outflow >= 0 &&
                        report.bottom_inflow >= 0 && report.inflow >= 0 && report.evaporation >= 0 &&
                        row.state.held_volume >= 0 && row.state.evap_volume >= 0 &&
                        row.state.captured_flow >= 0 && row.state.treated_volume >= 0 &&
                        row.state.richards_completed_time >= 0 && row.state.richards_mesh_start >= 0 &&
                        row.state.richards_bottom_loss == 0,"Invalid Richards accepted state");
                const bool proposal = f->reply && (phase == SWMM_SPLIT_LID_TRIAL || phase == SWMM_SPLIT_LID_ACCEPT);
                const bool mesh = std::find(mesh_richards.begin(),mesh_richards.end(),n) != mesh_richards.end();
                require(row.state.richards_mesh_pending_finish == (proposal && mesh) && !row.state.richards_mesh_routed,
                        "Richards packet has the wrong preparation/finish state");
                if (!proposal || (!mesh && phase == SWMM_SPLIT_LID_TRIAL))
                    require(row.state.captured_flow == 0,"Unfinished LID lateral capture");
                if (!proposal && mesh)
                    require(row.state.mobile_delta == 0 &&
                            std::all_of(row.state.port_delta.begin(),row.state.port_delta.end(),[](double v) { return v == 0; }),
                            "Richards boundary still has unfinished hydraulic ports");
                for (const auto& t : report.transfers)
                    require(t.from >= -5 && t.to >= -5 && t.from < int(size) && t.to < int(size) && t.volume >= 0,
                            "Invalid Richards column transfer");
                for (const auto& t : row.state.quality_transfers)
                    require(t.from >= -5 && t.to >= -5 && t.from < int(size) && t.to < int(size) && t.volume >= 0,
                            "Invalid Richards water transfer");
                for (const auto& p : row.state.quality_ports)
                    require(p.link >= 0 && p.link < ctx.n_links() && p.cell >= -1 && p.cell < int(size),
                            "Invalid Richards accepted port");
                auto h=column(item,"hydraulics",8);
                require(h[1] >= 0 && h[2] >= 0 && h[3] >= 0 && h[4] >= 0 && h[6] >= 0 && h[7] >= 0,
                        "Negative Richards hydraulic inventory");
                row.head=h[0]; row.depth=h[1]; row.volume=h[2]; row.full_volume=h[3];
                row.rpt_full_volume=h[4]; row.lat_flow=h[5]; row.evap_loss=h[6]; row.exfil_loss=h[7];
                f->lids.push_back(std::move(row));
            }
            if (!f->reply) {
                f->nodes.lat_flow=column(j,"lid_lateral_cfs",count);
                f->evap_rate=j.at("lid_evap_ft_per_s").get<double>();
                f->infil_factor=j.at("lid_infil_factor").get<double>();
                if (extended) f->recovery_factor=j.at("lid_recovery_factor").get<double>();
                require(std::isfinite(f->evap_rate) && f->evap_rate >= 0 &&
                        std::isfinite(f->infil_factor) && f->infil_factor >= 0 &&
                        std::isfinite(f->recovery_factor) && f->recovery_factor >= 0,"Invalid LID climate forcing");
            }
        }
    }
    if (sources || groundwater || extended) {
        require(integer(j, "source_count") == uint64_t(ctx.n_subcatches()), "Split source count mismatch");
        f->source_count = ctx.n_subcatches();
    }
    if (outfalls) {
        const auto& mapped = j.at("mapped_outfalls");
        require(mapped.is_array() && (sources || groundwater || extended || !mapped.empty()) && mapped.size() <= count,
                "Invalid mapped outfall count");
        int previous = -1;
        for (const auto& node : mapped) {
            require(node.is_number_unsigned() && node.get<uint64_t>() < count,
                    "Invalid mapped outfall index");
            const int n = node.get<int>();
            require(n > previous && ctx.node_subtypes.outfall_row(n) >= 0,
                    "Mapped outfalls must be ordered unique outfall nodes");
            f->mapped_outfalls.push_back(n);
            previous = n;
        }
    }
    require(phase != SWMM_SPLIT_OUTFALL_PREPARE || !f->mapped_outfalls.empty(), "No mapped outfalls to prepare");
    if (spatial && (!phase || (f->reply && (phase == SWMM_SPLIT_SOURCE_PREPARE ||
                                          phase == SWMM_SPLIT_ADVANCE || phase == SWMM_SPLIT_FINISH)))) {
        const auto& source=j.at("reviewed_sources");
        f->source_rates=column(source,"held_rate_cfs",count);
        f->source_previous_rates=column(source,"previous_rate_cfs",count);
        f->source_pending_runoff=column(source,"pending_runoff_ft3",count);
        f->source_pending_drain=column(source,"pending_drain_ft3",count);
        auto totals=column(source,"totals_ft3",6);
        std::copy(totals.begin(),totals.end(),f->source_totals.begin());
        f->source_storage=source.at("storage_ft3").get<double>();
        require(std::isfinite(f->source_storage) && f->source_storage >= 0,"Invalid source storage");
        for (const auto* values : {&f->source_rates,&f->source_previous_rates,&f->source_pending_runoff,&f->source_pending_drain,&totals})
            require(std::all_of(values->begin(),values->end(),[](double v) { return v >= 0; }),"Negative reviewed source water");
    }
    if (groundwater) {
        const auto n = ctx.link_subtypes.conduits.link_idx.size();
        require(integer(j,"conduit_count") == n, "Split conduit count mismatch");
        if (!phase || (f->reply && (phase == SWMM_SPLIT_ADVANCE || phase == SWMM_SPLIT_FINISH))) {
            const auto& c = j.at("groundwater_conduits");
            f->conduits.coupled = flags(c.at("coupled"),n);
            f->conduits.head = column(c,"head_relative_ft",n);
            f->conduits.conductivity = column(c,"conductivity_ft_per_s",n);
            f->conduits.path = column(c,"path_ft",n);
            f->conduits.gain_limit = column(c,"gain_limit_cfs",n);
            for (size_t i=0; i<n; ++i)
                require(f->conduits.conductivity[i] >= 0 && f->conduits.path[i] >= 0 &&
                        f->conduits.gain_limit[i] >= 0 && (!f->conduits.coupled[i] || f->conduits.path[i] > 0),
                        "Invalid split groundwater conduit budget");
            const auto ledger = column(j,"groundwater_ledger_m3",16);
            std::copy(ledger.begin(),ledger.end(),f->groundwater_ledger.begin());
        }
        if (!f->reply && phase == SWMM_SPLIT_ADVANCE)
            f->conduits.seepage = column(j,"conduit_seepage_cfs",n);
    }
    if (!phase) {
        require(f->start == 0, "Geometry must precede advancement");
        require(j.at("node_ids") == ctx.node_names.names(), "Split node IDs/order differ");
        if (spatial) {
            require(j.at("subcatchment_ids") == ctx.subcatch_names.names(),"Split source IDs/order differ");
            const auto& groups=j.at("source_groups");
            require(groups.is_array() && groups.size() == size_t(ctx.n_subcatches()),"Invalid source ownership extent");
            for (const auto& group : groups) {
                require(group.is_number_integer() && group >= -1 && group < ctx.n_subcatches(),"Invalid source owner group");
                f->source_groups.push_back(group.get<int>());
            }
            require(std::any_of(f->source_groups.begin(),f->source_groups.end(),[](int g) { return g >= 0; }),"Empty reviewed source owner");
        }
        if (groundwater) {
            require(j.at("link_ids") == ctx.link_names.names() &&
                    j.at("conduit_links") == ctx.link_subtypes.conduits.link_idx,
                    "Split link IDs/conduit order differ");
            f->aquifer_bed = flags(j.at("aquifer_bed"),count);
        }
        if (sources) require(j.at("gage_ids") == ctx.gage_names.names(), "Split gage IDs/order differ");
        f->model = j.at("configuration").get<std::string>();
        const auto& flags = j.at("coupled");
        require(flags.is_array() && flags.size() == count, "Invalid coupling flag count");
        for (const auto& flag : flags) {
            require(flag.is_number_unsigned() && (flag == 0 || flag == 1), "Invalid coupling flag");
            f->coupled.push_back(flag.get<uint8_t>());
        }
        require(
            groundwater || !f->mapped_outfalls.empty() || std::any_of(f->coupled.begin(), f->coupled.end(), [](auto bit) { return bit != 0; }),
            "Geometry has no coupled nodes");
        f->nodes.ponded_area = column(j, "ponded_area_ft2", count);
        f->pond_area = column(j, "pond_area_ft2", count, true);
        f->pond_band = column(j, "pond_band_ft", count, true);
        for (const auto* amounts : {&f->nodes.ponded_area, &f->pond_area, &f->pond_band})
            require(std::all_of(amounts->begin(), amounts->end(), [](double x) { return x >= 0; }),
                    "Negative footprint geometry");
    } else if (f->reply && phase == SWMM_SPLIT_SOURCE_PREPARE) {
        require(spatial,"Missing reviewed source owner");
    } else if (f->reply && phase == SWMM_SPLIT_OUTFALL_PREPARE) {
        const auto& boundaries = j.at("outfall_boundaries");
        require(boundaries.is_array() && boundaries.size() == f->mapped_outfalls.size(),
                "Missing outfall boundary rows");
        for (const auto& row : boundaries) {
            require(row.is_array() && row.size() == 3, "Invalid outfall boundary row");
            for (const auto& value : row)
                require(value.is_number() && std::isfinite(value.get<double>()),
                        "Invalid outfall boundary number");
            const double head = row[0].get<double>(), ramp = row[1].get<double>(),
                         limit = row[2].get<double>();
            require(ramp >= 0 && ramp <= 1 && limit >= 0,
                    "Invalid outfall ramp or withdrawal limit");
            f->outfall_boundaries.push_back({head, ramp, limit});
        }
    } else if (f->reply && phase == SWMM_SPLIT_CONDUCTANCE) {
        const auto& rows = j.at("conductance_ft2_per_s");
        require(rows.is_array(), "Invalid conductance rows");
        for (const auto& row : rows) {
            require(row.is_array() && row.size() == 2 && row[0].is_number_unsigned() &&
                        row[0].get<uint64_t>() < count,
                    "Invalid conductance node");
            double g = row[1].get<double>();
            require(std::isfinite(g) && g >= 0, "Invalid conductance");
            f->conductance.emplace_back(row[0].get<int>(), g);
        }
    } else {
        f->nodes.coupling_queue = column(j, "queued_ft3", count);
        f->nodes.coupling_volume = column(j, "direct_ft3", count);
        f->remaining = j.at("remaining_s").get<double>();
        require(f->remaining >= 0, "Invalid delivery span");
        if (f->reply) {
            const auto& bal = j.at("balance");
            for (const char* name : {"solver_nsteps", "solver_nrhs"})
                require(bal.at(name).is_number_integer() && bal.at(name) >= -1 &&
                            bal.at(name) <= std::numeric_limits<long>::max(),
                        "Invalid solver counter");
            require(bal.at("solver_n_tiers").is_number_integer() && bal.at("solver_n_tiers") >= 0 &&
                        bal.at("solver_n_tiers") <= 8,
                    "Invalid solver tier count");
            from_json(bal, f->balance);
            const auto& tiers = j.at("tier_cells");
            require(tiers.is_array() && tiers.size() == 8, "Invalid tier cells");
            for (size_t i = 0; i < 8; ++i) {
                require(tiers[i].is_number_unsigned() &&
                            tiers[i] <= std::numeric_limits<long>::max(),
                        "Invalid tier cell count");
                f->balance.solver_tier_cells[i] = tiers[i].get<long>();
            }
        } else {
            f->nodes.head = column(j, "head_ft", count);
            f->nodes.depth = column(j, "depth_ft", count);
            f->nodes.volume = column(j, "volume_ft3", count);
            f->nodes.rpt_full_volume = column(j, "rim_volume_ft3", count);
            f->report_flush = j.at("flush_on_report").get<bool>();
            if (groundwater || !f->mapped_outfalls.empty()) {
                f->nodes.inflow = column(j, "node_inflow_cfs", count);
                f->nodes.outflow = column(j, "node_outflow_cfs", count);
            }
            if (sources && phase == SWMM_SPLIT_ADVANCE) {
                const auto& weather = j.at("weather");
                f->gage_rates = column(weather, "gage_rates", ctx.n_gages());
                const auto& cursors = weather.at("gage_current");
                require(cursors.is_array() && cursors.size() == f->gage_rates.size(), "Invalid gage cursor count");
                for (int g=0; g<ctx.n_gages(); ++g) {
                    const auto& value = cursors[g];
                    require(value.is_number_integer() && value >= -2 && value <= std::numeric_limits<int>::max(),
                            "Invalid gage cursor");
                    const int row = value.get<int>();
                    const auto* table = openswmm::gage::gageRainSeries(ctx,g);
                    require(row < 0 || (table && size_t(row) < table->x.size()), "Gage cursor is outside source series");
                    f->gage_current.push_back(row);
                }
                f->evap_rate = weather.at("evap_ft_per_s").get<double>();
                require(std::isfinite(f->evap_rate), "Invalid weather evaporation");
                const auto& samples = weather.at("samples");
                require(samples.is_array(), "Invalid gage journal");
                double previous = -1;
                for (const auto& sample : samples) {
                    const double start = sample.at("start_s").get<double>();
                    require(std::isfinite(start) && start >= 0 && start > previous && start <= f->start + f->dt,
                            "Invalid gage journal boundary");
                    auto rates = column(sample, "rates", ctx.n_gages());
                    require(std::all_of(rates.begin(), rates.end(), [](double v) { return v >= 0; }),
                            "Negative gage sample");
                    f->weather.push_back({start, std::move(rates)});
                    previous = start;
                }
                require(std::all_of(f->gage_rates.begin(), f->gage_rates.end(), [](double v) { return v >= 0; }),
                        "Negative gage rate");
            }
        }
    }
    return f;
}
}
#endif

extern "C" {
int swmm_split_packet_phase(SWMM_SplitFrame f) {
    return f ? static_cast<openswmm::SplitFrame*>(f)->phase : -1;
}
void swmm_split_packet_free(char* p) {
    delete[] p;
}
int swmm_split_packet_encode(SWMM_Engine e, SWMM_SplitFrame frame, char** out) {
    if (!e)
        return SWMM_ERR_BADHANDLE;
    if (!frame || !out)
        return SWMM_ERR_BADPARAM;
#if defined(OPENSWMM_HAS_HYDROCOUPLE) && defined(OPENSWMM_HAS_2D)
    try {
        auto text = encode(*static_cast<Frame*>(frame), to_engine(e)->context()).dump();
        auto buffer = std::make_unique<char[]>(text.size() + 1);
        std::memcpy(buffer.get(), text.c_str(), text.size() + 1);
        *out = buffer.release();
        return SWMM_OK;
    } catch (const std::bad_alloc&) {
        return SWMM_ERR_NOMEM;
    } catch (const std::exception& ex) {
        to_engine(e)->context().error_message = ex.what();
        return SWMM_ERR_BADPARAM;
    }
#else
    return SWMM_ERR_PLUGIN;
#endif
}
int swmm_split_packet_decode(SWMM_Engine e, const char* bytes, size_t size, SWMM_SplitFrame* out) {
    if (!e)
        return SWMM_ERR_BADHANDLE;
    if (!bytes || !size || !out || std::memchr(bytes, 0, size))
        return SWMM_ERR_BADPARAM;
#if defined(OPENSWMM_HAS_HYDROCOUPLE) && defined(OPENSWMM_HAS_2D)
    try {
        *out = decode(Json::parse(bytes, bytes + size), to_engine(e)->context()).release();
        return SWMM_OK;
    } catch (const std::bad_alloc&) {
        return SWMM_ERR_NOMEM;
    } catch (const std::exception& ex) {
        to_engine(e)->context().error_message = ex.what();
        return SWMM_ERR_BADPARAM;
    }
#else
    return SWMM_ERR_PLUGIN;
#endif
}
}
