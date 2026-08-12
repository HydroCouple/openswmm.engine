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
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
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
