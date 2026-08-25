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
 * @file ReactionsWriter.hpp
 * @brief Canonical .rxn text from engine state (E-C3).
 *
 * @details The round-trip IS the spec (D-RC6): serialize -> apply_text ->
 *          serialize must be byte-identical, which is the sync contract the
 *          GUI text tab builds on. Numbers print in shortest-round-trip
 *          form; expressions emit verbatim (unquoted — E-D1 made commas
 *          re-parseable).
 *
 * @ingroup engine_transport
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_REACTIONS_WRITER_HPP
#define OPENSWMM_ENGINE_REACTIONS_WRITER_HPP

#include <string>

namespace openswmm { struct SimulationContext; }

namespace openswmm::transport {

/// Canonical .rxn text for the current reaction system (empty system ->
/// a header comment plus an empty [REACTION_SPECIES] shell is NOT emitted;
/// the result is an empty string when nothing is configured).
std::string serializeReactionSystem(const SimulationContext& ctx);

}  // namespace openswmm::transport

#endif  // OPENSWMM_ENGINE_REACTIONS_WRITER_HPP
