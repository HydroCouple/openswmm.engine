/**
 * @file NodeData.hpp
 * @brief Structure-of-Arrays (SoA) storage for all node types.
 *
 * @details Replaces the global `Node[]` array and `TNode`/`TJunct`/`TOutfall`/
 *          `TStorage` structs from src/solver/objects.h with a data-oriented
 *          layout. Parallel arrays improve cache efficiency for the hydraulic
 *          solver, which typically touches a subset of fields across all nodes.
 *
 * Node types supported:
 * - JUNCTION   — standard junction node
 * - OUTFALL    — open boundary condition
 * - DIVIDER    — flow diversion node
 * - STORAGE    — storage unit (pond/tank)
 *
 * @see Legacy reference: src/solver/objects.h  — TNode, TJunct, TOutfall, TStorage
 * @see Legacy reference: src/solver/globals.h  — Node[], Nnodes[]
 * @ingroup engine_data
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_NODE_DATA_HPP
#define OPENSWMM_ENGINE_NODE_DATA_HPP

#include <vector>
#include <cstdint>
#include <string>

namespace openswmm {

// ============================================================================
// Node type enumeration
// ============================================================================

/**
 * @brief Node type codes.
 * @see Legacy: NodeType in src/solver/enums.h
 */
enum class NodeType : int8_t {
    JUNCTION = 0,
    OUTFALL  = 1,
    DIVIDER  = 2,
    STORAGE  = 3
};

/**
 * @brief Outfall boundary condition type.
 * @see Legacy: OutfallType in src/solver/enums.h
 */
enum class OutfallType : int8_t {
    FREE      = 0,
    NORMAL    = 1,
    FIXED     = 2,
    TIDAL     = 3,
    TIMESERIES = 4
};

/**
 * @brief Flow divider type.
 * @see Legacy: DividerType in src/solver/enums.h
 */
enum class DividerType : int8_t {
    CUTOFF         = 0,
    OVERFLOW_DIV   = 1,  ///< Renamed from OVERFLOW to avoid macOS math.h macro collision
    TABULAR        = 2,
    WEIR           = 3
};

// ============================================================================
// NodeData — SoA layout
// ============================================================================

/**
 * @brief Structure-of-Arrays storage for all nodes.
 *
 * @details All parallel arrays are indexed by node index [0, count).
 *          Use SimulationContext::node_names to translate name → index.
 *
 * Fields are divided into:
 * - **Static properties** (set during input parsing, not changed during sim)
 * - **State variables** (updated each timestep by the hydraulic solver)
 * - **Cumulative statistics** (totals updated each timestep)
 *
 * @ingroup engine_data
 */
struct NodeData {

    // -----------------------------------------------------------------------
    // Static properties — set at parse time
    // -----------------------------------------------------------------------

    /** @brief Node type for each node. */
    std::vector<NodeType>   type;

    /**
     * @brief Invert elevation (project length units).
     * @see Legacy: Node[i].invertElev
     */
    std::vector<double>     invert_elev;

    /**
     * @brief Full depth of the node (project length units).
     * @see Legacy: Node[i].fullDepth
     */
    std::vector<double>     full_depth;

    /**
     * @brief Initial water depth (project length units).
     * @see Legacy: Node[i].initDepth
     */
    std::vector<double>     init_depth;

    /**
     * @brief Maximum depth allowed at the node (ponding or surcharge limit).
     * @see Legacy: Node[i].surDepth
     */
    std::vector<double>     sur_depth;

    /**
     * @brief Ponding area at the surface (sq project length units).
     * @see Legacy: Node[i].pondedArea
     */
    std::vector<double>     ponded_area;

    // -----------------------------------------------------------------------
    // Outfall-specific properties (valid when type[i] == OUTFALL)
    // -----------------------------------------------------------------------

    /** @brief Outfall boundary condition type. */
    std::vector<OutfallType> outfall_type;

    /**
     * @brief Fixed outfall stage or tidal curve / time series index.
     * @details If outfall_type == FIXED, stores the fixed water surface elevation.
     *          If TIDAL or TIMESERIES, stores the index into TableData.
     */
    std::vector<double>     outfall_param;

    /** @brief True if the outfall has a gated flap. */
    std::vector<bool>       outfall_has_flap_gate;

    // -----------------------------------------------------------------------
    // Storage-specific properties (valid when type[i] == STORAGE)
    // -----------------------------------------------------------------------

    /**
     * @brief Storage curve index into TableData (CURVE_STORAGE).
     * @details -1 if storage uses functional relationship (a,b,c parameters).
     */
    std::vector<int>        storage_curve;

    /** @brief Curve name for deferred resolution (populated during parsing). */
    std::vector<std::string> storage_curve_name;

    /** @brief Functional storage area parameter A (area = A * depth^B + C). */
    std::vector<double>     storage_a;
    /** @brief Functional storage area parameter B. */
    std::vector<double>     storage_b;
    /** @brief Functional storage area parameter C (baseline area). */
    std::vector<double>     storage_c;

    /** @brief Seepage rate from storage node (project units/day). */
    std::vector<double>     storage_seep_rate;

    /** @brief Green-Ampt suction head for exfiltration (in or mm, converted to ft). */
    std::vector<double>     exfil_suction;
    /** @brief Green-Ampt saturated hydraulic conductivity for exfiltration (in/hr or mm/hr, converted to ft/sec). */
    std::vector<double>     exfil_ksat;
    /** @brief Green-Ampt initial moisture deficit for exfiltration (0-1). */
    std::vector<double>     exfil_imd;

    // -----------------------------------------------------------------------
    // Divider-specific properties (valid when type[i] == DIVIDER)
    // -----------------------------------------------------------------------

    /** @brief Divider method (DividerType enum value). */
    std::vector<DividerType> divider_type;

    /** @brief Cutoff flow for CUTOFF dividers. */
    std::vector<double>     divider_cutoff;

    /** @brief Weir discharge coefficient for WEIR dividers. */
    std::vector<double>     divider_cd;

    /** @brief Weir max depth for WEIR dividers. */
    std::vector<double>     divider_max_depth;

    /** @brief Diversion curve index for TABULAR dividers (-1 = none). */
    std::vector<int>        divider_curve;

    /** @brief Diversion link index (-1 = not set). */
    std::vector<int>        divider_link;

    /** @brief Diversion link name (for deferred resolution). */
    std::vector<std::string> divider_link_name;

    /** @brief Diversion curve name (for deferred resolution, TABULAR only). */
    std::vector<std::string> divider_curve_name;

    // -----------------------------------------------------------------------
    // State variables — updated each timestep
    // -----------------------------------------------------------------------

    /**
     * @brief Current water depth above invert (project length units).
     * @see Legacy: Node[i].newDepth
     */
    std::vector<double>     depth;

    /**
     * @brief Current water surface head (project length units = invert + depth).
     * @see Legacy: Node[i].newHead
     */
    std::vector<double>     head;

    /**
     * @brief Current water volume (project volume units).
     * @see Legacy: Node[i].newVolume
     */
    std::vector<double>     volume;

    /**
     * @brief Current lateral inflow (project flow units).
     * @see Legacy: Node[i].newLatFlow
     */
    std::vector<double>     lat_flow;

    /**
     * @brief User-forced lateral inflow set via the API (project flow units).
     *
     * Unlike lat_flow, this is not cleared between routing steps.  The
     * value persists until the user explicitly changes it and is added
     * to lat_flow at each routing step.
     */
    std::vector<double>     user_lat_flow;

    /**
     * @brief Current total inflow to the node (project flow units).
     * @see Legacy: Node[i].inflow
     */
    std::vector<double>     inflow;

    /**
     * @brief Current total outflow from the node (project flow units).
     * @see Legacy: Node[i].outflow
     */
    std::vector<double>     outflow;

    /**
     * @brief Current overflow / ponded flow (project flow units).
     * @see Legacy: Node[i].overflow
     */
    std::vector<double>     overflow;

    /**
     * @brief Node losses (evaporation + seepage) (project flow units).
     * @see Legacy: Node[i].losses
     */
    std::vector<double>     losses;

    /**
     * @brief Crown elevation — top of highest connecting conduit (project length units).
     * @see Legacy: Node[i].crownElev
     */
    std::vector<double>     crown_elev;

    /**
     * @brief Node degree — number of connecting links (+ve downstream, -ve upstream terminal).
     * @see Legacy: Node[i].degree
     */
    std::vector<int>        degree;

    /**
     * @brief Net inflow from previous timestep (inflow - outflow) for averaging.
     * @see Legacy: Node[i].oldNetInflow
     */
    std::vector<double>     old_net_inflow;

    /**
     * @brief Full volume at node (project volume units).
     * @see Legacy: Node[i].fullVolume
     */
    std::vector<double>     full_volume;

    // -----------------------------------------------------------------------
    // Previous-step state (for output interpolation / CFL checks)
    // -----------------------------------------------------------------------

    /** @brief Depth at the previous timestep. */
    std::vector<double>     old_depth;

    /** @brief Volume at the previous timestep. */
    std::vector<double>     old_volume;

    /** @brief Lateral flow at the previous timestep. */
    std::vector<double>     old_lat_flow;

    // -----------------------------------------------------------------------
    // Cumulative statistics
    // -----------------------------------------------------------------------

    /**
     * @brief Total volume of water lost as overflow (project volume units).
     * @see Legacy: NodeStats[i].volFlooded
     */
    std::vector<double>     stat_vol_flooded;

    /**
     * @brief Total duration the node was flooded (seconds).
     * @see Legacy: NodeStats[i].timeFlooded
     */
    std::vector<double>     stat_time_flooded;

    /**
     * @brief Maximum reported depth (project length units).
     * @see Legacy: NodeStats[i].maxDepth
     */
    std::vector<double>     stat_max_depth;

    /**
     * @brief Maximum reported overflow rate (project flow units).
     * @see Legacy: NodeStats[i].maxOverflow
     */
    std::vector<double>     stat_max_overflow;

    /**
     * @brief Outfall cumulative average flow (flow units × reporting periods).
     * @see Legacy: OutfallStats[k].avgFlow
     */
    std::vector<double>     stat_outfall_avg_flow;

    /// Maximum lateral inflow at each node (project flow units).
    /// @see Legacy: NodeStats[i].maxLatFlow
    std::vector<double>     stat_max_lat_inflow;

    /// Maximum total inflow at each node (project flow units).
    /// @see Legacy: NodeStats[i].maxInflow
    std::vector<double>     stat_max_total_inflow;

    /// Cumulative lateral inflow volume at each node (ft3).
    /// @see Legacy: NodeStats[i].totLatFlow
    std::vector<double>     stat_lat_inflow_vol;

    /// Cumulative total inflow volume at each node (ft3).
    std::vector<double>     stat_total_inflow_vol;

    /**
     * @brief Outfall maximum flow (project flow units).
     * @see Legacy: OutfallStats[k].maxFlow
     */
    std::vector<double>     stat_outfall_max_flow;

    /**
     * @brief Outfall number of non-zero flow periods.
     * @see Legacy: OutfallStats[k].totalPeriods
     */
    std::vector<long>       stat_outfall_periods;

    /**
     * @brief Cumulative pollutant loads at each node.
     * @details Flat 2D: [node * n_pollutants + p].  Only meaningful for outfall
     *          nodes.  Resized by resize_loads() after pollutant count is known.
     * @see Legacy: OutfallStats[k].totalLoad[p]
     */
    std::vector<double>     stat_total_load;
    int                     stat_n_pollutants = 0;

    // -----------------------------------------------------------------------
    // Capacity management
    // -----------------------------------------------------------------------

    /** @brief Number of nodes. */
    int count() const noexcept { return static_cast<int>(type.size()); }

    /**
     * @brief Resize all arrays to hold exactly `n` nodes.
     *
     * @details Called once during input parsing after the total node count is
     *          known. All numeric arrays are zero-initialized; type arrays use
     *          JUNCTION as the default.
     */
    void resize(int n) {
        const auto un = static_cast<std::size_t>(n);
        type.assign(un, NodeType::JUNCTION);
        invert_elev.assign(un, 0.0);
        full_depth.assign(un, 0.0);
        init_depth.assign(un, 0.0);
        sur_depth.assign(un, 0.0);
        ponded_area.assign(un, 0.0);

        outfall_type.assign(un, OutfallType::FREE);
        outfall_param.assign(un, 0.0);
        outfall_has_flap_gate.assign(un, false);

        storage_curve.assign(un, -1);
        storage_curve_name.resize(un);
        storage_a.assign(un, 0.0);
        storage_b.assign(un, 0.0);
        storage_c.assign(un, 0.0);
        storage_seep_rate.assign(un, 0.0);
        exfil_suction.assign(un, 0.0);
        exfil_ksat.assign(un, 0.0);
        exfil_imd.assign(un, 0.0);

        divider_type.assign(un, DividerType::CUTOFF);
        divider_cutoff.assign(un, 0.0);
        divider_cd.assign(un, 0.0);
        divider_max_depth.assign(un, 0.0);
        divider_curve.assign(un, -1);
        divider_link.assign(un, -1);
        divider_link_name.resize(un);
        divider_curve_name.resize(un);

        depth.assign(un, 0.0);
        head.assign(un, 0.0);
        volume.assign(un, 0.0);
        lat_flow.assign(un, 0.0);
        user_lat_flow.assign(un, 0.0);
        inflow.assign(un, 0.0);
        outflow.assign(un, 0.0);
        overflow.assign(un, 0.0);
        losses.assign(un, 0.0);
        crown_elev.assign(un, 0.0);
        degree.assign(un, 0);
        old_net_inflow.assign(un, 0.0);
        full_volume.assign(un, 0.0);
        old_depth.assign(un, 0.0);
        old_volume.assign(un, 0.0);
        old_lat_flow.assign(un, 0.0);

        stat_vol_flooded.assign(un, 0.0);
        stat_time_flooded.assign(un, 0.0);
        stat_max_depth.assign(un, 0.0);
        stat_max_overflow.assign(un, 0.0);
        stat_max_lat_inflow.assign(un, 0.0);
        stat_max_total_inflow.assign(un, 0.0);
        stat_lat_inflow_vol.assign(un, 0.0);
        stat_total_inflow_vol.assign(un, 0.0);
        stat_outfall_avg_flow.assign(un, 0.0);
        stat_outfall_max_flow.assign(un, 0.0);
        stat_outfall_periods.assign(un, 0);
    }

    /**
     * @brief Resize pollutant load arrays after pollutant count is known.
     */
    void resize_loads(int n_pollutants) {
        stat_n_pollutants = n_pollutants;
        if (n_pollutants > 0) {
            auto total = static_cast<std::size_t>(count()) *
                         static_cast<std::size_t>(n_pollutants);
            stat_total_load.assign(total, 0.0);
        }
    }

    /**
     * @brief Snapshot current state into old-step arrays before solving.
     */
    void save_state() noexcept {
        old_depth    = depth;
        old_volume   = volume;
        old_lat_flow = lat_flow;
        // Save net inflow for trapezoidal averaging in next step
        for (std::size_t i = 0; i < inflow.size(); ++i) {
            old_net_inflow[i] = inflow[i] - outflow[i];
        }
    }

    /** @brief Zero all state variables (for a cold start). */
    void reset_state() noexcept {
        const auto n = depth.size();
        std::fill(depth.begin(),    depth.end(),    0.0);
        std::fill(head.begin(),     head.end(),     0.0);
        std::fill(volume.begin(),   volume.end(),   0.0);
        std::fill(lat_flow.begin(), lat_flow.end(), 0.0);
        std::fill(inflow.begin(),   inflow.end(),   0.0);
        std::fill(outflow.begin(),  outflow.end(),  0.0);
        std::fill(overflow.begin(), overflow.end(), 0.0);
        std::fill(losses.begin(),   losses.end(),   0.0);
        std::fill(old_net_inflow.begin(), old_net_inflow.end(), 0.0);
        std::fill(old_depth.begin(),    old_depth.end(),    0.0);
        std::fill(old_volume.begin(),   old_volume.end(),   0.0);
        std::fill(old_lat_flow.begin(), old_lat_flow.end(), 0.0);
        (void)n;
    }
};

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_NODE_DATA_HPP */
