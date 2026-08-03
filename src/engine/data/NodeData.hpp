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
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_NODE_DATA_HPP
#define OPENSWMM_ENGINE_NODE_DATA_HPP

#include <vector>
#include <cstdint>
#include <string>
#include <algorithm>

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

/**
 * @brief Storage-unit surface-area relation.
 *
 * @details Values mirror legacy `enum StorageType` (src/legacy/engine/enums.h:833-847)
 *          ordinal-for-ordinal, so the int carried by the C API is the same code the
 *          legacy solver uses.
 *
 *          The relation determines how the StorageData a/b/c coefficients are read —
 *          they are **overloaded**:
 *            - FUNCTIONAL:            Area = c + a*d^b        (power law)
 *            - CYLINDRICAL..PYRAMIDAL: Area = c + a*d + b*d²   (quadratic)
 *          See StorageGeometry.hpp for the raw-parameter → coefficient mapping.
 *
 * @see Legacy: StorageType in src/legacy/engine/enums.h
 */
enum class StorageShape : int8_t {
    TABULAR     = 0,  ///< Area vs. depth from a curve (StorageData::curve)
    FUNCTIONAL  = 1,  ///< Area = c + a*d^b
    CYLINDRICAL = 2,  ///< Elliptical cylinder  (p1 = major axis, p2 = minor axis)
    CONICAL     = 3,  ///< Elliptical cone      (p1, p2 = base axes, p3 = side slope)
    PARABOLOID  = 4,  ///< Elliptical paraboloid(p1, p2 = top axes,  p3 = height ≠ 0)
    PYRAMIDAL   = 5   ///< Rectangular pyramid  (p1 = length, p2 = width, p3 = side slope)
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

    /**
     * @brief Virtual-junction flag (0 = regular node, 1 = virtual junction).
     *
     * @details A virtual junction is a zero-storage, momentum-transmitting
     *          JUNCTION connecting exactly two conduits of identical cross
     *          section (INP section [VIRTUAL_JUNCTIONS]). The NodeType stays
     *          JUNCTION so the binary .out type-code space is unchanged.
     *          Refactored engine only — the legacy engine has no support.
     */
    std::vector<uint8_t>    is_virtual;

    // -----------------------------------------------------------------------
    // Node subtype properties (storage / outfall / divider)
    // -----------------------------------------------------------------------
    // Relational node refactor (Phase 4): the wide per-node storage_* / outfall_*
    // / divider_* / exfil_* arrays that used to live here have been REMOVED. They
    // now live in the dense relational side-tables (StorageData / OutfallData /
    // DividerData in NodeSubtypes.hpp), sized to the count of each subtype and
    // joined to base nodes by node_idx — the memory win of the refactor. Only the
    // base `type[i]` discriminator remains on NodeData. See
    // docs/relational/RELATIONAL_NODE_REFACTOR_PLAN.md.

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

    // -----------------------------------------------------------------------
    // Decomposed lateral inflow sources
    // Each source process writes to its own buffer.
    // assembleLateralInflows() sums them all into lat_flow.
    // -----------------------------------------------------------------------

    /** @brief Interpolated surface runoff from subcatchments (project flow units). */
    std::vector<double>     runoff_inflow;

    /** @brief Interpolated groundwater flow from subcatchments (project flow units). */
    std::vector<double>     gw_inflow;

    /** @brief External (timeseries/baseline) inflows (project flow units). */
    std::vector<double>     ext_inflow;

    /** @brief Dry weather flow inflows (project flow units). */
    std::vector<double>     dwf_inflow;

    /** @brief RDII unit hydrograph inflows (project flow units). */
    std::vector<double>     rdii_inflow;

    /** @brief Interface file (upstream model coupling) inflows (project flow units). */
    std::vector<double>     iface_inflow;

    /**
     * @brief 2D ↔ 1D coupling exchange flow at the node (project flow units).
     *
     * @details Signed: positive = 2D → 1D (surface drainage into the node),
     *          negative = 1D → 2D (surcharge spill onto the surface).
     *          Written by SurfaceRouter2D::advancePostRouting at the end of
     *          step N; read by assembleLateralInflows at the start of step
     *          N+1 (so the value persists across the step boundary and is
     *          consumed exactly once by the DW solver).
     *
     *          For mass-balance accounting the signed value is split:
     *          - positive side folds into step_ext_inflow → routing_external
     *          - negative side folds into routing_flooding (absolute value)
     *
     *          This replaces the earlier scheme of routing coupling Q through
     *          forcing.node_lat_inflow_value, which conflated user forcing
     *          with 2D coupling and dropped the negative (1D→2D) volume from
     *          the mass balance entirely.
     *
     * @see docs/1D_2D_COUPLING_GATE_REVIEW.md §11
     */
    std::vector<double>     coupling_inflow;

    /**
     * @brief Pending 1D↔2D exchange VOLUME for the next routing step (1D units, ft³).
     *
     * @details The exchange is carried across the staggered-coupling step boundary
     *          as a volume (not a rate) so it stays mass-conservative under
     *          VARIABLE_STEP: the producer (computeCouplingExchange / the live-RHS
     *          booking) writes Q·dt here at the end of step N; the consumer
     *          (assembleLateralInflows) re-derives the rate coupling_inflow =
     *          coupling_volume/dt at the start of step N+1 and clears this, so the
     *          1D node receives exactly the volume 2D exchanged regardless of how
     *          the timestep changes. Signed: + = 2D→1D drain, − = 1D→2D spill.
     *          In-memory only (not serialized to hotstart). See plan / review §11.
     */
    std::vector<double>     coupling_volume;

    /**
     * @brief Delivery queue for the 1D↔2D junction exchange (1D units, ft³).
     *
     * @details SurfaceRouter2D::fireAdvanceWindow moves each fired window's
     *          coupling_volume here after the 2D mass-balance ledger books it;
     *          assembleLateralInflows drains the queue at the uniform rate
     *          queue / SimulationContext::coupling_delivery_remaining, so a
     *          multi-step advance window's exchange volume arrives spread over
     *          the following routing steps instead of as a single-step pulse
     *          (window/routing-step × the physical rate — which flooded small
     *          junctions instantly and drove a per-window drain/spill churn).
     *          Degenerates exactly to the legacy one-step delivery when the 2D
     *          advance fires every routing step (delivery window ≤ routing
     *          step). Signed like coupling_volume: + = 2D→1D drain, − = 1D→2D
     *          spill. In-memory only (not serialized to hotstart).
     */
    std::vector<double>     coupling_queue;

    // -----------------------------------------------------------------------
    // Quality mass inflow assembly arrays
    // assembleQualityInflows() writes these; mixAtNodes() reads them.
    // -----------------------------------------------------------------------

    /** @brief Accumulated quality mass inflow rate per (node, pollutant).
     *  @details Flat 2D: [node * n_pollutants + pollutant]. Units: mass/sec. */
    std::vector<double>     qual_mass_in;

    /** @brief Accumulated volume inflow rate per node (ft3/sec). */
    std::vector<double>     qual_vol_in;

    /**
     * @brief Routing interface file quality mass rate per (node, pollutant) (mass/sec).
     * @details Written by iface::InterfaceManager::readInflows() each routing
     *          step (cleared in clearInflowSources()); read by
     *          QualitySolver::addIfaceLoads() → added to qual_mass_in.
     *          Flat 2D: [node * n_pollutants + pollutant].
     * @see Legacy: routing.c addIfaceInflows() quality portion
     */
    std::vector<double>     iface_qual_mass;

    /**
     * @brief LID drain quality mass rate per (node, pollutant) (mass/sec).
     * @details Set once per runoff step (cleared at runoff step start); read
     *          each routing step by addWetWeatherLoads() → added to qual_mass_in.
     *          Flat 2D: [node * n_pollutants + pollutant].
     *          Covers drain-to-node and drain-to-subcatch (routed to outlet node).
     *          Matches legacy lid_addDrainInflow() / lid_addDrainRunon() quality.
     * @see Legacy: lid.c lid_addDrainInflow(), lid_addDrainRunon()
     */
    std::vector<double>     lid_drain_qual_load;

    /**
     * @brief LID drain volume inflow rate per node (ft3/sec).
     * @details Set once per runoff step; read each routing step by
     *          addWetWeatherLoads() → added to qual_vol_in (denominator for mixing).
     * @see Legacy: lid.c lid_addDrainInflow() Node[k].newLatFlow contribution
     */
    std::vector<double>     lid_drain_qual_vol;

    // -----------------------------------------------------------------------
    // Per-node quality state — flat 2D: [node * n_pollutants + pollutant]
    // -----------------------------------------------------------------------

    /**
     * @brief Current quality concentration at each node.
     * @details Size = n_nodes * n_pollutants.
     * @see Legacy: Node[i].newQual[]
     */
    std::vector<double>     conc;

    /** @brief Previous-step quality at each node. */
    std::vector<double>     conc_old;

    /** @brief Hydraulic residence time for storage nodes (seconds). */
    std::vector<double>     hrt;

    /**
     * @brief User-forced quality mass flux at each node (mass/sec).
     *
     * @details Flat 2D: [node * n_pollutants + pollutant].
     *          Unlike the transient forcing in ForcingData, this persists
     *          until the user explicitly changes it and is applied as an
     *          additive mass source at each routing step (analogous to
     *          user_lat_flow for flow).
     */
    std::vector<double>     user_conc_mass_flux;

    /** @brief Number of pollutants in the quality arrays. */
    int                     conc_n_pollutants = 0;

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

    /**
     * @brief Total inflow at the previous timestep (for output interpolation).
     * @see Legacy: Node[i].oldFlowInflow (node.c:294, node_getResults node.c:491)
     */
    std::vector<double>     old_inflow;

    // -----------------------------------------------------------------------
    // Per-object INP comment
    // -----------------------------------------------------------------------

    /**
     * @brief Object comment from the INP file (lines with a single ';' prefix
     *        immediately above this object's data row).
     *
     * @details Multiple comment lines are joined by the literal two-character
     *          token "\\n" (backslash + n).  Empty string means no comment.
     *          Written back to INP by InpWriter as one ';'-prefixed row per
     *          part.  Also stored verbatim in the GeoPackage 'comment' column.
     */
    std::vector<std::string> comments;

    /**
     * @brief Per-object tag from the INP `[TAGS]` section.
     *
     * @details Free-form string label, used by GUIs for filtering and grouping
     *          (e.g. catchment-name labels, asset IDs, user-defined groups).
     *          Empty string means no tag. Written back to INP by InpWriter as
     *          a `Node <name> <tag>` row in `[TAGS]`. Index-keyed (per-`NodeData`
     *          field) so `swmm_node_rename` keeps the tag attached — the
     *          earlier name-keyed `SimulationContext::node_tags` map lost
     *          tags on rename.
     */
    std::vector<std::string> tags;

    // -----------------------------------------------------------------------
    // Report flag — per-object output filter
    // -----------------------------------------------------------------------

    /** @brief Whether this node is included in report/output (0=no, 1=yes).
     *  @see Legacy: Node[j].rptFlag */
    std::vector<char>       rpt_flag;

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

    /** @brief Date/time when maximum overflow occurred (OADate (days since 12/30/1899)). */
    std::vector<double>     stat_max_overflow_date;

    /// Cumulative depth for computing average (project length units × seconds).
    /// @see Legacy: NodeStats[i].avgDepth
    std::vector<double>     stat_sum_depth;

    /// Cumulative stored volume for computing average (internal ft³), storage nodes
    /// only. Accumulated per routing step alongside stat_sum_depth so the Storage
    /// Volume Summary's average is the true time-average of the node's nonlinear
    /// volume, not volume-of-the-average-depth (which understates a convex curve).
    /// @see Legacy: StorageStats[k].avgVol
    std::vector<double>     stat_sum_volume;

    /// Date/time when maximum depth occurred (OADate (days since 12/30/1899)).
    /// @see Legacy: NodeStats[i].maxDepthDate
    std::vector<double>     stat_max_depth_date;

    /// Maximum reported depth (used for output-step max tracking).
    /// @see Legacy: NodeStats[i].maxRptDepth
    std::vector<double>     stat_max_rpt_depth;

    /// Date/time when maximum total inflow occurred (OADate (days since 12/30/1899)).
    /// @see Legacy: NodeStats[i].maxInflowDate
    std::vector<double>     stat_max_inflow_date;

    /// Total time node was surcharged (seconds).
    /// @see Legacy: NodeStats[i].timeSurcharged
    std::vector<double>     stat_time_surcharged;

    /// Maximum surcharge height above crown (project length units).
    /// @see Legacy: NodeStats[i].maxDepth - Node[i].fullDepth
    std::vector<double>     stat_max_surcharge_height;

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

    /// Cumulative total outflow volume at each node (ft3).
    /// @see Legacy: NodeOutflow[j]
    std::vector<double>     stat_total_outflow_vol;

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
     * @brief Count of non-converging steps per node.
     * @details Incremented when a routing step fails to converge and the node
     *          itself did not converge (matching legacy NodeStats[i].nonConvergedCount).
     * @see Legacy: stats_updateConvergenceStats()
     */
    std::vector<int>        stat_non_converged_count;

    /**
     * @brief CFL time-step critical count per node.
     * @details Incremented when the node's depth-change rate produces the
     *          smallest CFL-limited timestep (matching legacy
     *          NodeStats[i].timeCourantCritical).
     * @see Legacy: stats_updateCriticalTimeCount()
     */
    std::vector<double>     stat_time_courant_critical;

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
        is_virtual.assign(un, 0);

        // Subtype config (storage/outfall/divider) lives in NodeSubtypes side-tables.
        depth.assign(un, 0.0);
        head.assign(un, 0.0);
        volume.assign(un, 0.0);
        lat_flow.assign(un, 0.0);
        user_lat_flow.assign(un, 0.0);
        runoff_inflow.assign(un, 0.0);
        gw_inflow.assign(un, 0.0);
        ext_inflow.assign(un, 0.0);
        dwf_inflow.assign(un, 0.0);
        rdii_inflow.assign(un, 0.0);
        iface_inflow.assign(un, 0.0);
        coupling_inflow.assign(un, 0.0);
        coupling_volume.assign(un, 0.0);
        coupling_queue.assign(un, 0.0);
        qual_mass_in.clear();
        iface_qual_mass.clear();
        qual_vol_in.assign(un, 0.0);
        lid_drain_qual_load.clear();
        lid_drain_qual_vol.assign(un, 0.0);
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
        old_inflow.assign(un, 0.0);

        comments.assign(un, std::string{});
        tags.assign(un, std::string{});

        rpt_flag.assign(un, 0);

        stat_vol_flooded.assign(un, 0.0);
        stat_time_flooded.assign(un, 0.0);
        stat_max_depth.assign(un, 0.0);
        stat_max_overflow.assign(un, 0.0);
        stat_max_overflow_date.assign(un, 0.0);
        stat_sum_depth.assign(un, 0.0);
        stat_sum_volume.assign(un, 0.0);
        stat_max_depth_date.assign(un, 0.0);
        stat_max_rpt_depth.assign(un, 0.0);
        stat_max_inflow_date.assign(un, 0.0);
        stat_time_surcharged.assign(un, 0.0);
        stat_max_surcharge_height.assign(un, 0.0);
        stat_max_lat_inflow.assign(un, 0.0);
        stat_max_total_inflow.assign(un, 0.0);
        stat_lat_inflow_vol.assign(un, 0.0);
        stat_total_inflow_vol.assign(un, 0.0);
        stat_total_outflow_vol.assign(un, 0.0);
        stat_outfall_avg_flow.assign(un, 0.0);
        stat_outfall_max_flow.assign(un, 0.0);
        stat_outfall_periods.assign(un, 0);
        stat_non_converged_count.assign(un, 0);
        stat_time_courant_critical.assign(un, 0.0);
    }

    /**
     * @brief Grow all arrays to hold at least `n` nodes, preserving existing data.
     *
     * @details Called by ensure_node_capacity() during incremental INP parsing.
     *          Uses vector::resize() (not assign) so existing elements are preserved.
     *          New elements get the same defaults as resize().
     */
    void grow_to(int n) {
        if (n <= count()) return;
        const auto un = static_cast<std::size_t>(n);
        auto g = [&](auto& vec, auto def) { vec.resize(un, def); };
        g(type, NodeType::JUNCTION);
        g(invert_elev, 0.0); g(full_depth, 0.0); g(init_depth, 0.0);
        g(sur_depth, 0.0); g(ponded_area, 0.0);
        g(is_virtual, static_cast<uint8_t>(0));
        // Subtype config (storage/outfall/divider) lives in NodeSubtypes side-tables.
        g(depth, 0.0); g(head, 0.0); g(volume, 0.0);
        g(lat_flow, 0.0); g(user_lat_flow, 0.0);
        g(runoff_inflow, 0.0); g(gw_inflow, 0.0); g(ext_inflow, 0.0);
        g(dwf_inflow, 0.0); g(rdii_inflow, 0.0); g(iface_inflow, 0.0);
        g(coupling_inflow, 0.0); g(coupling_volume, 0.0); g(coupling_queue, 0.0);
        qual_vol_in.resize(un, 0.0);
        lid_drain_qual_vol.resize(un, 0.0);
        g(inflow, 0.0); g(outflow, 0.0); g(overflow, 0.0);
        g(losses, 0.0); g(crown_elev, 0.0); g(degree, 0);
        g(old_net_inflow, 0.0); g(full_volume, 0.0);
        g(old_depth, 0.0); g(old_volume, 0.0); g(old_lat_flow, 0.0);
        g(old_inflow, 0.0);
        comments.resize(un, std::string{});
        tags.resize(un, std::string{});

        g(rpt_flag, static_cast<char>(0));
        g(stat_vol_flooded, 0.0); g(stat_time_flooded, 0.0);
        g(stat_max_depth, 0.0); g(stat_max_overflow, 0.0);
        g(stat_max_overflow_date, 0.0); g(stat_sum_depth, 0.0);
        g(stat_sum_volume, 0.0);
        g(stat_max_depth_date, 0.0); g(stat_max_rpt_depth, 0.0);
        g(stat_max_inflow_date, 0.0); g(stat_time_surcharged, 0.0);
        g(stat_max_surcharge_height, 0.0);
        g(stat_max_lat_inflow, 0.0); g(stat_max_total_inflow, 0.0);
        g(stat_lat_inflow_vol, 0.0); g(stat_total_inflow_vol, 0.0);
        g(stat_total_outflow_vol, 0.0);
        g(stat_outfall_avg_flow, 0.0); g(stat_outfall_max_flow, 0.0);
        g(stat_outfall_periods, 0L);
        g(stat_non_converged_count, 0); g(stat_time_courant_critical, 0.0);
        // Note: qual_mass_in, conc, conc_old, hrt handled by resize_quality()
    }

    /**
     * @brief Erase the node at index `idx` from every parallel array.
     *
     * @details Removes the element at `idx` from every SoA vector. For flat-2D
     *          quality arrays indexed as [node * n_pollutants + p], the full
     *          stride for `idx` is removed. Spatial arrays are NOT touched here;
     *          ObjectDeleter erases spatial data separately after calling this.
     *          Only call in BUILDING or OPENED state.
     */
    void erase_at(int idx) {
        const auto ui = static_cast<std::size_t>(idx);
        auto e = [&](auto& v) { if (ui < v.size()) v.erase(v.begin() + static_cast<std::ptrdiff_t>(idx)); };

        e(type); e(invert_elev); e(full_depth); e(init_depth); e(sur_depth); e(ponded_area);
        e(is_virtual);

        // Subtype config (storage/outfall/divider) lives in NodeSubtypes side-tables;
        // its rows are erased/renumbered by NodeSubtypes::erase_node (called by the
        // node-delete path), not here.
        e(depth); e(head); e(volume);
        e(lat_flow); e(user_lat_flow);
        e(runoff_inflow); e(gw_inflow); e(ext_inflow); e(dwf_inflow);
        e(rdii_inflow); e(iface_inflow);
        e(coupling_inflow); e(coupling_volume); e(coupling_queue);
        e(qual_vol_in); e(lid_drain_qual_vol);
        e(inflow); e(outflow); e(overflow); e(losses);
        e(crown_elev); e(degree); e(old_net_inflow); e(full_volume);
        e(old_depth); e(old_volume); e(old_lat_flow); e(old_inflow);
        e(comments); e(tags); e(rpt_flag);

        e(stat_vol_flooded); e(stat_time_flooded); e(stat_max_depth); e(stat_max_overflow);
        e(stat_max_overflow_date); e(stat_sum_depth); e(stat_sum_volume); e(stat_max_depth_date);
        e(stat_max_rpt_depth); e(stat_max_inflow_date); e(stat_time_surcharged);
        e(stat_max_surcharge_height); e(stat_outfall_avg_flow); e(stat_max_lat_inflow);
        e(stat_max_total_inflow); e(stat_lat_inflow_vol); e(stat_total_inflow_vol);
        e(stat_total_outflow_vol); e(stat_outfall_max_flow); e(stat_outfall_periods);
        e(stat_non_converged_count); e(stat_time_courant_critical);

        // Flat 2D quality arrays: [node * np + p] → erase the stride for idx
        if (conc_n_pollutants > 0) {
            const auto np = static_cast<std::size_t>(conc_n_pollutants);
            const auto base = ui * np;
            auto erase2d = [&](auto& v) {
                if (base + np <= v.size())
                    v.erase(v.begin() + static_cast<std::ptrdiff_t>(base),
                            v.begin() + static_cast<std::ptrdiff_t>(base + np));
            };
            erase2d(conc); erase2d(conc_old);
            erase2d(qual_mass_in); erase2d(iface_qual_mass);
            erase2d(lid_drain_qual_load); erase2d(user_conc_mass_flux);
            if (ui < hrt.size()) hrt.erase(hrt.begin() + static_cast<std::ptrdiff_t>(idx));
        }

        // Flat 2D stat load: [node * np + p]
        if (stat_n_pollutants > 0) {
            const auto np = static_cast<std::size_t>(stat_n_pollutants);
            const auto base = ui * np;
            if (base + np <= stat_total_load.size())
                stat_total_load.erase(
                    stat_total_load.begin() + static_cast<std::ptrdiff_t>(base),
                    stat_total_load.begin() + static_cast<std::ptrdiff_t>(base + np));
        }
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
     * @brief Resize per-node quality arrays after pollutant count is known.
     */
    void resize_quality(int n_pollutants) {
        conc_n_pollutants = n_pollutants;
        if (n_pollutants > 0) {
            auto total = static_cast<std::size_t>(count()) *
                         static_cast<std::size_t>(n_pollutants);
            conc.assign(total, 0.0);
            conc_old.assign(total, 0.0);
            hrt.assign(static_cast<std::size_t>(count()), 0.0);
            user_conc_mass_flux.assign(total, 0.0);
            qual_mass_in.assign(total, 0.0);
            iface_qual_mass.assign(total, 0.0);
            lid_drain_qual_load.assign(total, 0.0);
        }
    }

    /**
     * @brief Release excess vector capacity accumulated during parsing.
     * @details Called once after final sizing is complete. Each vector's
     *          capacity is reduced to match its size, freeing memory that
     *          was over-allocated by geometric growth during incremental
     *          parsing.
     */
    void shrink_to_fit() {
        type.shrink_to_fit();
        invert_elev.shrink_to_fit();
        full_depth.shrink_to_fit();
        init_depth.shrink_to_fit();
        sur_depth.shrink_to_fit();
        ponded_area.shrink_to_fit();
        is_virtual.shrink_to_fit();

        // Subtype config (storage/outfall/divider) lives in NodeSubtypes side-tables.
        depth.shrink_to_fit();
        head.shrink_to_fit();
        volume.shrink_to_fit();
        lat_flow.shrink_to_fit();
        user_lat_flow.shrink_to_fit();
        runoff_inflow.shrink_to_fit();
        gw_inflow.shrink_to_fit();
        ext_inflow.shrink_to_fit();
        dwf_inflow.shrink_to_fit();
        rdii_inflow.shrink_to_fit();
        iface_inflow.shrink_to_fit();
        coupling_inflow.shrink_to_fit();
        coupling_volume.shrink_to_fit();
        coupling_queue.shrink_to_fit();
        qual_mass_in.shrink_to_fit();
        iface_qual_mass.shrink_to_fit();
        qual_vol_in.shrink_to_fit();
        conc.shrink_to_fit();
        conc_old.shrink_to_fit();
        user_conc_mass_flux.shrink_to_fit();
        inflow.shrink_to_fit();
        outflow.shrink_to_fit();
        overflow.shrink_to_fit();
        losses.shrink_to_fit();
        crown_elev.shrink_to_fit();
        degree.shrink_to_fit();
        old_net_inflow.shrink_to_fit();
        full_volume.shrink_to_fit();
        old_depth.shrink_to_fit();
        old_volume.shrink_to_fit();
        old_lat_flow.shrink_to_fit();
        old_inflow.shrink_to_fit();

        comments.shrink_to_fit();
        tags.shrink_to_fit();

        rpt_flag.shrink_to_fit();

        stat_vol_flooded.shrink_to_fit();
        stat_time_flooded.shrink_to_fit();
        stat_max_depth.shrink_to_fit();
        stat_max_overflow.shrink_to_fit();
        stat_max_overflow_date.shrink_to_fit();
        stat_sum_depth.shrink_to_fit();
        stat_max_depth_date.shrink_to_fit();
        stat_max_rpt_depth.shrink_to_fit();
        stat_max_inflow_date.shrink_to_fit();
        stat_time_surcharged.shrink_to_fit();
        stat_max_surcharge_height.shrink_to_fit();
        stat_max_lat_inflow.shrink_to_fit();
        stat_max_total_inflow.shrink_to_fit();
        stat_lat_inflow_vol.shrink_to_fit();
        stat_total_inflow_vol.shrink_to_fit();
        stat_total_outflow_vol.shrink_to_fit();
        stat_outfall_avg_flow.shrink_to_fit();
        stat_outfall_max_flow.shrink_to_fit();
        stat_outfall_periods.shrink_to_fit();
        stat_total_load.shrink_to_fit();
    }

    /**
     * @brief Snapshot current state into old-step arrays before solving.
     */
    void save_state() noexcept {
        std::copy(depth.begin(),    depth.end(),    old_depth.begin());
        std::copy(volume.begin(),   volume.end(),   old_volume.begin());
        std::copy(lat_flow.begin(), lat_flow.end(), old_lat_flow.begin());
        // Legacy node_setOldHydState (node.c:294): oldFlowInflow = inflow
        std::copy(inflow.begin(),   inflow.end(),   old_inflow.begin());
        // Save net inflow for trapezoidal averaging in next step
        for (std::size_t i = 0; i < inflow.size(); ++i) {
            old_net_inflow[i] = inflow[i] - outflow[i];
        }
        std::copy(conc.begin(), conc.end(), conc_old.begin());
    }

    /** @brief Reset state variables, applying init_depth from input.
     *
     * @details Matches legacy node_initState() which sets oldDepth = initDepth
     *          and computes initial volume from depth. Flows and inflows are
     *          zeroed for a cold start.
     */
    void reset_state() noexcept {
        const auto n = depth.size();
        // Apply initial depths from input (matching legacy node_initState)
        for (std::size_t i = 0; i < n; ++i) {
            depth[i]     = init_depth[i];
            old_depth[i] = init_depth[i];
            head[i]      = invert_elev[i] + init_depth[i];
        }
        // Volumes must be computed from init_depth by caller (needs table data)
        std::fill(volume.begin(),   volume.end(),   0.0);
        std::fill(old_volume.begin(), old_volume.end(), 0.0);
        // Zero flows
        std::fill(lat_flow.begin(), lat_flow.end(), 0.0);
        std::fill(inflow.begin(),   inflow.end(),   0.0);
        std::fill(outflow.begin(),  outflow.end(),  0.0);
        std::fill(overflow.begin(), overflow.end(), 0.0);
        std::fill(losses.begin(),   losses.end(),   0.0);
        std::fill(old_net_inflow.begin(), old_net_inflow.end(), 0.0);
        std::fill(old_lat_flow.begin(), old_lat_flow.end(), 0.0);
        std::fill(old_inflow.begin(), old_inflow.end(), 0.0);
        // coupling_inflow / coupling_volume / coupling_queue are NOT cleared by
        // clearInflowSources (coupling_volume and coupling_queue are the
        // carry-across-steps fields — their end-of-step values must persist into
        // the next step's assembly), so zero them explicitly on cold start.
        std::fill(coupling_inflow.begin(), coupling_inflow.end(), 0.0);
        std::fill(coupling_volume.begin(), coupling_volume.end(), 0.0);
        std::fill(coupling_queue.begin(), coupling_queue.end(), 0.0);
        clearInflowSources();
        std::fill(conc.begin(), conc.end(), 0.0);
        std::fill(conc_old.begin(), conc_old.end(), 0.0);
        (void)n;
    }

    /**
     * @brief Zero routing-phase inflow source arrays.
     * @details Called at the start of each routing step before processes
     *          write to their respective source arrays.
     *
     *          NOTE: runoff_inflow and gw_inflow are zeroed in stepRunoff()
     *          Phase 2 before subcatchment accumulation (matching legacy
     *          initSystemInflows which zeros newLatFlow each routing step).
     */
    void clearInflowSources() noexcept {
        // runoff_inflow and gw_inflow: zeroed in stepRunoff() Phase 2
        std::fill(ext_inflow.begin(),    ext_inflow.end(),    0.0);
        std::fill(dwf_inflow.begin(),    dwf_inflow.end(),    0.0);
        std::fill(rdii_inflow.begin(),   rdii_inflow.end(),   0.0);
        std::fill(iface_inflow.begin(),  iface_inflow.end(),  0.0);
        std::fill(iface_qual_mass.begin(), iface_qual_mass.end(), 0.0);
        std::fill(qual_mass_in.begin(),  qual_mass_in.end(),  0.0);
        std::fill(qual_vol_in.begin(),   qual_vol_in.end(),   0.0);
    }
};

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_NODE_DATA_HPP */
