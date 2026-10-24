/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file      GwRechargePluginAbi.h
 * @brief     C ABI for groundwater recharge plugins.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_GW_RECHARGE_PLUGIN_ABI_H
#define OPENSWMM_GW_RECHARGE_PLUGIN_ABI_H
#include "GpuPluginAbi.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Optional, independent extension to the surface ABI. Missing exports mean
 * CPU execution. Version1 is host-parallel CLOSED_FORM recharge only;
 * it never owns water, quality, heat, clocks or pending transfer receipts. */
#define OPENSWMM_GW_RECHARGE_ABI_VERSION 1
#define OPENSWMM_GW_OPERATOR_CLOSED_FORM_RECHARGE 1u
#define OPENSWMM_GW_HOST_WATER_AND_RECEIPTS 1u
#define OPENSWMM_GW_HOST_TRANSPORT 2u
#define OPENSWMM_GW_HOST_REACTIONS 4u
#define OPENSWMM_GW_HOST_THERMAL 8u
#define OPENSWMM_GW_EXECUTION_HOST_OPENMP 1u

typedef struct OpenSwmmGwCapabilitiesV1 {
    uint32_t struct_size, version, vendor, execution_kind;
    uint32_t operators, closures, soil_laws, host_owned;
    uint32_t time_integrators, canonical_host_restart;
    uint32_t max_layers, species_classes; /* 0: no layer/species ownership */
    char label[128];
} OpenSwmmGwCapabilitiesV1;

typedef struct OpenSwmmGwRechargeInputV1 {
    uint32_t struct_size;
    int32_t law;
    double Ks, theta_s, theta_r, alpha, psi_b, lambda, vg_n, vg_L;
    double length, hu; /* SI; immutable accepted owner state, not a trial receipt. */
} OpenSwmmGwRechargeInputV1;

/* Caller supplies struct_size. Exact version/shape must agree before use. */
/**
 * @brief Query an independent host recharge-operator capability record.
 * @par Argument contract
 * - Argument 1 (out, caller-owned): Required OpenSwmmGwCapabilitiesV1 record with struct_size
 * set to its exact current sizeof. Other fields are overwritten only after that shape check.
 * @return Zero on success; nonzero for unsupported/null record. OpenMP implementation returns
 * one for invalid storage/shape and leaves it unchanged.
 * @note Capability availability does not mean the operator has executed. Initializes no solver,
 * keeps no caller pointers, and transfers no ownership. Plugin library must remain loaded while
 * using its entry points.
 */
OPENSWMM_GPU_ABI int openswmm_gw_capabilities_v1(OpenSwmmGwCapabilitiesV1*);
/* Synchronous pure operator: no retained pointers or partially accepted state.
 * Output is raw m/s recharge; donor/ET/table budgets remain with the core. */
/**
 * @brief Evaluate a synchronous batch of independent CLOSED_FORM recharge cells.
 * @par Argument contract
 * - Argument 1 (in): Borrowed count-element host input array; NULL allowed only for count == 0.
 * Each struct_size is exact, law in [0,3], Ks finite nonnegative m/s, 0 <= theta_r < theta_s <=
 * 1, positive finite alpha 1/m, psi_b m, lambda, vg_n > 1, finite vg_L, positive length m and
 * nonnegative hu m of water.
 * - Argument 2 (in): Nonnegative cell count.
 * - Argument 3 (out): Caller-owned count-double host array, NULL only for zero count. Receives
 * raw recharge in m/s, positive downward and negative capillary rise. Must not overlap inputs.
 * - Argument 4 (in): Requested host worker count; positive requests participate in
 * first-initialization selection, nonpositive uses defaults, and OPENSWMM_2D_THREADS takes
 * precedence.
 * @return OpenMP implementation: zero on success/empty batch, one for input validation failure,
 * two for runtime exceptions or nonfinite output.
 * @note Validates all inputs before writing, but a runtime failure may leave partial output.
 * Host-parallel work is fenced before returning. Calls are serialized by a recharge mutex;
 * initialization uses a separate mutex and its effective thread count is process-wide. No
 * accepted water/quality/heat state is committed and no pointers are retained.
 */
OPENSWMM_GPU_ABI int openswmm_gw_recharge_v1(const OpenSwmmGwRechargeInputV1*, int, double*, int);
#ifdef __cplusplus
}
#endif
#endif
