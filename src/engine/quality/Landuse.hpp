/**
 * @file Landuse.hpp
 * @brief Pollutant buildup and washoff per land use — batch SoA.
 *
 * @details Each subcatchment can have multiple land uses, each with per-
 *          pollutant buildup and washoff functions. The buildup/washoff
 *          equations are pure arithmetic — vectorisable across all
 *          (subcatchment × landuse × pollutant) triples.
 *
 *          **Buildup models:**
 *            - Power:       B(t) = min(c0, c1 * t^c2)
 *            - Exponential: B(t) = c0 * (1 - exp(-c1 * t))
 *            - Saturation:  B(t) = c0 * t / (c2 + t)
 *
 *          **Washoff models:**
 *            - Exponential: W = k * Q^n * B
 *            - Rating curve: W = k * Q^n
 *            - EMC: W = constant concentration
 *
 *          All models are applied identically per element — vectorisable.
 *
 * @note Legacy reference: src/legacy/engine/landuse.c, surfqual.c
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_LANDUSE_HPP
#define OPENSWMM_LANDUSE_HPP

#include <vector>

namespace openswmm {

struct SimulationContext;

namespace landuse {

// ============================================================================
// Buildup/washoff function types
// ============================================================================

enum class BuildupType : int {
    NONE     = 0,
    POWER    = 1,
    EXPON    = 2,
    SATUR    = 3,
    EXTERNAL = 4
};

enum class WashoffType : int {
    NONE    = 0,
    EXPON   = 1,
    RATING  = 2,
    EMC     = 3
};

// ============================================================================
// Per (landuse × pollutant) buildup/washoff parameters — SoA
// ============================================================================

struct BuildupParams {
    BuildupType type = BuildupType::NONE;
    double coeff[3] = {};     ///< c0 (max), c1 (rate), c2 (exponent)
    double max_days = 0.0;    ///< Time to reach max buildup
    int    normalizer = 0;    ///< 0=PER_AREA, 1=PER_CURB
};

struct WashoffParams {
    WashoffType type = WashoffType::NONE;
    double coeff = 0.0;       ///< Washoff coefficient
    double expon = 0.0;       ///< Washoff exponent
    double sweep_effic = 0.0; ///< Street sweep removal [0-1]
    double bmp_effic = 0.0;   ///< BMP removal [0-1]
};

// ============================================================================
// Surface quality SoA (per subcatchment × pollutant)
// ============================================================================

struct SurfaceQualitySoA {
    int n_subcatch = 0;
    int n_landuses = 0;
    int n_pollutants = 0;

    /// Per-landuse buildup: [subcatch * n_landuses * n_pollutants + lu * n_pollutants + p]
    /// Matches legacy Subcatch[j].landFactor[lu].buildup[p] (mass per normalizer unit)
    std::vector<double> buildup;

    /// Washoff concentration [subcatch * n_pollutants + pollutant] (mass/vol)
    std::vector<double> washoff_conc;

    /// Index into buildup array for (subcatch, landuse, pollutant)
    std::size_t bu_idx(int sc, int lu, int p) const {
        return static_cast<std::size_t>(sc) * static_cast<std::size_t>(n_landuses * n_pollutants)
             + static_cast<std::size_t>(lu) * static_cast<std::size_t>(n_pollutants)
             + static_cast<std::size_t>(p);
    }

    void resize(int n_sc, int n_lu, int n_poll);
};

// ============================================================================
// Landuse quality solver
// ============================================================================

class LanduseSolver {
public:
    void init(int n_landuses, int n_pollutants);

    /**
     * @brief Batch compute buildup for all subcatchments.
     *
     * @details For each (subcatch, pollutant):
     *   1. Convert current buildup to equivalent days (inverse function)
     *   2. Add timestep: days += dt / 86400
     *   3. Recompute buildup mass (forward function)
     *   All three steps are vectorisable (pow/exp/division).
     */
    void computeBuildup(SurfaceQualitySoA& sq,
                        const double* area, const double* curb_length,
                        double dt, int n_subcatch);

    /**
     * @brief Batch compute washoff for all subcatchments.
     *
     * @details For each (subcatch, pollutant):
     *   EMC: conc = coeff (constant — trivially vectorisable)
     *   Exponential: conc = k * Q^n * B / (Q * A) (vectorisable)
     *   Rating: conc = k * (Q*A)^(n-1) (vectorisable)
     */
    void computeWashoff(SurfaceQualitySoA& sq,
                        const double* runoff, const double* area,
                        int n_subcatch);

    /**
     * @brief Apply co-pollutant washoff fractions.
     *
     * @details After primary washoff is computed, for each pollutant p with
     *          a co-pollutant k: washoff[p] += co_frac[p] * washoff[k].
     *          Matches legacy landuse_getCoPollutLoad().
     *
     * @param sq           Surface quality SoA (washoff_conc modified in-place).
     * @param runoff       Runoff rate per subcatchment (for mass rate conversion).
     * @param area         Subcatchment area.
     * @param co_pollut    Co-pollutant index per pollutant (-1 = none).
     * @param co_frac      Co-pollutant fraction per pollutant.
     * @param n_subcatch   Number of subcatchments.
     */
    void applyCoPollutant(SurfaceQualitySoA& sq,
                          const double* runoff, const double* area,
                          const int* co_pollut, const double* co_frac,
                          int n_subcatch);

    /// Per (landuse × pollutant) parameters. Index: [lu * n_pollutants + p]
    std::vector<BuildupParams> buildup_params;
    std::vector<WashoffParams> washoff_params;
    int n_landuses_ = 0;
    int n_pollutants_ = 0;
};

} // namespace landuse
} // namespace openswmm

#endif // OPENSWMM_LANDUSE_HPP
