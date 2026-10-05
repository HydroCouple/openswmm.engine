// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string>
#include <vector>

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
struct SurfaceIntakeActual {
    double volume = 0;
    std::vector<double> mass;
};

// Serial transaction foundation, not a scheduler or runtime activation switch.
// Plan is read-only. Commit revalidates receiver headroom BEFORE callers install
// their trial source state. Invalid settlements book nothing and can be retried.
// Source state, pending provenance/rejection returns and restart integration are
// the caller's responsibilities; the production ownership gate remains closed.
class SurfaceExchange {
public:
    std::string plan(const SubsurfaceSolver&, double start, double end,
                     double completed_through, const std::vector<SurfaceIntakeRequest>&);
    std::string commit(SubsurfaceSolver&, const std::vector<SurfaceIntakeActual>&);
    void cancel() noexcept;
    const std::vector<SurfaceIntakeAward>& awards() const noexcept { return awards_; }
    const std::vector<SurfaceIntakeReceipt>& receipts() const noexcept { return receipts_; }
    double completedEnd() const noexcept { return completed_end_; }
private:
    double start_ = 0, end_ = 0, completed_end_ = 0;
    bool planned_ = false;
    const SubsurfaceSolver* receiver_ = nullptr;
    std::vector<SurfaceIntakeAward> awards_;
    std::vector<SurfaceIntakeReceipt> receipts_;
};
}
