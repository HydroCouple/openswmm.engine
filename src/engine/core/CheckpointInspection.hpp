// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openswmm {
/// Internal schema for a compatible fresh session's validated saved state.
/// Inspection prepares/discards output copies; it never installs or publishes.
/// Dates use the engine calendar; elapsed times and cadences use seconds.
struct CheckpointInspection {
    struct Reports {
        bool enabled=false;
        uint64_t records=0;
        double cadence_seconds=0;
        std::optional<double> first_date,last_date;
        bool operator==(const Reports&) const = default;
    };
    struct Driver {
        std::string identity;
        uint64_t next_sequence=0;
        double next_update_seconds=-1; // -1 means exhausted; equality means due now.
        bool operator==(const Driver&) const = default;
    };
    struct Companion {
        enum class Role { Source, Binary, Mesh };
        Role role=Role::Source;
        std::string path;
        uint64_t required_bytes=0; // source prefix, or complete immutable output file
        bool operator==(const Companion&) const = default;
    };
    uint32_t schema_version=1;
    double engine_seconds=0,engine_date=0,start_date=0,report_start_date=0,next_report_seconds=0;
    uint64_t checkpoint_bytes=0;
    std::optional<double> completed_source_seconds;
    std::optional<Driver> driver;
    Reports binary,mesh;
    bool file_outputs=false,relocatable_bundle=false,detailed_sources=false;
    uint64_t source_frames=0,source_prefix_bytes=0;
    std::vector<Companion> companions;
    /// Exact metadata serialization; rejects unknown schema/nonfinite/invalid UTF-8.
    std::string json() const;
    bool operator==(const CheckpointInspection&) const = default;
};
namespace detail { bool checkpointUtf8(std::string_view) noexcept; }
}
