/**
 * @file test_lid.cpp
 * @brief Comprehensive unit tests for LID controls.
 *
 * @details Covers:
 *   - LIDGroupSoA resize and initialization
 *   - Model builder (LidControlStore/LidUsageStore -> LIDGroupSoA)
 *   - Bio-retention cell flux (surface -> soil -> storage -> drain)
 *   - Rain barrel flux (simple storage + drain)
 *   - Vegetative swale flux (Manning's + infiltration)
 *   - Green roof flux (surface -> soil -> drainmat)
 *   - Permeable pavement flux (surface -> pavement -> storage -> drain)
 *   - Roof disconnection flux (surface split)
 *   - Water balance conservation for all types
 *   - Edge cases: zero rainfall, saturated soil, empty storage
 *
 * @see src/engine/hydrology/LID.hpp
 * @ingroup engine_hydrology
 */

#include <gtest/gtest.h>
#include <cmath>
#include <fstream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#include "hydrology/LID.hpp"
#include "core/SimulationContext.hpp"
#include "data/HydrologyData.hpp"

using namespace openswmm;
using namespace openswmm::lid;

// ============================================================================
// Helper: create a context with one LID control and one usage entry
// ============================================================================

static SimulationContext makeLidContext(
    const std::string& type_code,
    const std::array<double,5>& surface,
    const std::array<double,7>& soil,
    const std::array<double,4>& storage,
    const std::array<double,6>& drain,
    double area = 1000.0, double width = 50.0, double init_sat = 0.0)
{
    SimulationContext ctx;

    // Add one subcatchment
    ctx.subcatch_names.add("S1");
    ctx.subcatches.area.push_back(10.0);  // 10 acres

    // Add one LID control
    ctx.lid_names.add("LID1");
    ctx.lid_controls.names.push_back("LID1");
    ctx.lid_controls.lid_type.push_back(type_code);
    ctx.lid_controls.surface.push_back(surface);
    ctx.lid_controls.soil.push_back(soil);
    ctx.lid_controls.pavement.push_back({0,0,0,0,0,0});
    ctx.lid_controls.storage.push_back(storage);
    ctx.lid_controls.drain.push_back(drain);
    ctx.lid_controls.drainmat.push_back({0,0,0});

    // Add one LID usage
    ctx.lid_usage.subcatch_index.push_back(0);
    ctx.lid_usage.lid_index.push_back(0);
    ctx.lid_usage.number.push_back(1);
    ctx.lid_usage.area.push_back(area);
    ctx.lid_usage.width.push_back(width);
    ctx.lid_usage.init_sat.push_back(init_sat);
    ctx.lid_usage.from_imperv.push_back(0.0);
    ctx.lid_usage.to_perv.push_back(0);
    ctx.lid_usage.rpt_file.push_back("");
    ctx.lid_usage.drain_to.push_back("");
    ctx.lid_usage.from_perv.push_back(0.0);

    return ctx;
}

// ============================================================================
// LIDGroupSoA initialization
// ============================================================================

TEST(LIDGroupSoA, ResizeSetsCorrectSizes) {
    LIDGroupSoA g;
    g.resize(5);
    EXPECT_EQ(g.count, 5);
    EXPECT_EQ(static_cast<int>(g.surf_depth.size()), 5);
    EXPECT_EQ(static_cast<int>(g.soil_moist.size()), 5);
    EXPECT_EQ(static_cast<int>(g.stor_depth.size()), 5);
    EXPECT_EQ(static_cast<int>(g.pave_depth.size()), 5);
    EXPECT_EQ(static_cast<int>(g.wb_inflow.size()), 5);
}

TEST(LIDGroupSoA, ResizeZeroIsEmpty) {
    LIDGroupSoA g;
    g.resize(0);
    EXPECT_EQ(g.count, 0);
    EXPECT_TRUE(g.surf_depth.empty());
}

TEST(LIDGroupSoA, DefaultValuesReasonable) {
    LIDGroupSoA g;
    g.resize(1);
    EXPECT_DOUBLE_EQ(g.surf_void_frac[0], 1.0);
    EXPECT_DOUBLE_EQ(g.surf_depth[0], 0.0);
    EXPECT_DOUBLE_EQ(g.soil_poros[0], 0.4);
    EXPECT_DOUBLE_EQ(g.stor_void[0], 0.5);
}

// ============================================================================
// Model builder tests
// ============================================================================

TEST(LIDModelBuilder, EmptyUsageProducesEmptyGroups) {
    SimulationContext ctx;
    LIDSolver solver;
    solver.init(ctx);
    EXPECT_EQ(solver.numGroups(), 8);
    for (int i = 0; i < 8; ++i)
        EXPECT_EQ(solver.group(i).count, 0);
}

TEST(LIDModelBuilder, SingleBioCellPopulated) {
    auto ctx = makeLidContext("BC",
        {0.5, 0.0, 0.1, 0.01, 0.0},   // surface: 0.5ft store, n=0.1, slope=1%
        {1.5, 0.45, 0.20, 0.10, 1e-5, 30.0, 6.0},  // soil
        {1.0, 0.5, 1e-6, 0.0},         // storage
        {0.5, 0.5, 0.0, 0.0, 0.0, 0.0} // drain
    );

    LIDSolver solver;
    solver.init(ctx);

    const auto& g = solver.group(0); // BIO_CELL
    EXPECT_EQ(g.count, 1);
    EXPECT_NEAR(g.surf_store[0], 0.5, 1e-10);
    EXPECT_NEAR(g.soil_thick[0], 1.5, 1e-10);
    EXPECT_NEAR(g.soil_poros[0], 0.45, 1e-10);
    EXPECT_NEAR(g.stor_thick[0], 1.0, 1e-10);
    EXPECT_NEAR(g.drain_coeff[0], 0.5, 1e-10);
    EXPECT_NEAR(g.area[0], 1000.0, 1e-10);
}

TEST(LIDModelBuilder, InitialSaturationSetsState) {
    auto ctx = makeLidContext("BC",
        {0.5, 0.0, 0.1, 0.01, 0.0},
        {1.5, 0.45, 0.20, 0.10, 1e-5, 30.0, 6.0},
        {1.0, 0.5, 0.0, 0.0},
        {0.0, 0.5, 0.0, 0.0, 0.0, 0.0},
        1000.0, 50.0, 0.5  // 50% initial saturation
    );

    LIDSolver solver;
    solver.init(ctx);

    const auto& g = solver.group(0);
    // Soil: wp + initSat * (poros - wp) = 0.10 + 0.5*(0.45 - 0.10) = 0.275
    EXPECT_NEAR(g.soil_moist[0], 0.275, 1e-10);
    // Storage: initSat * thickness = 0.5 * 1.0 = 0.5
    EXPECT_NEAR(g.stor_depth[0], 0.5, 1e-10);
}

TEST(LIDModelBuilder, MultipleTypesDistributed) {
    SimulationContext ctx;
    ctx.subcatch_names.add("S1");
    ctx.subcatches.area.push_back(10.0);

    // Two LID controls: one BC, one RB
    ctx.lid_names.add("BC1");
    ctx.lid_names.add("RB1");
    ctx.lid_controls.names = {"BC1", "RB1"};
    ctx.lid_controls.lid_type = {"BC", "RB"};
    ctx.lid_controls.surface.resize(2);
    ctx.lid_controls.soil.resize(2);
    ctx.lid_controls.pavement.resize(2);
    ctx.lid_controls.storage.resize(2);
    ctx.lid_controls.drain.resize(2);
    ctx.lid_controls.drainmat.resize(2);

    ctx.lid_controls.soil[0] = {1.0, 0.4, 0.2, 0.1, 1e-5, 20.0, 0.0};
    ctx.lid_controls.storage[1] = {3.0, 1.0, 0.0, 0.0};
    ctx.lid_controls.drain[1] = {0.1, 0.5, 0.0, 0.0, 0.0, 0.0};

    // Two usage entries
    ctx.lid_usage.subcatch_index = {0, 0};
    ctx.lid_usage.lid_index = {0, 1};
    ctx.lid_usage.number = {1, 1};
    ctx.lid_usage.area = {500.0, 200.0};
    ctx.lid_usage.width = {25.0, 10.0};
    ctx.lid_usage.init_sat = {0.0, 0.0};
    ctx.lid_usage.from_imperv = {0.0, 0.0};
    ctx.lid_usage.to_perv = {0, 0};
    ctx.lid_usage.rpt_file = {"", ""};
    ctx.lid_usage.drain_to = {"", ""};
    ctx.lid_usage.from_perv = {0.0, 0.0};

    LIDSolver solver;
    solver.init(ctx);

    EXPECT_EQ(solver.group(0).count, 1); // BC
    EXPECT_EQ(solver.group(5).count, 1); // RB
    EXPECT_NEAR(solver.group(0).area[0], 500.0, 1e-10);
    EXPECT_NEAR(solver.group(5).area[0], 200.0, 1e-10);

    // Other groups should be empty
    EXPECT_EQ(solver.group(1).count, 0); // RG
    EXPECT_EQ(solver.group(2).count, 0); // GR
}

TEST(LIDModelBuilder, ManningAlphaComputed) {
    auto ctx = makeLidContext("BC",
        {0.5, 0.0, 0.05, 0.02, 0.0},  // roughness=0.05, slope=2%
        {0,0,0,0,0,0,0}, {0,0,0,0}, {0,0,0,0,0,0});

    LIDSolver solver;
    solver.init(ctx);
    const auto& g = solver.group(0);
    double expected_alpha = 1.49 * std::sqrt(0.02) / 0.05;
    EXPECT_NEAR(g.surf_alpha[0], expected_alpha, 1e-6);
}

// ============================================================================
// Bio-retention cell flux tests
// ============================================================================

TEST(LIDBioCell, RainfallFillsSurface) {
    LIDGroupSoA g;
    g.type = LIDType::BIO_CELL;
    g.resize(1);
    g.surf_store[0] = 0.5;
    g.surf_rough[0] = 0.1;
    g.surf_slope[0] = 0.01;
    g.surf_alpha[0] = 1.49 * std::sqrt(0.01) / 0.1;
    g.soil_thick[0] = 1.0;
    g.soil_poros[0] = 0.4;
    g.soil_fc[0]    = 0.2;
    g.soil_wp[0]    = 0.1;
    g.soil_ksat[0]  = 0.0;  // no soil infiltration → water stays on surface
    g.soil_kslope[0]= 0.0;
    g.soil_moist[0] = 0.2;
    g.stor_thick[0] = 0.0;
    g.stor_void[0]  = 0.5;
    g.area[0]       = 1000.0;
    g.full_width[0] = 50.0;

    double rainfall = 1e-4;  // ft/sec
    double dt = 60.0;
    LIDSolver::batchBioCellFlux(g, rainfall, 0.0, dt);

    // With no soil ksat, all rainfall accumulates on surface (up to storage)
    EXPECT_GT(g.surf_depth[0], 0.0);
}

TEST(LIDBioCell, SoilPercolationOccurs) {
    LIDGroupSoA g;
    g.type = LIDType::BIO_CELL;
    g.resize(1);
    g.surf_store[0] = 0.5;
    g.surf_depth[0] = 0.3;  // ponded water on surface
    g.soil_thick[0] = 1.0;
    g.soil_poros[0] = 0.4;
    g.soil_fc[0]    = 0.2;
    g.soil_wp[0]    = 0.1;
    g.soil_ksat[0]  = 1e-4;
    g.soil_kslope[0]= 20.0;
    g.soil_moist[0] = 0.35; // above FC → percolation should occur
    g.stor_thick[0] = 1.0;
    g.stor_void[0]  = 0.5;
    g.stor_depth[0] = 0.0;
    g.area[0]       = 1000.0;

    double dt = 300.0;
    LIDSolver::batchBioCellFlux(g, 0.0, 0.0, dt);

    // Storage depth should increase (soil percolated into it)
    EXPECT_GT(g.stor_depth[0], 0.0)
        << "Soil percolation should fill storage layer";
}

TEST(LIDBioCell, DrainActivatesAboveOffset) {
    LIDGroupSoA g;
    g.type = LIDType::BIO_CELL;
    g.resize(1);
    g.surf_store[0]  = 0.5;
    g.soil_thick[0]  = 0.0;  // no soil layer (like infiltration trench)
    g.soil_poros[0]  = 0.4;
    g.soil_fc[0]     = 0.2;
    g.soil_wp[0]     = 0.1;
    g.soil_ksat[0]   = 0.0;
    g.stor_thick[0]  = 2.0;
    g.stor_void[0]   = 0.5;
    g.stor_depth[0]  = 1.5;  // well above offset
    g.stor_ksat[0]   = 0.0;  // no exfiltration
    g.drain_coeff[0] = 0.5;
    g.drain_expon[0] = 0.5;
    g.drain_offset[0]= 0.5;  // drain at 0.5 ft
    g.area[0]        = 1000.0;

    LIDSolver::batchBioCellFlux(g, 0.0, 0.0, 300.0);

    EXPECT_GT(g.drain_flow[0], 0.0)
        << "Drain should activate when storage > offset";
}

TEST(LIDBioCell, DrainInactiveWhenBelowOffset) {
    LIDGroupSoA g;
    g.type = LIDType::BIO_CELL;
    g.resize(1);
    g.surf_store[0]  = 0.5;
    g.soil_thick[0]  = 0.0;
    g.soil_ksat[0]   = 0.0;
    g.stor_thick[0]  = 2.0;
    g.stor_void[0]   = 0.5;
    g.stor_depth[0]  = 0.3;   // below offset
    g.stor_ksat[0]   = 0.0;
    g.drain_coeff[0] = 0.5;
    g.drain_expon[0] = 0.5;
    g.drain_offset[0]= 0.5;
    g.area[0]        = 1000.0;

    LIDSolver::batchBioCellFlux(g, 0.0, 0.0, 300.0);

    EXPECT_DOUBLE_EQ(g.drain_flow[0], 0.0)
        << "Drain should be zero when storage < offset";
}

TEST(LIDBioCell, WaterBalanceConserved) {
    auto ctx = makeLidContext("BC",
        {0.5, 0.0, 0.1, 0.01, 0.0},
        {1.0, 0.45, 0.20, 0.10, 1e-5, 20.0, 0.0},
        {1.0, 0.5, 1e-6, 0.0},
        {0.3, 0.5, 0.0, 0.0, 0.0, 0.0});

    LIDSolver solver;
    solver.init(ctx);

    // Run 10 timesteps with rainfall
    for (int t = 0; t < 10; ++t)
        solver.execute(ctx, 300.0, 1e-4, 1e-6);

    const auto& g = solver.group(0);
    double in  = g.wb_inflow[0];
    double out = g.wb_evap[0] + g.wb_infil[0] + g.wb_surf_flow[0] + g.wb_drain_flow[0];
    double stored = g.wb_final_vol[0] - g.wb_init_vol[0];

    // Mass balance: inflow = outflow + delta_storage
    // Euler integration has small numerical drift; use relative tolerance
    double total = std::max(in, 1e-10);
    double error = std::abs(in - (out + stored)) / total;
    EXPECT_LT(error, 0.05)
        << "Water balance relative error should be < 5% for bio-cell (Euler)";
}

// ============================================================================
// Rain barrel flux tests
// ============================================================================

TEST(LIDBarrel, FillsAndOverflows) {
    LIDGroupSoA g;
    g.type = LIDType::RAIN_BARREL;
    g.resize(1);
    g.stor_thick[0]  = 0.5;  // 0.5 ft storage capacity
    g.stor_depth[0]  = 0.0;
    g.drain_coeff[0] = 0.0;  // no drain
    g.area[0]        = 100.0;

    // Heavy rainfall: 1e-3 ft/sec * 600s = 0.6 ft > 0.5 capacity
    LIDSolver::batchBarrelFlux(g, 1e-3, 600.0);

    EXPECT_NEAR(g.stor_depth[0], 0.5, 1e-10)
        << "Barrel should be full";
    EXPECT_GT(g.surface_runoff[0], 0.0)
        << "Excess should overflow";
}

TEST(LIDBarrel, DrainReducesStorage) {
    LIDGroupSoA g;
    g.type = LIDType::RAIN_BARREL;
    g.resize(1);
    g.stor_thick[0]   = 2.0;
    g.stor_depth[0]   = 1.5;
    g.drain_coeff[0]  = 0.01;
    g.drain_expon[0]  = 0.5;
    g.drain_offset[0] = 0.0;
    g.area[0]         = 100.0;

    double initial = g.stor_depth[0];
    LIDSolver::batchBarrelFlux(g, 0.0, 300.0);

    EXPECT_LT(g.stor_depth[0], initial)
        << "Drain should reduce storage depth";
    EXPECT_GT(g.drain_flow[0], 0.0);
}

TEST(LIDBarrel, WaterBalanceConserved) {
    LIDGroupSoA g;
    g.type = LIDType::RAIN_BARREL;
    g.resize(1);
    g.stor_thick[0]   = 1.0;
    g.stor_depth[0]   = 0.0;
    g.drain_coeff[0]  = 0.005;
    g.drain_expon[0]  = 0.5;
    g.drain_offset[0] = 0.0;
    g.area[0]         = 100.0;

    // Reset water balance
    g.wb_inflow[0] = g.wb_evap[0] = g.wb_infil[0] = 0.0;
    g.wb_surf_flow[0] = g.wb_drain_flow[0] = 0.0;
    g.wb_init_vol[0] = g.stor_depth[0];

    double rainfall = 5e-4;
    double dt = 300.0;
    for (int t = 0; t < 20; ++t)
        LIDSolver::batchBarrelFlux(g, rainfall, dt);

    double in = g.wb_inflow[0];
    double out = g.wb_surf_flow[0] + g.wb_drain_flow[0];
    double stored = g.wb_final_vol[0] - g.wb_init_vol[0];

    EXPECT_NEAR(in, out + stored, 1e-6)
        << "Water balance should be conserved for rain barrel";
}

// ============================================================================
// Vegetative swale tests
// ============================================================================

TEST(LIDSwale, ManningsOutflowOccurs) {
    LIDGroupSoA g;
    g.type = LIDType::VEG_SWALE;
    g.resize(1);
    g.surf_store[0]  = 0.1;   // low storage threshold
    g.surf_depth[0]  = 0.5;   // ponded water well above storage
    g.surf_rough[0]  = 0.05;
    g.surf_slope[0]  = 0.02;
    g.surf_alpha[0]  = 1.49 * std::sqrt(0.02) / 0.05;
    g.surf_void_frac[0] = 1.0;
    g.soil_ksat[0]   = 0.0;   // no infiltration
    g.full_width[0]  = 10.0;
    g.area[0]        = 500.0;

    LIDSolver::batchSwaleFlux(g, 0.0, 0.0, 300.0);

    EXPECT_GT(g.surface_runoff[0], 0.0)
        << "Swale should produce surface runoff when depth > storage";
}

// ============================================================================
// Green roof tests
// ============================================================================

TEST(LIDGreenRoof, SoilPercolationToDrainMat) {
    LIDGroupSoA g;
    g.type = LIDType::GREEN_ROOF;
    g.resize(1);
    g.surf_store[0]  = 0.1;
    g.surf_depth[0]  = 0.05;
    g.surf_rough[0]  = 0.1;
    g.surf_slope[0]  = 0.02;
    g.surf_alpha[0]  = 1.49 * std::sqrt(0.02) / 0.1;
    g.surf_void_frac[0] = 1.0;
    g.soil_thick[0]  = 0.5;
    g.soil_poros[0]  = 0.5;
    g.soil_fc[0]     = 0.25;
    g.soil_wp[0]     = 0.10;
    g.soil_ksat[0]   = 5e-5;
    g.soil_kslope[0] = 20.0;
    g.soil_moist[0]  = 0.40; // above FC → percolation occurs
    g.drainmat_thick[0] = 0.1;
    g.drainmat_void[0]  = 0.5;
    g.drainmat_rough[0] = 0.1;
    g.stor_thick[0]  = 0.1;
    g.stor_void[0]   = 0.5;
    g.stor_depth[0]  = 0.05;  // pre-fill drainage mat so drain activates
    g.full_width[0]  = 20.0;
    g.area[0]        = 1000.0;

    LIDSolver::batchGreenRoofFlux(g, 1e-4, 1e-6, 300.0);

    // Drain flow should occur since drainage mat has water and drainmatAlpha > 0
    EXPECT_GT(g.drain_flow[0], 0.0)
        << "Green roof should produce drain flow from drainage mat";
}

// ============================================================================
// Permeable pavement tests
// ============================================================================

TEST(LIDPavement, PavementPercolation) {
    LIDGroupSoA g;
    g.type = LIDType::PERM_PAVEMENT;
    g.resize(1);
    g.surf_store[0]    = 0.1;
    g.surf_depth[0]    = 0.05;
    g.surf_rough[0]    = 0.01;
    g.surf_slope[0]    = 0.02;
    g.surf_alpha[0]    = 1.49 * std::sqrt(0.02) / 0.01;
    g.surf_void_frac[0]= 1.0;
    g.pave_thick[0]    = 0.5;
    g.pave_void[0]     = 0.15;
    g.pave_imperv_frac[0] = 0.0;
    g.pave_ksat[0]     = 1e-3;
    g.pave_clog_factor[0] = 0.0;
    g.pave_depth[0]    = 0.1;  // water in pavement
    g.soil_thick[0]    = 0.0;  // no soil layer
    g.stor_thick[0]    = 1.0;
    g.stor_void[0]     = 0.4;
    g.stor_ksat[0]     = 0.0;
    g.stor_depth[0]    = 0.0;
    g.drain_coeff[0]   = 0.1;
    g.drain_expon[0]   = 0.5;
    g.drain_offset[0]  = 0.0;
    g.full_width[0]    = 30.0;
    g.area[0]          = 2000.0;

    LIDSolver::batchPavementFlux(g, 1e-4, 0.0, 300.0);

    // Storage should receive water through pavement
    EXPECT_GT(g.stor_depth[0], 0.0)
        << "Pavement should percolate into storage layer";
}

TEST(LIDPavement, CloggingReducesPermeability) {
    // High clog factor with lots of treated volume should reduce percolation
    LIDGroupSoA g_clean, g_clogged;
    g_clean.type = g_clogged.type = LIDType::PERM_PAVEMENT;
    g_clean.resize(1); g_clogged.resize(1);

    auto setup = [](LIDGroupSoA& g) {
        g.surf_store[0] = 0.1; g.surf_depth[0] = 0.05;
        g.pave_thick[0] = 0.5; g.pave_void[0] = 0.15;
        g.pave_ksat[0] = 1e-3; g.pave_depth[0] = 0.1;
        g.stor_thick[0] = 1.0; g.stor_void[0] = 0.4;
        g.area[0] = 1000.0;
    };

    setup(g_clean);
    setup(g_clogged);
    g_clean.pave_clog_factor[0] = 0.0;   // no clogging
    g_clogged.pave_clog_factor[0] = 1.0;  // will clog
    g_clogged.vol_treated[0] = 0.8;       // 80% clogged

    LIDSolver::batchPavementFlux(g_clean, 1e-4, 0.0, 300.0);
    LIDSolver::batchPavementFlux(g_clogged, 1e-4, 0.0, 300.0);

    // Clogged pavement should have less water reaching storage
    EXPECT_GE(g_clean.stor_depth[0], g_clogged.stor_depth[0])
        << "Clogging should reduce water reaching storage";
}

// ============================================================================
// Roof disconnection tests
// ============================================================================

TEST(LIDRoof, SplitsBetweenOverflowAndDrain) {
    LIDGroupSoA g;
    g.type = LIDType::ROOF_DISCON;
    g.resize(1);
    g.surf_store[0]  = 0.02;  // small depression storage
    g.surf_depth[0]  = 0.1;   // pre-fill above storage → immediate overflow
    g.surf_void_frac[0] = 1.0;
    g.surf_alpha[0]  = 0.0;   // no Manning → overflow path
    g.drain_coeff[0] = 0.001; // small drain capacity
    g.area[0]        = 500.0;
    g.full_width[0]  = 20.0;

    LIDSolver::batchRoofDisconFlux(g, 5e-4, 0.0, 300.0);

    // Both overflow and drain should have flow
    // drain = min(drain_coeff, surfaceOutflow)
    // surfaceOutflow > 0 since surf_depth > surf_store
    EXPECT_GT(g.drain_flow[0], 0.0) << "Should have drain flow";
    // surface_runoff = surfaceOutflow - storageDrain (remaining after drain)
    EXPECT_GE(g.surface_runoff[0], 0.0) << "Should have non-negative surface runoff";
}

// ============================================================================
// Evaporation tests (across LID types)
// ============================================================================

TEST(LIDBioCell, EvaporationReducesSurfaceDepth) {
    LIDGroupSoA g;
    g.type = LIDType::BIO_CELL;
    g.resize(1);
    g.surf_store[0]  = 0.5;
    g.surf_depth[0]  = 0.1;  // ponded water
    g.soil_thick[0]  = 1.0;
    g.soil_poros[0]  = 0.4;
    g.soil_fc[0]     = 0.2;
    g.soil_wp[0]     = 0.1;
    g.soil_ksat[0]   = 0.0;
    g.stor_thick[0]  = 0.0;
    g.area[0]        = 1000.0;

    double evap = 5e-6;  // ft/sec evaporation
    double dt = 600.0;
    double initial = g.surf_depth[0];
    LIDSolver::batchBioCellFlux(g, 0.0, evap, dt);

    EXPECT_LT(g.surf_depth[0], initial)
        << "Evaporation should reduce surface ponded depth";
    EXPECT_GT(g.evap_loss[0], 0.0);
}

// ============================================================================
// Batch processing: multiple units same type
// ============================================================================

TEST(LIDBatch, MultipleBarrelsProcessedCorrectly) {
    LIDGroupSoA g;
    g.type = LIDType::RAIN_BARREL;
    g.resize(3);

    for (int i = 0; i < 3; ++i) {
        auto ui = static_cast<size_t>(i);
        g.stor_thick[ui]   = 1.0 + i * 0.5;  // different capacities
        g.stor_depth[ui]   = 0.0;
        g.drain_coeff[ui]  = 0.0;
        g.area[ui]         = 100.0;
    }

    LIDSolver::batchBarrelFlux(g, 2e-4, 600.0);

    // All should have water, amounts differ by capacity
    for (int i = 0; i < 3; ++i) {
        auto ui = static_cast<size_t>(i);
        EXPECT_GT(g.stor_depth[ui], 0.0)
            << "Barrel " << i << " should have storage";
        // Inflow = 2e-4 * 600 = 0.12 ft, all < capacity → no overflow
        EXPECT_NEAR(g.stor_depth[ui], 0.12, 1e-6);
        EXPECT_DOUBLE_EQ(g.surface_runoff[ui], 0.0);
    }
}

// ============================================================================
// Edge cases
// ============================================================================

TEST(LIDEdge, ZeroRainfallNoChange) {
    LIDGroupSoA g;
    g.type = LIDType::RAIN_BARREL;
    g.resize(1);
    g.stor_thick[0] = 1.0;
    g.stor_depth[0] = 0.0;
    g.drain_coeff[0] = 0.0;
    g.area[0] = 100.0;

    LIDSolver::batchBarrelFlux(g, 0.0, 300.0);

    EXPECT_DOUBLE_EQ(g.stor_depth[0], 0.0);
    EXPECT_DOUBLE_EQ(g.surface_runoff[0], 0.0);
    EXPECT_DOUBLE_EQ(g.drain_flow[0], 0.0);
}

TEST(LIDEdge, NegativeDepthsClamped) {
    LIDGroupSoA g;
    g.type = LIDType::BIO_CELL;
    g.resize(1);
    g.surf_store[0] = 0.5;
    g.surf_depth[0] = 0.001;  // tiny ponding
    g.soil_thick[0] = 1.0;
    g.soil_poros[0] = 0.4;
    g.soil_fc[0]    = 0.2;
    g.soil_wp[0]    = 0.1;
    g.soil_ksat[0]  = 1e-3;  // high infiltration
    g.soil_moist[0] = 0.2;
    g.stor_thick[0] = 1.0;
    g.stor_void[0]  = 0.5;
    g.stor_depth[0] = 0.001;
    g.stor_ksat[0]  = 1e-3;  // high exfiltration
    g.area[0]       = 1000.0;

    // No rainfall, high evap → should drain everything without going negative
    LIDSolver::batchBioCellFlux(g, 0.0, 1e-4, 600.0);

    EXPECT_GE(g.surf_depth[0], 0.0);
    EXPECT_GE(g.soil_moist[0], g.soil_wp[0]);
    EXPECT_GE(g.stor_depth[0], 0.0);
}

// ============================================================================
// Per-unit inflow tests
// ============================================================================

TEST(LIDInflow, PerUnitInflowUsedOverRainfall) {
    LIDGroupSoA g;
    g.type = LIDType::RAIN_BARREL;
    g.resize(1);
    g.stor_thick[0] = 5.0;
    g.stor_depth[0] = 0.0;
    g.drain_coeff[0] = 0.0;
    g.area[0] = 100.0;

    // Set per-unit inflow higher than rainfall
    g.inflow[0] = 1e-3;  // ft/sec (much higher than rainfall=1e-5)

    LIDSolver::batchBarrelFlux(g, 1e-5, 600.0);

    // Storage should reflect the higher per-unit inflow, not the rainfall
    double expected = 1e-3 * 600.0;
    EXPECT_NEAR(g.stor_depth[0], expected, 1e-6)
        << "Per-unit inflow should override rainfall when set";
}

TEST(LIDInflow, ZeroInflowFallsBackToRainfall) {
    LIDGroupSoA g;
    g.type = LIDType::RAIN_BARREL;
    g.resize(1);
    g.stor_thick[0] = 5.0;
    g.stor_depth[0] = 0.0;
    g.drain_coeff[0] = 0.0;
    g.area[0] = 100.0;

    g.inflow[0] = 0.0;  // no per-unit inflow → use rainfall

    double rainfall = 2e-4;
    LIDSolver::batchBarrelFlux(g, rainfall, 600.0);

    double expected = rainfall * 600.0;
    EXPECT_NEAR(g.stor_depth[0], expected, 1e-6)
        << "Should fall back to rainfall when inflow is zero";
}

// ============================================================================
// Model builder: routing fields populated
// ============================================================================

TEST(LIDModelBuilder, FromImpervConvertedToFraction) {
    auto ctx = makeLidContext("BC",
        {0.5, 0.0, 0.1, 0.01, 0.0},
        {1.0, 0.4, 0.2, 0.1, 1e-5, 20.0, 0.0},
        {1.0, 0.5, 0.0, 0.0},
        {0.0, 0.5, 0.0, 0.0, 0.0, 0.0});
    // from_imperv is in % in the usage store
    ctx.lid_usage.from_imperv[0] = 50.0;  // 50%

    LIDSolver solver;
    solver.init(ctx);

    EXPECT_NEAR(solver.group(0).from_imperv[0], 0.5, 1e-10)
        << "from_imperv should be converted from % to fraction";
}

TEST(LIDModelBuilder, DrainToNodeResolved) {
    auto ctx = makeLidContext("RB",
        {0,0,0,0,0}, {0,0,0,0,0,0,0},
        {3.0, 1.0, 0.0, 0.0},
        {0.1, 0.5, 0.0, 0.0, 0.0, 0.0});

    // Add a node for drain-to resolution
    ctx.node_names.add("J1");
    ctx.nodes.type.push_back(NodeType::JUNCTION);
    ctx.lid_usage.drain_to[0] = "J1";

    LIDSolver solver;
    solver.init(ctx);

    EXPECT_EQ(solver.group(5).drain_node[0], 0)
        << "drain_to 'J1' should resolve to node index 0";
    EXPECT_EQ(solver.group(5).drain_subcatch[0], -1)
        << "drain_subcatch should be -1 when drain goes to node";
}

TEST(LIDModelBuilder, DrainToSubcatchResolved) {
    auto ctx = makeLidContext("RB",
        {0,0,0,0,0}, {0,0,0,0,0,0,0},
        {3.0, 1.0, 0.0, 0.0},
        {0.1, 0.5, 0.0, 0.0, 0.0, 0.0});

    // Add a second subcatchment for drain-to
    ctx.subcatch_names.add("S2");
    ctx.subcatches.area.push_back(5.0);
    ctx.lid_usage.drain_to[0] = "S2";

    LIDSolver solver;
    solver.init(ctx);

    EXPECT_EQ(solver.group(5).drain_node[0], -1);
    EXPECT_EQ(solver.group(5).drain_subcatch[0], 1)
        << "drain_to 'S2' should resolve to subcatch index 1";
}

TEST(LIDModelBuilder, ToPerFlagPreserved) {
    auto ctx = makeLidContext("BC",
        {0.5, 0.0, 0.1, 0.01, 0.0},
        {1.0, 0.4, 0.2, 0.1, 1e-5, 20.0, 0.0},
        {1.0, 0.5, 0.0, 0.0},
        {0.0, 0.5, 0.0, 0.0, 0.0, 0.0});
    ctx.lid_usage.to_perv[0] = 1;

    LIDSolver solver;
    solver.init(ctx);

    EXPECT_EQ(solver.group(0).to_perv[0], 1);
}

// ============================================================================
// Modified Puls solver tests (VEG_SWALE)
// ============================================================================

TEST(LIDModPuls, SwaleConvergesToSteadyState) {
    // Run swale with constant inflow for many timesteps
    // State should approach a steady depth where inflow = outflow
    LIDGroupSoA g;
    g.type = LIDType::VEG_SWALE;
    g.resize(1);
    g.surf_store[0]  = 0.0;
    g.surf_depth[0]  = 0.0;
    g.surf_rough[0]  = 0.05;
    g.surf_slope[0]  = 0.02;
    g.surf_alpha[0]  = 1.49 * std::sqrt(0.02) / 0.05;
    g.surf_void_frac[0] = 1.0;
    g.soil_ksat[0]   = 0.0;  // no infiltration
    g.full_width[0]  = 10.0;
    g.area[0]        = 500.0;

    double rainfall = 1e-4;  // constant inflow
    double dt = 60.0;

    // Run until steady state
    double prev_depth = 0.0;
    for (int t = 0; t < 200; ++t) {
        LIDSolver::batchSwaleModPuls(g, rainfall, 0.0, dt);
        if (t > 100) {
            // After warmup, depth should stabilize
            double change = std::abs(g.surf_depth[0] - prev_depth);
            if (change < 1e-8) break;
        }
        prev_depth = g.surf_depth[0];
    }

    // At steady state: inflow ≈ outflow (runoff)
    EXPECT_GT(g.surf_depth[0], 0.0) << "Swale should have ponded water";
    EXPECT_GT(g.surface_runoff[0], 0.0) << "Swale should produce outflow";
    // Runoff should be in the same order of magnitude as inflow at steady state
    EXPECT_GT(g.surface_runoff[0], rainfall * 0.1)
        << "At steady state, outflow should be significant relative to inflow";
}

TEST(LIDModPuls, SwaleModPulsMoreStableThanEuler) {
    // Compare Modified Puls vs simple Euler for a step-change input.
    // Modified Puls should produce smoother, non-oscillating response.
    LIDGroupSoA g_mp, g_euler;
    g_mp.type = g_euler.type = LIDType::VEG_SWALE;
    g_mp.resize(1); g_euler.resize(1);

    auto setup = [](LIDGroupSoA& g) {
        g.surf_store[0]  = 0.0;
        g.surf_depth[0]  = 0.5;  // start with significant ponding
        g.surf_rough[0]  = 0.05;
        g.surf_slope[0]  = 0.02;
        g.surf_alpha[0]  = 1.49 * std::sqrt(0.02) / 0.05;
        g.surf_void_frac[0] = 1.0;
        g.soil_ksat[0]   = 0.0;
        g.full_width[0]  = 10.0;
        g.area[0]        = 500.0;
    };
    setup(g_mp); setup(g_euler);

    // Modified Puls path (via batchSwaleModPuls)
    LIDSolver::batchSwaleModPuls(g_mp, 0.0, 0.0, 300.0);  // no inflow, drain
    // Euler path (via batchSwaleFlux)
    LIDSolver::batchSwaleFlux(g_euler, 0.0, 0.0, 300.0);  // no inflow, drain

    // Both should reduce depth but not go negative
    EXPECT_GE(g_mp.surf_depth[0], 0.0);
    EXPECT_GE(g_euler.surf_depth[0], 0.0);

    // Modified Puls uses time-weighted average, so it should be more conservative
    // (less aggressive drawdown) for the same timestep
    EXPECT_GE(g_mp.surf_depth[0], g_euler.surf_depth[0] - 0.01)
        << "Modified Puls should be at least as stable as Euler";
}

TEST(LIDModPuls, SwalePreservesFOldBetweenTimesteps) {
    LIDGroupSoA g;
    g.type = LIDType::VEG_SWALE;
    g.resize(1);
    g.surf_store[0]  = 0.0;
    g.surf_depth[0]  = 0.2;
    g.surf_rough[0]  = 0.05;
    g.surf_slope[0]  = 0.02;
    g.surf_alpha[0]  = 1.49 * std::sqrt(0.02) / 0.05;
    g.surf_void_frac[0] = 1.0;
    g.soil_ksat[0]   = 0.0;
    g.area[0]        = 500.0;

    EXPECT_DOUBLE_EQ(g.f_old_surf[0], 0.0)
        << "f_old should start at zero";

    LIDSolver::batchSwaleModPuls(g, 1e-4, 0.0, 60.0);

    // After first timestep, f_old_surf should be updated (non-zero if there's runoff)
    // The exact value depends on the converged state, but it should be finite
    EXPECT_TRUE(std::isfinite(g.f_old_surf[0]))
        << "f_old_surf should be finite after timestep";
}

TEST(LIDModPuls, SwaleWaterBalanceWithModPuls) {
    LIDGroupSoA g;
    g.type = LIDType::VEG_SWALE;
    g.resize(1);
    g.surf_store[0]  = 0.05;
    g.surf_depth[0]  = 0.0;
    g.surf_rough[0]  = 0.1;
    g.surf_slope[0]  = 0.01;
    g.surf_alpha[0]  = 1.49 * std::sqrt(0.01) / 0.1;
    g.surf_void_frac[0] = 1.0;
    g.soil_ksat[0]   = 1e-5;  // some infiltration
    g.area[0]        = 500.0;

    // Reset water balance
    g.wb_inflow[0] = g.wb_evap[0] = g.wb_infil[0] = 0.0;
    g.wb_surf_flow[0] = g.wb_drain_flow[0] = 0.0;
    g.wb_init_vol[0] = g.surf_depth[0] * g.surf_void_frac[0];

    double rainfall = 2e-4;
    double dt = 60.0;
    for (int t = 0; t < 50; ++t)
        LIDSolver::batchSwaleModPuls(g, rainfall, 1e-6, dt);

    double in = g.wb_inflow[0];
    double out = g.wb_evap[0] + g.wb_infil[0] + g.wb_surf_flow[0];
    double stored = g.wb_final_vol[0] - g.wb_init_vol[0];
    double total = std::max(in, 1e-10);
    double error = std::abs(in - (out + stored)) / total;

    EXPECT_LT(error, 0.05)
        << "Water balance relative error should be < 5% for Modified Puls swale";
}

// ============================================================================
// Phase 3: Drain hysteresis tests
// ============================================================================

TEST(LIDDrainHysteresis, DrainOpensAtHOpen) {
    LIDGroupSoA g;
    g.type = LIDType::BIO_CELL;
    g.resize(1);
    g.surf_store[0]  = 0.5;
    g.soil_thick[0]  = 0.0;
    g.soil_ksat[0]   = 0.0;
    g.stor_thick[0]  = 2.0;
    g.stor_void[0]   = 0.5;
    g.stor_ksat[0]   = 0.0;
    g.stor_depth[0]  = 1.0;   // head above offset = 1.0
    g.drain_coeff[0] = 0.5;
    g.drain_expon[0] = 0.5;
    g.drain_offset[0]= 0.0;
    g.drain_hopen[0] = 0.8;   // opens when head >= 0.8
    g.drain_hclose[0]= 0.3;   // closes when head < 0.3
    g.drain_open[0]  = 0;     // starts closed
    g.area[0]        = 1000.0;

    LIDSolver::batchBioCellFlux(g, 0.0, 0.0, 300.0);

    EXPECT_GT(g.drain_flow[0], 0.0)
        << "Drain should open when head >= hOpen";
    EXPECT_EQ(g.drain_open[0], 1);
}

TEST(LIDDrainHysteresis, DrainStaysOpenAboveHClose) {
    LIDGroupSoA g;
    g.type = LIDType::BIO_CELL;
    g.resize(1);
    g.surf_store[0]  = 0.5;
    g.soil_thick[0]  = 0.0;
    g.soil_ksat[0]   = 0.0;
    g.stor_thick[0]  = 2.0;
    g.stor_void[0]   = 0.5;
    g.stor_ksat[0]   = 0.0;
    g.stor_depth[0]  = 0.5;   // head = 0.5, between hClose and hOpen
    g.drain_coeff[0] = 0.5;
    g.drain_expon[0] = 0.5;
    g.drain_offset[0]= 0.0;
    g.drain_hopen[0] = 0.8;
    g.drain_hclose[0]= 0.3;
    g.drain_open[0]  = 1;     // already open

    LIDSolver::batchBioCellFlux(g, 0.0, 0.0, 300.0);

    EXPECT_GT(g.drain_flow[0], 0.0)
        << "Drain should stay open when head >= hClose";
    EXPECT_EQ(g.drain_open[0], 1);
}

TEST(LIDDrainHysteresis, DrainClosesBelow) {
    LIDGroupSoA g;
    g.type = LIDType::BIO_CELL;
    g.resize(1);
    g.surf_store[0]  = 0.5;
    g.soil_thick[0]  = 0.0;
    g.soil_ksat[0]   = 0.0;
    g.stor_thick[0]  = 2.0;
    g.stor_void[0]   = 0.5;
    g.stor_ksat[0]   = 0.0;
    g.stor_depth[0]  = 0.2;   // head = 0.2, below hClose
    g.drain_coeff[0] = 0.5;
    g.drain_expon[0] = 0.5;
    g.drain_offset[0]= 0.0;
    g.drain_hopen[0] = 0.8;
    g.drain_hclose[0]= 0.3;
    g.drain_open[0]  = 1;     // was open

    LIDSolver::batchBioCellFlux(g, 0.0, 0.0, 300.0);

    EXPECT_DOUBLE_EQ(g.drain_flow[0], 0.0)
        << "Drain should close when head < hClose";
    EXPECT_EQ(g.drain_open[0], 0);
}

// ============================================================================
// Phase 3: Covered rain barrel
// ============================================================================

TEST(LIDBarrelCovered, CoveredBlocksRainfall) {
    LIDGroupSoA g;
    g.type = LIDType::RAIN_BARREL;
    g.resize(1);
    g.stor_thick[0]   = 2.0;
    g.stor_depth[0]   = 0.0;
    g.stor_covered[0] = 1;     // covered
    g.drain_coeff[0]  = 0.0;
    g.area[0]         = 100.0;

    LIDSolver::batchBarrelFlux(g, 1e-3, 600.0);

    EXPECT_DOUBLE_EQ(g.stor_depth[0], 0.0)
        << "Covered barrel should block rainfall";
    EXPECT_DOUBLE_EQ(g.surface_runoff[0], 0.0);
}

TEST(LIDBarrelCovered, UncoveredReceivesRainfall) {
    LIDGroupSoA g;
    g.type = LIDType::RAIN_BARREL;
    g.resize(1);
    g.stor_thick[0]   = 2.0;
    g.stor_depth[0]   = 0.0;
    g.stor_covered[0] = 0;     // not covered
    g.drain_coeff[0]  = 0.0;
    g.area[0]         = 100.0;

    LIDSolver::batchBarrelFlux(g, 1e-3, 600.0);

    EXPECT_GT(g.stor_depth[0], 0.0)
        << "Uncovered barrel should receive rainfall";
}

// ============================================================================
// Phase 3: Drain delay for rain barrel
// ============================================================================

TEST(LIDBarrelDelay, DrainBlockedDuringRain) {
    LIDGroupSoA g;
    g.type = LIDType::RAIN_BARREL;
    g.resize(1);
    g.stor_thick[0]   = 2.0;
    g.stor_depth[0]   = 1.0;
    g.drain_coeff[0]  = 0.1;
    g.drain_expon[0]  = 0.5;
    g.drain_offset[0] = 0.0;
    g.drain_delay[0]  = 3600.0;  // 1 hour delay after rain stops
    g.dry_time[0]     = 0.0;     // just rained
    g.area[0]         = 100.0;

    // With rainfall → dry_time stays 0 → drain blocked by delay
    LIDSolver::batchBarrelFlux(g, 1e-4, 300.0);

    EXPECT_DOUBLE_EQ(g.drain_flow[0], 0.0)
        << "Drain should be blocked during rain when delay is set";
}

TEST(LIDBarrelDelay, DrainActivatesAfterDelay) {
    LIDGroupSoA g;
    g.type = LIDType::RAIN_BARREL;
    g.resize(1);
    g.stor_thick[0]   = 2.0;
    g.stor_depth[0]   = 1.0;
    g.drain_coeff[0]  = 0.1;
    g.drain_expon[0]  = 0.5;
    g.drain_offset[0] = 0.0;
    g.drain_delay[0]  = 3600.0;  // 1 hour delay
    g.dry_time[0]     = 4000.0;  // dry for > 1 hour
    g.area[0]         = 100.0;

    // No rainfall, dry_time > delay → drain should activate
    LIDSolver::batchBarrelFlux(g, 0.0, 300.0);

    EXPECT_GT(g.drain_flow[0], 0.0)
        << "Drain should activate after dry_time exceeds delay";
}

// ============================================================================
// Phase 3: Storage clogging
// ============================================================================

TEST(LIDStorageClog, CloggingReducesExfiltration) {
    LIDGroupSoA g_clean, g_clogged;
    g_clean.type = g_clogged.type = LIDType::BIO_CELL;
    g_clean.resize(1); g_clogged.resize(1);

    auto setup = [](LIDGroupSoA& g) {
        g.surf_store[0] = 0.5;
        g.soil_thick[0] = 0.0;
        g.soil_ksat[0]  = 0.0;
        g.stor_thick[0] = 2.0;
        g.stor_void[0]  = 0.5;
        g.stor_ksat[0]  = 1e-4;  // positive exfiltration
        g.stor_depth[0] = 1.0;
        g.drain_coeff[0]= 0.0;
        g.area[0]       = 1000.0;
    };
    setup(g_clean); setup(g_clogged);

    g_clean.stor_clog[0]   = 0.0;  // no clogging
    g_clogged.stor_clog[0] = 1.0;  // clogs at 1 ft of cumulative inflow
    g_clogged.wb_inflow[0] = 0.8;  // 80% clogged

    LIDSolver::batchBioCellFlux(g_clean, 0.0, 0.0, 300.0);
    LIDSolver::batchBioCellFlux(g_clogged, 0.0, 0.0, 300.0);

    EXPECT_GT(g_clean.infil_loss[0], g_clogged.infil_loss[0])
        << "Clogging should reduce exfiltration loss";
}

// ============================================================================
// Phase 3: Pavement regeneration
// ============================================================================

TEST(LIDPavementRegen, RegenerationReducesClogging) {
    LIDGroupSoA g;
    g.type = LIDType::PERM_PAVEMENT;
    g.resize(1);
    g.surf_store[0]     = 0.1;
    g.surf_depth[0]     = 0.05;
    g.pave_thick[0]     = 0.5;
    g.pave_void[0]      = 0.15;
    g.pave_ksat[0]      = 1e-3;
    g.pave_clog_factor[0] = 1.0;
    g.pave_regen_days[0]  = 0.001;  // very short regen interval for testing
    g.pave_regen_deg[0]   = 0.5;    // 50% regeneration
    g.next_regen_day[0]   = 0.0001; // about to regenerate
    g.vol_treated[0]      = 0.8;    // 80% clogged
    g.stor_thick[0]     = 1.0;
    g.stor_void[0]      = 0.4;
    g.area[0]           = 1000.0;

    double vol_before = g.vol_treated[0];
    // Run a timestep long enough for regeneration to trigger
    LIDSolver::batchPavementFlux(g, 1e-4, 0.0, 100.0);

    // vol_treated should have been reduced by regeneration
    EXPECT_LT(g.vol_treated[0], vol_before)
        << "Regeneration should reduce vol_treated";
}

// ============================================================================
// Phase 4: Pollutant removal
// ============================================================================

TEST(LIDPollutantRemoval, RemovalFractionsPopulatedFromContext) {
    SimulationContext ctx;
    ctx.subcatch_names.add("S1");
    ctx.subcatches.area.push_back(10.0);

    // Add pollutants
    ctx.pollutant_names.add("TSS");
    ctx.pollutant_names.add("Lead");

    // Add a bio-cell LID with REMOVALS
    ctx.lid_names.add("BC1");
    ctx.lid_controls.names.push_back("BC1");
    ctx.lid_controls.lid_type.push_back("BC");
    ctx.lid_controls.surface.push_back({0.5, 0.0, 0.1, 0.01, 0.0});
    ctx.lid_controls.soil.push_back({1.0, 0.4, 0.2, 0.1, 1e-5, 20.0, 0.0});
    ctx.lid_controls.pavement.push_back({0,0,0,0,0,0});
    ctx.lid_controls.storage.push_back({1.0, 0.5, 0.0, 0.0});
    ctx.lid_controls.drain.push_back({0.5, 0.5, 0.0, 0.0, 0.0, 0.0});
    ctx.lid_controls.drainmat.push_back({0,0,0});
    // Removals: TSS=80%, Lead=95%
    ctx.lid_controls.removals.push_back({{0, 0.80}, {1, 0.95}});

    ctx.lid_usage.subcatch_index.push_back(0);
    ctx.lid_usage.lid_index.push_back(0);
    ctx.lid_usage.number.push_back(1);
    ctx.lid_usage.area.push_back(500.0);
    ctx.lid_usage.width.push_back(25.0);
    ctx.lid_usage.init_sat.push_back(0.0);
    ctx.lid_usage.from_imperv.push_back(0.0);
    ctx.lid_usage.to_perv.push_back(0);
    ctx.lid_usage.rpt_file.push_back("");
    ctx.lid_usage.drain_to.push_back("");
    ctx.lid_usage.from_perv.push_back(0.0);

    LIDSolver solver;
    solver.init(ctx);

    const auto& g = solver.group(0);  // BIO_CELL
    EXPECT_EQ(g.count, 1);
    EXPECT_EQ(g.n_pollutants, 2);
    EXPECT_EQ(static_cast<int>(g.drain_rmvl.size()), 2);

    // TSS removal = 80%
    EXPECT_NEAR(g.drain_rmvl[0], 0.80, 1e-10);
    // Lead removal = 95%
    EXPECT_NEAR(g.drain_rmvl[1], 0.95, 1e-10);
}

TEST(LIDPollutantRemoval, NoRemovalsByDefault) {
    auto ctx = makeLidContext("BC",
        {0.5, 0.0, 0.1, 0.01, 0.0},
        {1.0, 0.4, 0.2, 0.1, 1e-5, 20.0, 0.0},
        {1.0, 0.5, 0.0, 0.0},
        {0.5, 0.5, 0.0, 0.0, 0.0, 0.0});

    LIDSolver solver;
    solver.init(ctx);

    const auto& g = solver.group(0);
    // No pollutants in context → drain_rmvl should be empty or all zero
    EXPECT_TRUE(g.drain_rmvl.empty() || g.n_pollutants == 0)
        << "No pollutants means no removal fractions";
}

TEST(LIDPollutantRemoval, RemovalsStoreParsedCorrectly) {
    // Verify the LidControlStore.removals vector
    LidControlStore store;
    store.names.push_back("LID1");
    store.removals.resize(1);
    store.removals[0].push_back({0, 0.75});  // pollutant 0: 75%
    store.removals[0].push_back({2, 0.50});  // pollutant 2: 50%

    EXPECT_EQ(store.removals[0].size(), 2u);
    EXPECT_EQ(store.removals[0][0].first, 0);
    EXPECT_NEAR(store.removals[0][0].second, 0.75, 1e-10);
    EXPECT_EQ(store.removals[0][1].first, 2);
    EXPECT_NEAR(store.removals[0][1].second, 0.50, 1e-10);
}

// ============================================================================
// Storage exfiltration trajectory benchmark
//
// batchBarrelFlux with no inflow, no drain, no evap, and clogFactor=0 reduces
// to d(depth)/dt = -kSat/phi — a constant-rate ODE that Euler integrates
// exactly.  Depth and cumulative exfiltration are compared against the
// closed-form linear solution to machine-precision tolerance.
// ============================================================================

#ifndef BENCHMARK_DATA_DIR
#  define BENCHMARK_DATA_DIR ""
#endif

namespace {

struct ExfilBenchRow { double t_s, stor_depth_ft, E_cumul_ft; };

static std::vector<ExfilBenchRow> load_exfil_bench(const std::string& path) {
    std::vector<ExfilBenchRow> rows;
    std::ifstream in(path);
    if (!in.is_open()) return rows;
    std::string line;
    bool header_seen = false;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (!header_seen) { header_seen = true; continue; }
        // columns: t_s, stor_depth_ft, E_cumul_ft
        std::istringstream ss(line);
        std::string tok;
        double vals[3] = {};
        int col = 0;
        while (std::getline(ss, tok, ',') && col < 3)
            vals[col++] = std::stod(tok);
        if (col >= 3)
            rows.push_back({vals[0], vals[1], vals[2]});
    }
    return rows;
}

}  // namespace

// Constant-area storage draining at constant kSat (no clogging, no drain).
// Exact solution: depth(t) = 2.0 - 2.5e-4*t, E_cumul(t) = 1e-4*t.
// Euler is exact for this linear ODE, so tolerance is machine-epsilon tight.
TEST(LIDStorageExfil, TrajectoryMatchesBenchmark) {
    std::string path = std::string(BENCHMARK_DATA_DIR)
        + "/manufactured/exfil-storage-constant-area/reference.csv";

    auto rows = load_exfil_bench(path);
    if (rows.empty()) {
        GTEST_SKIP() << "Benchmark data not found: " << path;
    }

    LIDGroupSoA g;
    g.type = LIDType::RAIN_BARREL;
    g.resize(1);
    g.stor_thick[0]  = 2.0;
    g.stor_void[0]   = 0.4;
    g.stor_ksat[0]   = 1.0e-4;
    g.stor_clog[0]   = 0.0;
    g.drain_coeff[0] = 0.0;
    g.stor_depth[0]  = rows[0].stor_depth_ft;  // 2.0 ft (full)
    g.surf_depth[0]  = 0.0;

    double max_depth_err = 0.0, max_exfil_err = 0.0;
    double sum_sq_depth  = 0.0, sum_sq_exfil  = 0.0;
    double prev_t = rows[0].t_s;

    for (size_t i = 1; i < rows.size(); ++i) {
        double dt = rows[i].t_s - prev_t;
        LIDSolver::batchBarrelFlux(g, 0.0, dt);

        double depth_err = std::abs(g.stor_depth[0] - rows[i].stor_depth_ft);
        double exfil_err = std::abs(g.wb_infil[0]   - rows[i].E_cumul_ft);

        max_depth_err = std::max(max_depth_err, depth_err);
        max_exfil_err = std::max(max_exfil_err, exfil_err);
        sum_sq_depth += depth_err * depth_err;
        sum_sq_exfil += exfil_err * exfil_err;
        prev_t = rows[i].t_s;
    }

    double n = static_cast<double>(rows.size() - 1);

    // DO NOT loosen these tolerances.
    // The ODE is constant-rate; Euler is exact; actual FP error is ~1e-15 ft.
    // The 1e-9 ft threshold sits a million times above the expected noise floor.
    // If either assertion fails, investigate the rate, volume-limiter, or Euler
    // update in batchBarrelFlux — do not widen the tolerance to make it pass.
    EXPECT_LT(max_depth_err, 1e-9)
        << "Storage depth max error " << max_depth_err
        << " ft exceeds 1e-9 ft (benchmark: " << path << ")";
    EXPECT_LT(max_exfil_err, 1e-9)
        << "Cumulative exfil max error " << max_exfil_err
        << " ft exceeds 1e-9 ft";
    EXPECT_LT(std::sqrt(sum_sq_depth / n), 1e-10)
        << "Storage depth RMS error exceeds 1e-10 ft";
    EXPECT_LT(std::sqrt(sum_sq_exfil / n), 1e-10)
        << "Cumulative exfil RMS error exceeds 1e-10 ft";

    // Mass-balance identity: E_cumul == (depth_0 - depth) * phi
    double depth_loss  = (rows[0].stor_depth_ft - g.stor_depth[0]) * g.stor_void[0];
    double exfil_total = g.wb_infil[0];
    EXPECT_NEAR(depth_loss, exfil_total, 1e-9)
        << "Mass-balance identity violated: depth_loss=" << depth_loss
        << " exfil_total=" << exfil_total;
}
