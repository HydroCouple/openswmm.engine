/**
 * @file test_grid_file_reader.cpp
 * @brief Unit tests for GridFileReader (SR-2a).
 *
 * @details Tests the HDF5 grid-file reader defined in SOFT_RAINFALL_DESIGN.md
 *          §3.3/§4.2. Fixtures are generated in SetUp() via the HDF5 C API
 *          (tiny 4×3 grid, 5 time steps) — no committed HDF5 fixtures.
 *
 *          Test coverage:
 *          - Schema validation accept/reject (missing dataset, dimension
 *            mismatch, unknown family attr, non-monotonic /time)
 *          - Plane streaming values exact
 *          - Temporal interpolation midpoint check
 *          - /location-absent behavior (reader reports has_location=false)
 *
 * @ingroup engine_uncertainty
 */

#include <gtest/gtest.h>

#include "uncertainty/GridFileReader.hpp"

#include <hdf5.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using openswmm::GridFileReader;
using openswmm::GridFamily;
using openswmm::GridSpreadKind;

namespace {

// ============================================================================
// Fixture helpers — write tiny HDF5 grid files via the C API
// ============================================================================

/// Write a string attribute on an HDF5 object.
void write_string_attr(hid_t loc, const char* name, const char* value) {
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

/// Create a valid grid file with the given parameters.
/// Grid: 4 columns (nx=4) × 3 rows (ny=3), 5 time steps.
struct GridFileParams {
    bool include_location = true;
    bool include_family_attr = true;
    bool include_spread_kind_attr = true;
    const char* family = "NORMAL";
    const char* spread_kind = "SD";
    const char* units = "mm/hr";
    const char* crs = nullptr;  // null = omit
    bool non_monotonic_time = false;
    bool wrong_spread_dims = false;
    bool omit_time = false;
    bool omit_x = false;
    bool omit_y = false;
    bool omit_spread = false;
};

/// Write a grid file to @p path per @p params. Returns true on success.
bool write_grid_file(const std::string& path, const GridFileParams& p) {
    hid_t file = H5Fcreate(path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    if (file < 0) return false;

    const int T = 5;
    const int ny = 3;
    const int nx = 4;

    // Root attributes
    if (p.include_family_attr)
        write_string_attr(file, "family", p.family);
    if (p.include_spread_kind_attr)
        write_string_attr(file, "spread_kind", p.spread_kind);
    write_string_attr(file, "units", p.units);
    if (p.crs)
        write_string_attr(file, "crs", p.crs);

    // /time (T) float64
    if (!p.omit_time) {
        std::vector<double> time_data(T);
        for (int t = 0; t < T; ++t)
            time_data[t] = p.non_monotonic_time ? (t == 2 ? 50.0 : 100.0 * t) : 100.0 * t;
        hsize_t tdims[1] = { static_cast<hsize_t>(T) };
        hid_t tspace = H5Screate_simple(1, tdims, nullptr);
        hid_t tds = H5Dcreate2(file, "/time", H5T_NATIVE_DOUBLE, tspace,
                               H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        H5Dwrite(tds, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, time_data.data());
        H5Dclose(tds);
        H5Sclose(tspace);
    }

    // /x (nx) float64
    if (!p.omit_x) {
        std::vector<double> x_data(nx);
        for (int i = 0; i < nx; ++i) x_data[i] = 100.0 * i;
        hsize_t xdims[1] = { static_cast<hsize_t>(nx) };
        hid_t xspace = H5Screate_simple(1, xdims, nullptr);
        hid_t xds = H5Dcreate2(file, "/x", H5T_NATIVE_DOUBLE, xspace,
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        H5Dwrite(xds, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, x_data.data());
        H5Dclose(xds);
        H5Sclose(xspace);
    }

    // /y (ny) float64
    if (!p.omit_y) {
        std::vector<double> y_data(ny);
        for (int i = 0; i < ny; ++i) y_data[i] = 200.0 * i;
        hsize_t ydims[1] = { static_cast<hsize_t>(ny) };
        hid_t yspace = H5Screate_simple(1, ydims, nullptr);
        hid_t yds = H5Dcreate2(file, "/y", H5T_NATIVE_DOUBLE, yspace,
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        H5Dwrite(yds, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, y_data.data());
        H5Dclose(yds);
        H5Sclose(yspace);
    }

    // /spread (T, ny, nx) float32
    if (!p.omit_spread) {
        std::vector<float> spread_data(T * ny * nx);
        for (int t = 0; t < T; ++t)
            for (int j = 0; j < ny; ++j)
                for (int i = 0; i < nx; ++i)
                    spread_data[t * ny * nx + j * nx + i] =
                        static_cast<float>(0.1 * (t + 1) + 0.01 * (j * nx + i));

        if (p.wrong_spread_dims) {
            // Create as 2-D instead of 3-D
            hsize_t sdims[2] = { static_cast<hsize_t>(ny), static_cast<hsize_t>(nx) };
            hid_t sspace = H5Screate_simple(2, sdims, nullptr);
            hid_t sds = H5Dcreate2(file, "/spread", H5T_NATIVE_FLOAT, sspace,
                                   H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
            H5Dwrite(sds, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     spread_data.data());
            H5Dclose(sds);
            H5Sclose(sspace);
        } else {
            hsize_t sdims[3] = { static_cast<hsize_t>(T),
                                 static_cast<hsize_t>(ny),
                                 static_cast<hsize_t>(nx) };
            hid_t sspace = H5Screate_simple(3, sdims, nullptr);
            hid_t sds = H5Dcreate2(file, "/spread", H5T_NATIVE_FLOAT, sspace,
                                   H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
            H5Dwrite(sds, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     spread_data.data());
            H5Dclose(sds);
            H5Sclose(sspace);
        }
    }

    // /location (T, ny, nx) float32 — optional
    if (p.include_location) {
        std::vector<float> loc_data(T * ny * nx);
        for (int t = 0; t < T; ++t)
            for (int j = 0; j < ny; ++j)
                for (int i = 0; i < nx; ++i)
                    loc_data[t * ny * nx + j * nx + i] =
                        static_cast<float>(1.0 * (t + 1) + 0.1 * (j * nx + i));

        hsize_t ldims[3] = { static_cast<hsize_t>(T),
                             static_cast<hsize_t>(ny),
                             static_cast<hsize_t>(nx) };
        hid_t lspace = H5Screate_simple(3, ldims, nullptr);
        hid_t lds = H5Dcreate2(file, "/location", H5T_NATIVE_FLOAT, lspace,
                               H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        H5Dwrite(lds, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                 loc_data.data());
        H5Dclose(lds);
        H5Sclose(lspace);
    }

    H5Fclose(file);
    return true;
}

/// Temporary file path for tests (unique per test via PID).
std::string test_file_path(const char* suffix) {
    return "/tmp/test_grid_reader_" + std::to_string(getpid()) + "_" + suffix + ".h5";
}

} // anonymous namespace

// ============================================================================
// Test suite: GridFileReaderSchema
// ============================================================================

class GridFileReaderSchema : public ::testing::Test {
protected:
    void SetUp() override {
        valid_path_ = test_file_path("valid");
        GridFileParams p;
        write_grid_file(valid_path_, p);
    }

    void TearDown() override {
        std::remove(valid_path_.c_str());
    }

    std::string valid_path_;
};

TEST_F(GridFileReaderSchema, OpensValidFile) {
    GridFileReader reader;
    ASSERT_TRUE(reader.open(valid_path_)) << reader.last_error();
    EXPECT_EQ(reader.n_time(), 5);
    EXPECT_EQ(reader.nx(), 4);
    EXPECT_EQ(reader.ny(), 3);
    EXPECT_EQ(reader.family(), GridFamily::NORMAL);
    EXPECT_EQ(reader.spread_kind(), GridSpreadKind::SD);
    EXPECT_EQ(reader.units(), "mm/hr");
    EXPECT_TRUE(reader.has_location());
}

TEST_F(GridFileReaderSchema, RejectsMissingSpread) {
    std::string path = test_file_path("no_spread");
    GridFileParams p;
    p.omit_spread = true;
    write_grid_file(path, p);

    GridFileReader reader;
    EXPECT_FALSE(reader.open(path));
    EXPECT_NE(reader.last_error().find("/spread"), std::string::npos);
    std::remove(path.c_str());
}

TEST_F(GridFileReaderSchema, RejectsMissingTime) {
    std::string path = test_file_path("no_time");
    GridFileParams p;
    p.omit_time = true;
    write_grid_file(path, p);

    GridFileReader reader;
    EXPECT_FALSE(reader.open(path));
    EXPECT_NE(reader.last_error().find("/time"), std::string::npos);
    std::remove(path.c_str());
}

TEST_F(GridFileReaderSchema, RejectsMissingX) {
    std::string path = test_file_path("no_x");
    GridFileParams p;
    p.omit_x = true;
    write_grid_file(path, p);

    GridFileReader reader;
    EXPECT_FALSE(reader.open(path));
    EXPECT_NE(reader.last_error().find("/x"), std::string::npos);
    std::remove(path.c_str());
}

TEST_F(GridFileReaderSchema, RejectsMissingY) {
    std::string path = test_file_path("no_y");
    GridFileParams p;
    p.omit_y = true;
    write_grid_file(path, p);

    GridFileReader reader;
    EXPECT_FALSE(reader.open(path));
    EXPECT_NE(reader.last_error().find("/y"), std::string::npos);
    std::remove(path.c_str());
}

TEST_F(GridFileReaderSchema, RejectsWrongSpreadDims) {
    std::string path = test_file_path("wrong_dims");
    GridFileParams p;
    p.wrong_spread_dims = true;
    write_grid_file(path, p);

    GridFileReader reader;
    EXPECT_FALSE(reader.open(path));
    EXPECT_NE(reader.last_error().find("3-D"), std::string::npos);
    std::remove(path.c_str());
}

TEST_F(GridFileReaderSchema, RejectsUnknownFamily) {
    std::string path = test_file_path("bad_family");
    GridFileParams p;
    p.family = "GAMMA";
    write_grid_file(path, p);

    GridFileReader reader;
    EXPECT_FALSE(reader.open(path));
    EXPECT_NE(reader.last_error().find("family"), std::string::npos);
    std::remove(path.c_str());
}

TEST_F(GridFileReaderSchema, RejectsNonMonotonicTime) {
    std::string path = test_file_path("non_mono");
    GridFileParams p;
    p.non_monotonic_time = true;
    write_grid_file(path, p);

    GridFileReader reader;
    EXPECT_FALSE(reader.open(path));
    EXPECT_NE(reader.last_error().find("strictly increasing"), std::string::npos);
    std::remove(path.c_str());
}

TEST_F(GridFileReaderSchema, LocationAbsentReportsFalse) {
    std::string path = test_file_path("no_loc");
    GridFileParams p;
    p.include_location = false;
    write_grid_file(path, p);

    GridFileReader reader;
    ASSERT_TRUE(reader.open(path)) << reader.last_error();
    EXPECT_FALSE(reader.has_location());
    std::remove(path.c_str());
}

TEST_F(GridFileReaderSchema, SupportsAllFamilies) {
    for (const char* fam : {"NORMAL", "LOGNORMAL", "UNIFORM", "MIXED"}) {
        std::string path = test_file_path(fam);
        GridFileParams p;
        p.family = fam;
        write_grid_file(path, p);

        GridFileReader reader;
        ASSERT_TRUE(reader.open(path)) << reader.last_error();
        if (std::string(fam) == "NORMAL")    EXPECT_EQ(reader.family(), GridFamily::NORMAL);
        else if (std::string(fam) == "LOGNORMAL") EXPECT_EQ(reader.family(), GridFamily::LOGNORMAL);
        else if (std::string(fam) == "UNIFORM")   EXPECT_EQ(reader.family(), GridFamily::UNIFORM);
        else if (std::string(fam) == "MIXED")     EXPECT_EQ(reader.family(), GridFamily::MIXED);
        std::remove(path.c_str());
    }
}

// ============================================================================
// Test suite: GridFileReaderStreaming
// ============================================================================

class GridFileReaderStreaming : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = test_file_path("stream");
        GridFileParams p;
        write_grid_file(path_, p);
    }

    void TearDown() override {
        std::remove(path_.c_str());
    }

    std::string path_;
};

TEST_F(GridFileReaderStreaming, PlanesAreExact) {
    GridFileReader reader;
    ASSERT_TRUE(reader.open(path_)) << reader.last_error();

    ASSERT_TRUE(reader.advance());
    ASSERT_TRUE(reader.has_current());
    EXPECT_EQ(reader.current_index(), 0);

    // Spread plane at t=0: 0.1*(0+1) + 0.01*(j*nx+i)
    const float* sp = reader.spread_now();
    ASSERT_NE(sp, nullptr);
    for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 4; ++i) {
            float expected = static_cast<float>(0.1 * 1 + 0.01 * (j * 4 + i));
            EXPECT_FLOAT_EQ(sp[j * 4 + i], expected)
                << " at (j=" << j << ", i=" << i << ")";
        }

    // Location plane at t=0: 1.0*(0+1) + 0.1*(j*nx+i)
    const float* loc = reader.location_now();
    ASSERT_NE(loc, nullptr);
    for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 4; ++i) {
            float expected = static_cast<float>(1.0 * 1 + 0.1 * (j * 4 + i));
            EXPECT_FLOAT_EQ(loc[j * 4 + i], expected)
                << " at (j=" << j << ", i=" << i << ")";
        }
}

TEST_F(GridFileReaderStreaming, AdvanceThroughAllPlanes) {
    GridFileReader reader;
    ASSERT_TRUE(reader.open(path_)) << reader.last_error();

    int count = 0;
    while (reader.advance()) {
        ASSERT_TRUE(reader.has_current());
        EXPECT_EQ(reader.current_index(), count);
        EXPECT_EQ(reader.time_now(), 100.0 * count);
        ++count;
    }
    EXPECT_EQ(count, 5);
    EXPECT_FALSE(reader.has_current());
}

TEST_F(GridFileReaderStreaming, NextPlaneAvailable) {
    GridFileReader reader;
    ASSERT_TRUE(reader.open(path_)) << reader.last_error();

    ASSERT_TRUE(reader.advance());
    // At t=0, next should be t=1
    ASSERT_NE(reader.spread_next(), nullptr);
    EXPECT_EQ(reader.time_next(), 100.0);

    // Advance to t=4 (last plane) — next should be null
    for (int i = 0; i < 4; ++i)
        ASSERT_TRUE(reader.advance());

    EXPECT_EQ(reader.current_index(), 4);
    EXPECT_EQ(reader.spread_next(), nullptr);
    EXPECT_EQ(reader.time_next(), reader.time_now());
}

TEST_F(GridFileReaderStreaming, TemporalInterpolationMidpoint) {
    GridFileReader reader;
    ASSERT_TRUE(reader.open(path_)) << reader.last_error();

    ASSERT_TRUE(reader.advance());
    // At t=0, next is t=1. Midpoint time = 50.0
    double t0 = reader.time_now();   // 0.0
    double t1 = reader.time_next();   // 100.0
    double alpha = 0.5;  // midpoint
    EXPECT_DOUBLE_EQ(t0 + alpha * (t1 - t0), 50.0);

    // Interpolate spread at midpoint for pixel (0,0)
    const float* sp0 = reader.spread_now();
    const float* sp1 = reader.spread_next();
    ASSERT_NE(sp0, nullptr);
    ASSERT_NE(sp1, nullptr);

    float mid = 0.5f * (sp0[0] + sp1[0]);
    float expected = 0.5f * (static_cast<float>(0.1 * 1) + static_cast<float>(0.1 * 2));
    EXPECT_FLOAT_EQ(mid, expected);
}

TEST_F(GridFileReaderStreaming, LocationNullWhenAbsent) {
    std::string path = test_file_path("no_loc_stream");
    GridFileParams p;
    p.include_location = false;
    write_grid_file(path, p);

    GridFileReader reader;
    ASSERT_TRUE(reader.open(path)) << reader.last_error();
    ASSERT_TRUE(reader.advance());
    EXPECT_EQ(reader.location_now(), nullptr);
    EXPECT_EQ(reader.location_next(), nullptr);
    EXPECT_NE(reader.spread_now(), nullptr);

    std::remove(path.c_str());
}

TEST_F(GridFileReaderStreaming, CoordinateAxesCorrect) {
    GridFileReader reader;
    ASSERT_TRUE(reader.open(path_)) << reader.last_error();

    const auto& x = reader.x_coords();
    const auto& y = reader.y_coords();
    ASSERT_EQ(x.size(), 4u);
    ASSERT_EQ(y.size(), 3u);
    EXPECT_DOUBLE_EQ(x[0], 0.0);
    EXPECT_DOUBLE_EQ(x[1], 100.0);
    EXPECT_DOUBLE_EQ(x[3], 300.0);
    EXPECT_DOUBLE_EQ(y[0], 0.0);
    EXPECT_DOUBLE_EQ(y[1], 200.0);
    EXPECT_DOUBLE_EQ(y[2], 400.0);
}

TEST_F(GridFileReaderStreaming, CannotOpenNonexistentFile) {
    GridFileReader reader;
    EXPECT_FALSE(reader.open("/tmp/does_not_exist_grid_reader_test.h5"));
    EXPECT_FALSE(reader.last_error().empty());
}