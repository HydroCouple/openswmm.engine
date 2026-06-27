/**
 * @file ClimateFormat.cpp
 * @brief User-CSV climate file — parser + materialiser.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "ClimateFormat.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

namespace openswmm::gpkg::formats {

namespace {

// Trim ASCII whitespace and CR/LF in place.
void trim(std::string& s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
}

std::vector<std::string> splitCsv(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (c == ',') {
            trim(cur);
            out.push_back(std::move(cur));
            cur.clear();
        } else if (c != '\r' && c != '\n') {
            cur.push_back(c);
        }
    }
    trim(cur);
    out.push_back(std::move(cur));
    return out;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

double parseOrZero(const std::string& s) {
    if (s.empty()) return 0.0;
    try { return std::stod(s); } catch (...) { return 0.0; }
}

} // namespace

FormatResult parseClimateCsv(const std::string&        path,
                              std::vector<ClimateRow>&  rows) {
    std::FILE* fp = std::fopen(path.c_str(), "r");
    if (!fp) return fail("could not open '" + path + "' for reading");

    char buf[2048];
    bool got_header = false;
    std::unordered_map<std::string, int> col;

    while (std::fgets(buf, sizeof(buf), fp)) {
        std::string line(buf);
        trim(line);
        if (line.empty() || line[0] == '#') continue;

        auto cells = splitCsv(line);
        if (!got_header) {
            for (std::size_t i = 0; i < cells.size(); ++i)
                col[lower(cells[i])] = static_cast<int>(i);
            // Minimum requirement: a date column.
            if (col.find("date") == col.end()) {
                std::fclose(fp);
                return fail("climate CSV missing required 'date' column header");
            }
            got_header = true;
            continue;
        }

        ClimateRow r;
        auto get = [&](const char* name) -> std::string {
            auto it = col.find(name);
            if (it == col.end() || it->second >= static_cast<int>(cells.size()))
                return {};
            return cells[static_cast<std::size_t>(it->second)];
        };
        r.record_date  = get("date");
        r.tmin         = parseOrZero(get("tmin"));
        r.tmax         = parseOrZero(get("tmax"));
        r.evaporation  = parseOrZero(get("evap"));
        r.wind_speed   = parseOrZero(get("wind"));
        r.sky_cover    = parseOrZero(get("sky"));
        r.humidity     = parseOrZero(get("humidity"));
        if (r.record_date.empty()) continue;
        rows.push_back(std::move(r));
    }
    std::fclose(fp);
    return ok();
}

FormatResult writeClimateCsv(const std::string&             path,
                              const std::vector<ClimateRow>& rows) {
    std::FILE* fp = std::fopen(path.c_str(), "w");
    if (!fp) return fail("could not open '" + path + "' for writing");
    std::fprintf(fp, "date,tmin,tmax,evap,wind,sky,humidity\n");
    for (const auto& r : rows) {
        std::fprintf(fp, "%s,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n",
                      r.record_date.c_str(),
                      r.tmin, r.tmax, r.evaporation,
                      r.wind_speed, r.sky_cover, r.humidity);
    }
    std::fclose(fp);
    return ok();
}

} // namespace openswmm::gpkg::formats
