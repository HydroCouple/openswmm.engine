/**
 * @file CorrelatedFieldGenerator.hpp
 * @brief Spatially-correlated multiplier field generation for 2D uncertainty.
 *
 * @details Generates M spatially-correlated lognormal-like multiplier fields
 *          over an unstructured triangular mesh for use in ensemble uncertainty
 *          propagation.
 *
 *          Algorithm:
 *          1. Build a grid-based spatial index for O(n_tri · avg_neighbors)
 *             neighbourhood queries.
 *          2. For each ensemble member i, draw n_tri i.i.d. N(0,1) samples.
 *          3. Smooth each sample field using an exponential kernel of width
 *             `corr_len`: z_smooth[t] = Σ_{s∈Nbhd(t)} exp(−d_ts/ell) · z[s]
 *                                        ÷  Σ exp(−d_ts/ell)
 *          4. Standardise the smoothed field (zero mean, unit variance across cells).
 *          5. Map to multiplier: W[i][t] = clamp(1 + pert · z_std[t], ε, 2−ε).
 *
 *          When `corr_len == 0`, the spatial path is skipped and the field is
 *          populated from the provided scalar per-member multiplier vector
 *          (W[i][t] = scalar_mult[i] for all t).
 *
 * @ingroup engine_2d
 */

#ifndef OPENSWMM_ENGINE_2D_CORRELATED_FIELD_GENERATOR_HPP
#define OPENSWMM_ENGINE_2D_CORRELATED_FIELD_GENERATOR_HPP

#ifdef OPENSWMM_HAS_2D

#include "SpatialUncertaintyField.hpp"
#include <vector>
#include <cstdint>

namespace openswmm::twoD {

/**
 * @brief Generates M spatially-correlated uncertainty multiplier fields.
 */
class CorrelatedFieldGenerator {
public:
    /**
     * @brief Generate spatially-correlated multiplier fields.
     *
     * @param cx          Triangle centroid X coordinates (length n_tri).
     * @param cy          Triangle centroid Y coordinates (length n_tri).
     * @param n_tri       Number of triangles.
     * @param scalar_mult Per-member scalar multipliers from LHS design
     *                    (length == out.n_members).  Used directly when
     *                    corr_len == 0; otherwise used as the global member
     *                    level around which spatial variation is added.
     * @param pert        Spatial perturbation half-range (dimensionless ≥ 0).
     *                    When 0, all values = scalar_mult[i].
     * @param corr_len    Exponential correlation length (m).
     *                    When 0: scalar mode (W[i][t] = scalar_mult[i]).
     *                    When >0: spatial correlation applied.
     * @param seed        Deterministic reproducibility seed.
     * @param out         Output SpatialUncertaintyField (allocated by this call).
     */
    static void generate(const double* cx, const double* cy, int n_tri,
                         const std::vector<double>& scalar_mult,
                         double pert, double corr_len, uint64_t seed,
                         SpatialUncertaintyField& out);

private:
    /// Build neighbourhood lists: for each cell t, indices of cells within
    /// `radius` metres (inclusive of self).
    static std::vector<std::vector<int>> buildNeighbourhood(
        const double* cx, const double* cy, int n_tri, double radius);

    /// Splitmix64 PRNG: fast, reproducible, no stdlib dependency.
    static uint64_t splitmix64(uint64_t& state) noexcept;

    /// Draw standard normal sample via Box-Muller from U[0,1) uniform pairs.
    static double boxMuller(uint64_t& state) noexcept;
};

} // namespace openswmm::twoD

#endif // OPENSWMM_HAS_2D
#endif // OPENSWMM_ENGINE_2D_CORRELATED_FIELD_GENERATOR_HPP
