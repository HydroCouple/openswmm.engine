// SPDX-License-Identifier: Apache-2.0

/**
 * @file      LidNodeAquifer.cpp
 * @brief     Water exchange between LID nodes and aquifers.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "LidNodeAquifer.hpp"
#ifdef OPENSWMM_HAS_2D
#include "LidNodeRichardsState.hpp"
#include "../core/SimulationContext.hpp"
#include "../hydraulics/Node.hpp"
#include "../hydraulics/Routing.hpp"
#include "../core/DateTime.hpp"
#include "../2d/subsurface/LidNodeExchange.hpp"
#include "../2d/data/SurfaceStateData.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <optional>
#include <type_traits>

namespace openswmm::lidnode {
namespace {
/** @brief Qualify and settle one real-node/aquifer cohort using private column drafts.
 * @param[in,out] ctx Compatible initialized water-only context, exact closed owner
 * clocks and no unresolved port/capture trials. Native units convert to SI columns;
 * router mode mutates this context during trials, so that caller must supply a copy.
 * @param[in,out] gw Active matching aquifer with unique resolved beds and reservations.
 * @param[in,out] surface Compatible surface owner for direct/tier settlement.
 * @param start Finite nonnegative owner-start elapsed seconds.
 * @param end Finite later endpoint seconds with finite positive difference.
 * @param evaporation Finite nonnegative held potential ft/s.
 * @param input Nonempty distinct valid Mesh Richards storage node indices; sorted
 * copy determines owner order. Must include complete lower-level firing cohort.
 * @param tier Negative selects ordinary firing acceptance, nonnegative a due tier.
 * @param router Optional borrowed private native router; requires a private ctx,
 * fixed forcing and compatible step_ends. Route callback checks continuous clocks,
 * convergence, finite hydraulic state and no flooding, finishes ports each substep.
 * @param step_ends Native routing endpoints supplied only with router mode.
 * @param queued True trials against current aquifer budgets then retains gross
 * receipts before publishing nodes; physical groundwater waits for owner firing.
 * False settles copied aquifer/surface and publishes them after all drafts qualify.
 * @return Reports/receipts or nonempty error. Validates geometry/state, derives finite
 * pressure/storage envelopes from node/aquifer/port heads, builds SI columns and
 * checks actual accepted surface supply against requested positive lateral volume.
 * Normal queued/direct refusals before publication retain real node water; helper
 * allocations and lower-level calls outside the draft catch can throw. Router-mode
 * ctx changes remain private to its caller. Does not provide general rollback of
 * external callbacks/owners; final draft publication is noexcept. Serialize all use. */
twoD::RichardsBedAcceptance acceptNodes(SimulationContext& ctx, twoD::SubsurfaceSolver& gw,
                                        twoD::SurfaceStateData& surface, double start, double end,
                                        double evaporation, const std::vector<int>& input, int tier,
                                        Router* router = nullptr,
                                        const std::vector<double>& step_ends = {},
                                        bool queued = false) {
    using twoD::RichardsBedAcceptance;
    const auto fail = [](const std::string& error) {
        RichardsBedAcceptance r;
        r.error = error;
        return r;
    };
    if (!std::isfinite(start) || start < 0 || !std::isfinite(end) || end <= start ||
        !std::isfinite(end - start) || !std::isfinite(evaporation) || evaporation < 0 ||
        input.empty())
        return fail("Invalid Richards node owner interval/evaporation.");
    if (ctx.n_pollutants() > 0 || ctx.options.water_age || ctx.reactions.configured)
        return fail("Joint Richards node adoption requires water-only routing.");
    if (ctx.options.heat_transport && !gw.heat().active())
        return fail("Mesh LID heat requires an active porous-ground thermal owner.");
    if (!ctx.nodes.aquifer2d_bed.empty() &&
        ctx.nodes.aquifer2d_bed.size() != std::size_t(ctx.n_nodes()))
        return fail("Invalid Richards node aquifer ownership layout.");
    auto nodes = input;
    std::sort(nodes.begin(), nodes.end());
    if (std::adjacent_find(nodes.begin(), nodes.end()) != nodes.end())
        return fail("Duplicate Richards node owner.");
    const double dt = end - start;
    auto& st = ctx.node_subtypes.storages;
    std::vector<int> bed_by_node(ctx.n_nodes(), -1), slot(ctx.n_nodes(), -1);
    for (std::size_t b = 0; b < gw.nodeBeds().size(); ++b) {
        const auto& bed = gw.nodeBeds()[b];
        if (!bed.exchange || bed.node < 0 || bed.node >= ctx.n_nodes())
            continue;
        auto& owner = bed_by_node[bed.node];
        owner = owner == -1 ? int(b) : -2;
    }
    std::vector<int> rows, beds;
    std::vector<richards::Column> columns;
    for (int n : nodes) {
        if (n < 0 || n >= ctx.n_nodes())
            return fail("Unknown Richards node owner.");
        const int r = ctx.node_subtypes.storage_row(n);
        if (r < 0 || ctx.nodes.type[n] != NodeType::STORAGE || !st.lid_state[r].richards ||
            st.lid[r].bottom != LidNodeBottom::MeshAquifer)
            return fail("Richards node owner requires an initialized Mesh column.");
        const auto& s = st.lid_state[r];
        const auto stack = layers(ctx, st.lid[r].control);
        if (s.richards_completed_time != start || s.richards_mesh_pending_finish ||
            s.captured_flow != 0 || s.mobile_delta != 0 ||
            std::any_of(s.port_delta.begin(), s.port_delta.end(), [](double v) { return v != 0; }))
            return fail("Richards node interval is stale or has unfinished forcing/port trials.");
        if (stack.empty() || s.cells.size() < 2 || s.richards_water.size() != s.cells.size() ||
            s.richards_pressure.size() != s.cells.size() || s.port_delta.size() != s.cells.size())
            return fail("Invalid Richards node geometry/state layout.");
        for (const auto& c : s.cells)
            if (c.layer < 1 || c.layer > int(stack.size()))
                return fail("Invalid Richards node layer mapping.");
        if (!std::isfinite(ctx.nodes.lat_flow[n]) || !std::isfinite(ctx.nodes.invert_elev[n]) ||
            !std::isfinite(ctx.nodes.full_depth[n]) || !std::isfinite(ctx.nodes.sur_depth[n]) ||
            ctx.nodes.full_depth[n] <= 0 || ctx.nodes.sur_depth[n] < 0 ||
            (ctx.options.allow_ponding && ctx.nodes.ponded_area[n] > 0))
            return fail("Invalid or unqualified ponded Richards hydraulic owner geometry.");
        if (!std::isfinite(ctx.climate_state.infil_factor) || ctx.climate_state.infil_factor < 0 ||
            !std::isfinite(st.evap_frac[r]) || st.evap_frac[r] < 0 || st.evap_frac[r] > 1)
            return fail("Invalid Richards node climate/evaporation factors.");
        const int bed = bed_by_node[n];
        if (bed == -2)
            return fail("Richards node has multiple unresolved bed owners.");
        if (bed < 0 || !gw.active() || gw.nodeBeds()[bed].cell < 0 ||
            gw.nodeBeds()[bed].cell >= gw.state().n_cells)
            return fail("Richards node has no enabled resolved aquifer bed.");
        twoD::SubsurfaceSolver::NodeWaterBudget budget;
        const auto budget_error = gw.nodeWaterBudget(gw.nodeBeds()[bed].cell, dt, budget);
        if (!budget_error.empty())
            return fail(budget_error);
        slot[n] = int(rows.size());
        rows.push_back(r);
        beds.push_back(bed);
        columns.push_back(detail::column(ctx, r, dt, evaporation, true));
    }
    constexpr double metres = .3048, cubic_metres = .028316846592;
    std::vector<double> port_heads(nodes.size(), 0);
    // Scan native topology once for the vector batch, rather than once per node.
    for (int link = 0; link < ctx.n_links(); ++link) {
        const int ends[]{ctx.links.node1[link], ctx.links.node2[link]};
        for (int side = 0; side < 2; ++side) {
            const int n = ends[side], other = ends[1 - side];
            if (n < 0 || n >= ctx.n_nodes() || slot[n] < 0)
                continue;
            if (other < 0 || other >= ctx.n_nodes())
                return fail("Invalid Richards port topology.");
            const double h =
                (ctx.nodes.invert_elev[other] +
                 portDepth(ctx, other, portOffset(ctx, link, other)) - ctx.nodes.invert_elev[n]) *
                metres;
            if (!std::isfinite(h))
                return fail("Invalid Richards port head snapshot.");
            port_heads[slot[n]] = std::max(port_heads[slot[n]], h);
        }
    }
    std::vector<twoD::RichardsBedOwner> owners;
    for (std::size_t j = 0; j < nodes.size(); ++j) {
        const int n = nodes[j], r = rows[j];
        const auto& s = st.lid_state[r];
        const auto& c = columns[j];
        const auto& bed = gw.nodeBeds()[beds[j]];
        const auto& g = gw.state();
        // A finite pressure envelope from actual geometry and available heads,
        // including backwater at native layer ports and existing elastic water.
        // The visible surface head is not the porous boundary sample.
        double max_head =
            std::max({(ctx.nodes.full_depth[n] + ctx.nodes.sur_depth[n]) * metres,
                      g.z_bed[bed.cell] + g.hg[bed.cell] - ctx.nodes.invert_elev[n] * metres,
                      port_heads[j]});
        for (std::size_t k = 1; k < c.cells.size(); ++k) {
            const double h =
                c.cells[k].z + richards::pressure(c.cells[k].material, c.cells[k].theta_s,
                                                  c.water[k] / c.cells[k].volume);
            if (!std::isfinite(h))
                return fail("Invalid Richards porous pressure snapshot.");
            max_head = std::max(max_head, h);
        }
        const auto& top = s.cells.front();
        std::vector<double> maximum{
            std::clamp(ctx.nodes.full_depth[n] - top.bottom, 0., top.top - top.bottom) * top.area *
            top.porosity * cubic_metres};
        for (std::size_t k = 1; k < c.cells.size(); ++k)
            maximum.push_back(
                std::max(c.water[k], richards::storage(c.cells[k].material, c.cells[k].theta_s,
                                                       max_head - c.cells[k].z) *
                                         c.cells[k].volume));
        owners.push_back({beds[j], &columns[j], ctx.nodes.invert_elev[n] * metres,
                          bottomCloggingFactor(ctx, r), std::move(maximum)});
    }
    // The lower-level transaction commits only into these private copies. Real
    // node pressure/capacity/forcing views must also qualify before publication.
    std::optional<twoD::SubsurfaceSolver> private_aquifer;
    std::optional<twoD::SurfaceStateData> private_surface;
    if (!queued) {
        private_aquifer.emplace(gw);
        private_surface.emplace(surface);
    }
    auto& aquifer = queued ? gw : *private_aquifer;
    auto& surf = queued ? surface : *private_surface;
    auto node_beds = ctx.nodes.aquifer2d_bed;
    node_beds.resize(ctx.n_nodes(), 0);
    for (int n : nodes)
        node_beds[n] = 1;
    twoD::RichardsBedStepper stepper;
    if (router)
        stepper.route = [&](double a, double b, const auto& cols, const auto& reports, auto& water,
                            auto& claims) -> std::string {
            const double interval = b - a;
            if (ctx.current_time != a)
                return "Private Richards routing clock is discontinuous.";
            ctx.save_lat_qual_state();
            for (std::size_t j = 0; j < nodes.size(); ++j) {
                auto d = detail::draft(ctx, rows[j], cols[j], reports[j], interval, b, true);
                d.state.richards_mesh_start = a;
                detail::publish(ctx, std::move(d));
            }
            prepareStepAfterMesh(ctx, interval, evaporation);
            ctx.save_hyd_state();
            router->step(ctx, interval, evaporation);
            if (!router->lastStepConverged())
                return "Private Richards native dynamic wave did not converge; retry the window.";
            for (int n = 0; n < ctx.n_nodes(); ++n)
                if (!std::isfinite(ctx.nodes.volume[n]) || !std::isfinite(ctx.nodes.depth[n]) ||
                    !std::isfinite(ctx.nodes.head[n]) || !std::isfinite(ctx.nodes.overflow[n]) ||
                    ctx.nodes.overflow[n] != 0)
                    return "Private Richards routing has nonfinite state or unqualified flooding.";
            for (double q : ctx.links.flow)
                if (!std::isfinite(q))
                    return "Nonfinite private Richards link flow.";
            finishStep(ctx);
            for (std::size_t j = 0; j < nodes.size(); ++j) {
                const auto& state = ctx.node_subtypes.storages.lid_state[rows[j]];
                for (const auto& p : state.quality_ports) {
                    if (p.cell > 0 && p.volume < 0)
                        claims[j].first -= p.volume * cubic_metres;
                    if (p.cell == int(water[j].size()) - 1 && p.volume > 0)
                        claims[j].second += p.volume * cubic_metres;
                }
                water[j][0] = ctx.nodes.volume[nodes[j]] * cubic_metres;
                for (std::size_t k = 1; k < water[j].size(); ++k)
                    water[j][k] = state.richards_water[k] * cubic_metres;
            }
            ctx.current_time = b;
            ctx.current_date = datetime::addSeconds(ctx.options.start_date, b);
            ctx.old_elapsed_ms = ctx.elapsed_ms;
            ctx.elapsed_ms += 1000 * interval;
            return {};
        };
    twoD::RichardsBedAcceptance accepted;
    if (queued) {
        std::vector<twoD::RichardsBedBinding> bindings;
        for (const auto& o : owners)
            bindings.push_back({o.bed, o.column, o.invert, o.max_water.back(), o.clogging});
        accepted.trial = twoD::trialRichardsBeds(aquifer, dt, bindings);
        accepted.error = accepted.trial.error;
        if (accepted.error.empty()) {
            for (std::size_t j = 0; j < columns.size(); ++j) {
                columns[j] = accepted.trial.columns[j];
                for (std::size_t k = 0; k < columns[j].water.size(); ++k)
                    if (!std::isfinite(columns[j].water[k]) || columns[j].water[k] < 0 ||
                        columns[j].water[k] > owners[j].max_water[k])
                        return fail(
                            "Mesh Richards column exceeds its hydraulic pressure/storage envelope.");
            }
        }
    } else
        accepted =
            router
                ? twoD::acceptRichardsBedsForTierAcrossSteps(aquifer, surf, tier, start, end,
                                                             step_ends, owners, &stepper)
                : (tier < 0
                       ? twoD::acceptRichardsBedsAtFiring(aquifer, surf, start, end, owners)
                       : twoD::acceptRichardsBedsForTier(aquifer, surf, tier, start, end, owners));
    if (!accepted.error.empty())
        return accepted;
    std::vector<detail::RichardsNodeDraft> drafts;
    try {
        if (!router)
            for (std::size_t j = 0; j < nodes.size(); ++j) {
                auto d = detail::draft(ctx, rows[j], columns[j], accepted.trial.reports[j], dt, end,
                                       true);
                d.state.richards_mesh_start =
                    start; // preserve the caller's exact accepted endpoint pair
                for (double v : {d.volume, d.depth, d.head, d.full_volume, d.lat_flow,
                                 d.state.held_volume, d.state.treated_volume})
                    if (!std::isfinite(v))
                        return fail("Nonfinite Richards routing publication draft.");
                const double requested =
                    std::max(0., ctx.nodes.lat_flow[nodes[j]]) * dt * cubic_metres;
                if (std::abs(accepted.trial.reports[j].inflow - requested) >
                    1e-12 * std::max(1., requested))
                    return fail("Richards accepted forcing does not match the routed interval.");
                drafts.push_back(std::move(d));
            }
    } catch (const std::exception& e) {
        return fail(e.what());
    }
    if (queued) {
        if (auto error = gw.retainLidReceipts(start, end, accepted.trial.receipts); !error.empty())
            return fail(error);
        for (auto& d : drafts)
            detail::publish(ctx, std::move(d));
        return accepted;
    }
    static_assert(std::is_nothrow_move_assignable_v<twoD::SubsurfaceSolver>);
    static_assert(std::is_nothrow_move_assignable_v<twoD::SurfaceStateData>);
    gw = std::move(aquifer);
    surface = std::move(surf);
    ctx.nodes.aquifer2d_bed.swap(
        node_beds); // existing hydraulic checkpoint profile refuses unpaired aquifer owners
    for (auto& d : drafts)
        detail::publish(ctx, std::move(d));
    return accepted;
}
}
void prepareMeshRichardsStep(SimulationContext& ctx, twoD::SubsurfaceSolver& gw,
                             twoD::SurfaceStateData& surface, double dt, double evaporation) {
    std::vector<int> nodes;
    for (int n = 0; n < ctx.n_nodes(); ++n)
        if (richardsMode(ctx, n) && bottomMode(ctx, n) == LidNodeBottom::MeshAquifer)
            nodes.push_back(n);
    if (nodes.empty()) {
        prepareStep(ctx, dt, evaporation);
        return;
    }
    if (!gw.lidReceiptsEnabled())
        throw std::runtime_error("Mesh Richards requires a configured groundwater receipt owner.");
    auto result = acceptNodes(ctx, gw, surface, ctx.current_time, ctx.current_time + dt,
                              evaporation, nodes, -1, nullptr, {}, true);
    if (!result.error.empty())
        throw std::runtime_error(result.error);
    prepareStepAfterMesh(ctx, dt, evaporation);
}
twoD::RichardsBedAcceptance
acceptRichardsNodesAtFiring(SimulationContext& ctx, twoD::SubsurfaceSolver& gw,
                            twoD::SurfaceStateData& surface, double start, double end,
                            double evaporation, const std::vector<int>& nodes) {
    return acceptNodes(ctx, gw, surface, start, end, evaporation, nodes, -1);
}
twoD::RichardsBedAcceptance acceptRichardsNodesForTier(SimulationContext& ctx,
                                                       twoD::SubsurfaceSolver& gw,
                                                       twoD::SurfaceStateData& surface, int tier,
                                                       double start, double end, double evaporation,
                                                       const std::vector<int>& nodes) {
    if (tier < 0) {
        twoD::RichardsBedAcceptance r;
        r.error = "Invalid Richards node groundwater tier.";
        return r;
    }
    return acceptNodes(ctx, gw, surface, start, end, evaporation, nodes, tier);
}
twoD::RichardsBedAcceptance
routeRichardsNodesForTierWindow(SimulationContext& ctx, Router& router, twoD::SubsurfaceSolver& gw,
                                twoD::SurfaceStateData& surface, int tier, double start, double end,
                                const std::vector<double>& step_ends, double evaporation,
                                const std::vector<int>& nodes) {
    const auto fail = [](const std::string& e) {
        twoD::RichardsBedAcceptance r;
        r.error = e;
        return r;
    };
    if (tier < 0 || ctx.current_time != start || ctx.options.routing_model != RoutingModel::DYNWAVE)
        return fail("Invalid private Richards routing tier, model or start clock.");
    for (int l = 0; l < ctx.n_links(); ++l)
        if (ctx.links.type[l] != LinkType::CONDUIT)
            return fail("Private Richards routing window currently requires native conduits.");
    for (int n = 0; n < ctx.n_nodes(); ++n) {
        if (ctx.options.allow_ponding && ctx.nodes.ponded_area[n] > 0)
            return fail("Private Richards routing window has unqualified ponding.");
        if (bottomMode(ctx, n) == LidNodeBottom::MeshAquifer &&
            std::find(nodes.begin(), nodes.end(), n) == nodes.end())
            return fail("Private Richards routing window omitted a global Mesh owner.");
    }
    // Clear live IO/accessor handles in the private context. Native conduit
    // routing reads owned hydraulic data; no callback can publish externally.
    try {
        auto candidate = ctx;
        candidate.twod_io = {};
        candidate.state_accessors = {};
        auto aquifer = gw;
        auto surf = surface;
        auto routing = router.privateDynamicWaveWindow();
        auto accepted = acceptNodes(candidate, aquifer, surf, start, end, evaporation, nodes, tier,
                                    routing.get(), step_ends);
        if (!accepted.error.empty())
            return accepted;
        candidate.twod_io = ctx.twod_io;
        candidate.state_accessors = ctx.state_accessors;
        static_assert(std::is_nothrow_move_assignable_v<SimulationContext>);
        static_assert(std::is_nothrow_move_assignable_v<twoD::SubsurfaceSolver>);
        static_assert(std::is_nothrow_move_assignable_v<twoD::SurfaceStateData>);
        ctx = std::move(candidate);
        gw = std::move(aquifer);
        surface = std::move(surf);
        router.publishDynamicWaveWindow(*routing);
        return accepted;
    } catch (const std::exception& e) {
        return fail(e.what());
    }
}

}
#endif
