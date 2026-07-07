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

#include <unordered_map>

#include "../data/MeshData.hpp"
#include "../data/SurfaceStateData.hpp"
#include "../data/SolverOptions2D.hpp"

// Forward declaration — avoid pulling in full SimulationContext
namespace openswmm {
struct SimulationContext;
struct NodeData;
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
 * @param opts   2D solver options (uses dry_depth as the wet/dry threshold).
 * @param dt     Current SWMM routing timestep (s).
 */
void computeCouplingExchange(const std::vector<CouplingPoint>& cps,
                              const MeshData& mesh,
                              SurfaceStateData& state,
                              SimulationContext& ctx,
                              const SolverOptions2D& opts,
                              double dt);

/**
 * @brief Live node-coupling orifice flux for ONE non-outfall coupling point.
 *
 * Evaluates the bidirectional capped-pipe orifice exchange Q (m³/s; > 0 drains
 * 2D → 1D, < 0 spills 1D → 2D) from the CURRENT 2D state (head/depth/vert_head,
 * reconstructed live inside the CVODE RHS) against the 1D node head, which is
 * frozen for the duration of a 2D advance() window. Unlike
 * computeCouplingExchange (which pre-computes a HELD flux per window and caps it
 * by available volume / dt to stop a held drain overshooting), this is the
 * continuous form for use inside the RHS: the orifice + capped-pipe gate + the
 * wet/dry Hermite ramp on the LIVE source-side depth make Q self-limit smoothly
 * as the cell drains, so CVODE integrates the stiff coupling implicitly and
 * stably across a large macro-window — no discrete avail/dt cap needed.
 *
 * Booking/conservation is handled by the caller integrating ∫Q dt (a per-point
 * accumulator carried in the augmented state vector) over the window.
 */
double computeNodeCouplingQ(const CouplingPoint& cp,
                            const MeshData& mesh,
                            const SurfaceStateData& state,
                            const NodeData& nodes,
                            const SolverOptions2D& opts) noexcept;

/**
 * @brief Scatter a signed volumetric exchange Q (m³/s) directly onto the cell
 *        derivatives ydot[] of the 2D volume ODE (for the live-RHS path).
 *
 * Same upwind-HGL stencil distribution as the held-flux scatterCouplingFlux, but
 * adds the per-cell share Q·w (Σw = 1) straight into ydot (m³/s) rather than into
 * a coupling_flux rate, so the exchange is conservative across the stencil.
 * Sign convention matches ydot: positive Q = source INTO the cells, negative =
 * sink OUT of them (so the RHS passes −Q_drain for a 2D → 1D drain).
 */
void scatterCouplingToYdot(const MeshData& mesh,
                           const SurfaceStateData& state,
                           const CouplingPoint& cp,
                           double Q,
                           double* ydot) noexcept;

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
 * @param opts  2D solver options (for unit-system coupling factors).
 */
void updateOutfallBoundaries(const std::vector<CouplingPoint>& cps,
                              const MeshData& mesh,
                              const SurfaceStateData& state,
                              SimulationContext& ctx,
                              const SolverOptions2D& opts);

/**
 * @brief Transfer outfall discharges into 2D coupling cells.
 *
 * After 1D routing, the outfall discharge is a source for the 2D cell
 * at the outfall coupling point. Withdrawal (net backflow into the pipe)
 * is capped at the water actually available in the receiving cell(s) so
 * the held sink cannot pull cell volumes negative over the window.
 *
 * @param cps        Coupling points.
 * @param mesh       Mesh data.
 * @param state      2D surface state.
 * @param ctx        Simulation context.
 * @param opts       2D solver options (for unit-system coupling factors).
 * @param dt         2D advance window (s); used for the withdrawal cap.
 * @param applied_q  Out: net SI exchange (m³/s, +into 2D) actually applied per
 *                   outfall node index — the mass-balance ledger must book
 *                   exactly these (clamped) values, not the raw 1D rates.
 * @return Number of outfalls whose withdrawal was clamped this window.
 */
int transferOutfallDischarges(const std::vector<CouplingPoint>& cps,
                                const MeshData& mesh,
                                SurfaceStateData& state,
                                const SimulationContext& ctx,
                                const SolverOptions2D& opts,
                                double dt,
                                std::unordered_map<int, double>& applied_q);

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_NODE_COUPLING_HPP
