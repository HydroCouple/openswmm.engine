/**
 * @file test_2d_output_options.cpp
 * @brief Results-file size controls for the 2D HDF5 writer
 *        (plans/2D_OUTPUT_FLOAT32_AND_VARIABLE_SELECTION_PLAN_2026-09-04.md):
 *        [2D_OPTIONS] OUTPUT_PRECISION, OUTPUT_COMPRESSION,
 *        REPORT_2D_VARIABLES, REPORT_2D_SPECIES, REPORT_2D_STEP.
 *
 * @details Pins:
 *   - the grammar: token ↔ bitmask helpers, presets, error on unknown token,
 *     option round trip through parse2DOptionsLine / format2DOptionValue and
 *     the swmm_options_*_ext C API;
 *   - the writer: FLOAT32 storage vs FLOAT64, shuffle+deflate, datasets not
 *     selected are NOT created, species subset by name, the 2D report step
 *     cadence, and the root attributes readers key on;
 *   - the writer's defaults drop the solver diagnostics.
 *
 * Artefacts land in tests/output/2d_output_options (CLAUDE.md §4.1).
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include <hdf5.h>

#include "2d/data/MeshData.hpp"
#include "2d/data/SolverOptions2D.hpp"
#include "2d/data/Report2DVars.hpp"
#include "2d/input/SectionHandlers2D.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include "2d/output/Default2DOutputPlugin.hpp"
#include "core/SimulationContext.hpp"
#include "core/DateTime.hpp"
#include "openswmm/plugin_sdk/SimulationSnapshot.hpp"

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_forcing.h>
#include <openswmm/engine/openswmm_sq2d.h>

namespace fs = std::filesystem;
using namespace openswmm::twoD;

namespace {

const fs::path kOutDir = fs::path(OPENSWMM_2D_OUTPUT_TEST_OUT_DIR) / "2d_output_options";

MeshData unitSquare() {
    MeshData mesh;
    mesh.resize_vertices(4);
    mesh.vx = {0.0, 1.0, 0.0, 1.0};
    mesh.vy = {0.0, 0.0, 1.0, 1.0};
    mesh.vz = {0.0, 0.0, 0.0, 0.0};
    mesh.resize_triangles(2);
    mesh.set_triangle(0, 0, 1, 3);
    mesh.set_triangle(1, 0, 3, 2);
    mesh.mannings_n[0] = mesh.mannings_n[1] = 0.035;
    buildMeshTopology(mesh);
    return mesh;
}

/// One tick of surface state for the 2-cell mesh, with 3 species rows.
openswmm::SimulationSnapshot makeSnap(double t_days, double depth0, double depth1) {
    openswmm::SimulationSnapshot snap;
    snap.sim_time           = t_days;
    snap.surface_tri_count  = 2;
    snap.surface_vert_count = 4;
    snap.surface_depth         = {depth0, depth1};
    snap.surface_head          = {depth0, depth1};
    snap.surface_grad_hx       = {0.0, 0.0};
    snap.surface_grad_hy       = {0.0, 0.0};
    snap.surface_grad_hx_lim   = {0.0, 0.0};
    snap.surface_grad_hy_lim   = {0.0, 0.0};
    snap.surface_rainfall      = {1e-6, 2e-6};
    snap.surface_rain_cum      = {3.0, 7.0};
    snap.surface_coupling_flux = {0.0, 0.0};
    snap.surface_net_source    = {0.0, 0.0};
    snap.surface_infil_rate    = {0.0, 0.0};
    snap.surface_infil_cum     = {0.0, 0.0};
    snap.surface_face_vx       = {0.123456789, 0.5};
    snap.surface_face_vy       = {0.0, 0.25};
    snap.surface_continuity_err = {0.0, 0.0};
    snap.surface_edge_stride   = 3;
    snap.surface_edge_flux     = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    snap.surface_vert_head     = {0.0, 0.0, 0.0, 0.0};
    snap.surface_vert_depth    = {0.0, 0.0, 0.0, 0.0};
    snap.surface_stat_max_depth    = {depth0, depth1};
    snap.surface_stat_max_velocity = {0.2, 0.4};
    snap.surface_stat_max_cont_err = {1e-6, 2e-6};
    snap.surface_species_count = 3;
    snap.surface_species_conc  = {1.0, 2.0,   10.0, 20.0,   100.0, 200.0};
    return snap;
}

const std::vector<std::string> kSpeciesNames = {"TSS", "__WATER_AGE__", "__TEMPERATURE__"};

struct H5File {
    hid_t id = H5I_INVALID_HID;
    explicit H5File(const fs::path& p) { id = H5Fopen(p.string().c_str(), H5F_ACC_RDONLY, H5P_DEFAULT); }
    ~H5File() { if (id >= 0) H5Fclose(id); }
    bool has(const char* name) const { return H5Lexists(id, name, H5P_DEFAULT) > 0; }
    /// Storage class + size of a dataset (H5T_FLOAT, 4 or 8).
    size_t storageSize(const char* name) const {
        hid_t ds = H5Dopen2(id, name, H5P_DEFAULT);
        hid_t t  = H5Dget_type(ds);
        const size_t sz = H5Tget_size(t);
        H5Tclose(t); H5Dclose(ds);
        return sz;
    }
    /// Allocated data bytes of a dataset — the sum of its chunks, excluding
    /// the file's object headers and chunk index. This is the payload
    /// OUTPUT_PRECISION actually thins.
    hsize_t datasetBytes(const char* name) const {
        hid_t ds = H5Dopen2(id, name, H5P_DEFAULT);
        const hsize_t n = H5Dget_storage_size(ds);
        H5Dclose(ds);
        return n;
    }
    std::vector<hsize_t> dims(const char* name) const {
        hid_t ds = H5Dopen2(id, name, H5P_DEFAULT);
        hid_t sp = H5Dget_space(ds);
        const int rank = H5Sget_simple_extent_ndims(sp);
        std::vector<hsize_t> d(static_cast<size_t>(rank));
        H5Sget_simple_extent_dims(sp, d.data(), nullptr);
        H5Sclose(sp); H5Dclose(ds);
        return d;
    }
    std::vector<double> readAll(const char* name) const {
        auto d = dims(name);
        size_t n = 1; for (auto x : d) n *= x;
        std::vector<double> v(n);
        hid_t ds = H5Dopen2(id, name, H5P_DEFAULT);
        H5Dread(ds, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, v.data());
        H5Dclose(ds);
        return v;
    }
    std::string strAttr(hid_t loc, const char* name) const {
        if (H5Aexists(loc, name) <= 0) return {};
        hid_t a = H5Aopen(loc, name, H5P_DEFAULT);
        hid_t t = H5Aget_type(a);
        const size_t sz = H5Tget_size(t);
        std::string s(sz, '\0');
        H5Aread(a, t, s.data());
        H5Tclose(t); H5Aclose(a);
        while (!s.empty() && s.back() == '\0') s.pop_back();
        return s;
    }
    std::string rootAttr(const char* name) const { return strAttr(id, name); }
    std::string dsAttr(const char* ds_name, const char* attr) const {
        hid_t ds = H5Dopen2(id, ds_name, H5P_DEFAULT);
        const std::string s = strAttr(ds, attr);
        H5Dclose(ds);
        return s;
    }
};

/// Run the writer lifecycle with the given options; returns the file path.
fs::path writeFile(const std::string& stem, const SolverOptions2D& opts,
                   double report_step_sec, int n_ticks, double tick_days,
                   std::string* cfg_err = nullptr) {
    std::error_code ec;
    fs::create_directories(kOutDir, ec);
    const fs::path p = kOutDir / (stem + ".h5");
    fs::remove(p, ec);

    MeshData mesh = unitSquare();
    Default2DOutputPlugin plugin(p.string());
    EXPECT_EQ(plugin.initialize({}, nullptr), 0);
    openswmm::SimulationContext ctx{};
    EXPECT_EQ(plugin.validate(ctx), 0);
    EXPECT_EQ(plugin.prepare(ctx), 0);
    const std::string err = plugin.configureOutput(opts, report_step_sec);
    if (cfg_err) *cfg_err = err;
    plugin.prepareMeshAndDatasets(mesh);
    for (int k = 0; k < n_ticks; ++k) {
        auto snap = makeSnap(k * tick_days, 0.05 + 0.01 * k, 0.10 + 0.01 * k);
        snap.surface_species_names = &kSpeciesNames;
        EXPECT_EQ(plugin.update(snap), 0);
    }
    EXPECT_EQ(plugin.finalize(ctx), 0);
    return p;
}

} // namespace

// ---------------------------------------------------------------------------
// Grammar
// ---------------------------------------------------------------------------

TEST(Output2DOptions, TokenMaskRoundTrip) {
    unsigned m = 0;
    EXPECT_TRUE(report2d::parseMask("DEFAULT", m).empty());
    EXPECT_EQ(m, report2d::DEFAULT_MASK);
    EXPECT_EQ(report2d::formatMask(m), "DEFAULT");

    EXPECT_TRUE(report2d::parseMask("minimal", m).empty());
    EXPECT_EQ(m, report2d::MINIMAL_MASK);
    EXPECT_EQ(report2d::formatMask(m), "MINIMAL");

    EXPECT_TRUE(report2d::parseMask("ALL", m).empty());
    EXPECT_EQ(m, report2d::ALL_MASK);

    // Token list, comma or space separated; DEPTH is always implied.
    EXPECT_TRUE(report2d::parseMask("VELOCITY, GRADIENTS", m).empty());
    EXPECT_EQ(m, report2d::DEPTH | report2d::VELOCITY | report2d::GRADIENTS);
    EXPECT_EQ(report2d::formatMask(m), "DEPTH VELOCITY GRADIENTS");

    EXPECT_FALSE(report2d::parseMask("VELOCITY BOGUS", m).empty());
    EXPECT_EQ(swmm_2d_output_variable_mask("BOGUS"), 0u);
    EXPECT_EQ(swmm_2d_output_variable_mask("DEFAULT"), report2d::DEFAULT_MASK);
    EXPECT_EQ(swmm_2d_output_variable_count(), 14);   // S7: + BUILDUP; G-O: + GROUNDWATER, GW_DETAILED
    EXPECT_STREQ(swmm_2d_output_variable_name(0), "DEPTH");
    EXPECT_STREQ(swmm_2d_output_variable_name(10), "ENVELOPES");
    EXPECT_STREQ(swmm_2d_output_variable_name(11), "BUILDUP");
    EXPECT_STREQ(swmm_2d_output_variable_name(12), "GROUNDWATER");
    EXPECT_STREQ(swmm_2d_output_variable_name(13), "GW_DETAILED");
    EXPECT_NE(report2d::DEFAULT_MASK & report2d::BUILDUP, 0u);
    EXPECT_NE(report2d::DEFAULT_MASK & report2d::GROUNDWATER, 0u);
    EXPECT_EQ(report2d::DEFAULT_MASK & report2d::GW_DETAILED, 0u);
    EXPECT_STREQ(swmm_2d_output_variable_text(report2d::MINIMAL_MASK), "MINIMAL");

    // Defaults drop the solver diagnostics only.
    EXPECT_EQ(report2d::DEFAULT_MASK & report2d::GRADIENTS, 0u);
    EXPECT_EQ(report2d::DEFAULT_MASK & report2d::CONTINUITY, 0u);
    EXPECT_EQ(report2d::DEFAULT_MASK & report2d::COUPLING, 0u);
    EXPECT_NE(report2d::DEFAULT_MASK & report2d::EDGE_FLUX, 0u);
}

TEST(Output2DOptions, ParseAndFormatKeys) {
    SolverOptions2D o;
    EXPECT_EQ(o.output_precision, OutputPrecision2D::FLOAT32);
    EXPECT_EQ(o.output_compression, 4);
    EXPECT_EQ(o.report_2d_vars, report2d::DEFAULT_MASK);
    EXPECT_TRUE(o.report_2d_species.empty());
    EXPECT_EQ(o.report_2d_step, 0.0);

    EXPECT_TRUE(parse2DOptionsLine({"OUTPUT_PRECISION", "FLOAT64"}, o, nullptr).empty());
    EXPECT_EQ(o.output_precision, OutputPrecision2D::FLOAT64);
    EXPECT_EQ(format2DOptionValue(o, "OUTPUT_PRECISION"), "FLOAT64");
    EXPECT_FALSE(parse2DOptionsLine({"OUTPUT_PRECISION", "HALF"}, o, nullptr).empty());

    EXPECT_TRUE(parse2DOptionsLine({"OUTPUT_COMPRESSION", "9"}, o, nullptr).empty());
    EXPECT_EQ(format2DOptionValue(o, "OUTPUT_COMPRESSION"), "9");
    EXPECT_FALSE(parse2DOptionsLine({"OUTPUT_COMPRESSION", "12"}, o, nullptr).empty());

    // Multi-token file line
    EXPECT_TRUE(parse2DOptionsLine({"REPORT_2D_VARIABLES", "DEPTH", "VELOCITY"}, o, nullptr).empty());
    EXPECT_EQ(o.report_2d_vars, report2d::DEPTH | report2d::VELOCITY);
    // Single joined token (the C API path)
    EXPECT_TRUE(parse2DOptionsLine({"REPORT_2D_VARIABLES", "MINIMAL"}, o, nullptr).empty());
    EXPECT_EQ(format2DOptionValue(o, "REPORT_2D_VARIABLES"), "MINIMAL");

    EXPECT_TRUE(parse2DOptionsLine({"REPORT_2D_SPECIES", "TSS,__TEMPERATURE__"}, o, nullptr).empty());
    ASSERT_EQ(o.report_2d_species.size(), 2u);
    EXPECT_EQ(format2DOptionValue(o, "REPORT_2D_SPECIES"), "TSS __TEMPERATURE__");
    EXPECT_TRUE(parse2DOptionsLine({"REPORT_2D_SPECIES", "ALL"}, o, nullptr).empty());
    EXPECT_TRUE(o.report_2d_species.empty());
    EXPECT_EQ(format2DOptionValue(o, "REPORT_2D_SPECIES"), "ALL");

    EXPECT_TRUE(parse2DOptionsLine({"REPORT_2D_STEP", "01:00:00"}, o, nullptr).empty());
    EXPECT_DOUBLE_EQ(o.report_2d_step, 3600.0);
    EXPECT_EQ(format2DOptionValue(o, "REPORT_2D_STEP"), "01:00:00");
    EXPECT_TRUE(parse2DOptionsLine({"REPORT_2D_STEP", "900"}, o, nullptr).empty());
    EXPECT_DOUBLE_EQ(o.report_2d_step, 900.0);

    EXPECT_TRUE(is2DOptionKey("REPORT_2D_VARIABLES"));
    EXPECT_TRUE(is2DOptionKey("output_precision"));
}

TEST(Output2DOptions, CApiExtRoundTripOnAModel) {
    SWMM_Engine e = swmm_engine_new();
    ASSERT_NE(e, nullptr);
    // Keys are accepted whether or not the model has a mesh yet.
    EXPECT_EQ(swmm_options_set_ext(e, "REPORT_2D_VARIABLES", "DEPTH VELOCITY NODE_HEAD"), SWMM_OK);
    EXPECT_EQ(swmm_options_set_ext(e, "OUTPUT_PRECISION", "FLOAT64"), SWMM_OK);
    EXPECT_EQ(swmm_options_set_ext(e, "REPORT_2D_STEP", "00:30:00"), SWMM_OK);
    char buf[128] = {};
    ASSERT_EQ(swmm_options_get_ext(e, "REPORT_2D_VARIABLES", buf, sizeof buf), SWMM_OK);
    EXPECT_STREQ(buf, "DEPTH VELOCITY NODE_HEAD");
    ASSERT_EQ(swmm_options_get_ext(e, "OUTPUT_PRECISION", buf, sizeof buf), SWMM_OK);
    EXPECT_STREQ(buf, "FLOAT64");
    ASSERT_EQ(swmm_options_get_ext(e, "REPORT_2D_STEP", buf, sizeof buf), SWMM_OK);
    EXPECT_STREQ(buf, "00:30:00");
    EXPECT_NE(swmm_options_set_ext(e, "REPORT_2D_VARIABLES", "NOPE"), SWMM_OK);
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------

TEST(Output2DWriter, DefaultIsFloat32AndDropsDiagnostics) {
    SolverOptions2D o;   // defaults: FLOAT32, level 4, DEFAULT mask
    const fs::path p = writeFile("default", o, 300.0, 2, 300.0 / 86400.0);
    H5File f(p);
    ASSERT_GE(f.id, 0);
    EXPECT_EQ(f.rootAttr("precision"), "float32");
    EXPECT_EQ(f.rootAttr("compression"), "4");
    EXPECT_EQ(f.rootAttr("report_2d_variables"), "DEFAULT");

    EXPECT_TRUE(f.has("Mesh2_face_depth"));
    EXPECT_EQ(f.storageSize("Mesh2_face_depth"), 4u);
    EXPECT_EQ(f.storageSize("Mesh2_face_max_depth"), 4u);
    // Geometry stays float64.
    EXPECT_EQ(f.storageSize("Mesh2_node_x"), 8u);
    EXPECT_EQ(f.storageSize("Mesh2_face_area"), 8u);
    // Time axis stays float64.
    EXPECT_EQ(f.storageSize("time"), 8u);

    EXPECT_TRUE(f.has("Mesh2_face_vx"));
    EXPECT_TRUE(f.has("Mesh2_edge_flux"));
    EXPECT_TRUE(f.has("Mesh2_node_head"));
    EXPECT_TRUE(f.has("Mesh2_face_rainfall"));
    EXPECT_TRUE(f.has("Mesh2_face_infil_rate"));
    EXPECT_TRUE(f.has("Mesh2_face_species_conc"));
    // Diagnostics off by default.
    EXPECT_FALSE(f.has("Mesh2_face_grad_hx"));
    EXPECT_FALSE(f.has("Mesh2_face_grad_hx_lim"));
    EXPECT_FALSE(f.has("Mesh2_face_continuity_err"));
    EXPECT_FALSE(f.has("Mesh2_face_coupling_flux"));
    EXPECT_FALSE(f.has("Mesh2_face_max_continuity_err"));

    // Values survive the float32 round trip to single precision.
    const auto vx = f.readAll("Mesh2_face_vx");
    ASSERT_EQ(vx.size(), 4u);
    EXPECT_NEAR(vx[0], 0.123456789, 1e-7);
    EXPECT_NE(vx[0], 0.123456789);   // and are NOT the double (proves storage)
    EXPECT_EQ(f.dims("time")[0], 2u);
}

TEST(Output2DWriter, Float64AllWritesEverythingExactly) {
    SolverOptions2D o;
    o.output_precision  = OutputPrecision2D::FLOAT64;
    o.output_compression = 0;
    o.report_2d_vars    = report2d::ALL_MASK;
    const fs::path p = writeFile("all_f64", o, 300.0, 1, 0.0);
    H5File f(p);
    ASSERT_GE(f.id, 0);
    EXPECT_EQ(f.rootAttr("precision"), "float64");
    EXPECT_EQ(f.rootAttr("compression"), "0");
    EXPECT_EQ(f.storageSize("Mesh2_face_depth"), 8u);
    EXPECT_TRUE(f.has("Mesh2_face_grad_hx_lim"));
    EXPECT_TRUE(f.has("Mesh2_face_continuity_err"));
    EXPECT_TRUE(f.has("Mesh2_face_coupling_flux"));
    EXPECT_TRUE(f.has("Mesh2_face_max_continuity_err"));
    const auto vx = f.readAll("Mesh2_face_vx");
    EXPECT_EQ(vx[0], 0.123456789);
}

TEST(Output2DWriter, MinimalCreatesOnlyDepthNodeAndEnvelopes) {
    SolverOptions2D o;
    o.report_2d_vars = report2d::MINIMAL_MASK;
    const fs::path p = writeFile("minimal", o, 300.0, 1, 0.0);
    H5File f(p);
    ASSERT_GE(f.id, 0);
    EXPECT_EQ(f.rootAttr("report_2d_variables"), "MINIMAL");
    EXPECT_TRUE(f.has("Mesh2_face_depth"));
    EXPECT_TRUE(f.has("Mesh2_face_head"));
    EXPECT_TRUE(f.has("Mesh2_node_head"));
    EXPECT_TRUE(f.has("Mesh2_node_depth"));
    EXPECT_TRUE(f.has("Mesh2_face_max_depth"));
    EXPECT_FALSE(f.has("Mesh2_face_vx"));
    EXPECT_FALSE(f.has("Mesh2_edge_flux"));
    EXPECT_FALSE(f.has("Mesh2_face_rainfall"));
    EXPECT_FALSE(f.has("Mesh2_face_rain_cum"));
    EXPECT_FALSE(f.has("Mesh2_face_infil_rate"));
    EXPECT_FALSE(f.has("Mesh2_face_species_conc"));
    EXPECT_FALSE(f.has("Mesh2_face_grad_hx"));
}

TEST(Output2DWriter, SpeciesSubsetByName) {
    SolverOptions2D o;
    o.report_2d_species = {"__TEMPERATURE__", "TSS"};   // order in file follows row order
    const fs::path p = writeFile("species_subset", o, 300.0, 1, 0.0);
    H5File f(p);
    ASSERT_GE(f.id, 0);
    ASSERT_TRUE(f.has("Mesh2_face_species_conc"));
    const auto d = f.dims("Mesh2_face_species_conc");
    ASSERT_EQ(d.size(), 3u);
    EXPECT_EQ(d[1], 2u);   // two of three rows
    EXPECT_EQ(f.dsAttr("Mesh2_face_species_conc", "species_names"), "TSS,__TEMPERATURE__");
    const auto v = f.readAll("Mesh2_face_species_conc");
    ASSERT_EQ(v.size(), 4u);
    EXPECT_NEAR(v[0], 1.0, 1e-6);     // TSS cell 0
    EXPECT_NEAR(v[1], 2.0, 1e-6);
    EXPECT_NEAR(v[2], 100.0, 1e-4);   // __TEMPERATURE__ cell 0
    EXPECT_NEAR(v[3], 200.0, 1e-4);
}

// S4b: the writer carries a per-row unit list beside species_names, in the
// same (filtered) order, so a reader never has to guess that the age column
// is hours and the temperature column degC.
TEST(Output2DWriter, SpeciesUnitsAttributeFollowsTheSelectedRows) {
    const std::vector<std::string> kUnits = {"MG/L", "hours", "degC"};
    SolverOptions2D o;
    o.report_2d_species = {"__WATER_AGE__", "TSS"};
    std::error_code ec;
    fs::create_directories(kOutDir, ec);
    const fs::path p = kOutDir / "species_units.h5";
    fs::remove(p, ec);
    MeshData mesh = unitSquare();
    Default2DOutputPlugin plugin(p.string());
    ASSERT_EQ(plugin.initialize({}, nullptr), 0);
    openswmm::SimulationContext ctx{};
    ASSERT_EQ(plugin.validate(ctx), 0);
    ASSERT_EQ(plugin.prepare(ctx), 0);
    ASSERT_TRUE(plugin.configureOutput(o, 300.0).empty());
    plugin.prepareMeshAndDatasets(mesh);
    auto snap = makeSnap(0.0, 0.05, 0.10);
    snap.surface_species_names = &kSpeciesNames;
    snap.surface_species_units = &kUnits;
    ASSERT_EQ(plugin.update(snap), 0);
    ASSERT_EQ(plugin.finalize(ctx), 0);

    H5File f(p);
    ASSERT_GE(f.id, 0);
    EXPECT_EQ(f.dsAttr("Mesh2_face_species_conc", "species_names"), "TSS,__WATER_AGE__");
    EXPECT_EQ(f.dsAttr("Mesh2_face_species_conc", "species_units"), "MG/L,hours");
}

// S4b, end to end: a still pan seeded at 1 hour of age, run for 20 minutes,
// reports 1 + 20/60 hours in the .h5 — hours like every 1D age column — and
// the file says so. Falsifies the pre-S4b behaviour (seconds, units "1").
TEST(Output2DWriter, DeckAgeColumnIsHoursEndToEnd) {
    std::error_code ec;
    fs::create_directories(kOutDir, ec);
    const fs::path inp = kOutDir / "age_hours.inp";
    const fs::path h5  = kOutDir / "age_hours.h5";
    fs::remove(h5, ec);
    {
        std::ofstream f(inp);
        f << "[OPTIONS]\n"
             "FLOW_UNITS           CMS\nFLOW_ROUTING         DYNWAVE\n"
             "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
             "END_DATE             01/01/2026\nEND_TIME             00:20:00\n"
             "REPORT_STEP          00:05:00\nROUTING_STEP         5\n"
             "WATER_AGE            YES\n\n"
             "[POLLUTANTS]\nCu MG/L 0 0 0 0\n\n"
             "[JUNCTIONS]\nJ1 0.0 1.0 0 0 0\n\n"
             "[OUTFALLS]\nO1 -0.5 FREE NO\n\n"
             "[CONDUITS]\nC1 J1 O1 30.0 0.013 0 0 0\n\n"
             "[XSECTIONS]\nC1 CIRCULAR 0.3 0 0 0 1\n\n"
             "[2D_OPTIONS]\nINTEGRATOR EXPLICIT\nLTS_TIERS 1\nMAX_TIMESTEP 5\n"
             "DRY_DEPTH 0.001\nCOUPLING_CD 0.7\nREPORT_2D YES\n"
             "OUTPUT_FILE age_hours.h5\nOUTPUT_PRECISION FLOAT64\n"
             "REPORT_2D_VARIABLES SPECIES\n\n"
             "[2D_VERTICES]\n 0.0 0.0 -10\n10.0 0.0 -10\n10.0 10.0 -10\n 0.0 10.0 -10\n\n"
             "[2D_TRIANGLES]\n0 1 2 0.03 0.5\n0 2 3 0.03 0.5\n\n"
             "[2D_VERTEX_NODE_MAP]\n0 O1 0.7 1.0\n\n"
             "[2D_INITIAL_QUALITY]\n* Cu 2.0\n* __WATER_AGE__ 1\n\n"
             "[REPORT]\nINPUT NO\n";
    }
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    const fs::path rpt = kOutDir / "age_hours.rpt", out = kOutDir / "age_hours.out";
    ASSERT_EQ(swmm_engine_open(e, inp.string().c_str(), rpt.string().c_str(),
                               out.string().c_str(), nullptr), SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(e), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(e, 1), SWMM_OK);
    double elapsed = 0.0;
    while (swmm_engine_step(e, &elapsed) == SWMM_OK && elapsed > 0.0) {}
    swmm_engine_end(e);
    swmm_engine_close(e);
    swmm_engine_destroy(e);

    H5File f(h5);
    ASSERT_GE(f.id, 0) << "no .h5 written";
    ASSERT_TRUE(f.has("Mesh2_face_species_conc"));
    EXPECT_EQ(f.dsAttr("Mesh2_face_species_conc", "species_names"), "Cu,__WATER_AGE__");
    EXPECT_EQ(f.dsAttr("Mesh2_face_species_conc", "species_units"), "MG/L,hours");
    const auto d = f.dims("Mesh2_face_species_conc");     // [time, species, face]
    ASSERT_EQ(d.size(), 3u);
    ASSERT_GE(d[0], 2u);
    ASSERT_EQ(d[1], 2u);
    ASSERT_EQ(d[2], 2u);
    const auto v = f.readAll("Mesh2_face_species_conc");
    const auto t = f.readAll("time");                    // days since start
    ASSERT_EQ(t.size(), d[0]);
    const size_t last = (d[0] - 1) * d[1] * d[2];
    const double age_h_exp = 1.0 + 20.0 / 60.0;
    for (size_t c = 0; c < d[2]; ++c) {
        EXPECT_NEAR(v[last + 1 * d[2] + c], age_h_exp, 1e-6)
            << "age at cell " << c << " must be reported in HOURS";
        EXPECT_NEAR(v[last + 0 * d[2] + c], 2.0, 1e-9) << "Cu at cell " << c;
    }
    // Between records the age column grows by exactly the elapsed time, in
    // hours (the `time` axis is absolute SWMM days; only differences are
    // used). The report-time old/new blend (f47f3bce) shifts a record's state
    // by a fraction of one routing step (5 s here), hence the tolerance.
    for (size_t k = 1; k < d[0]; ++k)
        for (size_t c = 0; c < d[2]; ++c) {
            const double age_k  = v[k * d[1] * d[2] + 1 * d[2] + c];
            const double age_0  = v[0 * d[1] * d[2] + 1 * d[2] + c];
            const double dt_h   = (t[k] - t[0]) * 24.0;
            EXPECT_NEAR(age_k - age_0, dt_h, 5.0 / 3600.0)
                << "record " << k << " cell " << c
                << ": the age column must advance by the elapsed hours";
        }
}

// A 1 mm, 1-min burst at 00:05 on a dry pan, reported every 5 min.
// Mesh2_face_rainfall is reported as the .out reports subcatchment rainfall:
// the gage rate that STARTS at the report instant, so the 00:05 record shows
// the burst (60 mm/hr) and the others zero. Mesh2_face_rain_cum carries the
// burst's exact volume from the next record on — including rain that falls
// wholly between report instants, which the rate never shows.
TEST(Output2DWriter, RainPulseBetweenReportsAccumulates) {
    std::error_code ec;
    fs::create_directories(kOutDir, ec);
    const fs::path inp = kOutDir / "rain_pulse.inp";
    const fs::path h5  = kOutDir / "rain_pulse.h5";
    fs::remove(h5, ec);
    {
        std::ofstream f(inp);
        f << "[OPTIONS]\n"
             "FLOW_UNITS           CMS\nFLOW_ROUTING         DYNWAVE\n"
             "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
             "END_DATE             01/01/2026\nEND_TIME             00:30:00\n"
             "REPORT_STEP          00:05:00\nWET_STEP             00:01:00\n"
             "DRY_STEP             00:01:00\nROUTING_STEP         5\n\n"
             "[RAINGAGES]\nRG1  VOLUME 0:01 1.0 TIMESERIES TS1\n\n"
             "[TIMESERIES]\nTS1  01/01/2026 00:05 1.0\nTS1  01/01/2026 00:06 0.0\n\n"
             "[SYMBOLS]\nRG1 5 5\n\n"
             "[JUNCTIONS]\nJ1 0.0 1.0 0 0 0\n\n"
             "[OUTFALLS]\nO1 -0.5 FREE NO\n\n"
             "[CONDUITS]\nC1 J1 O1 30.0 0.013 0 0 0\n\n"
             "[XSECTIONS]\nC1 CIRCULAR 0.3 0 0 0 1\n\n"
             "[2D_OPTIONS]\nINTEGRATOR EXPLICIT\nLTS_TIERS 1\nMAX_TIMESTEP 5\n"
             "REPORT_2D YES\nREPORT_2D_STEP 00:05:00\nRAINFALL_MODE NATURAL_NEIGHBOUR\n"
             "OUTPUT_FILE rain_pulse.h5\nOUTPUT_PRECISION FLOAT64\n\n"
             "[2D_VERTICES]\n 0.0 0.0 0\n10.0 0.0 0\n10.0 10.0 0\n 0.0 10.0 0\n\n"
             "[2D_TRIANGLES]\n0 1 2 0.03 0.0\n0 2 3 0.03 0.0\n\n"
             "[REPORT]\nINPUT NO\n";
    }
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    const fs::path rpt = kOutDir / "rain_pulse.rpt", out = kOutDir / "rain_pulse.out";
    ASSERT_EQ(swmm_engine_open(e, inp.string().c_str(), rpt.string().c_str(),
                               out.string().c_str(), nullptr), SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(e), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(e, 1), SWMM_OK);
    double elapsed = 0.0;
    while (swmm_engine_step(e, &elapsed) == SWMM_OK && elapsed > 0.0) {}
    swmm_engine_end(e);
    swmm_engine_close(e);
    swmm_engine_destroy(e);

    H5File f(h5);
    ASSERT_GE(f.id, 0) << "no .h5 written";
    ASSERT_TRUE(f.has("Mesh2_face_rainfall"));
    ASSERT_TRUE(f.has("Mesh2_face_rain_cum"));
    const auto d = f.dims("Mesh2_face_rain_cum");   // [time, face]
    ASSERT_EQ(d.size(), 2u);
    ASSERT_EQ(d[0], 6u);   // 00:05 … 00:30
    ASSERT_EQ(d[1], 2u);
    const auto rate = f.readAll("Mesh2_face_rainfall");
    const auto cum  = f.readAll("Mesh2_face_rain_cum");
    const auto area = f.readAll("Mesh2_face_area");
    for (size_t k = 0; k < d[0]; ++k)
        for (size_t c = 0; c < d[1]; ++c) {
            SCOPED_TRACE("record " + std::to_string(k) + " cell " + std::to_string(c));
            // 1 mm in 1 min = 60 mm/hr, from 00:05; the 1D splits runoff
            // steps at record edges, so the volume is exact.
            EXPECT_NEAR(rate[k * d[1] + c], k == 0 ? 60.0e-3 / 3600.0 : 0.0, 1e-18);
            EXPECT_NEAR(cum[k * d[1] + c], k == 0 ? 0.0 : 1.0e-3 * area[c], 1e-12);
        }
}

// Independent piecewise-constant rainfall oracle. Every format describes
// the same depths; no engine gage conversion or clock is used for expectations.
TEST(Output2DWriter, RainFormatsAndMismatchedClocks) {
    fs::create_directories(kOutDir);
    const std::vector<std::vector<double>> depths_mm = {
        {0.2, 0.0, 0.1, 0.15, 0.0, 0.3, 0.1},
        {0.1, 0.2, 0.0, 0.15}, {0.0, 0.3, 0.1}};
    const int intervals[] = {60, 120, 180};
    auto depthAt = [&](int g, double t) {
        double mm = 0;
        for (size_t k = 0; k < depths_mm[g].size(); ++k)
            mm += depths_mm[g][k] * std::clamp(
                (t - k * intervals[g]) / intervals[g], 0.0, 1.0);
        return mm * 0.001;
    };
    auto rateAt = [&](int g, double t) {
        const auto k = static_cast<size_t>(t / intervals[g]);
        return k < depths_mm[g].size() ? depths_mm[g][k] * 0.001 / intervals[g] : 0.0;
    };
    struct Clock { double route, substep; int report, wet, start, sync = 0, first = 0; bool compact = false; };
    const Clock clocks[] = {{7, 2, 17, 37, 0}, {31, 7, 60, 120, 17},
                            {2.5, 0.7, 19, 13, 0}, {7, 2, 17, 37, 0, 29, 68, true}, {31, 7, 53, 120, 17}};
    for (const auto* mode : {"NATURAL_NEIGHBOUR", "NEAREST_NEIGHBOUR"})
    for (const auto* units : {"CMS", "CFS"})
    for (const auto* format : {"INTENSITY", "VOLUME", "CUMULATIVE"})
    for (size_t ci = 0; ci < std::size(clocks); ++ci) {
        const auto& clock = clocks[ci];
        const int first_report = clock.first ? clock.first : clock.report;
        const int output_step = clock.compact ? 2 * clock.report : clock.report;
        const std::string stem = std::string("rain_clocks_") + mode + "_" + units + "_" + format + "_" + std::to_string(ci);
        SCOPED_TRACE(stem);
        const auto inp = kOutDir / (stem + ".inp");
        const auto h5 = kOutDir / (stem + ".h5");
        fs::remove(h5);
        auto hhmmss = [](int seconds) {
            std::ostringstream o;
            o << "00:" << std::setfill('0') << std::setw(2) << seconds / 60
              << ':' << std::setw(2) << seconds % 60;
            return o.str();
        };
        {
            std::ofstream f(inp);
            f << std::setprecision(17)
              << "[OPTIONS]\nFLOW_UNITS " << units << "\nFLOW_ROUTING DYNWAVE\n"
              << "START_DATE 01/01/2026\nSTART_TIME " << hhmmss(clock.start)
              << "\nEND_DATE 01/01/2026\nEND_TIME 00:06:10\nREPORT_STEP " << hhmmss(clock.report)
              << "\nWET_STEP " << hhmmss(clock.wet) << "\nDRY_STEP " << hhmmss(clock.wet)
              << "\nROUTING_STEP " << clock.route << "\nVARIABLE_STEP 0\n"
              << "REPORT_START_DATE 01/01/2026\nREPORT_START_TIME " << hhmmss(clock.start + first_report)
              << "\n[RAINGAGES]\n";
            for (int g = 0; g < 3; ++g)
                f << "G" << g << ' ' << format << " 0:0" << intervals[g] / 60 << " 1 TIMESERIES T" << g << '\n';
            f << "[TIMESERIES]\n";
            for (int g = 0; g < 3; ++g) {
                double counter = 0;
                for (size_t k = 0; k < depths_mm[g].size(); ++k) {
                    if (k == 3) counter = 0; // a real cumulative-gage reset
                    counter += depths_mm[g][k];
                    double v = std::string(format) == "INTENSITY" ? depths_mm[g][k] * 3600.0 / intervals[g]
                             : std::string(format) == "VOLUME" ? depths_mm[g][k] : counter;
                    if (std::string(units) == "CFS") v /= 25.4;
                    f << "T" << g << " 01/01/2026 " << hhmmss(static_cast<int>(k) * intervals[g]) << ' ' << v << '\n';
                }
            }
            f << "[SYMBOLS]\nG0 -50 -50\nG1 70 -50\nG2 10 80\n"
              << "[JUNCTIONS]\nJ 0 1 0 0 0\n[OUTFALLS]\nO -0.5 FREE NO\n"
              << "[CONDUITS]\nC J O 30 0.013 0 0 0\n[XSECTIONS]\nC CIRCULAR 0.3 0 0 0 1\n"
              << "[2D_OPTIONS]\nINTEGRATOR EXPLICIT\nLTS_TIERS 1\nMAX_TIMESTEP " << clock.substep
              << "\nREPORT_2D YES\nOUTPUT_FILE " << h5.filename().string()
              << "\nCOUPLING_SYNC " << clock.sync << "\nREPORT_2D_STEP " << hhmmss(output_step)
              << "\nOUTPUT_PRECISION " << (clock.compact ? "FLOAT32" : "FLOAT64") << "\nRAINFALL_MODE " << mode
              << "\n[2D_VERTICES]\n0 0 0\n10 0 0\n10 10 0\n0 10 0\n"
              << "[2D_TRIANGLES]\n0 1 2 0.03 0\n0 2 3 0.03 0\n[REPORT]\nINPUT NO\n";
        }
        struct Engine {
            SWMM_Engine e = swmm_engine_create();
            ~Engine() { swmm_engine_close(e); swmm_engine_destroy(e); }
        } engine;
        const auto rpt = kOutDir / (stem + ".rpt"), out = kOutDir / (stem + ".out");
        ASSERT_EQ(swmm_engine_open(engine.e, inp.string().c_str(), rpt.string().c_str(), out.string().c_str(), nullptr), SWMM_OK);
        ASSERT_EQ(swmm_engine_initialize(engine.e), SWMM_OK);
        double weights[2][3] = {}, area[2] = {};
        for (int c = 0; c < 2; ++c) {
            int method, count, ids[3]; double w[3];
            ASSERT_EQ(swmm_2d_get_rainfall_weights(engine.e, c, &method, ids, w, 3, &count), SWMM_OK);
            for (int k = 0; k < count; ++k) weights[c][ids[k]] = w[k];
            ASSERT_EQ(swmm_2d_triangle_get_area(engine.e, c, &area[c]), SWMM_OK);
        }
        ASSERT_EQ(swmm_engine_start(engine.e, 1), SWMM_OK);
        // On the batched/delayed-output case, also check persistent ADD
        // forcing on one cell; the other retains pure interpolated rainfall.
        const double added_rate = clock.compact ? 3e-7 : 0.0;
        if (added_rate > 0)
            ASSERT_EQ(swmm_2d_force_rainfall(engine.e, 0, added_rate,
                      SWMM_FORCING_ADD, SWMM_FORCING_PERSIST), SWMM_OK);
        double elapsed;
        do { ASSERT_EQ(swmm_engine_step(engine.e, &elapsed), SWMM_OK); } while (elapsed > 0);
        double final_vol[2], rain_in;
        ASSERT_EQ(swmm_2d_get_rain_volume_bulk(engine.e, final_vol), SWMM_OK);
        ASSERT_EQ(swmm_2d_get_mass_balance(engine.e, nullptr, nullptr, &rain_in, nullptr, nullptr,
                                          nullptr, nullptr, nullptr, nullptr, nullptr), SWMM_OK);
        EXPECT_NEAR(final_vol[0] + final_vol[1], rain_in, 1e-11);
        double expected_final_total = 0;
        for (int c = 0; c < 2; ++c) {
            double expected = c == 0 ? added_rate * (370 - clock.start) * area[c] : 0.0;
            for (int g = 0; g < 3; ++g)
                expected += weights[c][g] * (depthAt(g, 370) - depthAt(g, clock.start)) * area[c];
            expected_final_total += expected;
        }
        // end() flushes a final partial 2D synchronization batch.
        ASSERT_EQ(swmm_engine_end(engine.e), SWMM_OK);
        ASSERT_EQ(swmm_engine_close(engine.e), SWMM_OK);
        H5File f(h5);
        ASSERT_GE(f.id, 0);
        const auto final_mass = f.readAll("mass_balance_2d/rainfall_in");
        ASSERT_EQ(final_mass.size(), 1u);
        EXPECT_NEAR(final_mass[0], expected_final_total, 1e-9);
        EXPECT_EQ(f.dsAttr("time", "units"), "days since 1899-12-30 00:00:00");
        EXPECT_EQ(f.dsAttr("Mesh2_face_rainfall", "units"), "m s-1");
        EXPECT_EQ(f.dsAttr("Mesh2_face_rain_cum", "units"), "m3");
        const auto times = f.readAll("time"), rates = f.readAll("Mesh2_face_rainfall"), cum = f.readAll("Mesh2_face_rain_cum");
        ASSERT_EQ(rates.size(), times.size() * 2);
        ASSERT_EQ(cum.size(), rates.size());
        ASSERT_EQ(times.size(), static_cast<size_t>((370 - clock.start - first_report) / output_step + 1));
        for (size_t k = 0; k < times.size(); ++k) {
            EXPECT_NEAR((times[k] - openswmm::datetime::encodeDate(2026, 1, 1)) * 86400.0,
                        clock.start + first_report + k * output_step + 0.001, 1e-5);
            const double t = clock.start + first_report + k * output_step;
            for (int c = 0; c < 2; ++c) {
                double expected_rate = c == 0 ? added_rate : 0.0;
                double expected_depth = expected_rate * (t - clock.start);
                for (int g = 0; g < 3; ++g) {
                    expected_depth += weights[c][g] * (depthAt(g, t) - depthAt(g, clock.start));
                    // SWMM's report-gage lookup has a legacy +1 s convention.
                    expected_rate += weights[c][g] * rateAt(g, t + 1.001);
                }
                EXPECT_NEAR(rates[2*k+c], expected_rate, 1e-12) << "time " << t << " cell " << c;
                EXPECT_NEAR(cum[2*k+c], expected_depth * area[c], clock.compact ? 5e-9 : 1e-9) << "time " << t << " cell " << c;
            }
        }
    }
}

// S7, end to end: a covered pan under a 1 in/hr hour of rain writes
// Mesh2_face_buildup [time, species, face] in lbs/acre — the same number the
// C API's swmm_2d_get_buildup_bulk reads back at the end — and the store
// falls from the DRY_DAYS seed as the EXP washoff takes it. Falsifies a
// writer that forgets the dataset or a fill that reports the wrong row.
TEST(Output2DWriter, DeckBuildupColumnEndToEnd) {
    std::error_code ec;
    fs::create_directories(kOutDir, ec);
    const fs::path inp = kOutDir / "buildup_h5.inp";
    const fs::path h5  = kOutDir / "buildup_h5.h5";
    fs::remove(h5, ec);
    {
        std::ofstream f(inp);
        f << "[OPTIONS]\n"
             "FLOW_UNITS           CFS\nFLOW_ROUTING         DYNWAVE\n"
             "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
             "END_DATE             01/01/2026\nEND_TIME             01:00:00\n"
             "REPORT_STEP          00:05:00\nWET_STEP             00:05:00\nDRY_STEP             00:05:00\n"
             "ROUTING_STEP         5\nDRY_DAYS             5\n\n"
             "[RAINGAGES]\nRG1  INTENSITY 1:00 1.0 TIMESERIES TS1\n\n"   // one record = one hour
             "[TIMESERIES]\nTS1  01/01/2026 00:00 1.0\nTS1  01/01/2026 01:00 0.0\n\n"
             "[POLLUTANTS]\nTSS  MG/L  0  0  0  0  NO  *  0.0  0  0\n\n"
             "[LANDUSES]\nLU1  0  0  0\n\n"
             "[BUILDUP]\nLU1  TSS  POW  100  2  1  AREA\n\n"
             "[WASHOFF]\nLU1  TSS  EXP  0.5  1.0  0  0\n\n"
             "[JUNCTIONS]\nJ1 0.0 1.0 0 0 0\n\n"
             "[OUTFALLS]\nO1 -0.5 FREE NO\n\n"
             "[CONDUITS]\nC1 J1 O1 30.0 0.013 0 0 0\n\n"
             "[XSECTIONS]\nC1 CIRCULAR 0.3 0 0 0 1\n\n"
             "[2D_OPTIONS]\nINTEGRATOR EXPLICIT\nLTS_TIERS 1\nMAX_TIMESTEP 5\n"
             "DRY_DEPTH 0.001\nCOUPLING_CD 0.7\nREPORT_2D YES\nRAINFALL_MODE SYSTEM\n"
             "OUTPUT_FILE buildup_h5.h5\nOUTPUT_PRECISION FLOAT64\n"
             "REPORT_2D_VARIABLES SPECIES BUILDUP\n\n"
             "[2D_VERTICES]\n 0.0 0.0 5\n10.0 0.0 5\n10.0 10.0 5\n 0.0 10.0 5\n\n"
             "[2D_TRIANGLES]\n0 1 2 0.03 0.0\n0 2 3 0.03 0.0\n\n"
             "[2D_VERTEX_NODE_MAP]\n0 J1 0.7 1.0\n\n"
             "[2D_COVERAGES]\n* LU1 100\n\n"
             "[REPORT]\nINPUT NO\n";
    }
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    const fs::path rpt = kOutDir / "buildup_h5.rpt", out = kOutDir / "buildup_h5.out";
    ASSERT_EQ(swmm_engine_open(e, inp.string().c_str(), rpt.string().c_str(),
                               out.string().c_str(), nullptr), SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(e), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(e, 1), SWMM_OK);
    double elapsed = 0.0;
    while (swmm_engine_step(e, &elapsed) == SWMM_OK && elapsed > 0.0) {}
    double api[2] = {-1.0, -1.0};
    ASSERT_EQ(swmm_2d_get_buildup_bulk(e, "TSS", api, 2), SWMM_OK);
    EXPECT_EQ(swmm_2d_get_buildup_bulk(e, "NOPE", api, 2), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_2d_get_buildup_bulk(e, "TSS", api, 1), SWMM_ERR_BADPARAM);
    swmm_engine_end(e);
    swmm_engine_close(e);
    swmm_engine_destroy(e);

    H5File f(h5);
    ASSERT_GE(f.id, 0) << "no .h5 written";
    ASSERT_TRUE(f.has("Mesh2_face_buildup"));
    EXPECT_EQ(f.dsAttr("Mesh2_face_buildup", "species_names"), "TSS");
    EXPECT_EQ(f.dsAttr("Mesh2_face_buildup", "units"), "lb acre-1");
    const auto d = f.dims("Mesh2_face_buildup");            // [time, species, face]
    ASSERT_EQ(d.size(), 3u);
    ASSERT_GE(d[0], 2u);
    ASSERT_EQ(d[1], 1u);
    ASSERT_EQ(d[2], 2u);
    const auto v = f.readAll("Mesh2_face_buildup");
    // DRY_DAYS 5 at POW(100, 2, 1) seeds min(2·5, 100) = 10 lbs/acre; the
    // hour of rain washes it down monotonically on both cells. The first
    // record is the first REPORT_STEP (not t = 0), so it is already below.
    EXPECT_LT(v[0], 10.0); EXPECT_GT(v[0], 9.0);
    EXPECT_LT(v[1], 10.0); EXPECT_GT(v[1], 9.0);
    const size_t last = (d[0] - 1) * d[2];
    EXPECT_LT(v[last], 10.0);
    EXPECT_GT(v[last], 0.0);
    for (size_t k = 1; k < d[0]; ++k)
        for (size_t c = 0; c < d[2]; ++c)
            EXPECT_LE(v[k * d[2] + c], v[(k - 1) * d[2] + c] + 1e-12) << "record " << k;
    // The C API read the same store the last record holds (the final
    // record is written at end; the API was read before it, one report step
    // apart at most — compare against the last written record loosely).
    EXPECT_NEAR(api[0], v[last], 0.1 * 10.0);
    EXPECT_NEAR(api[1], v[last + 1], 0.1 * 10.0);
}

// G-O, end to end: a [2D_AQUIFER] under a ponded pan (tri, then mixed tri +
// quad) writes the per-cell groundwater fields, the domain ledger series, the
// per-bed node exchange series and the σ columns under GW_DETAILED; the
// water table sits on the bed + hg identity, the ledger's last row closes,
// and a deck without an aquifer writes none of it.
TEST(Output2DWriter, DeckGroundwaterFieldsEndToEnd) {
    std::error_code ec;
    fs::create_directories(kOutDir, ec);
    for (const bool mixed : {false, true}) {
        const std::string stem = mixed ? "gw_h5_mixed" : "gw_h5_tri";
        const fs::path inp = kOutDir / (stem + ".inp");
        const fs::path h5  = kOutDir / (stem + ".h5");
        fs::remove(h5, ec);
        {
            std::ofstream f(inp);
            f << "[OPTIONS]\n"
                 "FLOW_UNITS           CMS\nFLOW_ROUTING         DYNWAVE\n"
                 "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
                 "END_DATE             01/01/2026\nEND_TIME             00:30:00\n"
                 "REPORT_STEP          00:05:00\nWET_STEP             00:01:00\nDRY_STEP             00:01:00\n"
                 "ROUTING_STEP         5\nALLOW_PONDING        NO\n\n"
                 "[JUNCTIONS]\nJ1 0.0 3.0 0 0 0\n\n"
                 "[OUTFALLS]\nO1 -1.0 FREE NO\n\n"
                 "[CONDUITS]\nC1 J1 O1 30.0 0.013 0 0 0\n\n"
                 "[XSECTIONS]\nC1 CIRCULAR 0.5 0 0 0 1\n\n"
                 "[2D_OPTIONS]\nINTEGRATOR EXPLICIT\nLTS_TIERS 1\nMAX_TIMESTEP 5\n"
                 "DRY_DEPTH 0.001\nCOUPLING_CD 0.7\nREPORT_2D YES\n"
                 "OUTPUT_FILE " << stem << ".h5\nOUTPUT_PRECISION FLOAT64\n"
                 "REPORT_2D_VARIABLES DEPTH GROUNDWATER GW_DETAILED\n\n"
                 "[2D_VERTICES]\n0.0 0.0 -10.0\n10.0 0.0 -10.0\n10.0 10.0 -10.0\n0.0 10.0 -10.0\n";
            if (mixed) {
                f << "20.0 0.0 -10.0\n20.0 10.0 -10.0\n\n"
                     "[2D_TRIANGLES]\n0 1 2 0.03 0.5\n0 2 3 0.03 0.5\n\n"
                     "[2D_QUADS]\n1 4 5 2 0.03 0.5\n\n";
            } else {
                f << "\n[2D_TRIANGLES]\n0 1 2 0.03 0.5\n0 2 3 0.03 0.5\n\n";
            }
            f << "[2D_INFILTRATION_DEFAULTS]\n*  CONSTANT  50.0  -  -  -  -\n\n"
                 "[2D_AQUIFER_OPTIONS]\nSOIL_CHAR GARDNER\nCLOSURE SIGMA\nM_LAYERS 6\n\n"
                 "[2D_AQUIFER]\n*  36.0  4.0  0.45  0.10  2.0  HG0 1.0\n\n"
                 "[2D_AQUIFER_NODE]\nJ1  1\n\n"
                 "[REPORT]\nINPUT NO\n";
        }
        SWMM_Engine e = swmm_engine_create();
        ASSERT_NE(e, nullptr);
        const fs::path rpt = kOutDir / (stem + ".rpt"), out = kOutDir / (stem + ".out");
        ASSERT_EQ(swmm_engine_open(e, inp.string().c_str(), rpt.string().c_str(),
                                   out.string().c_str(), nullptr), SWMM_OK);
        ASSERT_EQ(swmm_engine_initialize(e), SWMM_OK);
        ASSERT_EQ(swmm_engine_start(e, 1), SWMM_OK);
        double elapsed = 0.0;
        while (swmm_engine_step(e, &elapsed) == SWMM_OK && elapsed > 0.0) {}
        swmm_engine_end(e);
        swmm_engine_close(e);
        swmm_engine_destroy(e);

        H5File f(h5);
        ASSERT_GE(f.id, 0) << stem << ": no .h5 written";
        const hsize_t nf = mixed ? 3 : 2;
        for (const char* name : {"Mesh2_face_gw_table_elev", "Mesh2_face_gw_hg", "Mesh2_face_gw_hu",
                                 "Mesh2_face_gw_recharge", "Mesh2_face_gw_lateral",
                                 "Mesh2_face_gw_node_exchange", "Mesh2_face_gw_deep",
                                 "Mesh2_face_gw_et", "Mesh2_face_gw_dunne", "Mesh2_face_gw_infil_in",
                                 "Mesh2_face_gw_link_seepage"}) {   // G-X3
            ASSERT_TRUE(f.has(name)) << stem << ": " << name;
            const auto d = f.dims(name);
            ASSERT_EQ(d.size(), 2u) << name;
            EXPECT_GE(d[0], 2u) << name;
            EXPECT_EQ(d[1], nf) << name;
        }
        ASSERT_TRUE(f.has("Mesh2_face_gw_bed_elev"));
        ASSERT_TRUE(f.has("Mesh2_face_gw_closure"));
        ASSERT_TRUE(f.has("groundwater_ledger"));
        ASSERT_TRUE(f.has("groundwater_node_exchange_cum"));
        ASSERT_TRUE(f.has("Mesh2_face_gw_theta_sigma")) << "GW_DETAILED was requested";
        // T7.5: the aquifer's species fields ride the GROUNDWATER mask. This
        // deck carries no [GW_*] transport rows and no pollutants, so the
        // kernel transports nothing and the datasets are ABSENT — the same
        // "a model with no transport gets no variable" rule the surface's
        // species field follows.
        EXPECT_FALSE(f.has("Mesh2_face_gw_sat_conc"))
            << "species datasets appeared on a deck with no transported rows";
        EXPECT_FALSE(f.has("groundwater_species_ledger"));
        EXPECT_EQ(f.dsAttr("groundwater_node_exchange_cum", "node_names"), "J1");
        EXPECT_EQ(f.dsAttr("Mesh2_face_gw_hg", "units"), "m");
        // water table = bed + hg, per record, per cell
        const auto te  = f.readAll("Mesh2_face_gw_table_elev");
        const auto hg  = f.readAll("Mesh2_face_gw_hg");
        const auto bed = f.readAll("Mesh2_face_gw_bed_elev");
        ASSERT_EQ(bed.size(), nf);
        ASSERT_EQ(te.size(), hg.size());
        for (size_t i = 0; i < te.size(); ++i)
            EXPECT_NEAR(te[i], bed[i % nf] + hg[i], 1e-9) << stem << " record/cell " << i;
        // the bed sits ZS = 4 m under the cell bed (−10)
        for (size_t c = 0; c < nf; ++c) EXPECT_NEAR(bed[c], -14.0, 1e-9);
        // the σ block is [time, 6, nf] and every cell is closure 2 (sigma)
        const auto dth = f.dims("Mesh2_face_gw_theta_sigma");
        ASSERT_EQ(dth.size(), 3u);
        EXPECT_EQ(dth[1], 6u);
        EXPECT_EQ(dth[2], nf);
        // the ledger's last row: storage − init − (in − out) == residual, and
        // the residual is machine-small against the storage
        const auto led = f.readAll("groundwater_ledger");
        const auto dl  = f.dims("groundwater_ledger");
        ASSERT_EQ(dl.size(), 2u);
        ASSERT_EQ(dl[1], 14u);   // Residual stays at 11; source in/out append at 12/13.
        EXPECT_EQ(f.dsAttr("groundwater_ledger", "terms"),
                  "recharge,lateral,deep,node,dunne,caprise,et,infil_in,"
                  "init_storage,storage,link,continuity_residual,source_in,source_out");
        ASSERT_GT(dl[0], 0u);
        const size_t last = (dl[0] - 1) * dl[1];
        const double storage = led[last + 9], init = led[last + 8], resid = led[last + 11];
        EXPECT_EQ(led[last + 10], 0.0) << "no conduit seeps on this deck";
        EXPECT_EQ(led[last + 12], 0.0) << "no named sources on this deck";
        EXPECT_EQ(led[last + 13], 0.0) << "no named sinks on this deck";
        EXPECT_GT(storage, 0.0);
        EXPECT_GT(led[last + 7], 0.0) << "the aquifer received infiltration";
        EXPECT_LT(std::fabs(resid), 1e-6 * storage + 1e-9) << stem << ": residual " << resid;
        if (std::getenv("OPENSWMM_TEST_VERBOSE")) {
            const auto nx = f.readAll("groundwater_node_exchange_cum");
            std::fprintf(stderr, "[%s] ledger last: init %.6g storage %.6g infil_in %.6g recharge %.6g "
                         "node %.6g dunne %.6g resid %.3e; J1 exchange cum %.6g; hg[0] %.6g\n",
                         stem.c_str(), init, storage, led[last + 7], led[last + 0], led[last + 3],
                         led[last + 4], resid, nx.empty() ? 0.0 : nx.back(), hg[hg.size() - nf]);
        }
    }
    // …and a deck without an aquifer writes none of it.
    {
        const fs::path h5 = kOutDir / "age_hours.h5";   // written by DeckAgeColumnIsHoursEndToEnd
        H5File f(h5);
        if (f.id >= 0) {
            EXPECT_FALSE(f.has("Mesh2_face_gw_hg"));
            EXPECT_FALSE(f.has("groundwater_ledger"));
        }
    }
}

TEST(Output2DWriter, Report2DStepThinsTheTimeAxis) {
    SolverOptions2D o;
    o.report_2d_step = 3600.0;   // 12 × the 300 s report step
    std::string err;
    // 25 report ticks of 300 s → t = 0..2 h → 2D writes at 0, 1 h, 2 h = 3.
    const fs::path p = writeFile("step_1h", o, 300.0, 25, 300.0 / 86400.0, &err);
    EXPECT_TRUE(err.empty()) << err;
    H5File f(p);
    ASSERT_GE(f.id, 0);
    EXPECT_EQ(f.dims("time")[0], 3u);
    EXPECT_EQ(f.dims("Mesh2_face_depth")[0], 3u);
    const auto t = f.readAll("time");
    EXPECT_NEAR(t[1] * 86400.0, 3600.0, 1e-6);
}

TEST(Output2DWriter, Report2DStepMustBeAMultipleOfReportStep) {
    SolverOptions2D o;
    o.report_2d_step = 1000.0;   // not a multiple of 300
    std::string err;
    (void)writeFile("step_bad", o, 300.0, 1, 0.0, &err);
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("multiple"), std::string::npos);
}

TEST(Output2DWriter, Float32FileIsSmaller) {
    SolverOptions2D a;  a.output_precision = OutputPrecision2D::FLOAT64; a.output_compression = 0;
    SolverOptions2D b;  b.output_precision = OutputPrecision2D::FLOAT32; b.output_compression = 0;
    a.report_2d_vars = b.report_2d_vars = report2d::ALL_MASK;
    const fs::path pa = writeFile("size_f64", a, 300.0, 400, 300.0 / 86400.0);
    const fs::path pb = writeFile("size_f32", b, 300.0, 400, 300.0 / 86400.0);
    const auto sa = fs::file_size(pa), sb = fs::file_size(pb);
    EXPECT_GT(sa, sb);

    // The ratio is measured on the PAYLOAD, not the whole file.
    //
    // Chunks here are one tick wide ({1, n_faces}), so on this 2-cell mesh a
    // chunk holds 8-16 bytes of data behind ~47 bytes of chunk-index entry —
    // precision-blind overhead that grows with every tick. Whole-file ratio
    // therefore does not approach 2.0 with more ticks, it converges DOWN to
    // about 1.18, which is why a 1.3 whole-file floor could never pass at this
    // mesh size (measured 1.155 at 400 ticks). The bytes OUTPUT_PRECISION
    // governs are exactly the allocated chunk bytes of the state datasets, and
    // those halve exactly: 400 ticks x 2 faces x 8 B vs 4 B.
    const H5File fa(pa), fb(pb);
    ASSERT_GE(fa.id, 0);
    ASSERT_GE(fb.id, 0);
    for (const char* ds : {"Mesh2_face_depth", "Mesh2_face_head",
                           "Mesh2_node_head", "Mesh2_edge_flux"}) {
        SCOPED_TRACE(ds);
        const auto ba = fa.datasetBytes(ds), bb = fb.datasetBytes(ds);
        ASSERT_GT(bb, 0u) << "dataset absent or unallocated under ALL_MASK";
        EXPECT_EQ(ba, bb * 2) << "float32 payload is not half of float64";
    }
    // `time` is deliberately float64 in both files — OUTPUT_PRECISION thins the
    // state fields, not the axis every reader keys its lookup on.
    EXPECT_EQ(fa.datasetBytes("time"), fb.datasetBytes("time"));
}
