/**
 * @file Constants.hpp
 * @brief Global physical, numerical, and model constants for OpenSWMM Engine.
 *
 * @details Consolidates all constants previously scattered across the engine
 *          into a single header. Organized by category:
 *
 *          - Physical: gravity, viscosity, Manning's factor
 *          - Mathematical: pi
 *          - Numerical: tolerances, thresholds, limits
 *          - DynamicWave solver: under-relaxation, velocity caps, slot widths
 *          - ODE solver: RK45 coefficients
 *          - Unit conversion: see UnitConversion.hpp for Ucf/Qcf tables
 *
 * @note Legacy reference: consts.h, enums.h
 * @ingroup engine_core
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_CONSTANTS_HPP
#define OPENSWMM_CONSTANTS_HPP

namespace openswmm {

// ============================================================================
// Physical constants
// ============================================================================

namespace constants {

/// Gravitational acceleration (ft/s²).
/// @see Legacy: GRAVITY in consts.h
constexpr double GRAVITY       = 32.2;

/// Pi.
constexpr double PI            = 3.14159265358979;

/// Manning's equation US customary unit factor (1.486).
/// Q = (PHI/n) * A * R^(2/3) * S^(1/2) where PHI = 1.486 for US, 1.0 for SI.
/// @see Legacy: PHI in consts.h
constexpr double PHI           = 1.486;

/// Manning's overland flow exponent (5/3).
/// @see Legacy: MEXP in consts.h
constexpr double MEXP          = 5.0 / 3.0;

/// Kinematic viscosity of water at 20°C (ft²/sec).
constexpr double VISCOS        = 1.1e-5;

/// Seconds per day.
constexpr double SEC_PER_DAY   = 86400.0;

/// Feet per inch.
constexpr double FT_PER_IN     = 1.0 / 12.0;

/// Inches per foot.
constexpr double IN_PER_FT     = 12.0;

// ============================================================================
// Numerical thresholds and tolerances
// ============================================================================

/// Minimum depth/area/flow threshold (ft or ft²).
/// Values below this are treated as zero for numerical stability.
/// @see Legacy: FUDGE in dynwave.c
constexpr double FUDGE         = 0.0001;

/// Default minimum surface area (ft²) — approximately a 4-ft diameter manhole.
/// @see Legacy: MIN_SURFAREA in consts.h
constexpr double MIN_SURFAREA  = 12.566;  // 4*pi

/// Minimum elevation drop for conduit slope computation (ft).
constexpr double MIN_DELTA_Z   = 0.001;

/// Tolerance for bounding state variables.
/// @see Legacy: XTOL in gwater.c
constexpr double XTOL          = 0.001;

/// Small positive number for underflow protection.
constexpr double TINY          = 1.0e-30;

// ============================================================================
// Dynamic wave solver constants
// ============================================================================

/// Picard iteration under-relaxation factor.
/// @see Legacy: OMEGA in dynwave.c
constexpr double OMEGA         = 0.5;

/// Default convergence tolerance for node depth change (ft).
/// @see Legacy: HeadTol in globals.h
constexpr double DEFAULT_HEAD_TOL  = 0.005;

/// Default maximum Picard iterations per routing step.
/// @see Legacy: MaxTrials in globals.h
constexpr int    DEFAULT_MAX_TRIALS = 8;

/// Maximum conduit velocity (ft/s) — prevents numerical blowup.
/// @see Legacy: MAXVELOCITY in dynwave.c
constexpr double MAX_VELOCITY  = 50.0;

/// Absolute minimum routing timestep (s).
/// @see Legacy: MINTIMESTEP in dynwave.c
constexpr double MIN_TIMESTEP  = 0.001;

/// EXTRAN surcharge crown cutoff fraction (depth/full_depth).
/// Above this, the Preissmann slot or surcharge algorithm activates.
constexpr double EXTRAN_CROWN_CUTOFF = 0.96;

/// Preissmann slot crown cutoff fraction.
constexpr double SLOT_CROWN_CUTOFF   = 0.985257;

/// Preissmann slot width factor (slot_width = y_full * this factor).
constexpr double SLOT_WIDTH_FACTOR   = 0.001;

// ============================================================================
// ODE solver constants (RK45 Cash-Karp)
// ============================================================================

namespace ode {

/// Maximum number of integration steps.
constexpr int    MAX_STEPS     = 10000;

/// Underflow protection for scaling.
constexpr double ODE_TINY      = 1.0e-30;

/// Safety factor for step size adjustment.
constexpr double SAFETY        = 0.9;

/// Exponent for step increase.
constexpr double PGROW         = -0.2;

/// Exponent for step decrease.
constexpr double PSHRNK        = -0.25;

/// Error control threshold = (5/SAFETY)^(1/PGROW).
constexpr double ERRCON        = 1.89e-4;

/// Groundwater ODE tolerance.
/// @see Legacy: GWTOL in gwater.c
constexpr double GWTOL         = 0.0001;

/// Runoff ODE tolerance.
constexpr double ODETOL        = 0.0001;

} // namespace ode

// ============================================================================
// Hydraulic structure constants
// ============================================================================

/// Minimum orifice/weir flow threshold.
constexpr double MIN_LINK_FLOW = 0.001;

// ============================================================================
// Cross-section constants
// ============================================================================

/// Maximum alpha fraction for rectangular shapes.
constexpr double RECT_ALFMAX   = 0.97;

/// Maximum alpha fraction for trapezoidal shapes.
constexpr double TRAP_ALFMAX   = 0.98;

/// Transect discretization table size.
constexpr int    N_TRANSECT_TBL = 51;

/// Maximum number of cross-section shape types.
constexpr int    MAX_SHAPES    = 26;

// ============================================================================
// Data structure constants
// ============================================================================

/// Number of flow classification bins.
constexpr int    N_FLOW_CLASSES = 7;

/// Maximum past rain hours tracked per gage.
constexpr int    MAX_PAST_RAIN = 48;

/// Number of time step histogram bins.
constexpr int    N_TIME_BINS   = 5;

} // namespace constants
} // namespace openswmm

#endif // OPENSWMM_CONSTANTS_HPP
