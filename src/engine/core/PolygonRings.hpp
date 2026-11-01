// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#pragma once

#include <cstddef>
#include <vector>

namespace openswmm::polygon {
struct Ring {
    std::size_t first, count; // excludes a repeated closing vertex
};

// A flat spatial polygon can contain successive explicitly closed rings.
// A final open ring remains compatible with ordinary SWMM [POLYGONS].
inline std::vector<Ring> rings(const std::vector<double>& x, const std::vector<double>& y) {
    std::vector<Ring> out;
    if (x.size() != y.size())
        return out;
    std::size_t first = 0;
    for (std::size_t i = 1; i < x.size(); ++i) {
        if (i - first >= 3 && x[i] == x[first] && y[i] == y[first]) {
            out.push_back({first, i - first});
            first = i + 1;
        }
    }
    if (x.size() - first >= 3)
        out.push_back({first, x.size() - first});
    return out;
}

inline bool contains(const std::vector<double>& x, const std::vector<double>& y,
                     Ring ring, double px, double py) {
    bool inside = false;
    for (std::size_t i = 0, j = ring.count - 1; i < ring.count; j = i++) {
        const auto a = ring.first + i, b = ring.first + j;
        if ((y[a] > py) != (y[b] > py) &&
            px < (x[b] - x[a]) * (py - y[a]) / (y[b] - y[a]) + x[a])
            inside = !inside;
    }
    return inside;
}

inline bool contains(const std::vector<double>& x, const std::vector<double>& y,
                     double px, double py) {
    bool inside = false;
    for (auto ring : rings(x, y))
        inside ^= contains(x, y, ring, px, py);
    return inside;
}
} // namespace openswmm::polygon
