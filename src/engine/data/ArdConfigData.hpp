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

#include <string>
#include <vector>

namespace openswmm {

/**
 * @brief One raw [TRANSPORT_BOUNDARIES] or [TRANSPORT_SOURCES] row (E5a).
 *
 * @details Stored UNRESOLVED at component-apply time: the rows name MSX
 *          species, and the reactions component may apply before or after
 *          transport.ard in [PROCESS_COMPONENTS] file order. Resolution to
 *          indices happens once, after all components applied
 *          (transport::resolveArdTransportRows, called from
 *          SWMMEngine::open) — failures are fatal there with the row's own
 *          text in the diagnostic.
 */
struct ArdTransportRow {
    std::string element;   ///< node name (boundary) / conduit name (source)
    std::string species;   ///< MSX species name (pollutants refused in E5a)
    bool        is_ts = false;
    double      value = 0.0;   ///< VALUE mode: conc (BC, species units) or
                               ///< mass rate (source, species mass units/s)
    std::string ts_name;       ///< TIMESERIES mode
};

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

    // ---- E5a: raw boundary/source rows (cold; see ArdTransportRow) -------
    std::vector<ArdTransportRow> boundary_rows;
    std::vector<ArdTransportRow> source_rows;

    // ---- E5a: resolved rows (hot; filled by resolveArdTransportRows) -----
    // Boundaries: the node's EXTERNAL INFLOW water carries the MSX species
    // at the given concentration (species units — internal store mass is
    // conc·ft³, so no conversion). Sources: distributed over the conduit's
    // cells, INTERNAL units conc·ft³/s (converted from species-mass/s by
    // dividing by kLitersPerFt3 at resolution).
    std::vector<int>    bc_node;    ///< node index
    std::vector<int>    bc_msx;     ///< ReactionData species index
    std::vector<double> bc_value;   ///< VALUE-mode concentration
    std::vector<int>    bc_ts;      ///< timeseries table index, -1 ⇒ VALUE
    std::vector<int>    src_link;   ///< link index (conduit)
    std::vector<int>    src_msx;
    std::vector<double> src_value;  ///< VALUE-mode rate, internal conc·ft³/s
    std::vector<int>    src_ts;     ///< ts index, -1 ⇒ VALUE (ts values are
                                    ///< species-mass/s; engine converts)
    bool transport_rows_resolved = false;

    // ---- E5b ------------------------------------------------------------
    /// [TRANSPORT_OPTIONS] TARGET_DX: transport-mesh cell length under
    /// non-FV hydraulics (display length units; 0 ⇒ FV_CELL_LENGTH rules).
    /// Ignored with a warning under FLOW_ROUTING FV (the solver mesh
    /// governs) — the plan §8 open item resolved.
    double target_dx = 0.0;
    /// IO3c provenance: true when THIS component's file carried the
    /// SCALAR_SCHEME / LIMITER alias. The live values sit in
    /// ctx.options.fv (the aliases write through at apply), and the
    /// INP writer emits FV_SCALAR_SCHEME/FV_LIMITER only under
    /// FLOW_ROUTING FV — so on a non-FV deck this file is the ONLY
    /// carrier of that state across a save. The serializer emits the
    /// alias (with the CURRENT live value, so API/GUI edits persist)
    /// exactly when the flag says the file owned it; a file that
    /// never spelled the alias never gains one (lesson 196).
    bool sets_scalar_scheme = false;
    bool sets_limiter = false;

    /// [TRANSPORT_OPTIONS] DETAILED_OUTPUT: per-cell CSV sidecar path,
    /// resolved at apply against the CONFIG file's directory (empty ⇒ off).
    std::string detailed_output_path;

    bool any_dispersion() const noexcept {
        return dispersion_mode != ArdDispersionMode::OFF ||
               !conduit_disp_link.empty();
    }
    bool any_transport_rows() const noexcept {
        return !boundary_rows.empty() || !source_rows.empty();
    }
    /// Anything in this config that only the ARD engine can honour — the
    /// bypass-warning predicate (lesson 10: silence is misleading whenever
    /// ANY of it is configured but the engine will not run).
    bool any_engine_content() const noexcept {
        return any_dispersion() || any_transport_rows() ||
               !detailed_output_path.empty() || target_dx > 0.0;
    }
};

/// Litres per cubic foot — the species-mass/s → internal conc·ft³/s source
/// conversion (internal MSX store mass is concentration (mass/L) × ft³).
inline constexpr double kLitersPerFt3 = 28.316846592;

}  // namespace openswmm

#endif  // OPENSWMM_ENGINE_DATA_ARD_CONFIG_DATA_HPP
