/**
 * @file OutputReader.hpp
 * @brief Internal C++ class for reading SWMM 5.x binary output files.
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
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_OUTPUT_READER_HPP
#define OPENSWMM_ENGINE_OUTPUT_READER_HPP

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

namespace openswmm {

/**
 * @brief Reads SWMM 5.x binary .out files produced by DefaultOutputPlugin.
 * @ingroup engine_output
 */
class OutputReader {
public:
    OutputReader() = default;
    ~OutputReader();

    OutputReader(const OutputReader&) = delete;
    OutputReader& operator=(const OutputReader&) = delete;

    /**
     * @brief Open and parse the header/footer of a binary output file.
     * @param path  File path to the .out file.
     * @returns true on success, false on error (bad magic, I/O failure, etc.).
     */
    bool open(const char* path);

    /**
     * @brief Close the file and release resources.
     */
    void close();

    /** @brief Whether the reader has a file open and valid. */
    bool is_open() const noexcept { return file_ != nullptr; }

    // -- Metadata ---------------------------------------------------------

    int version()       const noexcept { return version_; }
    int flow_units()    const noexcept { return flow_units_; }
    int subcatch_count() const noexcept { return n_subcatch_; }
    int node_count()    const noexcept { return n_nodes_; }
    int link_count()    const noexcept { return n_links_; }
    int pollut_count()  const noexcept { return n_polluts_; }
    int period_count()  const noexcept { return n_periods_; }
    double start_date() const noexcept { return start_date_; }
    int report_step()   const noexcept { return report_step_; }
    int error_code()    const noexcept { return error_code_; }

    int subcatch_var_count() const noexcept { return n_subcatch_vars_; }
    int node_var_count()     const noexcept { return n_node_vars_; }
    int link_var_count()     const noexcept { return n_link_vars_; }
    int system_var_count()   const noexcept { return n_system_vars_; }

    // -- Object IDs -------------------------------------------------------

    const char* subcatch_id(int index) const;
    const char* node_id(int index)     const;
    const char* link_id(int index)     const;

    // -- Per-period results (all objects, one variable) --------------------

    /** @brief Read one subcatch variable for all subcatchments at a period.
     *  @param period  Zero-based period index.
     *  @param var     Variable index within the subcatch result block.
     *  @param values  Caller-allocated array of subcatch_count floats.
     *  @returns true on success. */
    bool get_subcatch_result(int period, int var, float* values) const;

    /** @brief Read one node variable for all nodes at a period. */
    bool get_node_result(int period, int var, float* values) const;

    /** @brief Read one link variable for all links at a period. */
    bool get_link_result(int period, int var, float* values) const;

    /** @brief Read one system variable at a period. */
    bool get_system_result(int period, int var, float* value) const;

    // -- Time series (one object, one variable, period range) --------------

    bool get_subcatch_series(int obj_idx, int var,
                             int start_period, int end_period,
                             float* values) const;

    bool get_node_series(int obj_idx, int var,
                         int start_period, int end_period,
                         float* values) const;

    bool get_link_series(int obj_idx, int var,
                         int start_period, int end_period,
                         float* values) const;

    bool get_system_series(int var,
                           int start_period, int end_period,
                           float* values) const;

    // -- Per-object all-variables at one period ----------------------------

    /** @brief All variables for one subcatchment at one period. */
    bool get_subcatch_attribute(int obj_idx, int period,
                                float* values, int* count) const;

    /** @brief All variables for one node at one period. */
    bool get_node_attribute(int obj_idx, int period,
                            float* values, int* count) const;

    /** @brief All variables for one link at one period. */
    bool get_link_attribute(int obj_idx, int period,
                            float* values, int* count) const;

    // -- Period time -------------------------------------------------------

    /** @brief Get the simulation time for a given period. */
    bool get_period_time(int period, double* time) const;

private:
    static constexpr int32_t MAGIC_NUMBER = 516114522;

    FILE* file_ = nullptr;

    // Header fields
    int version_     = 0;
    int flow_units_  = 0;
    int n_subcatch_  = 0;
    int n_nodes_     = 0;
    int n_links_     = 0;
    int n_polluts_   = 0;

    // Variable counts (read from file)
    int n_subcatch_vars_ = 0;
    int n_node_vars_     = 0;
    int n_link_vars_     = 0;
    int n_system_vars_   = 0;

    // Report metadata
    double start_date_  = 0.0;
    int    report_step_ = 0;

    // Footer fields
    long id_start_pos_     = 0;
    long input_start_pos_  = 0;
    long output_start_pos_ = 0;
    int  n_periods_        = 0;
    int  error_code_       = 0;

    // Computed: bytes per period record
    long bytes_per_period_ = 0;

    // Object IDs
    std::vector<std::string> subcatch_ids_;
    std::vector<std::string> node_ids_;
    std::vector<std::string> link_ids_;

    // -- Helpers ----------------------------------------------------------

    bool readFooter();
    bool readHeader();
    bool readIDs();
    bool readVariableCodes();

    bool readInt4(int32_t& value) const;
    bool readReal4(float& value) const;
    bool readReal8(double& value) const;

    /** @brief Compute the file offset of a period record start. */
    long periodOffset(int period) const;

    /** @brief Seek to a specific variable within a period for a given object type.
     *  @param period   Period index.
     *  @param obj_type 0=subcatch, 1=node, 2=link, 3=system.
     *  @param obj_idx  Object index (ignored for system).
     *  @param var      Variable index within the object's result block.
     *  @returns true on success. */
    bool seekToVar(int period, int obj_type, int obj_idx, int var) const;
};

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_OUTPUT_READER_HPP */
