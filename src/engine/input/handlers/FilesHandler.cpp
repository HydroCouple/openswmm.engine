/**
 * @file FilesHandler.cpp
 * @brief [FILES] section handler — secondary file references.
 *
 * @see FilesHandler.hpp
 * @see src/legacy/engine/iface.c (legacy reference parser)
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "FilesHandler.hpp"

#include "../Tokenizer.hpp"
#include "../InputParseUtils.hpp"
#include "../../core/SimulationContext.hpp"

namespace openswmm::input {

namespace {

// Strip surrounding ASCII quote pairs from a token.  The Tokenizer already
// joins quoted tokens into a single string but leaves the quotes attached;
// keeping behaviour close to the legacy parser means stripping them here.
std::string unquote(std::string s) {
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'')
                       && s.back() == s.front())
        s = s.substr(1, s.size() - 2);
    return s;
}

// Parse the optional date+time suffix on a SAVE HOTSTART row.  Returns
// 0.0 when no date is present (legacy meaning: write at end of run).
double parse_optional_datetime(const std::vector<std::string>& tok,
                                std::size_t idx) {
    if (idx + 1 >= tok.size()) return 0.0;
    return parse_datetime(tok[idx], tok[idx + 1]);
}

} // anonymous

void handle_files(SimulationContext& ctx, const std::vector<std::string>& lines) {
    auto& files = ctx.files;

    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 3) continue;  // need at least mode + kind + path

        const std::string mode_word = Tokenizer::to_upper(tok[0]);
        const std::string kind_word = Tokenizer::to_upper(tok[1]);
        const std::string path      = unquote(tok[2]);

        FileMode mode = FileMode::NONE;
        if      (mode_word == "SAVE") mode = FileMode::SAVE;
        else if (mode_word == "USE")  mode = FileMode::USE;
        else continue;  // unrecognised mode — skip silently (legacy behaviour)

        if (kind_word == "RAINFALL") {
            files.rainfall_mode = mode;
            files.rainfall_path = path;
        } else if (kind_word == "RUNOFF") {
            files.runoff_mode = mode;
            files.runoff_path = path;
        } else if (kind_word == "RDII") {
            files.rdii_mode = mode;
            files.rdii_path = path;
        } else if (kind_word == "INFLOWS") {
            // Legacy: USE only.  Ignore the mode column for storage —
            // the slot semantics imply USE.
            files.inflows_path = path;
        } else if (kind_word == "OUTFLOWS") {
            // Legacy: SAVE only.
            files.outflows_path = path;
        } else if (kind_word == "HOTSTART") {
            if (mode == FileMode::SAVE) {
                files.hotstart_save_path = path;
                files.hotstart_save_datetime = parse_optional_datetime(tok, 3);
            } else {  // USE
                files.hotstart_use_path = path;
            }
        }
        // Unknown kinds are silently ignored — preserves the legacy
        // behaviour of skipping rows that aren't part of the spec.
    }
}

} /* namespace openswmm::input */
