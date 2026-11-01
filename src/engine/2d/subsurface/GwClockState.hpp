// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "GwClock.hpp"
#include <array>
#include <cstdint>
#include <map>
#include <memory>

namespace openswmm::twoD {
// Canonical accumulator values still include every accepted reservation. These
// sparse records identify how much of each slot can be gathered at a GW event.
enum class GwSlot {
    surface,
    surface_mass,
    et,
    node,
    node_mass,
    link,
    link_mass,
    link_export,
    contact_water,
    contact_node_mass,
    contact_link_mass,
    contact_node_out,
    contact_link_out,
    bed_down,
    bed_up,
    heat_link_volume,
    heat_node_volume,
    prepaid_heat,
    prepaid_water,
    prepaid_mass,
    count
};
struct GwDatedSlot {
    GwSlot field{};
    std::size_t index = 0;
    std::vector<std::pair<double, double>> packets; // producer end, signed amount
};
struct GwClockState {
    std::optional<GwClock> schedule;
    std::vector<int> cells, faces, dense;
    std::vector<double> link_out;      // gross paid bulk withdrawals awaiting their cell
    std::vector<double> receipt_limit; // next period from delivered directional rates
    std::vector<unsigned char> budget_active;
    std::vector<std::vector<GwDatedSlot>> receipts;
    std::size_t pending_slots = 0; // surface workers update disjoint rows concurrently
    std::array<std::uint64_t, 3> firings{}, subcycles{};
    std::array<std::uint64_t, 3> budget_firings{};
    std::array<double, 3> excess_in{}, excess_out{};
    std::array<double, 3> max_return_depth{};
    // Maximum directional exceedance / budget: zero, <=.25, <=.5, <=1, >1.
    std::array<std::array<std::uint64_t, 5>, 3> budget_exceedance{};
    std::array<double, 3> elapsed{}, maximum{};
    // One observation per accepted scheduled cell interval. Bins: <1, <2,
    // <4, <8, <16, >=16, and unbounded previous stability.
    std::array<std::uint64_t, 7> stability_to_period{};
    std::map<int, long> rung_batches;
};

// MSVC map/set moves can allocate sentinel nodes. Keep the optional clock
// behind an owning pointer so accepted solver publication remains nonthrowing;
// trial copies still deep-copy every schedule, receipt and histogram entry.
class GwClockStateOwner {
public:
    GwClockStateOwner() = default;
    GwClockStateOwner(const GwClockStateOwner& other)
        : state_(other.state_ ? std::make_unique<GwClockState>(*other.state_) : nullptr) {}
    GwClockStateOwner& operator=(const GwClockStateOwner& other) {
        if (this != &other)
            state_ = other.state_ ? std::make_unique<GwClockState>(*other.state_) : nullptr;
        return *this;
    }
    GwClockStateOwner(GwClockStateOwner&&) noexcept = default;
    GwClockStateOwner& operator=(GwClockStateOwner&&) noexcept = default;

    explicit operator bool() const noexcept { return bool(state_); }
    GwClockState& operator*() noexcept { return *state_; }
    const GwClockState& operator*() const noexcept { return *state_; }
    GwClockState* operator->() noexcept { return state_.get(); }
    const GwClockState* operator->() const noexcept { return state_.get(); }
    void reset() noexcept { state_.reset(); }
    void emplace(GwClockState&& state) {
        state_ = std::make_unique<GwClockState>(std::move(state));
    }

private:
    std::unique_ptr<GwClockState> state_;
};
}
