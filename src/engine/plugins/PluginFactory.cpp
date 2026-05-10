/**
 * @file PluginFactory.cpp
 * @brief Plugin loader, auto-discovery, and lifecycle manager — implementation.
 *
 * @see PluginFactory.hpp
 * @ingroup engine_plugins
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "PluginFactory.hpp"
#include "BuiltinPluginInfos.hpp"

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

// Platform-specific dynamic loading and path detection
#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#elif defined(__APPLE__) || defined(__linux__)
#  include <dlfcn.h>
#else
#  error "PluginFactory: unsupported platform"
#endif

namespace fs = std::filesystem;

namespace openswmm {

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
#else
    return ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void PluginFactory::platform_unload(void* handle) noexcept {
    if (!handle) return;
#if defined(_WIN32)
    ::FreeLibrary(static_cast<HMODULE>(handle));
#else
    ::dlclose(handle);
#endif
}

void* PluginFactory::platform_sym(void* handle, const char* sym) noexcept {
    if (!handle) return nullptr;
#if defined(_WIN32)
    return reinterpret_cast<void*>(
        ::GetProcAddress(static_cast<HMODULE>(handle), sym));
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
        // Not an OpenSWMM plugin — silently skip
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

} /* namespace openswmm */
