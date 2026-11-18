// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include "2d/solver/ExplicitInertialSolver.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include "2d/data/SolverOptions2D.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include "core/CompletedSourceArchive.hpp"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace openswmm::twoD;

TEST(TimestepReduction, MoreRequestedWorkersThanWetCellsMatchesSerialInEveryMomentumMode) {
    for (auto mode : {Momentum2D::LOCAL_INERTIAL, Momentum2D::FULL_SWE, Momentum2D::DIFFUSIVE_WAVE})
        for (int wet : {0, 1, 2}) {
            std::string reference;
            for (int threads : {1, 16}) {
                MeshData mesh;
                mesh.resize_vertices(4);
                mesh.vx = {0, 2, 2, 0};
                mesh.vy = {0, 0, 2, 2};
                mesh.vz = {0, 0, 0, 0};
                mesh.resize_triangles(2);
                mesh.set_triangle(0, 0, 1, 2);
                mesh.set_triangle(1, 0, 2, 3);
                mesh.mannings_n = {.03, .03};
                buildMeshTopology(mesh);
                SurfaceStateData state;
                state.resize(2, mesh.n_vertices());
                state.depth = {wet > 0 ? .6 : 0., wet > 1 ? .01 : 0.};
                state.head = state.depth;
                for (int i = 0; i < 2; ++i)
                    state.volume[i] = state.depth[i] * mesh.tri_area[i];
                SolverOptions2D options;
                options.momentum = mode;
                options.num_threads = threads;
                options.cfl_number = .15;
                options.max_timestep = 1.;
                options.lts_tiers = 3;
                ExplicitInertialSolver solver;
                solver.initialize(mesh, state, options);
                openswmm::detail::CompletedSourceArchive archive;
                for (int step = 0; step < 3; ++step) {
                    ASSERT_DOUBLE_EQ(solver.advance(step * .5, (step + 1) * .5), (step + 1) * .5);
                    auto count = solver.last_num_steps();
                    auto dt = solver.last_step_size();
                    archive(state.depth, state.head, state.volume, state.edge_flux, count, dt);
                }
                const auto bytes = archive.bytes();
                if (threads == 1)
                    reference = bytes;
                else
                    EXPECT_EQ(bytes, reference) << int(mode) << '/' << wet;
                if (const char* output = std::getenv("OPENSWMM_DT_REDUCTION_TEST_OUT")) {
                    std::filesystem::create_directories(output);
                    const auto file = std::to_string(int(mode)) + "-" + std::to_string(wet) + "-" +
                                      std::to_string(threads) + ".bin";
                    std::ofstream(std::filesystem::path(output) / file, std::ios::binary)
                        .write(bytes.data(), bytes.size());
                }
                solver.finalize();
            }
        }
}

// A cell that drains to the 1D network stays a film while its deeper neighbors feed it, so its
// |q|/h reaches hundreds of m/s. The CFL cell speed is limited to FROUDE_MAX·√(g·h), which keeps
// the step from collapsing.
TEST(TimestepReduction, DrainingFilmCellDoesNotCollapseTheLocalInertialStep) {
    const int nx = 12;
    MeshData mesh;
    mesh.resize_vertices(2 * (nx + 1));
    for (int i = 0; i <= nx; ++i) {
        mesh.vx[i] = mesh.vx[nx + 1 + i] = 5.0 * i;
        mesh.vy[i] = 0.0;
        mesh.vy[nx + 1 + i] = 5.0;
        mesh.vz[i] = mesh.vz[nx + 1 + i] = 0.0;
    }
    mesh.resize_triangles(2 * nx);
    for (int i = 0; i < nx; ++i) {
        mesh.set_triangle(2 * i, i, i + 1, nx + 2 + i);
        mesh.set_triangle(2 * i + 1, i, nx + 2 + i, nx + 1 + i);
    }
    mesh.mannings_n.assign(2 * nx, .03);
    buildMeshTopology(mesh);
    SurfaceStateData state;
    state.resize(2 * nx, mesh.n_vertices());
    for (int i = 0; i < 2 * nx; ++i) {
        state.depth[i] = state.head[i] = 0.15;
        state.volume[i] = 0.15 * mesh.tri_area[i];
    }
    state.coupling_flux[nx] = -0.05; // an inlet taking far more than its neighbors deliver
    SolverOptions2D options;
    options.momentum = Momentum2D::LOCAL_INERTIAL;
    options.dry_depth = 1.0e-4;
    options.num_threads = 1;
    options.lts_tiers = 1;
    options.max_timestep = 1.;
    ExplicitInertialSolver solver;
    solver.initialize(mesh, state, options);
    // The drain only lowers the water from its initial 0.15 m, so every cell's step stays near
    // CFL·L/((1 + FROUDE_MAX)·√(g·0.15)) = 0.7·2.357/(2.5·1.213) = 0.54 s or longer: at most two
    // CFL steps and a remainder per second. Without the limit the draining cell forces nine.
    long most = 0;
    for (int k = 0; k < 60; ++k) {
        ASSERT_DOUBLE_EQ(solver.advance(k, k + 1.), k + 1.);
        most = std::max(most, solver.last_num_steps());
    }
    EXPECT_LE(most, 3);
    EXPECT_LT(state.depth[nx], 1.0e-4) << "the inlet cell must have drained to a film";
    solver.finalize();
}
