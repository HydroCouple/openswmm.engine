/**
 * @file HydStructures.hpp
 * @brief Non-conduit link flow: pumps, orifices, weirs, outlets.
 *
 * @details Each structure type is computed in batch over all links of that
 *          type (similar to XSectGroups shape grouping). The flow equations
 *          are pure arithmetic — vectorisable inner loops.
 *
 *          **Batch architecture:**
 *          1. Group links by type at init (like XSectGroups)
 *          2. For each type group: gather head/depth → batch flow calc → scatter
 *
 *          **Pump types:**
 *            Type 1: volume-based curve
 *            Type 2: depth-based curve
 *            Type 3: head-based with speed setting
 *            Type 4: depth-based with dQ/dH
 *            Ideal:  demand-driven
 *
 *          **Orifice:** Q = Cd*A*sqrt(2gH), weir transition at partial fill
 *          **Weir:** Q = Cd*L*H^1.5 (transverse), variants for side/V-notch/trap
 *          **Outlet:** Q = coeff*H^expon or rating curve lookup
 *
 * @note Legacy reference: src/legacy/engine/link.c (pump/orifice/weir/outlet sections)
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_HYD_STRUCTURES_HPP
#define OPENSWMM_HYD_STRUCTURES_HPP

#include "../data/LinkData.hpp"
#include "../data/NodeData.hpp"
#include "../data/TableData.hpp"
#include <vector>

namespace openswmm {

struct SimulationContext;

namespace hydstruct {

// ============================================================================
// Constants
// ============================================================================

constexpr double GRAVITY = 32.2;
constexpr double FUDGE   = 0.0001;

// ============================================================================
// Per-type SoA groups (like XSectGroups but for structure types)
// ============================================================================

struct PumpGroup {
    int count = 0;
    std::vector<int>    link_idx;      ///< Global link index
    std::vector<int>    curve_idx;     ///< Pump curve table index
    std::vector<int>    curve_type;    ///< Pump type (1-5, 6=ideal)
    std::vector<double> speed;         ///< Current speed setting [0-1]
    std::vector<double> y_on;          ///< Startup depth
    std::vector<double> y_off;         ///< Shutoff depth
    void resize(int n);
};

struct OrificeGroup {
    int count = 0;
    std::vector<int>    link_idx;
    std::vector<int>    shape;         ///< BOTTOM or SIDE
    std::vector<double> c_orifice;     ///< Cd * sqrt(2g) * Area
    std::vector<double> c_weir;        ///< Cd * L * sqrt(2g) for partial fill
    std::vector<double> h_crit;        ///< Transition depth
    std::vector<bool>   has_flap;      ///< Flap gate
    void resize(int n);
};

struct WeirGroup {
    int count = 0;
    std::vector<int>    link_idx;
    std::vector<int>    weir_type;     ///< TRANSVERSE/SIDE/VNOTCH/TRAPEZOIDAL
    std::vector<double> c_disch1;      ///< Main discharge coefficient
    std::vector<double> c_disch2;      ///< End section coefficient (trapezoidal)
    std::vector<double> end_con;       ///< End contraction factor
    std::vector<double> slope;         ///< V-notch slope or trap slope
    std::vector<int>    cd_curve;      ///< Optional Cd(head) curve index
    std::vector<bool>   has_flap;
    void resize(int n);
};

struct OutletGroup {
    int count = 0;
    std::vector<int>    link_idx;
    std::vector<int>    curve_idx;     ///< Rating curve (-1 = power law)
    std::vector<double> q_coeff;       ///< Power law coefficient
    std::vector<double> q_expon;       ///< Power law exponent
    void resize(int n);
};

// ============================================================================
// Structure flow solver
// ============================================================================

class StructureSolver {
public:
    void init(SimulationContext& ctx);

    /**
     * @brief Compute flow for all non-conduit links (batch by type).
     *
     * @details For each type group:
     *   1. Gather upstream/downstream heads from node arrays
     *   2. Batch compute flow using type-specific equation
     *   3. Scatter flow + dQ/dH back to link arrays
     *
     * @param ctx  Simulation context.
     * @param dt   Timestep (seconds).
     */
    void computeAllFlows(SimulationContext& ctx, double dt);

    /// @brief Get pre-built list of all non-conduit link indices.
    const std::vector<int>& nonConduitIndices() const noexcept { return nc_indices_; }

private:
    PumpGroup    pumps_;
    OrificeGroup orifices_;
    WeirGroup    weirs_;
    OutletGroup  outlets_;
    std::vector<int> nc_indices_;   ///< All non-conduit link indices

    /// Batch pump flow — vectorisable curve lookups
    void computePumpFlows(SimulationContext& ctx, double dt);

    /// Batch orifice flow — Q = Cd*A*sqrt(2gH), vectorisable
    void computeOrificeFlows(SimulationContext& ctx);

    /// Batch weir flow — Q = Cd*L*H^expon, vectorisable
    void computeWeirFlows(SimulationContext& ctx);

    /// Batch outlet flow — Q = coeff*H^expon or curve lookup
    void computeOutletFlows(SimulationContext& ctx);
};

} // namespace hydstruct
} // namespace openswmm

#endif // OPENSWMM_HYD_STRUCTURES_HPP
