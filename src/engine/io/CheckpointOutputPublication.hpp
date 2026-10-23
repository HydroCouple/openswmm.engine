// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin

/**
 * @file      CheckpointOutputPublication.hpp
 * @brief     Publication of outputs associated with restored checkpoint sessions.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include <filesystem>
#include <cstdio>
#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>

namespace openswmm::io {
/**
 * @brief Obtain a borrowed platform descriptor for a C stream.
 * @param[in] stream Borrowed open C stream, or null.
 * @return Its CRT/POSIX descriptor, or -1 for null or descriptor failure.
 * @note The stream retains ownership; the caller must not independently
 * close the returned descriptor.
 */
int checkpointFileDescriptor(FILE* stream) noexcept;
/**
 * @brief Read an exact output range through a borrowed seekable writer descriptor.
 * @param[in] descriptor Nonnegative open readable seekable descriptor, flushed and exclusively
 * held by the caller for the complete operation; never closed or retained by this helper.
 * @param[in] offset Absolute byte offset in [0, INT64_MAX], with offset+bytes <= INT64_MAX.
 * @param[out] data Writable storage for at least bytes bytes; null permitted only for zero bytes.
 * May contain a copied prefix if reading or final position restoration fails.
 * @param[in] bytes Exact byte count; zero still checks/repositions/restores the descriptor.
 * @throws std::runtime_error Invalid range, failed initial seek/read (including early EOF), or
 * failed final restoration. EINTR reads retry; already copied bytes are not rolled back.
 * @details Saves the original file position, seeks and reads in at most INT_MAX chunks, then
 * attempts to restore the saved offset even after failed reads. A restoration failure is reported,
 * so the original position is not guaranteed on failure. Does not flush a C/HDF5 writer, lock the
 * descriptor, validate source identity or advance simulation. This avoids opening another handle
 * under Windows mandatory file locking.
 */
void checkpointReadOutput(int descriptor, uint64_t offset, char* data, std::size_t bytes);
// Create exclusively, or reopen without truncation, with rename sharing.
/**
 * @brief Open a writable binary output for checkpoint restoration.
 * @param[in] path Borrowed output path in platform filesystem encoding.
 * @param[in] existing True opens an existing file without truncation;
 * false exclusively creates a new file and fails if the path exists.
 * @return Owned read/write stream, positioned at the beginning, or null on
 * open/wrapping failure. The caller must eventually fclose() a returned stream.
 * @note Windows opens with delete/rename sharing so publication may replace
 * the live file while its original stream remains open.
 */
FILE* checkpointOpenOutput(const std::filesystem::path& path, bool existing = false);
// Internal checkpoint publication. Writers remain on their old handles until
// every rename succeeds. This is not an atomic view for external readers or a
// power-loss transaction. A failed rollback retains its backup for recovery.
class CheckpointOutputPublication {
public:
    struct Output {
        std::filesystem::path staging, destination;
        std::string fresh_header;
        int original_descriptor = -1;
    };
    /**
     * @brief Validate output pairs and reserve rollback headers before publication.
     * @param[in] outputs Nonempty vector of staging/destination pairs, consumed
     * by value. Each staging path is an existing regular file; each destination
     * must still be its writable fresh_header bytes. All paths must be distinct
     * and non-aliasing. original_descriptor, when >= 0, is borrowed for identity checks.
     * @throws std::runtime_error Validation, alias checks or backup reservation fails.
     * Filesystem and allocation exceptions may propagate. Reserved backups are
     * removed if construction fails.
     * @details Reserves hard-linked rollback headers and copies destination
     * permissions to staging files. It neither closes writer handles nor takes
     * responsibility for removing staging files.
     */
    explicit CheckpointOutputPublication(std::vector<Output>);
    /**
     * @brief Remove reserved backup headers unless needed for failed rollback recovery.
     * @note Cleanup errors are ignored. Staging files and published destinations
     * remain caller-managed.
     */
    ~CheckpointOutputPublication();
    CheckpointOutputPublication(const CheckpointOutputPublication&) = delete;
    CheckpointOutputPublication& operator=(const CheckpointOutputPublication&) = delete;
    /**
     * @brief Replace all validated fresh outputs, rolling back on a rename failure.
     * @throws std::runtime_error A destination changed, publication fails or
     * rollback fails. Recovery backup paths are included in the latter diagnostic.
     * Filesystem exceptions can propagate from preflight identity checks.
     * @details Revalidates each destination immediately before the rename sequence.
     * Writers remain on their old handles. This sequence is not an atomic view
     * for external readers and provides no power-loss transaction guarantee.
     * A failed rollback retains its backup header for manual recovery.
     * @pre Call at most once after successful construction and do not concurrently
     * modify any staging, destination or reserved backup path.
     */
    void publish();

private:
    struct Entry {
        Output output;
        std::filesystem::path backup;
        bool published = false, retain = false;
    };
    std::vector<Entry> entries_;
    /**
     * @brief Best-effort removal of reserved backups not retained for recovery.
     * @note Does not throw, remove staging paths or close original descriptors.
     * Backups marked retain remain available after a failed rollback.
     */
    void cleanup() noexcept;
};
}
