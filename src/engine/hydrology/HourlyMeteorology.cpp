// SPDX-License-Identifier: Apache-2.0
#include "HourlyMeteorology.hpp"
#include "../input/InputParseUtils.hpp"
#include "../input/Tokenizer.hpp"
#include "../core/DateTime.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <cstdio>

namespace openswmm::climate {
std::string HourlyMeteorology::load(const std::string& path) {
    std::ifstream stream(path);
    if (!stream) return "Cannot open hourly meteorology file: " + path;
    return read(stream);
}
std::string HourlyMeteorology::read(std::istream& stream) {
    std::vector<HourlyMetRecord> candidate;
    std::string line; int line_number = 0;
    while (std::getline(stream, line)) {
        ++line_number;
        const auto tok = input::Tokenizer::tokenize(line);
        if (tok.empty()) continue;
        const auto error = [&] { return "Invalid hourly meteorology at line " + std::to_string(line_number) +
            ": expected MM/DD/YYYY HH:00 temperature_C RH_percent wind_m/s shortwave_W/m2 longwave_W/m2."; };
        if (tok.size() != 7) return error();
        int m = 0, d = 0, y = 0, h = 0, minute = 0, end = 0;
        if (std::sscanf(tok[0].c_str(), "%d/%d/%d%n", &m, &d, &y, &end) != 3 || end != int(tok[0].size())) return error();
        end = 0;
        if (std::sscanf(tok[1].c_str(), "%d:%d%n", &h, &minute, &end) != 2 || end != int(tok[1].size()) ||
            h < 0 || h > 23 || minute != 0) return error();
        const double day = datetime::encodeDate(y, m, d);
        if (day <= 0) return error();
        HourlyMetRecord record; record.date = day + h / 24.;
        for (int i = 0; i < 5; ++i)
            if (!input::parse_double_strict(tok[i + 2], record.values[i]) || !std::isfinite(record.values[i])) return error();
        const auto& v = record.values;
        if (v[0] <= -100 || v[0] > 70 || v[1] < 0 || v[1] > 100 || v[2] < 0 || v[3] < 0 || v[4] < 0) return error();
        if (!candidate.empty() && std::abs((record.date - candidate.back().date) * 86400. - 3600.) > 1e-5)
            return "Hourly meteorology must have consecutive, unique hourly records; gap or duplicate at line " + std::to_string(line_number) + ".";
        candidate.push_back(record);
    }
    if (stream.bad()) return "Could not read hourly meteorology.";
    if (candidate.empty()) return "Hourly meteorology contains no records.";
    records_ = std::move(candidate); return {};
}
std::string HourlyMeteorology::average(double start, double seconds, std::array<double, 5>& result) const {
    if (!std::isfinite(start) || !std::isfinite(seconds) || seconds <= 0 || records_.empty()) return "Invalid hourly meteorology request.";
    constexpr double tolerance = 1e-5 / 86400.;
    const double finish = start + seconds / 86400.;
    if (!std::isfinite(finish) || start < records_.front().date - tolerance || finish > records_.back().date + 1./24 + tolerance)
        return "Hourly meteorology does not cover the requested simulation interval.";
    auto it = std::upper_bound(records_.begin(), records_.end(), start + tolerance,
        [](double date, const HourlyMetRecord& r) { return date < r.date; });
    if (it != records_.begin()) --it;
    std::array<double, 5> sum{}; double elapsed = 0;
    for (; it != records_.end() && it->date < finish; ++it) {
        const double duration = std::max(0., (std::min(finish, it->date + 1./24) - std::max(start, it->date)) * 86400.);
        for (int i = 0; i < 5; ++i) sum[i] += duration * it->values[i];
        elapsed += duration;
    }
    if (std::abs(elapsed - seconds) > 1e-4) return "Hourly meteorology interval has incomplete coverage.";
    // Normalize by covered duration to avoid day-number cancellation drift.
    for (auto& v : sum) v /= elapsed;
    result = sum; return {};
}
} // namespace openswmm::climate
