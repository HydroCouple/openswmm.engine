// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin

/**
 * @file      LidNode.hpp
 * @brief     Layered LID node storage, water exchange, and lifecycle operations.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_LID_NODE_HPP
#define OPENSWMM_LID_NODE_HPP
#include "../data/LidNodeData.hpp"
#include <string>
namespace openswmm {
struct SimulationContext;
namespace lidnode {
/**
 * @brief Read the initialized LID Richards mode flag for a node.
 *
 * @param[in] ctx Borrowed resolved context; no mutation or pointer retention.
 * @param[in] node Global node index resolved through the storage subtype map.
 * @return True for a joined storage state with richards enabled; false without a storage join.
 */
bool richardsMode(const SimulationContext& ctx, int node);
/**
 * @brief Read the assigned bottom-boundary policy for a node.
 *
 * @param[in] c Borrowed resolved configuration with valid subtype arrays.
 * @param[in] node Global node index; invalid values use the default.
 * @return Assigned CURRENT/SEALED/MESH_AQUIFER mode, or CURRENT for an invalid/nonassigned node.
 */
LidNodeBottom bottomMode(const SimulationContext& c, int node);
/**
 * @brief Limit and book a link transfer involving Richards or mixed retained-water ports.
 *
 * Mesh endpoints must share a pending accepted owner interval with matching dt and cannot already
 * await routed finish; violations throw before transfer mutation. Richards retained donors preserve
 * water below the pressure needed to reach the port elevation; Richards receivers admit water only
 * up to donor absolute head through their retention/storage relation. Ordinary media donors retain
 * the saturation throttle and wilting limit. Books signed per-cell port/quality water plus
 * compensating hydraulic inflow/outflow/mobile receipts; does not commit theta/water until finish.
 * General numeric/layout/material preconditions remain caller-owned.
 *
 * @param[in,out] ctx Borrowed initialized native node-LID context with coherent
 *     storage/cell/material and link/node joins; serial lifecycle ownership is required.
 * @param[in] link Valid global link with valid endpoint nodes, initialized port cells and
 *     materials.
 * @param[in] flow Finite signed aggregate ft3/s proposal, positive node1 to node2.
 * @param[in] dt Finite positive routing interval in seconds; required even for non-Mesh direct
 *     calls.
 * @return Accepted signed aggregate ft3/s after donor and receiver-head storage limits.
 */
double exchangeRichardsPorts(SimulationContext& ctx, int link, double flow, double dt);
/**
 * @brief Advance non-Mesh Richards owners privately, then publish the whole successful batch.
 *
 * Without mesh_prepared rejects any Mesh Richards owner before collecting/capturing forcing.
 * Converts other rows to private SI columns, advances the batch, rejects any failed report,
 * constructs every native publication draft and only then swaps all drafts into live state.
 * Solve/draft failure preserves live hydraulic/retained state; allocations and kernel diagnostics
 * can throw. Does not advance the global clock or perform final routing-port finish.
 *
 * @param[in,out] ctx Borrowed initialized native node-LID context with coherent
 *     storage/cell/material and link/node joins; serial lifecycle ownership is required.
 * @param[in] dt Finite positive interval in seconds for the kernel and capture accounting.
 * @param[in] evaporation Nonnegative finite potential ET in native ft/s.
 * @param[in] mesh_prepared True only after matching Mesh owner preparation; preflights those
 *     intervals and skips their independently owned columns.
 */
void prepareRichardsStep(SimulationContext& ctx, double dt, double evaporation,
                         bool mesh_prepared = false);
/**
 * @brief Recompute one initialized Richards row pressure, moisture and retained inventory.
 *
 * Sets surface retained theta to zero, inverts each porous water/geometric-volume through the SI
 * storage law, stores pressure converted to native ft and physical theta, and resums held_volume
 * ft3. Throws runtime_error on nonfinite pressure; earlier cells may already be updated. Does not
 * change conserved richards_water or mobile node volume.
 *
 * @param[in,out] ctx Borrowed initialized native node-LID context with coherent
 *     storage/cell/material and link/node joins; serial lifecycle ownership is required.
 * @param[in] row Valid dense storage row with aligned cells/richards_water/richards_pressure and
 *     valid authored retention materials.
 */
void refreshRichardsState(SimulationContext& ctx, int row);
/**
 * @brief Parse node-LID Richards numerical options and layer retention/storage materials.
 *
 * Unknown/unavailable explicit NODE stacks defer complete rows. OPTIONS enables the first stack
 * row; material rows require a valid porous layer and kernel-valid retention parameters. Duplicate
 * options/materials, trailing tokens and invalid values append errors while later rows continue.
 * Stores kernel SI retention/pressure options as authored, not hydraulic-ft conversions. Does not
 * initialize cells or advance a column.
 *
 * @param[in,out] ctx Borrowed initialized native node-LID context with coherent
 *     storage/cell/material and link/node joins; serial lifecycle ownership is required.
 * @param[in] lines [LID_RICHARDS] authored lines: control OPTIONS cells_per_layer atol rtol
 *     max_step, or control layer theta_r alpha n l specific_storage.
 */
void readRichards(SimulationContext& ctx, const std::vector<std::string>& lines);
/**
 * @brief Copy an authored node layer stack or adapt a supported conventional LID control.
 *
 * NODE copies its explicit layers. BC/RG/IT/PP adapt positive surface, PP pavement, soil and
 * storage thicknesses, converting void ratios to pore fractions; conventional storage adds
 * effectively free-draining aggregate plus a separate native-soil bottom law. Does not validate or
 * convert the full stack to hydraulic feet; callers use validateStack and the appropriate unit
 * conversion. Allocation can throw.
 *
 * @param[in] ctx Borrowed resolved LID controls; authored layer depths/conductivities remain in
 *     project rainfall/rain-depth units.
 * @param[in] control LID control index; negative/out-of-range values return an empty stack.
 * @return Owned layer copies; empty for unsupported types or absent NODE layers.
 */
std::vector<LidNodeLayer> layers(const SimulationContext& ctx, int control);
/**
 * @brief Check one authored node-layer material record.
 *
 * Surface requires positive thickness and vegetation fraction<1; media requires 0<=wilting<field
 * capacity<porosity<=1; aggregate requires positive thickness/pore fraction<=1. Bottom accepts
 * nonnegative active coefficients. Does not validate stack order, Richards retention/options or
 * treatment rules.
 *
 * @param[in] l Layer kind and seven numeric parameters, inspected without mutation.
 * @return True for finite nonnegative parameters, zero unused slots and valid kind-specific
 *     thickness/pore/moisture ordering.
 */
bool validLayer(const LidNodeLayer& l);
/**
 * @brief Validate layer material/order and optional Richards requirements.
 *
 * Requires at least one MEDIA/AGGREGATE, a SURFACE only once first and BOTTOM only once last.
 * Enabled Richards options on the first row require a surface layer, valid numerical settings and
 * retention/specific-storage records on every porous layer, with residual moisture below media
 * wilting point. Treatment rules and model references are validated separately.
 *
 * @param[in] stack Authored top-to-bottom layer records, read-only.
 * @return Empty string when valid, otherwise the first descriptive error.
 */
std::string validateStack(const std::vector<LidNodeLayer>& stack);
/**
 * @brief Sum all physical layer thicknesses while excluding the bottom boundary record.
 *
 * @param[in] stack Authored layer rows; no validation is performed.
 * @return Sum of params[0] in the stack shared rain-depth unit; zero for an empty stack.
 */
double thickness(const std::vector<LidNodeLayer>& stack);
// Validate references, layer stacks, MaxDepth and anchored offsets. Authored
// layer units remain unchanged; node/link values are canonical internal units.
/**
 * @brief Validate node-LID model references, layers, supported physics and anchored geometry.
 *
 * Checks NODE treatment rules/material stacks, DW routing and qualified quality/age/heat/reaction
 * adapters, MaxDepth equal to layer-thickness sum after conversion, and unique LID
 * endpoint/interface anchors. Conventional surface roughness/slopes/drains ignored by node
 * hydraulics generate warnings. Does not initialize cells, fix depths or stop after the first
 * error; allocations can throw.
 *
 * @param[in,out] ctx Resolved context; errors/warnings are appended while valid authored values
 *     remain unchanged.
 */
void validate(SimulationContext& ctx);
/**
 * @brief Synchronize dependent storage depth and authored outlet anchors after a control edit.
 *
 * For a valid stack converts its thickness to hydraulic ft and resets every storage using that
 * control. Repositions valid anchors at selected layer top/bottom, writing physical crest for
 * weir/rating outlets or endpoint offset for other links. Does not rebuild derived volumes/cells
 * immediately; initialize must run before simulation. Can invalidate borrowed runtime-cell
 * references.
 *
 * @param[in,out] ctx Resolved model; matching storage LID state is cleared, full depth replaced and
 *     full volume marked zero for later initialization.
 * @param[in] control LID control index; invalid/unsupported stack yields a no-op.
 */
void sync(SimulationContext& ctx, int control);
/**
 * @brief Build node-LID hydraulic cells and initial retained/mobile water state.
 *
 * Resets every storage LID state and materializes valid assigned stacks in native ft geometry.
 * Ordinary MEDIA uses five slices; Richards porous layers use configured cells_per_layer. Converts
 * authored conductivity/suction, computes geometric volume/area and initial moisture, records
 * invalid nonpositive areas, and initializes ordinary Green-Ampt or Richards water/pressure state.
 * Invalid stacks are skipped after state reset; validation is a separate phase. Without 2D support
 * a MESH_AQUIFER assignment appends an error and returns before initialization. Resets derived full
 * volumes for later preparation; allocations can leave partial state.
 *
 * @param[in,out] ctx Borrowed initialized native hydraulic/node-LID context with valid subtype
 *     joins and dimension-aligned runtime arrays; caller serializes lifecycle/routing phases.
 */
void initialize(SimulationContext& ctx);
// Reconstruct infiltration history from current physical moisture when loading
// a pre-V10 hot start. Exact continuation requires V10 history.
/**
 * @brief Initialize/reconstruct node-LID surface Green-Ampt history from physical moisture.
 *
 * Clears history/entry marker, then enables only a SURFACE over positive-conductivity MEDIA.
 * Initializes native Green-Ampt material, bounds empirical upper zone to the authored media layer
 * and seeds deficit/upper water/saturation using current hydraulic depth and retained moisture.
 * Used for pre-V10 hot starts; reconstructed history approximates prior wetting and is not an exact
 * continuation of missing cumulative history.
 *
 * @param[in,out] ctx Borrowed initialized native hydraulic/node-LID context with valid subtype
 *     joins and dimension-aligned runtime arrays; caller serializes lifecycle/routing phases.
 * @param[in] node Valid storage node with initialized positive-area cells; unchecked storage row.
 */
void initializeInfiltration(SimulationContext& ctx, int node);
/**
 * @brief Prepare one node-LID moisture/routing interval before hydraulic mutation.
 *
 * Nonpositive dt is a no-op. Prepares qualified non-Mesh Richards owners, then ordinary
 * retained/mobile split: snapshots quality water/transfers, reconciles infiltration, captures
 * positive top lateral water, removes retained ET down to wilting point and substeps vertical
 * drainage with <=1-second intervals and shared donor/receiver capacity limits. Accepted transfers
 * update Green-Ampt history. Reconciles mobile volume, derived capacity/head and temporarily
 * subtracts captured lateral flow. finishStep must run once after routing to restore forcing and
 * book receipts. Exceptions can leave partial ordinary state; Mesh owners require the paired
 * prepareStepAfterMesh path.
 *
 * @param[in,out] ctx Borrowed initialized native hydraulic/node-LID context with valid subtype
 *     joins and dimension-aligned runtime arrays; caller serializes lifecycle/routing phases.
 * @param[in] dt Finite positive accepted routing interval in seconds.
 * @param[in] evaporation Finite nonnegative potential evaporation rate in native ft/s.
 */
void prepareStep(SimulationContext& ctx, double dt, double evaporation);
// Internal mixed-mode preparation after all Mesh columns share an accepted interval.
/**
 * @brief Prepare mixed-mode LID rows after Mesh Richards owners accepted this interval.
 *
 * Requires validateRichardsRouting to confirm a prepared matching Mesh cohort, otherwise throws
 * runtime_error. Skips Mesh columns already advanced by their owner, prepares other Richards and
 * ordinary LID rows through the common path. Does not advance groundwater clocks or finish the Mesh
 * owner; caller preserves the accepted interval ordering.
 *
 * @param[in,out] ctx Borrowed initialized native hydraulic/node-LID context with valid subtype
 *     joins and dimension-aligned runtime arrays; caller serializes lifecycle/routing phases.
 * @param[in] dt Finite positive accepted routing interval in seconds.
 * @param[in] evaporation Finite nonnegative potential evaporation rate in native ft/s.
 */
void prepareStepAfterMesh(SimulationContext& ctx, double dt, double evaporation);
/**
 * @brief Finish all node-LID port transfers at a common accepted boundary.
 *
 * First checks every Mesh Richards owner agrees on accepted interval, then qualifies all pending
 * Mesh routed port states before publishing any row. Applies non-Mesh retained/Richards deltas,
 * refreshes retained inventory/capacity/head/infiltration, restores captured lateral forcing and
 * books ET/bottom-loss ft3. Mesh completion clears owned pending routing/port state. Call exactly
 * once after successful routing; repeated calls can double-book non-Mesh port/loss receipts. Does
 * not advance the global clock.
 *
 * @param[in,out] ctx Borrowed initialized native hydraulic/node-LID context with valid subtype
 *     joins and dimension-aligned runtime arrays; caller serializes lifecycle/routing phases.
 */
void finishStep(SimulationContext& ctx);
// Internal Mesh cohort boundary. Every connected Mesh endpoint must belong to
// the same accepted interval/cohort. Other LID rows and GW clocks are untouched.
/**
 * @brief Finish exactly one connected accepted Mesh Richards owner cohort.
 *
 * Validates all selected interval receipts and requires every connected Mesh endpoint belongs to
 * the same selected cohort. Sorts storage rows, privately qualifies pending routed port state then
 * finishes only those rows. Other LID rows/groundwater clocks remain untouched. Throws on
 * missing/duplicate/wrong owner, interval or connected-cohort mismatch before finish publication;
 * allocation can throw.
 *
 * @param[in,out] ctx Borrowed initialized native hydraulic/node-LID context with valid subtype
 *     joins and dimension-aligned runtime arrays; caller serializes lifecycle/routing phases.
 * @param[in] start Finite accepted interval start in native simulation seconds.
 * @param[in] end Accepted interval end in seconds matching every selected owner.
 * @param[in] nodes Nonempty unique valid node indices, each a MESH_AQUIFER Richards owner.
 */
void finishRichardsNodes(SimulationContext& ctx, double start, double end,
                         const std::vector<int>& nodes);
// Global routing preflight, before loss/flow/Picard mutation. Returns whether
// Mesh owners are present; all must have accepted the current routing interval.
/**
 * @brief Preflight the common accepted Mesh Richards routing interval without mutation.
 *
 * Every present owner must be pending finish, not already routed, start exactly at ctx.current_time
 * and share the same completed end. Throws on mismatch before hydraulic loss/flow/Picard mutation.
 * No-Mesh calls do not independently validate dt.
 *
 * @param[in] ctx Borrowed resolved context; no mutation or pointer retention.
 * @param[in] dt Finite positive routing seconds, required to match each present owner interval
 *     within eight-epsilon clock tolerance.
 * @return True when at least one MESH_AQUIFER Richards owner is present, false when none exists.
 */
bool validateRichardsRouting(const SimulationContext& ctx, double dt);
/**
 * @brief Clear trial port-transfer accumulators before a fresh hydraulic iteration.
 *
 * Throws before mutation if any Mesh Richards row has already routed and awaits finish. Otherwise
 * zeroes per-cell port_delta/mobile_delta and clears quality_ports for all storage rows. Does not
 * change retained theta, Richards water, mobile node volume or completed quality transfers; caller
 * must not discard an accepted unfinished port ledger.
 *
 * @param[in,out] ctx Borrowed initialized native hydraulic/node-LID context with valid subtype
 *     joins and dimension-aligned runtime arrays; caller serializes lifecycle/routing phases.
 */
void resetPorts(SimulationContext& ctx);
/**
 * @brief Resolve the actual hydraulic port elevation relative to a selected node invert.
 *
 * Keeps same-node crest subtraction exact by adding only the invert difference. No geometry
 * mutation or finite/index validation.
 *
 * @param[in] ctx Resolved native-ft link/node/subtype geometry with valid joins.
 * @param[in] link Valid global link index, unchecked.
 * @param[in] node Valid endpoint node index whose invert is the requested datum.
 * @return Native ft offset: physical weir/outlet crest translated from node1 datum, otherwise the
 *     matching endpoint offset.
 */
double portOffset(const SimulationContext& ctx, int link, int node);
// Physical interfaces belong to the cell above, allowing only roundoff-sized
// elevation differences. Shared by hydraulic and pollutant port coupling.
/**
 * @brief Map a hydraulic port offset to a physical LID cell with stable interface ownership.
 *
 * Intervals include the bottom and exclude the top after a 32*epsilon*max(1,abs(offset/bounds))
 * roundoff tolerance, so an exact shared interface belongs to the cell above. Physical
 * near-interface differences larger than roundoff are not snapped.
 *
 * @param[in] state Initialized top-to-bottom positive-thickness native-ft cells; no mutation.
 * @param[in] offset Finite port elevation relative to the node bottom, in ft.
 * @return Matching cell index; -1 for empty cells, otherwise surface index zero if outside all
 *     intervals.
 */
int portCell(const LidNodeState& state, double offset);
/**
 * @brief Read the port hydraulic head as a node-relative depth.
 *
 * Ordinary/non-LID returns nodes.depth. A retained SURFACE pool can raise its port head to
 * bottom+theta/porosity*thickness. A porous Richards cell returns center elevation plus its
 * native-ft pressure head. Uses current state, not pending port water; no state changes.
 *
 * @param[in] ctx Borrowed resolved context with coherent subtype/configuration/runtime arrays; no
 *     pointer is retained.
 * @param[in] node Valid global node index, unchecked.
 * @param[in] offset Finite physical port offset in ft, used for cell ownership.
 * @return Native ft head-depth for the selected hydraulic port.
 */
double portDepth(const SimulationContext& ctx, int node, double offset);
/**
 * @brief Limit and book one trial link transfer through LID mobile/retained ports.
 *
 * Delegates if either endpoint uses Richards. Ordinary LID donors below the water table share old
 * mobile+net-lateral+trial allowance; above-table ports draw retained water down to wilting with a
 * media saturation throttle. Receivers hold available above-table cell pore water and leave the
 * remainder mobile. Books equal hydraulic compensating inflow/outflow plus per-cell signed
 * water/quality receipts so retained water moves once. Does not commit theta until finish;
 * resetPorts starts a new Picard trial, so every candidate must rebuild its ledger consistently.
 *
 * @param[in,out] ctx Borrowed initialized native hydraulic/node-LID context with valid subtype
 *     joins and dimension-aligned runtime arrays; caller serializes lifecycle/routing phases.
 * @param[in] link Valid global hydraulic link index with valid endpoint nodes and actual
 *     crest/offset joins.
 * @param[in] flow Signed proposed aggregate flow in native ft3/s, positive node1 to node2.
 * @param[in] dt Finite positive accepted routing interval in seconds.
 * @return Accepted signed flow in ft3/s; nonpositive dt, zero flow or no active LID endpoints
 *     return the proposal unchanged.
 */
double exchangePorts(SimulationContext& ctx, int link, double flow, double dt);
/**
 * @brief Select the authored conductivity driving native storage bottom exfiltration.
 *
 * No unit conversion, monthly factor or clogging is applied here. Explicit SEALED/MESH_AQUIFER
 * suppress the ordinary exfiltration path; their owner supplies any qualified exchange separately.
 *
 * @param[in] c Borrowed resolved LID/storage configuration.
 * @param[in] storage_row Valid dense storage row or a negative absent sentinel; upper bounds are
 *     unchecked.
 * @return Project rainfall-intensity conductivity from an explicit BOTTOM row or storage
 *     exfil_ksat; zero for negative rows or assigned non-CURRENT bottoms.
 */
double bottomConductivity(const SimulationContext& c, int storage_row);
/**
 * @brief Evaluate the node-LID bottom clogging multiplier from cumulative treated water.
 *
 * Pore capacity excludes SURFACE cells and uses native ft3 consistently; configured clogging factor
 * is dimensionless. Does not apply monthly conductivity or mutate cumulative treated volume.
 *
 * @param[in] c Borrowed configuration and initialized cell pore volumes/treated_volume.
 * @param[in] storage_row Valid dense storage row or negative absent sentinel.
 * @return max(0,1-treated_volume/(clog_factor*porous_volume)), or one when no positive bottom
 *     clogging law/porous capacity applies.
 */
double bottomCloggingFactor(const SimulationContext& c, int storage_row);
/**
 * @brief Read retained node-LID water outside the mobile hydraulic volume.
 *
 * Does not rescan cells or reconcile pending port deltas; query after the appropriate
 * finish/publication phase for a completed inventory.
 *
 * @param[in] ctx Borrowed resolved context with coherent subtype/configuration/runtime arrays; no
 *     pointer is retained.
 * @param[in] node Global node index resolved through storage_row.
 * @return Cached retained water in native ft3, or zero for a non-storage node.
 */
double heldVolume(const SimulationContext& ctx, int node);
/**
 * @brief Check whether a node has initialized physical LID cells.
 *
 * @param[in] ctx Borrowed resolved context with coherent subtype/configuration/runtime arrays; no
 *     pointer is retained.
 * @param[in] node Global node index resolved through storage_row.
 * @return True for a storage join whose runtime cells are nonempty; false without that join.
 *     Assignment alone is insufficient.
 */
bool active(const SimulationContext& ctx, int node);
/**
 * @brief Validate a layer pollutant rule and optional legacy treatment expression.
 *
 * Validates syntax and pollutant references using the legacy treatment parser; does not attach the
 * rule, validate layer selection or evaluate a concentration. Temporary parser allocations may
 * throw.
 *
 * @param[in] ctx Borrowed coherent pollutant/node-LID state; no mutation or pointer retention.
 * @param[in] rule Known pollutant name, finite removal fraction [0,1], finite nonnegative decay in
 *     1/day and optional expression.
 * @param[in,out] error Receives validation diagnostic on failure; success does not clear a prior
 *     string.
 * @return True for a known pollutant, valid bounds and parseable expression; false otherwise.
 */
bool validTreatment(const SimulationContext& ctx, const LidLayerTreatment& rule,
                    std::string& error);
/**
 * @brief Parse authored node-LID layer treatment rows.
 *
 * Unresolved controls/stacks/pollutants defer whole rows. Rejects invalid/Bottom layer selection,
 * rule bounds/syntax and duplicate pollutant rules per layer by appending errors; successful
 * earlier rows remain installed. Converts percent removal to a fraction, retains decay/day and
 * expression text. Does not initialize runtime pollutant inventories.
 *
 * @param[in,out] ctx Borrowed resolved pollutant/node-LID context with initialized hydraulic
 *     quality-water/transfer/port receipts and aligned legacy concentration/mass arrays.
 * @param[in] lines Rows: control, one-based layer, pollutant, removal-percent, decay per day,
 *     optional expression (a lone dash means absent).
 */
void readTreatment(SimulationContext& ctx, const std::vector<std::string>& lines);
/**
 * @brief Apply accepted LID vertical/retained-port transfers and resident decay to pollutant mass.
 *
 * Requires hydraulic preparation snapshots and accepted quality transfer/port receipts; rows
 * lacking water snapshots are skipped. Lazily seeds per-cell concentration*ft3 inventories,
 * conservatively transfers mobile/lateral/retained mass, leaves solute behind during ET, applies
 * fixed/expression removal only at authored layer exits and books reaction/seep ledgers. Debits
 * retained outlet mass once and caches its fixed link-major concentration. Applies background and
 * layer decay (1/day converted to 1/s), returns updated mobile old concentration or preserves dry
 * solute for later redissolution. Mutates incoming quality rate/volume ledgers and must run once
 * per accepted interval; repeated calls double-process receipts. Allocation/expression failures can
 * leave partial state.
 *
 * @param[in,out] ctx Borrowed resolved pollutant/node-LID context with initialized hydraulic
 *     quality-water/transfer/port receipts and aligned legacy concentration/mass arrays.
 * @param[in] dt Positive native routing seconds; <=0 or no pollutants is a no-op.
 */
void prepareQuality(SimulationContext& ctx, double dt);
/**
 * @brief Refresh mobile outlet treatment against the current coupled node mixture.
 *
 * Requires prepareQuality to size/cache link-major outlets. Reads current node concentrations and
 * outgoing link direction, evaluates only treatment-bearing mobile ports, and preserves
 * already-debited retained outlet concentrations. No hydraulic water or resident mass is removed
 * here; optional ledger increment is (Cin-Cout)*abs(Q)*dt in the engine mass basis. No-pollutant
 * calls are a no-op.
 *
 * @param[in,out] ctx Borrowed resolved pollutant/node-LID context with initialized hydraulic
 *     quality-water/transfer/port receipts and aligned legacy concentration/mass arrays.
 * @param[in] dt Positive routing seconds used by expression residence-time evaluation and optional
 *     mass booking.
 * @param[in] book_reaction True only for the final accepted mixture to add removal mass to
 *     qual_routing_reacted; false during repeated mixture trials.
 */
void prepareOutletQuality(SimulationContext& ctx, double dt, bool book_reaction);
/**
 * @brief Divert the retained fraction of an arriving pollutant load into its LID port cell.
 *
 * Uses clamp(port.volume/volume,0,1), adds held_rate*dt only when the flattened cell-mass index
 * exists, and subtracts the held rate from node.qual_mass_in. Caller must ensure mass storage is
 * initialized and invoke once per arriving load to preserve accounting. Does not consume or clear
 * the port receipt.
 *
 * @param[in,out] ctx Borrowed resolved pollutant/node-LID context with initialized hydraulic
 *     quality-water/transfer/port receipts and aligned legacy concentration/mass arrays.
 * @param[in] node Valid receiving node index; no storage join returns false.
 * @param[in] link Global link identifying a positive accepted quality_ports receipt.
 * @param[in] volume Positive total arriving water volume in native ft3; <=0 returns false.
 * @param[in] pollutant Valid pollutant index, unchecked against the incoming node rate array.
 * @param[in] mass Arriving pollutant rate in engine concentration*ft3/s, despite the historical
 *     argument name.
 * @param[in] dt Positive routing seconds converting the diverted rate to retained mass.
 * @return True on the first matching positive port, even if retained mass storage was not sized;
 *     false when none exists.
 */
bool receiveQuality(SimulationContext& ctx, int node, int link, double volume, int pollutant,
                    double mass, double dt);
/**
 * @brief Read a cached treated outlet concentration with a caller fallback.
 *
 * Does not evaluate treatment, validate link/source incidence or change a cached value. Call after
 * the appropriate retained/mobile preparation phase.
 *
 * @param[in] ctx Borrowed coherent pollutant/node-LID state; no mutation or pointer retention.
 * @param[in] node Global source node index resolved through storage_row.
 * @param[in] link Global link index in the link-major concentration cache.
 * @param[in] pollutant Pollutant index used in link*n_pollutants+pollutant; valid indices expected.
 * @param[in] fallback Untreated concentration in the same engine units.
 * @return Finite cached concentration when the flattened index is within storage, otherwise
 *     fallback.
 */
double outletQuality(const SimulationContext& ctx, int node, int link, int pollutant,
                     double fallback);
/**
 * @brief Sum retained pollutant inventory across all initialized LID cells.
 *
 * Sums every cell-major quality_mass entry for the pollutant. Before lazy mass allocation, falls
 * back to heldVolume*current node concentration if available. Does not include mobile hydraulic
 * node mass or pending unprocessed transfer rates.
 *
 * @param[in] ctx Borrowed coherent pollutant/node-LID state; no mutation or pointer retention.
 * @param[in] node Global node index resolved through storage_row.
 * @param[in] pollutant Requested pollutant index; invalid values return zero.
 * @return Engine mass basis concentration*ft3, or zero without storage/valid pollutant.
 */
double heldMass(const SimulationContext& ctx, int node, int pollutant);
/**
 * @brief Parse LID storage-node assignments and bottom-boundary modes.
 *
 * Requires finite initial saturation in [0,100], a storage node and no existing assignment. Unknown
 * node/control names defer the entire row; malformed rows append diagnostics and processing
 * continues. Valid rows replace only the corresponding configuration. Does not initialize physical
 * cells or synchronize depths; partial successful rows remain when later input fails.
 *
 * @param[in,out] ctx Model name/subtype stores and LID assignment rows;
 *     errors/deferred_section_rows are appended.
 * @param[in] lines Authored [LID_NODES] lines: node control saturation-percent, optionally BOTTOM
 *     CURRENT|SEALED|MESH_AQUIFER.
 */
void readNodes(SimulationContext& ctx, const std::vector<std::string>& lines);
/**
 * @brief Parse hydraulic outlet anchors to numbered LID layer interfaces.
 *
 * Layer number must fit signed int. Unknown link names defer the row; duplicate link anchors and
 * malformed tokens append errors without aborting later rows. Physical endpoint ambiguity, actual
 * layer range and current offset agreement are checked later by validate. No hydraulic offset is
 * changed here.
 *
 * @param[in,out] ctx Model link names and LID outlet-anchor vector; errors/deferred rows are
 *     appended.
 * @param[in] lines [LID_NODE_OUTLETS] rows: link, positive integer physical layer number, TOP or
 *     BOTTOM.
 */
void readOutlets(SimulationContext& ctx, const std::vector<std::string>& lines);
} // namespace lidnode
} // namespace openswmm
#endif
