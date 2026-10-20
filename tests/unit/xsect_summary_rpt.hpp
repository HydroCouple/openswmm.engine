// SPDX-License-Identifier: Apache-2.0
/**
 * @file xsect_summary_rpt.hpp
 * @brief Rows of a report's "Cross Section Summary" table, by conduit name.
 *
 * Shared by the cross-section regression tests under unit/engine and
 * unit/legacy/engine: both engines print the same table (full depth, full
 * area, hydraulic radius, max. width, number of barrels, full flow), and for
 * several of the quantities the report is the only place either API exposes
 * them. Values carry the report's two decimals, so compare to 0.01.
 */
#pragma once

#include <cstdio>
#include <cstring>
#include <map>
#include <string>

struct XsectSummaryRow {
    double depth = 0, area = 0, rfull = 0, wmax = 0, qfull = 0;
    int barrels = 0;
};

/// The table's rows keyed by conduit name; empty when the report has none.
inline std::map<std::string, XsectSummaryRow> readXsectSummary(const std::string& rpt) {
    std::map<std::string, XsectSummaryRow> rows;
    FILE* f = std::fopen(rpt.c_str(), "r");
    if (!f) return rows;
    char line[512], name[64], shape[64];
    bool in = false;
    while (std::fgets(line, sizeof line, f)) {
        if (std::strstr(line, "Cross Section Summary")) { in = true; continue; }
        if (!in) continue;
        if (std::strstr(line, "****") && !rows.empty()) break;  // next section's banner
        XsectSummaryRow r;
        const int n = std::sscanf(line, "%63s %63s %lf %lf %lf %lf %d %lf", name, shape,
                                  &r.depth, &r.area, &r.rfull, &r.wmax, &r.barrels, &r.qfull);
        if (n >= 6) rows[name] = r;
    }
    std::fclose(f);
    return rows;
}
