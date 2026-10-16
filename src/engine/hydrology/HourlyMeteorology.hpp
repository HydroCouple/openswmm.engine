// SPDX-License-Identifier: Apache-2.0
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
    double date=0,tmin=70,tmax=70,previous_max=70,wind=0;
};
class HourlyMeteorology {
public:
    std::string read(std::istream&);
    std::string load(const std::string& path);
    std::string average(double start, double seconds, std::array<double, 5>& result) const;
    bool empty() const noexcept { return records_.empty(); }
    const std::vector<HourlyMetRecord>& records() const noexcept { return records_; }
private:
    std::vector<HourlyMetRecord> records_;
};
} // namespace openswmm::climate
