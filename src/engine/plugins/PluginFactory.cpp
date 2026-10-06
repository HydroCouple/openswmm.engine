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
 * @file PluginFactory.cpp
 * @brief Plugin loader, auto-discovery, and lifecycle manager — implementation.
 *
 * @see PluginFactory.hpp
 * @ingroup engine_plugins
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "PluginFactory.hpp"
#include "BuiltinPluginInfos.hpp"

// Slice RC.1 — GeoPackage is registered as an explicit built-in so it
// appears in the discovery API alongside the four Default plugins,
// instead of being picked up accidentally through the discover() scan's
// dlsym(openswmm_plugin_info) on the engine's own binary. See §R.3 in
// docs/GUI_IMPLEMENTATION_PLAN.md for the rationale.
#ifdef OPENSWMM_HAS_HDF5_MODEL
#  include "../io/hdf5/Hdf5PluginInfo.hpp"
#endif

#ifdef OPENSWMM_HAS_GEOPACKAGE
#  include "../input/geopackage/GeoPackagePluginInfo.hpp"
#endif

#include "../core/SimulationContext.hpp"
#include "../../../include/openswmm/plugin_sdk/IPluginComponentInfo.hpp"
#include "../../../include/openswmm/plugin_sdk/IInputPlugin.hpp"
#include "../../../include/openswmm/plugin_sdk/IOutputPlugin.hpp"
#include "../../../include/openswmm/plugin_sdk/IReportPlugin.hpp"
#include "../../../include/openswmm/plugin_sdk/IStateIOPlugin.hpp"
#include "../../../include/openswmm/plugin_sdk/PluginState.hpp"
#include "../../../include/openswmm/plugin_sdk/SimulationSnapshot.hpp"

#include <filesystem>
#include <algorithm>
#include <string>
#include <stdexcept>
#include <cstdlib>
#include <cstring>
#include <mutex>

// D2 (program plan §B.2.2): the HydroCouple component loader contract,
// upstreamed into HydroCouple in D0. Header-only; D-C6 forbids the SDK.
#ifdef OPENSWMM_HAS_HYDROCOUPLE
#include <hydrocouplecomponentabi.h>
#endif

// Platform-specific dynamic loading and path detection
#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#elif defined(__EMSCRIPTEN__)
   // WebAssembly: no dynamic loading (engine is statically linked into the
   // .wasm). platform_* helpers below are stubs; built-in plugins
   // (register_builtin_infos) are unaffected.
#elif defined(__APPLE__) || defined(__linux__)
#  include <dlfcn.h>
#else
#  error "PluginFactory: unsupported platform"
#endif

namespace fs = std::filesystem;

namespace openswmm {

// ============================================================================
// D2 — the process-global HydroCouple component catalogue (state)
// ============================================================================
//
// File-local so the header exposes no HydroCouple type. Everything in it is
// guarded by component_catalog_mutex(); see PluginFactory.hpp for why it is
// process-global and why its handles are never closed outside tests.
namespace {

struct ComponentCatalogState {
    bool                                               defaults_done = false;
    std::vector<std::string>                           explicit_dirs;
    std::vector<PluginFactory::ComponentLibraryRecord> records;
    std::vector<void*>                                 handles;
};

ComponentCatalogState& component_catalog_state() {
    static ComponentCatalogState s;
    return s;
}

std::mutex& component_catalog_mutex() {
    static std::mutex m;
    return m;
}

/// The same library reached through two spellings of its directory (a
/// symlinked build tree, `./components` vs an absolute path) must be one
/// record, so paths are compared canonicalised.
std::string component_canonical_path(const std::string& p) {
    std::error_code ec;
    const fs::path c = fs::weakly_canonical(fs::path(p), ec);
    return ec ? p : c.string();
}

} // namespace

// ============================================================================
// Constructor / Destructor
// ============================================================================

PluginFactory::PluginFactory() {
    register_builtin_infos();
    discover();
}

PluginFactory::~PluginFactory() {
    unload_all();
}

// ============================================================================
// Platform helpers
// ============================================================================

void* PluginFactory::platform_load(const std::string& path) {
#if defined(_WIN32)
    return static_cast<void*>(::LoadLibraryA(path.c_str()));
#elif defined(__EMSCRIPTEN__)
    (void)path;
    return nullptr;
#else
    return ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void PluginFactory::platform_unload(void* handle) noexcept {
    if (!handle) return;
#if defined(_WIN32)
    ::FreeLibrary(static_cast<HMODULE>(handle));
#elif defined(__EMSCRIPTEN__)
    // nothing to unload
#else
    ::dlclose(handle);
#endif
}

void* PluginFactory::platform_sym(void* handle, const char* sym) noexcept {
    if (!handle) return nullptr;
#if defined(_WIN32)
    return reinterpret_cast<void*>(
        ::GetProcAddress(static_cast<HMODULE>(handle), sym));
#elif defined(__EMSCRIPTEN__)
    (void)sym;
    return nullptr;
#else
    return ::dlsym(handle, sym);
#endif
}

std::string PluginFactory::platform_error() noexcept {
#if defined(_WIN32)
    DWORD err = ::GetLastError();
    char buf[256] = {};
    ::FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, err,
                     MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                     buf, sizeof(buf), nullptr);
    return std::string(buf);
#elif defined(__EMSCRIPTEN__)
    return "dynamic plugin loading is not available in WebAssembly";
#else
    const char* msg = ::dlerror();
    return msg ? std::string(msg) : "(unknown dlerror)";
#endif
}

// ============================================================================
// Path helpers
// ============================================================================

std::string PluginFactory::get_library_directory() {
#if defined(_WIN32)
    // Get the directory of the DLL containing this function
    HMODULE hm = nullptr;
    if (::GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&get_library_directory),
            &hm)) {
        char buf[MAX_PATH] = {};
        if (::GetModuleFileNameA(hm, buf, MAX_PATH) > 0) {
            fs::path p(buf);
            return p.parent_path().string();
        }
    }
    return {};
#elif defined(__EMSCRIPTEN__)
    return {};
#else
    // Use dladdr to find the shared library containing this function
    Dl_info dl_info;
    if (::dladdr(reinterpret_cast<void*>(&get_library_directory), &dl_info)
        && dl_info.dli_fname) {
        fs::path p(dl_info.dli_fname);
        return p.parent_path().string();
    }
    return {};
#endif
}

bool PluginFactory::is_file_path(const std::string& str) {
    if (str.empty()) return false;
    // Contains path separator
    if (str.find('/') != std::string::npos) return true;
    if (str.find('\\') != std::string::npos) return true;
    // Ends with known shared library extension
    return is_shared_library(str);
}

bool PluginFactory::is_shared_library(const std::string& filename) {
    if (filename.size() < 3) return false;
    // Check common extensions
    auto ends_with = [](const std::string& s, const std::string& suffix) {
        return s.size() >= suffix.size() &&
               s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
#if defined(_WIN32)
    return ends_with(filename, ".dll");
#elif defined(__APPLE__)
    return ends_with(filename, ".dylib") || ends_with(filename, ".so");
#else
    return ends_with(filename, ".so");
#endif
}

// ============================================================================
// Discovery
// ============================================================================

void PluginFactory::discover(std::function<void(const std::string&)> warn_cb) {
    std::string base_dir = get_library_directory();
    if (base_dir.empty()) return;

    scan_directory(base_dir, warn_cb);

    fs::path plugins_dir = fs::path(base_dir) / "plugins";
    if (fs::is_directory(plugins_dir)) {
        scan_directory(plugins_dir.string(), warn_cb);
    }

    fs::path components_dir = fs::path(base_dir) / "components";
    if (fs::is_directory(components_dir)) {
        scan_directory(components_dir.string(), warn_cb);
    }
}

void PluginFactory::scan_directory(
    const std::string& dir_path,
    std::function<void(const std::string&)> warn_cb)
{
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir_path, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const std::string filename = entry.path().filename().string();
        if (!is_shared_library(filename)) continue;

        // Skip if already loaded (by path)
        const std::string full_path = entry.path().string();
        bool already_loaded = false;
        for (const auto& lib : libs_) {
            if (lib.path == full_path) { already_loaded = true; break; }
        }
        if (already_loaded) continue;

        load_library(full_path, warn_cb);
    }
}

// ============================================================================
// load_library()
// ============================================================================

IPluginComponentInfo* PluginFactory::load_library(
    const std::string& path,
    std::function<void(const std::string&)> warn_cb)
{
    // Check if already loaded by path
    for (const auto& lib : libs_) {
        if (lib.path == path) return lib.info;
    }

    void* handle = platform_load(path);
    if (!handle) {
        // Silently skip during discovery — only warn for explicit loads
        if (warn_cb) {
            warn_cb("PluginFactory: failed to load '" + path +
                    "': " + platform_error());
        }
        return nullptr;
    }

    // Resolve factory function
    using FactoryFn = IPluginComponentInfo*(*)();
    auto factory_fn = reinterpret_cast<FactoryFn>(
        platform_sym(handle, "openswmm_plugin_info"));

    if (!factory_fn) {
        // Not an OpenSWMM IO plugin. D2: offer it to the HydroCouple
        // component catalogue before unloading, so a component library found
        // by any engine's discovery is recorded once, process-wide. If the
        // catalogue takes it, the handle is the catalogue's now.
        {
            std::lock_guard<std::mutex> lock(component_catalog_mutex());
            if (catalog_adopt_locked(path, handle)) return nullptr;
        }
        // Not a component either — silently skip, as before.
        platform_unload(handle);
        return nullptr;
    }

    IPluginComponentInfo* info = factory_fn();
    if (!info) {
        if (warn_cb) {
            warn_cb("PluginFactory: openswmm_plugin_info() returned null in '" +
                    path + "'");
        }
        platform_unload(handle);
        return nullptr;
    }

    // Store in libs_
    LibEntry entry;
    entry.handle = handle;
    entry.info   = info;
    entry.path   = path;
    std::size_t idx = libs_.size();
    libs_.push_back(entry);

    // Register in component registry: "id:version" and "id" (without version)
    std::string id = info->id();
    std::string ver = info->version();
    std::string key = id + ":" + ver;

    registry_[key] = idx;
    // Also register by id alone (first-loaded wins)
    if (registry_.find(id) == registry_.end()) {
        registry_[id] = idx;
    }

    // Verify the plugin's file_filters() advertise every capability it claims.
    validate_filter_invariant(info, path, warn_cb);

    return info;
}

// ============================================================================
// find_component()
// ============================================================================

IPluginComponentInfo* PluginFactory::find_component(
    const std::string& id_or_path,
    std::function<void(const std::string&)> warn_cb)
{
    if (id_or_path.empty()) return nullptr;

    // Step 1: Is it a file path? Load directly.
    if (is_file_path(id_or_path)) {
        return load_library(id_or_path, warn_cb);
    }

    // Step 2: Look up in registry by "id:version" or "id"
    auto it = registry_.find(id_or_path);
    if (it != registry_.end() && it->second < libs_.size()) {
        return libs_[it->second].info;
    }

    if (warn_cb) {
        warn_cb("PluginFactory: plugin '" + id_or_path +
                "' not found in registry or on disk");
    }
    return nullptr;
}

// ============================================================================
// discovered_components()
// ============================================================================

std::vector<PluginFactory::ComponentEntry> PluginFactory::discovered_components() const {
    std::vector<ComponentEntry> result;
    result.reserve(libs_.size());
    for (const auto& lib : libs_) {
        if (!lib.info) continue;
        ComponentEntry e;
        e.id           = lib.info->id();
        e.version      = lib.info->version();
        e.has_input    = lib.info->has_input();
        e.has_output   = lib.info->has_output();
        e.has_report   = lib.info->has_report();
        e.has_state_io = lib.info->has_state_io();
        e.info         = lib.info;
        // Slice RC.3 — built-ins are registered with a synthetic
        // `<built-in>` path and a null dlopen handle. Either marker
        // would do; matching on path keeps the check stable across
        // the future case where built-ins might be promoted to
        // load-on-demand shared libs.
        e.is_builtin   = (lib.path == "<built-in>");
        result.push_back(e);
    }
    return result;
}

// ============================================================================
// load_plugins()
// ============================================================================

int PluginFactory::load_plugins(
    const std::vector<PluginSpec>& specs,
    std::function<void(const std::string&)> warn_cb)
{
    int loaded = 0;

    for (const auto& spec : specs) {
        IPluginComponentInfo* info = find_component(spec.path, warn_cb);
        if (!info) continue;

        // Create plugin instances for each supported capability
        if (info->has_input()) {
            IInputPlugin* ip = info->create_input_plugin();
            if (ip) input_plugins_.push_back(ip);
        }
        if (info->has_output()) {
            IOutputPlugin* op = info->create_output_plugin();
            if (op) {
                output_plugins_.push_back(op);
                init_args_.push_back(spec.init_args);
            }
        }
        if (info->has_report()) {
            IReportPlugin* rp = info->create_report_plugin();
            if (rp) report_plugins_.push_back(rp);
        }
        if (info->has_state_io()) {
            IStateIOPlugin* sp = info->create_state_io_plugin();
            if (sp) state_io_plugins_.push_back(sp);
        }

        ++loaded;
    }

    return loaded;
}

// ============================================================================
// initialize_all()
// ============================================================================

int PluginFactory::initialize_all(SimulationContext& /*ctx*/) {
    int last_err = 0;

    for (std::size_t i = 0; i < output_plugins_.size(); ++i) {
        IOutputPlugin* p = output_plugins_[i];
        const std::vector<std::string>& args =
            (i < init_args_.size()) ? init_args_[i] : std::vector<std::string>{};

        IPluginComponentInfo* info = nullptr;
        for (const auto& le : libs_) {
            if (le.info && le.info->has_output()) {
                info = le.info;
                break;
            }
        }

        const int rc = p->initialize(args, info);
        if (rc != 0) last_err = rc;
    }

    for (auto* p : report_plugins_) {
        IPluginComponentInfo* info = nullptr;
        for (const auto& le : libs_) {
            if (le.info && le.info->has_report()) {
                info = le.info;
                break;
            }
        }
        const int rc = p->initialize({}, info);
        if (rc != 0) last_err = rc;
    }

    for (auto* p : state_io_plugins_) {
        IPluginComponentInfo* info = nullptr;
        for (const auto& le : libs_) {
            if (le.info && le.info->has_state_io()) {
                info = le.info;
                break;
            }
        }
        const int rc = p->initialize({}, info);
        if (rc != 0) last_err = rc;
    }

    return last_err;
}

// ============================================================================
// validate_all()
// ============================================================================

int PluginFactory::validate_all(SimulationContext& ctx) {
    int last_err = 0;
    for (auto* p : output_plugins_) {
        if (p->state() != PluginState::INITIALIZED) continue;
        const int rc = p->validate(ctx);
        if (rc != 0) last_err = rc;
    }
    for (auto* p : report_plugins_) {
        if (p->state() != PluginState::INITIALIZED) continue;
        const int rc = p->validate(ctx);
        if (rc != 0) last_err = rc;
    }
    for (auto* p : state_io_plugins_) {
        if (p->state() != PluginState::INITIALIZED) continue;
        const int rc = p->validate(ctx);
        if (rc != 0) last_err = rc;
    }
    return last_err;
}

// ============================================================================
// prepare_all()
// ============================================================================

int PluginFactory::prepare_all(SimulationContext& ctx) {
    int last_err = 0;
    for (auto* p : output_plugins_) {
        if (p->state() != PluginState::VALIDATED) continue;
        const int rc = p->prepare(ctx);
        if (rc != 0) last_err = rc;
    }
    for (auto* p : report_plugins_) {
        if (p->state() != PluginState::VALIDATED) continue;
        const int rc = p->prepare(ctx);
        if (rc != 0) last_err = rc;
    }
    return last_err;
}

// ============================================================================
// update_all()  — called from the IO thread
// ============================================================================

int PluginFactory::update_all(const SimulationSnapshot& snapshot) {
    int last_err = 0;
    for (auto* p : output_plugins_) {
        const PluginState s = p->state();
        if (s != PluginState::PREPARED && s != PluginState::UPDATING) continue;
        const int rc = p->update(snapshot);
        if (rc != 0) last_err = rc;
    }
    for (auto* p : report_plugins_) {
        const PluginState s = p->state();
        if (s != PluginState::PREPARED && s != PluginState::UPDATING) continue;
        const int rc = p->update(snapshot);
        if (rc != 0) last_err = rc;
    }
    return last_err;
}

// ============================================================================
// finalize_all()
// ============================================================================

int PluginFactory::finalize_all(SimulationContext& ctx) {
    int last_err = 0;
    for (auto* p : output_plugins_) {
        const int rc = p->finalize(ctx);
        if (rc != 0) last_err = rc;
    }
    for (auto* p : report_plugins_) {
        const int rc = p->finalize(ctx);
        if (rc != 0) last_err = rc;
    }
    for (auto* p : state_io_plugins_) {
        const int rc = p->finalize(ctx);
        if (rc != 0) last_err = rc;
    }
    return last_err;
}

// ============================================================================
// write_summary_all()
// ============================================================================

int PluginFactory::write_summary_all(SimulationContext& ctx) {
    int last_err = 0;
    for (auto* p : report_plugins_) {
        const int rc = p->write_summary(ctx);
        if (rc != 0) last_err = rc;
    }
    return last_err;
}

// ============================================================================
// Plugin injection
// ============================================================================

void PluginFactory::add_output_plugin(IOutputPlugin* plugin,
                                      std::vector<std::string> args) {
    output_plugins_.push_back(plugin);
    init_args_.push_back(std::move(args));
}

void PluginFactory::add_report_plugin(IReportPlugin* plugin) {
    report_plugins_.push_back(plugin);
}

void PluginFactory::add_input_plugin(IInputPlugin* plugin) {
    input_plugins_.push_back(plugin);
}

void PluginFactory::add_state_io_plugin(IStateIOPlugin* plugin) {
    state_io_plugins_.push_back(plugin);
}

// ============================================================================
// unload_all()
// ============================================================================

void PluginFactory::unload_all() {
    // Delete plugin instances before closing libs (vtable lives in lib)
    for (auto* p : input_plugins_) delete p;
    input_plugins_.clear();
    for (auto* p : output_plugins_) delete p;
    output_plugins_.clear();
    for (auto* p : report_plugins_) delete p;
    report_plugins_.clear();
    for (auto* p : state_io_plugins_) delete p;
    state_io_plugins_.clear();
    init_args_.clear();

    // Clear registry
    registry_.clear();

    // Close library handles
    for (auto& lib : libs_) {
        platform_unload(lib.handle);
    }
    libs_.clear();
}

// ============================================================================
// register_builtin_infos()
// ============================================================================

void PluginFactory::register_builtin_infos() {
    auto register_one = [this](IPluginComponentInfo* info) {
        if (!info) return;
        const std::string id  = info->id();
        const std::string ver = info->version();
        const std::string key = id + ":" + ver;
        if (registry_.find(key) != registry_.end()) return;  // idempotent

        LibEntry entry;
        entry.handle = nullptr;            // synthetic — no dlopen handle
        entry.info   = info;
        entry.path   = "<built-in>";
        const std::size_t idx = libs_.size();
        libs_.push_back(entry);

        registry_[key] = idx;
        if (registry_.find(id) == registry_.end()) {
            registry_[id] = idx;
        }
    };

    register_one(&BuiltinDefaultInputPluginInfo::instance());
    register_one(&BuiltinDefaultOutputPluginInfo::instance());
    register_one(&BuiltinDefaultReportPluginInfo::instance());
    register_one(&BuiltinDefaultStateIOPluginInfo::instance());

    // Slice RC.1 (APPROVED 2026-05-25, see §R.3) — GeoPackage is a first-
    // class statically-linked plugin. Registering it explicitly here, plus
    // the symbol-visibility hardening on the openswmm_geopackage target
    // (Slice RC.2), removes the accidental dlsym-leak via the engine
    // binary that produced phantom rows in the Simulation Options
    // Plugins tab.
#ifdef OPENSWMM_HAS_GEOPACKAGE
    register_one(&openswmm::gpkg::GeoPackagePluginInfo::instance());
#endif

    // The HDF5 model writer registers the same way and for the same reason:
    // statically linked, so it must be announced explicitly rather than
    // discovered by dlsym. See src/engine/io/hdf5/STRATEGY.md.
#ifdef OPENSWMM_HAS_HDF5_MODEL
    register_one(&openswmm::h5io::Hdf5PluginInfo::instance());
#endif
}

// ============================================================================
// validate_filter_invariant()
// ============================================================================

void PluginFactory::validate_filter_invariant(
    IPluginComponentInfo* info,
    const std::string& source_path,
    std::function<void(const std::string&)> warn_cb)
{
    if (!info) return;

    const auto filters = info->file_filters();
    auto has_role = [&](PluginRole r) {
        for (const auto& f : filters) if (f.role == r) return true;
        return false;
    };

    auto warn = [&](const char* cap, PluginRole r) {
        std::string msg = "PluginFactory: plugin '" + info->id() +
                          "' (" + source_path + ") declares " + cap +
                          " capability but advertises no FileFilter with role " +
                          plugin_role_to_string(r) +
                          " — host file dialogs will show no label for this format.";
        if (warn_cb) warn_cb(msg);
    };

    if (info->has_input()    && !has_role(PluginRole::INPUT_READ))
        warn("INPUT",    PluginRole::INPUT_READ);
    if (info->has_output()   && !has_role(PluginRole::OUTPUT_WRITE))
        warn("OUTPUT",   PluginRole::OUTPUT_WRITE);
    if (info->has_report()   && !has_role(PluginRole::REPORT_WRITE))
        warn("REPORT",   PluginRole::REPORT_WRITE);
    if (info->has_state_io()
        && !has_role(PluginRole::STATE_READ)
        && !has_role(PluginRole::STATE_WRITE))
        warn("STATE_IO", PluginRole::STATE_READ);
}


// ============================================================================
// D2 — the process-global HydroCouple component catalogue
// ============================================================================

bool PluginFactory::hydrocouple_enabled() noexcept {
#ifdef OPENSWMM_HAS_HYDROCOUPLE
    return true;
#else
    return false;
#endif
}

std::string PluginFactory::component_abi_stamp() {
#ifdef OPENSWMM_HAS_HYDROCOUPLE
    return HYDROCOUPLE_COMPONENT_ABI_STAMP;
#else
    return {};
#endif
}

bool PluginFactory::catalog_adopt_locked(const std::string& path, void* handle) {
#ifdef OPENSWMM_HAS_HYDROCOUPLE
    auto& st = component_catalog_state();
    const std::string canon = component_canonical_path(path);
    for (const auto& r : st.records) {
        if (component_canonical_path(r.path) == canon) {
            // Already recorded through another discovery route. dlopen
            // refcounted this open; give the extra reference back.
            platform_unload(handle);
            return true;
        }
    }

    ComponentLibraryRecord rec;
    rec.path = path;
    HydroCouple::IComponentInfo* info = nullptr;

    // The stamp is the ONLY symbol that may be called before the check
    // succeeds: it is pure C returning a string, safe across any toolchain
    // mismatch. Nothing C++ is touched until the two stamps agree.
    auto abi_fn = reinterpret_cast<HydroCoupleComponentAbiFn>(
        platform_sym(handle, HYDROCOUPLE_COMPONENT_ABI_SYMBOL));

    if (abi_fn) {
        const char* theirs = abi_fn();
        rec.stamp = theirs ? theirs : "";
        if (rec.stamp != HYDROCOUPLE_COMPONENT_ABI_STAMP) {
            // Refused, and recorded: a library that looks like a component
            // but cannot safely be called is exactly what a user needs to be
            // told about, with both stamps, not silently skipped.
            rec.load_error = "refused: HydroCouple component ABI stamp mismatch — library '" +
                             rec.stamp + "', engine '" + HYDROCOUPLE_COMPONENT_ABI_STAMP + "'";
            st.records.push_back(std::move(rec));
            platform_unload(handle);
            return true;
        }
        auto info_fn = reinterpret_cast<HydroCoupleComponentInfoFn>(
            platform_sym(handle, HYDROCOUPLE_COMPONENT_INFO_SYMBOL));
        if (!info_fn) {
            rec.load_error = std::string("stamped, but does not export ") +
                             HYDROCOUPLE_COMPONENT_INFO_SYMBOL;
            st.records.push_back(std::move(rec));
            platform_unload(handle);
            return true;
        }
        info = info_fn();
    } else {
        // The pre-stamp convention HydroCouple's Python loader uses. There is
        // genuinely no way to verify its toolchain before calling it, so it
        // is loaded best-effort and REPORTED as unstamped — never silently
        // treated as verified. Refusing it would split the ecosystem.
        auto legacy_fn = reinterpret_cast<HydroCoupleLegacyComponentInfoFn>(
            platform_sym(handle, HYDROCOUPLE_COMPONENT_LEGACY_INFO_SYMBOL));
        if (!legacy_fn) return false;   // not a component: caller keeps the handle
        rec.stamp = HYDROCOUPLE_COMPONENT_UNSTAMPED;
        info = legacy_fn();
    }

    if (!info) {
        rec.load_error = "the component-info entry point returned null";
        st.records.push_back(std::move(rec));
        platform_unload(handle);
        return true;
    }

    rec.id      = info->id();
    rec.caption = info->caption();
    rec.version = info->version();
    rec.kind    = dynamic_cast<HydroCouple::IModelComponentInfo*>(info) ? "model" : "other";

    // One id, one library. The later one is refused rather than allowed to
    // shadow the earlier, and the message names both so the duplicate can be
    // found; which one "should" win is not something discovery can know.
    for (const auto& r : st.records) {
        if (r.load_error.empty() && r.id == rec.id) {
            rec.load_error = "refused: duplicate component id '" + rec.id +
                             "' — already loaded from '" + r.path + "'";
            rec.kind.clear();
            st.records.push_back(std::move(rec));
            platform_unload(handle);
            return true;
        }
    }

    info->setLibraryFilePath(path);
    rec.info = info;
    st.records.push_back(std::move(rec));
    st.handles.push_back(handle);   // kept for the life of the process
    return true;
#else
    (void)path; (void)handle;
    return false;
#endif
}

int PluginFactory::catalog_scan_locked(const std::string& dir) {
    const std::size_t before = component_catalog_state().records.size();
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return 0;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        if (!is_shared_library(entry.path().filename().string())) continue;
        const std::string path = entry.path().string();
        void* handle = platform_load(path);
        if (!handle) continue;   // discovery never fails on an unloadable file
        if (!catalog_adopt_locked(path, handle)) platform_unload(handle);
    }
    return static_cast<int>(component_catalog_state().records.size() - before);
}

void PluginFactory::catalog_defaults_locked() {
    auto& st = component_catalog_state();
    if (st.defaults_done) return;
    st.defaults_done = true;

    // §B.2.3, in order: next to the host library, then the environment.
    const std::string base = get_library_directory();
    if (!base.empty()) {
        catalog_scan_locked(base);
        catalog_scan_locked((fs::path(base) / "plugins").string());
        catalog_scan_locked((fs::path(base) / "components").string());
    }
    if (const char* env = std::getenv("HYDROCOUPLE_COMPONENT_PATH")) {
#if defined(_WIN32)
        const char sep = ';';
#else
        const char sep = ':';
#endif
        std::string list(env), item;
        for (std::size_t i = 0; i <= list.size(); ++i) {
            if (i == list.size() || list[i] == sep) {
                if (!item.empty()) catalog_scan_locked(item);
                item.clear();
            } else {
                item.push_back(list[i]);
            }
        }
    }
}

int PluginFactory::component_search_path_add(const std::string& dir) {
    if (!hydrocouple_enabled()) return -1;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return -1;
    std::lock_guard<std::mutex> lock(component_catalog_mutex());
    // Defaults first, so an explicit directory can never displace a default
    // one under the duplicate-id rule merely by being scanned earlier.
    catalog_defaults_locked();
    component_catalog_state().explicit_dirs.push_back(dir);
    return catalog_scan_locked(dir);
}

std::vector<PluginFactory::ComponentLibraryRecord> PluginFactory::component_libraries() {
    std::lock_guard<std::mutex> lock(component_catalog_mutex());
    if (hydrocouple_enabled()) catalog_defaults_locked();
    return component_catalog_state().records;
}

void PluginFactory::component_catalog_reset_for_testing() {
    std::lock_guard<std::mutex> lock(component_catalog_mutex());
    auto& st = component_catalog_state();
    for (void* h : st.handles) platform_unload(h);
    st = ComponentCatalogState{};
}

} /* namespace openswmm */
