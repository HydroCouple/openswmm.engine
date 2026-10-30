// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "core/CompletedSourceArchive.hpp"
#include "hydrology/RichardsColumn.hpp"
#include <cmath>

namespace openswmm::richards::test {
inline std::vector<Column> columns(int count, int budget, bool mixed_height) {
    std::vector<Column> result;
    for (int i = 0; i < count; ++i) {
        Column c;
        c.options.enabled = true;
        c.options.max_step = 10. + i % 4;
        const int layers = mixed_height && i % 3 == 0 ? 1 : 8;
        const double area = .9 + .05 * (i % 5);
        c.cells.push_back({.1 * area, area, 1.5, .1, .7 + .1 * (i % 3)});
        c.water.push_back(i % 3 ? .02 * area : -0.);
        for (int k = 0; k < layers; ++k) {
            Cell s;
            s.dz = .12 + .01 * ((i + k) % 4);
            s.area = area * (1. + .02 * (k % 2));
            s.volume = s.dz * s.area;
            s.z = 1.5 - (k + .5) * s.dz;
            s.theta_s = .42 + .01 * (k % 3);
            s.Ks = 1.e-5 * (1. + .2 * (i % 4));
            s.wilting = .1 + .005 * (i % 5);
            s.potential_et = 1.e-8 * ((i + k) % 3);
            s.material = {.04 + .002 * ((i + k) % 3), 1.8 + .1 * (k % 3),
                          1.5 + .03 * (i % 4), .4 + .05 * (i % 3),
                          1.e-4 * (1. + .1 * (k % 4))};
            c.cells.push_back(s);
            const double theta = i % 11 == 0 ? s.theta_s + .000001
                : s.material.theta_r + .55 * (s.theta_s - s.material.theta_r);
            c.water.push_back(theta * s.volume);
        }
        c.inflow = i % 2 ? 1.e-6 : 0.;
        c.bottom = budget == 0 ? Bottom::FreeDrainage : Bottom::Head;
        if (budget == 2 && i % 5 == 0)
            c.bottom = Bottom::Sealed;
        c.bottom_K = 1.e-5;
        c.bottom_distance = .1 + .01 * (i % 4);
        c.bottom_head = i % 2 ? -.2 : .2;
        c.bottom_area = i % 3 ? 1.1 * area : 0.;
        c.bottom_factor = .2 + .1 * (i % 9);
        if (budget == 1 || (budget == 2 && i % 2)) {
            c.bottom_in_limit = i % 3 ? 1.e-6 : 0.;
            c.bottom_out_limit = i % 3 ? 2.e-6 : 0.;
        }
        result.push_back(std::move(c));
    }
    return result;
}
inline std::vector<Column*> pointers(std::vector<Column>& columns) {
    std::vector<Column*> result;
    for (auto& c : columns)
        result.push_back(&c);
    return result;
}
inline void record(detail::CompletedSourceArchive& out, std::vector<Column>& columns,
                   std::vector<Report>& reports) {
    uint64_t count = columns.size();
    out(count);
    for (std::size_t i = 0; i < columns.size(); ++i) {
        auto& r = reports[i];
        out(columns[i].water, r.ok, r.error, r.accepted, r.rejected, r.rhs, r.newton,
            r.min_step, r.bottom_volume, r.evaporation, r.balance, r.bottom_outflow,
            r.bottom_inflow, r.inflow, r.bottom_out_limited, r.bottom_in_limited);
        out.sequence(r.transfers, [&](auto& t) { out(t.from, t.to, t.volume); });
    }
}
inline std::string water(std::vector<Column>& columns) {
    detail::CompletedSourceArchive out;
    for (auto& c : columns)
        out(c.water);
    return out.bytes();
}
}
