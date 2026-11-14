// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "core/CompletedSourceArchive.hpp"
#include "core/DateTime.hpp"
#include "core/SimulationContext.hpp"
#include "hydrology/Groundwater.hpp"
#include <stdexcept>

namespace openswmm::testing {
// Heterogeneous input fixture shared by exact-output and isolated timing probes.
// No groundwater equations or expected-value implementation is duplicated here.
struct GroundwaterParallelFixture {
    SimulationContext context;
    groundwater::GWSolver solver;
    std::vector<double> infiltration, surface_head, pervious, surface_evaporation;

    explicit GroundwaterParallelFixture(int count, int threads, bool si = false)
        : infiltration(count), surface_head(count), pervious(count), surface_evaporation(count) {
        context.options.num_threads = threads;
        context.options.flow_units = si ? FlowUnits::CMS : FlowUnits::CFS;
        context.current_date = datetime::encodeDate(2026, 3, 31);
        for (int i = 0; i < count; ++i)
            context.subcatch_names.add("S" + std::to_string(i));
        context.subcatches.resize(count);
        context.node_names.add("N0");
        context.node_names.add("N1");
        context.nodes.resize(2);
        context.nodes.inflow[0] = .01;
        context.nodes.inflow[1] = .1;
        context.nodes.volume[0] = 1.;
        context.nodes.volume[1] = 10.;
        context.patterns.add("MONTHLY", 0, {.1, .3, .4, .5, .6, .7, .8, .8, .7, .5, .4, .2});
        context.forcing.subcatch_evap_mode.resize(count);
        context.forcing.subcatch_evap_value.resize(count);
        solver.init(count);
        auto& state = solver.state();
        for (int i = 0; i < count; ++i) {
            context.subcatches.area[i] = 5000. + 100. * (i % 9);
            context.subcatches.gw_node[i] = i % 3 ? i % 2 : -1;
            context.subcatches.outlet_node[i] = i % 2;
            state.porosity[i] = .4;
            state.field_cap[i] = .2;
            state.wilt_point[i] = .1;
            state.k_sat[i] = 1e-5 * (1. + .1 * (i % 5));
            state.k_slope[i] = 10.;
            state.tension_slope[i] = i % 3 ? 0. : 1.;
            state.upper_evap_frac[i] = .5;
            state.upper_evap_pat[i] = i % 2 ? 0 : -1;
            state.lower_evap_depth[i] = 2.;
            state.lower_loss_coeff[i] = 1e-6;
            state.total_depth[i] = i % 13 ? 10. : 0.;
            state.a1[i] = .001;
            state.b1[i] = i % 3 == 0 ? 0. : i % 3 == 1 ? 1. : 1.5;
            state.a2[i] = .0002;
            state.b2[i] = i % 4 == 0 ? 0. : i % 4 == 1 ? 1. : 1.25;
            state.a3[i] = i % 2 ? .00001 : 0.;
            state.h_star[i] = 2.;
            state.theta[i] = .15 + .02 * (i % 8);
            state.lower_depth[i] = 3. + .2 * (i % 9);
            state.old_flow[i] = -.00001 * (i % 3);
            state.gw_flow[i] = .00002 * (i % 4);
            state.upper_evap[i] = i % 2 ? 0. : -0.;
            state.lower_evap[i] = .000001;
            state.deep_loss[i] = .000002;
            if (i % 5 == 0) {
                auto& expression = state.lateral_expr[i];
                if (mathexpr::parse("0.000001 * HGW + 0.000002 * HSW", expression) != 0 ||
                    mathexpr::bind_variables(expression, groundwater::GW_VAR_NAMES,
                                            groundwater::GWV_MAX) != 2)
                    throw std::logic_error("invalid groundwater fixture lateral expression");
            }
            if (i % 7 == 0) {
                auto& expression = state.deep_expr[i];
                if (mathexpr::parse("0.000001 * THETA + 0.000002 * K", expression) != 0 ||
                    mathexpr::bind_variables(expression, groundwater::GW_VAR_NAMES,
                                            groundwater::GWV_MAX) != 2)
                    throw std::logic_error("invalid groundwater fixture deep expression");
            }
        }
        context.mass_balance.gw_infil = -0.;
        context.mass_balance.gw_upper_evap = .125;
        context.mass_balance.gw_lower_evap = .25;
        context.mass_balance.gw_lower_perc = .5;
        context.mass_balance.gw_lateral_flow = -1.;
    }

    void prepare(int step) {
        context.current_date = datetime::encodeDate(2026, 3, 31) + step;
        for (std::size_t i = 0; i < infiltration.size(); ++i) {
            infiltration[i] = (i + step) % 6 == 0 ? 0.
                : (i % 6) * 2e-7 + (step % 5) * 1e-8;
            surface_head[i] = i % 3 ? 2. : 8.;
            pervious[i] = i % 17 ? .2 + .1 * (i % 8) : 0.;
            surface_evaporation[i] = (i % 4) * 1e-8;
            context.forcing.subcatch_evap_mode[i] =
                (i + step) % 3 == 0 ? ForcingMode::NONE
                : (i + step) % 3 == 1 ? ForcingMode::ADD : ForcingMode::OVERRIDE;
            context.forcing.subcatch_evap_value[i] = (i % 5) * 2e-8;
        }
    }

    void advance(double dt, int step) {
        const int months[] = {0, 4, 12, 1, 9, 0, 6, -1, 2};
        solver.execute(context, dt, 1e-7, infiltration.data(), surface_head.data(),
                       pervious.data(), surface_evaporation.data(), months[step % 9]);
    }

    void capture(detail::CompletedSourceArchive& history) {
        auto& state = solver.state();
        auto& balance = context.mass_balance;
        history(state.theta, state.lower_depth, state.old_flow, state.gw_flow,
                state.upper_evap, state.lower_evap, state.deep_loss, state.max_infil_vol,
                balance.gw_infil, balance.gw_upper_evap, balance.gw_lower_evap,
                balance.gw_lower_perc, balance.gw_lateral_flow);
    }
};
}
