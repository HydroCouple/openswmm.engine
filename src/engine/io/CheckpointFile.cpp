// SPDX-License-Identifier: Apache-2.0

/**
 * @file      CheckpointFile.cpp
 * @brief     Versioned checkpoint file serialization and integrity checks.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "CheckpointFile.hpp"
#include "CheckpointOutputPublication.hpp"
#include "../core/CompletedSourceArchive.hpp"
#include "../core/FileIO.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <stdexcept>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
namespace openswmm::io {
namespace {
/**
 * @brief Enforce a checkpoint-file precondition or I/O result.
 * @param[in] ok Condition required for continued processing.
 * @param[in] message Non-null diagnostic copied into the exception on failure.
 * @throws std::runtime_error ok is false.
 */
void require(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
constexpr uint64_t seed = 14695981039346656037ULL;
/**
 * @brief Extend an FNV-1a fingerprint with a byte range.
 * @param[in] h Previous hash state, or seed for a fresh stream.
 * @param[in] data Borrowed readable bytes, non-null when n > 0.
 * @param[in] n Number of bytes to incorporate.
 * @return Updated 64-bit fingerprint with intentional unsigned wraparound.
 */
uint64_t hash(uint64_t h, const char* data, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        h ^= static_cast<unsigned char>(data[i]);
        h *= 1099511628211ULL;
    }
    return h;
}
struct File {
    FILE* f;
    /**
     * @brief Close the owned C stream when non-null.
     * @note Close errors are ignored during cleanup; the stream must not have
     * been closed elsewhere unless f was first reset to null.
     */
    ~File() {
        if (f)
            std::fclose(f);
    }
};
/**
 * @brief Hash and optionally copy exactly bytes bytes from a stream.
 * @param[in,out] f Readable stream positioned at the desired prefix start when descriptor <0;
 * advanced by bytes on success and possibly partly on failure. Ignored and may be null when
 * descriptor >=0.
 * @param[in] bytes Number of bytes to consume; zero returns the FNV seed.
 * @param[in,out] out Optional writable destination stream; null hashes only.
 * Neither stream is retained or closed, and output is not flushed here.
 * @param[in] descriptor Borrowed seekable writer descriptor, or negative to use f. A supplied
 * descriptor is read from byte zero with its original position restored for every chunk; caller
 * must flush and exclusively hold the writer throughout this call. No descriptor is closed.
 * @return FNV-1a fingerprint of the consumed byte range.
 * @throws std::runtime_error A full read or requested write cannot be completed.
 * @note A fixed 64 KiB buffer bounds temporary memory.
 */
uint64_t prefixHash(FILE* f, uint64_t bytes, FILE* out = nullptr, int descriptor = -1) {
    std::array<char, 64 * 1024> buffer{};
    uint64_t h = seed, offset = 0;
    while (bytes) {
        const auto n = std::min(bytes, uint64_t(buffer.size()));
        if (descriptor >= 0)
            checkpointReadOutput(descriptor, offset, buffer.data(), n);
        else
            require(std::fread(buffer.data(), 1, n, f) == n,
                    "Checkpoint companion is truncated or unreadable.");
        offset += n;
        h = hash(h, buffer.data(), n);
        if (out)
            require(std::fwrite(buffer.data(), 1, n, out) == n,
                    "Cannot copy checkpoint companion.");
        bytes -= n;
    }
    return h;
}
}
// Contract: openswmm::io::checkpointFileSize.
uint64_t checkpointFileSize(const std::filesystem::path& path) {
    require(std::filesystem::is_regular_file(std::filesystem::symlink_status(path)),
            "Checkpoint companion must be a regular file.");
    auto bytes = std::filesystem::file_size(path);
    require(bytes > 0 && bytes <= uint64_t(INT64_MAX), "Invalid checkpoint companion size.");
    return bytes;
}
// Contract: openswmm::io::checkpointSeek.
void checkpointSeek(FILE* f, uint64_t offset) {
    require(f && offset <= uint64_t(INT64_MAX), "Invalid checkpoint file offset.");
#ifdef _WIN32
    require(_fseeki64(f, static_cast<int64_t>(offset), SEEK_SET) == 0,
            "Cannot seek checkpoint file.");
#else
    require(fseeko(f, static_cast<off_t>(offset), SEEK_SET) == 0, "Cannot seek checkpoint file.");
#endif
}
// Contract: openswmm::io::CheckpointFile::encode.
std::string CheckpointFile::encode() const {
    detail::CompletedSourceArchive a;
    uint32_t version = 1, endian = 0x01020304;
    auto name = path_utf8(path);
    auto n = bytes, h = fingerprint;
    a(version, endian, name, n, h);
    return a.bytes();
}
// Contract: openswmm::io::CheckpointFile::decode.
CheckpointFile CheckpointFile::decode(std::string bytes) {
    require(bytes.size() < 16384, "Checkpoint companion contract exceeds its limit.");
    detail::CompletedSourceArchive a(std::move(bytes));
    uint32_t version, endian;
    std::string path;
    CheckpointFile file;
    a(version, endian, path, file.bytes, file.fingerprint);
    a.finish();
    require(version == 1 && endian == 0x01020304 && !path.empty() && path.size() < 8192 &&
                path.find('\0') == std::string::npos && file.bytes > 0 &&
                file.bytes <= uint64_t(INT64_MAX),
            "Invalid checkpoint companion contract.");
    file.path = utf8_path(path);
    return file;
}
// Contract: openswmm::io::CheckpointFile::verify.
void CheckpointFile::verify() const {
    require(checkpointFileSize(path) == bytes, "Checkpoint companion size changed.");
    File f{fopen_path(path, "rb")};
    require(f.f, "Cannot open checkpoint companion.");
    require(prefixHash(f.f, bytes) == fingerprint, "Checkpoint companion fingerprint differs.");
}
// Contract: openswmm::io::OwnedCheckpointFile::~OwnedCheckpointFile.
OwnedCheckpointFile::~OwnedCheckpointFile() {
    if (owned) {
        std::error_code ec;
        std::filesystem::remove(file.path, ec);
    }
}
// Contract: openswmm::io::copyCheckpointFile.
std::shared_ptr<OwnedCheckpointFile> copyCheckpointFile(const std::filesystem::path& source,
                                                        uint64_t bytes,
                                                        const std::filesystem::path& base,
                                                        int source_descriptor) {
    require(bytes > 0 && bytes <= checkpointFileSize(source), "Checkpoint prefix size differs.");
    File in{source_descriptor < 0 ? fopen_path(source, "rb") : nullptr};
    require(source_descriptor >= 0 || in.f, "Cannot read checkpoint prefix.");
    auto copy = std::make_shared<OwnedCheckpointFile>();
    copy->owned = false;
    File out{nullptr};
    static std::atomic<uint64_t> serial{0};
    for (int k = 0; k < 32 && !out.f; ++k) {
        copy->file.path = base;
        copy->file.path += ".file-part-" + std::to_string(++serial);
        out.f = fopen_path(copy->file.path, "wbx");
    }
    require(out.f, "Cannot create exclusive checkpoint companion.");
    copy->owned = true;
    copy->file.bytes = bytes;
    copy->file.fingerprint = prefixHash(in.f, bytes, out.f, source_descriptor);
    require(std::fflush(out.f) == 0 && !std::ferror(out.f), "Cannot flush checkpoint companion.");
#ifdef _WIN32
    require(_commit(_fileno(out.f)) == 0, "Cannot synchronize checkpoint companion.");
#else
    require(fsync(fileno(out.f)) == 0, "Cannot synchronize checkpoint companion.");
#endif
    const bool closed = std::fclose(out.f) == 0;
    out.f = nullptr;
    require(closed, "Cannot close checkpoint companion.");
    copy->file.verify();
    return copy;
}
}
