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

#include <algorithm>

namespace openswmm {
namespace street {

void buildTransect(const StreetParams& sp, transect::TransectData& td) {
    // Literal port of legacy transect_createStreetTransect (transect.c:625).
    //
    //   Point 0 = top of backing    Point 3 = bottom of depressed gutter
    //   Point 1 = top of curb       Point 4 = top of depressed gutter (crown)
    //   Point 2 = bottom of curb    Point 5 = street crown wall
    //
    // The two elevations v6 used to get wrong:
    //   y3 = Hdep + Sx*Wg   — the gutter's outer edge is the depression PLUS
    //                         the road cross-slope over the gutter width,
    //   y4 = y3 + Sx*(W-Wg) — and the crown builds on y3, not on Hdep.
    // Omitting Sx*Wg dropped the whole roadway 0.04 ft on a 2 %/2 ft gutter,
    // which is a 24 % area error at a 0.046 ft gutter flow depth (the STREET
    // inlet decks diverged from period 0).
    td.stations.clear();
    td.elevations.clear();

    const double w1 = sp.back_width;                   // backing
    const double w2 = sp.gutter_width;                 // depressed gutter
    const double w3 = sp.width;                        // curb to crown
    const double w4 = w3 - w2;                         // road, less the gutter
    const double y3 = sp.gutter_depression + sp.slope * w2;
    const double y1 = sp.curb_height + sp.gutter_depression;
    const double y4 = y3 + sp.slope * w4;
    // The backing's top is raised to the crown elevation when the crown is
    // higher (legacy `ymax = MAX(ymax, y4)` before Elev[0] is assigned).
    const double ymax = std::max(sp.back_slope * w1 + y1, y4);

    auto pt = [&](double x, double y) {
        td.stations.push_back(x);
        td.elevations.push_back(y);
    };

    pt(0.0,      ymax);   // 0: top of backing
    pt(w1,       y1);     // 1: top of curb
    pt(w1,       0.0);    // 2: bottom of curb (the thalweg)
    pt(w1 + w2,  y3);     // 3: outer edge of the depressed gutter
    pt(w1 + w3,  y4);     // 4: crown

    if (sp.sides == 1) {
        // A half street is closed by a vertical wall at the crown.
        pt(w1 + w3, ymax);
    } else {
        // The right side mirrors the left.
        pt(w1 + w3 + w4,           y3);
        pt(w1 + w3 + w4 + w2,      0.0);
        pt(w1 + w3 + w4 + w2,      y1);
        pt(w1 + w3 + w4 + w2 + w1, ymax);
    }

    // Manning's n and the bank stations (legacy: a street with no backing is
    // ALL main channel, keyed on backWidth — not on whether nBack was given).
    td.n_channel = sp.roughness;
    if (sp.back_width == 0.0) {
        td.n_left  = sp.roughness;
        td.n_right = sp.roughness;
        td.x_left_bank  = td.stations.front();
        td.x_right_bank = td.stations.back();
    } else {
        td.n_left  = sp.back_roughness;
        td.n_right = sp.back_roughness;
        td.x_left_bank  = td.stations[1];
        td.x_right_bank = (sp.sides == 2)
                        ? td.stations[td.stations.size() - 2]
                        : td.stations.back();
    }

    transect::buildTables(td, /*add_end_walls=*/false);
}

} // namespace street
} // namespace openswmm
