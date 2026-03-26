/**
 * @file OutputReader.cpp
 * @brief OutputReader — reads SWMM 5.x binary .out files.
 *
 * @details Implements random-access reads into the binary output format
 *          produced by DefaultOutputPlugin. The footer is read first to
 *          obtain section offsets, then the header and ID sections are parsed
 *          to populate metadata. Period data is read on demand via seeks.
 *
 * @see OutputReader.hpp
 * @see DefaultOutputPlugin.cpp — the writer
 * @ingroup engine_output
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "OutputReader.hpp"

#include <cstring>
#include <algorithm>

namespace openswmm {

// ============================================================================
// Lifecycle
// ============================================================================

OutputReader::~OutputReader() {
    close();
}

bool OutputReader::open(const char* path) {
    if (!path) return false;

    close();  // ensure clean state

    file_ = std::fopen(path, "rb");
    if (!file_) return false;

    // Read footer first (last 6 × int32)
    if (!readFooter()) { close(); return false; }

    // Read header from beginning
    if (!readHeader()) { close(); return false; }

    // Read object IDs
    if (!readIDs()) { close(); return false; }

    // Read variable codes and report metadata
    if (!readVariableCodes()) { close(); return false; }

    // Compute bytes per period:
    //   8 (date) + n_subcatch * n_subcatch_vars * 4
    //            + n_nodes    * n_node_vars     * 4
    //            + n_links    * n_link_vars     * 4
    //            + n_system_vars * 4
    bytes_per_period_ = static_cast<long>(sizeof(double))
        + static_cast<long>(n_subcatch_) * n_subcatch_vars_ * static_cast<long>(sizeof(float))
        + static_cast<long>(n_nodes_)    * n_node_vars_     * static_cast<long>(sizeof(float))
        + static_cast<long>(n_links_)    * n_link_vars_     * static_cast<long>(sizeof(float))
        + static_cast<long>(n_system_vars_)                 * static_cast<long>(sizeof(float));

    return true;
}

void OutputReader::close() {
    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
    }
    subcatch_ids_.clear();
    node_ids_.clear();
    link_ids_.clear();
    n_periods_ = 0;
}

// ============================================================================
// Object ID access
// ============================================================================

const char* OutputReader::subcatch_id(int index) const {
    if (index < 0 || index >= n_subcatch_) return nullptr;
    return subcatch_ids_[static_cast<std::size_t>(index)].c_str();
}

const char* OutputReader::node_id(int index) const {
    if (index < 0 || index >= n_nodes_) return nullptr;
    return node_ids_[static_cast<std::size_t>(index)].c_str();
}

const char* OutputReader::link_id(int index) const {
    if (index < 0 || index >= n_links_) return nullptr;
    return link_ids_[static_cast<std::size_t>(index)].c_str();
}

// ============================================================================
// Per-period results (all objects, one variable, one period)
// ============================================================================

bool OutputReader::get_subcatch_result(int period, int var, float* values) const {
    if (!file_ || !values) return false;
    if (period < 0 || period >= n_periods_) return false;
    if (var < 0 || var >= n_subcatch_vars_) return false;

    for (int j = 0; j < n_subcatch_; ++j) {
        if (!seekToVar(period, 0, j, var)) return false;
        if (!readReal4(values[j])) return false;
    }
    return true;
}

bool OutputReader::get_node_result(int period, int var, float* values) const {
    if (!file_ || !values) return false;
    if (period < 0 || period >= n_periods_) return false;
    if (var < 0 || var >= n_node_vars_) return false;

    for (int j = 0; j < n_nodes_; ++j) {
        if (!seekToVar(period, 1, j, var)) return false;
        if (!readReal4(values[j])) return false;
    }
    return true;
}

bool OutputReader::get_link_result(int period, int var, float* values) const {
    if (!file_ || !values) return false;
    if (period < 0 || period >= n_periods_) return false;
    if (var < 0 || var >= n_link_vars_) return false;

    for (int j = 0; j < n_links_; ++j) {
        if (!seekToVar(period, 2, j, var)) return false;
        if (!readReal4(values[j])) return false;
    }
    return true;
}

bool OutputReader::get_system_result(int period, int var, float* value) const {
    if (!file_ || !value) return false;
    if (period < 0 || period >= n_periods_) return false;
    if (var < 0 || var >= n_system_vars_) return false;

    if (!seekToVar(period, 3, 0, var)) return false;
    return readReal4(*value);
}

// ============================================================================
// Time series (one object, one variable, period range)
// ============================================================================

bool OutputReader::get_subcatch_series(int obj_idx, int var,
                                       int start_period, int end_period,
                                       float* values) const {
    if (!file_ || !values) return false;
    if (obj_idx < 0 || obj_idx >= n_subcatch_) return false;
    if (var < 0 || var >= n_subcatch_vars_) return false;
    if (start_period < 0 || end_period >= n_periods_ || start_period > end_period) return false;

    for (int p = start_period; p <= end_period; ++p) {
        if (!seekToVar(p, 0, obj_idx, var)) return false;
        if (!readReal4(values[p - start_period])) return false;
    }
    return true;
}

bool OutputReader::get_node_series(int obj_idx, int var,
                                    int start_period, int end_period,
                                    float* values) const {
    if (!file_ || !values) return false;
    if (obj_idx < 0 || obj_idx >= n_nodes_) return false;
    if (var < 0 || var >= n_node_vars_) return false;
    if (start_period < 0 || end_period >= n_periods_ || start_period > end_period) return false;

    for (int p = start_period; p <= end_period; ++p) {
        if (!seekToVar(p, 1, obj_idx, var)) return false;
        if (!readReal4(values[p - start_period])) return false;
    }
    return true;
}

bool OutputReader::get_link_series(int obj_idx, int var,
                                    int start_period, int end_period,
                                    float* values) const {
    if (!file_ || !values) return false;
    if (obj_idx < 0 || obj_idx >= n_links_) return false;
    if (var < 0 || var >= n_link_vars_) return false;
    if (start_period < 0 || end_period >= n_periods_ || start_period > end_period) return false;

    for (int p = start_period; p <= end_period; ++p) {
        if (!seekToVar(p, 2, obj_idx, var)) return false;
        if (!readReal4(values[p - start_period])) return false;
    }
    return true;
}

bool OutputReader::get_system_series(int var,
                                      int start_period, int end_period,
                                      float* values) const {
    if (!file_ || !values) return false;
    if (var < 0 || var >= n_system_vars_) return false;
    if (start_period < 0 || end_period >= n_periods_ || start_period > end_period) return false;

    for (int p = start_period; p <= end_period; ++p) {
        if (!seekToVar(p, 3, 0, var)) return false;
        if (!readReal4(values[p - start_period])) return false;
    }
    return true;
}

// ============================================================================
// Per-object all-variables at one period
// ============================================================================

bool OutputReader::get_subcatch_attribute(int obj_idx, int period,
                                          float* values, int* count) const {
    if (!file_ || !values) return false;
    if (obj_idx < 0 || obj_idx >= n_subcatch_) return false;
    if (period < 0 || period >= n_periods_) return false;

    if (!seekToVar(period, 0, obj_idx, 0)) return false;
    for (int v = 0; v < n_subcatch_vars_; ++v) {
        if (!readReal4(values[v])) return false;
    }
    if (count) *count = n_subcatch_vars_;
    return true;
}

bool OutputReader::get_node_attribute(int obj_idx, int period,
                                      float* values, int* count) const {
    if (!file_ || !values) return false;
    if (obj_idx < 0 || obj_idx >= n_nodes_) return false;
    if (period < 0 || period >= n_periods_) return false;

    if (!seekToVar(period, 1, obj_idx, 0)) return false;
    for (int v = 0; v < n_node_vars_; ++v) {
        if (!readReal4(values[v])) return false;
    }
    if (count) *count = n_node_vars_;
    return true;
}

bool OutputReader::get_link_attribute(int obj_idx, int period,
                                      float* values, int* count) const {
    if (!file_ || !values) return false;
    if (obj_idx < 0 || obj_idx >= n_links_) return false;
    if (period < 0 || period >= n_periods_) return false;

    if (!seekToVar(period, 2, obj_idx, 0)) return false;
    for (int v = 0; v < n_link_vars_; ++v) {
        if (!readReal4(values[v])) return false;
    }
    if (count) *count = n_link_vars_;
    return true;
}

// ============================================================================
// Period time
// ============================================================================

bool OutputReader::get_period_time(int period, double* time) const {
    if (!file_ || !time) return false;
    if (period < 0 || period >= n_periods_) return false;

    long offset = periodOffset(period);
    if (std::fseek(file_, offset, SEEK_SET) != 0) return false;
    return readReal8(*time);
}

// ============================================================================
// Internal: Footer, Header, IDs, Variable Codes
// ============================================================================

bool OutputReader::readFooter() {
    // Footer is the last 6 × sizeof(int32_t) bytes of the file
    if (std::fseek(file_, -6 * static_cast<long>(sizeof(int32_t)), SEEK_END) != 0)
        return false;

    int32_t id_pos = 0, input_pos = 0, output_pos = 0;
    int32_t periods = 0, err = 0, magic = 0;

    if (!readInt4(id_pos))     return false;
    if (!readInt4(input_pos))  return false;
    if (!readInt4(output_pos)) return false;
    if (!readInt4(periods))    return false;
    if (!readInt4(err))        return false;
    if (!readInt4(magic))      return false;

    if (magic != MAGIC_NUMBER) return false;

    id_start_pos_     = static_cast<long>(id_pos);
    input_start_pos_  = static_cast<long>(input_pos);
    output_start_pos_ = static_cast<long>(output_pos);
    n_periods_        = static_cast<int>(periods);
    error_code_       = static_cast<int>(err);

    return true;
}

bool OutputReader::readHeader() {
    if (std::fseek(file_, 0, SEEK_SET) != 0)
        return false;

    int32_t magic = 0, ver = 0, fu = 0;
    int32_t ns = 0, nn = 0, nl = 0, np = 0;

    if (!readInt4(magic)) return false;
    if (magic != MAGIC_NUMBER) return false;

    if (!readInt4(ver)) return false;
    if (!readInt4(fu))  return false;
    if (!readInt4(ns))  return false;
    if (!readInt4(nn))  return false;
    if (!readInt4(nl))  return false;
    if (!readInt4(np))  return false;

    version_    = static_cast<int>(ver);
    flow_units_ = static_cast<int>(fu);
    n_subcatch_ = static_cast<int>(ns);
    n_nodes_    = static_cast<int>(nn);
    n_links_    = static_cast<int>(nl);
    n_polluts_  = static_cast<int>(np);

    return true;
}

bool OutputReader::readIDs() {
    // ID section starts at id_start_pos_ (which is right after the 7-int header)
    if (std::fseek(file_, id_start_pos_, SEEK_SET) != 0)
        return false;

    auto readIDList = [this](int count, std::vector<std::string>& ids) -> bool {
        ids.resize(static_cast<std::size_t>(count));
        for (int j = 0; j < count; ++j) {
            int32_t len = 0;
            if (!readInt4(len)) return false;
            if (len < 0 || len > 1024) return false;  // sanity check
            std::string id(static_cast<std::size_t>(len), '\0');
            if (len > 0) {
                if (std::fread(&id[0], 1, static_cast<std::size_t>(len), file_)
                    != static_cast<std::size_t>(len))
                    return false;
            }
            ids[static_cast<std::size_t>(j)] = std::move(id);
        }
        return true;
    };

    if (!readIDList(n_subcatch_, subcatch_ids_)) return false;
    if (!readIDList(n_nodes_, node_ids_))        return false;
    if (!readIDList(n_links_, link_ids_))        return false;

    return true;
}

bool OutputReader::readVariableCodes() {
    // We need to skip from the current position (after IDs) past the input
    // section to read variable codes. The input section starts at input_start_pos_.
    // After input data comes variable codes, then report metadata.

    // Seek to input_start_pos_ to skip over it
    if (std::fseek(file_, input_start_pos_, SEEK_SET) != 0)
        return false;

    // Skip subcatchment input properties: count(4) + codes(count*4) + data(n_subcatch*count*4)
    {
        int32_t n_props = 0;
        if (!readInt4(n_props)) return false;
        // Skip property codes + per-subcatch data
        // Each subcatch has: 1 float for area (per prop)
        long skip = static_cast<long>(n_props) * static_cast<long>(sizeof(int32_t))  // codes
                  + static_cast<long>(n_subcatch_) * (static_cast<long>(sizeof(float)) * 1);  // area per subcatch × 1 prop listed
        // Actually, the writer writes: for each subcatch { area } — only the values for the listed props
        // Looking at the writer: writeReal4(area) per subcatch — so data is n_subcatch * 1 float for 1 prop
        // But generic: the codes specify the property ID; the data block has one value per property per object.
        // For subcatch: 1 prop → each subcatch writes 1 float
        // Wait—the actual layout in the writer is more nuanced. Let me re-check:
        //   writeInt4(1);  // 1 input property
        //   writeInt4(1);  // INPUT_AREA code
        //   for j: writeReal4(area)
        // So: 1 int(code count) + 1 int(code) + n_subcatch floats
        // Then nodes: 1 int(prop count=3) + 3 ints(codes) + n_nodes*(1 int + 2 floats)
        // Then links: 1 int(prop count=5) + 5 ints(codes) + n_links*(1 int + 4 floats)
        //
        // This is tricky because node data mixes int (type) and float (invert, depth).
        // The actual bytes are: per node writes 3 values (int32, float, float) = 12 bytes per node
        // Per link writes 5 values (int32, float, float, float, float) = 20 bytes per link
        //
        // Rather than parsing all of this, we can skip to the variable codes section by
        // computing total input section bytes:
        //   subcatch: 4 + 1*4 + n_subcatch*4           = 4 + 4 + n_subcatch*4
        //   nodes:    4 + 3*4 + n_nodes*(4+4+4)        = 4 + 12 + n_nodes*12
        //   links:    4 + 5*4 + n_links*(4+4+4+4+4)    = 4 + 20 + n_links*20
        (void)skip;  // discard the incremental computation
    }

    // Better approach: compute total input section size and seek past it
    long input_size =
        // Subcatch input: count(4) + codes(1×4) + data(n_subcatch × 1 × 4)
        4 + 1 * 4 + static_cast<long>(n_subcatch_) * 4
        // Node input: count(4) + codes(3×4) + data(n_nodes × (4+4+4))
        + 4 + 3 * 4 + static_cast<long>(n_nodes_) * 12
        // Link input: count(4) + codes(5×4) + data(n_links × (4+4+4+4+4))
        + 4 + 5 * 4 + static_cast<long>(n_links_) * 20;

    if (std::fseek(file_, input_start_pos_ + input_size, SEEK_SET) != 0)
        return false;

    // Now read variable code counts (skip the actual codes themselves)
    {
        int32_t nv = 0;
        // Subcatch vars
        if (!readInt4(nv)) return false;
        n_subcatch_vars_ = static_cast<int>(nv);
        if (std::fseek(file_, static_cast<long>(nv) * static_cast<long>(sizeof(int32_t)), SEEK_CUR) != 0)
            return false;

        // Node vars
        if (!readInt4(nv)) return false;
        n_node_vars_ = static_cast<int>(nv);
        if (std::fseek(file_, static_cast<long>(nv) * static_cast<long>(sizeof(int32_t)), SEEK_CUR) != 0)
            return false;

        // Link vars
        if (!readInt4(nv)) return false;
        n_link_vars_ = static_cast<int>(nv);
        if (std::fseek(file_, static_cast<long>(nv) * static_cast<long>(sizeof(int32_t)), SEEK_CUR) != 0)
            return false;

        // System vars
        if (!readInt4(nv)) return false;
        n_system_vars_ = static_cast<int>(nv);
        if (std::fseek(file_, static_cast<long>(nv) * static_cast<long>(sizeof(int32_t)), SEEK_CUR) != 0)
            return false;
    }

    // Read report metadata
    if (!readReal8(start_date_)) return false;
    {
        int32_t rs = 0;
        if (!readInt4(rs)) return false;
        report_step_ = static_cast<int>(rs);
    }

    return true;
}

// ============================================================================
// Seek helpers
// ============================================================================

long OutputReader::periodOffset(int period) const {
    return output_start_pos_ + static_cast<long>(period) * bytes_per_period_;
}

bool OutputReader::seekToVar(int period, int obj_type, int obj_idx, int var) const {
    // Start of period record
    long offset = periodOffset(period);

    // Skip date (8 bytes)
    offset += static_cast<long>(sizeof(double));

    // Navigate to the right object type block
    switch (obj_type) {
        case 0: // subcatchment
            // Offset into subcatch block: obj_idx * n_subcatch_vars + var
            offset += (static_cast<long>(obj_idx) * n_subcatch_vars_ + var)
                      * static_cast<long>(sizeof(float));
            break;

        case 1: // node
            // Skip entire subcatch block
            offset += static_cast<long>(n_subcatch_) * n_subcatch_vars_
                      * static_cast<long>(sizeof(float));
            // Offset into node block
            offset += (static_cast<long>(obj_idx) * n_node_vars_ + var)
                      * static_cast<long>(sizeof(float));
            break;

        case 2: // link
            // Skip subcatch + node blocks
            offset += static_cast<long>(n_subcatch_) * n_subcatch_vars_
                      * static_cast<long>(sizeof(float));
            offset += static_cast<long>(n_nodes_) * n_node_vars_
                      * static_cast<long>(sizeof(float));
            // Offset into link block
            offset += (static_cast<long>(obj_idx) * n_link_vars_ + var)
                      * static_cast<long>(sizeof(float));
            break;

        case 3: // system
            // Skip subcatch + node + link blocks
            offset += static_cast<long>(n_subcatch_) * n_subcatch_vars_
                      * static_cast<long>(sizeof(float));
            offset += static_cast<long>(n_nodes_) * n_node_vars_
                      * static_cast<long>(sizeof(float));
            offset += static_cast<long>(n_links_) * n_link_vars_
                      * static_cast<long>(sizeof(float));
            // Offset into system block
            offset += static_cast<long>(var) * static_cast<long>(sizeof(float));
            break;

        default:
            return false;
    }

    return std::fseek(file_, offset, SEEK_SET) == 0;
}

// ============================================================================
// Binary read helpers
// ============================================================================

bool OutputReader::readInt4(int32_t& value) const {
    return std::fread(&value, sizeof(int32_t), 1, file_) == 1;
}

bool OutputReader::readReal4(float& value) const {
    return std::fread(&value, sizeof(float), 1, file_) == 1;
}

bool OutputReader::readReal8(double& value) const {
    return std::fread(&value, sizeof(double), 1, file_) == 1;
}

} /* namespace openswmm */
