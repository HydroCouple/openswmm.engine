/**
 * @file QualityRouting.cpp
 * @brief Water quality routing — batch SoA, numerically identical to legacy.
 *
 * @details All quality operations use flat [node*n_pollutants+p] indexing
 *          for cache-friendly batch processing. Inner loops are vectorisable.
 *
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "QualityRouting.hpp"
#include "../core/SimulationContext.hpp"
#include "../math/SIMD.hpp"

#include <cmath>
#include <algorithm>
#include <vector>

// OpenMP support — graceful degradation when not available
#if defined(SWMM_USE_OPENMP)
#include <omp.h>
#endif

namespace openswmm {
namespace quality {

// ZERO_VOLUME defined in QualityRouting.hpp

void QualitySolver::init(int n_nodes, int n_links, int n_pollutants) {
    n_pollutants_ = n_pollutants;
    if (n_pollutants <= 0) return;
    node_mass_in_.assign(static_cast<size_t>(n_nodes * n_pollutants), 0.0);
    node_vol_in_.assign(static_cast<size_t>(n_nodes), 0.0);
    (void)n_links;
}

void QualitySolver::execute(SimulationContext& ctx, double dt) {
    if (n_pollutants_ <= 0) return;

    // Reset working arrays
    std::fill(node_mass_in_.begin(), node_mass_in_.end(), 0.0);
    std::fill(node_vol_in_.begin(), node_vol_in_.end(), 0.0);

    addWetWeatherLoads(ctx, dt);   // Subcatchment washoff → nodes
    accumulateLinkLoads(ctx, dt);
    mixAtNodes(ctx, dt);
    applyDecay(ctx, dt);
    updateLinkQuality(ctx);
}

// ============================================================================
// Add subcatchment washoff loads to node quality inflows — VECTORISABLE
// Matches legacy addWetWeatherInflows() in routing.c
// ============================================================================

void QualitySolver::addWetWeatherLoads(SimulationContext& ctx, double dt) {
    int np = n_pollutants_;
    auto& poll = ctx.pollutants;

    for (int i = 0; i < ctx.n_subcatches(); ++i) {
        auto ui = static_cast<std::size_t>(i);
        int out_node = ctx.subcatches.outlet_node[ui];
        if (out_node < 0 || out_node >= ctx.n_nodes()) continue;
        auto ud = static_cast<std::size_t>(out_node);

        // Time-weighted runoff: blend old and new
        // (currently runoff and routing use the same timestep, so f = 1.0)
        double q_new = ctx.subcatches.runoff[ui];
        double q_old = ctx.subcatches.old_runoff[ui];
        double q = 0.5 * (q_old + q_new);  // simple average
        if (q <= 0.0) continue;

        node_vol_in_[ud] += q * dt;

        for (int p = 0; p < np; ++p) {
            auto sc_idx = ui * static_cast<std::size_t>(np) + static_cast<std::size_t>(p);
            auto nd_idx = ud * static_cast<std::size_t>(np) + static_cast<std::size_t>(p);

            // Time-weighted concentration
            double c_new = (sc_idx < poll.subcatch_conc.size()) ? poll.subcatch_conc[sc_idx] : 0.0;
            double c_old = (sc_idx < poll.subcatch_conc_old.size()) ? poll.subcatch_conc_old[sc_idx] : 0.0;

            // Mass flow rate = weighted (q*c)
            double mass_rate = 0.5 * (q_old * c_old + q_new * c_new);

            if (mass_rate > 0.0 && nd_idx < node_mass_in_.size()) {
                node_mass_in_[nd_idx] += mass_rate;
            }
        }
    }
}

// ============================================================================
// Accumulate link mass flows to downstream nodes — VECTORISABLE
// ============================================================================

void QualitySolver::accumulateLinkLoads(SimulationContext& ctx, double dt) {
    int np = n_pollutants_;
    auto& links = ctx.links;
    auto& poll = ctx.pollutants;

    // Batch over all links — inner loop over pollutants is vectorisable
    for (int j = 0; j < ctx.n_links(); ++j) {
        auto uj = static_cast<size_t>(j);
        double q = std::fabs(links.flow[uj]);
        if (q <= 0.0) continue;

        // Downstream node (based on flow direction)
        int downstream = (links.flow[uj] >= 0.0) ? links.node2[uj] : links.node1[uj];
        if (downstream < 0 || downstream >= ctx.n_nodes()) continue;
        auto ud = static_cast<size_t>(downstream);

        node_vol_in_[ud] += q * dt;

        // Vectorisable inner loop over pollutants
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC ivdep
#endif
        for (int p = 0; p < np; ++p) {
            auto lp = uj * static_cast<size_t>(np) + static_cast<size_t>(p);
            auto np_idx = ud * static_cast<size_t>(np) + static_cast<size_t>(p);

            double c_link = (lp < poll.link_conc_old.size()) ? poll.link_conc_old[lp] : 0.0;
            double mass = q * c_link;
            if (np_idx < node_mass_in_.size()) {
                node_mass_in_[np_idx] += mass;
            }
        }
    }
    (void)dt;
}

// ============================================================================
// Complete mixing at all nodes — VECTORISABLE
// ============================================================================

void QualitySolver::mixAtNodes(SimulationContext& ctx, double dt) {
    int np = n_pollutants_;
    auto& nodes = ctx.nodes;
    auto& poll = ctx.pollutants;

    // Batch over all nodes — inner loop over pollutants is vectorisable
    // Outer node loop is parallelisable: each node reads only its own
    // pre-computed mass_in and vol_in (scatter phase is complete).
#if defined(SWMM_USE_OPENMP)
#pragma omp parallel for schedule(static)
#endif
    for (int i = 0; i < ctx.n_nodes(); ++i) {
        auto ui = static_cast<size_t>(i);
        double v_old = nodes.old_volume[ui];
        double v_in = node_vol_in_[ui];

        for (int p = 0; p < np; ++p) {
            auto idx = ui * static_cast<size_t>(np) + static_cast<size_t>(p);
            if (idx >= poll.node_conc.size()) continue;

            double c_old = poll.node_conc_old[idx];

            if (v_in <= 0.0) {
                poll.node_conc[idx] = c_old;
                continue;
            }

            double mass_in = (idx < node_mass_in_.size()) ? node_mass_in_[idx] * dt : 0.0;
            double c_in = mass_in / v_in;
            double c_max = std::max(c_old, c_in);

            double c_new = (v_old > ZERO_VOLUME)
                ? (c_old * v_old + mass_in) / (v_old + v_in)
                : c_in;

            // Evaporation concentration factor (P8-G20)
            // When water evaporates, concentration increases
            double v_new = nodes.volume[ui];
            if (v_new > ZERO_VOLUME && v_new < v_old + v_in) {
                c_new *= (v_old + v_in) / v_new;
            }

            c_new = std::min(c_new, c_max);
            c_new = std::max(c_new, 0.0);
            poll.node_conc[idx] = c_new;
        }
    }
    (void)dt;
}

// ============================================================================
// First-order decay — VECTORISABLE (batch multiply)
// ============================================================================

void QualitySolver::applyDecay(SimulationContext& ctx, double dt) {
    int np = n_pollutants_;
    auto& poll = ctx.pollutants;

    // For each pollutant, pre-compute the decay factor (1 - k*dt) once,
    // then apply it to the contiguous concentration arrays.
    // When np == 1 this becomes a simple scalar multiply over a flat array
    // which is trivially vectorisable.

    // Decay at nodes — vectorisable per-pollutant stripe
    for (int p = 0; p < np; ++p) {
        double k = poll.k_decay[static_cast<size_t>(p)];
        if (k == 0.0) continue;
        double decay_factor = 1.0 - k * dt;

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC ivdep
#endif
        for (int i = 0; i < ctx.n_nodes(); ++i) {
            auto idx = static_cast<size_t>(i) * static_cast<size_t>(np) + static_cast<size_t>(p);
            if (idx >= poll.node_conc.size()) continue;
            poll.node_conc[idx] *= decay_factor;
            if (poll.node_conc[idx] < 0.0) poll.node_conc[idx] = 0.0;
        }
    }

    // Decay at links — vectorisable per-pollutant stripe
    for (int p = 0; p < np; ++p) {
        double k = poll.k_decay[static_cast<size_t>(p)];
        if (k == 0.0) continue;
        double decay_factor = 1.0 - k * dt;

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC ivdep
#endif
        for (int j = 0; j < ctx.n_links(); ++j) {
            auto idx = static_cast<size_t>(j) * static_cast<size_t>(np) + static_cast<size_t>(p);
            if (idx >= poll.link_conc.size()) continue;
            poll.link_conc[idx] *= decay_factor;
            if (poll.link_conc[idx] < 0.0) poll.link_conc[idx] = 0.0;
        }
    }
}

// ============================================================================
// Update link quality from upstream node — VECTORISABLE
// ============================================================================

void QualitySolver::updateLinkQuality(SimulationContext& ctx) {
    int np = n_pollutants_;
    auto& links = ctx.links;
    auto& poll = ctx.pollutants;

    // Batch over all links — parallelisable (each link reads from upstream node, writes to own slot)
#if defined(SWMM_USE_OPENMP)
#pragma omp parallel for schedule(static)
#endif
    for (int j = 0; j < ctx.n_links(); ++j) {
        auto uj = static_cast<size_t>(j);
        int upstream = (links.flow[uj] >= 0.0) ? links.node1[uj] : links.node2[uj];
        if (upstream < 0 || upstream >= ctx.n_nodes()) continue;

        // Copy upstream node concentration to link — vectorisable inner loop
        for (int p = 0; p < np; ++p) {
            auto li = uj * static_cast<size_t>(np) + static_cast<size_t>(p);
            auto ni = static_cast<size_t>(upstream) * static_cast<size_t>(np) + static_cast<size_t>(p);
            if (li < poll.link_conc.size() && ni < poll.node_conc.size()) {
                poll.link_conc[li] = poll.node_conc[ni];
            }
        }
    }
}

} // namespace quality
} // namespace openswmm
