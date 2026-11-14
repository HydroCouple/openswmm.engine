// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin

/**
 * @file      test_report_tables.cpp
 * @brief     The end-of-run summary tables as data (swmm_summary_*, review item G-08).
 *
 * @details Each deck runs to ENDED; every value of every table is read through the C API
 *          (before and after swmm_engine_report(), which is when the LID water balance reaches
 *          the report) and compared with what the run's own .rpt prints, to the printed
 *          precision. The report writer and the API share one builder per table, so a wrong
 *          expression inside a builder moves both; the IndependentOracle test checks a subset
 *          against the raw statistics getters and the deck's known layout instead.
 *
 *          Inputs and outputs go to report_tables/ in the test's working directory.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_statistics.h>
#include <openswmm/engine/openswmm_subcatchments.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

const char* const kDir = "report_tables";

// Every table has rows: LIDs, three pollutants (mass, micro-mass, counts), a storage unit, a
// curve pump and an ideal pump, bottom and side orifices, a weir, an outlet (DUMMY), an
// irregular channel (CHANNEL), flooding, surcharge, a dry junction (balance printed as a
// volume) and maxima on the second day after a report start 30 minutes into the run.
const char* const kUsDeck = R"([TITLE]
G-08 report tables: every summary table, US units

[OPTIONS]
FLOW_UNITS CFS
INFILTRATION HORTON
FLOW_ROUTING DYNWAVE
START_DATE 06/01/2026
START_TIME 00:00:00
REPORT_START_DATE 06/01/2026
REPORT_START_TIME 00:30:00
END_DATE 06/02/2026
END_TIME 03:00:00
WET_STEP 00:01:00
DRY_STEP 00:05:00
REPORT_STEP 00:05:00
ROUTING_STEP 5
THREADS 1

[RAINGAGES]
RG1 INTENSITY 0:30 1.0 TIMESERIES RAIN

[SUBCATCHMENTS]
S1 RG1 J1 10 60 800 1.0 0
S2 RG1 J3 8 70 600 1.0 0
S3 RG1 ST1 5 50 400 1.0 0

[SUBAREAS]
S1 0.013 0.1 0.05 0.1 25 OUTLET
S2 0.013 0.1 0.05 0.1 25 OUTLET
S3 0.013 0.1 0.05 0.1 25 OUTLET

[INFILTRATION]
S1 3.0 0.5 4 7 0
S2 3.0 0.5 4 7 0
S3 3.0 0.5 4 7 0

[LID_CONTROLS]
BC1 BC
BC1 SURFACE 6.0 0.0 0.1 1.0 5
BC1 SOIL 18 0.5 0.2 0.1 5 10 3.5
BC1 STORAGE 12 0.75 0.5 0
BC1 DRAIN 0.5 0.5 6 6
RB1 RB
RB1 STORAGE 48 0.75 0 0
RB1 DRAIN 1.0 0.5 0 6

[LID_USAGE]
S1 BC1 2 500 10 0 30 0
S1 RB1 4 12 0 0 20 0
S2 BC1 1 800 20 0 25 0

[POLLUTANTS]
TSS MG/L 0 0 0 0 NO * 0 0 0
LEAD UG/L 0 0 0 0 NO * 0 0 0
FC #/L 0 0 0 0 NO * 0 0 0

[LANDUSES]
URBAN 0 0 0

[COVERAGES]
S1 URBAN 100
S2 URBAN 100
S3 URBAN 100

[BUILDUP]
URBAN TSS POW 50 0.5 2 AREA
URBAN LEAD POW 5 0.5 2 AREA
URBAN FC POW 1000000 0.5 2 AREA

[WASHOFF]
URBAN TSS EXP 0.1 1.5 0 0
URBAN LEAD EXP 0.1 1.5 0 0
URBAN FC EXP 0.1 1.5 0 0

[JUNCTIONS]
J1 100 6 0 0 0
JDRY 102 4 0 0 0
J3 99 5 0 0 0
J2 96 4 0 0 0
J4 92 6 0 0 0
WW 88 8 0 0 0
J5 84 6 0 0 0
JX 95 4 0 0 0

[OUTFALLS]
OUT1 80 FREE NO
OUT2 82 FREE NO
OUT3 81 FREE NO

[STORAGE]
ST1 85 12 0 FUNCTIONAL 2000 0 0 0 0

[CONDUITS]
C1 J1 J2 400 0.013 0 0 0 0
C2 J3 J2 300 0.013 0 0 0 0
C3 J2 ST1 400 0.013 0 0 0 0
C4 JDRY J1 200 0.013 0 0 0 0
C5 J4 WW 300 0.013 0 0 0 0
C6 J5 OUT1 200 0.03 0 0 0 0
CX JX J4 100 0.013 0 0 0 0

[PUMPS]
P1 ST1 J4 PC1 ON 2 0.5
P2 WW J5 * ON 0 0

[ORIFICES]
OR1 ST1 J5 BOTTOM 0 0.65 NO 0
OR2 ST1 J5 SIDE 4 0.65 NO 0

[WEIRS]
W1 J2 OUT2 TRANSVERSE 3.0 3.33 NO 0 0 YES

[OUTLETS]
OL1 ST1 OUT3 6 FUNCTIONAL/DEPTH 2.0 1.5 NO

[XSECTIONS]
C1 CIRCULAR 1.5 0 0 0 1
C2 CIRCULAR 1.5 0 0 0 1
C3 CIRCULAR 1.0 0 0 0 1
C4 CIRCULAR 1.0 0 0 0 1
C5 CIRCULAR 1.5 0 0 0 1
C6 IRREGULAR CH1 0 0 0 1
CX CIRCULAR 1.0 0 0 0 1
OR1 CIRCULAR 0.5 0 0 0
OR2 RECT_CLOSED 1 2 0 0
W1 RECT_OPEN 1 4 0 0

[TRANSECTS]
NC 0.03 0.03 0.015
X1 CH1 4 0 30 0 0 0 0 0 0
GR 10 0 5 10 5 20 10 30

[CURVES]
PC1 PUMP4 0 0
PC1 2 3
PC1 6 6

[TIMESERIES]
RAIN 06/01/2026 00:00 0.0
RAIN 06/01/2026 00:30 3.0
RAIN 06/01/2026 01:00 6.0
RAIN 06/01/2026 01:30 2.0
RAIN 06/01/2026 02:00 0.0
RAIN 06/02/2026 00:00 0.0
RAIN 06/02/2026 00:30 5.0
RAIN 06/02/2026 01:00 1.0
RAIN 06/02/2026 01:30 0.0

[REPORT]
INPUT NO
)";

// The same network in SI units (CMS: flows print with three decimals).
const char* const kSiDeck = R"([TITLE]
G-08 report tables: every summary table, SI units

[OPTIONS]
FLOW_UNITS CMS
INFILTRATION HORTON
FLOW_ROUTING DYNWAVE
START_DATE 06/01/2026
START_TIME 00:00:00
REPORT_START_DATE 06/01/2026
REPORT_START_TIME 00:20:00
END_DATE 06/02/2026
END_TIME 03:00:00
WET_STEP 00:01:00
DRY_STEP 00:05:00
REPORT_STEP 00:05:00
ROUTING_STEP 5
THREADS 1

[RAINGAGES]
RG1 INTENSITY 0:30 1.0 TIMESERIES RAIN

[SUBCATCHMENTS]
S1 RG1 J1 4 60 250 1.0 0
S2 RG1 J3 3.2 70 180 1.0 0
S3 RG1 ST1 2 50 120 1.0 0

[SUBAREAS]
S1 0.013 0.1 1.27 2.54 25 OUTLET
S2 0.013 0.1 1.27 2.54 25 OUTLET
S3 0.013 0.1 1.27 2.54 25 OUTLET

[INFILTRATION]
S1 75 12.5 4 7 0
S2 75 12.5 4 7 0
S3 75 12.5 4 7 0

[LID_CONTROLS]
BC1 BC
BC1 SURFACE 150 0.0 0.1 1.0 5
BC1 SOIL 450 0.5 0.2 0.1 125 10 90
BC1 STORAGE 300 0.75 12.5 0
BC1 DRAIN 2.5 0.5 150 6
RB1 RB
RB1 STORAGE 1200 0.75 0 0
RB1 DRAIN 5 0.5 0 6

[LID_USAGE]
S1 BC1 2 45 3 0 30 0
S1 RB1 4 1.1 0 0 20 0
S2 BC1 1 75 6 0 25 0

[POLLUTANTS]
TSS MG/L 0 0 0 0 NO * 0 0 0
ZN UG/L 0 0 0 0 NO * 0 0 0
ECOLI #/L 0 0 0 0 NO * 0 0 0

[LANDUSES]
URBAN 0 0 0

[COVERAGES]
S1 URBAN 100
S2 URBAN 100
S3 URBAN 100

[BUILDUP]
URBAN TSS POW 55 0.5 2 AREA
URBAN ZN POW 5.5 0.5 2 AREA
URBAN ECOLI POW 2500000 0.5 2 AREA

[WASHOFF]
URBAN TSS EXP 0.1 1.5 0 0
URBAN ZN EXP 0.1 1.5 0 0
URBAN ECOLI EXP 0.1 1.5 0 0

[JUNCTIONS]
J1 30.5 1.8 0 0 0
JDRY 31.1 1.2 0 0 0
J3 30.2 1.5 0 0 0
J2 29.3 1.2 0 0 0
J4 28.0 1.8 0 0 0
WW 26.8 2.4 0 0 0
J5 25.6 1.8 0 0 0
JX 29.0 1.2 0 0 0

[OUTFALLS]
OUT1 24.4 FREE NO
OUT2 25.0 FREE NO
OUT3 24.7 FREE NO

[STORAGE]
ST1 25.9 3.7 0 FUNCTIONAL 185 0 0 0 0

[CONDUITS]
C1 J1 J2 120 0.013 0 0 0 0
C2 J3 J2 90 0.013 0 0 0 0
C3 J2 ST1 120 0.013 0 0 0 0
C4 JDRY J1 60 0.013 0 0 0 0
C5 J4 WW 90 0.013 0 0 0 0
C6 J5 OUT1 60 0.03 0 0 0 0
CX JX J4 30 0.013 0 0 0 0

[PUMPS]
P1 ST1 J4 PC1 ON 0.6 0.15
P2 WW J5 * ON 0 0

[ORIFICES]
OR1 ST1 J5 BOTTOM 0 0.65 NO 0
OR2 ST1 J5 SIDE 1.2 0.65 NO 0

[WEIRS]
W1 J2 OUT2 TRANSVERSE 0.9 1.84 NO 0 0 YES

[OUTLETS]
OL1 ST1 OUT3 1.8 FUNCTIONAL/DEPTH 0.1 1.5 NO

[XSECTIONS]
C1 CIRCULAR 0.45 0 0 0 1
C2 CIRCULAR 0.45 0 0 0 1
C3 CIRCULAR 0.3 0 0 0 1
C4 CIRCULAR 0.3 0 0 0 1
C5 CIRCULAR 0.45 0 0 0 1
C6 IRREGULAR CH1 0 0 0 1
CX CIRCULAR 0.3 0 0 0 1
OR1 CIRCULAR 0.15 0 0 0
OR2 RECT_CLOSED 0.3 0.6 0 0
W1 RECT_OPEN 0.3 1.2 0 0

[TRANSECTS]
NC 0.03 0.03 0.015
X1 CH1 4 0 9 0 0 0 0 0 0
GR 3 0 1.5 3 1.5 6 3 9

[CURVES]
PC1 PUMP4 0 0
PC1 0.6 0.085
PC1 1.8 0.17

[TIMESERIES]
RAIN 06/01/2026 00:00 0.0
RAIN 06/01/2026 00:30 75
RAIN 06/01/2026 01:00 150
RAIN 06/01/2026 01:30 50
RAIN 06/01/2026 02:00 0.0
RAIN 06/02/2026 00:00 0.0
RAIN 06/02/2026 00:30 125
RAIN 06/02/2026 01:00 25
RAIN 06/02/2026 01:30 0.0

[REPORT]
INPUT NO
)";

// Kinematic wave, no storage, pumps or LIDs, nothing floods or surcharges.
const char* const kKinwaveDeck = R"([TITLE]
G-08 report tables: kinematic wave, no storage, pumps or LIDs

[OPTIONS]
FLOW_UNITS CFS
INFILTRATION HORTON
FLOW_ROUTING KINWAVE
START_DATE 06/01/2026
START_TIME 00:00:00
END_DATE 06/01/2026
END_TIME 06:00:00
WET_STEP 00:01:00
DRY_STEP 00:05:00
REPORT_STEP 00:05:00
ROUTING_STEP 30

[RAINGAGES]
RG1 INTENSITY 0:30 1.0 TIMESERIES RAIN

[SUBCATCHMENTS]
S1 RG1 J1 5 50 400 1.0 0

[SUBAREAS]
S1 0.013 0.1 0.05 0.1 25 OUTLET

[INFILTRATION]
S1 3.0 0.5 4 7 0

[JUNCTIONS]
J1 100 6 0 0 0
J2 98 6 0 0 0

[OUTFALLS]
OUT1 96 FREE NO

[CONDUITS]
C1 J1 J2 400 0.013 0 0 0 0
C2 J2 OUT1 400 0.013 0 0 0 0

[XSECTIONS]
C1 CIRCULAR 3 0 0 0 1
C2 CIRCULAR 3 0 0 0 1

[TIMESERIES]
RAIN 06/01/2026 00:00 0.5
RAIN 06/01/2026 01:00 0.0

[REPORT]
INPUT NO
)";

// ---------------------------------------------------------------------------
// Reading the tables through the C API
// ---------------------------------------------------------------------------

struct SummaryRow {
    int type = 0;
    int index = 0;
    std::string label;
    std::vector<double> values;
};

struct SummaryTable {
    std::string title;
    std::vector<std::string> headings;
    std::vector<std::string> units;
    std::vector<SummaryRow> rows;
};

/** Read all eleven tables through swmm_summary_*. */
std::vector<SummaryTable> readTables(SWMM_Engine e) {
    std::vector<SummaryTable> all;
    for (int t = 0; t < SWMM_SUMMARY_TABLE_COUNT; ++t) {
        SummaryTable s;
        char buf[256];
        EXPECT_EQ(swmm_summary_table_title(e, t, buf, int(sizeof buf)), SWMM_OK);
        s.title = buf;
        int columns = -1, rows = -1;
        EXPECT_EQ(swmm_summary_column_count(e, t, &columns), SWMM_OK);
        EXPECT_EQ(swmm_summary_row_count(e, t, &rows), SWMM_OK);
        for (int c = 0; c < columns; ++c) {
            char name[128], units[64];
            EXPECT_EQ(
                swmm_summary_column(e, t, c, name, int(sizeof name), units, int(sizeof units)),
                SWMM_OK);
            s.headings.push_back(name);
            s.units.push_back(units);
        }
        for (int r = 0; r < rows; ++r) {
            SummaryRow row;
            char label[128];
            EXPECT_EQ(swmm_summary_row(e, t, r, &row.type, &row.index, label, int(sizeof label)),
                      SWMM_OK);
            row.label = label;
            for (int c = 0; c < columns; ++c) {
                double v = 0.0;
                EXPECT_EQ(swmm_summary_value(e, t, r, c, &v), SWMM_OK);
                row.values.push_back(v);
            }
            s.rows.push_back(row);
        }
        all.push_back(s);
    }
    return all;
}

/**
 * The whole-table reads of every table equal its per-cell reads @p per_cell: objects, values
 * (bitwise, row-major) and the '\n'-joined labels, sized with a NULL-buffer query first. One
 * guard element past each table must stay untouched.
 */
void expectWholeTableReadsMatch(SWMM_Engine e, const std::vector<SummaryTable>& per_cell) {
    ASSERT_EQ(per_cell.size(), static_cast<std::size_t>(SWMM_SUMMARY_TABLE_COUNT));
    for (int t = 0; t < SWMM_SUMMARY_TABLE_COUNT; ++t) {
        const SummaryTable& s = per_cell[static_cast<std::size_t>(t)];
        SCOPED_TRACE("whole-table reads of " + s.title);
        const std::size_t rows = s.rows.size(), cols = s.headings.size();

        std::vector<int> types(rows + 1, 77), indices(rows + 1, 77);
        ASSERT_EQ(swmm_summary_rows_read(e, t, types.data(), indices.data(), int(rows + 1)),
                  SWMM_OK);
        std::vector<double> values(rows * cols + 1, 1234.5);
        ASSERT_EQ(swmm_summary_values_read(e, t, values.data(), int(values.size())), SWMM_OK);
        for (std::size_t r = 0; r < rows; ++r) {
            EXPECT_EQ(types[r], s.rows[r].type) << "row " << r;
            EXPECT_EQ(indices[r], s.rows[r].index) << "row " << r;
            EXPECT_EQ(
                std::memcmp(&values[r * cols], s.rows[r].values.data(), cols * sizeof(double)), 0)
                << "row " << r;
        }
        EXPECT_EQ(types[rows], 77);
        EXPECT_EQ(indices[rows], 77);
        EXPECT_EQ(values[rows * cols], 1234.5);

        std::string joined;
        for (std::size_t r = 0; r < rows; ++r)
            joined += (r > 0 ? "\n" : "") + s.rows[r].label;
        int needed = -1;
        EXPECT_EQ(swmm_summary_labels_read(e, t, nullptr, 0, &needed), SWMM_ERR_BADPARAM);
        ASSERT_EQ(needed, static_cast<int>(joined.size() + 1));
        std::vector<char> labels(static_cast<std::size_t>(needed) + 1, 'x');
        ASSERT_EQ(swmm_summary_labels_read(e, t, labels.data(), needed, nullptr), SWMM_OK);
        EXPECT_EQ(labels[static_cast<std::size_t>(needed) - 1], '\0');
        EXPECT_EQ(std::string(labels.data()), joined);
        EXPECT_EQ(labels[static_cast<std::size_t>(needed)], 'x');
    }
}

/** Whether two tables are identical, NaN bit patterns included. */
bool sameTable(const SummaryTable& a, const SummaryTable& b) {
    if (a.title != b.title || a.headings != b.headings || a.units != b.units ||
        a.rows.size() != b.rows.size())
        return false;
    for (std::size_t r = 0; r < a.rows.size(); ++r) {
        const auto &x = a.rows[r], &y = b.rows[r];
        if (x.type != y.type || x.index != y.index || x.label != y.label ||
            x.values.size() != y.values.size() ||
            std::memcmp(x.values.data(), y.values.data(), x.values.size() * sizeof(double)) != 0)
            return false;
    }
    return true;
}

/** A finished run: its tables before and after swmm_engine_report(), and its .rpt. */
struct DeckRun {
    std::vector<SummaryTable> before_report, after_report;
    std::vector<std::string> nodes, links, subcatches;
    std::string rpt; ///< the report text
    std::string rpt_path;
    bool ok = false;
};

/** Run @p inp to ENDED, read the tables, write the report, read them again. */
DeckRun runDeck(const std::string& inp, const std::string& name) {
    DeckRun run;
    run.rpt_path = std::string(kDir) + "/" + name + ".rpt";
    const std::string out = std::string(kDir) + "/" + name + ".out";
    SWMM_Engine e = swmm_engine_create();
    EXPECT_NE(e, nullptr);
    if (!e)
        return run;
    EXPECT_EQ(swmm_engine_open(e, inp.c_str(), run.rpt_path.c_str(), out.c_str(), nullptr),
              SWMM_OK);
    EXPECT_EQ(swmm_engine_initialize(e), SWMM_OK);
    EXPECT_EQ(swmm_engine_start(e, 1), SWMM_OK);
    double elapsed = 0.0;
    int rc = SWMM_OK;
    do {
        rc = swmm_engine_step(e, &elapsed);
    } while (rc == SWMM_OK && elapsed > 0.0);
    EXPECT_EQ(rc, SWMM_OK);
    EXPECT_EQ(swmm_engine_end(e), SWMM_OK);
    for (int i = 0; i < swmm_node_count(e); ++i)
        run.nodes.push_back(swmm_node_id(e, i));
    for (int i = 0; i < swmm_link_count(e); ++i)
        run.links.push_back(swmm_link_id(e, i));
    for (int i = 0; i < swmm_subcatch_count(e); ++i)
        run.subcatches.push_back(swmm_subcatch_id(e, i));
    run.before_report = readTables(e); // what the GUI reads: after end(), before report()
    expectWholeTableReadsMatch(e, run.before_report);
    EXPECT_EQ(swmm_engine_report(e), SWMM_OK);
    run.after_report = readTables(e);
    expectWholeTableReadsMatch(e, run.after_report);
    EXPECT_EQ(swmm_engine_close(e), SWMM_OK);
    swmm_engine_destroy(e);
    std::ifstream in(run.rpt_path, std::ios::binary);
    run.rpt.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    run.ok = !run.rpt.empty();
    return run;
}

/** Write @p text to report_tables/<name>.inp and return the path. */
std::string writeDeck(const std::string& name, const std::string& text) {
    fs::create_directories(kDir);
    const std::string path = std::string(kDir) + "/" + name + ".inp";
    std::ofstream(path, std::ios::binary) << text;
    return path;
}

// ---------------------------------------------------------------------------
// Reading the same tables from the .rpt
// ---------------------------------------------------------------------------

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);)
        lines.push_back(line);
    return lines;
}

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(' ');
    if (b == std::string::npos)
        return "";
    return s.substr(b, s.find_last_not_of(' ') - b + 1);
}

bool isDashes(const std::string& line) {
    const std::string t = trim(line);
    return !t.empty() && t.find_first_not_of('-') == std::string::npos;
}

std::vector<std::string> tokens(const std::string& line) {
    std::istringstream in(line);
    return {std::istream_iterator<std::string>(in), std::istream_iterator<std::string>()};
}

/**
 * The data lines of the report section titled @p title (System line included), or nullopt when
 * the report has no such section. A section without a table ("No nodes were flooded.") has none.
 */
std::optional<std::vector<std::string>> sectionRows(const std::vector<std::string>& lines,
                                                    const std::string& title) {
    std::size_t i = 0;
    while (i < lines.size() && lines[i] != "  " + title)
        ++i;
    if (i == lines.size())
        return std::nullopt;
    std::vector<std::string> body;
    for (std::size_t k = i + 2; k < lines.size(); ++k) {
        if (lines[k].rfind("  *", 0) == 0 || lines[k].rfind("  Analysis begun", 0) == 0)
            break;
        body.push_back(lines[k]);
    }
    std::vector<std::string> rows;
    int dashes = 0;
    std::size_t k = 0;
    for (; k < body.size() && dashes < 2; ++k)
        dashes += isDashes(body[k]) ? 1 : 0;
    if (dashes < 2)
        return rows;
    for (; k < body.size() && !trim(body[k]).empty() && !isDashes(body[k]); ++k)
        rows.push_back(body[k]);
    if (k + 1 < body.size() && isDashes(body[k]) && body[k + 1].rfind("  System", 0) == 0)
        rows.push_back(body[k + 1]);
    return rows;
}

std::string fmt(const char* format, double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, format, v);
    return buf;
}

/** "days" and "hr:min" of an elapsed-days value, rounded as the report's elapsedToParts(). */
std::pair<std::string, std::string> dayClock(double x) {
    int days = 0, hrs = 0, mins = 0;
    if (x > 0.0) {
        days = static_cast<int>(x);
        int secs = static_cast<int>(std::floor((x - std::floor(x)) * 86400.0 + 0.5));
        if (secs >= 86400)
            secs = 86399;
        hrs = secs / 60 / 60;
        mins = (secs / 60) % 60;
    }
    char clock[16];
    std::snprintf(clock, sizeof clock, "%02d:%02d", hrs, mins);
    return {std::to_string(days), clock};
}

/** How the report prints one column. */
enum class Fmt { F1, F2, F3, G3, Int, Flow, Time, Balance, BalanceVolume };

std::vector<Fmt> columnFormats(int table, std::size_t columns) {
    using F = Fmt;
    switch (table) {
    case SWMM_SUMMARY_NODE_DEPTH:
        return {F::F2, F::F2, F::F2, F::Time, F::F2};
    case SWMM_SUMMARY_NODE_INFLOW:
        return {F::Flow, F::Flow, F::Time, F::G3, F::G3, F::Balance, F::BalanceVolume};
    case SWMM_SUMMARY_NODE_SURCHARGE:
        return {F::F2, F::F3, F::F3};
    case SWMM_SUMMARY_NODE_FLOODING:
        return {F::F2, F::Flow, F::Time, F::F3, F::F3};
    case SWMM_SUMMARY_STORAGE_VOLUME:
        return {F::F3, F::F1, F::F1, F::F1, F::F3, F::F1, F::Time, F::Flow};
    case SWMM_SUMMARY_OUTFALL_LOADING: {
        std::vector<Fmt> f{F::F2, F::Flow, F::Flow, F::F3};
        f.resize(columns, F::F3); // pollutant loads
        return f;
    }
    case SWMM_SUMMARY_FLOW_CLASSIFICATION:
        return std::vector<Fmt>(10, F::F2);
    case SWMM_SUMMARY_CONDUIT_SURCHARGE:
        return std::vector<Fmt>(5, F::F2);
    case SWMM_SUMMARY_PUMPING: // pump flows are always %9.2f
        return {F::F2, F::Int, F::F2, F::F2, F::F2, F::F3, F::F2, F::F1, F::F1};
    case SWMM_SUMMARY_LID_PERFORMANCE:
        return std::vector<Fmt>(5, F::F2);
    default:
        return {};
    }
}

/** The report's text for each value of @p row, in order, as whitespace-separated tokens. */
std::vector<std::string> valueTokens(const SummaryTable& t, int table, const SummaryRow& row) {
    const auto formats = columnFormats(table, t.headings.size());
    EXPECT_EQ(formats.size(), row.values.size()) << t.title;
    std::vector<std::string> out;
    for (std::size_t c = 0; c < formats.size() && c < row.values.size(); ++c) {
        const double v = row.values[c];
        const bool three = t.units[c] == "CMS" || t.units[c] == "MGD";
        switch (formats[c]) {
        case Fmt::Balance:
        case Fmt::BalanceVolume:
            if (!std::isnan(v)) {
                out.push_back(fmt("%.3f", v));
                if (formats[c] == Fmt::BalanceVolume)
                    out.push_back(t.units[c]);
            }
            continue;
        default:
            break;
        }
        EXPECT_FALSE(std::isnan(v)) << t.title << " column " << c;
        switch (formats[c]) {
        case Fmt::F1:
            out.push_back(fmt("%.1f", v));
            break;
        case Fmt::F2:
            out.push_back(fmt("%.2f", v));
            break;
        case Fmt::F3:
            out.push_back(fmt("%.3f", v));
            break;
        case Fmt::G3:
            out.push_back(fmt("%.3g", v));
            break;
        case Fmt::Int:
            out.push_back(std::to_string(static_cast<int>(v)));
            break;
        case Fmt::Flow:
            out.push_back(fmt(three ? "%.3f" : "%.2f", v));
            break;
        case Fmt::Time: {
            const auto [days, clock] = dayClock(v);
            out.push_back(days);
            out.push_back(clock);
            break;
        }
        default:
            break;
        }
    }
    return out;
}

std::string objectName(const DeckRun& run, const SummaryRow& row) {
    const auto& names = row.type == SWMM_SUMMARY_OBJECT_NODE   ? run.nodes
                        : row.type == SWMM_SUMMARY_OBJECT_LINK ? run.links
                                                               : run.subcatches;
    return (row.index >= 0 && row.index < static_cast<int>(names.size()))
               ? names[static_cast<std::size_t>(row.index)]
               : "<bad index " + std::to_string(row.index) + ">";
}

/** Link Flow rows leave columns blank, so they are read by position. */
void expectLinkFlowLine(const DeckRun& run, const SummaryTable& t, const SummaryRow& row,
                        const std::string& line) {
    auto slice = [&](std::size_t from, std::size_t n) {
        return from < line.size() ? trim(line.substr(from, n)) : std::string();
    };
    const bool three = t.units[0] == "CMS" || t.units[0] == "MGD";
    EXPECT_EQ(slice(2, 20), objectName(run, row));
    EXPECT_EQ(slice(22, 9), row.label);
    EXPECT_EQ(slice(31, 9), fmt(three ? "%.3f" : "%.2f", row.values[0]));
    const auto [days, clock] = dayClock(row.values[1]);
    EXPECT_EQ(tokens(slice(40, 13)), (std::vector<std::string>{days, clock}));
    const double vel = row.values[2], ratio = row.values[3], depth = row.values[4];
    EXPECT_EQ(slice(53, 10), std::isnan(vel) ? "" : vel > 50.0 ? ">50.00" : fmt("%.2f", vel));
    EXPECT_EQ(slice(63, 8), std::isnan(ratio) ? "" : fmt("%.2f", ratio));
    EXPECT_EQ(slice(71, 8), std::isnan(depth) ? "" : fmt("%.2f", depth));
}

/** Tables whose rows carry the report's type column. */
bool hasTypeColumn(int table) {
    return table == SWMM_SUMMARY_NODE_DEPTH || table == SWMM_SUMMARY_NODE_INFLOW ||
           table == SWMM_SUMMARY_NODE_SURCHARGE || table == SWMM_SUMMARY_LINK_FLOW;
}

/** Every row and value of every table, against what the run's report prints. */
void expectTablesMatchReport(const DeckRun& run, const std::vector<SummaryTable>& tables) {
    const auto lines = splitLines(run.rpt);
    ASSERT_EQ(tables.size(), static_cast<std::size_t>(SWMM_SUMMARY_TABLE_COUNT));
    for (int table = 0; table < SWMM_SUMMARY_TABLE_COUNT; ++table) {
        const SummaryTable& t = tables[static_cast<std::size_t>(table)];
        SCOPED_TRACE(t.title);
        const auto printed = sectionRows(lines, t.title);
        if (!printed) {
            EXPECT_TRUE(t.rows.empty()) << "rows for a section the report does not print";
            continue;
        }
        ASSERT_EQ(printed->size(), t.rows.size());
        for (std::size_t r = 0; r < t.rows.size(); ++r) {
            const SummaryRow& row = t.rows[r];
            const std::string& line = (*printed)[r];
            SCOPED_TRACE(line);
            ASSERT_EQ(row.values.size(), t.headings.size());
            if (table == SWMM_SUMMARY_LINK_FLOW) {
                EXPECT_EQ(row.type, SWMM_SUMMARY_OBJECT_LINK);
                expectLinkFlowLine(run, t, row, line);
                continue;
            }
            std::vector<std::string> expected;
            if (row.type == SWMM_SUMMARY_OBJECT_SYSTEM) {
                EXPECT_EQ(row.index, -1);
                EXPECT_EQ(row.label, "");
                expected.push_back("System");
            } else if (table == SWMM_SUMMARY_LID_PERFORMANCE) {
                EXPECT_EQ(row.type, SWMM_SUMMARY_OBJECT_SUBCATCH);
                expected.push_back(row.label);
                expected.push_back(objectName(run, row));
            } else {
                expected.push_back(objectName(run, row));
                if (hasTypeColumn(table))
                    expected.push_back(row.label);
                else
                    EXPECT_EQ(row.label, "");
            }
            const auto values = valueTokens(t, table, row);
            expected.insert(expected.end(), values.begin(), values.end());
            EXPECT_EQ(tokens(line), expected);
        }
    }
}

/** The column headings and units of the US (@p si false) or SI deck. */
void expectColumns(const std::vector<SummaryTable>& tables, bool si) {
    const std::string flow = si ? "CMS" : "CFS", vol = si ? "10^6 ltr" : "10^6 gal",
                      len = si ? "Meters" : "Feet", depth = si ? "mm" : "in",
                      svol = si ? "1000 m3" : "1000 ft3", vel = si ? "m/sec" : "ft/sec",
                      load = si ? "kg" : "lbs";
    using Cols = std::vector<std::pair<std::string, std::string>>;
    const std::vector<Cols> want = {
        {{"Average Depth", len},
         {"Maximum Depth", len},
         {"Maximum HGL", len},
         {"Time of Max Occurrence", "days"},
         {"Reported Max Depth", len}},
        {{"Maximum Lateral Inflow", flow},
         {"Maximum Total Inflow", flow},
         {"Time of Max Occurrence", "days"},
         {"Lateral Inflow Volume", vol},
         {"Total Inflow Volume", vol},
         {"Flow Balance Error", "%"},
         {"Flow Balance Error Volume", si ? "ltr" : "gal"}},
        {{"Hours Surcharged", "hours"},
         {"Max. Height Above Crown", len},
         {"Min. Depth Below Rim", len}},
        {{"Hours Flooded", "hours"},
         {"Maximum Rate", flow},
         {"Time of Max Occurrence", "days"},
         {"Total Flood Volume", vol},
         {"Maximum Ponded Depth", len}},
        {{"Average Volume", svol},
         {"Avg Pcnt Full", "%"},
         {"Evap Pcnt Loss", "%"},
         {"Exfil Pcnt Loss", "%"},
         {"Maximum Volume", svol},
         {"Max Pcnt Full", "%"},
         {"Time of Max Occurrence", "days"},
         {"Maximum Outflow", flow}},
        {{"Flow Freq", "%"},
         {"Avg Flow", flow},
         {"Max Flow", flow},
         {"Total Volume", vol},
         {"TSS", load},
         {si ? "ZN" : "LEAD", load},
         {si ? "ECOLI" : "FC", "LogN"}},
        {{"Maximum |Flow|", flow},
         {"Time of Max Occurrence", "days"},
         {"Maximum |Veloc|", vel},
         {"Max/Full Flow", ""},
         {"Max/Full Depth", ""}},
        {{"Adjusted/Actual Length", ""},
         {"Dry", ""},
         {"Up Dry", ""},
         {"Down Dry", ""},
         {"Sub Crit", ""},
         {"Sup Crit", ""},
         {"Up Crit", ""},
         {"Down Crit", ""},
         {"Norm Ltd", ""},
         {"Inlet Ctrl", ""}},
        {{"Hours Full Both Ends", "hours"},
         {"Hours Full Upstream", "hours"},
         {"Hours Full Dnstream", "hours"},
         {"Hours Above Full Normal Flow", "hours"},
         {"Hours Capacity Limited", "hours"}},
        {{"Percent Utilized", "%"},
         {"Number of Start-Ups", ""},
         {"Min Flow", flow},
         {"Avg Flow", flow},
         {"Max Flow", flow},
         {"Total Volume", vol},
         {"Power Usage", "Kw-hr"},
         {"Time Off Pump Curve Low", "%"},
         {"Time Off Pump Curve High", "%"}},
        {{"Total Inflow", depth},
         {"Evap Loss", depth},
         {"Infil Loss", depth},
         {"Surface Runoff", depth},
         {"Drain Flow", depth}}};
    const char* const titles[] = {"Node Depth Summary",          "Node Inflow Summary",
                                  "Node Surcharge Summary",      "Node Flooding Summary",
                                  "Storage Volume Summary",      "Outfall Loading Summary",
                                  "Link Flow Summary",           "Flow Classification Summary",
                                  "Conduit Surcharge Summary",   "Pumping Summary",
                                  "LID Performance Summary"};
    ASSERT_EQ(tables.size(), want.size());
    for (std::size_t t = 0; t < want.size(); ++t) {
        EXPECT_EQ(tables[t].title, titles[t]);
        Cols got;
        for (std::size_t c = 0; c < tables[t].headings.size(); ++c)
            got.emplace_back(tables[t].headings[c], tables[t].units[c]);
        EXPECT_EQ(got, want[t]) << titles[t];
    }
}

/** Run @p deck and check its tables against its report, before and after report(). */
DeckRun runAndCompare(const std::string& inp, const std::string& name) {
    DeckRun run = runDeck(inp, name);
    EXPECT_TRUE(run.ok) << "no report at " << run.rpt_path;
    if (!run.ok)
        return run;
    for (int t = 0; t < SWMM_SUMMARY_TABLE_COUNT; ++t)
        EXPECT_TRUE(sameTable(run.before_report[static_cast<std::size_t>(t)],
                              run.after_report[static_cast<std::size_t>(t)]))
            << run.after_report[static_cast<std::size_t>(t)].title
            << " differs before and after swmm_engine_report()";
    {
        SCOPED_TRACE("before swmm_engine_report");
        expectTablesMatchReport(run, run.before_report);
    }
    {
        SCOPED_TRACE("after swmm_engine_report");
        expectTablesMatchReport(run, run.after_report);
    }
    return run;
}

const SummaryTable& table(const DeckRun& run, int t) {
    return run.before_report[static_cast<std::size_t>(t)];
}

} // namespace

// ===========================================================================
// Every table against the report
// ===========================================================================

TEST(ReportTables, UsDeckEveryTableMatchesTheReport) {
    const DeckRun run = runAndCompare(writeDeck("us_all_tables", kUsDeck), "us_all_tables");
    ASSERT_TRUE(run.ok);
    expectColumns(run.before_report, false);
    for (int t = 0; t < SWMM_SUMMARY_TABLE_COUNT; ++t)
        EXPECT_FALSE(table(run, t).rows.empty()) << table(run, t).title << " has no rows";
}

TEST(ReportTables, SiDeckEveryTableMatchesTheReport) {
    const DeckRun run = runAndCompare(writeDeck("si_all_tables", kSiDeck), "si_all_tables");
    ASSERT_TRUE(run.ok);
    expectColumns(run.before_report, true);
    for (int t = 0; t < SWMM_SUMMARY_TABLE_COUNT; ++t)
        EXPECT_FALSE(table(run, t).rows.empty()) << table(run, t).title << " has no rows";
}

// Existing report fixtures: run-on, a PUMP4 pump and a full storage unit; a 30-hour
// pollutant deck. Outputs go to report_tables/, inputs are read where they are.
TEST(ReportTables, ReportFixturesMatchTheReport) {
    fs::create_directories(kDir);
    for (const auto& [inp, name] :
         std::vector<std::pair<std::string, std::string>>{
             {"mc_storm_parity/report_statistics.inp", "report_statistics"},
             {"site_drainage_model.inp", "site_drainage_model"}}) {
        SCOPED_TRACE(inp);
        const DeckRun run = runAndCompare(inp, name);
        ASSERT_TRUE(run.ok);
        EXPECT_FALSE(table(run, SWMM_SUMMARY_LINK_FLOW).rows.empty());
    }
}

// ===========================================================================
// Tables the report does not print have no rows
// ===========================================================================

TEST(ReportTables, IgnoredRoutingLeavesOnlyTheLidTable) {
    std::string deck = kUsDeck;
    const std::string key = "FLOW_ROUTING DYNWAVE\n";
    deck.replace(deck.find(key), key.size(), key + "IGNORE_ROUTING YES\n");
    const DeckRun run = runAndCompare(writeDeck("ignore_routing", deck), "ignore_routing");
    ASSERT_TRUE(run.ok);
    for (int t = 0; t < SWMM_SUMMARY_TABLE_COUNT; ++t) {
        const SummaryTable& s = table(run, t);
        if (t == SWMM_SUMMARY_LID_PERFORMANCE) {
            EXPECT_EQ(s.rows.size(), 3u);
            continue;
        }
        EXPECT_TRUE(s.rows.empty()) << s.title;
        EXPECT_EQ(run.rpt.find("  " + s.title + "\n"), std::string::npos) << s.title;
    }
}

TEST(ReportTables, KinematicWaveSkipsDynamicWaveAndAbsentObjectTables) {
    const DeckRun run = runAndCompare(writeDeck("kinwave", kKinwaveDeck), "kinwave");
    ASSERT_TRUE(run.ok);
    for (const int t : {SWMM_SUMMARY_NODE_SURCHARGE, SWMM_SUMMARY_STORAGE_VOLUME,
                        SWMM_SUMMARY_FLOW_CLASSIFICATION, SWMM_SUMMARY_PUMPING,
                        SWMM_SUMMARY_LID_PERFORMANCE}) {
        EXPECT_TRUE(table(run, t).rows.empty()) << table(run, t).title;
        EXPECT_EQ(run.rpt.find("  " + table(run, t).title + "\n"), std::string::npos)
            << table(run, t).title;
    }
    // Printed, but with nothing to list.
    EXPECT_TRUE(table(run, SWMM_SUMMARY_NODE_FLOODING).rows.empty());
    EXPECT_NE(run.rpt.find("No nodes were flooded."), std::string::npos);
    EXPECT_TRUE(table(run, SWMM_SUMMARY_CONDUIT_SURCHARGE).rows.empty());
    EXPECT_NE(run.rpt.find("No conduits were surcharged."), std::string::npos);
    EXPECT_EQ(table(run, SWMM_SUMMARY_NODE_DEPTH).rows.size(), 3u);
    EXPECT_EQ(table(run, SWMM_SUMMARY_LINK_FLOW).rows.size(), 2u);
    EXPECT_EQ(table(run, SWMM_SUMMARY_OUTFALL_LOADING).rows.size(), 2u); // OUT1 and System
}

// ===========================================================================
// Independent of the shared builders: the raw statistics and the deck's layout
// ===========================================================================

TEST(ReportTables, IndependentOracle) {
    const std::string inp = writeDeck("us_oracle", kUsDeck);
    const std::string rpt = std::string(kDir) + "/us_oracle.rpt";
    const std::string out = std::string(kDir) + "/us_oracle.out";
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_engine_open(e, inp.c_str(), rpt.c_str(), out.c_str(), nullptr), SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(e), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(e, 1), SWMM_OK);
    double elapsed = 0.0;
    do {
        ASSERT_EQ(swmm_engine_step(e, &elapsed), SWMM_OK);
    } while (elapsed > 0.0);
    ASSERT_EQ(swmm_engine_end(e), SWMM_OK);
    const auto tables = readTables(e);

    auto nodeIndex = [&](const char* id) {
        for (int i = 0; i < swmm_node_count(e); ++i)
            if (std::strcmp(swmm_node_id(e, i), id) == 0)
                return i;
        return -1;
    };
    auto linkIndex = [&](const char* id) {
        for (int i = 0; i < swmm_link_count(e); ++i)
            if (std::strcmp(swmm_link_id(e, i), id) == 0)
                return i;
        return -1;
    };
    auto subcatchIndex = [&](const char* id) {
        for (int i = 0; i < swmm_subcatch_count(e); ++i)
            if (std::strcmp(swmm_subcatch_id(e, i), id) == 0)
                return i;
        return -1;
    };
    auto stat = [&](int (*get)(SWMM_Engine, int, double*), int i) {
        double v = std::nan("");
        EXPECT_EQ(get(e, i, &v), SWMM_OK);
        return v;
    };
    const auto& T = [&](int t) -> const SummaryTable& {
        return tables[static_cast<std::size_t>(t)];
    };
    // US units: flows and lengths are the internal cfs and ft; summary volumes 7.48 gal/ft3.
    const double Vcf = 7.48 / 1.0e6;

    // Node Depth: one row per node, in node order, typed; depth and HGL from the raw maximum.
    const std::map<std::string, std::string> node_type = {
        {"J1", "JUNCTION"}, {"JDRY", "JUNCTION"}, {"J3", "JUNCTION"}, {"J2", "JUNCTION"},
        {"J4", "JUNCTION"}, {"WW", "JUNCTION"},   {"J5", "JUNCTION"}, {"JX", "JUNCTION"},
        {"OUT1", "OUTFALL"}, {"OUT2", "OUTFALL"}, {"OUT3", "OUTFALL"}, {"ST1", "STORAGE"}};
    ASSERT_EQ(T(SWMM_SUMMARY_NODE_DEPTH).rows.size(), node_type.size());
    for (int i = 0; i < swmm_node_count(e); ++i) {
        const auto& row = T(SWMM_SUMMARY_NODE_DEPTH).rows[static_cast<std::size_t>(i)];
        SCOPED_TRACE(swmm_node_id(e, i));
        EXPECT_EQ(row.type, SWMM_SUMMARY_OBJECT_NODE);
        EXPECT_EQ(row.index, i);
        EXPECT_EQ(row.label, node_type.at(swmm_node_id(e, i)));
        double invert = 0.0;
        ASSERT_EQ(swmm_node_get_invert_elev(e, i, &invert), SWMM_OK);
        const double maxd = stat(swmm_stat_node_max_depth, i);
        EXPECT_EQ(row.values[1], maxd);
        EXPECT_NEAR(row.values[2], invert + maxd, 1e-9);
        const auto& inflow = T(SWMM_SUMMARY_NODE_INFLOW).rows[static_cast<std::size_t>(i)];
        EXPECT_EQ(inflow.index, i);
        EXPECT_EQ(inflow.label, row.label);
    }
    // The dry junction prints its balance as a volume.
    const auto& jx = T(SWMM_SUMMARY_NODE_INFLOW).rows[static_cast<std::size_t>(nodeIndex("JX"))];
    EXPECT_TRUE(std::isnan(jx.values[5]));
    EXPECT_FALSE(std::isnan(jx.values[6]));
    const auto& j1 = T(SWMM_SUMMARY_NODE_INFLOW).rows[static_cast<std::size_t>(nodeIndex("J1"))];
    EXPECT_FALSE(std::isnan(j1.values[5]));
    EXPECT_TRUE(std::isnan(j1.values[6]));

    // Node Surcharge: never an outfall.
    for (const auto& row : T(SWMM_SUMMARY_NODE_SURCHARGE).rows)
        EXPECT_NE(row.label, "OUTFALL");

    // Node Flooding: the nodes with flooding time, hours floored at 0.01, rate and volume raw.
    std::vector<int> flooded;
    for (int i = 0; i < swmm_node_count(e); ++i)
        if (stat(swmm_stat_node_time_flooded, i) > 0.0)
            flooded.push_back(i);
    ASSERT_FALSE(flooded.empty());
    ASSERT_EQ(T(SWMM_SUMMARY_NODE_FLOODING).rows.size(), flooded.size());
    for (std::size_t r = 0; r < flooded.size(); ++r) {
        const auto& row = T(SWMM_SUMMARY_NODE_FLOODING).rows[r];
        const int i = flooded[r];
        EXPECT_EQ(row.index, i);
        EXPECT_EQ(row.values[0], std::max(0.01, stat(swmm_stat_node_time_flooded, i) / 3600.0));
        EXPECT_EQ(row.values[1], stat(swmm_stat_node_max_overflow, i));
        EXPECT_EQ(row.values[3], stat(swmm_stat_node_vol_flooded, i) * Vcf);
    }

    // Storage Volume: the storage unit only.
    ASSERT_EQ(T(SWMM_SUMMARY_STORAGE_VOLUME).rows.size(), 1u);
    EXPECT_EQ(T(SWMM_SUMMARY_STORAGE_VOLUME).rows[0].index, nodeIndex("ST1"));

    // Outfall Loading: the outfalls in node order, then a System row that totals them.
    const auto& outfalls = T(SWMM_SUMMARY_OUTFALL_LOADING).rows;
    ASSERT_EQ(outfalls.size(), 4u);
    EXPECT_EQ(outfalls[0].index, nodeIndex("OUT1"));
    EXPECT_EQ(outfalls[1].index, nodeIndex("OUT2"));
    EXPECT_EQ(outfalls[2].index, nodeIndex("OUT3"));
    EXPECT_EQ(outfalls[3].type, SWMM_SUMMARY_OBJECT_SYSTEM);
    EXPECT_EQ(outfalls[3].index, -1);
    EXPECT_EQ(outfalls[3].values[0],
              (outfalls[0].values[0] + outfalls[1].values[0] + outfalls[2].values[0]) / 3);
    EXPECT_EQ(outfalls[3].values[1],
              outfalls[0].values[1] + outfalls[1].values[1] + outfalls[2].values[1]);
    EXPECT_EQ(outfalls[3].values[3],
              outfalls[0].values[3] + outfalls[1].values[3] + outfalls[2].values[3]);
    for (std::size_t c = 4; c < 6; ++c) // mass loads add (FC prints LOG10)
        EXPECT_NEAR(outfalls[3].values[c],
                    outfalls[0].values[c] + outfalls[1].values[c] + outfalls[2].values[c],
                    1e-12);

    // Link Flow: one row per link, labeled as the report types it; blanks where it prints none.
    struct LinkWant {
        const char* label;
        bool vel, ratio, depth;
    };
    const std::map<std::string, LinkWant> links = {
        {"C1", {"CONDUIT", true, true, true}},   {"C2", {"CONDUIT", true, true, true}},
        {"C3", {"CONDUIT", true, true, true}},   {"C4", {"CONDUIT", true, true, true}},
        {"C5", {"CONDUIT", true, true, true}},   {"C6", {"CHANNEL", true, true, true}},
        {"CX", {"CONDUIT", true, true, true}},   {"P1", {"PUMP", false, true, false}},
        {"P2", {"PUMP", false, false, false}},   {"OR1", {"ORIFICE", false, false, false}},
        {"OR2", {"ORIFICE", false, false, true}}, {"W1", {"WEIR", false, false, true}},
        {"OL1", {"DUMMY", false, false, false}}};
    ASSERT_EQ(T(SWMM_SUMMARY_LINK_FLOW).rows.size(), links.size());
    for (int i = 0; i < swmm_link_count(e); ++i) {
        const auto& row = T(SWMM_SUMMARY_LINK_FLOW).rows[static_cast<std::size_t>(i)];
        const LinkWant& w = links.at(swmm_link_id(e, i));
        SCOPED_TRACE(swmm_link_id(e, i));
        EXPECT_EQ(row.type, SWMM_SUMMARY_OBJECT_LINK);
        EXPECT_EQ(row.index, i);
        EXPECT_EQ(row.label, w.label);
        EXPECT_EQ(row.values[0], stat(swmm_stat_link_max_flow, i));
        EXPECT_EQ(!std::isnan(row.values[2]), w.vel);
        EXPECT_EQ(!std::isnan(row.values[3]), w.ratio);
        EXPECT_EQ(!std::isnan(row.values[4]), w.depth);
        if (w.vel)
            EXPECT_EQ(row.values[2], stat(swmm_stat_link_max_velocity, i));
        if (w.depth)
            EXPECT_EQ(row.values[4], stat(swmm_stat_link_max_filling, i));
    }

    // Flow Classification: the conduits, in link order.
    std::vector<int> conduits;
    for (const char* id : {"C1", "C2", "C3", "C4", "C5", "C6", "CX"})
        conduits.push_back(linkIndex(id));
    std::sort(conduits.begin(), conduits.end());
    ASSERT_EQ(T(SWMM_SUMMARY_FLOW_CLASSIFICATION).rows.size(), conduits.size());
    for (std::size_t r = 0; r < conduits.size(); ++r)
        EXPECT_EQ(T(SWMM_SUMMARY_FLOW_CLASSIFICATION).rows[r].index, conduits[r]);

    // Conduit Surcharge: hours surcharged from the raw statistic, floored at 0.01.
    for (const auto& row : T(SWMM_SUMMARY_CONDUIT_SURCHARGE).rows) {
        EXPECT_EQ(row.type, SWMM_SUMMARY_OBJECT_LINK);
        EXPECT_EQ(row.values[0],
                  std::max(0.01, stat(swmm_stat_link_surcharge_time, row.index) / 3600.0));
    }

    // Pumping: the two pumps.
    ASSERT_EQ(T(SWMM_SUMMARY_PUMPING).rows.size(), 2u);
    EXPECT_EQ(T(SWMM_SUMMARY_PUMPING).rows[0].index, linkIndex("P1"));
    EXPECT_EQ(T(SWMM_SUMMARY_PUMPING).rows[1].index, linkIndex("P2"));

    // LID Performance: one row per unit, keyed by subcatchment, labeled with the control.
    const auto& lids = T(SWMM_SUMMARY_LID_PERFORMANCE).rows;
    ASSERT_EQ(lids.size(), 3u);
    const std::pair<const char*, const char*> units[] = {
        {"BC1", "S1"}, {"RB1", "S1"}, {"BC1", "S2"}};
    for (std::size_t r = 0; r < 3; ++r) {
        EXPECT_EQ(lids[r].type, SWMM_SUMMARY_OBJECT_SUBCATCH);
        EXPECT_EQ(lids[r].label, units[r].first);
        EXPECT_EQ(lids[r].index, subcatchIndex(units[r].second));
        EXPECT_GT(lids[r].values[0], 0.0); // the water balance reached the table before report()
    }

    EXPECT_EQ(swmm_engine_close(e), SWMM_OK);
    swmm_engine_destroy(e);
}

// ===========================================================================
// Lifecycle, indices and buffers
// ===========================================================================

namespace {
/** Every summary call with valid arguments; all must return @p rc. */
void expectEveryCallReturns(SWMM_Engine e, int rc) {
    char buf[64];
    int n = 0, type = 0, index = 0;
    double v = 0.0;
    EXPECT_EQ(swmm_summary_table_title(e, 0, buf, int(sizeof buf)), rc);
    EXPECT_EQ(swmm_summary_column_count(e, 0, &n), rc);
    EXPECT_EQ(swmm_summary_column(e, 0, 0, buf, int(sizeof buf), buf, int(sizeof buf)), rc);
    EXPECT_EQ(swmm_summary_row_count(e, 0, &n), rc);
    EXPECT_EQ(swmm_summary_row(e, 0, 0, &type, &index, buf, int(sizeof buf)), rc);
    EXPECT_EQ(swmm_summary_value(e, 0, 0, 0, &v), rc);
    int types[64], indices[64];
    double values[512];
    EXPECT_EQ(swmm_summary_rows_read(e, 0, types, indices, 64), rc);
    EXPECT_EQ(swmm_summary_values_read(e, 0, values, 512), rc);
    EXPECT_EQ(swmm_summary_labels_read(e, 0, buf, int(sizeof buf), &n), rc);
}
} // namespace

TEST(ReportTables, OnlyAnEndedRunHasTables) {
    expectEveryCallReturns(nullptr, SWMM_ERR_BADHANDLE);

    const std::string inp = writeDeck("lifecycle", kKinwaveDeck);
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    {
        SCOPED_TRACE("CREATED");
        expectEveryCallReturns(e, SWMM_ERR_LIFECYCLE);
    }
    ASSERT_EQ(swmm_engine_open(e, inp.c_str(), (std::string(kDir) + "/lifecycle.rpt").c_str(),
                               (std::string(kDir) + "/lifecycle.out").c_str(), nullptr),
              SWMM_OK);
    {
        SCOPED_TRACE("OPENED");
        expectEveryCallReturns(e, SWMM_ERR_LIFECYCLE);
    }
    ASSERT_EQ(swmm_engine_initialize(e), SWMM_OK);
    {
        SCOPED_TRACE("INITIALIZED");
        expectEveryCallReturns(e, SWMM_ERR_LIFECYCLE);
    }
    ASSERT_EQ(swmm_engine_start(e, 1), SWMM_OK);
    double elapsed = 0.0;
    ASSERT_EQ(swmm_engine_step(e, &elapsed), SWMM_OK);
    {
        SCOPED_TRACE("RUNNING");
        expectEveryCallReturns(e, SWMM_ERR_LIFECYCLE);
    }
    do {
        ASSERT_EQ(swmm_engine_step(e, &elapsed), SWMM_OK);
    } while (elapsed > 0.0);
    ASSERT_EQ(swmm_engine_end(e), SWMM_OK);
    {
        SCOPED_TRACE("ENDED");
        expectEveryCallReturns(e, SWMM_OK);
    }
    ASSERT_EQ(swmm_engine_report(e), SWMM_OK);
    {
        SCOPED_TRACE("ENDED, reported");
        expectEveryCallReturns(e, SWMM_OK);
    }
    ASSERT_EQ(swmm_engine_close(e), SWMM_OK);
    {
        SCOPED_TRACE("CLOSED");
        expectEveryCallReturns(e, SWMM_ERR_LIFECYCLE);
    }
    swmm_engine_destroy(e);
}

TEST(ReportTables, BadIndicesAndBuffersAreRefused) {
    const std::string inp = writeDeck("arguments", kKinwaveDeck);
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_engine_open(e, inp.c_str(), (std::string(kDir) + "/arguments.rpt").c_str(),
                               (std::string(kDir) + "/arguments.out").c_str(), nullptr),
              SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(e), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(e, 1), SWMM_OK);
    double elapsed = 0.0;
    do {
        ASSERT_EQ(swmm_engine_step(e, &elapsed), SWMM_OK);
    } while (elapsed > 0.0);
    ASSERT_EQ(swmm_engine_end(e), SWMM_OK);

    const int depth = SWMM_SUMMARY_NODE_DEPTH;
    int columns = 0, rows = 0, n = 0, type = 7, index = 7;
    ASSERT_EQ(swmm_summary_column_count(e, depth, &columns), SWMM_OK);
    ASSERT_EQ(swmm_summary_row_count(e, depth, &rows), SWMM_OK);
    ASSERT_EQ(columns, 5);
    ASSERT_EQ(rows, 3);
    char buf[64];
    double v = 0.0;

    // Unknown tables.
    for (const int bad : {-1, static_cast<int>(SWMM_SUMMARY_TABLE_COUNT), 1000}) {
        SCOPED_TRACE(bad);
        EXPECT_EQ(swmm_summary_table_title(e, bad, buf, int(sizeof buf)), SWMM_ERR_BADINDEX);
        EXPECT_EQ(swmm_summary_column_count(e, bad, &n), SWMM_ERR_BADINDEX);
        EXPECT_EQ(swmm_summary_column(e, bad, 0, buf, int(sizeof buf), nullptr, 0),
                  SWMM_ERR_BADINDEX);
        EXPECT_EQ(swmm_summary_row_count(e, bad, &n), SWMM_ERR_BADINDEX);
        EXPECT_EQ(swmm_summary_row(e, bad, 0, &type, &index, nullptr, 0), SWMM_ERR_BADINDEX);
        EXPECT_EQ(swmm_summary_value(e, bad, 0, 0, &v), SWMM_ERR_BADINDEX);
    }
    // Columns and rows out of range, including a table with no rows.
    for (const int bad : {-1, columns}) {
        EXPECT_EQ(swmm_summary_column(e, depth, bad, buf, int(sizeof buf), buf, int(sizeof buf)),
                  SWMM_ERR_BADINDEX);
        EXPECT_EQ(swmm_summary_value(e, depth, 0, bad, &v), SWMM_ERR_BADINDEX);
    }
    for (const int bad : {-1, rows, -2}) {
        EXPECT_EQ(swmm_summary_row(e, depth, bad, &type, &index, buf, int(sizeof buf)),
                  SWMM_ERR_BADINDEX);
        EXPECT_EQ(swmm_summary_value(e, depth, bad, 0, &v), SWMM_ERR_BADINDEX);
    }
    EXPECT_EQ(swmm_summary_row(e, SWMM_SUMMARY_PUMPING, 0, &type, &index, nullptr, 0),
              SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_summary_value(e, SWMM_SUMMARY_PUMPING, 0, 0, &v), SWMM_ERR_BADINDEX);

    // Null and zero-length buffers.
    EXPECT_EQ(swmm_summary_table_title(e, depth, nullptr, 64), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_summary_table_title(e, depth, buf, 0), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_summary_table_title(e, depth, buf, -3), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_summary_column_count(e, depth, nullptr), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_summary_row_count(e, depth, nullptr), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_summary_column(e, depth, 0, buf, 0, nullptr, 0), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_summary_column(e, depth, 0, nullptr, 0, buf, 0), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_summary_row(e, depth, 0, nullptr, nullptr, buf, 0), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_summary_value(e, depth, 0, 0, nullptr), SWMM_ERR_BADPARAM);

    // Optional outputs may be null.
    EXPECT_EQ(swmm_summary_column(e, depth, 0, nullptr, 0, nullptr, 0), SWMM_OK);
    EXPECT_EQ(swmm_summary_row(e, depth, 0, nullptr, nullptr, nullptr, 0), SWMM_OK);
    type = index = 7;
    EXPECT_EQ(swmm_summary_row(e, depth, 2, &type, nullptr, nullptr, 0), SWMM_OK);
    EXPECT_EQ(type, SWMM_SUMMARY_OBJECT_NODE);
    EXPECT_EQ(swmm_summary_row(e, depth, 2, nullptr, &index, nullptr, 0), SWMM_OK);
    EXPECT_EQ(index, 2);

    // Truncation keeps the NUL.
    char small[5];
    std::memset(small, 'x', sizeof small);
    EXPECT_EQ(swmm_summary_table_title(e, depth, small, int(sizeof small)), SWMM_OK);
    EXPECT_STREQ(small, "Node");
    char one[1] = {'x'};
    EXPECT_EQ(swmm_summary_column(e, depth, 0, one, 1, nullptr, 0), SWMM_OK);
    EXPECT_EQ(one[0], '\0');
    char label[4];
    EXPECT_EQ(swmm_summary_row(e, depth, 0, nullptr, nullptr, label, int(sizeof label)), SWMM_OK);
    EXPECT_STREQ(label, "JUN");

    // ---- whole-table reads (Node Depth: 3 rows x 5 columns; Pumping: no rows) ----
    int types[4] = {7, 7, 7, 7}, indices[4] = {7, 7, 7, 7};
    double values[16];
    std::fill(std::begin(values), std::end(values), 1234.5);
    for (const int bad : {-1, static_cast<int>(SWMM_SUMMARY_TABLE_COUNT)}) {
        EXPECT_EQ(swmm_summary_rows_read(e, bad, types, indices, 4), SWMM_ERR_BADINDEX);
        EXPECT_EQ(swmm_summary_values_read(e, bad, values, 16), SWMM_ERR_BADINDEX);
        EXPECT_EQ(swmm_summary_labels_read(e, bad, buf, int(sizeof buf), &n), SWMM_ERR_BADINDEX);
    }
    EXPECT_EQ(swmm_summary_rows_read(e, depth, nullptr, indices, 4), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_summary_rows_read(e, depth, types, nullptr, 4), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_summary_rows_read(e, depth, types, indices, -1), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_summary_values_read(e, depth, nullptr, 16), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_summary_values_read(e, depth, values, -1), SWMM_ERR_BADPARAM);
    // Capacity 0 writes nothing; a short capacity writes a prefix only.
    EXPECT_EQ(swmm_summary_rows_read(e, depth, types, indices, 0), SWMM_OK);
    EXPECT_EQ(types[0], 7);
    EXPECT_EQ(swmm_summary_values_read(e, depth, values, 0), SWMM_OK);
    EXPECT_EQ(values[0], 1234.5);
    EXPECT_EQ(swmm_summary_rows_read(e, depth, types, indices, 2), SWMM_OK);
    EXPECT_EQ(types[0], SWMM_SUMMARY_OBJECT_NODE);
    EXPECT_EQ(indices[1], 1);
    EXPECT_EQ(types[2], 7);
    EXPECT_EQ(indices[2], 7);
    EXPECT_EQ(swmm_summary_values_read(e, depth, values, 7), SWMM_OK);
    for (int k = 0; k < 7; ++k) {
        double cell = 0.0;
        ASSERT_EQ(swmm_summary_value(e, depth, k / 5, k % 5, &cell), SWMM_OK);
        EXPECT_EQ(values[k], cell) << k;
    }
    EXPECT_EQ(values[7], 1234.5);

    // Labels: "JUNCTION\nJUNCTION\nOUTFALL" needs 26 bytes with its NUL.
    const std::string joined = "JUNCTION\nJUNCTION\nOUTFALL";
    n = -1;
    EXPECT_EQ(swmm_summary_labels_read(e, depth, nullptr, 0, &n), SWMM_ERR_BADPARAM); // size query
    EXPECT_EQ(n, 26);
    EXPECT_EQ(swmm_summary_labels_read(e, depth, nullptr, 0, nullptr), SWMM_ERR_BADPARAM);
    n = -1;
    EXPECT_EQ(swmm_summary_labels_read(e, depth, nullptr, 40, &n), SWMM_ERR_BADPARAM);
    EXPECT_EQ(n, 26);
    char text[40];
    for (const int short_len : {1, 10, 25}) {
        SCOPED_TRACE(short_len);
        std::memset(text, 'x', sizeof text);
        n = -1;
        EXPECT_EQ(swmm_summary_labels_read(e, depth, text, short_len, &n), SWMM_ERR_BADPARAM);
        EXPECT_EQ(n, 26);
        EXPECT_EQ(text[0], '\0');
        EXPECT_EQ(text[1], 'x');
    }
    std::memset(text, 'x', sizeof text);
    EXPECT_EQ(swmm_summary_labels_read(e, depth, text, 0, &n), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_summary_labels_read(e, depth, text, -1, &n), SWMM_ERR_BADPARAM);
    EXPECT_EQ(text[0], 'x');
    n = -1;
    EXPECT_EQ(swmm_summary_labels_read(e, depth, text, 26, &n), SWMM_OK);
    EXPECT_EQ(n, 26);
    EXPECT_EQ(std::string(text), joined);
    EXPECT_EQ(text[26], 'x');
    EXPECT_EQ(swmm_summary_labels_read(e, depth, text, int(sizeof text), nullptr), SWMM_OK);
    EXPECT_EQ(std::string(text), joined);

    // A table with no rows reads as nothing and "".
    EXPECT_EQ(swmm_summary_rows_read(e, SWMM_SUMMARY_PUMPING, types, indices, 4), SWMM_OK);
    EXPECT_EQ(swmm_summary_values_read(e, SWMM_SUMMARY_PUMPING, values, 16), SWMM_OK);
    n = -1;
    EXPECT_EQ(swmm_summary_labels_read(e, SWMM_SUMMARY_PUMPING, nullptr, 0, &n),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(n, 1);
    EXPECT_EQ(swmm_summary_labels_read(e, SWMM_SUMMARY_PUMPING, text, 1, &n), SWMM_OK);
    EXPECT_STREQ(text, "");

    EXPECT_EQ(swmm_engine_close(e), SWMM_OK);
    swmm_engine_destroy(e);
}
