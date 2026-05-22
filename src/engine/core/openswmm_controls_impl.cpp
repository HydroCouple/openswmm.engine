/**
 * @file openswmm_controls_impl.cpp
 * @brief C API implementation — control rules and direct link control actions.
 *
 * @see include/openswmm/engine/openswmm_controls.h
 * @ingroup engine_api
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "openswmm_api_common.hpp"
#include "../../../include/openswmm/engine/openswmm_controls.h"

#include <cctype>
#include <cstring>
#include <string>

namespace {

/// Parse the first whitespace-delimited token that follows the `RULE`
/// keyword (case-insensitive). Returns an empty string when the input has
/// no parseable `RULE <name>` prefix; the caller maps that to
/// `SWMM_ERR_BADPARAM`.
std::string parse_rule_name(const std::string& rule_text) {
    const char* s = rule_text.c_str();
    const char* end = s + rule_text.size();
    // Skip leading whitespace.
    while (s < end && std::isspace(static_cast<unsigned char>(*s))) ++s;
    // Match the literal "RULE" keyword case-insensitively.
    static constexpr char kKw[] = "RULE";
    constexpr int kKwLen = 4;
    if (end - s < kKwLen) return {};
    for (int i = 0; i < kKwLen; ++i) {
        if (std::toupper(static_cast<unsigned char>(s[i])) != kKw[i])
            return {};
    }
    s += kKwLen;
    // Require whitespace after the keyword so we don't match "RULES" etc.
    if (s >= end || !std::isspace(static_cast<unsigned char>(*s))) return {};
    // Skip whitespace between RULE and the name.
    while (s < end && std::isspace(static_cast<unsigned char>(*s))) ++s;
    // Read the name token (until the next whitespace).
    const char* name_begin = s;
    while (s < end && !std::isspace(static_cast<unsigned char>(*s))) ++s;
    if (s == name_begin) return {};
    return std::string(name_begin, static_cast<std::size_t>(s - name_begin));
}

}  // namespace

extern "C" {

// ============================================================================
// Control rules
// ============================================================================

SWMM_ENGINE_API int swmm_control_add_rule(SWMM_Engine engine, const char* rule_text) {
    CHECK_HANDLE(engine);
    if (!rule_text) return SWMM_ERR_BADPARAM;

    auto& ctx = to_engine(engine)->context();
    ctx.control_rules.rule_text.push_back(rule_text);

    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_control_count(SWMM_Engine engine) {
    if (!engine) return -1;
    return to_engine(engine)->context().control_rules.count();
}

SWMM_ENGINE_API int swmm_control_get_rule(SWMM_Engine engine, int idx, char* buf, int buflen) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.control_rules.count());
    if (!buf || buflen <= 0) return SWMM_ERR_BADPARAM;

    const auto& text = ctx.control_rules.rule_text[static_cast<std::size_t>(idx)];
    const int copy_len = std::min(static_cast<int>(text.size()), buflen - 1);
    std::memcpy(buf, text.c_str(), static_cast<std::size_t>(copy_len));
    buf[copy_len] = '\0';

    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_control_get_id(SWMM_Engine engine, int idx, char* buf, int buflen) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.control_rules.count());
    if (!buf || buflen <= 0) return SWMM_ERR_BADPARAM;

    const auto& text = ctx.control_rules.rule_text[static_cast<std::size_t>(idx)];
    const std::string name = parse_rule_name(text);
    if (name.empty()) return SWMM_ERR_BADPARAM;

    const int copy_len = std::min(static_cast<int>(name.size()), buflen - 1);
    std::memcpy(buf, name.c_str(), static_cast<std::size_t>(copy_len));
    buf[copy_len] = '\0';
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_control_clear_rules(SWMM_Engine engine) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    ctx.control_rules.rule_text.clear();
    return SWMM_OK;
}

// ============================================================================
// Direct control actions (without rules)
// ============================================================================

SWMM_ENGINE_API int swmm_control_set_link_setting(SWMM_Engine engine, int link_idx, double setting) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(link_idx >= 0 && link_idx < ctx.n_links());
    ctx.links.setting[static_cast<std::size_t>(link_idx)] = setting;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_control_set_link_status(SWMM_Engine engine, int link_idx, int status) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(link_idx >= 0 && link_idx < ctx.n_links());
    ctx.links.is_closed[static_cast<std::size_t>(link_idx)] = (status == 0);
    return SWMM_OK;
}

} /* extern "C" */
