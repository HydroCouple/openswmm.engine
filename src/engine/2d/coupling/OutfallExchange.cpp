// SPDX-License-Identifier: Apache-2.0

/**
 * @file      OutfallExchange.cpp
 * @brief     Routing-step reservation and application of outfall exchanges.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "OutfallExchange.hpp"
#include "../../core/SimulationContext.hpp"
#include "../../core/CompletedSourceArchive.hpp"
#include "../solver/InertialKernels.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace openswmm::twoD {
/**
 * @brief Build outfall donor partitions, optional spread receivers and native link-limit
 * storage.
 * @param[in] mesh Prepared SI mesh with valid stencils, neighbors, areas and coupling
 * coordinates.
 * @param[in] cps Stable resolved coupling order, with valid outfall node/cell/vertex indices.
 * @param[in,out] ctx Prepared engine graph. Clears/replaces surface_outfall_link_limit; incident
 * links determine equal conservative per-link withdrawal shares.
 * @throws std::runtime_error An outfall has no donor cells or no incident hydraulic link.
 * @throws std::out_of_range Some invalid resolved cell/node indices hit guarded vector accesses;
 * other geometry indices are unchecked and must already be valid.
 * @note Clears prior reservations/window/tailwater state, builds each point's 1/users(cell)
 * donor shares and radius-connected SPREAD set, then initializes native limits to infinity.
 * Allocations can throw after partial reset. Does not transfer water or quality. Requires
 * exclusive context/owner access and matching topology for all future prepare/apply calls.
 */
void OutfallExchange::initialize(const MeshData& mesh, const std::vector<CouplingPoint>& cps,
                                 SimulationContext& ctx) {
    points_.clear();
    nodes_.clear();
    pending_.clear();
    prepared_dt_ = 0;
    prepared_ = false;
    ctx.surface_outfall_link_limit.clear();
    degree_.assign(ctx.n_nodes(), 0);
    node_area_.assign(ctx.n_nodes(), 0);
    node_reserved_.assign(ctx.n_nodes(), 0);
    tw_engaged_s_.assign(ctx.n_nodes(), 0);
    tw_submerged_s_.assign(ctx.n_nodes(), 0);
    tw_max_.assign(ctx.n_nodes(), -1.0e30);
    std::vector<int> users(mesh.n_cells(), 0);
    std::vector<char> seen;
    for (std::size_t k = 0; k < cps.size(); ++k) {
        const auto& cp = cps[k];
        if (!cp.is_outfall)
            continue;
        Point p;
        p.index = static_cast<int>(k);
        p.node = cp.node_idx;
        if (cp.vertex_idx < 0)
            p.cells.push_back(cp.cell_idx);
        else
            for (int j = mesh.vert_stencil_ptr[cp.vertex_idx];
                 j < mesh.vert_stencil_ptr[cp.vertex_idx + 1]; ++j)
                p.cells.push_back(mesh.vert_stencil_idx[j]);
        if (p.cells.empty())
            throw std::runtime_error("2D outfall has no donor cells.");
        for (int c : p.cells) {
            ++users.at(c);
            p.area += mesh.tri_area[c];
        }
        node_area_.at(p.node) += p.area;
        p.reserved.resize(p.cells.size());
        // SPREAD r (plan §6 O4): the cells whose centroid lies within r of
        // the point, gathered outward from its own cells, receive its
        // discharge; withdrawals keep the own cells' budget.
        if (cp.spread > 0) {
            if (seen.empty())
                seen.assign(static_cast<std::size_t>(mesh.n_cells()), 0);
            const double cx =
                cp.vertex_idx >= 0 ? mesh.vx[cp.vertex_idx] : mesh.tri_cx[cp.cell_idx];
            const double cy =
                cp.vertex_idx >= 0 ? mesh.vy[cp.vertex_idx] : mesh.tri_cy[cp.cell_idx];
            p.spread = p.cells;
            p.bed_ref = -std::numeric_limits<double>::infinity();
            std::vector<int> marked = p.cells;
            for (int c : p.cells) {
                seen[static_cast<std::size_t>(c)] = 1;
                p.bed_ref = std::max(p.bed_ref, mesh.tri_cz[c]);
            }
            for (std::size_t q = 0; q < p.spread.size(); ++q) {
                const int c = p.spread[q];
                for (int e = 0; e < mesh.cell_vertex_count(c); ++e) {
                    const int nb = mesh.cell_neighbour(c, e);
                    if (nb < 0 || seen[static_cast<std::size_t>(nb)])
                        continue;
                    seen[static_cast<std::size_t>(nb)] = 1;
                    marked.push_back(nb);
                    const double dx = mesh.tri_cx[nb] - cx, dy = mesh.tri_cy[nb] - cy;
                    if (dx * dx + dy * dy <= cp.spread * cp.spread)
                        p.spread.push_back(nb);
                }
            }
            for (int c : marked)
                seen[static_cast<std::size_t>(c)] = 0;
        }
        if (std::find(nodes_.begin(), nodes_.end(), p.node) == nodes_.end())
            nodes_.push_back(p.node);
        points_.push_back(std::move(p));
    }
    if (points_.empty())
        return;
    pending_.assign(mesh.n_cells(), 0.0);
    for (int j = 0; j < ctx.n_links(); ++j) {
        if (ctx.links.node1[j] >= 0)
            ++degree_[ctx.links.node1[j]];
        if (ctx.links.node2[j] >= 0)
            ++degree_[ctx.links.node2[j]];
    }
    for (auto& p : points_) {
        for (int c : p.cells)
            p.fraction.push_back(1.0 / users[c]);
        if (degree_[p.node] == 0)
            throw std::runtime_error("2D outfall has no incident hydraulic link.");
    }
    ctx.surface_outfall_link_limit.assign(ctx.n_nodes(), std::numeric_limits<double>::infinity());
}

/**
 * @brief Reserve each outfall's disjoint share of frozen surface water for one routing step.
 * @param[in] state Prepared nonnegative SI cell volumes, read only.
 * @param[in] opts Positive finite flow_1d_to_2d conversion from native flow to m3/s.
 * @param[in,out] ctx Receives aggregate-per-barrel native flow limits per coupled outfall node.
 * @param[in] dt Finite positive step s; nonpositive/NaN returns leaving prior reservation state.
 * @note Mesh reference is a retained unused interface argument. Replaces per-point/per-node
 * volume reservations from max(0,state.volume), sets prepared interval/flag when points exist,
 * and divides withdrawal rate across incident links. Does not remove water or reserve native
 * mass separately. Requires initialized shapes and exclusive owner/context access; no allocation
 * or global rollback.
 */
void OutfallExchange::prepare(const MeshData&, const SurfaceStateData& state,
                              const SolverOptions2D& opts, SimulationContext& ctx, double dt) {
    if (!(dt > 0))
        return;
    if (!points_.empty()) {
        prepared_dt_ = dt;
        prepared_ = true;
    }
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

/**
 * @brief Apply completed routed outfall water and donor-paired species to canonical surface
 * cells.
 * @param[in] mesh Prepared SI geometry and reconstruction topology.
 * @param[in,out] state Matching canonical water/native transport and coupling/runoff ledgers.
 * Updates volume, head/depth, signed coupling_applied, native mass and gained/lost_coupling.
 * @param[in] opts Positive native-to-SI flow conversion and wet threshold for SPREAD selection.
 * @param[in,out] ctx Completed native node inflow/outflow, surface_outfall_quality incoming
 * native water/mass queues and 2D mass balance. Negative exchange carries actual surface
 * composition to 1D.
 * @param[in] dt Finite positive routed duration s, matching the intended prepared reservation
 * window; this function does not validate prepared_ or duration equality itself.
 * @param[in,out] accumulated At least original cps.size() entries; adds accepted signed
 * source-positive m3 at original point indices, retaining existing values.
 * @param[in] node_conc Optional borrowed node-major [node*n_species+species] native composition
 * for positive 1D discharge. Missing tuple elements contribute zero mass; null leaves species
 * unchanged.
 * @param[in,out] cell_accumulated Optional mesh-sized signed m3 receipt vector, incremented per
 * cell; extents are unchecked and it must not alias active owner vectors.
 * @return True when any nonzero point exchange was applied; false otherwise. Clears prepared_
 * after normal completion, expiring unused reservations without resetting diagnostic donor
 * totals.
 * @throws std::runtime_error Routed withdrawal exceeds point/node reservation, overlaps
 * overspend current cell water, or no positive distribution weight exists.
 * @note Positive discharge spreads by area over qualified cells; withdrawals use prepared donor
 * shares. Books gross step direction before later netting and pending per-cell receipts.
 * Allocation or a later refusal can follow earlier canonical/quality/ledger updates; no
 * whole-call rollback. Requires initialize/prepare at correct routing cadence and exclusive
 * coupled-owner access.
 */
bool OutfallExchange::apply(const MeshData& mesh, SurfaceStateData& state,
                            const SolverOptions2D& opts, SimulationContext& ctx, double dt,
                            std::vector<double>& accumulated, const std::vector<double>* node_conc,
                            std::vector<double>* cell_accumulated) {
    bool changed = false;
    auto& tr = state.transport;
    for (const auto& p : points_) {
        const double node_volume =
            (ctx.nodes.inflow[p.node] - ctx.nodes.outflow[p.node]) * opts.flow_1d_to_2d * dt;
        const double node_weight = node_volume < 0 ? node_reserved_[p.node] : node_area_[p.node];
        const double point_weight = node_volume < 0 ? p.total : p.area;
        if (node_volume < 0 && -node_volume > node_weight + 1e-10 * std::max(1.0, node_weight))
            throw std::runtime_error("1D outfall exceeded its reserved surface water budget.");
        const double volume = node_weight > 0 ? node_volume * (point_weight / node_weight) : 0;
        if (volume == 0)
            continue;
        if (-volume > p.total + 1e-10 * std::max(1.0, p.total))
            throw std::runtime_error("1D outfall exceeded its reserved surface water budget.");
        // SPREAD: a discharge lands on the own cells plus every cell in the
        // radius that is wet or no higher than the highest own cell (area-
        // weighted); a withdrawal draws on the own cells' reservations.
        std::vector<int> recv;
        if (volume > 0 && !p.spread.empty())
            for (std::size_t j = 0; j < p.spread.size(); ++j) {
                const int c = p.spread[j];
                if (j < p.cells.size() || state.depth[c] >= opts.dry_depth ||
                    mesh.tri_cz[c] <= p.bed_ref)
                    recv.push_back(c);
            }
        const std::vector<int>& cells = recv.empty() ? p.cells : recv;
        double total_weight = volume < 0 ? p.total : 0;
        if (volume > 0)
            for (int c : cells)
                total_weight += mesh.tri_area[c];
        if (!(total_weight > 0))
            throw std::runtime_error("Nonzero outfall exchange without a usable surface budget.");
        double remaining = std::fabs(volume), applied = 0;
        std::size_t last = 0;
        for (std::size_t j = 0; j < cells.size(); ++j)
            if ((volume < 0 ? p.reserved[j] : mesh.tri_area[cells[j]]) > 0)
                last = j;
        for (std::size_t j = 0; j < cells.size(); ++j) {
            const int c = cells[j];
            const double weight = volume < 0 ? p.reserved[j] : mesh.tri_area[c];
            if (!(weight > 0))
                continue;
            double take = j == last
                              ? remaining
                              : std::min(remaining, std::fabs(volume) * (weight / total_weight));
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
                        q1d.mass[un * static_cast<std::size_t>(np_1d) +
                                 static_cast<std::size_t>(sp)] += m1;
                    else if (sp >= tr.n_pollut && sp < tr.n_pollut + tr.n_msx &&
                             sp - tr.n_pollut < ns_1d)
                        q1d.msx[un * static_cast<std::size_t>(ns_1d) +
                                static_cast<std::size_t>(sp - tr.n_pollut)] += m1;
                    else if (sp == tr.age_row)
                        q1d.age_vol[un] += m1;
                    else if (sp == tr.temp_row)
                        q1d.temp_vol[un] += m1;
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
            if (cell_accumulated)
                (*cell_accumulated)[c] += dv;
            if (!tr.cell_runoff_vol.empty())
                tr.cell_runoff_vol[c] -= dv;
            inertial::cellEtaDepth(mesh, opts, c, state.volume[c], state.head[c], state.depth[c]);
            applied += dv;
        }
        accumulated[p.index] += applied;
        // Book each routing-step direction before a surface batch can net
        // a withdrawal against a later discharge. Match the 1D boundary
        // ledger and keep live storage current between surface advances.
        auto& mb = ctx.mass_balance_2d;
        if (applied > 0)
            mb.outfall_in += applied;
        else
            mb.outfall_out -= applied;
        mb.final_storage += applied;
        changed = true;
    }
    prepared_ = false;
    return changed;
}

/**
 * @brief Accumulate distinct outfall tailwater engagement and crown-submergence diagnostics.
 * @param[in] ctx Prepared outfall subtype/cache arrays and native node invert/full depth.
 * @param[in] dt Finite positive routed duration s; nonpositive/NaN returns.
 * @note Adds engagement when ramp>0 and cached stage exceeds invert; adds submerged duration
 * when stage reaches crown, and updates maximum native length. Multiple points share one
 * distinct-node record. Does not apply boundary stage or water; requires matching stable
 * identity/exclusive counters.
 */
void OutfallExchange::recordTailwater(const SimulationContext& ctx, double dt) {
    if (!(dt > 0))
        return;
    for (const int n : nodes_) {
        const int row = ctx.node_subtypes.outfall_row(n);
        if (row < 0)
            continue;
        const auto ur = static_cast<std::size_t>(row), un = static_cast<std::size_t>(n);
        const double h = ctx.node_subtypes.outfalls.head_2d[ur];
        const double z = ctx.nodes.invert_elev[un];
        // The predicate Outfall::setAllOutfallDepths applies the override on.
        if (!(ctx.node_subtypes.outfalls.ramp_2d[ur] > 0 && h > z))
            continue;
        tw_engaged_s_[un] += dt;
        if (h >= z + ctx.nodes.full_depth[un])
            tw_submerged_s_[un] += dt;
        tw_max_[un] = std::max(tw_max_[un], h);
    }
}

bool OutfallExchange::tailwater(int node, double& engaged_s, double& submerged_s,
                                double& max_stage) const {
    if (std::find(nodes_.begin(), nodes_.end(), node) == nodes_.end())
        return false;
    const auto un = static_cast<std::size_t>(node);
    engaged_s = tw_engaged_s_[un];
    submerged_s = tw_submerged_s_[un];
    max_stage = tw_max_[un];
    return true;
}

/**
 * @brief Archive mutable routing-window reservations and cumulative tailwater diagnostics.
 * @param[in,out] a Completed-source archive, writing or reading into matching initialized
 * shapes.
 * @note Includes pending cell receipts, node/point reservations, prepared interval/flag and
 * tailwater records when points exist. Model topology is separately initialized. No semantic
 * validation occurs here; caller invokes validateCheckpoint() before adopting a staged restore.
 * Archive failures can leave partial restored fields; requires exclusive stable owner access.
 */
void OutfallExchange::checkpoint(detail::CompletedSourceArchive& a) {
    a.fixed(pending_, node_reserved_);
    a(prepared_dt_, prepared_);
    for (auto& p : points_) {
        a.fixed(p.reserved);
        a(p.total);
    }
    if (!points_.empty())
        a.fixed(tw_engaged_s_, tw_submerged_s_, tw_max_);
}

/**
 * @brief Validate a restored completed outfall window against prepared topology and accepted
 * ledgers.
 * @param[in] opts Matching positive native-to-SI flow conversion.
 * @param[in] saved_limits Saved native per-link donor-rate limits, node-sized when outfalls
 * exist.
 * @param[in] accumulated Saved signed point m3 receipts, in original coupling order.
 * @param[in] changed Saved window-change flag; false requires all point/cell receipts zero.
 * @param[in] ledger Gross accepted [inward,outward] water m3, finite nonnegative by enclosing
 * validation.
 * @throws std::runtime_error Unconsumed reservation, invalid shape/value, inconsistent
 * reservation sum/limit/tailwater or receipt-versus-ledger mismatch.
 * @throws std::out_of_range Invalid prepared point/cell mappings encountered through guarded
 * access.
 * @note Allocates local validation arrays, reads only owner/arguments and does not repair or
 * adopt state. Requires matching initialized parallel vector shapes and completed routing phase.
 * Numerical receipt checks use relative 1e-12 while saved donor totals/limits are compared
 * exactly.
 */
void OutfallExchange::validateCheckpoint(const SolverOptions2D& opts,
                                         const std::vector<double>& saved_limits,
                                         const std::vector<double>& accumulated, bool changed,
                                         const std::array<double, 2>& ledger) const {
    if (prepared_ || !std::isfinite(prepared_dt_) || prepared_dt_ < 0)
        throw std::runtime_error("Outfall checkpoint has an unconsumed or invalid reservation.");
    if (saved_limits.size() != (points_.empty() ? 0 : degree_.size()))
        throw std::runtime_error("Outfall checkpoint donor-limit shape differs.");
    std::vector<double> reserved(degree_.size(), 0),
        limits(degree_.size(), std::numeric_limits<double>::infinity());
    std::vector<uint8_t> donors(pending_.size(), 0), mapped(accumulated.size(), 0);
    double window = 0, cells = 0;
    for (const auto& p : points_) {
        mapped.at(p.index) = 1;
        double total = 0;
        for (std::size_t j = 0; j < p.cells.size(); ++j) {
            const double v = p.reserved[j];
            if (!std::isfinite(v) || v < 0)
                throw std::runtime_error("Invalid outfall checkpoint reservation.");
            total += v;
            donors.at(p.cells[j]) = 1;
        }
        for (int c : p.spread)
            donors.at(c) = 1; // SPREAD receivers book receipts too
        if (!std::isfinite(total) || total != p.total || (prepared_dt_ == 0 && total != 0))
            throw std::runtime_error("Outfall checkpoint reservation totals differ.");
        reserved[p.node] += total;
        if (prepared_dt_ > 0) {
            if (std::isinf(limits[p.node]))
                limits[p.node] = 0;
            limits[p.node] += total / prepared_dt_ / degree_[p.node] / opts.flow_1d_to_2d;
        }
    }
    if (reserved != node_reserved_ || (!points_.empty() && limits != saved_limits))
        throw std::runtime_error("Outfall checkpoint donor limits differ from their reservations.");
    for (std::size_t n = 0; n < tw_engaged_s_.size(); ++n)
        if (!std::isfinite(tw_engaged_s_[n]) || tw_engaged_s_[n] < 0 ||
            !std::isfinite(tw_submerged_s_[n]) || tw_submerged_s_[n] < 0 ||
            tw_submerged_s_[n] > tw_engaged_s_[n] || std::isnan(tw_max_[n]))
            throw std::runtime_error("Invalid outfall checkpoint tailwater record.");
    for (std::size_t k = 0; k < accumulated.size(); ++k) {
        if (!std::isfinite(accumulated[k]) || (!mapped[k] && accumulated[k] != 0) ||
            (!changed && accumulated[k] != 0))
            throw std::runtime_error("Invalid outfall checkpoint point receipt.");
        window += accumulated[k];
    }
    for (std::size_t c = 0; c < pending_.size(); ++c) {
        if (!std::isfinite(pending_[c]) || (!donors[c] && pending_[c] != 0) ||
            (!changed && pending_[c] != 0))
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
