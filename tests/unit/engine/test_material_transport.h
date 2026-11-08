/* SPDX-License-Identifier: Apache-2.0 */
#ifndef TEST_MATERIAL_TRANSPORT_H
#define TEST_MATERIAL_TRANSPORT_H
#include <openswmm/engine/openswmm_split_material.h>
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Test adapter only. Modes: larger offer, wrong recipient, stale offer,
 * lost proof, duplicate proof, lost ACK, nonfinite encoded operand. */
typedef struct MaterialTestTransport {
    SWMM_Engine surface;
    int mode, offers, proofs, acknowledgements, reordered;
    int zero_offers, empty_proofs;
    uint64_t last_ordinal;
    char* response;
    char* saved_offer;
    char* saved_proof;
    FILE* log;
    SWMM_SplitFrame last_request;
    int last_phase;
} MaterialTestTransport;
int material_test_transport(SWMM_Engine,int,const char*,size_t,const char**,size_t*,void*);
int material_test_exchange(SWMM_Engine,int,SWMM_SplitFrame,SWMM_SplitFrame*,void*);
void material_test_transport_close(MaterialTestTransport*);
#ifdef __cplusplus
}
#endif
#endif
