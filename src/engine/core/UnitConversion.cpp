/**
 * @file UnitConversion.cpp
 * @brief Global unit conversion — matching legacy SWMM UCF().
 * @ingroup engine_core
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
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

double UCF_inv(int quantity, const SimulationOptions& opts) {
    int fu = static_cast<int>(opts.flow_units);
    if (quantity < FLOW) {
        int us = getUnitSystem(fu);
        if (quantity >= 0 && quantity <= 9)
            return Ucf_inv[static_cast<std::size_t>(quantity)][static_cast<std::size_t>(us)];
        return 1.0;
    }
    // FLOW
    if (fu >= 0 && fu <= 5)
        return Qcf_inv[static_cast<std::size_t>(fu)];
    return 1.0;
}

DisplayUnits DisplayUnits::from(const SimulationOptions& opts) {
    DisplayUnits d;
    int fu = static_cast<int>(opts.flow_units);
    if (fu < 0 || fu > 5) fu = 0;
    const int us = getUnitSystem(fu);
    const bool si = (us == 1);

    d.unit_system = us;
    d.flow_units  = fu;

    // internal → display multipliers
    d.flow      = Qcf[fu];
    d.length    = Ucf[LENGTH][us];
    d.volume    = Ucf[VOLUME][us];
    d.rainfall  = Ucf[RAINFALL][us];
    d.raindepth = Ucf[RAINDEPTH][us];
    d.evaprate  = Ucf[EVAPRATE][us];
    d.landarea  = Ucf[LANDAREA][us];
    // ft³ → 10^6 gal | 10^6 ltr  (legacy statsrpt.c Vcf: 7.48/1e6 | 28.3168/1e6)
    d.mvol      = si ? (28.3168 / 1.0e6) : FT3_TO_MGAL;
    // ft³ → acre-ft | hectare-m  (legacy report.c x*UCF(LENGTH)*UCF(LANDAREA))
    d.landvol   = si ? (0.0283168 / 1.0e4) : (1.0 / ACRES_TO_FT2);

    // unit-label words
    d.flow_word        = FlowUnitWords[fu];
    d.length_word      = si ? "Meters"    : "Feet";
    d.depth_word       = si ? "mm"        : "in";
    d.vel_word         = si ? "m/sec"     : "ft/sec";
    d.mvol_word        = si ? "10^6 ltr"  : "10^6 gal";
    d.landvol_word     = si ? "hectare-m" : "acre-feet";
    d.storage_vol_word = si ? "1000 m3"   : "1000 ft3";
    return d;
}

} // namespace ucf
} // namespace openswmm
