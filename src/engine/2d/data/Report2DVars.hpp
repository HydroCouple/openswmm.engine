/**
 * @file      Report2DVars.hpp
 * @brief     Header-only token ↔ bitmask helpers for
 *        [2D_OPTIONS] REPORT_2D_VARIABLES / REPORT_2D_SPECIES / REPORT_2D_STEP.
 *
 * @details Shared by the [2D_OPTIONS] parser (SectionHandlers2D, 2D builds
 *          only), the .inp writer and the GeoPackage writer/reader (compiled
 *          in every build), so the vocabulary lives in one place and no
 *          non-2D build gains a link dependency on the 2D TU.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_REPORT2D_VARS_HPP
#define OPENSWMM_ENGINE_2D_REPORT2D_VARS_HPP

#include "SolverOptions2D.hpp"

#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

namespace openswmm::twoD::report2d {

/**
 * @brief Group tokens in bit order (index i ↔ bit 1u << i).
 *
 * @return Borrowed immutable vector in dataset-group bit order. Its process-lifetime storage
 * must not be modified or freed; first initialization may allocate/throw and is synchronized by
 * C++ static initialization.
 */
inline const std::vector<std::string>& tokens() {
    static const std::vector<std::string> kTokens = {
        "DEPTH",        "VELOCITY",    "EDGE_FLUX", "NODE_HEAD",  "SPECIES",   "RAINFALL",
        "INFILTRATION", "COUPLING",    "GRADIENTS", "CONTINUITY", "ENVELOPES",
        "BUILDUP",                     // S7
        "GROUNDWATER",  "GW_DETAILED", // G-O
        "HEAT",
    };
    return kTokens;
}

/**
 * @brief Compare one report token against a required literal ignoring byte case.
 * @param[in] a Borrowed token string.
 * @param[in] b Required NUL-terminated token/literal; NULL is unsupported.
 * @return True for equal lengths/content ignoring case; no allocation or mutation. std::toupper
 * receives unsigned-byte values.
 */
inline bool iequalsTok(const std::string& a, const char* b) {
    std::size_t i = 0;
    for (; i < a.size() && b[i]; ++i)
        if (std::toupper(static_cast<unsigned char>(a[i])) !=
            std::toupper(static_cast<unsigned char>(b[i])))
            return false;
    return i == a.size() && b[i] == '\0';
}

/**
 * @brief Split on whitespace / commas.
 *
 * @param[in] text Borrowed comma/space/tab/CR/LF-separated list.
 * @return Newly owned tokens with empty fields discarded, preserving order/case. Other
 * characters remain literal; no quoting grammar or semantic validation. Allocation may throw and
 * no input storage is retained.
 */
inline std::vector<std::string> splitList(const std::string& text) {
    std::vector<std::string> toks;
    std::string cur;
    for (char c : text) {
        if (c == ',' || c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (!cur.empty()) {
                toks.push_back(cur);
                cur.clear();
            }
        } else {
            cur += c;
        }
    }
    if (!cur.empty())
        toks.push_back(cur);
    return toks;
}

/**
 * @brief Parse a token list or a preset (DEFAULT | MINIMAL | ALL | NONE) into a bitmask. DEPTH
 * is always included — every reader keys the time axis on it. Returns an error string, empty on
 * success.
 *
 * @param[in] text Borrowed case-insensitive preset/token list; empty lists are errors.
 * @param[out] mask Reset to zero on entry, then ORed incrementally. A successful parse always
 * includes DEPTH, including the NONE spelling.
 * @return Empty string on success or a descriptive error for an unknown/empty list.
 * @note A rejected later token leaves a partial mask; do not publish it on error. Allocations
 * may throw. No model state or input storage retained.
 */
inline std::string parseMask(const std::string& text, unsigned& mask) {
    mask = 0;
    const auto toks = splitList(text);
    if (toks.empty())
        return "REPORT_2D_VARIABLES needs DEFAULT|MINIMAL|ALL or a token list";
    const auto& names = tokens();
    for (const auto& t : toks) {
        if (iequalsTok(t, "DEFAULT")) {
            mask |= DEFAULT_MASK;
            continue;
        }
        if (iequalsTok(t, "MINIMAL")) {
            mask |= MINIMAL_MASK;
            continue;
        }
        if (iequalsTok(t, "ALL")) {
            mask |= ALL_MASK;
            continue;
        }
        if (iequalsTok(t, "NONE")) {
            continue;
        }
        bool found = false;
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (iequalsTok(t, names[i].c_str())) {
                mask |= (1u << i);
                found = true;
                break;
            }
        }
        if (!found) {
            std::string all;
            for (const auto& n : names) {
                if (!all.empty())
                    all += "|";
                all += n;
            }
            return "Unknown REPORT_2D_VARIABLES token '" + t +
                   "' (expected DEFAULT|MINIMAL|ALL|NONE or " + all + ")";
        }
    }
    mask |= DEPTH;
    return {};
}

/**
 * @brief Preset name when the mask equals one, else the space-separated token list.
 *
 * @param[in] mask Dataset-group bitmask; unknown bits are discarded.
 * @return Owned preset name or ordered space-separated tokens. Empty recognized selection
 * formats as DEPTH. Allocation may throw; no mutation or retained input.
 */
inline std::string formatMask(unsigned mask) {
    mask &= ALL_MASK;
    if (mask == DEFAULT_MASK)
        return "DEFAULT";
    if (mask == MINIMAL_MASK)
        return "MINIMAL";
    if (mask == ALL_MASK)
        return "ALL";
    const auto& names = tokens();
    std::string out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (mask & (1u << i)) {
            if (!out.empty())
                out += ' ';
            out += names[i];
        }
    }
    return out.empty() ? std::string("DEPTH") : out;
}

/**
 * @brief "ALL" for an empty (unfiltered) list, else the space-separated names.
 *
 * @param[in] species Borrowed ordered species-name filter; empty means unfiltered.
 * @return Newly owned ALL string for an empty list, otherwise names joined by one space without
 * validation/escaping. Allocation may throw.
 */
inline std::string formatSpecies(const std::vector<std::string>& species) {
    if (species.empty())
        return "ALL";
    std::string out;
    for (const auto& n : species) {
        if (!out.empty())
            out += ' ';
        out += n;
    }
    return out;
}

/**
 * @brief Species list from text; "ALL" (alone) → empty list.
 *
 * @param[in] text Borrowed list split by splitList(). A sole case-insensitive ALL removes
 * filtering.
 * @return Newly owned ordered names, empty for ALL or empty input. Species existence and
 * duplicate validity are not checked; allocation may throw.
 */
inline std::vector<std::string> parseSpecies(const std::string& text) {
    auto v = splitList(text);
    if (v.size() == 1 && iequalsTok(v[0], "ALL"))
        v.clear();
    return v;
}

/**
 * @brief HH:MM:SS (REPORT_STEP spelling).
 *
 * @param[in] seconds Report interval in seconds; positive values round to nearest whole second,
 * nonpositive and NaN become zero.
 * @pre Any positive value is finite and representable as long after adding 0.5; overflow is not
 * checked.
 * @return Owned HH:MM:SS string with hours allowed above 23. Formatting uses a bounded local
 * buffer; no shared storage or model mutation.
 */
inline std::string formatStep(double seconds) {
    const long total = seconds > 0.0 ? static_cast<long>(seconds + 0.5) : 0L;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02ld:%02ld:%02ld", total / 3600, (total % 3600) / 60,
                  total % 60);
    return buf;
}

} // namespace openswmm::twoD::report2d

#endif // OPENSWMM_ENGINE_2D_REPORT2D_VARS_HPP
