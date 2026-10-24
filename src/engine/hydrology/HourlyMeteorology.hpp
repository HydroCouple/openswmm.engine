// SPDX-License-Identifier: Apache-2.0

/**
 * @file      HourlyMeteorology.hpp
 * @brief     Preparation and interpolation of hourly meteorological forcing.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include <array>
#include <istream>
#include <string>
#include <vector>

namespace openswmm::climate {
// Date/time marks the START of the hourly interval. Temperatures are °C,
// humidity is percent, wind m/s and radiation W/m². All fields are required.
// A terminal record represents one additional complete hour.
struct HourlyMetRecord {
    double date = 0;
    std::array<double, 5> values{}; // temperature, RH, wind, Qsi, Qli
};
// Immutable daily source cache for independent surface clocks. Native
// climate-file cursors can run ahead of a mesh/source consumer.
struct SnowDailyWeather {
    double date = 0, tmin = 70, tmax = 70, previous_max = 70, wind = 0;
};
/** @brief Owned consecutive hourly records with interval-weighted means.
 * Concurrent const queries are safe only while the cache is stable; serialize
 * load/read/destruction with readers and release borrowed records before reload. */
class HourlyMeteorology {
public:
    /** @brief Replace the cache with validated consecutive hourly records from a stream.
     * @param[in,out] stream Borrowed text input consumed through EOF or first rejection;
     * never closed/rewound. Lines follow Input Tokenizer's comment/whitespace rules.
     * Each nonempty record has exactly date, HH:00 and five numeric weather fields.
     * @return Empty on success; otherwise line/coverage/read diagnostic retaining the
     * previous cache. Requires valid positive SWMM calendar days; hour [0,23], minute
     * zero, finite temperature (-100,70] C, RH [0,100] percent, and nonnegative wind
     * m/s and radiation W/m2. Adjacent starts differ by 3600 seconds within 1e-5 s.
     * Empty input is rejected. Standard stream exceptions/allocation failures propagate. */
    std::string read(std::istream& stream);
    /** @brief Open a text weather file and replace the cache through read().
     * @param path Filename passed to std::ifstream; no input-relative resolution here.
     * @return Empty on successful parse; open/parse/read diagnostic otherwise. Old
     * records remain on normal failure. The temporary stream is closed on return;
     * subsequent average() calls read only memory. Exceptions can propagate. */
    std::string load(const std::string& path);
    /** @brief Compute duration-weighted means of hourly piecewise-constant weather.
     * @param start Finite absolute SWMM interval-start day within cached coverage.
     * @param seconds Finite positive duration seconds; can span multiple days. The last
     * record supplies a complete additional hour, rather than only an endpoint sample.
     * @param[out] result Five means in record order/units (C, percent, m/s, W/m2, W/m2),
     * assigned only on success and retained on normal failure.
     * @return Empty on success, otherwise invalid-request/coverage diagnostic. Uses
     * second offsets to avoid adding tiny durations to large serial-day values; snaps
     * near-hour boundaries within 1e-5 s and tolerates that much missing endpoint span.
     * Normalizes by actually covered duration, not requested duration. No file access,
     * interpolation, record mutation or simulation-clock advancement occurs. */
    std::string average(double start, double seconds, std::array<double, 5>& result) const;
    /** @brief Test whether any validated records are stored.
     * @return True before the first successful read/load or after replacement by a
     * fresh default object. Failed reads retain the old cache and its empty state. */
    bool empty() const noexcept { return records_.empty(); }
    /** @brief Borrow immutable hourly starts and their complete-hour weather values.
     * @return Owned vector reference valid until destruction. A successful reload
     * replaces storage and invalidates element references/iterators; failed reloads
     * retain it. Caller serializes replacement and copies data that must outlive it. */
    const std::vector<HourlyMetRecord>& records() const noexcept { return records_; }

private:
    std::vector<HourlyMetRecord> records_;
};
} // namespace openswmm::climate
