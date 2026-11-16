// SPDX-License-Identifier: Apache-2.0

/**
 * @file      SurfaceCapabilities.hpp
 * @brief     The 2D configurations a model requires of a plugin surface solver.
 *
 * @details   One definition shared by the router, the solver factory and the Kokkos solver, so the
 *            core's gate and a plugin's own refusal can never disagree. A plugin advertises
 *            OPENSWMM_GPU_CAP_* bits in its probe; a model is served only by a solver that has
 *            every required bit. Header-only and free of engine state: the plugin compiles it too.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include <cstdint>
#include <string>
#include "GpuPluginAbi.h"
#include "../data/SolverOptions2D.hpp"

namespace openswmm::twoD {

/**
 * @brief Capabilities the in-tree Kokkos explicit marcher implements.
 * @note The core checks a model against this set before loading any plugin, so a model the
 * marcher cannot serve never probes an accelerator. Extend it in the same change that lands and
 * gates a new configuration in ExplicitKokkosSurfaceSolver.
 */
inline constexpr std::uint64_t kKokkosSurfaceCapabilities =
    OPENSWMM_GPU_CAP_LOCAL_INERTIAL | OPENSWMM_GPU_CAP_DIFFUSIVE_WAVE;

/**
 * @brief Capabilities a model requires of a plugin surface solver.
 * @param[in] opts Validated 2D options.
 * @param[in] has_quads The mesh holds at least one quadrilateral cell.
 * @param[in] transport Transport rows (species, age or temperature) are active.
 * @param[in] groundwater A 2D aquifer is coupled to the surface.
 * @param[in] porous_heat Porous ground heat is active.
 * @return Bitwise OR of OPENSWMM_GPU_CAP_* values; never zero.
 */
inline std::uint64_t requiredSurfaceCapabilities(const SolverOptions2D& opts, bool has_quads,
                                                 bool transport, bool groundwater = false,
                                                 bool porous_heat = false) {
    std::uint64_t need = 0;
    switch (opts.momentum) {
    case Momentum2D::LOCAL_INERTIAL:
        need |= OPENSWMM_GPU_CAP_LOCAL_INERTIAL;
        if (transport)
            need |= OPENSWMM_GPU_CAP_TRANSPORT_LI;
        break;
    case Momentum2D::DIFFUSIVE_WAVE:
        need |= OPENSWMM_GPU_CAP_DIFFUSIVE_WAVE;
        if (transport)
            need |= OPENSWMM_GPU_CAP_TRANSPORT_DW;
        break;
    case Momentum2D::FULL_SWE:
        need |= OPENSWMM_GPU_CAP_FULL_SWE;
        if (opts.reconstruction_order == 2)
            need |= OPENSWMM_GPU_CAP_SWE_ORDER2;
        if (transport)
            need |= OPENSWMM_GPU_CAP_TRANSPORT_SWE;
        break;
    }
    // FRONT_REBUILD AUTO is on for FULL_SWE only, where it is part of the scheme.
    if (opts.front_rebuild == 1 && opts.momentum != Momentum2D::FULL_SWE)
        need |= OPENSWMM_GPU_CAP_FRONT_REBUILD;
    if (has_quads)
        need |= OPENSWMM_GPU_CAP_QUADS;
    if (opts.momentumPhysics())
        need |= OPENSWMM_GPU_CAP_MOMENTUM_PHYSICS;
    if (groundwater)
        need |= OPENSWMM_GPU_CAP_GROUNDWATER;
    if (porous_heat)
        need |= OPENSWMM_GPU_CAP_POROUS_HEAT;
    return need;
}

/**
 * @brief Name the capabilities in a mask, for refusal messages.
 * @param[in] mask Bitwise OR of OPENSWMM_GPU_CAP_* values.
 * @return Comma-separated names in bit order; "none" for an empty mask.
 */
inline std::string surfaceCapabilityNames(std::uint64_t mask) {
    static constexpr const char* names[] = {"LOCAL_INERTIAL",
                                            "DIFFUSIVE_WAVE",
                                            "FULL_SWE",
                                            "RECONSTRUCTION_ORDER 2",
                                            "quadrilateral cells",
                                            "Coriolis/turbulence/wall drag",
                                            "FRONT_REBUILD YES",
                                            "transport (LOCAL_INERTIAL)",
                                            "transport (DIFFUSIVE_WAVE)",
                                            "transport (FULL_SWE)",
                                            "coupled groundwater",
                                            "porous ground heat"};
    std::string out;
    for (int bit = 0; bit < 64; ++bit) {
        if (!(mask & (1ull << bit)))
            continue;
        if (!out.empty())
            out += ", ";
        out += bit < 12 ? names[bit] : "capability bit " + std::to_string(bit);
    }
    return out.empty() ? "none" : out;
}

} // namespace openswmm::twoD
