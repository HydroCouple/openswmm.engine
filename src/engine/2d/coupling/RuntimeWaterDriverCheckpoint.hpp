// SPDX-License-Identifier: Apache-2.0
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
        std::string state; // driver schema, cursor, pending updates and mutable state
    };
    class PreparedRestore {
    public:
        virtual ~PreparedRestore() = default;
        /// Only nonthrowing in-memory adoption; no IO, callbacks or engine updates.
        virtual void install() noexcept = 0;
    };
    virtual ~RuntimeWaterDriverCheckpoint() = default;
    /// Exact immutable software/schema/input/mapping identity, matching the
    /// common stage's replay identity. Include every driver affecting this run.
    virtual std::string identity() const = 0;
    virtual double nextUpdateSeconds() const noexcept = 0;
    /// Called between accepted steps. Must not mutate or advance the driver.
    virtual Snapshot capture(double completed_seconds) const = 0;
    /// Validate the entire snapshot, cursor and pending schedule on private
    /// state, without changing the live driver or engine. Reject unsupported
    /// external state. A valid plan must adopt exactly the supplied snapshot;
    /// destroying an unused plan must leave all live state unchanged.
    virtual std::unique_ptr<PreparedRestore> prepareRestore(const Snapshot&,
        double completed_seconds) const = 0;
};
}
