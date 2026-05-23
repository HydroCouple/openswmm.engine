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
 * @license  MIT License
 */

#ifndef OPENSWMM_UNIT_CONVERSION_HPP
#define OPENSWMM_UNIT_CONVERSION_HPP

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
 * @brief Determine unit system (0=US, 1=SI) from flow units.
 *
 * @details CFS/GPM/MGD → US (0). CMS/LPS/MLD → SI (1).
 * @param flow_units  FlowUnits enum value.
 * @returns 0 for US, 1 for SI.
 */
int getUnitSystem(int flow_units);

} // namespace ucf
} // namespace openswmm

#endif // OPENSWMM_UNIT_CONVERSION_HPP
