/**
 * @file TopoSort.cpp
 * @brief Topological sort — Kahn's algorithm, numerically identical to legacy.
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "TopoSort.hpp"
#include <vector>

namespace openswmm {
namespace toposort {

int sortLinks(const int* node1, const int* node2,
              int n_links, int n_nodes,
              std::vector<int>& sorted_links) {
    sorted_links.clear();
    sorted_links.reserve(static_cast<std::size_t>(n_links));

    // Build CSR adjacency: for each node, list outgoing links
    std::vector<int> degree(static_cast<std::size_t>(n_nodes), 0);
    std::vector<int> in_degree(static_cast<std::size_t>(n_nodes), 0);

    // Count outgoing links per node
    for (int j = 0; j < n_links; ++j) {
        int n1 = node1[j];
        if (n1 >= 0 && n1 < n_nodes) degree[static_cast<std::size_t>(n1)]++;
    }

    // Build CSR offsets
    std::vector<int> start_pos(static_cast<std::size_t>(n_nodes + 1), 0);
    for (int i = 0; i < n_nodes; ++i) {
        start_pos[static_cast<std::size_t>(i + 1)] =
            start_pos[static_cast<std::size_t>(i)] + degree[static_cast<std::size_t>(i)];
    }

    // Fill adjacency list
    std::vector<int> adj_list(static_cast<std::size_t>(n_links));
    std::vector<int> cursor = start_pos;  // copy for filling
    for (int j = 0; j < n_links; ++j) {
        int n1 = node1[j];
        if (n1 >= 0 && n1 < n_nodes) {
            adj_list[static_cast<std::size_t>(cursor[static_cast<std::size_t>(n1)]++)] = j;
        }
    }

    // Compute in-degree
    for (int j = 0; j < n_links; ++j) {
        int n2 = node2[j];
        if (n2 >= 0 && n2 < n_nodes) in_degree[static_cast<std::size_t>(n2)]++;
    }

    // Kahn's algorithm: stack of zero in-degree nodes
    std::vector<int> stack;
    stack.reserve(static_cast<std::size_t>(n_nodes));
    for (int i = 0; i < n_nodes; ++i) {
        if (in_degree[static_cast<std::size_t>(i)] == 0)
            stack.push_back(i);
    }

    int first = 0;
    while (first < static_cast<int>(stack.size())) {
        int i1 = stack[static_cast<std::size_t>(first++)];
        auto ui1 = static_cast<std::size_t>(i1);

        int k1 = start_pos[ui1];
        int k2 = start_pos[ui1 + 1];

        for (int k = k1; k < k2; ++k) {
            int j = adj_list[static_cast<std::size_t>(k)];
            sorted_links.push_back(j);

            int i2 = node2[j];
            if (i2 >= 0 && i2 < n_nodes) {
                auto ui2 = static_cast<std::size_t>(i2);
                in_degree[ui2]--;
                if (in_degree[ui2] == 0) {
                    stack.push_back(i2);
                }
            }
        }
    }

    return static_cast<int>(sorted_links.size());
}

} // namespace toposort
} // namespace openswmm
