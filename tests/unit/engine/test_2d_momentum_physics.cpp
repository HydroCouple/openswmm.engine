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
#include <algorithm>
#include <cmath>
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
MeshData grid(int n, bool skew = false, bool quads = false, double angle = 0) {
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
    // Rigid rotation of the whole square about the origin (radians).
    for (int v = 0; v < m.n_vertices() && angle != 0; ++v) {
        const double x = m.vx[v], y = m.vy[v];
        m.vx[v] = std::cos(angle) * x - std::sin(angle) * y;
        m.vy[v] = std::sin(angle) * x + std::cos(angle) * y;
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
    Fields(int n = 8, bool skew = false, bool quads = false, double angle = 0)
        : m(grid(n, skew, quads, angle)) {
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

TEST(MomentumPhysics, BackgroundViscosityParsesAndRoundTrips) {
    SolverOptions2D o;
    EXPECT_DOUBLE_EQ(o.eddy_viscosity_background, 0);
    EXPECT_TRUE(is2DOptionKey("EDDY_VISCOSITY_BACKGROUND"));
    const auto keys = current2DOptionKeys();
    EXPECT_NE(std::find_if(keys.begin(), keys.end(),
                           [](const char* k) { return std::string(k) == "EDDY_VISCOSITY_BACKGROUND"; }),
              keys.end());
    for (const auto* val : {"nan", "inf", "-0.001", "UNSET", "x"})
        EXPECT_FALSE(parse2DOptionsLine({"EDDY_VISCOSITY_BACKGROUND", val}, o).empty()) << val;
    EXPECT_DOUBLE_EQ(o.eddy_viscosity_background, 0);
    ASSERT_TRUE(parse2DOptionsLine({"EDDY_VISCOSITY_BACKGROUND", "0.0123456789012345"}, o).empty());
    SolverOptions2D back;
    ASSERT_TRUE(parse2DOptionsLine({"EDDY_VISCOSITY_BACKGROUND",
                                    format2DOptionValue(o, "EDDY_VISCOSITY_BACKGROUND")}, back).empty());
    EXPECT_EQ(back.eddy_viscosity_background, o.eddy_viscosity_background);
    EXPECT_EQ(format2DOptionValue(SolverOptions2D{}, "EDDY_VISCOSITY_BACKGROUND"), "0");
}

TEST(MomentumPhysics, BackgroundViscosityIsInactiveWithoutSmagorinsky) {
    // Like SMAGORINSKY_CS, a stored background value is inert unless the
    // turbulence model selects it: same convention as inactive Coriolis inputs.
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
    p.eddy_viscosity_background = .01;
    EXPECT_NO_THROW(makeSurfaceSolver(p, nullptr, m.n_cells(), true));
    ExplicitInertialSolver x, y;
    x.initialize(m, a, o);
    y.initialize(m, b, p);
    x.advance(0, .3);
    y.advance(0, .3);
    EXPECT_EQ(a.volume, b.volume);
    EXPECT_EQ(ExplicitInertialSolverTestAccess::x(x), ExplicitInertialSolverTestAccess::x(y));
    EXPECT_EQ(ExplicitInertialSolverTestAccess::y(x), ExplicitInertialSolverTestAccess::y(y));
    SurfaceStateData c;
    c.resize(m.n_cells(), m.n_vertices());
    auto bad = p;
    bad.eddy_viscosity_background = -1;
    EXPECT_THROW(ExplicitInertialSolver{}.initialize(m, c, bad), std::invalid_argument);
}

TEST(MomentumPhysics, BackgroundViscosityIsAdditiveAndZeroIsBitIdentical) {
    Fields f(12, true);
    for (int i = 0; i < f.m.n_cells(); ++i) {
        f.x[i] = std::sin(3 * f.m.tri_cy[i]) + f.m.tri_cx[i];
        f.y[i] = std::cos(2 * f.m.tri_cx[i]);
    }
    const double bound0 = f.evaluate(.1);
    const auto nu0 = f.physics.viscosity, rx0 = f.rx, ry0 = f.ry;
    // Explicit zero background must not change a single bit.
    EXPECT_EQ(f.physics.evaluate(f.m, f.edges, f.h, f.eta, f.x, f.y, 1e-8, .1, f.rx, f.ry, -1, 0), bound0);
    EXPECT_EQ(f.physics.viscosity, nu0);
    EXPECT_EQ(f.rx, rx0);
    EXPECT_EQ(f.ry, ry0);
    f.physics.evaluate(f.m, f.edges, f.h, f.eta, f.x, f.y, 1e-8, .1, f.rx, f.ry, -1, .02);
    for (std::size_t i = 0; i < nu0.size(); ++i)
        EXPECT_DOUBLE_EQ(f.physics.viscosity[i], .02 + nu0[i]);
}

TEST(MomentumPhysics, BackgroundViscosityDecaysCosineFlowAtAnalyticalRate) {
    // u = U cos(pi xi) along a rotated square's xi axis, v = 0, constant h:
    // zero traction on all four walls and du/dt = 2 nu u_xixi, so the
    // amplitude decays exactly as exp(-2 nu pi^2 t) under div(2 h nu S).
    const double nu = .01, T = 2.0, exact = -2 * nu * M_PI * M_PI;
    for (double angle : {0.0, 64.0 * M_PI / 180}) {
        double previous = 1;
        for (int n : {16, 32}) {
            Fields f(n, false, false, angle);
            const double ex = std::cos(angle), ey = std::sin(angle);
            std::vector<double> shape(f.m.n_cells());
            for (int i = 0; i < f.m.n_cells(); ++i) {
                const double xi = ex * f.m.tri_cx[i] + ey * f.m.tri_cy[i];
                shape[i] = std::cos(M_PI * xi);
                f.x[i] = shape[i] * ex;
                f.y[i] = shape[i] * ey;
            }
            const double e0 = MomentumPhysics2D::energy(f.m, f.h, f.x, f.y, 1e-8);
            f.physics.advance(f.m, f.edges, f.h, f.eta, f.x, f.y, 1e-8, 0, T, nu);
            double num = 0, den = 0, px = 0, py = 0;
            for (int i = 0; i < f.m.n_cells(); ++i) {
                num += f.m.tri_area[i] * (f.x[i] * ex + f.y[i] * ey) * shape[i];
                den += f.m.tri_area[i] * shape[i] * shape[i];
                px += f.m.tri_area[i] * f.x[i];
                py += f.m.tri_area[i] * f.y[i];
            }
            const double rate = std::log(num / den) / T;
            const double error = std::abs(rate / exact - 1);
            EXPECT_LT(error, n == 32 ? .02 : .06) << "n=" << n << " angle=" << angle;
            EXPECT_LT(error, previous) << "refinement must reduce the decay-rate error";
            previous = error;
            EXPECT_NEAR(px, 0, 1e-12);
            EXPECT_NEAR(py, 0, 1e-12);
            EXPECT_LT(MomentumPhysics2D::energy(f.m, f.h, f.x, f.y, 1e-8), e0);
        }
    }
}

TEST(MomentumPhysics, BackgroundViscosityActsWithZeroSmagorinskyCoefficient) {
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
    o.turbulence = Turbulence2D::SMAGORINSKY;
    o.smagorinsky_cs = 0;
    auto p = o;
    p.eddy_viscosity_background = .01;
    ExplicitInertialSolver x, y;
    x.initialize(m, a, o);
    y.initialize(m, b, p);
    x.advance(0, .3);
    y.advance(0, .3);
    const double va = std::accumulate(a.volume.begin(), a.volume.end(), 0.);
    const double vb = std::accumulate(b.volume.begin(), b.volume.end(), 0.);
    EXPECT_NEAR(va, vb, 1e-13);
    // Cs = 0 previously skipped mixing; the background term must still act.
    EXPECT_NE(ExplicitInertialSolverTestAccess::x(x), ExplicitInertialSolverTestAccess::x(y));
}

TEST(MixingLength, ParsesAndRoundTrips) {
    SolverOptions2D o;
    EXPECT_DOUBLE_EQ(o.mixing_length_coefficient, .5);
    ASSERT_TRUE(parse2DOptionsLine({"TURBULENCE_MODEL", "MIXING_LENGTH"}, o).empty());
    EXPECT_EQ(o.turbulence, Turbulence2D::MIXING_LENGTH);
    EXPECT_TRUE(o.momentumPhysics());
    EXPECT_EQ(format2DOptionValue(o, "TURBULENCE_MODEL"), "MIXING_LENGTH");
    EXPECT_TRUE(is2DOptionKey("MIXING_LENGTH_COEFFICIENT"));
    for (const auto* val : {"nan", "inf", "-0.1", "UNSET", "x"})
        EXPECT_FALSE(parse2DOptionsLine({"MIXING_LENGTH_COEFFICIENT", val}, o).empty()) << val;
    ASSERT_TRUE(parse2DOptionsLine({"MIXING_LENGTH_COEFFICIENT", "0.412345678901234"}, o).empty());
    SolverOptions2D back;
    ASSERT_TRUE(parse2DOptionsLine({"MIXING_LENGTH_COEFFICIENT",
                                    format2DOptionValue(o, "MIXING_LENGTH_COEFFICIENT")}, back).empty());
    EXPECT_EQ(back.mixing_length_coefficient, o.mixing_length_coefficient);
}

TEST(MixingLength, ViscosityIsWallCappedLengthSquaredTimesExactVorticity) {
    // Affine velocity has exact least-squares gradients; depth varies per cell.
    // On the unit square every exterior edge bounds the domain, so d = distance to the nearest side.
    Fields f(16);
    const double a = .3, b = -.7, c = .2, d = .5, omega = std::abs(c - b);
    for (int i = 0; i < f.m.n_cells(); ++i) {
        const double x = f.m.tri_cx[i], y = f.m.tri_cy[i];
        f.h[i] = f.eta[i] = .2 + .1 * x + .05 * y;
        f.x[i] = f.h[i] * (a * x + b * y);
        f.y[i] = f.h[i] * (c * x + d * y);
    }
    // Cs is ignored once the depth mixing length is selected.
    f.physics.evaluate(f.m, f.edges, f.h, f.eta, f.x, f.y, 1e-8, .3, f.rx, f.ry, -1, .001, .5);
    const auto nu = f.physics.viscosity;
    f.physics.evaluate(f.m, f.edges, f.h, f.eta, f.x, f.y, 1e-8, 0, f.rx, f.ry, -1, .001, .5);
    EXPECT_EQ(f.physics.viscosity, nu);
    int depth_limited = 0, wall_limited = 0;
    for (int i = 0; i < f.m.n_cells(); ++i) {
        const double x = f.m.tri_cx[i], y = f.m.tri_cy[i];
        const double wall = std::min({x, 1 - x, y, 1 - y});
        EXPECT_NEAR(f.physics.wall_distance[i], wall, 1e-12);
        if (x < .1 || x > .9 || y < .1 || y > .9) continue; // full interior stencils only
        const double l = std::min(.5 * f.h[i], MomentumPhysics2D::kappa * wall);
        EXPECT_NEAR(nu[i], .001 + l * l * omega, 1e-12);
        (l < .5 * f.h[i] ? wall_limited : depth_limited)++;
    }
    EXPECT_GT(depth_limited, 20);
    EXPECT_GT(wall_limited, 20);
}

TEST(MixingLength, IrrotationalStrainCarriesNoMixingLengthViscosity) {
    // A converging/accelerating potential flow (u = a x, v = -a y) has strain but no
    // vorticity; the shear-based mixing length must not mix it (Smagorinsky does).
    Fields f(12);
    for (int i = 0; i < f.m.n_cells(); ++i) {
        f.x[i] = .8 * f.m.tri_cx[i];
        f.y[i] = -.8 * f.m.tri_cy[i];
    }
    f.physics.evaluate(f.m, f.edges, f.h, f.eta, f.x, f.y, 1e-8, 0, f.rx, f.ry, -1, 0, .5);
    double smag = 0;
    for (int i = 0; i < f.m.n_cells(); ++i)
        if (f.m.tri_cx[i] > .1 && f.m.tri_cx[i] < .9 && f.m.tri_cy[i] > .1 && f.m.tri_cy[i] < .9)
            EXPECT_NEAR(f.physics.viscosity[i], 0, 1e-13);
    f.evaluate(.2);
    for (int i = 0; i < f.m.n_cells(); ++i) smag = std::max(smag, f.physics.viscosity[i]);
    EXPECT_GT(smag, 1e-4);
}

TEST(MixingLength, EddyViscosityConvergesUnderRefinementWhileSmagorinskyCollapses) {
    // The reason for the closure: its length scale is the flow depth, not the cell.
    auto interiorMedian = [](int n, double cs, double c) {
        Fields f(n);
        std::fill(f.h.begin(), f.h.end(), .3);
        f.eta = f.h;
        for (int i = 0; i < f.m.n_cells(); ++i)
            f.x[i] = .3 * std::sin(2 * f.m.tri_cy[i]);
        f.physics.evaluate(f.m, f.edges, f.h, f.eta, f.x, f.y, 1e-8, cs, f.rx, f.ry, -1, 0, c);
        std::vector<double> nu;
        for (int i = 0; i < f.m.n_cells(); ++i)
            // Core where the wall cap is inactive (d > c h / kappa = 0.37); the capped
            // branch is checked exactly in ViscosityIsWallCappedLengthSquaredTimesExactVorticity.
            if (f.m.tri_cx[i] > .4 && f.m.tri_cx[i] < .6 && f.m.tri_cy[i] > .4 && f.m.tri_cy[i] < .6)
                nu.push_back(f.physics.viscosity[i]);
        std::nth_element(nu.begin(), nu.begin() + nu.size() / 2, nu.end());
        return nu[nu.size() / 2];
    };
    const double ml32 = interiorMedian(32, 0, .5), ml64 = interiorMedian(64, 0, .5);
    EXPECT_NEAR(ml64 / ml32, 1, .02);
    EXPECT_LT(interiorMedian(64, .2, 0) / interiorMedian(16, .2, 0), .1);
}

TEST(MixingLength, FullSolverConservesActsAndIgnoresSmagorinskyCoefficient) {
    auto m = grid(7, true);
    SurfaceStateData a;
    a.resize(m.n_cells(), m.n_vertices());
    for (int i = 0; i < m.n_cells(); ++i) {
        a.depth[i] = a.head[i] = m.tri_cx[i] < .5 ? .4 : .02;
        a.volume[i] = a.depth[i] * m.tri_area[i];
    }
    auto b = a, c = a;
    SolverOptions2D o;
    o.momentum = Momentum2D::FULL_SWE;
    o.lts_tiers = 1;
    o.reconstruction_order = 2;
    o.cfl_number = .4;
    o.turbulence = Turbulence2D::SMAGORINSKY;
    o.smagorinsky_cs = 0;
    auto p = o, q = o;
    p.turbulence = q.turbulence = Turbulence2D::MIXING_LENGTH;
    p.smagorinsky_cs = .1;
    q.smagorinsky_cs = .3;
    ExplicitInertialSolver x, y, z;
    x.initialize(m, a, o);
    y.initialize(m, b, p);
    z.initialize(m, c, q);
    x.advance(0, .3);
    y.advance(0, .3);
    z.advance(0, .3);
    EXPECT_NEAR(std::accumulate(b.volume.begin(), b.volume.end(), 0.),
                std::accumulate(a.volume.begin(), a.volume.end(), 0.), 1e-13);
    EXPECT_EQ(ExplicitInertialSolverTestAccess::x(y), ExplicitInertialSolverTestAccess::x(z));
    EXPECT_NE(ExplicitInertialSolverTestAccess::x(x), ExplicitInertialSolverTestAccess::x(y));
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
