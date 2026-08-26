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
 * @file openswmm_model_impl.cpp
 * @brief C API implementation — model building, options, user flags, CRS.
 *
 * @see include/openswmm/engine/openswmm_model.h
 * @ingroup engine_api
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "openswmm_api_common.hpp"
#include "InpWriter.hpp"
#include "DateTime.hpp"
#include "charconv_compat.hpp"
#include "../input/Tokenizer.hpp"
#include "../input/InputParseUtils.hpp"
#include "../input/PostParseResolver.hpp"
#include "../../../include/openswmm/engine/openswmm_model.h"
#include "../../../include/openswmm/engine/openswmm_hotstart.h"
#include "../../../include/openswmm/plugin_sdk/IInputPlugin.hpp"
#include "../../../include/openswmm/plugin_sdk/IPluginComponentInfo.hpp"

#ifdef OPENSWMM_HAS_2D
// [2D_OPTIONS] key routing for swmm_options_get_ext / swmm_options_set_ext
// (is2DOptionKey / format2DOptionValue / parse2DOptionsLine).
#include "../2d/input/SectionHandlers2D.hpp"
#endif

#include <cstdio>
#include <sstream>

namespace {

// Split a space-separated args string into a vector<string>, dropping
// empty tokens.  Mirrors the [PLUGINS]-line tokenizer for the common
// (no-quoting) case the GUI's free-form args field produces.
std::vector<std::string> split_args(const char* s) {
    std::vector<std::string> out;
    if (!s || s[0] == '\0') return out;
    std::istringstream is(s);
    std::string tok;
    while (is >> tok) out.push_back(std::move(tok));
    return out;
}

// Inverse: join init_args with single spaces.
std::string join_args(const std::vector<std::string>& args) {
    std::string out;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i) out += ' ';
        out += args[i];
    }
    return out;
}

// Common buffer-fill helper: NUL-terminated, truncated at sz - 1.
void fill_buf(char* buf, int sz, const std::string& s) {
    if (!buf || sz <= 0) return;
    std::size_t n = std::min(static_cast<std::size_t>(sz - 1), s.size());
    std::memcpy(buf, s.data(), n);
    buf[n] = '\0';
}

// [FILES] helpers — kept outside extern "C" so std::string return
// types don't trigger the -Wreturn-type-c-linkage warning.
const char* file_mode_to_str(openswmm::FileMode m) noexcept {
    switch (m) {
        case openswmm::FileMode::SAVE: return "SAVE";
        case openswmm::FileMode::USE:  return "USE";
        case openswmm::FileMode::NONE:
        default:                       return "";
    }
}

bool file_mode_from_str(const char* s, openswmm::FileMode& out) {
    if (!s) return false;
    std::string up(s);
    for (auto& c : up) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    if      (up.empty())     out = openswmm::FileMode::NONE;
    else if (up == "SAVE")   out = openswmm::FileMode::SAVE;
    else if (up == "USE")    out = openswmm::FileMode::USE;
    else if (up == "NONE")   out = openswmm::FileMode::NONE;
    else                     return false;
    return true;
}

std::string upper_key(const char* key) {
    std::string k(key);
    for (auto& c : k) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return k;
}

std::string upper_copy(const std::string& v) { return upper_key(v.c_str()); }

// ── Strict numeric parses for the options dispatch (H1) ────────────────
// The exception guard in swmm_options_set catches what THROWS — but two
// parse families never do: input::parse_time_seconds fabricates 0.0 from
// junk (and "1e999999" becomes 3600 s through its H[:M] fallthrough), and
// std::stoi/stol accept a numeric PREFIX ("1e999999" parses as 1). An API
// caller must get SWMM_ERR_BADPARAM, never a silent guess. Deck parsing
// (OptionsHandler) keeps the lenient forms deliberately — file semantics
// are parity-bound; only the C API is strict.
double stod_strict(const std::string& v) {
    std::size_t pos = 0;
    const double d = std::stod(v, &pos);              // throws on junk/overflow
    while (pos < v.size() &&
           isspace(static_cast<unsigned char>(v[pos]))) ++pos;
    if (pos != v.size()) throw std::invalid_argument("trailing characters");
    return d;
}
int stoi_strict(const std::string& v) {
    std::size_t pos = 0;
    const int i = std::stoi(v, &pos);
    while (pos < v.size() &&
           isspace(static_cast<unsigned char>(v[pos]))) ++pos;
    if (pos != v.size()) throw std::invalid_argument("trailing characters");
    return i;
}
long stol_strict(const std::string& v) {
    std::size_t pos = 0;
    const long l = std::stol(v, &pos);
    while (pos < v.size() &&
           isspace(static_cast<unsigned char>(v[pos]))) ++pos;
    if (pos != v.size()) throw std::invalid_argument("trailing characters");
    return l;
}
// Exactly the forms input::parse_time_seconds documents — a plain number
// of seconds, or H:M[:S] with digit-only H and M — validated to full
// consumption and computed with the SAME expression, so a well-formed
// value is bit-identical to the deck parser's result.
double time_seconds_strict(const std::string& v) {
    try { return stod_strict(v); } catch (const std::exception&) {}
    const auto c1 = v.find(':');
    if (c1 == std::string::npos)
        throw std::invalid_argument("neither seconds nor H:M[:S]");
    auto uint_strict = [](const std::string& f) -> unsigned long {
        if (f.empty()) throw std::invalid_argument("empty time field");
        for (const char ch : f)
            if (!isdigit(static_cast<unsigned char>(ch)))
                throw std::invalid_argument("non-digit time field");
        return std::stoul(f);
    };
    const unsigned long h = uint_strict(v.substr(0, c1));
    const auto c2 = v.find(':', c1 + 1);
    unsigned long m = 0;
    double s = 0.0;
    if (c2 == std::string::npos) {
        m = uint_strict(v.substr(c1 + 1));
    } else {
        m = uint_strict(v.substr(c1 + 1, c2 - c1 - 1));
        s = stod_strict(v.substr(c2 + 1));
    }
    return h * 3600.0 + m * 60.0 + s;
}

} // anonymous

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

    // Resolve cross-references exactly as the file-based open() path does:
    // final SoA sizing, quality-matrix allocation, display→internal unit
    // conversion, and all derived geometry (conduit slope/conveyance, xsect
    // a_full/s_full, head initialization, outfall normal depth).  Without this
    // the programmatically-built model is missing the derived state the router
    // and computational modules depend on.
    openswmm::input::resolve_cross_references(ctx);

    // Run the same runtime initialization as swmm_engine_initialize(): initial
    // node volumes / link flows plus computational-module and forcing-array
    // allocation (init_modules).  initialize() requires the OPENED state, so
    // present the finalized-build context as OPENED before delegating; it
    // transitions the engine to INITIALIZED on success.
    ctx.state = openswmm::EngineState::OPENED;
    rc = eng->initialize();
    if (rc != SWMM_OK) return rc;

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

SWMM_ENGINE_API int swmm_model_write_with_plugin(SWMM_Engine engine,
                                                  const char* new_path,
                                                  const char* output_plugin_id) {
    CHECK_HANDLE(engine);
    if (!new_path) return SWMM_ERR_BADPARAM;

    // Empty / NULL plugin id → built-in .inp writer.
    if (!output_plugin_id || output_plugin_id[0] == '\0') {
        return openswmm::inp_writer::writeInpFile(
            to_engine(engine)->context(), new_path);
    }

    auto* eng = to_engine(engine);

    // Resolve the writer plugin via the same id/path logic used by
    // swmm_engine_open's input_plugin_lib argument.  No load warnings
    // emitted to the engine's warn channel here — the GUI surfaces
    // resolution failures via the SWMM_ERR_BADPARAM return code.
    openswmm::IPluginComponentInfo* info =
        eng->plugin_factory().find_component(output_plugin_id);
    if (!info) return SWMM_ERR_BADPARAM;
    if (!info->has_input()) return SWMM_ERR_PLUGIN;

    // Always create a transient writer instance so the engine's primary
    // input plugin (which may be in any state) is left undisturbed.
    openswmm::IInputPlugin* writer = info->create_input_plugin();
    if (!writer) return SWMM_ERR_PLUGIN;

    int rc = writer->initialize({}, info);
    if (rc == 0)
        rc = writer->write(new_path, eng->context());
    // Best-effort finalize; ignore its return so we surface the write rc.
    (void)writer->finalize(eng->context());
    delete writer;

    return rc == 0 ? SWMM_OK : SWMM_ERR_PLUGIN;
}

// ============================================================================
// [PLUGINS] section accessors (Slice AA-3.1 Phase B)
// ============================================================================

SWMM_ENGINE_API int swmm_plugins_count(SWMM_Engine engine, int* count) {
    CHECK_HANDLE(engine);
    if (!count) return SWMM_ERR_BADPARAM;
    *count = static_cast<int>(to_engine(engine)->context().plugin_specs.size());
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_plugin_get(SWMM_Engine engine,
                                     int          idx,
                                     char*        path_buf,
                                     int          path_buf_sz,
                                     char*        args_buf,
                                     int          args_buf_sz) {
    CHECK_HANDLE(engine);
    const auto& specs = to_engine(engine)->context().plugin_specs;
    if (idx < 0 || idx >= static_cast<int>(specs.size())) return SWMM_ERR_BADINDEX;

    const auto& spec = specs[static_cast<std::size_t>(idx)];
    fill_buf(path_buf, path_buf_sz, spec.path);
    fill_buf(args_buf, args_buf_sz, join_args(spec.init_args));
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_plugin_set(SWMM_Engine engine,
                                     const char* path_or_id,
                                     const char* args) {
    CHECK_HANDLE(engine);
    if (!path_or_id || path_or_id[0] == '\0') return SWMM_ERR_BADPARAM;

    auto& specs = to_engine(engine)->context().plugin_specs;
    const std::string key(path_or_id);
    auto tokens = split_args(args);

    for (auto& spec : specs) {
        if (spec.path == key) {
            spec.init_args = std::move(tokens);
            return SWMM_OK;
        }
    }

    openswmm::PluginSpec spec;
    spec.path = key;
    spec.init_args = std::move(tokens);
    specs.push_back(std::move(spec));
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_plugin_remove(SWMM_Engine engine, const char* path_or_id) {
    CHECK_HANDLE(engine);
    if (!path_or_id) return SWMM_ERR_BADPARAM;

    auto& specs = to_engine(engine)->context().plugin_specs;
    const std::string key(path_or_id);
    for (auto it = specs.begin(); it != specs.end(); ++it) {
        if (it->path == key) { specs.erase(it); break; }
    }
    return SWMM_OK;  // idempotent
}

// ============================================================================
// [FILES] section accessors (Slice AA-3 follow-up)
// ============================================================================

SWMM_ENGINE_API int swmm_files_get(SWMM_Engine engine,
                                    const char* key, char* buf, int buflen) {
    CHECK_HANDLE(engine);
    if (!key || !buf || buflen <= 0) return SWMM_ERR_BADPARAM;

    const auto& f = to_engine(engine)->context().files;
    const std::string k = upper_key(key);
    std::string val;

    if      (k == "RAINFALL_PATH")           val = f.rainfall_path;
    else if (k == "RAINFALL_MODE")           val = file_mode_to_str(f.rainfall_mode);
    else if (k == "RUNOFF_PATH")             val = f.runoff_path;
    else if (k == "RUNOFF_MODE")             val = file_mode_to_str(f.runoff_mode);
    else if (k == "RDII_PATH")               val = f.rdii_path;
    else if (k == "RDII_MODE")               val = file_mode_to_str(f.rdii_mode);
    else if (k == "INFLOWS_PATH")            val = f.inflows_path;
    else if (k == "OUTFLOWS_PATH")           val = f.outflows_path;
    else if (k == "HOTSTART_USE_PATH")       val = f.hotstart_use_path;
    // HOTSTART_SAVE_* operate on slot 0 of the vector as back-compat
    // sugar for clients that only surface a single hot-start save.
    else if (k == "HOTSTART_SAVE_PATH")
        // Explicit .str() avoids the ternary-ambiguity that arises from
        // FilePathPair's two-way string convertibility (Slice IO-2).
        val = f.hotstart_saves.empty() ? std::string{}
                                       : f.hotstart_saves.front().path.str();
    else if (k == "HOTSTART_SAVE_DATETIME")
        val = std::to_string(f.hotstart_saves.empty()
                              ? 0.0 : f.hotstart_saves.front().datetime);
    else if (k == "HOTSTART_SAVE_COUNT")
        val = std::to_string(static_cast<int>(f.hotstart_saves.size()));
    else return SWMM_ERR_BADPARAM;

    fill_buf(buf, buflen, val);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_files_set(SWMM_Engine engine,
                                    const char* key, const char* value) {
    CHECK_HANDLE(engine);
    if (!key || !value) return SWMM_ERR_BADPARAM;

    auto& f = to_engine(engine)->context().files;
    const std::string k = upper_key(key);
    const std::string v(value);

    auto set_mode = [&](openswmm::FileMode& slot) -> int {
        if (!file_mode_from_str(value, slot)) return SWMM_ERR_BADPARAM;
        return SWMM_OK;
    };

    if      (k == "RAINFALL_PATH")           f.rainfall_path = v;
    else if (k == "RAINFALL_MODE")           return set_mode(f.rainfall_mode);
    else if (k == "RUNOFF_PATH")             f.runoff_path = v;
    else if (k == "RUNOFF_MODE")             return set_mode(f.runoff_mode);
    else if (k == "RDII_PATH")               f.rdii_path = v;
    else if (k == "RDII_MODE")               return set_mode(f.rdii_mode);
    else if (k == "INFLOWS_PATH")            f.inflows_path = v;
    else if (k == "OUTFLOWS_PATH")           f.outflows_path = v;
    else if (k == "HOTSTART_USE_PATH")       f.hotstart_use_path = v;
    // HOTSTART_SAVE_PATH operates on slot 0 of the vector.  Empty
    // value clears slot 0 (and removes the row if its datetime is
    // also zero) so existing single-slot client code keeps working.
    else if (k == "HOTSTART_SAVE_PATH") {
        if (f.hotstart_saves.empty()) f.hotstart_saves.emplace_back();
        f.hotstart_saves.front().path = v;
        if (v.empty() && f.hotstart_saves.front().datetime == 0.0)
            f.hotstart_saves.erase(f.hotstart_saves.begin());
    }
    else if (k == "HOTSTART_SAVE_DATETIME") {
        double dt = 0.0;
        // H1 §7 residue: std::stod alone accepts a numeric PREFIX, so
        // "1.5abc" stored 1.5 and "01/01/2026" stored 1.0 silently. The
        // contract is a decimal-day floating-point string — strict.
        try { dt = v.empty() ? 0.0 : stod_strict(v); }
        catch (...) { return SWMM_ERR_BADPARAM; }
        if (f.hotstart_saves.empty()) f.hotstart_saves.emplace_back();
        f.hotstart_saves.front().datetime = dt;
        if (f.hotstart_saves.front().path.empty() && dt == 0.0)
            f.hotstart_saves.erase(f.hotstart_saves.begin());
    }
    else return SWMM_ERR_BADPARAM;

    return SWMM_OK;
}

// ============================================================================
// External-file path slots — typed accessors (Slice IO-9)
//
// One uniform pair of (get, set) endpoints over every external-file slot
// on the in-memory model, regardless of which struct holds it. Reaches:
//   - FilesSpec slots (rainfall/runoff/rdii/inflows/outflows/hotstart_use)
//   - Hot-start save vector entries (by decimal index)
//   - Per-gage raingage file slots   (by gage id)
//   - Per-series timeseries file slots (by series id)
//   - Climate temp_file slot         (scalar)
//
// `get` returns both `.absolute` (resolved, ready for fopen) and
// `.original` (token as authored, possibly relative). `set` updates
// `.original`; FilePathPair::operator= clears `.absolute` so the next
// PostParseResolver pass refills it from the new token.
//
// See include/openswmm/engine/openswmm_model.h for the public contract
// and openswmm.gui/docs/IO_PORTABILITY_PLAN.md §3.3.
// ============================================================================

namespace {

// Resolve a (role, owner) pair to a slot pointer. Returns nullptr when the
// owner is missing in the model. `mutable_ctx` toggles read vs write — both
// share the same dispatch table; the const cast is contained here.
openswmm::FilePathPair* resolve_slot(SWMM_Engine             engine,
                                      SWMM_FilePathRole       role,
                                      const char*             owner) {
    auto& ctx = to_engine(engine)->context();
    switch (role) {
        case SWMM_FILE_RAINFALL:      return &ctx.files.rainfall_path;
        case SWMM_FILE_RUNOFF:        return &ctx.files.runoff_path;
        case SWMM_FILE_RDII:          return &ctx.files.rdii_path;
        case SWMM_FILE_INFLOWS:       return &ctx.files.inflows_path;
        case SWMM_FILE_OUTFLOWS:      return &ctx.files.outflows_path;
        case SWMM_FILE_HOTSTART_USE:  return &ctx.files.hotstart_use_path;
        case SWMM_FILE_CLIMATE_TEMP:  return &ctx.options.temp_file;

        case SWMM_FILE_HOTSTART_SAVE: {
            if (!owner) return nullptr;
            int idx = 0;
            // H1 §7 residue: a partial parse ("1.5") resolved to slot 1
            // instead of failing. The owner contract is a decimal index.
            try { idx = stoi_strict(owner); } catch (...) { return nullptr; }
            if (idx < 0 ||
                static_cast<std::size_t>(idx) >= ctx.files.hotstart_saves.size())
                return nullptr;
            return &ctx.files.hotstart_saves[
                static_cast<std::size_t>(idx)].path;
        }
        case SWMM_FILE_RAINGAGE_DATA: {
            if (!owner) return nullptr;
            int idx = ctx.gage_names.find(owner);
            if (idx < 0 ||
                static_cast<std::size_t>(idx) >= ctx.gages.file_path.size())
                return nullptr;
            return &ctx.gages.file_path[static_cast<std::size_t>(idx)];
        }
        case SWMM_FILE_TIMESERIES_DATA: {
            if (!owner) return nullptr;
            int idx = ctx.find_timeseries(owner);
            if (idx < 0 || idx >= static_cast<int>(ctx.tables.tables.size()))
                return nullptr;
            return &ctx.tables.tables[static_cast<std::size_t>(idx)].file_path;
        }

        case SWMM_FILE_MESH_2D:
#ifdef OPENSWMM_HAS_2D
            return ctx.twod_io.options ? &ctx.twod_io.options->mesh_file
                                       : nullptr;
#else
            return nullptr;
#endif
        case SWMM_FILE_OUTPUT_2D:
#ifdef OPENSWMM_HAS_2D
            return ctx.twod_io.options ? &ctx.twod_io.options->output_file
                                       : nullptr;
#else
            return nullptr;
#endif
        case SWMM_FILE_LID_REPORT: {
            if (!owner) return nullptr;
            int idx = 0;
            // H1 §7 residue: a partial parse ("1.5") resolved to slot 1
            // instead of failing. The owner contract is a decimal index.
            try { idx = stoi_strict(owner); } catch (...) { return nullptr; }
            if (idx < 0 ||
                static_cast<std::size_t>(idx) >= ctx.lid_usage.rpt_file.size())
                return nullptr;
            return &ctx.lid_usage.rpt_file[static_cast<std::size_t>(idx)];
        }
    }
    return nullptr;
}

} // anonymous

SWMM_ENGINE_API int
swmm_file_path_get(SWMM_Engine          engine,
                   SWMM_FilePathRole    role,
                   const char*          owner,
                   char*                absolute_buf,
                   int                  absolute_buflen,
                   char*                original_buf,
                   int                  original_buflen) {
    CHECK_HANDLE(engine);
    if (!absolute_buf || absolute_buflen <= 0 ||
        !original_buf || original_buflen <= 0) {
        return SWMM_ERR_BADPARAM;
    }
    openswmm::FilePathPair* slot = resolve_slot(engine, role, owner);
    if (!slot) return SWMM_ERR_BADPARAM;
    fill_buf(absolute_buf, absolute_buflen, slot->absolute);
    fill_buf(original_buf, original_buflen, slot->original);
    return SWMM_OK;
}

SWMM_ENGINE_API int
swmm_file_path_set(SWMM_Engine          engine,
                   SWMM_FilePathRole    role,
                   const char*          owner,
                   const char*          new_path) {
    CHECK_HANDLE(engine);
    if (!new_path) return SWMM_ERR_BADPARAM;
    openswmm::FilePathPair* slot = resolve_slot(engine, role, owner);
    if (!slot) return SWMM_ERR_BADPARAM;
    // FilePathPair::operator=(std::string) clears `.absolute` so callers
    // see a stale resolution gone after the assignment.
    *slot = std::string(new_path);
    return SWMM_OK;
}

// ============================================================================
// [FILES] section — multi-slot SAVE HOTSTART entries (Slice BV-01)
//
// Generalizes the slot-0 HOTSTART_SAVE_* sugar above to address the full
// hotstart_saves vector by index.  See openswmm_hotstart.h for the public
// API contract.
// ============================================================================

namespace {

// Bounds-checked accessor.  Returns nullptr if idx out of range or engine
// invalid.  Caller must have already done CHECK_HANDLE(engine).
openswmm::HotstartSaveEntry* hs_save_at(SWMM_Engine engine, int idx) noexcept {
    auto& v = to_engine(engine)->context().files.hotstart_saves;
    if (idx < 0 || static_cast<std::size_t>(idx) >= v.size()) return nullptr;
    return &v[static_cast<std::size_t>(idx)];
}

} // anonymous

SWMM_ENGINE_API int swmm_hotstart_saves_count(SWMM_Engine engine, int* count) {
    CHECK_HANDLE(engine);
    if (!count) return SWMM_ERR_BADPARAM;
    *count = static_cast<int>(
        to_engine(engine)->context().files.hotstart_saves.size());
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_hotstart_saves_get_path(
    SWMM_Engine engine, int idx, char* buf, int buflen)
{
    CHECK_HANDLE(engine);
    if (!buf || buflen <= 0) return SWMM_ERR_BADPARAM;
    auto* e = hs_save_at(engine, idx);
    if (!e) return SWMM_ERR_BADPARAM;
    fill_buf(buf, buflen, e->path);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_hotstart_saves_get_datetime(
    SWMM_Engine engine, int idx, double* datetime)
{
    CHECK_HANDLE(engine);
    if (!datetime) return SWMM_ERR_BADPARAM;
    auto* e = hs_save_at(engine, idx);
    if (!e) return SWMM_ERR_BADPARAM;
    *datetime = e->datetime;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_hotstart_saves_set_path(
    SWMM_Engine engine, int idx, const char* path)
{
    CHECK_HANDLE(engine);
    if (!path) return SWMM_ERR_BADPARAM;
    auto* e = hs_save_at(engine, idx);
    if (!e) return SWMM_ERR_BADPARAM;
    e->path = path;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_hotstart_saves_set_datetime(
    SWMM_Engine engine, int idx, double datetime)
{
    CHECK_HANDLE(engine);
    auto* e = hs_save_at(engine, idx);
    if (!e) return SWMM_ERR_BADPARAM;
    e->datetime = datetime;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_hotstart_saves_add(
    SWMM_Engine engine, const char* path, double datetime)
{
    CHECK_HANDLE(engine);
    if (!path) return SWMM_ERR_BADPARAM;
    to_engine(engine)->context().files.hotstart_saves.push_back(
        openswmm::HotstartSaveEntry{std::string(path), datetime});
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_hotstart_saves_remove(SWMM_Engine engine, int idx) {
    CHECK_HANDLE(engine);
    auto& v = to_engine(engine)->context().files.hotstart_saves;
    if (idx < 0 || static_cast<std::size_t>(idx) >= v.size())
        return SWMM_ERR_BADPARAM;
    v.erase(v.begin() + idx);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_hotstart_saves_clear(SWMM_Engine engine) {
    CHECK_HANDLE(engine);
    to_engine(engine)->context().files.hotstart_saves.clear();
    return SWMM_OK;
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

    // Encoding contract: get/set are symmetric — the value returned here MUST
    // be a string that swmm_options_set accepts for the same key. The GUI
    // (UnitSystem::syncFromEngine, SimulationOptionsDialog::readFromEngine)
    // and the Python bindings rely on this round-trip. Enum keys therefore
    // return the canonical token, never the underlying int.
    if      (k == "FLOW_UNITS") {
        switch (opt.flow_units) {
            case openswmm::FlowUnits::CFS: val = "CFS"; break;
            case openswmm::FlowUnits::GPM: val = "GPM"; break;
            case openswmm::FlowUnits::MGD: val = "MGD"; break;
            case openswmm::FlowUnits::CMS: val = "CMS"; break;
            case openswmm::FlowUnits::LPS: val = "LPS"; break;
            case openswmm::FlowUnits::MLD: val = "MLD"; break;
        }
    }
    else if (k == "FLOW_ROUTING" || k == "ROUTING_MODEL") {
        switch (opt.routing_model) {
            case openswmm::RoutingModel::STEADY:  val = "STEADY";  break;
            case openswmm::RoutingModel::KINWAVE: val = "KINWAVE"; break;
            case openswmm::RoutingModel::DYNWAVE: val = "DYNWAVE"; break;
            case openswmm::RoutingModel::FV:      val = "FV"; break;
        }
    }
    else if (k == "LINK_OFFSETS")  val = (opt.link_offsets == 1) ? "ELEVATION" : "DEPTH";
    else if (k == "ROUTING_STEP")  val = std::to_string(opt.routing_step);
    else if (k == "REPORT_STEP")   val = std::to_string(opt.report_step);
    else if (k == "CRS")           val = opt.crs;

    // [REPORT] section keys (Slice BV.1 — added 2026-05-22).
    // Bool keys serialize as YES/NO; selector keys serialize as
    // ALL / NONE / comma-joined names.  Symmetric with swmm_options_set.
    else if (k == "RPT_DISABLED")   val = opt.rpt_disabled   ? "YES" : "NO";
    else if (k == "RPT_INPUT")      val = opt.rpt_input      ? "YES" : "NO";
    else if (k == "RPT_CONTINUITY") val = opt.rpt_continuity ? "YES" : "NO";
    else if (k == "RPT_FLOWSTATS")  val = opt.rpt_flowstats  ? "YES" : "NO";
    else if (k == "RPT_CONTROLS")   val = opt.rpt_controls   ? "YES" : "NO";
    else if (k == "RPT_AVERAGES")   val = opt.rpt_averages   ? "YES" : "NO";
    else if (k == "RPT_SUBCATCHMENTS") {
        if      (opt.rpt_subcatchments == 0) val = "NONE";
        else if (opt.rpt_subcatchments == 1) val = "ALL";
        else {
            for (size_t i = 0; i < opt.rpt_subcatch_names.size(); ++i) {
                if (i) val += ',';
                val += opt.rpt_subcatch_names[i];
            }
        }
    }
    else if (k == "RPT_NODES") {
        if      (opt.rpt_nodes == 0) val = "NONE";
        else if (opt.rpt_nodes == 1) val = "ALL";
        else {
            for (size_t i = 0; i < opt.rpt_node_names.size(); ++i) {
                if (i) val += ',';
                val += opt.rpt_node_names[i];
            }
        }
    }
    else if (k == "RPT_LINKS") {
        if      (opt.rpt_links == 0) val = "NONE";
        else if (opt.rpt_links == 1) val = "ALL";
        else {
            for (size_t i = 0; i < opt.rpt_link_names.size(); ++i) {
                if (i) val += ',';
                val += opt.rpt_link_names[i];
            }
        }
    }

    // ------------------------------------------------------------------
    // Slice CY (2026-05-22) — Close the §M.4 deferred OPTIONS keys so the
    // SimulationOptionsDialog can hydrate every dialog control from the
    // active engine on project open. Encoding is symmetric with the
    // companion swmm_options_set branches further down. See
    // openswmm.gui/docs/GUI_IMPLEMENTATION_PLAN.md §M.4 / Slice CY.
    // ------------------------------------------------------------------

    // Dates & Times — date+time are stored combined in a single OADate
    // double (date in integer part, time-of-day in fractional). Get
    // returns the date portion for *_DATE keys and the time portion for
    // *_TIME keys so the GUI can populate START/END/REPORT controls
    // independently.
    else if (k == "START_DATE") {
        int y, m, d;
        openswmm::datetime::decodeDate(opt.start_date, y, m, d);
        char tmp[16];
        std::snprintf(tmp, sizeof(tmp), "%02d/%02d/%04d", m, d, y);
        val = tmp;
    }
    else if (k == "START_TIME") {
        int h, m, s;
        openswmm::datetime::decodeTime(opt.start_date, h, m, s);
        char tmp[16];
        std::snprintf(tmp, sizeof(tmp), "%02d:%02d:%02d", h, m, s);
        val = tmp;
    }
    else if (k == "END_DATE") {
        int y, m, d;
        openswmm::datetime::decodeDate(opt.end_date, y, m, d);
        char tmp[16];
        std::snprintf(tmp, sizeof(tmp), "%02d/%02d/%04d", m, d, y);
        val = tmp;
    }
    else if (k == "END_TIME") {
        int h, m, s;
        openswmm::datetime::decodeTime(opt.end_date, h, m, s);
        char tmp[16];
        std::snprintf(tmp, sizeof(tmp), "%02d:%02d:%02d", h, m, s);
        val = tmp;
    }
    else if (k == "REPORT_START_DATE") {
        int y, m, d;
        openswmm::datetime::decodeDate(opt.report_start, y, m, d);
        char tmp[16];
        std::snprintf(tmp, sizeof(tmp), "%02d/%02d/%04d", m, d, y);
        val = tmp;
    }
    else if (k == "REPORT_START_TIME") {
        int h, m, s;
        openswmm::datetime::decodeTime(opt.report_start, h, m, s);
        char tmp[16];
        std::snprintf(tmp, sizeof(tmp), "%02d:%02d:%02d", h, m, s);
        val = tmp;
    }

    // Step keys — emitted as integer seconds (GUI parseStepSeconds
    // accepts both seconds and HH:MM:SS; seconds matches REPORT_STEP).
    else if (k == "DRY_STEP")  val = std::to_string(static_cast<long long>(opt.dry_step));
    else if (k == "WET_STEP")  val = std::to_string(static_cast<long long>(opt.wet_step));
    else if (k == "RULE_STEP") val = std::to_string(static_cast<long long>(opt.rule_step));
    else if (k == "DRY_DAYS")  val = std::to_string(opt.dry_days);

    // Sweep day-of-year → MM/DD via year-2000 anchor (matches the
    // OptionsHandler parser's convention).
    else if (k == "SWEEP_START") {
        const auto dt = openswmm::datetime::encodeDate(2000, 1, 1)
                      + (opt.sweep_start - 1);
        int y, m, d;
        openswmm::datetime::decodeDate(dt, y, m, d);
        char tmp[8];
        std::snprintf(tmp, sizeof(tmp), "%02d/%02d", m, d);
        val = tmp;
    }
    else if (k == "SWEEP_END") {
        const auto dt = openswmm::datetime::encodeDate(2000, 1, 1)
                      + (opt.sweep_end - 1);
        int y, m, d;
        openswmm::datetime::decodeDate(dt, y, m, d);
        char tmp[8];
        std::snprintf(tmp, sizeof(tmp), "%02d/%02d", m, d);
        val = tmp;
    }

    // Models / Processes — INFILTRATION + booleans.
    else if (k == "INFILTRATION") {
        switch (opt.infiltration) {
            case openswmm::InfiltrationModel::HORTON:         val = "HORTON"; break;
            case openswmm::InfiltrationModel::MOD_HORTON:     val = "MOD_HORTON"; break;
            case openswmm::InfiltrationModel::GREEN_AMPT:     val = "GREEN_AMPT"; break;
            case openswmm::InfiltrationModel::MOD_GREEN_AMPT: val = "MOD_GREEN_AMPT"; break;
            case openswmm::InfiltrationModel::CURVE_NUMBER:   val = "CURVE_NUMBER"; break;
        }
    }
    else if (k == "ALLOW_PONDING")      val = opt.allow_ponding     ? "YES" : "NO";
    else if (k == "SKIP_STEADY_STATE")  val = opt.skip_steady_state ? "YES" : "NO";
    else if (k == "IGNORE_RAINFALL")    val = opt.ignore_rainfall   ? "YES" : "NO";
    else if (k == "IGNORE_SNOWMELT")    val = opt.ignore_snow_melt  ? "YES" : "NO";
    else if (k == "IGNORE_GROUNDWATER") val = opt.ignore_groundwater? "YES" : "NO";
    else if (k == "IGNORE_RDII")        val = opt.ignore_rdii       ? "YES" : "NO";
    else if (k == "IGNORE_QUALITY")     val = opt.ignore_quality    ? "YES" : "NO";
    else if (k == "IGNORE_ROUTING")     val = opt.ignore_routing    ? "YES" : "NO";
    else if (k == "IGNORE_2D")          val = opt.ignore_2d         ? "YES" : "NO";

    // Routing & Hydraulics — enums + scalars.
    else if (k == "SURCHARGE_METHOD") {
        if      (opt.surcharge_method == 0) val = "EXTRAN";
        else if (opt.surcharge_method == 1) val = "SLOT";
        else                                val = "DYNAMIC_SLOT";
    }
    else if (k == "NODE_CONTINUITY") {
        val = (opt.node_continuity == openswmm::NodeContinuity::SEMI_IMPLICIT)
              ? "SEMI_IMPLICIT" : "EXPLICIT";
    }
    else if (k == "FORCE_MAIN_EQUATION") {
        val = (opt.force_main_eqn == 1) ? "D-W" : "H-W";
    }
    else if (k == "NORMAL_FLOW_LIMITED") {
        switch (opt.normal_flow_ltd) {
            case 0:  val = "SLOPE";   break;
            case 1:  val = "FROUDE";  break;
            case 2:  val = "BOTH";    break;
            default: val = "NEITHER"; break;
        }
    }
    else if (k == "INERTIAL_DAMPING") {
        switch (opt.inertial_damping) {
            case 0:  val = "NONE";    break;
            case 1:  val = "PARTIAL"; break;
            default: val = "FULL";    break;
        }
    }
    else if (k == "ANDERSON_ACCEL")    val = opt.anderson_accel ? "YES" : "NO";
    else if (k == "DPS_CELERITY")      val = std::to_string(opt.dps_target_celerity);
    else if (k == "DPS_ALPHA")         val = std::to_string(opt.dps_alpha);
    else if (k == "DPS_DECAY_TIME")    val = std::to_string(opt.dps_decay_time);
    else if (k == "LENGTHENING_STEP")  val = std::to_string(opt.lengthening_step);
    else if (k == "VARIABLE_STEP")     val = std::to_string(opt.variable_step);
    else if (k == "MINIMUM_STEP")      val = std::to_string(opt.min_routing_step);
    else if (k == "MAX_TRIALS")        val = std::to_string(opt.max_trials);
    else if (k == "HEAD_TOLERANCE")    val = std::to_string(opt.head_tol);
    // LAT_FLOW_TOL / SYS_FLOW_TOL speak percent through this API on both
    // get and set, mirroring the [OPTIONS] surface; the stored fraction
    // (value / 100) is internal to the routing solver.
    else if (k == "LAT_FLOW_TOL")      val = std::to_string(opt.lat_flow_tol * 100.0);
    else if (k == "SYS_FLOW_TOL")      val = std::to_string(opt.sys_flow_tol * 100.0);
    else if (k == "MIN_SURFAREA")      val = std::to_string(opt.min_surf_area);
    else if (k == "MIN_SLOPE")         val = std::to_string(opt.min_slope);

    // Explicit finite-volume solver (FLOW_ROUTING FV). Readable and writable
    // under ANY routing model — the keys are inert rather than rejected, so a
    // GUI or script can configure FV before selecting it (matches the
    // [OPTIONS] parser contract, plan §4.2).
    else if (k == "FV_CELL_LENGTH")    val = std::to_string(opt.fv.cell_length);
    else if (k == "FV_MIN_CELLS")      val = std::to_string(opt.fv.min_cells);
    else if (k == "FV_CFL")            val = std::to_string(opt.fv.cfl);
    else if (k == "FV_RIEMANN")
        val = (opt.fv.riemann == openswmm::fv::RiemannSolver::HLL) ? "HLL" : "HLLC";
    else if (k == "FV_ORDER")          val = std::to_string(opt.fv.order);
    else if (k == "FV_LIMITER") {
        switch (opt.fv.limiter) {
            case openswmm::fv::Limiter::VANLEER:  val = "VANLEER";  break;
            case openswmm::fv::Limiter::SUPERBEE: val = "SUPERBEE"; break;
            default:                              val = "MINMOD";   break;
        }
    }
    else if (k == "FV_SCALAR_SCHEME") {
        switch (opt.fv.scalar_scheme) {
            case openswmm::fv::ScalarScheme::UPWIND: val = "UPWIND"; break;
            case openswmm::fv::ScalarScheme::QUICKEST_ULTIMATE:
                val = "QUICKEST_ULTIMATE"; break;
            default:                                 val = "MUSCL";  break;
        }
    }
    else if (k == "FV_TIME_INTEGRATION")
        val = (opt.fv.time_integration == openswmm::fv::TimeIntegration::RK2)
                  ? "RK2" : "EULER";
    else if (k == "FV_SLOT_CELERITY")  val = std::to_string(opt.fv.slot_celerity);
    else if (k == "FV_PRESSURIZED_IMPLICIT")
        val = opt.fv.pressurized_implicit ? "YES" : "NO";
    else if (k == "FV_DISPERSION")     val = std::to_string(opt.fv.dispersion);
    else if (k == "FV_STRUCTURE_COUPLING")
        val = (opt.fv.structure_coupling == openswmm::fv::StructureCoupling::ROUTING_STEP)
                  ? "ROUTING_STEP" : "SUBSTEP";
    else if (k == "FV_COMPACTION")     val = opt.fv.compaction ? "YES" : "NO";
    else if (k == "FV_NODE_COUPLING")
        val = (opt.fv.node_coupling == openswmm::fv::NodeCoupling::EXPLICIT)
                  ? "EXPLICIT" : "SEMI_IMPLICIT";
    else if (k == "FV_NODE_DT")
        val = (opt.fv.node_dt_limit == openswmm::fv::NodeDtLimit::NONE)
                  ? "NONE" : "STABILITY";
    else if (k == "FV_NODE_PICARD")
        val = std::to_string(opt.fv.node_picard_sweeps);
    else if (k == "FV_NODE_CELL_COUPLING")
        val = "NO";                       // retired option; kept readable
    else if (k == "FV_JUNCTION_MODEL")
        val = "ALGEBRAIC";                // retired option; junctions always are
    else if (k == "FV_BACKEND") {
        switch (opt.fv.backend) {
            case openswmm::fv::Backend::CPU:  val = "CPU";  break;
            case openswmm::fv::Backend::OMP:  val = "OMP";  break;
            case openswmm::fv::Backend::CUDA: val = "CUDA"; break;
            case openswmm::fv::Backend::HIP:  val = "HIP";  break;
            case openswmm::fv::Backend::SYCL: val = "SYCL"; break;
            default:                          val = "AUTO"; break;
        }
    }
    else if (k == "FV_MIN_PARALLEL_CELLS")
        val = std::to_string(opt.fv.min_parallel_cells);
    else if (k == "FV_LTS")            val = opt.fv.lts ? "YES" : "NO";
    else if (k == "FV_LTS_MAX_TIERS")  val = std::to_string(opt.fv.lts_max_tiers);
    else if (k == "FV_CFL_CENSUS_INTERVAL")
        val = std::to_string(opt.fv.cfl_census_interval);

    // Quality & transport (subplan Y0). Readable and writable under ANY
    // solver — the keys are inert rather than rejected, the same contract
    // the FV group above documents, so a GUI can configure LARD before
    // selecting it. Without these the C API returned BADPARAM and the
    // options dialog could not hydrate the page at all.
    else if (k == "QUALITY_SOLVER") {
        switch (opt.quality_solver) {
            case openswmm::QualitySolverKind::EULERIAN_ARD:
                val = "EULERIAN_ARD"; break;
            case openswmm::QualitySolverKind::LAGRANGIAN:
                val = "LAGRANGIAN";   break;
            default:
                val = "LEGACY";       break;
        }
    }
    else if (k == "WATER_AGE")         val = opt.water_age ? "YES" : "NO";
    else if (k == "OUTFALL_BACKFLOW_QUALITY")
        val = opt.outfall_backflow_zero ? "ZERO" : "LAST";
    else if (k == "HEAT_TRANSPORT")    val = opt.heat_transport ? "YES" : "NO";
    else if (k == "QUALITY_STEP")      val = std::to_string(opt.quality_step);
    else if (k == "MAX_SEGMENTS_PER_LINK")
        val = std::to_string(opt.max_segments_per_link);
    else if (k == "DISPERSION")        val = opt.lard_rwpt ? "RWPT" : "OFF";
    else if (k == "RWPT_SEED")         val = std::to_string(opt.rwpt_seed);

    // System / Performance
    else if (k == "THREADS")           val = std::to_string(opt.num_threads);

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

    // ── Exception guard at the C boundary ────────────────────────────────
    // Thirty of this function's key branches parse with raw std::stod /
    // std::stoi, which THROW on a malformed value. An exception crossing
    // `extern "C"` terminates the process: measured, `swmm_options_set(e,
    // "FV_CFL", "abc")` aborted, while `ROUTING_STEP = "xyz"` survived
    // because that one branch already had a local try/catch. A caller that
    // hands us a bad string — the MCP server's set_option tool takes
    // arbitrary text, and a GUI line edit is transiently empty while being
    // typed — must get SWMM_ERR_BADPARAM, never a dead process.
    //
    // One guard around the whole dispatch rather than 30 local ones: a
    // per-site fix can miss a site (and a future branch would be born
    // unguarded), whereas this cannot. Nothing in the dispatch throws for
    // any reason OTHER than a bad parse — the rest is assignment — so the
    // broad catch costs no diagnostic precision, and "no exception escapes
    // a C entry point" is the correct contract regardless.
    try {

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
        else if (vu == "FV")       opt.routing_model = openswmm::RoutingModel::FV;
        else if (vu == "KINWAVE")  opt.routing_model = openswmm::RoutingModel::KINWAVE;
        else if (vu == "STEADY")   opt.routing_model = openswmm::RoutingModel::STEADY;
        else return SWMM_ERR_BADPARAM;
    }
    else if (k == "LINK_OFFSETS") {
        std::string vu(v);
        for (auto& c : vu) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        if      (vu == "DEPTH" || vu == "DEPTH_OFFSET" || vu == "0") opt.link_offsets = 0;
        else if (vu == "ELEVATION" || vu == "ELEV_OFFSET" || vu == "1") opt.link_offsets = 1;
        else return SWMM_ERR_BADPARAM;
    }
    // parse_time_seconds (not std::stod) so the HH:MM:SS clock form is honored
    // — std::stod("0:00:05") stops at the first ':' and yields 0, silently
    // zeroing the step.  Matches WET_STEP/DRY_STEP below and the OptionsHandler
    // parser, both of which use parse_time_seconds.
    else if (k == "ROUTING_STEP") {
        opt.routing_step = time_seconds_strict(v);
    }
    else if (k == "REPORT_STEP") {
        opt.report_step = time_seconds_strict(v);
    }
    // Date/time keys are stored combined in a single OADate double. Get/set
    // for *_DATE addresses the integer (date) portion only and *_TIME the
    // fractional (time-of-day) portion, so the GUI can write them
    // independently — matching the OptionsHandler parser composition rule.
    // The previous stod_strict(v) implementation for START_DATE/END_DATE was
    // a pre-existing bug — std::stod("01/01/2004") writes 1.0 — fixed here
    // as part of Slice CY (closes the §M.4 deferred get/set parity gap).
    else if (k == "START_DATE") {
        opt.start_date = openswmm::input::parse_date(v)
                       + (opt.start_date - std::floor(opt.start_date));
        opt.total_duration_ms = -1.0;  // stale — recompute via totalDurationMs()
    }
    else if (k == "START_TIME") {
        opt.start_date = std::floor(opt.start_date)
                       + time_seconds_strict(v)
                         / openswmm::datetime::SecsPerDay;
        opt.total_duration_ms = -1.0;  // stale — recompute via totalDurationMs()
    }
    else if (k == "END_DATE") {
        opt.end_date = openswmm::input::parse_date(v)
                     + (opt.end_date - std::floor(opt.end_date));
        opt.total_duration_ms = -1.0;  // stale — recompute via totalDurationMs()
    }
    else if (k == "END_TIME") {
        opt.end_date = std::floor(opt.end_date)
                     + time_seconds_strict(v)
                       / openswmm::datetime::SecsPerDay;
        opt.total_duration_ms = -1.0;  // stale — recompute via totalDurationMs()
    }
    else if (k == "REPORT_START_DATE") {
        opt.report_start = openswmm::input::parse_date(v)
                         + (opt.report_start - std::floor(opt.report_start));
    }
    else if (k == "REPORT_START_TIME") {
        opt.report_start = std::floor(opt.report_start)
                         + time_seconds_strict(v)
                           / openswmm::datetime::SecsPerDay;
    }
    else if (k == "CRS") {
        opt.crs = v;
    }

    // [REPORT] section keys (Slice BV.1 — added 2026-05-22). Boolean keys
    // accept YES/NO/TRUE/FALSE/1/0; selector keys accept ALL, NONE, or a
    // comma- or whitespace-delimited list of names (mode set to 2 = SOME
    // and names pushed onto the matching rpt_*_names vector).
    else if (k.rfind("RPT_", 0) == 0) {
        auto parse_bool = [&v]() -> int {
            std::string vu(v);
            for (auto& c : vu) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
            if (vu == "YES" || vu == "TRUE"  || vu == "1") return 1;
            if (vu == "NO"  || vu == "FALSE" || vu == "0") return 0;
            return -1;
        };
        auto parse_selector = [&v](int& mode, std::vector<std::string>& names) -> int {
            std::string vu(v);
            for (auto& c : vu) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
            // Trim outer whitespace
            auto ltrim = vu.find_first_not_of(" \t");
            auto rtrim = vu.find_last_not_of(" \t");
            const std::string trimmed = (ltrim == std::string::npos)
                ? std::string()
                : vu.substr(ltrim, rtrim - ltrim + 1);
            if (trimmed == "NONE" || trimmed.empty()) { mode = 0; names.clear(); return 0; }
            if (trimmed == "ALL")                     { mode = 1; names.clear(); return 0; }
            // SOME mode — tokenize the original (case-preserving) value on
            // commas or whitespace. Skip empty tokens.
            names.clear();
            std::string token;
            for (char c : v) {
                if (c == ',' || c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                    if (!token.empty()) { names.push_back(token); token.clear(); }
                } else {
                    token.push_back(c);
                }
            }
            if (!token.empty()) names.push_back(token);
            mode = 2;
            return 0;
        };

        if      (k == "RPT_DISABLED")   { int b = parse_bool(); if (b < 0) return SWMM_ERR_BADPARAM; opt.rpt_disabled   = (b == 1); }
        else if (k == "RPT_INPUT")      { int b = parse_bool(); if (b < 0) return SWMM_ERR_BADPARAM; opt.rpt_input      = (b == 1); }
        else if (k == "RPT_CONTINUITY") { int b = parse_bool(); if (b < 0) return SWMM_ERR_BADPARAM; opt.rpt_continuity = (b == 1); }
        else if (k == "RPT_FLOWSTATS")  { int b = parse_bool(); if (b < 0) return SWMM_ERR_BADPARAM; opt.rpt_flowstats  = (b == 1); }
        else if (k == "RPT_CONTROLS")   { int b = parse_bool(); if (b < 0) return SWMM_ERR_BADPARAM; opt.rpt_controls   = (b == 1); }
        else if (k == "RPT_AVERAGES")   { int b = parse_bool(); if (b < 0) return SWMM_ERR_BADPARAM; opt.rpt_averages   = (b == 1); }
        else if (k == "RPT_SUBCATCHMENTS") parse_selector(opt.rpt_subcatchments, opt.rpt_subcatch_names);
        else if (k == "RPT_NODES")         parse_selector(opt.rpt_nodes,         opt.rpt_node_names);
        else if (k == "RPT_LINKS")         parse_selector(opt.rpt_links,         opt.rpt_link_names);
        else return SWMM_ERR_BADPARAM;
    }

    // ------------------------------------------------------------------
    // Slice CY (2026-05-22) — closure of §M.4 deferred OPTIONS keys.
    // Symmetric with the swmm_options_get branches above. Aliases mirror
    // the OptionsHandler parser so legacy .inp values and GUI tokens
    // both round-trip.
    // ------------------------------------------------------------------

    // Step keys — accept seconds or HH:MM:SS per parse_time_seconds.
    else if (k == "DRY_STEP")  opt.dry_step  = time_seconds_strict(v);
    else if (k == "WET_STEP")  opt.wet_step  = time_seconds_strict(v);
    else if (k == "RULE_STEP") opt.rule_step = time_seconds_strict(v);
    else if (k == "DRY_DAYS")  opt.dry_days  = stod_strict(v);

    // Sweep — accept MM/DD (legacy form, what the GUI writes), convert to
    // day-of-year via year-2000 anchor matching the parser convention.
    else if (k == "SWEEP_START" || k == "SWEEP_END") {
        unsigned sm = 0, sd = 0;
        const char* sp = v.data();
        const char* se = sp + v.size();
        std::from_chars(sp, se, sm);
        while (sp < se && *sp != '/') ++sp;
        if (sp < se) ++sp;
        std::from_chars(sp, se, sd);
        if (sm < 1 || sm > 12 || sd < 1 || sd > 31) return SWMM_ERR_BADPARAM;
        const int doy = openswmm::datetime::dayOfYear(
            openswmm::datetime::encodeDate(2000,
                                           static_cast<int>(sm),
                                           static_cast<int>(sd)));
        if (k == "SWEEP_START") opt.sweep_start = doy;
        else                    opt.sweep_end   = doy;
    }

    // Infiltration — token (aliases mirror OptionsHandler).
    else if (k == "INFILTRATION") {
        std::string vu(v);
        for (auto& c : vu) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        if      (vu == "HORTON")
            opt.infiltration = openswmm::InfiltrationModel::HORTON;
        else if (vu == "MOD_HORTON" || vu == "MODIFIED_HORTON")
            opt.infiltration = openswmm::InfiltrationModel::MOD_HORTON;
        else if (vu == "GREEN_AMPT")
            opt.infiltration = openswmm::InfiltrationModel::GREEN_AMPT;
        else if (vu == "MOD_GREEN_AMPT" || vu == "MODIFIED_GREEN_AMPT")
            opt.infiltration = openswmm::InfiltrationModel::MOD_GREEN_AMPT;
        else if (vu == "CURVE_NUMBER")
            opt.infiltration = openswmm::InfiltrationModel::CURVE_NUMBER;
        else return SWMM_ERR_BADPARAM;
    }

    // Boolean flags — common parser inline (cannot reach the RPT_ block's
    // parse_bool lambda from here).
    else if (k == "ALLOW_PONDING"
          || k == "SKIP_STEADY_STATE"
          || k == "IGNORE_RAINFALL"
          || k == "IGNORE_SNOWMELT"
          || k == "IGNORE_GROUNDWATER"
          || k == "IGNORE_RDII"
          || k == "IGNORE_QUALITY"
          || k == "IGNORE_ROUTING"
          || k == "IGNORE_2D"
          || k == "ANDERSON_ACCEL") {
        std::string vu(v);
        for (auto& c : vu) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        bool b;
        if      (vu == "YES" || vu == "TRUE"  || vu == "1") b = true;
        else if (vu == "NO"  || vu == "FALSE" || vu == "0") b = false;
        else return SWMM_ERR_BADPARAM;
        if      (k == "ALLOW_PONDING")      opt.allow_ponding      = b;
        else if (k == "SKIP_STEADY_STATE")  opt.skip_steady_state  = b;
        else if (k == "IGNORE_RAINFALL")    opt.ignore_rainfall    = b;
        else if (k == "IGNORE_SNOWMELT")    opt.ignore_snow_melt   = b;
        else if (k == "IGNORE_GROUNDWATER") opt.ignore_groundwater = b;
        else if (k == "IGNORE_RDII")        opt.ignore_rdii        = b;
        else if (k == "IGNORE_QUALITY")     opt.ignore_quality     = b;
        else if (k == "IGNORE_ROUTING")     opt.ignore_routing     = b;
        else if (k == "IGNORE_2D")          opt.ignore_2d          = b;
        else if (k == "ANDERSON_ACCEL")     opt.anderson_accel     = b;
    }

    // Routing & Hydraulics — enums.
    else if (k == "SURCHARGE_METHOD") {
        std::string vu(v);
        for (auto& c : vu) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        if      (vu == "EXTRAN")       opt.surcharge_method = 0;
        else if (vu == "SLOT")         opt.surcharge_method = 1;
        else if (vu == "DYNAMIC_SLOT") opt.surcharge_method = 2;
        else return SWMM_ERR_BADPARAM;
    }
    else if (k == "NODE_CONTINUITY") {
        std::string vu(v);
        for (auto& c : vu) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        if      (vu == "EXPLICIT")      opt.node_continuity = openswmm::NodeContinuity::EXPLICIT;
        else if (vu == "SEMI_IMPLICIT") opt.node_continuity = openswmm::NodeContinuity::SEMI_IMPLICIT;
        else return SWMM_ERR_BADPARAM;
    }
    else if (k == "FORCE_MAIN_EQUATION") {
        std::string vu(v);
        for (auto& c : vu) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        if      (vu == "H-W" || vu == "HW" || vu == "HAZEN-WILLIAMS") opt.force_main_eqn = 0;
        else if (vu == "D-W" || vu == "DW" || vu == "DARCY-WEISBACH") opt.force_main_eqn = 1;
        else return SWMM_ERR_BADPARAM;
    }
    else if (k == "NORMAL_FLOW_LIMITED") {
        std::string vu(v);
        for (auto& c : vu) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        if      (vu == "SLOPE")   opt.normal_flow_ltd = 0;
        else if (vu == "FROUDE")  opt.normal_flow_ltd = 1;
        else if (vu == "BOTH")    opt.normal_flow_ltd = 2;
        else if (vu == "NEITHER") opt.normal_flow_ltd = 3;
        else return SWMM_ERR_BADPARAM;
    }
    else if (k == "INERTIAL_DAMPING") {
        std::string vu(v);
        for (auto& c : vu) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        if      (vu == "NONE")    opt.inertial_damping = 0;
        else if (vu == "PARTIAL") opt.inertial_damping = 1;
        else if (vu == "FULL")    opt.inertial_damping = 2;
        else return SWMM_ERR_BADPARAM;
    }

    // Numeric scalars.
    else if (k == "DPS_CELERITY")      opt.dps_target_celerity = stod_strict(v);
    else if (k == "DPS_ALPHA")         opt.dps_alpha           = stod_strict(v);
    else if (k == "DPS_DECAY_TIME")    opt.dps_decay_time      = stod_strict(v);
    else if (k == "LENGTHENING_STEP")  opt.lengthening_step    = stod_strict(v);
    else if (k == "VARIABLE_STEP")     opt.variable_step       = stod_strict(v);
    // MINIMUM_STEP takes seconds or HH:MM:SS, same grammar as the [OPTIONS]
    // parser (OptionsHandler) and ROUTING_STEP above.
    else if (k == "MINIMUM_STEP")
        opt.min_routing_step = time_seconds_strict(v);
    else if (k == "MAX_TRIALS")        opt.max_trials          = stoi_strict(v);
    else if (k == "HEAD_TOLERANCE")    opt.head_tol            = stod_strict(v);
    // Flow tolerances are percentages per the INP/[OPTIONS] contract; the
    // OptionsHandler parser and the routing solver store them as fractions
    // (value / 100), so convert here to match — a raw std::stod stored 500%
    // for a "5" input and skewed dynamic-wave convergence.
    else if (k == "LAT_FLOW_TOL")      opt.lat_flow_tol        = stod_strict(v) / 100.0;
    else if (k == "SYS_FLOW_TOL")      opt.sys_flow_tol        = stod_strict(v) / 100.0;
    else if (k == "MIN_SURFAREA")      opt.min_surf_area       = stod_strict(v);
    else if (k == "MIN_SLOPE")         opt.min_slope           = stod_strict(v);
    else if (k == "THREADS")           opt.num_threads         = stoi_strict(v);

    // Explicit finite-volume solver. Same value grammar as the [OPTIONS]
    // parser, so a value round-trips between the file and this API unchanged.
    else if (k == "FV_CELL_LENGTH")    opt.fv.cell_length      = stod_strict(v);
    else if (k == "FV_MIN_CELLS")      opt.fv.min_cells        = std::max(1, stoi_strict(v));
    else if (k == "FV_CFL")            opt.fv.cfl              = stod_strict(v);
    else if (k == "FV_ORDER")          opt.fv.order            = stoi_strict(v);
    else if (k == "FV_SLOT_CELERITY")  opt.fv.slot_celerity    = stod_strict(v);
    else if (k == "FV_PRESSURIZED_IMPLICIT") {
        const std::string vu = upper_copy(v);
        opt.fv.pressurized_implicit =
            (vu == "YES" || vu == "TRUE" || vu == "ON" || vu == "1");
    }
    else if (k == "FV_DISPERSION")     opt.fv.dispersion       = stod_strict(v);
    else if (k == "FV_MIN_PARALLEL_CELLS")
        opt.fv.min_parallel_cells = stol_strict(v);
    else if (k == "FV_COMPACTION") {
        const std::string vu = upper_copy(v);
        opt.fv.compaction = !(vu == "NO" || vu == "FALSE" || vu == "0" || vu == "OFF");
    }
    else if (k == "FV_NODE_COUPLING") {
        const std::string vu = upper_copy(v);
        if      (vu == "EXPLICIT")
            opt.fv.node_coupling = openswmm::fv::NodeCoupling::EXPLICIT;
        else if (vu == "SEMI_IMPLICIT")
            opt.fv.node_coupling = openswmm::fv::NodeCoupling::SEMI_IMPLICIT;
        else return SWMM_ERR_BADPARAM;
    }
    else if (k == "FV_NODE_DT") {
        const std::string vu = upper_copy(v);
        if      (vu == "STABILITY") opt.fv.node_dt_limit = openswmm::fv::NodeDtLimit::STABILITY;
        else if (vu == "NONE")      opt.fv.node_dt_limit = openswmm::fv::NodeDtLimit::NONE;
        else return SWMM_ERR_BADPARAM;
    }
    else if (k == "FV_NODE_PICARD")
        opt.fv.node_picard_sweeps = std::max(1, stoi_strict(v));
    else if (k == "FV_NODE_CELL_COUPLING" || k == "FV_JUNCTION_MODEL") {
        // Retired options, accepted and ignored: junctions are always
        // algebraic interfaces now.
    }
    else if (k == "FV_LTS") {
        const std::string vu = upper_copy(v);
        opt.fv.lts = !(vu == "NO" || vu == "FALSE" || vu == "0" || vu == "OFF");
    }
    else if (k == "FV_LTS_MAX_TIERS")
        opt.fv.lts_max_tiers = std::max(1, stoi_strict(v));
    else if (k == "FV_CFL_CENSUS_INTERVAL")
        opt.fv.cfl_census_interval = std::max(1, stoi_strict(v));
    else if (k == "FV_RIEMANN") {
        const std::string vu = upper_copy(v);
        if      (vu == "HLL")  opt.fv.riemann = openswmm::fv::RiemannSolver::HLL;
        else if (vu == "HLLC") opt.fv.riemann = openswmm::fv::RiemannSolver::HLLC;
        else return SWMM_ERR_BADPARAM;
    }
    else if (k == "FV_LIMITER") {
        const std::string vu = upper_copy(v);
        if      (vu == "MINMOD")   opt.fv.limiter = openswmm::fv::Limiter::MINMOD;
        else if (vu == "VANLEER")  opt.fv.limiter = openswmm::fv::Limiter::VANLEER;
        else if (vu == "SUPERBEE") opt.fv.limiter = openswmm::fv::Limiter::SUPERBEE;
        else return SWMM_ERR_BADPARAM;
    }
    else if (k == "FV_SCALAR_SCHEME") {
        const std::string vu = upper_copy(v);
        if      (vu == "UPWIND") opt.fv.scalar_scheme = openswmm::fv::ScalarScheme::UPWIND;
        else if (vu == "MUSCL")  opt.fv.scalar_scheme = openswmm::fv::ScalarScheme::MUSCL;
        else if (vu == "QUICKEST_ULTIMATE" || vu == "QUICKEST")
            opt.fv.scalar_scheme = openswmm::fv::ScalarScheme::QUICKEST_ULTIMATE;
        else return SWMM_ERR_BADPARAM;
    }
    else if (k == "FV_TIME_INTEGRATION") {
        const std::string vu = upper_copy(v);
        if      (vu == "EULER") opt.fv.time_integration = openswmm::fv::TimeIntegration::EULER;
        else if (vu == "RK2")   opt.fv.time_integration = openswmm::fv::TimeIntegration::RK2;
        else return SWMM_ERR_BADPARAM;
    }
    else if (k == "FV_STRUCTURE_COUPLING") {
        const std::string vu = upper_copy(v);
        if      (vu == "SUBSTEP")
            opt.fv.structure_coupling = openswmm::fv::StructureCoupling::SUBSTEP;
        else if (vu == "ROUTING_STEP")
            opt.fv.structure_coupling = openswmm::fv::StructureCoupling::ROUTING_STEP;
        else return SWMM_ERR_BADPARAM;
    }
    else if (k == "FV_BACKEND") {
        const std::string vu = upper_copy(v);
        if      (vu == "CPU")  opt.fv.backend = openswmm::fv::Backend::CPU;
        else if (vu == "AUTO") opt.fv.backend = openswmm::fv::Backend::AUTO;
        else if (vu == "OMP")  opt.fv.backend = openswmm::fv::Backend::OMP;
        else if (vu == "CUDA") opt.fv.backend = openswmm::fv::Backend::CUDA;
        else if (vu == "HIP")  opt.fv.backend = openswmm::fv::Backend::HIP;
        else if (vu == "SYCL") opt.fv.backend = openswmm::fv::Backend::SYCL;
        else return SWMM_ERR_BADPARAM;
    }

    // Quality & transport (subplan Y0) — the setter half of the getter
    // block above. Enum keys REJECT unknown tokens (the FV precedent the
    // hydration contract's rejectBadEnumTokens case relies on) so a typo
    // surfaces as a failed set instead of a silently lost edit.
    else if (k == "QUALITY_SOLVER") {
        const std::string vu = upper_copy(v);
        if      (vu == "LEGACY")
            opt.quality_solver = openswmm::QualitySolverKind::LEGACY;
        else if (vu == "EULERIAN_ARD" || vu == "ARD")
            opt.quality_solver = openswmm::QualitySolverKind::EULERIAN_ARD;
        else if (vu == "LAGRANGIAN" || vu == "LARD")
            opt.quality_solver = openswmm::QualitySolverKind::LAGRANGIAN;
        else return SWMM_ERR_BADPARAM;
    }
    else if (k == "WATER_AGE") {
        const std::string vu = upper_copy(v);
        opt.water_age = (vu == "YES" || vu == "ON" || vu == "TRUE" ||
                         vu == "1");
    }
    else if (k == "OUTFALL_BACKFLOW_QUALITY") {
        const std::string vu = upper_copy(v);
        if      (vu == "LAST") opt.outfall_backflow_zero = false;
        else if (vu == "ZERO") opt.outfall_backflow_zero = true;
        else return SWMM_ERR_BADPARAM;
    }
    else if (k == "HEAT_TRANSPORT") {
        const std::string vu = upper_copy(v);
        opt.heat_transport = (vu == "YES" || vu == "ON" || vu == "TRUE" ||
                              vu == "1");
    }
    else if (k == "QUALITY_STEP")
        opt.quality_step = std::max(0.0, stod_strict(v));
    else if (k == "MAX_SEGMENTS_PER_LINK")
        opt.max_segments_per_link = std::max(2, stoi_strict(v));
    else if (k == "DISPERSION") {
        const std::string vu = upper_copy(v);
        if      (vu == "RWPT")                 opt.lard_rwpt = true;
        else if (vu == "OFF" || vu == "NONE")  opt.lard_rwpt = false;
        else return SWMM_ERR_BADPARAM;
    }
    else if (k == "RWPT_SEED")
        opt.rwpt_seed = stoi_strict(v);

    else {
        return SWMM_ERR_BADPARAM;
    }

    } catch (...) {
        // A malformed numeric for a recognised key. The key was valid, the
        // value was not — BADPARAM is the same answer an out-of-range enum
        // token gets, and the option keeps its previous value because the
        // throwing branch assigned nothing.
        return SWMM_ERR_BADPARAM;
    }

    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_options_get_ext(SWMM_Engine engine,
                                          const char* key, char* buf, int buflen) {
    CHECK_HANDLE(engine);
    if (!key || !buf || buflen <= 0) return SWMM_ERR_BADPARAM;

    auto& ctx = to_engine(engine)->context();

#ifdef OPENSWMM_HAS_2D
    // [2D_MESH_FILE] reference — mirror the set_ext write side.
    if (ctx.twod_io.options && upper_key(key) == "MESH_FILE") {
        const std::string& v = ctx.twod_io.options->mesh_file;
        std::strncpy(buf, v.c_str(), static_cast<std::size_t>(buflen - 1));
        buf[buflen - 1] = '\0';
        return SWMM_OK;
    }

    // [2D_OPTIONS] keys read the live SolverOptions2D (the solver's source
    // of truth, wired through ctx.twod_io) instead of the generic
    // ext_options map — see swmm_options_set_ext for the write side.
    if (ctx.twod_io.options && openswmm::twoD::is2DOptionKey(key)) {
        const std::string v =
            openswmm::twoD::format2DOptionValue(*ctx.twod_io.options, key);
        std::strncpy(buf, v.c_str(), static_cast<std::size_t>(buflen - 1));
        buf[buflen - 1] = '\0';
        return SWMM_OK;
    }
#endif

    const auto& ext = ctx.options.ext_options;
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

    auto& ctx = to_engine(engine)->context();

    // WRITE_ABSOLUTE_PATHS is a first-class SimulationOptions field, not an
    // extension key. Letting it fall through to ext_options produced a
    // round-trip asymmetry: the save itself still wrote RELATIVE paths (the
    // writer reads the bool, which stayed false), but the emitted deck carried
    // WRITE_ABSOLUTE_PATHS YES and so re-opened with the opt-out armed.
    if (upper_key(key) == "WRITE_ABSOLUTE_PATHS") {
        ctx.options.write_absolute_paths =
            openswmm::input::Tokenizer::parse_boolean(value);
        ctx.options.ext_options.erase(key);
        return SWMM_OK;
    }

#ifdef OPENSWMM_HAS_2D
    // [2D_MESH_FILE] reference: route to the live SolverOptions2D::mesh_file
    // so the GUI/API can attach (or detach) an external .2dm and have the
    // InpWriter emit the [2D_MESH_FILE] section on the next save — without
    // this the reference is dropped whenever the engine re-serialises the
    // .inp (the model becomes 1D-only). An empty value clears the reference
    // (engine reverts to the inline mesh, if any). Handled before the
    // is2DOptionKey routing so it never touches the [2D_OPTIONS] grammar.
    if (ctx.twod_io.options && upper_key(key) == "MESH_FILE") {
        ctx.twod_io.options->mesh_file = value;
        ctx.options.ext_options.erase(key);
        return SWMM_OK;
    }

    // Keys retired with the CVODE/ARKODE stack (D2) hard-fail here — letting
    // them fall through to the generic ext_options map would silently persist
    // them to the next save. (File load, by contrast, warns and ignores them
    // so legacy models still open.)
    if (openswmm::twoD::is2DRetiredOptionKey(key)) return SWMM_ERR_BADPARAM;

    // Route [2D_OPTIONS] keys into the live SolverOptions2D so GUI/API
    // edits actually reach the 2D solver and persist (InpWriter emits them
    // in [2D_OPTIONS]; the GeoPackage writer as 2D_* option keys).
    // Previously these landed in ext_options, where the solver never
    // looked — also erase any such stale copy so old [OPTIONS] pollution
    // self-heals on the next save.
    if (ctx.twod_io.options && openswmm::twoD::is2DOptionKey(key)) {
        // Parse into a copy and commit only on success: parse2DOptionsLine
        // assigns the field before validating, so feeding it the live
        // options would clobber the current value on a rejected set.
        openswmm::twoD::SolverOptions2D tmp = *ctx.twod_io.options;
        const std::string err =
            openswmm::twoD::parse2DOptionsLine({key, value}, tmp);
        if (!err.empty()) return SWMM_ERR_BADPARAM;
        *ctx.twod_io.options = std::move(tmp);
        ctx.options.ext_options.erase(key);
        return SWMM_OK;
    }
#endif

    ctx.options.ext_options[key] = value;
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
// Typed time-control accessors (OADate doubles)
// ============================================================================

SWMM_ENGINE_API int swmm_options_get_start_date(SWMM_Engine engine, double* value) {
    CHECK_HANDLE(engine);
    if (!value) return SWMM_ERR_BADPARAM;
    *value = to_engine(engine)->context().options.start_date;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_options_set_start_date(SWMM_Engine engine, double value) {
    CHECK_HANDLE(engine);
    auto& opt_sd = to_engine(engine)->context().options;
    opt_sd.start_date = value;
    opt_sd.total_duration_ms = -1.0;  // stale — recompute via totalDurationMs()
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_options_get_end_date(SWMM_Engine engine, double* value) {
    CHECK_HANDLE(engine);
    if (!value) return SWMM_ERR_BADPARAM;
    *value = to_engine(engine)->context().options.end_date;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_options_set_end_date(SWMM_Engine engine, double value) {
    CHECK_HANDLE(engine);
    auto& opt_ed = to_engine(engine)->context().options;
    opt_ed.end_date = value;
    opt_ed.total_duration_ms = -1.0;  // stale — recompute via totalDurationMs()
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_options_get_report_start(SWMM_Engine engine, double* value) {
    CHECK_HANDLE(engine);
    if (!value) return SWMM_ERR_BADPARAM;
    *value = to_engine(engine)->context().options.report_start;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_options_set_report_start(SWMM_Engine engine, double value) {
    CHECK_HANDLE(engine);
    to_engine(engine)->context().options.report_start = value;
    return SWMM_OK;
}

// ============================================================================
// User flags
// ============================================================================

SWMM_ENGINE_API int swmm_userflag_get_bool(SWMM_Engine engine,
                                             const char* name, int* value) {
    CHECK_HANDLE(engine);
    if (!name || !value) return SWMM_ERR_BADPARAM;
    const std::string n = upper_key(name);
    const auto& flags = to_engine(engine)->context().user_flags;
    if (!flags.is_defined(n)) return SWMM_ERR_BADPARAM;
    const auto& def = flags.get_def(n);
    if (def.type != openswmm::UserFlagType::BOOLEAN) return SWMM_ERR_BADPARAM;
    auto opt = flags.try_get_value("MODEL", "", n);
    if (!opt.has_value()) { *value = 0; return SWMM_OK; }
    *value = std::get<bool>(opt.value()) ? 1 : 0;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_userflag_get_int(SWMM_Engine engine,
                                            const char* name, int* value) {
    CHECK_HANDLE(engine);
    if (!name || !value) return SWMM_ERR_BADPARAM;
    const std::string n = upper_key(name);
    const auto& flags = to_engine(engine)->context().user_flags;
    if (!flags.is_defined(n)) return SWMM_ERR_BADPARAM;
    const auto& def = flags.get_def(n);
    if (def.type != openswmm::UserFlagType::INTEGER) return SWMM_ERR_BADPARAM;
    auto opt = flags.try_get_value("MODEL", "", n);
    if (!opt.has_value()) { *value = 0; return SWMM_OK; }
    *value = std::get<int>(opt.value());
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_userflag_get_real(SWMM_Engine engine,
                                             const char* name, double* value) {
    CHECK_HANDLE(engine);
    if (!name || !value) return SWMM_ERR_BADPARAM;
    const std::string n = upper_key(name);
    const auto& flags = to_engine(engine)->context().user_flags;
    if (!flags.is_defined(n)) return SWMM_ERR_BADPARAM;
    const auto& def = flags.get_def(n);
    if (def.type != openswmm::UserFlagType::REAL) return SWMM_ERR_BADPARAM;
    auto opt = flags.try_get_value("MODEL", "", n);
    if (!opt.has_value()) { *value = 0.0; return SWMM_OK; }
    *value = std::get<double>(opt.value());
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_userflag_set_bool(SWMM_Engine engine,
                                             const char* name, int value) {
    CHECK_HANDLE(engine);
    if (!name) return SWMM_ERR_BADPARAM;
    const std::string n = upper_key(name);
    auto& flags = to_engine(engine)->context().user_flags;
    // Register the schema so the value is readable: the getters gate on
    // is_defined(name) and the def's type. define() overwrites idempotently.
    flags.define({n, openswmm::UserFlagType::BOOLEAN, ""});
    flags.set("MODEL", "", n, value != 0);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_userflag_set_int(SWMM_Engine engine,
                                            const char* name, int value) {
    CHECK_HANDLE(engine);
    if (!name) return SWMM_ERR_BADPARAM;
    const std::string n = upper_key(name);
    auto& flags = to_engine(engine)->context().user_flags;
    flags.define({n, openswmm::UserFlagType::INTEGER, ""});
    flags.set("MODEL", "", n, value);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_userflag_set_real(SWMM_Engine engine,
                                             const char* name, double value) {
    CHECK_HANDLE(engine);
    if (!name) return SWMM_ERR_BADPARAM;
    const std::string n = upper_key(name);
    auto& flags = to_engine(engine)->context().user_flags;
    flags.define({n, openswmm::UserFlagType::REAL, ""});
    flags.set("MODEL", "", n, value);
    return SWMM_OK;
}

// ----------------------------------------------------------------------------
// User flag schema definitions + per-object values (GUI surface).
// Object types and flag names are stored uppercase (mirrors the INP handlers);
// object names are case-preserved.
// ----------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_userflag_def_count(SWMM_Engine engine, int* count) {
    CHECK_HANDLE(engine);
    if (!count) return SWMM_ERR_BADPARAM;
    *count = static_cast<int>(
        to_engine(engine)->context().user_flags.def_count());
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_userflag_def_get(SWMM_Engine engine, int index,
                                          char* name_buf, int name_buflen,
                                          int* type,
                                          char* desc_buf, int desc_buflen) {
    CHECK_HANDLE(engine);
    const auto& defs = to_engine(engine)->context().user_flags.all_defs();
    if (index < 0 || index >= static_cast<int>(defs.size()))
        return SWMM_ERR_BADINDEX;
    const auto& d = defs[static_cast<std::size_t>(index)];
    if (name_buf) fill_buf(name_buf, name_buflen, d.name);
    if (type)     *type = static_cast<int>(d.type);
    if (desc_buf) fill_buf(desc_buf, desc_buflen, d.description);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_userflag_define(SWMM_Engine engine, const char* name,
                                         int type, const char* description) {
    CHECK_HANDLE(engine);
    if (!name || name[0] == '\0') return SWMM_ERR_BADPARAM;
    if (type < 0 || type > 3) return SWMM_ERR_BADPARAM;
    auto& flags = to_engine(engine)->context().user_flags;
    flags.define({upper_key(name),
                  static_cast<openswmm::UserFlagType>(type),
                  description ? std::string(description) : std::string()});
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_userflag_undefine(SWMM_Engine engine, const char* name) {
    CHECK_HANDLE(engine);
    if (!name) return SWMM_ERR_BADPARAM;
    auto& flags = to_engine(engine)->context().user_flags;
    return flags.undefine(upper_key(name)) ? SWMM_OK : SWMM_ERR_BADPARAM;
}

SWMM_ENGINE_API int swmm_userflag_value_get(SWMM_Engine engine,
                                            const char* obj_type,
                                            const char* obj_name,
                                            const char* flag_name,
                                            char* buf, int buflen, int* found) {
    CHECK_HANDLE(engine);
    if (!obj_type || !obj_name || !flag_name || !buf || buflen <= 0 || !found)
        return SWMM_ERR_BADPARAM;
    const auto& flags = to_engine(engine)->context().user_flags;
    const auto opt = flags.try_get_value(upper_key(obj_type), obj_name,
                                         upper_key(flag_name));
    if (!opt.has_value()) {
        *found = 0;
        buf[0] = '\0';
        return SWMM_OK;
    }
    *found = 1;
    // String form is symmetric with the INP encoding (InpWriter): YES/NO,
    // %d, %g, string verbatim (no quoting at the API boundary).
    std::string s;
    const auto& v = opt.value();
    if (std::holds_alternative<bool>(v)) {
        s = std::get<bool>(v) ? "YES" : "NO";
    } else if (std::holds_alternative<int>(v)) {
        s = std::to_string(std::get<int>(v));
    } else if (std::holds_alternative<double>(v)) {
        char tmp[32];
        std::snprintf(tmp, sizeof(tmp), "%g", std::get<double>(v));
        s = tmp;
    } else {
        s = std::get<std::string>(v);
    }
    fill_buf(buf, buflen, s);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_userflag_value_set(SWMM_Engine engine,
                                            const char* obj_type,
                                            const char* obj_name,
                                            const char* flag_name,
                                            const char* value) {
    CHECK_HANDLE(engine);
    if (!obj_type || !obj_name || !flag_name || !value)
        return SWMM_ERR_BADPARAM;
    auto& flags = to_engine(engine)->context().user_flags;
    const std::string fname = upper_key(flag_name);
    if (!flags.is_defined(fname)) return SWMM_ERR_BADPARAM;

    const auto type = flags.get_def(fname).type;
    const std::string raw(value);
    openswmm::UserFlagValue v;
    switch (type) {
        case openswmm::UserFlagType::BOOLEAN:
            v = openswmm::input::Tokenizer::parse_boolean(raw);
            break;
        case openswmm::UserFlagType::INTEGER: {
            int iv = 0;
            const auto res =
                std::from_chars(raw.data(), raw.data() + raw.size(), iv);
            if (res.ec != std::errc{} || res.ptr != raw.data() + raw.size())
                return SWMM_ERR_BADPARAM;
            v = iv;
            break;
        }
        case openswmm::UserFlagType::REAL: {
            double dv = 0.0;
            const auto res = openswmm::from_chars_double(
                raw.data(), raw.data() + raw.size(), dv);
            if (res.ec != std::errc{}) return SWMM_ERR_BADPARAM;
            v = dv;
            break;
        }
        case openswmm::UserFlagType::STRING:
        default:
            v = raw;
            break;
    }
    flags.set(upper_key(obj_type), obj_name, fname, std::move(v));
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_userflag_value_clear(SWMM_Engine engine,
                                              const char* obj_type,
                                              const char* obj_name,
                                              const char* flag_name) {
    CHECK_HANDLE(engine);
    if (!obj_type || !obj_name || !flag_name) return SWMM_ERR_BADPARAM;
    auto& flags = to_engine(engine)->context().user_flags;
    flags.unset(upper_key(obj_type), obj_name, upper_key(flag_name));
    return SWMM_OK;
}

} /* extern "C" */
