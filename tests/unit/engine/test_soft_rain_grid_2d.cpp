/**
 * @file test_soft_rain_grid_2d.cpp
 * @brief Focused SR-2c tests for deterministic 2D gridded rainfall forcing.
 *
 * @ingroup engine_2d
 */

#include <gtest/gtest.h>

#define private public
#include "2d/SurfaceRouter2D.hpp"
#undef private

#include "2d/mesh/MeshBuilder.hpp"
#include "core/SimulationContext.hpp"
#include "uncertainty/UncertaintyConfig.hpp"

#include <hdf5.h>

#include <cstdio>
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

TEST(SoftRainGrid2D, LognormalHighCvWarnsOnce) {
    SimulationContext ctx;
    ctx.options.flow_units = FlowUnits::CMS;
    ctx.gage_names.add("RG1");
    ctx.gages.resize(1);
    ctx.gages.rainfall[0] = 0.0;

    SurfaceRouter2D router;
    router.mesh() = makeUnitSquareMesh();
    router.state().resize(router.mesh().n_triangles(), router.mesh().n_vertices());

    // With loc values 20/30 mm/hr after centroid mapping, spread values of 20/30
    // imply CV = 1.0 on both active cells, exceeding the 0.5 SR-3c guard.
    const std::string grid_path = makeGridFile("LOGNORMAL", "CV", 0.0f, 20.0f, 30.0f, 0.0f);
    SoftGridSourceSpec spec;
    spec.file_path = grid_path;
    spec.target = GridTarget::TWO_D;
    spec.mapping = GridMapping::CENTROID;
    spec.force_location = true;

    ASSERT_TRUE(router.initGridRainfall(spec, ""));
    router.updateRainfall(ctx);
    ASSERT_EQ(ctx.warnings.size(), 1u);
    EXPECT_NE(ctx.warnings[0].find("CV > 0.5"), std::string::npos);

    // Second call should not emit an additional warning.
    router.updateRainfall(ctx);
    EXPECT_EQ(ctx.warnings.size(), 1u);

    std::remove(grid_path.c_str());
}
