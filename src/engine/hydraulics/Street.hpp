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
 * @file Street.hpp
 * @brief Street cross-section geometry — gutter + road crown + backing.
 *
 * @details Defines a street conduit geometry from curb/gutter/road parameters.
 *          Produces a transect-like tabulated A/R/W geometry.
 *
 * @note Legacy reference: src/legacy/engine/street.c
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_STREET_HPP
#define OPENSWMM_STREET_HPP

#include "Transect.hpp"

namespace openswmm {

namespace street {

struct StreetParams {
    double width          = 0.0;  ///< Distance curb to crown (ft)
    double curb_height    = 0.0;  ///< Curb elevation (ft)
    double slope          = 0.0;  ///< Transverse slope (fraction)
    double roughness      = 0.013;///< Manning's n (road surface)
    double gutter_depression = 0.0; ///< Depressed gutter depth (ft)
    double gutter_width   = 0.0;  ///< Depressed gutter width (ft)
    int    sides          = 2;    ///< 1=half street, 2=full street
    double back_width     = 0.0;  ///< Backing width (ft)
    double back_slope     = 0.0;  ///< Backing slope (fraction)
    double back_roughness = 0.0;  ///< Backing Manning's n
};

/**
 * @brief Build a transect from street parameters.
 *
 * @param sp  Street geometry parameters.
 * @param td  [out] TransectData with station/elevation pairs and tables.
 */
void buildTransect(const StreetParams& sp, transect::TransectData& td);

} // namespace street
} // namespace openswmm

#endif // OPENSWMM_STREET_HPP
