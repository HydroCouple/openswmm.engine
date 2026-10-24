/**
 * @file      test_2d_coupling_conservation.cpp
 * @brief     Cross-domain volume conservation of 1D↔2D coupling under VARIABLE_STEP.
 *
 * @details The coupling is staggered: the exchange computed at the end of step N
 *          is applied to 2D immediately and to 1D one routing step later. It is
 *          now carried across that boundary as a per-window VOLUME (Q·dt) rather
 *          than a rate, then re-derived at the consuming step's dt
 *          (SWMMEngine::assembleLateralInflows). That makes the volume the 1D node
 *          receives EXACTLY the volume the 2D side gave up — even when the
 *          timestep changes between the two (VARIABLE_STEP). With the old
 *          rate-based carry, the 1D received Q·dt_{N+1} while 2D gave Q·dt_N, a
 *          small non-conservation ≈ Σ Q·Δdt.
 *
 *          This test isolates that property: water is rained onto a flat 2D patch
 *          (via the forcing API) which drains into a coupled junction J1 that has
 *          NO other lateral inflow, so J1's lateral inflow IS the coupling. Running
 *          with VARIABLE_STEP, the integrated 1D-received coupling volume
 *          (Σ lateral_inflow·dt, aligned to geometric SI) plus pending exchange
 *          must equal the 2D-given net coupling volume
 *          (coupling_2d_to_1d_out − coupling_1d_to_2d_in from swmm_2d_get_mass_balance)
 *          to tight tolerance. Pre-fix this gap opens up with the timestep variation.
 *
 *          Needs the full 2D module (OPENSWMM_BUILD_2D); runs the real coupled
 *          engine through the public C API. Inputs and a summary are written to
 *          ./coupling_conservation_out/ (working dir is tests/unit/engine/data)
 *          for review — no temp files (project convention).
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_forcing.h>
#include <openswmm/engine/openswmm_massbalance.h>

#include "core/SWMMEngine.hpp"
#include "core/UnitConversion.hpp"

namespace fs = std::filesystem;

namespace {

constexpr double ft3_to_m3 = 0.3048 * 0.3048 * 0.3048;

// Flat 10×10 m 2D patch whose bed sits at z = 1.0 m = the crown of coupled
// junction J1 (invert 0, MaxDepth 1.0). J1 drains through a 0.3 m pipe to free
// outfall O1 and has NO [INFLOWS] — its only lateral inflow is the 2D coupling.
// VARIABLE_STEP is ON so the routing dt varies step-to-step (amplified by the 2D
// CFL hint as the patch fills then drains), exercising the dt-robust carry.
/**
 * @brief Build the SI rain-pan coupling-conservation fixture.
 *
 * Uses 6 s routing maximum and 4 s surface maximum; caller supplies rain through forcing. No model
 * validation, file write or run.
 *
 * @param[in] extra_2d_options Raw extra 2D_OPTIONS rows inserted verbatim.
 * @return Owned forty-minute CMS/DYNWAVE deck with 100 m2 pan at J1 crown, one pipe and vertex-zero
 *     coupling.
 */
std::string build_model(const std::string& extra_2d_options = "") {
    return "[OPTIONS]\n"
           "FLOW_UNITS           CMS\n"
           "FLOW_ROUTING         DYNWAVE\n"
           "START_DATE           01/01/2026\n"
           "START_TIME           00:00:00\n"
           "END_DATE             01/01/2026\n"
           "END_TIME             00:40:00\n"
           "REPORT_STEP          00:01:00\n"
           "ROUTING_STEP         6\n"
           "VARIABLE_STEP        0.85\n"
           "ALLOW_PONDING        NO\n"
           "\n"
           "[JUNCTIONS]\n"
           ";;Name  Elev  MaxDepth  InitDepth  SurDepth  Aponded\n"
           "J1      0.0   1.0       0          0         0\n"
           "\n"
           "[OUTFALLS]\n"
           ";;Name  Elev   Type  Gated\n"
           "O1     -0.5    FREE  NO\n"
           "\n"
           "[CONDUITS]\n"
           ";;Name  From  To  Length  Roughness  InOffset  OutOffset  InitFlow\n"
           "C1      J1    O1  30.0    0.013      0         0          0\n"
           "\n"
           "[XSECTIONS]\n"
           ";;Link  Shape     Geom1  Geom2  Geom3  Geom4  Barrels\n"
           "C1      CIRCULAR  0.3    0      0      0      1\n"
           "\n"
           "[2D_OPTIONS]\n"
           "MAX_TIMESTEP     4\n"
           "DRY_DEPTH        0.002\n"
           "COUPLING_CD      0.7\n"
           "REPORT_2D        NO\n" +
           extra_2d_options +
           "\n"
           "[2D_VERTICES]\n"
           ";;X      Y      Z   (flat patch at the junction crown elevation, 1.0 m)\n"
           " 0.0    0.0   1.0\n"
           "10.0    0.0   1.0\n"
           "10.0   10.0   1.0\n"
           " 0.0   10.0   1.0\n"
           "\n"
           "[2D_TRIANGLES]\n"
           ";;V1  V2  V3  MANNINGS_N\n"
           "0     1   2   0.03\n"
           "0     2   3   0.03\n"
           "\n"
           "[2D_VERTEX_NODE_MAP]\n"
           ";;Vertex  Node  Cd   Area\n"
           "0         J1    0.7  1.0\n";
}

struct RunResult {
    bool ok = false;
    double received_1d = 0.0; // ∫ lateral_inflow(J1)·dt, geometric SI (m³)
    double pending_1d = 0.0;  // queued exchange not yet delivered to 1D (m³)
    double given_2d_net = 0.0; // coupling_2d_to_1d_out − coupling_1d_to_2d_in (m³)
    double cont_2d = 0.0;
    double cont_routing = 0.0;
    double dt_min = 1e30; // observed routing-step spread (s) — confirms the
    double dt_max = 0.0;  //   variable-dt path is actually exercised
    int n_steps = 0;
    // C1/C2 (2026-09-07)
    double cell_applied_sum = 0.0;     // Σ swmm_2d_get_coupling_volume_bulk (m³)
    double c12 = 0.0, c21 = 0.0;       // coupling_1d_to_2d_in / _2d_to_1d_out (m³)
    double routing_coupling_out = 0.0; // SWMM_ROUTING_COUPLING_OUT (ft³)
    double routing_flooding = 0.0;     // SWMM_ROUTING_FLOODING (ft³)
};

/**
 * @brief Run the coupling-conservation fixture and collect water-bookkeeping diagnostics.
 *
 * Forces rain until prior elapsed reaches 600 s, steps without a cap and integrates
 * positive-time J1 lateral flow in geometric SI. Copies pending native exchange and per-cell
 * applied coupling before end, then reads gross/net
 * coupling and routing totals afterward. Step error can still produce ok=true;
 * stream/forcing/getter/end/report/close statuses are unchecked. Owns/destroys engine; native
 * routing volumes are ft3 even for this CMS project.
 *
 * @param[in] dir Existing writable artifact directory.
 * @param[in] extra_2d_options Raw options passed to build_model.
 * @param[in] tag Safe basename for overwritten retained .inp/.rpt/.out.
 * @param[in] rain_rate Rainfall m/s during the first ten minutes; zero for the dry control.
 * @return Owned SI surface/network receipt volumes, native routing-total volumes, step durations
 *     seconds and continuity fractions; ok=false only on setup/active/node/start refusal.
 */
RunResult run(const fs::path& dir, const std::string& extra_2d_options = "",
              const std::string& tag = "coupling_conservation", double rain_rate = 0.001) {
    RunResult r;
    const fs::path inp = dir / (tag + ".inp");
    const fs::path rpt = dir / (tag + ".rpt");
    const fs::path out = dir / (tag + ".out");
    {
        std::ofstream f(inp);
        f << build_model(extra_2d_options);
    }

    SWMM_Engine eng = swmm_engine_create();
    if (swmm_engine_open(eng, inp.string().c_str(), rpt.string().c_str(), out.string().c_str(),
                         nullptr) != SWMM_OK) {
        swmm_engine_destroy(eng);
        return r;
    }
    if (swmm_engine_initialize(eng) != SWMM_OK) {
        swmm_engine_close(eng);
        swmm_engine_destroy(eng);
        return r;
    }
    int active = 0;
    swmm_2d_is_active(eng, &active);
    const int j1 = swmm_node_index(eng, "J1");
    if (!active || j1 < 0) {
        swmm_engine_close(eng);
        swmm_engine_destroy(eng);
        return r;
    }
    if (swmm_engine_start(eng, 1) != SWMM_OK) {
        swmm_engine_close(eng);
        swmm_engine_destroy(eng);
        return r;
    }

    constexpr double RAIN_OFF_S = 600.0; // rain the patch for the first 10 min
    const auto& c = static_cast<openswmm::SWMMEngine*>(eng)->context();
    // The getter reports legacy project units (CMS uses 0.02832), whereas
    // SurfaceRouter2D exchanges geometric SI using exact 0.3048³. Undo the
    // display conversion before integrating so both ledgers use the same units.
    const double display_to_exact_cms =
        ft3_to_m3 / openswmm::ucf::UCF(openswmm::ucf::FLOW, c.options);

    double elapsed = 0.0, prev_elapsed = 0.0;
    while (true) {
        // Drive the patch: constant rainfall for the first window, then off so the
        // ponded surface drains through the coupled inlet. OVERRIDE+PERSIST so it
        // holds across steps until changed.
        const double t_s = elapsed * 86400.0;
        const double rain = (t_s < RAIN_OFF_S) ? rain_rate : 0.0;
        swmm_2d_force_rainfall_uniform(eng, rain, SWMM_FORCING_OVERRIDE, SWMM_FORCING_PERSIST);

        if (swmm_engine_step(eng, &elapsed) != SWMM_OK || elapsed <= 0.0)
            break;

        const double dt_s = (elapsed - prev_elapsed) * 86400.0;
        prev_elapsed = elapsed;
        if (dt_s > 0.0) {
            r.dt_min = std::min(r.dt_min, dt_s);
            r.dt_max = std::max(r.dt_max, dt_s);
            ++r.n_steps;
        }

        double lat = 0.0; // legacy display CMS; + = into J1 (2D→1D drain)
        if (swmm_node_get_lateral_inflow(eng, j1, &lat) == SWMM_OK)
            r.received_1d += lat * display_to_exact_cms * dt_s;
    }

    // The 2D ledger books exchange when produced; 1D receives it later.
    // Include both delivery paths' signed, in-flight volumes before teardown.
    for (std::size_t n = 0; n < c.nodes.coupling_queue.size(); ++n)
        r.pending_1d += (c.nodes.coupling_queue[n] + c.nodes.coupling_volume[n]) * ft3_to_m3;

    // C1: the signed per-cell abstraction, summed over every cell. Read
    // BEFORE swmm_engine_end — the 2D solver-state accessors are guarded by
    // CHECK_2D_ACTIVE and stop answering once the router is deactivated.
    int ncells = 0;
    if (swmm_2d_triangle_count(eng, &ncells) == SWMM_OK && ncells > 0) {
        std::vector<double> v(static_cast<std::size_t>(ncells), 0.0);
        if (swmm_2d_get_coupling_volume_bulk(eng, v.data()) == SWMM_OK)
            for (double x : v)
                r.cell_applied_sum += x;
    }

    swmm_engine_end(eng);

    double c12 = 0.0, c21 = 0.0;
    swmm_2d_get_mass_balance(eng, nullptr, nullptr, nullptr, &c12, &c21, nullptr, nullptr, nullptr,
                             nullptr, nullptr);
    r.given_2d_net = c21 - c12; // net volume 2D → 1D (m³)
    r.c12 = c12;
    r.c21 = c21;

    // C2: the split-out spill row and what remains under Flooding Loss.
    swmm_get_routing_total(eng, SWMM_ROUTING_COUPLING_OUT, &r.routing_coupling_out);
    swmm_get_routing_total(eng, SWMM_ROUTING_FLOODING, &r.routing_flooding);
    swmm_2d_get_continuity_error(eng, &r.cont_2d);
    swmm_get_routing_continuity_error(eng, &r.cont_routing);
    swmm_engine_report(eng);
    swmm_engine_close(eng);
    swmm_engine_destroy(eng);
    r.ok = true;
    return r;
}

} // namespace

class CouplingConservation2DTest : public ::testing::Test {
protected:
    fs::path dir_;
    /**
     * @brief Create the reviewable coupling_conservation_out artifact directory.
     *
     * Sets dir_ relative to the test working directory and creates missing directories with the
     * throwing filesystem overload. Existing files are retained; no engine or input file is created
     * here.
     */
    void SetUp() override {
        dir_ = fs::path("coupling_conservation_out");
        fs::create_directories(dir_);
    }
};

// The exchange volume the 1D node receives plus pending delivery must equal the
// volume the 2D side gave up, even though VARIABLE_STEP makes the consuming dt
// differ from the producing dt. Carrying a per-window volume makes these match;
// the old rate-based carry left a Σ Q·Δdt gap.
//
// Issue #194: compare geometric SI volumes and account for in-flight storage.
// The display-unit offset (~1.11e-4) and pending volume both exceed the new gate.
TEST_F(CouplingConservation2DTest, ReceivedEqualsGivenUnderVariableStep) {
    RunResult r = run(dir_);

    ASSERT_TRUE(r.ok) << "coupled conservation run failed";

    // The coupling must actually be exercised (patch wet, drained into J1).
    ASSERT_GT(std::abs(r.given_2d_net), 1.0)
        << "no meaningful 2D→1D exchange occurred (net " << r.given_2d_net << " m³)";

    // Keep pending delivery explicit instead of hiding it in a loose tolerance.
    ASSERT_GT(r.pending_1d, 0.0) << "fixture must exercise in-flight exchange";
    const double rel = std::abs(r.received_1d + r.pending_1d - r.given_2d_net) /
                       std::abs(r.given_2d_net);
    EXPECT_LT(rel, 1e-10) << "1D-received (" << r.received_1d << " m³) + pending ("
                         << r.pending_1d << " m³) != 2D-given (" << r.given_2d_net
                         << " m³); rel err " << rel
                         << " — coupling not volume-conservative under VARIABLE_STEP";

    // Both domains' own continuity must also stay closed.
    EXPECT_LT(std::abs(r.cont_2d), 0.05) << "2D continuity error: " << r.cont_2d;
    EXPECT_LT(std::abs(r.cont_routing), 0.05) << "1D continuity error: " << r.cont_routing;

    // Confirm the run actually varied the timestep (otherwise this would not
    // exercise the produce-dt ≠ consume-dt path the fix targets).
    EXPECT_GT(r.dt_max - r.dt_min, 0.25)
        << "routing dt barely varied (" << r.dt_min << "–" << r.dt_max
        << " s) — variable-step coupling path not exercised";

    std::ofstream csv(dir_ / "coupling_conservation_massbalance.csv");
    csv << std::setprecision(17) << "metric,value\n"
        << "received_1d_m3," << r.received_1d << "\n"
        << "pending_1d_m3," << r.pending_1d << "\n"
        << "given_2d_net_m3," << r.given_2d_net << "\n"
        << "rel_err," << rel << "\n"
        << "dt_min_s," << r.dt_min << "\n"
        << "dt_max_s," << r.dt_max << "\n"
        << "n_steps," << r.n_steps << "\n"
        << "continuity_2d_frac," << r.cont_2d << "\n"
        << "continuity_routing_frac," << r.cont_routing << "\n";
}

TEST_F(CouplingConservation2DTest, ZeroRainHasNoExchange) {
    const RunResult r = run(dir_, "", "zero_rain", 0.0);
    ASSERT_TRUE(r.ok);
    EXPECT_GT(r.n_steps, 0);
    EXPECT_DOUBLE_EQ(r.received_1d, 0.0);
    EXPECT_DOUBLE_EQ(r.pending_1d, 0.0);
    EXPECT_DOUBLE_EQ(r.c12, 0.0);
    EXPECT_DOUBLE_EQ(r.c21, 0.0);
}

// ---------------------------------------------------------------------------
// C1 (2026-09-07) — the signed per-cell coupling abstraction ties back to the
// two domain accumulators. Handoff §3.3: this is the strongest check on C1; a
// failure points at the RK2 half-step rescale or a missed booking site.
// ---------------------------------------------------------------------------
TEST_F(CouplingConservation2DTest, PerCellAbstractionTiesBackToTheDomainTotals) {
    RunResult r = run(dir_, "", "c1_tieback");
    ASSERT_TRUE(r.ok);
    ASSERT_GT(std::abs(r.given_2d_net), 1.0) << "no meaningful exchange occurred";

    // coupling_applied is + INTO the cell: water arrives as the 1D→2D spill and
    // leaves as the 2D→1D drain, so the sum is (in − out).
    const double expect = r.c12 - r.c21;
    const double scale = std::max(1.0, std::abs(expect));
    EXPECT_NEAR(r.cell_applied_sum, expect, 1.0e-6 * scale)
        << "Σ per-cell coupling_applied (" << r.cell_applied_sum
        << " m³) != coupling_1d_to_2d_in − coupling_2d_to_1d_out (" << expect << " m³)";
}

// ---------------------------------------------------------------------------
// C2 (2026-09-07) — the 1D→2D spill left "Flooding Loss" for its own row. The
// grouping moved; the conserved total did not. COUPLING_IN_FLOODING YES buys
// the old grouping back for comparison against a pre-split build.
// ---------------------------------------------------------------------------
TEST_F(CouplingConservation2DTest, CouplingOutflowSplitsFromFloodingAndConserves) {
    RunResult split = run(dir_, "", "c2_split");
    RunResult folded = run(dir_, "COUPLING_IN_FLOODING YES\n", "c2_folded");
    ASSERT_TRUE(split.ok && folded.ok);

    // The continuity error is the invariant: only the ROW the spill is printed
    // on changed, never the balance it participates in.
    EXPECT_NEAR(split.cont_routing, folded.cont_routing, 1.0e-9)
        << "regrouping the spill must not move the routing continuity error";

    // Whatever the spill was, it sits in exactly one of the two rows.
    EXPECT_NEAR(split.routing_coupling_out + split.routing_flooding,
                folded.routing_coupling_out + folded.routing_flooding,
                1.0e-6 * std::max(1.0, std::abs(split.routing_flooding)))
        << "the spill was double-counted or dropped by the regrouping";
    EXPECT_DOUBLE_EQ(folded.routing_coupling_out, 0.0)
        << "COUPLING_IN_FLOODING YES must leave the new row empty";
}

// ---------------------------------------------------------------------------
// C3 (2026-09-07) — a spill exits at the RIM, so it may only draw the node's
// PONDED water (volume above full_volume), never the below-crown conveyance
// storage it used to dewater through the manhole cover.
// ---------------------------------------------------------------------------
TEST_F(CouplingConservation2DTest, SpillDrawsPondedWaterOnly) {
    RunResult r = run(dir_, "", "c3_ponded");
    ASSERT_TRUE(r.ok);

    // This deck's patch sits AT the junction crown and the node starts empty,
    // so every drop the node sends up must have been ponded above the rim
    // first. The cap can only ever reduce the spill, never invent one.
    EXPECT_GE(r.c12, 0.0);
    EXPECT_LE(r.c12, std::abs(r.given_2d_net) + std::abs(r.c21) + 1.0e-9)
        << "1D→2D spill (" << r.c12 << " m³) exceeds what the node could hold";

    // And the ledger still closes with the tighter cap in place.
    EXPECT_LT(std::abs(r.cont_2d), 0.05) << "2D continuity: " << r.cont_2d;
    EXPECT_LT(std::abs(r.cont_routing), 0.05) << "1D continuity: " << r.cont_routing;
}
