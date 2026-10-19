/**
 * @file NodeCoupling2DData.hpp
 * @brief Per-node 1D↔2D coupling structure rows ([2D_NODE_COUPLING]).
 *
 * @details One row per node that authored a [2D_NODE_COUPLING] line: the
 *          node's coupling structure type and the parameters it authored.
 *          Nodes without a row take the default type for their class
 *          (core/NodeCoupling2D.hpp). The type decides whether the node is
 *          coupleable at all: map rows ([2D_VERTEX_NODE_MAP] /
 *          [2D_TRIANGLE_NODE_MAP]) naming a NONE node are ignored.
 *
 *          Rows key on the node INDEX, so a rename needs nothing and a node
 *          delete drops and renumbers through erase_node(). Parameter values
 *          are stored as authored (project units); NaN means not authored.
 *
 * @see plans/1D2D_FINE_SCALE_COUPLING_PLAN_2026-10-19.md §5
 * @ingroup engine_data
 */

#ifndef OPENSWMM_ENGINE_NODE_COUPLING_2D_DATA_HPP
#define OPENSWMM_ENGINE_NODE_COUPLING_2D_DATA_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace openswmm {

/// Coupling structure type of a node. NONE = not coupleable.
enum class NodeCoupling2DType : int8_t {
    NONE    = 0,
    MANHOLE = 1,
    GRATE   = 2,
    CURB    = 3,
    OPEN    = 4,   ///< open-top storage / pond under the mesh
    OUTFALL = 5    ///< tailwater + discharge mechanism, not an exchange law
};

/// Optional KEY=VALUE parameters of a [2D_NODE_COUPLING] row, in write order.
enum class NodeCoupling2DKey : int {
    D = 0,              ///< manhole diameter (length)
    PERIMETER,          ///< weir crest length (length)
    AREA,               ///< orifice / open area (length²)
    CHAMBER_AREA,       ///< 1D chamber plan area above the rim (length²)
    LID_OPEN_FRACTION,  ///< open share of a covered lid, (0, 1]
    CREST,              ///< crest policy (NodeCoupling2DCrest)
    COEFFS,             ///< coefficient preset (NodeCoupling2DCoeffs)
    CW,                 ///< free-weir coefficient
    CSW,                ///< submerged-weir coefficient
    COI,                ///< orifice coefficient, surface → sewer
    COO,                ///< orifice coefficient, sewer → surface
    LAW,                ///< per-node exchange law (NodeCoupling2DLaw)
    SPREAD,             ///< outfall discharge spread radius (length)
    TAILWATER_RELAX,    ///< outfall tailwater relaxation time (s)
    COUNT
};

constexpr int kNodeCoupling2DKeys = static_cast<int>(NodeCoupling2DKey::COUNT);

/// CREST policy values (stored in the CREST parameter slot).
enum class NodeCoupling2DCrest : int { NODE_RIM = 0, MESH_BED = 1, MAX = 2, MIN = 3 };
/// COEFFS preset values (stored in the COEFFS parameter slot).
enum class NodeCoupling2DCoeffs : int {
    DEFAULT = 0, RUBINATO2017 = 1, RUBINATO2017_LOWER = 2, RUBINATO2017_UPPER = 3
};
/// LAW values (stored in the LAW parameter slot).
enum class NodeCoupling2DLaw : int { ORIFICE = 0, WEIR_ORIFICE = 1 };

using NodeCoupling2DParams = std::array<double, kNodeCoupling2DKeys>;

inline NodeCoupling2DParams unsetNodeCoupling2DParams() noexcept {
    NodeCoupling2DParams p;
    p.fill(std::numeric_limits<double>::quiet_NaN());
    return p;
}

/// Sparse [2D_NODE_COUPLING] rows (one per authored node).
struct NodeCoupling2DRows {
    std::vector<int>                  node;    ///< node index
    std::vector<NodeCoupling2DType>   type;
    std::vector<NodeCoupling2DParams> params;  ///< NaN = not authored

    int count() const noexcept { return static_cast<int>(node.size()); }

    /// Row of `node_idx`, or -1 when the node authored none.
    int row_of(int node_idx) const noexcept {
        for (std::size_t r = 0; r < node.size(); ++r)
            if (node[r] == node_idx) return static_cast<int>(r);
        return -1;
    }

    /// Row of `node_idx`, appended (type `t`, nothing authored) when missing.
    int ensure_row(int node_idx, NodeCoupling2DType t) {
        const int r = row_of(node_idx);
        if (r >= 0) return r;
        node.push_back(node_idx);
        type.push_back(t);
        params.push_back(unsetNodeCoupling2DParams());
        return count() - 1;
    }

    void erase_row(int r) {
        const auto d = static_cast<std::ptrdiff_t>(r);
        node.erase(node.begin() + d);
        type.erase(type.begin() + d);
        params.erase(params.begin() + d);
    }

    /// Node `idx` was deleted: drop its row, shift later node indices down.
    void erase_node(int idx) {
        const int r = row_of(idx);
        if (r >= 0) erase_row(r);
        for (auto& n : node)
            if (n > idx) --n;
    }

    void clear() { node.clear(); type.clear(); params.clear(); }
};

} // namespace openswmm

#endif // OPENSWMM_ENGINE_NODE_COUPLING_2D_DATA_HPP
