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
 * @file test_process_components.cpp
 * @brief IO1–IO2 gates for [PROCESS_COMPONENTS] + component config files
 *        (plans/transport/TRANSPORT_IO_PLUGIN_CONFIG_PLAN.md §7).
 *
 * @details Covers: registration parsing + round-trip through InpWriter;
 *          unknown-id and planned-id diagnostics (strict open fails with a
 *          precise message); config delivery to a registered test component
 *          (sections in order, relative-path resolution against the .inp
 *          directory); missing-config and nested-[PROCESS_COMPONENTS]
 *          fatality. Test artifacts use the underscore-prefix convention in
 *          the test working directory (CLAUDE.md §4.1).
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>

#include "core/InpWriter.hpp"
#include "core/SWMMEngine.hpp"
#include "plugins/ProcessComponentRegistry.hpp"

namespace {

openswmm::SWMMEngine& as_cpp_engine(SWMM_Engine engine) {
    return *static_cast<openswmm::SWMMEngine*>(engine);
}

/// Minimal runnable deck; `pc_lines` lands verbatim in [PROCESS_COMPONENTS].
void write_deck(const char* path, const std::string& pc_lines) {
    std::ofstream f(path);
    f << "[TITLE]\nIO1/IO2 process components gate deck\n\n"
      << "[OPTIONS]\n"
      << "FLOW_UNITS           CFS\nFLOW_ROUTING         DYNWAVE\n"
      << "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
      << "END_DATE             01/01/2026\nEND_TIME             00:30:00\n"
      << "ROUTING_STEP         5\nREPORT_STEP          00:05:00\n\n"
      << "[JUNCTIONS]\n;;Name Elev MaxDepth InitDepth SurDepth Aponded\n"
      << "J0     10.0 10 0.5 0 0\n\n"
      << "[OUTFALLS]\n;;Name Elev Type StageData Gated\nOUT 7.0 FREE  NO\n\n"
      << "[CONDUITS]\n;;Name From To Length N Zin Zout Q0\n"
      << "C1 J0 OUT 400 0.013 0 0 0\n\n"
      << "[XSECTIONS]\n;;Link Shape G1 G2 G3 G4\nC1 CIRCULAR 1.5 0 0 0\n\n";
    if (!pc_lines.empty())
        f << "[PROCESS_COMPONENTS]\n" << pc_lines << "\n";
    f << "[REPORT]\nINPUT NO\n";
}

class ProcessComponentsTest : public ::testing::Test {
protected:
    void SetUp() override {
        engine_ = swmm_engine_create();
        ASSERT_NE(engine_, nullptr);
    }
    void TearDown() override {
        if (engine_) { swmm_engine_destroy(engine_); engine_ = nullptr; }
    }
    int open_deck(const char* inp, const char* rpt, const char* out) {
        return swmm_engine_open(engine_, inp, rpt, out, nullptr);
    }
    static bool errors_contain(const openswmm::SimulationContext& ctx,
                               const std::string& needle) {
        for (const auto& e : ctx.errors)
            if (e.find(needle) != std::string::npos) return true;
        return false;
    }
    SWMM_Engine engine_ = nullptr;
};

// ---------------------------------------------------------------------------
// IO1 — unknown id fails a strict open with the known-id list.
// ---------------------------------------------------------------------------
TEST_F(ProcessComponentsTest, UnknownIdFailsStrictOpen) {
    write_deck("_pc_unknown.inp",
               "org.example.nosuch      config=\"x.cfg\"");
    EXPECT_NE(open_deck("_pc_unknown.inp", "_pc_unknown.rpt",
                        "_pc_unknown.out"), SWMM_OK);
    const auto& ctx = as_cpp_engine(engine_).context();
    EXPECT_TRUE(errors_contain(ctx, "unknown component id 'org.example.nosuch'"));
    EXPECT_TRUE(errors_contain(ctx, "org.hydrocouple.openswmm.reactions"))
        << "diagnostic must list the known ids";
    std::remove("_pc_unknown.inp");
}

// ---------------------------------------------------------------------------
// IO1 — planned-but-unimplemented id gives the phase-specific diagnostic.
// ---------------------------------------------------------------------------
TEST_F(ProcessComponentsTest, PlannedIdReportsPendingPhase) {
    write_deck("_pc_planned.inp",
               "org.hydrocouple.openswmm.reactions   config=\"model.rxn\"");
    EXPECT_NE(open_deck("_pc_planned.inp", "_pc_planned.rpt",
                        "_pc_planned.out"), SWMM_OK);
    const auto& ctx = as_cpp_engine(engine_).context();
    EXPECT_TRUE(errors_contain(ctx, "recognized but not yet implemented"));
    EXPECT_TRUE(errors_contain(ctx, "R1"));
    std::remove("_pc_planned.inp");
}

// ---------------------------------------------------------------------------
// IO2 — a registered component receives its config sections, in order, with
// the relative path resolved against the .inp directory.
// ---------------------------------------------------------------------------
TEST_F(ProcessComponentsTest, RegisteredComponentReceivesConfig) {
    // Register a test component (process-global registry; overwritten on
    // re-registration, so repeated test runs are fine).
    struct Captured {
        std::vector<std::string> tags;
        std::string source_path;
        std::string arg_version;
        int calls = 0;
    };
    static Captured cap;
    cap = {};
    openswmm::components::ProcessComponentRegistry::instance()
        .register_component(
            "org.test.iogate", "IO2 gate test component",
            [](openswmm::SimulationContext&,
               const openswmm::ProcessComponentSpec& spec,
               const openswmm::components::ComponentConfigSections& cfg,
               std::vector<std::string>&) {
                cap.calls++;
                cap.source_path = cfg.source_path;
                for (const auto& s : cfg.sections) cap.tags.push_back(s.first);
                for (const auto& a : spec.args)
                    if (a.first == "version") cap.arg_version = a.second;
            });

    // The deck lives in a SUBDIRECTORY of the working directory so that
    // config="…" exercises the [2D_MESH_FILE] §3 rule — resolution against
    // the .inp's own directory, NOT the process CWD. Were base_dir ignored,
    // the open below would fail with "not found or unreadable".
    std::filesystem::create_directories("_pc_sub");
    {
        std::ofstream c("_pc_sub/_pc_gate.cfg");
        c << ";; test component config\n"
          << "[ALPHA_OPTIONS]\nKEY1 VALUE1   ;; trailing comment\n\n"
          << "[BETA_TABLE]\nrow1 a b\nrow2 c d\n";
    }
    write_deck("_pc_sub/_pc_ok.inp",
               "org.test.iogate      config=\"_pc_gate.cfg\" version=\"9.9\"");
    ASSERT_EQ(open_deck("_pc_sub/_pc_ok.inp", "_pc_sub/_pc_ok.rpt",
                        "_pc_sub/_pc_ok.out"), SWMM_OK);

    EXPECT_EQ(cap.calls, 1);
    ASSERT_EQ(cap.tags.size(), 2u);
    EXPECT_EQ(cap.tags[0], "ALPHA_OPTIONS");
    EXPECT_EQ(cap.tags[1], "BETA_TABLE");
    EXPECT_EQ(cap.arg_version, "9.9");
    EXPECT_NE(cap.source_path.find("_pc_sub"), std::string::npos)
        << "config path must resolve against the .inp directory, got '"
        << cap.source_path << "'";

    // Round-trip: write the model back out and reopen it — the registration
    // line, its config path and its extra args must all survive, and the
    // reopened deck must re-deliver the config to the component.
    {
        const auto& ctx = as_cpp_engine(engine_).context();
        ASSERT_EQ(ctx.process_component_specs.size(), 1u);
        ASSERT_EQ(openswmm::inp_writer::writeInpFile(ctx, "_pc_sub/_pc_rt.inp"),
                  0);
    }
    cap = {};
    swmm_engine_destroy(engine_);
    engine_ = swmm_engine_create();
    ASSERT_NE(engine_, nullptr);
    ASSERT_EQ(open_deck("_pc_sub/_pc_rt.inp", "_pc_sub/_pc_rt.rpt",
                        "_pc_sub/_pc_rt.out"), SWMM_OK);
    {
        const auto& rt = as_cpp_engine(engine_).context();
        ASSERT_EQ(rt.process_component_specs.size(), 1u);
        EXPECT_EQ(rt.process_component_specs[0].id, "org.test.iogate");
        EXPECT_EQ(rt.process_component_specs[0].config_path, "_pc_gate.cfg");
        ASSERT_EQ(rt.process_component_specs[0].args.size(), 1u);
        EXPECT_EQ(rt.process_component_specs[0].args[0].first, "version");
        EXPECT_EQ(rt.process_component_specs[0].args[0].second, "9.9");
    }
    EXPECT_EQ(cap.calls, 1) << "reopened deck did not re-deliver the config";

    std::error_code ec;
    std::filesystem::remove_all("_pc_sub", ec);
}

// ---------------------------------------------------------------------------
// IO2 — missing config file and nested [PROCESS_COMPONENTS] are fatal.
// ---------------------------------------------------------------------------
TEST_F(ProcessComponentsTest, MissingConfigAndNestingAreFatal) {
    openswmm::components::ProcessComponentRegistry::instance()
        .register_component(
            "org.test.iogate2", "IO2 fatality test component",
            [](openswmm::SimulationContext&,
               const openswmm::ProcessComponentSpec&,
               const openswmm::components::ComponentConfigSections&,
               std::vector<std::string>&) {});

    write_deck("_pc_missing.inp",
               "org.test.iogate2      config=\"_does_not_exist.cfg\"");
    EXPECT_NE(open_deck("_pc_missing.inp", "_pc_missing.rpt",
                        "_pc_missing.out"), SWMM_OK);
    EXPECT_TRUE(errors_contain(as_cpp_engine(engine_).context(),
                               "not found or unreadable"));
    std::remove("_pc_missing.inp");

    swmm_engine_destroy(engine_);
    engine_ = swmm_engine_create();
    ASSERT_NE(engine_, nullptr);

    {
        std::ofstream c("_pc_nested.cfg");
        c << "[PROCESS_COMPONENTS]\norg.test.iogate2 config=\"x\"\n";
    }
    write_deck("_pc_nested.inp",
               "org.test.iogate2      config=\"_pc_nested.cfg\"");
    EXPECT_NE(open_deck("_pc_nested.inp", "_pc_nested.rpt",
                        "_pc_nested.out"), SWMM_OK);
    EXPECT_TRUE(errors_contain(as_cpp_engine(engine_).context(),
                               "nested [PROCESS_COMPONENTS]"));
    std::remove("_pc_nested.inp");
    std::remove("_pc_nested.cfg");
}

}  // namespace
