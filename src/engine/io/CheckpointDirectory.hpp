// SPDX-License-Identifier: Apache-2.0

/**
 * @file      CheckpointDirectory.hpp
 * @brief     Checkpoint directory layout, validation, and file management.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include <filesystem>

namespace openswmm::io {
/**
 * @brief Report compile-time support for exclusive checkpoint publication.
 * @return True for Windows, macOS, or Linux builds exposing SYS_renameat2;
 * false otherwise. This does not probe runtime filesystem support.
 */
bool checkpointNoReplaceAvailable() noexcept;
/// Atomically publish a private file/directory without replacing any destination.
/**
 * @brief Atomically rename a staged checkpoint without replacing a destination.
 * @param[in] staging Existing private file or directory to move; must be
 * on a filesystem where the platform's no-replace rename can succeed.
 * @param[in] destination Final path, which must not already exist.
 * @throws std::runtime_error Publication fails or no supported platform
 * primitive is compiled in. Paths remain caller-owned.
 * @note The platform operation protects even an existing empty directory.
 * No copy fallback or cross-filesystem publication is performed.
 */
void publishCheckpointNoReplace(const std::filesystem::path& staging,
                                const std::filesystem::path& destination);
/// Own an exclusively created private directory. Publish the complete bundle
/// without replacing an existing destination, including an empty directory.
class CheckpointDirectory {
public:
    /**
     * @brief Reserve an exclusive sibling directory for a checkpoint bundle.
     * @param[in] destination Final bundle path, which must not already exist.
     * The parent must exist and allow creation of a sibling staging directory.
     * @throws std::runtime_error The destination exists or 32 reservation attempts fail.
     * @throws std::filesystem::filesystem_error Filesystem inspection or creation fails.
     * @details The instance owns its newly created staging directory until
     * publish() succeeds; destruction recursively removes an unpublished bundle.
     */
    explicit CheckpointDirectory(std::filesystem::path destination);
    /**
     * @brief Recursively remove the owned staging directory if not published.
     * @note Cleanup uses error_code and ignores removal failures. A successfully
     * published destination is never removed by this destructor.
     */
    ~CheckpointDirectory();
    CheckpointDirectory(const CheckpointDirectory&) = delete;
    CheckpointDirectory& operator=(const CheckpointDirectory&) = delete;
    /**
     * @brief Expose the reserved private directory for writing bundle contents.
     * @return Borrowed path valid for this object's lifetime. After publish(),
     * this old staging name no longer identifies the published directory.
     */
    const std::filesystem::path& staging() const { return staging_; }
    /**
     * @brief Publish the complete reserved bundle by exclusive rename.
     * @throws std::runtime_error Staging ownership was already released or
     * exclusive publication fails. Filesystem failures may also propagate.
     * @post On success the instance releases cleanup ownership. On failure it
     * continues owning the staging tree so destruction can clean it up.
     */
    void publish();

private:
    std::filesystem::path destination_, staging_;
    bool owned_ = false;
};
}
