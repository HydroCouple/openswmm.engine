/**
 * @file Snow.hpp
 * @brief Snowmelt — degree-day and rain-on-snow methods.
 *
 * @details Batch-oriented: all snowpack computations use the same daily
 *          temperature/wind. The per-subcatchment snowmelt calculation is
 *          independent and vectorisable. Three subareas (plowable, imperv,
 *          perv) are processed per subcatchment — inner loop can be unrolled.
 *
 *          Key vectorisable operations:
 *          - ATI update: ati += tipm * (Ta - ati) → batch over subcatchments
 *          - Degree-day melt: smelt = dhm * (Ta - Tbase) → batch over subcatchments
 *          - Cold content update: cc += rnm * dhm * (ati - Ta) * dt → batch
 *          - Snow accumulation: wsnow += snowfall * dt → batch
 *
 * @note Legacy reference: src/legacy/engine/snow.c
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_SNOW_HPP
#define OPENSWMM_SNOW_HPP

#include <vector>

namespace openswmm {

struct SimulationContext;

namespace snow {

// ============================================================================
// Constants
// ============================================================================

constexpr int N_SUBAREAS = 3;    ///< Plowable, Impervious, Pervious
constexpr int SNOW_PLOWABLE = 0;
constexpr int SNOW_IMPERV   = 1;
constexpr int SNOW_PERV     = 2;

// ============================================================================
// Per-subcatchment snowpack state (SoA for vectorization)
// ============================================================================

struct SnowSoA {
    int n_subcatch = 0;

    // Per subcatchment × 3 subareas = flat arrays [subcatch * 3 + subarea]
    std::vector<double> wsnow;    ///< Snow water equivalent (ft)
    std::vector<double> fw;       ///< Free water in pack (ft)
    std::vector<double> coldc;    ///< Cold content (ft water equiv)
    std::vector<double> ati;      ///< Antecedent temperature index (deg F)
    std::vector<double> awe;      ///< Areal depletion index
    std::vector<double> imelt;    ///< Melt rate output (ft/sec)

    // Per subcatchment × 3 subareas: parameters
    std::vector<double> tbase;    ///< Base melt temperature (deg F)
    std::vector<double> dhm;      ///< Degree-day melt factor (ft/deg-F/sec)
    std::vector<double> dhmin;    ///< Min melt coeff (winter solstice) (ft/deg-F/sec)
    std::vector<double> dhmax;    ///< Max melt coeff (summer solstice) (ft/deg-F/sec)
    std::vector<double> fwfrac;   ///< Free water capacity fraction

    // Per subcatchment × 3 subareas: area fractions
    std::vector<double> fArea;    ///< Fraction of total area for each subarea

    // Per subcatchment × 3 subareas: areal depletion state
    std::vector<double> si;       ///< Snow depth for 100% cover (ft)
    std::vector<double> sba;      ///< Snow coverage area at start of new-snow ADC
    std::vector<double> sbws;     ///< Snow water equiv at end of new-snow ADC
    std::vector<double> asc;      ///< Computed areal snow coverage (0–1), per subarea

    // Global ADC curves (shared by all subcatchments, 10 points each)
    double adc_imperv[10] = {1,1,1,1,1,1,1,1,1,1};
    double adc_perv[10]   = {1,1,1,1,1,1,1,1,1,1};

    // Per subcatchment: area fractions
    std::vector<double> snn;      ///< Plowable fraction of impervious area

    // Per subcatchment: plowing parameters
    std::vector<double> weplow;   ///< Depth at which plowing begins (ft)
    std::vector<double> sfrac;    ///< Plowing fractions [subcatch * 5 + i]
                                  ///<   [0]=removed, [1]=to imperv, [2]=to perv,
                                  ///<   [3]=immediate melt, [4]=to other subcatch
    std::vector<int>    to_subcatch; ///< Target subcatchment for plowed snow

    // Global parameters
    double tipm    = 0.5;         ///< ATI weighting factor
    double rnm     = 0.6;         ///< Negative melt ratio
    double season  = 0.0;         ///< Current snowmelt season factor (-1 to +1)
    double removed = 0.0;         ///< Cumulative snow plowed out of system (ft3)

    void resize(int n);
};

// ============================================================================
// Snow solver
// ============================================================================

class SnowSolver {
public:
    void init(int n_subcatch);

    /**
     * @brief Compute snowmelt for all subcatchments (batch).
     *
     * @details For each subcatchment × subarea:
     *   1. Batch ATI update (vectorisable)
     *   2. Batch cold content update (vectorisable)
     *   3. Batch degree-day or rain-on-snow melt (vectorisable)
     *   4. Batch snow accumulation from snowfall (vectorisable)
     *   5. Batch free water routing (vectorisable)
     *
     * @param ctx  Simulation context.
     * @param dt   Timestep (seconds).
     * @param temp Air temperature (deg F, scalar — broadcast).
     * @param wind Wind speed (mph, scalar — broadcast).
     * @param rainfall Current rainfall rate (ft/sec, scalar — broadcast).
     */
    /**
     * @param gamma Psychrometric constant from climate (deg F^-1).
     * @param ea    Saturation vapor pressure from climate (in Hg).
     */
    void execute(SimulationContext& ctx, double dt,
                 double temp, double wind, double rainfall,
                 double snowfall = 0.0,
                 double gamma = 0.0, double ea = 0.0);

    /**
     * @brief Update seasonal melt coefficients based on day of year.
     * @param day_of_year  Day of year (1-365).
     * @note Legacy reference: snow.c — snow_setMeltCoeffs()
     */
    void setMeltCoeffs(int day_of_year);

    /**
     * @brief Snow plowing — redistribute excess snow between subareas.
     * @param ctx  Simulation context (for subcatchment areas).
     * @param dt   Timestep (seconds).
     * @param snowfall  Snowfall rate (ft/sec).
     * @note Legacy reference: snow.c — snow_plowSnow()
     */
    void plowSnow(SimulationContext& ctx, double dt, double snowfall);

    SnowSoA& state() { return soa_; }

private:
    SnowSoA soa_;

    /// Batch ATI update: ati[i] += tipm * (temp - ati[i])
    static void batchATIUpdate(double* ati, double temp, double tipm,
                               double dt, int count);

    /// Batch degree-day melt: melt[i] = dhm[i] * max(0, temp - tbase[i])
    static void batchDegreeDayMelt(const double* dhm, const double* tbase,
                                    double temp, double* melt, int count);

    /// Batch rain-on-snow melt
    static void batchRainOnSnowMelt(double temp, double wind, double gamma,
                                     double ea, double rainfall,
                                     double* melt, int count);

    /// Batch snow accumulation: wsnow[i] += snowfall * dt
    static void batchAccumulate(double* wsnow, double snowfall, double dt, int count);
};

} // namespace snow
} // namespace openswmm

#endif // OPENSWMM_SNOW_HPP
