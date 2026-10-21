/* SPDX-License-Identifier: Apache-2.0 */
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
    uint32_t struct_size,version,vendor,execution_kind;
    uint32_t operators,closures,soil_laws,host_owned;
    uint32_t time_integrators,canonical_host_restart;
    uint32_t max_layers,species_classes; /* 0: no layer/species ownership */
    char label[128];
} OpenSwmmGwCapabilitiesV1;

typedef struct OpenSwmmGwRechargeInputV1 {
    uint32_t struct_size;
    int32_t law;
    double Ks,theta_s,theta_r,alpha,psi_b,lambda,vg_n,vg_L;
    double length,hu; /* SI; immutable accepted owner state, not a trial receipt. */
} OpenSwmmGwRechargeInputV1;

/* Caller supplies struct_size. Exact version/shape must agree before use. */
OPENSWMM_GPU_ABI int openswmm_gw_capabilities_v1(OpenSwmmGwCapabilitiesV1*);
/* Synchronous pure operator: no retained pointers or partially accepted state.
 * Output is raw m/s recharge; donor/ET/table budgets remain with the core. */
OPENSWMM_GPU_ABI int openswmm_gw_recharge_v1(const OpenSwmmGwRechargeInputV1*,int,double*,int);
#ifdef __cplusplus
}
#endif
#endif
