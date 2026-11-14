/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OPENSWMM_SPLIT_MATERIAL_H
#define OPENSWMM_SPLIT_MATERIAL_H
#include "openswmm_split.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#define SWMM_SPLIT_MATERIAL_REPORT 8
#define SWMM_MATERIAL_OFFER 1
#define SWMM_MATERIAL_PROOF 2
#define SWMM_MATERIAL_ACK 3
#define SWMM_MATERIAL_PACKET_MAX_BYTES (64u * 1024u * 1024u)

/* Experimental trusted IN-PROCESS adapter for one exclusive native pair.
 * It retains the qualified closed one-pollutant serial CPU storage profile.
 * Select both roles while OPENED, exchange initialized geometry using the
 * material codec, set the ordinary split callback, start both, then bind once.
 * No SDK or remote authentication. Paired checkpoints use the separate binary
 * API below; ordinary single-role checkpoint and GUI resume remain unavailable.
 * The existing water configuration and schemas 1-9 remain separate.
 *
 * The restricted callback runs synchronously on the network owner thread at
 * the actual surface window: OFFER before credit, PROOF before receiving
 * preparation, ACK after commitment. It returns the entire decoded value.
 * Authority stays in a retained unique native capsule and is redeemed once
 * only after exact comparison with that issuance/session/sequence/window.
 * JSON decoding, copying and inspection never grant spending authority.
 *
 * request is borrowed. The response may alias request, or use caller-owned
 * storage valid AFTER callback return until immediately parsed by the library.
 * Never return callback-local stack storage. The library does not free it.
 * No reentrant stepping/evaluation, pair teardown or callback changes are
 * permitted. Codec calls are allowed. Callback/user storage outlives the pair.
 * Lost proof retains the full grant; lost ACK retains the committed receipt
 * and the producer's pending acknowledgement. Both failures stop the session.
 */
typedef int (*SWMM_MaterialTransport)(SWMM_Engine network, int stage,
    const char* request, size_t length, const char** response, size_t* response_length, void* user);
/* Read-only authored eligibility on an OPENED executable or metadata handle.
 * No role, geometry, runtime owner, clock or output is created by this check. */
SWMM_ENGINE_API int swmm_split_material_validate_input(SWMM_Engine);
SWMM_ENGINE_API int swmm_split_material_configure(SWMM_Engine, int role);
SWMM_ENGINE_API int swmm_split_material_set_transport(SWMM_Engine, SWMM_MaterialTransport, void*);
SWMM_ENGINE_API int swmm_split_material_bind(SWMM_Engine network, SWMM_Engine surface, uint64_t capacity);

#define SWMM_MATERIAL_CHECKPOINT_NUMERICAL (1UL << 0)
#define SWMM_MATERIAL_CHECKPOINT_OUTPUTS (1UL << 1)
#define SWMM_MATERIAL_CHECKPOINT_MAX_BYTES (64u * 1024u * 1024u)
typedef struct SWMM_MaterialCheckpointOpaque* SWMM_MaterialCheckpoint;

/* Compiled backend features, not a certificate for a model, component or graph.
 * A build without material support succeeds with flags=0. All other operations
 * below return SWMM_ERR_PLUGIN there, after argument checks. */
SWMM_ENGINE_API int swmm_split_material_checkpoint_capabilities(SWMM_Engine, unsigned long* flags);
/* Explicit material roles only, INITIALIZED before start; one call per role.
 * Prepares the network built-in binary writer or surface pollutant HDF5 writer.
 * Both must have separate fresh destinations, no text reports or averaging.
 * Then start network with start(1), surface with start(0), and bind as usual.
 * Outputs-off sessions skip preparation and start both roles with start(0). */
SWMM_ENGINE_API int swmm_split_material_checkpoint_prepare_outputs(SWMM_Engine);
/* The NETWORK handle exclusively coordinates its distinct bound surface.
 * Enable once at the fresh RUNNING boundary after binding, outputs=0 or 1.
 * No active callback, step, report, transport or pipeline may access engine
 * recovery. Immutable snapshot data access/import is independent of engines.
 * Capture requires an accepted acknowledged RUNNING boundary. Restore requires
 * two fresh identically configured/bound/enrolled RUNNING roles at time zero,
 * including fresh prepared writers for outputs=1. Both numerical roles and
 * writers validate before any publication/install; ordinary refusal allows a
 * valid retry. Runtime transport failure remains terminal and cannot be retried.
 * Original callbacks/channels remain target-local. No per-role, graph or in-flight
 * rollback is implied. Existing native arithmetic/work/size limits still apply. */
SWMM_ENGINE_API int swmm_split_material_checkpoint_enable(SWMM_Engine network, int outputs);
SWMM_ENGINE_API int swmm_split_material_checkpoint_capture(SWMM_Engine network, SWMM_MaterialCheckpoint* snapshot);
SWMM_ENGINE_API int swmm_split_material_checkpoint_restore(SWMM_Engine network, SWMM_MaterialCheckpoint snapshot);
/* Collective component clock join. Finite nonnegative expected_seconds must exactly
 * equal the incoming elapsed_ms/1000 before any numerical/output publication.
 * All other ownership, fresh-target and failure guarantees are those of restore. */
SWMM_ENGINE_API int swmm_split_material_checkpoint_restore_at(SWMM_Engine network, SWMM_MaterialCheckpoint snapshot, double expected_seconds);
/* Snapshots are immutable independent binary values, valid after engine close.
 * Import copies 1..MAX_BYTES caller bytes and validates outer framing, native
 * magic/version/endian/word size and checksum. Full model/numerical/output-prefix
 * compatibility is checked only during restore. JSON/schema10 is not a checkpoint.
 * Snapshots may seed multiple separately fresh pairs; saved IDs grant no authority.
 * Bytes are same-platform native archives, with no cross-platform portability.
 * Success assigns a new owned snapshot, without freeing any previous *snapshot.
 * All errors preserve every output argument. Borrowed data remains valid until
 * snapshot destruction; it is not NUL-terminated text. Do not mutate/free it.
 * No operation is safe concurrently with destruction or another engine operation. */
SWMM_ENGINE_API int swmm_split_material_checkpoint_from_bytes(const void* bytes, size_t length, SWMM_MaterialCheckpoint* snapshot);
SWMM_ENGINE_API int swmm_split_material_checkpoint_data(SWMM_MaterialCheckpoint snapshot, const void** bytes, size_t* length);
SWMM_ENGINE_API void swmm_split_material_checkpoint_destroy(SWMM_MaterialCheckpoint snapshot);
/* Separate no-throw TLS diagnostic; never overwrites either engine's root error.
 * Null handles => BADHANDLE; invalid outputs/mode/binary envelope => BADPARAM;
 * unavailable feature => PLUGIN; wrong lifecycle/role/enrollment => LIFECYCLE;
 * native capture/restore validation or output refusal => IO. Facade allocation
 * exceptions => NOMEM; other unexpected exceptions => INTERNAL. */
SWMM_ENGINE_API const char* swmm_split_material_checkpoint_last_error(void);

/* Separate schema10 codec. Boundary frames retain native geometry, conductance,
 * ADVANCE, settled REPORT (8) and FINISH. Codecs are side-effect-free and assign
 * output handles only on success. Copy validates and canonicalizes a complete
 * boundary/offer/proof/ack value; it does not redeem a native capsule.
 * uint64 identities use canonical decimal strings; physical doubles use finite
 * IEEE754 binary64 bit strings. Exact amounts retain <=64 original ordered
 * native components; receipt rows retain their existing canonical checks.
 * Release text with swmm_split_packet_free, frames with swmm_split_frame_destroy.
 * Diagnostic storage is thread-local; codec refusal preserves engine errors.
 * No-2D/no-component builds return SWMM_ERR_PLUGIN after argument validation.
 */
SWMM_ENGINE_API int swmm_split_material_packet_encode(SWMM_Engine, SWMM_SplitFrame, char**);
SWMM_ENGINE_API int swmm_split_material_packet_decode(SWMM_Engine, const char*, size_t, SWMM_SplitFrame*);
SWMM_ENGINE_API int swmm_split_material_packet_copy(SWMM_Engine, const char*, size_t, char**);
SWMM_ENGINE_API const char* swmm_split_material_last_error(void);
#ifdef __cplusplus
}
#endif
#endif
