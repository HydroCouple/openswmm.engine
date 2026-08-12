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
 * @file test_outfall_stage_data.cpp
 * @brief Regression cover for TIDAL/TIMESERIES outfall stage-data references.
 *
 * @details Pins the fix for the v6.0.0-alpha.2 defect in which a
 *          `TIMESERIES` outfall silently drew its stage from the wrong table.
 *
 *          The parser read the stage-data name from `[OUTFALLS]` and discarded
 *          it, deferring resolution to a post-parse pass that did not exist, so
 *          `OutfallData::param` kept its default of 0. Because curves and
 *          timeseries share one index space (`ctx.tables`), 0 is a *valid*
 *          index, and `Outfall.cpp` guarded only with `>= 0` — the outfall
 *          therefore read whichever table happened to be first in the model.
 *          A model whose first table was an unrelated shape curve reported the
 *          curve's y-value as the tailwater stage, with no error.
 *
 *          Covers:
 *            - TIMESERIES outfall resolves to the named series, *not* table 0,
 *              when a decoy curve is declared first (the original repro).
 *            - The reference survives an .inp write/read round-trip, including
 *              when the rewrite renumbers the table indices.
 *            - A FIXED stage survives the round-trip (the writer previously
 *              emitted the stage after the gate flag, so it re-parsed as 0).
 *            - An unresolvable stage-data name is a fatal ERR_NAME, matching
 *              legacy outfall_readParams() rather than silently aliasing a table.
 *            - FREE outfalls accept the EPA-GUI `*` stage-data placeholder
 *              without the gate column shifting.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>

#include "../../../src/engine/core/InpWriter.hpp"
#include "../../../src/engine/core/SWMMEngine.hpp"
#include "../../../src/engine/core/SimulationContext.hpp"
#include "../../../src/engine/data/NodeSubtypes.hpp"
#include "../../../src/engine/data/TableData.hpp"

using openswmm::NodeType;
using openswmm::OutfallType;
using openswmm::SimulationContext;
using openswmm::SWMMEngine;
using openswmm::TableType;

namespace {

// A one-conduit model whose FIRST table is a decoy shape curve (constant 99)
// and whose outfall points at a timeseries (constant 1). Under the defect the
// outfall resolved to table 0 — the curve — and sat at elevation 99.
// `%s` is the [OUTFALLS] stage-data row under test.
constexpr const char* kModelTemplate = R"INP([TITLE]
outfall stage-data regression

[OPTIONS]
FLOW_UNITS           CFS
FLOW_ROUTING         DYNWAVE
START_DATE           07/09/2026
START_TIME           00:00:00
END_DATE             07/09/2026
END_TIME             06:00:00
REPORT_STEP          00:15:00
ROUTING_STEP         0:00:20

[JUNCTIONS]
JUNC1            0          0          0          0          0

[OUTFALLS]
%s

[CONDUITS]
CONDUIT1         JUNC1            OUTFALL1         400        0.01       0          0          0          0

[XSECTIONS]
CONDUIT1         CIRCULAR     1                0          0          0          1

[CURVES]
SHAPECURVE_99    Shape      0          99
SHAPECURVE_99               1          99

[TIMESERIES]
TSERIES_ELEV_01             0          1
TSERIES_ELEV_01             6          1

[REPORT]
NODES ALL
LINKS ALL
)INP";

/// Write `outfall_row` into the template and return the temp .inp path.
std::string writeModel(const std::string& stem, const std::string& outfall_row) {
    const std::string path =
        std::string(testing::TempDir()) + "outfall_stage_" + stem + ".inp";
    std::string inp(kModelTemplate);
    const auto pos = inp.find("%s");
    inp.replace(pos, 2, outfall_row);
    std::ofstream(path) << inp;
    return path;
}

/// The single non-comment row of the [OUTFALLS] section of `path`.
std::string outfallRowOf(const std::string& path) {
    std::ifstream f(path);
    std::string   line;
    bool          in_section = false;
    while (std::getline(f, line)) {
        if (!line.empty() && line[0] == '[')
            in_section = (line.rfind("[OUTFALLS]", 0) == 0);
        else if (in_section && !line.empty() && line[0] != ';' &&
                 line.find_first_not_of(" \t\r") != std::string::npos)
            return line;
    }
    return {};
}

/// Resolved stage-data row for the model's single outfall.
struct Resolved {
    OutfallType bc_type{};
    int         table_index{-1};
    std::string table_id;      ///< empty when table_index is not a valid table
    TableType   table_type{};
};

Resolved firstOutfall(const SimulationContext& ctx) {
    Resolved out;
    for (int i = 0; i < ctx.n_nodes(); ++i) {
        if (ctx.nodes.type[static_cast<std::size_t>(i)] != NodeType::OUTFALL) continue;
        const int r = ctx.node_subtypes.outfall_row(i);
        if (r < 0) continue;
        const auto& O  = ctx.node_subtypes.outfalls;
        const auto  ur = static_cast<std::size_t>(r);
        out.bc_type     = O.bc_type[ur];
        out.table_index = static_cast<int>(O.param[ur]);
        if (out.table_index >= 0 &&
            out.table_index < static_cast<int>(ctx.tables.tables.size())) {
            const auto& t = ctx.tables.tables[static_cast<std::size_t>(out.table_index)];
            out.table_id   = t.id;
            out.table_type = t.type;
        }
        break;
    }
    return out;
}

} // namespace

// The original repro: the decoy shape curve occupies table 0, so an unresolved
// param of 0 aliases it. The outfall must land on the *named* timeseries.
TEST(OutfallStageData, TimeseriesResolvesToNamedSeriesNotTableZero) {
    const std::string inp = writeModel(
        "ts", "OUTFALL1         0          TIMESERIES TSERIES_ELEV_01  NO");

    SWMMEngine e;
    ASSERT_EQ(e.open(inp.c_str(), (inp + ".rpt").c_str(), ""), 0);

    const Resolved r = firstOutfall(e.context());
    EXPECT_EQ(r.bc_type, OutfallType::TIMESERIES);
    EXPECT_EQ(r.table_id, "TSERIES_ELEV_01");
    EXPECT_EQ(r.table_type, TableType::TIMESERIES);

    // The decoy curve is table 0; landing there is the defect.
    EXPECT_EQ(e.context().tables.tables[0].id, "SHAPECURVE_99");
    EXPECT_NE(r.table_index, 0);
}

// The writer used to emit only FIXED's numeric stage, so the timeseries name was
// dropped on save. The rewrite also reorders the tables, so a correct writer must
// round-trip the NAME, not the raw index.
TEST(OutfallStageData, TimeseriesReferenceSurvivesInpRoundTrip) {
    const std::string inp = writeModel(
        "ts_rt", "OUTFALL1         0          TIMESERIES TSERIES_ELEV_01  NO");

    SWMMEngine e1;
    ASSERT_EQ(e1.open(inp.c_str(), (inp + ".rpt").c_str(), ""), 0);

    const std::string out = inp + ".written.inp";
    ASSERT_EQ(openswmm::inp_writer::writeInpFile(e1.context(), out, nullptr), 0);

    // The [OUTFALLS] row must carry the series NAME. Asserting only on the
    // re-parsed index would pass vacuously: the rewrite emits [TIMESERIES]
    // before [CURVES], so the series lands at table 0 — precisely where the
    // old, broken parser already pointed. Pin the emitted text.
    const std::string row = outfallRowOf(out);
    EXPECT_NE(row.find("TIMESERIES"), std::string::npos) << row;
    EXPECT_NE(row.find("TSERIES_ELEV_01"), std::string::npos)
        << "writer dropped the stage-data name: " << row;

    SWMMEngine e2;
    ASSERT_EQ(e2.open(out.c_str(), (out + ".rpt").c_str(), ""), 0)
        << "written .inp must re-parse";

    const Resolved r = firstOutfall(e2.context());
    EXPECT_EQ(r.bc_type, OutfallType::TIMESERIES);
    EXPECT_EQ(r.table_id, "TSERIES_ELEV_01");
    EXPECT_EQ(r.table_type, TableType::TIMESERIES);
}

// The writer emitted "<name> <elev> FIXED <gated> <stage>" — stage *after* the
// gate — so on re-read tok[3] was "NO" and the stage collapsed to 0.
TEST(OutfallStageData, FixedStageSurvivesInpRoundTrip) {
    const std::string inp =
        writeModel("fixed", "OUTFALL1         0          FIXED      7.5              NO");

    SWMMEngine e1;
    ASSERT_EQ(e1.open(inp.c_str(), (inp + ".rpt").c_str(), ""), 0);
    EXPECT_DOUBLE_EQ(firstOutfall(e1.context()).table_index, 7); // param==7.5, truncated

    const std::string out = inp + ".written.inp";
    ASSERT_EQ(openswmm::inp_writer::writeInpFile(e1.context(), out, nullptr), 0);

    SWMMEngine e2;
    ASSERT_EQ(e2.open(out.c_str(), (out + ".rpt").c_str(), ""), 0);

    const int r = e2.context().node_subtypes.outfall_row(
        e2.context().node_names.find("OUTFALL1"));
    ASSERT_GE(r, 0);
    const auto& O = e2.context().node_subtypes.outfalls;
    EXPECT_EQ(O.bc_type[static_cast<std::size_t>(r)], OutfallType::FIXED);
    EXPECT_DOUBLE_EQ(O.param[static_cast<std::size_t>(r)], 7.5);
}

// An unresolvable name must fail loudly (legacy ERR_NAME) rather than silently
// aliasing table 0 — the silence is what let the original defect hide.
TEST(OutfallStageData, UnknownStageDataNameIsFatal) {
    const std::string inp = writeModel(
        "bad", "OUTFALL1         0          TIMESERIES NO_SUCH_SERIES   NO");

    SWMMEngine e;
    EXPECT_NE(e.open(inp.c_str(), (inp + ".rpt").c_str(), ""), 0)
        << "an undefined stage-data name must be a fatal input error";
}

// A TIMESERIES outfall pointing at a CURVE (or a TIDAL outfall pointing at a
// timeseries) is a type error; the shared index space cannot catch it for us.
TEST(OutfallStageData, TimeseriesPointingAtCurveIsFatal) {
    const std::string inp = writeModel(
        "wrongkind", "OUTFALL1         0          TIMESERIES SHAPECURVE_99    NO");

    SWMMEngine e;
    EXPECT_NE(e.open(inp.c_str(), (inp + ".rpt").c_str(), ""), 0)
        << "a TIMESERIES outfall must not accept a curve as its stage series";
}

// FREE/NORMAL carry no stage-data value, but the EPA GUI still emits the column
// as "*". The gate column must not shift when it is present or absent.
TEST(OutfallStageData, FreeOutfallAcceptsStarPlaceholderWithoutShiftingGate) {
    for (const auto& [row, gated] : std::initializer_list<std::pair<const char*, bool>>{
             {"OUTFALL1         0          FREE       *                YES", true},
             {"OUTFALL1         0          FREE       *                NO",  false},
             {"OUTFALL1         0          FREE       YES",                  true},
             {"OUTFALL1         0          FREE       NO",                   false},
         }) {
        const std::string inp = writeModel("free", row);
        SWMMEngine e;
        ASSERT_EQ(e.open(inp.c_str(), (inp + ".rpt").c_str(), ""), 0) << row;

        const int r = e.context().node_subtypes.outfall_row(
            e.context().node_names.find("OUTFALL1"));
        ASSERT_GE(r, 0) << row;
        const auto& O = e.context().node_subtypes.outfalls;
        EXPECT_EQ(O.bc_type[static_cast<std::size_t>(r)], OutfallType::FREE) << row;
        EXPECT_EQ(O.has_flap_gate[static_cast<std::size_t>(r)] != 0, gated) << row;
    }
}
