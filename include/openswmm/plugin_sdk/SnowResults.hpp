// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <vector>
namespace openswmm {
// One catalogue for immutable output snapshots and live clients. All quantities
// are SI, independent of project display units. Unassigned/unsupported values
// are NaN; the model code is -1 unassigned, 0 degree-day, 1 UEB.
struct SnowResultField { const char* name; const char* label; const char* units; };
inline constexpr std::array<SnowResultField,23> meshSnowFields{{
    {"Mesh2_face_snow_model","snow model (-1 unassigned, 0 degree-day, 1 UEB)","1"},
    {"Mesh2_face_snow_completed","snow state time since start","s"},
    {"Mesh2_face_snow_swe","snow water equivalent","m"},
    {"Mesh2_face_snow_cover","snow covered fraction","1"},
    {"Mesh2_face_snow_melt","snowpack liquid outflow rate","m s-1"},
    {"Mesh2_face_snow_surface_input","snow liquid input to surface (including throughfall)","m s-1"},
    {"Mesh2_face_snow_sublimation","snow sublimation rate","m s-1"},
    {"Mesh2_face_snow_energy","snow energy relative to ice at zero degrees C","kJ m-2"},
    {"Mesh2_face_snow_surface_temperature","snow surface temperature","degC"},
    {"Mesh2_face_snow_bulk_temperature","snow bulk temperature","degC"},
    {"Mesh2_face_snow_albedo","snow albedo","1"},
    {"Mesh2_face_snow_surface_age","snow dimensionless surface age","1"},
    {"Mesh2_face_snow_initial","initial snow water inventory","m3"},
    {"Mesh2_face_snow_raw_precipitation","cumulative precipitation before snow catch correction","m3"},
    {"Mesh2_face_snow_precipitation","cumulative precipitation after snow catch correction","m3"},
    {"Mesh2_face_snow_liquid_cum","cumulative snow liquid input to surface","m3"},
    {"Mesh2_face_snow_sublimation_cum","cumulative snow sublimation","m3"},
    {"Mesh2_face_snow_storage","stored snow water inventory","m3"},
    {"Mesh2_face_snow_residual","snow water continuity residual","m3"},
    {"Mesh2_face_snow_net_energy","cumulative net snow energy input","kJ"},
    {"Mesh2_face_snow_initial_energy","initial snow/ground energy","kJ"},
    {"Mesh2_face_snow_stored_energy","stored snow/ground energy","kJ"},
    {"Mesh2_face_snow_energy_residual","snow/ground energy continuity residual","kJ"}
}};
using MeshSnowSnapshot = std::array<std::vector<double>,meshSnowFields.size()>;
template<class State,class Ledger>
inline std::array<double,meshSnowFields.size()> meshSnowValues(const State& s,const Ledger& b) {
    return {double(s.model),b.completed_s,s.swe_m,s.cover_fraction,s.melt_ms,s.surface_input_ms,s.sublimation_ms,
        s.energy_kjm2,s.surface_temperature_c,s.bulk_temperature_c,s.albedo,s.surface_age,
        b.initial_m3,b.raw_precipitation_m3,b.precipitation_m3,b.liquid_m3,b.sublimation_m3,b.stored_m3,b.residual_m3,b.net_energy_kj,b.initial_energy_kj,b.stored_energy_kj,b.energy_residual_kj};
}
} // namespace openswmm
