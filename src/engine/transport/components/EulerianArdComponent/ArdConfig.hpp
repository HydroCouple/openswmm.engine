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
 * @file ArdConfig.hpp
 * @brief The `org.hydrocouple.openswmm.transport.ard` process component —
 *        phase E3: parse the dispersion subset of `model.ard` into
 *        SimulationContext::ard_config.
 *
 * @details E3 scope: `[TRANSPORT_OPTIONS] DISPERSION OFF|FISCHER|<value>`
 *          and `[CONDUIT_DISPERSION]` per-conduit overrides. The remaining
 *          [TRANSPORT_OPTIONS] keys (SCALAR_SCHEME/LIMITER/TARGET_DX — the
 *          [OPTIONS] FV_* keys still configure those today) and the shared
 *          1D sections ([TRANSPORT_BOUNDARIES]/[TRANSPORT_SOURCES] E5,
 *          [STORAGE_MIXING] E2b) are refused with precise deferral errors.
 *          Configurations under which the parsed dispersion cannot take
 *          effect (QUALITY_SOLVER other than EULERIAN_ARD, IGNORE_QUALITY,
 *          no pollutants) warn at open rather than running silently —
 *          the R4 silent-bypass lesson applied at introduction time.
 *
 * @see plans/transport/EULERIAN_ARD_TRANSPORT_PLAN.md §4, §6 E3
 * @ingroup engine_transport
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_TRANSPORT_ARD_CONFIG_HPP
#define OPENSWMM_ENGINE_TRANSPORT_ARD_CONFIG_HPP

#include <string>
#include <vector>

namespace openswmm {
struct SimulationContext;
}

namespace openswmm::transport {

/// Register `org.hydrocouple.openswmm.transport.ard` with the
/// process-component registry (idempotent; called from SWMMEngine::open
/// before resolution, next to registerReactionsComponent).
void registerArdComponent();

/// Warn when `[OPTIONS] FV_DISPERSION` is set under QUALITY_SOLVER
/// EULERIAN_ARD. The component's own bypass warnings only cover models that
/// HAVE a transport.ard file; this is the reverse case — the user spelled
/// dispersion the familiar way, selected the engine that now supports it,
/// and got nothing. FV_DISPERSION reaches no solver today (WARN 105 says so
/// under FLOW_ROUTING FV, and nothing says it under DYNWAVE), so E3's
/// arrival is what makes the silence misleading rather than merely inert.
void warnIfFvDispersionKeyIgnored(SimulationContext& ctx);

/// E5a: resolve the raw [TRANSPORT_BOUNDARIES]/[TRANSPORT_SOURCES] rows to
/// node/link/species/timeseries indices. Called from SWMMEngine::open AFTER
/// all process components (and the embedded reactions fallback) have
/// applied, because the rows name MSX species and the reactions component
/// may apply before or after transport.ard in file order. Pushes fatal
/// diagnostics into `errors`; pollutant species are refused (their loading
/// surface is the legacy pathways). Source VALUE rates convert from species
/// mass/s to internal conc·ft³/s here (kLitersPerFt3).
void resolveArdTransportRows(SimulationContext& ctx,
                             std::vector<std::string>& errors);

/// True when the external-load loaders must run even though the model has
/// no pollutants: a [TRANSPORT_BOUNDARIES] row injects
/// `qual_vol_in * concentration`, and `qual_vol_in` is accumulated by those
/// loaders. Without this an MSX-ONLY model — the nh2cl shape, and the one
/// E5a exists to enable — assembles no inflow volume and its boundary
/// delivers exactly nothing.
bool ardBoundariesNeedExternalVolumes(const SimulationContext& ctx);

}  // namespace openswmm::transport

#endif  // OPENSWMM_ENGINE_TRANSPORT_ARD_CONFIG_HPP
