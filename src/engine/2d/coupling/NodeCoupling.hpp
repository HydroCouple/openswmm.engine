/**
 * @file NodeCoupling.hpp
 * @brief Orifice-equation exchange between 2D surface and SWMM nodes.
 *
 * @details Handles:
 *          - Bidirectional orifice exchange at junction coupling points
 *          - Uncapped node surcharge spill and return flow
 *          - Outfall boundary feedback (dynamic tailwater from 2D)
 *          - Flap gate backflow prevention at outfalls
 *          - Ponding suppression for 2D-coupled nodes
 *          - Mass-conservative injection via the forcing API
 *
 * @see TWO_DIMENSIONAL_SURFACE_ROUTING_IMPLEMENTATION_STRATEGY.md §6, §13, §14
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_2D_NODE_COUPLING_HPP
#define OPENSWMM_ENGINE_2D_NODE_COUPLING_HPP

#include "../data/MeshData.hpp"
#include "../data/SurfaceStateData.hpp"
#include "../data/SolverOptions2D.hpp"

// Forward declaration — avoid pulling in full SimulationContext
namespace openswmm {
struct SimulationContext;
}

namespace openswmm::twoD {

/**
 * @brief Descriptor for a single coupling point between 2D and 1D.
 */
struct CouplingPoint {
    int cell_idx;       ///< Triangle index in the 2D mesh
    int vertex_idx;     ///< Vertex index (-1 if triangle-centroid coupling)
    int node_idx;       ///< SWMM node index
    double cd;          ///< Discharge coefficient
    double area;        ///< Effective exchange area (m²)
    bool is_outfall;    ///< True if the SWMM node is an outfall
    bool has_flap_gate; ///< True if outfall has a flap gate
};

/**
 * @brief Build the list of coupling points from mesh coupling maps.
 *
 * Resolves vertex/triangle → node mappings into CouplingPoint descriptors.
 * Must be called after node names are resolved to indices.
 *
 * @param mesh   Mesh data with coupling maps populated.
 * @param ctx    Simulation context (for node type and outfall queries).
 * @return Vector of coupling points.
 */
std::vector<CouplingPoint> buildCouplingPoints(const MeshData& mesh,
                                                const SimulationContext& ctx);

/**
 * @brief Compute exchange flows at all coupling points and inject into forcing API.
 *
 * For each coupling point:
 * 1. Computes head difference Δh = h_2d - h_swmm
 * 2. Applies orifice equation: Q = Cd * A * sign(Δh) * sqrt(2g|Δh|)
 * 3. Handles outfall boundary feedback and flap gates
 * 4. Suppresses ponding at coupled nodes
 * 5. Injects Q into forcing API as lateral inflow (ADD, RESET)
 * 6. Records coupling flux back into 2D state
 *
 * @param cps    Coupling points.
 * @param mesh   Mesh data.
 * @param state  2D surface state.
 * @param ctx    Simulation context (node heads, forcing API, mass balance).
 * @param dt     Current SWMM routing timestep (s).
 */
void computeCouplingExchange(const std::vector<CouplingPoint>& cps,
                              const MeshData& mesh,
                              SurfaceStateData& state,
                              SimulationContext& ctx,
                              double dt);

/**
 * @brief Update outfall boundary depths from 2D surface heads.
 *
 * For each outfall coupled to the 2D domain, sets the outfall depth to
 * max(h_standard, h_2d) to account for dynamic tailwater from 2D flooding.
 * Must be called before 1D routing step.
 *
 * @param cps   Coupling points.
 * @param mesh  Mesh data.
 * @param state 2D surface state.
 * @param ctx   Simulation context.
 */
void updateOutfallBoundaries(const std::vector<CouplingPoint>& cps,
                              const MeshData& mesh,
                              const SurfaceStateData& state,
                              SimulationContext& ctx);

/**
 * @brief Transfer outfall discharges into 2D coupling cells.
 *
 * After 1D routing, the outfall discharge is a source for the 2D cell
 * at the outfall coupling point.
 *
 * @param cps   Coupling points.
 * @param mesh  Mesh data.
 * @param state 2D surface state.
 * @param ctx   Simulation context.
 * @param dt    Routing timestep (s).
 */
void transferOutfallDischarges(const std::vector<CouplingPoint>& cps,
                                const MeshData& mesh,
                                SurfaceStateData& state,
                                const SimulationContext& ctx,
                                double dt);

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_NODE_COUPLING_HPP
