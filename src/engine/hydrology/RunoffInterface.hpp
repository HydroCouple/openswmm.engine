/**
 * @file RunoffInterface.hpp
 * @brief Runoff interface file — save/load pre-computed runoff results.
 *
 * @details Binary file format (matching legacy runoff.c):
 *   Header:
 *     - File stamp: "SWMM5-RUNOFF" (12 bytes)
 *     - nSubcatch (int32)
 *     - nPollut (int32)
 *     - flowUnits (int32)
 *     - maxSteps (int32, updated at close)
 *   Per timestep:
 *     - tStep (float32, seconds)
 *     - Per subcatchment:
 *       - rainfall, snow_depth, evap, infil, runoff, gw_flow, gw_elev,
 *         soil_moist (8 float32)
 *       - washoff concentration per pollutant (nPollut float32)
 *
 * @note Legacy reference: src/legacy/engine/runoff.c
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_RUNOFF_INTERFACE_HPP
#define OPENSWMM_RUNOFF_INTERFACE_HPP

#include <string>
#include <vector>
#include <cstdio>

namespace openswmm {

struct SimulationContext;

namespace runoff_iface {

static constexpr int N_FIXED_RESULTS = 8;  ///< Fixed result fields per subcatchment
static const char FILE_STAMP[] = "SWMM5-RUNOFF";

/**
 * @brief Runoff interface file manager.
 */
class RunoffInterfaceFile {
public:
    ~RunoffInterfaceFile() { close(); }

    /**
     * @brief Open file for writing (SAVE mode).
     * @returns 0 on success, error code on failure.
     */
    int openForWrite(const std::string& path, int n_subcatch, int n_pollut,
                     int flow_units);

    /**
     * @brief Open file for reading (USE mode).
     * @returns 0 on success, error code on failure.
     */
    int openForRead(const std::string& path, int n_subcatch, int n_pollut,
                    int flow_units);

    /**
     * @brief Save one timestep of runoff results to file.
     * @param ctx  Simulation context (subcatchment results read from here).
     * @param dt   Timestep duration (seconds).
     */
    void saveResults(const SimulationContext& ctx, double dt);

    /**
     * @brief Read one timestep of runoff results from file.
     * @param ctx  Simulation context (subcatchment results written here).
     * @returns true if data was read, false if EOF.
     */
    bool readResults(SimulationContext& ctx);

    /// Close the file and finalize header (write step count for SAVE mode).
    void close();

    bool isOpen() const { return fp_ != nullptr; }

private:
    std::FILE* fp_ = nullptr;
    bool writing_ = false;
    int n_subcatch_ = 0;
    int n_pollut_ = 0;
    int n_results_ = 0;       ///< N_FIXED_RESULTS + n_pollut
    int step_count_ = 0;
    long max_steps_pos_ = 0;   ///< File position of maxSteps field
    std::vector<float> buf_;   ///< Reusable buffer for read/write
};

} // namespace runoff_iface
} // namespace openswmm

#endif // OPENSWMM_RUNOFF_INTERFACE_HPP
