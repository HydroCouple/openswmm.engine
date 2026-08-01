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
#include <vector>

namespace openswmm::uncertainty {

// ============================================================================
// LayerTarget — which simulation layer carries the uncertainty
// ============================================================================

enum class LayerTarget : int8_t {
    NONE   = 0,  ///< No layer (unset / invalid)
    TWO_D  = 1,  ///< 2D surface routing
    ONE_D  = 2,  ///< 1D sewer / network routing
    RUNOFF = 3,  ///< Subcatchment runoff
    QUALITY = 4, ///< Water quality decay rate uncertainty
};

// ============================================================================
// DistType — probability distribution family for a parameter multiplier
// ============================================================================
//
// Consumed by the parameter registry (PR 9; see PARAMETER_REGISTRY.md §3).
// All families share the same meaning of the perturbation p: it is the
// half-range of the multiplier band. Sampling maps LHS strata midpoints
// through each family's inverse CDF (invCdfMultiplier in LhsShuffle.hpp).

enum class DistType : int8_t {
    UNIFORM  = 0,  ///< θ ∈ [1−p, 1+p], equal mass (default)
    NORMAL   = 1,  ///< ±3σ-truncated normal: mean 1, σ = p/3, hard band [1−p, 1+p]
    LOGNORMAL = 2, ///< median 1, σ_log = ln(1+p)/1.6449 → q95 = 1+p, q05 = 1/(1+p)
};

// ============================================================================
// ParamEntry — how a scalar parameter enters the deviation-form modal ODE
// ============================================================================
//
// See PARAMETER_REGISTRY.md §2. The deviation-form member ODE is
// d(δa_ij)/dt = −rate_ij·δa_ij + g_ij; every scalar parameter shapes rate
// and/or g in exactly one of these ways.

enum class ParamEntry : int8_t {
    RATE_MULT      = 0,  ///< Divides the decay rate; sensitivity −λ·K·(1/θ−1)·b_j. Prototype: Manning's n.
    FORCING_MULT   = 1,  ///< Scales the ROM's forcing projection; sensitivity (θ−1)·r_j. Prototype: rainfall.
    FORCING_VECTOR = 2,  ///< Scales a registered per-node field v; sensitivity (θ−1)·(Pᵀv)_j. Prototype: inflow/DWF.
    COUPLING_MULT  = 3,  ///< Scales the 2D↔1D orifice exchange flux (outside the modal ODE). Prototype: Cd.
    QUALITY_MULT   = 4,  ///< Scales the water quality decay rate; sensitivity affects QUALITY layer. Prototype: decay rate uncertainty.
};

// ============================================================================
// RegisteredParam — one registry entry (PR 9; owned by UncertaintyEnsemble)
// ============================================================================

/**
 * @brief A registered uncertain parameter: identity, ODE-entry taxonomy,
 *        distribution, and (after generate()) its per-member sample column.
 *
 * `seed_offset` decorrelates columns: the column's shuffle seed is
 * `ensemble.seed + seed_offset`. `reference_column = true` means ascending
 * strata with no shuffle (the Manning prototype — the reference column all
 * other rank correlations are judged against).
 */
struct RegisteredParam {
    std::string  name;                       ///< e.g. "MANNINGS_N", "INFLOW", user-defined
    LayerTarget  layer = LayerTarget::NONE;
    ParamEntry   entry = ParamEntry::FORCING_MULT;
    DistType     dist  = DistType::UNIFORM;
    double       pert  = 0.0;                ///< Half-range p of the multiplier band (§3)
    uint64_t     seed_offset = 0;            ///< Column seed = ensemble seed + seed_offset
    bool         reference_column = false;   ///< Ascending strata (no shuffle)
    std::vector<double> column;              ///< Filled by generate(); length n_members

    bool is_active() const noexcept { return pert > 0.0 && layer != LayerTarget::NONE; }
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
    /// How the parameter enters the modal ODE (PR 9c). Defaulted from the
    /// name by the parser (MANNINGS_N→RATE_MULT, RAINFALL→FORCING_MULT,
    /// INFLOW→FORCING_VECTOR) or given explicitly for user-defined names.
    ParamEntry   entry  = ParamEntry::FORCING_MULT;

    bool is_active() const noexcept { return perturbation > 0.0 && layer != LayerTarget::NONE; }
};

} // namespace openswmm::uncertainty

#endif // OPENSWMM_ENGINE_UNCERTAINTY_TYPES_HPP
