// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file      openswmm_api_common.hpp
 * @brief     Internal shared helpers for C API impl files.
 *
 * @details NOT installed — used only by openswmm_*_impl.cpp files.
 *          Provides the opaque-handle cast and common guard macros.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_API_COMMON_HPP
#define OPENSWMM_API_COMMON_HPP

#include "SWMMEngine.hpp"
#include "UnitConversion.hpp"
#include "../../../include/openswmm/engine/openswmm_engine.h"

#include <cstring>
#include <algorithm>
#include <cmath>

/**
 * @brief Recover the internal engine pointer from an opaque public handle.
 *
 * Caller applies handle/lifecycle guards and ensures the object remains alive; a non-NULL arbitrary
 * pointer is not made valid by this cast.
 *
 * @param[in] e Opaque handle originally produced by engine creation, or NULL; no identity/lifetime
 *     validation.
 * @return Same pointer cast to SWMMEngine*, possibly NULL; ownership is unchanged.
 */
static inline openswmm::SWMMEngine* to_engine(SWMM_Engine e) noexcept {
    return static_cast<openswmm::SWMMEngine*>(e);
}

// ============================================================================
// Unit conversion at the C API boundary
// ============================================================================
//
// The engine stores all state in internal SWMM units (ft, cfs, ft³, ft/s, sec).
// The public C API, like legacy swmm5.c, exposes values in the project's
// configured *display* units (selected by [OPTIONS] FLOW_UNITS): getters return
// display units, setters accept display units.
//
//   getter:  display  = internal * UCF(q)        -> to_display()
//   setter:  internal = display  * UCF_inv(q)    -> to_internal()
//
// Setters multiply by the precomputed reciprocal (UCF_inv / Qcf_inv) rather than
// dividing by UCF — no runtime division at any boundary site. The two roundings
// can leave a written value an ulp or two off when it is read back (98 m →
// 97.99999999999999). Setters pick the internal value whose display reads back
// unchanged where one exists (exact_inverse); for the rest, getters snap the
// result to the nearest 15-significant-digit decimal within two ulps
// (snap_display), so a value written with up to 15 significant digits always
// reads back exactly. `q` is a
// openswmm::ucf::Quantity code (LENGTH, FLOW, VOLUME, LANDAREA, RAINFALL,
// EVAPRATE, RAINDEPTH, ...). Templated on the context type so the include of the
// full SimulationContext definition is deferred to the impl translation units.

/**
 * @brief Snap a converted display value to the nearest 15-significant-digit decimal within two ulps.
 *
 * A value written in display units reaches internal units and comes back through two roundings, so
 * it can read back an ulp or two off. No internal value avoids that for every input, so getters
 * snap: a value written with up to 15 significant digits reads back exactly, and no value moves
 * by more than two ulps, which is within the conversion's own error. Magnitudes outside
 * [1e-8, 1e15), zero and non-finite values are returned unchanged.
 *
 * @param[in] v Display value computed from an internal one.
 * @return The snapped value, or @p v.
 */
static inline double snap_display(double v) noexcept {
    static constexpr double kPow10[23] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
                                          1e8,  1e9,  1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
                                          1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    const double a = std::fabs(v);
    if (!(a >= 1e-8 && a < 1e15))
        return v;
    // Scale to a 15-digit integer; correct log10 when it lands on the wrong side of a power of ten.
    int k = 14 - static_cast<int>(std::floor(std::log10(a)));
    double m = std::nearbyint(a * kPow10[k]);
    if (m >= 1e15 && k > 0)
        m = std::nearbyint(a * kPow10[--k]);
    else if (m < 1e14 && k < 22)
        m = std::nearbyint(a * kPow10[++k]);
    const double r = std::copysign(m / kPow10[k], v); // powers up to 1e22 are exact
    const double two_ulps = 2.0 * (std::nextafter(a, HUGE_VAL) - a);
    return std::fabs(r - v) <= two_ulps ? r : v;
}

/**
 * @brief Scale an internal scalar to project display units.
 *
 * Pure multiplicative conversion: absolute temperature requires separate affine offset handling. No
 * ownership or model-state mutation.
 *
 * @param[in] ctx Borrowed context exposing flow-unit options.
 * @param[in] q Quantity code interpreted by ucf::UCF; caller selects the physical quantity.
 * @param[in] v Internal-unit value; no finite/range checks.
 * @return v*UCF(q,options), snapped by at most two ulps when UCF is not 1 (snap_display).
 */
template <class Ctx> static inline double to_display(const Ctx& ctx, int q, double v) noexcept {
    const double f = openswmm::ucf::UCF(q, ctx.options);
    return f == 1.0 ? v : snap_display(v * f);
}

/**
 * @brief The internal value, within one ulp of @p internal, that a getter turns back into @p display.
 *
 * Getters return internal * factor. A setter's internal value comes from @p display through a
 * reciprocal or a division, so internal * factor can differ from @p display by an ulp. This returns
 * @p internal or one of its two floating-point neighbours, whichever reads back as @p display
 * exactly; @p internal when none does (not every display value is reachable) or it is not finite.
 *
 * @param[in] internal Internal value derived from @p display.
 * @param[in] factor Getter's multiplier from internal to display units.
 * @param[in] display Value the caller wrote.
 * @return The chosen internal value.
 */
static inline double exact_inverse(double internal, double factor, double display) noexcept {
    if (internal * factor == display || !std::isfinite(internal))
        return internal;
    const double up = std::nextafter(internal, HUGE_VAL);
    if (up * factor == display)
        return up;
    const double down = std::nextafter(internal, -HUGE_VAL);
    return down * factor == display ? down : internal;
}

/**
 * @brief Scale a display scalar to internal units using a reciprocal factor.
 *
 * Pure multiplicative conversion; absolute temperature requires separate affine handling. Caller is
 * responsible for compatible quantity and unit semantics.
 *
 * @param[in] ctx Borrowed context exposing flow-unit options.
 * @param[in] q Quantity code interpreted by ucf::UCF_inv.
 * @param[in] v Project display-unit value; no finite/range checks.
 * @return v*UCF_inv(q,options), nudged by at most one ulp so to_display() returns v (exact_inverse).
 */
template <class Ctx> static inline double to_internal(const Ctx& ctx, int q, double v) noexcept {
    return exact_inverse(v * openswmm::ucf::UCF_inv(q, ctx.options),
                         openswmm::ucf::UCF(q, ctx.options), v);
}

#define CHECK_HANDLE(e)                                                                            \
    do {                                                                                           \
        if (!(e))                                                                                  \
            return SWMM_ERR_BADHANDLE;                                                             \
    } while (0)
#define CHECK_INDEX(cond)                                                                          \
    do {                                                                                           \
        if (!(cond))                                                                               \
            return SWMM_ERR_BADINDEX;                                                              \
    } while (0)

// ============================================================================
// Lifecycle state guards
// ============================================================================

/// Geometry/topology setters: only BUILDING or OPENED
#define CHECK_GEOMETRY(ctx)                                                                        \
    do {                                                                                           \
        if ((ctx).state != openswmm::EngineState::BUILDING &&                                      \
            (ctx).state != openswmm::EngineState::OPENED)                                          \
            return SWMM_ERR_LIFECYCLE;                                                             \
    } while (0)

/// Model-topology editing (add / remove objects): BUILDING or OPENED.
/// Explicitly forbids INITIALIZED, STARTED, RUNNING, ENDED — once the
/// simulation is armed the object-count invariants are baked into
/// cached solver state that would need a re-init pass to pick up a
/// new node or link.
#define CHECK_EDITABLE(ctx)                                                                        \
    do {                                                                                           \
        if ((ctx).state != openswmm::EngineState::BUILDING &&                                      \
            (ctx).state != openswmm::EngineState::OPENED)                                          \
            return SWMM_ERR_LIFECYCLE;                                                             \
    } while (0)

/// Initial-condition setters: BUILDING, OPENED, or INITIALIZED
#define CHECK_INITIAL_COND(ctx)                                                                    \
    do {                                                                                           \
        if ((ctx).state != openswmm::EngineState::BUILDING &&                                      \
            (ctx).state != openswmm::EngineState::OPENED &&                                        \
            (ctx).state != openswmm::EngineState::INITIALIZED)                                     \
            return SWMM_ERR_LIFECYCLE;                                                             \
    } while (0)

/// Runtime forcing / state override: only RUNNING
#define CHECK_RUNNING(ctx)                                                                         \
    do {                                                                                           \
        if ((ctx).state != openswmm::EngineState::RUNNING)                                         \
            return SWMM_ERR_LIFECYCLE;                                                             \
    } while (0)

/// Read-only access: any state where data exists (not CREATED, CLOSED, or ERROR)
#define CHECK_READABLE(ctx)                                                                        \
    do {                                                                                           \
        if ((ctx).state == openswmm::EngineState::CREATED ||                                       \
            (ctx).state == openswmm::EngineState::CLOSED ||                                        \
            (ctx).state == openswmm::EngineState::ERROR_STATE)                                     \
            return SWMM_ERR_LIFECYCLE;                                                             \
    } while (0)

#endif /* OPENSWMM_API_COMMON_HPP */
