// SPDX-License-Identifier: Apache-2.0

/**
 * @file      openswmm_checkpoint_impl.cpp
 * @brief     Public C API implementation for simulation checkpoints.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <openswmm/engine/openswmm_checkpoint.h>
#include "SWMMEngine.hpp"
#include "CheckpointInspection.hpp"
#include "../io/CheckpointDirectory.hpp"
#include <algorithm>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <string_view>

namespace {
thread_local char diagnostic[8192] = {};
/**
 * @brief Set the thread-local bounded checkpoint diagnostic and return an error code.
 *
 * No allocation or ownership transfer; next checkpoint operation overwrites this thread's
 * diagnostic. Source must not overlap diagnostic storage.
 *
 * @param[in] code Status returned unchanged.
 * @param[in] message Borrowed UTF-8 text; truncation avoids splitting continuation bytes, but does
 *     not validate UTF-8.
 * @return code after copying a NUL-terminated diagnostic prefix.
 */
int failure(int code, std::string_view message) noexcept {
    auto n = std::min(message.size(), sizeof diagnostic - 1);
    // Do not split a UTF-8 sequence when truncating a long path diagnostic.
    if (n < message.size())
        while (n && (static_cast<unsigned char>(message[n]) & 0xc0) == 0x80)
            --n;
    std::memcpy(diagnostic, message.data(), n);
    diagnostic[n] = 0;
    return code;
}
/**
 * @brief Run a checkpoint callable with a C-boundary exception barrier.
 *
 * Clears this thread's previous diagnostic before invoking. Does not undo mutations before an
 * exception or synchronize handle access.
 *
 * @param[in] call Synchronously invoked callable returning an integer status; may update diagnostic
 *     itself.
 * @return Callable status, NOMEM for bad_alloc, INTERNAL for other exceptions with bounded
 *     diagnostic.
 */
template <class F> int operation(F&& call) noexcept {
    diagnostic[0] = 0;
    try {
        return call();
    } catch (const std::bad_alloc&) {
        return failure(SWMM_ERR_NOMEM, "Cannot allocate checkpoint operation memory.");
    } catch (const std::exception& e) {
        return failure(SWMM_ERR_INTERNAL, e.what());
    } catch (...) {
        return failure(SWMM_ERR_INTERNAL, "Non-standard exception in checkpoint operation.");
    }
}
/**
 * @brief Require a nonempty NUL-terminated path with valid UTF-8 encoding.
 *
 * @param[in] path NULL or borrowed NUL-terminated path; no filesystem access.
 * @return True for nonempty valid UTF-8 bytes; does not establish existence, writability or path
 *     safety.
 */
bool validPath(const char* path) {
    return path && *path && openswmm::detail::checkpointUtf8(path);
}
/**
 * @brief Interpret a checkpoint engine handle without runtime validation.
 *
 * @param[in] handle NULL or live SWMMEngine pointer created by the API; arbitrary/stale addresses
 *     invalid.
 * @return Borrowed typed pointer; NULL preserved, ownership unchanged.
 */
openswmm::SWMMEngine* engine(SWMM_Engine handle) {
    return static_cast<openswmm::SWMMEngine*>(handle);
}
/**
 * @brief Check a non-NULL engine for the required checkpoint session boundary.
 *
 * Does not prepare/advance the session or validate a checkpoint path.
 *
 * @param[in] handle Live engine handle; NULL returns BADHANDLE.
 * @param[in] fresh True requires FRESH status, false accepted BOUNDARY status.
 * @return SWMM_OK when required status bit is present, otherwise BADHANDLE/LIFECYCLE with
 *     thread-local diagnostic.
 */
int session(SWMM_Engine handle, bool fresh) {
    if (!handle)
        return failure(SWMM_ERR_BADHANDLE, "Null checkpoint engine handle.");
    const auto required = fresh ? SWMM_CHECKPOINT_SESSION_FRESH : SWMM_CHECKPOINT_SESSION_BOUNDARY;
    if (!(engine(handle)->checkpointSessionStatus() & required))
        return failure(
            SWMM_ERR_LIFECYCLE,
            fresh ? "Checkpoint inspection/restore requires an explicitly prepared fresh session."
                  : "Checkpoint Save requires an explicitly prepared accepted session boundary.");
    return SWMM_OK;
}
/**
 * @brief Translate an engine checkpoint error string to the public I/O status.
 *
 * @param[in] error Borrowed empty-success or diagnostic string.
 * @return SWMM_OK for empty text, otherwise IO with bounded thread-local diagnostic.
 */
int result(const std::string& error) {
    return error.empty() ? SWMM_OK : failure(SWMM_ERR_IO, error);
}
}
extern "C" {
int swmm_checkpoint_capabilities(SWMM_Engine handle, unsigned long* flags) {
    return operation([&] {
        if (!handle)
            return failure(SWMM_ERR_BADHANDLE, "Null checkpoint engine handle.");
        if (!flags)
            return failure(SWMM_ERR_BADPARAM, "Null checkpoint capability output.");
        unsigned long supported = SWMM_CHECKPOINT_INSPECTION | SWMM_CHECKPOINT_RESTORE |
                                  SWMM_CHECKPOINT_BINARY_PROFILE | SWMM_CHECKPOINT_SESSION_BUILDER;
        if (openswmm::io::checkpointNoReplaceAvailable())
            supported |= SWMM_CHECKPOINT_SAVE_NEW;
#ifdef OPENSWMM_HAS_2D
        supported |= SWMM_CHECKPOINT_SURFACE_PROFILE | SWMM_CHECKPOINT_SOURCE_STREAM;
#endif
#ifdef OPENSWMM_HAS_2D_HDF5_OUTPUT
        supported |= SWMM_CHECKPOINT_FILE_PROFILE;
        if (openswmm::io::checkpointNoReplaceAvailable())
            supported |= SWMM_CHECKPOINT_BUNDLE_EXPORT;
#endif
        *flags = supported;
        return int(SWMM_OK);
    });
}
int swmm_checkpoint_session_status(SWMM_Engine handle, unsigned long* flags) {
    return operation([&] {
        if (!handle)
            return failure(SWMM_ERR_BADHANDLE, "Null checkpoint engine handle.");
        if (!flags)
            return failure(SWMM_ERR_BADPARAM, "Null checkpoint session status output.");
        *flags = engine(handle)->checkpointSessionStatus();
        return int(SWMM_OK);
    });
}
int swmm_checkpoint_start_session(SWMM_Engine handle,
                                  const SWMM_CheckpointSessionOptions* options) {
    return operation([&] {
        if (!handle)
            return failure(SWMM_ERR_BADHANDLE, "Null checkpoint engine handle.");
        if (!options || options->struct_size < sizeof *options || options->schema_version != 1 ||
            options->output < 0 || options->output > SWMM_CHECKPOINT_OUTPUT_FILES ||
            (options->detailed_source_intervals != 0 && options->detailed_source_intervals != 1))
            return failure(SWMM_ERR_BADPARAM, "Invalid managed checkpoint options/schema.");
        if (engine(handle)->context().state != openswmm::EngineState::OPENED)
            return failure(SWMM_ERR_LIFECYCLE, "Managed checkpoint start requires OPENED.");
        const std::string source = options->source_history ? options->source_history : "",
                          token = options->ownership_token ? options->ownership_token : "";
        if (!openswmm::detail::checkpointUtf8(source) || !openswmm::detail::checkpointUtf8(token))
            return failure(SWMM_ERR_BADPARAM, "Invalid UTF-8 checkpoint options.");
#ifndef OPENSWMM_HAS_2D_HDF5_OUTPUT
        if (options->output >= SWMM_CHECKPOINT_OUTPUT_MESH)
            return failure(SWMM_ERR_LIFECYCLE,
                           "Managed mesh-output checkpoint profile unavailable in this build.");
#endif
#ifndef OPENSWMM_HAS_2D
        if (!source.empty() || !token.empty())
            return failure(SWMM_ERR_LIFECYCLE, "Managed source profile unavailable in this build.");
#endif
        return result(engine(handle)->startCheckpointSession(
            options->output, source, options->detailed_source_intervals != 0, token));
    });
}
int swmm_checkpoint_inspect(SWMM_Engine handle, const char* path,
                            SWMM_CheckpointInspection* snapshot) {
    return operation([&] {
        const int ready = session(handle, true);
        if (ready)
            return ready;
        if (!snapshot || !validPath(path))
            return failure(SWMM_ERR_BADPARAM,
                           "Checkpoint path must be nonempty UTF-8 and snapshot output nonnull.");
        openswmm::CheckpointInspection info;
        const auto error = engine(handle)->inspectCheckpoint(path, info);
        if (!error.empty())
            return result(error);
        auto copy = std::make_unique<std::string>(info.json());
        *snapshot = copy.release();
        return int(SWMM_OK);
    });
}
int swmm_checkpoint_inspection_json(SWMM_CheckpointInspection handle, char* out, size_t capacity,
                                    size_t* required) {
    return operation([&] {
        if (!handle)
            return failure(SWMM_ERR_BADHANDLE, "Null checkpoint inspection handle.");
        if (!required || (!out && capacity))
            return failure(SWMM_ERR_BADPARAM, "Invalid inspection JSON buffer arguments.");
        const auto& json = *static_cast<const std::string*>(handle);
        *required = json.size() + 1;
        if (!out)
            return int(SWMM_OK);
        if (capacity < *required)
            return failure(SWMM_ERR_BADPARAM, "Inspection JSON buffer is too small.");
        std::memcpy(out, json.c_str(), *required);
        return int(SWMM_OK);
    });
}
void swmm_checkpoint_inspection_destroy(SWMM_CheckpointInspection handle) {
    delete static_cast<std::string*>(handle);
}
int swmm_checkpoint_save(SWMM_Engine handle, const char* path) {
    return operation([&] {
        const int ready = session(handle, false);
        if (ready)
            return ready;
        if (!validPath(path))
            return failure(SWMM_ERR_BADPARAM, "Checkpoint destination must be nonempty UTF-8.");
        if (!openswmm::io::checkpointNoReplaceAvailable())
            return failure(SWMM_ERR_LIFECYCLE,
                           "Exclusive checkpoint publication unavailable in this build.");
        return result(engine(handle)->saveCheckpointNew(path));
    });
}
int swmm_checkpoint_restore(SWMM_Engine handle, const char* path) {
    return operation([&] {
        const int ready = session(handle, true);
        if (ready)
            return ready;
        if (!validPath(path))
            return failure(SWMM_ERR_BADPARAM, "Checkpoint path must be nonempty UTF-8.");
        return result(engine(handle)->loadCheckpoint(path));
    });
}
int swmm_checkpoint_export_bundle(const char* path, const char* folder) {
    return operation([&] {
        if (!validPath(path) || !validPath(folder))
            return failure(SWMM_ERR_BADPARAM,
                           "Checkpoint and bundle destination must be nonempty UTF-8.");
#if defined(OPENSWMM_HAS_2D) && defined(OPENSWMM_HAS_2D_HDF5_OUTPUT)
        if (!openswmm::io::checkpointNoReplaceAvailable())
            return failure(SWMM_ERR_LIFECYCLE,
                           "Exclusive checkpoint bundle publication unavailable in this build.");
        return result(openswmm::SWMMEngine::packageCheckpointBundle(path, folder));
#else
        return failure(SWMM_ERR_LIFECYCLE, "Checkpoint bundle export unavailable in this build.");
#endif
    });
}
const char* swmm_checkpoint_last_error_msg(void) {
    return diagnostic;
}
}
