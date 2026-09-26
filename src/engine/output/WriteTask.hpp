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
 * @file WriteTask.hpp
 * @brief Ring-buffer task descriptor for the IO thread (Phase 5, R17).
 *
 * @details A WriteTask packages a SimulationSnapshot for delivery to the
 *          IOThread write queue. The snapshot is moved (or deep-copied) from
 *          the live SimulationContext when an output boundary is reached.
 *
 *          Because the snapshot owns its own vectors, the main simulation
 *          thread is free to continue advancing immediately after posting —
 *          no mutex is needed on the simulation state.
 *
 * @see IOThread.hpp
 * @see include/openswmm/plugin_sdk/SimulationSnapshot.hpp
 * @ingroup engine_io
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_WRITE_TASK_HPP
#define OPENSWMM_ENGINE_WRITE_TASK_HPP

#include "../../../include/openswmm/plugin_sdk/SimulationSnapshot.hpp"

namespace openswmm {

/**
 * @brief One item in the IOThread write queue.
 *
 * @details Moved into the queue from the simulation thread; consumed by the
 *          IO thread. Ownership of all vector data is transferred with the
 *          move, so no copies are made for typical usage.
 *
 * @ingroup engine_io
 */
struct WriteTask {
    /** @brief The snapshot to be written (deep copy of relevant SoA slices). */
    SimulationSnapshot snapshot;

    /** @brief Sequential task index (0-based). Useful for ordering assertions. */
    int sequence = 0;

    WriteTask() = default;

    explicit WriteTask(SimulationSnapshot snap, int seq = 0)
        : snapshot(std::move(snap)), sequence(seq) {}

    // Movable; not copyable (snapshots can be large)
    WriteTask(WriteTask&&) noexcept = default;
    WriteTask& operator=(WriteTask&&) noexcept = default;
    WriteTask(const WriteTask&) = delete;
    WriteTask& operator=(const WriteTask&) = delete;
};

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_WRITE_TASK_HPP */
