/**
 * @file test_2d_rom_input_parser.cpp
 * @brief `[2D_ROM]` and `[UNCERTAINTY]` input-section parsing
 *        (SectionHandlers2D::parse2DROMLine / parseUncertaintyLine).
 *
 * @details Before this file the surface uncertainty ROM was reachable only
 *          by mutating `SurfaceRouter2D::options()` in C++ between
 *          `swmm_engine_open()` and `swmm_engine_initialize()` — every other
 *          test in this port does exactly that, and still does for options
 *          [2D_ROM] doesn't cover. This test is the guard that an ordinary
 *          `.inp` file can drive the same configuration, including the
 *          `[UNCERTAINTY]` precedence rule (it overrides the matching
 *          `[2D_ROM]` scalar field) and the W3-calibrated reduced-operator
 *          dials `[2D_ROM]` newly exposes (LEGACY_OPERATOR/ALPHA_PAR/
 *          ALPHA_PERP/C_FACTOR/GROUND_SCALE).
 *
 * @ingroup engine_2d
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>

#include "core/SWMMEngine.hpp"
#include "uncertainty/UncertaintyTypes.hpp"

namespace fs = std::filesystem;

namespace {

// Minimal 4-triangle 2D-enabled model (no rainfall/coupling needed — this
// test only exercises parsing, not a run) plus whatever extra text the
// caller appends after [2D_TRIANGLES].
std::string buildModel(const std::string& extra_sections) {
    return
        "[OPTIONS]\n"
        "FLOW_UNITS           CMS\n"
        "FLOW_ROUTING         DYNWAVE\n"
        "START_DATE           01/01/2026\n"
        "START_TIME           00:00:00\n"
        "END_DATE             01/01/2026\n"
        "END_TIME             00:05:00\n"
        "REPORT_STEP          00:01:00\n"
        "ROUTING_STEP         6\n"
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
        "[2D_VERTICES]\n"
        ";;X    Y    Z\n"
        " 0.0   0.0  1.00\n"
        "10.0   0.0  0.90\n"
        "10.0  10.0  0.80\n"
        " 0.0  10.0  0.90\n"
        " 5.0   5.0  0.85\n"
        "\n"
        "[2D_TRIANGLES]\n"
        ";;V1  V2  V3  MANNINGS_N\n"
        "0     1   4   0.03\n"
        "1     2   4   0.03\n"
        "2     3   4   0.03\n"
        "3     0   4   0.03\n"
        + extra_sections +
        "\n[2D_VERTEX_NODE_MAP]\n;;Vertex  Node  Cd   Area\n0  J1  0.7  1.0\n";
}

/// Opens (parses only — never initializes) and returns the engine handle on
/// success, or nullptr with @p err_out filled on parse failure.
SWMM_Engine parseOnly(const fs::path& dir, const std::string& tag,
                      const std::string& extra_sections, std::string* err_out) {
    const fs::path inp = dir / (tag + ".inp");
    const fs::path rpt = dir / (tag + ".rpt");
    const fs::path out = dir / (tag + ".out");
    { std::ofstream f(inp); f << buildModel(extra_sections); }

    SWMM_Engine eng = swmm_engine_create();
    const int rc = swmm_engine_open(eng, inp.string().c_str(), rpt.string().c_str(),
                                    out.string().c_str(), nullptr);
    if (rc != SWMM_OK) {
        if (err_out) {
            const char* msg = swmm_get_last_error_msg(eng);
            *err_out = msg ? msg : "";
        }
        swmm_engine_close(eng);
        swmm_engine_destroy(eng);
        return nullptr;
    }
    return eng;
}

}  // namespace

// ============================================================================
// [2D_ROM]
// ============================================================================

TEST(SurfaceROMInputParser, TwoDRomSectionSetsScalarOptions) {
    const fs::path dir = fs::current_path() / "rom_input_parser_out";
    fs::create_directories(dir);

    const std::string extra =
        "\n[2D_ROM]\n"
        ";;PARAMETER              VALUE\n"
        "ENABLE                   YES\n"
        "MEMBERS                  30\n"
        "MODES                    8\n"
        "MANNINGS_PERT            0.15\n"
        "RAINFALL_PERT            0.10\n"
        "K_EFF                    0.9\n"
        "WET_RESEED_FRACTION      0.30\n"
        "WET_RESEED_MIN_INTERVAL  500\n"
        "PARAMETRIC_TAILS         YES\n"
        "MODE_DROP_THRESHOLD      1e-8\n"
        "MANNINGS_CORR_LEN        5.0\n"
        "RAINFALL_CORR_LEN        3.0\n";

    std::string err;
    auto eng = parseOnly(dir, "rom_section", extra, &err);
    ASSERT_NE(eng, nullptr) << "parse failed: " << err;

    const auto& o = static_cast<openswmm::SWMMEngine*>(eng)->surfaceRouter2D().options();
    EXPECT_TRUE(o.enable_rom);
    EXPECT_EQ(o.rom_members, 30);
    EXPECT_EQ(o.rom_modes, 8);
    EXPECT_DOUBLE_EQ(o.rom_mannings_pert, 0.15);
    EXPECT_DOUBLE_EQ(o.rom_rainfall_pert, 0.10);
    EXPECT_DOUBLE_EQ(o.rom_k_eff, 0.9);
    EXPECT_DOUBLE_EQ(o.rom_wet_reseed_fraction, 0.30);
    EXPECT_DOUBLE_EQ(o.rom_wet_reseed_min_interval, 500.0);
    EXPECT_TRUE(o.rom_parametric_tails);
    EXPECT_DOUBLE_EQ(o.rom_mode_drop_threshold, 1e-8);
    EXPECT_DOUBLE_EQ(o.rom_mannings_corr_len, 5.0);
    EXPECT_DOUBLE_EQ(o.rom_rainfall_corr_len, 3.0);

    swmm_engine_close(eng);
    swmm_engine_destroy(eng);
}

TEST(SurfaceROMInputParser, TwoDRomSectionSetsReducedOperatorDials) {
    const fs::path dir = fs::current_path() / "rom_input_parser_out";
    fs::create_directories(dir);

    const std::string extra =
        "\n[2D_ROM]\n"
        "ENABLE          YES\n"
        "LEGACY_OPERATOR NO\n"
        "ALPHA_PAR       0.5\n"
        "ALPHA_PERP      1.5\n"
        "C_FACTOR        1.2\n"
        "GROUND_SCALE    0.4\n";

    std::string err;
    auto eng = parseOnly(dir, "rom_dials", extra, &err);
    ASSERT_NE(eng, nullptr) << "parse failed: " << err;

    const auto& o = static_cast<openswmm::SWMMEngine*>(eng)->surfaceRouter2D().options();
    EXPECT_TRUE(o.enable_rom);
    EXPECT_FALSE(o.rom_legacy_operator);
    EXPECT_DOUBLE_EQ(o.rom_alpha_par, 0.5);
    EXPECT_DOUBLE_EQ(o.rom_alpha_perp, 1.5);
    EXPECT_DOUBLE_EQ(o.rom_c_factor, 1.2);
    EXPECT_DOUBLE_EQ(o.rom_ground_scale, 0.4);

    swmm_engine_close(eng);
    swmm_engine_destroy(eng);
}

TEST(SurfaceROMInputParser, TwoDRomSectionLegacyOperatorYesRoundTrips) {
    const fs::path dir = fs::current_path() / "rom_input_parser_out";
    fs::create_directories(dir);

    const std::string extra = "\n[2D_ROM]\nENABLE YES\nLEGACY_OPERATOR YES\n";
    std::string err;
    auto eng = parseOnly(dir, "rom_legacy", extra, &err);
    ASSERT_NE(eng, nullptr) << "parse failed: " << err;

    EXPECT_TRUE(static_cast<openswmm::SWMMEngine*>(eng)
                    ->surfaceRouter2D().options().rom_legacy_operator);

    swmm_engine_close(eng);
    swmm_engine_destroy(eng);
}

TEST(SurfaceROMInputParser, TwoDRomSectionRejectsUnknownParameter) {
    const fs::path dir = fs::current_path() / "rom_input_parser_out";
    fs::create_directories(dir);

    const std::string extra = "\n[2D_ROM]\nENABLE YES\nNOT_A_REAL_PARAM 1.0\n";
    std::string err;
    auto eng = parseOnly(dir, "rom_bad_key", extra, &err);
    EXPECT_EQ(eng, nullptr);
    EXPECT_NE(err.find("[2D]"), std::string::npos) << "error was: " << err;
}

// ============================================================================
// [UNCERTAINTY]
// ============================================================================

TEST(SurfaceROMInputParser, UncertaintySectionEnablesRomAndSetsPerturbation) {
    const fs::path dir = fs::current_path() / "rom_input_parser_out";
    fs::create_directories(dir);

    // [UNCERTAINTY] alone, no [2D_ROM] at all — ENABLE must follow implicitly.
    const std::string extra = "\n[UNCERTAINTY]\n2D MANNINGS_N 0.22\n";
    std::string err;
    auto eng = parseOnly(dir, "unc_alone", extra, &err);
    ASSERT_NE(eng, nullptr) << "parse failed: " << err;

    const auto& o = static_cast<openswmm::SWMMEngine*>(eng)->surfaceRouter2D().options();
    EXPECT_TRUE(o.enable_rom);
    EXPECT_DOUBLE_EQ(o.rom_mannings_pert, 0.22);

    swmm_engine_close(eng);
    swmm_engine_destroy(eng);
}

TEST(SurfaceROMInputParser, UncertaintySectionOverridesTwoDRomScalar) {
    const fs::path dir = fs::current_path() / "rom_input_parser_out";
    fs::create_directories(dir);

    // [2D_ROM] sets 0.15; the later [UNCERTAINTY] entry for the same
    // parameter must win (the documented precedence).
    const std::string extra =
        "\n[2D_ROM]\nENABLE YES\nMANNINGS_PERT 0.15\n"
        "\n[UNCERTAINTY]\n2D MANNINGS_N 0.35\n";
    std::string err;
    auto eng = parseOnly(dir, "unc_override", extra, &err);
    ASSERT_NE(eng, nullptr) << "parse failed: " << err;

    EXPECT_DOUBLE_EQ(static_cast<openswmm::SWMMEngine*>(eng)
                        ->surfaceRouter2D().options().rom_mannings_pert,
                    0.35);

    swmm_engine_close(eng);
    swmm_engine_destroy(eng);
}

TEST(SurfaceROMInputParser, UncertaintySectionLegacyDistFirstGrammarParses) {
    // Legacy order: LAYER NAME DIST PERT (disambiguated by tokens[2] not
    // parsing as a number).
    const fs::path dir = fs::current_path() / "rom_input_parser_out";
    fs::create_directories(dir);

    const std::string extra = "\n[UNCERTAINTY]\n2D RAINFALL UNIFORM 0.18\n";
    std::string err;
    auto eng = parseOnly(dir, "unc_legacy_grammar", extra, &err);
    ASSERT_NE(eng, nullptr) << "parse failed: " << err;

    EXPECT_DOUBLE_EQ(static_cast<openswmm::SWMMEngine*>(eng)
                        ->surfaceRouter2D().options().rom_rainfall_pert,
                    0.18);

    swmm_engine_close(eng);
    swmm_engine_destroy(eng);
}

TEST(SurfaceROMInputParser, UncertaintySectionRecords1DAndQualitySpecsOnly) {
    // 1D/QUALITY specs must not touch the 2D solver options at all — they
    // are recorded for the (separate, unstarted) 1D ROM lifecycle to consume
    // later. This also confirms LAYER QUALITY's UNIFORM-only restriction and
    // the FORCING_VECTOR entry inference for INFLOW are both reachable.
    const fs::path dir = fs::current_path() / "rom_input_parser_out";
    fs::create_directories(dir);

    const std::string extra =
        "\n[UNCERTAINTY]\n"
        "1D INFLOW 0.30 LOGNORMAL FORCING_VECTOR\n"
        "QUALITY TSS 0.10\n";
    std::string err;
    auto eng = parseOnly(dir, "unc_1d_quality", extra, &err);
    ASSERT_NE(eng, nullptr) << "parse failed: " << err;

    auto* impl = static_cast<openswmm::SWMMEngine*>(eng);
    EXPECT_FALSE(impl->surfaceRouter2D().options().enable_rom)
        << "1D/QUALITY specs must not enable the 2D ROM";

    const auto& cfg = impl->uncertaintyConfig();
    ASSERT_EQ(cfg.sources.size(), 2u);

    const auto& s0 = cfg.sources[0];
    EXPECT_EQ(s0.layer, openswmm::uncertainty::LayerTarget::ONE_D);
    EXPECT_EQ(s0.name, "INFLOW");
    EXPECT_EQ(s0.dist, openswmm::uncertainty::DistType::LOGNORMAL);
    EXPECT_EQ(s0.entry, openswmm::uncertainty::ParamEntry::FORCING_VECTOR);
    EXPECT_DOUBLE_EQ(s0.perturbation, 0.30);

    const auto& s1 = cfg.sources[1];
    EXPECT_EQ(s1.layer, openswmm::uncertainty::LayerTarget::QUALITY);
    EXPECT_EQ(s1.name, "TSS");
    EXPECT_EQ(s1.entry, openswmm::uncertainty::ParamEntry::QUALITY_MULT);
    EXPECT_DOUBLE_EQ(s1.perturbation, 0.10);

    swmm_engine_close(eng);
    swmm_engine_destroy(eng);
}

TEST(SurfaceROMInputParser, UncertaintySectionQualityRejectsNonUniformDist) {
    const fs::path dir = fs::current_path() / "rom_input_parser_out";
    fs::create_directories(dir);

    const std::string extra = "\n[UNCERTAINTY]\nQUALITY TSS 0.10 LOGNORMAL\n";
    std::string err;
    auto eng = parseOnly(dir, "unc_quality_bad_dist", extra, &err);
    EXPECT_EQ(eng, nullptr);
    EXPECT_NE(err.find("[2D]"), std::string::npos) << "error was: " << err;
}

TEST(SurfaceROMInputParser, UncertaintySectionRejectsUnknownLayer) {
    const fs::path dir = fs::current_path() / "rom_input_parser_out";
    fs::create_directories(dir);

    const std::string extra = "\n[UNCERTAINTY]\n3D MANNINGS_N 0.10\n";
    std::string err;
    auto eng = parseOnly(dir, "unc_bad_layer", extra, &err);
    EXPECT_EQ(eng, nullptr);
    EXPECT_NE(err.find("[2D]"), std::string::npos) << "error was: " << err;
}

TEST(SurfaceROMInputParser, UncertaintySectionRejectsUnknownNameWithoutEntry) {
    const fs::path dir = fs::current_path() / "rom_input_parser_out";
    fs::create_directories(dir);

    const std::string extra = "\n[UNCERTAINTY]\n2D SOME_UNKNOWN_PARAM 0.10\n";
    std::string err;
    auto eng = parseOnly(dir, "unc_bad_name", extra, &err);
    EXPECT_EQ(eng, nullptr);
    EXPECT_NE(err.find("[2D]"), std::string::npos) << "error was: " << err;
}
