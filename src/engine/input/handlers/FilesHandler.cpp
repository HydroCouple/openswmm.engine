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
 * @file      FilesHandler.cpp
 * @brief     [FILES] section handler — secondary file references.
 *
 * @see FilesHandler.hpp
 * @see src/legacy/engine/iface.c (legacy reference parser)
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "FilesHandler.hpp"

#include "../Tokenizer.hpp"
#include "../InputParseUtils.hpp"
#include "../../core/SimulationContext.hpp"
#include "../../core/ErrorCodes.hpp"

namespace openswmm::input {

namespace {

// Strip surrounding ASCII quote pairs from a token.  The Tokenizer already
// joins quoted tokens into a single string but leaves the quotes attached;
// keeping behaviour close to the legacy parser means stripping them here.
/**
 * @brief Remove one matching pair of leading/trailing single or double quotes.
 *
 * @param[in] s Owned/copied token; no whitespace trimming or escape decoding.
 * @return Owned inner substring when quoted, otherwise unchanged token.
 */
std::string unquote(std::string s) {
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front())
        s = s.substr(1, s.size() - 2);
    return s;
}

// Parse the optional date+time suffix on a SAVE HOTSTART row.  Returns
// 0.0 when no date is present (legacy meaning: write at end of run).
/**
 * @brief Parse an optional adjacent date/time token pair.
 *
 * No explicit calendar validation/error diagnostic beyond the underlying parser.
 *
 * @param[in] tok Borrowed token vector.
 * @param[in] idx First candidate token index; idx+1 must not overflow size_t.
 * @return Parsed OADate via parse_datetime, or0 when fewer than two tokens remain.
 */
double parse_optional_datetime(const std::vector<std::string>& tok, std::size_t idx) {
    if (idx + 1 >= tok.size())
        return 0.0;
    return parse_datetime(tok[idx], tok[idx + 1]);
}

// A time token legacy datetime_strToTime accepts: decimal hours, or
// H[:M[:S]] in digits. parse_time_day_fraction cannot tell a bad token from
// midnight, so this checks the form.
/**
 * @brief Recognize a numeric time or up to three nonempty colon-delimited digit fields.
 *
 * Recognition alone does not enforce field limits, positivity or a date association; plain digits
 * pass either branch.
 *
 * @param[in] s Borrowed candidate token; numeric branch uses parse_double_strict.
 * @return True for a strict numeric value or digit-only H[:M[:S]] text, false otherwise.
 */
bool is_time_token(const std::string& s) {
    double hours = 0.0;
    if (parse_double_strict(s, hours))
        return true;
    int fields = 0;
    bool digit = false;
    for (const char c : s) {
        if (c >= '0' && c <= '9') {
            digit = true;
            continue;
        }
        if (c != ':' || !digit || ++fields > 2)
            return false;
        digit = false;
    }
    return digit;
}

} // anonymous

void handle_files(SimulationContext& ctx, const std::vector<std::string>& lines) {
    auto& files = ctx.files;

    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 3)
            continue; // need at least mode + kind + path

        const std::string mode_word = Tokenizer::to_upper(tok[0]);
        const std::string kind_word = Tokenizer::to_upper(tok[1]);
        const std::string path = unquote(tok[2]);

        FileMode mode = FileMode::NONE;
        if (mode_word == "SAVE")
            mode = FileMode::SAVE;
        else if (mode_word == "USE")
            mode = FileMode::USE;
        else
            continue; // unrecognised mode — skip silently (legacy behaviour)

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
            // Legacy: USE only — SAVE INFLOWS is a fatal input error
            // (iface.c iface_readFileParams returns ERR_ITEMS).
            if (mode != FileMode::USE) {
                ctx.errors.push_back(format_error(ERR_ITEMS, "", "[FILES] SAVE INFLOWS"));
                continue;
            }
            files.inflows_path = path;
        } else if (kind_word == "OUTFLOWS") {
            // Legacy: SAVE only — USE OUTFLOWS is a fatal input error.
            if (mode != FileMode::SAVE) {
                ctx.errors.push_back(format_error(ERR_ITEMS, "", "[FILES] USE OUTFLOWS"));
                continue;
            }
            files.outflows_path = path;
        } else if (kind_word == "HOTSTART") {
            if (mode == FileMode::SAVE) {
                // Legacy iface.c reads a datetime only from a five-token row
                // and rejects one whose date or time does not parse.
                if (tok.size() == 5) {
                    if (parse_date(tok[3]) <= 0.0) {
                        ctx.errors.push_back(format_error(ERR_DATETIME, tok[3]));
                        continue;
                    }
                    if (!is_time_token(tok[4])) {
                        ctx.errors.push_back(format_error(ERR_DATETIME, tok[4]));
                        continue;
                    }
                }
                HotstartSaveEntry entry;
                entry.path = path;
                entry.datetime = parse_optional_datetime(tok, 3);
                // Optional trailing format keyword, after the datetime if
                // there is one: `SAVE HOTSTART f [date time] LEGACY|NATIVE`.
                // It must trail: legacy SWMM 5 reads a datetime only from a
                // five-token row and ignores later tokens, so a four- or
                // six-token row still opens there. Any other token in that
                // position is ignored, with a warning.
                const std::size_t fmt_at = (tok.size() >= 6) ? 5 : 3;
                if (tok.size() == fmt_at + 1) {
                    const std::string fmt = Tokenizer::to_upper(tok[fmt_at]);
                    if (fmt == "LEGACY")
                        entry.format = HotstartFormat::LEGACY;
                    else if (fmt == "NATIVE")
                        entry.format = HotstartFormat::NATIVE;
                    else {
                        ctx.warnings.push_back("\n  WARNING: Unknown SAVE HOTSTART format '" +
                                               tok[fmt_at] +
                                               "' in [FILES] section - LEGACY or NATIVE expected; "
                                               "the default format will be used.");
                        if (ctx.warning_code == 0)
                            ctx.warning_code = 101;
                    }
                }
                files.hotstart_saves.push_back(std::move(entry));
            } else { // USE
                files.hotstart_use_path = path;
            }
        }
        // Unknown kinds are silently ignored — preserves the legacy
        // behaviour of skipping rows that aren't part of the spec.
    }
}

} /* namespace openswmm::input */
