// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "2d/heat/PorousHeat.hpp"
#include "2d/heat/PorewaterTransport.hpp"
#include "core/CompletedSourceArchive.hpp"
#include <cmath>
#include <limits>
#include <string>
#include <vector>
namespace openswmm::twoD::column_test {
struct ThermalCase {
    std::string label;
    std::vector<ThermalStore> stores;
    std::vector<ThermalContact> contacts;
    std::vector<ThermalBoundary> boundaries;
    double seconds = 10;
};
inline ThermalCase thermalCase(int n, int variant = 0) {
    ThermalCase c;
    c.label = "thermal-" + std::to_string(n) + "-" + std::to_string(variant);
    c.seconds = variant == 0 ? .25 : variant == 1 ? 10 : 100;
    for (int j = 0; j < n; ++j) {
        const double capacity = 1000 + 37 * j + 100 * variant;
        c.stores.push_back({capacity, j == 0 && variant == 0
                                         ? -0. : capacity * ((j % 5) - 2 + .25 * variant)});
        if (j)
            c.contacts.push_back({j - 1, j, j % 4 == 0 ? 0. : 20. + variant * j});
    }
    if (n > 2) c.contacts.push_back({0, n - 1, 7.});
    if (n) {
        c.boundaries.push_back({0, 1. + variant, -4. + variant, .125});
        c.boundaries.push_back({n - 1, 0., 12., -.25});
    }
    return c;
}
inline std::vector<ThermalCase> thermalCases() {
    std::vector<ThermalCase> out;
    for (int n : {0, 1, 2, 3, 8, 17, 33, 8, 1, 0})
        for (int v : {0, 1, 2}) out.push_back(thermalCase(n, v));
    const auto add = [&](ThermalCase c, const std::string& label) {
        c.label = label; out.push_back(std::move(c));
        auto recovery = thermalCase(3, 1); recovery.label = "recovery-" + label;
        out.push_back(std::move(recovery));
    };
    for (double dt : {0., -1., std::numeric_limits<double>::infinity(),
                      std::numeric_limits<double>::quiet_NaN(),
                      std::numeric_limits<double>::denorm_min(), 1.e20}) {
        auto c = thermalCase(3, 1); c.seconds = dt; add(c, "interval-" + std::to_string(out.size()));
    }
    for (int error = 0; error < 12; ++error) {
        auto c = thermalCase(3, 1);
        switch (error) {
        case 0: c.stores[1].capacity = 0; break;
        case 1: c.stores[1].energy = std::numeric_limits<double>::infinity(); break;
        case 2: c.stores[1].capacity = std::numeric_limits<double>::quiet_NaN(); break;
        case 3: c.contacts[0].b = c.contacts[0].a; break;
        case 4: c.contacts[0].a = -1; break;
        case 5: c.contacts[0].b = 3; break;
        case 6: c.contacts[0].conductance = -1; break;
        case 7: c.contacts[0].conductance = std::numeric_limits<double>::infinity(); break;
        case 8: c.boundaries[0].store = -1; break;
        case 9: c.boundaries[0].temperature = std::numeric_limits<double>::infinity(); break;
        case 10: c.boundaries[0].power = std::numeric_limits<double>::quiet_NaN(); break;
        case 11: c.boundaries[0].conductance = -1; break;
        }
        add(c, "invalid-" + std::to_string(error));
    }
    ThermalCase zero;
    zero.stores = {{1., -0.}, {2., 0.}};
    add(zero, "isolated-signed-zero");
    ThermalCase late;
    late.stores = {{1.e-308, 1.}};
    late.boundaries = {{0, 0., -1.e308, 0.}};
    add(late, "late-receipt-refusal");
    return out;
}
inline std::string input(ThermalCase c) {
    detail::CompletedSourceArchive a;
    a(c.label, c.seconds);
    a.sequence(c.stores, [&](auto& s) { a(s.capacity, s.energy); });
    a.sequence(c.contacts, [&](auto& s) { a(s.a, s.b, s.conductance); });
    a.sequence(c.boundaries, [&](auto& s) { a(s.store, s.conductance, s.temperature, s.power); });
    return a.bytes();
}
inline std::string output(ThermalSolution r) {
    detail::CompletedSourceArchive a;
    a(r.error, r.interval_limit, r.energy, r.temperature, r.contact_j, r.boundary_j,
      r.algebraic_residual, r.iterations);
    return a.bytes();
}
struct PorewaterCase {
    std::string label;
    std::vector<double> old_water, new_water, retention, mass, change, external_water,
        external_mass, withdrawal, reserved_water, reserved_mass;
};
inline PorewaterCase porewaterCase(int n, int variant = 0) {
    PorewaterCase c;
    c.label = "porewater-" + std::to_string(n) + "-" + std::to_string(variant);
    std::vector<double> flux(n + 1, 0);
    if (variant == 0)
        for (int j = 1; j < n; ++j) flux[j] = j % 2 ? .025 : -.025;
    for (int j = 0; j < n; ++j) {
        const double water = variant == 1 && j % 4 == 0 ? 0. : 1. + .1 * j;
        const double mass = variant == 2 ? 0. : water > 0 ? water * (1 + j % 4) : 2.;
        const double change = flux[j] - flux[j + 1];
        c.old_water.push_back(water); c.new_water.push_back(water + change);
        c.retention.push_back(variant == 1 ? 0. : .3 * (j % 3));
        c.mass.push_back(j == 0 && variant == 2 ? -0. : mass);
        c.change.push_back(change); c.external_water.push_back(0.);
        c.external_mass.push_back(variant == 2 ? .125 : 0.);
        c.withdrawal.push_back(water > 0 ? .05 : 0.);
        c.reserved_water.push_back(variant == 2 ? .1 * water : 0.);
        c.reserved_mass.push_back(variant == 2 ? .1 * mass : 0.);
    }
    return c;
}
inline std::vector<PorewaterCase> porewaterCases() {
    std::vector<PorewaterCase> out;
    for (int n : {0, 1, 2, 3, 8, 17, 33, 8, 1, 0})
        for (int v : {0, 1, 2}) out.push_back(porewaterCase(n, v));
    const auto add = [&](PorewaterCase c, const std::string& label) {
        c.label = label; out.push_back(std::move(c));
        auto recovery = porewaterCase(3, 0); recovery.label = "recovery-" + label;
        out.push_back(std::move(recovery));
    };
    for (int error = 0; error < 13; ++error) {
        auto c = porewaterCase(3, 0);
        switch (error) {
        case 0: c.old_water.pop_back(); break;
        case 1: c.new_water.pop_back(); break;
        case 2: c.mass[1] = -1; break;
        case 3: c.retention[1] = std::numeric_limits<double>::infinity(); break;
        case 4: c.external_mass[1] = std::numeric_limits<double>::quiet_NaN(); break;
        case 5: c.external_water[1] = .5; break;
        case 6: c.change[1] += .5; break;
        case 7: c.withdrawal[1] = -1; break;
        case 8: c.reserved_water[1] = 10; break;
        case 9: c.reserved_mass[1] = 10; break;
        case 10: c.reserved_water.pop_back(); break;
        case 11: c.external_mass[1] = -10; break;
        case 12: c.new_water[1] = -1; break;
        }
        add(c, "invalid-" + std::to_string(error));
    }
    auto late = porewaterCase(3, 2);
    late.retention.assign(3, 0); late.withdrawal.assign(3, 0);
    late.reserved_water.assign(3, 0); late.reserved_mass.assign(3, 0);
    late.external_mass.assign(3, std::numeric_limits<double>::max() * .6);
    add(late, "late-receipt-refusal");
    PorewaterCase dry;
    dry.old_water = {1}; dry.new_water = {0}; dry.change = {-1};
    dry.retention = {0}; dry.mass = {2}; dry.external_water = {-1};
    dry.external_mass = {0}; dry.withdrawal = {0};
    add(dry, "dry-residue");
    auto through = dry;
    through.old_water = {0}; through.change = {0}; through.external_water = {0};
    through.mass = {0}; through.external_mass = {2}; through.withdrawal = {1};
    add(through, "dry-throughflow");
    return out;
}
inline std::string input(PorewaterCase c) {
    detail::CompletedSourceArchive a;
    a(c.label, c.old_water, c.new_water, c.retention, c.mass, c.change, c.external_water,
      c.external_mass, c.withdrawal, c.reserved_water, c.reserved_mass);
    return a.bytes();
}
inline std::string output(PorewaterAdvection r) {
    detail::CompletedSourceArchive a;
    a(r.error, r.mass, r.concentration, r.withdrawn_mass, r.vertical_water, r.vertical_mass,
      r.external_mass, r.water_residual);
    return a.bytes();
}
} // namespace openswmm::twoD::column_test
