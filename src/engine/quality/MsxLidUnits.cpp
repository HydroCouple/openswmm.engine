// SPDX-License-Identifier: Apache-2.0

/**
 * @file      MsxLidUnits.cpp
 * @brief     Unit conversions for multispecies LID reaction quantities.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "../core/SimulationContext.hpp"
#include "../core/UnitConversion.hpp"
#include "../hydrology/LID.hpp"
#include "../transport/components/LidLayerCommon.hpp"
#include "MsxLidReactions.hpp"
#include <algorithm>
#include <cmath>

namespace openswmm::msxlid {
namespace {
constexpr int layers = 4;
/**
 * @brief Compute conventional LID active-layer water volumes for one unit.
 * @param[in] g Stable prepared group layer depths/moisture/area/type and completed storage
 * metadata.
 * @param[in] u Valid zero-based unit index covered by every accessed group vector.
 * @param[out] water Exactly4 values in native ft3, order surface/pavement/soil/storage.
 * Multiplies shared layer depths by area ft2, zeroes absent donors and uses completed swale
 * storage for surface.
 * @note No shape/finite validation, allocation, water/clock mutation; exclusive output tuple.
 */
void volumes(const lid::LIDGroupSoA& g, int u, double (&water)[layers]) {
    transport::layerVolumes(g, u, water);
    for (auto& value : water)
        value *= g.area[u];
    const auto d = transport::donorsFor(g.type, g.soil_thick[u] > 0);
    const int donors[] = {d.surface, d.pavement, d.soil, d.storage};
    for (int k = 0; k < layers; ++k)
        if (donors[k] == transport::kAbsent)
            water[k] = 0;
    if (g.type == lid::LIDType::VEG_SWALE)
        water[0] = lid::completedUnitStorage(g, u);
}
} // namespace
/**
 * @brief Initialize conventional reactive LID native species pools and full-coverage ownership
 * mask.
 * @param[in,out] c Prepared configured reaction species/global concentrations/surface mass
 * conversions and land area units; receives per-unit layer native stores, outlet/surface
 * receipts and errors.
 * @param[in] solver Stable resolved conventional LID groups/unit areas/type/control/subcatch
 * identity.
 * @note unsupported WALL species, covered reactive barrel, partial/mixed opt-in coverage append
 * c.errors rather than rolling back state or throwing a profile exception.
 * @note No controls returns. Rebuilds offsets and zeroes all conventional LID stores, then seeds
 * opted units concentration*water ft3 and books initial user mass with28.317*mcf. Requires
 * BULK-only system, full subcatchment coverage and every unit opted; tolerates .1%coverage
 * mismatch floor. No water or clock advance. Shape/index/finite preconditions and signed
 * products must qualify; allocation may throw after reset/earlier mass bookings. Exclusive
 * initial lifecycle, not safe to repeat on evolved or already-ledgered owner without
 * corresponding reset.
 */
void initializeUnits(SimulationContext& c, const lid::LIDSolver& solver) {
    auto& r = c.reactions;
    if (r.lid_controls.empty())
        return;
    auto& s = r.lid;
    const int ns = r.n_species(), nsc = c.n_subcatches();
    s.group_offsets = transport::buildOffsets(solver);
    const int units = s.group_offsets.back();
    s.mass.assign(std::size_t(units) * layers * ns, 0);
    s.old_water.assign(std::size_t(units) * layers, 0);
    s.surface_conc.assign(std::size_t(units) * ns, 0);
    s.drain_conc.assign(std::size_t(units) * ns, 0);
    s.surface_export.assign(std::size_t(nsc) * ns, 0);
    s.surface_carry.assign(std::size_t(nsc) * ns, 0);
    s.subcatchments.assign(nsc, 0);
    std::vector<double> area(nsc, 0.);
    std::vector<bool> untracked(nsc, false);
    for (int t = 0; t < solver.numGroups(); ++t) {
        const auto& g = solver.group(t);
        for (int u = 0; u < g.count; ++u) {
            const int sc = g.subcatch_idx[u];
            if (sc < 0 || sc >= nsc)
                continue;
            if (!r.lid_enabled(g.control_idx[u])) {
                untracked[sc] = true;
                continue;
            }
            if (std::any_of(r.species_is_wall.begin(), r.species_is_wall.end(),
                            [](auto value) { return value != 0; }))
                c.errors.push_back(
                    "Reactive conventional LIDs require a BULK-only reaction system.");
            s.subcatchments[sc] = 1;
            area[sc] += g.area[u];
            if (g.type == lid::LIDType::RAIN_BARREL && g.stor_covered[u])
                c.errors.push_back(
                    "Reactive rain barrels must be uncovered; covered-barrel rain returns "
                    "need a separate soil receipt.");
            const int flat = s.group_offsets[t] + u;
            double v[layers];
            volumes(g, u, v);
            for (int k = 0; k < layers; ++k) {
                s.old_water[flat * layers + k] = v[k];
                for (int sp = 0; sp < ns; ++sp) {
                    const double mass = r.init_global[sp] * v[k];
                    s.mass[(flat * layers + k) * ns + sp] = mass;
                    r.surface.led_init_buildup[sp] += mass * 28.317 * r.surface.mcf[sp];
                }
            }
        }
    }
    for (int sc = 0; sc < nsc; ++sc)
        if (s.subcatchments[sc]) {
            const double full = c.subcatches.area[sc] / ucf::UCF(ucf::LANDAREA, c.options);
            // Match the hydraulic full-coverage snap (0.1%); the legacy acre
            // factor itself differs slightly from the exact 43560-ft2 footprint.
            if (untracked[sc] || std::abs(area[sc] - full) > .001 * std::max(1., full))
                c.errors.push_back(
                    "Reactive conventional LIDs require full subcatchment coverage and every unit to opt "
                    "into [REACTION_LIDS]. Partial capture needs a separate donor-mass adapter.");
        }
}
/**
 * @brief Route/react native species through one accepted conventional LID hydraulic interval.
 * @param[in,out] c Prepared reactive conventional-LID offsets/pools, subcatch
 * rainfall/runon/masks, compiled laws/thermal layer view and user-mass surface ledgers; receives
 * surface/drain concentrations, exports, updated layer inventory/old-water and
 * deposition/infiltration/reaction receipts.
 * @param[in] solver Stable matching groups with accepted layer inflow/evaporation/surface/drain
 * rates, infiltration depth and completed water storage, same unit order as initialized offsets.
 * @param[in] dt Positive finite accepted interval s; no controls/offsets/nonpositive/NaN
 * returns.
 * @note Rebuilds active footprint, clears this-step exports, distributes rainfall/runon into
 * first present layer and processes top-to-bottom mixed donor transfers. Reactions run only for
 * old/current layer water>.0353147ft3 using held finite layer temperature or default; false
 * kinetics retains unreacted mass but transport continues. Internal/outlet/infiltration exports
 * share finite mixed pool; evaporation removes water only. Native pools use concentration*ft3,
 * user mass ledger conversion 28.317L/ft3*mcf. Does not advance hydraulic water/engine clock or
 * publish final runoff concentration. Allocates and updates progressively; no generic
 * shape/finite/profile validation or rollback. Requires exactly-once completed hydraulic receipt
 * ownership, exclusive native/context state and compiled laws.
 */
void routeUnits(SimulationContext& c, const lid::LIDSolver& solver, double dt) {
    auto& r = c.reactions;
    auto& s = r.lid;
    if (r.lid_controls.empty() || s.group_offsets.empty() || !(dt > 0))
        return;
    const int ns = r.n_species();
    std::fill(s.surface_export.begin(), s.surface_export.end(), 0.);
    std::vector<double> footprint(c.n_subcatches(), 0.);
    for (int t = 0; t < solver.numGroups(); ++t) {
        const auto& g = solver.group(t);
        for (int u = 0; u < g.count; ++u)
            if (r.lid_enabled(g.control_idx[u]) && g.subcatch_idx[u] >= 0)
                footprint[g.subcatch_idx[u]] += g.area[u];
    }
    for (int t = 0; t < solver.numGroups(); ++t) {
        const auto& g = solver.group(t);
        for (int u = 0; u < g.count; ++u)
            if (r.lid_enabled(g.control_idx[u])) {
                const int sc = g.subcatch_idx[u], flat = s.group_offsets[t] + u,
                          control = g.control_idx[u];
                if (!reactedSubcatchment(c, sc))
                    continue;
                const double area = g.area[u],
                             rain = std::max(0., c.subcatches.rainfall[sc]) * area * dt;
                const double fraction = footprint[sc] > 0 ? area / footprint[sc] : 0.;
                const auto d = transport::donorsFor(g.type, g.soil_thick[u] > 0);
                const int donor[] = {d.surface, d.pavement, d.soil, d.storage};
                const double rates[] = {g.in_surf[u], g.in_pave[u], g.in_soil[u], g.in_stor[u]};
                const double evap[] = {g.evap_surf[u], g.evap_pave[u], g.evap_soil[u],
                                       g.evap_stor[u]};
                double water[layers];
                volumes(g, u, water);
                double incoming[layers] = {}, internal[layers] = {};
                std::vector<double> arrivals(std::size_t(layers) * ns, 0.);
                int first = -1, last = -1;
                for (int k = 0; k < layers; ++k)
                    if (donor[k] != transport::kAbsent) {
                        if (first < 0)
                            first = k;
                        last = k;
                        incoming[k] = std::max(0., rates[k]) * area * dt;
                        if (donor[k] >= 0)
                            internal[donor[k]] += incoming[k];
                    }
                if (first < 0)
                    continue;
                // A barrel's external input enters STORAGE, not the absent surface.
                incoming[first] = std::max(0., g.inflow[u]) * area * dt;
                for (int sp = 0; sp < ns; ++sp) {
                    const double cr = sp < int(r.rain_conc.size()) ? r.rain_conc[sp] : 0.;
                    const auto index = std::size_t(sc) * ns + sp;
                    const double runon =
                        index < r.surface.runon_qual_rate.size()
                            ? r.surface.runon_qual_rate[index] * dt * fraction / 28.317
                            : 0.;
                    arrivals[first * ns + sp] = cr * rain + runon;
                    r.surface.led_wet_deposition[sp] += cr * rain * 28.317 * r.surface.mcf[sp];
                }
                const double surface = std::max(0., g.surface_runoff[u]) * area * dt;
                const double drain = std::max(0., g.drain_flow[u]) * area * dt;
                const double infiltrated = std::max(0., g.infil_loss[u]) * area;
                const int surf_layer =
                    donor[transport::kSurf] == transport::kAbsent ? first : transport::kSurf;
                const int drain_layer =
                    donor[transport::kStor] == transport::kAbsent ? last : transport::kStor;
                for (int k = 0; k < layers; ++k)
                    if (donor[k] != transport::kAbsent) {
                        const auto row = std::size_t(flat * layers + k) * ns;
                        double* mass = s.mass.data() + row;
                        for (int sp = 0; sp < ns; ++sp)
                            mass[sp] += arrivals[k * ns + sp];
                        const double pool = s.old_water[flat * layers + k] + incoming[k];
                        std::vector<double> reacted, clamped;
                        if (std::max(s.old_water[flat * layers + k], water[k]) > .0353147) {
                            double temp = r.default_temp_c;
                            const auto& thermal = c.lid_layer_state;
                            if (c.options.heat_transport && flat < thermal.n_units &&
                                thermal.n_species > int(LidSpecies::TEMPERATURE)) {
                                const double value = thermal.value[thermal.layer_index(
                                    flat, static_cast<LidLayer>(k), int(LidSpecies::TEMPERATURE))];
                                if (std::isfinite(value))
                                    temp = value;
                            }
                            reactPool(c, control, k + 1, pool, mass, dt, temp, reacted, clamped);
                            for (int sp = 0; sp < ns; ++sp) {
                                const double factor = 28.317 * r.surface.mcf[sp];
                                r.surface.led_reacted[sp] += reacted[sp] * factor;
                                r.surface.led_reaction_clamped[sp] += clamped[sp] * factor;
                            }
                        }
                        const double vs = k == surf_layer ? surface : 0.,
                                     vd = k == drain_layer ? drain : 0.,
                                     vi = k == last ? infiltrated : 0.;
                        const double outgoing = internal[k] + vs + vd + vi;
                        const double mixed = std::max({pool - std::max(0., evap[k]) * area * dt,
                                                       outgoing + water[k], outgoing});
                        for (int sp = 0; sp < ns; ++sp) {
                            const double concentration = mixed > 0 ? mass[sp] / mixed : 0.;
                            for (int dest = 0; dest < layers; ++dest)
                                if (donor[dest] == k)
                                    arrivals[dest * ns + sp] += concentration * incoming[dest];
                            s.surface_export[std::size_t(sc) * ns + sp] += concentration * vs;
                            if (k == surf_layer)
                                s.surface_conc[flat * ns + sp] = surface > 0 ? concentration : 0.;
                            if (k == drain_layer)
                                s.drain_conc[flat * ns + sp] = drain > 0 ? concentration : 0.;
                            r.surface.led_infil_loss[sp] +=
                                concentration * vi * 28.317 * r.surface.mcf[sp];
                            mass[sp] = std::max(0., mass[sp] - concentration * outgoing);
                        }
                        s.old_water[flat * layers + k] = water[k];
                    }
            }
    }
}
/**
 * @brief Publish conventional reactive LID surface export as accepted subcatchment runoff
 * concentration.
 * @param[in,out] c Prepared reactive subcatch mask, per-step surface_export/carry native
 * inventories and current accepted runoff ft3/s. Writes surface.washoff_conc and carries
 * undelivered inventory.
 * @param[in] dt Positive finite matching accepted runoff duration s, not independently
 * validated.
 * @note For each reactive source, divides export+old carry by positive runoff*dt ft3, else
 * publishes0 and retains all as carry. Does not clear surface_export, so repeated calls before
 * routeUnits clears it can duplicate carry. No physical water/global ledger/clock update or
 * allocation; exclusive owner, qualified extents and exactly-once publication chronology
 * required.
 */
void publishUnits(SimulationContext& c, double dt) {
    auto& r = c.reactions;
    const int ns = r.n_species();
    for (int sc = 0; sc < c.n_subcatches(); ++sc)
        if (reactedSubcatchment(c, sc))
            for (int sp = 0; sp < ns; ++sp) {
                const auto k = std::size_t(sc) * ns + sp;
                const double mass = r.lid.surface_export[k] + r.lid.surface_carry[k];
                const double water = c.subcatches.runoff[sc] * dt;
                r.surface.washoff_conc[k] = water > 0 ? mass / water : 0.;
                r.lid.surface_carry[k] = water > 0 ? 0. : mass;
            }
}
/**
 * @brief Borrow the prepared conventional reactive LID drain concentration tuple.
 * @param[in] c Stable opted-in control/offset/drain native state, alive through borrowed use.
 * @param[in] type Valid group type index with offsets[type] and offsets[type+1]; invalid
 * returnsnull.
 * @param[in] unit Valid nonnegative unit within that group; not independently checked against
 * next offset.
 * @param[in] control Opted-in native LID control ID; disabled returnsnull.
 * @return Nonowned pointer to n_species drain concentrations if flattened extent covered, else
 * null. No count output or ownership transfer. Reinitialization/resize/route mutation can
 * change/invalidate view; caller serializes lifecycle. Does not validate actual unit/control
 * match, finite values, consume receipts, allocate or advance physical/clock state.
 */
const double* drainValues(const SimulationContext& c, int type, int unit, int control) {
    const auto& r = c.reactions;
    if (!r.lid_enabled(control) || type < 0 || type + 1 >= int(r.lid.group_offsets.size()))
        return nullptr;
    const auto k = std::size_t(r.lid.group_offsets[type] + unit) * r.n_species();
    return k + r.n_species() <= r.lid.drain_conc.size() ? r.lid.drain_conc.data() + k : nullptr;
}
} // namespace openswmm::msxlid
