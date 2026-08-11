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
#include "../controls/Controls.hpp"
#include "../edit/ObjectDeleter.hpp"

#include <algorithm>
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

/// Render a parser rejection as "line N: reason", degrading gracefully when
/// either half is missing. Shared by validate_rule's errbuf and add_rule's
/// engine-error string so both report failures identically.
std::string format_parse_error(const openswmm::controls::ParseError& pe) {
    std::string msg;
    if (pe.line > 0) msg = "line " + std::to_string(pe.line) + ": ";
    msg += pe.message.empty() ? "control-rule parser rejected the rule text"
                              : pe.message;
    return msg;
}

/// Copy `src` into `buf` (capacity `buflen`), truncating as needed and always
/// null-terminating. No-op when the caller passed no buffer.
void copy_to_buffer(const std::string& src, char* buf, int buflen) {
    if (!buf || buflen <= 0) return;
    const int n = std::min(static_cast<int>(src.size()), buflen - 1);
    std::memcpy(buf, src.c_str(), static_cast<std::size_t>(n));
    buf[n] = '\0';
}

}  // namespace

extern "C" {

// ============================================================================
// Control rules
// ============================================================================

SWMM_ENGINE_API int swmm_control_add_rule(SWMM_Engine engine, const char* rule_text) {
    CHECK_HANDLE(engine);
    if (!rule_text) return SWMM_ERR_BADPARAM;

    auto* eng = to_engine(engine);
    auto& ctx = eng->context();

    // Rules are compiled during initialize() (SWMMEngine::initHydraulics), so
    // text added after that point would sit in the store unparsed and never
    // take effect. Compile it into the live engine instead.
    //
    // Before initialize() the text is stored verbatim and left alone: the
    // objects a rule references may legitimately not exist yet (a model under
    // construction adds rules and links in whatever order the caller likes),
    // so parsing here would reject good rules. initialize() does the real
    // compile and reports failures through ctx.error_code / errors.
    const bool defer_compile = (ctx.state == openswmm::EngineState::CREATED ||
                                ctx.state == openswmm::EngineState::BUILDING ||
                                ctx.state == openswmm::EngineState::OPENED);
    if (defer_compile) {
        ctx.control_rules.rule_text.push_back(rule_text);
        return SWMM_OK;
    }

    // Parse into a throwaway engine first: parseRuleText() appends rules as it
    // goes and only rebuilds the evaluation index once the whole block parses,
    // so compiling straight into the live engine would leave it half-mutated
    // on a mid-block rejection. Same trade-off as swmm_control_validate_rule —
    // a block referencing a VARIABLE / EXPRESSION declared in an *earlier*
    // block won't resolve here.
    openswmm::controls::ControlEngine sandbox;
    const int rc = sandbox.parseRuleText(std::string(rule_text), ctx);

    // rc == 0 means the text held no rule (empty, or only VARIABLE /
    // EXPRESSION lines) — nothing would be compiled, so storing it would leave
    // the rule count disagreeing with what the engine actually evaluates.
    if (rc <= 0) {
        ctx.error_code    = SWMM_ERR_BADPARAM;
        ctx.error_message =
            "swmm_control_add_rule: " +
            (rc == 0 ? std::string("no RULE block found in rule text")
                     : format_parse_error(sandbox.lastParseError()));
        return SWMM_ERR_BADPARAM;
    }

    // Known-good — compile into the live engine so the rule takes effect now.
    eng->controlEngine().parseRuleText(std::string(rule_text), ctx);
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

SWMM_ENGINE_API int swmm_control_remove_rule(SWMM_Engine engine, int idx) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.control_rules.count());
    ctx.control_rules.rule_text.erase(
        ctx.control_rules.rule_text.begin() + static_cast<std::ptrdiff_t>(idx));
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_control_find_references(SWMM_Engine engine,
                                                 const char* object_name,
                                                 int* rule_indices_out,
                                                 int* n_inout) {
    CHECK_HANDLE(engine);
    if (!object_name || !n_inout) return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();

    const auto hits = openswmm::edit::find_control_rule_refs(ctx, object_name);
    if (rule_indices_out) {
        const int cap = *n_inout;
        const int n_copy = std::min(cap, static_cast<int>(hits.size()));
        for (int i = 0; i < n_copy; ++i) rule_indices_out[i] = hits[static_cast<std::size_t>(i)];
    }
    *n_inout = static_cast<int>(hits.size());
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_control_validate_rule(SWMM_Engine engine,
                                                const char* rule_text,
                                                char* errbuf, int buflen,
                                                int* line_out) {
    CHECK_HANDLE(engine);
    if (!rule_text) return SWMM_ERR_BADPARAM;

    // Throwaway ControlEngine: parseRuleText mutates only its own rules_ /
    // pid_states_ vectors. Name resolution reads ctx.link_names /
    // ctx.table_names (find() is non-mutating). The live engine's rule list
    // and PID state are untouched.
    auto& ctx = to_engine(engine)->context();
    openswmm::controls::ControlEngine sandbox;
    const int rc = sandbox.parseRuleText(std::string(rule_text), ctx);

    // rc < 0 → parse error; rc == 0 → no rule found (e.g. empty/whitespace-only
    // input). Both fail validation: the validator's contract is "this string
    // is a valid control rule", and zero rules is not a valid rule.
    if (rc <= 0) {
        const auto& pe = sandbox.lastParseError();
        // rc == 0 means nothing was rejected, so there is no offending line.
        if (line_out) *line_out = (rc == 0) ? -1 : pe.line;
        copy_to_buffer(rc == 0 ? std::string("no RULE block found in rule text")
                               : format_parse_error(pe),
                       errbuf, buflen);
        return SWMM_ERR_BADPARAM;
    }

    if (line_out) *line_out = -1;
    if (errbuf && buflen > 0) errbuf[0] = '\0';
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
