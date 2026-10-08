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
 * @file main.cpp
 * @brief Link-level harness for the inlet parse/write path (no gtest, no
 *        engine runtime): drives handle_streets / handle_inlets /
 *        handle_inlet_usage / handle_inlet_junctions and InpWriter directly
 *        on a SimulationContext, then re-parses what it wrote.
 *
 * @details Written for the 2026-09-05 inlet-junction integration pass, to
 *          prove that the four parse/write translation units actually link
 *          and round-trip (a -fsyntax-only gate cannot). Build with
 *          tests/manual/inlet_parse_writer/build.sh; output files land beside
 *          this file (CLAUDE.md §4.1).
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/SimulationContext.hpp"
#include "core/InpWriter.hpp"
#include "input/handlers/InfraHandler.hpp"
#include "input/handlers/NodesHandler.hpp"
#include "input/handlers/LinksHandler.hpp"

using namespace openswmm;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%-56s %s\n", what.c_str(), ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}

std::vector<std::string> lines(std::initializer_list<const char*> l) {
    return std::vector<std::string>(l.begin(), l.end());
}

std::string slurp(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss; ss << f.rdbuf(); return ss.str();
}

// Split an .inp file into the raw line vectors the handlers consume.
std::vector<std::string> section_of(const std::string& text, const std::string& name) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string line; bool inside = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line[0] == '[') {
            inside = (line.rfind("[" + name + "]", 0) == 0);
            continue;
        }
        if (inside) out.push_back(line);
    }
    return out;
}

// Minimum network the inlet sections reference: J_IN -> IJ1 -> O_ST plus MH1.
void buildBase(SimulationContext& ctx) {
    input::handle_junctions(ctx, lines({"J_IN 10.0 5.0", "MH1 2.0 8.0"}));
    input::handle_outfalls(ctx, lines({"O_ST 8.0 FREE NO"}));
    input::handle_inlet_junctions(ctx, lines({
        "IJ1 9.0 0.5 Curb1 MH1 2 10 1.5 0.1 2.0 ON_SAG"}));
    input::handle_conduits(ctx, lines({
        "C_UP J_IN IJ1 100.0 0.016 0 0",
        "C_DN IJ1 O_ST 100.0 0.016 0 0"}));
    input::handle_xsections(ctx, lines({"C_UP STREET ST1", "C_DN STREET ST1"}));
}

// Parse an existing .inp's inlet-relevant sections and report any errors.
// Enough of the model to resolve the inlet references; not a full load.
int checkDeck(const std::string& path) {
    const std::string text = slurp(path);
    if (text.empty()) {
        std::printf("cannot read %s\n", path.c_str());
        return 1;
    }
    SimulationContext ctx;
    input::handle_streets(ctx, section_of(text, "STREETS"));
    input::handle_inlets(ctx, section_of(text, "INLETS"));
    input::handle_junctions(ctx, section_of(text, "JUNCTIONS"));
    input::handle_outfalls(ctx, section_of(text, "OUTFALLS"));
    input::handle_inlet_junctions(ctx, section_of(text, "INLET_JUNCTIONS"));
    input::handle_conduits(ctx, section_of(text, "CONDUITS"));
    input::handle_xsections(ctx, section_of(text, "XSECTIONS"));
    input::handle_inlet_usage(ctx, section_of(text, "INLET_USAGE"));

    std::printf("%s\n  streets=%d inlets=%d usages=%d nodes=%d links=%d\n",
                path.c_str(), ctx.streets.count(), ctx.inlets.count(),
                ctx.inlet_usages.count(), ctx.n_nodes(), ctx.n_links());
    for (const auto& e : ctx.errors) std::printf("  error: %s\n", e.c_str());
    std::printf("  %s\n", ctx.errors.empty() ? "no parse errors" : "FAILED");
    return ctx.errors.empty() ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    // With a path argument, syntax-check that deck's inlet sections instead
    // of running the built-in round-trip checks.
    if (argc > 1) return checkDeck(argv[1]);

    SimulationContext ctx;

    input::handle_streets(ctx, lines({
        "ST1 20 0.5 4 0.016 0 0 1 20 4 0.016"}));
    input::handle_inlets(ctx, lines({
        "Combo1 GRATE 2.0 2.0 P_BAR-50",
        "Combo1 CURB  3.0 0.5 HORIZONTAL",
        "Curb1  CURB  2.0 0.5 VERTICAL",
        "Cust1  CUSTOM DIV1"}));
    buildBase(ctx);
    input::handle_inlet_usage(ctx, lines({
        "C_UP Combo1 MH1 1 0 0 0 0 ON_GRADE"}));

    check(ctx.streets.count() == 1, "[STREETS] one row");
    check(ctx.inlets.count() == 3, "[INLETS] COMBO merged into one row");
    check(ctx.inlets.inlet_type[0] == "COMBO", "two-line GRATE+CURB -> COMBO");
    check(ctx.inlets.curb_throat[0] == 0, "COMBO keeps HORIZONTAL throat");
    check(ctx.inlets.curve_id[2] == "DIV1", "CUSTOM keeps curve id");
    check(ctx.inlet_usages.count() == 2, "one node row + one link row");
    check(ctx.inlet_usages.node_host[0] == ctx.node_names.find("IJ1"),
          "[INLET_JUNCTIONS] row is node-hosted");
    check(ctx.inlet_usages.pending_design_name[0] == "Curb1",
          "design name parked for PostParseResolver");
    check(ctx.inlet_usages.link_index[1] == ctx.link_names.find("C_UP"),
          "[INLET_USAGE] row is link-hosted");
    check(ctx.nodes.is_inlet[static_cast<std::size_t>(ctx.node_names.find("IJ1"))] == 1 &&
          ctx.nodes.is_virtual[static_cast<std::size_t>(ctx.node_names.find("IJ1"))] == 1,
          "IJ1 is both is_inlet and is_virtual");
    check(ctx.errors.empty(), "no parse errors");
    for (const auto& e : ctx.errors) std::printf("    error: %s\n", e.c_str());

    // The writer needs the references the resolver normally fills in; do the
    // two the [INLET_JUNCTIONS] grammar defers, so the section can be emitted.
    ctx.inlet_usages.design_index[0] = 1;                       // Curb1
    ctx.inlet_usages.node_index[0]   = ctx.node_names.find("MH1");
    ctx.inlet_usages.pending_design_name[0].clear();
    ctx.inlet_usages.pending_capture_name[0].clear();

    const std::string out = "tests/manual/inlet_parse_writer/written.inp";
    std::vector<std::string> warnings;
    check(inp_writer::writeInpFile(ctx, out, &warnings) == 0, "InpWriter::write");
    for (const auto& w : warnings) std::printf("    warning: %s\n", w.c_str());
    const std::string text = slurp(out);
    check(text.find("[INLET_JUNCTIONS]") != std::string::npos, "[INLET_JUNCTIONS] emitted");
    check(text.find("[INLET_USAGE]") != std::string::npos, "[INLET_USAGE] emitted");
    check(text.find("[INLETS]") != std::string::npos, "[INLETS] emitted");
    check(text.find("HORIZONTAL") != std::string::npos, "COMBO curb line has throat");
    check(text.find("[VIRTUAL_JUNCTIONS]") == std::string::npos,
          "an inlet junction is not also written as a virtual junction");
    check(text.find("IJ1") != std::string::npos && text.find("Curb1") != std::string::npos,
          "inlet junction row names its design");

    // Round trip: re-parse what was written and compare the stores.
    SimulationContext ctx2;
    input::handle_streets(ctx2, section_of(text, "STREETS"));
    input::handle_inlets(ctx2, section_of(text, "INLETS"));
    input::handle_junctions(ctx2, section_of(text, "JUNCTIONS"));
    input::handle_outfalls(ctx2, section_of(text, "OUTFALLS"));
    input::handle_inlet_junctions(ctx2, section_of(text, "INLET_JUNCTIONS"));
    input::handle_conduits(ctx2, section_of(text, "CONDUITS"));
    input::handle_xsections(ctx2, section_of(text, "XSECTIONS"));
    input::handle_inlet_usage(ctx2, section_of(text, "INLET_USAGE"));

    check(ctx2.errors.empty(), "re-parse has no errors");
    for (const auto& e : ctx2.errors) std::printf("    error: %s\n", e.c_str());
    check(ctx2.inlets.count() == ctx.inlets.count(), "inlet count round-trips");
    check(ctx2.inlets.inlet_type[0] == "COMBO", "COMBO round-trips");
    check(ctx2.inlets.curb_throat[0] == 0, "throat round-trips");
    check(ctx2.inlets.curb_length[0] == 3.0 && ctx2.inlets.curb_height[0] == 0.5,
          "COMBO curb dimensions round-trip");
    check(ctx2.inlets.length[0] == 2.0 && ctx2.inlets.width[0] == 2.0,
          "COMBO grate dimensions round-trip");
    check(ctx2.inlets.curve_id[2] == "DIV1", "CUSTOM curve id round-trips");
    check(ctx2.inlet_usages.count() == 2, "usage rows round-trip");
    check(ctx2.inlet_usages.pending_capture_name[0] == "MH1",
          "capture node name round-trips");
    check(ctx2.inlet_usages.num_inlets[0] == 2 &&
          ctx2.inlet_usages.placement[0] == 2,
          "[INLET_JUNCTIONS] tail round-trips");
    check(std::abs(ctx2.inlet_usages.clog_factor[0] - 0.9) < 1e-9 &&
          std::abs(ctx2.inlet_usages.flow_limit[0] - 1.5) < 1e-9 &&
          std::abs(ctx2.inlet_usages.local_depress[0] - 0.1) < 1e-9 &&
          std::abs(ctx2.inlet_usages.local_width[0] - 2.0) < 1e-9,
          "%Clog / Qmax / aLocal / wLocal round-trip verbatim");
    check(std::abs(ctx2.nodes.invert_elev[static_cast<std::size_t>(
              ctx2.node_names.find("IJ1"))] - 9.0) < 1e-9 &&
          std::abs(ctx2.nodes.rim_depth[static_cast<std::size_t>(
              ctx2.node_names.find("IJ1"))] - 0.5) < 1e-9,
          "Elev / MaxDepth round-trip in display units");

    // Pass 2 must be a fixed point of pass 1.
    const std::string out2 = "tests/manual/inlet_parse_writer/written2.inp";
    ctx2.inlet_usages.design_index[0] = 1;
    ctx2.inlet_usages.node_index[0]   = ctx2.node_names.find("MH1");
    ctx2.inlet_usages.pending_design_name[0].clear();
    ctx2.inlet_usages.pending_capture_name[0].clear();
    check(inp_writer::writeInpFile(ctx2, out2, nullptr) == 0, "second write");
    check(slurp(out2) == text, "pass 2 is a fixed point of pass 1");

    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
                failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
