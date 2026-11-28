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
 * @file      InputReader.cpp
 * @brief     Implementation of the top-level SWMM .inp file reader.
 *
 * @see InputReader.hpp
 * @ingroup engine_input
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "InputReader.hpp"
#include "core/FileIO.hpp" // issue #7: UTF-8 paths on Windows
#include "Tokenizer.hpp"
#include "../core/ErrorCodes.hpp"
#include "../core/PerfTimers.hpp"

#include <deque>
#include <fstream>
#include <istream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <cctype>

namespace openswmm::input {

namespace {
// Legacy SectWords (keywords.c), in legacy order. findmatch() accepts a header
// that merely BEGINS with one of them ("[CURVE]" is "[CURVE", "[JUNC]" is
// "[JUNC"), so legacy reads such a header as that section.
constexpr const char* kLegacySectionWords[] = {
    "TITLE",        "OPTION",      "FILE",       "RAINGAGE", "TEMPERATURE", "EVAP",
    "SUBCATCHMENT", "SUBAREA",     "INFIL",      "AQUIFER",  "GROUNDWATER", "SNOWPACK",
    "JUNC",         "OUTFALL",     "STORAGE",    "DIVIDER",  "CONDUIT",     "PUMP",
    "ORIFICE",      "WEIR",        "OUTLET",     "XSECT",    "TRANSECT",    "LOSS",
    "CONTROL",      "POLLUT",      "LANDUSE",    "BUILDUP",  "WASHOFF",     "COVERAGE",
    "INFLOW",       "DWF",         "PATTERN",    "RDII",     "HYDROGRAPH",  "LOADING",
    "TREATMENT",    "CURVE",       "TIMESERIES", "REPORT",   "COORDINATE",  "VERTICES",
    "POLYGON",      "LABEL",       "SYMBOL",     "BACKDROP", "TAG",         "PROFILE",
    "MAP",          "LID_CONTROL", "LID_USAGE",  "GWF",      "ADJUSTMENT",  "EVENT",
    "STREET",       "INLET_USAGE", "INLET"};

// This engine's tag for each entry of kLegacySectionWords, in the same order.
constexpr const char* kLegacySectionTags[] = {
    "TITLE",         "OPTIONS",      "FILES",        "RAINGAGES", "TEMPERATURE", "EVAPORATION",
    "SUBCATCHMENTS", "SUBAREAS",     "INFILTRATION", "AQUIFERS",  "GROUNDWATER", "SNOWPACKS",
    "JUNCTIONS",     "OUTFALLS",     "STORAGE",      "DIVIDERS",  "CONDUITS",    "PUMPS",
    "ORIFICES",      "WEIRS",        "OUTLETS",      "XSECTIONS", "TRANSECTS",   "LOSSES",
    "CONTROLS",      "POLLUTANTS",   "LANDUSES",     "BUILDUP",   "WASHOFF",     "COVERAGES",
    "INFLOWS",       "DWF",          "PATTERNS",     "RDII",      "HYDROGRAPHS", "LOADINGS",
    "TREATMENT",     "CURVES",       "TIMESERIES",   "REPORT",    "COORDINATES", "VERTICES",
    "POLYGONS",      "LABELS",       "SYMBOLS",      "BACKDROP",  "TAGS",        "PROFILES",
    "MAP",           "LID_CONTROLS", "LID_USAGE",    "GWF",       "ADJUSTMENTS", "EVENTS",
    "STREETS",       "INLET_USAGE",  "INLETS"};
static_assert(std::size(kLegacySectionWords) == std::size(kLegacySectionTags));

/**
 * @brief The legacy section a normalized tag begins with (legacy findmatch over SectWords).
 *
 * @param[in] tag Borrowed uppercase tag without brackets; no normalization here.
 * @return Index into kLegacySectionWords of the first word tag begins with, or -1.
 */
int legacy_section_index(const std::string& tag) {
    for (std::size_t i = 0; i < std::size(kLegacySectionWords); ++i)
        if (tag.rfind(kLegacySectionWords[i], 0) == 0)
            return static_cast<int>(i);
    return -1;
}

/**
 * @brief Test whether a normalized tag begins with a legacy section keyword.
 *
 * @param[in] tag Borrowed uppercase tag without brackets; no normalization here.
 * @return True for any legacy keyword prefix, including extended suffixes; false otherwise.
 */
bool legacy_knows_section(const std::string& tag) {
    return legacy_section_index(tag) >= 0;
}
} // namespace

// ============================================================================
// Constructor
// ============================================================================

InputReader::InputReader(SectionRegistry& registry) : registry_(registry) {}

// ============================================================================
// LineBlocks
// ============================================================================

/**
 * @brief The input in blocks of whole lines.
 *
 * A block ends at a line end (a partial last line is carried into the next block), so a view of
 * a line stays valid while its block is kept. Blocks are kept until release_old(), which drops all
 * but the newest: a section that hands out views keeps its blocks until it is dispatched; every
 * other section copies its lines, so its blocks go as soon as they are read.
 */
class LineBlocks {
public:
    /// Blocks read from `in`. `text_mode`: read as the Windows C runtime's text mode did -- the
    /// input ends at the first Ctrl-Z (0x1A), and a CR before a line's '\n' is dropped.
    LineBlocks(std::istream& in, bool text_mode) : in_(&in), text_mode_(text_mode) {}
    /// One block: all of `text`, which must outlive the read.
    explicit LineBlocks(std::string_view text) : text_(text) {}

    bool text_mode() const noexcept { return text_mode_; }

    /// The next block, or an empty view at the end of the input.
    std::string_view next() {
        if (!in_) {
            const std::string_view t = text_;
            text_ = {};
            return t;
        }
        std::string block = std::move(carry_);
        carry_.clear();
        while (!eof_) {
            const std::size_t old = block.size();
            block.resize(old + kBlockBytes);
            in_->read(block.data() + old, static_cast<std::streamsize>(kBlockBytes));
            const auto got = static_cast<std::size_t>(in_->gcount());
            block.resize(old + got);
            if (got < kBlockBytes)
                eof_ = true;
            if (text_mode_) {
                if (const auto z = block.find('\x1A', old); z != std::string::npos) {
                    block.resize(z);
                    eof_ = true;
                }
            }
            // Whole lines only: what follows the last '\n' read just now waits for the next block.
            std::size_t end = block.size();
            while (end > old && block[end - 1] != '\n')
                --end;
            if (end > old) {
                carry_.assign(block, end, std::string::npos);
                block.resize(end);
                break;
            }
        }
        if (block.empty())
            return {};
        blocks_.push_back(std::move(block));
        return blocks_.back();
    }

    /// Drop every block but the newest.
    void release_old() {
        while (blocks_.size() > 1)
            blocks_.pop_front();
    }

private:
    static constexpr std::size_t kBlockBytes = std::size_t{1} << 20;
    std::istream* in_ = nullptr;
    bool text_mode_ = false;
    bool eof_ = false;
    std::string_view text_;
    std::string carry_;
    std::deque<std::string> blocks_;
};

// ============================================================================
// read()
// ============================================================================

bool InputReader::read(const std::string& path, SimulationContext& ctx) {
    std::ifstream ifs(openswmm::io::utf8_path(path), std::ios::binary);
    if (!ifs.is_open()) {
        ctx.error_code = 2; // public SWMM_ERR_INPFILE (was 1 = NOMEM)
        ctx.error_message = "InputReader: cannot open file '" + path + "'";
        ctx.state = EngineState::ERROR_STATE;
        return false;
    }
    // The .inp reader would parse an HDF5 or GeoPackage model as text: the
    // model text stored inside parses, then its last section runs into binary
    // bytes. Name the input plugin instead.
    {
        char head[16] = {};
        ifs.read(head, sizeof head);
        const std::string_view sig(head, static_cast<std::size_t>(ifs.gcount()));
        const char* kind = nullptr;
        const char* plugin = nullptr;
        if (sig.substr(0, 8) == std::string_view("\x89HDF\r\n\x1a\n", 8)) {
            kind = "an HDF5 file";
            plugin = "org.hydrocouple.openswmm.plugins.hdf5";
        } else if (sig == std::string_view("SQLite format 3\0", 16)) {
            kind = "a GeoPackage (SQLite) file";
            plugin = "org.hydrocouple.openswmm.plugins.geopackage";
        }
        if (kind) {
            ctx.error_code = 2; // public SWMM_ERR_INPFILE
            ctx.error_message = "InputReader: '" + path + "' is " + kind +
                                ", not an .inp; open it with the input plugin " + plugin;
            ctx.state = EngineState::ERROR_STATE;
            return false;
        }
        ifs.clear();
        ifs.seekg(0, std::ios::beg);
    }
    ctx.inp_file_path = path;
#if defined(_WIN32)
    // The file used to be read in text mode; keep its rules.
    LineBlocks blocks(ifs, true);
#else
    LineBlocks blocks(ifs, false);
#endif
    return read_blocks(blocks, ctx);
}

// ============================================================================
// read_stream() / read_text()
// ============================================================================

bool InputReader::read_stream(std::istream& stream, SimulationContext& ctx) {
    LineBlocks blocks(stream, false);
    return read_blocks(blocks, ctx);
}

bool InputReader::read_text(std::string_view text, SimulationContext& ctx) {
    LineBlocks blocks(text);
    return read_blocks(blocks, ctx);
}

// ============================================================================
// read_blocks()
// ============================================================================

bool InputReader::read_blocks(LineBlocks& blocks, SimulationContext& ctx) {
    lines_read_ = 0;
    skipped_sections_.clear();
    ctx.unknown_section_headers.clear();

    // An .inp reports no subcatchments, nodes or links unless [REPORT] names
    // them (legacy RptFlags default FALSE; SWMM manual: default NONE). The
    // SimulationOptions default of ALL stays for models built through the API.
    ctx.options.rpt_subcatchments = 0;
    ctx.options.rpt_nodes = 0;
    ctx.options.rpt_links = 0;

    std::string current_tag;                  // e.g. "OPTIONS"
    bool views = false;                       // the section's handler takes line views
    bool skip = false;                        // the section is in sections_to_skip_
    std::vector<std::string_view> view_lines; // its lines then, borrowed from blocks
    std::vector<std::string> section_lines;   // its lines otherwise

    auto flush_section = [&]() {
        if (!current_tag.empty() && !skip) {
            perf::ScopedTimer _pt(perf::sec_read_dispatch);
            if (views) {
                registry_.dispatch_views(current_tag, ctx, view_lines);
                view_lines.clear();
            } else {
                dispatch_section(current_tag, section_lines, ctx);
                section_lines.clear();
            }
        }
        blocks.release_old(); // no view points before the current block now
    };
    const auto keep = [&](std::string_view line) {
        if (skip)
            return;
        if (views)
            view_lines.push_back(line);
        else
            section_lines.emplace_back(line);
    };

    for (std::string_view block = blocks.next(); !block.empty(); block = blocks.next()) {
        if (!views)
            blocks.release_old();
        std::size_t pos = 0;
        while (pos < block.size()) {
            std::size_t nl = block.find('\n', pos);
            if (nl == std::string_view::npos)
                nl = block.size();
            std::string_view raw_line = block.substr(pos, nl - pos);
            pos = nl + 1;
            ++lines_read_;

            // A UTF-8 byte-order mark (Notepad and many Windows tools write one)
            // would otherwise hide the first section header.
            if (lines_read_ == 1 && raw_line.substr(0, 3) == "\xEF\xBB\xBF")
                raw_line.remove_prefix(3);

            // Text mode turned a line's CR LF into LF before the CR strip below.
            if (blocks.text_mode() && nl < block.size() && !raw_line.empty() &&
                raw_line.back() == '\r')
                raw_line.remove_suffix(1);

            if (line_observer_)
                line_observer_(raw_line);

            // Strip \r (Windows CRLF)
            if (!raw_line.empty() && raw_line.back() == '\r') {
                raw_line.remove_suffix(1);
            }

            // Trim first so we can inspect the leading character without allocating.
            std::string_view trimmed_raw = Tokenizer::trim(raw_line);

            // Skip truly empty lines
            if (trimmed_raw.empty())
                continue;

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
                    // A tag this engine registers is taken as written (6.0-only sections such
                    // as [INLET_JUNCTIONS] and [RDII_DECAY] begin with a legacy word). Any other
                    // tag that begins with a legacy section word is read as that section.
                    if (!current_tag.empty() && !registry_.has(current_tag)) {
                        if (const int k = legacy_section_index(current_tag); k >= 0)
                            current_tag = kLegacySectionTags[k];
                    }
                    skip = std::find(sections_to_skip_.begin(), sections_to_skip_.end(),
                                     current_tag) != sections_to_skip_.end();
                    views = !skip && !current_tag.empty() && registry_.takes_views(current_tag);
                    if (!skip && !current_tag.empty() && !registry_.has(current_tag) &&
                        !legacy_knows_section(current_tag)) {
                        // legacy names the first token of the line, as written
                        const auto end = trimmed_raw.find_first_of(" \t");
                        ctx.unknown_section_headers.emplace_back(
                            std::string(trimmed_raw.substr(0, end)), lines_read_);
                    }
                    continue;
                }
                // Not a clean header — fall through to data-line handling below.
            }

            // Comment line (starts with ';') — keep verbatim inside a section so
            // section handlers can associate per-object comments with objects.
            if (trimmed_raw.front() == ';') {
                if (!current_tag.empty())
                    keep(trimmed_raw);
                continue;
            }

            // Data line. [TITLE] is free-form prose, so preserve it verbatim —
            // legacy SWMM stores the whole title line (input.c readTitle), and a
            // ';' in a sentence is description, not a comment. Stripping it here
            // truncated titles mid-sentence in the .rpt echo. Every other section
            // strips the inline trailing comment.
            // legacy readTitle copies the RAW line, leading blanks included, so
            // [TITLE] keeps it untrimmed (the .rpt echo and the .inp round trip
            // both reproduce it).
            if (current_tag == "TITLE") {
                keep(raw_line);
                continue;
            }
            std::string_view stripped = Tokenizer::strip_comment(trimmed_raw);
            std::string_view trimmed = Tokenizer::trim(stripped);
            if (!trimmed.empty() && !current_tag.empty())
                keep(trimmed);
            // Lines before the first section header are silently ignored
        }
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
    if (ctx.deferred_section_rows.empty())
        return;

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
            if (row.first == tag)
                lines.push_back(row.second);
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
    if (line.size() < 3 || line.front() != '[')
        return {};

    const auto close = line.find(']');
    if (close == std::string_view::npos)
        return {};

    std::string_view name = Tokenizer::trim(line.substr(1, close - 1));
    if (name.empty())
        return {};

    return Tokenizer::to_upper(name);
}

// ============================================================================
// dispatch_section()
// ============================================================================

void InputReader::dispatch_section(const std::string& tag, const std::vector<std::string>& lines,
                                   SimulationContext& ctx) {
    if (registry_.has(tag)) {
        registry_.dispatch(tag, ctx, lines);
    } else if (!legacy_knows_section(tag)) {
        // Unknown section — record it (caller may wish to warn)
        bool already = false;
        for (const auto& s : skipped_sections_) {
            if (s == tag) {
                already = true;
                break;
            }
        }
        if (!already)
            skipped_sections_.push_back(tag);

        // Issue warning via ctx
        if (ctx.warning_code == 0) {
            ctx.warning_code = 100; // SWMM_WARN_UNKNOWN_SECTION
        }
    }
}

} /* namespace openswmm::input */
