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
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
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
    std::vector<uint8_t> has_flap;     ///< Flap gate (uint8_t, not bool: avoids
                                       ///< vector<bool> bit-packing overhead in
                                       ///< per-iteration hot loops and enables
                                       ///< parallel-for writes without atomics).
    /// Most-recently-computed surface area (ft²) for scatter into node
    /// new_surf_area (matches legacy Orifice[k].surfArea).
    std::vector<double> surf_area;
    /// Equivalent length (ft) used for surface-area computation on SIDE
    /// orifices. Legacy: max(200, 2·routingStep·sqrt(g·yFull)).
    /// Pre-computed at init; constant per simulation.
    std::vector<double> length_eff;
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
    std::vector<uint8_t> has_flap;     ///< See comment on OrificeGroup::has_flap.
    /// Most-recently-computed surface area (ft²), populated by
    /// computeWeirFlows and scattered to node surface-area accumulators
    /// by the non_conduit_fn callback. Matches legacy Weir[k].surfArea.
    std::vector<double> surf_area;
    /// Effective length used for surface-area computation
    /// (legacy Weir[k].length = max(200, 2·routeStep·sqrt(g·yFull))).
    /// Pre-computed at init; constant per simulation.
    std::vector<double> length_eff;
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
     * @param node_new_surf_area
     *             Per-node surface area from the DW solver's xnode buffer.
     *             Read by computePumpFlows (pump flow-limiter, matching
     *             legacy getModPumpFlow) and written by computeWeirFlows
     *             (weir surface-area scatter, matching legacy
     *             findNonConduitSurfArea). Pass nullptr to skip both
     *             (pump falls back to MIN_SURFAREA; weir scatter is skipped).
     */
    void computeAllFlows(SimulationContext& ctx, double dt,
                         double* node_new_surf_area = nullptr);

    /// @brief Evaluate pump startup/shutoff depth hysteresis.
    ///        Must be called ONCE per timestep BEFORE the DW iteration loop,
    ///        matching legacy link_setTargetSetting() timing.
    void updatePumpTargetSettings(SimulationContext& ctx);

    /// @brief Get pre-built list of all non-conduit link indices.
    const std::vector<int>& nonConduitIndices() const noexcept { return nc_indices_; }

private:
    PumpGroup    pumps_;
    OrificeGroup orifices_;
    WeirGroup    weirs_;
    OutletGroup  outlets_;
    std::vector<int> nc_indices_;   ///< All non-conduit link indices

    /// Batch pump flow — vectorisable curve lookups. Reads node_new_surf_area
    /// for the non-storage pump flow-limiter (legacy getModPumpFlow).
    void computePumpFlows(SimulationContext& ctx, double dt,
                          const double* node_new_surf_area);

    /// Batch orifice flow — Q = Cd·A·sqrt(2gH), plus per-orifice
    /// surface-area scatter into `node_new_surf_area` (matching legacy
    /// findNonConduitSurfArea: BOTTOM orifice uses xsect_getAofY(xsect, y1);
    /// SIDE orifice uses xsect_getWofY(xsect, newDepth)·length_eff).
    /// When `node_new_surf_area` is null the scatter is skipped.
    void computeOrificeFlows(SimulationContext& ctx,
                             double* node_new_surf_area = nullptr);

    /// Batch weir flow — Q = Cd·L·H^expon, plus per-weir surface-area
    /// scatter into `node_new_surf_area` (matching legacy
    /// findNonConduitSurfArea). When `node_new_surf_area` is null the
    /// scatter is skipped; weir flows are still computed.
    void computeWeirFlows(SimulationContext& ctx,
                          double* node_new_surf_area = nullptr);

    /// Batch outlet flow — Q = coeff*H^expon or curve lookup
    void computeOutletFlows(SimulationContext& ctx);
};

} // namespace hydstruct
} // namespace openswmm

#endif // OPENSWMM_HYD_STRUCTURES_HPP
