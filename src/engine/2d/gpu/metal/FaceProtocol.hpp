// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

namespace openswmm::twoD::metal {

// Native, same-host protocol between the host plugin and its Metal companion.
struct FaceInput {
    float h, qhat, slope, n2, qmag, width, leftBudget, rightBudget;
};
struct StepParams {
    float dt, dry, fr;
};
struct FaceOutput {
    float q, transfer;
};
struct FaceRequest {
    std::uint32_t count;
    StepParams params;
};
constexpr int no_metal_device = 77;
constexpr std::uint32_t worker_ready = 0x53574631u;
static_assert(sizeof(FaceRequest) == 16 && sizeof(FaceInput) == 32 && sizeof(FaceOutput) == 8);

inline bool read_bytes(int fd, void* buffer, std::size_t bytes, bool allow_eof = false) {
    auto* next = static_cast<char*>(buffer);
    std::size_t remaining = bytes;
    while (remaining > 0) {
        const ssize_t n = ::read(fd, next, remaining);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            throw std::system_error(errno, std::generic_category(), "Metal worker read");
        if (n == 0) {
            if (allow_eof && remaining == bytes)
                return false;
            throw std::runtime_error("face worker disconnected before completing the response");
        }
        next += n;
        remaining -= static_cast<std::size_t>(n);
    }
    return true;
}
inline void write_bytes(int fd, const void* buffer, std::size_t bytes) {
    const auto* next = static_cast<const char*>(buffer);
    while (bytes > 0) {
        const ssize_t n = ::write(fd, next, bytes);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            throw std::system_error(n < 0 ? errno : EIO, std::generic_category(),
                                    "Metal worker write");
        next += n;
        bytes -= static_cast<std::size_t>(n);
    }
}
} // namespace openswmm::twoD::metal
