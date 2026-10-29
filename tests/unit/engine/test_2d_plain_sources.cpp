// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include "2d/solver/ExplicitInertialSolver.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include "core/CompletedSourceArchive.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace openswmm::twoD;

namespace {
struct PlainSourceRig {
    MeshData mesh;
    SurfaceStateData state;
    SolverOptions2D options;
    ExplicitInertialSolver solver;
    PlainSourceRig(Momentum2D mode, int threads, double depth) {
        mesh.resize_vertices(4);
        mesh.vx = {0, 2, 2, 0};
        mesh.vy = {0, 0, 2, 2};
        mesh.vz = {0, 0, 0, 0};
        mesh.resize_triangles(2);
        mesh.set_triangle(0, 0, 1, 2);
        mesh.set_triangle(1, 0, 2, 3);
        mesh.mannings_n = {.03, .03};
        buildMeshTopology(mesh);
        state.resize(2, mesh.n_vertices());
        state.depth = {depth, depth};
        state.head = state.depth;
        for (int i = 0; i < 2; ++i)
            state.volume[i] = depth * mesh.tri_area[i];
        options.momentum = mode;
        options.num_threads = threads;
        options.cfl_number = .15;
        options.max_timestep = .25;
        options.lts_tiers = 3;
        solver.initialize(mesh, state, options);
    }
    ~PlainSourceRig() { solver.finalize(); }
    void retainDisabledForcing(bool retained) {
        state.runtime_forcings = {};
        if (retained) {
            RuntimeForcing row;
            row.channel = 1;
            row.enabled = false;
            state.runtime_forcings.replace(row);
        }
        state.runtime_forcings.rebuild(2);
    }
    std::string physicalBytes() {
        openswmm::detail::CompletedSourceArchive archive;
        auto count = solver.last_num_steps();
        auto dt = solver.last_step_size();
        archive(state.volume, state.depth, state.head, state.edge_flux,
                state.infil_applied, state.coupling_applied, state.evap_loss_total, count, dt);
        for (auto& row : state.runtime_sources.rows)
            archive(row.receipt.requested, row.receipt.applied, row.receipt.water_in,
                    row.receipt.water_out, row.receipt.last_flow, row.receipt.last_dt);
        return archive.bytes();
    }
};
}

TEST(PlainSources, HeldSinksAndLiveRatesMatchFullPathAcrossAdvanceBoundaries) {
    for (auto mode : {Momentum2D::LOCAL_INERTIAL, Momentum2D::FULL_SWE,
                      Momentum2D::DIFFUSIVE_WAVE})
        for (int threads : {1, 4})
            for (double depth : {0., 1e-5, .1}) {
                PlainSourceRig plain(mode, threads, depth), full(mode, threads, depth);
                full.retainDisabledForcing(true);
                openswmm::detail::CompletedSourceArchive history;
                for (int step = 0; step < 8; ++step) {
                    // Enter and leave the full path between advances; rates remain live.
                    plain.retainDisabledForcing(step == 2 || step == 5);
                    for (auto* rig : {&plain, &full}) {
                        rig->state.runtime_sources = {};
                        if (step == 3 || step == 6) {
                            RuntimeSource source;
                            source.id = "live-water";
                            source.cell = 0;
                            source.flow = step == 3 ? .0003 : -.0002;
                            rig->state.runtime_sources.replace(source);
                        }
                        rig->state.runtime_sources.rebuild(2);
                        rig->state.rainfall = {step % 2 ? 1e-4 : -0., 1e-5};
                        rig->state.evap_rate = {step % 3 ? .03 : -0., .002};
                        rig->state.infil_rate = {step % 2 ? .02 : -0., .003};
                        rig->state.coupling_flux = {step % 2 ? -.05 : .0002, -.0003};
                        if (step == 0) {
                            rig->state.rainfall.assign(2, -0.);
                            rig->state.evap_rate.assign(2, -0.);
                            rig->state.infil_rate.assign(2, -0.);
                            rig->state.coupling_flux.assign(2, -0.);
                        }
                        ASSERT_DOUBLE_EQ(rig->solver.advance(step * .5, (step + 1) * .5),
                                         (step + 1) * .5);
                    }
                    auto expected = full.physicalBytes();
                    auto actual = plain.physicalBytes();
                    EXPECT_EQ(actual, expected) << int(mode) << '/' << threads << '/' << depth
                                                << '/' << step;
                    history(actual);
                }
                if (const char* output = std::getenv("OPENSWMM_PLAIN_SOURCE_TEST_OUT")) {
                    std::filesystem::create_directories(output);
                    const auto file = std::to_string(int(mode)) + "-" +
                                      std::to_string(threads) + "-" +
                                      std::to_string(depth) + ".bin";
                    const auto bytes = history.bytes();
                    std::ofstream(std::filesystem::path(output) / file, std::ios::binary)
                        .write(bytes.data(), bytes.size());
                }
            }
}
