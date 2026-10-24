// SPDX-License-Identifier: Apache-2.0

/**
 * @file      RuntimeCoupling.hpp
 * @brief     Runtime coupling state and canonical transport exchange quantities.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_RUNTIME_COUPLING_HPP
#define OPENSWMM_RUNTIME_COUPLING_HPP
#include <algorithm>
#include <array>
#include <unordered_map>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace openswmm::twoD {
// Transport rows store concentration * SI volume, including temperature-volume.
// Heat receipts use sensible energy relative to 0 C; no soil energy is implied.
inline constexpr double runtimeWaterHeatCapacity = 4186000.0; // J/(m3 K)
struct RuntimeReceipt {
    double requested = 0.0, applied = 0.0;
    double water_in = 0.0, water_out = 0.0;
    double heat_requested = 0.0, heat_applied = 0.0;
    double last_flow = 0.0, last_dt = 0.0;
    std::vector<double> requested_mass, applied_mass;
};
struct RuntimeSource {
    std::string id, owner;
    int cell = -1;
    double flow = 0.0, heat = 0.0;
    int boundary_edge = -1;
    double boundary_head = std::numeric_limits<double>::quiet_NaN();
    double until = std::numeric_limits<double>::infinity();
    bool enabled = true;
    std::vector<double> concentrations, rates;
    RuntimeReceipt receipt;
};
struct RuntimeSources {
    std::vector<RuntimeSource> rows;
    std::vector<std::vector<std::size_t>> cells;
    std::unordered_map<std::string, std::unordered_map<int, std::size_t>> index;
    /**
     * @brief Find a source prescription by its exact registry key.
     * @param[in] id Borrowed case-sensitive source identifier.
     * @param[in] cell Zero-based source cell key; may identify a disabled retained row.
     * @return Borrowed pointer into rows, or NULL when the key is absent. Pointer may be
     * invalidated by vector growth, replacement or registry destruction; never free it.
     * @pre index matches rows. No allocation or mutation; serialize access with registry edits.
     */
    RuntimeSource* find(const std::string& id, int cell) {
        const auto a = index.find(id);
        if (a == index.end())
            return nullptr;
        const auto b = a->second.find(cell);
        return b == a->second.end() ? nullptr : &rows[b->second];
    }
    /**
     * @brief Find a source prescription by its exact registry key.
     * @param[in] id Borrowed case-sensitive source identifier.
     * @param[in] cell Zero-based source cell key; may identify a disabled retained row.
     * @return Borrowed pointer into rows, or NULL when the key is absent. Pointer may be
     * invalidated by vector growth, replacement or registry destruction; never free it.
     * @pre index matches rows. No allocation or mutation; serialize access with registry edits.
     */
    const RuntimeSource* find(const std::string& id, int cell) const {
        return const_cast<RuntimeSources*>(this)->find(id, cell);
    }
    /**
     * @brief Recreate enabled source-to-cell gather indices.
     * @param[in] count Nonnegative mesh cell count, in cells.
     * @pre Every enabled row.cell lies in [0,count); rows/index are otherwise coherent.
     * @post Replaces cells with count vectors of enabled row offsets. Does not rebuild the
     * id/cell index or clear receipts.
     * @throws std::out_of_range For an enabled cell outside count; partial cells may already be
     * written.
     * @note May allocate/throw and invalidates cell-list references. Requires exclusive registry
     * access; no rollback.
     */
    void rebuild(int count) {
        cells.assign(count, {});
        for (std::size_t k = 0; k < rows.size(); ++k)
            if (rows[k].enabled)
                cells.at(rows[k].cell).push_back(k);
    }
    /**
     * @brief Upsert a source prescription while preserving accumulated receipts.
     * @param[in] value Owned draft with valid id/cell and transport tuple extent; moved into
     * rows.
     * @post Existing keys retain their receipt; new keys receive zeroed species receipt arrays
     * sized to concentrations. Caller must rebuild cell gathers after changing
     * placement/enabling.
     * @note No validation is performed. Vector/map allocations may throw after index/receipt
     * changes, so this low-level method is not transactional; edit a private draft when rollback
     * is needed. Existing row pointers may be invalidated.
     */
    void replace(RuntimeSource value) {
        if (auto* row = find(value.id, value.cell)) {
            value.receipt = std::move(row->receipt);
            *row = std::move(value);
            return;
        }
        index[value.id][value.cell] = rows.size();
        value.receipt.requested_mass.assign(value.concentrations.size(), 0.0);
        value.receipt.applied_mass.assign(value.concentrations.size(), 0.0);
        rows.push_back(std::move(value));
    }
    /**
     * @brief Remove disabled row offsets from every cell gather list.
     * @pre Cell offsets reference valid rows. Requires exclusive registry access.
     * @post rows, id/cell keys and all receipts remain intact; cell-list iterators may be
     * invalidated. No allocation or exceptions.
     */
    void prune() noexcept {
        for (auto& cell : cells)
            cell.erase(std::remove_if(cell.begin(), cell.end(),
                                      [this](auto k) { return !rows[k].enabled; }),
                       cell.end());
    }
    /**
     * @brief Disable one source-cell key or every cell belonging to a source id.
     * @param[in] id Borrowed exact source identifier.
     * @param[in] cell Nonnegative cell key to clear, or any negative value for all cells under
     * id.
     * @return False for an absent id/cell; true when a matching key exists, even if already
     * disabled.
     * @post Removes disabled entries from gather lists, preserving rows/index/receipts for later
     * reporting or replacement. Requires exclusive access; no allocation or exceptions.
     */
    bool clear(const std::string& id, int cell = -1) noexcept {
        const auto found = index.find(id);
        if (found == index.end())
            return false;
        if (cell >= 0) {
            auto* row = find(id, cell);
            if (!row)
                return false;
            row->enabled = false;
            if (cell < static_cast<int>(cells.size())) {
                auto& active = cells[cell];
                active.erase(std::remove_if(active.begin(), active.end(),
                                            [this](auto k) { return !rows[k].enabled; }),
                             active.end());
            }
            return true;
        } else
            for (const auto& entry : found->second)
                rows[entry.second].enabled = false;
        prune();
        return true;
    }
    /**
     * @brief Copy matching accepted receipts into a staged source registry.
     * @param[in] old Borrowed prior registry with coherent index/rows; distinct from this
     * registry when edits are in flight.
     * @post Every matching (id,cell) receives the previous receipt, including species vectors;
     * unmatched drafts remain unchanged.
     * @note Allocations may throw after earlier rows were copied. No topology rebuild or input
     * retention; caller publishes only a fully prepared private draft.
     */
    void syncReceipts(const RuntimeSources& old) {
        for (auto& row : rows)
            if (const auto* previous = old.find(row.id, row.cell))
                row.receipt = previous->receipt;
    }
    /**
     * @brief Test whether any retained runtime prescription is enabled.
     * @return True if at least one row.enabled is true, otherwise false. Expiry time is not
     * inspected; expire() must run first for time-based status.
     * @note Read-only, allocation-free; serialize with registry mutations.
     */
    bool active() const {
        for (const auto& r : rows)
            if (r.enabled)
                return true;
        return false;
    }
    /**
     * @brief Find the next enabled prescription expiry strictly after now.
     * @param[in] now Finite elapsed simulation time, seconds.
     * @return Earliest enabled until > now in elapsed seconds, or positive infinity if none
     * exists.
     * @note Read-only; expired/equal-time rows are skipped without disabling them. No
     * allocation; serialize with edits.
     */
    double nextTime(double now) const {
        double next = std::numeric_limits<double>::infinity();
        for (const auto& r : rows)
            if (r.enabled && r.until > now)
                next = std::min(next, r.until);
        return next;
    }
    /**
     * @brief Disable all enabled prescriptions whose expiry has been reached.
     * @param[in] now Finite elapsed simulation time, seconds; until <= now expires.
     * @post Keeps row identity and cumulative receipts; prunes disabled cell gather indices.
     * @return True when at least one enabled source was disabled; false otherwise.
     * @note Requires exclusive registry access. No allocation or exceptions.
     */
    bool expire(double now) {
        bool changed = false;
        for (auto& r : rows)
            if (r.enabled && r.until <= now) {
                r.enabled = false;
                changed = true;
            }
        if (changed)
            prune();
        return changed;
    }
};
// Runtime forcing channels: rain, evaporation, paired infiltration, rain quality.
struct RuntimeForcing {
    int channel = 0, cell = -1, mode = 1;
    double rate = 0.0, until = std::numeric_limits<double>::infinity();
    bool enabled = true;
    std::string owner;
    std::vector<double> concentrations;
    RuntimeReceipt receipt;
};
struct RuntimeForcings {
    std::vector<RuntimeForcing> rows;
    std::vector<std::array<int, 4>> cells;
    std::array<int, 4> uniform{{-1, -1, -1, -1}};
    std::unordered_map<int, std::unordered_map<int, std::size_t>> index;
    /**
     * @brief Find an exact channel/cell forcing row, including disabled retained rows.
     * @param[in] channel Runtime forcing channel key (normally 1..4).
     * @param[in] cell Zero-based cell key, or negative uniform key as authored.
     * @return Borrowed mutable row pointer or NULL for no key. Invalidated by rows
     * growth/replacement/destruction; never free it.
     * @pre index and rows are coherent. No allocation/mutation; serialize with edits.
     */
    RuntimeForcing* find(int channel, int cell) {
        auto a = index.find(channel);
        if (a == index.end())
            return nullptr;
        auto b = a->second.find(cell);
        return b == a->second.end() ? nullptr : &rows[b->second];
    }
    /**
     * @brief Resolve enabled cell-specific forcing with uniform fallback.
     * @param[in] channel Channel in [1,4]; unchecked and used as channel-1 array offset.
     * @param[in] cell Zero-based target cell; out-of-range/negative uses the uniform fallback
     * directly.
     * @return Borrowed enabled row pointer, first the cell override then uniform, or NULL if
     * neither is enabled. Expiry time is not checked here.
     * @pre Cached indices are rebuilt and coherent. Read-only, no allocation/exceptions;
     * serialize with edits.
     */
    const RuntimeForcing* get(int channel, int cell) const noexcept {
        int k = cell >= 0 && cell < static_cast<int>(cells.size()) ? cells[cell][channel - 1] : -1;
        if (k < 0 || !rows[k].enabled)
            k = uniform[channel - 1];
        return k >= 0 && rows[k].enabled ? &rows[k] : nullptr;
    }
    /**
     * @brief Resolve a forced scalar rate from a native value.
     * @param[in] channel Valid runtime channel [1,4].
     * @param[in] cell Cell whose specific/uniform forcing is selected by get().
     * @param[in] native Native rate in the channel's units, normally m/s.
     * @return native when no row, native + row.rate for mode 2, otherwise row.rate.
     * @note No validation, clamping, expiry or receipt update. Read-only and allocation-free;
     * channel units must be consistent.
     */
    double rate(int channel, int cell, double native) const noexcept {
        const auto* row = get(channel, cell);
        return !row ? native : row->mode == 2 ? native + row->rate : row->rate;
    }
    /**
     * @brief Upsert one forcing prescription while retaining its accumulated receipt.
     * @param[in] row Owned draft with a validated channel/cell key and consistent units; moved
     * into the registry.
     * @post Existing keys retain receipts; new rows keep their supplied receipt. Cached
     * cell/uniform indices are not rebuilt automatically.
     * @note Requires exclusive access. Allocation may throw after index changes; use a private
     * staged registry for transactional publication. Input storage is not retained and row
     * pointers may be invalidated.
     */
    void replace(RuntimeForcing row) {
        if (auto* old = find(row.channel, row.cell)) {
            row.receipt = std::move(old->receipt);
            *old = std::move(row);
        } else {
            index[row.channel][row.cell] = rows.size();
            rows.push_back(std::move(row));
        }
    }
    /**
     * @brief Rebuild channel/cell and uniform forcing gather indices.
     * @param[in] count Nonnegative number of mesh cells.
     * @pre Every enabled channel is [1,4]; every nonnegative enabled cell is < count.
     * @post Replaces cells and uniform; later rows overwrite earlier duplicate placement. Does
     * not rebuild the channel/cell identity map or alter receipts.
     * @throws std::out_of_range For a nonnegative cell outside count. Invalid channels are
     * unchecked.
     * @note May allocate and leave partial indices on failure; requires exclusive access.
     */
    void rebuild(int count) {
        cells.assign(count, {{-1, -1, -1, -1}});
        uniform = {{-1, -1, -1, -1}};
        for (std::size_t k = 0; k < rows.size(); ++k)
            if (rows[k].enabled) {
                const auto& row = rows[k];
                if (row.cell < 0)
                    uniform[row.channel - 1] = static_cast<int>(k);
                else
                    cells.at(row.cell)[row.channel - 1] = static_cast<int>(k);
            }
    }
    /**
     * @brief Test whether any retained runtime prescription is enabled.
     * @return True if at least one row.enabled is true, otherwise false. Expiry time is not
     * inspected; expire() must run first for time-based status.
     * @note Read-only, allocation-free; serialize with registry mutations.
     */
    bool active() const noexcept {
        for (const auto& row : rows)
            if (row.enabled)
                return true;
        return false;
    }
    /**
     * @brief Find the next enabled prescription expiry strictly after now.
     * @param[in] now Finite elapsed simulation time, seconds.
     * @return Earliest enabled until > now in elapsed seconds, or positive infinity if none
     * exists.
     * @note Read-only; expired/equal-time rows are skipped without disabling them. No
     * allocation; serialize with edits.
     */
    double nextTime(double now) const noexcept {
        double time = std::numeric_limits<double>::infinity();
        for (const auto& row : rows)
            if (row.enabled && row.until > now)
                time = std::min(time, row.until);
        return time;
    }
    /**
     * @brief Disable all enabled prescriptions whose expiry has been reached.
     * @param[in] now Finite elapsed simulation time, seconds; until <= now expires.
     * @post Keeps row identity and cumulative receipts; cached indices remain and get() checks
     * enabled flags, falling back to uniform forcing.
     * @note Requires exclusive registry access. No allocation or exceptions.
     */
    void expire(double now) noexcept {
        for (auto& row : rows)
            if (row.enabled && row.until <= now)
                row.enabled = false;
    }
};
} // namespace openswmm::twoD
#endif
