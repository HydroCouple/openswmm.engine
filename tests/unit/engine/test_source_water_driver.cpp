// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include "hydrology/SourceWaterDriver.hpp"
#include "core/SimulationContext.hpp"
#include "core/DateTime.hpp"
#include "core/UnitConversion.hpp"
#include "2d/solver/ExplicitInertialSolver.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include "2d/subsurface/SurfaceExchange.hpp"
#include "2d/subsurface/SubsurfaceSolver.hpp"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>

using namespace openswmm;
using namespace openswmm::runoff;
using namespace openswmm::twoD;
namespace {
constexpr double ft2 = .3048 * .3048, ft3 = ft2 * .3048;
constexpr int IT = static_cast<int>(lid::LIDType::INFIL_TRENCH);
SimulationContext model(int n = 3, double rain = 432) {
    SimulationContext c;
    c.options.start_date = datetime::encodeDate(2026, 10, 5);
    c.options.total_duration_ms = 86400000; c.options.ignore_snow_melt = true;
    c.subcatches.resize(n); c.gages.resize(1); c.forcing.resize(0, 0, n, 1, 0);
    c.gage_names.try_add("RG"); c.tables.tables.resize(1);
    c.gages.ts_index[0] = 0; c.gages.interval_sec[0] = 30;
    c.tables.tables[0].x = {c.options.start_date, datetime::addSeconds(c.options.start_date, 30)};
    c.tables.tables[0].y = {rain, 0};
    for (int i = 0; i < n; ++i) {
        c.subcatch_names.try_add("S" + std::to_string(i)); c.subcatches.gage[i] = 0;
        c.subcatches.area[i] = 1 / (43560 * ft2); c.subcatches.width[i] = 1;
        c.subcatches.n_perv[i] = .1; c.subcatches.n_imperv[i] = .01;
        c.subcatches.infil_model[i] = 2; c.subcatches.infil_p1[i] = 3.5;
        c.subcatches.infil_p2[i] = .5; c.subcatches.infil_p3[i] = .26;
    }
    return c;
}
void addLid(SimulationContext& c, int sc = 0, const std::string& type = "IT", double area = .5,
            double saturation = 0, double bottom = 0, double capture = 0,
            int to_perv = 0, const std::string& drain = "") {
    const int li = c.lid_controls.count(); c.lid_controls.names.push_back("L" + std::to_string(li));
    c.lid_controls.lid_type.push_back(type); c.lid_controls.surface.push_back({3, 0, .1, 0, 0});
    c.lid_controls.storage.push_back({12, .4, bottom, 0});
    c.lid_controls.drain.push_back({43.2, .5, 0, 0, 0, 0});
    c.lid_usage.subcatch_index.push_back(sc); c.lid_usage.lid_index.push_back(li);
    c.lid_usage.number.push_back(2); c.lid_usage.area.push_back(area / 2 / ft2);
    c.lid_usage.width.push_back(1); c.lid_usage.init_sat.push_back(saturation);
    c.lid_usage.from_imperv.push_back(capture); c.lid_usage.from_perv.push_back(0);
    c.lid_usage.to_perv.push_back(to_perv); c.lid_usage.drain_to.push_back(drain);
}
std::vector<std::pair<int, double>> areas(const SimulationContext& c) {
    std::vector<std::pair<int, double>> result;
    for (int sc = 0; sc < c.n_subcatches(); ++sc) {
        double a = 1;
        for (int u = 0; u < c.lid_usage.count(); ++u)
            if (c.lid_usage.subcatch_index[u] == sc) a -= c.lid_usage.area[u] * c.lid_usage.number[u] * ft2;
        result.emplace_back(sc, std::max(a, 0.0));
    }
    return result;
}
RunoffSolver::InfiltrationBoundary sealed = [](int, double, double, double& rate) { rate = 0; return true; };
void step(SourceWaterDriver& d, double end, const SourceWaterDriver::BottomCeiling* ceiling = nullptr) {
    ASSERT_EQ(d.stage(0, end, end, &sealed, ceiling), "");
    EXPECT_NEAR(d.balanceResidual(0, true), 0, 1e-10); ASSERT_EQ(d.commit(), "");
}
MeshData mesh() {
    MeshData m; m.resize_vertices(3); m.vx = {0, 2, 0}; m.vy = {0, 0, 2}; m.vz = {10, 10, 10};
    m.resize_triangles(1); m.set_triangle(0, 0, 1, 2); m.mannings_n[0] = .03; buildMeshTopology(m);
    return m;
}
}

TEST(SourceWaterDriver, PrivateTrialsCancelRetryAndKeepUnselectedSourcesUntouched) {
    auto c = model(); addLid(c); SourceWaterDriver d;
    ASSERT_EQ(d.initialize(c, {0}, areas(c)), "");
    ASSERT_EQ(d.stage(0, 1, 1, &sealed), ""); const double trial = d.lids(true).storedVolume();
    EXPECT_GT(trial, 0); EXPECT_DOUBLE_EQ(d.lids().storedVolume(), 0);
    EXPECT_DOUBLE_EQ(c.subcatches.stat_evap_vol[0], 0);
    EXPECT_FALSE(d.stage(0, 2, 2, &sealed).empty()); d.cancel();
    EXPECT_DOUBLE_EQ(d.clocks().groups()[0].completed_end, 0);
    ASSERT_EQ(d.stage(0, 1, 1, &sealed), ""); EXPECT_DOUBLE_EQ(d.lids(true).storedVolume(), trial);
    ASSERT_EQ(d.commit(), ""); EXPECT_DOUBLE_EQ(d.lids().storedVolume(), trial);
    EXPECT_DOUBLE_EQ(d.runoff().soa().depth_perv[2], 0); EXPECT_DOUBLE_EQ(d.ledgers()[2].rain, 0);
    EXPECT_FALSE(d.commit().empty()); EXPECT_FALSE(d.stage(0, 1, 2).empty());
}
TEST(SourceWaterDriver, ComponentAtmosphereIsPrivateAndClosesThePhysicalFootprint){
 auto c=model(1,0);addLid(c,0,"IT",.5,50,432);c.subcatches.frac_imperv[0]=.4;
 c.forcing.subcatch_evap_mode[0]=ForcingMode::OVERRIDE;c.forcing.subcatch_evap_value[0]=.001;
 c.forcing.subcatch_evap_persist[0]=ForcingPersist::PERSIST;
 SourceWaterDriver d;ASSERT_EQ(d.initialize(c,{0},areas(c)),"");
 SourceWaterDriver::BottomCeiling bottom=[](int,int,double,double){return 0.;};
 ASSERT_EQ(d.stage(0,1,1,&sealed,&bottom),"");
 double potential=0,evap=0,area=0;
 for(const auto& b:d.atmosphere(true)){
  potential+=b.potential;evap+=b.evaporation;area+=b.area;
  EXPECT_NEAR(b.potential,.001*b.area,1e-15);EXPECT_LE(b.evaporation,b.potential);
  if(b.kind==SourceEtKind::LID){EXPECT_GT(b.evaporation,0);EXPECT_TRUE(b.soil_eligible);}
 }
 EXPECT_NEAR(area*ft2,1,1e-15);EXPECT_NEAR(potential,.001/ft2,1e-15);
 EXPECT_NEAR(evap,d.ledgers(true)[0].evaporation,1e-15);
 d.cancel();for(const auto& b:d.atmosphere())EXPECT_DOUBLE_EQ(b.potential,0);
 ASSERT_EQ(d.stage(0,1,1,&sealed,&bottom),"");ASSERT_EQ(d.commit(),"");
 double retry=0;for(const auto& b:d.atmosphere())retry+=b.potential;EXPECT_DOUBLE_EQ(retry,potential);
 EXPECT_NEAR(d.balanceResidual(0),0,1e-10);
}
TEST(SourceWaterDriver, StepPetVolumesArePastOnlyAndEquivalentInUsAndSi){
 double prior=0;
 for(auto units:{FlowUnits::CFS,FlowUnits::CMS}){
  auto c=model(1,0);c.options.flow_units=units;c.subcatches.frac_imperv[0]=.4;
  c.subcatches.area[0]=units==FlowUnits::CMS ? .0001 : 1/(43560*ft2);
  c.climate_state.evap_method=climate::EvapMethod::TIMESERIES;c.climate_state.evap_ts_index=1;
  c.climate_state.evap_rate=99;c.tables.tables.resize(2);
  c.tables.tables[1].x={datetime::addSeconds(c.options.start_date,2),datetime::addSeconds(c.options.start_date,4)};
  const double display=units==FlowUnits::CMS ? 25.4 : 1;c.tables.tables[1].y={display,2*display};
  SourceWaterDriver d;ASSERT_EQ(d.initialize(c,{0},{{0,1}}),"");
  const double first=(c.tables.tables[1].x[0]-c.options.start_date)*86400;
  const double second=(c.tables.tables[1].x[1]-c.options.start_date)*86400;
  step(d,first);for(const auto& b:d.atmosphere())EXPECT_DOUBLE_EQ(b.potential,0);
  step(d,6);double total=0;
  for(const auto& b:d.atmosphere()){
   const double integral=(second-first)+2*(6-second);
   total+=b.potential;EXPECT_NEAR(b.potential,integral*display/ucf::UCF(ucf::EVAPRATE,c.options)*b.area,1e-14);
   EXPECT_DOUBLE_EQ(b.evaporation,0);
  }
  if(units==FlowUnits::CFS)prior=total;else EXPECT_NEAR(total,prior,1e-14);
  EXPECT_NEAR(d.balanceResidual(0),0,1e-10);
 }
}

TEST(SourceWaterDriver, VariableCadencesPreserveRunoffRunonAndCyclicHistories) {
    for (bool cycle : {false, true}) {
        auto c = model(); c.subcatches.outlet_subcatch[0] = 1;
        if (cycle) c.subcatches.outlet_subcatch[1] = 0;
        c.subcatches.n_perv[0] = c.subcatches.n_perv[1] = 0;
        SourceWaterDriver d; ASSERT_EQ(d.initialize(c, {1}, areas(c)), "");
        for (double end : {1., 5., 5.5, 12., 30., 37., 38.}) step(d, end);
        EXPECT_NEAR(d.balanceResidual(0), 0, 1e-10);
        EXPECT_GT(d.ledgers()[1].runon, 0); EXPECT_DOUBLE_EQ(d.ledgers()[2].rain, 0);
        if (cycle) { EXPECT_GT(d.pendingVolume(0), 0); EXPECT_TRUE(d.deliveries().empty()); }
        else { EXPECT_NEAR(d.pendingVolume(0), 0, 1e-12); EXPECT_GT(d.ledgers()[1].outlet, 0); }
    }
}

TEST(SourceWaterDriver, InterSubareaTransfersKeepTheirOriginalVolumeWhenStepChanges) {
    for (int route : {1, 2}) {
        auto c = model(); c.subcatches.frac_imperv[0] = .4; c.subcatches.pct_zero[0] = 50;
        c.subcatches.subarea_routing[0] = route; c.subcatches.pct_routed[0] = .7;
        c.subcatches.n_perv[0] = c.subcatches.n_imperv[0] = 0;
        SourceWaterDriver d; ASSERT_EQ(d.initialize(c, {0}, areas(c)), "");
        for (double end : {1., 5., 5.25, 10., 30., 35., 36.}) step(d, end);
        EXPECT_NEAR(d.balanceResidual(0), 0, 1e-10);
        EXPECT_NEAR(d.pendingVolume(0), 0, 1e-12); EXPECT_GT(d.ledgers()[0].outlet, 0);
    }
}

TEST(SourceWaterDriver, PartialLidRunonCountsFullPhysicalAreaOnlyOnce) {
    auto c = model(); c.subcatches.outlet_subcatch[0] = 1; c.subcatches.n_perv[0] = 0;
    addLid(c, 1, "IT", .5); SourceWaterDriver d; ASSERT_EQ(d.initialize(c, {0}, areas(c)), "");
    step(d, 1); step(d, 5); step(d, 6);
    const auto& b = d.ledgers()[1]; EXPECT_GT(b.runon, 0);
    EXPECT_NEAR(d.balanceResidual(0), 0, 1e-10);
    EXPECT_NEAR(d.ledgers()[0].rain + b.rain, 6 * .02 / ft2, 1e-6);
}

TEST(SourceWaterDriver, CaptureDrainsAndPerviousReturnsArePrivatePendingVolumes) {
    auto c = model(); c.subcatches.frac_imperv[0] = .4;
    c.subcatches.n_imperv[0] = c.subcatches.n_perv[0] = 0;
    addLid(c, 0, "IT", .5, 100, 0, 100, 1);
    SourceWaterDriver d; ASSERT_EQ(d.initialize(c, {0}, areas(c)), "");
    for (double end : {1., 5., 30., 31., 37.}) step(d, end);
    EXPECT_GT(d.ledgers()[0].captured, 0); EXPECT_GT(d.ledgers()[0].pervious_return, 0);
    EXPECT_DOUBLE_EQ(d.lids().group(IT).drain_flow[0], 0);
    EXPECT_DOUBLE_EQ(d.lids().group(IT).old_drain_flow[0], 0);
    for (const auto& delivery : d.deliveries()) EXPECT_FALSE(delivery.drain);
    EXPECT_NEAR(d.balanceResidual(0), 0, 1e-10);
}

TEST(SourceWaterDriver, ExplicitLidDrainsConnectAndDeliverToTargetSubcatchment) {
    auto c = model(); addLid(c, 0, "IT", .5, 50, 0, 0, 0, "S1");
    SourceWaterDriver d; ASSERT_EQ(d.initialize(c, {1}, areas(c)), "");
    ASSERT_EQ(d.clocks().groups()[0].subcatches, (std::vector<int>{0, 1}));
    step(d, 1); const double pending = d.pendingVolume(0); EXPECT_GT(pending, 0);
    step(d, 5); EXPECT_NEAR(d.ledgers()[1].runon, pending, 1e-12);
    EXPECT_NEAR(d.balanceResidual(0), 0, 1e-10);
}

TEST(SourceWaterDriver, NodeNamePrecedenceAndSelfDrainsResolveToOwnOutlet) {
    for (const auto& target : {std::string("S1"), std::string("S0")}) {
        auto c = model(); c.node_names.try_add("S1"); c.nodes.resize(1);
        c.subcatches.outlet_node[0] = 0; addLid(c, 0, "IT", .5, 50, 0, 0, 0, target);
        SourceWaterDriver d; ASSERT_EQ(d.initialize(c, {0}, areas(c)), "");
        EXPECT_EQ(d.clocks().groups()[0].subcatches, (std::vector<int>{0})); step(d, 1);
        ASSERT_FALSE(d.deliveries().empty()); EXPECT_TRUE(d.deliveries()[0].drain);
        EXPECT_EQ(d.deliveries()[0].node, 0); EXPECT_DOUBLE_EQ(d.pendingVolume(0), 0);
        EXPECT_DOUBLE_EQ(c.nodes.lid_drain_inflow[0], 0);
    }
}

TEST(SourceWaterDriver, MultipleCoveredBarrelsReturnOnlyTheirOwnRainArea) {
    auto c = model(); addLid(c, 0, "RB", .2, 0, 0, 0, 1);
    addLid(c, 0, "RB", .3, 0, 0, 0, 1);
    // Existing kernel flag; model authoring currently cannot represent COVERED.
    lid::LIDSolver initial; initial.init(c);
    initial.group(static_cast<int>(lid::LIDType::RAIN_BARREL)).stor_covered = {1, 1};
    SourceWaterDriver d; ASSERT_EQ(d.initialize(c, {0}, areas(c), &initial), ""); step(d, 1);
    EXPECT_NEAR(d.pendingVolume(0), .01 * .5 / ft2, 1e-8);
    step(d, 5); EXPECT_GT(d.ledgers()[0].pervious_return, 0);
    EXPECT_DOUBLE_EQ(d.lids().storedVolume(), 0); EXPECT_NEAR(d.balanceResidual(0), 0, 1e-10);
}

TEST(SourceWaterDriver, FullLidCoverageAndIndividualBottomCeilingsConserveWater) {
    auto c = model(); addLid(c, 0, "IT", .5, 50, 432); addLid(c, 0, "IT", .5, 50, 432);
    SourceWaterDriver d; ASSERT_EQ(d.initialize(c, {0}, areas(c)), "");
    SourceWaterDriver::BottomCeiling cap = [](int, int u, double, double) { return u == 0 ? 0 : .0001; };
    step(d, 1, &cap); const auto& g = d.lids().group(IT);
    EXPECT_DOUBLE_EQ(g.infil_loss[0], 0); EXPECT_NEAR(g.infil_loss[1], .0001, 1e-15);
    EXPECT_NEAR(d.ledgers()[0].infiltration, .0001 * .5 / ft2, 1e-15);
    EXPECT_NEAR(d.balanceResidual(0), 0, 1e-10);
}

TEST(SourceWaterDriver, BadBottomBatchCannotPartiallyInstallAnyWaterOrClock) {
    auto c = model(); addLid(c, 0, "IT", .25, 50, 432); addLid(c, 0, "IT", .25, 50, 432);
    SourceWaterDriver d; ASSERT_EQ(d.initialize(c, {0}, areas(c)), ""); const double initial = d.lids().storedVolume();
    SourceWaterDriver::BottomCeiling bad = [](int, int u, double, double) { return u == 0 ? .1 : -1; };
    EXPECT_FALSE(d.stage(0, 1, 1, &sealed, &bad).empty()); EXPECT_DOUBLE_EQ(d.lids().storedVolume(), initial);
    EXPECT_DOUBLE_EQ(d.ledgers()[0].rain, 0); EXPECT_DOUBLE_EQ(d.clocks().groups()[0].completed_end, 0);
    step(d, 1);
}

TEST(SourceWaterDriver, ResolvedPetIsAppliedOnceAndTrialsNeverWriteLidReports) {
    auto c = model(3, 0); addLid(c, 0, "IT", .5, 50); addLid(c, 2, "IT", .5, 50);
    c.forcing.subcatch_evap_mode[0] = ForcingMode::ADD;
    c.forcing.subcatch_evap_value[0] = .001; c.forcing.subcatch_evap_persist[0] = ForcingPersist::PERSIST;
    const auto path = std::filesystem::path(OPENSWMM_R4_WATER_OUT) / "uncommitted_lid.rpt";
    std::filesystem::create_directories(path.parent_path()); std::ofstream(path) << "report sentinel\n";
    c.lid_usage.rpt_file.resize(2); c.lid_usage.rpt_file[0].absolute = path.string();
    SourceWaterDriver d; ASSERT_EQ(d.initialize(c, {0}, areas(c)), "");
    ASSERT_EQ(d.stage(0, 1, 1, &sealed), ""); EXPECT_DOUBLE_EQ(d.lids(true).group(IT).evap_rate_unit[0], .001);
    EXPECT_DOUBLE_EQ(d.lids(true).group(IT).dry_time[1], d.lids().group(IT).dry_time[1]);
    EXPECT_NEAR(d.ledgers(true)[0].evaporation, .001 * .5 / ft2, 1e-15);
    d.cancel(); std::ifstream f(path); std::string sentinel; std::getline(f, sentinel); EXPECT_EQ(sentinel, "report sentinel");
    step(d, 1); std::ifstream committed(path); std::getline(committed, sentinel); EXPECT_EQ(sentinel, "report sentinel");
}

TEST(SourceWaterDriver, UnsupportedProfilesRefuseWithSpecificReasons) {
    for (int mode = 0; mode < 4; ++mode) {
        auto c = model();
        if (mode == 0) c.options.water_age = true;
        if (mode == 1) c.options.heat_transport = true;
        if (mode == 2) c.current_time = 1;
        if (mode == 3) c.subcatches.gw_aquifer[0] = 0;
        SourceWaterDriver d; EXPECT_FALSE(d.initialize(c, {0}, areas(c)).empty());
    }
}

TEST(SourceWaterDriver, IndependentGroupsKeepTheirOwnClockAndPendingState) {
    auto c = model(); addLid(c, 0); addLid(c, 2);
    SourceWaterDriver d; ASSERT_EQ(d.initialize(c, {0, 2}, areas(c)), "");
    step(d, 1); const double initial = d.lids().group(IT).stor_depth[0];
    ASSERT_EQ(d.stage(1, 5, 5, &sealed), ""); ASSERT_EQ(d.commit(), "");
    EXPECT_DOUBLE_EQ(d.clocks().groups()[0].completed_end, 1);
    EXPECT_DOUBLE_EQ(d.clocks().groups()[1].completed_end, 5);
    EXPECT_DOUBLE_EQ(d.lids().group(IT).stor_depth[0], initial);
    EXPECT_NEAR(d.balanceResidual(0), 0, 1e-10); EXPECT_NEAR(d.balanceResidual(1), 0, 1e-10);
}

TEST(SourceWaterDriver, OverCaptureRefusesBeforeAdoptingPrivateStores) {
    auto c = model(); c.subcatches.frac_imperv[0] = 1; c.subcatches.n_imperv[0] = 0;
    addLid(c, 0, "IT", .2, 0, 0, 100); addLid(c, 0, "IT", .2, 0, 0, 100);
    SourceWaterDriver d; ASSERT_EQ(d.initialize(c, {0}, areas(c)), "");
    EXPECT_NE(d.stage(0, 1, 1, &sealed).find("capture exceeds"), std::string::npos);
    EXPECT_DOUBLE_EQ(d.lids().storedVolume(), 0); EXPECT_DOUBLE_EQ(d.ledgers()[0].rain, 0);
    EXPECT_DOUBLE_EQ(d.clocks().groups()[0].completed_end, 0);
}

TEST(SourceWaterDriver, ResolvedLidBatchValidatesAllUnitsBeforeAnyMutation) {
    auto c = model(); addLid(c); addLid(c, 1);
    lid::LIDSolver l; l.init(c);
    std::vector<lid::LIDSolver::CompletedUnitInput> inputs{{IT, 0, .01}, {IT, 1, .01}};
    inputs[1].pet = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW(l.executeCompleted(1, 0, 1, inputs), std::invalid_argument);
    EXPECT_DOUBLE_EQ(l.storedVolume(), 0); EXPECT_DOUBLE_EQ(l.group(IT).wb_inflow[0], 0);
    inputs[1].pet = 0; inputs[1].unit = 0;
    EXPECT_THROW(l.executeCompleted(1, 0, 1, inputs), std::invalid_argument);
    EXPECT_DOUBLE_EQ(l.storedVolume(), 0);
}

TEST(SourceWaterDriver, CompletedMeshHorizonIncludesWetLazySourcesAcrossAdvances) {
    for (int tiers : {1, 3}) {
        auto m = mesh(); SurfaceStateData surface; surface.resize(1, 3); surface.head[0] = 10;
        surface.rainfall[0] = 1e-7; SolverOptions2D options; options.lts_tiers = tiers;
        ExplicitInertialSolver marcher; EXPECT_DOUBLE_EQ(marcher.completedSourceTime(), -1);
        marcher.initialize(m, surface, options); EXPECT_DOUBLE_EQ(marcher.completedSourceTime(), 0);
        EXPECT_DOUBLE_EQ(marcher.advance(0, .5), .5); EXPECT_DOUBLE_EQ(marcher.completedSourceTime(), .5);
        EXPECT_NEAR(surface.volume[0], 1e-7, 1e-15);
        EXPECT_DOUBLE_EQ(marcher.advance(.5, 2), 2); EXPECT_DOUBLE_EQ(marcher.completedSourceTime(), 2);
        EXPECT_NEAR(surface.volume[0], 4e-7, 1e-15); marcher.finalize();
    }
}

TEST(SourceWaterDriver, GroupWaterRefinementConvergesWithPendingTransfersInBalance) {
    const auto run = [](double dt) {
        auto c = model(1, 43.2); c.subcatches.frac_imperv[0] = .4; c.subcatches.slope[0] = .01;
        addLid(c, 0, "IT", .5, 50, 0, 100);
        SourceWaterDriver d; EXPECT_EQ(d.initialize(c, {0}, areas(c)), "");
        for (double end = dt; end <= 60; end += dt) step(d, end);
        EXPECT_NEAR(d.balanceResidual(0), 0, 1e-10);
        return std::pair{d.ledgers()[0].outlet, d.balanceResidual(0)};
    };
    const auto reference = run(.25); double previous = std::numeric_limits<double>::infinity();
    std::filesystem::create_directories(OPENSWMM_R4_WATER_OUT);
    std::ofstream audit(std::filesystem::path(OPENSWMM_R4_WATER_OUT) / "group-refinement.json");
    audit << std::setprecision(17) << "{\"reference_step_seconds\":0.25,\"reference_outlet_ft3\":" << reference.first << ",\"samples\":[";
    bool first = true;
    for (double dt : {4., 2., 1.}) {
        const auto result = run(dt); const double error = std::abs(result.first - reference.first);
        EXPECT_LT(error, previous); previous = error;
        if (!first) audit << ','; first = false;
        audit << "{\"step_seconds\":" << dt << ",\"outlet_ft3\":" << result.first
              << ",\"difference_ft3\":" << error << ",\"balance_residual_ft3\":" << result.second << '}';
    }
    audit << "],\"production_performance_measurement\":false}\n";
}

TEST(SourceWaterDriver, RealMarcherCompletionSupportsOneCellTwoSourceSettlement) {
    auto m = mesh(); SurfaceStateData surface; surface.resize(1, 3); surface.head[0] = 10;
    SolverOptions2D options; options.lts_tiers = 3;
    ExplicitInertialSolver marcher; marcher.initialize(m, surface, options);
    auto c = model(); c.subcatches.outlet_subcatch[0] = 1;
    SourceWaterDriver d; ASSERT_EQ(d.initialize(c, {0}, areas(c)), "");
    EXPECT_FALSE(d.stage(0, 1, marcher.completedSourceTime(), &sealed).empty());
    EXPECT_DOUBLE_EQ(marcher.advance(0, 1), 1); EXPECT_DOUBLE_EQ(marcher.completedSourceTime(), 1);
    ASSERT_EQ(d.stage(0, 1, marcher.completedSourceTime(), &sealed), ""); d.cancel();
    InertialEdges edges; edges.build(m); SubsurfaceConfig cfg; cfg.options.authored = true;
    cfg.options.closure = GwClosure::CLOSED_FORM; cfg.options.force_closed_form = true;
    GwAquiferRow row; row.Ks = .01; row.zs = 2; row.hg0 = 1; cfg.rows.push_back(row);
    SubsurfaceSolver gw; std::vector<std::string> warnings;
    ASSERT_EQ(gw.initialize(m, edges, options, {}, 0, cfg, warnings), "");
    SurfaceExchange exchange;
    std::vector<SurfaceIntakeRequest> requests;
    for (int sc : {0, 1}) requests.push_back({SurfaceDonorKind::NON_LID, "S" + std::to_string(sc), -1, 0, 1, 0, .003048, .003048});
    ASSERT_EQ(exchange.plan(gw, 0, 1, marcher.completedSourceTime(), requests), "");
    RunoffSolver::InfiltrationBoundary intake = [&](int sc, double, double available, double& rate) {
        rate = std::min(available, exchange.awards()[sc].maximum / .3048); return true;
    };
    ASSERT_EQ(d.stage(0, 1, marcher.completedSourceTime(), &intake), "");
    std::vector<SurfaceIntakeActual> actual;
    for (int sc : {0, 1}) actual.push_back({d.ledgers(true)[sc].infiltration * ft3, {}});
    EXPECT_DOUBLE_EQ(gw.state().xacc_from_surface[0], 0);
    ASSERT_EQ(exchange.commit(gw, actual), ""); ASSERT_EQ(d.commit(), "");
    EXPECT_NEAR(gw.state().xacc_from_surface[0], actual[0].volume + actual[1].volume, 1e-15);
    EXPECT_NEAR(d.balanceResidual(0), 0, 1e-10); EXPECT_DOUBLE_EQ(surface.volume[0], 0);
    std::filesystem::create_directories(OPENSWMM_R4_WATER_OUT);
    std::ofstream audit(std::filesystem::path(OPENSWMM_R4_WATER_OUT) / "mesh-adapter.json");
    audit << std::setprecision(17) << "{\"mesh_sources_completed_seconds\":" << marcher.completedSourceTime()
          << ",\"source_group_completed_seconds\":" << d.clocks().groups()[0].completed_end
          << ",\"accepted_source_volume_m3\":" << gw.state().xacc_from_surface[0]
          << ",\"source_balance_residual_ft3\":" << d.balanceResidual(0) << ",\"production_caller\":false}\n";
    marcher.finalize(); EXPECT_DOUBLE_EQ(marcher.completedSourceTime(), -1);
}

TEST(SourceWaterDriver, GeneralizedLayeredAndRoofUnitsUseExistingCompletedLaws) {
    for(const std::string type:{"BC","RG","GR","PP","RD"}) {
        SCOPED_TRACE(type);auto c=model(1);addLid(c,0,type,.5,50,432);
        c.lid_controls.soil={{12,.45,.3,.1,43.2,4,3.5}};
        c.lid_controls.pavement={{6,.2,0,43.2,0,0}};c.lid_controls.drainmat={{3,.5,.1}};
        SourceWaterDriver d;ASSERT_EQ(d.initialize(c,{0},areas(c)),"");
        SourceWaterDriver::BottomCeiling cap=[](int,int,double,double){return .0001;};
        for(double end:{.1,.3,1.,2.125,4.})step(d,end,&cap);
    }
}

TEST(SourceWaterDriver, PartialNativeSoilStateAdvancesOnlyOutsideAndCancelsWithItsTrial) {
    for(double fraction:{0.,.25,.5,1.})for(int method:{0,1,2,3,4,5}){
        SCOPED_TRACE(std::to_string(fraction)+"/method="+std::to_string(method));auto c=model(1);
        c.subcatches.infil_model[0]=method;
        if(method<=1){c.subcatches.infil_p1[0]=3.5;c.subcatches.infil_p2[0]=.5;c.subcatches.infil_p3[0]=4;c.subcatches.infil_p4[0]=7;}
        if(method==4){c.subcatches.infil_p1[0]=75;c.subcatches.infil_p3[0]=7;}
        SourceWaterDriver d;ASSERT_EQ(d.initialize(c,{0},areas(c)),"");ASSERT_EQ(d.configureSpatialCoverage({fraction}),"");
        int m;double original[6],trial[6],cancelled[6];d.runoff().infil_get_state(0,m,original);
        ASSERT_EQ(d.stage(0,1,1,&sealed),"");d.runoff(true).infil_get_state(0,m,trial);
        const auto b=d.ledgers(true)[0];EXPECT_DOUBLE_EQ(b.spatial_infiltration,0);
        EXPECT_NEAR(b.infiltration,b.outside_infiltration,1e-12);
        if(fraction<1)EXPECT_GT(b.outside_infiltration,0);else EXPECT_DOUBLE_EQ(b.outside_infiltration,0);
        d.cancel();d.runoff().infil_get_state(0,m,cancelled);
        for(int j=0;j<6;++j)EXPECT_DOUBLE_EQ(original[j],cancelled[j]);
        ASSERT_EQ(d.stage(0,1,1,&sealed),"");ASSERT_EQ(d.commit(),"");
        for(int j=0;j<6;++j)if(fraction==1)EXPECT_DOUBLE_EQ(trial[j],original[j]);
        EXPECT_NEAR(d.balanceResidual(0),0,1e-10);
    }
}
TEST(SourceWaterDriver, SwaleFootprintRequiresItsTrueGeometryAndConservedCompletedWater) {
    auto c=model(1);addLid(c,0,"VS",.5,0,0);c.lid_controls.surface[0]={12,.1,.1,1,1};
    SourceWaterDriver d;ASSERT_EQ(d.initialize(c,{0},areas(c)),"");ASSERT_EQ(d.configureSpatialCoverage({.5}),"");
    SourceWaterDriver::BottomCeiling cap=[](int,int,double,double){return .0001;};
    for(double end:{.1,.3,1.,2.125,4.})step(d,end,&cap);
}

TEST(SourceWaterDriver, ConfiguredInsideFootprintsCannotFallBackToUnboundedNativeLoss) {
    for(bool with_lid:{false,true}) {
        auto c=model(1);if(with_lid)addLid(c,0,"IT",.5,50,432);
        SourceWaterDriver d;ASSERT_EQ(d.initialize(c,{0},areas(c)),"");
        ASSERT_EQ(d.configureSpatialCoverage({.5}),"");
        EXPECT_FALSE(d.stage(0,1,1).empty());EXPECT_DOUBLE_EQ(d.clocks().groups()[0].completed_end,0);
        EXPECT_DOUBLE_EQ(d.ledgers()[0].rain,0);
        RunoffSolver::InfiltrationBoundary refuse=[](int,double,double,double&){return false;};
        SourceWaterDriver::BottomCeiling cap=[](int,int,double,double){return .0001;};
        EXPECT_FALSE(d.stage(0,1,1,&refuse,&cap).empty());EXPECT_DOUBLE_EQ(d.ledgers()[0].infiltration,0);
        ASSERT_EQ(d.stage(0,1,1,&sealed,&cap),"");ASSERT_EQ(d.commit(),"");
        EXPECT_NEAR(d.balanceResidual(0),0,1e-10);
        EXPECT_FALSE(d.configureSpatialCoverage({.25}).empty());
    }
}

TEST(SourceWaterDriver, PartialNativeHistoryRecoversThroughDryAndMonthlyIntervals) {
    for(int method:{0,1,2,3,4,5}) {
        SCOPED_TRACE(method);
        auto c=model(1);c.subcatches.infil_model[0]=method;
        if(method<=1){c.subcatches.infil_p1[0]=3.5;c.subcatches.infil_p2[0]=.5;c.subcatches.infil_p3[0]=4;c.subcatches.infil_p4[0]=7;}
        if(method==4){c.subcatches.infil_p1[0]=75;c.subcatches.infil_p3[0]=7;}
        c.options.start_date=datetime::encodeDate(2026,10,31)+datetime::encodeTime(23,59,0);
        c.tables.tables[0].x={c.options.start_date,datetime::addSeconds(c.options.start_date,30)};
        c.patterns.factors={std::vector<double>(12,1)};c.patterns.factors[0][10]=.2;
        c.subcatch_infil_pattern={0};
        c.forcing.subcatch_evap_mode[0]=ForcingMode::OVERRIDE;c.forcing.subcatch_evap_value[0]=.001;
        c.forcing.subcatch_evap_persist[0]=ForcingPersist::PERSIST;
        SourceWaterDriver d;ASSERT_EQ(d.initialize(c,{0},areas(c)),"");ASSERT_EQ(d.configureSpatialCoverage({.25}),"");
        int m;double initial[6],final[6];d.runoff().infil_get_state(0,m,initial);
        bool changed=false;
        for(double end:{1.,5.,30.,37.,60.,70.,100.,600.,3600.}){
            step(d,end);d.runoff().infil_get_state(0,m,final);
            for(int j=0;j<6;++j)changed|=initial[j]!=final[j];
        }
        d.runoff().infil_get_state(0,m,final);
        for(int j=0;j<6;++j)EXPECT_TRUE(std::isfinite(final[j]));
        if(method!=5)EXPECT_TRUE(changed);else EXPECT_FALSE(changed); // Constant capacity has no recovery state.
        EXPECT_GT(d.ledgers()[0].outside_infiltration,0);
        EXPECT_DOUBLE_EQ(d.ledgers()[0].spatial_infiltration,0);EXPECT_DOUBLE_EQ(d.runoff().soa().depth_perv[0],0);
    }
}

TEST(SourceWaterDriver, SwaleVolumeRefinesAndBoundsOverflowAndDryDemand) {
    std::filesystem::create_directories(OPENSWMM_R4_WATER_OUT);
    std::ofstream audit(std::filesystem::path(OPENSWMM_R4_WATER_OUT)/"swale-refinement.jsonl");
    audit<<std::setprecision(17);
    for(double fraction:{1.,.5}) {
    std::vector<double> levels,losses;
    for(double dt:{.4,.2,.1,.05,.0125}) {
        auto c=model(1,0);addLid(c,0,"VS",.5,0,0);c.lid_controls.surface[0]={12,.1,.1,1,1};
        c.forcing.subcatch_rainfall_mode[0]=ForcingMode::OVERRIDE;c.forcing.subcatch_rainfall_value[0]=86.4; // .002 ft/s
        c.forcing.subcatch_rainfall_persist[0]=ForcingPersist::PERSIST;
        c.forcing.subcatch_evap_mode[0]=ForcingMode::OVERRIDE;c.forcing.subcatch_evap_value[0]=1e-5;
        c.forcing.subcatch_evap_persist[0]=ForcingPersist::PERSIST;
        SourceWaterDriver d;ASSERT_EQ(d.initialize(c,{0},areas(c)),"");ASSERT_EQ(d.configureSpatialCoverage({fraction}),"");
        SourceWaterDriver::BottomCeiling cap=[](int,int,double,double){return .0001;};
        const int count=std::lround(4/dt);for(int k=1;k<=count;++k)step(d,k*dt,&cap);
        const auto& g=d.lids().group(int(lid::LIDType::VEG_SWALE));
        levels.push_back(g.surf_depth[0]);losses.push_back(g.wb_infil[0]);
        audit<<"{\"inside_fraction\":"<<fraction<<",\"dt_seconds\":"<<dt<<",\"depth_ft\":"<<levels.back()<<",\"infiltration_ft\":"<<losses.back()
             <<",\"balance_ft3\":"<<d.balanceResidual(0)<<"}\n";
    }
    for(int k=1;k<4;++k)EXPECT_LT(std::abs(levels[k]-levels.back()),std::abs(levels[k-1]-levels.back()));
    // Native Green-Ampt regime switches need not give monotone flux error.
    // Both footprint paths must reduce the coarse loss error and resolve the
    // final 0.05 s loss within 0.2% of the independent finer trajectory.
    EXPECT_LT(std::abs(losses[3]-losses.back()),std::abs(losses[0]-losses.back()));
    EXPECT_NEAR(losses[3],losses.back(),.002*losses.back());
    }
    for(double fraction:{0.,.25,.5,1.})for(double rain:{0.,43200.}) {
        auto c=model(1,rain);addLid(c,0,"VS",.5,0,0);c.lid_controls.surface[0]={.12,.1,.1,1,1};
        c.forcing.subcatch_evap_mode[0]=ForcingMode::OVERRIDE;c.forcing.subcatch_evap_value[0]=.01;
        c.forcing.subcatch_evap_persist[0]=ForcingPersist::PERSIST;
        SourceWaterDriver d;ASSERT_EQ(d.initialize(c,{0},areas(c)),"");ASSERT_EQ(d.configureSpatialCoverage({fraction}),"");
        SourceWaterDriver::BottomCeiling cap=[](int,int,double,double){return 0.;};
        for(double end:{.1,.3,1.,3.})step(d,end,&cap);
        const auto& g=d.lids().group(int(lid::LIDType::VEG_SWALE));
        EXPECT_GE(g.surf_depth[0],0);EXPECT_LE(g.surf_depth[0],g.surf_store[0]+1e-14);
        EXPECT_NEAR(d.ledgers()[0].infiltration,d.ledgers()[0].spatial_infiltration+d.ledgers()[0].outside_infiltration,1e-10);
        if(rain==0){EXPECT_DOUBLE_EQ(d.ledgers()[0].evaporation,0);EXPECT_DOUBLE_EQ(d.ledgers()[0].infiltration,0);}
        else EXPECT_GT(d.ledgers()[0].outlet,0);
    }
}

TEST(SourceWaterDriver, PartialPerviousWaterAndNativeHistoryAreEquivalentInUsAndSi) {
    std::vector<double> prior;
    for(auto units:{FlowUnits::CFS,FlowUnits::CMS}) {
        auto c=model(1);c.options.flow_units=units;const bool si=units==FlowUnits::CMS;
        c.subcatches.area[0]=si?.0001:1/(43560*ft2);c.subcatches.width[0]=si?.3048:1;
        c.subcatches.infil_p1[0]=3.5*(si?25.4:1);c.subcatches.infil_p2[0]=.5*(si?25.4:1);
        c.tables.tables[0].y={432*(si?25.4:1),0};
        SourceWaterDriver d;ASSERT_EQ(d.initialize(c,{0},{{0,1}}),"");ASSERT_EQ(d.configureSpatialCoverage({.5}),"");
        RunoffSolver::InfiltrationBoundary intake=[](int,double,double available,double& q){q=std::min(.001,available);return true;};
        for(double end:{.1,.3,1.,2.125,4.,30.,37.,100.}){ASSERT_EQ(d.stage(0,end,end,&intake),"");ASSERT_EQ(d.commit(),"");}
        const auto& b=d.ledgers()[0];int method;double state[6];d.runoff().infil_get_state(0,method,state);
        std::vector<double> values{b.rain,b.infiltration,b.spatial_infiltration,b.outside_infiltration,d.runoff().soa().depth_perv[0]};
        values.insert(values.end(),state,state+6);
        if(!si)prior=values;else for(int i=0;i<int(values.size());++i)EXPECT_NEAR(values[i],prior[i],1e-11);
        EXPECT_NEAR(d.balanceResidual(0),0,1e-10);
    }
}

TEST(SourceWaterDriver, MixedLidUsagesCoveredRainAndDrainsKeepTheirConnectedGroupVolumes) {
    auto c=model(2);c.subcatches.outlet_subcatch[0]=1;
    const std::vector<std::string> types{"BC","RG","GR","IT","PP","RB","VS","RD"};
    for(const auto& type:types)addLid(c,0,type,.1,30,432,0,0,"S1");
    c.lid_controls.soil.resize(types.size());c.lid_controls.pavement.resize(types.size());c.lid_controls.drainmat.resize(types.size());
    for(int i=0;i<int(types.size());++i){
        const auto& type=types[i];
        if(type=="BC"||type=="RG"||type=="GR"||type=="PP")c.lid_controls.soil[i]={12,.45,.3,.1,43.2,4,3.5};
        if(type=="PP")c.lid_controls.pavement[i]={6,.2,0,43.2,0,0};
        if(type=="GR")c.lid_controls.drainmat[i]={3,.5,.1};
        if(type=="VS")c.lid_controls.surface[i]={12,.1,.1,1,1};
    }
    lid::LIDSolver initial;initial.init(c);initial.group(int(lid::LIDType::RAIN_BARREL)).stor_covered[0]=1;
    SourceWaterDriver d;ASSERT_EQ(d.initialize(c,{0},areas(c),&initial),"");ASSERT_EQ(d.configureSpatialCoverage({.5,0}),"");
    ASSERT_EQ(d.clocks().groups().size(),1u);SourceWaterDriver::BottomCeiling cap=[](int,int,double,double){return .0001;};
    for(double end:{.1,.3,1.,2.125,4.,30.,37.,100.})step(d,end,&cap);
    EXPECT_GT(d.ledgers()[1].runon,0);EXPECT_GT(d.ledgers()[0].spatial_infiltration,0);
    EXPECT_GT(d.ledgers()[0].outside_infiltration,0);EXPECT_DOUBLE_EQ(d.ledgers()[1].spatial_infiltration,0);
    double spatial=0;for(const auto& [t,u]:d.lids().usageOrder()){
        const auto& g=d.lids().group(t);spatial+=g.wb_spatial_infil[u]*g.area[u];
        if(g.type==lid::LIDType::RAIN_BARREL||g.type==lid::LIDType::GREEN_ROOF||g.type==lid::LIDType::ROOF_DISCON)
            EXPECT_DOUBLE_EQ(g.wb_spatial_infil[u],0);
    }
    EXPECT_NEAR(spatial,d.ledgers()[0].spatial_infiltration,1e-11);
    EXPECT_NEAR(d.balanceResidual(0),0,1e-10);
}
