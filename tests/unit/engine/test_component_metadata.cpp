// SPDX-License-Identifier: Apache-2.0

/**
 * @file      test_component_metadata.cpp
 * @brief     Tests for component metadata.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_spatial.h>
#ifdef OPENSWMM_HAS_2D
#include <openswmm/engine/openswmm_2d.h>
#endif
#include <filesystem>
#include <fstream>
namespace fs = std::filesystem;
namespace {
struct Engine {
    SWMM_Engine value = swmm_engine_create();
    /**
     * @brief Destroy the sole owned metadata-test engine, including partial open state.
     *
     * Calls the C API destroy with value and does not clear aliases. This plain wrapper must not be
     * copied into independent owners; any borrowed context/handle references expire here.
     */
    ~Engine() { swmm_engine_destroy(value); }
};
/**
 * @brief Create a retained metadata fixture from the reference deck.
 *
 * Creates directories/copies reference with throwing filesystem APIs; appends absolute
 * declared.iface/.hsf paths without escaping or stream-status checks. Existing other files remain.
 * Does not open plugins, create declared outputs or run an engine.
 *
 * @param[in] name Controlled safe subdirectory name beneath component_metadata.
 * @param[in] declaredOutputs True appends output declarations and a deliberately missing plugin
 *     request.
 * @return Absolute fixture directory containing overwritten model.inp.
 */
fs::path fixture(const std::string& name, bool declaredOutputs = false) {
    auto directory = fs::absolute(fs::path("component_metadata") / name);
    fs::create_directories(directory);
    fs::copy_file("component_metadata/reference.inp", directory / "model.inp",
                  fs::copy_options::overwrite_existing);
    if (declaredOutputs) {
        std::ofstream f(directory / "model.inp", std::ios::app);
        f << "\n[FILES]\nSAVE OUTFLOWS \"" << (directory / "declared.iface").string() << "\"\n"
          << "SAVE HOTSTART \"" << (directory / "declared.hsf").string() << "\"\n"
          << "[PLUGINS]\nmissing_metadata_probe_plugin\n";
    }
    return directory;
}
}
TEST(ComponentMetadata, ParsedCatalogueWithoutRuntimePluginsOrOutputFiles) {
    const auto directory = fixture("discovery", true);
    Engine e;
    ASSERT_EQ(swmm_engine_open_metadata(e.value, (directory / "model.inp").string().c_str()),
              SWMM_OK);
    EXPECT_EQ(swmm_node_count(e.value), 2);
    EXPECT_EQ(swmm_link_count(e.value), 1);
    EXPECT_EQ(swmm_pollutant_count(e.value), 2);
    EXPECT_STREQ(swmm_pollutant_id(e.value, 0), "A");
    int units = -1;
    ASSERT_EQ(swmm_pollutant_get_units(e.value, 1, &units), 0);
    EXPECT_EQ(units, 1);
    int plugins = 0;
    ASSERT_EQ(swmm_plugins_count(e.value, &plugins), 0);
    EXPECT_EQ(plugins, 1);
    for (int i = 0; i < swmm_get_warning_count(e.value); ++i)
        EXPECT_EQ(
            std::string(swmm_get_warning_at(e.value, i)).find("missing_metadata_probe_plugin"),
            std::string::npos);
    double elapsed = -1;
    ASSERT_EQ(swmm_engine_get_elapsed_seconds(e.value, &elapsed), 0);
    EXPECT_DOUBLE_EQ(elapsed, 0);
    ASSERT_EQ(swmm_engine_close(e.value), 0);
    EXPECT_FALSE(fs::exists(directory / "declared.iface"));
    EXPECT_FALSE(fs::exists(directory / "declared.hsf"));
    for (const auto& entry : fs::directory_iterator(directory))
        EXPECT_EQ(entry.path().filename(), "model.inp");
}
TEST(ComponentMetadata, CannotInitializeOrStartAndCanReopenNormally) {
    const auto directory = fixture("reopen");
    Engine e;
    ASSERT_EQ(swmm_engine_open_metadata(e.value, (directory / "model.inp").string().c_str()), 0);
    EXPECT_EQ(swmm_engine_initialize(e.value), SWMM_ERR_LIFECYCLE);
    EXPECT_NE(swmm_engine_start(e.value, 1), 0);
    ASSERT_EQ(swmm_engine_close(e.value), 0);
    ASSERT_EQ(swmm_engine_open(e.value, (directory / "model.inp").string().c_str(), nullptr,
                               nullptr, nullptr),
              0);
    ASSERT_EQ(swmm_engine_initialize(e.value), 0);
    ASSERT_EQ(swmm_engine_start(e.value, 0), 0);
    double actual = 0;
    ASSERT_EQ(swmm_engine_advance_to(e.value, 60, &actual), 0);
    EXPECT_DOUBLE_EQ(actual, 60);
}
TEST(ComponentMetadata, ReactionMetadataUsesTheExistingRegistry) {
    const auto directory = fixture("reactions");
    std::ofstream(directory / "model.rxn")
        << "[REACTION_SPECIES]\nBULK X MG\nWALL W MG\n[REACTION_PIPES]\nFORMULA X 2*A\n";
    std::ofstream(directory / "model.inp", std::ios::app)
        << "\n[PROCESS_COMPONENTS]\norg.hydrocouple.openswmm.reactions config=\"model.rxn\"\n";
    Engine e;
    ASSERT_EQ(swmm_engine_open_metadata(e.value, (directory / "model.inp").string().c_str()), 0);
    ASSERT_EQ(swmm_reaction_species_count(e.value), 2);
    char name[64], units[64];
    int wall = -1;
    double atol, rtol;
    ASSERT_EQ(swmm_reaction_species_get(e.value, 1, name, 64, &wall, units, 64, &atol, &rtol), 0);
    EXPECT_STREQ(name, "W");
    EXPECT_STREQ(units, "MG");
    EXPECT_EQ(wall, 1);
}
TEST(ComponentMetadata, ValidatesHandlesPathsAndStrictInputEvenAfterLenientSelection) {
    Engine e;
    EXPECT_EQ(swmm_engine_open_metadata(nullptr, "x"), SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_engine_open_metadata(e.value, nullptr), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_engine_open_metadata(e.value, ""), SWMM_ERR_BADPARAM);
    const auto directory = fixture("invalid");
    std::ofstream(directory / "bad.inp")
        << "[OPTIONS]\nFLOW_UNITS CFS\n[JUNCTIONS]\nJ 10 10\n[CONDUITS]\nC J MISSING 10 .013 0 0\n[XSECTIONS]\nC CIRCULAR 2\n";
    swmm_engine_set_lenient_open(e.value, 1);
    EXPECT_NE(swmm_engine_open_metadata(e.value, (directory / "bad.inp").string().c_str()), 0);
}

TEST(ComponentMetadata, CoordinatePresenceDistinguishesMissingFromAuthoredOrigin) {
    const auto directory = fixture("coordinate-presence");
    std::ofstream(directory / "model.inp", std::ios::app) << "\n[COORDINATES]\nJ 0 0\n";
    Engine e;
    ASSERT_EQ(swmm_engine_open_metadata(e.value, (directory / "model.inp").string().c_str()), 0);
    int present = -1;
    const int j = swmm_node_index(e.value, "J"), o = swmm_node_index(e.value, "O");
    ASSERT_EQ(swmm_spatial_node_has_coord(e.value, j, &present), 0);
    EXPECT_EQ(present, 1);
    ASSERT_EQ(swmm_spatial_node_has_coord(e.value, o, &present), 0);
    EXPECT_EQ(present, 0);
    EXPECT_EQ(swmm_spatial_node_has_coord(nullptr, j, &present), SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_spatial_node_has_coord(e.value, -1, &present), SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_spatial_node_has_coord(e.value, j, nullptr), SWMM_ERR_BADPARAM);
    ASSERT_EQ(swmm_spatial_set_node_coord(e.value, o, 0, 0), 0);
    ASSERT_EQ(swmm_spatial_node_has_coord(e.value, o, &present), 0);
    EXPECT_EQ(present, 1);
}

#ifdef OPENSWMM_HAS_2D
TEST(ComponentMetadata, GeometryScaleTracksProjectUnitsAndSiDeclarationWithoutDoubleConversion) {
    for (const std::string mode : {"us", "si", "us_si_mesh"}) {
        const auto directory = fixture("geometry-scale-" + mode);
        const auto model = directory / "model.inp";
        std::ifstream input(model);
        std::string text{std::istreambuf_iterator<char>(input), {}};
        input.close();
        if (mode == "si")
            text.replace(text.find("FLOW_UNITS CFS"), 14, "FLOW_UNITS LPS");
        std::ofstream(model) << text << "\n[2D_OPTIONS]\nREPORT_2D NO\n[2D_VERTICES]\n"
                             << (mode == "us_si_mesh" ? ";; UNITS: SI (m)\n" : "")
                             << "0 0 10\n10 0 10\n10 10 10\n0 10 10\n[2D_QUADS]\n0 1 2 3 .03 0\n";
        Engine e;
        ASSERT_EQ(swmm_engine_open_metadata(e.value, model.string().c_str()), 0);
        double scale = 0, x = 0, y = 0, z = 0;
        ASSERT_EQ(swmm_2d_geometry_scale_to_si(e.value, &scale), 0);
        EXPECT_DOUBLE_EQ(scale, mode == "us" ? .3048 : 1.0);
        ASSERT_EQ(swmm_2d_vertex_get_xyz(e.value, 1, &x, &y, &z), 0);
        EXPECT_DOUBLE_EQ(x, 10);
        EXPECT_DOUBLE_EQ(z, 10);
        const double expected = 10 * scale;
        EXPECT_EQ(swmm_2d_geometry_scale_to_si(e.value, nullptr), SWMM_ERR_BADPARAM);
        ASSERT_EQ(swmm_engine_close(e.value), 0);
        ASSERT_EQ(swmm_engine_open(e.value, model.string().c_str(),
                                   (directory / "native.rpt").string().c_str(), nullptr, nullptr),
                  0);
        ASSERT_EQ(swmm_engine_initialize(e.value), 0) << swmm_get_last_error_msg(e.value);
        ASSERT_EQ(swmm_2d_geometry_scale_to_si(e.value, &scale), 0);
        EXPECT_DOUBLE_EQ(scale, 1.0);
        ASSERT_EQ(swmm_2d_vertex_get_xyz(e.value, 1, &x, &y, &z), 0);
        EXPECT_DOUBLE_EQ(x, expected);
        EXPECT_DOUBLE_EQ(z, expected);
    }
}
#endif
