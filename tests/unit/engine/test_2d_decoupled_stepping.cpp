/**
 * @file test_2d_decoupled_stepping.cpp
 * @brief Decoupled 1D/2D timesteps: per-step accumulated + interpolated exchange.
 *
 * @details 2026-07 decoupling plan. The 1D and 2D domains advance on their own
 *          clocks: the junction/outfall exchange is evaluated EVERY 1D routing
 *          step (live 1D heads vs the current 2D state), booked to the 1D node
 *          for next-step consumption, and accumulated per coupling point. The
 *          explicit marcher co-advances the surface across sync batches of
 *          clamp(MAX_TIMESTEP, routing_step, 60) with live in-marcher exchange
 *          and queue-spread delivery, so the two clocks meet without a
 *          macro-window.
 *
 *          Case: MarcherSyncBatchesConserve — cross-domain conservation across
 *          multi-step sync batches (exercises accumulators, delivery queue, and
 *          the withdrawal budget).
 *
 *          The three CVODE-era window cases (IntervalMapsToTimeWindow,
 *          ExplicitLargeWindow, FailedWindowsRedeliver) retired with the
 *          implicit solvers and their window machinery — see the D2 retirement.
 *
 *          Outputs land in ./decoupled_stepping_out/ (working dir is
 *          tests/unit/engine/data) for review — no temp files.
 */

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_forcing.h>

namespace fs = std::filesystem;

namespace {

// Same flat 10×10 m patch over coupled junction J1 as
// test_2d_coupling_conservation.cpp, with the 2D advance cadence injected per
// case. Rain wets the patch for 10 min; it then drains through the coupled
// inlet into J1 → C1 → free outfall O1.
std::string build_model(const std::string& coupling_lines) {
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
        "MAX_TIMESTEP     30\n"
        "DRY_DEPTH        0.002\n"
        "COUPLING_CD      0.7\n"
        "REPORT_2D        NO\n"
        + coupling_lines +
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
    double received_1d  = 0.0;  // ∫ lateral_inflow(J1)·dt over the run (m³)
    double given_2d_net = 0.0;  // coupling_2d_to_1d_out − coupling_1d_to_2d_in (m³)
    double rain_in_2d   = 0.0;  // ledger rainfall_in (m³)
    double final_storage_2d = 0.0;
    double cont_2d      = 0.0;
    double cont_routing = 0.0;
    int    n_steps      = 0;
};

RunResult run(const fs::path& dir, const std::string& tag,
              const std::string& coupling_lines) {
    RunResult r;
    const fs::path inp = dir / (tag + ".inp");
    const fs::path rpt = dir / (tag + ".rpt");
    const fs::path out = dir / (tag + ".out");
    { std::ofstream f(inp); f << build_model(coupling_lines); }

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

    constexpr double RAIN_OFF_S = 600.0;
    constexpr double RAIN_RATE  = 0.001;   // m/s over 100 m² → 60 m³ total

    double elapsed = 0.0, prev_elapsed = 0.0;
    while (true) {
        const double t_s  = elapsed * 86400.0;
        const double rain = (t_s < RAIN_OFF_S) ? RAIN_RATE : 0.0;
        swmm_2d_force_rainfall_uniform(eng, rain, SWMM_FORCING_OVERRIDE,
                                       SWMM_FORCING_PERSIST);

        if (swmm_engine_step(eng, &elapsed) != SWMM_OK || elapsed <= 0.0) break;
        const double dt_s = (elapsed - prev_elapsed) * 86400.0;
        prev_elapsed = elapsed;
        if (dt_s > 0.0) ++r.n_steps;

        double lat = 0.0;   // CMS; + = into J1 (2D→1D drain)
        if (swmm_node_get_lateral_inflow(eng, j1, &lat) == SWMM_OK)
            r.received_1d += lat * dt_s;
    }

    swmm_engine_end(eng);

    double c12 = 0.0, c21 = 0.0;
    swmm_2d_get_mass_balance(eng, nullptr, &r.final_storage_2d, &r.rain_in_2d,
                             &c12, &c21, nullptr, nullptr, nullptr, nullptr,
                             nullptr);
    r.given_2d_net = c21 - c12;
    swmm_2d_get_continuity_error(eng, &r.cont_2d);
    swmm_get_routing_continuity_error(eng, &r.cont_routing);
    swmm_engine_report(eng);
    swmm_engine_close(eng);
    swmm_engine_destroy(eng);
    r.ok = true;
    return r;
}

void writeCsv(const fs::path& dir, const std::string& tag, const RunResult& r,
              double rel) {
    std::ofstream csv(dir / (tag + "_summary.csv"));
    csv << "metric,value\n"
        << "received_1d_m3," << r.received_1d << "\n"
        << "given_2d_net_m3," << r.given_2d_net << "\n"
        << "rel_err," << rel << "\n"
        << "rain_in_2d_m3," << r.rain_in_2d << "\n"
        << "final_storage_2d_m3," << r.final_storage_2d << "\n"
        << "continuity_2d_frac," << r.cont_2d << "\n"
        << "continuity_routing_frac," << r.cont_routing << "\n"
        << "n_steps," << r.n_steps << "\n";
}

} // namespace

class DecoupledStepping2DTest : public ::testing::Test {
protected:
    fs::path dir_;
    void SetUp() override {
        dir_ = fs::path("decoupled_stepping_out");
        fs::create_directories(dir_);
    }
};

// The windowless co-advance batches the 2D across sync spans of
// clamp(MAX_TIMESTEP=30 s, routing_step, 60) with live in-marcher exchange and
// queue-spread delivery, while the 1D takes many variable steps inside each
// batch. The cross-domain ledger property: what the 1D receives must equal what
// the 2D gave. The positivity limiter additionally makes negative final storage
// structurally impossible, and the withdrawal budget keeps the 2D from being
// overdrawn. No INTEGRATOR line — the marcher is the default (and only) 2D
// integrator, so this also gates that default.
TEST_F(DecoupledStepping2DTest, MarcherSyncBatchesConserve) {
    RunResult r = run(dir_, "marcher_batches", "");
    ASSERT_TRUE(r.ok);
    ASSERT_GT(std::abs(r.given_2d_net), 1.0)
        << "no meaningful exchange (net " << r.given_2d_net << " m³)";

    const double rel = std::abs(r.received_1d - r.given_2d_net)
                       / std::abs(r.given_2d_net);
    EXPECT_LT(rel, 0.01)
        << "1D-received " << r.received_1d << " m³ != 2D-given "
        << r.given_2d_net << " m³ across marcher sync batches";

    EXPECT_GE(r.final_storage_2d, 0.0)
        << "marcher positivity violated: negative 2D storage "
        << r.final_storage_2d << " m³";
    EXPECT_LT(std::abs(r.cont_2d), 0.05)      << "2D continuity " << r.cont_2d;
    EXPECT_LT(std::abs(r.cont_routing), 0.35) << "1D continuity " << r.cont_routing;

    writeCsv(dir_, "marcher_batches", r, rel);
}
