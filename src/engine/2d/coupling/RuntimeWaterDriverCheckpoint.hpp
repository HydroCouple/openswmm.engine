// SPDX-License-Identifier: Apache-2.0

/**
 * @file      RuntimeWaterDriverCheckpoint.hpp
 * @brief     Synchronous restart contract for internal water-driver participants.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace openswmm::twoD {
/// Internal, synchronous water-driver restart participant. The caller retains
/// this object through engine close. No public plugin ABI or remote recovery.
class RuntimeWaterDriverCheckpoint {
public:
    struct Snapshot {
        uint64_t next_sequence = 0;
        double next_update_seconds = -1; // -1 means exhausted; otherwise elapsed seconds
        std::string state;               // driver schema, cursor, pending updates and mutable state
    };
    class PreparedRestore {
    public:
        /**
         * @brief Destroy a prepared adoption plan without changing any live driver or engine
         * state.
         * @note Derived plans own all staged private resources. Abandoning a plan before
         * install() must release them without installing state, performing engine updates or
         * advancing the driver.
         */
        virtual ~PreparedRestore() = default;
        /**
         * @brief Only nonthrowing in-memory adoption; no IO, callbacks or engine updates.
         *
         * @details Adopt the exactly prepared snapshot through nonthrowing in-memory state
         * changes.
         * @note Called only after the containing restore has accepted every participant.
         * Implementations must perform no I/O, callbacks or engine updates, and must not throw.
         * All allocation/validation belongs to prepareRestore(); the owning caller keeps the
         * driver and plan alive for this call. Requires exclusive driver access; this interface
         * provides no synchronization.
         */
        virtual void install() noexcept = 0;
    };
    /**
     * @brief Destroy the synchronous driver participant through its base interface.
     * @note The caller retains the participant until engine close, including all capture/restore
     * callbacks. No implicit engine deregistration or close is performed by this default
     * destructor.
     */
    virtual ~RuntimeWaterDriverCheckpoint() = default;
    /**
     * @brief Exact immutable software/schema/input/mapping identity, matching the common stage's
     * replay identity. Include every driver affecting this run.
     *
     * @details Return the exact immutable identity of every driver affecting this run.
     * @return Owned identity string covering software/schema, inputs and mapping. It must match
     * the common source stage replay identity and remain stable across capture and restoration.
     * @note This identifies compatibility, not mutable cursor state. Implementations may
     * allocate; callers keep the participant alive and stable for the synchronous call.
     */
    virtual std::string identity() const = 0;
    /**
     * @brief Report the next scheduled update on the engine's elapsed-second axis.
     * @return Next nonnegative elapsed simulation time s, or -1 when the update schedule is
     * exhausted, consistent with Snapshot::next_update_seconds and the captured driver cursor.
     * @note This read-only query must not advance the driver and must not throw. No
     * calendar-date conversion or schedule execution belongs here.
     */
    virtual double nextUpdateSeconds() const noexcept = 0;
    /**
     * @brief Called between accepted steps. Must not mutate or advance the driver.
     *
     * @details Copy a synchronous driver snapshot at an accepted engine boundary.
     * @param[in] completed_seconds Accepted elapsed simulation time s at which the driver is
     * quiescent.
     * @return Owned snapshot containing next sequence number, next update seconds (-1 if
     * exhausted) and a complete serialized mutable driver state including schema, cursor and
     * pending updates.
     * @note Called between accepted engine steps. Must neither mutate nor advance the driver or
     * engine; implementations may reject unsupported external state by exception. No pointer to
     * the returned state may depend on transient callback storage. Identity is carried
     * separately by identity().
     */
    virtual Snapshot capture(double completed_seconds) const = 0;
    /**
     * @brief Validate the entire snapshot, cursor and pending schedule on private state, without
     * changing the live driver or engine. Reject unsupported external state. A valid plan must
     * adopt exactly the supplied snapshot; destroying an unused plan must leave all live state
     * unchanged.
     *
     * @details Validate a complete saved driver state and stage its private in-memory
     * replacement.
     * @param[in] completed_seconds Restored accepted elapsed simulation boundary s, against
     * which saved cursor and pending schedule must be checked. The first unnamed argument is the
     * immutable snapshot containing exact next sequence/update metadata and the encoded driver
     * state.
     * @return Owned PreparedRestore plan whose install() adopts exactly that snapshot, with no
     * live mutation during preparation or unused-plan destruction. An implementation must reject
     * invalid snapshots or unsupported external state; null is not a usable adoption plan.
     * @note Validate software/schema/input/mapping compatibility, the full cursor and pending
     * schedule before returning. Preparation may allocate or throw; it must not call engine
     * updates or advance the live driver. The participant must remain alive until the plan is
     * installed or abandoned.
     */
    virtual std::unique_ptr<PreparedRestore> prepareRestore(const Snapshot&,
                                                            double completed_seconds) const = 0;
};
}
