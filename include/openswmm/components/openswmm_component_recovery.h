/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OPENSWMM_COMPONENT_RECOVERY_H
#define OPENSWMM_COMPONENT_RECOVERY_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define OPENSWMM_COMPONENT_RECOVERY_SYMBOL "openswmm_component_material_recovery"
#define OPENSWMM_COMPONENT_RECOVERY_VERSION 1u
#define OPENSWMM_COMPONENT_RECOVERY_STATE_VERSION 2u
#define OPENSWMM_COMPONENT_RECOVERY_MAX_BYTES (65u*1024u*1024u)
typedef struct OpenSWMM_MaterialGroupOpaque* OpenSWMM_MaterialGroup;
typedef struct OpenSWMM_MaterialStateOpaque* OpenSWMM_MaterialState;

/* Optional service of the ABI-checked engine COMPONENT MODULE, not libopenswmm.
 * Discover only through the same loaded module that created both components.
 * Check the HydroCouple component ABI stamp first. Pass the exact canonical
 * HydroCouple::IModelComponent* base address as void*; arbitrary/expired pointers,
 * pointers from a different ABI/module and concurrent access are caller errors.
 * No SDK types, native engine handles or restored callback authority cross here.
 *
 * Enroll exactly one reciprocal material network/surface pair at Valid before
 * either prepare. Both components must outlive group operations; a group borrows
 * them through guarded lifetimes. Duplicate enrollment is refused. outputs=0
 * suppresses native result writers; outputs=1 enables network binary + surface
 * HDF5 continuation, with no text reports. Prepare either order normally.
 * After both component preparations, call prepare(group) once to bind/enroll
 * the zero-time pair before restore or capture. Ordinary updates can also bind.
 * Enrollment is sticky until component reinitialization, even after group release.
 * The host indexes BOTH stable member IDs to ONE group and saves it once at an
 * idle synchronized Updated barrier. No ICheckpointableModelComponent capability
 * is conferred. The service covers no third component, adapter or general graph.
 *
 * Capture requires a healthy accepted RUNNING pair; restore requires both freshly
 * prepared at time zero, before any host update, and preserves fresh destinations.
 * It validates membership, observer history and native clock before one collective
 * numerical/output commit. Refusal preserves the target views/prefixes for retry.
 * Both clocks/views/statuses are installed before postcommit notifications, under
 * one pair lease. Listener/notification failures cannot undo committed physics.
 * No postcommit filesystem writes are attempted; the host may persist live views.
 *
 * State is an immutable owned binary value independent of groups/components.
 * import_state COPIES bounded caller bytes; data borrows until destroy_state.
 * Imported outer framing/history does not certify native model compatibility;
 * full validation occurs at restore. Checksums are not authentication. Same-build,
 * same-platform experimental format, reusable into independently fresh pairs.
 * Old receipt/session observations are explicitly historical, never authority.
 * Keep the creating module loaded through all group/state destruction.
 * destroy_group never finishes/closes components; a stale group safely refuses
 * operations after either component closes. Destroy handles exactly once, NULL OK.
 *
 * All calls return SWMM_OK or SWMM_ERR_* (openswmm_engine.h), preserve result
 * arguments on failure, and contain exceptions. last_error is thread-local text
 * borrowed until the next service call; it does not replace native root errors.
 * All access, including destruction, is serialized by the host. Description/data
 * are immutable observations and allowed during callbacks; mutations are refused.
 */
typedef struct OpenSWMM_MaterialRecoveryV1 {
    size_t struct_size;
    uint32_t version;
    int (*enroll)(void* network,void* surface,int outputs,OpenSWMM_MaterialGroup* group);
    int (*describe)(OpenSWMM_MaterialGroup group,const char** json,size_t* size);
    int (*prepare)(OpenSWMM_MaterialGroup group);
    int (*capture)(OpenSWMM_MaterialGroup group,OpenSWMM_MaterialState* state);
    int (*restore)(OpenSWMM_MaterialGroup group,OpenSWMM_MaterialState state);
    int (*import_state)(const void* bytes,size_t size,OpenSWMM_MaterialState* state);
    int (*data)(OpenSWMM_MaterialState state,const void** bytes,size_t* size);
    void (*destroy_group)(OpenSWMM_MaterialGroup group);
    void (*destroy_state)(OpenSWMM_MaterialState state);
    const char* (*last_error)(void);
} OpenSWMM_MaterialRecoveryV1;
/* V2 preserves the V1 function prefix and adds immutable state observation.
 * Request version=2 and sizeof(OpenSWMM_MaterialRecoveryV2); the discovery
 * function returns &table->base, whose version=2 and struct_size=sizeof(V2).
 * V1 requests retain exactly their original table/version/size. No HC ABI change.
 * describe_state borrows a JSON summary until state destruction, even after group
 * or component destruction. Schema openswmm.component-material-state/1 contains
 * group (the logical descriptor), accepted_steps and accepted_time_s. These are
 * archived observations, not authority or proof of native model compatibility.
 * Hosts join their manifest counters/members to this summary BEFORE restore;
 * restore still validates full observer history and native clock before commit.
 * The complete package remains same-build/same-platform, not authenticated.
 */
typedef struct OpenSWMM_MaterialRecoveryV2 {
    OpenSWMM_MaterialRecoveryV1 base;
    int (*describe_state)(OpenSWMM_MaterialState state,const char** json,size_t* size);
} OpenSWMM_MaterialRecoveryV2;
/* Returns NULL for unsupported version or caller table size, without mutation. */
typedef const OpenSWMM_MaterialRecoveryV1* (*OpenSWMM_MaterialRecoveryFn)(uint32_t version,size_t caller_size);
#ifdef __cplusplus
}
#endif
#endif
