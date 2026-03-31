/**
 * @file InflowsHandler.cpp
 * @brief Section handlers for [PATTERNS], [INFLOWS], [DWF], and [RDII].
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
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "InflowsHandler.hpp"

#include "../Tokenizer.hpp"
#include "../../core/SimulationContext.hpp"
#include "../../data/InflowData.hpp"

#include "../../core/charconv_compat.hpp"

#include <charconv>
#include <string>
#include <algorithm>

namespace openswmm::input {

// ============================================================================
// Helpers
// ============================================================================

static double to_double(std::string_view sv, double def = 0.0) noexcept {
    double v = def;
    openswmm::from_chars_double(sv.data(), sv.data() + sv.size(), v);
    return v;
}

/// Map a pattern type keyword to its integer code.
static int parse_pattern_type(std::string_view sv) noexcept {
    std::string upper(sv);
    for (auto& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (upper == "MONTHLY")  return 0;
    if (upper == "DAILY")    return 1;
    if (upper == "HOURLY")   return 2;
    if (upper == "WEEKEND")  return 3;
    return -1; // not a type keyword
}

/// Find a pattern index by name, or -1 if not found.
static int find_pattern(const PatternData& pat, const std::string& name) noexcept {
    for (int i = 0; i < pat.count(); ++i) {
        if (pat.names[static_cast<std::size_t>(i)] == name) return i;
    }
    return -1;
}

// ============================================================================
// handle_patterns()
// ============================================================================

void handle_patterns(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 2) continue;
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

void handle_inflows(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 3) continue;
        // Node  Constituent  TimeSeries  [Type]  [Mfactor]  [Sfactor]  [Baseline]  [Pattern]

        const int node_idx = ctx.node_names.find(tok[0]);
        if (node_idx < 0) continue;

        const std::string& constituent = tok[1];
        const std::string& ts_name     = tok[2];

        std::string inflow_type = (tok.size() > 3) ? Tokenizer::to_upper(tok[3]) : std::string("FLOW");
        double m_factor  = (tok.size() > 4) ? to_double(tok[4], 1.0) : 1.0;
        double s_factor  = (tok.size() > 5) ? to_double(tok[5], 1.0) : 1.0;
        double baseline  = (tok.size() > 6) ? to_double(tok[6], 0.0) : 0.0;
        std::string pat  = (tok.size() > 7) ? tok[7] : std::string{};

        ctx.ext_inflows.add(node_idx, constituent, ts_name,
                            inflow_type, m_factor, s_factor, baseline, pat);
    }
}

// ============================================================================
// handle_dwf()
// ============================================================================

void handle_dwf(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 3) continue;
        // Node  Constituent  AvgValue  [Pat1]  [Pat2]  [Pat3]  [Pat4]

        const int node_idx = ctx.node_names.find(tok[0]);
        if (node_idx < 0) continue;

        const std::string& constituent = tok[1];
        double avg_value = to_double(tok[2]);

        std::string p1 = (tok.size() > 3) ? tok[3] : std::string{};
        std::string p2 = (tok.size() > 4) ? tok[4] : std::string{};
        std::string p3 = (tok.size() > 5) ? tok[5] : std::string{};
        std::string p4 = (tok.size() > 6) ? tok[6] : std::string{};

        ctx.dwf_inflows.add(node_idx, constituent, avg_value, p1, p2, p3, p4);
    }
}

// ============================================================================
// handle_rdii()
// ============================================================================

void handle_rdii(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 3) continue;
        // Node  UHgroup  SewerArea

        const int node_idx = ctx.node_names.find(tok[0]);
        if (node_idx < 0) continue;

        const std::string& uh_name = tok[1];
        double sewer_area = to_double(tok[2]);

        ctx.rdii_assigns.add(node_idx, uh_name, sewer_area);
    }
}

// ============================================================================
// handle_hydrographs()
// ============================================================================
// [HYDROGRAPHS] section format:
//   UHgroup  RainGage             (gage assignment line)
//   UHgroup  Month  Response  R  T  K  [Dmax  Drecov  Dinit]
//
// Where Month is 1-12 or "All", Response is "Short"/"Medium"/"Long".
// ============================================================================

void handle_hydrographs(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 2) continue;

        const std::string& uh_name = tok[0];

        // If only 2 tokens: UHgroup  RainGage (gage assignment line)
        if (tok.size() == 2) {
            ctx.unit_hyds.add_gage(uh_name, tok[1]);
            continue;
        }

        // Otherwise: UHgroup  Month  Response  R  T  K  [Dmax  Drecov  Dinit]
        if (tok.size() < 6) continue;

        // Parse month: "All" → -1, or numeric 1-12 → 0-based (0-11)
        int month = -1;
        std::string month_upper = Tokenizer::to_upper(tok[1]);
        if (month_upper != "ALL") {
            month = static_cast<int>(to_double(tok[1])) - 1;
            if (month < 0 || month > 11) month = -1;
        }

        // Parse response type: Short=0, Medium=1, Long=2
        int response = -1;
        std::string resp_upper = Tokenizer::to_upper(tok[2]);
        if (resp_upper == "SHORT")       response = 0;
        else if (resp_upper == "MEDIUM") response = 1;
        else if (resp_upper == "LONG")   response = 2;
        if (response < 0) continue;

        UnitHydEntry entry;
        entry.name     = uh_name;
        entry.month    = month;
        entry.response = response;
        entry.r        = to_double(tok[3]);
        entry.t        = to_double(tok[4]);  // hours
        entry.k        = to_double(tok[5]);  // tBase/tPeak ratio
        entry.dmax     = (tok.size() > 6) ? to_double(tok[6]) : 0.0;
        entry.drecov   = (tok.size() > 7) ? to_double(tok[7]) : 0.0;
        entry.dinit    = (tok.size() > 8) ? to_double(tok[8]) : 0.0;

        ctx.unit_hyds.add(entry);
    }
}

} /* namespace openswmm::input */
