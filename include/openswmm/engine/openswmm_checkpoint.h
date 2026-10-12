/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OPENSWMM_CHECKPOINT_H
#define OPENSWMM_CHECKPOINT_H
#include "openswmm_engine.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Native backend support, not GUI activation or a session-builder certificate. */
#define SWMM_CHECKPOINT_INSPECTION       (1UL << 0)
#define SWMM_CHECKPOINT_RESTORE          (1UL << 1)
#define SWMM_CHECKPOINT_SAVE_NEW         (1UL << 2)
#define SWMM_CHECKPOINT_BINARY_PROFILE   (1UL << 3)
#define SWMM_CHECKPOINT_SURFACE_PROFILE  (1UL << 4)
#define SWMM_CHECKPOINT_SOURCE_STREAM    (1UL << 5)
#define SWMM_CHECKPOINT_FILE_PROFILE     (1UL << 6)
#define SWMM_CHECKPOINT_BUNDLE_EXPORT    (1UL << 7)

/* Cheap attachment/lifecycle hints; full compatibility remains validated by
 * inspect/restore/save. An ordinary initialized engine is not attached. */
#define SWMM_CHECKPOINT_SESSION_ATTACHED (1UL << 0)
#define SWMM_CHECKPOINT_SESSION_BOUNDARY (1UL << 1)
#define SWMM_CHECKPOINT_SESSION_FRESH    (1UL << 2)

typedef void* SWMM_CheckpointInspection;

SWMM_ENGINE_API int swmm_checkpoint_capabilities(SWMM_Engine, unsigned long* flags);
SWMM_ENGINE_API int swmm_checkpoint_session_status(SWMM_Engine, unsigned long* flags);

/* All engine calls are synchronous and require exclusive access to the handle
 * on its owning thread. Inspect/restore require a matching, explicitly prepared
 * fresh water-only checkpoint session, including source/driver/output ownership.
 * These functions do not prepare that session or enable authored R4 processing.
 * Inspection validates and discards private copies without adopting/publishing
 * state. It may require substantial IO and cannot be interrupted. Publication
 * eligibility is checked again by restore, not certified by inspection.
 *
 * Success creates a new immutable snapshot, valid after engine close/destroy.
 * Failure preserves *snapshot. Caller owns every successful returned snapshot;
 * replacing its variable does not destroy a prior snapshot. */
SWMM_ENGINE_API int swmm_checkpoint_inspect(SWMM_Engine, const char* path,
                                           SWMM_CheckpointInspection* snapshot);

/* Schema 1: raw engine calendar dates and seconds; optional fields are null;
 * uint64 counts/bytes/sequences are decimal strings. Driver update -1 means
 * exhausted; equality with engine_seconds means due now. This is metadata,
 * not a checkpoint format, timezone conversion or proof of adapter availability.
 *
 * NULL utf8 with zero capacity queries required bytes INCLUDING terminating NUL.
 * Short capacity returns BADPARAM, sets required and writes no buffer bytes.
 * Invalid arguments preserve required. Success writes the complete document.
 * Snapshot access requires a live returned handle, not concurrent destruction. */
SWMM_ENGINE_API int swmm_checkpoint_inspection_json(SWMM_CheckpointInspection,
                                                   char* utf8, size_t capacity,
                                                   size_t* required);
SWMM_ENGINE_API void swmm_checkpoint_inspection_destroy(SWMM_CheckpointInspection);

/* Save only at an accepted boundary. Atomically refuses an existing or
 * concurrently created destination, including symlinks/directories. Retains
 * successful companions; never cleans up older checkpoints. A recording/save
 * error does not imply rollback of previously accepted physics. */
SWMM_ENGINE_API int swmm_checkpoint_save(SWMM_Engine, const char* new_path);
SWMM_ENGINE_API int swmm_checkpoint_restore(SWMM_Engine, const char* path);
/* Transport of an existing file-backed checkpoint to a new exclusive folder.
 * Does not certify model/provider compatibility; external inputs remain needed. */
SWMM_ENGINE_API int swmm_checkpoint_export_bundle(const char* path, const char* new_folder);

/* Specific diagnostic of the last checkpoint operation on THIS thread.
 * Getter/destroy do not clear it; next operation overwrites it. Copy before
 * leaving the thread. It is independent of simulation error state and remains
 * available after engine destruction. Long diagnostics may be truncated.
 * Validation refusals use IO; unprepared/busy/not-fresh/unsupported use LIFECYCLE;
 * bad pointers/path encoding use BADPARAM/BADHANDLE. Exceptions never escape. */
SWMM_ENGINE_API const char* swmm_checkpoint_last_error_msg(void);

#ifdef __cplusplus
}
#endif
#endif
