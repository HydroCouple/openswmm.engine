/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OPENSWMM_COUPLING_H
#define OPENSWMM_COUPLING_H
#include "openswmm_engine.h"
#ifdef __cplusplus
extern "C" {
#endif
#define SWMM_COUPLING_SURFACE 0
#define SWMM_COUPLING_GROUNDWATER 1
/** Version 1: per-cell sources. Arrays follow the target domain's species
 * order. Concentrations use the same units as its concentration getter;
 * rates use signed concentration-unit*m3/s (negative removes available mass).
 * Reserved age is seconds and temperature
 * is degrees C. Reserved-row rates must be zero: use heat_w for energy.
 * flow_m3_s is positive INTO the domain; negative extracts donor quality.
 * Groundwater sources address the saturated zone. Heat is water sensible
 * heat, NOT soil conduction or heat storage. until_seconds=0 holds until clear.
 * All pointers are copied; caller retains ownership. */
typedef struct SWMM_CouplingSource {
    unsigned struct_size;
    const char* id;
    int domain;
    int cell;
    double flow_m3_s;
    double heat_w;
    double until_seconds;
    int species_count;
    const double* concentrations;
    const double* rates;
} SWMM_CouplingSource;
/** Signed cumulative receipt. Rejected=requested-applied; heat is J.
 * Species arrays are retrieved separately, in concentration-unit*m3.
 * Clearing/replacing preserves cumulative receipts for the same id/cell.
 * A groundwater ID matching an authored well overrides it in that cell;
 * clear/expiry resumes the authored prescription. Different IDs add. */
typedef struct SWMM_CouplingReceipt {
    double requested_m3, applied_m3, rejected_m3;
    double requested_heat_j, applied_heat_j, rejected_heat_j;
    double last_applied_m3_s, last_interval_seconds;
} SWMM_CouplingReceipt;
#define SWMM_COUPLING_FRAME_VERSION 1
#define SWMM_COUPLING_RAINFALL 1
#define SWMM_COUPLING_EVAPORATION 2
#define SWMM_COUPLING_INFILTRATION 3
#define SWMM_COUPLING_RAIN_QUALITY 4
/** Boundary kind: 0 restores authored/native, 1 datum head in m, 2 outward m3/s. */
typedef struct SWMM_CouplingBoundary {
    unsigned struct_size;
    int domain, cell, edge, kind;
    double value;
    int species_count;
    const double* concentrations;
} SWMM_CouplingBoundary;
/** Forcing rate in m/s. mode: 0 clear, 1 replace, 2 add (rain/evap only).
 * cell=-1 addresses all cells for rain/evap/quality; infiltration is per-cell,
 * replaces native infiltration and transfers donor water/quality to GW once.
 * species_count=0 keeps authored rainfall quality; a full tuple overrides it.
 * until_seconds=0 holds until clear. Quality-only channel ignores rate. */
typedef struct SWMM_CouplingForcing {
    unsigned struct_size;
    int channel, cell, mode;
    double rate_m_s, until_seconds;
    int species_count;
    const double* concentrations;
} SWMM_CouplingForcing;
typedef struct SWMM_CouplingClear {
    int domain, cell; /* cell=-1 clears all cells for this source ID. */
    const char* id;
} SWMM_CouplingClear;
/** Copied, atomic cross-domain transaction. provider=NULL/empty denotes legacy
 * ownership. Named providers namespace source IDs as provider/<provider>/<id>
 * and exclusively own each boundary/forcing until clear/expiry. Provider names
 * cannot contain '/'. Rejections leave clock and all prescriptions unchanged.
 * Native authored boundaries/forcing resume after clear. */
typedef struct SWMM_CouplingFrame {
    unsigned struct_size, version;
    const char* provider;
    int source_count;
    const SWMM_CouplingSource* sources;
    int boundary_count;
    const SWMM_CouplingBoundary* boundaries;
    int forcing_count;
    const SWMM_CouplingForcing* forcings;
    int clear_count;
    const SWMM_CouplingClear* clears;
} SWMM_CouplingFrame;
SWMM_ENGINE_API int swmm_coupling_apply_frame(SWMM_Engine, const SWMM_CouplingFrame*);
SWMM_ENGINE_API int swmm_coupling_clear_source_cell(SWMM_Engine, int domain, const char* id, int cell);
/** Bulk receipt reads; arrays are [record][species] in the selected domain order.
 * Every record is validated before writing any outputs. */
SWMM_ENGINE_API int swmm_coupling_get_receipts(SWMM_Engine, int domain, const char* id,
    const int* cells, int count, SWMM_CouplingReceipt*, double* requested_species,
    double* applied_species, int species_count);
/** Boundary receipts are positive INTO the domain; heat is total advective J.
 * Clearing preserves cumulative history. Native no-flow edges return zeros. */
SWMM_ENGINE_API int swmm_coupling_get_boundary_receipt(SWMM_Engine, int domain, int cell, int edge,
    SWMM_CouplingReceipt*, double* requested_species, double* applied_species, int species_count);
/** Paired-infiltration receipts: positive surface-to-GW; heat is advective J. */
SWMM_ENGINE_API int swmm_coupling_get_infiltration_receipt(SWMM_Engine, int cell,
    SWMM_CouplingReceipt*, double* requested_species, double* applied_species, int species_count);
/** Surface species ledger terms in native concentration-unit*m3:
 * 0 storage, 1 external in, 2 external out, 3 boundary in, 4 boundary out,
 * 5 rainfall in, 6 infiltration out, 7 native coupling in, 8 native coupling out,
 * 9 exfiltration in. */
SWMM_ENGINE_API int swmm_2d_get_species_ledger(SWMM_Engine, int species, int term, double* value);

/** Scalar convenience with the identical tuple/validation/receipt contract. */
SWMM_ENGINE_API int swmm_coupling_set_source(SWMM_Engine, const char* id, int domain, int cell,
    double flow_m3_s, double heat_w, const double* concentrations, const double* rates,
    int species_count, double until_seconds);
/** Saturated GW perimeter boundary: kind 1=head (m), 2=flow (m3/s, outward).
 * Incoming water carries the supplied concentrations; outgoing water uses donor
 * quality. Head uses a one-sided unconfined Darcy face; no solute diffusion or
 * soil thermal boundary is implied. Receipt source ID is boundary/<cell>/<edge>.
 * Clearing restores the native no-flow lateral boundary. */
SWMM_ENGINE_API int swmm_gw2d_set_runtime_boundary(SWMM_Engine, int cell, int edge,
    int kind, double value, const double* concentrations, int species_count);
SWMM_ENGINE_API int swmm_gw2d_clear_runtime_boundary(SWMM_Engine, int cell, int edge);
/** Last accepted outward m3/s on a live groundwater boundary. Returns zero
 * for native no-flow edges or a cleared boundary; cumulative receipts remain. */
SWMM_ENGINE_API int swmm_gw2d_get_runtime_boundary_flow(SWMM_Engine, int cell, int edge,
    double* flow_m3_s);
/** Surface direct boundary with the same kind/value convention as above.
 * Flow is total outward m3/s (unlike legacy per-width edge setters). */
SWMM_ENGINE_API int swmm_2d_set_runtime_boundary(SWMM_Engine, int cell, int edge,
    int kind, double value, const double* concentrations, int species_count);
/** Atomic batch replacement of listed (domain,id,cell) sources. No cell or
 * species topology changes. Legal after start between steps and in step
 * callbacks. Invalid batches leave all prescriptions and clocks unchanged. */
SWMM_ENGINE_API int swmm_coupling_set_sources(SWMM_Engine, const SWMM_CouplingSource*, int count);
SWMM_ENGINE_API int swmm_coupling_clear_source(SWMM_Engine, int domain, const char* id);
SWMM_ENGINE_API int swmm_coupling_get_receipt(SWMM_Engine, int domain, const char* id, int cell,
    SWMM_CouplingReceipt* out, double* requested_species, double* applied_species, int capacity);
/** Cumulative gross external source volumes, excluding surface hydraulic
 * boundary exchange (which has its own existing boundary ledger). */
SWMM_ENGINE_API int swmm_coupling_get_water_totals(SWMM_Engine, int domain,
    double* incoming_m3, double* outgoing_m3);
/** Advance to an elapsed-second exchange time (no overshoot); flush pending
 * 2D work before returning. No rollback/iteration capability is promised. */
SWMM_ENGINE_API int swmm_coupling_advance_to(SWMM_Engine, double target_seconds, double* actual_seconds);
/** Bitmask: 1 surface sources, 2 saturated GW sources, 4 surface water heat,
 * 8 surface runtime boundaries, 16 saturated groundwater water heat,
 * 32 groundwater lateral hydraulic/advective boundaries, 64 atomic frames,
 * 128 runtime rain quality, 256 paired infiltration, 512 boundary receipts. A zero bit means unavailable in this run. FULL_SWE coupling is not supported. */
SWMM_ENGINE_API int swmm_coupling_capabilities(SWMM_Engine, unsigned* flags);
#ifdef __cplusplus
}
#endif
#endif
