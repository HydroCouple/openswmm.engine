// SPDX-License-Identifier: Apache-2.0
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
bool enabledNode(const SimulationContext &c, int row) {
    const auto &s = c.node_subtypes.storages;
    return row >= 0 && row < s.count() && !s.lid_state[row].cells.empty() &&
           c.reactions.lid_enabled(s.lid[row].control);
}
double temperature(const SimulationContext &c, int node) {
    return c.options.heat_transport && node < int(c.heat_state.node_temp.size()) &&
                   std::isfinite(c.heat_state.node_temp[node])
               ? c.heat_state.node_temp[node]
               : c.reactions.default_temp_c;
}
} // namespace
bool reactPool(SimulationContext &c, int control, int layer, double volume, double *mass, double dt,
               double temp, std::vector<double> &reacted, std::vector<double> &clamped) {
    auto &r = c.reactions;
    const int ns = r.n_species();
    reacted.assign(ns, 0);
    clamped.assign(ns, 0);
    if (!(volume > quality::ZERO_VOLUME) || !(dt > 0))
        return true;
    std::vector<ReactionExprForm> forms(ns, ReactionExprForm::NONE);
    std::vector<RxExprSpan> spans(ns);
    bool any = false;
    for (int select : {0, layer})
        for (const auto &law : r.lid_laws)
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
        valid = valid && std::isfinite(values[s]) && (values[s] >= 0 || forms[s] == ReactionExprForm::RATE);
    if (!valid) {
        if (!r.warned_react_failure) {
            r.warned_react_failure = true;
            c.warnings.push_back("LID reaction solve failed; its unreacted inventory was retained.");
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
void initialize(SimulationContext &c) {
    auto &r = c.reactions;
    if (r.lid_controls.empty())
        return;
    const auto &s = c.node_subtypes.storages;
    const int ns = r.n_species();
    r.lid.node_mass.resize(s.count());
    r.lid.node_outlet.resize(s.count());
    r.lid.node_mobile_old.resize(s.count(), 0.);
    for (int row = 0; row < s.count(); ++row)
        if (enabledNode(c, row)) {
            const auto &state = s.lid_state[row];
            auto &mass = r.lid.node_mass[row];
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
double heldMass(const SimulationContext &c, int node, int species) {
    const int row = c.node_subtypes.storage_row(node), ns = c.reactions.n_species();
    if (row < 0 || row >= int(c.reactions.lid.node_mass.size()) || species < 0 || species >= ns)
        return 0;
    double total = 0;
    const auto &mass = c.reactions.lid.node_mass[row];
    for (std::size_t i = species; i < mass.size(); i += ns)
        total += mass[i];
    return total;
}
double mobileWater(const SimulationContext &c, int node, double fallback) {
    const int row = c.node_subtypes.storage_row(node);
    return enabledNode(c, row) && row < int(c.reactions.lid.node_mobile_old.size())
               ? c.reactions.lid.node_mobile_old[row]
               : fallback;
}
void prepareNodes(SimulationContext &c, double dt) {
    auto &r = c.reactions;
    if (r.lid_controls.empty() || !(dt > 0))
        return;
    initialize(c);
    transport::armMsxRoutingLedger(c);
    const auto &st = c.node_subtypes.storages;
    const int ns = r.n_species();
    r.msx_ext_mass_in.resize(std::size_t(c.n_nodes()) * ns, 0);
    for (int row = 0; row < st.count(); ++row)
        if (enabledNode(c, row)) {
            const auto &state = st.lid_state[row];
            if (state.quality_old_water.size() != state.cells.size())
                continue;
            const int node = st.node_idx[row], base = node * ns;
            auto water = state.quality_old_water;
            double mobile = state.quality_old_mobile;
            auto &mass = r.lid.node_mass[row];
            std::vector<double> mobile_mass(ns), external(ns);
            double capture = 0;
            for (const auto &t : state.quality_transfers)
                if (t.from == -2)
                    capture += t.volume;
            const double incoming = c.nodes.qual_vol_in[node] + (c.n_pollutants() > 0 ? capture : 0.);
            for (int sp = 0; sp < ns; ++sp) {
                mobile_mass[sp] = r.msx_node_conc[base + sp] * mobile;
                if (state.richards) {
                    mobile_mass[sp] += mass[sp];
                    mass[sp] = 0;
                }
                external[sp] = incoming > 0 ? std::max(0., r.msx_ext_mass_in[base + sp] * dt / incoming) : 0.;
            }
            for (const auto &t : state.quality_transfers) {
                if (t.to == -3) {
                    if (t.from < 0)
                        mobile = std::max(0., mobile - t.volume);
                    else
                        water[t.from] = std::max(0., water[t.from] - t.volume);
                    continue;
                }
                const double available = t.from == -2 ? t.volume : t.from < 0 ? mobile : water[t.from];
                for (int sp = 0; sp < ns; ++sp) {
                    const double resident = t.from == -2 ? external[sp] * t.volume
                                            : t.from < 0 ? mobile_mass[sp]
                                                         : mass[t.from * ns + sp];
                    const double sent =
                        available > 0 ? std::max(0., resident) * std::clamp(t.volume / available, 0., 1.)
                                      : 0.;
                    if (t.from == -2) {
                        r.msx_ext_mass_in[base + sp] = std::max(0., r.msx_ext_mass_in[base + sp] - sent / dt);
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
                        c.nodes.qual_vol_in[node] = std::max(0., c.nodes.qual_vol_in[node] - t.volume);
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
            auto &out = r.lid.node_outlet[row];
            out.assign(std::size_t(c.n_links()) * ns, std::numeric_limits<double>::quiet_NaN());
            for (const auto &p : state.quality_ports)
                if (p.volume < 0) {
                    const double available = water[p.cell];
                    for (int sp = 0; sp < ns; ++sp) {
                        auto &donor = mass[p.cell * ns + sp];
                        const double sent =
                            available > 0 ? std::max(0., donor) * std::clamp(-p.volume / available, 0., 1.)
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
double outlet(const SimulationContext &c, int node, int link, int species, double fallback) {
    const int row = c.node_subtypes.storage_row(node), ns = c.reactions.n_species();
    if (row < 0 || row >= int(c.reactions.lid.node_outlet.size()))
        return fallback;
    const auto &out = c.reactions.lid.node_outlet[row];
    const auto k = std::size_t(link) * ns + species;
    return k < out.size() && std::isfinite(out[k]) ? out[k] : fallback;
}
double receive(SimulationContext &c, int node, int link, double water, int species, double rate, double dt) {
    const int row = c.node_subtypes.storage_row(node), ns = c.reactions.n_species();
    if (!enabledNode(c, row) || !(water > 0))
        return 0.;
    for (const auto &p : c.node_subtypes.storages.lid_state[row].quality_ports)
        if (p.link == link && p.volume > 0) {
            const double sent = std::max(0., rate) * std::clamp(p.volume / water, 0., 1.);
            c.reactions.lid.node_mass[row][p.cell * ns + species] += sent * dt;
            return sent;
        }
    return 0.;
}
void reactNodes(SimulationContext &c, double dt) {
    auto &r = c.reactions;
    if (r.lid_controls.empty())
        return;
    const int ns = r.n_species();
    const auto &st = c.node_subtypes.storages;
    std::vector<double> reacted, clamped;
    for (int row = 0; row < st.count(); ++row)
        if (enabledNode(c, row)) {
            const auto &state = st.lid_state[row];
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
                reactPool(c, control, state.cells[i].layer, v, r.lid.node_mass[row].data() + i * ns, dt, temp,
                          reacted, clamped);
                book();
            }
            const double v = c.nodes.volume[node];
            if (!(v > quality::ZERO_VOLUME))
                continue;
            std::vector<double> shares(state.cells.size(), 0.);
            double total = 0;
            for (std::size_t i = 0; i < state.cells.size(); ++i) {
                const auto &cell = state.cells[i];
                if (state.richards && cell.kind != LidNodeLayerKind::Surface)
                    continue;
                shares[i] =
                    cell.geometric_volume * std::max(0., cell.porosity - cell.theta) *
                    std::clamp((c.nodes.depth[node] - cell.bottom) / (cell.top - cell.bottom), 0., 1.);
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
                    reactPool(c, control, state.cells[i].layer, volume, mass.data(), dt, temp, reacted,
                              clamped);
                    book();
                    for (int sp = 0; sp < ns; ++sp)
                        delta[sp] += mass[sp] - original[sp] * volume;
                }
            for (int sp = 0; sp < ns; ++sp)
                r.msx_node_conc[node * ns + sp] = std::max(0., original[sp] + delta[sp] / v);
        }
}
bool reactedSubcatchment(const SimulationContext &c, int sc) {
    return sc >= 0 && sc < int(c.reactions.lid.subcatchments.size()) &&
           c.reactions.lid.subcatchments[sc] != 0;
}
} // namespace openswmm::msxlid
