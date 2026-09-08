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
#include <filesystem>
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
#include "openswmm/plugin_sdk/SimulationSnapshot.hpp"

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_model.h>

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
    EXPECT_EQ(swmm_2d_output_variable_count(), 11);
    EXPECT_STREQ(swmm_2d_output_variable_name(0), "DEPTH");
    EXPECT_STREQ(swmm_2d_output_variable_name(10), "ENVELOPES");
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
    // Enough ticks that the payload dominates the HDF5 metadata.
    const fs::path pa = writeFile("size_f64", a, 300.0, 400, 300.0 / 86400.0);
    const fs::path pb = writeFile("size_f32", b, 300.0, 400, 300.0 / 86400.0);
    const auto sa = fs::file_size(pa), sb = fs::file_size(pb);
    EXPECT_GT(sa, sb);
    // On a 2-cell mesh metadata is a large share; still expect a clear win.
    EXPECT_GT(static_cast<double>(sa) / static_cast<double>(sb), 1.3);
}
