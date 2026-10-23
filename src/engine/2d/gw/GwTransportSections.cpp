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
 * @file      GwTransportSections.cpp
 * @brief     U4 — `[GW_*]` parsers, registration and resolution (see header).
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "GwTransportSections.hpp"
#include "core/FileIO.hpp" // issue #7: UTF-8 paths on Windows

#include "../data/MeshData.hpp"
#include "../../core/PathResolver.hpp"
#include "../../core/SimulationContext.hpp"
#include "../../input/InputParseUtils.hpp"
#include "../../input/Tokenizer.hpp"

#include <cctype>
#include <cmath>
#include <unordered_set>
#include <fstream>
#include <string>

namespace openswmm::twoD {

namespace {

/**
 * @brief Compare a string and nonnull null-terminated literal ignoring byte case.
 * @param[in] a Input byte string.
 * @param[in] b Nonnull valid C string.
 * @return True for equal length/case-folded bytes using unsigned-char toupper; no Unicode
 * normalization.
 * @note Read-only, no allocation or state mutation.
 */
bool iequals(const std::string& a, const char* b) {
    std::size_t i = 0;
    for (; i < a.size() && b[i]; ++i)
        if (std::toupper(static_cast<unsigned char>(a[i])) !=
            std::toupper(static_cast<unsigned char>(b[i])))
            return false;
    return i == a.size() && b[i] == '\0';
}

/**
 * @brief Parse an entire finite double token using the engine numeric helper.
 * @param[in] s Complete token, with no trailing text.
 * @param[out] v Parsed number when successful; may be changed on failed conversion.
 * @return True iff conversion consumes all characters without error and gives a finite value.
 * @note No range/unit/sign policy beyond finiteness; read-only token, no model mutation.
 */
bool num(const std::string& s, double& v) {
    auto [p, ec] = openswmm::from_chars_double(s.data(), s.data() + s.size(), v);
    return ec == std::errc{} && p == s.data() + s.size() && std::isfinite(v);
}

/**
 * @brief Parse an int-representable whole-valued finite double token as an integer.
 * @param[in] s Numeric token; caller must ensure value fits int because conversion precedes
 * range test.
 * @param[out] v Assigned cast value after successful finite parse; may change even if
 * nonintegral.
 * @return True iff finite full parse and cast value equals the input double exactly.
 * @note No positivity/mesh bound check; no model mutation or allocation.
 */
bool inum(const std::string& s, int& v) {
    double d = 0.0;
    if (!num(s, d))
        return false;
    v = static_cast<int>(d);
    return static_cast<double>(v) == d;
}

/**
 * @brief `* | TAG <t> | CELL <n>` starting at tokens[0]; advances @p at past it. CELL is 1-BASED
 * in the file (the upper bound is resolveGwTransport's).
 *
 * @details Decode a leading global/tag/cell groundwater row scope and next-token offset.
 * @param[in] tokens Complete row tokens, may be empty for diagnostic return.
 * @param[in] sec Nonnull diagnostic section label.
 * @param[out] scope Parsed GLOBAL/TAG/CELL kind on successful branch.
 * @param[out] tag TAG name when selected, otherwise retained.
 * @param[out] cell Zero-based CELL when selected, otherwise retained; input must be
 * int-representable.
 * @param[out] at First unconsumed token index on success (1 or 2); retained on errors.
 * @return Empty on valid scope, otherwise missing/invalid scope/index diagnostic.
 * @note No mesh upper-bound/tag existence check, unit conversion or physical owner mutation.
 * Allocation can throw; caller initializes untouched fields and stages partial outputs.
 */
std::string parseScope(const std::vector<std::string>& tokens, const char* sec, GwScope& scope,
                       std::string& tag, int& cell, std::size_t& at) {
    if (tokens.empty())
        return std::string(sec) + " empty row";
    if (tokens[0] == "*") {
        scope = GwScope::GLOBAL;
        at = 1;
        return {};
    }
    if (iequals(tokens[0], "TAG")) {
        if (tokens.size() < 2)
            return std::string(sec) + " TAG needs a name";
        scope = GwScope::TAG;
        tag = tokens[1];
        at = 2;
        return {};
    }
    if (iequals(tokens[0], "CELL")) {
        if (tokens.size() < 2)
            return std::string(sec) + " CELL needs an index";
        int n = 0;
        if (!inum(tokens[1], n) || n < 1)
            return std::string(sec) + " invalid CELL index (1-based): " + tokens[1];
        scope = GwScope::CELL;
        cell = n - 1;
        at = 2;
        return {};
    }
    return std::string(sec) + " row must start with CELL, TAG or '*': " + tokens[0];
}

/**
 * @brief A `VALUE | TS-NAME` argument: numeric fills @p v, otherwise @p ts.
 *
 * @details Decode one prescription token as finite numeric value or deferred series name.
 * @param[in] token Complete value/name token.
 * @param[out] v Finite number on successful numeric parse, otherwise zero.
 * @param[out] ts Cleared for numeric form, otherwise replaced with exact token.
 * @note Nonfinite numeric spellings are treated as names, not immediate numeric errors. No
 * series lookup or sign/unit validation; allocation can throw after numeric output change.
 */
void valueOrSeries(const std::string& token, double& v, std::string& ts) {
    if (num(token, v)) {
        ts.clear();
        return;
    }
    v = 0.0;
    ts = token;
}

/**
 * @brief Build a section callback retaining the row parser and borrowed authored-options flag.
 * @param[in] lp Owned nonempty row parser; called once per nonempty tokenized line.
 * @param[in,out] opts Stable borrowed authoring options; must outlive returned handler uses.
 * @return Callable taking writable context and raw section lines; sets authored=true, stops on
 * first row diagnostic with parse error code5/raw line, retains previous rows. Parser/allocation
 * exceptions propagate. No automatic reset, rollback, physical activation or calendar advance;
 * serial authoring.
 */
input::SectionHandler makeGwHandler(std::function<std::string(const std::vector<std::string>&)> lp,
                                    GwTransportOptions& opts) {
    return [lp = std::move(lp), &opts](openswmm::SimulationContext& ctx,
                                       const std::vector<std::string>& lines) {
        opts.authored = true;
        for (const auto& raw : lines) {
            auto tokens = openswmm::input::Tokenizer::tokenize(raw);
            if (tokens.empty())
                continue;
            const std::string err = lp(tokens);
            if (!err.empty()) {
                ctx.error_code = 5; // SWMM_ERR_PARSE
                ctx.error_message = "[GW] " + err + " — line: " + raw;
                return;
            }
        }
    };
}

} // namespace

// ---------------------------------------------------------------------------
// Tokens
// ---------------------------------------------------------------------------

/**
 * @brief Format the canonical groundwater zone keyword.
 * @param[in] z SAT, UNSAT or LAYER enum.
 * @return Static nonowned null-terminated literal, SAT for unknown enum. No allocation/mutation.
 */
const char* gwZoneToken(GwZone z) noexcept {
    switch (z) {
    case GwZone::SAT:
        return "SAT";
    case GwZone::UNSAT:
        return "UNSAT";
    case GwZone::LAYER:
        return "LAYER";
    }
    return "SAT";
}

/**
 * @brief Parse case-insensitive SAT, UNSAT or LAYER keyword.
 * @param[in] token Entire zone keyword.
 * @param[out] z Replaced on recognized keyword, retained on failure.
 * @return True when recognized; no allocation, model mutation or layer-index parsing.
 */
bool parseGwZone(const std::string& token, GwZone& z) noexcept {
    if (iequals(token, "SAT")) {
        z = GwZone::SAT;
        return true;
    }
    if (iequals(token, "UNSAT")) {
        z = GwZone::UNSAT;
        return true;
    }
    if (iequals(token, "LAYER")) {
        z = GwZone::LAYER;
        return true;
    }
    return false;
}

/**
 * @brief Format the canonical groundwater scope keyword.
 * @param[in] s GLOBAL, TAG or CELL enum.
 * @return Static nonowned literal '*', TAG or CELL; unknown enum falls back to '*'. No mutation.
 */
const char* gwScopeToken(GwScope s) noexcept {
    switch (s) {
    case GwScope::GLOBAL:
        return "*";
    case GwScope::TAG:
        return "TAG";
    case GwScope::CELL:
        return "CELL";
    }
    return "*";
}

// ---------------------------------------------------------------------------
// [GW_TRANSPORT_OPTIONS]
// ---------------------------------------------------------------------------

/**
 * @brief Apply one groundwater transport/thermal authoring option without resolving physical
 * owners.
 * @param[in] tokens Tokenized KEY VALUE [ARG...] row; empty is accepted. Most keys ignore
 * surplus tokens; UNSAT_TRANSPORT alone requires exactly two.
 * @param[in,out] o Mutable authoring options (opts in declaration), including thermal_authored
 * tags.
 * @return Empty on recognized valid row/empty input, otherwise grammar/key/value diagnostic.
 * @note Supports transport/conduction/dispersion YES|NO flags, BULK|LAYERED, WATER_ONLY|
 * POROUS_EQUILIBRIUM, thermal layers integer1..128, positive max step s/conductivity
 * W/(m*K)/surface resistance m2*K/W, mixing rules, surface/deep temperature degC or heat flux
 * W/m2/series and optional nonnegative depth m; C_DIFF is finite(0,1]. Stores values/names
 * without scaling or table lookup. Some multi-token thermal branches set fields before
 * validating later values, so diagnostic failure can leave partial options. Allocation can
 * throw. No runtime owner rebuild/physics/clock update; exclusive authoring access required; use
 * a candidate for transactional edits.
 */
std::string parseGwTransportOptionsLine(const std::vector<std::string>& tokens,
                                        GwTransportOptions& o) {
    if (tokens.empty())
        return {};
    if (tokens.size() < 2)
        return "[GW_TRANSPORT_OPTIONS] needs PARAMETER VALUE";
    const std::string& k = tokens[0];
    const std::string& v = tokens[1];

    if (iequals(k, "UNSAT_TRANSPORT")) {
        if (tokens.size() != 2)
            return "[GW_TRANSPORT_OPTIONS] UNSAT_TRANSPORT needs one value";
        if (iequals(v, "BULK"))
            o.unsat_transport = "BULK";
        else if (iequals(v, "LAYERED"))
            o.unsat_transport = "LAYERED";
        else
            return "[GW_TRANSPORT_OPTIONS] UNSAT_TRANSPORT expects BULK or LAYERED";
        return {};
    }

    if (iequals(k, "THERMAL_MODEL")) {
        if (iequals(v, "WATER_ONLY"))
            o.thermal_model = "WATER_ONLY";
        else if (iequals(v, "POROUS_EQUILIBRIUM"))
            o.thermal_model = "POROUS_EQUILIBRIUM";
        else
            return "[GW_TRANSPORT_OPTIONS] THERMAL_MODEL expects WATER_ONLY or POROUS_EQUILIBRIUM";
        o.thermal_authored.insert("THERMAL_MODEL");
        return {};
    }
    if (iequals(k, "THERMAL_LAYERS")) {
        double count = 0;
        if (!num(v, count) || count < 1 || count > 128 || std::floor(count) != count)
            return "[GW_TRANSPORT_OPTIONS] THERMAL_LAYERS must be an integer in [1,128]";
        o.thermal_layers = int(count);
        o.thermal_authored.insert("THERMAL_LAYERS");
        return {};
    }
    if (iequals(k, "LATERAL_CONDUCTION")) {
        if (iequals(v, "YES") || iequals(v, "ON"))
            o.lateral_conduction = true;
        else if (iequals(v, "NO") || iequals(v, "OFF"))
            o.lateral_conduction = false;
        else
            return "[GW_TRANSPORT_OPTIONS] LATERAL_CONDUCTION expects YES or NO";
        o.thermal_authored.insert("LATERAL_CONDUCTION");
        return {};
    }
    if (iequals(k, "SOIL_ET_HEAT")) {
        if (iequals(v, "LATENT_SURFACE"))
            o.soil_et_heat = "LATENT_SURFACE";
        else if (iequals(v, "SENSIBLE_ONLY"))
            o.soil_et_heat = "SENSIBLE_ONLY";
        else
            return "[GW_TRANSPORT_OPTIONS] SOIL_ET_HEAT expects LATENT_SURFACE or SENSIBLE_ONLY";
        o.thermal_authored.insert("SOIL_ET_HEAT");
        return {};
    }
    for (const auto* key : {"THERMAL_MAX_STEP", "WATER_THERMAL_CONDUCTIVITY",
                            "AIR_THERMAL_CONDUCTIVITY", "SURFACE_THERMAL_RESISTANCE"})
        if (iequals(k, key)) {
            double value = 0;
            if (!num(v, value) || !(value > 0))
                return std::string("[GW_TRANSPORT_OPTIONS] ") + key +
                       " must be finite and positive";
            if (iequals(k, "THERMAL_MAX_STEP"))
                o.thermal_max_step = value;
            else if (iequals(k, "WATER_THERMAL_CONDUCTIVITY"))
                o.water_conductivity = value;
            else if (iequals(k, "AIR_THERMAL_CONDUCTIVITY"))
                o.air_conductivity = value;
            else
                o.surface_resistance = value;
            o.thermal_authored.insert(key);
            return {};
        }

    auto yesno = [&](bool& field) -> std::string {
        if (iequals(v, "YES") || iequals(v, "ON"))
            field = true;
        else if (iequals(v, "NO") || iequals(v, "OFF"))
            field = false;
        else
            return "[GW_TRANSPORT_OPTIONS] " + k + ": expected YES|NO, got " + v;
        return {};
    };

    if (iequals(k, "TRANSPORT_POLLUTANTS"))
        return yesno(o.transport_pollutants);
    if (iequals(k, "TRANSPORT_MSX"))
        return yesno(o.transport_msx);
    if (iequals(k, "TRANSPORT_AGE"))
        return yesno(o.transport_age);
    if (iequals(k, "TRANSPORT_TEMPERATURE"))
        return yesno(o.transport_temperature);
    if (iequals(k, "DISPERSION"))
        return yesno(o.dispersion);
    if (iequals(k, "CONDUCTION")) {
        auto error = yesno(o.conduction);
        if (error.empty())
            o.thermal_authored.insert("CONDUCTION");
        return error;
    }

    if (iequals(k, "SURFACE_THERMAL_BC")) {
        if (iequals(v, "ADIABATIC")) {
            o.surface_thermal_bc = "ADIABATIC";
            o.surface_thermal_arg.clear();
            return {};
        }
        if (iequals(v, "AIR") || iequals(v, "SURFACE_WATER")) {
            o.surface_thermal_bc = iequals(v, "AIR") ? "AIR" : "SURFACE_WATER";
            o.surface_thermal_arg.clear();
        } else if (iequals(v, "FIXED") || iequals(v, "TIMESERIES") || iequals(v, "HEAT_FLUX") ||
                   iequals(v, "FLUX_TIMESERIES")) {
            if (tokens.size() < 3)
                return "[GW_TRANSPORT_OPTIONS] SURFACE_THERMAL_BC " + v +
                       " needs a value or series name";
            o.surface_thermal_bc = iequals(v, "FIXED")             ? "FIXED"
                                   : iequals(v, "HEAT_FLUX")       ? "HEAT_FLUX"
                                   : iequals(v, "FLUX_TIMESERIES") ? "FLUX_TIMESERIES"
                                                                   : "TIMESERIES";
            o.surface_thermal_arg = tokens[2];
            if (o.surface_thermal_bc == "FIXED" || o.surface_thermal_bc == "HEAT_FLUX") {
                double t = 0.0;
                if (!num(tokens[2], t))
                    return "[GW_TRANSPORT_OPTIONS] SURFACE_THERMAL_BC FIXED "
                           "needs a temperature, got " +
                           tokens[2];
            }
        } else {
            return "[GW_TRANSPORT_OPTIONS] SURFACE_THERMAL_BC: expected "
                   "AIR|SURFACE_WATER|FIXED <degC>|TIMESERIES <name>, got " +
                   v;
        }
        return {};
    }

    if (iequals(k, "DEEP_THERMAL_BC")) {
        if (iequals(v, "ADIABATIC")) {
            o.deep_thermal_bc = "ADIABATIC";
            o.deep_thermal_arg.clear();
            o.deep_depth = 0;
            return {};
        }
        if (!(iequals(v, "GEOTHERMAL_FLUX") || iequals(v, "FIXED_TEMP") ||
              iequals(v, "TIMESERIES") || iequals(v, "FLUX_TIMESERIES")))
            return "[GW_TRANSPORT_OPTIONS] DEEP_THERMAL_BC: expected "
                   "GEOTHERMAL_FLUX|FIXED_TEMP|TIMESERIES, got " +
                   v;
        if (tokens.size() < 3)
            return "[GW_TRANSPORT_OPTIONS] DEEP_THERMAL_BC needs a value or "
                   "series name";
        o.deep_thermal_bc = iequals(v, "GEOTHERMAL_FLUX")   ? "GEOTHERMAL_FLUX"
                            : iequals(v, "FIXED_TEMP")      ? "FIXED_TEMP"
                            : iequals(v, "FLUX_TIMESERIES") ? "FLUX_TIMESERIES"
                                                            : "TIMESERIES";
        o.deep_thermal_arg = tokens[2];
        if (o.deep_thermal_bc != "TIMESERIES" && o.deep_thermal_bc != "FLUX_TIMESERIES") {
            double d = 0.0;
            if (!num(tokens[2], d))
                return "[GW_TRANSPORT_OPTIONS] DEEP_THERMAL_BC " + o.deep_thermal_bc +
                       " needs a number, got " + tokens[2];
        }
        // Optional trailing `DEPTH <z>`.
        if (tokens.size() >= 5 && iequals(tokens[3], "DEPTH")) {
            double z = 0.0;
            if (!num(tokens[4], z) || z < 0.0)
                return "[GW_TRANSPORT_OPTIONS] DEEP_THERMAL_BC DEPTH must be "
                       ">= 0, got " +
                       tokens[4];
            o.deep_depth = z;
        }
        return {};
    }

    if (iequals(k, "THERMAL_MIXING")) {
        if (iequals(v, "ARITHMETIC"))
            o.thermal_mixing = "ARITHMETIC";
        else if (iequals(v, "GEOMETRIC"))
            o.thermal_mixing = "GEOMETRIC";
        else
            return "[GW_TRANSPORT_OPTIONS] THERMAL_MIXING: expected "
                   "ARITHMETIC|GEOMETRIC, got " +
                   v;
        o.thermal_authored.insert("THERMAL_MIXING");
        return {};
    }

    if (iequals(k, "C_DIFF")) {
        double c = 0.0;
        if (!num(v, c) || c <= 0.0 || c > 1.0)
            return "[GW_TRANSPORT_OPTIONS] C_DIFF must be in (0, 1], got " + v;
        o.c_diff = c;
        return {};
    }

    return "Unknown GW_TRANSPORT_OPTIONS parameter: " + k;
}

// ---------------------------------------------------------------------------
// [GW_TRANSPORT_PARAMS]
// ---------------------------------------------------------------------------

/**
 * @brief Parse one scoped positional groundwater material/dispersion authoring row.
 * @param[in] tokens Empty or (*|TAG name|CELL one-based-index) followed by up to nine optional
 * properties: rho_s kg/m3, c_s J/(kg*K), lambda_s W/(m*K), a_s m2/kg, alpha_L/alpha_T m, D_m/D_v
 * m2/s and geo_flux W/m2 or configured deep temperature degC. '-' retains struct default.
 * @param[in,out] rows Existing rows retained; appends one complete parsed row on success.
 * @return Empty on empty/success, otherwise scope/count/nonfinite/numeric/negative-property
 * error.
 * @note Only ninth property may be negative. Converts CELL to zero-based but defers mesh/tag and
 * physical model validation. Integer tokens must be int-representable; numeric helper casts
 * before its integral check. No unit conversion/owner rebuild; allocation may throw, serial
 * authoring only.
 */
std::string parseGwParamsLine(const std::vector<std::string>& tokens,
                              std::vector<GwParamsRow>& rows) {
    if (tokens.empty())
        return {};
    GwParamsRow r;
    std::size_t at = 0;
    const std::string e = parseScope(tokens, "[GW_TRANSPORT_PARAMS]", r.scope, r.tag, r.cell, at);
    if (!e.empty())
        return e;

    // Positional, all optional after the scope — an omitted column keeps the
    // struct default (which is the plan's example row).
    double* slots[] = {&r.rho_s,   &r.c_s, &r.lambda_s, &r.a_s,     &r.alpha_L,
                       &r.alpha_T, &r.D_m, &r.D_v,      &r.geo_flux};
    const char* names[] = {"rho_s",   "c_s", "lambda_s", "a_s",     "alpha_L",
                           "alpha_T", "D_m", "D_v",      "geo_flux"};
    constexpr int kSlots = 9;
    for (std::size_t k = at; k < tokens.size(); ++k) {
        const std::size_t i = k - at;
        if (i >= static_cast<std::size_t>(kSlots))
            return "[GW_TRANSPORT_PARAMS] too many columns (max 9 after the scope)";
        if (tokens[k] == "-")
            continue; // unset — keep the default
        double v = 0.0;
        if (!num(tokens[k], v))
            return std::string("[GW_TRANSPORT_PARAMS] invalid ") + names[i] + ": " + tokens[k];
        // Only geo_flux (which may also be a deep temperature) is signed.
        if (v < 0.0 && i != 8)
            return std::string("[GW_TRANSPORT_PARAMS] ") + names[i] + " must be >= 0, got " +
                   tokens[k];
        *slots[i] = v;
    }
    rows.push_back(std::move(r));
    return {};
}

// ---------------------------------------------------------------------------
// [GW_SORPTION]
// ---------------------------------------------------------------------------

/**
 * @brief Parse scoped species partition coefficient and optional decay authoring data.
 * @param[in] tokens Empty or scope SPECIES Kd [decay]; Kd finite nonnegative L/kg, decay finite
 * nonnegative 1/day, '-' or omission retains default fallback to pollutant decay. Surplus
 * ignored.
 * @param[in,out] rows Appends one complete row on success, preserves earlier rows on diagnostic
 * failure.
 * @return Empty on success/empty, otherwise scope/arity/numeric/sign diagnostic.
 * @note CELL is authored one-based, stored zero-based, int-representable; species/tag/mesh
 * lookup and kinetic conversion are separate resolution. No physical state mutation; allocation
 * may throw.
 */
std::string parseGwSorptionLine(const std::vector<std::string>& tokens,
                                std::vector<GwSorptionRow>& rows) {
    if (tokens.empty())
        return {};
    GwSorptionRow r;
    std::size_t at = 0;
    const std::string e = parseScope(tokens, "[GW_SORPTION]", r.scope, r.tag, r.cell, at);
    if (!e.empty())
        return e;
    if (tokens.size() < at + 2)
        return "[GW_SORPTION] needs SCOPE SPECIES Kd [decay]";
    r.species = tokens[at];
    if (!num(tokens[at + 1], r.kd) || r.kd < 0.0)
        return "[GW_SORPTION] Kd must be a finite value >= 0, got " + tokens[at + 1];
    if (tokens.size() > at + 2 && tokens[at + 2] != "-") {
        if (!num(tokens[at + 2], r.decay) || r.decay < 0.0)
            return "[GW_SORPTION] decay must be a finite value >= 0, got " + tokens[at + 2];
    }
    rows.push_back(std::move(r));
    return {};
}

// ---------------------------------------------------------------------------
// [GW_INITIAL_QUALITY]
// ---------------------------------------------------------------------------

/**
 * @brief Parse a scoped initial native quality row or retain an external file prescription.
 * @param[in] tokens Empty; FILE path; or scope (SAT|UNSAT|LAYER one-based-layer) SPECIES VALUE.
 * VALUE must be finite; native concentration, age authored hours or temperature degC is retained
 * without unit/sign conversion here. Surplus tokens are ignored.
 * @param[in,out] rows Appends complete inline row only after syntax validation; FILE appends
 * none.
 * @param[in,out] file Replaced by FILE path token only for that branch, otherwise retained.
 * @return Empty on empty/success, otherwise grammar/index/numeric diagnostic.
 * @note CELL becomes zero-based, LAYER remains one-based; integer input must be
 * int-representable. Does not open file or resolve mesh/species/sign/layout; use
 * resolveGwTransport and runtime owners. Allocation may throw; no physics/clock update,
 * exclusive authoring access.
 */
std::string parseGwInitialQualityLine(const std::vector<std::string>& tokens,
                                      std::vector<GwInitialQualityRow>& rows, std::string& file) {
    if (tokens.empty())
        return {};
    if (iequals(tokens[0], "FILE")) {
        if (tokens.size() < 2)
            return "[GW_INITIAL_QUALITY] FILE needs a path";
        file = tokens[1];
        return {};
    }
    GwInitialQualityRow r;
    std::size_t at = 0;
    const std::string e = parseScope(tokens, "[GW_INITIAL_QUALITY]", r.scope, r.tag, r.cell, at);
    if (!e.empty())
        return e;
    if (tokens.size() < at + 3)
        return "[GW_INITIAL_QUALITY] needs SCOPE ZONE SPECIES VALUE";
    if (!parseGwZone(tokens[at], r.zone))
        return "[GW_INITIAL_QUALITY] ZONE must be SAT, UNSAT or LAYER <j>, got " + tokens[at];
    ++at;
    if (r.zone == GwZone::LAYER) {
        if (tokens.size() < at + 3)
            return "[GW_INITIAL_QUALITY] LAYER needs <j> SPECIES VALUE";
        if (!inum(tokens[at], r.layer) || r.layer < 1)
            return "[GW_INITIAL_QUALITY] LAYER index must be >= 1, got " + tokens[at];
        ++at;
    }
    r.species = tokens[at];
    if (!num(tokens[at + 1], r.value))
        return "[GW_INITIAL_QUALITY] VALUE must be a finite number, got " + tokens[at + 1];
    // Temperature (degC) and age (signed, D-NS1) may be negative; a
    // concentration may not. The species kind is only known at resolve, so
    // the sign check lives there.
    rows.push_back(std::move(r));
    return {};
}

// ---------------------------------------------------------------------------
// [GW_BOUNDARY_QUALITY]
// ---------------------------------------------------------------------------

/**
 * @brief Parse a lateral groundwater boundary native concentration or flux prescription.
 * @param[in] tokens Empty or CELL EDGE SPECIES (CONC|TS|MASSFLUX|HEATFLUX) VALUE/NAME. CELL
 * positive one-based becomes zero-based; EDGE nonnegative local zero-based, upper bound
 * deferred. Numeric indices must be int-representable. Surplus tokens ignored.
 * @param[in,out] rows Existing rows retained; appends one complete parsed candidate on success.
 * @return Empty on empty/success, otherwise arity/index/kind diagnostic.
 * @note TS always stores name. Other kinds use finite numeric value or treat any nonnumeric
 * token as series name, including nonfinite numeric spellings. Retains prescribed native
 * concentration, mass-flux or W heat-flux conventions without scaling/sign/mesh/species/table
 * validation. Allocation can throw; resolution/physical acceptance are separate, serial
 * authoring only.
 */
std::string parseGwBoundaryQualityLine(const std::vector<std::string>& tokens,
                                       std::vector<GwBoundaryQualityRow>& rows) {
    if (tokens.empty())
        return {};
    if (tokens.size() < 5)
        return "[GW_BOUNDARY_QUALITY] needs CELL EDGE SPECIES KIND VALUE";
    GwBoundaryQualityRow r;
    int cell = 0;
    if (!inum(tokens[0], cell) || cell < 1)
        return "[GW_BOUNDARY_QUALITY] invalid CELL index (1-based): " + tokens[0];
    r.cell = cell - 1;
    // EDGE is the LOCAL edge of that cell, 0..nv-1 — cell-generic, so the
    // upper bound is the cell's own vertex count, checked at resolve.
    if (!inum(tokens[1], r.edge) || r.edge < 0)
        return "[GW_BOUNDARY_QUALITY] invalid EDGE (0-based, 0..nv-1): " + tokens[1];
    r.species = tokens[2];
    const std::string& kind = tokens[3];
    if (iequals(kind, "CONC"))
        r.kind = "CONC";
    else if (iequals(kind, "TS"))
        r.kind = "TS";
    else if (iequals(kind, "MASSFLUX"))
        r.kind = "MASSFLUX";
    else if (iequals(kind, "HEATFLUX"))
        r.kind = "HEATFLUX";
    else
        return "[GW_BOUNDARY_QUALITY] KIND must be CONC|TS|MASSFLUX|HEATFLUX, "
               "got " +
               kind;
    if (r.kind == "TS")
        r.ts_name = tokens[4];
    else
        valueOrSeries(tokens[4], r.value, r.ts_name);
    rows.push_back(std::move(r));
    return {};
}

// ---------------------------------------------------------------------------
// [GW_SOURCES]
// ---------------------------------------------------------------------------

/**
 * @brief Parse one named scoped groundwater injection/extraction and native constituent
 * prescription.
 * @param[in] tokens Empty or NAME (*|GLOBAL|CELL n|TAG name|XY x y) FLOW value/series [SCALE
 * factor] followed by repeated SPECIES (CONC|MASS) value/series. Flow m3/s is signed positive
 * injection; SCALE finite nonnegative multiplies total flow and mass rates, not concentration.
 * @param[in,out] rows Appends one complete local candidate only after all syntax checks succeed.
 * @return Empty on empty/success, otherwise location/arity/scale/species-kind diagnostic.
 * @note CELL is int-representable one-based converted to zero-based. XY finite coordinates must
 * be in the centroid frame used at resolution. Numeric/series discrimination defers
 * unknown/nonfinite spellings as names; duplicate source/species/table/coverage/native units are
 * resolved later. No physical source activation or clock advance; vector allocation may throw,
 * exclusive authoring access.
 */
std::string parseGwSourcesLine(const std::vector<std::string>& tokens,
                               std::vector<GwSourceRow>& rows) {
    if (tokens.empty())
        return {};
    if (tokens.size() < 4)
        return "[GW_SOURCES] needs NAME (* | CELL n | TAG t | XY x y) FLOW value";
    GwSourceRow r;
    r.name = tokens[0];
    std::size_t at = 1;
    if (tokens[at] == "*" || iequals(tokens[at], "GLOBAL")) {
        r.scope = GwScope::GLOBAL;
        ++at;
    } else if (iequals(tokens[at], "CELL")) {
        int n = 0;
        if (!inum(tokens[at + 1], n) || n < 1)
            return "[GW_SOURCES] invalid CELL index (1-based): " + tokens[at + 1];
        r.scope = GwScope::CELL;
        r.cell = n - 1;
        at += 2;
    } else if (iequals(tokens[at], "TAG")) {
        r.scope = GwScope::TAG;
        r.tag = tokens[at + 1];
        at += 2;
    } else if (iequals(tokens[at], "XY")) {
        if (tokens.size() < at + 3)
            return "[GW_SOURCES] XY needs x y";
        if (!num(tokens[at + 1], r.x) || !num(tokens[at + 2], r.y))
            return "[GW_SOURCES] XY needs two numbers";
        r.scope = GwScope::CELL; // resolved to a cell at resolve
        r.by_xy = true;
        at += 3;
    } else {
        return "[GW_SOURCES] location must be *, CELL, TAG or XY, got " + tokens[at];
    }

    if (at >= tokens.size() || !iequals(tokens[at], "FLOW"))
        return "[GW_SOURCES] '" + r.name + "' needs a FLOW value or series";
    if (at + 1 >= tokens.size())
        return "[GW_SOURCES] FLOW needs a value or series name";
    valueOrSeries(tokens[at + 1], r.flow, r.flow_ts);
    at += 2;

    if (at < tokens.size() && iequals(tokens[at], "SCALE")) {
        if (at + 1 >= tokens.size() || !num(tokens[at + 1], r.scale) || !std::isfinite(r.scale) ||
            r.scale < 0)
            return "[GW_SOURCES] SCALE must be a finite nonnegative multiplier";
        at += 2;
    }
    // Trailing species terms: SPECIES (CONC|MASS) value|ts, repeated.
    while (at < tokens.size()) {
        if (at + 2 >= tokens.size())
            return "[GW_SOURCES] '" + r.name +
                   "' trailing species term needs SPECIES (CONC|MASS) value";
        GwSourceSpeciesTerm t;
        t.species = tokens[at];
        if (iequals(tokens[at + 1], "CONC"))
            t.kind = "CONC";
        else if (iequals(tokens[at + 1], "MASS"))
            t.kind = "MASS";
        else
            return "[GW_SOURCES] species term kind must be CONC or MASS, got " + tokens[at + 1];
        valueOrSeries(tokens[at + 2], t.value, t.ts_name);
        r.species.push_back(std::move(t));
        at += 3;
    }
    rows.push_back(std::move(r));
    return {};
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

/**
 * @brief Register GW authoring callbacks that append into the borrowed model data.
 * @param[in,out] gw Stable model owner captured by reference, must outlive registry callback
 * use.
 * @param[in,out] registry Receives handlers for transport
 * options/params/sorption/initial/boundary quality and sources. Existing registry conflict
 * behavior is delegated.
 * @note Each handler marks authored, tokenizes nonblank lines, stops on first returned error
 * with ctx.error_code=5 and raw-line diagnostic; prior accepted rows remain. Initial/source
 * edits invalidate derived indexes. Captures no physical runtime owner; no
 * reset/rollback/physics. Allocation/callback exceptions propagate and registration may be
 * partial. Serial registry/model authoring required.
 */
void registerGwTransportSections(GwTransportData& gw, input::SectionRegistry& registry) {
    registry.register_custom("GW_TRANSPORT_OPTIONS",
                             makeGwHandler(
                                 [&gw](const std::vector<std::string>& t) {
                                     return parseGwTransportOptionsLine(t, gw.options);
                                 },
                                 gw.options));
    registry.register_custom("GW_TRANSPORT_PARAMS", makeGwHandler(
                                                        [&gw](const std::vector<std::string>& t) {
                                                            return parseGwParamsLine(t, gw.params);
                                                        },
                                                        gw.options));
    registry.register_custom("GW_SORPTION", makeGwHandler(
                                                [&gw](const std::vector<std::string>& t) {
                                                    return parseGwSorptionLine(t, gw.sorption);
                                                },
                                                gw.options));
    registry.register_custom("GW_INITIAL_QUALITY",
                             makeGwHandler(
                                 [&gw](const std::vector<std::string>& t) {
                                     gw.invalidateAuthoringIndexes();
                                     return parseGwInitialQualityLine(t, gw.initial_quality,
                                                                      gw.initial_quality_file);
                                 },
                                 gw.options));
    registry.register_custom("GW_BOUNDARY_QUALITY",
                             makeGwHandler(
                                 [&gw](const std::vector<std::string>& t) {
                                     return parseGwBoundaryQualityLine(t, gw.boundary_quality);
                                 },
                                 gw.options));
    registry.register_custom("GW_SOURCES", makeGwHandler(
                                               [&gw](const std::vector<std::string>& t) {
                                                   gw.invalidateAuthoringIndexes();
                                                   return parseGwSourcesLine(t, gw.sources);
                                               },
                                               gw.options));
}

// ---------------------------------------------------------------------------
// Resolution
// ---------------------------------------------------------------------------

/**
 * @param[in,out] ctx Matching prepared species registry, series names, input path and model
 * tables; logically read by this resolver.
 * @param[in] mesh Final cell count/tags/centroids/local vertex counts, stable and consistently
 * sized.
 * @param[in,out] gw Authoring rows; invalidates caches, loads external initial quality and
 * updates XY source cell assignments while preserving authored location fields.
 * @return All collected file/grammar/cell/tag/species/series/duplicate-source/sign diagnostics;
 * empty when these checks succeed. Does not guarantee complete runtime thermal/native profile
 * qualification.
 * @note Resolves sidecar relative to INP directory; comma/semicolon/tab become separators.
 * Removes previous non-LAYER file rows marked -2, ignores malformed physical first line as
 * header, appends other valid rows while collecting errors. LAYER rows lack that marker and are
 * not deduplicated on re-resolve. XY uses nearest centroid, first minimum wins; caller ensures
 * compatible coordinate units. Signed initial age/temperature accepted here, other negative
 * concentration refused. Does not seed native inventories, activate sources, convert units or
 * advance time. Errors may coexist with loaded/ resolved mutations; allocation/file exceptions
 * can propagate. Exclusive authoring/resolve access.
 */
std::vector<std::string> resolveGwTransport(SimulationContext& ctx, const MeshData& mesh,
                                            GwTransportData& gw) {
    std::vector<std::string> errs;
    if (gw.empty())
        return errs;
    gw.invalidateAuthoringIndexes();

    const int n_cells = mesh.n_cells();

    // `[GW_INITIAL_QUALITY] FILE <csv>`: element,zone,species,value.
    if (!gw.initial_quality_file.empty()) {
        // Drop what a previous resolve loaded (re-resolve is idempotent).
        for (int i = static_cast<int>(gw.initial_quality.size()) - 1; i >= 0; --i)
            if (gw.initial_quality[static_cast<std::size_t>(i)].layer == -2)
                gw.initial_quality.erase(gw.initial_quality.begin() + i);
        const std::string dir = openswmm::io::parentDir(ctx.inp_file_path);
        const std::string path = openswmm::io::resolveRelative(gw.initial_quality_file, dir);
        std::ifstream in(openswmm::io::utf8_path(path));
        if (!in.is_open()) {
            errs.push_back("[GW_INITIAL_QUALITY] FILE '" + gw.initial_quality_file +
                           "' not found or unreadable (" + path + ").");
        } else {
            std::string line;
            int lineno = 0;
            while (std::getline(in, line)) {
                ++lineno;
                for (char& ch : line)
                    if (ch == ',' || ch == ';' || ch == '\t')
                        ch = ' ';
                auto tok = openswmm::input::Tokenizer::tokenize(line);
                if (tok.empty())
                    continue;
                std::vector<GwInitialQualityRow> one;
                std::string ignored;
                const std::string e = parseGwInitialQualityLine(tok, one, ignored);
                if (!e.empty()) {
                    if (lineno == 1)
                        continue; // header
                    errs.push_back("[GW_INITIAL_QUALITY] FILE '" + gw.initial_quality_file +
                                   "' line " + std::to_string(lineno) + ": " + e);
                    continue;
                }
                for (auto& r : one) {
                    if (r.zone != GwZone::LAYER)
                        r.layer = -2; // "from the file"
                    gw.initial_quality.push_back(std::move(r));
                }
            }
        }
    }

    auto checkCell = [&](int cell, const char* sec) {
        if (cell < 0)
            return; // GLOBAL / TAG rows carry -1
        if (cell >= n_cells)
            errs.push_back(std::string(sec) + " CELL " + std::to_string(cell + 1) +
                           " is off the mesh (" + std::to_string(n_cells) + " cells).");
    };
    const std::unordered_set<std::string> knownTags(mesh.tri_tag.begin(), mesh.tri_tag.end());
    auto checkTag = [&](const std::string& tag, const char* sec) {
        if (tag.empty() || knownTags.count(tag))
            return;
        errs.push_back(std::string(sec) + " TAG '" + tag + "' matches no cell tag on the mesh.");
    };
    auto checkSpecies = [&](const std::string& name, const char* sec) {
        if (name == "__WATER_AGE__" || name == "__TEMPERATURE__")
            return true;
        if (ctx.species_registry.find(name) >= 0)
            return true;
        errs.push_back(std::string(sec) + " unknown species '" + name +
                       "' — declare it in [POLLUTANTS] or the reactions "
                       "component, or name __WATER_AGE__ / __TEMPERATURE__.");
        return false;
    };
    auto checkSeries = [&](const std::string& name, const char* sec) {
        if (name.empty())
            return;
        if (ctx.find_timeseries(name) < 0)
            errs.push_back(std::string(sec) + " unknown time series '" + name + "'.");
    };

    for (const auto& r : gw.params) {
        checkCell(r.cell, "[GW_TRANSPORT_PARAMS]");
        checkTag(r.tag, "[GW_TRANSPORT_PARAMS]");
    }
    for (const auto& r : gw.sorption) {
        checkCell(r.cell, "[GW_SORPTION]");
        checkTag(r.tag, "[GW_SORPTION]");
        checkSpecies(r.species, "[GW_SORPTION]");
    }
    for (const auto& r : gw.initial_quality) {
        checkCell(r.cell, "[GW_INITIAL_QUALITY]");
        checkTag(r.tag, "[GW_INITIAL_QUALITY]");
        if (!checkSpecies(r.species, "[GW_INITIAL_QUALITY]"))
            continue;
        const bool signed_row = r.species == "__TEMPERATURE__" || r.species == "__WATER_AGE__";
        if (!signed_row && r.value < 0.0)
            errs.push_back("[GW_INITIAL_QUALITY] negative value for '" + r.species + "'.");
    }
    for (const auto& r : gw.boundary_quality) {
        checkCell(r.cell, "[GW_BOUNDARY_QUALITY]");
        // EDGE 0..nv-1 of ITS cell: a triangle has 3 local edges, a quad 4.
        if (r.cell >= 0 && r.cell < n_cells) {
            const int nv = static_cast<int>(mesh.cell_nv[static_cast<std::size_t>(r.cell)]);
            if (r.edge >= nv)
                errs.push_back("[GW_BOUNDARY_QUALITY] EDGE " + std::to_string(r.edge) +
                               " on CELL " + std::to_string(r.cell + 1) + " (" +
                               std::to_string(nv) +
                               " vertices) — EDGE is "
                               "0.." +
                               std::to_string(nv - 1) + ".");
        }
        checkSpecies(r.species, "[GW_BOUNDARY_QUALITY]");
        checkSeries(r.ts_name, "[GW_BOUNDARY_QUALITY]");
    }
    for (auto& r : gw.sources) {
        if (r.by_xy) {
            // Nearest cell centroid — the same rule the mesh editor uses to
            // place a picked point, and cell-generic (centroids are true-area).
            int best = -1;
            double best_d2 = 0.0;
            for (int c = 0; c < n_cells; ++c) {
                const auto uc = static_cast<std::size_t>(c);
                const double dx = mesh.tri_cx[uc] - r.x;
                const double dy = mesh.tri_cy[uc] - r.y;
                const double d2 = dx * dx + dy * dy;
                if (best < 0 || d2 < best_d2) {
                    best = c;
                    best_d2 = d2;
                }
            }
            if (best < 0)
                errs.push_back("[GW_SOURCES] '" + r.name +
                               "' XY cannot be placed: the mesh has no cells.");
            r.cell = best;
        }
        checkCell(r.cell, "[GW_SOURCES]");
        checkTag(r.tag, "[GW_SOURCES]");
        checkSeries(r.flow_ts, "[GW_SOURCES]");
        for (const auto& t : r.species) {
            checkSpecies(t.species, "[GW_SOURCES]");
            checkSeries(t.ts_name, "[GW_SOURCES]");
        }
    }

    // Duplicate names are an error, not last-wins — the same reasoning
    // [INITIAL_QUALITY] applies to its own duplicates.
    std::unordered_set<std::string> sourceNames;
    for (const auto& source : gw.sources)
        if (!sourceNames.insert(source.name).second)
            errs.push_back("[GW_SOURCES] duplicate source name '" + source.name + "'.");

    return errs;
}

/**
 * @param[in] gw Stable authoring record; default options alone do not count as authored data.
 * @return Empty when data empty, otherwise owned warning text that authored GW rows have no
 * active aquifer kernel. Caller, not this helper, determines whether the kernel is absent.
 * @note Read-only, may allocate; does not validate, disable or mutate authoring/runtime owners.
 */
std::string gwTransportInertWarning(const GwTransportData& gw) {
    if (gw.empty())
        return {};
    // T7.1 (2026-09-20): the two-zone kernel now transports, so this warning
    // is no longer about the release — it is about THIS deck. It fires only
    // when the [GW_*] rows have no kernel to configure, which the caller
    // decides (it knows whether a [2D_AQUIFER] resolved).
    return "[GW_*] subsurface transport is AUTHORED but INERT this run: the "
           "rows configure the two-zone [2D_AQUIFER] kernel, and no "
           "[2D_AQUIFER] resolved on this model. The rows are validated, "
           "kept and written back unchanged.";
}

} // namespace openswmm::twoD
