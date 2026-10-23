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
 * @file      IInputPlugin.hpp
 * @brief     Interface for input file reader/writer plugins.
 *
 * @details Implement this interface to support reading and writing SWMM model
 *          data from/to different file formats (e.g., .inp, GeoJSON, HDF5,
 *          database).
 *
 *          Input plugins are responsible for:
 *          - Reading model data from a file into a SimulationContext
 *          - Writing model data from a SimulationContext to a file
 *
 *          **Threading model:** All methods run on the owning simulation thread; callers serialize access.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

/**
 * @defgroup engine_plugin_input Input Plugin Interface
 * @ingroup  engine_plugin_sdk
 *
 * @see IOutputPlugin.hpp    — for time-series output plugins
 * @see IReportPlugin.hpp    — for summary report plugins
 * @see IPluginComponentInfo.hpp
 */

#ifndef OPENSWMM_IINPUT_PLUGIN_HPP
#define OPENSWMM_IINPUT_PLUGIN_HPP

#include <string>
#include <vector>
#include "PluginState.hpp"

namespace openswmm {

struct SimulationContext;
class IPluginComponentInfo;

/**
 * @brief Interface for input file reader/writer plugins.
 * @ingroup engine_plugin_input
 */
class IInputPlugin {
public:
    /** @brief Destroy this polymorphic instance after its callbacks and resource lifecycle finish.
     * Default base destructor performs no finalize, file flush, I/O-thread join or model
     * cleanup. Host deletes instances while their library remains loaded; implementations
     * release owned resources safely. Never delete shared component metadata here. */
    virtual ~IInputPlugin() = default;

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

    /** @brief Populate authored model data from the plugin's supported input format.
     * @param path Borrowed filesystem/location string, interpreted by implementation;
     * copy if retained. Host supplies valid syntax/access for the chosen format.
     * @param[in,out] ctx Owning host's model store to populate, including aligned arrays,
     * names/options/tables and supported plugin_specs. Native authored/context unit
     * conventions apply. Host resolves cross references and validates after return.
     * @return Zero on parsed success, otherwise format/I/O error with last diagnostic.
     * Interface supplies no rollback; failed reads may partly populate context. Input
     * plugin is injected before engine open, because unread PLUGINS cannot discover it.
     * Owning simulation thread, serialized against all other model access. */
    virtual int read(const std::string& path, SimulationContext& ctx) = 0;

    /** @brief Serialize authored model data in this input plugin's format.
     * @param path Borrowed destination path/location; can be created/truncated/replaced
     * according to implementation policy. No base atomic-write/durability guarantee.
     * @param ctx Borrowed compatible read-only model in native authored/context units;
     * not an output-time snapshot or ownership transfer.
     * @return Zero on success, otherwise implementation I/O/format code/diagnostic.
     * May leave partial output on failure; does not imply a complete runtime checkpoint.
     * Serialize call with model mutation and plugin configuration/resource access. */
    virtual int write(const std::string& path, const SimulationContext& ctx) = 0;

    /** @brief Copy unsupported section/element identifiers recorded by the last model read.
     * @return Owned independent strings; default empty. Format may use INP tags or its
     * own keys. Empty alone does not establish full model fidelity or read success.
     * Implementation defines reset timing; serialize query with read/configuration. */
    virtual std::vector<std::string> skipped_sections() const { return {}; }

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

#endif /* OPENSWMM_IINPUT_PLUGIN_HPP */
