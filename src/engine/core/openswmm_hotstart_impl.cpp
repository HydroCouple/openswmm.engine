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
 * @file      openswmm_hotstart_impl.cpp
 * @brief     Hot start C API implementation — thin wrappers around HotStartManager.
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
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "SWMMEngine.hpp"
#include "HotStartManager.hpp"
#include "../plugins/PluginFactory.hpp"
#include "../../../include/openswmm/engine/openswmm_hotstart.h"
#include "../../../include/openswmm/plugin_sdk/IStateIOPlugin.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

// ============================================================================
// Internal cast helpers
// ============================================================================

/**
 * @brief Recover a borrowed internal SWMMEngine pointer from its public handle.
 *
 * No identity, lifetime or state validation; caller applies guards and prevents use after
 * close/destruction.
 *
 * @param[in] e Opaque handle originally created by the corresponding C API, or NULL.
 * @return Same pointer with internal type, possibly NULL; ownership unchanged.
 */
static inline openswmm::SWMMEngine* to_engine(SWMM_Engine e) noexcept {
    return static_cast<openswmm::SWMMEngine*>(e);
}

/**
 * @brief Recover a borrowed internal HotStartFile pointer from its public handle.
 *
 * No identity, lifetime or state validation; caller applies guards and prevents use after
 * close/destruction.
 *
 * @param[in] hs Opaque handle originally created by the corresponding C API, or NULL.
 * @return Same pointer with internal type, possibly NULL; ownership unchanged.
 */
static inline openswmm::HotStartFile* to_hs(SWMM_HotStart hs) noexcept {
    return static_cast<openswmm::HotStartFile*>(hs);
}

#define CHECK_ENGINE(e)                                                                            \
    do {                                                                                           \
        if (!(e))                                                                                  \
            return SWMM_ERR_BADHANDLE;                                                             \
    } while (0)
#define CHECK_HS(hs)                                                                               \
    do {                                                                                           \
        if (!(hs))                                                                                 \
            return SWMM_ERR_BADHANDLE;                                                             \
    } while (0)

// ============================================================================
// C API implementation
// ============================================================================

extern "C" {

SWMM_ENGINE_API const char* swmm_hotstart_last_io_error(void) {
    return openswmm::HotStartManager::last_io_error().c_str();
}

// ----------------------------------------------------------------------------
// swmm_hotstart_save()
// ----------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_hotstart_save(SWMM_Engine engine, const char* path) {
    CHECK_ENGINE(engine);
    if (!path || path[0] == '\0')
        return SWMM_ERR_BADPARAM;

    openswmm::SWMMEngine* eng = to_engine(engine);
    openswmm::SimulationContext& ctx = eng->context();

#ifdef OPENSWMM_HAS_2D
    if (eng->livePassiveExchange()) return SWMM_ERR_HOTSTART;
#endif

    // Must be RUNNING or ENDED to save
    if (ctx.state != openswmm::EngineState::RUNNING && ctx.state != openswmm::EngineState::ENDED) {
        return SWMM_ERR_LIFECYCLE;
    }

    // A heat model with a full engine checkpoint attached writes the V15
    // file; without one it writes the record-based file like any other
    // model (which refuses coupled mesh/ground heat with a message).
    bool full_continuation = ctx.options.heat_transport;
#ifdef OPENSWMM_HAS_2D
    full_continuation |= eng->surfaceRouter2D().subsurface().independentClock();
#endif
    if (full_continuation && eng->hotstartCheckpointAttached()) {
        return eng->saveNativeHotstart(path) == "" ? SWMM_OK : SWMM_ERR_HOTSTART;
    }

    // Dispatch through state-IO plugins. The first plugin whose write_state()
    // succeeds wins; if none is registered (shouldn't happen — SWMMEngine
    // injects DefaultStateIOPlugin in open()), fall back to HotStartManager
    // directly so this path remains usable in legacy embeddings.
    const auto& state_plugins = eng->plugin_factory().state_io_plugins();
    if (!state_plugins.empty()) {
        for (auto* sp : state_plugins) {
            const int rc = sp->write_state(path, ctx);
            if (rc == 0)
                return SWMM_OK;
        }
        return SWMM_ERR_HOTSTART;
    }

    // Fallback (no plugin registered): direct HotStartManager call.
    openswmm::HotStartFile* hs =
        openswmm::HotStartManager::save(ctx, &eng->runoff_solver(), &eng->gw_solver(), path);

    if (!hs)
        return SWMM_ERR_HOTSTART;
    delete hs;
    return SWMM_OK;
}

// ----------------------------------------------------------------------------
// swmm_hotstart_open()
// ----------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_hotstart_open(const char* path, SWMM_HotStart* hs) {
    if (!hs)
        return SWMM_ERR_BADPARAM;
    *hs = nullptr;
    if (!path || path[0] == '\0')
        return SWMM_ERR_BADPARAM;

    openswmm::HotStartFile* file = openswmm::HotStartManager::open(path);
    if (!file)
        return SWMM_ERR_HOTSTART;

    *hs = static_cast<SWMM_HotStart>(file);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_hotstart_open_legacy(SWMM_Engine model, const char* path,
                                              SWMM_HotStart* hs) {
    if (!hs)
        return SWMM_ERR_BADPARAM;
    *hs = nullptr;
    CHECK_ENGINE(model);
    if (!path || path[0] == '\0')
        return SWMM_ERR_BADPARAM;
    openswmm::HotStartFile* file =
        openswmm::HotStartManager::read_legacy(path, to_engine(model)->context());
    if (!file)
        return SWMM_ERR_HOTSTART;
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
    if (file->legacy_version)
        return SWMM_ERR_HOTSTART; // inspection only; USE HOTSTART reads legacy files itself

    if (!file->engine_checkpoint.empty()) {
        const auto error = eng->queueHotstartCheckpoint(file->engine_checkpoint);
        return error.empty() ? SWMM_OK : SWMM_ERR_HOTSTART;
    }

    // Gap #54: apply V2 format including infiltration + GW state
    const int result = openswmm::HotStartManager::apply(
        *file, ctx, &eng->runoff_solver(), &eng->gw_solver(), [&eng](const std::string& msg) {
            (void)eng;
            (void)msg;
        });

    if (result < 0)
        return SWMM_ERR_HOTSTART;
    // The run's mass balance opened at initialize(); it now starts from the
    // applied state.
    eng->reopenMassBalance();
    return SWMM_OK;
}

// ----------------------------------------------------------------------------
// Modification API
// ----------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_hotstart_set_node_depth(SWMM_HotStart hs, const char* node_id,
                                                 double depth) {
    CHECK_HS(hs);
    if (!node_id)
        return SWMM_ERR_BADPARAM;
    if (to_hs(hs)->legacy_version)
        return SWMM_ERR_HOTSTART; // read-only: closing would write a native file over it
    return to_hs(hs)->set_node_depth(node_id, depth) ? SWMM_OK : SWMM_ERR_BADPARAM;
}

SWMM_ENGINE_API int swmm_hotstart_set_node_head(SWMM_HotStart hs, const char* node_id,
                                                double head) {
    CHECK_HS(hs);
    if (!node_id)
        return SWMM_ERR_BADPARAM;
    if (to_hs(hs)->legacy_version)
        return SWMM_ERR_HOTSTART; // read-only: closing would write a native file over it
    return to_hs(hs)->set_node_head(node_id, head) ? SWMM_OK : SWMM_ERR_BADPARAM;
}

SWMM_ENGINE_API int swmm_hotstart_set_link_flow(SWMM_HotStart hs, const char* link_id,
                                                double flow) {
    CHECK_HS(hs);
    if (!link_id)
        return SWMM_ERR_BADPARAM;
    if (to_hs(hs)->legacy_version)
        return SWMM_ERR_HOTSTART; // read-only: closing would write a native file over it
    return to_hs(hs)->set_link_flow(link_id, flow) ? SWMM_OK : SWMM_ERR_BADPARAM;
}

SWMM_ENGINE_API int swmm_hotstart_set_link_depth(SWMM_HotStart hs, const char* link_id,
                                                 double depth) {
    CHECK_HS(hs);
    if (!link_id)
        return SWMM_ERR_BADPARAM;
    if (to_hs(hs)->legacy_version)
        return SWMM_ERR_HOTSTART; // read-only: closing would write a native file over it
    return to_hs(hs)->set_link_depth(link_id, depth) ? SWMM_OK : SWMM_ERR_BADPARAM;
}

SWMM_ENGINE_API int swmm_hotstart_set_subcatch_runoff(SWMM_HotStart hs, const char* subcatch_id,
                                                      double runoff) {
    CHECK_HS(hs);
    if (!subcatch_id)
        return SWMM_ERR_BADPARAM;
    if (to_hs(hs)->legacy_version)
        return SWMM_ERR_HOTSTART; // read-only: closing would write a native file over it
    return to_hs(hs)->set_subcatch_runoff(subcatch_id, runoff) ? SWMM_OK : SWMM_ERR_BADPARAM;
}

/** Refuse edits a restart would never see: legacy handles and full-checkpoint files. */
static int stateEditRefusal(const openswmm::HotStartFile* file) noexcept {
    return file->legacy_version || !file->engine_checkpoint.empty() ? SWMM_ERR_HOTSTART : SWMM_OK;
}

SWMM_ENGINE_API int swmm_hotstart_set_node_state(SWMM_HotStart hs, const char* node_id,
                                                 double depth) {
    CHECK_HS(hs);
    if (!node_id || !std::isfinite(depth) || depth < 0.0)
        return SWMM_ERR_BADPARAM;
    if (const int refused = stateEditRefusal(to_hs(hs)))
        return refused;
    return to_hs(hs)->set_node_state(node_id, depth) ? SWMM_OK : SWMM_ERR_BADPARAM;
}

SWMM_ENGINE_API int swmm_hotstart_set_link_state(SWMM_HotStart hs, const char* link_id,
                                                 double flow, double depth) {
    CHECK_HS(hs);
    if (!link_id || !std::isfinite(flow) || !std::isfinite(depth) || depth < 0.0)
        return SWMM_ERR_BADPARAM;
    if (const int refused = stateEditRefusal(to_hs(hs)))
        return refused;
    return to_hs(hs)->set_link_state(link_id, flow, depth) ? SWMM_OK : SWMM_ERR_BADPARAM;
}

// ----------------------------------------------------------------------------
// Query metadata
// ----------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_hotstart_get_sim_time(SWMM_HotStart hs, double* sim_time) {
    CHECK_HS(hs);
    if (!sim_time)
        return SWMM_ERR_BADPARAM;
    *sim_time = to_hs(hs)->header.sim_time;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_hotstart_get_start_date(SWMM_HotStart hs, double* start_date) {
    CHECK_HS(hs);
    if (!start_date)
        return SWMM_ERR_BADPARAM;
    *start_date = to_hs(hs)->header.start_date;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_hotstart_get_crs(SWMM_HotStart hs, char* buf, int buflen) {
    CHECK_HS(hs);
    if (!buf || buflen <= 0)
        return SWMM_ERR_BADPARAM;
    const std::string& crs = to_hs(hs)->header.crs;
    const int copy_len =
        static_cast<int>(crs.size()) < buflen - 1 ? static_cast<int>(crs.size()) : buflen - 1;
    std::memcpy(buf, crs.c_str(), static_cast<std::size_t>(copy_len));
    buf[copy_len] = '\0';
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_hotstart_node_count(SWMM_HotStart hs) {
    if (!hs)
        return -1;
    return static_cast<int>(to_hs(hs)->nodes.size());
}

SWMM_ENGINE_API int swmm_hotstart_link_count(SWMM_HotStart hs) {
    if (!hs)
        return -1;
    return static_cast<int>(to_hs(hs)->links.size());
}

// ----------------------------------------------------------------------------
// Contents and per-object records
// ----------------------------------------------------------------------------

/** Copy an ID into an optional caller buffer; false for a buffer without room for NUL. */
static bool copyId(const std::string& id, char* buf, int len) noexcept {
    if (!buf)
        return true;
    if (len <= 0)
        return false;
    const auto n = std::min(id.size(), static_cast<std::size_t>(len - 1));
    std::memcpy(buf, id.data(), n);
    buf[n] = '\0';
    return true;
}

static void put(double* out, double value) noexcept {
    if (out)
        *out = value;
}

SWMM_ENGINE_API int swmm_hotstart_get_contents(SWMM_HotStart hs, int* flags) {
    CHECK_HS(hs);
    if (!flags)
        return SWMM_ERR_BADPARAM;
    const auto& f = *to_hs(hs);
    int v = 0;
    if (!f.species.empty())
        v |= SWMM_HOTSTART_HAS_SPECIES;
    if (f.gw_n_cells > 0)
        v |= SWMM_HOTSTART_HAS_GROUNDWATER;
    if (!f.gw_species.empty() || !f.gw_transport_state.empty())
        v |= SWMM_HOTSTART_HAS_GW_TRANSPORT;
    if (!f.snow_state.empty())
        v |= SWMM_HOTSTART_HAS_SNOW;
    if (std::any_of(f.nodes.begin(), f.nodes.end(),
                    [](const auto& n) { return !n.lid_cells.empty(); }))
        v |= SWMM_HOTSTART_HAS_LID;
    if (!f.surface_state.empty())
        v |= SWMM_HOTSTART_HAS_SURFACE;
    if (!f.engine_checkpoint.empty())
        v |= SWMM_HOTSTART_HAS_ENGINE_CHECKPOINT;
    if (f.legacy_version)
        v |= SWMM_HOTSTART_LEGACY;
    *flags = v;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_hotstart_get_version(SWMM_HotStart hs, int* version) {
    CHECK_HS(hs);
    if (!version)
        return SWMM_ERR_BADPARAM;
    const auto& f = *to_hs(hs);
    *version = f.legacy_version ? f.legacy_version : static_cast<int>(f.header.version);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_hotstart_get_saved_at(SWMM_HotStart hs, long long* seconds) {
    CHECK_HS(hs);
    if (!seconds)
        return SWMM_ERR_BADPARAM;
    *seconds = static_cast<long long>(to_hs(hs)->header.timestamp);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_hotstart_get_end_date(SWMM_HotStart hs, double* end_date) {
    CHECK_HS(hs);
    if (!end_date)
        return SWMM_ERR_BADPARAM;
    *end_date = to_hs(hs)->header.end_date;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_hotstart_subcatch_count(SWMM_HotStart hs) {
    if (!hs)
        return -1;
    return static_cast<int>(to_hs(hs)->subcatches.size());
}

SWMM_ENGINE_API int swmm_hotstart_get_node(SWMM_HotStart hs, int index, char* id, int id_len,
                                           double* depth, double* head, double* volume,
                                           double* age) {
    CHECK_HS(hs);
    const auto& nodes = to_hs(hs)->nodes;
    if (index < 0 || static_cast<std::size_t>(index) >= nodes.size())
        return SWMM_ERR_BADINDEX;
    const auto& rec = nodes[static_cast<std::size_t>(index)];
    if (!copyId(rec.id, id, id_len))
        return SWMM_ERR_BADPARAM;
    put(depth, rec.depth);
    put(head, rec.head);
    put(volume, rec.volume);
    put(age, rec.age);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_hotstart_get_link(SWMM_HotStart hs, int index, char* id, int id_len,
                                           double* flow, double* depth, double* volume,
                                           double* age) {
    CHECK_HS(hs);
    const auto& links = to_hs(hs)->links;
    if (index < 0 || static_cast<std::size_t>(index) >= links.size())
        return SWMM_ERR_BADINDEX;
    const auto& rec = links[static_cast<std::size_t>(index)];
    if (!copyId(rec.id, id, id_len))
        return SWMM_ERR_BADPARAM;
    put(flow, rec.flow);
    put(depth, rec.depth);
    put(volume, rec.volume);
    put(age, rec.age);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_hotstart_get_subcatch(SWMM_HotStart hs, int index, char* id, int id_len,
                                               double* runoff, double* gw_flow,
                                               double* gw_upper_moisture, double* gw_lower_depth) {
    CHECK_HS(hs);
    const auto& f = *to_hs(hs);
    if (index < 0 || static_cast<std::size_t>(index) >= f.subcatches.size())
        return SWMM_ERR_BADINDEX;
    const auto& rec = f.subcatches[static_cast<std::size_t>(index)];
    if (!copyId(rec.id, id, id_len))
        return SWMM_ERR_BADPARAM;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    // Native files mark absent groundwater with a negative moisture (and V1 files carry none).
    const bool gw = f.legacy_version ? std::isfinite(rec.gw_theta) : rec.gw_theta >= 0.0;
    put(runoff, rec.runoff);
    put(gw_flow, gw ? rec.gwater : nan);
    put(gw_upper_moisture, gw ? rec.gw_theta : nan);
    put(gw_lower_depth, gw ? rec.gw_lower_depth : nan);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_hotstart_species_count(SWMM_HotStart hs) {
    if (!hs)
        return -1;
    return static_cast<int>(to_hs(hs)->species.size());
}

SWMM_ENGINE_API int swmm_hotstart_get_species(SWMM_HotStart hs, int index, char* name,
                                              int name_len) {
    CHECK_HS(hs);
    const auto& species = to_hs(hs)->species;
    if (index < 0 || static_cast<std::size_t>(index) >= species.size())
        return SWMM_ERR_BADINDEX;
    if (!name || name_len <= 0)
        return SWMM_ERR_BADPARAM;
    copyId(species[static_cast<std::size_t>(index)], name, name_len);
    return SWMM_OK;
}

/** Shared bounds checks for node/link species lookups. */
static int speciesValue(const std::vector<double>& block, std::size_t records, int record,
                        std::size_t species, int index, double* value) noexcept {
    if (!value)
        return SWMM_ERR_BADPARAM;
    if (record < 0 || static_cast<std::size_t>(record) >= records || index < 0 ||
        static_cast<std::size_t>(index) >= species)
        return SWMM_ERR_BADINDEX;
    const auto at = static_cast<std::size_t>(record) * species + static_cast<std::size_t>(index);
    if (at >= block.size())
        return SWMM_ERR_BADINDEX;
    *value = block[at];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_hotstart_get_node_species(SWMM_HotStart hs, int node_index,
                                                   int species_index, double* value) {
    CHECK_HS(hs);
    const auto& f = *to_hs(hs);
    return speciesValue(f.node_species, f.nodes.size(), node_index, f.species.size(),
                        species_index, value);
}

SWMM_ENGINE_API int swmm_hotstart_get_link_species(SWMM_HotStart hs, int link_index,
                                                   int species_index, double* value) {
    CHECK_HS(hs);
    const auto& f = *to_hs(hs);
    return speciesValue(f.link_species, f.links.size(), link_index, f.species.size(),
                        species_index, value);
}

// ----------------------------------------------------------------------------
// Warning access
// ----------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_hotstart_warning_count(SWMM_HotStart hs) {
    if (!hs)
        return 0;
    return static_cast<int>(to_hs(hs)->warnings.size());
}

SWMM_ENGINE_API const char* swmm_hotstart_warning(SWMM_HotStart hs, int index) {
    if (!hs)
        return nullptr;
    const auto& w = to_hs(hs)->warnings;
    if (index < 0 || static_cast<std::size_t>(index) >= w.size())
        return nullptr;
    return w[static_cast<std::size_t>(index)].c_str();
}

// ----------------------------------------------------------------------------
// Close
// ----------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_hotstart_close(SWMM_HotStart hs) {
    if (!hs)
        return SWMM_OK; // safe no-op on null

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
