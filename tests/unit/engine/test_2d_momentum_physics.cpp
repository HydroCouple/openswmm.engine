// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include <gtest/gtest.h>
#include "2d/solver/MomentumPhysics2D.hpp"
#include "2d/solver/ExplicitInertialSolver.hpp"
#include "2d/solver/SurfaceSolverFactory.hpp"
#include "2d/solver/InertialKernels.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include "2d/input/SectionHandlers2D.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include "2d/data/BoundaryData.hpp"
#include <numeric>

using namespace openswmm::twoD;
namespace openswmm::twoD {
struct ExplicitInertialSolverTestAccess {
    static double wallEnergy(const ExplicitInertialSolver& s) { return s.wall_drag_energy_; }
    static const std::vector<double>& x(const ExplicitInertialSolver& s) { return s.qcx_; }
    static const std::vector<double>& y(const ExplicitInertialSolver& s) { return s.qcy_; }
};
}
namespace {
MeshData grid(int n, bool skew = false, bool quads = false) {
    MeshData m;
    m.resize_vertices((n + 1) * (n + 1));
    for (int y = 0; y <= n; ++y)
        for (int x = 0; x <= n; ++x) {
            const int v = y * (n + 1) + x;
            m.vx[v] = double(x) / n + (skew ? .18 * double(y) / n : 0);
            m.vy[v] = double(y) / n +
                      (skew && x && y && x < n && y < n ? .025 * std::sin(7 * x + 3 * y) / n : 0);
            m.vz[v] = 0;
        }
    m.resize_triangles((quads ? 1 : 2) * n * n);
    int t = 0;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const int a = y * (n + 1) + x, b = a + 1, c = a + n + 2, d = a + n + 1;
            if (quads)
                m.set_quad(t++, a, b, c, d);
            else {
                m.set_triangle(t++, a, b, c);
                m.set_triangle(t++, a, c, d);
            }
        }
    std::fill(m.mannings_n.begin(), m.mannings_n.end(), 0);
    buildMeshTopology(m);
    return m;
}
struct Fields {
    MeshData m;
    InertialEdges edges;
    MomentumPhysics2D physics;
    std::vector<double> h, eta, x, y, rx, ry;
    Fields(int n = 8, bool skew = false, bool quads = false) : m(grid(n, skew, quads)) {
        edges.build(m);
        h.assign(m.n_cells(), 1);
        eta = h;
        x = h;
        y.assign(h.size(), 0);
    }
    double evaluate(double cs = .1, double fixed = -1) {
        return physics.evaluate(m, edges, h, eta, x, y, 1e-8, cs, rx, ry, fixed);
    }
};
}

TEST(MomentumPhysics, OptionsValidateAndRetainDisabledDefaults) {
    SolverOptions2D o;
    EXPECT_FALSE(o.momentumPhysics());
    EXPECT_DOUBLE_EQ(o.coriolisParameter(), 0);
    EXPECT_TRUE(parse2DOptionsLine({"CORIOLIS", "LATITUDE"}, o).empty());
    EXPECT_TRUE(parse2DOptionsLine({"CORIOLIS_LATITUDE", "90"}, o).empty());
    EXPECT_NEAR(o.coriolisParameter(), 1.4584231711061317e-4, 1e-18);
    EXPECT_TRUE(parse2DOptionsLine({"CORIOLIS_LATITUDE", "-90"}, o).empty());
    EXPECT_LT(o.coriolisParameter(), 0);
    EXPECT_FALSE(parse2DOptionsLine({"CORIOLIS_LATITUDE", "91"}, o).empty());
    for (const auto* val : {"nan", "inf", "-0.1"})
        EXPECT_FALSE(parse2DOptionsLine({"SMAGORINSKY_CS", val}, o).empty());
    EXPECT_TRUE(parse2DOptionsLine({"SMAGORINSKY_CS", "0"}, o).empty());
    EXPECT_TRUE(parse2DOptionsLine({"TURBULENCE_MODEL", "SMAGORINSKY"}, o).empty());
}

TEST(MomentumPhysics, ExactInertialRotationAndHemispheres) {
    for (double f : {-.001, 0., .001}) {
        std::vector<double> x{2}, y{3};
        for (int k = 0; k < 10000; ++k)
            MomentumPhysics2D::rotate(x, y, f * .3);
        EXPECT_NEAR(x[0], 2 * std::cos(f * 3000) + 3 * std::sin(f * 3000), 3e-12);
        EXPECT_NEAR(y[0], 3 * std::cos(f * 3000) - 2 * std::sin(f * 3000), 3e-12);
        EXPECT_NEAR(x[0] * x[0] + y[0] * y[0], 13, 5e-11);
    }
}

TEST(MomentumPhysics, LinearStrainIsExactOnSkewTrianglesAndQuads) {
    for (bool quads : {false, true}) {
        Fields f(8, true, quads);
        for (int i = 0; i < f.m.n_cells(); ++i) {
            f.x[i] = 2 * f.m.tri_cx[i] + 3 * f.m.tri_cy[i];
            f.y[i] = 4 * f.m.tri_cx[i] - f.m.tri_cy[i];
        }
        f.evaluate();
        for (int i = 0; i < f.m.n_cells(); ++i)
            if (f.m.tri_cx[i] > .25 && f.m.tri_cx[i] < .75 && f.m.tri_cy[i] > .25 &&
                f.m.tri_cy[i] < .75) {
                EXPECT_NEAR(f.physics.strain[i], std::sqrt(59.), 1e-12);
                EXPECT_NEAR(f.physics.viscosity[i], .01 * f.m.tri_area[i] * std::sqrt(59.), 1e-14);
            }
    }
}

TEST(MomentumPhysics, RigidRotationAndTranslationHaveNoMixingStress) {
    Fields f(8, true);
    for (int i = 0; i < f.m.n_cells(); ++i) {
        f.x[i] = 2 - 3 * f.m.tri_cy[i];
        f.y[i] = 1 + 3 * f.m.tri_cx[i];
    }
    f.evaluate();
    for (std::size_t i = 0; i < f.x.size(); ++i) {
        EXPECT_NEAR(f.physics.strain[i], 0, 1e-12);
        EXPECT_NEAR(f.rx[i], 0, 1e-23);
        EXPECT_NEAR(f.ry[i], 0, 1e-23);
    }
}

TEST(MomentumPhysics, SkewMeshStressConservesMomentumAndDissipatesEnergy) {
    Fields f(9, true);
    double px = 0, py = 0, power = 0;
    for (int i = 0; i < f.m.n_cells(); ++i) {
        f.h[i] = .3 + f.m.tri_cx[i];
        f.eta[i] = f.h[i];
        f.x[i] = f.h[i] * std::sin(19 * f.m.tri_cy[i]);
        f.y[i] = f.h[i] * std::cos(13 * f.m.tri_cx[i]);
    }
    EXPECT_GT(f.evaluate(), 0);
    for (int i = 0; i < f.m.n_cells(); ++i) {
        const double a = f.m.tri_area[i];
        px += a * f.rx[i];
        py += a * f.ry[i];
        power += a * (f.x[i] * f.rx[i] + f.y[i] * f.ry[i]) / f.h[i];
    }
    EXPECT_NEAR(px, 0, 1e-13);
    EXPECT_NEAR(py, 0, 1e-13);
    EXPECT_LT(power, 0);
    double initial_x = 0, initial_y = 0;
    for (int i = 0; i < f.m.n_cells(); ++i) {
        initial_x += f.m.tri_area[i] * f.x[i];
        initial_y += f.m.tri_area[i] * f.y[i];
    }
    const double e0 = MomentumPhysics2D::energy(f.m, f.h, f.x, f.y, 1e-8);
    f.physics.advance(f.m, f.edges, f.h, f.eta, f.x, f.y, 1e-8, .2, .4);
    EXPECT_LT(MomentumPhysics2D::energy(f.m, f.h, f.x, f.y, 1e-8), e0);
    double nx = 0, ny = 0;
    for (int i = 0; i < f.m.n_cells(); ++i) {
        nx += f.m.tri_area[i] * f.x[i];
        ny += f.m.tri_area[i] * f.y[i];
    }
    EXPECT_NEAR(nx, initial_x, 1e-13);
    EXPECT_NEAR(ny, initial_y, 1e-13);
}

TEST(MomentumPhysics, DrySillsAndBlockedFacesDoNotTransferMomentum) {
    Fields f;
    for (int i = 0; i < f.m.n_cells(); ++i) {
        f.x[i] = std::sin(i);
        f.y[i] = std::cos(i);
    }
    std::fill(f.edges.conveyance.begin(), f.edges.conveyance.end(), 0);
    f.evaluate();
    for (double x : f.rx)
        EXPECT_DOUBLE_EQ(x, 0);
    for (double y : f.ry)
        EXPECT_DOUBLE_EQ(y, 0);
    std::fill(f.edges.conveyance.begin(), f.edges.conveyance.end(), 1);
    f.h[20] = 0;
    f.eta[20] = 0;
    f.x[20] = f.y[20] = 0;
    f.evaluate();
    EXPECT_DOUBLE_EQ(f.rx[20], 0);
    EXPECT_DOUBLE_EQ(f.ry[20], 0);
}

TEST(MomentumPhysics, ManufacturedConstantViscosityInteriorConverges) {
    double previous = 0;
    for (int n : {16, 32, 64}) {
        Fields f(n, false, true);
        for (int i = 0; i < f.m.n_cells(); ++i)
            f.x[i] = std::sin(2 * f.m.tri_cy[i]);
        f.evaluate(.1, .03);
        double error = 0;
        int count = 0;
        for (int i = 0; i < f.m.n_cells(); ++i)
            if (f.m.tri_cx[i] > .2 && f.m.tri_cx[i] < .8 && f.m.tri_cy[i] > .2 &&
                f.m.tri_cy[i] < .8) {
                error += std::pow(f.rx[i] + .12 * std::sin(2 * f.m.tri_cy[i]), 2);
                ++count;
            }
        error = std::sqrt(error / count);
        if (previous)
            EXPECT_LT(error, previous * .3);
        previous = error;
    }
}

TEST(MomentumPhysics, SolverRejectsUnsupportedConfigurationsAndMissingLatitude) {
    auto m = grid(4);
    SurfaceStateData state;
    state.resize(m.n_cells(), m.n_vertices());
    SolverOptions2D o;
    o.coriolis = Coriolis2D::LATITUDE;
    ExplicitInertialSolver s;
    EXPECT_THROW(s.initialize(m, state, o), std::invalid_argument);
    EXPECT_THROW(makeSurfaceSolver(o, nullptr, m.n_cells(), true), std::invalid_argument);
    o.momentum = Momentum2D::FULL_SWE;
    o.lts_tiers = 1;
    EXPECT_THROW(s.initialize(m, state, o), std::invalid_argument);
    o.coriolis_latitude_set = true;
    o.coriolis_latitude = 40;
    EXPECT_NO_THROW(s.initialize(m, state, o));
}

TEST(MomentumPhysics, FullSolverPreservesLakeAndVolumeWithBothClosuresAndOrders) {
    for (auto closure : {CellClosure2D::FLAT, CellClosure2D::VFR})
        for (int order : {1, 2}) {
            auto m = grid(5, true);
            SurfaceStateData state;
            state.resize(m.n_cells(), m.n_vertices());
            for (int i = 0; i < m.n_cells(); ++i) {
                state.depth[i] = state.head[i] = .5;
                state.volume[i] = m.tri_area[i] * .5;
            }
            SolverOptions2D o;
            o.momentum = Momentum2D::FULL_SWE;
            o.lts_tiers = 1;
            o.cfl_number = .4;
            o.cell_closure = closure;
            o.reconstruction_order = order;
            o.turbulence = Turbulence2D::SMAGORINSKY;
            o.coriolis = Coriolis2D::CONSTANT_F;
            o.coriolis_f = .001;
            o.coriolis_f_set = true;
            ExplicitInertialSolver s;
            s.initialize(m, state, o);
            s.advance(0, .1);
            EXPECT_NEAR(std::accumulate(state.volume.begin(), state.volume.end(), 0.), .5, 1e-13);
            for (double q : ExplicitInertialSolverTestAccess::x(s))
                EXPECT_NEAR(q, 0, 1e-13);
            for (double q : ExplicitInertialSolverTestAccess::y(s))
                EXPECT_NEAR(q, 0, 1e-13);
        }
}

TEST(MomentumPhysics, ManufacturedNonlinearStressConvergesOnTriangles) {
    double previous = 0;
    for (int n : {16, 32, 64}) {
        Fields f(n);
        for (int i = 0; i < f.m.n_cells(); ++i)
            f.x[i] = std::pow(f.m.tri_cy[i], 2);
        f.evaluate();
        double error = 0;
        int count = 0;
        for (int i = 0; i < f.m.n_cells(); ++i)
            if (f.m.tri_cx[i] > .2 && f.m.tri_cx[i] < .8 && f.m.tri_cy[i] > .2 &&
                f.m.tri_cy[i] < .8) {
                const double exact = 8 * .01 * f.m.tri_area[i] * f.m.tri_cy[i];
                error += std::pow((f.rx[i] - exact) / f.m.tri_area[i], 2);
                ++count;
            }
        error = std::sqrt(error / count);
        if (previous)
            EXPECT_LT(error, previous * .65);
        previous = error;
    }
}

TEST(MomentumPhysics, FullSolverMatchesAnalyticalInertialOscillation) {
    for (int order : {1, 2}) {
        auto m = grid(4);
        SurfaceStateData state;
        state.resize(m.n_cells(), m.n_vertices());
        BoundaryData boundary;
        boundary.resize(m.n_edge_slots());
        for (int i = 0; i < m.n_cells(); ++i) {
            state.depth[i] = state.head[i] = .5;
            state.volume[i] = .5 * m.tri_area[i];
            m.tri_init_u[i] = .2;
            m.tri_init_v[i] = .3;
            for (int k = 0; k < m.cell_vertex_count(i); ++k)
                if (m.cell_neighbour(i, k) < 0) {
                    const int slot = MeshData::slot(i, k);
                    boundary.edge_bc_type[slot] = int8_t(BoundaryType::SPECIFIED_STAGE);
                    boundary.edge_bc_head[slot] = .5;
                }
        }
        state.boundary = &boundary;
        SolverOptions2D o;
        o.momentum = Momentum2D::FULL_SWE;
        o.lts_tiers = 1;
        o.cfl_number = .4;
        o.reconstruction_order = order;
        o.coriolis = Coriolis2D::CONSTANT_F;
        o.coriolis_f = .1;
        o.coriolis_f_set = true;
        o.turbulence = Turbulence2D::SMAGORINSKY;
        ExplicitInertialSolver s;
        s.initialize(m, state, o);
        s.advance(0, 2);
        const double u = .2 * std::cos(.2) + .3 * std::sin(.2),
                     v = .3 * std::cos(.2) - .2 * std::sin(.2);
        for (int i = 0; i < m.n_cells(); ++i) {
            EXPECT_NEAR(state.depth[i], .5, 2e-11);
            EXPECT_NEAR(ExplicitInertialSolverTestAccess::x(s)[i] / state.depth[i], u, 2e-10);
            EXPECT_NEAR(ExplicitInertialSolverTestAccess::y(s)[i] / state.depth[i], v, 2e-10);
        }
    }
}

TEST(MomentumPhysics, ExplicitlyDisabledOptionsAreBitIdentical) {
    auto m = grid(7, true);
    SurfaceStateData a;
    a.resize(m.n_cells(), m.n_vertices());
    for (int i = 0; i < m.n_cells(); ++i) {
        a.depth[i] = a.head[i] = m.tri_cx[i] < .5 ? .4 : .02;
        a.volume[i] = a.depth[i] * m.tri_area[i];
    }
    auto b = a;
    SolverOptions2D o;
    o.momentum = Momentum2D::FULL_SWE;
    o.lts_tiers = 1;
    o.reconstruction_order = 2;
    o.cfl_number = .4;
    auto p = o;
    ASSERT_TRUE(parse2DOptionsLine({"CORIOLIS", "OFF"}, p).empty());
    ASSERT_TRUE(parse2DOptionsLine({"TURBULENCE_MODEL", "NONE"}, p).empty());
    ASSERT_TRUE(parse2DOptionsLine({"SMAGORINSKY_CS", "0.2"}, p).empty());
    ExplicitInertialSolver x, y;
    x.initialize(m, a, o);
    y.initialize(m, b, p);
    x.advance(0, .3);
    y.advance(0, .3);
    EXPECT_EQ(a.volume, b.volume);
    EXPECT_EQ(ExplicitInertialSolverTestAccess::x(x), ExplicitInertialSolverTestAccess::x(y));
    EXPECT_EQ(ExplicitInertialSolverTestAccess::y(x), ExplicitInertialSolverTestAccess::y(y));
}

TEST(MomentumPhysics, GeostrophicStateConvergesWithMeshRefinement) {
    double previous = 0;
    for (int n : {8, 16, 32}) {
        auto m = grid(n);
        for (auto& x : m.vx)
            x *= 1000;
        for (auto& y : m.vy)
            y *= 1000;
        buildMeshTopology(m);
        SurfaceStateData state;
        state.resize(m.n_cells(), m.n_vertices());
        BoundaryData boundary;
        boundary.resize(m.n_edge_slots());
        constexpr double f = .1, v = .1, g = 9.81;
        for (int i = 0; i < m.n_cells(); ++i) {
            const double h = 1 + f * v / g * m.tri_cx[i];
            state.depth[i] = state.head[i] = h;
            state.volume[i] = h * m.tri_area[i];
            m.tri_init_v[i] = v;
            for (int k = 0; k < 3; ++k)
                if (m.cell_neighbour(i, k) < 0) {
                    const int slot = MeshData::slot(i, k);
                    boundary.edge_bc_type[slot] = int8_t(BoundaryType::SPECIFIED_STAGE);
                    boundary.edge_bc_head[slot] = h;
                }
        }
        state.boundary = &boundary;
        SolverOptions2D o;
        o.momentum = Momentum2D::FULL_SWE;
        o.lts_tiers = 1;
        o.cfl_number = .4;
        o.reconstruction_order = 2;
        o.max_timestep = .02;
        o.coriolis = Coriolis2D::CONSTANT_F;
        o.coriolis_f = f;
        o.coriolis_f_set = true;
        ExplicitInertialSolver s;
        s.initialize(m, state, o);
        s.advance(0, .2);
        double error = 0;
        int count = 0;
        for (int i = 0; i < m.n_cells(); ++i)
            if (m.tri_cx[i] > 250 && m.tri_cx[i] < 750 && m.tri_cy[i] > 250 && m.tri_cy[i] < 750) {
                const double u = ExplicitInertialSolverTestAccess::x(s)[i] / state.depth[i];
                const double dv = ExplicitInertialSolverTestAccess::y(s)[i] / state.depth[i] - v;
                error += u * u + dv * dv;
                ++count;
            }
        error = std::sqrt(error / count);
        if (previous)
            EXPECT_LT(error, previous * .8);
        previous = error;
    }
}

TEST(MomentumPhysics, WetDrySlopingDamBreakConservesWithFlatAndVfrStorage) {
    for (auto closure : {CellClosure2D::FLAT, CellClosure2D::VFR}) {
        auto m = grid(9, true);
        for (int v = 0; v < m.n_vertices(); ++v)
            m.vz[v] = .4 * m.vx[v] + .1 * m.vy[v];
        buildMeshTopology(m);
        SolverOptions2D o;
        o.momentum = Momentum2D::FULL_SWE;
        o.lts_tiers = 1;
        o.cfl_number = .4;
        o.reconstruction_order = 2;
        o.cell_closure = closure;
        o.dry_depth = 1e-6;
        o.h_move = 1e-6;
        o.turbulence = Turbulence2D::SMAGORINSKY;
        o.coriolis = Coriolis2D::CONSTANT_F;
        o.coriolis_f_set = true;
        o.coriolis_f = .01;
        SurfaceStateData state;
        state.resize(m.n_cells(), m.n_vertices());
        for (int i = 0; i < m.n_cells(); ++i) {
            const double stage = m.tri_cx[i] < .4 ? .38 : .12;
            state.volume[i] = inertial::cellVolumeFromEta(m, o, i, stage);
            inertial::cellEtaDepth(m, o, i, state.volume[i], state.head[i], state.depth[i]);
        }
        const double volume = std::accumulate(state.volume.begin(), state.volume.end(), 0.);
        ExplicitInertialSolver solver;
        solver.initialize(m, state, o);
        solver.advance(0, .2);
        EXPECT_NEAR(std::accumulate(state.volume.begin(), state.volume.end(), 0.), volume, 1e-12);
        for (double h : state.depth)
            EXPECT_GE(h, 0);
        for (double q : ExplicitInertialSolverTestAccess::x(solver))
            EXPECT_TRUE(std::isfinite(q));
        for (double q : ExplicitInertialSolverTestAccess::y(solver))
            EXPECT_TRUE(std::isfinite(q));
    }
}

TEST(MomentumPhysics, WallDragExactTangentialDecayAndDryZeroLimits) {
    Fields f(1, false, true);
    BoundaryData b; b.resize(f.m.n_edge_slots());
    const int slot = MeshData::slot(0, 0);
    b.edge_wall_drag[slot] = .02;
    for (double sign : {-1., 1.}) {
        const double nx=f.m.edge_nx[slot], ny=f.m.edge_ny[slot];
        f.x[0] = -ny * sign * 2 + nx * .3; f.y[0] = nx * sign * 2 + ny * .3;
        const double e0 = MomentumPhysics2D::energy(f.m, f.h, f.x, f.y, 1e-8);
        const double removed = MomentumPhysics2D::wallDrag(f.m,b,{slot},f.h,f.eta,f.x,f.y,
                                                          1e-8, 3, false);
        EXPECT_NEAR(-f.x[0]*ny+f.y[0]*nx, sign * 2 / (1 + .02 * 2 * 3), 1e-14);
        EXPECT_NEAR(f.x[0]*nx+f.y[0]*ny, .3, 1e-14);
        EXPECT_NEAR(removed, e0-MomentumPhysics2D::energy(f.m,f.h,f.x,f.y,1e-8),1e-14);
    }
    b.edge_wall_drag[slot] = 0;
    const auto x = f.x, y = f.y;
    EXPECT_DOUBLE_EQ(MomentumPhysics2D::wallDrag(f.m,b,{slot},f.h,f.eta,f.x,f.y,1e-8,1e6,false),0);
    EXPECT_EQ(f.x,x); EXPECT_EQ(f.y,y);
    b.edge_wall_drag[slot] = 1e6; f.h[0] = 0;
    EXPECT_DOUBLE_EQ(MomentumPhysics2D::wallDrag(f.m,b,{slot},f.h,f.eta,f.x,f.y,1e-8,1e6,false),0);
    EXPECT_EQ(f.x,x); EXPECT_EQ(f.y,y);
}

TEST(MomentumPhysics, WallDragUsesPartiallyWettedSlopingEdgeArea) {
    Fields f(1, false, true);
    BoundaryData b; b.resize(f.m.n_edge_slots());
    const int slot = MeshData::slot(0,0);
    int a,z; f.m.cell_edge_vertices(0,0,a,z);
    f.m.vz[a] = 0; f.m.vz[z] = 1;
    const double nx=f.m.edge_nx[slot], ny=f.m.edge_ny[slot];
    f.h[0] = .1; f.eta[0] = .5; f.x[0] = -.2*ny; f.y[0] = .2*nx;
    b.edge_wall_drag[slot] = .02;
    // Edge-averaged wet height = .5^2/(2*1) = .125 m.
    MomentumPhysics2D::wallDrag(f.m,b,{slot},f.h,f.eta,f.x,f.y,1e-8,3,true);
    EXPECT_NEAR(-f.x[0]*ny+f.y[0]*nx, .2/(1+.02*.125/.1*2*3), 1e-14);
    f.eta[0] = -.1; const auto x=f.x;
    EXPECT_DOUBLE_EQ(MomentumPhysics2D::wallDrag(f.m,b,{slot},f.h,f.eta,f.x,f.y,1e-8,3,true),0);
    EXPECT_EQ(f.x,x);
}

TEST(MomentumPhysics, WallDragCornerCompositionOnlyRemovesEnergy) {
    Fields f(1, true, true);
    BoundaryData b; b.resize(f.m.n_edge_slots());
    std::vector<int> slots;
    for (int e=0;e<4;++e) { slots.push_back(MeshData::slot(0,e)); b.edge_wall_drag[slots.back()]=.02; }
    f.x[0]=3; f.y[0]=-2;
    const double before=MomentumPhysics2D::energy(f.m,f.h,f.x,f.y,1e-8);
    const double removed=MomentumPhysics2D::wallDrag(f.m,b,slots,f.h,f.eta,f.x,f.y,1e-8,1e6,false);
    const double after=MomentumPhysics2D::energy(f.m,f.h,f.x,f.y,1e-8);
    EXPECT_GE(removed,0); EXPECT_LE(after,before);
    EXPECT_NEAR(removed,before-after,1e-13);
}

TEST(MomentumPhysics, WallDragParserAndUnsupportedSolverContracts) {
    std::vector<SurfaceRouter2D::PendingBoundaryRow> rows;
    for (const char* cw : {"nan","inf","-.1","*"})
        EXPECT_FALSE(parse2DBoundaryConditionsLine({"0","0","WALL_DRAG",cw},rows).empty());
    EXPECT_TRUE(parse2DBoundaryConditionsLine({"0","0","WALL_DRAG",".02","*","THROAT"},rows).empty());
    ASSERT_EQ(rows.size(),1u); EXPECT_EQ(rows[0].bc_type,0); EXPECT_DOUBLE_EQ(rows[0].param1,.02);
    EXPECT_EQ(rows[0].group,"THROAT");
    EXPECT_TRUE(parse2DBoundaryConditionsLine({"0","1","WALL","123"},rows).empty());
    EXPECT_DOUBLE_EQ(rows.back().param1,0); EXPECT_TRUE(rows.back().name.empty());
    SolverOptions2D o; o.wall_drag_enabled=true;
    EXPECT_THROW(makeSurfaceSolver(o),std::invalid_argument);
    o.momentum=Momentum2D::FULL_SWE; o.lts_tiers=1;
    EXPECT_NO_THROW(makeSurfaceSolver(o));
}


TEST(MomentumPhysics, FullSolverWallAndSmagorinskyKeepClosedMassAndRemoveEnergy) {
    for (bool quads : {false,true})
        for (auto closure : {CellClosure2D::FLAT,CellClosure2D::VFR})
            for (int order : {1,2}) {
                auto m=grid(5,false,quads);
                SurfaceStateData state; state.resize(m.n_cells(),m.n_vertices());
                BoundaryData b; b.resize(m.n_edge_slots());
                for(int i=0;i<m.n_cells();++i) {
                    state.depth[i]=state.head[i]=.5;
                    state.volume[i]=.5*m.tri_area[i];
                    m.tri_init_u[i]=.2*std::sin(3.141592653589793*m.tri_cy[i]);
                    m.tri_init_v[i]=.1*std::sin(3.141592653589793*m.tri_cx[i]);
                    for(int e=0;e<m.cell_vertex_count(i);++e)
                        if(m.cell_neighbour(i,e)<0) b.edge_wall_drag[MeshData::slot(i,e)]=.02;
                }
                state.boundary=&b;
                SolverOptions2D o; o.momentum=Momentum2D::FULL_SWE; o.lts_tiers=1;
                o.reconstruction_order=order; o.cell_closure=closure;
                o.cfl_number=.4; o.turbulence=Turbulence2D::SMAGORINSKY;
                ExplicitInertialSolver solver; solver.initialize(m,state,o);
                const auto total=[&] {
                    double energy=MomentumPhysics2D::energy(m,state.depth,
                        ExplicitInertialSolverTestAccess::x(solver),
                        ExplicitInertialSolverTestAccess::y(solver),o.dry_depth);
                    for(int i=0;i<m.n_cells();++i) energy+=.5*9.81*m.tri_area[i]*state.depth[i]*state.depth[i];
                    return energy;
                };
                const double before=total();
                solver.advance(0,.1);
                EXPECT_NEAR(std::accumulate(state.volume.begin(),state.volume.end(),0.),.5,1e-13);
                EXPECT_LT(total(),before+1e-8);
                EXPECT_GT(ExplicitInertialSolverTestAccess::wallEnergy(solver),0);
            }
}
