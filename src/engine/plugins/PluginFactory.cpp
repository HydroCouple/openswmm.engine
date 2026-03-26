/**
 * @file PluginFactory.cpp
 * @brief Plugin loader and lifecycle manager — implementation.
 *
 * @see PluginFactory.hpp
 * @ingroup engine_plugins
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "PluginFactory.hpp"

#include "../core/SimulationContext.hpp"
#include "../../../include/openswmm/plugin_sdk/IPluginComponentInfo.hpp"
#include "../../../include/openswmm/plugin_sdk/IOutputPlugin.hpp"
#include "../../../include/openswmm/plugin_sdk/IReportPlugin.hpp"
#include "../../../include/openswmm/plugin_sdk/PluginState.hpp"
#include "../../../include/openswmm/plugin_sdk/SimulationSnapshot.hpp"

// Platform-specific dynamic loading
#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#elif defined(__APPLE__) || defined(__linux__)
#  include <dlfcn.h>
#else
#  error "PluginFactory: unsupported platform"
#endif

#include <string>
#include <stdexcept>

namespace openswmm {

// ============================================================================
// Destructor
// ============================================================================

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
// load_plugins()
// ============================================================================

int PluginFactory::load_plugins(
    const std::vector<PluginSpec>& specs,
    std::function<void(const std::string&)> warn_cb)
{
    int loaded = 0;

    for (const auto& spec : specs) {
        void* handle = platform_load(spec.path);
        if (!handle) {
            if (warn_cb) {
                warn_cb("PluginFactory: failed to load '" + spec.path +
                        "': " + platform_error());
            }
            continue;
        }

        // Resolve factory function
        using FactoryFn = IPluginComponentInfo*(*)();
        auto factory_fn = reinterpret_cast<FactoryFn>(
            platform_sym(handle, "openswmm_plugin_info"));

        if (!factory_fn) {
            if (warn_cb) {
                warn_cb("PluginFactory: '" + spec.path +
                        "' does not export openswmm_plugin_info()");
            }
            platform_unload(handle);
            continue;
        }

        IPluginComponentInfo* info = factory_fn();
        if (!info) {
            if (warn_cb) {
                warn_cb("PluginFactory: openswmm_plugin_info() returned null in '" +
                        spec.path + "'");
            }
            platform_unload(handle);
            continue;
        }

        LibEntry entry;
        entry.handle = handle;
        entry.info   = info;
        entry.path   = spec.path;
        libs_.push_back(entry);

        // Create output plugin instance if supported
        if (info->provides_output()) {
            IOutputPlugin* op = info->create_output_plugin();
            if (op) {
                output_plugins_.push_back(op);
                init_args_.push_back(spec.init_args);
            }
        }

        // Create report plugin instance if supported
        if (info->provides_report()) {
            IReportPlugin* rp = info->create_report_plugin();
            if (rp) {
                report_plugins_.push_back(rp);
            }
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
        const auto& lib = libs_[0];  // info for this plugin
        // Find the matching lib entry for this plugin
        const std::vector<std::string>& args =
            (i < init_args_.size()) ? init_args_[i] : std::vector<std::string>{};

        // Find the component info for this plugin's library
        IPluginComponentInfo* info = nullptr;
        for (const auto& le : libs_) {
            if (le.info && le.info->provides_output()) {
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
            if (le.info && le.info->provides_report()) {
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
// unload_all()
// ============================================================================

// ============================================================================
// add_output_plugin() / add_report_plugin()
// ============================================================================

void PluginFactory::add_output_plugin(IOutputPlugin* plugin,
                                      std::vector<std::string> args) {
    output_plugins_.push_back(plugin);
    init_args_.push_back(std::move(args));
}

void PluginFactory::add_report_plugin(IReportPlugin* plugin) {
    report_plugins_.push_back(plugin);
}

// ============================================================================
// unload_all()
// ============================================================================

void PluginFactory::unload_all() {
    // Delete plugin instances before closing libs (vtable lives in lib)
    for (auto* p : output_plugins_) delete p;
    output_plugins_.clear();
    for (auto* p : report_plugins_) delete p;
    report_plugins_.clear();
    init_args_.clear();

    // Close library handles
    for (auto& lib : libs_) {
        platform_unload(lib.handle);
    }
    libs_.clear();
}

} /* namespace openswmm */
