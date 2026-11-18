// SPDX-License-Identifier: Apache-2.0

/**
 * @file      SurfaceExchange.hpp
 * @brief     Conservative surface exchange reservations and donor water limits.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include <string>
#include <vector>
#include <functional>

namespace openswmm::twoD {
class SubsurfaceSolver;

enum class SurfaceDonorKind { MESH, NON_LID, LID_BOTTOM };

// SI units. One donor's water ceiling is repeated identically across its cells.
// Candidates must already account for competing source withdrawals (ET,
// outside/native loss, drains). Receiver denial never migrates to another cell.
struct SurfaceIntakeRequest {
    SurfaceDonorKind kind = SurfaceDonorKind::NON_LID;
    std::string source;
    int unit = -1, cell = -1;
    double area = 0, pond = 0, water_ceiling = 0, candidate = 0;
};
struct SurfaceIntakeAward {
    SurfaceIntakeRequest request;
    double demand = 0, maximum = 0;
};
struct SurfaceIntakeReceipt {
    SurfaceIntakeRequest request;
    double start = 0, end = 0, volume = 0;
    // Actual donor-pool masses in the receiver's species order; never inferred
    // from outlet or mesh concentration. Empty only when transport is inactive.
    std::vector<double> mass;
};
enum class SurfaceReturnKind { NUMERICAL_REFUND, PHYSICAL_DUNNE, COLUMN_EXCESS };
// Refunds retain the original donor tuple; physical/older-column emergence
// identifies only the receiver cell, to be split by reviewed surface ownership.
struct SurfaceReturnReceipt {
    SurfaceReturnKind kind = SurfaceReturnKind::NUMERICAL_REFUND;
    SurfaceIntakeReceipt intake;
    double available_at = 0.0;
    double volume = 0.0;
    std::vector<double> mass;
};
struct SurfaceIntakeActual {
    double volume = 0;
    std::vector<double> mass;
};

// Serial transaction foundation, not a scheduler or runtime activation switch.
// Plan is read-only. Commit revalidates receiver headroom BEFORE callers install
// their trial source state. Invalid settlements book nothing and can be retried.
// Reviewed source ownership retains pending provenance in the receiver.
// Source adoption and restart integration remain the caller's responsibilities;
// the production ownership gate remains closed.
class SurfaceExchange {
public:
    // Optional qualified mesh prescription; returns a capacity volume in m3.
    // Both planning and settlement use it; donor/receiver ceilings still apply.
    using Capacity = std::function<double(const SurfaceIntakeRequest&, double)>;
    /**
     * @brief Plan donor-limited, receiver-limited intake over a contiguous completed interval.
     * @par Argument contract The borrowed receiver must remain alive until commit/cancel.
     * start/end/completed_through use elapsed seconds. The request array is borrowed during the
     * call and copied into awards; request area m2, pond head m and water ceilings/candidates
     * m3. The optional final Capacity pointer is a synchronous borrowed callback returning
     * capacity m3 for a request and interval seconds.
     * @return Empty for a published pending plan, otherwise refusal text. No receiver state is
     * changed; allocation/callback exceptions propagate.
     * @note Detailed validation and deterministic scaling are documented with the
     * implementation. Public plan publication requires exclusive access and no previous pending
     * plan.
     */
    std::string plan(const SubsurfaceSolver&, double start, double end, double completed_through,
                     const std::vector<SurfaceIntakeRequest>&, const Capacity* = nullptr);
    /**
     * @brief Settle the current awards against the same aquifer owner.
     * @par Argument contract The first argument is the borrowed mutable receiver used by plan.
     * The actual-transfer vector must match awards order/count and contain paired water
     * m3/species inventory in receiver order. The optional Capacity callback is reevaluated
     * synchronously and must match planning semantics.
     * @return Empty on success, refusal text before booking on invalid/stale data. Allocation
     * exceptions during receiver receipt retention can leave partial booking; caller supplies an
     * enclosing transaction when required.
     * @note Does not install donor trial state. No callback/input-array pointers are retained
     * after successful settlement.
     */
    std::string commit(SubsurfaceSolver&, const std::vector<SurfaceIntakeActual>&,
                       const Capacity* = nullptr);
    void cancel() noexcept;
    /**
     * @brief Read this exchange transaction's stable result state.
     * @return Borrowed object-owned sorted intake-award vector; demand/maximum volumes use m3.
     * Invalidated by plan/cancel/replacement/destruction.
     * @note No allocation or ownership transfer; caller serializes reads with mutation and must
     * not free/modify borrowed vectors.
     */
    const std::vector<SurfaceIntakeAward>& awards() const noexcept { return awards_; }
    /**
     * @brief Read this exchange transaction's stable result state.
     * @return Borrowed object-owned last successful settlement receipts, with copied paired
     * source tuples. Invalidated by the next successful commit/replacement/destruction; cancel
     * preserves them.
     * @note No allocation or ownership transfer; caller serializes reads with mutation and must
     * not free/modify borrowed vectors.
     */
    const std::vector<SurfaceIntakeReceipt>& receipts() const noexcept { return receipts_; }
    /**
     * @brief Read this exchange transaction's stable result state.
     * @return Last successfully committed contiguous endpoint in elapsed seconds; initially zero
     * and preserved by cancel.
     * @note No allocation or ownership transfer; caller serializes reads with mutation and must
     * not free/modify borrowed vectors.
     */
    double completedEnd() const noexcept { return completed_end_; }

private:
    friend class CommonSourceStage;
    double start_ = 0, end_ = 0, completed_end_ = 0;
    bool planned_ = false;
    const SubsurfaceSolver* receiver_ = nullptr;
    std::vector<SurfaceIntakeAward> awards_;
    std::vector<SurfaceIntakeReceipt> receipts_;
};
}
