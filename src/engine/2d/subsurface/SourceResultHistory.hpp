// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <map>
#include <cstdint>
#include <string>
#include <vector>

namespace openswmm::twoD {
class CommonSourceStage;

/// Columnar diagnostics. Identifiers refer to the saved name/usage tables;
/// Frame meaning belongs to its recorder, never to the hydraulic report index.
struct SourceResultTable {
    std::vector<std::string> fields, units;
    std::vector<std::vector<double>> columns;
    std::size_t rows() const { return columns.empty() ? 0 : columns.front().size(); }
};

/// Internal R4 qualification recorder: explicitly capture the initial boundary
/// and EVERY accepted source interval. Capture failure changes neither physics
/// nor history. Receiver clocks and pending queues are endpoint snapshots.
/// Optional marcher/writer hooks remain behind the reviewed runtime gate.
/// Paired engine continuation uses EngineSourceResultHistory; production
/// streaming remains unqualified.
class SourceResultHistory {
public:
    std::string append(const CommonSourceStage&);
    /// Rebind only an exact component-checkpoint match and verified final
    /// diagnostic projection. Does not restore any hydraulic engine state.
    std::string resume(const CommonSourceStage&);
    const std::map<std::string,SourceResultTable>& tables() const { return tables_; }
    const std::vector<std::string>& sourceNames() const { return sources_; }
    const std::vector<std::string>& pollutantNames() const { return pollutants_; }
    const std::vector<std::string>& pollutantMassUnits() const { return mass_units_; }
    /// Optional water-only hydraulic-return diagnostics. Older histories and
    /// profile-off runs have no such capability; absence is not a zero series.
    bool outfallReturnsAvailable() const { return outfall_returns_; }
    /// Standalone HDF5 result artifact; atomically replaces ONLY this artifact.
    /// Never pass an existing hydraulic results/model file as the destination.
    std::string save(const std::string& path) const;
    /// An older HDF5 file without /source_history is successfully unavailable.
    /// Failure/unavailability preserve out. Append requires explicit verified resume.
    static std::string read(const std::string& path, SourceResultHistory& out, bool& available);
private:
    friend struct SourceResultFile;
    friend struct EngineSourceResultFile;
    friend class EngineSourceResultHistory;
    friend class Default2DOutputPlugin;
    friend class ExplicitInertialSolver;
    std::string refreshCheckpointMatch(const CommonSourceStage&);
    void initializeIdentity(const CommonSourceStage&);
    std::string resumeCapturedBoundary(const CommonSourceStage&);
    std::string writeIntoOpenResult(std::int64_t file) const; // engine's HDF5 instance only
    std::string extends(const SourceResultHistory& prefix) const;
    static std::map<std::string,SourceResultTable> schema(bool outfall_returns=false);
    std::string validate() const;
    const CommonSourceStage* stage_=nullptr;
    bool outfall_returns_=false;
    std::string model_, boundary_;
    std::vector<std::string> sources_,pollutants_,mass_units_;
    std::map<std::string,SourceResultTable> tables_;
};
}
