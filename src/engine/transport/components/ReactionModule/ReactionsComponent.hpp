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
 * @file ReactionsComponent.hpp
 * @brief The `org.hydrocouple.openswmm.reactions` process component —
 *        phase R1: parse `model.rxn` [REACTION_*] sections into
 *        ReactionData + the species registry.
 *
 * @details R1 scope: structure and references are fully validated (species
 *          declared before use, unique names, no collision with pollutant
 *          names, valid forms/kinds/options); expression BODIES are stored
 *          as source and compiled by phase R2's Tier-1 VM. Sections whose
 *          semantics land in later phases ([REACTION_SOURCES],
 *          [REACTION_PARAMETERS], [REACTION_PATTERNS], [REACTION_REPORT],
 *          [REACTION_SUBCATCHMENTS]) produce a precise
 *          "arrives with plan phase …" error rather than being silently
 *          accepted. Embedded [REACTION_*] sections in the legacy .inp are
 *          honored with a style warning when no external component is
 *          registered; the external file wins wholesale on conflict
 *          (TRANSPORT_IO_PLUGIN_CONFIG_PLAN §3.2).
 *
 * @ingroup engine_transport
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_TRANSPORT_REACTIONS_COMPONENT_HPP
#define OPENSWMM_ENGINE_TRANSPORT_REACTIONS_COMPONENT_HPP

#include <string>
#include <vector>

namespace openswmm {
struct SimulationContext;
}
namespace openswmm::components {
struct ComponentConfigSections;
}

namespace openswmm::transport {

/// Register `org.hydrocouple.openswmm.reactions` with the process-component
/// registry (idempotent; called from SWMMEngine::open before resolution).
void registerReactionsComponent();

/// The 12 [REACTION_*] tags (upper-case, no brackets) — shared by the
/// embedded-fallback handlers and the config parser.
const std::vector<std::string>& reactionSectionTags();

/// Parse + validate a set of reaction sections into ctx.reactions and the
/// species registry. Shared by the component apply hook (external file) and
/// the embedded fallback. Diagnostics are appended to `errors` (fatal).
void applyReactionSections(SimulationContext& ctx,
                           const components::ComponentConfigSections& config,
                           std::vector<std::string>& errors);

/// Embedded fallback (called from SWMMEngine::open after component
/// resolution): if [REACTION_*] sections were found in the legacy .inp,
/// apply them with a style warning — unless an external reactions component
/// was registered, in which case the external file wins wholesale and the
/// embedded sections are reported ignored.
void applyEmbeddedReactionSections(SimulationContext& ctx,
                                   bool external_component_registered,
                                   std::vector<std::string>& errors);

/// Recompile every reaction expression (terms, pipes, tanks) into a fresh
/// flat token pool from the sources in ctx.reactions (E-C2: the one compile
/// path, shared by the file apply and the CRUD API's eager validation —
/// D-RC4, no second compiler). Returns true and sets rx.compiled on
/// success; on failure appends diagnostics and leaves rx.compiled false
/// (the pool/spans hold the partial result — callers roll back or clear).
bool recompileReactionSystem(SimulationContext& ctx,
                             std::vector<std::string>& errors);

}  // namespace openswmm::transport

#endif  // OPENSWMM_ENGINE_TRANSPORT_REACTIONS_COMPONENT_HPP
