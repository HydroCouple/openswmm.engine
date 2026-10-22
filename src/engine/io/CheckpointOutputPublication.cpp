// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin

/**
 * @file      CheckpointOutputPublication.cpp
 * @brief     Publication of outputs associated with restored checkpoint sessions.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

// FileRenameInfoEx provides open-handle replacement starting with Windows 10.
#if defined(_WIN32) && (!defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0A00)
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include "CheckpointOutputPublication.hpp"
#include "../core/FileIO.hpp"
#include <atomic>
#include <fstream>
#include <stdexcept>
#include <sys/stat.h>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <new>
#ifndef _WIN32
#include <unistd.h>
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

namespace openswmm::io {
// Contract: openswmm::io::checkpointOpenOutput.
FILE* checkpointOpenOutput(const std::filesystem::path& path, bool existing) {
#ifdef _WIN32
    // Publication replaces live files. CRT fopen does not share DELETE access.
    HANDLE handle =
        CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    existing ? OPEN_EXISTING : CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return nullptr;
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDWR | _O_BINARY);
    if (fd < 0) {
        CloseHandle(handle);
        return nullptr;
    }
    FILE* stream = _fdopen(fd, existing ? "r+b" : "w+b");
    if (!stream)
        _close(fd);
    return stream;
#else
    return fopen_path(path, existing ? "r+b" : "w+bx");
#endif
}
// Contract: openswmm::io::checkpointFileDescriptor.
int checkpointFileDescriptor(FILE* stream) noexcept {
#ifdef _WIN32
    return stream ? _fileno(stream) : -1;
#else
    return stream ? fileno(stream) : -1;
#endif
}
void checkpointReadOutput(int descriptor, uint64_t offset, char* data, std::size_t bytes) {
    if (descriptor < 0 || offset > uint64_t(INT64_MAX) || bytes > uint64_t(INT64_MAX) - offset)
        throw std::runtime_error("Invalid checkpoint read range.");
#ifdef _WIN32
    const auto position = _lseeki64(descriptor, 0, SEEK_CUR);
    const bool positioned = position >= 0 && _lseeki64(descriptor, offset, SEEK_SET) >= 0;
#else
    const auto position = lseek(descriptor, 0, SEEK_CUR);
    const bool positioned = position >= 0 && lseek(descriptor, offset, SEEK_SET) >= 0;
#endif
    bool complete = positioned;
    while (complete && bytes) {
        const auto n = unsigned(std::min(bytes, std::size_t(INT_MAX)));
#ifdef _WIN32
        const auto got = _read(descriptor, data, n);
#else
        const auto got = ::read(descriptor, data, n);
#endif
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0) {
            complete = false;
            break;
        }
        data += got;
        bytes -= got;
    }
#ifdef _WIN32
    const bool restored = position >= 0 && _lseeki64(descriptor, position, SEEK_SET) == position;
#else
    const bool restored = position >= 0 && lseek(descriptor, position, SEEK_SET) == position;
#endif
    if (!complete || !restored)
        throw std::runtime_error("Cannot read checkpoint output range.");
}
namespace {
void renameOutput(const std::filesystem::path& from, const std::filesystem::path& to,
                  std::error_code& ec) {
#ifdef _WIN32
    try {
        // Replacement must preserve handles to the old destination. Prepare
        // all allocations before opening a DELETE handle; report allocation
        // errors through ec so an earlier output can still be rolled back.
        const auto name = std::filesystem::absolute(to).native();
        const auto size =
            offsetof(FILE_RENAME_INFO, FileName) + (name.size() + 1) * sizeof(wchar_t);
        if (size > MAXDWORD) {
            ec = std::make_error_code(std::errc::filename_too_long);
            return;
        }
        std::vector<unsigned char> buffer(size);
        auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
        info->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
        info->RootDirectory = nullptr;
        info->FileNameLength = DWORD(name.size() * sizeof(wchar_t));
        std::memcpy(info->FileName, name.data(), info->FileNameLength);
        const HANDLE file = CreateFileW(
            from.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            ec = {int(GetLastError()), std::system_category()};
            return;
        }
        const bool moved =
            SetFileInformationByHandle(file, FileRenameInfoEx, info, DWORD(size)) != 0;
        const DWORD error = moved ? ERROR_SUCCESS : GetLastError();
        CloseHandle(file);
        ec = {int(error), std::system_category()};
    } catch (const std::filesystem::filesystem_error& error) {
        ec = error.code();
    } catch (const std::bad_alloc&) {
        ec = std::make_error_code(std::errc::not_enough_memory);
    }
#else
    std::filesystem::rename(from, to, ec);
#endif
}
/**
 * @brief Enforce a publication precondition.
 * @param[in] ok Condition required to proceed.
 * @param[in] message Non-null diagnostic copied into an exception on failure.
 * @throws std::runtime_error ok is false.
 */
void require(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
/**
 * @brief Check that a destination still matches its fresh header and file identity.
 * @param[in] output Contract with destination bytes and optional borrowed
 * original descriptor; the staging path is not inspected here.
 * @return True only for a writable regular file with exact fresh_header
 * contents and, when requested, the same identity as original_descriptor.
 * Filesystem/read failures and identity mismatches return false.
 * @note Does not lock the file or prevent changes after the check.
 */
bool fresh(const CheckpointOutputPublication::Output& output) {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(output.destination, ec);
    const auto writable = std::filesystem::perms::owner_write |
                          std::filesystem::perms::group_write |
                          std::filesystem::perms::others_write;
    if (ec || !std::filesystem::is_regular_file(status) ||
        (status.permissions() & writable) == std::filesystem::perms::none ||
        std::filesystem::file_size(output.destination, ec) != output.fresh_header.size() || ec)
        return false;
    if (output.original_descriptor >= 0) {
#ifdef _WIN32
        // CRT stat inode fields are not Windows file identities. Compare the
        // live handle with the named file by volume serial and file index.
        const auto original = reinterpret_cast<HANDLE>(_get_osfhandle(output.original_descriptor));
        const auto named_handle =
            CreateFileW(output.destination.c_str(), FILE_READ_ATTRIBUTES,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (named_handle == INVALID_HANDLE_VALUE)
            return false;
        BY_HANDLE_FILE_INFORMATION opened{}, named{};
        const bool identified = GetFileInformationByHandle(original, &opened) &&
                                GetFileInformationByHandle(named_handle, &named);
        CloseHandle(named_handle);
        if (!identified || (named.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            opened.dwVolumeSerialNumber != named.dwVolumeSerialNumber ||
            opened.nFileIndexHigh != named.nFileIndexHigh ||
            opened.nFileIndexLow != named.nFileIndexLow)
            return false;
#else
        struct stat opened{}, named{};
        if (fstat(output.original_descriptor, &opened) || stat(output.destination.c_str(), &named))
            return false;
        if (opened.st_dev != named.st_dev || opened.st_ino != named.st_ino)
            return false;
#endif
    }
    std::string bytes(output.fresh_header.size(), '\0');
    if (output.original_descriptor >= 0) {
        try {
            checkpointReadOutput(output.original_descriptor, 0, bytes.data(), bytes.size());
        } catch (const std::runtime_error&) {
            return false;
        }
        return bytes == output.fresh_header;
    }
    std::ifstream in(output.destination, std::ios::binary);
    in.read(bytes.data(), bytes.size());
    return bool(in) && in.peek() == std::char_traits<char>::eof() && bytes == output.fresh_header;
}
}
// Contract: openswmm::io::CheckpointOutputPublication::CheckpointOutputPublication.
CheckpointOutputPublication::CheckpointOutputPublication(std::vector<Output> outputs) {
    try {
        require(!outputs.empty(), "Checkpoint publication needs an output.");
        entries_.reserve(outputs.size());
        std::vector<std::filesystem::path> paths;
        for (auto& output : outputs) {
            require(!output.fresh_header.empty() && fresh(output),
                    "Checkpoint destination is no longer its fresh result header.");
            require(
                std::filesystem::is_regular_file(std::filesystem::symlink_status(output.staging)),
                "Checkpoint staging file is unavailable.");
            for (const auto& path : {output.staging, output.destination}) {
                const auto canonical = std::filesystem::weakly_canonical(path);
                for (const auto& previous : paths)
                    require(canonical != previous && !std::filesystem::equivalent(path, previous),
                            "Checkpoint output paths alias each other.");
                paths.push_back(canonical);
            }
            entries_.push_back({std::move(output)});
        }
        static std::atomic<unsigned long long> serial{0};
        for (auto& entry : entries_) {
            std::error_code ec;
            std::filesystem::permissions(
                entry.output.staging,
                std::filesystem::status(entry.output.destination).permissions());
            for (int k = 0; k < 32; ++k) {
                auto backup = entry.output.destination;
                backup += ".checkpoint-backup-" + std::to_string(++serial);
                std::filesystem::create_hard_link(entry.output.destination, backup, ec);
                if (!ec) {
                    entry.backup = std::move(backup);
                    break;
                }
                if (ec != std::errc::file_exists)
                    break;
            }
            require(!entry.backup.empty(), "Cannot reserve checkpoint output rollback header.");
        }
    } catch (...) {
        cleanup();
        throw;
    }
}
// Contract: openswmm::io::CheckpointOutputPublication::~CheckpointOutputPublication.
CheckpointOutputPublication::~CheckpointOutputPublication() {
    cleanup();
}
// Contract: openswmm::io::CheckpointOutputPublication::cleanup.
void CheckpointOutputPublication::cleanup() noexcept {
    for (auto& entry : entries_)
        if (!entry.backup.empty() && !entry.retain) {
            std::error_code ec;
            std::filesystem::remove(entry.backup, ec);
        }
}
// Contract: openswmm::io::CheckpointOutputPublication::publish.
void CheckpointOutputPublication::publish() {
    for (const auto& entry : entries_) {
        require(!entry.published && fresh(entry.output),
                "Checkpoint destination changed before publication.");
        require(std::filesystem::equivalent(entry.output.destination, entry.backup),
                "Checkpoint destination identity changed before publication.");
    }
    std::error_code error;
    for (auto& entry : entries_) {
        renameOutput(entry.output.staging, entry.output.destination, error);
        if (error)
            break;
        entry.published = true;
    }
    if (!error)
        return;
    bool rollback_failed = false;
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it)
        if (it->published) {
            std::error_code ec;
            renameOutput(it->backup, it->output.destination, ec);
            if (ec) {
                it->retain = true;
                rollback_failed = true;
            } else {
                it->backup.clear();
                it->published = false;
            }
        }
    if (rollback_failed) {
        std::string message =
            "Checkpoint output publication and rollback failed; recovery headers:";
        for (const auto& entry : entries_)
            if (entry.retain)
                message += " " + entry.backup.string();
        throw std::runtime_error(message);
    }
    throw std::runtime_error(
        "Cannot publish checkpoint outputs; fresh destinations were restored.");
}
}
