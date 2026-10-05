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
// Reconstruct infiltration history from current physical moisture when loading
// a pre-V10 hot start. Exact continuation requires V10 history.
void initializeInfiltration(SimulationContext&, int node);
void prepareStep(SimulationContext&, double dt, double evaporation);
void finishStep(SimulationContext&);
void resetPorts(SimulationContext&);
double portOffset(const SimulationContext&, int link, int node);
// Physical interfaces belong to the cell above, allowing only roundoff-sized
// elevation differences. Shared by hydraulic and pollutant port coupling.
int portCell(const LidNodeState&, double offset);
double portDepth(const SimulationContext&, int node, double offset);
double exchangePorts(SimulationContext&, int link, double flow, double dt);
double bottomConductivity(const SimulationContext&, int storage_row);
double bottomCloggingFactor(const SimulationContext&, int storage_row);
double heldVolume(const SimulationContext&, int node);
bool active(const SimulationContext&, int node);
bool validTreatment(const SimulationContext&, const LidLayerTreatment&, std::string& error);
void readTreatment(SimulationContext&, const std::vector<std::string>&);
void prepareQuality(SimulationContext&, double dt);
void prepareOutletQuality(SimulationContext&, double dt, bool book_reaction);
bool receiveQuality(SimulationContext&, int node, int link, double volume, int pollutant, double mass, double dt);
double outletQuality(const SimulationContext&, int node, int link, int pollutant, double fallback);
double heldMass(const SimulationContext&, int node, int pollutant);
void readNodes(SimulationContext&, const std::vector<std::string>&);
void readOutlets(SimulationContext&, const std::vector<std::string>&);
} // namespace lidnode
} // namespace openswmm
#endif
