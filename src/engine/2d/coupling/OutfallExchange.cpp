// SPDX-License-Identifier: Apache-2.0
#include "OutfallExchange.hpp"
#include "../../core/SimulationContext.hpp"
#include "../solver/InertialKernels.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace openswmm::twoD {
void OutfallExchange::initialize(const MeshData& mesh,
        const std::vector<CouplingPoint>& cps, SimulationContext& ctx) {
    points_.clear();
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
        std::vector<double>& accumulated, const std::vector<double>* node_conc) {
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
            for (int sp = 0; sp < tr.n_species; ++sp) {
                double& mass = tr.cell_mass[tr.idx(sp, c)];
                if (volume < 0) {
                    const double dm = old > 0 ? mass * (take / old) : 0;
                    mass -= dm;
                    tr.lost_coupling[sp] += dm;
                } else if (node_conc) {
                    const auto ix = static_cast<std::size_t>(p.node) * tr.n_species + sp;
                    const double dm = ix < node_conc->size() ? take * (*node_conc)[ix] : 0;
                    mass += dm;
                    tr.gained_coupling[sp] += dm;
                }
            }
            state.volume[c] = old + dv;
            state.coupling_applied[c] += dv;
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
    return changed;
}
}
