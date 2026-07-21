/**
 * @file SoilParameterLHS.hpp
 * @brief Typed adapter that exposes the soil-parameter LHS column from
 *        a shared UncertaintyEnsemble to runoff-layer consumers.
 *
 * @details Phase 3 scope: captures the shuffled soil multipliers generated
 *          by UncertaintyEnsemble::generate() and provides typed helpers
 *          that scale base soil parameters for each ensemble member.
 *
 *          Usage:
 *          @code
 *          UncertaintyEnsemble ens;
 *          ens.soil_pert = 0.20;
 *          ens.generate();
 *
 *          SoilParameterLHS lhs;
 *          lhs.generate(ens);            // copies soil_mult from ens
 *
 *          RunoffEnsemble re;
 *          re.initialize(lhs.members(), lhs.soil_mult);
 *          re.initGreenAmpt(S, Ks, IMD, opts);
 *          @endcode
 *
 * @ingroup engine_uncertainty
 */

#ifndef OPENSWMM_ENGINE_UNCERTAINTY_SOIL_PARAMETER_LHS_HPP
#define OPENSWMM_ENGINE_UNCERTAINTY_SOIL_PARAMETER_LHS_HPP

#include "UncertaintyEnsemble.hpp"
#include <vector>

namespace openswmm::uncertainty {

/**
 * @brief Soil-parameter LHS column extracted from a shared ensemble.
 *
 * A single multiplicative perturbation is applied uniformly to the primary
 * hydraulic conductivity parameter for each infiltration model:
 *   - Horton:      scales f0 and fmin by mult[i]
 *   - Green-Ampt:  scales Ks by mult[i]
 *   - Curve Number: scales the effective CN multiplier by mult[i]
 *
 * mult[i] ∈ [1 - soil_pert, 1 + soil_pert] (LHS, shuffled ordering).
 */
struct SoilParameterLHS {

    /// Soil multiplier per ensemble member — length n_members.
    /// Populated by generate(); empty before generate() is called.
    std::vector<double> soil_mult;

    // ------------------------------------------------------------------
    // Methods
    // ------------------------------------------------------------------

    /**
     * @brief Copy the soil-parameter column from a generated ensemble.
     *
     * The ensemble must have had generate() called before this.
     * Repeated calls replace the previous column.
     *
     * @param ens  A fully generated UncertaintyEnsemble.
     */
    void generate(const UncertaintyEnsemble& ens) {
        soil_mult = ens.soil_mult;
    }

    /// Number of members (matches the source ensemble's n_members).
    int members() const noexcept {
        return static_cast<int>(soil_mult.size());
    }

    /// True after generate() has been called successfully.
    bool is_ready() const noexcept { return !soil_mult.empty(); }

    /// Per-member multiplier for member i.
    double mult(int i) const noexcept {
        return soil_mult[static_cast<std::size_t>(i)];
    }
};

} // namespace openswmm::uncertainty

#endif // OPENSWMM_ENGINE_UNCERTAINTY_SOIL_PARAMETER_LHS_HPP
