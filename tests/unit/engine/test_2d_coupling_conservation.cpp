/**
 * @file test_2d_coupling_conservation.cpp
 * @brief Cross-domain volume conservation of 1D↔2D coupling under VARIABLE_STEP.
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
 *          (Σ lateral_inflow·dt) must equal the 2D-given net coupling volume
 *          (coupling_2d_to_1d_out − coupling_1d_to_2d_in from swmm_2d_get_mass_balance)
 *          to tight tolerance. Pre-fix this gap opens up with the timestep variation.
 *
 *          Needs the full 2D module (OPENSWMM_BUILD_2D); runs the real coupled
 *          engine through the public C API. Inputs and a summary are written to
 *          ./coupling_conservation_out/ (working dir is tests/unit/engine/data)
 *          for review — no temp files (project convention).
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_forcing.h>

namespace fs = std::filesystem;

namespace {

// Flat 10×10 m 2D patch whose bed sits at z = 1.0 m = the crown of coupled
// junction J1 (invert 0, MaxDepth 1.0). J1 drains through a 0.3 m pipe to free
// outfall O1 and has NO [INFLOWS] — its only lateral inflow is the 2D coupling.
// VARIABLE_STEP is ON so the routing dt varies step-to-step (amplified by the 2D
// CFL hint as the patch fills then drains), exercising the dt-robust carry.
std::string build_model() {
    return
        "[OPTIONS]\n"
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
        "LINEAR_SOLVER    GMRES\n"
        "PRECONDITIONER   JACOBI\n"
        "REPORT_2D        NO\n"
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
    bool   ok = false;
    double received_1d = 0.0;   // ∫ lateral_inflow(J1)·dt over the run (m³)
    double given_2d_net = 0.0;  // coupling_2d_to_1d_out − coupling_1d_to_2d_in (m³)
    double cont_2d = 0.0;
    double cont_routing = 0.0;
    double dt_min = 1e30;       // observed routing-step spread (s) — confirms the
    double dt_max = 0.0;        //   variable-dt path is actually exercised
    int    n_steps = 0;
};

RunResult run(const fs::path& dir) {
    RunResult r;
    const fs::path inp = dir / "coupling_conservation.inp";
    const fs::path rpt = dir / "coupling_conservation.rpt";
    const fs::path out = dir / "coupling_conservation.out";
    { std::ofstream f(inp); f << build_model(); }

    SWMM_Engine eng = swmm_engine_create();
    if (swmm_engine_open(eng, inp.string().c_str(), rpt.string().c_str(),
                         out.string().c_str(), nullptr) != SWMM_OK) {
        swmm_engine_destroy(eng); return r;
    }
    if (swmm_engine_initialize(eng) != SWMM_OK) {
        swmm_engine_close(eng); swmm_engine_destroy(eng); return r;
    }
    int active = 0;
    swmm_2d_is_active(eng, &active);
    const int j1 = swmm_node_index(eng, "J1");
    if (!active || j1 < 0) { swmm_engine_close(eng); swmm_engine_destroy(eng); return r; }
    if (swmm_engine_start(eng, 1) != SWMM_OK) {
        swmm_engine_close(eng); swmm_engine_destroy(eng); return r;
    }

    constexpr double RAIN_OFF_S = 600.0;     // rain the patch for the first 10 min
    constexpr double RAIN_RATE  = 0.001;     // m/s (fills above the crown, then drains)

    double elapsed = 0.0, prev_elapsed = 0.0;
    while (true) {
        // Drive the patch: constant rainfall for the first window, then off so the
        // ponded surface drains through the coupled inlet. OVERRIDE+PERSIST so it
        // holds across steps until changed.
        const double t_s = elapsed * 86400.0;
        const double rain = (t_s < RAIN_OFF_S) ? RAIN_RATE : 0.0;
        swmm_2d_force_rainfall_uniform(eng, rain, SWMM_FORCING_OVERRIDE, SWMM_FORCING_PERSIST);

        if (swmm_engine_step(eng, &elapsed) != SWMM_OK || elapsed <= 0.0) break;

        const double dt_s = (elapsed - prev_elapsed) * 86400.0;
        prev_elapsed = elapsed;
        if (dt_s > 0.0) {
            r.dt_min = std::min(r.dt_min, dt_s);
            r.dt_max = std::max(r.dt_max, dt_s);
            ++r.n_steps;
        }

        double lat = 0.0;  // project flow units (CMS = m³/s); + = into J1 (2D→1D drain)
        if (swmm_node_get_lateral_inflow(eng, j1, &lat) == SWMM_OK)
            r.received_1d += lat * dt_s;
    }

    swmm_engine_end(eng);

    double c12 = 0.0, c21 = 0.0;
    swmm_2d_get_mass_balance(eng, nullptr, nullptr, nullptr, &c12, &c21,
                             nullptr, nullptr, nullptr, nullptr, nullptr);
    r.given_2d_net = c21 - c12;   // net volume 2D → 1D (m³)
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
    void SetUp() override {
        dir_ = fs::path("coupling_conservation_out");
        fs::create_directories(dir_);
    }
};

// The exchange volume the 1D node receives must equal the volume the 2D side gave
// up, even though VARIABLE_STEP makes the consuming dt differ from the producing
// dt. Carrying the exchange as a per-window volume (the fix) makes these match;
// the old rate-based carry left a Σ Q·Δdt gap.
TEST_F(CouplingConservation2DTest, ReceivedEqualsGivenUnderVariableStep) {
    RunResult r = run(dir_);

    ASSERT_TRUE(r.ok) << "coupled conservation run failed";

    // The coupling must actually be exercised (patch wet, drained into J1).
    ASSERT_GT(std::abs(r.given_2d_net), 1.0)
        << "no meaningful 2D→1D exchange occurred (net " << r.given_2d_net << " m³)";

    // Cross-domain conservation: 1D-received ≈ 2D-given. The only admissible
    // discrepancy is the single-step boundary term (the last window's exchange,
    // ~0 once the patch has drained), so the match is tight.
    const double rel = std::abs(r.received_1d - r.given_2d_net)
                       / std::abs(r.given_2d_net);
    EXPECT_LT(rel, 0.005)
        << "1D-received (" << r.received_1d << " m³) != 2D-given ("
        << r.given_2d_net << " m³); rel err " << rel
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
    csv << "metric,value\n"
        << "received_1d_m3," << r.received_1d << "\n"
        << "given_2d_net_m3," << r.given_2d_net << "\n"
        << "rel_err," << rel << "\n"
        << "dt_min_s," << r.dt_min << "\n"
        << "dt_max_s," << r.dt_max << "\n"
        << "n_steps," << r.n_steps << "\n"
        << "continuity_2d_frac," << r.cont_2d << "\n"
        << "continuity_routing_frac," << r.cont_routing << "\n";
}
