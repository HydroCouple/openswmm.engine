// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include "core/AtomicOutputFile.hpp"
#include "core/InpWriter.hpp"
#include "core/SimulationContext.hpp"
#include "2d/data/MeshData.hpp"
#include "2d/data/SolverOptions2D.hpp"
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
        for (const auto& p : fs::directory_iterator(dir))
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
