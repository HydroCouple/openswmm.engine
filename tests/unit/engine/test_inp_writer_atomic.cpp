// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include "core/AtomicOutputFile.hpp"
#include "core/InpWriter.hpp"
#include "core/SimulationContext.hpp"
#include "2d/data/MeshData.hpp"
#include "2d/data/SolverOptions2D.hpp"
#include "plugins/ProcessComponentRegistry.hpp"
#include <cstdlib>
#include <fstream>
#ifndef _WIN32
#include <csignal>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using openswmm::io::AtomicOutputFile;
using openswmm::io::path_utf8;

namespace {
std::string read(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
void put(const fs::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary); f << text; f.close(); EXPECT_TRUE(f.good());
}
#ifndef _WIN32
template<class F> bool limited(rlim_t bytes, F action) {
    const pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        struct rlimit limit;
        if (getrlimit(RLIMIT_FSIZE, &limit) != 0) _exit(2);
        limit.rlim_cur = bytes;
        std::signal(SIGXFSZ, SIG_IGN);
        if (setrlimit(RLIMIT_FSIZE, &limit) != 0) _exit(3);
        const bool ok = action();
        _exit(ok ? 0 : 1);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
#endif

class InpWriterAtomic : public testing::Test {
protected:
    fs::path dir, model, meshFile;
    openswmm::SimulationContext ctx;
    openswmm::twoD::MeshData mesh;
    openswmm::twoD::SolverOptions2D options;
    void SetUp() override {
        const char* root = std::getenv("OPENSWMM_ATOMIC_WRITE_TEST_OUTPUT");
        dir = root ? openswmm::io::utf8_path(std::string(root)) : fs::path("_atomic_writer");
        dir /= std::string(testing::UnitTest::GetInstance()->current_test_info()->name()) + "-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        fs::create_directories(dir);
        model = dir / "model.inp"; meshFile = dir / "mesh.2dm";
        put(model, "original model\n"); put(meshFile, "original mesh\n");
        ctx.title_notes.push_back("atomic writer test");
    }
    void attachMesh() {
        mesh.resize_vertices(3); mesh.resize_triangles(1);
        mesh.vx[1] = 1; mesh.vy[2] = 1; mesh.set_triangle(0, 0, 1, 2);
        options.mesh_file = path_utf8(fs::absolute(meshFile));
        ctx.twod_io.mesh = &mesh; ctx.twod_io.options = &options;
    }
    int write(std::vector<std::string>* warnings = nullptr) {
        return openswmm::inp_writer::writeInpFile(ctx, path_utf8(model), warnings);
    }
    void TearDown() override {
        for (const auto& p : fs::recursive_directory_iterator(dir))
            EXPECT_NE(p.path().filename().string().find(".openswmm-save-"), 0u) << p.path();
    }
};

TEST_F(InpWriterAtomic, ReplacesExistingUnicodeFileAndPreservesPermissions) {
    model = dir / openswmm::io::utf8_path(std::string("modèle-河.inp"));
    put(model, "old");
    fs::permissions(model, fs::perms::owner_read | fs::perms::owner_write);
    ASSERT_EQ(write(), 0);
    EXPECT_NE(read(model).find("atomic writer test"), std::string::npos);
    EXPECT_EQ(fs::status(model).permissions() & fs::perms::group_write, fs::perms::none);
    EXPECT_EQ(write(), 0);
}
TEST_F(InpWriterAtomic, RefusesDirectoryAndMissingParent) {
    model = dir / "directory"; fs::create_directory(model);
    EXPECT_NE(write(), 0); EXPECT_TRUE(fs::is_directory(model));
    model = dir / "missing" / "model.inp";
    EXPECT_NE(write(), 0); EXPECT_FALSE(fs::exists(model));
}
TEST_F(InpWriterAtomic, RefusesReadOnlyOutput) {
    fs::permissions(model, fs::perms::owner_read);
    EXPECT_NE(write(), 0); EXPECT_EQ(read(model), "original model\n");
    fs::permissions(model, fs::perms::owner_read | fs::perms::owner_write);
}
TEST_F(InpWriterAtomic, SidecarOpenFailureKeepsModel) {
    attachMesh(); fs::remove(meshFile); fs::create_directory(meshFile);
    std::vector<std::string> warnings;
    EXPECT_NE(write(&warnings), 0);
    EXPECT_EQ(read(model), "original model\n"); EXPECT_FALSE(warnings.empty());
}
TEST_F(InpWriterAtomic, SidecarSuccessAndRebasedReference) {
    attachMesh(); ASSERT_EQ(write(), 0);
    EXPECT_NE(read(meshFile).find("[2D_VERTICES]"), std::string::npos);
    EXPECT_NE(read(model).find("FILE mesh.2dm"), std::string::npos);
}
TEST_F(InpWriterAtomic, AbandonedOutputIsDiscarded) {
    { AtomicOutputFile output(model); ASSERT_NE(output.stream(), nullptr);
      std::fputs("partial replacement", output.stream()); }
    EXPECT_EQ(read(model), "original model\n");
}
TEST_F(InpWriterAtomic, PublicationFailureKeepsCompetingDestination) {
    const auto target = dir / "new.inp";
    { AtomicOutputFile output(target); ASSERT_NE(output.stream(), nullptr);
      std::fputs("new", output.stream());
      fs::create_directory(target); put(target / "keep", "do not replace");
      EXPECT_FALSE(output.commit()); EXPECT_FALSE(output.error().empty()); }
    EXPECT_EQ(read(target / "keep"), "do not replace");
}
#ifndef _WIN32
TEST_F(InpWriterAtomic, SymlinkSaveUpdatesTargetAndKeepsLink) {
    const auto original = model;
    model = dir / "alias.inp"; fs::create_symlink("model.inp", model);
    ASSERT_EQ(write(), 0); EXPECT_TRUE(fs::is_symlink(model));
    EXPECT_NE(read(original).find("atomic writer test"), std::string::npos);
}
TEST_F(InpWriterAtomic, DanglingSymlinkErrorNamesRequestedDestination) {
    const auto link = dir / "dangling.inp";
    fs::create_symlink("missing.inp", link);
    AtomicOutputFile output(link);
    EXPECT_EQ(output.stream(), nullptr);
    EXPECT_NE(output.error().find(path_utf8(link)), std::string::npos);
    EXPECT_TRUE(fs::is_symlink(link));
    EXPECT_FALSE(fs::exists(dir / "missing.inp"));
}
TEST_F(InpWriterAtomic, BufferedFlushFailureKeepsOriginal) {
    EXPECT_TRUE(limited(16, [&] {
        AtomicOutputFile output(model);
        if (!output.stream()) return false;
        char buffer[4096];
        if (std::setvbuf(output.stream(), buffer, _IOFBF, sizeof buffer) != 0) return false;
        std::fputs("a buffered payload longer than sixteen bytes", output.stream());
        return !output.commit() && !output.error().empty();
    }));
    EXPECT_EQ(read(model), "original model\n");
}
TEST_F(InpWriterAtomic, MainShortWriteKeepsOriginalAndRetrySucceeds) {
    ctx.title_notes.assign(800, std::string(120, 'x'));
    EXPECT_TRUE(limited(32768, [&] { return write() != 0; }));
    EXPECT_EQ(read(model), "original model\n");
    ASSERT_EQ(write(), 0); EXPECT_GT(read(model).size(), 32768u);
}
TEST_F(InpWriterAtomic, SidecarShortWriteKeepsBothFilesAndRetrySucceeds) {
    attachMesh(); mesh.vtag[0] = std::string(131072, 'x');
    EXPECT_TRUE(limited(32768, [&] { return write() != 0; }));
    EXPECT_EQ(read(model), "original model\n");
    EXPECT_EQ(read(meshFile), "original mesh\n");
    mesh.vtag[0] = "corridor";
    ASSERT_EQ(write(), 0); EXPECT_NE(read(meshFile).find("corridor"), std::string::npos);
}
#endif
} // namespace


class InpWriterComponent : public InpWriterAtomic {
protected:
    fs::path sourceConfig, targetConfig;
    void component(bool rendered, bool absolute = false) {
        fs::create_directories(dir / "source");
        fs::create_directories(dir / "saved");
        sourceConfig = fs::absolute(dir / "source" / "component.cfg");
        targetConfig = fs::absolute(dir / "saved" / "component.cfg");
        model = fs::absolute(dir / "saved" / "model.inp");
        put(model, "original model\n");
        put(sourceConfig, "original config\n");
        put(targetConfig, "previous destination\n");
        openswmm::ProcessComponentSpec spec;
        spec.id = "org.test.checked-component-save";
        spec.config_path = absolute ? path_utf8(targetConfig) : "component.cfg";
        spec.resolved_config_path = path_utf8(sourceConfig);
        ctx.process_component_specs.push_back(spec);
        openswmm::components::ProcessComponentRegistry::instance().register_component(
            spec.id, "checked component writer test", nullptr,
            rendered ? openswmm::components::ComponentConfigSave(
                [](const openswmm::SimulationContext& c, const openswmm::ProcessComponentSpec&) {
                    // A compact title requests a large component without making the INP large.
                    return c.title_notes.front() == "large config"
                        ? std::string(100000, 'x') : std::string("rendered config\n");
                }) : nullptr);
    }
};

TEST_F(InpWriterComponent, RenderedFailurePreservesModelAndRetry) {
    component(true);
    fs::permissions(targetConfig, fs::perms::owner_read);
    std::vector<std::string> warnings;
    EXPECT_NE(write(&warnings), 0);
    EXPECT_EQ(read(model), "original model\n");
    EXPECT_EQ(read(targetConfig), "previous destination\n");
    ASSERT_FALSE(warnings.empty());
    EXPECT_NE(warnings.back().find("component.cfg"), std::string::npos);
    fs::permissions(targetConfig, fs::perms::owner_read | fs::perms::owner_write);
    ASSERT_EQ(write(), 0);
    EXPECT_EQ(read(targetConfig), "rendered config\n");
    EXPECT_EQ(read(sourceConfig), "original config\n");
}

TEST_F(InpWriterComponent, CopyFailurePreservesModelAndRetry) {
    component(false);
    fs::remove(targetConfig); fs::create_directory(targetConfig);
    EXPECT_NE(write(), 0);
    EXPECT_EQ(read(model), "original model\n");
    fs::remove(targetConfig);
    ASSERT_EQ(write(), 0);
    EXPECT_EQ(read(targetConfig), "original config\n");
}

TEST_F(InpWriterComponent, MissingCopySourceIsFailure) {
    component(false); fs::remove(sourceConfig);
    EXPECT_NE(write(), 0);
    EXPECT_EQ(read(model), "original model\n");
    EXPECT_EQ(read(targetConfig), "previous destination\n");
}

TEST_F(InpWriterComponent, AbsoluteRenderedReferenceWritesReferencedFile) {
    component(true, true);
    ASSERT_EQ(write(), 0);
    EXPECT_EQ(read(targetConfig), "rendered config\n");
    EXPECT_NE(read(model).find("config=\"component.cfg\""), std::string::npos);
}

TEST_F(InpWriterComponent, CopySameSourceIsUnchanged) {
    component(false);
    ctx.process_component_specs.front().resolved_config_path = path_utf8(targetConfig);
    ASSERT_EQ(write(), 0);
    EXPECT_EQ(read(targetConfig), "previous destination\n");
}

#ifndef _WIN32
TEST_F(InpWriterComponent, RenderedShortWritePreservesConfigAndModel) {
    component(true); ctx.title_notes.front() = "large config";
    ASSERT_TRUE(limited(32768, [&] {
        return write() != 0 && read(targetConfig) == "previous destination\n"
            && read(model) == "original model\n";
    }));
    ASSERT_EQ(write(), 0); EXPECT_EQ(read(targetConfig).size(), 100000u);
}
TEST_F(InpWriterComponent, CopyShortWritePreservesConfigAndModel) {
    component(false); put(sourceConfig, std::string(100000, 'x'));
    ASSERT_TRUE(limited(32768, [&] {
        return write() != 0 && read(targetConfig) == "previous destination\n"
            && read(model) == "original model\n";
    }));
    ASSERT_EQ(write(), 0); EXPECT_EQ(read(targetConfig), read(sourceConfig));
}
#endif

TEST_F(InpWriterComponent, RepeatedRenderedSaveReportsReplacementOnlyOnce) {
    component(true);
    std::vector<std::string> warnings;
    ASSERT_EQ(write(&warnings), 0);
    ASSERT_FALSE(warnings.empty());
    EXPECT_NE(warnings.back().find("replaced an existing, different"), std::string::npos);
    warnings.clear();
    ASSERT_EQ(write(&warnings), 0);
    EXPECT_TRUE(warnings.empty());
}


TEST_F(InpWriterComponent, StagingMapsEveryOutputAndKeepsFinalReferences) {
    component(true); attachMesh();
    fs::create_directories(dir / "staging");
    std::vector<std::pair<std::string, int>> outputs;
    openswmm::inp_writer::InpWriteOptions opts;
    opts.map_output = [&](const std::string& finalPath, auto kind) {
        outputs.emplace_back(finalPath, static_cast<int>(kind));
        return path_utf8(dir / "staging" / (std::to_string(static_cast<int>(kind)) + ".stage"));
    };
    ASSERT_EQ(openswmm::inp_writer::writeInpFile(ctx, path_utf8(model), nullptr, opts), 0);
    ASSERT_EQ(outputs.size(), 3u);
    EXPECT_EQ(read(model), "original model\n");
    EXPECT_EQ(read(targetConfig), "previous destination\n");
    EXPECT_EQ(read(meshFile), "original mesh\n");
    EXPECT_EQ(read(dir / "staging/2.stage"), "rendered config\n");
    const auto staged = read(dir / "staging/0.stage");
    EXPECT_NE(staged.find("config=\"component.cfg\""), std::string::npos);
    EXPECT_NE(staged.find("FILE ../mesh.2dm"), std::string::npos);
    EXPECT_EQ(staged.find(".stage"), std::string::npos);
    EXPECT_NE(read(dir / "staging/1.stage").find("[2D_VERTICES]"), std::string::npos);
}

TEST_F(InpWriterComponent, StagingRefusalLeavesAllFinalOutputsUnchanged) {
    component(true); attachMesh();
    openswmm::inp_writer::InpWriteOptions opts;
    opts.map_output = [&](const std::string&, auto kind) {
        return kind == openswmm::inp_writer::InpWriteOptions::OutputKind::Mesh
            ? std::string() : path_utf8(dir / (std::to_string(static_cast<int>(kind)) + ".stage"));
    };
    std::vector<std::string> warnings;
    EXPECT_NE(openswmm::inp_writer::writeInpFile(ctx, path_utf8(model), &warnings, opts), 0);
    EXPECT_EQ(read(model), "original model\n");
    EXPECT_EQ(read(targetConfig), "previous destination\n");
    EXPECT_EQ(read(meshFile), "original mesh\n");
    ASSERT_FALSE(warnings.empty());
}

TEST_F(InpWriterComponent, StagingCopiesConfigWithoutCreatingFinalDirectories) {
    component(false);
    ctx.process_component_specs.front().config_path = "nested/component.cfg";
    openswmm::inp_writer::InpWriteOptions opts;
    opts.map_output = [&](const std::string&, auto kind) {
        return path_utf8(dir / (std::to_string(static_cast<int>(kind)) + ".stage"));
    };
    ASSERT_EQ(openswmm::inp_writer::writeInpFile(ctx, path_utf8(model), nullptr, opts), 0);
    EXPECT_FALSE(fs::exists(model.parent_path() / "nested"));
    EXPECT_EQ(read(dir / "2.stage"), "original config\n");
    EXPECT_EQ(read(model), "original model\n");
}

TEST_F(InpWriterComponent, StagingRejectsFinalDestinationAlias) {
    component(true);
    const auto alias = dir / "alias.inp";
    fs::create_hard_link(model, alias);
    openswmm::inp_writer::InpWriteOptions opts;
    opts.map_output = [&](const std::string&, auto) { return path_utf8(alias); };
    EXPECT_NE(openswmm::inp_writer::writeInpFile(ctx, path_utf8(model), nullptr, opts), 0);
    EXPECT_EQ(read(model), "original model\n");
    EXPECT_EQ(read(targetConfig), "previous destination\n");
}
#ifndef _WIN32
TEST_F(InpWriterComponent, StagedShortWritePreservesAllFinalOutputs) {
    component(true); attachMesh(); ctx.title_notes.front() = "large config";
    openswmm::inp_writer::InpWriteOptions opts;
    opts.map_output = [&](const std::string&, auto kind) {
        return path_utf8(dir / (std::to_string(static_cast<int>(kind)) + ".stage"));
    };
    ASSERT_TRUE(limited(32768, [&] {
        return openswmm::inp_writer::writeInpFile(ctx, path_utf8(model), nullptr, opts) != 0
            && read(model) == "original model\n"
            && read(targetConfig) == "previous destination\n"
            && read(meshFile) == "original mesh\n";
    }));
}
#endif
