/**
 * @file NodeCoupling2D.hpp
 * @brief [2D_NODE_COUPLING]: per-node coupling type — tokens, class
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

const char* typeToken(NodeCoupling2DType t) noexcept;
/// Case-insensitive; false for an unknown token.
bool parseType(std::string_view token, NodeCoupling2DType& out);

const char* keyToken(NodeCoupling2DKey k) noexcept;
/// Case-insensitive; false for an unknown key.
bool parseKey(std::string_view token, NodeCoupling2DKey& out);

/// Parse a value token for `k` into its stored double (categorical keys
/// store their enum value). Returns an error message, empty on success.
std::string parseValue(NodeCoupling2DKey k, const std::string& token, double& out);
/// The token parseValue reads back to `v`.
std::string formatValue(NodeCoupling2DKey k, double v);

/// Whether a row of type `t` may author key `k`: structure keys on
/// MANHOLE/GRATE/CURB/OPEN, SPREAD/TAILWATER_RELAX on OUTFALL, none on NONE.
bool keyAllowed(NodeCoupling2DType t, NodeCoupling2DKey k) noexcept;

/// Class default for a node without a row (D6).
NodeCoupling2DType defaultType(const SimulationContext& ctx, int node);
/// The authored type, or the class default.
NodeCoupling2DType effectiveType(const SimulationContext& ctx, int node);
/// Empty when `t` is legal for the node's class, else the reason.
std::string checkType(const SimulationContext& ctx, int node, NodeCoupling2DType t);

/// [2D_NODE_COUPLING] section handler: NODE TYPE [KEY=VALUE ...].
/// Rows naming a node declared further down are deferred to the replay.
void readSection(SimulationContext& ctx, const std::vector<std::string>& lines);

/// The authored parameters of row `r`: "KEY=VALUE KEY=VALUE" ("" when none).
std::string formatParams(const SimulationContext& ctx, int r);
/// One section line for row `r`: "NODE TYPE KEY=VALUE ..." (no newline).
std::string formatRow(const SimulationContext& ctx, int r);

} // namespace openswmm::nodecoupling2d

#endif // OPENSWMM_ENGINE_NODE_COUPLING_2D_HPP
