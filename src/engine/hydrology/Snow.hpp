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
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
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
    std::vector<double> fwfrac;   ///< Free water capacity fraction

    // Per subcatchment: area fractions
    std::vector<double> snn;      ///< Plowable fraction of impervious area

    // Global parameters
    double tipm = 0.5;            ///< ATI weighting factor
    double rnm  = 0.6;            ///< Negative melt ratio

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
    void execute(SimulationContext& ctx, double dt,
                 double temp, double wind, double rainfall);

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
