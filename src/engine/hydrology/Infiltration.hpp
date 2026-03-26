/**
 * @file Infiltration.hpp
 * @brief Infiltration models — Horton, Green-Ampt, SCS Curve Number.
 *
 * @details All three models are implemented with exact numerical parity
 *          to legacy infil.c. Each model maintains per-subcatchment state
 *          and computes infiltration rate given current precipitation,
 *          ponded depth, and timestep.
 *
 * @note Legacy reference: src/legacy/engine/infil.c
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_INFILTRATION_HPP
#define OPENSWMM_INFILTRATION_HPP

namespace openswmm {

struct SimulationOptions;

// ============================================================================
// Infiltration model type
// ============================================================================

enum class InfilModel : int {
    HORTON     = 0,
    GREEN_AMPT = 1,
    CURVE_NUM  = 2
};

// ============================================================================
// Horton infiltration state (per subcatchment)
// ============================================================================

struct HortonState {
    double f0      = 0.0;   ///< Initial infiltration rate (ft/sec)
    double fmin    = 0.0;   ///< Minimum (ultimate) rate (ft/sec)
    double decay   = 0.0;   ///< Decay constant k (1/sec)
    double regen   = 0.0;   ///< Regeneration constant kr (1/sec)
    double Fmax    = 0.0;   ///< Max cumulative infiltration (ft)
    double tp      = 0.0;   ///< Cumulative infiltration time (sec)
    double Fe      = 0.0;   ///< Cumulative excess infiltration (ft)
};

// ============================================================================
// Green-Ampt state (per subcatchment)
// ============================================================================

struct GreenAmptState {
    double S       = 0.0;   ///< Capillary suction head (ft)
    double Ks      = 0.0;   ///< Saturated hydraulic conductivity (ft/sec)
    double IMDmax  = 0.0;   ///< Maximum initial moisture deficit (0-1)
    double IMD     = 0.0;   ///< Current moisture deficit (0-1)
    double F       = 0.0;   ///< Cumulative infiltration (ft)
    double Fu      = 0.0;   ///< Upper zone saturation volume (ft)
    double Fumax   = 0.0;   ///< Max upper zone saturation (ft)
    double Lu      = 0.0;   ///< Upper zone depth (ft)
    bool   saturated = false; ///< True when surface is saturated
};

// ============================================================================
// SCS Curve Number state (per subcatchment)
// ============================================================================

struct CurveNumState {
    double Smax    = 0.0;   ///< Max retention S = (1000/CN - 10)/12 (ft)
    double S       = 0.0;   ///< Current retention (ft)
    double P       = 0.0;   ///< Cumulative precipitation (ft)
    double F       = 0.0;   ///< Cumulative infiltration (ft)
    double regen   = 0.0;   ///< Regeneration rate
    double T       = 0.0;   ///< Time since last rainfall (sec)
    double Tmax    = 0.0;   ///< Inter-event time (sec)
};

// ============================================================================
// Infiltration functions
// ============================================================================

namespace infil {

/**
 * @brief Compute Horton infiltration rate for one timestep.
 *
 * @param state   [in/out] Horton state for this subcatchment.
 * @param precip  Rainfall rate (ft/sec).
 * @param depth   Ponded depth (ft).
 * @param dt      Timestep (seconds).
 * @returns Infiltration rate (ft/sec).
 */
double horton_getInfil(HortonState& state, double precip, double depth, double dt);

/**
 * @brief Compute Green-Ampt infiltration rate.
 *
 * @param state   [in/out] Green-Ampt state.
 * @param precip  Rainfall rate (ft/sec).
 * @param depth   Ponded depth (ft).
 * @param dt      Timestep (seconds).
 * @returns Infiltration rate (ft/sec).
 */
double grnampt_getInfil(GreenAmptState& state, double precip, double depth, double dt);

/**
 * @brief Compute SCS Curve Number infiltration rate.
 *
 * @param state   [in/out] CN state.
 * @param precip  Rainfall rate (ft/sec).
 * @param depth   Ponded depth (ft).
 * @param dt      Timestep (seconds).
 * @returns Infiltration rate (ft/sec).
 */
double curvenum_getInfil(CurveNumState& state, double precip, double depth, double dt);

/**
 * @brief Initialise Horton parameters from user input.
 *
 * @param state  [out] State to initialise.
 * @param f0     Max infiltration rate (in/hr or mm/hr — converted internally).
 * @param fmin   Min infiltration rate.
 * @param decay  Decay constant (1/hr).
 * @param regen  Regeneration constant (1/hr).
 * @param Fmax   Max cumulative infiltration (0 = unlimited).
 * @param opts   Simulation options (for unit system).
 */
void horton_init(HortonState& state, double f0, double fmin,
                 double decay, double regen, double Fmax,
                 const SimulationOptions& opts);

/**
 * @brief Initialise Green-Ampt parameters.
 *
 * @param state  [out] State to initialise.
 * @param S      Suction head (in or mm).
 * @param Ks     Hydraulic conductivity (in/hr or mm/hr).
 * @param IMD    Initial moisture deficit (0-1).
 * @param opts   Simulation options (for unit system).
 */
void grnampt_init(GreenAmptState& state, double S, double Ks, double IMD,
                  const SimulationOptions& opts);

/**
 * @brief Initialise Curve Number parameters.
 *
 * @param state  [out] State to initialise.
 * @param CN     SCS Curve Number (1-100).
 * @param regen  Regeneration rate.
 */
void curvenum_init(CurveNumState& state, double CN, double regen);

} // namespace infil
} // namespace openswmm

#endif // OPENSWMM_INFILTRATION_HPP
