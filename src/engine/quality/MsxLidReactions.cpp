// SPDX-License-Identifier: Apache-2.0

/**
 * @file      MsxLidReactions.cpp
 * @brief     Multispecies reactions in LID storage and treatment layers.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "MsxLidReactions.hpp"
#include "../core/SimulationContext.hpp"
#include "../hydrology/LidNode.hpp"
#include "../transport/components/ReactionModule/MsxLegacyTransport.hpp"
#include "../transport/components/ReactionModule/ReactionIntegrator.hpp"
#include "../transport/components/ReactionModule/ReactionLegacyBinding.hpp"
#include "QualityRouting.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace openswmm::msxlid {
namespace {
/**
 * @brief Test whether a valid storage subtype has native layer cells and opted-in reaction
 * control.
 * @param[in] c Stable storage/layer/reaction configuration.
 * @param[in] row Zero-based storage subtype row; invalid returnsfalse before layer access.
 * @return True for nonempty native LID layer state and reactions.lid_enabled(control). No
 * configured/ compiled/shape/kinetic validation, allocation, state or clock mutation.
 */
bool enabledNode(const SimulationContext& c, int row) {
    const auto& s = c.node_subtypes.storages;
    return row >= 0 && row < s.count() && !s.lid_state[row].cells.empty() &&
           c.reactions.lid_enabled(s.lid[row].control);
}
/**
 * @brief Read finite native node heat temperature or reaction fallback.
 * @param[in] c Stable heat/process/reaction state.
 * @param[in] node Valid nonnegative native node index (negative is not guarded before vector
 * access).
 * @return Node degC when heat enabled, index covered and finite; otherwise default reaction
 * degC.
 * @note Pure/no allocation/physical or clock change; no physical absolute-temperature
 * validation.
 */
double temperature(const SimulationContext& c, int node) {
    return c.options.heat_transport && node < int(c.heat_state.node_temp.size()) &&
                   std::isfinite(c.heat_state.node_temp[node])
               ? c.heat_state.node_temp[node]
               : c.reactions.default_temp_c;
}
} // namespace
/**
 * @brief React one finite native LID water pool through scoped compiled species laws.
 * @param[in,out] c Prepared compiled reaction metadata/workspace identities; may append one
 * failure warning or negative-rate clamp diagnostics. No hydraulic water or engine clock
 * mutation.
 * @param[in] control Valid native LID control ID selecting laws.
 * @param[in] layer Layer selector; applies all-layer0 first then specific layer with later row
 * wins.
 * @param[in] volume Finite native water ft3; volume<=quality::ZERO_VOLUME or NaN returns
 * success/no reaction.
 * @param[in,out] mass Writable n_species native inventory tuple (concentration*ft3),
 * valid/nonaliasing with output vectors and stable through call. Not dereferenced when no
 * eligible laws/invalid-small pool.
 * @param[in] dt Positive finite reaction interval s; nonpositive/NaN returns success/no
 * reaction.
 * @param[in] temp Held reaction temperature degC (temperature in declaration), passed as TEMP
 * variable.
 * @param[out] reacted Resized/reset n_species, then signed (before-after)*volume on accepted
 * kinetics.
 * @param[out] clamped Resized/reset n_species, then positive inventory added when negative RATE
 * result is floored0. Other negative forms/nonfinite values refuse the solve.
 * @return True for skipped/no-law or accepted solve; false for integrator/value refusal,
 * retaining physical mass and zeroed result arrays. Warns once per context on failed solve;
 * transport callers may still continue water/native transfers after false.
 * @note Builds private concentration/forms/spans/workspace, accepts only finite results and
 * signed RATE intermediate, then installs clipped mass. No generic law-index/extent/input-finite
 * validation. Allocation/integrator/warning operations may throw; failure during final clamp
 * warnings may follow partial accepted mass updates. Exclusive reaction/context/output ownership
 * and prepared row extents.
 */
bool reactPool(SimulationContext& c, int control, int layer, double volume, double* mass, double dt,
               double temp, std::vector<double>& reacted, std::vector<double>& clamped) {
    auto& r = c.reactions;
    const int ns = r.n_species();
    reacted.assign(ns, 0);
    clamped.assign(ns, 0);
    if (!(volume > quality::ZERO_VOLUME) || !(dt > 0))
        return true;
    std::vector<ReactionExprForm> forms(ns, ReactionExprForm::NONE);
    std::vector<RxExprSpan> spans(ns);
    bool any = false;
    for (int select : {0, layer})
        for (const auto& law : r.lid_laws)
            if (law.control == control && law.layer == select) {
                forms[law.species] = law.form;
                spans[law.species] = law.compiled;
                any = true;
            }
    if (!any)
        return true;
    std::vector<double> values(ns), before(ns);
    for (int s = 0; s < ns; ++s)
        values[s] = before[s] = mass[s] / volume;
    double variables[int(RxHydVar::COUNT_)] = {};
    variables[int(RxHydVar::TEMP)] = temp;
    transport::RxWorkspace work;
    work.init(r);
    const auto result =
        transport::ReactionIntegrator::step(r, forms, spans, dt, values.data(), variables, work);
    bool valid = result.ok;
    for (int s = 0; s < ns; ++s)
        valid = valid && std::isfinite(values[s]) &&
                (values[s] >= 0 || forms[s] == ReactionExprForm::RATE);
    if (!valid) {
        if (!r.warned_react_failure) {
            r.warned_react_failure = true;
            c.warnings.push_back(
                "LID reaction solve failed; its unreacted inventory was retained.");
        }
        return false;
    }
    for (int s = 0; s < ns; ++s) {
        reacted[s] = (before[s] - values[s]) * volume;
        clamped[s] = std::max(0., -values[s]) * volume;
        mass[s] = std::max(0., values[s]) * volume;
        if (clamped[s] > 0)
            transport::noteNegativeClamp(c, "a LID layer");
    }
    return true;
}
/**
 * @brief Ensure opted-in native LID-node layer species stores exist at current seed phase.
 * @param[in,out] c Prepared storage nodes/layer geometry/optional Richards water, compiled row
 * count and native node concentrations; allocates lid.node_mass/outlet/mobile history by storage
 * row.
 * @note No control opt-ins returns. Correctly sized existing layer mass preserved; mismatched
 * pools seed concentration*held layer water ft3, records native mobile volume. Does not validate
 * BULK profile, book initial mass ledger or reset evolved correctly shaped stores. Parallel
 * layer/native arrays must cover prepared counts. Allocation can leave partial shapes/seeds;
 * exclusive lifecycle access, no hydraulic or engine clock advance.
 */
void initialize(SimulationContext& c) {
    auto& r = c.reactions;
    if (r.lid_controls.empty())
        return;
    const auto& s = c.node_subtypes.storages;
    const int ns = r.n_species();
    r.lid.node_mass.resize(s.count());
    r.lid.node_outlet.resize(s.count());
    r.lid.node_mobile_old.resize(s.count(), 0.);
    for (int row = 0; row < s.count(); ++row)
        if (enabledNode(c, row)) {
            const auto& state = s.lid_state[row];
            auto& mass = r.lid.node_mass[row];
            const int node = s.node_idx[row];
            if (mass.size() == state.cells.size() * ns)
                continue;
            r.lid.node_mobile_old[row] = c.nodes.volume[node];
            mass.resize(state.cells.size() * ns);
            for (std::size_t i = 0; i < state.cells.size(); ++i) {
                const double water = state.richards && i < state.richards_water.size()
                                         ? state.richards_water[i]
                                         : state.cells[i].theta * state.cells[i].geometric_volume;
                for (int sp = 0; sp < ns; ++sp)
                    mass[i * ns + sp] = water * r.msx_node_conc[node * ns + sp];
            }
        }
}
/**
 * @brief Sum one node's held layer inventory for a reaction species.
 * @param[in] c Stable native storage mapping and lid.node_mass layout.
 * @param[in] node Valid native node index supplied to storage_row mapping.
 * @param[in] species Zero-based species below n_species; invalid returns0.
 * @return Sum all stored layer entries at species stride, native concentration*ft3;0 for absent
 * storage mass. Does not require current opt-in, clear receipts or change clock. Read-only/no
 * allocation.
 */
double heldMass(const SimulationContext& c, int node, int species) {
    const int row = c.node_subtypes.storage_row(node), ns = c.reactions.n_species();
    if (row < 0 || row >= int(c.reactions.lid.node_mass.size()) || species < 0 || species >= ns)
        return 0;
    double total = 0;
    const auto& mass = c.reactions.lid.node_mass[row];
    for (std::size_t i = species; i < mass.size(); i += ns)
        total += mass[i];
    return total;
}
/**
 * @brief Read the retained native LID-node mobile water history with caller fallback.
 * @param[in] c Stable storage mapping/opt-in and prepared node_mobile_old.
 * @param[in] node Valid native node ID for mapping.
 * @param[in] fallback Caller native ft3 value when no enabled/retained owner.
 * @return Stored old mobile ft3 or fallback, without validating finite/positive value. No
 * mutation/allocation.
 */
double mobileWater(const SimulationContext& c, int node, double fallback) {
    const int row = c.node_subtypes.storage_row(node);
    return enabledNode(c, row) && row < int(c.reactions.lid.node_mobile_old.size())
               ? c.reactions.lid.node_mobile_old[row]
               : fallback;
}
/**
 * @brief Replay accepted native LID water transfers into layer/mobile species inventories before
 * routing.
 * @param[in,out] c Prepared completed hydraulic layer receipts/ports, prior layer/native
 * concentration, external species mass-rate arrays and native MSX ledgers. Uses initialize() and
 * arms routing ledger.
 * @param[in] dt Positive finite accepted hydraulic interval s; nonpositive/NaN/no opt-ins
 * returns.
 * @note Requires consistent native node/link/layer/species shapes and one owner for completed
 * transfer receipt. Reconstructs old layer/mobile ft3, partitions external incoming mass into
 * capture, internal layer/mobile transfers, massless evaporation and seepage. External capture
 * removes its rate share and books MSX inflow; ports with negative volume export layer donor
 * concentration by link, retain NaN sentinel on unowned links. Publishes remaining mobile
 * concentration/history; Richards surface mass is paired into mobile first. No hydraulic
 * water/clock advance, receipt arrays not cleared here. Changes ledgers/stores progressively,
 * can allocate/throw without rollback; exactly-once accepted receipt replay and exclusive
 * context/native workspace required. Missing old-water shape skips node.
 */
void prepareNodes(SimulationContext& c, double dt) {
    auto& r = c.reactions;
    if (r.lid_controls.empty() || !(dt > 0))
        return;
    initialize(c);
    transport::armMsxRoutingLedger(c);
    const auto& st = c.node_subtypes.storages;
    const int ns = r.n_species();
    r.msx_ext_mass_in.resize(std::size_t(c.n_nodes()) * ns, 0);
    for (int row = 0; row < st.count(); ++row)
        if (enabledNode(c, row)) {
            const auto& state = st.lid_state[row];
            if (state.quality_old_water.size() != state.cells.size())
                continue;
            const int node = st.node_idx[row], base = node * ns;
            auto water = state.quality_old_water;
            double mobile = state.quality_old_mobile;
            auto& mass = r.lid.node_mass[row];
            std::vector<double> mobile_mass(ns), external(ns);
            double capture = 0;
            for (const auto& t : state.quality_transfers)
                if (t.from == -2)
                    capture += t.volume;
            const double incoming =
                c.nodes.qual_vol_in[node] + (c.n_pollutants() > 0 ? capture : 0.);
            for (int sp = 0; sp < ns; ++sp) {
                mobile_mass[sp] = r.msx_node_conc[base + sp] * mobile;
                if (state.richards) {
                    mobile_mass[sp] += mass[sp];
                    mass[sp] = 0;
                }
                external[sp] =
                    incoming > 0 ? std::max(0., r.msx_ext_mass_in[base + sp] * dt / incoming) : 0.;
            }
            for (const auto& t : state.quality_transfers) {
                if (t.to == -3) {
                    if (t.from < 0)
                        mobile = std::max(0., mobile - t.volume);
                    else
                        water[t.from] = std::max(0., water[t.from] - t.volume);
                    continue;
                }
                const double available = t.from == -2 ? t.volume
                                         : t.from < 0 ? mobile
                                                      : water[t.from];
                for (int sp = 0; sp < ns; ++sp) {
                    const double resident = t.from == -2 ? external[sp] * t.volume
                                            : t.from < 0 ? mobile_mass[sp]
                                                         : mass[t.from * ns + sp];
                    const double sent = available > 0 ? std::max(0., resident) *
                                                            std::clamp(t.volume / available, 0., 1.)
                                                      : 0.;
                    if (t.from == -2) {
                        r.msx_ext_mass_in[base + sp] =
                            std::max(0., r.msx_ext_mass_in[base + sp] - sent / dt);
                        r.msx_ledger.inflow[sp] += sent;
                    } else if (t.from < 0)
                        mobile_mass[sp] -= sent;
                    else
                        mass[t.from * ns + sp] -= sent;
                    if (t.to == -4)
                        r.msx_ledger.seepage[sp] += sent;
                    else if (t.to < 0)
                        mobile_mass[sp] += sent;
                    else
                        mass[t.to * ns + sp] += sent;
                }
                if (t.from == -2) {
                    if (c.n_pollutants() == 0)
                        c.nodes.qual_vol_in[node] =
                            std::max(0., c.nodes.qual_vol_in[node] - t.volume);
                } else if (t.from < 0)
                    mobile = std::max(0., mobile - t.volume);
                else
                    water[t.from] = std::max(0., water[t.from] - t.volume);
                if (t.to != -4) {
                    if (t.to < 0)
                        mobile += t.volume;
                    else
                        water[t.to] += t.volume;
                }
            }
            auto& out = r.lid.node_outlet[row];
            out.assign(std::size_t(c.n_links()) * ns, std::numeric_limits<double>::quiet_NaN());
            for (const auto& p : state.quality_ports)
                if (p.volume < 0) {
                    const double available = water[p.cell];
                    for (int sp = 0; sp < ns; ++sp) {
                        auto& donor = mass[p.cell * ns + sp];
                        const double sent =
                            available > 0
                                ? std::max(0., donor) * std::clamp(-p.volume / available, 0., 1.)
                                : 0.;
                        donor -= sent;
                        out[p.link * ns + sp] = sent / (-p.volume);
                    }
                    water[p.cell] = std::max(0., available + p.volume);
                }
            r.lid.node_mobile_old[row] = mobile;
            for (int sp = 0; sp < ns; ++sp) {
                if (mobile > 1.e-12)
                    r.msx_node_conc[base + sp] = mobile_mass[sp] / mobile;
                else {
                    mass[sp] += mobile_mass[sp];
                    r.msx_node_conc[base + sp] = 0;
                }
            }
        }
}
/**
 * @brief Read a prepared node-layer outlet concentration for one link/species.
 * @param[in] c Stable native storage mapping and node_outlet arrays.
 * @param[in] node Valid native node ID.
 * @param[in] link Valid nonnegative native link index.
 * @param[in] species Valid zero-based reaction row below n_species (not individually bounds
 * checked).
 * @param[in] fallback Native concentration when no covered finite exported port value.
 * @return Finite prepared outlet concentration or fallback. No lookup refresh, receipt
 * consumption, allocation, physical/clock mutation; caller retains matching completed hydraulic
 * port phase.
 */
double outlet(const SimulationContext& c, int node, int link, int species, double fallback) {
    const int row = c.node_subtypes.storage_row(node), ns = c.reactions.n_species();
    if (row < 0 || row >= int(c.reactions.lid.node_outlet.size()))
        return fallback;
    const auto& out = c.reactions.lid.node_outlet[row];
    const auto k = std::size_t(link) * ns + species;
    return k < out.size() && std::isfinite(out[k]) ? out[k] : fallback;
}
/**
 * @brief Capture an accepted incoming link native mass-rate share into the first matching LID
 * port.
 * @param[in,out] c Prepared enabled native LID-node layers, positive incoming quality ports and
 * matching writable layer species inventories.
 * @param[in] node Valid native storage node ID.
 * @param[in] link Valid native link identity matched against prepared ports.
 * @param[in] water Positive finite total matching incoming hydraulic water ft3, denominator for
 * port share.
 * @param[in] species Valid nonnegative reaction row below n_species, unchecked.
 * @param[in] rate Native concentration*ft3/s incoming mass rate (named mass in declaration),
 * negative floored0; no generic finite input check.
 * @param[in] dt Positive finite accepted interval s, used to convert captured rate into stored
 * inventory.
 * @return Accepted native mass rate, not interval mass, or0 for disabled/nonpositive water/no
 * positive matching port. Uses clamp(port.volume/water,0,1) and stops at first match.
 * @note Adds rate*dt to existing layer mass, no hydraulic water/global ledger/clock change or
 * receipt clear. No allocation/rollback; exclusive native owner and matching exactly-once port
 * chronology.
 */
double receive(SimulationContext& c, int node, int link, double water, int species, double rate,
               double dt) {
    const int row = c.node_subtypes.storage_row(node), ns = c.reactions.n_species();
    if (!enabledNode(c, row) || !(water > 0))
        return 0.;
    for (const auto& p : c.node_subtypes.storages.lid_state[row].quality_ports)
        if (p.link == link && p.volume > 0) {
            const double sent = std::max(0., rate) * std::clamp(p.volume / water, 0., 1.);
            c.reactions.lid.node_mass[row][p.cell * ns + species] += sent * dt;
            return sent;
        }
    return 0.;
}
/**
 * @brief React native LID-node held layer pools and weighted mobile water portions once per
 * interval.
 * @param[in,out] c Prepared opted-in native storage geometry/optional Richards moisture,
 * layer/node species inventories, compiled reaction laws and armed MSX reacted/clamped ledgers.
 * @param[in] dt Positive finite reaction duration s; reactPool handles nonpositive/no-water
 * skips.
 * @note No control opt-ins returns. Reacts each held cell with current volume/temperature;
 * partitions positive native mobile water among eligible pore deficits, only surface cells for
 * Richards, then recombines species deltas at original concentration. Books accepted signed
 * reacted/clamped outputs; failed reactPool retains unreacted mass and warning, other pools can
 * proceed. No hydraulic water, engine clock update or duplicate generic native reaction step
 * should occur for the same ownership. Allocates scratch/mutates pools+ledgers progressively,
 * may throw without rollback; exclusive prepared context/row/layer extents and once-only
 * accepted reaction phase required.
 */
void reactNodes(SimulationContext& c, double dt) {
    auto& r = c.reactions;
    if (r.lid_controls.empty())
        return;
    const int ns = r.n_species();
    const auto& st = c.node_subtypes.storages;
    std::vector<double> reacted, clamped;
    for (int row = 0; row < st.count(); ++row)
        if (enabledNode(c, row)) {
            const auto& state = st.lid_state[row];
            const int node = st.node_idx[row], control = st.lid[row].control;
            const double temp = temperature(c, node);
            auto book = [&] {
                for (int sp = 0; sp < ns; ++sp) {
                    r.msx_ledger.reacted[sp] += reacted[sp];
                    r.msx_ledger.clamped[sp] += clamped[sp];
                }
            };
            for (std::size_t i = 0; i < state.cells.size(); ++i) {
                const double v = state.richards && i < state.richards_water.size()
                                     ? state.richards_water[i]
                                     : state.cells[i].theta * state.cells[i].geometric_volume;
                reactPool(c, control, state.cells[i].layer, v, r.lid.node_mass[row].data() + i * ns,
                          dt, temp, reacted, clamped);
                book();
            }
            const double v = c.nodes.volume[node];
            if (!(v > quality::ZERO_VOLUME))
                continue;
            std::vector<double> shares(state.cells.size(), 0.);
            double total = 0;
            for (std::size_t i = 0; i < state.cells.size(); ++i) {
                const auto& cell = state.cells[i];
                if (state.richards && cell.kind != LidNodeLayerKind::Surface)
                    continue;
                shares[i] =
                    cell.geometric_volume * std::max(0., cell.porosity - cell.theta) *
                    std::clamp((c.nodes.depth[node] - cell.bottom) / (cell.top - cell.bottom), 0.,
                               1.);
                total += shares[i];
            }
            if (!(total > 0))
                continue;
            std::vector<double> original(ns), delta(ns, 0.), mass(ns);
            for (int sp = 0; sp < ns; ++sp)
                original[sp] = r.msx_node_conc[node * ns + sp];
            for (std::size_t i = 0; i < shares.size(); ++i)
                if (shares[i] > 0) {
                    const double volume = v * shares[i] / total;
                    for (int sp = 0; sp < ns; ++sp)
                        mass[sp] = original[sp] * volume;
                    reactPool(c, control, state.cells[i].layer, volume, mass.data(), dt, temp,
                              reacted, clamped);
                    book();
                    for (int sp = 0; sp < ns; ++sp)
                        delta[sp] += mass[sp] - original[sp] * volume;
                }
            for (int sp = 0; sp < ns; ++sp)
                r.msx_node_conc[node * ns + sp] = std::max(0., original[sp] + delta[sp] / v);
        }
}
/**
 * @brief Test the prepared conventional-LID reactive subcatchment mask.
 * @param[in] c Stable reaction lid state.
 * @param[in] sc Zero-based subcatchment identity; invalid returnsfalse.
 * @return True for covered nonzero mask, independent of current wetness or reaction result.
 * Read-only, allocation-free; does not activate, qualify or advance owner.
 */
bool reactedSubcatchment(const SimulationContext& c, int sc) {
    return sc >= 0 && sc < int(c.reactions.lid.subcatchments.size()) &&
           c.reactions.lid.subcatchments[sc] != 0;
}
} // namespace openswmm::msxlid
