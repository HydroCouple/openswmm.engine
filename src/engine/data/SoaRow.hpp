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
 * @file      SoaRow.hpp
 * @brief     Take one row out of a structure-of-arrays store and put it back.
 *
 * A store opts in by declaring `template <class F> static void for_each_column(F&& f)`, which
 * calls f with a pointer to each per-row std::vector member. The functions here use that one
 * list, so a store's erase and restore cannot disagree about its columns. A column shorter than
 * the row index is left alone and stays empty in the taken row, which makes put_row(idx, row)
 * the exact inverse of take_row(idx). Chunked columns (several cells per row) are not listed;
 * each store handles those itself.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_SOA_ROW_HPP
#define OPENSWMM_ENGINE_SOA_ROW_HPP

#include <cstddef>
#include <utility>
#include <vector>

namespace openswmm::soa {

/**
 * @brief Remove row idx from every listed column and return it as a one-row store.
 * @param[in,out] store Store with a static for_each_column.
 * @param[in] idx Zero-based row; columns too short to hold it are left alone.
 * @return Store whose columns each hold that row's cell, or nothing.
 */
template <class Store>
Store take_row(Store& store, int idx) {
    Store row{};
    const auto ui = static_cast<std::size_t>(idx);
    Store::for_each_column([&](auto member) {
        auto& v = store.*member;
        if (ui >= v.size())
            return;
        (row.*member).push_back(std::move(v[ui]));
        v.erase(v.begin() + static_cast<std::ptrdiff_t>(idx));
    });
    return row;
}

/**
 * @brief Copy row idx out of every listed column without changing the store.
 * @param[in] store Store with a static for_each_column.
 * @param[in] idx Zero-based row; columns too short to hold it contribute nothing.
 * @return One-row store, as take_row would return.
 */
template <class Store>
Store copy_row(const Store& store, int idx) {
    Store row{};
    const auto ui = static_cast<std::size_t>(idx);
    Store::for_each_column([&](auto member) {
        const auto& v = store.*member;
        if (ui < v.size())
            (row.*member).push_back(v[ui]);
    });
    return row;
}

/**
 * @brief Insert a row returned by take_row or copy_row at idx; later rows shift up.
 * @param[in,out] store Store with a static for_each_column.
 * @param[in] idx Insert position; each column the row holds a cell for must have at least idx
 *     cells.
 * @param[in] row One-row store; its cells are moved out.
 */
template <class Store>
void put_row(Store& store, int idx, Store&& row) {
    Store::for_each_column([&](auto member) {
        auto& from = row.*member;
        if (from.empty())
            return;
        auto& v = store.*member;
        v.insert(v.begin() + static_cast<std::ptrdiff_t>(idx), std::move(from.front()));
    });
}

/**
 * @brief Remove the `stride` cells of row idx from a flat [row * stride + k] column.
 * @return The removed cells, or nothing when stride is not positive or the column is too short.
 */
template <class T>
std::vector<T> take_chunk(std::vector<T>& v, int idx, int stride) {
    std::vector<T> chunk;
    if (stride <= 0)
        return chunk;
    const auto base = static_cast<std::size_t>(idx) * static_cast<std::size_t>(stride);
    const auto end = base + static_cast<std::size_t>(stride);
    if (end > v.size())
        return chunk;
    chunk.assign(v.begin() + static_cast<std::ptrdiff_t>(base),
                 v.begin() + static_cast<std::ptrdiff_t>(end));
    v.erase(v.begin() + static_cast<std::ptrdiff_t>(base), v.begin() + static_cast<std::ptrdiff_t>(end));
    return chunk;
}

/**
 * @brief Insert a chunk returned by take_chunk back at row idx; nothing when it is empty.
 * @param[in] stride Must equal the stride the chunk was taken with.
 */
template <class T>
void put_chunk(std::vector<T>& v, int idx, int stride, const std::vector<T>& chunk) {
    if (chunk.empty())
        return;
    const auto base = static_cast<std::ptrdiff_t>(idx) * stride;
    v.insert(v.begin() + base, chunk.begin(), chunk.end());
}

} // namespace openswmm::soa

#endif // OPENSWMM_ENGINE_SOA_ROW_HPP
