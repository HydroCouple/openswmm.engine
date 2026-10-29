// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "core/SimulationContext.hpp"
#include "core/CompletedSourceArchive.hpp"
#include "core/UnitConversion.hpp"
#include "hydrology/Runoff.hpp"
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef SWMM_USE_OPENMP
#include <omp.h>
#endif

namespace runoff_parallel_fixture {
using namespace openswmm;
struct Case {
    SimulationContext ctx;
    runoff::RunoffSolver solver;
    std::vector<int> owners;
    std::vector<runoff::RunoffSourceForcing> completed;
    std::vector<int> callbacks;
    int kind;
    // kind: ordinary, owner map, nested, configured inactive snow,
    // completed forcing, declining boundary, throwing boundary.
    Case(int n, int threads, bool si, int mode = 0) : kind(mode) {
        ctx.options.flow_units = si ? FlowUnits::CMS : FlowUnits::CFS;
        ctx.options.num_threads = threads;
        ctx.options.ignore_snow_melt = mode != 3;
        ctx.options.evap_dry_only = true;
        if (mode == 3)
            ctx.snowpacks.plowable.resize(1);
        for (int i = 0; i < n; ++i)
            ctx.subcatch_names.add("R" + std::to_string(i));
        ctx.subcatches.resize(n);
        const double area = ucf::UCF(ucf::LANDAREA, ctx.options);
        const double depth = ucf::UCF(ucf::RAINDEPTH, ctx.options);
        const double rain = ucf::UCF(ucf::RAINFALL, ctx.options);
        ctx.forcing.subcatch_rainfall_mode.assign(n, ForcingMode::OVERRIDE);
        ctx.forcing.subcatch_rainfall_value.resize(n);
        ctx.forcing.subcatch_evap_mode.assign(n, ForcingMode::OVERRIDE);
        ctx.forcing.subcatch_evap_value.resize(n);
        ctx.subcatch_infil_pattern.assign(n, 0);
        ctx.subcatch_d_store_pattern.assign(n, 1);
        ctx.subcatch_n_perv_pattern.assign(n, 2);
        ctx.patterns.types.assign(3, 0);
        ctx.patterns.factors = {{.8, 1.2, .4, 1., 0., 1., 1., 1., 1., 1., 1., 1.},
                               {1.1, .7, 1., 0., 1., 1., 1., 1., 1., 1., 1., 1.},
                               {.9, 1.3, 1., 0., 1., 1., 1., 1., 1., 1., 1., 1.}};
        owners.assign(n, -1);
        completed.resize(n);
        for (int i = 0; i < n; ++i) {
            const double ft2 = i % 97 == 0 ? 0. : 32000. + 173. * (i % 19);
            ctx.subcatches.area[i] = ft2 * area;
            ctx.subcatches.total_lid_area_ft2[i] = i % 101 == 0 ? ft2 : .03 * ft2;
            ctx.subcatches.width[i] = 90. + i % 23;
            ctx.subcatches.slope[i] = .002 + .001 * (i % 9);
            ctx.subcatches.frac_imperv[i] = i % 13 == 0 ? 0. : i % 11 == 0 ? 1. : .2 + .1 * (i % 6);
            ctx.subcatches.pct_zero[i] = 10. + 7. * (i % 11);
            ctx.subcatches.n_imperv[i] = i % 41 == 0 ? 0. : .013;
            ctx.subcatches.n_perv[i] = i % 43 == 0 ? 0. : .12;
            ctx.subcatches.ds_imperv[i] = .004 * depth;
            ctx.subcatches.ds_perv[i] = .009 * depth;
            ctx.subcatches.subarea_routing[i] = i % 3;
            ctx.subcatches.pct_routed[i] = .15 + .1 * (i % 5);
            ctx.subcatches.infil_model[i] = i % 5;
            if (i % 5 < 2) {
                ctx.subcatches.infil_p1[i] = 6e-5 * rain;
                ctx.subcatches.infil_p2[i] = 2e-6 * rain;
                ctx.subcatches.infil_p3[i] = 3.;
                ctx.subcatches.infil_p4[i] = i % 17 == 0 ? 0. : 7.;
                ctx.subcatches.infil_p5[i] = .3 * depth;
            } else if (i % 5 < 4) {
                ctx.subcatches.infil_p1[i] = .3 * depth;
                ctx.subcatches.infil_p2[i] = 3e-6 * rain;
                ctx.subcatches.infil_p3[i] = .25;
            } else {
                ctx.subcatches.infil_p1[i] = 72.;
                ctx.subcatches.infil_p3[i] = 7.;
            }
            ctx.subcatches.gw_max_infil_vol[i] = i % 7 == 0 ? .0001 : 1e31;
            owners[i] = i % 7 == 0 ? 0 : -1;
            completed[i].subcatch = i;
            completed[i].rain = 3e-5 + 1e-6 * (i % 5);
            completed[i].pet = 1e-7;
            completed[i].surface_return = {1e-7, 2e-7, 3e-7};
        }
        solver.init(ctx);
        auto& s = solver.soa();
        for (int i = 0; i < n; ++i) {
            s.depth_imperv0[i] = .001 * (i % 9);
            s.depth_imperv1[i] = .002 * (i % 7);
            s.depth_perv[i] = .003 * (i % 5);
            s.old_runoff_imperv0[i] = 1e-6 * (i % 3);
            s.old_runoff_imperv1[i] = 2e-6 * (i % 4);
            s.old_runoff_perv[i] = 3e-7 * (i % 5);
        }
        prepare(0);
    }
    void prepare(int step) {
        const double rain = ucf::UCF(ucf::RAINFALL, ctx.options);
        for (int i = 0; i < ctx.n_subcatches(); ++i) {
            const double r = step % 4 == 2 ? 0. : 3e-5 + 1e-6 * ((i + step) % 5);
            ctx.forcing.subcatch_rainfall_value[i] = r * rain;
            ctx.forcing.subcatch_evap_value[i] = (1 + (i + step) % 3) * 1e-7;
            ctx.subcatches.runon_rate[i] = (i % 6) * 1e-7;
            ctx.subcatches.lid_return_to_perv_cfs[i] = (i % 4) * 2e-7;
            completed[i].rain = r;
        }
    }
    void advance(double dt, int step) {
        runoff::RunoffSolver::InfiltrationBoundary boundary = [&](int i, double, double, double&) {
            callbacks.push_back(i);
            if (kind == 6 && i >= 17)
                throw std::runtime_error("fixture intake failure");
            return false;
        };
        const auto call = [&] {
            solver.execute(ctx, dt, 2e-7, .9, .85, step % 4,
                           kind >= 5 ? &boundary : nullptr,
                           kind == 4 ? &completed : nullptr, nullptr,
                           kind == 1 ? &owners : nullptr);
        };
#ifdef SWMM_USE_OPENMP
        if (kind == 2) {
#pragma omp parallel num_threads(2)
            {
#pragma omp single
                call();
            }
        } else
#endif
            call();
    }
    std::string capture() {
        detail::CompletedSourceArchive a;
        solver.checkpoint(a, false);
        auto& s = ctx.subcatches;
        a(s.rainfall, s.runoff, s.subarea_runoff_rate, s.ponded_depth,
          s.old_ponded_depth, s.evap_loss, s.infil_loss, s.stat_evap_vol,
          s.stat_infil_vol, s.stat_imperv_vol, s.stat_perv_vol, s.lid_return_to_perv_cfs, callbacks);
        return a.bytes();
    }
    void requireFinite() const {
        const auto& s = solver.soa();
        for (const auto* values : {&s.runoff, &s.depth_imperv0, &s.depth_imperv1,
                                  &s.depth_perv, &s.infil_vol, &ctx.subcatches.stat_evap_vol})
            for (double x : *values)
                if (!std::isfinite(x) || x < 0)
                    throw std::runtime_error("invalid runoff fixture state");
    }
};
inline std::string history(int n, int threads, bool si, int kind) {
    Case f(n, threads, si, kind);
    detail::CompletedSourceArchive history;
    for (int step = 0; step < 9; ++step) {
        f.prepare(step);
        bool threw = false;
        try { f.advance(7. + 11. * (step % 4), step); }
        catch (const std::runtime_error&) { threw = true; }
        if (threw != (kind == 6 && n > 17))
            throw std::runtime_error("unexpected runoff fixture exception");
        f.requireFinite();
        auto frame = f.capture();
        history(frame, threw);
        if (threw) break;
    }
    return history.bytes();
}
} // namespace runoff_parallel_fixture
