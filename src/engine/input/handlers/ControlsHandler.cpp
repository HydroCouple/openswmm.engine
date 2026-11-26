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
 * @file      ControlsHandler.cpp
 * @brief     Section handlers for [CONTROLS] and [REPORT].
 *
 * ### [CONTROLS] format
 * ```
 * RULE R1
 * IF NODE J1 DEPTH > 5.0
 * THEN PUMP P1 STATUS = ON
 * ELSE PUMP P1 STATUS = OFF
 * PRIORITY 5
 *
 * ; SIMULATION TIME / CLOCKTIME / TIMEOPEN / TIMECLOSED accept either
 * ; decimal hours or HH:MM:SS — the value is converted to decimal days
 * ; internally to match the simulation clock (legacy controls.c parity).
 * RULE R2
 * IF SIMULATION TIME > 2.5          ; 2.5 hours after start
 * THEN ORIFICE O1 SETTING = 0.5
 *
 * RULE R3
 * IF SIMULATION TIME > 04:30:00     ; same as 4.5 hours
 * THEN ORIFICE O2 SETTING = 0.5
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
 * LINK_STEPS  NO     (OpenSWMM extension: Conduit Time Step Summary)
 * ```
 *
 * @see Legacy reference: src/solver/input.c — readControl(), readReport()
 * @ingroup engine_input
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "ControlsHandler.hpp"

#include "../Tokenizer.hpp"
#include "../../core/SimulationContext.hpp"
#include "../../data/InfraData.hpp"
#include "../InputParseUtils.hpp"

#include <string>
#include <string_view>

namespace openswmm::input {

// ============================================================================
// handle_controls()
// ============================================================================

/**
 * @brief Append raw nonempty control-rule blocks split at RULE rows.
 *
 * Flushes a prior block when a case-insensitive RULE token starts a new one, joining retained rows
 * with actual newlines. Text before the first RULE also forms a block; no grammar validation, name
 * indexing, control compilation or runtime execution here. Allocation failure can leave prior
 * appended blocks.
 *
 * @param[in,out] ctx Context whose control_rules.rule_text receives owned blocks; existing blocks
 *     retained.
 * @param[in] lines Borrowed raw section rows; tokenizer determines empty rows/first keyword,
 *     retained text is copied as authored.
 */
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
        if (tok.empty())
            continue;

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

/**
 * @brief Parse a report selector or append its requested names.
 *
 * Only first selector is folded for comparison; explicit names preserve spelling and are not
 * resolved or deduplicated.
 *
 * @param[in] tok Borrowed tokenized report row.
 * @param[in] start Index of selector/first name; >=size returns NONE.
 * @param[in,out] out Name list appended only for explicit names; never cleared by NONE/ALL.
 * @return 0 for missing/NONE, 1 for ALL, 2 for an explicit name list.
 */
static int parse_report_list(const std::vector<std::string>& tok, std::size_t start,
                             std::vector<std::string>& out) {
    if (tok.size() <= start)
        return 0;

    const std::string val = Tokenizer::to_upper(tok[start]);
    if (val == "NONE")
        return 0;
    if (val == "ALL")
        return 1;

    // Individual names
    for (std::size_t i = start; i < tok.size(); ++i) {
        out.push_back(tok[i]);
    }
    return 2;
}

// ============================================================================
// handle_report()
// ============================================================================

/**
 * @brief Update recognized report flags and per-category object selectors.
 *
 * Parses object ALL/NONE/lists and INPUT/CONTINUITY/FLOWSTATS/CONTROLS/AVERAGES/LINK_STEPS/DISABLED
 * booleans. Explicit lists append names across repeated rows; ALL/NONE do not clear them. Does not
 * resolve names, compile output schema, write files or validate report availability.
 *
 * @param[in,out] ctx Context options modified incrementally; prior flags and name selections
 *     retained unless overwritten.
 * @param[in] lines Borrowed REPORT data lines; rows under two tokens and unknown keys skip
 *     silently.
 */
void handle_report(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 2)
            continue;

        // This engine's own LINK_STEPS is taken as written (it begins with the legacy LINK); any
        // other key is read as legacy report_readOptions reads ReportWords, with findmatch, and is
        // ERROR 205 when it begins with none of them.
        static constexpr std::string_view kReportWords[] = {
            "DISABLED",   "INPUT",     "SUBCATCH", "NODE",     "LINK",
            "CONTINUITY", "FLOWSTATS", "CONTROL",  "AVERAGES", "NODESTATS"};
        static constexpr const char* kReportKeys[] = {
            "DISABLED",   "INPUT",     "SUBCATCHMENTS", "NODES",    "LINKS",
            "CONTINUITY", "FLOWSTATS", "CONTROLS",      "AVERAGES", "NODESTATS"};
        std::string keyword = Tokenizer::to_upper(tok[0]);
        if (keyword != "LINK_STEPS") {
            const int k = legacy_findmatch(tok[0], kReportWords);
            if (k < 0) {
                ctx.errors.push_back(format_error(ERR_KEYWORD, tok[0]));
                continue;
            }
            keyword = kReportKeys[k];
        }
        // A YES/NO value (legacy findmatch over NoYesWords, ERROR 205 otherwise).
        auto flag = [&](bool& out) {
            const int v = legacy_no_yes(tok[1]);
            if (v < 0)
                ctx.errors.push_back(format_error(ERR_KEYWORD, tok[1]));
            else
                out = v == 1;
        };

        if (keyword == "SUBCATCHMENTS") {
            ctx.options.rpt_subcatchments =
                parse_report_list(tok, 1, ctx.options.rpt_subcatch_names);
        } else if (keyword == "NODES") {
            ctx.options.rpt_nodes = parse_report_list(tok, 1, ctx.options.rpt_node_names);
        } else if (keyword == "LINKS") {
            ctx.options.rpt_links = parse_report_list(tok, 1, ctx.options.rpt_link_names);
        } else if (keyword == "INPUT") {
            flag(ctx.options.rpt_input);
        } else if (keyword == "CONTINUITY") {
            flag(ctx.options.rpt_continuity);
        } else if (keyword == "FLOWSTATS") {
            flag(ctx.options.rpt_flowstats);
        } else if (keyword == "CONTROLS") {
            flag(ctx.options.rpt_controls);
        } else if (keyword == "AVERAGES") {
            flag(ctx.options.rpt_averages);
        } else if (keyword == "LINK_STEPS") {
            flag(ctx.options.rpt_link_steps);
        } else if (keyword == "DISABLED") {
            flag(ctx.options.rpt_disabled);
        }
    }
}

} /* namespace openswmm::input */
