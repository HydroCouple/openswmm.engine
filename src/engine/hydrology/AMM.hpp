/**
 * @file AMM.hpp
 * @brief Antecedent Moisture Model — alternative RDII formulation.
 *
 * @details Implements the reparameterized AMM equations from:
 *
 *   Edgren, D., Czachorski, R., & Gonwa, W. (2024).
 *   "Reparameterizing the Antecedent Moisture Model."
 *   Journal of Water Management Modeling, C525.
 *   https://doi.org/10.14796/JWMM.C525
 *
 * The AMM replaces the RTK unit-hydrograph convolution with a three-level
 * recursive model that accounts for antecedent soil moisture and seasonal
 * temperature effects on rainfall capture fraction:
 *
 *   Level 1 — Rainfall-runoff function   (Eq 1-4)
 *   Level 2 — Antecedent moisture        (Eq 5-6)
 *   Level 3 — Seasonal hydrologic cond.  (Eq 7-11)
 *
 * Plus an optional baseflow component    (Eq 12-16).
 *
 * @note Selected via RDII_METHOD AMM in [OPTIONS].
 * @ingroup new_engine
 *
 * @license  MIT License
 */

#ifndef OPENSWMM_AMM_HPP
#define OPENSWMM_AMM_HPP

#include <vector>
#include <string>
#include <unordered_map>
#include <cstddef>

namespace openswmm {

struct SimulationContext;

namespace amm {

// ============================================================================
// Parameter structure (one per component in [AMM] section)
// ============================================================================

/**
 * @brief Parameters for one AMM component (standard 3-level or 2-level baseflow).
 *
 * ### Calibratable parameters (standard component)
 * | Symbol    | Field       | Description                              | Units   |
 * |-----------|-------------|------------------------------------------|---------|
 * | RD        | RD          | Dry-weather capture fraction             | -       |
 * | PAT       | PAT         | Precipitation averaging time             | seconds |
 * | HHL       | HHL         | Hydrograph half-life                     | seconds |
 * | AMHL      | AMHL        | Antecedent moisture half-life            | seconds |
 * | Cold SHCF | cold_SHCF   | Cold-weather SHCF (high capture)         | 1/L     |
 * | Hot SHCF  | hot_SHCF    | Hot-weather SHCF (low capture)           | 1/L     |
 * | Cold Temp | cold_temp   | Cold reference temperature               | deg     |
 * | Hot Temp  | hot_temp    | Hot reference temperature                | deg     |
 * | TAT       | TAT         | Temperature averaging time               | seconds |
 */
struct AMMParams {
    double area      = 0.0;    ///< Contributing area (project area units)
    double RD        = 0.0;    ///< Min capture fraction (dry weather)
    double PAT       = 0.0;    ///< Precipitation averaging time (sec)
    double HHL       = 3600.0; ///< Hydrograph half-life (sec)
    double AMHL      = 28800.0;///< Antecedent moisture half-life (sec)
    double cold_SHCF = 0.0;    ///< SHCF at cold temperature (1/length)
    double hot_SHCF  = 0.0;    ///< SHCF at hot temperature  (1/length)
    double cold_temp = 30.0;   ///< Cold reference temp
    double hot_temp  = 70.0;   ///< Hot reference temp
    double TAT       = 0.0;    ///< Temperature averaging time (sec)

    bool   is_baseflow = false;///< True -> skip Level 2, use cold_R / hot_R
    double cold_R    = 0.0;    ///< Cold capture fraction (baseflow)
    double hot_R     = 0.0;    ///< Hot capture fraction  (baseflow)

    double area_to_flow = 1.0 / 12.0;  ///< area*depth -> volume conversion
};

// ============================================================================
// Per-component runtime state
// ============================================================================

struct AMMState {
    double Q_prev   = 0.0;
    double RW_prev  = 0.0;

    std::vector<double> precip_buf;
    int    precip_head = 0;

    std::vector<double> temp_buf;
    int    temp_head   = 0;
};

// ============================================================================
// Solver
// ============================================================================

/**
 * @brief Antecedent Moisture Model solver for engine integration.
 *
 * Manages multiple AMM components (each assigned to a node) and computes
 * RDII inflow contributions at each timestep via the step() interface
 * or the computeAll() interface that matches RDIISolver's pattern.
 */
class AMMSolver {
public:
    /**
     * @brief Add a named AMM component group.
     * @param name   Group name (matches [AMM] section 2nd field).
     * @param params AMM parameters for this component.
     * @returns Index of the registered parameter set.
     */
    int addComponentParams(const std::string& name, const AMMParams& params);

    /**
     * @brief Look up AMM component index by name.
     * @returns Index, or -1 if not found.
     */
    int findComponent(const std::string& name) const;

    /**
     * @brief Initialize from SimulationContext AMM assignments.
     *
     * Resolves named AMM groups to parameter indices and allocates
     * ring buffers for the moving-average computations.
     */
    void init(SimulationContext& ctx);

    /**
     * @brief Reset all state to zero (for re-running).
     */
    void reset();

    /**
     * @brief Compute AMM inflows for all groups, scatter to node lat_flow.
     *
     * @param ctx       SimulationContext (reads nodes.lat_flow, air_temp).
     * @param rainfall  Average rainfall intensity this step (depth/time).
     * @param air_temp  Air temperature this step (same units as params).
     * @param dt        Routing timestep (seconds).
     */
    void computeAll(SimulationContext& ctx, double rainfall,
                    double air_temp, double dt);

    int componentCount() const { return static_cast<int>(params_.size()); }

private:
    // Named parameter registry (from [AMM] component definitions)
    std::vector<AMMParams> params_;
    std::unordered_map<std::string, int> name_to_idx_;

    // Per-assignment runtime data
    struct Assignment {
        int node_idx  = -1;
        int param_idx = -1;
        double area   = 0.0;
    };
    std::vector<Assignment> assigns_;
    std::vector<AMMState>   states_;

    // Diagnostic snapshots
    std::vector<double> last_SHCF_;
    std::vector<double> last_RW_;
    std::vector<double> last_MAP_;

    // ---- Helper functions matching paper equations ----
    static double shapeFactor(double dt, double HHL);
    static double antecedentMoistureRetentionFactor(double dt, double AMHL);
    static double updateMovingAverage(std::vector<double>& buf, int& head,
                                       double new_value);
    static double computeSHCF(double MATemp,
                              double cold_SHCF, double hot_SHCF,
                              double cold_temp, double hot_temp);
    static double computeRW(double AMRF, double SHCF, double MAP,
                            double RW_prev);
    static double computeBaseflowR(double MATemp,
                                   double cold_R, double hot_R,
                                   double cold_temp, double hot_temp);
    static double computeQ(double area, double RD, double RW_t, double RW_prev,
                           double MAP, double SF, double dt,
                           double Q_prev, double area_to_flow);
};

} // namespace amm
} // namespace openswmm

#endif // OPENSWMM_AMM_HPP
