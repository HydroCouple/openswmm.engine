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
 * @file MsxSurfaceData.hpp
 * @brief BW-MSX (2026-09-19) — surface buildup / washoff / sweeping state and
 *        parameters for the reactions component's (MSX) species.
 *
 * @details `[BUILDUP]`, `[WASHOFF]` and `[LOADINGS]` are pollutant-keyed
 *          everywhere (`BuildupData`, `WashoffData`, `SurfaceQualitySoA`,
 *          `subcatches.conc`). MSX species are declared by the reactions
 *          component, which is applied only after the `.inp` has been read,
 *          so a row naming one cannot be resolved by the section handler.
 *          The handlers therefore park such rows here BY NAME
 *          (`MsxSurfaceRows`) and `msxsurf::resolve()` binds them after the
 *          component is in (PostParseResolver time), exactly the deferred
 *          pattern `[INITIAL_QUALITY]` and `[INFLOWS]` use for MSX species.
 *
 *          The resolved parameters and the per-(subcatchment × land use ×
 *          species) buildup state live in this struct, separate from the
 *          pollutant arrays so the pollutant path is untouched (bit-identity
 *          for decks without MSX rows). The arithmetic that advances them is
 *          the subcatchment surface-quality step's, transcribed once in
 *          `quality/MsxSurfaceQuality.cpp`.
 *
 *          Design: `plans/transport/OVERLAND_TRANSPORT_HEAT_MSX_PLAN_2026-09-01.md`
 *          §8.9; program plan decisions D-A24, D-A27–D-A29.
 *
 * @ingroup engine_data
 */

#ifndef OPENSWMM_ENGINE_DATA_MSX_SURFACE_DATA_HPP
#define OPENSWMM_ENGINE_DATA_MSX_SURFACE_DATA_HPP

#include <cstddef>
#include <string>
#include <vector>

namespace openswmm {

/// Rows parked by the section handlers for names that are not pollutants.
struct MsxSurfaceRows {
    struct Buildup {
        std::string landuse, species;
        int    func_type = 0;      ///< 0=NONE,1=POW,2=EXP,3=SAT,4=EXT
        double c1 = 0.0, c2 = 0.0, c3 = 0.0;
        std::string ts_name;       ///< EXT: time series name (resolved later)
        int    normalizer = 0;     ///< 0=PER_AREA, 1=PER_CURB
    };
    struct Washoff {
        std::string landuse, species;
        int    func_type = 0;      ///< 0=NONE,1=EXP,2=RC,3=EMC
        double coeff = 0.0, expon = 0.0, sweep_effic = 0.0, bmp_effic = 0.0;
    };
    struct Loading {
        std::string subcatch, species;
        double value = 0.0;        ///< initial buildup, mass per unit area
    };
    std::vector<Buildup> buildup;
    std::vector<Washoff> washoff;
    std::vector<Loading> loadings;
    bool empty() const noexcept {
        return buildup.empty() && washoff.empty() && loadings.empty();
    }
};

/**
 * @brief Resolved MSX surface-quality parameters and state.
 *
 * Index conventions (nlu = land uses, nm = MSX species, nsc = subcatchments):
 *  - parameters: `[lu * nm + m]`
 *  - buildup state: `bu_idx(sc, lu, m) = (sc * nlu + lu) * nm + m`
 *    (mass per normalizer unit — user mass per acre/ha or per curb unit —
 *    the `SurfaceQualitySoA` convention)
 *  - per-(sc, m) washoff concentration: `[sc * nm + m]` (concentration units)
 *  - ledgers per species `[m]` in USER MASS (lbs / kg), the `mass_balance.qual_*`
 *    convention.
 */
struct MsxSurfaceData {
    MsxSurfaceRows rows;                 ///< parse-time, by name

    int n_landuses = 0;
    int n_species  = 0;                  ///< MSX species count at resolve time
    int n_subcatch = 0;

    // ---- parameters [lu * n_species + m] --------------------------------
    std::vector<int>    bu_type;         ///< 0=NONE,1=POW,2=EXP,3=SAT,4=EXT
    std::vector<double> bu_c1, bu_c2, bu_c3;   ///< EXT: c3 = time-series index
    std::vector<int>    bu_normalizer;   ///< 0=PER_AREA, 1=PER_CURB
    std::vector<double> bu_max_days;     ///< derived (99.9 % of max)
    std::vector<int>    wo_type;         ///< 0=NONE,1=EXP,2=RC,3=EMC
    std::vector<double> wo_coeff, wo_expon, wo_sweep_effic, wo_bmp_effic;

    // ---- initial loading [sc * n_species + m] (mass per unit area) ------
    std::vector<double> init_loading;

    // ---- per-species unit factor: concentration-mass → user mass --------
    std::vector<double> mcf;             ///< D-A27: MG → UCF(MASS), UG → /1000, else 1

    // ---- state ----------------------------------------------------------
    std::vector<double> buildup;         ///< bu_idx(sc, lu, m), per normalizer unit
    std::vector<double> washoff_conc;    ///< [sc * n_species + m], conc units (mg/L-like)
    /// Previous runoff step's `washoff_conc`, the "old" end of the routing-step
    /// interpolation the delivery seam does. The pollutant mirror of this is
    /// `SubcatchData::conc_old`, rolled by `SubcatchData::save_state()`; this
    /// one is rolled beside it, on the same runoff cadence.
    std::vector<double> washoff_conc_old;

    // ---- ledgers per species [m], user mass ---------------------------
    std::vector<double> led_init_buildup;
    std::vector<double> led_buildup;     ///< net accrual over the run
    std::vector<double> led_runoff_load; ///< delivered to the network
    std::vector<double> led_sweeping;
    std::vector<double> led_bmp_removal;
    std::vector<double> led_subcatch_load; ///< [sc * n_species + m], per-subcatchment total (report)

    bool resolved = false;
    bool active() const noexcept { return resolved && n_species > 0 && n_landuses > 0; }

    std::size_t pidx(int lu, int m) const noexcept {
        return static_cast<std::size_t>(lu) * static_cast<std::size_t>(n_species) +
               static_cast<std::size_t>(m);
    }
    std::size_t bu_idx(int sc, int lu, int m) const noexcept {
        return (static_cast<std::size_t>(sc) * static_cast<std::size_t>(n_landuses) +
                static_cast<std::size_t>(lu)) * static_cast<std::size_t>(n_species) +
               static_cast<std::size_t>(m);
    }
    std::size_t sidx(int sc, int m) const noexcept {
        return static_cast<std::size_t>(sc) * static_cast<std::size_t>(n_species) +
               static_cast<std::size_t>(m);
    }

    void resize_params(int nlu, int nm) {
        n_landuses = nlu; n_species = nm;
        const auto n = static_cast<std::size_t>(nlu) * static_cast<std::size_t>(nm);
        bu_type.assign(n, 0); bu_c1.assign(n, 0.0); bu_c2.assign(n, 0.0); bu_c3.assign(n, 0.0);
        bu_normalizer.assign(n, 0); bu_max_days.assign(n, 0.0);
        wo_type.assign(n, 0); wo_coeff.assign(n, 0.0); wo_expon.assign(n, 0.0);
        wo_sweep_effic.assign(n, 0.0); wo_bmp_effic.assign(n, 0.0);
        mcf.assign(static_cast<std::size_t>(nm), 1.0);
    }
    void resize_state(int nsc) {
        n_subcatch = nsc;
        const auto nm = static_cast<std::size_t>(n_species);
        buildup.assign(static_cast<std::size_t>(nsc) * static_cast<std::size_t>(n_landuses) * nm, 0.0);
        washoff_conc.assign(static_cast<std::size_t>(nsc) * nm, 0.0);
        washoff_conc_old.assign(static_cast<std::size_t>(nsc) * nm, 0.0);
        if (init_loading.size() != static_cast<std::size_t>(nsc) * nm)
            init_loading.assign(static_cast<std::size_t>(nsc) * nm, 0.0);
        led_init_buildup.assign(nm, 0.0); led_buildup.assign(nm, 0.0);
        led_runoff_load.assign(nm, 0.0);  led_sweeping.assign(nm, 0.0);
        led_bmp_removal.assign(nm, 0.0);
        led_subcatch_load.assign(static_cast<std::size_t>(nsc) * nm, 0.0);
    }
};

} // namespace openswmm

#endif // OPENSWMM_ENGINE_DATA_MSX_SURFACE_DATA_HPP
