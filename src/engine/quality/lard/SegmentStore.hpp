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
 * @file      SegmentStore.hpp
 * @brief     LARD segment slabs — per-link ring buffers of plug-flow segments.
 *
 * @details Subplan X2 (`LARD_AGE_EXPEDITE_SUBPLAN_2026-08-23.md`), layout per
 *          the strategy's binding §16 amendments:
 *
 *          - **D-L2 slab layout**: link `l` owns physical slots
 *            `[l*cap, (l+1)*cap)` with a per-link `head`/`count` ring —
 *            prepend at the front (upstream), consume from the back
 *            (downstream), no compaction, no mid-step reallocation.
 *          - **D-L1 species-major**: `conc[s * slab_total + l*cap + slot]` —
 *            one species is a contiguous stripe across every slab, which is
 *            what vectorizes decay and (later) advection sweeps.
 *          - **D-L5**: overflow merges the two front segments
 *            (mass-conservative) instead of growing under the step.
 *
 *          Front (logical index 0) is the **current upstream** end; the ring
 *          is reversed in place when the link's flow changes sign (§4.4).
 *          All operations conserve mass exactly: a drain reports the mass it
 *          removed, a push books the mass it added, and a merge is a
 *          volume-weighted average.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_QUALITY_LARD_SEGMENT_STORE_HPP
#define OPENSWMM_QUALITY_LARD_SEGMENT_STORE_HPP
#include "../../core/CompletedSourceArchive.hpp"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace openswmm {
namespace lard {

/// EPANET-style cap on segments per link (strategy §4.1 default; the
/// [OPTIONS] MAX_SEGMENTS_PER_LINK override arrives with X3).
constexpr int kMaxSegmentsPerLink = 100;

/// Merge tolerance for a released segment against the current front segment
/// (strategy §4.5): merge when |dC| <= atol + rtol·|C_front| for EVERY
/// species. Plug-flow stretches then collapse to one segment — the dominant
/// constant-factor win in EPANET's LTD.
constexpr double kMergeAtol = 1.0e-6;
constexpr double kMergeRtol = 1.0e-4;

class SegmentStore {
public:
    /**
     * @brief Serialize or restore fixed-sized segment ring arrays and validate restored
     * numerical state.
     * @param[in,out] a Archive already configured for read/write and this owner's dimensions;
     * fixed() may mutate buffers on load. Dimensions nl/ns/cap are not serialized here and must
     * match completed source.
     * @throws std::runtime_error Invalid head/count, negative/nonfinite volume or nonfinite
     * concentration; archive failures also propagate. Negative finite concentration is accepted.
     * Validation follows archive mutation with no rollback, and does not enforce
     * unused-slot/active-volume consistency.
     * @note Exclusive checkpoint barrier; no hydraulic/clock advance or inventory ledger
     * booking.
     */
    void checkpoint(detail::CompletedSourceArchive& a) {
        a.fixed(head_, count_, vol_, conc_);
        for (int l = 0; l < nl_; ++l)
            if (head_[l] < 0 || head_[l] >= cap_ || count_[l] < 0 || count_[l] > cap_)
                throw std::runtime_error("Invalid LARD checkpoint ring.");
        for (double v : vol_)
            if (!std::isfinite(v) || v < 0)
                throw std::runtime_error("Invalid LARD checkpoint water volume.");
        for (double v : conc_)
            if (!std::isfinite(v))
                throw std::runtime_error("Nonfinite LARD checkpoint concentration.");
    }
    /**
     * @brief Replace all segment storage with empty rings at new dimensions.
     * @param[in] n_links Nonnegative link count, representable flattened products.
     * @param[in] n_species Nonnegative concentration row count; zero still allocates one dummy
     * slab.
     * @param[in] cap Ring capacity, at least2 when pushes can fill a ring and require pair
     * merging.
     * @note Resets all heads/counts/volumes/concentrations, discarding inventories without
     * ledger booking. No dimension/overflow validation. Allocation can throw after
     * dimensions/earlier vectors change; exclusive initialization/reset, not an
     * evolved-state-preserving resize.
     */
    void resize(int n_links, int n_species, int cap = kMaxSegmentsPerLink) {
        nl_ = n_links;
        ns_ = n_species;
        cap_ = cap;
        const std::size_t slab = static_cast<std::size_t>(nl_) * static_cast<std::size_t>(cap_);
        head_.assign(static_cast<std::size_t>(nl_), 0);
        count_.assign(static_cast<std::size_t>(nl_), 0);
        vol_.assign(slab, 0.0);
        conc_.assign(slab * static_cast<std::size_t>(ns_ > 0 ? ns_ : 1), 0.0);
    }

    /**
     * @brief Read the number of live segments in one ring.
     * @param[in] l Valid nonnegative link below initialized n_links, unchecked.
     * @return Live count in0..capacity for a valid store; no allocation/state change.
     */
    int count(int l) const { return count_[static_cast<std::size_t>(l)]; }
    /**
     * @brief Read initialized concentration row count.
     * @return Number of species, possibly0 before configuration; no mutation or allocation.
     */
    int species() const { return ns_; }
    /**
     * @brief Read slots reserved per link ring.
     * @return Configured capacity (default100 before resize); no mutation or allocation.
     */
    int capacity() const { return cap_; }

    /**
     * @brief Sum live segment water volumes for one link.
     * @param[in] l Valid initialized link index, unchecked.
     * @return Native water ft3 summed over active ring entries; empty yields0. No repair,
     * allocation or state change.
     */
    double total_volume(int l) const {
        double v = 0.0;
        for (int i = 0; i < count(l); ++i)
            v += vol_[phys(l, i)];
        return v;
    }

    /**
     * @brief Read a live segment's stored water volume.
     * @param[in] l Valid initialized link index.
     * @param[in] i Valid logical active segment index in0..count(l)-1, ordered front to back.
     * @return Native ft3; no bounds/finite check, mutation or allocation.
     */
    double seg_volume(int l, int i) const { return vol_[phys(l, i)]; }
    /**
     * @brief Read one species concentration in a live segment.
     * @param[in] l Valid initialized link index.
     * @param[in] i Valid active logical segment index, front to back.
     * @param[in] s Valid species row in0..species()-1.
     * @return Stored family concentration in caller's row units; no mutation/allocation or
     * bounds validation.
     */
    double seg_conc(int l, int i, int s) const { return conc_[cidx(l, i, s)]; }
    /**
     * @brief X3b: the RWPT exchange rewrites segment concentrations in place (mass-conserving by
     * its own construction — every debit pairs with a credit; see RwptDispersion::exchange).
     *
     * @details Replace one species concentration without volume or ledger changes.
     * @param[in] l Valid initialized link index.
     * @param[in] i Valid active logical segment index.
     * @param[in] s Valid initialized species row.
     * @param[in] c New caller-unit concentration, copied without finite/sign validation.
     * @note Exclusive ring mutation; no allocation, positivity repair or removed-mass
     * bookkeeping.
     */
    void set_seg_conc(int l, int i, int s, double c) { conc_[cidx(l, i, s)] = c; }

    /**
     * @brief Volume-weighted mean concentration over the link's segments. `out` has ns_ entries;
     * an empty link reports 0.
     *
     * @details Compute all species' water-volume-weighted mean over a link's live segments.
     * @param[in] l Valid initialized link index.
     * @param[out] out Writable species() tuple, nonaliasing ring concentrations/volumes; reset
     * then filled.
     * @note Leaves zeros for empty/zero total volume. No bounds/finite repair, allocation or
     * owner mutation; borrowed output stays caller-owned, no ledger or clock effect.
     */
    void mean_conc(int l, double* out) const {
        for (int s = 0; s < ns_; ++s)
            out[s] = 0.0;
        double v = 0.0;
        for (int i = 0; i < count(l); ++i) {
            const double sv = vol_[phys(l, i)];
            v += sv;
            for (int s = 0; s < ns_; ++s)
                out[s] += sv * conc_[cidx(l, i, s)];
        }
        if (v > 0.0)
            for (int s = 0; s < ns_; ++s)
                out[s] /= v;
    }

    /**
     * @brief Remove one whole link's contents (init/re-seed).
     *
     * @details Mark one ring empty without clearing its inactive backing slots.
     * @param[in] l Valid initialized link index.
     * @note Resets head/count0; retained mass/volume is discarded logically without ledger
     * booking. Exclusive lifecycle access; no allocation, numeric repair or clock advance.
     */
    void clear_link(int l) {
        head_[static_cast<std::size_t>(l)] = 0;
        count_[static_cast<std::size_t>(l)] = 0;
    }

    /**
     * @brief Drain up to `vol_target` from the BACK (downstream end).
     *
     * @param mass_out  ns_ entries, ACCUMULATED into (not reset) — the mass carried by the
     * drained water.
     * @return the volume actually drained (< target iff the link emptied).
     *
     * @details Drain available water from the back (largest logical index) and accumulate its
     * species inventories.
     * @param[in] l Valid initialized link index.
     * @param[in] vol_target Finite desired withdrawal ft3; nonpositive/NaN drains nothing.
     * - Additional argument mass_out: Writable species() tuple, adds concentration*ft3 without
     * initial clearing; nonaliasing ring storage and stable throughout call.
     * @return Actual drained ft3 up to available ring volume; empties exhausted segments and
     * updates ring.
     * @note Requires finite nonnegative stored water/concentrations and positive ring capacity.
     * No allocation, ledger or clock update; caller owns removed inventory booking and exclusive
     * ring access.
     */
    double drain_back(int l, double vol_target, double* mass_out) {
        double drained = 0.0;
        while (vol_target - drained > 0.0 && count(l) > 0) {
            const int i = count(l) - 1; // back
            const std::size_t p = phys(l, i);
            const double take = std::min(vol_[p], vol_target - drained);
            for (int s = 0; s < ns_; ++s)
                mass_out[s] += take * conc_[cidx(l, i, s)];
            vol_[p] -= take;
            drained += take;
            if (vol_[p] <= 0.0)
                count_[static_cast<std::size_t>(l)] -= 1;
        }
        return drained;
    }

    /**
     * @brief Same as drain_back but from the FRONT (upstream end) — the volume- reconciliation
     * path for a link that shrank by more than its outflow.
     *
     * @details Drain available water from the front (logical index0) and accumulate its species
     * inventories.
     * @param[in] l Valid initialized link index.
     * @param[in] vol_target Finite desired withdrawal ft3; nonpositive/NaN drains nothing.
     * @param[in,out] mass_out Writable species() tuple, adds concentration*ft3 without initial
     * clearing; nonaliasing ring storage and stable throughout call.
     * @return Actual drained ft3 up to available ring volume; empties exhausted segments and
     * updates ring.
     * @note Requires finite nonnegative stored water/concentrations and positive ring capacity.
     * No allocation, ledger or clock update; caller owns removed inventory booking and exclusive
     * ring access.
     */
    double drain_front(int l, double vol_target, double* mass_out) {
        double drained = 0.0;
        while (vol_target - drained > 0.0 && count(l) > 0) {
            const std::size_t p = phys(l, 0);
            const double take = std::min(vol_[p], vol_target - drained);
            for (int s = 0; s < ns_; ++s)
                mass_out[s] += take * conc_[cidx(l, 0, s)];
            vol_[p] -= take;
            drained += take;
            if (vol_[p] <= 0.0) {
                head_[static_cast<std::size_t>(l)] =
                    (head_[static_cast<std::size_t>(l)] + 1) % cap_;
                count_[static_cast<std::size_t>(l)] -= 1;
            }
        }
        return drained;
    }

    /**
     * @brief Release a new segment at the FRONT (upstream end).
     *
     * @details Merges into the existing front segment when every species is within the §4.5
     * tolerance; on a full ring the two front segments merge first (D-L5) so the push never
     * reallocates.
     *
     * @details Insert incoming water at logical front, merging nearby tuples or freeing a
     * full-ring slot.
     * @param[in] l Valid initialized link index.
     * @param[in] volume Positive finite native water ft3; nonpositive returns (NaN is not
     * rejected).
     * @param[in] c Stable readable species() caller-unit concentration tuple, nonaliasing
     * mutable ring.
     * @note Merges with front when all tuple differences meet tolerance; at capacity merges
     * front pair then inserts. Capacity>=2 required for full-ring operation. Preserves
     * represented finite volume/mass with weighted merges but loses parcel resolution; no
     * ledger/clock effect. No allocation or shape/finite validation; caller enforces finite
     * nonnegative water and exclusive store mutation.
     */
    void push_front(int l, double volume, const double* c) {
        if (volume <= 0.0)
            return;
        if (count(l) > 0 && mergeable(l, 0, c)) {
            merge_into(l, 0, volume, c);
            return;
        }
        if (count(l) >= cap_) {
            // D-L5: merge front pair, mass-conservative, then push.
            merge_front_pair(l);
        }
        head_[static_cast<std::size_t>(l)] = (head_[static_cast<std::size_t>(l)] - 1 + cap_) % cap_;
        count_[static_cast<std::size_t>(l)] += 1;
        const std::size_t p = phys(l, 0);
        vol_[p] = volume;
        for (int s = 0; s < ns_; ++s)
            conc_[cidx(l, 0, s)] = c[s];
    }

    /**
     * @brief §4.4 flow reversal: front becomes back, in place.
     *
     * @details Reverse live segment water and species tuple order in place.
     * @param[in] l Valid initialized link index.
     * @note Reverses logical entries without changing head/count or total inventory; used when
     * hydraulic orientation flips. No allocation or finite validation; caller owns exclusive
     * ring mutation.
     */
    void reverse(int l) {
        const int n = count(l);
        for (int i = 0, j = n - 1; i < j; ++i, --j) {
            std::swap(vol_[phys(l, i)], vol_[phys(l, j)]);
            for (int s = 0; s < ns_; ++s)
                std::swap(conc_[cidx(l, i, s)], conc_[cidx(l, j, s)]);
        }
    }

    /**
     * @brief Add a constant to one species on every LIVE segment.
     *
     * @details X4: the water-age row ages by exactly +dt per routing step in every parcel
     * (zero-order growth, water age plan §1). Only live segments are touched — stale slot
     * contents never age.
     *
     * @details Add a uniform concentration increment to one species in every live segment.
     * @param[in] s Valid initialized species row, unchecked.
     * @param[in] delta Signed caller-unit concentration increment; no positivity/finite
     * validation.
     * @note Adds concentration, not total mass. Does not book mass ledgers or advance time
     * itself; caller supplies aging/source ownership and duration. Allocation-free, exclusive
     * store access.
     */
    void add_species(int s, double delta) {
        for (int l = 0; l < nl_; ++l)
            for (int i = 0; i < count(l); ++i)
                conc_[cidx(l, i, s)] += delta;
    }

    /**
     * @brief Exact-exponential decay on one species over every LIVE segment.
     *
     * @param factor  exp(-k·dt), applied multiplicatively.
     * @return the total mass removed (for the reacted ledger row).
     *
     * @details Multiply one species in every live segment and return its signed inventory
     * reduction.
     * @param[in] s Valid initialized species row, unchecked.
     * - Additional argument factor: Finite concentration multiplier; caller qualifies range,
     * routine does not clamp.
     * @return Sum volume*old_concentration*(1-factor), concentration*ft3; factor>1 produces
     * negative removal.
     * @note No ledger/clock booking; caller routes returned inventory to its family ledger.
     * Allocation-free, exclusive mutation; no numerical/domain validation.
     */
    double decay_species(int s, double factor) {
        double removed = 0.0;
        for (int l = 0; l < nl_; ++l)
            for (int i = 0; i < count(l); ++i) {
                const std::size_t ci = cidx(l, i, s);
                removed += vol_[phys(l, i)] * conc_[ci] * (1.0 - factor);
                conc_[ci] *= factor;
            }
        return removed;
    }

private:
    /**
     * @brief Logical index i (0 = front/upstream) → physical slot in the slab.
     *
     * @details Map a logical ring entry to the link-major water slot.
     * @param[in] l Valid nonnegative link index.
     * @param[in] i Nonnegative logical offset valid for caller's active/reserved operation.
     * @return l*cap+(head[l]+i)%cap; requires cap>0 and fitting arithmetic. No checks or
     * mutation.
     */
    std::size_t phys(int l, int i) const {
        const int slot = (head_[static_cast<std::size_t>(l)] + i) % cap_;
        return static_cast<std::size_t>(l) * static_cast<std::size_t>(cap_) +
               static_cast<std::size_t>(slot);
    }
    /**
     * @brief Species-major concentration index (D-L1).
     *
     * @details Map one logical segment/species to species-major concentration backing storage.
     * @param[in] l Valid link identity.
     * @param[in] i Valid nonnegative logical ring offset.
     * @param[in] s Valid species row.
     * @return s*n_links*capacity+phys(l,i), with fitting products and positive cap; no
     * checks/mutation.
     */
    std::size_t cidx(int l, int i, int s) const {
        return static_cast<std::size_t>(s) * static_cast<std::size_t>(nl_) *
                   static_cast<std::size_t>(cap_) +
               phys(l, i);
    }

    /**
     * @brief Test a candidate tuple against one segment using per-species merge tolerances.
     * @param[in] l Valid initialized link index.
     * @param[in] i Valid active logical segment index.
     * @param[in] c Stable readable species() tuple, no ownership transfer.
     * @return True when every abs(c-c_existing)<=1e-6+1e-4*abs(existing); zero species trivially
     * true.
     * @note No finite validation (NaN comparisons may pass), allocation or mutation; caller
     * supplies finite tuples.
     */
    bool mergeable(int l, int i, const double* c) const {
        for (int s = 0; s < ns_; ++s) {
            const double cf = conc_[cidx(l, i, s)];
            if (std::abs(c[s] - cf) > kMergeAtol + kMergeRtol * std::abs(cf))
                return false;
        }
        return true;
    }

    /**
     * @brief Blend new water and species concentration into one existing segment.
     * @param[in] l Valid initialized link index.
     * @param[in] i Valid active logical segment index.
     * @param[in] volume Finite nonnegative incoming native ft3.
     * @param[in] c Stable readable species() tuple, nonaliasing concentration updates when
     * multiple rows used.
     * @note Positive combined volume writes water-weighted concentration and volume; nonpositive
     * total returns. Conserves represented volume/mass for qualified finite inputs, no
     * ledger/clock booking or allocation. Exclusive ring mutation; dimensions, signs and pointer
     * extents unchecked.
     */
    void merge_into(int l, int i, double volume, const double* c) {
        const std::size_t p = phys(l, i);
        const double v0 = vol_[p];
        const double vt = v0 + volume;
        if (vt <= 0.0)
            return;
        for (int s = 0; s < ns_; ++s) {
            const std::size_t ci = cidx(l, i, s);
            conc_[ci] = (conc_[ci] * v0 + c[s] * volume) / vt;
        }
        vol_[p] = vt;
    }

    /**
     * @brief Free one slot by combining the two logical front segments.
     * @param[in] l Valid initialized link index.
     * @note Fewer than2 live entries returns. Writes summed water and weighted concentration
     * into second entry, advances head and decrements count. Combined nonpositive volume skips
     * concentration blending. No allocation, finite validation or ledger change; finite
     * nonnegative ring invariant and exclusive access required.
     */
    void merge_front_pair(int l) {
        if (count(l) < 2)
            return;
        // Fold segment 1 into segment 0 (volume-weighted), drop slot 1 by
        // moving the head forward over it after copying 0 into 1.
        const std::size_t p0 = phys(l, 0);
        const std::size_t p1 = phys(l, 1);
        const double v0 = vol_[p0], v1 = vol_[p1], vt = v0 + v1;
        if (vt > 0.0)
            for (int s = 0; s < ns_; ++s) {
                const std::size_t c0 = cidx(l, 0, s);
                const std::size_t c1 = cidx(l, 1, s);
                conc_[c1] = (conc_[c0] * v0 + conc_[c1] * v1) / vt;
            }
        vol_[p1] = vt;
        head_[static_cast<std::size_t>(l)] = (head_[static_cast<std::size_t>(l)] + 1) % cap_;
        count_[static_cast<std::size_t>(l)] -= 1;
    }

    int nl_ = 0, ns_ = 0, cap_ = kMaxSegmentsPerLink;
    std::vector<int> head_, count_;
    std::vector<double> vol_;
    std::vector<double> conc_; ///< species-major (D-L1)
};

} // namespace lard
} // namespace openswmm

#endif // OPENSWMM_QUALITY_LARD_SEGMENT_STORE_HPP
