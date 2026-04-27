/**
 * @file openswmm_hotstart_impl.cpp
 * @brief Hot start C API implementation — thin wrappers around HotStartManager.
 *
 * @details Every function declared in
 *          `include/openswmm/engine/openswmm_hotstart.h` is implemented here.
 *
 *          Pattern:
 *            1. Validate handle (non-null, correct type).
 *            2. Cast opaque `void*` → `HotStartFile*` or `SWMMEngine*`.
 *            3. Delegate to `HotStartManager` static methods.
 *            4. Return `SWMM_OK` or an error code.
 *
 * @note Must be compiled as C++ (pulls in C++ headers), but exported symbols
 *       use C linkage via `extern "C"`.
 *
 * @see include/openswmm/engine/openswmm_hotstart.h
 * @see HotStartManager.hpp
 * @see SWMMEngine.hpp
 * @ingroup engine_hotstart
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "SWMMEngine.hpp"
#include "HotStartManager.hpp"
#include "../plugins/PluginFactory.hpp"
#include "../../../include/openswmm/engine/openswmm_hotstart.h"
#include "../../../include/openswmm/plugin_sdk/IStateIOPlugin.hpp"

#include <cstring>

// ============================================================================
// Internal cast helpers
// ============================================================================

static inline openswmm::SWMMEngine*  to_engine(SWMM_Engine e) noexcept {
    return static_cast<openswmm::SWMMEngine*>(e);
}

static inline openswmm::HotStartFile* to_hs(SWMM_HotStart hs) noexcept {
    return static_cast<openswmm::HotStartFile*>(hs);
}

#define CHECK_ENGINE(e)  do { if (!(e)) return SWMM_ERR_BADHANDLE; } while(0)
#define CHECK_HS(hs)     do { if (!(hs)) return SWMM_ERR_BADHANDLE; } while(0)

// ============================================================================
// C API implementation
// ============================================================================

extern "C" {

// ----------------------------------------------------------------------------
// swmm_hotstart_save()
// ----------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_hotstart_save(SWMM_Engine engine, const char* path) {
    CHECK_ENGINE(engine);
    if (!path || path[0] == '\0') return SWMM_ERR_BADPARAM;

    openswmm::SWMMEngine* eng = to_engine(engine);
    openswmm::SimulationContext& ctx = eng->context();

    // Must be RUNNING or ENDED to save
    if (ctx.state != openswmm::EngineState::RUNNING &&
        ctx.state != openswmm::EngineState::ENDED) {
        return SWMM_ERR_LIFECYCLE;
    }

    // Dispatch through state-IO plugins. The first plugin whose write_state()
    // succeeds wins; if none is registered (shouldn't happen — SWMMEngine
    // injects DefaultStateIOPlugin in open()), fall back to HotStartManager
    // directly so this path remains usable in legacy embeddings.
    const auto& state_plugins = eng->plugin_factory().state_io_plugins();
    if (!state_plugins.empty()) {
        for (auto* sp : state_plugins) {
            const int rc = sp->write_state(path, ctx);
            if (rc == 0) return SWMM_OK;
        }
        return SWMM_ERR_HOTSTART;
    }

    // Fallback (no plugin registered): direct HotStartManager call.
    openswmm::HotStartFile* hs =
        openswmm::HotStartManager::save(ctx,
                                        &eng->runoff_solver(),
                                        &eng->gw_solver(),
                                        path);

    if (!hs) return SWMM_ERR_HOTSTART;
    delete hs;
    return SWMM_OK;
}

// ----------------------------------------------------------------------------
// swmm_hotstart_open()
// ----------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_hotstart_open(const char* path, SWMM_HotStart* hs) {
    if (!hs) return SWMM_ERR_BADPARAM;
    *hs = nullptr;
    if (!path || path[0] == '\0') return SWMM_ERR_BADPARAM;

    openswmm::HotStartFile* file = openswmm::HotStartManager::open(path);
    if (!file) return SWMM_ERR_HOTSTART;

    *hs = static_cast<SWMM_HotStart>(file);
    return SWMM_OK;
}

// ----------------------------------------------------------------------------
// swmm_hotstart_apply()
// ----------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_hotstart_apply(SWMM_Engine engine, SWMM_HotStart hs) {
    CHECK_ENGINE(engine);
    CHECK_HS(hs);

    openswmm::SWMMEngine* eng = to_engine(engine);
    openswmm::SimulationContext& ctx = eng->context();

    if (ctx.state != openswmm::EngineState::INITIALIZED) {
        return SWMM_ERR_LIFECYCLE;
    }

    openswmm::HotStartFile* file = to_hs(hs);

    // Gap #54: apply V2 format including infiltration + GW state
    openswmm::HotStartManager::apply(
        *file,
        ctx,
        &eng->runoff_solver(),
        &eng->gw_solver(),
        [&eng](const std::string& msg) {
            (void)eng;
            (void)msg;
        }
    );

    return SWMM_OK;
}

// ----------------------------------------------------------------------------
// Modification API
// ----------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_hotstart_set_node_depth(
    SWMM_HotStart hs, const char* node_id, double depth)
{
    CHECK_HS(hs);
    if (!node_id) return SWMM_ERR_BADPARAM;
    return to_hs(hs)->set_node_depth(node_id, depth)
               ? SWMM_OK : SWMM_ERR_BADPARAM;
}

SWMM_ENGINE_API int swmm_hotstart_set_node_head(
    SWMM_HotStart hs, const char* node_id, double head)
{
    CHECK_HS(hs);
    if (!node_id) return SWMM_ERR_BADPARAM;
    return to_hs(hs)->set_node_head(node_id, head)
               ? SWMM_OK : SWMM_ERR_BADPARAM;
}

SWMM_ENGINE_API int swmm_hotstart_set_link_flow(
    SWMM_HotStart hs, const char* link_id, double flow)
{
    CHECK_HS(hs);
    if (!link_id) return SWMM_ERR_BADPARAM;
    return to_hs(hs)->set_link_flow(link_id, flow)
               ? SWMM_OK : SWMM_ERR_BADPARAM;
}

SWMM_ENGINE_API int swmm_hotstart_set_link_depth(
    SWMM_HotStart hs, const char* link_id, double depth)
{
    CHECK_HS(hs);
    if (!link_id) return SWMM_ERR_BADPARAM;
    return to_hs(hs)->set_link_depth(link_id, depth)
               ? SWMM_OK : SWMM_ERR_BADPARAM;
}

SWMM_ENGINE_API int swmm_hotstart_set_subcatch_runoff(
    SWMM_HotStart hs, const char* subcatch_id, double runoff)
{
    CHECK_HS(hs);
    if (!subcatch_id) return SWMM_ERR_BADPARAM;
    return to_hs(hs)->set_subcatch_runoff(subcatch_id, runoff)
               ? SWMM_OK : SWMM_ERR_BADPARAM;
}

// ----------------------------------------------------------------------------
// Query metadata
// ----------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_hotstart_get_sim_time(SWMM_HotStart hs, double* sim_time) {
    CHECK_HS(hs);
    if (!sim_time) return SWMM_ERR_BADPARAM;
    *sim_time = to_hs(hs)->header.sim_time;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_hotstart_get_crs(SWMM_HotStart hs, char* buf, int buflen) {
    CHECK_HS(hs);
    if (!buf || buflen <= 0) return SWMM_ERR_BADPARAM;
    const std::string& crs = to_hs(hs)->header.crs;
    const int copy_len = static_cast<int>(crs.size()) < buflen - 1
                             ? static_cast<int>(crs.size()) : buflen - 1;
    std::memcpy(buf, crs.c_str(), static_cast<std::size_t>(copy_len));
    buf[copy_len] = '\0';
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_hotstart_node_count(SWMM_HotStart hs) {
    if (!hs) return -1;
    return static_cast<int>(to_hs(hs)->nodes.size());
}

SWMM_ENGINE_API int swmm_hotstart_link_count(SWMM_HotStart hs) {
    if (!hs) return -1;
    return static_cast<int>(to_hs(hs)->links.size());
}

// ----------------------------------------------------------------------------
// Warning access
// ----------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_hotstart_warning_count(SWMM_HotStart hs) {
    if (!hs) return 0;
    return static_cast<int>(to_hs(hs)->warnings.size());
}

SWMM_ENGINE_API const char* swmm_hotstart_warning(SWMM_HotStart hs, int index) {
    if (!hs) return nullptr;
    const auto& w = to_hs(hs)->warnings;
    if (index < 0 || static_cast<std::size_t>(index) >= w.size()) return nullptr;
    return w[static_cast<std::size_t>(index)].c_str();
}

// ----------------------------------------------------------------------------
// Close
// ----------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_hotstart_close(SWMM_HotStart hs) {
    if (!hs) return SWMM_OK; // safe no-op on null

    openswmm::HotStartFile* file = to_hs(hs);

    // Flush modifications if dirty
    if (file->dirty) {
        if (!openswmm::HotStartManager::flush(*file)) {
            delete file;
            return SWMM_ERR_IO;
        }
    }

    delete file;
    return SWMM_OK;
}

} /* extern "C" */
