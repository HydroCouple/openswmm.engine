/**
 * @file SimulationSnapshot.hpp
 * @brief Read-only snapshot of simulation state passed to plugins.
 *
 * @details A SimulationSnapshot is a deep copy of the relevant SoA arrays
 *          from SimulationContext at an output time boundary. Plugins receive
 *          a const reference to the snapshot from the IO thread.
 *
 *          Because the snapshot is a separate copy, plugins can safely read
 *          all fields without any mutex or synchronization. The main simulation
 *          thread continues advancing while the IO thread processes the snapshot.
 *
 * @note The snapshot is allocated on the heap and managed by the IOThread's
 *       ring buffer. Do NOT store pointers to snapshot data beyond the
 *       scope of IOutputPlugin::update() or IReportPlugin::update().
 *
 * @ingroup engine_plugins
 * @see IOThread.hpp (in src/engine/output/)
 * @see IOutputPlugin.hpp
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_SIMULATION_SNAPSHOT_HPP
#define OPENSWMM_SIMULATION_SNAPSHOT_HPP

#include <vector>
#include <string>
#include <cstdint>

namespace openswmm {

/**
 * @brief Snapshot of node state at an output time step.
 * @ingroup engine_plugins
 */
struct NodeSnapshot {
    std::vector<double> depth;          ///< Water depth [project length units]
    std::vector<double> head;           ///< Hydraulic head [project length units]
    std::vector<double> volume;         ///< Stored volume [project volume units]
    std::vector<double> lateral_inflow; ///< Lateral inflow [project flow units]
    std::vector<double> total_inflow;   ///< Total inflow [project flow units]
    std::vector<double> overflow;       ///< Overflow / surcharge [project flow units]
};

/**
 * @brief Snapshot of link state at an output time step.
 * @ingroup engine_plugins
 */
struct LinkSnapshot {
    std::vector<double> flow;       ///< Flow rate [internal flow units]
    std::vector<double> depth;      ///< Water depth [internal length units]
    std::vector<double> velocity;   ///< Mean velocity [internal length/time]
    std::vector<double> volume;     ///< Link volume [internal volume units]
    std::vector<double> capacity;   ///< Full-flow capacity fraction [0, 1]
};

/**
 * @brief Snapshot of subcatchment state at an output time step.
 * @ingroup engine_plugins
 */
struct SubcatchSnapshot {
    std::vector<double> rainfall;   ///< Rainfall rate [internal rate units]
    std::vector<double> snow_depth; ///< Snow depth [internal length units]
    std::vector<double> evap;       ///< Evaporation [internal rate units]
    std::vector<double> infil;      ///< Infiltration [internal rate units]
    std::vector<double> runoff;     ///< Surface runoff [internal flow units]
    std::vector<double> gw_flow;    ///< Groundwater outflow [internal flow units]
    std::vector<double> gw_elev;    ///< Groundwater elevation [internal length units]
    std::vector<double> soil_moist; ///< Soil moisture [-]
};

/**
 * @brief Snapshot of rain gage state at an output time step.
 * @ingroup engine_plugins
 */
struct GageSnapshot {
    std::vector<double> rainfall;   ///< Current rainfall rate [project rate units]
};

/**
 * @brief Complete simulation state snapshot at one output time step.
 *
 * @details Created by the main simulation thread when output is due, then
 *          posted to the IOThread write queue. Plugins receive a const
 *          reference to this during IOutputPlugin::update().
 *
 * @ingroup engine_plugins
 */
struct SimulationSnapshot {
    // -----------------------------------------------------------------------
    // Timing
    // -----------------------------------------------------------------------

    /** @brief Simulation date/time (SWMM DateTime: days since 12/31/1899). */
    double sim_time = 0.0;

    /** @brief Wall-clock Unix timestamp when snapshot was taken. */
    std::int64_t wall_time_unix = 0;

    /** @brief Sequential output step index (0-based). */
    int output_step_index = 0;

    // -----------------------------------------------------------------------
    // Object counts
    // -----------------------------------------------------------------------

    int node_count     = 0; ///< Number of nodes
    int link_count     = 0; ///< Number of links
    int subcatch_count = 0; ///< Number of subcatchments
    int gage_count     = 0; ///< Number of rain gages
    int pollut_count   = 0; ///< Number of pollutants

    // -----------------------------------------------------------------------
    // State arrays (parallel to the engine's SoA arrays)
    // -----------------------------------------------------------------------

    NodeSnapshot     nodes;       ///< Node state
    LinkSnapshot     links;       ///< Link state
    SubcatchSnapshot subcatch;    ///< Subcatchment state
    GageSnapshot     gages;       ///< Gage state

    // -----------------------------------------------------------------------
    // System-level results (for binary output file)
    // -----------------------------------------------------------------------

    double sys_temperature    = 0.0;  ///< Air temperature
    double sys_rainfall       = 0.0;  ///< Average rainfall over all gages
    double sys_snow_depth     = 0.0;  ///< Total snow depth
    double sys_evap           = 0.0;  ///< Total evaporation loss
    double sys_infil          = 0.0;  ///< Total infiltration loss
    double sys_runoff         = 0.0;  ///< Total runoff flow
    double sys_dw_inflow      = 0.0;  ///< Total dry weather inflow
    double sys_gw_inflow      = 0.0;  ///< Total groundwater inflow
    double sys_ii_inflow      = 0.0;  ///< Total RDII inflow
    double sys_ext_inflow     = 0.0;  ///< Total external inflow
    double sys_flooding       = 0.0;  ///< Total flooding
    double sys_outflow        = 0.0;  ///< Total outflow
    double sys_storage        = 0.0;  ///< Total storage volume
    double sys_pet            = 0.0;  ///< Potential evapotranspiration

    // -----------------------------------------------------------------------
    // Pollutant concentrations (optional; populated only if quality routing active)
    // -----------------------------------------------------------------------

    /**
     * @brief Node pollutant concentrations.
     * @details Layout: [node_index * pollut_count + pollut_index]
     *          Size: node_count * pollut_count doubles.
     */
    std::vector<double> node_quality;

    /**
     * @brief Link pollutant concentrations.
     * @details Layout: [link_index * pollut_count + pollut_index]
     */
    std::vector<double> link_quality;

    /**
     * @brief Subcatchment pollutant loadings.
     */
    std::vector<double> subcatch_quality;

    // -----------------------------------------------------------------------
    // String tables (for plugins that need to label their output)
    // -----------------------------------------------------------------------

    /** @brief Node IDs in index order. Pointer to engine-managed strings. */
    const std::vector<std::string>* node_ids     = nullptr;

    /** @brief Link IDs in index order. */
    const std::vector<std::string>* link_ids     = nullptr;

    /** @brief Subcatchment IDs in index order. */
    const std::vector<std::string>* subcatch_ids = nullptr;

    /** @brief Gage IDs in index order. */
    const std::vector<std::string>* gage_ids     = nullptr;

    /** @brief Pollutant names in index order. */
    const std::vector<std::string>* pollut_names = nullptr;

    // -----------------------------------------------------------------------
    // Unit information
    // -----------------------------------------------------------------------

    /** @brief Flow unit string (e.g., "CFS", "LPS"). */
    const char* flow_units = nullptr;

    /** @brief Length unit string (e.g., "FEET", "METERS"). */
    const char* length_units = nullptr;

    /** @brief Flow units code (FlowUnits enum value: 0=CFS, 3=CMS, etc.). */
    int flow_units_code = 0;

    // -----------------------------------------------------------------------
    // 2D surface routing state (optional; populated only when the engine was
    // built with OPENSWMM_BUILD_2D and the input file contains a 2D mesh)
    //
    // Layout: per-triangle vectors are sized `surface_tri_count`; per-vertex
    // vectors are sized `surface_vert_count`. `surface_edge_flux` is flat
    // [tri * 3 + edge] of size `surface_tri_count * 3`.
    //
    // Fields are deep-copied from `SurfaceRouter2D::state()` on the main
    // simulation thread; consumers (Default2DOutputPlugin and any third-party
    // 2D output plugins) read them through the IO-thread snapshot reference
    // without further synchronization.
    //
    // Vectors are empty when 2D is inactive (either OPENSWMM_BUILD_2D=OFF or
    // the input file contained no [2D_VERTICES] section). Plugins must check
    // `surface_tri_count == 0` to skip 2D-specific work in that case.
    // -----------------------------------------------------------------------

    int surface_tri_count  = 0;             ///< Number of triangles (faces)
    int surface_vert_count = 0;             ///< Number of vertices (nodes)

    std::vector<double> surface_depth;          ///< Overland flow depth ψ_o (m), per face
    std::vector<double> surface_head;           ///< Total head h_o = z_s + ψ_o (m), per face
    std::vector<double> surface_grad_hx;        ///< Unlimited head gradient ∂h/∂x, per face
    std::vector<double> surface_grad_hy;        ///< Unlimited head gradient ∂h/∂y, per face
    std::vector<double> surface_grad_hx_lim;    ///< Slope-limited head gradient ∂h/∂x, per face
    std::vector<double> surface_grad_hy_lim;    ///< Slope-limited head gradient ∂h/∂y, per face
    std::vector<double> surface_rainfall;       ///< Rainfall intensity (m/s), per face
    std::vector<double> surface_coupling_flux;  ///< Coupling flux to SWMM node (m/s, + = into 2D), per face
    std::vector<double> surface_net_source;     ///< Net source/sink (m/s), per face
    std::vector<double> surface_edge_flux;      ///< Normal flux through each edge, flat [tri*3+edge]
    std::vector<double> surface_vert_head;      ///< Reconstructed head at vertices (m)
};

} /* namespace openswmm */

#endif /* OPENSWMM_SIMULATION_SNAPSHOT_HPP */
