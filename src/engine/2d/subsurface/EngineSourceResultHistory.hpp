// SPDX-License-Identifier: Apache-2.0

/**
 * @file      EngineSourceResultHistory.hpp
 * @brief     Water-driver history associated with source-interval results.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include "SourceResultHistory.hpp"

namespace openswmm {
class SWMMEngine;
}
namespace openswmm::twoD {
/// Internal water-only companion to source-interval results. Frame zero and
/// every accepted ENGINE step capture cumulative water and pending queues.
/// Source clocks may repeat. No hydraulic output append or production streaming.
/// Caller owns the recorder and keeps it alive until engine end/close.
class EngineSourceResultHistory {
public:
    /**
     * @brief Borrow this recorder's columnar engine-boundary tables with field/unit schemas.
     * @return Const reference owned by the recorder. Valid until destruction or replacement;
     * append, resume, read publication or assignment may invalidate its elements. Requires
     * stable recorder access, performs no copy/allocation and provides no synchronization.
     */
    const std::map<std::string, SourceResultTable>& tables() const { return tables_; }
    /**
     * @brief Borrow this recorder's saved source identifier names.
     * @return Const reference owned by the recorder. Valid until destruction or replacement;
     * append, resume, read publication or assignment may invalidate its elements. Requires
     * stable recorder access, performs no copy/allocation and provides no synchronization.
     */
    const std::vector<std::string>& sourceNames() const { return sources_; }
    /**
     * @brief Borrow this recorder's saved node identifier names.
     * @return Const reference owned by the recorder. Valid until destruction or replacement;
     * append, resume, read publication or assignment may invalidate its elements. Requires
     * stable recorder access, performs no copy/allocation and provides no synchronization.
     */
    const std::vector<std::string>& nodeNames() const { return nodes_; }
    /**
     * @brief Test availability of water-only hydraulic-return snapshots.
     * @return True when saved outfall credit/queue fields are present; false means unavailable,
     * not a recorded zero series. Does not inspect the current engine or change recorder state.
     */
    bool outfallReturnsAvailable() const { return outfall_returns_; }
    /**
     * @brief Test whether this history includes immutable fine source-interval snapshots.
     * @return True when the paired source history capability is configured. Its time/frame axis
     * is separate from accepted engine steps; false denotes unavailable detailed data.
     */
    bool detailedSourceIntervalsAvailable() const { return detailed_; }
    // Immutable source-interval snapshots, on their own frame/time axis.
    // Resume through the containing engine history, never a component restore.
    /**
     * @brief Borrow the fine source-interval companion on its own accepted-source time axis.
     * @return Pointer to the recorder-owned immutable companion when detailed capability is
     * present, otherwise null. Recorder replacement/destruction invalidates the pointer.
     * Continue through the containing engine history, never through a standalone component
     * restore; no copy or allocation.
     */
    const SourceResultHistory* sourceIntervals() const {
        return detailed_ ? &source_intervals_ : nullptr;
    }
    /**
     * @brief Read the saved absolute simulation start serial.
     * @return SWMM date serial in days since 1899-12-30; zero in a default empty recorder. This
     * is separate from the elapsed-second start/end clocks in the result tables. No mutation
     * occurs.
     */
    double simulationStartDate() const { return start_date_; } // SWMM date serial
    /**
     * @brief Atomically replaces ONLY a standalone companion history artifact.
     *
     * @param[in] path Standalone destination with an existing parent directory. An existing file
     * must have the exact engine source-history artifact kind; hydraulic/model/component-history
     * files are refused rather than replaced.
     * @return Empty after validated exclusive sibling write, flush/close and rename; diagnostic
     * on validation, I/O, unsupported build or caught standard exception. A temporary creation
     * collision is never removed. Later temporary cleanup is best effort; rename supplies no
     * power-loss guarantee.
     * @note Uses the engine HDF5 mutex. Same-destination writers need caller coordination; saved
     * data includes the whole-engine checkpoint anchor and optional paired fine source
     * intervals.
     */
    std::string save(const std::string& path) const;
    /**
     * @brief Read optional engine source history into a privately validated candidate.
     * @param[in] path Nonempty existing HDF5 file, at most 256 MiB.
     * @param[out] out Replaced only after schema versions 1..4, capability metadata, bounded
     * columns, water/delivery receipts and optional paired intervals validate. Unchanged on
     * error/unavailability.
     * @param[out] available Set false immediately and true only after successful out
     * replacement.
     * @return Empty for valid data or absent engine_source_history; diagnostic on malformed
     * data, I/O, unsupported build or caught standard exception. Absent capability is not a zero
     * series.
     * @note Uses the engine HDF5 mutex. A read recorder needs an exact whole-engine resume
     * before continued capture; no hydraulic state is restored by reading diagnostics.
     */
    static std::string read(const std::string& path, EngineSourceResultHistory& out,
                            bool& available);

private:
    friend class openswmm::SWMMEngine;
    friend struct EngineSourceResultFile;
    friend class ExplicitInertialSolver;
    friend class EngineSourceResultStream;
    static std::map<std::string, SourceResultTable> schema(bool outfall_returns,
                                                           bool lid_beds = false);
    /**
     * @brief Construct one engine-boundary water/queue projection with stable saved identities.
     * @param[in] e Prepared accepted-boundary engine with a live common source stage and
     * consistent source delivery bridge or enabled LID bottom receipts. All borrowed subsystem
     * arrays must remain stable and internally sized during this synchronous read.
     * @param[in] frame Exactly representable nonnegative result frame identifier.
     * @param[in] start Beginning of this accepted engine interval in elapsed seconds; end uses ctx
     * time.
     * @return Owned one-frame projection of cumulative source water, groups, node delivery, pending
     * queues and optional LID/porous diagnostics. Does not advance clocks or consume any queue.
     * @throws std::runtime_error On receipt/schema failure or pending ordinal above 2^53-1.
     * Allocation errors propagate. Low-level helper does not validate complete model checkpoint
     * eligibility.
     * @note Converts 1D feet/ft3 quantities to SI and preserves source clocks independently of
     * engine steps; repeated source clocks are valid. No whole-engine checkpoint bytes/live pointer
     * are bound until the surrounding append/resume operation publishes the candidate.
     */
    static EngineSourceResultHistory projection(const openswmm::SWMMEngine& e, std::size_t frame,
                                                double start);
    /**
     * @brief Capture initial state or the next accepted engine-step water boundary transactionally.
     * @param[in] e Stable checkpoint-capable engine with a managed source bridge or enabled
     * LID bottom attachment. After frame zero its exact address/identities/capabilities/start date
     * must match.
     * @param[in] start Accepted interval start elapsed s, exactly the previous saved engine end.
     * Initial capture requires start/current time zero; subsequent engine time must be strictly
     * greater.
     * @return Empty after checkpoint capture, boundary projection and complete candidate validation;
     * diagnostic on refusal or caught standard exception, with history/physics unchanged.
     * @note Copies existing history before appending and offsets pending-row identifiers globally.
     * Detailed mode seeds source frame zero on first capture; intermediate source intervals are
     * captured through captureSourceInterval(). The engine remains borrowed and must outlive
     * recorder use. Checkpoint capture can settle associated output metadata; this method does not
     * advance physics.
     */
    std::string append(const openswmm::SWMMEngine& e, double start);
    /**
     * @brief Rebind a saved recorder only to an exact restored whole-engine checkpoint and
     * projection.
     * @param[in] e Stable restored checkpoint-capable engine with matching
     * managed-source/LID ownership. Its address is retained only after complete validation and must
     * outlive future recorder use.
     * @return Empty after exact checkpoint bytes, identifiers/capabilities/start date and final
     * table projection agree; otherwise diagnostic with this recorder and engine unchanged. Caught
     * standard exceptions become diagnostics.
     * @note Detailed companion history is privately checked against its earlier captured component
     * boundary and rebound to this engine's common stage. No hydraulic/component state is installed;
     * whole-engine restoration must already have completed. Future captures append after the saved
     * end.
     */
    std::string resume(const openswmm::SWMMEngine& e);
    std::string validate() const;
    std::string captureSourceInterval(const CommonSourceStage&);
    std::string validatePairing() const;
    const openswmm::SWMMEngine* engine_ = nullptr;
    bool outfall_returns_ = false;
    bool lid_beds_ = false;
    bool detailed_ = false;
    SourceResultHistory source_intervals_;
    double start_date_ = 0;
    std::string boundary_;
    std::vector<std::string> sources_, nodes_;
    std::map<std::string, SourceResultTable> tables_;
};
}
