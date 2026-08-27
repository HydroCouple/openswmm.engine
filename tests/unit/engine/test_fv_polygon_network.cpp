// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file test_fv_polygon_network.cpp
 * @brief Network-level FV regression for a POLYGON cross-section (plan test 22).
 *
 * @details Runs the SAME small FV network twice — once with a built-in
 *          CIRCULAR conduit, once with that conduit replaced by a POLYGON
 *          whose `[CURVES] XPOLYGON` boundary traces the identical circle —
 *          and compares what a modeller would actually read off the run:
 *          peak outfall flow, routed volume, and the routing continuity
 *          error.
 *
 *          This is the only test in the Phase 8 set that exercises POLYGON
 *          through the full input path (`[CURVES]` parsing -> stride
 *          resolution -> `fromArcSpec` -> `compile()` -> PostParseResolver
 *          -> mesh build -> FV routing). Everything else builds the boundary
 *          in-process, so a break anywhere in the parsing/resolution chain
 *          would be invisible to them.
 *
 * @ingroup engine_fv
 */

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/engine/openswmm_massbalance.h>

namespace fs = std::filesystem;

namespace {

std::string outDir() {
    const std::string d = std::string(OPENSWMM_FV_TEST_OUT_DIR) + "/polygon_network";
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
}

/// Two conduits in series under FLOW_ROUTING FV. `polygon` selects whether
/// the FIRST conduit is a built-in CIRCULAR or a POLYGON tracing the same
/// circle; everything else is byte-identical between the two models.
///
/// Deliberately kept in a STABLE, part-full regime (12 cfs into a 4 ft pipe
/// with ~64 cfs of full-flow capacity). Driving it into surcharge instead
/// was tried and rejected: at 100 cfs the pair enters a violent filling
/// transient where peak outflow overshoots the inflow and the two
/// representations diverge by 2.6% — not because either is wrong, but
/// because a surcharge surge amplifies any difference chaotically. That
/// makes a poor regression signal. The cost of the stable regime is that
/// the routed result becomes fairly INSENSITIVE to the cross-section, so
/// it cannot double as the "is this the right shape?" check; that job is
/// done statically, from the report file, by checkGeometry() below.
///
/// The XPOLYGON curve is the four-quarter-arc bulge construction
/// (`bulge = tan(theta/4)` with theta = pi/2), the same one
/// LegacyShapeBoundary::buildCircle uses internally and
/// test_xsect_boundary.cpp's FourQuarterArcBulgesFormAFullCircle proves
/// traces an exact circle. Rows carry X Y BULGE triples, so the parser's
/// stride resolves to 3 on the first data row.
std::string writeModel(const std::string& name, bool polygon, double d) {
    const std::string path = outDir() + "/" + name + ".inp";
    const double r = 0.5 * d;
    const double b = std::tan(0.25 * 3.14159265358979323846 / 2.0);

    std::ofstream os(path);
    os.precision(17);
    os << "[OPTIONS]\nFLOW_UNITS           CFS\nFLOW_ROUTING         FV\n"
          "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
          "END_DATE             01/01/2026\nEND_TIME             02:00:00\n"
          "REPORT_STEP          00:01:00\nROUTING_STEP         5\n"
          "ALLOW_PONDING        NO\n\n"
          "[JUNCTIONS]\nJ0  40.0  12.0  0  0  0\nJ1  39.6  12.0  0  0  0\n\n"
          "[OUTFALLS]\nO1  39.2  FREE  NO\n\n"
          "[CONDUITS]\n"
          "C0  J0  J1  200.0  0.013  0  0  0\n"
          "C1  J1  O1  200.0  0.013  0  0  0\n\n";

    if (polygon) {
        os << "[CURVES]\n;;Name  Type      X  Y  Bulge\n"
           << "PCIRC   XPOLYGON  " << r << "  0.0  " << b
           << "   0.0  " << r << "  " << b << "\n"
           << "PCIRC             " << -r << "  0.0  " << b
           << "   0.0  " << -r << "  " << b << "\n\n";
    }

    os << "[XSECTIONS]\n";
    if (polygon)
        // scale=1, open-flag=0 (closed), geom3/4 unused, 1 barrel, curve name.
        os << "C0  POLYGON   1.0  0  0  0  1  PCIRC\n";
    else
        os << "C0  CIRCULAR  " << d << "  0  0  0  1\n";
    os << "C1  CIRCULAR  " << d << "  0  0  0  1\n\n"
          "[INFLOWS]\nJ0  FLOW  \"\"  FLOW  1.0  1.0  12.0\n\n"
          "[TIMESERIES]\n\n[REPORT]\nINPUT  YES\nCONTROLS  NO\n";
    return path;
}

struct Result {
    double peak_outflow   = 0.0;   ///< max |Q| in the downstream conduit (cfs)
    double routed_volume  = 0.0;   ///< cumulative outfall discharge
    double continuity_err = 0.0;   ///< routing continuity error (fraction)
    int    c0_poly_n      = 0;     ///< vertex count reported for the swapped link
    // Straight from the report file's Cross Section Summary — the geometry
    // a modeller reads off the run, for the swapped conduit C0.
    double c0_full_area   = 0.0;
    double c0_full_depth  = 0.0;
    double c0_hyd_rad     = 0.0;
    std::string rpt_path;
};

/// Open, run to completion, and read back the quantities a modeller sees.
Result runModel(const std::string& inp, const std::string& tag) {
    Result out;
    SWMM_Engine e = swmm_engine_create();
    EXPECT_NE(e, nullptr);
    const std::string rpt = outDir() + "/" + tag + ".rpt";
    const std::string bin = outDir() + "/" + tag + ".out";
    EXPECT_EQ(swmm_engine_open(e, inp.c_str(), rpt.c_str(), bin.c_str(), nullptr), 0)
        << swmm_get_last_error_msg(e);
    EXPECT_EQ(swmm_engine_initialize(e), 0);
    EXPECT_EQ(swmm_engine_start(e, 1), 0);

    const int c0 = swmm_link_index(e, "C0");
    const int c1 = swmm_link_index(e, "C1");
    EXPECT_GE(c0, 0);
    EXPECT_GE(c1, 0);

    // Does this link carry a compiled boundary at all? Only the count is
    // used: the vertex POSITIONS cannot discriminate here, because
    // swmm_link_get_polygon reports arcs by their endpoints (its own @note),
    // so an exact circle reads back as the inscribed diamond through its
    // four quadrant points — identical coordinates, identical bounding box.
    // The geometry itself is checked by volume below instead.
    int npts = 0;
    if (swmm_link_get_polygon(e, c0, nullptr, nullptr, &npts) == 0)
        out.c0_poly_n = npts;

    double t = 0.0;
    while (swmm_engine_step(e, &t) == 0 && t > 0.0) {
        double q = 0.0;
        if (swmm_link_get_flow(e, c1, &q) == 0)
            out.peak_outflow = std::max(out.peak_outflow, std::fabs(q));
    }

    swmm_get_routing_total(e, SWMM_ROUTING_OUTFLOW, &out.routed_volume);
    swmm_get_routing_continuity_error(e, &out.continuity_err);

    out.rpt_path = rpt;
    swmm_engine_end(e);
    swmm_engine_report(e);
    swmm_engine_close(e);
    swmm_engine_destroy(e);
    return out;
}

/// Read one conduit's row out of the report file's "Cross Section Summary".
/// That table is what a modeller actually reads to confirm a section
/// resolved as intended, and it is written from the resolved link arrays —
/// so it reports the COMPILED geometry for a POLYGON, not the input tokens.
/// Columns, per DefaultReportPlugin: name, shape, full depth, full area,
/// hydraulic radius, max width, barrels, full flow.
bool readXSectSummary(const std::string& rpt, const std::string& conduit,
                      double* full_depth, double* full_area, double* hyd_rad) {
    std::ifstream is(rpt);
    if (!is) return false;
    std::string line;
    bool in_table = false;
    while (std::getline(is, line)) {
        if (line.find("Cross Section Summary") != std::string::npos) {
            in_table = true;
            continue;
        }
        if (!in_table) continue;
        std::istringstream ss(line);
        std::string name, shape;
        double fd = 0.0, fa = 0.0, hr = 0.0;
        if (!(ss >> name >> shape >> fd >> fa >> hr)) continue;
        if (name != conduit) continue;
        *full_depth = fd;
        *full_area  = fa;
        *hyd_rad    = hr;
        return true;
    }
    return false;
}

double relDiff(double a, double b) {
    const double s = std::max(std::fabs(a), std::fabs(b));
    return (s > 0.0) ? std::fabs(a - b) / s : 0.0;
}

} // namespace

TEST(FvPolygonNetwork, PolygonCircleRoutesLikeTheBuiltInCircularConduit) {
    // Test 22. The plan asks for peak outflow and total volume within 0.1%,
    // reasoning that "the arc form is exact, so drift is a bug".
    //
    // That reasoning is half right and the conclusion needs care. The two
    // conduits are NOT the same geometry: the POLYGON is the exact circle,
    // while CIRCULAR is EPA's 26-point interpolated table, and the two
    // genuinely disagree — by ~1.2% in area around y/D = 0.02-0.05 and by
    // far more in the first thousandth of the depth (measured in
    // test_fv_solver_closure.cpp's WhereTheBackendsDisagreeTheCompiledOneIsRight,
    // where the legacy table's worst low-fill relative error is 4.7 against
    // the compiled path's 4.5e-7). So a difference at the network level is
    // EXPECTED, not a bug, and the honest bound is one sized to the geometry
    // difference actually being introduced rather than to zero.
    //
    // What makes 0.1% nevertheless the right order to test at here: the
    // conduit runs part-full but well loaded, where the table is at its
    // best, so the routed result tracks closely — measured, peak outflow
    // differs by 1.9e-5 and routed volume by 3.7e-6, both ~50x inside the
    // plan's bound.
    //
    // The routed numbers CANNOT, however, stand in for a geometry check,
    // and this was established by mutation rather than assumed: replacing
    // the circle with the inscribed DIAMOND (drop the bulge column — a
    // 36%-smaller section) moves peak outflow by only 9.6e-7 and routed
    // volume by 2.3e-4, both of which sail through every tolerance below.
    // The reason is physical, not a tolerance-tuning problem: a steady
    // inflow well inside capacity is passed along by whatever section is
    // there, so the routed result is nearly independent of it. Driving the
    // model into surcharge to force sensitivity was tried and rejected too
    // — see writeModel(). Hence the report-file geometry assertions that
    // follow: they are the part of this test that actually pins the shape,
    // and the routed comparisons are the part that pins the routing.
    const double d = 4.0;
    const std::string inp_circ = writeModel("net_circular", false, d);
    const std::string inp_poly = writeModel("net_polygon",  true,  d);

    const Result rc = runModel(inp_circ, "net_circular");
    const Result rp = runModel(inp_poly,  "net_polygon");

    // --- the pair really is an A/B ----------------------------------------
    EXPECT_EQ(rp.c0_poly_n, 4)
        << "POLYGON conduit did not resolve to a 4-arc compiled boundary";
    EXPECT_EQ(rc.c0_poly_n, 0)
        << "the CIRCULAR conduit unexpectedly carries a compiled boundary";

    // --- the POLYGON really is the intended CIRCLE -------------------------
    // Read back from the report file, and by AREA/hydraulic radius rather
    // than by vertex coordinates. This is the assertion that makes the
    // routed comparisons below non-vacuous, and coordinates could not do it:
    // swmm_link_get_polygon reports arcs by their ENDPOINTS (its own @note),
    // so an exact circle and the inscribed DIAMOND through its four quadrant
    // points read back as literally the same four vertices, with the same
    // bounding box. Dropping the bulge column is the natural way to get that
    // diamond by accident, and it was tried: it leaves full depth and max
    // width at 4.00 — unchanged — while full area falls 12.57 -> 8.00 and
    // hydraulic radius 1.00 -> 0.71. Both are checked, so the shape is
    // pinned by the two fields that actually move.
    double fd_p = 0.0, fa_p = 0.0, hr_p = 0.0;
    double fd_c = 0.0, fa_c = 0.0, hr_c = 0.0;
    ASSERT_TRUE(readXSectSummary(rp.rpt_path, "C0", &fd_p, &fa_p, &hr_p))
        << "no Cross Section Summary row for C0 in " << rp.rpt_path;
    ASSERT_TRUE(readXSectSummary(rc.rpt_path, "C0", &fd_c, &fa_c, &hr_c))
        << "no Cross Section Summary row for C0 in " << rc.rpt_path;

    const double a_true = 0.25 * 3.14159265358979323846 * d * d;   // 12.566
    const double r_true = 0.25 * d;                                // 1.000
    // The report prints 2 decimals, so compare at that resolution — enough
    // to separate a circle from a diamond by a mile, which is the job.
    EXPECT_NEAR(fa_p, a_true, 0.01) << "POLYGON full area is not the circle's";
    EXPECT_NEAR(hr_p, r_true, 0.01) << "POLYGON hydraulic radius is not the circle's";
    EXPECT_NEAR(fd_p, d, 0.01)      << "POLYGON full depth is not the diameter";
    EXPECT_NEAR(fa_c, a_true, 0.01) << "CIRCULAR full area disagrees with pi*d^2/4";
    EXPECT_NEAR(hr_c, r_true, 0.01) << "CIRCULAR hydraulic radius disagrees with d/4";

    // --- non-vacuity: the run has to have actually routed water ------------
    ASSERT_GT(rp.peak_outflow, 0.1) << "no flow — the model did not route";
    ASSERT_GT(rp.routed_volume, 0.0);

    // --- the routed result tracks -----------------------------------------
    EXPECT_LT(relDiff(rc.peak_outflow, rp.peak_outflow), 1.0e-3)
        << "peak outflow: circular=" << rc.peak_outflow
        << " polygon=" << rp.peak_outflow;
    EXPECT_LT(relDiff(rc.routed_volume, rp.routed_volume), 1.0e-3)
        << "routed volume: circular=" << rc.routed_volume
        << " polygon=" << rp.routed_volume;

    // --- continuity is a property of the SCHEME, not of the geometry -------
    // Unlike the two above, this one genuinely should not care which section
    // representation is in use: mass conservation is enforced by the FV
    // update itself. The plan's 0.01% is asserted as an absolute difference
    // in the error FRACTION, which is how a report reader compares two runs.
    EXPECT_LT(std::fabs(rc.continuity_err - rp.continuity_err), 1.0e-4)
        << "continuity: circular=" << rc.continuity_err
        << " polygon=" << rp.continuity_err;
    EXPECT_LT(std::fabs(rp.continuity_err), 0.02)
        << "POLYGON run's own continuity error is too large: "
        << rp.continuity_err;

    std::printf("[fv-net] peak Q: circ=%.6f poly=%.6f (rel %.2e)\n",
                rc.peak_outflow, rp.peak_outflow,
                relDiff(rc.peak_outflow, rp.peak_outflow));
    std::printf("[fv-net] volume: circ=%.6f poly=%.6f (rel %.2e)\n",
                rc.routed_volume, rp.routed_volume,
                relDiff(rc.routed_volume, rp.routed_volume));
    std::printf("[fv-net] continuity: circ=%.6f%% poly=%.6f%%\n",
                100.0 * rc.continuity_err, 100.0 * rp.continuity_err);
    std::printf("[fv-net] C0 geometry: circ A=%.2f R=%.2f | poly A=%.2f R=%.2f\n",
                fa_c, hr_c, fa_p, hr_p);
}
