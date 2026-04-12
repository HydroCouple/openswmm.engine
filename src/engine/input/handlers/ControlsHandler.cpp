/**
 * @file ControlsHandler.cpp
 * @brief Section handlers for [CONTROLS] and [REPORT].
 *
 * ### [CONTROLS] format
 * ```
 * RULE R1
 * IF NODE J1 DEPTH > 5.0
 * THEN PUMP P1 STATUS = ON
 * ELSE PUMP P1 STATUS = OFF
 * PRIORITY 5
 *
 * RULE R2
 * IF SIMULATION TIME > 3600
 * THEN ORIFICE O1 SETTING = 0.5
 * ```
 *
 * ### [REPORT] format
 * ```
 * SUBCATCHMENTS  ALL
 * NODES  ALL
 * LINKS  ALL
 * INPUT  NO
 * CONTINUITY  YES
 * FLOWSTATS  YES
 * CONTROLS  NO
 * ```
 *
 * @see Legacy reference: src/solver/input.c — readControl(), readReport()
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "ControlsHandler.hpp"

#include "../Tokenizer.hpp"
#include "../../core/SimulationContext.hpp"
#include "../../data/InfraData.hpp"

#include <string>

namespace openswmm::input {

// ============================================================================
// handle_controls()
// ============================================================================

void handle_controls(SimulationContext& ctx, const std::vector<std::string>& lines) {
    // Each rule starts with "RULE name" and ends before the next "RULE" or
    // end of section. We accumulate lines into a single text block per rule.

    std::string current_block;

    auto flush_block = [&]() {
        if (!current_block.empty()) {
            ctx.control_rules.rule_text.push_back(current_block);
            current_block.clear();
        }
    };

    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.empty()) continue;

        const std::string keyword = Tokenizer::to_upper(tok[0]);

        if (keyword == "RULE") {
            // Flush the previous rule block (if any) and start a new one
            flush_block();
        }

        // Append this line to the current block
        if (!current_block.empty()) {
            current_block += '\n';
        }
        current_block += line;
    }

    // Flush the last rule block
    flush_block();
}

// ============================================================================
// Helpers
// ============================================================================

/// Parse an object-list keyword value: ALL, NONE, or a list of names.
/// Returns 0=NONE, 1=ALL, 2=SOME (names appended to `out`).
static int parse_report_list(const std::vector<std::string>& tok,
                             std::size_t start,
                             std::vector<std::string>& out) {
    if (tok.size() <= start) return 0;

    const std::string val = Tokenizer::to_upper(tok[start]);
    if (val == "NONE") return 0;
    if (val == "ALL")  return 1;

    // Individual names
    for (std::size_t i = start; i < tok.size(); ++i) {
        out.push_back(tok[i]);
    }
    return 2;
}

// ============================================================================
// handle_report()
// ============================================================================

void handle_report(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 2) continue;

        const std::string keyword = Tokenizer::to_upper(tok[0]);

        if (keyword == "SUBCATCHMENTS") {
            ctx.options.rpt_subcatchments = parse_report_list(
                tok, 1, ctx.options.rpt_subcatch_names);
        }
        else if (keyword == "NODES") {
            ctx.options.rpt_nodes = parse_report_list(
                tok, 1, ctx.options.rpt_node_names);
        }
        else if (keyword == "LINKS") {
            ctx.options.rpt_links = parse_report_list(
                tok, 1, ctx.options.rpt_link_names);
        }
        else if (keyword == "INPUT") {
            ctx.options.rpt_input = Tokenizer::parse_boolean(tok[1]);
        }
        else if (keyword == "CONTINUITY") {
            ctx.options.rpt_continuity = Tokenizer::parse_boolean(tok[1]);
        }
        else if (keyword == "FLOWSTATS") {
            ctx.options.rpt_flowstats = Tokenizer::parse_boolean(tok[1]);
        }
        else if (keyword == "CONTROLS") {
            ctx.options.rpt_controls = Tokenizer::parse_boolean(tok[1]);
        }
        else if (keyword == "AVERAGES") {
            ctx.options.rpt_averages = Tokenizer::parse_boolean(tok[1]);
        }
        else if (keyword == "DISABLED") {
            ctx.options.rpt_disabled = Tokenizer::parse_boolean(tok[1]);
        }
    }
}

} /* namespace openswmm::input */
