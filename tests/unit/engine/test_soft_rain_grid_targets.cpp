/**
 * @file test_soft_rain_grid_targets.cpp
 * @brief Focused SR-2d tests for RUNOFF and INFLOWS grid-target staging.
 *
 * @ingroup engine_uncertainty
 */

#include <gtest/gtest.h>

#define private public
#include "core/SWMMEngine.hpp"
#undef private

#include "uncertainty/UncertaintyConfig.hpp"

#include <hdf5.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

using openswmm::ForcingMode;
using openswmm::SWMMEngine;
using openswmm::uncertainty::GridMapping;
using openswmm::uncertainty::GridTarget;
using openswmm::uncertainty::SoftGridSourceSpec;

namespace {

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

std::string makeGridFile() {
    std::string path = "/tmp/test_soft_rain_grid_targets.h5";
    hid_t file = H5Fcreate(path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);

    writeStringAttr(file, "family", "NORMAL");
    writeStringAttr(file, "spread_kind", "SD");
    writeStringAttr(file, "units", "mm/hr");

    { double t[1] = {0.0}; hsize_t d[1] = {1}; hid_t s = H5Screate_simple(1, d, nullptr);
      hid_t ds = H5Dcreate2(file, "/time", H5T_NATIVE_DOUBLE, s, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
      H5Dwrite(ds, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, t); H5Dclose(ds); H5Sclose(s); }
    { double x[2] = {0.0, 1.0}; hsize_t d[1] = {2}; hid_t s = H5Screate_simple(1, d, nullptr);
      hid_t ds = H5Dcreate2(file, "/x", H5T_NATIVE_DOUBLE, s, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
      H5Dwrite(ds, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, x); H5Dclose(ds); H5Sclose(s); }
    { double y[2] = {0.0, 1.0}; hsize_t d[1] = {2}; hid_t s = H5Screate_simple(1, d, nullptr);
      hid_t ds = H5Dcreate2(file, "/y", H5T_NATIVE_DOUBLE, s, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
      H5Dwrite(ds, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, y); H5Dclose(ds); H5Sclose(s); }
    { float spread[4] = {0,0,0,0}; hsize_t d[3] = {1,2,2}; hid_t s = H5Screate_simple(3, d, nullptr);
      hid_t ds = H5Dcreate2(file, "/spread", H5T_NATIVE_FLOAT, s, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
      H5Dwrite(ds, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, spread); H5Dclose(ds); H5Sclose(s); }
    { float loc[4] = {10.0f, 20.0f, 30.0f, 40.0f}; hsize_t d[3] = {1,2,2}; hid_t s = H5Screate_simple(3, d, nullptr);
      hid_t ds = H5Dcreate2(file, "/location", H5T_NATIVE_FLOAT, s, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
      H5Dwrite(ds, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, loc); H5Dclose(ds); H5Sclose(s); }

    H5Fclose(file);
    return path;
}

TEST(SoftRainGridTargets, RunoffTargetStagesOverrideRainfall) {
    SWMMEngine eng;
    eng.ctx_.subcatch_names.add("S1");
    eng.ctx_.subcatches.grow_to(1);
    eng.ctx_.spatial.subcatch_x.resize(1, 1.0);  // nearest x=1
    eng.ctx_.spatial.subcatch_y.resize(1, 0.0);  // nearest y=0 -> flat index 1 -> 20
    eng.ctx_.forcing.resize(/*nodes=*/0, /*links=*/0, /*subcatches=*/1, /*gages=*/0, /*polluts=*/0);

    SoftGridSourceSpec spec;
    spec.file_path = makeGridFile();
    spec.target = GridTarget::RUNOFF;
    spec.mapping = GridMapping::CENTROID;
    spec.force_location = true;
    eng.uncertainty_config_.grid_sources.push_back(spec);

    eng.initSoftGridRuntimes();
    ASSERT_EQ(eng.soft_grid_runtimes_.size(), 1u);

    eng.stageSoftGridForcings();
    EXPECT_EQ(eng.ctx_.forcing.subcatch_rainfall_mode[0], ForcingMode::OVERRIDE);
    EXPECT_DOUBLE_EQ(eng.ctx_.forcing.subcatch_rainfall_value[0], 20.0);

    std::remove(spec.file_path.c_str());
}

TEST(SoftRainGridTargets, InflowsTargetStagesAdditiveNodeFlow) {
    SWMMEngine eng;
    eng.ctx_.node_names.add("N1");
    eng.ctx_.node_names.add("N2");
    eng.ctx_.nodes.grow_to(2);
    eng.ctx_.spatial.node_x = {0.0, 1.0};
    eng.ctx_.spatial.node_y = {1.0, 0.0};
    eng.ctx_.forcing.resize(/*nodes=*/2, /*links=*/0, /*subcatches=*/0, /*gages=*/0, /*polluts=*/0);

    const std::string grid_path = makeGridFile();
    const std::string nodes_path = "/tmp/test_soft_rain_grid_targets_nodes.txt";
    {
        std::ofstream f(nodes_path);
        f << "N2\n";
    }

    SoftGridSourceSpec spec;
    spec.file_path = grid_path;
    spec.target = GridTarget::INFLOWS;
    spec.mapping = GridMapping::CENTROID;
    spec.force_location = true;
    spec.nodes_file = nodes_path;
    eng.uncertainty_config_.grid_sources.push_back(spec);

    eng.initSoftGridRuntimes();
    ASSERT_EQ(eng.soft_grid_runtimes_.size(), 1u);

    eng.stageSoftGridForcings();
    EXPECT_EQ(eng.ctx_.forcing.node_lat_inflow_mode[0], ForcingMode::NONE);
    EXPECT_EQ(eng.ctx_.forcing.node_lat_inflow_mode[1], ForcingMode::ADD);
    EXPECT_DOUBLE_EQ(eng.ctx_.forcing.node_lat_inflow_value[1], 20.0);

    std::remove(grid_path.c_str());
    std::remove(nodes_path.c_str());
}

} // anonymous namespace
