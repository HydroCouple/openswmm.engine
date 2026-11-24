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

#include <string_view>
#include <utility>

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

    // Legacy iface_readFileParams reads the mode and the kind with findmatch():
    // a keyword matches when it is the leading part of the token (input.c
    // match()), so `SAVE Outflows/Calibration` is SAVE OUTFLOWS. Any other
    // token is ERROR 205 naming it, and a row with one token is ERROR 203. The
    // engine compared whole words and dropped every other row without a
    // message, so decks spelled that way ran without their interface file
    // (issue #215).
    static constexpr std::pair<std::string_view, FileMode> kModes[] = {{"NO", FileMode::NONE},
                                                                       {"SCRATCH", FileMode::NONE},
                                                                       {"USE", FileMode::USE},
                                                                       {"SAVE", FileMode::SAVE}};
    static constexpr std::string_view kKinds[] = {"RAINFALL", "RUNOFF",  "HOTSTART",
                                                  "RDII",     "INFLOWS", "OUTFLOWS"};
    const auto leads_with = [](std::string_view token, std::string_view keyword) {
        return token.size() >= keyword.size() && token.compare(0, keyword.size(), keyword) == 0;
    };

    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.empty())
            continue;
        if (tok.size() < 2) {
            ctx.errors.push_back(format_error(ERR_ITEMS, "", "[FILES] " + line));
            continue;
        }

        const std::string mode_word = Tokenizer::to_upper(tok[0]);
        const std::string kind_token = Tokenizer::to_upper(tok[1]);

        FileMode mode = FileMode::NONE;
        bool mode_known = false;
        for (const auto& [keyword, value] : kModes) {
            if (leads_with(mode_word, keyword)) {
                mode = value;
                mode_known = true;
                break;
            }
        }
        if (!mode_known) {
            ctx.errors.push_back(format_error(ERR_KEYWORD, tok[0], "in [FILES]"));
            continue;
        }
        std::string kind_word;
        for (const auto keyword : kKinds) {
            if (leads_with(kind_token, keyword)) {
                kind_word = std::string(keyword);
                break;
            }
        }
        if (kind_word.empty()) {
            ctx.errors.push_back(format_error(ERR_KEYWORD, tok[1], "in [FILES]"));
            continue;
        }
        if (tok.size() < 3)
            continue; // legacy: the keywords alone are accepted and set nothing
        const std::string path = unquote(tok[2]);

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
            } else if (mode == FileMode::USE) {
                files.hotstart_use_path = path;
            } // legacy handles only USE and SAVE for HOTSTART
        }
    }
}

void handle_restart(SimulationContext& ctx, const std::vector<std::string>& lines) {
    auto& r = ctx.restart;
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.empty())
            continue;
        const std::string key = Tokenizer::to_upper(tok[0]);
        if (tok.size() < 2) {
            ctx.errors.push_back(format_error(ERR_ITEMS, "", "[RESTART] " + tok[0]));
            continue;
        }
        const std::string word = Tokenizer::to_upper(tok[1]);
        auto bad = [&](int code) {
            ctx.errors.push_back(format_error(code, tok[1], "[RESTART] " + key));
        };
        if (key == "SOURCE_FILE") {
            r.source_file = unquote(tok[1]);
        } else if (key == "SOURCE_RUN") {
            r.source_run = tok[1];
        } else if (key == "SOURCE_RECORD") {
            r.source_record = word == "LATEST" ? "LATEST" : tok[1];
        } else if (key == "MODE") {
            if (word == "INITIAL_CONDITIONS")
                r.mode = RestartMode::INITIAL_CONDITIONS;
            else if (word == "CONTINUE")
                r.mode = RestartMode::CONTINUE;
            else
                bad(ERR_KEYWORD);
        } else if (key == "RECORD_FILE") {
            r.record_file = unquote(tok[1]);
        } else if (key == "RECORD_FORMAT") {
            if (word == "GEOPACKAGE")
                r.record_format = RestartFormat::GEOPACKAGE;
            else if (word == "HDF5")
                r.record_format = RestartFormat::HDF5;
            else
                bad(ERR_KEYWORD);
        } else if (key == "RECORD_AT") {
            if (word == "END") {
                r.record_at.push_back(0.0);
            } else if (tok.size() < 3 || parse_date(tok[1]) <= 0.0 || !is_time_token(tok[2])) {
                bad(ERR_DATETIME);
            } else {
                r.record_at.push_back(parse_datetime(tok[1], tok[2]));
            }
        } else if (key == "RECORD_EVERY") {
            const double seconds = is_time_token(tok[1]) ? parse_time_seconds(tok[1]) : 0.0;
            if (seconds <= 0.0)
                bad(ERR_NUMBER);
            else
                r.record_every = seconds;
        } else if (key == "RECORD_CONTINUATION") {
            if (word == "YES")
                r.record_continuation = true;
            else if (word == "NO")
                r.record_continuation = false;
            else
                bad(ERR_KEYWORD);
        } else {
            ctx.errors.push_back(format_error(ERR_KEYWORD, tok[0], "[RESTART]"));
        }
    }
}

} /* namespace openswmm::input */
