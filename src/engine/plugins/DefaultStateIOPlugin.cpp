/**
 * @file DefaultStateIOPlugin.cpp
 * @brief Built-in state IO plugin implementation.
 *
 * @see DefaultStateIOPlugin.hpp
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "DefaultStateIOPlugin.hpp"

#include "../core/HotStartManager.hpp"
#include "../core/SimulationContext.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>

namespace openswmm {

namespace {

// Read the leading bytes of a file into buf without depending on
// HotStartManager internals. Returns the number of bytes read.
std::size_t read_head(const std::string& path, char* buf, std::size_t len) noexcept {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return 0;
    const std::size_t n = std::fread(buf, 1, len, f);
    std::fclose(f);
    return n;
}

bool ends_with_ci(const std::string& s, const char* suffix) noexcept {
    const std::size_t n = std::strlen(suffix);
    if (s.size() < n) return false;
    for (std::size_t i = 0; i < n; ++i) {
        const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(s[s.size() - n + i])));
        const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(suffix[i])));
        if (a != b) return false;
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------

int DefaultStateIOPlugin::initialize(
    const std::vector<std::string>& /*init_args*/,
    const IPluginComponentInfo*     /*info*/)
{
    state_ = PluginState::INITIALIZED;
    last_error_.clear();
    return 0;
}

int DefaultStateIOPlugin::validate(const SimulationContext& /*ctx*/) {
    state_ = PluginState::VALIDATED;
    return 0;
}

int DefaultStateIOPlugin::finalize(const SimulationContext& /*ctx*/) {
    state_ = PluginState::FINALIZED;
    return 0;
}

// ---------------------------------------------------------------------------
// can_read() — magic-number sniff for the formats this plugin handles.
// ---------------------------------------------------------------------------

bool DefaultStateIOPlugin::can_read(const std::string& path) const {
    char head[16] = {};
    const std::size_t n = read_head(path, head, sizeof(head));
    if (n >= 16 && std::memcmp(head, "OPENSWMM_HS_V1\0", 15) == 0) {
        return true;
    }
    if (n >= 14 && std::memcmp(head, "SWMM5-HOTSTART", 14) == 0) {
        // Legacy V1..V4 magic prefix; suffix digit is part of the same field.
        return true;
    }
    // Extension-only fallback for cases where the file does not yet exist
    // (e.g. Save dialogs invoking can_read() with a target path).
    return ends_with_ci(path, ".hs") || ends_with_ci(path, ".hsf");
}

// ---------------------------------------------------------------------------
// read_state() — open + apply via HotStartManager.
// ---------------------------------------------------------------------------

int DefaultStateIOPlugin::read_state(const std::string& path, SimulationContext& ctx) {
    warnings_.clear();
    last_error_.clear();

    std::unique_ptr<HotStartFile> hs(HotStartManager::open(path));
    if (!hs) {
        last_error_ = "DefaultStateIOPlugin: failed to open '" + path + "'";
        return 1;
    }

    auto warn_cb = [this](const std::string& msg) { warnings_.push_back(msg); };

    // Solver-internal state flows through ctx.state_accessors (wired by
    // SWMMEngine at open()). HotStartManager::apply auto-promotes to V2 when
    // accessors are available and the file is V2.
    HotStartManager::apply(*hs, ctx, warn_cb);

    // Surface any warnings collected by HotStartManager itself.
    for (const auto& w : hs->warnings) warnings_.push_back(w);

    return 0;
}

// ---------------------------------------------------------------------------
// write_state() — V2 path when solvers are available, else V1.
// ---------------------------------------------------------------------------

int DefaultStateIOPlugin::write_state(const std::string& path, const SimulationContext& ctx) {
    warnings_.clear();
    last_error_.clear();

    // HotStartManager::save auto-promotes to V2 when ctx.state_accessors are
    // wired (set by SWMMEngine), capturing infiltration + GW state.
    HotStartFile* hs = HotStartManager::save(ctx, path);

    if (!hs) {
        last_error_ = "DefaultStateIOPlugin: failed to write '" + path + "'";
        return 1;
    }

    delete hs;  // file is flushed by save(); handle not exposed.
    return 0;
}

} /* namespace openswmm */
