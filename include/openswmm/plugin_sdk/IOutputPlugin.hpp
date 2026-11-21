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
 * @file      IOutputPlugin.hpp
 * @brief     Interface for output-writing plugins.
 *
 * @details Implement this interface to write SWMM simulation results to custom
 *          formats (HDF5, NetCDF, CSV, database, etc.).
 *
 *          **Threading model:** IOutputPlugin::update() is called from the IO
 *          thread, not the main simulation thread. update() receives a
 *          `const SimulationSnapshot&` — a read-only deep copy of the simulation
 *          arrays at the output time, plus borrowed immutable model metadata. Do NOT access the live
 *          SimulationContext from within update().
 *
 *          All other lifecycle methods (initialize, validate, prepare, finalize)
 *          are called from the main thread before/after the simulation loop.
 *
 * @par Intended lifecycle order (implementations maintain their state)
 * @code
 *   initialize()  →  validate()  →  prepare()
 *      →  update() [N times, IO thread]
 *      →  finalize()
 * @endcode
 *
 * @note Return codes: 0 = success; non-zero = error. PluginFactory aggregates
 *       return codes but does not set plugin states; implementations own transitions.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

/**
 * @defgroup engine_plugin_output Output Plugin Interface
 * @ingroup  engine_plugin_sdk
 *
 * @see IReportPlugin.hpp   — for summary-report plugins
 * @see SimulationSnapshot.hpp — read-only snapshot passed to update()
 * @see IPluginComponentInfo.hpp — factory / metadata interface
 * @see Legacy reference: src/solver/output.c — swmm_step() output writing
 */

#ifndef OPENSWMM_IOUTPUT_PLUGIN_HPP
#define OPENSWMM_IOUTPUT_PLUGIN_HPP

#include <string>
#include <vector>
#include "PluginState.hpp"
#include "SimulationSnapshot.hpp"

/* Forward declaration — avoid pulling in the full SimulationContext */
namespace openswmm {
struct SimulationContext;
}

namespace openswmm {

/**
 * @brief Interface for output-writing plugins.
 * @ingroup engine_plugin_output
 */
class IOutputPlugin {
public:
    /** @brief Destroy this polymorphic instance after its callbacks and resource lifecycle finish.
     * Default base destructor performs no finalize, file flush, I/O-thread join or model
     * cleanup. Host deletes instances while their library remains loaded; implementations
     * release owned resources safely. Never delete shared component metadata here. */
    virtual ~IOutputPlugin() = default;

    // -----------------------------------------------------------------------
    // State query
    // -----------------------------------------------------------------------

    /** @brief Read this implementation's current plugin lifecycle state.
     * @return Implementation-maintained enum; factory uses it to gate validation,
     * preparation and output updates, but does not automatically perform transitions.
     * No ownership transfer. Base interface supplies no synchronization; serialize with
     * mutation, or implement a safe concurrent query if a host needs one. Must not throw. */
    virtual PluginState state() const noexcept = 0;

    // -----------------------------------------------------------------------
    // Lifecycle methods (main thread)
    // -----------------------------------------------------------------------

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
                           const class IPluginComponentInfo* info) = 0;

    /** @brief Qualify instance configuration against the loaded compatible model.
     * @param ctx Borrowed read-only initialized model for this call; arrays use their
     * native context units, not snapshot display units. Do not retain mutable/live-array
     * views across execution or assume the host transfers ownership.
     * @return Zero when accepted, otherwise implementation-specific error code. Set
     * VALIDATED on success and maintain failure diagnostics/state explicitly. Factory
     * calls only INITIALIZED instances; neither base nor factory provides validation,
     * transactional rollback or default path/identity compatibility checks. */
    virtual int validate(const SimulationContext& ctx) = 0;

    /** @brief Allocate/open model-dependent output resources before asynchronous updates.
     * @param ctx Borrowed compatible initialized model in native context units; copy
     * required immutable metadata. Does not transfer model ownership.
     * @return Zero after successful headers/resource setup and PREPARED transition,
     * otherwise implementation error. Factory calls only VALIDATED instances. Resource
     * creation may leave partial files/handles on failure; implementation must clean up
     * through its documented finalize/destructor path. Owning simulation thread only. */
    virtual int prepare(const SimulationContext& ctx) = 0;

    /** @brief Consume one output-time snapshot on the serialized I/O worker.
     * @param snapshot Borrowed read-only row, valid only during callback. Numeric vectors
     * are snapshot-owned, while ID/unit/species pointers borrow host metadata which must
     * remain immutable/alive until I/O drains. One-dimensional hydraulic reports use
     * project display units; 2D/snow fields retain documented SI units. Copy any retained
     * data and handle optional empty vectors/null metadata. Never access live context.
     * @return Zero on accepted processing, otherwise implementation error. Factory invokes
     * PREPARED/UPDATING instances and aggregates codes; implementation owns state/error
     * transition and partial-write handling. Main simulation may advance concurrently.
     * Base report update is an unconditional no-op returning zero. Exceptions are not
     * caught by factory; worker handles its own failure policy. */
    virtual int update(const SimulationSnapshot& snapshot) = 0;

    /** @brief Finish this instance after producer/update work has stopped.
     * @param ctx Borrowed compatible final model/native-unit metadata for this call;
     * read without mutation or retention of live pointers.
     * @return Zero on successful resource cleanup, otherwise implementation-specific
     * error. Implementation owns flush/close/state transitions and repeat-call policy.
     * Factory calls every instance without a state gate, including partly initialized
     * ones, so cleanup must handle partial resources. Base destructor does not call this.
     * Output/report callbacks must be drained/joined before this owning-thread call. */
    virtual int finalize(const SimulationContext& ctx) = 0;

    // -----------------------------------------------------------------------
    // Optional: error message
    // -----------------------------------------------------------------------

    /** @brief Borrow the latest implementation error text without clearing it.
     * @return Null-terminated member/static bytes; default is a nonnull empty literal.
     * Never free. Copy before the next potentially mutating plugin call or destruction;
     * implementations define diagnostic invalidation and should return nonnull strings.
     * No base synchronization; serialize access to mutable diagnostic storage. Must not throw. */
    virtual const char* last_error_message() const noexcept { return ""; }
};

} /* namespace openswmm */

#endif /* OPENSWMM_IOUTPUT_PLUGIN_HPP */
