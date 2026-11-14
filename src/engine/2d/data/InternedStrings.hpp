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
 * @file      InternedStrings.hpp
 * @brief     A per-element string column that stores each distinct string once.
 * @ingroup   engine_2d
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_INTERNED_STRINGS_HPP
#define OPENSWMM_ENGINE_2D_INTERNED_STRINGS_HPP

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace openswmm::twoD {

/**
 * @brief One string per element (a cell's tag, a vertex's coupled node name), held as a 2-byte
 *        index into the distinct strings.
 *
 * A mesh's tags and coupled node names are empty or drawn from a few values, but a std::string per
 * element costs 24 B each. Here an element costs 2 B and each distinct string is stored once. A
 * column that reaches 65,536 distinct strings (say, a unique tag per vertex) switches, for good,
 * to one std::string per element, so it never costs more than the plain array did.
 *
 * Reads (`v[i]`, const) never modify the column, so concurrent reads are safe; treat the returned
 * reference as valid until the next write. Writes go through set(). dense() / assign() convert to
 * and from a std::vector<std::string> for archives.
 */
class InternedStrings {
public:
    std::size_t size() const noexcept { return interned_ ? ids_.size() : strings_.size(); }
    bool empty() const noexcept { return size() == 0; }
    /// As std::vector::resize: keeps the elements below n, new elements are empty.
    void resize(std::size_t n) {
        if (interned_)
            ids_.resize(n, 0);
        else
            strings_.resize(n);
    }
    void reserve(std::size_t n) {
        if (interned_)
            ids_.reserve(n);
        else
            strings_.reserve(n);
    }
    std::size_t capacity() const noexcept {
        return interned_ ? ids_.capacity() : strings_.capacity();
    }
    const std::string& operator[](std::size_t i) const {
        return interned_ ? table_[ids_[i]] : strings_[i];
    }
    void set(std::size_t i, const std::string& value) {
        if (!interned_) {
            strings_[i] = value;
            return;
        }
        // A reader sets one element per row: most are empty or repeat the previous row's tag.
        if (value.empty()) {
            ids_[i] = 0;
            return;
        }
        if (value == table_[last_]) {
            ids_[i] = last_;
            return;
        }
        const auto found = index_.find(value);
        if (found != index_.end()) {
            ids_[i] = last_ = found->second;
            return;
        }
        if (table_.size() > kMaxId) { // too many distinct strings to index in 2 bytes
            strings_ = dense();
            strings_.reserve(ids_.capacity());
            strings_[i] = value;
            interned_ = false;
            ids_ = {};
            table_ = {};
            index_ = {};
            return;
        }
        const auto id = static_cast<std::uint16_t>(table_.size());
        table_.push_back(value);
        index_.emplace(value, id);
        ids_[i] = last_ = id;
    }
    /// True while elements are 2-byte indices (fewer than 65,537 distinct strings so far).
    bool interned() const noexcept { return interned_; }
    /// One std::string per element: an archive's encoding.
    std::vector<std::string> dense() const {
        if (!interned_)
            return strings_;
        std::vector<std::string> v(ids_.size());
        for (std::size_t i = 0; i < ids_.size(); ++i)
            v[i] = table_[ids_[i]];
        return v;
    }
    /// Replace every element from one std::string per element.
    void assign(const std::vector<std::string>& v) {
        *this = InternedStrings{};
        resize(v.size());
        for (std::size_t i = 0; i < v.size(); ++i)
            if (!v[i].empty())
                set(i, v[i]);
    }

private:
    static constexpr std::size_t kMaxId = 65535;
    bool interned_ = true;
    std::vector<std::uint16_t> ids_;                         ///< index into table_; 0 = empty
    std::uint16_t last_ = 0;                                 ///< the id set() used last
    std::deque<std::string> table_{std::string{}};           ///< distinct strings, [0] empty
    std::unordered_map<std::string, std::uint16_t> index_{{std::string{}, 0}};
    std::vector<std::string> strings_;                       ///< used once not interned_
};

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_INTERNED_STRINGS_HPP
