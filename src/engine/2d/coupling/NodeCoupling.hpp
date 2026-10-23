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
 * @file      NodeCoupling.hpp
 * @brief     Orifice-equation exchange between 2D surface and SWMM nodes.
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
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_NODE_COUPLING_HPP
#define OPENSWMM_ENGINE_2D_NODE_COUPLING_HPP

#include <unordered_map>

#include "../data/MeshData.hpp"
#include "../data/SurfaceStateData.hpp"
#include "../data/SolverOptions2D.hpp"
#include "../../data/NodeCoupling2DData.hpp"
#include "ExchangeLaw.hpp"

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
    /// True when the input authored an explicit AREA token for this point;
    /// false = defaulted. COUPLING_AREA AUTO (clamp(1.25 × largest connected
    /// conduit area, 0.05, 2.0) m²) replaces the area of every junction point
    /// either way — mapper-written default tokens look authored.
    bool area_authored = true;
    /// True when the input authored this point's CD; false = the router
    /// substitutes [2D_OPTIONS] COUPLING_CD.
    bool cd_authored = true;
    /// Exchange law resolved for this point (CouplingLaw2D: 0 ORIFICE — the
    /// legacy path on cd/area — or 1 WEIR_ORIFICE on geo/coef).
    int8_t law = 0;
    exchange::Geometry geo;      ///< WEIR_ORIFICE structure (SI, 2D frame)
    exchange::Coefficients coef; ///< WEIR_ORIFICE coefficients
    /// Chamber plan area above the rim (m²): [2D_NODE_COUPLING] CHAMBER_AREA,
    /// else the structure's open area (the map-row area under ORIFICE). The
    /// pond area under COUPLING_POND_MODE CHAMBER, the floor under TRACKING.
    double chamber_area = 0.0;
    /// COUPLING_EQUALIZE resolved for this point.
    bool equalize = false;
    /// Head-sampling cells [head_begin, head_end) in the router's shared list
    /// (SurfaceStateData::coupling_head_cells): the wet-depth-weighted η over
    /// them drives the exchange. Empty = the coupling cell's own head.
    int head_begin = 0;
    int head_end = 0;
    /// Outfalls ([2D_NODE_COUPLING] OUTFALL row): SPREAD radius (m; 0 = the
    /// point's own cells) over which a discharge is delivered, and
    /// TAILWATER_RELAX time (s; 0 = the tailwater follows the surface).
    double spread = 0.0;
    double tw_relax = 0.0;
};

/**
 * @brief Build the list of coupling points from mesh coupling maps.
 *
 * Resolves vertex/triangle → node mappings into CouplingPoint descriptors. Must be called after
 * node names are resolved to indices.
 *
 * @param mesh   Mesh data with coupling maps populated.
 * @param ctx    Simulation context (for node type and outfall queries).
 * @return Vector of coupling points.
 *
 * - Additional argument mesh: Prepared resolved mesh coupling arrays and repeated tri_couplings
 * rows, with consistent vertex/cell shapes and valid nonnegative node IDs into ctx.
 * - Additional argument ctx: Matching engine node types/outfall subtype/flap-gate tables,
 * borrowed during call.
 * @return Owned coupling vector: vertex points first in vertex order, using the lowest incident
 * cell as representative, then valid authored cell rows in input order. Unattached vertices,
 * negative node IDs and invalid cell rows are skipped; positive node bounds are not guarded.
 * @note Copies coefficient/area authored flags and outfall gate metadata. Does not resolve final
 * exchange structure, mutate mesh/context, deduplicate repeated points or reserve water.
 * Allocates and can throw; caller preserves mesh/context identity for later use.
 */
std::vector<CouplingPoint> buildCouplingPoints(const MeshData& mesh, const SimulationContext& ctx);

/**
 * @brief Resolve a junction point's exchange law, structure and coefficients from its node's
 * [2D_NODE_COUPLING] row, the class preset and the global options
 * (plans/1D2D_FINE_SCALE_COUPLING_PLAN §3.1, §3.4–3.5).
 *
 * @details Precedence: map-row CD/AREA (when authored) > node row > preset > [2D_OPTIONS]. An
 * authored map-row CD sets both orifice coefficients; an authored map-row AREA sets the orifice
 * area. MANHOLE: L = πD, A = πD²/4 (D default 1.2 m, 4 ft in US units); GRATE / CURB: AREA (else
 * the map-row area, else the manhole area) and PERIMETER (else 4√A); OPEN: AREA (else the
 * storage area at full depth) and PERIMETER (else √(4πA)). LID_OPEN_FRACTION scales L and A.
 * Coefficients come from COEFFS (node, else COUPLING_COEFFS): DEFAULT 0.540 / ⅔·C_w / 0.167 /
 * 0.160, RUBINATO2017 0.540 / 0.056 / 0.167 / 0.160, _LOWER 0.493 / 0.055 / 0.160 / 0.159,
 * _UPPER 0.587 / 0.057 / 0.174 / 0.161; an OPEN node without COEFFS uses 1.0 throughout; CW /
 * CSW / COI / COO override. The crest follows CREST: the node rim (invert + full depth), the
 * coupling bed (vertex z or cell centroid z), or their MAX / MIN.
 *
 * @param row              The node's row in ctx.node_coupling_2d, or -1.
 * @param type             The node's effective coupling type.
 * @param project_len_to_m Project length unit → metres (authored values).
 */
void resolveExchangeStructure(CouplingPoint& cp, const MeshData& mesh, const SimulationContext& ctx,
                              const SolverOptions2D& opts, int row, NodeCoupling2DType type,
                              double project_len_to_m);

/**
 * @brief Head sensitivity G = −∂Q/∂h_1d ≥ 0 of the coupling orifice at a point (SI: m³/s per m
 * of 1D head, with h_1d in 2D metres).
 *
 * @details Windowless-coupling stabilizer (2026-07-29 plan §5.4): scattered into the
 * dynamic-wave node continuity denominator (`sumdqdh`) each Picard iteration so the exchange
 * stops being a zero-sensitivity explicit source — the measured fix for drain/spill iteration
 * churn. Gate/ramp derivative terms are dropped to guarantee G ≥ 0 (pure damping; the
 * denominator can only grow).
 *
 * @param[in] cp Valid resolved non-outfall structure and cell/vertex/sampling identity.
 * @param[in] mesh Prepared SI geometry, valid stencils and fallback weights.
 * @param[in] state Matching live SI cell heads/depths and optional sampling/pond-area buffers.
 * @param[in] nodes Prepared 1D node heads/depths/elevations in engine native length units.
 * @param[in] opts Positive wet threshold and resolved native-to-SI length conversion.
 * @note All selected indices and optional borrowed buffer extents are caller preconditions; no
 * canonical storage, quality, receipts or clock is changed. Passive physical parameters must be
 * finite. Caller serializes reads against live head reconstruction and node mutation.
 *
 * @return Nonnegative stabilizing sensitivity m3/(s m) to SI 1D head; nonpositive/NaN computed
 * sensitivity is returned as zero. Omits gate/ramp/equalization derivatives and the experimental
 * head offset, so this is the documented damping approximation rather than a complete law
 * derivative.
 */
double computeNodeCouplingDQdh1d(const CouplingPoint& cp, const MeshData& mesh,
                                 const SurfaceStateData& state, const NodeData& nodes,
                                 const SolverOptions2D& opts) noexcept;

/**
 * @brief Per-routing-step outfall discharge accumulation.
 *
 * @details Per-step successor of the retired transferOutfallDischarges: samples the LIVE net
 * outfall exchange (nodes.inflow − nodes.outflow, this routing step) and accumulates Q_net·dt
 * (m³, + = 1D discharge onto the 2D surface, − = surface water drawn back through the outfall)
 * into accum_m3[k]. Withdrawals are capped by the shared per-cell budget so the accumulated
 * window sink cannot overdraw the frozen 2D state.
 *
 * @param sample_row Optional (nullable): net 2D-source rate row for the interpolated-forcing
 * series — this step's capped Q_net written as +Q_net into sample_row[k] (already
 * source-positive). Junction slots left untouched.
 * @return Number of outfalls whose withdrawal was clamped this step.
 *
 * @param[in] cps Stable coupling order with valid node/cell/stencil indices.
 * @param[in] mesh Prepared topology and geometric partition weights.
 * @param[in] state Retained interface argument, unused by this implementation; budget represents
 * current provisional available surface water instead.
 * @param[in] ctx Live native node inflow/outflow sampled for each outfall.
 * @param[in] opts Resolved positive native-flow-to-m3/s conversion.
 * @param[in] dt Finite positive routing duration s; nonpositive returns zero, NaN is not
 * rejected.
 * @param[in,out] accum_m3 At least cps.size() entries; adds signed source-positive accepted m3
 * to outfall slots, preserving junction entries.
 * @param[in,out] cell_budget_m3 At least mesh.n_cells() provisional m3; withdrawals draw and
 * discharges credit it, sharing budgets across successive points/steps. Does not mutate
 * canonical state.
 * @note sample_row must have cps.size() writable entries when nonnull. A zero-rate outfall
 * leaves its sample slot untouched, as do junctions. Requires serialized bookkeeping and
 * resolved finite physical data; no shape validation or allocation occurs.
 */
int accumulateOutfallDischargeStep(const std::vector<CouplingPoint>& cps, const MeshData& mesh,
                                   const SurfaceStateData& state, const SimulationContext& ctx,
                                   const SolverOptions2D& opts, double dt,
                                   std::vector<double>& accum_m3,
                                   std::vector<double>& cell_budget_m3, double* sample_row);

/**
 * @brief Inject a per-point accumulated exchange volume into the 2D window source field.
 *
 * @details Window-fire counterpart of the per-step accumulators: converts each accumulated
 * volume to the mean rate over the window (Q_k = accum_m3[k] / window_dt) and scatters it into
 * state.coupling_flux with the given sign convention (+1: accum is already 2D-source-positive,
 * e.g. outfall discharge; −1: accum is 2D→1D-drain-positive, e.g. junction exchange, so the
 * 2D-side source is its negation). coupling_flux is NOT cleared here — callers zero it once
 * before injecting both accumulators.
 *
 * @param node_conc S3 (nullable): the 1D nodes' published concentrations, flat `[node *
 * n_species + s]` with the 2D transport stride. When given, each 2D-source volume also scatters
 * `transport.coupling_src` at the point's node concentration (callers clear coupling_src
 * alongside coupling_flux).
 *
 * @param[in] cps Matching stable point order and valid cell/vertex identities.
 * @param[in] mesh Prepared SI geometry/stencils and positive recipient areas.
 * @param[in,out] state Adds coupling_flux m/s and, for positive source rates with valid node
 * tuples, transport.coupling_src native concentration*m/s. Existing rates are retained.
 * @param[in] accum_m3 Readable vector with at least cps.size() signed accumulated m3 entries.
 * @param[in] window_dt Finite positive window duration s; nonpositive returns without work.
 * @param[in] sign Multiplier, normally +1 source-positive or -1 drain-positive; not validated.
 * @note Missing/short node_conc tuples omit species sources for that point. Does not
 * clear/consume accum_m3, change canonical water/mass or reserve donors. Caller clears shared
 * rate arrays once, serializes their scatter and supplies finite values; no allocation occurs.
 */
void injectAccumulatedExchange(const std::vector<CouplingPoint>& cps, const MeshData& mesh,
                               SurfaceStateData& state, const std::vector<double>& accum_m3,
                               double window_dt, double sign,
                               const std::vector<double>* node_conc = nullptr);

/**
 * @brief Live node-coupling orifice flux for ONE non-outfall coupling point.
 *
 * Evaluates the bidirectional capped-pipe orifice exchange Q (m³/s; > 0 drains 2D → 1D, < 0
 * spills 1D → 2D) from the CURRENT 2D state (head/depth/vert_head, reconstructed live inside the
 * CVODE RHS) against the 1D node head, which is frozen for the duration of a 2D advance()
 * window. Unlike the retired computeCouplingExchange (which pre-computed a HELD flux per window
 * and capped it by available volume / dt to stop a held drain overshooting), this is the
 * continuous form for use inside the RHS: the orifice + capped-pipe gate + the wet/dry Hermite
 * ramp on the LIVE source-side depth make Q self-limit smoothly as the cell drains, so CVODE
 * integrates the stiff coupling implicitly and stably across a large macro-window — no discrete
 * avail/dt cap needed.
 *
 * Booking/conservation is handled by the caller integrating ∫Q dt (a per-point accumulator
 * carried in the augmented state vector) over the window.
 *
 * @param provisional_vol_m3 Optional per-cell volume (m³) overriding the 2D state's own
 * depth/head for the driving-head and wet/dry-ramp terms. nullptr on the live-RHS path (CVODE's
 * state is already current). The per-routing-step decoupled path passes the window's remaining
 * withdrawal budget so the exchange self-limits as the surface drains provisionally — without it
 * the frozen window state makes the same full-rate drain repeat every sub-step and the window's
 * total exchange scales with the number of routing steps in it.
 *
 * @param[in] cp Valid resolved non-outfall structure and cell/vertex/sampling identity.
 * @param[in] mesh Prepared SI geometry, valid stencils and fallback weights.
 * @param[in] state Matching live SI cell heads/depths and optional sampling/pond-area buffers.
 * @param[in] nodes Prepared 1D node heads/depths/elevations in engine native length units.
 * @param[in] opts Positive wet threshold and resolved native-to-SI length conversion.
 * @note All selected indices and optional borrowed buffer extents are caller preconditions; no
 * canonical storage, quality, receipts or clock is changed. Passive physical parameters must be
 * finite. Caller serializes reads against live head reconstruction and node mutation.
 *
 * @param[in] h1d_offset_m Finite added node head m, used by experimental within-window head
 * ramp; zero selects frozen 1D head. It is applied after native-to-SI conversion.
 * @return Signed exchange m3/s, positive surface-to-node. Applies selected law, source-side
 * wetness, legacy crown/area gates where applicable and optional equalization cap. It does not
 * discretely reserve the requested flow or guarantee available water over an arbitrary caller
 * step.
 */
double computeNodeCouplingQ(const CouplingPoint& cp, const MeshData& mesh,
                            const SurfaceStateData& state, const NodeData& nodes,
                            const SolverOptions2D& opts, const double* provisional_vol_m3 = nullptr,
                            double h1d_offset_m = 0.0) noexcept;

/**
 * @brief Scatter a signed volumetric exchange Q (m³/s) directly onto the cell derivatives ydot[]
 * of the 2D volume ODE (for the live-RHS path).
 *
 * Same upwind-HGL stencil distribution as the held-flux scatterCouplingFlux, but adds the
 * per-cell share Q·w (Σw = 1) straight into ydot (m³/s) rather than into a coupling_flux rate,
 * so the exchange is conservative across the stencil. Sign convention matches ydot: positive Q =
 * source INTO the cells, negative = sink OUT of them (so the RHS passes −Q_drain for a 2D → 1D
 * drain).
 *
 * @param[in] mesh Prepared SI stencil geometry and partition weights.
 * @param[in] state Live matching heads used only to choose upwind weights.
 * @param[in] cp Valid resolved cell/vertex coupling; indices are unchecked.
 * @param[in] Q Signed volumetric source m3/s, positive into cells.
 * @param[in,out] ydot Nonnull writable derivative buffer with at least mesh.n_cells() entries.
 * Adds shares, retaining existing derivatives; no area division, storage advance or species
 * scatter.
 * @note Requires exclusive affected derivative slots and stable reconstruction. Allocation-free;
 * no donor-budget or finite-value validation occurs.
 */
void scatterCouplingToYdot(const MeshData& mesh, const SurfaceStateData& state,
                           const CouplingPoint& cp, double Q, double* ydot) noexcept;

/**
 * @brief Update outfall boundary depths from 2D surface heads.
 *
 * For each outfall coupled to the 2D domain, sets the outfall depth to max(h_standard, h_2d) to
 * account for dynamic tailwater from 2D flooding. Must be called before 1D routing step.
 *
 * @param cps   Coupling points.
 * @param mesh  Mesh data.
 * @param state 2D surface state.
 * @param ctx   Simulation context.
 * @param opts  2D solver options (for unit-system coupling factors).
 * @param dt    Routing step about to run (s), for TAILWATER_RELAX.
 *
 * @note Publishes only cached head_2d (native 1D length) and wet ramp, consumed by later 1D
 * boundary iterations; it does not directly overwrite node head/depth or book water. Vertex
 * wetness uses the deepest live stencil cell; optional sampling range changes stage to a
 * wet-depth mean. Multiple points for one outfall use the last point's target/ramp/relaxation.
 * Positive relaxation time uses min(1,dt/tau) after first initialization. Requires valid
 * resolved indices, positive dry_depth and stable buffers. Allocates staging arrays and may
 * throw after some ramps were written; caller serializes with routing and does not assume a
 * fully atomic boundary-cache update.
 */
void updateOutfallBoundaries(const std::vector<CouplingPoint>& cps, const MeshData& mesh,
                             const SurfaceStateData& state, SimulationContext& ctx,
                             const SolverOptions2D& opts, double dt);

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_NODE_COUPLING_HPP
