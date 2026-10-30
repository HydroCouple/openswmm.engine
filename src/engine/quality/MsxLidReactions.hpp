// SPDX-License-Identifier: Apache-2.0

/**
 * @file      MsxLidReactions.hpp
 * @brief     Multispecies reactions in LID storage and treatment layers.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include <vector>
namespace openswmm {
struct SimulationContext;
namespace lid {
class LIDSolver;
}
namespace msxlid {
void initialize(SimulationContext&);
void initializeUnits(SimulationContext&, const lid::LIDSolver&);
void routeUnits(SimulationContext&, const lid::LIDSolver&, double dt);
void publishUnits(SimulationContext&, double dt);
const double* drainValues(const SimulationContext&, int type, int unit, int control);
void prepareNodes(SimulationContext&, double dt);
void reactNodes(SimulationContext&, double dt);
double outlet(const SimulationContext&, int node, int link, int species, double fallback);
double receive(SimulationContext&, int node, int link, double water, int species, double mass,
               double dt);
double heldMass(const SimulationContext&, int node, int species);
double mobileWater(const SimulationContext&, int node, double fallback);
bool reactedSubcatchment(const SimulationContext&, int subcatchment);
bool reactPool(SimulationContext&, int control, int layer, double volume, double* mass, double dt,
               double temperature, std::vector<double>& reacted, std::vector<double>& clamped);
} // namespace msxlid
} // namespace openswmm
