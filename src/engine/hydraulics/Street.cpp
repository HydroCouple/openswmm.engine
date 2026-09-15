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
 * @file Street.cpp
 * @brief Street cross-section — numerically identical to legacy street.c.
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "Street.hpp"

namespace openswmm {
namespace street {

void buildTransect(const StreetParams& sp, transect::TransectData& td) {
    td.stations.clear();
    td.elevations.clear();

    double y_gutter = sp.gutter_depression;
    double y_crown  = y_gutter + sp.slope * (sp.width - sp.gutter_width);

    if (sp.sides == 2) {
        // Full street: left backing → left curb → left gutter → crown → right gutter → right curb → right backing
        double x = 0.0;
        double y_max = sp.curb_height + y_gutter;

        // Left backing
        if (sp.back_width > 0.0) {
            td.stations.push_back(x);
            td.elevations.push_back(y_max + sp.back_slope * sp.back_width);
            x += sp.back_width;
        }

        // Left curb top
        td.stations.push_back(x);
        td.elevations.push_back(y_max);

        // Left curb bottom
        td.stations.push_back(x);
        td.elevations.push_back(0.0);

        // Left gutter bottom
        x += sp.gutter_width;
        td.stations.push_back(x);
        td.elevations.push_back(y_gutter);

        // Crown
        x += (sp.width - sp.gutter_width);
        td.stations.push_back(x);
        td.elevations.push_back(y_crown);

        // Right gutter bottom
        x += (sp.width - sp.gutter_width);
        td.stations.push_back(x);
        td.elevations.push_back(y_gutter);

        // Right curb bottom
        x += sp.gutter_width;
        td.stations.push_back(x);
        td.elevations.push_back(0.0);

        // Right curb top
        td.stations.push_back(x);
        td.elevations.push_back(y_max);

        // Right backing
        if (sp.back_width > 0.0) {
            x += sp.back_width;
            td.stations.push_back(x);
            td.elevations.push_back(y_max + sp.back_slope * sp.back_width);
        }
    } else {
        // Half street: curb → gutter → crown
        double x = 0.0;
        double y_max = sp.curb_height + y_gutter;

        td.stations.push_back(x);
        td.elevations.push_back(y_max);

        td.stations.push_back(x);
        td.elevations.push_back(0.0);

        x += sp.gutter_width;
        td.stations.push_back(x);
        td.elevations.push_back(y_gutter);

        x += (sp.width - sp.gutter_width);
        td.stations.push_back(x);
        td.elevations.push_back(y_crown);
    }

    td.n_channel = sp.roughness;
    td.n_left = sp.back_roughness > 0.0 ? sp.back_roughness : sp.roughness;
    td.n_right = td.n_left;

    transect::buildTables(td);
}

} // namespace street
} // namespace openswmm
