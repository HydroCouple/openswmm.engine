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
    double simulationStartDate() const { return start_date_; } // SWMM date serial
    /// Atomically replaces ONLY a standalone companion history artifact.
    std::string save(const std::string& path) const;
    static std::string read(const std::string& path,EngineSourceResultHistory& out,bool& available);
private:
    friend class openswmm::SWMMEngine;
    friend struct EngineSourceResultFile;
    static std::map<std::string,SourceResultTable> schema(bool outfall_returns);
    static EngineSourceResultHistory projection(const openswmm::SWMMEngine&,std::size_t frame,double start);
    std::string append(const openswmm::SWMMEngine&,double start);
    std::string resume(const openswmm::SWMMEngine&);
    std::string validate() const;
    const openswmm::SWMMEngine* engine_=nullptr;
    bool outfall_returns_=false;
    double start_date_=0;
    std::string boundary_;
    std::vector<std::string> sources_,nodes_;
    std::map<std::string,SourceResultTable> tables_;
};
}
