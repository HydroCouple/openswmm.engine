// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NodeCoupling.hpp"
#include <algorithm>
#include <array>

namespace openswmm::detail { class CompletedSourceArchive; }
namespace openswmm::twoD {
// A routing-step reservation, consumed before the next surface advance.
// Overlapping outfalls divide each donor cell; incident 1D links divide each
// outfall's rate. Unused reservations expire rather than being spent twice.
class OutfallExchange {
public:
    void initialize(const MeshData&, const std::vector<CouplingPoint>&,
                    SimulationContext&);
    void prepare(const MeshData&, const SurfaceStateData&, const SolverOptions2D&, SimulationContext&, double dt);
    bool apply(const MeshData&, SurfaceStateData&, const SolverOptions2D&,
               SimulationContext&, double dt, std::vector<double>& accumulated,
               const std::vector<double>* node_conc = nullptr,
               std::vector<double>* cell_accumulated = nullptr);
    /// Book one routing step of each coupled outfall's tailwater (after
    /// updateOutfallBoundaries cached it): time the override is engaged,
    /// time the outlet crown is drowned, and the highest engaged stage.
    void recordTailwater(const SimulationContext&, double dt);
    /// A coupled outfall's tailwater record for the report: seconds with the
    /// 2D tailwater engaged / above the outlet crown, and the highest engaged
    /// stage (1D length; -1e30 if never engaged). False for an uncoupled node.
    bool tailwater(int node, double& engaged_s, double& submerged_s, double& max_stage) const;
private:
    friend class SurfaceRouter2D;
    void checkpoint(detail::CompletedSourceArchive&);
    void validateCheckpoint(const SolverOptions2D&, const std::vector<double>& limits,
                            const std::vector<double>& accumulated, bool changed,
                            const std::array<double,2>& ledger) const;
    void resetWindow() { std::fill(pending_.begin(), pending_.end(), 0.0); }
    struct Point {
        int index, node;
        std::vector<int> cells;
        std::vector<double> fraction, reserved;
        double total = 0, area = 0;
        /// SPREAD: the receiving cells within the radius (own cells first),
        /// and the highest own-cell bed (m) — a farther cell receives while
        /// it is wet or no higher than that.
        std::vector<int> spread;
        double bed_ref = 0;
    };
    std::vector<Point> points_;
    std::vector<int> nodes_;   ///< distinct outfall nodes of points_
    std::vector<int> degree_;
    std::vector<double> node_area_, node_reserved_;
    /// Per node: tailwater record (s, s, 1D length); see recordTailwater().
    std::vector<double> tw_engaged_s_, tw_submerged_s_, tw_max_;
    std::vector<double> pending_; // accepted per-cell receipts since the last surface batch
    double prepared_dt_ = 0;
    bool prepared_ = false; // false after consumption; unused reservations expire
};
}
