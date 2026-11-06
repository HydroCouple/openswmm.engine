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
 * @file      IStateIOPlugin.hpp
 * @brief     Interface for state / hot-start IO plugins.
 *
 * @details Implement this interface to support reading and writing simulation
 *          state (also called "hot-start") to and from different file formats
 *          (e.g., the built-in OpenSWMM binary, legacy SWMM5 binary, HDF5,
 *          NetCDF, cloud blob storage).
 *
 *          State plugins are responsible for:
 *          - Reading a persisted simulation state into a SimulationContext
 *            BEFORE simulation starts (engine in INITIALIZED state).
 *          - Writing the current simulation state to a path during a run
 *            (scheduled checkpoints) or once at the end (final snapshot).
 *
 *          **Threading model:** All methods run on the owning simulation thread; callers serialize access.
 *
 *          The engine resolves which plugin handles a given path on read by
 *          calling can_read() on each registered state plugin in turn; the
 *          first match wins, with the built-in default plugin acting as
 *          fallback so legacy formats keep working when no external plugin
 *          claims them.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

/**
 * @defgroup engine_plugin_state_io State IO Plugin Interface
 * @ingroup  engine_plugin_sdk
 *
 * @see IInputPlugin.hpp     — for model-data input plugins
 * @see IOutputPlugin.hpp    — for time-series output plugins
 * @see IReportPlugin.hpp    — for summary report plugins
 * @see IPluginComponentInfo.hpp
 */

#ifndef OPENSWMM_ISTATE_IO_PLUGIN_HPP
#define OPENSWMM_ISTATE_IO_PLUGIN_HPP

#include <string>
#include <vector>
#include "PluginState.hpp"

namespace openswmm {

struct SimulationContext;
class IPluginComponentInfo;

/**
 * @brief Interface for state / hot-start IO plugins.
 * @ingroup engine_plugin_state_io
 */
class IStateIOPlugin {
public:
    /** @brief Destroy this polymorphic instance after its callbacks and resource lifecycle finish.
     * Default base destructor performs no finalize, file flush, I/O-thread join or model
     * cleanup. Host deletes instances while their library remains loaded; implementations
     * release owned resources safely. Never delete shared component metadata here. */
    virtual ~IStateIOPlugin() = default;

    /** @brief Read this implementation's current plugin lifecycle state.
     * @return Implementation-maintained enum; factory uses it to gate validation,
     * preparation and output updates, but does not automatically perform transitions.
     * No ownership transfer. Base interface supplies no synchronization; serialize with
     * mutation, or implement a safe concurrent query if a host needs one. Must not throw. */
    virtual PluginState state() const noexcept = 0;

    /** @brief Parse instance configuration before model validation/preparation.
     * @param init_args Borrowed tokens for this call; copy retained values. Factory
     * output instances receive associated argument rows; report/state instances currently
     * receive an empty vector through initialize_all. Input hosts configure separately.
     * @param info Nullable borrowed component metadata. Library/host owns it; never
     * free it. Factory currently chooses first loaded metadata with matching capability,
     * so this pointer need not uniquely identify the instance. Retain only while the
     * component library is loaded and host guarantees its lifetime.
     * @return Zero on success, otherwise implementation error code/diagnostic. Successful
     * implementation sets INITIALIZED; factory aggregates codes without rollback or
     * setting ERROR. Define/reject repeat initialization consistently. Avoid retaining
     * live model references; defer model-dependent resources to later lifecycle calls. */
    virtual int initialize(const std::vector<std::string>& init_args,
                           const IPluginComponentInfo* info) = 0;

    /** @brief Qualify instance configuration against the loaded compatible model.
     * @param ctx Borrowed read-only initialized model for this call; arrays use their
     * native context units, not snapshot display units. Do not retain mutable/live-array
     * views across execution or assume the host transfers ownership.
     * @return Zero when accepted, otherwise implementation-specific error code. Set
     * VALIDATED on success and maintain failure diagnostics/state explicitly. Factory
     * calls only INITIALIZED instances; neither base nor factory provides validation,
     * transactional rollback or default path/identity compatibility checks. */
    virtual int validate(const SimulationContext& ctx) = 0;

    /** @brief Offer cheap format recognition for ordered state-reader dispatch.
     * @param path Borrowed candidate path; implementation may inspect extension or a
     * small header, without full decode or mutation of live model state.
     * @return True if this instance recognizes format, false otherwise (base always
     * false). Recognition does not certify file readability, identity or valid contents.
     * First claiming reader wins in host dispatch; keep probe inexpensive and serialize
     * with plugin resource mutation. No owned handle/path is returned. */
    virtual bool can_read(const std::string& path) const { return false; }

    /** @brief Decode supported persisted runtime state into an initialized compatible model.
     * @param path Borrowed source path/location; validity, identity and schema are
     * implementation-qualified independently of can_read's cheap recognition.
     * @param[in,out] ctx Host-owned initialized model/solver state accessor set; native
     * units/layout/lifecycle must match selected format. Host invokes before new run.
     * @return Zero on accepted restoration, otherwise implementation error/diagnostic.
     * Nonfatal omissions may be copied from warnings(), but native/coupled archives can
     * require exact identity. Interface supplies no model transaction; implementations
     * must document candidate validation and partial-state behavior on error. Do not
     * retain mutable model pointers or race solver/IO/model edits. */
    virtual int read_state(const std::string& path, SimulationContext& ctx) = 0;

    /** @brief Save supported runtime state at a caller-qualified accepted boundary.
     * @param path Borrowed destination path/location, potentially replaced by this call.
     * @param ctx Read-only compatible host state/accessors in native units. Caller
     * serializes all solver/source/thermal owners and plugin access for a coherent snapshot.
     * @return Zero on accepted save, otherwise implementation I/O/format code. Implementers
     * should complete/close per-call resources and describe staging, companion files and
     * flush/durability policy; this interface promises neither fsync nor global atomicity.
     * Safe repetition and supported checkpoint/hot-start fidelity are format-specific;
     * partial files may remain after failure. */
    virtual int write_state(const std::string& path, const SimulationContext& ctx) = 0;

    /** @brief Copy nonfatal diagnostics from the implementation's last state I/O operation.
     * @return Independent message vector; base returns empty. Implementations document
     * which operation clears messages, typically each read/write. Empty does not certify
     * complete restore, exact model identity or absence of a fatal error. Serialize query
     * with state I/O and copy diagnostics before invoking the next operation. */
    virtual std::vector<std::string> warnings() const { return {}; }

    /** @brief Finish this instance after producer/update work has stopped.
     * @param ctx Borrowed compatible final model/native-unit metadata for this call;
     * read without mutation or retention of live pointers.
     * @return Zero on successful resource cleanup, otherwise implementation-specific
     * error. Implementation owns flush/close/state transitions and repeat-call policy.
     * Factory calls every instance without a state gate, including partly initialized
     * ones, so cleanup must handle partial resources. Base destructor does not call this.
     * Output/report callbacks must be drained/joined before this owning-thread call. */
    virtual int finalize(const SimulationContext& ctx) = 0;

    /** @brief Borrow the latest implementation error text without clearing it.
     * @return Null-terminated member/static bytes; default is a nonnull empty literal.
     * Never free. Copy before the next potentially mutating plugin call or destruction;
     * implementations define diagnostic invalidation and should return nonnull strings.
     * No base synchronization; serialize access to mutable diagnostic storage. Must not throw. */
    virtual const char* last_error_message() const noexcept { return ""; }
};

} /* namespace openswmm */

#endif /* OPENSWMM_ISTATE_IO_PLUGIN_HPP */
