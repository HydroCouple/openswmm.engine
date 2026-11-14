// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include <gtest/gtest.h>
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_output2d.h>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

#ifdef OPENSWMM_HAS_2D_HDF5_OUTPUT
#include <hdf5.h>
namespace {
void textAttribute(hid_t object, const char* name, const char* value) {
    hid_t type = H5Tcopy(H5T_C_S1);
    ASSERT_GE(H5Tset_size(type, H5T_VARIABLE), 0);
    hid_t space = H5Screate(H5S_SCALAR);
    hid_t attr = H5Acreate2(object, name, type, space, H5P_DEFAULT, H5P_DEFAULT);
    ASSERT_GE(H5Awrite(attr, type, &value), 0);
    H5Aclose(attr); H5Sclose(space); H5Tclose(type);
}
hid_t dataset(hid_t file, const char* name, const std::vector<hsize_t>& dims,
              const void* values, hid_t type = H5T_NATIVE_DOUBLE) {
    hid_t space = dims.empty() ? H5Screate(H5S_SCALAR) : H5Screate_simple(dims.size(), dims.data(), nullptr);
    hid_t ds = H5Dcreate2(file, name, type, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    EXPECT_GE(ds, 0);
    EXPECT_GE(H5Dwrite(ds, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, values), 0);
    H5Sclose(space);
    return ds;
}
std::string fixture() {
    const auto directory = std::filesystem::path(OUTPUT2D_TEST_DIRECTORY);
    std::filesystem::create_directories(directory);
    const auto path = (directory / "mixed.h5").string();
    hid_t file = H5Fcreate(path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    double coords[]{0, 1, 0, 1};
    for (const char* axis : {"Mesh2_node_x", "Mesh2_node_y", "Mesh2_node_z"})
        H5Dclose(dataset(file, axis, {4}, coords));
    int cells[]{1, 2, 3, -1, 1, 2, 4, 3};
    H5Dclose(dataset(file, "Mesh2_face_nodes", {2, 4}, cells, H5T_NATIVE_INT));
    double dates[]{46023, 46023.5, 46024};
    hid_t time = dataset(file, "time", {3}, dates);
    textAttribute(time, "units", "days since 1899-12-30 00:00:00"); H5Dclose(time);
    float depths[]{1, 2, 3, 4, 5, 6};
    hid_t depth = dataset(file, "Mesh2_face_depth", {3, 2}, depths, H5T_NATIVE_FLOAT);
    textAttribute(depth, "units", "m"); H5Dclose(depth);
    double layers[3 * 2 * 4 * 2];
    for (int i = 0; i < 48; ++i) layers[i] = i;
    hid_t layer = dataset(file, "Mesh2_face_gw_layer_conc", {3, 2, 4, 2}, layers);
    textAttribute(layer, "layout", "[time,species,layer,face]"); H5Dclose(layer);
    hid_t group = H5Gcreate2(file, "diagnostics", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    double balance = 12.5;
    H5Dclose(dataset(group, "balance", {}, &balance));
    const char* names[]{"TSS", "Dye α"};
    hid_t strings = H5Tcopy(H5T_C_S1);
    H5Tset_size(strings, H5T_VARIABLE); H5Tset_cset(strings, H5T_CSET_UTF8);
    H5Dclose(dataset(group, "names", {2}, names, strings));
    H5Tclose(strings);
    H5Gclose(group); H5Fclose(file);
    return path;
}
class Output2D : public ::testing::Test {
protected:
    SWMM_Output2D reader = nullptr;
    void SetUp() override { ASSERT_EQ(swmm_output2d_open(fixture().c_str(), &reader), SWMM_OK); }
    void TearDown() override { swmm_output2d_close(reader); }
};
}

TEST_F(Output2D, DiscoversNestedNumericAndStringDatasets) {
    EXPECT_EQ(swmm_output2d_available(), 1);
    size_t required = 0;
    ASSERT_EQ(swmm_output2d_datasets(reader, nullptr, 0, &required), SWMM_OK);
    std::vector<char> names(required);
    ASSERT_EQ(swmm_output2d_datasets(reader, names.data(), names.size(), &required), SWMM_OK);
    EXPECT_NE(std::string(names.data()).find("/diagnostics/names"), std::string::npos);
    int rank, integer; uint64_t dims[8], unlimited;
    ASSERT_EQ(swmm_output2d_shape(reader, "/Mesh2_face_gw_layer_conc", &rank, dims, &integer, &unlimited), SWMM_OK);
    EXPECT_EQ(rank, 4); EXPECT_EQ(dims[2], 4); EXPECT_EQ(integer, 0);
    EXPECT_EQ(swmm_output2d_shape(reader, "/missing", &rank, dims, &integer, &unlimited), SWMM_ERR_BADINDEX);
    EXPECT_NE(std::string(swmm_output2d_error()).find("not found"), std::string::npos);
}

TEST_F(Output2D, ReadsBoundedSlabsAndScalars) {
    uint64_t start[]{0, 1}, count[]{2, 1}, stride[]{2, 1};
    double values[2]{};
    ASSERT_EQ(swmm_output2d_read_double(reader, "/Mesh2_face_depth", start, count, stride, values, 2), SWMM_OK);
    EXPECT_EQ(values[0], 2); EXPECT_EQ(values[1], 6);
    uint64_t s[]{1, 1, 2, 1}, c[]{1, 1, 2, 1}, step[]{1, 1, 1, 1};
    ASSERT_EQ(swmm_output2d_read_double(reader, "/Mesh2_face_gw_layer_conc", s, c, step, values, 2), SWMM_OK);
    EXPECT_EQ(values[0], 29); EXPECT_EQ(values[1], 31);
    ASSERT_EQ(swmm_output2d_read_double(reader, "/diagnostics/balance", nullptr, nullptr, nullptr, values, 1), SWMM_OK);
    EXPECT_EQ(values[0], 12.5);
    uint64_t cs[]{0, 0}, cc[]{2, 4}, st[]{1, 1};
    int64_t cells[8]{};
    ASSERT_EQ(swmm_output2d_read_int64(reader, "/Mesh2_face_nodes", cs, cc, st, cells, 8), SWMM_OK);
    EXPECT_EQ(cells[3], -1); EXPECT_EQ(cells[6], 4);
}

TEST_F(Output2D, ValidatesBeforeWritingAndAllowsEmptySelections) {
    uint64_t start[]{0, 0}, count[]{3, 2}, stride[]{1, 1};
    double sentinel = -999;
    EXPECT_EQ(swmm_output2d_read_double(reader, "/Mesh2_face_depth", start, count, stride, &sentinel, 1), SWMM_ERR_BADPARAM);
    EXPECT_EQ(sentinel, -999);
    count[0] = std::numeric_limits<uint64_t>::max();
    EXPECT_EQ(swmm_output2d_read_double(reader, "/Mesh2_face_depth", start, count, stride, &sentinel, 1), SWMM_ERR_BADINDEX);
    count[0] = 0;
    EXPECT_EQ(swmm_output2d_read_double(reader, "/Mesh2_face_depth", start, count, stride, nullptr, 0), SWMM_OK);
    stride[0] = 0;
    EXPECT_EQ(swmm_output2d_read_double(reader, "/Mesh2_face_depth", start, count, stride, nullptr, 0), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_output2d_datasets(nullptr, nullptr, 0, nullptr), SWMM_ERR_BADHANDLE);
}

TEST_F(Output2D, ReadsVariableStringAttributesAndIdentityTables) {
    size_t needed;
    ASSERT_EQ(swmm_output2d_attribute(reader, "/time", "units", nullptr, 0, &needed), SWMM_OK);
    std::vector<char> value(needed);
    ASSERT_EQ(swmm_output2d_attribute(reader, "/time", "units", value.data(), value.size(), &needed), SWMM_OK);
    EXPECT_EQ(std::string(value.data()), "days since 1899-12-30 00:00:00");
    ASSERT_EQ(swmm_output2d_strings(reader, "/diagnostics/names", nullptr, 0, &needed), SWMM_OK);
    value.resize(needed);
    ASSERT_EQ(swmm_output2d_strings(reader, "/diagnostics/names", value.data(), value.size(), &needed), SWMM_OK);
    EXPECT_EQ(std::string(value.data()), "TSS\nDye α\n");
    char tiny[]{'X', '\0'};
    EXPECT_EQ(swmm_output2d_strings(reader, "/diagnostics/names", tiny, 2, &needed), SWMM_ERR_BADPARAM);
    EXPECT_EQ(tiny[0], 'X');
}

TEST(Output2DFiles, RejectsMissingAndNonResultFiles) {
    SWMM_Output2D reader = nullptr;
    EXPECT_EQ(swmm_output2d_open("not-a-result-file.h5", &reader), SWMM_ERR_IO);
    EXPECT_EQ(reader, nullptr);
    EXPECT_EQ(swmm_output2d_open(nullptr, &reader), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_output2d_open("irrelevant", nullptr), SWMM_ERR_BADPARAM);
    swmm_output2d_close(nullptr);
    const auto invalid = (std::filesystem::path(OUTPUT2D_TEST_DIRECTORY) / "not_results.h5").string();
    hid_t file = H5Fcreate(invalid.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    ASSERT_GE(file, 0);
    H5Fclose(file);
    EXPECT_EQ(swmm_output2d_open(invalid.c_str(), &reader), SWMM_ERR_BADINDEX);
    EXPECT_EQ(reader, nullptr);
}
#else
TEST(Output2DUnavailable, StableABIWithoutOptionalDependencies) {
    EXPECT_EQ(swmm_output2d_available(), 0);
    SWMM_Output2D reader = nullptr;
    EXPECT_EQ(swmm_output2d_open("anything.h5", &reader), SWMM_ERR_PLUGIN);
    EXPECT_EQ(reader, nullptr);
    EXPECT_FALSE(std::string(swmm_output2d_error()).empty());
    swmm_output2d_close(reader);
}
#endif
