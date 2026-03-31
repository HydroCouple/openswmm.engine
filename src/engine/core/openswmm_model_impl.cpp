/**
 * @file openswmm_model_impl.cpp
 * @brief C API implementation — model building, options, user flags, CRS.
 *
 * @see include/openswmm/engine/openswmm_model.h
 * @ingroup engine_api
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "openswmm_api_common.hpp"
#include "InpWriter.hpp"
#include "../../../include/openswmm/engine/openswmm_model.h"

extern "C" {

// ============================================================================
// Model building
// ============================================================================

SWMM_ENGINE_API SWMM_Engine swmm_engine_new(void) {
    try {
        auto* eng = new openswmm::SWMMEngine();
        eng->context().state = openswmm::EngineState::BUILDING;
        return static_cast<SWMM_Engine>(eng);
    } catch (...) {
        return nullptr;
    }
}

SWMM_ENGINE_API int swmm_validate_model(SWMM_Engine engine) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();

    // Must be in BUILDING or OPENED state
    if (ctx.state != openswmm::EngineState::BUILDING &&
        ctx.state != openswmm::EngineState::OPENED)
        return SWMM_ERR_LIFECYCLE;

    // Check: at least one node
    if (ctx.n_nodes() == 0) return SWMM_ERR_BADPARAM;

    // Check: at least one outfall
    bool has_outfall = false;
    for (int i = 0; i < ctx.n_nodes(); ++i) {
        if (ctx.nodes.type[static_cast<std::size_t>(i)] == openswmm::NodeType::OUTFALL) {
            has_outfall = true;
            break;
        }
    }
    if (!has_outfall) return SWMM_ERR_BADPARAM;

    // Check: all links reference valid nodes
    for (int j = 0; j < ctx.n_links(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        int n1 = ctx.links.node1[uj];
        int n2 = ctx.links.node2[uj];
        if (n1 < 0 || n1 >= ctx.n_nodes() || n2 < 0 || n2 >= ctx.n_nodes())
            return SWMM_ERR_BADPARAM;
    }

    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_finalize_model(SWMM_Engine engine) {
    CHECK_HANDLE(engine);
    auto* eng = to_engine(engine);
    auto& ctx = eng->context();

    if (ctx.state != openswmm::EngineState::BUILDING)
        return SWMM_ERR_LIFECYCLE;

    // Validate first
    int rc = swmm_validate_model(engine);
    if (rc != SWMM_OK) return rc;

    // Compute conduit slopes from node inverts + offsets + length
    for (int j = 0; j < ctx.n_links(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (ctx.links.type[uj] != openswmm::LinkType::CONDUIT) continue;

        int n1 = ctx.links.node1[uj];
        int n2 = ctx.links.node2[uj];
        double z1 = ctx.nodes.invert_elev[static_cast<std::size_t>(n1)] + ctx.links.offset1[uj];
        double z2 = ctx.nodes.invert_elev[static_cast<std::size_t>(n2)] + ctx.links.offset2[uj];
        double len = ctx.links.length[uj];
        if (len > 0.0) {
            ctx.links.slope[uj] = (z1 - z2) / len;
        }
    }

    // Transition to INITIALIZED (equivalent to open + initialize for file-based)
    ctx.state = openswmm::EngineState::INITIALIZED;
    ctx.reset_state();
    ctx.dt_output_remaining = ctx.options.report_step;
    ctx.current_date = ctx.options.start_date;
    ctx.current_time = 0.0;

    return SWMM_OK;
}

// ============================================================================
// Model serialisation
// ============================================================================

SWMM_ENGINE_API int swmm_model_write(SWMM_Engine engine, const char* new_inp_path) {
    CHECK_HANDLE(engine);
    if (!new_inp_path) return SWMM_ERR_BADPARAM;
    return openswmm::inp_writer::writeInpFile(to_engine(engine)->context(), new_inp_path);
}

// ============================================================================
// Title / notes access
// ============================================================================

SWMM_ENGINE_API int swmm_title_get_count(SWMM_Engine engine, int* count) {
    CHECK_HANDLE(engine);
    if (!count) return SWMM_ERR_BADPARAM;
    *count = static_cast<int>(to_engine(engine)->context().title_notes.size());
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_title_get_line(SWMM_Engine engine,
                                          int index, char* buf, int buflen) {
    CHECK_HANDLE(engine);
    if (!buf || buflen <= 0) return SWMM_ERR_BADPARAM;
    const auto& notes = to_engine(engine)->context().title_notes;
    if (index < 0 || index >= static_cast<int>(notes.size()))
        return SWMM_ERR_BADPARAM;
    std::strncpy(buf, notes[static_cast<std::size_t>(index)].c_str(),
                 static_cast<std::size_t>(buflen - 1));
    buf[buflen - 1] = '\0';
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_title_add_line(SWMM_Engine engine, const char* line) {
    CHECK_HANDLE(engine);
    if (!line) return SWMM_ERR_BADPARAM;
    to_engine(engine)->context().title_notes.emplace_back(line);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_title_set(SWMM_Engine engine, const char* text) {
    CHECK_HANDLE(engine);
    if (!text) return SWMM_ERR_BADPARAM;
    auto& notes = to_engine(engine)->context().title_notes;
    notes.clear();
    std::string input(text);
    std::size_t pos = 0;
    while (pos < input.size()) {
        auto nl = input.find('\n', pos);
        if (nl == std::string::npos) {
            notes.push_back(input.substr(pos));
            break;
        }
        notes.push_back(input.substr(pos, nl - pos));
        pos = nl + 1;
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_title_clear(SWMM_Engine engine) {
    CHECK_HANDLE(engine);
    to_engine(engine)->context().title_notes.clear();
    return SWMM_OK;
}

// ============================================================================
// OPTIONS access
// ============================================================================

SWMM_ENGINE_API int swmm_options_get(SWMM_Engine engine,
                                      const char* key, char* buf, int buflen) {
    CHECK_HANDLE(engine);
    if (!key || !buf || buflen <= 0) return SWMM_ERR_BADPARAM;

    const auto& opt = to_engine(engine)->context().options;
    std::string val;

    std::string k(key);
    for (auto& c : k) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));

    if      (k == "FLOW_UNITS")    val = std::to_string(static_cast<int>(opt.flow_units));
    else if (k == "FLOW_ROUTING")  val = std::to_string(static_cast<int>(opt.routing_model));
    else if (k == "ROUTING_STEP")  val = std::to_string(opt.routing_step);
    else if (k == "REPORT_STEP")   val = std::to_string(opt.report_step);
    else if (k == "CRS")           val = opt.crs;
    else return SWMM_ERR_BADPARAM;

    std::strncpy(buf, val.c_str(), static_cast<std::size_t>(buflen - 1));
    buf[buflen - 1] = '\0';
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_options_set(SWMM_Engine engine,
                                      const char* key, const char* value) {
    CHECK_HANDLE(engine);
    if (!key || !value) return SWMM_ERR_BADPARAM;

    auto& opt = to_engine(engine)->context().options;
    std::string k(key);
    for (auto& c : k) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    std::string v(value);

    if (k == "FLOW_UNITS") {
        std::string vu(v);
        for (auto& c : vu) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        if      (vu == "CFS") opt.flow_units = openswmm::FlowUnits::CFS;
        else if (vu == "GPM") opt.flow_units = openswmm::FlowUnits::GPM;
        else if (vu == "MGD") opt.flow_units = openswmm::FlowUnits::MGD;
        else if (vu == "CMS") opt.flow_units = openswmm::FlowUnits::CMS;
        else if (vu == "LPS") opt.flow_units = openswmm::FlowUnits::LPS;
        else if (vu == "MLD") opt.flow_units = openswmm::FlowUnits::MLD;
        else return SWMM_ERR_BADPARAM;
    }
    else if (k == "FLOW_ROUTING" || k == "ROUTING_MODEL") {
        std::string vu(v);
        for (auto& c : vu) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        if      (vu == "DYNWAVE")  opt.routing_model = openswmm::RoutingModel::DYNWAVE;
        else if (vu == "KINWAVE")  opt.routing_model = openswmm::RoutingModel::KINWAVE;
        else if (vu == "STEADY")   opt.routing_model = openswmm::RoutingModel::STEADY;
        else return SWMM_ERR_BADPARAM;
    }
    else if (k == "ROUTING_STEP") {
        opt.routing_step = std::stod(v);
    }
    else if (k == "REPORT_STEP") {
        opt.report_step = std::stod(v);
    }
    else if (k == "START_DATE") {
        opt.start_date = std::stod(v);
    }
    else if (k == "END_DATE") {
        opt.end_date = std::stod(v);
    }
    else if (k == "CRS") {
        opt.crs = v;
    }
    else {
        return SWMM_ERR_BADPARAM;
    }

    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_options_get_ext(SWMM_Engine engine,
                                          const char* key, char* buf, int buflen) {
    CHECK_HANDLE(engine);
    if (!key || !buf || buflen <= 0) return SWMM_ERR_BADPARAM;

    const auto& ext = to_engine(engine)->context().options.ext_options;
    auto it = ext.find(key);
    if (it == ext.end()) return SWMM_ERR_BADPARAM;

    std::strncpy(buf, it->second.c_str(), static_cast<std::size_t>(buflen - 1));
    buf[buflen - 1] = '\0';
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_options_set_ext(SWMM_Engine engine,
                                          const char* key, const char* value) {
    CHECK_HANDLE(engine);
    if (!key || !value) return SWMM_ERR_BADPARAM;
    to_engine(engine)->context().options.ext_options[key] = value;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_get_crs(SWMM_Engine engine, char* buf, int buflen) {
    CHECK_HANDLE(engine);
    if (!buf || buflen <= 0) return SWMM_ERR_BADPARAM;
    const auto& crs = to_engine(engine)->context().options.crs;
    if (crs.empty()) return SWMM_ERR_CRS;
    std::strncpy(buf, crs.c_str(), static_cast<std::size_t>(buflen - 1));
    buf[buflen - 1] = '\0';
    return SWMM_OK;
}

// ============================================================================
// User flags
// ============================================================================

SWMM_ENGINE_API int swmm_userflag_get_bool(SWMM_Engine engine,
                                             const char* name, int* value) {
    CHECK_HANDLE(engine);
    if (!name || !value) return SWMM_ERR_BADPARAM;
    const auto& flags = to_engine(engine)->context().user_flags;
    if (!flags.is_defined(name)) return SWMM_ERR_BADPARAM;
    const auto& def = flags.get_def(name);
    if (def.type != openswmm::UserFlagType::BOOLEAN) return SWMM_ERR_BADPARAM;
    auto opt = flags.try_get_value("MODEL", "", name);
    if (!opt.has_value()) { *value = 0; return SWMM_OK; }
    *value = std::get<bool>(opt.value()) ? 1 : 0;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_userflag_get_int(SWMM_Engine engine,
                                            const char* name, int* value) {
    CHECK_HANDLE(engine);
    if (!name || !value) return SWMM_ERR_BADPARAM;
    const auto& flags = to_engine(engine)->context().user_flags;
    if (!flags.is_defined(name)) return SWMM_ERR_BADPARAM;
    const auto& def = flags.get_def(name);
    if (def.type != openswmm::UserFlagType::INTEGER) return SWMM_ERR_BADPARAM;
    auto opt = flags.try_get_value("MODEL", "", name);
    if (!opt.has_value()) { *value = 0; return SWMM_OK; }
    *value = std::get<int>(opt.value());
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_userflag_get_real(SWMM_Engine engine,
                                             const char* name, double* value) {
    CHECK_HANDLE(engine);
    if (!name || !value) return SWMM_ERR_BADPARAM;
    const auto& flags = to_engine(engine)->context().user_flags;
    if (!flags.is_defined(name)) return SWMM_ERR_BADPARAM;
    const auto& def = flags.get_def(name);
    if (def.type != openswmm::UserFlagType::REAL) return SWMM_ERR_BADPARAM;
    auto opt = flags.try_get_value("MODEL", "", name);
    if (!opt.has_value()) { *value = 0.0; return SWMM_OK; }
    *value = std::get<double>(opt.value());
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_userflag_set_bool(SWMM_Engine engine,
                                             const char* name, int value) {
    CHECK_HANDLE(engine);
    if (!name) return SWMM_ERR_BADPARAM;
    auto& flags = to_engine(engine)->context().user_flags;
    flags.set("MODEL", "", name, value != 0);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_userflag_set_int(SWMM_Engine engine,
                                            const char* name, int value) {
    CHECK_HANDLE(engine);
    if (!name) return SWMM_ERR_BADPARAM;
    auto& flags = to_engine(engine)->context().user_flags;
    flags.set("MODEL", "", name, value);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_userflag_set_real(SWMM_Engine engine,
                                             const char* name, double value) {
    CHECK_HANDLE(engine);
    if (!name) return SWMM_ERR_BADPARAM;
    auto& flags = to_engine(engine)->context().user_flags;
    flags.set("MODEL", "", name, value);
    return SWMM_OK;
}

} /* extern "C" */
