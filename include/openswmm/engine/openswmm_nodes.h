/**
 * @file openswmm_nodes.h
 * @brief OpenSWMM Engine — Node C API.
 *
 * @details Node add (BUILDING state), geometry setters, state get/set,
 *          lateral inflow injection (RUNNING), bulk access, quality.
 *
 * @ingroup engine_api
 * @see openswmm_engine.h
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_NODES_H
#define OPENSWMM_NODES_H

#include "openswmm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Node type classification.
 *
 * @details Every node in a SWMM model belongs to one of these types. The type
 *          governs which property setters/getters are valid and how the node
 *          participates in routing.
 */
typedef enum SWMM_NodeType {
    SWMM_NODE_JUNCTION = 0, /**< Standard junction node. */
    SWMM_NODE_OUTFALL  = 1, /**< Outfall boundary node. */
    SWMM_NODE_STORAGE  = 2, /**< Storage unit (pond, tank, etc.). */
    SWMM_NODE_DIVIDER  = 3  /**< Flow divider. */
} SWMM_NodeType;

/* =========================================================================
 * Identity
 * ========================================================================= */

/**
 * @brief Get the total number of nodes in the model.
 * @param engine  Engine handle.
 * @returns Number of nodes, or -1 on error.
 */
SWMM_ENGINE_API int swmm_node_count(SWMM_Engine engine);

/**
 * @brief Look up a node's zero-based index by its string identifier.
 * @param engine  Engine handle.
 * @param id      Null-terminated node identifier.
 * @returns Zero-based index, or -1 if not found.
 */
SWMM_ENGINE_API int swmm_node_index(SWMM_Engine engine, const char* id);

/**
 * @brief Get the string identifier of a node by index.
 * @param engine  Engine handle.
 * @param idx     Zero-based node index.
 * @returns Null-terminated string owned by the engine, or NULL on error.
 */
SWMM_ENGINE_API const char* swmm_node_id(SWMM_Engine engine, int idx);

/* =========================================================================
 * Creation (BUILDING or OPENED — "editable" states)
 * ========================================================================= */

/**
 * @brief Add a new node to the model.
 *
 * @details The engine must be in SWMM_STATE_BUILDING (programmatic
 *          construction) or SWMM_STATE_OPENED (interactive editing after
 *          the .inp has been parsed). Returns SWMM_ERR_LIFECYCLE for any
 *          other state — once the simulation has been initialized, started,
 *          or run, the node-count invariant is baked into solver state and
 *          the engine must be closed + re-opened to accept new objects.
 *
 *          The node is appended to the model's node list and its index
 *          equals the previous count.
 *
 * @param engine  Engine handle.
 * @param id      Unique null-terminated identifier for the new node.
 * @param type    Node type (see @ref SWMM_NodeType).
 * @returns SWMM_OK on success, SWMM_ERR_LIFECYCLE if not in an editable
 *          state, or another error code.
 */
SWMM_ENGINE_API int swmm_node_add(SWMM_Engine engine, const char* id, int type);

/**
 * @brief Remove the most recently added node (undo-of-add).
 *
 * @details Pops the tail of the node list. The engine must be in
 *          SWMM_STATE_BUILDING or SWMM_STATE_OPENED. Returns
 *          SWMM_ERR_BADINDEX if the tail doesn't match \p id (guards
 *          against undo / redo order mismatches), SWMM_ERR_BADPARAM if
 *          any link references the tail node (the caller must cascade
 *          those removals first via @ref swmm_link_pop_last), or
 *          SWMM_ERR_LIFECYCLE for any other state.
 *
 *          This is an intentionally narrow surface that avoids
 *          renumbering any cross-references; for a general
 *          swmm_node_remove(idx) see the engine roadmap — full remove
 *          requires renumbering every link / subcatch / control / report
 *          reference and is tracked separately.
 *
 * @param engine  Engine handle.
 * @param id      Expected tail identifier (null-terminated).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_pop_last(SWMM_Engine engine, const char* id);

/* =========================================================================
 * Geometry setters (BUILDING or OPENED)
 * ========================================================================= */

/**
 * @brief Set a node's invert elevation.
 * @param engine  Engine handle.
 * @param idx     Zero-based node index.
 * @param elev    Invert elevation in project length units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_set_invert_elev(SWMM_Engine engine, int idx, double elev);

/**
 * @brief Set a node's maximum depth (distance from invert to crown).
 * @param engine  Engine handle.
 * @param idx     Zero-based node index.
 * @param depth   Maximum depth in project length units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_set_max_depth(SWMM_Engine engine, int idx, double depth);

/**
 * @brief Set the allowed surcharge depth above the node's crown.
 * @param engine  Engine handle.
 * @param idx     Zero-based node index.
 * @param depth   Surcharge depth in project length units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_set_surcharge_depth(SWMM_Engine engine, int idx, double depth);

/**
 * @brief Set the ponded surface area when depth exceeds the maximum.
 *
 * @details When ponding is modeled, excess water is stored on the surface
 *          using this area. Set to 0 to disable ponding at this node.
 *
 * @param engine  Engine handle.
 * @param idx     Zero-based node index.
 * @param area    Ponded area in project area units (e.g., ft² or m²).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_set_pond_area(SWMM_Engine engine, int idx, double area);

/**
 * @brief Set a node's initial water depth at simulation start.
 * @param engine  Engine handle.
 * @param idx     Zero-based node index.
 * @param depth   Initial depth in project length units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_set_initial_depth(SWMM_Engine engine, int idx, double depth);

/* =========================================================================
 * Geometry getters
 * ========================================================================= */

/**
 * @brief Get the type of a node.
 * @param engine  Engine handle.
 * @param idx     Zero-based node index.
 * @param[out] type  Receives the node type (see @ref SWMM_NodeType).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_get_type(SWMM_Engine engine, int idx, int* type);

/**
 * @brief Get a node's invert elevation.
 * @param engine  Engine handle.
 * @param idx     Zero-based node index.
 * @param[out] elev  Receives the invert elevation in project length units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_get_invert_elev(SWMM_Engine engine, int idx, double* elev);

/**
 * @brief Get a node's maximum depth.
 * @param engine  Engine handle.
 * @param idx     Zero-based node index.
 * @param[out] depth  Receives the maximum depth in project length units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_get_max_depth(SWMM_Engine engine, int idx, double* depth);

/* =========================================================================
 * Hydraulic state getters/setters
 * ========================================================================= */

/**
 * @brief Get the current water depth at a node.
 * @param engine  Engine handle.
 * @param idx     Zero-based node index.
 * @param[out] depth  Receives depth in project length units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_get_depth(SWMM_Engine engine, int idx, double* depth);

/**
 * @brief Set the water depth at a node (runtime override).
 * @param engine  Engine handle (RUNNING state).
 * @param idx     Zero-based node index.
 * @param depth   New depth in project length units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_set_depth(SWMM_Engine engine, int idx, double depth);

/**
 * @brief Get the current hydraulic head at a node (invert + depth).
 * @param engine  Engine handle.
 * @param idx     Zero-based node index.
 * @param[out] head  Receives the head in project length units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_get_head(SWMM_Engine engine, int idx, double* head);

/**
 * @brief Get the current stored water volume at a node.
 * @param engine  Engine handle.
 * @param idx     Zero-based node index.
 * @param[out] volume  Receives the volume in project volume units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_get_volume(SWMM_Engine engine, int idx, double* volume);

/**
 * @brief Get the current lateral inflow at a node.
 *
 * @details Lateral inflow is the externally applied flow (DWF, RDII, user
 *          inflows) as opposed to the total inflow which includes upstream
 *          link contributions.
 *
 * @param engine  Engine handle.
 * @param idx     Zero-based node index.
 * @param[out] inflow  Receives the lateral inflow in project flow units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_get_lateral_inflow(SWMM_Engine engine, int idx, double* inflow);

/**
 * @brief Get the current overflow (flooding) rate at a node.
 *
 * @details Overflow occurs when the water depth exceeds the node's maximum
 *          depth and ponding is not enabled (or the ponded area is exceeded).
 *
 * @param engine  Engine handle.
 * @param idx     Zero-based node index.
 * @param[out] overflow  Receives the overflow rate in project flow units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_get_overflow(SWMM_Engine engine, int idx, double* overflow);

/**
 * @brief Get the total inflow to a node (lateral + upstream links).
 * @param engine  Engine handle.
 * @param idx     Zero-based node index.
 * @param[out] inflow  Receives the total inflow in project flow units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_get_inflow(SWMM_Engine engine, int idx, double* inflow);

/* =========================================================================
 * Runtime forcing (RUNNING state only)
 * ========================================================================= */

/**
 * @brief Override the lateral inflow at a node for the current timestep.
 *
 * @details Applied for one timestep only; call each step to sustain.
 *          This replaces any externally defined inflows (DWF, RDII, etc.)
 *          for this node during the current step.
 *
 * @param engine  Engine handle (RUNNING state).
 * @param idx     Zero-based node index.
 * @param flow    Lateral inflow in project flow units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_set_lateral_inflow(SWMM_Engine engine, int idx, double flow);

/**
 * @brief Set a persistent quality mass flux at a node (mass/sec).
 *
 * @details The mass flux is applied additively at each routing step,
 *          converting to a concentration delta: C += (mass_rate * dt) / volume.
 *          The value persists until the user explicitly changes it (analogous
 *          to swmm_node_set_lateral_inflow for flow). Set to 0.0 to stop.
 *
 * @param engine        Engine handle (RUNNING state).
 * @param node_idx      Zero-based node index.
 * @param pollutant_idx Zero-based pollutant index.
 * @param mass_rate     Mass flux in mass/sec (project mass units).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_set_quality_mass_flux(SWMM_Engine engine, int node_idx,
                                                     int pollutant_idx, double mass_rate);

/**
 * @brief Set a fixed head boundary condition at a node.
 *
 * @details Useful for outfall nodes or real-time coupled boundary conditions.
 *          Applied for the current timestep only.
 *
 * @param engine  Engine handle (RUNNING state).
 * @param idx     Zero-based node index.
 * @param head    Head boundary in project length units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_set_head_boundary(SWMM_Engine engine, int idx, double head);

/* =========================================================================
 * Water quality
 * ========================================================================= */

/**
 * @brief Get the pollutant concentration at a node.
 * @param engine        Engine handle.
 * @param node_idx      Zero-based node index.
 * @param pollutant_idx Zero-based pollutant index.
 * @param[out] conc     Receives the concentration in pollutant units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_get_quality(SWMM_Engine engine, int node_idx,
                                           int pollutant_idx, double* conc);

/* =========================================================================
 * Storage Node API
 * ========================================================================= */

/**
 * @brief Assign a storage curve (depth vs. area) to a storage node.
 * @param engine     Engine handle.
 * @param idx        Zero-based node index (must be SWMM_NODE_STORAGE).
 * @param curve_idx  Zero-based curve index (from swmm_curve_add()).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_set_storage_curve(SWMM_Engine engine, int idx, int curve_idx);

/**
 * @brief Get the storage curve index assigned to a storage node.
 * @param engine         Engine handle.
 * @param idx            Zero-based node index.
 * @param[out] curve_idx Receives the curve index, or -1 if functional.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_get_storage_curve(SWMM_Engine engine, int idx, int* curve_idx);

/**
 * @brief Set functional storage parameters: Area = a * Depth^b + c.
 *
 * @details Defines the depth–area relationship using a power-law equation
 *          instead of a tabular curve.
 *
 * @param engine  Engine handle.
 * @param idx     Zero-based node index (must be SWMM_NODE_STORAGE).
 * @param a       Coefficient a.
 * @param b       Exponent b.
 * @param c       Constant c.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_set_storage_functional(SWMM_Engine engine, int idx, double a, double b, double c);

/**
 * @brief Get functional storage parameters.
 * @param engine  Engine handle.
 * @param idx     Zero-based node index.
 * @param[out] a  Receives coefficient a.
 * @param[out] b  Receives exponent b.
 * @param[out] c  Receives constant c.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_get_storage_functional(SWMM_Engine engine, int idx, double* a, double* b, double* c);

/**
 * @brief Set the seepage rate for a storage node.
 * @param engine  Engine handle.
 * @param idx     Zero-based node index (must be SWMM_NODE_STORAGE).
 * @param rate    Seepage rate in project length/time units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_set_storage_seep_rate(SWMM_Engine engine, int idx, double rate);

/**
 * @brief Get the seepage rate for a storage node.
 * @param engine     Engine handle.
 * @param idx        Zero-based node index.
 * @param[out] rate  Receives the seepage rate.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_get_storage_seep_rate(SWMM_Engine engine, int idx, double* rate);

/**
 * @brief Set Green–Ampt exfiltration parameters for a storage node.
 *
 * @details Exfiltration models soil infiltration losses from the bottom and
 *          banks of a storage unit using the Green–Ampt method.
 *
 * @param engine   Engine handle.
 * @param idx      Zero-based node index (must be SWMM_NODE_STORAGE).
 * @param suction  Soil capillary suction head (project length units).
 * @param ksat     Saturated hydraulic conductivity (project length/time).
 * @param imd      Initial moisture deficit (fraction: 0–1).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_set_exfil_params(SWMM_Engine engine, int idx, double suction, double ksat, double imd);

/**
 * @brief Get Green–Ampt exfiltration parameters for a storage node.
 * @param engine        Engine handle.
 * @param idx           Zero-based node index.
 * @param[out] suction  Receives the suction head.
 * @param[out] ksat     Receives the saturated hydraulic conductivity.
 * @param[out] imd      Receives the initial moisture deficit.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_get_exfil_params(SWMM_Engine engine, int idx, double* suction, double* ksat, double* imd);

/* =========================================================================
 * Outfall Node API
 * ========================================================================= */

/**
 * @brief Set the outfall boundary condition type.
 *
 * @details Common types: 0=FREE, 1=NORMAL, 2=FIXED, 3=TIDAL, 4=TIMESERIES.
 *
 * @param engine  Engine handle.
 * @param idx     Zero-based node index (must be SWMM_NODE_OUTFALL).
 * @param type    Outfall type code.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_set_outfall_type(SWMM_Engine engine, int idx, int type);

/**
 * @brief Get the outfall boundary condition type.
 * @param engine     Engine handle.
 * @param idx        Zero-based node index.
 * @param[out] type  Receives the outfall type code.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_get_outfall_type(SWMM_Engine engine, int idx, int* type);

/**
 * @brief Set a fixed outfall stage (for FIXED type outfalls).
 * @param engine  Engine handle.
 * @param idx     Zero-based node index (must be SWMM_NODE_OUTFALL).
 * @param stage   Fixed stage elevation in project length units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_set_outfall_stage(SWMM_Engine engine, int idx, double stage);

/**
 * @brief Assign a tidal curve to an outfall (for TIDAL type outfalls).
 * @param engine     Engine handle.
 * @param idx        Zero-based node index (must be SWMM_NODE_OUTFALL).
 * @param curve_idx  Zero-based curve index defining hour-of-day vs. stage.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_set_outfall_tidal(SWMM_Engine engine, int idx, int curve_idx);

/**
 * @brief Assign a time series to an outfall (for TIMESERIES type outfalls).
 * @param engine  Engine handle.
 * @param idx     Zero-based node index (must be SWMM_NODE_OUTFALL).
 * @param ts_idx  Zero-based time series index defining time vs. stage.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_set_outfall_timeseries(SWMM_Engine engine, int idx, int ts_idx);

/**
 * @brief Get the outfall stage parameter (fixed stage, or current computed stage).
 * @param engine      Engine handle.
 * @param idx         Zero-based node index.
 * @param[out] param  Receives the outfall parameter value.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_get_outfall_param(SWMM_Engine engine, int idx, double* param);

/**
 * @brief Set whether a flap gate exists at the outfall.
 *
 * @details A flap gate prevents reverse flow through the outfall.
 *
 * @param engine    Engine handle.
 * @param idx       Zero-based node index (must be SWMM_NODE_OUTFALL).
 * @param has_gate  Non-zero to enable flap gate; zero to disable.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_set_outfall_flap_gate(SWMM_Engine engine, int idx, int has_gate);

/**
 * @brief Get whether a flap gate exists at the outfall.
 * @param engine        Engine handle.
 * @param idx           Zero-based node index.
 * @param[out] has_gate Receives 1 if flap gate present, 0 otherwise.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_get_outfall_flap_gate(SWMM_Engine engine, int idx, int* has_gate);

/* =========================================================================
 * Additional Geometry / State Getters
 * ========================================================================= */

/**
 * @brief Get the surcharge depth above the node's crown.
 * @param engine      Engine handle.
 * @param idx         Zero-based node index.
 * @param[out] depth  Receives the surcharge depth in project length units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_get_surcharge_depth(SWMM_Engine engine, int idx, double* depth);

/**
 * @brief Get the ponded area at a node.
 * @param engine     Engine handle.
 * @param idx        Zero-based node index.
 * @param[out] area  Receives the ponded area in project area units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_get_ponded_area(SWMM_Engine engine, int idx, double* area);

/**
 * @brief Get the initial depth at a node.
 * @param engine      Engine handle.
 * @param idx         Zero-based node index.
 * @param[out] depth  Receives the initial depth in project length units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_get_initial_depth(SWMM_Engine engine, int idx, double* depth);

/**
 * @brief Get the crown elevation (invert + max depth) at a node.
 * @param engine     Engine handle.
 * @param idx        Zero-based node index.
 * @param[out] elev  Receives the crown elevation in project length units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_get_crown_elev(SWMM_Engine engine, int idx, double* elev);

/**
 * @brief Get the full (maximum) stored volume at a node.
 * @param engine    Engine handle.
 * @param idx       Zero-based node index.
 * @param[out] vol  Receives the full volume in project volume units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_get_full_volume(SWMM_Engine engine, int idx, double* vol);

/**
 * @brief Get the cumulative water losses at a node (evaporation + exfiltration).
 * @param engine       Engine handle.
 * @param idx          Zero-based node index.
 * @param[out] losses  Receives the loss volume in project volume units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_get_losses(SWMM_Engine engine, int idx, double* losses);

/**
 * @brief Get the total outflow from a node through downstream links.
 * @param engine        Engine handle.
 * @param idx           Zero-based node index.
 * @param[out] outflow  Receives the outflow in project flow units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_get_outflow(SWMM_Engine engine, int idx, double* outflow);

/**
 * @brief Get the degree (number of connected links) of a node.
 * @param engine       Engine handle.
 * @param idx          Zero-based node index.
 * @param[out] degree  Receives the number of links connected to this node.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_get_degree(SWMM_Engine engine, int idx, int* degree);

/* =========================================================================
 * Node Statistics
 * ========================================================================= */

/**
 * @brief Get the maximum depth recorded at a node during the simulation.
 * @param engine    Engine handle (ENDED or RUNNING state).
 * @param idx       Zero-based node index.
 * @param[out] val  Receives the maximum depth in project length units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_get_stat_max_depth(SWMM_Engine engine, int idx, double* val);

/**
 * @brief Get the maximum overflow rate recorded at a node.
 * @param engine    Engine handle.
 * @param idx       Zero-based node index.
 * @param[out] val  Receives the maximum overflow in project flow units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_get_stat_max_overflow(SWMM_Engine engine, int idx, double* val);

/**
 * @brief Get the total flood volume at a node over the simulation.
 * @param engine    Engine handle.
 * @param idx       Zero-based node index.
 * @param[out] val  Receives the flooded volume in project volume units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_get_stat_vol_flooded(SWMM_Engine engine, int idx, double* val);

/**
 * @brief Get the total time a node was flooded during the simulation.
 * @param engine    Engine handle.
 * @param idx       Zero-based node index.
 * @param[out] val  Receives the flooded duration in hours.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_get_stat_time_flooded(SWMM_Engine engine, int idx, double* val);

/* =========================================================================
 * Bulk access
 * ========================================================================= */

/**
 * @brief Get depths for all nodes in a single call.
 * @param engine  Engine handle.
 * @param[out] buf  Caller-allocated buffer of at least @p count doubles.
 * @param count     Number of elements (should equal swmm_node_count()).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_get_depths_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Get hydraulic heads for all nodes in a single call.
 * @param engine  Engine handle.
 * @param[out] buf  Caller-allocated buffer of at least @p count doubles.
 * @param count     Number of elements.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_get_heads_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Get total inflows for all nodes in a single call.
 * @param engine  Engine handle.
 * @param[out] buf  Caller-allocated buffer of at least @p count doubles.
 * @param count     Number of elements.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_get_inflows_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Get overflow rates for all nodes in a single call.
 * @param engine  Engine handle.
 * @param[out] buf  Caller-allocated buffer of at least @p count doubles.
 * @param count     Number of elements.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_get_overflows_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Set depths for all nodes in a single call (runtime override).
 * @param engine  Engine handle (RUNNING state).
 * @param buf     Array of depth values, one per node.
 * @param count   Number of elements (should equal swmm_node_count()).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_set_depths_bulk(SWMM_Engine engine, const double* buf, int count);

/**
 * @brief Set lateral inflows for all nodes in a single call (runtime override).
 * @param engine  Engine handle (RUNNING state).
 * @param buf     Array of lateral inflow values, one per node.
 * @param count   Number of elements.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_set_lat_inflows_bulk(SWMM_Engine engine, const double* buf, int count);

/**
 * @brief Get pollutant concentrations at all nodes for one pollutant.
 * @param engine        Engine handle.
 * @param pollutant_idx Zero-based pollutant index.
 * @param[out] buf      Caller-allocated buffer of at least @p count doubles.
 * @param count         Number of elements (should equal swmm_node_count()).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_node_get_quality_bulk(SWMM_Engine engine, int pollutant_idx,
                                                    double* buf, int count);

/* =========================================================================
 * Outfall-to-subcatchment routing
 * ========================================================================= */

/** @brief Set outfall route-to subcatchment index (-1 = none). */
SWMM_ENGINE_API int swmm_node_set_outfall_route_to(SWMM_Engine engine, int idx, int subcatch_idx);

/** @brief Get outfall route-to subcatchment index (-1 = none). */
SWMM_ENGINE_API int swmm_node_get_outfall_route_to(SWMM_Engine engine, int idx, int* subcatch_idx);

/* =========================================================================
 * Depth from volume (inverse of getVolume)
 * ========================================================================= */

/** @brief Compute depth from volume for a node (inverse of volume-depth curve). */
SWMM_ENGINE_API int swmm_node_get_depth_from_volume(SWMM_Engine engine, int idx,
                                                      double volume, double* depth);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_NODES_H */
