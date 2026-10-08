// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include "hydrology/SourceClockGroups.hpp"
#include "hydrology/LID.hpp"
#include "core/SWMMEngine.hpp"
#include "core/DateTime.hpp"
#include "core/UnitConversion.hpp"
#include <openswmm/engine/openswmm_engine.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>

using namespace openswmm;
using namespace openswmm::runoff;
namespace {
SimulationContext context(int n = 3) {
    SimulationContext c;
    c.options.start_date = datetime::encodeDate(2026, 10, 5);
    c.options.end_date = c.options.start_date + 1;
    c.options.total_duration_ms = 86400000;
    c.options.ignore_snow_melt = true;
    c.subcatches.resize(n); c.gages.resize(1); c.forcing.resize(0, 0, n, 1, 0);
    c.gage_names.try_add("RG");
    c.tables.tables.resize(1); c.gages.ts_index[0] = 0; c.gages.interval_sec[0] = 300;
    c.tables.tables[0].x = {c.options.start_date}; c.tables.tables[0].y = {0};
    for (int i = 0; i < n; ++i) {
        c.subcatch_names.try_add("S" + std::to_string(i));
        c.subcatches.gage[i] = 0; c.subcatches.area[i] = 1.0 / (43560 * .3048 * .3048);
        c.subcatches.n_perv[i] = .1; c.subcatches.n_imperv[i] = .01;
        c.subcatches.width[i] = 1; c.subcatches.infil_model[i] = 2;
        c.subcatches.infil_p1[i] = 3.5; c.subcatches.infil_p2[i] = .5; c.subcatches.infil_p3[i] = .26;
    }
    return c;
}
void rain(SimulationContext& c, std::vector<double> times, std::vector<double> rates, double period = 1) {
    for (double& t : times) t = datetime::addSeconds(c.options.start_date, t);
    c.tables.tables[0].x = std::move(times); c.tables.tables[0].y = std::move(rates);
    c.gages.interval_sec[0] = period;
}
double supply(const SourceClockGroup& g, int sc, bool pet = false) {
    double total = 0;
    for (const auto& f : g.pending) for (const auto& s : f.sources)
        if (s.subcatch == sc) total += (pet ? s.pet : s.rain) * (f.end - f.start);
    return total;
}
RunoffSolver::InfiltrationBoundary sealed = [](int, double, double, double& rate) { rate = 0; return true; };
void solve(SimulationContext& c, RunoffSolver& r, const SourceClockGroup& g,
           const RunoffSolver::InfiltrationBoundary* b = nullptr) {
    for (const auto& f : g.pending)
        r.execute(c, f.end - f.start, 0, f.infil_factor, f.recovery_factor, f.month, b, &f.sources);
}
}

TEST(SourceClockGroups, WholeRunoffAndLidDrainGraphsIncludeUpstreamCyclesAndReturns) {
    auto c = context(5); c.subcatches.outlet_subcatch[0] = 1; c.subcatches.outlet_subcatch[1] = 0;
    c.subcatches.outlet_subcatch[3] = 3;
    c.lid_usage.subcatch_index = {2, 1}; c.lid_usage.drain_to = {"s1", "S1"}; c.lid_usage.to_perv = {0, 1};
    SourceClockGroups a, b; ASSERT_EQ(a.initialize(c, {4, 1, 1}), ""); ASSERT_EQ(b.initialize(c, {1, 4}), "");
    ASSERT_EQ(a.groups().size(), 2); EXPECT_EQ(a.groups()[0].subcatches, (std::vector<int>{0, 1, 2}));
    EXPECT_EQ(a.groups()[0].lid_usages, (std::vector<int>{0, 1})); EXPECT_EQ(a.groups()[1].subcatches, (std::vector<int>{4}));
    EXPECT_EQ(a.groups()[0].subcatches, b.groups()[0].subcatches); EXPECT_EQ(a.groupForSubcatch(3), -1);
    // Shared rain gage and common network outlet do not connect source clocks.
    c.subcatches.outlet_node = {0, 0, 0, 0, 0}; ASSERT_EQ(b.initialize(c, {3, 4}), "");
    EXPECT_EQ(b.groups().size(), 2); EXPECT_EQ(b.groupForSubcatch(-1), -1); EXPECT_EQ(b.groupForSubcatch(5), -1);
}

TEST(SourceClockGroups, DrainNodePrecedenceAndInvalidTopologyAreExplicit) {
    auto c = context(); c.node_names.try_add("S2");
    c.lid_usage.subcatch_index = {0}; c.lid_usage.drain_to = {"s2"};
    SourceClockGroups a; ASSERT_EQ(a.initialize(c, {0}), ""); EXPECT_EQ(a.groups()[0].subcatches, (std::vector<int>{0}));
    c.lid_usage.drain_to[0] = "missing"; EXPECT_FALSE(a.initialize(c, {0}).empty());
    EXPECT_EQ(a.groups()[0].subcatches, (std::vector<int>{0}));
    c.lid_usage.drain_to[0] = ""; c.subcatches.outlet_subcatch[1] = 9;
    EXPECT_FALSE(a.initialize(c, {0}).empty()); c.subcatches.outlet_subcatch[1] = -1;
    EXPECT_FALSE(a.initialize(c, {3}).empty()); c.lid_usage.subcatch_index[0] = -1;
    EXPECT_FALSE(a.initialize(c, {0}).empty());
}

TEST(SourceClockGroups, IndependentCompletedClocksCancelRetryAndNoReplay) {
    auto c = context(); SourceClockGroups a; ASSERT_EQ(a.initialize(c, {0, 2}), "");
    EXPECT_FALSE(a.stage(0, 300, .5).empty()); EXPECT_FALSE(a.stage(0, 0, .5).empty());
    EXPECT_FALSE(a.stage(0, 1, std::numeric_limits<double>::quiet_NaN()).empty());
    ASSERT_EQ(a.stage(0, .5, .5), ""); EXPECT_DOUBLE_EQ(a.groups()[0].completed_end, 0);
    EXPECT_FALSE(a.stage(0, 1, 1).empty()); EXPECT_FALSE(a.initialize(c, {0}).empty());
    ASSERT_EQ(a.stage(1, .25, .5), ""); ASSERT_EQ(a.commit(1), "");
    EXPECT_DOUBLE_EQ(a.groups()[1].completed_end, .25); a.cancel(0); EXPECT_DOUBLE_EQ(a.groups()[0].completed_end, 0);
    ASSERT_EQ(a.stage(0, .5, .5), ""); ASSERT_EQ(a.commit(0), "");
    EXPECT_FALSE(a.stage(0, .5, 1).empty()); EXPECT_FALSE(a.commit(0).empty());
    ASSERT_EQ(a.stage(0, 1, 1), ""); EXPECT_DOUBLE_EQ(a.groups()[0].pending.front().start, .5);
    a.cancel(0); EXPECT_FALSE(a.initialize(c, {0}).empty());
}

TEST(SourceClockGroups, SubsecondRainCannotBorrowFutureGlobalCursorOrRecord) {
    auto c = context(); rain(c, {.75, 2}, {36, 360});
    c.gages.rainfall[0] = 360; c.gages.st_cur[0] = 1; c.tables.tables[0].cursor.index = 1;
    c.climate_state.evap_rate = 100; SourceClockGroups a; ASSERT_EQ(a.initialize(c, {0}), "");
    ASSERT_EQ(a.stage(0, .5, .5), ""); EXPECT_DOUBLE_EQ(supply(a.groups()[0], 0), 0);
    ASSERT_EQ(a.commit(0), ""); ASSERT_EQ(a.stage(0, 1, 1), "");
    const double change = (c.tables.tables[0].x[0] - c.options.start_date) * 86400;
    EXPECT_NEAR(supply(a.groups()[0], 0), (1 - change) * 36 / ucf::UCF(ucf::RAINFALL, c.options), 1e-15);
    EXPECT_DOUBLE_EQ(supply(a.groups()[0], 0, true), 0);
    EXPECT_DOUBLE_EQ(c.gages.rainfall[0], 360); EXPECT_EQ(c.gages.st_cur[0], 1); EXPECT_EQ(c.tables.tables[0].cursor.index, 1);
    // Input records are an immutable snapshot; subsequent global cursor and
    // value changes cannot rewrite already staged forcing.
    c.tables.tables[0].y[0] = 999; EXPECT_DOUBLE_EQ(a.groups()[0].pending.back().sources[0].rain, 36 / ucf::UCF(ucf::RAINFALL, c.options));
}

TEST(SourceClockGroups, RainIntensityVolumeCumulativeResetAndDryGapsInUsAndSi) {
    for (auto units : {FlowUnits::CFS, FlowUnits::CMS}) for (int type = 0; type < 3; ++type) {
        auto c = context(1); c.options.flow_units = units; c.gages.rain_type[0] = type;
        rain(c, {0, 2, 4}, type == 2 ? std::vector<double>{1, 2, .5} : std::vector<double>{1, 1, .5});
        SourceClockGroups a; ASSERT_EQ(a.initialize(c, {0}), ""); ASSERT_EQ(a.stage(0, 6, 6), "");
        const double expected = type == 0 ? 2.5 / ucf::UCF(ucf::RAINFALL, c.options)
                                          : 2.5 * 3600 / ucf::UCF(ucf::RAINFALL, c.options);
        EXPECT_NEAR(supply(a.groups()[0], 0), expected, 1e-12);
        int dry = 0; for (const auto& f : a.groups()[0].pending) if (f.sources[0].rain == 0) ++dry;
        EXPECT_GE(dry, 2);
    }
}

TEST(SourceClockGroups, RainFileUnitsScaleMonthlyFactorsAndPersistentPrescriptions) {
    auto c = context(2); c.options.flow_units = FlowUnits::CMS;
    c.gages.source[0] = RainSource::FILE_RAIN; c.gages.file_format[0] = RainFileFormat::STAN_PRCP;
    c.gages.rain_series[0] = c.tables.tables[0]; c.gages.rain_series[0].y[0] = 1;
    c.gages.scale_factor[0] = 2; c.adjust_rain[9] = 3; c.subcatches.rain_scale_factor[0] = 4;
    c.forcing.gage_rainfall_mode[0] = ForcingMode::ADD; c.forcing.gage_rainfall_value[0] = 1;
    c.forcing.gage_rainfall_persist[0] = ForcingPersist::PERSIST;
    c.forcing.subcatch_rainfall_mode[1] = ForcingMode::OVERRIDE; c.forcing.subcatch_rainfall_value[1] = 7;
    c.forcing.subcatch_rainfall_persist[1] = ForcingPersist::PERSIST;
    c.subcatches.outlet_subcatch[0] = 1; SourceClockGroups a; ASSERT_EQ(a.initialize(c, {0}), "");
    ASSERT_EQ(a.stage(0, 1, 1), "");
    EXPECT_NEAR(supply(a.groups()[0], 0), (25.4 * 2 + 1) * 3 * 4 / ucf::UCF(ucf::RAINFALL, c.options), 1e-15);
    EXPECT_DOUBLE_EQ(supply(a.groups()[0], 1), 7 / ucf::UCF(ucf::RAINFALL, c.options));
}

TEST(SourceClockGroups, StepPetIsPastOnlyAndOverridesRespectDryOnly) {
    auto c = context(); c.climate_state.evap_method = climate::EvapMethod::TIMESERIES;
    c.climate_state.evap_ts_index = 1; c.tables.tables.resize(2);
    c.tables.tables[1].x = {datetime::addSeconds(c.options.start_date, 2), datetime::addSeconds(c.options.start_date, 4)};
    c.tables.tables[1].y = {1, 2}; c.climate_state.evap_rate = 99;
    c.subcatches.outlet_subcatch[0] = 1; c.subcatches.outlet_subcatch[1] = 2;
    c.options.evap_dry_only = true; c.forcing.subcatch_rainfall_mode[0] = ForcingMode::OVERRIDE;
    c.forcing.subcatch_rainfall_value[0] = 1; c.forcing.subcatch_rainfall_persist[0] = ForcingPersist::PERSIST;
    c.forcing.subcatch_evap_mode[0] = ForcingMode::OVERRIDE; c.forcing.subcatch_evap_value[0] = .01;
    c.forcing.subcatch_evap_persist[0] = ForcingPersist::PERSIST;
    c.forcing.subcatch_evap_mode[1] = ForcingMode::ADD; c.forcing.subcatch_evap_value[1] = .02;
    c.forcing.subcatch_evap_persist[1] = ForcingPersist::PERSIST;
    SourceClockGroups a; ASSERT_EQ(a.initialize(c, {0}), ""); ASSERT_EQ(a.stage(0, 1, 1), "");
    EXPECT_DOUBLE_EQ(supply(a.groups()[0], 2, true), 0); EXPECT_DOUBLE_EQ(supply(a.groups()[0], 0, true), .01);
    EXPECT_DOUBLE_EQ(supply(a.groups()[0], 1, true), .02);
    ASSERT_EQ(a.commit(0), ""); ASSERT_EQ(a.stage(0, 5, 5), "");
    const double t2 = (c.tables.tables[1].x[0] - c.options.start_date) * 86400;
    const double t4 = (c.tables.tables[1].x[1] - c.options.start_date) * 86400;
    EXPECT_NEAR(supply(a.groups()[0], 2, true), ((t4 - t2) + 2 * (5 - t4)) / ucf::UCF(ucf::EVAPRATE, c.options), 1e-15);
}

TEST(SourceClockGroups, MonthBoundaryChangesPetRainInfiltrationAndRecoveryTogether) {
    auto c = context(1); c.options.start_date = datetime::encodeDate(2026, 10, 31) + datetime::encodeTime(23, 59, 59);
    c.options.end_date = c.options.start_date + 1; rain(c, {0}, {1}, 300);
    c.climate_state.evap_method = climate::EvapMethod::MONTHLY;
    c.climate_state.monthly_evap[9] = 1; c.climate_state.monthly_evap[10] = 2;
    c.adjust_rain[10] = 3; c.adjust_hydcon[10] = .5; c.adjust_evap[10] = .25;
    c.patterns.factors = {std::vector<double>(12, 1)}; c.patterns.factors[0][10] = .75;
    c.climate_state.recovery_pat_index = 0; SourceClockGroups a; ASSERT_EQ(a.initialize(c, {0}), "");
    ASSERT_EQ(a.stage(0, 3, 3), ""); ASSERT_EQ(a.groups()[0].pending.size(), 2);
    const auto& old = a.groups()[0].pending[0]; const auto& now = a.groups()[0].pending[1];
    EXPECT_EQ(old.month, 9); EXPECT_EQ(now.month, 10); EXPECT_DOUBLE_EQ(now.infil_factor, .5);
    EXPECT_DOUBLE_EQ(now.recovery_factor, .75); EXPECT_DOUBLE_EQ(now.sources[0].rain, 3 * old.sources[0].rain);
    EXPECT_DOUBLE_EQ(now.sources[0].pet, 2.25 * old.sources[0].pet); EXPECT_DOUBLE_EQ(old.end, now.start);
}

TEST(SourceClockGroups, UnsupportedHistoriesHaveNamedRefusalsAndNoPartialInitialization) {
    auto c = context(); SourceClockGroups a; ASSERT_EQ(a.initialize(c, {}), ""); EXPECT_TRUE(a.groups().empty());
    c.options.ignore_snow_melt = false; EXPECT_NE(a.initialize(c, {0}).find("snow"), std::string::npos);
    c.options.ignore_snow_melt = true; c.climate_state.evap_method = climate::EvapMethod::TEMPERATURE;
    EXPECT_NE(a.initialize(c, {0}).find("history"), std::string::npos); c.climate_state.evap_method = climate::EvapMethod::CONSTANT;
    c.forcing.subcatch_evap_mode[0] = ForcingMode::ADD;
    EXPECT_NE(a.initialize(c, {0}).find("one-step"), std::string::npos); c.forcing.subcatch_evap_mode[0] = ForcingMode::NONE;
    c.node_subtypes.outfalls.add_default(0); c.node_subtypes.outfalls.route_to[0] = 0;
    EXPECT_NE(a.initialize(c, {0}).find("outfall"), std::string::npos); c.node_subtypes.outfalls.route_to[0] = -1;
    c.files.runoff_mode = FileMode::USE; EXPECT_NE(a.initialize(c, {0}).find("USE RUNOFF"), std::string::npos);
    EXPECT_TRUE(a.groups().empty()); c.files.runoff_mode = FileMode::NONE;
    c.tables.tables[0].x.push_back(c.options.start_date); c.tables.tables[0].y.push_back(1);
    EXPECT_FALSE(a.initialize(c, {0}).empty()); EXPECT_TRUE(a.groups().empty());
}

TEST(SourceClockGroups, SelectedRunoffMatchesNativeBatchAndLeavesPeerStatesUntouched) {
    auto c = context(); c.gages.rainfall[0] = 1; auto native = c;
    RunoffSolver a, b; a.init(c); b.init(native);
    std::vector<RunoffSourceForcing> rates{{0, 1 / ucf::UCF(ucf::RAINFALL, c.options), 0}};
    int old_model; double old_state[6]; a.infil_get_state(2, old_model, old_state);
    a.execute(c, 10, 0, 1, 1, 9, nullptr, &rates); b.execute(native, 10, 0, 1, 1, 9);
    EXPECT_DOUBLE_EQ(a.soa().depth_perv[0], b.soa().depth_perv[0]); EXPECT_DOUBLE_EQ(a.soa().infil_vol[0], b.soa().infil_vol[0]);
    EXPECT_GT(a.soa().infil_vol[0], 0); EXPECT_DOUBLE_EQ(c.subcatches.stat_infil_vol[2], 0);
    EXPECT_DOUBLE_EQ(c.subcatches.rainfall[2], 0); EXPECT_DOUBLE_EQ(a.soa().depth_perv[2], 0);
    int model; double state[6]; a.infil_get_state(2, model, state); EXPECT_EQ(model, old_model);
    for (int k = 0; k < 6; ++k) EXPECT_DOUBLE_EQ(state[k], old_state[k]);
    EXPECT_DOUBLE_EQ(c.gages.rainfall[0], 1);
}

TEST(SourceClockGroups, BadSelectedBatchesCannotPartiallyAdvanceSources) {
    auto c = context(); RunoffSolver r; r.init(c);
    for (auto rates : {std::vector<RunoffSourceForcing>{{0, .01, 0}, {0, .01, 0}},
                       std::vector<RunoffSourceForcing>{{0, .01, 0}, {3, .01, 0}},
                       std::vector<RunoffSourceForcing>{{0, .01, 0}, {1, -1, 0}}}) {
        EXPECT_THROW(r.execute(c, 1, 0, 1, 1, 9, nullptr, &rates), std::invalid_argument);
        EXPECT_DOUBLE_EQ(r.soa().depth_perv[0], 0); EXPECT_DOUBLE_EQ(c.subcatches.rainfall[0], 0);
    }
    std::vector<RunoffSourceForcing> rates{{0, .01, 0}};
    EXPECT_THROW(r.execute(c, 0, 0, 1, 1, 9, nullptr, &rates), std::invalid_argument);
    rates.clear(); r.execute(c, 1, 0, 1, 1, 9, nullptr, &rates); EXPECT_DOUBLE_EQ(c.subcatches.stat_infil_vol[0], 0);
}

TEST(SourceClockGroups, RealRunoffTrialsConserveRainStorageAndActualSurfaceEvaporation) {
    auto c = context(); rain(c, {0, 2}, {432, 0}, 2); c.subcatches.outlet_subcatch[0] = 1;
    c.forcing.subcatch_evap_mode[0] = ForcingMode::OVERRIDE; c.forcing.subcatch_evap_value[0] = .001;
    c.forcing.subcatch_evap_persist[0] = ForcingPersist::PERSIST;
    SourceClockGroups clocks; ASSERT_EQ(clocks.initialize(c, {0}), "");
    RunoffSolver r; r.init(c, {{0, 1}, {1, 1}});
    ASSERT_EQ(clocks.stage(0, 2, 2), ""); const auto initial = c.subcatches; auto trial = r;
    solve(c, trial, clocks.groups()[0], &sealed); auto proposed = c.subcatches; c.subcatches = initial;
    EXPECT_DOUBLE_EQ(r.soa().depth_perv[0], 0); EXPECT_DOUBLE_EQ(c.subcatches.stat_evap_vol[0], 0);
    clocks.cancel(0); ASSERT_EQ(clocks.stage(0, 2, 2), "");
    EXPECT_NEAR(trial.soa().depth_perv[0] * trial.soa().area[0] + proposed.stat_evap_vol[0],
                supply(clocks.groups()[0], 0) * trial.soa().area[0], 1e-15);
    c.subcatches = std::move(proposed); r = std::move(trial); ASSERT_EQ(clocks.commit(0), "");
    const double stored = r.soa().depth_perv[0]; ASSERT_EQ(clocks.stage(0, 3, 3), ""); solve(c, r, clocks.groups()[0], &sealed);
    EXPECT_NEAR((stored - r.soa().depth_perv[0]) * r.soa().area[0], r.soa().actual_perv_evap_vol[0], 1e-15);
    EXPECT_DOUBLE_EQ(r.soa().infil_vol[0], 0); EXPECT_DOUBLE_EQ(c.subcatches.stat_infil_vol[2], 0);
    ASSERT_EQ(clocks.commit(0), ""); EXPECT_DOUBLE_EQ(clocks.groups()[0].completed_end, 3);
}

TEST(SourceClockGroups, RealEngineFutureRunoffCursorCannotAdvanceCompletedSourceForcing) {
    const auto dir = std::filesystem::path(OPENSWMM_R4_CLOCK_OUT); std::filesystem::create_directories(dir);
    std::ofstream f(dir / "clock.inp");
    f << "[OPTIONS]\nFLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\nINFILTRATION GREEN_AMPT\nIGNORE_SNOWMELT YES\n"
         "START_DATE 10/05/2026\nEND_DATE 10/05/2026\nEND_TIME 00:10:00\nWET_STEP 00:05:00\nROUTING_STEP 1\n"
         "[JUNCTIONS]\nJ1 0 3\n[OUTFALLS]\nO1 -1 FREE NO\n[CONDUITS]\nC1 J1 O1 30 .013 0 0\n"
         "[XSECTIONS]\nC1 CIRCULAR .5 0 0 0 1\n[RAINGAGES]\nRG INTENSITY 0:05 1 TIMESERIES rain\n"
         "[TIMESERIES]\nrain 0:00 1\nrain 0:05 2\n[SUBCATCHMENTS]\nS0 RG J1 .01 0 1 0 0\n"
         "S1 RG J1 .01 0 1 0 0\n[SUBAREAS]\nS0 .01 .1 0 0 0 OUTLET\nS1 .01 .1 0 0 0 OUTLET\n"
         "[INFILTRATION]\nS0 3.5 .5 .26\nS1 3.5 .5 .26\n"; f.close();
    SWMM_Engine h = swmm_engine_create();
    struct Cleanup { SWMM_Engine h; ~Cleanup() { swmm_engine_close(h); swmm_engine_destroy(h); } } cleanup{h};
    ASSERT_EQ(swmm_engine_open(h, (dir / "clock.inp").c_str(), (dir / "clock.rpt").c_str(), (dir / "clock.out").c_str(), nullptr), SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(h), SWMM_OK); ASSERT_EQ(swmm_engine_start(h, 0), SWMM_OK);
    auto* engine = static_cast<SWMMEngine*>(h); ASSERT_EQ(engine->openRunoffIfaceWrite((dir / "clock.rnf").string()), 0);
    double elapsed = 0; ASSERT_EQ(swmm_engine_step(h, &elapsed), SWMM_OK); engine->closeRunoffIface();
    std::ifstream input(dir / "clock.rnf", std::ios::binary); input.seekg(28); float runoff_end = 0;
    input.read(reinterpret_cast<char*>(&runoff_end), sizeof(runoff_end)); ASSERT_TRUE(input.good());
    auto& c = engine->context(); SourceClockGroups clocks; ASSERT_EQ(clocks.initialize(c, {0}), "");
    EXPECT_GT(runoff_end, c.current_time); EXPECT_FALSE(clocks.stage(0, runoff_end, c.current_time).empty());
    ASSERT_EQ(clocks.stage(0, c.current_time, c.current_time), "");
    EXPECT_NEAR(supply(clocks.groups()[0], 0), c.current_time / ucf::UCF(ucf::RAINFALL, c.options), 1e-15);
    ASSERT_EQ(clocks.commit(0), "");
    std::ofstream audit(dir / "clock.json"); audit << std::setprecision(17)
        << "{\"routing_completed_seconds\":" << c.current_time << ",\"legacy_runoff_evaluated_through_seconds\":" << runoff_end
        << ",\"source_group_completed_seconds\":" << clocks.groups()[0].completed_end << ",\"production_caller\":false}\n";
}

TEST(SourceClockGroups, TimingRefinementConvergesWithoutAdvancingAnUnselectedControl) {
    struct Result { double storage, outflow, input; int calls; };
    const auto run = [](double step) {
        auto c = context(); rain(c, {0, 30}, {43.2, 0}, 30); c.subcatches.slope[0] = .01;
        SourceClockGroups clocks; EXPECT_EQ(clocks.initialize(c, {0}), "");
        RunoffSolver r; r.init(c, {{0, 1}}); Result result{};
        for (double end = step; end <= 60; end += step) {
            EXPECT_EQ(clocks.stage(0, end, end), "");
            result.input += supply(clocks.groups()[0], 0) * r.soa().area[0];
            for (const auto& f : clocks.groups()[0].pending) {
                r.execute(c, f.end - f.start, 0, f.infil_factor, f.recovery_factor, f.month, &sealed, &f.sources);
                result.outflow += r.soa().outflow_vol[0]; ++result.calls;
            }
            EXPECT_EQ(clocks.commit(0), "");
        }
        result.storage = r.soa().depth_perv[0] * r.soa().area[0];
        EXPECT_NEAR(result.input, result.storage + result.outflow, 1e-12);
        EXPECT_DOUBLE_EQ(r.soa().depth_perv[2], 0); EXPECT_DOUBLE_EQ(c.subcatches.stat_imperv_vol[2], 0);
        EXPECT_DOUBLE_EQ(c.subcatches.stat_infil_vol[2], 0); EXPECT_DOUBLE_EQ(c.subcatches.stat_evap_vol[2], 0);
        return result;
    };
    const auto reference = run(.25), coarse = run(10), medium = run(5), fine = run(2.5);
    const auto error = [](const auto& a, const auto& b) { return std::abs(a.storage - b.storage) + std::abs(a.outflow - b.outflow); };
    EXPECT_LT(error(fine, reference), error(medium, reference)); EXPECT_LT(error(medium, reference), error(coarse, reference));
    EXPECT_NEAR(coarse.input, fine.input, 1e-15); EXPECT_NEAR(reference.input, fine.input, 1e-15);
    const auto dir = std::filesystem::path(OPENSWMM_R4_CLOCK_OUT); std::filesystem::create_directories(dir);
    std::ofstream audit(dir / "refinement.json"); audit << std::setprecision(17)
        << "{\"scope\":\"one non-LID water kernel; no production mesh scheduler\",\"reference_step_seconds\":0.25,\"rows\":[";
    int k = 0; for (const auto& pair : {std::pair<double, Result>{10, coarse}, {5, medium}, {2.5, fine}}) {
        if (k++) audit << ',';
        audit << "{\"step_seconds\":" << pair.first << ",\"kernel_calls\":" << pair.second.calls
              << ",\"input_ft3\":" << pair.second.input << ",\"storage_ft3\":" << pair.second.storage
              << ",\"outflow_ft3\":" << pair.second.outflow
              << ",\"continuity_residual_ft3\":" << pair.second.input - pair.second.storage - pair.second.outflow
              << ",\"difference_from_reference_ft3\":" << error(pair.second, reference) << '}';
    }
    audit << "]}\n";
}

TEST(SourceClockGroups, StorageLidUsesItsGroupsCompletedRainProfile) {
    auto c = context(1); rain(c, {0, 2}, {43.2, 0}, 2);
    c.lid_controls.names = {"L1"}; c.lid_controls.lid_type = {"IT"};
    c.lid_controls.surface = {{{3, 0, .1, 0, 0}}}; c.lid_controls.storage = {{{12, .4, 0, 0}}};
    c.lid_usage.subcatch_index = {0}; c.lid_usage.lid_index = {0}; c.lid_usage.number = {2};
    c.lid_usage.area = {.25 / (.3048 * .3048)}; c.lid_usage.width = {1}; c.lid_usage.init_sat = {0};
    c.lid_usage.from_imperv = {0}; c.lid_usage.to_perv = {0};
    SourceClockGroups clocks; ASSERT_EQ(clocks.initialize(c, {0}), "");
    ASSERT_EQ(clocks.stage(0, 1, 1), ""); ASSERT_EQ(clocks.groups()[0].lid_usages, (std::vector<int>{0}));
    lid::LIDSolver r; r.init(c); auto& unit = r.group(static_cast<int>(lid::LIDType::INFIL_TRENCH));
    r.setNativeInfil({0}, {0});
    for (const auto& f : clocks.groups()[0].pending) {
        unit.inflow[0] = f.sources[0].rain; unit.subcatch_rain[0] = f.sources[0].rain;
        r.setRunoffTime(f.start); r.execute(c, f.end - f.start, 0, f.sources[0].pet);
    }
    const double storage = unit.surf_depth[0] * unit.surf_void_frac[0] + unit.stor_depth[0] * unit.stor_void[0];
    EXPECT_NEAR(storage + unit.wb_surf_flow[0] + unit.wb_drain_flow[0] + unit.wb_evap[0] + unit.wb_infil[0], supply(clocks.groups()[0], 0), 1e-12);
    EXPECT_DOUBLE_EQ(unit.wb_infil[0], 0); EXPECT_NEAR(unit.area[0] * .3048 * .3048, .5, 1e-15);
    ASSERT_EQ(clocks.commit(0), ""); EXPECT_DOUBLE_EQ(clocks.groups()[0].completed_end, 1);
}

TEST(SourceClockGroups, CompletedRunoffVolumeClosesForMixedSubareasAndInstantDrainage) {
    for (double manning : {0.0, .1}) {
        auto c = context(1); c.subcatches.frac_imperv[0] = .4; c.subcatches.pct_zero[0] = 50;
        c.subcatches.ds_imperv[0] = .1; c.subcatches.ds_perv[0] = .2;
        c.subcatches.slope[0] = .02; c.subcatches.n_perv[0] = manning; c.subcatches.n_imperv[0] = manning;
        RunoffSolver r; r.init(c, {{0, 1}}); double input = 0, output = 0;
        const double area = r.soa().area[0];
        for (const auto& interval : {std::pair<double, double>{2, .01}, {7, .02}, {3, 0}, {1, 0}}) {
            std::vector<RunoffSourceForcing> rates{{0, interval.second, .0001}};
            r.execute(c, interval.first, 0, 1, 1, 9, &sealed, &rates);
            input += interval.first * interval.second * area;
            output += r.soa().outflow_vol[0];
            const double storage = area * (r.soa().depth_imperv0[0] * .2 + r.soa().depth_imperv1[0] * .2 + r.soa().depth_perv[0] * .6);
            EXPECT_NEAR(input, storage + output + c.subcatches.stat_evap_vol[0], 1e-12);
        }
        EXPECT_GT(output, 0);
    }
}
