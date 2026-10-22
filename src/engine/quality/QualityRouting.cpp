#include "../hydrology/LidNode.hpp"
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
 * @file      QualityRouting.cpp
 * @brief     Water quality routing — batch SoA, numerically identical to legacy.
 *
 * @details All quality operations use flat [node*n_pollutants+p] indexing
 *          for cache-friendly batch processing. Inner loops are vectorisable.
 *
 * @ingroup new_engine
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "QualityRouting.hpp"
#include "MsxLidReactions.hpp"

#include "../transport/components/EulerianArdComponent/ArdConfig.hpp"
#include "../transport/components/ReactionModule/MsxLegacyTransport.hpp"
#include "../transport/components/ReactionModule/ReactionLegacyBinding.hpp"
#include "../transport/components/HeatFluxModules/BedExchange.hpp"
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

// ZERO_VOLUME, ZERO_DEPTH and LEGACY_ZERO are defined in QualityRouting.hpp

// ============================================================================
// The shared shape of the legacy mixing kernels — see QualityRouting.hpp.
// ============================================================================

/**
 * @brief Test whether a link has no mixed conduit water pool.
 * @param[in] ctx Stable covered link/type/subtype hydraulic arrays.
 * @param[in] link Valid nonnegative native link index, unchecked.
 *
 * @return True for non-conduit type or DUMMY section; no flow/depth check.
 * @note Read-only and allocation-free; no generic finite/physical validation.
 */
bool linkTakesUpstreamValue(const SimulationContext& ctx, int link) {
    const auto uj = static_cast<std::size_t>(link);
    return ctx.links.type[uj] != LinkType::CONDUIT ||
           ctx.links.xsect_shape[uj] == XsectShape::DUMMY;
}

/**
 * @brief Calculate a conduit quality mixing inflow from accepted hydraulic balance.
 * @param[in] ctx Stable covered link/type/subtype hydraulic arrays.
 * @param[in] link Valid nonnegative native link index, unchecked.
 * @param[in] dt Accepted interval s; nonpositive omits volume correction. Positive finite
 * duration required for ordinary routing.
 * @return Nonnegative ft3/s: absolute KW upstream flow or link flow; DYNWAVE/FV add (new volume
 * + barrel seep/evap losses - old volume)/dt for dt>0.
 * @note Read-only and allocation-free; no generic finite/physical validation.
 */
double conduitMixingInflow(const SimulationContext& ctx, int link, double dt) {
    const auto uj = static_cast<std::size_t>(link);
    // Legacy reads |Conduit.q1| * barrels. Under DW and SF the solver writes
    // one flow to both conduit ends, so |Link.newFlow| is that same number.
    // KW publishes its accepted upstream flow separately from Link.newFlow.
    const auto model = ctx.options.routing_model;
    double q_in =
        std::fabs(model == RoutingModel::KINWAVE ? ctx.links.kw_inflow[uj] : ctx.links.flow[uj]);

    // Legacy gates this correction on `RouteModel == DW` alone: dynamic wave
    // produces a SINGLE flow rate per conduit, so the storage the conduit
    // gained over the step is inflow that rate does not carry. Kinematic wave
    // carries separate upstream and downstream flows and needs none — adding
    // it there double-counts the fill. FV is this engine's other single-rate
    // dynamic model, so it takes the DW branch.
    if (model != RoutingModel::DYNWAVE && model != RoutingModel::FV)
        return std::max(q_in, 0.0);
    if (dt <= 0.0)
        return std::max(q_in, 0.0);

    const int cr = ctx.link_subtypes.conduit_row(link);
    double v_losses = 0.0;
    if (cr >= 0) {
        const auto ucr = static_cast<std::size_t>(cr);
        const auto& CD = ctx.link_subtypes.conduits;
        const double barrels = static_cast<double>(CD.barrels[ucr]);
        v_losses = CD.seep_loss_rate[ucr] * barrels * dt + CD.evap_loss_rate[ucr] * barrels * dt;
    }
    q_in += (ctx.links.volume[uj] + v_losses - ctx.links.old_volume[uj]) / dt;
    return std::max(q_in, 0.0);
}

/**
 * @brief Test the accepted conduit water/depth effective-zero thresholds.
 * @param[in] ctx Stable covered link/type/subtype hydraulic arrays.
 * @param[in] link Valid nonnegative native link index, unchecked.
 *
 * @return True for volume<.0353147ft3 OR depth<=.003281ft; no mutation.
 * @note Read-only and allocation-free; no generic finite/physical validation.
 */
bool linkIsDry(const SimulationContext& ctx, int link) {
    const auto uj = static_cast<std::size_t>(link);
    return ctx.links.volume[uj] < ZERO_VOLUME || ctx.links.depth[uj] <= ZERO_DEPTH;
}

/**
 * @brief Select storage or retained-water node mixing.
 * @param[in] ctx Stable node/type/subtype old/current hydraulic arrays; paired storage loss and
 * LID states covered.
 * @param[in] node Valid nonnegative native node ID; storageEvapFactor returns1 when type extent
 * absent.
 *
 * @return True for STORAGE type regardless of dryness, or old volume>.0353147ft3.
 * @note Read-only/no allocation; no geometry refresh, physical change or clock update.
 */
bool nodeIsReactor(const SimulationContext& ctx, int node) {
    const auto ui = static_cast<std::size_t>(node);
    return ctx.nodes.type[ui] == NodeType::STORAGE || ctx.nodes.old_volume[ui] > ZERO_VOLUME;
}

// Review B1 (2026-10-15): one owner for the storage evaporation factor, so the
// pollutant reactor and the species mirror cannot drift apart again (the
// mirror had none). Legacy findStorageQual: fEvap = 1 + vEvap/v1 from the
// unit's ACTUAL evaporation volume, skipped below a litre of old volume.
/**
 * @brief Calculate storage concentration increase from non-LID evaporation.
 * @param[in] ctx Stable node/type/subtype old/current hydraulic arrays; paired storage loss and
 * LID states covered.
 * @param[in] node Valid nonnegative native node ID; storageEvapFactor returns1 when type extent
 * absent.
 *
 * @return 1 plus max(0,total evaporation minus native LID evaporation)/old volume when old
 * volume>one litre; otherwise1.
 * @note Read-only/no allocation; no geometry refresh, physical change or clock update.
 */
double storageEvapFactor(const SimulationContext& ctx, int node) {
    double f_evap = 1.0;
    const auto ui = static_cast<std::size_t>(node);
    if (ui >= ctx.nodes.type.size() || ctx.nodes.type[ui] != NodeType::STORAGE)
        return f_evap;
    const int srow = ctx.node_subtypes.storage_row(node);
    if (srow < 0)
        return f_evap;
    const auto us = static_cast<std::size_t>(srow);
    const auto& st = ctx.node_subtypes.storages;
    const double v_old = ctx.nodes.old_volume[ui];
    const double v_evap = (us < st.evap_loss.size())
                              ? std::max(0.0, st.evap_loss[us] - st.lid_state[us].evap_volume)
                              : 0.0;
    if (v_evap > 0.0 && v_old > ZERO_VOLUME)
        f_evap += v_evap / v_old;
    return f_evap;
}

// Review A6 (2026-10-15): residence time at storage units that carry no
// treatment expression. Tank kinetics read nodes.hrt; it was only ever
// updated inside the treatment path, so an untreated storage unit held its
// initial value for the whole run under every quality solver. Treated
// nodes keep their existing update (applyNodeTreatment, legacy-parity
// gated) and are skipped here so no node is advanced twice. The flow is the
// node's hydraulic inflow, legacy updateHRT's `Node.inflow`. Runs only with
// a reactions component: HRT is read by nothing else, and ordinary decks
// keep byte-identical hot-start files.
/**
 * @brief Advance residence time only for untreated storage nodes under active native reactions.
 * @param[in,out] ctx Prepared storage node HRT/old volume and hydraulic inflow; updates covered
 * HRT seconds.
 * @param[in] dt Positive finite accepted interval s; nonpositive or inactive legacy reactions
 * returns.
 * @note Skips treated nodes (their treatment path advances HRT); essentially empty old volume
 * resets0, otherwise max((old HRT+dt)*old volume/(old volume+abs(inflow)*dt),0). Missing inflow
 * uses0. No quality/water ledger or clock mutation, allocation or finite repair; exclusive
 * once-per-interval state.
 */
void updateUntreatedStorageHrt(SimulationContext& ctx, double dt) {
    if (dt <= 0.0 || !transport::legacyReactionsActive(ctx))
        return;
    auto& nodes = ctx.nodes;
    const auto& treat = ctx.treatment;
    const bool any_treatment = ctx.n_pollutants() > 0 && treat.hasAny();
    for (int j = 0; j < ctx.n_nodes(); ++j) {
        const auto uj = static_cast<std::size_t>(j);
        if (nodes.type[uj] != NodeType::STORAGE || uj >= nodes.hrt.size())
            continue;
        if (any_treatment && uj < treat.has_treatment.size() && treat.has_treatment[uj])
            continue;
        const double v_old = nodes.old_volume[uj];
        if (v_old < LEGACY_ZERO) {
            nodes.hrt[uj] = 0.0;
        } else {
            const double q = (uj < nodes.inflow.size()) ? std::abs(nodes.inflow[uj]) : 0.0;
            nodes.hrt[uj] = std::max((nodes.hrt[uj] + dt) * v_old / (v_old + q * dt), 0.0);
        }
    }
}

/**
 * @brief Test node effective dryness only when mixing inflow is negligible.
 * @param[in] ctx Stable node/type/subtype old/current hydraulic arrays; paired storage loss and
 * LID states covered.
 * @param[in] node Valid nonnegative native node ID; storageEvapFactor returns1 when type extent
 * absent.
 * @param[in] q_in Mixing inflow rate ft3/s, not interval volume; negative meets the rate test.
 * @return True when volume<=.0353147ft3 OR depth<=.003281ft, AND q_in<=1e-10ft3/s.
 * @note Read-only/no allocation; no geometry refresh, physical change or clock update.
 */
bool nodeIsDry(const SimulationContext& ctx, int node, double q_in) {
    const auto ui = static_cast<std::size_t>(node);
    return (ctx.nodes.volume[ui] <= ZERO_VOLUME || ctx.nodes.depth[ui] <= ZERO_DEPTH) &&
           q_in <= LEGACY_ZERO;
}

namespace {

/**
 * @brief The external-load loaders below do two jobs: they accumulate per-pollutant MASS into
 * qual_mass_in, and they accumulate the node's total external inflow VOLUME into qual_vol_in.
 * Only the first is pollutant-shaped. E5a's [TRANSPORT_BOUNDARIES] injects `qual_vol_in *
 * concentration`, so an MSX-only model (no [POLLUTANTS] — the nh2cl shape) needs the volume half
 * to run even at np == 0, where every mass loop is already a no-op. Measured before this: a
 * boundary on an MSX-only deck delivered exactly 0.0 while the same deck with one inert
 * pollutant row delivered 8.0.
 *
 * @details Test whether shared external carrier/load assembly is needed by any quality family.
 * @param[in] np Pollutant count; positive enables loaders.
 * @param[in] ctx Stable ARD boundary policy and water-age/heat/native reaction enable state.
 * @return True if any consumer needs loads or carrier volumes. Does not initialize or change
 * context.
 */
bool loadersNeeded(int np, const SimulationContext& ctx) {
    // A1a: water age needs the volume half (and the per-loader age-volume
    // contributions) even on a deck with no [POLLUTANTS] — the pure-age
    // model is A1a's motivating configuration (lesson 20).
    // H1: heat needs the volume half for the same reason age does — a
    // temperature-only deck (no [POLLUTANTS]) is a supported configuration
    // and every mass loop is already a no-op at np == 0.
    // R4b: and the MSX mirror is the FOURTH member of this family — an
    // MSX-only deck (a reactions component, no [POLLUTANTS]) needs
    // qual_vol_in zeroed and accumulated per step. Without this,
    // assembleExternalLoads early-returns, the accumulator is never reset,
    // v_in grows monotonically and routeLegacyMsx's mixing denominator
    // diverges: the first run of the R4b gate read C3 at 2.5e-20 from
    // exactly that. (The comment at the top of this function ALREADY
    // recorded the 0.0-vs-8.0 version of this defect for ARD boundaries —
    // same shape, fourth recurrence.)
    return np > 0 || transport::ardBoundariesNeedExternalVolumes(ctx) || ctx.options.water_age ||
           ctx.options.heat_transport || transport::legacyReactionsActive(ctx);
}

/**
 * @brief A1a: one loader's age-volume contribution — `q · age_source` (a RATE, age·ft³/s, the
 * age analogue of qual_mass_in). No-op when WATER_AGE is off or the state is unsized (the ARD
 * engine sizes it at init; the assemble stage zeroes it each step).
 *
 * @details Add configured source scalar times water flow to a node's mixing-rate numerator.
 * @param[in,out] ctx Enabled scalar configuration and covered writable node accumulator;
 * disabled or uncovered node returns. External-inflow age may use non-NaN per-node override.
 * @param[in] node Valid nonnegative native node index.
 * @param[in] q Carrier flow ft3/s, caller validates finite/sign.
 * @param[in] src WaterAgeSource category used for configured global/per-node source age s.
 * @note Adds q*scalar, not q*dt*scalar; no hydraulic/clock/global-ledger mutation or allocation.
 * Caller owns exclusive external assembly and correctly cleared accumulator.
 */
inline void addAgeVolume(SimulationContext& ctx, int node, double q, WaterAgeSource src) {
    if (!ctx.options.water_age)
        return;
    auto& s = ctx.water_age_state.node_age_vol_in;
    const auto un = static_cast<std::size_t>(node);
    if (un >= s.size())
        return;
    double age = ctx.water_age_config.source_age(src, node);
    // Z1 (amendment D-Y4): an [INFLOWS] row naming __WATER_AGE__ is the
    // more specific statement of THIS node's inflow age and wins over the
    // source table's EXTERNAL_INFLOW entry (constant or node override).
    // NaN marks "no row at this node" — never a value.
    if (src == WaterAgeSource::EXTERNAL_INFLOW) {
        const auto& ov = ctx.water_age_state.node_ext_inflow_age;
        if (un < ov.size() && !std::isnan(ov[un]))
            age = ov[un];
    }
    s[un] += q * age;
}

/**
 * @brief H1: one loader's temperature-volume contribution — `q · T_source` (a RATE, °C·ft³/s),
 * the heat analogue of the age channel above and the same seam (master plan §4.3 / D-UT10).
 * Carries temperature-volume rather than Joules because ρw·cp cancel identically until H2 brings
 * the energy fluxes that make them load-bearing — see HeatData.hpp.
 *
 * @details Add configured source scalar times water flow to a node's mixing-rate numerator.
 * @param[in,out] ctx Enabled scalar configuration and covered writable node accumulator;
 * disabled or uncovered node returns.
 * @param[in] node Valid nonnegative native node index.
 * @param[in] q Carrier flow ft3/s, caller validates finite/sign.
 * @param[in] src HeatSource category used for configured global/per-node source temperature
 * degC.
 * @note Adds q*scalar, not q*dt*scalar; no hydraulic/clock/global-ledger mutation or allocation.
 * Caller owns exclusive external assembly and correctly cleared accumulator.
 */
inline void addTempVolume(SimulationContext& ctx, int node, double q, HeatSource src) {
    if (!ctx.options.heat_transport)
        return;
    auto& s = ctx.heat_state.node_temp_vol_in;
    const auto un = static_cast<std::size_t>(node);
    if (un >= s.size())
        return;
    s[un] += q * ctx.heat_config.source_temp(src, node);
}

/**
 * @brief Legacy `Node[j].treatment[p].equation != NULL` — findStorageQual skips its first-order
 * reaction for a pollutant the node treats, so the two removals never stack.
 *
 * @details Test whether a flattened node/pollutant treatment expression string is nonempty.
 * @param[in] ctx Stable treatment row count/expression table.
 * @param[in] node Valid nonnegative native node index.
 * @param[in] p Valid nonnegative pollutant row; products must fit index arithmetic.
 * @return False for nonpositive treatment pollutant count or uncovered/empty expression;
 * otherwise true. No compilation, evaluation, allocation, finite validation or state change.
 */
inline bool nodeHasTreatmentFor(const SimulationContext& ctx, int node, int p) {
    const auto& tr = ctx.treatment;
    if (tr.n_pollutants <= 0)
        return false;
    const auto idx = static_cast<std::size_t>(node) * static_cast<std::size_t>(tr.n_pollutants) +
                     static_cast<std::size_t>(p);
    return idx < tr.expressions.size() && !tr.expressions[idx].empty();
}

/**
 * @brief Apply runtime link concentration override/add modes after transport and kinetics.
 * @param[in,out] ctx Writable link concentrations, forcing modes/values and forcing inflow
 * ledger.
 * @param[in] n_pollutants Matching link/forcing row stride; nonpositive returns.
 * @param[in] dt Accepted interval s, used to book ADD value*dt without water-volume weighting.
 * @note OVERRIDE installs max(value,0) without ledger booking; ADD installs max(old+value,0) and
 * books signed raw forcing value*dt. Uncovered mode/conc tuples skip; parallel value/ledger
 * extents must cover. No general finite check, water/clock update or allocation. Exclusive final
 * quality phase.
 */
void applyLinkQualityForcing(SimulationContext& ctx, int n_pollutants, double dt) {
    if (n_pollutants <= 0)
        return;

    auto& f = ctx.forcing;
    if (f.link_quality_mode.empty())
        return;

    for (int j = 0; j < ctx.n_links(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        for (int p = 0; p < n_pollutants; ++p) {
            auto flat = uj * static_cast<std::size_t>(n_pollutants) + static_cast<std::size_t>(p);
            if (flat >= f.link_quality_mode.size() || flat >= ctx.links.conc.size()) {
                continue;
            }

            if (f.link_quality_mode[flat] == ForcingMode::OVERRIDE) {
                ctx.links.conc[flat] = std::max(f.link_quality_value[flat], 0.0);
            } else if (f.link_quality_mode[flat] == ForcingMode::ADD) {
                ctx.links.conc[flat] =
                    std::max(ctx.links.conc[flat] + f.link_quality_value[flat], 0.0);
                ctx.mass_balance.routing_forcing_qual_inflow[static_cast<std::size_t>(p)] +=
                    f.link_quality_value[flat] * dt;
            }
        }
    }
}

} // namespace

/**
 * @brief Set the solver's pollutant row count for subsequent routing/assembly.
 * @param[in] n_nodes Unused node count; storage resides on SimulationContext, not this
 * initializer.
 * @param[in] n_links Unused link count.
 * @param[in] n_pollutants Pollutant count/flattened stride; caller must keep context arrays
 * consistent.
 * @note Does not resize context or reset prior lateral scratch, inventories or clocks;
 * allocation-free.
 */
void QualitySolver::init(int n_nodes, int n_links, int n_pollutants) {
    n_pollutants_ = n_pollutants;
    (void)n_nodes;
    (void)n_links;
}

// Review D2 / gaining conduits (2026-10-15): water that already reached a
// node hydraulically (through a surface outfall from the mesh, or gained by a
// conduit from the aquifer) joins the node's mixing volume with the mass the
// donor gave up. The volume is outside qual_vol_lat_: it is not lateral flow
// and takes no part in legacy's floored-net denominator.
/**
 * @brief Consume pending surface-outfall and gaining-link quality-only receipts.
 * @param[in,out] ctx Prepared completed hydraulic/source state and writable
 * quality/scalar/native species arrays, receipts, ledgers and diagnostics used by this stage.
 * @param[in] dt Matching accepted positive finite routing duration s; most stages do not
 * validate it.
 * @details Adds accepted positive receipt volume directly to node qual_vol_in, divides stored
 * pollutant/native species mass and age/temperature volume numerators by dt to recover rates,
 * books pollutant ex/GW mass, then clears each receipt container. Does not change hydraulic
 * water (already accepted there). Inactive receipt skipped; nonpositive/NaN dt returns before
 * consumption. Native destination shape mismatch resets0. Requires covered receipt scalar/volume
 * arrays; allocation can leave partial receipts.
 * @note Requires matching initialized solver pollutant count, node/link/subtype arrays and
 * writable quality/ledger extents. Carrier volumes use ft3; pollutant rates use
 * concentration*ft3/s and ledger increments concentration*ft3. No implicit litre or user-mass
 * conversion, hydraulic water advance or engine clock change. Caller serializes quality phases;
 * repeat calls can double-book loads.
 */
void QualitySolver::addQualityOnlyInflows(SimulationContext& ctx, double dt) {
    if (!(dt > 0.0))
        return;
    const int np = ctx.n_pollutants();
    const int ns = ctx.reactions.n_species();
    auto& nodes = ctx.nodes;
    auto& mb = ctx.mass_balance;
    auto apply = [&](SimulationContext::QualityOnlyInflow& q, std::vector<double>& row) {
        if (!q.any)
            return;
        const auto nn =
            std::min<std::size_t>(q.vol.size(), static_cast<std::size_t>(ctx.n_nodes()));
        auto& rx = ctx.reactions;
        if (ns > 0 && rx.msx_ext_mass_in.size() !=
                          static_cast<std::size_t>(ctx.n_nodes()) * static_cast<std::size_t>(ns))
            rx.msx_ext_mass_in.assign(
                static_cast<std::size_t>(ctx.n_nodes()) * static_cast<std::size_t>(ns), 0.0);
        for (std::size_t i = 0; i < nn; ++i) {
            if (q.vol[i] <= 0.0)
                continue;
            nodes.qual_vol_in[i] += q.vol[i];
            for (int p = 0; p < np; ++p) {
                const auto k = i * static_cast<std::size_t>(np) + static_cast<std::size_t>(p);
                if (k >= q.mass.size() || k >= nodes.qual_mass_in.size())
                    continue;
                nodes.qual_mass_in[k] += q.mass[k] / dt;
                if (static_cast<std::size_t>(p) < row.size())
                    row[static_cast<std::size_t>(p)] += q.mass[k];
            }
            for (int m = 0; m < ns; ++m) {
                const auto k = i * static_cast<std::size_t>(ns) + static_cast<std::size_t>(m);
                if (k < q.msx.size() && k < rx.msx_ext_mass_in.size())
                    rx.msx_ext_mass_in[k] += q.msx[k] / dt;
            }
            if (ctx.options.water_age && i < ctx.water_age_state.node_age_vol_in.size())
                ctx.water_age_state.node_age_vol_in[i] += q.age_vol[i] / dt;
            if (ctx.options.heat_transport && i < ctx.heat_state.node_temp_vol_in.size())
                ctx.heat_state.node_temp_vol_in[i] += q.temp_vol[i] / dt;
        }
        q.clear();
    };
    apply(ctx.surface_outfall_quality, mb.qual_routing_ex_in);
    apply(ctx.link_gain_quality, mb.qual_routing_gw_in);
}

/**
 * @brief Reset and assemble all external quality/carrier/scalar sources for this interval.
 * @param[in,out] ctx Prepared completed hydraulic/source state and writable
 * quality/scalar/native species arrays, receipts, ledgers and diagnostics used by this stage.
 * @param[in] dt Matching accepted positive finite routing duration s; most stages do not
 * validate it.
 * @details Resets node pollutant rates/volumes, age/heat rate numerators and lateral scratch;
 * scalar shape mismatch calls resize (may reset scalar histories). Adds runoff/LID drains, RDII,
 * DWF, GW, interface, direct/runtime, 2D coupling and pending quality-only receipts. Negative
 * lateral components reduce mixing denominator to positive net hydraulic lateral contribution;
 * only negative correction applied. Does not itself clear native msx_ext_mass_in unless a
 * receipt shape mismatch triggers its reset. No consumers returns; callers must pre-evaluate
 * accepted hydraulics and source values. Allocation or warning failures can leave partial
 * assembly and consumed receipts; not transactional.
 * @note Requires matching initialized solver pollutant count, node/link/subtype arrays and
 * writable quality/ledger extents. Carrier volumes use ft3; pollutant rates use
 * concentration*ft3/s and ledger increments concentration*ft3. No implicit litre or user-mass
 * conversion, hydraulic water advance or engine clock change. Caller serializes quality phases;
 * repeat calls can double-book loads.
 */
void QualitySolver::assembleExternalLoads(SimulationContext& ctx, double dt) {
    if (!loadersNeeded(n_pollutants_, ctx))
        return;

    // Reset quality assembly arrays on NodeData
    std::fill(ctx.nodes.qual_mass_in.begin(), ctx.nodes.qual_mass_in.end(), 0.0);

    // A1a: size + zero the age-volume accumulator alongside the pollutant
    // loads (same lifecycle: assembled per routing step by the loaders).
    if (ctx.options.water_age) {
        auto& ws = ctx.water_age_state;
        if (ws.node_age_vol_in.size() != static_cast<std::size_t>(ctx.n_nodes()))
            // A3: pass the subcatchment count — the 2-arg form defaults it
            // to 0 and would WIPE subarea_age on any re-size.
            ws.resize(ctx.n_nodes(), ctx.n_links(), ctx.n_subcatches());
        std::fill(ws.node_age_vol_in.begin(), ws.node_age_vol_in.end(), 0.0);
    }
    // H1: same lifecycle for the temperature-volume accumulator.
    if (ctx.options.heat_transport) {
        auto& hs = ctx.heat_state;
        if (hs.node_temp_vol_in.size() != static_cast<std::size_t>(ctx.n_nodes()))
            hs.resize(ctx.n_nodes(), ctx.n_links(),
                      ctx.heat_config.global_temp[static_cast<int>(HeatSource::INITIAL_STATE)]);
        std::fill(hs.node_temp_vol_in.begin(), hs.node_temp_vol_in.end(), 0.0);
    }
    std::fill(ctx.nodes.qual_vol_in.begin(), ctx.nodes.qual_vol_in.end(), 0.0);
    qual_vol_lat_.assign(static_cast<std::size_t>(ctx.n_nodes()), 0.0);
    qual_vol_lat_neg_.assign(static_cast<std::size_t>(ctx.n_nodes()), 0.0);

    addWetWeatherLoads(ctx, dt);    // Subcatchment washoff → nodes
    addRdiiLoads(ctx, dt);          // RDII pollutant loads → nodes
    addDwfLoads(ctx, dt);           // Dry weather pollutant loads → nodes
    addGwLoads(ctx, dt);            // Groundwater inflow pollutant loads → nodes
    addIfaceLoads(ctx, dt);         // Routing interface file loads → nodes
    addExtInflowLoads(ctx, dt);     // Direct [INFLOWS] CONCEN/MASS loads → nodes
    addCouplingLoads(ctx, dt);      // S3: 2D→1D junction drain (volume + mass)
    addQualityOnlyInflows(ctx, dt); // review D2 + gaining conduits

    // legacy seeds the node's quality-inflow DENOMINATOR with the NET lateral
    // flow floored at zero — routing.c:480, Node.qualInflow =
    // MAX(0, Node.newLatFlow) — and dynwave's Node.inflow carries the same
    // floored net into qualrout_execute. A lateral component that WITHDRAWS
    // water (two-way groundwater exchange running aquifer-ward) therefore
    // shrinks the carrier volume under the remaining lateral MASS, and the
    // node's mixed concentration legitimately rises ABOVE every source
    // concentration. v6 summed each positive source's own volume, so the
    // negative component never touched the denominator: on usgs-runoff
    // (Cppt 18 mg/L, GW drawing up to 2,000 cfs out of the receptors) legacy
    // climbs to 47 mg/L while v6 could never exceed 18. Replace the lateral
    // share of the accumulated volume with legacy's floored net. The
    // correction is never positive (net <= sum of positive parts), and the
    // S3 coupling volume is deliberately outside it (legacy has no 2D seam;
    // legacy-comparable runs carry coupling_inflow == 0).
    // Review B2 (2026-10-15): the denominator is a property of the node's
    // WATER, shared by every species family that mixes over qual_vol_in
    // (reaction species, age, temperature). Gated on the pollutant count it
    // made an MSX-only deck mix the same inflow over a different volume.
    {
        auto& nodes = ctx.nodes;
        const std::size_t nn = std::min(qual_vol_lat_.size(), nodes.qual_vol_in.size());
        for (std::size_t ui = 0; ui < nn; ++ui) {
            // Fire only when a negative lateral component actually arrived —
            // on every other deck the correction is EXACTLY zero, so their
            // ULPs are untouched. When it fires, take legacy's denominator
            // from the flow side's own net lateral (the same newLatFlow
            // legacy floors), not from re-summing the quality-side parts.
            if (!(ui < qual_vol_lat_neg_.size() && qual_vol_lat_neg_[ui] < 0.0))
                continue;
            const double lat_net_pos =
                (ui < nodes.lat_flow.size()) ? std::max(0.0, nodes.lat_flow[ui]) * dt : 0.0;
            const double corr = lat_net_pos - qual_vol_lat_[ui];
            if (corr < 0.0)
                nodes.qual_vol_in[ui] += corr;
        }
    }
}

// ============================================================================
// S3 (D-2DT4, 2D→1D half): the 2D surface's junction drain. Engine-only (no
// legacy counterpart — legacy has no 2D surface), added LAST so
// legacy-comparable runs (coupling_inflow == 0) keep their ULPs.
//
// Two things arrive together and must both be booked, or the mix is wrong in
// opposite directions: the WATER joins qual_vol_in (the mixing denominator —
// before S3 the drain's water reached the node hydraulically but the quality
// mix never saw it, so drained water diluted nothing) and the MASS joins
// qual_mass_in at the rate assembleLateralInflows drained from the queue.
// The spill (coupling_inflow < 0) books nothing here: the CSTR already takes
// every outflow at the mixed concentration through the reduced inflow volume.
// Age and temperature ride the EXTERNAL_INFLOW stand-ins until S4 carries
// them on the surface. Booked as external inflow in the quality continuity
// table, where the volume already goes on the flow side.
// ============================================================================

/**
 * @brief Add accepted 2D-to-node coupling carrier volumes, scalar numerators and pollutant
 * rates.
 * @param[in,out] ctx Prepared completed hydraulic/source state and writable
 * quality/scalar/native species arrays, receipts, ledgers and diagnostics used by this stage.
 * @param[in] dt Matching accepted positive finite routing duration s; most stages do not
 * validate it.
 * @details Positive water adds q*dt; age/heat use supplied coupling tuple when flagged/covered,
 * otherwise external source configuration. Positive pollutant mass rate is added even if water
 * is nonpositive, and books ex-in mass*dt. Empty carrier returns before pollutant tuple
 * handling. Does not clear coupling receipts; exactly-once after hydraulic drain assembly,
 * prepared full node carrier extent.
 * @note Requires matching initialized solver pollutant count, node/link/subtype arrays and
 * writable quality/ledger extents. Carrier volumes use ft3; pollutant rates use
 * concentration*ft3/s and ledger increments concentration*ft3. No implicit litre or user-mass
 * conversion, hydraulic water advance or engine clock change. Caller serializes quality phases;
 * repeat calls can double-book loads.
 */
void QualitySolver::addCouplingLoads(SimulationContext& ctx, double dt) {
    int np = n_pollutants_;
    if (!loadersNeeded(np, ctx))
        return;
    auto& nodes = ctx.nodes;
    if (nodes.coupling_inflow.empty())
        return;

    for (int i = 0; i < ctx.n_nodes(); ++i) {
        auto ui = static_cast<std::size_t>(i);
        const double q = nodes.coupling_inflow[ui];
        if (q > 0.0) {
            nodes.qual_vol_in[ui] += q * dt;
            // S4: when the surface carries the row, the tuple's own
            // age-volume / temperature-volume arrives (drained by the same
            // rule as the water, so the ratio is the cell's value); the
            // EXTERNAL_INFLOW stand-in survives only for a surface that does
            // not carry that row.
            if (nodes.coupling_tuple_age && ctx.options.water_age &&
                ui < ctx.water_age_state.node_age_vol_in.size() &&
                ui < nodes.coupling_age_vol_inflow.size())
                ctx.water_age_state.node_age_vol_in[ui] += nodes.coupling_age_vol_inflow[ui];
            else
                addAgeVolume(ctx, i, q, WaterAgeSource::EXTERNAL_INFLOW);
            if (nodes.coupling_tuple_temp && ctx.options.heat_transport &&
                ui < ctx.heat_state.node_temp_vol_in.size() &&
                ui < nodes.coupling_temp_vol_inflow.size())
                ctx.heat_state.node_temp_vol_in[ui] += nodes.coupling_temp_vol_inflow[ui];
            else
                addTempVolume(ctx, i, q, HeatSource::EXTERNAL_INFLOW);
        }
        if (nodes.coupling_qual_inflow.empty())
            continue;
        for (int p = 0; p < np; ++p) {
            auto nd_idx = ui * static_cast<std::size_t>(np) + static_cast<std::size_t>(p);
            if (nd_idx >= nodes.coupling_qual_inflow.size())
                continue;
            const double mass_rate = nodes.coupling_qual_inflow[nd_idx];
            if (!(mass_rate > 0.0))
                continue;
            if (nd_idx < nodes.qual_mass_in.size())
                nodes.qual_mass_in[nd_idx] += mass_rate;
            auto pi = static_cast<std::size_t>(p);
            if (pi < ctx.mass_balance.qual_routing_ex_in.size())
                ctx.mass_balance.qual_routing_ex_in[pi] += mass_rate * dt;
        }
    }
}

// ============================================================================
// Add direct external inflow ([INFLOWS] CONCEN/MASS) pollutant loads.
// Matches legacy routing.c addExternalInflows() pollutant portion:
//   w = inflow value; if CONCEN, w *= node flow; Node[j].newQual[p] += w
// ============================================================================

/**
 * @brief Add direct external water and signed pollutant/runtime mass-rate forcing.
 * @param[in,out] ctx Prepared completed hydraulic/source state and writable
 * quality/scalar/native species arrays, receipts, ledgers and diagnostics used by this stage.
 * @param[in] dt Matching accepted positive finite routing duration s; most stages do not
 * validate it.
 * @details Positive ext_inflow adds carrier and scalar numerators; negative water is retained
 * only for lateral correction. Signed ext_qual_mass and API user_conc_mass_flux add rates/ledger
 * mass, even with no positive carrier. API mass flux also books forcing ledger and warns once
 * for extraction; actual available-mass clamp occurs during mixing. Requires lateral scratch
 * initialized by assembly; warning allocation can throw after earlier additions. Does not
 * consume persistent forcing.
 * @note Requires matching initialized solver pollutant count, node/link/subtype arrays and
 * writable quality/ledger extents. Carrier volumes use ft3; pollutant rates use
 * concentration*ft3/s and ledger increments concentration*ft3. No implicit litre or user-mass
 * conversion, hydraulic water advance or engine clock change. Caller serializes quality phases;
 * repeat calls can double-book loads.
 */
void QualitySolver::addExtInflowLoads(SimulationContext& ctx, double dt) {
    int np = n_pollutants_;
    if (!loadersNeeded(np, ctx))
        return;
    auto& nodes = ctx.nodes;

    for (int i = 0; i < ctx.n_nodes(); ++i) {
        auto ui = static_cast<std::size_t>(i);

        // The direct inflow's own water joins the mixing denominator, matching
        // legacy findNodeQual(), which divides the accumulated mass rate by
        // Node[j].inflow — a total that includes the external lateral inflow.
        // (LID drain-to-node water rides nodes.lid_drain_inflow, not this
        //  array; its volume, age and temperature are booked once, by the
        //  drain loader below, at the storage layer's own values.)
        double q = nodes.ext_inflow[ui];
        if (q < 0.0 && ui < qual_vol_lat_neg_.size())
            qual_vol_lat_neg_[ui] += q * dt;
        if (q > 0.0) {
            nodes.qual_vol_in[ui] += q * dt;
            qual_vol_lat_[ui] += q * dt;
            addAgeVolume(ctx, i, q, WaterAgeSource::EXTERNAL_INFLOW);
            addTempVolume(ctx, i, q, HeatSource::EXTERNAL_INFLOW);
        }

        if (nodes.ext_qual_mass.empty())
            continue;
        for (int p = 0; p < np; ++p) {
            auto nd_idx = ui * static_cast<std::size_t>(np) + static_cast<std::size_t>(p);
            if (nd_idx >= nodes.ext_qual_mass.size())
                continue;
            double mass_rate = nodes.ext_qual_mass[nd_idx];
            if (mass_rate == 0.0)
                continue;
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
                auto nd_idx = ui * static_cast<std::size_t>(np) + static_cast<std::size_t>(p);
                if (nd_idx >= nodes.user_conc_mass_flux.size())
                    continue;
                const double w = nodes.user_conc_mass_flux[nd_idx];
                if (w == 0.0)
                    continue;
                // D-NS1 (X6): negative forced mass is extraction —
                // DELIBERATE deviation from legacy's positive-only rule
                // (routing.c `if (w > 0.0)`), per the user decision of
                // 2026-08-23. The signed rate books signed; the mix stage
                // clamps to available and un-books any shortfall. First
                // negative warns once (the API has no parse stage).
                if (w < 0.0 && !ctx.negsrc.api_warned) {
                    ctx.negsrc.api_warned = true;
                    ctx.warnings.push_back("D-NS1: a negative quality mass flux was applied "
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

/**
 * @brief Route accepted legacy quality, native reaction species, age and heat through one
 * interval.
 * @param[in,out] ctx Prepared completed hydraulic/source state and writable
 * quality/scalar/native species arrays, receipts, ledgers and diagnostics used by this stage.
 * @param[in] dt Matching accepted positive finite routing duration s; most stages do not
 * validate it.
 * @details Assembles sources, prepares native LID transfers, accumulates links/mixes nodes,
 * advances untreated HRT, routes native MSX, applies treatment/kinetics, updates links/forcing
 * then age/heat and optional bed-solute exchange. With native LID pollutant cells, repeats
 * fixed-point node mixtures up to100 iterations restoring baseline selected ledgers/inventories
 * each time; nonconvergence warns once and retains final iterate (no error result). Accepted
 * outlet phase follows iteration. No pollutants, legacy reactions, age or heat returns even if
 * other external-volume consumers exist. Caller ensures positive finite dt, initial shapes and
 * unique accepted-step ownership; dependency/allocation failures may leave partial
 * transport/ledgers/receipts. No global rollback or engine clock advance.
 * @note Requires matching initialized solver pollutant count, node/link/subtype arrays and
 * writable quality/ledger extents. Carrier volumes use ft3; pollutant rates use
 * concentration*ft3/s and ledger increments concentration*ft3. No implicit litre or user-mass
 * conversion, hydraulic water advance or engine clock change. Caller serializes quality phases;
 * repeat calls can double-book loads.
 */
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
    if (n_pollutants_ <= 0 && !transport::legacyReactionsActive(ctx) && !ctx.options.water_age &&
        !ctx.options.heat_transport)
        return;

    assembleExternalLoads(ctx, dt);
    lidnode::prepareQuality(ctx, dt);
    msxlid::prepareNodes(ctx, dt);
    const bool coupled_lids =
        n_pollutants_ > 0 && std::any_of(ctx.node_subtypes.storages.lid_state.begin(),
                                         ctx.node_subtypes.storages.lid_state.end(),
                                         [](const auto& state) { return !state.cells.empty(); });
    if (coupled_lids) {
        // LID hydraulics books the accepted port volume once. Use the same
        // mobile mixture for its donor debit and recipient credit, including
        // a reactor that fills and drains within this step. Legacy's lagged
        // zero-volume link value otherwise drops that reactor's entire load.
        const auto mass_in = ctx.nodes.qual_mass_in;
        const auto vol_in = ctx.nodes.qual_vol_in;
        const auto reacted = ctx.mass_balance.qual_routing_reacted;
        const auto seeped = ctx.mass_balance.qual_routing_seep;
        const auto dry = ctx.mass_balance.qual_routing_final_dry;
        const auto external = ctx.mass_balance.qual_routing_ex_in;
        const auto negative_sources = ctx.negsrc;
        std::vector<std::vector<double>> held;
        for (const auto& state : ctx.node_subtypes.storages.lid_state)
            held.push_back(state.quality_mass);
        ctx.nodes.conc = ctx.nodes.conc_old;
        bool converged = false;
        for (int iteration = 0; iteration < 100; ++iteration) {
            const auto previous = ctx.nodes.conc;
            ctx.nodes.qual_mass_in = mass_in;
            ctx.nodes.qual_vol_in = vol_in;
            ctx.mass_balance.qual_routing_reacted = reacted;
            ctx.mass_balance.qual_routing_seep = seeped;
            ctx.mass_balance.qual_routing_final_dry = dry;
            ctx.mass_balance.qual_routing_ex_in = external;
            ctx.negsrc = negative_sources;
            for (std::size_t r = 0; r < held.size(); ++r)
                ctx.node_subtypes.storages.lid_state[r].quality_mass = held[r];
            lidnode::prepareOutletQuality(ctx, dt, false);
            accumulateLinkLoads(ctx, dt);
            mixAtNodes(ctx, dt);
            converged = true;
            for (std::size_t i = 0; i < previous.size(); ++i)
                if (std::abs(previous[i] - ctx.nodes.conc[i]) >
                    1.e-11 * std::max({1.0, std::abs(previous[i]), std::abs(ctx.nodes.conc[i])})) {
                    converged = false;
                    break;
                }
            if (converged)
                break;
        }
        if (!converged) {
            const std::string warning =
                "LID quality mixtures did not converge; inspect quality continuity and reduce the routing step";
            if (std::find(ctx.warnings.begin(), ctx.warnings.end(), warning) == ctx.warnings.end())
                ctx.warnings.push_back(warning);
        }
        // Iteration is provisional: layer reactions are booked only once.
        lidnode::prepareOutletQuality(ctx, dt, true);
    } else {
        accumulateLinkLoads(ctx, dt);
        mixAtNodes(ctx, dt);
    }
    // R4b (transport half): MSX element state advects on the same CSTR
    // mirror family as age and heat. It runs BEFORE the react stages —
    // transport-then-react, the pollutant family's own order — because
    // FORMULA's contract is that react is the LAST writer of a derived
    // species. The first draft placed it after reactLegacyLinks and the
    // FORMULA tracking gate promptly drifted by 4.3e-3: transport was
    // mixing a value the formula had just pinned.
    updateUntreatedStorageHrt(ctx, dt); // review A6
    transport::routeLegacyMsx(ctx, dt);
    // Legacy's per-node order is reaction, then mixing, then treatment — the
    // first two inside findStorageQual, treatmnt_treat immediately after it
    // (qualrout.c's node loop). mixAtNodes now carries the reaction, so this
    // call is in the right place; the decay stage below is a no-op left in
    // the sequence for shape.
    applyTreatment(ctx, dt);
    // R4: with a reactions component configured, pollutant decay upgrades to
    // the exact exponential and MSX species react per element via the shared
    // integrator (ReactionLegacyBinding). Nodes react here (where applyDecay
    // ran); links react AFTER updateLinkQuality so the mixing pass does not
    // overwrite them (its internal linear decay is zeroed below). Without a
    // reactions component the legacy path runs untouched — bit-parity
    // (G-UT1).
    if (transport::legacyReactionsActive(ctx)) {
        transport::reactLegacyNodes(ctx, dt);
        msxlid::reactNodes(ctx, dt);
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

    // H6b: the bed's SOLUTE exchange, after every pollutant stage and after
    // the heat mirror (whose own bed coupling is inside `applyHeatFluxes`).
    //
    // It runs last for the same reason the two mirrors do: it reads the
    // settled link concentrations rather than an intermediate, so with
    // `[HEAT_FLUXES] SEDIMENT_EXCHANGE` off — which is the default — every
    // pollutant trajectory is bit-identical to the pre-H6b engine.
    //
    // `ctx.links.conc` is already [link * np + p], the layout
    // `applyBedSoluteExchange` documents, so the array is passed rather than
    // repacked. A repack would be a second layout to keep in agreement.
    //
    // The bed carries ONE species set — pollutants then MSX — and LEGACY
    // holds those in two arrays, so two calls with offsets into the same
    // store. MSX rows exchange like the reference's solutes do
    // (HTSComponent carries its transient-storage solutes generically).
    if (transport::heat::bedExchangeEnabled(ctx)) {
        const int np = ctx.links.conc_n_pollutants;
        const int nm = ctx.reactions.n_species();
        const int nt = (np > 0 ? np : 0) + (nm > 0 ? nm : 0);
        if (np > 0 && !ctx.links.conc.empty())
            transport::heat::applyBedSoluteExchange(ctx, ctx.links.conc.data(), np, 0, nt, dt);
        if (nm > 0 && !ctx.reactions.msx_link_conc.empty())
            transport::heat::applyBedSoluteExchange(ctx, ctx.reactions.msx_link_conc.data(), nm,
                                                    (np > 0 ? np : 0), nt, dt);
    }
}

// ============================================================================
// Add subcatchment washoff loads to node quality inflows — VECTORISABLE
// Matches legacy addWetWeatherInflows() in routing.c
// ============================================================================

/**
 * @brief Add old/new runoff and conventional LID drain mass-rate blends at valid outlet nodes.
 * @param[in,out] ctx Prepared completed hydraulic/source state and writable
 * quality/scalar/native species arrays, receipts, ledgers and diagnostics used by this stage.
 * @param[in] dt Matching accepted positive finite routing duration s; most stages do not
 * validate it.
 * @details Uses runoff_interp_f unbounded: blended positive flow gates carrier/scalar addition,
 * but pollutant rate is blend(q_old*c_old,q_new*c_new), not mean flow times current
 * concentration. Only positive mass rates are added/booked to wet ledger; missing
 * concentration/history uses0. LID carrier and load histories use same blend, prepared layer
 * age/temperature numerators when active else rainfall source scalar. No queue clearing; caller
 * initializes lateral scratch and calls once per routing step.
 * @note Requires matching initialized solver pollutant count, node/link/subtype arrays and
 * writable quality/ledger extents. Carrier volumes use ft3; pollutant rates use
 * concentration*ft3/s and ledger increments concentration*ft3. No implicit litre or user-mass
 * conversion, hydraulic water advance or engine clock change. Caller serializes quality phases;
 * repeat calls can double-book loads.
 */
void QualitySolver::addWetWeatherLoads(SimulationContext& ctx, double dt) {
    int np = n_pollutants_;

    for (int i = 0; i < ctx.n_subcatches(); ++i) {
        auto ui = static_cast<std::size_t>(i);
        int out_node = ctx.subcatches.outlet_node[ui];
        if (out_node < 0 || out_node >= ctx.n_nodes())
            continue;
        auto ud = static_cast<std::size_t>(out_node);

        // legacy addWetWeatherInflows (routing.c:713-719) weights both the
        // runoff flow and the washoff load beside it by where THIS routing
        // instant falls between the two runoff times:
        //     f = (routingTime - OldRunoffTime)/(NewRunoffTime - OldRunoffTime)
        // This used a fixed midpoint, 0.5*(old + new), for both. With a 60 s
        // routing step inside a 15-minute runoff step legacy sweeps f from 0
        // to 1 across fifteen steps while this delivered the average every
        // time, so a storm's load arrived on the wrong schedule — and the
        // FLOW side of the same seam (assembleLateralInflows) had been using
        // legacy's f all along, so the two halves disagreed with each other.
        const double f = ctx.runoff_interp_f;
        const double f1 = 1.0 - f;
        double q_new = ctx.subcatches.runoff[ui];
        double q_old = ctx.subcatches.old_runoff[ui];
        double q = f1 * q_old + f * q_new;
        if (q <= 0.0)
            continue;

        ctx.nodes.qual_vol_in[ud] += q * dt;
        if (ud < qual_vol_lat_.size())
            qual_vol_lat_[ud] += q * dt;
        // A3: runoff arrives at the age the SUBCATCHMENT computed, not at the
        // configured RAINFALL age. The rainfall age is what enters the
        // subareas; by the time water leaves it has aged on the surface and
        // mixed with whatever was already ponded, and that is the number the
        // node must receive. Falls back to the configured source age when the
        // watershed state is unsized (WATER_AGE on, runoff never stepped).
        {
            const bool have_sc =
                ctx.options.water_age && ui < ctx.water_age_state.subcatch_runoff_age.size();
            if (have_sc) {
                auto& acc = ctx.water_age_state.node_age_vol_in;
                if (ud < acc.size())
                    acc[ud] += q * ctx.water_age_state.subcatch_runoff_age[ui];
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
            const bool have_sc =
                ctx.options.heat_transport && ui < ctx.heat_state.subcatch_runoff_temp.size();
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
            double c_new =
                (sc_idx < ctx.subcatches.conc.size()) ? ctx.subcatches.conc[sc_idx] : 0.0;
            double c_old =
                (sc_idx < ctx.subcatches.conc_old.size()) ? ctx.subcatches.conc_old[sc_idx] : 0.0;

            // legacy surfqual_getWtdWashoff: the interpolated PRODUCT
            // runoff*conc, not the product of the two interpolations — so the
            // effective concentration the node sees is not the interpolated
            // concentration. Same f as the flow above.
            double mass_rate = f1 * (q_old * c_old) + f * (q_new * c_new);

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
    // legacy lid_addDrainInflow weights both halves of the pair by where
    // this routing instant falls between the two runoff steps:
    //   q = (1-f)*oldDrainFlow + f*newDrainFlow
    //   w = (1-f)*oldDrainFlow*oldQual + f*newDrainFlow*newQual
    // The per-node accumulators hold exactly those products, so the same
    // f addWetWeatherLoads uses interpolates them per node.
    const double f_dr = ctx.runoff_interp_f;
    const double f1_dr = 1.0 - f_dr;
    for (int j = 0; j < ctx.n_nodes(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        double v_new =
            (uj < ctx.nodes.lid_drain_qual_vol.size()) ? ctx.nodes.lid_drain_qual_vol[uj] : 0.0;
        double v_old = (uj < ctx.nodes.lid_drain_qual_vol_old.size())
                           ? ctx.nodes.lid_drain_qual_vol_old[uj]
                           : 0.0;
        double drain_vol_rate = f1_dr * v_old + f_dr * v_new;
        if (drain_vol_rate <= 0.0)
            continue;

        ctx.nodes.qual_vol_in[uj] += drain_vol_rate * dt;
        if (uj < qual_vol_lat_.size())
            qual_vol_lat_[uj] += drain_vol_rate * dt;
        // A4 RETIRES the RAINFALL stand-in here: the drain now arrives at the
        // age of the LID storage layer it was drawn from, accumulated as a
        // q·age rate in A6b beside this very volume. Falls back to the
        // configured source age only when the layer block is unsized (WATER_AGE
        // on but the LID manager built no units).
        if (ctx.options.water_age && uj < ctx.water_age_state.node_lid_drain_age_vol_in.size() &&
            ctx.lid_layer_state.active()) {
            auto& acc = ctx.water_age_state.node_age_vol_in;
            if (uj < acc.size())
                acc[uj] += ctx.water_age_state.node_lid_drain_age_vol_in[uj];
        } else {
            addAgeVolume(ctx, j, drain_vol_rate, WaterAgeSource::RAINFALL);
        }
        // H5b at the node seam: the drain arrives at its storage layer's
        // temperature, accumulated beside this volume in the runoff step.
        // The RAINFALL stand-in survives only when the layer block is unsized.
        if (ctx.options.heat_transport && uj < ctx.heat_state.node_lid_drain_temp_vol_in.size() &&
            ctx.lid_layer_state.active()) {
            auto& tacc = ctx.heat_state.node_temp_vol_in;
            if (uj < tacc.size())
                tacc[uj] += ctx.heat_state.node_lid_drain_temp_vol_in[uj];
        } else {
            addTempVolume(ctx, j, drain_vol_rate, HeatSource::RAINFALL);
        }

        for (int p = 0; p < np; ++p) {
            auto nd_idx = uj * static_cast<std::size_t>(np) + static_cast<std::size_t>(p);
            double w_new = (nd_idx < ctx.nodes.lid_drain_qual_load.size())
                               ? ctx.nodes.lid_drain_qual_load[nd_idx]
                               : 0.0;
            double w_old = (nd_idx < ctx.nodes.lid_drain_qual_load_old.size())
                               ? ctx.nodes.lid_drain_qual_load_old[nd_idx]
                               : 0.0;
            double load = f1_dr * w_old + f_dr * w_new;
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

/**
 * @brief Add positive RDII water and positive configured pollutant concentration loads.
 * @param[in,out] ctx Prepared completed hydraulic/source state and writable
 * quality/scalar/native species arrays, receipts, ledgers and diagnostics used by this stage.
 * @param[in] dt Matching accepted positive finite routing duration s; most stages do not
 * validate it.
 * @details Adds carrier q*dt, source age/temperature rates and q*c pollutant rate, booking
 * q*c*dt to II ledger. Zero/negative water or concentration skips corresponding contribution;
 * requires assembly lateral scratch and covered per-pollutant defaults. No source consumption or
 * negative-lateral tracking.
 * @note Requires matching initialized solver pollutant count, node/link/subtype arrays and
 * writable quality/ledger extents. Carrier volumes use ft3; pollutant rates use
 * concentration*ft3/s and ledger increments concentration*ft3. No implicit litre or user-mass
 * conversion, hydraulic water advance or engine clock change. Caller serializes quality phases;
 * repeat calls can double-book loads.
 */
void QualitySolver::addRdiiLoads(SimulationContext& ctx, double dt) {
    int np = n_pollutants_;
    if (!loadersNeeded(np, ctx))
        return;
    auto& nodes = ctx.nodes;

    for (int i = 0; i < ctx.n_nodes(); ++i) {
        auto ui = static_cast<std::size_t>(i);
        double q = nodes.rdii_inflow[ui];
        if (q <= 0.0)
            continue;

        // Add volume inflow from RDII
        nodes.qual_vol_in[ui] += q * dt;
        qual_vol_lat_[ui] += q * dt;
        addAgeVolume(ctx, i, q, WaterAgeSource::RDII);
        addTempVolume(ctx, i, q, HeatSource::RDII);

        // Add pollutant mass loads: mass_rate = q * c_rdii[p]
        // Matching legacy: w = q * Pollut[p].rdiiConcen
        OPENSWMM_IVDEP
        for (int p = 0; p < np; ++p) {
            double c_rdii = ctx.pollutants.c_rdii[static_cast<std::size_t>(p)];
            if (c_rdii <= 0.0)
                continue;
            auto nd_idx = ui * static_cast<std::size_t>(np) + static_cast<std::size_t>(p);
            if (nd_idx < nodes.qual_mass_in.size()) {
                nodes.qual_mass_in[nd_idx] += q * c_rdii;
            }
        }

        // Mass balance: track RDII quality inflow
        for (int p = 0; p < np; ++p) {
            double c_rdii = ctx.pollutants.c_rdii[static_cast<std::size_t>(p)];
            if (c_rdii <= 0.0)
                continue;
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

/**
 * @brief Add positive DWF carrier and default plus evaluated pollutant quality adjustments.
 * @param[in,out] ctx Prepared completed hydraulic/source state and writable
 * quality/scalar/native species arrays, receipts, ledgers and diagnostics used by this stage.
 * @param[in] dt Matching accepted positive finite routing duration s; most stages do not
 * validate it.
 * @details Tracks negative water for denominator correction, but loads only positive water. Rate
 * is q*max(default concentration,0)+dwf_qual_mass adjustment; signed nonzero total is added and
 * booked DW mass*dt. Adds configured DWF age/temperature numerators. Requires initialized
 * lateral scratch; persistent input arrays remain for later steps, actual extraction clamp
 * occurs at mixing.
 * @note Requires matching initialized solver pollutant count, node/link/subtype arrays and
 * writable quality/ledger extents. Carrier volumes use ft3; pollutant rates use
 * concentration*ft3/s and ledger increments concentration*ft3. No implicit litre or user-mass
 * conversion, hydraulic water advance or engine clock change. Caller serializes quality phases;
 * repeat calls can double-book loads.
 */
void QualitySolver::addDwfLoads(SimulationContext& ctx, double dt) {
    int np = n_pollutants_;
    if (!loadersNeeded(np, ctx))
        return;
    auto& nodes = ctx.nodes;

    for (int i = 0; i < ctx.n_nodes(); ++i) {
        auto ui = static_cast<std::size_t>(i);
        double q = nodes.dwf_inflow[ui];
        if (q < 0.0 && ui < qual_vol_lat_neg_.size())
            qual_vol_lat_neg_[ui] += q * dt;
        if (q <= 0.0)
            continue;

        // Add volume inflow from DWF (legacy qualrout.c uses Node[j].inflow,
        // which includes DWF, as the mixing denominator). Without this the
        // mass added below is discarded by mixAtNodes when v_in == 0.
        nodes.qual_vol_in[ui] += q * dt;
        qual_vol_lat_[ui] += q * dt;
        addAgeVolume(ctx, i, q, WaterAgeSource::DWF);
        addTempVolume(ctx, i, q, HeatSource::DWF);

        OPENSWMM_IVDEP
        for (int p = 0; p < np; ++p) {
            double c_dwf = ctx.pollutants.c_dwf[static_cast<std::size_t>(p)];
            auto nd_idx = ui * static_cast<std::size_t>(np) + static_cast<std::size_t>(p);
            // Per-node [DWF] pollutant rows arrive as a net adjustment on top
            // of the global default (InflowSolver pass 2 — legacy
            // addDryWeatherInflows adds the row's q·concentration and
            // subtracts the q·dwfConcen default it displaces).
            double w_adj =
                (nd_idx < nodes.dwf_qual_mass.size()) ? nodes.dwf_qual_mass[nd_idx] : 0.0;
            double w = (c_dwf > 0.0 ? q * c_dwf : 0.0) + w_adj;
            if (w == 0.0)
                continue;
            if (nd_idx < nodes.qual_mass_in.size()) {
                nodes.qual_mass_in[nd_idx] += w;
            }
            // Mass balance: track dry weather quality inflow
            auto pi = static_cast<std::size_t>(p);
            if (pi < ctx.mass_balance.qual_routing_dw_in.size()) {
                ctx.mass_balance.qual_routing_dw_in[pi] += w * dt;
            }
        }
    }
}

// ============================================================================
// Add groundwater inflow pollutant loads to node quality inflows
// Matches legacy addGroundwaterInflows() pollutant portion
// (routing.c:678-686: w = q * Pollut[p].gwConcen)
// ============================================================================

/**
 * @brief Add positive groundwater carrier and configured positive pollutant groundwater loads.
 * @param[in,out] ctx Prepared completed hydraulic/source state and writable
 * quality/scalar/native species arrays, receipts, ledgers and diagnostics used by this stage.
 * @param[in] dt Matching accepted positive finite routing duration s; most stages do not
 * validate it.
 * @details Negative groundwater flow only records lateral denominator correction. Positive q
 * adds q*dt, source scalar rates and q*c for positive c_gw, booking GW mass*dt. Requires
 * initialized lateral scratch; quality-only gaining-conduit receipts are handled separately, not
 * consumed here.
 * @note Requires matching initialized solver pollutant count, node/link/subtype arrays and
 * writable quality/ledger extents. Carrier volumes use ft3; pollutant rates use
 * concentration*ft3/s and ledger increments concentration*ft3. No implicit litre or user-mass
 * conversion, hydraulic water advance or engine clock change. Caller serializes quality phases;
 * repeat calls can double-book loads.
 */
void QualitySolver::addGwLoads(SimulationContext& ctx, double dt) {
    int np = n_pollutants_;
    if (!loadersNeeded(np, ctx))
        return;
    auto& nodes = ctx.nodes;

    for (int i = 0; i < ctx.n_nodes(); ++i) {
        auto ui = static_cast<std::size_t>(i);
        double q = nodes.gw_inflow[ui];
        if (q < 0.0 && ui < qual_vol_lat_neg_.size())
            qual_vol_lat_neg_[ui] += q * dt;
        if (q <= 0.0)
            continue; // pollutant load only for positive inflow

        // Add volume inflow from groundwater (see addDwfLoads: the mass below
        // is discarded by mixAtNodes unless its carrier volume is counted).
        nodes.qual_vol_in[ui] += q * dt;
        qual_vol_lat_[ui] += q * dt;
        addAgeVolume(ctx, i, q, WaterAgeSource::GW);
        addTempVolume(ctx, i, q, HeatSource::GW);

        OPENSWMM_IVDEP
        for (int p = 0; p < np; ++p) {
            double c_gw = ctx.pollutants.c_gw[static_cast<std::size_t>(p)];
            if (c_gw <= 0.0)
                continue;
            auto nd_idx = ui * static_cast<std::size_t>(np) + static_cast<std::size_t>(p);
            if (nd_idx < nodes.qual_mass_in.size()) {
                nodes.qual_mass_in[nd_idx] += q * c_gw;
            }
        }

        // Mass balance: track groundwater quality inflow
        for (int p = 0; p < np; ++p) {
            double c_gw = ctx.pollutants.c_gw[static_cast<std::size_t>(p)];
            if (c_gw <= 0.0)
                continue;
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

/**
 * @brief Add positive routing-interface water and positive supplied pollutant mass rates.
 * @param[in,out] ctx Prepared completed hydraulic/source state and writable
 * quality/scalar/native species arrays, receipts, ledgers and diagnostics used by this stage.
 * @param[in] dt Matching accepted positive finite routing duration s; most stages do not
 * validate it.
 * @details Empty iface_qual_mass returns before any carrier/scalar handling. Negative water only
 * records lateral correction; positive water adds q*dt, source age/temperature and positive
 * pollutant rates, booking ex-in mass*dt. Missing individual pollutant rate tuple uses0.
 * Prepared lateral scratch required; no interface read/advance or receipt consumption.
 * @note Requires matching initialized solver pollutant count, node/link/subtype arrays and
 * writable quality/ledger extents. Carrier volumes use ft3; pollutant rates use
 * concentration*ft3/s and ledger increments concentration*ft3. No implicit litre or user-mass
 * conversion, hydraulic water advance or engine clock change. Caller serializes quality phases;
 * repeat calls can double-book loads.
 */
void QualitySolver::addIfaceLoads(SimulationContext& ctx, double dt) {
    int np = n_pollutants_;
    if (!loadersNeeded(np, ctx))
        return;
    auto& nodes = ctx.nodes;
    if (nodes.iface_qual_mass.empty())
        return;

    for (int i = 0; i < ctx.n_nodes(); ++i) {
        auto ui = static_cast<std::size_t>(i);
        double q = nodes.iface_inflow[ui];
        if (q < 0.0 && ui < qual_vol_lat_neg_.size())
            qual_vol_lat_neg_[ui] += q * dt;
        if (q <= 0.0)
            continue;

        // Add volume inflow from the interface file
        nodes.qual_vol_in[ui] += q * dt;
        qual_vol_lat_[ui] += q * dt;
        addAgeVolume(ctx, i, q, WaterAgeSource::IFACE);
        addTempVolume(ctx, i, q, HeatSource::IFACE);

        OPENSWMM_IVDEP
        for (int p = 0; p < np; ++p) {
            auto nd_idx = ui * static_cast<std::size_t>(np) + static_cast<std::size_t>(p);
            double mass_rate =
                (nd_idx < nodes.iface_qual_mass.size()) ? nodes.iface_qual_mass[nd_idx] : 0.0;
            if (mass_rate <= 0.0)
                continue;
            if (nd_idx < nodes.qual_mass_in.size()) {
                nodes.qual_mass_in[nd_idx] += mass_rate;
            }
        }

        // Mass balance: interface loads count as external quality inflow
        // (legacy massbal_addInflowQual(EXTERNAL_INFLOW, p, w))
        for (int p = 0; p < np; ++p) {
            auto nd_idx = ui * static_cast<std::size_t>(np) + static_cast<std::size_t>(p);
            double mass_rate =
                (nd_idx < nodes.iface_qual_mass.size()) ? nodes.iface_qual_mass[nd_idx] : 0.0;
            if (mass_rate <= 0.0)
                continue;
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

/**
 * @brief Accumulate signed-flow downstream link loads and native LID quality port receipts.
 * @param[in,out] ctx Prepared completed hydraulic/source state and writable
 * quality/scalar/native species arrays, receipts, ledgers and diagnostics used by this stage.
 * @param[in] dt Matching accepted positive finite routing duration s; most stages do not
 * validate it.
 * @details Positive abs(flow) adds downstream carrier volume and old link concentration mass
 * rate; upstream shortcut links touching native LID use current node/outlet quality, with
 * outfall-backflow policy. Matching native LID quality ports capture incoming mass and subtract
 * captured water from mobile mixing denominator. Outfall-to-LID shortcuts book external mass.
 * Requires valid source node IDs and all accepted port mappings; does not clear prior assembly
 * or advance hydraulic receipts.
 * @note Requires matching initialized solver pollutant count, node/link/subtype arrays and
 * writable quality/ledger extents. Carrier volumes use ft3; pollutant rates use
 * concentration*ft3/s and ledger increments concentration*ft3. No implicit litre or user-mass
 * conversion, hydraulic water advance or engine clock change. Caller serializes quality phases;
 * repeat calls can double-book loads.
 */
void QualitySolver::accumulateLinkLoads(SimulationContext& ctx, double dt) {
    int np = n_pollutants_;
    auto& links = ctx.links;

    // Batch over all links — inner loop over pollutants is vectorisable
    for (int j = 0; j < ctx.n_links(); ++j) {
        auto uj = static_cast<size_t>(j);
        double q = std::fabs(links.flow[uj]);
        if (q <= 0.0)
            continue;

        // Downstream node (based on flow direction)
        int downstream = (links.flow[uj] >= 0.0) ? links.node2[uj] : links.node1[uj];
        if (downstream < 0 || downstream >= ctx.n_nodes())
            continue;
        auto ud = static_cast<size_t>(downstream);

        ctx.nodes.qual_vol_in[ud] += q * dt;

        // Vectorisable inner loop over pollutants
        OPENSWMM_IVDEP
        for (int p = 0; p < np; ++p) {
            auto lp = uj * static_cast<size_t>(np) + static_cast<size_t>(p);
            auto np_idx = ud * static_cast<size_t>(np) + static_cast<size_t>(p);

            double c_link = (lp < links.conc_old.size()) ? links.conc_old[lp] : 0.0;
            if (linkTakesUpstreamValue(ctx, j)) {
                const int source = links.flow[uj] >= 0 ? links.node1[uj] : links.node2[uj];
                if (lidnode::active(ctx, source) || lidnode::active(ctx, downstream)) {
                    // The coupled LID solve uses one mobile mixture on both
                    // sides of an accepted zero-volume link transfer.
                    c_link = ctx.nodes.conc[source * np + p];
                    if (ctx.nodes.type[source] == NodeType::OUTFALL)
                        c_link = ctx.options.outfall_backflow_zero
                                     ? 0.0
                                     : ctx.nodes.conc_old[source * np + p];
                }
                c_link = lidnode::outletQuality(ctx, source, j, p, c_link);
            }
            double mass = q * c_link;
            if (linkTakesUpstreamValue(ctx, j) && lidnode::active(ctx, downstream)) {
                const int source = links.flow[uj] >= 0 ? links.node1[uj] : links.node2[uj];
                // Reverse flow from a held-quality boundary is an external
                // pollutant source, just as it is an external water source.
                if (ctx.nodes.type[source] == NodeType::OUTFALL)
                    ctx.mass_balance.qual_routing_ex_in[p] += mass * dt;
            }
            if (np_idx < ctx.nodes.qual_mass_in.size()) {
                ctx.nodes.qual_mass_in[np_idx] += mass;
                lidnode::receiveQuality(ctx, downstream, j, q * dt, p, mass, dt);
            }
        }
        const int r = ctx.node_subtypes.storage_row(downstream);
        if (r >= 0)
            for (const auto& port : ctx.node_subtypes.storages.lid_state[r].quality_ports)
                if (port.link == j && port.volume > 0)
                    ctx.nodes.qual_vol_in[ud] -= port.volume;
    }
    (void)dt;
}

// ============================================================================
// Complete mixing at all nodes — legacy qualrout.c findStorageQual /
// findNodeQual, op for op.
//
// Legacy dispatches on the node: a STORAGE unit, or any node that STARTED the
// step holding more than a litre, is a mixed reactor whose own contents are
// part of the balance (findStorageQual); everything else is pure flow-through
// whose concentration is simply mass-in over flow-in, with no volume term, no
// evaporation factor and no reaction (findNodeQual). This engine ran one
// formula for every node and approximated the second case by substituting
// `mass_in / v_in` whenever the starting volume was negligible — which is the
// right answer for a junction and the WRONG one for a storage unit filling
// from dry, where legacy still divides by (v1 + vIn).
// ============================================================================

/**
 * @brief Mix pollutant node pools with legacy reactor, dry, evaporation and treatment ownership
 * rules.
 * @param[in,out] ctx Prepared completed hydraulic/source state and writable
 * quality/scalar/native species arrays, receipts, ledgers and diagnostics used by this stage.
 * @param[in] dt Matching accepted positive finite routing duration s; most stages do not
 * validate it.
 * @details Flow-through uses assembled rate/volume, old quality for retained depth without
 * inflow, or raw mass-rate fallback at dry node. Reactor uses old-water evaporation multiplier,
 * old-pool explicit first-order decay unless native kinetics or pollutant treatment owns it,
 * mixed concentration cap, dry reset, seep/reacted/dry ledgers. Negative external mass is capped
 * to available old inventory and books clamp correction; native LID nodes retain pool at
 * otherwise dry threshold. No generic finite repair; serial shared-ledger updates. Requires
 * correct old/current node shapes and source assembly; may update warnings/diagnostics and
 * earlier nodes before dependency exception.
 * @note Requires matching initialized solver pollutant count, node/link/subtype arrays and
 * writable quality/ledger extents. Carrier volumes use ft3; pollutant rates use
 * concentration*ft3/s and ledger increments concentration*ft3. No implicit litre or user-mass
 * conversion, hydraulic water advance or engine clock change. Caller serializes quality phases;
 * repeat calls can double-book loads.
 */
void QualitySolver::mixAtNodes(SimulationContext& ctx, double dt) {
    int np = n_pollutants_;
    auto& nodes = ctx.nodes;
    auto& poll = ctx.pollutants;
    auto& mb = ctx.mass_balance;
    // OUTFALL_BACKFLOW_QUALITY ZERO: a supplying outfall is a fresh
    // boundary — its held state reads zero, so backflow re-enters clean
    // instead of carrying the last mix (legacy LAST keeps the hold).
    const bool zero_bf_opt = ctx.options.outfall_backflow_zero;
    // R4: with a reactions component the exact exponential in
    // reactLegacyNodes replaces this linear decay, and must not double-apply.
    const bool reactions_active = transport::legacyReactionsActive(ctx);
    const bool any_treatment = ctx.treatment.hasAny();

    // Batch over all nodes — inner loop over pollutants is vectorisable
    // Outer node loop is parallelisable: each node reads only its own
    // pre-computed mass_in and vol_in (scatter phase is complete).
    // Legacy parity: src/legacy/engine/qualrout.c runs this loop serially.
#if defined(SWMM_USE_OPENMP)
// #pragma omp parallel for schedule(static)
#endif
    for (int i = 0; i < ctx.n_nodes(); ++i) {
        auto ui = static_cast<size_t>(i);
        const double v_old = nodes.old_volume[ui];
        const double v_in = nodes.qual_vol_in[ui];
        const bool is_storage = (nodes.type[ui] == NodeType::STORAGE);
        const bool zero_bf = zero_bf_opt && nodes.type[ui] == NodeType::OUTFALL;

        // --- legacy qualrout_execute's dispatch (see nodeIsReactor).
        if (!nodeIsReactor(ctx, i)) {
            // ---- findNodeQual: no storage volume, so the concentration is
            //      just the inflow's, and nothing reacts or concentrates.
            //
            // legacy tests the inflow RATE against ZERO (1e-10 cfs), not the
            // volume against nothing: `if (qNode > ZERO)`, qualrout.c:301.
            // v6 divided whenever v_in was merely non-zero, so a node taking
            // 1.6e-18 cfs of numerical residue divided a residual mass by a
            // residual volume and published the ratio. On duobiocell — a deck
            // that never generates one drop of runoff — that read 227 mg/L at
            // a dry, empty junction where legacy reads zero. The reactor half
            // of this kernel has tested the rate against the same constant
            // since the node-CSTR round (see nodeIsDry); only this half was
            // left behind.
            const double q_node = (dt > 0.0) ? v_in / dt : 0.0;
            for (int p = 0; p < np; ++p) {
                auto idx = ui * static_cast<size_t>(np) + static_cast<size_t>(p);
                if (idx >= nodes.conc.size())
                    continue;
                if (q_node > LEGACY_ZERO) {
                    double mass_in =
                        (idx < nodes.qual_mass_in.size()) ? nodes.qual_mass_in[idx] * dt : 0.0;
                    // D-NS1 (X6): a negative load is extraction, clamped to
                    // the mass the store holds; the shortfall is counted and
                    // un-booked so the ledger carries what actually left. The
                    // branch is untaken on every non-negative deck.
                    if (mass_in < 0.0) {
                        const double avail = nodes.conc_old[idx] * v_old;
                        if (mass_in < -avail) {
                            bookNegativeSourceClamp(ctx, i, p, -(avail + mass_in));
                            mass_in = -avail;
                        }
                    }
                    nodes.conc[idx] = mass_in / v_in;
                } else if (zero_bf) {
                    nodes.conc[idx] = 0.0;
                } else if (nodes.depth[ui] > ZERO_DEPTH) {
                    // Legacy 5.2.1: a WET node with no inflow holds its
                    // quality.
                    nodes.conc[idx] = nodes.conc_old[idx];
                } else {
                    // A DRY one leaves Node.newQual as the accumulator
                    // (qualrout.c findNodeQual): the summed q*c load rate,
                    // published as its concentration. The inflow is only
                    // at or below ZERO (1e-10 cfs), not absent, so the
                    // load is tiny but not zero (1943-h-h-elements-si-units
                    // reports 1e-10..1e-13 mg/L there).
                    nodes.conc[idx] =
                        (idx < nodes.qual_mass_in.size()) ? nodes.qual_mass_in[idx] : 0.0;
                }
            }
            continue;
        }

        // ---- findStorageQual: a mixed reactor.
        // Evaporation concentration factor (P8-G20, repaired in KD1):
        // legacy scales the STORED concentration by fEvap = 1 + vEvap/v1 from
        // the storage unit's ACTUAL evaporation volume. An earlier spelling
        // inferred evaporation from v_new < v_old + v_in — true for EVERY
        // draining node, and outflow is not evaporation — inflating the store
        // to retain mass the downstream link had already pulled.
        const double f_evap = storageEvapFactor(ctx, i);
        double q_exfil = 0.0;
        if (is_storage) {
            const int srow = ctx.node_subtypes.storage_row(i);
            if (srow >= 0) {
                const auto us = static_cast<size_t>(srow);
                const auto& st = ctx.node_subtypes.storages;
                const double v_exfil = (us < st.exfil_loss.size()) ? st.exfil_loss[us] : 0.0;
                // legacy holds Storage[k].exfilLoss as a VOLUME over the step
                // and books the seepage ledger at the rate exfilLoss/tStep.
                q_exfil = (dt > 0.0) ? v_exfil / dt : 0.0;
            }
        }

        // Legacy zeroes a reactor that ends the step essentially empty AND is
        // taking nothing in, booking what it held to final storage.
        //
        // NOTE the inflow RATE here is v_in/dt, where legacy reads
        // Node[j].inflow — the single number the hydraulics produced, summed
        // as rates and multiplied by dt once, against this engine's
        // qual_vol_in, which sums per-source volumes each already multiplied
        // by dt. The two agree to within summation order; closing that gap
        // means moving every consumer of qual_vol_in and is left standing.
        const double q_in = (dt > 0.0) ? v_in / dt : 0.0;
        // A LID may release freshly percolated water and end this step
        // with an empty mobile store. Its outgoing links still need the
        // mixture that supplied that water; zeroing it drops the load.
        const bool dry = nodeIsDry(ctx, i, q_in) && !lidnode::active(ctx, i);

        for (int p = 0; p < np; ++p) {
            auto idx = ui * static_cast<size_t>(np) + static_cast<size_t>(p);
            if (idx >= nodes.conc.size())
                continue;
            const auto up = static_cast<size_t>(p);

            double c1 = nodes.conc_old[idx];

            // legacy massbal_addSeepageLoss(p, qExfil * c1), booked BEFORE
            // the evaporation factor is applied.
            const double seeped = q_exfil * c1 * dt;

            c1 *= f_evap;

            // getReactedQual — legacy applies the first-order reaction to the
            // STORED concentration BEFORE mixing in this step's inflow, and
            // SKIPS it entirely for a pollutant that has its own treatment
            // equation at this node. This engine decayed AFTER mixing, in a
            // separate pass that also bit the fresh inflow.
            double k = (up < poll.k_decay.size()) ? poll.k_decay[up] : 0.0;
            if (reactions_active)
                k = 0.0;
            if (k != 0.0 && any_treatment && nodeHasTreatmentFor(ctx, i, p))
                k = 0.0;
            double reacted = 0.0;
            if (k != 0.0) {
                const double c2 = std::max(0.0, c1 * (1.0 - k * dt));
                reacted = (c1 - c2) * v_old;
                c1 = c2;
            }

            double mass_in = (idx < nodes.qual_mass_in.size()) ? nodes.qual_mass_in[idx] * dt : 0.0;
            if (mass_in < 0.0) {
                const double avail = nodes.conc_old[idx] * v_old;
                if (mass_in < -avail) {
                    bookNegativeSourceClamp(ctx, i, p, -(avail + mass_in));
                    mass_in = -avail;
                }
            }

            // getMixedQual(c1, v_old, wIn, q_in, dt), in legacy's own operand
            // order: cIn is formed from the inflow load, the mixture is
            // capped at max(c, cIn), and the whole thing is skipped when the
            // inflow is below the effective zero.
            double c_new;
            if (q_in <= LEGACY_ZERO) {
                c_new = c1;
            } else {
                const double c_in = mass_in / v_in;
                const double c_max = std::max(c1, c_in);
                c_new = (c1 * v_old + mass_in) / (v_old + v_in);
                c_new = std::min(c_new, c_max);
                c_new = std::max(c_new, 0.0);
            }

            if (dry) {
                if (up < mb.qual_routing_final_dry.size())
                    mb.qual_routing_final_dry[up] += c_new * nodes.volume[ui];
                c_new = 0.0;
            }

            c_new = std::max(c_new, 0.0);
            nodes.conc[idx] = c_new;

            if (reacted > 0.0 && up < mb.qual_routing_reacted.size())
                mb.qual_routing_reacted[up] += reacted;
            if (seeped != 0.0 && up < mb.qual_routing_seep.size())
                mb.qual_routing_seep[up] += seeped;
        }
    }
}

// ============================================================================
// First-order decay — VECTORISABLE (batch multiply)
// ============================================================================

/**
 * @brief Retain the compatibility decay-stage hook without applying a second decay.
 * @param[in,out] ctx Prepared completed hydraulic/source state and writable
 * quality/scalar/native species arrays, receipts, ledgers and diagnostics used by this stage.
 * @param[in] dt Matching accepted positive finite routing duration s; most stages do not
 * validate it.
 * @details Currently a no-op: node decay is inside mixAtNodes and link decay inside
 * updateLinkQuality. Neither context nor dt is read or changed; no allocation, ledger or clock
 * effect.
 * @note Requires matching initialized solver pollutant count, node/link/subtype arrays and
 * writable quality/ledger extents. Carrier volumes use ft3; pollutant rates use
 * concentration*ft3/s and ledger increments concentration*ft3. No implicit litre or user-mass
 * conversion, hydraulic water advance or engine clock change. Caller serializes quality phases;
 * repeat calls can double-book loads.
 */
void QualitySolver::applyDecay(SimulationContext& ctx, double dt) {
    // Both halves of the first-order reaction now live where legacy puts
    // them: inside the mixing kernels, applied to the STORED concentration
    // before this step's inflow joins it (getReactedQual, called from
    // findStorageQual and findLinkQual). Running it here as a separate pass
    // afterwards decayed the fresh inflow as well — a whole step of decay on
    // water that had only just arrived — and used the NEW volume as the
    // booking basis where legacy uses the old.
    //
    // Kept as a no-op rather than deleted: it is a declared stage of the
    // solver and part of its public shape.
    (void)ctx;
    (void)dt;
}

// ============================================================================
// Update link quality from upstream node — VECTORISABLE
// ============================================================================

/**
 * @brief Publish link pollutant concentration using shared legacy conduit mixing rules.
 * @param[in,out] ctx Prepared completed hydraulic/source state and writable
 * quality/scalar/native species arrays, receipts, ledgers and diagnostics used by this stage.
 * @param[in] dt Matching accepted positive finite routing duration s; most stages do not
 * validate it.
 * @details Non-conduit/DUMMY copies selected upstream/native-LID outlet quality without
 * reaction. STEADY conduit uses geometric node1 concentration, positive exponential decay and
 * signed flow mass booking; other models use old-volume explicit decay then accepted mixing
 * inflow and donor quality. Barrel seep/evaporation modifies pool/ledgers; dry nonsteady
 * conduits book/reset residual. Native reaction ownership suppresses pollutant decay. Requires
 * complete old concentrations, node/subtype geometry and ledgers; caller serializes after node
 * treatment, before final native link kinetics.
 * @note Requires matching initialized solver pollutant count, node/link/subtype arrays and
 * writable quality/ledger extents. Carrier volumes use ft3; pollutant rates use
 * concentration*ft3/s and ledger increments concentration*ft3. No implicit litre or user-mass
 * conversion, hydraulic water advance or engine clock change. Caller serializes quality phases;
 * repeat calls can double-book loads.
 */
void QualitySolver::updateLinkQuality(SimulationContext& ctx, double dt) {
    int np = n_pollutants_;
    auto& links = ctx.links;
    auto& nodes = ctx.nodes;
    auto& poll = ctx.pollutants;
    auto& mb = ctx.mass_balance;

    const bool is_steady = (ctx.options.routing_model == RoutingModel::STEADY);
    // R4: reactions-active decays links exactly in reactLegacyLinks AFTER
    // this mixing pass; the in-mix linear decay must not double-apply.
    const bool reactions_active = transport::legacyReactionsActive(ctx);
    const auto& CD = ctx.link_subtypes.conduits;

    // Batch over all links — parallelisable (each link writes to its own slot)
    // Legacy parity: src/legacy/engine/qualrout.c runs this loop serially.
#if defined(SWMM_USE_OPENMP)
// #pragma omp parallel for schedule(static)
#endif
    for (int j = 0; j < ctx.n_links(); ++j) {
        auto uj = static_cast<size_t>(j);

        int upstream = (links.flow[uj] >= 0.0) ? links.node1[uj] : links.node2[uj];
        if (upstream < 0 || upstream >= ctx.n_nodes())
            continue;
        auto un = static_cast<size_t>(upstream);

        // --- legacy findLinkQual opens with the non-conduit shortcut (see
        //     linkTakesUpstreamValue). This engine used to run the CSTR on
        //     such links, which held the last mix whenever one carried no
        //     flow.
        if (linkTakesUpstreamValue(ctx, j)) {
            for (int p = 0; p < np; ++p) {
                auto li = uj * static_cast<size_t>(np) + static_cast<size_t>(p);
                auto ni = un * static_cast<size_t>(np) + static_cast<size_t>(p);
                if (li >= links.conc.size() || ni >= nodes.conc.size())
                    break;
                links.conc[li] = lidnode::outletQuality(ctx, upstream, j, p, nodes.conc[ni]);
            }
            continue;
        }

        // Legacy's test above is on the link's TYPE and cross-section alone —
        // every CONDUIT owns a Conduit[] row. A row that does not resolve is
        // a malformed context, not a different kind of link, so the barrel
        // count and loss rates fall back to their defaults exactly as this
        // function has always done rather than diverting the link.
        const int cr = ctx.link_subtypes.conduit_row(j);
        const auto ucr = static_cast<size_t>(cr < 0 ? 0 : cr);
        const double barrels = (cr >= 0) ? static_cast<double>(CD.barrels[ucr]) : 1.0;
        const double q_seep = (cr >= 0) ? CD.seep_loss_rate[ucr] * barrels : 0.0;
        const double v_evap = (cr >= 0) ? CD.evap_loss_rate[ucr] * barrels * dt : 0.0;

        const double v_old = links.old_volume[uj];

        // Evaporation concentration factor: fEvap = 1 + vEvap/v1.
        double f_evap = 1.0;
        if (v_evap > 0.0 && v_old > ZERO_VOLUME)
            f_evap += v_evap / v_old;

        // |Link.newFlow| plus the DW volume-change correction — see
        // conduitMixingInflow, which the age, heat and MSX mirrors share.
        const double q_in = conduitMixingInflow(ctx, j, dt);

        // --- legacy zeroes a link that ends the step essentially empty and
        //     books what it held to final storage. The test is on EITHER
        //     measure of emptiness — under a litre of water, or under a
        //     millimetre of depth — and the depth half has no analogue here
        //     at all. This engine did the opposite: it handed a zero-volume
        //     link the upstream node's concentration outright, so a filling
        //     conduit published its inflow concentration from the first
        //     period instead of mixing up to it.
        const bool dry = !is_steady && linkIsDry(ctx, j);

        for (int p = 0; p < np; ++p) {
            auto li = uj * static_cast<size_t>(np) + static_cast<size_t>(p);
            auto ni = un * static_cast<size_t>(np) + static_cast<size_t>(p);
            if (li >= links.conc.size() || ni >= nodes.conc.size())
                continue;
            const auto up = static_cast<size_t>(p);

            double k = (up < poll.k_decay.size()) ? poll.k_decay[up] : 0.0;
            if (reactions_active)
                k = 0.0;

            double c_new;
            double reacted = 0.0; // KD1: decayed mass to book
            double seeped = 0.0;  // mass carried off by seepage

            if (is_steady) {
                // Steady Flow quality routing (legacy findSFLinkQual). The
                // conduit's quality is the quality of node1 — the DOWNSTREAM
                // node is never consulted, even on reverse flow — scaled by
                // fEvap and then decayed EXPONENTIALLY over the whole step.
                const int n1 = links.node1[uj];
                if (n1 < 0 || n1 >= ctx.n_nodes())
                    continue;
                const auto n1i = static_cast<size_t>(n1) * static_cast<size_t>(np) + up;
                if (n1i >= nodes.conc.size())
                    continue;

                double c1 = nodes.conc[n1i];
                seeped = q_seep * c1 * dt;
                c1 *= f_evap;
                c_new = c1;
                if (k > 0.0) {
                    c_new = std::max(0.0, c1 * std::exp(-k * dt));
                    // legacy findSFLinkQual: lossRate = (c1 - c2) * newFlow
                    reacted = (c1 - c_new) * links.flow[uj] * dt;
                }
            } else {
                double c1 = links.conc_old[li];
                seeped = q_seep * c1 * dt;
                c1 *= f_evap;

                // getReactedQual: c2 = max(0, c1 * (1 - k*dt)); the factor is
                // clamped rather than the product so the booked removal below
                // equals the actual loss even when 1 - k*dt goes negative
                // (KD1).
                double c2 = c1 * std::max(1.0 - k * dt, 0.0);
                reacted = (c1 - c2) * v_old;

                // getMixedQual(c2, v_old, wIn, q_in, dt), spelled in legacy's
                // own operand order — cIn is formed as wIn*dt/vIn, not as the
                // upstream concentration, and the mix is capped at
                // max(c2, cIn). Both are algebraic no-ops in exact
                // arithmetic and neither is one in floating point.
                if (q_in <= LEGACY_ZERO) {
                    c_new = c2;
                } else {
                    const double w_in =
                        lidnode::outletQuality(ctx, upstream, j, p, nodes.conc[ni]) * q_in;
                    const double v_in = q_in * dt;
                    const double c_in = w_in * dt / v_in;
                    const double c_max = std::max(c2, c_in);
                    c_new = (c2 * v_old + w_in * dt) / (v_old + v_in);
                    c_new = std::min(c_new, c_max);
                    c_new = std::max(c_new, 0.0);
                }

                if (dry) {
                    if (up < mb.qual_routing_final_dry.size())
                        mb.qual_routing_final_dry[up] += c_new * links.volume[uj];
                    c_new = 0.0;
                }
            }

            c_new = std::max(c_new, 0.0);
            links.conc[li] = c_new;

            // KD1: book in-link decay (legacy qualrout books both its
            // link forms; this loop is serial — see the pragma note).
            if (reacted > 0.0 && up < mb.qual_routing_reacted.size())
                mb.qual_routing_reacted[up] += reacted;
            // legacy massbal_addSeepageLoss(p, qSeep * c1), a rate the step
            // totals scale by tStep. Until now this ledger row was never
            // written and the .rpt reported zero exfiltrated mass.
            if (seeped != 0.0 && up < mb.qual_routing_seep.size())
                mb.qual_routing_seep[up] += seeped;
        }
    }
}

// ============================================================================
// Apply treatment expressions at nodes — matching legacy treatmnt_treat()
// ============================================================================

/**
 * @brief Apply compiled treatment to each marked node using its assembled inflow view.
 * @param[in,out] ctx Prepared completed hydraulic/source state and writable
 * quality/scalar/native species arrays, receipts, ledgers and diagnostics used by this stage.
 * @param[in] dt Matching accepted positive finite routing duration s; most stages do not
 * validate it.
 * @param[in] full_inflow True when assembly includes link plus external inflow; false for
 * external-only ARD seam.
 * @details Builds shared treatment cin, then delegates one-node resolver/HRT/ledger work. With
 * full_inflow, cin=mass_rate/(volume/dt); external-only path intentionally uses
 * mass_rate/volume. Thus caller's mode and denominator convention affect removal expressions. No
 * pollutants/no treatments returns; allocations/evaluation can leave earlier nodes treated.
 * Shared scratch makes calls exclusive.
 * @note Requires matching initialized solver pollutant count, node/link/subtype arrays and
 * writable quality/ledger extents. Carrier volumes use ft3; pollutant rates use
 * concentration*ft3/s and ledger increments concentration*ft3. No implicit litre or user-mass
 * conversion, hydraulic water advance or engine clock change. Caller serializes quality phases;
 * repeat calls can double-book loads.
 */
void QualitySolver::applyTreatment(SimulationContext& ctx, double dt, bool full_inflow) {
    int np = n_pollutants_;
    if (np <= 0)
        return;
    auto& treat = ctx.treatment;
    if (!treat.hasAny())
        return;

    auto& nodes = ctx.nodes;
    int nn = ctx.n_nodes();

    for (int j = 0; j < nn; ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (!treat.has_treatment[uj])
            continue;

        // 1. Inflow concentration. legacy treatmnt_setInflow (treatmnt.c:186)
        //    is `Cin[p] = wIn[p] / qIn` — a mass RATE over a flow RATE.
        //    qual_mass_in is a rate; qual_vol_in is a VOLUME (all eleven
        //    writers accumulate `q * dt`, and every other reader either
        //    divides it by dt or adds it to volumes — the "ft3/sec" in
        //    NodeData's comment is stale). Dividing the rate by the volume
        //    therefore left every Cin a factor of dt out. The apply path
        //    multiplies it straight back into the treated concentration and a
        //    removal expression naming a pollutant reads it directly: the
        //    `treatment` deck's `R = 0.01*COD*HRT^0.75` at storage node P005
        //    published 25.9 mg/L of COD against legacy's 755.
        double vol_in = nodes.qual_vol_in[uj];         // total inflow volume (ft3) this step
        double q_raw = (dt > 0.0) ? vol_in / dt : 0.0; // inflow rate (ft3/s)
        for (int p = 0; p < np; ++p) {
            auto mi = uj * static_cast<std::size_t>(np) + static_cast<std::size_t>(p);
            const bool have = mi < nodes.qual_mass_in.size();
            if (full_inflow) {
                treat.cin[static_cast<std::size_t>(p)] =
                    (q_raw > 0.0 && have) ? nodes.qual_mass_in[mi] / q_raw : 0.0;
            } else {
                // EULERIAN_ARD reaches here having assembled the EXTERNAL
                // loads only, so these accumulators are not the node's
                // influent and the ratio above is not legacy's Cin. Corrected,
                // it exceeds the node's mixed concentration often enough that
                // the MIN(cOut, node conc) clamp turns every removal into a
                // no-op — which ArdE5bTest.TreatmentAppliesAtArdNodeStores
                // correctly refuses. The real fix is for ARD to supply its own
                // Cin from its own inflow figures, the way the LARD MIX
                // already does (LagrangianSolver.hpp:430-438); until then the
                // pre-existing ratio is kept here so that path is unchanged.
                treat.cin[static_cast<std::size_t>(p)] =
                    (vol_in > 0.0 && have) ? nodes.qual_mass_in[mi] / vol_in : 0.0;
            }
        }
        applyNodeTreatment(ctx, j, dt, q_raw, treat.cin.data());
    }
}

/**
 * @brief Resolve one node's compiled co-treatment laws and book accepted pollutant removal.
 * @param[in,out] ctx Compiled treatment rows and shared cin/removal scratch, node
 * concentrations/HRT and reacted mass ledger. Requires all configured node/pollutant/subtype
 * arrays prepared.
 * @param[in] j Valid nonnegative native node index (node in declaration); absent treatment mask
 * returns.
 * @param[in] dt Accepted routing duration s; positive finite required for ordinary HRT/mass
 * bookkeeping.
 * @param[in] q_raw Caller-complete inflow ft3/s (q_in_cfs in declaration); caller selects its
 * assembly source.
 * @param[in] cin Borrowed np inflow concentrations, null supplies zeros; may equal
 * treat.cin.data().
 * @note Uses display flow/depth/area, average native volume ft3, HRT hours and dt seconds for
 * expression variables. Resolves R references recursively with cycle sentinel; invalid/cyclic
 * references become0, no explicit warning/error result. Removal clamps0..1, concentration result
 * cannot increase node; zero inflow suppresses removal-typed expression. Updates storage HRT and
 * positive net lost-mass ledger; shared scratch overwritten. No water/clock advance. Temporary
 * vectors/evaluation can throw after HRT or earlier pollutant changes; exclusive
 * context/compiled-table lifecycle, no rollback.
 */
void applyNodeTreatment(SimulationContext& ctx, int j, double dt, double q_raw, const double* cin) {
    const int np = ctx.n_pollutants();
    if (np <= 0)
        return;
    auto& treat = ctx.treatment;
    if (!treat.hasAny())
        return;
    auto& nodes = ctx.nodes;
    const auto uj = static_cast<std::size_t>(j);
    if (uj >= treat.has_treatment.size() || !treat.has_treatment[uj])
        return;
    {
        // The caller's inflow concentrations become the process-variable
        // inputs. `cin` may alias `treat.cin` (the LEGACY pass does).
        if (cin != treat.cin.data())
            for (int p = 0; p < np; ++p)
                treat.cin[static_cast<std::size_t>(p)] = cin ? cin[p] : 0.0;

        // 2. Get node state for process variables — apply UCF conversions (Gap #16)
        // Matching legacy treatmnt.c getVariableValue():
        //   pvFLOW  = Q * UCF(FLOW)   — inflow in user display units
        //   pvDEPTH = y * UCF(LENGTH) — avg depth in user display units
        //   pvAREA  = (a1+a2)/2 * UCF(LENGTH)^2 — avg surface area in user display units
        int unit_sys = static_cast<int>(ctx.options.flow_units);
        double ucf_flow = ucf::UCF(ucf::FLOW, ctx.options);
        double ucf_length = ucf::UCF(ucf::LENGTH, ctx.options);

        double hrt_hours = nodes.hrt[uj] / 3600.0;
        double q = q_raw * ucf_flow; // flow in user units
        // legacy passes vAvg = (oldVolume + newVolume)/2 as the V process
        // variable (qualrout.c:121, 141) — not the end-of-step volume.
        double v = (nodes.old_volume[uj] + nodes.volume[uj]) * 0.5; // ft3
        double d_ft = (nodes.depth[uj] + nodes.old_depth[uj]) * 0.5;
        double d = d_ft * ucf_length; // depth in user units

        // AREA: average surface area at old and new depth (Gap #16)
        double a1 = node::getSurfArea(nodes, j, nodes.old_depth[uj], &ctx.tables,
                                      ucf::getUnitSystem(unit_sys), &ctx.node_subtypes);
        double a2 = node::getSurfArea(nodes, j, nodes.depth[uj], &ctx.tables,
                                      ucf::getUnitSystem(unit_sys), &ctx.node_subtypes);
        double area = (a1 + a2) * 0.5 * ucf_length * ucf_length; // user units²

        // 3. Update HRT for storage nodes. legacy updateHRT (qualrout.c:575)
        // takes the node's OLD volume, not the average and not the new one,
        // and RESETS the residence time to zero once the store empties:
        //
        //     if (v < ZERO) hrt = 0.0;
        //     else hrt = (hrt + tStep) * v / (v + q*tStep);
        //
        // v6 skipped the update entirely on an empty store, so a stale
        // residence time survived every dry spell and kept growing — and an
        // expression like `R = 0.01*COD*HRT^0.75` reads it directly. On the
        // `treatment` deck that drove node P005's COD to 25.9 mg/L against
        // legacy's 755.
        if (nodes.type[uj] == NodeType::STORAGE) {
            const double v_old = nodes.old_volume[uj];
            if (v_old < LEGACY_ZERO) {
                nodes.hrt[uj] = 0.0;
            } else {
                const double qdt = std::abs(q_raw) * dt;
                nodes.hrt[uj] = std::max((nodes.hrt[uj] + dt) * v_old / (v_old + qdt), 0.0);
            }
            hrt_hours = nodes.hrt[uj] / 3600.0;
        }

        // 4. Initialize removal array (-1 = not computed)
        for (int p = 0; p < np; ++p)
            treat.removal[static_cast<std::size_t>(p)] = -1.0;

        // 4b. What a BARE pollutant name reads. legacy getVariableValue
        // (treatmnt.c:344-351) gives Cin[p] when THAT pollutant's treatment
        // at this node is a removal equation, and the node's current
        // concentration otherwise — so the choice depends on the OTHER
        // pollutant's equation type, not this one's. Resolved here, where the
        // per-pollutant compiled expressions are in hand, and handed to the
        // evaluator as a plain array.
        std::vector<double> cpollut(static_cast<std::size_t>(np), 0.0);
        for (int p = 0; p < np; ++p) {
            const auto up2 = static_cast<std::size_t>(p);
            const auto ci = uj * static_cast<std::size_t>(np) + up2;
            const bool p_is_removal = ci < treat.compiled.size() && treat.compiled[ci].is_removal;
            cpollut[up2] =
                p_is_removal ? treat.cin[up2] : ((ci < nodes.conc.size()) ? nodes.conc[ci] : 0.0);
        }

        // 5. Evaluate treatment for each pollutant (with co-treatment + cycle detection)
        //    Uses the legacy getRemoval() pattern:
        //    - removal[p] == -1: not computed → evaluate now
        //    - removal[p] in [0,1]: already computed → use cached
        //    - removal[p] == 10: currently computing → cycle detected, return 0
        // legacy latches a NODE-WIDE error the first time a removal turns out
        // to depend on itself (treatmnt.c:402-406: `if (R[p] > 1.0 || ErrCode)
        // { ErrCode = 1; return 0.0; }`). The latch is tested BEFORE the cached
        // value, so once one removal cycles every other pollutant at this node
        // reads 0 too — even one already computed. v6 returned 0 for the cycle
        // itself but kept evaluating the rest, so err-cyclic-treatment treated
        // pollutants legacy leaves alone.
        // (`ErrCode = 1` never equals ERR_CYCLIC_TREATMENT = 161, so legacy
        // does not report the error and does not skip the apply pass — it just
        // proceeds with the zeroed removals.)
        bool cyclic_err = false;
        auto getRemoval = [&](int p, auto& self) -> double {
            auto up = static_cast<std::size_t>(p);
            if (treat.removal[up] > 1.0 || cyclic_err) {
                cyclic_err = true;
                return 0.0; // cycle — and every later pollutant follows it
            }
            if (treat.removal[up] >= 0.0 && treat.removal[up] <= 1.0)
                return treat.removal[up]; // already computed

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

            // legacy: `c0 = Node[J].newQual[p]; if (c0 == 0.0) { R[p] = 0; }`
            // (treatmnt.c:397, 416-420) — the NODE concentration alone decides,
            // not the inflow.
            if (c_node <= 0.0) {
                treat.removal[up] = 0.0;
                return 0.0;
            }

            // Resolve every R_POLLUT dependency through getRemoval, including
            // one that is ALREADY being computed — that is the case legacy
            // relies on to break a cycle, and it is also what sets the latch
            // (treatmnt.c:384 routes R_xxx straight back into getRemoval).
            // The old guard skipped exactly that case.
            for (const auto& tok : expr.tokens) {
                if (tok.var == treatment::TreatVar::R_POLLUT && tok.pollut_ref >= 0 &&
                    tok.pollut_ref < np) {
                    self(tok.pollut_ref, self);
                }
            }

            // What R_xxx READS. legacy hands back getRemoval's return value,
            // which is 0 for a pollutant still being computed; this engine
            // passed the raw array, whose in-progress entries hold the 10.0
            // marker. `R_TN = 0.1*R_TN` therefore evaluated to 0.1*10 = 1.0 —
            // a 100 % removal — where legacy computes 0.1*0 = 0 and removes
            // nothing. err-cyclic-treatment drove TN to zero against legacy's
            // 2.96 on exactly this.
            std::vector<double> r_view(treat.removal.begin(), treat.removal.end());
            for (auto& x : r_view)
                if (x < 0.0 || x > 1.0)
                    x = 0.0;

            double result =
                treatment::evaluate(expr, c_in, dt, hrt_hours, q, v, d, treat.cin.data(),
                                    r_view.data(), np, area, cpollut.data());
            result = std::max(result, 0.0);

            if (expr.is_removal) {
                treat.removal[up] = std::min(result, 1.0);
            } else {
                result = std::min(result, c_node);
                treat.removal[up] = (c_node > 0.0) ? 1.0 - result / c_node : 0.0;
            }
            return treat.removal[up];
        };

        // legacy's driving loop (treatmnt.c:214-226) decides two cases WITHOUT
        // evaluating anything: a pollutant with no equation, and a REMOVAL-type
        // equation at a node taking no inflow, are both fixed at R = 0. Only
        // what is left goes to getRemoval.
        for (int p = 0; p < np; ++p) {
            const auto up2 = static_cast<std::size_t>(p);
            const auto ci2 = uj * static_cast<std::size_t>(np) + up2;
            const bool no_eqn = ci2 >= treat.compiled.size() || treat.compiled[ci2].tokens.empty();
            if (no_eqn) {
                treat.removal[up2] = 0.0;
            } else if (treat.compiled[ci2].is_removal && std::fabs(q_raw) <= LEGACY_ZERO) {
                treat.removal[up2] = 0.0;
            } else {
                getRemoval(p, getRemoval);
            }
        }

        // 6. Apply removals to nodal concentrations + mass balance (Gap #17)
        // Legacy mass loss formula (treatmnt.c lines 262-263):
        //   massLost = (Cin*q*tStep + oldQual*oldVol - cOut*(q*tStep + oldVol)) / tStep
        // where q is in ft3/s (internal), oldQual/oldVol are pre-quality-step values.
        double v_old = nodes.old_volume[uj];
        for (int p = 0; p < np; ++p) {
            double R = treat.removal[static_cast<std::size_t>(p)];
            if (R <= 0.0)
                continue;

            auto ci = uj * static_cast<std::size_t>(np) + static_cast<std::size_t>(p);
            if (ci >= nodes.conc.size())
                continue;

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
            double c_in_p = treat.cin[static_cast<std::size_t>(p)];
            double c_old_p = (ci < nodes.conc_old.size()) ? nodes.conc_old[ci] : c_node;
            double mass_lost = 0.0;
            if (dt > 0.0) {
                mass_lost =
                    (c_in_p * q_raw * dt + c_old_p * v_old - cOut * (q_raw * dt + v_old)) / dt;
                mass_lost = std::max(0.0, mass_lost);
            }

            // Review B6 (2026-10-15): `mass_lost` is legacy's RATE (it
            // passes it to massbal_addReactedMass, which multiplies by the
            // step). Every other writer of qual_routing_reacted books MASS,
            // so the rate goes in times dt — booked bare, "Mass Reacted"
            // held one routing step's worth per second of removal.
            if (mass_lost > 0.0 &&
                static_cast<std::size_t>(p) < ctx.mass_balance.qual_routing_reacted.size()) {
                ctx.mass_balance.qual_routing_reacted[static_cast<std::size_t>(p)] +=
                    mass_lost * dt;
            }

            nodes.conc[ci] = cOut;
        }
    }
}

} // namespace quality
} // namespace openswmm
