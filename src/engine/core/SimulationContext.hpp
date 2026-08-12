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
 * @file SimulationContext.hpp
 * @brief The central, reentrant simulation context for the new engine.
 *
 * @details SimulationContext is the single owner of all simulation state.
 *          It replaces the ~70 global variables in src/solver/globals.h with
 *          a value type that is thread-safe (one context per thread) and
 *          trivially clonable for parallel runs.
 *
 * ### Architecture summary
 *
 * | Legacy (globals.h) | New location |
 * |--------------------|-------------|
 * | `FlowUnits`, `RouteModel`, … | `ctx.options` (SimulationOptions) |
 * | `StartDate`, `EndDate`, `ElapsedTime`, … | `ctx.current_time`, `ctx.options.*` |
 * | `Node[]`, `Nnodes[]` | `ctx.nodes` (NodeData) + `ctx.node_names` (NameIndex) |
 * | `Link[]`, `Nlinks[]` | `ctx.links` (LinkData) + `ctx.link_names` (NameIndex) |
 * | `Subcatch[]`, `Nsubcatch` | `ctx.subcatches` (SubcatchData) + `ctx.subcatch_names` |
 * | `Gage[]`, `Ngages` | `ctx.gages` (GageData) + `ctx.gage_names` |
 * | `Pollut[]`, `Npolluts` + quality state | `ctx.pollutants` (PollutantData) |
 * | `Tseries[]` / `Curve[]` | `ctx.tables` (TableData; kind-aware find_curve/find_timeseries) |
 * | `Coord[]` | `ctx.spatial` (SpatialFrame) |
 * | — (new) | `ctx.user_flags` (UserFlags) |
 *
 * ### Lifecycle
 *
 * ```
 * ctx.reset()          — full cold reset (call before re-running)
 * ctx.save_state()     — snapshot current state into old-step arrays
 * ctx.advance(dt)      — advance simulation clock by dt seconds
 * ctx.output_due()     — true when elapsed_ms >= next_report_ms (legacy grid)
 * ctx.is_complete()    — true when current_time >= end_time
 * ```
 *
 * ### Thread safety
 *
 * A SimulationContext must not be shared between threads without external
 * synchronization. The IO thread receives a **deep copy** (SimulationSnapshot)
 * posted through the ring queue — not a pointer to the live context.
 *
 * @see Legacy reference: src/solver/globals.h
 * @see src/engine/data/NodeData.hpp
 * @see src/engine/data/LinkData.hpp
 * @see src/engine/data/SubcatchData.hpp
 * @see src/engine/data/GageData.hpp
 * @see src/engine/data/PollutantData.hpp
 * @see src/engine/data/TableData.hpp
 * @see src/engine/data/NameIndex.hpp
 * @see src/engine/core/SimulationOptions.hpp
 * @see src/engine/core/SpatialFrame.hpp
 * @see src/engine/core/UserFlags.hpp
 * @see include/openswmm/plugin_sdk/SimulationSnapshot.hpp
 * @ingroup engine_core
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_SIMULATION_CONTEXT_HPP
#define OPENSWMM_ENGINE_SIMULATION_CONTEXT_HPP

#include <cmath>
#include <ctime>
#include <functional>
#include "FilePathPair.hpp"
#include "../data/GageData.hpp"
#include "../data/LinkData.hpp"
#include "../data/NameIndex.hpp"
#include "../data/NodeData.hpp"
#include "../data/NodeSubtypes.hpp"
#include "../data/LinkSubtypes.hpp"
#include "../data/PollutantData.hpp"
#include "../data/SubcatchData.hpp"
#include "../data/TableData.hpp"
#include "SimulationOptions.hpp"
#include "SpatialFrame.hpp"
#include "UserFlags.hpp"
#include "../data/QualityData.hpp"
#include "../data/InflowData.hpp"
#include "../data/InfraData.hpp"
#include "../hydraulics/Transect.hpp"
#include "../data/HydrologyData.hpp"
#include "../data/ForcingData.hpp"
#include "../hydrology/Climate.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace openswmm {

// ============================================================================
// Plugin specification (from [PLUGINS] section)
// ============================================================================

/**
 * @brief One plugin entry from the [PLUGINS] section.
 *
 * @details path is the shared library path (.so/.dylib/.dll).
 *          init_args are the remaining tokens on the same line, passed
 *          verbatim to IOutputPlugin::initialize() / IReportPlugin::initialize().
 *
 * @ingroup engine_core
 */
struct PluginSpec {
    std::string              path;       ///< Shared library path
    std::vector<std::string> init_args;  ///< Extra tokens from the [PLUGINS] row
};

// ============================================================================
// [FILES] section spec — secondary file references
// ============================================================================

/**
 * @brief Mode keyword for one [FILES] row — `SAVE` or `USE`.
 *
 * @details `NONE` is the in-memory default for unconfigured slots so the
 *          writer can skip them.  `SAVE` writes data out at simulation end;
 *          `USE` reads data in at simulation start.
 *
 * @ingroup engine_core
 */
enum class FileMode {
    NONE,
    SAVE,
    USE
};

/**
 * @brief Configuration parsed from the `[FILES]` section.
 *
 * @details Mirrors legacy SWMM5's `Frainfall`, `Frunoff`, `Frdii`,
 *          `Finflows`, `Foutflows`, `FhotstartInput`, `FhotstartOutputs`
 *          structs (see `legacy/engine/iface.c`).  Each kind has an
 *          (`mode`, `path`) pair; modes that the legacy parser doesn't
 *          accept (e.g. RAINFALL only ever reads `USE`) are still stored
 *          so a faithful round-trip is possible — validation can flag
 *          illegal combinations later.
 *
 *          Multi-save HOTSTART (legacy supported up to 10 SAVE rows
 *          with optional datetimes) is now honoured: `hotstart_saves`
 *          is a vector and the parser appends per row.  The C API's
 *          singular `HOTSTART_SAVE_PATH` / `_DATETIME` keys operate on
 *          slot 0 as a back-compat sugar for GUI clients that only
 *          surface a single hot-start save.
 *
 * @ingroup engine_core
 */
struct HotstartSaveEntry {
    FilePathPair path;
    /// Optional `SAVE HOTSTART` datetime as a SWMM decimal day.
    /// `0.0` means "no datetime — write at end of run".
    double       datetime = 0.0;
};

struct FilesSpec {
    FileMode     rainfall_mode = FileMode::NONE;
    FilePathPair rainfall_path;

    FileMode     runoff_mode   = FileMode::NONE;
    FilePathPair runoff_path;

    FileMode     rdii_mode     = FileMode::NONE;
    FilePathPair rdii_path;

    /// Legacy semantics: USE only.
    FilePathPair inflows_path;

    /// Legacy semantics: SAVE only.
    FilePathPair outflows_path;

    /// Legacy semantics: USE — single hot-start input file.
    FilePathPair hotstart_use_path;

    /// Legacy semantics: SAVE — one or more hot-start output files,
    /// each with an optional datetime (SWMM decimal day, 0 = end of
    /// run).  Legacy supported up to 10; we don't enforce that here.
    std::vector<HotstartSaveEntry> hotstart_saves;

    /// True when at least one slot is set; used by InpWriter to decide
    /// whether to emit a `[FILES]` section.
    [[nodiscard]] bool has_any() const noexcept {
        return rainfall_mode != FileMode::NONE
            || runoff_mode   != FileMode::NONE
            || rdii_mode     != FileMode::NONE
            || !inflows_path.empty()
            || !outflows_path.empty()
            || !hotstart_use_path.empty()
            || !hotstart_saves.empty();
    }
};

// ============================================================================
// Engine state enumeration
// ============================================================================

/**
 * @brief High-level engine lifecycle state.
 *
 * @details Mirrors SWMM_EngineState in include/openswmm/engine/openswmm_engine.h
 *          but lives in the C++ domain (scoped enum).
 *
 * @ingroup engine_core
 */
enum class EngineState : int32_t {
    CREATED      = 0,  ///< Context allocated, no input loaded
    OPENED       = 1,  ///< Input file parsed, objects allocated
    INITIALIZED  = 2,  ///< Initial conditions applied
    RUNNING      = 3,  ///< Simulation loop in progress
    PAUSED       = 4,  ///< Simulation paused (future hot-swap support)
    ENDED        = 5,  ///< Simulation loop completed
    REPORTED     = 6,  ///< Summary report written
    CLOSED       = 7,  ///< Resources released
    ERROR_STATE  = 8,  ///< Fatal error; call swmm_engine_last_error()
    BUILDING     = 9   ///< Programmatic model construction in progress (no .inp)
};

// ============================================================================
// StateAccessors
// ============================================================================

/**
 * @brief Solver-neutral hooks for reading and writing solver-internal state
 *        (infiltration, groundwater) at hot-start save/load time.
 *
 * @details The state-IO plugin layer intentionally does not depend on
 *          `runoff::RunoffSolver` or `groundwater::GWSolver` types — those are
 *          implementation details of the engine. SWMMEngine wires the engine's
 *          live solvers into these callbacks at open() time so plugins can
 *          read/write per-subcatchment state through `SimulationContext` alone.
 *
 *          Each callback returns `true` if it handled the call, `false` if no
 *          handler is wired (in which case the caller may choose to fall back
 *          to default values or skip that part of the snapshot).
 *
 * @ingroup engine_core
 */
struct StateAccessors {
    /// Read infiltration model id and the 6-double infiltration state into
    /// the caller-provided buffers. `out_infil` must point to a 6-element array.
    std::function<bool(int subcatch_index, int& out_model, double* out_infil)> get_infil_state;

    /// Apply infiltration state to a subcatchment.
    /// `infil` must point to a 6-element array.
    std::function<bool(int subcatch_index, int model, const double* infil)> set_infil_state;

    /// Read groundwater zone state (upper-zone moisture and lower-zone depth).
    std::function<bool(int subcatch_index, double& out_theta, double& out_lower_depth)> get_gw_state;

    /// Apply groundwater zone state to a subcatchment.
    std::function<bool(int subcatch_index, double theta, double lower_depth)> set_gw_state;

    /// True iff the read-side accessors are wired.
    bool can_read() const noexcept {
        return static_cast<bool>(get_infil_state) || static_cast<bool>(get_gw_state);
    }

    /// True iff the write-side accessors are wired.
    bool can_write() const noexcept {
        return static_cast<bool>(set_infil_state) || static_cast<bool>(set_gw_state);
    }
};

// ============================================================================
// 2D model-IO bridge (forward declarations)
// ============================================================================

// Plain 2D data structs (all header-only and define-free; see
// src/engine/2d/data/). Forward-declared so SimulationContext can carry
// non-owning pointers without pulling the 2D module into every TU.
namespace twoD {
struct MeshData;
struct SolverOptions2D;
struct BoundaryData;
struct PendingBoundaryRow;
struct PendingEdgeConveyanceRow;
} // namespace twoD

// ============================================================================
// SimulationContext
// ============================================================================

/**
 * @brief Central, reentrant simulation context.
 *
 * @details Owns all simulation data. Passed by (non-const) reference to every
 *          solver function. One context per concurrent simulation.
 *
 * @ingroup engine_core
 */
struct SimulationContext {

    // =========================================================================
    // Engine state
    // =========================================================================

    /** @brief Current lifecycle state of the engine. */
    EngineState state = EngineState::CREATED;

    /**
     * @brief Wall-clock time stamped at the start of SWMMEngine::open().
     *
     * @details Reported as "Analysis begun on:" and used as the origin for
     *          "Total elapsed time:" in the .rpt. Stamped before input
     *          parsing so that parse + cross-reference resolution +
     *          validation + module initialization are all included in the
     *          reported elapsed time. This matches legacy, which takes its
     *          timestamp in report_writeLogo() before project_readInput()
     *          (see legacy/engine/report.c). Zero until open() runs.
     */
    std::time_t wall_start = 0;

    // =========================================================================
    // Project title / notes
    // =========================================================================

    /**
     * @brief Project title and notes (from [TITLE] section).
     *
     * @details Each entry is one line of free-form text from the [TITLE]
     *          section of the .inp file. Lines beginning with ";;" (comments)
     *          are stripped by the input reader before reaching the handler.
     *
     * @see Legacy: Title[MAXTITLE] in globals.h (limited to 3 lines)
     */
    std::vector<std::string> title_notes;

    // =========================================================================
    // Options & configuration
    // =========================================================================

    /**
     * @brief Parsed simulation options (from [OPTIONS] section).
     * @see Legacy: FlowUnits, RouteModel, RouteStep, … in globals.h
     */
    SimulationOptions options;

    /**
     * @brief Daily climate state — temperature, evaporation, wind, humidity.
     *
     * @details Scalar broadcast to all subcatchments; updated at the runoff
     *          step boundary by SWMMEngine. Lives on the context so that
     *          downstream solvers (RDII exponential IA, future models)
     *          can read it through `ctx.climate_state` without reaching
     *          into the engine.
     *
     * @see Legacy: Temp, Evap, Wind, Humidity in globals.h
     */
    climate::ClimateState climate_state;

    // =========================================================================
    // Simulation clock
    // =========================================================================

    /**
     * @brief Current simulation time in SECONDS from start_date.
     *
     * @details Updated by TimestepController::advance() with the routing
     *          step in seconds (TimestepController.cpp:88). The value 0.0
     *          corresponds to options.start_date.
     *
     *          The companion field current_date is the OADate (decimal
     *          days since 12/30/1899) computed via
     *          datetime::addSeconds(start_date, current_time).
     *
     *          Callers that need elapsed time in decimal days (e.g. the
     *          control engine's SIM_TIME premise) must divide by
     *          constants::SEC_PER_DAY.  Legacy `ElapsedTime` is in days;
     *          this field stores seconds.
     *
     * @see Legacy: ElapsedTime in globals.h (legacy is in days)
     */
    double current_time = 0.0;

    /**
     * @brief Absolute current date/time (decimal days, OADate (days since 12/30/1899)).
     *
     * @details = options.start_date + current_time
     *
     * @see Legacy: StartDateTime + ElapsedTime in globals.h
     */
    double current_date = 0.0;

    /**
     * @brief Elapsed routing time in MILLISECONDS from start_date.
     *
     * @details Legacy-parity clock: accumulated exactly as legacy
     *          `NewRoutingTime = NewRoutingTime + 1000.0 * routingStep`
     *          (swmm5.c / routing.c:309). Kept alongside current_time
     *          (seconds) because the two accumulations are NOT
     *          FP-identical; report scheduling and report datestamps
     *          must use this clock to match legacy bit-for-bit.
     *
     * @see Legacy: NewRoutingTime in globals.h
     */
    double elapsed_ms = 0.0;

    /**
     * @brief Elapsed routing time (ms) at the START of the current step.
     *
     * @details Legacy OldRoutingTime; with elapsed_ms brackets the report
     *          instant for interpolated output.
     */
    double old_elapsed_ms = 0.0;

    /**
     * @brief Next report instant in MILLISECONDS from start_date.
     *
     * @details Legacy ReportTime: seeded to 1000*report_step at start and
     *          advanced by 1000*report_step per period (swmm5.c:721,1044).
     *          The grid is anchored at SIM start; periods whose datestamp
     *          falls before options.report_start are skipped (not written)
     *          but the grid still advances — exactly legacy
     *          output_saveResults() (output.c:481).
     */
    double next_report_ms = 0.0;

    /**
     * @brief Time remaining until the next control rule event (seconds).
     *
     * @details 0.0 means no control step scheduled; updated by the control
     *          rule processor.
     */
    double dt_controls_remaining = 0.0;

    // =========================================================================
    // Object data stores (Structure-of-Arrays)
    // =========================================================================

    /**
     * @brief All node state and properties.
     * @see Legacy: Node[], NodeStats[] in globals.h + TNode in objects.h
     */
    NodeData nodes;

    /**
     * @brief Relational side-tables for node subtypes (storage/outfall/divider).
     *
     * @details Phase 1 (shadow) of the relational node refactor: built from
     *          `nodes` at hydraulics init and kept as an exact mirror of the
     *          wide subtype arrays. Not yet read by the solver. See
     *          docs/relational/RELATIONAL_NODE_REFACTOR_PLAN.md and
     *          src/engine/data/NodeSubtypes.hpp.
     */
    NodeSubtypes node_subtypes;

    /**
     * @brief All link state and properties.
     * @see Legacy: Link[], LinkStats[] in globals.h + TLink in objects.h
     */
    LinkData links;

    /**
     * @brief Relational per-subtype link side-tables (Phase 6) — the link
     *        analogue of node_subtypes. Authoritative store for conduit/pump/
     *        orifice/weir/outlet config; shared fields (xsect_*, has_flap_gate,
     *        pump_curve_name) stay on `links`. @see data/LinkSubtypes.hpp.
     */
    LinkSubtypes link_subtypes;

    /**
     * @brief All subcatchment state and properties.
     * @see Legacy: Subcatch[], SubcatchStats[] in globals.h + TSubcatch
     */
    SubcatchData subcatches;

    /**
     * @brief All rain gage state and properties.
     * @see Legacy: Gage[] in globals.h + TGage in objects.h
     */
    GageData gages;

    /**
     * @brief Pollutant definitions and per-object quality state.
     * @see Legacy: Pollut[], node.newQual[], link.newQual[] in globals.h
     */
    PollutantData pollutants;

    /**
     * @brief All time series and rating curves.
     * @see Legacy: Tseries[], Curve[] in globals.h + TTable in objects.h
     */
    TableData tables;

    // =========================================================================
    // Name-to-index lookup (O(1))
    // =========================================================================

    /**
     * @brief Node name → node index.
     * @see Legacy: project_findObject(NODE, name) in project.c
     */
    NameIndex node_names;

    /**
     * @brief Link name → link index.
     * @see Legacy: project_findObject(LINK, name) in project.c
     */
    NameIndex link_names;

    /**
     * @brief Subcatchment name → subcatchment index.
     * @see Legacy: project_findObject(SUBCATCH, name) in project.c
     */
    NameIndex subcatch_names;

    /**
     * @brief Rain gage name → gage index.
     * @see Legacy: project_findObject(GAGE, name) in project.c
     */
    NameIndex gage_names;

    /**
     * @brief Pollutant name → pollutant index.
     * @see Legacy: project_findObject(POLLUT, name) in project.c
     */
    NameIndex pollutant_names;

    // NOTE: there is deliberately NO shared name registry for tables.
    // Legacy keeps TSERIES and CURVE in separate hash tables, so a curve and
    // a timeseries may legally share one name; ctx.tables is the authority
    // (each row stores id + type) and lookups are kind-aware via
    // find_timeseries() / find_curve() below.

    // =========================================================================
    // Quality data (landuse, buildup, washoff, treatment)
    // =========================================================================

    NameIndex        landuse_names;
    LanduseData      landuses;
    BuildupData      buildup;
    WashoffData      washoff;
    TreatmentData    treatment;

    // =========================================================================
    // Inflow data (external, DWF, RDII, patterns)
    // =========================================================================

    ExtInflowData    ext_inflows;
    DwfData          dwf_inflows;
    RDIIAssignData   rdii_assigns;
    UnitHydData      unit_hyds;      ///< Parsed [HYDROGRAPHS] data
    RDIIDecayData    rdii_decay;     ///< Parsed [RDII_DECAY] data (exponential IA model)
    PatternData      patterns;

    // =========================================================================
    // Infrastructure data (transects, streets, inlets, controls)
    // =========================================================================

    TransectStore    transects;
    /// Built transect geometry tables (indexed same as transects).
    std::vector<transect::TransectData> transect_tables;
    /// Bumped whenever a link's cross-section/flow properties are recomputed
    /// mid-run (C-API setters via recompute_conduit_flow_properties), so
    /// consumers holding per-link XSectParams caches know to rebuild
    /// (e.g. SWMMEngine's reporting-path cache).
    std::uint64_t xsect_generation = 0;

    /// True when the model was loaded from a GeoPackage, whose hydraulic fields
    /// are stored in CANONICAL internal units (feet/cfs/ft³) — NOT the display
    /// units a .inp uses. resolve_cross_references then SKIPS the display→
    /// internal conversion (convert_inputs_to_internal) so the round-trip is
    /// bit-for-bit identical (no non-invertible ×0.3048). The GeoPackage reader
    /// converts only the cross-section raw geom1-4 (the lone display-unit fields
    /// it stores) itself. Left false for the .inp path.
    bool gpkg_units_internal = false;

    StreetStore      streets;
    InletStore       inlets;
    InletUsageStore  inlet_usages;
    ControlRuleStore control_rules;

    // =========================================================================
    // Events (from [EVENTS] section)
    // =========================================================================

    /**
     * @brief Event time periods for event-based analysis/reporting.
     * @details Each pair is {start, end} in DateTime (decimal days).
     * @see Legacy: TEvent in objects.h
     */
    struct Event {
        double start = 0.0;  ///< Event start (DateTime decimal days)
        double end   = 0.0;  ///< Event end (DateTime decimal days)
    };
    std::vector<Event> events;

    // =========================================================================
    // Subcatchment adjustment patterns (from [ADJUSTMENTS] section)
    // =========================================================================

    /**
     * @brief Monthly climate adjustment factors from [ADJUSTMENTS] section.
     * @details Copied into ClimateState during initialize(). Stored here so
     *          that input parsing populates them without accessing SWMMEngine.
     * @see Legacy: Adjust struct (temp[], evap[], rain[], hydcon[])
     */
    double adjust_temp[12]   = {0,0,0,0,0,0,0,0,0,0,0,0};
    double adjust_evap[12]   = {1,1,1,1,1,1,1,1,1,1,1,1};
    double adjust_rain[12]   = {1,1,1,1,1,1,1,1,1,1,1,1};
    double adjust_hydcon[12] = {1,1,1,1,1,1,1,1,1,1,1,1};

    /**
     * @brief Per-subcatchment pattern indices for N-PERV, DSTORE, INFIL adjustments.
     * @details Index into ctx.tables / pattern tables. -1 = no pattern.
     * @see Legacy: Subcatch[i].nPervPattern, dStorePattern, infilPattern
     */
    std::vector<int> subcatch_n_perv_pattern;
    std::vector<int> subcatch_d_store_pattern;
    std::vector<int> subcatch_infil_pattern;

    /// Base values for pattern-adjusted parameters (populated at init).
    std::vector<double> base_n_perv;
    std::vector<double> base_ds_perv;
    bool has_subcatch_adj_patterns = false; ///< True if any pattern index >= 0

    // =========================================================================
    // Hydrology data (snowpacks, aquifers, LID)
    // =========================================================================

    SnowpackStore    snowpacks;
    NameIndex        snowpack_names;
    AquiferStore     aquifers;
    NameIndex        aquifer_names;
    LidControlStore  lid_controls;
    NameIndex        lid_names;
    LidUsageStore    lid_usage;

    // =========================================================================
    // Spatial data
    // =========================================================================

    /**
     * @brief Coordinate reference system and georeferenced coordinates.
     * @see Legacy: Coord[] in globals.h
     */
    SpatialFrame spatial;

    // =========================================================================
    // User-defined flags
    // =========================================================================

    /**
     * @brief User-defined flags from [USER_FLAGS] section.
     * @details New in 6.0.0. No legacy equivalent.
     * @see UserFlags.hpp
     */
    UserFlags user_flags;

    // =========================================================================
    // Object tags (from [TAGS] section)
    // =========================================================================

    // Tags from the [TAGS] section now live per-index on
    // NodeData::tags / LinkData::tags / SubcatchData::tags. Storing
    // them name-keyed here was a latent rename bug — a tagged node
    // would lose its tag the moment `swmm_node_rename` was called.

    // =========================================================================
    // Runtime forcing data
    // =========================================================================

    /**
     * @brief Per-element runtime forcing state (lateral inflows, head
     *        boundaries, rainfall, evap, link settings, quality mass fluxes).
     * @details Set via swmm_forcing_*() API. Applied by applyForcings() in
     *          the step loop. Auto-cleared by clear_reset_entries() at end
     *          of each step (for RESET-persistence entries).
     * @see ForcingData.hpp, openswmm_forcing.h
     */
    ForcingData forcing;

    // =========================================================================
    // Plugin specifications (from [PLUGINS] section)
    // =========================================================================

    /**
     * @brief Plugin library specs parsed from [PLUGINS].
     * @details Consumed by PluginFactory::load_plugins() during open().
     *          New in 6.0.0 — no legacy equivalent.
     */
    std::vector<PluginSpec> plugin_specs;

    /**
     * @brief Secondary file references parsed from [FILES].
     * @details Mirrors legacy SWMM5's TFile struct array — rainfall,
     *          runoff, RDII, inflows, outflows, hotstart save/use.
     *          The simulation engine consults specific slots when the
     *          corresponding feature is requested (e.g. interfacing
     *          a routing run with a separately-saved runoff file).
     */
    FilesSpec files;

    /**
     * @brief Solver-neutral accessors for reading and writing solver-internal
     *        state at hot-start save/load time.
     *
     * @details Wired by SWMMEngine at open() time so state-IO plugins can
     *          access per-subcatchment infiltration and groundwater state
     *          without depending on solver types.
     */
    StateAccessors state_accessors;

    /**
     * @brief Non-owning pointers to the engine's 2D surface-routing model
     *        storage (mesh, solver options, boundary conditions, parse-time
     *        pending rows).
     *
     * @details Wired by SWMMEngine (wire2DModelIO) so serialization consumers
     *          — the built-in InpWriter and input plugins such as the
     *          GeoPackage reader/writer — can read AND populate the 2D model
     *          without depending on SurfaceRouter2D or the engine. All
     *          pointers are null when the engine was built without 2D
     *          support, or for detached/test-built contexts; consumers must
     *          runtime-guard on the pointers.
     *
     *          This member is intentionally UNCONDITIONAL (no
     *          OPENSWMM_HAS_2D guard): translation units outside the engine
     *          target (e.g. the geopackage static lib) compile this header
     *          without that define, and a conditional member would give the
     *          struct two layouts in one binary. It is also intentionally
     *          preserved by reset() — it describes wiring, not model data.
     */
    struct TwoDModelIO {
        twoD::MeshData*                              mesh       = nullptr;
        twoD::SolverOptions2D*                       options    = nullptr;
        twoD::BoundaryData*                          boundary   = nullptr;
        std::vector<twoD::PendingBoundaryRow>*       pending_bc = nullptr;
        std::vector<twoD::PendingEdgeConveyanceRow>* pending_ec = nullptr;
    } twod_io;

    /**
     * @brief Section rows whose target object had not been parsed yet.
     *
     * @details Sections are dispatched in the order they appear in the file,
     *          so a property section that names an object defined further down
     *          (e.g. an [XSECTIONS] row for a link declared in a later
     *          [ORIFICES]) cannot resolve its name on the first pass. Legacy
     *          SWMM avoids this with an ID pre-pass; here a handler stashes the
     *          unresolved row as (section tag, raw line) and InputReader
     *          re-dispatches it once every section has been read. Rows still
     *          unresolved after that replay name an object that does not exist
     *          and raise ERROR 209, as legacy does.
     *
     *          Parse-time scratch only — empty once reading finishes.
     */
    std::vector<std::pair<std::string, std::string>> deferred_section_rows;

    // =========================================================================
    // Error / warning tracking
    // =========================================================================

    /**
     * @brief Most recent error code (0 = no error).
     * @see Legacy: ErrorCode in globals.h
     */
    int error_code = 0;

    /**
     * @brief Most recent warning code (0 = no warning).
     * @see Legacy: WarningCode in globals.h
     */
    int warning_code = 0;

    /**
     * @brief Human-readable message for the last error/warning.
     */
    std::string error_message;

    /**
     * @brief Accumulated warning messages written to report file.
     * @details Matches legacy behavior where warnings are collected during
     *          validation and written after the title section in the .rpt file.
     *          Format: "WARNING NN: description for ObjectID"
     * @see Legacy: report_writeWarningMsg() in report.c
     */
    std::vector<std::string> warnings;

    /**
     * @brief Accumulated error messages written to report file.
     * @details Matches legacy behavior where errors are collected during
     *          parsing/validation and written to the .rpt file.
     *          Format: "ERROR NNN: description for ObjectID"
     * @see Legacy: report_writeErrorMsg() in report.c
     */
    std::vector<std::string> errors;

    /**
     * @brief Per-node flag: 1 if the node is a 2D-coupled junction (a non-outfall
     *        coupling point), else 0. Empty when the 2D module is inactive.
     * @details Populated by SurfaceRouter2D::initialize from the coupling map.
     *          Lets the 1D dynamic-wave solver treat a coupled junction as
     *          pond-capable so its HGL can rise above the crown over the
     *          auto-assigned ponded_area (the 2D-cell footprint), regardless of
     *          the global ALLOW_PONDING option — which only governs ordinary
     *          (uncoupled) junctions. Outfall coupling uses a tailwater head BC
     *          instead, so outfalls are NOT flagged here.
     */
    std::vector<std::uint8_t> coupled_node;

    /**
     * @brief Remaining seconds of the current 1D↔2D coupling delivery window.
     * @details Set by SurfaceRouter2D::fireAdvanceWindow to the length of the
     *          2D advance window whose junction-exchange volumes it just moved
     *          into NodeData::coupling_queue. assembleLateralInflows drains the
     *          queue at the uniform rate queue/remaining, counts this down once
     *          per routing step, and flushes the whole remainder on the step
     *          where remaining ≤ dt — so the 1D node receives exactly the
     *          exchanged volume spread over the window instead of as a
     *          single-step pulse. 0 (the cold-start default, and the steady
     *          state when the 2D advance fires every routing step) reproduces
     *          the legacy one-step delivery exactly.
     */
    double coupling_delivery_remaining = 0.0;

    // =========================================================================
    // Mass balance accumulators (SoA — vectorisable batch updates)
    // =========================================================================

    /**
     * @brief Cumulative mass balance totals for runoff and routing.
     *
     * @details Updated every timestep by the runoff and routing modules.
     *          Stored as contiguous doubles for batch accumulation.
     *          All volumes in ft3, all flows integrated to volumes.
     *
     * @see Legacy: RunoffTotals, RoutingTotals in massbal.c
     */
    struct MassBalance {
        // Runoff totals
        double runoff_rainfall   = 0.0;  ///< Total rainfall volume (ft3)
        double runoff_evap       = 0.0;  ///< Total evaporation volume (ft3)
        double runoff_infil      = 0.0;  ///< Total infiltration volume (ft3)
        double runoff_runoff     = 0.0;  ///< Total surface runoff volume (ft3)
        double runoff_snowremov  = 0.0;  ///< Total snow removal volume (ft3)
        double runoff_init_store = 0.0;  ///< Initial surface storage (ft3)
        double runoff_final_store= 0.0;  ///< Final surface storage (ft3)

        // Routing totals
        double routing_dry_weather   = 0.0;
        double routing_wet_weather   = 0.0;
        double routing_gw_inflow     = 0.0;
        double routing_rdii          = 0.0;
        double routing_external      = 0.0;
        double routing_flooding      = 0.0;
        double routing_outflow       = 0.0;
        double routing_evap_loss     = 0.0;
        double routing_seep_loss     = 0.0;
        double routing_init_storage  = 0.0;
        double routing_final_storage = 0.0;

        // User-forced volumes (diagnostic — subset of routing_external)
        double routing_forcing_inflow = 0.0; ///< Cumulative user-forced lateral inflow (ft3)

        // User-forced quality mass (diagnostic — cumulative mass injected via user_conc_mass_flux)
        std::vector<double> routing_forcing_qual_inflow; ///< Per-pollutant cumulative user-forced quality mass

        // Groundwater mass balance totals (all in feet — depth per unit area)
        // Matches legacy TGwaterTotals
        double gw_infil         = 0.0; ///< Cumulative infiltration to GW (ft)
        double gw_upper_evap    = 0.0; ///< Cumulative upper zone evaporation (ft)
        double gw_lower_evap    = 0.0; ///< Cumulative lower zone evaporation (ft)
        double gw_lower_perc    = 0.0; ///< Cumulative deep percolation (ft)
        double gw_lateral_flow  = 0.0; ///< Cumulative lateral GW flow (ft)
        double gw_init_storage  = 0.0; ///< Initial GW storage (ft)
        double gw_final_storage = 0.0; ///< Final GW storage (ft)

        // Per-step accumulators (reset each step for reporting)
        double step_flooding     = 0.0;
        double step_outflow      = 0.0;
        double step_dw_inflow    = 0.0;
        double step_gw_inflow    = 0.0;
        double step_rdii_inflow  = 0.0;
        double step_ext_inflow   = 0.0;

        // Quality mass balance (per-pollutant, in mass units)
        std::vector<double> qual_init_buildup;   ///< Initial buildup mass
        std::vector<double> qual_final_buildup;  ///< Final buildup mass
        std::vector<double> qual_surface_buildup; ///< Accumulated buildup during sim
        std::vector<double> qual_wet_deposition; ///< Wet deposition mass
        std::vector<double> qual_sweeping;       ///< Mass removed by sweeping
        std::vector<double> qual_bmp_removal;    ///< BMP treatment removal
        std::vector<double> qual_infil_loss;     ///< Mass lost to infiltration
        std::vector<double> qual_runoff_load;    ///< Mass load in surface runoff
        std::vector<double> qual_routing_wet;    ///< Wet weather quality inflow to routing
        std::vector<double> qual_routing_outflow;///< Quality mass leaving at outfalls
        std::vector<double> qual_routing_flood;  ///< Quality mass lost to flooding
        std::vector<double> qual_routing_init;   ///< Initial stored quality mass
        std::vector<double> qual_routing_final;  ///< Final stored quality mass
        std::vector<double> qual_routing_reacted;///< Quality mass lost to decay
        std::vector<double> qual_routing_ii_in;  ///< RDII quality mass inflow
        std::vector<double> qual_routing_dw_in;  ///< Dry weather quality mass inflow
        std::vector<double> qual_routing_gw_in;  ///< Groundwater quality mass inflow
        std::vector<double> qual_routing_ex_in;  ///< External (interface file) quality mass inflow
        std::vector<double> qual_routing_seep;   ///< Quality mass lost to seepage
        std::vector<double> qual_routing_evap;   ///< Quality mass lost to evaporation

        void resize_quality(int n_pollutants) {
            auto np = static_cast<std::size_t>(n_pollutants);
            qual_init_buildup.assign(np, 0.0);
            qual_final_buildup.assign(np, 0.0);
            qual_surface_buildup.assign(np, 0.0);
            qual_wet_deposition.assign(np, 0.0);
            qual_sweeping.assign(np, 0.0);
            qual_bmp_removal.assign(np, 0.0);
            qual_infil_loss.assign(np, 0.0);
            qual_runoff_load.assign(np, 0.0);
            qual_routing_wet.assign(np, 0.0);
            qual_routing_outflow.assign(np, 0.0);
            qual_routing_flood.assign(np, 0.0);
            qual_routing_init.assign(np, 0.0);
            qual_routing_final.assign(np, 0.0);
            qual_routing_reacted.assign(np, 0.0);
            qual_routing_ii_in.assign(np, 0.0);
            qual_routing_dw_in.assign(np, 0.0);
            qual_routing_gw_in.assign(np, 0.0);
            qual_routing_ex_in.assign(np, 0.0);
            qual_routing_seep.assign(np, 0.0);
            qual_routing_evap.assign(np, 0.0);
            routing_forcing_qual_inflow.assign(np, 0.0);
        }

        void reset() {
            // Save quality vectors, reset scalars, restore vectors
            auto qi = std::move(qual_init_buildup);
            auto qf = std::move(qual_final_buildup);
            auto qsb = std::move(qual_surface_buildup);
            auto qwd = std::move(qual_wet_deposition);
            auto qsw = std::move(qual_sweeping);
            auto qbmp = std::move(qual_bmp_removal);
            auto qil = std::move(qual_infil_loss);
            auto qrl = std::move(qual_runoff_load);
            auto qrw = std::move(qual_routing_wet);
            auto qro = std::move(qual_routing_outflow);
            auto qrf = std::move(qual_routing_flood);
            auto qri = std::move(qual_routing_init);
            auto qrfi = std::move(qual_routing_final);
            auto qrr = std::move(qual_routing_reacted);
            auto qrii = std::move(qual_routing_ii_in);
            auto qrdw = std::move(qual_routing_dw_in);
            auto qrgw = std::move(qual_routing_gw_in);
            auto qrex = std::move(qual_routing_ex_in);
            auto qrseep = std::move(qual_routing_seep);
            auto qrevap = std::move(qual_routing_evap);
            auto qrfqi = std::move(routing_forcing_qual_inflow);
            *this = MassBalance{};
            qual_init_buildup = std::move(qi);
            qual_final_buildup = std::move(qf);
            qual_surface_buildup = std::move(qsb);
            qual_wet_deposition = std::move(qwd);
            qual_sweeping = std::move(qsw);
            qual_bmp_removal = std::move(qbmp);
            qual_infil_loss = std::move(qil);
            qual_runoff_load = std::move(qrl);
            qual_routing_wet = std::move(qrw);
            qual_routing_outflow = std::move(qro);
            qual_routing_flood = std::move(qrf);
            qual_routing_init = std::move(qri);
            qual_routing_final = std::move(qrfi);
            qual_routing_reacted = std::move(qrr);
            qual_routing_ii_in = std::move(qrii);
            qual_routing_dw_in = std::move(qrdw);
            qual_routing_gw_in = std::move(qrgw);
            qual_routing_ex_in = std::move(qrex);
            qual_routing_seep = std::move(qrseep);
            qual_routing_evap = std::move(qrevap);
            routing_forcing_qual_inflow = std::move(qrfqi);
            // Zero out the quality vectors (except init_buildup which is
            // computed once during initQuality and must survive reset)
            for (auto* v : {&qual_final_buildup,
                            &qual_surface_buildup, &qual_wet_deposition,
                            &qual_sweeping, &qual_bmp_removal,
                            &qual_infil_loss, &qual_runoff_load,
                            &qual_routing_wet, &qual_routing_outflow,
                            &qual_routing_flood, &qual_routing_init,
                            &qual_routing_final, &qual_routing_reacted,
                            &qual_routing_ii_in,
                            &qual_routing_dw_in,
                            &qual_routing_gw_in,
                            &qual_routing_ex_in,
                            &qual_routing_seep,
                            &qual_routing_evap,
                            &routing_forcing_qual_inflow}) {
                std::fill(v->begin(), v->end(), 0.0);
            }
        }

        /// Runoff continuity error (fraction).
        double runoff_error() const {
            double total_in = runoff_rainfall + runoff_init_store;
            double total_out = runoff_evap + runoff_infil + runoff_runoff + runoff_final_store;
            return (total_in > 0.0) ? (total_in - total_out) / total_in : 0.0;
        }

        /// Routing continuity error (fraction).
        double routing_error() const {
            double total_in = routing_dry_weather + routing_wet_weather +
                              routing_gw_inflow + routing_rdii + routing_external +
                              routing_init_storage;
            double total_out = routing_flooding + routing_outflow +
                               routing_evap_loss + routing_seep_loss +
                               routing_final_storage;
            return (total_in > 0.0) ? (total_in - total_out) / total_in : 0.0;
        }

        /// Groundwater continuity error (fraction). Gap #72.
        double gw_error() const {
            double total_in  = gw_infil + gw_init_storage;
            double total_out = gw_upper_evap + gw_lower_evap + gw_lower_perc +
                               gw_lateral_flow + gw_final_storage;
            return (total_in > 0.0) ? (total_in - total_out) / total_in : 0.0;
        }
    } mass_balance;

    /**
     * @brief System mass-balance totals for the optional 2D surface domain.
     *
     * @details Accumulated each executed 2D step by SurfaceRouter2D. Unlike
     *          the 1D MassBalance (ft³ for US flow units), all volumes here are
     *          in the 2D solver's SI internal units (m³). This is a SEPARATE
     *          balance from the 1D routing balance: the coupling terms are
     *          inflows/outflows of the 2D domain, signed oppositely to the 1D
     *          routing_external/routing_flooding terms.
     */
    struct MassBalance2D {
        double init_storage          = 0.0;  ///< Initial surface storage (m³)
        double final_storage         = 0.0;  ///< Latest surface storage (m³)
        double rainfall_in           = 0.0;  ///< Cumulative rainfall volume (m³)
        double coupling_1d_to_2d_in  = 0.0;  ///< Cumulative 1D→2D spill into 2D (m³)
        double coupling_2d_to_1d_out = 0.0;  ///< Cumulative 2D→1D drainage out (m³)
        double outfall_in            = 0.0;  ///< Cumulative 1D outfall discharge into 2D (m³)
        double outfall_out           = 0.0;  ///< Cumulative 2D→pipe withdrawal at submerged outfalls (m³)
        double boundary_in           = 0.0;  ///< Cumulative boundary inflow (m³)
        double boundary_out          = 0.0;  ///< Cumulative boundary outflow (m³)
        double evap_out              = 0.0;  ///< Cumulative evaporation loss (m³)
        bool   active                = false;///< True if the 2D module ran

        // Cumulative marcher statistics (published by SurfaceRouter2D at
        // finalize from ISurfaceSolver::run_stats). Printed as the "2D Solver
        // Statistics" report block. -1 = not populated.
        long   solver_nsteps         = -1;   ///< internal (marcher) substeps
        long   solver_nrhs           = 0;    ///< face-kernel evaluations
        double solver_avg_h          = 0.0;  ///< mean accepted internal step (s)
        double solver_last_h         = 0.0;  ///< last accepted internal step (s)

        // Marcher telemetry (guarded by n_tiers > 0 / non-negative fractions).
        double solver_active_min     = -1.0; ///< min active-cell fraction
        double solver_active_mean    = -1.0; ///< mean active-cell fraction
        double solver_active_max     = -1.0; ///< max active-cell fraction
        long   solver_tier_cells[8]  = {0};  ///< cumulative cells per LTS tier
        int    solver_n_tiers        = 0;    ///< populated tier count

        /// 2D surface continuity error (fraction).
        double error() const {
            double total_in  = rainfall_in + coupling_1d_to_2d_in + outfall_in
                               + boundary_in + init_storage;
            double total_out = coupling_2d_to_1d_out + outfall_out + boundary_out
                               + evap_out + final_storage;
            return (total_in > 0.0) ? (total_in - total_out) / total_in : 0.0;
        }
    } mass_balance_2d;

    // =========================================================================
    // Routing time-step statistics
    // =========================================================================

    /// Top-N element statistic entry (matching legacy TMaxStats).
    struct MaxStats {
        int    obj_type = -1;  ///< 0 = NODE, 1 = LINK
        int    index    = -1;  ///< element index (-1 = unused slot)
        double value    = 0.0; ///< statistic value (percentage or index)
    };

    static constexpr int MAX_STATS = 5;

    /// Top-5 CFL time-step critical elements.
    MaxStats max_courant_crit[MAX_STATS] = {};
    /// Top-5 links with highest flow instability index.
    MaxStats max_flow_turns[MAX_STATS] = {};
    /// Top-5 nodes with highest non-convergence frequency.
    MaxStats max_non_converged[MAX_STATS] = {};
    /// Top-5 nodes with highest continuity errors.
    MaxStats max_mass_bal_errs[MAX_STATS] = {};

    struct RoutingStepStats {
        double min_step  = 1.0e30; ///< Minimum routing time step used (sec)
        double max_step  = 0.0;    ///< Maximum routing time step used (sec)
        double sum_step  = 0.0;    ///< Sum of all routing time steps (sec)
        long   n_steps   = 0;      ///< Total number of routing steps
        double steady_pct = 0.0;   ///< Percent of time in steady state

        /// Number of time step histogram bins (matching legacy TIMELEVELS=5).
        static constexpr int N_TIME_BINS = 5;
        long   step_counts[N_TIME_BINS + 1] = {};   ///< Histogram bin counts
        double step_intervals[N_TIME_BINS + 1] = {}; ///< Histogram bin edges

        /// Non-convergence and iteration tracking
        long   n_non_converged = 0;   ///< Steps that didn't converge
        double sum_iterations  = 0.0; ///< Sum of iterations for averaging
        double max_courant     = 0.0; ///< Maximum Courant number observed

        void update(double dt) {
            min_step = std::min(min_step, dt);
            max_step = std::max(max_step, dt);
            sum_step += dt;
            ++n_steps;
        }

        /// Record iteration count for a routing step
        void update_iterations(int iters, bool converged) {
            sum_iterations += iters;
            if (!converged) ++n_non_converged;
        }

        /// Initialize histogram bin edges using log-scale between max and min
        /// routing steps. Call once at simulation start (matching legacy stats.c
        /// stats_open: intervals from RouteStep down to MinRouteStep).
        void init_histogram(double route_step, double min_route_step) {
            if (route_step <= 0.0) return;
            if (min_route_step <= 0.0) min_route_step = route_step;
            double log_hi = std::log10(route_step);
            double log_lo = std::log10(min_route_step);
            double delta = (log_hi - log_lo) / static_cast<double>(N_TIME_BINS);
            step_intervals[0] = route_step;
            for (int i = 1; i <= N_TIME_BINS; ++i)
                step_intervals[i] = std::pow(10.0, log_hi - i * delta);
            step_intervals[N_TIME_BINS] = min_route_step;
        }

        /// Build histogram bin edges post-hoc from observed min/max
        /// (fallback if init_histogram was not called).
        void build_histogram() {
            if (n_steps == 0 || max_step <= 0.0) return;
            double hi = max_step;
            double lo = (min_step < 1.0e30) ? min_step : 0.0;
            if (lo <= 0.0) lo = hi;
            init_histogram(hi, lo);
        }

        /// Add a step to the histogram (call during simulation or post-process)
        void record_step_bin(double dt) {
            for (int i = 0; i < N_TIME_BINS; ++i) {
                if (dt >= step_intervals[i+1]) {
                    step_counts[i]++;
                    return;
                }
            }
            step_counts[N_TIME_BINS - 1]++;
        }

        double avg_step() const {
            return (n_steps > 0) ? sum_step / static_cast<double>(n_steps) : 0.0;
        }

        double pct_non_converged() const {
            return (n_steps > 0) ? 100.0 * static_cast<double>(n_non_converged) / static_cast<double>(n_steps) : 0.0;
        }

        double computed_avg_iterations() const {
            return (n_steps > 0) ? sum_iterations / static_cast<double>(n_steps) : 0.0;
        }
    } routing_stats;

    // =========================================================================
    // Virtual-junction diagnostics (refactored engine only)
    // =========================================================================

    /**
     * @brief Per-virtual-junction momentum-residual accumulators.
     *
     * @details Populated by DWSolver after each converged routing step; the
     *          residual is the discrete momentum-flux imbalance across the
     *          pair interface (see VIRTUAL_JUNCTION_IMPLEMENTATION_PLAN.md
     *          §3.2). Reported in the .rpt Virtual Junction Summary.
     */
    struct VJDiag {
        std::vector<int>       node_idx;   ///< virtual junction node index
        std::vector<int>       up_link;    ///< upstream conduit (through orientation, -1 sag/peak)
        std::vector<int>       dn_link;    ///< downstream conduit (-1 sag/peak)
        std::vector<double>    resid_max;  ///< max |R_j| over the run (cfs·ft/s)
        std::vector<double>    resid_sum;  ///< Σ|R_j| for mean reporting
        std::vector<long long> resid_n;    ///< number of accumulated steps
        void clear() {
            node_idx.clear(); up_link.clear(); dn_link.clear();
            resid_max.clear(); resid_sum.clear(); resid_n.clear();
        }
    } vj_diag;

    // =========================================================================
    // Control action log — Gap #67
    // Populated by ControlEngine::applyPendingActions() when rpt_controls is on.
    // =========================================================================

    /// One entry per control rule action that changed a link setting.
    struct ControlLogEntry {
        int         link_idx;    ///< Index of the link whose setting changed
        std::string rule_name;   ///< Name of the rule that triggered the change
        double      new_setting; ///< The new target setting value (0-1)
        double      date;        ///< OADate when the change occurred
    };

    /// Chronological log of all control actions taken during the simulation.
    std::vector<ControlLogEntry> control_log;

    // =========================================================================
    // Input file path (for model write / hot start)
    // =========================================================================

    /**
     * @brief Path to the input .inp file (empty if not opened from a file).
     * @see Legacy: InpFile in globals.h
     */
    std::string inp_file_path;

    // =========================================================================
    // Context-level operations
    // =========================================================================

    /**
     * @brief Insertion sort into a descending-by-absolute-value top-N array.
     * @details Matches legacy stats_updateMaxStats() in stats.c.
     */
    static void updateMaxStats(MaxStats arr[], int obj_type, int idx, double value) {
        MaxStats candidate;
        candidate.obj_type = obj_type;
        candidate.index    = idx;
        candidate.value    = value;
        for (int k = 0; k < MAX_STATS; ++k) {
            if (std::fabs(candidate.value) > std::fabs(arr[k].value)) {
                MaxStats tmp = arr[k];
                arr[k] = candidate;
                candidate = tmp;
            }
        }
    }

    /**
     * @brief Compute top-5 arrays for CFL-critical, flow turns, and non-convergence.
     * @details Called once at end of simulation before reporting (matching legacy
     *          stats_findMaxStats in stats.c).
     */
    void finalize_max_stats() {
        long step_count = routing_stats.n_steps;
        if (step_count <= 0) return;
        double inv_steps = 1.0 / static_cast<double>(step_count);

        // CFL-critical elements: percentage of steps each element was critical
        for (int j = 0; j < n_nodes(); ++j) {
            double x = nodes.stat_time_courant_critical[static_cast<std::size_t>(j)] * inv_steps;
            updateMaxStats(max_courant_crit, 0, j, 100.0 * x);
        }
        for (int j = 0; j < n_links(); ++j) {
            double x = links.stat_time_courant_critical[static_cast<std::size_t>(j)] * inv_steps;
            updateMaxStats(max_courant_crit, 1, j, 100.0 * x);
        }

        // Flow instability index (matching legacy normalization)
        long rpt_steps = routing_stats.n_steps;
        if (rpt_steps > 2) {
            double z = 100.0 / (2.0 / 3.0 * static_cast<double>(rpt_steps - 2));
            for (int j = 0; j < n_links(); ++j) {
                double x = static_cast<double>(links.stat_flow_turns[static_cast<std::size_t>(j)]) * z;
                updateMaxStats(max_flow_turns, 1, j, x);
            }
        }

        // Non-convergence: fraction of total steps each node failed to converge
        for (int j = 0; j < n_nodes(); ++j) {
            double x = static_cast<double>(nodes.stat_non_converged_count[static_cast<std::size_t>(j)]) * inv_steps;
            updateMaxStats(max_non_converged, 0, j, x);
        }
    }

    /**
     * @brief Fully reset the context to a CREATED state.
     *
     * @details Clears all object data and name indices. Does NOT free the
     *          option structs (they are re-parsed from the input file).
     *          Call this before re-running or re-opening a simulation.
     */
    void reset() {
        state = EngineState::CREATED;
        control_log.clear();
        current_time = 0.0;
        current_date = 0.0;
        dt_controls_remaining = 0.0;
        elapsed_ms = 0.0;
        old_elapsed_ms = 0.0;
        next_report_ms = 0.0;
        error_code = 0;
        warning_code = 0;
        error_message.clear();
        warnings.clear();
        errors.clear();
        title_notes.clear();
        deferred_section_rows.clear();

        // Clear SoA stores
        nodes      = NodeData{};
        links      = LinkData{};
        subcatches = SubcatchData{};
        gages      = GageData{};
        pollutants = PollutantData{};
        tables     = TableData{};

        // Clear name indices
        node_names.clear();
        link_names.clear();
        subcatch_names.clear();
        gage_names.clear();
        pollutant_names.clear();

        // Clear inflow-related stores that aren't reset by their owning solvers
        rdii_decay = RDIIDecayData{};

        // Virtual-junction diagnostics
        vj_diag.clear();

        // Clear daily climate state (re-initialized by SWMMEngine on next run)
        climate_state = climate::ClimateState{};

        // Clear spatial, flags, events, and forcing. Per-object tags
        // are owned by NodeData/LinkData/SubcatchData and cleared when
        // those SoAs are resized/cleared by the wider reset path.
        spatial    = SpatialFrame{};
        user_flags.clear();
        events.clear();
        std::fill(std::begin(adjust_temp), std::end(adjust_temp), 0.0);
        std::fill(std::begin(adjust_evap), std::end(adjust_evap), 1.0);
        std::fill(std::begin(adjust_rain), std::end(adjust_rain), 1.0);
        std::fill(std::begin(adjust_hydcon), std::end(adjust_hydcon), 1.0);
        subcatch_n_perv_pattern.clear();
        subcatch_d_store_pattern.clear();
        subcatch_infil_pattern.clear();
        base_n_perv.clear();
        base_ds_perv.clear();
        has_subcatch_adj_patterns = false;
        forcing    = ForcingData{};
    }

    /**
     * @brief Snapshot current state into old-step arrays before solving.
     *
     * @details Must be called at the start of each timestep, before the
     *          hydraulic and quality solvers update the state.
     */
    void save_state() noexcept {
        nodes.save_state();
        links.save_state();
        // NOTE: subcatches.save_state() is intentionally NOT called here.
        // Subcatchment old-state (old_runoff/old_runon/conc_old) is the
        // runoff-step snapshot used to linearly interpolate lateral inflow
        // between runoff evaluations (legacy subcatch_setOldState, called
        // ONLY inside runoff_execute — i.e. per WET/DRY step, not per routing
        // step). Saving it here, every routing step, clobbered old_runoff with
        // the current value so old==new and the lateral inflow jumped to the
        // full new runoff instantly instead of ramping. It is now saved in the
        // runoff-advance loop in SWMMEngine::stepRunoff().
    }

    /**
     * @brief Reset all state variables to initial conditions (cold start).
     *
     * @details Sets all current and old-step arrays to zero. Invert elevations
     *          and static properties are NOT changed.
     */
    void reset_state() noexcept {
        nodes.reset_state();
        links.reset_state();
        subcatches.reset_state();
        gages.reset_state();
        tables.reset_cursors();
        current_time = 0.0;
        current_date = options.start_date;
        dt_controls_remaining = 0.0;
        elapsed_ms = 0.0;
        old_elapsed_ms = 0.0;
        // Legacy swmm5.c:721 — ReportTime = 1000 * ReportStep (ms from SIM start)
        next_report_ms = 1000.0 * options.report_step;
    }

    /**
     * @brief Allocate all object arrays after input parsing is complete.
     *
     * @details Called by InputReader after all [JUNCTIONS], [CONDUITS], etc.
     *          sections have been parsed and the final object counts are known.
     *          Resizes SoA arrays and pollutant quality matrices.
     *
     * @note Name indices must already be populated (resize() uses node_names.size(),
     *       etc. to determine array length).
     */
    void allocate_objects() {
        nodes.resize(node_names.size());
        links.resize(link_names.size());
        subcatches.resize(subcatch_names.size());
        gages.resize(gage_names.size());
        pollutants.resize_pollutants(pollutant_names.size());
        int np = static_cast<int>(pollutant_names.size());
        nodes.resize_quality(np);
        links.resize_quality(np);
        subcatches.resize_quality(np);

        // Resize spatial coordinate arrays
        spatial.node_x.assign(static_cast<std::size_t>(node_names.size()), 0.0);
        spatial.node_y.assign(static_cast<std::size_t>(node_names.size()), 0.0);
        spatial.link_x.assign(static_cast<std::size_t>(link_names.size()), 0.0);
        spatial.link_y.assign(static_cast<std::size_t>(link_names.size()), 0.0);
        spatial.subcatch_x.assign(static_cast<std::size_t>(subcatch_names.size()), 0.0);
        spatial.subcatch_y.assign(static_cast<std::size_t>(subcatch_names.size()), 0.0);

        // Resize link vertex and subcatchment polygon arrays
        spatial.link_vertices_x.resize(static_cast<std::size_t>(link_names.size()));
        spatial.link_vertices_y.resize(static_cast<std::size_t>(link_names.size()));
        spatial.subcatch_polygon_x.resize(static_cast<std::size_t>(subcatch_names.size()));
        spatial.subcatch_polygon_y.resize(static_cast<std::size_t>(subcatch_names.size()));

        // Resize gage coordinates
        spatial.gage_x.assign(static_cast<std::size_t>(gage_names.size()), 0.0);
        spatial.gage_y.assign(static_cast<std::size_t>(gage_names.size()), 0.0);
    }

    /**
     * @brief Release excess vector capacity on all owned SoA data stores.
     *
     * @details Called once after parsing and final sizing are complete.
     *          During incremental parsing, vectors grow via geometric capacity
     *          doubling. This method reclaims that excess by calling
     *          shrink_to_fit() on every data store.
     */
    void shrink_all_to_fit() {
        nodes.shrink_to_fit();
        links.shrink_to_fit();
        subcatches.shrink_to_fit();
        gages.shrink_to_fit();
        pollutants.shrink_to_fit();
        landuses.shrink_to_fit();
        buildup.shrink_to_fit();
        washoff.shrink_to_fit();
        treatment.shrink_to_fit();
        spatial.shrink_to_fit();
    }

    // =========================================================================
    // Convenience accessors
    // =========================================================================

    /** @brief Number of nodes. */
    int n_nodes()      const noexcept { return node_names.size(); }

    /** @brief Number of links. */
    int n_links()      const noexcept { return link_names.size(); }

    /** @brief Number of subcatchments. */
    int n_subcatches() const noexcept { return subcatch_names.size(); }

    /** @brief Number of rain gages. */
    int n_gages()      const noexcept { return gage_names.size(); }

    /** @brief Number of pollutants. */
    int n_pollutants() const noexcept { return pollutant_names.size(); }
    int n_landuses()   const noexcept { return landuse_names.size(); }

    /** @brief Number of tables (time series + curves). */
    int n_tables()     const noexcept { return static_cast<int>(tables.count()); }

    // =========================================================================
    // Kind-aware table lookups (case-insensitive, legacy hash.c parity).
    // Legacy keeps TSERIES and CURVE in separate hash tables, so the same
    // name may denote both a curve and a timeseries; every consumer knows
    // which kind it wants.
    // =========================================================================

    /** @brief Find a timeseries table by name; -1 if none. */
    int find_timeseries(std::string_view name) const noexcept {
        for (int i = 0; i < n_tables(); ++i)
            if (tables[i].type == TableType::TIMESERIES && ieq(tables[i].id, name))
                return i;
        return -1;
    }

    /** @brief Find a curve table (any CURVE_* type) by name; -1 if none. */
    int find_curve(std::string_view name) const noexcept {
        for (int i = 0; i < n_tables(); ++i)
            if (tables[i].type != TableType::TIMESERIES && ieq(tables[i].id, name))
                return i;
        return -1;
    }

    /**
     * @brief Find a table of either kind by name; -1 if none.
     * @details Timeseries win ties with a same-named curve (row order breaks
     *          exact ties). Only for callers with no kind context (e.g. the
     *          generic swmm_table_index C API).
     */
    int find_table_any(std::string_view name) const noexcept {
        const int ts = find_timeseries(name);
        if (ts >= 0) return ts;
        return find_curve(name);
    }
};

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_SIMULATION_CONTEXT_HPP */
