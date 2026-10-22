// SPDX-License-Identifier: Apache-2.0

/**
 * @file      EngineSourceResultStream.hpp
 * @brief     Versioned binary companion streams for complete source result histories.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include "SourceResultHistory.hpp"
#include "../../io/CheckpointOutputPublication.hpp"
#include <functional>
#include <memory>

namespace openswmm {
class SWMMEngine;
}
namespace openswmm::twoD {
/// Internal, same-platform full-history companion. Unlike qualification tables,
/// retention is independent of duration. Caller owns it through engine close.
/// A failed capture can follow accepted physics/prescriptions; inspect status
/// and settle recording before advancing. It does not roll physics back.
/// Not a public result format, security hash, or power-loss transaction.
class EngineSourceResultStream {
public:
    enum class Kind : uint32_t {
        Initial,
        SourceInterval,
        EngineStep,
        CouplingBarrier,
        PrescriptionUpdate,
        Checkpoint
    };
    struct Frame {
        Kind kind = Kind::Initial;
        uint64_t sequence = 0, source_intervals = 0, boundary_fingerprint = 0;
        double engine_start = 0, engine_end = 0, source_start = 0, source_end = 0;
        std::map<std::string, SourceResultTable> tables;
    };
    struct Cursor {
        std::string identity;
        uint64_t bytes = 0, frames = 0, fingerprint = 0;
        /**
         * @brief Compare all saved-prefix cursor fields exactly.
         * @return True when identity, byte count, frame count and rolling fingerprint are equal.
         * The unnamed argument is the other stable Cursor. This comparison performs no file
         * verification.
         */
        bool operator==(const Cursor&) const = default;
    };
    /**
     * @brief Construct an unattached recorder with empty path/cursor and zero accepted clocks.
     * @note No file is opened or engine registered. The owning engine initializes it at its
     * initial boundary and retains the recorder through engine close; copying is intentionally
     * disabled.
     */
    EngineSourceResultStream() = default;
    EngineSourceResultStream(const EngineSourceResultStream&) = delete;
    EngineSourceResultStream& operator=(const EngineSourceResultStream&) = delete;
    /**
     * @brief Borrow the recorder's committed prefix identity, byte/frame counts and rolling
     * fingerprint.
     * @return Const recorder-owned reference; invalidated by recorder replacement/destruction
     * and possibly subsequent capture/adoption. No copy, file verification or synchronization
     * occurs. Keep the recorder stable while reading. lastError text is cleared by successful
     * engine capture/ adoption; a successful fine source capture alone does not clear a
     * previously stored error.
     */
    const Cursor& cursor() const { return cursor_; }
    /**
     * @brief Borrow the recorder's most recently committed frame.
     * @return Const recorder-owned reference; invalidated by recorder replacement/destruction
     * and possibly subsequent capture/adoption. No copy, file verification or synchronization
     * occurs. Keep the recorder stable while reading. lastError text is cleared by successful
     * engine capture/ adoption; a successful fine source capture alone does not clear a
     * previously stored error.
     */
    const Frame& latest() const { return latest_; }
    /**
     * @brief Borrow the recorder's attached companion filesystem path.
     * @return Const recorder-owned reference; invalidated by recorder replacement/destruction
     * and possibly subsequent capture/adoption. No copy, file verification or synchronization
     * occurs. Keep the recorder stable while reading. lastError text is cleared by successful
     * engine capture/ adoption; a successful fine source capture alone does not clear a
     * previously stored error.
     */
    const std::string& path() const { return path_; }
    /**
     * @brief Test whether fine source intervals are included on their independent time axis.
     * @return Stored profile flag, fixed by initialization. Does not validate pending capture or
     * file prefix state; false includes the default unattached recorder.
     */
    bool detailed() const { return detailed_; }
    /**
     * @brief Read whether an engine-level capture remains pending after its physical anchor was
     * obtained.
     * @return Stored pending_capture_ flag. A failed early engine-anchor check or fine
     * source-interval capture need not set it, so false alone does not prove successful/healthy
     * recording; inspect returned status and lastError(). Successful engine capture or restore
     * adoption clears it.
     */
    bool hasPendingCapture() const { return pending_capture_; }
    /**
     * @brief Borrow the recorder's last stored capture error text.
     * @return Const recorder-owned reference; invalidated by recorder replacement/destruction
     * and possibly subsequent capture/adoption. No copy, file verification or synchronization
     * occurs. Keep the recorder stable while reading. lastError text is cleared by successful
     * engine capture/ adoption; a successful fine source capture alone does not clear a
     * previously stored error.
     *
     * @note Successful engine capture/adopt clears this text. A successful fine source-interval
     * capture does not clear an earlier stored error. Use the immediate operation's returned
     * status together with hasPendingCapture() rather than treating empty text alone as recorder
     * health.
     */
    const std::string& lastError() const { return error_; }
    // Largest encoded frame, not cumulative result size. Excludes physical/output images.
    /**
     * @brief Read the largest committed encoded frame payload size seen by this recorder.
     * @return Largest encoded frame payload seen by this recorder, in bytes, initially zero. It
     * is not cumulative file size and excludes framing/header bytes and external physical/output
     * images. No scan, copy or allocation occurs.
     */
    std::size_t largestFrameBytes() const { return largest_frame_; }
    /**
     * @brief Visits a verified saved prefix one frame at a time, without retaining it.
     *
     * @par Arguments in order Stable saved companion path, expected committed Cursor, and an
     * optional synchronous read-only frame callback. Callback references live only during the
     * visit and must not be retained; an empty callback still verifies the full prefix. The file
     * must not be replaced/modified during scanning.
     * @return Empty after full identity/frame-chain/schema/clock/cursor verification; caught
     * standard exception text on file, validation or callback failure. Earlier callback effects
     * remain even if a later frame/final-cursor check fails. Extra file bytes beyond
     * cursor.bytes are ignored.
     * @note Reads/retains one bounded frame at a time, not complete result history. No file
     * mutation, engine restoration, synchronization or cryptographic authenticity guarantee is
     * provided.
     */
    static std::string visit(const std::string&, const Cursor&,
                             const std::function<void(const Frame&)>&);

private:
    friend class openswmm::SWMMEngine;
    friend class ExplicitInertialSolver;
    struct PreparedRestore;
    /**
     * @brief Exclusively create the companion and attach it to a prepared engine at time zero.
     * @par Arguments in order Borrowed live engine, new companion path with existing parent
     * directory, and detailed-interval flag. Engine must have a common source stage and managed
     * delivery or LID bottom receipts, and outlive the recorder through close. Existing
     * destination files are never truncated.
     * @return Empty after synchronized model header/initial frame and guard capture; standard
     * exception diagnostic otherwise. This call's created file is removed best-effort on
     * failure. No physics is advanced; source captures and engine/barrier captures share one
     * committed prefix.
     */
    std::string initialize(const openswmm::SWMMEngine&, const std::string&, bool);
    /**
     * @brief Append one accepted engine-step or zero-time barrier frame with a physical anchor.
     * @param[in] fingerprint Optional physical-checkpoint fingerprint; zero computes it from the
     * attached engine. It is the anchor argument at the implementation.
     * @par Leading unnamed arguments Exact borrowed attached engine, capture Kind, and elapsed
     * engine interval start seconds. EngineStep advances engine end; barriers hold it fixed and
     * suppress replay of prior routing receipts.
     * @return Empty after synchronized append and pending/error reset; diagnostic otherwise.
     * Once the physical anchor exists, failure leaves capture pending. Accepted physics is not
     * rolled back; caller settles recording before advancing and serializes engine/stream/file
     * access.
     */
    std::string capture(const openswmm::SWMMEngine&, Kind, double, uint64_t fingerprint = 0);
    std::string captureSourceInterval(const CommonSourceStage&);
    void preflight() const;
    std::string checkpointContract(uint64_t physical_fingerprint,
                                   uint64_t whole_checkpoint_fingerprint) const;
    std::unique_ptr<PreparedRestore> prepareRestore(const std::string& contract,
                                                    uint64_t whole_checkpoint_fingerprint) const;
    void adopt(PreparedRestore&) noexcept;
    static uint64_t fingerprint(const std::string&);
    static std::string encodeCursor(const Cursor&);
    static Cursor decodeCursor(std::string);
    void append(Frame);
    const openswmm::SWMMEngine* engine_ = nullptr;
    std::string path_, metadata_, fresh_header_;
    Cursor cursor_;
    Frame latest_;
    uint64_t device_ = 0, inode_ = 0, source_intervals_ = 0;
    double engine_end_ = 0, source_end_ = 0;
    bool detailed_ = false, pending_capture_ = false;
    Kind pending_kind_ = Kind::Initial;
    double pending_start_ = 0;
    std::string error_;
    std::size_t largest_frame_ = 0;
    mutable uint64_t preparation_sequence_ = 0;
};
struct EngineSourceResultStream::PreparedRestore {
    ~PreparedRestore();
    io::CheckpointOutputPublication::Output publication;
    Cursor cursor;
    Frame latest;
    uint64_t device = 0, inode = 0;
    std::size_t largest_frame = 0;
    bool owns_staging = false;
};
}
