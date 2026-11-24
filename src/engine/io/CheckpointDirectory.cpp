// SPDX-License-Identifier: Apache-2.0

/**
 * @file      CheckpointDirectory.cpp
 * @brief     Checkpoint directory layout, validation, and file management.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "CheckpointDirectory.hpp"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#ifdef __APPLE__
#include <fcntl.h>
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(__EMSCRIPTEN__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace openswmm::io {
// Contract: openswmm::io::checkpointNoReplaceAvailable.
bool checkpointNoReplaceAvailable() noexcept {
#if defined(_WIN32) || defined(__APPLE__) || (defined(__linux__) && defined(SYS_renameat2)) ||     \
    defined(__EMSCRIPTEN__)
    return true;
#else
    return false;
#endif
}
// Contract: openswmm::io::CheckpointDirectory::CheckpointDirectory.
CheckpointDirectory::CheckpointDirectory(std::filesystem::path destination)
    : destination_(std::move(destination)) {
    if (std::filesystem::exists(std::filesystem::symlink_status(destination_)))
        throw std::runtime_error("Checkpoint bundle destination already exists.");
    static std::atomic<uint64_t> serial{0};
    for (int k = 0; k < 32; ++k) {
        staging_ = destination_;
        staging_ += ".bundle-part-" + std::to_string(++serial);
        std::error_code ec;
        if (std::filesystem::create_directory(staging_, ec)) {
            owned_ = true;
            return;
        }
        if (ec && ec != std::errc::file_exists)
            throw std::filesystem::filesystem_error(
                "Cannot create checkpoint bundle staging directory.", staging_, ec);
    }
    throw std::runtime_error("Cannot create exclusive checkpoint bundle staging directory.");
}
// Contract: openswmm::io::CheckpointDirectory::~CheckpointDirectory.
CheckpointDirectory::~CheckpointDirectory() {
    if (owned_) {
        std::error_code ec;
        std::filesystem::remove_all(staging_, ec);
    }
}
// Contract: openswmm::io::CheckpointDirectory::publish.
void CheckpointDirectory::publish() {
    if (!owned_)
        throw std::runtime_error("Checkpoint bundle staging directory is not owned.");
    publishCheckpointNoReplace(staging_, destination_);
    owned_ = false;
}
// Contract: openswmm::io::publishCheckpointNoReplace.
void publishCheckpointNoReplace(const std::filesystem::path& staging,
                                const std::filesystem::path& destination) {
    // Plain filesystem::rename can replace an empty directory on POSIX.
    // Use each platform's atomic no-replace operation instead.
#ifdef _WIN32
    const bool ok = MoveFileExW(staging.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH) != 0;
#elif defined(__APPLE__)
    const bool ok =
        renameatx_np(AT_FDCWD, staging.c_str(), AT_FDCWD, destination.c_str(), RENAME_EXCL) == 0;
#elif defined(__linux__) && defined(SYS_renameat2)
    const bool ok = syscall(SYS_renameat2, AT_FDCWD, staging.c_str(), AT_FDCWD, destination.c_str(),
                            1 /* RENAME_NOREPLACE */) == 0;
#elif defined(__EMSCRIPTEN__)
    // Emscripten has neither renameat2 nor renamex_np, and no hard links.
    // Claim the destination with an exclusive create (mkdir, or open with
    // O_EXCL, fails when it exists), then rename onto that claim: rename may
    // replace only the empty claim made here, never an earlier destination.
    std::error_code type_error;
    const bool directory = std::filesystem::is_directory(staging, type_error);
    bool claimed = false;
    if (!type_error && directory) {
        claimed = ::mkdir(destination.c_str(), 0777) == 0;
    } else if (!type_error) {
        const int fd = ::open(destination.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
        claimed = fd >= 0;
        if (claimed)
            ::close(fd);
    }
    const bool ok = claimed && ::rename(staging.c_str(), destination.c_str()) == 0;
    if (claimed && !ok) {
        // Remove only the claim: an empty directory, or the empty file created above.
        struct stat st{};
        if (directory)
            ::rmdir(destination.c_str());
        else if (::stat(destination.c_str(), &st) == 0 && st.st_size == 0)
            ::unlink(destination.c_str());
    }
#else
    const bool ok = false;
#endif
    if (!ok)
        throw std::runtime_error("Cannot publish checkpoint without replacing its destination.");
}
}
