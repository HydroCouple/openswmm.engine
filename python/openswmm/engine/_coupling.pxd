# SPDX-License-Identifier: Apache-2.0
from ._common cimport SWMM_Engine

cdef extern from "openswmm/engine/openswmm_coupling.h":
    ctypedef struct SWMM_CouplingSource:
        unsigned struct_size
        const char* id
        int domain
        int cell
        double flow_m3_s
        double heat_w
        double until_seconds
        int species_count
        const double* concentrations
        const double* rates
    ctypedef struct SWMM_CouplingReceipt:
        double requested_m3
        double applied_m3
        double rejected_m3
        double requested_heat_j
        double applied_heat_j
        double rejected_heat_j
        double last_applied_m3_s
        double last_interval_seconds
    int swmm_coupling_set_source(SWMM_Engine, const char*, int, int, double, double, const double*, const double*, int, double) nogil
    int swmm_coupling_advance_to(SWMM_Engine, double, double*) nogil
    int swmm_coupling_set_sources(SWMM_Engine, const SWMM_CouplingSource*, int) nogil
    int swmm_coupling_clear_source(SWMM_Engine, int, const char*) nogil
    int swmm_coupling_get_receipt(SWMM_Engine, int, const char*, int, SWMM_CouplingReceipt*, double*, double*, int) nogil
    int swmm_2d_set_runtime_boundary(SWMM_Engine, int, int, int, double, const double*, int) nogil
    int swmm_gw2d_set_runtime_boundary(SWMM_Engine, int, int, int, double, const double*, int) nogil
    int swmm_gw2d_clear_runtime_boundary(SWMM_Engine, int, int) nogil
    int swmm_gw2d_get_runtime_boundary_flow(SWMM_Engine, int, int, double*) nogil
    int swmm_coupling_get_water_totals(SWMM_Engine, int, double*, double*) nogil
    int swmm_coupling_capabilities(SWMM_Engine, unsigned*) nogil

    ctypedef struct SWMM_CouplingBoundary:
        unsigned struct_size
        int domain
        int cell
        int edge
        int kind
        double value
        int species_count
        const double* concentrations
    ctypedef struct SWMM_CouplingForcing:
        unsigned struct_size
        int channel
        int cell
        int mode
        double rate_m_s
        double until_seconds
        int species_count
        const double* concentrations
    ctypedef struct SWMM_CouplingClear:
        int domain
        int cell
        const char* id
    ctypedef struct SWMM_CouplingFrame:
        unsigned struct_size
        unsigned version
        const char* provider
        int source_count
        const SWMM_CouplingSource* sources
        int boundary_count
        const SWMM_CouplingBoundary* boundaries
        int forcing_count
        const SWMM_CouplingForcing* forcings
        int clear_count
        const SWMM_CouplingClear* clears
    int swmm_coupling_apply_frame(SWMM_Engine, const SWMM_CouplingFrame*) nogil
    int swmm_coupling_clear_source_cell(SWMM_Engine, int, const char*, int) nogil
    int swmm_coupling_get_receipts(SWMM_Engine, int, const char*, const int*, int, SWMM_CouplingReceipt*, double*, double*, int) nogil
    int swmm_coupling_get_boundary_receipt(SWMM_Engine, int, int, int, SWMM_CouplingReceipt*, double*, double*, int) nogil
    int swmm_coupling_get_infiltration_receipt(SWMM_Engine, int, SWMM_CouplingReceipt*, double*, double*, int) nogil
    int swmm_2d_get_species_ledger(SWMM_Engine, int, int, double*) nogil
