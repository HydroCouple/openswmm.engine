/**
 * @file RaingageFormat.cpp
 * @brief SWMM "Standard" raingage text format — parser + materialiser.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "RaingageFormat.hpp"

#include "../../../core/DateTime.hpp"

#include <cstdio>
#include <cstring>

namespace openswmm::gpkg::formats {

FormatResult parseRaingageStd(const std::string&         path,
                               std::vector<RaingageRow>&  rows) {
    std::FILE* fp = std::fopen(path.c_str(), "r");
    if (!fp) return fail("could not open '" + path + "' for reading");

    char line[512];
    while (std::fgets(line, sizeof(line), fp)) {
        if (line[0] == ';' || line[0] == '\n' || line[0] == '\r' || line[0] == '\0')
            continue;

        char  station[64] = {};
        int   y = 0, mo = 0, d = 0, h = 0, mi = 0;
        double value = 0.0;
        int fields = std::sscanf(line, "%63s %d %d %d %d %d %lf",
                                  station, &y, &mo, &d, &h, &mi, &value);
        if (fields < 7) continue;

        RaingageRow r;
        r.station_id   = station;
        r.timestamp_oa = datetime::encodeDate(y, mo, d)
                       + datetime::encodeTime(h, mi, 0);
        r.rainfall     = value;
        rows.push_back(std::move(r));
    }
    std::fclose(fp);
    return ok();
}

FormatResult writeRaingageStd(const std::string&              path,
                               const std::vector<RaingageRow>& rows) {
    std::FILE* fp = std::fopen(path.c_str(), "w");
    if (!fp) return fail("could not open '" + path + "' for writing");

    for (const auto& r : rows) {
        int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
        datetime::decodeDate(r.timestamp_oa, y, mo, d);
        datetime::decodeTime(r.timestamp_oa, h, mi, s);
        std::fprintf(fp, "%-12s %4d %02d %02d %02d %02d  %.4f\n",
                      r.station_id.c_str(), y, mo, d, h, mi, r.rainfall);
    }
    std::fclose(fp);
    return ok();
}

} // namespace openswmm::gpkg::formats
