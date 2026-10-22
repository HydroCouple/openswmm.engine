// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file      HotStartManager.hpp
 * @brief     Hot start file manager — in-memory representation + I/O.
 *
 * @details `HotStartManager` provides the C++ implementation of the hot start
 *          API declared in `include/openswmm/engine/openswmm_hotstart.h`.
 *
 *          ### Binary file format
 *
 *          **V1** (legacy — no infiltration/GW state):
 *          ```
 *          Offset  Size   Field
 *          ------  ----   -----
 *          0       16     Magic "OPENSWMM_HS_V1\0"
 *          16      4      Version: uint32_t = 1
 *          ...            (nodes, links as before)
 *                         Per-subcatch: [uint32 name_len, char name[],
 *                                        double runoff, double gwater]
 *          EOF-4   4      CRC32: uint32_t
 *          ```
 *
 *          **V2** (Gap #54 — includes infiltration model state and GW zone state):
 *          ```
 *          0       16     Magic "OPENSWMM_HS_V1\0"  (same magic, version byte changes)
 *          16      4      Version: uint32_t = 2
 *          20      8      Timestamp (Unix seconds): int64_t
 *          28      8      Simulation time at save (decimal days): double
 *          36      8      Start date (OADate): double
 *          44      8      End date (OADate): double
 *          52      4      CRS string length: uint32_t
 *          56      n      CRS string (null-terminated)
 *          56+n    4      Node count: uint32_t
 *          ...     ...    Per-node: [uint32 name_len, char name[], double depth,
 *                                    double head, double volume]
 *                         Link count: uint32_t
 *                         Per-link: [uint32 name_len, char name[], double flow,
 *                                    double depth, double volume]
 *                         Subcatch count: uint32_t
 *                         Per-subcatch V2: [uint32 name_len, char name[],
 *                                           double runoff, double gwater,
 *                                           uint32_t infil_model,
 *                                           double infil[6],
 *                                           double gw_theta,
 *                                           double gw_lower_depth]
 *          EOF-4   4      CRC32 of all preceding bytes: uint32_t
 *          ```
 *
 *          **V15** keeps the compatible record blocks and appends a complete
 *          accepted engine-session checkpoint. It is used for qualified
 *          thermal and 2D restarts so water, clocks, thermal stores, and
 *          accepted exchange receipts restore together. V15 files restore
 *          only into the matching initialized model through SWMMEngine.
 *
 *          ### Threading
 *
 *          `HotStartFile` instances are not thread-safe. The C API wraps each
 *          handle with a raw pointer cast; callers are responsible for
 *          not sharing handles across threads.
 *
 * @see include/openswmm/engine/openswmm_hotstart.h — C API
 * @see Legacy reference: src/solver/hotstart.c — swmm_saveHotstart()
 * @ingroup engine_hotstart
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_HOT_START_MANAGER_HPP
#define OPENSWMM_ENGINE_HOT_START_MANAGER_HPP

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace openswmm {

struct SimulationContext;
namespace runoff {
class RunoffSolver;
}
namespace groundwater {
class GWSolver;
}

// ============================================================================
// Per-object state records
// ============================================================================

/** @brief Node hydraulic state at hot-start save time. */
struct HotStartLidCellRecord {
    int layer = 0;
    double bottom = 0.0, top = 0.0, porosity = 0.0, volume = 0.0, theta = 0.0;
};
struct HotStartNodeRecord {
    std::string id;
    std::vector<double> lid_richards;     ///< V11: per-row storage and material identity
    std::vector<double> lid_infiltration; ///< V10 S,Ks,IMDmax,IMD,F,Fu,Fumax,Lu,T,head,saturated
    std::vector<double> lid_quality_mass; ///< V9 retained cell pollutant mass
    std::vector<HotStartLidCellRecord> lid_cells; ///< V8 retained moisture and geometry identity
    double lid_treated_volume = 0.0;              ///< V8 clogging history (ft3)
    double depth = 0.0;
    double head = 0.0;
    double volume = 0.0;
    /// A2a (V3): water age (seconds); -1 = not tracked / pre-V3 file.
    double age = -1.0;
};

/** @brief Link hydraulic state at hot-start save time. */
struct HotStartLinkRecord {
    std::string id;
    double flow = 0.0;
    double depth = 0.0;
    double volume = 0.0;
    /// A2a (V3): water age (seconds); -1 = not tracked / pre-V3 file.
    double age = -1.0;
};

/** @brief Subcatchment state at hot-start save time. */
struct HotStartSubcatchRecord {
    std::string id;
    double runoff = 0.0;
    double gwater = 0.0;

    // Gap #54: infiltration model state (V2 only; -1 = not present / V1 file)
    int infil_model = -1;
    double infil[6] = {0, 0, 0, 0, 0, 0}; ///< flat 6-elem state (model-dependent)

    // Gap #54: groundwater zone state (V2 only; gw_theta < 0 = not present)
    double gw_theta = -1.0;      ///< upper zone moisture content
    double gw_lower_depth = 0.0; ///< lower zone depth (ft)
};

// ============================================================================
// File header (in-memory mirror of the on-disk header)
// ============================================================================

/** @brief In-memory representation of the OPENSWMM_HS_V1 header. */
struct HotStartHeader {
    uint32_t version = 1;
    int64_t timestamp = 0;   ///< Unix epoch seconds at save time
    double sim_time = 0.0;   ///< Simulation elapsed time (decimal days)
    double start_date = 0.0; ///< options.start_date (OADate (days since 12/30/1899))
    double end_date = 0.0;   ///< options.end_date (OADate (days since 12/30/1899))
    std::string crs;         ///< CRS string (may be empty)
};

// ============================================================================
// HotStartFile — opaque handle (cast to void* in C API)
// ============================================================================

/**
 * @brief In-memory hot start file data.
 *
 * @details Created by `HotStartManager::save()` or `HotStartManager::open()`.
 *          The C API casts `HotStartFile*` to/from `void* SWMM_HotStart`.
 *
 * @ingroup engine_hotstart
 */
struct HotStartFile {
    std::string snow_state; ///< V14: native degree-day and UEB accepted/held states
    std::string
        engine_checkpoint; ///< V15: complete accepted engine/2D state for qualified native restarts (may be empty from V16)
    std::string gw_transport_state; ///< V17: canonical SIGMA chemistry, edge receipts and clocks
    std::string
        surface_state; ///< V16: subcatchment ponded water, groundwater flow and surface quality (StateAccessors::get_surface_state)
    HotStartHeader header;
    std::vector<HotStartNodeRecord> nodes;
    std::vector<HotStartLinkRecord> links;
    std::vector<HotStartSubcatchRecord> subcatches;

    /// U2 (2026-09-07, D-IQ5) — V4 species block: per-element concentrations
    /// for every [POLLUTANTS] entry and every reactions-component species,
    /// keyed by NAME so a file applies to a model whose species order
    /// differs. `node_species` / `link_species` are [element * n + s] with
    /// the same element order as `nodes` / `links`. Age keeps its per-record
    /// field (V3); temperature is not carried (its seed paths are per
    /// engine — a follow-up).
    std::vector<std::string> species;
    std::vector<double> node_species;
    std::vector<double> link_species;

    /// G1 (2026-09-07) — V5 block: the two-zone groundwater state.
    ///
    /// A run restarted without this reruns the entire wetting history from a
    /// dry table, which for an aquifer is not an approximation — the whole
    /// point of the kernel is that it has memory measured in seasons. Cells
    /// are carried by INDEX, not name, because a mesh has no cell ids; a file
    /// whose `gw_n_cells` does not match the model is reported and skipped
    /// rather than applied to the wrong cells.
    ///
    /// `gw_theta_sigma` is layer-major `[layer * n_cells + cell]`, the SoA's
    /// own order, and is empty when no cell uses closure B. The ledger terms
    /// ride along so a restarted run's continuity check continues the
    /// original's instead of starting from zero against a non-zero storage.
    uint32_t gw_n_cells = 0;
    uint32_t gw_m_layers = 0;
    std::vector<double> gw_hg;          ///< saturated thickness (m)
    std::vector<double> gw_hu;          ///< unsaturated storage (m of water)
    std::vector<double> gw_theta_sigma; ///< closure-B layers, layer-major
    std::vector<std::vector<double>>
        gw_interface; ///< V12: receiving interface; V13 appends pending/cumulative ET and held surface evaporation
    std::vector<double> gw_ledger; ///< the cumulative terms (m3): 9, +link since G-X3

    /// T7.5 (2026-09-21) — V6 block: the aquifer's TRANSPORTED tuple.
    ///
    /// Without it a restarted run keeps the water table's memory (V5) and
    /// throws away what is dissolved in it, which for a plume is the whole
    /// state. Species are carried by NAME, not by row index: a row layout
    /// depends on `[POLLUTANTS]`, the MSX species list and the AGE /
    /// TEMPERATURE switches, any of which can differ between the run that
    /// wrote the file and the run that reads it. A species the reader does
    /// not have is dropped with a warning; one it has and the file does not
    /// simply starts from its `[GW_INITIAL_QUALITY]` seed.
    ///
    /// Both stores are species-major `[s * n_cells + cell]`, the kernel's
    /// own order. `gw_species_ledger` is `[s * kGwSpeciesLedgerTerms + t]`.
    std::vector<std::string> gw_species;   ///< row names, in file order
    std::vector<double> gw_sat_mass;       ///< saturated-zone mass
    std::vector<double> gw_unsat_mass;     ///< column-store mass
    std::vector<double> gw_species_ledger; ///< per species, cumulative
    /// init, infil_in, node_in, link_in, lateral_net, deep, node_out,
    /// link_out, dunne, et, reaction, source_in, source_out — the `residual` is derived, never
    /// stored, so a file can never disagree with itself about it.
    static constexpr int kGwSpeciesLedgerTerms = 13;

    std::string path;   ///< File path (for flush-on-close)
    bool dirty = false; ///< True if set_*() was called

    std::vector<std::string> warnings; ///< Populated by HotStartManager::apply()

    // -----------------------------------------------------------------------
    // Modification helpers
    // -----------------------------------------------------------------------

    /**
     * @brief Edit the first exact-ID stored node depth and mark the file dirty.
     *
     * Changes only this scalar and dirty flag; does not recompute related depths/heads/volumes, update
     * opaque full engine checkpoints, apply to a live model or flush to disk.
     *
     * @param[in] id Borrowed byte-exact record ID, case-sensitive; no engine name-index lookup.
     * @param[in] v New node depth in ft above invert; no finite/range validation.
     * @return True on a matching record, false with no mutation when absent.
     */
    bool set_node_depth(const std::string& id, double v);
    /**
     * @brief Edit the first exact-ID stored node hydraulic head and mark the file dirty.
     *
     * Changes only this scalar and dirty flag; does not recompute related depths/heads/volumes, update
     * opaque full engine checkpoints, apply to a live model or flush to disk.
     *
     * @param[in] id Borrowed byte-exact record ID, case-sensitive; no engine name-index lookup.
     * @param[in] v New node hydraulic head in ft elevation; no finite/range validation.
     * @return True on a matching record, false with no mutation when absent.
     */
    bool set_node_head(const std::string& id, double v);
    /**
     * @brief Edit the first exact-ID stored link flow and mark the file dirty.
     *
     * Changes only this scalar and dirty flag; does not recompute related depths/heads/volumes, update
     * opaque full engine checkpoints, apply to a live model or flush to disk.
     *
     * @param[in] id Borrowed byte-exact record ID, case-sensitive; no engine name-index lookup.
     * @param[in] v New link flow in cfs in stored link orientation; no finite/range validation.
     * @return True on a matching record, false with no mutation when absent.
     */
    bool set_link_flow(const std::string& id, double v);
    /**
     * @brief Edit the first exact-ID stored link depth and mark the file dirty.
     *
     * Changes only this scalar and dirty flag; does not recompute related depths/heads/volumes, update
     * opaque full engine checkpoints, apply to a live model or flush to disk.
     *
     * @param[in] id Borrowed byte-exact record ID, case-sensitive; no engine name-index lookup.
     * @param[in] v New link depth in ft; no finite/range validation.
     * @return True on a matching record, false with no mutation when absent.
     */
    bool set_link_depth(const std::string& id, double v);
    /**
     * @brief Edit the first exact-ID stored subcatchment runoff and mark the file dirty.
     *
     * Changes only this scalar and dirty flag; does not recompute related depths/heads/volumes, update
     * opaque full engine checkpoints, apply to a live model or flush to disk.
     *
     * @param[in] id Borrowed byte-exact record ID, case-sensitive; no engine name-index lookup.
     * @param[in] v New subcatchment runoff in cfs; no finite/range validation.
     * @return True on a matching record, false with no mutation when absent.
     */
    bool set_subcatch_runoff(const std::string& id, double v);
};

// ============================================================================
// HotStartManager — static methods (no state; acts as a utility namespace)
// ============================================================================

/**
 * @brief Static utility class for hot start file operations.
 *
 * @details All operations on the binary file are handled here.
 *          The `HotStartFile` struct is the data carrier;
 *          `HotStartManager` provides the I/O logic.
 *
 * @ingroup engine_hotstart
 */
/**
 * @brief Reaction species carried after legacy's `.hsf` routing records.
 *
 * @details Legacy hotstart.c stops reading after the link records, so the
 *          trailer is invisible to it. Network concentrations are applied as
 *          the file is read; the surface block waits here until
 *          msxsurf::initState() has sized the store (apply_species_surface()).
 *          Species are matched by name.
 */
struct LegacySpeciesTrailer {
    std::vector<std::string> species;
    int n_subcatch = 0;
    int n_landuses = -1;         ///< -1: the file has no surface block
    std::vector<double> washoff; ///< [sc * ns + s] washoff concentration
    std::vector<double> ponded;  ///< [sc * ns + s] ponded mass
    std::vector<double> buildup; ///< [(sc * nlu + lu) * ns + s], per normalizer unit
};

class HotStartManager {
public:
    /**
     * @brief Prevent instantiation of the static hot-start utility class.
     */
    HotStartManager() = delete;

    // -----------------------------------------------------------------------
    // Save
    // -----------------------------------------------------------------------

    /**
     * @brief Capture current engine state and write a feature-dependent native hot start.
     *
     * @param ctx   Simulation context (must be RUNNING or ENDED).
     * @param path  Output file path.
     * @returns Heap-allocated `HotStartFile*` on success (caller owns), or
     *          nullptr on I/O error (error description in `last_io_error`).
     * @note Caller deletes the returned object. Source context must be coherent at an accepted boundary; this helper does not enforce the engine lifecycle itself. Destination is directly overwritten. Advanced features can require full session checkpoint capture instead; allocation exceptions may propagate.
     */
    static HotStartFile* save(const SimulationContext& ctx, const std::string& path);

    /**
     * @brief Capture native engine state including optional infiltration and groundwater solvers.
     *
     * @details Gap #54: extends V1 save with per-subcatchment infiltration model
     *          state (6-element flat array) and GW zone state (theta, lower_depth).
     *          Uses at least version 2 and promotes for the captured feature blocks.
     *
     * @param ctx      Simulation context (must be RUNNING or ENDED).
     * @param runoff   RunoffSolver providing infiltration state (may be nullptr).
     * @param gw       GWSolver providing GW zone state (may be nullptr).
     * @param path     Output file path.
     * @return Owned heap file on success (caller deletes), NULL on handled refusal/IO failure with last_io_error().
     * @note Delegates to save_with_engine_checkpoint with an empty payload; feature-dependent full-restart requirements can refuse this partial capture. Destination is directly overwritten; exceptions outside capture guards can propagate.
     */
    static HotStartFile* save(const SimulationContext& ctx, const runoff::RunoffSolver* runoff,
                              const groundwater::GWSolver* gw, const std::string& path);

    /**
     * @brief Capture a native hot start with an optional complete engine-session checkpoint.
     *
     * Chooses feature-dependent versions through V17; nonempty engine payload promotes at least V15 and
     * is limited to 512 MiB. Payload must come from the matching initialized model and qualified
     * session snapshot, not arbitrary bytes. Empty payload refuses configurations needing full restart
     * state. Native file write is direct truncation, without atomic staging. Allocation/other
     * exceptions outside capture guards can propagate.
     *
     * @param[in] ctx Borrowed coherent model state at an accepted save boundary; lifecycle
     *     qualification is enforced by SWMMEngine callers.
     * @param[in] runoff Borrowed runoff solver for infiltration capture, may be NULL.
     * @param[in] gw Borrowed groundwater solver for zone-state capture, may be NULL.
     * @param[in] path UTF-8 destination path overwritten by native binary serialization.
     * @param[in] engine_checkpoint Owned-by-caller opaque accepted-boundary SWMMEngine checkpoint bytes
     *     copied into the file; empty requests a partial native save.
     * @return Heap-allocated file on success, caller deletes it; NULL on handled refusal/IO failure
     *     with last_io_error().
     */
    static HotStartFile* save_with_engine_checkpoint(const SimulationContext& ctx,
                                                     const runoff::RunoffSolver* runoff,
                                                     const groundwater::GWSolver* gw,
                                                     const std::string& path,
                                                     const std::string& engine_checkpoint);

    // -----------------------------------------------------------------------
    // Open
    // -----------------------------------------------------------------------

    /**
     * @brief Read and validate a hot start file.
     *
     * @details Validates magic, version, and CRC32. Loads all records into
     *          memory.
     *
     * @param path  File path.
     * @returns Heap-allocated `HotStartFile*` on success, nullptr on error.
     */
    static HotStartFile* open(const std::string& path);

    /**
     * @brief Probe the native hot-start magic without parsing records.
     *
     * Does not validate version, checksum or trailing records. Declared noexcept; exceptions during
     * path/stream construction therefore terminate.
     *
     * @param[in] path Borrowed UTF-8 input path.
     * @return True when 16 bytes can be read and the first 15 match the native stamp including its NUL;
     *     false for missing/short/legacy files.
     */
    static bool is_native_file(const std::string& path) noexcept;

    // -----------------------------------------------------------------------
    // Apply
    // -----------------------------------------------------------------------

    /**
     * @brief Apply hot start records to a simulation context.
     *
     * @details For each record in the hot start file, the matching object
     *          (by string ID) in `ctx` is updated. Objects in the hot start
     *          that are absent from `ctx` generate warnings — they do not fail.
     *
     * @param hs       Hot start file (from save() or open()).
     * @param ctx      Target simulation context (must be INITIALIZED).
     * @param warn_cb  Optional callback for per-missing-object warnings.
     * @returns Number of missing-object warnings generated (0 = perfect match).
     * @retval -1 Qualified-restart or state-restore refusal; details in last_io_error() and/or hs.warnings.
     * @note No whole-context rollback. A late failure can follow earlier state changes. Full embedded session checkpoints must restore through SWMMEngine start. Warning callback strings are borrowed for the synchronous call; callback exceptions propagate.
     */
    static int apply(HotStartFile& hs, SimulationContext& ctx,
                     std::function<void(const std::string&)> warn_cb = {});

    /**
     * @brief Apply hot start records including infiltration and GW state.
     *
     * @details Gap #54: extended apply that also restores infiltration model
     *          state and GW zone state when the file is V2 format.
     *
     * @param hs       Hot start file (from save() or open()).
     * @param ctx      Target simulation context (must be INITIALIZED).
     * @param runoff   RunoffSolver to restore infiltration state into (may be nullptr).
     * @param gw       GWSolver to restore GW zone state into (may be nullptr).
     * @param warn_cb  Optional callback for per-missing-object warnings.
     * @returns Number of missing-object warnings generated.
     * @retval -1 Qualified-restart or state-restore refusal; details in last_io_error() and/or hs.warnings.
     * @note No whole-context rollback. A late failure can follow earlier state changes. Full embedded session checkpoints must restore through SWMMEngine start. Warning callback strings are borrowed for the synchronous call; callback exceptions propagate.
     */
    static int apply(HotStartFile& hs, SimulationContext& ctx, runoff::RunoffSolver* runoff,
                     groundwater::GWSolver* gw,
                     std::function<void(const std::string&)> warn_cb = {});

    // -----------------------------------------------------------------------
    // Legacy EPA SWMM5 hotstart (.hsf) — read + apply routing state
    // -----------------------------------------------------------------------

    /**
     * @brief Read a legacy EPA SWMM5 `.hsf` (USE HOTSTART) file and apply its
     *        routing state to the context, BY OBJECT INDEX (the legacy format
     *        stores node/link state in object order, no IDs).
     *
     * @details Mirrors legacy hotstart.c readRouting(): sets each node's depth +
     *          lateral inflow and each link's flow + depth + setting, all stored
     *          as float in internal units (ft, cfs). Also restores node/link
     *          pollutant concentrations (current and old) and v4 storage HRT.
     *          Supports legacy versions 1–4. Subcatchment state is skipped with
     *          a warning; this is a routing-only restore. Derived hydraulic
     *          state (head, volumes, old-step values) is recomputed by the caller.
     *          Cold quality seeds must not overwrite the restored concentrations.
     *
     * @param path    Absolute path to the legacy `.hsf` file.
     * @param ctx     Allocated target context; node/link/pollutant counts must
     *                match the file.
     * @param warn_cb Optional warning callback.
     * @param runoff  When given, receives the file's runoff section
     *                (legacy_runoff_size() doubles) for the caller to apply
     *                once the hydrology and quality state exist; when null the
     *                section is skipped with a warning.
     * @returns 0 on success; non-zero error code otherwise (description in
     *          last_io_error()).
     * @param[out] species Optional trailer destination for deferred surface-species application; NULL discards it.
     * @note Parsing applies records incrementally; later failure does not roll back earlier context mutations.
     */
    static int apply_legacy_routing(const std::string& path, SimulationContext& ctx,
                                    std::function<void(const std::string&)> warn_cb = {},
                                    std::vector<double>* runoff = nullptr,
                                    LegacySpeciesTrailer* species = nullptr);

    /**
     * @brief Apply a legacy trailer surface block by reaction-species name.
     *
     * Mismatch emits one warning without surface mutation; unknown species are silently skipped and
     * matching uses exact-case ReactionData names. Destination entries are bounds-tested independently,
     * so true does not guarantee every expected destination was populated. Does not reset ledgers or
     * initialize absent species.
     *
     * @param[in] tr Borrowed trailer with correctly shaped washoff/ponded/buildup arrays for its
     *     counts; source payload lengths are unchecked.
     * @param[in,out] ctx Allocated/resolved MSX surface state. Receives matching species current/old
     *     washoff, ponded mass and buildup.
     * @param[in] warn Optional synchronous warning callback; borrowed message string is valid only
     *     during the call.
     * @return False if trailer has no surface block, surface state inactive, or subcatchment/land-use
     *     counts differ; true otherwise.
     */
    static bool apply_species_surface(const LegacySpeciesTrailer& tr, SimulationContext& ctx,
                                      const std::function<void(const std::string&)>& warn);

    /**
     * @brief Number of doubles in legacy's runoff section (hotstart.c
     *        saveRunoff) for the current model with @p n_land land uses.
     *
     * @details Per subcatchment: 3 sub-area ponded depths + runoff (4),
     *          infiltration state (6), +4 with groundwater, +15 with a
     *          snowpack, and when pollutants exist runoff and ponded quality
     *          (2 per pollutant) plus per land use the buildup of each
     *          pollutant and the date it was last swept.
     * @param[in] ctx Borrowed model counts and subcatchment aquifer/snowpack references.
     * @param[in] n_land Land-use count; negative treated as zero.
     * @return Number of native double slots, not bytes; caller ensures size_t arithmetic does not overflow.
     */
    static std::size_t legacy_runoff_size(const SimulationContext& ctx, int n_land);

    /**
     * @brief Write the current routing state as a legacy EPA SWMM5 `.hsf`
     *        (SAVE HOTSTART), byte-format-compatible with legacy readRouting()
     *        and apply_legacy_routing() above.
     *
     * @details Mirrors legacy hotstart.c saveHotstart()+saveRunoff()+
     *          saveRouting() for the "SWMM5-HOTSTART4" version: 15-char stamp +
     *          object counts (nSub, nLand, nNodes, nLinks, nPollut, flowUnits
     *          as int32), then one double record per subcatchment (ponded
     *          depths, runoff, infiltration, groundwater, snowpack, quality,
     *          buildup — the `runoff` argument), then per node
     *          `depth, latFlow[, storage hrt], qual[]` and per link
     *          `flow, depth, setting, qual[]` — all float, internal units
     *          (ft, cfs). Storage residence time is preserved when quality
     *          state is allocated, and otherwise written as zero.
     *
     * @param path  Absolute path for the `.hsf` file.
     * @param ctx   Source context (final routing state).
     * @param runoff Legacy runoff section (legacy_runoff_size() doubles for
     *               the model's land uses), written between the header and
     *               the routing records. Null writes no runoff section and
     *               zero land uses, which only a model without subcatchments
     *               can read back.
     * @returns 0 on success; non-zero on file-open/write error
     *          (description in last_io_error()).
     */
    static int save_legacy_routing(const std::string& path, const SimulationContext& ctx,
                                   const std::vector<double>* runoff = nullptr);

    // -----------------------------------------------------------------------
    // Flush (write-back modifications)
    // -----------------------------------------------------------------------

    /**
     * @brief Rewrite a dirty native hot-start object to its stored path.
     *
     * On success clears dirty. Failure leaves dirty set and generally updates thread-local error; a
     * direct truncated write may have partially replaced the destination. Does not apply edits to a
     * live simulation or update embedded engine-session bytes.
     *
     * @param[in,out] hs Caller-owned coherent file representation; path must be nonempty when dirty.
     * @return True for an already clean object or successful write; false for empty path/write failure.
     */
    static bool flush(HotStartFile& hs);

    // -----------------------------------------------------------------------
    // Last I/O error (thread-local; for error reporting to C API layer)
    // -----------------------------------------------------------------------

    /** @brief Description of the most recent I/O error (empty = none). */
    static const std::string& last_io_error() noexcept;

private:
    // -----------------------------------------------------------------------
    // Binary I/O helpers
    // -----------------------------------------------------------------------

    /**
     * @brief Serialize a native hot-start representation with a trailing IEEE CRC32.
     *
     * Builds the full body in memory, then writes it and its checksum. Numeric POD encoding uses native
     * machine representation; no byte-order conversion despite the intended little-endian format. Does
     * not create directories, atomically stage, fsync or validate opaque state semantics; allocation
     * exceptions can propagate.
     *
     * @param[in] hs Coherent header/version and record arrays; caller ensures all count/length
     *     narrowing and represented feature state are valid.
     * @param[in] path UTF-8 destination opened directly with binary truncation.
     * @return True for successful stream write, false for guarded encoding/IO failure; some low-level
     *     false paths do not replace last_io_error().
     */
    static bool write_file(const HotStartFile& hs, const std::string& path);
    /**
     * @brief Read and check a native hot-start file into an existing representation.
     *
     * Reads entire file into memory, supports versions 1..17 and native numeric representation. String
     * lengths are bounded at 4096 bytes including stored NUL; embedded engine payload bounded at 512
     * MiB. Checks serialized shape bounds where implemented, without proving model compatibility or
     * opaque payload semantics. Allocation exceptions can propagate.
     *
     * @param[in,out] hs Destination populated incrementally; failed decoding can leave partial records,
     *     so callers normally pass a fresh object.
     * @param[in] path Borrowed UTF-8 input path; stored in hs.path after successful decoding.
     * @return True after checksum/magic/version and guarded record decoding, false on refusal;
     *     thread-local diagnostics are set on many failure paths.
     */
    static bool read_file(HotStartFile& hs, const std::string& path);

    // -----------------------------------------------------------------------
    // CRC32 (IEEE 802.3 polynomial, no external dependency)
    // -----------------------------------------------------------------------

    /**
     * @brief Compute an IEEE 802.3 reflected CRC32 over borrowed bytes.
     *
     * @param[in] data Readable len-byte span; may be NULL only when len==0.
     * @param[in] len Number of bytes to checksum.
     * @return CRC using polynomial 0xEDB88320 and initial/final XOR 0xFFFFFFFF; zero for an empty span.
     */
    static uint32_t crc32(const uint8_t* data, std::size_t len) noexcept;
};

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_HOT_START_MANAGER_HPP */
