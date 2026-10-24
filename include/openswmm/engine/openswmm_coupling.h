/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file      openswmm_coupling.h
 * @brief     Public C API for runtime coupling prescriptions and exchanges.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

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
 * ownership. Named providers namespace source IDs as `provider/<provider>/<id>`
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
/**
 * @brief Atomically apply copied cross-domain sources, boundaries, forcings and clears.
 *
 * provider NULL/empty uses legacy ownership. A named provider cannot contain a slash; source IDs
 * become `provider/<provider>/<id>`. Boundary/forcing ownership is exclusive until clear/expiry.
 * Duplicate or conflicting entries are refused before publication. Accepted changes flush prior
 * pending physics, preserve cumulative receipts and record the prescription; native authored
 * settings resume after clear.
 *
 * @param[in] engine Non-null RUNNING engine with a supported CPU mesh backend. Call between steps
 *     or from a permitted step callback.
 * @param[in] frame Non-null frame with struct_size=sizeof(SWMM_CouplingFrame),
 *     version=SWMM_COUPLING_FRAME_VERSION and nonnegative counts. Each nonempty array requires a
 *     valid pointer; nested pointers are copied before return.
 * @return SWMM_OK on acceptance; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE for an
 *     unavailable state/domain, SWMM_ERR_PLUGIN for an unsupported runtime profile,
 *     SWMM_ERR_BADPARAM for invalid tuples/ownership conflicts, SWMM_ERR_BADINDEX for invalid
 *     cells/edges or a missing clear target, or SWMM_ERR_NOMEM/SWMM_ERR_INTERNAL for exceptions.
 *     Refusal preserves prescriptions and clocks.
 */
SWMM_ENGINE_API int swmm_coupling_apply_frame(SWMM_Engine engine, const SWMM_CouplingFrame* frame);
/**
 * @brief Disable a legacy source in one cell or all of its cells while retaining receipts.
 *
 * An absent source returns SWMM_ERR_BADINDEX. Clearing a groundwater source that overrides an
 * authored well resumes that well; other source IDs continue to add.
 *
 * @param[in] engine Non-null RUNNING engine with a supported CPU mesh backend. Call between steps
 *     or from a permitted step callback.
 * @param[in] domain SWMM_COUPLING_SURFACE (0) or SWMM_COUPLING_GROUNDWATER (1).
 * @param[in] id Non-null NUL-terminated source ID. Set/clear APIs refuse empty IDs and the reserved
 *     boundary/ and provider/ prefixes. Receipt reads use the full stored ID, including
 *     `provider/<provider>/` for named providers.
 * @param[in] cell Zero-based target cell, or -1 to clear every cell for this ID.
 * @return SWMM_OK on acceptance; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE for an
 *     unavailable state/domain, SWMM_ERR_PLUGIN for an unsupported runtime profile,
 *     SWMM_ERR_BADPARAM for invalid tuples/ownership conflicts, SWMM_ERR_BADINDEX for invalid
 *     cells/edges or a missing clear target, or SWMM_ERR_NOMEM/SWMM_ERR_INTERNAL for exceptions.
 *     Refusal preserves prescriptions and clocks.
 */
SWMM_ENGINE_API int swmm_coupling_clear_source_cell(SWMM_Engine engine, int domain, const char* id,
                                                    int cell);
/**
 * @brief Read several source receipts after validating the entire request.
 *
 * No outputs are written if any requested record is missing or any argument fails validation.
 * Records remain readable after source clear/expiry while the domain is active and runtime access
 * is permitted.
 *
 * @param[in] engine Non-null live engine with the selected mesh domain active; exclusive access is
 *     required.
 * @param[in] domain SWMM_COUPLING_SURFACE (0) or SWMM_COUPLING_GROUNDWATER (1).
 * @param[in] id Non-null NUL-terminated source ID. Set/clear APIs refuse empty IDs and the reserved
 *     boundary/ and provider/ prefixes. Receipt reads use the full stored ID, including
 *     `provider/<provider>/` for named providers.
 * @param[in] cells Array of count target cells; may be NULL when count=0.
 * @param[in] count Nonnegative number of receipt records to retrieve.
 * @param[out] out Caller-owned array of count receipts; may be NULL when count=0.
 * @param[out] requested_species Row-major [count][species_count] requested amounts in native
 *     concentration-unit*m3; may be NULL when either dimension is zero.
 * @param[out] applied_species Row-major [count][species_count] applied amounts in the same
 *     order/units; may be NULL when either dimension is zero.
 * @param[in] species_count Exact number of transport rows in the selected domain, including
 *     reserved temperature and age rows.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE when runtime
 *     reads are unavailable, SWMM_ERR_BADPARAM for invalid domain/output/species count, or
 *     SWMM_ERR_BADINDEX for a missing record.
 */
SWMM_ENGINE_API int swmm_coupling_get_receipts(SWMM_Engine engine, int domain, const char* id,
                                               const int* cells, int count,
                                               SWMM_CouplingReceipt* out, double* requested_species,
                                               double* applied_species, int species_count);
/**
 * @brief Read signed cumulative hydraulic and advective exchange for a perimeter edge.
 *
 * Positive water is INTO the domain, heat is total advective J, and species amounts use native
 * concentration-unit*m3. A valid native no-flow edge with no receipt returns zero-filled outputs.
 * Clearing a runtime boundary preserves its cumulative history.
 *
 * @param[in] engine Non-null live engine with the selected mesh domain active; exclusive access is
 *     required.
 * @param[in] domain SWMM_COUPLING_SURFACE (0) or SWMM_COUPLING_GROUNDWATER (1).
 * @param[in] cell Zero-based mesh cell index; groundwater must cover this cell with positive
 *     saturated thickness.
 * @param[in] edge Zero-based local cell edge. Must be a perimeter edge with positive length and
 *     center distance.
 * @param[out] out Non-null destination for signed cumulative water (m3), heat (J), and last
 *     accepted flow/interval (m3/s and s). Rejected equals requested minus applied.
 * @param[out] requested_species Caller-owned array of species_count cumulative requested amounts,
 *     in native concentration-unit*m3; may be NULL only when species_count=0.
 * @param[out] applied_species Caller-owned array of species_count cumulative applied amounts in the
 *     same order/units; may be NULL only when species_count=0.
 * @param[in] species_count Exact number of transport rows in the selected domain, including
 *     reserved temperature and age rows.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE when runtime
 *     reads are unavailable, SWMM_ERR_BADPARAM for invalid domain/output/species count, or
 *     SWMM_ERR_BADINDEX for an invalid cell or local edge.
 */
SWMM_ENGINE_API int swmm_coupling_get_boundary_receipt(SWMM_Engine engine, int domain, int cell,
                                                       int edge, SWMM_CouplingReceipt* out,
                                                       double* requested_species,
                                                       double* applied_species, int species_count);
/**
 * @brief Read the paired surface-to-groundwater infiltration receipt for one cell.
 *
 * Positive exchange is surface-to-groundwater; heat is advective J. The groundwater domain must
 * cover the cell. A cell without an infiltration forcing record returns SWMM_ERR_BADINDEX.
 *
 * @param[in] engine Non-null live engine with the selected mesh domain active; exclusive access is
 *     required.
 * @param[in] cell Zero-based mesh cell index; groundwater must cover this cell with positive
 *     saturated thickness.
 * @param[out] out Non-null destination for signed cumulative water (m3), heat (J), and last
 *     accepted flow/interval (m3/s and s). Rejected equals requested minus applied.
 * @param[out] requested_species Caller-owned array of species_count cumulative requested amounts,
 *     in native concentration-unit*m3; may be NULL only when species_count=0.
 * @param[out] applied_species Caller-owned array of species_count cumulative applied amounts in the
 *     same order/units; may be NULL only when species_count=0.
 * @param[in] species_count Exact surface-domain transport row count; output species follow the
 *     surface row order.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE when runtime
 *     reads are unavailable, SWMM_ERR_BADPARAM for invalid domain/output/species count, or
 *     SWMM_ERR_BADINDEX for a missing record.
 */
SWMM_ENGINE_API int swmm_coupling_get_infiltration_receipt(SWMM_Engine engine, int cell,
                                                           SWMM_CouplingReceipt* out,
                                                           double* requested_species,
                                                           double* applied_species,
                                                           int species_count);
/**
 * @brief Read a surface species storage or cumulative balance term.
 *
 * Storage sums the current per-cell masses; other selectors return accumulated ledger entries.
 * Terms 10 and 11 return zero when their corresponding calculation is inactive.
 *
 * @param[in] engine Non-null live engine with the selected mesh domain active; exclusive access is
 *     required.
 * @param[in] species Zero-based surface transport row index.
 * @param[in] term Ledger selector: 0 storage, 1 external in, 2 external out, 3 boundary in, 4
 *     boundary out, 5 rainfall in, 6 infiltration out, 7 native coupling in, 8 native coupling out,
 *     9 exfiltration in, 10 generated age, 11 signed reaction loss.
 * @param[out] value Non-null destination in native concentration-unit*m3. Generated age is RK2
 *     age-volume (otherwise zero); qualified coupled RK2 reaction production is negative loss.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE for
 *     unavailable surface runtime access, or SWMM_ERR_BADPARAM for null value/invalid species/term.
 */
SWMM_ENGINE_API int swmm_2d_get_species_ledger(SWMM_Engine engine, int species, int term,
                                               double* value);

/**
 * @brief Replace one copied legacy source prescription without altering species topology.
 *
 * Equivalent to swmm_coupling_set_sources() with one row. Replacing the same ID/cell retains its
 * cumulative receipts. Groundwater addresses the saturated zone; matching an authored well ID
 * overrides that well in this cell, while distinct IDs add.
 *
 * @param[in] engine Non-null RUNNING engine with a supported CPU mesh backend. Call between steps
 *     or from a permitted step callback.
 * @param[in] id Non-null NUL-terminated source ID. Set/clear APIs refuse empty IDs and the reserved
 *     boundary/ and provider/ prefixes. Receipt reads use the full stored ID, including
 *     `provider/<provider>/` for named providers.
 * @param[in] domain SWMM_COUPLING_SURFACE (0) or SWMM_COUPLING_GROUNDWATER (1).
 * @param[in] cell Zero-based mesh cell index; groundwater must cover this cell with positive
 *     saturated thickness.
 * @param[in] flow_m3_s Finite signed water rate in m3/s, positive INTO the domain. Negative flow
 *     extracts donor water quality.
 * @param[in] heat_w Finite signed water sensible-heat rate in W; nonzero requires a temperature
 *     row. Does not prescribe soil conduction or storage.
 * @param[in] concentrations Copied array of species_count concentrations in domain row order;
 *     required when the count is nonzero. Values must be finite and nonnegative except temperature,
 *     which may be negative. Age is seconds; temperature is degrees C; other rows match the domain
 *     concentration getter.
 * @param[in] rates Copied array of species_count finite signed concentration-unit*m3/s rates.
 *     Negative removes available mass. Reserved temperature/age rates must be zero; required when
 *     count is nonzero.
 * @param[in] species_count Exact number of transport rows in the selected domain, including
 *     reserved temperature and age rows.
 * @param[in] until_seconds Finite absolute elapsed simulation expiry time in seconds, strictly
 *     later than the current coupling time; zero holds until clear.
 * @return SWMM_OK on acceptance; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE for an
 *     unavailable state/domain, SWMM_ERR_PLUGIN for an unsupported runtime profile,
 *     SWMM_ERR_BADPARAM for invalid tuples/ownership conflicts, SWMM_ERR_BADINDEX for invalid
 *     cells/edges or a missing clear target, or SWMM_ERR_NOMEM/SWMM_ERR_INTERNAL for exceptions.
 *     Refusal preserves prescriptions and clocks.
 */
SWMM_ENGINE_API int swmm_coupling_set_source(SWMM_Engine engine, const char* id, int domain,
                                             int cell, double flow_m3_s, double heat_w,
                                             const double* concentrations, const double* rates,
                                             int species_count, double until_seconds);
/**
 * @brief Install or clear a saturated groundwater perimeter head or outward flow.
 *
 * A head prescription uses a one-sided unconfined Darcy face. Incoming water carries the supplied
 * quality; outgoing water uses donor quality. No solute diffusion or soil thermal boundary is
 * implied. Authored aquifer boundary entries on the same edge are refused. Receipt ID is
 * `boundary/<cell>/<edge>`; a different provider owning this edge prevents replacement.
 *
 * @param[in] engine Non-null RUNNING engine with a supported CPU mesh backend. Call between steps
 *     or from a permitted step callback.
 * @param[in] cell Zero-based mesh cell index; groundwater must cover this cell with positive
 *     saturated thickness.
 * @param[in] edge Zero-based local cell edge. Must be a perimeter edge with positive length and
 *     center distance.
 * @param[in] kind 0 restores native no-flow, 1 prescribes datum head, 2 prescribes outward flow.
 * @param[in] value Finite datum head in m for kind=1 or total outward m3/s for kind=2; ignored for
 *     kind=0.
 * @param[in] concentrations Copied array of species_count concentrations in domain row order;
 *     required when the count is nonzero. Values must be finite and nonnegative except temperature,
 *     which may be negative. Age is seconds; temperature is degrees C; other rows match the domain
 *     concentration getter.
 * @param[in] species_count Exact groundwater transport row count for kind 1/2; must be zero for
 *     kind=0 (concentrations may then be NULL).
 * @return SWMM_OK on acceptance; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE for an
 *     unavailable state/domain, SWMM_ERR_PLUGIN for an unsupported runtime profile,
 *     SWMM_ERR_BADPARAM for invalid tuples/ownership conflicts, SWMM_ERR_BADINDEX for invalid
 *     cells/edges or a missing clear target, or SWMM_ERR_NOMEM/SWMM_ERR_INTERNAL for exceptions.
 *     Refusal preserves prescriptions and clocks.
 */
SWMM_ENGINE_API int swmm_gw2d_set_runtime_boundary(SWMM_Engine engine, int cell, int edge, int kind,
                                                   double value, const double* concentrations,
                                                   int species_count);
/**
 * @brief Restore native no-flow on a legacy-owned groundwater perimeter edge.
 *
 * Equivalent to kind=0 in swmm_gw2d_set_runtime_boundary(). Preserves accumulated receipts;
 * clearing an already native edge succeeds unless an authored/other-provider boundary prevents the
 * operation.
 *
 * @param[in] engine Non-null RUNNING engine with a supported CPU mesh backend. Call between steps
 *     or from a permitted step callback.
 * @param[in] cell Zero-based mesh cell index; groundwater must cover this cell with positive
 *     saturated thickness.
 * @param[in] edge Zero-based local cell edge. Must be a perimeter edge with positive length and
 *     center distance.
 * @return SWMM_OK on acceptance; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE for an
 *     unavailable state/domain, SWMM_ERR_PLUGIN for an unsupported runtime profile,
 *     SWMM_ERR_BADPARAM for invalid tuples/ownership conflicts, SWMM_ERR_BADINDEX for invalid
 *     cells/edges or a missing clear target, or SWMM_ERR_NOMEM/SWMM_ERR_INTERNAL for exceptions.
 *     Refusal preserves prescriptions and clocks.
 */
SWMM_ENGINE_API int swmm_gw2d_clear_runtime_boundary(SWMM_Engine engine, int cell, int edge);
/**
 * @brief Read the last accepted outward groundwater boundary discharge.
 *
 * Cumulative history is retained separately by swmm_coupling_get_boundary_receipt(). The
 * groundwater domain must be active and cover the cell.
 *
 * @param[in] engine Non-null live engine with the selected mesh domain active; exclusive access is
 *     required.
 * @param[in] cell Zero-based mesh cell index; groundwater must cover this cell with positive
 *     saturated thickness.
 * @param[in] edge Zero-based local cell edge. Must be a perimeter edge with positive length and
 *     center distance.
 * @param[out] flow_m3_s Non-null destination in outward m3/s; zero for native no-flow or a cleared
 *     boundary.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE when runtime
 *     reads are unavailable, SWMM_ERR_BADPARAM for invalid domain/output/species count, or
 *     SWMM_ERR_BADINDEX for an invalid cell or local edge.
 */
SWMM_ENGINE_API int swmm_gw2d_get_runtime_boundary_flow(SWMM_Engine engine, int cell, int edge,
                                                        double* flow_m3_s);
/**
 * @brief Install or clear a surface perimeter stage or total outward flow.
 *
 * Uses legacy provider ownership. Active prescriptions disconnect authored time-series/rating
 * bindings; clearing restores them and preserves receipts. Incoming quality follows the supplied
 * tuple; profile-specific checkpoint/transport guards are checked before publication.
 *
 * @param[in] engine Non-null RUNNING engine with a supported CPU mesh backend. Call between steps
 *     or from a permitted step callback.
 * @param[in] cell Zero-based mesh cell index; groundwater must cover this cell with positive
 *     saturated thickness.
 * @param[in] edge Zero-based local cell edge. Must be a perimeter edge with positive length and
 *     center distance.
 * @param[in] kind 0 restores the authored/native boundary, 1 prescribes datum stage, 2 prescribes
 *     outward flow.
 * @param[in] value Finite stage in m for kind=1 or total outward m3/s for kind=2; ignored for
 *     kind=0. Flow is total discharge, unlike legacy per-width edge setters.
 * @param[in] concentrations Copied array of species_count concentrations in domain row order;
 *     required when the count is nonzero. Values must be finite and nonnegative except temperature,
 *     which may be negative. Age is seconds; temperature is degrees C; other rows match the domain
 *     concentration getter.
 * @param[in] species_count Exact surface transport row count for kind 1/2; zero for kind=0
 *     (concentrations may then be NULL).
 * @return SWMM_OK on acceptance; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE for an
 *     unavailable state/domain, SWMM_ERR_PLUGIN for an unsupported runtime profile,
 *     SWMM_ERR_BADPARAM for invalid tuples/ownership conflicts, SWMM_ERR_BADINDEX for invalid
 *     cells/edges or a missing clear target, or SWMM_ERR_NOMEM/SWMM_ERR_INTERNAL for exceptions.
 *     Refusal preserves prescriptions and clocks.
 */
SWMM_ENGINE_API int swmm_2d_set_runtime_boundary(SWMM_Engine engine, int cell, int edge, int kind,
                                                 double value, const double* concentrations,
                                                 int species_count);
/**
 * @brief Atomically replace a batch of legacy source ID/cell prescriptions.
 *
 * No cell/species topology changes are permitted. All rows are validated before pending old physics
 * is flushed and the batch is published. Unlisted sources remain active; receipts for replaced rows
 * are retained. Uses an unnamed-provider frame.
 *
 * @param[in] engine Non-null RUNNING engine with a supported CPU mesh backend. Call between steps
 *     or from a permitted step callback.
 * @param[in] sources Copied array of count rows with struct_size=sizeof(SWMM_CouplingSource); may
 *     be NULL only when count=0.
 * @param[in] count Nonnegative row count. Duplicate (domain,id,cell) entries are refused.
 * @return SWMM_OK on acceptance; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE for an
 *     unavailable state/domain, SWMM_ERR_PLUGIN for an unsupported runtime profile,
 *     SWMM_ERR_BADPARAM for invalid tuples/ownership conflicts, SWMM_ERR_BADINDEX for invalid
 *     cells/edges or a missing clear target, or SWMM_ERR_NOMEM/SWMM_ERR_INTERNAL for exceptions.
 *     Refusal preserves prescriptions and clocks.
 */
SWMM_ENGINE_API int swmm_coupling_set_sources(SWMM_Engine engine,
                                              const SWMM_CouplingSource* sources, int count);
/**
 * @brief Disable all legacy source cells for one ID and preserve their receipts.
 *
 * Equivalent to swmm_coupling_clear_source_cell() with cell=-1. An absent ID returns
 * SWMM_ERR_BADINDEX. Named-provider sources must be cleared through their provider frame.
 *
 * @param[in] engine Non-null RUNNING engine with a supported CPU mesh backend. Call between steps
 *     or from a permitted step callback.
 * @param[in] domain SWMM_COUPLING_SURFACE (0) or SWMM_COUPLING_GROUNDWATER (1).
 * @param[in] id Non-null NUL-terminated source ID. Set/clear APIs refuse empty IDs and the reserved
 *     boundary/ and provider/ prefixes. Receipt reads use the full stored ID, including
 *     `provider/<provider>/` for named providers.
 * @return SWMM_OK on acceptance; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE for an
 *     unavailable state/domain, SWMM_ERR_PLUGIN for an unsupported runtime profile,
 *     SWMM_ERR_BADPARAM for invalid tuples/ownership conflicts, SWMM_ERR_BADINDEX for invalid
 *     cells/edges or a missing clear target, or SWMM_ERR_NOMEM/SWMM_ERR_INTERNAL for exceptions.
 *     Refusal preserves prescriptions and clocks.
 */
SWMM_ENGINE_API int swmm_coupling_clear_source(SWMM_Engine engine, int domain, const char* id);
/**
 * @brief Read one stored source receipt, including retained cleared/expired history.
 *
 * Species rows follow the selected domain order. The record must already exist; validation errors
 * leave the outputs unchanged.
 *
 * @param[in] engine Non-null live engine with the selected mesh domain active; exclusive access is
 *     required.
 * @param[in] domain SWMM_COUPLING_SURFACE (0) or SWMM_COUPLING_GROUNDWATER (1).
 * @param[in] id Non-null NUL-terminated source ID. Set/clear APIs refuse empty IDs and the reserved
 *     boundary/ and provider/ prefixes. Receipt reads use the full stored ID, including
 *     `provider/<provider>/` for named providers.
 * @param[in] cell Zero-based mesh cell index; groundwater must cover this cell with positive
 *     saturated thickness.
 * @param[out] out Non-null destination for signed cumulative water (m3), heat (J), and last
 *     accepted flow/interval (m3/s and s). Rejected equals requested minus applied.
 * @param[out] requested_species Caller-owned array of species_count cumulative requested amounts,
 *     in native concentration-unit*m3; may be NULL only when species_count=0.
 * @param[out] applied_species Caller-owned array of species_count cumulative applied amounts in the
 *     same order/units; may be NULL only when species_count=0.
 * @param[in] capacity Must equal the selected domain species count exactly; it is not a minimum
 *     buffer capacity. Both species arrays require this many elements.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE when runtime
 *     reads are unavailable, SWMM_ERR_BADPARAM for invalid domain/output/species count, or
 *     SWMM_ERR_BADINDEX for a missing record.
 */
SWMM_ENGINE_API int swmm_coupling_get_receipt(SWMM_Engine engine, int domain, const char* id,
                                              int cell, SWMM_CouplingReceipt* out,
                                              double* requested_species, double* applied_species,
                                              int capacity);
/**
 * @brief Sum gross cumulative external source inflow and outflow for a domain.
 *
 * Sums stored source receipt rows, including their retained history. Surface hydraulic boundary
 * exchange has a separate boundary ledger and is excluded.
 *
 * @param[in] engine Non-null live engine with the selected mesh domain active; exclusive access is
 *     required.
 * @param[in] domain SWMM_COUPLING_SURFACE (0) or SWMM_COUPLING_GROUNDWATER (1).
 * @param[out] incoming_m3 Non-null destination for cumulative gross incoming water in m3.
 * @param[out] outgoing_m3 Non-null destination for cumulative gross outgoing water in m3.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE for
 *     unavailable domain/runtime access, or SWMM_ERR_BADPARAM for invalid domain/null outputs.
 */
SWMM_ENGINE_API int swmm_coupling_get_water_totals(SWMM_Engine engine, int domain,
                                                   double* incoming_m3, double* outgoing_m3);
/**
 * @brief Advance synchronously to an elapsed-second exchange time without overshoot.
 *
 * Pending 2D work is flushed before successful return. Due external driver updates must be supplied
 * first. The operation does not promise rollback or iterative coupling; inspect actual_seconds
 * after a stepping failure.
 *
 * @param[in] engine Non-null RUNNING engine; cannot be called from within an active step/callback.
 * @param[in] target_seconds Finite absolute elapsed simulation time in seconds between the current
 *     coupling time and simulation end, inclusive.
 * @param[out] actual_seconds Non-null destination for the accepted coupling clock in seconds. With
 *     valid arguments it is set before lifecycle/target checks, and updated after stepping; on
 *     failure it may show partial advancement.
 * @return SWMM_OK on successful advancement; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADPARAM
 *     for null output/nonfinite or out-of-range target, SWMM_ERR_LIFECYCLE for a
 *     running/driver/boundary refusal, or an error from stepping.
 */
SWMM_ENGINE_API int swmm_coupling_advance_to(SWMM_Engine engine, double target_seconds,
                                             double* actual_seconds);
/**
 * @brief Query runtime coupling features currently available in this engine.
 *
 * An absent bit means unavailable in this run. Capability bits do not bypass model-specific owner,
 * transport, FULL_SWE or checkpoint-profile validation performed before frame publication.
 *
 * @param[in] engine Non-null live engine; the returned mask depends on the active mesh backend and
 *     transport rows.
 * @param[out] flags Non-null destination for a bitmask: 1 surface sources, 2 saturated GW sources,
 *     4 surface water heat, 8 surface boundaries, 16 GW water heat, 32 GW hydraulic/advective
 *     boundaries, 64 atomic frames, 128 rain quality, 256 paired infiltration, 512 boundary
 *     receipts.
 * @return SWMM_OK on success, including a zero mask for an unavailable CPU runtime backend;
 *     SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for null flags.
 */
SWMM_ENGINE_API int swmm_coupling_capabilities(SWMM_Engine engine, unsigned* flags);
#ifdef __cplusplus
}
#endif
#endif
