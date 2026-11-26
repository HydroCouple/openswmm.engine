// SPDX-License-Identifier: Apache-2.0

/**
 * @file      test_gw2d_parallel_sweeps.cpp
 * @brief     Track 4A (2026-11-26): the 2D subsurface solver's parallel cell sweep and ratio
 *            face loop reproduce the serial answers bit for bit, and the lateral Darcy scheme
 *            converges under mesh refinement.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>
#include "2d/mesh/MeshBuilder.hpp"
#include "2d/solver/InertialEdges.hpp"
#include "2d/subsurface/SubsurfaceSolver.hpp"
#include "2d/subsurface/SoilCharacteristic.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include "2d/data/SolverOptions2D.hpp"
#include "core/CompletedSourceArchive.hpp"
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numbers>
#include <string>
#include <vector>

using namespace openswmm::twoD;
namespace fs = std::filesystem;

namespace {
const fs::path kOutDir = fs::path(OPENSWMM_GW2D_SWEEP_TEST_OUT_DIR) / "gw2d_parallel_sweeps";

/**
 * @brief Build an nx by ny grid of squares, each split into two right triangles.
 *
 * With `orthogonal` the diagonal flips on a checkerboard, so every shared edge is perpendicular
 * to the segment joining its two cell centroids and the two-point flux across it is consistent.
 * Without it every diagonal runs the same way (the layout the surface tests' makeGridMesh
 * builds): the centroid chord across a vertical or horizontal edge is then skewed from the
 * edge normal.
 */
MeshData gridMesh(int nx, int ny, double dx, double z, bool orthogonal) {
    MeshData mesh;
    const int nvx = nx + 1, nvy = ny + 1;
    mesh.resize_vertices(nvx * nvy);
    for (int j = 0; j < nvy; ++j)
        for (int i = 0; i < nvx; ++i) {
            const int v = j * nvx + i;
            mesh.vx[v] = i * dx;
            mesh.vy[v] = j * dx;
            mesh.vz[v] = z;
        }
    mesh.resize_triangles(2 * nx * ny);
    int t = 0;
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            const int v00 = j * nvx + i, v10 = v00 + 1, v01 = v00 + nvx, v11 = v01 + 1;
            if (orthogonal && ((i + j) % 2 == 1)) {
                mesh.set_triangle(t++, v00, v10, v01);
                mesh.set_triangle(t++, v10, v11, v01);
            } else {
                mesh.set_triangle(t++, v00, v10, v11);
                mesh.set_triangle(t++, v00, v11, v01);
            }
        }
    for (int c = 0; c < mesh.n_triangles(); ++c)
        mesh.mannings_n[c] = .03;
    buildMeshTopology(mesh);
    return mesh;
}

std::string checkpointBytes(SubsurfaceSolver& gw) {
    openswmm::detail::CompletedSourceArchive archive;
    gw.checkpoint(archive, false);
    return archive.bytes();
}

void saveBytes(const std::string& name, const std::string& bytes) {
    fs::create_directories(kOutDir);
    std::ofstream(kOutDir / name, std::ios::binary)
        .write(bytes.data(), std::streamsize(bytes.size()));
}

/// Bitwise equality of two arrays, reporting the first mismatch only.
void expectSame(const char* what, const std::vector<double>& a, const std::vector<double>& b) {
    ASSERT_EQ(a.size(), b.size()) << what;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::memcmp(&a[i], &b[i], sizeof(double)) != 0) {
            ADD_FAILURE() << what << "[" << i << "] serial " << a[i] << " parallel " << b[i];
            return;
        }
}

void expectSameState(const SubsurfaceState& s, const SubsurfaceState& p, const SurfaceStateData& a,
                     const SurfaceStateData& b) {
    expectSame("hg", s.hg, p.hg);
    expectSame("hu", s.hu, p.hu);
    expectSame("theta_sigma", s.theta_sigma, p.theta_sigma);
    expectSame("eacc_L", s.eacc_L, p.eacc_L);
    expectSame("eacc_R", s.eacc_R, p.eacc_R);
    expectSame("xacc_from_surface", s.xacc_from_surface, p.xacc_from_surface);
    expectSame("xacc_to_surface", s.xacc_to_surface, p.xacc_to_surface);
    expectSame("nacc", s.nacc, p.nacc);
    expectSame("qlat_last", s.qlat_last, p.qlat_last);
    expectSame("q0_last", s.q0_last, p.q0_last);
    expectSame("dunne_cumulative", s.dunne_cumulative, p.dunne_cumulative);
    EXPECT_EQ(s.led_recharge, p.led_recharge);
    EXPECT_EQ(s.led_lateral, p.led_lateral);
    EXPECT_EQ(s.led_node, p.led_node);
    EXPECT_EQ(s.led_dunne, p.led_dunne);
    EXPECT_EQ(s.led_et, p.led_et);
    EXPECT_EQ(s.led_infil_in, p.led_infil_in);
    expectSame("surf.depth", a.depth, b.depth);
    expectSame("surf.volume", a.volume, b.volume);
}

/**
 * @brief A connected 32 x 16 grid (1024 cells, ~1500 interior faces) over a 4 m aquifer with
 * mixed closures and soil laws per cell, node beds on every fourth cell, and seeded surface and
 * node accumulators: enough work of every kind for both parallel paths to engage (they need
 * at least 512 cells or faces and more than one thread).
 */
struct SweepRig {
    static constexpr int kNx = 32, kNy = 16, kCells = 2 * kNx * kNy;
    MeshData mesh;
    InertialEdges edges;
    SurfaceStateData surf;
    SolverOptions2D opts;
    SubsurfaceConfig cfg;
    SubsurfaceSolver gw;

    explicit SweepRig(int threads, GwClosure closure, bool mixed) {
        mesh = gridMesh(kNx, kNy, 1., 10., true);
        edges.build(mesh);
        surf.resize(mesh.n_triangles(), mesh.n_vertices());
        opts.momentum = Momentum2D::LOCAL_INERTIAL;
        opts.num_threads = threads;
        cfg.options.authored = true;
        cfg.options.closure = closure;
        cfg.options.force_closed_form = true;
        cfg.options.gw_et = "NONE";
        GwAquiferRow row;
        row.Ks = 1.e-3;
        row.zs = 4;
        row.theta_s = .45;
        row.theta_r = .1;
        row.alpha = 2;
        row.hg0 = 2;
        cfg.rows.push_back(row);
        for (int c = 0; c < kCells; c += 4) {
            GwNodeBed bed;
            bed.node = c / 4;
            bed.cell = c;
            bed.area = .1;
            bed.Kc = 1e-5;
            bed.dC = .1;
            cfg.node_beds.push_back(bed);
        }
        std::vector<std::string> warnings;
        const auto error = gw.initialize(mesh, edges, opts, {}, kCells / 4, cfg, warnings);
        if (!error.empty())
            throw std::runtime_error(error);
        auto& s = gw.state();
        const SoilChar laws[] = {SoilChar::GARDNER, SoilChar::RUSSO, SoilChar::BROOKS_COREY,
                                 SoilChar::VAN_GENUCHTEN};
        for (int i = 0; i < kCells; ++i) {
            if (mixed) {
                s.closure[i] = static_cast<int8_t>(i % 3 == 0   ? GwClosure::SIGMA
                                                   : i % 3 == 1 ? GwClosure::CLOSED_FORM
                                                                : GwClosure::ENSLAVED);
                s.soil_char[i] = static_cast<int8_t>(laws[i % 4]);
            }
            s.psi_b[i] = .2;
            s.lambda[i] = .5;
            s.vg_n[i] = 2;
            s.vg_L[i] = .5;
            // A head field with gradients in both directions and a few dry pockets.
            s.hg[i] = 1.5 + .4 * std::sin(.37 * i) + .1 * (i % 11) / 11.;
            const double theta = .2 + .005 * (i % 7);
            s.hu[i] = theta * (s.zs[i] - s.hg[i]);
            for (int j = 0; j < s.m_layers; ++j)
                s.theta_sigma[std::size_t(j) * kCells + i] = theta;
            s.xacc_from_surface[i] = (i % 2 ? 1e-6 : -1e-6) * (1 + i % 5);
        }
        for (std::size_t n = 0; n < s.nacc.size(); ++n)
            s.nacc[n] = n % 2 ? 2e-7 : -2e-7;
        for (int i = 0; i < kCells; ++i)
            surf.depth[i] = i % 3 == 0 ? 0. : .01 * (i % 5);
        gw.refreshDtCell(mesh, edges);
        gw.assignTiers(1., 1);
    }
};
} // namespace

TEST(Gw2DParallelSweeps, CellSweepMatchesSerialAcrossClosuresAndSoils) {
    for (int threads : {2, 4, 8}) {
        SCOPED_TRACE("threads=" + std::to_string(threads));
        SweepRig serial(1, GwClosure::SIGMA, true), parallel(threads, GwClosure::SIGMA, true);
        for (int step = 0; step < 4; ++step) {
            const double dt = 1., time = step * dt;
            for (auto* rig : {&serial, &parallel})
                for (int i = 0; i < SweepRig::kCells; ++i)
                    rig->surf.depth[i] = (i + step) % 3 == 0 ? 0. : .01 * ((i + step) % 5);
            serial.gw.fireGwCells(0, dt, serial.surf, time);
            parallel.gw.fireGwCells(0, dt, parallel.surf, time);
            expectSameState(serial.gw.state(), parallel.gw.state(), serial.surf, parallel.surf);
            EXPECT_EQ(checkpointBytes(serial.gw), checkpointBytes(parallel.gw)) << "step " << step;
        }
        EXPECT_EQ(serial.gw.parallelCellSweeps(), 0);
        if (SubsurfaceSolver::parallelCellSweepAvailable())
            EXPECT_EQ(parallel.gw.parallelCellSweeps(), 4);
        saveBytes("cell_sweep_" + std::to_string(threads) + ".chk", checkpointBytes(parallel.gw));
        if (threads == 2) {
            // The state after four sweeps, in text, for tolerance comparisons across numerics
            // changes of the column (Track 4B): the checkpoint bytes above are bit-level.
            fs::create_directories(kOutDir);
            std::ofstream csv(kOutDir / "sigma_state_after_sweeps.csv");
            csv.precision(17);
            const auto& s = serial.gw.state();
            csv << "cell,closure,soil,hg,hu";
            for (int j = 0; j < s.m_layers; ++j)
                csv << ",theta" << j;
            csv << "\n";
            for (int i = 0; i < SweepRig::kCells; ++i) {
                csv << i << "," << int(s.closure[i]) << "," << int(s.soil_char[i]) << "," << s.hg[i]
                    << "," << s.hu[i];
                for (int j = 0; j < s.m_layers; ++j)
                    csv << "," << s.theta_sigma[std::size_t(j) * SweepRig::kCells + i];
                csv << "\n";
            }
            csv << "ledger," << s.led_recharge << "," << s.led_lateral << "," << s.led_node << ","
                << s.led_dunne << "," << s.led_infil_in << "\n";
        }
    }
}

TEST(Gw2DParallelSweeps, RatioFaceLoopAndCellSweepMatchSerialOnAConnectedMesh) {
    for (auto closure : {GwClosure::CLOSED_FORM, GwClosure::SIGMA})
        for (int threads : {2, 4, 8}) {
            SCOPED_TRACE("closure=" + std::to_string(int(closure)) +
                         " threads=" + std::to_string(threads));
            SweepRig serial(1, closure, false), parallel(threads, closure, false);
            ASSERT_TRUE(serial.gw.tierHasFaces(0));
            ASSERT_GE(serial.edges.ne, 512);
            for (int step = 0; step < 6; ++step) {
                const double dt = 1., time = step * dt;
                serial.gw.fireGwFaces(0, dt);
                parallel.gw.fireGwFaces(0, dt);
                expectSame("eacc_L after faces", serial.gw.state().eacc_L,
                           parallel.gw.state().eacc_L);
                expectSame("eacc_R after faces", serial.gw.state().eacc_R,
                           parallel.gw.state().eacc_R);
                serial.gw.fireGwCells(0, dt, serial.surf, time);
                parallel.gw.fireGwCells(0, dt, parallel.surf, time);
                expectSameState(serial.gw.state(), parallel.gw.state(), serial.surf, parallel.surf);
            }
            EXPECT_EQ(checkpointBytes(serial.gw), checkpointBytes(parallel.gw));
            EXPECT_EQ(serial.gw.parallelRatioFacePasses(), 0);
            EXPECT_EQ(serial.gw.parallelCellSweeps(), 0);
            if (SubsurfaceSolver::parallelCellSweepAvailable()) {
                EXPECT_EQ(parallel.gw.parallelRatioFacePasses(), 6);
                EXPECT_EQ(parallel.gw.parallelCellSweeps(), 6);
            }
            EXPECT_NE(parallel.gw.state().led_lateral, 0.) << "no lateral flow was exercised";
            saveBytes("faces_" + std::to_string(int(closure)) + "_" + std::to_string(threads) +
                          ".chk",
                      checkpointBytes(parallel.gw));
        }
}

namespace {
/**
 * @brief A strip of nx squares (2 nx cells) over a flat 4 m aquifer, closed on every side, with
 * the water table a step of +-delta about `table` at the middle; the lateral Darcy term is the
 * only process (no recharge, ET, Dunne return or node exchange).
 *
 * The ENSLAVED closure keeps the column at rest without input (its balance W(h) is solved
 * instantaneously, so a cell that receives nothing keeps its table). CLOSED_FORM would not do:
 * its unsaturated store has its own ODE and drains toward the table even with no recharge,
 * which on this soil moves the table by over a metre and buries the 2 cm lateral signal.
 */
struct Strip {
    MeshData mesh;
    InertialEdges edges;
    SurfaceStateData surf;
    SolverOptions2D opts;
    SubsurfaceConfig cfg;
    SubsurfaceSolver gw;
    static constexpr double kKs = 1e-3, kZs = 4., kTheta = .45, kThetaR = .1, kAlpha = 2.;

    Strip(int nx, double length, double table, double delta, bool orthogonal) {
        mesh = gridMesh(nx, 1, length / nx, 10., orthogonal);
        edges.build(mesh);
        surf.resize(mesh.n_triangles(), mesh.n_vertices());
        opts.momentum = Momentum2D::LOCAL_INERTIAL;
        cfg.options.authored = true;
        cfg.options.closure = GwClosure::ENSLAVED;
        cfg.options.force_closed_form = true;
        cfg.options.dunne = false;
        cfg.options.gw_et = "NONE";
        GwAquiferRow row;
        row.Ks = kKs;
        row.zs = kZs;
        row.theta_s = kTheta;
        row.theta_r = kThetaR;
        row.alpha = kAlpha;
        row.hg0 = table;
        cfg.rows.push_back(row);
        for (int c = 0; c < mesh.n_triangles(); ++c) {
            GwAquiferRow cell = row;
            cell.scope = 2;
            cell.cell = c;
            cell.hg0 = table + (mesh.tri_cx[c] < length / 2 ? -delta : delta);
            cfg.rows.push_back(cell);
        }
        std::vector<std::string> warnings;
        const auto error = gw.initialize(mesh, edges, opts, {}, 0, cfg, warnings);
        if (!error.empty())
            throw std::runtime_error(error);
        gw.assignTiers(1., 1);
    }
    void march(double dt, int steps) {
        for (int k = 0; k < steps; ++k) {
            gw.fireGwFaces(0, dt);
            gw.fireGwCells(0, dt, surf, k * dt);
        }
    }
};

struct StripRun {
    std::vector<double> hg, x;
    int steps = 0;
    double drift = 0;
};

/// Linearized Boussinesq (diffusion) solution of the closed strip for the initial step.
double diffusionStep(double x, double t, double length, double table, double delta, double D) {
    double h = table;
    for (int n = 1; n < 400; n += 2) {
        const double k = n * std::numbers::pi / length;
        const double a = -(4 * delta / (n * std::numbers::pi)) * std::sin(n * std::numbers::pi / 2);
        h += a * std::cos(k * x) * std::exp(-D * k * k * t);
    }
    return h;
}

/// The value of `run` at the cell whose centroid coincides with x (the meshes nest exactly).
double valueAt(const StripRun& run, double x) {
    std::size_t best = 0;
    double best_d = 1e300;
    for (std::size_t f = 0; f < run.x.size(); ++f)
        if (const double d = std::abs(run.x[f] - x); d < best_d) {
            best_d = d;
            best = f;
        }
    return run.hg[best];
}
} // namespace

TEST(Gw2DLateralDarcy, StripConvergesUnderMeshRefinement) {
    // Flat bed, uniform soil, a 2 cm table step about 2 m. The ENSLAVED column's effective
    // specific yield is dW/dh = theta_s - dW_u/dL at the initial depth to the table, taken from
    // the soil's own equilibrium storage, so the linearized Boussinesq (diffusion) solution with
    // D = K h / Sy is a reference to the percent the donor-thickness nonlinearity allows. The
    // gate is the comparison with the finest mesh (the meshes nest, centroid on centroid): the
    // error must fall with every halving and already be small on the coarsest mesh. dt scales
    // with dx^2 at a tenth of the explicit stability bound of the triangle operator, so time and
    // space refine together. The same problem on the zigzag layout is recorded, not gated: its
    // two-point flux takes the skewed centroid chord as the gradient arm (InertialEdges::inv_dx
    // against inv_dx_normal), so it converges to a different limit.
    constexpr double length = 16., table = 2., delta = .02;
    soil::Params p;
    p.law = SoilChar::RUSSO;
    p.Ks = Strip::kKs;
    p.theta_s = Strip::kTheta;
    p.theta_r = Strip::kThetaR;
    p.alpha = Strip::kAlpha;
    const double L0 = Strip::kZs - table, eps = 1e-4;
    const double Sy =
        p.theta_s -
        (soil::equilibriumStorage(p, L0 + eps) - soil::equilibriumStorage(p, L0 - eps)) / (2 * eps);
    RecordProperty("effective_specific_yield", std::to_string(Sy));
    EXPECT_GT(Sy, .1);
    EXPECT_LT(Sy, p.theta_s - p.theta_r);
    const double D = Strip::kKs * table / Sy;
    const double t_end = .05 * length * length / D; // the first mode decays to 0.61
    fs::create_directories(kOutDir);
    std::ofstream csv(kOutDir / "lateral_strip_convergence.csv");
    csv << "mesh,nx,dx,steps,err_vs_analytic,err_vs_finest,water_drift\n";
    const auto solve = [&](int nx, bool orthogonal) {
        Strip strip(nx, length, table, delta, orthogonal);
        StripRun run;
        run.x = strip.mesh.tri_cx;
        const double dx = length / nx;
        run.steps = static_cast<int>(std::ceil(t_end / (.05 * dx * dx / D)));
        const double before = strip.gw.state().storage();
        strip.march(t_end / run.steps, run.steps);
        run.drift = std::abs(strip.gw.state().storage() - before);
        run.hg = strip.gw.state().hg;
        return run;
    };
    constexpr int finest = 128;
    StripRun fine[2];
    for (bool orthogonal : {true, false}) {
        const char* layout = orthogonal ? "orthogonal" : "zigzag";
        SCOPED_TRACE(layout);
        StripRun& reference = fine[orthogonal ? 0 : 1];
        reference = solve(finest, orthogonal);
        EXPECT_LT(reference.drift, 1e-9);
        double previous = 0;
        for (int nx : {16, 32, 64}) {
            const StripRun run = solve(nx, orthogonal);
            std::ofstream profile(kOutDir / (std::string("lateral_strip_profile_") + layout +
                                             "_nx" + std::to_string(nx) + ".csv"));
            profile << "cell,x,hg,analytic,finest\n";
            double err_analytic = 0, err_self = 0;
            for (std::size_t c = 0; c < run.hg.size(); ++c) {
                const double x = run.x[c];
                const double analytic = diffusionStep(x, t_end, length, table, delta, D);
                const double at_finest = valueAt(reference, x);
                profile << c << "," << x << "," << run.hg[c] << "," << analytic << "," << at_finest
                        << "\n";
                err_analytic = std::max(err_analytic, std::abs(run.hg[c] - analytic));
                err_self = std::max(err_self, std::abs(run.hg[c] - at_finest));
            }
            csv << layout << "," << nx << "," << length / nx << "," << run.steps << ","
                << err_analytic << "," << err_self << "," << run.drift << "\n";
            const std::string tag = std::string(layout) + "_nx" + std::to_string(nx);
            RecordProperty(tag + "_err_analytic_over_delta", std::to_string(err_analytic / delta));
            RecordProperty(tag + "_err_finest_over_delta", std::to_string(err_self / delta));
            EXPECT_LT(run.drift, 1e-9) << "lateral Darcy must conserve the strip's water";
            // Both layouts: the gradient arm is the face-normal projection of the centroid
            // chord (inv_dx_normal), so the zigzag's skewed pairs no longer change the limit.
            EXPECT_LT(err_analytic, .05 * delta) << "nx=" << nx;
            if (orthogonal) {
                if (nx == 16)
                    EXPECT_LT(err_self, 1e-3 * delta) << "coarsest mesh already converged";
                if (previous > 0)
                    EXPECT_LT(err_self, previous) << "nx=" << nx;
            }
            previous = err_self;
        }
    }
    // The two layouts' finest solutions: the gap is the skewed-chord error of the zigzag.
    double layout_gap = 0;
    for (std::size_t c = 0; c < fine[0].hg.size(); ++c)
        layout_gap = std::max(layout_gap, std::abs(fine[0].hg[c] - valueAt(fine[1], fine[0].x[c])));
    RecordProperty("zigzag_vs_orthogonal_finest_over_delta", std::to_string(layout_gap / delta));
    csv << "layout_gap_over_delta,,,,," << layout_gap / delta << ",\n";
    // With the centroid chord as the arm this gap was 2.7 % of the step (2026-11-26).
    EXPECT_LT(layout_gap, 1e-6 * delta);
}

TEST(SoilCurve, ValuesMatchTheScalarFunctionsAndSlopesMatchCenteredDifferences) {
    // Track 4B: soil::curve() feeds the σ column's Newton matrix. Its values must be the
    // scalar functions' (exactly for the laws that share expressions, to roundoff for van
    // Genuchten's Kr through the Se^(1/m) = 1/(1 + (αψ)^n) identity) and its slopes the
    // derivatives of those functions, checked against centered differences away from clamps
    // and kinks. The Russo inverse must still invert its retention curve.
    struct Case {
        SoilChar law;
        double vg_n;
    };
    for (const Case c : {Case{SoilChar::GARDNER, 1.6}, Case{SoilChar::RUSSO, 1.6},
                         Case{SoilChar::BROOKS_COREY, 1.6}, Case{SoilChar::VAN_GENUCHTEN, 1.6},
                         Case{SoilChar::VAN_GENUCHTEN, 2.5}}) {
        SCOPED_TRACE("law=" + std::to_string(int(c.law)) + " n=" + std::to_string(c.vg_n));
        soil::Params p;
        p.law = c.law;
        p.Ks = 1e-5;
        p.alpha = 2.;
        p.psi_b = .2;
        p.lambda = .5;
        p.vg_n = c.vg_n;
        p.vg_L = .5;
        int checked_dse = 0, checked_dkr = 0;
        for (double psi = 1e-4; psi < 2e3; psi *= 1.07) {
            const soil::Curve k = soil::curve(p, psi);
            const double se = soil::effectiveSaturation(p, psi);
            const double kr = soil::relativeConductivity(p, psi);
            if (c.law == SoilChar::VAN_GENUCHTEN) {
                EXPECT_NEAR(k.Se, se, 1e-14 * se) << psi;
                // The scalar form evaluates 1 − Se^(1/m) by cancellation near saturation (its
                // error reaches 1e-12 relative at ψ ~ 1e-4 for n = 2.5); the curve's ap/(1+ap)
                // does not.
                EXPECT_NEAR(k.Kr, kr, 1e-11 * std::max(kr, 1e-300)) << psi;
            } else {
                EXPECT_EQ(k.Se, se) << psi;
                EXPECT_EQ(k.Kr, kr) << psi;
            }
            // A centered difference of values near 1 over 2h carries eps/h of roundoff, which
            // near saturation is far above the 1e-5 relative agreement asked of the slope.
            constexpr double eps = std::numeric_limits<double>::epsilon();
            const double h = 1e-6 * std::max(psi, 1e-2);
            const bool kink = c.law == SoilChar::BROOKS_COREY && std::abs(psi - p.psi_b) < 2 * h;
            // On a clamp (the 1e-8 floor, or saturated at exactly 1) the slopes are zero; in the
            // last 1e-9 below saturation the slope is real but too small to difference.
            const bool clamped = se <= 1e-8 * (1 + 1e-6) || se >= 1.;
            const bool se_open = !clamped && se < 1 - 1e-9 && !kink;
            if (se_open) {
                const double fd = (soil::effectiveSaturation(p, psi + h) -
                                   soil::effectiveSaturation(p, psi - h)) /
                                  (2 * h);
                EXPECT_NEAR(k.dSe, fd, 1e-5 * std::abs(fd) + 8 * eps * se / h) << psi;
                ++checked_dse;
                // The inverse returns the suction the curve was evaluated at, to the roundoff of
                // Se (eps) divided by the retention slope.
                EXPECT_NEAR(soil::suctionAtSaturation(p, k.Se), psi,
                            1e-9 * psi + 8 * eps / std::abs(k.dSe))
                    << psi;
            } else if (clamped && !kink) {
                EXPECT_EQ(k.dSe, 0.) << psi;
            }
            const bool kr_open = kr > 1e-300 && kr < 1 - 1e-12 && !kink;
            if (kr_open) {
                // Differenced on the curve's own values: the scalar form's cancellation (above)
                // would put 1e-4 of noise on a 1e-8 step near saturation.
                const double fd =
                    (soil::curve(p, psi + h).Kr - soil::curve(p, psi - h).Kr) / (2 * h);
                const double chord = (k.Kr - 1) / psi;
                // van Genuchten evaluates 1 − (ap/(1 + ap))^m by cancellation in its tail, which
                // puts eps·(1 + ap)/m of relative noise on Kr and so on its difference quotient.
                double noise = 8 * eps * kr / h;
                if (c.law == SoilChar::VAN_GENUCHTEN) {
                    const double u = 1 + std::pow(p.alpha * psi, p.vg_n);
                    noise *= 1 + u / (1 - 1 / p.vg_n);
                }
                const double rel = 1e-5;
                if (c.law == SoilChar::VAN_GENUCHTEN && k.dKr == chord) {
                    // The chord bound: only for n < 2, and only where the slope is unbounded.
                    EXPECT_LT(c.vg_n, 2.) << psi;
                    EXPECT_LT(psi, 1e-2) << psi;
                    EXPECT_LT(fd, chord) << "bounded slope must be steeper than the chord " << psi;
                } else {
                    EXPECT_NEAR(k.dKr, fd, rel * std::abs(fd) + noise) << psi;
                }
                ++checked_dkr;
            }
        }
        EXPECT_GT(checked_dse, 100);
        EXPECT_GT(checked_dkr, 100);
        // Saturated: unit values, zero slopes.
        const soil::Curve wet = soil::curve(p, 0.);
        EXPECT_EQ(wet.Se, 1.);
        EXPECT_EQ(wet.Kr, 1.);
        EXPECT_EQ(wet.dSe, 0.);
        EXPECT_EQ(wet.dKr, 0.);
    }
}
