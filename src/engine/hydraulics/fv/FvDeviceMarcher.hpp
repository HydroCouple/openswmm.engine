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
 * @file      FvDeviceMarcher.hpp
 * @brief     Interface between ExplicitFvSolver and a device substep marcher.
 *
 * @details The 1D finite volume solver keeps one owner of the model state, the
 *          ledgers and the engine-facing accessors: ExplicitFvSolver. A device
 *          backend does not replace that class. It replaces the SUBSTEP LOOP of
 *          one advance(): the solver hands the marcher its arrays, the marcher
 *          integrates the whole routing step on the device with global
 *          stepping, and hands the arrays back.
 *
 *          The marcher lives in the GPU plugin (src/engine/hydraulics/fv/gpu),
 *          so this header carries no Kokkos and no engine types: plain structs
 *          of raw pointers that cross the plugin boundary as-is.
 *
 *          Scope of the first marcher (the solver checks it and otherwise keeps
 *          the CPU path): first order, forward Euler, slot closure, no species,
 *          no unsteady friction, no implicit pressurized pass, no accepted-water
 *          tracking, structure and boundary forcing held over the routing step.
 *
 * @see plans/workplans/FV1D_KOKKOS_GPU_PROGRAM_2026-11-18.md
 * @ingroup engine_fv
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_FV_DEVICE_MARCHER_HPP
#define OPENSWMM_ENGINE_FV_DEVICE_MARCHER_HPP

#include <cstdint>

#include "FvSubstepArrays.hpp"

namespace openswmm::fv {

/// Static description of one network, valid for the duration of bind().
/// Every pointer is a host array the marcher copies; none is retained.
struct FvDeviceMesh {
    int n_conduits = 0;
    int n_geom = 0;
    int n_node_faces = 0;   ///< node_face_ptr[n_nodes]
    int n_node_structs = 0; ///< node_struct_ptr[n_nodes]
    int n_vol_tbl = 0;      ///< entries of node_vol_tbl

    /// Sizes, mesh, static solver flags and options. Its state and forcing
    /// pointers are ignored here; the marcher points them at its own storage.
    substep::Arrays host{};
};

/// One advance() worth of state and forcing, on the host. The marcher copies
/// the inputs to the device, integrates, and copies the outputs back into
/// the same arrays.
struct FvDeviceAdvance {
    double t_current = 0.0;
    double t_target = 0.0;

    /// State, ledgers and forcing, all host arrays sized as in FvDeviceMesh.
    /// The mesh pointers are ignored.
    substep::Arrays host{};
};

/// What the substep loop reports back, mirroring the CPU loop's locals.
struct FvDeviceResult {
    double t_reached = 0.0;
    long steps = 0;        ///< accepted substeps
    long flux_evals = 0;   ///< full face sweeps × faces, attempts included
    double last_h = 0.0;   ///< last accepted substep (s)
    double min_h = 0.0;    ///< smallest accepted substep of this advance (s)
    double dt_cache = 0.0; ///< what suggested_step() reports
    bool ok = true;        ///< false: `error` says why; state is unspecified
    const char* error = nullptr;
};

/**
 * @brief Device substep loop for the explicit FV network solver.
 *
 * Owned by ExplicitFvSolver through a unique_ptr; the virtual destructor makes
 * deletion across the plugin boundary correct under the shared C++ ABI the GPU
 * plugins already require (GpuPluginAbi.h).
 */
class IFvDeviceMarcher {
public:
    virtual ~IFvDeviceMarcher() = default;

    /// Copy the static network to the device. Called once per initialize().
    virtual void bind(const FvDeviceMesh& mesh) = 0;

    /// Integrate one routing step on the device.
    virtual FvDeviceResult advance(const FvDeviceAdvance& step) = 0;

    /// Short backend label for logs, e.g. "omp" or "cuda".
    virtual const char* label() const noexcept = 0;
};

} // namespace openswmm::fv

#endif // OPENSWMM_ENGINE_FV_DEVICE_MARCHER_HPP
