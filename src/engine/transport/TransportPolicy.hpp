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
 * @file TransportPolicy.hpp
 * @brief The Domain × Species-class transport contract (OPT plan §5, E2).
 *
 * @details One function decides which species classes run in which domain
 *          and why. Before E2 the answer was re-derived in three places —
 *          `ArdEngine::init`, the LARD `rowLayout()` and the 2D
 *          `SurfaceRouter2D::initialize` row block — and no surface (API or
 *          report) could explain "why is there no 2D quality". Now:
 *
 *          - `network1DEnables()` / `surface2DEnables()` are the allocation-
 *            free class enables the engines size from (LARD calls its layout
 *            per step, so this path must stay cheap).
 *          - `resolve()` builds the full matrix with states and reasons for
 *            the C API (`swmm_get_transport_matrix`), the `.rpt` header and
 *            the GUI hub.
 *
 *          Row ORDER stays where it is: ARD and 2D carry pollutants, MSX,
 *          age, temperature (the SpeciesRegistry order); LARD keeps its
 *          pollutants, age, temperature, MSX order for the bit-inertness
 *          argument recorded on `SpeciesRowLayout`. E2 unifies the ENABLE
 *          decision, not the index arithmetic — flipping LARD's order is a
 *          separate, corpus-gated change.
 *
 *          Policy (OPT plan §5.1): inherit-on. A class enabled at project
 *          level runs in every domain whose solver carries it; per-domain
 *          opt-outs exist only where a key does (2D: `TRANSPORT_*`; 1D:
 *          `IGNORE_QUALITY`; runoff/GW/LID have none). Groundwater has no
 *          transported quality (legacy parity) and reports UNAVAILABLE with
 *          the seam-column explanation rather than staying silent.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_TRANSPORT_POLICY_HPP
#define OPENSWMM_ENGINE_TRANSPORT_POLICY_HPP

#include <string>
#include <vector>

namespace openswmm {
struct SimulationContext;
}

namespace openswmm::transport {

/// Rows of the matrix. Values are the C API's SWMM_TRANSPORT_DOMAIN_* codes.
enum class Domain : int {
    RUNOFF      = 0,  ///< subcatchment runoff + LID
    GROUNDWATER = 1,  ///< legacy per-subcatchment aquifers
    NETWORK_1D  = 2,  ///< LEGACY / EULERIAN_ARD / LAGRANGIAN routing
    SURFACE_2D  = 3,  ///< the 2D mesh
    COUNT       = 4
};

/// Columns of the matrix. Values are the C API's SWMM_TRANSPORT_CLASS_* codes.
enum class SpeciesClass : int {
    POLLUTANTS  = 0,  ///< [POLLUTANTS]
    MSX         = 1,  ///< reactions component species
    AGE         = 2,  ///< __WATER_AGE__
    TEMPERATURE = 3,  ///< __TEMPERATURE__
    COUNT       = 4
};

/// One cell's state. Values are the C API's SWMM_TRANSPORT_* state codes.
enum class CellState : int {
    ENABLED          = 0,  ///< rows are carried in this domain
    DISABLED_BY_USER = 1,  ///< a key turned it off — `reason` names the key
    UNAVAILABLE      = 2   ///< nothing to carry, or the solver lacks it — `reason` says which
};

struct Cell {
    CellState   state  = CellState::UNAVAILABLE;
    int         count  = 0;      ///< rows of this class carried (0 unless ENABLED)
    std::string reason;          ///< key (DISABLED_BY_USER) or explanation (UNAVAILABLE); empty when ENABLED
};

/**
 * @brief Allocation-free class enables for one domain — what the engines
 *        size from. `n_msx` is the reactions component's species count when
 *        MSX is on for the domain; `msx_has_wall` tells ARD / 2D that the
 *        component declares WALL species (their own fallback / warning).
 */
struct ClassEnables {
    int  n_pollut     = 0;
    int  n_msx        = 0;
    bool age          = false;
    bool temperature  = false;
    bool msx_has_wall = false;
    int  total() const noexcept { return n_pollut + n_msx + (age ? 1 : 0) + (temperature ? 1 : 0); }
};

/// 1D network enables: LEGACY / ARD / LARD all size from this.
ClassEnables network1DEnables(const SimulationContext& ctx) noexcept;

/// 2D surface enables: the 1D rule plus the [2D_OPTIONS] TRANSPORT_* keys.
/// Age and temperature are NOT gated by IGNORE_QUALITY here (pre-E2 rule:
/// "IGNORE_QUALITY turns off the pollutant and MSX rows only").
ClassEnables surface2DEnables(const SimulationContext& ctx) noexcept;

/// The canonical row layout (pollutants, MSX, age, temperature) for a set of
/// enables — the SpeciesRegistry order ARD and 2D use.
struct RowLayout {
    int n_pollut = 0;
    int n_msx    = 0;
    int age_row  = -1;
    int temp_row = -1;
    int ns       = 0;
    std::vector<std::string> names;   ///< one per row
};
RowLayout canonicalRows(const SimulationContext& ctx, const ClassEnables& e);

struct Matrix {
    Cell cells[static_cast<int>(Domain::COUNT)][static_cast<int>(SpeciesClass::COUNT)];
    const Cell& at(Domain d, SpeciesClass c) const noexcept {
        return cells[static_cast<int>(d)][static_cast<int>(c)];
    }
    Cell& at(Domain d, SpeciesClass c) noexcept {
        return cells[static_cast<int>(d)][static_cast<int>(c)];
    }
};

/// Build the full matrix (states + reasons). Pure: never writes warnings.
Matrix resolve(const SimulationContext& ctx);

const char* domainName(Domain d) noexcept;
const char* speciesClassName(SpeciesClass c) noexcept;
const char* cellStateName(CellState s) noexcept;

/// The `.rpt` block: a fixed-width table, one line per domain, one column
/// per class, each cell "on(n)" / "off:KEY" / "n/a:reason". Ends with '\n'.
std::string formatReportBlock(const Matrix& m);

}  // namespace openswmm::transport

#endif  // OPENSWMM_ENGINE_TRANSPORT_POLICY_HPP
