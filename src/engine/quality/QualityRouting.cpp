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
#include "Treatment.hpp"
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
    (void)n_nodes;
    (void)n_links;
}

void QualitySolver::execute(SimulationContext& ctx, double dt) {
    if (n_pollutants_ <= 0) return;

    // Reset quality assembly arrays on NodeData
    std::fill(ctx.nodes.qual_mass_in.begin(), ctx.nodes.qual_mass_in.end(), 0.0);
    std::fill(ctx.nodes.qual_vol_in.begin(),  ctx.nodes.qual_vol_in.end(),  0.0);

    addWetWeatherLoads(ctx, dt);   // Subcatchment washoff → nodes
    addRdiiLoads(ctx, dt);         // RDII pollutant loads → nodes
    accumulateLinkLoads(ctx, dt);
    mixAtNodes(ctx, dt);
    applyTreatment(ctx, dt);       // Treatment before decay (matching legacy order)
    applyDecay(ctx, dt);
    updateLinkQuality(ctx);
}

// ============================================================================
// Add subcatchment washoff loads to node quality inflows — VECTORISABLE
// Matches legacy addWetWeatherInflows() in routing.c
// ============================================================================

void QualitySolver::addWetWeatherLoads(SimulationContext& ctx, double dt) {
    int np = n_pollutants_;

    for (int i = 0; i < ctx.n_subcatches(); ++i) {
        auto ui = static_cast<std::size_t>(i);
        int out_node = ctx.subcatches.outlet_node[ui];
        if (out_node < 0 || out_node >= ctx.n_nodes()) continue;
        auto ud = static_cast<std::size_t>(out_node);

        // Time-weighted runoff: blend old and new (trapezoidal rule preserved)
        double q_new = ctx.subcatches.runoff[ui];
        double q_old = ctx.subcatches.old_runoff[ui];
        double q = 0.5 * (q_old + q_new);
        if (q <= 0.0) continue;

        ctx.nodes.qual_vol_in[ud] += q * dt;

        for (int p = 0; p < np; ++p) {
            auto sc_idx = ui * static_cast<std::size_t>(np) + static_cast<std::size_t>(p);
            auto nd_idx = ud * static_cast<std::size_t>(np) + static_cast<std::size_t>(p);

            // Time-weighted concentration
            double c_new = (sc_idx < ctx.subcatches.conc.size()) ? ctx.subcatches.conc[sc_idx] : 0.0;
            double c_old = (sc_idx < ctx.subcatches.conc_old.size()) ? ctx.subcatches.conc_old[sc_idx] : 0.0;

            // Mass flow rate = weighted (q*c) — trapezoidal rule preserved
            double mass_rate = 0.5 * (q_old * c_old + q_new * c_new);

            if (mass_rate > 0.0 && nd_idx < ctx.nodes.qual_mass_in.size()) {
                ctx.nodes.qual_mass_in[nd_idx] += mass_rate;
            }
        }
    }
}

// ============================================================================
// Add RDII pollutant loads to node quality inflows
// Matches legacy addRdiiInflows() quality portion (routing.c:741-749)
// ============================================================================

void QualitySolver::addRdiiLoads(SimulationContext& ctx, double dt) {
    int np = n_pollutants_;
    if (np <= 0) return;
    auto& nodes = ctx.nodes;

    for (int i = 0; i < ctx.n_nodes(); ++i) {
        auto ui = static_cast<std::size_t>(i);
        double q = nodes.rdii_inflow[ui];
        if (q <= 0.0) continue;

        // Add volume inflow from RDII
        nodes.qual_vol_in[ui] += q * dt;

        // Add pollutant mass loads: mass_rate = q * c_rdii[p]
        // Matching legacy: w = q * Pollut[p].rdiiConcen
        OPENSWMM_IVDEP
        for (int p = 0; p < np; ++p) {
            double c_rdii = ctx.pollutants.c_rdii[static_cast<std::size_t>(p)];
            if (c_rdii <= 0.0) continue;
            auto nd_idx = ui * static_cast<std::size_t>(np) + static_cast<std::size_t>(p);
            if (nd_idx < nodes.qual_mass_in.size()) {
                nodes.qual_mass_in[nd_idx] += q * c_rdii;
            }
        }

        // Mass balance: track RDII quality inflow
        for (int p = 0; p < np; ++p) {
            double c_rdii = ctx.pollutants.c_rdii[static_cast<std::size_t>(p)];
            if (c_rdii <= 0.0) continue;
            auto pi = static_cast<std::size_t>(p);
            if (pi < ctx.mass_balance.qual_routing_ii_in.size()) {
                ctx.mass_balance.qual_routing_ii_in[pi] += q * c_rdii * dt;
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

    // Batch over all links — inner loop over pollutants is vectorisable
    for (int j = 0; j < ctx.n_links(); ++j) {
        auto uj = static_cast<size_t>(j);
        double q = std::fabs(links.flow[uj]);
        if (q <= 0.0) continue;

        // Downstream node (based on flow direction)
        int downstream = (links.flow[uj] >= 0.0) ? links.node2[uj] : links.node1[uj];
        if (downstream < 0 || downstream >= ctx.n_nodes()) continue;
        auto ud = static_cast<size_t>(downstream);

        ctx.nodes.qual_vol_in[ud] += q * dt;

        // Vectorisable inner loop over pollutants
        OPENSWMM_IVDEP
        for (int p = 0; p < np; ++p) {
            auto lp = uj * static_cast<size_t>(np) + static_cast<size_t>(p);
            auto np_idx = ud * static_cast<size_t>(np) + static_cast<size_t>(p);

            double c_link = (lp < links.conc_old.size()) ? links.conc_old[lp] : 0.0;
            double mass = q * c_link;
            if (np_idx < ctx.nodes.qual_mass_in.size()) {
                ctx.nodes.qual_mass_in[np_idx] += mass;
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

    // Batch over all nodes — inner loop over pollutants is vectorisable
    // Outer node loop is parallelisable: each node reads only its own
    // pre-computed mass_in and vol_in (scatter phase is complete).
#if defined(SWMM_USE_OPENMP)
#pragma omp parallel for schedule(static)
#endif
    for (int i = 0; i < ctx.n_nodes(); ++i) {
        auto ui = static_cast<size_t>(i);
        double v_old = nodes.old_volume[ui];
        double v_in = nodes.qual_vol_in[ui];

        for (int p = 0; p < np; ++p) {
            auto idx = ui * static_cast<size_t>(np) + static_cast<size_t>(p);
            if (idx >= nodes.conc.size()) continue;

            double c_old = nodes.conc_old[idx];

            if (v_in <= 0.0) {
                nodes.conc[idx] = c_old;
                continue;
            }

            double mass_in = (idx < nodes.qual_mass_in.size()) ? nodes.qual_mass_in[idx] * dt : 0.0;
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
            nodes.conc[idx] = c_new;
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

        OPENSWMM_IVDEP
        for (int i = 0; i < ctx.n_nodes(); ++i) {
            auto idx = static_cast<size_t>(i) * static_cast<size_t>(np) + static_cast<size_t>(p);
            if (idx >= ctx.nodes.conc.size()) continue;
            ctx.nodes.conc[idx] *= decay_factor;
            if (ctx.nodes.conc[idx] < 0.0) ctx.nodes.conc[idx] = 0.0;
        }
    }

    // Decay at links — vectorisable per-pollutant stripe
    for (int p = 0; p < np; ++p) {
        double k = poll.k_decay[static_cast<size_t>(p)];
        if (k == 0.0) continue;
        double decay_factor = 1.0 - k * dt;

        OPENSWMM_IVDEP
        for (int j = 0; j < ctx.n_links(); ++j) {
            auto idx = static_cast<size_t>(j) * static_cast<size_t>(np) + static_cast<size_t>(p);
            if (idx >= ctx.links.conc.size()) continue;
            ctx.links.conc[idx] *= decay_factor;
            if (ctx.links.conc[idx] < 0.0) ctx.links.conc[idx] = 0.0;
        }
    }
}

// ============================================================================
// Update link quality from upstream node — VECTORISABLE
// ============================================================================

void QualitySolver::updateLinkQuality(SimulationContext& ctx) {
    int np = n_pollutants_;
    auto& links = ctx.links;
    auto& nodes = ctx.nodes;

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
            if (li < links.conc.size() && ni < nodes.conc.size()) {
                links.conc[li] = nodes.conc[ni];
            }
        }
    }
}

// ============================================================================
// Apply treatment expressions at nodes — matching legacy treatmnt_treat()
// ============================================================================

void QualitySolver::applyTreatment(SimulationContext& ctx, double dt) {
    int np = n_pollutants_;
    if (np <= 0) return;
    auto& treat = ctx.treatment;
    if (!treat.hasAny()) return;

    auto& nodes = ctx.nodes;
    int nn = ctx.n_nodes();

    for (int j = 0; j < nn; ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (!treat.has_treatment[uj]) continue;

        // 1. Compute inflow concentration: Cin[p] = mass_in[p] / vol_in
        double qIn = nodes.qual_vol_in[uj];  // total inflow volume this step
        for (int p = 0; p < np; ++p) {
            auto mi = uj * static_cast<std::size_t>(np) + static_cast<std::size_t>(p);
            treat.cin[static_cast<std::size_t>(p)] =
                (qIn > 0.0 && mi < nodes.qual_mass_in.size())
                ? nodes.qual_mass_in[mi] / qIn : 0.0;
        }

        // 2. Get node state for process variables
        double hrt_hours = nodes.hrt[uj] / 3600.0;
        double q = nodes.lat_flow[uj];                // current inflow
        double v = nodes.volume[uj];                   // current volume
        double d = (nodes.depth[uj] + nodes.old_depth[uj]) / 2.0;  // avg depth

        // 3. Update HRT for storage nodes (matching legacy updateHRT)
        if (nodes.type[uj] == NodeType::STORAGE && v > 0.0) {
            double qdt = std::abs(q) * dt;
            nodes.hrt[uj] = (nodes.hrt[uj] + dt) * v / (v + qdt);
            hrt_hours = nodes.hrt[uj] / 3600.0;
        }

        // 4. Initialize removal array (-1 = not computed)
        for (int p = 0; p < np; ++p)
            treat.removal[static_cast<std::size_t>(p)] = -1.0;

        // 5. Evaluate treatment for each pollutant (with co-treatment + cycle detection)
        //    Uses the legacy getRemoval() pattern:
        //    - removal[p] == -1: not computed → evaluate now
        //    - removal[p] in [0,1]: already computed → use cached
        //    - removal[p] == 10: currently computing → cycle detected, return 0
        auto getRemoval = [&](int p, auto& self) -> double {
            auto up = static_cast<std::size_t>(p);
            if (treat.removal[up] >= 0.0 && treat.removal[up] <= 1.0)
                return treat.removal[up];  // already computed
            if (treat.removal[up] > 1.0)
                return 0.0;  // cycle detected

            auto idx = uj * static_cast<std::size_t>(np) + up;
            if (idx >= treat.compiled.size() || treat.compiled[idx].tokens.empty()) {
                treat.removal[up] = 0.0;
                return 0.0;
            }

            // Mark as being computed (cycle detection flag)
            treat.removal[up] = 10.0;

            const auto& expr = treat.compiled[idx];
            auto ci_idx = uj * static_cast<std::size_t>(np) + up;
            double c_node = (ci_idx < nodes.conc.size()) ? nodes.conc[ci_idx] : 0.0;
            double c_in = expr.is_removal ? treat.cin[up] : c_node;

            if (c_in <= 0.0 && c_node <= 0.0) {
                treat.removal[up] = 0.0;
                return 0.0;
            }

            // Before evaluation, ensure any R_POLLUT dependencies are resolved
            // The extended evaluate reads from treat.removal[] directly
            // For any R_POLLUT(q) where removal[q]==-1, recursively compute it
            for (const auto& tok : expr.tokens) {
                if (tok.var == treatment::TreatVar::R_POLLUT &&
                    tok.pollut_ref >= 0 && tok.pollut_ref < np) {
                    auto uq = static_cast<std::size_t>(tok.pollut_ref);
                    if (treat.removal[uq] < 0.0)
                        self(tok.pollut_ref, self);
                }
            }

            double result = treatment::evaluate(
                expr, c_in, dt, hrt_hours, q, v, d,
                treat.cin.data(), treat.removal.data(), np);
            result = std::max(result, 0.0);

            if (expr.is_removal) {
                treat.removal[up] = std::min(result, 1.0);
            } else {
                result = std::min(result, c_node);
                treat.removal[up] = (c_node > 0.0) ? 1.0 - result / c_node : 0.0;
            }
            return treat.removal[up];
        };

        for (int p = 0; p < np; ++p)
            getRemoval(p, getRemoval);

        // 6. Apply removals to nodal concentrations + mass balance
        for (int p = 0; p < np; ++p) {
            double R = treat.removal[static_cast<std::size_t>(p)];
            if (R <= 0.0) continue;

            auto ci = uj * static_cast<std::size_t>(np) + static_cast<std::size_t>(p);
            if (ci >= nodes.conc.size()) continue;

            const auto& expr = treat.compiled[ci];
            double c_node = nodes.conc[ci];
            double cOut;

            if (expr.is_removal) {
                double c_in = treat.cin[static_cast<std::size_t>(p)];
                cOut = (c_in > 0.0) ? (1.0 - R) * c_in : c_node;
                cOut = std::min(cOut, c_node);
            } else {
                cOut = (1.0 - R) * c_node;
            }
            cOut = std::max(cOut, 0.0);

            // Mass balance: track mass removed by treatment
            // mass_lost = (c_before - c_after) * volume
            double mass_removed = (c_node - cOut) * nodes.volume[uj];
            if (mass_removed > 0.0 &&
                static_cast<std::size_t>(p) < ctx.mass_balance.qual_routing_reacted.size()) {
                ctx.mass_balance.qual_routing_reacted[static_cast<std::size_t>(p)]
                    += mass_removed;
            }

            nodes.conc[ci] = cOut;
        }
    }
}

} // namespace quality
} // namespace openswmm
