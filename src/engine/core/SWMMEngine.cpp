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
 * @file SWMMEngine.cpp
 * @brief Implementation of the SWMMEngine lifecycle manager.
 *
 * @see SWMMEngine.hpp
 * @ingroup engine_core
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "SWMMEngine.hpp"
#include "DateTime.hpp"
#include "SimulationContext.hpp"
#include "PerfTimers.hpp"
#include "UnitConversion.hpp"
#include "../hydrology/Gage.hpp"
#include "../hydraulics/Link.hpp"
#include "../hydraulics/XSectBatch.hpp"
#include "../hydraulics/Outfall.hpp"
#include "../hydraulics/Node.hpp"
#include "../hydraulics/ForceMain.hpp"
#include <cmath>
#include <algorithm>
#include <cassert>
#include <vector>
#include "../hydraulics/TimestepController.hpp"
#include "../input/PostParseResolver.hpp"
#include "../transport/components/HeatFluxModules/HeatOverrides.hpp"  // PE2
#include "../plugins/DefaultInputPlugin.hpp"
#include "../plugins/ProcessComponentRegistry.hpp"
#include "../transport/components/EulerianArdComponent/ArdConfig.hpp"
#include "../transport/components/ReactionModule/ReactionLegacyBinding.hpp"
#include "../transport/components/ReactionModule/ReactionsComponent.hpp"
#include "../transport/components/HeatModule/HeatComponent.hpp"
#include "../transport/components/WaterAgeModule/WaterAgeComponent.hpp"
#include "../transport/components/HeatModule/HeatLid.hpp"
#include "../transport/components/HeatModule/HeatWatershed.hpp"
#include "../transport/components/WaterAgeModule/WaterAgeWatershed.hpp"
#include "../transport/components/WaterAgeModule/WaterAgeLid.hpp"
#include "../plugins/DefaultStateIOPlugin.hpp"
#include "HotStartManager.hpp"
#include "../../../include/openswmm/plugin_sdk/IPluginComponentInfo.hpp"
#include "../plugins/DefaultOutputPlugin.hpp"
#include "../plugins/DefaultReportPlugin.hpp"

#ifdef OPENSWMM_HAS_2D
#include "../2d/input/SectionHandlers2D.hpp"
#include "../2d/output/Default2DOutputPlugin.hpp"
#endif
#include <filesystem>  // 2D mesh file + [PROCESS_COMPONENTS] path resolution

#include <cstring>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>       // call_once: process-global one-time init, see open()
#include "ErrorCodes.hpp"
#include "ThreadInfo.hpp"

// libomp's KMP_BLOCKTIME override — declared here at file scope because
// OpenMP support — graceful degradation when not available
#if defined(SWMM_USE_OPENMP)
#include <omp.h>
#else
static inline int omp_get_max_threads() { return 1; }
static inline void omp_set_num_threads(int) {}
#endif

// Error codes — these are returned across the C ABI and decoded by
// swmm_error_message(), so their VALUES must match the public
// SWMM_ErrorCode enum in openswmm_engine.h exactly.  They previously did
// not: WRONG_STATE was 3 (public RPTFILE) and PARSE was 4 (public OUTFILE),
// so a parse error surfaced to callers as "Cannot open output file" and a
// lifecycle error as "Cannot open report file".  Values corrected below to
// the public enum; the local names are kept to avoid churn at call sites.
static constexpr int SWMM_OK                 = 0;
static constexpr int SWMM_ERR_MEMORY         = 1;   // public SWMM_ERR_NOMEM
static constexpr int SWMM_ERR_FILE_NOT_FOUND = 2;   // public SWMM_ERR_INPFILE
static constexpr int SWMM_ERR_WRONG_STATE    = 6;   // public SWMM_ERR_LIFECYCLE
static constexpr int SWMM_ERR_PARSE          = 5;   // public SWMM_ERR_PARSE
static constexpr int SWMM_ERR_PLUGIN         = 10;  // public SWMM_ERR_PLUGIN
static constexpr int SWMM_ERR_IO             = 11;  // public SWMM_ERR_IO

namespace openswmm {

// A3 parity tracing: routing-step serial (updated by the RSTEP trace in
// stepRouting) so the per-link term trace in DynamicWave.cpp can be gated on
// a step number (SWMM_TRACE_LSTEP) instead of an invocation count — bypassed
// links make invocation counts hard to predict. Mirrors SwmmTraceRstepSn in
// the legacy engine (dwflow.c / routing.c).
long g_trace_rstep_sn = 0;

// ============================================================================
// Constructor / Destructor
// ============================================================================

SWMMEngine::SWMMEngine()
    : io_thread_(plugins_)   // IOThread needs a PluginFactory& at construction
{
    ctx_.state = EngineState::CREATED;
#ifdef OPENSWMM_HAS_2D
    wire2DModelIO();
#endif
}

#ifdef OPENSWMM_HAS_2D
void SWMMEngine::wire2DModelIO() noexcept {
    ctx_.twod_io.mesh       = &surface_router_.mesh();
    ctx_.twod_io.options    = &surface_router_.options();
    ctx_.twod_io.boundary   = &surface_router_.boundary();
    ctx_.twod_io.pending_bc = &surface_router_.pendingBCRows();
    ctx_.twod_io.pending_ec = &surface_router_.pendingEdgeConveyanceRows();
    ctx_.twod_io.pending_iq = &surface_router_.pendingInitialQualityRows();
    ctx_.twod_io.pending_bq = &surface_router_.pendingBoundaryQualityRows();
    ctx_.twod_io.infil      = &surface_router_.infil();
}
#endif

SWMMEngine::~SWMMEngine() {
    if (ctx_.state == EngineState::RUNNING ||
        ctx_.state == EngineState::ENDED) {
        close();
    }
}

// ============================================================================
// open()
// ============================================================================

int SWMMEngine::open(const char* inp_path,
                     const char* rpt_path,
                     const char* out_path,
                     const char* input_plugin_lib) noexcept {
    if (ctx_.state != EngineState::CREATED &&
        ctx_.state != EngineState::CLOSED) {
        set_error(SWMM_ERR_WRONG_STATE,
                  "swmm_engine_open: engine is not in CREATED or CLOSED state");
        return SWMM_ERR_WRONG_STATE;
    }

    // Reset context for a fresh run
    ctx_.reset();

    // Zero the load-phase accumulators so a process that opens several models
    // reports each one separately (see core/PerfTimers.hpp).
    perf::reset_load();
    perf::reset_fv();

    // Stamp the report wall clock before any parsing work. Legacy takes this
    // timestamp in report_writeLogo(), which swmm_open() calls before
    // project_readInput(), so its "Total elapsed time" covers parse +
    // validate + init. Stamping here keeps the reported elapsed time
    // comparable with legacy/PCSWMM instead of excluding the (potentially
    // very long, on large models) initialization window.
    std::time(&ctx_.wall_start);

    rpt_path_ = rpt_path ? rpt_path : "";
    out_path_ = out_path ? out_path : "";

    // Resolve input plugin: path, id:version, or fall back to default
    if (plugins_.input_plugins().empty()) {
        if (input_plugin_lib && input_plugin_lib[0] != '\0') {
            auto warn_cb = [this](const std::string& msg) {
                emit_warning(SWMM_ERR_PLUGIN, msg.c_str());
            };
            IPluginComponentInfo* info = plugins_.find_component(
                input_plugin_lib, warn_cb);
            if (info && info->has_input()) {
                IInputPlugin* ip = info->create_input_plugin();
                if (ip) {
                    ip->initialize({}, info);
                    plugins_.add_input_plugin(ip);
                }
            }
        }
        // Fall back to built-in .inp reader
        if (plugins_.input_plugins().empty()) {
            auto* ip = new DefaultInputPlugin();
            ip->initialize({}, nullptr);
            plugins_.add_input_plugin(ip);
        }
    }

    auto* input_plugin = plugins_.input_plugins().front();

#ifdef OPENSWMM_HAS_2D
    // Register 2D section handlers before reading so the parser can populate
    // surface_router_ mesh and options directly.
    if (auto* dip = dynamic_cast<DefaultInputPlugin*>(input_plugin)) {
        twoD::register2DSections(surface_router_.mesh(),
                                 surface_router_.options(),
                                 surface_router_.pendingBCRows(),
                                 surface_router_.pendingEdgeConveyanceRows(),
                                 surface_router_.pendingInitialQualityRows(),
                                 surface_router_.pendingBoundaryQualityRows(),
                                 dip->registry());
    }

    // Scan the inline .inp for `;; UNITS: SI (m)` so SurfaceRouter2D::initialize
    // can skip its FLOW_UNITS-based mesh scaling when the producer declared
    // the mesh is already SI. The external-mesh path runs its own prescan
    // below and overrides this if both files carry the header.
    if (inp_path && inp_path[0] != '\0') {
        perf::ScopedTimer _pt(perf::sec_open_prescan2d);
        twoD::prescan2DUnitsHeader(inp_path, surface_router_.options());
    }
#endif

    {
        perf::ScopedTimer _pt(perf::sec_open_read);
        if (input_plugin->read(inp_path ? inp_path : "", ctx_) != 0) {
            return ctx_.error_code != 0 ? ctx_.error_code : SWMM_ERR_PARSE;
        }
    }

#ifdef OPENSWMM_HAS_2D
    // If [2D_MESH_FILE] was present, load the external mesh file now.
    {
        const std::string& mf = surface_router_.options().mesh_file;
        if (!mf.empty()) {
            std::string base_dir;
            if (inp_path && inp_path[0] != '\0')
                base_dir = std::filesystem::path(inp_path).parent_path().string();
            std::string err = twoD::load2DMeshExternalFile(
                surface_router_.mesh(), surface_router_.options(),
                surface_router_.pendingBCRows(),
                surface_router_.pendingEdgeConveyanceRows(),
                &surface_router_.infil(),
                mf, base_dir, &ctx_.warnings,
                &surface_router_.pendingInitialQualityRows(),
                &surface_router_.pendingBoundaryQualityRows());
            if (!err.empty()) {
                if (lenient_open_) {
                    // A missing/unreadable external mesh must not make the
                    // whole model unopenable in an editor — discard any
                    // partially-read mesh state, record the diagnostic, and
                    // continue 1D-only. Running still uses a strict open.
                    surface_router_.mesh() = {};
                    surface_router_.pendingBCRows().clear();
                    surface_router_.pendingEdgeConveyanceRows().clear();
                    ctx_.errors.push_back(err);
                } else {
                    ctx_.error_code    = SWMM_ERR_PARSE;
                    ctx_.error_message = err;
                    ctx_.errors.push_back(err);
                    write_open_failure_report();
                    return SWMM_ERR_PARSE;
                }
            }
        }
    }
#endif

    // Species registry (master plan §4.1, phase T0a): pollutants occupy the
    // first slots, index-aligned with the legacy pollutant index. MSX
    // species (reactions component, below) and reserved species (age A1,
    // temperature H1) append after. Rebuilt on every open.
    ctx_.species_registry.clear();
    for (int p = 0; p < ctx_.n_pollutants(); ++p)
        ctx_.species_registry.add(ctx_.pollutant_names.name_of(p),
                                  SpeciesKind::POLLUTANT, std::string{});

    // Resolve [PROCESS_COMPONENTS] registrations (Unified Transport suite
    // D-UT8, phase IO1): look up each id, read its external config file
    // (relative to the .inp, [2D_MESH_FILE] path rules), and deliver the
    // parsed sections to the component's apply hook. Mirrors the external
    // 2D mesh handling directly above: fatal on strict open, recorded and
    // survivable on lenient (editor) open. Implemented components register
    // first (idempotent — overwrites the planned-id placeholder).
    //
    // ONCE per process, not once per open(). ProcessComponentRegistry is a
    // singleton whose header states it is not thread-safe for registration
    // ("register at startup only"), and register_component() move-assigns a
    // heap-allocated std::string description over the existing map node. Two
    // engines opening on different threads therefore both freed the same
    // string buffer — the `malloc(): unaligned tcache chunk detected` abort in
    // ConcurrentEngines.TwoInstancesDeterministic. Registration is idempotent
    // (same four ids, same data every time), so hoisting it to a one-time init
    // is behaviour-preserving for a single engine and removes the race.
    static std::once_flag process_components_registered;
    std::call_once(process_components_registered, [] {
        transport::registerReactionsComponent();
        transport::registerArdComponent();
        transport::registerWaterAgeComponent();
        transport::registerHeatComponent();
    });
    {
        std::string base_dir;
        if (inp_path && inp_path[0] != '\0')
            base_dir = std::filesystem::path(inp_path).parent_path().string();

        std::vector<std::string> errs;
        if (!ctx_.process_component_specs.empty())
            errs = components::resolve_process_components(ctx_, base_dir);

        // Embedded [REACTION_*] fallback (D-UT8): honored with a style
        // warning when no external reactions component is registered; the
        // external file wins wholesale otherwise.
        bool reactions_registered = false;
        for (const auto& spec : ctx_.process_component_specs)
            if (spec.id == "org.hydrocouple.openswmm.reactions")
                reactions_registered = true;
        transport::applyEmbeddedReactionSections(ctx_, reactions_registered,
                                                 errs);

        // E5a: [TRANSPORT_BOUNDARIES]/[TRANSPORT_SOURCES] rows reference MSX
        // species by NAME, and the reactions component may apply before OR
        // after transport.ard in file order — so the rows are stored raw at
        // apply and resolved HERE, after every component (and the embedded
        // fallback) has run. Resolution failures are fatal like any other
        // component config error.
        transport::resolveArdTransportRows(ctx_, errs);

        // PE2: per-element heat attributes resolve here for the SAME reason
        // — the rows name links, nodes and tags, and every component plus
        // the full .inp parse must be complete before a name can be
        // resolved or reported as unknown (D-RQ1 timing, D-PE5 fatality).
        {
            auto pe = transport::heat::resolveHeatOverrides(ctx_);
            for (auto& e : pe) errs.push_back(std::move(e));
        }

        if (!errs.empty()) {
            for (const auto& e : errs) ctx_.errors.push_back(e);
            if (!lenient_open_) {
                ctx_.error_code    = SWMM_ERR_PARSE;
                ctx_.error_message = errs.front();
                write_open_failure_report();
                return SWMM_ERR_PARSE;
            }
        }

        // A configured reactions component that no engine will run is a
        // silent no-result run unless we say so (R4).
        transport::warnIfLegacyBindingBypassed(ctx_);
        // The mirror case for E3: dispersion spelled the FV way while the
        // ARD engine reads it from model.ard.
        transport::warnIfFvDispersionKeyIgnored(ctx_);

        // X2: pollutant transport under LARD is live. X4: water age is
        // live too (segment age row + node stores). What still does not
        // run under this dispatch: temperature (H7), the reactions
        // component (deferred L3) and treatment expressions. Each
        // remaining bypass warns by name at open (silent no-result runs
        // are never allowed — the E1-era rule; enumerate silent-bypass
        // configurations, R4 lesson 5). All stay inside stepRouting's
        // !ignore_quality condition: under IGNORE_QUALITY the stage is
        // skipped before the solver choice is consulted, and that
        // configuration's own warning family covers it.
        // P2.3 (2026-09-01): treatment interop under LARD landed — the
        // legacy evaluator runs on the published node concentrations after
        // lard_.step, which ARE the LARD node stores (nodes.conc), so no
        // absorb step exists the way ARD needs one. The LAST LARD bypass
        // warning is GONE, not narrowed; this engine now runs every
        // configured quality feature or refuses at parse.
        // H6b bed bindings (2026-09-01): ALL THREE engines now carry the
        // bed — LEGACY on links, ARD per cell (1:1 slices, the reference's
        // own element mapping), LARD per link against the segment mean with
        // uniform increments. The "binds to LEGACY only" warning that stood
        // here is gone WITH its condition, not narrowed.

        // H6b: the ground temperature is usually the LARGEST term acting on
        // a buried pipe, and it has a default. An unstated one is therefore
        // warned, not refused — the `SolarConfig::has_timezone` precedent,
        // not the `has_latitude` one.
        if (ctx_.heat_config.sediment_exchange &&
            !ctx_.heat_config.sediment.has_ground_temp) {
            ctx_.warnings.push_back(
                "[SEDIMENT_EXCHANGE] GROUND_TEMPERATURE is not set; the "
                "default 12 degC is in force. For a buried conduit this term "
                "is often the largest one in the energy balance, so the "
                "default is unlikely to be what you meant.");
        }

        // X3a: the LARD stepping keys are consumed by no other engine —
        // the inverse bypass direction (E3 lesson 10: config spelled for
        // one engine while another runs).
        if (ctx_.options.quality_solver != QualitySolverKind::LAGRANGIAN &&
            !ctx_.options.ignore_quality) {
            if (ctx_.options.quality_step > 0.0)
                ctx_.warnings.push_back(
                    "[OPTIONS] QUALITY_STEP is set but QUALITY_SOLVER is "
                    "not LAGRANGIAN — only the LARD engine substeps on "
                    "this key; it has no effect this simulation.");
            if (ctx_.options.max_segments_per_link != 100)
                ctx_.warnings.push_back(
                    "[OPTIONS] MAX_SEGMENTS_PER_LINK is set but "
                    "QUALITY_SOLVER is not LAGRANGIAN — only the LARD "
                    "engine holds segments; it has no effect this "
                    "simulation.");
            if (ctx_.options.lard_rwpt)
                ctx_.warnings.push_back(
                    "[OPTIONS] DISPERSION RWPT is set but QUALITY_SOLVER "
                    "is not LAGRANGIAN — RWPT is the LARD engine's "
                    "dispersion; the ARD engine reads dispersion from the "
                    "transport.ard component instead. No dispersion is "
                    "applied from this key this simulation.");
        }
        // D-NS1 (X6): a configured negative quality inflow is EXTRACTION —
        // legal, warned per row at open (subplan §3.1 rule 2). Runtime
        // clamps + the end-of-run summary are the other two warnings of
        // the contract. Only the configured BASELINE is scanned here; a
        // timeseries that goes negative at runtime is caught by the clamp
        // warning instead.
        for (int i = 0; i < ctx_.ext_inflows.count(); ++i) {
            const auto ui = static_cast<std::size_t>(i);
            if (ctx_.ext_inflows.inflow_type[ui] == "FLOW") continue;
            if (ctx_.ext_inflows.baseline[ui] >= 0.0) continue;
            ctx_.warnings.push_back(
                "[INFLOWS] node '" + ctx_.ext_inflows.node_name[ui] +
                "' pollutant '" + ctx_.ext_inflows.constituent[ui] +
                "': the baseline is negative — this row EXTRACTS mass "
                "(D-NS1). Extraction clamps per step to the mass the "
                "element holds.");
        }

        // A1a/A1b: the reserved species registers so downstream consumers
        // see the registry truth. Age now tracks under BOTH engines (ARD
        // mesh row / LEGACY CSTR mirror) — the A1b bypass warning is
        // retired; IGNORE_QUALITY remains the only bypass and warns via
        // the waterage component.
        // A2b: the REPORTED species list — pollutants, then the age
        // pseudo-column. Built once here so the snapshot's pollut_names
        // pointer stays valid for the whole run.
        ctx_.reported_species_names.clear();
        for (int p = 0; p < ctx_.n_pollutants(); ++p)
            ctx_.reported_species_names.push_back(
                ctx_.pollutant_names.name_of(p));

        if (ctx_.options.water_age) {
            ctx_.species_registry.add("__WATER_AGE__",
                                      SpeciesKind::RESERVED_AGE, "hours");
            ctx_.reported_species_names.push_back("__WATER_AGE__");
            if (ctx_.options.ignore_quality)
                ctx_.warnings.push_back(
                    "[OPTIONS] WATER_AGE ON but IGNORE_QUALITY is YES — the "
                    "quality stage does not run, so no age is tracked this "
                    "simulation.");
        }

        // H1: temperature is the LAST reported column — after age — so a
        // deck that adds heat to an existing water-age model does not move
        // the age column a consumer already keys on by index. The NAME is
        // the contract either way (lesson 40), which is what
        // swmm_output_get_pollut_id exists to expose.
        if (ctx_.options.heat_transport) {
            ctx_.species_registry.add("__TEMPERATURE__",
                                      SpeciesKind::RESERVED_TEMPERATURE,
                                      "degC");
            ctx_.reported_species_names.push_back("__TEMPERATURE__");
            if (ctx_.options.ignore_quality)
                ctx_.warnings.push_back(
                    "[OPTIONS] HEAT_TRANSPORT ON but IGNORE_QUALITY is YES — "
                    "the quality stage does not run, so no temperature is "
                    "tracked this simulation.");
            // H4 retired H1's "ARD tracks no temperature" warning: the
            // reserved row now rides the mesh under EULERIAN_ARD, with
            // advection, dispersion, node mixing and per-cell surface
            // fluxes. Retiring a deferral means flipping its gate in the
            // same changeset (lesson 21) — see
            // test_heat_transport.cpp's ARD gate, now inverted.
        }
    }

    // Warn about unknown/skipped sections. Route through push_report_warning so
    // the warning reaches the .rpt (legacy report_writeWarningMsg), not just the
    // API callback. Wording matches legacy input.c ("Unknown section '[X]' ...");
    // the "at line N" locus is omitted (source line numbers are not retained by
    // the parser — see plan Phase 6).
    for (const auto& tag : input_plugin->skipped_sections()) {
        push_report_warning(
            "WARNING: Unknown section '[" + tag + "]' will be skipped.", 100);
    }

    // Resolve cross-references (forward refs, final array sizing, head init)
    {
        perf::ScopedTimer _pt(perf::sec_open_resolve);
        input::resolve_cross_references(ctx_);
    }

    // Project-level sanity checks + step-clamp warnings (legacy project_validate:
    // WARNING 01/06/07). Must run before the fatal gate below so any warnings it
    // records reach the report.
    {
        perf::ScopedTimer _pt(perf::sec_open_validate);
        validate_project();
    }

    // Post-parse validation errors accumulated during resolution (e.g.
    // ERR_TRANSECT_MANNING 227 for a zero channel Manning's n) are fatal:
    // surface the first one and fail the open. Without this check the
    // errors were silently swallowed and the model opened "successfully"
    // with broken derived state.
    if (!ctx_.errors.empty()) {
        // Write the accumulated errors/warnings to the report file, matching
        // legacy where a failed open still produces a .rpt containing the error
        // (report_writeErrorMsg). Without this the diagnostics would only reach
        // stderr and the .rpt would be absent.
        write_open_failure_report();
        if (!lenient_open_) {
            set_error(SWMM_ERR_PARSE, ctx_.errors.front().c_str());
            return SWMM_ERR_PARSE;
        }
        // Lenient open: keep the errors in ctx_.errors (queryable by the caller)
        // but do NOT fail — fall through so the engine reaches OPENED with all
        // parsed objects intact and editable. An editor/GUI can then load as
        // much of the model as parsed and surface the errors as diagnostics.
        // Running the model still requires a fresh, strict open.
    }

    // Phase 4: load plugins listed in [PLUGINS]
    if (!ctx_.plugin_specs.empty()) {
        plugins_.load_plugins(ctx_.plugin_specs, [this](const std::string& msg) {
            emit_warning(SWMM_ERR_PLUGIN, msg.c_str());
        });
    }

    // Inject built-in output/report plugins when paths are configured.
    // These are registered AFTER user-defined [PLUGINS] so that user plugins
    // have the opportunity to override default output.
    if (!out_path_.empty()) {
        auto* op = new DefaultOutputPlugin(out_path_);
        op->initialize({}, nullptr);
        op->validate(ctx_);
        plugins_.add_output_plugin(op);
    }
    if (!rpt_path_.empty()) {
        auto* rp = new DefaultReportPlugin(rpt_path_);
        rp->initialize({}, nullptr);
        rp->validate(ctx_);
        plugins_.add_report_plugin(rp);
    }

#ifdef OPENSWMM_HAS_2D
    // Inject built-in 2D HDF5 output plugin when [2D_OPTIONS] OUTPUT_FILE is set.
    // Mesh prep is deferred to start() because the mesh topology is not built
    // until SurfaceRouter2D::initialize() runs (called from SWMMEngine::initialize).
    {
        const std::string& of = surface_router_.options().output_file;
        if (!of.empty()) {
            std::string resolved = of;
            std::filesystem::path p(of);
            if (p.is_relative() && inp_path && inp_path[0] != '\0') {
                resolved = (std::filesystem::path(inp_path).parent_path() / p).string();
            }
            auto* op = new twoD::Default2DOutputPlugin(resolved);
            op->initialize({}, nullptr);
            op->validate(ctx_);
            plugins_.add_output_plugin(op);
            surface_output_plugin_ = op;
        }
    }
#endif

    // Wire solver-neutral state accessors so state-IO plugins can read/write
    // infiltration and groundwater state through SimulationContext alone.
    {
        runoff::RunoffSolver* runoff_ptr = &runoff_;
        groundwater::GWSolver* gw_ptr    = &groundwater_;

        ctx_.state_accessors.get_infil_state =
            [runoff_ptr](int i, int& model, double* infil) -> bool {
                runoff_ptr->infil_get_state(i, model, infil);
                return true;
            };
        ctx_.state_accessors.set_infil_state =
            [runoff_ptr](int i, int model, const double* infil) -> bool {
                runoff_ptr->infil_set_state(i, model, infil);
                return true;
            };
        ctx_.state_accessors.get_gw_state =
            [gw_ptr](int i, double& theta, double& lower_depth) -> bool {
                const auto& gwa = gw_ptr->state();
                const auto ui = static_cast<std::size_t>(i);
                if (ui >= gwa.theta.size()) return false;
                theta       = gwa.theta[ui];
                lower_depth = ui < gwa.lower_depth.size() ? gwa.lower_depth[ui] : 0.0;
                return true;
            };
        ctx_.state_accessors.set_gw_state =
            [gw_ptr](int i, double theta, double lower_depth) -> bool {
                auto& gwa = gw_ptr->state();
                const auto ui = static_cast<std::size_t>(i);
                if (ui >= gwa.theta.size()) return false;
                gwa.theta[ui] = theta;
                if (ui < gwa.lower_depth.size()) gwa.lower_depth[ui] = lower_depth;
                return true;
            };
    }

    // Inject the default state-IO plugin if no external one resolved during
    // [PLUGINS] processing. Coexist-with-fallback: external state plugins
    // win on read via can_read() in the C-API dispatch; the default handles
    // legacy formats and any unclaimed paths.
    {
        bool has_default_state_io = false;
        for (auto* sp : plugins_.state_io_plugins()) {
            if (dynamic_cast<DefaultStateIOPlugin*>(sp)) {
                has_default_state_io = true;
                break;
            }
        }
        if (!has_default_state_io) {
            auto* sp = new DefaultStateIOPlugin();
            sp->initialize({}, nullptr);
            sp->validate(ctx_);
            plugins_.add_state_io_plugin(sp);
        }
    }

    ctx_.state = EngineState::OPENED;
    return SWMM_OK;
}

// ============================================================================
// initialize()
// ============================================================================

int SWMMEngine::initialize() noexcept {
    // INITIALIZED is accepted so a caller can re-initialize before start()
    // (e.g. after adding control rules through the C API); every init_* step
    // rebuilds from ctx_ rather than appending, and ControlEngine::clearRules
    // guards the one known stacking hazard. Re-init after start()/end() still
    // requires a fresh open().
    if (ctx_.state != EngineState::OPENED &&
        ctx_.state != EngineState::INITIALIZED) {
        set_error(SWMM_ERR_WRONG_STATE,
                  "swmm_engine_initialize: must call open() first");
        return SWMM_ERR_WRONG_STATE;
    }

    // Everything from here to init_modules() is the "state seeding" phase —
    // per-node/per-link loops over ctx_. Closed out just before init_modules(),
    // which is broken into its own four legs.
    const auto _pt_state0 = perf::now();

    // Apply initial depths/flows from input (all defaults already in NodeData etc.)
    // reset_state() applies init_depth to depth/old_depth/head but volumes need
    // separate computation using node geometry tables.
    ctx_.reset_state();

    // MIN_SURFAREA is a project OPTION, and the junction storage convention was
    // not reading it. Legacy keeps no junction storage at all (node_getVolume
    // returns fullVolume*(d/fd), and fullVolume is 0 for a plain junction);
    // this engine books it deliberately (plan §7B.6) at MIN_SURFAREA*fullDepth,
    // but took the 12.566 ft² COMPILE-TIME constant rather than the option. So
    // a deck asking for a smaller manhole got it honoured in the dynamic wave's
    // surface-area floor (DynamicWave.cpp:391) and nowhere else — and the FV
    // solver, whose node area IS this volume divided by full depth, could not
    // see the option at all: MIN_SURFAREA 0.01 and 12.566 produced byte-
    // identical FV output. On the SWASHES 1D chains, where the nodes are an
    // artifact of discretizing a continuous channel and the decks ask for 0.01,
    // that is 1257x the intended storage at every node.
    //
    // Setting full_volume here means node::getVolume takes its fullVolume > 0
    // branch everywhere, so the mass balance, the dynamic wave and the FV mesh
    // all read ONE number. Default is unchanged: min_surf_area defaults to 0,
    // meaning "use the constant".
    {
        const double ucf_len = ucf::Ucf[ucf::LENGTH][
            ucf::getUnitSystem(static_cast<int>(ctx_.options.flow_units))];
        const double min_sa = (ctx_.options.min_surf_area > 0.0)
            ? ctx_.options.min_surf_area / (ucf_len * ucf_len)
            : constants::MIN_SURFAREA;
        for (int i = 0; i < ctx_.n_nodes(); ++i) {
            const auto ui = static_cast<std::size_t>(i);
            if (ctx_.nodes.type[ui] == NodeType::STORAGE) continue;
            const double fd = ctx_.nodes.full_depth[ui];
            if (fd > 0.0 && !(ctx_.nodes.full_volume[ui] > 0.0))
                ctx_.nodes.full_volume[ui] = min_sa * fd;
        }
    }

    // Compute initial volumes from init_depth (matching legacy node_initState)
    for (int i = 0; i < ctx_.n_nodes(); ++i) {
        auto ui = static_cast<std::size_t>(i);
        double d = ctx_.nodes.init_depth[ui];
        if (d > 0.0) {
            double vol = node::getVolume(ctx_.nodes, i, d, &ctx_.tables,
                ucf::getUnitSystem(static_cast<int>(ctx_.options.flow_units)),
                &ctx_.node_subtypes);
            ctx_.nodes.volume[ui] = vol;
            ctx_.nodes.old_volume[ui] = vol;
        }
        // Compute full volume for surcharge detection
        double fd = ctx_.nodes.full_depth[ui];
        if (fd > 0.0) {
            ctx_.nodes.full_volume[ui] = node::getVolume(ctx_.nodes, i, fd, &ctx_.tables,
                ucf::getUnitSystem(static_cast<int>(ctx_.options.flow_units)),
                &ctx_.node_subtypes);
        }
    }

    // Legacy-convention reported full volume (see report_full_volume_ doc): 0 for
    // plain junctions/outfalls/dividers, pump wet-well xMax set in the loop below.
    // Used ONLY to map the reported .out NODE_VOLUME to legacy; the internal
    // volume-state (ctx_.nodes.volume / full_volume) is untouched.
    report_full_volume_.assign(static_cast<std::size_t>(ctx_.n_nodes()), 0.0);

    // Type-1 (volume-controlled) pumps: the inlet junction acts as a wet well
    // whose full volume is the pump curve's maximum volume. Legacy pump_validate
    // (link.c) overrides the inlet node's fullVolume with the curve's xMax; the
    // refactored engine otherwise leaves it at MIN_SURFAREA*fullDepth, which
    // undersizes the volume->flow lookup and makes the pump run on a lower curve
    // segment (e.g. extran6 pump 90011 ran at 10 cfs instead of 20). Run AFTER
    // the full_volume loop above so the max() override survives.
    {
        const int us = ucf::getUnitSystem(static_cast<int>(ctx_.options.flow_units));
        const double ucf_vol = ucf::Ucf[ucf::VOLUME][us];
        for (int j = 0; j < ctx_.n_links(); ++j) {
            auto uj = static_cast<std::size_t>(j);
            if (ctx_.links.type[uj] != LinkType::PUMP) continue;
            const int pr = ctx_.link_subtypes.pump_row(j);
            int ci = (pr >= 0) ? ctx_.link_subtypes.pumps.curve[static_cast<std::size_t>(pr)] : -1;
            if (ci < 0 || ci >= static_cast<int>(ctx_.tables.tables.size())) continue;
            const auto& tbl = ctx_.tables.tables[static_cast<std::size_t>(ci)];
            if (tbl.type != TableType::CURVE_PUMP1) continue;
            int n1 = ctx_.links.node1[uj];
            if (n1 < 0) continue;
            auto un1 = static_cast<std::size_t>(n1);
            if (ctx_.nodes.type[un1] == NodeType::STORAGE) continue;
            // xMax = largest volume on the pump curve (legacy Pump.xMax). The
            // file-backed x_max field is unset for inline [CURVES], so read the
            // in-memory x points directly.
            double xmax = tbl.x_max;
            if (xmax <= 0.0 && !tbl.x.empty())
                xmax = *std::max_element(tbl.x.begin(), tbl.x.end());
            double xmax_internal = xmax / ucf_vol;
            ctx_.nodes.full_volume[un1] =
                std::max(ctx_.nodes.full_volume[un1], xmax_internal);
            // Legacy reports the pump wet-well's volume from this xMax.
            report_full_volume_[un1] =
                std::max(report_full_volume_[un1], xmax_internal);
            // The initial volume/old_volume loop above ran BEFORE this xMax
            // override, so it sized the wet well with the MIN_SURFAREA fallback
            // (full_volume was still 0). Legacy sets fullVolume in pump_validate
            // BEFORE node_initState computes oldVolume, so recompute the inlet's
            // initial volume here with the corrected full_volume — otherwise the
            // Type-1 pump's getMaxOutflow cap (inflow + oldVolume/dt) uses an
            // undersized oldVolume and under-pumps at startup (extran6: cap 1.34
            // vs legacy 3.0), seeding a wet-well surcharge instability.
            double d0 = ctx_.nodes.init_depth[un1];
            if (d0 > 0.0) {
                double v0 = node::getVolume(ctx_.nodes, n1, d0, &ctx_.tables,
                                            us, &ctx_.node_subtypes);
                ctx_.nodes.volume[un1] = v0;
                ctx_.nodes.old_volume[un1] = v0;
            }
        }
    }

    // Apply q0 to link flow and compute initial conduit depth (Gap #43)
    // Cache the translated batch cross-section code for every conduit BEFORE
    // the initial-state loops below. buildXSectParams()/getAofY() dispatch on
    // links.xsect_batch_shape, which is otherwise not populated until
    // routing_init() inside init_modules() — i.e. AFTER these loops. Without
    // this, getAofY() returned 0 (DUMMY shape) here, so the q0 (fix-#6) and
    // backwater (fix-#10) initial-volume calculations silently produced zero
    // storage (e.g. extran2's fixed-outfall backwater, extran8a's q0 conduits),
    // re-creating that water during step 1 as a false continuity error.
    for (int j = 0; j < ctx_.n_links(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (ctx_.links.type[uj] != LinkType::CONDUIT) continue;
        ctx_.links.xsect_batch_shape[uj] =
            link::translateShape(ctx_.links.xsect_shape[uj]);
    }

    // Matches legacy link_initState / conduit_initState in link.c:
    //   Link[j].oldFlow = Link[j].newFlow = q0
    //   conduit: newDepth = oldDepth = link_getYnorm(j, q0/barrels)
    for (int j = 0; j < ctx_.n_links(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        double q0 = ctx_.links.q0[uj];
        ctx_.links.flow[uj]     = q0;
        ctx_.links.old_flow[uj] = q0;
        if (ctx_.links.type[uj] == LinkType::CONDUIT && q0 != 0.0) {
            const int cr = ctx_.link_subtypes.conduit_row(j);
            const auto& CD = ctx_.link_subtypes.conduits;
            XSectParams xs = link::buildXSectParams(ctx_.links, uj, &ctx_.transect_tables);
            int barrels = (cr >= 0) ? CD.barrels[static_cast<std::size_t>(cr)] : 1;
            double q_per_barrel = std::fabs(q0) / std::max(barrels, 1);
            double beta = (cr >= 0) ? CD.beta[static_cast<std::size_t>(cr)] : 0.0;
            double y = link::getDepthFromFlow(xs, beta, q_per_barrel);
            ctx_.links.depth[uj]     = y;
            ctx_.links.old_depth[uj] = y;
            // Initial conduit storage volume = area(y) * length * barrels,
            // matching legacy flowrout.c (Link.newVolume = Conduit.a1 * length
            // * barrels). Without this the routing mass-balance "Initial Stored
            // Volume" omitted conduits that start with flow (e.g. extran8a
            // q0=20), producing a large false continuity error (-22%).
            double len = (cr >= 0) ? CD.length[static_cast<std::size_t>(cr)] : 0.0;
            double vol = xsect::getAofY(xs, y) * len * barrels;
            ctx_.links.volume[uj]     = vol;
            ctx_.links.old_volume[uj] = vol;
        }
    }

    // initNodeDepths (legacy flowrout.c): seed each non-storage / non-outfall
    // junction's initial depth from the AVERAGE of its connecting links' flow
    // depths (y = link.depth + offset1), so a node on a q0 conduit inherits that
    // conduit's normal depth. Without this the junction starts dry (at invert)
    // while the conduit starts with q0 flow, so there is zero head gradient and
    // the q0 flow COLLAPSES in the first routing step — extran8a's chain
    // 10081/10082 dropped 20→0 cfs and the inflow backed up and flooded
    // (continuity +23.8%). User-supplied initial depths and storage / outfall
    // boundary depths are preserved (set below). Must run AFTER the q0 loop
    // (which sets link depths) and BEFORE the q0=0 backwater fill (which reads
    // node depths).
    {
        const int us = ucf::getUnitSystem(static_cast<int>(ctx_.options.flow_units));
        const int nn = ctx_.n_nodes();
        std::vector<double> acc(static_cast<std::size_t>(nn), 0.0);
        std::vector<int>    cnt(static_cast<std::size_t>(nn), 0);
        for (int j = 0; j < ctx_.n_links(); ++j) {
            auto uj = static_cast<std::size_t>(j);
            double ld = ctx_.links.depth[uj];
            double y  = (ld > constants::FUDGE) ? ld + ctx_.links.offset1[uj] : 0.0;
            int n1 = ctx_.links.node1[uj];
            int n2 = ctx_.links.node2[uj];
            if (n1 >= 0) { acc[static_cast<std::size_t>(n1)] += y; ++cnt[static_cast<std::size_t>(n1)]; }
            if (n2 >= 0) { acc[static_cast<std::size_t>(n2)] += y; ++cnt[static_cast<std::size_t>(n2)]; }
        }
        for (int i = 0; i < nn; ++i) {
            auto ui = static_cast<std::size_t>(i);
            if (ctx_.nodes.type[ui] == NodeType::OUTFALL) continue;
            if (ctx_.nodes.type[ui] == NodeType::STORAGE) continue;
            if (ctx_.nodes.init_depth[ui] > 0.0) continue;   // user-supplied depth
            if (cnt[ui] <= 0) continue;
            double y = acc[ui] / static_cast<double>(cnt[ui]);
            if (y <= 0.0) continue;
            ctx_.nodes.depth[ui]     = y;
            ctx_.nodes.old_depth[ui] = y;
            ctx_.nodes.head[ui]      = ctx_.nodes.invert_elev[ui] + y;
            double vol = node::getVolume(ctx_.nodes, i, y, &ctx_.tables, us);
            ctx_.nodes.volume[ui]     = vol;
            ctx_.nodes.old_volume[ui] = vol;
        }
    }

    // Set outfall-node boundary depths (legacy link_setOutfallDepth), then
    // backfill each remaining conduit's initial depth to the average of its
    // end-node depths (legacy initLinkDepths in flowrout.c). Conduits sitting
    // below a fixed/tidal outfall stage thereby start with their standing
    // backwater counted as INITIAL stored volume, instead of that water being
    // created during the first routing step and showing up as a continuity
    // error (e.g. extran2's fixed 94.4 ft outfall backs ~1.57 ac-ft into the
    // downstream trapezoidal channels).
    // Set FIXED-outfall node depths from their stage (= stage − invert), so the
    // downstream conduit backwater can be computed below. Done inline rather
    // than via outfall::setAllOutfallDepths because that routine's
    // outfall→conduit cache is not populated until init_modules (and it gives
    // FREE/NORMAL outfalls zero depth at zero initial flow anyway).
    for (int oi = 0; oi < ctx_.n_nodes(); ++oi) {
        auto uo = static_cast<std::size_t>(oi);
        if (ctx_.nodes.type[uo] != NodeType::OUTFALL) continue;
        const int r = ctx_.node_subtypes.outfall_row(oi);
        if (r >= 0 &&
            ctx_.node_subtypes.outfalls.bc_type[static_cast<std::size_t>(r)] == OutfallType::FIXED) {
            double stage = ctx_.node_subtypes.outfalls.param[static_cast<std::size_t>(r)];  // internal ft
            ctx_.nodes.depth[uo] =
                std::max(0.0, stage - ctx_.nodes.invert_elev[uo]);
        }
    }
    for (int j = 0; j < ctx_.n_links(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (ctx_.links.type[uj] != LinkType::CONDUIT) continue;
        if (ctx_.links.q0[uj] != 0.0) continue;  // q0 conduits already at normal depth
        double yfull = ctx_.links.xsect_y_full[uj];
        int n1 = ctx_.links.node1[uj];
        int n2 = ctx_.links.node2[uj];
        if (n1 < 0 || n2 < 0) continue;
        double y1 = std::clamp(ctx_.nodes.depth[static_cast<std::size_t>(n1)]
                               - ctx_.links.offset1[uj], 0.0, yfull);
        double y2 = std::clamp(ctx_.nodes.depth[static_cast<std::size_t>(n2)]
                               - ctx_.links.offset2[uj], 0.0, yfull);
        double y = std::max(0.5 * (y1 + y2), constants::FUDGE);
        ctx_.links.depth[uj]     = y;
        ctx_.links.old_depth[uj] = y;
        XSectParams xs = link::buildXSectParams(ctx_.links, uj,
                                                &ctx_.transect_tables);
        const int cr = ctx_.link_subtypes.conduit_row(j);
        const auto& CD = ctx_.link_subtypes.conduits;
        int barrels = std::max((cr >= 0) ? CD.barrels[static_cast<std::size_t>(cr)] : 1, 1);
        double len = (cr >= 0) ? CD.length[static_cast<std::size_t>(cr)] : 0.0;
        double vol = xsect::getAofY(xs, y) * len * barrels;
        ctx_.links.volume[uj]     = vol;
        ctx_.links.old_volume[uj] = vol;
    }

    // ── USE HOTSTART: load saved routing state from a hot-start file ──
    // Legacy SWMM applies the hotstart in routing_open(), OVERRIDING the q0 /
    // initLinkDepths initial state with the saved node depths and link flows.
    // The refactored CLI previously never consumed ctx.files.hotstart_use_path
    // at all, so a continuation run (extran8b USEing the file extran8a SAVEd)
    // started cold from q0 (Initial Stored Volume 0.993) instead of the
    // hot-started state legacy loads (1.396). Apply it here — AFTER the q0 /
    // backwater init loops (which it overrides) and BEFORE init_modules() so the
    // DW solver seeds area_mid_ from the hot-started link depths, and before the
    // old_net_inflow seeding below (which reads the hot-started link flows).
    bool hotstart_loaded = false;
    if (!ctx_.files.hotstart_use_path.empty()) {
        const std::string& hs_path =
            !ctx_.files.hotstart_use_path.absolute.empty()
                ? ctx_.files.hotstart_use_path.absolute
                : ctx_.files.hotstart_use_path.original;
        // Read the legacy EPA SWMM5 `.hsf` routing state (the format SAVE writes
        // and the de-facto interchange format). Native OPENSWMM_HS_V1 files are
        // applied via the C-API swmm_hotstart_apply path instead.
        const int rc = HotStartManager::apply_legacy_routing(
            hs_path, ctx_,
            [this](const std::string& m) { ctx_.warnings.push_back(m); });
        if (rc != 0) {
            set_error(CFFI_ERR_HOTSTART,
                      ("USE HOTSTART: " + HotStartManager::last_io_error()).c_str());
            return CFFI_ERR_HOTSTART;
        }
        hotstart_loaded = true;
        // Recompute derived state from the applied depths/flows so continuity,
        // the area_mid_ seeding in init_modules(), and the old_net_inflow seeding
        // below all start from the hot-started state (legacy initNodes / initLinks).
        const int us_hs = ucf::getUnitSystem(
            static_cast<int>(ctx_.options.flow_units));
        for (int i = 0; i < ctx_.n_nodes(); ++i) {
            auto ui = static_cast<std::size_t>(i);
            double y = ctx_.nodes.depth[ui];
            ctx_.nodes.old_depth[ui] = y;
            ctx_.nodes.head[ui]      = ctx_.nodes.invert_elev[ui] + y;
            double vol = node::getVolume(ctx_.nodes, i, y, &ctx_.tables, us_hs,
                                         &ctx_.node_subtypes);
            ctx_.nodes.volume[ui]     = vol;
            ctx_.nodes.old_volume[ui] = vol;
        }
        for (int j = 0; j < ctx_.n_links(); ++j) {
            auto uj = static_cast<std::size_t>(j);
            ctx_.links.old_flow[uj] = ctx_.links.flow[uj];
            if (ctx_.links.type[uj] == LinkType::CONDUIT) {
                double y = ctx_.links.depth[uj];
                ctx_.links.old_depth[uj] = y;
                XSectParams xs = link::buildXSectParams(ctx_.links, uj,
                                                        &ctx_.transect_tables);
                const int cr = ctx_.link_subtypes.conduit_row(j);
                const auto& CD = ctx_.link_subtypes.conduits;
                int barrels = std::max((cr >= 0) ? CD.barrels[static_cast<std::size_t>(cr)] : 1, 1);
                double len = (cr >= 0) ? CD.length[static_cast<std::size_t>(cr)] : 0.0;
                double vol = xsect::getAofY(xs, y) * len * barrels;
                ctx_.links.volume[uj]     = vol;
                ctx_.links.old_volume[uj] = vol;
            }
        }
    }

    // Initialize output timer + legacy-parity ms clocks
    ctx_.current_date        = ctx_.options.start_date;
    ctx_.current_time        = 0.0;
    ctx_.elapsed_ms          = 0.0;   // legacy NewRoutingTime (swmm5.c:720)
    ctx_.old_elapsed_ms      = 0.0;
    // Legacy swmm5.c:721 — ReportTime = 1000 * (double)ReportStep
    ctx_.next_report_ms      = 1000.0 * ctx_.options.report_step;
    new_rule_time_ms_        = 0.0;   // legacy NewRuleTime (routing.c:131)

    perf::sec_init_state += perf::since(_pt_state0);

    // Initialize all computational modules (batch SoA setup)
    init_modules();

    // A mesh the finite-volume solver cannot build is a fatal model error, not
    // a warning: there is no fallback routing, so continuing would produce a
    // plausible-looking report describing a network that never moved water.
    if (!ctx_.errors.empty()) {
        set_error(SWMM_ERR_PARSE, ctx_.errors.front().c_str());
        return SWMM_ERR_PARSE;
    }

    // Seed node inflow/outflow from the initial link flows so the FIRST
    // routing step's trapezoidal node-continuity term reads the correct
    // old_net_inflow. The per-step save_state() (called before the first
    // stepRouting) records old_net_inflow = inflow - outflow; legacy gets the
    // matching value because initNodes (flowrout.c:461-473) distributes each
    // link's initial flow to its end nodes and node_setOldHydState then stores
    // oldNetInflow = inflow - outflow. Without this a node draining a conduit
    // that starts with flow (q0 != 0, e.g. extran8a node 30081 with 20 cfs out
    // via conduit 10081) began step 0 with old_net_inflow = 0 instead of -20,
    // biasing dV = 0.5*(old_net_inflow + dQ)*dt and the first-step depth by
    // ~0.01 ft, seeding a slowly-decaying ~0.09 cfs startup transient. The DW
    // per-step initNodeStates zeroes inflow before re-accumulating, so this
    // seed is consumed only by the step-0 save_state and never double-counted.
    // Faithful port of legacy initNodes (flowrout.c:440-473): each node's
    // initial inflow is SEEDED with its lateral flow (line 443
    // `Node[i].inflow = Node[i].newLatFlow`) before the link flows are
    // distributed. For a cold start nodes.lat_flow is still 0 here (lateral
    // inflows are not assembled until stepping), so this term vanishes and the
    // result is identical to the link-only seeding. For a USE HOTSTART run,
    // apply_legacy_routing loaded the saved newLatFlow into nodes.lat_flow
    // (e.g. extran8b node 30081 = +20 cfs, balancing its q0=20 drain conduit
    // 10081), exactly as legacy readRouting sets Node[].newLatFlow before
    // initNodes folds it in. Omitting it left node 30081 at
    // old_net_inflow = (link in 0.022 − link out 19.99) = −19.97 instead of the
    // legacy (+20 + 0.022 − 19.99) = +0.029 — a 20-cfs step-0 seed error that
    // this fold removes so the trapezoidal dV = 0.5*(old_net_inflow + dQ)*dt
    // bit-matches legacy at step 0. (It does NOT close extran8b's remaining
    // ~0.5 cfs headline flowΔ: a fine-resolution diff shows the first
    // divergence is ~1.7e-4 on the 0.025-cfs flow in conduit 10006 at t=200s —
    // i.e. at the float32 floor of the SAVEd hot-start state — which then
    // amplifies through the stiff rising storm-wave, the same network seed-
    // amplification class as user2/user5/user3, not a local arithmetic bug.)
    (void)hotstart_loaded;
    for (int i = 0; i < ctx_.n_nodes(); ++i) {
        auto ui = static_cast<std::size_t>(i);
        ctx_.nodes.inflow[ui] += ctx_.nodes.lat_flow[ui];
    }
    for (int j = 0; j < ctx_.n_links(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        double q = ctx_.links.flow[uj];
        int n1 = ctx_.links.node1[uj];
        int n2 = ctx_.links.node2[uj];
        if (q >= 0.0) {
            if (n1 >= 0) ctx_.nodes.outflow[static_cast<std::size_t>(n1)] += q;
            if (n2 >= 0) ctx_.nodes.inflow[static_cast<std::size_t>(n2)]  += q;
        } else {
            if (n1 >= 0) ctx_.nodes.inflow[static_cast<std::size_t>(n1)]  -= q;
            if (n2 >= 0) ctx_.nodes.outflow[static_cast<std::size_t>(n2)] -= q;
        }
    }

    ctx_.state = EngineState::INITIALIZED;
    return SWMM_OK;
}

// ============================================================================
// start()
// ============================================================================

// ============================================================================
// F8 — snow cover volume, the ledger term the engine never had
// ============================================================================
//
// Legacy `snow_getSnowCover` (snow.c:587):
//
//     for (i = SNOW_PLOWABLE; i <= SNOW_PERV; i++)
//         snowCover += (wsnow[i] + fw[i]) * fArea[i];
//     return snowCover * (Subcatch[j].area - Subcatch[j].lidArea);
//
// Two details are load-bearing and easy to drop:
//   * `fw` is IN. The free-water store is water the pack is holding, not
//     water that has left it. Counting only `wsnow` understates the term by
//     up to `fwfrac` of every pack and would leave a residual that looks
//     like a leak -- which is exactly the shape of the residual F8's first
//     write-up chased and had to retract.
//   * LID area is OUT, matching the plow-removal volume (Gap #60) and legacy
//     Build 5.2.0. Water in a LID unit is already counted by
//     `lid_.storedVolume()` in init/final STORAGE; counting it here as well
//     would double it.
static double snowCoverVolumeFt3(const openswmm::SimulationContext& ctx,
                                 const openswmm::snow::SnowSoA& soa) {
    double total = 0.0;
    for (int i = 0; i < ctx.n_subcatches(); ++i) {
        auto ui = static_cast<std::size_t>(i);
        if (ctx.subcatches.snowpack[ui] < 0) continue;
        double depth = 0.0;
        for (int k = 0; k < openswmm::snow::N_SUBAREAS; ++k) {
            auto idx = static_cast<std::size_t>(i * openswmm::snow::N_SUBAREAS + k);
            if (idx >= soa.wsnow.size()) break;
            depth += (soa.wsnow[idx] + soa.fw[idx]) * soa.fArea[idx];
        }
        const double lid_ft2 = (ui < ctx.subcatches.total_lid_area_ft2.size())
                                   ? ctx.subcatches.total_lid_area_ft2[ui] : 0.0;
        // `subcatches.area` is in PROJECT LAND-AREA UNITS (acres in US,
        // hectares in SI); `total_lid_area_ft2` is in ft². Subtracting one
        // from the other without converting is the defect the comment at
        // the rainfall-volume site warns about — it was 2.471x wrong on SI
        // there, and would be here.
        double area_ft2 = ctx.subcatches.area[ui] /
                              ucf::UCF(ucf::LANDAREA, ctx.options) - lid_ft2;
        if (area_ft2 < 0.0) area_ft2 = 0.0;
        total += depth * area_ft2;
    }
    return total;
}

int SWMMEngine::start(int save_results) noexcept {
    if (ctx_.state != EngineState::INITIALIZED) {
        set_error(SWMM_ERR_WRONG_STATE,
                  "swmm_engine_start: must call initialize() first");
        return SWMM_ERR_WRONG_STATE;
    }

    save_results_ = save_results;

    // Legacy DoRouting (swmm5.c:748): DoRouting = (Nobjects[NODE] > 0 &&
    // !IgnoreRouting). Routing execution, statistics, mass balance, the
    // routing-step-size coarsening, and the outfall interface write are all
    // gated on this in step()/postOutputSnapshot().
    do_routing_ = (ctx_.n_nodes() > 0 && !ctx_.options.ignore_routing);

    // Everything up to prepare_all() is interface-file work ([FILES] inflows /
    // outflows / hotstart / RDII / rainfall).
    const auto _pt_iface0 = perf::now();

    // Open routing interface files ([FILES] USE INFLOWS / SAVE OUTFLOWS) and
    // process headers eagerly — matching legacy routing_open() →
    // iface_openRoutingFiles(). Without this the InterfaceManager stays
    // closed and the readInflows()/writeOutfallResults() calls in the step
    // loop silently no-op.
    if (!ctx_.files.inflows_path.empty() || !ctx_.files.outflows_path.empty()) {
        // Prefer the resolved absolute path (IO-3) and fall back to the
        // verbatim token — same policy as the USE HOTSTART open above.
        const std::string& in_path =
            !ctx_.files.inflows_path.absolute.empty()
                ? ctx_.files.inflows_path.absolute
                : ctx_.files.inflows_path.original;
        const std::string& out_path =
            !ctx_.files.outflows_path.absolute.empty()
                ? ctx_.files.outflows_path.absolute
                : ctx_.files.outflows_path.original;

        const int rc = iface_.openFiles(in_path, out_path);
        if (rc == -1) {
            ctx_.errors.push_back(format_error(ERR_ROUTING_IFACE_SAME, ""));
            set_error(SWMM_ERR_IO, "swmm_engine_start: inflows and outflows "
                                   "routing interface files have the same name");
            return SWMM_ERR_IO;
        }
        if (rc == -2) {
            ctx_.errors.push_back(format_error(ERR_ROUTING_IFACE, out_path));
            set_error(SWMM_ERR_IO, "swmm_engine_start: cannot open routing "
                                   "interface file for writing");
            return SWMM_ERR_IO;
        }
        if (rc == -3) {
            iface_.closeFiles();
            ctx_.errors.push_back(format_error(ERR_ROUTING_IFACE, in_path));
            set_error(SWMM_ERR_IO, "swmm_engine_start: cannot open routing "
                                   "interface file for reading");
            return SWMM_ERR_IO;
        }
        if (!in_path.empty() && iface_.readFileHeader(ctx_) != 0) {
            iface_.closeFiles();
            ctx_.errors.push_back(
                format_error(ERR_ROUTING_IFACE_FORMAT, in_path));
            set_error(SWMM_ERR_IO, "swmm_engine_start: invalid format for "
                                   "routing interface file");
            return SWMM_ERR_IO;
        }
        if (!out_path.empty()) {
            iface_.writeFileHeader(ctx_);
        }
    }

    // [FILES] SAVE/USE RUNOFF — auto-open the runoff interface file
    // (legacy runoff_open() → openRunoffFile()). If the C API already
    // opened one explicitly (swmm_runoff_iface_open_*), the slot defers.
    if (ctx_.files.runoff_mode != FileMode::NONE
        && !ctx_.files.runoff_path.empty()
        && !(runoff_iface_file_ && runoff_iface_file_->isOpen())) {
        const std::string& rp = !ctx_.files.runoff_path.absolute.empty()
            ? ctx_.files.runoff_path.absolute
            : ctx_.files.runoff_path.original;
        const int rc = (ctx_.files.runoff_mode == FileMode::SAVE)
            ? openRunoffIfaceWrite(rp)
            : openRunoffIfaceRead(rp);
        if (rc != 0) {
            ctx_.errors.push_back(format_error(
                (rc == -3) ? ERR_RUNOFF_IFACE_COMPAT : ERR_RUNOFF_IFACE, rp));
            set_error(SWMM_ERR_IO, "swmm_engine_start: cannot open runoff "
                                   "interface file");
            return SWMM_ERR_IO;
        }
    }

    // [FILES] USE/SAVE RDII — open the RDII interface file. USE bypasses
    // the internal unit-hydrograph computation entirely (legacy
    // rdii_openRdii() skips createRdiiFile() when Frdii.mode == USE_FILE).
    if (ctx_.files.rdii_mode != FileMode::NONE
        && !ctx_.files.rdii_path.empty()
        && !ctx_.options.ignore_rdii
        && !ctx_.options.ignore_rainfall) {   // legacy: rain_open() gates rdii_openRdii() (swmm5.c:735)
        const std::string& dp = !ctx_.files.rdii_path.absolute.empty()
            ? ctx_.files.rdii_path.absolute
            : ctx_.files.rdii_path.original;
        if (ctx_.files.rdii_mode == FileMode::USE) {
            const int rc = rdii_iface_file_.openForRead(ctx_, dp);
            if (rc != 0) {
                ctx_.errors.push_back(format_error(
                    (rc == -2) ? ERR_RDII_IFACE_FORMAT : ERR_RDII_IFACE, dp));
                set_error(SWMM_ERR_IO, "swmm_engine_start: cannot open RDII "
                                       "interface file");
                return SWMM_ERR_IO;
            }
        } else {   // SAVE
            const std::vector<int> rdii_nodes = rdii_.rdiiNodeList();
            const int rc = rdii_iface_file_.openForWrite(
                dp, static_cast<int>(ctx_.options.wet_step), rdii_nodes);
            if (rc == -1) {
                ctx_.errors.push_back(format_error(ERR_RDII_IFACE, dp));
                set_error(SWMM_ERR_IO, "swmm_engine_start: cannot open RDII "
                                       "interface file for writing");
                return SWMM_ERR_IO;
            }
            // rc == -2: model has no RDII inflows — nothing to save;
            // legacy leaves the slot inert in that case too.
        }
    }

    // [FILES] USE/SAVE RAINFALL — the collated binary rainfall interface
    // file is not implemented in the refactored engine (gage data files are
    // read directly). Warn instead of silently ignoring; see
    // plans/FILES_INTERFACE_GAP_CLOSURE_PLAN_2026-07-02.md Phase 3.
    if (ctx_.files.rainfall_mode != FileMode::NONE
        && !ctx_.files.rainfall_path.empty()) {
        const char* what = (ctx_.files.rainfall_mode == FileMode::USE)
                           ? "USE RAINFALL" : "SAVE RAINFALL";
        ctx_.warnings.push_back(
            format_warning(WARN_FILES_SLOT_UNSUPPORTED, what));
        emit_warning(SWMM_ERR_IO, "[FILES] RAINFALL interface file is not "
                                  "supported by this engine and was ignored");
    }

    perf::sec_start_iface += perf::since(_pt_iface0);

    // Phase 4: call prepare() on all plugins (opens output files/headers)
    if (!plugins_.empty()) {
        perf::ScopedTimer _pt(perf::sec_start_plugins);
        plugins_prepare_attempted_ = true;
        const int rc = plugins_.prepare_all(ctx_);
        if (rc != 0) {
            set_error(SWMM_ERR_PLUGIN, "swmm_engine_start: plugin prepare() failed");
            return SWMM_ERR_PLUGIN;
        }
    }

#ifdef OPENSWMM_HAS_2D
    // After plugin->prepare() created the HDF5 file (root attrs only), write
    // the static mesh topology and create the time-varying datasets. This is
    // a separate step because the IOutputPlugin contract has no mesh access
    // through SimulationContext — SurfaceRouter2D owns the mesh privately.
    if (surface_output_plugin_ && surface_router_.isActive()) {
        // The mesh coordinates about to be written were scaled to SI by
        // SurfaceRouter2D::initialize(); hand the plugin the factor so the
        // file's /crs variable can state how they relate to the model CRS's
        // own linear unit (issue #155). The CRS itself arrived via prepare().
        surface_output_plugin_->setMeshCoordinateScale(
            surface_router_.options().mesh_to_si_factor);
        surface_output_plugin_->prepareMeshAndDatasets(surface_router_.mesh());
    }
#endif

    // Phase 5: start the IO writer thread
    io_thread_.start();

    // Initialize averaging accumulators if rpt_averages is enabled
    if (ctx_.options.rpt_averages) {
        avg_.resize(ctx_.n_nodes(), ctx_.n_links());
    }

    ctx_.state = EngineState::RUNNING;
    return SWMM_OK;
}

// ============================================================================
// step()
// ============================================================================

int SWMMEngine::step(double* elapsed_time) noexcept {
    if (ctx_.state != EngineState::RUNNING) {
        if (elapsed_time) *elapsed_time = 0.0;
        set_error(SWMM_ERR_WRONG_STATE,
                  "swmm_engine_step: engine is not running");
        return SWMM_ERR_WRONG_STATE;
    }

    // Check if simulation is complete
    if (hydraulics::TimestepController::simulation_complete(ctx_)) {
        if (elapsed_time) *elapsed_time = 0.0;
        ctx_.state = EngineState::ENDED;
        return SWMM_OK;
    }

    // Compute next explicit timestep using CFL-based adaptive stepping
    double dt_next;
    if (!do_routing_) {
        // IGNORE_ROUTING (legacy DoRouting == false, swmm5.c:963): with routing
        // off there is no CFL constraint — the clock advances at
        // MIN(WetStep, ReportStep), bounded only by the remaining simulation
        // duration. The CFL/RouteStep clamp and control-rule alignment inside
        // TimestepController::compute_next must NOT apply here, so mirror only
        // its total-duration clamp (compute_next steps 2; TimestepController.cpp:54).
        double dt = std::min(ctx_.options.wet_step, ctx_.options.report_step);
        const double total_msec = ctx_.options.totalDurationMs();
        if (ctx_.elapsed_ms + 1000.0 * dt > total_msec) {
            dt = (total_msec - ctx_.elapsed_ms) / 1000.0;
            dt = std::max(dt, 0.001);  // legacy floor: 1 msec
        }
        dt_next = dt;
    } else {
        double dt_cfl = ctx_.options.routing_step;
        if (ctx_.options.variable_step > 0.0) {
            dt_cfl = router_.getAdaptiveStep(ctx_, ctx_.options.routing_step,
                                              ctx_.options.variable_step);
            // Track max Courant number: ratio of fixed step to CFL-limited step
            if (dt_cfl > 0.0 && dt_cfl < ctx_.options.routing_step) {
                double courant = ctx_.options.routing_step / dt_cfl;
                ctx_.routing_stats.max_courant =
                    std::max(ctx_.routing_stats.max_courant, courant);
            }
        }
        // Control-rule grid alignment (legacy routing.c:196-204): when
        // RULE_STEP > 0 the routing step is snapped so it lands exactly on
        // the next rule-evaluation boundary. Recomputed from the ABSOLUTE
        // rule grid each step (advance()'s decrement would drift); the
        // clamp itself lives in TimestepController::compute_next step 3.
        if (ctx_.options.rule_step > 0.0) {
            ctx_.dt_controls_remaining =
                (new_rule_time_ms_ + 1000.0 * ctx_.options.rule_step -
                 ctx_.elapsed_ms) / 1000.0;
        }
        // No 2D constraint on the 1D step: the marcher owns 2D stability
        // through its internal CFL subcycling, and exchange stability is owned
        // by per-substep evaluation + limiter + the node conductance.
        dt_next = hydraulics::TimestepController::compute_next(ctx_, dt_cfl);
    }

    // Fire step-begin callback
    emit_progress();
    if (callbacks_.on_step_begin) {
        callbacks_.on_step_begin(
            static_cast<void*>(this),
            ctx_.current_date,
            dt_next,
            callbacks_.step_begin_ud
        );
    }

    // Snapshot state before solving
    ctx_.save_state();

    // Reset mass balance accumulators (matching legacy massbal_initTimeStepTotals)
    resetStepMassBalance();

    // ---- Apply user-injected runtime forcings ----
    applyForcings(dt_next);

    // ---- Full simulation pipeline (matching legacy swmm_step order) ----
    // Reference: swmm5.c::execRouting() → runoff_execute() + routing_execute()

    stepRunoff(dt_next);
    // IGNORE_ROUTING (legacy `if (DoRouting) routing_execute()`, swmm5.c:997,
    // and massbal.c:299): skip the routing step and its statistics / routing
    // mass-balance accumulation. Runoff and its final-storage bookkeeping still
    // run every step.
    if (do_routing_) {
        stepRouting(dt_next);
        if (const int drc = checkRoutingDiverged(); drc != SWMM_OK)
            return drc;
        updateStatistics(dt_next);
        updateRoutingMassBalance(dt_next);
    }
    computeFinalStorage();
    // IGNORE_QUALITY: surface buildup was never updated this run, so skip the
    // final quality mass-balance pass (legacy skips the whole quality path).
    if (!ctx_.options.ignore_quality) {
        computeFinalQualityMassBalance();
    }

    // ---- Clear auto-reset forcings ----
    ctx_.forcing.clear_reset_entries();

    // Advance clock (must happen before output_due check)
    hydraulics::TimestepController::advance(ctx_, dt_next);

    // Advance the control-rule clock when the step landed on its grid
    // (legacy routing.c:407-408, 1 ms tolerance).
    if (do_routing_ && ctx_.options.rule_step > 0.0 &&
        std::fabs(ctx_.elapsed_ms -
                  (new_rule_time_ms_ + 1000.0 * ctx_.options.rule_step)) < 1.0) {
        new_rule_time_ms_ += 1000.0 * ctx_.options.rule_step;
    }

    // Accumulate node/link results for time-step averaging with legacy's
    // update ordering (swmm5.c saveResults): each step's END state
    // accumulates exactly once, into the report period whose window it
    // terminates in — a boundary hit (NewRoutingTime == ReportTime) goes to
    // the CLOSING period (update BEFORE the save), an overshoot (>) goes to
    // the NEXT period (update AFTER the save), and non-boundary steps always
    // accumulate. The == compare is exact only because elapsed_ms and
    // next_report_ms use the identical `+= 1000.0 * x` recurrences
    // (TimestepController::advance / reset_output_timer) — keep the three
    // sites in lockstep.
    const bool avg_out_due =
        ctx_.options.rpt_averages &&
        hydraulics::TimestepController::output_due(ctx_);
    const bool avg_exact =
        avg_out_due && (ctx_.elapsed_ms == ctx_.next_report_ms);
    if (ctx_.options.rpt_averages && (!avg_out_due || avg_exact)) {
        accumulateAvgResults();
    }

    // Post snapshot after advance so output_due() fires correctly.
    // All subcatch/node/link state arrays still reflect the end of the
    // just-completed routing step — advance only updates timers, not state.
    postOutputSnapshot(dt_next);

    if (avg_out_due && !avg_exact) {
        accumulateAvgResults();
    }

    // Fire step-end callback
    if (callbacks_.on_step_end) {
        callbacks_.on_step_end(
            static_cast<void*>(this),
            ctx_.current_date,
            dt_next,
            callbacks_.step_end_ud
        );
    }

    if (elapsed_time) *elapsed_time = ctx_.current_time / hydraulics::TimestepController::SEC_PER_DAY;
    return SWMM_OK;
}

// ============================================================================
// stepRunoff() — Phase A: runoff sub-stepping
// ============================================================================

/**
 * @brief Execute Phase A: runoff sub-stepping.
 *
 * @details Runs multiple runoff substeps per routing step using variable
 *          timestep control (matching legacy runoff_getTimeStep).
 *          Updates rain gages, climate, snowmelt, runoff, infiltration,
 *          groundwater, LIDs, quality buildup/washoff.
 *
 * @param dt_routing  Routing timestep (seconds).
 */
void SWMMEngine::stepRunoff(double dt_routing) noexcept {
    // ================================================================
    // PHASE A: RUNOFF WITH INDEPENDENT CLOCK (matching legacy)
    //
    // Legacy architecture (runoff.c + routing.c):
    //   1. runoff_execute() advances on its OWN clock (300s wet / 3600s dry)
    //   2. At each routing step, addWetWeatherInflows() INTERPOLATES:
    //        f = (routingTime - OldRunoffTime) / (NewRunoffTime - OldRunoffTime)
    //        q = (1 - f) * oldRunoff + f * newRunoff
    //   3. This produces smooth lateral inflows that ramp linearly between
    //      runoff evaluation boundaries.
    //
    // Without this, computing runoff at every 5-sec routing step produces
    // 15-18% higher peak rates (same volume, sharper peaks).
    // ================================================================

    double routing_time = ctx_.current_time;  // seconds from simulation start

    // --- Phase 1: Advance runoff clock if needed ---
    // Legacy: while (NewRunoffTime < nextRoutingTime) runoff_execute();
    // Runoff must catch up past the END of this routing step so that
    // infil/evap/runoff reflect the interval STARTING at the report
    // boundary. Legacy achieves this accidentally via variable timestep
    // Legacy uses strict < (runoff.c line 164: while(NewRunoffTime < nextRoutingTime))
    //
    // Clamp next_routing_time to the simulation end, matching legacy
    // swmm5.c::execRouting() line 900-905. Without this clamp, floating-point
    // drift in `routing_time + dt_routing` can leave next_routing_time slightly
    // above total_sec while new_runoff_time_ has already been clamped to
    // total_sec inside this loop body — causing the while-loop to fire
    // indefinitely with dt_runoff = 0.
    // PARITY: use the legacy-exact TotalDuration (swmm5.c:3198-3200) — the
    // combined-serial product (end-start)*86400 rounds differently and
    // perturbs the final clipped runoff step (see totalDurationMs()).
    const double total_sec_clamp = ctx_.options.totalDurationMs() / 1000.0;
    double next_routing_time = std::min(routing_time + dt_routing, total_sec_clamp);
    while (new_runoff_time_ < next_routing_time) {
        // Save old runoff/runon/conc + GW/snow/LID-drain state for interpolation
        // at the RUNOFF step cadence (matching legacy subcatch_setOldState +
        // lid_setOldGroupState, which legacy calls inside runoff_execute — NOT
        // per routing step). subcatches.save_state() snapshots
        // old_runoff/old_runon_inflow/conc_old; the rest is saved separately
        // (not covered by save_state). This is the ONLY place the subcatch
        // old-state is taken (see SimulationContext::save_state()).
        // lid_drain_flow is zeroed after the roll (legacy lid.c:1341-1342) and
        // re-accumulated by the A6b drain-routing loop later this substep.
        ctx_.subcatches.save_state();
        for (int i = 0; i < ctx_.n_subcatches(); ++i) {
            auto ui = static_cast<std::size_t>(i);
            ctx_.subcatches.old_gw_flow[ui] = ctx_.subcatches.gw_flow[ui];
            ctx_.subcatches.old_snow_depth[ui] = ctx_.subcatches.snow_depth[ui];
            ctx_.subcatches.old_lid_drain_flow[ui] =
                ctx_.subcatches.lid_drain_flow[ui];
            ctx_.subcatches.lid_drain_flow[ui] = 0.0;
        }

        // Advance runoff clock
        old_runoff_time_ = new_runoff_time_;
        old_runoff_ms_   = new_runoff_ms_;   // legacy runoff.c:229
        double abs_time = datetime::addSeconds(ctx_.options.start_date, old_runoff_time_);

        // Gap #26: clear LID drain quality node accumulators each runoff step.
        // They are repopulated in A6b and consumed each routing step by addWetWeatherLoads().
        // A4: water age keeps this alive at zero pollutants. The drain
        // volume accumulator is the age's mixing denominator, so the two
        // must be cleared and refilled on the same cadence.
        // The drain's WATER channel is cleared on this cadence regardless of
        // quality — it is what carries the drain into the network at all.
        std::fill(ctx_.nodes.lid_drain_inflow.begin(),
                  ctx_.nodes.lid_drain_inflow.end(), 0.0);
        if (ctx_.n_pollutants() > 0 || ctx_.options.water_age ||
            ctx_.options.heat_transport) {
            std::fill(ctx_.nodes.lid_drain_qual_load.begin(),
                      ctx_.nodes.lid_drain_qual_load.end(), 0.0);
            std::fill(ctx_.nodes.lid_drain_qual_vol.begin(),
                      ctx_.nodes.lid_drain_qual_vol.end(), 0.0);
            std::fill(ctx_.water_age_state.node_lid_drain_age_vol_in.begin(),
                      ctx_.water_age_state.node_lid_drain_age_vol_in.end(),
                      0.0);
            std::fill(ctx_.heat_state.node_lid_drain_temp_vol_in.begin(),
                      ctx_.heat_state.node_lid_drain_temp_vol_in.end(), 0.0);
        }

        // Current-step rainfall flag (legacy IsRaining): set BEFORE the timestep
        // is chosen, from this step's gage state.  has_runoff_/has_snow_ are
        // members carrying the PREVIOUS step's state (legacy HasRunoff/HasSnow).
        bool is_raining = false;

        // A1. Update rain gages and detect rainfall
        gage::updateAllGages(ctx_, abs_time);
        for (int g = 0; g < ctx_.n_gages(); ++g) {
            if (ctx_.gages.rainfall[static_cast<std::size_t>(g)] > 0.0)
                is_raining = true;
        }

        // A2. Update climate state
        int doy = datetime::dayOfYear(abs_time);
        int mon = datetime::monthOfYear(abs_time) - 1;
        int unit_sys = ucf::getUnitSystem(static_cast<int>(ctx_.options.flow_units));

        // A2a. Temperature source (before updateDailyClimate so
        //       Hargreaves/gamma/ea use the current temperature)
        if (ctx_.climate_state.temp_ts_index >= 0) {
            // Temperature from timeseries
            auto& tbl = ctx_.tables.tables[static_cast<std::size_t>(ctx_.climate_state.temp_ts_index)];
            ctx_.climate_state.temperature_src = table_lookup_cursor(tbl, abs_time);
            ctx_.climate_state.temperature_src += ctx_.climate_state.adjust_temp[mon];
        } else if (ctx_.options.temp_source == 2 && climate_file_.isOpen()) {
            // Temperature from climate file (Gap #9: sub-daily sinusoidal interp)
            climate::DailyClimateRecord rec;
            if (climate_file_.getRecord(abs_time, rec)) {
                if (!std::isnan(rec.tmin) && !std::isnan(rec.tmax)) {
                    double tmin = rec.tmin + ctx_.climate_state.adjust_temp[mon];
                    double tmax = rec.tmax + ctx_.climate_state.adjust_temp[mon];
                    ctx_.climate_state.temp_range = tmax - tmin;

                    if (doy != ctx_.climate_state.last_temp_doy) {
                        // New day: update sunrise/sunset params and roll over prev max.
                        ctx_.climate_state.prev_tmax     = ctx_.climate_state.has_minmax
                                                 ? ctx_.climate_state.tmax_daily : tmax;
                        ctx_.climate_state.tmin_daily    = tmin;
                        ctx_.climate_state.tmax_daily    = tmax;
                        ctx_.climate_state.has_minmax    = true;
                        ctx_.climate_state.last_temp_doy = doy;
                        climate::updateTempTimes(ctx_.climate_state, doy);
                    } else {
                        // Same day: refresh min/max (unchanged; tmax may update).
                        ctx_.climate_state.tmin_daily = tmin;
                        ctx_.climate_state.tmax_daily = tmax;
                    }

                    // Sub-daily sinusoidal interpolation (Gap #9).
                    // hour = fractional part of OADate × 24.
                    double hour = (abs_time - std::floor(abs_time)) * 24.0;
                    ctx_.climate_state.temperature_src = climate::getSubdailyTemp(ctx_.climate_state, hour);
                }
            }
        }

        // A2a'. Climate temperature forcing — applied before
        // updateDailyClimate so Hargreaves/gamma/ea use the forced value.
        // An OVERRIDE prescription replaces the data-source value (and
        // bypasses monthly adjustments by design); ADD augments it. The
        // source/default base (temperature_src) is resolved fresh each step so
        // a one-shot or cleared forcing reverts to the source rather than
        // sticking at the last forced value.
        ctx_.climate_state.temperature =
            ctx_.forcing.effective_temperature(ctx_.climate_state.temperature_src);

        climate::updateDailyClimate(ctx_.climate_state, doy, mon);

        // A2b'. Seasonal snowmelt coefficients. Legacy recomputes these once
        // a day, from setTemp in climate.c:1176-1180, and NOTHING in this
        // engine called the modern equivalent: `dhm` stayed at its
        // `assign(0.0)` value, so `imelt = dhm * (temp - tbase)` was
        // identically zero and the degree-day melt term never fired on any
        // deck. Only rain-on-snow melt could produce water. The unit suite
        // did not catch it because every snow gate calls setMeltCoeffs
        // itself before stepping the solver.
        if (doy != last_melt_doy_) {
            last_melt_doy_ = doy;
            snow_.setMeltCoeffs(doy);
        }

        // A2b. Evaporation from timeseries or climate file
        if (ctx_.climate_state.evap_method == climate::EvapMethod::TIMESERIES &&
            ctx_.climate_state.evap_ts_index >= 0) {
            auto& tbl = ctx_.tables.tables[static_cast<std::size_t>(ctx_.climate_state.evap_ts_index)];
            double evap_user = table_lookup_cursor(tbl, abs_time);
            ctx_.climate_state.evap_rate = evap_user / ucf::Ucf[ucf::EVAPRATE][unit_sys];
            ctx_.climate_state.evap_rate *= ctx_.climate_state.adjust_evap[mon];
        }
        else if (ctx_.climate_state.evap_method == climate::EvapMethod::PAN &&
                 climate_file_.isOpen()) {
            // Pan evaporation from climate file × monthly pan coefficient
            climate::DailyClimateRecord rec;
            if (climate_file_.getRecord(abs_time, rec) && !std::isnan(rec.evap)) {
                // rec.evap is in user units (in/day US, mm/day SI)
                ctx_.climate_state.evap_rate = rec.evap / ucf::Ucf[ucf::EVAPRATE][unit_sys];
                ctx_.climate_state.evap_rate *= ctx_.options.pan_coeff[mon];
                ctx_.climate_state.evap_rate *= ctx_.climate_state.adjust_evap[mon];
            }
        }

        // A2b'. System-wide evaporation forcing — applied after all evap
        // sources and monthly adjustments so the prescription is final.
        // Per-subcatchment PET forcing still takes precedence downstream
        // (effective_evap_rate in the runoff/LID/GW solvers).
        ctx_.climate_state.evap_rate =
            ctx_.forcing.effective_climate_evap(ctx_.climate_state.evap_rate);

        // A2c. Wind speed lookup
        if (ctx_.options.wind_type == 0) {
            ctx_.climate_state.wind_speed_src = ctx_.options.wind_speed[mon];
        } else if (ctx_.options.wind_type == 1 && climate_file_.isOpen()) {
            // Wind from climate file
            climate::DailyClimateRecord rec;
            if (climate_file_.getRecord(abs_time, rec) && !std::isnan(rec.wind)) {
                ctx_.climate_state.wind_speed_src = rec.wind;
            }
        }
        // A2c'. Climate wind forcing (OVERRIDE replaces, ADD augments). The
        // source/default base (wind_speed_src) is resolved fresh each step so
        // a one-shot or cleared forcing reverts to the source.
        ctx_.climate_state.wind_speed =
            ctx_.forcing.effective_wind(ctx_.climate_state.wind_speed_src);

        // A2c''. H2: relative humidity, the same monthly lookup wind takes.
        // ClimateState::humidity has carried a 50 % default since before the
        // transport program with NO writer anywhere; SurfaceExchange is its
        // first consumer, so this is where it starts being real.
        ctx_.climate_state.humidity = ctx_.options.humidity[mon];

        // A2d. Monthly adjustment factors
        ctx_.climate_state.infil_factor = ctx_.adjust_hydcon[mon];

        // A2e. Recovery pattern lookup (monthly pattern for soil recovery)
        if (ctx_.climate_state.recovery_pat_index >= 0) {
            auto ui = static_cast<std::size_t>(ctx_.climate_state.recovery_pat_index);
            if (ui < ctx_.patterns.factors.size()) {
                const auto& facs = ctx_.patterns.factors[ui];
                auto umon = static_cast<std::size_t>(mon);
                ctx_.climate_state.recovery_factor = (umon < facs.size()) ? facs[umon] : 1.0;
            }
        }

        // A2f. Apply rainfall adjustment to all gages
        {
            double rain_factor = ctx_.adjust_rain[mon];
            if (rain_factor != 1.0) {
                for (int g = 0; g < ctx_.n_gages(); ++g)
                    ctx_.gages.rainfall[static_cast<std::size_t>(g)] *= rain_factor;
            }
        }

        // A2z. [FILES] USE RUNOFF — replace this runoff substep with the
        // next record from the runoff interface file (legacy
        // runoff_execute(): gage + climate state above still update; the
        // record supplies runoff/evap/infil/GW state and its own timestep,
        // which drives the runoff clock exactly like legacy
        // runoff_readFromFile()).
        if (runoff_iface_file_ && runoff_iface_file_->isOpen()
            && ctx_.files.runoff_mode == FileMode::USE) {
            double file_dt = 0.0;
            if (!runoff_iface_file_->readResults(ctx_, &file_dt)) {
                // Legacy ERR 327 (read beyond end of runoff interface
                // file). Freeze the runoff clock at the simulation end so
                // the last state holds and the loop cannot spin.
                ctx_.errors.push_back(format_error(ERR_RUNOFF_IFACE_EOF, ""));
                emit_warning(SWMM_ERR_IO, "USE RUNOFF: attempting to read "
                             "beyond end of runoff interface file");
                new_runoff_time_ = total_sec_clamp;
                new_runoff_ms_ = ctx_.options.totalDurationMs();
                break;
            }
            if (file_dt <= 0.0) file_dt = 1.0;
            new_runoff_time_ = old_runoff_time_ + file_dt;
            new_runoff_ms_ = new_runoff_ms_ + 1000.0 * file_dt;
            const double total_ms_use = ctx_.options.totalDurationMs();
            const double total_sec_use = total_ms_use / 1000.0;
            if (new_runoff_time_ > total_sec_use)
                new_runoff_time_ = total_sec_use;
            if (new_runoff_ms_ > total_ms_use) new_runoff_ms_ = total_ms_use;

            // RDII stays on its internal path (legacy computes RDII
            // independently of the runoff file) unless USE RDII overrides.
            // IGNORE_RAINFALL suppresses RDII too (legacy rain_open gates it).
            if (!ctx_.options.ignore_rdii
                && !ctx_.options.ignore_rainfall
                && !(rdii_iface_file_.isOpen()
                     && !rdii_iface_file_.isWriting())) {
                rdii_.advance(ctx_, new_runoff_time_);
            }
            continue;   // file replaces snowmelt/runoff/GW/LID/quality steps
        }

        // Compute variable runoff timestep
        double dt_runoff = computeRunoffTimestep(abs_time, is_raining, has_runoff_, has_snow_);
        if (dt_runoff <= 0.0) dt_runoff = 1.0;

        // Update runoff clock
        new_runoff_time_ = old_runoff_time_ + dt_runoff;
        // PARITY: ms mirror with the exact legacy ops (runoff.c:230-237):
        //   NewRunoffTime += (double)(1000 * runoffStep);
        //   if > TotalDuration: runoffStep = (TotalDuration-OldRunoffTime)/1000
        //                       NewRunoffTime = TotalDuration
        // TotalDuration is (End-Start)*MSECperDAY (project.c), formed here
        // directly in ms — NOT 1000*total_sec, which rounds differently.
        new_runoff_ms_ = new_runoff_ms_ + 1000.0 * dt_runoff;
        // Total simulation duration — legacy-exact TotalDuration
        // (swmm5.c:3198-3200; see SimulationOptions::totalDurationMs()).
        const double total_ms = ctx_.options.totalDurationMs();
        double total_sec = total_ms / 1000.0;
        if (new_runoff_time_ > total_sec) {
            dt_runoff = (total_ms - old_runoff_ms_) / 1000.0;  // legacy runoff.c:235
            new_runoff_time_ = total_sec;
        }
        if (new_runoff_ms_ > total_ms) new_runoff_ms_ = total_ms;

        // A2a. RDII convolution at wet weather step (matching legacy RdiiStep = WetStep).
        //      Legacy pre-computes RDII in createRdiiFile() at WetStep cadence;
        //      we compute here using each UH group's assigned rain gage.
        //      Results are buffered in rdii_ and applied during routing.
        //      IGNORE_RAINFALL suppresses RDII too (legacy rain_open gates it).
        if (!ctx_.options.ignore_rdii && !ctx_.options.ignore_rainfall) {
            if (rdii_iface_file_.isOpen() && !rdii_iface_file_.isWriting()) {
                // [FILES] USE RDII: UH computation bypassed — flows come
                // straight from the interface file in stepRouting() B2a
                // (legacy rdii_openRdii() skips createRdiiFile()).
            } else {
                rdii_.advance(ctx_, new_runoff_time_);
                // [FILES] SAVE RDII: export the freshly computed flows
                // (legacy saveRdiiFlows(): date + per-node cfs).
                if (rdii_iface_file_.isOpen() && rdii_iface_file_.isWriting()) {
                    rdii_iface_file_.saveFlows(abs_time, rdii_.nodeFlows());
                }
            }
        }

        // A3. Snowmelt — per-subcatchment precipitation split into rain vs.
        //     snow, accumulation + plowing, then melt and per-subcatch net
        //     precip wiring (Gap #18/#20). Matches legacy runoff.c:
        //     snow_plowSnow() each runoff step, then subcatch_getRunoff →
        //     getNetPrecip → snow_getSnowMelt with that subcatchment's own
        //     rainfall/snowfall.
        // IGNORE_SNOWMELT: skip the whole snow accumulation/plow/melt path
        // (legacy runoff.c:254 snow_plowSnow skip + gage.c:517 precip split).
        // The runoff solver then falls back to raw gage precip via the matching
        // guard in Runoff.cpp execute() (subcatch.c:784), so snow_net_* being
        // left stale is harmless.
        if (!ctx_.options.ignore_snow_melt) {
            // Per-subcatchment rain/snow assembly (ft/sec). The gage value
            // is split by air temperature vs. the dividing temperature;
            // rainfall and snowfall forcing channels then resolve on their
            // respective components.
            auto un_sc = static_cast<std::size_t>(ctx_.n_subcatches());
            snow_rain_.assign(un_sc, 0.0);
            snow_snow_.assign(un_sc, 0.0);
            for (int i = 0; i < ctx_.n_subcatches(); ++i) {
                auto ui = static_cast<std::size_t>(i);
                if (ctx_.subcatches.snowpack[ui] < 0) continue;
                // Single source of truth for the split (gage.c:513-523 parity):
                // applies the IgnoreSnowmelt guard, the temperature test, the
                // gage snow catch factor, and the subcatchment scale factors.
                // Returns ft/sec.
                gage::PrecipSplit p = gage::splitPrecip(ctx_, ui);
                // Forcing channels resolve on top. effective_rainfall() takes
                // user units, so convert out and back; the snowfall channel
                // already stores ft/sec.
                double rain_inhr = p.rainfall * ucf::Ucf[ucf::RAINFALL][0];
                rain_inhr = ctx_.forcing.effective_rainfall(ui, rain_inhr);
                snow_rain_[ui] = rain_inhr / ucf::Ucf[ucf::RAINFALL][0];
                snow_snow_[ui] = ctx_.forcing.effective_snowfall(ui, p.snowfall);
            }

            // S2b — hand the snow solver what it needs to carry water age.
            // A scalar in, nothing out: hydrology keeps no dependency on the
            // transport layer, the same way `season` is handed in. Set
            // BEFORE plowSnow, because plowSnow is where snowfall is mixed
            // into the pack at its source age.
            {
                auto& snow_soa = snow_.state();
                snow_soa.track_age  = ctx_.options.water_age;
                snow_soa.precip_age = ctx_.water_age_config.global_age[
                    static_cast<int>(WaterAgeSource::RAINFALL)];
            }

            // Accumulation + plowing BEFORE melt (legacy runoff.c:254).
            snow_.plowSnow(ctx_, dt_runoff, snow_snow_.data());

            snow_.execute(ctx_, dt_runoff, ctx_.climate_state.temperature,
                          ctx_.climate_state.wind_speed, snow_rain_.data(),
                          snow_snow_.data(),
                          ctx_.climate_state.gamma, ctx_.climate_state.ea);

            // A3a (Gap #20): Build per-subcatch snow-modified net precip.
            // netPrecip[i] = imelt[i] + rainfall*(1-asc[i])  (matching legacy)
            // Combined imperv = area-weighted avg of plowable + non-plowable imperv.
            const auto& soa = snow_.state();
            for (int i = 0; i < ctx_.n_subcatches(); ++i) {
                auto ui = static_cast<std::size_t>(i);
                if (ctx_.subcatches.snowpack[ui] < 0) continue;

                double rainfall_ft = snow_rain_[ui];

                auto plow_idx   = static_cast<std::size_t>(i * snow::N_SUBAREAS + snow::SNOW_PLOWABLE);
                auto imperv_idx = static_cast<std::size_t>(i * snow::N_SUBAREAS + snow::SNOW_IMPERV);
                auto perv_idx   = static_cast<std::size_t>(i * snow::N_SUBAREAS + snow::SNOW_PERV);

                double fPlow   = soa.fArea[plow_idx];
                double fImperv = soa.fArea[imperv_idx];
                double fTotalI = fPlow + fImperv;

                double net_plow   = soa.imelt[plow_idx]   + rainfall_ft * (1.0 - soa.asc[plow_idx]);
                double net_imperv = soa.imelt[imperv_idx] + rainfall_ft * (1.0 - soa.asc[imperv_idx]);
                double net_perv   = soa.imelt[perv_idx]   + rainfall_ft * (1.0 - soa.asc[perv_idx]);

                ctx_.subcatches.snow_net_imperv[ui] = (fTotalI > 0.0)
                    ? (net_plow * fPlow + net_imperv * fImperv) / fTotalI
                    : net_plow;
                ctx_.subcatches.snow_net_perv[ui] = net_perv;

                // S2: the MELT-ONLY part, under the identical area blend.
                // Arriving water under a pack is two different waters —
                // meltwater at 0 C carrying the pack's age, and rain that
                // reached the ground through the snow-free fraction — and
                // their sum cannot say what either is worth. The split is a
                // local here; publishing it follows A4's precedent rather
                // than having transport reconstruct it.
                ctx_.subcatches.snow_melt_imperv[ui] = (fTotalI > 0.0)
                    ? (soa.imelt[plow_idx] * fPlow +
                       soa.imelt[imperv_idx] * fImperv) / fTotalI
                    : soa.imelt[plow_idx];
                ctx_.subcatches.snow_melt_perv[ui] = soa.imelt[perv_idx];

                // S2b — the AGE of that meltwater, under the identical
                // blend. Weighted by melt VOLUME rather than by area alone:
                // the impervious value mixes plowable and non-plowable melt,
                // and two surfaces contributing different rates do not
                // contribute their ages equally. Weighting by area would be
                // the same expression as the rate blend and would still be
                // wrong, because an age is an intensive property of the
                // water and the rate is not.
                if (soa.track_age) {
                    const double m_plow = soa.imelt[plow_idx] * fPlow;
                    const double m_imp  = soa.imelt[imperv_idx] * fImperv;
                    const double m_sum  = m_plow + m_imp;
                    ctx_.subcatches.snow_melt_age_imperv[ui] =
                        (m_sum > 0.0)
                            ? (soa.out_age[plow_idx] * m_plow +
                               soa.out_age[imperv_idx] * m_imp) / m_sum
                            : soa.out_age[plow_idx];
                    ctx_.subcatches.snow_melt_age_perv[ui] =
                        soa.out_age[perv_idx];
                }
            }

            // Legacy newSnowDepth (subcatch.c:816): the per-subcatch SWE
            // snapshot is taken each runoff step so the reporter can blend
            // old/new to the report instant (subcatch.c:888-890). Same
            // recompute as the API getter.
            for (int i = 0; i < ctx_.n_subcatches(); ++i) {
                auto ui = static_cast<std::size_t>(i);
                ctx_.subcatches.snow_depth[ui] = subcatchSnowDepth(i);
            }
        }

        // A3b. Apply N-PERV/DSTORE pattern adjustments (before runoff)
        if (ctx_.has_subcatch_adj_patterns) {
            for (int i = 0; i < ctx_.n_subcatches(); ++i) {
                auto ui = static_cast<std::size_t>(i);
                // N-PERV pattern
                if (ui < ctx_.subcatch_n_perv_pattern.size()) {
                    int pi = ctx_.subcatch_n_perv_pattern[ui];
                    if (pi >= 0 && static_cast<std::size_t>(pi) < ctx_.patterns.factors.size()) {
                        const auto& facs = ctx_.patterns.factors[static_cast<std::size_t>(pi)];
                        auto umon = static_cast<std::size_t>(mon);
                        double f = (umon < facs.size()) ? facs[umon] : 1.0;
                        ctx_.subcatches.n_perv[ui] = ctx_.base_n_perv[ui] * f;
                    }
                }
                // DSTORE pattern
                if (ui < ctx_.subcatch_d_store_pattern.size()) {
                    int pi = ctx_.subcatch_d_store_pattern[ui];
                    if (pi >= 0 && static_cast<std::size_t>(pi) < ctx_.patterns.factors.size()) {
                        const auto& facs = ctx_.patterns.factors[static_cast<std::size_t>(pi)];
                        auto umon = static_cast<std::size_t>(mon);
                        double f = (umon < facs.size()) ? facs[umon] : 1.0;
                        ctx_.subcatches.ds_perv[ui] = ctx_.base_ds_perv[ui] * f;
                    }
                }
                // INFIL pattern: scales infil_factor for this subcatchment
                // (applied globally via ctx_.climate_state.infil_factor already;
                //  per-subcatchment INFIL pattern would require per-subcatch
                //  infil_factor which is a deeper refactor — noted for future)
            }
        }

        // A4. Runoff (computes subcatches.runoff[i] = newRunoff rate)
        //     Runoff solver is self-contained; output is subcatches.runoff[i].
        //     Routing picks it up via Phase 2 interpolation → nodes.runoff_inflow[].
        runoff_.execute(ctx_, dt_runoff, ctx_.climate_state.evap_rate,
                        ctx_.climate_state.infil_factor, ctx_.climate_state.recovery_factor, mon);

        // A3: subcatchment surface age, immediately after the depths this
        // step produced. Reads the solver's own SoA because
        // ctx.subcatches.ponded_depth is declared but written by NOBODY.
        transport::routeSubcatchmentAge(ctx_, runoff_.soa(), dt_runoff);

        // H5a: the temperature mirror, on the same clock and reading the
        // same SoA. It carries the surface energy balance (D-H5a), so it
        // must see dt_runoff — H2's node/link bindings run on the ROUTING
        // clock and pass their own dt; handing either the other's interval
        // would scale every flux wrongly.
        transport::routeSubcatchmentTemperature(ctx_, runoff_.soa(),
                                                dt_runoff);

        // Update persistent state flags for the NEXT step's timestep selection
        // (legacy runoff.c sets HasRunoff/HasSnow here, after computing runoff,
        // and reads them in the next runoff_getTimeStep call — one-step lag).
        // Keeping wet_step while has_runoff_ holds integrates the recession limb
        // at the fine step legacy uses.
        has_runoff_ = false;
        for (int i = 0; i < ctx_.n_subcatches(); ++i) {
            if (ctx_.subcatches.runoff[static_cast<std::size_t>(i)] > 0.0) {
                has_runoff_ = true;
                break;
            }
        }

        // A4b. Runoff mass-balance accumulation is deferred to A6c (after the
        // LID routing adjusts subcatches.runoff by −VlidIn/+VlidOut/+drain);
        // accumulating here would capture the pre-LID runoff and miss the LID
        // exchange, leaving the continuity unbalanced (issue #102 D/E).

        // A4b'. Phase 1b auto-save hook — when the runoff interface file
        // is open in SAVE mode, emit one record per substep. saveResults
        // is a cheap no-op when the file is not in SAVE mode, so the
        // unconditional call here costs nothing for ordinary runs.
        saveRunoffIfaceStep(dt_runoff);

        // A4c. Surface quality: buildup + washoff. IGNORE_QUALITY skips all
        //      pollutant buildup/washoff/sweeping (legacy runoff.c:274
        //      `if (IgnoreQuality) continue;`).
        if (!ctx_.options.ignore_quality) {
            stepSurfaceQuality(dt_runoff);
        }

        // A5. Groundwater — IGNORE_GROUNDWATER skips the coupling + solver
        //     entirely (legacy subcatch.c:712 `!IgnoreGwater && ...groundwater`).
        //     With the solver skipped, gw_flow[] stays 0 so the downstream GW
        //     scatter contributes nothing to node inflow, and the GW mass
        //     balance terms (accumulated inside groundwater_.execute) self-zero
        //     (legacy massbal.c:286). The auto-coupling in resolve_cross_references
        //     already forces this flag on when the model has no aquifers.
        if (!ctx_.options.ignore_groundwater) {
            // A5a. Assemble GW coupling (pre-compute sw_head from routing state)
            assembleGWCoupling(dt_runoff);

            // A5b. Groundwater solver (reads subcatches.gw_sw_head, not nodes directly)
            stepGroundwater(dt_runoff);
        }

        // A6. LID performance
        // A6a. Compute per-unit LID inflow from non-LID subarea runoff + rainfall.
        // Gap #23: matches legacy lid_getRunoff() line ~1669:
        //   lidInflow = (qImperv * fromImperv + qPerv * fromPerv) / lidArea
        // where qImperv/qPerv are CFS from non-LID impervious/pervious subareas.
        const auto& rsoa = runoff_.soa();
        for (int t = 0; t < lid_.numGroups(); ++t) {
            auto& g = lid_.group(t);
            if (g.count == 0) continue;
            for (int u = 0; u < g.count; ++u) {
                auto uu = static_cast<std::size_t>(u);
                int sc = g.subcatch_idx[uu];
                if (sc < 0 || sc >= ctx_.n_subcatches()) {
                    g.inflow[uu] = 0.0;
                    continue;
                }
                auto usc = static_cast<std::size_t>(sc);
                // Net precip on the LID surface in INTERNAL units (ft/s) —
                // set by RunoffSolver::execute() earlier this step, matching
                // legacy lidInflow += subcatch->rainfall. ctx_.gages.rainfall
                // holds display in/hr and must not be used here (issue #131:
                // it saturated LID soils in one step, 43200x the real rate).
                double rain = ctx_.subcatches.rainfall[usc];
                // Per-subarea runoff CFS from non-LID area (set by RunoffSolver, Gap #23)
                double q_imperv = rsoa.imperv_runoff_cfs[usc];
                double q_perv   = rsoa.perv_runoff_cfs[usc];
                double lid_area = g.area[uu];
                // Inflow = rainfall on LID + fraction of non-LID subarea runoff captured
                double captured_cfs = q_imperv * g.from_imperv[uu]
                                    + q_perv   * g.from_perv[uu];
                double q_from_sc = (lid_area > 0.0) ? captured_cfs / lid_area : 0.0;
                // −VlidIn (legacy subcatch.c:746-751,
                // `vOutflow = Voutflow − VlidIn + VlidOut`): the captured share
                // is the LID's inflow, so it is no longer the subcatchment's
                // outflow. Without this it leaves the outlet in full AND runs
                // through the LID, so LID capture buys no runoff reduction and
                // the run gains volume equal to whatever the unit sheds. The
                // +VlidOut half is applied below, after the units are stepped.
                // q_imperv/q_perv already carry legacy's fOutlet scaling
                // (Runoff.cpp), which is what keeps this subtraction bounded by
                // the outlet runoff it is removing from.
                if (captured_cfs > 0.0)
                    ctx_.subcatches.runoff[usc] -= captured_cfs;
                // Legacy lid.c:1714-1718: when the LID occupies the full
                // subcatchment (non-LID area snapped to zero), upstream and
                // outfall runon flows onto the LID units — the runoff solver
                // has no subarea left to receive it (issue #131).
                if (rsoa.area[usc] <= 0.0 &&
                    usc < ctx_.subcatches.total_lid_area_ft2.size() &&
                    ctx_.subcatches.total_lid_area_ft2[usc] > 0.0) {
                    double runon_q = ctx_.subcatches.runon_inflow[usc];  // CFS
                    if (runon_q > 0.0)
                        q_from_sc += runon_q /
                            ctx_.subcatches.total_lid_area_ft2[usc];
                }
                g.inflow[uu] = rain + q_from_sc;
                // A4: the age of that inflow, weighted by the same four
                // rates. This is the only place they exist together, which
                // is why the age is assembled here rather than in the
                // transport module.
                transport::setLidInflowAge(
                    ctx_, t, u, sc, rain,
                    q_imperv * g.from_imperv[uu], q_perv * g.from_perv[uu],
                    (rsoa.area[usc] <= 0.0 &&
                     usc < ctx_.subcatches.total_lid_area_ft2.size() &&
                     ctx_.subcatches.total_lid_area_ft2[usc] > 0.0)
                        ? ctx_.subcatches.runon_inflow[usc]
                        : 0.0,
                    lid_area);
                // H5b: the temperature of that same inflow, weighted by the
                // same four rates.
                transport::setLidInflowTemperature(
                    ctx_, t, u, sc, rain,
                    q_imperv * g.from_imperv[uu], q_perv * g.from_perv[uu],
                    (rsoa.area[usc] <= 0.0 &&
                     usc < ctx_.subcatches.total_lid_area_ft2.size() &&
                     ctx_.subcatches.total_lid_area_ft2[usc] > 0.0)
                        ? ctx_.subcatches.runon_inflow[usc]
                        : 0.0,
                    lid_area);
            }
        }

        lid_.execute(ctx_, dt_runoff, 0.0, ctx_.climate_state.evap_rate);

        // A4: LID layer ages, immediately after the depths this step
        // produced — the same slot A3's watershed update occupies relative
        // to the runoff solver.
        transport::routeLidLayerAge(ctx_, lid_, dt_runoff);
        // H5b: and the temperature column, on the same clock. Separate
        // entry point rather than a species loop inside one routine: the
        // two tracks differ in their operators (no aging, no zero floor, a
        // dry-layer policy, and a coupled conduction solve), not merely in
        // which row they write.
        transport::routeLidLayerTemperature(ctx_, lid_, dt_runoff);

        // A6b. Route LID outputs back to subcatchment runoff totals
        for (int t = 0; t < lid_.numGroups(); ++t) {
            const auto& g = lid_.group(t);
            if (g.count == 0) continue;
            for (int u = 0; u < g.count; ++u) {
                auto uu = static_cast<std::size_t>(u);
                int sc = g.subcatch_idx[uu];
                if (sc < 0 || sc >= ctx_.n_subcatches()) continue;
                auto usc = static_cast<std::size_t>(sc);
                double lid_area = g.area[uu];

                // Surface runoff from LID (ft/sec * ft² = CFS):
                //  to_perv==0 → goes to subcatch outlet (add to subcatch runoff CFS)
                //  to_perv==1 → returned to pervious area next step (Gap #23)
                if (!g.to_perv[uu]) {
                    ctx_.subcatches.runoff[usc] += g.surface_runoff[uu] * lid_area;  // CFS
                } else {
                    // Store as pervious return flow; consumed by RunoffSolver next step.
                    // Matches legacy lid_getFlowToPerv() one-step-lag mechanism.
                    ctx_.subcatches.lid_return_to_perv_cfs[usc] +=
                        g.surface_runoff[uu] * lid_area;  // CFS
                }
                // Drain flow (ft/sec * ft² = CFS):
                //  drain_node >= 0        → external node inflow (lid_addDrainInflow)
                //  drain_subcatch != self → runon to that subcatch (lid_addDrainRunon)
                //  no target / self       → the subcatchment's OWN outlet, node
                //                           or subcatchment (legacy lid.c:1215
                //                           assigns drainNode = outNode and
                //                           drainSubcatch = outSubcatch at
                //                           init when the user set neither).
                //  The no-target case once recirculated as runon-to-self,
                //  which double-feeds the subcatchment and leaks continuity;
                //  then (merge a38f0c0b) it was added to subcatches.runoff,
                //  which routed the water through the runoff channel while the
                //  quality block below booked the same volume to the outlet
                //  NODE — water once, quality twice, and neither temperature
                //  nor age paired on the water side. Resolving to the outlet
                //  here makes water and quality take the SAME path.
                // Legacy group newDrainFlow (lid.c:1729): destination-agnostic
                // total drain rate, rolled per runoff step so the reporter can
                // fold interpolated drain flow into reported runoff
                // (subcatch.c:897-902). Legacy zeroes a unit's drain when it
                // returns to the pervious area (lid.c:1893-1898); that path is
                // not modeled here, so the rollup matches what is routed.
                ctx_.subcatches.lid_drain_flow[usc] +=
                    g.drain_flow[uu] * lid_area;  // CFS
                int dn  = g.drain_node[uu];
                int dsc = g.drain_subcatch[uu];
                if (dn < 0 && (dsc < 0 || dsc == sc)) {
                    dn  = ctx_.subcatches.outlet_node[usc];
                    dsc = ctx_.subcatches.outlet_subcatch[usc];
                }
                if (dn >= 0) {
                    auto un = static_cast<std::size_t>(dn);
                    // NOT ext_inflow: clearInflowSources() zeroes that at the
                    // top of every routing step, after this ran, so a drain
                    // booked there never reached the network. See
                    // NodeData::lid_drain_inflow.
                    if (un < ctx_.nodes.lid_drain_inflow.size()) {
                        ctx_.nodes.lid_drain_inflow[un] += g.drain_flow[uu] * lid_area;  // CFS
                    }
                } else if (dsc >= 0 && dsc != sc) {
                    auto utsc = static_cast<std::size_t>(dsc);
                    if (utsc < ctx_.subcatches.lid_drain_runon_cfs.size()) {
                        const double q_dr = g.drain_flow[uu] * lid_area;
                        ctx_.subcatches.lid_drain_runon_cfs[utsc] += q_dr;
                        // A4: and its age, so assembleRunon can hand the
                        // rate and the q·age together.
                        auto& wsr = ctx_.water_age_state
                                        .subcatch_lid_drain_age_cfs;
                        const auto& lst = ctx_.lid_layer_state;
                        const auto uts2 = static_cast<std::size_t>(t);
                        if (ctx_.options.water_age && lst.active() &&
                            utsc < wsr.size() &&
                            uts2 + 1 < lst.group_offset.size()) {
                            const int fl = lst.group_offset[uts2] + u;
                            if (fl >= 0 && fl < lst.n_units)
                                wsr[utsc] += q_dr * lst.drain_value[
                                    static_cast<std::size_t>(fl) *
                                        static_cast<std::size_t>(
                                            lst.n_species) +
                                    static_cast<std::size_t>(LidSpecies::AGE)];
                        }
                        // H5b: the same seam for temperature — and this is
                        // the third run-on contributor H5a carried as owed,
                        // because a drain's temperature is a per-layer
                        // quantity that did not exist until now. It goes
                        // through addRunonTemperatureAt so the numerator and
                        // the rate move together (H5a's pair invariant).
                        if (ctx_.options.heat_transport && lst.active() &&
                            uts2 + 1 < lst.group_offset.size()) {
                            const int fl = lst.group_offset[uts2] + u;
                            // Merge repair (issue #156 session, 2026-08-29):
                            // the merged H5b block referenced `target_sc`,
                            // which does not exist in this branch — the runon
                            // receiver here is g.drain_subcatch[uu] (utsc).
                            if (fl >= 0 && fl < lst.n_units)
                                transport::addRunonTemperatureAt(
                                    ctx_, dsc, q_dr,
                                    lst.drain_value[
                                        static_cast<std::size_t>(fl) *
                                            static_cast<std::size_t>(
                                                lst.n_species) +
                                        static_cast<std::size_t>(
                                            LidSpecies::TEMPERATURE)]);
                        }
                    }
                }
                // (A subcatchment with neither an outlet node nor an outlet
                //  subcatchment drops its runoff too; the drain follows it.)

                // Gap #26: LID drain quality routing.
                // The drain carries source-subcatch quality, reduced by drainRmvl.
                // Route to the destination node's quality inflow accumulators
                // (lid_drain_qual_load / lid_drain_qual_vol), which are drained
                // into qual_mass_in / qual_vol_in each routing step.
                // Matches legacy lid_addDrainInflow() (drain-to-node) and
                // lid_addDrainRunon() (drain-to-subcatch) quality handling.
                {
                    int np_ctx = ctx_.n_pollutants();
                    int np_lid = g.n_pollutants;
                    int np_use = std::min(np_ctx, np_lid);
                    // IGNORE_QUALITY: no LID drain pollutant loads (runoff.c:274).
                    // A4: water age joins the guard. Gated on np_use alone,
                    // a pure-age model never accumulated the drain volume at
                    // all, so neither the water nor its age reached the node
                    // — the np-guard family once more.
                    // LID fix round: heat joins the guard for the same
                    // reason age did — the drain's temperature is paired
                    // inside this block, so a heat-only deck must reach it.
                    if ((np_use > 0 || ctx_.options.water_age ||
                         ctx_.options.heat_transport)
                        && g.drain_flow[uu] > 0.0
                        && !ctx_.options.ignore_quality) {
                        double drain_cfs = g.drain_flow[uu] * lid_area;
                        // Determine destination node index
                        int dest_node = g.drain_node[uu];
                        if (dest_node < 0) {
                            // Drain-to-subcatch: use outlet node of target subcatch
                            int target_sc = (g.drain_subcatch[uu] >= 0)
                                            ? g.drain_subcatch[uu] : sc;
                            if (target_sc >= 0 && target_sc < ctx_.n_subcatches())
                                dest_node = ctx_.subcatches.outlet_node[
                                    static_cast<std::size_t>(target_sc)];
                        }
                        if (dest_node >= 0 && dest_node < ctx_.n_nodes()) {
                            auto udn = static_cast<std::size_t>(dest_node);
                            // Add drain volume to node quality vol accumulator
                            if (udn < ctx_.nodes.lid_drain_qual_vol.size())
                                ctx_.nodes.lid_drain_qual_vol[udn] += drain_cfs;
                            // A4: and its age, as the usual q·age rate. The
                            // drain draws from the storage layer (decision
                            // 2026-08-18), which is what drain_value holds.
                            if (ctx_.options.water_age) {
                                const auto& lst = ctx_.lid_layer_state;
                                const auto uts = static_cast<std::size_t>(t);
                                if (lst.active() &&
                                    uts + 1 < lst.group_offset.size()) {
                                    const int flat = lst.group_offset[uts] + u;
                                    const auto ai =
                                        static_cast<std::size_t>(flat) *
                                            static_cast<std::size_t>(
                                                lst.n_species) +
                                        static_cast<std::size_t>(
                                            LidSpecies::AGE);
                                    if (flat >= 0 && flat < lst.n_units &&
                                        udn < ctx_.water_age_state
                                                  .node_lid_drain_age_vol_in
                                                  .size())
                                        ctx_.water_age_state
                                            .node_lid_drain_age_vol_in[udn] +=
                                            drain_cfs * lst.drain_value[ai];
                                }
                            }
                            // H5b pairing at the NODE seam: the drain leaves
                            // at the storage layer's temperature, booked as
                            // the usual q·T rate beside its age. Consumed by
                            // the wet-weather loader in place of the RAINFALL
                            // stand-in it used until the LID fix round.
                            if (ctx_.options.heat_transport) {
                                const auto& lst = ctx_.lid_layer_state;
                                const auto uts = static_cast<std::size_t>(t);
                                if (lst.active() &&
                                    uts + 1 < lst.group_offset.size()) {
                                    const int flat = lst.group_offset[uts] + u;
                                    const auto ti =
                                        static_cast<std::size_t>(flat) *
                                            static_cast<std::size_t>(
                                                lst.n_species) +
                                        static_cast<std::size_t>(
                                            LidSpecies::TEMPERATURE);
                                    if (flat >= 0 && flat < lst.n_units &&
                                        udn < ctx_.heat_state
                                                  .node_lid_drain_temp_vol_in
                                                  .size())
                                        ctx_.heat_state
                                            .node_lid_drain_temp_vol_in[udn] +=
                                            drain_cfs * lst.drain_value[ti];
                                }
                            }
                            // Add drain mass load per pollutant
                            for (int p = 0; p < np_use; ++p) {
                                auto rmvl_idx = static_cast<std::size_t>(u * np_lid + p);
                                double rmvl = (rmvl_idx < g.drain_rmvl.size())
                                              ? g.drain_rmvl[rmvl_idx] : 0.0;
                                auto src_idx = static_cast<std::size_t>(sc * np_ctx + p);
                                double c_src = (src_idx < ctx_.subcatches.conc.size())
                                               ? ctx_.subcatches.conc[src_idx] : 0.0;
                                auto nd_idx = udn * static_cast<std::size_t>(np_ctx)
                                            + static_cast<std::size_t>(p);
                                if (nd_idx < ctx_.nodes.lid_drain_qual_load.size())
                                    ctx_.nodes.lid_drain_qual_load[nd_idx] +=
                                        drain_cfs * c_src * (1.0 - rmvl);
                            }
                        }
                    }
                }
            }
        }

        // A6c. Accumulate runoff mass-balance totals now that the LID routing
        // has finalised subcatches.runoff (moved from A4b — see note there).
        accumulateRunoffMassBalance(dt_runoff);

        // A7. Street sweeping buildup removal (Gap #34)
        // Matches legacy surfqual_sweepBuildup(): per-(subcatch, landuse)
        // last-swept tracking so each subcatchment sweeps on its own schedule.
        {
            int np  = ctx_.n_pollutants();
            int nlu = ctx_.n_landuses();
            // IGNORE_QUALITY: skip street-sweeping buildup removal (runoff.c:274).
            if (np > 0 && nlu > 0 && !is_raining
                && !ctx_.options.ignore_quality) {
                int sweep_doy = datetime::dayOfYear(abs_time);
                int ss = ctx_.options.sweep_start;
                int se = ctx_.options.sweep_end;
                bool in_season = (ss <= se)
                    ? (sweep_doy >= ss && sweep_doy <= se)
                    : (sweep_doy >= ss || sweep_doy <= se);

                if (in_season) {
                    double dt_days = dt_runoff / ucf::SEC_PER_DAY;
                    int nlu_sz = static_cast<int>(ctx_.subcatches.sweep_last_swept.size())
                                 / std::max(ctx_.n_subcatches(), 1);

                    for (int i = 0; i < ctx_.n_subcatches(); ++i) {
                        auto ui = static_cast<std::size_t>(i);

                        // Skip subcatchment if snow on plowable impervious area.
                        // Matches legacy: if (snowpack->wsnow[IMPERV0] > MIN_TOTAL_DEPTH) return;
                        int sp = ctx_.subcatches.snowpack[ui];
                        if (sp >= 0) {
                            // snow_imperv_depth proxied via snow_net_imperv: skip if positive net
                            // (conservative check — full snow state tracked in Snow module)
                            if (ctx_.subcatches.snow_net_imperv[ui] > 0.0) continue;
                        }

                        for (int lu = 0; lu < nlu; ++lu) {
                            auto ulu = static_cast<std::size_t>(lu);
                            double interval = ctx_.landuses.sweep_interval[ulu];
                            if (interval <= 0.0) continue;

                            auto cov_idx = ui * static_cast<std::size_t>(nlu) + ulu;
                            double frac = (cov_idx < ctx_.subcatches.coverage.size())
                                          ? ctx_.subcatches.coverage[cov_idx] / 100.0 : 0.0;
                            if (frac <= 0.0) continue;

                            // Per-(subcatch, landuse) last_swept accumulator
                            auto sw_idx = ui * static_cast<std::size_t>(
                                              std::max(nlu_sz, nlu)) + ulu;
                            if (sw_idx >= ctx_.subcatches.sweep_last_swept.size()) continue;

                            ctx_.subcatches.sweep_last_swept[sw_idx] += dt_days;
                            if (ctx_.subcatches.sweep_last_swept[sw_idx] < interval) continue;
                            ctx_.subcatches.sweep_last_swept[sw_idx] = 0.0;

                            double removal_frac = ctx_.landuses.sweep_removal[ulu] / 100.0;
                            for (int p = 0; p < np; ++p) {
                                auto k = static_cast<std::size_t>(lu * np + p);
                                double effic = landuse_solver_.washoff_params[k].sweep_effic / 100.0;
                                auto bu = surface_quality_.bu_idx(i, lu, p);
                                const auto& bp = landuse_solver_.buildup_params[k];
                                double norm = (bp.normalizer == 0)
                                    ? frac * ctx_.subcatches.area[ui]
                                    : frac * ctx_.subcatches.curb_length[ui];
                                double old_bu = surface_quality_.buildup[bu];
                                double removed = old_bu * removal_frac * effic;
                                surface_quality_.buildup[bu] =
                                    std::max(old_bu - removed, 0.0);
                                ctx_.mass_balance.qual_sweeping[
                                    static_cast<std::size_t>(p)] += removed * norm;
                            }
                        }
                    }
                }
            }
        }

        // A8. Subcatchment-to-subcatchment + outfall runon → runon_inflow[]
        assembleRunon(dt_runoff);
    }

    // --- Phase 2: Interpolate runoff & GW to decomposed node inflow arrays ---
    // Matching legacy addWetWeatherInflows():
    //   f = (routingTime - OldRunoffTime) / (NewRunoffTime - OldRunoffTime)
    //   q = (1 - f) * oldRunoff + f * newRunoff
    // Writes to nodes.runoff_inflow[] and nodes.gw_inflow[] (assembled later).

    // PARITY: legacy addWetWeatherInflows (routing.c:703) forms the weight in
    // the MILLISECOND clock domain:
    //   f = (routingTime - OldRunoffTime) / (NewRunoffTime - OldRunoffTime)
    // where routingTime is OldRoutingTime (ms). ctx_.elapsed_ms is the exact
    // legacy OldRoutingTime here (advanced with elapsed_ms + 1000*dt in
    // TimestepController, matching routing.c:401). Forming f from the
    // seconds-domain clock rounds differently (~1e-11) and perturbs every
    // wet-weather lateral inflow.
    double span = new_runoff_ms_ - old_runoff_ms_;
    double f = (span > 0.0) ? (ctx_.elapsed_ms - old_runoff_ms_) / span : 1.0;
    if (f < 0.0) f = 0.0;
    if (f > 1.0) f = 1.0;

    // Zero decomposed node inflow arrays before accumulating from subcatchments.
    // Multiple subcatchments may drain to the same node, so we must use += below,
    // which requires a fresh zero each routing step (matching legacy initSystemInflows).
    std::fill(ctx_.nodes.runoff_inflow.begin(), ctx_.nodes.runoff_inflow.end(), 0.0);
    std::fill(ctx_.nodes.gw_inflow.begin(),     ctx_.nodes.gw_inflow.end(),     0.0);

    // PARITY: also record the PER-SUBCATCHMENT interpolated flows so
    // assembleLateralInflows() can replay legacy's per-node accumulation
    // order (routing.c adds each subcatchment's q into Node.newLatFlow
    // individually — pre-summed totals round differently).
    const auto un_sub = static_cast<std::size_t>(ctx_.n_subcatches());
    wet_q_interp_.assign(un_sub, 0.0);
    gw_q_interp_.assign(un_sub, 0.0);
    gw_q_node_.assign(un_sub, -1);

    for (int i = 0; i < ctx_.n_subcatches(); ++i) {
        auto ui = static_cast<std::size_t>(i);
        if (ctx_.subcatches.area[ui] <= 0.0) continue;

        // Interpolated runoff rate. RUN-ON IS NOT ADDED HERE: it is already
        // inside `runoff[]`.
        //
        // `assembleRunon` sums every run-on contributor into
        // `runon_inflow[]` — subcatchment-to-subcatchment, LID drain
        // (Gap #25) and outfall return (Gap #28) — and `Runoff.cpp:333`
        // consumes that array WHOLESALE as an inflow rate:
        // `precip += runon_q / total_area`. Whatever went in comes back out
        // in `runoff[]`, so adding `q_runon` again at the node booked the
        // same water twice.
        //
        // The per-contributor question is answered uniformly, and that is
        // why: the solver does not distinguish where a contribution came
        // from, it reads one lump. `total_area` there is `soa_.area[ui]`,
        // the same area this loop already guards on, so there is no
        // subcatchment that receives run-on and fails to consume it.
        //
        // Legacy is unambiguous — `subcatch_getWtdOutflow` is
        // `(1-f)*oldRunoff + f*newRunoff` and nothing else. Measured:
        // cascade 0.511 acre-feet against legacy's 0.218, three-deep 0.536
        // against 0.318; the excess on cascade is 0.293 against the donor's
        // own runoff of 0.294. The conveyance was receiving 2.3× what our
        // own runoff ledger said left the surface — and NEITHER continuity
        // check noticed, because each balance was self-consistent on its own
        // side of the seam.
        double q = (1.0 - f) * ctx_.subcatches.old_runoff[ui]
                 +        f  * ctx_.subcatches.runoff[ui];

        // Scatter to decomposed runoff inflow array
        int out_node = ctx_.subcatches.outlet_node[ui];
        if (out_node >= 0 && out_node < ctx_.n_nodes()) {
            ctx_.nodes.runoff_inflow[static_cast<std::size_t>(out_node)] += q;
            wet_q_interp_[ui] = q;
        }

        // Interpolated GW flow (matching legacy addGroundwaterInflows)
        int gw_node = ctx_.subcatches.gw_node[ui];
        if (gw_node < 0) gw_node = ctx_.subcatches.outlet_node[ui];
        if (gw_node >= 0 && gw_node < ctx_.n_nodes()) {
            double gw_q = (1.0 - f) * ctx_.subcatches.old_gw_flow[ui]
                        +        f  * ctx_.subcatches.gw_flow[ui];
            if (std::fabs(gw_q) > 1.0e-6) {
                ctx_.nodes.gw_inflow[static_cast<std::size_t>(gw_node)] += gw_q;
                gw_q_interp_[ui] = gw_q;
                gw_q_node_[ui]   = gw_node;
            }
        }
    }
}

// ============================================================================
// computeRunoffTimestep() — variable runoff timestep matching legacy
// ============================================================================

/**
 * @brief Compute variable runoff timestep matching legacy runoff_getTimeStep().
 *
 * @details Selects wet_step or dry_step based on current conditions, then
 *          shortens to align with next rain gage boundary.
 *
 * @param abs_time     Current absolute OADate (days since 12/30/1899).
 * @param is_raining   True if any gage has rainfall > 0.
 * @param has_runoff   True if any subcatchment produces runoff > 0.
 * @param has_snow     True if any subcatchment has snow depth > 0.
 * @returns Runoff timestep in seconds.
 */
double SWMMEngine::computeRunoffTimestep(double abs_time, bool is_raining,
                                         bool has_runoff, bool has_snow) noexcept {
    // Matches legacy runoff_getTimeStep() exactly:
    //   1. Start with maxStep = dry_step
    //   2. Shorten to next evaporation change date
    //   3. Shorten to next rain gage boundary
    //   4. Choose wet_step or dry_step based on conditions
    //   5. Return min(wet/dry step, maxStep)
    long max_step = static_cast<long>(ctx_.options.dry_step);

    // Shorten to next rain gage boundary
    // (matching legacy gage_getNextRainDate)
    for (int g = 0; g < ctx_.n_gages(); ++g) {
        auto ug = static_cast<std::size_t>(g);

        // Select the gage's rain series the same way updateAllGages() does:
        // a FILE_RAIN gage reads from its own resolved rain_series (built by
        // load_external_rain_files and NOT present in the shared tables pool),
        // while a TIMESERIES gage reads from tables[ts_index].  Using only
        // ts_index here skipped every file gage (ts_index == -1), so the runoff
        // step was never shortened to a file gage's rain-interval boundary —
        // dry_step then overshot rain-burst onsets and dropped rainfall.
        // Matches legacy gage_getNextRainDate(), which snaps for all gages.
        Table* rtbl = nullptr;
        int ts_idx = ctx_.gages.ts_index[ug];
        if (ctx_.gages.source[ug] == RainSource::FILE_RAIN) {
            if (ug < ctx_.gages.rain_series.size() &&
                !ctx_.gages.rain_series[ug].empty())
                rtbl = &ctx_.gages.rain_series[ug];
        } else if (ts_idx >= 0 && ts_idx < static_cast<int>(ctx_.tables.tables.size())) {
            rtbl = &ctx_.tables.tables[static_cast<std::size_t>(ts_idx)];
        }
        if (!rtbl) continue;
        auto& tbl = *rtbl;
        int idx = tbl.cursor.index;
        int n = static_cast<int>(tbl.x.size());
        if (idx < 0 || idx >= n) continue;

        // gage_getNextRainDate logic:
        // - If before startDate: return startDate
        // - If before endDate: return endDate
        // - Otherwise: return nextDate
        double interval_sec = ctx_.gages.interval_sec[ug];
        double entry_start = tbl.x[static_cast<std::size_t>(idx)];
        double entry_end = datetime::addSeconds(entry_start, interval_sec);
        double t_shifted = abs_time + datetime::OneSecond;

        double next_rain_date;
        if (t_shifted < entry_start) {
            next_rain_date = entry_start;
        } else if (t_shifted < entry_end) {
            next_rain_date = entry_end;
        } else if (idx + 1 < n) {
            next_rain_date = tbl.x[static_cast<std::size_t>(idx + 1)];
        } else {
            continue; // No more data
        }

        long secs_to_change = datetime::timeDiff(next_rain_date, abs_time);
        if (secs_to_change > 0 && secs_to_change < max_step)
            max_step = secs_to_change;
    }

    // Choose wet or dry step based on conditions
    long time_step;
    if (is_raining || has_snow || has_runoff)
        time_step = static_cast<long>(ctx_.options.wet_step);
    else
        time_step = static_cast<long>(ctx_.options.dry_step);

    // Limit to max_step (alignment constraint)
    if (time_step > max_step)
        time_step = max_step;

    return static_cast<double>(time_step);
}

// ============================================================================
// accumulateRunoffMassBalance() — runoff mass balance for one substep
// ============================================================================

/**
 * @brief Accumulate runoff mass balance totals for one substep.
 *
 * @details Sums rainfall, evaporation, infiltration, and runoff volumes
 *          over all subcatchments for a single runoff substep. Also updates
 *          per-subcatchment statistics (max runoff, precipitation volume,
 *          runoff volume).
 *
 * @param dt_runoff  Runoff substep duration (seconds).
 */
void SWMMEngine::accumulateRunoffMassBalance(double dt_runoff) noexcept {
    //      Sum over all subcatchments for this runoff substep
    //
    //      Units:
    //        rainfall   = in/hr (project rain units)
    //        evap_loss  = ft/sec (depth rate on pervious)
    //        infil_loss = ft/sec (depth rate on pervious)
    //        runoff     = cfs (flow)
    //        area       = acres
    //
    //      Volume conversions:
    //        rain_vol (ft³) = rainfall(in/hr) / UCF(RAINFALL) * area(ac) * ACRES_TO_FT2 * dt(sec)
    //        infil_vol(ft³) = infil_rate(ft/sec) * area(ac) * ACRES_TO_FT2 * dt(sec)
    //        runoff_vol(ft³)= runoff(cfs) * dt(sec)
    //
    const double RAIN_TO_FTSEC = 1.0 / ucf::UCF(ucf::RAINFALL, ctx_.options);
    // Subcatchment area is stored in project land-area units (acres for US,
    // hectares for SI).  Convert to internal ft² via 1/UCF(LANDAREA) so SI
    // models scale by 107639 (ha→ft²), not 43560 (ac→ft²).  The surface-runoff
    // term below is a routed cfs flow that already used the correct ft² area,
    // so without this the precip/infil volumes were 2.471× too small for SI,
    // breaking the runoff continuity balance.
    const double LANDAREA_TO_FT2 = 1.0 / ucf::UCF(ucf::LANDAREA, ctx_.options);

    const auto& rsoa = runoff_.soa();
    for (int i = 0; i < ctx_.n_subcatches(); ++i) {
        auto ui = static_cast<std::size_t>(i);
        double area_ft2 = ctx_.subcatches.area[ui] * LANDAREA_TO_FT2;
        // evap_loss / infil_loss are averaged over the NON-LID area only
        // (RunoffSolver divides Vevap/Vinfil by soa.area = full − LID area).
        // Multiplying them by the full subcatchment area double-counts the
        // loss when LIDs cover part of the subcatchment (issue #102 —
        // exposed once the LID footprint is actually removed, fix B).
        double nonlid_area_ft2 = rsoa.area[ui];  // ft²

        // Rainfall volume (ft³) — falls on the whole subcatchment (incl. LID)
        double rain_ftsec = ctx_.subcatches.rainfall[ui];
        ctx_.mass_balance.runoff_rainfall += rain_ftsec * area_ft2 * dt_runoff;

        // Evaporation volume (ft³) — evap_loss is a non-LID-area-averaged rate
        ctx_.mass_balance.runoff_evap +=
            ctx_.subcatches.evap_loss[ui] * nonlid_area_ft2 * dt_runoff;

        // Infiltration volume (ft³) — infil_loss is a non-LID-area-averaged rate
        ctx_.mass_balance.runoff_infil +=
            ctx_.subcatches.infil_loss[ui] * nonlid_area_ft2 * dt_runoff;

        // Surface runoff volume (ft³) — runoff is cfs.
        //
        // ONLY when the outlet is a drainage-system node. A subcatchment that
        // sheds onto another subcatchment has not left the runoff system: its
        // water arrives as run-on and is counted again when the receiver
        // discharges. Adding it here books the same water twice and the
        // ledger reads a system output that never happened — measured on
        // tests/parity/transport/age_legacy.inp, whose S1 drains onto S2:
        // runoff continuity −23.667 % against legacy's −0.271 %, precipitation
        // and infiltration agreeing to the digit and only runoff differing,
        // by S1's own 1.628 in.
        //
        // Legacy guards exactly this (subcatch.c:761-765): `outNode == -1 &&
        // outSubcatch != subcatchIndex` zeroes vOutflow before
        // massbal_updateRunoffTotals. The self-outlet exclusion matters —
        // a subcatchment routed to ITSELF is a real system output, not a
        // cascade.
        //
        // This is a LEDGER term only: runoff_runoff is read by the continuity
        // total, the report row and the mass-balance API, and by nothing that
        // routes water. The routed hydrology and every .out column are
        // unaffected.
        const bool sheds_to_node = ctx_.subcatches.outlet_node[ui] >= 0;
        const bool sheds_to_self =
            ctx_.subcatches.outlet_subcatch[ui] == i;
        if (sheds_to_node || sheds_to_self) {
            ctx_.mass_balance.runoff_runoff +=
                ctx_.subcatches.runoff[ui] * dt_runoff;
        }

        // Subcatchment-level statistics
        if (ctx_.subcatches.runoff[ui] > ctx_.subcatches.stat_max_runoff[ui])
            ctx_.subcatches.stat_max_runoff[ui] = ctx_.subcatches.runoff[ui];
        ctx_.subcatches.stat_precip_vol[ui] +=
            rain_ftsec * area_ft2 * dt_runoff;
        ctx_.subcatches.stat_runoff_vol[ui] +=
            ctx_.subcatches.runoff[ui] * dt_runoff;
    }

    // LID unit losses belong in the runoff mass balance — legacy
    // evalLidUnit() adds lidEvap/lidInfil × lidArea to the runoff totals
    // (lid.c:1912-1913). Without them, rain captured by LID units reads as
    // a runoff continuity error once the LID footprint is excluded from
    // runoff generation (issue #131). evap_loss/infil_loss are per-step
    // loss depths (ft).
    for (int t = 0; t < lid_.numGroups(); ++t) {
        const auto& g = lid_.group(t);
        for (int u = 0; u < g.count; ++u) {
            auto uu = static_cast<std::size_t>(u);
            ctx_.mass_balance.runoff_evap  += g.evap_loss[uu] * g.area[uu];
            ctx_.mass_balance.runoff_infil += g.infil_loss[uu] * g.area[uu];
        }
    }

    // LID drain flow discharged to a NODE has left the runoff system — the
    // legacy RUNOFF_DRAINS term (evalLidUnit books it only when
    // drainNode >= 0; drains onto another subcatchment stay internal and are
    // counted when the receiver sheds). nodes.lid_drain_inflow holds exactly
    // that set for this runoff step. Without this term the ledger sprang a
    // hole the size of the drain the moment the water stopped riding
    // subcatches.runoff (LID fix round, 2026-08-30).
    double lid_drain_cfs = 0.0;
    for (double q : ctx_.nodes.lid_drain_inflow) lid_drain_cfs += q;
    ctx_.mass_balance.runoff_lid_drain += lid_drain_cfs * dt_runoff;
}

// ============================================================================
// refreshLanduseParams() — re-derive the buildup/washoff parameter cache
// ============================================================================

/**
 * @brief Re-derive the land-use buildup/washoff parameter cache from the live
 *        context (BuildupData/WashoffData).
 *
 * @details Used both during start-up transfer and by the C API runtime setters
 * (swmm_buildup_set / swmm_washoff_set), which mutate ctx.buildup/ctx.washoff;
 * the per-step path reads landuse_solver_.buildup_params/washoff_params, so a
 * mid-run edit needs this refresh to take effect on the next step. The
 * accumulated buildup pool (surface_quality_.buildup) is left untouched.
 */
void SWMMEngine::refreshLanduseParams() noexcept {
    int np  = ctx_.n_pollutants();
    int nlu = ctx_.n_landuses();
    if (np <= 0 || nlu <= 0) return;
    if (static_cast<int>(landuse_solver_.buildup_params.size()) != nlu * np)
        return;
    for (int lu = 0; lu < nlu; ++lu) {
        for (int p = 0; p < np; ++p) {
            auto k = static_cast<std::size_t>(lu * np + p);
            auto& bp = landuse_solver_.buildup_params[k];
            bp.type = static_cast<landuse::BuildupType>(ctx_.buildup.func_type[k]);
            bp.coeff[0] = ctx_.buildup.coeff1[k];  // max buildup
            bp.coeff[1] = ctx_.buildup.coeff2[k];  // rate constant
            bp.coeff[2] = ctx_.buildup.coeff3[k];  // exponent/half-sat
            bp.normalizer = ctx_.buildup.normalizer[k];
            bp.max_days = 0.0;
            // Compute max_days: time to reach 99.9% of max buildup
            if (bp.type == landuse::BuildupType::EXPON && bp.coeff[1] > 0.0)
                bp.max_days = -std::log(0.001) / bp.coeff[1];
            else if (bp.type == landuse::BuildupType::POWER && bp.coeff[1] > 0.0
                     && bp.coeff[2] > 0.0 && bp.coeff[0] > 0.0)
                bp.max_days = std::pow(bp.coeff[0] / bp.coeff[1], 1.0 / bp.coeff[2]);
            else if (bp.type == landuse::BuildupType::SATUR && bp.coeff[2] > 0.0
                     && bp.coeff[0] > 0.0)
                bp.max_days = 999.0 * bp.coeff[2];  // asymptotic

            auto& wp = landuse_solver_.washoff_params[k];
            wp.type = static_cast<landuse::WashoffType>(ctx_.washoff.func_type[k]);
            wp.coeff = ctx_.washoff.coeff[k];
            wp.expon = ctx_.washoff.expon[k];
            wp.sweep_effic = ctx_.washoff.sweep_effic[k];
            wp.bmp_effic = ctx_.washoff.bmp_effic[k];
        }
    }
}

// ============================================================================
// refreshTreatment() — recompile one treatment expression cell
// ============================================================================

/**
 * @brief Recompile one (node, pollutant) treatment expression from the live
 *        context and refresh the per-node has-treatment flag.
 *
 * @details The step loop (QualitySolver::applyTreatment) evaluates the
 * compiled cache (`ctx.treatment.compiled` / `has_treatment`), which
 * initQuality() builds once at start. The C API setters
 * (swmm_treatment_set/_clear) mutate the expression string only, so they call
 * this to keep the cache coherent — a mid-run treatment edit takes effect on
 * the next step. An empty expression clears the cell. Mirrors the
 * initQuality() compile (plain parse + pollutant_idx tag); the start-up cyclic
 * co-treatment check (Gap #85) is not re-run for runtime edits.
 *
 * @return 0 on success, or the nonzero treatment::parse error code (the cell
 *         is left cleared so a bad edit cannot leave a stale expression live).
 */
int SWMMEngine::refreshTreatment(int node_idx, int pollut_idx) noexcept {
    int np = ctx_.n_pollutants();
    int nn = ctx_.n_nodes();
    if (np <= 0 || nn <= 0) return 0;
    if (node_idx < 0 || node_idx >= nn || pollut_idx < 0 || pollut_idx >= np)
        return 0;
    if (ctx_.treatment.n_nodes != nn || ctx_.treatment.n_pollutants != np)
        ctx_.treatment.resize(nn, np);

    auto idx = static_cast<std::size_t>(node_idx * np + pollut_idx);
    const auto& expr_str = ctx_.treatment.expressions[idx];
    int rc = 0;
    if (expr_str.empty()) {
        ctx_.treatment.compiled[idx] = treatment::TreatExpr{};
    } else {
        treatment::TreatExpr te;
        rc = treatment::parse(expr_str, te);
        if (rc == 0) {
            te.pollutant_idx = pollut_idx;
            ctx_.treatment.compiled[idx] = std::move(te);
        } else {
            ctx_.treatment.compiled[idx] = treatment::TreatExpr{};
        }
    }

    bool any = false;
    for (int p = 0; p < np && !any; ++p) {
        auto k = static_cast<std::size_t>(node_idx * np + p);
        any = !ctx_.treatment.compiled[k].tokens.empty();
    }
    ctx_.treatment.has_treatment[static_cast<std::size_t>(node_idx)] = any;
    return rc;
}

// ============================================================================
// refreshLIDDrainParams() — re-copy drain coefficients into the LID solver
// ============================================================================

/**
 * @brief Re-copy the drain-layer coefficients from the live context into the
 *        LID solver's per-unit parameter columns.
 *
 * @details The step loop reads the per-unit copies that LIDSolver::init()
 * makes from ctx.lid_controls.drain at start, so a swmm_lid_set_drain edit
 * would otherwise be silently inert mid-run. Mirrors the init() drain-layer
 * transfer for every unit (a handful of values; no per-unit state is touched,
 * so this is safe while the simulation is running).
 */
void SWMMEngine::refreshLIDDrainParams() noexcept {
    const auto& drain = ctx_.lid_controls.drain;
    // Mirror the LID init() unit conversions (issue #102): coeff/expon stay in
    // user units (converted at compute time by getDrainRate); the head-based
    // columns convert in|mm → ft and the delay converts hours → seconds.
    const double ucfRainDepth = ucf::UCF(ucf::RAINDEPTH, ctx_.options);
    for (int t = 0; t < lid_.numGroups(); ++t) {
        auto& g = lid_.group(t);
        for (int i = 0; i < g.count; ++i) {
            auto ui = static_cast<std::size_t>(i);
            int li = g.control_idx[ui];
            if (li < 0 || static_cast<std::size_t>(li) >= drain.size()) continue;
            const auto& p = drain[static_cast<std::size_t>(li)];
            g.drain_coeff[ui]  = p[0];
            g.drain_expon[ui]  = p[1];
            g.drain_offset[ui] = p[2] / ucfRainDepth;
            g.drain_delay[ui]  = p[3] * 3600.0;
            g.drain_hopen[ui]  = p[4] / ucfRainDepth;
            g.drain_hclose[ui] = p[5] / ucfRainDepth;
        }
    }
}

// ============================================================================
// refreshAquiferParams() — re-derive GW flux-coefficient columns
// ============================================================================

/**
 * @brief Re-derive the groundwater solver's per-subcatchment flux-coefficient
 *        columns from the live context aquifers.
 *
 * @details The GW solver reads per-subcatchment copies of the aquifer
 * parameters made once at start (see the init transfer in initHydrology);
 * swmm_aquifer_set_param writes the context store, so mid-run edits of the
 * runtime-editable parameters call this to keep the copies coherent. Mirrors
 * the start-up transfer (same unit conversions) for the flux coefficients
 * only — the structural columns (porosity, field capacity, wilting point,
 * total depth) are pre-start-only and the GW state (theta, lower_depth) is
 * never touched.
 */
void SWMMEngine::refreshAquiferParams() noexcept {
    auto& gw = groundwater_.state();
    if (gw.k_sat.empty()) return;  // GW solver not initialized yet
    int unit_sys = ucf::getUnitSystem(static_cast<int>(ctx_.options.flow_units));
    for (int i = 0; i < ctx_.n_subcatches(); ++i) {
        auto ui = static_cast<std::size_t>(i);
        int aq_idx = ctx_.subcatches.gw_aquifer[ui];
        if (aq_idx < 0) continue;
        auto uaq = static_cast<std::size_t>(aq_idx);

        gw.k_sat[ui]            = ctx_.aquifers.conductivity[uaq]
                                  / ucf::Ucf[ucf::RAINFALL][unit_sys];
        gw.k_slope[ui]          = ctx_.aquifers.conduct_slope[uaq];
        gw.tension_slope[ui]    = ctx_.aquifers.tension_slope[uaq]
                                  / ucf::Ucf[ucf::LENGTH][unit_sys];
        gw.upper_evap_frac[ui]  = ctx_.aquifers.upper_evap[uaq];
        gw.lower_evap_depth[ui] = ctx_.aquifers.lower_evap[uaq]
                                  / ucf::Ucf[ucf::LENGTH][unit_sys];
        gw.lower_loss_coeff[ui] = ctx_.aquifers.lower_loss[uaq]
                                  / ucf::Ucf[ucf::RAINFALL][unit_sys];
    }
}

// ============================================================================
// stepSurfaceQuality() — surface quality buildup + washoff for one substep
// ============================================================================

/**
 * @brief Compute surface quality buildup and washoff for one substep.
 *
 * @details Matching legacy surfqual_getBuildup + surfqual_getWashoff.
 *          Iterates over subcatchments, pollutants, and land uses to
 *          accumulate buildup and compute washoff loads.
 *
 * @param dt_runoff  Runoff substep duration (seconds).
 */
void SWMMEngine::stepSurfaceQuality(double dt_runoff) noexcept {
    //      Matching legacy surfqual_getBuildup + surfqual_getWashoff
    if (ctx_.n_pollutants() > 0 && ctx_.n_landuses() > 0) {
        int np = ctx_.n_pollutants();
        int nlu = ctx_.n_landuses();
        constexpr double MIN_RUNOFF_RATE = 1.0e-9; // ft/sec threshold
        double dt_days = dt_runoff / ucf::SEC_PER_DAY;

        for (int i = 0; i < ctx_.n_subcatches(); ++i) {
            auto ui = static_cast<std::size_t>(i);
            double q = ctx_.subcatches.runoff[ui];  // cfs
            double area_ac = ctx_.subcatches.area[ui]; // acres

            for (int p = 0; p < np; ++p) {
                auto sq_idx = ui * static_cast<std::size_t>(np)
                              + static_cast<std::size_t>(p);
                // THE UNIT CONVENTION, fixed 2026-08-23 after the known-mass
                // audit (QUALITY_LEDGER_UNITS_AUDIT §7): total_washoff_load
                // accumulates in CONCENTRATION MASS UNITS per second (mg/s,
                // or µg/s, or counts/s — whatever the pollutant's units are),
                // and every LEDGER booking converts to USER MASS (lbs/kg) at
                // the seam via mcf_p, exactly as legacy applies Pollut[].mcf
                // at source (landuse.c:585, surfqual.c:352/357/366). Before
                // this, the accumulator mixed three unit systems — EMC in
                // mg/L·ft³/s, EXPON in user-mass/s, RATING in mg/s — and the
                // ledger row printed 16057× legacy on a 100 mg/L EMC deck
                // while the Washoff Summary printed 1/28.3 of it.
                //
                // mcf_p mirrors legacy landuse.c:167-169: UCF(MASS) for mg,
                // /1000 for µg, 1.0 for counts.
                const double mass_ucf_p = ucf::UCF(ucf::MASS, ctx_.options);
                double mcf_p = mass_ucf_p;
                if (static_cast<std::size_t>(p) < ctx_.pollutants.units.size()) {
                    switch (ctx_.pollutants.units[static_cast<std::size_t>(p)]) {
                        case MassUnits::UG_PER_L:     mcf_p = mass_ucf_p / 1000.0; break;
                        case MassUnits::COUNTS_PER_L: mcf_p = 1.0; break;
                        default: break;
                    }
                }
                constexpr double kLperFt3 = 28.317;
                double total_washoff_load = 0.0; // concen. mass units / sec

                // Iterate over land uses weighted by coverage
                // Buildup is now stored PER LAND USE: bu_idx(i, lu, p)
                for (int lu = 0; lu < nlu; ++lu) {
                    auto cov_idx = ui * static_cast<std::size_t>(nlu)
                                   + static_cast<std::size_t>(lu);
                    double frac = (cov_idx < ctx_.subcatches.coverage.size())
                                  ? ctx_.subcatches.coverage[cov_idx] / 100.0 : 0.0;
                    if (frac <= 0.0) continue;

                    auto k = static_cast<std::size_t>(lu * np + p);
                    const auto& bp = landuse_solver_.buildup_params[k];
                    const auto& wp = landuse_solver_.washoff_params[k];
                    auto bu = surface_quality_.bu_idx(i, lu, p);

                    // Normalizer for this land use (absolute mass = buildup * norm)
                    double norm = (bp.normalizer == 0)
                        ? frac * area_ac : frac * ctx_.subcatches.curb_length[ui];

                    // --- Buildup accumulation (matching legacy surfqual_getBuildup)
                    if (bp.type != landuse::BuildupType::NONE) {
                        double mass = surface_quality_.buildup[bu];  // per-normalizer-unit

                        // Gap #35: EXTERNAL buildup uses a time series for rate.
                        // Matches legacy landuse_getExternalBuildup():
                        //   rate = sf * tseries_lookup(ts, time)
                        //   buildup = MIN(buildup + rate * dt_days, maxBuildup)
                        if (bp.type == landuse::BuildupType::EXTERNAL) {
                            double max_bu = bp.coeff[0];
                            double sf     = bp.coeff[1];   // scaling factor
                            int ts_idx    = static_cast<int>(bp.coeff[2]);
                            double rate   = 0.0;
                            if (ts_idx >= 0 &&
                                ts_idx < static_cast<int>(ctx_.tables.tables.size())) {
                                double abs_t = datetime::addSeconds(
                                    ctx_.options.start_date, old_runoff_time_);
                                rate = sf * table_lookup_cursor(
                                    ctx_.tables.tables[static_cast<std::size_t>(ts_idx)],
                                    abs_t);
                            }
                            double new_mass = std::min(mass + rate * dt_days, max_bu);
                            new_mass = std::max(new_mass, 0.0);
                            double buildup_change = new_mass - mass;
                            if (buildup_change != 0.0) {
                                surface_quality_.buildup[bu] = new_mass;
                                ctx_.mass_balance.qual_surface_buildup[
                                    static_cast<std::size_t>(p)] += buildup_change * norm;
                            }
                            continue;
                        }

                        double days = 0.0;
                        double c0 = bp.coeff[0], c1 = bp.coeff[1], c2 = bp.coeff[2];
                        // Inverse: mass → equivalent days
                        if (mass > 0.0) {
                            switch (bp.type) {
                                case landuse::BuildupType::POWER:
                                    days = (c1 * c2 > 0.0) ? std::pow(mass / c1, 1.0 / c2) : 0.0;
                                    break;
                                case landuse::BuildupType::EXPON:
                                    days = (c0 * c1 > 0.0 && mass < c0) ? -std::log(1.0 - mass / c0) / c1 : 0.0;
                                    break;
                                case landuse::BuildupType::SATUR:
                                    days = (c0 > mass) ? mass * c2 / (c0 - mass) : bp.max_days;
                                    break;
                                default: break;
                            }
                        }
                        days += dt_days;
                        // Forward: days → new mass
                        double new_mass = 0.0;
                        if (days > 0.0) {
                            switch (bp.type) {
                                case landuse::BuildupType::POWER:
                                    new_mass = std::min(c1 * std::pow(days, c2), c0);
                                    break;
                                case landuse::BuildupType::EXPON:
                                    new_mass = c0 * (1.0 - std::exp(-c1 * days));
                                    break;
                                case landuse::BuildupType::SATUR:
                                    new_mass = (c2 + days > 0.0) ? c0 * days / (c2 + days) : 0.0;
                                    break;
                                default: break;
                            }
                        }
                        double buildup_change = new_mass - mass;
                        surface_quality_.buildup[bu] = new_mass;

                        // Track net buildup increase for mass balance
                        if (buildup_change > 0.0) {
                            ctx_.mass_balance.qual_surface_buildup[
                                static_cast<std::size_t>(p)] += buildup_change * norm;
                        }
                    }

                    // --- Washoff computation (matching legacy surfqual_getWashoff)
                    if (wp.type != landuse::WashoffType::NONE && q > MIN_RUNOFF_RATE) {
                        double buildup = surface_quality_.buildup[bu];

                        // Unit conversions matching legacy landuse_getWashoffLoad:
                        //   EXPON uses unit runoff in display units (in/hr or mm/hr):
                        //     q_expon = (q_cfs / area_ft2) * UCF(RAINFALL)
                        //   RATING and EMC use volumetric flow in display units:
                        //     q_flow = q_cfs * UCF(FLOW)
                        double area_ft2 = area_ac * 43560.0;
                        double q_expon  = (area_ft2 > 0.0)
                            ? (q / area_ft2) * ucf::UCF(ucf::RAINFALL, ctx_.options)
                            : 0.0;
                        double q_flow   = q * ucf::UCF(ucf::FLOW, ctx_.options);

                        // `load` is in CONCENTRATION MASS UNITS per second
                        // (mg/s for a mg/L pollutant). Each branch mirrors
                        // legacy landuse_getWashoffQual × Q, with the
                        // parse-time coefficient pre-multiplies
                        // (landuse.c:332-334) applied here at runtime instead,
                        // because our parser stores the user coefficient raw:
                        //
                        //   EMC:    coeff[mg/L] × LperFT3 → mg/ft³, × Q[cfs].
                        //           Legacy uses INTERNAL flow here, not
                        //           UCF(FLOW) — the previous q_flow form was
                        //           inert under CFS and wrong under any other
                        //           flow unit.
                        //   EXPON:  coeff is PER HOUR in the deck; legacy
                        //           divides by 3600 at parse. Without it the
                        //           load was 3600× the formulation. Buildup
                        //           is stored in user mass, so /mcf brings it
                        //           to concentration mass units (legacy:
                        //           `buildup / Pollut[p].mcf`).
                        //   RATING: coeff × (Q·UCF(FLOW))^expon is already
                        //           legacy's coeff·UCF^e × Q_int^e — unchanged.
                        double load = 0.0; // concen. mass units / sec
                        switch (wp.type) {
                            case landuse::WashoffType::EMC:
                                load = wp.coeff * kLperFt3 * q * frac;
                                break;
                            case landuse::WashoffType::EXPON:
                                if (buildup > 0.0 && mcf_p > 0.0)
                                    load = (wp.coeff / 3600.0)
                                         * std::pow(q_expon, wp.expon)
                                         * (buildup * norm) / mcf_p;
                                break;
                            case landuse::WashoffType::RATING:
                                load = wp.coeff * std::pow(q_flow, wp.expon) * frac;
                                break;
                            default: break;
                        }

                        // Cap washoff to available buildup. Buildup is user
                        // mass; the cap must be in the SAME units as `load`
                        // (concen. mass), or the comparison is the mixed-unit
                        // defect all over again — before this fix the EMC/
                        // RATING mg-based load was compared against a lbs cap.
                        double avail = (mcf_p > 0.0)
                            ? (buildup * norm) / mcf_p : 0.0;
                        double max_load = (dt_runoff > 0.0) ? avail / dt_runoff : 0.0;
                        if (load > max_load && wp.type != landuse::WashoffType::EMC)
                            load = max_load;

                        // Reduce per-landuse buildup by washoff amount —
                        // buildup is user mass per normalizer unit, so the
                        // reduction converts back (legacy reduces buildup by
                        // washoffLoad, which is already lbs).
                        //
                        // ...unless there is NO buildup function and the
                        // washoff exceeds what is on the ground, in which
                        // case legacy books the load as BUILDUP_LOAD instead
                        // (landuse.c:585-593, "otherwise add washoff to
                        // buildup mass balance totals so that things will
                        // balance"). Without that branch an EMC-only deck
                        // discharges mass the ledger never received: the
                        // Runoff Quality Continuity block reads Surface
                        // Buildup 0.000 against Surface Runoff 113.269 where
                        // legacy reads 113.082 on both. It is also what makes
                        // the printed continuity error meaningful rather than
                        // merely non-zero.
                        const double washed_mass = load * mcf_p * dt_runoff;
                        const bool has_buildup_fn =
                            bp.type != landuse::BuildupType::NONE;
                        if (!has_buildup_fn && washed_mass > buildup * norm) {
                            auto upb2 = static_cast<std::size_t>(p);
                            if (upb2 < ctx_.mass_balance.qual_surface_buildup.size())
                                ctx_.mass_balance.qual_surface_buildup[upb2] +=
                                    washed_mass;
                            surface_quality_.buildup[bu] = 0.0;
                        } else if (norm > 0.0) {
                            double washed = washed_mass / norm;
                            surface_quality_.buildup[bu] =
                                std::max(surface_quality_.buildup[bu] - washed, 0.0);
                        }

                        // BMP removal — and BOOK it. qual_bmp_removal had no
                        // writer anywhere (Finding 8, the fourth
                        // rendered-but-never-written ledger row after the
                        // snapshot quality vectors, the snow rows and the
                        // subcatchment temperature column). Legacy books it at
                        // surfqual.c:352 in user mass.
                        double bmp_removed = load * (wp.bmp_effic / 100.0);
                        if (bmp_removed > 0.0) {
                            auto upb = static_cast<std::size_t>(p);
                            if (upb < ctx_.mass_balance.qual_bmp_removal.size())
                                ctx_.mass_balance.qual_bmp_removal[upb] +=
                                    bmp_removed * dt_runoff * mcf_p;
                            load -= bmp_removed;
                        }

                        total_washoff_load += load;
                    }
                } // end land use loop

                // Ponded quality tracking — Gap #36: complete-mix balance
                // Matches legacy findPondedLoads() in surfqual.c.
                // Mixes wet deposition with existing ponded mass; distributes
                // to infiltration and runoff outflow proportionally.
                if (sq_idx < ctx_.subcatches.ponded_qual.size()) {
                    constexpr double L_PER_FT3 = 28.317;  // liters per ft3
                    auto up = static_cast<std::size_t>(p);
                    double area_ft2_pq = area_ac * 43560.0;

                    // Rainfall at this subcatch (ft/sec)
                    double rrate = ctx_.gages.rainfall.empty() ? 0.0
                        : (ctx_.subcatches.gage[ui] >= 0 &&
                           ctx_.subcatches.gage[ui] < static_cast<int>(ctx_.gages.rainfall.size()))
                          ? ctx_.gages.rainfall[static_cast<size_t>(ctx_.subcatches.gage[ui])]
                          : 0.0;

                    // Volumes over dt (ft3)
                    double v_rain_pq    = rrate * area_ft2_pq * dt_runoff;
                    double v_runon_pq   = ctx_.subcatches.runon_inflow[ui] * dt_runoff;
                    double v_infil_pq   = ctx_.subcatches.infil_loss[ui] * area_ft2_pq * dt_runoff;
                    double v_outflow_pq = q * dt_runoff;

                    // Vinflow: rain + runon + existing ponded water volume
                    double v_ponded_pq  = ctx_.subcatches.ponded_depth[ui] * area_ft2_pq;
                    double v_inflow_pq  = v_rain_pq + v_runon_pq + v_ponded_pq;

                    // Wet deposition mass: c_rain[mg/L] * LperFT3[L/ft3] * v_rain[ft3]
                    double c_rain_pq = (up < ctx_.pollutants.c_rain.size())
                        ? ctx_.pollutants.c_rain[up] : 0.0;
                    double w_rain_pq = c_rain_pq * L_PER_FT3 * v_rain_pq;

                    // Accumulate wet deposition in mass balance — LEDGER
                    // booking, so convert to user mass at the seam (legacy
                    // books WET_DEPOSITION_LOAD in lbs/kg).
                    if (w_rain_pq > 0.0 && up < ctx_.mass_balance.qual_wet_deposition.size())
                        ctx_.mass_balance.qual_wet_deposition[up] += w_rain_pq * mcf_p;

                    double w_mass_pq = ctx_.subcatches.ponded_qual[sq_idx];  // mg

                    if (v_inflow_pq <= 0.0) {
                        // Dry surface. The `+=` into qual_final_buildup that
                        // used to sit here was DEAD (Finding 9):
                        // computeFinalQualityMassBalance overwrites the term
                        // with `=` at end-of-run, discarding every value
                        // accumulated here — and it was in mg against that
                        // site's user mass, so surviving would have corrupted
                        // the term. Removed rather than converted. The
                        // residual ponded mass at final time is NOT in the
                        // final-buildup term on either path; that matches the
                        // 4073 computation and is recorded as a small parity
                        // gap against legacy's FINAL_STORED_LOAD.
                        ctx_.subcatches.ponded_qual[sq_idx] = 0.0;
                    } else {
                        // Complete-mix balance (matching legacy findPondedLoads):
                        //   wRunon ≈ 0 (upstream subcatch concentrations not propagated here)
                        double w_total_pq  = w_mass_pq + w_rain_pq;
                        double c_ponded_pq = w_total_pq / v_inflow_pq;  // mg/ft3

                        // Mass lost to infiltration
                        double w_infil_pq = std::min(c_ponded_pq * v_infil_pq, w_total_pq);
                        w_total_pq -= w_infil_pq;
                        // Ledger booking → user mass (legacy INFIL_LOAD, lbs/kg)
                        if (w_infil_pq > 0.0 && up < ctx_.mass_balance.qual_infil_loss.size())
                            ctx_.mass_balance.qual_infil_loss[up] += w_infil_pq * mcf_p;

                        // Mass carried out with runoff outflow
                        double w_outflow_pq = std::min(c_ponded_pq * v_outflow_pq, w_total_pq);
                        w_total_pq -= w_outflow_pq;

                        // Add ponded outflow to total washoff load (mass/sec)
                        if (w_outflow_pq > 0.0 && dt_runoff > 0.0)
                            total_washoff_load += w_outflow_pq / dt_runoff;

                        // Update remaining ponded mass for next timestep
                        ctx_.subcatches.ponded_qual[sq_idx] = std::max(w_total_pq, 0.0);
                    }
                }

                // Gap #37: LID area wet deposition and runon quality loads.
                // Matches legacy findLidLoads() in surfqual.c.
                // Wet deposition falling on the LID surface contributes to outflow
                // quality. Runon quality loads are counted only when LIDs occupy the
                // full subcatchment (all runon arrives on the LID surface).
                if (ui < ctx_.subcatches.total_lid_area_ft2.size()) {
                    double lid_ft2 = ctx_.subcatches.total_lid_area_ft2[ui];
                    if (lid_ft2 > 0.0 && dt_runoff > 0.0) {
                        constexpr double L_PER_FT3_37 = 28.317;
                        // Rainfall rate for this subcatch (ft/sec)
                        int gi37 = ctx_.subcatches.gage[ui];
                        double rrate37 = (gi37 >= 0 &&
                                          gi37 < static_cast<int>(ctx_.gages.rainfall.size()))
                                          ? ctx_.gages.rainfall[static_cast<std::size_t>(gi37)]
                                          : 0.0;
                        auto up37 = static_cast<std::size_t>(p);
                        double c_rain37 = (up37 < ctx_.pollutants.c_rain.size())
                                          ? ctx_.pollutants.c_rain[up37] : 0.0;
                        // Wet deposition on LID area (mass/sec)
                        double v_lid_rain = rrate37 * lid_ft2 * dt_runoff;  // ft³
                        double w_lid_rain = c_rain37 * L_PER_FT3_37 * v_lid_rain;  // mg
                        if (w_lid_rain > 0.0)
                            total_washoff_load += w_lid_rain / dt_runoff;
                        // Runon quality (only when LIDs cover full subcatchment)
                        double full_ft2 = ctx_.subcatches.area[ui] * 43560.0;
                        if (std::fabs(lid_ft2 - full_ft2) < 1.0) {
                            double q_runon37 = ctx_.subcatches.runon_inflow[ui];  // CFS
                            double c_old37 = (sq_idx < ctx_.subcatches.conc_old.size())
                                             ? ctx_.subcatches.conc_old[sq_idx] : 0.0;
                            // conc_old is mg/L (the reported convention), so
                            // × LperFT3 brings cfs·mg/L·s to mg — legacy
                            // findLidLoads multiplies its runon term by
                            // LperFT3 for the same reason.
                            double w_lid_runon =
                                q_runon37 * c_old37 * L_PER_FT3_37 * dt_runoff;  // mg
                            if (w_lid_runon > 0.0)
                                total_washoff_load += w_lid_runon / dt_runoff;
                        }
                    }
                }

                // Convert washoff load to the REPORTED concentration, mg/L.
                // load/q is mass/ft³; ÷ LperFT3 gives mass/L, exactly
                // legacy's `newQual[p] = cOut / LperFT3` (surfqual.c:370).
                // For EMC this leaves the value where it already was (the
                // old form skipped LperFT3 in both load and conc, cancelling);
                // for EXPON and RATING it is a real correction — their
                // concentrations were in incompatible units before this.
                double conc = 0.0;
                if (q > MIN_RUNOFF_RATE && total_washoff_load > 0.0)
                    conc = total_washoff_load / q / kLperFt3;

                ctx_.subcatches.conc[sq_idx] = conc;

                // Update quality mass balance and per-subcatch total load.
                // ONE conversion, applied to BOTH bookings, so the Washoff
                // Summary and the continuity ledger can never again disagree
                // by a unit factor: `mass` is user mass (lbs/kg), matching
                // legacy's massLoad at surfqual.c:357/366. On the known-mass
                // deck (EMC 100 mg/L, V = 18157.174 ft³) this is
                // 100 × 18157.174 × 28.317 × 2.203e-6 ≈ 113.3 lbs against
                // legacy's 113.082 — where the old code booked 1 815 717.383.
                if (total_washoff_load > 0.0) {
                    double mass = total_washoff_load * dt_runoff * mcf_p;
                    // The LEDGER term is booked only when this subcatchment's
                    // load actually reaches the conveyance system — legacy's
                    // third `!= subcatchIndex` site (surfqual.c:363,
                    // `outNode >= 0 || outSubcatch == subcatchIndex`). A
                    // subcatchment shedding onto a peer has not delivered its
                    // load to the system; the receiver books it on discharge,
                    // and adding it here counts the same mass twice. The
                    // volumetric sibling of this is `421e95c2`.
                    const bool load_reaches_system =
                        (ctx_.subcatches.outlet_node[ui] >= 0 ||
                         ctx_.subcatches.outlet_subcatch[ui] == i);
                    if (load_reaches_system)
                        ctx_.mass_balance.qual_runoff_load[
                            static_cast<std::size_t>(p)] += mass;
                    // The PER-SUBCATCHMENT total is unconditional, as legacy's
                    // is (surfqual.c:356, above its own guard): it is what this
                    // subcatchment washed off, not what the system received.
                    ctx_.subcatches.total_load[sq_idx] += mass;
                }
            } // end pollutant loop
        } // end subcatch loop
    }
}

// ============================================================================
// stepGroundwater() — groundwater computation for one substep
// ============================================================================

/**
 * @brief Execute groundwater computation for one substep.
 *
 * @details Runs the groundwater ODE solver for all subcatchments, then
 *          scatters GW lateral flow contributions to receiving nodes
 *          and accumulates groundwater inflow mass balance.
 *
 * @param dt_runoff  Runoff substep duration (seconds).
 */
void SWMMEngine::stepGroundwater(double dt_runoff) noexcept {
    int ns = ctx_.n_subcatches();

    // GW surface water head and available node flow are pre-computed by
    // assembleGWCoupling() before this function is called.

    // Build per-subcatchment FracPerv and pervious evap rate
    // Legacy: FracPerv = subcatch_getFracPerv(j)
    //         MaxEvap = Evap.rate * FracPerv
    //         AvailEvap = max(MaxEvap - evap, 0)
    gw_frac_perv_.assign(static_cast<std::size_t>(ns), 0.0);
    gw_perv_evap_.assign(static_cast<std::size_t>(ns), 0.0);
    for (int i = 0; i < ns; ++i) {
        auto ui = static_cast<std::size_t>(i);
        double total_area = ctx_.subcatches.area[ui] * ucf::ACRES_TO_FT2;
        if (total_area <= 0.0) continue;
        double frac_perv = 1.0 - ctx_.subcatches.frac_imperv[ui];
        gw_frac_perv_[ui] = std::max(frac_perv, 0.0);
        gw_perv_evap_[ui] = ctx_.subcatches.evap_loss[ui];
    }

    // Pass actual infiltration rate to groundwater (upper zone percolation input).
    // sw_head is read from the pre-assembled subcatches.gw_sw_head[].
    groundwater_.execute(ctx_, dt_runoff, ctx_.climate_state.evap_rate,
                         ctx_.subcatches.infil_loss.data(),
                         ctx_.subcatches.gw_sw_head.data(),
                         gw_frac_perv_.data(), gw_perv_evap_.data());

    // Store GW flow rates in subcatch state for Phase 2 interpolation.
    // GW flow is in ft/sec (rate per unit area).
    // Convert to CFS: q_cfs = gw_flow * area_ft2
    // Note: do NOT scatter to lat_flow here — Phase 2 does interpolated scatter.
    // Note: do NOT accumulate routing_gw_inflow here — updateRoutingMassBalance does it.
    for (int i = 0; i < ns; ++i) {
        auto ui = static_cast<std::size_t>(i);
        double gw_rate = groundwater_.state().gw_flow[ui]; // ft/sec
        double area_ft2 = ctx_.subcatches.area[ui] * ucf::ACRES_TO_FT2;
        ctx_.subcatches.gw_flow[ui] = gw_rate * area_ft2; // CFS

        // Gap #40: propagate max infiltration volume to SubcatchData for next
        // runoff step.  If no GW is active, leave the default (DBL_MAX = no cap).
        if (ctx_.subcatches.gw_aquifer[ui] >= 0) {
            ctx_.subcatches.gw_max_infil_vol[ui] =
                groundwater_.state().max_infil_vol[ui];
        }
    }

    // Gap #63: Accumulate per-subcatch GW statistics.
    // All GWSoA outputs are ft/sec (per unit area); multiply by area*dt for ft³.
    const auto& gws = groundwater_.state();
    for (int i = 0; i < ns; ++i) {
        auto ui = static_cast<std::size_t>(i);
        if (ctx_.subcatches.gw_aquifer[ui] < 0) continue;
        double area_ft2 = ctx_.subcatches.area[ui] * ucf::ACRES_TO_FT2;
        ctx_.subcatches.stat_gw_infil_vol[ui]      += ctx_.subcatches.infil_loss[ui] * area_ft2 * dt_runoff;
        ctx_.subcatches.stat_gw_upper_evap_vol[ui] += gws.upper_evap[ui]  * area_ft2 * dt_runoff;
        ctx_.subcatches.stat_gw_lower_evap_vol[ui] += gws.lower_evap[ui]  * area_ft2 * dt_runoff;
        ctx_.subcatches.stat_gw_deep_perc_vol[ui]  += gws.deep_loss[ui]   * area_ft2 * dt_runoff;
        double flow_cfs = ctx_.subcatches.gw_flow[ui]; // CFS (already computed above)
        ctx_.subcatches.stat_gw_flow_vol[ui] += flow_cfs * dt_runoff;
        if (flow_cfs > ctx_.subcatches.stat_gw_max_flow[ui])
            ctx_.subcatches.stat_gw_max_flow[ui] = flow_cfs;
        ctx_.subcatches.stat_gw_sum_theta[ui] += gws.theta[ui];
        ctx_.subcatches.stat_gw_sum_depth[ui] += gws.lower_depth[ui];
        ctx_.subcatches.stat_gw_final_theta[ui] = gws.theta[ui];
        ctx_.subcatches.stat_gw_final_depth[ui] = gws.lower_depth[ui];
        ctx_.subcatches.stat_gw_steps[ui]++;
    }
}

// ============================================================================
// stepRouting() — Phase B: hydraulic and quality routing
// ============================================================================

/**
 * @brief Execute Phase B: hydraulic and quality routing.
 *
 * @details Evaluates controls, computes inflows (external, DWF, RDII),
 *          runs hydraulic routing, inlet capture, culvert control,
 *          exfiltration, and quality transport.
 *
 * @param dt_routing  Routing timestep (seconds).
 */
void SWMMEngine::stepRouting(double dt_routing) noexcept {
    // Track routing time-step statistics
    ctx_.routing_stats.update(dt_routing);
    ctx_.routing_stats.record_step_bin(dt_routing);

    // ================================================================
    // PHASE B: ROUTING (once per routing step)
    // Reference: routing_execute() in legacy routing.c
    // ================================================================

    // B0a. Check if between routing events — skip routing if so
    //       (matching legacy isBetweenEvents() in routing.c)
    if (isBetweenEvents(ctx_.current_date)) {
        // Advance next_event_ index past expired events
        while (next_event_ < static_cast<int>(ctx_.events.size()) &&
               ctx_.current_date > ctx_.events[static_cast<size_t>(next_event_)].end) {
            next_event_++;
        }
        return;
    }

    // B0. Half-step mass balance update (P8-G12)
    // Legacy: massbal_updateRoutingTotals(routingStep/2) at start of routing
    // (mass balance accumulators updated with half the step's contribution)

    // B1a. Evaluate pump startup/shutoff depth hysteresis ONCE per timestep
    //      (matching legacy routing.c: link_setTargetSetting runs BEFORE controls_evaluate)
    hydstruct_.updatePumpTargetSettings(ctx_);

    // B1b. Evaluate control rules (P8-G18: orifice gradual open/close)
    //      Control rules can override pump target_setting set above.
    controls_.evaluate(ctx_, ctx_.current_time, dt_routing);
    // Apply setting transitions: move setting toward target_setting
    for (int j = 0; j < ctx_.n_links(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        double target = ctx_.links.target_setting[uj];
        double current = ctx_.links.setting[uj];
        if (target != current) {
            // Update the open<->closed transition timestamp ONLY when one
            // side of the change crosses zero, matching legacy
            // routing.c:295-299. LINK_TIMEOPEN / LINK_TIMECLOSED rule
            // premises read this via ctx.links.time_last_set. (P1-C09)
            if (target * current == 0.0)
                ctx_.links.time_last_set[uj] = ctx_.current_date;

            // Gradual transition (P8-G18): use orifice open/close rate
            // Legacy: link_setSetting() applies orate for time-based ramp
            // orate is stored in hours (from inp file); convert to seconds
            // Legacy: Orifice[k].orate = x[4] * 3600 (hours → seconds)
            // Legacy: step = tstep / Orifice[k].orate (fraction of full opening per step)
            const int orr = ctx_.link_subtypes.orifice_row(j);
            double orate_sec = ((orr >= 0)
                ? ctx_.link_subtypes.orifices.orate[static_cast<std::size_t>(orr)] : 0.0) * 3600.0;
            if (orate_sec > 0.0 && dt_routing > 0.0) {
                double step = dt_routing / orate_sec;
                double delta_val = std::fabs(target - current);
                if (step + 0.001 >= delta_val) {
                    ctx_.links.setting[uj] = target;
                } else if (target > current) {
                    ctx_.links.setting[uj] = current + step;
                } else {
                    ctx_.links.setting[uj] = current - step;
                }
            } else {
                // Instantaneous transition (rate == 0 or non-orifice links)
                ctx_.links.setting[uj] = target;
            }
        }
    }

    // B2. Compute all inflow sources into decomposed arrays, then assemble.
    //     Each source writes to its own per-node buffer; assembleLateralInflows()
    //     sums them all into nodes.lat_flow[] for the routing solver.
    ctx_.nodes.clearInflowSources();

    // Legacy getDateTime() adds +1ms offset to avoid boundary rounding issues
    // with pattern lookups at exact hour/day boundaries.
    double routing_date = datetime::addSeconds(ctx_.options.start_date,
                                               ctx_.current_time + 0.001);
    inflow_.computeAll(ctx_, routing_date, dt_routing);

    // B2a. RDII inflows — apply pre-computed values from wet weather step,
    //      or, under [FILES] USE RDII, the interface file's flows (legacy
    //      addRdiiInflows() always reads from the RDII file; USE just
    //      changes whose file it is).
    if (!ctx_.options.ignore_rdii) {
        if (rdii_iface_file_.isOpen() && !rdii_iface_file_.isWriting()) {
            rdii_iface_file_.applyFlows(ctx_, ctx_.current_date);
        } else {
            // ctx_.current_time is the START of this routing step, matching
            // legacy addRdiiInflows(currentDate) taken at routing_execute
            // entry before the clock advances.
            rdii_.applyRdiiInflows(ctx_, ctx_.current_time);
        }
    }

    // B2b. Interface file inflows (from upstream model coupling)
    iface_.readInflows(ctx_, ctx_.current_date);

    // B2c. Assemble all decomposed sources into nodes.lat_flow[]
    assembleLateralInflows(dt_routing);

#ifdef OPENSWMM_HAS_2D
    // B2c. Pre-routing: update outfall boundary heads from 2D surface state.
    //      Must happen after setOutfallDepths() (called inside router_.step)
    //      but the 2D pre-routing hook modifies outfall heads from 2D state.
    surface_router_.updateOutfallsPreRouting(ctx_);
#endif

    // B2d. Check if system is in steady state — skip routing if so
    //       (matching legacy isInSteadyState() in routing.c)
    int action_count = controls_.lastActionCount();
    if (isInSteadyState(action_count)) {
        return;
    }

    // B3. Hydraulic routing (batch xsect geometry → batch momentum)
    //     Includes: conduit flow, pump/orifice/weir/outlet flow,
    //     divider logic, outfall boundary conditions
    // Pass non-conduit flow callback so pumps/orifices/weirs/outlets are
    // computed INSIDE the DW Picard iteration loop (matching legacy findLinkFlows).
    // The callback applies under-relaxation for iterations > 0, matching legacy
    // findNonConduitFlow() lines 435-438 in dynwave.c.
    constexpr double OMEGA_NC = 0.5; // under-relaxation for non-conduit flows
    // Pre-fetch the non-conduit index list (built once at init)
    const auto& nc_idx = hydstruct_.nonConduitIndices();
    auto& dw = router_.dwSolver();
    auto non_conduit_fn = [this, &nc_idx, &dw](SimulationContext& ctx, double dt, int step) {
        auto& links = ctx.links;

        // PARITY: legacy findLinkFlows (dynwave.c:383-398) processes
        // non-conduit links ONE AT A TIME in link-index order —
        // findNonConduitFlow(i) computes the link's flow from the CURRENT
        // node inflow/outflow accumulators, then updateNodeFlows(i)
        // immediately scatters it. Batch-computing all structures before
        // scattering breaks pump flow-limiting whenever two pumps share a
        // wet well: the second pump's getModPumpFlow must see the first
        // pump's draw already in Node.outflow (Bellinge G70F11Pp1/p2).
        //
        // Each per-link compute threads the DW solver's per-node
        // surface-area buffer through so (a) the pump limiter reads the
        // live Xnode.newSurfArea equivalent, and (b) orifices scatter their
        // per-structure surfArea at their sequential position (matching
        // legacy findNonConduitSurfArea + updateNodeFlows ordering).
        double* surf_buf = dw.nodeNewSurfAreaDataMut();

        for (std::size_t k = 0; k < nc_idx.size(); ++k) {
            int j = nc_idx[k];
            auto uj = static_cast<std::size_t>(j);

            double q_last = links.flow[uj];
            const bool bypassed = dw.isBypassed(j);

            if (!bypassed) {
                // Compute this link's flow (sets links.flow / links.dqdh /
                // per-structure surf areas). dqdh is zeroed inside, matching
                // legacy findNonConduitFlow (dynwave.c:423); only per-type
                // code with a usable derivative overwrites it (orifice, weir,
                // outlet=0, pump3/4/5). Do NOT approximate a q/(2·dh) here.
                hydstruct_.computeNonConduitFlowOne(ctx, dt, surf_buf, j);

                double q_new = links.flow[uj];
                // Under-relaxation for iterations > 0 (legacy dynwave.c:435-438);
                // pumps are exempt.
                if (step > 0 && links.type[uj] != LinkType::PUMP) {
                    q_new = (1.0 - 0.5) * q_last + 0.5 * q_new;
                    // Don't allow flow to change direction without first being ~0
                    if (q_new * q_last < 0.0)
                        q_new = 0.001 * (q_new >= 0.0 ? 1.0 : -1.0);
                }
                links.flow[uj] = q_new;
            } else {
                // Bypassed: hold flow/dqdh at the previous iteration's value
                // (legacy skips findNonConduitFlow) but STILL scatter the held
                // orifice surface area — legacy updateNodeFlows runs for
                // bypassed links too, adding the held Link.surfArea1/2.
                hydstruct_.scatterHeldSurfArea(ctx, surf_buf, j);
            }

            // Scatter to node inflow/outflow (legacy updateNodeFlows) —
            // IMMEDIATELY, so later links in this loop see the update.
            double q = links.flow[uj];
            int n1 = links.node1[uj], n2 = links.node2[uj];
            if (n1 < 0 || n2 < 0) continue;
            auto un1 = static_cast<std::size_t>(n1);
            auto un2 = static_cast<std::size_t>(n2);
            if (q >= 0.0) {
                ctx.nodes.outflow[un1] += q;
                ctx.nodes.inflow[un2]  += q;
            } else {
                ctx.nodes.inflow[un1]  -= q;
                ctx.nodes.outflow[un2] -= q;
            }

            // Scatter dqdh — legacy dynwave.c lines 565-575:
            // TYPE4_PUMP adds dqdh to node1 (inlet) only; skip node2.
            //
            // Only when the dynamic-wave solver owns this step. This callback is
            // SHARED with the finite-volume router, which never calls
            // DWSolver::init, so the accumulator below is an empty vector there
            // — writing to it corrupted the heap for any FV model carrying a
            // pump, orifice, weir or outlet. Skipping is not merely safe, it is
            // correct: ∂Q/∂h is the head sensitivity the implicit node
            // continuity solve needs, and an explicit solver has no such solve.
            if (dw.isInitialized()) {
                double dqdh = links.dqdh[uj];
                const int pr_t4 = ctx_.link_subtypes.pump_row(j);
                const bool is_type4_pump = (links.type[uj] == LinkType::PUMP &&
                                            pr_t4 >= 0 &&
                                            ctx_.link_subtypes.pumps.curve_type[static_cast<std::size_t>(pr_t4)] == 4);
                dw.nodeSumDqdh(n1) += dqdh;
                if (!is_type4_pump) dw.nodeSumDqdh(n2) += dqdh;
            }
        }

#ifdef OPENSWMM_HAS_2D
        // Windowless-coupling conductance (2026-07-29 plan §5.4): the 2D
        // exchange head sensitivity G = −∂Q/∂h ≥ 0 on the node continuity
        // denominator — pure damping for the drain/spill Picard churn the
        // zero-sensitivity explicit source produced. Gated to the default
        // EXPLICIT node continuity until the SEMI_IMPLICIT denominator sign
        // convention is ruled on (DynamicWave.cpp:2932, pre-existing).
        if (dw.isInitialized() && surface_router_.isActive() &&
            ctx_.options.node_continuity == NodeContinuity::EXPLICIT) {
            std::vector<std::pair<int, double>> gs;
            surface_router_.computeCouplingConductances(ctx_, gs);
            for (const auto& [gn, gv] : gs) dw.nodeSumDqdh(gn) += gv;
        }
#endif
    };
    // B3-pre. Exfiltration (storage node Green-Ampt seepage).
    //         Must run BEFORE router_.step() so that pre-computed exfil rates
    //         (written to nodes.storage_exfil_loss) are available to
    //         Router::initNodeFlows() for joint evap+exfil capping.
    exfil_.computeAll(ctx_, dt_routing);

    int iters;
    {
        openswmm::perf::ScopedTimer _pt_1d(openswmm::perf::sec_1d_step);
        iters = router_.step(ctx_, dt_routing, ctx_.climate_state.evap_rate, non_conduit_fn);
    }
    // Legacy counts a step as non-converging from the ACTUAL final Picard flag,
    // not merely from "used all MaxTrials" — a step converging on the last
    // allowed iteration is converged (dynwave.c:245). Using the real flag here
    // matches legacy's "% of Steps Not Converging".
    ctx_.routing_stats.update_iterations(iters, router_.lastStepConverged());

    // A3 parity tracing (env-gated, zero cost when SWMM_TRACE_RSTEP unset):
    // one CSV row per routing step, format-matched to the legacy trace at the
    // end of routing_execute (routing.c) for first-divergence hunting.
    {
        static FILE* trace_file = nullptr;
        static bool  trace_init = false;
        if (!trace_init) {
            trace_init = true;
            const char* p = std::getenv("SWMM_TRACE_RSTEP");
            if (p && *p) {
                trace_file = std::fopen(p, "w");
                if (trace_file)
                    std::fprintf(trace_file,
                                 "step,new_ms,dt_ms,iters,qsum,ysum,lsum,rosum,qhash,yhash\n");
            }
        }
        if (trace_file) {
            static long trace_sn = 0;
            // Order-fixed serial sums (link/node index order, matching the
            // legacy trace) — hex-exact fingerprints of the hydraulic state.
            // FNV-1a 64-bit hashes over the raw bit patterns of link flow &
            // node depth (element order) — exact first-divergence detector
            // (the %a sums absorb small-magnitude element diffs).
            double q_sum = 0.0, y_sum = 0.0, l_sum = 0.0, ro_sum = 0.0;
            unsigned long long q_hash = 14695981039346656037ULL;
            unsigned long long y_hash = 14695981039346656037ULL;
            unsigned long long bits = 0;
            for (int tj = 0; tj < ctx_.n_links(); ++tj) {
                q_sum += ctx_.links.flow[static_cast<std::size_t>(tj)];
                std::memcpy(&bits, &ctx_.links.flow[static_cast<std::size_t>(tj)],
                            sizeof bits);
                q_hash = (q_hash ^ bits) * 1099511628211ULL;
            }
            for (int tj = 0; tj < ctx_.n_nodes(); ++tj) {
                y_sum += ctx_.nodes.depth[static_cast<std::size_t>(tj)];
                std::memcpy(&bits, &ctx_.nodes.depth[static_cast<std::size_t>(tj)],
                            sizeof bits);
                y_hash = (y_hash ^ bits) * 1099511628211ULL;
            }
            for (int tj = 0; tj < ctx_.n_nodes(); ++tj)
                l_sum += ctx_.nodes.lat_flow[static_cast<std::size_t>(tj)];
            for (int tj = 0; tj < ctx_.n_subcatches(); ++tj)
                ro_sum += ctx_.subcatches.runoff[static_cast<std::size_t>(tj)];
            std::fprintf(trace_file, "%ld,%.6f,%.6f,%d,%a,%a,%a,%a,%016llx,%016llx\n",
                         ++trace_sn,
                         ctx_.elapsed_ms + 1000.0 * dt_routing,
                         1000.0 * dt_routing, iters, q_sum, y_sum, l_sum,
                         ro_sum, q_hash, y_hash);
            g_trace_rstep_sn = trace_sn;  // step-gate for DynamicWave link trace

            // Optional per-element dump at one step (SWMM_TRACE_DUMP_STEP=N),
            // format-matched to the legacy dump for element-level pinpointing.
            {
                static long dump_step = -1;
                static bool dump_init = false;
                if (!dump_init) {
                    dump_init = true;
                    const char* d = std::getenv("SWMM_TRACE_DUMP_STEP");
                    if (d && *d) dump_step = std::atol(d);
                }
                if (trace_sn == dump_step) {
                    char fname[512];
                    std::snprintf(fname, sizeof(fname), "%s.dump%ld",
                                  std::getenv("SWMM_TRACE_RSTEP"), dump_step);
                    if (FILE* df = std::fopen(fname, "w")) {
                        for (int tj = 0; tj < ctx_.n_links(); ++tj) {
                            auto utj = static_cast<std::size_t>(tj);
                            std::fprintf(df, "L,%d,%a,%a\n", tj,
                                         ctx_.links.flow[utj],
                                         ctx_.links.dqdh[utj]);
                        }
                        for (int tj = 0; tj < ctx_.n_nodes(); ++tj) {
                            auto utj = static_cast<std::size_t>(tj);
                            std::fprintf(df, "N,%d,%a,%a,%a,%a,%a\n", tj,
                                         ctx_.nodes.depth[utj],
                                         ctx_.nodes.inflow[utj],
                                         ctx_.nodes.outflow[utj],
                                         ctx_.nodes.lat_flow[utj],
                                         ctx_.nodes.old_lat_flow[utj]);
                        }
                        for (int tj = 0; tj < ctx_.n_subcatches(); ++tj) {
                            auto utj = static_cast<std::size_t>(tj);
                            std::fprintf(df, "S,%d,%a,%a,%a,%a\n", tj,
                                         ctx_.subcatches.runoff[utj],
                                         ctx_.subcatches.rainfall[utj],
                                         ctx_.subcatches.infil_loss[utj],
                                         ctx_.subcatches.old_runoff[utj]);
                        }
                        std::fclose(df);
                    }
                }
            }
        }
    }

#ifdef OPENSWMM_HAS_2D
    // B3+. Post-routing: compute 2D↔1D coupling exchange, update rainfall,
    //      advance the 2D solver, transfer outfall discharges to 2D cells.
    surface_router_.advancePostRouting(ctx_, dt_routing, ctx_.current_time);
#endif

    // B3a. Inlet capture (street inlet HEC-22 calculations)
    inlet_.computeAll(ctx_, dt_routing);

    // B3b. Culvert inlet control (FHWA HEC-5 equations)
    //      Uses pre-built culvert_links_ (populated in initHydraulics)
    //
    //      NOT for FV: it applies the same closure as a cap on the flux
    //      crossing the culvert's upstream face, inside the solver. Rewriting
    //      links.flow here afterwards would contradict the node ledger
    //      publishFv already booked from those fluxes.
    if (!culvert_links_.empty() &&
        ctx_.options.routing_model != RoutingModel::FV) {
        culvert::batchComputeInletControl(
            culvert_links_.data(),
            static_cast<int>(culvert_links_.size()),
            ctx_);
    }

    // B4. Non-conduit link flows are now computed inside the DW Picard loop
    //     via the non_conduit_fn callback passed to router_.step().

    // B4a. Outfall results for the routing interface file are written at
    //      reporting cadence in postOutputSnapshot() — matching legacy
    //      output_saveResults() → iface_saveOutletResults() — not here.

    // B4b. Gap #55: inlet quality mass transfer (bypass↔capture based on net flow).
    //      IGNORE_QUALITY skips inlet quality adjustment + quality routing
    //      (legacy routing.c:252 `Nobjects[POLLUT] > 0 && !IgnoreQuality`).
    if (ctx_.n_pollutants() > 0 && !ctx_.options.ignore_quality) {
        inlet_.adjustQualInflows(ctx_, dt_routing);
    }

    // B5. Water quality routing (P8-G13: fill stub bodies).
    //     QUALITY_SOLVER EULERIAN_ARD (master plan D-UT6, phase E1): the
    //     external-load assembly is shared with the legacy path; the CSTR
    //     transport stages are replaced by the ARD engine on the FV cell
    //     mesh. Lazy first-step init because the transport mesh needs
    //     Router::init's mod_length/rough_factor. Init failure falls back
    //     to LEGACY with a warning — never a silent no-quality run.
    //
    //     R4: a reactions component keeps the stage alive with zero
    //     pollutants — an MSX-only model has species to react but no
    //     [POLLUTANTS] row to gate on. Bypasses that DO skip it
    //     (EULERIAN_ARD, IGNORE_QUALITY) warn at open rather than running
    //     silently; see transport::warnIfLegacyBindingBypassed.
    //     H1: HEAT_TRANSPORT joins the same list for the same reason — a
    //     temperature-only deck has no [POLLUTANTS] row to gate on, and
    //     without this the quality stage never runs, so heat_state is never
    //     sized and routeLegacyHeat never fires.
    if ((ctx_.n_pollutants() > 0 ||
         transport::legacyReactionsActive(ctx_) ||
         ctx_.options.water_age ||
         ctx_.options.heat_transport) &&
        !ctx_.options.ignore_quality) {
        if (ctx_.options.quality_solver == QualitySolverKind::EULERIAN_ARD) {
            if (!ard_init_attempted_) {
                ard_init_attempted_ = true;
                const bool ok = ard_.init(ctx_);
                for (const auto& w : ard_.warnings())
                    ctx_.warnings.push_back(w);
                if (!ok)
                    ctx_.warnings.push_back(
                        std::string(
                            "QUALITY_SOLVER EULERIAN_ARD: transport mesh "
                            "unavailable — falling back to LEGACY quality "
                            "routing.") +
                        (ctx_.ard_config.any_dispersion()
                             ? " The transport.ard dispersion configuration "
                               "does not apply under the LEGACY engine."
                             : ""));
            }
            if (ard_.initialized()) {
                quality_.assembleExternalLoads(ctx_, dt_routing);
                ard_.step(ctx_, dt_routing);
                // E5b treatment interop: the legacy evaluator runs on the
                // PUBLISHED node concentrations (same expressions, same
                // process variables, books its own reacted losses); the
                // engine then absorbs the treated concentrations back into
                // its node stores. Ordering note (documented decision):
                // under ARD treatment applies AFTER the reaction stage at
                // end of step, where legacy applies it before decay.
                if (ctx_.treatment.hasAny()) {
                    quality_.applyTreatment(ctx_, dt_routing);
                    ard_.absorbTreatedNodeConc(ctx_);
                }
            } else {
                quality_.execute(ctx_, dt_routing);
            }
        } else if (ctx_.options.quality_solver ==
                   QualitySolverKind::LAGRANGIAN) {
            // X2: LTD segment transport. External loads come from the SAME
            // shared loader seam both other engines consume (the ARD
            // precedent one branch up); the solver drains them as rate×dt /
            // volume inside its topological mixing pass. Treatment and the
            // reactions component do not run here yet — warned at open.
            quality_.assembleExternalLoads(ctx_, dt_routing);
            lard_.step(ctx_, dt_routing);
            // P2.3: treatment applies INSIDE the solver's MIX (per node,
            // this substep's own inflow figures, before RELEASE draws the
            // treated state) — quality::applyNodeTreatment, the seam shared
            // with the LEGACY pass. The handoff's end-of-step call here was
            // a no-op twice over (cin read the LEGACY accumulators, and a
            // junction's ~zero stored volume gave the late write no weight
            // in the next mix) and its own gate caught it on first run.
        } else {
            quality_.execute(ctx_, dt_routing);
        }
    }
}

// ============================================================================
// updateStatistics() — update node and link statistics after routing
// ============================================================================

/**
 * @brief Update node and link statistics after routing.
 *
 * @details Updates max depth, max overflow, flooding duration/volume for
 *          nodes, and max flow, max velocity, max filling, surcharge
 *          duration, and volume conveyed for links.
 *
 * @param dt_routing  Routing timestep (seconds).
 */
void SWMMEngine::ensureXspCache() noexcept {
    const auto n = static_cast<std::size_t>(ctx_.n_links());
    if (xsp_cache_.size() == n && xsp_cache_gen_ == ctx_.xsect_generation)
        return;
    xsp_cache_.resize(n);
    for (std::size_t uj = 0; uj < n; ++uj)
        xsp_cache_[uj] = link::buildXSectParams(ctx_.links, uj,
                                                &ctx_.transect_tables);
    xsp_cache_gen_ = ctx_.xsect_generation;
}

void SWMMEngine::updateStatistics(double dt_routing) noexcept {
    const int np = ctx_.n_pollutants();

    // B6. Update statistics (P8-G11)
    for (int j = 0; j < ctx_.n_nodes(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        // Depth statistics
        double cur_depth = ctx_.nodes.depth[uj];
        ctx_.nodes.stat_sum_depth[uj] += cur_depth;
        // Storage-volume statistics: accumulate the node's actual stored volume at
        // this depth (legacy StorageStats avgVol += newVolume). Recomputed from the
        // maintained depth via the same relation the report uses, so the Storage
        // Volume Summary's average is exact for the nonlinear shapes rather than a
        // volume-of-average-depth approximation.
        if (ctx_.nodes.type[uj] == NodeType::STORAGE) {
            const int us = ucf::getUnitSystem(static_cast<int>(ctx_.options.flow_units));
            ctx_.nodes.stat_sum_volume[uj] += node::getVolume(
                ctx_.nodes, j, cur_depth, &ctx_.tables, us, &ctx_.node_subtypes);
        }
        if (cur_depth > ctx_.nodes.stat_max_depth[uj]) {
            ctx_.nodes.stat_max_depth[uj] = cur_depth;
            ctx_.nodes.stat_max_depth_date[uj] = ctx_.current_date;
        }
        if (cur_depth > ctx_.nodes.stat_max_rpt_depth[uj])
            ctx_.nodes.stat_max_rpt_depth[uj] = cur_depth;
        if (ctx_.nodes.overflow[uj] > ctx_.nodes.stat_max_overflow[uj]) {
            ctx_.nodes.stat_max_overflow[uj] = ctx_.nodes.overflow[uj];
            ctx_.nodes.stat_max_overflow_date[uj] = ctx_.current_date;
        }
        if (ctx_.nodes.overflow[uj] > 0.0) {
            ctx_.nodes.stat_time_flooded[uj] += dt_routing;
            ctx_.nodes.stat_vol_flooded[uj] += ctx_.nodes.overflow[uj] * dt_routing;
        }

        // Node surcharge tracking
        double full_d = ctx_.nodes.full_depth[uj];
        if (full_d > 0.0 && cur_depth > full_d) {
            ctx_.nodes.stat_time_surcharged[uj] += dt_routing;
            double surcharge_h = cur_depth - full_d;
            if (surcharge_h > ctx_.nodes.stat_max_surcharge_height[uj])
                ctx_.nodes.stat_max_surcharge_height[uj] = surcharge_h;
        }

        // Node inflow statistics (matching legacy stats_updateNodeStats)
        double lat = ctx_.nodes.lat_flow[uj];
        double total_inflow = ctx_.nodes.inflow[uj];
        if (std::fabs(lat) > ctx_.nodes.stat_max_lat_inflow[uj])
            ctx_.nodes.stat_max_lat_inflow[uj] = std::fabs(lat);
        if (total_inflow > ctx_.nodes.stat_max_total_inflow[uj]) {
            ctx_.nodes.stat_max_total_inflow[uj] = total_inflow;
            ctx_.nodes.stat_max_inflow_date[uj] = ctx_.current_date;
        }
        ctx_.nodes.stat_lat_inflow_vol[uj]   += std::fabs(lat) * dt_routing;
        ctx_.nodes.stat_total_inflow_vol[uj] += total_inflow * dt_routing;
        // PARITY stats.c:588 stats_updateStorageStats — a storage unit's peak
        // RELEASE, which is what its outlet structures actually passed. Not
        // interchangeable with peak inflow: attenuating the two is the point.
        if (ctx_.nodes.type[uj] == NodeType::STORAGE &&
            ctx_.nodes.outflow[uj] > ctx_.nodes.stat_storage_max_outflow[uj])
            ctx_.nodes.stat_storage_max_outflow[uj] = ctx_.nodes.outflow[uj];
        // For outfall nodes, outflow = inflow by definition (matching legacy
        // massbal.c line 587: NodeOutflow[j] += Node[j].inflow * tStep).
        // For non-storage terminal nodes (degree==0), same treatment.
        if (ctx_.nodes.type[uj] == NodeType::OUTFALL ||
            (ctx_.nodes.degree[uj] == 0 &&
             ctx_.nodes.type[uj] != NodeType::STORAGE)) {
            ctx_.nodes.stat_total_outflow_vol[uj] += total_inflow * dt_routing;
        } else {
            ctx_.nodes.stat_total_outflow_vol[uj] += ctx_.nodes.outflow[uj] * dt_routing;
            if (ctx_.nodes.volume[uj] <= ctx_.nodes.full_volume[uj])
                ctx_.nodes.stat_total_outflow_vol[uj] += ctx_.nodes.overflow[uj] * dt_routing;
        }

        // Outfall statistics
        if (ctx_.nodes.type[uj] == NodeType::OUTFALL) {
            double qi = ctx_.nodes.inflow[uj];
            if (qi > 0.001) { // MIN_RUNOFF_FLOW threshold
                ctx_.nodes.stat_outfall_avg_flow[uj] += qi;
                if (qi > ctx_.nodes.stat_outfall_max_flow[uj])
                    ctx_.nodes.stat_outfall_max_flow[uj] = qi;
                ctx_.nodes.stat_outfall_periods[uj]++;
            }
            // Outfall pollutant loads: load += inflow * conc * dt
            if (np > 0) {
                auto base = uj * static_cast<std::size_t>(np);
                for (int p = 0; p < np; ++p) {
                    auto idx = base + static_cast<std::size_t>(p);
                    if (idx < ctx_.nodes.stat_total_load.size() &&
                        idx < ctx_.nodes.conc.size()) {
                        ctx_.nodes.stat_total_load[idx] +=
                            qi * ctx_.nodes.conc[idx] * dt_routing;
                    }
                }
            }
        }
    }
    ensureXspCache();
    double step_slot_vol = 0.0, step_stored_vol = 0.0;   // slot program R0
    for (int j = 0; j < ctx_.n_links(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        double q = std::fabs(ctx_.links.flow[uj]);
        if (q > ctx_.links.stat_max_flow[uj]) {
            ctx_.links.stat_max_flow[uj] = q;
            ctx_.links.stat_max_flow_date[uj] = ctx_.current_date;
        }

        // Volume conveyed
        ctx_.links.stat_vol_flow[uj] += q * dt_routing;

        // Velocity (matching legacy link_getVelocity in link.c)
        // Uses geometric area from cross-section shape at current depth,
        // guards on depth <= 0.01, and divides flow by barrels.
        double vel = 0.0;
        if (ctx_.links.type[uj] == LinkType::CONDUIT) {
            const int cr = ctx_.link_subtypes.conduit_row(j);
            const int nb = (cr >= 0) ? ctx_.link_subtypes.conduits.barrels[static_cast<std::size_t>(cr)] : 1;
            vel = link::getVelocity(xsp_cache_[uj], q, ctx_.links.depth[uj], nb);
        }
        if (vel > ctx_.links.stat_max_veloc[uj])
            ctx_.links.stat_max_veloc[uj] = vel;

        // Filling ratio (depth / full depth)
        double y_full = ctx_.links.xsect_y_full[uj];
        double d = ctx_.links.depth[uj];
        double filling = (y_full > 0.0) ? d / y_full : 0.0;
        if (filling > ctx_.links.stat_max_filling[uj])
            ctx_.links.stat_max_filling[uj] = filling;

        // Surcharge duration tracking
        if (d >= y_full && y_full > 0.0)
            ctx_.links.stat_time_surcharged[uj] += dt_routing;

        // Slot-storage share accumulation (FV slot program R0). The run
        // share is ∫slot dt / ∫stored dt — a ratio of integrals, never an
        // average of ratios. All terms are 0 under the dynamic-wave router.
        {
            const double v  = ctx_.links.volume[uj];
            const double sv = ctx_.links.slot_volume[uj];
            ctx_.links.stat_vol_dt[uj]      += v  * dt_routing;
            step_stored_vol += v;
            if (sv > 0.0) {
                ctx_.links.stat_slot_vol_dt[uj] += sv * dt_routing;
                step_slot_vol += sv;
                const double share = (v > 0.0) ? sv / v : 0.0;
                if (share > ctx_.links.stat_peak_slot_share[uj])
                    ctx_.links.stat_peak_slot_share[uj] = share;
                if (share > 0.01)
                    ctx_.links.stat_time_slot_above[uj] += dt_routing;
            }
        }

        // Conduit surcharge detail tracking (upstream/downstream/both)
        // Gap #57: use persistent full_state set by routing solver (area/depth-based).
        if (ctx_.links.type[uj] == LinkType::CONDUIT && y_full > 0.0) {
            int n1 = ctx_.links.node1[uj];
            int n2 = ctx_.links.node2[uj];
            const int cr = ctx_.link_subtypes.conduit_row(j);
            const auto& CD = ctx_.link_subtypes.conduits;
            int8_t fs = (cr >= 0) ? CD.full_state[static_cast<std::size_t>(cr)] : int8_t{0};
            bool up_full = (fs & 1) != 0;
            bool dn_full = (fs & 2) != 0;
            if (up_full) ctx_.links.stat_time_full_upstream[uj] += dt_routing;
            if (dn_full) ctx_.links.stat_time_full_dnstream[uj] += dt_routing;
            if (up_full && dn_full) ctx_.links.stat_time_full_both[uj] += dt_routing;
            // Gap #58: Capacity-limited conduit identification.
            // KW: capacityLimited = (a1 >= aFull)  ≡ up_full
            // DW: capacityLimited = (a1 >= aFull) && (HGL slope > bed slope)
            if (up_full) {
                bool cap_ltd = true;
                if ((ctx_.options.routing_model == RoutingModel::DYNWAVE ||
                     ctx_.options.routing_model == RoutingModel::FV) &&
                    n1 >= 0 && n2 >= 0) {
                    double h1h = ctx_.nodes.head[static_cast<std::size_t>(n1)];
                    double h2h = ctx_.nodes.head[static_cast<std::size_t>(n2)];
                    double len = (cr >= 0) ? CD.mod_length[static_cast<std::size_t>(cr)] : 0.0;
                    if (len <= 0.0) len = (cr >= 0) ? CD.length[static_cast<std::size_t>(cr)] : 0.0;
                    double slp = std::fabs((cr >= 0) ? CD.slope[static_cast<std::size_t>(cr)] : 0.0);
                    cap_ltd = (h1h - h2h) > slp * len;
                }
                if (cap_ltd) ctx_.links.stat_time_capacity_limited[uj] += dt_routing;
            }
        }

        // Flow classification counter
        int fc = static_cast<int>(ctx_.links.flow_class[uj]);
        if (fc >= 0 && fc < LinkData::N_FLOW_CLASSES) {
            auto fc_idx = uj * LinkData::N_FLOW_CLASSES + static_cast<std::size_t>(fc);
            if (fc_idx < ctx_.links.stat_flow_class.size())
                ++ctx_.links.stat_flow_class[fc_idx];
        }

        // Normal flow limited / inlet control counters (conduit-only side-table
        // fields; non-conduits had a false wide default -> skip).
        {
            const int crs = ctx_.link_subtypes.conduit_row(j);
            if (crs >= 0) {
                const auto ucrs = static_cast<std::size_t>(crs);
                auto& CDs = ctx_.link_subtypes.conduits;
                if (CDs.normal_flow_limited[ucrs]) {
                    ++ctx_.links.stat_norm_ltd[uj];
                    CDs.normal_flow_limited[ucrs] = uint8_t{0};  // reset for next step
                }
                if (CDs.inlet_control[ucrs]) {
                    ++ctx_.links.stat_inlet_ctrl[uj];
                    CDs.inlet_control[ucrs] = uint8_t{0};
                }
            }
        }

        // Pump utilization statistics
        if (ctx_.links.type[uj] == LinkType::PUMP) {
            bool is_on = (ctx_.links.setting[uj] > 0.0 && q > 0.0);
            bool was_on = ctx_.links.stat_pump_was_on[uj];

            // Legacy counts start-ups only on OFF→ON transitions (stats.c:682-683);
            // refactored previously incremented on either direction, giving a 2×
            // inflation in reported start-up counts.
            if (is_on && !was_on) {
                ctx_.links.stat_pump_cycles[uj]++;
            }
            if (is_on != was_on) {
                ctx_.links.stat_pump_was_on[uj] = is_on;
            }
            if (is_on) {
                ctx_.links.stat_pump_on_time[uj] += dt_routing;
                ctx_.links.stat_pump_volume[uj] += q * dt_routing;

                // Pump energy (kWh), matching legacy link_getPower:
                //   power_kW = |dh| * |q| / 8.814 * KWperHP ; energy += power_kW * dt/3600
                // KWperHP = 0.7457 (conversion factor).
                const int pn1 = ctx_.links.node1[uj];
                const int pn2 = ctx_.links.node2[uj];
                if (pn1 >= 0 && pn2 >= 0) {
                    const auto un1 = static_cast<std::size_t>(pn1);
                    const auto un2 = static_cast<std::size_t>(pn2);
                    const double h1 = ctx_.nodes.invert_elev[un1] + ctx_.nodes.depth[un1];
                    const double h2 = ctx_.nodes.invert_elev[un2] + ctx_.nodes.depth[un2];
                    const double dh = std::fabs(h1 - h2);
                    constexpr double KWperHP = 0.7457;
                    const double power_kw = dh * q / 8.814 * KWperHP;
                    ctx_.links.stat_pump_energy[uj] += power_kw * dt_routing / 3600.0;
                }
            }
        }

        // Flow turn tracking (matching legacy stats_updateLinkStats flow turn logic)
        {
            double dq = ctx_.links.flow[uj] - ctx_.links.old_flow[uj];
            int old_sign = ctx_.links.stat_flow_turn_sign[uj];
            int new_sign = (dq > 0.0) ? 1 : ((dq < 0.0) ? -1 : 0);
            ctx_.links.stat_flow_turn_sign[uj] = new_sign;
            if (std::fabs(dq) > 0.001 && old_sign * new_sign < 0)
                ++ctx_.links.stat_flow_turns[uj];
        }

        // Link pollutant loads: load += |flow| * conc * dt
        if (np > 0 && q > 0.0) {
            auto base = uj * static_cast<std::size_t>(np);
            for (int p = 0; p < np; ++p) {
                auto idx = base + static_cast<std::size_t>(p);
                if (idx < ctx_.links.stat_total_load.size() &&
                    idx < ctx_.links.conc.size()) {
                    ctx_.links.stat_total_load[idx] +=
                        q * ctx_.links.conc[idx] * dt_routing;
                }
            }
        }
    }

    // System-level slot share (FV slot program R0): the peak instantaneous
    // share and the time above the 1 % budget need the per-step totals; the
    // run-level integrated share is recovered at report time by summing the
    // per-link integrals.
    if (step_slot_vol > 0.0) {
        const double sys_share =
            (step_stored_vol > 0.0) ? step_slot_vol / step_stored_vol : 0.0;
        if (sys_share > ctx_.routing_stats.slot_peak_share)
            ctx_.routing_stats.slot_peak_share = sys_share;
        if (sys_share > 0.01)
            ctx_.routing_stats.slot_time_above_s += dt_routing;
    }
    // OPENSWMM_FV_SLOT_TRACE=1 → one CSV row per routing step on stdout.
    static const bool slot_trace = []() {
        const char* e = std::getenv("OPENSWMM_FV_SLOT_TRACE");
        return e && *e && *e != '0';
    }();
    if (slot_trace && step_stored_vol > 0.0) {
        std::printf("SLOT_TRACE,%.6f,%.6g,%.6g,%.6g\n",
                    ctx_.current_time, step_slot_vol, step_stored_vol,
                    step_slot_vol / step_stored_vol);
    }
}

// ============================================================================
// effectiveUserLatFlow — per-step forced lateral inflow at a node
// ============================================================================

// Effective runtime-forced lateral inflow at node uj for this step: the
// persistent runtime-API value (user_lat_flow, set via
// swmm_node_set_lateral_inflow) with any active ForcingData lateral-inflow
// forcing overlaid — OVERRIDE replaces it, ADD adds to it. The forcing is
// never written back into user_lat_flow: it is re-applied each step while
// its mode is active and vanishes when the mode clears, so a RESET forcing
// lasts exactly one step and a PERSIST+ADD forcing contributes a steady
// (non-compounding) rate. Issue #113.
static double effectiveUserLatFlow(const SimulationContext& ctx,
                                   std::size_t uj) noexcept {
    double q = ctx.nodes.user_lat_flow[uj];
    if (uj < ctx.forcing.node_lat_inflow_mode.size()) {
        const auto m = ctx.forcing.node_lat_inflow_mode[uj];
        if (m == ForcingMode::OVERRIDE)
            q = ctx.forcing.node_lat_inflow_value[uj];
        else if (m == ForcingMode::ADD)
            q += ctx.forcing.node_lat_inflow_value[uj];
    }
    return q;
}

// ============================================================================
// updateRoutingMassBalance() — routing mass balance totals after routing
// ============================================================================

/**
 * @brief Update routing mass balance totals after routing.
 *
 * @details Accumulates flooding, outfall outflow, evaporation/seepage losses,
 *          link seepage losses, and wet weather inflow volumes for the
 *          routing mass balance.
 *
 * @param dt_routing  Routing timestep (seconds).
 */
void SWMMEngine::updateRoutingMassBalance(double dt_routing) noexcept {
    // B7. Mass balance update (P8-G12: routing totals)
    //     Accumulate ALL flow paths matching legacy massbal_updateRoutingTotals
    ctx_.mass_balance.step_flooding  = 0.0;
    ctx_.mass_balance.step_outflow   = 0.0;

    // Accumulate DWF, GW, RDII, and external inflow volumes from step accumulators
    // (step accumulators are set during Inflow::computeAll / RDIISolver::computeAll
    //  and stepRunoff Phase 2 for GW)
    ctx_.mass_balance.routing_dry_weather += ctx_.mass_balance.step_dw_inflow * dt_routing;
    ctx_.mass_balance.routing_gw_inflow   += ctx_.mass_balance.step_gw_inflow * dt_routing;
    ctx_.mass_balance.routing_rdii        += ctx_.mass_balance.step_rdii_inflow * dt_routing;
    ctx_.mass_balance.routing_external    += ctx_.mass_balance.step_ext_inflow * dt_routing;

    // Mass-balance accounting for the per-node system-outflow / flooding
    // categorisation (matching legacy node_getSystemOutflow + removeOutflows).
    //
    // Legacy logic (node.c:414-468):
    //   OUTFALL                                : outflow = inflow (when outflow==0),
    //                                            then overflow=0, newVolume=0
    //   non-DW + degree==0 + non-STORAGE       : outflow = inflow (terminal)
    //   else (interior, including DW degree==0): outflow = overflow only when
    //                                            newVolume <= fullVolume,
    //                                            isFlooded = TRUE
    //
    // Critical: under DYNWAVE, degree==0 non-STORAGE nodes are NOT terminal —
    // they fall into the "interior" branch and only contribute system flow
    // when overflow is positive AND newVolume <= fullVolume.
    // ==DYNWAVE audit: FV also gives every node a head and a volume, so
    // degree==0 non-STORAGE nodes are interior under FV exactly as under DW.
    const bool is_dw = (ctx_.options.routing_model == RoutingModel::DYNWAVE ||
                        ctx_.options.routing_model == RoutingModel::FV);

    // Legacy KW/SF Node.degree (outflow-link count, toposort.c:70-91) for the
    // terminal-node test below — ctx_.nodes.degree is the DW hybrid and reads
    // nonzero for a fed terminal junction, mis-filing its throughflow as
    // interior flooding. Static topology: computed once, sized to the model.
    if (!is_dw &&
        outflow_degree_.size() != static_cast<std::size_t>(ctx_.n_nodes())) {
        const int nn = ctx_.n_nodes();
        outflow_degree_.assign(static_cast<std::size_t>(nn), 0);
        for (int j = 0; j < ctx_.n_links(); ++j) {
            auto uj = static_cast<std::size_t>(j);
            int n = (ctx_.links.direction[uj] < 0) ? ctx_.links.node2[uj]
                                                   : ctx_.links.node1[uj];
            if (n < 0 || n >= nn) continue;
            if (ctx_.nodes.type[static_cast<std::size_t>(n)] == NodeType::OUTFALL) {
                n = (ctx_.links.direction[uj] < 0) ? ctx_.links.node1[uj]
                                                   : ctx_.links.node2[uj];
                if (n < 0 || n >= nn) continue;
            }
            outflow_degree_[static_cast<std::size_t>(n)]++;
        }
    }

    for (int j = 0; j < ctx_.n_nodes(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        const NodeType nt = ctx_.nodes.type[uj];

        if (nt == NodeType::OUTFALL) {
            // Legacy node_getSystemOutflow (node.c:428-447) + removeOutflows
            // (routing.c:921-931): an outfall's system flow is its pipe inflow
            // when discharging. When the outfall instead sends water BACK into
            // the network (outflow > 0, inflow == 0 — e.g. a 2D tailwater or a
            // FIXED/TIDAL/TIMESERIES stage above the upstream HGL), the system
            // flow is NEGATIVE and legacy books it as an EXTERNAL system INFLOW
            // of magnitude outflow (massbal_addInflowFlow(EXTERNAL_INFLOW, -q)).
            // Mirroring that closes routing continuity for any backflowing
            // outfall (a pre-existing gap for tidal/fixed outfalls, and required
            // for the 2D-coupled withdrawal path in transferOutfallDischarges).
            double q_in  = ctx_.nodes.inflow[uj];
            double q_out = ctx_.nodes.outflow[uj];
            if (q_out > 0.0 && q_in <= 0.0) {
                // Backflow into the network — system external inflow. step_*
                // is added here (after the step_ext_inflow consumption at the
                // top of this function) for the per-step snapshot only,
                // mirroring the coupling-spill booking below.
                ctx_.mass_balance.routing_external += q_out * dt_routing;
                ctx_.mass_balance.step_ext_inflow  += q_out;
            } else {
                // Normal discharge — system outflow = inflow.
                ctx_.mass_balance.routing_outflow += q_in * dt_routing;
                ctx_.mass_balance.step_outflow    += q_in;

                // Gap #28: accumulate outfall discharge as routed volume for next
                // runoff step (matching legacy Outfall[i].vRouted accumulation).
                const int ofr = ctx_.node_subtypes.outfall_row(static_cast<int>(uj));
                int sc = (ofr >= 0)
                    ? ctx_.node_subtypes.outfalls.route_to[static_cast<std::size_t>(ofr)] : -1;
                if (sc >= 0 && sc < ctx_.n_subcatches() && q_in > 0.0) {
                    auto usc = static_cast<std::size_t>(sc);
                    ctx_.subcatches.outfall_runon_vol[usc] += q_in * dt_routing;
                    // A4: water returning from an outfall carries that
                    // node's age. Without it the run-on age is divided by a
                    // rate this volume contributed to but its age did not.
                    auto& wo = ctx_.water_age_state.subcatch_outfall_age_vol;
                    if (ctx_.options.water_age && usc < wo.size() &&
                        uj < ctx_.water_age_state.node_age.size())
                        wo[usc] += q_in * dt_routing *
                                   ctx_.water_age_state.node_age[uj];
                    // H5a: and that node's temperature, at the same seam.
                    auto& ho = ctx_.heat_state.subcatch_outfall_temp_vol;
                    if (ctx_.options.heat_transport && usc < ho.size() &&
                        uj < ctx_.heat_state.node_temp.size())
                        ho[usc] += q_in * dt_routing *
                                   ctx_.heat_state.node_temp[uj];
                }
            }
            // Legacy node_getSystemOutflow zeroes an outfall's overflow and
            // volume every step (node.c:445-447) so an outfall never reports
            // flooding or stored volume. Applied for the tree-layout routers
            // only: KW/SF setNewNodeState computes a raw net-inflow overflow
            // at outfalls that legacy discards here. DW outfalls never
            // acquire overflow (boundary-condition depth path), so the DW
            // behavior is left untouched.
            if (!is_dw) {
                ctx_.nodes.overflow[uj] = 0.0;
                ctx_.nodes.volume[uj]   = 0.0;
            }
        }
        else if (!is_dw && outflow_degree_[uj] == 0 && nt != NodeType::STORAGE) {
            // Non-DW terminal node (no OUTFLOW links — legacy KW degree):
            // its inflow is system outflow, and legacy node_getSystemOutflow
            // (node.c:453-459) zeroes its overflow and volume so it reports
            // neither flooding nor storage.
            if (ctx_.nodes.outflow[uj] == 0.0) {
                ctx_.mass_balance.routing_outflow += ctx_.nodes.inflow[uj] * dt_routing;
                ctx_.mass_balance.step_outflow    += ctx_.nodes.inflow[uj];
            }
            ctx_.nodes.overflow[uj] = 0.0;
            ctx_.nodes.volume[uj]   = 0.0;
        }
        else {
            // Interior node (also DW terminal nodes): overflow counted as
            // flooding only if newVolume <= fullVolume (matching legacy).
            if (ctx_.nodes.overflow[uj] > 0.0 &&
                ctx_.nodes.volume[uj] <= ctx_.nodes.full_volume[uj]) {
                ctx_.mass_balance.routing_flooding += ctx_.nodes.overflow[uj] * dt_routing;
                ctx_.mass_balance.step_flooding    += ctx_.nodes.overflow[uj];
            }
        }

        // Node evaporation and seepage losses
        ctx_.mass_balance.routing_evap_loss += ctx_.nodes.losses[uj] * dt_routing;
    }

    // Accumulate link evaporation and seepage losses
    for (int j = 0; j < ctx_.n_links(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (ctx_.links.type[uj] == LinkType::CONDUIT) {
            const int cr = ctx_.link_subtypes.conduit_row(j);
            const auto& CD = ctx_.link_subtypes.conduits;
            const auto ucr = static_cast<std::size_t>(cr);
            int barrels = std::max((cr >= 0) ? CD.barrels[ucr] : 1, 1);
            ctx_.mass_balance.routing_evap_loss +=
                ((cr >= 0) ? CD.evap_loss_rate[ucr] : 0.0) * barrels * dt_routing;
            ctx_.mass_balance.routing_seep_loss +=
                ((cr >= 0) ? CD.seep_loss_rate[ucr] : 0.0) * barrels * dt_routing;
        }
    }

    // Wet weather inflow: computed directly from decomposed runoff_inflow array
    // (replaces the previous back-calculation: total_lat - DWF - GW - RDII - ext).
    {
        double runoff_q = 0.0;
        double user_q_total = 0.0;
        double coupling_out_q = 0.0;   // 1D → 2D, absolute Σ over coupled nodes
        for (int j = 0; j < ctx_.n_nodes(); ++j) {
            auto uj = static_cast<std::size_t>(j);
            if (ctx_.nodes.runoff_inflow[uj] > 0.0)
                runoff_q += ctx_.nodes.runoff_inflow[uj];
            if (const double q_user = effectiveUserLatFlow(ctx_, uj);
                q_user > 0.0)
                user_q_total += q_user;
            if (ctx_.nodes.coupling_inflow[uj] < 0.0)
                coupling_out_q += -ctx_.nodes.coupling_inflow[uj];
        }
        if (runoff_q > 0.0) {
            ctx_.mass_balance.routing_wet_weather += runoff_q * dt_routing;
        }
        if (user_q_total > 0.0) {
            ctx_.mass_balance.routing_forcing_inflow += user_q_total * dt_routing;
        }
        // 1D → 2D coupling spill folds into routing_flooding (and the
        // per-step accumulator) so it appears under the existing "Flooding
        // Loss" row in the continuity report. The positive (2D → 1D) side
        // was already added to step_ext_inflow → routing_external by
        // assembleLateralInflows. See review §11.
        if (coupling_out_q > 0.0) {
            ctx_.mass_balance.routing_flooding += coupling_out_q * dt_routing;
            ctx_.mass_balance.step_flooding    += coupling_out_q;
        }
    }

    // Quality routing mass balance (matching legacy massbal_updateRoutingTotals)
    int np = ctx_.n_pollutants();
    if (np > 0) {
        for (int j = 0; j < ctx_.n_nodes(); ++j) {
            auto uj = static_cast<std::size_t>(j);

            // Wet weather quality inflow is NOT booked here. This used to add
            // lat_flow * node concentration for every node with lateral flow,
            // which is wrong twice over: the node's resulting concentration is
            // not the source's, and lat_flow lumps runoff together with DWF,
            // GW, RDII and direct [INFLOWS] — so each of those was counted a
            // second time as "wet weather". It read 0.000 only for as long as
            // direct pollutant inflows delivered no mass at all. Each source
            // now books its own load in its own QualitySolver adder, matching
            // legacy massbal_addInflowQual() call sites.

            // Quality outflow at outfalls: inflow × concentration
            if (ctx_.nodes.type[uj] == NodeType::OUTFALL && ctx_.nodes.inflow[uj] > 0.0) {
                for (int p = 0; p < np; ++p) {
                    auto qi = uj * static_cast<std::size_t>(np) + static_cast<std::size_t>(p);
                    if (qi < ctx_.nodes.conc.size()) {
                        double load = ctx_.nodes.inflow[uj] *
                                      ctx_.nodes.conc[qi] * dt_routing;
                        if (load > 0.0)
                            ctx_.mass_balance.qual_routing_outflow[static_cast<std::size_t>(p)] += load;
                    }
                }
            }
        }
    }
}

// ============================================================================
// computeFinalStorage() — final storage volumes for mass balance
// ============================================================================

/**
 * @brief Compute final storage volumes for runoff and routing mass balance.
 *
 * @details Computes runoff final storage from ponded depth on subareas,
 *          and routing final storage from node + link volumes.
 */
void SWMMEngine::computeFinalStorage() noexcept {
    // B8a. Compute runoff final storage: ponded depth × subarea area (ft³)
    //      Matches legacy subcatch_getStorage()
    {
        const auto& soa = runoff_.soa();
        ctx_.mass_balance.runoff_final_store = 0.0;
        for (int i = 0; i < ctx_.n_subcatches(); ++i) {
            auto ui = static_cast<std::size_t>(i);
            double fi = soa.imperv_pct[ui];
            double fp = 1.0 - fi;
            double f0 = fi * soa.imperv0_pct[ui];
            double f1 = fi * (1.0 - soa.imperv0_pct[ui]);
            double area = soa.area[ui]; // ft²
            ctx_.mass_balance.runoff_final_store +=
                (soa.depth_imperv0[ui] * f0
                 + soa.depth_imperv1[ui] * f1
                 + soa.depth_perv[ui] * fp) * area;
        }
        // Water still held in LID units is runoff-system storage (legacy
        // massbal counts lid_getStoredVolume() in final storage).
        ctx_.mass_balance.runoff_final_store += lid_.storedVolume();

        // F8 — the pack still standing, and the snow ploughed out of the
        // system. Legacy massbal.c:671-679.
        ctx_.mass_balance.runoff_final_snow =
            snowCoverVolumeFt3(ctx_, snow_.state());
        // `runoff_snowremov` HAD NO WRITER ANYWHERE. It was declared, exposed
        // through `SWMM_RUNOFF_SNOWREMOV`, returned by the mass-balance API
        // and read by callers — and nothing ever assigned it, so every deck
        // that ploughs snow reported zero removal while `SnowSoA::removed`
        // accumulated the real figure with no consumer. Same shape as F1
        // (`setMeltCoeffs` with no caller) and as the snapshot quality
        // vectors that wrote every pollutant column as zero: a field that
        // exists, is exposed, is read, and is never written.
        ctx_.mass_balance.runoff_snowremov = snow_.state().removed;
    }

    // (LID exfiltration / evaporation are folded into the runoff-continuity
    //  infil / evap terms per-step in accumulateRunoffMassBalance — this
    //  function is called every routing step, so cumulative terms cannot be
    //  added here.)

    // B8. Compute routing final storage for mass balance
    //     Sum node volumes + link volumes (matching legacy). Nodes use the
    //     legacy-convention reported volume (junctions contribute 0, as in
    //     legacy node_getVolume) so the continuity error matches legacy rather
    //     than crediting the MIN_SURFAREA junction-state volume as storage.
    ctx_.mass_balance.routing_final_storage = 0.0;
    for (int j = 0; j < ctx_.n_nodes(); ++j) {
        ctx_.mass_balance.routing_final_storage += reportedNodeVolume(j);
    }
    for (int j = 0; j < ctx_.n_links(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        ctx_.mass_balance.routing_final_storage += ctx_.links.volume[uj];
    }

    // B8a. Final stored pollutant mass — the closing term of the quality
    //      ledger, mirroring the opening term recorded in initMassBalance().
    //      Without it every quality continuity error was reported as the whole
    //      of the mass still in the system (~51 % on a steady feed).
    //      Legacy: massbal_getStoredMass().
    {
        const int np = ctx_.n_pollutants();
        for (int p = 0; p < np; ++p) {
            const auto up = static_cast<std::size_t>(p);
            if (up >= ctx_.mass_balance.qual_routing_final.size()) break;
            double m = 0.0;
            for (int j = 0; j < ctx_.n_nodes(); ++j) {
                const auto idx = static_cast<std::size_t>(j) *
                                 static_cast<std::size_t>(np) + up;
                if (idx < ctx_.nodes.conc.size())
                    m += ctx_.nodes.conc[idx] * reportedNodeVolume(j);
            }
            for (int j = 0; j < ctx_.n_links(); ++j) {
                const auto uj  = static_cast<std::size_t>(j);
                const auto idx = uj * static_cast<std::size_t>(np) + up;
                if (idx < ctx_.links.conc.size())
                    m += ctx_.links.conc[idx] * ctx_.links.volume[uj];
            }
            ctx_.mass_balance.qual_routing_final[up] = m;
        }
    }
}

// ============================================================================
// computeFinalQualityMassBalance() — final quality buildup mass balance
// ============================================================================

/**
 * @brief Compute final quality buildup mass for quality mass balance.
 *
 * @details Sums the current buildup across all subcatchments for each
 *          pollutant and stores it in the mass balance final buildup array.
 */
void SWMMEngine::computeFinalQualityMassBalance() noexcept {
    // B9. Quality mass balance: compute final buildup (sum over land uses)
    // Matches legacy massbal_getBuildup: sum landFactor[lu].buildup[p] * norm
    if (ctx_.n_pollutants() > 0) {
        int np = ctx_.n_pollutants();
        int nlu = std::max(ctx_.n_landuses(), 1);
        for (int p = 0; p < np; ++p) {
            double total = 0.0;
            for (int i = 0; i < ctx_.n_subcatches(); ++i) {
                auto ui = static_cast<std::size_t>(i);
                double area_ac = ctx_.subcatches.area[ui];
                for (int lu = 0; lu < nlu; ++lu) {
                    auto cov_idx = ui * static_cast<std::size_t>(nlu)
                                   + static_cast<std::size_t>(lu);
                    double frac = (cov_idx < ctx_.subcatches.coverage.size())
                                  ? ctx_.subcatches.coverage[cov_idx] / 100.0 : 0.0;
                    if (frac <= 0.0) continue;

                    auto k = static_cast<std::size_t>(lu * np + p);
                    const auto& bp = landuse_solver_.buildup_params[k];
                    double norm = (bp.normalizer == 0)
                        ? frac * area_ac : frac * ctx_.subcatches.curb_length[ui];

                    auto bu = surface_quality_.bu_idx(i, lu, p);
                    if (bu < surface_quality_.buildup.size())
                        total += surface_quality_.buildup[bu] * norm;
                }
            }
            ctx_.mass_balance.qual_final_buildup[static_cast<std::size_t>(p)] = total;
        }
    }
}

// ============================================================================
// postOutputSnapshot() — post snapshot to IO thread if output is due
// ============================================================================

namespace {
/// Convert the 1D portion of a snapshot from internal (ft/cfs/ft³) to project
/// display units, in place. This is the single conversion boundary: all output
/// plugins (.out, GeoPackage) then consume display-unit data directly instead
/// of each re-applying Ucf/Qcf. The 2D surface_* fields are SI-native and are
/// left untouched. Quality concentrations, capacity and soil-moisture are
/// dimensionless and are also left untouched. For US projects every factor is
/// 1.0 except rainfall/evap rates, so the byte-identical .out is preserved.
void convertSnapshotToDisplay(SimulationSnapshot& s, const ucf::DisplayUnits& du) {
    auto scale = [](std::vector<double>& v, double f) {
        if (f == 1.0) return;
        for (auto& x : v) x *= f;
    };
    // Subcatchments (legacy subcatch_getResults field order)
    scale(s.subcatch.rainfall,   du.rainfall);
    scale(s.subcatch.snow_depth, du.raindepth);
    scale(s.subcatch.evap,       du.evaprate);
    scale(s.subcatch.infil,      du.rainfall);
    scale(s.subcatch.runoff,     du.flow);
    scale(s.subcatch.gw_flow,    du.flow);
    scale(s.subcatch.gw_elev,    du.length);
    // Nodes (legacy node_getResults field order)
    scale(s.nodes.depth,          du.length);
    scale(s.nodes.head,           du.length);
    scale(s.nodes.volume,         du.volume);
    scale(s.nodes.lateral_inflow, du.flow);
    scale(s.nodes.total_inflow,   du.flow);
    scale(s.nodes.overflow,       du.flow);
    // Links (legacy link_getResults field order)
    scale(s.links.flow,     du.flow);
    scale(s.links.depth,    du.length);
    scale(s.links.velocity, du.length);
    scale(s.links.volume,   du.volume);
    // System scalars (legacy SysResults order).
    // The temperature conversion is AFFINE, so it must be skipped — not merely
    // fed a zero — on a model with no subcatchments; see
    // SimulationSnapshot::has_subcatchments.
    if (s.has_subcatchments)
        s.sys_temperature = du.temperature(s.sys_temperature);
    s.sys_rainfall   *= du.rainfall;
    s.sys_snow_depth *= du.raindepth;
    s.sys_infil      *= du.rainfall;
    s.sys_runoff     *= du.flow;
    s.sys_dw_inflow  *= du.flow;
    s.sys_gw_inflow  *= du.flow;
    s.sys_ii_inflow  *= du.flow;
    s.sys_ext_inflow *= du.flow;
    s.sys_flooding   *= du.flow;
    s.sys_outflow    *= du.flow;
    s.sys_storage    *= du.volume;
    s.sys_evap       *= du.evaprate;
    s.sys_pet        *= du.evaprate;
}
} // namespace

/**
 * @brief Post a snapshot to the IO thread if output is due.
 *
 * @details Checks the output timer and, if a report interval has elapsed,
 *          builds a SimulationSnapshot from the current context state and
 *          posts it to the IO thread for asynchronous writing.
 */
void SWMMEngine::postOutputSnapshot(double /*dt_step*/) noexcept {
    // Post snapshot to IO thread if output is due (Phase 5)
    // Called AFTER advance(). State arrays (lat_flow, depth, flow, infil etc.)
    // still reflect the just-completed routing step. current_date is at the
    // report boundary.
    if (hydraulics::TimestepController::output_due(ctx_)) {
        // Legacy-parity report datestamp: getDateTime(ReportTime) ==
        // datetime_addSeconds(StartDateTime, (elapsedMsec + 1) / 1000.0)
        // (swmm5.c:3543-3546 — note the legacy +1 ms fudge, visible as the
        // ".001" in every legacy .out datestamp).
        const double report_date = datetime::addSeconds(
            ctx_.options.start_date, (ctx_.next_report_ms + 1.0) / 1000.0);

        // REPORT_START gate — legacy output_saveResults() (output.c:481):
        // periods before ReportStart are not written (no bytes, no Nperiods,
        // no interface row), but the report grid still advances.
        if (report_date < ctx_.options.report_start) {
            hydraulics::TimestepController::reset_output_timer(ctx_);
            return;
        }

        // Routing interface file: write one outfall row per reporting step
        // (legacy output_saveResults() → iface_saveOutletResults()). Not
        // gated on save_results_ — the interface file is a model-coupling
        // artifact, independent of .out persistence. IGNORE_ROUTING suppresses
        // it (legacy saves outflows only when !IgnoreRouting, output.c:519).
        if (do_routing_) {
            iface_.writeOutfallResults(ctx_, report_date);
        }

        if (save_results_ && !plugins_.empty()) {
            // Build a SimulationSnapshot from the current context.
            // Vector assignments are O(n) memcpy; the I/O thread consumes
            // and destroys the snapshot after processing.
            SimulationSnapshot snap;
            snap.sim_time         = report_date;
            snap.node_count       = ctx_.n_nodes();
            snap.link_count       = ctx_.n_links();
            snap.subcatch_count   = ctx_.n_subcatches();
            snap.gage_count       = ctx_.n_gages();
            // A2b: the reported species block is pollutants + the water-age
            // pseudo-column, so consumers stride by the REPORTED count.
            snap.pollut_count     = ctx_.n_reported_species();
            snap.flow_units_code  = static_cast<int>(ctx_.options.flow_units);

            // Legacy-parity interpolation weight at the report instant
            // (output.c:650/682): f = (reportTime - OldRoutingTime) /
            //                         (NewRoutingTime - OldRoutingTime),
            // all on the ms clock. The routing step brackets the report time
            // (elapsed_ms >= next_report_ms > old_elapsed_ms) so f ∈ (0, 1].
            const double route_span = ctx_.elapsed_ms - ctx_.old_elapsed_ms;
            const double f_rt = (route_span > 0.0)
                ? (ctx_.next_report_ms - ctx_.old_elapsed_ms) / route_span
                : 1.0;
            const double f1_rt = 1.0 - f_rt;

            // Node results — legacy node_getResults (node.c:483-494):
            // depth/volume/latflow/inflow time-weighted between old and new;
            // overflow is point-in-time. Volume interpolates the legacy-mapped
            // old/new volumes (junctions => 0, storage => curve volume).
            {
                const int nN = ctx_.n_nodes();
                snap.nodes.depth.resize(static_cast<std::size_t>(nN));
                snap.nodes.head.resize(static_cast<std::size_t>(nN));
                snap.nodes.volume.resize(static_cast<std::size_t>(nN));
                snap.nodes.lateral_inflow.resize(static_cast<std::size_t>(nN));
                snap.nodes.total_inflow.resize(static_cast<std::size_t>(nN));
                for (int i = 0; i < nN; ++i) {
                    auto ui = static_cast<std::size_t>(i);
                    const double y = f1_rt * ctx_.nodes.old_depth[ui]
                                   + f_rt  * ctx_.nodes.depth[ui];
                    snap.nodes.depth[ui] = y;
                    // Placeholder; rewritten below in float32 arithmetic to
                    // reproduce legacy NODE_HEAD bit-for-bit. Under
                    // REPORT_SIGNED_HEADS (issue #156 O-6) the HEAD field
                    // carries the TRUE signed head, interpolated like every
                    // other snapshot field via the SIGNED offset head−invert
                    // (== depth exactly for any node at/above its invert, so
                    // never-negative decks differ from the legacy rewrite by
                    // float32 rounding only). There is no old_head array; the
                    // old endpoint uses the floored old_depth, so a sub-invert
                    // excursion is attenuated toward the invert only when the
                    // report instant falls strictly inside a routing span.
                    // DEPTH stays floored either way. (A point-in-time head
                    // was measured 2.175 ft off the interpolated series on a
                    // never-negative FV corpus deck — P5 task-2 finding.)
                    snap.nodes.head[ui] = ctx_.options.report_signed_heads
                        ? ctx_.nodes.invert_elev[ui]
                          + f1_rt * ctx_.nodes.old_depth[ui]
                          + f_rt  * (ctx_.nodes.head[ui]
                                     - ctx_.nodes.invert_elev[ui])
                        : ctx_.nodes.invert_elev[ui] + y;
                    snap.nodes.volume[ui] =
                        f1_rt * reportedNodeVolume(i, ctx_.nodes.old_depth[ui],
                                                   ctx_.nodes.old_volume[ui])
                      + f_rt  * reportedNodeVolume(i, ctx_.nodes.depth[ui],
                                                   ctx_.nodes.volume[ui]);
                    snap.nodes.lateral_inflow[ui] =
                        f1_rt * ctx_.nodes.old_lat_flow[ui]
                      + f_rt  * ctx_.nodes.lat_flow[ui];
                    snap.nodes.total_inflow[ui] =
                        f1_rt * ctx_.nodes.old_inflow[ui]
                      + f_rt  * ctx_.nodes.inflow[ui];
                }
            }
            snap.nodes.overflow = ctx_.nodes.overflow;

            // Link results — legacy link_getResults (link.c:693-720):
            // depth/flow/volume time-weighted between old and new; velocity
            // computed FROM the interpolated q and y (not interpolated
            // itself); capacity from the interpolated depth; pump flow snaps
            // to old/new across on/off transitions; direction applied last.
            {
                const int nL = ctx_.n_links();
                snap.links.flow.resize(static_cast<std::size_t>(nL));
                snap.links.depth.resize(static_cast<std::size_t>(nL));
                snap.links.volume.resize(static_cast<std::size_t>(nL));
                snap.links.velocity.resize(static_cast<std::size_t>(nL));
                snap.links.capacity.resize(static_cast<std::size_t>(nL));
                ensureXspCache();
                for (int j = 0; j < nL; ++j) {
                    auto uj = static_cast<std::size_t>(j);
                    double y = f1_rt * ctx_.links.old_depth[uj]
                             + f_rt  * ctx_.links.depth[uj];
                    double q = f1_rt * ctx_.links.old_flow[uj]
                             + f_rt  * ctx_.links.flow[uj];
                    double v = f1_rt * ctx_.links.old_volume[uj]
                             + f_rt  * ctx_.links.volume[uj];
                    double veloc = 0.0;
                    double cap   = 0.0;

                    auto lt = ctx_.links.type[uj];
                    if (lt == LinkType::CONDUIT) {
                        const XSectParams& xs = xsp_cache_[uj];
                        const int cr = ctx_.link_subtypes.conduit_row(j);
                        const int nb = (cr >= 0) ? ctx_.link_subtypes.conduits.barrels[static_cast<std::size_t>(cr)] : 1;
                        veloc = link::getVelocity(xs, q, y, nb);
                        cap   = link::getCapacity(xs, y);
                    } else {
                        cap = ctx_.links.setting[uj];
                    }

                    // Pump on/off override (link.c:706-710): no time-weighting
                    // across a start/stop — snap to the nearer state's flow.
                    if (lt == LinkType::PUMP &&
                        ctx_.links.old_flow[uj] * ctx_.links.flow[uj] == 0.0) {
                        q = (f_rt >= f1_rt) ? ctx_.links.flow[uj]
                                            : ctx_.links.old_flow[uj];
                    }

                    // Apply link direction (matching legacy link_getResults)
                    int dir = ctx_.links.direction[uj];
                    snap.links.depth[uj]    = y;
                    snap.links.flow[uj]     = q * dir;
                    snap.links.velocity[uj] = veloc * dir;
                    snap.links.volume[uj]   = v;
                    snap.links.capacity[uj] = cap;
                }
            }

            // When rpt_averages is enabled, the node/link snapshot arrays are
            // overwritten with time-step averages AFTER the display-unit
            // conversion below (the accumulators hold float32 DISPLAY values,
            // legacy output.c:72). The averaging machinery covers ONLY nodes
            // and links — legacy writes subcatchment results via
            // subcatch_getResults(j, f) unconditionally (output.c:479-490),
            // so the interpolated subcatch blocks below run in ALL modes.

            // Copy subcatchment state
            // Rainfall, infil, and evap all come from the most recent runoff
            // evaluation and are self-consistent. Using ctx_.subcatches.rainfall
            // (set during runoff) rather than querying the gage at report time
            // ensures rain and infil are time-aligned.
            snap.subcatch.rainfall = ctx_.subcatches.rainfall;
            snap.subcatch.evap     = ctx_.subcatches.evap_loss;
            snap.subcatch.infil    = ctx_.subcatches.infil_loss;
            snap.subcatch.runoff   = ctx_.subcatches.runoff;
            snap.subcatch.gw_flow  = ctx_.subcatches.gw_flow;

            // Legacy-parity reported losses on LID subcatchments. Legacy
            // (subcatch.c:740-744) reports rates averaged over the FULL
            // subcatchment area and includes the LID units' own losses:
            //   evapLoss  = Vevap / tStep / area   (Vevap includes LID evap)
            //   infilLoss = (Vinfil + VlidInfil) / tStep / area
            // This engine keeps evap_loss/infil_loss averaged over the
            // NON-LID area with the LID losses ledgered separately (see
            // accumulateRunoffMassBalance) — coherent internally, but the
            // reported value must be legacy's. Rebuild it here from the
            // non-LID rate and the LID units' last-step losses (per-step
            // depths over each unit's area; the last runoff step's span is
            // the divisor legacy used when it formed the rate).
            {
                const double land2ft2 =
                    1.0 / ucf::UCF(ucf::LANDAREA, ctx_.options);
                const double dt_last =
                    (new_runoff_ms_ - old_runoff_ms_) / 1000.0;
                const auto& rsoa = runoff_.soa();
                std::vector<double> lid_evap_cfs, lid_infil_cfs;
                for (int t = 0; t < lid_.numGroups(); ++t) {
                    const auto& g = lid_.group(t);
                    for (int u = 0; u < g.count; ++u) {
                        auto uu = static_cast<std::size_t>(u);
                        int sc = g.subcatch_idx[uu];
                        if (sc < 0 || sc >= ctx_.n_subcatches() ||
                            dt_last <= 0.0) continue;
                        if (lid_evap_cfs.empty()) {
                            auto un = static_cast<std::size_t>(
                                ctx_.n_subcatches());
                            lid_evap_cfs.assign(un, 0.0);
                            lid_infil_cfs.assign(un, 0.0);
                        }
                        auto usc = static_cast<std::size_t>(sc);
                        lid_evap_cfs[usc] +=
                            g.evap_loss[uu] * g.area[uu] / dt_last;
                        lid_infil_cfs[usc] +=
                            g.infil_loss[uu] * g.area[uu] / dt_last;
                    }
                }
                for (int i = 0; i < ctx_.n_subcatches(); ++i) {
                    auto ui = static_cast<std::size_t>(i);
                    if (ctx_.subcatches.total_lid_area_ft2[ui] <= 0.0)
                        continue;
                    const double full_ft2 =
                        ctx_.subcatches.area[ui] * land2ft2;
                    if (full_ft2 <= 0.0) continue;
                    const double nonlid_ft2 = rsoa.area[ui];
                    const double ev = lid_evap_cfs.empty()
                        ? 0.0 : lid_evap_cfs[ui];
                    const double in = lid_infil_cfs.empty()
                        ? 0.0 : lid_infil_cfs[ui];
                    snap.subcatch.evap[ui] =
                        (ctx_.subcatches.evap_loss[ui] * nonlid_ft2 + ev)
                        / full_ft2;
                    snap.subcatch.infil[ui] =
                        (ctx_.subcatches.infil_loss[ui] * nonlid_ft2 + in)
                        / full_ft2;
                }
            }

            // Report subcatchment RUNOFF time-interpolated between the old/new
            // WET_STEP values, matching legacy subcatch_getResults (subcatch.c:865)
            // + output.c:323 which use f = (reportTime-OldRunoffTime)/span. The
            // routing lateral inflow already applies this interpolation; the .out
            // column previously used the raw constant new value, so the reported
            // runoff stepped instead of ramping within a WET_STEP. This is
            // OUTPUT-ONLY (the applied routing inflow is unchanged). Legacy also
            // zeroes runoff below MIN_RUNOFF * area_ft2. Runs in ALL report
            // modes — the averages machinery covers only nodes/links; legacy
            // interpolates subcatchments unconditionally (output.c:479-490).
            {
                constexpr double MIN_RUNOFF = 2.31481e-8;  // ft/s (legacy consts.h)
                // Weight at the REPORT instant (legacy output.c:593):
                // f = (reportTime - OldRunoffTime)/(NewRunoffTime - OldRunoffTime).
                // PARITY: formed entirely in the MILLISECOND clock domain —
                // next_report_ms is the legacy report grid (ms) and the
                // runoff clock ms mirrors accumulate with the legacy ops.
                // No [0,1] clamp (legacy has none; the runoff window brackets
                // the report time).
                const double span = new_runoff_ms_ - old_runoff_ms_;
                double f = (span > 0.0)
                         ? (ctx_.next_report_ms - old_runoff_ms_) / span
                         : 1.0;
                const double f1 = 1.0 - f;
                const double land2ft2 = 1.0 / ucf::UCF(ucf::LANDAREA, ctx_.options);
                for (int i = 0; i < ctx_.n_subcatches(); ++i) {
                    auto ui = static_cast<std::size_t>(i);
                    double ro = f1 * ctx_.subcatches.old_runoff[ui]
                              + f  * ctx_.subcatches.runoff[ui];
                    // Legacy adds the interpolated LID drain to reported
                    // runoff BEFORE the MIN_RUNOFF cutoff, gated on
                    // lidArea > 0 (subcatch.c:897-905).
                    if (ctx_.subcatches.total_lid_area_ft2[ui] > 0.0)
                        ro += f1 * ctx_.subcatches.old_lid_drain_flow[ui]
                            + f  * ctx_.subcatches.lid_drain_flow[ui];
                    if (ro < MIN_RUNOFF * (ctx_.subcatches.area[ui] * land2ft2))
                        ro = 0.0;
                    snap.subcatch.runoff[ui] = ro;
                    // Legacy interpolates GW flow on the same runoff-clock
                    // weight (subcatch.c:909-910); the raw copy above only
                    // sized the array.
                    snap.subcatch.gw_flow[ui] =
                        f1 * ctx_.subcatches.old_gw_flow[ui]
                        + f * ctx_.subcatches.gw_flow[ui];
                }
            }

            // GW elevation and soil moisture (matching legacy subcatch_getResults):
            //   gw_elev   = (bottomElev + lowerDepth) * UCF(LENGTH)
            //   soil_moist = theta (upper zone moisture content)
            {
                const int nS = ctx_.n_subcatches();
                snap.subcatch.gw_elev.resize(static_cast<std::size_t>(nS), 0.0);
                snap.subcatch.soil_moist.resize(static_cast<std::size_t>(nS), 0.0);
                const auto& gw = groundwater_.state();
                for (int s = 0; s < nS; ++s) {
                    auto us = static_cast<std::size_t>(s);
                    int aq = ctx_.subcatches.gw_aquifer[us];
                    if (aq >= 0) {
                        auto uaq = static_cast<std::size_t>(aq);
                        double bot = ctx_.aquifers.bottom_elev[uaq];
                        snap.subcatch.gw_elev[us] = bot + gw.lower_depth[us];
                        snap.subcatch.soil_moist[us] = gw.theta[us];
                    }
                }
            }

            // Per-subcatch snow depth: legacy interpolates oldSnowDepth/
            // newSnowDepth to the report instant on the runoff-clock weight
            // (subcatch.c:888-890) — in ALL report modes. The per-step SWE
            // recompute now lives in stepRunoff (ctx_.subcatches.snow_depth).
            {
                const int nS = ctx_.n_subcatches();
                snap.subcatch.snow_depth.resize(static_cast<std::size_t>(nS), 0.0);
                const double span = new_runoff_ms_ - old_runoff_ms_;
                const double f = (span > 0.0)
                               ? (ctx_.next_report_ms - old_runoff_ms_) / span
                               : 1.0;
                const double f1 = 1.0 - f;
                for (int s = 0; s < nS; ++s) {
                    auto us = static_cast<std::size_t>(s);
                    snap.subcatch.snow_depth[us] =
                        f1 * ctx_.subcatches.old_snow_depth[us]
                        + f * ctx_.subcatches.snow_depth[us];
                }
            }

            // System-level results.
            //
            // Legacy assigns SYS_TEMPERATURE and SYS_PET at the END of
            // output_saveSubcatchResults, and that function is called only
            // behind `if (Nobjects[SUBCATCH] > 0)` (output.c:489-490). On a
            // deck with NO subcatchments both therefore stay at the
            // zero-initialised value of SysResults — regardless of the
            // temperature data or the unit system. Reproducing that is not
            // cosmetic: on an SI model this engine correctly converts its
            // 0 degF to -17.78 degC, which is a defensible number and a parity
            // failure on every period (extran-family and other pipe-only decks
            // are exactly this shape).
            const bool has_subcatchments = ctx_.n_subcatches() > 0;
            snap.has_subcatchments = has_subcatchments;
            snap.sys_temperature =
                has_subcatchments ? ctx_.climate_state.temperature : 0.0;

            // The subcatchment-derived system results (rainfall, snow depth,
            // evap, infil, runoff) are computed AFTER the unit-conversion
            // boundary from the reported per-subcatch float32 values — see the
            // legacy-REAL4 accumulation block below. Zero them here so a deck
            // with no subcatchments writes legacy's zero-initialised
            // SysResults and the conversion boundary scales inert values.
            snap.sys_rainfall   = 0.0;
            snap.sys_snow_depth = 0.0;
            snap.sys_evap       = 0.0;
            snap.sys_infil      = 0.0;
            snap.sys_runoff     = 0.0;
            snap.sys_pet = has_subcatchments ? ctx_.climate_state.evap_rate : 0.0;

            snap.sys_dw_inflow  = ctx_.mass_balance.step_dw_inflow;
            snap.sys_gw_inflow  = ctx_.mass_balance.step_gw_inflow;
            snap.sys_ii_inflow  = ctx_.mass_balance.step_rdii_inflow;
            snap.sys_ext_inflow = ctx_.mass_balance.step_ext_inflow;
            snap.sys_flooding   = ctx_.mass_balance.step_flooding;
            snap.sys_outflow    = ctx_.mass_balance.step_outflow;

            // Total storage volume — the SUM OF THE REPORTED node and link
            // volumes, not of the raw end-of-step state. Legacy accumulates
            // SYS_STORAGE from the routing-time-INTERPOLATED NodeResults /
            // LinkResults it is writing (output.c:667, 700), in a REAL4
            // accumulator (element-by-element float rounding). Summing the
            // un-interpolated ctx state diverged from the per-element output
            // during transients (deadend repro: 1.0127 vs 1.0046 at period 0)
            // even though every element cell matched. snap.nodes/links.volume
            // are internal-unit; legacy accumulates display-unit REAL4 —
            // convert per element before the float add, then return to
            // internal units (convertSnapshotToDisplay scales sys_storage).
            {
                const double vol_ucf = ucf::Ucf[ucf::VOLUME][
                    ucf::getUnitSystem(static_cast<int>(ctx_.options.flow_units))];
                float tot_store = 0.0f;
                if (ctx_.options.rpt_averages) {
                    // Averages mode: legacy output_saveAvgResults sums the
                    // RAW end-of-step volumes over ALL nodes then ALL links
                    // (output.c:926-948, Node/Link newVolume), not the
                    // averaged or interpolated report values.
                    for (int j = 0; j < ctx_.n_nodes(); ++j)
                        tot_store += static_cast<float>(
                            reportedNodeVolume(j) * vol_ucf);
                    for (std::size_t uj = 0; uj < ctx_.links.volume.size(); ++uj)
                        tot_store += static_cast<float>(
                            ctx_.links.volume[uj] * vol_ucf);
                } else {
                    for (std::size_t uj = 0; uj < snap.nodes.volume.size(); ++uj)
                        tot_store += static_cast<float>(snap.nodes.volume[uj] * vol_ucf);
                    for (std::size_t uj = 0; uj < snap.links.volume.size(); ++uj)
                        tot_store += static_cast<float>(snap.links.volume[uj] * vol_ucf);
                }
                snap.sys_storage = static_cast<double>(tot_store) / vol_ucf;
            }

            // 2D surface routing state (deep-copied; empty when 2D inactive,
            // and Default2DOutputPlugin::update() short-circuits on
            // surface_tri_count == 0). The render fields refresh on the
            // REPORT_STEP grid; bring them up to date first for the case where
            // the report instant does not land on a co-advance batch boundary.
#ifdef OPENSWMM_HAS_2D
            if (surface_router_.isActive())
                surface_router_.refreshRenderFieldsIfStale();
#endif
            fillSurfaceSnapshot(snap);

            // Single conversion boundary: convert the 1D snapshot to project
            // display units once here so all output plugins consume display
            // data directly (2D surface_* fields stay SI-native, untouched).
            const auto du = ucf::DisplayUnits::from(ctx_.options);
            convertSnapshotToDisplay(snap, du);

            // Averages mode: overwrite node/link arrays with the float32
            // display-unit averages (legacy output_saveAvgResults). Must be
            // after the conversion boundary — the accumulators already hold
            // display values.
            if (ctx_.options.rpt_averages) {
                applyAvgResults(snap);
            }

            // Reproduce legacy node_getResults() NODE_HEAD bit-for-bit (node.c:484-486):
            //   x[NODE_DEPTH] = (float)(depth_disp);
            //   x[NODE_HEAD]  = x[NODE_DEPTH] + (float)(invertElev * UCF(LENGTH));
            // i.e. head = f32( f32(depth_disp) + f32(invert_disp) ): two SEPARATELY
            // float32-rounded terms summed in float arithmetic (double-rounded depth).
            // Our routed head double is the more-accurate single-rounded
            // f32(invert+depth); that differs from legacy by +/-1 ULP on ~15% of rows.
            // This is an OUTPUT-ONLY rewrite of snap.nodes.head: it touches neither
            // ctx_.nodes.head nor depth/flow/volume, so routing parity is preserved.
            // Term-by-term (not via head-depth subtraction) so it is exact in every
            // unit system, including SI where du.length != 1.0. Skipped under
            // rpt_averages (that path averages the double head; legacy averages the
            // per-step float32 head — a separate concern from this 1-ULP instant bug).
            // Skipped under REPORT_SIGNED_HEADS (issue #156 O-6): the parity
            // rewrite derives head from the FLOORED depth, which is exactly
            // what the option exists to bypass.
            if (!ctx_.options.rpt_averages &&
                !ctx_.options.report_signed_heads) {
                const std::size_t nn =
                    std::min<std::size_t>(snap.nodes.head.size(), snap.nodes.depth.size());
                for (std::size_t i = 0; i < nn; ++i) {
                    const float depth_f32  = static_cast<float>(snap.nodes.depth[i]);
                    const float invert_f32 =
                        static_cast<float>(ctx_.nodes.invert_elev[i] * du.length);
                    snap.nodes.head[i] =
                        static_cast<double>(depth_f32 + invert_f32);
                }
            }

            // Legacy-parity subcatchment RAINFALL: legacy reports the gage's
            // report-time rainfall (gage_setReportRainfall at the report
            // instant, output.c:587-590; subcatch.c:853-855) in the gage's
            // NATIVE intensity units with no UCF applied. Overwrite the
            // display-converted runoff-step rainfall AFTER the unit-conversion
            // boundary so the value reaches the writer exactly as legacy's —
            // no internal-unit round trip.
            {
                const int nG = ctx_.n_gages();
                std::vector<double> gage_report(static_cast<std::size_t>(nG), 0.0);
                for (int g = 0; g < nG; ++g)
                    gage_report[static_cast<std::size_t>(g)] =
                        gage::getReportRainfall(ctx_, g, report_date);
                const std::size_t nS = snap.subcatch.rainfall.size();
                for (std::size_t s = 0; s < nS; ++s) {
                    int g = ctx_.subcatches.gage[s];
                    snap.subcatch.rainfall[s] =
                        (g >= 0 && g < nG)
                            ? gage_report[static_cast<std::size_t>(g)] : 0.0;
                }
            }

            // Legacy-parity subcatchment-derived system results. Legacy
            // accumulates SysResults (a REAL4 array) from the REPORTED
            // per-subcatch float32 display-unit values as it writes them
            // (output.c:596-630): SYS_RUNOFF is a plain float sum of
            // SUBCATCH_RUNOFF, the others are area-weighted with a REAL4
            // running totalArea and divided in float at the end. Summing the
            // raw double state diverged three ways: no report-time
            // interpolation, no MIN_RUNOFF zeroing (an LID draining to
            // pervious keeps a sub-threshold trickle alive that legacy
            // reports as 0.0 system runoff for the rest of the run), and
            // double instead of float32 rounding. The writer's SYS_INFLOW
            // (reader name TOTAL_LATFLOW) is a float sum that includes
            // SYS_RUNOFF, so it inherits this fix. Placed after the
            // conversion boundary (and after the gage report-rainfall
            // overwrite) so the accumulation sees exactly the float32 values
            // the writer emits; ctx_.subcatches.area is already in display
            // land-area units (legacy's Subcatch[j].area * UCF(LANDAREA)).
            if (has_subcatchments) {
                float total_area = 0.0f;
                float a_rain = 0.0f, a_snow = 0.0f, a_evap = 0.0f;
                float a_infil = 0.0f, a_runoff = 0.0f;
                const auto& gw = groundwater_.state();
                for (int i = 0; i < ctx_.n_subcatches(); ++i) {
                    auto ui = static_cast<std::size_t>(i);
                    const double area = ctx_.subcatches.area[ui];
                    total_area += static_cast<float>(area);
                    a_rain += static_cast<float>(
                        static_cast<float>(snap.subcatch.rainfall[ui]) * area);
                    a_snow += static_cast<float>(
                        static_cast<float>(snap.subcatch.snow_depth[ui]) * area);
                    a_evap += static_cast<float>(
                        static_cast<float>(snap.subcatch.evap[ui]) * area);
                    // Legacy adds GW evaporation un-rounded (output.c:616-617):
                    // SYS_EVAP += (REAL4)(gw->evapLoss * UCF(EVAPRATE) * area)
                    if (ctx_.subcatches.gw_aquifer[ui] >= 0 &&
                        ui < gw.upper_evap.size()) {
                        a_evap += static_cast<float>(
                            (gw.upper_evap[ui] + gw.lower_evap[ui]) *
                            du.evaprate * area);
                    }
                    a_infil += static_cast<float>(
                        static_cast<float>(snap.subcatch.infil[ui]) * area);
                    a_runoff += static_cast<float>(snap.subcatch.runoff[ui]);
                }
                if (total_area > 0.0) {
                    a_evap  /= total_area;
                    a_rain  /= total_area;
                    a_snow  /= total_area;
                    a_infil /= total_area;
                }
                snap.sys_rainfall   = static_cast<double>(a_rain);
                snap.sys_snow_depth = static_cast<double>(a_snow);
                snap.sys_evap       = static_cast<double>(a_evap);
                snap.sys_infil      = static_cast<double>(a_infil);
                snap.sys_runoff     = static_cast<double>(a_runoff);
            }

            // ---------------------------------------------------------------
            // Pollutant concentrations.
            //
            // These three vectors are what DefaultOutputPlugin (binary .out)
            // and GeoPackageOutputPlugin write into their pollutant columns.
            // NOTHING populated them: both readers guard with
            // `qi < size()` and fall back to 0.0, so every pollutant column
            // in every .out file was written as ZERO while the header
            // advertised the column count and unit codes. The engine's own
            // state was correct throughout (nodes.conc / links.conc carry
            // the routed values, which is why the transport gates — all of
            // which read the arrays directly — never saw it).
            //
            // Interpolation matches the neighbouring fields AND legacy:
            //   node.c:502    z = f1*oldQual[p] + wt*newQual[p]
            //   link.c:724    c = f1*oldQual[p] + f *newQual[p]
            //   subcatch.c:929-930  runoff == 0 ? 0 : f1*old + wt*new
            // Concentrations are already in user units (no UCF applies).
            // IGNORE_QUALITY leaves the vectors EMPTY, which is also what
            // the writer wants (it sets n_polluts_ = 0 in that mode).
            //
            // A2b: the reported block is (pollutants, then water age when
            // enabled) — see ctx_.reported_species_names. TWO strides are in
            // play and must not be confused (lessons 14/15): the SOURCE
            // arrays are np-strided (nodes.conc etc.), the REPORTED arrays
            // are nr-strided. Age converts SECONDS → HOURS here, the unit
            // the plan reports it in (§1).
            if (!ctx_.options.ignore_quality &&
                ctx_.n_reported_species() > 0) {
                const auto np_s = static_cast<std::size_t>(ctx_.n_pollutants());
                const auto nr_s =
                    static_cast<std::size_t>(ctx_.n_reported_species());
                const auto nN_s = static_cast<std::size_t>(ctx_.n_nodes());
                const auto nL_s = static_cast<std::size_t>(ctx_.n_links());
                const auto nS_s = static_cast<std::size_t>(ctx_.n_subcatches());
                const bool age_col = ctx_.options.water_age && nr_s > np_s;
                // H1: temperature is the trailing column, after age when
                // both are on. Its index is derived from the SAME toggle
                // that built reported_species_names, so the two cannot
                // disagree without the species-name gate catching it.
                const bool temp_col = ctx_.options.heat_transport;
                const std::size_t temp_i = np_s + (ctx_.options.water_age ? 1u : 0u);
                constexpr double kSecPerHour = 3600.0;
                // Reported-depth threshold for "this element holds water".
                // Deliberately tiny: the intent is to exclude elements the
                // report itself shows as empty, not to impose a physical
                // wetting depth.
                constexpr double kDryReportDepth = 1.0e-9;

                snap.node_quality.assign(nN_s * nr_s, 0.0);
                for (std::size_t n = 0; n < nN_s; ++n) {
                    for (std::size_t p = 0; p < np_s; ++p) {
                        const std::size_t src = n * np_s + p;
                        if (src < ctx_.nodes.conc.size() &&
                            src < ctx_.nodes.conc_old.size())
                            snap.node_quality[n * nr_s + p] =
                                f1_rt * ctx_.nodes.conc_old[src] +
                                f_rt * ctx_.nodes.conc[src];
                    }
                    // Age is a published state (already the step's value),
                    // so it takes no old/new interpolation — there is no
                    // node_age_old to weight against.
                    //
                    // DRY-ELEMENT MASK (A2b carry c). An element with no
                    // water still ages in the STATE — it has to, or a
                    // refilling pipe would jump discontinuously and a
                    // hotstart would lose the age it must restore — but
                    // REPORTING that age is nonsense: validation saw links
                    // publish exactly 6.000000 h of age on water that never
                    // existed. The mask lives at the report boundary only,
                    // keyed on the element's own REPORTED depth, so the
                    // record is internally consistent (a reader seeing
                    // depth 0 sees age 0) and the state is untouched. Same
                    // shape as legacy's washoff runoff gate
                    // (subcatch.c:929), which reports 0 rather than a
                    // residual concentration.
                    //
                    // Keyed on DEPTH, not reported volume: legacy maps a
                    // junction's reported volume to 0 by convention, so a
                    // volume test would mask every junction age in the
                    // model.
                    if (age_col && n < ctx_.water_age_state.node_age.size()) {
                        const bool wet = (n < snap.nodes.depth.size() &&
                                          snap.nodes.depth[n] > kDryReportDepth);
                        snap.node_quality[n * nr_s + np_s] =
                            wet ? ctx_.water_age_state.node_age[n] / kSecPerHour
                                : 0.0;
                    }
                    // H1 temperature. NOTE THE OPPOSITE CALL from age above,
                    // and it is deliberate: there is NO dry-element mask on
                    // temperature. The age mask can write 0 because zero age
                    // is unambiguous — no water has ever been 0 h old and
                    // present. Zero DEGREES is an ordinary temperature, so a
                    // mask here would publish "freezing" for "empty" and a
                    // reader could not tell the two apart. Reporting the
                    // carried state is the lesser of the two wrongs; it is
                    // recorded as a convention rather than defended as
                    // correct, and the honest fix is a per-column no-data
                    // convention the .out format does not currently have.
                    if (temp_col && n < ctx_.heat_state.node_temp.size())
                        snap.node_quality[n * nr_s + temp_i] =
                            ctx_.heat_state.node_temp[n];
                }

                snap.link_quality.assign(nL_s * nr_s, 0.0);
                for (std::size_t l = 0; l < nL_s; ++l) {
                    for (std::size_t p = 0; p < np_s; ++p) {
                        const std::size_t src = l * np_s + p;
                        if (src < ctx_.links.conc.size() &&
                            src < ctx_.links.conc_old.size())
                            snap.link_quality[l * nr_s + p] =
                                f1_rt * ctx_.links.conc_old[src] +
                                f_rt * ctx_.links.conc[src];
                    }
                    // Dry-element mask — see the node comment above, but note
                    // the test differs, and has to. A node reports depth 0
                    // when empty; a LINK never does, because the dynamic-wave
                    // router floors a dry conduit at FUDGE (1e-4 ft). That is
                    // 1e5 times any "tiny" depth threshold, so a depth test
                    // cannot fire on the very case this mask exists for —
                    // measured on the bone-dry gate deck: depth
                    // 9.99999975e-05 and age 6.166667 h, the original defect
                    // untouched.
                    //
                    // Links therefore key on VOLUME (the field that IS
                    // meaningful for them — the inverse of the node trap),
                    // against the volume the quality subsystem itself calls
                    // empty. Read from INTERNAL state so the comparison is
                    // unit-safe: snap volumes are in user units, ZERO_VOLUME
                    // is ft3. Using the transport's own criterion means the
                    // age is reported exactly when the age was routed.
                    //
                    // ...OR the link is CONVEYING water. A pump, orifice or
                    // weir stores nothing — volume is 0 by construction — but
                    // it carries flow, and its link_age is its upstream
                    // node's age, which is a real number a user wants
                    // (measured: pump 0.124060 h against node J1's 0.124060).
                    // A volume-only test would blank the age of every
                    // regulator in every model, which is the node trap again
                    // with the fields exchanged. An idle regulator has
                    // neither volume nor flow and is still masked.
                    if (age_col && l < ctx_.water_age_state.link_age.size()) {
                        const bool holds =
                            (l < ctx_.links.volume.size() &&
                             ctx_.links.volume[l] > quality::ZERO_VOLUME);
                        const bool conveys =
                            (l < ctx_.links.flow.size() &&
                             std::fabs(ctx_.links.flow[l]) > constants::TINY);
                        const bool wet = holds || conveys;
                        snap.link_quality[l * nr_s + np_s] =
                            wet ? ctx_.water_age_state.link_age[l] / kSecPerHour
                                : 0.0;
                    }
                    // H1 temperature — unmasked, for the reason given in the
                    // node loop (0 degC is a real temperature, so it cannot
                    // double as "no water").
                    if (temp_col && l < ctx_.heat_state.link_temp.size())
                        snap.link_quality[l * nr_s + temp_i] =
                            ctx_.heat_state.link_temp[l];
                }

                // Subcatchment washoff carries legacy's runoff gate: a
                // subcatchment with no runoff reports 0, not its residual
                // concentration (subcatch.c:929). A3 landed the watershed
                // age states, so the age column below now reports the age of
                // the water being shed rather than the placeholder 0 this
                // comment used to promise.
                snap.subcatch_quality.assign(nS_s * nr_s, 0.0);
                for (std::size_t s = 0; s < nS_s; ++s) {
                    // H5a temperature — written BEFORE the runoff gate, and
                    // unmasked, exactly as the node and link loops above do.
                    //
                    // Until 2026-08-22 there was no writer here at all: the
                    // age row below had one and temperature did not, so
                    // subcatch_quality kept the assign(…, 0.0) and the output
                    // plugin faithfully wrote the zero. Measured on
                    // tests/parity/transport/heat_parity.inp — nodes and links
                    // carried −4.147…17.66 °C while every subcatchment read
                    // exactly 0.0 for the whole run. The column existed in the
                    // header the whole time, which is why a column-presence
                    // check passed over it.
                    //
                    // It is OUTSIDE the has_runoff gate on purpose, and that
                    // is a deliberate divergence from the age column beside
                    // it. Age is gated because legacy's washoff convention
                    // says a subcatchment producing nothing reports nothing.
                    // Temperature cannot use that convention: 0 °C is a real
                    // temperature and cannot double as "no water" — the same
                    // reasoning H1 applied at nodes and links. D-H5c exists
                    // precisely so the dry-element value is the deck's choice
                    // (HOLD | AIR | DEFAULT) rather than an accident, so the
                    // state carries a meaningful number when dry and blanking
                    // it here would throw that away.
                    if (temp_col && s < ctx_.heat_state
                                            .subcatch_runoff_temp.size())
                        snap.subcatch_quality[s * nr_s + temp_i] =
                            ctx_.heat_state.subcatch_runoff_temp[s];

                    const bool has_runoff =
                        (s < ctx_.subcatches.runoff.size() &&
                         ctx_.subcatches.runoff[s] != 0.0);
                    if (!has_runoff) continue;
                    for (std::size_t p = 0; p < np_s; ++p) {
                        const std::size_t src = s * np_s + p;
                        if (src < ctx_.subcatches.conc.size() &&
                            src < ctx_.subcatches.conc_old.size())
                            snap.subcatch_quality[s * nr_s + p] =
                                f1_rt * ctx_.subcatches.conc_old[src] +
                                f_rt * ctx_.subcatches.conc[src];
                    }
                    // A3 retires the placeholder that reported 0 here: the
                    // subcatchment now publishes the age of the water it is
                    // shedding, in HOURS like every other age column. Gated
                    // on has_runoff above, which is legacy's own washoff
                    // convention — a subcatchment producing nothing reports
                    // nothing rather than the age of a puddle it is keeping.
                    if (age_col && s < ctx_.water_age_state
                                           .subcatch_runoff_age.size())
                        snap.subcatch_quality[s * nr_s + np_s] =
                            ctx_.water_age_state.subcatch_runoff_age[s] /
                            kSecPerHour;
                }
            }

            // Attach name table pointers (valid for lifetime of ctx_)
            snap.node_ids     = &ctx_.node_names.names();
            snap.link_ids     = &ctx_.link_names.names();
            snap.subcatch_ids = &ctx_.subcatch_names.names();
            snap.gage_ids     = &ctx_.gage_names.names();
            snap.pollut_names = &ctx_.reported_species_names;

            io_thread_.post(std::move(snap));
        }
#ifdef OPENSWMM_HAS_2D
        else if (surface_output_plugin_ != nullptr
                 && surface_router_.isActive() && !plugins_.empty()) {
            // 2D output is configured but the 1D binary .out is not being saved
            // (no .out path → no 1D output writer registered). Post a snapshot
            // carrying only the surface state so the 2D HDF5 still writes. The
            // 1D writers are absent in this mode, so the empty 1D arrays are
            // never consumed; DefaultReportPlugin::update() is a no-op.
            SimulationSnapshot snap;
            snap.sim_time = report_date;
            surface_router_.refreshRenderFieldsIfStale();
            fillSurfaceSnapshot(snap);
            io_thread_.post(std::move(snap));
        }
#endif
        hydraulics::TimestepController::reset_output_timer(ctx_);
    }
}


void SWMMEngine::fillSurfaceSnapshot(SimulationSnapshot& snap) const noexcept {
#ifdef OPENSWMM_HAS_2D
    if (!surface_router_.isActive()) return;
    const auto& st  = surface_router_.state();
    snap.surface_tri_count     = surface_router_.mesh().n_triangles();
    snap.surface_vert_count    = surface_router_.mesh().n_vertices();
    snap.surface_depth          = st.depth;
    snap.surface_head           = st.head;
    snap.surface_grad_hx        = st.grad_hx;
    snap.surface_grad_hy        = st.grad_hy;
    snap.surface_grad_hx_lim    = st.grad_hx_lim;
    snap.surface_grad_hy_lim    = st.grad_hy_lim;
    snap.surface_rainfall       = st.rainfall;
    snap.surface_coupling_flux  = st.coupling_flux;
    snap.surface_net_source     = st.net_source;
    // Per-cell infiltration (plan §5.5.6). The held rate lives on the surface
    // state; the cumulative depth comes from the ROUTER's applied-loss
    // accumulator (SurfaceRouter2D::infilCumulative) rather than
    // Infil2D::cumulative(), so sum(infil_cum * area) equals the ledger's
    // infil_out exactly. It is empty when no [2D_INFILTRATION*] model
    // resolved — publish zeros then, so both sidecar variables always carry
    // one full [nFace] row per time step.
    snap.surface_infil_rate     = st.infil_rate;
    // Overland transport S1: species concentration per cell, derived from the
    // mass store behind the hydraulics' own dry threshold. Species-major, one
    // block per species, so the plugin can write a [time, species, face]
    // dataset with a single extend.
    {
        const auto& tr = st.transport;
        snap.surface_species_count = tr.n_species;
        snap.surface_species_names = tr.active() ? &tr.row_names : nullptr;
        if (tr.active()) {
            const auto& mesh = surface_router_.mesh();
            const double dry_depth = surface_router_.options().dry_depth;
            const auto nt = static_cast<std::size_t>(tr.n_cells);
            snap.surface_species_conc.assign(
                static_cast<std::size_t>(tr.n_species) * nt, 0.0);
            for (int sp = 0; sp < tr.n_species; ++sp)
                for (int c = 0; c < tr.n_cells; ++c) {
                    const double v_dry = dry_depth * mesh.tri_area[c];
                    snap.surface_species_conc[static_cast<std::size_t>(sp) * nt +
                                              static_cast<std::size_t>(c)] =
                        tr.concentration(sp, c, st.volume[c], v_dry);
                }
        } else {
            snap.surface_species_conc.clear();
        }
    }
    const auto& infil_cum = surface_router_.infilCumulative();
    if (infil_cum.size() == st.infil_rate.size())
        snap.surface_infil_cum = infil_cum;
    else
        snap.surface_infil_cum.assign(st.infil_rate.size(), 0.0);
    snap.surface_rain_cum       = surface_router_.rainCumulative();
    // Output sign convention: the integrator stores edge_flux and the face
    // velocity INFLOW-positive (a positive edge_flux raises the cell — see
    // SurfaceFluxCalculator), whereas the documented public/HDF5 convention
    // (openswmm_2d.h) is OUTWARD-positive: positive flux leaves the cell and the
    // reported velocity is the physical down-gradient flow. Flip the sign here,
    // at the output boundary only; the internal state is untouched so the volume
    // update and mass balance are unaffected.
    snap.surface_edge_flux      = st.edge_flux;
    for (double& f : snap.surface_edge_flux) f = -f;
    snap.surface_vert_head      = st.vert_head;
    snap.surface_vert_depth     = st.vert_depth_signed;
    snap.surface_face_vx        = st.face_vx;
    snap.surface_face_vy        = st.face_vy;
    for (double& v : snap.surface_face_vx) v = -v;
    for (double& v : snap.surface_face_vy) v = -v;
    snap.surface_continuity_err = st.cell_continuity_err;
    // Cumulative rendering envelopes (SI-native; not display-converted).
    snap.surface_stat_max_depth    = st.stat_max_depth;
    snap.surface_stat_max_velocity = st.stat_max_velocity;
    snap.surface_stat_max_cont_err = st.stat_max_cont_err;
#else
    (void)snap;
#endif
}

// ============================================================================
// accumulateAvgResults() — accumulate node/link state for time-step averaging
// ============================================================================

void SWMMEngine::accumulateAvgResults() noexcept {
    // Legacy output_updateAvgResults (output.c:853-901): per routing step it
    // evaluates node_getResults/link_getResults at f = 1.0 — the REAL4
    // DISPLAY-unit report values — and adds them into REAL4 xAvg slots. The
    // whole chain is float32 arithmetic on display values; reproduce it
    // exactly (see AvgAccumulator). Quality columns are NOT averaged yet —
    // legacy also averages NODE_QUAL/LINK_QUAL; documented follow-up.
    const auto du = ucf::DisplayUnits::from(ctx_.options);

    // Node slots — legacy node_getResults at f = 1 (node.c:481-505):
    // head = f32(depth_display) + f32(invert_display), summed in float.
    for (int j = 0; j < ctx_.n_nodes(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        const float dd =
            static_cast<float>(ctx_.nodes.depth[uj] * du.length);
        avg_.node_depth[uj] += dd;
        avg_.node_head[uj] += dd +
            static_cast<float>(ctx_.nodes.invert_elev[uj] * du.length);
        avg_.node_volume[uj] +=
            static_cast<float>(reportedNodeVolume(j) * du.volume);
        avg_.node_lat_inflow[uj] +=
            static_cast<float>(ctx_.nodes.lat_flow[uj] * du.flow);
        avg_.node_total_inflow[uj] +=
            static_cast<float>(ctx_.nodes.inflow[uj] * du.flow);
        avg_.node_overflow[uj] +=
            static_cast<float>(ctx_.nodes.overflow[uj] * du.flow);
    }

    // Link slots — legacy link_getResults at f = 1 (link.c:677-727):
    // flow/velocity carry direction; velocity converts with UCF(LENGTH).
    ensureXspCache();
    for (int j = 0; j < ctx_.n_links(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        double q = ctx_.links.flow[uj];
        double d = ctx_.links.depth[uj];
        const double dir = static_cast<double>(ctx_.links.direction[uj]);

        avg_.link_flow[uj]   += static_cast<float>(q * du.flow * dir);
        avg_.link_depth[uj]  += static_cast<float>(d * du.length);
        avg_.link_volume[uj] +=
            static_cast<float>(ctx_.links.volume[uj] * du.volume);

        auto lt = ctx_.links.type[uj];
        if (lt == LinkType::CONDUIT) {
            const XSectParams& xs = xsp_cache_[uj];
            const int cr = ctx_.link_subtypes.conduit_row(j);
            const int nb = (cr >= 0) ? ctx_.link_subtypes.conduits.barrels[static_cast<std::size_t>(cr)] : 1;
            avg_.link_velocity[uj] += static_cast<float>(
                link::getVelocity(xs, q, d, nb) * du.length * dir);
            avg_.link_capacity[uj] +=
                static_cast<float>(link::getCapacity(xs, d));
        } else {
            // Non-conduit capacity (pump speed, regulator opening):
            // Legacy preserves last value — multiply by (n_steps+1) so that
            // division by n_steps in applyAvgResults yields the last value
            // (output.c:889-895; float times int, in float).
            avg_.link_capacity[uj] = static_cast<float>(ctx_.links.setting[uj])
                * static_cast<float>(avg_.n_steps + 1);
        }
    }

    ++avg_.n_steps;
}

// ============================================================================
// applyAvgResults() — write averaged node/link values into a snapshot
// ============================================================================

void SWMMEngine::applyAvgResults(SimulationSnapshot& snap) noexcept {
    if (avg_.n_steps <= 0) return;
    // Legacy output_saveAvgResults (output.c:907-951): REAL4 slot divided by
    // Nsteps in float arithmetic. The results are DISPLAY-unit values, so
    // this must run AFTER convertSnapshotToDisplay — the doubles written
    // here hold exact float32 values that the writer's final float cast
    // reproduces bit-for-bit.
    const float n = static_cast<float>(avg_.n_steps);

    // Average node results (direction/units already in the accumulators)
    for (int j = 0; j < ctx_.n_nodes(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        snap.nodes.depth[uj]          = avg_.node_depth[uj] / n;
        snap.nodes.head[uj]           = avg_.node_head[uj] / n;
        snap.nodes.volume[uj]         = avg_.node_volume[uj] / n;
        snap.nodes.lateral_inflow[uj] = avg_.node_lat_inflow[uj] / n;
        snap.nodes.total_inflow[uj]   = avg_.node_total_inflow[uj] / n;
        snap.nodes.overflow[uj]       = avg_.node_overflow[uj] / n;
    }

    // Average link results
    for (int j = 0; j < ctx_.n_links(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        snap.links.flow[uj]     = avg_.link_flow[uj] / n;
        snap.links.depth[uj]    = avg_.link_depth[uj] / n;
        snap.links.velocity[uj] = avg_.link_velocity[uj] / n;
        snap.links.volume[uj]   = avg_.link_volume[uj] / n;
        snap.links.capacity[uj] = avg_.link_capacity[uj] / n;
    }

    // Reset for next report period
    avg_.reset();
}

int SWMMEngine::end() noexcept {
    if (ctx_.state != EngineState::RUNNING &&
        ctx_.state != EngineState::ENDED) {
        // A failed run sits in ERROR_STATE holding the root-cause message.
        // Callers (GUI, CLI, swmm_engine_run) call end() unconditionally on
        // their teardown path, so a wrong-state set_error here would clobber
        // the reason swmm_get_last_error_msg reports. Preserve it.
        if (ctx_.state == EngineState::ERROR_STATE)
            return (ctx_.error_code != SWMM_OK) ? ctx_.error_code
                                                : SWMM_ERR_WRONG_STATE;
        set_error(SWMM_ERR_WRONG_STATE,
                  "swmm_engine_end: engine must be running or ended");
        return SWMM_ERR_WRONG_STATE;
    }

    // Compute final GW storage for mass balance reporting
    {
        auto& gw = groundwater_.state();
        ctx_.mass_balance.gw_final_storage = 0.0;
        for (int i = 0; i < ctx_.n_subcatches(); ++i) {
            auto ui = static_cast<std::size_t>(i);
            if (gw.total_depth[ui] <= 0.0) continue;
            double upper_d = gw.total_depth[ui] - gw.lower_depth[ui];
            double vol = gw.theta[ui] * upper_d + gw.porosity[ui] * gw.lower_depth[ui];
            double area = ctx_.subcatches.area[ui] * 43560.0;
            ctx_.mass_balance.gw_final_storage += vol * area;
        }
    }

    // D-NS1 (X6): summarize extraction clamps before the report renders,
    // so the counts reach the .rpt warning block (gates read
    // ctx.warnings directly, so .rpt ordering is not load-bearing).
    quality::summarizeNegativeSourceClamps(ctx_);

    // Build routing time step histogram for report
    ctx_.routing_stats.build_histogram();

    // Publish the FV solver's cumulative counters into the context so the
    // report plugin can print them — IReportPlugin only ever sees a
    // SimulationContext, never the Router. Exactly the 2D pattern
    // (SurfaceRouter2D::finalize -> mass_balance_2d.solver_*), except the FV
    // solver's counters survive finalize(), so ordering here is free.
    // The nsteps > 0 test is load-bearing: INetworkSolver::run_stats() defaults
    // to a ZEROED struct, so a backend carrying no counters returns nsteps == 0
    // — not the -1 the report block treats as "skip me". Leaving the sentinel
    // in place for that case is what keeps the block from printing a row of
    // zeros, which reads as "the solver did nothing" instead of "nobody
    // counted".
    if (const auto* fv = router_.fvSolver();
        fv != nullptr && fv->run_stats().nsteps > 0) {
        const auto s = fv->run_stats();
        auto& rs = ctx_.routing_stats;
        rs.fv_nsteps      = s.nsteps;
        rs.fv_nflux       = s.nflux;
        rs.fv_avg_h       = s.avg_h;
        rs.fv_last_h      = s.last_h;
        rs.fv_min_h       = s.min_h;
        rs.fv_active_min  = s.active_frac_min;
        rs.fv_active_mean = s.active_frac_mean;
        rs.fv_active_max  = s.active_frac_max;
        rs.fv_n_tiers     = s.n_tiers;
        for (int k = 0; k < s.n_tiers && k < 8; ++k)
            rs.fv_tier_cells[k] = s.tier_cells[k];
        rs.fv_dt_argmin_pressurized = s.dt_argmin_pressurized;
        rs.fv_dt_argmin_band        = s.dt_argmin_band;
        rs.fv_dt_argmin_free        = s.dt_argmin_free;
        rs.fv_dt_argmin_node        = s.dt_argmin_node;
    }

    // Finalize per-element max stats for report (top-5 CFL-critical, flow turns,
    // non-convergence — matching legacy stats_findMaxStats)
#ifdef OPENSWMM_HAS_2D
    // Finalize 2D surface routing module BEFORE the max-stats finalize: the
    // partial-window flush advances the 2D state and books its exchanges, so
    // the statistics/reports must read the post-flush state, not a snapshot
    // taken one window early.
    surface_router_.finalize(ctx_);
#endif

    // Benchmark aid: attribute wall time between the 2D solve, its window
    // overhead (rainfall/coupling assembly), and the 1D routing step. Gated
    // by OPENSWMM_PERF so normal runs are unaffected. (See core/PerfTimers.hpp.)
    if (std::getenv("OPENSWMM_PERF") != nullptr) {
        std::fprintf(stderr,
            "[PERF] 2D-window=%.2fs (2D-advance=%.2fs, 2D-overhead=%.2fs)  "
            "1D-step=%.2fs\n",
            openswmm::perf::sec_2d_window,
            openswmm::perf::sec_2d_advance,
            openswmm::perf::sec_2d_window - openswmm::perf::sec_2d_advance,
            openswmm::perf::sec_1d_step);
    }

    ctx_.finalize_max_stats();

    // SAVE HOTSTART (end-of-run entries): write the final routing state to each
    // SAVE HOTSTART file with no datetime suffix, matching legacy swmm_end →
    // hotstart_save (swmm5.c). Datetime-suffixed intermediate saves are a
    // follow-up (not exercised by the QA suite). Legacy .hsf format so the file
    // round-trips through USE HOTSTART (apply_legacy_routing).
    for (const auto& entry : ctx_.files.hotstart_saves) {
        if (entry.datetime != 0.0) continue;   // intermediate save — not yet
        const std::string& sp = !entry.path.absolute.empty()
            ? entry.path.absolute : entry.path.original;
        if (sp.empty()) continue;
        const int rc = HotStartManager::save_legacy_routing(sp, ctx_);
        if (rc != 0) {
            ctx_.warnings.push_back(
                "SAVE HOTSTART: " + HotStartManager::last_io_error());
        }
    }

    // Phase 5: drain and join the IO thread (all writes must complete first)
    io_thread_.stop();

    // Phase 4: finalize plugins (flush/close output files)
    if (!plugins_.empty()) {
        plugins_.finalize_all(ctx_);
    }

    ctx_.state = EngineState::ENDED;
    return SWMM_OK;
}

// ============================================================================
// report()
// ============================================================================

int SWMMEngine::report() noexcept {
    if (ctx_.state != EngineState::ENDED) {
        // Same preservation as end(): never clobber a failed run's recorded
        // cause with a wrong-state complaint on the unconditional teardown path.
        if (ctx_.state == EngineState::ERROR_STATE)
            return (ctx_.error_code != SWMM_OK) ? ctx_.error_code
                                                : SWMM_ERR_WRONG_STATE;
        set_error(SWMM_ERR_WRONG_STATE,
                  "swmm_engine_report: must call end() first");
        return SWMM_ERR_WRONG_STATE;
    }

    // Copy LID water balance results from solver groups into lid_usage for reporting
    {
        int n_usage = ctx_.lid_usage.count();
        const int n_groups = lid_.numGroups();
        for (int t = 0; t < n_groups; ++t) {
            const auto& g = lid_.group(t);
            for (int s = 0; s < g.count; ++s) {
                auto us = static_cast<std::size_t>(s);
                int ctrl_idx = (us < g.control_idx.size()) ? g.control_idx[us] : -1;
                int sc_idx   = (us < g.subcatch_idx.size()) ? g.subcatch_idx[us] : -1;
                if (ctrl_idx < 0 || sc_idx < 0) continue;
                // Find matching lid_usage entry
                for (int j = 0; j < n_usage; ++j) {
                    auto uj = static_cast<std::size_t>(j);
                    if (ctx_.lid_usage.lid_index[uj] == ctrl_idx &&
                        ctx_.lid_usage.subcatch_index[uj] == sc_idx) {
                        ctx_.lid_usage.wb_inflow[uj]     = (us < g.wb_inflow.size())     ? g.wb_inflow[us]     : 0.0;
                        ctx_.lid_usage.wb_evap[uj]       = (us < g.wb_evap.size())       ? g.wb_evap[us]       : 0.0;
                        ctx_.lid_usage.wb_infil[uj]      = (us < g.wb_infil.size())      ? g.wb_infil[us]      : 0.0;
                        ctx_.lid_usage.wb_surf_flow[uj]  = (us < g.wb_surf_flow.size())  ? g.wb_surf_flow[us]  : 0.0;
                        ctx_.lid_usage.wb_drain_flow[uj] = (us < g.wb_drain_flow.size()) ? g.wb_drain_flow[us] : 0.0;
                        ctx_.lid_usage.wb_init_vol[uj]   = (us < g.wb_init_vol.size())   ? g.wb_init_vol[us]   : 0.0;
                        ctx_.lid_usage.wb_final_vol[uj]  = (us < g.wb_final_vol.size())  ? g.wb_final_vol[us]  : 0.0;
                        break;
                    }
                }
            }
        }
    }

    // Gap #68: copy inlet stats into inlet_usages for reporting
    if (ctx_.inlet_usages.count() > 0)
        inlet_.gatherStats(ctx_.inlet_usages);

    // Phase 4: write summary reports via all report plugins
    if (!plugins_.empty()) {
        plugins_.write_summary_all(ctx_);
    }

    ctx_.state = EngineState::REPORTED;
    return SWMM_OK;
}

// ============================================================================
// close()
// ============================================================================

int SWMMEngine::close() noexcept {
    // Load-phase breakdown. Emitted here rather than from end() so it reports
    // for a bare open()+close() too — the benchmark harness times open alone,
    // open+initialize, and the full sequence. (See core/PerfTimers.hpp.)
    if (perf::enabled()) perf::dump_load();

    // FV phase breakdown, only when the FV solver actually ran — an empty line
    // on a DYNWAVE run would be noise the harness has to filter.
    if (perf::enabled() && perf::n_fv_substep > 0) perf::dump_fv();

    // Stop IO thread if still running (safe to call even if already stopped)
    io_thread_.stop();

    // A run that failed after open() never reaches end()/report(), so without
    // this flush the root cause would never reach the .rpt — the report
    // plugin's destructor only stamps the generic "[Report interrupted]"
    // footer. Record the fatal message alongside any accumulated errors and
    // write them out, mirroring write_open_failure_report() for open()-time
    // failures. Many set_error sites (hotstart, interface files) do not push
    // into ctx_.errors themselves, hence the append here.
    if (ctx_.state == EngineState::ERROR_STATE) {
        try {
            if (!ctx_.error_message.empty() &&
                std::find(ctx_.errors.begin(), ctx_.errors.end(),
                          ctx_.error_message) == ctx_.errors.end())
                ctx_.errors.push_back(ctx_.error_message);
        } catch (...) {
            // Out-of-memory recording the error is non-fatal for close().
        }
        if (plugins_prepare_attempted_)
            plugins_.finalize_all(ctx_);   // flushes into the open .rpt/.out
        else
            write_open_failure_report();   // .rpt was never opened — write one
    }
    plugins_prepare_attempted_ = false;

    // Close routing interface files
    iface_.closeFiles();

    // Close the RDII interface file (no-op if never opened)
    rdii_iface_file_.close();

    // Phase 1b: close the runoff interface file (no-op if never opened).
    // Done before plugins_.unload_all so that any plugin holding the
    // runoff file open via swmm_runoff_iface_* sees a clean shutdown.
    closeRunoffIface();

    // Unload all dynamically loaded plugin libraries
    plugins_.unload_all();

    ctx_.state = EngineState::CLOSED;
    return SWMM_OK;
}

// ============================================================================
// subcatchSnowDepth — area-weighted snow pack SWE on a subcatchment (ft)
// ============================================================================

double SWMMEngine::subcatchSnowDepth(int idx) const noexcept {
    auto us = static_cast<std::size_t>(idx);
    if (ctx_.subcatches.snowpack[us] < 0) return 0.0;
    const auto& soa = snow_.state();
    double fi = ctx_.subcatches.frac_imperv[us];
    double sn = (us < soa.snn.size()) ? soa.snn[us] : 0.0;
    double fArea[3] = { sn * fi, (1.0 - sn) * fi, 1.0 - fi };
    int base = idx * snow::N_SUBAREAS;
    double sd = 0.0;
    for (int k = 0; k < snow::N_SUBAREAS; ++k) {
        auto uk = static_cast<std::size_t>(base + k);
        if (uk < soa.wsnow.size())
            sd += soa.wsnow[uk] * fArea[k];
    }
    return sd;
}

// ============================================================================
// applyForcings — inject user-specified runtime forcing values
// ============================================================================

void SWMMEngine::applyForcings(double dt) noexcept {
    auto& f = ctx_.forcing;

    // ---- Node lateral inflow forcing ----
    // Applied as a per-step OVERLAY, not written into user_lat_flow (which
    // holds the persistent swmm_node_set_lateral_inflow value): while the
    // forcing mode is active, effectiveUserLatFlow() folds the ForcingData
    // value into lateral-inflow assembly and mass-balance tracking each
    // step; when clear_reset_entries() clears the mode the forcing simply
    // stops. The previous += mutation compounded PERSIST+ADD forcings each
    // step and left expired RESET forcings behind permanently. Issue #113.

    // ---- Node head boundary forcing (outfalls only) ----
    //
    // C6: the prescribed-HGL value stays in forcing.node_head_boundary_value
    // and is consumed by Outfall::setAllOutfallDepths as an overlay on top of
    // the legacy outfall logic and the C4 2D-coupling override. Do NOT
    // mutate outfall_type or outfall_param here — the original outfall_type
    // must survive a prescribed step so that "unfix" (mode = NONE) returns
    // the outfall to its original FREE / NORMAL / FIXED / TIDAL / TIMESERIES
    // behaviour without any state restoration. See
    // docs/1D_2D_COUPLING_GATE_REVIEW.md §6 (C6).
    //
    // This block is intentionally a no-op now; staging into the forcing
    // buffer is handled by swmm_forcing_node_head_boundary() in the API.

    // ---- Gage rainfall forcing (before runoff substeps read gages) ----
    for (int g = 0; g < ctx_.n_gages(); ++g) {
        auto ug = static_cast<std::size_t>(g);
        if (f.gage_rainfall_mode[ug] == ForcingMode::OVERRIDE) {
            ctx_.gages.rainfall[ug] = f.gage_rainfall_value[ug];
        } else if (f.gage_rainfall_mode[ug] == ForcingMode::ADD) {
            ctx_.gages.rainfall[ug] += f.gage_rainfall_value[ug];
        }
    }

    // ---- Subcatchment rainfall forcing ----
    // No action needed here: subcatch_rainfall_{mode,value} are consumed by
    // the runoff solver's rainfall assembly via forcing::effective_rainfall()
    // so the override survives the per-step gage re-read. (Previously this
    // block pre-wrote subcatches.rainfall, which the runoff solver then
    // overwrote from the gage — the forcing had no effect.)

    // ---- Subcatchment PET forcing ----
    // No action needed here: subcatch_evap_{mode,value} hold a prescribed
    // PET *rate* (ft/sec) that is consumed directly by the runoff, LID, and
    // groundwater solvers via forcing::effective_evap_rate(), so capping to
    // available water and mass-balance accounting happen along the normal
    // computation paths. (Previously this block overwrote evap_loss, which
    // the runoff solver then recomputed — the forcing had no effect.)

    // ---- Link setting forcing (pump/orifice/weir control override) ----
    for (int j = 0; j < ctx_.n_links(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (f.link_setting_mode[uj] == ForcingMode::OVERRIDE) {
            ctx_.links.setting[uj]        = f.link_setting_value[uj];
            ctx_.links.target_setting[uj] = f.link_setting_value[uj];
        } else if (f.link_setting_mode[uj] == ForcingMode::ADD) {
            ctx_.links.setting[uj]        += f.link_setting_value[uj];
            ctx_.links.target_setting[uj]  = ctx_.links.setting[uj];
        }
    }

    // ---- Link flow forcing ----
    for (int j = 0; j < ctx_.n_links(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (f.link_flow_mode[uj] == ForcingMode::OVERRIDE) {
            ctx_.links.flow[uj] = f.link_flow_value[uj];
        } else if (f.link_flow_mode[uj] == ForcingMode::ADD) {
            ctx_.links.flow[uj] += f.link_flow_value[uj];
        }
    }

    // ---- Node quality mass flux forcing (transient via ForcingData) ----
    // For OVERRIDE mode, the concentration is set directly.
    // For ADD mode, mass_rate (mass/sec) is added as concentration delta.
    int np = ctx_.n_pollutants();
    if (np > 0) {
        for (int i = 0; i < ctx_.n_nodes(); ++i) {
            for (int p = 0; p < np; ++p) {
                auto flat = static_cast<std::size_t>(i) * static_cast<std::size_t>(np)
                          + static_cast<std::size_t>(p);
                if (f.node_quality_mode[flat] == ForcingMode::OVERRIDE) {
                    ctx_.nodes.conc[flat] = f.node_quality_value[flat];
                } else if (f.node_quality_mode[flat] == ForcingMode::ADD) {
                    ctx_.nodes.conc[flat] += f.node_quality_value[flat];
                    ctx_.mass_balance.routing_forcing_qual_inflow[
                        static_cast<std::size_t>(p)] +=
                        f.node_quality_value[flat] * dt;
                }
            }
        }

        // Link quality forcing is applied after QualitySolver::updateLinkQuality()
        // so the prescribed concentration is not overwritten by upstream-node
        // mixing during the same routing step.

        // ---- Persistent user quality mass flux (user_conc_mass_flux) ----
        // NOT applied here. It used to be a post-quality concentration bump
        // (C += mass_rate*dt/volume), which the next routing step's mixing
        // overwrote, so the forced mass was booked in the ledger but never
        // actually entered the system. It is delivered in the loader stage
        // instead — QualitySolver::addExtInflowLoads() — exactly as legacy
        // addExternalInflows() does it (routing.c: Node[j].newQual[p] += w;
        // massbal_addInflowQual(EXTERNAL_INFLOW, p, w)). That is also the
        // only place that reaches BOTH the legacy CSTR mixing and the ARD
        // node stores. This mirrors the forced-lateral-inflow treatment,
        // which likewise counts as external inflow (issue #113).
    }
}

// ============================================================================
// Phase 1b: runoff interface file management
// ============================================================================
//
// Thin wrappers around runoff_iface::RunoffInterfaceFile. The auto-save
// hook lives in stepRunoff() right after accumulateRunoffMassBalance —
// see the call site in this file for the in-loop emit.

int SWMMEngine::openRunoffIfaceWrite(const std::string& path) noexcept {
    if (runoff_iface_file_ && runoff_iface_file_->isOpen()) {
        // Refuse to silently leak the previous file — caller must close
        // explicitly so it's obvious in tests / debug logs.
        return -10;
    }
    runoff_iface_file_ = std::make_unique<runoff_iface::RunoffInterfaceFile>();
    const int rc = runoff_iface_file_->openForWrite(
        path,
        ctx_.n_subcatches(),
        ctx_.n_pollutants(),
        static_cast<int>(ctx_.options.flow_units));
    if (rc != 0) runoff_iface_file_.reset();
    return rc;
}

int SWMMEngine::openRunoffIfaceRead(const std::string& path) noexcept {
    if (runoff_iface_file_ && runoff_iface_file_->isOpen()) return -10;
    runoff_iface_file_ = std::make_unique<runoff_iface::RunoffInterfaceFile>();
    const int rc = runoff_iface_file_->openForRead(
        path,
        ctx_.n_subcatches(),
        ctx_.n_pollutants(),
        static_cast<int>(ctx_.options.flow_units));
    if (rc != 0) runoff_iface_file_.reset();
    return rc;
}

void SWMMEngine::saveRunoffIfaceStep(double dt) noexcept {
    if (!runoff_iface_file_) return;
    // saveResults is a no-op if the file is not in SAVE mode.
    runoff_iface_file_->saveResults(ctx_, dt);
}

bool SWMMEngine::readRunoffIfaceStep() noexcept {
    if (!runoff_iface_file_) return false;
    return runoff_iface_file_->readResults(ctx_);
}

void SWMMEngine::closeRunoffIface() noexcept {
    if (!runoff_iface_file_) return;
    runoff_iface_file_->close();
    runoff_iface_file_.reset();
}

FileMode SWMMEngine::runoffIfaceMode() const noexcept {
    if (!runoff_iface_file_ || !runoff_iface_file_->isOpen())
        return FileMode::NONE;
    // RunoffInterfaceFile doesn't expose its mode directly; infer from
    // the FilesSpec which is set by the C API entry points before
    // calling open*. Falling back to SAVE keeps the diagnostic non-NONE.
    return ctx_.files.runoff_mode;
}

// ============================================================================
// Callback registration
// ============================================================================

void SWMMEngine::set_progress_callback(SWMM_ProgressCallback cb, void* ud) noexcept {
    callbacks_.on_progress  = cb;
    callbacks_.progress_ud  = ud;
}

void SWMMEngine::set_warning_callback(SWMM_WarningCallback cb, void* ud) noexcept {
    callbacks_.on_warning  = cb;
    callbacks_.warning_ud  = ud;
}

void SWMMEngine::set_step_begin_callback(SWMM_StepBeginCallback cb, void* ud) noexcept {
    callbacks_.on_step_begin   = cb;
    callbacks_.step_begin_ud   = ud;
}

void SWMMEngine::set_step_end_callback(SWMM_StepEndCallback cb, void* ud) noexcept {
    callbacks_.on_step_end  = cb;
    callbacks_.step_end_ud  = ud;
}

// ============================================================================
// Private helpers
// ============================================================================

void SWMMEngine::validate_project() noexcept {
    auto& opt = ctx_.options;

    // WARNING 01: wet-weather routing step reduced to a rain gage's recording
    // interval (legacy gage_validate / gage.c). Progressively clamp WetStep to
    // the smallest gage interval it exceeds, warning per gage.
    const int ng = ctx_.n_gages();
    for (int g = 0; g < ng; ++g) {
        const double interval =
            static_cast<double>(ctx_.gages.interval_sec[static_cast<std::size_t>(g)]);
        if (interval > 0.0 && opt.wet_step > interval) {
            opt.wet_step = interval;
            ctx_.warnings.push_back(
                format_warning(WARN_WET_STEP_REDUCED, ctx_.gage_names.name_of(g)));
        }
    }

    // WARNING 06: dry-weather step increased to the wet-weather step (legacy
    // project.c). Empty object id, matching legacy.
    if (opt.dry_step < opt.wet_step) {
        opt.dry_step = opt.wet_step;
        ctx_.warnings.push_back(format_warning(WARN_DRY_STEP_INCREASED, ""));
    }

    // WARNING 07: routing step reduced to the wet-weather step (legacy project.c).
    if (opt.routing_step > opt.wet_step) {
        opt.routing_step = opt.wet_step;
        ctx_.warnings.push_back(format_warning(WARN_ROUTING_STEP_REDUCED, ""));
    }
}

int SWMMEngine::checkRoutingDiverged() noexcept {
    // A diverged routing solve used to run to completion and write NaN into
    // the report, which reads as a finished simulation (TwinOaks-v2 under
    // DYNWAVE + SURCHARGE_METHOD SLOT: a slot-surcharged pass-through node
    // beside a dry one drove 2.8e5 cfs through a 9 ft pipe, cascaded, and
    // reached 1e290 before the report accumulators overflowed). Stop at the
    // step that goes non-physical and name the element, so the failure is
    // reportable instead of silent.
    //
    // The bound is deliberately absurd, not tight: real heads are O(1e3) ft
    // and real flows O(1e5) cfs at the very largest, so 1e9 cannot fire on a
    // converging model however badly conditioned. Catching a merely LARGE
    // value is the caller's job (continuity error, non-convergence percent);
    // this exists only to stop unbounded growth. Note an isfinite() test
    // alone is not enough -- the runaway stays finite for a long way up.
    constexpr double kAbsurd = 1.0e9;
    auto diverged = [](double v) {
        return !std::isfinite(v) || std::fabs(v) > kAbsurd;
    };

    // FV cell-state guard (issue #156 R3). The published node/link state
    // checked below is a per-conduit REDUCTION of the FV cells, and the
    // reduction stays far under kAbsurd long after the cells have left the
    // physics (the mixed-flow study P6 measured 30 of 118 series beyond 10x
    // the observed range, every run status OK — the substep retry loop cannot
    // reject a dt-independent amplification, so it accepts the diverged step
    // silently). The solver checks its own cells against physical-absurdity
    // bounds once per routing step and reports the worst offender here.
    if (const fv::INetworkSolver* s = router_.fvSolver()) {
        fv::INetworkSolver::Divergence dv;
        if (s->divergence(dv)) {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                "ERROR %d: the FV solution diverged in conduit '%s' "
                "(%s %.6g) after %.4f hours -- a step-size-independent "
                "instability the substep retry cannot shrink away; the run "
                "cannot continue.",
                CFFI_ERR_NUMERICAL,
                dv.link >= 0 ? ctx_.link_names.name_of(dv.link).c_str() : "?",
                dv.what ? dv.what : "state", dv.value,
                ctx_.current_time / 3600.0);
            ctx_.errors.push_back(buf);
            set_error(CFFI_ERR_NUMERICAL, buf);
            return CFFI_ERR_NUMERICAL;
        }
    }

    const int nn = ctx_.n_nodes();
    for (int i = 0; i < nn; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        if (!diverged(ctx_.nodes.head[ui])) continue;
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "ERROR %d: the routing solution diverged at node '%s' "
            "(head %.6g) after %.4f hours -- the run cannot continue.",
            CFFI_ERR_NUMERICAL, ctx_.node_names.name_of(i).c_str(),
            ctx_.nodes.head[ui], ctx_.current_time / 3600.0);
        ctx_.errors.push_back(buf);
        set_error(CFFI_ERR_NUMERICAL, buf);
        return CFFI_ERR_NUMERICAL;
    }

    const int nl = ctx_.n_links();
    for (int j = 0; j < nl; ++j) {
        const auto uj = static_cast<std::size_t>(j);
        if (!diverged(ctx_.links.flow[uj])) continue;
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "ERROR %d: the routing solution diverged at link '%s' "
            "(flow %.6g) after %.4f hours -- the run cannot continue.",
            CFFI_ERR_NUMERICAL, ctx_.link_names.name_of(j).c_str(),
            ctx_.links.flow[uj], ctx_.current_time / 3600.0);
        ctx_.errors.push_back(buf);
        set_error(CFFI_ERR_NUMERICAL, buf);
        return CFFI_ERR_NUMERICAL;
    }
    return SWMM_OK;
}

void SWMMEngine::write_open_failure_report() noexcept {
    if (rpt_path_.empty() || ctx_.options.rpt_disabled) return;
    try {
        DefaultReportPlugin rp(rpt_path_);
        rp.initialize({}, nullptr);
        rp.prepare(ctx_);    // opens the file + writes title/errors/warnings
        rp.finalize(ctx_);   // flush
    } catch (...) {
        // Never let report-writing throw out of the open() error path.
    }
}

void SWMMEngine::set_error(int code, const char* message) noexcept {
    ctx_.error_code    = code;
    ctx_.error_message = message ? message : "";
    ctx_.state         = EngineState::ERROR_STATE;
    emit_warning(code, message);
}

void SWMMEngine::emit_warning(int code, const char* message) noexcept {
    if (callbacks_.on_warning) {
        callbacks_.on_warning(
            static_cast<void*>(this),
            code,
            message,
            callbacks_.warning_ud
        );
    }
}

void SWMMEngine::push_report_warning(const std::string& message,
                                     int code) noexcept {
    try {
        ctx_.warnings.push_back(message);
    } catch (...) {
        // Out-of-memory while recording a warning is non-fatal; still notify.
    }
    emit_warning(code, message.c_str());
}

void SWMMEngine::emit_progress() noexcept {
    if (!callbacks_.on_progress) return;
    const double total = (ctx_.options.end_date - ctx_.options.start_date)
                         * hydraulics::TimestepController::SEC_PER_DAY;
    const double frac  = (total > 0.0) ? (ctx_.current_time / total) : 0.0;
    callbacks_.on_progress(
        static_cast<void*>(this),
        frac,
        ctx_.current_date,
        callbacks_.progress_ud
    );
}

// ============================================================================
// init_modules() — initialize all computational modules from SimulationContext
// ============================================================================

void SWMMEngine::init_modules() noexcept {
    { perf::ScopedTimer _pt(perf::sec_init_hydraulics); initHydraulics(); }
    { perf::ScopedTimer _pt(perf::sec_init_hydrology);  initHydrology();  }
    { perf::ScopedTimer _pt(perf::sec_init_quality);    initQuality();    }
    { perf::ScopedTimer _pt(perf::sec_init_geometry);   initGeometry();   }
    initMassBalance();

    // Allocate forcing arrays to match object counts
    ctx_.forcing.resize(ctx_.n_nodes(), ctx_.n_links(),
                        ctx_.n_subcatches(), ctx_.n_gages(),
                        ctx_.n_pollutants());
}

// ============================================================================
// initHydraulics() — initialize hydraulic routing: router, exfiltration,
//                     inlets, culverts, controls, inflows, RDII, interface
// ============================================================================

/**
 * @brief Initialize hydraulic routing: router, exfiltration, inlets, culverts.
 *
 * @details Sets up the hydraulic router (with routing model selection),
 *          configures OpenMP threading, computes conduit conveyance,
 *          initializes controls, inflows, RDII, exfiltration, inlets,
 *          and interface files.
 */
void SWMMEngine::initHydraulics() noexcept {
    // 1. Router: build XSectGroups from link cross-section data,
    //    compute Manning conveyance for all conduits (batch)
    RouteModel rm = RouteModel::DYNWAVE;
    if (ctx_.options.routing_model == RoutingModel::KINWAVE) rm = RouteModel::KINWAVE;
    else if (ctx_.options.routing_model == RoutingModel::STEADY) rm = RouteModel::STEADY;
    else if (ctx_.options.routing_model == RoutingModel::FV) rm = RouteModel::FV;
    router_.init(ctx_, rm);

    // Surface what the FV mesh builder found. Without this the diagnostics were
    // collected into Router::fv_errors_ and never read by anyone: initFv bails
    // leaving fv_solver_ == nullptr, stepFv then returns 0 for every step, and
    // the model RUNS TO COMPLETION WITH NO HYDRAULIC ROUTING AT ALL — clean
    // exit, empty report, no message. A DUMMY-shape conduit is enough to
    // trigger it, and DUMMY conduits are common in real models.
    for (const std::string& w : router_.fvWarnings())
        ctx_.warnings.push_back(w);
    for (const std::string& e : router_.fvErrors())
        ctx_.errors.push_back(e);

    // Relational node refactor — Phase 4 (authoritative): the storage/outfall/
    // divider side-tables are the single source of truth, populated by the
    // parse/edit writers. Re-derive the base→row reverse map here so it is
    // consistent and sized before compute (also captures the outfall→conduit
    // cache written by buildOutfallLinkMap during router_.init above). No
    // build-from-wide. See docs/relational/RELATIONAL_NODE_REFACTOR_PLAN.md.
    ctx_.node_subtypes.rebuild_index(ctx_.nodes.count());

    // Gap #83: Outfall connectivity + no-outlets validation
    // Gap #84: Conduit adverse slope (non-DW) + regulator from non-storage
    {
        int n_nl = ctx_.n_nodes();
        int n_ll = ctx_.n_links();
        std::vector<int> n_in(static_cast<std::size_t>(n_nl), 0);
        std::vector<int> n_out(static_cast<std::size_t>(n_nl), 0);
        for (int j = 0; j < n_ll; ++j) {
            auto uj = static_cast<std::size_t>(j);
            int n1 = ctx_.links.node1[uj], n2 = ctx_.links.node2[uj];
            if (n1 >= 0 && n1 < n_nl) n_out[static_cast<std::size_t>(n1)]++;
            if (n2 >= 0 && n2 < n_nl) n_in[static_cast<std::size_t>(n2)]++;
        }
        // Count outfall nodes (legacy does not error on multiple inlets or outlet links for outfalls)
        int n_outlets = 0;
        for (int i = 0; i < n_nl; ++i) {
            auto ui = static_cast<std::size_t>(i);
            if (ctx_.nodes.type[ui] == NodeType::OUTFALL)
                ++n_outlets;
        }
        // Gap #83b: drainage system must have at least one outlet.
        // Legacy raises ERR_NO_OUTLETS from validateGeneralLayout(), which
        // flowrout_init() calls ONLY for dynamic wave; kinematic wave and
        // steady routing go through validateTreeLayout(), which never
        // requires an outfall. FV joins DW here as the DW-class scheme.
        // A model with a 2D surface mesh is exempt: water can leave the
        // system through the 2D domain (boundary conditions / vertex-node
        // coupling), so a 1D outfall is not required. The mesh is parsed at
        // open, so triangle/vertex counts are valid here even though
        // surface_router_.initialize() (which sets isActive()) runs below.
        bool has_2d_domain = false;
#ifdef OPENSWMM_HAS_2D
        has_2d_domain = surface_router_.mesh().n_triangles() >= 1
                        && surface_router_.mesh().n_vertices() >= 3;
#endif
        if (n_outlets == 0
            && (rm == RouteModel::DYNWAVE || rm == RouteModel::FV)
            && !has_2d_domain) {
            ctx_.errors.push_back(format_error(ERR_NO_OUTLETS, ""));
            set_error(SWMM_ERR_PARSE, ctx_.errors.back().c_str());
        }
        // Gap #84a: adverse slope only errors for non-DW routing. FV joins DW
        // here — a conservative scheme resolves an adverse slope natively.
        // DUMMY cross-sections are exempt, exactly as legacy
        // validateTreeLayout (flowrout.c:255-257: `slope < 0 &&
        // xsect.type != DUMMY`) — a dummy conveys the node's inflow and has
        // no slope-driven physics (490-kw-h-h-elements CDT-39).
        if (rm != RouteModel::DYNWAVE && rm != RouteModel::FV) {
            for (int j = 0; j < n_ll; ++j) {
                auto uj = static_cast<std::size_t>(j);
                if (ctx_.links.type[uj] != LinkType::CONDUIT) continue;
                if (ctx_.links.xsect_shape[uj] == XsectShape::DUMMY) continue;
                const int cr = ctx_.link_subtypes.conduit_row(j);
                if (cr >= 0 && ctx_.link_subtypes.conduits.slope[static_cast<std::size_t>(cr)] < 0.0) {
                    ctx_.errors.push_back(format_error(ERR_SLOPE, ctx_.link_names.names()[uj]));
                    set_error(SWMM_ERR_PARSE, ctx_.errors.back().c_str());
                }
            }
        }
        // Gap #84b: legacy SWMM5 does not report ERROR 139 for regulators on
        // non-storage nodes in practice, so we suppress this check to match legacy output.
    }

    // Gap #44: detect routing loop — matching legacy ERR_LOOP in toposort.c
    if (router_.hasCycle()) {
        set_error(SWMM_ERR_PARSE,
                  "Routing loop detected: network contains a cycle that cannot "
                  "be topologically sorted (KW/STEADY routing only).");
    }

#ifdef OPENSWMM_HAS_2D
    // 1a. Initialize optional 2D surface routing module.
    //     Builds mesh topology, vertex stencils, resolves coupling maps,
    //     suppresses ponding at coupled nodes, and initializes the 2D solver.
    //     initialize() throws std::runtime_error on invalid 2D input (bad mesh,
    //     unknown coupled node, out-of-range edge). This function is noexcept, so
    //     an escaping exception would std::terminate the process. Catch it and
    //     route the message through the error path so it reaches the .rpt and the
    //     open fails gracefully, exactly like a 1D validation error.
    try {
        surface_router_.initialize(ctx_);
    } catch (const std::exception& e) {
        ctx_.errors.push_back(std::string("2D initialization failed: ") + e.what());
        set_error(SWMM_ERR_PARSE, ctx_.errors.back().c_str());
    } catch (...) {
        ctx_.errors.push_back("2D initialization failed: unknown error.");
        set_error(SWMM_ERR_PARSE, ctx_.errors.back().c_str());
    }
#endif

    // 1b. Configure OpenMP thread count from THREADS option.
    //     0 = auto (omp_get_max_threads() + heuristics); N = honoured exactly,
    //     with warnings when N exceeds the logical CPUs / the runtime limit
    //     (see ThreadInfo.hpp and THREAD_LIMITS_AND_OVERSUBSCRIPTION_PLAN).
    //     DWSolver applies its own per-thread conduit-count gate.
    //     The global OMP thread count is also set for Runoff/Quality modules.
    {
        // Resolve first: the wait-policy decision below depends on whether
        // the requested team oversubscribes the machine.
        // Thread warnings are collected locally and pushed through
        // push_report_warning() at the end of this block so they reach both
        // the .rpt warning list and the host's warning callback.
        std::vector<std::string> thread_warnings;
        const int nt = threadinfo::resolveRequested(
            ctx_.options.num_threads, "OpenMP team", &thread_warnings);
        const bool oversubscribed = threadinfo::isOversubscribed(nt);
        (void)oversubscribed;   // only consulted by the POSIX wait-policy block
#if defined(SWMM_USE_OPENMP) && !defined(_WIN32)
        // B2 threading wait policy: the persistent-team DW Picard loop
        // synchronizes with several barriers per iteration (~1.2M iterations
        // on large runs). With libomp's DEFAULT passive wait policy each
        // barrier costs 5-30µs (kernel futex sleep/wake — measured; this is
        // why legacy-style threading was net-negative). With active spinning
        // a barrier costs ~0.5-3µs. Request active waiting whenever DW
        // threading is possible (THREADS > 1 / 0 = auto, or the
        // SWMM_DW_THREADS A/B override). setenv(overwrite=0) respects an
        // explicit user override of either variable. This MUST run before
        // the first OpenMP runtime call in the process — libomp reads the
        // environment once, at lazy runtime init (typically the
        // omp_get_max_threads() below). kmp_set_blocktime() is also called
        // as a runtime-effective fallback in case a host application already
        // initialized OpenMP before engine start.
        {
            const char* dw_env = std::getenv("SWMM_DW_THREADS");
            const int dw_forced = dw_env ? std::atoi(dw_env) : -1;
            const bool dw_threading_possible =
                rm == RouteModel::DYNWAVE &&
                (ctx_.options.num_threads != 1 || dw_forced > 1) &&
                dw_forced != 1;
            // Never request active spinning for an oversubscribed team: a
            // spinner waiting on a DESCHEDULED spinner turns every barrier
            // into a scheduler-quantum stall (far worse than passive waits).
            if (dw_threading_possible && !oversubscribed) {
                // setenv() is not thread-safe in glibc: adding a name reallocs
                // the environ array and frees the old block, while ~30 getenv()
                // call sites on the routing hot path read it concurrently. Two
                // engines starting on different threads double-freed that block.
                // libomp reads the environment once at lazy runtime init, so
                // doing this on the first engine to get here is sufficient —
                // the values do not vary per engine. kmp_set_blocktime below
                // stays per-call: it is a runtime call, not an environment write.
                static std::once_flag omp_wait_policy_set;
                std::call_once(omp_wait_policy_set, [] {
                    setenv("OMP_WAIT_POLICY", "active", 0);
                    setenv("KMP_BLOCKTIME", "infinite", 0);
                });
#if defined(KMP_VERSION_MAJOR)
                // libomp extension: set spin-wait blocktime on the calling
                // (master) thread; workers forked by it inherit the setting.
                // Effective even when the runtime pre-dates the setenv above.
                kmp_set_blocktime(2147483647);
#endif
            }
        }
#endif
        omp_set_num_threads(nt);
#if defined(SWMM_USE_OPENMP)
        // OMP_THREAD_LIMIT (or a runtime that refuses oversubscription) caps
        // the team silently — measure what we actually got and say so.
        if (nt > 1) {
            int got = 1;
#pragma omp parallel num_threads(nt)
            {
#pragma omp master
                got = omp_get_num_threads();
            }
            if (got < nt) {
                char buf[256];
                std::snprintf(buf, sizeof buf,
                    "THREADS = %d requested but the OpenMP runtime provided a "
                    "team of %d (OMP_THREAD_LIMIT or runtime policy).", nt, got);
                thread_warnings.emplace_back(buf);
            }
        }
#endif

        // DWSolver gets its own thread count with per-thread conduit gate
        // (or the SWMM_DW_THREADS forced override).
        if (rm == RouteModel::DYNWAVE) {
            router_.setDWNumThreads(ctx_.options.num_threads, &thread_warnings);
        }

#ifdef OPENSWMM_HAS_2D
        for (const auto& w : surface_router_.threadWarnings())
            thread_warnings.push_back(w);
#endif
        for (const auto& w : thread_warnings)
            push_report_warning(w, 0);   // code 0: advisory, no SWMM_WarnCode applies
    }

    // Initialize routing time-step histogram bins (log-scale from RouteStep
    // down to MinRouteStep, matching legacy stats.c stats_open). For DYNWAVE
    // legacy runs stats_open after dynwave_validate, which clamps
    // MinRouteStep = min(MinRouteStep, RouteStep) then >= MINTIMESTEP.
    {
        double hist_min_step = ctx_.options.min_routing_step;
        if (rm == RouteModel::DYNWAVE || rm == RouteModel::FV) {
            hist_min_step = std::max(
                std::min(hist_min_step, ctx_.options.routing_step),
                constants::MIN_TIMESTEP);
        }
        ctx_.routing_stats.init_histogram(ctx_.options.routing_step,
                                          hist_min_step);
    }

    // NOTE: Conduit conveyance (beta, rough_factor, q_full) is computed in
    // PostParseResolver and then adjusted for conduit lengthening in
    // Routing::init(). Do not recompute here as it would overwrite the
    // lengthening adjustments.

    // 8. Controls: rules can be added via:
    //    a) Parsed from [CONTROLS] section text (needs rule parser)
    //    b) Built programmatically via C API (swmm_link_set_target_setting etc.)
    //    c) Registered via controls_.init(rules) before start()
    // The ControlEngine is ready to evaluate rules; rules vector may be empty
    // if all control is done via API runtime calls instead of rule text.

    // 9. Inflow solver: populate from ext_inflows + dwf_inflows data
    inflow_.init(ctx_);

    // 10a. RDII solver: initialize unit hydrograph groups
    rdii_.init(ctx_);
    // Gap #80: RDII UH validation
    {
        for (const auto& kv : rdii_.uhNameIndex()) {
            const std::string& uh_name = kv.first;
            int uh_i = kv.second;
            if (uh_i < 0 || uh_i >= static_cast<int>(rdii_.uh_params.size())) continue;
            const auto& uh = rdii_.uh_params[static_cast<std::size_t>(uh_i)];
            bool time_err = false, ratio_err = false;
            for (int m = 0; m < 12 && (!time_err || !ratio_err); ++m) {
                double rsum = 0.0;
                for (int k = 0; k < 3; ++k) {
                    // Legacy rdii.c:873: a zero base time means the UH does
                    // not exist for this month/term — skipped entirely, no
                    // rsum contribution. Legacy checks ONLY tPeak < 0; a
                    // recession constant K < 0 (tBase < tPeak) is accepted
                    // (e.g. simple-two-pump-model CHES_47_SEP Short K=-0.42).
                    if (uh.tBase[m][k] == 0.0) continue;
                    if (!time_err && uh.tPeak[m][k] < 0.0) {
                        ctx_.errors.push_back(format_error(ERR_UNITHYD_TIMES, uh_name));
                        set_error(SWMM_ERR_PARSE, ctx_.errors.back().c_str());
                        time_err = true;
                    }
                    if (uh.r[m][k] < 0.0) {
                        if (!ratio_err) {
                            ctx_.errors.push_back(format_error(ERR_UNITHYD_RATIOS, uh_name));
                            set_error(SWMM_ERR_PARSE, ctx_.errors.back().c_str());
                            ratio_err = true;
                        }
                    } else {
                        rsum += uh.r[m][k];
                    }
                }
                if (!ratio_err && rsum > 1.01) {
                    ctx_.errors.push_back(format_error(ERR_UNITHYD_RATIOS, uh_name));
                    set_error(SWMM_ERR_PARSE, ctx_.errors.back().c_str());
                    ratio_err = true;
                }
            }
        }
        // Validate RDII sewer areas
        const auto& assigns = ctx_.rdii_assigns;
        for (int i = 0; i < assigns.count(); ++i) {
            auto ui = static_cast<std::size_t>(i);
            if (assigns.sewer_area[ui] < 0.0) {
                int ni = assigns.node_idx[ui];
                const std::string& nname = (ni >= 0 && ni < ctx_.n_nodes())
                    ? ctx_.node_names.names()[static_cast<std::size_t>(ni)] : "?";
                ctx_.errors.push_back(format_error(ERR_RDII_AREA, nname));
                set_error(SWMM_ERR_PARSE, ctx_.errors.back().c_str());
            }
        }
    }

    // 10b. Non-conduit hydraulic structures (pumps, orifices, weirs, outlets).
    // Phase 6 Stage D: the relational link side-tables are now populated
    // authoritatively at parse/resolve (LinksHandler + PostParseResolver +
    // Router::init) and on GeoPackage load, so the temporary build() mirror is
    // gone — the side-tables stand on their own.
    hydstruct_.init(ctx_);

    // KINWAVE/STEADY evaluate orifice/weir/outlet/pump discharge inline inside
    // their sorted-link loop (legacy getLinkInflow), so the router needs the
    // structure solver itself — DW/FV get theirs via the non_conduit_fn
    // callback below. Must be wired before the first router_.step().
    router_.setStructureSolver(&hydstruct_);

    // 10c. Exfiltration solver: initialize Green-Ampt state for storage nodes
    exfil_.init(ctx_);

    // 10d. Inlet solver: initialize street inlet data
    inlet_.init(ctx_);

    // 10d-1. Pre-build culvert link index list (avoids per-timestep heap alloc)
    culvert_links_.clear();
    for (int j = 0; j < ctx_.n_links(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (ctx_.links.type[uj] == LinkType::CONDUIT) {
            const int cr = ctx_.link_subtypes.conduit_row(j);
            if (cr >= 0 && ctx_.link_subtypes.conduits.culvert_code[static_cast<std::size_t>(cr)] > 0)
                culvert_links_.push_back(j);
        }
    }

    // 10d. Interface file manager: initialize (files opened later in start())
    iface_.init(ctx_);

    // 10d-2. Sort routing events chronologically and resolve overlaps
    //        (matching legacy sortEvents() in routing.c)
    if (!ctx_.events.empty()) {
        std::sort(ctx_.events.begin(), ctx_.events.end(),
                  [](const SimulationContext::Event& a, const SimulationContext::Event& b) {
                      return a.start < b.start;
                  });
        // Resolve overlapping events
        for (size_t i = 0; i + 1 < ctx_.events.size(); ++i) {
            if (ctx_.events[i].end > ctx_.events[i + 1].start)
                ctx_.events[i].end = ctx_.events[i + 1].start;
        }
        next_event_ = 0;
    }

    // 10e. Control rule parsing from [CONTROLS] section text.
    //      Surface parser errors so malformed rules don't get silently
    //      dropped (P1-C11). Legacy input.c returns ERR_RULE / ERR_KEYWORD
    //      / ERR_DATETIME via error_setInpError; we use the same channel
    //      via ctx.error_code / error_message.
    if (ctx_.control_rules.count() > 0) {
        // parseRuleText() appends, so clear first: initialize() may run more
        // than once over the same rule store, and swmm_control_add_rule()
        // compiles into this same engine once the model is initialized.
        controls_.clearRules();
        for (size_t i = 0; i < ctx_.control_rules.rule_text.size(); ++i) {
            const auto& text = ctx_.control_rules.rule_text[i];
            const int rc = controls_.parseRuleText(text, ctx_);
            if (rc < 0) {
                const auto& pe = controls_.lastParseError();
                ctx_.error_code = 217;  // legacy ERR_RULE (error.h:174)
                ctx_.error_message =
                    "Failed to parse [CONTROLS] rule block #" +
                    std::to_string(i + 1);
                if (pe.line > 0)
                    ctx_.error_message += ", line " + std::to_string(pe.line);
                if (!pe.message.empty())
                    ctx_.error_message += ": " + pe.message;
                ctx_.errors.push_back(ctx_.error_message);
                return;
            }
        }
    }
}

// ============================================================================
// initHydrology() — initialize hydrology solvers: runoff, snow, GW, LID
// ============================================================================

/**
 * @brief Initialize hydrology solvers: runoff, snow, groundwater, LID.
 *
 * @details Populates RunoffSoA from subcatchment properties, resizes gage
 *          states, and initializes snow, groundwater, and LID solvers.
 */
void SWMMEngine::initHydrology() noexcept {
    // 1. LID solver — must run before the runoff solver: it fills
    //    ctx_.subcatches.total_lid_area_ft2, which RunoffSolver::init()
    //    subtracts from the runoff-generating area (Gap #23). With the
    //    order reversed the array is still zeroed, the subtraction is a
    //    no-op, and the LID footprint's rainfall is counted twice
    //    (issue #131).
    lid_.init(ctx_);
    // A4: size the per-layer species block against the units the manager
    // just built, and seed whatever they already hold.
    transport::initLidLayerAge(ctx_, lid_);
    transport::initLidLayerTemperature(ctx_, lid_);

    // 2. Runoff solver: populate RunoffSoA from subcatchment properties
    runoff_.init(ctx_);

    // 2b. Store base values for pattern-adjusted subcatchment parameters
    {
        int ns = ctx_.n_subcatches();
        auto uns = static_cast<std::size_t>(ns);
        ctx_.base_n_perv.resize(uns);
        ctx_.base_ds_perv.resize(uns);
        for (int i = 0; i < ns; ++i) {
            auto ui = static_cast<std::size_t>(i);
            ctx_.base_n_perv[ui]  = ctx_.subcatches.n_perv[ui];
            ctx_.base_ds_perv[ui] = ctx_.subcatches.ds_perv[ui];
        }
        // Check if any pattern assignments exist
        ctx_.has_subcatch_adj_patterns = false;
        for (size_t i = 0; i < ctx_.subcatch_n_perv_pattern.size(); ++i)
            if (ctx_.subcatch_n_perv_pattern[i] >= 0)
            { ctx_.has_subcatch_adj_patterns = true; break; }
        if (!ctx_.has_subcatch_adj_patterns) {
            for (size_t i = 0; i < ctx_.subcatch_d_store_pattern.size(); ++i)
                if (ctx_.subcatch_d_store_pattern[i] >= 0)
                { ctx_.has_subcatch_adj_patterns = true; break; }
        }
        if (!ctx_.has_subcatch_adj_patterns) {
            for (size_t i = 0; i < ctx_.subcatch_infil_pattern.size(); ++i)
                if (ctx_.subcatch_infil_pattern[i] >= 0)
                { ctx_.has_subcatch_adj_patterns = true; break; }
        }
    }

    // 3. Gage states
    gage_states_.resize(static_cast<std::size_t>(ctx_.n_gages()));

    // 4. Snow solver: init and populate from parsed snowpack data
    snow_.init(ctx_.n_subcatches());
    {
        auto& soa = snow_.state();
        int unit_sys_snow = ucf::getUnitSystem(static_cast<int>(ctx_.options.flow_units));

        // Transfer global snowmelt parameters
        soa.tipm = ctx_.options.snow_ati_wt;
        soa.rnm  = ctx_.options.snow_nrg_ratio;

        // Transfer ADC curves from options
        for (int k = 0; k < 10; ++k) {
            soa.adc_imperv[k] = ctx_.options.adc_imperv[k];
            soa.adc_perv[k]   = ctx_.options.adc_perv[k];
        }

        // Transfer per-subcatchment snowpack parameters
        for (int i = 0; i < ctx_.n_subcatches(); ++i) {
            auto ui = static_cast<std::size_t>(i);
            int sp_idx = ctx_.subcatches.snowpack[ui];
            if (sp_idx < 0) continue;
            auto usp = static_cast<std::size_t>(sp_idx);

            // Surface types: 0=PLOWABLE, 1=IMPERVIOUS, 2=PERVIOUS
            const std::array<double, 7>* surfaces[3] = {nullptr, nullptr, nullptr};
            if (usp < ctx_.snowpacks.plowable.size())   surfaces[0] = &ctx_.snowpacks.plowable[usp];
            if (usp < ctx_.snowpacks.impervious.size())  surfaces[1] = &ctx_.snowpacks.impervious[usp];
            if (usp < ctx_.snowpacks.pervious.size())    surfaces[2] = &ctx_.snowpacks.pervious[usp];

            for (int k = 0; k < snow::N_SUBAREAS; ++k) {
                auto idx = static_cast<std::size_t>(i * snow::N_SUBAREAS + k);
                if (!surfaces[k]) continue;
                const auto& p = *surfaces[k];
                // p[0]=Cmin, p[1]=Cmax, p[2]=Tbase, p[3]=FWF, p[4]=SD0, p[5]=FW0, p[6]=SNN0
                soa.dhmin[idx]  = p[0] / ucf::Ucf[ucf::RAINFALL][unit_sys_snow];
                soa.dhmax[idx]  = p[1] / ucf::Ucf[ucf::RAINFALL][unit_sys_snow];
                soa.tbase[idx]  = p[2];
                if (unit_sys_snow == 1) // SI: convert °C to °F
                    soa.tbase[idx] = soa.tbase[idx] * 9.0 / 5.0 + 32.0;
                soa.fwfrac[idx] = p[3];
                soa.wsnow[idx]  = p[4] / ucf::Ucf[ucf::RAINDEPTH][unit_sys_snow];
                soa.fw[idx]     = p[5] / ucf::Ucf[ucf::RAINDEPTH][unit_sys_snow];
                // F6: the 7th field is SNN0 on the PLOWABLE row and SD100 —
                // the depth at which areal coverage reaches 100 % — on the
                // other two. Legacy splits it exactly this way
                // (`snow.c:348-352`). Pinning `si` to the INITIAL pack depth
                // instead made `asc` identically 1 on every deck, so
                // `rain·(1 − asc)` was always zero and no rain ever reached
                // the ground under a pack unless the deck wrote an explicit
                // ADC row.
                if (k == snow::SNOW_PLOWABLE) {
                    soa.snn[ui] = p[6];
                    // The plowable surface has no SD100 of its own; legacy
                    // leaves its `si` at the init value (0).
                } else {
                    soa.si[idx] = p[6] / ucf::Ucf[ucf::RAINDEPTH][unit_sys_snow];
                }
            }

            // Fractional area of each snow surface (legacy snow_initSnowpack,
            // snow.c:178-182). Without this the SoA fArea stays zero, so
            // plowSnow() and the melt area-weighting treat every surface as
            // having no area — packs could never accumulate (Gap: M3 repair).
            //   plowable = snn * fracImperv
            //   imperv   = (1 - snn) * fracImperv
            //   pervious = 1 - fracImperv
            {
                double fimp = ctx_.subcatches.frac_imperv[ui];
                double snn  = soa.snn[ui];
                auto base = static_cast<std::size_t>(i * snow::N_SUBAREAS);
                soa.fArea[base + snow::SNOW_PLOWABLE] = snn * fimp;
                soa.fArea[base + snow::SNOW_IMPERV]   = (1.0 - snn) * fimp;
                soa.fArea[base + snow::SNOW_PERV]     = 1.0 - fimp;
                // Match legacy: zero initial state where the surface has no
                // area (snow.c:184-197).
                for (int k = 0; k < snow::N_SUBAREAS; ++k) {
                    auto idx = base + static_cast<std::size_t>(k);
                    if (soa.fArea[idx] <= 0.0) {
                        soa.wsnow[idx] = 0.0;
                        soa.fw[idx]    = 0.0;
                        soa.si[idx]    = 0.0;
                    }
                }
            }

            // Transfer plowing/removal parameters
            if (usp < ctx_.snowpacks.removal.size()) {
                const auto& r = ctx_.snowpacks.removal[usp];
                auto sf = static_cast<std::size_t>(i * 5);
                soa.weplow[ui]  = r[0] / ucf::Ucf[ucf::RAINDEPTH][unit_sys_snow];
                soa.sfrac[sf+0] = r[1]; // fraction removed
                soa.sfrac[sf+1] = r[2]; // fraction to impervious
                soa.sfrac[sf+2] = r[3]; // fraction to pervious
                soa.sfrac[sf+3] = r[4]; // fraction immediate melt
                soa.sfrac[sf+4] = r[5]; // fraction to other subcatchment

                // Resolve target subcatchment for plowed snow
                if (usp < ctx_.snowpacks.removal_subcatch.size() &&
                    !ctx_.snowpacks.removal_subcatch[usp].empty()) {
                    soa.to_subcatch[ui] = ctx_.subcatch_names.find(
                        ctx_.snowpacks.removal_subcatch[usp]);
                }
            }
        }
    }

    // Date sanity (legacy project_validate, project.c:161-169): the run must
    // end after it starts, and the report window must begin before the end.
    // Push + set_error only (this function is void); the collected errors
    // abort the run at the initialize-time errors gate.
    // Gated on an EXPLICIT end date: a programmatically-built model reaches
    // initialize() through swmm_finalize_model with end_date still at its
    // zero default, and the check must not reject that build path.
    if (ctx_.options.end_date > 0.0 &&
        ctx_.options.end_date <= ctx_.options.start_date) {
        ctx_.errors.push_back(format_error(ERR_START_DATE, ""));
        set_error(SWMM_ERR_PARSE, ctx_.errors.back().c_str());
    } else if (ctx_.options.report_start > 0.0 &&
               ctx_.options.end_date <= ctx_.options.report_start) {
        ctx_.errors.push_back(format_error(ERR_REPORT_DATE, ""));
        set_error(SWMM_ERR_PARSE, ctx_.errors.back().c_str());
    }

    // Climate-file requirement (legacy climate_validate, climate.c:850-858):
    // FILE wind, FILE evaporation, or TEMPERATURE (Hargreaves) evaporation
    // need the [TEMPERATURE] FILE — its daily min/max drive them.
    if ((ctx_.options.wind_type == 1 || ctx_.options.evap_type == 4 ||
         ctx_.options.evap_type == 3) &&
        ctx_.options.temp_file.empty()) {
        ctx_.errors.push_back(format_error(ERR_NO_CLIMATE_FILE, ""));
        set_error(SWMM_ERR_PARSE, ctx_.errors.back().c_str());
    }

    // 5. Climate: transfer evaporation data from options to climate state
    {
        int evap_type = ctx_.options.evap_type;
        if (evap_type == 0) {
            // CONSTANT
            // EVAPRATE units follow the project's unit system: in/day (US) or
            // mm/day (SI). The conversion factor must therefore be selected by
            // the model's flow-unit system, not hardcoded to the US index — an
            // SI (CMS) model with `CONSTANT 3.0` means 3 mm/day, and using the
            // US factor (1036800 vs 26334720) over-evaporates by ~25×.
            ctx_.climate_state.evap_method = climate::EvapMethod::CONSTANT;
            ctx_.climate_state.evap_rate = ctx_.options.evap_values[0]
                               / ucf::UCF(ucf::EVAPRATE, ctx_.options);
        } else if (evap_type == 1) {
            // MONTHLY
            ctx_.climate_state.evap_method = climate::EvapMethod::MONTHLY;
            ctx_.climate_state.evaprate_ucf = ucf::UCF(ucf::EVAPRATE, ctx_.options);
            for (int i = 0; i < 12; ++i)
                ctx_.climate_state.monthly_evap[i] = ctx_.options.evap_values[i];
        } else if (evap_type == 2) {
            ctx_.climate_state.evap_method = climate::EvapMethod::TIMESERIES;
        } else if (evap_type == 3) {
            ctx_.climate_state.evap_method = climate::EvapMethod::TEMPERATURE;
        } else if (evap_type == 4) {
            ctx_.climate_state.evap_method = climate::EvapMethod::PAN;
        }

        // Transfer latitude for Hargreaves ET calculation
        ctx_.climate_state.latitude = ctx_.options.snow_lat;

        // Transfer site elevation for psychrometric constant (Gap #8)
        ctx_.climate_state.elev = ctx_.options.snow_elev;

        // Transfer longitude/solar-time correction. The [TEMPERATURE] SNOWMELT
        // field and the C API store minutes; ClimateState::dtlong is in hours
        // (used directly in the sunrise/sunset calc), matching legacy
        // climate.c (Temp.dtlong = x[5] / 60.0).
        ctx_.climate_state.dtlong = ctx_.options.snow_dtlong / 60.0;

        // Transfer monthly adjustment arrays
        for (int i = 0; i < 12; ++i) {
            ctx_.climate_state.adjust_temp[i] = ctx_.adjust_temp[i];
            ctx_.climate_state.adjust_evap[i] = ctx_.adjust_evap[i];
            ctx_.climate_state.adjust_rain[i] = ctx_.adjust_rain[i];
            ctx_.climate_state.adjust_hydcon[i] = ctx_.adjust_hydcon[i];
        }

        // Resolve timeseries names to table indices
        if (ctx_.options.temp_source == 1 && !ctx_.options.temp_ts_name.empty()) {
            ctx_.climate_state.temp_ts_index = ctx_.find_timeseries(ctx_.options.temp_ts_name);
        }

        // With NO temperature source, legacy's air temperature stays at its
        // project_setDefaults value: `Temp.ta = 70.0` degF (project.c:941).
        // climate.c's setTemp never reassigns it without FILE_TEMP or
        // TSERIES_TEMP data, so a deck with no [TEMPERATURE] section computes
        // AND reports with 70 degF — 21.11 degC after the SI display
        // conversion (221-h-h-si-units-elements, 15-subs-si-units). A deck
        // with no subcatchments still reports a raw 0, because legacy assigns
        // SYS_TEMPERATURE only inside output_saveSubcatchResults — that path
        // is the has_subcatchments guard at the snapshot, not this default.
        //
        // An API-prescribed temperature still overrides this, exactly as
        // legacy's `Temp.apiTemp` does (climate.c:1222).
        if (ctx_.options.temp_source == 0 && ctx_.climate_state.temp_ts_index < 0) {
            ctx_.climate_state.temperature_src = 70.0;
            ctx_.climate_state.temperature = 70.0;
        }
        if (evap_type == 2 && !ctx_.options.evap_ts_name.empty()) {
            ctx_.climate_state.evap_ts_index = ctx_.find_timeseries(ctx_.options.evap_ts_name);
        }

        // Resolve recovery pattern name to pattern index (case-insensitive)
        if (!ctx_.options.evap_recovery_pat.empty()) {
            int np = ctx_.patterns.count();
            for (int i = 0; i < np; ++i) {
                if (ieq(ctx_.patterns.names[static_cast<std::size_t>(i)],
                        ctx_.options.evap_recovery_pat)) {
                    ctx_.climate_state.recovery_pat_index = i;
                    break;
                }
            }
        }

        // Open climate file if temperature or evaporation uses FILE source.
        // Use the RESOLVED path: `.original` is the token as authored, so a
        // relative one fopen()s against the process cwd rather than the .inp
        // directory — and ClimateFileReader::open just returns false, which
        // nothing here checks, so the run silently proceeds with no climate
        // data. `.absolute` is filled by resolve_external_file_slots; fall back
        // to the token for programmatic models where the resolver never ran.
        if ((ctx_.options.temp_source == 2 || evap_type == 4) &&
            !ctx_.options.temp_file.empty()) {
            int us = ucf::getUnitSystem(static_cast<int>(ctx_.options.flow_units));
            const std::string& climate_path =
                !ctx_.options.temp_file.absolute.empty()
                    ? ctx_.options.temp_file.absolute
                    : ctx_.options.temp_file.original;
            if (!climate_file_.open(climate_path,
                                    ctx_.options.temp_file_start, us,
                                    ctx_.options.temp_units)) {
                ctx_.warnings.push_back(
                    "[TEMPERATURE] climate file '" + climate_path +
                    "' could not be opened or its format was not recognised — "
                    "no climate data will be applied");
            }
        }
    }

    // 6. Groundwater solver: init and populate from parsed aquifer/GW data
    groundwater_.init(ctx_.n_subcatches());
    {
        auto& gw = groundwater_.state();
        int unit_sys = ucf::getUnitSystem(static_cast<int>(ctx_.options.flow_units));
        for (int i = 0; i < ctx_.n_subcatches(); ++i) {
            auto ui = static_cast<std::size_t>(i);
            int aq_idx = ctx_.subcatches.gw_aquifer[ui];
            if (aq_idx < 0) continue;
            auto uaq = static_cast<std::size_t>(aq_idx);

            // Copy aquifer properties
            gw.porosity[ui]         = ctx_.aquifers.porosity[uaq];
            gw.field_cap[ui]        = ctx_.aquifers.field_capacity[uaq];
            gw.wilt_point[ui]       = ctx_.aquifers.wilting_point[uaq];
            // All conversions match legacy gwater.c line 170-182
            gw.k_sat[ui]            = ctx_.aquifers.conductivity[uaq]
                                      / ucf::Ucf[ucf::RAINFALL][unit_sys];
            gw.k_slope[ui]          = ctx_.aquifers.conduct_slope[uaq];
            gw.tension_slope[ui]    = ctx_.aquifers.tension_slope[uaq]
                                      / ucf::Ucf[ucf::LENGTH][unit_sys];
            gw.upper_evap_frac[ui]  = ctx_.aquifers.upper_evap[uaq];
            // Resolve upper evaporation pattern name to index (case-insensitive)
            gw.upper_evap_pat[ui] = -1;
            const auto& pat_name = ctx_.aquifers.upper_evap_pat[uaq];
            if (!pat_name.empty())
                gw.upper_evap_pat[ui] = ctx_.patterns.find(pat_name);
            gw.lower_evap_depth[ui] = ctx_.aquifers.lower_evap[uaq]
                                      / ucf::Ucf[ucf::LENGTH][unit_sys];
            gw.lower_loss_coeff[ui] = ctx_.aquifers.lower_loss[uaq]
                                      / ucf::Ucf[ucf::RAINFALL][unit_sys];
            gw.total_depth[ui]      = (ctx_.subcatches.gw_surf_elev[ui]
                                       - ctx_.aquifers.bottom_elev[uaq])
                                      / ucf::Ucf[ucf::LENGTH][unit_sys];

            // Copy GW lateral flow coefficients
            gw.a1[ui]     = ctx_.subcatches.gw_a1[ui];
            gw.b1[ui]     = ctx_.subcatches.gw_b1[ui];
            gw.a2[ui]     = ctx_.subcatches.gw_a2[ui];
            gw.b2[ui]     = ctx_.subcatches.gw_b2[ui];
            gw.a3[ui]     = ctx_.subcatches.gw_a3[ui];
            gw.h_star[ui] = ctx_.subcatches.gw_hstar[ui]
                            / ucf::Ucf[ucf::LENGTH][unit_sys];

            // Initial conditions from aquifer
            gw.theta[ui]      = ctx_.aquifers.upper_moist[uaq];
            gw.lower_depth[ui] = ctx_.aquifers.water_table_elev[uaq]
                                 / ucf::Ucf[ucf::LENGTH][unit_sys];
        }

        // Parse [GWF] custom expressions (stored in ext_options during input parsing).
        // Format: ext_options["GWF:<subcatch_name>:LATERAL"] = expression string
        //         ext_options["GWF:<subcatch_name>:DEEP"]    = expression string
        for (int i = 0; i < ctx_.n_subcatches(); ++i) {
            auto ui = static_cast<std::size_t>(i);
            if (ctx_.subcatches.gw_aquifer[ui] < 0) continue;

            const std::string& sc_name = ctx_.subcatch_names.names()[ui];

            auto lat_it = ctx_.options.ext_options.find("GWF:" + sc_name + ":LATERAL");
            if (lat_it != ctx_.options.ext_options.end() && !lat_it->second.empty()) {
                mathexpr::Expression expr;
                if (mathexpr::parse(lat_it->second, expr) == 0 && expr.valid) {
                    mathexpr::bind_variables(expr,
                        groundwater::GW_VAR_NAMES, groundwater::GWV_MAX);
                    gw.lateral_expr[ui] = std::move(expr);
                }
            }

            auto deep_it = ctx_.options.ext_options.find("GWF:" + sc_name + ":DEEP");
            if (deep_it != ctx_.options.ext_options.end() && !deep_it->second.empty()) {
                mathexpr::Expression expr;
                if (mathexpr::parse(deep_it->second, expr) == 0 && expr.valid) {
                    mathexpr::bind_variables(expr,
                        groundwater::GW_VAR_NAMES, groundwater::GWV_MAX);
                    gw.deep_expr[ui] = std::move(expr);
                }
            }
        }
    }

    // Gap #81: Aquifer parameter validation
    {
        int naq = ctx_.aquifers.count();
        for (int a = 0; a < naq; ++a) {
            auto ua = static_cast<std::size_t>(a);
            const std::string& aname = ctx_.aquifers.names[ua];
            double por  = ctx_.aquifers.porosity[ua];
            double fc   = ctx_.aquifers.field_capacity[ua];
            double wp   = ctx_.aquifers.wilting_point[ua];
            double ksat = ctx_.aquifers.conductivity[ua];
            double uevap = ctx_.aquifers.upper_evap[ua];
            bool bad = (por <= 0.0 || por > 1.0
                     || fc  < 0.0  || fc  >= por
                     || wp  < 0.0  || wp  >= fc
                     || ksat < 0.0
                     || uevap < 0.0 || uevap > 1.0);
            if (bad) {
                ctx_.errors.push_back(format_error(ERR_AQUIFER_PARAMS, aname));
                set_error(SWMM_ERR_PARSE, ctx_.errors.back().c_str());
            }
        }
    }

    // 7. LID solver: initialized first (step 1) so total_lid_area_ft2 is
    //    populated before RunoffSolver::init() consumes it.

    // Gap #82: LID parameter validation
    {
        int nlid = ctx_.lid_controls.count();
        for (int j = 0; j < nlid; ++j) {
            auto uj = static_cast<std::size_t>(j);
            const std::string& lname = ctx_.lid_controls.names[uj];
            // Soil layer: [0]=thickness, [1]=porosity, [2]=fieldCap, [3]=wiltPt
            if (uj < ctx_.lid_controls.soil.size()) {
                const auto& s = ctx_.lid_controls.soil[uj];
                if (s[0] > 0.0) { // soil layer defined
                    if (s[1] <= 0.0 || s[1] > 1.0 || s[2] < 0.0 || s[2] >= s[1]
                        || s[3] < 0.0 || s[3] >= s[2]) {
                        ctx_.errors.push_back(format_error(ERR_LID_PARAMS, lname));
                        set_error(SWMM_ERR_PARSE, ctx_.errors.back().c_str());
                    }
                }
            }
            // Storage layer: [0]=thickness, [1]=void RATIO (voids/solids),
            // converted to a fraction p/(p+1) in LIDSolver::init (issue #102).
            // Legacy readStorageData accepts any non-negative ratio; a cap at
            // 1.0 here would silently limit storage to 50 % voids.
            if (uj < ctx_.lid_controls.storage.size()) {
                const auto& s = ctx_.lid_controls.storage[uj];
                if (s[0] > 0.0 && s[1] <= 0.0) {
                    ctx_.errors.push_back(format_error(ERR_LID_PARAMS, lname));
                    set_error(SWMM_ERR_PARSE, ctx_.errors.back().c_str());
                }
            }
            // Pavement layer: [0]=thickness, [1]=void RATIO — same convention.
            if (uj < ctx_.lid_controls.pavement.size()) {
                const auto& p = ctx_.lid_controls.pavement[uj];
                if (p[0] > 0.0 && p[1] <= 0.0) {
                    ctx_.errors.push_back(format_error(ERR_LID_PARAMS, lname));
                    set_error(SWMM_ERR_PARSE, ctx_.errors.back().c_str());
                }
            }
            // Drain layer: [4]=hOpen, [5]=hClose — hOpen must be >= hClose
            if (uj < ctx_.lid_controls.drain.size()) {
                const auto& d = ctx_.lid_controls.drain[uj];
                if (d[0] > 0.0 && d[4] < d[5]) { // drain enabled and hOpen < hClose
                    ctx_.errors.push_back(format_error(ERR_LID_PARAMS, lname));
                    set_error(SWMM_ERR_PARSE, ctx_.errors.back().c_str());
                }
            }
        }
    }
}

// ============================================================================
// initQuality() — initialize water quality: landuse, surface quality, mass bal
// ============================================================================

/**
 * @brief Initialize water quality: landuse solver, surface quality, mass balance.
 *
 * @details Initializes the quality routing solver, treatment arrays,
 *          buildup/washoff data, landuse solver parameters, surface quality
 *          state, quality mass balance vectors, and initial buildup from
 *          antecedent dry days.
 */
void SWMMEngine::initQuality() noexcept {
    // 7. Quality solver
    quality_.init(ctx_.n_nodes(), ctx_.n_links(), ctx_.n_pollutants());

    // Seed initial concentrations from [POLLUTANTS] Cinit. A wet node/link
    // starts at the pollutant's initial concentration, a dry one at zero —
    // legacy qualrout.c qualrout_init(), called from routing_open() at the
    // same point in the sequence (after the router has set initial depths).
    // Without this every run started clean and "Initial Stored Mass" was
    // always 0.
    {
        const int np = ctx_.n_pollutants();
        if (np > 0) {
            // Legacy qualrout.c: static const double ZeroDepth = 0.003281 (1 mm).
            constexpr double zero_depth = 0.003281;
            for (int i = 0; i < ctx_.n_nodes(); ++i) {
                const auto ui = static_cast<std::size_t>(i);
                const bool wet = ctx_.nodes.depth[ui] > zero_depth;
                for (int p = 0; p < np; ++p) {
                    const auto idx = ui * static_cast<std::size_t>(np) +
                                     static_cast<std::size_t>(p);
                    if (idx >= ctx_.nodes.conc.size()) continue;
                    const double c = wet
                        ? ctx_.pollutants.init_conc[static_cast<std::size_t>(p)]
                        : 0.0;
                    ctx_.nodes.conc[idx]     = c;
                    ctx_.nodes.conc_old[idx] = c;
                }
            }
            for (int j = 0; j < ctx_.n_links(); ++j) {
                const auto uj = static_cast<std::size_t>(j);
                const bool wet = ctx_.links.depth[uj] > zero_depth;
                for (int p = 0; p < np; ++p) {
                    const auto idx = uj * static_cast<std::size_t>(np) +
                                     static_cast<std::size_t>(p);
                    if (idx >= ctx_.links.conc.size()) continue;
                    const double c = wet
                        ? ctx_.pollutants.init_conc[static_cast<std::size_t>(p)]
                        : 0.0;
                    ctx_.links.conc[idx]     = c;
                    ctx_.links.conc_old[idx] = c;
                }
            }

            // [INITIAL_QUALITY] per-element pollutant overrides — applied
            // after the global Cinit seed ("specific beats general"), wet or
            // dry (D-IQ4: an explicit per-element row is the user's intent,
            // unlike the global default; mass stays C·V-safe because a dry
            // element holds no volume). One write site feeds all three
            // quality engines (D-IQ6): ARD and LARD both initialize lazily
            // on their first step FROM these arrays, and the legacy CSTR
            // routes on them directly. Reserved-species rows (age /
            // temperature, kind < 0) are consumed at their seed sites (E-A3).
            // When hotstart quality restore lands it must win over both
            // seeds — apply it AFTER this block (D-IQ5).
            {
                const auto& iq = ctx_.initial_quality;
                for (int r = 0; r < iq.count(); ++r) {
                    const auto ur = static_cast<std::size_t>(r);
                    const int k = iq.kind[ur];
                    if (k < 0 || k >= np) continue;   // reserved/unresolved
                    const int ei = iq.elem_idx[ur];
                    if (ei < 0) continue;
                    auto& conc     = iq.is_link[ur] ? ctx_.links.conc
                                                    : ctx_.nodes.conc;
                    auto& conc_old = iq.is_link[ur] ? ctx_.links.conc_old
                                                    : ctx_.nodes.conc_old;
                    const auto idx = static_cast<std::size_t>(ei) *
                                     static_cast<std::size_t>(np) +
                                     static_cast<std::size_t>(k);
                    if (idx >= conc.size()) continue;
                    conc[idx]     = iq.value[ur];
                    conc_old[idx] = iq.value[ur];
                }
            }
        }
    }

    // 10. Treatment: resize for nodes x pollutants + compile expressions
    if (ctx_.n_pollutants() > 0 && ctx_.n_nodes() > 0) {
        if (ctx_.treatment.n_nodes == 0) {
            ctx_.treatment.resize(ctx_.n_nodes(), ctx_.n_pollutants());
        }
        // Compile treatment expressions
        int np = ctx_.n_pollutants();
        for (int n = 0; n < ctx_.n_nodes(); ++n) {
            for (int p = 0; p < np; ++p) {
                auto idx = static_cast<std::size_t>(n * np + p);
                const auto& expr_str = ctx_.treatment.expressions[idx];
                if (expr_str.empty()) continue;
                treatment::TreatExpr te;
                if (treatment::parse(expr_str, te) == 0) {
                    te.pollutant_idx = p;
                    ctx_.treatment.compiled[idx] = std::move(te);
                    ctx_.treatment.has_treatment[static_cast<std::size_t>(n)] = true;
                }
            }
        }
    }

    // Gap #85: Cyclic treatment expression detection
    // A cycle exists when pollutant p's expression references R_POLLUT(q)
    // and pollutant q's expression references R_POLLUT(p) (directly or transitively).
    if (ctx_.n_pollutants() > 0 && ctx_.n_nodes() > 0) {
        int np = ctx_.n_pollutants();
        int nn = ctx_.n_nodes();
        for (int n = 0; n < nn; ++n) {
            auto un = static_cast<std::size_t>(n);
            if (!ctx_.treatment.has_treatment[un]) continue;
            // Build dependency: dep[p] = set of pollutants that p's expression depends on
            std::vector<std::vector<int>> dep(static_cast<std::size_t>(np));
            for (int p = 0; p < np; ++p) {
                auto idx = static_cast<std::size_t>(n * np + p);
                const auto& te = ctx_.treatment.compiled[idx];
                for (const auto& tok : te.tokens) {
                    if (tok.type == treatment::TokenType::VARIABLE
                        && tok.var == treatment::TreatVar::R_POLLUT
                        && tok.pollut_ref >= 0 && tok.pollut_ref < np
                        && tok.pollut_ref != p) {
                        dep[static_cast<std::size_t>(p)].push_back(tok.pollut_ref);
                    }
                }
            }
            // DFS cycle detection
            std::vector<int> color(static_cast<std::size_t>(np), 0); // 0=white,1=gray,2=black
            bool cycle = false;
            std::function<void(int)> dfs = [&](int p) {
                if (cycle) return;
                color[static_cast<std::size_t>(p)] = 1;
                for (int q : dep[static_cast<std::size_t>(p)]) {
                    if (color[static_cast<std::size_t>(q)] == 1) { cycle = true; return; }
                    if (color[static_cast<std::size_t>(q)] == 0) dfs(q);
                }
                color[static_cast<std::size_t>(p)] = 2;
            };
            for (int p = 0; p < np && !cycle; ++p)
                if (color[static_cast<std::size_t>(p)] == 0) dfs(p);
            if (cycle) {
                const auto& nname = ctx_.node_names.names()[un];
                ctx_.errors.push_back(format_error(ERR_CYCLIC_TREATMENT, nname));
                set_error(SWMM_ERR_PARSE, ctx_.errors.back().c_str());
            }
        }
    }

    // 11. Buildup/washoff: resize for landuses x pollutants
    if (ctx_.n_landuses() > 0 && ctx_.n_pollutants() > 0) {
        if (ctx_.buildup.n_landuses == 0) {
            ctx_.buildup.resize(ctx_.n_landuses(), ctx_.n_pollutants());
            ctx_.washoff.resize(ctx_.n_landuses(), ctx_.n_pollutants());
        }
    }

    // 11b. Initialize landuse quality solver and surface quality state
    if (ctx_.n_pollutants() > 0) {
        int nlu = std::max(ctx_.n_landuses(), 1);
        int np  = ctx_.n_pollutants();
        landuse_solver_.init(nlu, np);
        surface_quality_.resize(ctx_.n_subcatches(), nlu, np);
        ctx_.subcatches.resize_total_load(ctx_.n_subcatches(), np);

        // Transfer parsed BuildupData/WashoffData into LanduseSolver params
        refreshLanduseParams();

        // Initialize quality mass balance vectors
        ctx_.mass_balance.resize_quality(np);

        // Compute initial buildup from antecedent dry days
        // (matching legacy landuse_getInitBuildup)
        // Buildup is stored PER LAND USE: buildup[bu_idx(sc,lu,p)] = mass/normalizer
        double dry_days = ctx_.options.dry_days;
        if (dry_days > 0.0 && ctx_.n_landuses() > 0) {
            for (int i = 0; i < ctx_.n_subcatches(); ++i) {
                auto ui = static_cast<std::size_t>(i);
                for (int lu = 0; lu < nlu; ++lu) {
                    auto cov_idx = ui * static_cast<std::size_t>(nlu)
                                   + static_cast<std::size_t>(lu);
                    double frac = (cov_idx < ctx_.subcatches.coverage.size())
                                  ? ctx_.subcatches.coverage[cov_idx] / 100.0 : 0.0;
                    if (frac <= 0.0) continue;

                    for (int p = 0; p < np; ++p) {
                        auto k = static_cast<std::size_t>(lu * np + p);
                        const auto& bp = landuse_solver_.buildup_params[k];
                        if (bp.type == landuse::BuildupType::NONE) continue;

                        double mass = 0.0;
                        double c0 = bp.coeff[0], c1 = bp.coeff[1], c2 = bp.coeff[2];
                        switch (bp.type) {
                            case landuse::BuildupType::POWER:
                                mass = std::min(c1 * std::pow(dry_days, c2), c0);
                                break;
                            case landuse::BuildupType::EXPON:
                                mass = c0 * (1.0 - std::exp(-c1 * dry_days));
                                break;
                            case landuse::BuildupType::SATUR:
                                mass = (c2 + dry_days > 0.0) ? c0 * dry_days / (c2 + dry_days) : 0.0;
                                break;
                            default: break;
                        }

                        // Store per-landuse buildup (per normalizer unit)
                        auto bu = surface_quality_.bu_idx(i, lu, p);
                        surface_quality_.buildup[bu] = mass;

                        // Normalize to absolute mass for mass balance
                        double norm = (bp.normalizer == 0)
                            ? frac * ctx_.subcatches.area[ui]
                            : frac * ctx_.subcatches.curb_length[ui];
                        ctx_.mass_balance.qual_init_buildup[static_cast<std::size_t>(p)] += mass * norm;
                    }
                }
            }
        }
    }
}

// ============================================================================
// initGeometry() — initialize node/link geometry: crown elevations, full vols
// ============================================================================

/**
 * @brief Initialize node/link geometry: crown elevations, full volumes.
 *
 * @details Computes node crown elevations from connecting link crowns and
 *          tracks node degree (connectivity count). Initializes node full
 *          volumes from full depth using the node volume function.
 */
void SWMMEngine::initGeometry() noexcept {
    // 12. Initialize node crown elevations (matching legacy dynwave_init)
    //     Crown elevation = highest connecting link crown
    for (int i = 0; i < ctx_.n_nodes(); ++i) {
        ctx_.nodes.crown_elev[static_cast<std::size_t>(i)] =
            ctx_.nodes.invert_elev[static_cast<std::size_t>(i)];
    }
    for (int j = 0; j < ctx_.n_links(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        int n1 = ctx_.links.node1[uj];
        int n2 = ctx_.links.node2[uj];
        auto un1 = static_cast<std::size_t>(n1);
        auto un2 = static_cast<std::size_t>(n2);
        // PARITY dynwave.c:143-152: crown elevations accumulate over ALL link
        // types — legacy adds Link.offset1/2 + Link.xsect.yFull for every
        // link, so a weir or orifice hanging off a junction raises its crown
        // (and thus its EXTRAN surcharge threshold), e.g. Bellinge G75F65Y.
        // Legacy link_readParams mirrors offset2 = offset1 for orifices,
        // weirs and outlets (link.c:368-391); weir/outlet crest heights live
        // in the subtype tables here, so reconstruct them per type. Pumps
        // keep offset 0 and yFull 0 (link.c:334-335, pump_validate), making
        // their contribution the node invert — a no-op under MAX.
        double off1 = ctx_.links.offset1[uj];
        double off2 = ctx_.links.offset2[uj];
        switch (ctx_.links.type[uj]) {
            case LinkType::ORIFICE:
                off2 = off1;                              // link.c:368-369
                break;
            case LinkType::WEIR: {
                const int wr = ctx_.link_subtypes.weir_row(j);
                off1 = (wr >= 0) ? ctx_.link_subtypes.weirs
                           .crest_height[static_cast<std::size_t>(wr)] : 0.0;
                off2 = off1;                              // link.c:377-378
                break;
            }
            case LinkType::OUTLET: {
                const int olr = ctx_.link_subtypes.outlet_row(j);
                off1 = (olr >= 0) ? ctx_.link_subtypes.outlets
                           .crest_height[static_cast<std::size_t>(olr)] : 0.0;
                off2 = off1;                              // link.c:390-391
                break;
            }
            default: break;  // CONDUIT: parsed offsets; PUMP: zeros
        }
        double z1 = ctx_.nodes.invert_elev[un1] + off1
                     + ctx_.links.xsect_y_full[uj];
        double z2 = ctx_.nodes.invert_elev[un2] + off2
                     + ctx_.links.xsect_y_full[uj];
        if (z1 > ctx_.nodes.crown_elev[un1])
            ctx_.nodes.crown_elev[un1] = z1;
        if (z2 > ctx_.nodes.crown_elev[un2])
            ctx_.nodes.crown_elev[un2] = z2;
        // Track node degree (connectivity count) — conduits only, unchanged.
        if (ctx_.links.type[uj] != LinkType::CONDUIT) continue;
        ctx_.nodes.degree[un1]++;
        ctx_.nodes.degree[un2]++;
    }

    // 12b. PARITY: negate the degree of nodes with NO inflow links so that
    // `degree < 0` marks an "upstream terminal" node — matching legacy
    // flowrout.c::validateGeneralLayout. The EXTRAN surcharge depth update
    // (setNodeDepth) multiplies dy by corr = 0.6 for these nodes
    // (`if (Node[i].degree < 0) corr = 0.6`); without it a surcharging headwater
    // junction (e.g. extran1 node 80408, fed only by a 45-cfs external inflow
    // through a single OUTFLOW conduit) raised its head 1/0.6 = 1.67x too fast.
    // An "inflow link" is one whose DOWNSTREAM node — node2, or node1 when node1
    // is an OUTFALL — is this node. Done here, after BOTH degree passes, so the
    // sign is not clobbered by a subsequent ++.
    {
        const int nn = ctx_.n_nodes();
        std::vector<int> inflow_links(static_cast<std::size_t>(nn), 0);
        for (int j = 0; j < ctx_.n_links(); ++j) {
            auto uj = static_cast<std::size_t>(j);
            int n1 = ctx_.links.node1[uj];
            int n2 = ctx_.links.node2[uj];
            int dn = n2;
            if (n1 >= 0 && n1 < nn &&
                ctx_.nodes.type[static_cast<std::size_t>(n1)] == NodeType::OUTFALL)
                dn = n1;
            if (dn >= 0 && dn < nn) inflow_links[static_cast<std::size_t>(dn)]++;
        }
        for (int i = 0; i < nn; ++i) {
            auto ui = static_cast<std::size_t>(i);
            if (inflow_links[ui] == 0) ctx_.nodes.degree[ui] = -ctx_.nodes.degree[ui];
        }
    }

    // 13. Initialize node full volumes
    for (int j = 0; j < ctx_.n_nodes(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        ctx_.nodes.full_volume[uj] =
            node::getVolume(ctx_.nodes, j, ctx_.nodes.full_depth[uj], &ctx_.tables,
                ucf::getUnitSystem(static_cast<int>(ctx_.options.flow_units)),
                &ctx_.node_subtypes);
    }
}

// ============================================================================
// initMassBalance() — initialize mass balance: record initial storage volumes
// ============================================================================

/**
 * @brief Initialize mass balance: record initial storage volumes.
 *
 * @details Resets mass balance state, then records initial routing storage
 *          (sum of node + link volumes) and initial runoff storage
 *          (ponded depth * area for all subcatchments).
 */
void SWMMEngine::resetStepMassBalance() noexcept {
    ctx_.mass_balance.step_flooding     = 0.0;
    ctx_.mass_balance.step_outflow      = 0.0;
    ctx_.mass_balance.step_dw_inflow    = 0.0;
    ctx_.mass_balance.step_gw_inflow    = 0.0;
    ctx_.mass_balance.step_rdii_inflow  = 0.0;
    ctx_.mass_balance.step_ext_inflow   = 0.0;
}

// ============================================================================
// assembleGWCoupling — pre-compute GW surface water head from routing state
// ============================================================================

void SWMMEngine::assembleGWCoupling(double dt_runoff) noexcept {
    int ns = ctx_.n_subcatches();
    // Build per-subcatchment sw_head from each subcatch's GW receiving node.
    // Legacy: Hsw = Node[n].newDepth + Node[n].invertElev - GW->bottomElev
    for (int i = 0; i < ns; ++i) {
        auto ui = static_cast<std::size_t>(i);
        ctx_.subcatches.gw_sw_head[ui] = 0.0;
        ctx_.subcatches.gw_node_avail_flow[ui] = 0.0;

        int aq_idx = ctx_.subcatches.gw_aquifer[ui];
        if (aq_idx < 0) continue;
        int gw_node = ctx_.subcatches.gw_node[ui];
        if (gw_node < 0) gw_node = ctx_.subcatches.outlet_node[ui];
        if (gw_node >= 0 && gw_node < ctx_.n_nodes()) {
            auto un = static_cast<std::size_t>(gw_node);
            double bottom_elev = ctx_.aquifers.bottom_elev[static_cast<std::size_t>(aq_idx)];
            // Gap #41: fixedDepth option — use a fixed SW head instead of live node depth.
            // gw_tw stores the fixedDepth parameter (tok[9] in [GROUNDWATER] section).
            double fixed_depth = ctx_.subcatches.gw_tw[ui];
            if (fixed_depth > 0.0) {
                // Hsw = fixedDepth + nodeInvertElev - bottomElev  (matching legacy gwater.c)
                ctx_.subcatches.gw_sw_head[ui] =
                    fixed_depth + ctx_.nodes.invert_elev[un] - bottom_elev;
            } else {
                ctx_.subcatches.gw_sw_head[ui] =
                    ctx_.nodes.depth[un] + ctx_.nodes.invert_elev[un] - bottom_elev;
            }

            // Available node flow for GW negative flow limit
            double area_ft2 = ctx_.subcatches.area[ui] * ucf::ACRES_TO_FT2;
            if (area_ft2 > 0.0 && dt_runoff > 0.0) {
                ctx_.subcatches.gw_node_avail_flow[ui] =
                    (ctx_.nodes.inflow[un] + ctx_.nodes.volume[un] / dt_runoff) / area_ft2;
            }
        }
    }
}

// ============================================================================
// assembleRunon — subcatch-to-subcatch + outfall runon into runon_inflow[]
// ============================================================================

void SWMMEngine::assembleRunon(double dt_runoff) noexcept {
    std::fill(ctx_.subcatches.runon_inflow.begin(),
              ctx_.subcatches.runon_inflow.end(), 0.0);

    // Subcatch-to-subcatch: upstream runoff → downstream runon (CFS)
    for (int i = 0; i < ctx_.n_subcatches(); ++i) {
        auto ui = static_cast<std::size_t>(i);
        int out_sc = ctx_.subcatches.outlet_subcatch[ui];
        // `out_sc != i` is legacy's guard (subcatch.c:546-548,
        // `k >= 0 && k != subcatchIndex`) and its absence here was the whole
        // defect: a subcatchment routed to ITSELF fed its own runoff back into
        // its own run-on every step and recirculated. Measured against legacy
        // 5.x on the selfroute fixture: 2.328 acre-feet of surface runoff
        // booked against legacy's 0.417 — 5.6× — with −265 % continuity, while
        // direct, 2-deep, 3-deep and all-direct cascades all agreed to the
        // digit.
        //
        // A self-route is not a cascade. Legacy treats it as a subcatchment
        // that discharges to the system like any other; the outlet name is
        // simply a no-op. `421e95c2` already carries that reading in the
        // runoff ledger (`sheds_to_self`), and this is the site that makes the
        // ledger's number true rather than merely consistent.
        //
        // This guard covers the age and temperature run-on seams below too:
        // both live inside this branch, so a self-route was also feeding its
        // own age and its own heat back to itself.
        if (out_sc >= 0 && out_sc < ctx_.n_subcatches() && out_sc != i) {
            auto usc = static_cast<std::size_t>(out_sc);
            ctx_.subcatches.runon_inflow[usc] += ctx_.subcatches.runoff[ui];
            // A3: run-on carries the donor's runoff age. Without this the
            // FLOW path adds q_runon while the age path adds nothing, and a
            // two-subcatchment cascade — the plan's own A3 criterion —
            // delivers water with no upstream age attached.
            transport::addRunonAge(ctx_, i, out_sc,
                                   ctx_.subcatches.runoff[ui]);
            // H5a: the same seam for temperature.
            transport::addRunonTemperature(ctx_, i, out_sc,
                                           ctx_.subcatches.runoff[ui]);
        }
    }

    // Gap #25: LID drain runon — drain accumulated CFS into runon_inflow and reset.
    // Matches legacy lid_addDrainRunon(): LID drain flow reaching a subcatch is
    // treated as runon inflow to that subcatch for the next runoff step.
    for (int i = 0; i < ctx_.n_subcatches(); ++i) {
        auto ui = static_cast<std::size_t>(i);
        double q = ctx_.subcatches.lid_drain_runon_cfs[ui];
        if (q > 0.0) {
            ctx_.subcatches.runon_inflow[ui] += q;
            ctx_.subcatches.lid_drain_runon_cfs[ui] = 0.0;
            auto& wsr = ctx_.water_age_state.subcatch_lid_drain_age_cfs;
            if (ctx_.options.water_age && ui < wsr.size() &&
                ui < ctx_.water_age_state.subcatch_runon_age_vol_in.size()) {
                ctx_.water_age_state.subcatch_runon_age_vol_in[ui] += wsr[ui];
                wsr[ui] = 0.0;
            }
        }
    }

    // Gap #28: outfall runon — drain accumulated volume into runon_inflow (CFS)
    // and reset the accumulator (matching legacy Outfall[i].vRouted / tStep).
    if (dt_runoff > 0.0) {
        for (int i = 0; i < ctx_.n_subcatches(); ++i) {
            auto ui = static_cast<std::size_t>(i);
            double vol = ctx_.subcatches.outfall_runon_vol[ui];
            if (vol > 0.0) {
                ctx_.subcatches.runon_inflow[ui] += vol / dt_runoff;
                auto& wo = ctx_.water_age_state.subcatch_outfall_age_vol;
                if (ctx_.options.water_age && ui < wo.size() &&
                    ui < ctx_.water_age_state.subcatch_runon_age_vol_in.size()) {
                    ctx_.water_age_state.subcatch_runon_age_vol_in[ui] +=
                        wo[ui] / dt_runoff;
                    wo[ui] = 0.0;
                }
                // H5a: the temperature of that returning water. Goes through
                // addRunonTemperatureAt so the numerator and the rate move
                // together — see HeatData.hpp on why they are a pair.
                auto& ho = ctx_.heat_state.subcatch_outfall_temp_vol;
                if (ctx_.options.heat_transport && ui < ho.size()) {
                    transport::addRunonTemperatureAt(
                        ctx_, i, vol / dt_runoff, ho[ui] / vol);
                    ho[ui] = 0.0;
                }
                // Water re-entering the runoff system from an outfall is new
                // inflow to its mass balance (legacy runoff.c:520
                // RUNOFF_RUNON) — without this, LID or subarea uptake of the
                // routed volume reads as a continuity error (issue #131).
                ctx_.mass_balance.runoff_runon += vol;
                ctx_.subcatches.outfall_runon_vol[ui] = 0.0;
            }
        }
    }
}

// ============================================================================
// assembleLateralInflows — sum decomposed inflow sources into lat_flow
// ============================================================================

void SWMMEngine::assembleLateralInflows(double dt_routing) noexcept {
    // Sum all decomposed inflow source arrays into nodes.lat_flow[].
    // Each source process writes to its own buffer; this is the single
    // assembly point where they are combined for the routing solver.
    // Also computes step mass balance accumulators from component sums.
    double sum_dw   = 0.0;
    double sum_gw   = 0.0;
    double sum_rdii = 0.0;
    double sum_ext  = 0.0;

    for (int j = 0; j < ctx_.n_nodes(); ++j) {
        auto uj = static_cast<std::size_t>(j);

        // Re-derive the 1D↔2D coupling RATE from the exchange VOLUME queued by
        // the 2D co-advance (coupling_queue, 1D ft³), drained at the uniform
        // rate queue / coupling_delivery_remaining. Delivering a VOLUME (not a
        // rate) keeps the exchange conservative under VARIABLE_STEP; draining it
        // over the remaining delivery span (the sync batch it was accumulated
        // over) removes the single-step pulse a volume/dt dump would produce — a
        // batch/routing-step-sized rate spike that floods small junctions
        // instantly and drives a drain/spill churn. The step where
        // remaining ≤ dt flushes the remainder exactly, so the node always
        // receives the full queued volume before the next batch's exchange
        // arrives (jitter leftovers carry over in the queue). When the 2D
        // co-advance spans a single routing step, remaining ≤ dt on the first
        // delivery step and this is byte-for-byte a one-step delivery.
        {
            double q_couple = 0.0;
            if (dt_routing > 0.0 && ctx_.nodes.coupling_queue[uj] != 0.0) {
                if (ctx_.coupling_delivery_remaining > dt_routing) {
                    q_couple = ctx_.nodes.coupling_queue[uj]
                               / ctx_.coupling_delivery_remaining;
                    ctx_.nodes.coupling_queue[uj] -= q_couple * dt_routing;
                } else {
                    q_couple = ctx_.nodes.coupling_queue[uj] / dt_routing;
                    ctx_.nodes.coupling_queue[uj] = 0.0;
                }
            }
            // Decoupled-timestep path (2026-07 plan): the per-step exchange
            // evaluator (computeCouplingExchangeStep) books each routing
            // step's exchange VOLUME into coupling_volume at the END of step
            // N; consume it here at the start of step N+1 by re-deriving the
            // rate against THIS step's dt — the node receives exactly the
            // booked volume regardless of how the timestep changes
            // (VARIABLE_STEP-conservative, the contract documented on
            // NodeData::coupling_volume). Composes with the queue drain above
            // (failure redelivery / live-path window totals).
            if (dt_routing > 0.0 && ctx_.nodes.coupling_volume[uj] != 0.0) {
                q_couple += ctx_.nodes.coupling_volume[uj] / dt_routing;
                ctx_.nodes.coupling_volume[uj] = 0.0;
            }
            ctx_.nodes.coupling_inflow[uj] = q_couple;

            // S3: the 2D→1D species mass queue drains by the SAME rule as the
            // volume queue above (uniform rate over the remaining span, flushed
            // when remaining ≤ dt), so the delivered mass/volume ratio is the
            // cell concentration the marcher booked. Written as a RATE for
            // QualitySolver::addCouplingLoads (sized [node*np+p]; empty when
            // the model carries no pollutants).
            auto drain_queue = [&](double& qm) -> double {
                double rate = 0.0;
                if (dt_routing > 0.0 && qm != 0.0) {
                    if (ctx_.coupling_delivery_remaining > dt_routing) {
                        rate = qm / ctx_.coupling_delivery_remaining;
                        qm  -= rate * dt_routing;
                    } else {
                        rate = qm / dt_routing;
                        qm   = 0.0;
                    }
                }
                return rate;
            };
            if (!ctx_.nodes.coupling_qual_queue.empty()) {
                const auto np = static_cast<std::size_t>(ctx_.n_pollutants());
                for (std::size_t p = 0; p < np; ++p) {
                    const std::size_t idx = uj * np + p;
                    ctx_.nodes.coupling_qual_inflow[idx] =
                        drain_queue(ctx_.nodes.coupling_qual_queue[idx]);
                }
            }
            // S4: the age-volume and temperature-volume halves, same rule.
            if (uj < ctx_.nodes.coupling_age_vol_queue.size())
                ctx_.nodes.coupling_age_vol_inflow[uj] =
                    drain_queue(ctx_.nodes.coupling_age_vol_queue[uj]);
            if (uj < ctx_.nodes.coupling_temp_vol_queue.size())
                ctx_.nodes.coupling_temp_vol_inflow[uj] =
                    drain_queue(ctx_.nodes.coupling_temp_vol_queue[uj]);
        }

        // PARITY: accumulate in legacy routing_execute source ORDER
        // (routing.c:466-473): external → dry-weather → wet-weather (per
        // subcatchment, below) → groundwater (per subcatchment, below) →
        // RDII → iface. Engine-only extensions (user API lateral flow,
        // 2D coupling) are added last; both are 0.0 in legacy-comparable
        // runs. FP addition is not associative, so any other order drifts
        // the lateral inflow by 1 ULP at multi-source nodes.
        ctx_.nodes.lat_flow[uj] = ctx_.nodes.ext_inflow[uj]
                                + ctx_.nodes.dwf_inflow[uj];
        // LID drain-to-node water (legacy lid_addDrainInflow →
        // Node.newLatFlow, booked as EXTERNAL_INFLOW). Added after the
        // legacy pair so legacy-comparable runs (0.0 here) keep their ULPs.
        ctx_.nodes.lat_flow[uj] += ctx_.nodes.lid_drain_inflow[uj];

        sum_dw   += ctx_.nodes.dwf_inflow[uj];
        sum_gw   += ctx_.nodes.gw_inflow[uj];
        sum_rdii += ctx_.nodes.rdii_inflow[uj];
        // Interface file inflows count as external inflow for continuity
        // (legacy addIfaceInflows → massbal_addInflowFlow(EXTERNAL_INFLOW, q))
        sum_ext  += ctx_.nodes.ext_inflow[uj]
                  + ctx_.nodes.iface_inflow[uj]
                  + ctx_.nodes.lid_drain_inflow[uj];

        // 2D → 1D coupling (positive coupling_inflow) folds into the
        // routing_external category for continuity reporting; the negative
        // side (1D → 2D spill) is accumulated separately in
        // updateRoutingMassBalance as routing_flooding. See review §11.
        if (ctx_.nodes.coupling_inflow[uj] > 0.0)
            sum_ext += ctx_.nodes.coupling_inflow[uj];
    }

    // One tick of the coupling delivery window per routing step (NOT per
    // node). Clamped at 0; stays 0 until the next 2D advance queues volumes.
    if (ctx_.coupling_delivery_remaining > 0.0)
        ctx_.coupling_delivery_remaining =
            std::max(0.0, ctx_.coupling_delivery_remaining - dt_routing);

    // Wet-weather runoff: per subcatchment in index order, exactly like
    // legacy addWetWeatherInflows (routing.c:709-717).
    {
        const int n_sub = ctx_.n_subcatches();
        const int n_nodes = ctx_.n_nodes();
        if (static_cast<int>(wet_q_interp_.size()) == n_sub) {
            for (int i = 0; i < n_sub; ++i) {
                auto ui = static_cast<std::size_t>(i);
                int out_node = ctx_.subcatches.outlet_node[ui];
                if (out_node >= 0 && out_node < n_nodes)
                    ctx_.nodes.lat_flow[static_cast<std::size_t>(out_node)]
                        += wet_q_interp_[ui];
            }
        }
        // Groundwater: per subcatchment, matching legacy addGroundwaterInflows.
        if (static_cast<int>(gw_q_interp_.size()) == n_sub) {
            for (int i = 0; i < n_sub; ++i) {
                auto ui = static_cast<std::size_t>(i);
                int gwn = gw_q_node_[ui];
                if (gwn >= 0 && gwn < n_nodes)
                    ctx_.nodes.lat_flow[static_cast<std::size_t>(gwn)]
                        += gw_q_interp_[ui];
            }
        }
    }

    // RDII, then iface, then engine-only extensions — legacy source order.
    for (int j = 0; j < ctx_.n_nodes(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        ctx_.nodes.lat_flow[uj] += ctx_.nodes.rdii_inflow[uj];
        ctx_.nodes.lat_flow[uj] += ctx_.nodes.iface_inflow[uj];

        // Runtime-forced lateral inflow: persistent API value with any
        // active ForcingData forcing overlaid (see effectiveUserLatFlow).
        // Counts as external inflow for continuity, matching legacy
        // addExternalInflows (routing.c: apiExtInflow →
        // massbal_addInflowFlow(EXTERNAL_INFLOW, q), positive only).
        // Without this the routed volume appears in total_out but never in
        // total_in and the continuity error grows unboundedly negative. The
        // cumulative routing_forcing_inflow diagnostic (a subset of
        // routing_external) is accumulated in updateRoutingMassBalance.
        // Issue #113.
        const double q_user = effectiveUserLatFlow(ctx_, uj);
        if (q_user > 0.0)
            sum_ext += q_user;

        ctx_.nodes.lat_flow[uj] += q_user
                                 + ctx_.nodes.coupling_inflow[uj];
    }

    ctx_.mass_balance.step_dw_inflow   = sum_dw;
    ctx_.mass_balance.step_gw_inflow   = sum_gw;
    ctx_.mass_balance.step_rdii_inflow = sum_rdii;
    ctx_.mass_balance.step_ext_inflow  = sum_ext;
}

// ============================================================================
// isBetweenEvents — check if routing should be skipped between event windows
// Matches legacy routing.c:isBetweenEvents()
// ============================================================================

bool SWMMEngine::isBetweenEvents(double current_date) const {
    if (ctx_.events.empty()) return false;
    if (next_event_ >= static_cast<int>(ctx_.events.size())) return true;

    const auto& ev = ctx_.events[static_cast<size_t>(next_event_)];

    // Past current event → between events
    if (current_date > ev.end) return true;

    // Inside current event → not between
    if (current_date >= ev.start) return false;

    // Before current event → between events
    return true;
}

// ============================================================================
// isInSteadyState — check if routing can be skipped (steady conditions)
// Matches legacy routing.c:isInSteadyState() + inflowHasChanged()
// ============================================================================

bool SWMMEngine::isInSteadyState(int action_count) const {
    if (!ctx_.options.skip_steady_state) return false;
    if (ctx_.current_time == 0.0) return false;
    if (action_count > 0) return false;

    // Check flow error exceeds tolerance
    double flow_error = std::abs(ctx_.mass_balance.routing_error());
    if (flow_error > ctx_.options.sys_flow_tol) return false;

    // Check if any lateral inflow has changed significantly
    constexpr double TINY = 1e-6;
    for (int j = 0; j < ctx_.n_nodes(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        double qOld = ctx_.nodes.old_lat_flow[uj];
        double qNew = ctx_.nodes.lat_flow[uj];
        double diff;
        if (std::abs(qOld) > TINY)      diff = (qNew / qOld) - 1.0;
        else if (std::abs(qNew) > TINY)  diff = 1.0;
        else                              diff = 0.0;
        if (std::abs(diff) > ctx_.options.lat_flow_tol) return false;
    }
    return true;
}

double SWMMEngine::reportedNodeVolume(int i) const noexcept {
    auto ui = static_cast<std::size_t>(i);
    return reportedNodeVolume(i, ctx_.nodes.depth[ui], ctx_.nodes.volume[ui]);
}

double SWMMEngine::reportedNodeVolume(int i, double depth,
                                      double volume) const noexcept {
    auto ui = static_cast<std::size_t>(i);
    if (ctx_.nodes.type[ui] == NodeType::STORAGE)
        return volume;                         // storage curve volume (= legacy)
    double fd = ctx_.nodes.full_depth[ui];
    if (!(fd > 0.0)) return 0.0;

    // A PONDING node holds real water above its rim, and both solvers write
    // that volume directly rather than deriving it from depth (DW
    // getFloodedDepth, FV applyNodeCapacity). Reporting the rim relation
    // instead dropped the entire pond out of Final Stored Volume, and the
    // ponded inflow then read as a continuity error — 27 % under FV and 83 %
    // under DW on a two-hour single-junction pond. Reporting the volume is also
    // what keeps the flooding term out of the mass balance for these nodes:
    // updateRoutingMassBalance books overflow as a loss only while the volume
    // is at or below full, which is exactly the ponded/not-ponded distinction.
    if (ctx_.options.allow_ponding && ctx_.nodes.ponded_area[ui] > 0.0 &&
        depth > fd)
        return volume;

    // Legacy convention: a plain junction contributes ZERO to reported
    // storage, because report_full_volume_ is 0 for it. FV junctions are now
    // algebraic INTERFACES that hold no water of their own — the water at a
    // junction's head stands in the incident cells, already counted through
    // link volumes — so FV shares the convention. (The earlier bucket model
    // DID hold MIN_SURFAREA·depth of real water per junction and reported it
    // here; keeping that relation after the buckets were removed re-counted
    // the cells' water and read as a continuity error proportional to
    // junction count — measured −0.005 % per junction on a 120-junction
    // chain, one MIN_SURFAREA·depth per node.)

    return report_full_volume_[ui] * (depth / fd);
}

void SWMMEngine::initMassBalance() noexcept {
    // 14. Mass balance: record initial storage (nodes + links, matching legacy)
    ctx_.mass_balance.reset();
    ctx_.negsrc.reset();  // D-NS1 (X6): clamp counters share the lifecycle
    for (int j = 0; j < ctx_.n_nodes(); ++j) {
        // Legacy-convention node volume (junctions => 0) so init storage matches
        // legacy; the internal volume-state is unchanged.
        ctx_.mass_balance.routing_init_storage += reportedNodeVolume(j);
    }
    for (int j = 0; j < ctx_.n_links(); ++j) {
        ctx_.mass_balance.routing_init_storage +=
            ctx_.links.volume[static_cast<std::size_t>(j)];
    }

    // Initial stored pollutant mass, from the concentrations initQuality()
    // seeded out of [POLLUTANTS] Cinit. Legacy: massbal_open() sums
    // Node[j].newQual[p] * Node[j].newVolume + Link[j].newQual[p] * link volume
    // into QualTotals[p].initStorage.
    {
        const int np = ctx_.n_pollutants();
        for (int p = 0; p < np; ++p) {
            const auto up = static_cast<std::size_t>(p);
            if (up >= ctx_.mass_balance.qual_routing_init.size()) break;
            double m = 0.0;
            for (int j = 0; j < ctx_.n_nodes(); ++j) {
                const auto idx = static_cast<std::size_t>(j) *
                                 static_cast<std::size_t>(np) + up;
                if (idx < ctx_.nodes.conc.size())
                    m += ctx_.nodes.conc[idx] * reportedNodeVolume(j);
            }
            for (int j = 0; j < ctx_.n_links(); ++j) {
                const auto uj  = static_cast<std::size_t>(j);
                const auto idx = uj * static_cast<std::size_t>(np) + up;
                if (idx < ctx_.links.conc.size())
                    m += ctx_.links.conc[idx] * ctx_.links.volume[uj];
            }
            ctx_.mass_balance.qual_routing_init[up] = m;
        }
    }

    // Record initial runoff storage
    for (int j = 0; j < ctx_.n_subcatches(); ++j) {
        // Initial surface storage approximated from initial depth * area
        auto uj = static_cast<std::size_t>(j);
        ctx_.mass_balance.runoff_init_store +=
            ctx_.subcatches.ponded_depth[uj] * ctx_.subcatches.area[uj];
    }
    // Initial water in LID units (InitSat + wilting-point soil moisture) is
    // part of runoff storage, mirroring the final-storage accounting.
    ctx_.mass_balance.runoff_init_store += lid_.storedVolume();

    // F8 — the pack the deck starts with is water the model was GIVEN, and
    // the ledger had no row for it. Legacy massbal.c:124-129.
    ctx_.mass_balance.runoff_init_snow =
        snowCoverVolumeFt3(ctx_, snow_.state());

    // Record initial groundwater storage
    // Legacy: GwaterTotals.initStorage += gwater_getVolume(j) * Subcatch[j].area
    // gwater_getVolume = theta * upperDepth + porosity * lowerDepth
    {
        auto& gw = groundwater_.state();
        for (int i = 0; i < ctx_.n_subcatches(); ++i) {
            auto ui = static_cast<std::size_t>(i);
            if (gw.total_depth[ui] <= 0.0) continue;
            double upper_d = gw.total_depth[ui] - gw.lower_depth[ui];
            double vol = gw.theta[ui] * upper_d + gw.porosity[ui] * gw.lower_depth[ui];
            double area = ctx_.subcatches.area[ui] * ucf::ACRES_TO_FT2;
            ctx_.mass_balance.gw_init_storage += vol * area;
        }
    }
}

} /* namespace openswmm */
