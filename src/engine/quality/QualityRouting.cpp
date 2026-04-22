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
#include "../core/UnitConversion.hpp"
#include "../hydraulics/Node.hpp"
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
    updateLinkQuality(ctx, dt);
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

    // Gap #26: LID drain quality — add drain loads to destination node inflows.
    // lid_drain_qual_load[node * np + p] (mass/sec) and lid_drain_qual_vol[node]
    // (CFS) are set once per runoff step in A6b and persist until overwritten.
    // Matches legacy lid_addDrainInflow() (node drain) / lid_addDrainRunon()
    // (subcatch drain routed to that subcatch's outlet node).
    for (int j = 0; j < ctx.n_nodes(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        double drain_vol_rate = (uj < ctx.nodes.lid_drain_qual_vol.size())
                                ? ctx.nodes.lid_drain_qual_vol[uj] : 0.0;
        if (drain_vol_rate <= 0.0) continue;

        ctx.nodes.qual_vol_in[uj] += drain_vol_rate * dt;

        for (int p = 0; p < np; ++p) {
            auto nd_idx = uj * static_cast<std::size_t>(np) + static_cast<std::size_t>(p);
            double load = (nd_idx < ctx.nodes.lid_drain_qual_load.size())
                          ? ctx.nodes.lid_drain_qual_load[nd_idx] : 0.0;
            if (load > 0.0 && nd_idx < ctx.nodes.qual_mass_in.size())
                ctx.nodes.qual_mass_in[nd_idx] += load;
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

    // Link decay is applied within updateLinkQuality() (volume-balance mixing)
    // so no separate per-link decay pass is needed here.
}

// ============================================================================
// Update link quality from upstream node — VECTORISABLE
// ============================================================================

void QualitySolver::updateLinkQuality(SimulationContext& ctx, double dt) {
    int np = n_pollutants_;
    auto& links = ctx.links;
    auto& nodes = ctx.nodes;
    auto& poll  = ctx.pollutants;

    const bool is_steady = (ctx.options.routing_model == RoutingModel::STEADY);

    // Batch over all links — parallelisable (each link writes to its own slot)
#if defined(SWMM_USE_OPENMP)
#pragma omp parallel for schedule(static)
#endif
    for (int j = 0; j < ctx.n_links(); ++j) {
        auto uj = static_cast<size_t>(j);
        double q = std::fabs(links.flow[uj]);

        int upstream = (links.flow[uj] >= 0.0) ? links.node1[uj] : links.node2[uj];
        if (upstream < 0 || upstream >= ctx.n_nodes()) continue;
        auto un = static_cast<size_t>(upstream);

        double v_old = links.old_volume[uj];
        double v_new = links.volume[uj];

        // Matching legacy qualrout.c findLinkQual():
        //   New concentration = volume-balance complete-mixing with upstream
        //   inflow, corrected for volume change, plus first-order in-link decay.
        //
        //   c_new = (c_old * v_old + c_up * q_in * dt - k * c_old * v_old * dt)
        //           / max(v_new, ZERO_VOLUME)
        //
        // When there is no flow, the link retains its old concentration
        // (with decay applied below).

        // Evaporation concentration factor (Gap #2 / legacy findLinkQual):
        //   fEvap = 1 + vEvap / v_old  where vEvap = evapLossRate * nBarrels * dt
        //   Concentrates pollutants when conduit water evaporates.
        double fEvap = 1.0;
        if (v_old > ZERO_VOLUME) {
            int nb = (uj < links.barrels.size()) ? links.barrels[uj] : 1;
            double evap_rate = (uj < links.evap_loss_rate.size()) ? links.evap_loss_rate[uj] : 0.0;
            double v_evap = evap_rate * static_cast<double>(nb) * dt;
            if (v_evap > 0.0) fEvap = 1.0 + v_evap / v_old;
        }

        for (int p = 0; p < np; ++p) {
            auto li = uj * static_cast<size_t>(np) + static_cast<size_t>(p);
            auto ni = un  * static_cast<size_t>(np) + static_cast<size_t>(p);
            if (li >= links.conc.size() || ni >= nodes.conc.size()) continue;

            double c_old = links.conc_old[li];
            double c_up  = nodes.conc[ni];    // upstream node after node mixing

            double k = (static_cast<size_t>(p) < poll.k_decay.size())
                ? poll.k_decay[static_cast<size_t>(p)] : 0.0;

            double c_new;

            if (is_steady) {
                // Gap #38: Steady Flow quality routing (legacy findSFLinkQual).
                // Link conc = upstream node conc, scaled by fEvap then exact
                // exponential decay over the full routing timestep.
                // No volume-balance mixing — steady flow has invariant volumes.
                double c1 = c_up * fEvap;
                c_new = (k > 0.0) ? c1 * std::exp(-k * dt) : c1;
            } else if (q <= 0.0) {
                // No flow: retain old concentration with in-place decay
                // Apply fEvap first (legacy order: fEvap then decay)
                c_new = c_old * fEvap * std::max(1.0 - k * dt, 0.0);
            } else if (v_new <= ZERO_VOLUME) {
                // Zero-volume link — matching legacy qualrout.c findLinkQual:
                // when vNew == 0 the link carries upstream mass instantaneously,
                // so assign upstream node concentration directly.
                c_new = c_up;
            } else {
                // Legacy order (findLinkQual): fEvap → decay → mix with upstream
                //   c1 = c_old * fEvap
                //   c2 = c1 * (1 - k*dt)                 [getReactedQual]
                //   c_new = (c2 * v_old + c_up * q_in * dt) / (v_old + q_in * dt)
                //
                // Volume-change correction for DW (matching legacy line ~337):
                //   qIn += (v2 + vLosses - v1) / tStep
                double q_in = q;
                if (v_new > v_old) q_in += (v_new - v_old) / dt;
                q_in = std::max(q_in, 0.0);

                double c1 = c_old * fEvap;           // evap-concentrated
                double c2 = c1 * std::max(1.0 - k * dt, 0.0);  // decayed
                double w_in = c_up * q_in;           // mass inflow rate

                // getMixedQual: (c2 * v_old + w_in * dt) / (v_old + q_in * dt)
                double denom = v_old + q_in * dt;
                c_new = (denom > ZERO_VOLUME)
                    ? (c2 * v_old + w_in * dt) / denom : c_up;
            }

            c_new = std::max(c_new, 0.0);
            links.conc[li] = c_new;
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
        double vol_in = nodes.qual_vol_in[uj];  // total inflow volume (ft3) this step
        double q_raw  = (dt > 0.0) ? vol_in / dt : 0.0;  // inflow rate (ft3/s)
        for (int p = 0; p < np; ++p) {
            auto mi = uj * static_cast<std::size_t>(np) + static_cast<std::size_t>(p);
            treat.cin[static_cast<std::size_t>(p)] =
                (vol_in > 0.0 && mi < nodes.qual_mass_in.size())
                ? nodes.qual_mass_in[mi] / vol_in : 0.0;
        }

        // 2. Get node state for process variables — apply UCF conversions (Gap #16)
        // Matching legacy treatmnt.c getVariableValue():
        //   pvFLOW  = Q * UCF(FLOW)   — inflow in user display units
        //   pvDEPTH = y * UCF(LENGTH) — avg depth in user display units
        //   pvAREA  = (a1+a2)/2 * UCF(LENGTH)^2 — avg surface area in user display units
        int unit_sys = static_cast<int>(ctx.options.flow_units);
        double ucf_flow   = ucf::UCF(ucf::FLOW,   ctx.options);
        double ucf_length = ucf::UCF(ucf::LENGTH,  ctx.options);

        double hrt_hours = nodes.hrt[uj] / 3600.0;
        double q = q_raw * ucf_flow;                        // flow in user units
        double v = nodes.volume[uj];                         // volume (ft3)
        double d_ft = (nodes.depth[uj] + nodes.old_depth[uj]) * 0.5;
        double d  = d_ft * ucf_length;                      // depth in user units

        // AREA: average surface area at old and new depth (Gap #16)
        double a1 = node::getSurfArea(nodes, j, nodes.old_depth[uj], &ctx.tables,
                                      ucf::getUnitSystem(unit_sys));
        double a2 = node::getSurfArea(nodes, j, nodes.depth[uj],     &ctx.tables,
                                      ucf::getUnitSystem(unit_sys));
        double area = (a1 + a2) * 0.5 * ucf_length * ucf_length;  // user units²

        // 3. Update HRT for storage nodes (matching legacy updateHRT)
        if (nodes.type[uj] == NodeType::STORAGE && v > 0.0) {
            double qdt = std::abs(q_raw) * dt;
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
                treat.cin.data(), treat.removal.data(), np, area);
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

        // 6. Apply removals to nodal concentrations + mass balance (Gap #17)
        // Legacy mass loss formula (treatmnt.c lines 262-263):
        //   massLost = (Cin*q*tStep + oldQual*oldVol - cOut*(q*tStep + oldVol)) / tStep
        // where q is in ft3/s (internal), oldQual/oldVol are pre-quality-step values.
        double v_old = nodes.old_volume[uj];
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

            // Legacy mass loss: accounts for flow-through removal component
            // massLost = (Cin*q*dt + c_old*v_old - cOut*(q*dt + v_old)) / dt
            double c_in_p  = treat.cin[static_cast<std::size_t>(p)];
            double c_old_p = (ci < nodes.conc_old.size()) ? nodes.conc_old[ci] : c_node;
            double mass_lost = 0.0;
            if (dt > 0.0) {
                mass_lost = (c_in_p * q_raw * dt + c_old_p * v_old
                            - cOut * (q_raw * dt + v_old)) / dt;
                mass_lost = std::max(0.0, mass_lost);
            }

            if (mass_lost > 0.0 &&
                static_cast<std::size_t>(p) < ctx.mass_balance.qual_routing_reacted.size()) {
                ctx.mass_balance.qual_routing_reacted[static_cast<std::size_t>(p)]
                    += mass_lost;
            }

            nodes.conc[ci] = cOut;
        }
    }
}

} // namespace quality
} // namespace openswmm
