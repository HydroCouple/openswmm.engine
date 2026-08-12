// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file TimeseriesFormat.cpp
 * @brief SWMM-native timeseries text format — parser + materialiser.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "TimeseriesFormat.hpp"

#include "../../../core/DateTime.hpp"

#include <cstdio>
#include <cstring>

namespace openswmm::gpkg::formats {

FormatResult parseTimeseriesText(const std::string&          path,
                                  std::vector<TimeseriesRow>& rows) {
    std::FILE* fp = std::fopen(path.c_str(), "r");
    if (!fp) return fail("could not open '" + path + "' for reading");

    char line[512];
    while (std::fgets(line, sizeof(line), fp)) {
        // Skip comment / blank lines.
        if (line[0] == ';' || line[0] == '\n' || line[0] == '\r' || line[0] == '\0')
            continue;

        int   month = 0, day = 0, year = 0;
        int   hour  = 0, minute = 0, second = 0;
        char  date_str[32] = {};
        char  time_str[32] = {};
        double value = 0.0;

        int fields = std::sscanf(line, "%31s %31s %lf", date_str, time_str, &value);
        if (fields < 3) continue;

        if (std::sscanf(date_str, "%d/%d/%d", &month, &day, &year) != 3) continue;
        if (std::sscanf(time_str, "%d:%d:%d", &hour, &minute, &second) < 2) continue;

        TimeseriesRow row;
        row.timestamp_oa = datetime::encodeDate(year, month, day)
                         + datetime::encodeTime(hour, minute, second);
        row.value        = value;
        rows.push_back(row);
    }
    std::fclose(fp);
    return ok();
}

FormatResult writeTimeseriesText(const std::string&                path,
                                  const std::vector<TimeseriesRow>& rows) {
    std::FILE* fp = std::fopen(path.c_str(), "w");
    if (!fp) return fail("could not open '" + path + "' for writing");

    for (const auto& r : rows) {
        int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
        datetime::decodeDate(r.timestamp_oa, y, mo, d);
        datetime::decodeTime(r.timestamp_oa, h, mi, s);
        std::fprintf(fp, "%02d/%02d/%04d %02d:%02d:%02d %.6f\n",
                      mo, d, y, h, mi, s, r.value);
    }
    std::fclose(fp);
    return ok();
}

} // namespace openswmm::gpkg::formats
