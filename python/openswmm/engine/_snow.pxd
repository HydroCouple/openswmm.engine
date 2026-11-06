from ._common cimport SWMM_Engine

cdef extern from "openswmm/engine/openswmm_snow.h":
    ctypedef struct SWMM_SnowState:
        int model
        double swe_m
        double cover_fraction
        double melt_ms
        double surface_input_ms
        double energy_kjm2
        double surface_temperature_c
        double bulk_temperature_c
        double albedo
        double surface_age
        double sublimation_ms
        double water_age_s

cdef SWMM_Engine _h(view) except NULL
cdef bytes _text(value)
cdef dict _state(SWMM_SnowState v)
