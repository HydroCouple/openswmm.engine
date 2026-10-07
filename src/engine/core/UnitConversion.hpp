// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file UnitConversion.hpp
 * @brief Global unit conversion factors — matching legacy SWMM Ucf[]/Qcf[].
 *
 * @details Provides static UCF() function and quantity enums identical to
 *          legacy SWMM's UCF(u) macro / function. All internal computations
 *          use ft, ft², ft³, sec as base units. UCF converts from project
 *          display units (in/hr, acres, CFS, etc.) to internal units.
 *
 *          Usage:
 *          @code
 *          double rain_ftsec = rainfall_inhr / ucf::UCF(ucf::RAINFALL, opts);
 *          double depth_ft   = depth_in / ucf::UCF(ucf::RAINDEPTH, opts);
 *          double flow_cfs   = flow_display / ucf::UCF(ucf::FLOW, opts);
 *          @endcode
 *
 * @note Legacy reference: src/legacy/engine/swmm5.c Ucf[], Qcf[], UCF()
 * @ingroup engine_core
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_UNIT_CONVERSION_HPP
#define OPENSWMM_UNIT_CONVERSION_HPP

#include <array>

namespace openswmm {

struct SimulationOptions;

namespace ucf {

// ============================================================================
// Quantity codes — matching legacy enums.h
// ============================================================================

enum Quantity {
    RAINFALL   = 0,   ///< Divide in/hr (US) or mm/hr (SI) by this → ft/sec
    RAINDEPTH  = 1,   ///< Divide in (US) or mm (SI) by this → ft
    EVAPRATE   = 2,   ///< Divide in/day (US) or mm/day (SI) by this → ft/sec
    LENGTH     = 3,   ///< Divide ft (US) or m (SI) by this → ft
    LANDAREA   = 4,   ///< Divide ac (US) or ha (SI) by this → ft²
    VOLUME     = 5,   ///< Divide ft³ (US) or m³ (SI) by this → ft³
    WINDSPEED  = 6,   ///< Divide mph (US) or km/hr (SI) by this → mph
    TEMPERATURE= 7,   ///< Divide °F (US) or °C (SI) by this → °F
    MASS       = 8,   ///< Divide lb (US) or kg (SI) by this → mg
    GWFLOW     = 9,   ///< Divide cfs/ac (US) or cms/ha (SI) by this → ft/sec
    FLOW       = 10   ///< Divide display flow units by this → cfs
};

// ============================================================================
// Conversion factor tables — identical to legacy Ucf[10][2] and Qcf[6]
// ============================================================================

/// Ucf[quantity][unit_system] — multiply project units by 1/Ucf to get internal
/// Or equivalently: internal = project / UCF; project = internal * UCF
static constexpr double Ucf[10][2] = {
    //  US           SI
    {43200.0,    1097280.0 },   // RAINFALL: in/hr, mm/hr → ft/sec
    {12.0,       304.8     },   // RAINDEPTH: in, mm → ft
    {1036800.0,  26334720.0},   // EVAPRATE: in/day, mm/day → ft/sec
    {1.0,        0.3048    },   // LENGTH: ft, m → ft
    {2.2956e-5,  0.92903e-5},   // LANDAREA: ac, ha → ft²
    {1.0,        0.02832   },   // VOLUME: ft³, m³ → ft³
    {1.0,        1.608     },   // WINDSPEED: mph, km/hr → mph
    {1.0,        1.8       },   // TEMPERATURE: °F, °C → °F
    {2.203e-6,   1.0e-6    },   // MASS: lb, kg → mg
    {43560.0,    3048.0    }    // GWFLOW: cfs/ac, cms/ha → ft/sec
};

/// Qcf[flow_units] — conversion from display flow to cfs
static constexpr double Qcf[6] = {
    1.0,       // CFS → cfs
    448.831,   // GPM → cfs
    0.64632,   // MGD → cfs
    0.02832,   // CMS → cfs
    28.317,    // LPS → cfs
    2.4466     // MLD → cfs
};

// ============================================================================
// Precomputed reciprocal tables — multiply instead of divide
// ============================================================================
//
// Display → internal conversion is `internal = display / Ucf` everywhere. To
// let hot/peripheral conversion sites multiply by a hoisted loop-invariant
// instead of dividing, expose the compile-time reciprocals. These are derived
// from Ucf/Qcf so they can never drift out of sync.
//
// Convention:  internal = display * Ucf_inv[q][us]   (display → internal)
//              display  = internal * Ucf[q][us]       (internal → display)

namespace detail {
inline constexpr std::array<std::array<double, 2>, 10> makeUcfInv() {
    std::array<std::array<double, 2>, 10> r{};
    for (int q = 0; q < 10; ++q)
        for (int u = 0; u < 2; ++u)
            r[static_cast<std::size_t>(q)][static_cast<std::size_t>(u)] =
                1.0 / Ucf[q][u];
    return r;
}
inline constexpr std::array<double, 6> makeQcfInv() {
    std::array<double, 6> r{};
    for (int f = 0; f < 6; ++f) r[static_cast<std::size_t>(f)] = 1.0 / Qcf[f];
    return r;
}
} // namespace detail

/// Reciprocal of Ucf[quantity][unit_system] — multiply display by this → internal.
inline constexpr std::array<std::array<double, 2>, 10> Ucf_inv = detail::makeUcfInv();

/// Reciprocal of Qcf[flow_units] — multiply display flow by this → cfs.
inline constexpr std::array<double, 6> Qcf_inv = detail::makeQcfInv();

// ============================================================================
// Convenience constants — common fixed conversions not unit-system dependent
// ============================================================================

static constexpr double ACRES_TO_FT2 = 43560.0;     ///< 1 acre = 43560 ft²
static constexpr double FT3_TO_MGAL  = 7.48052e-6;  ///< ft³ → million gallons
static constexpr double SEC_PER_DAY  = 86400.0;     ///< seconds per day

// ============================================================================
// UCF function — matching legacy UCF()
// ============================================================================

/**
 * @brief Get unit conversion factor for a quantity.
 *
 * @details For non-flow quantities, returns Ucf[quantity][unit_system].
 *          For FLOW, returns Qcf[flow_units].
 *
 *          To convert FROM project/display units TO internal units:
 *            internal = display / UCF(quantity, opts)
 *
 *          To convert FROM internal units TO project/display units:
 *            display = internal * UCF(quantity, opts)
 *
 * @param quantity  Quantity code (RAINFALL, LENGTH, FLOW, etc.)
 * @param opts      Simulation options (for unit system and flow units).
 * @returns Conversion factor.
 */
double UCF(int quantity, const SimulationOptions& opts);

/**
 * @brief Reciprocal of UCF — multiply display by this to get internal units.
 *
 * @details `internal = display * UCF_inv(quantity, opts)`. Equivalent to
 *          `1.0 / UCF(quantity, opts)` but uses the precomputed reciprocal
 *          tables (no runtime division). For hot loops, prefer hoisting the
 *          factor (e.g. `Ucf_inv[LENGTH][us]`) out of the loop directly.
 *
 * @param quantity  Quantity code (RAINFALL, LENGTH, FLOW, etc.)
 * @param opts      Simulation options (for unit system and flow units).
 * @returns Reciprocal conversion factor (display → internal).
 */
double UCF_inv(int quantity, const SimulationOptions& opts);

/**
 * @brief Determine unit system (0=US, 1=SI) from flow units.
 *
 * @details CFS/GPM/MGD → US (0). CMS/LPS/MLD → SI (1).
 * @param flow_units  FlowUnits enum value.
 * @returns 0 for US, 1 for SI.
 */
int getUnitSystem(int flow_units);

/// Legacy FlowUnitWords — index by FlowUnits enum (CFS,GPM,MGD,CMS,LPS,MLD).
inline constexpr const char* FlowUnitWords[6] =
    { "CFS", "GPM", "MGD", "CMS", "LPS", "MLD" };

// ============================================================================
// DisplayUnits — single source of truth for internal → display conversion
// ============================================================================
//
// Built once from SimulationOptions via DisplayUnits::from(). Carries the
// internal→display multipliers (display = internal * factor) plus the matching
// unit-label words, so every output / report consumer shares one definition
// instead of re-deriving Ucf / Qcf inline. Mirrors legacy statsrpt.c / report.c
// UCF()-based output and the VolUnitsWords / PondingUnitsWords tables.
struct DisplayUnits {
    int unit_system = 0;            ///< 0 = US, 1 = SI
    int flow_units  = 0;            ///< FlowUnits enum value

    // --- internal → display multipliers (display = internal * factor) ---
    double flow      = 1.0;         ///< cfs  → display flow (Qcf)
    double length    = 1.0;         ///< ft   → ft | m
    double volume    = 1.0;         ///< ft³  → ft³ | m³
    double rainfall  = 1.0;         ///< ft/s → in/hr | mm/hr
    double raindepth = 1.0;         ///< ft   → in | mm (also runoff-depth columns)
    double evaprate  = 1.0;         ///< ft/s → in/day | mm/day
    double landarea  = 1.0;         ///< ft²  → ac | ha
    double mvol      = 1.0;         ///< ft³  → 10^6 gal | 10^6 ltr
    double landvol   = 1.0;         ///< ft³  → acre-ft | hectare-m

    // --- unit-label words (legacy statsrpt.c / report.c) ---
    const char* flow_word        = "CFS";      ///< CFS | CMS | ...
    const char* length_word      = "Feet";     ///< depth / HGL / surcharge column
    const char* depth_word       = "in";       ///< rainfall / runoff depth (in | mm)
    const char* vel_word         = "ft/sec";   ///< link velocity (ft/sec | m/sec)
    const char* mvol_word        = "10^6 gal"; ///< 10^6 gal | 10^6 ltr
    const char* landvol_word     = "acre-feet";///< acre-feet | hectare-m
    const char* storage_vol_word = "1000 ft3"; ///< 1000 ft3 | 1000 m3

    /// Build from simulation options (selects unit system + flow units).
    static DisplayUnits from(const SimulationOptions& opts);

    /// Internal temperature (°F) → display (°F for US, °C for SI).
    double temperature(double tF) const {
        return unit_system == 0 ? tF : (5.0 / 9.0) * (tF - 32.0);
    }
};

} // namespace ucf
} // namespace openswmm

#endif // OPENSWMM_UNIT_CONVERSION_HPP
