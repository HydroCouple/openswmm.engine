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
 * @file      DefaultStateIOPlugin.hpp
 * @brief     Built-in state IO plugin — wraps HotStartManager for read/write.
 *
 * @details Provides the default `IStateIOPlugin` implementation. Reads and
 *          writes the OpenSWMM `OPENSWMM_HS_V1` / `V2` binary hot-start format
 *          (and recognises legacy `SWMM5-HOTSTART*` files for read).
 *
 *          Solver-internal state (infiltration, groundwater) flows through
 *          `SimulationContext::state_accessors`, wired by SWMMEngine at
 *          `open()` time. The plugin itself holds no references to engine
 *          subsystems and is fully self-contained.
 *
 * @see IStateIOPlugin.hpp
 * @see HotStartManager.hpp
 * @see SimulationContext::state_accessors
 * @see docs/STATE_IO_PLUGIN_PLAN.md
 * @ingroup engine_plugins
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_DEFAULT_STATE_IO_PLUGIN_HPP
#define OPENSWMM_ENGINE_DEFAULT_STATE_IO_PLUGIN_HPP

#include "../../../include/openswmm/plugin_sdk/IStateIOPlugin.hpp"

#include <string>
#include <vector>

namespace openswmm {

/**
 * @brief Default state-IO plugin: OpenSWMM binary + legacy SWMM5 hot-start.
 *
 * @ingroup engine_plugins
 */
class DefaultStateIOPlugin : public IStateIOPlugin {
public:
    /** @brief Construct an UNLOADED stateless hot-start facade with empty diagnostics. */
    DefaultStateIOPlugin() = default;

    /** @brief Read the last assigned lifecycle marker without synchronization.
     * @return Stored state; methods do not generally enforce an incoming-state transition.
     * Serialize plugin lifecycle/IO with active worker calls. */
    PluginState state() const noexcept override { return state_; }

    /** @brief Mark INITIALIZED and clear last_error.
     * @param init_args Ignored initialization strings.
     * @param info Ignored optional metadata pointer.
     * @return Zero; the previous warnings list is retained. */
    int initialize(const std::vector<std::string>& init_args,
                   const IPluginComponentInfo* info) override;

    /** @brief Mark VALIDATED without inspecting model compatibility.
     * @param ctx Ignored context; format/model checks happen during state application.
     * @return Zero unconditionally. */
    int validate(const SimulationContext& ctx) override;

    /** @brief Probe a leading hot-start signature or a recognized filename suffix.
     * @param path UTF-8 filename; attempts a read-only 16-byte prefix, then also accepts
     * a case-insensitive .hs/.hsf suffix even for absent or invalid-content files.
     * @return True for the recognized OpenSWMM/legacy prefix or suffix fallback. This
     * is a format-selection hint, not full validation or proof read_state() will succeed.
     * Does not mutate diagnostics, warnings or lifecycle state. */
    bool can_read(const std::string& path) const override;

    /** @brief Open and apply a hot-start through HotStartManager.
     * @param path UTF-8 source filename; this call does not rely on can_read() being called.
     * @param[in,out] ctx Compatible model/solver state with state_accessors wired when
     * solver-internal restoration is required. Application can mutate a prefix before
     * a later failure; caller owns transaction/rollback if needed.
     * @return Zero on successful apply, one on normal open/apply failure. Clears warnings
     * and last_error initially; appends apply warnings plus file warnings on success.
     * No lifecycle state change occurs and exceptions propagate. */
    int read_state(const std::string& path, SimulationContext& ctx) override;

    /** @brief Save current model/solver state through HotStartManager.
     * @param path UTF-8 destination following the manager's direct-write behavior;
     * this plugin adds no atomic replacement or rollback.
     * @param ctx Borrowed state; wired state_accessors include solver-internal data
     * and let the manager select its extended format.
     * @return Zero after save returns an owned handle (deleted here), one for a normal
     * failed save. Clears warnings/error initially, does not copy save warnings, and
     * does not change lifecycle state. Exceptions propagate. */
    int write_state(const std::string& path, const SimulationContext& ctx) override;

    /** @brief Copy warnings collected by the last read_state()/write_state() cycle.
     * @return Independent list; initialize/validate/finalize do not clear it. */
    std::vector<std::string> warnings() const override { return warnings_; }

    /** @brief Mark FINALIZED without opening/closing a persistent stream.
     * @param ctx Ignored context.
     * @return Zero; diagnostics and warnings remain. */
    int finalize(const SimulationContext& ctx) override;

    /** @brief Borrow the stored diagnostic text without clearing it.
     * @return NUL-terminated member bytes, possibly empty/stale; valid until the next
     * error mutation or destruction. Never free; use the method return code for success. */
    const char* last_error_message() const noexcept override { return last_error_.c_str(); }

private:
    PluginState state_ = PluginState::UNLOADED;
    std::string last_error_;
    std::vector<std::string> warnings_;
};

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_DEFAULT_STATE_IO_PLUGIN_HPP */
