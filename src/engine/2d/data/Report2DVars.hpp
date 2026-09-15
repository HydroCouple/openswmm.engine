/**
 * @file Report2DVars.hpp
 * @brief Header-only token ↔ bitmask helpers for
 *        [2D_OPTIONS] REPORT_2D_VARIABLES / REPORT_2D_SPECIES / REPORT_2D_STEP.
 *
 * @details Shared by the [2D_OPTIONS] parser (SectionHandlers2D, 2D builds
 *          only), the .inp writer and the GeoPackage writer/reader (compiled
 *          in every build), so the vocabulary lives in one place and no
 *          non-2D build gains a link dependency on the 2D TU.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */
#ifndef OPENSWMM_ENGINE_2D_REPORT2D_VARS_HPP
#define OPENSWMM_ENGINE_2D_REPORT2D_VARS_HPP

#include "SolverOptions2D.hpp"

#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

namespace openswmm::twoD::report2d {

/// Group tokens in bit order (index i ↔ bit 1u << i).
inline const std::vector<std::string>& tokens() {
    static const std::vector<std::string> kTokens = {
        "DEPTH", "VELOCITY", "EDGE_FLUX", "NODE_HEAD", "SPECIES", "RAINFALL",
        "INFILTRATION", "COUPLING", "GRADIENTS", "CONTINUITY", "ENVELOPES",
    };
    return kTokens;
}

inline bool iequalsTok(const std::string& a, const char* b) {
    std::size_t i = 0;
    for (; i < a.size() && b[i]; ++i)
        if (std::toupper(static_cast<unsigned char>(a[i])) !=
            std::toupper(static_cast<unsigned char>(b[i]))) return false;
    return i == a.size() && b[i] == '\0';
}

/// Split on whitespace / commas.
inline std::vector<std::string> splitList(const std::string& text) {
    std::vector<std::string> toks;
    std::string cur;
    for (char c : text) {
        if (c == ',' || c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (!cur.empty()) { toks.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) toks.push_back(cur);
    return toks;
}

/**
 * @brief Parse a token list or a preset (DEFAULT | MINIMAL | ALL | NONE) into
 *        a bitmask. DEPTH is always included — every reader keys the time
 *        axis on it. Returns an error string, empty on success.
 */
inline std::string parseMask(const std::string& text, unsigned& mask) {
    mask = 0;
    const auto toks = splitList(text);
    if (toks.empty())
        return "REPORT_2D_VARIABLES needs DEFAULT|MINIMAL|ALL or a token list";
    const auto& names = tokens();
    for (const auto& t : toks) {
        if (iequalsTok(t, "DEFAULT")) { mask |= DEFAULT_MASK; continue; }
        if (iequalsTok(t, "MINIMAL")) { mask |= MINIMAL_MASK; continue; }
        if (iequalsTok(t, "ALL"))     { mask |= ALL_MASK;     continue; }
        if (iequalsTok(t, "NONE"))    { continue; }
        bool found = false;
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (iequalsTok(t, names[i].c_str())) { mask |= (1u << i); found = true; break; }
        }
        if (!found) {
            std::string all;
            for (const auto& n : names) { if (!all.empty()) all += "|"; all += n; }
            return "Unknown REPORT_2D_VARIABLES token '" + t +
                   "' (expected DEFAULT|MINIMAL|ALL|NONE or " + all + ")";
        }
    }
    mask |= DEPTH;
    return {};
}

/// Preset name when the mask equals one, else the space-separated token list.
inline std::string formatMask(unsigned mask) {
    mask &= ALL_MASK;
    if (mask == DEFAULT_MASK) return "DEFAULT";
    if (mask == MINIMAL_MASK) return "MINIMAL";
    if (mask == ALL_MASK)     return "ALL";
    const auto& names = tokens();
    std::string out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (mask & (1u << i)) {
            if (!out.empty()) out += ' ';
            out += names[i];
        }
    }
    return out.empty() ? std::string("DEPTH") : out;
}

/// "ALL" for an empty (unfiltered) list, else the space-separated names.
inline std::string formatSpecies(const std::vector<std::string>& species) {
    if (species.empty()) return "ALL";
    std::string out;
    for (const auto& n : species) {
        if (!out.empty()) out += ' ';
        out += n;
    }
    return out;
}

/// Species list from text; "ALL" (alone) → empty list.
inline std::vector<std::string> parseSpecies(const std::string& text) {
    auto v = splitList(text);
    if (v.size() == 1 && iequalsTok(v[0], "ALL")) v.clear();
    return v;
}

/// HH:MM:SS (REPORT_STEP spelling).
inline std::string formatStep(double seconds) {
    const long total = seconds > 0.0 ? static_cast<long>(seconds + 0.5) : 0L;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02ld:%02ld:%02ld",
                  total / 3600, (total % 3600) / 60, total % 60);
    return buf;
}

} // namespace openswmm::twoD::report2d

#endif // OPENSWMM_ENGINE_2D_REPORT2D_VARS_HPP
