/**
 * @file test_dynamic_preissmann_slot.cpp
 * @brief API-level tests for Dynamic Preissmann Slot (DPS) behavior.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "hydraulics/DynamicWave.hpp"
#include "hydraulics/XSectBatch.hpp"
#include "core/SimulationContext.hpp"
#include "core/OperatorSnapshotState.hpp"

using namespace openswmm;
using namespace openswmm::dynwave;

namespace
{

    SimulationContext buildMinimalContext(double diameter_ft)
    {
        SimulationContext ctx;

        ctx.options.flow_units = FlowUnits::CFS;
        ctx.options.routing_model = RoutingModel::DYNWAVE;

        ctx.nodes.resize(2);
        ctx.nodes.type[0] = NodeType::JUNCTION;
        ctx.nodes.type[1] = NodeType::JUNCTION;
        ctx.nodes.invert_elev[0] = 100.0;
        ctx.nodes.invert_elev[1] = 99.0;
        ctx.nodes.depth[0] = diameter_ft;
        ctx.nodes.depth[1] = diameter_ft;
        ctx.nodes.head[0] = ctx.nodes.invert_elev[0] + ctx.nodes.depth[0];
        ctx.nodes.head[1] = ctx.nodes.invert_elev[1] + ctx.nodes.depth[1];
        ctx.nodes.volume[0] = 0.0;
        ctx.nodes.volume[1] = 0.0;

        ctx.links.resize(1);
        ctx.links.type[0] = LinkType::CONDUIT;
        ctx.links.node1[0] = 0;
        ctx.links.node2[0] = 1;
        ctx.links.offset1[0] = 0.0;
        ctx.links.offset2[0] = 0.0;
        ctx.links.length[0] = 1000.0;
        ctx.links.mod_length[0] = 1000.0;
        ctx.links.barrels[0] = 1;
        ctx.links.roughness[0] = 0.013;
        ctx.links.slope[0] = 0.001;
        ctx.links.flow[0] = 0.0;

        XSectParams xs;
        double p[4] = {diameter_ft, 0.0, 0.0, 0.0};
        xsect::setParams(xs, static_cast<int>(XsectShape::CIRCULAR), p, 1.0);

        ctx.links.xsect_shape[0] = XsectShape::CIRCULAR;
        ctx.links.xsect_y_full[0] = xs.y_full;
        ctx.links.xsect_a_full[0] = xs.a_full;
        ctx.links.xsect_w_max[0] = xs.w_max;
        ctx.links.xsect_r_full[0] = xs.r_full;
        ctx.links.xsect_s_full[0] = xs.s_full;
        ctx.links.xsect_s_max[0] = xs.s_max;

        return ctx;
    }

    XSectGroups buildSingleCircularGroup(double diameter_ft)
    {
        std::vector<XSectParams> params(1);
        double p[4] = {diameter_ft, 0.0, 0.0, 0.0};
        xsect::setParams(params[0], static_cast<int>(XsectShape::CIRCULAR), p, 1.0);

        XSectGroups groups;
        groups.build(params.data(), static_cast<int>(params.size()));
        return groups;
    }

    struct SnapshotCapture
    {
        OperatorSnapshotState staging;
        SWMM_OperatorSnapshot snap{};
    };

    SnapshotCapture snapshotFor(DWSolver &solver,
                                const SimulationContext &ctx,
                                int n_nodes,
                                int n_links)
    {
        SnapshotCapture captured;
        captured.staging.resizeStaging(n_nodes, n_links, solver.numConduits());
        solver.populateSnapshot(ctx, 0.0, 0, true, captured.snap, captured.staging);
        return captured;
    }

    double expectedP0(const SimulationContext &ctx)
    {
        const double c_pT_fts = ctx.options.dps_target_celerity * 3.28084;
        const double alpha = std::max(ctx.options.dps_alpha, 2.0);
        const double af = ctx.links.xsect_a_full[0];
        const double tw = ctx.links.xsect_w_max[0];
        const double l_d = (tw > 0.0) ? af / tw : 0.0;
        const double c_g = (l_d > 0.0) ? std::sqrt(32.2 * l_d) : 1.0;
        return std::max(c_pT_fts / (alpha * c_g), 1.0);
    }

} // namespace

TEST(DPSOptions, DefaultsLiveInSimulationOptions)
{
    SimulationContext ctx;
    EXPECT_DOUBLE_EQ(ctx.options.dps_target_celerity, 25.0);
    EXPECT_DOUBLE_EQ(ctx.options.dps_alpha, 3.0);
    EXPECT_DOUBLE_EQ(ctx.options.dps_decay_time, 0.5);
}

TEST(DPSPublicApi, EnumValueIsStable)
{
    EXPECT_EQ(static_cast<int>(SurchargeMethod::DYNAMIC_SLOT), 2);
}

TEST(DPSPublicApi, InitWithContextExposesDpsArraysInSnapshot)
{
    SimulationContext ctx = buildMinimalContext(3.0);
    ctx.options.surcharge_method = static_cast<int>(SurchargeMethod::DYNAMIC_SLOT);

    XSectGroups groups = buildSingleCircularGroup(3.0);
    DWSolver solver;
    solver.surcharge_method = SurchargeMethod::DYNAMIC_SLOT;
    solver.init(2, 1, groups, ctx);

    SnapshotCapture captured = snapshotFor(solver, ctx, 2, 1);
    ASSERT_NE(captured.snap.dps_slot_area, nullptr);
    ASSERT_NE(captured.snap.dps_surcharge_head, nullptr);
    ASSERT_NE(captured.snap.dps_preissmann_num, nullptr);
    EXPECT_GE(captured.snap.dps_preissmann_num[0], 1.0);
}

TEST(DPSPublicApi, ExtranModeDoesNotExposeDpsArrays)
{
    SimulationContext ctx = buildMinimalContext(3.0);
    XSectGroups groups = buildSingleCircularGroup(3.0);

    DWSolver solver;
    solver.surcharge_method = SurchargeMethod::EXTRAN;
    solver.init(2, 1, groups, ctx);

    SnapshotCapture captured = snapshotFor(solver, ctx, 2, 1);
    EXPECT_EQ(captured.snap.dps_slot_area, nullptr);
    EXPECT_EQ(captured.snap.dps_surcharge_head, nullptr);
    EXPECT_EQ(captured.snap.dps_preissmann_num, nullptr);
}

TEST(DPSPublicApi, InitialPreissmannNumberMatchesConfiguredOptions)
{
    SimulationContext ctx = buildMinimalContext(3.0);
    ctx.options.surcharge_method = static_cast<int>(SurchargeMethod::DYNAMIC_SLOT);
    ctx.options.dps_target_celerity = 25.0;
    ctx.options.dps_alpha = 3.0;

    XSectGroups groups = buildSingleCircularGroup(3.0);
    DWSolver solver;
    solver.surcharge_method = SurchargeMethod::DYNAMIC_SLOT;
    solver.init(2, 1, groups, ctx);

    SnapshotCapture captured = snapshotFor(solver, ctx, 2, 1);
    ASSERT_NE(captured.snap.dps_preissmann_num, nullptr);
    EXPECT_NEAR(captured.snap.dps_preissmann_num[0], expectedP0(ctx), 1e-9);
}

TEST(DPSPublicApi, HigherTargetCelerityIncreasesInitialPreissmannNumber)
{
    SimulationContext low_ctx = buildMinimalContext(3.0);
    low_ctx.options.surcharge_method = static_cast<int>(SurchargeMethod::DYNAMIC_SLOT);
    low_ctx.options.dps_target_celerity = 20.0;

    SimulationContext high_ctx = low_ctx;
    high_ctx.options.dps_target_celerity = 40.0;

    XSectGroups groups = buildSingleCircularGroup(3.0);

    DWSolver low_solver;
    low_solver.surcharge_method = SurchargeMethod::DYNAMIC_SLOT;
    low_solver.init(2, 1, groups, low_ctx);
    SnapshotCapture low_captured = snapshotFor(low_solver, low_ctx, 2, 1);

    DWSolver high_solver;
    high_solver.surcharge_method = SurchargeMethod::DYNAMIC_SLOT;
    high_solver.init(2, 1, groups, high_ctx);
    SnapshotCapture high_captured = snapshotFor(high_solver, high_ctx, 2, 1);

    ASSERT_NE(low_captured.snap.dps_preissmann_num, nullptr);
    ASSERT_NE(high_captured.snap.dps_preissmann_num, nullptr);
    EXPECT_GT(high_captured.snap.dps_preissmann_num[0], low_captured.snap.dps_preissmann_num[0]);
}

TEST(DPSPublicApi, HigherAlphaDecreasesInitialPreissmannNumber)
{
    SimulationContext low_alpha_ctx = buildMinimalContext(3.0);
    low_alpha_ctx.options.surcharge_method = static_cast<int>(SurchargeMethod::DYNAMIC_SLOT);
    low_alpha_ctx.options.dps_alpha = 2.0;

    SimulationContext high_alpha_ctx = low_alpha_ctx;
    high_alpha_ctx.options.dps_alpha = 6.0;

    XSectGroups groups = buildSingleCircularGroup(3.0);

    DWSolver low_alpha_solver;
    low_alpha_solver.surcharge_method = SurchargeMethod::DYNAMIC_SLOT;
    low_alpha_solver.init(2, 1, groups, low_alpha_ctx);
    SnapshotCapture low_alpha_captured = snapshotFor(low_alpha_solver, low_alpha_ctx, 2, 1);

    DWSolver high_alpha_solver;
    high_alpha_solver.surcharge_method = SurchargeMethod::DYNAMIC_SLOT;
    high_alpha_solver.init(2, 1, groups, high_alpha_ctx);
    SnapshotCapture high_alpha_captured = snapshotFor(high_alpha_solver, high_alpha_ctx, 2, 1);

    ASSERT_NE(low_alpha_captured.snap.dps_preissmann_num, nullptr);
    ASSERT_NE(high_alpha_captured.snap.dps_preissmann_num, nullptr);
    EXPECT_LT(high_alpha_captured.snap.dps_preissmann_num[0], low_alpha_captured.snap.dps_preissmann_num[0]);
}
