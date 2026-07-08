/**
 * @file UncertaintyEnsemble.hpp
 * @brief Single authoritative owner of ensemble member count, seed, and
 *        scalar parameter samples for all uncertainty layers.
 *
 * @details `UncertaintyEnsemble` is the long-term owner of:
 *   - member count M (all layers share the same M)
 *   - the random seed that drives reproducibility
 *   - the parameter registry (PR 9): a list of `RegisteredParam` entries,
 *     each carrying its own decorrelated LHS column after `generate()`
 *
 * Any scalar parameter can be registered via `registerParam()` (name, layer,
 * ODE-entry taxonomy, distribution, perturbation) — see
 * docs/uncertainty/PARAMETER_REGISTRY.md. The four legacy parameters
 * (2D Manning, 2D rainfall, soil, Cd) are auto-registered by `generate()`
 * when nothing else has been registered, with columns bit-identical to the
 * pre-registry design; the legacy vector fields and accessors are preserved
 * as copies of those columns.
 *
 * Iman-Conover rank-correlation control is a documented non-goal
 * (PARAMETER_REGISTRY.md §7).
 *
 * @ingroup engine_uncertainty
 */

#ifndef OPENSWMM_ENGINE_UNCERTAINTY_ENSEMBLE_HPP
#define OPENSWMM_ENGINE_UNCERTAINTY_ENSEMBLE_HPP

#include "UncertaintyTypes.hpp"
#include <vector>
#include <cstdint>
#include <cstddef>

namespace openswmm::uncertainty {

/**
 * @brief Shared ensemble owner.
 *
 * Lifecycle:
 *   1. Set `n_members`, `seed`, and any perturbation parameters.
 *   2. Call `generate()` — builds the LHS design for all registered parameters.
 *   3. Pass `manningsSamples2D()` / `rainfallSamples2D()` to `SpectralROM`.
 *   4. On reseed, call `generate()` again (seed incremented if desired).
 */
struct UncertaintyEnsemble {

    // ------------------------------------------------------------------
    // Configuration (set before generate())
    // ------------------------------------------------------------------

    int      n_members         = 50;    ///< Number of ensemble members M (shared).
    uint64_t seed              = 42;    ///< Deterministic seed for reproducibility.

    double   mannings_pert_2d  = 0.20;  ///< Half-range: n ∈ [1-p, 1+p] × base.
    double   rainfall_pert_2d  = 0.20;  ///< Half-range: r ∈ [1-p, 1+p] × base.
    double   soil_pert         = 0.20;  ///< Half-range: Ks/f0 ∈ [1-p, 1+p] × base (runoff layer).
    double   cd_pert           = 0.10;  ///< Half-range: Cd ∈ [1-p, 1+p] × nominal (coupling layer).

    // ------------------------------------------------------------------
    // Parameter registry (PR 9)
    // ------------------------------------------------------------------

    /// All registered parameters in registration order. Columns are filled
    /// by generate(). Public for testability, like the rest of this struct.
    std::vector<RegisteredParam> params;

    // ------------------------------------------------------------------
    // State (set by generate())
    // ------------------------------------------------------------------
    // Legacy back-compat vectors — copies of the corresponding registered
    // columns (MANNINGS_N/2D, RAINFALL/2D, SOIL/RUNOFF, CD/2D). Preserved so
    // every pre-registry consumer and accessor keeps working unchanged.

    /// Manning's n multiplier per member — length n_members.
    std::vector<double> mannings_mult_2d;
    /// Rainfall multiplier per member — length n_members.
    std::vector<double> rainfall_mult_2d;
    /// Soil hydraulic conductivity multiplier per member — length n_members.
    /// Decorrelated from Manning and rainfall via shuffled LHS strata.
    std::vector<double> soil_mult;
    /// Discharge coefficient multiplier per member — length n_members.
    /// Decorrelated from Manning, rainfall, and soil via independently shuffled strata.
    std::vector<double> cd_mult;

    // ------------------------------------------------------------------
    // Methods
    // ------------------------------------------------------------------

    /**
     * @brief Register an uncertain parameter for column generation.
     *
     * Assigns the next free seed_offset (registration order determines the
     * shuffle seed, so registration order is part of reproducibility).
     * Call before generate(). Re-registering an existing (name, layer) pair
     * updates that entry in place instead of duplicating it.
     *
     * @return Reference to the stored entry (column filled by generate()).
     */
    RegisteredParam& registerParam(const std::string& name, LayerTarget layer,
                                   ParamEntry entry, DistType dist, double pert);

    /**
     * @brief Register the four legacy parameters from the legacy pert fields.
     *
     * MANNINGS_N/2D (RATE_MULT, ascending reference column), RAINFALL/2D
     * (FORCING_MULT, seed+1), SOIL/RUNOFF (RATE_MULT, seed+2), CD/2D
     * (COUPLING_MULT, seed+3) — all UNIFORM. With these registrations,
     * generate() reproduces the pre-registry columns bit-exactly.
     * Called automatically by generate() when no parameter has been
     * registered yet; call it explicitly first when mixing legacy and
     * additional registered parameters.
     */
    void registerDefaults();

    /// Look up a registered parameter's sample column by (name, layer).
    /// LayerTarget::NONE matches any layer. Returns nullptr if absent or
    /// generate() has not run.
    const std::vector<double>* column(const std::string& name,
                                      LayerTarget layer = LayerTarget::NONE) const noexcept;

    /**
     * @brief Build the deterministic Latin-hypercube design for every
     *        registered parameter.
     *
     * If nothing has been registered, registerDefaults() runs first (legacy
     * behavior). Each column stratifies [0,1) into n_members strata
     * (midpoints): the reference column ascending, every other column an
     * independent Fisher-Yates shuffle seeded by `seed + seed_offset` — LHS
     * coverage is exactly preserved per column while every pair has
     * near-zero rank correlation (expected |rho| ~ 1/sqrt(n_members-1)).
     * Strata map to multipliers through each parameter's distribution
     * (invCdfMultiplier: UNIFORM band, ±3σ-truncated NORMAL, median-1
     * LOGNORMAL — see PARAMETER_REGISTRY.md §3).
     *
     * Calling generate() again replaces all columns; callers that hold
     * pointers must re-query.
     *
     * @throws std::invalid_argument if n_members < 2.
     */
    void generate();

    /// True after generate() has been called successfully.
    bool is_ready() const noexcept { return !mannings_mult_2d.empty(); }

    /// Const view of Manning's n multipliers (length n_members).
    const std::vector<double>& manningsSamples2D() const noexcept {
        return mannings_mult_2d;
    }

    /// Const view of rainfall multipliers (length n_members).
    const std::vector<double>& rainfallSamples2D() const noexcept {
        return rainfall_mult_2d;
    }

    /// Const view of soil hydraulic conductivity multipliers (length n_members).
    /// Decorrelated from Manning and rainfall; use for runoff-layer LHS design.
    const std::vector<double>& soilSamples() const noexcept {
        return soil_mult;
    }

    /// Const view of discharge coefficient multipliers (length n_members).
    /// Decorrelated from Manning, rainfall, and soil; use for coupling-layer LHS design.
    const std::vector<double>& cdSamples() const noexcept {
        return cd_mult;
    }
};

} // namespace openswmm::uncertainty

#endif // OPENSWMM_ENGINE_UNCERTAINTY_ENSEMBLE_HPP
