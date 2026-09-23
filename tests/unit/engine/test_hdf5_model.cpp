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
 * @file test_hdf5_model.cpp
 * @brief The HDF5 columnar model writer — layout, units and fidelity.
 *
 * @details The fixture is the one the file-IO parity audit uses
 *          (`inp_roundtrip/column_and_precision.inp`): every value in it is
 *          chosen so that a dropped column, a shifted column or a truncating
 *          format is unambiguous. Reusing it here means the HDF5 writer is
 *          held to the same standard as the .inp writer, against the same
 *          numbers, rather than to a fixture built to flatter it.
 *
 *          Design: `src/engine/io/hdf5/STRATEGY.md`.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <openswmm/engine/openswmm_engine.h>   // SWMM_OK

#include "core/SWMMEngine.hpp"
#include "io/hdf5/Hdf5InputPlugin.hpp"
#include "io/hdf5/Hdf5ModelWriter.hpp"
#include "io/hdf5/Hdf5Util.hpp"

#include <string>
#include <vector>

using namespace openswmm;
using namespace openswmm::h5io;

namespace {

std::string data(const std::string& name) {
    return "inp_roundtrip/" + name;
}

/// Open the audit fixture and write it as HDF5. Artifacts land beside the
/// fixture, reviewable on disk (CLAUDE.md §4.1), not in a temp directory.
std::string writeFixture(const std::string& fixture, const std::string& out_name) {
    SWMMEngine eng;
    eng.set_lenient_open(true);
    const int rc = eng.open(data(fixture).c_str(), nullptr, nullptr);
    EXPECT_EQ(rc, SWMM_OK) << "open(" << fixture << ") failed";
    if (rc != SWMM_OK) return {};

    const std::string out = data(out_name);
    std::vector<std::string> warnings;
    EXPECT_EQ(write_model(out, eng.context(), &warnings), 0);
    return out;
}

/// Row index of `id` in a group's `name`-like column, or -1.
int find_row(hid_t g, const char* col, const std::string& id) {
    std::vector<std::string> names;
    if (!read_column(g, col, names)) return -1;
    for (std::size_t i = 0; i < names.size(); ++i)
        if (names[i] == id) return static_cast<int>(i);
    return -1;
}

/// Every HDF5 call goes through the engine's h5io helpers, never through the
/// H5* API directly. HDF5 is PRIVATE-linked into the engine with hidden
/// symbols, so a test that linked its own copy would be handing ids from one
/// HDF5 instance to another — which fails exactly as if the file were corrupt.
struct File {
    Handle h;
    explicit File(const std::string& p) : h(open_file_readonly(p)) {}
    bool ok() const { return h.ok(); }
    hid_t get() const { return h.get(); }
};

}  // namespace

// The container must say what convention its numbers follow. A consumer that
// assumes the wrong one is wrong by 43200 on seepage or 3.28084 on lengths,
// and nothing in the file would contradict them.
TEST(Hdf5Model, RootAttributesDeclareTheUnitConvention) {
    const auto path = writeFixture("column_and_precision.inp", "_cp_model.h5");
    ASSERT_FALSE(path.empty());
    File f(path);
    ASSERT_TRUE(f.ok());

    std::string v;
    ASSERT_TRUE(read_attr(f.get(), "schema_version", v));
    EXPECT_EQ(v, kSchemaVersion);
    ASSERT_TRUE(read_attr(f.get(), "units_convention", v));
    EXPECT_EQ(v, "authored") << "the file must declare authored, not internal, units";
    ASSERT_TRUE(read_attr(f.get(), "flow_units", v));
    EXPECT_EQ(v, "CFS");
    ASSERT_TRUE(read_attr(f.get(), "unit_system", v));
    EXPECT_EQ(v, "US");
}

// The defect that opened the file-IO audit: the culvert code drives inlet
// control and used to vanish on save. It gets a first-class column here, so
// it cannot be lost by running off the end of a row.
TEST(Hdf5Model, CrossSectionsCarryRawGeomsAndTheCulvertCode) {
    const auto path = writeFixture("column_and_precision.inp", "_cp_model.h5");
    ASSERT_FALSE(path.empty());
    File f(path);
    ASSERT_TRUE(f.ok());
    Handle g = open_group(f.get(), "links/xsections");
    ASSERT_TRUE(g.ok());

    const int i = find_row(g.get(), "link", "C_CULVERT");
    ASSERT_GE(i, 0) << "C_CULVERT missing from links/xsections";

    std::vector<std::string> shape;
    std::vector<double> g1;
    std::vector<int> barrels, culvert;
    ASSERT_TRUE(read_column(g.get(), "shape", shape));
    ASSERT_TRUE(read_column(g.get(), "geom1", g1));
    ASSERT_TRUE(read_column(g.get(), "barrels", barrels));
    ASSERT_TRUE(read_column(g.get(), "culvert_code", culvert));

    // The shape is the legacy KEYWORD, never an enum ordinal: two enums in
    // this codebase disagree on ordering and an `enum + 1` translation between
    // them was once the largest parity defect in the project.
    EXPECT_EQ(shape.at(static_cast<std::size_t>(i)), "CIRCULAR");
    EXPECT_NEAR(g1.at(static_cast<std::size_t>(i)), 1.5, 1e-12);
    EXPECT_EQ(barrels.at(static_cast<std::size_t>(i)), 1);
    EXPECT_EQ(culvert.at(static_cast<std::size_t>(i)), 4) << "culvert code lost";

    const int j = find_row(g.get(), "link", "C_SEEP");
    ASSERT_GE(j, 0);
    EXPECT_EQ(culvert.at(static_cast<std::size_t>(j)), 0) << "0 means not a culvert";
}

// Full IEEE-754 doubles, not a formatted approximation. This is the whole
// advantage of a binary container over text, and it is the defect class that
// drove a quarter of the corpus to simulate differently after a save.
TEST(Hdf5Model, SolverFacingValuesKeepFullDoublePrecision) {
    const auto path = writeFixture("column_and_precision.inp", "_cp_model.h5");
    ASSERT_FALSE(path.empty());
    File f(path);
    ASSERT_TRUE(f.ok());

    Handle sub = open_group(f.get(), "subcatchments");
    ASSERT_TRUE(sub.ok());
    std::vector<double> area;
    ASSERT_TRUE(read_column(sub.get(), "area", area));
    ASSERT_EQ(area.size(), 1u);
    EXPECT_DOUBLE_EQ(area[0], 0.018939394);

    Handle st = open_group(f.get(), "nodes/storage");
    ASSERT_TRUE(st.ok());
    std::vector<double> a1;
    ASSERT_TRUE(read_column(st.get(), "a1", a1));
    ASSERT_EQ(a1.size(), 1u);
    // A(d) = A0 + A1*d^A2, and the [STORAGE] column order is A1 A2 A0 — the
    // store's a/b/c are coefficient/exponent/constant, which is easy to
    // scramble and was scrambled here once.
    EXPECT_DOUBLE_EQ(a1[0], 278.539816);
}

// Seepage arrives and leaves in in/hr while the solver consumes ft/s. Writing
// the internal value under an in/hr label is exactly the defect the audit
// found in the .inp writer; the label makes the claim checkable.
TEST(Hdf5Model, SeepageIsAuthoredRateWithItsUnitLabel) {
    const auto path = writeFixture("column_and_precision.inp", "_cp_model.h5");
    ASSERT_FALSE(path.empty());
    File f(path);
    ASSERT_TRUE(f.ok());
    Handle g = open_group(f.get(), "links/conduits");
    ASSERT_TRUE(g.ok());

    const int i = find_row(g.get(), "name", "C_SEEP");
    ASSERT_GE(i, 0);
    std::vector<double> seep;
    ASSERT_TRUE(read_column(g.get(), "seep_rate", seep));
    EXPECT_NEAR(seep.at(static_cast<std::size_t>(i)), 0.25, 1e-12)
        << "seepage written in internal ft/s rather than authored in/hr";

    EXPECT_EQ(column_units(g.get(), "seep_rate"), "in/hr");
}

// Dividers carry the full optional tail. The .inp reader used to drop
// InitDepth, SurDepth and Aponded outright and read an OVERFLOW divider's
// MaxDepth from the wrong column.
TEST(Hdf5Model, DividerAndNodeColumnsSurvive) {
    const auto path = writeFixture("column_and_precision.inp", "_cp_model.h5");
    ASSERT_FALSE(path.empty());
    File f(path);
    ASSERT_TRUE(f.ok());

    Handle nodes = open_group(f.get(), "nodes");
    ASSERT_TRUE(nodes.ok());
    const int i = find_row(nodes.get(), "name", "DIV_OVR");
    ASSERT_GE(i, 0);
    const auto ui = static_cast<std::size_t>(i);

    std::vector<double> full, init, sur, ponded;
    ASSERT_TRUE(read_column(nodes.get(), "full_depth", full));
    ASSERT_TRUE(read_column(nodes.get(), "init_depth", init));
    ASSERT_TRUE(read_column(nodes.get(), "sur_depth", sur));
    ASSERT_TRUE(read_column(nodes.get(), "ponded_area", ponded));
    EXPECT_NEAR(full.at(ui), 6.07, 1e-9);
    EXPECT_NEAR(init.at(ui), 1.11, 1e-9);
    EXPECT_NEAR(sur.at(ui), 2.22, 1e-9);
    EXPECT_NEAR(ponded.at(ui), 200.0, 1e-9);

    Handle div = open_group(f.get(), "nodes/dividers");
    ASSERT_TRUE(div.ok());
    std::vector<std::string> method;
    ASSERT_TRUE(read_column(div.get(), "method", method));
    const int d = find_row(div.get(), "name", "DIV_OVR");
    ASSERT_GE(d, 0);
    EXPECT_EQ(method.at(static_cast<std::size_t>(d)), "OVERFLOW");
}

// Every column in a group must agree on length, or a columnar reader cannot
// zip them into rows. An empty section is a zero-length dataset, not a
// missing one.
TEST(Hdf5Model, ColumnsWithinAGroupShareOneLength) {
    const auto path = writeFixture("column_and_precision.inp", "_cp_model.h5");
    ASSERT_FALSE(path.empty());
    File f(path);
    ASSERT_TRUE(f.ok());

    for (const char* grp : {"nodes", "links", "links/xsections", "subcatchments",
                            "links/conduits", "nodes/dividers"}) {
        Handle g = open_group(f.get(), grp);
        ASSERT_TRUE(g.ok()) << grp << " missing";
        const std::size_t n = column_length(g.get(), "name")
                            + column_length(g.get(), "link");
        for (const char* col : {"name", "link", "type", "shape", "geom1",
                                "invert_elev", "area", "length", "method"}) {
            const std::size_t m = column_length(g.get(), col);
            if (m == 0) continue;  // column absent from this group
            EXPECT_EQ(m, n) << grp << "/" << col << " disagrees on row count";
        }
    }
}

// Ragged tables use the CSR idiom: a flat value array plus offsets of length
// n+1. A malformed offsets array silently truncates the last object, so the
// invariant is asserted rather than assumed.
TEST(Hdf5Model, RaggedTablesUseWellFormedCsrOffsets) {
    const auto path = writeFixture("column_and_precision.inp", "_cp_model.h5");
    ASSERT_FALSE(path.empty());
    File f(path);
    ASSERT_TRUE(f.ok());

    Handle g = open_group(f.get(), "tables/timeseries");
    ASSERT_TRUE(g.ok());
    std::vector<std::string> name;
    std::vector<int> offs;
    std::vector<double> time, value;
    ASSERT_TRUE(read_column(g.get(), "name", name));
    ASSERT_TRUE(read_column(g.get(), "offsets", offs));
    ASSERT_TRUE(read_column(g.get(), "time", time));
    ASSERT_TRUE(read_column(g.get(), "value", value));

    ASSERT_EQ(offs.size(), name.size() + 1) << "offsets must be n+1";
    EXPECT_EQ(offs.front(), 0);
    EXPECT_EQ(static_cast<std::size_t>(offs.back()), time.size());
    EXPECT_EQ(time.size(), value.size());
    for (std::size_t i = 1; i < offs.size(); ++i)
        EXPECT_LE(offs[i - 1], offs[i]) << "offsets must be non-decreasing";
}

// Reading is not implemented in schema 1.0. It must REFUSE rather than
// half-populate a context: a partially loaded model still runs and still
// produces numbers, which is a far worse failure than not opening.
TEST(Hdf5Model, ReadIsRefusedRatherThanPartiallyImplemented) {
    const auto path = writeFixture("column_and_precision.inp", "_cp_model.h5");
    ASSERT_FALSE(path.empty());

    Hdf5InputPlugin plugin;
    ASSERT_EQ(plugin.initialize({}, nullptr), 0);
    SimulationContext ctx;
    EXPECT_NE(plugin.read(path, ctx), 0);
    EXPECT_NE(std::string(plugin.last_error_message()).find("not implemented"),
              std::string::npos);
    EXPECT_EQ(ctx.n_nodes(), 0) << "a refused read must leave the context untouched";
}
