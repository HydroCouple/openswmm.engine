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
#include <sstream>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>

#include "core/InpWriter.hpp"
#include "core/SWMMEngine.hpp"
#include "plugins/DefaultInputPlugin.hpp"
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
//
// The id here must be one whose component has NOT landed yet: R1 registered
// the real reactions component, so that id now takes the implemented path.
// Heat was the stand-in until H1 landed and registered the real heat
// component; per this comment's own instruction the stand-in moved on to
// LARD (T5), which is still planned. Move it again when T5 lands.
// ---------------------------------------------------------------------------
TEST_F(ProcessComponentsTest, PlannedIdReportsPendingPhase) {
    write_deck("_pc_planned.inp",
               "org.hydrocouple.openswmm.transport.lard config=\"model.lard\"");
    EXPECT_NE(open_deck("_pc_planned.inp", "_pc_planned.rpt",
                        "_pc_planned.out"), SWMM_OK);
    const auto& ctx = as_cpp_engine(engine_).context();
    EXPECT_TRUE(errors_contain(ctx, "recognized but not yet implemented"));
    EXPECT_TRUE(errors_contain(ctx, "T5"));
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

// ---------------------------------------------------------------------------
// Slice IO-4 — an absolute config="…" is rebased against the destination
// directory on save, like every other external-file path slot. A GUI that
// hands the engine an absolute path (or an API-built model) must not have it
// frozen into the .inp: the saved deck has to stay portable AND reopenable.
// ---------------------------------------------------------------------------
TEST_F(ProcessComponentsTest, AbsoluteConfigPathIsRebasedOnWrite) {
    namespace fs = std::filesystem;
    openswmm::components::ProcessComponentRegistry::instance()
        .register_component(
            "org.test.iogate3", "Slice IO-4 rebase test component",
            [](openswmm::SimulationContext&,
               const openswmm::ProcessComponentSpec&,
               const openswmm::components::ComponentConfigSections&,
               std::vector<std::string>&) {});

    fs::create_directories("_pc_abs");
    { std::ofstream c("_pc_abs/_pc_abs.cfg"); c << "[ALPHA_OPTIONS]\nKEY1 V\n"; }
    write_deck("_pc_abs/_pc_abs.inp",
               "org.test.iogate3      config=\"_pc_abs.cfg\"");
    ASSERT_EQ(open_deck("_pc_abs/_pc_abs.inp", "_pc_abs/_pc_abs.rpt",
                        "_pc_abs/_pc_abs.out"), SWMM_OK);

    const fs::path abs_cfg = fs::absolute("_pc_abs/_pc_abs.cfg");
    auto& ctx = as_cpp_engine(engine_).context();
    ASSERT_EQ(ctx.process_component_specs.size(), 1u);
    ctx.process_component_specs[0].config_path = abs_cfg.string();

    const fs::path out_inp = fs::absolute("_pc_abs") / "_pc_abs_rt.inp";
    ASSERT_EQ(openswmm::inp_writer::writeInpFile(ctx, out_inp.string()), 0);

    // The emitted row must carry the rebased token, not the absolute one.
    std::string row;
    {
        std::ifstream in(out_inp);
        std::string line;
        bool in_section = false;
        while (std::getline(in, line)) {
            if (!line.empty() && line[0] == '[') {
                in_section = line.rfind("[PROCESS_COMPONENTS]", 0) == 0;
                continue;
            }
            if (in_section && !line.empty() && line[0] != ';') { row = line; break; }
        }
    }
    ASSERT_FALSE(row.empty()) << "[PROCESS_COMPONENTS] row was not emitted";
    EXPECT_EQ(row.find(abs_cfg.parent_path().string()), std::string::npos)
        << "absolute config path written verbatim, bypassing the Slice IO-4 "
           "rebase every other external-file slot honors: " << row;
    EXPECT_NE(row.find("config=\"_pc_abs.cfg\""), std::string::npos)
        << "expected the path rebased against the destination directory, got: "
        << row;

    // …and the rebased reference must still resolve on reopen.
    swmm_engine_destroy(engine_);
    engine_ = swmm_engine_create();
    ASSERT_NE(engine_, nullptr);
    EXPECT_EQ(open_deck(out_inp.string().c_str(), "_pc_abs/_pc_abs_rt.rpt",
                        "_pc_abs/_pc_abs_rt.out"), SWMM_OK);

    std::error_code ec;
    fs::remove_all("_pc_abs", ec);
}

}  // namespace

// ---------------------------------------------------------------------------
// A save that DESTROYS embedded model data must say so, THROUGH THE PATH THE
// PRODUCTION CALLERS TAKE.
//
// Found 2026-08-26. The writer's "embedded [REACTION_*] sections are lost from
// this save" notice (InpWriter.cpp:2580-2586) is gated on an OPTIONAL warnings
// sink, and every production caller passed nullptr -- so it never fired.
// Opening a deck with embedded reaction sections, editing anything and saving
// destroyed the reaction system silently, from the GUI included.
//
// The reason it survived is the shape this gate is built to avoid: the
// existing round-trip coverage calls `inp_writer::writeInpFile` DIRECTLY and
// hands it a sink, so it exercises a code path production never takes and
// certifies a behaviour users never get. Lesson 91's family, one level out.
//
// So this gate drives `swmm_model_write_with_plugin` -- literally what the GUI
// calls -- and reads `swmm_get_warning_at`, literally what the GUI reads. It
// touches the writer's own API nowhere.
TEST_F(ProcessComponentsTest, SavingWarnsThroughTheApiWhenEmbeddedSectionsAreLost) {
    // A deck carrying an EMBEDDED [REACTION_*] section -- no external config
    // file, which is exactly the configuration that gets silently dropped.
    {
        std::ofstream f("_pc_embed.inp");
        f << "[TITLE]\nembedded reaction sections, saved via the C API\n\n"
          << "[OPTIONS]\n"
          << "FLOW_UNITS           CFS\nFLOW_ROUTING         DYNWAVE\n"
          << "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
          << "END_DATE             01/01/2026\nEND_TIME             00:30:00\n"
          << "ROUTING_STEP         5\nREPORT_STEP          00:05:00\n\n"
          << "[JUNCTIONS]\nJ0     10.0 10 0.5 0 0\n\n"
          << "[OUTFALLS]\nOUT 7.0 FREE  NO\n\n"
          << "[CONDUITS]\nC1 J0 OUT 400 0.013 0 0 0\n\n"
          << "[XSECTIONS]\nC1 CIRCULAR 1.5 0 0 0\n\n"
          << "[POLLUTANTS]\nTSS MG/L 0.0 0.0 0.0 0.0 NO * 0.0 0.0 0.0\n\n"
          << "[REACTION_OPTIONS]\nSOLVER RK5\n\n"
          // A reactions config is rejected without at least one species, so
          // the embedded block has to be a VALID one for the deck to open at
          // all. Species 'A' deliberately does not collide with the [POLLUTANTS]
          // name above -- a collision is its own open failure.
          << "[REACTION_SPECIES]\nBULK A MG\n\n"
          << "[REPORT]\nINPUT NO\n";
    }

    ASSERT_EQ(open_deck("_pc_embed.inp", "_pc_embed.rpt", "_pc_embed.out"), 0)
        << swmm_get_last_error_msg(engine_);

    // SETUP leg: the deck must actually carry an embedded section, or the
    // assertion below is vacuous and would pass on any deck at all.
    const auto& ctx = as_cpp_engine(engine_).context();
    ASSERT_FALSE(ctx.embedded_component_sections.empty())
        << "the deck carries no embedded component sections, so this gate "
           "cannot observe the loss it exists for -- fix the deck, not the "
           "assertion";

    const int warns_before = swmm_get_warning_count(engine_);

    // THE PRODUCTION PATH. Empty plugin id == the built-in writer, which is
    // what the GUI's save uses.
    ASSERT_EQ(swmm_model_write_with_plugin(engine_, "_pc_embed_saved.inp", ""), 0)
        << swmm_get_last_error_msg(engine_);

    // The save SUCCEEDS -- warn, not refuse -- but it must not be silent.
    const int warns_after = swmm_get_warning_count(engine_);
    ASSERT_GT(warns_after, warns_before)
        << "saving destroyed the embedded reaction sections and emitted NO "
           "warning through the C API. The notice exists in InpWriter but is "
           "gated on an optional sink; if this fails, a production caller is "
           "passing nullptr again.";

    // ...and it must be the RIGHT warning, naming the section it dropped.
    bool names_loss = false, names_section = false;
    for (int i = warns_before; i < warns_after; ++i) {
        const std::string w = swmm_get_warning_at(engine_, i);
        if (w.find("lost from this save") != std::string::npos) names_loss = true;
        if (w.find("REACTION_OPTIONS") != std::string::npos) names_section = true;
    }
    EXPECT_TRUE(names_loss)
        << "a warning was emitted but it does not say the data is lost";
    EXPECT_TRUE(names_section)
        << "the warning does not name the section that was dropped, so a user "
           "cannot tell what to rescue";

    // And the loss itself is real -- the saved deck has no reaction section.
    // This is the claim the warning makes; if it stops being true the warning
    // becomes a lie rather than a courtesy.
    std::ifstream in("_pc_embed_saved.inp");
    ASSERT_TRUE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    EXPECT_EQ(ss.str().find("[REACTION_OPTIONS]"), std::string::npos)
        << "the saved deck DOES contain the reaction section -- if per-component "
           "saveData() has landed (IO3), this gate and the warning it checks "
           "are both obsolete and should be replaced by a round-trip assertion";
}


// ---------------------------------------------------------------------------
// Closes the "only swmm_model_write_with_plugin is gated" gap recorded in the
// 2026-08-26 handoff §5. Falsifier ii of that round proved the point the hard
// way: reverting the sink at `swmm_model_write` alone leaves the gate above
// GREEN, because it drives the other entry point entirely. Two entry points,
// two gates.
TEST_F(ProcessComponentsTest, SwmmModelWriteAlsoWarnsWhenEmbeddedSectionsAreLost) {
    {
        std::ofstream f("_pc_embed2.inp");
        f << "[TITLE]\nembedded reaction sections, saved via swmm_model_write\n\n"
          << "[OPTIONS]\n"
          << "FLOW_UNITS           CFS\nFLOW_ROUTING         DYNWAVE\n"
          << "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
          << "END_DATE             01/01/2026\nEND_TIME             00:30:00\n"
          << "ROUTING_STEP         5\nREPORT_STEP          00:05:00\n\n"
          << "[JUNCTIONS]\nJ0     10.0 10 0.5 0 0\n\n"
          << "[OUTFALLS]\nOUT 7.0 FREE  NO\n\n"
          << "[CONDUITS]\nC1 J0 OUT 400 0.013 0 0 0\n\n"
          << "[XSECTIONS]\nC1 CIRCULAR 1.5 0 0 0\n\n"
          << "[POLLUTANTS]\nTSS MG/L 0.0 0.0 0.0 0.0 NO * 0.0 0.0 0.0\n\n"
          << "[REACTION_OPTIONS]\nSOLVER RK5\n\n"
          << "[REACTION_SPECIES]\nBULK A MG\n\n"
          << "[REPORT]\nINPUT NO\n";
    }

    ASSERT_EQ(open_deck("_pc_embed2.inp", "_pc_embed2.rpt", "_pc_embed2.out"), 0)
        << swmm_get_last_error_msg(engine_);

    const auto& ctx = as_cpp_engine(engine_).context();
    ASSERT_FALSE(ctx.embedded_component_sections.empty())
        << "the deck carries no embedded component sections, so this gate "
           "cannot observe the loss it exists for";

    const int warns_before = swmm_get_warning_count(engine_);
    ASSERT_EQ(swmm_model_write(engine_, "_pc_embed2_saved.inp"), 0)
        << swmm_get_last_error_msg(engine_);
    const int warns_after = swmm_get_warning_count(engine_);

    ASSERT_GT(warns_after, warns_before)
        << "swmm_model_write destroyed the embedded reaction sections and "
           "emitted NO warning through the C API";

    bool names_loss = false, names_section = false;
    for (int i = warns_before; i < warns_after; ++i) {
        const std::string w = swmm_get_warning_at(engine_, i);
        if (w.find("lost from this save") != std::string::npos) names_loss = true;
        if (w.find("REACTION_OPTIONS") != std::string::npos) names_section = true;
    }
    EXPECT_TRUE(names_loss);
    EXPECT_TRUE(names_section)
        << "the warning does not name the section that was dropped";
}

// ---------------------------------------------------------------------------
// Closes the "falsifier vi has no gate" gap recorded in the same handoff §5.
//
// DefaultInputPlugin::write receives a CONST context and cannot reach
// ctx.warnings, so it routes the loss notice to last_error_ -- but ONLY when
// the write actually fails. The round's author drafted it the other way first
// and caught it: a warning sitting in an ERROR channel after a SUCCESSFUL
// write is worse than the silence it replaces, because a caller checking
// last_error_message() reads it as a failure. Nothing stopped a future edit
// from putting it back. This does.
TEST_F(ProcessComponentsTest, PluginWriteLeavesTheErrorChannelCleanOnSuccess) {
    {
        std::ofstream f("_pc_embed3.inp");
        f << "[TITLE]\nembedded sections, written through the input plugin\n\n"
          << "[OPTIONS]\n"
          << "FLOW_UNITS           CFS\nFLOW_ROUTING         DYNWAVE\n"
          << "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
          << "END_DATE             01/01/2026\nEND_TIME             00:30:00\n"
          << "ROUTING_STEP         5\nREPORT_STEP          00:05:00\n\n"
          << "[JUNCTIONS]\nJ0     10.0 10 0.5 0 0\n\n"
          << "[OUTFALLS]\nOUT 7.0 FREE  NO\n\n"
          << "[CONDUITS]\nC1 J0 OUT 400 0.013 0 0 0\n\n"
          << "[XSECTIONS]\nC1 CIRCULAR 1.5 0 0 0\n\n"
          << "[POLLUTANTS]\nTSS MG/L 0.0 0.0 0.0 0.0 NO * 0.0 0.0 0.0\n\n"
          << "[REACTION_OPTIONS]\nSOLVER RK5\n\n"
          << "[REACTION_SPECIES]\nBULK A MG\n\n"
          << "[REPORT]\nINPUT NO\n";
    }

    ASSERT_EQ(open_deck("_pc_embed3.inp", "_pc_embed3.rpt", "_pc_embed3.out"), 0)
        << swmm_get_last_error_msg(engine_);

    const auto& ctx = as_cpp_engine(engine_).context();
    ASSERT_FALSE(ctx.embedded_component_sections.empty())
        << "the deck carries no embedded component sections, so a successful "
           "write here would drop nothing and the gate would be vacuous";

    openswmm::DefaultInputPlugin plugin;
    ASSERT_EQ(plugin.write("_pc_embed3_saved.inp", ctx), 0);

    EXPECT_STREQ(plugin.last_error_message(), "")
        << "a SUCCESSFUL write left text in the error channel. Data-loss "
           "notices must not be reported as errors -- a caller checking "
           "last_error_message() would read this as a failed save.";
}
