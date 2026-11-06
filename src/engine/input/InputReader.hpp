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
 * @file      InputReader.hpp
 * @brief     Top-level SWMM .inp file reader.
 *
 * @details InputReader performs a single-pass read of the .inp file:
 *
 *  1. Read lines one at a time.
 *  2. Detect section headers (`[SECTIONNAME]`).
 *  3. Accumulate non-blank, comment-stripped lines for the current section.
 *  4. When the next section header (or EOF) is reached, dispatch the
 *     accumulated lines to the SectionRegistry.
 *
 * The SectionRegistry then calls the appropriate built-in or custom handler,
 * which populates the SimulationContext.
 *
 * ### Two-pass vs single-pass
 *
 * The legacy SWMM uses two passes: one to count objects, one to read data.
 * The new engine does a **single pass** using dynamic resize: objects are
 * added to name indices as their section is parsed, and NodeData::resize()
 * is called at the end of each section.
 *
 * ### Parsing order
 *
 * SWMM input files may list sections in any order. Cross-references
 * (e.g., a junction referencing a rain gage) are resolved after all sections
 * are parsed in a final resolution step.
 *
 * @see Tokenizer.hpp — used for line splitting
 * @see SectionRegistry.hpp — dispatches each section
 * @see Legacy reference: src/solver/input.c — input_readData()
 * @ingroup engine_input
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

/**
 * @defgroup engine_input Engine input
 * @brief Input dispatch, parsing, path resolution and format-specific readers.
 * @ingroup new_engine
 */

#ifndef OPENSWMM_ENGINE_INPUT_READER_HPP
#define OPENSWMM_ENGINE_INPUT_READER_HPP

#include "SectionRegistry.hpp"
#include "../core/SimulationContext.hpp"

#include <string>
#include <vector>
#include <iosfwd>

namespace openswmm::input {

/**
 * @brief Reads and parses a SWMM .inp file into a SimulationContext.
 *
 * @ingroup engine_input
 */
class InputReader {
public:
    /**
     * @brief Borrow a pre-populated handler registry.
     *
     * No handlers are registered automatically by this constructor. Caller serializes reader, registry
     * and context access.
     *
     * @param[in] registry Mutable registry reference that must outlive the reader and all synchronous
     *     reads.
     */
    explicit InputReader(SectionRegistry& registry);

    /**
     * @brief Open a UTF-8 input path and parse its section stream.
     *
     * Open failure sets error_code=2, an error message and ERROR_STATE. On success delegates
     * read_stream(); allocation/handler exceptions propagate. Does not perform post-parse
     * cross-reference resolution or full model validation itself.
     *
     * @param[in] path Borrowed input filename; copied into ctx.inp_file_path only after successful
     *     open.
     * @param[in,out] ctx Caller-owned model context; handlers mutate it directly. This reader does not
     *     clear prior objects, errors or error_code and does not roll back partial input.
     * @return False for open failure or nonzero ctx.error_code after parsing; true otherwise.
     */
    bool read(const std::string& path, SimulationContext& ctx);

    /**
     * @brief Read section batches and replay deferred rows once.
     *
     * Resets reader counters/skipped sections and ctx.unknown_section_headers, sets report object
     * selections to NONE, preserves object comment rows and TITLE prose, ignores data before the first
     * header. Existing objects/errors remain. Does not distinguish EOF from a stream read failure or
     * call PostParseResolver.
     *
     * @param[in,out] stream Borrowed open stream read from its current position until getline stops; no
     *     seek/close is performed.
     * @param[in,out] ctx Caller-owned model context; handlers mutate it directly. This reader does not
     *     clear prior objects, errors or error_code and does not roll back partial input.
     * @return Whether ctx.error_code equals zero at the end; appended ctx.errors alone do not force
     *     false.
     */
    bool read_stream(std::istream& stream, SimulationContext& ctx);

    /**
     * @brief Read the physical-line counter from the most recent stream parse.
     * @return Number of getline successes, including blank/comment/header lines. An open failure in
     *     read() leaves the previous counter unchanged.
     */
    int lines_read() const noexcept { return lines_read_; }

    /**
     * @brief Borrow unrecognized, unhandled section tags from the last stream parse.
     *
     * Tags are unique in encounter order; known legacy prefixes without handlers are silently omitted.
     * @return Owned vector reference, invalidated by the next read_stream() or reader destruction.
     */
    const std::vector<std::string>& skipped_sections() const noexcept { return skipped_sections_; }

private:
    SectionRegistry& registry_;
    int lines_read_ = 0;
    std::vector<std::string> skipped_sections_;

    /**
     * @brief Extract and uppercase the text before the first closing bracket.
     *
     * This helper itself does not reject trailing text after the first closing bracket.
     *
     * @param[in] line Borrowed line beginning with opening bracket; caller performs
     *     trim/comment/header-shape checks.
     * @return Owned uppercase trimmed tag; empty for missing opening/closing bracket or an empty name.
     */
    static std::string parse_section_header(std::string_view line);

    /**
     * @brief Dispatch registered sections or record unfamiliar section tags.
     *
     * Known legacy section prefixes without handlers are ignored. Other unknown tags are deduplicated
     * into skipped_sections_ and set warning_code=100 only when no warning code exists. Handler
     * exceptions propagate.
     *
     * @param[in] tag Normalized section tag without brackets.
     * @param[in] lines Borrowed batch passed synchronously to a registered handler.
     * @param[in,out] ctx Caller-owned model context; handlers mutate it directly. This reader does not
     *     clear prior objects, errors or error_code and does not roll back partial input.
     */
    void dispatch_section(const std::string& tag, const std::vector<std::string>& lines,
                          SimulationContext& ctx);

    /**
     * @brief Replay unresolved rows once, grouped by first-seen section tag.
     *
     * Moves the pending list aside and preserves order within each tag, then redispatches. Rows
     * deferred again append ERR_NAME diagnostics to ctx.errors and are cleared; this function does not
     * itself set error_code. No rollback on handler exceptions.
     *
     * @param[in,out] ctx Caller-owned model context; handlers mutate it directly. This reader does not
     *     clear prior objects, errors or error_code and does not roll back partial input.
     */
    void replay_deferred_rows(SimulationContext& ctx);
};

} /* namespace openswmm::input */

#endif /* OPENSWMM_ENGINE_INPUT_READER_HPP */
