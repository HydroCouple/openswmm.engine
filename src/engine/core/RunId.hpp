// SPDX-License-Identifier: Apache-2.0
/**
 * @file      RunId.hpp
 * @brief     Identifier of one simulation execution.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <random>
#include <string>

namespace openswmm {

/**
 * @brief Create the identifier of one execution, such as run_20261119T120000Z_3fa9c2d1.
 *
 * The UTC wall-clock second orders runs for a reader; 32 random bits keep two runs started in the
 * same second apart. Every writer of one execution uses the same identifier.
 *
 * @return New identifier made of ASCII letters, digits and underscores.
 */
inline std::string newRunId() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    std::random_device device;
    const std::uint32_t tag = (static_cast<std::uint32_t>(device()) << 16) ^ device();
    char text[48];
    std::snprintf(text, sizeof text, "run_%04d%02d%02dT%02d%02d%02dZ_%08x", utc.tm_year + 1900,
                  utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec,
                  static_cast<unsigned>(tag));
    return text;
}

} // namespace openswmm
