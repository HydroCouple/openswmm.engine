/**
 * @file      NodeCoupling2D.hpp
 * @brief     [2D_NODE_COUPLING]: per-node coupling type — tokens, class
 *        defaults, validation, input section and row formatting.
 *
 * @details The coupling type is a node attribute held in the model
 *          (SimulationContext::node_coupling_2d) so every editor reads and
 *          writes one value; the mesh map rows stay derived. A node without
 *          a row takes its class default:
 *            junction → MANHOLE, divider → MANHOLE, storage → OPEN,
 *            outfall → OUTFALL, inlet or virtual junction → NONE.
 *          Legal combinations: OUTFALL only on outfalls, and an outfall is
 *          OUTFALL or NONE; OPEN only on storage nodes.
 *
 * @see plans/1D2D_FINE_SCALE_COUPLING_PLAN_2026-10-19.md §5, D5/D6
 * @ingroup engine_core
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_NODE_COUPLING_2D_HPP
#define OPENSWMM_ENGINE_NODE_COUPLING_2D_HPP

#include "../data/NodeCoupling2DData.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace openswmm {
struct SimulationContext;
}

namespace openswmm::nodecoupling2d {

/**
 * @brief Borrow the canonical coupling-type token.
 *
 * @param[in] t Coupling type enum; out-of-range values use NONE fallback.
 * @return Static NUL-terminated token; never free.
 */
const char* typeToken(NodeCoupling2DType t) noexcept;
/**
 * @brief Parse a complete case-insensitive coupling-type token.
 *
 * @param[in] token Borrowed token, uppercased with Tokenizer C-locale rules; not trimmed.
 * @param[out] out Assigned only on successful token lookup; preserved on refusal.
 * @return True for NONE/MANHOLE/GRATE/CURB/OPEN/OUTFALL, false otherwise.
 */
bool parseType(std::string_view token, NodeCoupling2DType& out);

/**
 * @brief Borrow the canonical optional-parameter key.
 *
 * @param[in] k Valid NodeCoupling2DKey excluding COUNT unless a fallback is explicitly described.
 * @return Static token, or empty literal for out-of-range key; never free.
 */
const char* keyToken(NodeCoupling2DKey k) noexcept;
/**
 * @brief Parse a complete coupling-parameter key token.
 *
 * @param[in] token Borrowed untrimmed token; comparison uses tokenizer locale uppercase.
 * @param[out] out Assigned only for a recognized key, preserved otherwise.
 * @return True for a configured key excluding COUNT, false otherwise.
 */
bool parseKey(std::string_view token, NodeCoupling2DKey& out);

/**
 * @brief Parse and range-check one coupling parameter in authored units.
 *
 * Length/area values remain project length/square-length units, TAILWATER_RELAX seconds, opening
 * fraction dimensionless. Opening fraction requires (0,1], SPREAD/TAILWATER_RELAX >=0, other
 * numeric values >0. Key/type applicability is checked separately by keyAllowed().
 *
 * @param[in] k Valid NodeCoupling2DKey excluding COUNT unless a fallback is explicitly described.
 * @param[in] token Borrowed value text. CREST/COEFFS/LAW use categorical tokens; numeric keys
 *     require complete finite strtod syntax.
 * @param[out] out Receives numeric value or categorical ordinal as double. Numeric failures may
 *     modify it before refusal; categorical lookup refusal preserves it.
 * @return Empty diagnostic on success; owned key-specific explanation on refusal.
 */
std::string parseValue(NodeCoupling2DKey k, const std::string& token, double& out);
/**
 * @brief Format a stored parameter using categorical names or a round-trip numeric token.
 *
 * Numeric form tries precisions 6..17 until strtod reproduces v; no unit conversion or
 * locale-independent decimal forcing.
 *
 * @param[in] k Valid NodeCoupling2DKey excluding COUNT unless a fallback is explicitly described.
 * @param[in] v Stored value; must be finite and int-representable because the implementation casts
 *     before selecting even the numeric branch. Categorical values should be integral ordinals.
 * @return Owned categorical token or numeric %g text; invalid categorical index yields empty
 *     string.
 */
std::string formatValue(NodeCoupling2DKey k, double v);

/**
 * @brief Check parameter-family applicability for a coupling type.
 *
 * Does not validate enum bounds or node class; caller uses parseType/parseKey/checkType first.
 *
 * @param[in] t Valid coupling type.
 * @param[in] k Valid NodeCoupling2DKey excluding COUNT unless a fallback is explicitly described.
 * @return False for NONE; OUTFALL accepts only SPREAD/TAILWATER_RELAX; structure types accept all
 *     other valid keys.
 */
bool keyAllowed(NodeCoupling2DType t, NodeCoupling2DKey k) noexcept;

/**
 * @brief Select an unauthored node coupling type from its class/flags.
 *
 * @param[in] ctx Borrowed aligned model context; caller prevents concurrent structural edits.
 * @param[in] node Caller-validated existing node index; no bounds guard in this helper.
 * @return OUTFALL for outfall, OPEN for storage, MANHOLE for divider; junction uses NONE if
 *     inlet/virtual, MANHOLE otherwise.
 */
NodeCoupling2DType defaultType(const SimulationContext& ctx, int node);
/**
 * @brief Read an authored coupling type or fall back to class defaults.
 *
 * Does not revalidate a stored type against current node class or parameter rows.
 *
 * @param[in] ctx Borrowed aligned model context; caller prevents concurrent structural edits.
 * @param[in] node Caller-validated existing node index; no bounds guard in this helper.
 * @return Stored type from first matching sparse row, otherwise defaultType(ctx,node).
 */
NodeCoupling2DType effectiveType(const SimulationContext& ctx, int node);
/**
 * @brief Check basic coupling-type compatibility with one node class.
 *
 * Does not apply virtual/inlet default policy as a prohibition or validate
 * mesh-map/topology/exchange parameters.
 *
 * @param[in] ctx Borrowed aligned model context; caller prevents concurrent structural edits.
 * @param[in] node Caller-validated existing node index; no bounds guard in this helper.
 * @param[in] t Valid coupling-type enum; no independent enum-bound check.
 * @return Empty if compatible; otherwise owned reason: outfall must use OUTFALL/NONE, OUTFALL
 *     requires outfall and OPEN requires storage.
 */
std::string checkType(const SimulationContext& ctx, int node, NodeCoupling2DType t);

/**
 * @brief Parse and append validated sparse NODE TYPE KEY=VALUE authoring rows.
 *
 * Checks node existence/type, duplicate node rows, key syntax/applicability/uniqueness and values
 * before appending that row. Unknown nodes defer for one later replay. No engine lifecycle guard,
 * mesh allocation or automatic exchange-map update. Allocation failures can interrupt
 * parallel-column appends.
 *
 * @param[in,out] ctx Owned context: appends accepted rows, errors and unresolved-node deferred
 *     rows; does not clear earlier rows.
 * @param[in] lines Borrowed section strings tokenized synchronously; comment/blank-only rows are
 *     skipped.
 */
void readSection(SimulationContext& ctx, const std::vector<std::string>& lines);

/**
 * @brief Format authored non-NaN parameters in enum write order.
 *
 * Uses formatValue() representability preconditions and performs no type applicability
 * revalidation.
 *
 * @param[in] ctx Borrowed aligned model context; caller prevents concurrent structural edits.
 * @param[in] r Existing sparse coupling-row index; caller validates all column alignment.
 * @return Owned space-separated KEY=VALUE tokens, empty when all slots are NaN.
 */
std::string formatParams(const SimulationContext& ctx, int r);
/**
 * @brief Format one sparse node-coupling section row without a newline.
 *
 * NODE/TYPE head is formatted through a 96-byte fixed buffer, so a sufficiently long node name is
 * truncated. Parameter text is appended afterward without that limit.
 *
 * @param[in] ctx Borrowed aligned model context; caller prevents concurrent structural edits.
 * @param[in] r Existing sparse row with a valid referenced node and format-compatible parameter
 *     values.
 * @return Owned NODE TYPE and optional parameter string, with trailing spaces removed.
 */
std::string formatRow(const SimulationContext& ctx, int r);

} // namespace openswmm::nodecoupling2d

#endif // OPENSWMM_ENGINE_NODE_COUPLING_2D_HPP
