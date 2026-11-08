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
 * No SDK, remote authentication, retry, paired checkpoint or GUI activation.
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
