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
 * @file      OutputReader.hpp
 * @brief     Internal C++ class for reading SWMM 5.x binary output files.
 *
 * @details Reads the binary .out format produced by DefaultOutputPlugin.
 *          This is an internal implementation class — the public API is
 *          the C FFI in openswmm_output.h.
 *
 *          Binary layout:
 *            [Header]        magic, version, flow_units, counts (7×int32)
 *            [ID Section]    subcatch/node/link names (len-prefixed strings)
 *            [Input Section] static properties (subcatch areas, node/link geometry)
 *            [Variable Codes] per-type variable code arrays + report metadata
 *            [Output Section] per-period: date(8B) + subcatch + node + link + system results
 *            [Footer]        id_start, input_start, output_start, n_periods, error_code, magic
 *
 * @see DefaultOutputPlugin.cpp for the writer
 * @ingroup engine_output
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_OUTPUT_READER_HPP
#define OPENSWMM_ENGINE_OUTPUT_READER_HPP

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

/**
 * @defgroup engine_output Saved-result access
 * @ingroup new_engine
 *
 * Read-only saved-result access and output serialization. Reader objects own file
 * resources and expose format-specific time/index/attribute contracts; one-dimensional
 * project display units and two-dimensional native SI outputs remain distinct.
 */

namespace openswmm {

/**
 * @brief Reads SWMM 5.x binary .out files produced by DefaultOutputPlugin.
 * Reads use a shared mutable FILE cursor even on const methods: serialize all
 * operations per reader, including refresh/close. Returns raw stored units;
 * no endian/float-representation conversion or general corrupt-file hardening
 * is provided. File counts/offsets must fit native allocation/long arithmetic.
 * @ingroup engine_output
 */
class OutputReader {
public:
    /** @brief Construct an unopened reader with zero metadata and empty identifier arrays. */
    OutputReader() = default;
    /** @brief Close the file and destroy all reader-owned ID and code buffers. */
    ~OutputReader();

    /** @brief Disable copying of the owned FILE cursor and buffers. */
    OutputReader(const OutputReader&) = delete;
    /** @brief Disable assignment of the owned FILE cursor and buffers. */
    OutputReader& operator=(const OutputReader&) = delete;

    /**
     * @brief Open and parse the header/footer of a binary output file.
     * @returns true on success, false on error (bad magic, I/O failure, etc.).
     */
    /** @param path NUL-terminated UTF-8 filename, read-only. Null returns false while
     * preserving an existing open reader; a nonnull path closes it before attempting
     * the new file. Requires the expected SWMM binary layout and complete footer.
     * Failure closes any newly opened file but retains some parsed scalar metadata;
     * allocation exceptions propagate. A footer simulation error code does not itself
     * make open fail. Caller serializes file access and checks is_open() before ID reads. */
    bool open(const char* path);

    /**
     * @brief Open a file that is still being written (no footer yet).
     *
     * @details Every offset the footer would supply is derived by parsing
     *          the header forward, and the period count is derived from the
     *          file size: `floor((size - output_start) / bytes_per_period)`,
     *          so a partially flushed last record is never counted. Call
     *          refresh() to pick up periods appended since. Works on any
     *          file with a complete header — including one from a run that
     *          died before writing its footer. Never writes to the file.
     * @returns true on success (possibly with zero periods yet).
     */
    /** @param path NUL-terminated UTF-8 filename with a complete expected header/input/
     * variable section. Null preserves an existing reader; nonnull closes it first.
     * Initial refresh errors can leave the count zero while openLive still succeeds.
     * This method neither waits for a header nor writes/locks the growing source file.
     * Caller coordinates writer visibility and serializes this reader's cursor access. */
    bool openLive(const char* path);

    /**
     * @brief Re-count periods on a live file.
     *
     * @details Re-reads the file size and updates period_count(). When the
     *          footer has appeared (the run finished) it is adopted — period
     *          count and error code become the writer's own — and the reader
     *          leaves live mode; subsequent calls are no-ops. On a reader
     *          opened with open() this returns period_count() unchanged.
     * @returns the current period count, or -1 if no file is open.
     */
    /** @return Current whole-record count, -1 with no open file. Seek/size failures
     * on a live file retain the previous count rather than reporting -1. Truncation
     * can reduce the count; no monotonic append check is enforced here. A structurally
     * matching footer supplies authoritative count/error and ends live mode. */
    int refresh();

    /** @brief True while opened via openLive() and the footer has not yet
     *  been seen. */
    /** @return Stored live-mode flag; false initially, after close or once refresh accepts a footer. */
    bool is_live() const noexcept { return live_; }

    /**
     * @brief Close the file and release resources.
     */
    /** @details Safe repeatedly. Clears file/ID buffers and resets period count/live
     * flag; other scalar counts/metadata and subcatch code storage can remain stale.
     * Borrowed ID pointers become invalid. Never call concurrently with a read. */
    void close();

    /** @brief Whether the reader has a file open and valid. */
    /** @return True exactly when file_ is nonnull. Does not revalidate external file contents. */
    bool is_open() const noexcept { return file_ != nullptr; }

    // -- Metadata ---------------------------------------------------------

    /** @brief Read retained parsed metadata without performing IO.
     * @return Raw engine/version integer from the last parsed header. Except period_count(), these scalar values can remain
     * stale after close(); inspect is_open() before treating them as current. */
    int version() const noexcept { return version_; }
    /** @brief Read retained parsed metadata without performing IO.
     * @return Raw SWMM flow-unit enum from the last parsed header; no unit conversion. Except period_count(), these scalar values can remain
     * stale after close(); inspect is_open() before treating them as current. */
    int flow_units() const noexcept { return flow_units_; }
    /** @brief Read retained parsed metadata without performing IO.
     * @return Subcatchment count from the last parsed header. Except period_count(), these scalar values can remain
     * stale after close(); inspect is_open() before treating them as current. */
    int subcatch_count() const noexcept { return n_subcatch_; }
    /** @brief Read retained parsed metadata without performing IO.
     * @return Node count from the last parsed header. Except period_count(), these scalar values can remain
     * stale after close(); inspect is_open() before treating them as current. */
    int node_count() const noexcept { return n_nodes_; }
    /** @brief Read retained parsed metadata without performing IO.
     * @return Link count from the last parsed header. Except period_count(), these scalar values can remain
     * stale after close(); inspect is_open() before treating them as current. */
    int link_count() const noexcept { return n_links_; }
    /** @brief Read retained parsed metadata without performing IO.
     * @return Pollutant/species-column count from the last parsed header. Except period_count(), these scalar values can remain
     * stale after close(); inspect is_open() before treating them as current. */
    int pollut_count() const noexcept { return n_polluts_; }
    /** @brief Read retained parsed metadata without performing IO.
     * @return Current retained period count; refresh() updates live files and close() resets it. Except period_count(), these scalar values can remain
     * stale after close(); inspect is_open() before treating them as current. */
    int period_count() const noexcept { return n_periods_; }
    /** @brief Read retained parsed metadata without performing IO.
     * @return Raw SWMM serial day (day zero 1899-12-30), including fractional days. Except period_count(), these scalar values can remain
     * stale after close(); inspect is_open() before treating them as current. */
    double start_date() const noexcept { return start_date_; }
    /** @brief Read retained parsed metadata without performing IO.
     * @return Stored report interval in seconds; not the routing timestep. Except period_count(), these scalar values can remain
     * stale after close(); inspect is_open() before treating them as current. */
    int report_step() const noexcept { return report_step_; }
    /** @brief Read retained parsed metadata without performing IO.
     * @return Simulation error recorded by the footer; zero while initially live. Except period_count(), these scalar values can remain
     * stale after close(); inspect is_open() before treating them as current. */
    int error_code() const noexcept { return error_code_; }

    /** @brief Test presence of a stored subcatchment variable code.
     * @param code Nonnegative code, not a physical column ordinal.
     * @return True when the code appears in the retained code list; does not require an
     * open file, so the answer can reflect the last parsed file after close(). */
    bool has_subcatch_variable(int code) const {
        const int i = subcatchColumn(code);
        return i >= 0 && i < n_subcatch_vars_;
    }
    /** @brief Read retained parsed metadata without performing IO.
     * @return Stored physical subcatch column count, which can exceed the legacy attribute prefix. Except period_count(), these scalar values can remain
     * stale after close(); inspect is_open() before treating them as current. */
    int subcatch_var_count() const noexcept { return n_subcatch_vars_; }
    /** @brief Read retained parsed metadata without performing IO.
     * @return Stored node-column count, including pollutant/species columns. Except period_count(), these scalar values can remain
     * stale after close(); inspect is_open() before treating them as current. */
    int node_var_count() const noexcept { return n_node_vars_; }
    /** @brief Read retained parsed metadata without performing IO.
     * @return Stored link-column count, including pollutant/species columns. Except period_count(), these scalar values can remain
     * stale after close(); inspect is_open() before treating them as current. */
    int link_var_count() const noexcept { return n_link_vars_; }
    /** @brief Read retained parsed metadata without performing IO.
     * @return Stored system result-column count. Except period_count(), these scalar values can remain
     * stale after close(); inspect is_open() before treating them as current. */
    int system_var_count() const noexcept { return n_system_vars_; }

    // -- Object IDs -------------------------------------------------------

    /** @brief Borrow one subcatch identifier stored by the file.
     * @param index Zero-based subcatch index. Requires a successfully open reader; close() clears names while retaining object counts.
     * @return Reader-owned NUL-terminated bytes, or null for a rejected index.
     * Never free; lifetime ends at close(), reopen or destruction. */
    const char* subcatch_id(int index) const;
    /** @brief Borrow one node identifier stored by the file.
     * @param index Zero-based node index. Requires a successfully open reader; close() clears names while retaining object counts.
     * @return Reader-owned NUL-terminated bytes, or null for a rejected index.
     * Never free; lifetime ends at close(), reopen or destruction. */
    const char* node_id(int index) const;
    /** @brief Borrow one link identifier stored by the file.
     * @param index Zero-based link index. Requires a successfully open reader; close() clears names while retaining object counts.
     * @return Reader-owned NUL-terminated bytes, or null for a rejected index.
     * Never free; lifetime ends at close(), reopen or destruction. */
    const char* link_id(int index) const;

    /**
     * @brief Species (pollutant) ID at a column index, or nullptr.
     *
     * @details The writer has always emitted a fourth ID list after the
     *          link IDs — the species names, one per pollutant column —
     *          but the reader stopped after three and no public entry
     *          point exposed them. Water age reports as a trailing
     *          `__WATER_AGE__` column whose unit field necessarily reuses
     *          a concentration code (the `.out` unit enum has no HOURS
     *          slot), so the NAME is the only way a consumer can tell
     *          hours from mg/L. That made this reader a prerequisite for
     *          water age being usable at all, not a convenience.
     */
    /** @brief Borrow one pollut identifier stored by the file.
     * @param index Zero-based pollut index. Bounds use the actual stored name array.
     * @return Reader-owned NUL-terminated bytes, or null for a rejected index.
     * Never free; lifetime ends at close(), reopen or destruction. */
    const char* pollut_id(int index) const;
    /** @brief Read a stored pollutant concentration-unit code.
     * @param index Zero-based pollutant column.
     * @return Raw file code, or -1 outside the stored unit array. A water-age sentinel
     * name still denotes hours even though its unit slot reuses a concentration enum. */
    int pollut_units(int index) const;

    // -- Per-period results (all objects, one variable) --------------------

    /** @brief Read one subcatch variable at one report period.
     * @param period Zero-based period index.
     * @param var Semantic subcatch code resolved through the file code list, not a column ordinal.
     * @param[out] values Required writable array of at least subcatch_count() floats, in object order.
     * @return True after all reads, false for closed/null/range/seek/read failure.
     * Stored output units are returned unchanged; failed reads can leave a written prefix.
     * All operations on this reader's shared FILE cursor must be externally serialized. */
    bool get_subcatch_result(int period, int var, float* values) const;

    /** @brief Read one node variable for all nodes at a period. */
    /** @brief Read one node variable at one report period.
     * @param period Zero-based period index.
     * @param var Zero-based physical variable-column ordinal in the stored block.
     * @param[out] values Required writable array of at least node_count() floats, in object order.
     * @return True after all reads, false for closed/null/range/seek/read failure.
     * Stored output units are returned unchanged; failed reads can leave a written prefix.
     * All operations on this reader's shared FILE cursor must be externally serialized. */
    bool get_node_result(int period, int var, float* values) const;

    /** @brief Read one link variable for all links at a period. */
    /** @brief Read one link variable at one report period.
     * @param period Zero-based period index.
     * @param var Zero-based physical variable-column ordinal in the stored block.
     * @param[out] values Required writable array of at least link_count() floats, in object order.
     * @return True after all reads, false for closed/null/range/seek/read failure.
     * Stored output units are returned unchanged; failed reads can leave a written prefix.
     * All operations on this reader's shared FILE cursor must be externally serialized. */
    bool get_link_result(int period, int var, float* values) const;

    /** @brief Read one system variable at a period. */
    /** @brief Read one system variable at one report period.
     * @param period Zero-based period index.
     * @param var Zero-based physical variable-column ordinal in the stored block.
     * @param[out] value Required writable float.
     * @return True after all reads, false for closed/null/range/seek/read failure.
     * Stored output units are returned unchanged; failed reads can leave a written prefix.
     * All operations on this reader's shared FILE cursor must be externally serialized. */
    bool get_system_result(int period, int var, float* value) const;

    /** @brief Read all node/link variables at one period with one contiguous read.
     * Values are object-major: values[object * var_count + variable].
     * Storage is reused across calls; no history or live-file data is cached. */
    /** @brief Read the contiguous object-major node result block.
     * @param period Zero-based in-range report period.
     * @param[out] values Resized to node_count()*node_var_count(); allocation
     * errors propagate. A zero-object block succeeds with an empty destination.
     * @return True on complete read; false for invalid layout/range/IO, potentially
     * after resize/partial overwrite. Stored units/order are unchanged. */
    bool get_node_block(int period, std::vector<float>& values) const;
    /** @brief Read the contiguous object-major link result block.
     * @param period Zero-based in-range report period.
     * @param[out] values Resized to link_count()*link_var_count(); allocation
     * errors propagate. A zero-object block succeeds with an empty destination.
     * @return True on complete read; false for invalid layout/range/IO, potentially
     * after resize/partial overwrite. Stored units/order are unchanged. */
    bool get_link_block(int period, std::vector<float>& values) const;

    // -- Time series (one object, one variable, period range) --------------

    /** @brief Read an inclusive report-period series for one subcatch variable.
     * @param obj_idx Zero-based in-range object index.
     * @param var Semantic subcatch code resolved through the file code list, not a column ordinal.
     * @param start_period Inclusive first period, at least zero.
     * @param end_period Inclusive final period, >=start_period and <period_count().
     * @param[out] values Required array of at least end_period-start_period+1 floats;
     * first output corresponds to start_period.
     * @return True on complete read, false for closed/null/range/seek/read failure;
     * a read failure can leave a written prefix. Raw stored units and values are retained. */
    bool get_subcatch_series(int obj_idx, int var, int start_period, int end_period,
                             float* values) const;

    /** @brief Read an inclusive report-period series for one node variable.
     * @param obj_idx Zero-based in-range object index.
     * @param var Zero-based physical variable-column ordinal in the stored block.
     * @param start_period Inclusive first period, at least zero.
     * @param end_period Inclusive final period, >=start_period and <period_count().
     * @param[out] values Required array of at least end_period-start_period+1 floats;
     * first output corresponds to start_period.
     * @return True on complete read, false for closed/null/range/seek/read failure;
     * a read failure can leave a written prefix. Raw stored units and values are retained. */
    bool get_node_series(int obj_idx, int var, int start_period, int end_period,
                         float* values) const;

    /** @brief Read an inclusive report-period series for one link variable.
     * @param obj_idx Zero-based in-range object index.
     * @param var Zero-based physical variable-column ordinal in the stored block.
     * @param start_period Inclusive first period, at least zero.
     * @param end_period Inclusive final period, >=start_period and <period_count().
     * @param[out] values Required array of at least end_period-start_period+1 floats;
     * first output corresponds to start_period.
     * @return True on complete read, false for closed/null/range/seek/read failure;
     * a read failure can leave a written prefix. Raw stored units and values are retained. */
    bool get_link_series(int obj_idx, int var, int start_period, int end_period,
                         float* values) const;

    /** @brief Read an inclusive report-period series for one system variable.
     * @param var Zero-based physical variable-column ordinal in the stored block.
     * @param start_period Inclusive first period, at least zero.
     * @param end_period Inclusive final period, >=start_period and <period_count().
     * @param[out] values Required array of at least end_period-start_period+1 floats;
     * first output corresponds to start_period.
     * @return True on complete read, false for closed/null/range/seek/read failure;
     * a read failure can leave a written prefix. Raw stored units and values are retained. */
    bool get_system_series(int var, int start_period, int end_period, float* values) const;

    // -- Per-object all-variables at one period ----------------------------

    /** @brief All variables for one subcatchment at one period. */
    /** @brief Read one subcatch object's stored result columns for a report period.
     * @param obj_idx Zero-based in-range object index.
     * @param period Zero-based in-range period.
     * @param[out] values Required array of at least min(subcatch_var_count(),8+pollut_count()) floats.
     * @param[out] count Optional number-written destination, set to min(subcatch_var_count(),8+pollut_count()) only on success.
     * @return True after all reads, false for closed/null/range/seek/read failure;
     * values can contain a partial prefix on failure while count remains unchanged.
     * Optional process columns beyond the legacy 8+pollutant prefix require code-based APIs. */
    bool get_subcatch_attribute(int obj_idx, int period, float* values, int* count) const;

    /** @brief All variables for one node at one period. */
    /** @brief Read one node object's stored result columns for a report period.
     * @param obj_idx Zero-based in-range object index.
     * @param period Zero-based in-range period.
     * @param[out] values Required array of at least node_var_count() floats.
     * @param[out] count Optional number-written destination, set to node_var_count() only on success.
     * @return True after all reads, false for closed/null/range/seek/read failure;
     * values can contain a partial prefix on failure while count remains unchanged. */
    bool get_node_attribute(int obj_idx, int period, float* values, int* count) const;

    /** @brief All variables for one link at one period. */
    /** @brief Read one link object's stored result columns for a report period.
     * @param obj_idx Zero-based in-range object index.
     * @param period Zero-based in-range period.
     * @param[out] values Required array of at least link_var_count() floats.
     * @param[out] count Optional number-written destination, set to link_var_count() only on success.
     * @return True after all reads, false for closed/null/range/seek/read failure;
     * values can contain a partial prefix on failure while count remains unchanged. */
    bool get_link_attribute(int obj_idx, int period, float* values, int* count) const;

    // -- Period time -------------------------------------------------------

    /** @brief Get the simulation time for a given period. */
    /** @param period Zero-based index in [0,period_count()).
     * @param[out] time Required writable double receiving the raw SWMM serial day
     * value (day zero 1899-12-30). No timezone conversion is applied.
     * @return True after a complete read, false for closed/null/range/seek/read failure.
     * A failed low-level fread can leave a partially changed destination. */
    bool get_period_time(int period, double* time) const;

    // -- Per-node summary statistics — Slice QA-01 -------------------------
    //
    // Computed on-demand from the per-period node results stored in the
    // file. See openswmm_output.h for unit conventions + the report-step
    // vs. routing-step precision caveat. Each method is O(n_periods).
    /** @brief Compute a node summary from its complete stored report-period series.
     * @param node_idx Zero-based in-range node.
     * @param[out] value Required destination. Maximum stored report-period depth in the file's length unit.
     * @return True on success (zero for no periods), false for closed/null/index/read
     * failure. Destination changes only after the series was read. Allocation errors
     * propagate. Report-step sampling can differ from routing-step engine statistics;
     * no resampling or time interpolation is performed. */
    bool get_node_stat_max_depth(int node_idx, double* value) const;
    /** @brief Compute a node summary from its complete stored report-period series.
     * @param node_idx Zero-based in-range node.
     * @param[out] value Required destination. Maximum stored report-period overflow in the file's flow unit.
     * @return True on success (zero for no periods), false for closed/null/index/read
     * failure. Destination changes only after the series was read. Allocation errors
     * propagate. Report-step sampling can differ from routing-step engine statistics;
     * no resampling or time interpolation is performed. */
    bool get_node_stat_max_overflow(int node_idx, double* value) const;
    /** @brief Compute a node summary from its complete stored report-period series.
     * @param node_idx Zero-based in-range node.
     * @param[out] value Required destination. Sum of positive report-period overflow times report_step seconds, in
     * raw file-flow-unit times seconds (for example L for LPS, ft3 for CFS; GPM
     * remains gallons/minute times seconds, without minute conversion).
     * @return True on success (zero for no periods), false for closed/null/index/read
     * failure. Destination changes only after the series was read. Allocation errors
     * propagate. Report-step sampling can differ from routing-step engine statistics;
     * no resampling or time interpolation is performed. */
    bool get_node_stat_vol_flooded(int node_idx, double* value) const;
    /** @brief Compute a node summary from its complete stored report-period series.
     * @param node_idx Zero-based in-range node.
     * @param[out] value Required destination. Number of positive-overflow periods times report_step, in seconds.
     * @return True on success (zero for no periods), false for closed/null/index/read
     * failure. Destination changes only after the series was read. Allocation errors
     * propagate. Report-step sampling can differ from routing-step engine statistics;
     * no resampling or time interpolation is performed. */
    bool get_node_stat_time_flooded(int node_idx, double* value) const;

private:
    static constexpr int32_t MAGIC_NUMBER = 516114522;

    FILE* file_ = nullptr;

    // Header fields
    int version_ = 0;
    int flow_units_ = 0;
    int n_subcatch_ = 0;
    int n_nodes_ = 0;
    int n_links_ = 0;
    int n_polluts_ = 0;

    // Variable counts (read from file)
    int n_subcatch_vars_ = 0;
    std::vector<int> subcatch_codes_;
    /** @brief Resolve a subcatchment variable code to its first stored column.
     * @param code Nonnegative semantic code; negative values are rejected.
     * @return First column ordinal, or -1 when absent. Does not access the file cursor. */
    int subcatchColumn(int code) const;
    int n_node_vars_ = 0;
    int n_link_vars_ = 0;
    int n_system_vars_ = 0;

    // Report metadata
    double start_date_ = 0.0;
    int report_step_ = 0;

    // Footer fields
    long id_start_pos_ = 0;
    long input_start_pos_ = 0;
    long output_start_pos_ = 0;
    int n_periods_ = 0;
    int error_code_ = 0;

    // Computed: bytes per period record
    long bytes_per_period_ = 0;

    // openLive(): period count comes from the file size until the footer lands
    bool live_ = false;

    // Object IDs
    std::vector<std::string> subcatch_ids_;
    std::vector<std::string> node_ids_;
    std::vector<std::string> link_ids_;
    std::vector<std::string> pollut_ids_;
    std::vector<int> pollut_units_;

    // -- Helpers ----------------------------------------------------------

    /** @brief Read one contiguous object-major period block into reusable vector storage.
     * @param period Zero-based valid period.
     * @param objectType Block selector, 0=subcatch, 1=node, 2=link or 3=system.
     * @param count Nonnegative object count matching the chosen block.
     * @param variables Positive stored column count matching that block.
     * @param[out] values Resized to count*variables after max_size overflow check.
     * @return True for an empty valid block or a complete contiguous read; false for
     * validation/seek/short read. Destination can be resized/partially overwritten.
     * Caller supplies coherent selector/count/layout; allocation errors propagate. */
    bool readResultBlock(int period, int objectType, int count, int variables,
                         std::vector<float>& values) const;
    /** @brief Parse the final six native-endian int32 fields and validate closing magic.
     * @return False for seek/read/magic failure; true after copying offsets, periods
     * and simulation error into metadata. Requires file_ open. Does not validate
     * nonnegative counts, offset ordering or consistency with file size. */
    bool readFooter();
    /** @brief Seek to zero, check opening magic and read version/units/object counts.
     * @return True after complete native-endian reads; false for seek/read/magic failure.
     * Requires file_ open. Counts and version compatibility are not deeply validated. */
    bool readHeader();
    /** @brief Read owned subcatch/node/link/pollutant IDs and pollutant unit codes.
     * @return True on complete read, false for seek/read or ID-length failure (0..1024
     * bytes per ID). Requires file_ open and meaningful parsed counts/id offset.
     * Allocation errors propagate; false can leave partially replaced buffers. Names
     * are retained as raw bytes, without UTF-8 or uniqueness validation. */
    bool readIDs();
    /** @brief Skip the fixed SWMM input-property layout and parse result-column metadata.
     * @return True after storing subcatch codes, all variable counts, start date and
     * report step. False for seek/read or subcatch-count bounds failure; allocation
     * errors propagate and metadata can be partially updated. Requires an open file
     * and valid object counts/input offset. Assumes one subcatch, three node and five
     * link input properties; does not generically interpret arbitrary property codes.
     * Only subcatch variable count is bounded to [0,1,000,000]; other counts are trusted. */
    bool readVariableCodes();

    /** @brief Read one native-representation four-byte integer from the shared FILE cursor.
     * @param[out] value Destination; a short fread can change some object bytes.
     * @return True for one complete object, false otherwise. Requires file_ open
     * and exclusive cursor access; no endian or format conversion occurs. */
    bool readInt4(int32_t& value) const;
    /** @brief Read one native-representation four-byte float from the shared FILE cursor.
     * @param[out] value Destination; a short fread can change some object bytes.
     * @return True for one complete object, false otherwise. Requires file_ open
     * and exclusive cursor access; no endian or format conversion occurs. */
    bool readReal4(float& value) const;
    /** @brief Read one native-representation eight-byte float from the shared FILE cursor.
     * @param[out] value Destination; a short fread can change some object bytes.
     * @return True for one complete object, false otherwise. Requires file_ open
     * and exclusive cursor access; no endian or format conversion occurs. */
    bool readReal8(double& value) const;

    /** @brief Compute the file offset of a period record start. */
    /** @param period Logical period ordinal; not range-checked here.
     * @return output_start_pos + period*bytes_per_period in native long. Caller ensures
     * representable arithmetic and a valid parsed layout before seeking. */
    long periodOffset(int period) const;

    /** @brief Seek to a specific variable within a period for a given object type.
     *  @param period   Period index.
     *  @param obj_type 0=subcatch, 1=node, 2=link, 3=system.
     *  @param obj_idx  Object index (ignored for system).
     *  @param var      Variable index within the object's result block.
     *  @returns true on success. */
    /** @details Requires open file and caller-validated indices/layout; this helper only
     * rejects an unknown object type and a failed fseek. No endian conversion occurs. */
    bool seekToVar(int period, int obj_type, int obj_idx, int var) const;
};

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_OUTPUT_READER_HPP */
