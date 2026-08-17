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
 * @file ArdConfigData.hpp
 * @brief Parsed configuration of the Eulerian ARD transport component
 *        (`org.hydrocouple.openswmm.transport.ard`, config file `model.ard`).
 *
 * @details Phase E3 of the Eulerian ARD plan activates the dispersion
 *          subset of the component config (D-UT8 placement: these options
 *          live in the external component file, NOT in [OPTIONS]):
 *
 *          ```
 *          [TRANSPORT_OPTIONS]
 *          DISPERSION   OFF | FISCHER | <value>   ; global coefficient model
 *
 *          [CONDUIT_DISPERSION]
 *          <conduit_name>  <value>                ; per-conduit override
 *          ```
 *
 *          Values are entered in project display units (len²/s — m²/s under
 *          SI flow units, ft²/s under US); the engine converts to internal
 *          ft²/s at init, mirroring the FV_DISPERSION treatment in
 *          Router::initFv. Per-conduit overrides win over the global model
 *          in their conduit regardless of mode (including OFF). The
 *          remaining [TRANSPORT_OPTIONS] keys and the shared 1D sections
 *          ([TRANSPORT_BOUNDARIES]/[TRANSPORT_SOURCES]/[STORAGE_MIXING])
 *          arrive with plan phases E5/E2b and are refused with precise
 *          deferral errors until then.
 *
 * @see plans/transport/EULERIAN_ARD_TRANSPORT_PLAN.md §4, §6 E3
 * @see plans/transport/TRANSPORT_IO_PLUGIN_CONFIG_PLAN.md §3.2
 * @ingroup engine_data
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_DATA_ARD_CONFIG_DATA_HPP
#define OPENSWMM_ENGINE_DATA_ARD_CONFIG_DATA_HPP

#include <vector>

namespace openswmm {

/// Global dispersion coefficient model ([TRANSPORT_OPTIONS] DISPERSION).
enum class ArdDispersionMode : int {
    OFF     = 0,  ///< no global dispersion (per-conduit overrides still apply)
    FISCHER = 1,  ///< Fischer et al. (1979): D = 0.011 v²B²/(Y·U*), U* = √(gYS)
    VALUE   = 2,  ///< uniform user value (display units, len²/s)
};

/**
 * @brief Parsed `model.ard` state consumed by ArdEngine::init (phase E3).
 *
 * @details Filled by the transport.ard component apply hook at open; reset
 *          wholesale at the start of each apply so a reopened model never
 *          inherits stale rows. Per-conduit overrides are resolved to LINK
 *          indices at apply time (unknown names are fatal there, where the
 *          row text is still available for the diagnostic).
 */
struct ArdConfigData {
    /// True once a [PROCESS_COMPONENTS] transport.ard registration applied.
    bool configured = false;

    ArdDispersionMode dispersion_mode = ArdDispersionMode::OFF;

    /// Global coefficient for VALUE mode (display units, len²/s).
    double dispersion_value = 0.0;

    // Per-conduit overrides (parallel arrays, SoA): link index + coefficient
    // in display units. Applied to every cell of the conduit; wins over the
    // global model.
    std::vector<int>    conduit_disp_link;
    std::vector<double> conduit_disp_value;

    bool any_dispersion() const noexcept {
        return dispersion_mode != ArdDispersionMode::OFF ||
               !conduit_disp_link.empty();
    }
};

}  // namespace openswmm

#endif  // OPENSWMM_ENGINE_DATA_ARD_CONFIG_DATA_HPP
