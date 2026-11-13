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
 * @file      LazyArray.hpp
 * @brief     A per-element array that is not allocated while every element holds the default.
 * @ingroup   engine_2d
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_LAZY_ARRAY_HPP
#define OPENSWMM_ENGINE_2D_LAZY_ARRAY_HPP

#include <cstddef>
#include <cstring>
#include <vector>

namespace openswmm::twoD {

/**
 * @brief An array of size() elements whose storage appears with the first non-default write.
 *
 * For optional per-cell data that most runs never touch (the API's forcing overrides, a mesh's
 * initial HGL and velocity): until a
 * value other than the default is written, every element reads as the default and nothing is
 * allocated. The first such write allocates the whole array, since an override may be set on every
 * cell at once. Reads (`v[i]`, const, by value) never allocate, so concurrent reads are safe;
 * writes go through set(). "Default" is compared bit for bit, so dense() is exactly the plain
 * array this replaces.
 */
template <class T> class LazyArray {
public:
    std::size_t size() const noexcept { return n_; }
    bool empty() const noexcept { return n_ == 0; }
    /// True once a non-default value has been written.
    bool allocated() const noexcept { return !data_.empty(); }
    /// n elements, every one at @p value (which becomes the default); releases the storage.
    void assign(std::size_t n, T value) {
        std::vector<T>().swap(data_);
        n_ = n;
        default_ = value;
    }
    /// As std::vector::resize(n, value): keeps the elements below n; @p value is the default.
    void resize(std::size_t n, T value) {
        default_ = value;
        if (!data_.empty())
            data_.resize(n, value);
        n_ = n;
    }
    /// Room for n elements once allocated; nothing to reserve before that.
    void reserve(std::size_t n) {
        if (!data_.empty())
            data_.reserve(n);
    }
    std::size_t capacity() const noexcept { return data_.capacity(); }
    T operator[](std::size_t i) const { return data_.empty() ? default_ : data_[i]; }
    void set(std::size_t i, T value) {
        if (data_.empty()) {
            if (std::memcmp(&value, &default_, sizeof(T)) == 0)
                return;
            data_.assign(n_, default_);
        }
        data_[i] = value;
    }
    /// Every element becomes @p value (the default releases the storage).
    void fill(T value) {
        if (std::memcmp(&value, &default_, sizeof(T)) == 0)
            std::vector<T>().swap(data_);
        else
            data_.assign(n_, value);
    }
    /// Replace every element x by f(x), as a loop over the plain array would.
    template <class F> void transform(F&& f) {
        if (data_.empty())
            default_ = f(default_);
        else
            for (T& x : data_)
                x = f(x);
    }
    /// One value per element: an archive's encoding.
    std::vector<T> dense() const { return data_.empty() ? std::vector<T>(n_, default_) : data_; }
    /// Replace every element from one value per element; size() becomes its size.
    void assign(const std::vector<T>& v) {
        assign(v.size(), default_);
        for (std::size_t i = 0; i < v.size(); ++i)
            set(i, v[i]);
    }

private:
    std::size_t n_ = 0;
    T default_{};
    std::vector<T> data_;
};

/// Archive a LazyArray in the plain array's fixed-extent encoding (see fixedSlots()).
template <class Archive, class T> void fixedSlot(Archive& a, LazyArray<T>& values) {
    auto dense = values.dense();
    a.fixed(dense);
    if (a.reading())
        values.assign(dense);
}

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_LAZY_ARRAY_HPP
