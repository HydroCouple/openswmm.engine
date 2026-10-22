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
 * @file      DegreeDaySnow.hpp
 * @brief     Snowmelt — degree-day and rain-on-snow methods.
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
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_SNOW_HPP
#define OPENSWMM_SNOW_HPP

#include <cstddef>
#include <vector>
#include "SnowModel.hpp"

namespace openswmm {

struct SimulationContext;

namespace snow {

// ============================================================================
// Constants
// ============================================================================

constexpr int N_SUBAREAS = 3; ///< Plowable, Impervious, Pervious
constexpr int SNOW_PLOWABLE = 0;
constexpr int SNOW_IMPERV = 1;
constexpr int SNOW_PERV = 2;

// ============================================================================
// Per-subcatchment snowpack state (SoA for vectorization)
// ============================================================================

struct SnowSoA {
    int n_subcatch = 0;
    // Native UEB columns publish into the reporting arrays below, but their
    // water must never also be advanced by the degree-day solver.
    std::vector<bool> external;
    std::vector<double> external_net; ///< UEB melt plus throughfall, ft/s

    // Per subcatchment × 3 subareas = flat arrays [subcatch * 3 + subarea]
    std::vector<double> wsnow; ///< Snow water equivalent (ft)
    std::vector<double> fw;    ///< Free water in pack (ft)
    std::vector<double> coldc; ///< Cold content (ft water equiv)
    std::vector<double> ati;   ///< Antecedent temperature index (deg F)
    std::vector<double> awe;   ///< Areal depletion index
    std::vector<double> imelt; ///< Melt rate output (ft/sec)

    // Per subcatchment × 3 subareas: parameters
    std::vector<double> tbase;  ///< Base melt temperature (deg F)
    std::vector<double> dhm;    ///< Degree-day melt factor (ft/deg-F/sec)
    std::vector<double> dhmin;  ///< Min melt coeff (winter solstice) (ft/deg-F/sec)
    std::vector<double> dhmax;  ///< Max melt coeff (summer solstice) (ft/deg-F/sec)
    std::vector<double> fwfrac; ///< Free water capacity fraction

    // Per subcatchment × 3 subareas: area fractions
    std::vector<double> fArea; ///< Fraction of total area for each subarea

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
    std::vector<double>
        ueb_transfer_energy;            // pending DD-to-UEB plow heat, kJ/m2 of receiving subarea
    std::vector<unsigned char> enabled; // driver ownership mask, true by default
    bool conservative_plowing =
        false; // completed source footprints use actual donor/receiver areas
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
    std::vector<double> si;   ///< Snow depth for 100% cover (ft)
    std::vector<double> sba;  ///< Snow coverage area at start of new-snow ADC
    std::vector<double> sbws; ///< Snow water equiv at end of new-snow ADC
    std::vector<double> asc;  ///< Computed areal snow coverage (0–1), per subarea

    // Global ADC curves (shared by all subcatchments, 10 points each)
    double adc_imperv[10] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    double adc_perv[10] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1};

    // Per subcatchment: area fractions
    std::vector<double> snn; ///< Plowable fraction of impervious area

    // Per subcatchment: plowing parameters
    std::vector<double> weplow;   ///< Depth at which plowing begins (ft)
    std::vector<double> sfrac;    ///< Plowing fractions [subcatch * 5 + i]
                                  ///<   [0]=removed, [1]=to imperv, [2]=to perv,
                                  ///<   [3]=immediate melt, [4]=to other subcatch
    std::vector<int> to_subcatch; ///< Target subcatchment for plowed snow

    // Global parameters
    double tipm = 0.5;    ///< ATI weighting factor
    double rnm = 0.6;     ///< Negative melt ratio
    double season = 0.0;  ///< Current snowmelt season factor (-1 to +1)
    double removed = 0.0; ///< Cumulative snow plowed out of system (ft3)

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

    /** @brief Reset/rescale all per-source/per-surface arrays for n three-surface packs.
     * @param n Nonnegative count with n*3 and n*5 representable as int and allocatable.
     * Resets water/free-water/cold/age/flux/parameters, enables every source and marks
     * all degree-day-owned; default ATI/base=32 F, depletion index/coverage one, free
     * water fraction 0.1 and area fractions zero. Does not reset global ADC curves,
     * tipm/rnm/season/removed/precip_age/track_age/conservative_plowing. Allocation can
     * leave partial arrays. Caller configures physical parameters and global lifecycle. */
    void resize(int n);
};

// ============================================================================
// Snow solver
// ============================================================================

class DegreeDaySnow : public surface::SnowModel {
public:
    /** @brief Reset native three-subarea storage through SnowSoA::resize().
     * @param n_subcatch Nonnegative model source count with safe integer products/storage.
     * Does not populate snowpack geometry/parameters or reset retained global controls/
     * removed totals. Caller configures compatible state before accumulation/melt. */
    void init(int n_subcatch);
    /** @brief Allocate native storage rounded up to whole three-element packs.
     * @param n Nonnegative requested point count with n+2 representable as int.
     * Calls init(ceil(n/3)), so actual step capacity is 3*ceil(n/3); extra slots are
     * allocated/defaulted too. Does not load physical parameters or distinguish mesh
     * from native sources; caller supplies per-element configuration and ownership. */
    void initializeElements(int n) override { init((n + N_SUBAREAS - 1) / N_SUBAREAS); }
    /** @brief Accumulate and melt one SI point element without batch plowing.
     * @param e Flat allocated element index (source*3+subarea); invalid returns zeroed output.
     * @param input Finite positive dt_s; interval rain/snow depths metres, temperature C,
     * wind m/s and native gamma/ea. Other weather/energy fields are ignored by degree day.
     * @param[out] output Reset first, even for invalid index/nonpositive dt. Replaces
     * melt_m with total liquid pack drainage, throughfall_m with uncovered rain, swe_m
     * with snow+free water and cover_frac. Thermal/albedo/sublimation fields stay zero.
     * Adds snowfall once, ages/mixes the optional complete snow/free-water age pool and
     * uses current seasonal melt coefficients/depletion history. Does not honor native
     * enabled/external source masks or perform inter-element plowing: caller exclusively
     * owns this element and sets coefficients first. No finite-input checks, status or
     * rollback; dt NaN is not caught by the nonpositive guard. */
    void step(int e, const surface::SnowInputs& input, surface::SnowOutputs& output) override;
    /** @brief Read the fixed native per-element payload size.
     * @return Twelve doubles; pack/unpack omit geometry/global parameters, removed total,
     * source masks, pending UEB transfer energy and external-solver runtime ownership. */
    int stateSize() const override { return 12; }
    /** @brief Copy twelve native element history values without validating the buffer.
     * @param e Valid allocated flat index, unchecked.
     * @param[out] output Writable twelve doubles: wsnow/fw/coldc(ft), ati(F), awe,
     * imelt(ft/s), si(ft), sba/sbws/asc (depletion fractions/indices), age/out_age(seconds).
     * No metadata/version, unit conversion, global state or complete restart identity.
     * Serialize reads with mutation; pointer is borrowed only during the call. */
    void pack(int e, double* output) const override;
    /** @brief Assign twelve native element history fields from a matching payload.
     * @param e Valid allocated flat index, unchecked.
     * @param input Readable twelve doubles in pack() order/native units. No finite,
     * nonnegative, coverage or model/shape validation; caller qualifies payload before
     * publishing a restart. Does not restore geometry/globals/masks or resize storage. */
    void unpack(int e, const double* input) override;

    /** @brief Melt every enabled native-owned batch pack after snowfall/plowing preparation.
     * @param ctx Unused by this degree-day melt overload; geometry is already in soa_.
     * @param dt Finite positive seconds, unchecked and used as divisor.
     * @param temp Held air Fahrenheit.
     * @param wind Held nonnegative mph for rain-on-snow.
     * @param rainfall Readable n_subcatch finite nonnegative ft/s values.
     * @param snowfall Readable n_subcatch finite nonnegative ft/s values, already added
     * to wsnow by plowSnow; used for depletion/cold-content transitions, not added again.
     * @param gamma Native psychrometric coefficient F^-1, default zero.
     * @param ea Native vapour pressure in Hg, default zero.
     * Processes three subareas in order, skips disabled/external source rows, computes
     * liquid melt/coverage and optional out_age. Must pair exactly once with plowSnow
     * and current setMeltCoeffs; does not advance context date or perform external UEB.
     * No-op when n_subcatch==0. Invalid inputs can propagate without status/rollback. */
    void execute(SimulationContext& ctx, double dt, double temp, double wind,
                 const double* rainfall, const double* snowfall, double gamma = 0.0,
                 double ea = 0.0) override;

    /** @brief Broadcast native precipitation rates then call the array melt overload.
     * @param ctx Forwarded compatible context; unused by the degree-day melt kernel.
     * @param dt Finite positive interval seconds.
     * @param temp Held Fahrenheit.
     * @param wind Held mph.
     * @param rainfall Uniform finite nonnegative rain ft/s.
     * @param snowfall Uniform snow ft/s, already accumulated by prior plowSnow.
     * @param gamma Native psychrometric F^-1 coefficient.
     * @param ea Native vapour pressure in Hg.
     * Allocates temporary n_subcatch arrays, no-op for zero sources. Same ownership/
     * sequence/output contract as array execute; allocation exceptions propagate. */
    void execute(SimulationContext& ctx, double dt, double temp, double wind, double rainfall,
                 double snowfall = 0.0, double gamma = 0.0, double ea = 0.0);

    /** @brief Update every native subarea's seasonal degree-day coefficient.
     * @param day_of_year Valid ordinal [1,366], unchecked.
     * Stores sin(0.0172615*(day-81)) season and interpolates current dhm from dhmin/dhmax.
     * This legacy phase/period is preserved, not normalized to 365 days. Does not advance
     * water, reset age/fluxes, load parameters or skip externally owned/masked rows. */
    void setMeltCoeffs(int day_of_year);

    /** @brief Add native snowfall once, age packs, then redistribute plowable solid snow.
     * @param[in,out] ctx Compatible source geometry/project units, total LID areas and
     * cross-source plow destinations; context is read for physical non-LID footprints.
     * @param dt Finite positive seconds; no general validation (instant plow melt alone
     * guards dt>0). Caller follows once with execute() for the same interval.
     * @param snowfall Readable n_subcatch finite nonnegative snow ft/s values.
     * Skips disabled/external donor sources. First pass ages existing complete-mix water
     * and adds new snow at precip_age, then a separate pass plows to removal, native
     * impervious/pervious, instant melt or another pervious source. Zero threshold plows
     * every step. Fractions/areas must be qualified; only their total donor subtraction
     * is capped at one. Conservative mode or external receivers include donor/receiver
     * physical-area ratio; ordinary legacy transfer preserves its fraction-only arithmetic.
     * Can transfer cold energy to an external UEB receiver and records removed ft3.
     * Mutates owned histories immediately; no conservation diagnostic/rollback or clock
     * advance, and external receivers must be settled by their own qualified driver. */
    void plowSnow(SimulationContext& ctx, double dt, const double* snowfall) override;

    /** @brief Broadcast uniform snowfall then use native accumulation/plowing.
     * @param[in,out] ctx Compatible geometry/units forwarded to array plowSnow.
     * @param dt Finite positive seconds.
     * @param snowfall Uniform finite nonnegative ft/s, added exactly once per enabled
     * native pack before melt execution. No-op with zero sources; temporary allocation
     * can throw. Same plowing/age/ownership and immediate-mutation contract as array overload. */
    void plowSnow(SimulationContext& ctx, double dt, double snowfall);

    /** @brief Borrow mutable native geometry, controls, ownership and pack history.
     * @return Owned SoA; resize/init/move/destruction can invalidate elements. Caller keeps
     * all arrays aligned and physically valid and serializes edits with every snow call.
     * No automatic model qualification, external UEB synchronization or trial rollback. */
    SnowSoA& state() { return soa_; }
    /** @brief Borrow immutable current native snow state and liquid-flux outputs.
     * @return Owned SoA, flat source*3+subarea layout. Current imelt is ft/s; out_age
     * seconds meaningful only for positive emitted liquid. Init/resize/replacement can
     * invalidate elements and stepping changes contents; copy stable snapshots as needed. */
    const SnowSoA& state() const { return soa_; }

private:
    SnowSoA soa_;
    /** @brief Melt/drain one already accumulated native subarea.
     * @param ui Valid flat array index with aligned qualified parameters/history.
     * @param subarea PLOWABLE/IMPERV/PERV code; plowable bypasses depletion.
     * @param dt Finite positive seconds.
     * @param temp Air Fahrenheit.
     * @param rain Finite nonnegative rainfall ft/s; covered fraction enters free water.
     * @param snow Finite nonnegative snowfall ft/s already in wsnow, used for ADC/cold history.
     * @param rmelt Raw native rain-on-snow potential ft/s; positive overrides degree day.
     * Retains existing instant-plow imelt, drains thin packs at <=0.001 inch, otherwise
     * updates depletion, cold content, solid/free water and capacity drainage. Optional
     * age follows the complete mixture; emitted liquid age survives an emptied pack.
     * Mutates owned state with no index/finite checks or rollback. */
    void meltElement(std::size_t ui, int subarea, double dt, double temp, double rain, double snow,
                     double rmelt);

    /** @brief Compute raw rain-on-snow potential with the legacy wind/vapour formula.
     * @param temp Finite Fahrenheit.
     * @param wind Finite nonnegative mph.
     * @param gamma Psychrometric coefficient F^-1.
     * @param ea Saturation vapour pressure in Hg.
     * @param rainfall Finite rain ft/s, internally multiplied by 43200 to in/hr.
     * @return ft/s, zero at rainfall <= 0.02 in/hr; can be negative for cold conditions.
     * Caller tests positive potential before using it; no finite/clamp/state mutation. */
    static double rainMeltRate(double temp, double wind, double gamma, double ea, double rainfall);
};

using SnowSolver = DegreeDaySnow;
} // namespace snow
} // namespace openswmm

namespace openswmm::surface {
using DegreeDaySnow = snow::DegreeDaySnow;
}

#endif // OPENSWMM_SNOW_HPP
