// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <map>
#include <string>
#include <vector>

namespace openswmm::twoD {
class CommonSourceStage;

/// Columnar diagnostics. Identifiers refer to the saved name/usage tables;
/// frame is an accepted source boundary, never the hydraulic report index.
struct SourceResultTable {
    std::vector<std::string> fields, units;
    std::vector<std::vector<double>> columns;
    std::size_t rows() const { return columns.empty() ? 0 : columns.front().size(); }
};

/// Internal R4 qualification recorder: explicitly capture the initial boundary
/// and EVERY accepted source interval. Capture failure changes neither physics
/// nor history. Receiver clocks and pending queues are endpoint snapshots.
/// No production scheduler/writer/GUI caller or restart concatenation yet.
class SourceResultHistory {
public:
    std::string append(const CommonSourceStage&);
    const std::map<std::string,SourceResultTable>& tables() const { return tables_; }
    const std::vector<std::string>& sourceNames() const { return sources_; }
    const std::vector<std::string>& pollutantNames() const { return pollutants_; }
    const std::vector<std::string>& pollutantMassUnits() const { return mass_units_; }
    /// Standalone HDF5 result artifact; atomically replaces ONLY this artifact.
    /// Never pass an existing hydraulic results/model file as the destination.
    std::string save(const std::string& path) const;
    /// An older HDF5 file without /source_history is successfully unavailable.
    /// Failure/unavailability preserve out. Loaded histories are read-only.
    static std::string read(const std::string& path, SourceResultHistory& out, bool& available);
private:
    friend struct SourceResultFile;
    static std::map<std::string,SourceResultTable> schema();
    std::string validate() const;
    const CommonSourceStage* stage_=nullptr;
    std::string model_;
    std::vector<std::string> sources_,pollutants_,mass_units_;
    std::map<std::string,SourceResultTable> tables_;
};
}
