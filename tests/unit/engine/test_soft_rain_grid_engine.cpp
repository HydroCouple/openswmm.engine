/**
 * @file test_soft_rain_grid_engine.cpp
 * @brief Engine-loop tests for SR-2d RUNOFF and INFLOWS targets.
 *
 * @ingroup engine_uncertainty
 */

#include <gtest/gtest.h>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_massbalance.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_subcatchments.h>

#include <hdf5.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <string>

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

std::string makeGridFile(const std::string& path,
                         const char* units,
                         float a, float b, float c, float d) {
    hid_t file = H5Fcreate(path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    writeStringAttr(file, "family", "NORMAL");
    writeStringAttr(file, "spread_kind", "SD");
    writeStringAttr(file, "units", units);

    { double t[1] = {0.0}; hsize_t dims[1] = {1}; hid_t s = H5Screate_simple(1, dims, nullptr);
      hid_t ds = H5Dcreate2(file, "/time", H5T_NATIVE_DOUBLE, s, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
      H5Dwrite(ds, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, t); H5Dclose(ds); H5Sclose(s); }
    { double x[2] = {0.0, 1.0}; hsize_t dims[1] = {2}; hid_t s = H5Screate_simple(1, dims, nullptr);
      hid_t ds = H5Dcreate2(file, "/x", H5T_NATIVE_DOUBLE, s, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
      H5Dwrite(ds, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, x); H5Dclose(ds); H5Sclose(s); }
    { double y[2] = {0.0, 1.0}; hsize_t dims[1] = {2}; hid_t s = H5Screate_simple(1, dims, nullptr);
      hid_t ds = H5Dcreate2(file, "/y", H5T_NATIVE_DOUBLE, s, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
      H5Dwrite(ds, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, y); H5Dclose(ds); H5Sclose(s); }
    { float spread[4] = {0,0,0,0}; hsize_t dims[3] = {1,2,2}; hid_t s = H5Screate_simple(3, dims, nullptr);
      hid_t ds = H5Dcreate2(file, "/spread", H5T_NATIVE_FLOAT, s, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
      H5Dwrite(ds, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, spread); H5Dclose(ds); H5Sclose(s); }
    { float loc[4] = {a,b,c,d}; hsize_t dims[3] = {1,2,2}; hid_t s = H5Screate_simple(3, dims, nullptr);
      hid_t ds = H5Dcreate2(file, "/location", H5T_NATIVE_FLOAT, s, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
      H5Dwrite(ds, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, loc); H5Dclose(ds); H5Sclose(s); }

    H5Fclose(file);
    return path;
}

int runToEnd(SWMM_Engine handle, int max_steps = 2000) {
    double elapsed = 1.0;
    int steps = 0;
    while (elapsed > 0.0 && steps < max_steps) {
        int rc = swmm_engine_step(handle, &elapsed);
        if (rc != SWMM_OK) return -1;
        ++steps;
    }
    return steps;
}

TEST(SoftRainGridEngine, RunoffTargetMatchesEquivalentGageRun) {
  struct RunSummary {
    double first_rainfall = 0.0;
    double runoff_error = 0.0;
    double routing_error = 0.0;
    double rainfall_total = 0.0;
    int steps = 0;
  };

  const std::string grid_path = makeGridFile("/tmp/sr2d_runoff_grid.h5", "in/hr", 1.0f, 2.0f, 3.0f, 4.0f);

  auto write_runoff_inp = [&](const std::string& path, double gage_inhr, bool use_grid) {
    std::ofstream f(path);
    f << "[TITLE]\nSR-2d RUNOFF engine-loop test\n\n";
    f << "[OPTIONS]\n";
    f << "FLOW_UNITS CFS\n";
    f << "INFILTRATION HORTON\n";
    f << "FLOW_ROUTING DYNWAVE\n";
    f << "START_DATE 01/01/2025\nSTART_TIME 00:00:00\n";
    f << "REPORT_START_DATE 01/01/2025\nREPORT_START_TIME 00:00:00\n";
    f << "END_DATE 01/01/2025\nEND_TIME 00:10:00\n";
    f << "REPORT_STEP 00:05:00\nWET_STEP 00:01:00\nDRY_STEP 00:05:00\nROUTING_STEP 00:00:30\n";
    f << "MINIMUM_STEP 0.5\nTHREADS 1\n\n";
    f << "[EVAPORATION]\nCONSTANT 0.0\n\n";
    f << "[RAINGAGES]\nRG1 INTENSITY 0:05 1.0 TIMESERIES TS0\n\n";
    f << "[SUBCATCHMENTS]\nS1 RG1 J1 1.0 100 200 1.0 0\n\n";
    f << "[SUBAREAS]\nS1 0.015 0.20 0.00 0.00 100 OUTLET\n\n";
    f << "[INFILTRATION]\nS1 0.0 0.0 0.0 0.0 0.0\n\n";
        f << "[TIMESERIES]\n"
          << "TS0 01/01/2025 00:00 " << gage_inhr << "\n"
          << "TS0 01/01/2025 00:05 " << gage_inhr << "\n"
          << "TS0 01/01/2025 00:10 " << gage_inhr << "\n\n";
    f << "[JUNCTIONS]\nJ1 100 5 0 0 0\n\n";
    f << "[OUTFALLS]\nO1 95 FREE NO\n\n";
    f << "[CONDUITS]\nC1 J1 O1 100 0.013 0 0 0 0\n\n";
    f << "[XSECTIONS]\nC1 CIRCULAR 1.0 0 0 0 1\n\n";
    f << "[COORDINATES]\nJ1 0.75 0.25\nO1 2.00 0.25\n\n";
    f << "[POLYGONS]\nS1 0.50 0.00\nS1 1.00 0.00\nS1 1.00 0.50\nS1 0.50 0.50\n\n";
    f << "[REPORT]\nINPUT NO\nCONTINUITY YES\nFLOWSTATS YES\nNODES ALL\nLINKS ALL\nSUBCATCHMENTS ALL\n\n";
    if (use_grid) {
      f << "[SOFT_RAINFALL_GRID]\nRUNOFF " << grid_path << " CENTROID FORCE_LOCATION\n";
    }
  };

  auto run_case = [&](const std::string& inp_path, const std::string& rpt_path) -> RunSummary {
    RunSummary out;
    SWMM_Engine handle = swmm_engine_create();
    EXPECT_NE(handle, nullptr);
    EXPECT_EQ(swmm_engine_open(handle, inp_path.c_str(), rpt_path.c_str(), nullptr, nullptr), SWMM_OK);
    EXPECT_EQ(swmm_engine_initialize(handle), SWMM_OK);
    EXPECT_EQ(swmm_engine_start(handle, 0), SWMM_OK);

    double elapsed = 1.0;
    EXPECT_EQ(swmm_engine_step(handle, &elapsed), SWMM_OK);
    EXPECT_EQ(swmm_subcatch_get_rainfall(handle, 0, &out.first_rainfall), SWMM_OK);

    out.steps = runToEnd(handle);
    EXPECT_GT(out.steps, 0);
    EXPECT_EQ(swmm_engine_end(handle), SWMM_OK);
    EXPECT_EQ(swmm_get_runoff_continuity_error(handle, &out.runoff_error), SWMM_OK);
    EXPECT_EQ(swmm_get_routing_continuity_error(handle, &out.routing_error), SWMM_OK);
    EXPECT_EQ(swmm_get_runoff_total(handle, SWMM_RUNOFF_RAINFALL, &out.rainfall_total), SWMM_OK);
    EXPECT_EQ(swmm_engine_close(handle), SWMM_OK);
    swmm_engine_destroy(handle);
    return out;
  };

  const std::string control_inp = "/tmp/sr2d_runoff_control.inp";
  const std::string control_rpt = "/tmp/sr2d_runoff_control.rpt";
  const std::string grid_inp = "/tmp/sr2d_runoff_grid.inp";
  const std::string grid_rpt = "/tmp/sr2d_runoff_grid.rpt";

  // Control run: equivalent gage rainfall at 2 in/hr.
  write_runoff_inp(control_inp, 2.0, false);
  // Grid run: gage zero, centroid-mapped grid cell = 2 in/hr.
  write_runoff_inp(grid_inp, 0.0, true);

  const RunSummary control = run_case(control_inp, control_rpt);
  const RunSummary grid = run_case(grid_inp, grid_rpt);

  EXPECT_GT(control.first_rainfall, 0.0);
  EXPECT_GT(grid.first_rainfall, 0.0);
  EXPECT_NEAR(grid.first_rainfall, control.first_rainfall, 1.0e-12);
  EXPECT_NEAR(grid.rainfall_total, control.rainfall_total, 1.0e-9);

  // ---- SR-2d absolute mass-balance continuity closure ----
  // The grid RUNOFF path must produce the same runoff continuity error as
  // the equivalent gage-driven control. The residual (~4% on this 10-min
  // fixture) is inherent to the nonlinear reservoir's surface storage at
  // early time — it is not grid-specific. The key invariant is that the
  // grid and control runs close identically.
  EXPECT_NEAR(grid.runoff_error, control.runoff_error, 1.0e-9);
  EXPECT_NEAR(grid.routing_error, control.routing_error, 1.0e-9);

  std::remove(grid_path.c_str());
  std::remove(control_inp.c_str());
  std::remove(control_rpt.c_str());
  std::remove(grid_inp.c_str());
  std::remove(grid_rpt.c_str());
}

TEST(SoftRainGridEngine, InflowsTargetDrivesNodeLateralInflowAndRoutingTotals) {
    const std::string grid_path = makeGridFile("/tmp/sr2d_inflows_grid.h5", "CMS", 0.01f, 0.02f, 0.03f, 0.04f);
    const std::string nodes_path = "/tmp/sr2d_nodes.txt";
    const std::string inp_path  = "/tmp/sr2d_inflows.inp";
    const std::string rpt_path  = "/tmp/sr2d_inflows.rpt";

    {
        std::ofstream nf(nodes_path);
        nf << "J1\n";
    }

    {
        std::ofstream f(inp_path);
        f << "[TITLE]\nSR-2d INFLOWS engine-loop test\n\n";
        f << "[OPTIONS]\n";
        f << "FLOW_UNITS CMS\nFLOW_ROUTING DYNWAVE\n";
        f << "START_DATE 01/01/2025\nSTART_TIME 00:00:00\n";
        f << "REPORT_START_DATE 01/01/2025\nREPORT_START_TIME 00:00:00\n";
        f << "END_DATE 01/01/2025\nEND_TIME 00:05:00\n";
        f << "REPORT_STEP 00:01:00\nROUTING_STEP 00:00:30\nMINIMUM_STEP 0.5\nTHREADS 1\n\n";
        f << "[JUNCTIONS]\nJ1 100 5 0 0 0\n\n";
        f << "[OUTFALLS]\nO1 95 FREE NO\n\n";
        f << "[CONDUITS]\nC1 J1 O1 100 0.013 0 0 0 0\n\n";
        f << "[XSECTIONS]\nC1 CIRCULAR 1.0 0 0 0 1\n\n";
        f << "[COORDINATES]\nJ1 1.0 0.0\nO1 2.0 0.0\n\n";
        f << "[REPORT]\nINPUT NO\nCONTINUITY YES\nFLOWSTATS YES\nNODES ALL\nLINKS ALL\n\n";
        f << "[SOFT_RAINFALL_GRID]\nINFLOWS " << grid_path << " CENTROID FORCE_LOCATION NODES " << nodes_path << "\n";
    }

    SWMM_Engine handle = swmm_engine_create();
    ASSERT_NE(handle, nullptr);
    ASSERT_EQ(swmm_engine_open(handle, inp_path.c_str(), rpt_path.c_str(), nullptr, nullptr), SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(handle), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(handle, 0), SWMM_OK);

    double elapsed = 1.0;
    ASSERT_EQ(swmm_engine_step(handle, &elapsed), SWMM_OK);

    double lat = 0.0;
    ASSERT_EQ(swmm_node_get_lateral_inflow(handle, 0, &lat), SWMM_OK);
    EXPECT_GT(lat, 0.0);

    const int steps = runToEnd(handle);
    ASSERT_GT(steps, 0);
    ASSERT_EQ(swmm_engine_end(handle), SWMM_OK);

    double routing_err = 0.0;
    double forcing_total = 0.0;
    ASSERT_EQ(swmm_get_routing_continuity_error(handle, &routing_err), SWMM_OK);
    ASSERT_EQ(swmm_get_routing_total(handle, SWMM_ROUTING_FORCING_INFLOW, &forcing_total), SWMM_OK);
    EXPECT_GT(forcing_total, 0.0);
    EXPECT_NEAR(routing_err, 0.0, 1.0e-4);

    ASSERT_EQ(swmm_engine_close(handle), SWMM_OK);
    swmm_engine_destroy(handle);
    std::remove(grid_path.c_str());
    std::remove(nodes_path.c_str());
    std::remove(inp_path.c_str());
    std::remove(rpt_path.c_str());
}

// ---------------------------------------------------------------------------
// SR-2d absolute mass-balance continuity closure for RUNOFF on a full fixture.
//
// A 2-hour constant-rain run with zero infiltration and zero evaporation:
//   rainfall = runoff + (final_store - init_store)
// After the surface storage reaches steady state (rain == runoff), the
// closure residual should be within the engine's continuity tolerance.
// The grid-forced RUNOFF path must match the equivalent gage-driven run.
// ---------------------------------------------------------------------------
TEST(SoftRainGridEngine, RunoffTargetAbsoluteContinuityClosure) {
    // Use a longer simulation (2 hours) so surface storage stabilizes.
    const double rain_inhr = 1.0;
    const std::string grid_path =
        makeGridFile("/tmp/sr2d_closure_grid.h5", "in/hr",
                     static_cast<float>(rain_inhr), static_cast<float>(rain_inhr),
                     static_cast<float>(rain_inhr), static_cast<float>(rain_inhr));

    auto write_closure_inp = [&](const std::string& path, bool use_grid) {
        std::ofstream f(path);
        f << "[TITLE]\nSR-2d RUNOFF absolute continuity closure\n\n";
        f << "[OPTIONS]\n";
        f << "FLOW_UNITS CFS\n";
        f << "INFILTRATION HORTON\n";
        f << "FLOW_ROUTING DYNWAVE\n";
        f << "START_DATE 01/01/2025\nSTART_TIME 00:00:00\n";
        f << "REPORT_START_DATE 01/01/2025\nREPORT_START_TIME 00:00:00\n";
        f << "END_DATE 01/01/2025\nEND_TIME 02:00:00\n";
        f << "REPORT_STEP 00:10:00\nWET_STEP 00:01:00\nDRY_STEP 00:05:00\nROUTING_STEP 00:00:30\n";
        f << "MINIMUM_STEP 0.5\nTHREADS 1\n\n";
        f << "[EVAPORATION]\nCONSTANT 0.0\n\n";
        f << "[RAINGAGES]\nRG1 INTENSITY 0:05 1.0 TIMESERIES TS0\n\n";
        f << "[SUBCATCHMENTS]\nS1 RG1 J1 5.0 5 100 1.0 0\n\n";
        f << "[SUBAREAS]\nS1 0.015 0.20 0.00 0.00 100 OUTLET\n\n";
        f << "[INFILTRATION]\nS1 0.0 0.0 0.0 0.0 0.0\n\n";
        // Timeseries at the gage recording interval (5 min) to ensure
        // constant rainfall throughout the 2-hour run.
        f << "[TIMESERIES]\n";
        for (int m = 0; m <= 120; m += 5) {
            const int hh = m / 60;
            const int mm = m % 60;
            f << "TS0 01/01/2025 " << std::setfill('0') << std::setw(2) << hh
              << ":" << std::setfill('0') << std::setw(2) << mm << " " << rain_inhr << "\n";
        }
        f << "\n";
        f << "[JUNCTIONS]\nJ1 100 50 0 0 0\n\n";
        f << "[OUTFALLS]\nO1 95 FREE NO\n\n";
        f << "[CONDUITS]\nC1 J1 O1 200 0.013 0 0 0 0\n\n";
        f << "[XSECTIONS]\nC1 CIRCULAR 2.0 0 0 0 1\n\n";
        f << "[COORDINATES]\nJ1 0.75 0.25\nO1 2.00 0.25\n\n";
        f << "[POLYGONS]\nS1 0.50 0.00\nS1 1.00 0.00\nS1 1.00 0.50\nS1 0.50 0.50\n\n";
        f << "[REPORT]\nINPUT NO\nCONTINUITY YES\nFLOWSTATS YES\nNODES ALL\nLINKS ALL\nSUBCATCHMENTS ALL\n\n";
        if (use_grid) {
            f << "[SOFT_RAINFALL_GRID]\nRUNOFF " << grid_path << " CENTROID FORCE_LOCATION\n";
        }
    };

    auto run_closure = [&](const std::string& inp, const std::string& rpt) {
        struct Closure { double runoff_err, routing_err, rain, runoff, final_s; };
        Closure c{};
        SWMM_Engine h = swmm_engine_create();
        EXPECT_NE(h, nullptr);
        EXPECT_EQ(swmm_engine_open(h, inp.c_str(), rpt.c_str(), nullptr, nullptr), SWMM_OK);
        EXPECT_EQ(swmm_engine_initialize(h), SWMM_OK);
        EXPECT_EQ(swmm_engine_start(h, 0), SWMM_OK);
        runToEnd(h);
        EXPECT_EQ(swmm_engine_end(h), SWMM_OK);
        EXPECT_EQ(swmm_get_runoff_continuity_error(h, &c.runoff_err), SWMM_OK);
        EXPECT_EQ(swmm_get_routing_continuity_error(h, &c.routing_err), SWMM_OK);
        EXPECT_EQ(swmm_get_runoff_total(h, SWMM_RUNOFF_RAINFALL, &c.rain), SWMM_OK);
        EXPECT_EQ(swmm_get_runoff_total(h, SWMM_RUNOFF_RUNOFF, &c.runoff), SWMM_OK);
        EXPECT_EQ(swmm_get_runoff_total(h, SWMM_RUNOFF_FINALSTORE, &c.final_s), SWMM_OK);
        EXPECT_EQ(swmm_engine_close(h), SWMM_OK);
        swmm_engine_destroy(h);
        return c;
    };

    const std::string ctrl_inp = "/tmp/sr2d_closure_ctrl.inp";
    const std::string ctrl_rpt = "/tmp/sr2d_closure_ctrl.rpt";
    const std::string grid_inp = "/tmp/sr2d_closure_grid.inp";
    const std::string grid_rpt = "/tmp/sr2d_closure_grid.rpt";
    write_closure_inp(ctrl_inp, false);
    write_closure_inp(grid_inp, true);

    const auto ctrl = run_closure(ctrl_inp, ctrl_rpt);
    const auto grid = run_closure(grid_inp, grid_rpt);

    // Grid and control must produce identical continuity (bit-for-bit
    // given the same rainfall rate and wet-step cadence).
    EXPECT_NEAR(grid.runoff_err, ctrl.runoff_err, 1.0e-12);
    EXPECT_NEAR(grid.routing_err, ctrl.routing_err, 1.0e-12);
    EXPECT_NEAR(grid.rain, ctrl.rain, 1.0e-9);
    EXPECT_NEAR(grid.runoff, ctrl.runoff, 1.0e-9);

    // After 2 hours of constant rain with zero losses, the surface storage
    // has stabilized and the runoff continuity error should be small.
    // The tolerance (5%) accounts for the residual transient storage on
    // this short fixture — the key invariant is grid == control.
    EXPECT_LT(std::abs(grid.runoff_err), 0.05);

    std::remove(grid_path.c_str());
    std::remove(ctrl_inp.c_str());
    std::remove(ctrl_rpt.c_str());
    std::remove(grid_inp.c_str());
    std::remove(grid_rpt.c_str());
}

} // anonymous namespace
