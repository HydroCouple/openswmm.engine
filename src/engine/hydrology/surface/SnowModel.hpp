// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin

/**
 * @file      SnowModel.hpp
 * @brief     Shared snow model contracts and state types.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
namespace openswmm {
struct SimulationContext;
}
namespace openswmm::surface {
/** @brief One element's interval weather, with SI point-input units.
 * dt_s is seconds; rain_m/snow_m are interval water-equivalent depths (not rates),
 * Ta_C is Celsius and wind_ms m/s. rh is fraction, Qsi/Qli/Qg incoming W/m2;
 * cosine and horizontal-radiation factors are dimensionless. gamma/ea retain
 * native degree-day F^-1/in-Hg conventions. Individual models may ignore fields. */
struct SnowInputs {
    double dt_s = 0, rain_m = 0, snow_m = 0, Ta_C = 0, wind_ms = 0;
    double rh = 0, Qsi = 0, Qli = 0, Qg = 0, cos_psi_avg = 0, hri = 0;
    // Existing climate coefficients for degree-day rain-on-snow; passed
    // through unchanged on the legacy batch boundary (F^-1 and in Hg).
    double gamma = 0, ea = 0;
};
/** @brief Point snow results: interval depths metres and model-specific thermal state.
 * melt_m can include pack drainage, mixed rainfall and instant plow liquid, not
 * only phase change; throughfall_m is bypass rain. swe_m includes retained snow/
 * free water, cover_frac is dimensionless. Temperatures C, energy kJ/m2. Derived
 * implementations specify which thermal fields are available/defaulted. */
struct SnowOutputs {
    double melt_m = 0, throughfall_m = 0, sublim_m = 0, swe_m = 0, cover_frac = 0;
    double Ts_C = 0, T_C = 0, albedo = 0, energy_kJm2 = 0;
};
/** @brief Polymorphic point and native-batch snow boundary.
 * One owner serializes state mutation and buffer use. Configure compatible model
 * geometry/materials after allocation; a successful allocation alone does not
 * establish physical snowpack parameters. Derived methods define failure and
 * payload validation; this interface supplies no universal rollback/status. */
class SnowModel {
public:
    /** @brief Destroy derived owned snow state through the base interface.
     * Caller ends all active callbacks/borrows before destruction; no implicit commit. */
    virtual ~SnowModel() = default;
    /** @brief Allocate/reset model-specific storage for point elements.
     * @param n Nonnegative requested element count, representable by the implementation.
     * Derived storage may round capacity up; caller then supplies model parameters.
     * No common external clock, physical water debit or context binding is performed. */
    virtual void initializeElements(int n) = 0;
    /** @brief Advance one model element through its SI point boundary.
     * @param e Valid initialized element index with compatible parameters/history.
     * @param input Borrowed finite interval weather in SnowInputs units; positive dt_s.
     * @param[out] output Element interval depths/state; derived class defines available
     * fields and invalid-input behavior. Must not alias input or mutable model storage.
     * Point stepping does not imply native inter-subcatchment plowing; caller must use
     * the derived model's specified accumulation/plowing lifecycle exactly once. */
    virtual void step(int e, const SnowInputs& input, SnowOutputs& output) = 0;
    /** @brief Read the element payload length expected by pack/unpack.
     * @return Number of double slots for this implementation, not a portable versioned
     * restart schema. Use matching model/configuration and its documented native units. */
    virtual int stateSize() const = 0;
    /** @brief Copy one model's element payload into a caller buffer.
     * @param e Valid initialized element index.
     * @param[out] output Writable array of at least stateSize() doubles, not retained.
     * Slot order/units are implementation-specific; geometry/global state can be omitted.
     * Caller serializes packing with mutation; no common bounds/error check is promised. */
    virtual void pack(int e, double* output) const = 0;
    /** @brief Restore one matching model's element payload.
     * @param e Valid initialized element index with matching model/configuration.
     * @param input Readable stateSize()-double buffer in the implementation's slot order/
     * units, borrowed during this call. Caller validates shape/finite physical values.
     * No common schema migration, rollback or clock/geometry restoration is implied. */
    virtual void unpack(int e, const double* input) = 0;
    // Existing subcatchments use this native-unit batch boundary to preserve
    // operation order and inter-subcatchment plowing. SI elements use step().
    /** @brief Execute the native subcatchment batch boundary for legacy arithmetic/order.
     * @param[in,out] context Compatible model geometry/ownership as required by derived solver.
     * @param dt_s Finite positive interval seconds.
     * @param temp_F Held air temperature Fahrenheit.
     * @param wind_mph Held wind speed mph.
     * @param rain_ft_s Readable per-subcatchment rain ft/s array of initialized batch count.
     * @param snow_ft_s Readable same-count snow ft/s array; accumulation/plowing sequencing
     * is defined by the derived model, not automatically repeated here.
     * @param gamma Native psychrometric coefficient F^-1.
     * @param ea Native saturation vapour pressure in Hg.
     * Caller uses the derived batch lifecycle rather than also point-stepping the same
     * water interval. No portable external clock advancement or shared failure result. */
    virtual void execute(SimulationContext& context, double dt_s, double temp_F, double wind_mph,
                         const double* rain_ft_s, const double* snow_ft_s, double gamma,
                         double ea) = 0;
    /** @brief Accumulate and redistribute native batch snowfall through derived lifecycle.
     * @param[in,out] context Compatible subcatchment geometry/units and plow destinations.
     * @param dt_s Finite positive elapsed seconds.
     * @param snow_ft_s Readable per-subcatchment snowfall ft/s array, qualified by caller.
     * Derived implementation defines enabled/external ownership, removed-water and age
     * bookkeeping. Invoke at most once before the corresponding native melt execution;
     * this interface itself does not validate or settle an external receiver. */
    virtual void plowSnow(SimulationContext& context, double dt_s, const double* snow_ft_s) = 0;
};
}
