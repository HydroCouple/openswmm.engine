// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#ifndef OPENSWMM_LID_NODE_DATA_HPP
#define OPENSWMM_LID_NODE_DATA_HPP
#include <array>
#include <vector>

namespace openswmm {
// Authored, ordered layers. Thickness/suction are in rain-depth units (in/mm),
// conductivity in in/hr or mm/hr. BOTTOM is a boundary, not a numbered layer.
enum class LidNodeLayerKind { Surface = 0, Media = 1, Aggregate = 2, Bottom = 3 };
struct LidNodeLayer {
    LidNodeLayerKind kind = LidNodeLayerKind::Media;
    std::array<double, 7> params{};
};
struct LidNodeCell {
    double bottom = 0.0, top = 0.0; // feet above invert
    double geometric_volume = 0.0, area = 0.0;
    double porosity = 1.0, theta = 0.0, field_capacity = 0.0, wilting_point = 0.0;
    double conductivity = 0.0, exponent = 3.0, suction = 0.0;
    LidNodeLayerKind kind = LidNodeLayerKind::Aggregate;
    int layer = 0;
};
struct LidNodeState {
    double mobile_delta = 0.0; // accepted port balance in the current hydraulic iteration
    std::vector<double> port_delta; // accepted link exchange volumes, reset each hydraulic iteration
    std::vector<LidNodeCell> cells; // top to bottom; MEDIA subdivided dynamically
    double held_volume = 0.0;
    double evap_volume = 0.0;
    double captured_flow = 0.0;
    double treated_volume = 0.0;
};
struct LidNodeConfig {
    int control = -1;
    double initial_saturation = 0.0; // percent
};
struct LidNodeOutlet {
    int link = -1;
    int layer = 1; // one-based, top to bottom
    bool top = false;
};
} // namespace openswmm
#endif
