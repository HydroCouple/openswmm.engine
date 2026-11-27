// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Caleb Buahin. All rights reserved.
#pragma once

/**
 * @file RunoffProfile.hpp
 * @brief Compile-time gated event counters for the runoff, infiltration, ponded-depth ODE
 * and 2D subsurface kernels.
 *
 * The counters are perf::ProcessCounter entries and print on the [PERF-PROCESS-COUNTS] line
 * under OPENSWMM_PERF=1, like the σ-column histogram. Unlike the per-step ProcessTimers they
 * sit inside per-subarea and per-layer loops, so they are compiled out of the shipping
 * engine: configure with -DOPENSWMM_RUNOFF_PROFILE=ON to have them. Without that define every
 * macro here expands to nothing and OdeScalarProfile is an empty object. None of them reads
 * or writes a numerical value of the model.
 */
#include "ProcessPerf.hpp"

#if defined(OPENSWMM_RUNOFF_PROFILE)
#define OPENSWMM_RUNOFF_PROFILE_COUNT(counter)                                                     \
    ::openswmm::perf::count(::openswmm::perf::ProcessCounter::counter)
#define OPENSWMM_RUNOFF_PROFILE_COUNT_N(counter, n)                                                \
    ::openswmm::perf::count(::openswmm::perf::ProcessCounter::counter,                             \
                            static_cast<unsigned long long>(n))
#else
#define OPENSWMM_RUNOFF_PROFILE_COUNT(counter) ((void)0)
#define OPENSWMM_RUNOFF_PROFILE_COUNT_N(counter, n) ((void)0)
#endif

namespace openswmm::perf {

/// Tally of one scalar Cash-Karp integration (ode::integrateScalar): the accepted steps and
/// the rejected trials, booked with a per-call step histogram when the object goes out of
/// scope. Empty unless OPENSWMM_RUNOFF_PROFILE is defined.
struct OdeScalarProfile {
#if defined(OPENSWMM_RUNOFF_PROFILE)
    unsigned long long steps = 0, rejects = 0;
    bool failed = false;
    void accept() noexcept { ++steps; }
    void reject() noexcept { ++rejects; }
    void fail() noexcept { failed = true; }
    ~OdeScalarProfile() noexcept {
        count(ProcessCounter::runoff_ode_calls);
        count(ProcessCounter::runoff_ode_steps, steps);
        count(ProcessCounter::runoff_ode_rejects, rejects);
        count(steps <= 1   ? ProcessCounter::runoff_ode_hist_1
              : steps == 2 ? ProcessCounter::runoff_ode_hist_2
              : steps <= 4 ? ProcessCounter::runoff_ode_hist_3_4
              : steps <= 8 ? ProcessCounter::runoff_ode_hist_5_8
                           : ProcessCounter::runoff_ode_hist_gt8);
        if (failed)
            count(ProcessCounter::runoff_ode_fail);
    }
#else
    void accept() noexcept {}
    void reject() noexcept {}
    void fail() noexcept {}
#endif
};

} // namespace openswmm::perf
