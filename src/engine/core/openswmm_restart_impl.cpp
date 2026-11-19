// SPDX-License-Identifier: Apache-2.0
/**
 * @file      openswmm_restart_impl.cpp
 * @brief     C API of openswmm_restart.h: the [RESTART] settings and read-only inspection.
 * @see include/openswmm/engine/openswmm_restart.h
 */

#include "openswmm_api_common.hpp"

#include "../../../include/openswmm/engine/openswmm_restart.h"
#include "../input/Tokenizer.hpp"
#include "restart/RestartStore.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

thread_local std::string tl_restart_error;

void copyOut(char* buf, int buflen, const std::string& text) {
    const std::size_t n = std::min(text.size(), static_cast<std::size_t>(buflen - 1));
    std::memcpy(buf, text.data(), n);
    buf[n] = '\0';
}

std::string upper(const char* s) {
    return openswmm::input::Tokenizer::to_upper(s ? s : "");
}

} // namespace

extern "C" {

SWMM_ENGINE_API int swmm_restart_get(SWMM_Engine engine, const char* key, char* buf, int buflen) {
    CHECK_HANDLE(engine);
    if (!key || !buf || buflen <= 0)
        return SWMM_ERR_BADPARAM;
    const auto& r = to_engine(engine)->context().restart;
    const std::string k = upper(key);
    std::string v;
    if (k == "SOURCE_FILE")
        v = r.source_file.original;
    else if (k == "SOURCE_RUN")
        v = r.source_run;
    else if (k == "SOURCE_RECORD")
        v = r.source_record;
    else if (k == "MODE")
        v = r.mode == openswmm::RestartMode::CONTINUE ? "CONTINUE" : "INITIAL_CONDITIONS";
    else if (k == "RECORD_FILE")
        v = r.record_file.original;
    else if (k == "RECORD_FORMAT")
        v = r.record_format == openswmm::RestartFormat::HDF5         ? "HDF5"
            : r.record_format == openswmm::RestartFormat::GEOPACKAGE ? "GEOPACKAGE"
                                                                     : "";
    else if (k == "RECORD_EVERY") {
        char text[40];
        std::snprintf(text, sizeof text, "%.17g", r.record_every);
        v = text;
    } else if (k == "RECORD_CONTINUATION")
        v = r.record_continuation ? "YES" : "NO";
    else
        return SWMM_ERR_BADPARAM;
    copyOut(buf, buflen, v);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_restart_set(SWMM_Engine engine, const char* key, const char* value) {
    CHECK_HANDLE(engine);
    if (!key || !value)
        return SWMM_ERR_BADPARAM;
    auto& r = to_engine(engine)->context().restart;
    const std::string k = upper(key);
    const std::string v(value);
    const std::string w = upper(value);
    if (k == "SOURCE_FILE") {
        r.source_file = v;
    } else if (k == "SOURCE_RUN") {
        r.source_run = v;
    } else if (k == "SOURCE_RECORD") {
        r.source_record = w == "LATEST" ? "LATEST" : v;
    } else if (k == "MODE") {
        if (w.empty() || w == "INITIAL_CONDITIONS")
            r.mode = openswmm::RestartMode::INITIAL_CONDITIONS;
        else if (w == "CONTINUE")
            r.mode = openswmm::RestartMode::CONTINUE;
        else
            return SWMM_ERR_BADPARAM;
    } else if (k == "RECORD_FILE") {
        r.record_file = v;
    } else if (k == "RECORD_FORMAT") {
        if (w.empty())
            r.record_format = openswmm::RestartFormat::NONE;
        else if (w == "GEOPACKAGE")
            r.record_format = openswmm::RestartFormat::GEOPACKAGE;
        else if (w == "HDF5")
            r.record_format = openswmm::RestartFormat::HDF5;
        else
            return SWMM_ERR_BADPARAM;
    } else if (k == "RECORD_EVERY") {
        double seconds = 0.0;
        if (!v.empty()) {
            char* end = nullptr;
            seconds = std::strtod(v.c_str(), &end);
            if (!end || *end != '\0' || !std::isfinite(seconds) || seconds < 0.0)
                return SWMM_ERR_BADPARAM;
        }
        r.record_every = seconds;
    } else if (k == "RECORD_CONTINUATION") {
        if (w.empty() || w == "NO")
            r.record_continuation = false;
        else if (w == "YES")
            r.record_continuation = true;
        else
            return SWMM_ERR_BADPARAM;
    } else {
        return SWMM_ERR_BADPARAM;
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_restart_record_at_count(SWMM_Engine engine, int* count) {
    CHECK_HANDLE(engine);
    if (!count)
        return SWMM_ERR_BADPARAM;
    *count = static_cast<int>(to_engine(engine)->context().restart.record_at.size());
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_restart_record_at_get(SWMM_Engine engine, int idx, double* datetime) {
    CHECK_HANDLE(engine);
    const auto& at = to_engine(engine)->context().restart.record_at;
    if (!datetime || idx < 0 || idx >= static_cast<int>(at.size()))
        return SWMM_ERR_BADPARAM;
    *datetime = at[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_restart_record_at_add(SWMM_Engine engine, double datetime) {
    CHECK_HANDLE(engine);
    if (!std::isfinite(datetime) || datetime < 0.0)
        return SWMM_ERR_BADPARAM;
    to_engine(engine)->context().restart.record_at.push_back(datetime);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_restart_record_at_clear(SWMM_Engine engine) {
    CHECK_HANDLE(engine);
    to_engine(engine)->context().restart.record_at.clear();
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_restart_uses_checkpoint(SWMM_Engine engine, int* uses) {
    CHECK_HANDLE(engine);
    if (!uses)
        return SWMM_ERR_BADPARAM;
    *uses = to_engine(engine)->restartNeedsCheckpoint() ? 1 : 0;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_restart_probe(const char* path, int* backend) {
    if (!path || !backend)
        return SWMM_ERR_BADPARAM;
    *backend = static_cast<int>(openswmm::restart::probe(path));
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_restart_inspect_json(const char* path, char* utf8, size_t capacity,
                                              size_t* required) {
    tl_restart_error.clear();
    if (!path || !required || (!utf8 && capacity)) {
        tl_restart_error = "invalid arguments";
        return SWMM_ERR_BADPARAM;
    }
    namespace rs = openswmm::restart;
    const auto kind = rs::probe(path);
    if (kind != rs::Backend::GEOPACKAGE && kind != rs::Backend::HDF5) {
        tl_restart_error = std::string("'") + path +
                           (kind == rs::Backend::NONE ? "' does not exist or is empty"
                                                      : "' is not a GeoPackage or HDF5 file");
        return SWMM_ERR_HOTSTART;
    }
    std::string error;
    auto store = rs::openStore(path, kind, false, error);
    if (!store) {
        tl_restart_error = error;
        return SWMM_ERR_HOTSTART;
    }
    rs::Catalog catalog;
    if (error = store->catalog(catalog); !error.empty()) {
        tl_restart_error = error;
        return SWMM_ERR_HOTSTART;
    }
    const std::string json = rs::catalogJson(path, catalog);
    *required = json.size() + 1;
    if (!utf8)
        return SWMM_OK;
    if (capacity < *required) {
        tl_restart_error = "buffer too small";
        return SWMM_ERR_BADPARAM;
    }
    std::memcpy(utf8, json.c_str(), *required);
    return SWMM_OK;
}

SWMM_ENGINE_API const char* swmm_restart_last_error(void) {
    return tl_restart_error.c_str();
}

} // extern "C"
