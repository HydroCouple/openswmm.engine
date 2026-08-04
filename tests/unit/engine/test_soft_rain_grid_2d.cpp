/**
 * @file test_soft_rain_grid_2d.cpp
 * @brief Focused SR-2c tests for deterministic 2D gridded rainfall forcing.
 *
 * @details Only the deterministic /location path is exercised — the two
 *          cases this file originally carried for the /spread-consuming
 *          soft-forcing extension (CL-1c correlated coherence, the SR-3c
 *          lognormal-CV warning) are not: that machinery reads state
 *          (SpectralROM soft-forcing, grid_soft_* fields) which is not part
 *          of this port. See SurfaceRouter2D.hpp's initGridRainfall() note.
 *
 * @ingroup engine_2d
 */

#include <gtest/gtest.h>

// updateRainfall() is private (it's an internal step of the co-advance
// cycle, not public API); reach it directly here rather than drive the
// whole engine lifecycle just to exercise one function.
#define private public
#include "2d/SurfaceRouter2D.hpp"
#undef private

#include "2d/mesh/MeshBuilder.hpp"
#include "core/SimulationContext.hpp"
#include "core/SWMMEngine.hpp"
#include "uncertainty/UncertaintyConfig.hpp"

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_2d.h>

#include <hdf5.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using openswmm::FlowUnits;
using openswmm::SimulationContext;
using openswmm::twoD::MeshData;
using openswmm::twoD::SurfaceRouter2D;
using openswmm::uncertainty::GridMapping;
using openswmm::uncertainty::GridTarget;
using openswmm::uncertainty::SoftGridSourceSpec;

namespace {

static MeshData makeUnitSquareMesh() {
    MeshData mesh;
    mesh.resize_vertices(4);
    mesh.vx = {0.0, 1.0, 0.0, 1.0};
    mesh.vy = {0.0, 0.0, 1.0, 1.0};
    mesh.vz = {0.0, 0.0, 0.0, 0.0};

    mesh.resize_triangles(2);
    mesh.tri_v0[0] = 0; mesh.tri_v1[0] = 1; mesh.tri_v2[0] = 3; // centroid (2/3,1/3)
    mesh.tri_v0[1] = 0; mesh.tri_v1[1] = 3; mesh.tri_v2[1] = 2; // centroid (1/3,2/3)
    mesh.mannings_n[0] = 0.035;
    mesh.mannings_n[1] = 0.035;

    buildMeshTopology(mesh);
    return mesh;
}

void writeStringAttr(hid_t loc, const char* name, const char* value) {
    hid_t atype = H5Tcopy(H5T_C_S1);
    H5Tset_size(atype, std::strlen(value));
    H5Tset_strpad(atype, H5T_STR_NULLTERM);
    hid_t aspace = H5Screate(H5S_SCALAR);
    hid_t attr = H5Acreate2(loc, name, atype, aspace, H5P_DEFAULT, H5P_DEFAULT);
    H5Awrite(attr, atype, value);
    H5Aclose(attr);
    H5Sclose(aspace);
    H5Tclose(atype);
}

std::string makeGridFile(const char* family = "NORMAL",
                         const char* spread_kind = "SD",
                         float s0 = 0.0f, float s1 = 0.0f,
                         float s2 = 0.0f, float s3 = 0.0f) {
    std::string path = "/tmp/test_soft_rain_grid_2d.h5";
    hid_t file = H5Fcreate(path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);

    writeStringAttr(file, "family", family);
    writeStringAttr(file, "spread_kind", spread_kind);
    writeStringAttr(file, "units", "mm/hr");

    // time = [0]
    {
        double time_data[1] = {0.0};
        hsize_t dims[1] = {1};
        hid_t space = H5Screate_simple(1, dims, nullptr);
        hid_t ds = H5Dcreate2(file, "/time", H5T_NATIVE_DOUBLE, space,
                              H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        H5Dwrite(ds, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, time_data);
        H5Dclose(ds);
        H5Sclose(space);
    }

    // x = [0,1], y = [0,1]
    {
        double x_data[2] = {0.0, 1.0};
        hsize_t dims[1] = {2};
        hid_t space = H5Screate_simple(1, dims, nullptr);
        hid_t ds = H5Dcreate2(file, "/x", H5T_NATIVE_DOUBLE, space,
                              H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        H5Dwrite(ds, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, x_data);
        H5Dclose(ds);
        H5Sclose(space);
    }
    {
        double y_data[2] = {0.0, 1.0};
        hsize_t dims[1] = {2};
        hid_t space = H5Screate_simple(1, dims, nullptr);
        hid_t ds = H5Dcreate2(file, "/y", H5T_NATIVE_DOUBLE, space,
                              H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        H5Dwrite(ds, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, y_data);
        H5Dclose(ds);
        H5Sclose(space);
    }

    // spread dummy [1,2,2]
    {
        float spread[4] = {s0, s1, s2, s3};
        hsize_t dims[3] = {1, 2, 2};
        hid_t space = H5Screate_simple(3, dims, nullptr);
        hid_t ds = H5Dcreate2(file, "/spread", H5T_NATIVE_FLOAT, space,
                              H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        H5Dwrite(ds, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, spread);
        H5Dclose(ds);
        H5Sclose(space);
    }

    // location [1,2,2] row-major: [[10,20],[30,40]] mm/hr
    {
        float loc[4] = {10.0f, 20.0f, 30.0f, 40.0f};
        hsize_t dims[3] = {1, 2, 2};
        hid_t space = H5Screate_simple(3, dims, nullptr);
        hid_t ds = H5Dcreate2(file, "/location", H5T_NATIVE_FLOAT, space,
                              H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        H5Dwrite(ds, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, loc);
        H5Dclose(ds);
        H5Sclose(space);
    }

    H5Fclose(file);
    return path;
}

} // anonymous namespace

TEST(SoftRainGrid2D, NoGridSourceUsesLegacyGageFallback) {
    SimulationContext ctx;
    ctx.options.flow_units = FlowUnits::CMS; // SI => mm/hr -> m/s
    ctx.gage_names.add("RG1");
    ctx.gages.resize(1);
    ctx.gages.rainfall[0] = 12.0; // mm/hr

    SurfaceRouter2D router;
    router.mesh() = makeUnitSquareMesh();
    router.state().resize(router.mesh().n_triangles(), router.mesh().n_vertices());

    router.updateRainfall(ctx);

    const double expected = 12.0 * 0.001 / 3600.0;
    ASSERT_EQ(router.state().rainfall.size(), 2u);
    EXPECT_DOUBLE_EQ(router.state().rainfall[0], expected);
    EXPECT_DOUBLE_EQ(router.state().rainfall[1], expected);
}

TEST(SoftRainGrid2D, ForceLocationGridOverridesGageFallback) {
    SimulationContext ctx;
    ctx.options.flow_units = FlowUnits::CMS; // SI => mm/hr -> m/s
    ctx.gage_names.add("RG1");
    ctx.gages.resize(1);
    ctx.gages.rainfall[0] = 999.0; // should be ignored when grid forcing active

    SurfaceRouter2D router;
    router.mesh() = makeUnitSquareMesh();
    router.state().resize(router.mesh().n_triangles(), router.mesh().n_vertices());

    const std::string grid_path = makeGridFile();
    SoftGridSourceSpec spec;
    spec.file_path = grid_path;
    spec.target = GridTarget::TWO_D;
    spec.mapping = GridMapping::CENTROID;
    spec.force_location = true;

    ASSERT_TRUE(router.initGridRainfall(spec, ""));
    ASSERT_TRUE(router.gridRainfallActive());

    router.updateRainfall(ctx);

    // T0 centroid (2/3,1/3) -> nearest cell center (1,0) -> flat index 1 -> 20 mm/hr
    // T1 centroid (1/3,2/3) -> nearest cell center (0,1) -> flat index 2 -> 30 mm/hr
    const double expected_t0 = 20.0 * 0.001 / 3600.0;
    const double expected_t1 = 30.0 * 0.001 / 3600.0;
    ASSERT_EQ(router.state().rainfall.size(), 2u);
    EXPECT_DOUBLE_EQ(router.state().rainfall[0], expected_t0);
    EXPECT_DOUBLE_EQ(router.state().rainfall[1], expected_t1);

    std::remove(grid_path.c_str());
}

// ============================================================================
// End-to-end: [SOFT_RAINFALL_GRID] parsing -> initHydraulics() trigger ->
// SurfaceRouter2D::initGridRainfall(), through the real engine lifecycle.
// The two tests above call initGridRainfall() directly; this one is the
// guard that the wiring reaching it — registerSoftRainfallGridSection and
// the uncertainty_config_.grid_sources scan in SWMMEngine::initHydraulics()
// — actually connects to a real .inp.
// ============================================================================

namespace {

std::string buildGridModel(const std::string& grid_path) {
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
        " 0.0   0.0  0.0\n"
        " 1.0   0.0  0.0\n"
        " 0.0   1.0  0.0\n"
        " 1.0   1.0  0.0\n"
        "\n"
        "[2D_TRIANGLES]\n"
        ";;V1  V2  V3  MANNINGS_N\n"
        "0     1   3   0.035\n"
        "0     3   2   0.035\n"
        "\n"
        "[2D_VERTEX_NODE_MAP]\n;;Vertex  Node  Cd   Area\n0  J1  0.7  1.0\n"
        "\n[SOFT_RAINFALL_GRID]\n"
        ";;Target File Mapping [Options]\n"
        "2D  \"" + grid_path + "\"  CENTROID  FORCE_LOCATION\n";
}

}  // namespace

TEST(SoftRainGrid2D, InpSectionReachesInitGridRainfallThroughTheRealEngine) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::current_path() / "soft_rain_grid_2d_out";
    fs::create_directories(dir);

    const std::string grid_path = makeGridFile();
    const fs::path inp = dir / "grid_end_to_end.inp";
    const fs::path rpt = dir / "grid_end_to_end.rpt";
    const fs::path out = dir / "grid_end_to_end.out";
    { std::ofstream f(inp); f << buildGridModel(grid_path); }

    SWMM_Engine eng = swmm_engine_create();
    ASSERT_EQ(swmm_engine_open(eng, inp.string().c_str(), rpt.string().c_str(),
                               out.string().c_str(), nullptr), SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(eng), SWMM_OK);

    int active = 0;
    swmm_2d_is_active(eng, &active);
    ASSERT_TRUE(active);

    auto* impl = static_cast<openswmm::SWMMEngine*>(eng);
    EXPECT_TRUE(impl->surfaceRouter2D().gridRainfallActive())
        << "[SOFT_RAINFALL_GRID] FORCE_LOCATION did not reach initGridRainfall()";

    swmm_engine_close(eng);
    swmm_engine_destroy(eng);
    std::remove(grid_path.c_str());
}
