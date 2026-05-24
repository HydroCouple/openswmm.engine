/**
 * @file UncertaintyTypes.hpp
 * @brief Shared uncertainty enumeration and source-specification types.
 *
 * @details These types live outside the `2d/` subtree so they can be shared
 *          by the 2D ROM, the future 1D ROM, and any other uncertainty
 *          consumer without creating a cross-module dependency on `2d/`.
 *
 * @ingroup engine_uncertainty
 */

#ifndef OPENSWMM_ENGINE_UNCERTAINTY_TYPES_HPP
#define OPENSWMM_ENGINE_UNCERTAINTY_TYPES_HPP

#include <cstdint>
#include <string>

namespace openswmm::uncertainty {

// ============================================================================
// LayerTarget — which simulation layer carries the uncertainty
// ============================================================================

enum class LayerTarget : int8_t {
    NONE   = 0,  ///< No layer (unset / invalid)
    TWO_D  = 1,  ///< 2D surface routing
    ONE_D  = 2,  ///< 1D sewer / network routing
    RUNOFF = 3,  ///< Subcatchment runoff
};

// ============================================================================
// DistType — probability distribution family for a parameter
// ============================================================================

enum class DistType : int8_t {
    UNIFORM  = 0,  ///< U[1-p, 1+p] multiplicative (default)
    NORMAL   = 1,  ///< N(1, p²) multiplicative (truncated at 0)
    LOGNORMAL = 2, ///< Lognormal with sigma = p (multiplicative)
};

// ============================================================================
// UncertaintySourceSpec — describes one uncertain input parameter
// ============================================================================

/**
 * @brief Specification for a single uncertain model parameter.
 *
 * The `perturbation` field is a dimensionless half-range (for UNIFORM) or
 * coefficient-of-variation (for NORMAL / LOGNORMAL).  The actual parameter
 * value used by a given ensemble member is `base × mult`, where `mult` is
 * drawn from the design described by this struct.
 */
struct UncertaintySourceSpec {
    std::string  name;            ///< Human-readable parameter name (e.g. "MANNINGS_N")
    LayerTarget  layer  = LayerTarget::NONE;
    DistType     dist   = DistType::UNIFORM;
    double       perturbation = 0.0;  ///< Half-range / CV (dimensionless, ≥ 0)

    bool is_active() const noexcept { return perturbation > 0.0 && layer != LayerTarget::NONE; }
};

} // namespace openswmm::uncertainty

#endif // OPENSWMM_ENGINE_UNCERTAINTY_TYPES_HPP
