// SPDX-License-Identifier: Apache-2.0
/**
 * @file      RestartStore.hpp
 * @brief     Restart records kept in a GeoPackage or HDF5 file, behind one interface.
 *
 * A record is the bytes of a native hot start file captured at an accepted step, filed under an
 * immutable run ID and record ID. Both containers store the same bytes, so a record written to
 * one restores exactly like a record written to the other.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace openswmm {
struct SimulationContext;
}

namespace openswmm::restart {

/** @brief What a file holds, judged from its first bytes. Values match SWMM_RESTART_BACKEND_*. */
enum class Backend { NONE = 0, HOTSTART = 1, GEOPACKAGE = 2, HDF5 = 3, OTHER = 4 };

/**
 * @brief Classify @p path from its first bytes without changing it.
 * @param[in] path UTF-8 path.
 * @return NONE for a missing or empty file, OTHER for a directory or unknown content.
 */
Backend probe(const std::string& path) noexcept;

/** @brief Upper-case name of a backend, as used in JSON and messages. */
const char* backendName(Backend backend) noexcept;

/** @brief One run of a restart file. Empty strings are absent values. */
struct RunInfo {
    std::string run_id;
    std::string created_at;     ///< UTC wall clock, ISO 8601.
    std::string engine_version; ///< Engine that ran it.
    std::string parent_run_id;  ///< Run whose record this run started from.
    std::string parent_record_id;
    std::string restart_mode;   ///< INITIAL_CONDITIONS or CONTINUE when it started from a record.
    std::string restart_source; ///< File that record was read from.
};

/** @brief One restart record. */
struct RecordInfo {
    std::string record_id;           ///< rec_NNNNNN, unique within its run.
    int ordinal = 0;                 ///< 1, 2, ... in capture order.
    std::string created_at;          ///< UTC wall clock, ISO 8601.
    double model_datetime = 0.0;     ///< Decimal days of the accepted step captured.
    double elapsed_seconds = 0.0;    ///< Simulated seconds since the run's start.
    double requested_datetime = 0.0; ///< Decimal days asked for; 0 for END.
    int hsf_version = 0;             ///< Native hot start version of the bytes.
    bool continuation = false;       ///< The bytes carry an engine checkpoint.
    bool binary_output = false;      ///< That checkpoint carries the binary results written so far.
    std::uint64_t byte_count = 0;
    std::string sha256;   ///< Digest of the bytes, lowercase hexadecimal.
    std::string platform; ///< platformTag() of the writer.
    std::string engine_version;
    std::string status; ///< "complete" when the record can be restored.
};

/** @brief Runs with their records, in creation order. */
struct Catalog {
    Backend backend = Backend::NONE;
    std::vector<std::pair<RunInfo, std::vector<RecordInfo>>> runs;
};

/**
 * @brief A GeoPackage or HDF5 restart file opened for reading or for one run's recording.
 *
 * Every operation returns an empty string on success and the reason otherwise.
 */
class Store {
public:
    virtual ~Store() = default;

    /** @brief List the file's runs and records. */
    virtual std::string catalog(Catalog& out) = 0;

    /**
     * @brief Read one record's bytes and check their length and digest.
     * @param[out] bytes Receives the record's native hot start bytes.
     */
    virtual std::string read(const std::string& run_id, const std::string& record_id,
                             std::string& bytes) = 0;

    /**
     * @brief Register the recording run, with a model snapshot where the container keeps one.
     * @param[in] ctx Model being run; read only.
     */
    virtual std::string beginRun(const SimulationContext& ctx, const RunInfo& run) = 0;

    /** @brief Store one record of the run; it is complete once this returns success. */
    virtual std::string append(const std::string& run_id, const RecordInfo& record,
                               const std::string& bytes) = 0;

    /** @brief Mark the run finished. */
    virtual std::string finishRun(const std::string& run_id) = 0;
};

/**
 * @brief Open a restart file.
 * @param[in] path UTF-8 path.
 * @param[in] backend GEOPACKAGE or HDF5; the caller has checked the content or chosen the format.
 * @param[in] write False opens read-only and never creates or changes the file. True creates a
 *     missing file and refuses an HDF5 file that holds other content.
 * @param[out] error Reason when the result is null, for example a backend this build lacks.
 */
std::unique_ptr<Store> openStore(const std::string& path, Backend backend, bool write,
                                 std::string& error);

/** @brief Describe @p catalog of @p path as the JSON swmm_restart_inspect_json() returns. */
std::string catalogJson(const std::string& path, const Catalog& catalog);

/**
 * @brief Operating system, processor and byte order of this build, such as darwin-arm64-le.
 *
 * Native hot start bytes use the writer's machine representation, so a record restores only where
 * this tag matches.
 */
std::string platformTag();

/** @brief Current UTC wall clock as ISO 8601, such as 2026-11-19T12:00:00Z. */
std::string utcNow();

} // namespace openswmm::restart
