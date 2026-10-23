/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file      openswmm_checkpoint.h
 * @brief     Public C API for creating, inspecting, and restoring simulation checkpoints.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_CHECKPOINT_H
#define OPENSWMM_CHECKPOINT_H
#include "openswmm_engine.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Native backend support, not GUI activation or a session-builder certificate. */
#define SWMM_CHECKPOINT_INSPECTION (1UL << 0)
#define SWMM_CHECKPOINT_RESTORE (1UL << 1)
#define SWMM_CHECKPOINT_SAVE_NEW (1UL << 2)
#define SWMM_CHECKPOINT_BINARY_PROFILE (1UL << 3)
#define SWMM_CHECKPOINT_SURFACE_PROFILE (1UL << 4)
#define SWMM_CHECKPOINT_SOURCE_STREAM (1UL << 5)
#define SWMM_CHECKPOINT_FILE_PROFILE (1UL << 6)
#define SWMM_CHECKPOINT_BUNDLE_EXPORT (1UL << 7)
#define SWMM_CHECKPOINT_SESSION_BUILDER (1UL << 8)

/* Cheap attachment/lifecycle hints; full compatibility remains validated by
 * inspect/restore/save. An ordinary initialized engine is not attached. */
#define SWMM_CHECKPOINT_SESSION_ATTACHED (1UL << 0)
#define SWMM_CHECKPOINT_SESSION_BOUNDARY (1UL << 1)
#define SWMM_CHECKPOINT_SESSION_FRESH (1UL << 2)
#define SWMM_CHECKPOINT_SESSION_MANAGED (1UL << 3)

#define SWMM_CHECKPOINT_OUTPUT_NONE 0
#define SWMM_CHECKPOINT_OUTPUT_BINARY 1
#define SWMM_CHECKPOINT_OUTPUT_MESH 2
#define SWMM_CHECKPOINT_OUTPUT_COMBINED 3
#define SWMM_CHECKPOINT_OUTPUT_FILES 4

typedef struct SWMM_CheckpointSessionOptions {
    size_t struct_size;           /**< Byte size; set to sizeof(SWMM_CheckpointSessionOptions). */
    unsigned long schema_version; /**< Options schema; must be 1. */
    int output;                   /**< SWMM_CHECKPOINT_OUTPUT_* profile, from NONE through FILES. */
    const char*
        source_history; /**< UTF-8 journal path; NULL/empty disables it; FILES requires one. */
    int detailed_source_intervals; /**< 0 or 1; 1 records detailed intervals and requires a journal. */
    const char*
        ownership_token; /**< Current reviewed spatial ownership token; NULL when no owners. */
} SWMM_CheckpointSessionOptions;

/* Explicit native-input profile, called in OPENED before ordinary initialize.
 * Initializes, prepares output, starts and attaches at the initial boundary.
 * Engine owns source arrays/graphs, stage and journal through close/destruction.
 * Reviewed spatial ownership requires the current authoring preview token.
 * Mesh sessions require the qualified CPU aquifer/common-source profile.
 * Qualified matched mesh transport supports first-order or RK2 advancement,
 * numerical/binary/mesh/combined output, and the documented ground profiles.
 * Native LEGACY pollutant networks without catchments, LIDs, treatment, native
 * heat/age or reactions can retain quality with first-order shared mesh exchange
 * or an independent RK2 mesh. Shared RK2 network exchange remains refused.
 * Other external/live forcing, physics and plugin recovery combinations retain
 * their separate profile guards.
 * Ordinary initialize keeps its existing ownership guard. No callbacks allowed.
 * Preflight refusal preserves OPENED; failure after initialization closes the
 * engine. Partial fresh outputs may remain; caller owns its private workspace.
 * Every output/journal must have a separate fresh protected destination. */
/* Profiles without binary output must open with out=NULL. Profiles without
 * mesh output must open without OUTPUT_FILE. For a mesh profile, set the
 * absolute fresh SWMM_FILE_OUTPUT_2D slot before this call; the managed builder
 * binds the built-in writer to that slot (including an OPENED path edit). */
/**
 * @brief Initialize and attach a managed checkpoint session at its initial boundary.
 *
 * See the profile and fresh-destination requirements above. A preflight refusal preserves OPENED; a
 * failure after initialization closes the engine. Retrieve the operation diagnostic with
 * swmm_checkpoint_last_error_msg().
 *
 * @param[in] engine Non-null engine in SWMM_STATE_OPENED; requires exclusive access on its owning
 *     thread.
 * @param[in] options Non-null options with struct_size >= sizeof(SWMM_CheckpointSessionOptions),
 *     schema_version=1, a SWMM_CHECKPOINT_OUTPUT_* profile and detailed_source_intervals equal to 0
 *     or 1. Strings are borrowed for this synchronous call and copied internally.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine, SWMM_ERR_BADPARAM for invalid
 *     schema/options or UTF-8, SWMM_ERR_LIFECYCLE for an invalid state or unavailable build
 *     profile, or SWMM_ERR_IO for a profile/preparation refusal. Allocation and unexpected
 *     exceptions map to SWMM_ERR_NOMEM and SWMM_ERR_INTERNAL.
 */
SWMM_ENGINE_API int swmm_checkpoint_start_session(SWMM_Engine engine,
                                                  const SWMM_CheckpointSessionOptions* options);

typedef void* SWMM_CheckpointInspection;

/**
 * @brief Query checkpoint features compiled into this backend.
 *
 * Capabilities describe backend support, not compatibility of a particular model or session.
 *
 * @param[in] engine Non-null live engine handle; no prepared checkpoint session is required.
 * @param[out] flags Non-null destination for the bitwise OR of supported SWMM_CHECKPOINT_* feature
 *     flags. Written only on success.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for null
 *     flags.
 */
SWMM_ENGINE_API int swmm_checkpoint_capabilities(SWMM_Engine engine, unsigned long* flags);
/**
 * @brief Read the current checkpoint attachment and boundary hints.
 *
 * These inexpensive hints do not replace full validation by inspect, save or restore.
 *
 * @param[in] engine Non-null live engine handle.
 * @param[out] flags Non-null destination for SWMM_CHECKPOINT_SESSION_* bits; written only on
 *     success.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for null
 *     flags.
 */
SWMM_ENGINE_API int swmm_checkpoint_session_status(SWMM_Engine engine, unsigned long* flags);

/* All engine calls are synchronous and require exclusive access to the handle
 * on its owning thread. Inspect/restore require a matching, explicitly prepared
 * fresh supported checkpoint session, including source/driver/output ownership.
 * These functions do not prepare that session or enable authored R4 processing.
 * Inspection validates and discards private copies without adopting/publishing
 * state. It may require substantial IO and cannot be interrupted. Publication
 * eligibility is checked again by restore, not certified by inspection.
 *
 * Success creates a new immutable snapshot, valid after engine close/destroy.
 * Failure preserves *snapshot. Caller owns every successful returned snapshot;
 * replacing its variable does not destroy a prior snapshot. */
/**
 * @brief Validate a checkpoint against a fresh session and return independent metadata.
 *
 * Inspection may perform substantial synchronous IO; it cannot be interrupted and does not adopt
 * checkpoint state. The returned snapshot survives engine close/destruction and must be released
 * with swmm_checkpoint_inspection_destroy(). Restore rechecks publication eligibility.
 *
 * @param[in] engine Non-null engine attached to a matching, explicitly prepared fresh checkpoint
 *     session.
 * @param[in] path Nonempty NUL-terminated UTF-8 checkpoint path, borrowed for the synchronous call.
 * @param[out] snapshot Non-null destination for a newly allocated immutable inspection handle.
 *     Preserved on failure; a previous handle is not freed when replaced.
 * @return SWMM_OK on success; SWMM_ERR_LIFECYCLE for a session that is not fresh/prepared,
 *     SWMM_ERR_BADHANDLE for a null engine, SWMM_ERR_BADPARAM for invalid path/output, SWMM_ERR_IO
 *     for validation/IO refusal, or SWMM_ERR_NOMEM/SWMM_ERR_INTERNAL for exceptions.
 */
SWMM_ENGINE_API int swmm_checkpoint_inspect(SWMM_Engine engine, const char* path,
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
/**
 * @brief Copy inspection metadata as a complete NUL-terminated UTF-8 JSON document.
 *
 * Schema 1 uses raw engine calendar dates and seconds, null optional fields and decimal strings for
 * uint64 counts/bytes/sequences. A driver update of -1 means exhausted; equality to engine_seconds
 * means due now. Metadata does not certify adapter availability.
 *
 * @param[in] snapshot Live inspection handle returned by swmm_checkpoint_inspect(); do not destroy
 *     concurrently.
 * @param[out] utf8 Caller-owned destination of capacity bytes, or NULL with capacity=0 to query the
 *     required size. No bytes are written when too small.
 * @param[in] capacity Destination capacity in bytes, including space for the terminating NUL.
 * @param[out] required Non-null destination for required bytes INCLUDING the terminating NUL.
 *     Updated for a size query or short buffer; preserved for invalid handle/arguments.
 * @return SWMM_OK after a size query or full copy; SWMM_ERR_BADHANDLE for null snapshot;
 *     SWMM_ERR_BADPARAM for null required, NULL buffer with nonzero capacity, or insufficient
 *     capacity.
 */
SWMM_ENGINE_API int swmm_checkpoint_inspection_json(SWMM_CheckpointInspection snapshot, char* utf8,
                                                    size_t capacity, size_t* required);
/**
 * @brief Release an inspection snapshot independently of engine lifetime.
 *
 * Does not clear the thread-local checkpoint diagnostic.
 *
 * @param[in] snapshot Snapshot owned by the caller, or NULL for a no-op. Every non-null handle must
 *     be destroyed exactly once; do not use it afterward.
 */
SWMM_ENGINE_API void swmm_checkpoint_inspection_destroy(SWMM_CheckpointInspection snapshot);

/* Save only at an accepted boundary. Atomically refuses an existing or
 * concurrently created destination, including symlinks/directories. Retains
 * successful companions; never cleans up older checkpoints. A recording/save
 * error does not imply rollback of previously accepted physics. */
/**
 * @brief Publish a new checkpoint exclusively at an accepted session boundary.
 *
 * Retains successful companions and older checkpoints. Recording/save failure does not roll back
 * previously accepted physics; the caller owns its private workspace and cleanup.
 *
 * @param[in] engine Non-null engine attached at an accepted checkpoint session boundary.
 * @param[in] new_path Nonempty NUL-terminated UTF-8 destination. It must not exist; files,
 *     directories, symlinks and concurrent creation are refused.
 * @return SWMM_OK after successful publication; SWMM_ERR_BADHANDLE/BADPARAM for invalid
 *     handle/path, SWMM_ERR_LIFECYCLE for an unprepared boundary or unavailable exclusive
 *     publication, SWMM_ERR_IO for a save refusal, or SWMM_ERR_NOMEM/SWMM_ERR_INTERNAL for
 *     exceptions.
 */
SWMM_ENGINE_API int swmm_checkpoint_save(SWMM_Engine engine, const char* new_path);
/**
 * @brief Restore a compatible checkpoint into an explicitly prepared fresh session.
 *
 * Does not prepare the required session. All engine operations require exclusive access on the
 * owning thread; an earlier successful inspection does not bypass restore validation.
 *
 * @param[in] engine Non-null engine attached to a matching fresh checkpoint session, including
 *     source, driver and output ownership.
 * @param[in] path Nonempty NUL-terminated UTF-8 checkpoint path, borrowed for this synchronous
 *     call.
 * @return SWMM_OK after restoration; SWMM_ERR_BADHANDLE/BADPARAM for invalid handle/path,
 *     SWMM_ERR_LIFECYCLE for a session that is not fresh/prepared, SWMM_ERR_IO for compatibility/IO
 *     refusal, or SWMM_ERR_NOMEM/SWMM_ERR_INTERNAL for exceptions.
 */
SWMM_ENGINE_API int swmm_checkpoint_restore(SWMM_Engine engine, const char* path);
/* Transport of an existing file-backed checkpoint to a new exclusive folder.
 * Does not certify model/provider compatibility; external inputs remain needed. */
/**
 * @brief Package an existing file-backed checkpoint in a new exclusive directory.
 *
 * Does not need an engine handle or certify model/provider compatibility. External inputs remain
 * necessary.
 *
 * @param[in] path Nonempty NUL-terminated UTF-8 source checkpoint path.
 * @param[in] new_folder Nonempty NUL-terminated UTF-8 destination folder that must not already
 *     exist.
 * @return SWMM_OK on successful export; SWMM_ERR_BADPARAM for invalid paths, SWMM_ERR_LIFECYCLE
 *     when the build lacks file-backed bundle/exclusive-publication support, SWMM_ERR_IO for
 *     packaging refusal, or SWMM_ERR_NOMEM/SWMM_ERR_INTERNAL for exceptions.
 */
SWMM_ENGINE_API int swmm_checkpoint_export_bundle(const char* path, const char* new_folder);

/* Specific diagnostic of the last checkpoint operation on THIS thread.
 * Getter/destroy do not clear it; next operation overwrites it. Copy before
 * leaving the thread. It is independent of simulation error state and remains
 * available after engine destruction. Long diagnostics may be truncated.
 * Validation refusals use IO; unprepared/busy/not-fresh/unsupported use LIFECYCLE;
 * bad pointers/path encoding use BADPARAM/BADHANDLE. Exceptions never escape. */
/**
 * @brief Borrow the diagnostic from the last checkpoint operation on this thread.
 *
 * The next checkpoint operation clears/overwrites the diagnostic; this getter and inspection
 * destruction do not. The buffer survives engine destruction, is separate from simulation errors,
 * and may truncate long messages.
 * @return Pointer to engine-independent thread-local NUL-terminated storage; empty when the last
 *     checkpoint operation succeeded. Never free it; copy it before another checkpoint operation or
 *     leaving the thread.
 */
SWMM_ENGINE_API const char* swmm_checkpoint_last_error_msg(void);

#ifdef __cplusplus
}
#endif
#endif
