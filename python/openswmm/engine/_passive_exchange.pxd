# SPDX-License-Identifier: Apache-2.0
from libc.stdint cimport uint64_t
from ._common cimport SWMM_Engine

cdef extern from "openswmm/engine/openswmm_passive_exchange.h":
    ctypedef struct SWMM_PassiveAmountV1:
        uint64_t count
        double components[64]
    ctypedef struct SWMM_PassiveStatusV1:
        uint64_t receipt_count
        uint64_t leg_count
        uint64_t unsealed_leg_count
        int pollutant
        int units
        int failed
        int window_open
        int delivery_frozen
        int history_preflight
        int checkpoint_attached
        int history_refused
        double elapsed_ms
        double delivery_seconds
    ctypedef struct SWMM_PassivePoolV1:
        int node
        int units
        double settled_volume_ft3
        double settled_depth_ft
        double settled_concentration
        double settled_physical_amount
        double hydraulic_volume_ft3
        SWMM_PassiveAmountV1 roundoff
        SWMM_PassiveAmountV1 pending_drain
        SWMM_PassiveAmountV1 pending_spill
    ctypedef struct SWMM_PassiveReceiptV1:
        uint64_t id
        uint64_t row_count
        uint64_t first_leg
        uint64_t leg_count
        uint64_t generation
        int phase
        double capture_ms
        double start_ms
        double end_ms
        double eligible_ms
        double publication_delivery_seconds
    ctypedef struct SWMM_PassiveRowV1:
        uint64_t point
        int node
        int units
        SWMM_PassiveAmountV1 drain_water
        SWMM_PassiveAmountV1 spill_water
        SWMM_PassiveAmountV1 drain_amount
        SWMM_PassiveAmountV1 spill_amount
    ctypedef struct SWMM_PassiveLegV1:
        uint64_t point
        int cell
        int node
        int drain
        int applied
        int sealed
        int phase
        double water_m3
        double source_amount
        double water_ft3
        double native_amount
        double source_before
        double source_after
        double sampled_concentration
        double volume_before_m3
        double volume_after_m3
        double start_ms
        double end_ms
        double offset_seconds
        double duration_seconds
        double remaining_water_ft3
        double remaining_native_amount
    int swmm_passive_exchange_enable(SWMM_Engine, uint64_t, int)
    int swmm_passive_exchange_enable_checkpoint(SWMM_Engine)
    int swmm_passive_exchange_status_v1(SWMM_Engine, size_t, SWMM_PassiveStatusV1*)
    int swmm_passive_exchange_pool_v1(SWMM_Engine, int, size_t, SWMM_PassivePoolV1*)
    int swmm_passive_exchange_receipt_v1(SWMM_Engine, uint64_t, size_t, SWMM_PassiveReceiptV1*)
    int swmm_passive_exchange_row_v1(SWMM_Engine, uint64_t, int, uint64_t, size_t, SWMM_PassiveRowV1*)
    int swmm_passive_exchange_leg_v1(SWMM_Engine, uint64_t, size_t, SWMM_PassiveLegV1*)
    int swmm_passive_exchange_attempt_v1(SWMM_Engine, size_t, SWMM_PassiveLegV1*)
    const char* swmm_passive_exchange_last_error()
