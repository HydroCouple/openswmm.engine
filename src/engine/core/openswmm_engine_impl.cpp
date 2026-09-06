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
 * @file openswmm_engine_impl.cpp
 * @brief C API implementation — engine lifecycle, callbacks, errors, timing.
 *
 * @see include/openswmm/engine/openswmm_engine.h
 * @ingroup engine_api
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "openswmm_api_common.hpp"
#include "ThreadInfo.hpp"

extern "C" {

// ============================================================================
// Lifecycle
// ============================================================================

SWMM_ENGINE_API SWMM_Engine swmm_engine_create(void) {
    try {
        return static_cast<SWMM_Engine>(new openswmm::SWMMEngine());
    } catch (...) {
        return nullptr;
    }
}

SWMM_ENGINE_API int swmm_engine_open(SWMM_Engine engine,
                                      const char* inp, const char* rpt, const char* out,
                                      const char* input_plugin_lib) {
    CHECK_HANDLE(engine);
    return to_engine(engine)->open(inp, rpt, out, input_plugin_lib);
}

SWMM_ENGINE_API int swmm_engine_initialize(SWMM_Engine engine) {
    CHECK_HANDLE(engine);
    return to_engine(engine)->initialize();
}

SWMM_ENGINE_API int swmm_engine_start(SWMM_Engine engine, int save_results) {
    CHECK_HANDLE(engine);
    return to_engine(engine)->start(save_results);
}

SWMM_ENGINE_API int swmm_engine_step(SWMM_Engine engine, double* elapsed_time) {
    CHECK_HANDLE(engine);
    return to_engine(engine)->step(elapsed_time);
}

// Gap #50: swmm_stride equivalent — advance up to n_steps routing steps.
SWMM_ENGINE_API int swmm_engine_stride(SWMM_Engine engine, int n_steps, double* elapsed_time) {
    CHECK_HANDLE(engine);
    if (elapsed_time) *elapsed_time = 0.0;
    if (n_steps <= 0) return SWMM_OK;

    double t = 0.0;
    int err = SWMM_OK;
    for (int s = 0; s < n_steps; ++s) {
        err = to_engine(engine)->step(&t);
        if (elapsed_time) *elapsed_time = t;
        if (err != SWMM_OK || t <= 0.0) break;
    }
    return err;
}

SWMM_ENGINE_API int swmm_engine_end(SWMM_Engine engine) {
    CHECK_HANDLE(engine);
    return to_engine(engine)->end();
}

SWMM_ENGINE_API int swmm_engine_report(SWMM_Engine engine) {
    CHECK_HANDLE(engine);
    return to_engine(engine)->report();
}

SWMM_ENGINE_API int swmm_engine_close(SWMM_Engine engine) {
    CHECK_HANDLE(engine);
    return to_engine(engine)->close();
}

SWMM_ENGINE_API void swmm_engine_destroy(SWMM_Engine engine) {
    delete to_engine(engine);
}

// Translate the C++ internal openswmm::EngineState (an implementation detail
// that may carry sub-states like PAUSED/REPORTED/ERROR_STATE) to the public
// SWMM_EngineState lifecycle exposed across the C ABI.
static SWMM_EngineState swmm_engine_state_to_public(openswmm::EngineState s) noexcept {
    using S = openswmm::EngineState;
    switch (s) {
        case S::CREATED:     return SWMM_STATE_CREATED;
        case S::OPENED:      return SWMM_STATE_OPENED;
        case S::INITIALIZED: return SWMM_STATE_INITIALIZED;
        case S::RUNNING:     return SWMM_STATE_RUNNING;
        case S::PAUSED:      return SWMM_STATE_RUNNING;
        case S::ENDED:       return SWMM_STATE_ENDED;
        case S::REPORTED:    return SWMM_STATE_ENDED;
        case S::CLOSED:      return SWMM_STATE_CLOSED;
        case S::ERROR_STATE: return SWMM_STATE_NONE;
        case S::BUILDING:    return SWMM_STATE_BUILDING;
    }
    return SWMM_STATE_NONE;
}

SWMM_ENGINE_API int swmm_engine_get_state(SWMM_Engine engine, int* state) {
    CHECK_HANDLE(engine);
    if (state) {
        *state = static_cast<int>(
            swmm_engine_state_to_public(to_engine(engine)->context().state));
    }
    return SWMM_OK;
}

// ============================================================================
// Convenience run functions
// ============================================================================

SWMM_ENGINE_API int swmm_engine_run_with_callback(
    const char* inp, const char* rpt, const char* out,
    const char* input_plugin_lib,
    SWMM_ProgressCallback callback, void* user_data)
{
    SWMM_Engine e = swmm_engine_create();
    if (!e) return SWMM_ERR_NOMEM;

    int err = SWMM_OK;

    // Register progress callback before running
    if (callback) {
        swmm_set_progress_callback(e, callback, user_data);
    }

    err = swmm_engine_open(e, inp, rpt, out, input_plugin_lib);
    if (err != SWMM_OK) { swmm_engine_destroy(e); return err; }

    err = swmm_engine_initialize(e);
    if (err != SWMM_OK) { swmm_engine_close(e); swmm_engine_destroy(e); return err; }

    err = swmm_engine_start(e, 1);
    if (err != SWMM_OK) { swmm_engine_close(e); swmm_engine_destroy(e); return err; }

    double elapsed = 0.0;
    do {
        err = swmm_engine_step(e, &elapsed);
    } while (elapsed > 0.0 && err == SWMM_OK);

    swmm_engine_end(e);
    swmm_engine_report(e);
    swmm_engine_close(e);

    if (err == SWMM_OK) err = swmm_get_last_error(e);

    swmm_engine_destroy(e);
    return err;
}

SWMM_ENGINE_API int swmm_engine_run(const char* inp, const char* rpt, const char* out,
                                     const char* input_plugin_lib) {
    return swmm_engine_run_with_callback(inp, rpt, out, input_plugin_lib, nullptr, nullptr);
}

// ============================================================================
// Callback registration
// ============================================================================

SWMM_ENGINE_API int swmm_set_progress_callback(SWMM_Engine engine,
                                                 SWMM_ProgressCallback callback, void* user_data) {
    CHECK_HANDLE(engine);
    to_engine(engine)->set_progress_callback(callback, user_data);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_set_warning_callback(SWMM_Engine engine,
                                               SWMM_WarningCallback callback, void* user_data) {
    CHECK_HANDLE(engine);
    to_engine(engine)->set_warning_callback(callback, user_data);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_set_step_begin_callback(SWMM_Engine engine,
                                                   SWMM_StepBeginCallback callback, void* user_data) {
    CHECK_HANDLE(engine);
    to_engine(engine)->set_step_begin_callback(callback, user_data);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_set_step_end_callback(SWMM_Engine engine,
                                                 SWMM_StepEndCallback callback, void* user_data) {
    CHECK_HANDLE(engine);
    to_engine(engine)->set_step_end_callback(callback, user_data);
    return SWMM_OK;
}

// ============================================================================
// Error reporting
// ============================================================================

SWMM_ENGINE_API int swmm_get_last_error(SWMM_Engine engine) {
    if (!engine) return SWMM_ERR_BADHANDLE;
    return to_engine(engine)->last_error();
}

SWMM_ENGINE_API const char* swmm_get_last_error_msg(SWMM_Engine engine) {
    if (!engine) return "invalid engine handle";
    return to_engine(engine)->last_error_message();
}

SWMM_ENGINE_API void swmm_engine_set_lenient_open(SWMM_Engine engine, int on) {
    if (!engine) return;
    to_engine(engine)->set_lenient_open(on != 0);
}

SWMM_ENGINE_API int swmm_get_error_count(SWMM_Engine engine) {
    if (!engine) return 0;
    return static_cast<int>(to_engine(engine)->context().errors.size());
}

SWMM_ENGINE_API const char* swmm_get_error_at(SWMM_Engine engine, int index) {
    if (!engine) return "";
    const auto& errs = to_engine(engine)->context().errors;
    if (index < 0 || index >= static_cast<int>(errs.size())) return "";
    return errs[static_cast<std::size_t>(index)].c_str();
}

SWMM_ENGINE_API int swmm_get_warning_count(SWMM_Engine engine) {
    if (!engine) return 0;
    return static_cast<int>(to_engine(engine)->context().warnings.size());
}

SWMM_ENGINE_API const char* swmm_get_warning_at(SWMM_Engine engine, int index) {
    if (!engine) return "";
    const auto& warns = to_engine(engine)->context().warnings;
    if (index < 0 || index >= static_cast<int>(warns.size())) return "";
    return warns[static_cast<std::size_t>(index)].c_str();
}

// ============================================================================
// Thread capability query
// ============================================================================

SWMM_ENGINE_API int swmm_get_thread_info(SWMM_ThreadInfo* out) {
    if (!out) return SWMM_ERR_BADPARAM;
    namespace ti = openswmm::threadinfo;
    out->logical_cpus       = ti::logicalCpus();
    out->omp_max_threads    = ti::ompMaxThreads();
    out->omp_available      = ti::ompAvailable();
    out->perf_cores         = ti::perfCores();
    out->kokkos_omp_threads = ti::kokkosOmpThreads();
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_get_effective_threads(SWMM_Engine engine, int threads_option,
                                               int* global_threads, int* dw_threads,
                                               int* twod_threads) {
    if (!engine) return SWMM_ERR_BADPARAM;
    namespace ti = openswmm::threadinfo;
    const auto& ctx = to_engine(engine)->context();

    if (global_threads)
        *global_threads = ti::resolveRequested(threads_option, "OpenMP team", nullptr);

    if (dw_threads) {
        *dw_threads = 0;
        if (ctx.options.routing_model == openswmm::RoutingModel::DYNWAVE) {
            int n_conduits = 0;
            for (const auto t : ctx.links.type)
                if (t == openswmm::LinkType::CONDUIT) ++n_conduits;
            *dw_threads = ti::dwThreads(threads_option, n_conduits, nullptr);
        }
    }

    if (twod_threads) {
        *twod_threads = 0;
#ifdef OPENSWMM_HAS_2D
        const int n_tri = to_engine(engine)->surfaceRouter2D().mesh().n_triangles();
        if (n_tri > 0 && !ctx.options.ignore_2d)
            *twod_threads = ti::twoDThreads(threads_option, n_tri, nullptr);
#endif
    }
    return SWMM_OK;
}

SWMM_ENGINE_API const char* swmm_error_message(int code) {
    switch (code) {
        case SWMM_OK:            return "Success";
        case SWMM_ERR_NOMEM:     return "Out of memory";
        case SWMM_ERR_INPFILE:   return "Cannot open input file";
        case SWMM_ERR_RPTFILE:   return "Cannot open report file";
        case SWMM_ERR_OUTFILE:   return "Cannot open output file";
        case SWMM_ERR_PARSE:     return "Input file parse error";
        case SWMM_ERR_LIFECYCLE: return "Function called in wrong lifecycle state";
        case SWMM_ERR_BADHANDLE: return "NULL or invalid engine handle";
        case SWMM_ERR_BADINDEX:  return "Object index out of range";
        case SWMM_ERR_BADPARAM:  return "Invalid parameter value";
        case SWMM_ERR_PLUGIN:    return "Plugin load or lifecycle error";
        case SWMM_ERR_IO:        return "IO thread error";
        case SWMM_ERR_HOTSTART:  return "Hot start file error";
        case SWMM_ERR_CRS:       return "Coordinate reference system error";
        case SWMM_ERR_NUMERICAL: return "Numerical instability (fatal)";
        case SWMM_ERR_INTERNAL:  return "Internal engine error";
        // Virtual-junction rule codes (ErrorCodes.hpp ERR_VJ_*)
        case 609: return "Virtual junction must connect exactly two conduits";
        case 611: return "Virtual junction connects conduits with different cross sections";
        case 613: return "Virtual junction has a conduit with a nonzero offset";
        case 615: return "Virtual junction conduit inverts do not agree at the node";
        case 617: return "Virtual junction cannot receive lateral inflow";
        case 619: return "Virtual junctions require DYNWAVE flow routing";
        case 621: return "Too many items for virtual junction";
        default:                 return "Unknown error";
    }
}

// ============================================================================
// Simulation timing
// ============================================================================

SWMM_ENGINE_API int swmm_get_start_time(SWMM_Engine engine, double* start) {
    CHECK_HANDLE(engine);
    if (start) *start = to_engine(engine)->context().options.start_date;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_get_end_time(SWMM_Engine engine, double* end) {
    CHECK_HANDLE(engine);
    if (end) *end = to_engine(engine)->context().options.end_date;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_get_current_time(SWMM_Engine engine, double* current) {
    CHECK_HANDLE(engine);
    if (current) *current = to_engine(engine)->context().current_time;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_get_routing_step(SWMM_Engine engine, double* dt) {
    CHECK_HANDLE(engine);
    if (dt) *dt = to_engine(engine)->context().options.routing_step;
    return SWMM_OK;
}

// ============================================================================
// Unit system query
// ============================================================================
// The C API getters/setters exchange values in the project's DISPLAY units
// (selected by [OPTIONS] FLOW_UNITS); these accessors tell callers which units
// those are. swmm_options_get(engine, "FLOW_UNITS", ...) returns the same
// information as a string — these are the typed convenience equivalents for
// bindings and coupling code that need to convert without string parsing.

SWMM_ENGINE_API int swmm_get_flow_units(SWMM_Engine engine, int* flow_units) {
    CHECK_HANDLE(engine);
    if (flow_units)
        *flow_units = static_cast<int>(to_engine(engine)->context().options.flow_units);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_get_unit_system(SWMM_Engine engine, int* unit_system) {
    CHECK_HANDLE(engine);
    if (unit_system) {
        const int fu = static_cast<int>(to_engine(engine)->context().options.flow_units);
        *unit_system = openswmm::ucf::getUnitSystem(fu);  // 0 = US, 1 = SI
    }
    return SWMM_OK;
}

// ============================================================================
// Routing event and steady-state status
// ============================================================================

SWMM_ENGINE_API int swmm_is_between_events(SWMM_Engine engine, int* is_between) {
    CHECK_HANDLE(engine);
    auto* eng = to_engine(engine);
    if (is_between) {
        const auto& events = eng->context().events;
        *is_between = (!events.empty()) ? 1 : 0;
        // Delegate to engine method if it exists; for now use event list check
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_get_event_count(SWMM_Engine engine, int* count) {
    CHECK_HANDLE(engine);
    if (count) *count = static_cast<int>(to_engine(engine)->context().events.size());
    return SWMM_OK;
}

/* -------------------------------------------------------------------------
 * [EVENTS] section accessors (Slice CW — added 2026-05-21).
 * Mirrors the read path in handle_events (InfraHandler.cpp).
 * ------------------------------------------------------------------------- */

SWMM_ENGINE_API int swmm_events_count(SWMM_Engine engine, int* count) {
    return swmm_get_event_count(engine, count);
}

SWMM_ENGINE_API int swmm_events_get(SWMM_Engine engine, int idx,
                                    double* start, double* end) {
    CHECK_HANDLE(engine);
    const auto& events = to_engine(engine)->context().events;
    if (idx < 0 || static_cast<size_t>(idx) >= events.size())
        return SWMM_ERR_BADINDEX;
    if (start) *start = events[static_cast<size_t>(idx)].start;
    if (end)   *end   = events[static_cast<size_t>(idx)].end;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_events_set(SWMM_Engine engine, int idx,
                                    double start, double end) {
    CHECK_HANDLE(engine);
    auto& events = to_engine(engine)->context().events;
    if (idx < 0 || static_cast<size_t>(idx) >= events.size())
        return SWMM_ERR_BADINDEX;
    if (start >= end) return SWMM_ERR_BADPARAM;
    events[static_cast<size_t>(idx)].start = start;
    events[static_cast<size_t>(idx)].end   = end;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_events_add(SWMM_Engine engine,
                                    double start, double end, int* out_idx) {
    CHECK_HANDLE(engine);
    if (start >= end) return SWMM_ERR_BADPARAM;
    auto& events = to_engine(engine)->context().events;
    events.push_back({start, end});
    if (out_idx) *out_idx = static_cast<int>(events.size()) - 1;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_events_remove(SWMM_Engine engine, int idx) {
    CHECK_HANDLE(engine);
    auto& events = to_engine(engine)->context().events;
    if (idx < 0 || static_cast<size_t>(idx) >= events.size())
        return SWMM_ERR_BADINDEX;
    events.erase(events.begin() + idx);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_events_clear(SWMM_Engine engine) {
    CHECK_HANDLE(engine);
    to_engine(engine)->context().events.clear();
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_get_steady_state_skip(SWMM_Engine engine, int* enabled) {
    CHECK_HANDLE(engine);
    if (enabled) *enabled = to_engine(engine)->context().options.skip_steady_state ? 1 : 0;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_set_steady_state_skip(SWMM_Engine engine, int enabled) {
    CHECK_HANDLE(engine);
    to_engine(engine)->context().options.skip_steady_state = (enabled != 0);
    return SWMM_OK;
}

// ============================================================================
// Phase 1b: Runoff interface file (legacy "Frunoff")
// ============================================================================
//
// Thin pass-throughs to SWMMEngine's runoff-iface methods.  The non-trivial
// work (header validation, per-substep auto-save) lives in SWMMEngine.cpp;
// here we just translate between the public C ABI and the C++ helpers.

SWMM_ENGINE_API int swmm_runoff_iface_open_write(SWMM_Engine engine, const char* path) {
    CHECK_HANDLE(engine);
    if (!path || path[0] == '\0') return SWMM_ERR_BADPARAM;
    return to_engine(engine)->openRunoffIfaceWrite(path);
}

SWMM_ENGINE_API int swmm_runoff_iface_open_read(SWMM_Engine engine, const char* path) {
    CHECK_HANDLE(engine);
    if (!path || path[0] == '\0') return SWMM_ERR_BADPARAM;
    return to_engine(engine)->openRunoffIfaceRead(path);
}

SWMM_ENGINE_API int swmm_runoff_iface_save_step(SWMM_Engine engine, double dt) {
    CHECK_HANDLE(engine);
    to_engine(engine)->saveRunoffIfaceStep(dt);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_runoff_iface_read_step(SWMM_Engine engine, int* has_data) {
    CHECK_HANDLE(engine);
    const bool ok = to_engine(engine)->readRunoffIfaceStep();
    if (has_data) *has_data = ok ? 1 : 0;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_runoff_iface_close(SWMM_Engine engine) {
    CHECK_HANDLE(engine);
    to_engine(engine)->closeRunoffIface();
    return SWMM_OK;
}

} /* extern "C" */
