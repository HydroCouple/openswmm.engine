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
 * @brief Per-routing-step junction exchange: evaluate + book + accumulate.
 *
 * @details Decoupled-timestep coupling (2026-07 plan). Called EVERY 1D routing
 *          step (not once per 2D advance window): evaluates the same capped-pipe
 *          orifice exchange as the retired computeCouplingExchange but with the LIVE 1D node
 *          heads of this routing step against the FROZEN 2D state from the last
 *          fired window, and with dt = the routing step, so head transients are
 *          integrated at the 1D cadence instead of being sampled once at window
 *          fire and amplified into whole-window volumes.
 *
 *          Per point k it books Q·dt into nodes.coupling_volume (ft³, consumed
 *          by assembleLateralInflows at the start of the NEXT routing step) and
 *          accumulates the same signed volume (m³, SI) into accum_m3[k] for the
 *          2D-side injection when the advance window fires.
 *
 *          The 2D-side withdrawal cap draws down cell_budget_m3 (per-cell m³,
 *          seeded from max(0, volume) at the last window boundary): N steps of
 *          drain against the frozen state can never overdraw the water the 2D
 *          domain actually held. Budget is decremented across the vertex
 *          stencil by the geometric partition-of-unity weights.
 *
 * @param cps            Coupling points (outfalls skipped).
 * @param mesh           Mesh data.
 * @param state          2D surface state (FROZEN — read-only here).
 * @param ctx            Simulation context (live 1D node heads/volumes).
 * @param opts           2D solver options.
 * @param dt             Current 1D routing step (s).
 * @param accum_m3       In/out: per-coupling-point window accumulator (m³,
 *                       + = 2D→1D drain, − = 1D→2D spill); sized like cps.
 * @param cell_budget_m3 In/out: per-cell remaining withdrawal budget (m³).
 * @param sample_row     Optional (nullable): per-point NET 2D-SOURCE rate row
 *                       for the interpolated-forcing series — this step's
 *                       capped junction exchange written as −Q (drain-positive
 *                       Q is a 2D sink) into sample_row[k]. Outfall slots are
 *                       left untouched (accumulateOutfallDischargeStep owns
 *                       them); caller zeroes the row each step.
 */
void computeCouplingExchangeStep(const std::vector<CouplingPoint>& cps,
                                 const MeshData& mesh,
                                 const SurfaceStateData& state,
                                 SimulationContext& ctx,
                                 const SolverOptions2D& opts,
                                 double dt,
                                 std::vector<double>& accum_m3,
                                 std::vector<double>& cell_budget_m3,
                                 double* sample_row);

/**
 * @brief Per-routing-step outfall discharge accumulation.
 *
 * @details Per-step successor of the retired transferOutfallDischarges: samples the LIVE
 *          net outfall exchange (nodes.inflow − nodes.outflow, this routing
 *          step) and accumulates Q_net·dt (m³, + = 1D discharge onto the 2D
 *          surface, − = surface water drawn back through the outfall) into
 *          accum_m3[k]. Withdrawals are capped by the shared per-cell budget so
 *          the accumulated window sink cannot overdraw the frozen 2D state.
 *
 * @param sample_row Optional (nullable): net 2D-source rate row for the
 *                   interpolated-forcing series — this step's capped Q_net
 *                   written as +Q_net into sample_row[k] (already
 *                   source-positive). Junction slots left untouched.
 * @return Number of outfalls whose withdrawal was clamped this step.
 */
int accumulateOutfallDischargeStep(const std::vector<CouplingPoint>& cps,
                                   const MeshData& mesh,
                                   const SurfaceStateData& state,
                                   const SimulationContext& ctx,
                                   const SolverOptions2D& opts,
                                   double dt,
                                   std::vector<double>& accum_m3,
                                   std::vector<double>& cell_budget_m3,
                                   double* sample_row);

/**
 * @brief Inject a per-point accumulated exchange volume into the 2D window
 *        source field.
 *
 * @details Window-fire counterpart of the per-step accumulators: converts each
 *          accumulated volume to the mean rate over the window
 *          (Q_k = accum_m3[k] / window_dt) and scatters it into
 *          state.coupling_flux with the given sign convention
 *          (+1: accum is already 2D-source-positive, e.g. outfall discharge;
 *          −1: accum is 2D→1D-drain-positive, e.g. junction exchange, so the
 *          2D-side source is its negation). coupling_flux is NOT cleared here —
 *          callers zero it once before injecting both accumulators.
 */
void injectAccumulatedExchange(const std::vector<CouplingPoint>& cps,
                               const MeshData& mesh,
                               SurfaceStateData& state,
                               const std::vector<double>& accum_m3,
                               double window_dt,
                               double sign);

/**
 * @brief Live node-coupling orifice flux for ONE non-outfall coupling point.
 *
 * Evaluates the bidirectional capped-pipe orifice exchange Q (m³/s; > 0 drains
 * 2D → 1D, < 0 spills 1D → 2D) from the CURRENT 2D state (head/depth/vert_head,
 * reconstructed live inside the CVODE RHS) against the 1D node head, which is
 * frozen for the duration of a 2D advance() window. Unlike
 * the retired computeCouplingExchange (which pre-computed a HELD flux per window and capped it
 * by available volume / dt to stop a held drain overshooting), this is the
 * continuous form for use inside the RHS: the orifice + capped-pipe gate + the
 * wet/dry Hermite ramp on the LIVE source-side depth make Q self-limit smoothly
 * as the cell drains, so CVODE integrates the stiff coupling implicitly and
 * stably across a large macro-window — no discrete avail/dt cap needed.
 *
 * Booking/conservation is handled by the caller integrating ∫Q dt (a per-point
 * accumulator carried in the augmented state vector) over the window.
 *
 * @param provisional_vol_m3 Optional per-cell volume (m³) overriding the 2D
 *        state's own depth/head for the driving-head and wet/dry-ramp terms.
 *        nullptr on the live-RHS path (CVODE's state is already current). The
 *        per-routing-step decoupled path passes the window's remaining
 *        withdrawal budget so the exchange self-limits as the surface drains
 *        provisionally — without it the frozen window state makes the same
 *        full-rate drain repeat every sub-step and the window's total exchange
 *        scales with the number of routing steps in it.
 */
double computeNodeCouplingQ(const CouplingPoint& cp,
                            const MeshData& mesh,
                            const SurfaceStateData& state,
                            const NodeData& nodes,
                            const SolverOptions2D& opts,
                            const double* provisional_vol_m3 = nullptr) noexcept;

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


} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_NODE_COUPLING_HPP
