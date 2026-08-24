// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

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
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "QualityRouting.hpp"

#include "../transport/components/EulerianArdComponent/ArdConfig.hpp"
#include "../transport/components/ReactionModule/ReactionLegacyBinding.hpp"
#include "../transport/components/HeatModule/HeatLegacy.hpp"
#include "../transport/components/WaterAgeModule/WaterAgeLegacy.hpp"
#include "NegativeSources.hpp"
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

namespace {

/// The external-load loaders below do two jobs: they accumulate per-pollutant
/// MASS into qual_mass_in, and they accumulate the node's total external
/// inflow VOLUME into qual_vol_in. Only the first is pollutant-shaped. E5a's
/// [TRANSPORT_BOUNDARIES] injects `qual_vol_in * concentration`, so an
/// MSX-only model (no [POLLUTANTS] — the nh2cl shape) needs the volume half
/// to run even at np == 0, where every mass loop is already a no-op.
/// Measured before this: a boundary on an MSX-only deck delivered exactly
/// 0.0 while the same deck with one inert pollutant row delivered 8.0.
bool loadersNeeded(int np, const SimulationContext& ctx) {
    // A1a: water age needs the volume half (and the per-loader age-volume
    // contributions) even on a deck with no [POLLUTANTS] — the pure-age
    // model is A1a's motivating configuration (lesson 20).
    // H1: heat needs the volume half for the same reason age does — a
    // temperature-only deck (no [POLLUTANTS]) is a supported configuration
    // and every mass loop is already a no-op at np == 0.
    return np > 0 || transport::ardBoundariesNeedExternalVolumes(ctx) ||
           ctx.options.water_age || ctx.options.heat_transport;
}

/// A1a: one loader's age-volume contribution — `q · age_source` (a RATE,
/// age·ft³/s, the age analogue of qual_mass_in). No-op when WATER_AGE is
/// off or the state is unsized (the ARD engine sizes it at init; the
/// assemble stage zeroes it each step).
inline void addAgeVolume(SimulationContext& ctx, int node, double q,
                         WaterAgeSource src) {
    if (!ctx.options.water_age) return;
    auto& s = ctx.water_age_state.node_age_vol_in;
    const auto un = static_cast<std::size_t>(node);
    if (un >= s.size()) return;
    double age = ctx.water_age_config.source_age(src, node);
    // Z1 (amendment D-Y4): an [INFLOWS] row naming __WATER_AGE__ is the
    // more specific statement of THIS node's inflow age and wins over the
    // source table's EXTERNAL_INFLOW entry (constant or node override).
    // NaN marks "no row at this node" — never a value.
    if (src == WaterAgeSource::EXTERNAL_INFLOW) {
        const auto& ov = ctx.water_age_state.node_ext_inflow_age;
        if (un < ov.size() && !std::isnan(ov[un])) age = ov[un];
    }
    s[un] += q * age;
}

/// H1: one loader's temperature-volume contribution — `q · T_source` (a
/// RATE, °C·ft³/s), the heat analogue of the age channel above and the
/// same seam (master plan §4.3 / D-UT10). Carries temperature-volume
/// rather than Joules because ρw·cp cancel identically until H2 brings the
/// energy fluxes that make them load-bearing — see HeatData.hpp.
inline void addTempVolume(SimulationContext& ctx, int node, double q,
                          HeatSource src) {
    if (!ctx.options.heat_transport) return;
    auto& s = ctx.heat_state.node_temp_vol_in;
    const auto un = static_cast<std::size_t>(node);
    if (un >= s.size()) return;
    s[un] += q * ctx.heat_config.source_temp(src, node);
}

void applyLinkQualityForcing(SimulationContext& ctx, int n_pollutants, double dt) {
    if (n_pollutants <= 0) return;

    auto& f = ctx.forcing;
    if (f.link_quality_mode.empty()) return;

    for (int j = 0; j < ctx.n_links(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        for (int p = 0; p < n_pollutants; ++p) {
            auto flat = uj * static_cast<std::size_t>(n_pollutants)
                      + static_cast<std::size_t>(p);
            if (flat >= f.link_quality_mode.size()
                || flat >= ctx.links.conc.size()) {
                continue;
            }

            if (f.link_quality_mode[flat] == ForcingMode::OVERRIDE) {
                ctx.links.conc[flat] = std::max(f.link_quality_value[flat], 0.0);
            } else if (f.link_quality_mode[flat] == ForcingMode::ADD) {
                ctx.links.conc[flat] =
                    std::max(ctx.links.conc[flat] + f.link_quality_value[flat], 0.0);
                ctx.mass_balance.routing_forcing_qual_inflow[
                    static_cast<std::size_t>(p)] +=
                    f.link_quality_value[flat] * dt;
            }
        }
    }
}

} // namespace

void QualitySolver::init(int n_nodes, int n_links, int n_pollutants) {
    n_pollutants_ = n_pollutants;
    (void)n_nodes;
    (void)n_links;
}

void QualitySolver::assembleExternalLoads(SimulationContext& ctx, double dt) {
    if (!loadersNeeded(n_pollutants_, ctx)) return;

    // Reset quality assembly arrays on NodeData
    std::fill(ctx.nodes.qual_mass_in.begin(), ctx.nodes.qual_mass_in.end(), 0.0);

    // A1a: size + zero the age-volume accumulator alongside the pollutant
    // loads (same lifecycle: assembled per routing step by the loaders).
    if (ctx.options.water_age) {
        auto& ws = ctx.water_age_state;
        if (ws.node_age_vol_in.size() !=
            static_cast<std::size_t>(ctx.n_nodes()))
            // A3: pass the subcatchment count — the 2-arg form defaults it
            // to 0 and would WIPE subarea_age on any re-size.
            ws.resize(ctx.n_nodes(), ctx.n_links(), ctx.n_subcatches());
        std::fill(ws.node_age_vol_in.begin(), ws.node_age_vol_in.end(), 0.0);
    }
    // H1: same lifecycle for the temperature-volume accumulator.
    if (ctx.options.heat_transport) {
        auto& hs = ctx.heat_state;
        if (hs.node_temp_vol_in.size() !=
            static_cast<std::size_t>(ctx.n_nodes()))
            hs.resize(ctx.n_nodes(), ctx.n_links(),
                      ctx.heat_config.global_temp[
                          static_cast<int>(HeatSource::INITIAL_STATE)]);
        std::fill(hs.node_temp_vol_in.begin(), hs.node_temp_vol_in.end(), 0.0);
    }
    std::fill(ctx.nodes.qual_vol_in.begin(),  ctx.nodes.qual_vol_in.end(),  0.0);

    addWetWeatherLoads(ctx, dt);   // Subcatchment washoff → nodes
    addRdiiLoads(ctx, dt);         // RDII pollutant loads → nodes
    addDwfLoads(ctx, dt);          // Dry weather pollutant loads → nodes
    addGwLoads(ctx, dt);           // Groundwater inflow pollutant loads → nodes
    addIfaceLoads(ctx, dt);        // Routing interface file loads → nodes
    addExtInflowLoads(ctx, dt);    // Direct [INFLOWS] CONCEN/MASS loads → nodes
}

// ============================================================================
// Add direct external inflow ([INFLOWS] CONCEN/MASS) pollutant loads.
// Matches legacy routing.c addExternalInflows() pollutant portion:
//   w = inflow value; if CONCEN, w *= node flow; Node[j].newQual[p] += w
// ============================================================================

void QualitySolver::addExtInflowLoads(SimulationContext& ctx, double dt) {
    int np = n_pollutants_;
    if (!loadersNeeded(np, ctx)) return;
    auto& nodes = ctx.nodes;

    for (int i = 0; i < ctx.n_nodes(); ++i) {
        auto ui = static_cast<std::size_t>(i);

        // The direct inflow's own water joins the mixing denominator, matching
        // legacy findNodeQual(), which divides the accumulated mass rate by
        // Node[j].inflow — a total that includes the external lateral inflow.
        double q = nodes.ext_inflow[ui];
        if (q > 0.0) {
            nodes.qual_vol_in[ui] += q * dt;
            addAgeVolume(ctx, i, q, WaterAgeSource::EXTERNAL_INFLOW);
            addTempVolume(ctx, i, q, HeatSource::EXTERNAL_INFLOW);
        }

        if (nodes.ext_qual_mass.empty()) continue;
        for (int p = 0; p < np; ++p) {
            auto nd_idx = ui * static_cast<std::size_t>(np) +
                          static_cast<std::size_t>(p);
            if (nd_idx >= nodes.ext_qual_mass.size()) continue;
            double mass_rate = nodes.ext_qual_mass[nd_idx];
            if (mass_rate == 0.0) continue;
            if (nd_idx < nodes.qual_mass_in.size())
                nodes.qual_mass_in[nd_idx] += mass_rate;

            // Legacy lumps direct and interface-file loads into EXTERNAL_INFLOW.
            auto pi = static_cast<std::size_t>(p);
            if (pi < ctx.mass_balance.qual_routing_ex_in.size())
                ctx.mass_balance.qual_routing_ex_in[pi] += mass_rate * dt;
        }
    }

    // Runtime-API forced quality mass (swmm_node_set_quality_mass_flux), a
    // mass RATE like the loads above. Legacy addExternalInflows() delivers it
    // in exactly this stage and books it as EXTERNAL_INFLOW, positive only
    // (routing.c: w = Node[j].apiExtQualMassFlux[p]; if (w > 0.0) {
    // Node[j].newQual[p] += w; massbal_addInflowQual(EXTERNAL_INFLOW, p, w); }).
    //
    // It carries no water of its own, so nothing is added to qual_vol_in — the
    // mixing denominator stays the node's actual inflow, as in legacy.
    // routing_forcing_qual_inflow remains a diagnostic SUBSET of the external
    // total (never added to it twice), mirroring routing_forcing_inflow on the
    // flow side.
    if (!nodes.user_conc_mass_flux.empty()) {
        for (int i = 0; i < ctx.n_nodes(); ++i) {
            auto ui = static_cast<std::size_t>(i);
            for (int p = 0; p < np; ++p) {
                auto nd_idx = ui * static_cast<std::size_t>(np) +
                              static_cast<std::size_t>(p);
                if (nd_idx >= nodes.user_conc_mass_flux.size()) continue;
                const double w = nodes.user_conc_mass_flux[nd_idx];
                if (w == 0.0) continue;
                // D-NS1 (X6): negative forced mass is extraction —
                // DELIBERATE deviation from legacy's positive-only rule
                // (routing.c `if (w > 0.0)`), per the user decision of
                // 2026-08-23. The signed rate books signed; the mix stage
                // clamps to available and un-books any shortfall. First
                // negative warns once (the API has no parse stage).
                if (w < 0.0 && !ctx.negsrc.api_warned) {
                    ctx.negsrc.api_warned = true;
                    ctx.warnings.push_back(
                        "D-NS1: a negative quality mass flux was applied "
                        "via the runtime API (extraction). It is clamped "
                        "per step to the mass the element holds.");
                }
                if (nd_idx < nodes.qual_mass_in.size())
                    nodes.qual_mass_in[nd_idx] += w;

                auto pi = static_cast<std::size_t>(p);
                if (pi < ctx.mass_balance.qual_routing_ex_in.size())
                    ctx.mass_balance.qual_routing_ex_in[pi] += w * dt;
                if (pi < ctx.mass_balance.routing_forcing_qual_inflow.size())
                    ctx.mass_balance.routing_forcing_qual_inflow[pi] += w * dt;
            }
        }
    }
}

void QualitySolver::execute(SimulationContext& ctx, double dt) {
    // R4: an MSX-only model (a reactions component and no [POLLUTANTS]) is a
    // legitimate shape — EPANET-MSX decks routinely declare no legacy
    // pollutant. Every stage below is a no-op at np == 0, so letting it
    // through costs nothing and is the only way reactLegacyNodes/Links run
    // for such a model. A1b: the pure-age LEGACY model is the same shape —
    // the age mirror needs the volume accumulation these stages perform.
    // Without a reactions component or WATER_AGE the early return is
    // unchanged, so parity is preserved by construction.
    // H1: and the temperature-only model is that same shape once more. This
    // is the SECOND guard of this family on the path — the routing-step
    // guard in SWMMEngine::stepRouting is the first — and a feature has to
    // clear both to run at np == 0.
    if (n_pollutants_ <= 0 && !transport::legacyReactionsActive(ctx) &&
        !ctx.options.water_age && !ctx.options.heat_transport)
        return;

    assembleExternalLoads(ctx, dt);
    accumulateLinkLoads(ctx, dt);
    mixAtNodes(ctx, dt);
    applyTreatment(ctx, dt);       // Treatment before decay (matching legacy order)
    // R4: with a reactions component configured, pollutant decay upgrades to
    // the exact exponential and MSX species react per element via the shared
    // integrator (ReactionLegacyBinding). Nodes react here (where applyDecay
    // ran); links react AFTER updateLinkQuality so the mixing pass does not
    // overwrite them (its internal linear decay is zeroed below). Without a
    // reactions component the legacy path runs untouched — bit-parity
    // (G-UT1).
    if (transport::legacyReactionsActive(ctx)) {
        transport::reactLegacyNodes(ctx, dt);
    } else {
        applyDecay(ctx, dt);
    }
    updateLinkQuality(ctx, dt);
    if (transport::legacyReactionsActive(ctx))
        transport::reactLegacyLinks(ctx, dt);
    applyLinkQualityForcing(ctx, n_pollutants_, dt);

    // A1b: the LEGACY age mirror runs LAST — it reads the fully accumulated
    // qual_vol_in as its mixing denominator and writes only water_age_state,
    // so WATER_AGE ON leaves every pollutant trajectory bit-identical.
    transport::routeLegacyAge(ctx, dt);

    // H1: the LEGACY temperature mirror runs beside the age mirror, on the
    // same fully accumulated qual_vol_in denominator, and writes only
    // heat_state — so HEAT_TRANSPORT ON leaves both the pollutant and the
    // water-age trajectories bit-identical under LEGACY.
    transport::routeLegacyHeat(ctx, dt);
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
        // A3: runoff arrives at the age the SUBCATCHMENT computed, not at the
        // configured RAINFALL age. The rainfall age is what enters the
        // subareas; by the time water leaves it has aged on the surface and
        // mixed with whatever was already ponded, and that is the number the
        // node must receive. Falls back to the configured source age when the
        // watershed state is unsized (WATER_AGE on, runoff never stepped).
        {
            const bool have_sc = ctx.options.water_age &&
                                 ui < ctx.water_age_state
                                          .subcatch_runoff_age.size();
            if (have_sc) {
                auto& acc = ctx.water_age_state.node_age_vol_in;
                if (ud < acc.size())
                    acc[ud] += q * ctx.water_age_state
                                       .subcatch_runoff_age[ui];
            } else {
                addAgeVolume(ctx, out_node, q, WaterAgeSource::RAINFALL);
            }
        }
        // H5a: the temperature mirror of the block above. Runoff reaches the
        // node at the SUBCATCHMENT's computed temperature, not the configured
        // RAINFALL temperature — rain is what enters the subareas; by the
        // time water leaves it has exchanged with the atmosphere and mixed
        // with whatever was ponded. Falls back to the configured source when
        // the watershed state is unsized (HEAT_TRANSPORT on, runoff never
        // stepped).
        {
            const bool have_sc = ctx.options.heat_transport &&
                                 ui < ctx.heat_state
                                          .subcatch_runoff_temp.size();
            if (have_sc) {
                auto& acc = ctx.heat_state.node_temp_vol_in;
                if (ud < acc.size())
                    acc[ud] += q * ctx.heat_state.subcatch_runoff_temp[ui];
            } else {
                addTempVolume(ctx, out_node, q, HeatSource::RAINFALL);
            }
        }

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

            // Mass balance: wet weather quality inflow is attributed HERE, from
            // the subcatchment's own washoff load — matching legacy
            // addWetWeatherInflows(): massbal_addInflowQual(WET_WEATHER_INFLOW,
            // p, q * Subcatch[i].newQual[p]).
            auto pi = static_cast<std::size_t>(p);
            if (mass_rate > 0.0 && pi < ctx.mass_balance.qual_routing_wet.size())
                ctx.mass_balance.qual_routing_wet[pi] += mass_rate * dt;
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
        // A4 RETIRES the RAINFALL stand-in here: the drain now arrives at the
        // age of the LID storage layer it was drawn from, accumulated as a
        // q·age rate in A6b beside this very volume. Falls back to the
        // configured source age only when the layer block is unsized (WATER_AGE
        // on but the LID manager built no units).
        if (ctx.options.water_age &&
            uj < ctx.water_age_state.node_lid_drain_age_vol_in.size() &&
            ctx.lid_layer_state.active()) {
            auto& acc = ctx.water_age_state.node_age_vol_in;
            if (uj < acc.size())
                acc[uj] += ctx.water_age_state.node_lid_drain_age_vol_in[uj];
        } else {
            addAgeVolume(ctx, j, drain_vol_rate, WaterAgeSource::RAINFALL);
        }
        addTempVolume(ctx, j, drain_vol_rate, HeatSource::RAINFALL);

        for (int p = 0; p < np; ++p) {
            auto nd_idx = uj * static_cast<std::size_t>(np) + static_cast<std::size_t>(p);
            double load = (nd_idx < ctx.nodes.lid_drain_qual_load.size())
                          ? ctx.nodes.lid_drain_qual_load[nd_idx] : 0.0;
            if (load > 0.0 && nd_idx < ctx.nodes.qual_mass_in.size())
                ctx.nodes.qual_mass_in[nd_idx] += load;

            // Legacy lid_addDrainInflow() / lid_addDrainRunon() also book drain
            // loads as WET_WEATHER_INFLOW.
            auto pi = static_cast<std::size_t>(p);
            if (load > 0.0 && pi < ctx.mass_balance.qual_routing_wet.size())
                ctx.mass_balance.qual_routing_wet[pi] += load * dt;
        }
    }
}

// ============================================================================
// Add RDII pollutant loads to node quality inflows
// Matches legacy addRdiiInflows() quality portion (routing.c:741-749)
// ============================================================================

void QualitySolver::addRdiiLoads(SimulationContext& ctx, double dt) {
    int np = n_pollutants_;
    if (!loadersNeeded(np, ctx)) return;
    auto& nodes = ctx.nodes;

    for (int i = 0; i < ctx.n_nodes(); ++i) {
        auto ui = static_cast<std::size_t>(i);
        double q = nodes.rdii_inflow[ui];
        if (q <= 0.0) continue;

        // Add volume inflow from RDII
        nodes.qual_vol_in[ui] += q * dt;
        addAgeVolume(ctx, i, q, WaterAgeSource::RDII);
        addTempVolume(ctx, i, q, HeatSource::RDII);

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
// Add default dry weather pollutant loads to node quality inflows
// Matches legacy addDwfInflows() default-concentration portion
// (routing.c:560-571: w = q * Pollut[p].dwfConcen)
// ============================================================================

void QualitySolver::addDwfLoads(SimulationContext& ctx, double dt) {
    int np = n_pollutants_;
    if (!loadersNeeded(np, ctx)) return;
    auto& nodes = ctx.nodes;

    for (int i = 0; i < ctx.n_nodes(); ++i) {
        auto ui = static_cast<std::size_t>(i);
        double q = nodes.dwf_inflow[ui];
        if (q <= 0.0) continue;

        // Add volume inflow from DWF (legacy qualrout.c uses Node[j].inflow,
        // which includes DWF, as the mixing denominator). Without this the
        // mass added below is discarded by mixAtNodes when v_in == 0.
        nodes.qual_vol_in[ui] += q * dt;
        addAgeVolume(ctx, i, q, WaterAgeSource::DWF);
        addTempVolume(ctx, i, q, HeatSource::DWF);

        OPENSWMM_IVDEP
        for (int p = 0; p < np; ++p) {
            double c_dwf = ctx.pollutants.c_dwf[static_cast<std::size_t>(p)];
            if (c_dwf <= 0.0) continue;
            auto nd_idx = ui * static_cast<std::size_t>(np) + static_cast<std::size_t>(p);
            if (nd_idx < nodes.qual_mass_in.size()) {
                nodes.qual_mass_in[nd_idx] += q * c_dwf;
            }
        }

        // Mass balance: track dry weather quality inflow
        for (int p = 0; p < np; ++p) {
            double c_dwf = ctx.pollutants.c_dwf[static_cast<std::size_t>(p)];
            if (c_dwf <= 0.0) continue;
            auto pi = static_cast<std::size_t>(p);
            if (pi < ctx.mass_balance.qual_routing_dw_in.size()) {
                ctx.mass_balance.qual_routing_dw_in[pi] += q * c_dwf * dt;
            }
        }
    }
}

// ============================================================================
// Add groundwater inflow pollutant loads to node quality inflows
// Matches legacy addGroundwaterInflows() pollutant portion
// (routing.c:678-686: w = q * Pollut[p].gwConcen)
// ============================================================================

void QualitySolver::addGwLoads(SimulationContext& ctx, double dt) {
    int np = n_pollutants_;
    if (!loadersNeeded(np, ctx)) return;
    auto& nodes = ctx.nodes;

    for (int i = 0; i < ctx.n_nodes(); ++i) {
        auto ui = static_cast<std::size_t>(i);
        double q = nodes.gw_inflow[ui];
        if (q <= 0.0) continue;  // pollutant load only for positive inflow

        // Add volume inflow from groundwater (see addDwfLoads: the mass below
        // is discarded by mixAtNodes unless its carrier volume is counted).
        nodes.qual_vol_in[ui] += q * dt;
        addAgeVolume(ctx, i, q, WaterAgeSource::GW);
        addTempVolume(ctx, i, q, HeatSource::GW);

        OPENSWMM_IVDEP
        for (int p = 0; p < np; ++p) {
            double c_gw = ctx.pollutants.c_gw[static_cast<std::size_t>(p)];
            if (c_gw <= 0.0) continue;
            auto nd_idx = ui * static_cast<std::size_t>(np) + static_cast<std::size_t>(p);
            if (nd_idx < nodes.qual_mass_in.size()) {
                nodes.qual_mass_in[nd_idx] += q * c_gw;
            }
        }

        // Mass balance: track groundwater quality inflow
        for (int p = 0; p < np; ++p) {
            double c_gw = ctx.pollutants.c_gw[static_cast<std::size_t>(p)];
            if (c_gw <= 0.0) continue;
            auto pi = static_cast<std::size_t>(p);
            if (pi < ctx.mass_balance.qual_routing_gw_in.size()) {
                ctx.mass_balance.qual_routing_gw_in[pi] += q * c_gw * dt;
            }
        }
    }
}

// ============================================================================
// Add routing interface file pollutant loads to node quality inflows
// Matches legacy addIfaceInflows() quality portion (routing.c:756-795):
// mass rates (q * c_iface) are pre-computed per node by
// iface::InterfaceManager::readInflows() into nodes.iface_qual_mass.
// ============================================================================

void QualitySolver::addIfaceLoads(SimulationContext& ctx, double dt) {
    int np = n_pollutants_;
    if (!loadersNeeded(np, ctx)) return;
    auto& nodes = ctx.nodes;
    if (nodes.iface_qual_mass.empty()) return;

    for (int i = 0; i < ctx.n_nodes(); ++i) {
        auto ui = static_cast<std::size_t>(i);
        double q = nodes.iface_inflow[ui];
        if (q <= 0.0) continue;

        // Add volume inflow from the interface file
        nodes.qual_vol_in[ui] += q * dt;
        addAgeVolume(ctx, i, q, WaterAgeSource::IFACE);
        addTempVolume(ctx, i, q, HeatSource::IFACE);

        OPENSWMM_IVDEP
        for (int p = 0; p < np; ++p) {
            auto nd_idx = ui * static_cast<std::size_t>(np) + static_cast<std::size_t>(p);
            double mass_rate = (nd_idx < nodes.iface_qual_mass.size())
                               ? nodes.iface_qual_mass[nd_idx] : 0.0;
            if (mass_rate <= 0.0) continue;
            if (nd_idx < nodes.qual_mass_in.size()) {
                nodes.qual_mass_in[nd_idx] += mass_rate;
            }
        }

        // Mass balance: interface loads count as external quality inflow
        // (legacy massbal_addInflowQual(EXTERNAL_INFLOW, p, w))
        for (int p = 0; p < np; ++p) {
            auto nd_idx = ui * static_cast<std::size_t>(np) + static_cast<std::size_t>(p);
            double mass_rate = (nd_idx < nodes.iface_qual_mass.size())
                               ? nodes.iface_qual_mass[nd_idx] : 0.0;
            if (mass_rate <= 0.0) continue;
            auto pi = static_cast<std::size_t>(p);
            if (pi < ctx.mass_balance.qual_routing_ex_in.size()) {
                ctx.mass_balance.qual_routing_ex_in[pi] += mass_rate * dt;
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
    // Legacy parity: src/legacy/engine/qualrout.c runs this loop serially.
#if defined(SWMM_USE_OPENMP)
// #pragma omp parallel for schedule(static)
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
            // D-NS1 (X6): a negative load is extraction, clamped to the
            // mass the store holds; the shortfall is counted and un-booked
            // so the ledger carries what actually left. The branch is
            // untaken on every non-negative deck — bit-inert by
            // construction.
            if (mass_in < 0.0) {
                const double avail = c_old * v_old;
                if (mass_in < -avail) {
                    bookNegativeSourceClamp(ctx, i, p, -(avail + mass_in));
                    mass_in = -avail;
                }
            }
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
    // Legacy parity: src/legacy/engine/qualrout.c runs this loop serially.
#if defined(SWMM_USE_OPENMP)
// #pragma omp parallel for schedule(static)
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
            int cr = ctx.link_subtypes.conduit_row(j);
            int nb = (cr >= 0) ? ctx.link_subtypes.conduits.barrels[static_cast<size_t>(cr)] : 1;
            double evap_rate = (cr >= 0) ? ctx.link_subtypes.conduits.evap_loss_rate[static_cast<size_t>(cr)] : 0.0;
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
            // R4: reactions-active runs decay links exactly in
            // reactLegacyLinks AFTER this mixing pass; the in-mix linear
            // decay must not double-apply.
            if (transport::legacyReactionsActive(ctx)) k = 0.0;

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
                                      ucf::getUnitSystem(unit_sys), &ctx.node_subtypes);
        double a2 = node::getSurfArea(nodes, j, nodes.depth[uj],     &ctx.tables,
                                      ucf::getUnitSystem(unit_sys), &ctx.node_subtypes);
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
