// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

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
 * @license  Apache-2.0
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

    /// S2b — WATER AGE of the pack's water (seconds), per subcatchment ×
    /// subarea. **Complete-mix over `wsnow + fw` TOGETHER.**
    ///
    /// Two stores would be more faithful — snow and free water have
    /// genuinely different residence times — but the melt path moves water
    /// between them within a single step anyway, and one age is the minimum
    /// that carries residence time at all. Recorded as a deliberate
    /// approximation in SNOW_DIVERGENCE_REGISTER.md rather than left
    /// unnamed, which is what lesson 64 is actually about.
    ///
    /// Maintained inside this solver on purpose. Plowing moves water between
    /// surfaces AND to another subcatchment inside `plowSnow`; an age update
    /// running afterwards can see only the end state and would have to guess
    /// which water went where. The age therefore moves at the point the
    /// water moves, which is the only place that knows.
    std::vector<double> age;
    /// S2b — age of the water this step PUBLISHED as `imelt` (seconds).
    ///
    /// Not the same as `age`, and the difference is the whole reason this
    /// array exists: in a complete-mix pool the water leaving carries the
    /// pool's age, but a pack that EMPTIES this step leaves `age` at 0 with
    /// no water to describe. Meltwater from such a pack still carries the
    /// age the pack had. Sentinel-free — it is only meaningful where
    /// `imelt > 0`, and callers gate on that.
    std::vector<double> out_age;

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

    /// S2b — age of water arriving from the sky (seconds). Set by the engine
    /// each runoff step from the water-age component's `RAINFALL` source.
    ///
    /// A scalar set from outside rather than a lookup into the transport
    /// config, so hydrology keeps no dependency on the transport layer —
    /// the same shape as `season`, `tipm` and `rnm`.
    double precip_age = 0.0;
    /// S2b — whether to maintain `age` / `out_age` at all.
    ///
    /// The age arrays are pure bookkeeping: nothing in the water balance
    /// reads them, so a deck is byte-identical either way. The flag exists
    /// to keep the arithmetic off the hot path when WATER_AGE is off, not to
    /// protect an answer.
    bool track_age = false;

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
     * @param rainfall Per-subcatchment rainfall rate (ft/sec, sized
     *                 n_subcatch — matching legacy subcatch_getRunoff →
     *                 snow_getSnowMelt which receives each subcatchment's
     *                 own precipitation).
     * @param snowfall Per-subcatchment snowfall rate (ft/sec, sized
     *                 n_subcatch).
     * @param gamma Psychrometric constant from climate (deg F^-1).
     * @param ea    Saturation vapor pressure from climate (in Hg).
     */
    void execute(SimulationContext& ctx, double dt,
                 double temp, double wind, const double* rainfall,
                 const double* snowfall,
                 double gamma = 0.0, double ea = 0.0);

    /// Scalar convenience overload: broadcast a single rainfall (and
    /// optional snowfall) rate to every subcatchment. Equivalent to filling
    /// per-subcatchment arrays with the same value and calling the array
    /// form above.
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
     * @brief Snow accumulation + plowing — adds new snowfall to each pack
     *        and redistributes excess snow between subareas.
     * @param ctx  Simulation context (for subcatchment areas).
     * @param dt   Timestep (seconds).
     * @param snowfall  Per-subcatchment snowfall rate (ft/sec, sized n_subcatch).
     * @note Legacy reference: snow.c — snow_plowSnow(), called from
     *       runoff_execute() each runoff step before melt computation.
     */
    void plowSnow(SimulationContext& ctx, double dt, const double* snowfall);

    /// Scalar convenience overload: broadcast a single snowfall rate to every
    /// subcatchment before accumulation + plowing.
    void plowSnow(SimulationContext& ctx, double dt, double snowfall);

    SnowSoA& state() { return soa_; }
    const SnowSoA& state() const { return soa_; }

private:
    SnowSoA soa_;

    /// Rain-on-snow melt rate for one rainfall value (legacy getRainmelt)
    static double rainMeltRate(double temp, double wind, double gamma,
                               double ea, double rainfall);
};

} // namespace snow
} // namespace openswmm

#endif // OPENSWMM_SNOW_HPP
