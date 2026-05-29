/**
 * @file openswmm_inflows.h
 * @brief OpenSWMM Engine — Inflows (External, DWF, RDII) C API.
 *
 * @details External inflow addition, dry weather flow, RDII assignment,
 *          and count queries.
 *
 * @ingroup engine_api
 * @see openswmm_engine.h
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_INFLOWS_H
#define OPENSWMM_INFLOWS_H

#include "openswmm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * External inflows
 * ========================================================================= */

/**
 * @brief Add an external inflow to a node.
 *
 * @details External inflows define time-varying flows or pollutant loads
 *          applied at a node, optionally driven by a time series, with
 *          scaling, baseline, and pattern modifiers.
 *
 * @param engine     Engine handle.
 * @param node_idx   Zero-based index of the receiving node.
 * @param constituent Constituent name ("FLOW" for flow, or a pollutant name).
 * @param ts_name    Time series name (NULL or "" for constant baseline only).
 * @param type       Inflow type: "FLOW", "CONCEN", or "MASS".
 * @param m_factor   Multiplier applied to the time series values.
 * @param s_factor   Scale factor (unit conversion).
 * @param baseline   Constant baseline value added to the time series.
 * @param pattern    Time pattern name (NULL or "" for none).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_ext_inflow_add(SWMM_Engine engine, int node_idx, const char* constituent,
                                          const char* ts_name, const char* type,
                                          double m_factor, double s_factor, double baseline,
                                          const char* pattern);

/**
 * @brief Read back an external inflow entry by index.
 *
 * @details The model stores external inflows as a flat SoA, indexed
 *          0..swmm_ext_inflow_count()-1. To list a single node's inflows,
 *          iterate all entries and filter by @p node_idx.
 *
 * @param engine              Engine handle.
 * @param entry_idx           Zero-based entry index.
 * @param node_idx            [out] Receiving node index.
 * @param constituent_buf     [out] Constituent name buffer (NUL-terminated).
 * @param constituent_buflen  Size of @p constituent_buf.
 * @param ts_buf              [out] Time series name buffer (NUL-terminated; empty if none).
 * @param ts_buflen           Size of @p ts_buf.
 * @param type_buf            [out] Inflow type buffer (NUL-terminated; "FLOW"/"CONCEN"/"MASS").
 * @param type_buflen         Size of @p type_buf.
 * @param m_factor            [out] Multiplier factor.
 * @param s_factor            [out] Scale factor.
 * @param baseline            [out] Baseline value.
 * @param pattern_buf         [out] Pattern name buffer (NUL-terminated; empty if none).
 * @param pattern_buflen      Size of @p pattern_buf.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_ext_inflow_get(SWMM_Engine engine, int entry_idx,
                                          int* node_idx,
                                          char* constituent_buf, int constituent_buflen,
                                          char* ts_buf,          int ts_buflen,
                                          char* type_buf,        int type_buflen,
                                          double* m_factor, double* s_factor, double* baseline,
                                          char* pattern_buf,     int pattern_buflen);

/**
 * @brief Remove an external inflow entry by index.
 *
 * @details Entries with index > @p entry_idx shift down by one. Callers
 *          holding cached indices must re-resolve via swmm_ext_inflow_count()
 *          / iteration.
 *
 * @param engine     Engine handle.
 * @param entry_idx  Zero-based entry index (0..swmm_ext_inflow_count()-1).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_ext_inflow_remove(SWMM_Engine engine, int entry_idx);

/* =========================================================================
 * Dry weather flow
 * ========================================================================= */

/**
 * @brief Add a dry weather flow component to a node.
 *
 * @details Dry weather flow represents the base sanitary flow entering the
 *          system at a node, modulated by up to four time patterns.
 *
 * @param engine      Engine handle.
 * @param node_idx    Zero-based index of the receiving node.
 * @param constituent Constituent name ("FLOW" or a pollutant name).
 * @param avg_value   Average DWF value.
 * @param pat1        Monthly time pattern name (NULL for none).
 * @param pat2        Daily time pattern name (NULL for none).
 * @param pat3        Hourly time pattern name (NULL for none).
 * @param pat4        Weekend time pattern name (NULL for none).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_dwf_add(SWMM_Engine engine, int node_idx, const char* constituent,
                                   double avg_value, const char* pat1, const char* pat2,
                                   const char* pat3, const char* pat4);

/**
 * @brief Read back a dry weather flow entry by index.
 *
 * @param engine              Engine handle.
 * @param entry_idx           Zero-based entry index (0..swmm_dwf_count()-1).
 * @param node_idx            [out] Receiving node index.
 * @param constituent_buf     [out] Constituent name (NUL-terminated).
 * @param constituent_buflen  Size of @p constituent_buf.
 * @param avg_value           [out] Average value.
 * @param pat1_buf            [out] Monthly pattern name (NUL-terminated; empty if none).
 * @param pat1_buflen         Size of @p pat1_buf.
 * @param pat2_buf            [out] Daily pattern name.
 * @param pat2_buflen         Size of @p pat2_buf.
 * @param pat3_buf            [out] Hourly pattern name.
 * @param pat3_buflen         Size of @p pat3_buf.
 * @param pat4_buf            [out] Weekend pattern name.
 * @param pat4_buflen         Size of @p pat4_buf.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_dwf_get(SWMM_Engine engine, int entry_idx,
                                   int* node_idx,
                                   char* constituent_buf, int constituent_buflen,
                                   double* avg_value,
                                   char* pat1_buf, int pat1_buflen,
                                   char* pat2_buf, int pat2_buflen,
                                   char* pat3_buf, int pat3_buflen,
                                   char* pat4_buf, int pat4_buflen);

/**
 * @brief Remove a DWF entry by index. Subsequent entries shift down.
 * @param engine     Engine handle.
 * @param entry_idx  Zero-based entry index (0..swmm_dwf_count()-1).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_dwf_remove(SWMM_Engine engine, int entry_idx);

/* =========================================================================
 * RDII (Rainfall-Dependent Infiltration/Inflow)
 * ========================================================================= */

/**
 * @brief Add RDII inflow to a node using a unit hydrograph.
 *
 * @details Associates a node with a unit hydrograph group and its sewershed
 *          area to compute rainfall-dependent infiltration/inflow.
 *
 * @param engine    Engine handle.
 * @param node_idx  Zero-based index of the receiving node.
 * @param uh_name   Unit hydrograph group name.
 * @param area      Sewershed area in project area units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_rdii_add(SWMM_Engine engine, int node_idx, const char* uh_name, double area);

/**
 * @brief Read back an RDII assignment by entry index.
 *
 * @param engine       Engine handle.
 * @param entry_idx    Zero-based index into the RDII assignment list (0..swmm_rdii_count()-1).
 * @param node_idx     [out] Receiving node index.
 * @param uh_buf       [out] Buffer to receive the unit hydrograph name (NUL-terminated, truncated if too small).
 * @param buflen       Size of @p uh_buf in bytes.
 * @param area         [out] Sewershed area in project area units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_rdii_get(SWMM_Engine engine, int entry_idx,
                                    int* node_idx, char* uh_buf, int buflen,
                                    double* area);

/**
 * @brief Remove an RDII entry by index. Subsequent entries shift down.
 * @param engine     Engine handle.
 * @param entry_idx  Zero-based entry index (0..swmm_rdii_count()-1).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_rdii_remove(SWMM_Engine engine, int entry_idx);

/* =========================================================================
 * Unit hydrographs ([HYDROGRAPHS] section)
 * =========================================================================
 *
 * A unit hydrograph group is identified by name and has two kinds of input
 * lines:
 *   - A gage assignment line: "UHname  RainGage"
 *   - One or more parameter lines: "UHname Month Response R T K [Dmax Drecov Dinit]"
 *
 * Both kinds are added separately. `swmm_hydrograph_add` adds a parameter
 * line, `swmm_hydrograph_add_gage` adds the gage assignment. Counts and
 * getters are also separated.
 *
 * `response` is encoded as: 0 = SHORT, 1 = MEDIUM, 2 = LONG.
 * `month` is encoded as: 0..11 = JAN..DEC, or -1 = ALL.
 * ========================================================================= */

/**
 * @brief Add a unit hydrograph parameter line.
 *
 * @param engine    Engine handle.
 * @param uh_name   Unit hydrograph group name.
 * @param month     0..11 = JAN..DEC, or -1 for ALL months.
 * @param response  0 = SHORT, 1 = MEDIUM, 2 = LONG.
 * @param r         Fraction of rainfall volume that becomes RDII.
 * @param t         Time to peak (hours).
 * @param k         Ratio of recession-limb time to time-to-peak (>= 0).
 *                  Base time = t * (1 + k); the falling limb spans k * t hours.
 * @param dmax      Maximum initial-abstraction depth (project depth units; 0 if unused).
 * @param drecov    Linear-model IA recovery rate (project depth/day; 0 if unused or if [RDII_DECAY] is configured).
 * @param dinit     Initial IA already used at start of simulation (project depth units; 0 if unused).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_hydrograph_add(SWMM_Engine engine, const char* uh_name,
                                          int month, int response,
                                          double r, double t, double k,
                                          double dmax, double drecov, double dinit);

/**
 * @brief Read back a hydrograph parameter entry by index.
 *
 * @param engine     Engine handle.
 * @param entry_idx  Zero-based index (0..swmm_hydrograph_count()-1).
 * @param uh_buf     [out] UH group name buffer (NUL-terminated).
 * @param buflen     Size of @p uh_buf.
 * @param month      [out] Month code (-1, or 0..11).
 * @param response   [out] Response code (0..2).
 * @param r          [out] Rainfall fraction.
 * @param t          [out] Time to peak (hours).
 * @param k          [out] Recession ratio.
 * @param dmax       [out] IA max depth.
 * @param drecov     [out] Linear IA recovery rate.
 * @param dinit      [out] Initial IA used.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_hydrograph_get(SWMM_Engine engine, int entry_idx,
                                          char* uh_buf, int buflen,
                                          int* month, int* response,
                                          double* r, double* t, double* k,
                                          double* dmax, double* drecov, double* dinit);

/**
 * @brief Count parameter entries in the model.
 * @returns Number of parameter lines, or -1 on error.
 */
SWMM_ENGINE_API int swmm_hydrograph_count(SWMM_Engine engine);

/**
 * @brief Assign a rain gage to a unit hydrograph group.
 *
 * @param engine     Engine handle.
 * @param uh_name    Unit hydrograph group name.
 * @param gage_name  Rain gage name.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_hydrograph_add_gage(SWMM_Engine engine,
                                               const char* uh_name,
                                               const char* gage_name);

/**
 * @brief Read back a UH-to-gage assignment by index.
 *
 * @param engine      Engine handle.
 * @param entry_idx   Zero-based index (0..swmm_hydrograph_gage_count()-1).
 * @param uh_buf      [out] UH group name (NUL-terminated).
 * @param uh_buflen   Size of @p uh_buf.
 * @param gage_buf    [out] Rain gage name (NUL-terminated).
 * @param gage_buflen Size of @p gage_buf.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_hydrograph_get_gage(SWMM_Engine engine, int entry_idx,
                                               char* uh_buf, int uh_buflen,
                                               char* gage_buf, int gage_buflen);

/**
 * @brief Count UH-to-gage assignments.
 * @returns Number of assignments, or -1 on error.
 */
SWMM_ENGINE_API int swmm_hydrograph_gage_count(SWMM_Engine engine);

/**
 * @brief Count the unique unit-hydrograph group names defined.
 *
 * @details A unit-hydrograph "group" is identified by name; the engine stores
 *          one entry per `(group, month, response)`. This count is the number
 *          of *distinct* group names across all parameter entries, useful for
 *          enumerating groups in a UI without manually de-duplicating the
 *          per-entry list.
 *
 * @param engine  Engine handle.
 * @returns Number of unique groups, or -1 on error.
 */
SWMM_ENGINE_API int swmm_hydrograph_group_count(SWMM_Engine engine);

/**
 * @brief Read back the name of a unit-hydrograph group by its zero-based index.
 *
 * @details Groups are enumerated in first-occurrence order across the parameter
 *          entry list (matches the order in which they appear in the
 *          `[HYDROGRAPHS]` section of the input file).
 *
 * @param engine  Engine handle.
 * @param idx     Zero-based group index (0..swmm_hydrograph_group_count()-1).
 * @param buf     [out] Caller-allocated buffer to receive the group name.
 * @param buflen  Size of @p buf in bytes.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_hydrograph_group_id(SWMM_Engine engine, int idx,
                                               char* buf, int buflen);

/* =========================================================================
 * Mutation surface (BS-02) — upsert + key-based remove + rename
 * =========================================================================
 *
 * The legacy `swmm_hydrograph_add` / `swmm_hydrograph_add_gage` surface above
 * is append-only, which prevents in-place edits from a UI. The following
 * setters/removers support the `HydrographGroupEditor` MVC layer in
 * openswmm.gui: every mutation in the editor routes through one of these
 * symbols, and the GUI's `SWMMModelLayer` emits a `hydrographChanged(uhName)`
 * signal so all subscribed views (Object Browser, property panel, picker
 * combos, etc.) refresh in lock-step.
 *
 * Upsert contract (set_rtk, set_ia, decay_set):
 *   - If an entry matching the supplied key exists, update only the fields
 *     this setter owns and leave the rest untouched.
 *   - Otherwise append a new entry with the supplied fields set and the
 *     unspecified fields zeroed (so set_rtk followed by set_ia on the same
 *     key composes correctly).
 *
 * Remove contract (remove_entry, remove_group, decay_remove):
 *   - Key-based, not index-based — indices shift as entries are removed and
 *     a UI cannot keep them in sync. Idempotent: returns SWMM_OK if no
 *     match is found.
 *
 * Group remove cascades to: [HYDROGRAPHS] parameter rows, gage assignments,
 * [RDII_DECAY] rows, AND any [RDII] node assignments referencing the group
 * (so the engine never sees a dangling UH-name reference after a delete).
 * ========================================================================= */

/**
 * @brief Upsert R/T/K parameters for one (group, month, response) row.
 *
 * @param engine    Engine handle.
 * @param uh_name   Unit hydrograph group name (non-null, non-empty).
 * @param month     0..11 = JAN..DEC, or -1 for ALL.
 * @param response  0 = SHORT, 1 = MEDIUM, 2 = LONG.
 * @param r         Rainfall fraction.
 * @param t         Time to peak (hours).
 * @param k         Recession-limb-to-peak-time ratio (>= 0). Base time = t * (1 + k).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_hydrograph_set_rtk(SWMM_Engine engine, const char* uh_name,
                                              int month, int response,
                                              double r, double t, double k);

/**
 * @brief Upsert linear-IA parameters for one (group, month, response) row.
 *
 * @param engine    Engine handle.
 * @param uh_name   Unit hydrograph group name (non-null, non-empty).
 * @param month     0..11 = JAN..DEC, or -1 for ALL.
 * @param response  0 = SHORT, 1 = MEDIUM, 2 = LONG.
 * @param dmax      Maximum initial-abstraction depth.
 * @param drecov    Linear IA recovery rate.
 * @param dinit     Initial IA already used.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_hydrograph_set_ia(SWMM_Engine engine, const char* uh_name,
                                             int month, int response,
                                             double dmax, double drecov, double dinit);

/**
 * @brief Remove one parameter entry by (group, month, response).
 *
 * @details Idempotent — returns SWMM_OK whether or not a matching row exists.
 *          Indices shift, so any UI must look up entries by key rather than
 *          caching indices across calls.
 *
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_hydrograph_remove_entry(SWMM_Engine engine, const char* uh_name,
                                                   int month, int response);

/**
 * @brief Remove an entire UH group: parameter rows + gage assignment +
 *        [RDII_DECAY] rows + [RDII] node assignments referencing the group.
 *
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_hydrograph_remove_group(SWMM_Engine engine, const char* uh_name);

/**
 * @brief Bulk-clear every per-month parameter row for a group, leaving any
 *        existing month=-1 (ALL) row intact.
 *
 * @details Used by the editor when the user switches from per-season to ALL.
 *          Single pass — avoids the O(n^2) cost of repeated
 *          `swmm_hydrograph_remove_entry` calls.
 *
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_hydrograph_clear_group_months(SWMM_Engine engine, const char* uh_name);

/**
 * @brief Set, replace, or clear the rain gage assigned to a UH group.
 *
 * @param engine     Engine handle.
 * @param uh_name    Unit hydrograph group name (non-null, non-empty).
 * @param gage_name  Rain gage name, or NULL/empty to clear an existing
 *                   assignment.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_hydrograph_set_gage(SWMM_Engine engine, const char* uh_name,
                                               const char* gage_name);

/**
 * @brief Rename a UH group, propagating the new name to parameter rows,
 *        gage assignments, [RDII_DECAY] rows, and [RDII] node assignments.
 *
 * @param engine  Engine handle.
 * @param idx     Zero-based group index (0..swmm_hydrograph_group_count()-1).
 * @param new_id  New group name (non-null, non-empty, must not already exist).
 * @returns SWMM_OK on success, SWMM_ERR_BADPARAM if new_id is invalid or
 *          duplicates an existing group, or another error code.
 */
SWMM_ENGINE_API int swmm_hydrograph_group_rename(SWMM_Engine engine, int idx, const char* new_id);

/**
 * @brief Upsert exponential-decay parameters for one (group, response) row.
 *
 * @details Same upsert contract as `swmm_hydrograph_set_rtk`. Use
 *          `swmm_rdii_decay_remove` to delete a row (existence of a row
 *          IS the "active" flag in the engine model).
 *
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_rdii_decay_set(SWMM_Engine engine, const char* uh_name,
                                          int response,
                                          double k_dep, double k_0, double k_T,
                                          double T_ref, double theta_rec, double T_freeze);

/**
 * @brief Remove the exponential-decay row for one (group, response) pair.
 *
 * @details Idempotent — returns SWMM_OK whether or not a matching row exists.
 *
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_rdii_decay_remove(SWMM_Engine engine, const char* uh_name, int response);

/* =========================================================================
 * Exponential IA decay ([RDII_DECAY] section)
 * =========================================================================
 *
 * Physics-based replacement for the legacy linear IA recovery. When a row
 * is present for a (UH group, response) pair, the exponential depletion /
 * temperature-dependent recovery model is used in place of the linear
 * `drecov` rate from the corresponding [HYDROGRAPHS] entry.
 *
 * @see docs/RDII_ExpDecay_Implementation.md
 * ========================================================================= */

/**
 * @brief Add an exponential-decay parameter row for a (UH, response) pair.
 *
 * @param engine     Engine handle.
 * @param uh_name    Unit hydrograph group name (must match a [HYDROGRAPHS] entry).
 * @param response   0 = SHORT, 1 = MEDIUM, 2 = LONG.
 * @param k_dep      Depletion rate (1/project-depth-unit) — temperature-independent.
 * @param k_0        Base recovery rate (1/hr) — gravity drainage / capillary.
 * @param k_T        Thermal recovery rate at T_ref (1/hr) — ET-driven drying.
 * @param T_ref      Reference temperature (deg C) for the thermal term.
 * @param theta_rec  Temperature sensitivity (1/deg C) of the thermal term.
 * @param T_freeze   Recovery is suppressed when air temperature <= T_freeze (deg C).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_rdii_decay_add(SWMM_Engine engine, const char* uh_name,
                                          int response,
                                          double k_dep, double k_0, double k_T,
                                          double T_ref, double theta_rec, double T_freeze);

/**
 * @brief Read back an exponential-decay parameter row by index.
 *
 * @param engine     Engine handle.
 * @param entry_idx  Zero-based index (0..swmm_rdii_decay_count()-1).
 * @param uh_buf     [out] UH group name (NUL-terminated).
 * @param buflen     Size of @p uh_buf.
 * @param response   [out] Response code (0..2).
 * @param k_dep      [out] Depletion rate.
 * @param k_0        [out] Base recovery rate.
 * @param k_T        [out] Thermal recovery rate.
 * @param T_ref      [out] Reference temperature.
 * @param theta_rec  [out] Temperature sensitivity.
 * @param T_freeze   [out] Frozen-ground threshold.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_rdii_decay_get(SWMM_Engine engine, int entry_idx,
                                          char* uh_buf, int buflen,
                                          int* response,
                                          double* k_dep, double* k_0, double* k_T,
                                          double* T_ref, double* theta_rec, double* T_freeze);

/**
 * @brief Count exponential-decay parameter rows.
 * @returns Number of rows, or -1 on error.
 */
SWMM_ENGINE_API int swmm_rdii_decay_count(SWMM_Engine engine);

/* =========================================================================
 * Count queries
 * ========================================================================= */

/**
 * @brief Get the total number of external inflows defined.
 * @param engine  Engine handle.
 * @returns Number of external inflows, or -1 on error.
 */
SWMM_ENGINE_API int swmm_ext_inflow_count(SWMM_Engine engine);

/**
 * @brief Get the total number of dry weather flow entries defined.
 * @param engine  Engine handle.
 * @returns Number of DWF entries, or -1 on error.
 */
SWMM_ENGINE_API int swmm_dwf_count(SWMM_Engine engine);

/**
 * @brief Get the total number of RDII entries defined.
 * @param engine  Engine handle.
 * @returns Number of RDII entries, or -1 on error.
 */
SWMM_ENGINE_API int swmm_rdii_count(SWMM_Engine engine);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_INFLOWS_H */
