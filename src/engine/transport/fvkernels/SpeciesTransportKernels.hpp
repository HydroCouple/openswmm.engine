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
 * @file SpeciesTransportKernels.hpp
 * @brief Shared 1D FV species transport kernels (advection reconstruction,
 *        Zalesak FCT flux limiting, implicit per-chain dispersion).
 *
 * @details Phase E0 of the Unified Transport suite
 *          (plans/transport/EULERIAN_ARD_TRANSPORT_PLAN.md, rev. 2): the
 *          species-transport machinery formerly private to
 *          hydraulics/fv/ExplicitFvSolver is promoted here VERBATIM so the
 *          standalone Eulerian ARD engine and the FV hydraulic solver share
 *          one implementation (master plan D-UT1 as amended). The FV solver
 *          calls these free functions through a thin view over its own
 *          state; behavior is bitwise-identical to the pre-move code by
 *          construction (E0 gate).
 *
 *          The kernels operate on the FV cell mesh types
 *          (`NetworkMeshData` / `NetworkStateData`) and the face records the
 *          hydrodynamic solve produced. They own no state: every array —
 *          including scratch — is supplied by the caller through
 *          @ref SpeciesKernelView, so the same code can later be driven by
 *          the HydraulicsProjection path (plan §3.2) without change.
 *
 * @ingroup engine_transport
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_TRANSPORT_FVKERNELS_SPECIES_TRANSPORT_KERNELS_HPP
#define OPENSWMM_ENGINE_TRANSPORT_FVKERNELS_SPECIES_TRANSPORT_KERNELS_HPP

#include <algorithm>
#include <cmath>
#include <vector>

#include "../../hydraulics/fv/FvKernels.hpp"
#include "../../hydraulics/fv/FvOptions.hpp"
#include "../../hydraulics/fv/NetworkMeshData.hpp"

namespace openswmm::transport::fvkernels {

using openswmm::fv::Limiter;
using openswmm::fv::NetworkMeshData;
using openswmm::fv::NetworkStateData;
using openswmm::fv::ScalarScheme;

/// Slope limiters for the second-order scalar reconstruction. All three are
/// TVD; superbee is the sharpest and can artificially steepen smooth profiles,
/// minmod the most diffusive and most robust. (Moved verbatim from
/// ExplicitFvSolver.cpp; also used by the solver's hydrodynamic
/// reconstructState.)
inline double limitSlope(double a, double b, Limiter lim) {
    if (a * b <= 0.0) return 0.0;                  // extremum ⇒ no slope
    switch (lim) {
        case Limiter::VANLEER:
            return 2.0 * a * b / (a + b);
        case Limiter::SUPERBEE: {
            const double s = (a > 0.0) ? 1.0 : -1.0;
            return s * std::max(std::min(2.0 * std::fabs(a), std::fabs(b)),
                                std::min(std::fabs(a), 2.0 * std::fabs(b)));
        }
        case Limiter::MINMOD:
        default:
            return (std::fabs(a) < std::fabs(b)) ? a : b;
    }
}

/**
 * @brief Non-owning view over everything the species kernels read and write.
 *
 * @details Built per call by the driving solver. Read-only inputs are the
 *          mesh, the hydrodynamic face records of the current substep, and
 *          the derived cell velocity/active flags; `state->cell_phi` is the
 *          transported field; the remaining vectors are caller-owned scratch
 *          sized exactly as ExplicitFvSolver sizes its members (f_phi_l/r and
 *          f_phi_flux are [n_species * n_faces]; the rest are per-face or
 *          per-cell).
 */
struct SpeciesKernelView {
    const NetworkMeshData* mesh = nullptr;
    NetworkStateData*      state = nullptr;

    // Scheme options (subset of FvOptions the kernels consume).
    ScalarScheme scalar_scheme = ScalarScheme::MUSCL;
    Limiter      limiter       = Limiter::MINMOD;
    double       dispersion    = 0.0;
    bool         hllc          = true;

    /// Per-cell dispersion coefficients [n_cells], ft²/s (phase E3:
    /// per-conduit user overrides + FISCHER auto-computation). When null,
    /// the scalar `dispersion` above applies uniformly — the pre-E3 path,
    /// bitwise-unchanged (see the exactness note in dispersionSolve). When
    /// set, `dispersion` is ignored and each interior face uses the
    /// arithmetic mean of its two cells' coefficients.
    const std::vector<double>* cell_dispersion = nullptr;

    // Hydrodynamic face records of the current substep (read-only).
    const std::vector<double>* f_mass  = nullptr;  ///< positivity-scaled mass flux
    const std::vector<double>* f_sstar = nullptr;  ///< HLLC contact speed
    const std::vector<openswmm::fv::kernels::FaceState>* f_state_l = nullptr;
    const std::vector<openswmm::fv::kernels::FaceState>* f_state_r = nullptr;
    const std::vector<openswmm::fv::kernels::FaceFlux>*  f_flux    = nullptr;

    // Derived cell fields (read-only).
    const std::vector<double>* cell_u      = nullptr;  ///< Q/A, dry-guarded
    const std::vector<char>*   cell_active = nullptr;
    const std::vector<int>*    active_faces = nullptr;

    // Caller-owned outputs / scratch.
    std::vector<double>* f_phi_l    = nullptr;  ///< [s*n_faces+f] reconstructed
    std::vector<double>* f_phi_r    = nullptr;  ///<   face values
    std::vector<double>* f_phi_flux = nullptr;  ///< [s*n_faces+f] limited flux
    std::vector<double>* cell_slope = nullptr;
    std::vector<double>* lo_flux    = nullptr;
    std::vector<double>* anti_flux  = nullptr;
    std::vector<double>* td         = nullptr;
    std::vector<double>* anew       = nullptr;
    std::vector<double>* rplus      = nullptr;
    std::vector<double>* rminus     = nullptr;
};

/// Scalar reconstruction — the anti-diffusion layer (FV plan §3.2). Computes
/// limited face values for every species and calls limitSpeciesFluxes to
/// assemble/limit the face species fluxes into `f_phi_flux`.
void reconstructScalars(const SpeciesKernelView& v, double dt);

/// Assemble the face species fluxes and limit them (Zalesak FCT). See the
/// implementation notes: limits the FLUX, not the result, so the discrete
/// maximum principle and exact solute conservation hold together.
void limitSpeciesFluxes(const SpeciesKernelView& v, int species, double dt);

/// Implicit longitudinal dispersion (decision D-FV1): one Thomas tridiagonal
/// solve per cell chain per species; unconditionally stable, removes the
/// Δx²/(2·D_L) explicit constraint. E3: honours `v.cell_dispersion` when set
/// (per-conduit / FISCHER coefficients); scalar `v.dispersion` otherwise.
void dispersionSolve(const SpeciesKernelView& v, double dt);

}  // namespace openswmm::transport::fvkernels

#endif  // OPENSWMM_ENGINE_TRANSPORT_FVKERNELS_SPECIES_TRANSPORT_KERNELS_HPP
