// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

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
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_SIMULATION_SNAPSHOT_HPP
#define OPENSWMM_SIMULATION_SNAPSHOT_HPP

#include <vector>
#include <string>
#include <cstdint>

namespace openswmm {

// NOTE ON UNITS: the 1D node / link / subcatchment / gage / system fields below
// are delivered in PROJECT DISPLAY units (e.g. m, m³, CMS for an SI project;
// ft, ft³, CFS for US). The engine applies a single conversion boundary
// (convertSnapshotToDisplay) when building the snapshot, so output plugins
// write values directly without re-applying Ucf/Qcf. Dimensionless fields
// (capacity, soil moisture, pollutant concentrations) are unconverted. The 2D
// surface_* arrays on SimulationSnapshot are the exception: they are SI-native
// solver outputs and are NOT touched by the boundary.

/**
 * @brief Snapshot of node state at an output time step.
 * @ingroup engine_plugins
 */
struct NodeSnapshot {
    std::vector<double> depth;          ///< Water depth [display length units]
    std::vector<double> head;           ///< Hydraulic head [display length units]
    std::vector<double> volume;         ///< Stored volume [display volume units]
    std::vector<double> lateral_inflow; ///< Lateral inflow [display flow units]
    std::vector<double> total_inflow;   ///< Total inflow [display flow units]
    std::vector<double> overflow;       ///< Overflow / surcharge [display flow units]
};

/**
 * @brief Snapshot of link state at an output time step.
 * @ingroup engine_plugins
 */
struct LinkSnapshot {
    std::vector<double> flow;       ///< Flow rate [display flow units]
    std::vector<double> depth;      ///< Water depth [display length units]
    std::vector<double> velocity;   ///< Mean velocity [display length/time]
    std::vector<double> volume;     ///< Link volume [display volume units]
    std::vector<double> capacity;   ///< Full-flow capacity fraction [0, 1]
};

/**
 * @brief Snapshot of subcatchment state at an output time step.
 * @ingroup engine_plugins
 */
struct SubcatchSnapshot {
    std::vector<double> rainfall;   ///< Rainfall rate [display rate units]
    std::vector<double> snow_depth; ///< Snow depth [display depth units]
    std::vector<double> evap;       ///< Evaporation [display rate units]
    std::vector<double> infil;      ///< Infiltration [display rate units]
    std::vector<double> runoff;     ///< Surface runoff [display flow units]
    std::vector<double> gw_flow;    ///< Groundwater outflow [display flow units]
    std::vector<double> gw_elev;    ///< Groundwater elevation [display length units]
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

    /**
     * @brief Whether the model has any subcatchments.
     * @details Legacy assigns SYS_TEMPERATURE and SYS_PET — and applies the SI
     *          temperature conversion — at the end of
     *          output_saveSubcatchResults, which is called only behind
     *          `if (Nobjects[SUBCATCH] > 0)` (output.c:489-490). With no
     *          subcatchments both stay at the zero-initialised value of
     *          SysResults and are never converted, so a raw 0 is reported
     *          whatever the unit system. Carried on the snapshot because the
     *          display-unit conversion is where that has to be honoured: the
     *          degF->degC conversion is AFFINE, so converting a zero yields
     *          -17.78 degC rather than zero.
     */
    bool has_subcatchments = true;

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
    std::vector<double> surface_infil_rate;     ///< Held infiltration loss rate (m/s, ≥ 0), per face — all zero when no [2D_INFILTRATION*] model resolved
    std::vector<double> surface_infil_cum;      ///< Cumulative infiltrated depth (m), per face
    std::vector<double> surface_rain_cum;       ///< Cumulative rainfall volume (m³), per face
    /**
     * @brief 2D surface species concentration (overland transport S1/S2).
     * @details Layout: [species * surface_tri_count + face], species-major to
     *          match the engine's own cell store. `surface_species_count`
     *          rows; the species order is the pollutant order
     *          (`pollut_count` rows in S1; age/temperature/MSX join in S4).
     *          A dry cell reports 0 (its mass is held, not divided).
     *          Empty when the model carries no 2D transport.
     */
    std::vector<double> surface_species_conc;
    int surface_species_count = 0;
    std::vector<double> surface_edge_flux;      ///< Normal flux through each edge, flat [tri*3+edge]
    std::vector<double> surface_vert_head;      ///< Reconstructed head at vertices (m) — SOLVER field (dry-cell head = bed)
    std::vector<double> surface_vert_depth;     ///< SIGNED vertex depth η_v − z_v (m) — wet-masked, wetted-contact-gated render reconstruction; > 0 where water reaches the vertex, 0 = no-data sentinel (older engines also emitted negatives on the dry side of partially wet cells — readers stay negative-tolerant)
    std::vector<double> surface_face_vx;        ///< Cell-centred velocity X (m/s), per face
    std::vector<double> surface_face_vy;        ///< Cell-centred velocity Y (m/s), per face
    std::vector<double> surface_continuity_err; ///< Per-cell continuity residual (m³/s), per face

    // Cumulative rendering envelopes (SI-native, monotone over the run), per face.
    // A snapshot of these at any time is the envelope up to that time; the 2D
    // output plugin overwrites the fixed [nFace] envelope datasets in place.
    std::vector<double> surface_stat_max_depth;    ///< Max overland depth ψ_o (m), per face
    std::vector<double> surface_stat_max_velocity; ///< Max cell speed |v| (m/s), per face
    std::vector<double> surface_stat_max_cont_err; ///< Max |continuity residual| (m³/s), per face
};

} /* namespace openswmm */

#endif /* OPENSWMM_SIMULATION_SNAPSHOT_HPP */
