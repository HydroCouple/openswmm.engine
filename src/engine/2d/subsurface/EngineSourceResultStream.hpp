// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "SourceResultHistory.hpp"
#include "../../io/CheckpointOutputPublication.hpp"
#include <functional>
#include <memory>

namespace openswmm { class SWMMEngine; }
namespace openswmm::twoD {
/// Internal, same-platform full-history companion. Unlike qualification tables,
/// retention is independent of duration. Caller owns it through engine close.
/// A failed capture can follow accepted physics/prescriptions; inspect status
/// and settle recording before advancing. It does not roll physics back.
/// Not a public result format, security hash, or power-loss transaction.
class EngineSourceResultStream {
public:
    enum class Kind : uint32_t { Initial, SourceInterval, EngineStep, CouplingBarrier, PrescriptionUpdate, Checkpoint };
    struct Frame {
        Kind kind=Kind::Initial;
        uint64_t sequence=0, source_intervals=0, boundary_fingerprint=0;
        double engine_start=0,engine_end=0,source_start=0,source_end=0;
        std::map<std::string,SourceResultTable> tables;
    };
    struct Cursor {
        std::string identity;
        uint64_t bytes=0,frames=0,fingerprint=0;
        bool operator==(const Cursor&) const = default;
    };
    EngineSourceResultStream()=default;
    EngineSourceResultStream(const EngineSourceResultStream&)=delete;
    EngineSourceResultStream& operator=(const EngineSourceResultStream&)=delete;
    const Cursor& cursor() const { return cursor_; }
    const Frame& latest() const { return latest_; }
    const std::string& path() const { return path_; }
    bool detailed() const { return detailed_; }
    bool hasPendingCapture() const { return pending_capture_; }
    const std::string& lastError() const { return error_; }
    // Largest encoded frame, not cumulative result size. Excludes physical/output images.
    std::size_t largestFrameBytes() const { return largest_frame_; }
    /// Visits a verified saved prefix one frame at a time, without retaining it.
    static std::string visit(const std::string&,const Cursor&,const std::function<void(const Frame&)>&);
private:
    friend class openswmm::SWMMEngine;
    friend class ExplicitInertialSolver;
    struct PreparedRestore;
    std::string initialize(const openswmm::SWMMEngine&,const std::string&,bool);
    std::string capture(const openswmm::SWMMEngine&,Kind,double,uint64_t fingerprint=0);
    std::string captureSourceInterval(const CommonSourceStage&);
    void preflight() const;
    std::string checkpointContract(uint64_t physical_fingerprint,uint64_t whole_checkpoint_fingerprint) const;
    std::unique_ptr<PreparedRestore> prepareRestore(const std::string& contract,uint64_t whole_checkpoint_fingerprint) const;
    void adopt(PreparedRestore&) noexcept;
    static uint64_t fingerprint(const std::string&);
    static std::string encodeCursor(const Cursor&);
    static Cursor decodeCursor(std::string);
    void append(Frame);
    const openswmm::SWMMEngine* engine_=nullptr;
    std::string path_,metadata_,fresh_header_;
    Cursor cursor_;
    Frame latest_;
    uint64_t device_=0,inode_=0,source_intervals_=0;
    double engine_end_=0,source_end_=0;
    bool detailed_=false,pending_capture_=false;
    Kind pending_kind_=Kind::Initial;
    double pending_start_=0;
    std::string error_;
    std::size_t largest_frame_=0;
    mutable uint64_t preparation_sequence_=0;
};
struct EngineSourceResultStream::PreparedRestore {
    ~PreparedRestore();
    io::CheckpointOutputPublication::Output publication;
    Cursor cursor;
    Frame latest;
    uint64_t device=0,inode=0;
    std::size_t largest_frame=0;
    bool owns_staging=false;
};
}
