// SPDX-License-Identifier: Apache-2.0
// Regression gates for issue #195: local-inertial edge transmissivity (Option B).
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_2d.h>
#include "2d/data/MeshData.hpp"
#include "2d/data/BoundaryData.hpp"
#include "2d/data/SolverOptions2D.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include "2d/solver/ExplicitInertialSolver.hpp"
#include "core/SWMMEngine.hpp"

using namespace openswmm::twoD;

namespace openswmm::twoD {
struct ExplicitInertialSolverTestAccess {
    static void refresh(ExplicitInertialSolver& s) { s.refreshConveyance(); }
    static const auto& discharge(const ExplicitInertialSolver& s) { return s.q_; }
    static const auto& qx(const ExplicitInertialSolver& s) { return s.qcx_; }
    static const auto& qy(const ExplicitInertialSolver& s) { return s.qcy_; }
};
}

namespace {
MeshData channel(int shape = 0) {
    MeshData m;
    m.resize_vertices(15);
    for (int y = 0; y <= 2; ++y)
        for (int x = 0; x <= 4; ++x) {
            const int v = y * 5 + x;
            m.vx[v] = x;
            m.vy[v] = y;
        }
    int count = 0;
    for (int p = 0; p < 8; ++p)
        count += shape == 1 || (shape == 2 && p % 2) ? 1 : 2;
    m.resize_triangles(count);
    int c = 0;
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 4; ++x) {
            const int a = y * 5 + x, b = a + 1, d = a + 5, e = d + 1;
            if (shape == 1 || (shape == 2 && (y * 4 + x) % 2))
                m.set_quad(c++, a, b, e, d);
            else {
                m.set_triangle(c++, a, b, e);
                m.set_triangle(c++, a, e, d);
            }
        }
    buildMeshTopology(m);
    return m;
}

void barrier(MeshData& m, double psi) {
    for (int c = 0; c < m.n_cells(); ++c)
        for (int k = 0; k < m.cell_vertex_count(c); ++k) {
            const int n = m.cell_neighbour(c, k);
            if (n >= 0 && (m.tri_cx[c] < 2) != (m.tri_cx[n] < 2))
                m.edge_conveyance[MeshData::slot(c, k)] = psi;
        }
}

SurfaceStateData water(const MeshData& m, double right = 0) {
    SurfaceStateData s;
    s.resize(m.n_cells(), m.n_vertices());
    for (int c = 0; c < m.n_cells(); ++c)
        s.volume[c] = m.tri_area[c] * (m.tri_cx[c] < 2 ? .2 : right);
    return s;
}

double rightVolume(const MeshData& m, const SurfaceStateData& s) {
    double v = 0;
    for (int c = 0; c < m.n_cells(); ++c)
        if (m.tri_cx[c] > 2)
            v += s.volume[c];
    return v;
}

double barrierFlux(const ExplicitInertialSolver& solver, const SurfaceStateData& s) {
    const auto& ed = solver.inertialEdges();
    double f = 0;
    for (int e = 0; e < ed.ne; ++e)
        if (ed.mx[e] == 2)
            f -= s.edge_flux[ed.slotL[e]];
    return f;
}

SolverOptions2D options(int tiers = 1) {
    SolverOptions2D o;
    o.theta = 1;
    o.lts_tiers = tiers;
    o.max_timestep = .01;
    o.cfl_number = .4;
    o.num_threads = 1;
    return o;
}
}

TEST(EdgeConveyance, FactorsLeaveGeometricWidthsAndCflLengthsUnchanged) {
    for (int shape : {0, 1, 2}) {
        auto m = channel(shape);
        InertialEdges open, restricted;
        open.build(m);
        barrier(m, .3);
        restricted.build(m);
        EXPECT_EQ(open.xi, restricted.xi);
        EXPECT_EQ(open.cell_lchar, restricted.cell_lchar);
        EXPECT_EQ(open.cell_lpos, restricted.cell_lpos);
        int blocked = 0;
        for (int e = 0; e < restricted.ne; ++e) {
            EXPECT_EQ(open.conveyance[e], 1);
            EXPECT_EQ(restricted.conveyance[e], restricted.mx[e] == 2 ? .3 : 1);
            blocked += restricted.conveyance[e] < 1;
        }
        EXPECT_EQ(blocked, 2);
    }
}

TEST(EdgeConveyance, ClosedFullWidthBarrierKeepsDrySideDry) {
    for (int shape : {0, 1, 2})
        for (int tiers : {1, 4})
            for (auto face : {FaceDepth2D::MEAN, FaceDepth2D::VFR_FACE}) {
                auto m = channel(shape);
                barrier(m, 0);
                auto s = water(m);
                auto o = options(tiers);
                o.cell_closure = CellClosure2D::VFR;
                o.face_reconstruction = face;
                o.theta = .8;
                o.advection = true;
                for (int c = 0; c < m.n_cells(); ++c)
                    m.tri_init_u[c] = m.tri_cx[c] < 2 ? .1 : 0;
                ExplicitInertialSolver solver;
                solver.initialize(m, s, o);
                const double initial = std::accumulate(s.volume.begin(), s.volume.end(), 0.);
                double t = 0;
                for (double end : {.01, .17, .5}) {
                    solver.advance(t, end);
                    EXPECT_EQ(rightVolume(m, s), 0);
                    EXPECT_EQ(barrierFlux(solver, s), 0);
                    EXPECT_NEAR(std::accumulate(s.volume.begin(), s.volume.end(), 0.), initial,
                                1e-13);
                    for (double v : s.volume)
                        EXPECT_GE(v, 0);
                    t = end;
                }
            }
}

TEST(EdgeConveyance, FluxAndWaterTransferScaleOnceAndRankWithFactor) {
    for (auto closure : {CellClosure2D::FLAT, CellClosure2D::VFR}) {
        double full_flux = 0, full_volume = 0, previous_flux = 1e30, previous_volume = 1e30;
        std::vector<double> full_discharge;
        for (double psi : {1., .5, .3, 0.}) {
            auto m = channel();
            barrier(m, psi);
            auto s = water(m);
            auto o = options();
            o.cell_closure = closure;
            ExplicitInertialSolver solver;
            solver.initialize(m, s, o);
            solver.advance(0, .001); // One uncapped step: the momentum update is independent of ψ.
            const double flux = barrierFlux(solver, s), volume = rightVolume(m, s);
            if (psi == 1) {
                full_flux = flux;
                full_volume = volume;
                full_discharge = ExplicitInertialSolverTestAccess::discharge(solver);
            }
            if (psi > 0)
                EXPECT_EQ(ExplicitInertialSolverTestAccess::discharge(solver), full_discharge);
            EXPECT_NEAR(flux, psi * full_flux, 1e-15);
            EXPECT_NEAR(volume, psi * full_volume, 1e-15);
            EXPECT_LT(flux, previous_flux);
            EXPECT_LT(volume, previous_volume);
            previous_flux = flux;
            previous_volume = volume;
        }
    }
}

TEST(EdgeConveyance, InitialAndUpdatedPerotVectorsUseTransmittedDischarge) {
    auto m = channel();
    std::fill(m.edge_conveyance.begin(), m.edge_conveyance.end(), .3);
    std::fill(m.tri_init_u.begin(), m.tri_init_u.end(), .1);
    auto s = water(m, .2);
    auto o = options();
    o.theta = .8;
    ExplicitInertialSolver solver;
    solver.initialize(m, s, o);
    auto check = [&] {
        const auto& ed = solver.inertialEdges();
        const auto& q = ExplicitInertialSolverTestAccess::discharge(solver);
        for (int c = 0; c < m.n_cells(); ++c) {
            double x = 0, y = 0;
            for (int p = ed.cell_ptr[c]; p < ed.cell_ptr[c + 1]; ++p) {
                const int e = ed.cell_edge[p];
                const double f = ed.cell_sign[p] * q[e] * ed.xi[e] * .3;
                x += f * ed.cell_arm_x[p];
                y += f * ed.cell_arm_y[p];
            }
            EXPECT_NEAR(ExplicitInertialSolverTestAccess::qx(solver)[c], x / m.tri_area[c], 1e-15);
            EXPECT_NEAR(ExplicitInertialSolverTestAccess::qy(solver)[c], y / m.tri_area[c], 1e-15);
        }
    };
    check();
    solver.advance(0, .001);
    check();
}

TEST(EdgeConveyance, RuntimeApiCloseAndResetAffectTheNextAdvance) {
    openswmm::SWMMEngine engine;
    auto& router = engine.surfaceRouter2D();
    router.mesh() = channel();
    router.options().backend = Backend2D::CPU;
    router.options().mesh_units_si = true;
    router.initialize(engine.context()); // Supplies a live mesh for the actual setter/reset API.
    ASSERT_TRUE(router.isActive());
    auto handle = reinterpret_cast<SWMM_Engine>(&engine);
    auto& m = router.mesh();
    auto s = water(m, .05);
    auto o = options();
    o.theta = .8;
    ExplicitInertialSolver solver;
    solver.initialize(m, s, o);
    solver.advance(0, .001);
    for (int c = 0; c < m.n_cells(); ++c)
        for (int k = 0; k < m.cell_vertex_count(c); ++k)
            ASSERT_EQ(swmm_2d_set_edge_conveyance(handle, c, k, 0), SWMM_OK);
    const auto closed = s.volume;
    solver.advance(.001, .002);
    EXPECT_EQ(s.volume, closed);
    for (double q : ExplicitInertialSolverTestAccess::discharge(solver))
        EXPECT_EQ(q, 0);
    for (double q : ExplicitInertialSolverTestAccess::qx(solver))
        EXPECT_EQ(q, 0);
    for (double f : s.edge_flux)
        EXPECT_EQ(f, 0);
    ASSERT_EQ(swmm_2d_reset_edge_conveyance(handle), SWMM_OK);
    solver.advance(.002, .003);
    EXPECT_NE(s.volume, closed);
}

TEST(EdgeConveyance, PositivityBudgetUsesTransmittedVolume) {
    MeshData m;
    m.resize_vertices(4);
    m.vx = {0, 1, 1, 0};
    m.vy = {0, 0, 1, 1};
    m.resize_triangles(2);
    m.set_triangle(0, 0, 1, 2);
    m.set_triangle(1, 0, 2, 3);
    buildMeshTopology(m);
    InertialEdges ed;
    ed.build(m);
    ASSERT_EQ(ed.ne, 1);
    std::fill(m.edge_conveyance.begin(), m.edge_conveyance.end(), .3);
    std::fill(m.mannings_n.begin(), m.mannings_n.end(), 0);
    std::fill(m.tri_init_u.begin(), m.tri_init_u.end(), 1000 * ed.nx[0]);
    std::fill(m.tri_init_v.begin(), m.tri_init_v.end(), 1000 * ed.ny[0]);
    SurfaceStateData s;
    s.resize(2, 4);
    s.volume = {.1, .025};
    const auto before = s.volume;
    auto o = options();
    o.froude_max = 1e6;
    ExplicitInertialSolver solver;
    solver.initialize(m, s, o);
    solver.advance(0, .001);
    const double budget = o.exchange_beta / 3 * before[0];
    EXPECT_NEAR(before[0] - s.volume[0], budget, 1e-15);
    EXPECT_NEAR(s.volume[1] - before[1], budget, 1e-15);
    const double q = ExplicitInertialSolverTestAccess::discharge(solver)[0];
    EXPECT_NEAR(q * ed.xi[0] * .3 * .001, budget, 1e-15);
}

TEST(EdgeConveyance, AdvectiveAndDispersiveSpeciesExchangeRespectsBarrier) {
    for (double dispersion : {0., 1.}) {
        double full_mass = 0;
        for (double psi : {1., .3, 0.}) {
            auto m = channel();
            barrier(m, psi);
            auto s = water(m, .05);
            s.transport.resize(1, m.n_cells(), 0);
            for (int c = 0; c < m.n_cells(); ++c)
                s.transport.cell_mass[c] = m.tri_cx[c] < 2 ? s.volume[c] : 0;
            const double total =
                std::accumulate(s.transport.cell_mass.begin(), s.transport.cell_mass.end(), 0.);
            auto o = options();
            o.dispersion = dispersion;
            ExplicitInertialSolver solver;
            solver.initialize(m, s, o);
            solver.advance(0, .001);
            double mass = 0;
            for (int c = 0; c < m.n_cells(); ++c)
                if (m.tri_cx[c] > 2)
                    mass += s.transport.cell_mass[c];
            if (psi == 1)
                full_mass = mass;
            EXPECT_GT(full_mass, 0);
            EXPECT_NEAR(mass, psi * full_mass, 1e-15);
            EXPECT_NEAR(
                std::accumulate(s.transport.cell_mass.begin(), s.transport.cell_mass.end(), 0.),
                total, 1e-14);
        }
    }
}

TEST(EdgeConveyance, RuntimeRefreshRetainsBoundaryDischargeContribution) {
    auto m = channel();
    BoundaryData bd;
    bd.resize(m.n_edge_slots());
    int inlet = -1;
    for (int c = 0; c < m.n_cells() && inlet < 0; ++c)
        for (int k = 0; k < m.cell_vertex_count(c); ++k) {
            const int slot = MeshData::slot(c, k);
            if (m.cell_neighbour(c, k) < 0 && m.edge_nx[slot] < -.9) {
                inlet = slot;
                break;
            }
        }
    ASSERT_GE(inlet, 0);
    bd.edge_bc_type[inlet] = int8_t(BoundaryType::SPECIFIED_FLOW);
    bd.edge_bc_flow[inlet] = -.01;
    auto s = water(m, .2);
    s.boundary = &bd;
    auto o = options();
    o.theta = .8;
    ExplicitInertialSolver solver;
    solver.initialize(m, s, o);
    solver.advance(0, .001);
    const auto qx = ExplicitInertialSolverTestAccess::qx(solver);
    const auto qy = ExplicitInertialSolverTestAccess::qy(solver);
    EXPECT_GT(*std::max_element(qx.begin(), qx.end()), 0);
    // The first face pass starts at rest: all current cell discharge is from the inlet.
    for (double q : ExplicitInertialSolverTestAccess::discharge(solver))
        EXPECT_EQ(q, 0);
    std::fill(m.edge_conveyance.begin(), m.edge_conveyance.end(), .3);
    ExplicitInertialSolverTestAccess::refresh(solver);
    EXPECT_EQ(ExplicitInertialSolverTestAccess::qx(solver), qx);
    EXPECT_EQ(ExplicitInertialSolverTestAccess::qy(solver), qy);
}
