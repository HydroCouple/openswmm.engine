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
 * @file RoutingInterfaceFormat.cpp
 * @brief SWMM5 routing-interface text format — parser + materialiser.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "RoutingInterfaceFormat.hpp"

#include "../../../core/DateTime.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

namespace openswmm::gpkg::formats {

namespace {

void rtrim(std::string& s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'
                          || s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
}

// Read the leading integer from a header line like "  60   - reporting time step".
// Returns -1 if no integer was found.
int leadingInt(const std::string& s) {
    int v = 0;
    if (std::sscanf(s.c_str(), "%d", &v) != 1) return -1;
    return v;
}

std::vector<std::string> tokenize(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream is(s);
    std::string tok;
    while (is >> tok) out.push_back(std::move(tok));
    return out;
}

} // namespace

FormatResult parseRoutingInterfaceText(
    const std::string&                path,
    RoutingInterfaceMetadata&         meta,
    std::vector<RoutingInterfaceRow>& rows) {

    std::FILE* fp = std::fopen(path.c_str(), "r");
    if (!fp) return fail("could not open '" + path + "' for reading");

    char buf[1024];

    auto readLine = [&](std::string& dst) -> bool {
        if (!std::fgets(buf, sizeof(buf), fp)) return false;
        dst.assign(buf);
        rtrim(dst);
        return true;
    };

    std::string line;

    // Line 1: "SWMM5 Interface File" — signature.
    if (!readLine(line)) { std::fclose(fp); return fail("empty file"); }
    if (line.find("Interface File") == std::string::npos) {
        std::fclose(fp);
        return fail("missing 'SWMM5 Interface File' signature");
    }

    // Line 2: title.
    if (!readLine(meta.title)) { std::fclose(fp); return fail("missing title"); }

    // Line 3: "<step> - reporting time step in sec".
    if (!readLine(line)) { std::fclose(fp); return fail("missing step line"); }
    meta.report_step_sec = leadingInt(line);
    if (meta.report_step_sec < 0) {
        std::fclose(fp);
        return fail("could not parse reporting time step");
    }

    // Line 4: "<N+1> - number of constituents..."
    if (!readLine(line)) { std::fclose(fp); return fail("missing constituent count"); }
    int n_constituents = leadingInt(line);
    if (n_constituents < 1) {
        std::fclose(fp);
        return fail("invalid constituent count");
    }

    // Next: FLOW units, then each pollutant "<id> <units>".
    if (!readLine(line)) { std::fclose(fp); return fail("missing FLOW line"); }
    {
        auto t = tokenize(line);
        if (t.size() < 2 || t[0] != "FLOW") {
            std::fclose(fp);
            return fail("expected 'FLOW <units>' line");
        }
        meta.flow_units = t[1];
    }
    meta.pollutant_ids.clear();
    meta.pollutant_units.clear();
    for (int i = 1; i < n_constituents; ++i) {
        if (!readLine(line)) {
            std::fclose(fp);
            return fail("missing pollutant declaration");
        }
        auto t = tokenize(line);
        if (t.size() < 2) {
            std::fclose(fp);
            return fail("malformed pollutant declaration");
        }
        meta.pollutant_ids.push_back(t[0]);
        meta.pollutant_units.push_back(t[1]);
    }

    // "<M> - number of nodes..."
    if (!readLine(line)) { std::fclose(fp); return fail("missing node count"); }
    int n_nodes = leadingInt(line);
    if (n_nodes < 0) {
        std::fclose(fp);
        return fail("invalid node count");
    }

    meta.object_ids.clear();
    meta.object_ids.reserve(static_cast<std::size_t>(n_nodes));
    for (int i = 0; i < n_nodes; ++i) {
        if (!readLine(line)) {
            std::fclose(fp);
            return fail("missing node id");
        }
        auto t = tokenize(line);
        if (t.empty()) continue;
        meta.object_ids.push_back(t[0]);
    }

    // Column header line — skip.
    if (!readLine(line)) { std::fclose(fp); return ok(); }  // no data rows

    // Data rows: <id> <y> <mo> <d> <h> <mi> <s> <flow> <pollut1> ...
    const std::size_t expected_pollut = meta.pollutant_ids.size();
    while (readLine(line)) {
        if (line.empty()) continue;
        auto t = tokenize(line);
        if (t.size() < 8) continue;  // need id + 6 date parts + flow

        RoutingInterfaceRow row;
        row.object_id = t[0];
        int y  = std::atoi(t[1].c_str());
        int mo = std::atoi(t[2].c_str());
        int d  = std::atoi(t[3].c_str());
        int h  = std::atoi(t[4].c_str());
        int mi = std::atoi(t[5].c_str());
        int s  = std::atoi(t[6].c_str());
        row.timestamp_oa = datetime::encodeDate(y, mo, d)
                         + datetime::encodeTime(h, mi, s);
        row.flow_value   = std::atof(t[7].c_str());

        row.pollutant_values.reserve(expected_pollut);
        for (std::size_t i = 0; i < expected_pollut; ++i) {
            const std::size_t col = 8 + i;
            row.pollutant_values.push_back(
                col < t.size() ? std::atof(t[col].c_str()) : 0.0);
        }
        rows.push_back(std::move(row));
    }

    std::fclose(fp);
    return ok();
}

FormatResult writeRoutingInterfaceText(
    const std::string&                      path,
    const RoutingInterfaceMetadata&         meta,
    const std::vector<RoutingInterfaceRow>& rows) {

    std::FILE* fp = std::fopen(path.c_str(), "w");
    if (!fp) return fail("could not open '" + path + "' for writing");

    std::fprintf(fp, "SWMM5 Interface File");
    std::fprintf(fp, "\n%s", meta.title.c_str());
    std::fprintf(fp, "\n%-4d - reporting time step in sec",
                  meta.report_step_sec);

    const int n_const = 1 + static_cast<int>(meta.pollutant_ids.size());
    std::fprintf(fp, "\n%-4d - number of constituents as listed below:",
                  n_const);
    std::fprintf(fp, "\nFLOW %s", meta.flow_units.c_str());
    for (std::size_t i = 0; i < meta.pollutant_ids.size(); ++i) {
        std::fprintf(fp, "\n%s %s",
                      meta.pollutant_ids[i].c_str(),
                      i < meta.pollutant_units.size()
                          ? meta.pollutant_units[i].c_str() : "");
    }

    std::fprintf(fp, "\n%-4d - number of nodes as listed below:",
                  static_cast<int>(meta.object_ids.size()));
    for (const auto& id : meta.object_ids)
        std::fprintf(fp, "\n%s", id.c_str());

    // Column header.
    std::fprintf(fp,
        "\nNode             Year Mon Day Hr  Min Sec FLOW      ");
    for (const auto& p : meta.pollutant_ids)
        std::fprintf(fp, " %-10s", p.c_str());

    for (const auto& r : rows) {
        int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
        datetime::decodeDate(r.timestamp_oa, y, mo, d);
        datetime::decodeTime(r.timestamp_oa, h, mi, s);
        std::fprintf(fp, "\n%-16s %04d %02d  %02d  %02d  %02d  %02d  %-10f",
                      r.object_id.c_str(), y, mo, d, h, mi, s,
                      r.flow_value);
        for (double v : r.pollutant_values)
            std::fprintf(fp, " %-10f", v);
    }
    std::fprintf(fp, "\n");

    std::fclose(fp);
    return ok();
}

} // namespace openswmm::gpkg::formats
