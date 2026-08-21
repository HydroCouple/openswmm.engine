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
 * @file Infil2D.cpp
 * @brief Per-cell infiltration for the 2D overland-flow mesh (track I).
 *
 * @see Infil2D.hpp — the contract, and plan §5.5 (D-I1…D-I6).
 * @ingroup twoD
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "Infil2D.hpp"

#include "../data/MeshData.hpp"
#include "../data/SurfaceStateData.hpp"
#include "../../core/SimulationOptions.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string_view>

namespace openswmm::twoD {

namespace {

// ---------------------------------------------------------------------------
// SI ⇄ legacy-kernel units
// ---------------------------------------------------------------------------
// The 2D solver is SI (m, m/s); infil::*_getInfil is ft, ft/s for every
// project (US or SI) — see §5.5.1. Conversion happens only here, at the call
// boundary, so the kernels keep bit-parity with legacy infil.c.

constexpr double kFeetPerMeter  = 3.280839895013123;  ///< m → ft
constexpr double kMetersPerFoot = 0.3048;             ///< ft → m (exact)

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

/// Case-insensitive comparison (file tokens are case-insensitive on input).
bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::toupper(static_cast<unsigned char>(a[i]))
            != std::toupper(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

/// Validate one row. @p who names the offending cell or tag in every message.
bool validateRow(const Infil2DRow& row, const std::string& who, std::string& err) {
    if (!row.has_method) return true;

    // D-I4: LOST is the only destination this release routes.
    if (row.dest != Infil2DDest::LOST) {
        err = "2D infiltration " + who + ": destination "
            + infil2DDestToken(row.dest)
            + " is not supported in this release; see plan §5.5.4";
        return false;
    }

    switch (row.method) {
        case InfilModel::HORTON:
        case InfilModel::MOD_HORTON:
            if (row.p[0] < 0.0 || row.p[1] < 0.0) {
                err = "2D infiltration " + who
                    + ": Horton rates f0 and fmin must be >= 0";
                return false;
            }
            break;

        case InfilModel::GREEN_AMPT:
        case InfilModel::MOD_GREEN_AMPT:
            if (row.p[1] < 0.0) {
                err = "2D infiltration " + who
                    + ": Green-Ampt conductivity Ks must be >= 0";
                return false;
            }
            if (row.p[2] < 0.0 || row.p[2] > 1.0) {
                err = "2D infiltration " + who
                    + ": Green-Ampt initial moisture deficit IMD must be in 0..1";
                return false;
            }
            break;

        case InfilModel::CURVE_NUM:
            if (row.p[0] < 1.0 || row.p[0] > 100.0) {
                err = "2D infiltration " + who
                    + ": curve number must be in 1..100";
                return false;
            }
            break;

        case InfilModel::CONSTANT:
            if (row.p[0] < 0.0) {
                err = "2D infiltration " + who + ": constant rate must be >= 0";
                return false;
            }
            break;

        default:
            err = "2D infiltration " + who + ": unknown infiltration method";
            return false;
    }
    return true;
}

} // namespace

// ============================================================================
// Free helpers
// ============================================================================

bool parseInfil2DMethod(const std::string& token, InfilModel& method,
                        bool& has_method) {
    if (iequals(token, "NONE")) {
        method     = InfilModel::HORTON;   // meaningless when has_method is false
        has_method = false;
        return true;
    }

    has_method = true;
    if (iequals(token, "HORTON"))              { method = InfilModel::HORTON;         return true; }
    if (iequals(token, "MODIFIED_HORTON"))     { method = InfilModel::MOD_HORTON;     return true; }
    if (iequals(token, "GREEN_AMPT"))          { method = InfilModel::GREEN_AMPT;     return true; }
    if (iequals(token, "MODIFIED_GREEN_AMPT")) { method = InfilModel::MOD_GREEN_AMPT; return true; }
    if (iequals(token, "CURVE_NUMBER"))        { method = InfilModel::CURVE_NUM;      return true; }
    if (iequals(token, "CONSTANT"))            { method = InfilModel::CONSTANT;       return true; }

    has_method = false;
    return false;
}

const char* infil2DMethodToken(const Infil2DRow& row) {
    if (!row.has_method) return "NONE";
    switch (row.method) {
        case InfilModel::HORTON:         return "HORTON";
        case InfilModel::MOD_HORTON:     return "MODIFIED_HORTON";
        case InfilModel::GREEN_AMPT:     return "GREEN_AMPT";
        case InfilModel::MOD_GREEN_AMPT: return "MODIFIED_GREEN_AMPT";
        case InfilModel::CURVE_NUM:      return "CURVE_NUMBER";
        case InfilModel::CONSTANT:       return "CONSTANT";
    }
    return "NONE";
}

bool parseInfil2DDest(const std::string& token, Infil2DDest& dest) {
    if (iequals(token, "LOST"))             { dest = Infil2DDest::LOST;             return true; }
    if (iequals(token, "SUBCATCH_AQUIFER")) { dest = Infil2DDest::SUBCATCH_AQUIFER; return true; }
    if (iequals(token, "AQUIFER_2D"))       { dest = Infil2DDest::AQUIFER_2D;       return true; }
    return false;
}

const char* infil2DDestToken(Infil2DDest dest) {
    switch (dest) {
        case Infil2DDest::LOST:             return "LOST";
        case Infil2DDest::SUBCATCH_AQUIFER: return "SUBCATCH_AQUIFER";
        case Infil2DDest::AQUIFER_2D:       return "AQUIFER_2D";
    }
    return "LOST";
}

int infil2DParamCount(InfilModel method) {
    switch (method) {
        case InfilModel::HORTON:
        case InfilModel::MOD_HORTON:     return 5;  // f0 fmin decay dry_time Fmax
        case InfilModel::GREEN_AMPT:
        case InfilModel::MOD_GREEN_AMPT: return 3;  // S Ks IMD
        case InfilModel::CURVE_NUM:      return 3;  // CN (unused) dry_time
        case InfilModel::CONSTANT:       return 1;  // rate
    }
    return 0;
}

// ============================================================================
// Infil2D
// ============================================================================

bool Infil2D::resolve(const MeshData& mesh, const SimulationOptions& opts,
                      std::string& err) {
    const int  nt   = mesh.n_triangles();
    const auto nt_u = static_cast<std::size_t>(nt);

    resolved_.assign(nt_u, Infil2DRow{});
    prov_.assign(nt_u, Infil2DProvenance::NONE);
    cum_depth_.assign(nt_u, 0.0);
    horton_.clear();
    grnampt_.clear();
    curvenum_.clear();
    active_ = false;

    // D-I1: the cadence is INFIL_STEP, falling back to the project WET_STEP
    // (SimulationOptions::wet_step is already in seconds).
    step_seconds_ = (options_.infil_step > 0.0) ? options_.infil_step : opts.wet_step;

    // --- D-I3 precedence, least specific first: '*' → tag → override -------

    const Infil2DDefault* star = nullptr;
    for (const auto& d : defaults_) {
        if (!validateRow(d.row, "tag '" + d.tag + "'", err)) return false;
        if (d.tag == "*") star = &d;
    }

    if (star != nullptr && star->row.has_method) {
        for (std::size_t i = 0; i < nt_u; ++i) {
            resolved_[i] = star->row;
            prov_[i]     = Infil2DProvenance::STAR;
        }
    }

    // A tag row spelled NONE deliberately clears the '*' default for its cells.
    const std::size_t n_tagged = std::min(nt_u, mesh.tri_tag.size());
    for (const auto& d : defaults_) {
        if (d.tag == "*") continue;
        for (std::size_t i = 0; i < n_tagged; ++i) {
            if (mesh.tri_tag[i] != d.tag) continue;
            if (d.row.has_method) {
                resolved_[i] = d.row;
                prov_[i]     = Infil2DProvenance::TAG;
            } else {
                resolved_[i] = Infil2DRow{};
                prov_[i]     = Infil2DProvenance::NONE;
            }
        }
    }

    for (const auto& o : overrides_) {
        if (o.tri < 0 || o.tri >= nt) {
            err = "2D infiltration cell " + std::to_string(o.tri + 1)
                + ": triangle index out of range (mesh has "
                + std::to_string(nt) + " triangles)";
            return false;
        }
        if (!validateRow(o.row, "cell " + std::to_string(o.tri + 1), err)) return false;

        const auto ui = static_cast<std::size_t>(o.tri);
        if (o.row.has_method) {
            resolved_[ui] = o.row;
            prov_[ui]     = Infil2DProvenance::OVERRIDE;
        } else {
            resolved_[ui] = Infil2DRow{};
            prov_[ui]     = Infil2DProvenance::NONE;
        }
    }

    // --- kernel state, from the PROJECT-UNIT parameters (§5.5.1) -----------

    for (std::size_t i = 0; i < nt_u; ++i) {
        const Infil2DRow& r = resolved_[i];
        if (!r.has_method) continue;
        active_ = true;

        switch (r.method) {
            case InfilModel::HORTON:
            case InfilModel::MOD_HORTON:
                if (horton_.empty()) horton_.assign(nt_u, infil::HortonState{});
                infil::horton_init(horton_[i], r.p[0], r.p[1], r.p[2], r.p[3],
                                   r.p[4], opts);
                break;

            case InfilModel::CONSTANT:
                // A constant rate is a degenerate Horton (f0 == fmin, no decay,
                // no regeneration), so horton_init performs exactly the
                // in/hr|mm/hr → ft/s conversion this method needs and
                // horton_[i].fmin carries the rate. No extra storage, and the
                // slot reads sanely if anyone inspects it.
                if (horton_.empty()) horton_.assign(nt_u, infil::HortonState{});
                infil::horton_init(horton_[i], r.p[0], r.p[0], 0.0, 0.0, 0.0, opts);
                break;

            case InfilModel::GREEN_AMPT:
            case InfilModel::MOD_GREEN_AMPT:
                if (grnampt_.empty()) grnampt_.assign(nt_u, infil::GreenAmptState{});
                infil::grnampt_init(grnampt_[i], r.p[0], r.p[1], r.p[2], opts);
                break;

            case InfilModel::CURVE_NUM:
                // Drying time is p[2] — the third positional column, matching
                // legacy curvenum_setParams() and Runoff.cpp:252-253.
                if (curvenum_.empty()) curvenum_.assign(nt_u, infil::CurveNumState{});
                infil::curvenum_init(curvenum_[i], r.p[0], r.p[2]);
                break;

            default:
                err = "2D infiltration cell " + std::to_string(i + 1)
                    + ": unknown infiltration method";
                return false;
        }
    }

    // Nothing resolved: drop back to the unconfigured fast path so the
    // bitwise-regression gate (I7) holds with no arrays allocated.
    if (!active_) {
        resolved_.clear();
        prov_.clear();
        cum_depth_.clear();
    }

    return true;
}

void Infil2D::updateRates(const MeshData& mesh, SurfaceStateData& state, double dt) {
    (void)mesh;
    if (!active_ || dt <= 0.0) return;

    // §5.5.2: advance EVERY model-carrying cell, active or not — a cell that
    // sat inactive must not present full initial capacity when rain arrives.
    const std::size_t n = std::min(resolved_.size(), state.infil_rate.size());
    for (std::size_t i = 0; i < n; ++i) {
        const Infil2DRow& r = resolved_[i];
        if (!r.has_method) continue;

        const double precip_ft = state.rainfall[i] * kFeetPerMeter;
        const double depth_ft  = state.depth[i]    * kFeetPerMeter;

        double f_ftsec = 0.0;
        switch (r.method) {
            case InfilModel::HORTON:
                f_ftsec = infil::horton_getInfil(horton_[i], precip_ft, depth_ft, dt);
                break;
            case InfilModel::MOD_HORTON:
                f_ftsec = infil::modHorton_getInfil(horton_[i], precip_ft, depth_ft, dt);
                break;
            case InfilModel::GREEN_AMPT:
            case InfilModel::MOD_GREEN_AMPT:
                // The modified variant is selected by the enum, not a bool.
                f_ftsec = infil::grnampt_getInfil(grnampt_[i], precip_ft, depth_ft,
                                                  dt, r.method);
                break;
            case InfilModel::CURVE_NUM:
                // Runoff.cpp:428 folds inter-subarea runon into the depth
                // argument (and passes rainfall alone as the rate); a mesh cell
                // has no runon, so the ponded depth passes through unchanged.
                f_ftsec = infil::curvenum_getInfil(curvenum_[i], precip_ft, depth_ft, dt);
                break;
            case InfilModel::CONSTANT:
                f_ftsec = infil::constant_getInfil(horton_[i].fmin, precip_ft,
                                                   depth_ft, dt);
                break;
            default:
                break;
        }

        const double rate_si = std::max(0.0, f_ftsec * kMetersPerFoot);
        state.infil_rate[i] = rate_si;
        cum_depth_[i] += rate_si * dt;
    }
}

void Infil2D::reset() {
    defaults_.clear();
    overrides_.clear();
    options_ = Infil2DOptions{};

    active_       = false;
    step_seconds_ = 0.0;

    resolved_.clear();
    prov_.clear();
    cum_depth_.clear();
    horton_.clear();
    grnampt_.clear();
    curvenum_.clear();
}

} // namespace openswmm::twoD
