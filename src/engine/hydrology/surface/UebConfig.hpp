// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "UebSnow.hpp"
#include "../../core/FilePathPair.hpp"
#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace openswmm::surface {
enum class SnowKind { DegreeDay, Ueb };
struct SnowpackConfig {
    SnowKind model = SnowKind::Ueb;
    ueb::Site site;
    double initial_energy = 0;
};
struct RadiationSource {
    enum class Mode { Computed, Timeseries, File };
    Mode mode = Mode::Computed;
    std::string series;
    bool authored = false;
};
struct ClimateSite {
    double latitude = 0, longitude = 0, timezone = 0, elevation = 0;
};
struct SnowConfig {
    SnowKind model = SnowKind::DegreeDay;
    bool model_authored = false;
    bool division_temperature = false;
    double step = 3600;
    ueb::Parameters parameters;
    double bc_a = .8, bc_c = 2.4;
    // NaN/unspecified is represented explicitly: never invent a temperature range.
    std::array<std::optional<double>, 12> monthly_range;
    std::optional<ClimateSite> site;
    RadiationSource shortwave, longwave;
    FilePathPair met_file;
    std::map<int, SnowpackConfig> packs;

    SnowKind modelFor(int pack) const {
        const auto it = packs.find(pack);
        return it == packs.end() ? model : it->second.model;
    }
    bool hasTemperatureRows() const;
};

// Shared by the input reader and public option setters. A failed update leaves
// the configuration unchanged. Tokens exclude the section name.
std::string setUebOption(SnowConfig&, const std::vector<std::string>&, bool validate = true);
std::vector<std::string> uebOptionRows(const SnowConfig&);
std::optional<std::string> uebOptionValue(const SnowConfig&, const std::string& key);
std::string setSnowTemperature(SnowConfig&, const std::vector<std::string>&);
std::string setSnowpackUeb(SnowConfig&, int pack, const std::vector<std::string>&);
} // namespace openswmm::surface
