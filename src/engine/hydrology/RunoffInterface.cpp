/**
 * @file RunoffInterface.cpp
 * @brief Runoff interface file — binary save/load of pre-computed runoff.
 *
 * @details Matching legacy runoff.c: runoff_initFile, runoff_saveToFile,
 *          runoff_readFromFile.
 *
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "RunoffInterface.hpp"
#include "../core/SimulationContext.hpp"
#include <cstring>

namespace openswmm {
namespace runoff_iface {

// ============================================================================
// Open for writing (SAVE mode)
// ============================================================================

int RunoffInterfaceFile::openForWrite(const std::string& path, int n_subcatch,
                                      int n_pollut, int flow_units) {
    fp_ = std::fopen(path.c_str(), "wb");
    if (!fp_) return -1;

    writing_ = true;
    n_subcatch_ = n_subcatch;
    n_pollut_ = n_pollut;
    n_results_ = N_FIXED_RESULTS + n_pollut;
    step_count_ = 0;

    buf_.resize(static_cast<size_t>(n_results_));

    // Write header
    std::fwrite(FILE_STAMP, sizeof(char), 12, fp_);

    int32_t ns = static_cast<int32_t>(n_subcatch);
    int32_t np = static_cast<int32_t>(n_pollut);
    int32_t fu = static_cast<int32_t>(flow_units);
    int32_t ms = 0;  // placeholder for max steps

    std::fwrite(&ns, sizeof(int32_t), 1, fp_);
    std::fwrite(&np, sizeof(int32_t), 1, fp_);
    std::fwrite(&fu, sizeof(int32_t), 1, fp_);

    // Save position of maxSteps for later update
    max_steps_pos_ = std::ftell(fp_);
    std::fwrite(&ms, sizeof(int32_t), 1, fp_);

    return 0;
}

// ============================================================================
// Open for reading (USE mode)
// ============================================================================

int RunoffInterfaceFile::openForRead(const std::string& path, int n_subcatch,
                                     int n_pollut, int flow_units) {
    fp_ = std::fopen(path.c_str(), "rb");
    if (!fp_) return -1;

    writing_ = false;

    // Read and verify header
    char stamp[12] = {};
    std::fread(stamp, sizeof(char), 12, fp_);
    if (std::strncmp(stamp, FILE_STAMP, 12) != 0) {
        std::fclose(fp_);
        fp_ = nullptr;
        return -2;  // invalid file format
    }

    int32_t ns, np, fu, ms;
    std::fread(&ns, sizeof(int32_t), 1, fp_);
    std::fread(&np, sizeof(int32_t), 1, fp_);
    std::fread(&fu, sizeof(int32_t), 1, fp_);
    std::fread(&ms, sizeof(int32_t), 1, fp_);

    // Verify compatibility
    if (ns != static_cast<int32_t>(n_subcatch) ||
        np != static_cast<int32_t>(n_pollut) ||
        fu != static_cast<int32_t>(flow_units)) {
        std::fclose(fp_);
        fp_ = nullptr;
        return -3;  // incompatible data
    }

    n_subcatch_ = n_subcatch;
    n_pollut_ = n_pollut;
    n_results_ = N_FIXED_RESULTS + n_pollut;
    step_count_ = 0;

    buf_.resize(static_cast<size_t>(n_results_));

    return 0;
}

// ============================================================================
// Save one timestep of results
// ============================================================================

void RunoffInterfaceFile::saveResults(const SimulationContext& ctx, double dt) {
    if (!fp_ || !writing_) return;

    float dt_f = static_cast<float>(dt);
    std::fwrite(&dt_f, sizeof(float), 1, fp_);

    int np = n_pollut_;
    for (int j = 0; j < n_subcatch_; ++j) {
        auto uj = static_cast<size_t>(j);

        // Fixed results (matching legacy SUBCATCH_RAINFALL..SUBCATCH_SOIL_MOIST)
        double rainfall = 0.0;
        if (ctx.subcatches.gage[uj] >= 0 &&
            ctx.subcatches.gage[uj] < static_cast<int>(ctx.gages.rainfall.size()))
            rainfall = ctx.gages.rainfall[static_cast<size_t>(ctx.subcatches.gage[uj])];

        buf_[0] = static_cast<float>(rainfall);
        buf_[1] = 0.0f;  // snow depth (placeholder)
        buf_[2] = static_cast<float>(ctx.subcatches.evap_loss[uj]);
        buf_[3] = static_cast<float>(ctx.subcatches.infil_loss[uj]);
        buf_[4] = static_cast<float>(ctx.subcatches.runoff[uj]);
        buf_[5] = static_cast<float>(ctx.subcatches.gw_flow[uj]);
        buf_[6] = 0.0f;  // GW elevation (placeholder)
        buf_[7] = 0.0f;  // soil moisture (placeholder)

        // Pollutant washoff concentrations
        for (int p = 0; p < np; ++p) {
            auto sq = uj * static_cast<size_t>(np) + static_cast<size_t>(p);
            buf_[static_cast<size_t>(N_FIXED_RESULTS + p)] =
                (sq < ctx.subcatches.conc.size())
                ? static_cast<float>(ctx.subcatches.conc[sq]) : 0.0f;
        }

        std::fwrite(buf_.data(), sizeof(float),
                    static_cast<size_t>(n_results_), fp_);
    }
    step_count_++;
}

// ============================================================================
// Read one timestep of results
// ============================================================================

bool RunoffInterfaceFile::readResults(SimulationContext& ctx) {
    if (!fp_ || writing_) return false;

    float dt_f;
    if (std::fread(&dt_f, sizeof(float), 1, fp_) != 1) return false;

    int np = n_pollut_;
    for (int j = 0; j < n_subcatch_; ++j) {
        auto uj = static_cast<size_t>(j);

        if (std::fread(buf_.data(), sizeof(float),
                       static_cast<size_t>(n_results_), fp_)
            != static_cast<size_t>(n_results_))
            return false;

        // Restore subcatchment state from file
        ctx.subcatches.runoff[uj] = static_cast<double>(buf_[4]);

        // Restore pollutant concentrations
        for (int p = 0; p < np; ++p) {
            auto sq = uj * static_cast<size_t>(np) + static_cast<size_t>(p);
            if (sq < ctx.subcatches.conc.size())
                ctx.subcatches.conc[sq] =
                    static_cast<double>(buf_[static_cast<size_t>(N_FIXED_RESULTS + p)]);
        }
    }
    step_count_++;
    return true;
}

// ============================================================================
// Close
// ============================================================================

void RunoffInterfaceFile::close() {
    if (!fp_) return;

    if (writing_ && max_steps_pos_ > 0) {
        // Update maxSteps in header
        std::fseek(fp_, max_steps_pos_, SEEK_SET);
        int32_t ms = static_cast<int32_t>(step_count_);
        std::fwrite(&ms, sizeof(int32_t), 1, fp_);
    }

    std::fclose(fp_);
    fp_ = nullptr;
}

} // namespace runoff_iface
} // namespace openswmm
