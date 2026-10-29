// SPDX-License-Identifier: Apache-2.0

/**
 * @file      PorousHeat.cpp
 * @brief     Local thermal equilibrium in porous ground layers.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "PorousHeat.hpp"
#include "../subsurface/SubsurfaceData.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
namespace openswmm::twoD {
std::string ThermalMaterial::validate() const {
    if (!std::isfinite(porosity) || porosity < 0 || porosity >= 1)
        return "Thermal porosity must be finite and in [0,1).";
    for (double value : {grain_density, grain_specific_heat, grain_conductivity, water_conductivity,
                         air_conductivity, water_heat_capacity})
        if (!(value > 0) || !std::isfinite(value))
            return "Thermal phase density, heat capacity and conductivities must be finite and positive.";
    return {};
}
double ThermalMaterial::volumetricCapacity(double theta) const {
    return (1 - porosity) * grain_density * grain_specific_heat + theta * water_heat_capacity;
}
double ThermalMaterial::conductivity(double theta) const {
    return geometric ? std::exp((1 - porosity) * std::log(grain_conductivity) +
                                theta * std::log(water_conductivity) +
                                (porosity - theta) * std::log(air_conductivity))
                     : (1 - porosity) * grain_conductivity + theta * water_conductivity +
                           (porosity - theta) * air_conductivity;
}
namespace {
/**
 * @brief Accumulate a thermal-solver dot product using long-double products and sum.
 * @param[in] a First operand and controlling extent.
 * @param[in] b Second operand with at least a.size() entries; surplus values are ignored.
 * @return Sum of corresponding products, zero for an empty a. Sizes/finiteness are unchecked.
 */
long double dot(const std::vector<double>& a, const std::vector<double>& b) {
    long double result = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        result += static_cast<long double>(a[i]) * b[i];
    return result;
}
void clearThermalSolution(ThermalSolution& out) {
    out.error.clear();
    out.interval_limit = false;
    out.energy.clear();
    out.temperature.clear();
    out.contact_j.clear();
    out.boundary_j.clear();
    out.algebraic_residual = 0;
    out.iterations = 0;
}
}
namespace {
template <bool Reset>
#if defined(_MSC_VER)
__forceinline
#elif defined(__GNUC__) || defined(__clang__)
inline __attribute__((always_inline))
#else
inline
#endif
ThermalSolution& solveThermalContactsImpl(const std::vector<ThermalStore>& stores,
                                          const std::vector<ThermalContact>& contacts,
                                          const std::vector<ThermalBoundary>& boundaries, double dt,
                                          ThermalContactScratch& scratch) {
    auto& out = scratch.result;
    if constexpr (Reset)
        clearThermalSolution(out);
    const auto refused = [&](const char* message) -> ThermalSolution& {
        out.error = message;
        return out;
    };
    if (!(dt > 0) || !std::isfinite(dt))
        return refused("Thermal step must be finite and positive.");
    const auto n = stores.size();
    auto& diag = scratch.diag;
    auto& rhs = scratch.rhs;
    auto& x = scratch.x;
    auto& r = scratch.r;
    auto& z = scratch.z;
    auto& p = scratch.p;
    auto& ap = scratch.ap;
    for (auto* values : {&diag, &rhs, &x, &r, &z, &p, &ap})
        values->assign(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        if (!(stores[i].capacity > 0) || !std::isfinite(stores[i].capacity) ||
            !std::isfinite(stores[i].energy))
            return refused("Thermal stores require finite energy and positive capacity.");
        diag[i] = stores[i].capacity;
        rhs[i] = stores[i].energy;
        x[i] = rhs[i] / diag[i];
    }
    for (const auto& c : contacts) {
        if (c.a < 0 || c.b < 0 || std::size_t(c.a) >= n || std::size_t(c.b) >= n || c.a == c.b ||
            c.conductance < 0 || !std::isfinite(c.conductance))
            return refused("Invalid reciprocal thermal contact or conductance.");
        diag[c.a] += dt * c.conductance;
        diag[c.b] += dt * c.conductance;
    }
    for (const auto& b : boundaries) {
        if (b.store < 0 || std::size_t(b.store) >= n || b.conductance < 0 ||
            !std::isfinite(b.conductance) || !std::isfinite(b.temperature) ||
            !std::isfinite(b.power))
            return refused("Invalid thermal boundary.");
        diag[b.store] += dt * b.conductance;
        rhs[b.store] += dt * (b.conductance * b.temperature + b.power);
    }
    for (std::size_t i = 0; i < n; ++i)
        if (!std::isfinite(diag[i]) || !std::isfinite(rhs[i]))
            return refused(
                "Thermal system overflow; reduce the step or check material/forcing values.");
    const auto multiply = [&](const std::vector<double>& v, std::vector<double>& result) {
        for (std::size_t i = 0; i < n; ++i)
            result[i] = stores[i].capacity * v[i];
        // Difference form retains the exact uniform-field nullspace of the
        // contact Laplacian even for large conductance/capacity ratios.
        for (const auto& c : contacts) {
            const double q = dt * c.conductance * (v[c.a] - v[c.b]);
            result[c.a] += q;
            result[c.b] -= q;
        }
        for (const auto& b : boundaries)
            result[b.store] += dt * b.conductance * v[b.store];
    };
    multiply(x, ap);
    long double scale = 0, capacity_scale = 0;
    for (std::size_t i = 0; i < n; ++i) {
        r[i] = rhs[i] - ap[i];
        z[i] = r[i] / diag[i];
        p[i] = z[i];
        scale += static_cast<long double>(rhs[i]) * rhs[i] / diag[i];
        capacity_scale +=
            static_cast<long double>(stores[i].capacity) * stores[i].capacity / diag[i];
    }
    const long double tolerance =
        1e-26L * std::max(scale, capacity_scale); // 1e-13 in preconditioned norm
    long double rz = dot(r, z);
    const auto max_iterations = std::min<std::size_t>(4096, 4 * n + 64);
    for (std::size_t iteration = 0; rz > tolerance && iteration < max_iterations; ++iteration) {
        multiply(p, ap);
        const long double pap = dot(p, ap);
        if (!(pap > 0) || !std::isfinite(pap))
            return refused("Thermal implicit solve lost positive definiteness.");
        const double alpha = static_cast<double>(rz / pap);
        for (std::size_t i = 0; i < n; ++i) {
            x[i] += alpha * p[i];
            r[i] -= alpha * ap[i];
            z[i] = r[i] / diag[i];
        }
        long double next = dot(r, z);
        if (next <= tolerance) {
            // Check the actual equation, not just the recursive residual.
            multiply(x, ap);
            for (std::size_t i = 0; i < n; ++i) {
                r[i] = rhs[i] - ap[i];
                z[i] = r[i] / diag[i];
            }
            next = dot(r, z);
        }
        const double beta = static_cast<double>(next / rz);
        for (std::size_t i = 0; i < n; ++i)
            p[i] = z[i] + beta * p[i];
        rz = next;
        out.iterations = int(iteration + 1);
    }
    if (!std::isfinite(rz) || rz > tolerance)
        return refused("Thermal implicit solve did not converge; reduce the thermal step.");
    auto& energy = scratch.energy;
    energy.clear();
    energy.reserve(n);
    for (const auto& s : stores)
        energy.push_back(s.energy);
    out.contact_j.reserve(contacts.size());
    out.boundary_j.reserve(boundaries.size());
    for (const auto& c : contacts) {
        const double joules = dt * c.conductance * (x[c.a] - x[c.b]);
        energy[c.a] -= joules;
        energy[c.b] += joules;
        out.contact_j.push_back(joules);
    }
    for (const auto& b : boundaries) {
        const double joules = dt * (b.conductance * (b.temperature - x[b.store]) + b.power);
        energy[b.store] += joules;
        out.boundary_j.push_back(joules);
    }
    // Energies are updated by the accepted equal/opposite face receipts.
    // The solve residual measures their difference from C*T, in joules.
    for (std::size_t i = 0; i < n; ++i) {
        if (!std::isfinite(energy[i]) || !std::isfinite(x[i]))
            return refused("Nonfinite thermal solution.");
        out.algebraic_residual =
            std::max(out.algebraic_residual, std::abs(energy[i] - stores[i].capacity * x[i]));
        x[i] = energy[i] / stores[i].capacity;
    }
    out.energy.swap(energy);
    out.temperature.swap(x);
    return out;
}
} // namespace
/**
 * @brief Solve reciprocal heat contacts by backward Euler and diagonal-preconditioned CG.
 * @param[in] stores Finite heat stores: positive finite capacity J/K and finite energy J. Inputs
 * are read-only; temperatures are energy/capacity in degrees C relative to the energy datum.
 * @param[in] contacts Reciprocal connections with distinct in-range store indices and finite
 * nonnegative conductance W/K. Positive returned contact receipts move from a to b.
 * @param[in] boundaries In-range store indices, finite nonnegative conductance W/K, finite
 * prescribed temperature degrees C and finite signed power W. Positive receipts enter the store.
 * @param[in] dt Finite strictly positive interval in seconds; boundaries remain fixed
 * throughout.
 * @return Owned solution with empty error on success, final energy J/temperature degrees C,
 * contact and boundary receipts J in input order, iteration count and equation residual J. On
 * numerical/physical refusal error is nonempty; other fields may be partial and must not be
 * published. Allocation failures propagate. All inputs remain unchanged.
 * @note Conjugate-gradient work is bounded by min(4096,4*n+64) iterations, with a 1e-13 relative
 * preconditioned-norm scale. Energies are updated from equal/opposite receipts, then
 * temperatures are recovered from those conservative energies; algebraic_residual measures their
 * solve defect.
 */
ThermalSolution solveThermalContacts(const std::vector<ThermalStore>& stores,
                                     const std::vector<ThermalContact>& contacts,
                                     const std::vector<ThermalBoundary>& boundaries, double dt) {
    ThermalContactScratch scratch;
    solveThermalContactsImpl<false>(stores, contacts, boundaries, dt, scratch);
    return std::move(scratch.result);
}
ThermalSolution& solveThermalContacts(const std::vector<ThermalStore>& stores,
                                      const std::vector<ThermalContact>& contacts,
                                      const std::vector<ThermalBoundary>& boundaries, double dt,
                                      ThermalContactScratch& scratch) {
    return solveThermalContactsImpl<true>(stores, contacts, boundaries, dt, scratch);
}
/**
 * @brief Solve reciprocal heat contacts by bounded implicit-midpoint subdivisions.
 * @param[in] stores Finite heat stores: positive finite capacity J/K and finite energy J. Inputs
 * are read-only; temperatures are energy/capacity in degrees C relative to the energy datum.
 * @param[in] contacts Reciprocal connections with distinct in-range store indices and finite
 * nonnegative conductance W/K. Positive returned contact receipts move from a to b.
 * @param[in] boundaries In-range store indices, finite nonnegative conductance W/K, finite
 * prescribed temperature degrees C and finite signed power W. Positive receipts enter the store.
 * @param[in] dt Finite strictly positive interval in seconds; boundaries remain fixed
 * throughout.
 * @return Owned solution with empty error on success, final energy J/temperature degrees C,
 * contact and boundary receipts J in input order, iteration count and equation residual J. On
 * numerical/physical refusal error is nonempty; other fields may be partial and must not be
 * published. Allocation failures propagate. All inputs remain unchanged.
 * @note Subdivision enforces dt_sub*sum(conductance)/(2*capacity) <= 1. More than 65536 required
 * subintervals returns interval_limit=true; other refusals leave it false. Each midpoint solve
 * uses backward Euler over half a subinterval, and doubled receipts update energy
 * conservatively. This method advances neither moisture nor external thermal owners.
 */
ThermalSolution solveThermalContactsRk2(const std::vector<ThermalStore>& stores,
                                        const std::vector<ThermalContact>& contacts,
                                        const std::vector<ThermalBoundary>& boundaries, double dt) {
    ThermalMidpointScratch scratch;
    solveThermalContactsRk2(stores, contacts, boundaries, dt, scratch);
    return std::move(scratch.result);
}
ThermalSolution& solveThermalContactsRk2(const std::vector<ThermalStore>& stores,
                                         const std::vector<ThermalContact>& contacts,
                                         const std::vector<ThermalBoundary>& boundaries, double dt,
                                         ThermalMidpointScratch& scratch) {
    auto& out = scratch.result;
    clearThermalSolution(out);
    const auto refused = [&](const char* message) -> ThermalSolution& {
        clearThermalSolution(out);
        out.error = message;
        return out;
    };
    if (!(dt > 0) || !std::isfinite(dt))
        return refused("Thermal step must be finite and positive.");
    auto& rate = scratch.rate;
    rate.assign(stores.size(), 0);
    for (const auto& store : stores)
        if (!(store.capacity > 0) || !std::isfinite(store.capacity) ||
            !std::isfinite(store.energy) || !std::isfinite(store.energy / store.capacity))
            return refused(
                "Thermal stores require finite temperature and energy and positive capacity.");
    for (const auto& c : contacts) {
        if (c.a < 0 || c.b < 0 || std::size_t(c.a) >= stores.size() ||
            std::size_t(c.b) >= stores.size() || c.a == c.b || c.conductance < 0 ||
            !std::isfinite(c.conductance))
            return refused("Invalid reciprocal thermal contact or conductance.");
        rate[c.a] += c.conductance;
        rate[c.b] += c.conductance;
    }
    for (const auto& b : boundaries) {
        if (b.store < 0 || std::size_t(b.store) >= stores.size() || b.conductance < 0 ||
            !std::isfinite(b.conductance) || !std::isfinite(b.temperature) ||
            !std::isfinite(b.power))
            return refused("Invalid thermal boundary.");
        rate[b.store] += b.conductance;
    }
    long double divisions = 1;
    for (std::size_t i = 0; i < stores.size(); ++i)
        divisions = std::max(divisions, std::ceil(static_cast<long double>(dt) * rate[i] /
                                                  (2.L * stores[i].capacity)));
    if (!std::isfinite(divisions) || divisions > 65536) {
        auto& limit = refused("Thermal midpoint subdivision limit exceeded; reduce the step.");
        limit.interval_limit = true;
        return limit;
    }
    const int steps = int(divisions);
    const double half = .5 * (dt / steps);
    if (!(half > 0))
        return refused("Thermal midpoint interval is below the representable step.");
    auto& current = scratch.current;
    current = stores;
    out.contact_j.assign(contacts.size(), 0);
    out.boundary_j.assign(boundaries.size(), 0);
    // Small reciprocal receipts must survive repeated updates of a much
    // larger soil energy. Keep the original one-step arithmetic unchanged.
    auto& energy_correction = scratch.energy_correction;
    auto& contact_correction = scratch.contact_correction;
    auto& boundary_correction = scratch.boundary_correction;
    energy_correction.assign(steps > 1 ? stores.size() : 0, 0);
    contact_correction.assign(steps > 1 ? contacts.size() : 0, 0);
    boundary_correction.assign(steps > 1 ? boundaries.size() : 0, 0);
    const auto add = [](double& value, double increment, double& correction) {
        const double adjusted = increment - correction, next = value + adjusted;
        correction = (next - value) - adjusted;
        value = next;
    };
    for (int step = 0; step < steps; ++step) {
        const auto& middle =
            solveThermalContacts(current, contacts, boundaries, half, scratch.implicit);
        if (!middle.error.empty())
            return refused(middle.error.c_str());
        // (C + h L/2) Tmid = Eold + h forcing/2. Twice the
        // midpoint receipts give the Crank-Nicolson energy update. When
        // h*sum(G)/(2*C)<=1, both propagation factors are nonnegative.
        for (std::size_t k = 0; k < contacts.size(); ++k) {
            const double joules = 2 * middle.contact_j[k];
            const auto& c = contacts[k];
            if (steps == 1) {
                current[c.a].energy -= joules;
                current[c.b].energy += joules;
                out.contact_j[k] += joules;
            } else {
                add(current[c.a].energy, -joules, energy_correction[c.a]);
                add(current[c.b].energy, joules, energy_correction[c.b]);
                add(out.contact_j[k], joules, contact_correction[k]);
            }
            if (!std::isfinite(joules) || !std::isfinite(out.contact_j[k]))
                return refused("Nonfinite thermal contact receipt.");
        }
        for (std::size_t k = 0; k < boundaries.size(); ++k) {
            const double joules = 2 * middle.boundary_j[k];
            if (steps == 1) {
                current[boundaries[k].store].energy += joules;
                out.boundary_j[k] += joules;
            } else {
                add(current[boundaries[k].store].energy, joules,
                    energy_correction[boundaries[k].store]);
                add(out.boundary_j[k], joules, boundary_correction[k]);
            }
            if (!std::isfinite(joules) || !std::isfinite(out.boundary_j[k]))
                return refused("Nonfinite thermal boundary receipt.");
        }
        for (const auto& store : current)
            if (!std::isfinite(store.energy) || !std::isfinite(store.energy / store.capacity))
                return refused("Nonfinite thermal midpoint solution.");
        out.iterations += middle.iterations;
        out.algebraic_residual = std::max(out.algebraic_residual, 2 * middle.algebraic_residual);
    }
    for (const auto& store : current) {
        out.energy.push_back(store.energy);
        out.temperature.push_back(store.energy / store.capacity);
    }
    return out;
}
/**
 * @brief Build a validated candidate physical heat column and publish it on success.
 * @param[in] a Footprint in m2, finite and positive.
 * @param[in] edges Fixed depths in m, beginning at zero and strictly increasing, water.size()+1.
 * @param[in] m Material with finite porosity in [0,1) and positive phase properties.
 * @param[in] water Nonempty water fractions per layer, finite in [0,m.porosity].
 * @param[in] temperatures Finite degrees C per layer, with water.size() entries.
 * @return Empty on replacement success; diagnostic on validation refusal with this unchanged.
 * Allocation errors propagate before publication.
 */
std::string PorousHeatColumn::initialize(double a, const std::vector<double>& edges,
                                         const ThermalMaterial& m, const std::vector<double>& water,
                                         const std::vector<double>& temperatures) {
    if (auto error = m.validate(); !error.empty())
        return error;
    const auto n = water.size();
    if (!(a > 0) || !std::isfinite(a) || !n || edges.size() != n + 1 || temperatures.size() != n ||
        edges.front() != 0)
        return "Thermal column requires positive area and a fixed depth grid starting at zero.";
    PorousHeatColumn next;
    next.material = m;
    next.area = a;
    next.depth_edges = edges;
    next.energy.resize(n);
    for (std::size_t j = 0; j < n; ++j)
        if (!(edges[j + 1] > edges[j]) || !std::isfinite(edges[j + 1]) ||
            !std::isfinite(temperatures[j]))
            return "Thermal layer depths must increase and initial temperatures must be finite.";
    if (auto error = next.setMoisture(water); !error.empty())
        return error;
    for (std::size_t j = 0; j < n; ++j)
        next.energy[j] = next.capacity[j] * temperatures[j];
    next.initial_energy = next.energy;
    *this = std::move(next);
    return {};
}
/**
 * @brief Recompute moisture-dependent heat capacity and conductivity without changing energy.
 * @param[in] water Finite per-layer water fraction in [0,material.porosity], with one fewer
 * entry than depth_edges. Geometry/material must already describe this column.
 * @return Empty on success; diagnostic for shape, bounds or invalid derived properties.
 * Validation and temporary allocation precede field replacement; allocation errors propagate.
 * @note Water receipts must carry their own heat before this call. Energy and initial_energy are
 * untouched; changing capacity therefore changes the implied temperature.
 */
std::string PorousHeatColumn::setMoisture(const std::vector<double>& water) {
    if (depth_edges.size() != water.size() + 1)
        return "Thermal moisture shape differs from the fixed depth grid.";
    for (double value : water)
        if (!std::isfinite(value) || value < 0 || value > material.porosity)
            return "Thermal moisture must lie between zero and porosity.";
    std::vector<double> cap(water.size()), lambda(water.size());
    for (std::size_t j = 0; j < water.size(); ++j) {
        cap[j] =
            area * (depth_edges[j + 1] - depth_edges[j]) * material.volumetricCapacity(water[j]);
        lambda[j] = material.conductivity(water[j]);
        if (!(cap[j] > 0) || !std::isfinite(cap[j]) || !(lambda[j] > 0) ||
            !std::isfinite(lambda[j]))
            return "Invalid thermal layer capacity or conductivity.";
    }
    theta = water;
    capacity = std::move(cap);
    conductivity = std::move(lambda);
    return {};
}
std::vector<ThermalStore> PorousHeatColumn::stores() const {
    std::vector<ThermalStore> result;
    stores(result);
    return result;
}
void PorousHeatColumn::stores(std::vector<ThermalStore>& result) const {
    result.clear();
    result.reserve(energy.size());
    for (std::size_t j = 0; j < energy.size(); ++j)
        result.push_back({capacity[j], energy[j]});
}
std::vector<ThermalContact> PorousHeatColumn::contacts() const {
    std::vector<ThermalContact> result;
    contacts(result);
    return result;
}
void PorousHeatColumn::contacts(std::vector<ThermalContact>& result) const {
    result.clear();
    result.reserve(energy.empty() ? 0 : energy.size() - 1);
    for (std::size_t j = 1; j < energy.size(); ++j) {
        const double resistance = .5 * (depth_edges[j] - depth_edges[j - 1]) / conductivity[j - 1] +
                                  .5 * (depth_edges[j + 1] - depth_edges[j]) / conductivity[j];
        result.push_back({int(j - 1), int(j), area / resistance});
    }
}
/**
 * @brief Reconstruct conservative layer water fluxes and apply implicit upwind heat advection.
 * @param[in,out] column Prepared nonempty physical column; replaced only after successful solve
 * and finite energy checks. Material water heat capacity is J/(m3 K), energy J and area m2.
 * @param[in] moisture Endpoint water fractions per layer; same layer count and valid moisture
 * bounds.
 * @param[in] external_water Signed accepted external water volumes m3 per layer, excluding
 * purely geometric hydraulic-grid remapping. The reconstructed column water balance must close.
 * @param[in] external_j Signed paired external sensible energy J per layer.
 * @param[in] withdrawal Optional nonnegative finite implicit outgoing volumes m3 per layer;
 * empty means none. Their donor heat is solved at the new implicit layer temperature and
 * subtracted.
 * @return Owned result with empty error on success, n-1 interior downward-positive water/energy
 * receipts m3/J, n donor temperatures degrees C and outgoing energies J, net external energy J
 * and bottom water closure residual m3. On refusal error is nonempty and column is unchanged;
 * other result fields may be partial. Allocation exceptions propagate without publishing the
 * candidate.
 * @note The prepared column's existing arrays/geometry must have consistent layer extents.
 * Closure tolerance is 1e-10 times total absolute external/change volume plus 1e-12 m3. This is
 * an integrated accepted-transfer update, not a rate calculation or conductive heat step.
 */
ThermalAdvection advectPorousColumn(PorousHeatColumn& column, const std::vector<double>& moisture,
                                    const std::vector<double>& external_water,
                                    const std::vector<double>& external_j,
                                    const std::vector<double>& withdrawal) {
    ThermalAdvection out;
    const auto n = column.theta.size();
    if (!n || moisture.size() != n || external_water.size() != n || external_j.size() != n ||
        (!withdrawal.empty() && withdrawal.size() != n)) {
        out.error = "Thermal advection layer shapes differ.";
        return out;
    }
    auto next = column;
    if (auto error = next.setMoisture(moisture); !error.empty()) {
        out.error = error;
        return out;
    }
    std::vector<double> lower(n, 0), diagonal = next.capacity, upper(n, 0), rhs = column.energy,
                                     water(n + 1, 0);
    double volume_scale = 0;
    for (std::size_t j = 0; j < n; ++j) {
        if (!std::isfinite(external_water[j]) || !std::isfinite(external_j[j])) {
            out.error = "Nonfinite physical water/energy receipt.";
            return out;
        }
        const double bulk = column.area * (column.depth_edges[j + 1] - column.depth_edges[j]);
        const double delta = (moisture[j] - column.theta[j]) * bulk;
        water[j + 1] = water[j] + external_water[j] - delta;
        volume_scale += std::abs(external_water[j]) + std::abs(delta);
        rhs[j] += external_j[j];
        out.external_j += external_j[j];
    }
    out.water_residual = water.back();
    if (std::abs(out.water_residual) > 1e-10 * volume_scale + 1e-12) {
        out.error = "Accepted groundwater transfers do not close the fixed-grid water balance.";
        return out;
    }
    const double cp = column.material.water_heat_capacity;
    if (!withdrawal.empty())
        for (std::size_t j = 0; j < n; ++j) {
            if (withdrawal[j] < 0 || !std::isfinite(withdrawal[j])) {
                out.error = "Invalid accepted thermal withdrawal volume.";
                return out;
            }
            diagonal[j] += cp * withdrawal[j];
        }
    for (std::size_t j = 1; j < n; ++j) {
        const double coefficient = cp * std::abs(water[j]);
        if (water[j] >= 0) {
            diagonal[j - 1] += coefficient;
            lower[j] -= coefficient;
        } else {
            diagonal[j] += coefficient;
            upper[j - 1] -= coefficient;
        }
    }
    // Conservative implicit upwind advection: a tridiagonal M-matrix,
    // distinct from the symmetric conductive/contact system above.
    for (std::size_t j = 1; j < n; ++j) {
        const double f = lower[j] / diagonal[j - 1];
        diagonal[j] -= f * upper[j - 1];
        rhs[j] -= f * rhs[j - 1];
    }
    std::vector<double> temperature(n);
    out.withdrawn_j.assign(n, 0);
    for (std::size_t k = n; k-- > 0;) {
        if (!(diagonal[k] > 0) || !std::isfinite(diagonal[k])) {
            out.error = "Invalid implicit thermal advection capacity.";
            return out;
        }
        temperature[k] = (rhs[k] - (k + 1 < n ? upper[k] * temperature[k + 1] : 0)) / diagonal[k];
        if (!std::isfinite(temperature[k])) {
            out.error = "Nonfinite thermal advection solution.";
            return out;
        }
        const double withdrawn = withdrawal.empty() ? 0 : cp * withdrawal[k] * temperature[k];
        out.withdrawn_j[k] = withdrawn;
        out.external_j -= withdrawn;
        next.energy[k] = column.energy[k] + external_j[k] - withdrawn;
    }
    out.vertical_water.reserve(n - 1);
    out.vertical_j.reserve(n - 1);
    for (std::size_t j = 1; j < n; ++j) {
        const double joules = cp * water[j] * (water[j] >= 0 ? temperature[j - 1] : temperature[j]);
        next.energy[j - 1] -= joules;
        next.energy[j] += joules;
        out.vertical_water.push_back(water[j]);
        out.vertical_j.push_back(joules);
    }
    for (double value : next.energy)
        if (!std::isfinite(value)) {
            out.error = "Nonfinite accepted thermal energy receipt.";
            return out;
        }
    out.temperature = std::move(temperature);
    column = std::move(next);
    return out;
}
/**
 * @brief Apply a conservative explicit heat-advection tangent for one shared RK stage.
 * @param[in,out] column Beginning-state fixed column, published only after all checks pass.
 * @param[in] moisture Endpoint water fractions, used to compute next capacity and conductivity.
 * @param[in] change Integrated beginning-state physical water tangent m3 per layer; used for
 * flux reconstruction rather than inferring it from endpoint moisture.
 * @param[in] external_water Signed finite accepted external water volume m3 per layer.
 * @param[in] external_j Signed finite paired external energy J per layer.
 * @param[in] withdrawal Optional nonnegative finite outgoing volume m3 per layer; empty means
 * none.
 * @param[in] temperature Finite beginning-state donor temperatures degrees C per layer,
 * excluding already reserved outgoing water. These are copied into the returned
 * donor-temperature vector.
 * @return Owned receipts with empty error on success; interior n-1 downward-positive flux
 * volumes m3 and energy J, per-layer withdrawal J, net external energy J and closure residual
 * m3. A nonempty error means refusal with column unchanged and no publishable receipts.
 * @note All nonoptional vectors have n=column.theta.size() entries; n must be positive. Existing
 * column geometry must be consistent. The water closure tolerance is 1e-10*absolute volume scale
 * plus 1e-12 m3. Allocation failures propagate; moisture, conduction and outside owners are not
 * advanced independently. Supplied donor temperatures are trusted rather than recomputed.
 */
ThermalAdvection advectPorousColumnEuler(PorousHeatColumn& column,
                                         const std::vector<double>& moisture,
                                         const std::vector<double>& change,
                                         const std::vector<double>& external_water,
                                         const std::vector<double>& external_j,
                                         const std::vector<double>& withdrawal,
                                         const std::vector<double>& temperature) {
    const auto refused = [](const char* message) {
        ThermalAdvection out;
        out.error = message;
        return out;
    };
    const auto n = column.theta.size();
    if (!n || column.energy.size() != n || moisture.size() != n || change.size() != n ||
        external_water.size() != n || external_j.size() != n || temperature.size() != n ||
        (!withdrawal.empty() && withdrawal.size() != n))
        return refused("Thermal Euler layer shapes differ.");
    if (auto error = column.material.validate(); !error.empty())
        return refused(error.c_str());
    auto next = column;
    if (auto error = next.setMoisture(moisture); !error.empty())
        return refused(error.c_str());
    std::vector<double> water(n + 1, 0);
    double scale = 0;
    for (std::size_t j = 0; j < n; ++j) {
        if (!std::isfinite(change[j]) || !std::isfinite(external_water[j]) ||
            !std::isfinite(external_j[j]) || !std::isfinite(temperature[j]) ||
            (!withdrawal.empty() && (!std::isfinite(withdrawal[j]) || withdrawal[j] < 0)))
            return refused("Invalid physical thermal Euler receipt or donor.");
        water[j + 1] = water[j] + external_water[j] - change[j];
        scale += std::abs(external_water[j]) + std::abs(change[j]);
        if (!std::isfinite(water[j + 1]) || !std::isfinite(scale))
            return refused("Thermal Euler water receipt overflow.");
    }
    if (std::abs(water.back()) > 1e-10 * scale + 1e-12)
        return refused(
            "Thermal Euler water increments do not close the accepted physical balance.");
    ThermalAdvection out;
    out.temperature = temperature;
    out.water_residual = water.back();
    out.withdrawn_j.resize(n);
    const double cp = column.material.water_heat_capacity;
    for (std::size_t j = 0; j < n; ++j) {
        out.withdrawn_j[j] = withdrawal.empty() ? 0 : cp * withdrawal[j] * temperature[j];
        next.energy[j] += external_j[j] - out.withdrawn_j[j];
        out.external_j += external_j[j] - out.withdrawn_j[j];
    }
    for (std::size_t j = 1; j < n; ++j) {
        const double joules = cp * water[j] * (water[j] >= 0 ? temperature[j - 1] : temperature[j]);
        if (!std::isfinite(joules))
            return refused("Thermal Euler internal energy receipt overflow.");
        next.energy[j - 1] -= joules;
        next.energy[j] += joules;
        out.vertical_water.push_back(water[j]);
        out.vertical_j.push_back(joules);
    }
    if (!std::isfinite(out.external_j))
        return refused("Thermal Euler external energy receipt overflow.");
    for (std::size_t j = 0; j < n; ++j)
        if (!std::isfinite(next.energy[j]) || !std::isfinite(next.energy[j] / next.capacity[j]))
            return refused("Nonfinite thermal Euler energy or temperature.");
    column = std::move(next);
    return out;
}
/**
 * @brief Project a moving hydraulic-layer water tangent onto the fixed thermal grid.
 * @param[in] column Prepared column with positive finite area m2, finite porosity in [0,1), and
 * strictly increasing fixed depth edges starting at zero.
 * @param[in] length Current unsaturated length m, finite positive and no greater than grid
 * depth.
 * @param[in] delta_length Integrated change of unsaturated length m, finite and signed.
 * @param[in] old_water Nonempty accepted water depths m per equal stretching hydraulic layer;
 * derived water fraction may exceed [0,porosity] only by the 1e-12 roundoff tolerance.
 * @param[in] delta_water Finite water-depth tangent m per hydraulic layer; old_water.size()
 * entries.
 * @param[out] output Replaced on success with integrated water changes m3 per fixed layer.
 * Remains unchanged on validation refusal or allocation failure.
 * @return Empty on success; diagnostic for invalid geometry, hydraulic moisture or overflow.
 * @note Includes moving-interface jump terms. At an aligned fixed-grid boundary, the receiving
 * layer is chosen from movement direction; no heat inventory or input column is changed.
 */
std::string projectGroundWaterIncrement(const PorousHeatColumn& column, double length,
                                        double delta_length, const std::vector<double>& old_water,
                                        const std::vector<double>& delta_water,
                                        std::vector<double>& output) {
    const auto m = old_water.size(), n = column.theta.size();
    const auto& edges = column.depth_edges;
    if (!m || !n || delta_water.size() != m || edges.size() != n + 1 || !(column.area > 0) ||
        !std::isfinite(column.area) || !(length > 0) || !std::isfinite(length) ||
        !std::isfinite(delta_length) || length > edges.back() ||
        !std::isfinite(column.material.porosity) || column.material.porosity < 0 ||
        column.material.porosity >= 1)
        return "Invalid moving-column thermal projection geometry.";
    const double dz = length / m, phi = column.material.porosity;
    std::vector<double> theta(m), increment(m), next(n, 0);
    for (std::size_t k = 0; k < m; ++k) {
        theta[k] = old_water[k] / dz;
        if (!std::isfinite(theta[k]) || theta[k] < -1e-12 || theta[k] > phi + 1e-12)
            return "Invalid moving-column thermal water increment.";
        theta[k] = std::clamp(theta[k], 0., phi); // derived view, as in moisture projection
        increment[k] = (delta_water[k] - theta[k] * delta_length / m) / dz;
        if (!std::isfinite(increment[k]))
            return "Nonfinite moving-column thermal water increment.";
    }
    for (std::size_t j = 0; j < n; ++j) {
        if (!(edges[j + 1] > edges[j]) || !std::isfinite(edges[j + 1]) || edges.front() != 0)
            return "Thermal projection depth edges must increase from zero.";
        for (std::size_t k = 0; k < m; ++k) {
            const double overlap =
                std::max(0., std::min(edges[j + 1], dz * (k + 1)) - std::max(edges[j], dz * k));
            next[j] += column.area * overlap * increment[k];
        }
        for (std::size_t k = 1; k <= m; ++k) {
            const double z = dz * k, movement = delta_length * (double(k) / m);
            const bool contains = (z > edges[j] && z < edges[j + 1]) ||
                                  (z == edges[j] && movement > 0) ||
                                  (z == edges[j + 1] && movement < 0);
            if (contains)
                next[j] += column.area * (theta[k - 1] - (k < m ? theta[k] : phi)) * movement;
        }
        if (!std::isfinite(next[j]))
            return "Moving-column thermal water projection overflow.";
    }
    output = std::move(next);
    return {};
}
/**
 * @brief Project accepted hydraulic moisture into a fixed physical thermal depth grid.
 * @param[in] s Stable prepared subsurface owner with per-cell arrays sized through n_cells and,
 * for SIGMA, theta_sigma sized m_layers*n_cells in layer-major order.
 * @param[in] i Cell index in [0,s.n_cells).
 * @param[in] hg Accepted saturated thickness m, finite within [0,s.zs[i]].
 * @param[in] edges Finite increasing fixed depths m, from zero to aquifer depth within 1e-12
 * relative tolerance, with at least two entries. Cell area and depth must be positive.
 * @param[out] output On success, replaced by one volumetric water fraction per fixed layer;
 * unchanged on refusal. Fractions outside phase bounds by at most 1e-12 are clipped for
 * roundoff.
 * @param[out] saturated_water Optional distinct output for saturated pore-water volume m3 per
 * fixed layer; untouched on failure. It must not alias output if both results are needed.
 * @return Empty on success; diagnostic for invalid target/grid, missing SIGMA layers or moisture
 * outside finite physical bounds. Allocation failures propagate without output publication.
 * @note Bulk closures distribute unsaturated water uniformly; SIGMA integrates overlaps of its
 * accepted stretching layers. No hydraulic or heat inventory is changed.
 */
std::string projectGroundMoisture(const SubsurfaceState& s, int i, double hg,
                                  const std::vector<double>& edges, std::vector<double>& output,
                                  std::vector<double>* saturated_water) {
    if (i < 0 || i >= s.n_cells || edges.size() < 2)
        return "Invalid fixed-grid groundwater projection target.";
    const double depth = s.zs[i], porosity = s.theta_s[i], area = s.area[i];
    if (!(depth > 0) || !(area > 0) || !std::isfinite(hg) || hg < 0 || hg > depth ||
        edges.front() != 0 || std::abs(edges.back() - depth) > 1e-12 * depth)
        return "Thermal depth grid must span ground surface to aquifer bottom.";
    const double unsaturated = depth - hg;
    const bool sigma = static_cast<GwClosure>(s.closure[i]) == GwClosure::SIGMA;
    if (sigma && s.m_layers <= 0)
        return "SIGMA moisture projection requires accepted hydraulic layers.";
    std::vector<double> water(edges.size() - 1, 0), sat(water.size());
    for (std::size_t j = 0; j < water.size(); ++j) {
        const double top = edges[j], bottom = edges[j + 1], dz = bottom - top;
        if (!(dz > 0) || !std::isfinite(bottom))
            return "Thermal depth edges must increase.";
        const double sat_depth = std::max(0., bottom - std::max(top, unsaturated));
        double unsat_water = 0;
        if (unsaturated > 0) {
            if (sigma)
                for (int k = 0; k < s.m_layers; ++k) {
                    const double overlap =
                        std::max(0., std::min(bottom, unsaturated * (k + 1) / s.m_layers) -
                                         std::max(top, unsaturated * k / s.m_layers));
                    unsat_water += overlap * s.theta_sigma[std::size_t(k) * s.n_cells + i];
                }
            else
                unsat_water =
                    std::max(0., std::min(bottom, unsaturated) - top) * s.hu[i] / unsaturated;
        }
        sat[j] = sat_depth * porosity * area;
        water[j] = (unsat_water + sat_depth * porosity) / dz;
        if (!std::isfinite(water[j]) || water[j] < -1e-12 || water[j] > porosity + 1e-12)
            return "Hydraulic moisture is outside thermal phase-volume bounds.";
        water[j] = std::clamp(water[j], 0.,
                              porosity); // only accepted hydraulic roundoff, never thermal energy
    }
    output = std::move(water);
    if (saturated_water)
        *saturated_water = std::move(sat);
    return {};
}
} // namespace openswmm::twoD
