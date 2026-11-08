/**
 * @file      test_fv_coupled_ponding.cpp
 * @brief     A 2D-coupled junction under FLOW_ROUTING FV ponds on the 2D footprint the surface
 *            router assigns it, so its head follows the surface instead of clamping at the rim.
 *
 * @details The 2D router assigns a coupled junction's ponding area (the median-dual footprint of
 *          the cells around its coupling vertex) in SurfaceRouter2D::initialize, which runs at
 *          start(), after Router::init built the FV mesh from ctx.nodes.ponded_area (0 for an
 *          ordinary junction). Dynamic wave reads the footprint live; the FV mesh copied the zero
 *          and so sealed every coupled junction at its rim: water the surface pushed in was booked
 *          as flooding and spilled straight back, and the 1D HGL never met the 2D surface.
 *          Router::refreshFvNodePonding re-reads the arrays once the 2D router has initialized.
          Arming the pond then exposed a second defect, fixed alongside: the bucket of a pondable
          algebraic junction carries the nominal below-rim junction volume as a base that is not
          water, and faces, floors and the re-promotion fold treated it as water (pondBase).
 *
 * @ingroup engine_fv
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/engine/openswmm_nodes.h>

#include "core/SWMMEngine.hpp"

namespace fs = std::filesystem;

namespace {

const fs::path kOut = "fv_coupled_ponding_out";
constexpr int kN = 10;                       // 10 x 10 m patch of 1 m squares
constexpr int kVertex = (kN / 2) * (kN + 1) + kN / 2; // J1's coupling vertex

/**
 * @brief The CouplingPonds fixture under the requested routing: a 0.2 m pipe fed two 0.09 m3/s
 *        pulses at J1 (MaxDepth 1 m), J1 coupled to the centre vertex of a flat 10 m pan.
 * @param[in] routing FLOW_ROUTING token.
 * @return Owned forty-minute CMS deck.
 */
std::string deck(const char* routing, const char* opts2d = "") {
    const double h = 10.0 / kN;
    std::ostringstream m;
    m << "[OPTIONS]\nFLOW_UNITS CMS\nFLOW_ROUTING " << routing
      << "\nSTART_DATE 01/01/2026\n"
         "START_TIME 00:00:00\nEND_DATE 01/01/2026\nEND_TIME 00:40:00\nREPORT_STEP 00:01:00\n"
         "ROUTING_STEP 2\nALLOW_PONDING NO\n\n"
         "[JUNCTIONS]\nJ1 0.0 1.0 0 0 0\n\n[OUTFALLS]\nO1 -0.5 FREE NO\n\n"
         "[CONDUITS]\nC1 J1 O1 30.0 0.013 0 0 0\n\n[XSECTIONS]\nC1 CIRCULAR 0.2 0 0 0 1\n\n"
         "[INFLOWS]\nJ1 FLOW IN_TS FLOW 1.0 1.0\n\n"
         "[TIMESERIES]\n"
         "IN_TS 0:00 0.0\nIN_TS 0:02 0.09\nIN_TS 0:08 0.09\nIN_TS 0:09 0.0\n"
         "IN_TS 0:14 0.0\nIN_TS 0:15 0.09\nIN_TS 0:20 0.09\nIN_TS 0:21 0.0\nIN_TS 0:40 0.0\n\n"
         "[2D_OPTIONS]\nMAX_TIMESTEP 1\nDRY_DEPTH 0.002\nREPORT_2D NO\n"
      << opts2d << "\n[2D_VERTICES]\n";
    for (int j = 0; j <= kN; ++j)
        for (int i = 0; i <= kN; ++i)
            m << i * h << " " << j * h << " 1.0\n";
    m << "\n[2D_TRIANGLES]\n";
    auto v = [](int i, int j) { return j * (kN + 1) + i; };
    for (int j = 0; j < kN; ++j)
        for (int i = 0; i < kN; ++i) {
            m << v(i, j) << " " << v(i + 1, j) << " " << v(i + 1, j + 1) << " 0.03\n";
            m << v(i, j) << " " << v(i + 1, j + 1) << " " << v(i, j + 1) << " 0.03\n";
        }
    m << "\n[2D_VERTEX_NODE_MAP]\n" << kVertex << " J1\n";
    return m.str();
}

struct PondRun {
    bool ok = false;
    double peak_depth = 0.0;     ///< J1, m
    int ponded_steps = 0;        ///< routing steps with J1 more than 2 cm above its rim
    double worst_hgl_gap = 0.0;  ///< max |J1 head - 2D head at its vertex| while ponded, m
    double mean_hgl_gap = 0.0;   ///< mean of the same, m
    int reversals = 0;           ///< sign changes of J1's coupling inflow
    double spilled = 0.0, drained = 0.0, cont_2d = 0.0, cont_routing = 0.0;
};

/**
 * @brief Run the fixture and compare J1's head with the 2D surface at its vertex every step.
 * @param[in] tag Safe retained kOut basename.
 * @param[in] routing FLOW_ROUTING token.
 * @return Measures above; ok=false when the lifecycle refused the deck.
 */
PondRun run(const std::string& tag, const char* routing, const char* opts2d = "") {
    PondRun r;
    fs::create_directories(kOut);
    const fs::path inp = kOut / (tag + ".inp");
    {
        std::ofstream f(inp);
        f << deck(routing, opts2d);
    }
    SWMM_Engine e = swmm_engine_create();
    if (swmm_engine_open(e, inp.string().c_str(), (kOut / (tag + ".rpt")).string().c_str(), "",
                         nullptr) != SWMM_OK ||
        swmm_engine_initialize(e) != SWMM_OK || swmm_engine_start(e, 0) != SWMM_OK) {
        swmm_engine_destroy(e);
        return r;
    }
    auto* engine = static_cast<openswmm::SWMMEngine*>(e);
    const auto& ctx = engine->context();
    const int j1 = swmm_node_index(e, "J1");
    const auto uj = static_cast<std::size_t>(j1);
    const double invert_m = ctx.nodes.invert_elev[uj] * 0.3048;
    std::ofstream trace(kOut / (tag + "_trace.csv"));
    trace << "elapsed_days,j1_depth_m,j1_head_m,eta_2d_vertex_m,coupling_inflow,j1_volume_m3,lat_flow_cfs,"
             "overflow_cfs,c1_flow_cms,spilled_m3,drained_m3\n";
    double elapsed = 0.0, gap_sum = 0.0;
    int last_sign = 0;
    for (;;) {
        if (swmm_engine_step(e, &elapsed) != SWMM_OK || elapsed <= 0.0)
            break;
        double depth = 0.0;
        swmm_node_get_depth(e, j1, &depth);
        r.peak_depth = std::max(r.peak_depth, depth);
        const double head_1d = invert_m + depth;
        const auto& st = engine->surfaceRouter2D().state();
        const double eta_2d = st.vert_head[static_cast<std::size_t>(kVertex)];
        const double q = ctx.nodes.coupling_inflow[uj];
        double c1q = 0.0, sp = 0.0, dr = 0.0;
        swmm_link_get_flow(e, swmm_link_index(e, "C1"), &c1q);
        swmm_2d_get_mass_balance(e, nullptr, nullptr, nullptr, &sp, &dr, nullptr, nullptr, nullptr,
                                 nullptr, nullptr);
        trace << elapsed << ',' << depth << ',' << head_1d << ',' << eta_2d << ',' << q << ','
              << ctx.nodes.volume[uj] * 0.3048 * 0.3048 * 0.3048 << ',' << ctx.nodes.lat_flow[uj]
              << ',' << ctx.nodes.overflow[uj] << ',' << c1q << ',' << sp << ',' << dr << '\n';
        if (depth > 1.02) {
            ++r.ponded_steps;
            const double gap = std::fabs(head_1d - eta_2d);
            r.worst_hgl_gap = std::max(r.worst_hgl_gap, gap);
            gap_sum += gap;
        }
        const int sign = q > 1e-6 ? 1 : (q < -1e-6 ? -1 : 0);
        if (sign != 0) {
            if (last_sign != 0 && sign != last_sign)
                ++r.reversals;
            last_sign = sign;
        }
    }
    if (r.ponded_steps > 0)
        r.mean_hgl_gap = gap_sum / r.ponded_steps;
    swmm_engine_end(e);
    swmm_2d_get_mass_balance(e, nullptr, nullptr, nullptr, &r.spilled, &r.drained, nullptr, nullptr,
                             nullptr, nullptr, nullptr);
    swmm_2d_get_continuity_error(e, &r.cont_2d);
    swmm_get_routing_continuity_error(e, &r.cont_routing);
    swmm_engine_report(e);
    swmm_engine_close(e);
    swmm_engine_destroy(e);
    r.ok = true;
    std::printf("[fv-coupled-ponding] %-8s peak depth %.3f m, %d ponded steps, HGL gap worst %.3f "
                "mean %.3f m, %d reversals, spilled %.2f drained %.2f m3, continuity 2D %.4f "
                "routing %.4f\n",
                routing, r.peak_depth, r.ponded_steps, r.worst_hgl_gap, r.mean_hgl_gap, r.reversals,
                r.spilled, r.drained, r.cont_2d, r.cont_routing);
    return r;
}

} // namespace

// The pipe takes a third of the pulse; the rest must stand on the pan above
// J1's rim with J1's head at the surface, under either router.
TEST(FvCoupledPonding, ACoupledJunctionPondsOnTheSurfaceFootprint) {
    const PondRun fv = run("fv", "FV");
    ASSERT_TRUE(fv.ok);
    ASSERT_GT(fv.spilled, 1.0) << "fixture never spilled";
    // Ponded: the junction's head rises above its 1 m rim (it clamped there
    // before the mesh re-read the footprint) ...
    EXPECT_GT(fv.peak_depth, 1.03) << "J1 never ponded above its rim";
    EXPECT_GT(fv.ponded_steps, 20);
    // ... and while ponded it is the surface: one HGL.
    EXPECT_LT(fv.mean_hgl_gap, 0.05) << "J1 head does not follow the 2D surface";
    EXPECT_LT(fv.worst_hgl_gap, 0.25);
    EXPECT_LT(std::fabs(fv.cont_2d), 0.01);
    // The routing ledger closes to the one phantom: a junction still ponded at
    // the last step publishes v_full + pond as stored volume (1.17 m3 here).
    EXPECT_LT(std::fabs(fv.cont_routing), 0.03);
}

// Same footprint, same physics: FV and dynamic wave pond J1 to the same depth
// and the surface recaptures what was spilled without cycling it.
TEST(FvCoupledPonding, FvAndDynamicWaveAgreeOnThePond) {
    const PondRun fv = run("fv_vs_dw_fv", "FV");
    const PondRun dw = run("fv_vs_dw_dw", "DYNWAVE");
    ASSERT_TRUE(fv.ok && dw.ok);
    EXPECT_NEAR(fv.peak_depth, dw.peak_depth, 0.15) << "pond depth differs between the routers";
    EXPECT_GT(fv.drained, 0.5 * fv.spilled) << "the pan was not recaptured";
    EXPECT_LT(fv.reversals, 3 * dw.reversals + 10) << "drain/spill churn under FV";
}

namespace {
std::string pondOnlyDeck(const char* routing) {
    std::ostringstream m;
    m << "[OPTIONS]\nFLOW_UNITS CMS\nFLOW_ROUTING " << routing
      << "\nSTART_DATE 01/01/2026\n"
         "START_TIME 00:00:00\nEND_DATE 01/01/2026\nEND_TIME 00:40:00\nREPORT_STEP 00:01:00\n"
         "ROUTING_STEP 2\nALLOW_PONDING YES\n\n"
         "[JUNCTIONS]\nJ1 0.0 1.0 0 0 1.0\n\n[OUTFALLS]\nO1 -0.5 FREE NO\n\n"
         "[CONDUITS]\nC1 J1 O1 30.0 0.013 0 0 0\n\n[XSECTIONS]\nC1 CIRCULAR 0.2 0 0 0 1\n\n"
         "[INFLOWS]\nJ1 FLOW IN_TS FLOW 1.0 1.0\n\n"
         "[TIMESERIES]\n"
         "IN_TS 0:00 0.0\nIN_TS 0:02 0.09\nIN_TS 0:08 0.09\nIN_TS 0:09 0.0\n"
         "IN_TS 0:14 0.0\nIN_TS 0:15 0.09\nIN_TS 0:20 0.09\nIN_TS 0:21 0.0\nIN_TS 0:40 0.0\n\n"
         "[REPORT]\nCONTINUITY YES\n";
    return m.str();
}
struct PondOnly {
    double peak_depth = 0.0, max_volume = 0.0, continuity = 0.0;
};
PondOnly pondOnly(const char* routing) {
    PondOnly out;
    fs::create_directories(kOut);
    const std::string tag = std::string("pond_only_") + routing;
    const fs::path inp = kOut / (tag + ".inp");
    { std::ofstream f(inp); f << pondOnlyDeck(routing); }
    SWMM_Engine e = swmm_engine_create();
    EXPECT_EQ(swmm_engine_open(e, inp.string().c_str(), (kOut / (tag + ".rpt")).string().c_str(), "", nullptr), SWMM_OK);
    EXPECT_EQ(swmm_engine_initialize(e), SWMM_OK);
    EXPECT_EQ(swmm_engine_start(e, 0), SWMM_OK);
    const int j1 = swmm_node_index(e, "J1");
    double elapsed = 0.0, peak = 0.0, vmax = 0.0;
    std::ofstream trace(kOut / (tag + "_trace.csv"));
    trace << "t_s,depth_m,volume_m3,lat_cfs,overflow_cfs,c1_cms\n";
    for (;;) {
        if (swmm_engine_step(e, &elapsed) != SWMM_OK || elapsed <= 0.0) break;
        double d = 0, v = 0, q = 0, o = 0, c = 0;
        swmm_node_get_depth(e, j1, &d); swmm_node_get_volume(e, j1, &v); swmm_node_get_lateral_inflow(e, j1, &q);
        swmm_node_get_overflow(e, j1, &o); swmm_link_get_flow(e, swmm_link_index(e, "C1"), &c);
        peak = std::max(peak, d); vmax = std::max(vmax, v);
        trace << elapsed * 86400 << ',' << d << ',' << v << ',' << q << ',' << o << ',' << c << '\n';
    }
    swmm_engine_end(e);
    double cont = 0.0; swmm_get_routing_continuity_error(e, &cont);
    swmm_engine_report(e); swmm_engine_close(e); swmm_engine_destroy(e);
    std::printf("[fv-pond-only] %-8s peak depth %.3f m, max volume %.3f m3, routing continuity %.4f\n", routing, peak, vmax, cont);
    out.peak_depth = peak;
    out.max_volume = vmax;
    out.continuity = cont;
    return out;
}
} // namespace

// The ponding junction alone, no 2D: ALLOW_PONDING YES, Aponded 1 m2. A pondable
// algebraic junction is stored as v_full + pond; folding the v_full base back
// into the carry at every re-promotion re-ponded 1.17 m3 that never existed
// each time the pond drained to the rim (peak 467 m, routing continuity -234 %
// on this deck before the fix; dynamic wave 1.93 m, -0.2 %).
TEST(FvCoupledPonding, APondingJunctionConservesWithoutAnySurface) {
    const PondOnly fv = pondOnly("FV");
    const PondOnly dw = pondOnly("DYNWAVE");
    EXPECT_LT(std::fabs(fv.continuity), 0.005) << "FV pond created or lost water";
    EXPECT_NEAR(fv.peak_depth, dw.peak_depth, 0.1) << "pond depth differs between the routers";
    EXPECT_GT(fv.peak_depth, 1.5) << "the junction never ponded";
}
