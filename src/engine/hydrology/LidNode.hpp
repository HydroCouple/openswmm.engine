// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#ifndef OPENSWMM_LID_NODE_HPP
#define OPENSWMM_LID_NODE_HPP
#include "../data/LidNodeData.hpp"
#include <string>
namespace openswmm {
struct SimulationContext;
namespace lidnode {
std::vector<LidNodeLayer> layers(const SimulationContext&, int control);
bool validLayer(const LidNodeLayer&);
std::string validateStack(const std::vector<LidNodeLayer>&);
double thickness(const std::vector<LidNodeLayer>&);
// Validate references, layer stacks, MaxDepth and anchored offsets. Authored
// layer units remain unchanged; node/link values are canonical internal units.
void validate(SimulationContext&);
void sync(SimulationContext&, int control);
void initialize(SimulationContext&);
void prepareStep(SimulationContext&, double dt, double evaporation);
void finishStep(SimulationContext&);
void resetPorts(SimulationContext&);
double portDepth(const SimulationContext&, int node, double offset);
double exchangePorts(SimulationContext&, int link, double flow, double dt);
double bottomConductivity(const SimulationContext&, int storage_row);
double bottomCloggingFactor(const SimulationContext&, int storage_row);
double heldVolume(const SimulationContext&, int node);
bool active(const SimulationContext&, int node);
void readNodes(SimulationContext&, const std::vector<std::string>&);
void readOutlets(SimulationContext&, const std::vector<std::string>&);
} // namespace lidnode
} // namespace openswmm
#endif
