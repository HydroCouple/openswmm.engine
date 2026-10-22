// SPDX-License-Identifier: Apache-2.0

/**
 * @file      CheckpointFile.hpp
 * @brief     Versioned checkpoint file serialization and integrity checks.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include <cstdint>
#include <cstdio>

namespace openswmm::io {
/// Internal immutable output companion. Integrity detects accidental damage,
/// not malicious edits. Physical/model archives remain independently validated.
struct CheckpointFile {
    std::filesystem::path path;
    uint64_t bytes = 0, fingerprint = 0;
    /**
     * @brief Serialize this companion's path, length and fingerprint contract.
     * @return Newly owned binary archive containing version 1, endian marker,
     * UTF-8 path, byte count and FNV-1a fingerprint; no file data is embedded.
     * @note Does not validate the fields or read the companion file.
     */
    std::string encode() const;
    /**
     * @brief Decode and validate a versioned companion-file contract.
     * @param[in] bytes Owned binary archive, consumed by value; size must be
     * less than 16384 bytes and the encoded UTF-8 path less than 8192 bytes.
     * @return Contract with a nonempty path and byte count in [1, INT64_MAX].
     * @throws std::runtime_error Version, endian marker, field bounds or archive
     * shape is invalid. Archive decoding exceptions may also propagate.
     * @note This validates metadata only; call verify() to inspect file contents.
     */
    static CheckpointFile decode(std::string);
    /**
     * @brief Check the named companion's exact size and fingerprint.
     * @throws std::runtime_error The path is not a non-symlink regular file,
     * is unreadable, has changed length or has a different FNV-1a fingerprint.
     * Filesystem inspection errors may also propagate.
     * @note Uses a fixed 64 KiB read buffer. This detects accidental damage;
     * it is not cryptographic authentication and does not lock the file.
     */
    void verify() const; // fixed 64 KiB read buffer
};
struct OwnedCheckpointFile {
    CheckpointFile file;
    bool owned = true;
    /**
     * @brief Remove file.path when this owner has not called retain().
     * @note Uses error_code cleanup and ignores removal failures. No other
     * pathname is removed and the companion contract itself owns no open handle.
     */
    ~OwnedCheckpointFile();
    /**
     * @brief Release automatic removal responsibility for the companion file.
     * @post Destruction leaves file.path on disk. Caller takes responsibility
     * for publication or later cleanup; the file is not moved or opened.
     */
    void retain() noexcept { owned = false; }
};
/// Copy exactly a known prefix; validate the copy again before publication.
/// Exclusive creation and ownership ensure failures cannot remove other files.
/// A supplied descriptor is borrowed from a flushed, exclusively held writer;
/// its position is preserved, avoiding a second handle under mandatory locks.
/**
 * @brief Copy and verify an exact source prefix into a private companion file.
 * @param[in] source Non-symlink regular source file, read from byte zero.
 * @param[in] bytes Prefix length in [1, source file size], no larger than INT64_MAX.
 * @param[in] staging_base Base path for exclusive sibling names ending
 * in .file-part-N; its parent must exist and permit file creation.
 * @return Shared cleanup owner for a flushed, synchronized, closed and
 * fingerprint-verified private copy. Call retain() only when removal ownership
 * has been transferred; otherwise the last owner's destruction removes it.
 * @throws std::runtime_error Validation, opening, copying, synchronization,
 * closing, verification or all 32 exclusive reservations fail. Filesystem
 * and allocation exceptions may propagate. Partial owned copies are cleaned up.
 * @note The source is not locked; concurrent modifications can invalidate
 * the copy contract and are the caller's responsibility.
 */
std::shared_ptr<OwnedCheckpointFile> copyCheckpointFile(const std::filesystem::path& source,
                                                        uint64_t bytes,
                                                        const std::filesystem::path& staging_base,
                                                        int source_descriptor = -1);
/**
 * @brief Read a checkpoint companion's validated size.
 * @par Input argument
 * The unnamed path argument identifies a non-symlink regular file to inspect.
 * @return Length in bytes, in [1, INT64_MAX].
 * @throws std::runtime_error File type or size is invalid.
 * @throws std::filesystem::filesystem_error Metadata inspection fails.
 */
uint64_t checkpointFileSize(const std::filesystem::path&);
/**
 * @brief Position an existing stream at an absolute checkpoint byte offset.
 * @par Stream argument
 * The first, unnamed argument is a non-null open seekable stream,
 * borrowed and not closed.
 * @par Offset argument
 * The second, unnamed argument is an absolute byte offset in [0, INT64_MAX]; seeking beyond
 * the current end is allowed when supported by the underlying stream.
 * @throws std::runtime_error The stream or offset is invalid, or seeking fails.
 * @note This does not clear existing stream error flags or validate file length.
 */
void checkpointSeek(FILE*, uint64_t);
}
