// SPDX-License-Identifier: Apache-2.0
// Native consistency checks independent of UK observations and calibration.
#include <gtest/gtest.h>
#include "2d/data/MeshData.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include "2d/data/SolverOptions2D.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include "2d/solver/ExplicitInertialSolver.hpp"
#include <array>
#include <cmath>
#include <fstream>
#include <filesystem>

using namespace openswmm::twoD;
namespace openswmm::twoD {
struct ExplicitInertialSolverTestAccess {
    static void gradients(ExplicitInertialSolver& s) {
        s.syncAndRebuild(0);
        s.computeLimitedGradientsSwe();
    }
    static auto gradient(const ExplicitInertialSolver& s, int i) {
        const auto n = s.state_->depth.size();
        return std::array<double, 8>{s.gex_[i], s.gey_[i], s.gex_[n + i], s.gey_[n + i],
                                     s.gux_[i], s.guy_[i], s.gvx_[i],     s.gvy_[i]};
    }
    static std::array<double, 3> boundedBankRelease(ExplicitInertialSolver& s, double end) {
        s.syncAndRebuild(0);
        auto energy = [&]() {
            double value = 0;
            for (int i = 0; i < s.mesh_->n_cells(); ++i) {
                const double h = s.state_->depth[i];
                const double kinetic =
                    h > s.opts_->dry_depth
                        ? .5 * (s.qcx_[i] * s.qcx_[i] + s.qcy_[i] * s.qcy_[i]) / h
                        : 0;
                value += s.mesh_->tri_area[i] *
                         (kinetic + 9.80665 * (.5 * h * h + s.mesh_->tri_cz[i] * h));
            }
            return value;
        };
        const double initial = energy();
        double time = 0, maximum = initial, speed = 0;
        for (int k = 0; k < 5000 && time < end; ++k) {
            s.refreshDt0();
            const double dt = s.runMomentumStep(std::min(s.dt0_, end - time), time);
            time += dt;
            maximum = std::max(maximum, energy());
            for (int i = 0; i < s.mesh_->n_cells(); ++i)
                if (s.state_->depth[i] > s.opts_->dry_depth)
                    speed = std::max(speed, std::hypot(s.qcx_[i], s.qcy_[i]) / s.state_->depth[i]);
            if (dt < 1e-10 || !std::isfinite(maximum) || maximum > 2 * initial)
                break;
        }
        return {time, maximum / initial, speed};
    }
    static void setVelocity(ExplicitInertialSolver& s, int i, double u, double v) {
        s.qcx_[i] = s.state_->depth[i] * u;
        s.qcy_[i] = s.state_->depth[i] * v;
    }
    static void facePass(ExplicitInertialSolver& s) {
        s.syncAndRebuild(0);
        s.fireFacesSwe(s.active_faces_, .001, true);
    }
    template <class Solver>
    static double momentumBooking(const Solver& s, int edge, bool left, bool x) {
        // Support both the separate and interleaved private scratch layouts.
        if constexpr (requires { s.face_accum_; })
            return s.face_accum_[s.face_accum_stride_ * edge + (x ? 2 : 4) + !left];
        else if (x)
            return left ? s.macc_x_L_[edge] : s.macc_x_R_[edge];
        else
            return left ? s.macc_y_L_[edge] : s.macc_y_R_[edge];
    }
    static bool zeroBookings(const ExplicitInertialSolver& s) {
        for (int e = 0; e < s.edges_.ne; ++e)
            for (bool left : {false, true})
                for (bool x : {false, true})
                    if (momentumBooking(s, e, left, x) != 0)
                        return false;
        return true;
    }
    static double maxMomentum(const ExplicitInertialSolver& s) {
        double result = 0;
        for (std::size_t i = 0; i < s.qcx_.size(); ++i)
            result = std::max(result, std::hypot(s.qcx_[i], s.qcy_[i]));
        return result;
    }
    static std::array<double, 3> wallEuler(ExplicitInertialSolver& s, int i, double dt,
                                           std::array<double, 2> slope = {0, 0}) {
        s.syncAndRebuild(0);
        const double h = s.state_->depth[i], area = s.mesh_->tri_area[i];
        s.fireFacesSwe(s.active_faces_, dt, true);
        double x = area * s.qcx_[i], y = area * s.qcy_[i];
        for (int p = s.edges_.cell_ptr[i]; p < s.edges_.cell_ptr[i + 1]; ++p) {
            const int e = s.edges_.cell_edge[p];
            x += momentumBooking(s, e, s.edges_.cell_sign[p] > 0, true);
            y += momentumBooking(s, e, s.edges_.cell_sign[p] > 0, false);
        }
        for (int w = s.wall_ptr_[i]; w < s.wall_ptr_[i + 1]; ++w) {
            const int slot = s.wall_slot_[w];
            const double hw = h + slope[0] * (s.mesh_->edge_mx[slot] - s.mesh_->tri_cx[i]) +
                              slope[1] * (s.mesh_->edge_my[slot] - s.mesh_->tri_cy[i]);
            x -= .5 * 9.80665 * hw * hw * s.mesh_->edge_nx[slot] * s.mesh_->edge_length[slot] * dt;
            y -= .5 * 9.80665 * hw * hw * s.mesh_->edge_ny[slot] * s.mesh_->edge_length[slot] * dt;
        }
        s.fireCells(s.active_cells_, dt, true);
        return {area * s.qcx_[i] - x, area * s.qcy_[i] - y, s.state_->depth[i] - h};
    }
};
}
namespace {
constexpr int N = 8;
MeshData irregular(double angle) {
    MeshData m;
    const double co = std::cos(angle), si = std::sin(angle);
    m.resize_vertices((N + 1) * (N + 1));
    for (int j = 0; j <= N; ++j)
        for (int i = 0; i <= N; ++i) {
            const double x =
                i + ((i > 0 && i < N && j > 0 && j < N) ? .17 * std::sin(1.7 * i + 2.3 * j) : 0);
            const double y =
                j + ((i > 0 && i < N && j > 0 && j < N) ? .13 * std::cos(2.1 * i - 1.4 * j) : 0);
            const int v = j * (N + 1) + i;
            m.vx[v] = co * x - si * y;
            m.vy[v] = si * x + co * y;
            m.vz[v] = 0;
        }
    m.resize_triangles(2 * N * N);
    int c = 0;
    for (int j = 0; j < N; ++j)
        for (int i = 0; i < N; ++i) {
            const int a = j * (N + 1) + i, b = a + 1, d = a + N + 1, e = d + 1;
            m.set_triangle(c++, a, b, e);
            m.set_triangle(c++, a, e, d);
        }
    for (int i = 0; i < m.n_cells(); ++i)
        m.mannings_n[i] = 0;
    buildMeshTopology(m);
    return m;
}
struct Rig {
    MeshData mesh;
    SurfaceStateData state;
    SolverOptions2D opts;
    ExplicitInertialSolver solver;
    Rig(double angle, int field) : mesh(irregular(angle)) {
        state.resize(mesh.n_cells(), mesh.n_vertices());
        const double co = std::cos(angle), si = std::sin(angle);
        for (int i = 0; i < mesh.n_cells(); ++i) {
            const double x = co * mesh.tri_cx[i] + si * mesh.tri_cy[i];
            const double y = -si * mesh.tri_cx[i] + co * mesh.tri_cy[i];
            const double h = field == 0 ? 2 : field == 1 ? 2 + .02 * x + .03 * y : 2 + .02 * x;
            state.head[i] = state.depth[i] = h;
            state.volume[i] = mesh.tri_area[i] * h;
            mesh.tri_init_u[i] = field == 0   ? .4
                                 : field == 1 ? .4 + .01 * x - .02 * y
                                              : co * (.4 + .01 * x);
            mesh.tri_init_v[i] = field == 0   ? -.2
                                 : field == 1 ? -.2 + .02 * x + .005 * y
                                              : si * (.4 + .01 * x);
        }
        opts.momentum = Momentum2D::FULL_SWE;
        opts.reconstruction_order = 2;
        opts.lts_tiers = 1;
        opts.num_threads = 1;
        solver.initialize(mesh, state, opts);
    }
};
// Restrict linear exactness to cells where the exact midpoint values lie inside
// the native BJ stencil bounds. Otherwise a scalar maximum-principle limiter
// may legitimately clip a linear gradient on an irregular stencil.
bool admissible(const MeshData& m, int i, double bx, double by) {
    double lo = bx * m.tri_cx[i] + by * m.tri_cy[i], hi = lo;
    for (int e = 0; e < 3; ++e) {
        const int j = m.cell_neighbour(i, e);
        if (j < 0)
            return false;
        const double w = bx * m.tri_cx[j] + by * m.tri_cy[j];
        lo = std::min(lo, w);
        hi = std::max(hi, w);
    }
    for (int e = 0; e < 3; ++e) {
        const int slot = MeshData::slot(i, e);
        const double w = bx * m.edge_mx[slot] + by * m.edge_my[slot];
        if (w < lo - 1e-12 || w > hi + 1e-12)
            return false;
    }
    return true;
}
TEST(SweReconstruction, ConstantFieldsOnIrregularRotatedTin) {
    for (double angle : {0., 64. * std::acos(-1.) / 180}) {
        Rig r(angle, 0);
        ExplicitInertialSolverTestAccess::gradients(r.solver);
        for (int i = 0; i < r.mesh.n_cells(); ++i)
            for (double g : ExplicitInertialSolverTestAccess::gradient(r.solver, i))
                EXPECT_NEAR(g, 0, 2e-11);
    }
}
TEST(SweReconstruction, AdmissibleLinearFieldsAreExactOnIrregularTin) {
    for (double angle : {0., 64. * std::acos(-1.) / 180}) {
        Rig r(angle, 1);
        ExplicitInertialSolverTestAccess::gradients(r.solver);
        const double co = std::cos(angle), si = std::sin(angle);
        const std::array<std::array<double, 2>, 4> logical = {
            {{.02, .03}, {.02, .03}, {.01, -.02}, {.02, .005}}};
        // Velocity uses vertex-ring bounds. An arbitrary affine velocity
        // field is not a reflecting-wall solution; exclude every stencil
        // touching a wall vertex here. Compatible tangential wall fields
        // are verified separately below with the same exactness tolerance.
        std::vector<unsigned char> wall_vertex(r.mesh.n_vertices(), 0);
        for (int cell = 0; cell < r.mesh.n_cells(); ++cell)
            for (int e = 0; e < 3; ++e)
                if (r.mesh.cell_neighbour(cell, e) < 0) {
                    int a, b;
                    r.mesh.cell_edge_vertices(cell, e, a, b);
                    wall_vertex[a] = wall_vertex[b] = 1;
                }
        int scored = 0;
        for (int i = 0; i < r.mesh.n_cells(); ++i) {
            auto g = ExplicitInertialSolverTestAccess::gradient(r.solver, i);
            bool interior_ring = true;
            for (int k = 0; k < 3; ++k)
                interior_ring &= !wall_vertex[r.mesh.cell_vertex(i, k)];
            for (int f = 0; f < 4; ++f) {
                if (f >= 2 && !interior_ring)
                    continue;
                const double gx = co * logical[f][0] - si * logical[f][1],
                             gy = si * logical[f][0] + co * logical[f][1];
                if (!admissible(r.mesh, i, gx, gy))
                    continue;
                ++scored;
                EXPECT_NEAR(g[2 * f], gx, 2e-11) << "cell=" << i << " field=" << f;
                EXPECT_NEAR(g[2 * f + 1], gy, 2e-11) << "cell=" << i << " field=" << f;
            }
        }
        EXPECT_GT(scored, 100);
    }
}
TEST(SweReconstruction, TangentialLinearSurfaceAtReflectingWall) {
    for (double angle : {0., 64. * std::acos(-1.) / 180}) {
        Rig r(angle, 2);
        ExplicitInertialSolverTestAccess::gradients(r.solver);
        const double co = std::cos(angle), si = std::sin(angle);
        int scored = 0;
        for (int i = 0; i < r.mesh.n_cells(); ++i) {
            const double x = co * r.mesh.tri_cx[i] + si * r.mesh.tri_cy[i],
                         y = -si * r.mesh.tri_cx[i] + co * r.mesh.tri_cy[i];
            if (x < 1 || x > N - 1 || y > .6)
                continue;
            bool wall = false;
            for (int e = 0; e < 3; ++e)
                wall |= r.mesh.cell_neighbour(i, e) < 0;
            if (!wall)
                continue;
            ++scored;
            const auto g = ExplicitInertialSolverTestAccess::gradient(r.solver, i);
            EXPECT_NEAR(g[0], .02 * co, 2e-11);
            EXPECT_NEAR(g[1], .02 * si, 2e-11);
            EXPECT_NEAR(g[4], .01 * co * co, 2e-11);
            EXPECT_NEAR(g[5], .01 * co * si, 2e-11);
            EXPECT_NEAR(g[6], .01 * si * co, 2e-11);
            EXPECT_NEAR(g[7], .01 * si * si, 2e-11);
        }
        EXPECT_GE(scored, 4);
    }
}
TEST(SweReconstruction, WallPressureUsesBeginningEulerDepth) {
    auto m = irregular(0);
    SurfaceStateData state;
    state.resize(m.n_cells(), m.n_vertices());
    for (int i = 0; i < m.n_cells(); ++i) {
        state.head[i] = state.depth[i] = 1;
        state.volume[i] = m.tri_area[i];
    }
    const int target = 4;
    for (int e = 0; e < 3; ++e) {
        const int j = m.cell_neighbour(target, e);
        if (j >= 0) {
            state.head[j] = state.depth[j] = 2;
            state.volume[j] = 2 * m.tri_area[j];
        }
    }
    SolverOptions2D opts;
    opts.momentum = Momentum2D::FULL_SWE;
    opts.reconstruction_order = 1;
    opts.lts_tiers = 1;
    opts.num_threads = 1;
    ExplicitInertialSolver solver;
    solver.initialize(m, state, opts);
    const auto error = ExplicitInertialSolverTestAccess::wallEuler(solver, target, .005);
    ASSERT_GT(std::abs(error[2]), 1e-6) << "Fixture must change depth before wall pressure";
    EXPECT_NEAR(error[0], 0, 2e-12);
    EXPECT_NEAR(error[1], 0, 2e-12);
}

TEST(SweReconstruction, ReconstructedDepthStaysNonnegativeAtWetDryInterface) {
    Rig r(0, 0);
    for (int i = 0; i < r.mesh.n_cells(); ++i) {
        const double x = r.mesh.tri_cx[i];
        const double h = x < 2 ? 0 : x < 3 ? .0005 : x < 4 ? .02 : 2;
        r.state.head[i] = r.state.depth[i] = h;
        r.state.volume[i] = h * r.mesh.tri_area[i];
    }
    ExplicitInertialSolverTestAccess::gradients(r.solver);
    for (int i = 0; i < r.mesh.n_cells(); ++i) {
        const auto g = ExplicitInertialSolverTestAccess::gradient(r.solver, i);
        for (double value : g)
            ASSERT_TRUE(std::isfinite(value));
        for (int e = 0; e < 3; ++e) {
            const int slot = MeshData::slot(i, e);
            const double hf = r.state.depth[i] + g[2] * (r.mesh.edge_mx[slot] - r.mesh.tri_cx[i]) +
                              g[3] * (r.mesh.edge_my[slot] - r.mesh.tri_cy[i]);
            EXPECT_GE(hf, -2e-12);
        }
        if (r.state.depth[i] <= 10 * r.opts.dry_depth)
            for (double value : g)
                EXPECT_DOUBLE_EQ(value, 0);
    }
}
TEST(SweReconstruction, SlopingLakeRemainsStillOnIrregularRotatedTin) {
    for (double angle : {0., 64. * std::acos(-1.) / 180}) {
        auto m = irregular(angle);
        for (int v = 0; v < m.n_vertices(); ++v)
            m.vz[v] = .03 * m.vx[v] + .02 * m.vy[v];
        buildMeshTopology(m);
        SurfaceStateData state;
        state.resize(m.n_cells(), m.n_vertices());
        double initial = 0;
        for (int i = 0; i < m.n_cells(); ++i) {
            state.head[i] = 1;
            state.depth[i] = 1 - m.tri_cz[i];
            state.volume[i] = m.tri_area[i] * state.depth[i];
            initial += state.volume[i];
        }
        SolverOptions2D opts;
        opts.momentum = Momentum2D::FULL_SWE;
        opts.reconstruction_order = 2;
        opts.lts_tiers = 1;
        opts.max_timestep = .002;
        opts.num_threads = 1;
        ExplicitInertialSolver solver;
        solver.initialize(m, state, opts);
        ASSERT_NEAR(solver.advance(0, .1), .1, 1e-14);
        double final = 0;
        for (int i = 0; i < m.n_cells(); ++i) {
            EXPECT_NEAR(state.head[i], 1, 2e-11);
            final += state.volume[i];
        }
        EXPECT_NEAR(final, initial, 2e-11);
        EXPECT_LE(ExplicitInertialSolverTestAccess::maxMomentum(solver), 2e-11);
    }
}

TEST(SweReconstruction, WallPressureUsesReconstructedBeginningState) {
    for (double angle : {0., 64. * std::acos(-1.) / 180}) {
        Rig r(angle, 2);
        const double co = std::cos(angle), si = std::sin(angle);
        const auto error =
            ExplicitInertialSolverTestAccess::wallEuler(r.solver, 4, .005, {.02 * co, .02 * si});
        EXPECT_NEAR(error[0], 0, 2e-12);
        EXPECT_NEAR(error[1], 0, 2e-12);
    }
}

TEST(SweReconstruction, ThreadCountDoesNotChangeReconstruction) {
    const double angle = 64 * std::acos(-1.) / 180;
    Rig serial(angle, 1), parallel(angle, 1);
    parallel.opts.num_threads = 4;
    ExplicitInertialSolverTestAccess::gradients(serial.solver);
    ExplicitInertialSolverTestAccess::gradients(parallel.solver);
    for (int i = 0; i < serial.mesh.n_cells(); ++i)
        EXPECT_EQ(ExplicitInertialSolverTestAccess::gradient(serial.solver, i),
                  ExplicitInertialSolverTestAccess::gradient(parallel.solver, i));
}

TEST(SweReconstruction, UnrepresentablePressureIsRefusedBeforeAnyFaceBooking) {
    Rig r(0, 0);
    const int last = r.mesh.n_cells() - 1;
    r.state.head[last] = r.state.depth[last] = 1e200;
    r.state.volume[last] = 1e200 * r.mesh.tri_area[last];
    const auto water = r.state.volume;
    EXPECT_THROW(ExplicitInertialSolverTestAccess::facePass(r.solver), std::runtime_error);
    EXPECT_EQ(r.state.volume, water);
    EXPECT_TRUE(ExplicitInertialSolverTestAccess::zeroBookings(r.solver));
}

TEST(SweReconstruction, ClosedBankReleaseDoesNotGenerateRunawayEnergy) {
    auto m = irregular(0);
    for (int v = 0; v < m.n_vertices(); ++v) {
        m.vx[v] *= .1;
        m.vy[v] *= .1;
        m.vz[v] = .155 / .34 * std::max(.34 - m.vy[v], 0.);
    }
    buildMeshTopology(m);
    SurfaceStateData state;
    state.resize(m.n_cells(), m.n_vertices());
    for (int i = 0; i < m.n_cells(); ++i) {
        m.mannings_n[i] = .01;
        state.depth[i] = .4;
        state.head[i] = m.tri_cz[i] + .4;
        state.volume[i] = .4 * m.tri_area[i];
    }
    SolverOptions2D opts;
    opts.momentum = Momentum2D::FULL_SWE;
    opts.reconstruction_order = 2;
    opts.turbulence = Turbulence2D::SMAGORINSKY;
    opts.smagorinsky_cs = 0;
    opts.cfl_number = .4;
    opts.lts_tiers = 1;
    opts.max_timestep = .01;
    opts.num_threads = 1;
    ExplicitInertialSolver solver;
    solver.initialize(m, state, opts);
    const auto result = ExplicitInertialSolverTestAccess::boundedBankRelease(solver, .5);
    RecordProperty("reached_s", std::to_string(result[0]));
    RecordProperty("maximum_energy_ratio", std::to_string(result[1]));
    RecordProperty("maximum_speed_mps", std::to_string(result[2]));
    EXPECT_NEAR(result[0], .5, 1e-12) << "max speed=" << result[2];
    EXPECT_LE(result[1], 1.02) << "max speed=" << result[2];
}

TEST(SweReconstruction, CroppedTinBankCornerDoesNotGenerateRunawayEnergy) {
    std::ifstream file(std::filesystem::path(__FILE__).parent_path() /
                       "data/swe_reconstruction/bank_corner.mesh");
    int nv = 0, nt = 0;
    if (!(file >> nv >> nt))
        throw std::runtime_error("Missing TIN bank fixture");
    MeshData m;
    m.resize_vertices(nv);
    m.resize_triangles(nt);
    for (int v = 0; v < nv; ++v)
        file >> m.vx[v] >> m.vy[v] >> m.vz[v];
    for (int i = 0; i < nt; ++i) {
        int a, b, c;
        file >> a >> b >> c;
        m.set_triangle(i, a, b, c);
        m.mannings_n[i] = .01;
    }
    if (!file)
        throw std::runtime_error("Truncated TIN bank fixture");
    buildMeshTopology(m);
    SurfaceStateData state;
    state.resize(nt, nv);
    for (int i = 0; i < nt; ++i) {
        state.depth[i] = .4;
        state.head[i] = m.tri_cz[i] + .4;
        state.volume[i] = .4 * m.tri_area[i];
    }
    SolverOptions2D opts;
    opts.momentum = Momentum2D::FULL_SWE;
    opts.reconstruction_order = 2;
    opts.turbulence = Turbulence2D::SMAGORINSKY;
    opts.smagorinsky_cs = 0;
    opts.cfl_number = .4;
    opts.lts_tiers = 1;
    opts.max_timestep = .01;
    opts.num_threads = 1;
    ExplicitInertialSolver solver;
    solver.initialize(m, state, opts);
    const auto result = ExplicitInertialSolverTestAccess::boundedBankRelease(solver, .5);
    RecordProperty("reached_s", std::to_string(result[0]));
    RecordProperty("maximum_energy_ratio", std::to_string(result[1]));
    RecordProperty("maximum_speed_mps", std::to_string(result[2]));
    EXPECT_NEAR(result[0], .5, 1e-12) << "max speed=" << result[2];
    EXPECT_LE(result[1], 1.02) << "max speed=" << result[2];
}

TEST(SweReconstruction, ReconstructedVelocityHasNoVertexOvershoot) {
    Rig r(64 * std::acos(-1.) / 180, 0);
    for (int i = 0; i < r.mesh.n_cells(); ++i)
        ExplicitInertialSolverTestAccess::setVelocity(r.solver, i, std::sin(1.37 * i),
                                                      std::cos(2.41 * i));
    ExplicitInertialSolverTestAccess::gradients(r.solver);
    for (int i = 0; i < r.mesh.n_cells(); ++i) {
        const auto g = ExplicitInertialSolverTestAccess::gradient(r.solver, i);
        for (int k = 0; k < 3; ++k) {
            const int v = r.mesh.cell_vertex(i, k);
            const double ax = r.mesh.vx[v] - r.mesh.tri_cx[i], ay = r.mesh.vy[v] - r.mesh.tri_cy[i];
            EXPECT_LE(std::abs(std::sin(1.37 * i) + g[4] * ax + g[5] * ay), 1 + 2e-11);
            EXPECT_LE(std::abs(std::cos(2.41 * i) + g[6] * ax + g[7] * ay), 1 + 2e-11);
        }
    }
}

} // namespace
