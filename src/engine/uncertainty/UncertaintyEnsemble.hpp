/**
 * @file UncertaintyEnsemble.hpp
 * @brief Single authoritative owner of ensemble member count, seed, and
 *        scalar parameter samples for all uncertainty layers.
 *
 * @details `UncertaintyEnsemble` is the long-term owner of:
 *   - member count M (all layers share the same M)
 *   - the random seed that drives reproducibility
 *   - scalar LHS sample columns for each registered parameter
 *
 * Consumers (currently `SpectralROM`) call `manningsSamples2D()` /
 * `rainfallSamples2D()` to obtain read-only views of the pre-generated
 * design, rather than building their own internal LHS.
 *
 * Phase 0/1 scope: scalar 2D Manning's n and rainfall multipliers only.
 * The spatial-field path, 1D ensemble, and Iman-Conover correlation are
 * deferred to later phases.
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
    // State (set by generate())
    // ------------------------------------------------------------------

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
     * @brief Build a deterministic Latin-hypercube design.
     *
     * Stratifies [1-p, 1+p] into n_members strata (midpoints).
     * Manning and rainfall columns are stratifications; rainfall order is
     * reversed relative to Manning, giving rank correlation exactly −1
     * (comonotone-opposite), NOT independence. Acceptable only while a single
     * parameter is perturbed at a time; PR 5 replaces this with independent
     * shuffled columns.
     *
     * Calling generate() again (e.g. on reseed) replaces the previous
     * design; callers that hold pointers to the vectors must re-query
     * the accessors.
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
