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
 * @file      BoundaryData.hpp
 * @brief     Structure-of-Arrays (SoA) storage for 2D mesh boundary conditions.
 *
 * @details Stores per-edge boundary condition type, parameters, and cumulative
 *          flux tracking. Arrays are flat 2D [cell * kMaxCellVerts + edge_local],
 *          sized to n_cells * kMaxCellVerts (matching edge_flux, edge_length, etc.).
 *
 *          Boundary types:
 *          - WALL: Zero-flux (default). No water crosses the boundary.
 *          - NORMAL_FLOW: Manning outflow based on bed slope at the edge.
 *          - SPECIFIED_STAGE: Prescribed water surface elevation (constant
 *            or time-varying via timeseries).
 *
 * @see TWO_DIMENSIONAL_SURFACE_ROUTING_IMPLEMENTATION_STRATEGY.md
 * @ingroup engine_2d
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_BOUNDARY_DATA_HPP
#define OPENSWMM_ENGINE_2D_BOUNDARY_DATA_HPP

#include <unordered_map>
#include <stdexcept>
#include <cstring>
#include <utility>
#include "../coupling/RuntimeCoupling.hpp"
#include <vector>
#include <cstdint>
#include <map>
#include <string>

namespace openswmm::twoD {

/**
 * @brief Boundary condition types for 2D mesh edges.
 *
 * SPECIFIED_FLOW (3) and RATING_CURVE (4) added per GUI plan §V V-E4 / V-E5. All five types are
 * enforced at flux time by the explicit marcher via SurfaceFluxCalculator::boundaryEdgeFlux (and
 * the Kokkos backend's device-side equivalent); time-varying values are resolved each step in
 * SurfaceRouter2D::resolveBoundaryValues.
 */
enum class BoundaryType : int8_t {
    WALL = 0,            ///< Zero-flux wall (default)
    NORMAL_FLOW = 1,     ///< Manning outflow using bed slope
    SPECIFIED_STAGE = 2, ///< Prescribed water surface elevation (constant or TS)
    SPECIFIED_FLOW = 3,  ///< Prescribed discharge per metre of edge (constant or TS)
    RATING_CURVE = 4     ///< Stage → flow lookup (curve registry index)
};

/**
 * @brief Per-slot names kept only for the slots that have one.
 *
 * A boundary names a time series or rating curve only on its authored edges, but BoundaryData
 * has a slot for every edge of every cell; a std::string per slot cost 24 B each, almost all
 * empty. size() is the logical slot count (extent checks and archives use it). Reads return the
 * slot's name or an empty string and never insert, so concurrent reads are safe; writes go through
 * set(). resize() keeps the names of slots below the new size, as std::vector::resize did.
 */
class SlotNames {
public:
    std::size_t size() const noexcept { return n_; }
    bool empty() const noexcept { return n_ == 0; }
    void resize(std::size_t n) {
        if (n < n_)
            std::erase_if(names_, [n](const auto& kv) { return kv.first >= n; });
        n_ = n;
    }
    const std::string& operator[](std::size_t slot) const {
        const auto it = names_.find(slot);
        return it == names_.end() ? none() : it->second;
    }
    const std::string& at(std::size_t slot) const {
        if (slot >= n_)
            throw std::out_of_range("SlotNames::at");
        return (*this)[slot];
    }
    /// Name a slot; an empty name clears it.
    void set(std::size_t slot, std::string name) {
        if (name.empty())
            names_.erase(slot);
        else
            names_[slot] = std::move(name);
    }
    /// Edit every stored name in place (any order); a name left empty is cleared.
    template <class F> void edit(F&& f) {
        for (auto it = names_.begin(); it != names_.end();) {
            f(it->second);
            it = it->second.empty() ? names_.erase(it) : std::next(it);
        }
    }
    /// The names as a dense array of size() entries: an archive's fixed-extent encoding.
    std::vector<std::string> dense() const {
        std::vector<std::string> v(n_);
        for (const auto& [slot, name] : names_)
            v[slot] = name;
        return v;
    }
    /// Replace every name from a dense array; size() becomes its size.
    void assign(const std::vector<std::string>& v) {
        names_.clear();
        n_ = v.size();
        for (std::size_t i = 0; i < v.size(); ++i)
            if (!v[i].empty())
                names_.emplace(i, v[i]);
    }

private:
    static const std::string& none() {
        static const std::string empty;
        return empty;
    }
    std::size_t n_ = 0;
    std::unordered_map<std::size_t, std::string> names_;
};

/**
 * @brief Per-slot values kept only for the slots that differ from the array's default.
 *
 * A boundary's head, flow, slope, table indices and cumulative flux mean something only on the
 * edges that carry a boundary condition, but BoundaryData has a slot for every edge of every cell;
 * a dense array per value cost 44 B per slot (176 MB per million cells), almost all at the
 * default. Reads look and behave like the dense array's: `v[slot]` is the slot's value or the
 * default, and `v[slot] = x`, `+=` and `*=` write through a reference object. A value equal to the
 * default bit for bit is not stored, so dense() reproduces the dense array exactly (a NaN or a
 * negative zero is kept). size() is the logical slot count (extent checks and archives use it).
 * Const reads never insert, so concurrent reads are safe; writes need exclusive access, as a
 * write to a new slot allocates.
 */
template <class T> class SlotValues {
public:
    /// What a non-const `v[slot]` returns: reads the slot's value, writes through set().
    class Ref {
    public:
        operator T() const { return std::as_const(*owner_)[slot_]; }
        Ref& operator=(T value) {
            owner_->set(slot_, value);
            return *this;
        }
        Ref& operator=(const Ref& other) { return *this = static_cast<T>(other); }
        Ref& operator+=(T value) { return *this = static_cast<T>(*this) + value; }
        Ref& operator*=(T value) { return *this = static_cast<T>(*this) * value; }

    private:
        friend class SlotValues;
        Ref(SlotValues* owner, std::size_t slot) : owner_(owner), slot_(slot) {}
        SlotValues* owner_;
        std::size_t slot_;
    };

    SlotValues() = default;
    explicit SlotValues(T default_value) : default_(default_value) {}

    std::size_t size() const noexcept { return n_; }
    bool empty() const noexcept { return n_ == 0; }
    /// How many slots hold a value other than the default (what the array costs in memory).
    std::size_t stored() const noexcept { return values_.size(); }
    /// n slots, every one at @p value (which becomes the default), as std::vector::assign.
    void assign(std::size_t n, T value) {
        values_.clear();
        n_ = n;
        default_ = value;
    }
    T operator[](std::size_t slot) const {
        const auto it = values_.find(slot);
        return it == values_.end() ? default_ : it->second;
    }
    Ref operator[](std::size_t slot) { return Ref(this, slot); }
    T at(std::size_t slot) const {
        check(slot);
        return (*this)[slot];
    }
    Ref at(std::size_t slot) {
        check(slot);
        return Ref(this, slot);
    }
    void set(std::size_t slot, T value) {
        if (std::memcmp(&value, &default_, sizeof(T)) == 0)
            values_.erase(slot);
        else
            values_[slot] = value;
    }
    /// Edit every stored (non-default) value in place, in any order.
    template <class F> void edit(F&& f) {
        for (auto it = values_.begin(); it != values_.end();) {
            f(it->first, it->second);
            it = std::memcmp(&it->second, &default_, sizeof(T)) == 0 ? values_.erase(it)
                                                                      : std::next(it);
        }
    }
    /// The values as a dense array of size() entries: an archive's fixed-extent encoding.
    std::vector<T> dense() const {
        std::vector<T> v(n_, default_);
        for (const auto& [slot, value] : values_)
            v[slot] = value;
        return v;
    }
    /// Replace every value from a dense array; size() becomes its size.
    void assign(const std::vector<T>& v) {
        values_.clear();
        n_ = v.size();
        for (std::size_t i = 0; i < v.size(); ++i)
            set(i, v[i]);
    }

private:
    void check(std::size_t slot) const {
        if (slot >= n_)
            throw std::out_of_range("SlotValues::at");
    }
    std::size_t n_ = 0;
    T default_{};
    std::unordered_map<std::size_t, T> values_;
};

/**
 * @brief Archive per-slot arrays in argument order, each in the dense fixed-extent encoding.
 *
 * A SlotValues goes through a dense copy (and is rebuilt from it on a read), so an archive's
 * bytes do not depend on how the values are held in memory; any other argument is passed to
 * Archive::fixed unchanged.
 */
template <class Archive, class V> void fixedSlot(Archive& a, V& values) {
    a.fixed(values);
}
template <class Archive, class T> void fixedSlot(Archive& a, SlotValues<T>& values) {
    auto dense = values.dense();
    a.fixed(dense);
    if (a.reading())
        values.assign(dense);
}
template <class Archive, class... V> void fixedSlots(Archive& a, V&... values) {
    (fixedSlot(a, values), ...);
}

/**
 * @brief SoA storage for per-edge boundary conditions.
 *
 * All arrays are flat 2D: indexed as [cell * kMaxCellVerts + edge_local] where edge_local ∈
 * {0,1,2}. The type, wall drag and exterior flag are dense, one per slot, since the flux loop
 * reads them for every edge; the values that only a boundary edge carries are kept per stored
 * slot (SlotValues, SlotNames).
 */
struct BoundaryData {

    /// Boundary condition type per edge (cast from BoundaryType)
    std::vector<int8_t> edge_bc_type;

    /// Optional tangential wall drag: tau/rho = -Cw |ut| ut. Zero = free slip.
    std::vector<double> edge_wall_drag;
    /// Derived from authored connectivity before runtime topology exists.
    std::vector<uint8_t> edge_is_exterior;

    /// Signed bed slope for NORMAL_FLOW edges (dimensionless).
    /// Positive values drain outward; negative values prescribe inward flow.
    /// A zero slope zeroes the Manning outflow — the edge behaves as a
    /// Wall (validate_project warns; no auto-compute from bed geometry).
    SlotValues<double> edge_bed_slope;

    /// Prescribed total head for SPECIFIED_STAGE edges (m).
    /// For time-varying: updated each step from timeseries.
    SlotValues<double> edge_bc_head;

    /// Timeseries index for time-varying SPECIFIED_STAGE.
    /// -1 = constant (use edge_bc_head as-is), -2 = unresolved name,
    /// >= 0 = resolved table index into SimulationContext::tables.
    SlotValues<int> edge_bc_tseries{-1};

    /// Timeseries name for deferred resolution (cleared after resolve).
    SlotNames edge_bc_tseries_name;

    /// Cumulative boundary flux (m³, positive = outflow from domain).
    /// Tracked for mass balance reporting.
    SlotValues<double> edge_bc_cum_flux;

    // -----------------------------------------------------------------------
    // V-E4 — SPECIFIED_FLOW slots (per-metre-of-edge discharge).
    // Sign convention: outward-positive (matches edge_bc_cum_flux).
    // -----------------------------------------------------------------------

    /// Prescribed discharge per metre of edge (m³/s/m) for SPECIFIED_FLOW edges.
    /// For time-varying: updated each step from timeseries.
    SlotValues<double> edge_bc_flow;

    /// Timeseries index for time-varying SPECIFIED_FLOW.
    /// -1 = constant (use edge_bc_flow as-is), -2 = unresolved name,
    /// >= 0 = resolved table index into SimulationContext::tables.
    SlotValues<int> edge_bc_flow_tseries{-1};

    /// Timeseries name for deferred resolution (cleared after resolve).
    SlotNames edge_bc_flow_tseries_name;

    // -----------------------------------------------------------------------
    // V-E5 — RATING_CURVE slot (stage → flow lookup).
    // -----------------------------------------------------------------------

    /// Curve registry index for RATING_CURVE edges.
    /// -1 = unset, -2 = unresolved name, >= 0 = resolved curve index.
    SlotValues<int> edge_bc_rating_curve{-1};

    /// Curve name for deferred resolution (cleared after resolve).
    SlotNames edge_bc_rating_curve_name;

    struct RuntimeOriginal {
        int8_t type;
        double head, flow, slope;
        int stage_ts, flow_ts, rating;
        std::string stage_name, flow_name, rating_name;
    };
    std::map<int, RuntimeOriginal> runtime_original;
    std::map<int, std::string> runtime_owner;
    mutable std::map<int, RuntimeReceipt> receipts;
    /**
     * @brief Ensure a boundary receipt has the selected transport tuple width.
     * @param[in] slot Internal padded edge slot, MeshData::slot(cell,edge); may create a new
     * receipt map entry.
     * @param[in] species Nonnegative number of transported species.
     * @post If applied_mass has a different extent, both species vectors reset to zeros.
     * Existing cumulative water/heat totals are preserved.
     * @note This const method mutates the mutable receipts map. Requires exclusive receipt
     * access, may allocate/throw, and may leave partial vectors on failure.
     */
    void ensureReceipt(int slot, int species) const {
        auto& receipt = receipts[slot];

        if (receipt.applied_mass.size() != static_cast<std::size_t>(species)) {
            receipt.requested_mass.assign(species, 0.0);
            receipt.applied_mass.assign(species, 0.0);
        }
    }
    /**
     * @brief Capture a boundary's authored prescription before its first runtime override.
     * @param[in] slot Valid internal padded edge slot across every boundary array.
     * @post Saves type, scalar values, table indices and table names only if no original exists;
     * repeated calls preserve the first baseline.
     * @note Does not save cumulative flux or species receipts. Requires exclusive access;
     * map/string allocation may throw. No slot/extent validation.
     */
    void saveRuntime(int slot) {
        if (runtime_original.count(slot))
            return;
        runtime_original.emplace(
            slot,
            RuntimeOriginal{edge_bc_type[slot], edge_bc_head[slot], edge_bc_flow[slot],
                            edge_bed_slope[slot], edge_bc_tseries[slot], edge_bc_flow_tseries[slot],
                            edge_bc_rating_curve[slot], edge_bc_tseries_name[slot],
                            edge_bc_flow_tseries_name[slot], edge_bc_rating_curve_name[slot]});
    }
    /**
     * @brief Restore a saved authored boundary prescription for one runtime slot.
     * @param[in] slot Valid internal edge slot with sized boundary arrays.
     * @post Restores saved scalar/table/name fields, erases its baseline and runtime owner, and
     * preserves cumulative flux/receipts. An absent baseline is a no-op, including its owner
     * map.
     * @note String assignments may allocate/throw after partial scalar restoration. Quality
     * overrides and lookup-cache invalidation are the caller's responsibility; requires
     * exclusive access.
     */
    void clearRuntime(int slot) {
        const auto it = runtime_original.find(slot);
        if (it == runtime_original.end())
            return;
        const auto& b = it->second;
        edge_bc_type[slot] = b.type;
        edge_bc_head[slot] = b.head;
        edge_bc_flow[slot] = b.flow;
        edge_bed_slope[slot] = b.slope;
        edge_bc_tseries[slot] = b.stage_ts;
        edge_bc_flow_tseries[slot] = b.flow_ts;
        edge_bc_rating_curve[slot] = b.rating;
        edge_bc_tseries_name.set(slot, b.stage_name);
        edge_bc_flow_tseries_name.set(slot, b.flow_name);
        edge_bc_rating_curve_name.set(slot, b.rating_name);
        runtime_original.erase(it);
        runtime_owner.erase(slot);
    }

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    /**
     * @brief Resize all arrays to n_edges and initialize to WALL defaults.
     * @param n_edges Total number of edge slots (n_cells * kMaxCellVerts).
     *
     * @pre n_edges is nonnegative. Call only during allocation/reset, with exclusive access.
     * @post Clears runtime owners/originals/receipts and zeroes cumulative flux. Numeric arrays
     * receive wall defaults; resized string vectors retain names in existing slots, so this is
     * not a complete string reset.
     * @note Allocations may throw after partial resizing; prior references/iterators can be
     * invalidated. No transactional guarantee.
     */
    void resize(int n_edges) {
        runtime_original.clear();
        runtime_owner.clear();
        receipts.clear();
        auto n = static_cast<std::size_t>(n_edges);
        edge_bc_type.assign(n, static_cast<int8_t>(BoundaryType::WALL));
        edge_bed_slope.assign(n, 0.0);
        edge_wall_drag.assign(n, 0.0);
        edge_is_exterior.assign(n, 0);
        edge_bc_head.assign(n, 0.0);
        edge_bc_tseries.assign(n, -1);
        edge_bc_tseries_name.resize(n);
        edge_bc_cum_flux.assign(n, 0.0);
        edge_bc_flow.assign(n, 0.0);
        edge_bc_flow_tseries.assign(n, -1);
        edge_bc_flow_tseries_name.resize(n);
        edge_bc_rating_curve.assign(n, -1);
        edge_bc_rating_curve_name.resize(n);
    }

    /**
     * @brief Report the number of padded boundary edge slots.
     * @return edge_bc_type.size() narrowed to int; includes interior/padding slots. No mutation
     * or allocation; all boundary arrays are expected to have matching extents.
     */
    int size() const noexcept { return static_cast<int>(edge_bc_type.size()); }
};

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_BOUNDARY_DATA_HPP
