/**
 * @file InputReader.cpp
 * @brief Implementation of the top-level SWMM .inp file reader.
 *
 * @see InputReader.hpp
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "InputReader.hpp"
#include "Tokenizer.hpp"

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
        ctx.error_code    = 1;  // SWMM_ERR_FILE_NOT_FOUND
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

        // Strip comment, then trim
        std::string_view stripped = Tokenizer::strip_comment(raw_line);
        std::string_view trimmed  = Tokenizer::trim(stripped);

        // Skip blank lines
        if (trimmed.empty()) continue;

        // Detect section header: starts with '['
        if (trimmed.front() == '[') {
            flush_section();
            current_tag = parse_section_header(trimmed);
            continue;
        }

        // Data line inside a section
        if (!current_tag.empty()) {
            section_lines.emplace_back(trimmed);
        }
        // Lines before the first section header are silently ignored
    }

    // Flush the last section
    flush_section();

    return ctx.error_code == 0;
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
