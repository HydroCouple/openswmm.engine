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
 * @file      InflowsHandler.cpp
 * @brief     Section handlers for [PATTERNS], [INFLOWS], [DWF], [RDII],
 *        [HYDROGRAPHS], and [RDII_DECAY].
 *
 * ### [PATTERNS] format
 * ```
 * ;; Name  Type  Multipliers...
 * P1  MONTHLY  1.0  1.0  1.2  1.3  1.4  1.3  1.2  1.0  0.9  0.8  0.9  1.0
 * P1           0.5  0.6          ;; continuation line (same name, more values)
 * ```
 *
 * ### [INFLOWS] format
 * ```
 * ;; Node  Constituent  TimeSeries  Type  Mfactor  Sfactor  Baseline  Pattern
 * J1       FLOW          TS1         FLOW   1.0      1.0      0.0
 * ```
 *
 * ### [DWF] format
 * ```
 * ;; Node  Constituent  AvgValue  Pat1  Pat2  Pat3  Pat4
 * J1       FLOW          0.001     "Monthly" "" "Hourly"
 * ```
 *
 * ### [RDII] format
 * ```
 * ;; Node  UHgroup  SewerArea
 * J1       UH1      1000.0
 * ```
 *
 * @see Legacy reference: src/solver/input.c
 * @ingroup engine_input
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "InflowsHandler.hpp"

#include "../Tokenizer.hpp"
#include "../../core/SimulationContext.hpp"
#include "../../data/InflowData.hpp"

#include "../InputParseUtils.hpp"

#include <charconv>
#include <string>
#include <algorithm>

namespace openswmm::input {

/**
 * @brief Parse one complete case-insensitive pattern-type token.
 *
 * Constructs an uppercase string despite noexcept, so allocation failure terminates.
 *
 * @param[in] sv Borrowed untrimmed token, uppercased through C-locale character rules.
 * @return 0 MONTHLY, 1 DAILY, 2 HOURLY, 3 WEEKEND, or -1 if unknown.
 */
static int parse_pattern_type(std::string_view sv) noexcept {
    std::string upper(sv);
    for (auto& c : upper)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (upper == "MONTHLY")
        return 0;
    if (upper == "DAILY")
        return 1;
    if (upper == "HOURLY")
        return 2;
    if (upper == "WEEKEND")
        return 3;
    return -1; // not a type keyword
}

/**
 * @brief Find an authored pattern by its case-insensitive identifier.
 *
 * @param[in] pat Borrowed PatternData with coherent name index.
 * @param[in] name Borrowed untrimmed identifier.
 * @return PatternData::find result: index or -1; no factor count/type validation.
 */
static int find_pattern(const PatternData& pat, const std::string& name) noexcept {
    return pat.find(name);
}

// ============================================================================
// handle_patterns()
// ============================================================================

/**
 * @brief Append multiplier factors to named temporal patterns.
 *
 * New rows may explicitly select MONTHLY/DAILY/HOURLY/WEEKEND; omitted type defaults MONTHLY.
 * Repeating a name appends factors, even if a different type token is supplied (stored type remains
 * unchanged). Numeric conversion defaults to one on failure; no expected factor-length,
 * finite/range or duplicate-row validation. Short rows skip.
 *
 * @param[in,out] ctx Authored model stores/diagnostics modified incrementally; existing rows
 *     retained and caller serializes mutations.
 * @param[in] lines Borrowed section data lines tokenized synchronously; stored names/values are
 *     copied, no borrowed line views escape.
 */
void handle_patterns(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 2)
            continue;
        // Name  [Type]  Multipliers...

        const std::string& name = tok[0];
        int existing = find_pattern(ctx.patterns, name);

        // Determine if the second token is a type keyword or a numeric value.
        int type_code = parse_pattern_type(tok[1]);

        if (type_code >= 0 && existing < 0) {
            // New pattern with explicit type keyword
            std::vector<double> facs;
            for (std::size_t i = 2; i < tok.size(); ++i) {
                facs.push_back(to_double(tok[i], 1.0));
            }
            ctx.patterns.add(name, type_code, facs);
        } else if (type_code >= 0 && existing >= 0) {
            // Same name with type keyword again — append multipliers
            auto& facs = ctx.patterns.factors[static_cast<std::size_t>(existing)];
            for (std::size_t i = 2; i < tok.size(); ++i) {
                facs.push_back(to_double(tok[i], 1.0));
            }
        } else if (existing >= 0) {
            // Continuation line — second token is numeric, append all values
            auto& facs = ctx.patterns.factors[static_cast<std::size_t>(existing)];
            for (std::size_t i = 1; i < tok.size(); ++i) {
                facs.push_back(to_double(tok[i], 1.0));
            }
        } else {
            // New pattern without explicit type — default to MONTHLY
            std::vector<double> facs;
            for (std::size_t i = 1; i < tok.size(); ++i) {
                facs.push_back(to_double(tok[i], 1.0));
            }
            ctx.patterns.add(name, 0, facs);
        }
    }
}

// ============================================================================
// handle_inflows()
// ============================================================================

/**
 * @brief Append raw external inflow rows for later reference/constituent resolution.
 *
 * Requires NODE CONSTITUENT SERIES; short rows skip. Retains authored node name even when currently
 * unresolved. Optional type defaults FLOW, mass/series multipliers one and baseline zero, pattern
 * empty; permissive numeric conversion uses those defaults. Raw baseline/series units follow
 * constituent/type until loader conversion; no node creation, table binding, sign validation or
 * runtime source injection.
 *
 * @param[in,out] ctx Authored model stores/diagnostics modified incrementally; existing rows
 *     retained and caller serializes mutations.
 * @param[in] lines Borrowed section data lines tokenized synchronously; stored names/values are
 *     copied, no borrowed line views escape.
 */
void handle_inflows(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 3)
            continue;
        // Node  Constituent  TimeSeries  [Type]  [Mfactor]  [Sfactor]  [Baseline]  [Pattern]

        // The node may be defined in a later section (legacy two-pass parsing
        // is order-independent) — store the raw name and let PostParseResolver
        // re-resolve; a name that never resolves becomes ERR_NAME there.
        const int node_idx = ctx.node_names.find(tok[0]);

        const std::string& constituent = tok[1];
        const std::string& ts_name = tok[2];

        std::string inflow_type =
            (tok.size() > 3) ? Tokenizer::to_upper(tok[3]) : std::string("FLOW");
        double m_factor = (tok.size() > 4) ? to_double(tok[4], 1.0) : 1.0;
        double s_factor = (tok.size() > 5) ? to_double(tok[5], 1.0) : 1.0;
        double baseline = (tok.size() > 6) ? to_double(tok[6], 0.0) : 0.0;
        std::string pat = (tok.size() > 7) ? tok[7] : std::string{};

        ctx.ext_inflows.add(node_idx, constituent, ts_name, inflow_type, m_factor, s_factor,
                            baseline, pat, tok[0]);
    }
}

// ============================================================================
// handle_dwf()
// ============================================================================

/**
 * @brief Append dry-weather inflow definitions with optional pattern names.
 *
 * Requires NODE CONSTITUENT AVERAGE; short rows skip. Retains node name for later resolution and up
 * to four positional pattern names. Stores authored flow/concentration average with permissive
 * numeric parsing; does not resolve constituent, validate patterns or apply loads.
 *
 * @param[in,out] ctx Authored model stores/diagnostics modified incrementally; existing rows
 *     retained and caller serializes mutations.
 * @param[in] lines Borrowed section data lines tokenized synchronously; stored names/values are
 *     copied, no borrowed line views escape.
 */
void handle_dwf(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 3)
            continue;
        // Node  Constituent  AvgValue  [Pat1]  [Pat2]  [Pat3]  [Pat4]

        // Deferred node resolution — see handle_inflows() note.
        const int node_idx = ctx.node_names.find(tok[0]);

        const std::string& constituent = tok[1];
        double avg_value = to_double(tok[2]);

        std::string p1 = (tok.size() > 3) ? tok[3] : std::string{};
        std::string p2 = (tok.size() > 4) ? tok[4] : std::string{};
        std::string p3 = (tok.size() > 5) ? tok[5] : std::string{};
        std::string p4 = (tok.size() > 6) ? tok[6] : std::string{};

        ctx.dwf_inflows.add(node_idx, constituent, avg_value, p1, p2, p3, p4, tok[0]);
    }
}

// ============================================================================
// handle_rdii()
// ============================================================================

/**
 * @brief Append node-to-unit-hydrograph RDII assignments.
 *
 * Requires NODE UH_GROUP SEWER_AREA; short rows skip. Stores raw project land-area units and node
 * name/index, allowing unresolved nodes until post-parse. Does not check group existence,
 * nonnegative/finite area, duplicate assignments or compute inflow.
 *
 * @param[in,out] ctx Authored model stores/diagnostics modified incrementally; existing rows
 *     retained and caller serializes mutations.
 * @param[in] lines Borrowed section data lines tokenized synchronously; stored names/values are
 *     copied, no borrowed line views escape.
 */
void handle_rdii(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 3)
            continue;
        // Node  UHgroup  SewerArea

        // Deferred node resolution — see handle_inflows() note.
        const int node_idx = ctx.node_names.find(tok[0]);

        const std::string& uh_name = tok[1];
        double sewer_area = to_double(tok[2]);

        ctx.rdii_assigns.add(node_idx, uh_name, sewer_area, tok[0]);
    }
}

// ============================================================================
// handle_hydrographs()
// ============================================================================
// [HYDROGRAPHS] section format:
//   UHgroup  RainGage             (gage assignment line)
//   UHgroup  Month  Response  R  T  K  [Dmax  Drecov  Dinit]
//
// Where Month is a month name, 1-12 or "All", Response is "Short"/"Medium"/"Long".
// ============================================================================

/**
 * @brief Append unit-hydrograph gage bindings and month/response parameters.
 *
 * Two tokens bind group to rain gage; response rows require >=6 tokens and malformed item counts
 * append ERR_ITEMS. Months accept ALL (-1), case-insensitive three-letter month prefixes or
 * complete integers 1..12; invalid month appends ERR_KEYWORD. SHORT/MEDIUM/LONG are exact folded
 * tokens; invalid response silently skips. Stores R, peak time hours, K ratio and optional
 * initial-abstraction values in project rain units without numeric/range checks. No model-index
 * resolution or duplicate replacement here.
 *
 * @param[in,out] ctx Authored model stores/diagnostics modified incrementally; existing rows
 *     retained and caller serializes mutations.
 * @param[in] lines Borrowed section data lines tokenized synchronously; stored names/values are
 *     copied, no borrowed line views escape.
 */
void handle_hydrographs(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.empty())
            continue;
        // Legacy rdii_readUnitHydParams: a gage row has 2 items, a response
        // row at least 6; anything else is ERR_ITEMS (rdii.c).
        if (tok.size() == 1 || (tok.size() > 2 && tok.size() < 6)) {
            ctx.errors.push_back(format_error(ERR_ITEMS, tok[0]));
            continue;
        }

        const std::string& uh_name = tok[0];

        // If only 2 tokens: UHgroup  RainGage (gage assignment line)
        if (tok.size() == 2) {
            ctx.unit_hyds.add_gage(uh_name, tok[1]);
            continue;
        }

        // Otherwise: UHgroup  Month  Response  R  T  K  [Dmax  Drecov  Dinit]

        // Legacy datetime_findMonth accepts case-insensitive three-letter
        // month prefixes. A named month must not fall through to ALL.
        int month = -1;
        std::string month_upper = Tokenizer::to_upper(tok[1]);
        if (month_upper != "ALL") {
            static const char* months[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                           "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
            for (int m = 0; m < 12; ++m) {
                if (month_upper.compare(0, 3, months[m]) == 0) {
                    month = m;
                    break;
                }
            }
            // Preserve the engine's numeric-month extension.
            if (month < 0) {
                int numeric_month = 0;
                const auto [end, ec] =
                    std::from_chars(tok[1].data(), tok[1].data() + tok[1].size(), numeric_month);
                if (ec == std::errc{} && end == tok[1].data() + tok[1].size() &&
                    numeric_month >= 1 && numeric_month <= 12)
                    month = numeric_month - 1;
            }
            if (month < 0) {
                ctx.errors.push_back(format_error(ERR_KEYWORD, tok[1]));
                continue;
            }
        }

        // Parse response type: Short=0, Medium=1, Long=2
        int response = -1;
        std::string resp_upper = Tokenizer::to_upper(tok[2]);
        if (resp_upper == "SHORT")
            response = 0;
        else if (resp_upper == "MEDIUM")
            response = 1;
        else if (resp_upper == "LONG")
            response = 2;
        if (response < 0)
            continue;

        UnitHydEntry entry;
        entry.name = uh_name;
        entry.month = month;
        entry.response = response;
        entry.r = to_double(tok[3]);
        entry.t = to_double(tok[4]); // hours
        entry.k = to_double(tok[5]); // tBase/tPeak ratio
        entry.dmax = (tok.size() > 6) ? to_double(tok[6]) : 0.0;
        entry.drecov = (tok.size() > 7) ? to_double(tok[7]) : 0.0;
        entry.dinit = (tok.size() > 8) ? to_double(tok[8]) : 0.0;

        ctx.unit_hyds.add(entry);
    }
}

// ============================================================================
// handle_rdii_decay()
// ============================================================================
// [RDII_DECAY] section format:
//   UHGroup  Response  k_dep  k_0  k_T  T_ref  theta_rec  T_freeze  [SNOW snow_T snow_ddf]
//
// Where Response is "SHORT"/"MEDIUM"/"LONG". Coefficients with units:
//   k_dep     1/(project rain-depth unit) — depletion rate, 1/in for US-unit
//             projects and 1/mm for SI (e.g. 7.62 1/in == 0.3 1/mm); applied
//             to the same units as rainfall depth and Dmax
//   k_0       1/hr   — base recovery rate
//   k_T       1/hr   — thermal recovery rate at T_ref
//   T_ref     deg C  — reference temperature
//   theta_rec 1/degC — temperature sensitivity
//   T_freeze  deg C  — recovery suppressed below this temperature
//
// Optional degree-day snow model — literal keyword SNOW followed by:
//   snow_T    deg C  — rain/snow partition threshold & melt base
//   snow_ddf  project rain-depth unit/degC/day — degree-day melt factor
//             (snow_ddf = 0 with SNOW on is accumulate-only: cold-period
//              precipitation is withheld from the IA model and never melts)
// Rows without the SNOW keyword run the exponential IA model with no snow
// partition (fully backward compatible).
//
// One row per (UH group, response). A group with no row uses the legacy
// linear IA model; a group with one row falls back to linear on the two
// unspecified responses.
// @see docs/RDII_ExpDecay_Implementation.md
// ============================================================================

/**
 * @brief Append exponential RDII abstraction/recovery parameters and optional degree-day snow.
 *
 * Rows under eight tokens, unknown response, negative depletion/base/thermal rates or
 * malformed/negative SNOW melt factor skip silently. k_dep uses reciprocal project rain-depth
 * units, k_0/k_T 1/hour, temperatures degC, sensitivity 1/degC, snow_ddf project depth/degC/day.
 * Numeric parsing is permissive; no full finite-value checks or group existence validation. A
 * non-SNOW extra token does not activate the optional clause; runtime evaluation occurs later.
 *
 * @param[in,out] ctx Authored model stores/diagnostics modified incrementally; existing rows
 *     retained and caller serializes mutations.
 * @param[in] lines Borrowed section data lines tokenized synchronously; stored names/values are
 *     copied, no borrowed line views escape.
 */
void handle_rdii_decay(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 8)
            continue;

        RDIIDecayEntry e;
        e.uh_name = tok[0];

        std::string resp_upper = Tokenizer::to_upper(tok[1]);
        if (resp_upper == "SHORT")
            e.response = 0;
        else if (resp_upper == "MEDIUM")
            e.response = 1;
        else if (resp_upper == "LONG")
            e.response = 2;
        else
            continue;

        e.k_dep = to_double(tok[2]);
        e.k_0 = to_double(tok[3]);
        e.k_T = to_double(tok[4]);
        e.T_ref = to_double(tok[5]);
        e.theta_rec = to_double(tok[6]);
        e.T_freeze = to_double(tok[7]);

        if (e.k_dep < 0.0 || e.k_0 < 0.0 || e.k_T < 0.0)
            continue;

        // Optional degree-day snow model: SNOW snow_T snow_ddf
        if (tok.size() > 8 && Tokenizer::to_upper(tok[8]) == "SNOW") {
            if (tok.size() < 11)
                continue; // malformed snow clause
            e.snow_on = true;
            e.snow_T = to_double(tok[9]);
            e.snow_ddf = to_double(tok[10]);
            if (e.snow_ddf < 0.0)
                continue;
        }

        ctx.rdii_decay.add(e);
    }
}

} /* namespace openswmm::input */
