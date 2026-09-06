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
 * @file InputReader.cpp
 * @brief Implementation of the top-level SWMM .inp file reader.
 *
 * @see InputReader.hpp
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "InputReader.hpp"
#include "Tokenizer.hpp"
#include "../core/ErrorCodes.hpp"
#include "../core/PerfTimers.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>

namespace openswmm::input {

// ============================================================================
// Constructor
// ============================================================================

InputReader::InputReader(SectionRegistry& registry)
    : registry_(registry)
{}

// ============================================================================
// read()
// ============================================================================

bool InputReader::read(const std::string& path, SimulationContext& ctx) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        ctx.error_code    = 2;  // public SWMM_ERR_INPFILE (was 1 = NOMEM)
        ctx.error_message = "InputReader: cannot open file '" + path + "'";
        ctx.state         = EngineState::ERROR_STATE;
        return false;
    }
    ctx.inp_file_path = path;
    return read_stream(ifs, ctx);
}

// ============================================================================
// read_stream()
// ============================================================================

bool InputReader::read_stream(std::istream& stream, SimulationContext& ctx) {
    lines_read_ = 0;
    skipped_sections_.clear();

    std::string            current_tag;     // e.g. "OPTIONS"
    std::vector<std::string> section_lines; // accumulated data lines

    auto flush_section = [&]() {
        if (!current_tag.empty()) {
            perf::ScopedTimer _pt(perf::sec_read_dispatch);
            dispatch_section(current_tag, section_lines, ctx);
            section_lines.clear();
        }
    };

    std::string raw_line;
    while (std::getline(stream, raw_line)) {
        ++lines_read_;

        // Strip \r (Windows CRLF)
        if (!raw_line.empty() && raw_line.back() == '\r') {
            raw_line.pop_back();
        }

        // Trim first so we can inspect the leading character without allocating.
        std::string_view trimmed_raw = Tokenizer::trim(raw_line);

        // Skip truly empty lines
        if (trimmed_raw.empty()) continue;

        // Detect section header. A real header is a clean "[NAME]" token on its
        // own line (an optional trailing comment is allowed). A line that merely
        // begins with '[' but carries other text after the ']' — e.g. a wrapped
        // [TITLE] line that happens to start with a bracketed word — is NOT a
        // header; it falls through and is kept as section data.
        if (trimmed_raw.front() == '[') {
            std::string_view header = Tokenizer::trim(Tokenizer::strip_comment(trimmed_raw));
            if (!header.empty() && header.back() == ']') {
                flush_section();
                current_tag = parse_section_header(header);
                continue;
            }
            // Not a clean header — fall through to data-line handling below.
        }

        // Comment line (starts with ';') — keep verbatim inside a section so
        // section handlers can associate per-object comments with objects.
        if (trimmed_raw.front() == ';') {
            if (!current_tag.empty())
                section_lines.emplace_back(std::string(trimmed_raw));
            continue;
        }

        // Data line. [TITLE] is free-form prose, so preserve it verbatim —
        // legacy SWMM stores the whole title line (input.c readTitle), and a
        // ';' in a sentence is description, not a comment. Stripping it here
        // truncated titles mid-sentence in the .rpt echo. Every other section
        // strips the inline trailing comment.
        std::string_view stripped = (current_tag == "TITLE")
                                        ? trimmed_raw
                                        : Tokenizer::strip_comment(trimmed_raw);
        std::string_view trimmed  = Tokenizer::trim(stripped);
        if (!trimmed.empty() && !current_tag.empty())
            section_lines.emplace_back(std::string(trimmed));
        // Lines before the first section header are silently ignored
    }

    // Flush the last section
    flush_section();

    // Sections are dispatched in file order, so a property row naming an object
    // declared further down could not resolve. Give those rows one more pass now
    // that every section has been read.
    replay_deferred_rows(ctx);

    return ctx.error_code == 0;
}

// ============================================================================
// replay_deferred_rows()
// ============================================================================

void InputReader::replay_deferred_rows(SimulationContext& ctx) {
    if (ctx.deferred_section_rows.empty()) return;

    const auto pending = std::move(ctx.deferred_section_rows);
    ctx.deferred_section_rows.clear();

    // Group by tag, preserving file order within each tag. A handler that still
    // cannot resolve a row re-stashes it, which is how the unresolvable ones are
    // detected below — no per-handler "am I replaying?" flag is needed.
    std::vector<std::string> tags;
    for (const auto& row : pending) {
        if (std::find(tags.begin(), tags.end(), row.first) == tags.end())
            tags.push_back(row.first);
    }

    for (const auto& tag : tags) {
        std::vector<std::string> lines;
        for (const auto& row : pending) {
            if (row.first == tag) lines.push_back(row.second);
        }
        dispatch_section(tag, lines, ctx);
    }

    // Anything re-stashed on the replay names an object that does not exist.
    // Legacy SWMM treats that as fatal (ERROR 209), so it must not be silent.
    for (const auto& row : ctx.deferred_section_rows) {
        auto tok = Tokenizer::tokenize(row.second);
        ctx.errors.push_back(format_error(ERR_NAME, tok.empty() ? row.second : tok[0]));
    }
    ctx.deferred_section_rows.clear();
}

// ============================================================================
// parse_section_header()
// ============================================================================

std::string InputReader::parse_section_header(std::string_view line) {
    // Expect: '[' NAME ']'  (optional trailing whitespace/comment already stripped)
    if (line.size() < 3 || line.front() != '[') return {};

    const auto close = line.find(']');
    if (close == std::string_view::npos) return {};

    std::string_view name = Tokenizer::trim(line.substr(1, close - 1));
    if (name.empty()) return {};

    return Tokenizer::to_upper(name);
}

// ============================================================================
// dispatch_section()
// ============================================================================

void InputReader::dispatch_section(
    const std::string&             tag,
    const std::vector<std::string>& lines,
    SimulationContext&             ctx
) {
    if (registry_.has(tag)) {
        registry_.dispatch(tag, ctx, lines);
    } else {
        // Unknown section — record it (caller may wish to warn)
        bool already = false;
        for (const auto& s : skipped_sections_) {
            if (s == tag) { already = true; break; }
        }
        if (!already) skipped_sections_.push_back(tag);

        // Issue warning via ctx
        if (ctx.warning_code == 0) {
            ctx.warning_code = 100;  // SWMM_WARN_UNKNOWN_SECTION
        }
    }
}

} /* namespace openswmm::input */
