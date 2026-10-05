// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin

#pragma once
namespace openswmm { struct SimulationContext; }
namespace openswmm::surface {
struct SnowInputs {
    double dt_s = 0, rain_m = 0, snow_m = 0, Ta_C = 0, wind_ms = 0;
    double rh = 0, Qsi = 0, Qli = 0, Qg = 0, cos_psi_avg = 0, hri = 0;
    // Existing climate coefficients for degree-day rain-on-snow; passed
    // through unchanged on the legacy batch boundary (F^-1 and in Hg).
    double gamma = 0, ea = 0;
};
struct SnowOutputs {
    double melt_m = 0, throughfall_m = 0, sublim_m = 0, swe_m = 0, cover_frac = 0;
    double Ts_C = 0, T_C = 0, albedo = 0, energy_kJm2 = 0;
};
class SnowModel {
public:
    virtual ~SnowModel() = default;
    virtual void initializeElements(int n) = 0;
    virtual void step(int e, const SnowInputs&, SnowOutputs&) = 0;
    virtual int stateSize() const = 0;
    virtual void pack(int e, double*) const = 0;
    virtual void unpack(int e, const double*) = 0;
    // Existing subcatchments use this native-unit batch boundary to preserve
    // operation order and inter-subcatchment plowing. SI elements use step().
    virtual void execute(SimulationContext&, double dt_s, double temp_F, double wind_mph,
                         const double* rain_ft_s, const double* snow_ft_s, double gamma, double ea) = 0;
    virtual void plowSnow(SimulationContext&, double dt_s, const double* snow_ft_s) = 0;
};
}
