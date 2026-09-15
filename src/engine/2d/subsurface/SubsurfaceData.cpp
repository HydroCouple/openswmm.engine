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
 * @file SubsurfaceData.cpp
 * @brief Sizing, storage accounting and token round-trip for the two-zone
 *        groundwater state (G-step 1).
 *
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "SubsurfaceData.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace openswmm::twoD {

void SubsurfaceState::resize(int n, int m) {
    n_cells  = n < 0 ? 0 : n;
    m_layers = m < 1 ? 1 : m;
    const auto nn = static_cast<std::size_t>(n_cells);

    Ks.assign(nn, 1.0e-5);
    zs.assign(nn, 5.0);
    theta_s.assign(nn, 0.45);
    theta_r.assign(nn, 0.10);
    alpha.assign(nn, 2.0);
    psi_b.assign(nn, 0.20);
    lambda.assign(nn, 0.40);
    vg_n.assign(nn, 1.6);
    vg_L.assign(nn, 0.5);
    c_loss.assign(nn, 0.0);
    soil_char.assign(nn, static_cast<int8_t>(SoilChar::RUSSO));
    closure.assign(nn, static_cast<int8_t>(GwClosure::CLOSED_FORM));
    area.assign(nn, 0.0);
    z_bed.assign(nn, 0.0);

    hg.assign(nn, 0.0);
    hu.assign(nn, 0.0);
    theta_sigma.assign(nn * static_cast<std::size_t>(m_layers), 0.0);

    q0_last.assign(nn, 0.0);
    qnode_last.assign(nn, 0.0);
    qlat_last.assign(nn, 0.0);
    qdeep_last.assign(nn, 0.0);
    qet_last.assign(nn, 0.0);
    dunne_last.assign(nn, 0.0);
    qplus_last.assign(nn, 0.0);

    dt_cell.assign(nn, 0.0);
    tier.assign(nn, 0);
    xacc_from_surface.assign(nn, 0.0);
    xacc_to_surface.assign(nn, 0.0);
    // eacc_L/R and nacc are sized by the solver against the edge and node
    // counts, which this struct deliberately does not know.
}

double SubsurfaceState::storage() const noexcept {
    // Saturated: h_g·θ_s·A (drainable + retained; the aquifer is full below
    // the table). Unsaturated: closure A carries `hu` as an equivalent depth
    // directly; closure B integrates the σ layers, which is Σ w_j = Σ θ_j·L·Δσ.
    double s = 0.0;
    const double dsig = 1.0 / static_cast<double>(m_layers <= 0 ? 1 : m_layers);
    for (int i = 0; i < n_cells; ++i) {
        const auto u = static_cast<std::size_t>(i);
        double col = 0.0;
        if (closure[u] == static_cast<int8_t>(GwClosure::SIGMA)) {
            const double L = zs[u] - hg[u];
            const double Ldsig = (L > 0.0 ? L : 0.0) * dsig;
            for (int j = 0; j < m_layers; ++j) {
                col += theta_sigma[static_cast<std::size_t>(j) *
                                       static_cast<std::size_t>(n_cells) + u] *
                       Ldsig;
            }
        } else {
            col = hu[u];
        }
        s += (hg[u] * theta_s[u] + col) * area[u];
    }
    return s;
}

namespace {

std::string upperTrim(const std::string& t) {
    std::string s;
    s.reserve(t.size());
    for (char c : t) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            s.push_back(static_cast<char>(
                std::toupper(static_cast<unsigned char>(c))));
        }
    }
    return s;
}

}  // namespace

const char* soilCharToken(SoilChar s) noexcept {
    switch (s) {
        case SoilChar::RUSSO:         return "RUSSO";
        case SoilChar::GARDNER:       return "GARDNER";
        case SoilChar::BROOKS_COREY:  return "BROOKS_COREY";
        case SoilChar::VAN_GENUCHTEN: return "VAN_GENUCHTEN";
    }
    return "RUSSO";
}

bool parseSoilChar(const std::string& t, SoilChar& s) noexcept {
    const std::string u = upperTrim(t);
    if (u == "RUSSO")                                    { s = SoilChar::RUSSO;         return true; }
    if (u == "GARDNER")                                  { s = SoilChar::GARDNER;       return true; }
    if (u == "BROOKS_COREY" || u == "BROOKSCOREY" ||
        u == "BROOKS-COREY" || u == "BC")                { s = SoilChar::BROOKS_COREY;  return true; }
    if (u == "VAN_GENUCHTEN" || u == "VANGENUCHTEN" ||
        u == "VAN-GENUCHTEN" || u == "VG")               { s = SoilChar::VAN_GENUCHTEN; return true; }
    return false;
}

const char* gwClosureToken(GwClosure c) noexcept {
    switch (c) {
        case GwClosure::AUTO:        return "AUTO";
        case GwClosure::CLOSED_FORM: return "CLOSED_FORM";
        case GwClosure::ENSLAVED:    return "ENSLAVED";
        case GwClosure::SIGMA:       return "SIGMA";
    }
    return "AUTO";
}

bool parseGwClosure(const std::string& t, GwClosure& c) noexcept {
    const std::string u = upperTrim(t);
    if (u == "AUTO")                              { c = GwClosure::AUTO;        return true; }
    if (u == "CLOSED_FORM" || u == "CLOSEDFORM" ||
        u == "CLOSED-FORM" || u == "A")           { c = GwClosure::CLOSED_FORM; return true; }
    if (u == "ENSLAVED")                          { c = GwClosure::ENSLAVED;    return true; }
    if (u == "SIGMA" || u == "COLUMN" || u == "B"){ c = GwClosure::SIGMA;       return true; }
    return false;
}

}  // namespace openswmm::twoD
