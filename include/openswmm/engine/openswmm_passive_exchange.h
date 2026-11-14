/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OPENSWMM_PASSIVE_EXCHANGE_H
#define OPENSWMM_PASSIVE_EXCHANGE_H
#include "openswmm_engine.h"
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define SWMM_PASSIVE_EXCHANGE_VERSION 1
#define SWMM_PASSIVE_EXCHANGE_COMPONENTS 64
#define SWMM_PASSIVE_PHASE_SETTLED 0
#define SWMM_PASSIVE_PHASE_ADVANCE 1
#define SWMM_PASSIVE_PHASE_REPORT 2
#define SWMM_PASSIVE_PHASE_FINAL 3

/* Experimental integrated native reference, NOT a split/portable transfer.
 * Exactly one declared passive pollutant, isolated wet storage, closed flat
 * first-order serial CPU surface and no other material/source/forcing owner.
 * Select once after ordinary initialize/start(0), before the first step.
 * Earlier native paths and water split schemas remain separate alternatives.
 * Positive partial legs must conserve represented slice + remainder exactly;
 * unsupported arithmetic stops execution with retained diagnostic obligations.
 * Capacity is 1..65536; preflight is 0/1. Conservative history admission may
 * refuse a step before mutation. It does not predict the physical substep count.
 */
SWMM_ENGINE_API int swmm_passive_exchange_enable(SWMM_Engine, uint64_t capacity, int preflight);
/* Separate numerical checkpoint attachment at the initial enrolled boundary.
 * Delegates native profile checks; on refusal the coupling owner is retained.
 * Then use openswmm_checkpoint.h inspect/save/restore with matching enrollment
 * and capacity/preflight in a fresh target. Output continuation and full-engine
 * restart after end are not qualified by this facade. */
SWMM_ENGINE_API int swmm_passive_exchange_enable_checkpoint(SWMM_Engine);

/* Exact expansion, in canonical order. The exact SUM of count components is
 * authoritative; count==0 means zero. Unused slots are zero. No scalar fallback.
 * Amounts retain declared concentration times ft3, NOT kg. Surface leg amounts
 * retain declared concentration times m3. Neither record performs conversion. */
typedef struct SWMM_PassiveAmountV1 {
    uint64_t count;
    double components[SWMM_PASSIVE_EXCHANGE_COMPONENTS];
} SWMM_PassiveAmountV1;

typedef struct SWMM_PassiveStatusV1 {
    uint64_t receipt_count, leg_count, unsealed_leg_count;
    int pollutant, units; /* index 0; units: 0 MG/L, 1 UG/L, 2 #/L */
    int failed, window_open, delivery_frozen, history_preflight, checkpoint_attached, history_refused;
    double elapsed_ms, delivery_seconds;
} SWMM_PassiveStatusV1;

typedef struct SWMM_PassivePoolV1 {
    int node, units;
    double settled_volume_ft3, settled_depth_ft, settled_concentration;
    double settled_physical_amount; /* native rounded C*V; R is separate */
    double hydraulic_volume_ft3; /* may differ after an unsuccessful step */
    SWMM_PassiveAmountV1 roundoff, pending_drain, pending_spill; /* sealed only */
} SWMM_PassivePoolV1;

typedef struct SWMM_PassiveReceiptV1 {
    uint64_t id, row_count, first_leg, leg_count, generation;
    int phase;
    double capture_ms, start_ms, end_ms, eligible_ms, publication_delivery_seconds;
} SWMM_PassiveReceiptV1;

typedef struct SWMM_PassiveRowV1 {
    uint64_t point; /* original native coupling point, not the row ordinal */
    int node, units;
    SWMM_PassiveAmountV1 drain_water, spill_water, drain_amount, spill_amount;
} SWMM_PassiveRowV1;

typedef struct SWMM_PassiveLegV1 {
    uint64_t point;
    int cell, node, drain, applied, sealed, phase;
    double water_m3, source_amount, water_ft3, native_amount;
    /* after values are proposed when applied==0, observed when applied==1. */
    double source_before, source_after, sampled_concentration;
    double volume_before_m3, volume_after_m3, start_ms, end_ms, offset_seconds, duration_seconds;
    /* Current remainders apply only when sealed==1. Unsealed/attempt records
     * observe a producer leg, not a committed receiving reservation. */
    double remaining_water_ft3, remaining_native_amount;
} SWMM_PassiveLegV1;

/* Read-only copies; no ownership/funding authority is copied. Caller serializes
 * all access on the engine owner thread and never calls from callbacks. Reads
 * during active advancement refuse. Terminal/open-window observations remain
 * available after execution stops; they do not certify an accepted checkpoint.
 * Outputs are preserved on refusal, as is the engine's terminal diagnostic.
 * Pass sizeof the named V1 record. V1 layouts will not be extended in place.
 * No-2D builds return SWMM_ERR_PLUGIN after handle/output-pointer/size validation.
 * Pollutant identity is read through swmm_pollutant_id(engine,status.pollutant).
 */
SWMM_ENGINE_API int swmm_passive_exchange_status_v1(SWMM_Engine, size_t, SWMM_PassiveStatusV1*);
SWMM_ENGINE_API int swmm_passive_exchange_pool_v1(SWMM_Engine, int node, size_t, SWMM_PassivePoolV1*);
/* Receipt and row ordinals are zero-based. remaining must be 0 (accepted
 * original rows) or 1 (current remaining rows). */
SWMM_ENGINE_API int swmm_passive_exchange_receipt_v1(SWMM_Engine, uint64_t receipt, size_t, SWMM_PassiveReceiptV1*);
SWMM_ENGINE_API int swmm_passive_exchange_row_v1(SWMM_Engine, uint64_t receipt, int remaining,
                                               uint64_t row, size_t, SWMM_PassiveRowV1*);
/* Global journal order includes unsealed legs; receipt.first_leg locates its
 * immutable proof legs in this journal. Attempt may describe a refused leg
 * that was never appended. Before the first attempt it has no carrier and applied==0. */
SWMM_ENGINE_API int swmm_passive_exchange_leg_v1(SWMM_Engine, uint64_t leg, size_t, SWMM_PassiveLegV1*);
SWMM_ENGINE_API int swmm_passive_exchange_attempt_v1(SWMM_Engine, size_t, SWMM_PassiveLegV1*);
/* Thread-local UTF-8 diagnostic; overwritten by the next facade operation.
 * This getter does not clear it. It does not replace engine/checkpoint errors. */
SWMM_ENGINE_API const char* swmm_passive_exchange_last_error(void);
#ifdef __cplusplus
}
#endif
#endif
