/**
 * @file TopoSort.hpp
 * @brief Topological sort of network links for KW/steady-state routing.
 *
 * @details Kahn's algorithm: process nodes with zero in-degree first,
 *          then their downstream neighbours. Produces a sorted link
 *          index array for upstream-to-downstream processing.
 *
 *          SoA data: InDegree[], AdjList[] (CSR format).
 *
 * @note Legacy reference: src/legacy/engine/toposort.c
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_TOPOSORT_HPP
#define OPENSWMM_TOPOSORT_HPP

#include <vector>

namespace openswmm {

struct SimulationContext;

namespace toposort {

/**
 * @brief Sort links in topological (upstream→downstream) order.
 *
 * @details Uses Kahn's algorithm with CSR adjacency from node1/node2.
 *          Returns the number of sorted links. If < n_links, a cycle exists.
 *
 * @param node1        [in] Upstream node index per link.
 * @param node2        [in] Downstream node index per link.
 * @param n_links      Number of links.
 * @param n_nodes      Number of nodes.
 * @param sorted_links [out] Topologically sorted link indices.
 * @returns Number of sorted links (< n_links indicates cycle).
 */
int sortLinks(const int* node1, const int* node2,
              int n_links, int n_nodes,
              std::vector<int>& sorted_links);

} // namespace toposort
} // namespace openswmm

#endif // OPENSWMM_TOPOSORT_HPP
