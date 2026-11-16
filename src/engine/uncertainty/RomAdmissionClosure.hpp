/**
 * @file RomAdmissionClosure.hpp
 * @brief Admission closure of the per-member stores (PORT_HAMILTONIAN O-8).
 *
 * The per-member solve (RomPortGraph.hpp) gives an admitted node a store V(h)
 * with a floor: its ports carry the dry factor, its negative source is a sink
 * limited by its water. A node that is not admitted is linear (S·z) and has no
 * floor. That is right where the node's level is tied by free conduits to the
 * rest of the linear network, which ends at an outfall: a member's deficit
 * there becomes less outflow at the outfall (the deviation form's linear
 * response). It is wrong where the node exchanges water with the rest ONLY
 * through ports that are floored or one-way — a floored line into an admitted
 * store, a raised-end pipe, a pump, a weir/orifice/outlet, the street orifice:
 *   - behind a floored line into an empty store, a deficit has nowhere to go
 *     but below the invert (Bellinge G71F06R_ovl behind the empty wet well
 *     G71F68Y, 68–157 ft, O-6 class C);
 *   - a one-way structure kept as a plane line q* + d·z_up ignores the
 *     downstream level, so a deficit at its upstream end runs it backwards
 *     and drains the node below it (G71F12P, a junction wet well whose only
 *     outlets are two pumps and an overflow orifice, drained G71O020 106 ft
 *     below its invert through the reversed orifice line).
 *
 * The rule (depth-1 closure, O-8b): remove the admitted nodes and keep only
 * FREE links between non-admitted nodes. A non-admitted node is admitted when
 *   (1) it is on the BOUNDARY: it is the far end of a ONE_WAY or RAISED link,
 *       or of a FREE conduit (the engine floors those at both ends), whose
 *       other end is admitted; and
 *   (2) its component of the remaining free graph is ungrounded: it contains
 *       no grounded node (an outfall, or a node the caller marks grounded) and
 *       no free link to the outside (an end at -1).
 * Only the boundary nodes are admitted, never the rest of the component.
 * Why one hop: the boundary node is where the water leaves the component the
 * wrong way (the line into the empty store, the reversed one-way line), and
 * its floor — water >= 0 — bounds what the whole component can lose through
 * it. The interior is tied to the boundary node by free two-way conduits and
 * inherits the floor through the plane, so it stays linear. The component rule
 * (admit the whole component) put an entire sub-catchment into the nonlinear
 * set whenever a basin or pump station sat at its foot: k_max 827 and a
 * 20-fold slower Bellinge smoke (O-8). A node that also has a free path to a
 * grounded part is not admitted: its deficit goes to the outfall response
 * (the deviation form's linear answer) and the floored line's dry factor, which
 * the engine applies at both ends, keeps the line from pulling it below the
 * invert. The engine applies the closure once each time the admitted set
 * grows from the other rules and does not iterate it on its own output: the
 * boundary nodes it adds are not themselves seeds for another hop.
 *
 * Pure function over the link table; no engine types.
 */
#ifndef OPENSWMM_ENGINE_UNCERTAINTY_ROM_ADMISSION_CLOSURE_HPP
#define OPENSWMM_ENGINE_UNCERTAINTY_ROM_ADMISSION_CLOSURE_HPP

#include <cstddef>
#include <vector>

namespace openswmm::uncertainty {

/// One link of the active network for the closure. Ends are active node
/// indices; −1 is the outside (an outfall, not an active node).
struct ClosureLink {
    enum Kind { FREE = 0, ONE_WAY = 1, RAISED = 2 };
    int a = -1, b = -1;
    Kind kind = FREE;   ///< FREE: a two-way conduit. ONE_WAY: pump, weir, orifice, outlet. RAISED: raised-end pipe.
};

/// Nodes (ascending) that must join the admitted set: boundary nodes (a link of
/// any kind to an admitted node) of a component of non-admitted nodes, joined
/// by FREE links between non-admitted nodes, that has no grounded node and no
/// FREE link to the outside.
/// admitted, grounded: size n (grounded may be empty = none).
inline std::vector<int> admissionClosure(int n, const std::vector<ClosureLink>& links,
                                         const std::vector<char>& admitted,
                                         const std::vector<char>& grounded = {}) {
    std::vector<int> out;
    if (n <= 0) return out;
    const auto un = static_cast<std::size_t>(n);
    auto adm = [&](int t) { return t >= 0 && static_cast<std::size_t>(t) < admitted.size() && admitted[static_cast<std::size_t>(t)]; };
    // Union–find over free links between non-admitted nodes; a free link to
    // the outside grounds its end.
    std::vector<int> parent(un);
    for (std::size_t t = 0; t < un; ++t) parent[t] = static_cast<int>(t);
    auto find = [&](int x) {
        while (parent[static_cast<std::size_t>(x)] != x) {
            parent[static_cast<std::size_t>(x)] = parent[static_cast<std::size_t>(parent[static_cast<std::size_t>(x)])];
            x = parent[static_cast<std::size_t>(x)];
        }
        return x;
    };
    std::vector<char> ground(un, 0);
    for (std::size_t t = 0; t < un && t < grounded.size(); ++t) ground[t] = grounded[t] ? 1 : 0;
    for (const auto& l : links) {
        if (l.kind != ClosureLink::FREE) continue;
        const bool va = l.a >= 0 && l.a < n, vb = l.b >= 0 && l.b < n;
        if (va && vb) {
            if (adm(l.a) || adm(l.b)) continue;   // a floored line into an admitted store
            const int ra = find(l.a), rb = find(l.b);
            if (ra != rb) parent[static_cast<std::size_t>(ra)] = rb;
        } else if (va != vb) {
            const int t = va ? l.a : l.b;         // the other end is the outside
            if (!adm(t)) ground[static_cast<std::size_t>(t)] = 1;
        }
    }
    std::vector<char> root_ground(un, 0);
    for (std::size_t t = 0; t < un; ++t)
        if (!adm(static_cast<int>(t)) && ground[t]) root_ground[static_cast<std::size_t>(find(static_cast<int>(t)))] = 1;
    std::vector<char> boundary(un, 0);
    for (const auto& l : links) {
        const bool va = l.a >= 0 && l.a < n, vb = l.b >= 0 && l.b < n;
        if (va && vb) {
            if (adm(l.a) && !adm(l.b)) boundary[static_cast<std::size_t>(l.b)] = 1;
            if (adm(l.b) && !adm(l.a)) boundary[static_cast<std::size_t>(l.a)] = 1;
        }
    }
    for (std::size_t t = 0; t < un; ++t)
        if (boundary[t] && !adm(static_cast<int>(t)) && !root_ground[static_cast<std::size_t>(find(static_cast<int>(t)))]) out.push_back(static_cast<int>(t));
    return out;
}

}  // namespace openswmm::uncertainty

#endif  // OPENSWMM_ENGINE_UNCERTAINTY_ROM_ADMISSION_CLOSURE_HPP
