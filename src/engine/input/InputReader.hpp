/**
 * @file InputReader.hpp
 * @brief Top-level SWMM .inp file reader.
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
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
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
     * @brief Construct an InputReader with a pre-populated registry.
     *
     * @param registry  Reference to the section registry (must outlive this reader).
     */
    explicit InputReader(SectionRegistry& registry);

    /**
     * @brief Read and parse the specified .inp file.
     *
     * @details After parsing, `ctx` holds all objects, options, and tables.
     *          Any parse errors are reported via `ctx.error_code` /
     *          `ctx.error_message`. The function returns false if a fatal
     *          parse error occurred.
     *
     * @param path  Path to the input file.
     * @param ctx   Simulation context to populate.
     * @returns     true on success, false if a fatal error occurred.
     *
     * @see ctx.error_code, ctx.error_message for diagnostics.
     */
    bool read(const std::string& path, SimulationContext& ctx);

    /**
     * @brief Parse from an already-open stream (for testing / stdin).
     *
     * @param stream  Input stream.
     * @param ctx     Simulation context to populate.
     * @returns       true on success.
     */
    bool read_stream(std::istream& stream, SimulationContext& ctx);

    /**
     * @brief Number of lines successfully parsed in the last read() call.
     */
    int lines_read() const noexcept { return lines_read_; }

    /**
     * @brief List of section tags that were skipped (no registered handler).
     */
    const std::vector<std::string>& skipped_sections() const noexcept {
        return skipped_sections_;
    }

private:
    SectionRegistry&        registry_;
    int                     lines_read_       = 0;
    std::vector<std::string> skipped_sections_;

    /**
     * @brief Parse a section header line and extract the tag.
     *
     * @details Expects a line of the form `[SECTIONNAME]` (after comment strip).
     *          Returns empty string if the line is not a section header.
     *
     * @param line  Comment-stripped, trimmed line.
     * @returns     Uppercase section name (without brackets), or "" if not a header.
     */
    static std::string parse_section_header(std::string_view line);

    /**
     * @brief Dispatch accumulated section lines and warn about unknown sections.
     */
    void dispatch_section(
        const std::string&             tag,
        const std::vector<std::string>& lines,
        SimulationContext&             ctx
    );
};

} /* namespace openswmm::input */

#endif /* OPENSWMM_ENGINE_INPUT_READER_HPP */
