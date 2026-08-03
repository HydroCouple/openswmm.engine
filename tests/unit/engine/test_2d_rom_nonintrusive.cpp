/**
 * @file test_2d_rom_nonintrusive.cpp
 * @brief The surface uncertainty ROM must not perturb the deterministic run.
 *
 * @details The ROM is a read-only observer: it runs an M-member deviation-form
 *          ensemble alongside the deterministic marcher, reading mesh geometry
 *          and post-step state and writing only its own buffers. This test is
 *          the guard on that contract.
 *
 *          The same coupled 1D/2D model is run twice through the public C API —
 *          once with the ROM off, once with it on — and every deterministic
 *          quantity is compared with `==`, not a tolerance. Bit-identity is the
 *          right bar and an achievable one: if the ROM never writes mesh or
 *          state, the deterministic arithmetic is unchanged instruction for
 *          instruction. Anything looser would hide exactly the kind of feedback
 *          this test exists to catch, and the engine is held to elementwise
 *          float32 parity with the legacy solver, so a "small" perturbation from
 *          an optional sidecar is not acceptable.
 *
 *          The second half matters as much as the first: a bit-identity test
 *          passes trivially if the ROM silently never ran. So the ROM-on run
 *          also asserts the ensemble was built, seeded, and produced ordered
 *          per-cell quantiles with real spread.
 *
 *          Needs the full 2D module (OPENSWMM_BUILD_2D). Inputs and outputs are
 *          written to ./rom_nonintrusive_out/ (working dir is
 *          tests/unit/engine/data) for review — no temp files, per project
 *          convention.
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_nodes.h>

#include "core/SWMMEngine.hpp"

namespace fs = std::filesystem;

namespace {

// A sloped N×N patch draining into coupled junction J1. Sloped, not flat: the
// ROM's Manning-sensitivity forcing is proportional to b_j = P[:,j]ᵀ·h_det, so a
// perfectly uniform depth field projects to ~0 on every retained (zero-mean)
// eigenmode and the ensemble would carry no spread — the test would then assert
// bit-identity against a ROM that was running but doing nothing.
constexpr int    kN        = 4;      // 4×4 quads → 32 triangles
constexpr double kExtent   = 12.0;   // m
constexpr double kCrown    = 1.0;    // m — J1's crown; patch drops away from it

std::string build_model() {
    std::string s =
        "[OPTIONS]\n"
        "FLOW_UNITS           CMS\n"
        "FLOW_ROUTING         DYNWAVE\n"
        "START_DATE           01/01/2026\n"
        "START_TIME           00:00:00\n"
        "END_DATE             01/01/2026\n"
        "END_TIME             00:20:00\n"
        "REPORT_STEP          00:01:00\n"
        "ROUTING_STEP         6\n"
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
        "[RAINGAGES]\n"
        ";;Name  Format     Interval  SCF  Source\n"
        "RG1     INTENSITY  0:05      1.0  TIMESERIES TS1\n"
        "\n"
        "[TIMESERIES]\n"
        ";;Name  Time   Value (mm/hr)\n"
        "TS1     0:00   60.0\n"
        "TS1     0:10   60.0\n"
        "TS1     0:11   0.0\n"
        "TS1     1:00   0.0\n"
        "\n"
        "[2D_OPTIONS]\n"
        "MAX_TIMESTEP     4\n"
        "DRY_DEPTH        0.002\n"
        "COUPLING_CD      0.7\n"
        "REPORT_2D        NO\n"
        "\n"
        "[2D_VERTICES]\n"
        ";;X      Y      Z\n";

    char buf[128];
    const double d = kExtent / kN;
    for (int i = 0; i <= kN; ++i) {
        for (int j = 0; j <= kN; ++j) {
            // Bed falls away from the J1 corner in both directions, so the
            // ponded depth field is genuinely non-uniform.
            const double z = kCrown - 0.02 * (i + j);
            std::snprintf(buf, sizeof buf, "%8.3f %8.3f %8.4f\n",
                          j * d, i * d, z);
            s += buf;
        }
    }

    s += "\n[2D_TRIANGLES]\n;;V1  V2  V3  MANNINGS_N\n";
    for (int i = 0; i < kN; ++i) {
        for (int j = 0; j < kN; ++j) {
            const int v00 = i * (kN + 1) + j,       v01 = i * (kN + 1) + j + 1;
            const int v10 = (i + 1) * (kN + 1) + j, v11 = (i + 1) * (kN + 1) + j + 1;
            std::snprintf(buf, sizeof buf, "%d %d %d 0.03\n", v00, v01, v11);
            s += buf;
            std::snprintf(buf, sizeof buf, "%d %d %d 0.03\n", v00, v11, v10);
            s += buf;
        }
    }

    s += "\n[2D_VERTEX_NODE_MAP]\n;;Vertex  Node  Cd   Area\n0  J1  0.7  1.0\n";
    return s;
}

/// Everything the deterministic side produced, sampled every routing step.
/// Compared with `==` between the two runs.
struct Trajectory {
    bool ok = false;
    std::vector<double> elapsed;      // days
    std::vector<double> surface_vol;  // m³
    std::vector<double> node_head;    // project units
    std::vector<double> exchange;     // m³/s
    double cont_2d = 0.0;
    double cont_routing = 0.0;

    // ROM-side observations (only populated on the ROM-on run).
    bool   rom_ready      = false;
    int    rom_n_cells    = 0;
    int    rom_n_modes    = 0;
    double rom_max_spread = 0.0;
    bool   rom_quantiles_ordered = true;
};

Trajectory run(const fs::path& dir, bool enable_rom) {
    Trajectory tr;
    const std::string tag = enable_rom ? "rom_on" : "rom_off";
    const fs::path inp = dir / ("rom_nonintrusive_" + tag + ".inp");
    const fs::path rpt = dir / ("rom_nonintrusive_" + tag + ".rpt");
    const fs::path out = dir / ("rom_nonintrusive_" + tag + ".out");
    { std::ofstream f(inp); f << build_model(); }

    SWMM_Engine eng = swmm_engine_create();
    if (swmm_engine_open(eng, inp.string().c_str(), rpt.string().c_str(),
                         out.string().c_str(), nullptr) != SWMM_OK) {
        swmm_engine_destroy(eng);
        return tr;
    }

    // The [2D_ROM] input section is not wired on this branch yet, so the ROM is
    // switched on through the C++ object directly. open() has parsed and
    // populated the router's options; initialize() is what consumes them and
    // builds the ensemble, so this window is the correct place to set it.
    auto* impl = static_cast<openswmm::SWMMEngine*>(eng);
    if (enable_rom) {
        auto& opts = impl->surfaceRouter2D().options();
        opts.enable_rom          = true;
        opts.rom_members         = 24;
        opts.rom_modes           = 6;
        opts.rom_mannings_pert   = 0.20;
        opts.rom_rainfall_pert   = 0.20;
    }

    if (swmm_engine_initialize(eng) != SWMM_OK) {
        swmm_engine_close(eng); swmm_engine_destroy(eng);
        return tr;
    }
    int active = 0;
    swmm_2d_is_active(eng, &active);
    const int j1 = swmm_node_index(eng, "J1");
    if (!active || j1 < 0) {
        swmm_engine_close(eng); swmm_engine_destroy(eng);
        return tr;
    }
    if (swmm_engine_start(eng, 1) != SWMM_OK) {
        swmm_engine_close(eng); swmm_engine_destroy(eng);
        return tr;
    }

    double elapsed = 0.0;
    while (true) {
        if (swmm_engine_step(eng, &elapsed) != SWMM_OK || elapsed <= 0.0) break;
        tr.elapsed.push_back(elapsed);

        double vol = 0.0;
        swmm_2d_get_total_volume(eng, &vol);
        tr.surface_vol.push_back(vol);

        double head = 0.0;
        swmm_node_get_head(eng, j1, &head);
        tr.node_head.push_back(head);

        double exch = 0.0;
        swmm_2d_get_total_exchange_flow(eng, &exch);
        tr.exchange.push_back(exch);
    }

    swmm_engine_end(eng);
    swmm_2d_get_continuity_error(eng, &tr.cont_2d);
    swmm_get_routing_continuity_error(eng, &tr.cont_routing);

    // Observe the ensemble before the engine tears it down.
    if (enable_rom) {
        const auto* rom = impl->surfaceRouter2D().rom();
        if (rom && rom->is_ready()) {
            tr.rom_ready   = true;
            tr.rom_n_cells = rom->n_tri;
            tr.rom_n_modes = rom->n_kept;
            const std::size_t n = rom->q05.size();
            for (std::size_t c = 0; c < n; ++c) {
                if (!(rom->q05[c] <= rom->q50[c] && rom->q50[c] <= rom->q95[c]))
                    tr.rom_quantiles_ordered = false;
                tr.rom_max_spread =
                    std::max(tr.rom_max_spread, rom->q95[c] - rom->q05[c]);
            }
        }
    }

    swmm_engine_report(eng);
    swmm_engine_close(eng);
    swmm_engine_destroy(eng);
    tr.ok = true;
    return tr;
}

}  // namespace

// ============================================================================

TEST(SurfaceROMNonIntrusive, DeterministicRunIsBitIdenticalWithROMOnOrOff) {
    const fs::path dir = fs::current_path() / "rom_nonintrusive_out";
    fs::create_directories(dir);

    const Trajectory off = run(dir, /*enable_rom=*/false);
    const Trajectory on  = run(dir, /*enable_rom=*/true);

    ASSERT_TRUE(off.ok) << "ROM-off run failed";
    ASSERT_TRUE(on.ok)  << "ROM-on run failed";
    ASSERT_GT(off.elapsed.size(), 10u) << "run too short to be meaningful";

    // --- The ROM genuinely ran. Without this the comparison below is vacuous.
    EXPECT_TRUE(on.rom_ready) << "ROM never became ready — bit-identity would "
                                 "then prove nothing";
    EXPECT_EQ(on.rom_n_cells, 2 * kN * kN);
    EXPECT_GT(on.rom_n_modes, 0);
    EXPECT_TRUE(on.rom_quantiles_ordered) << "q05 <= q50 <= q95 violated";
    EXPECT_GT(on.rom_max_spread, 0.0)
        << "ensemble produced no spread anywhere — the ROM was allocated but "
           "carried no uncertainty, so this test would not be exercising it";

    // --- The deterministic run is untouched, bit for bit.
    ASSERT_EQ(off.elapsed.size(), on.elapsed.size())
        << "step count changed: the ROM altered the deterministic timestepping";

    for (std::size_t i = 0; i < off.elapsed.size(); ++i) {
        ASSERT_EQ(off.elapsed[i], on.elapsed[i])
            << "elapsed time diverged at step " << i;
        ASSERT_EQ(off.surface_vol[i], on.surface_vol[i])
            << "2D surface volume diverged at step " << i;
        ASSERT_EQ(off.node_head[i], on.node_head[i])
            << "J1 head diverged at step " << i;
        ASSERT_EQ(off.exchange[i], on.exchange[i])
            << "coupling exchange diverged at step " << i;
    }

    EXPECT_EQ(off.cont_2d, on.cont_2d);
    EXPECT_EQ(off.cont_routing, on.cont_routing);
}

TEST(SurfaceROMNonIntrusive, BinaryOutputIsByteIdenticalWithROMOnOrOff) {
    // The trajectory comparison above samples the state the C API exposes; this
    // compares the whole reported result file, so anything the ROM perturbed
    // that the sampled quantities happen to miss still fails.
    const fs::path dir = fs::current_path() / "rom_nonintrusive_out";
    const fs::path a = dir / "rom_nonintrusive_rom_off.out";
    const fs::path b = dir / "rom_nonintrusive_rom_on.out";

    ASSERT_TRUE(fs::exists(a)) << a << " missing — run the trajectory test first";
    ASSERT_TRUE(fs::exists(b)) << b << " missing — run the trajectory test first";
    ASSERT_EQ(fs::file_size(a), fs::file_size(b)) << "output sizes differ";

    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    ASSERT_TRUE(fa && fb);

    constexpr std::size_t kChunk = 64 * 1024;
    std::vector<char> ba(kChunk), bb(kChunk);
    std::size_t offset = 0;
    while (fa && fb) {
        fa.read(ba.data(), static_cast<std::streamsize>(kChunk));
        fb.read(bb.data(), static_cast<std::streamsize>(kChunk));
        const auto na = fa.gcount(), nb = fb.gcount();
        ASSERT_EQ(na, nb);
        if (na == 0) break;
        for (std::streamsize i = 0; i < na; ++i) {
            ASSERT_EQ(ba[static_cast<std::size_t>(i)],
                      bb[static_cast<std::size_t>(i)])
                << "output files differ at byte " << (offset + static_cast<std::size_t>(i));
        }
        offset += static_cast<std::size_t>(na);
    }
}
