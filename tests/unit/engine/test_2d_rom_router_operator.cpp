/**
 * @file test_2d_rom_router_operator.cpp
 * @brief SurfaceRouter2D's wiring of the calibrated reduced deviation
 *        operator: the production default, the legacy fallback, and
 *        open-boundary grounding — as driven through the real engine
 *        lifecycle, not by constructing SpectralROM/DeviationOperator2D
 *        directly (that level is covered by test_engine_2d_deviation_operator
 *        and the marcher-MC validation in
 *        tests/regression/test_2d_rom_marcher_coverage.cpp).
 *
 * @details What this file guards, that those two do not:
 *          - SolverOptions2D's rom_alpha_par/alpha_perp/c_factor/ground_scale
 *            defaults match the calibrated values VALIDATION.md records —
 *            catches accidental drift without re-running the ~100 s MC harness.
 *          - With those defaults, SurfaceRouter2D actually installs the
 *            reduced operator on rom() and leaves romBasis() un-depth-weighted
 *            (the configuration the calibration was measured against).
 *          - options_.rom_legacy_operator = true actually reverts to the
 *            pre-W3 path: diagonal decay, depth-weighted basis.
 *          - Open-boundary grounding is wired from the router's own
 *            boundary_ (populated from [2D_BOUNDARY_CONDITIONS]), not merely
 *            from mesh geometry: a fully-WALL model and an otherwise-
 *            identical model with one NORMAL_FLOW edge must produce
 *            different bases (the grounded one loses its null mode).
 *
 * @ingroup engine_2d
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_forcing.h>
#include <openswmm/engine/openswmm_nodes.h>

#include "2d/data/SolverOptions2D.hpp"
#include "core/SWMMEngine.hpp"

namespace fs = std::filesystem;

namespace {

// Sloped 4×4 patch draining into coupled junction J1 (mirrors
// test_2d_rom_nonintrusive.cpp's fixture) — non-uniform depth so the
// Manning-sensitivity forcing is nonzero and the ROM carries real spread.
// No [2D_BOUNDARY_CONDITIONS] here: every boundary edge defaults to WALL, so
// this fixture's own grounding vector is all-zero (closed box). The grounding
// case below adds one open edge on top of this same geometry.
constexpr int    kN      = 4;
constexpr double kExtent = 12.0;
constexpr double kCrown  = 1.0;

std::string buildModel(bool with_open_boundary) {
    std::string s =
        "[OPTIONS]\n"
        "FLOW_UNITS           CMS\n"
        "FLOW_ROUTING         DYNWAVE\n"
        "START_DATE           01/01/2026\n"
        "START_TIME           00:00:00\n"
        "END_DATE             01/01/2026\n"
        "END_TIME             00:10:00\n"
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
    for (int i = 0; i <= kN; ++i)
        for (int j = 0; j <= kN; ++j) {
            const double z = kCrown - 0.02 * (i + j);
            std::snprintf(buf, sizeof buf, "%8.3f %8.3f %8.4f\n",
                         j * d, i * d, z);
            s += buf;
        }

    s += "\n[2D_TRIANGLES]\n;;V1  V2  V3  MANNINGS_N\n";
    for (int i = 0; i < kN; ++i)
        for (int j = 0; j < kN; ++j) {
            const int v00 = i * (kN + 1) + j,       v01 = i * (kN + 1) + j + 1;
            const int v10 = (i + 1) * (kN + 1) + j, v11 = (i + 1) * (kN + 1) + j + 1;
            std::snprintf(buf, sizeof buf, "%d %d %d 0.03\n", v00, v01, v11);
            s += buf;
            std::snprintf(buf, sizeof buf, "%d %d %d 0.03\n", v00, v11, v10);
            s += buf;
        }

    if (with_open_boundary) {
        // Verified against the actual mesh topology (scratch probe, not
        // guessed): triangle 0's local edge 2 has nbr=-1 (true domain
        // boundary, midpoint y=0 — the v0-v1 edge along the bottom row).
        // Local edges 0 and 1 of triangle 0 are both interior (shared with
        // triangles 3 and 1 respectively).
        s += "\n[2D_BOUNDARY_CONDITIONS]\n"
             ";;TRI  EDGE  TYPE         SLOPE\n"
             "0      2     NORMAL_FLOW  0.02\n";
    }

    s += "\n[2D_VERTEX_NODE_MAP]\n;;Vertex  Node  Cd   Area\n0  J1  0.7  1.0\n";
    return s;
}

// Drives one run, applying `configure` to the router's options between open()
// and initialize() — the window the port's option toggles all use, since the
// [2D_ROM] input section is not wired yet. Returns the live engine handle
// (caller must swmm_engine_close/destroy) so tests can inspect rom()/
// romBasis() after stepping. Null on any lifecycle failure.
SWMM_Engine driveRun(const fs::path& inp_path, const fs::path& rpt_path,
                    const fs::path& out_path,
                    const std::function<void(openswmm::twoD::SolverOptions2D&)>& configure) {
    SWMM_Engine eng = swmm_engine_create();
    if (swmm_engine_open(eng, inp_path.string().c_str(), rpt_path.string().c_str(),
                         out_path.string().c_str(), nullptr) != SWMM_OK) {
        swmm_engine_destroy(eng);
        return nullptr;
    }
    auto* impl = static_cast<openswmm::SWMMEngine*>(eng);
    configure(impl->surfaceRouter2D().options());

    if (swmm_engine_initialize(eng) != SWMM_OK) {
        swmm_engine_close(eng); swmm_engine_destroy(eng);
        return nullptr;
    }
    int active = 0;
    swmm_2d_is_active(eng, &active);
    if (!active) {
        swmm_engine_close(eng); swmm_engine_destroy(eng);
        return nullptr;
    }
    if (swmm_engine_start(eng, 1) != SWMM_OK) {
        swmm_engine_close(eng); swmm_engine_destroy(eng);
        return nullptr;
    }

    double elapsed = 0.0;
    int n_steps = 0;
    while (swmm_engine_step(eng, &elapsed) == SWMM_OK && elapsed > 0.0) {
        if (++n_steps > 400) break;   // safety valve; the fixture ends ~10 min
    }
    return eng;
}

}  // namespace

// ============================================================================

TEST(SurfaceROMRouterOperator, DefaultOptionsMatchCalibratedDials) {
    // Guards against silent drift of the constants VALIDATION.md's "Solver-
    // mode compatibility" section records as calibrated against brute-force
    // marcher Monte Carlo (coverage 1.000, width-med 1.43, in-band 0.884).
    // Changing these without re-running that harness invalidates the record.
    const openswmm::twoD::SolverOptions2D defaults;
    EXPECT_FALSE(defaults.rom_legacy_operator);
    EXPECT_DOUBLE_EQ(defaults.rom_alpha_par, 0.62);
    EXPECT_DOUBLE_EQ(defaults.rom_alpha_perp, 2.00);
    EXPECT_DOUBLE_EQ(defaults.rom_c_factor, 5.0 / 3.0);
    EXPECT_DOUBLE_EQ(defaults.rom_ground_scale, 0.25);
}

TEST(SurfaceROMRouterOperator, DefaultPathInstallsReducedOperatorOnUndepthWeightedBasis) {
    const fs::path dir = fs::current_path() / "rom_router_operator_out";
    fs::create_directories(dir);
    { std::ofstream f(dir / "default.inp"); f << buildModel(/*open=*/false); }

    auto eng = driveRun(dir / "default.inp", dir / "default.rpt", dir / "default.out",
                        [](openswmm::twoD::SolverOptions2D& o) {
                            o.enable_rom        = true;
                            o.rom_members       = 20;
                            o.rom_modes         = 6;
                            o.rom_mannings_pert = 0.20;
                            o.rom_rainfall_pert = 0.20;
                            // rom_legacy_operator left at its default (false).
                        });
    ASSERT_NE(eng, nullptr);
    auto* impl = static_cast<openswmm::SWMMEngine*>(eng);
    const auto& router = impl->surfaceRouter2D();

    ASSERT_NE(router.rom(), nullptr);
    ASSERT_TRUE(router.rom()->is_ready());
    EXPECT_TRUE(router.rom()->hasReducedOperator())
        << "default options must install the calibrated reduced operator";

    ASSERT_NE(router.romBasis(), nullptr);
    EXPECT_FALSE(router.romBasis()->depth_weighted)
        << "the reduced operator applies its own depth weighting at "
           "assembly time; rebuilding the basis on top of that would leave "
           "the W3-calibrated configuration";

    swmm_engine_end(eng);
    swmm_engine_report(eng);
    swmm_engine_close(eng);
    swmm_engine_destroy(eng);
}

TEST(SurfaceROMRouterOperator, SpatialManningFieldInstallsPerMemberOperators) {
    // PR H15: MANNINGS_CORR_LEN > 0 generates a per-member spatial Manning
    // field, which used to drop the ROM onto the diagonal path silently. The
    // router must now assemble one reduced operator per member so the field
    // rides the production operator.
    const fs::path dir = fs::current_path() / "rom_router_operator_out";
    fs::create_directories(dir);
    { std::ofstream f(dir / "spatial_mann.inp"); f << buildModel(/*open=*/false); }

    auto eng = driveRun(dir / "spatial_mann.inp", dir / "spatial_mann.rpt",
                        dir / "spatial_mann.out",
                        [](openswmm::twoD::SolverOptions2D& o) {
                            o.enable_rom            = true;
                            o.rom_members           = 12;
                            o.rom_modes             = 6;
                            o.rom_mannings_pert     = 0.20;
                            o.rom_rainfall_pert     = 0.0;
                            o.rom_mannings_corr_len = 1.5;   // metres; fixture is a few m wide
                        });
    ASSERT_NE(eng, nullptr);
    auto* impl = static_cast<openswmm::SWMMEngine*>(eng);
    const auto& router = impl->surfaceRouter2D();
    ASSERT_NE(router.rom(), nullptr);
    ASSERT_TRUE(router.rom()->spatial_mannings.is_spatial())
        << "fixture precondition: the correlation length must produce a field";
    EXPECT_TRUE(router.rom()->hasReducedOperator());
    EXPECT_TRUE(router.rom()->hasPerMemberOperators())
        << "a spatial Manning field must get per-member reduced operators (H15)";
    swmm_engine_end(eng);
    swmm_engine_report(eng);
    swmm_engine_close(eng);
    swmm_engine_destroy(eng);
}

TEST(SurfaceROMRouterOperator, ManningElasticityOptionReachesTheRom) {
    const fs::path dir = fs::current_path() / "rom_router_operator_out";
    fs::create_directories(dir);
    { std::ofstream f(dir / "elasticity.inp"); f << buildModel(/*open=*/false); }
    auto eng = driveRun(dir / "elasticity.inp", dir / "elasticity.rpt", dir / "elasticity.out",
                        [](openswmm::twoD::SolverOptions2D& o) {
                            o.enable_rom = true; o.rom_members = 12; o.rom_modes = 6;
                            o.rom_mannings_pert = 0.20; o.rom_rainfall_pert = 0.0;
                            o.rom_manning_elasticity = 0.6;
                        });
    ASSERT_NE(eng, nullptr);
    auto* impl = static_cast<openswmm::SWMMEngine*>(eng);
    ASSERT_NE(impl->surfaceRouter2D().rom(), nullptr);
    EXPECT_DOUBLE_EQ(impl->surfaceRouter2D().rom()->manning_elasticity, 0.6);
    swmm_engine_end(eng); swmm_engine_report(eng);
    swmm_engine_close(eng); swmm_engine_destroy(eng);
}

TEST(SurfaceROMRouterOperator, RelativeDeviationAppliesOnlyOnAGroundedBasis) {
    // Relative reconstruction is the default (2026-10-11), but it needs the
    // constant mode, which only a GROUNDED basis carries. Open boundary:
    // relative. Closed box: the router must fall back to the absolute form,
    // and the band must keep its Manning spread.
    //
    // Measured 2026-10-11 on this fixture: open (relative) 1.58e-3; closed
    // with the fallback (absolute) 6.20e-3; closed with relative FORCED
    // 2.42e-5 — a 256x collapse a "> 0" check cannot see. So the closed box
    // is held to a tenth of the open box's spread.
    const fs::path dir = fs::current_path() / "rom_router_operator_out";
    fs::create_directories(dir);
    auto configure = [](openswmm::twoD::SolverOptions2D& o) {
        o.enable_rom = true; o.rom_members = 12; o.rom_modes = 6;
        o.rom_mannings_pert = 0.20; o.rom_rainfall_pert = 0.0;
    };
    double spread_open = 0.0, spread_closed = 0.0;
    for (const bool open : {true, false}) {
        const std::string tag = open ? "relative_open" : "relative_closed";
        { std::ofstream f(dir / (tag + ".inp")); f << buildModel(open); }
        auto eng = driveRun(dir / (tag + ".inp"), dir / (tag + ".rpt"),
                            dir / (tag + ".out"), configure);
        ASSERT_NE(eng, nullptr) << tag;
        auto* impl = static_cast<openswmm::SWMMEngine*>(eng);
        const auto* rom = impl->surfaceRouter2D().rom();
        ASSERT_NE(rom, nullptr) << tag;
        EXPECT_EQ(rom->relative_deviation, open) << tag;
        double max_spread = 0.0;
        for (std::size_t c = 0; c < rom->q05.size(); ++c)
            max_spread = std::max(max_spread, rom->q95[c] - rom->q05[c]);
        (open ? spread_open : spread_closed) = max_spread;
        swmm_engine_end(eng); swmm_engine_report(eng);
        swmm_engine_close(eng); swmm_engine_destroy(eng);
    }
    EXPECT_GT(spread_open, 0.0);
    EXPECT_GE(spread_closed, 0.1 * spread_open)
        << "closed-box band collapsed: open " << spread_open
        << " vs closed " << spread_closed;
}

TEST(SurfaceROMRouterOperator, LegacyOptionRestoresDiagonalPathWithDepthWeightedBasis) {
    const fs::path dir = fs::current_path() / "rom_router_operator_out";
    fs::create_directories(dir);
    { std::ofstream f(dir / "legacy.inp"); f << buildModel(/*open=*/false); }

    auto eng = driveRun(dir / "legacy.inp", dir / "legacy.rpt", dir / "legacy.out",
                        [](openswmm::twoD::SolverOptions2D& o) {
                            o.enable_rom          = true;
                            o.rom_members         = 20;
                            o.rom_modes           = 6;
                            o.rom_mannings_pert   = 0.20;
                            o.rom_rainfall_pert   = 0.20;
                            o.rom_legacy_operator = true;
                        });
    ASSERT_NE(eng, nullptr);
    auto* impl = static_cast<openswmm::SWMMEngine*>(eng);
    const auto& router = impl->surfaceRouter2D();

    ASSERT_NE(router.rom(), nullptr);
    ASSERT_TRUE(router.rom()->is_ready());
    EXPECT_FALSE(router.rom()->hasReducedOperator())
        << "rom_legacy_operator must keep advance() on the diagonal path";

    ASSERT_NE(router.romBasis(), nullptr);
    EXPECT_TRUE(router.romBasis()->depth_weighted)
        << "the legacy path refits the eigenbasis to depth at every seed, "
           "as it did before W3";

    swmm_engine_end(eng);
    swmm_engine_report(eng);
    swmm_engine_close(eng);
    swmm_engine_destroy(eng);
}

TEST(SurfaceROMRouterOperator, GroundingEnginesOnlyWithAnOpenBoundaryEdge) {
    // Same geometry both times; the only difference is one NORMAL_FLOW edge.
    // Grounding removes the Laplacian's true null mode (the constant vector),
    // so num_null must drop from >=1 (closed box) to 0 (grounded) — the same
    // structural signature test_engine_2d_mesh_eigen_basis's
    // GroundedBasisCapturesUniformShift measures directly on MeshEigenBasis,
    // here confirmed to actually be reachable through the router's own
    // boundary_ (populated from [2D_BOUNDARY_CONDITIONS]), not just in theory.
    const fs::path dir = fs::current_path() / "rom_router_operator_out";
    fs::create_directories(dir);

    auto configure = [](openswmm::twoD::SolverOptions2D& o) {
        o.enable_rom        = true;
        o.rom_members       = 4;    // irrelevant to this check; keep it cheap
        o.rom_modes         = 6;
    };

    { std::ofstream f(dir / "closed.inp"); f << buildModel(/*open=*/false); }
    auto eng_closed = driveRun(dir / "closed.inp", dir / "closed.rpt",
                              dir / "closed.out", configure);
    ASSERT_NE(eng_closed, nullptr);
    auto* impl_closed = static_cast<openswmm::SWMMEngine*>(eng_closed);
    ASSERT_NE(impl_closed->surfaceRouter2D().romBasis(), nullptr);
    const int num_null_closed =
        impl_closed->surfaceRouter2D().romBasis()->num_null;
    swmm_engine_end(eng_closed);
    swmm_engine_close(eng_closed);
    swmm_engine_destroy(eng_closed);

    { std::ofstream f(dir / "open.inp"); f << buildModel(/*open=*/true); }
    auto eng_open = driveRun(dir / "open.inp", dir / "open.rpt",
                             dir / "open.out", configure);
    ASSERT_NE(eng_open, nullptr);
    auto* impl_open = static_cast<openswmm::SWMMEngine*>(eng_open);
    ASSERT_NE(impl_open->surfaceRouter2D().romBasis(), nullptr);
    const int num_null_open = impl_open->surfaceRouter2D().romBasis()->num_null;
    swmm_engine_end(eng_open);
    swmm_engine_close(eng_open);
    swmm_engine_destroy(eng_open);

    EXPECT_GE(num_null_closed, 1)
        << "fully-WALL fixture should still have the ungrounded null mode";
    EXPECT_EQ(num_null_open, 0)
        << "one NORMAL_FLOW edge should ground the basis and remove it";
}

TEST(SurfaceROMRouterOperator, WettedAreaGrowthDoesNotEraseTheBand) {
    // Settings audit (2026-10-12): on the shipped path the eigenbasis is fixed
    // geometry, so a wet-area reseed rebuilt nothing and only zeroed every
    // member's deviation. With the old trigger armed as aggressively as it
    // goes (no minimum interval), the band must be exactly the band with the
    // trigger off: reseeding is now confined to the legacy path, whose basis
    // actually changes.
    const fs::path dir = fs::current_path() / "rom_router_operator_out";
    fs::create_directories(dir);
    { std::ofstream f(dir / "reseed.inp"); f << buildModel(true); }
    std::vector<double> band[2];
    for (const bool armed : {false, true}) {
        auto configure = [armed](openswmm::twoD::SolverOptions2D& o) {
            o.enable_rom = true; o.rom_members = 12; o.rom_modes = 6;
            o.rom_mannings_pert = 0.20; o.rom_rainfall_pert = 0.0;
            o.rom_wet_reseed_fraction = armed ? 0.01 : 0.0;
            o.rom_wet_reseed_min_interval = 0.0;
        };
        auto eng = driveRun(dir / "reseed.inp", dir / "reseed.rpt", dir / "reseed.out", configure);
        ASSERT_NE(eng, nullptr);
        const auto* rom = static_cast<openswmm::SWMMEngine*>(eng)->surfaceRouter2D().rom();
        ASSERT_NE(rom, nullptr);
        for (std::size_t c = 0; c < rom->q05.size(); ++c)
            band[armed ? 1 : 0].push_back(rom->q95[c] - rom->q05[c]);
        swmm_engine_end(eng); swmm_engine_report(eng);
        swmm_engine_close(eng); swmm_engine_destroy(eng);
    }
    ASSERT_EQ(band[0].size(), band[1].size());
    double max_band = 0.0;
    for (std::size_t c = 0; c < band[0].size(); ++c) {
        EXPECT_EQ(band[1][c], band[0][c]) << "cell " << c;
        max_band = std::max(max_band, band[0][c]);
    }
    EXPECT_GT(max_band, 0.0);
}

TEST(SurfaceROMRouterOperator, AreaBasisHookSelectsTheBasisOnlyWhereTheRelativeFormRuns) {
    // PORT_HAMILTONIAN.md S-2 (temporary A/B hook rom_area_basis): the
    // A-orthonormal basis is built exactly where the two-channel relative
    // form runs (grounded domain); a closed box and hook=false keep the plain
    // orthonormal basis (and the volume Gram path).
    EXPECT_TRUE(openswmm::twoD::SolverOptions2D{}.rom_area_basis);
    const fs::path dir = fs::current_path() / "rom_router_operator_out";
    fs::create_directories(dir);
    for (const bool open : {true, false}) {
        for (const bool hook : {true, false}) {
            const std::string tag = std::string("areabasis_") + (open ? "open" : "closed") + (hook ? "_on" : "_off");
            { std::ofstream f(dir / (tag + ".inp")); f << buildModel(open); }
            auto eng = driveRun(dir / (tag + ".inp"), dir / (tag + ".rpt"), dir / (tag + ".out"),
                                [hook](openswmm::twoD::SolverOptions2D& o) {
                                    o.enable_rom = true; o.rom_members = 12; o.rom_modes = 6;
                                    o.rom_mannings_pert = 0.20; o.rom_rainfall_pert = 0.0;
                                    o.rom_area_basis = hook;
                                });
            ASSERT_NE(eng, nullptr) << tag;
            const auto& router = static_cast<openswmm::SWMMEngine*>(eng)->surfaceRouter2D();
            ASSERT_NE(router.romBasis(), nullptr) << tag;
            EXPECT_EQ(router.romBasis()->area_weighted, open && hook) << tag;
            double spread = 0.0;
            for (std::size_t c = 0; c < router.rom()->q05.size(); ++c)
                spread = std::max(spread, router.rom()->q95[c] - router.rom()->q05[c]);
            EXPECT_GT(spread, 0.0) << tag;
            swmm_engine_end(eng); swmm_engine_report(eng);
            swmm_engine_close(eng); swmm_engine_destroy(eng);
        }
    }
}
