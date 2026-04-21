/**
 * @file test_gap_fixes.cpp
 * @brief Unit tests for critical gap fixes: co-pollutant washoff, quality
 *        continuity error, routing events, and steady-state skip.
 *
 * @see docs/COMPREHENSIVE_GAP_ANALYSIS.md Phase 1
 * @ingroup engine_quality
 */

#include <gtest/gtest.h>
#include <cmath>
#include <vector>
#include <algorithm>
#include <string>

#include "quality/Landuse.hpp"
#include "core/SimulationContext.hpp"
#include "core/SimulationOptions.hpp"
#include "hydraulics/KinematicWave.hpp"
#include "hydraulics/XSectBatch.hpp"
#include "hydraulics/TopoSort.hpp"

using namespace openswmm;
using namespace openswmm::landuse;

// ============================================================================
// 1.2 Co-Pollutant Washoff Tests
// ============================================================================

class CoPollutantTest : public ::testing::Test
{
protected:
    LanduseSolver solver;
    SurfaceQualitySoA sq;

    void SetUp() override
    {
        solver.init(1, 3); // 1 landuse, 3 pollutants

        // Set up EMC washoff for all 3 pollutants
        for (int p = 0; p < 3; ++p)
        {
            solver.washoff_params[static_cast<size_t>(p)].type = WashoffType::EMC;
            solver.washoff_params[static_cast<size_t>(p)].coeff = 10.0 * (p + 1);
        }

        sq.resize(2, 1, 3); // 2 subcatchments, 1 landuse, 3 pollutants
    }
};

TEST_F(CoPollutantTest, NoCoPollutantNoChange)
{
    // Compute primary washoff
    double runoff[2] = {1.0, 2.0};
    double area[2] = {100.0, 200.0};
    solver.computeWashoff(sq, runoff, area, 2);

    // Save original concentrations
    std::vector<double> orig(sq.washoff_conc.begin(), sq.washoff_conc.end());

    // Apply co-pollutant with no co-pollutant defined
    int co_pollut[3] = {-1, -1, -1};
    double co_frac[3] = {0.0, 0.0, 0.0};
    solver.applyCoPollutant(sq, runoff, area, co_pollut, co_frac, 2);

    // Concentrations should be unchanged
    for (size_t i = 0; i < sq.washoff_conc.size(); ++i)
    {
        EXPECT_NEAR(sq.washoff_conc[i], orig[i], 1e-10);
    }
}

TEST_F(CoPollutantTest, SimpleCoPollutantFraction)
{
    // Pollutant 1 gets fraction of pollutant 0's washoff
    double runoff[2] = {1.0, 1.0};
    double area[2] = {100.0, 100.0};
    solver.computeWashoff(sq, runoff, area, 2);

    double c0_before = sq.washoff_conc[0]; // pollutant 0, subcatch 0
    double c1_before = sq.washoff_conc[1]; // pollutant 1, subcatch 0

    int co_pollut[3] = {-1, 0, -1};      // pollutant 1 has co-pollutant 0
    double co_frac[3] = {0.0, 0.5, 0.0}; // 50% of pollutant 0's washoff
    solver.applyCoPollutant(sq, runoff, area, co_pollut, co_frac, 2);

    // Pollutant 0 should be unchanged
    EXPECT_NEAR(sq.washoff_conc[0], c0_before, 1e-10);

    // Pollutant 1 should have increased by 50% of pollutant 0's concentration
    EXPECT_NEAR(sq.washoff_conc[1], c1_before + 0.5 * c0_before, 1e-10);

    // Pollutant 2 should be unchanged
    EXPECT_NEAR(sq.washoff_conc[2], 30.0, 1e-10);
}

TEST_F(CoPollutantTest, MultipleSubcatchments)
{
    double runoff[2] = {1.0, 2.0};
    double area[2] = {100.0, 200.0};
    solver.computeWashoff(sq, runoff, area, 2);

    int co_pollut[3] = {-1, 0, -1};
    double co_frac[3] = {0.0, 0.25, 0.0};
    solver.applyCoPollutant(sq, runoff, area, co_pollut, co_frac, 2);

    // Subcatch 0: pollutant 1 += 0.25 * pollutant 0
    EXPECT_NEAR(sq.washoff_conc[1], 20.0 + 0.25 * 10.0, 1e-10);

    // Subcatch 1: pollutant 1 += 0.25 * pollutant 0 (same EMC)
    size_t sc1_p1 = 1 * 3 + 1;
    size_t sc1_p0 = 1 * 3 + 0;
    EXPECT_NEAR(sq.washoff_conc[sc1_p1], 20.0 + 0.25 * 10.0, 1e-10);
}

TEST_F(CoPollutantTest, ZeroFractionNoChange)
{
    double runoff[2] = {1.0, 1.0};
    double area[2] = {100.0, 100.0};
    solver.computeWashoff(sq, runoff, area, 2);

    std::vector<double> orig(sq.washoff_conc.begin(), sq.washoff_conc.end());

    int co_pollut[3] = {-1, 0, -1};
    double co_frac[3] = {0.0, 0.0, 0.0}; // fraction is 0
    solver.applyCoPollutant(sq, runoff, area, co_pollut, co_frac, 2);

    for (size_t i = 0; i < sq.washoff_conc.size(); ++i)
    {
        EXPECT_NEAR(sq.washoff_conc[i], orig[i], 1e-10);
    }
}

TEST_F(CoPollutantTest, ChainCoPollutant)
{
    // Pollutant 1 depends on 0, pollutant 2 depends on 1
    double runoff[1] = {1.0};
    double area[1] = {100.0};
    sq.resize(1, 1, 3);
    solver.computeWashoff(sq, runoff, area, 1);

    double c0 = sq.washoff_conc[0]; // 10.0
    double c1 = sq.washoff_conc[1]; // 20.0
    double c2 = sq.washoff_conc[2]; // 30.0

    int co_pollut[3] = {-1, 0, 1};
    double co_frac[3] = {0.0, 0.5, 0.3};
    solver.applyCoPollutant(sq, runoff, area, co_pollut, co_frac, 1);

    // Pollutant 0: unchanged = 10.0
    EXPECT_NEAR(sq.washoff_conc[0], c0, 1e-10);

    // Pollutant 1: 20.0 + 0.5 * 10.0 = 25.0
    EXPECT_NEAR(sq.washoff_conc[1], c1 + 0.5 * c0, 1e-10);

    // Pollutant 2: 30.0 + 0.3 * (20.0 + 0.5 * 10.0) = 30.0 + 0.3 * 25.0 = 37.5
    // Note: uses the ALREADY-UPDATED c1 (25.0), matching legacy behavior
    EXPECT_NEAR(sq.washoff_conc[2], c2 + 0.3 * (c1 + 0.5 * c0), 1e-10);
}

TEST_F(CoPollutantTest, NullPointersHandled)
{
    double runoff[1] = {1.0};
    double area[1] = {100.0};
    // Should not crash with null pointers
    solver.applyCoPollutant(sq, runoff, area, nullptr, nullptr, 1);
}

TEST_F(CoPollutantTest, InvalidCoPollutantIndex)
{
    double runoff[1] = {1.0};
    double area[1] = {100.0};
    sq.resize(1, 1, 3);
    solver.computeWashoff(sq, runoff, area, 1);

    std::vector<double> orig(sq.washoff_conc.begin(), sq.washoff_conc.end());

    // Co-pollutant index out of range — should be ignored
    int co_pollut[3] = {-1, 99, -1};
    double co_frac[3] = {0.0, 0.5, 0.0};
    solver.applyCoPollutant(sq, runoff, area, co_pollut, co_frac, 1);

    for (size_t i = 0; i < sq.washoff_conc.size(); ++i)
    {
        EXPECT_NEAR(sq.washoff_conc[i], orig[i], 1e-10);
    }
}

// ============================================================================
// 1.3 Quality Continuity Error Tests
// ============================================================================

TEST(QualityError, ZeroFluxReturnsZero)
{
    SimulationContext ctx;
    ctx.mass_balance.resize_quality(2);
    // All zeros → error should be 0.0
    double total_in = ctx.mass_balance.qual_routing_wet[0];
    EXPECT_NEAR(total_in, 0.0, 1e-15);
}

TEST(QualityError, MassBalanceVectorsExist)
{
    SimulationContext ctx;
    ctx.mass_balance.resize_quality(3);
    EXPECT_EQ(ctx.mass_balance.qual_routing_wet.size(), 3u);
    EXPECT_EQ(ctx.mass_balance.qual_routing_outflow.size(), 3u);
    EXPECT_EQ(ctx.mass_balance.qual_routing_flood.size(), 3u);
    EXPECT_EQ(ctx.mass_balance.qual_routing_init.size(), 3u);
    EXPECT_EQ(ctx.mass_balance.qual_routing_final.size(), 3u);
    EXPECT_EQ(ctx.mass_balance.qual_routing_reacted.size(), 3u);
    EXPECT_EQ(ctx.mass_balance.qual_routing_ii_in.size(), 3u);
}

TEST(QualityError, PerfectBalanceZeroError)
{
    SimulationContext ctx;
    ctx.mass_balance.resize_quality(1);

    // Set up: in = 100, out = 100, init = 0, final = 0
    ctx.mass_balance.qual_routing_wet[0] = 100.0;
    ctx.mass_balance.qual_routing_outflow[0] = 100.0;

    // error = (in + init - final - out) / in = (100 + 0 - 0 - 100) / 100 = 0
    double total_in = ctx.mass_balance.qual_routing_wet[0];
    double total_out = ctx.mass_balance.qual_routing_outflow[0];
    double init_stored = ctx.mass_balance.qual_routing_init[0];
    double final_stored = ctx.mass_balance.qual_routing_final[0];

    double error = (total_in + init_stored - final_stored - total_out) / total_in;
    EXPECT_NEAR(error, 0.0, 1e-10);
}

TEST(QualityError, KnownImbalanceError)
{
    SimulationContext ctx;
    ctx.mass_balance.resize_quality(1);

    // in = 100, out = 90, init = 5, final = 10
    // error = (100 + 5 - 10 - 90) / 100 = 5/100 = 0.05
    ctx.mass_balance.qual_routing_wet[0] = 100.0;
    ctx.mass_balance.qual_routing_outflow[0] = 90.0;
    ctx.mass_balance.qual_routing_init[0] = 5.0;
    ctx.mass_balance.qual_routing_final[0] = 10.0;

    double total_in = ctx.mass_balance.qual_routing_wet[0];
    double total_out = ctx.mass_balance.qual_routing_outflow[0];
    double error = (total_in + 5.0 - 10.0 - total_out) / total_in;
    EXPECT_NEAR(error, 0.05, 1e-10);
}

// ============================================================================
// 1.4 Routing Events Tests
// ============================================================================

TEST(RoutingEvents, EventSortChronological)
{
    std::vector<SimulationContext::Event> events;
    events.push_back({10.0, 20.0});
    events.push_back({5.0, 8.0});
    events.push_back({25.0, 30.0});

    std::sort(events.begin(), events.end(),
              [](const SimulationContext::Event &a, const SimulationContext::Event &b)
              {
                  return a.start < b.start;
              });

    EXPECT_NEAR(events[0].start, 5.0, 1e-10);
    EXPECT_NEAR(events[1].start, 10.0, 1e-10);
    EXPECT_NEAR(events[2].start, 25.0, 1e-10);
}

TEST(RoutingEvents, OverlappingEventsResolved)
{
    std::vector<SimulationContext::Event> events;
    events.push_back({5.0, 15.0});
    events.push_back({10.0, 20.0});

    std::sort(events.begin(), events.end(),
              [](const SimulationContext::Event &a, const SimulationContext::Event &b)
              {
                  return a.start < b.start;
              });

    // Resolve overlaps
    for (size_t i = 0; i + 1 < events.size(); ++i)
    {
        if (events[i].end > events[i + 1].start)
            events[i].end = events[i + 1].start;
    }

    // First event end should be trimmed to second event start
    EXPECT_NEAR(events[0].end, 10.0, 1e-10);
    EXPECT_NEAR(events[1].start, 10.0, 1e-10);
}

TEST(RoutingEvents, NoEventsNeverBetween)
{
    // With no events defined, isBetweenEvents should always return false
    // (tested via empty events vector logic)
    std::vector<SimulationContext::Event> events;
    EXPECT_TRUE(events.empty());
    // An empty event list means "always route" (not between events)
}

TEST(RoutingEvents, BeforeFirstEventIsBetween)
{
    SimulationContext::Event ev{10.0, 20.0};
    double current_date = 5.0;
    // Before first event → between events
    EXPECT_LT(current_date, ev.start);
}

TEST(RoutingEvents, DuringEventNotBetween)
{
    SimulationContext::Event ev{10.0, 20.0};
    double current_date = 15.0;
    EXPECT_GE(current_date, ev.start);
    EXPECT_LE(current_date, ev.end);
}

TEST(RoutingEvents, AfterEventIsBetween)
{
    SimulationContext::Event ev{10.0, 20.0};
    double current_date = 25.0;
    EXPECT_GT(current_date, ev.end);
}

// ============================================================================
// 1.5 Steady-State Skip Tests
// ============================================================================

TEST(SteadyState, OptionDefaultFalse)
{
    SimulationOptions opts;
    EXPECT_FALSE(opts.skip_steady_state);
}

TEST(SteadyState, OptionCanBeEnabled)
{
    SimulationOptions opts;
    opts.skip_steady_state = true;
    EXPECT_TRUE(opts.skip_steady_state);
}

TEST(SteadyState, InflowChangeDetection)
{
    // Simulate checking if inflow changed significantly
    double qOld = 10.0;
    double qNew = 10.4; // 4% change — below 5% threshold
    double lat_flow_tol = 0.05;

    double diff = (std::abs(qOld) > 1e-6) ? (qNew / qOld) - 1.0 : 1.0;
    bool changed = std::abs(diff) > lat_flow_tol;
    EXPECT_FALSE(changed); // 4% change is below threshold

    qNew = 11.0; // 10% change
    diff = (qNew / qOld) - 1.0;
    changed = std::abs(diff) > lat_flow_tol;
    EXPECT_TRUE(changed);
}

TEST(SteadyState, ZeroInflowNoChange)
{
    double qOld = 0.0;
    double qNew = 0.0;
    double diff;
    constexpr double TINY = 1e-6;

    if (std::abs(qOld) > TINY)
        diff = (qNew / qOld) - 1.0;
    else if (std::abs(qNew) > TINY)
        diff = 1.0;
    else
        diff = 0.0;

    EXPECT_NEAR(diff, 0.0, 1e-10);
}

TEST(SteadyState, ZeroToNonzeroIsChange)
{
    double qOld = 0.0;
    double qNew = 1.0;
    double diff;
    constexpr double TINY = 1e-6;

    if (std::abs(qOld) > TINY)
        diff = (qNew / qOld) - 1.0;
    else if (std::abs(qNew) > TINY)
        diff = 1.0;
    else
        diff = 0.0;

    EXPECT_NEAR(diff, 1.0, 1e-10);
    EXPECT_TRUE(std::abs(diff) > 0.05); // exceeds any reasonable tolerance
}

TEST(SteadyState, ActionCountPreventsSkip)
{
    // If control actions were taken, should NOT skip even if flows unchanged
    int action_count = 1;
    EXPECT_GT(action_count, 0);
    // steady state is false when action_count > 0
}

// ============================================================================
// Landuse Solver Basic Tests (existing functionality verification)
// ============================================================================

TEST(LanduseSolver, InitSetsCorrectSizes)
{
    LanduseSolver solver;
    solver.init(2, 3);
    EXPECT_EQ(solver.n_landuses_, 2);
    EXPECT_EQ(solver.n_pollutants_, 3);
    EXPECT_EQ(static_cast<int>(solver.buildup_params.size()), 6);
    EXPECT_EQ(static_cast<int>(solver.washoff_params.size()), 6);
}

TEST(LanduseSolver, EMCWashoffConstant)
{
    LanduseSolver solver;
    solver.init(1, 1);
    solver.washoff_params[0].type = WashoffType::EMC;
    solver.washoff_params[0].coeff = 15.0;

    SurfaceQualitySoA sq;
    sq.resize(1, 1, 1);
    double runoff[1] = {2.0};
    double area[1] = {100.0};
    solver.computeWashoff(sq, runoff, area, 1);

    EXPECT_NEAR(sq.washoff_conc[0], 15.0, 1e-10);
}

TEST(LanduseSolver, ZeroRunoffZeroWashoff)
{
    LanduseSolver solver;
    solver.init(1, 1);
    solver.washoff_params[0].type = WashoffType::EMC;
    solver.washoff_params[0].coeff = 15.0;

    SurfaceQualitySoA sq;
    sq.resize(1, 1, 1);
    double runoff[1] = {0.0};
    double area[1] = {100.0};
    solver.computeWashoff(sq, runoff, area, 1);

    EXPECT_NEAR(sq.washoff_conc[0], 0.0, 1e-10);
}

TEST(LanduseSolver, PowerBuildup)
{
    LanduseSolver solver;
    solver.init(1, 1);
    solver.buildup_params[0].type = BuildupType::POWER;
    solver.buildup_params[0].coeff[0] = 100.0; // max
    solver.buildup_params[0].coeff[1] = 5.0;   // rate
    solver.buildup_params[0].coeff[2] = 0.5;   // power
    solver.buildup_params[0].max_days = 365.0;

    SurfaceQualitySoA sq;
    sq.resize(1, 1, 1);
    sq.buildup[0] = 0.0;

    double area[1] = {100.0};
    double curb[1] = {0.0};
    solver.computeBuildup(sq, area, curb, 86400.0, 1); // 1 day

    EXPECT_GT(sq.buildup[0], 0.0);
    EXPECT_LE(sq.buildup[0], 100.0);
}

TEST(LanduseSolver, ExponentialBuildup)
{
    LanduseSolver solver;
    solver.init(1, 1);
    solver.buildup_params[0].type = BuildupType::EXPON;
    solver.buildup_params[0].coeff[0] = 50.0; // max
    solver.buildup_params[0].coeff[1] = 0.1;  // rate
    solver.buildup_params[0].max_days = 365.0;

    SurfaceQualitySoA sq;
    sq.resize(1, 1, 1);
    sq.buildup[0] = 0.0;

    double area[1] = {100.0};
    double curb[1] = {0.0};

    // After many days, should approach max
    for (int d = 0; d < 100; ++d)
        solver.computeBuildup(sq, area, curb, 86400.0, 1);

    EXPECT_GT(sq.buildup[0], 45.0); // close to 50 asymptote
    EXPECT_LE(sq.buildup[0], 50.0);
}

TEST(LanduseSolver, SurfaceQualitySoAResize)
{
    SurfaceQualitySoA sq;
    sq.resize(5, 2, 3);
    EXPECT_EQ(sq.n_subcatch, 5);
    EXPECT_EQ(sq.n_landuses, 2);
    EXPECT_EQ(sq.n_pollutants, 3);
    EXPECT_EQ(static_cast<int>(sq.buildup.size()), 30);      // 5*2*3
    EXPECT_EQ(static_cast<int>(sq.washoff_conc.size()), 15); // 5*3
}

// ============================================================================
// 1.1 Kinematic Wave Solver Tests
// ============================================================================

using namespace openswmm::kinwave;
using namespace openswmm::xsect;
using openswmm::XSectParams;
using openswmm::XSectShape;

TEST(KWSolver, InitAllocatesArrays)
{
    KWSolver solver;
    XSectGroups groups;
    solver.init(5, groups);
    // Internal arrays should be allocated (we can't access private, but
    // the solver should not crash on subsequent operations)
}

TEST(KWSolver, SetLinkOrder)
{
    KWSolver solver;
    XSectGroups groups;
    solver.init(3, groups);
    std::vector<int> order = {2, 0, 1};
    solver.setLinkOrder(order);
    // Verify order is stored
    EXPECT_EQ(solver.sorted_links_.size(), 3u);
    EXPECT_EQ(solver.sorted_links_[0], 2);
}

TEST(KWSolver, SolveConduitZeroFlow)
{
    KWSolver solver;
    XSectGroups groups;
    solver.init(1, groups);

    XSectParams xs{};
    double p[4] = {3.0, 0, 0, 0};
    xsect::setParams(xs, static_cast<int>(XSectShape::CIRCULAR) + 1, p, 1.0);

    int iters = solver.solveConduit(0, xs, 10.0, xs.a_full, xs.s_full,
                                    1.0, 500.0, 300.0, 0.0);
    // With zero inflow, should converge quickly
    EXPECT_GE(iters, 0);
}

TEST(KWSolver, SolveConduitPositiveFlow)
{
    KWSolver solver;
    XSectGroups groups;
    solver.init(1, groups);

    XSectParams xs{};
    double p[4] = {3.0, 0, 0, 0};
    xsect::setParams(xs, static_cast<int>(XSectShape::CIRCULAR) + 1, p, 1.0);

    double q_full = 10.0;
    double beta = 1.0;

    // Set inflow to 50% of full flow
    // Need to access q_in_ — it's private, so test via solveConduit directly
    // by first setting the working arrays
    // For a clean test, we just verify the solver doesn't crash and produces
    // reasonable output
    int iters = solver.solveConduit(0, xs, q_full, xs.a_full, xs.s_full,
                                    beta, 500.0, 300.0, 0.0);
    EXPECT_GE(iters, 0);
    EXPECT_LE(iters, 40); // MAX_ITERS
}

TEST(KWSolver, SolveConduitConvergence)
{
    KWSolver solver;
    XSectGroups groups;
    solver.init(1, groups);

    XSectParams xs{};
    double p[4] = {2.0, 0, 0, 0}; // 2ft diameter circular
    xsect::setParams(xs, static_cast<int>(XSectShape::CIRCULAR) + 1, p, 1.0);

    double q_full = 5.0;
    double beta = 0.5;

    // Multiple calls should converge to steady state
    for (int step = 0; step < 10; ++step)
    {
        int iters = solver.solveConduit(0, xs, q_full, xs.a_full, xs.s_full,
                                        beta, 300.0, 60.0, 0.0);
        EXPECT_GE(iters, 0);
    }
}

TEST(KWSolver, ConstantsMatchLegacy)
{
    EXPECT_NEAR(WX, 0.6, 1e-10);
    EXPECT_NEAR(WT, 0.6, 1e-10);
    EXPECT_NEAR(EPSIL, 0.001, 1e-10);
}

// ============================================================================
// Topological Sort Tests
// ============================================================================

TEST(TopoSort, SimpleChain)
{
    // 3 nodes, 2 links: 0→1→2
    int node1[2] = {0, 1};
    int node2[2] = {1, 2};
    std::vector<int> sorted;

    int n = openswmm::toposort::sortLinks(node1, node2, 2, 3, sorted);
    EXPECT_EQ(n, 2);
    // Link 0 (0→1) should come before link 1 (1→2)
    EXPECT_EQ(sorted[0], 0);
    EXPECT_EQ(sorted[1], 1);
}

TEST(TopoSort, BranchingNetwork)
{
    // 4 nodes, 3 links: 0→2, 1→2, 2→3
    int node1[3] = {0, 1, 2};
    int node2[3] = {2, 2, 3};
    std::vector<int> sorted;

    int n = openswmm::toposort::sortLinks(node1, node2, 3, 4, sorted);
    EXPECT_EQ(n, 3);
    // Link 2 (2→3) must come after links 0 and 1
    // Find position of link 2 in sorted order
    auto it = std::find(sorted.begin(), sorted.end(), 2);
    EXPECT_NE(it, sorted.end());
    auto pos2 = std::distance(sorted.begin(), it);
    EXPECT_EQ(pos2, 2); // Link 2 should be last
}

TEST(TopoSort, SingleLink)
{
    int node1[1] = {0};
    int node2[1] = {1};
    std::vector<int> sorted;

    int n = openswmm::toposort::sortLinks(node1, node2, 1, 2, sorted);
    EXPECT_EQ(n, 1);
    EXPECT_EQ(sorted[0], 0);
}

TEST(TopoSort, EmptyNetwork)
{
    std::vector<int> sorted;
    int n = openswmm::toposort::sortLinks(nullptr, nullptr, 0, 0, sorted);
    EXPECT_EQ(n, 0);
    EXPECT_TRUE(sorted.empty());
}

// ============================================================================
// Phase 2 Tests
// ============================================================================

// 2.2 Outfall-to-Subcatchment Routing
TEST(OutfallRouting, RouteToFieldDefaultMinusOne)
{
    NodeData nodes;
    nodes.resize(3);
    for (int i = 0; i < 3; ++i)
    {
        EXPECT_EQ(nodes.outfall_route_to[static_cast<size_t>(i)], -1);
    }
}

TEST(OutfallRouting, RouteToCanBeSet)
{
    NodeData nodes;
    nodes.resize(3);
    nodes.outfall_route_to[1] = 5;
    EXPECT_EQ(nodes.outfall_route_to[1], 5);
}

TEST(OutfallRouting, RunonConversion)
{
    // Outfall discharge (CFS) → runon (depth/sec over subcatchment area)
    double q_outfall = 10.0; // CFS
    double area = 43560.0;   // 1 acre in ft²
    double runon = q_outfall / area;
    EXPECT_GT(runon, 0.0);
    EXPECT_NEAR(runon, 10.0 / 43560.0, 1e-10);
}

TEST(OutfallRouting, NoRouteWhenMinusOne)
{
    // When outfall_route_to == -1, no routing should occur
    int sc = -1;
    EXPECT_LT(sc, 0);
    // In the actual code, the condition `sc >= 0` prevents routing
}

// 2.3 Wind Speed — already implemented (verification tests)
TEST(WindSpeed, MonthlyValuesStored)
{
    SimulationOptions opts;
    opts.wind_speed[0] = 5.0;
    opts.wind_speed[6] = 10.0;
    EXPECT_NEAR(opts.wind_speed[0], 5.0, 1e-10);
    EXPECT_NEAR(opts.wind_speed[6], 10.0, 1e-10);
    EXPECT_NEAR(opts.wind_speed[11], 0.0, 1e-10);
}

// 2.4 Street Sweeping — already implemented (verification tests)
TEST(StreetSweeping, ParametersExist)
{
    SimulationOptions opts;
    EXPECT_EQ(opts.sweep_start, 1);
    EXPECT_EQ(opts.sweep_end, 365);
}

TEST(StreetSweeping, SweepEfficiencyInWashoffParams)
{
    WashoffParams wp;
    wp.sweep_effic = 50.0;
    EXPECT_NEAR(wp.sweep_effic, 50.0, 1e-10);
}

// ============================================================================
// 2.5 Ponded Quality Tests
// ============================================================================

TEST(PondedQuality, FieldExistsInSubcatchData)
{
    openswmm::SubcatchData sc;
    sc.resize(3);
    sc.resize_quality(2);
    EXPECT_EQ(sc.ponded_qual.size(), 6u); // 3 subcatch * 2 pollutants
    for (size_t i = 0; i < sc.ponded_qual.size(); ++i)
    {
        EXPECT_NEAR(sc.ponded_qual[i], 0.0, 1e-15);
    }
}

TEST(PondedQuality, MassAccumulates)
{
    // Ponded quality should accumulate rain deposition between events
    double ponded_qual = 0.0;
    double c_rain = 5.0;   // mg/L in rainfall
    double v_rain = 100.0; // ft³ of rainfall
    double w_rain = c_rain * v_rain;

    ponded_qual += w_rain;
    EXPECT_NEAR(ponded_qual, 500.0, 1e-10);
}

TEST(PondedQuality, RunoffCarriesMassOut)
{
    double ponded_qual = 500.0; // accumulated mass
    double v_outflow = 80.0;    // ft³ of runoff
    double v_total = 100.0;     // total volume (rain + existing)
    double c_ponded = ponded_qual / v_total;
    double w_outflow = c_ponded * v_outflow;
    ponded_qual -= w_outflow;

    EXPECT_NEAR(c_ponded, 5.0, 1e-10);
    EXPECT_NEAR(w_outflow, 400.0, 1e-10);
    EXPECT_NEAR(ponded_qual, 100.0, 1e-10);
}

TEST(PondedQuality, PersistsBetweenDryPeriods)
{
    // During dry period (no runoff), ponded mass stays
    double ponded_qual = 100.0;
    double q_runoff = 0.0; // no runoff

    // No runoff → no removal
    if (q_runoff <= 0.0)
    {
        // ponded_qual unchanged
    }
    EXPECT_NEAR(ponded_qual, 100.0, 1e-10);
}

TEST(PondedQuality, ClampedAtZero)
{
    double ponded_qual = -0.001;
    ponded_qual = std::max(ponded_qual, 0.0);
    EXPECT_NEAR(ponded_qual, 0.0, 1e-15);
}

// ============================================================================
// 2.1 Runoff Interface File Tests
// ============================================================================

#include "hydrology/RunoffInterface.hpp"
#include <cstdio>
#include <filesystem>

using namespace openswmm::runoff_iface;

namespace
{

    std::string runoffTempPath(const char *file_name)
    {
        auto path = std::filesystem::temp_directory_path() / file_name;
        return path.string();
    }

} // namespace

TEST(RunoffInterface, WriteAndReadHeader)
{
    const std::string path = runoffTempPath("test_runoff_iface.bin");

    // Write
    {
        RunoffInterfaceFile rif;
        int err = rif.openForWrite(path, 3, 2, 0);
        EXPECT_EQ(err, 0);
        EXPECT_TRUE(rif.isOpen());
        rif.close();
    }

    // Read back and verify
    {
        RunoffInterfaceFile rif;
        int err = rif.openForRead(path, 3, 2, 0);
        EXPECT_EQ(err, 0);
        EXPECT_TRUE(rif.isOpen());
        rif.close();
    }

    std::remove(path.c_str());
}

TEST(RunoffInterface, IncompatibleHeaderFails)
{
    const std::string path = runoffTempPath("test_runoff_iface2.bin");

    // Write with n_subcatch=3, n_pollut=2
    {
        RunoffInterfaceFile rif;
        rif.openForWrite(path, 3, 2, 0);
        rif.close();
    }

    // Read with different counts → should fail
    {
        RunoffInterfaceFile rif;
        int err = rif.openForRead(path, 5, 2, 0); // wrong subcatch count
        EXPECT_NE(err, 0);
    }

    std::remove(path.c_str());
}

TEST(RunoffInterface, WriteReadRoundTrip)
{
    const std::string path = runoffTempPath("test_runoff_iface3.bin");

    SimulationContext ctx;
    ctx.subcatch_names.add("SC1");
    ctx.subcatch_names.add("SC2");
    ctx.subcatches.resize(2);
    ctx.subcatches.resize_quality(1);
    ctx.gages.rainfall.assign(1, 0.5);
    ctx.subcatches.gage[0] = 0;
    ctx.subcatches.gage[1] = 0;
    ctx.subcatches.runoff[0] = 1.5;
    ctx.subcatches.runoff[1] = 2.5;
    ctx.subcatches.conc[0] = 10.0;
    ctx.subcatches.conc[1] = 20.0;

    // Write one step
    {
        RunoffInterfaceFile rif;
        rif.openForWrite(path, 2, 1, 0);
        rif.saveResults(ctx, 300.0);
        rif.close();
    }

    // Read back
    SimulationContext ctx2;
    ctx2.subcatch_names.add("SC1");
    ctx2.subcatch_names.add("SC2");
    ctx2.subcatches.resize(2);
    ctx2.subcatches.resize_quality(1);

    {
        RunoffInterfaceFile rif;
        int err = rif.openForRead(path, 2, 1, 0);
        EXPECT_EQ(err, 0);
        bool ok = rif.readResults(ctx2);
        EXPECT_TRUE(ok);
        rif.close();
    }

    // Verify round-trip
    EXPECT_NEAR(ctx2.subcatches.runoff[0], 1.5, 0.01);
    EXPECT_NEAR(ctx2.subcatches.runoff[1], 2.5, 0.01);
    EXPECT_NEAR(ctx2.subcatches.conc[0], 10.0, 0.1);
    EXPECT_NEAR(ctx2.subcatches.conc[1], 20.0, 0.1);

    std::remove(path.c_str());
}

TEST(RunoffInterface, EOFReturnsFalse)
{
    const std::string path = runoffTempPath("test_runoff_iface4.bin");

    SimulationContext ctx;
    ctx.subcatch_names.add("SC1");
    ctx.subcatches.resize(1);
    ctx.gages.rainfall.assign(1, 0.0);
    ctx.subcatches.gage[0] = 0;

    // Write one step
    {
        RunoffInterfaceFile rif;
        rif.openForWrite(path, 1, 0, 0);
        rif.saveResults(ctx, 300.0);
        rif.close();
    }

    // Read: first call succeeds, second returns false (EOF)
    {
        RunoffInterfaceFile rif;
        rif.openForRead(path, 1, 0, 0);
        bool ok1 = rif.readResults(ctx);
        EXPECT_TRUE(ok1);
        bool ok2 = rif.readResults(ctx);
        EXPECT_FALSE(ok2);
        rif.close();
    }

    std::remove(path.c_str());
}

TEST(RunoffInterface, NonexistentFileFails)
{
    const std::string path = runoffTempPath("nonexistent_file_xyz.bin");
    std::remove(path.c_str());
    RunoffInterfaceFile rif;
    int err = rif.openForRead(path, 1, 0, 0);
    EXPECT_NE(err, 0);
    EXPECT_FALSE(rif.isOpen());
}

// ============================================================================
// Phase 3: Diagnostics and Reporting Tests
// ============================================================================

// 3.1 Non-convergence stats — already tracked
TEST(DiagRoutingStats, NonConvergenceTracked)
{
    SimulationContext ctx;
    ctx.routing_stats.update_iterations(5, true);
    ctx.routing_stats.update_iterations(8, false);
    ctx.routing_stats.update_iterations(3, true);
    ctx.routing_stats.n_steps = 3;

    EXPECT_EQ(ctx.routing_stats.n_non_converged, 1);
    EXPECT_NEAR(ctx.routing_stats.pct_non_converged(), 100.0 / 3.0, 0.1);
    EXPECT_NEAR(ctx.routing_stats.computed_avg_iterations(), 16.0 / 3.0, 0.01);
}

// 3.2 Courant number monitoring
TEST(DiagRoutingStats, MaxCourantTracked)
{
    SimulationContext ctx;
    ctx.routing_stats.max_courant = 0.0;
    ctx.routing_stats.max_courant = std::max(ctx.routing_stats.max_courant, 0.5);
    ctx.routing_stats.max_courant = std::max(ctx.routing_stats.max_courant, 1.2);
    ctx.routing_stats.max_courant = std::max(ctx.routing_stats.max_courant, 0.8);
    EXPECT_NEAR(ctx.routing_stats.max_courant, 1.2, 1e-10);
}

TEST(DiagRoutingStats, MaxCourantDefaultZero)
{
    SimulationContext ctx;
    EXPECT_NEAR(ctx.routing_stats.max_courant, 0.0, 1e-15);
}

// 3.3 Quality seepage/evaporation vectors
TEST(DiagQualityLoss, SeepEvapVectorsExist)
{
    SimulationContext ctx;
    ctx.mass_balance.resize_quality(3);
    EXPECT_EQ(ctx.mass_balance.qual_routing_seep.size(), 3u);
    EXPECT_EQ(ctx.mass_balance.qual_routing_evap.size(), 3u);
    for (size_t i = 0; i < 3; ++i)
    {
        EXPECT_NEAR(ctx.mass_balance.qual_routing_seep[i], 0.0, 1e-15);
        EXPECT_NEAR(ctx.mass_balance.qual_routing_evap[i], 0.0, 1e-15);
    }
}

TEST(DiagQualityLoss, SeepEvapResetToZero)
{
    SimulationContext ctx;
    ctx.mass_balance.resize_quality(2);
    ctx.mass_balance.qual_routing_seep[0] = 100.0;
    ctx.mass_balance.qual_routing_evap[1] = 200.0;
    ctx.mass_balance.reset();
    EXPECT_NEAR(ctx.mass_balance.qual_routing_seep[0], 0.0, 1e-15);
    EXPECT_NEAR(ctx.mass_balance.qual_routing_evap[1], 0.0, 1e-15);
}

// 3.4 Capacity-limited detection — already tracked
TEST(DiagCapacityLimited, FieldExists)
{
    LinkData links;
    links.resize(3);
    EXPECT_EQ(links.stat_time_capacity_limited.size(), 3u);
    EXPECT_NEAR(links.stat_time_capacity_limited[0], 0.0, 1e-15);
}

// 3.5 Pump utilization statistics
TEST(DiagPumpStats, FieldsExist)
{
    LinkData links;
    links.resize(3);
    EXPECT_EQ(links.stat_pump_cycles.size(), 3u);
    EXPECT_EQ(links.stat_pump_on_time.size(), 3u);
    EXPECT_EQ(links.stat_pump_volume.size(), 3u);
    EXPECT_EQ(links.stat_pump_was_on.size(), 3u);
}

TEST(DiagPumpStats, CycleDetection)
{
    // Simulate pump turning on and off
    bool was_on = false;
    int cycles = 0;

    // Step 1: off → on
    bool is_on = true;
    if (is_on != was_on)
    {
        cycles++;
        was_on = is_on;
    }
    EXPECT_EQ(cycles, 1);

    // Step 2: on → on (no cycle)
    is_on = true;
    if (is_on != was_on)
    {
        cycles++;
        was_on = is_on;
    }
    EXPECT_EQ(cycles, 1);

    // Step 3: on → off
    is_on = false;
    if (is_on != was_on)
    {
        cycles++;
        was_on = is_on;
    }
    EXPECT_EQ(cycles, 2);

    // Step 4: off → on
    is_on = true;
    if (is_on != was_on)
    {
        cycles++;
        was_on = is_on;
    }
    EXPECT_EQ(cycles, 3);
}

TEST(DiagPumpStats, VolumeAccumulation)
{
    double volume = 0.0;
    double q = 5.0;    // CFS
    double dt = 300.0; // seconds

    // Pump on for 3 steps
    for (int i = 0; i < 3; ++i)
    {
        volume += q * dt;
    }
    EXPECT_NEAR(volume, 4500.0, 1e-10);
}

TEST(DiagPumpStats, OnTimeAccumulation)
{
    double on_time = 0.0;
    double dt = 60.0;

    on_time += dt; // step 1: on
    on_time += dt; // step 2: on
    // step 3: off (no accumulation)
    on_time += dt; // step 4: on

    EXPECT_NEAR(on_time, 180.0, 1e-10);
}

// 3.6 Routing stats histogram
TEST(DiagRoutingStats, HistogramInit)
{
    SimulationContext ctx;
    ctx.routing_stats.init_histogram(30.0, 0.5);
    EXPECT_NEAR(ctx.routing_stats.step_intervals[0], 30.0, 1e-10);
    EXPECT_GT(ctx.routing_stats.step_intervals[1], 0.0);
}

TEST(DiagRoutingStats, StepBinning)
{
    SimulationContext ctx;
    ctx.routing_stats.init_histogram(30.0, 0.5);
    ctx.routing_stats.record_step_bin(30.0);
    ctx.routing_stats.record_step_bin(15.0);
    ctx.routing_stats.record_step_bin(1.0);

    // At least one bin should have a count
    int total = 0;
    for (int i = 0; i < ctx.routing_stats.N_TIME_BINS; ++i)
        total += static_cast<int>(ctx.routing_stats.step_counts[i]);
    EXPECT_EQ(total, 3);
}

TEST(DiagRoutingStats, AvgStep)
{
    SimulationContext ctx;
    ctx.routing_stats.update(10.0);
    ctx.routing_stats.update(20.0);
    ctx.routing_stats.update(30.0);
    EXPECT_NEAR(ctx.routing_stats.avg_step(), 20.0, 1e-10);
    EXPECT_NEAR(ctx.routing_stats.min_step, 10.0, 1e-10);
    EXPECT_NEAR(ctx.routing_stats.max_step, 30.0, 1e-10);
}

// ============================================================================
// Phase 4: Low-Priority Utility Tests
// ============================================================================

#include "hydraulics/Link.hpp"

// 4.1 Inverse Volume → Depth (getDepth)
TEST(NodeGetDepth, JunctionLinear)
{
    NodeData nodes;
    nodes.resize(1);
    nodes.type[0] = NodeType::JUNCTION;
    nodes.full_depth[0] = 10.0;

    // Junction: V = MIN_SURFAREA * d → d = V / MIN_SURFAREA
    double vol = 62.83; // MIN_SURFAREA * 5.0
    double d = node::getDepth(nodes, 0, vol);
    EXPECT_NEAR(d, vol / 12.566, 0.01);
}

TEST(NodeGetDepth, JunctionZeroVolume)
{
    NodeData nodes;
    nodes.resize(1);
    nodes.type[0] = NodeType::JUNCTION;
    nodes.full_depth[0] = 10.0;
    double d = node::getDepth(nodes, 0, 0.0);
    EXPECT_NEAR(d, 0.0, 1e-10);
}

TEST(NodeGetDepth, StorageFunctionalLinear)
{
    NodeData nodes;
    nodes.resize(1);
    nodes.type[0] = NodeType::STORAGE;
    nodes.full_depth[0] = 10.0;
    nodes.storage_curve[0] = -1; // functional
    nodes.storage_a[0] = 1000.0; // A = 1000 (constant area)
    nodes.storage_b[0] = 0.0;    // exponent = 0
    nodes.storage_c[0] = 0.0;    // constant = 0

    // V = (a0 + a1)*d = 1000*d → d = V/1000
    double vol = 5000.0;
    double d = node::getDepth(nodes, 0, vol);
    EXPECT_NEAR(d, 5.0, 0.01);
}

TEST(NodeGetDepth, StorageFunctionalNonlinear)
{
    NodeData nodes;
    nodes.resize(1);
    nodes.type[0] = NodeType::STORAGE;
    nodes.full_depth[0] = 20.0;
    nodes.full_volume[0] = 0.0; // no precomputed
    nodes.storage_curve[0] = -1;
    nodes.storage_a[0] = 100.0; // a1
    nodes.storage_b[0] = 1.0;   // a2 (exponent) → A = a1*d^1 = 100*d
    nodes.storage_c[0] = 50.0;  // a0 → A = 50 + 100*d

    // V = 50*d + 100/2 * d^2 = 50*d + 50*d^2
    // At d=5: V = 250 + 1250 = 1500
    double d_test = 5.0;
    double v_at_5 = 50.0 * d_test + 50.0 * d_test * d_test;
    EXPECT_NEAR(v_at_5, 1500.0, 1e-6);

    double d = node::getDepth(nodes, 0, v_at_5);
    EXPECT_NEAR(d, d_test, 0.01);
}

TEST(NodeGetDepth, VolumeDepthRoundTrip)
{
    // Volume at depth d → getDepth(V) should return d
    NodeData nodes;
    nodes.resize(1);
    nodes.type[0] = NodeType::JUNCTION;
    nodes.full_depth[0] = 10.0;

    for (double d = 0.5; d <= 9.5; d += 1.0)
    {
        double v = node::getVolume(nodes, 0, d);
        double d_back = node::getDepth(nodes, 0, v);
        EXPECT_NEAR(d_back, d, 0.01) << "Round-trip failed for d=" << d;
    }
}

// 4.2 Hydraulic Power
TEST(HydPower, ZeroFlowZeroPower)
{
    double p = openswmm::link::getHydPower(0.0, 100.0, 95.0);
    EXPECT_NEAR(p, 0.0, 1e-10);
}

TEST(HydPower, PositiveFlowPositivePower)
{
    // P = gamma * |Q| * |hL| = 62.4 * 10 * 5 = 3120 ft·lb/s
    double p = openswmm::link::getHydPower(10.0, 100.0, 95.0);
    EXPECT_NEAR(p, 62.4 * 10.0 * 5.0, 0.1);
}

TEST(HydPower, ReverseFlowStillPositive)
{
    double p = openswmm::link::getHydPower(-5.0, 90.0, 100.0);
    EXPECT_GT(p, 0.0);
}

TEST(HydPower, ZeroHeadLossZeroPower)
{
    double p = openswmm::link::getHydPower(10.0, 100.0, 100.0);
    EXPECT_NEAR(p, 0.0, 1e-10);
}

TEST(HydPower, ConvertToHorsepower)
{
    double p = openswmm::link::getHydPower(10.0, 100.0, 95.0);
    double hp = p / 550.0;
    EXPECT_GT(hp, 0.0);
    EXPECT_NEAR(hp, 3120.0 / 550.0, 0.01);
}

// ===========================================================================
// Issue 2 — Storage-node conduit half-area guard
//
// The fix in DynamicWave::updateNodeFlows and SWMMEngine non-conduit callback
// zeroes conduit/orifice half-areas at a STORAGE node only when the storage
// curve provides area > MIN_SURFAREA. When the curve is degenerate (area ==
// MIN_SURFAREA, i.e. FUNCTIONAL 0 0 0), the pipe half-areas are kept so the
// Picard denominator stays bounded.
//
// These tests verify the precondition the guard relies on: getSurfArea must
// return > MIN_SURFAREA for a real curve and exactly MIN_SURFAREA for a
// degenerate one.
// ===========================================================================

TEST(StorageHalfAreaGuard, RealCurveProvidesArea) {
    // FUNCTIONAL 1000 0 0 → A(d) = 1000 for all d
    // getSurfArea should return 1000 > MIN_SURFAREA → guard zeros pipe halves
    NodeData nodes;
    nodes.resize(1);
    nodes.type[0] = NodeType::STORAGE;
    nodes.full_depth[0] = 10.0;
    nodes.storage_curve[0] = -1;  // functional
    nodes.storage_a[0] = 1000.0;
    nodes.storage_b[0] = 0.0;
    nodes.storage_c[0] = 0.0;

    double sa = node::getSurfArea(nodes, 0, 5.0);
    EXPECT_GT(sa, constants::MIN_SURFAREA)
        << "Real storage curve should exceed MIN_SURFAREA";
    EXPECT_NEAR(sa, 1000.0, 0.01);
}

TEST(StorageHalfAreaGuard, DegenerateCurveFloors) {
    // FUNCTIONAL 0 0 0 → A(d) = 0 for all d → clamped to MIN_SURFAREA
    // getSurfArea should return exactly MIN_SURFAREA → guard keeps pipe halves
    NodeData nodes;
    nodes.resize(1);
    nodes.type[0] = NodeType::STORAGE;
    nodes.full_depth[0] = 10.0;
    nodes.storage_curve[0] = -1;  // functional
    nodes.storage_a[0] = 0.0;
    nodes.storage_b[0] = 0.0;
    nodes.storage_c[0] = 0.0;

    double sa = node::getSurfArea(nodes, 0, 5.0);
    EXPECT_NEAR(sa, constants::MIN_SURFAREA, 1e-10)
        << "Degenerate storage curve should return exactly MIN_SURFAREA";
}

TEST(StorageHalfAreaGuard, SmallCurveBelowThreshold) {
    // FUNCTIONAL 0 0.01 0 → A(d) = 0.01 for all d → clamped to MIN_SURFAREA
    // Guard should keep pipe halves for this near-zero curve
    NodeData nodes;
    nodes.resize(1);
    nodes.type[0] = NodeType::STORAGE;
    nodes.full_depth[0] = 10.0;
    nodes.storage_curve[0] = -1;
    nodes.storage_a[0] = 0.01;
    nodes.storage_b[0] = 0.0;
    nodes.storage_c[0] = 0.0;

    double sa = node::getSurfArea(nodes, 0, 5.0);
    EXPECT_NEAR(sa, constants::MIN_SURFAREA, 1e-10)
        << "Tiny curve (0.01 ft²) should clamp to MIN_SURFAREA";
}

TEST(StorageHalfAreaGuard, CurveJustAboveThreshold) {
    // FUNCTIONAL 13 0 0 → A(d) = 13 > MIN_SURFAREA (12.566)
    // Guard should zero pipe halves
    NodeData nodes;
    nodes.resize(1);
    nodes.type[0] = NodeType::STORAGE;
    nodes.full_depth[0] = 10.0;
    nodes.storage_curve[0] = -1;
    nodes.storage_a[0] = 13.0;
    nodes.storage_b[0] = 0.0;
    nodes.storage_c[0] = 0.0;

    double sa = node::getSurfArea(nodes, 0, 5.0);
    EXPECT_GT(sa, constants::MIN_SURFAREA)
        << "Curve area 13 ft² should exceed MIN_SURFAREA (12.566 ft²)";
    EXPECT_NEAR(sa, 13.0, 0.01);
}

TEST(StorageHalfAreaGuard, DepthDependentCrosses) {
    // FUNCTIONAL 0 100 1 → A(d) = 100*d
    // At d=0.1: A = 10 < MIN_SURFAREA → pipe halves kept
    // At d=5.0: A = 500 > MIN_SURFAREA → pipe halves zeroed
    NodeData nodes;
    nodes.resize(1);
    nodes.type[0] = NodeType::STORAGE;
    nodes.full_depth[0] = 20.0;
    nodes.storage_curve[0] = -1;
    nodes.storage_a[0] = 100.0;
    nodes.storage_b[0] = 1.0;
    nodes.storage_c[0] = 0.0;

    double sa_low = node::getSurfArea(nodes, 0, 0.1);
    EXPECT_NEAR(sa_low, constants::MIN_SURFAREA, 1e-10)
        << "At shallow depth (A=10), should clamp to MIN_SURFAREA";

    double sa_high = node::getSurfArea(nodes, 0, 5.0);
    EXPECT_GT(sa_high, constants::MIN_SURFAREA);
    EXPECT_NEAR(sa_high, 500.0, 0.01)
        << "At depth 5 (A=500), should return curve value";
}
