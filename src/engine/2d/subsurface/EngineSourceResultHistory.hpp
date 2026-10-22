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
    static EngineSourceResultHistory projection(const openswmm::SWMMEngine&, std::size_t frame,
                                                double start);
    std::string append(const openswmm::SWMMEngine&, double start);
    std::string resume(const openswmm::SWMMEngine&);
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
