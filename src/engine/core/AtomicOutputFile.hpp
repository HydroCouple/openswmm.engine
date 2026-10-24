// SPDX-License-Identifier: Apache-2.0

/**
 * @file      AtomicOutputFile.hpp
 * @brief     Checked serialization and publication of a single output file.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once

#include "FileIO.hpp"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <system_error>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace openswmm::io {

// One output only: checked serialization into an exclusively created sibling,
// followed by atomic replacement. This is not a multi-file transaction.
/**
 * @brief Own a staging stream and publish it by a checked single-file rename.
 *
 * Noncopyable; destroys unpublished staging on scope exit. Caller writes synchronously through
 * stream(), then calls commit() and inspects error() on failure. No internal thread
 * synchronization; retain the object until serialization completes.
 */
class AtomicOutputFile {
public:
    // `binary` publishes exactly the bytes written; the default text mode lets
    // Windows turn "\n" into "\r\n".
    /**
     * @brief Create an exclusive staging sibling for a single output destination.
     *
     * Refuses an existing nonregular destination or one without any write permission bits, then tries
     * up to 32 unique staging names and copies existing permissions. Filesystem/refusal diagnostics are
     * stored rather than thrown; allocation/path-conversion exceptions may still propagate. Check
     * stream() before serializing. Does not create missing parent directories.
     *
     * @param[in] destination Borrowed native path copied into the object; existing symlinks resolve to
     *     their target, dangling symlinks are refused.
     * @param[in] binary True uses binary-exclusive mode; false permits platform text newline
     *     translation.
     */
    explicit AtomicOutputFile(const std::filesystem::path& destination, bool binary = false)
        : destination_(destination) {
        namespace fs = std::filesystem;
        std::error_code ec;
        // Preserve ordinary Save-through-symlink semantics. A dangling link
        // is refused rather than silently replacing the link itself.
        if (fs::is_symlink(destination_, ec)) {
            const auto resolved = fs::canonical(destination_, ec);
            if (ec) {
                fail("resolve destination", ec);
                return;
            }
            destination_ = resolved;
        } else if (ec && ec != std::errc::no_such_file_or_directory) {
            fail("inspect destination", ec);
            return;
        }
        ec.clear();
        const auto status = fs::status(destination_, ec);
        const bool exists = fs::exists(status);
        if (ec && ec != std::errc::no_such_file_or_directory) {
            fail("inspect destination", ec);
            return;
        }
        if (exists && !fs::is_regular_file(status)) {
            fail("replace a non-regular destination",
                 std::make_error_code(std::errc::invalid_argument));
            return;
        }
        const auto writable =
            fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write;
        if (exists && (status.permissions() & writable) == fs::perms::none) {
            fail("write a read-only destination",
                 std::make_error_code(std::errc::permission_denied));
            return;
        }
        static std::atomic<unsigned long long> sequence{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 32; ++attempt) {
            const auto candidate =
                destination_.parent_path() /
                (".openswmm-save-" + std::to_string(stamp) + "-" + std::to_string(sequence++));
            file_ = fopen_path(candidate, binary ? "wbx" : "wx");
            if (file_) {
                staging_ = candidate;
                break;
            }
            if (errno != EEXIST)
                break;
        }
        if (!file_) {
            fail("create temporary output", systemError());
            return;
        }
        if (exists) {
            fs::permissions(staging_, status.permissions(), ec);
            if (ec) {
                fail("preserve destination permissions", ec);
                closeAndDiscard();
            }
        }
    }

    /**
     * @brief Close any open staging stream and best-effort discard unpublished output.
     *
     * Does not commit. Remove/close errors are ignored; a failed cleanup may leave the staging file.
     * Published destination is retained.
     */
    ~AtomicOutputFile() { closeAndDiscard(); }
    /**
     * @brief Disallow copying ownership of the staging stream and publication state.
     */
    AtomicOutputFile(const AtomicOutputFile&) = delete;
    /**
     * @brief Disallow copy replacement of a pending file publication.
     */
    AtomicOutputFile& operator=(const AtomicOutputFile&) = delete;

    /**
     * @brief Borrow the writable staging stream.
     *
     * Caller writes but must not fclose() or retain it past commit/destruction; ownership stays with
     * this object. Caller serializes all access.
     * @return Nonowning FILE pointer, or nullptr after preparation failure/commit.
     */
    FILE* stream() const { return file_; }
    /**
     * @brief Borrow the latest stored file-operation diagnostic.
     * @return Owned diagnostic reference, empty initially; copy before a later operation or
     *     destruction.
     */
    const std::string& error() const { return error_; }

    /**
     * @brief Flush, sync, close and attempt to rename staging over the destination.
     *
     * Checks ferror, fflush, platform file sync and fclose in order, closes even on failure, then uses
     * filesystem::rename. On failure error() describes the first relevant stage and unpublished staging
     * remains for destructor cleanup. After any attempt the stream is unavailable; a second commit
     * returns false. This is one-file rename publication, not a multi-file transaction or
     * parent-directory durability guarantee.
     * @return True only after successful publication; false for missing stream or an IO/rename refusal.
     */
    bool commit() {
        if (!file_)
            return false;
        bool ok = std::ferror(file_) == 0;
        if (!ok)
            fail("write output", systemError());
        if (std::fflush(file_) != 0) {
            if (ok)
                fail("flush output", systemError());
            ok = false;
        }
        if (ok) {
#ifdef _WIN32
            const int sync = ::_commit(::_fileno(file_));
#else
            int sync;
            do {
                sync = ::fsync(::fileno(file_));
            } while (sync != 0 && errno == EINTR);
#endif
            if (sync != 0) {
                fail("sync output", systemError());
                ok = false;
            }
        }
        if (std::fclose(file_) != 0) {
            if (ok)
                fail("close output", systemError());
            ok = false;
        }
        file_ = nullptr;
        if (!ok)
            return false;
        std::error_code ec;
        std::filesystem::rename(staging_, destination_, ec);
        if (ec) {
            fail("publish output", ec);
            return false;
        }
        staging_.clear();
        return true;
    }

private:
    /**
     * @brief Capture errno as a generic error or synthesize IO failure.
     *
     * Does not clear errno; callers invoke immediately after the failing C operation.
     * @return Current nonzero errno in generic_category, otherwise errc::io_error.
     */
    static std::error_code systemError() {
        return errno ? std::error_code(errno, std::generic_category())
                     : std::make_error_code(std::errc::io_error);
    }
    /**
     * @brief Replace the stored publication diagnostic.
     *
     * Includes destination path via path_utf8(); allocation/encoding exceptions propagate. Does not
     * close/discard resources itself.
     *
     * @param[in] operation Non-null borrowed ASCII operation label.
     * @param[in] ec Borrowed error code rendered into the diagnostic.
     */
    void fail(const char* operation, const std::error_code& ec) {
        error_ = "Cannot " + std::string(operation) + " '" + path_utf8(destination_) +
                 "': " + ec.message();
    }
    /**
     * @brief Best-effort close staging and remove its current filesystem path.
     *
     * Nulls file_ after fclose and removes nonempty staging_ with error_code suppression. Does not
     * clear staging_, so repeated cleanup can retry a previously failed removal. Never
     * renames/publishes.
     */
    void closeAndDiscard() {
        if (file_) {
            std::fclose(file_);
            file_ = nullptr;
        }
        if (!staging_.empty()) {
            std::error_code ec;
            std::filesystem::remove(staging_, ec);
        }
    }
    std::filesystem::path destination_, staging_;
    FILE* file_ = nullptr;
    std::string error_;
};
} // namespace openswmm::io
