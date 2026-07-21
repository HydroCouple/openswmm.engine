/**
 * @file RunoffEnsemble.hpp
 * @brief M-member runoff ensemble with per-member infiltration state.
 *
 * @details Each ensemble member carries an independent copy of the chosen
 *          infiltration model's state, initialised with soil parameters
 *          scaled by the member's LHS multiplier from SoilParameterLHS.
 *
 *          The multiplier is applied once at initialisation time to the
 *          primary hydraulic conductivity parameter:
 *            - Horton:     f0 *= mult[i],  fmin *= mult[i]
 *            - Green-Ampt: Ks *= mult[i]
 *            - Curve Number: CN is scaled via (CN-based S) × mult[i]
 *
 *          After initialisation, each member's state evolves independently
 *          through step().  Member i's infiltration rate is available via
 *          rate(i) after the most recent step() call.
 *
 *          Usage:
 *          @code
 *          UncertaintyEnsemble ens;
 *          ens.n_members = 50;  ens.soil_pert = 0.20;  ens.generate();
 *          SoilParameterLHS lhs;  lhs.generate(ens);
 *
 *          RunoffEnsemble re;
 *          re.initialize(lhs.members(), lhs.soil_mult);
 *          re.initGreenAmpt(S_in, Ks_in_hr, IMD, opts);
 *
 *          for (int t = 0; t < n_steps; ++t) {
 *              re.step(precip, depth, dt, InfilModel::GREEN_AMPT);
 *              // re.rate(i) → member i's infiltration rate this step
 *          }
 *          @endcode
 *
 * @ingroup engine_uncertainty
 */

#ifndef OPENSWMM_ENGINE_UNCERTAINTY_RUNOFF_ENSEMBLE_HPP
#define OPENSWMM_ENGINE_UNCERTAINTY_RUNOFF_ENSEMBLE_HPP

#include "hydrology/Infiltration.hpp"
#include <vector>

namespace openswmm {
struct SimulationOptions;
}

namespace openswmm::uncertainty {

/**
 * @brief M-member runoff ensemble.
 *
 * Supports Horton, Green-Ampt (standard and modified), and SCS Curve Number
 * infiltration models.  Only one model is active per RunoffEnsemble instance.
 */
struct RunoffEnsemble {

    // ------------------------------------------------------------------
    // State
    // ------------------------------------------------------------------

    int n_members = 0;

    /// Per-member soil multiplier (copy of SoilParameterLHS::soil_mult).
    std::vector<double> soil_mult;

    /// Per-member Horton states (active when initHorton() was called).
    std::vector<HortonState>    horton_states;

    /// Per-member Green-Ampt states (active when initGreenAmpt() was called).
    std::vector<GreenAmptState> ga_states;

    /// Per-member SCS CN states (active when initCurveNum() was called).
    std::vector<CurveNumState>  cn_states;

    /// Current per-member infiltration rates (ft/sec), set by step().
    std::vector<double> infil_rates;

    // ------------------------------------------------------------------
    // Setup
    // ------------------------------------------------------------------

    /**
     * @brief Set ensemble size and soil multipliers.
     *
     * Must be called before any initXxx() or step() call.
     *
     * @param M            Number of members.
     * @param soil_mult_in Multiplier vector of length M (typically from
     *                     SoilParameterLHS::soil_mult).
     */
    void initialize(int M, const std::vector<double>& soil_mult_in);

    /**
     * @brief Initialise M Horton states with member-scaled f0 and fmin.
     *
     * Parameters are in the same units as infil::horton_init() expects
     * (in/hr or mm/hr depending on opts.flow_units).
     *
     * @param f0     Max infiltration rate.
     * @param fmin   Min infiltration rate.
     * @param decay  Decay constant (1/hr).
     * @param regen  Regeneration constant (days; converted internally).
     * @param Fmax   Max cumulative infiltration (0 = unlimited).
     * @param opts   Simulation options (for unit conversion).
     */
    void initHorton(double f0, double fmin, double decay, double regen,
                    double Fmax, const SimulationOptions& opts);

    /**
     * @brief Initialise M Green-Ampt states with member-scaled Ks.
     *
     * @param S      Capillary suction head (in or mm).
     * @param Ks     Saturated hydraulic conductivity (in/hr or mm/hr).
     * @param IMDmax Initial moisture deficit (0–1).
     * @param opts   Simulation options.
     */
    void initGreenAmpt(double S, double Ks, double IMDmax,
                       const SimulationOptions& opts);

    /**
     * @brief Initialise M Curve Number states with member-scaled retention.
     *
     * The CN-derived retention S = (1000/CN - 10)/12 ft is scaled by
     * 1/mult[i] so that higher mult → lower S → more runoff (consistent
     * with higher conductivity meaning less runoff).
     *
     * @param CN    SCS Curve Number (1–100).
     * @param regen Regeneration rate constant (1/sec).
     */
    void initCurveNum(double CN, double regen);

    // ------------------------------------------------------------------
    // Advance
    // ------------------------------------------------------------------

    /**
     * @brief Advance all members one timestep.
     *
     * Calls the appropriate infil::xxx_getInfil() for each member
     * and stores results in infil_rates[i].
     *
     * @param precip Rainfall rate (ft/sec).
     * @param depth  Ponded depth (ft).
     * @param dt     Timestep (seconds).
     * @param model  Which model to use (must match the initXxx() call).
     */
    void step(double precip, double depth, double dt, InfilModel model);

    // ------------------------------------------------------------------
    // Accessors
    // ------------------------------------------------------------------

    /// Infiltration rate for member i after the most recent step() call (ft/sec).
    double rate(int i) const noexcept {
        return infil_rates[static_cast<std::size_t>(i)];
    }

    /// Number of active ensemble members.
    int members() const noexcept { return n_members; }

    /// True after initialize() has been called.
    bool is_ready() const noexcept { return n_members > 0; }
};

} // namespace openswmm::uncertainty

#endif // OPENSWMM_ENGINE_UNCERTAINTY_RUNOFF_ENSEMBLE_HPP
