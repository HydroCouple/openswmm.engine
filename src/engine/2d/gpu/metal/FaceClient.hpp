// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <fcntl.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <vector>
#include <crt_externs.h>

#include <dlfcn.h>
#include <filesystem>
#include "FaceProtocol.hpp"

namespace openswmm::twoD::metal {
// A separate process owns the experimental Kokkos runtime. The original
// engine plugin keeps its pinned Kokkos ABI and its FP64 cell and source ledgers.
class FaceClient {
    int channel_ = -1;
    pid_t child_ = -1;

    static void check(int error, const char* operation) {
        if (error)
            throw std::system_error(error, std::generic_category(), operation);
    }
    int close_worker() noexcept {
        int status = -1;
        if (channel_ >= 0)
            ::close(channel_);
        if (child_ > 0) {
            pid_t result;
            do {
                result = ::waitpid(child_, &status, 0);
            } while (result < 0 && errno == EINTR);
            if (result < 0)
                status = -1;
        }
        child_ = -1;
        channel_ = -1;
        return status;
    }

public:
    std::vector<FaceInput> inputs;
    std::vector<FaceOutput> outputs;

    FaceClient() {
        static int plugin_address;
        Dl_info location{};
        if (!::dladdr(&plugin_address, &location) || !location.dli_fname)
            throw std::runtime_error("cannot locate the OpenSWMM plugin for the Metal worker");
        const auto executable =
            std::filesystem::absolute(location.dli_fname).parent_path() / "openswmm_metal_faces";
        int sockets[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
            throw std::system_error(errno, std::generic_category(), "face socketpair");
        channel_ = sockets[0];
        posix_spawn_file_actions_t actions;
        bool initialized = false;
        try {
            int on = 1;
            for (const auto fd : sockets) {
                if (::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 ||
                    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)) < 0)
                    throw std::system_error(errno, std::generic_category(),
                                            "configure face socket");
            }
            check(posix_spawn_file_actions_init(&actions), "initialize spawn");
            initialized = true;
            check(posix_spawn_file_actions_addclose(&actions, channel_),
                  "close parent socket in child");
            check(posix_spawn_file_actions_adddup2(&actions, sockets[1], STDIN_FILENO),
                  "attach face socket");
            char* argv[] = {const_cast<char*>(executable.c_str()), nullptr};
            pid_t pid;
            check(posix_spawn(&pid, executable.c_str(), &actions, nullptr, argv, *_NSGetEnviron()),
                  "spawn face worker");
            child_ = pid;
            posix_spawn_file_actions_destroy(&actions);
            initialized = false;
            ::close(sockets[1]);
            sockets[1] = -1;
            std::uint32_t ready;
            if (!read_bytes(channel_, &ready, sizeof(ready), true)) {
                const int status = close_worker();
                if (status >= 0 && WIFEXITED(status) && WEXITSTATUS(status) == no_metal_device)
                    throw std::runtime_error("no Metal GPU available for OpenSWMM face flow");
                throw std::runtime_error("Metal face worker exited during startup");
            }
            if (ready != worker_ready)
                throw std::runtime_error("unexpected face protocol version");
        } catch (...) {
            if (initialized)
                posix_spawn_file_actions_destroy(&actions);
            if (sockets[1] >= 0)
                ::close(sockets[1]);
            close_worker();
            throw;
        }
    }
    FaceClient(const FaceClient&) = delete;
    FaceClient& operator=(const FaceClient&) = delete;
    ~FaceClient() { close_worker(); }

    void run(const StepParams params) {
        const FaceRequest req{static_cast<std::uint32_t>(inputs.size()), params};
        outputs.resize(req.count);
        write_bytes(channel_, &req, sizeof(req));
        write_bytes(channel_, inputs.data(), inputs.size() * sizeof(FaceInput));
        read_bytes(channel_, outputs.data(), outputs.size() * sizeof(FaceOutput));
    }
};
}
