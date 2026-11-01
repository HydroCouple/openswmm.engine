// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include "2d/solver/ExplicitInertialSolver.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include "2d/data/SolverOptions2D.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include "core/CompletedSourceArchive.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace openswmm::twoD;

TEST(TimestepReduction, MoreRequestedWorkersThanWetCellsMatchesSerialInEveryMomentumMode) {
    for (auto mode : {Momentum2D::LOCAL_INERTIAL, Momentum2D::FULL_SWE,
                      Momentum2D::DIFFUSIVE_WAVE})
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
                    ASSERT_DOUBLE_EQ(solver.advance(step * .5, (step + 1) * .5),
                                     (step + 1) * .5);
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
                    const auto file = std::to_string(int(mode)) + "-" + std::to_string(wet) +
                                      "-" + std::to_string(threads) + ".bin";
                    std::ofstream(std::filesystem::path(output) / file, std::ios::binary)
                        .write(bytes.data(), bytes.size());
                }
                solver.finalize();
            }
        }
}
