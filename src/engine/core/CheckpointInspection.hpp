// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin

/**
 * @file      CheckpointInspection.hpp
 * @brief     Validated checkpoint schemas used to bind compatible fresh sessions.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

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
        bool enabled = false;
        uint64_t records = 0;
        double cadence_seconds = 0;
        std::optional<double> first_date, last_date;
        /**
         * @brief Compare all Reports fields using default member equality.
         *
         * @param[in] other Borrowed metadata value to compare; no filesystem, schema or compatibility
         *     validation.
         * @return True when every corresponding member compares equal; optional/vector/string comparisons
         *     follow their value semantics.
         */
        bool operator==(const Reports& other) const = default;
    };
    struct Driver {
        std::string identity;
        uint64_t next_sequence = 0;
        double next_update_seconds = -1; // -1 means exhausted; equality means due now.
        /**
         * @brief Compare all Driver fields using default member equality.
         *
         * @param[in] other Borrowed metadata value to compare; no filesystem, schema or compatibility
         *     validation.
         * @return True when every corresponding member compares equal; optional/vector/string comparisons
         *     follow their value semantics.
         */
        bool operator==(const Driver& other) const = default;
    };
    struct Companion {
        enum class Role { Source, Binary, Mesh };
        Role role = Role::Source;
        std::string path;
        uint64_t required_bytes = 0; // source prefix, or complete immutable output file
        /**
         * @brief Compare all Companion fields using default member equality.
         *
         * @param[in] other Borrowed metadata value to compare; no filesystem, schema or compatibility
         *     validation.
         * @return True when every corresponding member compares equal; optional/vector/string comparisons
         *     follow their value semantics.
         */
        bool operator==(const Companion& other) const = default;
    };
    uint32_t schema_version = 1;
    double engine_seconds = 0, engine_date = 0, start_date = 0, report_start_date = 0,
           next_report_seconds = 0;
    uint64_t checkpoint_bytes = 0;
    std::optional<double> completed_source_seconds;
    std::optional<Driver> driver;
    Reports binary, mesh;
    bool file_outputs = false, relocatable_bundle = false, detailed_sources = false;
    uint64_t source_frames = 0, source_prefix_bytes = 0;
    std::vector<Companion> companions;
    /**
     * @brief Serialize exact checkpoint inspection metadata as compact schema-one JSON.
     *
     * Serializes engine calendar dates and elapsed/cadence seconds as documented by fields, without
     * output installation or file access. Rejects unsupported schema, nonfinite floating fields,
     * invalid UTF-8 and invalid companion role with std::logic_error. Does not independently prove
     * checkpoint/model compatibility or nonnegative semantic ranges; allocation can throw.
     * @return Owned UTF-8 JSON; uint64 counts are decimal strings to preserve full integer precision.
     */
    std::string json() const;
    /**
     * @brief Compare all CheckpointInspection fields using default member equality.
     *
     * @param[in] other Borrowed metadata value to compare; no filesystem, schema or compatibility
     *     validation.
     * @return True when every corresponding member compares equal; optional/vector/string comparisons
     *     follow their value semantics.
     */
    bool operator==(const CheckpointInspection& other) const = default;
};
namespace detail {
/**
 * @brief Validate a complete byte sequence as UTF-8 scalar encodings.
 *
 * Does not reject ASCII control characters or impose a normalization policy; JSON quoting escapes
 * controls separately. Allocation-free.
 *
 * @param[in] text Borrowed bytes, possibly empty or containing ASCII NUL/control characters.
 * @return True for complete shortest-form Unicode scalar encodings; false for
 *     truncated/malformed/overlong sequences, surrogates or values >U+10FFFF.
 */
bool checkpointUtf8(std::string_view text) noexcept;
}
}
