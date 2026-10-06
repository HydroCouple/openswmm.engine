// SPDX-License-Identifier: Apache-2.0
#include <cmath>
#include <limits>
#include <gtest/gtest.h>
#include <openswmm/engine/openswmm_trace.h>
#include <vector>

namespace
{
struct Fixture
{
    SWMM_Trace h = nullptr;
    std::vector<SWMM_TraceNodeAverage> na;
    std::vector<SWMM_TraceLinkAverage> la;
    std::vector<SWMM_TraceValue> nv, lv;
    SWMM_TraceSummary summary{};
    Fixture(std::initializer_list<SWMM_TraceNodeInput> nodes,
            std::initializer_list<SWMM_TraceLinkInput> links)
        : na(nodes.size()), la(links.size()), nv(nodes.size()), lv(links.size())
    {
        EXPECT_EQ(0, swmm_trace_create(nodes.begin(), int(nodes.size()), links.begin(),
                                       int(links.size()), nullptr, &h));
    }
    ~Fixture() { swmm_trace_close(h); }
    void flow(int i, double q, double velocity = 1)
    {
        la[i].net_flow_m3s = q;
        la[i].absolute_flow_m3s = std::abs(q);
        la[i].absolute_velocity_mps = velocity;
    }
    void run(int seed = 0, int dir = 0)
    {
        ASSERT_EQ(0, swmm_trace_set_averages(h, na.data(), int(na.size()), la.data(),
                                             int(la.size()), nullptr));
        ASSERT_EQ(0, swmm_trace_estimate(h, dir, seed, nv.data(), int(nv.size()), lv.data(),
                                         int(lv.size()), &summary, nullptr, nullptr))
            << swmm_trace_error(h);
    }
};
TEST(FlowTrace, SplitAndConfluence)
{
    Fixture f({{"S", 0, 0}, {"A", 0, 0}, {"B", 0, 0}, {"O", 1, 0}},
              {{"SA", 0, 1, 0, 10}, {"SB", 0, 2, 0, 20}, {"AO", 1, 3, 0, 10}, {"BO", 2, 3, 0, 20}});
    f.flow(0, 6);
    f.flow(1, 4);
    f.flow(2, 6);
    f.flow(3, 4);
    f.na[0].lateral_in_m3s = 10;
    f.run();
    EXPECT_NEAR(.6, f.lv[0].ratio, 1e-12);
    EXPECT_NEAR(.4, f.lv[1].ratio, 1e-12);
    EXPECT_NEAR(1, f.nv[3].ratio, 1e-12);
    EXPECT_NEAR(28, f.nv[3].time_s, 1e-12);
    EXPECT_NEAR(20, f.lv[2].to_time_s, 1e-12);
    EXPECT_NEAR(40, f.lv[3].to_time_s, 1e-12);
    EXPECT_NEAR(1, f.summary.terminal[SWMM_TRACE_OUTFALL], 1e-12);
    f.run(3, SWMM_TRACE_UPSTREAM);
    EXPECT_NEAR(1, f.summary.terminal[SWMM_TRACE_SOURCE], 1e-12);
    EXPECT_NEAR(28, f.nv[0].time_s, 1e-12);
}
TEST(FlowTrace, StorageDelayAndLocalSource)
{
    Fixture f({{"A", 0, 0}, {"Tank", 2, 0}, {"O", 1, 0}},
              {{"AT", 0, 1, 0, 10}, {"TO", 1, 2, 0, 20}});
    f.flow(0, 6);
    f.flow(1, 10);
    f.na[0].lateral_in_m3s = 6;
    f.na[1].lateral_in_m3s = 4;
    f.na[1].volume_m3 = 100;
    f.run();
    EXPECT_NEAR(40, f.nv[2].time_s, 1e-12);
    f.run(2, 1);
    EXPECT_NEAR(.6, f.nv[0].terminal_fraction, 1e-12);
    EXPECT_NEAR(.4, f.nv[1].terminal_fraction, 1e-12);
    EXPECT_NEAR(40, f.nv[0].time_s, 1e-12);
    EXPECT_NEAR(30, f.nv[1].time_s, 1e-12);
}
TEST(FlowTrace, LeakyCycleCountsRepeatPassages)
{
    Fixture f({{"S", 0, 0}, {"A", 0, 0}, {"O", 1, 0}},
              {{"SA", 0, 1, 0, 4}, {"AS", 1, 0, 0, 6}, {"AO", 1, 2, 0, 2}});
    f.flow(0, 2);
    f.flow(1, 1);
    f.flow(2, 1);
    f.na[0].lateral_in_m3s = 1;
    f.run();
    EXPECT_TRUE(f.summary.cyclic);
    EXPECT_NEAR(2, f.nv[0].ratio, 1e-10);
    EXPECT_NEAR(2, f.lv[0].ratio, 1e-10);
    EXPECT_NEAR(1, f.summary.terminal[SWMM_TRACE_OUTFALL], 1e-10);
    EXPECT_NEAR(16, f.nv[2].time_s, 1e-9);
    EXPECT_NEAR(10, f.nv[0].time_s, 1e-9);
}
TEST(FlowTrace, ClosedCycleTerminatesAsTrapped)
{
    Fixture f({{"S", 0, 0}, {"A", 0, 0}, {"B", 0, 0}},
              {{"SA", 0, 1, 0, 1}, {"AB", 1, 2, 0, 1}, {"BA", 2, 1, 0, 1}});
    f.flow(0, 1);
    f.flow(1, 1);
    f.flow(2, 1);
    f.run();
    EXPECT_NEAR(1, f.summary.terminal[SWMM_TRACE_CIRCULATION], 1e-12);
    EXPECT_TRUE(std::isnan(f.nv[1].ratio));
    EXPECT_TRUE(std::isnan(f.nv[1].time_s));
    EXPECT_TRUE(f.lv[1].flags & SWMM_TRACE_TRAPPED);
}
TEST(FlowTrace, UnknownPathRetainsFlowAndReportsCoverage)
{
    Fixture f({{"S", 0, 0}, {"O", 1, 0}}, {{"known", 0, 1, 0, 10}, {"unknown", 0, 1, 0, 10}});
    f.flow(0, 6, 1);
    f.flow(1, 4, 0);
    f.run();
    EXPECT_NEAR(1, f.nv[1].ratio, 1e-12);
    EXPECT_NEAR(.6, f.nv[1].time_coverage, 1e-12);
    EXPECT_NEAR(10, f.nv[1].time_s, 1e-12);
    EXPECT_TRUE(f.nv[1].flags & SWMM_TRACE_PARTIAL_TIME);
    EXPECT_TRUE(std::isnan(f.lv[1].time_s));
}
TEST(FlowTrace, ReversalUsesNetNotGrossWeight)
{
    Fixture f({{"S", 0, 0}, {"O1", 1, 0}, {"O2", 1, 0}},
              {{"reverse", 1, 0, 0, 10}, {"forward", 0, 2, 0, 10}});
    f.flow(0, -1);
    f.la[0].absolute_flow_m3s = 9;
    f.flow(1, 1);
    f.run();
    EXPECT_NEAR(.5, f.lv[0].ratio, 1e-12);
    EXPECT_NEAR(.5, f.lv[1].ratio, 1e-12);
    EXPECT_TRUE(f.lv[0].flags & SWMM_TRACE_REVERSAL);
    EXPECT_NEAR(10, f.lv[0].from_time_s, 1e-12);
    EXPECT_NEAR(0, f.lv[0].to_time_s, 1e-12);
}
TEST(FlowTrace, OverflowAndWithdrawalAbsorbSeparatelyFromLinks)
{
    Fixture f({{"S", 0, 0}, {"O", 1, 0}}, {{"SO", 0, 1, 0, 10}});
    f.flow(0, 6);
    f.na[0].withdrawal_m3s = 2;
    f.na[0].overflow_m3s = 2;
    f.run();
    EXPECT_NEAR(.6, f.lv[0].ratio, 1e-12);
    EXPECT_NEAR(.4, f.summary.terminal[SWMM_TRACE_LOSS], 1e-12);
}
TEST(FlowTrace, PondedOverflowIsNotIrreversibleLoss)
{
    Fixture f({{"S", 0, SWMM_TRACE_PONDING}, {"O", 1, 0}}, {{"SO", 0, 1, 0, 10}});
    f.flow(0, 6);
    f.na[0].overflow_m3s = 4;
    f.run();
    EXPECT_NEAR(1, f.lv[0].ratio, 1e-12);
    EXPECT_NEAR(0, f.summary.terminal[SWMM_TRACE_LOSS], 1e-12);
    EXPECT_TRUE(f.nv[0].flags & SWMM_TRACE_APPROXIMATE);
}
TEST(FlowTrace, ArbitraryDryNodeAndOutfall)
{
    Fixture f({{"dry", 0, 0}, {"out", 1, 0}}, {});
    f.run();
    EXPECT_NEAR(1, f.summary.terminal[SWMM_TRACE_RETAINED], 1e-12);
    EXPECT_TRUE(f.nv[1].flags & SWMM_TRACE_UNREACHABLE);
    f.run(1);
    EXPECT_NEAR(1, f.summary.terminal[SWMM_TRACE_OUTFALL], 1e-12);
    EXPECT_DOUBLE_EQ(0, f.nv[1].time_s);
}
TEST(FlowTrace, ValidationAndCancellation)
{
    Fixture f({{"S", 0, 0}, {"O", 1, 0}}, {{"SO", 0, 1, 0, 1}});
    f.flow(0, 1);
    f.run();
    EXPECT_EQ(SWMM_TRACE_INVALID, swmm_trace_estimate(f.h, 0, 8, f.nv.data(), 2, f.lv.data(), 1,
                                                      &f.summary, nullptr, nullptr));
    EXPECT_EQ(SWMM_TRACE_INVALID, swmm_trace_get_averages(f.h, f.na.data(), 1, nullptr, 0));
    auto cancel = [](double, const char *, void *) { return 1; };
    EXPECT_EQ(SWMM_TRACE_CANCELLED, swmm_trace_estimate(f.h, 0, 0, f.nv.data(), 2, f.lv.data(), 1,
                                                        &f.summary, cancel, nullptr));
}
} // namespace

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
namespace
{
// A small public-format .out fixture gives exact integrals at unequal report
// intervals and exercises ID remapping independently of simulation dynamics.
std::string writeOutput(const std::string &name, int units = 3, bool invalidTime = false,
                        int count = 3, int extra = 0, bool noLinks = false,
                        bool missingOverflow = false)
{
    const auto folder = std::filesystem::path(TRACE_TEST_OUTPUT);
    std::filesystem::create_directories(folder);
    auto path = (folder / name).string();
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    auto integer = [&](int32_t v) { file.write(reinterpret_cast<const char *>(&v), 4); };
    auto real = [&](float v) { file.write(reinterpret_cast<const char *>(&v), 4); };
    auto date = [&](double v) { file.write(reinterpret_cast<const char *>(&v), 8); };
    const int subcatchments = extra ? 1 : 0;
    for (auto n : {516114522, 60000, units, subcatchments, 2, noLinks ? 0 : 1, extra})
        integer(n);
    int idPosition = int(file.tellp());
    std::vector<std::string> ids;
    if (subcatchments)
        ids.push_back("C");
    ids.insert(ids.end(), {"O", "S"});
    if (!noLinks)
        ids.push_back("SO");
    for (int i = 0; i < extra; ++i)
        ids.push_back("P" + std::to_string(i));
    for (const std::string &id : ids)
    {
        integer(int(id.size()));
        file.write(id.data(), id.size());
    }
    int inputPosition = int(file.tellp());
    integer(1);
    integer(1);
    if (subcatchments)
        real(1);
    integer(3);
    for (int i = 0; i < 3; ++i)
        integer(i);
    for (int i = 0; i < 2; ++i)
    {
        integer(i == 0 ? 1 : 0);
        real(0);
        real(10);
    }
    integer(5);
    for (int i = 0; i < 5; ++i)
        integer(i);
    if (!noLinks)
    {
        integer(0);
        for (int i = 0; i < 4; ++i)
            real(1);
    }
    for (int vars : {subcatchments ? 8 + extra : 0, 6 + extra - int(missingOverflow), 5 + extra, 0})
    {
        integer(vars);
        for (int i = 0; i < vars; ++i)
            integer(i);
    }
    date(45000);
    integer(3600);
    int outputPosition = int(file.tellp());
    const double times[] = {1. / 24, 2. / 24, 4. / 24};
    const float flows[] = {-2, 2, 6}, volumes[] = {10, 20, 40};
    for (int i = 0; i < count; ++i)
    {
        date(45000 + (invalidTime ? times[0] : times[i]));
        if (subcatchments)
            for (int k = 0; k < 8 + extra; ++k)
                real(99);
        for (int node = 0; node < 2; ++node)
        {
            real(0);
            real(0);
            real(node ? volumes[i] : 0);
            real(node ? flows[i] : 0);
            real(std::abs(flows[i]));
            if (!missingOverflow)
                real(0);
            for (int k = 0; k < extra; ++k)
                real(std::numeric_limits<float>::quiet_NaN());
        }
        if (!noLinks)
        {
            real(flows[i]);
            real(0);
            real(flows[i]);
            real(volumes[i]);
            real(0);
            for (int k = 0; k < extra; ++k)
                real(std::numeric_limits<float>::quiet_NaN());
        }
    }
    for (auto n : {idPosition, inputPosition, outputPosition, count, 0, 516114522})
        integer(n);
    return path;
}
TEST(FlowTraceReports, BlockReadsSkipSubcatchmentsAndUnusedPollutantColumns)
{
    Fixture f({{"S", 0, 0}, {"O", 1, 0}}, {{"SO", 0, 1, 0, 30}});
    const auto baseline = writeOutput("block-baseline.out");
    ASSERT_EQ(0, swmm_trace_prepare(f.h, baseline.c_str(), nullptr, nullptr, nullptr, nullptr));
    ASSERT_EQ(0, swmm_trace_get_averages(f.h, f.na.data(), 2, f.la.data(), 1));
    const auto nodes = f.na;
    const auto links = f.la;
    const auto wide = writeOutput("block-pollutants.out", 3, false, 3, 4);
    ASSERT_EQ(0, swmm_trace_prepare(f.h, wide.c_str(), nullptr, nullptr, nullptr, nullptr));
    ASSERT_EQ(0, swmm_trace_get_averages(f.h, f.na.data(), 2, f.la.data(), 1));
    for (int i = 0; i < 2; ++i)
    {
        EXPECT_DOUBLE_EQ(nodes[i].volume_m3, f.na[i].volume_m3);
        EXPECT_DOUBLE_EQ(nodes[i].lateral_in_m3s, f.na[i].lateral_in_m3s);
        EXPECT_DOUBLE_EQ(nodes[i].withdrawal_m3s, f.na[i].withdrawal_m3s);
        EXPECT_DOUBLE_EQ(nodes[i].inflow_m3s, f.na[i].inflow_m3s);
        EXPECT_DOUBLE_EQ(nodes[i].overflow_m3s, f.na[i].overflow_m3s);
        EXPECT_EQ(nodes[i].flags, f.na[i].flags);
    }
    EXPECT_DOUBLE_EQ(links[0].net_flow_m3s, f.la[0].net_flow_m3s);
    EXPECT_DOUBLE_EQ(links[0].absolute_flow_m3s, f.la[0].absolute_flow_m3s);
    EXPECT_DOUBLE_EQ(links[0].absolute_velocity_mps, f.la[0].absolute_velocity_mps);
    EXPECT_DOUBLE_EQ(links[0].last_volume_m3, f.la[0].last_volume_m3);
    EXPECT_EQ(links[0].flags, f.la[0].flags);
    auto cancel = [](double progress, const char *, void *) { return int(progress > .05); };
    EXPECT_EQ(SWMM_TRACE_CANCELLED,
              swmm_trace_prepare(f.h, wide.c_str(), nullptr, nullptr, cancel, nullptr));
}
TEST(FlowTraceReports, BlockReadsHandleNoLinksAndRejectMissingHydraulicFields)
{
    Fixture isolated({{"S", 0, 0}, {"O", 1, 0}}, {});
    const auto noLinks = writeOutput("block-no-links.out", 3, false, 3, 0, true);
    ASSERT_EQ(0,
              swmm_trace_prepare(isolated.h, noLinks.c_str(), nullptr, nullptr, nullptr, nullptr));
    ASSERT_EQ(0, swmm_trace_get_averages(isolated.h, isolated.na.data(), 2, nullptr, 0));
    EXPECT_NEAR(25, isolated.na[0].volume_m3, 1e-8);
    Fixture f({{"S", 0, 0}, {"O", 1, 0}}, {{"SO", 0, 1, 0, 30}});
    const auto missing = writeOutput("block-missing-overflow.out", 3, false, 3, 0, false, true);
    EXPECT_EQ(SWMM_TRACE_IO,
              swmm_trace_prepare(f.h, missing.c_str(), nullptr, nullptr, nullptr, nullptr));
}
TEST(FlowTraceReports, UnequalIntervalsCrossingsUnitsAndCache)
{
    const std::string path = writeOutput("unequal.out");
    const auto cache = std::string(TRACE_TEST_OUTPUT) + "/unequal.trace.nc";
    std::filesystem::remove(cache);
    Fixture f({{"S", 0, 0}, {"O", 1, 0}}, {{"SO", 0, 1, 0, 30}});
    ASSERT_EQ(0, swmm_trace_prepare(f.h, path.c_str(), cache.c_str(), "fixture-digest-v1", nullptr,
                                    nullptr))
        << swmm_trace_error(f.h);
    ASSERT_EQ(0, swmm_trace_get_averages(f.h, f.na.data(), 2, f.la.data(), 1));
    SWMM_TraceInfo info{};
    swmm_trace_get_info(f.h, &info);
    EXPECT_NEAR(10800, info.duration_s, 1e-5);
    EXPECT_EQ(0, info.cache_loaded);
    EXPECT_NEAR(8. / 3, f.la[0].net_flow_m3s, 1e-8);
    EXPECT_NEAR(3, f.la[0].absolute_flow_m3s, 1e-8);
    EXPECT_NEAR(3, f.la[0].absolute_velocity_mps, 1e-8);
    EXPECT_NEAR(30600, f.la[0].forward_volume_m3, 1e-4);
    EXPECT_NEAR(1800, f.la[0].reverse_volume_m3, 1e-4);
    EXPECT_NEAR(25, f.na[0].volume_m3, 1e-6);
    EXPECT_NEAR(10, f.la[0].travel_s, 1e-7);
    ASSERT_EQ(0, swmm_trace_prepare(f.h, path.c_str(), cache.c_str(), "fixture-digest-v1", nullptr,
                                    nullptr));
    swmm_trace_get_info(f.h, &info);
    EXPECT_EQ(1, info.cache_loaded);
    ASSERT_EQ(0, swmm_trace_prepare(f.h, path.c_str(), cache.c_str(), "fixture-digest-v2", nullptr,
                                    nullptr));
    swmm_trace_get_info(f.h, &info);
    EXPECT_EQ(0, info.cache_loaded);
    Fixture changed({{"S", 0, 0}, {"O", 1, 0}}, {{"SO", 0, 1, 0, 60}});
    ASSERT_EQ(0, swmm_trace_prepare(changed.h, path.c_str(), cache.c_str(), "fixture-digest-v2",
                                    nullptr, nullptr));
    swmm_trace_get_info(changed.h, &info);
    EXPECT_EQ(0, info.cache_loaded);
    swmm_trace_get_averages(changed.h, changed.na.data(), 2, changed.la.data(), 1);
    EXPECT_NEAR(20, changed.la[0].travel_s, 1e-6);
    const auto us = writeOutput("us-units.out", 0);
    ASSERT_EQ(0, swmm_trace_prepare(f.h, us.c_str(), nullptr, nullptr, nullptr, nullptr));
    swmm_trace_get_averages(f.h, f.na.data(), 2, f.la.data(), 1);
    EXPECT_NEAR(3 * .028316846592, f.la[0].absolute_flow_m3s, 1e-9);
    EXPECT_NEAR(3 * .3048, f.la[0].absolute_velocity_mps, 1e-8);
    EXPECT_NEAR(25 * .028316846592, f.na[0].volume_m3, 1e-8);
}
TEST(FlowTraceReports, InvalidTimestampMissingIdsCancellationAndSnapshot)
{
    Fixture f({{"S", 0, 0}, {"O", 1, 0}}, {{"SO", 0, 1, 0, 30}});
    auto bad = writeOutput("bad-dates.out", 3, true);
    EXPECT_EQ(SWMM_TRACE_INVALID,
              swmm_trace_prepare(f.h, bad.c_str(), nullptr, nullptr, nullptr, nullptr));
    const auto good = writeOutput("valid.out");
    auto cancel = [](double, const char *, void *) { return 1; };
    EXPECT_EQ(SWMM_TRACE_CANCELLED,
              swmm_trace_prepare(f.h, good.c_str(), nullptr, nullptr, cancel, nullptr));
    Fixture wrong({{"missing", 0, 0}, {"O", 1, 0}}, {{"SO", 0, 1, 0, 30}});
    EXPECT_EQ(SWMM_TRACE_MISMATCH,
              swmm_trace_prepare(wrong.h, good.c_str(), nullptr, nullptr, nullptr, nullptr));
    const auto one = writeOutput("snapshot.out", 3, false, 1);
    ASSERT_EQ(0, swmm_trace_prepare(f.h, one.c_str(), nullptr, nullptr, nullptr, nullptr));
    SWMM_TraceInfo info{};
    swmm_trace_get_info(f.h, &info);
    EXPECT_EQ(0, info.duration_s);
    ASSERT_EQ(0, swmm_trace_estimate(f.h, 0, 0, f.nv.data(), 2, f.lv.data(), 1, &f.summary, nullptr,
                                     nullptr));
    EXPECT_TRUE(std::isnan(f.summary.partial_balance_residual_m3));
}
TEST(FlowTrace, OutfallBoundaryIsIncludedInPartialBudget)
{
    Fixture f({{"S", 0, 0}, {"O", 1, 0}}, {{"SO", 0, 1, 0, 10}});
    f.flow(0, 2);
    f.na[0].lateral_in_m3s = 2;
    SWMM_TraceInfo info{2, 2, 2, 1, 2, 3, 0, 45000, 45000 + 1. / 24, 3600};
    ASSERT_EQ(0, swmm_trace_set_averages(f.h, f.na.data(), 2, f.la.data(), 1, &info));
    ASSERT_EQ(0, swmm_trace_estimate(f.h, 0, 0, f.nv.data(), 2, f.lv.data(), 1, &f.summary, nullptr,
                                     nullptr));
    EXPECT_NEAR(7200, f.summary.boundary_out_m3, 1e-7);
    EXPECT_NEAR(0, f.summary.partial_balance_residual_m3, 1e-7);
}
} // namespace
namespace
{
TEST(FlowTraceReports, EveryFlowUnitAndCorruptCacheRecovery)
{
    const double factors[] = {.028316846592, .0000630901964,     .04381263638888889, 1,
                              .001,          .011574074074074073};
    Fixture f({{"S", 0, 0}, {"O", 1, 0}}, {{"SO", 0, 1, 0, 30}});
    for (int unit = 0; unit < 6; ++unit)
    {
        auto path = writeOutput("units-" + std::to_string(unit) + ".out", unit);
        ASSERT_EQ(0, swmm_trace_prepare(f.h, path.c_str(), nullptr, nullptr, nullptr, nullptr));
        swmm_trace_get_averages(f.h, f.na.data(), 2, f.la.data(), 1);
        EXPECT_NEAR(3 * factors[unit], f.la[0].absolute_flow_m3s, 1e-8);
    }
    const auto path = writeOutput("cache-recovery.out");
    const std::string cache = std::string(TRACE_TEST_OUTPUT) + "/corrupted.trace.nc";
    {
        std::ofstream broken(cache);
        broken << "incomplete cache";
    }
    ASSERT_EQ(0, swmm_trace_prepare(f.h, path.c_str(), cache.c_str(), "correct-digest", nullptr,
                                    nullptr));
    SWMM_TraceInfo info{};
    swmm_trace_get_info(f.h, &info);
    EXPECT_EQ(info.cache_loaded, 0);
    auto cancel = [](double, const char *, void *) { return 1; };
    EXPECT_EQ(SWMM_TRACE_CANCELLED, swmm_trace_prepare(f.h, path.c_str(), cache.c_str(),
                                                       "correct-digest", cancel, nullptr));
    ASSERT_EQ(0, swmm_trace_prepare(f.h, path.c_str(), cache.c_str(), "correct-digest", nullptr,
                                    nullptr));
    swmm_trace_get_info(f.h, &info);
    EXPECT_EQ(info.cache_loaded, 1);
}
TEST(FlowTrace, UnreachableCyclesAndUnresolvedBoundaries)
{
    Fixture f({{"S", 0, 0}, {"O", 1, SWMM_TRACE_ROUTED_OUTFALL}, {"A", 0, 0}, {"B", 0, 0}},
              {{"SO", 0, 1, 0, 1}, {"AB", 2, 3, 0, 1}, {"BA", 3, 2, 0, 1}});
    f.flow(0, 1);
    f.flow(1, 1);
    f.flow(2, 1);
    f.run();
    EXPECT_FALSE(f.summary.cyclic);
    EXPECT_NEAR(1, f.summary.terminal[SWMM_TRACE_UNRESOLVED], 1e-12);
    EXPECT_TRUE(f.nv[1].flags & SWMM_TRACE_APPROXIMATE);
}
} // namespace

#include <chrono>
#include <iostream>
namespace
{
TEST(FlowTraceScale, TenThousandLinkWarmSolve)
{
    constexpr int count = 10000;
    std::vector<std::string> nodeIds, linkIds;
    nodeIds.reserve(count + 1);
    linkIds.reserve(count);
    for (int i = 0; i <= count; ++i)
        nodeIds.push_back("N" + std::to_string(i));
    for (int i = 0; i < count; ++i)
        linkIds.push_back("L" + std::to_string(i));
    std::vector<SWMM_TraceNodeInput> nodes;
    std::vector<SWMM_TraceLinkInput> links;
    for (int i = 0; i <= count; ++i)
        nodes.push_back({nodeIds[i].c_str(), i == count ? 1 : 0, 0});
    for (int i = 0; i < count; ++i)
        links.push_back({linkIds[i].c_str(), i, i + 1, 0, 10});
    SWMM_Trace handle = nullptr;
    ASSERT_EQ(0, swmm_trace_create(nodes.data(), nodes.size(), links.data(), links.size(), nullptr,
                                   &handle));
    std::vector<SWMM_TraceNodeAverage> na(count + 1);
    std::vector<SWMM_TraceLinkAverage> la(count);
    na[0].lateral_in_m3s = 1;
    for (auto &v : la)
    {
        v.net_flow_m3s = v.absolute_flow_m3s = v.absolute_velocity_mps = 1;
    }
    ASSERT_EQ(0,
              swmm_trace_set_averages(handle, na.data(), na.size(), la.data(), la.size(), nullptr));
    std::vector<SWMM_TraceValue> nv(count + 1), lv(count);
    SWMM_TraceSummary summary{};
    auto start = std::chrono::steady_clock::now();
    int rc = swmm_trace_estimate(handle, 0, 0, nv.data(), nv.size(), lv.data(), lv.size(), &summary,
                                 nullptr, nullptr);
    double milliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    EXPECT_EQ(0, rc) << swmm_trace_error(handle);
    EXPECT_NEAR(100000, nv.back().time_s, 1e-7);
    EXPECT_NEAR(1, summary.terminal[SWMM_TRACE_OUTFALL], 1e-12);
    std::cout << "10,000-link warm trace: " << milliseconds << " ms\n";
    swmm_trace_close(handle);
}
} // namespace
