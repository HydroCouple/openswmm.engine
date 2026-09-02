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
 * @file test_inp_writer_roundtrip.cpp
 * @brief Eleven `.inp` writer defects, three of them fatal on reload and the
 *        rest silent data loss. One test per defect.
 *
 * @details Every defect here had the same shape: the writer consulted only a
 *          resolved index, and emitted `*` (or nothing at all) when that index
 *          was -1 — even though the model still held the name. Three of the ten
 *          produced an `.inp` the engine then refused to re-open
 *          (`ERROR 209: undefined object *`), so the GUI's Save destroyed the
 *          model it had just written.
 *
 *          Several defects are only reachable through a fixture that is easy
 *          to get wrong, and every one of them silently PASSES against the
 *          unfixed engine if the fixture is not exact. Each was confirmed by
 *          building this file against the pre-fix engine and checking it fails:
 *
 *          - Defect 5 needs `[INLET_USAGE]` **after** the node sections.
 *            `handle_inlet_usage()` resolves link/inlet/node eagerly and drops
 *            any row it cannot resolve, with no deferred pass, so a
 *            forward-referenced node means the READER discards the row and the
 *            writer never sees it — the test would then be asserting on the
 *            wrong component. `section_order.inp` carries exactly that shape
 *            and is deliberately NOT used for defect 5; `inlet_usage.inp` is.
 *          - Defect 9 needs a gage with neither a resolved series index nor a
 *            file path. Any fixture whose gages all resolve leaves the branch
 *            unexecuted. `gage_nosource.inp` names an undefined series.
 *          - Defects 6, 7 and 8 need the referenced gage / pump curve /
 *            snowpack to be MISSING. Against a model where they all resolve,
 *            the old index-only writer produced exactly the right names and
 *            these tests passed while testing nothing. A dangling reference is
 *            fatal on a strict open, so `dangling_refs.inp` is opened the way
 *            the GUI opens a model for editing — leniently.
 *
 *          Per CLAUDE.md §4.1 every artifact is written next to the fixtures,
 *          under `tests/unit/engine/data/inp_roundtrip/`, so a reviewer can
 *          open the produced `.inp` directly. Generated names carry the `_`
 *          prefix that `.gitignore` excludes; files are overwritten each run
 *          and no temporary directory is used.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../../src/engine/core/SWMMEngine.hpp"
#include "../../src/engine/core/SimulationContext.hpp"
#include "../../src/engine/core/InpWriter.hpp"

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_subcatchments.h>
#include <openswmm/engine/openswmm_tables.h>

namespace {

using openswmm::SimulationContext;
using openswmm::SWMMEngine;

constexpr const char* kDir = "inp_roundtrip/";

std::string data(const std::string& name) { return kDir + name; }

std::string slurp(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// Body of one `.inp` section: the lines after the `[NAME]` header and before
/// the next one, with `;;` comment rows and blank lines removed. Returns an
/// empty vector when the section is absent — which for most of these defects
/// IS the failure, so callers assert on emptiness explicitly.
std::vector<std::string> section(const std::string& text, const std::string& name) {
    std::vector<std::string> rows;
    std::istringstream in(text);
    std::string line;
    bool inside = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string trimmed = line;
        while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front())))
            trimmed.erase(trimmed.begin());
        if (!trimmed.empty() && trimmed.front() == '[') {
            inside = (trimmed.rfind("[" + name + "]", 0) == 0);
            continue;
        }
        if (!inside) continue;
        if (trimmed.empty() || trimmed.rfind(";;", 0) == 0 || trimmed.front() == ';') continue;
        rows.push_back(trimmed);
    }
    return rows;
}

/// Whitespace-split one row, so assertions name columns rather than counting
/// spaces (the writer's column widths are not part of any contract).
std::vector<std::string> cols(const std::string& row) {
    std::istringstream in(row);
    std::vector<std::string> out;
    std::string tok;
    while (in >> tok) out.push_back(tok);
    return out;
}

/// The row of `sec` whose first column is `id`, or an empty vector.
std::vector<std::string> row(const std::string& text, const std::string& sec,
                             const std::string& id) {
    for (const auto& r : section(text, sec)) {
        auto c = cols(r);
        if (!c.empty() && c[0] == id) return c;
    }
    return {};
}

/// Open a fixture and write it back out. Fails the calling test (via the
/// gtest fatal-assertion protocol) if the open or the write reports an error.
/// `warnings` receives the writer's own diagnostics — a `… row omitted`
/// warning means an index failed to resolve, which is a defect, not noise.
struct Generation {
    std::string text;
    std::vector<std::string> warnings;
};

Generation writeOnce(const std::string& in_name, const std::string& out_name) {
    Generation g;
    SWMMEngine eng;
    const int rc = eng.open(data(in_name).c_str(),
                            data("_" + in_name + ".rpt").c_str(), nullptr);
    EXPECT_EQ(rc, SWMM_OK) << "open(" << in_name << ") failed";
    for (const auto& e : eng.context().errors) ADD_FAILURE() << in_name << ": " << e;
    if (rc != SWMM_OK) return g;

    const std::string out = data(out_name);
    EXPECT_EQ(openswmm::inp_writer::writeInpFile(eng.context(), out, &g.warnings), 0)
        << "writeInpFile(" << out_name << ") failed";
    for (const auto& w : g.warnings) ADD_FAILURE() << in_name << " writer warning: " << w;
    g.text = slurp(out);
    return g;
}


/// A lenient open, the mode the GUI uses to load a model for editing
/// (`swmm_engine_set_lenient_open`). Unresolved references are recorded as
/// errors but the model still loads, keeping the parser's retained names with
/// their indices at -1 — the only state in which defects 6, 7 and 8 are
/// reachable. Expected errors are NOT failed on here; the caller asserts on
/// what the writer then does with those names.
Generation writeOnceLenient(const std::string& in_name, const std::string& out_name) {
    Generation g;
    SWMMEngine eng;
    eng.set_lenient_open(true);
    const int rc = eng.open(data(in_name).c_str(),
                            data("_" + in_name + ".rpt").c_str(), nullptr);
    EXPECT_EQ(rc, SWMM_OK) << "lenient open(" << in_name << ") failed";
    if (rc != SWMM_OK) return g;

    const std::string out = data(out_name);
    EXPECT_EQ(openswmm::inp_writer::writeInpFile(eng.context(), out, &g.warnings), 0);
    g.text = slurp(out);
    return g;
}

/// Generation 1 of a fixture: what the GUI's Save would produce from it.
Generation gen1(const std::string& fixture) {
    return writeOnce(fixture, "_" + fixture.substr(0, fixture.size() - 4) + "_rt1.inp");
}

/// A C API handle on an already-written file, so the assertions run against
/// the surface the GUI actually uses rather than engine internals.
struct Reopened {
    SWMM_Engine e = nullptr;
    Reopened(const std::string& name) {
        e = swmm_engine_create();
        const int rc = swmm_engine_open(e, data(name).c_str(),
                                        data("_" + name + ".rpt").c_str(), nullptr, nullptr);
        EXPECT_EQ(rc, SWMM_OK) << "reopen(" << name << "): "
                               << (swmm_get_last_error_msg(e) ? swmm_get_last_error_msg(e) : "");
        for (int i = 0; i < swmm_get_error_count(e); ++i)
            ADD_FAILURE() << "reopen(" << name << "): " << swmm_get_error_at(e, i);
    }
    ~Reopened() { if (e) { swmm_engine_close(e); swmm_engine_destroy(e); } }
};

}  // namespace

// ===========================================================================
// Defect 1 — [SUBCATCHMENTS] Outlet
//
// A subcatchment draining to another subcatchment has outlet_node == -1; the
// writer consulted only the node index, so the column saved as '*' and the
// model would not reload.
// ===========================================================================

TEST(InpWriterRoundTrip, SubcatchmentOutletKeepsTheSubcatchmentName) {
    const auto g = gen1("hydrology.inp");
    ASSERT_FALSE(g.text.empty());

    EXPECT_EQ(row(g.text, "SUBCATCHMENTS", "S1").at(2), "S2");
    EXPECT_EQ(row(g.text, "SUBCATCHMENTS", "S2").at(2), "J1");  // a real node still wins
    EXPECT_EQ(row(g.text, "SUBCATCHMENTS", "S3").at(2), "S1");
}

// The extreme form, and the one the field model that exposed this hit: a
// subcatchment whose outlet is itself. There is no outlet node of any kind.
TEST(InpWriterRoundTrip, ASelfDrainingSubcatchmentKeepsItsOwnName) {
    const auto g = gen1("self_outlet.inp");
    ASSERT_FALSE(g.text.empty());

    EXPECT_EQ(row(g.text, "SUBCATCHMENTS", "S3").at(2), "S3");
}

// ===========================================================================
// Defect 2 — [AQUIFERS] parsed but never written
// ===========================================================================

TEST(InpWriterRoundTrip, AquifersSurviveWithTheOptionalEvapPattern) {
    const auto g = gen1("hydrology.inp");
    ASSERT_FALSE(g.text.empty());

    ASSERT_EQ(section(g.text, "AQUIFERS").size(), 2u) << "[AQUIFERS] missing or short";

    // The 14th column is optional. AQ1 carries it, AQ2 does not — pinning both
    // halves, because writing it unconditionally would shift nothing here but
    // would hand the reader an empty token on every pattern-less aquifer.
    const auto aq1 = row(g.text, "AQUIFERS", "AQ1");
    const auto aq2 = row(g.text, "AQUIFERS", "AQ2");
    ASSERT_EQ(aq1.size(), 14u);
    ASSERT_EQ(aq2.size(), 13u);
    EXPECT_EQ(aq1.at(13), "MPAT");
    EXPECT_EQ(aq1.at(1), "0.5");   // porosity
    EXPECT_EQ(aq1.at(4), "5");     // ksat
}

TEST(InpWriterRoundTrip, AquiferParametersSurviveThroughTheApi) {
    gen1("hydrology.inp");
    Reopened m("_hydrology_rt1.inp");
    ASSERT_NE(m.e, nullptr);

    ASSERT_EQ(swmm_aquifer_count(m.e), 2);
    const int a1 = swmm_aquifer_index(m.e, "AQ1");
    const int a2 = swmm_aquifer_index(m.e, "AQ2");
    ASSERT_GE(a1, 0);
    ASSERT_GE(a2, 0);

    double por = 0.0, ksat = 0.0, ebot = -1.0;
    ASSERT_EQ(swmm_aquifer_get_param(m.e, a1, SWMM_AQUIFER_POROSITY, &por), SWMM_OK);
    ASSERT_EQ(swmm_aquifer_get_param(m.e, a1, SWMM_AQUIFER_CONDUCTIVITY, &ksat), SWMM_OK);
    ASSERT_EQ(swmm_aquifer_get_param(m.e, a1, SWMM_AQUIFER_BOTTOM_ELEV, &ebot), SWMM_OK);
    EXPECT_DOUBLE_EQ(por, 0.5);
    EXPECT_DOUBLE_EQ(ksat, 5.0);
    EXPECT_DOUBLE_EQ(ebot, 0.0);

    char pat[64] = {0};
    ASSERT_EQ(swmm_aquifer_get_evap_pattern(m.e, a1, pat, sizeof pat), SWMM_OK);
    EXPECT_STREQ(pat, "MPAT");
    char none[64] = {'x', 0};
    ASSERT_EQ(swmm_aquifer_get_evap_pattern(m.e, a2, none, sizeof none), SWMM_OK);
    EXPECT_STREQ(none, "");
}

// ===========================================================================
// Defect 3 — [GROUNDWATER] receiving node forward reference
//
// [GROUNDWATER] normally precedes [JUNCTIONS], and handle_groundwater()
// resolved eagerly with no deferred pass. This was a live runtime bug as well
// as a write bug: groundwater discharged to the subcatchment's outlet node
// instead of the specified one.
// ===========================================================================

TEST(InpWriterRoundTrip, GroundwaterNodeResolvesThroughAForwardReference) {
    // The source model, before anything is written: J2 is defined after the
    // [GROUNDWATER] section, so this is the deferred pass under test.
    SWMMEngine eng;
    ASSERT_EQ(eng.open(data("hydrology.inp").c_str(),
                       data("_hydrology_gw.rpt").c_str(), nullptr), SWMM_OK);
    const auto& ctx = eng.context();

    const int s1 = ctx.subcatch_names.find("S1");
    ASSERT_GE(s1, 0);
    const int node = ctx.subcatches.gw_node[static_cast<std::size_t>(s1)];
    ASSERT_GE(node, 0) << "S1 groundwater node never resolved";
    EXPECT_EQ(ctx.node_names.name_of(node), "J2");

    // S1's outlet is the subcatchment S2, so a fallback to the outlet node
    // could not have produced J2 by accident.
    EXPECT_NE(ctx.node_names.name_of(node), "J1");
}

TEST(InpWriterRoundTrip, GroundwaterRowIsWrittenWithItsNode) {
    const auto g = gen1("hydrology.inp");
    ASSERT_FALSE(g.text.empty());

    ASSERT_EQ(section(g.text, "GROUNDWATER").size(), 2u) << "[GROUNDWATER] missing";
    const auto s1 = row(g.text, "GROUNDWATER", "S1");
    ASSERT_GE(s1.size(), 3u);
    EXPECT_EQ(s1.at(1), "AQ1");
    EXPECT_EQ(s1.at(2), "J2");
}

TEST(InpWriterRoundTrip, GroundwaterNodeSurvivesTheRoundTripThroughTheApi) {
    gen1("hydrology.inp");
    Reopened m("_hydrology_rt1.inp");
    ASSERT_NE(m.e, nullptr);

    const int si = swmm_subcatch_index(m.e, "S1");
    ASSERT_GE(si, 0);
    int gw = -1;
    ASSERT_EQ(swmm_subcatch_get_gw_node(m.e, si, &gw), SWMM_OK);
    ASSERT_GE(gw, 0);
    EXPECT_STREQ(swmm_node_id(m.e, gw), "J2");
}

// ===========================================================================
// Defect 4 — [GWF] round-tripped through the [OPTIONS] passthrough
//
// Expressions live in options.ext_options under "GWF:<sub>:<type>".
// handle_options() uppercases the key and keeps only the first value token, so
// the passthrough mangled the key case (breaking the lookup that reads them
// back) and truncated any real expression.
// ===========================================================================

TEST(InpWriterRoundTrip, GwfExpressionsAreWrittenWholeAndNotAsOptions) {
    const auto g = gen1("hydrology.inp");
    ASSERT_FALSE(g.text.empty());

    const auto gwf = section(g.text, "GWF");
    ASSERT_EQ(gwf.size(), 2u) << "[GWF] missing";

    // Multi-token expression, intact. Compared as whole rows because the
    // failure mode was truncation to the first token ("0.001").
    EXPECT_NE(gwf[0].find("0.001 * (Hgw - 10.0)"), std::string::npos) << gwf[0];
    EXPECT_NE(gwf[1].find("0.0005 * Hgw"), std::string::npos) << gwf[1];
    EXPECT_EQ(cols(gwf[0]).at(1), "LATERAL");
    EXPECT_EQ(cols(gwf[1]).at(1), "DEEP");

    // And nothing leaked into [OPTIONS]: a surviving "GWF:S1:LATERAL" line
    // there is the mangled form, which reloads as an unusable option.
    for (const auto& opt : section(g.text, "OPTIONS"))
        EXPECT_NE(cols(opt).at(0).rfind("GWF:", 0), 0u) << "GWF key leaked: " << opt;
}

// ===========================================================================
// Defect 5 — [INLET_USAGE] parsed but never written
//
// Uses inlet_usage.inp, NOT section_order.inp: see the file header.
// ===========================================================================

TEST(InpWriterRoundTrip, InletUsageIsWritten) {
    const auto g = gen1("inlet_usage.inp");
    ASSERT_FALSE(g.text.empty());

    const auto rows = section(g.text, "INLET_USAGE");
    ASSERT_EQ(rows.size(), 1u) << "[INLET_USAGE] missing";
    const auto c = cols(rows[0]);
    ASSERT_EQ(c.size(), 9u);
    EXPECT_EQ(c[0], "GUT1");
    EXPECT_EQ(c[1], "IN_A");
    EXPECT_EQ(c[2], "J5");
    EXPECT_EQ(c[3], "1");
    EXPECT_EQ(c[4], "0");         // %Clog, stored as 1 - pct/100
    EXPECT_EQ(c[8], "ON_GRADE");
}

TEST(InpWriterRoundTrip, InletUsageIsStillThereAfterReopening) {
    gen1("inlet_usage.inp");

    SWMMEngine eng;
    ASSERT_EQ(eng.open(data("_inlet_usage_rt1.inp").c_str(),
                       data("_inlet_usage_reopen.rpt").c_str(), nullptr), SWMM_OK);
    const auto& ctx = eng.context();

    ASSERT_EQ(ctx.inlet_usages.count(), 1) << "inlet usage lost on the round trip";
    EXPECT_EQ(ctx.link_names.name_of(ctx.inlet_usages.link_index[0]), "GUT1");
    EXPECT_EQ(ctx.node_names.name_of(ctx.inlet_usages.node_index[0]), "J5");
    EXPECT_EQ(ctx.inlets.names[static_cast<std::size_t>(ctx.inlet_usages.design_index[0])],
              "IN_A");
    EXPECT_EQ(ctx.inlet_usages.num_inlets[0], 1);
    EXPECT_DOUBLE_EQ(ctx.inlet_usages.clog_factor[0], 1.0);
    EXPECT_EQ(ctx.inlet_usages.placement[0], 1);  // ON_GRADE
}

// ===========================================================================
// Defect 6 — [SUBCATCHMENTS] RainGage '*'
// ===========================================================================

TEST(InpWriterRoundTrip, SubcatchmentRainGageKeepsItsName) {
    // A resolvable gage was never the failing case — the index alone produced
    // the right name. The defect only appears when the index is -1, so this
    // asserts on the dangling-reference fixture. hydrology.inp is checked too,
    // to pin that the normal path did not regress.
    const auto d = writeOnceLenient("dangling_refs.inp", "_dangling_refs_rt1.inp");
    ASSERT_FALSE(d.text.empty());
    const auto s1 = row(d.text, "SUBCATCHMENTS", "S1");
    ASSERT_GE(s1.size(), 2u);
    EXPECT_EQ(s1.at(1), "RGX") << "unresolved gage written as '*' — fatal on reload";

    const auto g = gen1("hydrology.inp");
    ASSERT_FALSE(g.text.empty());
    for (const auto& r : section(g.text, "SUBCATCHMENTS"))
        EXPECT_EQ(cols(r).at(1), "RG1") << r;
}

// ===========================================================================
// Defect 7 — [PUMPS] curve silently downgraded to IDEAL
//
// '*' is the legal ideal-pump placeholder (see test_pump_ideal_curve.cpp), so
// this defect was invisible on reload: the model opened fine and simply pumped
// differently.
// ===========================================================================

TEST(InpWriterRoundTrip, PumpKeepsItsCurveInsteadOfBecomingIdeal) {
    // As with the gage: a resolvable curve was already written correctly, so
    // the discriminating case is the unresolved one. '*' is the legal IDEAL
    // placeholder, which is what made this defect silent — the model reloads
    // cleanly and simply pumps differently.
    const auto d = writeOnceLenient("dangling_refs.inp", "_dangling_refs_rt1.inp");
    ASSERT_FALSE(d.text.empty());
    const auto dp = row(d.text, "PUMPS", "P1");
    ASSERT_GE(dp.size(), 5u);
    EXPECT_EQ(dp.at(3), "PCX") << "pump silently downgraded to IDEAL";

    const auto g = gen1("hydrology.inp");
    ASSERT_FALSE(g.text.empty());
    const auto p1 = row(g.text, "PUMPS", "P1");
    ASSERT_GE(p1.size(), 5u);
    EXPECT_EQ(p1.at(3), "PC1");
    EXPECT_EQ(p1.at(4), "ON");
}

TEST(InpWriterRoundTrip, PumpCurveStillResolvesAfterReopening) {
    gen1("hydrology.inp");
    Reopened m("_hydrology_rt1.inp");
    ASSERT_NE(m.e, nullptr);

    const int li = swmm_link_index(m.e, "P1");
    ASSERT_GE(li, 0);
    int curve = -1;
    ASSERT_EQ(swmm_link_get_pump_curve(m.e, li, &curve), SWMM_OK);
    ASSERT_GE(curve, 0) << "pump silently became IDEAL";
    EXPECT_STREQ(swmm_table_id(m.e, curve), "PC1");
}

// ===========================================================================
// Defect 8 — [SUBCATCHMENTS] Snowpack name dropped
// ===========================================================================

TEST(InpWriterRoundTrip, SubcatchmentKeepsItsSnowpack) {
    // The unresolved case dropped the column outright rather than writing '*',
    // so the assertion that has teeth is on the column's presence.
    const auto d = writeOnceLenient("dangling_refs.inp", "_dangling_refs_rt1.inp");
    ASSERT_FALSE(d.text.empty());
    const auto ds1 = row(d.text, "SUBCATCHMENTS", "S1");
    ASSERT_EQ(ds1.size(), 9u) << "snowpack column dropped for an unresolved pack";
    EXPECT_EQ(ds1.at(8), "SPX");

    const auto g = gen1("hydrology.inp");
    ASSERT_FALSE(g.text.empty());

    const auto s1 = row(g.text, "SUBCATCHMENTS", "S1");
    ASSERT_EQ(s1.size(), 9u) << "snowpack column absent entirely";
    EXPECT_EQ(s1.at(8), "SP1");

    // S2 has no pack and no scale factors, so token 8 is correctly omitted
    // rather than padded with '*' — the column is positional and a stray
    // placeholder would shift tokens 9 and 10.
    EXPECT_EQ(row(g.text, "SUBCATCHMENTS", "S2").size(), 8u);
}

// ===========================================================================
// Defect 9 — a [RAINGAGES] row could vanish entirely
//
// The ts_index / file_path chain had no else, so a gage with neither wrote
// nothing while [SUBCATCHMENTS] still named it — fatal, and on the
// *subcatchment*, which makes it hard to trace back to the gage.
// ===========================================================================

TEST(InpWriterRoundTrip, AGageWithNoResolvableSourceStillWritesItsRow) {
    const auto g = gen1("gage_nosource.inp");
    ASSERT_FALSE(g.text.empty());

    ASSERT_EQ(section(g.text, "RAINGAGES").size(), 2u)
        << "a gage row vanished; the subcatchment naming it will not reload";
    const auto rg2 = row(g.text, "RAINGAGES", "RG2");
    ASSERT_GE(rg2.size(), 6u);
    EXPECT_EQ(rg2.at(4), "TIMESERIES");
    EXPECT_EQ(rg2.at(5), "TSX");  // the retained name, not a dropped row

    // The reference that would have gone fatal.
    EXPECT_EQ(row(g.text, "SUBCATCHMENTS", "S2").at(1), "RG2");
}

TEST(InpWriterRoundTrip, AGageWithNoResolvableSourceStillReopens) {
    gen1("gage_nosource.inp");
    Reopened m("_gage_nosource_rt1.inp");
    EXPECT_NE(m.e, nullptr);
}

// ===========================================================================
// Defect 10 — link end nodes never re-resolved
//
// Legacy parsing is order-independent, but the link handlers resolved eagerly,
// kept no name, and were never retried — so an .inp whose link sections came
// first loaded with node1/node2 == -1 and NO error at all.
// ===========================================================================

TEST(InpWriterRoundTrip, LinkEndNodesBindWhenTheLinksPrecedeTheNodes) {
    SWMMEngine eng;
    ASSERT_EQ(eng.open(data("section_order.inp").c_str(),
                       data("_section_order_open.rpt").c_str(), nullptr), SWMM_OK);
    const auto& ctx = eng.context();

    ASSERT_GT(ctx.n_links(), 0);
    for (int j = 0; j < ctx.n_links(); ++j) {
        const auto u = static_cast<std::size_t>(j);
        EXPECT_GE(ctx.links.node1[u], 0) << ctx.link_names.name_of(j) << " orphaned upstream";
        EXPECT_GE(ctx.links.node2[u], 0) << ctx.link_names.name_of(j) << " orphaned downstream";
    }
}

TEST(InpWriterRoundTrip, LinkEndNodesAreWrittenAsNames) {
    const auto g = gen1("section_order.inp");
    ASSERT_FALSE(g.text.empty());

    const auto gut1 = row(g.text, "CONDUITS", "GUT1");
    ASSERT_GE(gut1.size(), 3u);
    EXPECT_EQ(gut1.at(1), "J3");
    EXPECT_EQ(gut1.at(2), "J4");

    const auto c2 = row(g.text, "CONDUITS", "C2");
    ASSERT_GE(c2.size(), 3u);
    EXPECT_EQ(c2.at(1), "J4");
    EXPECT_EQ(c2.at(2), "O1");

    const auto w1 = row(g.text, "WEIRS", "W1");
    ASSERT_GE(w1.size(), 3u);
    EXPECT_EQ(w1.at(1), "J3");
    EXPECT_EQ(w1.at(2), "J4");
}

// The deliberate behaviour change that came with the fix. Previously this
// model loaded silently with an orphaned link; it is now a fatal ERR_NAME,
// matching legacy link_readParams().
TEST(InpWriterRoundTrip, AnUnresolvableLinkEndNodeIsFatal) {
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    EXPECT_NE(swmm_engine_open(e, data("orphan_link.inp").c_str(),
                               data("_orphan_link.rpt").c_str(), nullptr, nullptr),
              SWMM_OK)
        << "a link naming an undefined node opened silently";

    bool named = false;
    for (int i = 0; i < swmm_get_error_count(e); ++i) {
        const std::string msg = swmm_get_error_at(e, i);
        if (msg.find("209") != std::string::npos &&
            msg.find("J_MISSING") != std::string::npos)
            named = true;
    }
    // Asserting the code AND the offending name: "the open failed" alone would
    // also pass if the fixture broke for some unrelated reason.
    EXPECT_TRUE(named) << "expected ERROR 209 naming J_MISSING";

    swmm_engine_destroy(e);
}

// ===========================================================================
// Flap gates on structure links
//
// Legacy funnels both flap-gate flavours through one predicate
// (link.c:646 link_setFlapGate), and both flavours have to survive a save.
// Two defects met here:
//
//   [ORIFICES]  the writer printed a literal `SIDE … NO` and dropped the
//               open/close-time column outright, so saving a model destroyed
//               the orientation, the flap gate and the close time.
//   [OUTLETS]   the Gated column was written but never PARSED, so a save
//               round-trip silently rewrote the user's YES as NO.
// ===========================================================================

TEST(InpWriterRoundTrip, OrificeRowKeepsItsTypeGateAndCloseTime) {
    const auto g = gen1("flap_gates.inp");
    ASSERT_FALSE(g.text.empty());

    // Columns: Name From To Type Offset Cd Gated CloseTime.
    // Type and Gated are checked BEFORE the column count, so a writer that
    // drops CloseTime still reports which of the other two it also got wrong
    // rather than aborting the test on the size assertion alone.
    const auto side = row(g.text, "ORIFICES", "O_SIDE");
    ASSERT_GE(side.size(), 7u) << "row is not even the pre-fix width";
    EXPECT_EQ(side.at(3), "SIDE");
    EXPECT_EQ(side.at(6), "YES");

    // The BOTTOM orifice is the one the hardcoded "SIDE" silently converted.
    const auto bot = row(g.text, "ORIFICES", "O_BOT");
    ASSERT_GE(bot.size(), 7u);
    EXPECT_EQ(bot.at(3), "BOTTOM");
    EXPECT_EQ(bot.at(6), "NO");

    // The open/close time was dropped outright.
    ASSERT_EQ(side.size(), 8u) << "the CloseTime column is missing entirely";
    ASSERT_EQ(bot.size(), 8u);
    EXPECT_NEAR(std::stod(side.at(7)), 1.5, 1e-6);
    EXPECT_NEAR(std::stod(bot.at(7)), 2.25, 1e-6);
}

TEST(InpWriterRoundTrip, StructureFlapGatesSurviveAReopen) {
    gen1("flap_gates.inp");
    Reopened r("_flap_gates_rt1.inp");
    ASSERT_NE(r.e, nullptr);

    // Read back through the C API — the surface the GUI actually uses.
    auto gated = [&](const char* name) {
        const int j = swmm_link_index(r.e, name);
        EXPECT_GE(j, 0) << name << " did not survive the round-trip at all";
        int flag = -1;
        EXPECT_EQ(swmm_link_get_flap_gate(r.e, j, &flag), SWMM_OK) << name;
        return flag;
    };

    EXPECT_EQ(gated("C1"), 1)      << "[LOSSES] conduit gate";
    EXPECT_EQ(gated("O_SIDE"), 1)  << "[ORIFICES] gate";
    EXPECT_EQ(gated("O_BOT"), 0)   << "an ungated orifice must stay ungated";
    EXPECT_EQ(gated("W1"), 1)      << "[WEIRS] gate";
    // Both outlet rating layouts: the Gated column sits at a different token
    // index in each, so a fix that reads one fixed index passes only half.
    EXPECT_EQ(gated("L_TAB"), 1)   << "[OUTLETS] TABULAR gate";
    EXPECT_EQ(gated("L_FUN"), 1)   << "[OUTLETS] FUNCTIONAL gate";

    const int o1 = swmm_node_index(r.e, "O1");
    ASSERT_GE(o1, 0);
    int oflag = -1;
    EXPECT_EQ(swmm_node_get_outfall_flap_gate(r.e, o1, &oflag), SWMM_OK);
    EXPECT_EQ(oflag, 1) << "[OUTFALLS] gate";
}

// Gated YES is not the only spelling a deck can carry: Tokenizer::parse_boolean
// accepts YES/TRUE/1, and [LOSSES] and [OUTFALLS] have always used it. The
// orifice, weir and outlet sections instead tested a bare
// to_upper(tok) == "YES", so the same word meant different things depending on
// which section it appeared in. Every gate in this fixture is spelled TRUE or 1.
TEST(InpWriterRoundTrip, AlternateBooleanSpellingsAreAcceptedForEveryGate) {
    const auto g = gen1("flap_gate_spellings.inp");
    ASSERT_FALSE(g.text.empty());

    // The writer normalises every accepted spelling to YES on the way out.
    EXPECT_EQ(row(g.text, "ORIFICES", "OR1").at(6), "YES") << "[ORIFICES] TRUE";
    EXPECT_EQ(row(g.text, "WEIRS",    "W1").at(6),  "YES") << "[WEIRS] 1";
    EXPECT_EQ(row(g.text, "OUTLETS",  "L1").at(7),  "YES") << "[OUTLETS] TRUE";
    EXPECT_EQ(row(g.text, "LOSSES",   "C1").at(4),  "YES") << "[LOSSES] TRUE";
    // FREE outfalls carry no stage-data column, so Gated is the 4th token,
    // not the 5th — the same left-shift the parser handles at NodesHandler.cpp.
    EXPECT_EQ(row(g.text, "OUTFALLS", "O1").at(3),  "YES") << "[OUTFALLS] 1";

    // And the flags really are set, not merely echoed as text.
    Reopened r("_flap_gate_spellings_rt1.inp");
    ASSERT_NE(r.e, nullptr);
    for (const char* name : {"C1", "OR1", "W1", "L1"}) {
        const int j = swmm_link_index(r.e, name);
        ASSERT_GE(j, 0) << name;
        int flag = -1;
        EXPECT_EQ(swmm_link_get_flap_gate(r.e, j, &flag), SWMM_OK) << name;
        EXPECT_EQ(flag, 1) << name << ": alternate spelling did not set the gate";
    }
    const int o1 = swmm_node_index(r.e, "O1");
    ASSERT_GE(o1, 0);
    int oflag = -1;
    EXPECT_EQ(swmm_node_get_outfall_flap_gate(r.e, o1, &oflag), SWMM_OK);
    EXPECT_EQ(oflag, 1) << "[OUTFALLS] gate from the '1' spelling";
}

// ===========================================================================
// Idempotency
//
// The writer must converge: reading back what it wrote and writing again has
// to reproduce the file exactly, or every save walks the model.
// ===========================================================================

TEST(InpWriterRoundTrip, TheWriterConvergesAtTheSecondGeneration) {
    for (const char* fixture : {"hydrology.inp", "section_order.inp", "inlet_usage.inp",
                                "flap_gates.inp", "flap_gate_spellings.inp"}) {
        const std::string stem(fixture, std::string(fixture).size() - 4);
        writeOnce(fixture, "_" + stem + "_rt1.inp");
        writeOnce("_" + stem + "_rt1.inp", "_" + stem + "_rt2.inp");
        writeOnce("_" + stem + "_rt2.inp", "_" + stem + "_rt3.inp");

        EXPECT_EQ(slurp(data("_" + stem + "_rt2.inp")),
                  slurp(data("_" + stem + "_rt3.inp")))
            << fixture << ": writer does not converge";
    }
}

// Generation 1 -> 2 is NOT yet byte-stable, and the reasons are two known
// pre-existing round-trip defects unrelated to the ten above:
//
//   SWEEP_END  OptionsHandler parses MM/DD against leap year 2000 ("12/31" ->
//              day 366) while InpWriter::fmt_sweep renders day-of-year against
//              non-leap 2001, so 366 comes back as "1/1".
//   Units      the [MAP] writer emits the default "None" but SpatialHandler
//              uppercases on read.
//
// Pinned as a SUBSET so that fixing either one keeps this green — it fails only
// if some NEW key starts drifting between generations.
TEST(InpWriterRoundTrip, GenerationOneDiffersOnlyByTheTwoKnownPreExistingGaps) {
    writeOnce("hydrology.inp", "_hydrology_rt1.inp");
    writeOnce("_hydrology_rt1.inp", "_hydrology_rt2.inp");

    std::istringstream a(slurp(data("_hydrology_rt1.inp")));
    std::istringstream b(slurp(data("_hydrology_rt2.inp")));
    std::string la, lb;
    while (std::getline(a, la) && std::getline(b, lb)) {
        if (la == lb) continue;
        const auto key = cols(la).empty() ? std::string() : cols(la).at(0);
        EXPECT_TRUE(key == "SWEEP_END" || key == "Units")
            << "new generation-to-generation drift:\n  gen1: " << la << "\n  gen2: " << lb;
    }
}

// ===========================================================================
// Defect 11 — [XSECTIONS] IRREGULAR transect name replaced by derived numerics
//
// The writer had name-emitting branches for STREET and CUSTOM but none for
// IRREGULAR, so the fallback emitted the resolver-derived y_full/w_max where
// the transect NAME belongs. On reload that numeric parsed as an undefined
// transect name, the reference failed to resolve with no diagnostic, and the
// conduit degenerated to zero area/depth — one Save silently killed every
// transect conduit in the model.
// ===========================================================================

namespace {

/// Internal-surface link lookup by name; the C API's get_xsect is itself part
/// of the defect family (it returned y_full where the transect reference
/// belongs), so these assertions read the context directly.
int linkIndex(const SimulationContext& ctx, const std::string& name) {
    for (int j = 0; j < ctx.n_links(); ++j)
        if (ctx.link_names.name_of(j) == name) return j;
    return -1;
}

/// All GR (elev, station) pairs belonging to `transect` in a written
/// [TRANSECTS] section, in file order.
std::vector<std::pair<double, double>> grPairs(const std::string& text,
                                               const std::string& transect) {
    std::vector<std::pair<double, double>> pairs;
    bool inside = false;
    for (const auto& r : section(text, "TRANSECTS")) {
        auto c = cols(r);
        if (c.empty()) continue;
        if (c[0] == "X1") { inside = (c.size() > 1 && c[1] == transect); continue; }
        if (c[0] == "NC") continue;
        if (c[0] == "GR" && inside)
            for (size_t i = 1; i + 1 < c.size(); i += 2)
                pairs.emplace_back(std::stod(c[i]), std::stod(c[i + 1]));
    }
    return pairs;
}

}  // namespace

TEST(InpWriterRoundTrip, IrregularXsectKeepsTheTransectName) {
    const auto g = gen1("irregular_transect.inp");
    ASSERT_FALSE(g.text.empty());

    const auto c1 = row(g.text, "XSECTIONS", "C1");
    ASSERT_GE(c1.size(), 3u);
    EXPECT_EQ(c1.at(1), "IRREGULAR");
    EXPECT_EQ(c1.at(2), "TRAP_MAIN") << "transect name replaced by a numeric";

    const auto c2 = row(g.text, "XSECTIONS", "C2");
    ASSERT_GE(c2.size(), 3u);
    EXPECT_EQ(c2.at(2), "CREEK_A");
    EXPECT_EQ(c2.back(), "2") << "barrels lost from the IRREGULAR row";
}

TEST(InpWriterRoundTrip, IrregularXsectSurvivesReopen) {
    SWMMEngine gen0;
    ASSERT_EQ(gen0.open(data("irregular_transect.inp").c_str(),
                        data("_irregular_transect.inp.rpt").c_str(), nullptr),
              SWMM_OK);
    const auto& c0 = gen0.context();

    gen1("irregular_transect.inp");

    SWMMEngine rt;
    ASSERT_EQ(rt.open(data("_irregular_transect_rt1.inp").c_str(),
                      data("_irregular_transect_rt1.inp.rpt").c_str(), nullptr),
              SWMM_OK);
    for (const auto& e : rt.context().errors) ADD_FAILURE() << "reopen: " << e;
    const auto& c1 = rt.context();

    for (const char* name : {"C1", "C2"}) {
        const int j0 = linkIndex(c0, name);
        const int j1 = linkIndex(c1, name);
        ASSERT_GE(j0, 0);
        ASSERT_GE(j1, 0);
        const auto u0 = static_cast<size_t>(j0);
        const auto u1 = static_cast<size_t>(j1);
        EXPECT_GE(c1.links.xsect_curve[u1], 0)
            << name << ": transect reference did not survive the round trip";
        EXPECT_DOUBLE_EQ(c1.links.xsect_y_full[u1], c0.links.xsect_y_full[u0]) << name;
        EXPECT_DOUBLE_EQ(c1.links.xsect_a_full[u1], c0.links.xsect_a_full[u0]) << name;
        EXPECT_DOUBLE_EQ(c1.links.xsect_r_full[u1], c0.links.xsect_r_full[u0]) << name;
        EXPECT_DOUBLE_EQ(c1.links.xsect_w_max[u1], c0.links.xsect_w_max[u0]) << name;
    }
}

// [TRANSECTS] itself round-trips the RAW store (transforms are applied to a
// copy at resolve time), so every X1 modifier and GR pair must come back
// numerically identical — including CREEK_A's 6-decimal station, which the old
// %10.4f GR format silently rounded on every save.
TEST(InpWriterRoundTrip, TransectsRoundTripRawX1AndGRData) {
    const auto g = gen1("irregular_transect.inp");
    ASSERT_FALSE(g.text.empty());

    std::vector<std::string> x1;
    for (const auto& r : section(g.text, "TRANSECTS")) {
        auto c = cols(r);
        if (!c.empty() && c[0] == "X1" && c.size() > 1 && c[1] == "TRAP_MAIN") x1 = c;
    }
    ASSERT_EQ(x1.size(), 10u) << "X1 line must carry all 10 legacy tokens";
    EXPECT_DOUBLE_EQ(std::stod(x1.at(3)), 20.0) << "Xleft";
    EXPECT_DOUBLE_EQ(std::stod(x1.at(4)), 80.0) << "Xright";
    EXPECT_DOUBLE_EQ(std::stod(x1.at(7)), 1.5) << "Lfactor";
    EXPECT_DOUBLE_EQ(std::stod(x1.at(8)), 2.0) << "Xfactor";
    EXPECT_DOUBLE_EQ(std::stod(x1.at(9)), 0.5) << "Yfactor (additive elevation offset)";

    const std::vector<std::pair<double, double>> trap = {
        {10.0, 0.0}, {8.0, 20.0}, {6.0, 40.0}, {5.5, 50.0},
        {6.0, 60.0}, {8.0, 80.0}, {10.0, 100.0}};
    EXPECT_EQ(grPairs(g.text, "TRAP_MAIN"), trap);

    const std::vector<std::pair<double, double>> creek = {
        {5.0, 0.0}, {3.0, 10.0}, {2.0, 12.123456}, {2.0, 20.0},
        {3.5, 30.0}, {5.0, 40.0}};
    EXPECT_EQ(grPairs(g.text, "CREEK_A"), creek)
        << "GR precision loss: 12.123456 must not round to 12.1235";
}

// A dangling transect reference (lenient open, the GUI's editing mode) must
// still save as the NAME — the same contract defects 6-8 pin for gages, pump
// curves and snowpacks.
TEST(InpWriterRoundTrip, ADanglingTransectNameSurvivesSave) {
    const auto g = writeOnceLenient("irregular_dangling.inp", "_irregular_dangling_rt1.inp");
    ASSERT_FALSE(g.text.empty());

    const auto c1 = row(g.text, "XSECTIONS", "C1");
    ASSERT_GE(c1.size(), 3u);
    EXPECT_EQ(c1.at(1), "IRREGULAR");
    EXPECT_EQ(c1.at(2), "TRX") << "unresolved transect name dropped on save";
}

// The deliberate behaviour change that came with the fix: an IRREGULAR link
// whose transect does not exist previously loaded silently with a_full == 0
// (a dead conduit) — exactly the state a corrupted save produced. It is now a
// fatal ERR_NAME on a strict open, matching legacy transect_validate; the
// lenient (GUI editing) open above still loads it for repair.
TEST(InpWriterRoundTrip, AnUnresolvableTransectIsFatalOnAStrictOpen) {
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    EXPECT_NE(swmm_engine_open(e, data("irregular_dangling.inp").c_str(),
                               data("_irregular_dangling.rpt").c_str(), nullptr, nullptr),
              SWMM_OK)
        << "an IRREGULAR link naming an undefined transect opened silently";

    bool named = false;
    for (int i = 0; i < swmm_get_error_count(e); ++i) {
        const std::string msg = swmm_get_error_at(e, i);
        if (msg.find("209") != std::string::npos &&
            msg.find("TRX") != std::string::npos)
            named = true;
    }
    EXPECT_TRUE(named) << "expected ERROR 209 naming TRX";

    swmm_engine_destroy(e);
}

// The API half of the same defect family: swmm_link_set_xsect had a STREET
// index→name branch but no IRREGULAR one, so the GUI's transect picker
// (which passes geom1 = transect index) fell into the default arm — the index
// was stored as y_full in feet and the reference left dangling. get_xsect
// mirrored it by returning y_full where the index belongs, so any get→set
// cycle rewrote the reference as a number.
TEST(InpWriterRoundTrip, SetXsectIrregularBindsTheTransectByIndex) {
    Reopened r("irregular_transect.inp");

    // C1 currently references TRAP_MAIN (index 0); move it to CREEK_A (1).
    int shape = -1;
    double g1 = -99, g2 = -99, g3 = -99, g4 = -99;
    ASSERT_EQ(swmm_link_get_xsect(r.e, 0, &shape, &g1, &g2, &g3, &g4), SWMM_OK);
    EXPECT_EQ(g1, 0.0) << "get_xsect should report the transect index";
    EXPECT_EQ(g2, 0.0);

    ASSERT_EQ(swmm_link_set_xsect(r.e, 0, shape, 1.0, 0, 0, 0), SWMM_OK);
    ASSERT_EQ(swmm_link_get_xsect(r.e, 0, &shape, &g1, &g2, &g3, &g4), SWMM_OK);
    EXPECT_EQ(g1, 1.0) << "set→get is not a fixed point";

    // A bad index must be rejected, not stored as a depth.
    EXPECT_EQ(swmm_link_set_xsect(r.e, 0, shape, 99.0, 0, 0, 0), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_link_set_xsect(r.e, 0, shape, -1.0, 0, 0, 0), SWMM_ERR_BADPARAM);
}

TEST(InpWriterRoundTrip, SetXsectIrregularGeometryMatchesTheResolver) {
    // Binding C1 to CREEK_A through the API must produce exactly the geometry
    // the resolver derives for C2 (which references CREEK_A in the deck) —
    // the shared buildFromStore helper is what keeps the two bit-identical.
    SWMMEngine eng;
    ASSERT_EQ(eng.open(data("irregular_transect.inp").c_str(),
                       data("_irregular_transect.inp.rpt").c_str(), nullptr),
              SWMM_OK);
    auto& ctx = eng.context();
    const int c1 = linkIndex(ctx, "C1");
    const int c2 = linkIndex(ctx, "C2");
    ASSERT_GE(c1, 0);
    ASSERT_GE(c2, 0);

    ASSERT_EQ(swmm_link_set_xsect(reinterpret_cast<SWMM_Engine>(&eng), c1,
                                  static_cast<int>(ctx.links.xsect_shape[static_cast<size_t>(c1)]),
                                  1.0, 0, 0, 0),
              SWMM_OK);

    const auto u1 = static_cast<size_t>(c1);
    const auto u2 = static_cast<size_t>(c2);
    EXPECT_EQ(ctx.links.pump_curve_name[u1], "CREEK_A");
    EXPECT_DOUBLE_EQ(ctx.links.xsect_y_full[u1], ctx.links.xsect_y_full[u2]);
    EXPECT_DOUBLE_EQ(ctx.links.xsect_a_full[u1], ctx.links.xsect_a_full[u2]);
    EXPECT_DOUBLE_EQ(ctx.links.xsect_r_full[u1], ctx.links.xsect_r_full[u2]);
    EXPECT_DOUBLE_EQ(ctx.links.xsect_w_max[u1],  ctx.links.xsect_w_max[u2]);

    // And the writer must now save C1 with the new transect's NAME.
    std::vector<std::string> warnings;
    ASSERT_EQ(openswmm::inp_writer::writeInpFile(
                  ctx, data("_irregular_transect_api.inp"), &warnings), 0);
    const auto text = slurp(data("_irregular_transect_api.inp"));
    EXPECT_EQ(row(text, "XSECTIONS", "C1").at(2), "CREEK_A");
}

TEST(InpWriterRoundTrip, IrregularFixtureConvergesAtTheSecondGeneration) {
    writeOnce("irregular_transect.inp", "_irregular_transect_rt1.inp");
    writeOnce("_irregular_transect_rt1.inp", "_irregular_transect_rt2.inp");
    writeOnce("_irregular_transect_rt2.inp", "_irregular_transect_rt3.inp");

    EXPECT_EQ(slurp(data("_irregular_transect_rt2.inp")),
              slurp(data("_irregular_transect_rt3.inp")))
        << "irregular_transect.inp: writer does not converge";
}

// ===========================================================================
// Unsteady friction — [OPTIONS] UNSTEADY_FRICTION / UF_K3 (issue #156)
//
// Both keys are inert in this build (no solver reads them), so a writer that
// drops them loses data with no other symptom: the file still reloads, just
// with the defaults back in place. Non-default values in the fixture are what
// make that visible.
// ===========================================================================

TEST(InpWriterRoundTrip, UnsteadyFrictionKeysSurviveTheRoundTrip) {
    const auto g = gen1("uf_options.inp");
    EXPECT_EQ(row(g.text, "OPTIONS", "UNSTEADY_FRICTION").at(1), "VITKOVSKY");
    EXPECT_DOUBLE_EQ(std::stod(row(g.text, "OPTIONS", "UF_K3").at(1)), 0.02);

    Reopened r("_uf_options_rt1.inp");
    char buf[64] = {};
    ASSERT_EQ(swmm_options_get(r.e, "UNSTEADY_FRICTION", buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "VITKOVSKY");
    ASSERT_EQ(swmm_options_get(r.e, "UF_K3", buf, sizeof(buf)), SWMM_OK);
    EXPECT_DOUBLE_EQ(std::stod(buf), 0.02);
}

TEST(InpWriterRoundTrip, UnsteadyFrictionKeysAreWrittenEvenAtTheirDefaults) {
    // The keys are emitted unconditionally (like SURCHARGE_METHOD), so a deck
    // that never named them still carries them forward. Without this the
    // previous test would also pass against a writer that only emits
    // non-defaults — and the GUI's Save would silently drop a user's explicit
    // "NONE" back to an absent key.
    const auto g = gen1("hydrology.inp");
    EXPECT_EQ(row(g.text, "OPTIONS", "UNSTEADY_FRICTION").at(1), "NONE");
    EXPECT_DOUBLE_EQ(std::stod(row(g.text, "OPTIONS", "UF_K3").at(1)), 0.015);
}

// ===========================================================================
// Defect 12 — save-idempotence drift in [OPTIONS] SWEEP_* and [MAP] Units.
//
// Two independent parse/format asymmetries made the SECOND save differ from
// the first on every deck (found by H6b's save check, on a control deck with
// no sweep or map configuration at all):
//
//   * SWEEP_* parsed against a LEAP-year anchor (2000: 12/31 → doy 366) but
//     formatted against a non-leap one (2001: doy 366 → "1/1"), so every
//     date past Feb 28 shifted one day per save/reopen cycle and the default
//     12/31 wrapped to January 1st.
//   * [MAP] Units wrote the literal "None" while the parser uppercases, so
//     gen1 said "None" and every later generation said "NONE".
//
// The gate drives one deck to generation 3 and requires generations 2 and 3
// to agree byte-for-byte — idempotence, not just plausibility — plus the
// specific values that used to walk.
// ===========================================================================

TEST(InpWriterRoundTrip, SweepDatesAndMapUnitsAreSaveIdempotent) {
    {
        std::ofstream f(data("_swp_idem.inp"));
        f << "[OPTIONS]\n"
             "FLOW_UNITS           CFS\nFLOW_ROUTING         KINWAVE\n"
             "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
             "END_DATE             01/01/2026\nEND_TIME             01:00:00\n"
             "ROUTING_STEP         5\nREPORT_STEP          00:05:00\n"
             "SWEEP_START          03/15\nSWEEP_END            12/31\n\n"
             "[JUNCTIONS]\nJ0 10.0 10 0.5 0 0\n\n"
             "[OUTFALLS]\nOUT 7.0 FREE NO\n\n"
             "[CONDUITS]\nC1 J0 OUT 400 0.013 0 0 0\n\n"
             "[XSECTIONS]\nC1 CIRCULAR 1.5 0 0 0\n\n"
             "[MAP]\nDIMENSIONS 0 0 100 100\nUnits      None\n\n"
             "[REPORT]\nINPUT NO\n";
    }
    const auto g1 = writeOnce("_swp_idem.inp",     "_swp_idem_rt1.inp");
    const auto g2 = writeOnce("_swp_idem_rt1.inp", "_swp_idem_rt2.inp");
    const auto g3 = writeOnce("_swp_idem_rt2.inp", "_swp_idem_rt3.inp");

    // The two values that used to walk, at every generation.
    for (const auto* g : {&g1, &g2, &g3}) {
        EXPECT_EQ(row(g->text, "OPTIONS", "SWEEP_START").at(1), "3/15");
        EXPECT_EQ(row(g->text, "OPTIONS", "SWEEP_END").at(1), "12/31")
            << "the leap-anchor parse turned December 31st into January 1st";
        EXPECT_EQ(row(g->text, "MAP", "Units").at(1), "None")
            << "the [MAP] Units value changed case between generations";
    }

    // And the strong form: a second save of a saved file changes NOTHING.
    EXPECT_EQ(g2.text, g3.text)
        << "generation 2 and generation 3 differ — the writer is not "
           "idempotent over its own output";
}

// ===========================================================================
// Defect 12 — authored form (offset convention + conduit orientation)
//
// resolve_cross_references rewrites ELEVATION offsets as depths and reverses
// adverse-slope conduits in place. The writer emitted both as-is, so a plain
// Open → Save wrote depths under `LINK_OFFSETS ELEVATION` (the next open
// subtracted the invert again and clamped to 0) and swapped From/To, offsets,
// losses and InitFlow on every adverse conduit. Legacy SWMM-GUI never hit
// this because it exports its own object model, never engine state.
// ===========================================================================

namespace {
double num(const std::vector<std::string>& c, std::size_t i) { return std::stod(c.at(i)); }
}

TEST(InpWriterRoundTrip, ElevationOffsetsAreWrittenAsElevations) {
    const auto g = gen1("authored_form.inp");
    ASSERT_FALSE(g.text.empty());
    EXPECT_EQ(row(g.text, "OPTIONS", "LINK_OFFSETS").at(1), "ELEVATION");

    const auto c_ok = row(g.text, "CONDUITS", "C_OK");
    ASSERT_GE(c_ok.size(), 7u);
    EXPECT_NEAR(num(c_ok, 5), 12.5, 1e-6) << "InOffset was written as a depth";
    EXPECT_NEAR(num(c_ok, 6), 11.0, 1e-6) << "OutOffset was written as a depth";

    EXPECT_NEAR(num(row(g.text, "ORIFICES", "OR1"), 4), 11.5, 1e-6);
    EXPECT_NEAR(num(row(g.text, "WEIRS",    "W1"),  4), 12.0, 1e-6);
    EXPECT_NEAR(num(row(g.text, "OUTLETS",  "L1"),  3), 11.25, 1e-6);
}

TEST(InpWriterRoundTrip, AdverseConduitKeepsAuthoredOrientation) {
    const auto g = gen1("authored_form.inp");
    ASSERT_FALSE(g.text.empty());

    const auto c = row(g.text, "CONDUITS", "C_ADV");
    ASSERT_GE(c.size(), 8u);
    EXPECT_EQ(c.at(1), "J1") << "From node was swapped by the adverse-slope reversal";
    EXPECT_EQ(c.at(2), "J2");
    EXPECT_NEAR(num(c, 5), 10.5,  1e-6);
    EXPECT_NEAR(num(c, 6), 12.25, 1e-6);
    EXPECT_NEAR(num(c, 7), 0.75,  1e-6) << "InitFlow sign was flipped";

    const auto l = row(g.text, "LOSSES", "C_ADV");
    ASSERT_GE(l.size(), 3u);
    EXPECT_NEAR(num(l, 1), 0.1, 1e-6) << "Kentry/Kexit were swapped";
    EXPECT_NEAR(num(l, 2), 0.2, 1e-6);

    // Vertices were never reordered by the reversal; they must still read
    // J1 → J2 alongside the restored endpoints.
    const auto v = section(g.text, "VERTICES");
    ASSERT_EQ(v.size(), 2u);
    EXPECT_NEAR(num(cols(v[0]), 1), 100.0, 1e-6);
    EXPECT_NEAR(num(cols(v[1]), 1), 200.0, 1e-6);
}

TEST(InpWriterRoundTrip, AuthoredFormIsSaveIdempotentAndReopensClean) {
    const auto g1 = gen1("authored_form.inp");
    const auto g2 = writeOnce("_authored_form_rt1.inp", "_authored_form_rt2.inp");
    const auto g3 = writeOnce("_authored_form_rt2.inp", "_authored_form_rt3.inp");
    EXPECT_EQ(g2.text, g3.text);
    EXPECT_EQ(row(g1.text, "CONDUITS", "C_ADV"), row(g3.text, "CONDUITS", "C_ADV"));
    EXPECT_EQ(row(g1.text, "CONDUITS", "C_OK"),  row(g3.text, "CONDUITS", "C_OK"));

    // The reopened model must hold the same depths as the original open —
    // and no negative-offset warning, which is what the double-subtraction
    // used to produce.
    Reopened r("_authored_form_rt2.inp");
    const int idx = swmm_link_index(r.e, "C_OK");
    ASSERT_GE(idx, 0);
    double up = 0, dn = 0;
    ASSERT_EQ(swmm_link_get_offset_up(r.e, idx, &up), SWMM_OK);
    ASSERT_EQ(swmm_link_get_offset_dn(r.e, idx, &dn), SWMM_OK);
    EXPECT_NEAR(up, 0.5, 1e-6);
    EXPECT_NEAR(dn, 0.0, 1e-6);
    for (int i = 0; i < swmm_get_warning_count(r.e); ++i) {
        const std::string w = swmm_get_warning_at(r.e, i);
        EXPECT_EQ(w.find("negative offset"), std::string::npos) << w;
    }
}

TEST(InpWriterRoundTrip, RestoreAuthoredOrientationApiUnreversesTheLiveContext) {
    SWMM_Engine e = swmm_engine_create();
    ASSERT_EQ(swmm_engine_open(e, data("authored_form.inp").c_str(),
                               data("_authored_form.inp.rpt").c_str(), nullptr, nullptr),
              SWMM_OK);
    const int idx = swmm_link_index(e, "C_ADV");
    const int j1  = swmm_node_index(e, "J1");
    const int j2  = swmm_node_index(e, "J2");
    ASSERT_GE(idx, 0); ASSERT_GE(j1, 0); ASSERT_GE(j2, 0);

    int from = -1;
    swmm_link_get_from_node(e, idx, &from);
    EXPECT_EQ(from, j2) << "fixture no longer triggers the parse-time reversal";

    int n = 0;
    ASSERT_EQ(swmm_links_restore_authored_orientation(e, &n), SWMM_OK);
    EXPECT_EQ(n, 1);
    swmm_link_get_from_node(e, idx, &from);
    EXPECT_EQ(from, j1);
    double up = 0;
    swmm_link_get_offset_up(e, idx, &up);
    EXPECT_NEAR(up, 0.5, 1e-6) << "offset did not travel back with its node";

    // Idempotent, and the writer no longer has anything to undo.
    ASSERT_EQ(swmm_links_restore_authored_orientation(e, &n), SWMM_OK);
    EXPECT_EQ(n, 0);
    swmm_engine_close(e);
    swmm_engine_destroy(e);
}
