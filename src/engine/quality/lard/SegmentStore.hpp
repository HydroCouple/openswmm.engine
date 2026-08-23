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
 * @file SegmentStore.hpp
 * @brief LARD segment slabs — per-link ring buffers of plug-flow segments.
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
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_QUALITY_LARD_SEGMENT_STORE_HPP
#define OPENSWMM_QUALITY_LARD_SEGMENT_STORE_HPP

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
    void resize(int n_links, int n_species, int cap = kMaxSegmentsPerLink) {
        nl_ = n_links;
        ns_ = n_species;
        cap_ = cap;
        const std::size_t slab =
            static_cast<std::size_t>(nl_) * static_cast<std::size_t>(cap_);
        head_.assign(static_cast<std::size_t>(nl_), 0);
        count_.assign(static_cast<std::size_t>(nl_), 0);
        vol_.assign(slab, 0.0);
        conc_.assign(slab * static_cast<std::size_t>(ns_ > 0 ? ns_ : 1), 0.0);
    }

    int  count(int l) const { return count_[static_cast<std::size_t>(l)]; }
    int  species() const { return ns_; }
    int  capacity() const { return cap_; }

    double total_volume(int l) const {
        double v = 0.0;
        for (int i = 0; i < count(l); ++i) v += vol_[phys(l, i)];
        return v;
    }

    double seg_volume(int l, int i) const { return vol_[phys(l, i)]; }
    double seg_conc(int l, int i, int s) const { return conc_[cidx(l, i, s)]; }

    /// Volume-weighted mean concentration over the link's segments.
    /// `out` has ns_ entries; an empty link reports 0.
    void mean_conc(int l, double* out) const {
        for (int s = 0; s < ns_; ++s) out[s] = 0.0;
        double v = 0.0;
        for (int i = 0; i < count(l); ++i) {
            const double sv = vol_[phys(l, i)];
            v += sv;
            for (int s = 0; s < ns_; ++s) out[s] += sv * conc_[cidx(l, i, s)];
        }
        if (v > 0.0)
            for (int s = 0; s < ns_; ++s) out[s] /= v;
    }

    /// Remove one whole link's contents (init/re-seed).
    void clear_link(int l) {
        head_[static_cast<std::size_t>(l)] = 0;
        count_[static_cast<std::size_t>(l)] = 0;
    }

    /**
     * @brief Drain up to `vol_target` from the BACK (downstream end).
     *
     * @param mass_out  ns_ entries, ACCUMULATED into (not reset) — the mass
     *                  carried by the drained water.
     * @return the volume actually drained (< target iff the link emptied).
     */
    double drain_back(int l, double vol_target, double* mass_out) {
        double drained = 0.0;
        while (vol_target - drained > 0.0 && count(l) > 0) {
            const int i = count(l) - 1;  // back
            const std::size_t p = phys(l, i);
            const double take = std::min(vol_[p], vol_target - drained);
            for (int s = 0; s < ns_; ++s)
                mass_out[s] += take * conc_[cidx(l, i, s)];
            vol_[p] -= take;
            drained += take;
            if (vol_[p] <= 0.0) count_[static_cast<std::size_t>(l)] -= 1;
        }
        return drained;
    }

    /// Same as drain_back but from the FRONT (upstream end) — the volume-
    /// reconciliation path for a link that shrank by more than its outflow.
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
     * @details Merges into the existing front segment when every species is
     *          within the §4.5 tolerance; on a full ring the two front
     *          segments merge first (D-L5) so the push never reallocates.
     */
    void push_front(int l, double volume, const double* c) {
        if (volume <= 0.0) return;
        if (count(l) > 0 && mergeable(l, 0, c)) {
            merge_into(l, 0, volume, c);
            return;
        }
        if (count(l) >= cap_) {
            // D-L5: merge front pair, mass-conservative, then push.
            merge_front_pair(l);
        }
        head_[static_cast<std::size_t>(l)] =
            (head_[static_cast<std::size_t>(l)] - 1 + cap_) % cap_;
        count_[static_cast<std::size_t>(l)] += 1;
        const std::size_t p = phys(l, 0);
        vol_[p] = volume;
        for (int s = 0; s < ns_; ++s) conc_[cidx(l, 0, s)] = c[s];
    }

    /// §4.4 flow reversal: front becomes back, in place.
    void reverse(int l) {
        const int n = count(l);
        for (int i = 0, j = n - 1; i < j; ++i, --j) {
            std::swap(vol_[phys(l, i)], vol_[phys(l, j)]);
            for (int s = 0; s < ns_; ++s)
                std::swap(conc_[cidx(l, i, s)], conc_[cidx(l, j, s)]);
        }
    }

    /**
     * @brief Exact-exponential decay on one species over every LIVE segment.
     *
     * @param factor  exp(-k·dt), applied multiplicatively.
     * @return the total mass removed (for the reacted ledger row).
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
    /// Logical index i (0 = front/upstream) → physical slot in the slab.
    std::size_t phys(int l, int i) const {
        const int slot = (head_[static_cast<std::size_t>(l)] + i) % cap_;
        return static_cast<std::size_t>(l) * static_cast<std::size_t>(cap_) +
               static_cast<std::size_t>(slot);
    }
    /// Species-major concentration index (D-L1).
    std::size_t cidx(int l, int i, int s) const {
        return static_cast<std::size_t>(s) * static_cast<std::size_t>(nl_) *
                   static_cast<std::size_t>(cap_) +
               phys(l, i);
    }

    bool mergeable(int l, int i, const double* c) const {
        for (int s = 0; s < ns_; ++s) {
            const double cf = conc_[cidx(l, i, s)];
            if (std::abs(c[s] - cf) > kMergeAtol + kMergeRtol * std::abs(cf))
                return false;
        }
        return true;
    }

    void merge_into(int l, int i, double volume, const double* c) {
        const std::size_t p = phys(l, i);
        const double v0 = vol_[p];
        const double vt = v0 + volume;
        if (vt <= 0.0) return;
        for (int s = 0; s < ns_; ++s) {
            const std::size_t ci = cidx(l, i, s);
            conc_[ci] = (conc_[ci] * v0 + c[s] * volume) / vt;
        }
        vol_[p] = vt;
    }

    void merge_front_pair(int l) {
        if (count(l) < 2) return;
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
        head_[static_cast<std::size_t>(l)] =
            (head_[static_cast<std::size_t>(l)] + 1) % cap_;
        count_[static_cast<std::size_t>(l)] -= 1;
    }

    int nl_ = 0, ns_ = 0, cap_ = kMaxSegmentsPerLink;
    std::vector<int> head_, count_;
    std::vector<double> vol_;
    std::vector<double> conc_;  ///< species-major (D-L1)
};

}  // namespace lard
}  // namespace openswmm

#endif  // OPENSWMM_QUALITY_LARD_SEGMENT_STORE_HPP
