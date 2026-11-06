/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OPENSWMM_SPLIT_H
#define OPENSWMM_SPLIT_H
#include "openswmm_engine.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define SWMM_SPLIT_NETWORK 1
#define SWMM_SPLIT_SURFACE 2
#define SWMM_SPLIT_CONDUCTANCE 1
#define SWMM_SPLIT_ADVANCE 2
#define SWMM_SPLIT_FINISH 3
#define SWMM_SPLIT_OUTFALL_PREPARE 4
#define SWMM_SPLIT_SOURCE_PREPARE 5
#define SWMM_SPLIT_LID_TRIAL 6
#define SWMM_SPLIT_LID_ACCEPT 7

/** Immutable, owned native-peer phase snapshot. No engine/context is shared.
 * The snapshot retains native floating-point values without a lossy SI round
 * trip. It is valid only for matching native configurations in the same process;
 * it is not a portable HydroCouple transfer item, checkpoint or wire format.
 * Public inspection below returns SI. Do not destroy a borrowed callback request.
 */
typedef void* SWMM_SplitFrame;
/** Synchronous on the network's owner thread. Request is borrowed until return.
 * Return one newly owned reply from swmm_split_evaluate(); the caller consumes
 * and destroys it even on error. No other network mutation/reentrant stepping
 * is permitted. Callback, user and receiving engine must outlive the run.
 * Failure stops after any already-completed physics; there is no rollback/retry.
 */
typedef int (*SWMM_SplitCallback)(SWMM_Engine network, int phase,
    SWMM_SplitFrame request, SWMM_SplitFrame* reply, void* user);

/** Select once in executable OPENED, before binding private output paths.
 * Profile: native CPU first-order DYNWAVE, water only, ordinary watershed and
 * conventional LID outlets, and MESH Gardner groundwater with explicit closure
 * and GW_ET NONE, or the reviewed common-source profile. Reviewed footprints
 * retain their native source/bridge owner in the surface participant. This
 * profile requires mesh results, wall boundaries and no ordinary groundwater
 * node/link beds. Qualified mapped outfalls retain native reservations; returns
 * into reviewed catchments carry native accepted water at interval closure.
 * Reviewed source profiles additionally support water-only MESH_AQUIFER Richards
 * node LIDs through explicit trial/acceptance and accepted hydraulic-port finish.
 * Classical node LIDs retain native hydraulic preparation/port finish; their
 * accepted layer and Green-Ampt states accompany groundwater settlement.
 * Non-Mesh Richards node LIDs, snow,
 * tracking ponds, events, plugins or process rows.
 * Network retains 1D routing and disables its embedded surface owner. Surface
 * retains the native surface solver and disables 1D routing. For watershed
 * models the network alone advances source/weather clocks and sends the native
 * gage journal and evaporation; the surface never runs duplicate runoff/LIDs.
 * Groundwater runs only in the surface participant; native node queues and
 * conduit budgets/accepted seepage cross the boundary without a second solver.
 * Input must omit OUTPUT_FILE; bind a private surface file after selection.
 * Keep model configuration fixed after selection. Resolved mesh eligibility is
 * checked by geometry export before start. This API does not load HydroCouple
 * or its SDK. This experimental native-peer API does not advertise CF4/Composer
 * or general conservative-transfer qualification.
 */
SWMM_ENGINE_API int swmm_split_configure(SWMM_Engine engine, int role);
/** Export initialized surface geometry; import it to the initialized network
 * before start. Both instances must have the same original configuration.
 * Geometry uses the native footprint calculation, not a second implementation.
 */
SWMM_ENGINE_API int swmm_split_geometry(SWMM_Engine surface, SWMM_SplitFrame* frame);
SWMM_ENGINE_API int swmm_split_apply_geometry(SWMM_Engine network, SWMM_SplitFrame frame);
SWMM_ENGINE_API int swmm_split_set_callback(SWMM_Engine network, SWMM_SplitCallback callback,
                                            void* user);
/** Evaluate native conductance without advancing, or advance exactly the
 * requested native routing interval, or finish the surface owner. A successful
 * advance preserves native batching and report flushes. Ordinary step/advance
 * calls on a surface-role engine are refused; advance_to is unavailable for
 * both roles until its additional synchronization flush is represented.
 * Replies are sequence/phase/participant bound.
 * Output handle is preserved on failure. Exclusive receiving-engine access is
 * required. Geometry/callback setup precedes start of either instance.
 */
SWMM_ENGINE_API int swmm_split_evaluate(SWMM_Engine surface, int phase,
                                        SWMM_SplitFrame request, SWMM_SplitFrame* reply);
SWMM_ENGINE_API void swmm_split_frame_destroy(SWMM_SplitFrame frame);
/** Read one native-node row as SI: head/depth m, queued/direct pending m3,
 * and remaining uniform delivery duration s. Inspection never advances time.
 */
SWMM_ENGINE_API int swmm_split_frame_node(SWMM_SplitFrame frame, int node,
    double* head_m, double* depth_m, double* queued_m3, double* direct_m3,
    double* delivery_seconds);
SWMM_ENGINE_API int swmm_split_frame_time(SWMM_SplitFrame frame, uint64_t* sequence,
    double* start_seconds, double* duration_seconds);
/** Read a reviewed-source receipt boundary. Rates are native ft3/s; pending
 * runoff/drain are ft3 held by the native bridge for a future receiving interval.
 * Geometry starts with zero rates. ADVANCE/FINISH retain the last accepted offer.
 * Receipt/geometry only; unchanged outputs on invalid/unavailable rows. */
SWMM_ENGINE_API int swmm_split_frame_source(SWMM_SplitFrame frame, int node,
    double* rate_cfs, double* previous_rate_cfs, double* pending_runoff_ft3,
    double* pending_drain_ft3);
/** Inspect one accepted reviewed-source outfall return in an ADVANCE request or
 * receipt. Native ft3, including zero rows; unchanged output when unavailable. */
SWMM_ENGINE_API int swmm_split_frame_return(SWMM_SplitFrame frame, int subcatchment,
                                           double* volume_ft3);
#ifdef __cplusplus
}
#endif
#endif
