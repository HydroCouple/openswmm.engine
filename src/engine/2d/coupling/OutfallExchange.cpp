// SPDX-License-Identifier: Apache-2.0
#include "OutfallExchange.hpp"
#include "../../core/SimulationContext.hpp"
#include "../../core/CompletedSourceArchive.hpp"
#include "../solver/InertialKernels.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace openswmm::twoD {
void OutfallExchange::initialize(const MeshData& mesh,
        const std::vector<CouplingPoint>& cps, SimulationContext& ctx) {
    points_.clear();
    pending_.clear(); prepared_dt_ = 0; prepared_ = false;
    ctx.surface_outfall_link_limit.clear();
    degree_.assign(ctx.n_nodes(), 0);
    node_area_.assign(ctx.n_nodes(), 0);
    node_reserved_.assign(ctx.n_nodes(), 0);
    std::vector<int> users(mesh.n_cells(), 0);
    for (std::size_t k = 0; k < cps.size(); ++k) {
        const auto& cp = cps[k];
        if (!cp.is_outfall) continue;
        Point p; p.index = static_cast<int>(k); p.node = cp.node_idx;
        if (cp.vertex_idx < 0) p.cells.push_back(cp.cell_idx);
        else for (int j = mesh.vert_stencil_ptr[cp.vertex_idx];
                  j < mesh.vert_stencil_ptr[cp.vertex_idx + 1]; ++j)
            p.cells.push_back(mesh.vert_stencil_idx[j]);
        if (p.cells.empty()) throw std::runtime_error("2D outfall has no donor cells.");
        for (int c : p.cells) { ++users.at(c); p.area += mesh.tri_area[c]; }
        node_area_.at(p.node) += p.area;
        p.reserved.resize(p.cells.size());
        points_.push_back(std::move(p));
    }
    if (points_.empty()) return;
    pending_.assign(mesh.n_cells(), 0.0);
    for (int j = 0; j < ctx.n_links(); ++j) {
        if (ctx.links.node1[j] >= 0) ++degree_[ctx.links.node1[j]];
        if (ctx.links.node2[j] >= 0) ++degree_[ctx.links.node2[j]];
    }
    for (auto& p : points_) {
        for (int c : p.cells) p.fraction.push_back(1.0 / users[c]);
        if (degree_[p.node] == 0)
            throw std::runtime_error("2D outfall has no incident hydraulic link.");
    }
    ctx.surface_outfall_link_limit.assign(ctx.n_nodes(),
                                        std::numeric_limits<double>::infinity());
}

void OutfallExchange::prepare(const MeshData&, const SurfaceStateData& state,
                              const SolverOptions2D& opts, SimulationContext& ctx, double dt) {
    if (!(dt > 0)) return;
    if (!points_.empty()) { prepared_dt_ = dt; prepared_ = true; }
    for (const auto& p : points_) {
        ctx.surface_outfall_link_limit[p.node] = 0;
        node_reserved_[p.node] = 0;
    }
    for (auto& p : points_) {
        p.total = 0;
        for (std::size_t j = 0; j < p.cells.size(); ++j) {
            p.reserved[j] = std::max(0.0, state.volume[p.cells[j]]) * p.fraction[j];
            p.total += p.reserved[j];
        }
        node_reserved_[p.node] += p.total;
        // Conservative equal shares across incident links. This includes
        // parallel conduits and structures; the rate is aggregate-barrel.
        ctx.surface_outfall_link_limit[p.node] +=
            p.total / dt / degree_[p.node] / opts.flow_1d_to_2d;
    }
}

bool OutfallExchange::apply(const MeshData& mesh, SurfaceStateData& state,
        const SolverOptions2D& opts, SimulationContext& ctx, double dt,
        std::vector<double>& accumulated, const std::vector<double>* node_conc,
        std::vector<double>* cell_accumulated) {
    bool changed = false;
    auto& tr = state.transport;
    for (const auto& p : points_) {
        const double node_volume = (ctx.nodes.inflow[p.node] - ctx.nodes.outflow[p.node])
                                   * opts.flow_1d_to_2d * dt;
        const double node_weight = node_volume < 0 ? node_reserved_[p.node] : node_area_[p.node];
        const double point_weight = node_volume < 0 ? p.total : p.area;
        if (node_volume < 0 && -node_volume > node_weight + 1e-10 * std::max(1.0, node_weight))
            throw std::runtime_error("1D outfall exceeded its reserved surface water budget.");
        const double volume = node_weight > 0 ? node_volume * (point_weight / node_weight) : 0;
        if (volume == 0) continue;
        if (-volume > p.total + 1e-10 * std::max(1.0, p.total))
            throw std::runtime_error("1D outfall exceeded its reserved surface water budget.");
        double total_weight = volume < 0 ? p.total : 0;
        if (volume > 0) for (int c : p.cells) total_weight += mesh.tri_area[c];
        if (!(total_weight > 0))
            throw std::runtime_error("Nonzero outfall exchange without a usable surface budget.");
        double remaining = std::fabs(volume), applied = 0;
        std::size_t last = 0;
        for (std::size_t j = 0; j < p.cells.size(); ++j)
            if ((volume < 0 ? p.reserved[j] : mesh.tri_area[p.cells[j]]) > 0) last = j;
        for (std::size_t j = 0; j < p.cells.size(); ++j) {
            const int c = p.cells[j];
            const double weight = volume < 0 ? p.reserved[j] : mesh.tri_area[c];
            if (!(weight > 0)) continue;
            double take = j == last ? remaining : std::min(remaining,
                                      std::fabs(volume) * (weight / total_weight));
            const double old = state.volume[c];
            if (volume < 0) {
                if (take > old + 1e-10 * std::max(1.0, old))
                    throw std::runtime_error("Overlapping outfalls overspent a donor cell.");
                take = std::min(take, std::max(0.0, old));
            }
            remaining -= take;
            const double dv = volume < 0 ? -take : take;
            // Review D2 (2026-10-15): mesh water entering the network through
            // this outfall carries the cell's quality. The water has already
            // reached the outfall hydraulically; its volume and mass join the
            // outfall's mixing at the next quality step
            // (QualitySolver::addQualityOnlyInflows). Before, the mass left
            // the mesh into lost_coupling and the outfall fed the network at
            // its OUTFALL_BACKFLOW_QUALITY value.
            auto& q1d = ctx.surface_outfall_quality;
            const double to_1d = opts.flow_1d_to_2d > 0 ? 1.0 / opts.flow_1d_to_2d : 1.0;
            const int np_1d = ctx.n_pollutants(), ns_1d = ctx.reactions.n_species();
            if (volume < 0 && take > 0 && tr.n_species > 0) {
                q1d.ensure(ctx.n_nodes(), np_1d, ns_1d);
                q1d.vol[static_cast<std::size_t>(p.node)] += take * to_1d;
                q1d.any = true;
            }
            for (int sp = 0; sp < tr.n_species; ++sp) {
                double& mass = tr.cell_mass[tr.idx(sp, c)];
                if (volume < 0) {
                    const double dm = old > 0 ? mass * (take / old) : 0;
                    mass -= dm;
                    tr.lost_coupling[sp] += dm;
                    const auto un = static_cast<std::size_t>(p.node);
                    const double m1 = dm * to_1d;
                    if (sp < tr.n_pollut && sp < np_1d)
                        q1d.mass[un * static_cast<std::size_t>(np_1d) + static_cast<std::size_t>(sp)] += m1;
                    else if (sp >= tr.n_pollut && sp < tr.n_pollut + tr.n_msx && sp - tr.n_pollut < ns_1d)
                        q1d.msx[un * static_cast<std::size_t>(ns_1d) + static_cast<std::size_t>(sp - tr.n_pollut)] += m1;
                    else if (sp == tr.age_row) q1d.age_vol[un] += m1;
                    else if (sp == tr.temp_row) q1d.temp_vol[un] += m1;
                } else if (node_conc) {
                    const auto ix = static_cast<std::size_t>(p.node) * tr.n_species + sp;
                    const double dm = ix < node_conc->size() ? take * (*node_conc)[ix] : 0;
                    mass += dm;
                    tr.gained_coupling[sp] += dm;
                }
            }
            state.volume[c] = old + dv;
            state.coupling_applied[c] += dv;
            pending_[c] += dv;
            if (cell_accumulated) (*cell_accumulated)[c] += dv;
            if (!tr.cell_runoff_vol.empty()) tr.cell_runoff_vol[c] -= dv;
            inertial::cellEtaDepth(mesh, opts, c, state.volume[c], state.head[c], state.depth[c]);
            applied += dv;
        }
        accumulated[p.index] += applied;
        // Book each routing-step direction before a surface batch can net
        // a withdrawal against a later discharge. Match the 1D boundary
        // ledger and keep live storage current between surface advances.
        auto& mb = ctx.mass_balance_2d;
        if (applied > 0) mb.outfall_in += applied;
        else            mb.outfall_out -= applied;
        mb.final_storage += applied;
        changed = true;
    }
    prepared_ = false;
    return changed;
}

void OutfallExchange::checkpoint(detail::CompletedSourceArchive& a) {
    a.fixed(pending_, node_reserved_);
    a(prepared_dt_, prepared_);
    for (auto& p : points_) { a.fixed(p.reserved); a(p.total); }
}

void OutfallExchange::validateCheckpoint(const SolverOptions2D& opts,
        const std::vector<double>& saved_limits, const std::vector<double>& accumulated, bool changed,
        const std::array<double,2>& ledger) const {
    if (prepared_ || !std::isfinite(prepared_dt_) || prepared_dt_ < 0)
        throw std::runtime_error("Outfall checkpoint has an unconsumed or invalid reservation.");
    if (saved_limits.size() != (points_.empty() ? 0 : degree_.size()))
        throw std::runtime_error("Outfall checkpoint donor-limit shape differs.");
    std::vector<double> reserved(degree_.size(), 0), limits(degree_.size(), std::numeric_limits<double>::infinity());
    std::vector<uint8_t> donors(pending_.size(), 0), mapped(accumulated.size(), 0);
    double window = 0, cells = 0;
    for (const auto& p : points_) {
        mapped.at(p.index) = 1;
        double total = 0;
        for (std::size_t j = 0; j < p.cells.size(); ++j) {
            const double v = p.reserved[j];
            if (!std::isfinite(v) || v < 0) throw std::runtime_error("Invalid outfall checkpoint reservation.");
            total += v; donors.at(p.cells[j]) = 1;
        }
        if (!std::isfinite(total) || total != p.total || (prepared_dt_ == 0 && total != 0))
            throw std::runtime_error("Outfall checkpoint reservation totals differ.");
        reserved[p.node] += total;
        if (prepared_dt_ > 0) {
            if (std::isinf(limits[p.node])) limits[p.node] = 0;
            limits[p.node] += total / prepared_dt_ / degree_[p.node] / opts.flow_1d_to_2d;
        }
    }
    if (reserved != node_reserved_ || (!points_.empty() && limits != saved_limits))
        throw std::runtime_error("Outfall checkpoint donor limits differ from their reservations.");
    for (std::size_t k = 0; k < accumulated.size(); ++k) {
        if (!std::isfinite(accumulated[k]) || (!mapped[k] && accumulated[k] != 0) || (!changed && accumulated[k] != 0))
            throw std::runtime_error("Invalid outfall checkpoint point receipt.");
        window += accumulated[k];
    }
    for (std::size_t c = 0; c < pending_.size(); ++c) {
        if (!std::isfinite(pending_[c]) || (!donors[c] && pending_[c] != 0) || (!changed && pending_[c] != 0))
            throw std::runtime_error("Invalid outfall checkpoint cell receipt.");
        cells += pending_[c];
    }
    if (!std::isfinite(window) || !std::isfinite(cells) ||
        std::abs(window - cells) > 1e-12 * std::max({1.0, ledger[0], ledger[1]}) ||
        window > ledger[0] + 1e-12 * std::max(1.0, ledger[0]) ||
        -window > ledger[1] + 1e-12 * std::max(1.0, ledger[1]))
        throw std::runtime_error("Outfall checkpoint receipts differ from the accepted ledger.");
}
}
