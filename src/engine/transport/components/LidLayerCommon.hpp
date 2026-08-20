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
 * @file LidLayerCommon.hpp
 * @brief LID layer topology and geometry, shared by every species track.
 *
 * @details A4 wrote these as file-local helpers in `WaterAgeLid.cpp`, when
 *          age was the only species. H5b adds temperature and needs the same
 *          three facts: which layer feeds which, how much water each holds,
 *          and how the per-type groups flatten to unit indices.
 *
 *          They are extracted rather than copied. Copies of exactly this
 *          kind of shared expression are what let `RadiativeExchange`
 *          acquire its own element traversal and thereby its own copy of
 *          H2's divergence (D-H5e, lesson 81) — and the donor map is worse
 *          to duplicate than a traversal, because it encodes eight
 *          hand-checked per-type stacks.
 *
 * @par The donor map is also the PHYSICAL adjacency
 *      For a vertical stack, "which layer feeds which" and "which layers
 *      touch" are the same relation. H5b's conduction term therefore reads
 *      this map rather than introducing a second stacking table that could
 *      disagree with it.
 *
 * @see src/engine/data/LidLayerSpeciesData.hpp
 * @ingroup engine_transport
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_TRANSPORT_LID_LAYER_COMMON_HPP
#define OPENSWMM_ENGINE_TRANSPORT_LID_LAYER_COMMON_HPP

#include <vector>

#include "../../data/LidLayerSpeciesData.hpp"
#include "../../hydrology/LID.hpp"

namespace openswmm::transport {

constexpr int kNL = LidLayerSpeciesState::kLayerCount;

/// -1 means "external to the unit"; -2 means "this type has no such layer".
constexpr int kExternal = -1;
constexpr int kAbsent   = -2;

constexpr int kSurf = static_cast<int>(LidLayer::SURFACE);
constexpr int kPave = static_cast<int>(LidLayer::PAVEMENT);
constexpr int kSoil = static_cast<int>(LidLayer::SOIL);
constexpr int kStor = static_cast<int>(LidLayer::STORAGE);

/// Where each layer's water comes from — and, equivalently, which layers are
/// physically adjacent. Explicit per type rather than inferred: the eight
/// stacks genuinely differ, and the solver is itself written one routine per
/// type.
struct Donors { int surface, pavement, soil, storage; };

Donors donorsFor(lid::LIDType t, bool has_soil);

/// Void-weighted water held in each layer, as a depth per unit area (ft).
/// The four terms are exactly those `LIDSolver::storedVolume()` sums.
void layerVolumes(const lid::LIDGroupSoA& g, std::size_t ui,
                  double (&v)[kNL]);

/// Flat index of each per-type group's first unit; `back() == n_units`.
std::vector<int> buildOffsets(const lid::LIDSolver& solver);

}  // namespace openswmm::transport

#endif  // OPENSWMM_ENGINE_TRANSPORT_LID_LAYER_COMMON_HPP
