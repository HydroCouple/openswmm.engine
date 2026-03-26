/**
 * @file openswmm_controls_impl.cpp
 * @brief C API implementation — control rules and direct link control actions.
 *
 * @see include/openswmm/engine/openswmm_controls.h
 * @ingroup engine_api
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "openswmm_api_common.hpp"
#include "../../../include/openswmm/engine/openswmm_controls.h"

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
