// SPDX-License-Identifier: Apache-2.0
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
class AtomicOutputFile {
public:
    explicit AtomicOutputFile(const std::filesystem::path& destination)
        : destination_(destination)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        // Preserve ordinary Save-through-symlink semantics. A dangling link
        // is refused rather than silently replacing the link itself.
        if (fs::is_symlink(destination_, ec)) {
            const auto resolved = fs::canonical(destination_, ec);
            if (ec) { fail("resolve destination", ec); return; }
            destination_ = resolved;
        } else if (ec && ec != std::errc::no_such_file_or_directory) {
            fail("inspect destination", ec); return;
        }
        ec.clear();
        const auto status = fs::status(destination_, ec);
        const bool exists = fs::exists(status);
        if (ec && ec != std::errc::no_such_file_or_directory) {
            fail("inspect destination", ec); return;
        }
        if (exists && !fs::is_regular_file(status)) {
            fail("replace a non-regular destination", std::make_error_code(std::errc::invalid_argument));
            return;
        }
        const auto writable = fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write;
        if (exists && (status.permissions() & writable) == fs::perms::none) {
            fail("write a read-only destination", std::make_error_code(std::errc::permission_denied));
            return;
        }
        static std::atomic<unsigned long long> sequence{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 32; ++attempt) {
            const auto candidate = destination_.parent_path() /
                (".openswmm-save-" + std::to_string(stamp) + "-" + std::to_string(sequence++));
            file_ = fopen_path(candidate, "wx");
            if (file_) { staging_ = candidate; break; }
            if (errno != EEXIST) break;
        }
        if (!file_) { fail("create temporary output", systemError()); return; }
        if (exists) {
            fs::permissions(staging_, status.permissions(), ec);
            if (ec) { fail("preserve destination permissions", ec); closeAndDiscard(); }
        }
    }

    ~AtomicOutputFile() { closeAndDiscard(); }
    AtomicOutputFile(const AtomicOutputFile&) = delete;
    AtomicOutputFile& operator=(const AtomicOutputFile&) = delete;

    FILE* stream() const { return file_; }
    const std::string& error() const { return error_; }

    bool commit()
    {
        if (!file_) return false;
        bool ok = std::ferror(file_) == 0;
        if (!ok) fail("write output", systemError());
        if (std::fflush(file_) != 0) {
            if (ok) fail("flush output", systemError());
            ok = false;
        }
        if (ok) {
#ifdef _WIN32
            const int sync = ::_commit(::_fileno(file_));
#else
            int sync;
            do { sync = ::fsync(::fileno(file_)); } while (sync != 0 && errno == EINTR);
#endif
            if (sync != 0) { fail("sync output", systemError()); ok = false; }
        }
        if (std::fclose(file_) != 0) {
            if (ok) fail("close output", systemError());
            ok = false;
        }
        file_ = nullptr;
        if (!ok) return false;
        std::error_code ec;
        std::filesystem::rename(staging_, destination_, ec);
        if (ec) { fail("publish output", ec); return false; }
        staging_.clear();
        return true;
    }

private:
    static std::error_code systemError()
    {
        return errno ? std::error_code(errno, std::generic_category())
                     : std::make_error_code(std::errc::io_error);
    }
    void fail(const char* operation, const std::error_code& ec)
    {
        error_ = "Cannot " + std::string(operation) + " '" + path_utf8(destination_) + "': " + ec.message();
    }
    void closeAndDiscard()
    {
        if (file_) { std::fclose(file_); file_ = nullptr; }
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
