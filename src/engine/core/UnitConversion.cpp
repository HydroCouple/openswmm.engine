/**
 * @file UnitConversion.cpp
 * @brief Global unit conversion — matching legacy SWMM UCF().
 * @ingroup engine_core
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "UnitConversion.hpp"
#include "SimulationOptions.hpp"

namespace openswmm {
namespace ucf {

int getUnitSystem(int flow_units) {
    // CFS=0, GPM=1, MGD=2 → US (0)
    // CMS=3, LPS=4, MLD=5 → SI (1)
    return (flow_units >= 3) ? 1 : 0;
}

double UCF(int quantity, const SimulationOptions& opts) {
    int fu = static_cast<int>(opts.flow_units);
    if (quantity < FLOW) {
        int us = getUnitSystem(fu);
        if (quantity >= 0 && quantity <= 9)
            return Ucf[quantity][us];
        return 1.0;
    }
    // FLOW
    if (fu >= 0 && fu <= 5)
        return Qcf[fu];
    return 1.0;
}

} // namespace ucf
} // namespace openswmm
