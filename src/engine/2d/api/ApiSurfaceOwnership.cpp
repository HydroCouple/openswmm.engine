// SPDX-License-Identifier: Apache-2.0

/**
 * @file      ApiSurfaceOwnership.cpp
 * @brief     C API implementation for surface ownership assignments.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <openswmm/engine/openswmm_surface_ownership.h>
#include "../../core/SWMMEngine.hpp"
#include "../subsurface/SurfaceOwnership.hpp"
#include <algorithm>
#include <cstring>
namespace {
using namespace openswmm::twoD;
/**
 * @brief Copy an optional string result with bounded NUL termination.
 * @param[in] s Borrowed source string; not retained.
 * @param[out] out Optional writable size-byte outfer; NULL skips the copy.
 * @param[in] size Byte capacity including NUL; nonpositive skips copying.
 * @post Copies at most size-1 bytes and terminates when storage/capacity are usable. No
 * allocation or validation of caller capacity.
 */
void copy(const std::string& s, char* out, int size) {
    if (!out || size <= 0)
        return;
    const auto n = std::min(s.size(), std::size_t(size - 1));
    std::memcpy(out, s.data(), n);
    out[n] = 0;
}
/**
 * @brief Validate a handle and pre-run surface-ownership editing state.
 * @param[in] e Optional engine pointer; NULL is refused without dereferencing.
 * @return SWMM_OK for OPENED/BUILDING, SWMM_ERR_BADHANDLE for NULL, or SWMM_ERR_LIFECYCLE for
 * another state. No mutation.
 */
int editable(openswmm::SWMMEngine* e) {
    if (!e)
        return SWMM_ERR_BADHANDLE;
    const auto s = e->context().state;
    return s == openswmm::EngineState::OPENED || s == openswmm::EngineState::BUILDING
               ? SWMM_OK
               : SWMM_ERR_LIFECYCLE;
}
/**
 * @brief Convert subcatchment row indices into authored ownership records.
 * @param[in] e Borrowed engine with parsed subcatchment names and current ownership.
 * @param[in] rows Required count-element array for positive count; ignored for count == -1 or
 * zero.
 * @param[in] count -1 copies current authored records; otherwise a nonnegative row count.
 * @param[in,out] out Replaced for count == -1; otherwise appended to incrementally.
 * @return SWMM_OK or SWMM_ERR_BADPARAM for invalid count/storage/subcatchment. A bad later row
 * leaves earlier appended records in out; the engine is unchanged. Allocation may throw.
 */
int records(openswmm::SWMMEngine& e, const int* rows, int count,
            std::vector<SurfaceOwnerRecord>& out) {
    if (count == -1) {
        out = e.surfaceRouter2D().aquiferConfig().surface_owners;
        return SWMM_OK;
    }
    if (count < 0 || (count > 0 && !rows))
        return SWMM_ERR_BADPARAM;
    for (int i = 0; i < count; ++i) {
        if (rows[i] < 0 || rows[i] >= e.context().n_subcatches())
            return SWMM_ERR_BADPARAM;
        out.push_back({e.context().subcatch_names.name_of(rows[i])});
    }
    return SWMM_OK;
}
/**
 * @brief Resolve ownership against current model geometry without publishing it.
 * @param[in] e Borrowed live engine; referenced geometry/configuration must remain stable during
 * the call.
 * @param[in] rows Borrowed proposed ownership records.
 * @param[in] progress Optional synchronous callback receiving done/total counters; nonzero
 * cancels.
 * @param[in,out] user Opaque caller context forwarded to progress; may be NULL and is not
 * retained.
 * @return Value-owned preview containing resolved shares, diagnostic strings, token and
 * cancellation status. No engine authoring data is published; allocation/resolution may throw.
 */
SurfaceOwnershipPreview preview(openswmm::SWMMEngine& e,
                                const std::vector<SurfaceOwnerRecord>& rows,
                                SWMM_SurfaceOwnerProgress progress = nullptr,
                                void* user = nullptr) {
    auto& r = e.surfaceRouter2D();
    return resolveSurfaceOwnership(e.context(), r.mesh(), r.options(), r.aquiferConfig(), rows,
                                   progress ? std::function<bool(int, int)>([=](int d, int n) {
                                       return progress(d, n, user) != 0;
                                   })
                                            : std::function<bool(int, int)>{});
}
/**
 * @brief Join ownership-preview errors for a C API diagnostic string.
 * @param[in] p Borrowed completed preview.
 * @return Newly owned newline-separated error string, empty for no errors. Does not mutate p;
 * allocation may throw.
 */
std::string diagnostics(const SurfaceOwnershipPreview& p) {
    std::string out;
    for (const auto& e : p.errors) {
        if (!out.empty())
            out += '\n';
        out += e;
    }
    return out;
}
}
extern "C" {
int swmm_surface_owner_get(SWMM_Engine handle, int* rows, int capacity, int* count) {
    auto* e = reinterpret_cast<openswmm::SWMMEngine*>(handle);
    const int status = editable(e);
    if (status)
        return status;
    if (!count || capacity < 0)
        return SWMM_ERR_BADPARAM;
    const auto& cfg = e->surfaceRouter2D().aquiferConfig();
    *count = int(cfg.surface_owners.size());
    if (!rows)
        return capacity == 0 ? SWMM_OK : SWMM_ERR_BADPARAM;
    if (capacity < *count)
        return SWMM_ERR_BADPARAM;
    for (int i = 0; i < *count; ++i)
        rows[i] = e->context().subcatch_names.find(cfg.surface_owners[i].subcatch);
    return SWMM_OK;
}
int swmm_surface_owner_preview(SWMM_Engine handle, const int* rows, int count,
                               SWMM_SurfaceOwnerObject* objects, int oc, int* on,
                               SWMM_SurfaceOwnerShare* shares, int sc, int* sn, double* areas,
                               int ac, int* an, int* valid, char* token, int tc, char* diag, int dc,
                               SWMM_SurfaceOwnerProgress progress, void* user) {
    auto* e = reinterpret_cast<openswmm::SWMMEngine*>(handle);
    const int status = editable(e);
    if (status)
        return status;
    if (!on || !sn || !an || !valid || !token || tc < 17 || oc < 0 || sc < 0 || ac < 0)
        return SWMM_ERR_BADPARAM;
    std::vector<SurfaceOwnerRecord> proposed;
    const int parse = records(*e, rows, count, proposed);
    if (parse)
        return parse;
    const auto p = preview(*e, proposed, progress, user);
    *on = int(p.objects.size());
    *sn = int(p.shares.size());
    *an = int(p.mesh_weather_area.size());
    *valid = p.valid() && !p.cancelled;
    copy(p.token, token, tc);
    copy(diagnostics(p), diag, dc);
    if (p.cancelled)
        return SWMM_ERR_BADPARAM;
    if ((objects && oc < *on) || (shares && sc < *sn) || (areas && ac < *an))
        return SWMM_ERR_BADPARAM;
    if (objects)
        for (int i = 0; i < *on; ++i) {
            const auto& o = p.objects[i];
            auto& v = objects[i];
            v = {};
            v.subcatch = o.subcatch;
            v.reviewed = o.reviewed;
            v.lumped = o.lumped;
            v.status = o.status;
            v.declared_area = o.declared_area;
            v.polygon_area = o.polygon_area;
            v.lid_area = o.lid_area;
            v.pervious_area = o.pervious_area;
            v.impervious_area = o.impervious_area;
            v.native_lid_area = o.native_lid_area;
            v.inside_area = o.inside_area;
            v.outside_area = o.outside_area;
            copy(o.name, v.name, sizeof v.name);
            copy(o.tag, v.tag, sizeof v.tag);
            copy(o.reason, v.reason, sizeof v.reason);
        }
    if (shares)
        for (int i = 0; i < *sn; ++i) {
            const auto& s = p.shares[i];
            shares[i] = {s.subcatch,        s.cell,     s.weather_area,   s.pervious_area,
                         s.impervious_area, s.lid_area, s.native_lid_area};
        }
    if (areas)
        std::copy(p.mesh_weather_area.begin(), p.mesh_weather_area.end(), areas);
    return SWMM_OK;
}
int swmm_surface_owner_replace(SWMM_Engine handle, const int* rows, int count, const char* expected,
                               char* diag, int dc) {
    auto* e = reinterpret_cast<openswmm::SWMMEngine*>(handle);
    const int status = editable(e);
    if (status)
        return status;
    if (!expected || count < 0)
        return SWMM_ERR_BADPARAM;
    std::vector<SurfaceOwnerRecord> proposed;
    const int parse = records(*e, rows, count, proposed);
    if (parse)
        return parse;
    const auto p = preview(*e, proposed);
    if (p.token != expected) {
        copy("Ownership preview is stale; preview the changed model again.", diag, dc);
        return SWMM_ERR_BADPARAM;
    }
    if (count && !p.valid()) {
        copy(diagnostics(p), diag, dc);
        return SWMM_ERR_BADPARAM;
    }
    e->surfaceRouter2D().aquiferConfig().surface_owners = std::move(proposed);
    copy("", diag, dc);
    return SWMM_OK;
}
}
