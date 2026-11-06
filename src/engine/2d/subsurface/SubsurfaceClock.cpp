// SPDX-License-Identifier: Apache-2.0
#include "SubsurfaceSolver.hpp"
#include "../data/SolverOptions2D.hpp"
#include "../data/SurfaceStateData.hpp"
#include "../solver/InertialEdges.hpp"
#include "../gw/GwBoundaryForcing.hpp"
#include "core/ProcessPerf.hpp"
#include <atomic>

namespace openswmm::twoD {

void SubsurfaceSolver::prepareClock(double epoch) {
    if (!independentClock() || clock_state_)
        return;
    refreshDtCell(*mesh_, *edges_);
    GwClockState state;
    state.dense.assign(state_.n_cells, -1);
    state.receipts.resize(state_.n_cells);
    state.link_out.assign(state_.n_cells, 0.);
    state.receipt_limit.assign(state_.n_cells, 1e30);
    state.budget_active.assign(state_.n_cells, 0);
    std::vector<double> bounds, accuracy;
    for (int i = 0; i < state_.n_cells; ++i) {
        if (!covers(i))
            continue;
        state.dense[i] = static_cast<int>(bounds.size());
        state.cells.push_back(i);
        bounds.push_back(state_.dt_cell[i]);
        accuracy.push_back(clockAccuracyLimit(i));
    }
    std::vector<GwClock::Face> topology;
    for (int e = 0; e < static_cast<int>(state_.eacc_L.size()); ++e) {
        const int left = state.dense[edges_->cL[e]], right = state.dense[edges_->cR[e]];
        if (left < 0 || right < 0)
            continue;
        state.faces.push_back(e);
        topology.emplace_back(left, right);
    }
    state.schedule.emplace(
        GwClock::Options{epoch, options_.base_step, options_.max_step, opts_->max_timestep}, bounds,
        std::move(topology), accuracy);
    // Keep the legacy kernel's topology lists valid for component archives.
    assignTiers(options_.base_step, 1);
    clock_state_.emplace(std::move(state));
}

struct SubsurfaceSolver::IndependentOwner final : GwClock::Owner {
    SubsurfaceSolver& gw;
    SurfaceStateData& surface;
    GwClockState& state;
    IndependentOwner(SubsurfaceSolver& kernel, SurfaceStateData& s)
        : gw(kernel), surface(s), state(*kernel.clock_state_) {}
    void faces(std::span<const GwClock::Interval> work) override {
        if (work.empty())
            return;
        perf::ProcessTimer timer(perf::ProcessPhase::gw_faces);
        std::vector<int> ids;
        std::vector<double> elapsed;
        ids.reserve(work.size());
        elapsed.reserve(work.size());
        for (const auto& edge : work) {
            ids.push_back(state.faces[edge.id]);
            elapsed.push_back(edge.end - edge.start);
        }
        if (gw.tr_.n_layers > 0 && gw.heat_.active()) {
            std::vector<int> neighbours;
            for (int e : ids) {
                neighbours.push_back(gw.edges_->cL[e]);
                neighbours.push_back(gw.edges_->cR[e]);
            }
            // A dispersion gradient reads the one-ring stencil of both
            // endpoints, including neighbours without a face in this batch.
            if (gw.tr_.dispersion_on) {
                const auto endpoints = neighbours.size();
                for (std::size_t k = 0; k < endpoints; ++k) {
                    const int cell = neighbours[k];
                    for (int p = gw.edges_->cell_ptr[cell]; p < gw.edges_->cell_ptr[cell + 1];
                         ++p) {
                        const int edge = gw.edges_->cell_edge[p];
                        const int other =
                            gw.edges_->cL[edge] == cell ? gw.edges_->cR[edge] : gw.edges_->cL[edge];
                        if (gw.covers(other))
                            neighbours.push_back(other);
                    }
                }
            }
            std::sort(neighbours.begin(), neighbours.end());
            neighbours.erase(std::unique(neighbours.begin(), neighbours.end()), neighbours.end());
            for (int cell : neighbours) {
                if (auto error = gw.refreshSigmaPorewater(cell); !error.empty())
                    throw std::runtime_error(error);
                gw.heat_.refreshView(cell, gw.tr_);
            }
        }
        gw.fireGwFaceList(ids, 0., elapsed);
    }
    void cells(std::span<const GwClock::Interval> work, std::optional<double> through) override {
        std::vector<int> ids;
        std::vector<std::vector<ClockBudget>> rates;
        std::set<int> rungs;
        for (const auto& interval : work) {
            const int cell = state.cells[interval.id];
            ids.push_back(cell);
            rates.push_back(gw.clockBudgets(cell, through.value_or(-1.)));
            for (auto& b : rates.back()) {
                b.in /= interval.end - interval.start;
                b.out /= interval.end - interval.start;
            }
            const int rung = state.schedule->rung(interval.id);
            rungs.insert(rung);
            const double bound = state.schedule->committedBound(interval.id);
            const double ratio = bound / state.schedule->period(rung);
            constexpr std::array<double, 5> edges{1., 2., 4., 8., 16.};
            const auto bin =
                bound >= 1e29 ? 6
                              : std::upper_bound(edges.begin(), edges.end(), ratio) - edges.begin();
            ++state.stability_to_period[bin];
        }
        if (work.empty())
            return;
        gw.source_firing_end_ = work.front().end;
        gw.fireGwCellList(ids, 0., surface, 0., work.front().end, GwFiringOrigin::direct,
                          through.value_or(-1.));
        for (std::size_t j = 0; j < ids.size(); ++j) {
            const int cell = ids[j];
            // Re-size after gathering: future reservations remain physically
            // reserved, but cannot set an observed delivered rate.
            const auto budgets = gw.clockBudgets(cell, through.value_or(-1.));
            double limit = 1e30;
            for (std::size_t k = 0; k < budgets.size(); ++k) {
                if (rates[j][k].in > 0)
                    limit = std::min(limit, budgets[k].room_in / rates[j][k].in);
                if (rates[j][k].out > 0)
                    limit = std::min(limit, budgets[k].room_out / rates[j][k].out);
            }
            state.receipt_limit[cell] = limit;
            state.budget_active[cell] = 0;
        }
        for (int rung : rungs)
            ++state.rung_batches[rung];
    }
    double stability(int id) const override {
        const int cell = state.cells[id];
        gw.refreshCellBound(cell, *gw.mesh_, *gw.edges_);
        return gw.state_.dt_cell[cell];
    }
    double accuracy(int id) const override {
        const int cell = state.cells[id];
        return std::max(gw.options_.base_step,
                        std::min(gw.clockAccuracyLimit(cell), state.receipt_limit[cell]));
    }
    void early(double end, std::vector<int>& due) override {
        if (std::floor(end / gw.options_.base_step) <=
            std::floor(state.schedule->time() / gw.options_.base_step))
            return;
        std::vector<unsigned char> scheduled(state.cells.size(), 0);
        for (int id : due)
            scheduled[id] = 1;
        for (int cell : state.cells) {
            if (scheduled[state.dense[cell]] || state.schedule->completed(state.dense[cell]) == end)
                continue;
            bool trigger = false;
            double excess_in = 0, excess_out = 0, ratio = 0;
            for (const auto& b : gw.clockBudgets(cell, end)) {
                trigger |= (b.in > 0 && b.in >= b.room_in) || (b.out > 0 && b.out >= b.room_out);
                excess_in = std::max(excess_in, std::max(0., b.in - b.room_in));
                excess_out = std::max(excess_out, std::max(0., b.out - b.room_out));
                for (auto [v, room] : {std::pair{b.in, b.room_in}, std::pair{b.out, b.room_out}})
                    if (v > room)
                        ratio = std::max(ratio, room > 0 ? (v - room) / room : 1e30);
            }
            if (!trigger)
                continue;
            due.push_back(state.dense[cell]); // GwClock sorts/deduplicates in O(N log N)
            state.budget_active[cell] = 1;
            const auto k = gw.state_.closure[cell];
            state.excess_in[k] = std::max(state.excess_in[k], excess_in);
            state.excess_out[k] = std::max(state.excess_out[k], excess_out);
            const int bin = ratio == 0     ? 0
                            : ratio <= .25 ? 1
                            : ratio <= .5  ? 2
                            : ratio <= 1   ? 3
                                           : 4;
            ++state.budget_exceedance[k][bin];
        }
    }
};

void SubsurfaceSolver::advanceClock(double end, SurfaceStateData& surface, bool gw_only,
                                    bool finish, const std::function<void()>& publish) {
    prepareClock();
    IndependentOwner owner(*this, surface);
    const double previous = clock_state_->schedule->time();
    clock_state_->schedule->check(end, owner, gw_only, finish, publish);
    // Publish on absolute INFIL_STEP crossings, independent of 1D windows.
    // Each cell retains its authored interval, including a resumed run.
    if (clock_state_->cells.empty())
        return;
    const double interval = state_.infil_interval[clock_state_->cells.front()];
    if (!(interval > 0) || std::floor(end / interval) <= std::floor(previous / interval))
        return;
    for (int cell : clock_state_->cells) {
        const double step = state_.infil_interval[cell];
        if (step > 0 && std::floor(end / step) > std::floor(previous / step))
            publishCellInfiltration(cell, surface, step, end);
    }
}

void SubsurfaceSolver::landClockCells(std::span<const int> mesh_cells, SurfaceStateData& surface) {
    if (!clock_state_ || mesh_cells.empty())
        return;
    std::vector<int> dense;
    for (int cell : mesh_cells)
        if (covers(cell))
            dense.push_back(clock_state_->dense[cell]);
    // An API frame can touch several cells. Validate the entire landing on
    // private owners before installing any new state or prescription.
    auto trial = *this;
    auto mesh_state = surface;
    const bool ground_heat = surface.heat.aquifer_ground == &heat_;
    if (ground_heat)
        mesh_state.heat.aquifer_ground = &trial.heat_;
    if (!trial.node_concentrations_.empty())
        trial.node_row_conc_ = trial.node_concentrations_.data();
    IndependentOwner owner(trial, mesh_state);
    trial.clock_state_->schedule->land(dense, owner);
    *this = std::move(trial);
    if (!node_concentrations_.empty())
        node_row_conc_ = node_concentrations_.data();
    // Groundwater only writes the receiving-rate publication on its surface
    // twin. Keep the router's other live buffers and borrowed views intact.
    std::copy(mesh_state.infil_rate.begin(), mesh_state.infil_rate.end(),
              surface.infil_rate.begin());
}
void SubsurfaceSolver::refreshClockAfterEdit(SurfaceStateData& surface) {
    if (!clock_state_)
        return;
    IndependentOwner owner(*this, surface);
    clock_state_->schedule->refresh(owner);
}

bool SubsurfaceSolver::hasClockReceipts() const {
    return clock_state_ && clock_state_->pending_slots != 0;
}

int SubsurfaceSolver::clockRung(int i) const {
    if (clock_state_ && covers(i))
        return clock_state_->schedule->rung(clock_state_->dense[i]);
    return state_.tier[i];
}

std::pair<double, double> SubsurfaceSolver::pendingClockWater(int i, double through) const {
    const auto amount = [&](GwSlot slot, const auto& values, std::size_t index) {
        return clockAmount(i, slot, values, index, through);
    };
    double incoming = amount(GwSlot::surface, state_.xacc_from_surface, i);
    if (source_owned_[i]) {
        incoming = 0;
        for (const auto& r : source_receipts_[i])
            if (r.end <= through)
                incoming += r.volume;
    }
    double outgoing = amount(GwSlot::et, state_.et_pending, i);
    const auto count = [&](double volume) {
        incoming += std::max(0., volume);
        outgoing += std::max(0., -volume);
    };
    const double link_out = tr_.n_layers ? amount(GwSlot::contact_link_out, tr_.contact_link_out, i)
                            : clock_state_
                                ? amount(GwSlot::contact_link_out, clock_state_->link_out, i)
                                : std::max(0., -state_.lacc[i]);
    incoming += std::max(0., amount(GwSlot::link, state_.lacc, i) + link_out);
    outgoing += link_out;
    for (int b : cell_beds_[i]) {
        if (lid_receipts_enabled_ || clock_state_) {
            incoming += amount(GwSlot::bed_down, node_pending_down_, b);
            outgoing += amount(GwSlot::bed_up, node_pending_up_, b);
            for (const auto& r : lid_receipts_[b])
                if (r.end <= through) {
                    incoming += r.down;
                    outgoing += r.up;
                }
        } else {
            const double node_out =
                tr_.n_layers ? tr_.contact_node_out[b] : std::max(0., state_.nacc[b]);
            incoming += std::max(0., node_out - state_.nacc[b]);
            outgoing += node_out;
        }
    }
    if (!state_.eacc_L.empty())
        for (int p = edges_->cell_ptr[i]; p < edges_->cell_ptr[i + 1]; ++p) {
            const auto e = edges_->cell_edge[p];
            count(edges_->cL[e] == i ? state_.eacc_L[e] : state_.eacc_R[e]);
        }
    return {incoming, outgoing};
}

double SubsurfaceSolver::clockAmount(int cell, GwSlot field, const std::vector<double>& values,
                                     std::size_t index, double through) const {
    if (through < 0)
        return 0;
    double value = values[index];
    if (clock_state_)
        for (const auto& slot : clock_state_->receipts[cell])
            if (slot.field == field && slot.index == index)
                for (auto [end, amount] : slot.packets)
                    if (end > through)
                        value -= amount;
    return value;
}

std::vector<SubsurfaceSolver::ClockBudget> SubsurfaceSolver::clockBudgets(int i,
                                                                          double through) const {
    auto [in, out] = pendingClockWater(i, through);
    const double yield = specificYieldOf(i), A = state_.area[i];
    const double head = options_.head_tolerance * yield * A * std::max(.01, state_.hg[i]);
    const auto budget = [&](double room) {
        return options_.budget_fraction * std::min(head, std::max(0., room));
    };
    std::vector<ClockBudget> result;
    const bool sigma = state_.closure[i] == static_cast<int>(GwClosure::SIGMA);
    if (sigma) {
        double intake = clockAmount(i, GwSlot::surface, state_.xacc_from_surface, i, through);
        if (source_owned_[i]) {
            intake = 0;
            for (const auto& r : source_receipts_[i])
                if (r.end <= through)
                    intake += r.volume;
        }
        const double et = clockAmount(i, GwSlot::et, state_.et_pending, i, through);
        in -= intake;
        out -= et;
        // ET spends the existing column, independently of saturated withdrawals.
        const double L = state_.zs[i] - state_.hg[i];
        result.push_back({0., et, 0., budget((state_.hu[i] - state_.theta_r[i] * L) * A)});
        if (tr_.n_layers == 0)
            result.push_back({intake, 0., budget(infiltrationStorageDeficit(i)), 0.});
        else {
            // Top intake and contacts share the same receiving layer budget.
            const double dzA = L * A / tr_.n_layers;
            for (int j = 0; j < tr_.n_layers; ++j) {
                const auto index = std::size_t(j) * state_.n_cells + i;
                const double v =
                    clockAmount(i, GwSlot::contact_water, tr_.contact_water, index, through);
                in -= v;
                result.push_back({std::max(0., v) + (j == 0 ? intake : 0.), 0.,
                                  budget((state_.theta_s[i] - state_.theta_sigma[index]) * dzA),
                                  0.});
            }
        }
    }
    result.push_back(
        {std::max(0., in), std::max(0., out),
         budget(sigma ? yield * A * (state_.zs[i] - state_.hg[i]) : infiltrationStorageDeficit(i)),
         budget(unreservedDrainableWater(i, yield))});
    return result;
}

double SubsurfaceSolver::clockAccuracyLimit(int i) const {
    const double A = state_.area[i], zs = state_.zs[i];
    double incoming = 0., outgoing = state_.c_loss[i] * state_.hg[i] / zs * A;
    const double head = state_.z_bed[i] + state_.hg[i];
    const auto add = [&](double flow) {
        incoming += std::max(0., flow);
        outgoing += std::max(0., -flow);
    };
    // Internal recharge moves the table without any external receipt. Use
    // the same held-state constitutive flux and yield as the cell kernel;
    // otherwise an isolated draining column can sleep until its stability
    // deadline regardless of the authored head tolerance.
    const auto p = paramsOf(i);
    const auto closure = static_cast<GwClosure>(state_.closure[i]);
    const double length = std::max(0., zs - state_.hg[i]);
    double bottom = state_.theta_r[i];
    const double yield = specificYield(i, p, state_.theta_s[i], length, closure, &bottom);
    double recharge = 0;
    if (length > 0 && closure == GwClosure::SIGMA)
        recharge = soil::conductivity(
            p, soil::suctionAtSaturation(
                   p, std::clamp((bottom - state_.theta_r[i]) /
                                     std::max(state_.theta_s[i] - state_.theta_r[i], 1e-12),
                                 1e-6, 1.)));
    else if (length > 0 && closure == GwClosure::CLOSED_FORM)
        recharge = rawRecharge(i, p, length);
    if (recharge > 0)
        recharge = std::min(recharge, std::max(0., state_.hu[i] - state_.theta_r[i] * length) /
                                          options_.base_step);
    else if (!et_caprise_)
        recharge = 0;
    add(recharge * A);
    if (!state_.eacc_L.empty())
        for (int p = edges_->cell_ptr[i]; p < edges_->cell_ptr[i + 1]; ++p) {
            const int e = edges_->cell_edge[p],
                      j = edges_->cL[e] == i ? edges_->cR[e] : edges_->cL[e];
            if (!covers(j))
                continue;
            const double dh = state_.z_bed[j] + state_.hg[j] - head;
            const double a = state_.Ks[i], b = state_.Ks[j];
            const double K = a + b > 0 ? 2 * a * b / (a + b) : 0;
            add(K * (dh > 0 ? state_.hg[j] : state_.hg[i]) * edges_->xi[e] * edges_->inv_dx[e] *
                dh);
        }
    const double t = cell_completed_.empty() ? 0. : cell_completed_[i];
    if (std::size_t(i) < cell_boundaries_.size())
        for (auto id : cell_boundaries_[i]) {
            const auto pulse = integrateGwBoundary(boundaries_[id], t, 1., head, state_.z_bed[i],
                                                   zs, state_.Ks[i], tr_.n_species);
            incoming += pulse.in;
            outgoing += pulse.out;
        }
    const double speed = std::max(incoming, outgoing) / (A * yield);
    return speed > 0 ? options_.budget_fraction * options_.head_tolerance *
                           std::max(.01, state_.hg[i]) / speed
                     : 1e30;
}

void SubsurfaceSolver::recordClockAmount(int cell, GwSlot type, std::size_t index, double amount,
                                         double end) {
    // Each surface worker owns a distinct cell row. Routing and GW
    // bookings are serial. Ahead-of-march 1D receipts can arrive first.
    auto& row = clock_state_->receipts[cell];
    auto slot = std::find_if(row.begin(), row.end(),
                             [&](const auto& s) { return s.field == type && s.index == index; });
    if (amount == 0)
        return;
    if (slot == row.end()) {
        row.push_back({type, index, {}});
        std::atomic_ref(clock_state_->pending_slots).fetch_add(1, std::memory_order_relaxed);
        slot = std::prev(row.end());
    }
    auto& packets = slot->packets;
    // Without forcing knots the next possible gather is after the last
    // processed check. Coalesce fully delivered history before appending.
    if (packets.size() >= 2 && (cell_sources_.empty() || cell_sources_[cell].empty()) &&
        (cell_boundaries_.empty() || cell_boundaries_[cell].empty()) &&
        packets.back().first <= clock_state_->schedule->time()) {
        for (std::size_t j = 1; j < packets.size(); ++j)
            packets[0].second += packets[j].second;
        packets[0].first = packets.back().first;
        packets.resize(1);
    }
    auto it = std::lower_bound(packets.begin(), packets.end(), end,
                               [](const auto& p, double t) { return p.first < t; });
    if (it != packets.end() && it->first == end)
        it->second += amount;
    else
        packets.insert(it, {end, amount});
}

double SubsurfaceSolver::takeDatedAmount(int cell, GwSlot type, std::vector<double>& field,
                                         std::size_t index, bool peek) {
    const double total = field[index];
    const bool mixed = type == GwSlot::prepaid_heat || type == GwSlot::prepaid_water ||
                       type == GwSlot::prepaid_mass;
    if (!mixed && clock_cutoff_ < 0)
        return 0;
    auto& row = clock_state_->receipts[cell];
    auto slot = std::find_if(row.begin(), row.end(),
                             [&](const auto& s) { return s.field == type && s.index == index; });
    double take = mixed ? total * gather_fraction_ : total;
    if (slot != row.end()) {
        auto& packets = slot->packets;
        auto stop = std::upper_bound(packets.begin(), packets.end(), clock_cutoff_,
                                     [](double t, const auto& p) { return t < p.first; });
        if (stop != packets.end() || (mixed && gather_fraction_ != 1.)) {
            double mature = 0, future = 0;
            for (auto it = packets.begin(); it != stop; ++it)
                mature += it->second;
            if (mixed)
                for (auto it = stop; it != packets.end(); ++it)
                    future += it->second;
            take = mature + (mixed ? (total - (mature + future)) * gather_fraction_ : 0.);
        } else
            take = total;
        if (!peek) {
            if (stop == packets.end()) {
                row.erase(slot);
                --clock_state_->pending_slots; // GW gathers run serially
            } else
                packets.erase(packets.begin(), stop);
        }
    }
    if (!peek)
        field[index] -= take;
    return take;
}

double SubsurfaceSolver::takeInfiltrationMass(int cell, int species, const CellFlux& f) {
    auto k = tr_.idx(species, cell);
    if (!f.eligible_intake_mass)
        return takeClockAmount(cell, GwSlot::surface_mass, tr_.xacc_from_surface, k);
    const double mass = (*f.eligible_intake_mass)[species];
    tr_.xacc_from_surface[k] -= mass;
    return mass;
}
}
