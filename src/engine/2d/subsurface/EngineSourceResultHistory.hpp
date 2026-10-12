// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "SourceResultHistory.hpp"

namespace openswmm { class SWMMEngine; }
namespace openswmm::twoD {
/// Internal water-only companion to source-interval results. Frame zero and
/// every accepted ENGINE step capture cumulative water and pending queues.
/// Source clocks may repeat. No hydraulic output append or production streaming.
/// Caller owns the recorder and keeps it alive until engine end/close.
class EngineSourceResultHistory {
public:
    const std::map<std::string,SourceResultTable>& tables() const { return tables_; }
    const std::vector<std::string>& sourceNames() const { return sources_; }
    const std::vector<std::string>& nodeNames() const { return nodes_; }
    bool outfallReturnsAvailable() const { return outfall_returns_; }
    bool detailedSourceIntervalsAvailable() const { return detailed_; }
    // Immutable source-interval snapshots, on their own frame/time axis.
    // Resume through the containing engine history, never a component restore.
    const SourceResultHistory* sourceIntervals() const { return detailed_?&source_intervals_:nullptr; }
    double simulationStartDate() const { return start_date_; } // SWMM date serial
    /// Atomically replaces ONLY a standalone companion history artifact.
    std::string save(const std::string& path) const;
    static std::string read(const std::string& path,EngineSourceResultHistory& out,bool& available);
private:
    friend class openswmm::SWMMEngine;
    friend struct EngineSourceResultFile;
    friend class ExplicitInertialSolver;
    friend class EngineSourceResultStream;
    static std::map<std::string,SourceResultTable> schema(bool outfall_returns);
    static EngineSourceResultHistory projection(const openswmm::SWMMEngine&,std::size_t frame,double start);
    std::string append(const openswmm::SWMMEngine&,double start);
    std::string resume(const openswmm::SWMMEngine&);
    std::string validate() const;
    std::string captureSourceInterval(const CommonSourceStage&);
    std::string validatePairing() const;
    const openswmm::SWMMEngine* engine_=nullptr;
    bool outfall_returns_=false;
    bool detailed_=false;
    SourceResultHistory source_intervals_;
    double start_date_=0;
    std::string boundary_;
    std::vector<std::string> sources_,nodes_;
    std::map<std::string,SourceResultTable> tables_;
};
}
