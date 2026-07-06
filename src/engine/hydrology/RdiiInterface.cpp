/**
 * @file RdiiInterface.cpp
 * @brief RDII interface file — use/save pre-computed RDII inflows.
 *
 * @details Matching legacy rdii.c: rdii_openRdii(), readRdiiFileHeader(),
 *          readRdiiTextFileHeader(), readRdiiFlows(), readRdiiTextFlows(),
 *          saveRdiiFlows(), rdii_getNumRdiiFlows()/rdii_getRdiiFlow().
 *
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "RdiiInterface.hpp"
#include "../core/SimulationContext.hpp"
#include "../core/DateTime.hpp"
#include "../core/UnitConversion.hpp"

#include <cstdint>
#include <cstring>

namespace openswmm {
namespace rdii_iface {

namespace {
constexpr int    MAXLINE = 1024;
constexpr double NO_DATE = -1.0e10;
} // namespace

// ============================================================================
// openForRead — auto-detect binary vs text (legacy rdii_openRdii)
// ============================================================================

int RdiiInterfaceFile::openForRead(SimulationContext& ctx,
                                   const std::string& path) {
    close();
    writing_ = false;
    have_record_ = false;
    start_date_ = NO_DATE;
    end_date_   = NO_DATE;

    // Try binary first: check the file stamp (legacy reads strlen(FileStamp)
    // bytes and compares).
    fp_ = std::fopen(path.c_str(), "rb");
    if (!fp_) return -1;

    char stamp[16] = {};
    const std::size_t stamp_len = std::strlen(RDII_FILE_STAMP);
    if (std::fread(stamp, sizeof(char), stamp_len, fp_) == stamp_len &&
        std::strncmp(stamp, RDII_FILE_STAMP, stamp_len) == 0) {
        binary_ = true;
        const int rc = readBinaryHeader(ctx);
        if (rc != 0) { close(); return rc; }
    } else {
        // Not binary — reopen in text mode (legacy openRdiiTextFile).
        std::fclose(fp_);
        fp_ = std::fopen(path.c_str(), "rt");
        if (!fp_) return -1;
        binary_ = false;
        const int rc = readTextHeader(ctx);
        if (rc != 0) { close(); return rc; }
    }

    // Prime the first record (legacy rdii_openRdii → readRdiiFlows()).
    readNextRecord();
    return 0;
}

// ============================================================================
// Binary header: RdiiStep (int32), NumRdiiNodes (int32), indices (int32 × N)
// ============================================================================

int RdiiInterfaceFile::readBinaryHeader(SimulationContext& ctx) {
    std::int32_t step = 0, n_nodes = 0;
    if (std::fread(&step, sizeof(std::int32_t), 1, fp_) != 1 || step <= 0)
        return -2;
    if (std::fread(&n_nodes, sizeof(std::int32_t), 1, fp_) != 1 || n_nodes <= 0)
        return -2;

    rdii_step_ = static_cast<int>(step);
    node_idx_.assign(static_cast<std::size_t>(n_nodes), -1);
    flows_.assign(static_cast<std::size_t>(n_nodes), 0.0f);

    std::vector<std::int32_t> idx(static_cast<std::size_t>(n_nodes));
    if (std::fread(idx.data(), sizeof(std::int32_t),
                   static_cast<std::size_t>(n_nodes), fp_)
        != static_cast<std::size_t>(n_nodes))
        return -2;

    for (std::size_t i = 0; i < idx.size(); ++i) {
        const int j = static_cast<int>(idx[i]);
        if (j < 0 || j >= ctx.n_nodes()) return -2;  // legacy: node must exist
        node_idx_[i] = j;
    }
    return 0;
}

// ============================================================================
// Text header (legacy readRdiiTextFileHeader): node NAMES, declared units
// ============================================================================

int RdiiInterfaceFile::readTextHeader(SimulationContext& ctx) {
    char line[MAXLINE + 1];
    char s1[MAXLINE + 1];
    char s2[MAXLINE + 1];

    // Line 1: "SWMM5 ..." magic
    if (!std::fgets(line, MAXLINE, fp_)) return -2;
    if (std::sscanf(line, "%s", s1) != 1 ||
        std::strncmp(s1, "SWMM5", 5) != 0) return -2;

    // Line 2: title (skipped)
    if (!std::fgets(line, MAXLINE, fp_)) return -2;

    // Line 3: RDII time step (sec)
    if (!std::fgets(line, MAXLINE, fp_)) return -2;
    if (std::sscanf(line, "%d", &rdii_step_) != 1 || rdii_step_ <= 0)
        return -2;

    // Line 4: number of constituents (= 1 for RDII, skipped)
    if (!std::fgets(line, MAXLINE, fp_)) return -2;

    // Line 5: "FLOW <units>"
    if (!std::fgets(line, MAXLINE, fp_)) return -2;
    if (std::sscanf(line, "%s %s", s1, s2) < 2) return -2;
    static const char* flow_words[] = {"CFS","GPM","MGD","CMS","LPS","MLD"};
    flow_units_ = -1;
    for (int fi = 0; fi < 6; ++fi) {
        if (std::strncmp(s2, flow_words[fi], 3) == 0) { flow_units_ = fi; break; }
    }
    if (flow_units_ < 0) return -2;

    // Line 6: number of RDII nodes
    int n_nodes = 0;
    if (!std::fgets(line, MAXLINE, fp_)) return -2;
    if (std::sscanf(line, "%d", &n_nodes) != 1 || n_nodes <= 0) return -2;

    node_idx_.assign(static_cast<std::size_t>(n_nodes), -1);
    flows_.assign(static_cast<std::size_t>(n_nodes), 0.0f);

    // Node names, resolved against the project (legacy: unresolved = error)
    for (int i = 0; i < n_nodes; ++i) {
        if (!std::fgets(line, MAXLINE, fp_)) return -2;
        if (std::sscanf(line, "%s", s1) != 1) return -2;
        const int j = ctx.node_names.find(s1);
        if (j < 0) return -2;
        node_idx_[static_cast<std::size_t>(i)] = j;
    }

    // Column headings line (skipped)
    if (!std::fgets(line, MAXLINE, fp_)) return -2;
    return 0;
}

// ============================================================================
// readNextRecord — legacy readRdiiFlows() / readRdiiTextFlows()
// ============================================================================

bool RdiiInterfaceFile::readNextRecord() {
    have_record_ = false;
    start_date_ = NO_DATE;
    end_date_   = NO_DATE;
    if (!fp_ || writing_) return false;

    if (binary_) {
        double date = NO_DATE;
        if (std::fread(&date, sizeof(double), 1, fp_) != 1) return false;
        if (std::fread(flows_.data(), sizeof(float), flows_.size(), fp_)
            != flows_.size()) return false;
        start_date_ = date;
    } else {
        char line[MAXLINE + 1];
        char s[MAXLINE + 1];
        int yr = 0, mon = 0, day = 0, hr = 0, mn = 0, sec = 0;
        double x = 0.0;
        for (std::size_t i = 0; i < flows_.size(); ++i) {
            if (!std::fgets(line, MAXLINE, fp_)) return false;
            if (std::sscanf(line, "%s %d %d %d %d %d %d %lf",
                            s, &yr, &mon, &day, &hr, &mn, &sec, &x) < 8)
                return false;
            // Convert declared units → cfs (legacy: x / Qcf[RdiiFlowUnits])
            flows_[i] = static_cast<float>(x / ucf::Qcf[flow_units_]);
        }
        start_date_ = datetime::encodeDate(yr, mon, day)
                    + datetime::encodeTime(hr, mn, sec);
    }

    end_date_ = datetime::addSeconds(start_date_,
                                     static_cast<double>(rdii_step_));
    have_record_ = true;
    return true;
}

// ============================================================================
// applyFlows — legacy rdii_getNumRdiiFlows() bracketing + addRdiiInflows()
// ============================================================================

void RdiiInterfaceFile::applyFlows(SimulationContext& ctx,
                                   double current_date) {
    if (!fp_ || writing_) return;

    // Advance until the record interval [start, end) brackets current_date.
    while (have_record_) {
        if (current_date < start_date_) return;   // record not reached yet
        if (current_date < end_date_) break;      // bracketed — apply
        readNextRecord();                          // move to next period
    }
    if (!have_record_) return;                     // past end of file

    for (std::size_t i = 0; i < node_idx_.size(); ++i) {
        const int j = node_idx_[i];
        if (j < 0 || j >= ctx.n_nodes()) continue;
        ctx.nodes.rdii_inflow[static_cast<std::size_t>(j)] +=
            static_cast<double>(flows_[i]);
    }
}

// ============================================================================
// openForWrite — binary header (legacy createRdiiFile header layout)
// ============================================================================

int RdiiInterfaceFile::openForWrite(const std::string& path, int rdii_step,
                                    const std::vector<int>& node_idx) {
    close();
    if (node_idx.empty()) return -2;   // no RDII in model — nothing to save

    fp_ = std::fopen(path.c_str(), "wb");
    if (!fp_) return -1;

    writing_ = true;
    binary_  = true;
    rdii_step_ = (rdii_step > 0) ? rdii_step : 300;
    node_idx_ = node_idx;
    flows_.assign(node_idx_.size(), 0.0f);

    std::fwrite(RDII_FILE_STAMP, sizeof(char),
                std::strlen(RDII_FILE_STAMP), fp_);
    const std::int32_t step = static_cast<std::int32_t>(rdii_step_);
    const std::int32_t n    = static_cast<std::int32_t>(node_idx_.size());
    std::fwrite(&step, sizeof(std::int32_t), 1, fp_);
    std::fwrite(&n,    sizeof(std::int32_t), 1, fp_);
    std::vector<std::int32_t> idx(node_idx_.begin(), node_idx_.end());
    std::fwrite(idx.data(), sizeof(std::int32_t), idx.size(), fp_);
    return 0;
}

// ============================================================================
// saveFlows — legacy saveRdiiFlows(): date (float64) + flows (float32 × N)
// ============================================================================

void RdiiInterfaceFile::saveFlows(double current_date,
                                  const std::vector<double>& node_flows) {
    if (!fp_ || !writing_) return;

    for (std::size_t i = 0; i < node_idx_.size(); ++i) {
        const auto j = static_cast<std::size_t>(node_idx_[i]);
        flows_[i] = (j < node_flows.size())
                    ? static_cast<float>(node_flows[j]) : 0.0f;
    }
    std::fwrite(&current_date, sizeof(double), 1, fp_);
    std::fwrite(flows_.data(), sizeof(float), flows_.size(), fp_);
}

// ============================================================================
// close
// ============================================================================

void RdiiInterfaceFile::close() {
    if (fp_) {
        std::fclose(fp_);
        fp_ = nullptr;
    }
    writing_ = false;
    have_record_ = false;
}

} // namespace rdii_iface
} // namespace openswmm
