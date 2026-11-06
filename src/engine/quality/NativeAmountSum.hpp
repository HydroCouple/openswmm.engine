// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>

namespace openswmm::quality {
// Bounded error-free growing expansion of represented native amounts. Fixed
// storage also permits allocation-free funding checks at the actual spill site.
class NativeAmountSum {
public:
    static constexpr std::size_t capacity = 64;
    bool tryAdd(double x) noexcept {
        if (!std::isfinite(x)) return false;
        std::size_t out = 0;
        for (std::size_t i = 0; i < count_; ++i) {
            double y = parts_[i];
            if (std::abs(x) < std::abs(y)) std::swap(x, y);
            const double hi = x + y;
            if (!std::isfinite(hi)) return false;
            const double lo = y - (hi - x);
            if (lo != 0) parts_[out++] = lo;
            x = hi;
        }
        if (x != 0) {
            if (out == capacity) return false;
            parts_[out++] = x;
        }
        count_ = out;
        return true;
    }
    void add(double x) {
        if (!tryAdd(x))
            throw std::invalid_argument("Passive storage exchange: native aggregate overflow/capacity");
    }
    bool tryAdd(const NativeAmountSum& amount) noexcept {
        const auto copy = amount;
        for (double part : copy.parts()) if (!tryAdd(part)) return false;
        return true;
    }
    void add(const NativeAmountSum& amount) {
        if (!tryAdd(amount))
            throw std::invalid_argument("Passive storage exchange: native aggregate overflow/capacity");
    }
    bool zero() const noexcept { return count_ == 0; }
    bool negative() const noexcept { return count_ != 0 && parts_[count_ - 1] < 0; }
    bool atLeast(double amount) const noexcept {
        auto difference = *this;
        return difference.tryAdd(-amount) && !difference.negative();
    }
    bool atLeast(const NativeAmountSum& amount) const noexcept {
        auto difference = *this;
        for (double part : amount.parts()) if (!difference.tryAdd(-part)) return false;
        return !difference.negative();
    }
    bool magnitudeAtMost(double bound) const noexcept {
        auto magnitude = *this;
        if (magnitude.negative())
            for (std::size_t i = 0; i < magnitude.count_; ++i) magnitude.parts_[i] = -magnitude.parts_[i];
        return magnitude.tryAdd(-bound) && (magnitude.zero() || magnitude.negative());
    }
    double value() const {
        if (count_ > 1)
            throw std::invalid_argument("Passive storage exchange: native aggregate is not exactly representable");
        return zero() ? 0.0 : parts_[0];
    }
    // Correct rounding is a separate operation from strict value(). Exact
    // comparisons establish a finite adjacent bracket before a midpoint test.
    double rounded() const {
        if (zero()) return 0;
        double candidate = parts_[count_ - 1];
        for (int step = 0; step < 8; ++step) {
            auto difference = *this; difference.add(-candidate);
            if (difference.zero()) return candidate;
            const double neighbor = std::nextafter(candidate, difference.negative() ?
                -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity());
            if (!std::isfinite(neighbor)) break;
            auto other = *this; other.add(-neighbor);
            if (other.zero()) return neighbor;
            if (other.negative() == difference.negative()) { candidate = neighbor; continue; }
            const double lo = std::min(candidate, neighbor), hi = std::max(candidate, neighbor);
            const double gap = hi - lo;
            // Represented inputs lie on the minimum-subnormal lattice: an
            // interior value in this last interval cannot exist.
            if (gap == std::numeric_limits<double>::denorm_min()) break;
            auto midpoint = *this; midpoint.add(-lo);
            for (std::size_t i = 0; i < midpoint.count_; ++i) {
                midpoint.parts_[i] *= 2;
                if (!std::isfinite(midpoint.parts_[i]))
                    throw std::invalid_argument("Passive storage exchange: native midpoint overflow");
            }
            midpoint.add(-gap);
            if (midpoint.zero()) return (std::bit_cast<std::uint64_t>(lo) & 1) == 0 ? lo : hi;
            return midpoint.negative() ? lo : hi;
        }
        throw std::invalid_argument("Passive storage exchange: no finite exact rounding bracket");
    }
    std::span<const double> parts() const noexcept { return {parts_.data(), count_}; }
    bool operator==(const NativeAmountSum& other) const noexcept {
        return count_ == other.count_ && std::equal(parts().begin(), parts().end(), other.parts().begin());
    }
private:
    std::array<double, capacity> parts_{};
    std::size_t count_ = 0;
};
}
