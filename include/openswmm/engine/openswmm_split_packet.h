/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OPENSWMM_SPLIT_PACKET_H
#define OPENSWMM_SPLIT_PACKET_H
#include "openswmm_split.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Versioned, UTF-8 water-phase transaction. See docs/SPLIT_WATER_PACKET.md.
 * These operations copy data and never advance either engine. The handle
 * supplies the configured node ordering. Outputs are assigned only on success.
 * Release encoded text with swmm_split_packet_free and decoded snapshots with
 * swmm_split_frame_destroy. Unavailable builds return SWMM_ERR_PLUGIN. */
/* Authored eligibility check for an OPENED executable or metadata handle.
 * Resolved mesh geometry is checked during surface preparation. */
SWMM_ENGINE_API int swmm_split_validate_input(SWMM_Engine);
SWMM_ENGINE_API int swmm_split_packet_encode(SWMM_Engine, SWMM_SplitFrame, char** json);
SWMM_ENGINE_API int swmm_split_packet_decode(SWMM_Engine, const char* json, size_t length,
                                             SWMM_SplitFrame*);
/* Native phase code (0 for geometry), or -1 for a null frame. */
SWMM_ENGINE_API int swmm_split_packet_phase(SWMM_SplitFrame);
SWMM_ENGINE_API void swmm_split_packet_free(char*);
#ifdef __cplusplus
}
#endif
#endif
