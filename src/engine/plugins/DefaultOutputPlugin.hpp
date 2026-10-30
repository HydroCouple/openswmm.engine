// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file      DefaultOutputPlugin.hpp
 * @brief     Built-in output plugin that writes the legacy SWMM binary output format.
 *
 * @details DefaultOutputPlugin wraps the legacy `src/output/swmm_output.c` binary
 *          writer via the IOutputPlugin interface. It is automatically registered
 *          by SWMMEngine::start() when save_results != 0 and no [PLUGINS] section
 *          is present (or as an additional plugin alongside user-defined ones).
 *
 *          The binary output file is the standard SWMM 5.x .out format, readable
 *          by EPA SWMM's built-in post-processor and third-party tools.
 *
 * @see IOutputPlugin.hpp
 * @see src/output/swmm_output.c — legacy binary output writer
 * @see docs/MASTER_IMPLEMENTATION_PLAN.md Phase 4, R15
 * @ingroup engine_plugins
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_DEFAULT_OUTPUT_PLUGIN_HPP
#define OPENSWMM_ENGINE_DEFAULT_OUTPUT_PLUGIN_HPP

#include "../../../include/openswmm/plugin_sdk/IOutputPlugin.hpp"

#include "../io/CheckpointOutputPublication.hpp"
#include "../io/CheckpointFile.hpp"
#include <string>
#include <atomic>
#include <cstdio>
#include <vector>
#include <memory>

namespace openswmm {

/**
 * @brief Default output plugin: writes SWMM 5.x compatible binary .out file.
 *
 * @details This plugin is injected automatically when the engine is opened with
 *          a non-empty out_path. It does NOT require a [PLUGINS] entry.
 *
 * @ingroup engine_plugins
 */
class DefaultOutputPlugin final : public IOutputPlugin {
public:
    /** @brief Store a binary output destination and start in LOADED state.
     * @param out_path UTF-8 filename moved into storage; no file is opened/validated yet. */
    explicit DefaultOutputPlugin(std::string out_path);
    /** @brief Destroy member buffers without implicitly closing out_file_.
     * Caller must finalize() before deletion or otherwise arrange owned stream cleanup;
     * the defaulted destructor adds no FILE RAII or footer. */
    ~DefaultOutputPlugin() override = default;

    // -----------------------------------------------------------------------
    // IOutputPlugin interface
    // -----------------------------------------------------------------------

    /** @brief Read the last assigned lifecycle marker without synchronization.
     * @return Stored state; methods do not generally enforce an incoming-state transition.
     * Serialize plugin lifecycle/IO with active worker calls. */
    PluginState state() const noexcept override { return state_; }

    /** @brief Reset write counters and mark INITIALIZED.
     * @param init_args Ignored strings.
     * @param info Ignored metadata pointer.
     * @return Zero. Does not close a preexisting stream or clear all metadata/diagnostics. */
    int initialize(const std::vector<std::string>& init_args,
                   const IPluginComponentInfo* info) override;

    /** @brief Mark VALIDATED without checking path or model arrays.
     * @param ctx Ignored context.
     * @return Zero unconditionally; prepare/update caller invariants still apply. */
    int validate(const SimulationContext& ctx) override;

    /** @brief Open/truncate the binary output and write its reporting layout/header.
     * @param ctx Stable aligned model, reporting flags, species naming/units and options.
     * Static node/link lengths are internal feet and converted here; subcatch areas
     * are already acres/hectares. Managed sessions use exclusive checkpoint output
     * creation and capture its fresh header. Call once with no prior owned open stream.
     * @return Zero after marking PREPARED/counters zero; -1 for open or managed prefix
     * read failure, with last_error set. General header fwrite/setvbuf errors are not
     * checked. Exceptions can leave a partial file/open stream; no rollback is added. */
    int prepare(const SimulationContext& ctx) override;

    /** @brief Append and flush one complete report-period record, then increment counters.
     * @param snapshot Stable immutable packet with the same object counts/report flags
     * and species stride as prepare(). Values already use project display units, link
     * directions are already applied, and sim_time is a SWMM serial day. Ordinary
     * missing vector entries become zero; optional missing snow-process fields NaN.
     * No finite-value/layout validation or unit conversion is applied to period data.
     * @return -1 if no output stream, otherwise zero after marking UPDATING, writing
     * and flushing. fwrite/fflush errors are not tested, so counters can advance despite
     * IO failure. Call serially, normally on IOThread, without lifecycle/checkpoint races. */
    int update(const SimulationSnapshot& snapshot) override;

    /** @brief Write a footer with current counters, close the output and mark FINALIZED.
     * @param ctx Ignored; footer error code is always zero rather than context error.
     * @return Zero including when no stream exists. Native long offsets narrow to int
     * without overflow checks; writer/file-size limits must remain valid. fwrite/close
     * errors are ignored. Caller first drains all outstanding update calls. */
    int finalize(const SimulationContext& ctx) override;

    /** @brief Borrow the stored diagnostic text without clearing it.
     * @return NUL-terminated member bytes, possibly empty/stale; valid until the next
     * error mutation or destruction. Never free; use the method return code for success. */
    const char* last_error_message() const noexcept override { return last_error_.c_str(); }

private:
    friend class SWMMEngine;
    struct PreparedCheckpoint;
    bool checkpoint_fresh_ = false;
    std::string checkpoint_header_;
    /** @brief Capture a validated in-memory report-prefix archive.
     * @param ctx Compatible model/options for exact report-date/count validation.
     * @param next_report_ms Finite next report-grid time in milliseconds, at least one step.
     * @return Version-one owned archive of period count and prefix (at most 64 MiB).
     * Requires fresh managed writer and matching step/period counters. Validation/IO/
     * allocation failures throw without publishing a replacement or installing solver state. */
    std::string checkpointBytes(const SimulationContext& ctx, double next_report_ms) const;
    /** @brief Validate an in-memory archive and stage an exclusive open replacement stream.
     * @param ctx Compatible model/options.
     * @param next_report_ms Expected next report-grid clock in milliseconds.
     * @param bytes Owned version-one checkpoint archive consumed by value.
     * @return Shared cleanup owner with validated prefix/periods written and flushed
     * to a unique .report-part-N sibling, still open at its end. Requires a managed
     * fresh writer with zero counters and unchanged fresh header. Tries up to thirty-two
     * exclusive names; archive/validation/IO/allocation failures throw before publication.
     * Normal staged cleanup does not remove other preexisting paths. */
    std::shared_ptr<PreparedCheckpoint> prepareCheckpoint(const SimulationContext& ctx,
                                                          double next_report_ms, std::string bytes);
    /** @brief Publish one validated staged output, then adopt its already open stream.
     * @param[in,out] prepared Exclusive prepared owner, consumed on successful adoption.
     * Publication exceptions occur before adoption; rollback follows CheckpointOutputPublication.
     * This rename is not a power-loss transaction or atomic external-reader view.
     * Requires a live fresh original writer and no concurrent output/path mutation. */
    void installCheckpoint(PreparedCheckpoint& prepared);
    /** @brief Describe a staged output replacement without performing its renames.
     * @param prepared Borrowed validated staging owner whose stream remains open.
     * @return Value containing staging/destination, fresh header and borrowed original
     * writer descriptor. Owner must keep paths/stream valid through publication;
     * publication helper validates identity/rollback rules. */
    io::CheckpointOutputPublication::Output
    checkpointPublication(const PreparedCheckpoint& prepared) const;
    /** @brief Finish nonthrowing stream/counter ownership transfer after successful publication.
     * @param[in,out] prepared Valid published staging owner with open stream and period count.
     * Closes the old writer (status ignored), transfers/clears the prepared stream/path,
     * retains any companion cleanup owner, restores counters and PREPARED/UPDATING state.
     * Must be called only after publication; no validation/rollback is performed here. */
    void adoptCheckpoint(PreparedCheckpoint& prepared) noexcept;
    /** @brief Check a complete in-memory prefix against its fresh writer and report clock.
     * @param ctx Compatible options/start/report-start dates and positive report step.
     * @param next_report_ms Exact finite next grid time in milliseconds.
     * @param periods Nonnegative expected completed report count.
     * @param bytes Prefix starting with the identical captured header, at most 64 MiB.
     * @throws std::runtime_error Writer readiness, shape, count/grid/date or value mismatch.
     * Each record date must exactly match the engine convention; ordinary float values
     * must be finite, optional snow-process columns may be NaN. No file mutation occurs. */
    void validateCheckpoint(const SimulationContext& ctx, double next_report_ms, int periods,
                            const std::string& bytes) const;
    /** @brief Flush and copy the current binary stream prefix, restoring its cursor.
     * @return Owned bytes from zero to current ftell, limited to 64 MiB.
     * @throws std::runtime_error Closed/errored/flush/size/seek/read/restore failure.
     * Requires exclusive stream access; allocation failures propagate. Failure after a
     * seek can leave cursor/error state changed; this is not a const-IO synchronization guarantee. */
    std::string readPrefix() const;
    /** @brief Copy and validate the current report stream as a private file companion.
     * @param ctx Compatible report options.
     * @param next Expected next report-grid milliseconds.
     * @param base Destination base for exclusively created private companion paths.
     * @param[out] artifact Shared cleanup owner assigned after successful validation/encoding;
     * prior caller value is retained on earlier failure.
     * @return Version-one metadata archive containing period count and file descriptor
     * contract, not embedded report bytes. Requires settled writer/counters. Flush,
     * copy, fingerprint, exact format/date/value validation or archive errors throw.
     * The active writer continues on its original file; caller serializes source mutation. */
    std::string captureFileCheckpoint(const SimulationContext& ctx, double next,
                                      const std::filesystem::path& base,
                                      std::shared_ptr<io::OwnedCheckpointFile>& artifact) const;
    /** @brief Verify a file companion, copy it beside the fresh destination and keep it open.
     * @param ctx Compatible model/report options.
     * @param next Expected next report-grid milliseconds.
     * @param bytes Owned version-one companion metadata archive.
     * @return Shared staged owner positioned at the validated report end for appending.
     * Requires a fresh managed writer with zero counters/header unchanged. Decodes and
     * verifies source fingerprint, checks copied fingerprint, validates exact stream
     * shape/dates/finiteness and throws on failure before publication. Shared companion
     * ownership removes abandoned staging unless retained after successful adoption. */
    std::shared_ptr<PreparedCheckpoint> prepareFileCheckpoint(const SimulationContext& ctx,
                                                              double next, std::string bytes);
    /** @brief Validate an entire checkpoint stream using bounded scratch instead of a memory prefix.
     * @param file Required borrowed readable seekable stream; rewound then consumed
     * through EOF, not closed or restored to its initial cursor.
     * @param bytes Exact declared stream length.
     * @param ctx Compatible options/start/report-start and positive report step.
     * @param next Exact next report-grid milliseconds.
     * @param periods Nonnegative expected report count.
     * @throws std::runtime_error Managed readiness, size/header/grid/count/date/value/EOF mismatch.
     * Checks 16,384 floats per buffer; ordinary values finite, optional snow fields may
     * be NaN. Does not publish/adopt output; caller owns stream synchronization/cleanup. */
    void validateFileCheckpoint(FILE* file, uint64_t bytes, const SimulationContext& ctx,
                                double next, int periods) const;

    std::string out_path_;
    PluginState state_ = PluginState::UNLOADED;
    std::string last_error_;
    int step_count_ = 0;

    FILE* out_file_ = nullptr;
    int n_subcatch_ = 0;
    int n_nodes_ = 0;
    int n_links_ = 0;
    int n_polluts_ = 0;
    int n_subcatch_vars_ = 0;
    bool snow_process_ = false;
    int n_node_vars_ = 0;
    int n_link_vars_ = 0;
    int flow_units_code_ = 0;
    long id_start_pos_ = 0;
    long input_start_pos_ = 0;
    long output_start_pos_ = 0;
    int n_periods_ = 0;

    // Static-object header is written from the SimulationContext (internal
    // units), so it still needs this length factor. Subcatchment area is
    // already held in display units and needs none. Per-timestep results
    // arrive pre-converted by the engine boundary and need no plugin-side
    // conversion.
    double ucf_length_ = 1.0;

    // Per-object report flags, captured during prepare() from SimulationContext
    std::vector<char> subcatch_rpt_flag_;
    std::vector<char> node_rpt_flag_;
    std::vector<char> link_rpt_flag_;

    static constexpr int MAGIC_NUMBER = 516114522;
    static constexpr int VERSION = 60000; // SWMM 6.0.000
    static constexpr int MAX_SYS_RESULTS = 15;

    /** @brief Write native binary counts, IDs, units, static attributes, codes and report metadata.
     * @param ctx Stable aligned model arrays and reported-species mapping. Captures
     * report flags and writes only flagged objects, with optional UEB snow process
     * columns and ignore_quality handling. Requires an open empty writable stream and
     * prepared unit factors; updates file offsets/counts. fwrite/ftell errors are not checked. */
    void writeHeader(const SimulationContext& ctx);
    /** @brief Write one native-int byte length followed by identifier bytes without a terminator.
     * @param id Required NUL-terminated string; length must fit int. Requires an open
     * writable stream. Neither fwrite result nor string encoding is validated. */
    void writeID(const char* id);
    /** @brief Append one native-representation integer to the owned stream.
     * @param value Value copied as sizeof(int) bytes, assumed four by the binary format.
     * Requires open stream; fwrite status is discarded and no endian conversion occurs. */
    void writeInt4(int value);
    /** @brief Append one native float to the owned stream.
     * @param value Four-byte IEEE-format value assumed by the file format, including
     * nonfinite bits unchanged. Requires open stream; fwrite status is discarded. */
    void writeReal4(float value);
    /** @brief Append one native double to the owned stream.
     * @param value Eight-byte IEEE-format value assumed by the file format. Requires
     * open stream; fwrite status is discarded and no endian conversion occurs. */
    void writeReal8(double value);
};

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_DEFAULT_OUTPUT_PLUGIN_HPP */
