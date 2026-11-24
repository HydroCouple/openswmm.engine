/**
 * @file      test_2d_inflow_boundary.cpp
 * @brief     A SPECIFIED_FLOW inflow under FULL_SWE admits at most a critical inflow (issue
 *            #214): the ghost of a prescribed discharge is never shallower than the critical
 *            depth of that discharge, so an inlet that once ran shallow and fast cannot feed its
 *            own jet. Kernel-level pin of swe::boundaryFlux plus two strip runs on a mild slope
 *            whose steady inlet depth is the Manning normal depth.
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_engine.h>

#include "2d/solver/SweKernels.hpp"

namespace fs = std::filesystem;
using openswmm::twoD::swe::boundaryFlux;
using openswmm::twoD::swe::BoundaryFlux;
using openswmm::twoD::swe::criticalDepth;
using openswmm::twoD::swe::kGravity;

namespace {

constexpr int kSpecifiedFlow = 3;

// A west edge (outward normal -x) of a cell whose water runs +x into the domain.
BoundaryFlux westInflow(double h_i, double u_i, double q_in) {
    return boundaryFlux(kSpecifiedFlow, 1.0, -1.0, 0.0, /*eta_i*/ h_i, h_i, h_i * u_i, 0.0,
                        /*n_man*/ 0.03, /*bed_slope*/ 0.0, /*eta_bc*/ 0.0, /*q_out*/ -q_in,
                        /*conv*/ 1.0, /*dry*/ 1.0e-3);
}

// Normal x-momentum flux of a state (h, q): q²/h + g h²/2.
double momentumFlux(double h, double q) {
    return q * q / h + 0.5 * kGravity * h * h;
}

} // namespace

// The prescribed mass flux is imposed whatever the interior does, and the momentum flux the
// cell takes is that of a Riemann problem against a ghost no shallower than critical depth. A
// supercritical, shallow interior used to be copied into the ghost, so the boundary handed back
// the jet's own momentum (q²/h, 108 m³/s² here against 13 for the critical ghost).
TEST(SweInflowBoundary, SupercriticalInteriorMeetsACriticalGhost) {
    const double q = 2.0, h_c = criticalDepth(q);
    const double h_jet = 0.037, u_jet = q / h_jet; // Fr ≈ 90
    const auto r = westInflow(h_jet, u_jet, q);
    ASSERT_TRUE(r.wet);
    EXPECT_NEAR(r.flow, q, 1e-12) << "the prescribed mass flux is imposed verbatim";
    EXPECT_GT(r.fmx, 0.0);
    EXPECT_LT(r.fmx, 1.5 * momentumFlux(h_c, q))
        << "the momentum flux must be that of a critical inflow, not the jet's "
        << momentumFlux(h_jet, q);
}

// A subcritical interior in equilibrium with the discharge keeps the invariant branch: the ghost
// equals the interior and the boundary flux is the interior's own.
TEST(SweInflowBoundary, SubcriticalInteriorKeepsTheInvariantBranch) {
    const double q = 2.0, h = 1.0, u = q / h; // Fr = 0.64
    const auto r = westInflow(h, u, q);
    ASSERT_TRUE(r.wet);
    EXPECT_NEAR(r.flow, q, 1e-12);
    EXPECT_NEAR(r.fmx, momentumFlux(h, q), 1e-9 * momentumFlux(h, q));
}

// A dry cell receives the inflow at critical depth.
TEST(SweInflowBoundary, DryCellReceivesTheInflow) {
    const auto r = westInflow(0.0, 0.0, 2.0);
    EXPECT_NEAR(r.flow, 2.0, 1e-12);
}

namespace {

const fs::path kOutDir = fs::path(OPENSWMM_INFLOW_BC_TEST_OUT_DIR) / "inflow_boundary_2d";

struct Strip {
    static constexpr int nx = 100;
    static constexpr double dx = 0.5, width = 1.0, slope = 0.0043, n_man = 0.0328, q = 2.0;
    static double normalDepth() { return std::pow(q * n_man / std::sqrt(slope), 0.6); }
};

// A 50 m x 1 m strip on a 0.43 % slope, n = 0.0328, 2 m²/s in at x = 0 and the normal-depth
// stage at x = 50 m: the steady state is uniform flow at the Manning normal depth (1.00 m,
// Fr = 0.64; critical depth 0.74 m). The SWASHES MacDonald inlet in miniature: two triangles
// per half metre, FROUDE_MAX 3, laid out as the suite's strips are. Before the fix the inlet
// of this strip sat at 0.16 m with the discharge right.
std::string stripDeck(double init_depth) {
    const int nvx = Strip::nx + 1;
    auto vid = [&](int ix, int iy) { return iy * nvx + ix; };
    auto z = [&](int ix) { return Strip::slope * (Strip::nx - ix) * Strip::dx; };
    std::ostringstream d;
    d << "[OPTIONS]\nFLOW_UNITS CMS\nINFILTRATION HORTON\nFLOW_ROUTING DYNWAVE\n"
         "START_DATE 01/01/2026\nSTART_TIME 00:00:00\nEND_DATE 01/01/2026\nEND_TIME 00:20:00\n"
         "REPORT_STEP 00:01:00\nWET_STEP 00:01:00\nDRY_STEP 00:01:00\nROUTING_STEP 1\n"
         "THREADS 1\n\n"
         "[JUNCTIONS]\nJD1 0 10 0 0 0\n\n[OUTFALLS]\nOD1 0 FREE NO\n\n"
         "[CONDUITS]\nCD1 JD1 OD1 100 0.01 0 0 0\n\n[XSECTIONS]\nCD1 CIRCULAR 1.0 0 0 0 1\n\n"
         "[COORDINATES]\nJD1 -5 -5\nOD1 -6 -5\n\n"
         "[2D_OPTIONS]\nINTEGRATOR EXPLICIT\nMOMENTUM_EQUATION FULL_SWE\nCFL_NUMBER 0.5\n"
         "MAX_TIMESTEP 1.0\nDRY_DEPTH 0.001\nREPORT_2D NO\nRAINFALL_MODE NONE\nFROUDE_MAX 3\n\n"
         "[2D_VERTICES]\n;; UNITS: SI (m)\n";
    for (int iy = 0; iy <= 1; ++iy)
        for (int ix = 0; ix < nvx; ++ix)
            d << ix * Strip::dx << " " << iy * Strip::width << " " << z(ix) << "\n";
    d << "\n[2D_TRIANGLES]\n";
    std::ostringstream bc;
    int tri = 0;
    for (int ix = 0; ix < Strip::nx; ++ix) {
        const int sw = vid(ix, 0), se = vid(ix + 1, 0), nw = vid(ix, 1), ne = vid(ix + 1, 1);
        if (ix % 2 == 0) { // SW-NE diagonal: west edge is edge 1 of the second triangle
            d << sw << " " << se << " " << ne << " " << Strip::n_man << " " << init_depth << "\n";
            d << sw << " " << ne << " " << nw << " " << Strip::n_man << " " << init_depth << "\n";
            if (ix == 0)
                bc << tri + 1 << " 1 SPECIFIED_FLOW " << -Strip::q << "\n";
            if (ix == Strip::nx - 1)
                bc << tri << " 0 SPECIFIED_STAGE " << Strip::normalDepth() << "\n";
        } else { // NW-SE diagonal: west edge is edge 1 of the first triangle
            d << sw << " " << se << " " << nw << " " << Strip::n_man << " " << init_depth << "\n";
            d << se << " " << ne << " " << nw << " " << Strip::n_man << " " << init_depth << "\n";
            if (ix == 0)
                bc << tri << " 1 SPECIFIED_FLOW " << -Strip::q << "\n";
            if (ix == Strip::nx - 1)
                bc << tri + 1 << " 2 SPECIFIED_STAGE " << Strip::normalDepth() << "\n";
        }
        tri += 2;
    }
    d << "\n[2D_BOUNDARY_CONDITIONS]\n;;TRI EDGE TYPE PARAM_1\n" << bc.str() << "\n";
    return d.str();
}

struct StripResult {
    bool ok = false;
    std::string error;
    double inlet_depth = 0.0, mid_depth = 0.0, min_inlet_depth = 1e9;
};

StripResult runStrip(const std::string& tag, double init_depth) {
    StripResult r;
    fs::create_directories(kOutDir);
    const auto inp = kOutDir / (tag + ".inp");
    std::ofstream(inp, std::ios::trunc) << stripDeck(init_depth);
    SWMM_Engine e = swmm_engine_create();
    const auto fail = [&] {
        r.error = swmm_get_last_error_msg(e);
        swmm_engine_destroy(e);
        return r;
    };
    if (swmm_engine_open(e, inp.string().c_str(), (kOutDir / (tag + ".rpt")).string().c_str(), "",
                         nullptr) != SWMM_OK ||
        swmm_engine_initialize(e) != SWMM_OK || swmm_engine_start(e, 0) != SWMM_OK)
        return fail();
    int nt = 0;
    swmm_2d_triangle_count(e, &nt);
    if (nt != 2 * Strip::nx)
        return fail();
    std::vector<double> depths(static_cast<std::size_t>(nt));
    double elapsed = 0.0;
    for (;;) {
        if (swmm_engine_step(e, &elapsed) != SWMM_OK)
            return fail();
        if (swmm_2d_get_depths_bulk(e, depths.data()) == SWMM_OK)
            r.min_inlet_depth = std::min(r.min_inlet_depth, 0.5 * (depths[0] + depths[1]));
        if (elapsed <= 0.0)
            break;
    }
    swmm_2d_get_depths_bulk(e, depths.data());
    r.inlet_depth = 0.5 * (depths[0] + depths[1]);
    double mid = 0.0;
    const int t0 = 2 * (Strip::nx / 2 - 5), t1 = 2 * (Strip::nx / 2 + 5); // ten mid columns
    for (int t = t0; t < t1; ++t)
        mid += depths[static_cast<std::size_t>(t)];
    r.mid_depth = mid / static_cast<double>(t1 - t0);
    swmm_engine_end(e);
    swmm_engine_close(e);
    swmm_engine_destroy(e);
    r.ok = true;
    return r;
}

} // namespace

// Started dry, the inlet settles at the normal depth and stays subcritical. Before the fix the
// first ghost was critical, the inlet cell went supercritical, and the ghost then copied that
// shallow state back: the inlet sat at a few centimetres while the discharge was right.
TEST(SweInflowBoundary, DryStartSettlesAtTheNormalDepth) {
    const auto r = runStrip("dry_start", 0.0);
    ASSERT_TRUE(r.ok) << r.error;
    const double h_n = Strip::normalDepth();
    EXPECT_NEAR(r.inlet_depth, h_n, 0.02 * h_n) << "inlet depth " << r.inlet_depth;
    EXPECT_NEAR(r.mid_depth, h_n, 0.03 * h_n) << "mid-channel depth " << r.mid_depth;
    EXPECT_GT(r.inlet_depth, criticalDepth(Strip::q)) << "the inlet must be subcritical";
}

// Started on the analytic profile, the inlet does not drift to the supercritical branch.
TEST(SweInflowBoundary, NormalDepthStartDoesNotDriftSupercritical) {
    const double h_n = Strip::normalDepth();
    const auto r = runStrip("normal_start", h_n);
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_NEAR(r.inlet_depth, h_n, 0.02 * h_n) << "inlet depth " << r.inlet_depth;
    EXPECT_GT(r.min_inlet_depth, criticalDepth(Strip::q))
        << "the inlet dipped below critical depth during the run";
}
